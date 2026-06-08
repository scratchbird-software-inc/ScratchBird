// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "index_posting.hpp"

#include <algorithm>
#include <cstring>
#include <utility>

namespace scratchbird::core::index {
namespace {
using scratchbird::core::platform::DiagnosticArgument;
using scratchbird::core::platform::HostToLittle32;
using scratchbird::core::platform::HostToLittle64;
using scratchbird::core::platform::LittleToHost32;
using scratchbird::core::platform::LittleToHost64;
using scratchbird::core::platform::MakeDiagnostic;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;
using scratchbird::core::platform::UuidKind;

Status OkStatus() { return {StatusCode::ok, Severity::info, Subsystem::engine}; }
Status ErrorStatus() { return {StatusCode::platform_required_feature_missing, Severity::error, Subsystem::engine}; }

constexpr u32 kPostingListVersion = 2;
constexpr u32 kEntrySerializedBytes = 17 + 17 + 17 + 8 + 8 + 8 + 4;
constexpr u32 kFlagCompressedDuplicates = 1u << 0;
constexpr u32 kFlagProofPresent = 1u << 1;
constexpr u32 kFlagProofNonUniqueExact = 1u << 2;
constexpr u32 kFlagProofBytewiseStable = 1u << 3;
constexpr u32 kFlagProofStableRowUuidLocators = 1u << 4;
constexpr u32 kFlagProofMgaRecheck = 1u << 5;
constexpr u32 kFlagProofParserDonorAuthority = 1u << 6;
constexpr u32 kFlagProofTimestampUuidAuthority = 1u << 7;
constexpr u32 kFlagProofAccepted = 1u << 8;
constexpr u32 kFlagRecheckRequired = 1u << 9;

bool IsSupportedLegacyUuidKind(byte value) {
  const auto kind = static_cast<UuidKind>(value);
  switch (kind) {
    case UuidKind::database:
    case UuidKind::cluster:
    case UuidKind::filespace:
    case UuidKind::schema:
    case UuidKind::object:
    case UuidKind::row:
    case UuidKind::page:
    case UuidKind::transaction:
    case UuidKind::session:
    case UuidKind::principal:
      return true;
    case UuidKind::unknown:
      return false;
  }
  return false;
}

void Store32(std::vector<byte>* out, u32 value) {
  value = HostToLittle32(value);
  const auto* ptr = reinterpret_cast<const byte*>(&value);
  out->insert(out->end(), ptr, ptr + sizeof(value));
}
void Store64(std::vector<byte>* out, u64 value) {
  value = HostToLittle64(value);
  const auto* ptr = reinterpret_cast<const byte*>(&value);
  out->insert(out->end(), ptr, ptr + sizeof(value));
}
void StoreUuid(std::vector<byte>* out, const TypedUuid& uuid) {
  out->push_back(static_cast<byte>(uuid.kind));
  out->insert(out->end(), uuid.value.bytes.begin(), uuid.value.bytes.end());
}
u32 Load32(const std::vector<byte>& in, std::size_t* offset) {
  u32 value = 0;
  std::memcpy(&value, in.data() + *offset, sizeof(value));
  *offset += sizeof(value);
  return LittleToHost32(value);
}
u64 Load64(const std::vector<byte>& in, std::size_t* offset) {
  u64 value = 0;
  std::memcpy(&value, in.data() + *offset, sizeof(value));
  *offset += sizeof(value);
  return LittleToHost64(value);
}
TypedUuid LoadUuid(const std::vector<byte>& in, std::size_t* offset) {
  TypedUuid uuid;
  uuid.kind = static_cast<scratchbird::core::platform::UuidKind>(in[*offset]);
  *offset += 1;
  std::memcpy(uuid.value.bytes.data(), in.data() + *offset, uuid.value.bytes.size());
  *offset += uuid.value.bytes.size();
  return uuid;
}

bool HasFlag(u32 flags, u32 flag) {
  return (flags & flag) != 0;
}

u32 EqualityProofFlags(const IndexPostingList& posting_list) {
  u32 flags = 0;
  if (posting_list.compressed_duplicates) {
    flags |= kFlagCompressedDuplicates;
  }
  if (posting_list.recheck_required) {
    flags |= kFlagRecheckRequired;
  }
  const auto& proof = posting_list.equality_proof;
  if (proof.proof_present) {
    flags |= kFlagProofPresent;
  }
  if (proof.non_unique_exact) {
    flags |= kFlagProofNonUniqueExact;
  }
  if (proof.encoded_key_bytewise_stable) {
    flags |= kFlagProofBytewiseStable;
  }
  if (proof.stable_row_uuid_locators) {
    flags |= kFlagProofStableRowUuidLocators;
  }
  if (proof.preserves_mga_visibility_recheck) {
    flags |= kFlagProofMgaRecheck;
  }
  if (proof.parser_or_donor_finality_authority) {
    flags |= kFlagProofParserDonorAuthority;
  }
  if (proof.timestamp_or_uuid_order_finality_authority) {
    flags |= kFlagProofTimestampUuidAuthority;
  }
  if (IndexPostingEqualityProofAccepted(proof)) {
    flags |= kFlagProofAccepted;
  }
  return flags;
}

IndexPostingEqualityProof ProofFromFlags(u32 flags) {
  IndexPostingEqualityProof proof;
  proof.proof_present = HasFlag(flags, kFlagProofPresent);
  proof.non_unique_exact = HasFlag(flags, kFlagProofNonUniqueExact);
  proof.encoded_key_bytewise_stable = HasFlag(flags, kFlagProofBytewiseStable);
  proof.stable_row_uuid_locators = HasFlag(flags, kFlagProofStableRowUuidLocators);
  proof.preserves_mga_visibility_recheck = HasFlag(flags, kFlagProofMgaRecheck);
  proof.parser_or_donor_finality_authority = HasFlag(flags, kFlagProofParserDonorAuthority);
  proof.timestamp_or_uuid_order_finality_authority = HasFlag(flags, kFlagProofTimestampUuidAuthority);
  return proof;
}

bool UuidLess(const TypedUuid& left, const TypedUuid& right) {
  if (left.kind != right.kind) {
    return static_cast<u32>(left.kind) < static_cast<u32>(right.kind);
  }
  return left.value.bytes < right.value.bytes;
}

bool EntryLess(const IndexPostingEntry& left, const IndexPostingEntry& right) {
  if (UuidLess(left.locator.table_uuid, right.locator.table_uuid)) {
    return true;
  }
  if (UuidLess(right.locator.table_uuid, left.locator.table_uuid)) {
    return false;
  }
  if (UuidLess(left.locator.row_uuid, right.locator.row_uuid)) {
    return true;
  }
  if (UuidLess(right.locator.row_uuid, left.locator.row_uuid)) {
    return false;
  }
  if (UuidLess(left.locator.version_uuid, right.locator.version_uuid)) {
    return true;
  }
  if (UuidLess(right.locator.version_uuid, left.locator.version_uuid)) {
    return false;
  }
  if (left.locator.local_transaction_id != right.locator.local_transaction_id) {
    return left.locator.local_transaction_id < right.locator.local_transaction_id;
  }
  if (left.visible_from_transaction_id != right.visible_from_transaction_id) {
    return left.visible_from_transaction_id < right.visible_from_transaction_id;
  }
  if (left.visible_until_transaction_id != right.visible_until_transaction_id) {
    return left.visible_until_transaction_id < right.visible_until_transaction_id;
  }
  return left.flags < right.flags;
}

bool LocatorValidForPostingCompression(const IndexPostingEntry& entry) {
  return entry.locator.table_uuid.valid() &&
         entry.locator.row_uuid.valid() &&
         entry.locator.row_uuid.kind == UuidKind::row &&
         entry.locator.version_uuid.valid() &&
         entry.locator.version_uuid.kind == UuidKind::row &&
         entry.locator.local_transaction_id != 0 &&
         entry.visible_from_transaction_id != 0;
}

void AddEvidence(IndexPostingListResult* result, std::string name, std::string value) {
  result->evidence.push_back({std::move(name), std::move(value)});
}

void PopulateCountersAndEvidence(IndexPostingListResult* result) {
  const auto& list = result->posting_list;
  const bool proof_accepted = IndexPostingEqualityProofAccepted(list.equality_proof);
  result->counters.compressed_key_count =
      list.compressed_duplicates && !list.entries.empty() ? 1 : 0;
  result->counters.posting_entry_count =
      static_cast<u64>(list.entries.size());
  result->counters.bytes_before =
      static_cast<u64>(list.entries.size()) *
      (static_cast<u64>(list.encoded_key.size()) + kEntrySerializedBytes);
  result->counters.bytes_after =
      static_cast<u64>(result->serialized.size());
  result->counters.bytes_saved =
      result->counters.bytes_before > result->counters.bytes_after
          ? result->counters.bytes_before - result->counters.bytes_after
          : 0;
  result->counters.equality_proof_accepted =
      list.compressed_duplicates && proof_accepted ? 1 : 0;
  result->counters.equality_proof_refused =
      list.compressed_duplicates && !proof_accepted ? 1 : 0;
  result->counters.recheck_required = list.recheck_required ? 1 : 0;
  result->counters.non_unique_exact_mode =
      list.equality_proof.non_unique_exact ? 1 : 0;

  AddEvidence(result, "compressed_key_count",
              std::to_string(result->counters.compressed_key_count));
  AddEvidence(result, "posting_entry_count",
              std::to_string(result->counters.posting_entry_count));
  AddEvidence(result, "bytes_before",
              std::to_string(result->counters.bytes_before));
  AddEvidence(result, "bytes_after",
              std::to_string(result->counters.bytes_after));
  AddEvidence(result, "bytes_saved",
              std::to_string(result->counters.bytes_saved));
  AddEvidence(result, "equality_proof_accepted",
              result->counters.equality_proof_accepted ? "true" : "false");
  AddEvidence(result, "equality_proof_refused",
              result->counters.equality_proof_refused ? "true" : "false");
  AddEvidence(result, "recheck_required",
              result->counters.recheck_required ? "true" : "false");
  AddEvidence(result, "non_unique_exact_mode",
              result->counters.non_unique_exact_mode ? "true" : "false");
}

IndexPostingListResult ParseLegacyPostingList(const std::vector<byte>& serialized) {
  IndexPostingListResult result;
  if (serialized.size() < 4 + 17 + 4 + 4) {
    result.status = ErrorStatus();
    result.diagnostic = MakeIndexPostingDiagnostic(result.status,
                                                   "SB-INDEX-POSTING-BAD-ENVELOPE",
                                                   "index.posting.bad_envelope");
    return result;
  }

  std::size_t offset = 4;
  result.posting_list.index_uuid = LoadUuid(serialized, &offset);
  const u32 key_size = Load32(serialized, &offset);
  if (serialized.size() < offset + key_size + sizeof(u32)) {
    result.status = ErrorStatus();
    result.diagnostic = MakeIndexPostingDiagnostic(result.status,
                                                   "SB-INDEX-POSTING-TRUNCATED",
                                                   "index.posting.truncated");
    return result;
  }
  result.posting_list.encoded_key.assign(
      serialized.begin() + static_cast<std::ptrdiff_t>(offset),
      serialized.begin() + static_cast<std::ptrdiff_t>(offset + key_size));
  result.posting_list.compressed_duplicates = false;
  result.posting_list.recheck_required = true;
  offset += key_size;

  const u32 count = Load32(serialized, &offset);
  for (u32 i = 0; i < count; ++i) {
    if (serialized.size() < offset + kEntrySerializedBytes) {
      result.status = ErrorStatus();
      result.diagnostic = MakeIndexPostingDiagnostic(result.status,
                                                     "SB-INDEX-POSTING-ENTRY-TRUNCATED",
                                                     "index.posting.entry_truncated",
                                                     std::to_string(i));
      return result;
    }
    IndexPostingEntry entry;
    entry.locator.table_uuid = LoadUuid(serialized, &offset);
    entry.locator.row_uuid = LoadUuid(serialized, &offset);
    entry.locator.version_uuid = LoadUuid(serialized, &offset);
    entry.locator.local_transaction_id = Load64(serialized, &offset);
    entry.visible_from_transaction_id = Load64(serialized, &offset);
    entry.visible_until_transaction_id = Load64(serialized, &offset);
    entry.flags = Load32(serialized, &offset);
    result.posting_list.entries.push_back(std::move(entry));
  }
  if (offset != serialized.size()) {
    result.status = ErrorStatus();
    result.diagnostic = MakeIndexPostingDiagnostic(result.status,
                                                   "SB-INDEX-POSTING-TRAILING-BYTES",
                                                   "index.posting.trailing_bytes");
    return result;
  }

  result.status = OkStatus();
  result.serialized = serialized;
  PopulateCountersAndEvidence(&result);
  return result;
}
}  // namespace

bool IndexPostingEqualityProofAccepted(const IndexPostingEqualityProof& proof) {
  return proof.proof_present &&
         proof.non_unique_exact &&
         proof.encoded_key_bytewise_stable &&
         proof.stable_row_uuid_locators &&
         proof.preserves_mga_visibility_recheck &&
         !proof.parser_or_donor_finality_authority &&
         !proof.timestamp_or_uuid_order_finality_authority;
}

IndexPostingListResult BuildIndexPostingList(const IndexPostingList& posting_list) {
  IndexPostingListResult result;
  result.posting_list = posting_list;
  if (!posting_list.index_uuid.valid() || posting_list.encoded_key.empty()) {
    result.status = ErrorStatus();
    result.diagnostic = MakeIndexPostingDiagnostic(result.status,
                                                   "SB-INDEX-POSTING-INVALID-HEADER",
                                                   "index.posting.invalid_header");
    return result;
  }
  if (posting_list.compressed_duplicates &&
      !IndexPostingEqualityProofAccepted(posting_list.equality_proof)) {
    result.status = ErrorStatus();
    result.diagnostic = MakeIndexPostingDiagnostic(result.status,
                                                   "SB-INDEX-POSTING-EQUALITY-PROOF-REFUSED",
                                                   "index.posting.equality_proof_refused");
    PopulateCountersAndEvidence(&result);
    return result;
  }
  std::sort(result.posting_list.entries.begin(),
            result.posting_list.entries.end(),
            EntryLess);
  for (const auto& entry : result.posting_list.entries) {
    if (posting_list.compressed_duplicates) {
      if (!LocatorValidForPostingCompression(entry)) {
        result.status = ErrorStatus();
        result.diagnostic = MakeIndexPostingDiagnostic(result.status,
                                                       "SB-INDEX-POSTING-LOCATOR-INVALID",
                                                       "index.posting.locator_invalid");
        PopulateCountersAndEvidence(&result);
        return result;
      }
    } else if (!entry.locator.table_uuid.valid() || !entry.locator.row_uuid.valid()) {
      result.status = ErrorStatus();
      result.diagnostic = MakeIndexPostingDiagnostic(result.status,
                                                     "SB-INDEX-POSTING-LOCATOR-MISSING",
                                                     "index.posting.locator_missing");
      PopulateCountersAndEvidence(&result);
      return result;
    }
  }
  result.status = OkStatus();
  result.serialized = {'S', 'B', 'P', 'L'};
  Store32(&result.serialized, kPostingListVersion);
  Store32(&result.serialized, EqualityProofFlags(result.posting_list));
  StoreUuid(&result.serialized, result.posting_list.index_uuid);
  Store32(&result.serialized, static_cast<u32>(result.posting_list.encoded_key.size()));
  result.serialized.insert(result.serialized.end(),
                           result.posting_list.encoded_key.begin(),
                           result.posting_list.encoded_key.end());
  Store32(&result.serialized, static_cast<u32>(result.posting_list.entries.size()));
  for (const auto& entry : result.posting_list.entries) {
    StoreUuid(&result.serialized, entry.locator.table_uuid);
    StoreUuid(&result.serialized, entry.locator.row_uuid);
    StoreUuid(&result.serialized, entry.locator.version_uuid);
    Store64(&result.serialized, entry.locator.local_transaction_id);
    Store64(&result.serialized, entry.visible_from_transaction_id);
    Store64(&result.serialized, entry.visible_until_transaction_id);
    Store32(&result.serialized, entry.flags);
  }
  PopulateCountersAndEvidence(&result);
  return result;
}

IndexPostingListResult ParseIndexPostingList(const std::vector<byte>& serialized) {
  IndexPostingListResult result;
  if (serialized.size() < 4 + 17 + 4 + 4 ||
      serialized[0] != 'S' || serialized[1] != 'B' || serialized[2] != 'P' || serialized[3] != 'L') {
    result.status = ErrorStatus();
    result.diagnostic = MakeIndexPostingDiagnostic(result.status,
                                                   "SB-INDEX-POSTING-BAD-ENVELOPE",
                                                   "index.posting.bad_envelope");
    return result;
  }
  std::size_t offset = 4;
  const u32 candidate_version = Load32(serialized, &offset);
  offset = 4;
  if (candidate_version != kPostingListVersion &&
      IsSupportedLegacyUuidKind(serialized[offset])) {
    return ParseLegacyPostingList(serialized);
  }
  if (serialized.size() < 4 + 4 + 4 + 17 + 4 + 4) {
    result.status = ErrorStatus();
    result.diagnostic = MakeIndexPostingDiagnostic(result.status,
                                                   "SB-INDEX-POSTING-BAD-ENVELOPE",
                                                   "index.posting.bad_envelope");
    return result;
  }
  const u32 version = Load32(serialized, &offset);
  if (version != kPostingListVersion) {
    result.status = ErrorStatus();
    result.diagnostic = MakeIndexPostingDiagnostic(result.status,
                                                   "SB-INDEX-POSTING-UNSUPPORTED-VERSION",
                                                   "index.posting.unsupported_version",
                                                   std::to_string(version));
    return result;
  }
  const u32 flags = Load32(serialized, &offset);
  const bool serialized_proof_accepted = HasFlag(flags, kFlagProofAccepted);
  result.posting_list.index_uuid = LoadUuid(serialized, &offset);
  const u32 key_size = Load32(serialized, &offset);
  if (serialized.size() < offset + key_size + sizeof(u32)) {
    result.status = ErrorStatus();
    result.diagnostic = MakeIndexPostingDiagnostic(result.status,
                                                   "SB-INDEX-POSTING-TRUNCATED",
                                                   "index.posting.truncated");
    return result;
  }
  result.posting_list.encoded_key.assign(serialized.begin() + static_cast<std::ptrdiff_t>(offset),
                                         serialized.begin() + static_cast<std::ptrdiff_t>(offset + key_size));
  result.posting_list.compressed_duplicates = HasFlag(flags, kFlagCompressedDuplicates);
  result.posting_list.recheck_required = HasFlag(flags, kFlagRecheckRequired);
  result.posting_list.equality_proof = ProofFromFlags(flags);
  offset += key_size;
  const u32 count = Load32(serialized, &offset);
  for (u32 i = 0; i < count; ++i) {
    if (serialized.size() < offset + 17 + 17 + 17 + 8 + 8 + 8 + 4) {
      result.status = ErrorStatus();
      result.diagnostic = MakeIndexPostingDiagnostic(result.status,
                                                     "SB-INDEX-POSTING-ENTRY-TRUNCATED",
                                                     "index.posting.entry_truncated",
                                                     std::to_string(i));
      return result;
    }
    IndexPostingEntry entry;
    entry.locator.table_uuid = LoadUuid(serialized, &offset);
    entry.locator.row_uuid = LoadUuid(serialized, &offset);
    entry.locator.version_uuid = LoadUuid(serialized, &offset);
    entry.locator.local_transaction_id = Load64(serialized, &offset);
    entry.visible_from_transaction_id = Load64(serialized, &offset);
    entry.visible_until_transaction_id = Load64(serialized, &offset);
    entry.flags = Load32(serialized, &offset);
    if (result.posting_list.compressed_duplicates &&
        !LocatorValidForPostingCompression(entry)) {
      result.status = ErrorStatus();
      result.diagnostic = MakeIndexPostingDiagnostic(result.status,
                                                     "SB-INDEX-POSTING-LOCATOR-INVALID",
                                                     "index.posting.locator_invalid",
                                                     std::to_string(i));
      return result;
    }
    result.posting_list.entries.push_back(std::move(entry));
  }
  if (offset != serialized.size()) {
    result.status = ErrorStatus();
    result.diagnostic = MakeIndexPostingDiagnostic(result.status,
                                                   "SB-INDEX-POSTING-TRAILING-BYTES",
                                                   "index.posting.trailing_bytes");
    return result;
  }
  if (result.posting_list.compressed_duplicates &&
      serialized_proof_accepted !=
          IndexPostingEqualityProofAccepted(result.posting_list.equality_proof)) {
    result.status = ErrorStatus();
    result.diagnostic = MakeIndexPostingDiagnostic(result.status,
                                                   "SB-INDEX-POSTING-EQUALITY-PROOF-MISMATCH",
                                                   "index.posting.equality_proof_mismatch");
    return result;
  }
  if (result.posting_list.compressed_duplicates &&
      !IndexPostingEqualityProofAccepted(result.posting_list.equality_proof)) {
    result.status = ErrorStatus();
    result.diagnostic = MakeIndexPostingDiagnostic(result.status,
                                                   "SB-INDEX-POSTING-EQUALITY-PROOF-REFUSED",
                                                   "index.posting.equality_proof_refused");
    PopulateCountersAndEvidence(&result);
    return result;
  }
  std::sort(result.posting_list.entries.begin(),
            result.posting_list.entries.end(),
            EntryLess);
  result.status = OkStatus();
  result.serialized = serialized;
  PopulateCountersAndEvidence(&result);
  return result;
}

DiagnosticRecord MakeIndexPostingDiagnostic(Status status,
                                            std::string diagnostic_code,
                                            std::string message_key,
                                            std::string detail) {
  std::vector<DiagnosticArgument> arguments;
  if (!detail.empty()) {
    arguments.push_back({"detail", std::move(detail)});
  }
  return MakeDiagnostic(status.code, status.severity, status.subsystem,
                        std::move(diagnostic_code), std::move(message_key),
                        std::move(arguments), {}, "core.index.posting");
}

}  // namespace scratchbird::core::index
