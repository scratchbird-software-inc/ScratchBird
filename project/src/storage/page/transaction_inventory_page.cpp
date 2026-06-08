// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "transaction_inventory_page.hpp"

#include "hash_digest.hpp"
#include "page_header.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <utility>
#include <vector>

namespace scratchbird::storage::page {
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

inline constexpr std::array<byte, 8> kTxnInvMagic = {'S', 'B', 'T', 'I', 'P', '0', '0', '2'};
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

namespace EntryFlag {
inline constexpr u32 evidence_required = 1u << 0;
inline constexpr u32 evidence_written = 1u << 1;
inline constexpr u32 rollback_only = 1u << 2;
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
  const auto horizons = body.horizons.valid ? TransactionInventoryPageBodyResult{} : TransactionInventoryPageBodyResult{};
  (void)horizons;
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
    offset += kEntryBytes;
  }

  StoreLittle32(result.serialized.data() + kOffsetBodyBytes, offset);
  StoreDigest(&result.serialized,
              kOffsetChecksumDigest,
              ComputeTransactionInventoryPageChecksumDigest(result.serialized));
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
  const auto computed_checksum_digest =
      ComputeTransactionInventoryPageChecksumDigest(serialized);
  if (!DigestEqual(stored_checksum_digest, computed_checksum_digest)) {
    return TxnPageError("SB-TXN-INVENTORY-PAGE-CHECKSUM-MISMATCH",
                        "transaction_inventory_page.checksum_mismatch",
                        std::to_string(page_number));
  }
  const u32 entry_count = LoadLittle32(serialized.data() + kOffsetEntryCount);
  const u32 body_bytes = LoadLittle32(serialized.data() + kOffsetBodyBytes);
  const auto stored_chain_digest = LoadDigest(serialized, kOffsetChainDigest);
  if (body_bytes > serialized.size() || body_bytes < kTransactionInventoryPageBodyHeaderBytes ||
      kTransactionInventoryPageBodyHeaderBytes + entry_count * kEntryBytes > body_bytes) {
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
    entry.evidence_record_required = (flags & EntryFlag::evidence_required) != 0;
    entry.evidence_record_written = (flags & EntryFlag::evidence_written) != 0;
    entry.rollback_only = (flags & EntryFlag::rollback_only) != 0;
    entry.begin_unix_epoch_millis = LoadLittle64(serialized.data() + offset + 32);
    entry.final_unix_epoch_millis = LoadLittle64(serialized.data() + offset + 40);
    entry.begin_visible_through_local_transaction_id =
        LoadLittle64(serialized.data() + offset + kEntryOffsetBeginVisibleThrough);
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
  if (!DigestEqual(computed_chain_digest, stored_chain_digest)) {
    return TxnPageError("SB-TXN-INVENTORY-PAGE-CHAIN-DIGEST-MISMATCH",
                        "transaction_inventory_page.chain_digest_mismatch",
                        std::to_string(page_number));
  }
  return result;
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
