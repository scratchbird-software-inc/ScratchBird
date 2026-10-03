// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "transaction_inventory_page.hpp"
#include "native_inventory_chain_backing.hpp"
#include "native_metadata_memory.hpp"
#include "transaction_inventory_validation.hpp"
#include "transaction_horizon_projection.hpp"
#include "native_decoded_storage_ranges.hpp"

#include "hash_digest.hpp"
#include "hash_digest_parts.hpp"
#include "page_header.hpp"
#include "disk_device.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <mutex>
#include <set>
#include <utility>
#include <vector>
#include <type_traits>

namespace scratchbird::storage::page {
using scratchbird::core::platform::Uuid;
namespace {

using scratchbird::core::platform::DiagnosticArgument;
using scratchbird::core::platform::LoadLittle16;
using scratchbird::core::platform::LoadLittle32;
using scratchbird::core::platform::LoadLittle64;
using scratchbird::core::platform::MakeDiagnostic;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::StoreLittle16;
using scratchbird::core::platform::StoreLittle32;
using scratchbird::core::platform::StoreLittle64;
using scratchbird::core::platform::Subsystem;
using scratchbird::core::platform::UuidKind;
using scratchbird::transaction::mga::ComputeLocalTransactionHorizons;
using scratchbird::transaction::mga::MakeLocalTransactionId;
using scratchbird::transaction::mga::TransactionInventoryEntry;
using scratchbird::transaction::mga::TransactionScope;
using scratchbird::transaction::mga::TransactionState;
using scratchbird::transaction::mga::kInvalidLocalTransactionId;
using scratchbird::storage::disk::kPageHeaderSerializedBytes;
namespace core_hash = scratchbird::core::hash;

inline constexpr std::array<byte, 8> kTxnInvMagic = {'S', 'B', 'T', 'I', 'P', '0', '0', '3'};
inline constexpr std::array<byte, 8> kTxnInvV1WeakMagic = {'S', 'B', 'T', 'I', 'P', '0', '0', '1'};
inline constexpr u32 kOffsetMagic = 0;
inline constexpr u32 kOffsetHeaderBytes = 8;
inline constexpr u32 kOffsetEntryCount = 12;
inline constexpr u32 kOffsetBodyBytes = 16;
inline constexpr u32 kOffsetNextPageNumber = 24;
inline constexpr u32 kOffsetNextLocalId = 32;
inline constexpr u32 kOffsetOit = 40;
inline constexpr u32 kOffsetOat = 48;
inline constexpr u32 kOffsetOst = 56;
inline constexpr u32 kOffsetChecksumDigest = 64;
inline constexpr u32 kOffsetInventoryGeneration = 96;
inline constexpr u32 kOffsetPreviousPageNumber = 104;
inline constexpr u32 kOffsetChainDigest = 112;
inline constexpr u32 kEntryBytes = 72;
inline constexpr u32 kEntryOffsetBeginVisibleThrough = 48;
inline constexpr u32 kOffsetNextCommitSequence = 144;

namespace EntryFlag {
inline constexpr u32 evidence_required = 1u << 0;
inline constexpr u32 evidence_written = 1u << 1;
inline constexpr u32 rollback_only = 1u << 2;
inline constexpr u32 stable_snapshot = 1u << 5;
}

Status TxnPageOkStatus() {
  return {StatusCode::ok, Severity::info, Subsystem::storage_page};
}

Status TxnPageErrorStatus() {
  return {StatusCode::platform_required_feature_missing, Severity::error, Subsystem::storage_page};
}

TransactionInventoryPageBodyResult TxnPageError(std::string diagnostic_code,
                                                std::string message_key,
                                                std::string detail = {}) {
  TransactionInventoryPageBodyResult result;
  result.status = TxnPageErrorStatus();
  result.diagnostic = MakeTransactionInventoryPageDiagnostic(result.status,
                                                             std::move(diagnostic_code),
                                                             std::move(message_key),
                                                             std::move(detail));
  return result;
}

u32 EntryFlags(const TransactionInventoryEntry& entry) {
  u32 flags = 0;
  if (entry.evidence_record_required) { flags |= EntryFlag::evidence_required; }
  if (entry.evidence_record_written) { flags |= EntryFlag::evidence_written; }
  if (entry.rollback_only) { flags |= EntryFlag::rollback_only; }
  if (entry.stable_snapshot) { flags |= EntryFlag::stable_snapshot; }
  const u32 origin = entry.archived_from_state == TransactionState::committed ? 1u :
      entry.archived_from_state == TransactionState::rolled_back ? 2u :
      entry.archived_from_state == TransactionState::failed_terminal ? 3u : 0u;
  flags |= origin << 3;
  return flags;
}

void AppendU16(std::vector<byte>* out, u16 value) {
  const std::size_t offset = out->size();
  out->resize(offset + sizeof(value));
  StoreLittle16(out->data() + offset, value);
}

void AppendU32(std::vector<byte>* out, u32 value) {
  const std::size_t offset = out->size();
  out->resize(offset + sizeof(value));
  StoreLittle32(out->data() + offset, value);
}

void AppendU64(std::vector<byte>* out, u64 value) {
  const std::size_t offset = out->size();
  out->resize(offset + sizeof(value));
  StoreLittle64(out->data() + offset, value);
}

void StoreDigest(std::vector<byte>* out,
                 u32 offset,
                 const TransactionInventoryPageDigest& digest) {
  std::copy(digest.begin(), digest.end(), out->begin() + offset);
}

TransactionInventoryPageDigest LoadDigest(const std::vector<byte>& in,
                                          u32 offset) {
  TransactionInventoryPageDigest digest{};
  std::copy(in.begin() + offset,
            in.begin() + offset + digest.size(),
            digest.begin());
  return digest;
}

TransactionInventoryPageDigest EmptyDigest() {
  return {};
}

TransactionInventoryPageDigest Sha256OrEmpty(const std::vector<byte>& bytes) {
  const auto computed = core_hash::ComputeSha256Digest(bytes);
  return computed.ok() ? computed.digest : EmptyDigest();
}

std::vector<byte> ChecksumDigestInput(std::vector<byte> body) {
  if (body.size() >= kOffsetChecksumDigest + kTransactionInventoryPageDigestBytes) {
    std::fill(body.begin() + kOffsetChecksumDigest,
              body.begin() + kOffsetChecksumDigest +
                  kTransactionInventoryPageDigestBytes,
              static_cast<byte>(0));
  }
  return body;
}

bool DigestEqual(const TransactionInventoryPageDigest& lhs,
                 const TransactionInventoryPageDigest& rhs) {
  byte diff = 0;
  for (std::size_t index = 0; index < lhs.size(); ++index) {
    diff = static_cast<byte>(diff | (lhs[index] ^ rhs[index]));
  }
  return diff == 0;
}

}  // namespace

u64 ComputeTransactionInventoryPageChecksum(const std::vector<byte>& body) {
  return TransactionInventoryPageDigestLow64(
      ComputeTransactionInventoryPageChecksumDigest(body));
}

TransactionInventoryPageDigest ComputeTransactionInventoryPageChecksumDigest(
    const std::vector<byte>& body) {
  return Sha256OrEmpty(ChecksumDigestInput(body));
}

TransactionInventoryPageDigest ComputeTransactionInventoryPageChainDigest(
    const TransactionInventoryPageBody& body) {
  std::vector<byte> canonical;
  canonical.insert(canonical.end(), kTxnInvMagic.begin(), kTxnInvMagic.end());
  AppendU32(&canonical, kTransactionInventoryPageBodyHeaderBytes);
  AppendU64(&canonical, body.page_number);
  AppendU64(&canonical, body.previous_page_number);
  AppendU64(&canonical, body.next_page_number);
  AppendU64(&canonical, body.inventory_generation);
  AppendU64(&canonical, body.inventory.next_local_transaction_id);
  AppendU64(&canonical, body.inventory.next_commit_sequence);
  AppendU64(&canonical, body.horizons.oldest_interesting_transaction.value);
  AppendU64(&canonical, body.horizons.oldest_active_transaction.value);
  AppendU64(&canonical, body.horizons.oldest_snapshot_transaction.value);
  AppendU64(&canonical, body.horizons.next_transaction_id.value);
  AppendU32(&canonical, static_cast<u32>(body.inventory.entries.size()));
  for (const TransactionInventoryEntry& entry : body.inventory.entries) {
    AppendU64(&canonical, entry.identity.local_id.value);
    canonical.insert(canonical.end(),
                     entry.identity.transaction_uuid.value.bytes.begin(),
                     entry.identity.transaction_uuid.value.bytes.end());
    AppendU16(&canonical, static_cast<u16>(entry.identity.scope));
    AppendU16(&canonical, static_cast<u16>(entry.state));
    AppendU32(&canonical, EntryFlags(entry));
    AppendU64(&canonical, entry.begin_unix_epoch_millis);
    AppendU64(&canonical, entry.final_unix_epoch_millis);
    AppendU64(&canonical, entry.begin_visible_through_local_transaction_id);
    AppendU64(&canonical, entry.begin_visible_through_commit_sequence);
    AppendU64(&canonical, entry.commit_sequence);
  }
  return Sha256OrEmpty(canonical);
}

u64 TransactionInventoryPageDigestLow64(
    const TransactionInventoryPageDigest& digest) {
  return LoadLittle64(digest.data());
}

bool TransactionInventoryPageDigestPresent(
    const TransactionInventoryPageDigest& digest) {
  return std::any_of(digest.begin(), digest.end(), [](byte value) {
    return value != 0;
  });
}

u32 MaxTransactionInventoryEntriesPerPage(u32 page_size) {
  if (page_size <= kPageHeaderSerializedBytes + kTransactionInventoryPageBodyHeaderBytes) {
    return 0;
  }
  return (page_size - kPageHeaderSerializedBytes - kTransactionInventoryPageBodyHeaderBytes) / kEntryBytes;
}

TransactionInventoryPageBodyResult BuildTransactionInventoryPageBody(const TransactionInventoryPageBody& body,
                                                                     u32 page_size) {
  if (const auto reason = scratchbird::transaction::mga::ValidateLocalTransactionInventoryStructure(body.inventory); *reason)
    return TxnPageError("CATALOG.INVALID_INPUT", "transaction_inventory_page.inventory_invalid", reason);
  if (page_size <= kPageHeaderSerializedBytes + kTransactionInventoryPageBodyHeaderBytes) {
    return TxnPageError("SB-TXN-INVENTORY-PAGE-BODY-INVALID",
                        "transaction_inventory_page.page_size_too_small",
                        std::to_string(page_size));
  }
  const u32 max_entries = MaxTransactionInventoryEntriesPerPage(page_size);
  if (body.inventory.entries.size() > max_entries) {
    return TxnPageError("SB-TXN-INVENTORY-PAGE-CAPACITY-INVALID",
                        "transaction_inventory_page.entry_count_exceeds_page_capacity",
                        std::to_string(body.inventory.entries.size()));
  }
  const u32 body_bytes = kTransactionInventoryPageBodyHeaderBytes +
                         static_cast<u32>(body.inventory.entries.size() * kEntryBytes);
  if (body_bytes > page_size - kPageHeaderSerializedBytes) {
    return TxnPageError("SB-TXN-INVENTORY-PAGE-BODY-INVALID",
                        "transaction_inventory_page.body_too_large",
                        std::to_string(body_bytes));
  }
  if (body.inventory_generation == 0) {
    return TxnPageError("SB-TXN-INVENTORY-PAGE-GENERATION-INVALID",
                        "transaction_inventory_page.generation_invalid");
  }
  const auto computed_horizons = ComputeLocalTransactionHorizons(body.inventory);
  if (!computed_horizons.ok()) {
    TransactionInventoryPageBodyResult result;
    result.status = computed_horizons.status;
    result.diagnostic = computed_horizons.diagnostic;
    return result;
  }

  TransactionInventoryPageBodyResult result;
  result.status = TxnPageOkStatus();
  result.body = body;
  result.body.horizons = body.horizons.valid ? body.horizons : computed_horizons.horizons;
  result.body.chain_digest = ComputeTransactionInventoryPageChainDigest(result.body);
  if (!TransactionInventoryPageDigestPresent(result.body.chain_digest)) {
    return TxnPageError("SB-TXN-INVENTORY-PAGE-CHAIN-DIGEST-MISSING",
                        "transaction_inventory_page.chain_digest_missing");
  }
  result.serialized.assign(page_size - kPageHeaderSerializedBytes, 0);
  std::copy(kTxnInvMagic.begin(), kTxnInvMagic.end(), result.serialized.begin() + kOffsetMagic);
  StoreLittle32(result.serialized.data() + kOffsetHeaderBytes, kTransactionInventoryPageBodyHeaderBytes);
  StoreLittle32(result.serialized.data() + kOffsetEntryCount, static_cast<u32>(body.inventory.entries.size()));
  StoreLittle64(result.serialized.data() + kOffsetNextPageNumber, body.next_page_number);
  StoreLittle64(result.serialized.data() + kOffsetNextLocalId, body.inventory.next_local_transaction_id);
  StoreLittle64(result.serialized.data() + kOffsetNextCommitSequence, body.inventory.next_commit_sequence);
  StoreLittle64(result.serialized.data() + kOffsetOit, result.body.horizons.oldest_interesting_transaction.value);
  StoreLittle64(result.serialized.data() + kOffsetOat, result.body.horizons.oldest_active_transaction.value);
  StoreLittle64(result.serialized.data() + kOffsetOst, result.body.horizons.oldest_snapshot_transaction.value);
  StoreLittle64(result.serialized.data() + kOffsetInventoryGeneration, result.body.inventory_generation);
  StoreLittle64(result.serialized.data() + kOffsetPreviousPageNumber, result.body.previous_page_number);
  StoreDigest(&result.serialized, kOffsetChainDigest, result.body.chain_digest);

  u32 offset = kTransactionInventoryPageBodyHeaderBytes;
  for (const TransactionInventoryEntry& entry : body.inventory.entries) {
    if (!entry.identity.valid()) {
      return TxnPageError("SB-TXN-INVENTORY-PAGE-BODY-INVALID",
                          "transaction_inventory_page.invalid_entry_identity");
    }
    StoreLittle64(result.serialized.data() + offset, entry.identity.local_id.value);
    std::copy(entry.identity.transaction_uuid.value.bytes.begin(),
              entry.identity.transaction_uuid.value.bytes.end(),
              result.serialized.begin() + offset + 8);
    StoreLittle16(result.serialized.data() + offset + 24, static_cast<u16>(entry.identity.scope));
    StoreLittle16(result.serialized.data() + offset + 26, static_cast<u16>(entry.state));
    StoreLittle32(result.serialized.data() + offset + 28, EntryFlags(entry));
    StoreLittle64(result.serialized.data() + offset + 32, entry.begin_unix_epoch_millis);
    StoreLittle64(result.serialized.data() + offset + 40, entry.final_unix_epoch_millis);
    StoreLittle64(result.serialized.data() + offset + kEntryOffsetBeginVisibleThrough,
                  entry.begin_visible_through_local_transaction_id);
    StoreLittle64(result.serialized.data() + offset + 56, entry.begin_visible_through_commit_sequence);
    StoreLittle64(result.serialized.data() + offset + 64, entry.commit_sequence);
    offset += kEntryBytes;
  }

  StoreLittle32(result.serialized.data() + kOffsetBodyBytes, offset);
  const auto checksum = scratchbird::core::hash::ComputeSha256Digest(ChecksumDigestInput(result.serialized));
  if (!checksum.ok()) {
    TransactionInventoryPageBodyResult failure;
    failure.status = checksum.status;
    failure.diagnostic = checksum.diagnostic;
    return failure;
  }
  StoreDigest(&result.serialized, kOffsetChecksumDigest, checksum.digest);
  return result;
}

TransactionInventoryPageBodyResult ParseTransactionInventoryPageBody(const std::vector<byte>& serialized,
                                                                     u64 page_number) {
  if (serialized.size() < kTransactionInventoryPageBodyHeaderBytes) {
    return TxnPageError("SB-TXN-INVENTORY-PAGE-BODY-INVALID",
                        "transaction_inventory_page.body_short",
                        std::to_string(page_number));
  }
  if (std::equal(kTxnInvV1WeakMagic.begin(),
                 kTxnInvV1WeakMagic.end(),
                 serialized.begin() + kOffsetMagic)) {
    return TxnPageError("SB-TXN-INVENTORY-PAGE-WEAK-DIGEST-REFUSED",
                        "transaction_inventory_page.weak_digest_refused",
                        std::to_string(page_number));
  }
  if (!std::equal(kTxnInvMagic.begin(), kTxnInvMagic.end(), serialized.begin() + kOffsetMagic)) {
    return TxnPageError("SB-TXN-INVENTORY-PAGE-BODY-INVALID",
                        "transaction_inventory_page.magic_invalid",
                        std::to_string(page_number));
  }
  if (LoadLittle32(serialized.data() + kOffsetHeaderBytes) != kTransactionInventoryPageBodyHeaderBytes) {
    return TxnPageError("SB-TXN-INVENTORY-PAGE-BODY-INVALID",
                        "transaction_inventory_page.header_bytes_invalid");
  }
  const auto stored_checksum_digest = LoadDigest(serialized, kOffsetChecksumDigest);
  const auto checksum = scratchbird::core::hash::ComputeSha256Digest(ChecksumDigestInput(serialized));
  if (!checksum.ok()) {
    TransactionInventoryPageBodyResult failure;
    failure.status = checksum.status;
    failure.diagnostic = checksum.diagnostic;
    return failure;
  }
  if (!DigestEqual(stored_checksum_digest, checksum.digest)) {
    return TxnPageError("SB-TXN-INVENTORY-PAGE-CHECKSUM-MISMATCH",
                        "transaction_inventory_page.checksum_mismatch",
                        std::to_string(page_number));
  }
  const u32 entry_count = LoadLittle32(serialized.data() + kOffsetEntryCount);
  const u32 body_bytes = LoadLittle32(serialized.data() + kOffsetBodyBytes);
  const auto stored_chain_digest = LoadDigest(serialized, kOffsetChainDigest);
  if (body_bytes > serialized.size() || body_bytes < kTransactionInventoryPageBodyHeaderBytes ||
      static_cast<u64>(kTransactionInventoryPageBodyHeaderBytes) + static_cast<u64>(entry_count) * kEntryBytes != body_bytes) {
    return TxnPageError("SB-TXN-INVENTORY-PAGE-BODY-INVALID",
                        "transaction_inventory_page.body_bytes_invalid");
  }

  TransactionInventoryPageBodyResult result;
  result.status = TxnPageOkStatus();
  result.body.page_number = page_number;
  result.body.previous_page_number = LoadLittle64(serialized.data() + kOffsetPreviousPageNumber);
  result.body.next_page_number = LoadLittle64(serialized.data() + kOffsetNextPageNumber);
  result.body.inventory_generation = LoadLittle64(serialized.data() + kOffsetInventoryGeneration);
  result.body.chain_digest = stored_chain_digest;
  if (result.body.inventory_generation == 0) {
    return TxnPageError("SB-TXN-INVENTORY-PAGE-GENERATION-INVALID",
                        "transaction_inventory_page.generation_invalid",
                        std::to_string(page_number));
  }
  if (!TransactionInventoryPageDigestPresent(stored_chain_digest)) {
    return TxnPageError("SB-TXN-INVENTORY-PAGE-CHAIN-DIGEST-MISMATCH",
                        "transaction_inventory_page.chain_digest_missing",
                        std::to_string(page_number));
  }
  result.body.inventory.next_local_transaction_id = LoadLittle64(serialized.data() + kOffsetNextLocalId);
  result.body.inventory.next_commit_sequence = LoadLittle64(serialized.data() + kOffsetNextCommitSequence);
  result.body.horizons.oldest_interesting_transaction = MakeLocalTransactionId(LoadLittle64(serialized.data() + kOffsetOit));
  result.body.horizons.oldest_active_transaction = MakeLocalTransactionId(LoadLittle64(serialized.data() + kOffsetOat));
  result.body.horizons.oldest_snapshot_transaction = MakeLocalTransactionId(LoadLittle64(serialized.data() + kOffsetOst));
  result.body.horizons.next_transaction_id = MakeLocalTransactionId(result.body.inventory.next_local_transaction_id);
  result.body.horizons.valid = true;
  result.serialized = serialized;

  u32 offset = kTransactionInventoryPageBodyHeaderBytes;
  for (u32 i = 0; i < entry_count; ++i) {
    TransactionInventoryEntry entry;
    entry.identity.local_id = MakeLocalTransactionId(LoadLittle64(serialized.data() + offset));
    entry.identity.transaction_uuid.kind = UuidKind::transaction;
    std::copy(serialized.begin() + offset + 8,
              serialized.begin() + offset + 24,
              entry.identity.transaction_uuid.value.bytes.begin());
    entry.identity.scope = static_cast<TransactionScope>(LoadLittle16(serialized.data() + offset + 24));
    entry.state = static_cast<TransactionState>(LoadLittle16(serialized.data() + offset + 26));
    const u32 flags = LoadLittle32(serialized.data() + offset + 28);
    if ((flags & ~63u) != 0)
      return TxnPageError("CATALOG.INVALID_INPUT", "transaction_inventory_page.entry_flags_invalid");
    entry.evidence_record_required = (flags & EntryFlag::evidence_required) != 0;
    entry.evidence_record_written = (flags & EntryFlag::evidence_written) != 0;
    entry.rollback_only = (flags & EntryFlag::rollback_only) != 0;
    entry.stable_snapshot = (flags & EntryFlag::stable_snapshot) != 0;
    const auto origin = (flags >> 3) & 3u;
    entry.archived_from_state = origin == 1 ? TransactionState::committed :
        origin == 2 ? TransactionState::rolled_back : origin == 3 ? TransactionState::failed_terminal : TransactionState::none;
    entry.begin_unix_epoch_millis = LoadLittle64(serialized.data() + offset + 32);
    entry.final_unix_epoch_millis = LoadLittle64(serialized.data() + offset + 40);
    entry.begin_visible_through_local_transaction_id =
        LoadLittle64(serialized.data() + offset + kEntryOffsetBeginVisibleThrough);
    entry.begin_visible_through_commit_sequence = LoadLittle64(serialized.data() + offset + 56);
    entry.commit_sequence = LoadLittle64(serialized.data() + offset + 64);
    if (!entry.identity.valid()) {
      return TxnPageError("SB-TXN-INVENTORY-PAGE-BODY-INVALID",
                          "transaction_inventory_page.parsed_identity_invalid",
                          std::to_string(i));
    }
    result.body.inventory.entries.push_back(entry);
    offset += kEntryBytes;
  }
  const auto computed_chain_digest =
      ComputeTransactionInventoryPageChainDigest(result.body);
  if (const auto reason = scratchbird::transaction::mga::ValidateLocalTransactionInventoryStructure(result.body.inventory); *reason)
    return TxnPageError("CATALOG.INVALID_INPUT", "transaction_inventory_page.inventory_invalid", reason);
  if (!DigestEqual(computed_chain_digest, stored_chain_digest)) {
    return TxnPageError("SB-TXN-INVENTORY-PAGE-CHAIN-DIGEST-MISMATCH",
                        "transaction_inventory_page.chain_digest_mismatch",
                        std::to_string(page_number));
  }
  return result;
}

namespace native_inventory {
namespace disk = scratchbird::storage::disk;
namespace mga = scratchbird::transaction::mga;
using Error = NativeInventoryError;
using scratchbird::core::platform::Uuid;
constexpr std::size_t family = 128, entries = 384, digest_at = 320;
constexpr std::array<byte,8> magic{{'S','B','T','I','N','V','0','1'}};
bool V7(const Uuid& value) { return scratchbird::core::uuid::IsEngineIdentityUuid(value); }
bool Zero(const byte* data, std::size_t size) {
  return std::all_of(data,data+size,[](byte v) { return v==0; });
}
void Put(byte* out, const Uuid& value) { std::copy(value.bytes.begin(),value.bytes.end(),out); }
Uuid Get(const byte* in) { Uuid value; std::copy_n(in,16,value.bytes.begin()); return value; }
void PutRef(byte* out, const disk::NativePageReference& ref) {
  Put(out,ref.filespace_uuid); StoreLittle64(out+16,ref.page_number);
  StoreLittle64(out+24,ref.page_generation); Put(out+32,ref.page_size_profile_uuid);
}
disk::NativePageReference GetRef(const byte* in) {
  return {Get(in),LoadLittle64(in+16),LoadLittle64(in+24),Get(in+32)};
}
bool RefValid(const disk::NativePageReference& ref) {
  const auto* profile=disk::FindCanonicalFilespacePageProfile(ref.page_size_profile_uuid);
  return V7(ref.filespace_uuid) && ref.page_number && ref.page_generation && profile
      && ref.page_number<std::numeric_limits<u64>::max()/profile->page_size_bytes
      && disk::CheckFileDeviceExtent(ref.page_number*profile->page_size_bytes,profile->page_size_bytes).ok();
}
bool SameSlot(const disk::NativePageReference& a,const disk::NativePageReference& b) {
  return a.filespace_uuid==b.filespace_uuid && a.page_number==b.page_number;
}
bool ProfilesAgree(const disk::NativePageReference& a,const disk::NativePageReference& b) {
  return a.filespace_uuid!=b.filespace_uuid || a.page_size_profile_uuid==b.page_size_profile_uuid;
}
template<class Page> disk::NativePageReference Self(const Page& p) {
  return {p.header.filespace_uuid,p.header.page_number,p.header.page_generation,p.header.page_size_profile_uuid};
}
NativeTransactionInventoryPageResult Fail(Error error) { return {error,std::nullopt,{}}; }
template<class Page,class Validator> Error Validate(const Page& p,Validator validate_inventory) {
  if (!disk::EncodeNativeCommonPageHeader(p.header).ok() || p.header.page_type!=0x0301)
    return Error::invalid_header;
  if (!V7(p.object_uuid) || !p.inventory_generation
      || p.inventory.entries.size()>(p.header.page_size_bytes-entries)/kEntryBytes)
    return Error::invalid_family;
  const auto self=Self(p);
  if (!RefValid(self)) return Error::invalid_reference;
  for (const auto* neighbor : {&p.previous,&p.next})
    if (*neighbor && (!RefValid(**neighbor) || SameSlot(self,**neighbor) || !ProfilesAgree(self,**neighbor)))
      return Error::invalid_reference;
  if (p.previous && p.next && (SameSlot(*p.previous,*p.next) || !ProfilesAgree(*p.previous,*p.next)))
    return Error::invalid_reference;
  if (*validate_inventory(p.inventory)) return Error::invalid_inventory;
  u64 prior=0;
  for (const auto& entry:p.inventory.entries) {
    if (entry.identity.local_id.value<=prior) return Error::invalid_inventory;
    prior=entry.identity.local_id.value;
  }
  return Error::none;
}
Error Validate(const NativeTransactionInventoryPage& p) {
  return Validate(p,[](const auto& inventory){return mga::ValidateLocalTransactionInventoryStructure(inventory);});
}
// Sort caller-owned indices, never records. Mark duplicates at their original
// positions so the shared rule loop preserves the owning validator's first error.
const char* ValidateBorrowed(const NativeTransactionInventoryView& inventory,
    std::span<std::size_t> indices,std::span<byte> markers) {
  const auto rows=inventory.entries;const auto count=rows.size();
  for(std::size_t i=0;i<count;++i){indices[i]=i;markers[i]=0;}
  const auto mark=[&](auto key,byte bit){
    if(count<2)return;
    std::sort(indices.begin(),indices.begin()+count,[&](auto a,auto b){
      const auto& x=key(rows[a]);const auto& y=key(rows[b]);return x<y||(x==y&&a<b);});
    for(std::size_t i=1;i<count;++i)
      if(key(rows[indices[i]])==key(rows[indices[i-1]]))markers[indices[i]]|=bit;
  };
  mark([](const auto& e){return e.commit_sequence;},1);
  mark([](const auto& e){return e.identity.local_id.value;},2);
  mark([](const auto& e)->const auto&{return e.identity.transaction_uuid.value.bytes;},4);
  return mga::detail::ValidateInventoryStructure(inventory,
      [&](const auto& e){return !(markers[&e-rows.data()]&1);},
      [&](const auto& e){return !(markers[&e-rows.data()]&2);},
      [&](const auto& e){return !(markers[&e-rows.data()]&4);});
}
core_hash::Sha256PartsResult Digest(std::span<const byte> bytes) {
  const std::array<byte,32> zero{};
  const core_hash::HashDigestSegment parts[]={{bytes.data(),digest_at},{zero.data(),zero.size()},
      {bytes.data()+digest_at+32,bytes.size()-digest_at-32}};
  return core_hash::ComputeSha256DigestPartsNative(parts,3);
}
} // namespace native_inventory

NativeTransactionInventoryPageResult EncodeNativeTransactionInventoryPage(
    const NativeTransactionInventoryPage& p) noexcept {
  using namespace native_inventory;
  try {
    const auto valid=Validate(p); if (valid!=Error::none) return Fail(valid);
    const auto horizons=ComputeLocalTransactionHorizons(p.inventory);
    if (!horizons.ok()) return Fail(Error::invalid_inventory);
    const auto header=disk::EncodeNativeCommonPageHeader(p.header);
    std::vector<byte> bytes(p.header.page_size_bytes,0);
    std::copy(header.bytes->begin(),header.bytes->end(),bytes.begin());
    auto* f=bytes.data()+family;
    std::copy(magic.begin(),magic.end(),f); StoreLittle16(f+8,1); StoreLittle16(f+10,256);
    StoreLittle32(f+12,static_cast<u32>(entries+kEntryBytes*p.inventory.entries.size()));
    Put(f+16,p.object_uuid); StoreLittle64(f+32,p.inventory_generation);
    StoreLittle64(f+40,p.inventory.next_local_transaction_id);
    StoreLittle64(f+48,p.inventory.next_commit_sequence);
    StoreLittle32(f+56,static_cast<u32>(p.inventory.entries.size()));
    if (p.previous) PutRef(f+64,*p.previous);
    if (p.next) PutRef(f+112,*p.next);
    StoreLittle64(f+160,horizons.horizons.oldest_interesting_transaction.value);
    StoreLittle64(f+168,horizons.horizons.oldest_active_transaction.value);
    StoreLittle64(f+176,horizons.horizons.oldest_snapshot_transaction.value);
    std::size_t at=entries;
    for (const auto& e:p.inventory.entries) {
      auto* out=bytes.data()+at;
      StoreLittle64(out,e.identity.local_id.value); Put(out+8,e.identity.transaction_uuid.value);
      StoreLittle16(out+24,static_cast<u16>(e.identity.scope));
      StoreLittle16(out+26,static_cast<u16>(e.state)); StoreLittle32(out+28,EntryFlags(e));
      StoreLittle64(out+32,e.begin_unix_epoch_millis); StoreLittle64(out+40,e.final_unix_epoch_millis);
      StoreLittle64(out+48,e.begin_visible_through_local_transaction_id);
      StoreLittle64(out+56,e.begin_visible_through_commit_sequence); StoreLittle64(out+64,e.commit_sequence);
      at+=kEntryBytes;
    }
    const auto digest=Digest(bytes); if (!digest.ok()) return Fail(Error::hash_failure);
    std::copy(digest.digest.begin(),digest.digest.end(),bytes.begin()+digest_at);
    auto page=p; page.inventory.publication_base.reset();
    return {Error::none,std::move(page),std::move(bytes)};
  } catch (const std::bad_alloc&) { return Fail(Error::resource_exhausted); }
    catch (const std::length_error&) { return Fail(Error::resource_exhausted); }
    catch (...) { return Fail(Error::invalid_inventory); }
}

namespace {
template<class Page,class Validator> NativeInventoryError DecodeInventoryValues(
    std::span<const byte> bytes,Page& p,Validator validate_inventory) {
  using namespace native_inventory;
  try {
    const auto header=disk::DecodeNativeCommonPageHeader(bytes.data(),std::min<std::size_t>(bytes.size(),128));
    if (!header.ok() || header.header->page_type!=0x0301 || bytes.size()!=header.header->page_size_bytes)
      return Error::invalid_header;
    const auto digest=Digest(bytes); if (!digest.ok()) return Error::hash_failure;
    if (!std::equal(digest.digest.begin(),digest.digest.end(),bytes.begin()+digest_at)) return Error::invalid_integrity;
    const auto* f=bytes.data()+family; const auto count=LoadLittle32(f+56);
    if (!std::equal(magic.begin(),magic.end(),f) || LoadLittle16(f+8)!=1 || LoadLittle16(f+10)!=256
        || count>(bytes.size()-entries)/kEntryBytes || LoadLittle32(f+12)!=entries+kEntryBytes*count
        || !Zero(f+60,4) || !Zero(f+184,8) || !Zero(f+224,32)
        || !Zero(bytes.data()+entries+kEntryBytes*count,bytes.size()-entries-kEntryBytes*count))
      return Error::invalid_family;
    p.header=*header.header; p.object_uuid=Get(f+16);
    p.inventory_generation=LoadLittle64(f+32); p.inventory.next_local_transaction_id=LoadLittle64(f+40);
    p.inventory.next_commit_sequence=LoadLittle64(f+48);
    if (!Zero(f+64,48)) p.previous=GetRef(f+64);
    if (!Zero(f+112,48)) p.next=GetRef(f+112);
    if constexpr(std::is_same_v<Page,NativeTransactionInventoryPage>)p.inventory.entries.resize(count);
    else {if(count>p.inventory.entries.size())return Error::resource_exhausted;p.inventory.entries=p.inventory.entries.first(count);}
    for (u32 i=0;i<count;++i) {
      const auto* in=bytes.data()+entries+kEntryBytes*i; TransactionInventoryEntry e;
      e.identity.local_id=MakeLocalTransactionId(LoadLittle64(in));
      e.identity.transaction_uuid={UuidKind::transaction,Get(in+8)};
      e.identity.scope=static_cast<TransactionScope>(LoadLittle16(in+24));
      e.state=static_cast<TransactionState>(LoadLittle16(in+26));
      const auto flags=LoadLittle32(in+28); if (flags&~63u) return Error::invalid_inventory;
      e.evidence_record_required=flags&1; e.evidence_record_written=flags&2; e.rollback_only=flags&4; e.stable_snapshot=flags&32;
      const auto origin=(flags>>3)&3;
      e.archived_from_state=origin==1?TransactionState::committed:origin==2?TransactionState::rolled_back:
          origin==3?TransactionState::failed_terminal:TransactionState::none;
      e.begin_unix_epoch_millis=LoadLittle64(in+32); e.final_unix_epoch_millis=LoadLittle64(in+40);
      e.begin_visible_through_local_transaction_id=LoadLittle64(in+48);
      e.begin_visible_through_commit_sequence=LoadLittle64(in+56); e.commit_sequence=LoadLittle64(in+64);
      p.inventory.entries[i]=e;
    }
    const auto valid=Validate(p,validate_inventory); if (valid!=Error::none) return valid;
    LocalTransactionHorizons horizons;
    if(mga::detail::ProjectValidatedLocalHorizons(p.inventory,{},horizons)||
        LoadLittle64(f+160)!=horizons.oldest_interesting_transaction.value||
        LoadLittle64(f+168)!=horizons.oldest_active_transaction.value||
        LoadLittle64(f+176)!=horizons.oldest_snapshot_transaction.value)
      return Error::invalid_inventory;
    return Error::none;
  } catch (const std::bad_alloc&) { return Error::resource_exhausted; }
    catch (const std::length_error&) { return Error::resource_exhausted; }
    catch (...) { return Error::invalid_inventory; }
}
} // namespace
NativeTransactionInventoryPageResult DecodeNativeTransactionInventoryPage(const std::vector<byte>& bytes) noexcept {
  NativeTransactionInventoryPage page;
  const auto error=DecodeInventoryValues(bytes,page,[](const auto& inventory){return scratchbird::transaction::mga::ValidateLocalTransactionInventoryStructure(inventory);});
  if(error!=NativeInventoryError::none)return native_inventory::Fail(error);
  try {return {NativeInventoryError::none,std::move(page),bytes};}
  catch(const std::bad_alloc&){return native_inventory::Fail(NativeInventoryError::resource_exhausted);}
  catch(const std::length_error&){return native_inventory::Fail(NativeInventoryError::resource_exhausted);}
}
NativeInventoryViewValidation ValidateNativeTransactionInventoryView(
    const NativeTransactionInventoryView& inventory,std::span<std::size_t> indices,
    std::span<byte> markers) noexcept {
  using E=NativeInventoryError;
  const std::span<const byte> object{reinterpret_cast<const byte*>(&inventory),sizeof(inventory)};
  if(reinterpret_cast<std::uintptr_t>(inventory.entries.data())%alignof(TransactionInventoryEntry)||
     reinterpret_cast<std::uintptr_t>(indices.data())%alignof(std::size_t)||
     !disk::detail::DisjointNativeDecodeRegions(object,indices,markers)||
     !disk::detail::DisjointNativeDecodeRegions(inventory.entries,indices,markers))
    return {E::invalid_backing,"invalid_backing"};
  if(indices.size()<inventory.entries.size()||markers.size()<inventory.entries.size())
    return {E::resource_exhausted,"insufficient_backing"};
  const auto* why=native_inventory::ValidateBorrowed(inventory,indices,markers);
  return {*why?E::invalid_inventory:E::none,why};
}
NativeInventoryViewValidation ValidateNativeTransactionInventoryEvolutionView(
    const NativeTransactionInventoryView& before,const NativeTransactionInventoryView& after,
    std::span<std::size_t> indices,std::span<std::size_t> uuid_indices,
    std::span<byte> markers) noexcept {
  using E=NativeInventoryError;
  namespace mga=scratchbird::transaction::mga;
  const auto disjoint=[&](auto input){
    return disk::detail::DisjointNativeDecodeRegions(input,indices,uuid_indices,markers);};
  if(reinterpret_cast<std::uintptr_t>(before.entries.data())%alignof(TransactionInventoryEntry)||
     reinterpret_cast<std::uintptr_t>(after.entries.data())%alignof(TransactionInventoryEntry)||
     reinterpret_cast<std::uintptr_t>(indices.data())%alignof(std::size_t)||
     reinterpret_cast<std::uintptr_t>(uuid_indices.data())%alignof(std::size_t)||
     !disjoint(std::span{&before,1})||!disjoint(std::span{&after,1})||
     !disjoint(before.entries)||!disjoint(after.entries))
    return {E::invalid_backing,"invalid_backing"};
  const auto count=before.entries.size(),maximum=std::max(count,after.entries.size());
  if(indices.size()<maximum||markers.size()<maximum||uuid_indices.size()<count)
    return {E::resource_exhausted,"insufficient_backing"};
  if(const auto* why=native_inventory::ValidateBorrowed(before,indices,markers);*why)
    return {E::invalid_inventory,why};
  if(const auto* why=native_inventory::ValidateBorrowed(after,indices,markers);*why)
    return {E::invalid_inventory,why};
  const auto old=before.entries;
  for(std::size_t i=0;i<count;++i){indices[i]=uuid_indices[i]=i;markers[i]=0;}
  auto locals=indices.first(count),uuids=uuid_indices.first(count);
  if(count>1){
    std::sort(locals.begin(),locals.end(),[&](auto a,auto b){
      return old[a].identity.local_id.value<old[b].identity.local_id.value;});
    std::sort(uuids.begin(),uuids.end(),[&](auto a,auto b){
      return old[a].identity.transaction_uuid.value<old[b].identity.transaction_uuid.value;});
  }
  const auto* why=mga::detail::ValidateInventoryEvolutionValues(before,after,
    [&](u64 id)->const TransactionInventoryEntry*{
      const auto at=std::lower_bound(locals.begin(),locals.end(),id,
        [&](auto index,u64 value){return old[index].identity.local_id.value<value;});
      return at==locals.end()||old[*at].identity.local_id.value!=id?nullptr:&old[*at];
    },
    [&](const Uuid& uuid){
      const auto at=std::lower_bound(uuids.begin(),uuids.end(),uuid,
        [&](auto index,const auto& value){return old[index].identity.transaction_uuid.value<value;});
      return at!=uuids.end()&&old[*at].identity.transaction_uuid.value==uuid;
    },
    [&](const auto& entry){markers[&entry-old.data()]=1;},
    [&]{
      for(std::size_t i=0;i<count;++i)if(!markers[i]){
        const auto outcome=mga::InventoryVisibilityState(old[i]);
        if(outcome!=mga::TransactionState::committed&&outcome!=mga::TransactionState::rolled_back)return true;
      }
      return false;
    });
  return {*why?E::invalid_inventory:E::none,why};
}
NativeTransactionInventoryPageViewResult DecodeNativeTransactionInventoryPageInto(
    std::span<const byte> bytes,std::span<TransactionInventoryEntry> entries,
    std::span<std::size_t> indices,std::span<byte> markers) noexcept {
  using E=NativeInventoryError;
  if(reinterpret_cast<std::uintptr_t>(entries.data())%alignof(TransactionInventoryEntry)||
     reinterpret_cast<std::uintptr_t>(indices.data())%alignof(std::size_t)||
     !disk::detail::DisjointNativeDecodeRegions(bytes,entries,indices,markers))
    return {E::invalid_backing,std::nullopt};
  NativeTransactionInventoryPageView page;
  page.inventory.entries=entries.first(std::min({entries.size(),indices.size(),markers.size()}));
  const auto error=DecodeInventoryValues(bytes,page,[&](const auto& inventory){return native_inventory::ValidateBorrowed(inventory,indices,markers);});
  if(error!=E::none)return {error,std::nullopt};
  return {E::none,page};
}

namespace {
NativeInventoryChainView ReadInventoryChainView(
    const Uuid& database_uuid,std::span<const disk::NativeFilespaceDevice> devices,
    const disk::FilespaceRootReference& head,u64 maximum_retained_image_bytes,
    const std::array<byte,32>* root_sha256,
    const std::span<const NativeInventoryHistoricalPageZero>* historical,
    std::pmr::memory_resource& resource,
    std::span<disk::FileDevice::ReadLatencyBatch* const> observations,
    NativeInventoryChainDeviceRead& receipt) noexcept {
  using namespace native_inventory;
  const auto fail=[](Error e) { NativeInventoryChainView r; r.error=e; return r; };
  try {
    disk::detail::NativeMetadataScratch scratch{&resource};
    std::pmr::map<disk::FileDevice*,disk::FileDevice::ReadLatencyBatch*> batches(&resource);
    const auto read=[&](disk::FileDevice& device,u64 offset,void* data,std::size_t count){
      const auto it=batches.find(&device);
      auto io=it==batches.end()?device.ReadAt(offset,data,count):it->second->ReadAt(offset,data,count);
      receipt.io_status=io.status;receipt.io_diagnostic=std::move(io.diagnostic);
      if(io.bytes_transferred>std::numeric_limits<u64>::max()-receipt.physical_bytes_read)throw Error::invalid_reference;
      receipt.physical_bytes_read+=io.bytes_transferred;return io;
    };
    const auto extent=[&](disk::FileDevice& device){auto io=device.Size();receipt.io_status=io.status;
      receipt.io_diagnostic=std::move(io.diagnostic);return io;};

    disk::NativePageReference next{head.filespace_uuid,head.page_number,head.page_generation,head.page_size_profile_uuid};
    if (!V7(database_uuid) || !V7(head.object_uuid) || head.kind!=4 || head.page_type!=0x0301
        || !RefValid(next) || devices.empty() || !maximum_retained_image_bytes) return fail(Error::invalid_reference);
    if (historical && (historical->size()!=devices.size() || !root_sha256 ||
        std::none_of(root_sha256->begin(),root_sha256->end(),[](byte v){return v!=0;}))) return fail(Error::invalid_reference);
    if(!observations.empty()){
      if(observations.size()!=devices.size())return fail(Error::invalid_reference);
      for(std::size_t i=0;i<devices.size();++i){
        if(!devices[i].device||!observations[i]||&observations[i]->device()!=devices[i].device)return fail(Error::invalid_reference);
        batches.emplace(devices[i].device,observations[i]);
      }
    }
    std::pmr::map<Uuid,std::span<const byte>> historical_images(&resource);
    if(historical)for(const auto& image:*historical)
      if(!historical_images.emplace(image.filespace_uuid,image.image).second)return fail(Error::invalid_filespace);
    std::pmr::vector<disk::NativeFilespaceDevice> ordered(devices.begin(),devices.end(),&resource);
    std::sort(ordered.begin(),ordered.end(),[](const auto& a,const auto& b) { return a.filespace_uuid.bytes<b.filespace_uuid.bytes; });
    for (std::size_t i=0;i<ordered.size();++i) {
      const auto& fs=ordered[i];
      if (!V7(fs.filespace_uuid) || !disk::FindCanonicalFilespacePageProfile(fs.page_size_profile_uuid)
          || !fs.device || (i && fs.filespace_uuid==ordered[i-1].filespace_uuid)) return fail(Error::invalid_filespace);
      for (std::size_t j=0;j<i;++j) if (fs.device==ordered[j].device) return fail(Error::invalid_filespace);
    }
    std::pmr::vector<std::unique_lock<std::recursive_mutex>> guards(&resource); guards.reserve(ordered.size());
    for (const auto& fs:ordered) guards.push_back(fs.device->AcquireOperationGuard());
    u64 context_bytes=0;
    std::pmr::vector<u64> physical_sizes(&resource); if(historical)physical_sizes.reserve(ordered.size());
    const auto observe=[&](const disk::NativeFilespaceDevice& fs,const disk::FilespacePageZeroView& zero,bool final,std::size_t index)->Error {
      const auto size=extent(*fs.device); if(!size.ok())return Error::io_failure;
      if(final){if(size.size_bytes!=physical_sizes[index])return Error::binding_mismatch;}
      else {if(size.size_bytes<zero.total_pages*u64{zero.bootstrap.page_size_bytes})return Error::invalid_filespace;physical_sizes.push_back(size.size_bytes);}
      auto actual=scratch.Array<byte>(zero.bootstrap.page_size_bytes);const auto io=read(*fs.device,0,actual.data(),actual.size());
      if(!io.ok()||io.bytes_transferred!=actual.size())return Error::io_failure;
      const auto& retained=historical_images.at(fs.filespace_uuid);
      if(!std::equal(retained.begin(),retained.begin()+4096,actual.begin())||
         !std::equal(retained.begin()+4480,retained.end(),actual.begin()+4480))return Error::binding_mismatch;
      return Error::none;
    };
    std::pmr::vector<disk::FilespacePageZeroView> zeros(&resource); zeros.reserve(ordered.size());
    for (const auto& fs:ordered) {
      const disk::FilespaceBootstrapBinding binding{database_uuid,fs.filespace_uuid,fs.page_size_profile_uuid};
      disk::FilespacePageZeroViewResult zero;
      if(historical){const auto at=historical_images.find(fs.filespace_uuid);if(at==historical_images.end())return fail(Error::invalid_filespace);
        const u64 size=disk::FindCanonicalFilespacePageProfile(fs.page_size_profile_uuid)->page_size_bytes;
        if(at->second.size()!=size)return fail(Error::invalid_filespace);
        if(size>(maximum_retained_image_bytes-context_bytes)/3)return fail(Error::resource_exhausted);context_bytes+=3*size;
        zero=disk::DecodeFilespacePageZeroInto(scratch.Copy<byte>(at->second),scratch.Array<disk::FilespaceRootReference>(32),&binding);
      }else {std::span<const byte> raw;
        zero=disk::detail::ReadNativeMetadataPageZero(binding,scratch,
          [&](u64 offset,void* data,std::size_t count){return read(*fs.device,offset,data,count);},
          [&]{return extent(*fs.device);},raw);}
      if (!zero.ok()) {
        if (zero.error==disk::FilespacePageZeroError::resource_exhausted) return fail(Error::resource_exhausted);
        if (zero.error==disk::FilespacePageZeroError::hash_provider_failure) return fail(Error::hash_failure);
        if (zero.error==disk::FilespacePageZeroError::io_failure) return fail(Error::io_failure);
        return fail(Error::invalid_filespace);
      }
      if(historical){if(zero.record->bootstrap.flags&disk::FilespaceBootstrapFlag::payload_encrypted)return fail(Error::encrypted_requires_crypto_authority);
        const auto error=observe(fs,*zero.record,false,zeros.size());if(error!=Error::none)return fail(error);}
      zeros.push_back(*zero.record);
    }
    NativeInventoryChainView result;std::pmr::vector<NativeInventoryChainPageView> pages(&resource);
    std::pmr::vector<TransactionInventoryEntry> entries(&resource); result.retained_image_bytes=context_bytes;
    std::pmr::set<std::pair<Uuid,u64>> slots(&resource);std::pmr::set<Uuid> page_ids(&resource);
    for (;;) {
      const auto fs=std::lower_bound(ordered.begin(),ordered.end(),next.filespace_uuid,
          [](const auto& a,const auto& id) { return a.filespace_uuid.bytes<id.bytes; });
      if (fs==ordered.end() || fs->filespace_uuid!=next.filespace_uuid || fs->page_size_profile_uuid!=next.page_size_profile_uuid)
        return fail(Error::invalid_filespace);
      const auto& zero=zeros[static_cast<std::size_t>(fs-ordered.begin())];
      if (zero.bootstrap.filespace_role>4 || next.page_number>=zero.total_pages) return fail(Error::invalid_filespace);
      if (zero.bootstrap.flags&disk::FilespaceBootstrapFlag::payload_encrypted) return fail(Error::encrypted_requires_crypto_authority);
      if (!slots.emplace(next.filespace_uuid,next.page_number).second) return fail(Error::chain_mismatch);
      const auto size=zero.bootstrap.page_size_bytes;
      if (size>maximum_retained_image_bytes-result.retained_image_bytes) return fail(Error::resource_exhausted);
      auto bytes=scratch.Array<byte>(size);
      const auto io=read(*fs->device,next.page_number*size,bytes.data(),bytes.size());
      if (!io.ok() || io.bytes_transferred!=bytes.size()) return fail(Error::io_failure);
      const auto header=disk::DecodeNativeCommonPageHeader(bytes.data(),128);
      if (!header.ok()) return fail(Error::invalid_header);
      if (header.header->flags&1u) return fail(Error::encrypted_requires_crypto_authority);
      const auto count=std::min<std::size_t>(LoadLittle32(bytes.data()+184),(bytes.size()-384)/72);
      auto decoded=DecodeNativeTransactionInventoryPageInto(bytes,scratch.Array<TransactionInventoryEntry>(count),
        scratch.Array<std::size_t>(count),scratch.Array<byte>(count));
      if(!decoded.ok())return fail(decoded.error);
      const auto& p=*decoded.page;
      if (p.header.database_uuid!=database_uuid || Self(p)!=next || p.object_uuid!=head.object_uuid)
        return fail(Error::binding_mismatch);
      if (!page_ids.insert(p.header.page_uuid).second) return fail(Error::chain_mismatch);
      if (pages.empty()) {
        if (p.previous) return fail(Error::chain_mismatch);
        if(root_sha256){const auto hash=core_hash::ComputeSha256DigestNative(bytes.data(),bytes.size());if(!hash.ok())return fail(Error::hash_failure);
          if(hash.digest!=*root_sha256)return fail(Error::binding_mismatch);}
        result.inventory.next_local_transaction_id=p.inventory.next_local_transaction_id;
        result.inventory.next_commit_sequence=p.inventory.next_commit_sequence;
      } else {
        const auto& prior=pages.back().page;
        if (!p.previous || *p.previous!=Self(prior) || p.inventory_generation!=prior.inventory_generation
            || p.inventory.next_local_transaction_id!=result.inventory.next_local_transaction_id
            || p.inventory.next_commit_sequence!=result.inventory.next_commit_sequence) return fail(Error::chain_mismatch);
      }
      if (!entries.empty() && !p.inventory.entries.empty()
          && p.inventory.entries.front().identity.local_id.value<=entries.back().identity.local_id.value)
        return fail(Error::chain_mismatch);
      entries.insert(entries.end(),p.inventory.entries.begin(),p.inventory.entries.end());
      result.retained_image_bytes+=size; pages.push_back({p,bytes});
      const auto& tail=pages.back().page;
      if (!tail.next) break;
      next=*tail.next;
    }
    result.inventory.entries=scratch.Copy<TransactionInventoryEntry>(entries);
    const auto valid=ValidateNativeTransactionInventoryView(result.inventory,
      scratch.Array<std::size_t>(entries.size()),scratch.Array<byte>(entries.size()));
    if(!valid.ok())return fail(valid.error);
    LocalTransactionHorizons horizons;
    if(mga::detail::ProjectValidatedLocalHorizons(result.inventory,{},horizons))return fail(Error::invalid_inventory);
    if(historical)for(std::size_t i=0;i<ordered.size();++i){const auto error=observe(ordered[i],zeros[i],true,i);if(error!=Error::none)return fail(error);}
    result.pages=scratch.Copy<NativeInventoryChainPageView>(pages);result.horizons=horizons; result.error=Error::none; return result;
  } catch (Error error) { return fail(error); }
    catch (const std::bad_alloc&) { return fail(Error::resource_exhausted); }
    catch (const std::length_error&) { return fail(Error::resource_exhausted); }
    catch (...) { return fail(Error::io_failure); }
}

NativeTransactionInventoryChainResult ReadNativeInventoryChain(
    const Uuid& database,const std::vector<disk::NativeFilespaceDevice>& files,
    const disk::FilespaceRootReference& head,u64 allowance,
    const std::array<byte,32>* sha=nullptr,const std::map<Uuid,std::vector<byte>>* historical=nullptr) noexcept {
  const auto fail=[](NativeInventoryError e){NativeTransactionInventoryChainResult r;r.error=e;return r;};
  try{
    disk::detail::NativeMetadataHeapMemory heap;std::pmr::monotonic_buffer_resource resource(&heap);
    disk::detail::NativeMetadataScratch scratch{&resource};
    std::span<NativeInventoryHistoricalPageZero> values;
    if(historical){values=scratch.Array<NativeInventoryHistoricalPageZero>(historical->size());
      std::size_t i=0;for(const auto& [id,image]:*historical)values[i++]={id,image};}
    const std::span<const NativeInventoryHistoricalPageZero> contexts=values;
    NativeInventoryChainDeviceRead receipt;
    const auto view=ReadInventoryChainView(database,files,head,allowance,sha,
      historical?&contexts:nullptr,resource,{},receipt);
    if(!view.ok())return fail(view.error);
    NativeTransactionInventoryChainResult out;out.retained_image_bytes=view.retained_image_bytes;
    out.horizons=view.horizons;out.inventory.next_local_transaction_id=view.inventory.next_local_transaction_id;
    out.inventory.next_commit_sequence=view.inventory.next_commit_sequence;
    out.inventory.entries.assign(view.inventory.entries.begin(),view.inventory.entries.end());
    out.pages.reserve(view.pages.size());
    // Full owning compatibility copies verified fields/images after fencing,
    // without fabricating a publication-CAS base or replaying provider checks.
    for(const auto& image:view.pages){
      const auto& page=image.page;NativeTransactionInventoryPage value;
      value.header=page.header;value.object_uuid=page.object_uuid;value.inventory_generation=page.inventory_generation;
      value.previous=page.previous;value.next=page.next;
      value.inventory.next_local_transaction_id=page.inventory.next_local_transaction_id;
      value.inventory.next_commit_sequence=page.inventory.next_commit_sequence;
      value.inventory.entries.assign(page.inventory.entries.begin(),page.inventory.entries.end());
      NativeTransactionInventoryPageResult r;r.error=NativeInventoryError::none;r.page=std::move(value);
      r.bytes.assign(image.image.begin(),image.image.end());out.pages.push_back(std::move(r));
    }
    out.error=NativeInventoryError::none;return out;
  }catch(const std::bad_alloc&){return fail(NativeInventoryError::resource_exhausted);}
   catch(const std::length_error&){return fail(NativeInventoryError::resource_exhausted);}
   catch(...){return fail(NativeInventoryError::io_failure);}
}

} // namespace

NativeInventoryChainDeviceRead detail::ReadNativeInventoryChainBacked(
    const Uuid& database,std::span<const disk::NativeFilespaceDevice> files,
    const disk::FilespaceRootReference& head,u64 allowance,NativeInventoryChainReadContext context,
    const std::array<byte,32>* sha,std::span<const NativeInventoryHistoricalPageZero> historical,
    std::span<disk::FileDevice::ReadLatencyBatch* const> batches,std::pmr::memory_resource& resource) noexcept {
  NativeInventoryChainDeviceRead out;using C=NativeInventoryChainReadContext;
  if((context!=C::current&&context!=C::historical)||
     (context==C::historical?(!sha||historical.size()!=files.size()):(sha||!historical.empty()))||
     files.empty()||batches.size()!=files.size())return out;
  out.chain=ReadInventoryChainView(database,files,head,allowance,sha,
    context==C::historical?&historical:nullptr,resource,batches,out);return out;
}
NativeInventoryChainDeviceRead ReadNativeTransactionInventoryChainInto(
    const Uuid& database,std::span<const disk::NativeFilespaceDevice> files,
    const disk::FilespaceRootReference& head,u64 allowance,NativeInventoryChainReadContext context,
    const std::array<byte,32>* sha,std::span<const NativeInventoryHistoricalPageZero> historical,
    std::span<disk::FileDevice::ReadLatencyBatch* const> batches,std::span<byte> backing) noexcept {
  NativeInventoryChainDeviceRead out;using C=NativeInventoryChainReadContext;
  if((context!=C::current&&context!=C::historical)||
     (context==C::historical?(!sha||historical.size()!=files.size()):(sha||!historical.empty()))||
     files.empty()||batches.size()!=files.size())return out;
  for(std::size_t i=0;i<files.size();++i)if(!files[i].device||!batches[i]||&batches[i]->device()!=files[i].device)return out;
  const auto disjoint=[&](auto input){return disk::detail::DisjointNativeDecodeRegions(backing,input);};
  bool valid=disjoint(std::span{&database,1})&&disjoint(std::span{&head,1})&&disjoint(files)&&disjoint(batches)&&
    disjoint(historical)&&(!sha||disjoint(std::span{sha,1}));
  for(std::size_t i=0;i<files.size();++i)valid=valid&&disjoint(std::span{files[i].device,1})&&disjoint(std::span{batches[i],1});
  for(const auto& input:historical)valid=valid&&disjoint(input.image);
  if(!valid){out.chain.error=NativeInventoryError::invalid_backing;return out;}
  disk::detail::NativeMetadataMemory resource(backing);
  out=detail::ReadNativeInventoryChainBacked(database,files,head,allowance,context,sha,historical,batches,resource);
  if(out.ok())out.chain.backing_bytes_used=resource.used();return out;
}


NativeTransactionInventoryChainResult ReadNativeTransactionInventoryChainFromOpenDevices(
    const Uuid& database,const std::vector<disk::NativeFilespaceDevice>& devices,
    const disk::FilespaceRootReference& root,u64 budget) noexcept {
  return ReadNativeInventoryChain(database,devices,root,budget);
}
NativeTransactionInventoryChainResult ReadNativeTransactionInventoryChainAtHistoricalRootFromOpenDevices(
    const Uuid& database,const std::vector<disk::NativeFilespaceDevice>& devices,
    const disk::FilespaceRootReference& root,const std::array<byte,32>& sha,
    const std::map<Uuid,std::vector<byte>>& contexts,u64 budget) noexcept {
  return ReadNativeInventoryChain(database,devices,root,budget,&sha,&contexts);
}

DiagnosticRecord MakeTransactionInventoryPageDiagnostic(Status status,
                                                       std::string diagnostic_code,
                                                       std::string message_key,
                                                       std::string detail) {
  std::vector<DiagnosticArgument> arguments;
  if (!detail.empty()) {
    arguments.push_back({"detail", detail});
  }
  return MakeDiagnostic(status.code,
                        status.severity,
                        status.subsystem,
                        std::move(diagnostic_code),
                        std::move(message_key),
                        std::move(arguments),
                        {},
                        "storage.page.transaction_inventory");
}

}  // namespace scratchbird::storage::page
