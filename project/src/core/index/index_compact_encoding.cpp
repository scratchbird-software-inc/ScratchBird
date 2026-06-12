// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "index_compact_encoding.hpp"

#include "index_key_encoding.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>

namespace scratchbird::core::index {
namespace {

using scratchbird::core::platform::DiagnosticArgument;
using scratchbird::core::platform::LoadLittle16;
using scratchbird::core::platform::LoadLittle32;
using scratchbird::core::platform::LoadLittle64;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::StoreLittle16;
using scratchbird::core::platform::StoreLittle32;
using scratchbird::core::platform::StoreLittle64;
using scratchbird::core::platform::Subsystem;
using scratchbird::core::uuid::CompareUuid128;
using scratchbird::core::uuid::CompareUuidV7ForIndex;
using scratchbird::core::uuid::ExtractUuidV7TimePrefix;

constexpr std::array<byte, 8> kExactCompactMagic = {'S', 'B', 'I', 'C',
                                                    'P', '1', '6', '0'};
constexpr std::array<byte, 8> kExactFallbackMagic = {'S', 'B', 'I', 'U',
                                                     'P', '1', '6', '0'};
constexpr std::array<byte, 8> kPostingCompactMagic = {'S', 'B', 'I', 'C',
                                                      'L', '1', '6', '0'};
constexpr std::array<byte, 8> kPostingFallbackMagic = {'S', 'B', 'I', 'U',
                                                       'L', '1', '6', '0'};
constexpr std::array<byte, 8> kCandidateFallbackMagic = {'S', 'B', 'I', 'U',
                                                         'C', '1', '6', '0'};
constexpr std::array<byte, 8> kUuidFallbackMagic = {'S', 'B', 'I', 'U',
                                                    'V', '1', '6', '0'};
constexpr std::array<byte, 8> kCandidateBitmapMagic = {'S', 'B', 'C', 'B',
                                                       'M', '0', '7', '0'};
constexpr std::array<byte, 8> kUuidV7Magic = {'S', 'B', 'V', '7',
                                              'I', 'D', 'X', '1'};
constexpr u16 kFormatVersion = 1;

constexpr u32 kPostingFlagCompressedDuplicates = 1u << 0u;
constexpr u32 kPostingFlagRecheckRequired = 1u << 1u;
constexpr u32 kPostingFlagProofPresent = 1u << 2u;
constexpr u32 kPostingFlagProofNonUniqueExact = 1u << 3u;
constexpr u32 kPostingFlagProofBytewiseStable = 1u << 4u;
constexpr u32 kPostingFlagProofStableLocators = 1u << 5u;
constexpr u32 kPostingFlagProofMgaRecheck = 1u << 6u;
constexpr u32 kPostingFlagProofParserReferenceAuthority = 1u << 7u;
constexpr u32 kPostingFlagProofTimestampUuidAuthority = 1u << 8u;

Status OkStatus() {
  return {StatusCode::ok, Severity::info, Subsystem::engine};
}

Status ErrorStatus() {
  return {StatusCode::diagnostic_invalid_record, Severity::error,
          Subsystem::engine};
}

u64 Fnv1a64(const byte* data, std::size_t size) {
  u64 hash = 1469598103934665603ull;
  for (std::size_t i = 0; i < size; ++i) {
    hash ^= static_cast<u64>(data[i]);
    hash *= 1099511628211ull;
  }
  return hash;
}

void Append16(std::vector<byte>* out, u16 value) {
  const auto offset = out->size();
  out->resize(offset + sizeof(value));
  StoreLittle16(out->data() + offset, value);
}

void Append32(std::vector<byte>* out, u32 value) {
  const auto offset = out->size();
  out->resize(offset + sizeof(value));
  StoreLittle32(out->data() + offset, value);
}

void Append64(std::vector<byte>* out, u64 value) {
  const auto offset = out->size();
  out->resize(offset + sizeof(value));
  StoreLittle64(out->data() + offset, value);
}

void AppendChecksum(std::vector<byte>* out) {
  Append64(out, Fnv1a64(out->data(), out->size()));
}

bool ReadBytes(const std::vector<byte>& in,
               std::size_t* offset,
               std::size_t count,
               const byte** out) {
  if (*offset > in.size() || count > in.size() - *offset) {
    return false;
  }
  *out = in.data() + *offset;
  *offset += count;
  return true;
}

bool Read16(const std::vector<byte>& in, std::size_t* offset, u16* value) {
  const byte* ptr = nullptr;
  if (!ReadBytes(in, offset, sizeof(*value), &ptr)) {
    return false;
  }
  *value = LoadLittle16(ptr);
  return true;
}

bool Read32(const std::vector<byte>& in, std::size_t* offset, u32* value) {
  const byte* ptr = nullptr;
  if (!ReadBytes(in, offset, sizeof(*value), &ptr)) {
    return false;
  }
  *value = LoadLittle32(ptr);
  return true;
}

bool Read64(const std::vector<byte>& in, std::size_t* offset, u64* value) {
  const byte* ptr = nullptr;
  if (!ReadBytes(in, offset, sizeof(*value), &ptr)) {
    return false;
  }
  *value = LoadLittle64(ptr);
  return true;
}

void AppendUvarint(std::vector<byte>* out, u64 value) {
  while (value >= 0x80u) {
    out->push_back(static_cast<byte>((value & 0x7fu) | 0x80u));
    value >>= 7u;
  }
  out->push_back(static_cast<byte>(value));
}

bool ReadUvarint(const std::vector<byte>& in, std::size_t* offset, u64* value) {
  u64 result = 0;
  u32 shift = 0;
  u32 byte_count = 0;
  while (*offset < in.size() && shift < 64u) {
    const byte current = in[(*offset)++];
    ++byte_count;
    if (byte_count > 10) {
      return false;
    }
    if (shift == 63u && (current & 0x7eu) != 0) {
      return false;
    }
    result |= static_cast<u64>(current & 0x7fu) << shift;
    if ((current & 0x80u) == 0) {
      if (byte_count > 1 && (current & 0x7fu) == 0) {
        return false;
      }
      *value = result;
      return true;
    }
    shift += 7u;
  }
  return false;
}

u64 UvarintSize(u64 value) {
  u64 size = 1;
  while (value >= 0x80u) {
    value >>= 7u;
    ++size;
  }
  return size;
}

void AppendUuid(std::vector<byte>* out, const TypedUuid& uuid) {
  out->push_back(static_cast<byte>(uuid.kind));
  out->insert(out->end(), uuid.value.bytes.begin(), uuid.value.bytes.end());
}

bool ReadUuid(const std::vector<byte>& in, std::size_t* offset, TypedUuid* uuid) {
  const byte* ptr = nullptr;
  if (!ReadBytes(in, offset, 17, &ptr)) {
    return false;
  }
  uuid->kind = static_cast<UuidKind>(ptr[0]);
  std::copy(ptr + 1, ptr + 17, uuid->value.bytes.begin());
  return true;
}

bool HasMagic(const std::vector<byte>& bytes,
              const std::array<byte, 8>& magic) {
  return bytes.size() >= magic.size() &&
         std::equal(magic.begin(), magic.end(), bytes.begin());
}

bool ChecksumValid(const std::vector<byte>& bytes) {
  if (bytes.size() < sizeof(u64)) {
    return false;
  }
  const u64 stored = LoadLittle64(bytes.data() + bytes.size() - sizeof(u64));
  const u64 computed = Fnv1a64(bytes.data(), bytes.size() - sizeof(u64));
  return stored == computed;
}

bool AddOverflows(u64 left, u64 right) {
  return right > std::numeric_limits<u64>::max() - left;
}

int CompareBytes(const std::vector<byte>& left,
                 const std::vector<byte>& right) {
  const auto count = std::min(left.size(), right.size());
  for (std::size_t i = 0; i < count; ++i) {
    if (left[i] < right[i]) {
      return -1;
    }
    if (left[i] > right[i]) {
      return 1;
    }
  }
  return left.size() < right.size() ? -1 : (right.size() < left.size() ? 1 : 0);
}

int CompareTypedUuid(const TypedUuid& left, const TypedUuid& right) {
  if (left.kind != right.kind) {
    return static_cast<int>(left.kind) < static_cast<int>(right.kind) ? -1 : 1;
  }
  return CompareUuid128(left.value, right.value);
}

bool ValidRowUuid(const TypedUuid& uuid) {
  return uuid.valid() && uuid.kind == UuidKind::row;
}

bool ValidEncodedKey(const std::vector<byte>& key) {
  return !key.empty() &&
         IsOrderPreservingIndexKeyEncoding(
             std::string_view(reinterpret_cast<const char*>(key.data()),
                              key.size()));
}

int CompareExactRecords(const ExactIndexPageCompactRecord& left,
                        const ExactIndexPageCompactRecord& right) {
  const int key = CompareBytes(left.encoded_key, right.encoded_key);
  if (key != 0) {
    return key;
  }
  const int row = CompareTypedUuid(left.row_uuid, right.row_uuid);
  if (row != 0) {
    return row;
  }
  const int version = CompareTypedUuid(left.version_uuid, right.version_uuid);
  if (version != 0) {
    return version;
  }
  if (left.row_ordinal != right.row_ordinal) {
    return left.row_ordinal < right.row_ordinal ? -1 : 1;
  }
  return 0;
}

bool ExactRecordsEqual(const std::vector<ExactIndexPageCompactRecord>& left,
                       const std::vector<ExactIndexPageCompactRecord>& right) {
  if (left.size() != right.size()) {
    return false;
  }
  for (std::size_t i = 0; i < left.size(); ++i) {
    if (left[i].encoded_key != right[i].encoded_key ||
        left[i].row_uuid.kind != right[i].row_uuid.kind ||
        left[i].row_uuid.value != right[i].row_uuid.value ||
        left[i].version_uuid.kind != right[i].version_uuid.kind ||
        left[i].version_uuid.value != right[i].version_uuid.value ||
        left[i].row_ordinal != right[i].row_ordinal ||
        left[i].flags != right[i].flags ||
        left[i].payload_metadata != right[i].payload_metadata) {
      return false;
    }
  }
  return true;
}

bool ValidateCompactAuthority(const IndexCompactAuthorityContext& authority,
                              bool require_uuid_order) {
  if (!authority.exact_source_proven ||
      !authority.order_correctness_proven ||
      !authority.encoded_key_order_proven ||
      !authority.mga_visibility_recheck_required ||
      !authority.security_recheck_required ||
      authority.parser_or_reference_authority ||
      authority.provider_authority ||
      authority.wal_or_finality_authority ||
      authority.uuid_order_finality_authority) {
    return false;
  }
  if (require_uuid_order && !authority.uuidv7_order_equivalence_proven) {
    return false;
  }
  return true;
}

std::vector<std::string> BaseEvidence(IndexCompactEncodingKind kind) {
  return {"index_compact_encoding=" +
              std::string(IndexCompactEncodingKindName(kind)),
          "compact_encoding_storage_cpu_only=true",
          "exact_uncompressed_fallback_available=true",
          "exact_fallback_equivalence_proven=true",
          "mga_visibility_recheck_required=true",
          "security_authorization_recheck_required=true",
          "visibility_authority=false",
          "authorization_authority=false",
          "transaction_finality_authority=false",
          "recovery_authority=false",
          "parser_or_reference_authority=false",
          "provider_authority=false",
          "wal_or_finality_authority=false",
          "uuid_order_finality_authority=false"};
}

void AppendEvidence(std::vector<std::string>* out,
                    const std::vector<std::string>& evidence) {
  out->insert(out->end(), evidence.begin(), evidence.end());
}

void AppendPolicyEvidence(std::vector<std::string>* out,
                          const CompressionPolicyDecision& decision) {
  for (const auto& item : decision.evidence) {
    out->push_back("policy." + item);
  }
}

CompressionPolicyDecision CostDecision(CompressionPolicyRequest policy,
                                       CompressionFamily family,
                                       const IndexCompactAuthorityContext& authority,
                                       u64 uncompressed_bytes,
                                       u64 compact_bytes,
                                       bool index_correctness_proven) {
  policy.family = family;
  policy.parser_or_reference_authority = authority.parser_or_reference_authority ||
                                     authority.provider_authority;
  policy.wal_or_finality_authority = authority.wal_or_finality_authority ||
                                     authority.uuid_order_finality_authority;
  policy.exact_uncompressed_fallback_available = true;
  policy.exact_semantic_equivalence_proven = true;
  policy.exact_binary_equivalence_proven = true;
  policy.runtime_index_compression_requested = true;
  policy.index_runtime_correctness_proven = index_correctness_proven;
  policy.uncompressed_bytes = uncompressed_bytes;
  policy.estimated_compressed_bytes = compact_bytes;
  return EvaluateCompressionPolicy(policy);
}

std::size_t CommonPrefixBytes(const std::vector<byte>& left,
                              const std::vector<byte>& right) {
  const auto limit = std::min(left.size(), right.size());
  std::size_t prefix = 0;
  while (prefix < limit && left[prefix] == right[prefix]) {
    ++prefix;
  }
  return prefix;
}

u64 EstimateExactPageUncompressedBytes(
    const std::vector<ExactIndexPageCompactRecord>& records) {
  u64 bytes = 8 + 2 + 2 + 4 + 8;
  for (const auto& record : records) {
    bytes += UvarintSize(record.encoded_key.size()) + record.encoded_key.size();
    bytes += 17 + 17;
    bytes += UvarintSize(record.row_ordinal);
    bytes += UvarintSize(record.flags);
    bytes += UvarintSize(record.payload_metadata.size()) +
             record.payload_metadata.size();
  }
  return bytes;
}

ExactIndexPageCompactResult RefuseExact(std::string code,
                                        std::string key,
                                        std::string reason,
                                        IndexCompactRepairState state =
                                            IndexCompactRepairState::refused) {
  ExactIndexPageCompactResult result;
  result.status = ErrorStatus();
  result.fail_closed = true;
  result.repair_state = state;
  result.diagnostic = MakeIndexCompactEncodingDiagnostic(
      result.status, std::move(code), std::move(key), reason);
  result.refusal_reasons.push_back(reason);
  result.evidence = BaseEvidence(result.encoding);
  result.evidence.push_back("fail_closed=true");
  result.evidence.push_back("fallback_refusal_reason=" + reason);
  return result;
}

CompactPostingListResult RefusePosting(std::string code,
                                       std::string key,
                                       std::string reason) {
  CompactPostingListResult result;
  result.status = ErrorStatus();
  result.fail_closed = true;
  result.diagnostic = MakeIndexCompactEncodingDiagnostic(
      result.status, std::move(code), std::move(key), reason);
  result.refusal_reasons.push_back(reason);
  result.evidence = BaseEvidence(result.encoding);
  result.evidence.push_back("fail_closed=true");
  result.evidence.push_back("fallback_refusal_reason=" + reason);
  return result;
}

CompactCandidateSetResult RefuseCandidate(std::string code,
                                          std::string key,
                                          std::string reason) {
  CompactCandidateSetResult result;
  result.status = ErrorStatus();
  result.fail_closed = true;
  result.diagnostic = MakeIndexCompactEncodingDiagnostic(
      result.status, std::move(code), std::move(key), reason);
  result.refusal_reasons.push_back(reason);
  result.evidence = BaseEvidence(result.encoding);
  result.evidence.push_back("fail_closed=true");
  result.evidence.push_back("fallback_refusal_reason=" + reason);
  return result;
}

UuidV7CompactKeyBlockResult RefuseUuid(std::string code,
                                       std::string key,
                                       std::string reason) {
  UuidV7CompactKeyBlockResult result;
  result.status = ErrorStatus();
  result.fail_closed = true;
  result.diagnostic = MakeIndexCompactEncodingDiagnostic(
      result.status, std::move(code), std::move(key), reason);
  result.refusal_reasons.push_back(reason);
  result.evidence = BaseEvidence(result.encoding);
  result.evidence.push_back("fail_closed=true");
  result.evidence.push_back("fallback_refusal_reason=" + reason);
  return result;
}

std::optional<std::string> ValidateExactRecords(
    const std::vector<ExactIndexPageCompactRecord>& records) {
  if (records.empty()) {
    return std::string("exact_page_empty");
  }
  for (std::size_t i = 0; i < records.size(); ++i) {
    const auto& record = records[i];
    if (!ValidEncodedKey(record.encoded_key)) {
      return "exact_page_encoded_key_invalid";
    }
    if (!ValidRowUuid(record.row_uuid) || !ValidRowUuid(record.version_uuid)) {
      return "exact_page_row_version_uuid_invalid";
    }
    if (record.row_ordinal == 0) {
      return "exact_page_row_ordinal_invalid";
    }
    if (i != 0 && CompareExactRecords(records[i - 1], record) >= 0) {
      return "exact_page_order_not_proven";
    }
  }
  return std::nullopt;
}

std::vector<byte> SerializeExactPage(
    const std::vector<ExactIndexPageCompactRecord>& records,
    IndexCompactEncodingKind encoding) {
  const auto& magic = encoding == IndexCompactEncodingKind::exact_page_prefix_delta
                          ? kExactCompactMagic
                          : kExactFallbackMagic;
  std::vector<byte> out(magic.begin(), magic.end());
  Append16(&out, kFormatVersion);
  Append16(&out, static_cast<u16>(encoding));
  Append32(&out, static_cast<u32>(records.size()));
  std::vector<byte> previous_key;
  for (const auto& record : records) {
    if (encoding == IndexCompactEncodingKind::exact_page_prefix_delta) {
      const auto prefix = CommonPrefixBytes(previous_key, record.encoded_key);
      AppendUvarint(&out, static_cast<u64>(prefix));
      AppendUvarint(&out,
                    static_cast<u64>(record.encoded_key.size() - prefix));
      out.insert(out.end(),
                 record.encoded_key.begin() +
                     static_cast<std::ptrdiff_t>(prefix),
                 record.encoded_key.end());
      previous_key = record.encoded_key;
    } else {
      AppendUvarint(&out, static_cast<u64>(record.encoded_key.size()));
      out.insert(out.end(), record.encoded_key.begin(), record.encoded_key.end());
    }
    AppendUuid(&out, record.row_uuid);
    AppendUuid(&out, record.version_uuid);
    AppendUvarint(&out, record.row_ordinal);
    AppendUvarint(&out, record.flags);
    AppendUvarint(&out, static_cast<u64>(record.payload_metadata.size()));
    out.insert(out.end(), record.payload_metadata.begin(),
               record.payload_metadata.end());
  }
  AppendChecksum(&out);
  return out;
}

ExactIndexPageCompactResult DecodeExactPageInternal(
    const std::vector<byte>& serialized,
    const IndexCompactAuthorityContext& authority) {
  if (!ValidateCompactAuthority(authority, false)) {
    return RefuseExact("INDEX_COMPACT.UNSAFE_AUTHORITY",
                       "index.compact.unsafe_authority",
                       "compact_authority_context_invalid");
  }
  if (serialized.size() < 8 + 2 + 2 + 4 + 8) {
    return RefuseExact("INDEX_COMPACT.EXACT_PAGE_TRUNCATED",
                       "index.compact.exact_page_truncated",
                       "exact_page_truncated");
  }
  if (!ChecksumValid(serialized)) {
    return RefuseExact("INDEX_COMPACT.EXACT_PAGE_CHECKSUM_MISMATCH",
                       "index.compact.exact_page_checksum_mismatch",
                       "exact_page_checksum_mismatch");
  }
  const bool compact = HasMagic(serialized, kExactCompactMagic);
  const bool fallback = HasMagic(serialized, kExactFallbackMagic);
  if (!compact && !fallback) {
    return RefuseExact("INDEX_COMPACT.EXACT_PAGE_BAD_MAGIC",
                       "index.compact.exact_page_bad_magic",
                       "exact_page_bad_magic");
  }

  std::size_t offset = 8;
  u16 version = 0;
  u16 raw_kind = 0;
  u32 count = 0;
  if (!Read16(serialized, &offset, &version) ||
      !Read16(serialized, &offset, &raw_kind) ||
      !Read32(serialized, &offset, &count)) {
    return RefuseExact("INDEX_COMPACT.EXACT_PAGE_TRUNCATED",
                       "index.compact.exact_page_truncated",
                       "exact_page_header_truncated");
  }
  if (version != kFormatVersion) {
    return RefuseExact("INDEX_COMPACT.EXACT_PAGE_BAD_VERSION",
                       "index.compact.exact_page_bad_version",
                       "exact_page_bad_version");
  }
  const auto kind = static_cast<IndexCompactEncodingKind>(raw_kind);
  if ((compact && kind != IndexCompactEncodingKind::exact_page_prefix_delta) ||
      (fallback && kind != IndexCompactEncodingKind::exact_page_uncompressed)) {
    return RefuseExact("INDEX_COMPACT.EXACT_PAGE_KIND_MISMATCH",
                       "index.compact.exact_page_kind_mismatch",
                       "exact_page_kind_mismatch");
  }

  std::vector<ExactIndexPageCompactRecord> records;
  std::vector<byte> previous_key;
  records.reserve(count);
  for (u32 i = 0; i < count; ++i) {
    ExactIndexPageCompactRecord record;
    if (compact) {
      u64 prefix = 0;
      u64 suffix = 0;
      if (!ReadUvarint(serialized, &offset, &prefix) ||
          !ReadUvarint(serialized, &offset, &suffix) ||
          prefix > previous_key.size() ||
          suffix > std::numeric_limits<u32>::max() ||
          AddOverflows(prefix, suffix)) {
        return RefuseExact("INDEX_COMPACT.EXACT_PAGE_KEY_CORRUPT",
                           "index.compact.exact_page_key_corrupt",
                           "exact_page_compact_key_corrupt");
      }
      const byte* suffix_bytes = nullptr;
      if (!ReadBytes(serialized, &offset, static_cast<std::size_t>(suffix),
                     &suffix_bytes)) {
        return RefuseExact("INDEX_COMPACT.EXACT_PAGE_TRUNCATED",
                           "index.compact.exact_page_truncated",
                           "exact_page_suffix_truncated");
      }
      record.encoded_key.assign(previous_key.begin(),
                                previous_key.begin() +
                                    static_cast<std::ptrdiff_t>(prefix));
      record.encoded_key.insert(record.encoded_key.end(), suffix_bytes,
                                suffix_bytes + suffix);
      previous_key = record.encoded_key;
    } else {
      u64 key_size = 0;
      if (!ReadUvarint(serialized, &offset, &key_size) ||
          key_size > std::numeric_limits<u32>::max()) {
        return RefuseExact("INDEX_COMPACT.EXACT_PAGE_KEY_CORRUPT",
                           "index.compact.exact_page_key_corrupt",
                           "exact_page_key_size_corrupt");
      }
      const byte* key_bytes = nullptr;
      if (!ReadBytes(serialized, &offset, static_cast<std::size_t>(key_size),
                     &key_bytes)) {
        return RefuseExact("INDEX_COMPACT.EXACT_PAGE_TRUNCATED",
                           "index.compact.exact_page_truncated",
                           "exact_page_key_truncated");
      }
      record.encoded_key.assign(key_bytes, key_bytes + key_size);
    }
    u64 flags = 0;
    u64 payload_size = 0;
    if (!ReadUuid(serialized, &offset, &record.row_uuid) ||
        !ReadUuid(serialized, &offset, &record.version_uuid) ||
        !ReadUvarint(serialized, &offset, &record.row_ordinal) ||
        !ReadUvarint(serialized, &offset, &flags) ||
        flags > std::numeric_limits<u32>::max() ||
        !ReadUvarint(serialized, &offset, &payload_size) ||
        payload_size > std::numeric_limits<u32>::max()) {
      return RefuseExact("INDEX_COMPACT.EXACT_PAGE_RECORD_CORRUPT",
                         "index.compact.exact_page_record_corrupt",
                         "exact_page_record_corrupt");
    }
    record.flags = static_cast<u32>(flags);
    const byte* payload = nullptr;
    if (!ReadBytes(serialized, &offset, static_cast<std::size_t>(payload_size),
                   &payload)) {
      return RefuseExact("INDEX_COMPACT.EXACT_PAGE_TRUNCATED",
                         "index.compact.exact_page_truncated",
                         "exact_page_payload_truncated");
    }
    record.payload_metadata.assign(payload, payload + payload_size);
    records.push_back(std::move(record));
  }
  if (offset != serialized.size() - sizeof(u64)) {
    return RefuseExact("INDEX_COMPACT.EXACT_PAGE_TRAILING_BYTES",
                       "index.compact.exact_page_trailing_bytes",
                       "exact_page_trailing_bytes");
  }
  if (const auto reason = ValidateExactRecords(records)) {
    return RefuseExact("INDEX_COMPACT.EXACT_PAGE_VALIDATION_FAILED",
                       "index.compact.exact_page_validation_failed",
                       *reason);
  }

  ExactIndexPageCompactResult result;
  result.status = OkStatus();
  result.compressed = compact;
  result.fallback_uncompressed = fallback;
  result.exact_round_trip = true;
  result.order_preserved = true;
  result.encoding = kind;
  result.serialized = serialized;
  result.records = std::move(records);
  result.diagnostic = MakeIndexCompactEncodingDiagnostic(
      result.status, "INDEX_COMPACT.EXACT_PAGE_OK",
      "index.compact.exact_page_ok",
      IndexCompactEncodingKindName(kind));
  result.evidence = BaseEvidence(kind);
  result.evidence.push_back("exact_page.round_trip=true");
  result.evidence.push_back("exact_page.order_preserved=true");
  result.evidence.push_back("exact_page.record_count=" +
                            std::to_string(result.records.size()));
  result.evidence.push_back("exact_page.corruption_detected=false");
  return result;
}

u32 PostingProofFlags(const IndexPostingList& list) {
  u32 flags = 0;
  if (list.compressed_duplicates) {
    flags |= kPostingFlagCompressedDuplicates;
  }
  if (list.recheck_required) {
    flags |= kPostingFlagRecheckRequired;
  }
  if (list.equality_proof.proof_present) {
    flags |= kPostingFlagProofPresent;
  }
  if (list.equality_proof.non_unique_exact) {
    flags |= kPostingFlagProofNonUniqueExact;
  }
  if (list.equality_proof.encoded_key_bytewise_stable) {
    flags |= kPostingFlagProofBytewiseStable;
  }
  if (list.equality_proof.stable_row_uuid_locators) {
    flags |= kPostingFlagProofStableLocators;
  }
  if (list.equality_proof.preserves_mga_visibility_recheck) {
    flags |= kPostingFlagProofMgaRecheck;
  }
  if (list.equality_proof.parser_or_reference_finality_authority) {
    flags |= kPostingFlagProofParserReferenceAuthority;
  }
  if (list.equality_proof.timestamp_or_uuid_order_finality_authority) {
    flags |= kPostingFlagProofTimestampUuidAuthority;
  }
  return flags;
}

IndexPostingEqualityProof PostingProofFromFlags(u32 flags) {
  IndexPostingEqualityProof proof;
  proof.proof_present = (flags & kPostingFlagProofPresent) != 0;
  proof.non_unique_exact = (flags & kPostingFlagProofNonUniqueExact) != 0;
  proof.encoded_key_bytewise_stable =
      (flags & kPostingFlagProofBytewiseStable) != 0;
  proof.stable_row_uuid_locators =
      (flags & kPostingFlagProofStableLocators) != 0;
  proof.preserves_mga_visibility_recheck =
      (flags & kPostingFlagProofMgaRecheck) != 0;
  proof.parser_or_reference_finality_authority =
      (flags & kPostingFlagProofParserReferenceAuthority) != 0;
  proof.timestamp_or_uuid_order_finality_authority =
      (flags & kPostingFlagProofTimestampUuidAuthority) != 0;
  return proof;
}

bool PostingUuidLess(const TypedUuid& left, const TypedUuid& right) {
  return CompareTypedUuid(left, right) < 0;
}

int ComparePostingEntries(const IndexPostingEntry& left,
                          const IndexPostingEntry& right) {
  const int table = CompareTypedUuid(left.locator.table_uuid,
                                     right.locator.table_uuid);
  if (table != 0) {
    return table;
  }
  const int row = CompareTypedUuid(left.locator.row_uuid,
                                   right.locator.row_uuid);
  if (row != 0) {
    return row;
  }
  const int version = CompareTypedUuid(left.locator.version_uuid,
                                       right.locator.version_uuid);
  if (version != 0) {
    return version;
  }
  if (left.locator.local_transaction_id !=
      right.locator.local_transaction_id) {
    return left.locator.local_transaction_id <
                   right.locator.local_transaction_id
               ? -1
               : 1;
  }
  if (left.visible_from_transaction_id != right.visible_from_transaction_id) {
    return left.visible_from_transaction_id < right.visible_from_transaction_id
               ? -1
               : 1;
  }
  if (left.visible_until_transaction_id != right.visible_until_transaction_id) {
    return left.visible_until_transaction_id <
                   right.visible_until_transaction_id
               ? -1
               : 1;
  }
  if (left.flags != right.flags) {
    return left.flags < right.flags ? -1 : 1;
  }
  return 0;
}

void SortPostingEntries(std::vector<IndexPostingEntry>* entries) {
  std::sort(entries->begin(), entries->end(),
            [](const auto& left, const auto& right) {
              return ComparePostingEntries(left, right) < 0;
            });
}

bool PostingEntryValid(const IndexPostingEntry& entry) {
  return entry.locator.table_uuid.valid() &&
         ValidRowUuid(entry.locator.row_uuid) &&
         ValidRowUuid(entry.locator.version_uuid) &&
         entry.locator.local_transaction_id != 0 &&
         entry.visible_from_transaction_id != 0;
}

std::optional<std::string> ValidatePostingListForCompact(
    const IndexPostingList& list) {
  if (list.entries.empty()) {
    return std::string("posting_list_empty");
  }
  if (!list.index_uuid.valid() || list.encoded_key.empty()) {
    return std::string("posting_list_header_invalid");
  }
  if (list.equality_proof.parser_or_reference_finality_authority ||
      list.equality_proof.timestamp_or_uuid_order_finality_authority) {
    return std::string("posting_list_unsafe_authority");
  }
  for (std::size_t i = 0; i < list.entries.size(); ++i) {
    if (!PostingEntryValid(list.entries[i])) {
      return "posting_list_entry_invalid";
    }
    if (i != 0 && ComparePostingEntries(list.entries[i - 1], list.entries[i]) > 0) {
      return "posting_list_entries_unsorted";
    }
  }
  return std::nullopt;
}

bool PostingEntriesEqual(const IndexPostingEntry& left,
                         const IndexPostingEntry& right) {
  return left.locator.table_uuid.kind == right.locator.table_uuid.kind &&
         left.locator.table_uuid.value == right.locator.table_uuid.value &&
         left.locator.row_uuid.kind == right.locator.row_uuid.kind &&
         left.locator.row_uuid.value == right.locator.row_uuid.value &&
         left.locator.version_uuid.kind == right.locator.version_uuid.kind &&
         left.locator.version_uuid.value == right.locator.version_uuid.value &&
         left.locator.local_transaction_id ==
             right.locator.local_transaction_id &&
         left.visible_from_transaction_id == right.visible_from_transaction_id &&
         left.visible_until_transaction_id ==
             right.visible_until_transaction_id &&
         left.flags == right.flags;
}

bool PostingListsEqual(IndexPostingList left, IndexPostingList right) {
  SortPostingEntries(&left.entries);
  SortPostingEntries(&right.entries);
  if (left.index_uuid.kind != right.index_uuid.kind ||
      left.index_uuid.value != right.index_uuid.value ||
      left.encoded_key != right.encoded_key ||
      left.compressed_duplicates != right.compressed_duplicates ||
      left.recheck_required != right.recheck_required ||
      PostingProofFlags(left) != PostingProofFlags(right) ||
      left.entries.size() != right.entries.size()) {
    return false;
  }
  for (std::size_t i = 0; i < left.entries.size(); ++i) {
    if (!PostingEntriesEqual(left.entries[i], right.entries[i])) {
      return false;
    }
  }
  return true;
}

u64 EstimatePostingUncompressedBytes(const IndexPostingList& list) {
  return 8 + 2 + 2 + 4 + 17 +
         UvarintSize(list.encoded_key.size()) + list.encoded_key.size() +
         UvarintSize(list.entries.size()) +
         static_cast<u64>(list.entries.size()) *
             (17 + 17 + 17 + 8 + 8 + 8 + 4);
}

std::vector<byte> SerializePostingList(const IndexPostingList& input,
                                       IndexCompactEncodingKind encoding) {
  IndexPostingList list = input;
  SortPostingEntries(&list.entries);
  const auto& magic = encoding == IndexCompactEncodingKind::posting_varint
                          ? kPostingCompactMagic
                          : kPostingFallbackMagic;
  std::vector<byte> out(magic.begin(), magic.end());
  Append16(&out, kFormatVersion);
  Append16(&out, static_cast<u16>(encoding));
  Append32(&out, PostingProofFlags(list));
  AppendUuid(&out, list.index_uuid);
  AppendUvarint(&out, static_cast<u64>(list.encoded_key.size()));
  out.insert(out.end(), list.encoded_key.begin(), list.encoded_key.end());
  AppendUvarint(&out, static_cast<u64>(list.entries.size()));
  for (const auto& entry : list.entries) {
    AppendUuid(&out, entry.locator.table_uuid);
    AppendUuid(&out, entry.locator.row_uuid);
    AppendUuid(&out, entry.locator.version_uuid);
    if (encoding == IndexCompactEncodingKind::posting_varint) {
      AppendUvarint(&out, entry.locator.local_transaction_id);
      AppendUvarint(&out, entry.visible_from_transaction_id);
      AppendUvarint(&out, entry.visible_until_transaction_id);
      AppendUvarint(&out, entry.flags);
    } else {
      Append64(&out, entry.locator.local_transaction_id);
      Append64(&out, entry.visible_from_transaction_id);
      Append64(&out, entry.visible_until_transaction_id);
      Append32(&out, entry.flags);
    }
  }
  AppendChecksum(&out);
  return out;
}

bool DecodePostingInteger(const std::vector<byte>& serialized,
                          std::size_t* offset,
                          bool compact,
                          u64* value) {
  if (compact) {
    return ReadUvarint(serialized, offset, value);
  }
  return Read64(serialized, offset, value);
}

CompactPostingListResult DecodePostingInternal(
    const std::vector<byte>& serialized,
    const IndexCompactAuthorityContext& authority) {
  if (!ValidateCompactAuthority(authority, false)) {
    return RefusePosting("INDEX_COMPACT.UNSAFE_AUTHORITY",
                         "index.compact.unsafe_authority",
                         "compact_authority_context_invalid");
  }
  if (serialized.size() < 8 + 2 + 2 + 4 + 17 + 8) {
    return RefusePosting("INDEX_COMPACT.POSTING_TRUNCATED",
                         "index.compact.posting_truncated",
                         "posting_list_truncated");
  }
  if (!ChecksumValid(serialized)) {
    return RefusePosting("INDEX_COMPACT.POSTING_CHECKSUM_MISMATCH",
                         "index.compact.posting_checksum_mismatch",
                         "posting_list_checksum_mismatch");
  }
  const bool compact = HasMagic(serialized, kPostingCompactMagic);
  const bool fallback = HasMagic(serialized, kPostingFallbackMagic);
  if (!compact && !fallback) {
    return RefusePosting("INDEX_COMPACT.POSTING_BAD_MAGIC",
                         "index.compact.posting_bad_magic",
                         "posting_list_bad_magic");
  }

  std::size_t offset = 8;
  u16 version = 0;
  u16 raw_kind = 0;
  u32 flags = 0;
  if (!Read16(serialized, &offset, &version) ||
      !Read16(serialized, &offset, &raw_kind) ||
      !Read32(serialized, &offset, &flags)) {
    return RefusePosting("INDEX_COMPACT.POSTING_TRUNCATED",
                         "index.compact.posting_truncated",
                         "posting_list_header_truncated");
  }
  if (version != kFormatVersion) {
    return RefusePosting("INDEX_COMPACT.POSTING_BAD_VERSION",
                         "index.compact.posting_bad_version",
                         "posting_list_bad_version");
  }
  const auto kind = static_cast<IndexCompactEncodingKind>(raw_kind);
  if ((compact && kind != IndexCompactEncodingKind::posting_varint) ||
      (fallback && kind != IndexCompactEncodingKind::posting_uncompressed)) {
    return RefusePosting("INDEX_COMPACT.POSTING_KIND_MISMATCH",
                         "index.compact.posting_kind_mismatch",
                         "posting_list_kind_mismatch");
  }

  IndexPostingList list;
  list.compressed_duplicates =
      (flags & kPostingFlagCompressedDuplicates) != 0;
  list.recheck_required = (flags & kPostingFlagRecheckRequired) != 0;
  list.equality_proof = PostingProofFromFlags(flags);
  u64 key_size = 0;
  u64 count = 0;
  if (!ReadUuid(serialized, &offset, &list.index_uuid) ||
      !ReadUvarint(serialized, &offset, &key_size) ||
      key_size > std::numeric_limits<u32>::max()) {
    return RefusePosting("INDEX_COMPACT.POSTING_HEADER_CORRUPT",
                         "index.compact.posting_header_corrupt",
                         "posting_list_header_corrupt");
  }
  const byte* key_bytes = nullptr;
  if (!ReadBytes(serialized, &offset, static_cast<std::size_t>(key_size),
                 &key_bytes) ||
      !ReadUvarint(serialized, &offset, &count) ||
      count > std::numeric_limits<u32>::max()) {
    return RefusePosting("INDEX_COMPACT.POSTING_TRUNCATED",
                         "index.compact.posting_truncated",
                         "posting_list_key_or_count_truncated");
  }
  list.encoded_key.assign(key_bytes, key_bytes + key_size);
  list.entries.reserve(static_cast<std::size_t>(count));
  for (u64 i = 0; i < count; ++i) {
    IndexPostingEntry entry;
    u64 entry_flags = 0;
    if (!ReadUuid(serialized, &offset, &entry.locator.table_uuid) ||
        !ReadUuid(serialized, &offset, &entry.locator.row_uuid) ||
        !ReadUuid(serialized, &offset, &entry.locator.version_uuid) ||
        !DecodePostingInteger(serialized, &offset, compact,
                              &entry.locator.local_transaction_id) ||
        !DecodePostingInteger(serialized, &offset, compact,
                              &entry.visible_from_transaction_id) ||
        !DecodePostingInteger(serialized, &offset, compact,
                              &entry.visible_until_transaction_id)) {
      return RefusePosting("INDEX_COMPACT.POSTING_ENTRY_CORRUPT",
                           "index.compact.posting_entry_corrupt",
                           "posting_list_entry_corrupt");
    }
    if (compact) {
      if (!ReadUvarint(serialized, &offset, &entry_flags) ||
          entry_flags > std::numeric_limits<u32>::max()) {
        return RefusePosting("INDEX_COMPACT.POSTING_ENTRY_CORRUPT",
                             "index.compact.posting_entry_corrupt",
                             "posting_list_flags_corrupt");
      }
      entry.flags = static_cast<u32>(entry_flags);
    } else if (!Read32(serialized, &offset, &entry.flags)) {
      return RefusePosting("INDEX_COMPACT.POSTING_ENTRY_CORRUPT",
                           "index.compact.posting_entry_corrupt",
                           "posting_list_flags_corrupt");
    }
    list.entries.push_back(std::move(entry));
  }
  if (offset != serialized.size() - sizeof(u64)) {
    return RefusePosting("INDEX_COMPACT.POSTING_TRAILING_BYTES",
                         "index.compact.posting_trailing_bytes",
                         "posting_list_trailing_bytes");
  }
  SortPostingEntries(&list.entries);
  if (const auto reason = ValidatePostingListForCompact(list)) {
    return RefusePosting("INDEX_COMPACT.POSTING_VALIDATION_FAILED",
                         "index.compact.posting_validation_failed", *reason);
  }
  if (compact && list.compressed_duplicates &&
      !IndexPostingEqualityProofAccepted(list.equality_proof)) {
    return RefusePosting("INDEX_COMPACT.POSTING_PROOF_REFUSED",
                         "index.compact.posting_proof_refused",
                         "posting_list_equality_proof_refused");
  }

  CompactPostingListResult result;
  result.status = OkStatus();
  result.compressed = compact;
  result.fallback_uncompressed = fallback;
  result.exact_round_trip = true;
  result.encoding = kind;
  result.serialized = serialized;
  result.posting_list = std::move(list);
  result.diagnostic = MakeIndexCompactEncodingDiagnostic(
      result.status, "INDEX_COMPACT.POSTING_OK",
      "index.compact.posting_ok", IndexCompactEncodingKindName(kind));
  result.evidence = BaseEvidence(kind);
  result.evidence.push_back("posting_list.round_trip=true");
  result.evidence.push_back("posting_list.corruption_detected=false");
  result.evidence.push_back("posting_list.entry_count=" +
                            std::to_string(result.posting_list.entries.size()));
  result.evidence.push_back("posting_list.varint_integer_encoding=" +
                            std::string(compact ? "true" : "false"));
  return result;
}

std::vector<byte> SerializeCandidateOrdinals(const std::vector<u64>& ordinals) {
  std::vector<byte> out(kCandidateFallbackMagic.begin(),
                        kCandidateFallbackMagic.end());
  Append16(&out, kFormatVersion);
  Append16(&out,
           static_cast<u16>(
               IndexCompactEncodingKind::candidate_set_uncompressed_ordinals));
  AppendUvarint(&out, static_cast<u64>(ordinals.size()));
  for (const auto ordinal : ordinals) {
    AppendUvarint(&out, ordinal);
  }
  AppendChecksum(&out);
  return out;
}

std::optional<std::string> ValidateOrdinals(const std::vector<u64>& ordinals) {
  if (ordinals.empty()) {
    return std::string("candidate_set_empty");
  }
  for (std::size_t i = 1; i < ordinals.size(); ++i) {
    if (ordinals[i] <= ordinals[i - 1]) {
      return "candidate_set_ordinals_not_strict";
    }
  }
  return std::nullopt;
}

std::vector<byte> SerializeUuidFallback(const std::vector<TypedUuid>& keys,
                                        UuidKind expected_kind,
                                        u64 generation) {
  std::vector<byte> out(kUuidFallbackMagic.begin(), kUuidFallbackMagic.end());
  Append16(&out, kFormatVersion);
  Append16(&out, static_cast<u16>(expected_kind));
  Append64(&out, generation);
  AppendUvarint(&out, static_cast<u64>(keys.size()));
  for (const auto& key : keys) {
    AppendUuid(&out, key);
  }
  AppendChecksum(&out);
  return out;
}

bool UuidVectorsEqual(const std::vector<TypedUuid>& left,
                      const std::vector<TypedUuid>& right) {
  if (left.size() != right.size()) {
    return false;
  }
  for (std::size_t i = 0; i < left.size(); ++i) {
    if (left[i].kind != right[i].kind || left[i].value != right[i].value) {
      return false;
    }
  }
  return true;
}

std::optional<std::string> ProveUuidV7OrderEquivalence(
    const std::vector<TypedUuid>& keys,
    UuidKind expected_kind,
    std::vector<TypedUuid>* sorted) {
  if (keys.empty() || expected_kind == UuidKind::unknown) {
    return std::string("uuidv7_key_block_empty_or_kind_unknown");
  }
  for (const auto& key : keys) {
    if (key.kind != expected_kind || !key.valid()) {
      return "uuidv7_key_kind_invalid";
    }
    const auto time = ExtractUuidV7TimePrefix(key.value);
    if (!time.ok) {
      return time.refusal_reason.empty() ? "uuidv7_time_prefix_invalid"
                                         : time.refusal_reason;
    }
  }
  auto full_order = keys;
  std::sort(full_order.begin(), full_order.end(),
            [](const auto& left, const auto& right) {
              return CompareUuid128(left.value, right.value) < 0;
            });
  auto compact_order = keys;
  std::sort(compact_order.begin(), compact_order.end(),
            [&](const auto& left, const auto& right) {
              return CompareUuidV7ForIndex(left, right, expected_kind)
                         .comparison < 0;
            });
  if (!UuidVectorsEqual(full_order, compact_order)) {
    return "uuidv7_order_equivalence_failed";
  }
  if (sorted != nullptr) {
    *sorted = std::move(full_order);
  }
  return std::nullopt;
}

}  // namespace

const char* IndexCompactEncodingKindName(IndexCompactEncodingKind kind) {
  switch (kind) {
    case IndexCompactEncodingKind::exact_page_uncompressed:
      return "exact_page_uncompressed";
    case IndexCompactEncodingKind::exact_page_prefix_delta:
      return "exact_page_prefix_delta";
    case IndexCompactEncodingKind::posting_uncompressed:
      return "posting_uncompressed";
    case IndexCompactEncodingKind::posting_varint:
      return "posting_varint";
    case IndexCompactEncodingKind::candidate_set_uncompressed_ordinals:
      return "candidate_set_uncompressed_ordinals";
    case IndexCompactEncodingKind::candidate_set_bitmap:
      return "candidate_set_bitmap";
    case IndexCompactEncodingKind::uuidv7_prefix_delta:
      return "uuidv7_prefix_delta";
  }
  return "unknown";
}

const char* IndexCompactRepairStateName(IndexCompactRepairState state) {
  switch (state) {
    case IndexCompactRepairState::validated:
      return "validated";
    case IndexCompactRepairState::repaired_from_exact_source:
      return "repaired_from_exact_source";
    case IndexCompactRepairState::refused:
      return "refused";
  }
  return "unknown";
}

ExactIndexPageCompactResult BuildExactIndexPageCompactEncoding(
    const ExactIndexPageCompactRequest& request) {
  if (!ValidateCompactAuthority(request.authority, false)) {
    return RefuseExact("INDEX_COMPACT.UNSAFE_AUTHORITY",
                       "index.compact.unsafe_authority",
                       "compact_authority_context_invalid");
  }
  if (const auto reason = ValidateExactRecords(request.records)) {
    return RefuseExact("INDEX_COMPACT.EXACT_PAGE_INPUT_INVALID",
                       "index.compact.exact_page_input_invalid", *reason);
  }

  const auto compact_serialized = SerializeExactPage(
      request.records, IndexCompactEncodingKind::exact_page_prefix_delta);
  const auto fallback_serialized = SerializeExactPage(
      request.records, IndexCompactEncodingKind::exact_page_uncompressed);
  const u64 uncompressed_bytes =
      std::max<u64>(EstimateExactPageUncompressedBytes(request.records),
                    fallback_serialized.size());
  auto policy = CostDecision(request.policy,
                             CompressionFamily::kExactIndexPage,
                             request.authority,
                             uncompressed_bytes,
                             compact_serialized.size(),
                             true);
  const bool use_compact = !request.use_policy || policy.accepted;
  const auto selected_kind = use_compact
                                 ? IndexCompactEncodingKind::exact_page_prefix_delta
                                 : IndexCompactEncodingKind::exact_page_uncompressed;
  auto decoded = DecodeExactPageInternal(use_compact ? compact_serialized
                                                     : fallback_serialized,
                                         request.authority);
  if (!decoded.ok()) {
    return decoded;
  }
  decoded.policy_decision = std::move(policy);
  decoded.compressed = use_compact;
  decoded.fallback_uncompressed = !use_compact;
  decoded.encoding = selected_kind;
  decoded.exact_round_trip = ExactRecordsEqual(decoded.records, request.records);
  decoded.order_preserved = decoded.exact_round_trip;
  if (!decoded.exact_round_trip) {
    return RefuseExact("INDEX_COMPACT.EXACT_PAGE_ROUND_TRIP_MISMATCH",
                       "index.compact.exact_page_round_trip_mismatch",
                       "exact_page_round_trip_mismatch");
  }
  decoded.evidence = BaseEvidence(selected_kind);
  decoded.evidence.push_back("exact_page.round_trip=" +
                             std::string(decoded.exact_round_trip ? "true"
                                                                  : "false"));
  decoded.evidence.push_back("exact_page.order_preserved=true");
  decoded.evidence.push_back("exact_page.prefix_delta_encoding=" +
                             std::string(use_compact ? "true" : "false"));
  decoded.evidence.push_back("exact_page.costed_decision=true");
  decoded.evidence.push_back("exact_page.record_count=" +
                             std::to_string(decoded.records.size()));
  AppendPolicyEvidence(&decoded.evidence, decoded.policy_decision);
  if (!use_compact) {
    decoded.evidence.push_back("exact_page.uncompressed_fallback_used=true");
  }
  decoded.diagnostic = MakeIndexCompactEncodingDiagnostic(
      decoded.status, "INDEX_COMPACT.EXACT_PAGE_OK",
      "index.compact.exact_page_ok",
      IndexCompactEncodingKindName(selected_kind));
  return decoded;
}

ExactIndexPageCompactResult DecodeExactIndexPageCompactEncoding(
    const std::vector<byte>& serialized,
    const IndexCompactAuthorityContext& authority) {
  return DecodeExactPageInternal(serialized, authority);
}

ExactIndexPageCompactResult RepairOrValidateExactIndexPageCompactEncoding(
    const std::vector<byte>& serialized,
    const IndexCompactAuthorityContext& authority,
    const std::vector<ExactIndexPageCompactRecord>* exact_source,
    const IndexCompactRepairAdmission& admission) {
  auto opened = DecodeExactPageInternal(serialized, authority);
  if (opened.ok()) {
    opened.repair_state = IndexCompactRepairState::validated;
    opened.evidence.push_back("exact_page.repair_state=validated");
    return opened;
  }

  const auto reason = opened.refusal_reasons.empty()
                          ? std::string("unknown")
                          : opened.refusal_reasons.front();
  if (!admission.repair_admitted ||
      !admission.exact_source_available ||
      !admission.same_page_identity_proven ||
      !admission.order_proof_present ||
      exact_source == nullptr) {
    auto refused = RefuseExact("INDEX_COMPACT.EXACT_PAGE_REPAIR_REFUSED",
                               "index.compact.exact_page_repair_refused",
                               "exact_page_repair_admission_not_proven");
    refused.evidence.push_back("exact_page.original_reason=" + reason);
    refused.evidence.push_back("exact_page.repair_state=refused");
    return refused;
  }

  ExactIndexPageCompactRequest rebuild;
  rebuild.records = *exact_source;
  rebuild.authority = authority;
  rebuild.policy =
      DefaultCompressionPolicyRequest(CompressionFamily::kExactIndexPage);
  rebuild.use_policy = false;
  auto repaired = BuildExactIndexPageCompactEncoding(rebuild);
  if (!repaired.ok()) {
    auto refused = RefuseExact("INDEX_COMPACT.EXACT_PAGE_REPAIR_REFUSED",
                               "index.compact.exact_page_repair_refused",
                               repaired.refusal_reasons.empty()
                                   ? "exact_page_rebuild_input_invalid"
                                   : repaired.refusal_reasons.front());
    refused.evidence.push_back("exact_page.original_reason=" + reason);
    return refused;
  }
  repaired.repaired = true;
  repaired.repair_state = IndexCompactRepairState::repaired_from_exact_source;
  repaired.diagnostic = MakeIndexCompactEncodingDiagnostic(
      repaired.status, "INDEX_COMPACT.EXACT_PAGE_REPAIRED",
      "index.compact.exact_page_repaired",
      "exact_page_repaired_from_exact_source");
  repaired.evidence.push_back("exact_page.original_reason=" + reason);
  repaired.evidence.push_back(
      "exact_page.repair_state=repaired_from_exact_source");
  repaired.evidence.push_back("exact_page.repair.exact_source_used=true");
  repaired.evidence.push_back("exact_page.repair.non_authoritative=true");
  if (!admission.proof_detail.empty()) {
    repaired.evidence.push_back("exact_page.repair.proof_detail=" +
                                admission.proof_detail);
  }
  return repaired;
}

CompactPostingListResult BuildCompactPostingListEncoding(
    const CompactPostingListRequest& request) {
  if (!ValidateCompactAuthority(request.authority, false)) {
    return RefusePosting("INDEX_COMPACT.UNSAFE_AUTHORITY",
                         "index.compact.unsafe_authority",
                         "compact_authority_context_invalid");
  }
  IndexPostingList list = request.posting_list;
  SortPostingEntries(&list.entries);
  if (const auto reason = ValidatePostingListForCompact(list)) {
    return RefusePosting("INDEX_COMPACT.POSTING_INPUT_INVALID",
                         "index.compact.posting_input_invalid", *reason);
  }

  const auto compact_serialized =
      SerializePostingList(list, IndexCompactEncodingKind::posting_varint);
  const auto fallback_serialized =
      SerializePostingList(list, IndexCompactEncodingKind::posting_uncompressed);
  const bool proof_allows_compact =
      !list.compressed_duplicates ||
      IndexPostingEqualityProofAccepted(list.equality_proof);
  auto policy = CostDecision(request.policy,
                             CompressionFamily::kPostingList,
                             request.authority,
                             std::max<u64>(EstimatePostingUncompressedBytes(list),
                                           fallback_serialized.size()),
                             compact_serialized.size(),
                             proof_allows_compact);
  const bool use_compact =
      proof_allows_compact && (!request.use_policy || policy.accepted);
  auto decoded = DecodePostingInternal(use_compact ? compact_serialized
                                                   : fallback_serialized,
                                       request.authority);
  if (!decoded.ok()) {
    return decoded;
  }
  decoded.policy_decision = std::move(policy);
  decoded.compressed = use_compact;
  decoded.fallback_uncompressed = !use_compact;
  decoded.encoding = use_compact ? IndexCompactEncodingKind::posting_varint
                                 : IndexCompactEncodingKind::posting_uncompressed;
  decoded.exact_round_trip = PostingListsEqual(decoded.posting_list, list);
  if (!decoded.exact_round_trip) {
    return RefusePosting("INDEX_COMPACT.POSTING_ROUND_TRIP_MISMATCH",
                         "index.compact.posting_round_trip_mismatch",
                         "posting_list_round_trip_mismatch");
  }
  decoded.evidence = BaseEvidence(decoded.encoding);
  decoded.evidence.push_back("posting_list.round_trip=true");
  decoded.evidence.push_back("posting_list.costed_decision=true");
  decoded.evidence.push_back("posting_list.varint_integer_encoding=" +
                             std::string(use_compact ? "true" : "false"));
  decoded.evidence.push_back("posting_list.entry_count=" +
                             std::to_string(decoded.posting_list.entries.size()));
  decoded.evidence.push_back("posting_list.equality_proof_allows_compact=" +
                             std::string(proof_allows_compact ? "true"
                                                              : "false"));
  if (!use_compact) {
    decoded.evidence.push_back("posting_list.uncompressed_fallback_used=true");
  }
  AppendPolicyEvidence(&decoded.evidence, decoded.policy_decision);
  return decoded;
}

CompactPostingListResult DecodeCompactPostingListEncoding(
    const std::vector<byte>& serialized,
    const IndexCompactAuthorityContext& authority) {
  return DecodePostingInternal(serialized, authority);
}

CompactCandidateSetResult BuildCompactCandidateSetEncoding(
    const CompactCandidateSetRequest& request) {
  if (!ValidateCompactAuthority(request.authority, false)) {
    return RefuseCandidate("INDEX_COMPACT.UNSAFE_AUTHORITY",
                           "index.compact.unsafe_authority",
                           "compact_authority_context_invalid");
  }
  if (const auto reason = ValidateOrdinals(request.row_ordinals)) {
    return RefuseCandidate("INDEX_COMPACT.CANDIDATE_SET_INPUT_INVALID",
                           "index.compact.candidate_set_input_invalid",
                           *reason);
  }
  auto built = MakeCompressedBitmapCandidateSetFromRowOrdinals(
      request.row_ordinals,
      request.candidate_authority,
      request.deleted_overlay_present);
  if (!built.ok()) {
    return RefuseCandidate("INDEX_COMPACT.CANDIDATE_SET_INPUT_INVALID",
                           "index.compact.candidate_set_input_invalid",
                           built.refusal_reasons.empty()
                               ? "candidate_set_build_refused"
                               : built.refusal_reasons.front());
  }
  const auto compressed = SerializeCompressedBitmapCandidateSet(built.output);
  if (!compressed.ok()) {
    return RefuseCandidate("INDEX_COMPACT.CANDIDATE_SET_SERIALIZE_REFUSED",
                           "index.compact.candidate_set_serialize_refused",
                           compressed.refusal_reasons.empty()
                               ? "candidate_set_serialize_refused"
                               : compressed.refusal_reasons.front());
  }
  const auto fallback = SerializeCandidateOrdinals(request.row_ordinals);
  auto policy = CostDecision(request.policy,
                             CompressionFamily::kCandidateSet,
                             request.authority,
                             fallback.size(),
                             compressed.serialized.size(),
                             true);
  const bool use_compact = !request.use_policy || policy.accepted;

  auto decoded = DecodeCompactCandidateSetEncoding(
      use_compact ? compressed.serialized : fallback,
      request.candidate_authority,
      request.authority);
  if (!decoded.ok()) {
    return decoded;
  }
  decoded.policy_decision = std::move(policy);
  decoded.compressed = use_compact;
  decoded.fallback_uncompressed = !use_compact;
  decoded.exact_round_trip = decoded.exact_ordinals == request.row_ordinals;
  decoded.encoding = use_compact
                         ? IndexCompactEncodingKind::candidate_set_bitmap
                         : IndexCompactEncodingKind::
                               candidate_set_uncompressed_ordinals;
  if (!decoded.exact_round_trip) {
    return RefuseCandidate("INDEX_COMPACT.CANDIDATE_SET_ROUND_TRIP_MISMATCH",
                           "index.compact.candidate_set_round_trip_mismatch",
                           "candidate_set_round_trip_mismatch");
  }
  decoded.evidence = BaseEvidence(decoded.encoding);
  decoded.evidence.push_back("candidate_set.round_trip=" +
                             std::string(decoded.exact_round_trip ? "true"
                                                                  : "false"));
  decoded.evidence.push_back("candidate_set.costed_decision=true");
  decoded.evidence.push_back("candidate_set.row_ordinal_count=" +
                             std::to_string(decoded.exact_ordinals.size()));
  decoded.evidence.push_back("candidate_set.materialized_rows=false");
  decoded.evidence.push_back("candidate_set.bitmap_container_encoding=" +
                             std::string(use_compact ? "true" : "false"));
  if (!use_compact) {
    decoded.evidence.push_back("candidate_set.uncompressed_fallback_used=true");
  }
  AppendPolicyEvidence(&decoded.evidence, decoded.policy_decision);
  return decoded;
}

CompactCandidateSetResult DecodeCompactCandidateSetEncoding(
    const std::vector<byte>& serialized,
    const CandidateSetAuthorityContext& candidate_authority,
    const IndexCompactAuthorityContext& authority) {
  if (!ValidateCompactAuthority(authority, false)) {
    return RefuseCandidate("INDEX_COMPACT.UNSAFE_AUTHORITY",
                           "index.compact.unsafe_authority",
                           "compact_authority_context_invalid");
  }
  if (HasMagic(serialized, kCandidateBitmapMagic)) {
    auto parsed =
        DeserializeCompressedBitmapCandidateSet(serialized, candidate_authority);
    if (!parsed.ok()) {
      return RefuseCandidate("INDEX_COMPACT.CANDIDATE_SET_CORRUPT",
                             "index.compact.candidate_set_corrupt",
                             parsed.refusal_reasons.empty()
                                 ? "candidate_set_bitmap_corrupt"
                                 : parsed.refusal_reasons.front());
    }
    CompactCandidateSetResult result;
    result.status = OkStatus();
    result.compressed = true;
    result.encoding = IndexCompactEncodingKind::candidate_set_bitmap;
    result.serialized = serialized;
    result.candidate_set = std::move(parsed.output);
    result.exact_ordinals =
        ExpandCompactCandidateSetOrdinalsForProof(result.candidate_set);
    result.exact_round_trip = true;
    result.diagnostic = MakeIndexCompactEncodingDiagnostic(
        result.status, "INDEX_COMPACT.CANDIDATE_SET_OK",
        "index.compact.candidate_set_ok",
        IndexCompactEncodingKindName(result.encoding));
    result.evidence = BaseEvidence(result.encoding);
    result.evidence.push_back("candidate_set.round_trip=true");
    result.evidence.push_back("candidate_set.materialized_rows=false");
    result.evidence.push_back("candidate_set.bitmap_container_encoding=true");
    return result;
  }
  if (!HasMagic(serialized, kCandidateFallbackMagic)) {
    return RefuseCandidate("INDEX_COMPACT.CANDIDATE_SET_BAD_MAGIC",
                           "index.compact.candidate_set_bad_magic",
                           "candidate_set_bad_magic");
  }
  if (!ChecksumValid(serialized)) {
    return RefuseCandidate("INDEX_COMPACT.CANDIDATE_SET_CHECKSUM_MISMATCH",
                           "index.compact.candidate_set_checksum_mismatch",
                           "candidate_set_checksum_mismatch");
  }
  std::size_t offset = 8;
  u16 version = 0;
  u16 raw_kind = 0;
  u64 count = 0;
  if (!Read16(serialized, &offset, &version) ||
      !Read16(serialized, &offset, &raw_kind) ||
      !ReadUvarint(serialized, &offset, &count) ||
      version != kFormatVersion ||
      static_cast<IndexCompactEncodingKind>(raw_kind) !=
          IndexCompactEncodingKind::candidate_set_uncompressed_ordinals) {
    return RefuseCandidate("INDEX_COMPACT.CANDIDATE_SET_HEADER_CORRUPT",
                           "index.compact.candidate_set_header_corrupt",
                           "candidate_set_header_corrupt");
  }
  if (count > std::numeric_limits<u32>::max()) {
    return RefuseCandidate("INDEX_COMPACT.CANDIDATE_SET_HEADER_CORRUPT",
                           "index.compact.candidate_set_header_corrupt",
                           "candidate_set_count_corrupt");
  }
  std::vector<u64> ordinals;
  ordinals.reserve(static_cast<std::size_t>(count));
  for (u64 i = 0; i < count; ++i) {
    u64 ordinal = 0;
    if (!ReadUvarint(serialized, &offset, &ordinal)) {
      return RefuseCandidate("INDEX_COMPACT.CANDIDATE_SET_TRUNCATED",
                             "index.compact.candidate_set_truncated",
                             "candidate_set_ordinals_truncated");
    }
    ordinals.push_back(ordinal);
  }
  if (offset != serialized.size() - sizeof(u64)) {
    return RefuseCandidate("INDEX_COMPACT.CANDIDATE_SET_TRAILING_BYTES",
                           "index.compact.candidate_set_trailing_bytes",
                           "candidate_set_trailing_bytes");
  }
  if (const auto reason = ValidateOrdinals(ordinals)) {
    return RefuseCandidate("INDEX_COMPACT.CANDIDATE_SET_CORRUPT",
                           "index.compact.candidate_set_corrupt", *reason);
  }
  CompactCandidateSetResult result;
  result.status = OkStatus();
  result.fallback_uncompressed = true;
  result.encoding = IndexCompactEncodingKind::candidate_set_uncompressed_ordinals;
  result.serialized = serialized;
  result.exact_ordinals = std::move(ordinals);
  result.exact_round_trip = true;
  result.diagnostic = MakeIndexCompactEncodingDiagnostic(
      result.status, "INDEX_COMPACT.CANDIDATE_SET_OK",
      "index.compact.candidate_set_ok",
      IndexCompactEncodingKindName(result.encoding));
  result.evidence = BaseEvidence(result.encoding);
  result.evidence.push_back("candidate_set.round_trip=true");
  result.evidence.push_back("candidate_set.uncompressed_fallback_used=true");
  result.evidence.push_back("candidate_set.materialized_rows=false");
  return result;
}

std::vector<u64> ExpandCompactCandidateSetOrdinalsForProof(
    const CandidateSet& set) {
  std::vector<u64> ordinals;
  if (set.encoding != CandidateSetEncoding::compressed_bitmap) {
    return ordinals;
  }
  for (const auto& container : set.compressed_bitmap_containers) {
    switch (container.type) {
      case CandidateSetCompressedBitmapContainerType::array_sparse:
        for (const auto offset : container.array_offsets) {
          ordinals.push_back(container.base_row_ordinal + offset);
        }
        break;
      case CandidateSetCompressedBitmapContainerType::run:
        for (const auto& run : container.runs) {
          for (u64 delta = 0; delta < run.run_length; ++delta) {
            ordinals.push_back(container.base_row_ordinal +
                               run.start_offset + delta);
          }
        }
        break;
      case CandidateSetCompressedBitmapContainerType::dense_bitmap:
        for (u32 word_index = 0;
             word_index < static_cast<u32>(container.bitmap_words.size());
             ++word_index) {
          u64 word = container.bitmap_words[word_index];
          while (word != 0) {
            const u32 bit = static_cast<u32>(__builtin_ctzll(word));
            const u32 offset = word_index * 64u + bit;
            if (offset < container.ordinal_span) {
              ordinals.push_back(container.base_row_ordinal + offset);
            }
            word &= word - 1ull;
          }
        }
        break;
    }
  }
  return ordinals;
}

UuidV7CompactKeyBlockResult BuildUuidV7CompactKeyBlock(
    const UuidV7CompactKeyBlockRequest& request) {
  if (!ValidateCompactAuthority(request.authority, true)) {
    return RefuseUuid("INDEX_COMPACT.UNSAFE_AUTHORITY",
                      "index.compact.unsafe_authority",
                      "compact_authority_context_invalid");
  }
  std::vector<TypedUuid> sorted;
  if (const auto reason =
          ProveUuidV7OrderEquivalence(request.keys, request.expected_kind,
                                      &sorted)) {
    return RefuseUuid("INDEX_COMPACT.UUIDV7_ORDER_EQUIVALENCE_REFUSED",
                      "index.compact.uuidv7_order_equivalence_refused",
                      *reason);
  }

  UuidV7IndexEncodeRequest encode;
  encode.uuids = sorted;
  encode.expected_kind = request.expected_kind;
  encode.dictionary_generation = request.dictionary_generation;
  const auto compact = BuildUuidV7IndexPageEncoding(encode);
  const bool compact_candidate_ok = compact.ok;
  const auto fallback_serialized =
      SerializeUuidFallback(sorted,
                            request.expected_kind,
                            request.dictionary_generation);
  const auto compact_bytes = compact_candidate_ok
                                 ? static_cast<u64>(compact.serialized.size())
                                 : static_cast<u64>(fallback_serialized.size());
  auto policy = CostDecision(request.policy,
                             CompressionFamily::kExactIndexPage,
                             request.authority,
                             fallback_serialized.size(),
                             compact_bytes,
                             true);
  const bool use_compact =
      compact_candidate_ok && (!request.use_policy || policy.accepted);
  auto decoded = DecodeUuidV7CompactKeyBlock(
      use_compact ? compact.serialized : fallback_serialized,
      request.expected_kind,
      request.dictionary_generation,
      request.authority);
  if (!decoded.ok()) {
    return decoded;
  }
  decoded.policy_decision = std::move(policy);
  decoded.compressed = use_compact;
  decoded.fallback_uncompressed = !use_compact;
  decoded.encoding = use_compact ? IndexCompactEncodingKind::uuidv7_prefix_delta
                                 : IndexCompactEncodingKind::exact_page_uncompressed;
  decoded.exact_round_trip = UuidVectorsEqual(decoded.decoded_keys, sorted);
  decoded.order_equivalent_to_full_uuid_bytes = true;
  if (!decoded.exact_round_trip) {
    return RefuseUuid("INDEX_COMPACT.UUIDV7_ROUND_TRIP_MISMATCH",
                      "index.compact.uuidv7_round_trip_mismatch",
                      "uuidv7_round_trip_mismatch");
  }
  decoded.evidence = BaseEvidence(IndexCompactEncodingKind::uuidv7_prefix_delta);
  decoded.evidence.push_back("uuidv7.round_trip=" +
                             std::string(decoded.exact_round_trip ? "true"
                                                                  : "false"));
  decoded.evidence.push_back(
      "uuidv7.order_equivalent_to_full_uuid_bytes=true");
  decoded.evidence.push_back("uuidv7.prefix_delta_encoding=" +
                             std::string(use_compact ? "true" : "false"));
  decoded.evidence.push_back("uuidv7.costed_decision=true");
  if (!compact_candidate_ok) {
    decoded.evidence.push_back("uuidv7.compact_candidate_refusal=" +
                               compact.refusal_reason);
  }
  if (!use_compact) {
    decoded.evidence.push_back("uuidv7.uncompressed_fallback_used=true");
  }
  AppendPolicyEvidence(&decoded.evidence, decoded.policy_decision);
  return decoded;
}

UuidV7CompactKeyBlockResult DecodeUuidV7CompactKeyBlock(
    const std::vector<byte>& serialized,
    UuidKind expected_kind,
    u64 expected_dictionary_generation,
    const IndexCompactAuthorityContext& authority) {
  if (!ValidateCompactAuthority(authority, true)) {
    return RefuseUuid("INDEX_COMPACT.UNSAFE_AUTHORITY",
                      "index.compact.unsafe_authority",
                      "compact_authority_context_invalid");
  }
  if (HasMagic(serialized, kUuidV7Magic)) {
    auto decoded = DecodeUuidV7IndexPageEncoding(
        serialized, expected_kind, expected_dictionary_generation);
    if (!decoded.ok) {
      return RefuseUuid("INDEX_COMPACT.UUIDV7_CORRUPT",
                        "index.compact.uuidv7_corrupt",
                        decoded.refusal_reason.empty()
                            ? "uuidv7_compact_corrupt"
                            : decoded.refusal_reason);
    }
    std::vector<TypedUuid> sorted;
    if (const auto reason =
            ProveUuidV7OrderEquivalence(decoded.decoded_round_trip,
                                        expected_kind, &sorted)) {
      return RefuseUuid("INDEX_COMPACT.UUIDV7_ORDER_EQUIVALENCE_REFUSED",
                        "index.compact.uuidv7_order_equivalence_refused",
                        *reason);
    }
    UuidV7CompactKeyBlockResult result;
    result.status = OkStatus();
    result.compressed = true;
    result.exact_round_trip = true;
    result.order_equivalent_to_full_uuid_bytes = true;
    result.encoding = IndexCompactEncodingKind::uuidv7_prefix_delta;
    result.serialized = serialized;
    result.decoded_keys = std::move(sorted);
    result.dictionary = decoded.dictionary;
    result.diagnostic = MakeIndexCompactEncodingDiagnostic(
        result.status, "INDEX_COMPACT.UUIDV7_OK",
        "index.compact.uuidv7_ok", IndexCompactEncodingKindName(result.encoding));
    result.evidence = BaseEvidence(result.encoding);
    result.evidence.push_back("uuidv7.round_trip=true");
    result.evidence.push_back(
        "uuidv7.order_equivalent_to_full_uuid_bytes=true");
    return result;
  }
  if (!HasMagic(serialized, kUuidFallbackMagic)) {
    return RefuseUuid("INDEX_COMPACT.UUIDV7_BAD_MAGIC",
                      "index.compact.uuidv7_bad_magic",
                      "uuidv7_bad_magic");
  }
  if (!ChecksumValid(serialized)) {
    return RefuseUuid("INDEX_COMPACT.UUIDV7_CHECKSUM_MISMATCH",
                      "index.compact.uuidv7_checksum_mismatch",
                      "uuidv7_checksum_mismatch");
  }
  std::size_t offset = 8;
  u16 version = 0;
  u16 raw_kind = 0;
  u64 generation = 0;
  u64 count = 0;
  if (!Read16(serialized, &offset, &version) ||
      !Read16(serialized, &offset, &raw_kind) ||
      !Read64(serialized, &offset, &generation) ||
      !ReadUvarint(serialized, &offset, &count) ||
      version != kFormatVersion ||
      static_cast<UuidKind>(raw_kind) != expected_kind ||
      generation != expected_dictionary_generation) {
    return RefuseUuid("INDEX_COMPACT.UUIDV7_HEADER_CORRUPT",
                      "index.compact.uuidv7_header_corrupt",
                      "uuidv7_header_corrupt");
  }
  std::vector<TypedUuid> keys;
  keys.reserve(static_cast<std::size_t>(count));
  for (u64 i = 0; i < count; ++i) {
    TypedUuid key;
    if (!ReadUuid(serialized, &offset, &key)) {
      return RefuseUuid("INDEX_COMPACT.UUIDV7_TRUNCATED",
                        "index.compact.uuidv7_truncated",
                        "uuidv7_keys_truncated");
    }
    keys.push_back(key);
  }
  if (offset != serialized.size() - sizeof(u64)) {
    return RefuseUuid("INDEX_COMPACT.UUIDV7_TRAILING_BYTES",
                      "index.compact.uuidv7_trailing_bytes",
                      "uuidv7_trailing_bytes");
  }
  std::vector<TypedUuid> sorted;
  if (const auto reason =
          ProveUuidV7OrderEquivalence(keys, expected_kind, &sorted)) {
    return RefuseUuid("INDEX_COMPACT.UUIDV7_ORDER_EQUIVALENCE_REFUSED",
                      "index.compact.uuidv7_order_equivalence_refused",
                      *reason);
  }
  UuidV7CompactKeyBlockResult result;
  result.status = OkStatus();
  result.fallback_uncompressed = true;
  result.exact_round_trip = true;
  result.order_equivalent_to_full_uuid_bytes = true;
  result.encoding = IndexCompactEncodingKind::exact_page_uncompressed;
  result.serialized = serialized;
  result.decoded_keys = std::move(sorted);
  result.diagnostic = MakeIndexCompactEncodingDiagnostic(
      result.status, "INDEX_COMPACT.UUIDV7_OK",
      "index.compact.uuidv7_ok", "uuidv7_uncompressed");
  result.evidence = BaseEvidence(IndexCompactEncodingKind::uuidv7_prefix_delta);
  result.evidence.push_back("uuidv7.round_trip=true");
  result.evidence.push_back("uuidv7.uncompressed_fallback_used=true");
  result.evidence.push_back("uuidv7.order_equivalent_to_full_uuid_bytes=true");
  return result;
}

DiagnosticRecord MakeIndexCompactEncodingDiagnostic(
    Status status,
    std::string diagnostic_code,
    std::string message_key,
    std::string detail) {
  DiagnosticRecord record;
  record.status = status;
  record.diagnostic_code = std::move(diagnostic_code);
  record.message_key = std::move(message_key);
  if (!detail.empty()) {
    record.arguments.push_back(DiagnosticArgument{"detail", std::move(detail)});
  }
  record.source_component = "core.index.index_compact_encoding";
  return record;
}

}  // namespace scratchbird::core::index
