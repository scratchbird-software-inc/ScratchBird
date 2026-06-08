// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "candidate_set.hpp"

#include "compression_policy.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <map>
#include <optional>
#include <sstream>
#include <utility>

namespace scratchbird::core::index {
namespace {

using scratchbird::core::platform::DiagnosticArgument;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;
using scratchbird::core::platform::UuidKind;

constexpr std::array<byte, 8> kCompressedBitmapMagic = {'S', 'B', 'C', 'B',
                                                        'M', '0', '7', '0'};
constexpr u16 kCompressedBitmapFormatVersion = 1;
constexpr u64 kCompressedBitmapContainerOrdinalSpan = 65536;
constexpr u32 kDenseBitmapWordCount = 1024;
constexpr u32 kSparseToDenseCardinalityThreshold = 4096;
constexpr u16 kFlagDeletedOverlayPresent = 1u << 0u;
constexpr u16 kFlagRequiresExactRecheck = 1u << 1u;
constexpr u16 kFlagRequiresMgaRecheck = 1u << 2u;
constexpr u16 kFlagRequiresSecurityRecheck = 1u << 3u;
constexpr u16 kFlagNonAuthorityEvidence = 1u << 4u;

Status OkStatus() {
  return {StatusCode::ok, Severity::info, Subsystem::engine};
}

Status RefusalStatus() {
  return {StatusCode::diagnostic_invalid_record, Severity::error,
          Subsystem::engine};
}

std::string CountEvidence(std::string key, std::size_t value) {
  return std::move(key) + "=" + std::to_string(value);
}

std::vector<std::string> BaseEvidence(CandidateSetOperationKind operation,
                                      CandidateSetEncoding encoding,
                                      std::size_t row_count) {
  return {"operation=" + std::string(CandidateSetOperationKindName(operation)),
          "encoding=" + std::string(CandidateSetEncodingName(encoding)),
          CountEvidence("row_count", row_count),
          "exact_recheck.required=true",
          "mga_visibility_recheck.required=true",
          "security_authorization_recheck.required=true"};
}

std::vector<std::string> BaseCompressedBitmapEvidence(
    CandidateSetOperationKind operation,
    u64 cardinality) {
  return {"operation=" + std::string(CandidateSetOperationKindName(operation)),
          "encoding=" +
              std::string(CandidateSetEncodingName(
                  CandidateSetEncoding::compressed_bitmap)),
          "row_count.materialized=0",
          "compressed_bitmap.cardinality=" + std::to_string(cardinality),
          "exact_recheck.required=true",
          "mga_visibility_recheck.required=true",
          "security_authorization_recheck.required=true",
          "candidate_set_finality_authority=false",
          "parser_or_donor_authority=false",
          "provider_finality_authority=false",
          "wal_recovery_or_finality_authority=false"};
}

struct RowKey {
  TypedUuid uuid;

  friend bool operator<(const RowKey& left, const RowKey& right) {
    if (left.uuid.kind != right.uuid.kind) {
      return static_cast<u32>(left.uuid.kind) < static_cast<u32>(right.uuid.kind);
    }
    return left.uuid.value.bytes < right.uuid.value.bytes;
  }
};

bool RowUuidLess(const CandidateSetRow& left, const CandidateSetRow& right) {
  return RowKey{left.row_uuid} < RowKey{right.row_uuid};
}

bool SameUuid(const TypedUuid& left, const TypedUuid& right) {
  return left.kind == right.kind && left.value == right.value;
}

bool ValidRowUuid(const TypedUuid& uuid) {
  return uuid.valid() && uuid.kind == UuidKind::row;
}

bool RowsStrictlyOrdered(const std::vector<CandidateSetRow>& rows) {
  for (std::size_t i = 0; i < rows.size(); ++i) {
    if (!ValidRowUuid(rows[i].row_uuid)) {
      return false;
    }
    if (i != 0 && !RowUuidLess(rows[i - 1], rows[i])) {
      return false;
    }
  }
  return true;
}

bool EncodingSupported(CandidateSetEncoding encoding) {
  return encoding == CandidateSetEncoding::exact_row_uuid_ordered_stream ||
         encoding == CandidateSetEncoding::compressed_bitmap;
}

bool EncodingFlagsConsistent(const CandidateSet& input) {
  if (input.encoding == CandidateSetEncoding::compressed_bitmap) {
    return input.compressed;
  }
  if (input.encoding == CandidateSetEncoding::exact_row_uuid_ordered_stream) {
    return !input.compressed;
  }
  return false;
}

bool AddOverflows(u64 left, u64 right) {
  return right > std::numeric_limits<u64>::max() - left;
}

void AppendEvidence(std::vector<std::string>* out,
                    const std::vector<std::string>& values) {
  out->insert(out->end(), values.begin(), values.end());
}

CandidateSetResult Refuse(CandidateSetOperationKind operation,
                          const std::string& diagnostic_code,
                          const std::string& message_key,
                          const std::string& reason,
                          CandidateSetEncoding encoding =
                              CandidateSetEncoding::unknown) {
  CandidateSetResult result;
  result.status = RefusalStatus();
  result.fail_closed = true;
  result.operation = operation;
  result.diagnostic =
      MakeCandidateSetDiagnostic(result.status, diagnostic_code, message_key,
                                 reason);
  result.refusal_reasons.push_back(reason);
  result.evidence =
      BaseEvidence(operation, encoding, result.output.rows.size());
  result.evidence.push_back("fallback_refusal_reason=" + reason);
  result.evidence.push_back("fail_closed=true");
  return result;
}

CandidateSetSerializedResult RefuseSerialized(const std::string& diagnostic_code,
                                              const std::string& message_key,
                                              const std::string& reason) {
  CandidateSetSerializedResult result;
  result.status = RefusalStatus();
  result.fail_closed = true;
  result.diagnostic =
      MakeCandidateSetDiagnostic(result.status, diagnostic_code, message_key,
                                 reason);
  result.refusal_reasons.push_back(reason);
  result.evidence = BaseCompressedBitmapEvidence(
      CandidateSetOperationKind::build_compressed_bitmap, 0);
  result.evidence.push_back("fallback_refusal_reason=" + reason);
  result.evidence.push_back("fail_closed=true");
  return result;
}

CandidateSetCardinalityResult RefuseCardinality(
    CandidateSetOperationKind operation,
    const std::string& diagnostic_code,
    const std::string& message_key,
    const std::string& reason,
    CandidateSetEncoding encoding = CandidateSetEncoding::unknown) {
  CandidateSetCardinalityResult result;
  result.status = RefusalStatus();
  result.fail_closed = true;
  result.operation = operation;
  result.diagnostic =
      MakeCandidateSetDiagnostic(result.status, diagnostic_code, message_key,
                                 reason);
  result.refusal_reasons.push_back(reason);
  result.evidence = BaseEvidence(operation, encoding, 0);
  result.evidence.push_back("fallback_refusal_reason=" + reason);
  result.evidence.push_back("fail_closed=true");
  return result;
}

CandidateSetResult ValidateAuthority(CandidateSetOperationKind operation,
                                     const CandidateSetAuthorityContext& authority) {
  if (authority.parser_or_donor_finality_or_visibility_authority ||
      authority.client_finality_or_visibility_authority ||
      authority.provider_finality_or_visibility_authority ||
      authority.wal_recovery_or_finality_authority) {
    return Refuse(operation, "SB_CANDIDATE_SET.UNSAFE_AUTHORITY",
                  "candidate_set.unsafe_authority",
                  "unsafe_visibility_or_finality_authority");
  }
  if (!authority.engine_mga_authoritative ||
      !authority.row_mga_recheck_required) {
    return Refuse(operation, "SB_CANDIDATE_SET.MGA_RECHECK_REQUIRED",
                  "candidate_set.mga_recheck_required",
                  "missing_mga_visibility_recheck");
  }
  if (!authority.security_context_bound ||
      !authority.row_security_recheck_required) {
    return Refuse(operation, "SB_CANDIDATE_SET.SECURITY_RECHECK_REQUIRED",
                  "candidate_set.security_recheck_required",
                  "missing_security_authorization_recheck");
  }
  if (!authority.exact_recheck_available) {
    return Refuse(operation, "SB_CANDIDATE_SET.EXACT_RECHECK_REQUIRED",
                  "candidate_set.exact_recheck_required",
                  "missing_exact_recheck");
  }
  CandidateSetResult ok;
  ok.status = OkStatus();
  ok.operation = operation;
  return ok;
}

bool HasContainerPayloadForType(
    const CandidateSetCompressedBitmapContainer& container) {
  switch (container.type) {
    case CandidateSetCompressedBitmapContainerType::array_sparse:
      return container.array_offsets.size() == container.cardinality &&
             container.runs.empty() && container.bitmap_words.empty();
    case CandidateSetCompressedBitmapContainerType::run:
      return !container.runs.empty() && container.array_offsets.empty() &&
             container.bitmap_words.empty();
    case CandidateSetCompressedBitmapContainerType::dense_bitmap:
      return container.bitmap_words.size() == kDenseBitmapWordCount &&
             container.array_offsets.empty() && container.runs.empty();
  }
  return false;
}

u64 CountDenseCardinality(const std::vector<u64>& words);

bool ValidateCompressedBitmapRepresentation(const CandidateSet& set) {
  if (set.encoding != CandidateSetEncoding::compressed_bitmap ||
      !set.compressed || !set.approximate) {
    return false;
  }
  if (set.candidate_set_finality_authority ||
      set.parser_or_donor_finality_or_visibility_authority ||
      set.provider_finality_or_visibility_authority ||
      set.wal_recovery_or_finality_authority ||
      set.final_rows_authorized ||
      !set.non_authority_evidence_present) {
    return false;
  }
  if (!set.requires_exact_recheck ||
      !set.requires_mga_visibility_recheck ||
      !set.requires_security_authorization_recheck) {
    return false;
  }

  if (set.compressed_bitmap_containers.empty()) {
    return set.compressed_bitmap_cardinality == 0 &&
           !set.compressed_bitmap_row_ordinal_range_present &&
           set.compressed_bitmap_min_row_ordinal == 0 &&
           set.compressed_bitmap_max_row_ordinal == 0;
  }
  if (!set.compressed_bitmap_row_ordinal_range_present ||
      set.compressed_bitmap_min_row_ordinal >
          set.compressed_bitmap_max_row_ordinal) {
    return false;
  }

  u64 previous_base = 0;
  bool first = true;
  u64 cardinality = 0;
  u64 min_row_ordinal = 0;
  u64 max_row_ordinal = 0;
  for (const auto& container : set.compressed_bitmap_containers) {
    if (container.base_row_ordinal % kCompressedBitmapContainerOrdinalSpan != 0 ||
        container.ordinal_span == 0 ||
        container.ordinal_span > kCompressedBitmapContainerOrdinalSpan ||
        container.cardinality == 0 ||
        container.cardinality > container.ordinal_span ||
        AddOverflows(container.base_row_ordinal, container.ordinal_span - 1) ||
        !HasContainerPayloadForType(container)) {
      return false;
    }
    if (!first && container.base_row_ordinal <= previous_base) {
      return false;
    }
    u64 payload_cardinality = 0;
    switch (container.type) {
      case CandidateSetCompressedBitmapContainerType::array_sparse:
        payload_cardinality = container.array_offsets.size();
        for (std::size_t i = 0; i < container.array_offsets.size(); ++i) {
          if (container.array_offsets[i] >= container.ordinal_span ||
              (i != 0 &&
               container.array_offsets[i] <= container.array_offsets[i - 1])) {
            return false;
          }
        }
        break;
      case CandidateSetCompressedBitmapContainerType::run:
        for (std::size_t i = 0; i < container.runs.size(); ++i) {
          const auto& run = container.runs[i];
          if (run.run_length == 0 ||
              run.start_offset + run.run_length > container.ordinal_span ||
              (i != 0 &&
               run.start_offset <
                   container.runs[i - 1].start_offset +
                       container.runs[i - 1].run_length) ||
              AddOverflows(payload_cardinality, run.run_length)) {
            return false;
          }
          payload_cardinality += run.run_length;
        }
        break;
      case CandidateSetCompressedBitmapContainerType::dense_bitmap:
        for (const auto word : container.bitmap_words) {
          payload_cardinality += static_cast<u64>(__builtin_popcountll(word));
        }
        break;
    }
    if (payload_cardinality != container.cardinality) {
      return false;
    }
    first = false;
    previous_base = container.base_row_ordinal;
    u64 container_cardinality = 0;
    u32 first_offset = 0;
    u32 last_offset = 0;
    switch (container.type) {
      case CandidateSetCompressedBitmapContainerType::array_sparse:
        for (std::size_t i = 0; i < container.array_offsets.size(); ++i) {
          const auto offset = container.array_offsets[i];
          if (offset >= container.ordinal_span ||
              (i != 0 && offset <= container.array_offsets[i - 1])) {
            return false;
          }
        }
        container_cardinality =
            static_cast<u64>(container.array_offsets.size());
        first_offset = container.array_offsets.front();
        last_offset = container.array_offsets.back();
        break;
      case CandidateSetCompressedBitmapContainerType::run: {
        u64 previous_end = 0;
        bool first_run = true;
        for (const auto& run : container.runs) {
          const u64 run_start = run.start_offset;
          const u64 run_end = run_start + run.run_length;
          if (run.run_length == 0 || run_start >= container.ordinal_span ||
              run_end > container.ordinal_span ||
              (!first_run && run_start <= previous_end)) {
            return false;
          }
          if (AddOverflows(container_cardinality, run.run_length)) {
            return false;
          }
          if (first_run) {
            first_offset = run.start_offset;
            first_run = false;
          }
          last_offset = static_cast<u32>(run_end - 1);
          previous_end = run_end;
          container_cardinality += run.run_length;
        }
        break;
      }
      case CandidateSetCompressedBitmapContainerType::dense_bitmap:
        container_cardinality = CountDenseCardinality(container.bitmap_words);
        {
          bool first_dense_bit = true;
        for (u32 word_index = 0;
             word_index < static_cast<u32>(container.bitmap_words.size());
             ++word_index) {
          u64 word = container.bitmap_words[word_index];
          while (word != 0) {
            const u32 bit = static_cast<u32>(__builtin_ctzll(word));
            const u32 offset = word_index * 64u + bit;
            if (offset >= container.ordinal_span) {
              return false;
            }
            if (first_dense_bit) {
              first_offset = offset;
              first_dense_bit = false;
            }
            last_offset = offset;
            word &= word - 1ull;
          }
        }
        }
        break;
    }
    if (container_cardinality != container.cardinality ||
        AddOverflows(cardinality, container.cardinality)) {
      return false;
    }
    const u64 container_min = container.base_row_ordinal + first_offset;
    const u64 container_max = container.base_row_ordinal + last_offset;
    if (cardinality == 0) {
      min_row_ordinal = container_min;
    }
    max_row_ordinal = container_max;
    cardinality += container.cardinality;
  }
  return set.compressed_bitmap_cardinality == cardinality &&
         set.compressed_bitmap_min_row_ordinal == min_row_ordinal &&
         set.compressed_bitmap_max_row_ordinal == max_row_ordinal;
}

CandidateSet NormalizeRows(std::vector<CandidateSetRow> rows,
                           CandidateSetEncoding encoding) {
  std::map<RowKey, CandidateSetRow> merged;
  for (auto& row : rows) {
    auto key = RowKey{row.row_uuid};
    auto found = merged.find(key);
    if (found == merged.end()) {
      merged.emplace(key, std::move(row));
      continue;
    }
    found->second.score = std::max(found->second.score, row.score);
    found->second.exact_predicate_match =
        found->second.exact_predicate_match || row.exact_predicate_match;
    found->second.mga_visible = found->second.mga_visible || row.mga_visible;
    found->second.security_authorized =
        found->second.security_authorized || row.security_authorized;
    found->second.exact_payload_available =
        found->second.exact_payload_available || row.exact_payload_available;
  }

  CandidateSet set;
  set.encoding = encoding;
  set.row_uuid_ordered = true;
  set.compressed = encoding == CandidateSetEncoding::compressed_bitmap;
  set.approximate = set.compressed;
  set.requires_exact_recheck = true;
  set.requires_mga_visibility_recheck = true;
  set.requires_security_authorization_recheck = true;
  for (auto& item : merged) {
    set.rows.push_back(std::move(item.second));
  }
  return set;
}

CandidateSetResult Finish(CandidateSetOperationKind operation,
                          CandidateSet set,
                          std::vector<std::string> evidence) {
  CandidateSetResult result;
  result.status = OkStatus();
  result.operation = operation;
  result.output = std::move(set);
  result.output.evidence = evidence;
  result.evidence = std::move(evidence);
  result.diagnostic = MakeCandidateSetDiagnostic(
      result.status, "SB_CANDIDATE_SET.OK", "candidate_set.ok",
      CandidateSetOperationKindName(operation));
  return result;
}

CandidateSetResult ValidateInputSet(CandidateSetOperationKind operation,
                                    const CandidateSet& input) {
  if (!EncodingSupported(input.encoding) || !EncodingFlagsConsistent(input)) {
    return Refuse(operation, "SB_CANDIDATE_SET.UNSUPPORTED_ENCODING",
                  "candidate_set.unsupported_encoding",
                  "unsupported_or_inconsistent_candidate_set_encoding",
                  input.encoding);
  }
  if (input.encoding == CandidateSetEncoding::compressed_bitmap) {
    if (!ValidateCompressedBitmapRepresentation(input)) {
      return Refuse(operation, "SB_CANDIDATE_SET.COMPRESSED_FORMAT_INVALID",
                    "candidate_set.compressed_format_invalid",
                    "invalid_compressed_bitmap_container_representation",
                    input.encoding);
    }
    CandidateSetResult ok;
    ok.status = OkStatus();
    ok.operation = operation;
    return ok;
  }
  if (!input.row_uuid_ordered || !RowsStrictlyOrdered(input.rows)) {
    return Refuse(operation, "SB_CANDIDATE_SET.UNSORTED_EXACT_STREAM",
                  "candidate_set.unsorted_exact_stream",
                  "unsorted_exact_row_uuid_stream", input.encoding);
  }
  if (!input.requires_exact_recheck ||
      !input.requires_mga_visibility_recheck ||
      !input.requires_security_authorization_recheck) {
    return Refuse(operation, "SB_CANDIDATE_SET.RECHECK_CONTRACT_REQUIRED",
                  "candidate_set.recheck_contract_required",
                  "missing_candidate_recheck_contract", input.encoding);
  }
  CandidateSetResult ok;
  ok.status = OkStatus();
  ok.operation = operation;
  return ok;
}

std::vector<CandidateSetRow> MaterializedRowsForCompatibilityBridge(
    const CandidateSet& set) {
  return set.rows;
}

u64 Fnv1a64(const byte* data, std::size_t size) {
  u64 hash = 1469598103934665603ull;
  for (std::size_t i = 0; i < size; ++i) {
    hash ^= data[i];
    hash *= 1099511628211ull;
  }
  return hash;
}

void Append16(std::vector<byte>* out, u16 value) {
  const auto stored = scratchbird::core::platform::HostToLittle16(value);
  const auto* bytes = reinterpret_cast<const byte*>(&stored);
  out->insert(out->end(), bytes, bytes + sizeof(stored));
}

void Append32(std::vector<byte>* out, u32 value) {
  const auto stored = scratchbird::core::platform::HostToLittle32(value);
  const auto* bytes = reinterpret_cast<const byte*>(&stored);
  out->insert(out->end(), bytes, bytes + sizeof(stored));
}

void Append64(std::vector<byte>* out, u64 value) {
  const auto stored = scratchbird::core::platform::HostToLittle64(value);
  const auto* bytes = reinterpret_cast<const byte*>(&stored);
  out->insert(out->end(), bytes, bytes + sizeof(stored));
}

bool Read16(const std::vector<byte>& bytes, std::size_t* offset, u16* value) {
  if (*offset + sizeof(u16) > bytes.size()) {
    return false;
  }
  *value = scratchbird::core::platform::LoadLittle16(bytes.data() + *offset);
  *offset += sizeof(u16);
  return true;
}

bool Read32(const std::vector<byte>& bytes, std::size_t* offset, u32* value) {
  if (*offset + sizeof(u32) > bytes.size()) {
    return false;
  }
  *value = scratchbird::core::platform::LoadLittle32(bytes.data() + *offset);
  *offset += sizeof(u32);
  return true;
}

bool Read64(const std::vector<byte>& bytes, std::size_t* offset, u64* value) {
  if (*offset + sizeof(u64) > bytes.size()) {
    return false;
  }
  *value = scratchbird::core::platform::LoadLittle64(bytes.data() + *offset);
  *offset += sizeof(u64);
  return true;
}

std::vector<CandidateSetCompressedBitmapRun> BuildRuns(
    const std::vector<u16>& offsets) {
  std::vector<CandidateSetCompressedBitmapRun> runs;
  if (offsets.empty()) {
    return runs;
  }
  CandidateSetCompressedBitmapRun current;
  current.start_offset = offsets.front();
  current.run_length = 1;
  for (std::size_t i = 1; i < offsets.size(); ++i) {
    if (static_cast<u32>(offsets[i - 1]) + 1u == offsets[i]) {
      ++current.run_length;
      continue;
    }
    runs.push_back(current);
    current.start_offset = offsets[i];
    current.run_length = 1;
  }
  runs.push_back(current);
  return runs;
}

CandidateSetCompressedBitmapContainer BuildCompressedBitmapContainer(
    u64 base_row_ordinal,
    const std::vector<u16>& offsets) {
  CandidateSetCompressedBitmapContainer container;
  container.base_row_ordinal = base_row_ordinal;
  container.ordinal_span = static_cast<u32>(kCompressedBitmapContainerOrdinalSpan);
  container.cardinality = static_cast<u32>(offsets.size());

  auto runs = BuildRuns(offsets);
  if (container.cardinality > kSparseToDenseCardinalityThreshold) {
    container.type = CandidateSetCompressedBitmapContainerType::dense_bitmap;
    container.bitmap_words.assign(kDenseBitmapWordCount, 0);
    for (const auto offset : offsets) {
      container.bitmap_words[offset / 64] |= (1ull << (offset % 64));
    }
    return container;
  }
  if (!runs.empty() && runs.size() * 2 < offsets.size()) {
    container.type = CandidateSetCompressedBitmapContainerType::run;
    container.runs = std::move(runs);
    return container;
  }
  container.type = CandidateSetCompressedBitmapContainerType::array_sparse;
  container.array_offsets = offsets;
  return container;
}

u64 CountDenseCardinality(const std::vector<u64>& words) {
  u64 count = 0;
  for (const auto word : words) {
    count += static_cast<u64>(__builtin_popcountll(word));
  }
  return count;
}

u16 CompressedBitmapFlags(const CandidateSet& set) {
  u16 flags = 0;
  if (set.deleted_overlay_present) {
    flags |= kFlagDeletedOverlayPresent;
  }
  if (set.requires_exact_recheck) {
    flags |= kFlagRequiresExactRecheck;
  }
  if (set.requires_mga_visibility_recheck) {
    flags |= kFlagRequiresMgaRecheck;
  }
  if (set.requires_security_authorization_recheck) {
    flags |= kFlagRequiresSecurityRecheck;
  }
  if (set.non_authority_evidence_present) {
    flags |= kFlagNonAuthorityEvidence;
  }
  return flags;
}

CandidateSet BuildCompressedBitmapSetFromSortedOrdinals(
    const std::vector<u64>& row_ordinals,
    bool deleted_overlay_present) {
  CandidateSet set;
  set.encoding = CandidateSetEncoding::compressed_bitmap;
  set.row_uuid_ordered = false;
  set.compressed = true;
  set.approximate = true;
  set.requires_exact_recheck = true;
  set.requires_mga_visibility_recheck = true;
  set.requires_security_authorization_recheck = true;
  set.deleted_overlay_present = deleted_overlay_present;
  set.non_authority_evidence_present = true;
  set.compressed_bitmap_cardinality = static_cast<u64>(row_ordinals.size());
  set.compressed_bitmap_row_ordinal_range_present = !row_ordinals.empty();
  if (!row_ordinals.empty()) {
    set.compressed_bitmap_min_row_ordinal = row_ordinals.front();
    set.compressed_bitmap_max_row_ordinal = row_ordinals.back();
  }

  u64 current_base = 0;
  std::vector<u16> offsets;
  bool have_container = false;
  const auto flush = [&]() {
    if (have_container) {
      set.compressed_bitmap_containers.push_back(
          BuildCompressedBitmapContainer(current_base, offsets));
      offsets.clear();
    }
  };

  for (const auto ordinal : row_ordinals) {
    const u64 base =
        (ordinal / kCompressedBitmapContainerOrdinalSpan) *
        kCompressedBitmapContainerOrdinalSpan;
    if (!have_container) {
      current_base = base;
      have_container = true;
    } else if (base != current_base) {
      flush();
      current_base = base;
    }
    offsets.push_back(static_cast<u16>(ordinal - current_base));
  }
  flush();
  return set;
}

std::vector<u64> DenseWordsFromContainer(
    const CandidateSetCompressedBitmapContainer& container) {
  std::vector<u64> words(kDenseBitmapWordCount, 0);
  switch (container.type) {
    case CandidateSetCompressedBitmapContainerType::array_sparse:
      for (const auto offset : container.array_offsets) {
        words[offset / 64] |= 1ull << (offset % 64);
      }
      break;
    case CandidateSetCompressedBitmapContainerType::run:
      for (const auto& run : container.runs) {
        for (u32 delta = 0; delta < run.run_length; ++delta) {
          const u32 offset = static_cast<u32>(run.start_offset) + delta;
          words[offset / 64] |= 1ull << (offset % 64);
        }
      }
      break;
    case CandidateSetCompressedBitmapContainerType::dense_bitmap:
      words = container.bitmap_words;
      break;
  }
  return words;
}

void ClearOffsetsOutsideSpan(std::vector<u64>* words, u32 ordinal_span) {
  for (u32 offset = ordinal_span; offset < kCompressedBitmapContainerOrdinalSpan;
       ++offset) {
    (*words)[offset / 64] &= ~(1ull << (offset % 64));
  }
}

void KeepOnlyUniverseRange(std::vector<u64>* words,
                           u64 base_row_ordinal,
                           u64 universe_min_row_ordinal,
                           u64 universe_max_row_ordinal) {
  for (u32 offset = 0; offset < kCompressedBitmapContainerOrdinalSpan;
       ++offset) {
    if (AddOverflows(base_row_ordinal, offset)) {
      (*words)[offset / 64] &= ~(1ull << (offset % 64));
      continue;
    }
    const u64 ordinal = base_row_ordinal + offset;
    if (ordinal < universe_min_row_ordinal ||
        ordinal > universe_max_row_ordinal) {
      (*words)[offset / 64] &= ~(1ull << (offset % 64));
    }
  }
}

u64 CountWordsCardinality(const std::vector<u64>& words) {
  u64 count = 0;
  for (const auto word : words) {
    count += static_cast<u64>(__builtin_popcountll(word));
  }
  return count;
}

std::optional<CandidateSetCompressedBitmapContainer> BuildContainerFromWords(
    u64 base_row_ordinal,
    u32 ordinal_span,
    std::vector<u64> words) {
  ClearOffsetsOutsideSpan(&words, ordinal_span);
  const u64 cardinality = CountWordsCardinality(words);
  if (cardinality == 0) {
    return std::nullopt;
  }
  CandidateSetCompressedBitmapContainer container;
  container.base_row_ordinal = base_row_ordinal;
  container.ordinal_span = ordinal_span;
  container.cardinality = static_cast<u32>(cardinality);

  std::vector<u16> offsets;
  offsets.reserve(static_cast<std::size_t>(
      std::min<u64>(cardinality, kCompressedBitmapContainerOrdinalSpan)));
  for (u32 word_index = 0; word_index < words.size(); ++word_index) {
    u64 word = words[word_index];
    while (word != 0) {
      const u32 bit = static_cast<u32>(__builtin_ctzll(word));
      const u32 offset = word_index * 64u + bit;
      if (offset < ordinal_span) {
        offsets.push_back(static_cast<u16>(offset));
      }
      word &= word - 1ull;
    }
  }
  auto runs = BuildRuns(offsets);
  if (cardinality > kSparseToDenseCardinalityThreshold) {
    container.type = CandidateSetCompressedBitmapContainerType::dense_bitmap;
    container.bitmap_words = std::move(words);
    return container;
  }
  if (!runs.empty() && runs.size() * 2 < offsets.size()) {
    container.type = CandidateSetCompressedBitmapContainerType::run;
    container.runs = std::move(runs);
    return container;
  }
  container.type = CandidateSetCompressedBitmapContainerType::array_sparse;
  container.array_offsets = std::move(offsets);
  return container;
}

u32 FirstContainerOffset(const CandidateSetCompressedBitmapContainer& container) {
  switch (container.type) {
    case CandidateSetCompressedBitmapContainerType::array_sparse:
      return container.array_offsets.front();
    case CandidateSetCompressedBitmapContainerType::run:
      return container.runs.front().start_offset;
    case CandidateSetCompressedBitmapContainerType::dense_bitmap:
      for (u32 word_index = 0; word_index < container.bitmap_words.size();
           ++word_index) {
        const u64 word = container.bitmap_words[word_index];
        if (word != 0) {
          return word_index * 64u +
                 static_cast<u32>(__builtin_ctzll(word));
        }
      }
      break;
  }
  return 0;
}

u32 LastContainerOffset(const CandidateSetCompressedBitmapContainer& container) {
  switch (container.type) {
    case CandidateSetCompressedBitmapContainerType::array_sparse:
      return container.array_offsets.back();
    case CandidateSetCompressedBitmapContainerType::run: {
      const auto& run = container.runs.back();
      return static_cast<u32>(run.start_offset) + run.run_length - 1u;
    }
    case CandidateSetCompressedBitmapContainerType::dense_bitmap:
      for (u32 reverse = static_cast<u32>(container.bitmap_words.size());
           reverse > 0; --reverse) {
        const u64 word = container.bitmap_words[reverse - 1u];
        if (word != 0) {
          return (reverse - 1u) * 64u + 63u -
                 static_cast<u32>(__builtin_clzll(word));
        }
      }
      break;
  }
  return 0;
}

CandidateSet BuildCompressedBitmapSetFromContainers(
    std::vector<CandidateSetCompressedBitmapContainer> containers,
    bool deleted_overlay_present) {
  CandidateSet set;
  set.encoding = CandidateSetEncoding::compressed_bitmap;
  set.row_uuid_ordered = false;
  set.compressed = true;
  set.approximate = true;
  set.requires_exact_recheck = true;
  set.requires_mga_visibility_recheck = true;
  set.requires_security_authorization_recheck = true;
  set.deleted_overlay_present = deleted_overlay_present;
  set.non_authority_evidence_present = true;
  set.compressed_bitmap_containers = std::move(containers);

  for (std::size_t i = 0; i < set.compressed_bitmap_containers.size(); ++i) {
    const auto& container = set.compressed_bitmap_containers[i];
    set.compressed_bitmap_cardinality += container.cardinality;
    const u64 min_ordinal =
        container.base_row_ordinal + FirstContainerOffset(container);
    const u64 max_ordinal =
        container.base_row_ordinal + LastContainerOffset(container);
    if (i == 0) {
      set.compressed_bitmap_min_row_ordinal = min_ordinal;
    }
    set.compressed_bitmap_max_row_ordinal = max_ordinal;
  }
  set.compressed_bitmap_row_ordinal_range_present =
      !set.compressed_bitmap_containers.empty();
  return set;
}

std::vector<std::string> CompressedBitmapAlgebraEvidence(
    CandidateSetOperationKind operation,
    const CandidateSet& output,
    std::size_t input_container_count,
    std::string action) {
  auto evidence =
      BaseCompressedBitmapEvidence(operation, output.compressed_bitmap_cardinality);
  evidence.push_back("compressed_bitmap.algebra=container_level");
  evidence.push_back("compressed_bitmap.operation_kind=" +
                     std::string(CandidateSetOperationKindName(operation)));
  evidence.push_back("compressed_bitmap.materialized_row_expansion=false");
  evidence.push_back("compatibility_bridge.used=false");
  evidence.push_back("candidate_stream_only=true");
  evidence.push_back("candidate_set_finality_authority=false");
  evidence.push_back("parser_or_donor_authority=false");
  evidence.push_back("provider_finality_authority=false");
  evidence.push_back("wal_recovery_or_finality_authority=false");
  evidence.push_back("exact_recheck.required=true");
  evidence.push_back("mga_visibility_recheck.required=true");
  evidence.push_back("security_authorization_recheck.required=true");
  evidence.push_back("compressed_bitmap.deleted_overlay_present=" +
                     std::string(output.deleted_overlay_present ? "true"
                                                                : "false"));
  evidence.push_back(CountEvidence("compressed_bitmap.input_container_count",
                                   input_container_count));
  evidence.push_back(CountEvidence("compressed_bitmap.output_container_count",
                                   output.compressed_bitmap_containers.size()));
  evidence.push_back("compressed_bitmap.action=" + std::move(action));
  return evidence;
}

bool BothCompressedBitmaps(const CandidateSet& left, const CandidateSet& right) {
  return left.encoding == CandidateSetEncoding::compressed_bitmap &&
         right.encoding == CandidateSetEncoding::compressed_bitmap;
}

CandidateSetResult RefuseMixedCompressedExact(CandidateSetOperationKind operation) {
  return Refuse(operation, "SB_CANDIDATE_SET.MIXED_ENCODING_UNSUPPORTED",
                "candidate_set.mixed_encoding_unsupported",
                "mixed_compressed_exact_algebra_unsupported");
}

bool HasMaterializedRowsForMixedAlgebra(const CandidateSet& set) {
  if (set.encoding != CandidateSetEncoding::compressed_bitmap) {
    return true;
  }
  return !set.rows.empty() &&
         set.row_uuid_ordered &&
         RowsStrictlyOrdered(set.rows) &&
         set.rows.size() == set.compressed_bitmap_cardinality;
}

bool MixedCompressedExactCanUseMaterializedRows(const CandidateSet& left,
                                                const CandidateSet& right) {
  return HasMaterializedRowsForMixedAlgebra(left) &&
         HasMaterializedRowsForMixedAlgebra(right);
}

CandidateSetResult CompressedBitmapBinaryAlgebra(
    CandidateSetOperationKind operation,
    const CandidateSet& left,
    const CandidateSet& right) {
  std::vector<CandidateSetCompressedBitmapContainer> containers;
  std::size_t left_index = 0;
  std::size_t right_index = 0;
  while (left_index < left.compressed_bitmap_containers.size() ||
         right_index < right.compressed_bitmap_containers.size()) {
    const CandidateSetCompressedBitmapContainer* left_container = nullptr;
    const CandidateSetCompressedBitmapContainer* right_container = nullptr;
    u64 base = 0;
    u32 span = static_cast<u32>(kCompressedBitmapContainerOrdinalSpan);

    if (left_index < left.compressed_bitmap_containers.size() &&
        (right_index >= right.compressed_bitmap_containers.size() ||
         left.compressed_bitmap_containers[left_index].base_row_ordinal <
             right.compressed_bitmap_containers[right_index].base_row_ordinal)) {
      left_container = &left.compressed_bitmap_containers[left_index++];
      base = left_container->base_row_ordinal;
      span = left_container->ordinal_span;
    } else if (right_index < right.compressed_bitmap_containers.size() &&
               (left_index >= left.compressed_bitmap_containers.size() ||
                right.compressed_bitmap_containers[right_index].base_row_ordinal <
                    left.compressed_bitmap_containers[left_index]
                        .base_row_ordinal)) {
      right_container = &right.compressed_bitmap_containers[right_index++];
      base = right_container->base_row_ordinal;
      span = right_container->ordinal_span;
    } else {
      left_container = &left.compressed_bitmap_containers[left_index++];
      right_container = &right.compressed_bitmap_containers[right_index++];
      base = left_container->base_row_ordinal;
      span = std::max(left_container->ordinal_span, right_container->ordinal_span);
    }

    std::vector<u64> words(kDenseBitmapWordCount, 0);
    std::vector<u64> left_words(kDenseBitmapWordCount, 0);
    std::vector<u64> right_words(kDenseBitmapWordCount, 0);
    if (left_container != nullptr) {
      left_words = DenseWordsFromContainer(*left_container);
    }
    if (right_container != nullptr) {
      right_words = DenseWordsFromContainer(*right_container);
    }
    for (std::size_t i = 0; i < words.size(); ++i) {
      switch (operation) {
        case CandidateSetOperationKind::union_sets:
          words[i] = left_words[i] | right_words[i];
          break;
        case CandidateSetOperationKind::intersect_sets:
          words[i] = left_words[i] & right_words[i];
          break;
        case CandidateSetOperationKind::subtract_sets:
          words[i] = left_words[i] & ~right_words[i];
          break;
        default:
          break;
      }
    }
    if (auto container = BuildContainerFromWords(base, span, std::move(words))) {
      containers.push_back(std::move(*container));
    }
  }

  const auto input_container_count =
      left.compressed_bitmap_containers.size() +
      right.compressed_bitmap_containers.size();
  const bool deleted_overlay_present =
      left.deleted_overlay_present || right.deleted_overlay_present;
  auto set = BuildCompressedBitmapSetFromContainers(std::move(containers),
                                                    deleted_overlay_present);
  auto evidence = CompressedBitmapAlgebraEvidence(
      operation, set, input_container_count,
      operation == CandidateSetOperationKind::union_sets
          ? "or_union"
          : operation == CandidateSetOperationKind::intersect_sets
                ? "and_intersection"
                : "andnot_subtract");
  evidence.push_back(CountEvidence("compressed_bitmap.left_container_count",
                                   left.compressed_bitmap_containers.size()));
  evidence.push_back(CountEvidence("compressed_bitmap.right_container_count",
                                   right.compressed_bitmap_containers.size()));
  if (deleted_overlay_present) {
    evidence.push_back(
        "compressed_bitmap.deleted_overlay_algebra=preserved_for_exact_recheck");
  }
  return Finish(operation, std::move(set), std::move(evidence));
}

CandidateSetResult CompressedBitmapFirstK(const CandidateSet& input, u64 k) {
  std::vector<CandidateSetCompressedBitmapContainer> containers;
  u64 remaining = k;
  for (const auto& source : input.compressed_bitmap_containers) {
    if (remaining == 0) {
      break;
    }
    auto source_words = DenseWordsFromContainer(source);
    std::vector<u64> output_words(kDenseBitmapWordCount, 0);
    for (u32 word_index = 0; word_index < source_words.size(); ++word_index) {
      u64 word = source_words[word_index];
      while (word != 0 && remaining != 0) {
        const u32 bit = static_cast<u32>(__builtin_ctzll(word));
        output_words[word_index] |= 1ull << bit;
        word &= word - 1ull;
        --remaining;
      }
      if (remaining == 0) {
        break;
      }
    }
    if (auto container =
            BuildContainerFromWords(source.base_row_ordinal,
                                    source.ordinal_span,
                                    std::move(output_words))) {
      containers.push_back(std::move(*container));
    }
  }
  auto set = BuildCompressedBitmapSetFromContainers(
      std::move(containers), input.deleted_overlay_present);
  auto evidence = CompressedBitmapAlgebraEvidence(
      CandidateSetOperationKind::top_k, set,
      input.compressed_bitmap_containers.size(), "first_k_ordinal");
  evidence.push_back("top_k.action=first_k_ordinal");
  evidence.push_back("top_k.k=" + std::to_string(k));
  if (input.deleted_overlay_present) {
    evidence.push_back(
        "compressed_bitmap.deleted_overlay_algebra=preserved_for_exact_recheck");
  }
  return Finish(CandidateSetOperationKind::top_k, std::move(set),
                std::move(evidence));
}

}  // namespace

const char* CandidateSetEncodingName(CandidateSetEncoding encoding) {
  switch (encoding) {
    case CandidateSetEncoding::exact_row_uuid_ordered_stream:
      return "exact_row_uuid_ordered_stream";
    case CandidateSetEncoding::compressed_bitmap:
      return "compressed_bitmap";
    case CandidateSetEncoding::unknown:
      break;
  }
  return "unknown";
}

const char* CandidateSetOperationKindName(CandidateSetOperationKind operation) {
  switch (operation) {
    case CandidateSetOperationKind::build_exact_stream:
      return "build_exact_stream";
    case CandidateSetOperationKind::build_compressed_bitmap:
      return "build_compressed_bitmap";
    case CandidateSetOperationKind::union_sets:
      return "union";
    case CandidateSetOperationKind::intersect_sets:
      return "intersect";
    case CandidateSetOperationKind::subtract_sets:
      return "subtract";
    case CandidateSetOperationKind::top_k:
      return "top_k";
    case CandidateSetOperationKind::rerank:
      return "rerank";
    case CandidateSetOperationKind::exact_recheck:
      return "exact_recheck";
    case CandidateSetOperationKind::complement:
      return "complement";
    case CandidateSetOperationKind::popcount:
      return "popcount";
  }
  return "unknown";
}

const char* CandidateSetCompressionPolicyFamilyName() {
  return CompressionFamilyName(CompressionFamily::kCandidateSet);
}

const char* CandidateSetCompressedBitmapContainerTypeName(
    CandidateSetCompressedBitmapContainerType type) {
  switch (type) {
    case CandidateSetCompressedBitmapContainerType::array_sparse:
      return "array_sparse";
    case CandidateSetCompressedBitmapContainerType::run:
      return "run";
    case CandidateSetCompressedBitmapContainerType::dense_bitmap:
      return "dense_bitmap";
  }
  return "unknown";
}

CandidateSetResult MakeExactRowUuidOrderedCandidateSet(
    std::vector<CandidateSetRow> rows,
    const CandidateSetAuthorityContext& authority,
    bool require_ordered_input) {
  const auto authority_result =
      ValidateAuthority(CandidateSetOperationKind::build_exact_stream,
                        authority);
  if (!authority_result.ok()) {
    return authority_result;
  }
  if (require_ordered_input && !RowsStrictlyOrdered(rows)) {
    return Refuse(CandidateSetOperationKind::build_exact_stream,
                  "SB_CANDIDATE_SET.UNSORTED_EXACT_STREAM",
                  "candidate_set.unsorted_exact_stream",
                  "unsorted_exact_row_uuid_stream",
                  CandidateSetEncoding::exact_row_uuid_ordered_stream);
  }
  for (const auto& row : rows) {
    if (!ValidRowUuid(row.row_uuid)) {
      return Refuse(CandidateSetOperationKind::build_exact_stream,
                    "SB_CANDIDATE_SET.EXACT_ROW_UUID_REQUIRED",
                    "candidate_set.exact_row_uuid_required",
                    "missing_exact_row_uuid",
                    CandidateSetEncoding::exact_row_uuid_ordered_stream);
    }
  }
  std::sort(rows.begin(), rows.end(), RowUuidLess);
  auto set = NormalizeRows(std::move(rows),
                           CandidateSetEncoding::exact_row_uuid_ordered_stream);
  auto evidence = BaseEvidence(CandidateSetOperationKind::build_exact_stream,
                               set.encoding, set.rows.size());
  evidence.push_back("exact_stream.row_uuid_ordered=true");
  evidence.push_back("candidate_stream_only=true");
  return Finish(CandidateSetOperationKind::build_exact_stream, std::move(set),
                std::move(evidence));
}

CandidateSetResult MakeCompressedBitmapCandidateSet(
    std::vector<CandidateSetRow> row_dictionary,
    std::vector<CandidateSetCompressedRange> ranges,
    const CandidateSetAuthorityContext& authority) {
  const auto authority_result =
      ValidateAuthority(CandidateSetOperationKind::build_compressed_bitmap,
                        authority);
  if (!authority_result.ok()) {
    return authority_result;
  }
  if (!RowsStrictlyOrdered(row_dictionary)) {
    return Refuse(CandidateSetOperationKind::build_compressed_bitmap,
                  "SB_CANDIDATE_SET.COMPRESSED_INPUT_CORRUPT",
                  "candidate_set.compressed_input_corrupt",
                  "compressed_dictionary_not_row_uuid_ordered",
                  CandidateSetEncoding::compressed_bitmap);
  }

  std::vector<CandidateSetRow> rows;
  std::vector<u64> row_ordinals;
  u64 previous_end = 0;
  bool first = true;
  for (const auto& range : ranges) {
    if (range.run_length == 0 ||
        AddOverflows(range.start_ordinal, range.run_length) ||
        range.start_ordinal + range.run_length > row_dictionary.size()) {
      return Refuse(CandidateSetOperationKind::build_compressed_bitmap,
                    "SB_CANDIDATE_SET.COMPRESSED_INPUT_CORRUPT",
                    "candidate_set.compressed_input_corrupt",
                    "compressed_bitmap_range_invalid",
                    CandidateSetEncoding::compressed_bitmap);
    }
    if (!first && range.start_ordinal < previous_end) {
      return Refuse(CandidateSetOperationKind::build_compressed_bitmap,
                    "SB_CANDIDATE_SET.COMPRESSED_INPUT_CORRUPT",
                    "candidate_set.compressed_input_corrupt",
                    "compressed_bitmap_range_overlap",
                    CandidateSetEncoding::compressed_bitmap);
    }
    first = false;
    previous_end = range.start_ordinal + range.run_length;
    for (u64 offset = 0; offset < range.run_length; ++offset) {
      rows.push_back(row_dictionary[range.start_ordinal + offset]);
      row_ordinals.push_back(range.start_ordinal + offset);
    }
  }

  auto set = NormalizeRows(std::move(rows), CandidateSetEncoding::compressed_bitmap);
  auto format = BuildCompressedBitmapSetFromSortedOrdinals(row_ordinals, false);
  set.row_uuid_ordered = true;
  set.deleted_overlay_present = false;
  set.non_authority_evidence_present = true;
  set.compressed_bitmap_cardinality = format.compressed_bitmap_cardinality;
  set.compressed_bitmap_row_ordinal_range_present =
      format.compressed_bitmap_row_ordinal_range_present;
  set.compressed_bitmap_min_row_ordinal =
      format.compressed_bitmap_min_row_ordinal;
  set.compressed_bitmap_max_row_ordinal =
      format.compressed_bitmap_max_row_ordinal;
  set.compressed_bitmap_containers =
      std::move(format.compressed_bitmap_containers);
  set.compressed_ranges = std::move(ranges);
  auto evidence =
      BaseEvidence(CandidateSetOperationKind::build_compressed_bitmap,
                   set.encoding, set.rows.size());
  evidence.push_back(CountEvidence("compressed_bitmap.range_count",
                                   set.compressed_ranges.size()));
  evidence.push_back(CountEvidence("compressed_bitmap.container_count",
                                   set.compressed_bitmap_containers.size()));
  evidence.push_back("compatibility_bridge.materialized_rows=true");
  evidence.push_back("candidate_set_finality_authority=false");
  return Finish(CandidateSetOperationKind::build_compressed_bitmap,
                std::move(set), std::move(evidence));
}

CandidateSetResult MakeCompressedBitmapCandidateSetFromRowOrdinals(
    std::vector<u64> row_ordinals,
    const CandidateSetAuthorityContext& authority,
    bool deleted_overlay_present) {
  const auto authority_result =
      ValidateAuthority(CandidateSetOperationKind::build_compressed_bitmap,
                        authority);
  if (!authority_result.ok()) {
    return authority_result;
  }
  if (row_ordinals.empty()) {
    return Refuse(CandidateSetOperationKind::build_compressed_bitmap,
                  "SB_CANDIDATE_SET.COMPRESSED_INPUT_CORRUPT",
                  "candidate_set.compressed_input_corrupt",
                  "compressed_bitmap_empty_input",
                  CandidateSetEncoding::compressed_bitmap);
  }
  for (std::size_t i = 0; i < row_ordinals.size(); ++i) {
    if (i != 0 && row_ordinals[i] <= row_ordinals[i - 1]) {
      return Refuse(CandidateSetOperationKind::build_compressed_bitmap,
                    "SB_CANDIDATE_SET.COMPRESSED_INPUT_CORRUPT",
                    "candidate_set.compressed_input_corrupt",
                    row_ordinals[i] == row_ordinals[i - 1]
                        ? "compressed_bitmap_duplicate_row_ordinal"
                        : "compressed_bitmap_unsorted_row_ordinals",
                    CandidateSetEncoding::compressed_bitmap);
    }
  }

  auto set = BuildCompressedBitmapSetFromSortedOrdinals(row_ordinals,
                                                        deleted_overlay_present);
  auto evidence = BaseCompressedBitmapEvidence(
      CandidateSetOperationKind::build_compressed_bitmap,
      set.compressed_bitmap_cardinality);
  evidence.push_back("compressed_bitmap.layout_magic=SBCBM070");
  evidence.push_back("compressed_bitmap.layout_version=1");
  evidence.push_back(CountEvidence("compressed_bitmap.container_count",
                                   set.compressed_bitmap_containers.size()));
  evidence.push_back("compressed_bitmap.deleted_overlay_present=" +
                     std::string(deleted_overlay_present ? "true" : "false"));
  evidence.push_back("compressed_bitmap.materialized_row_expansion=false");
  evidence.push_back("ordinary_inspection.container_level=true");
  for (std::size_t i = 0; i < set.compressed_bitmap_containers.size(); ++i) {
    const auto& container = set.compressed_bitmap_containers[i];
    evidence.push_back("compressed_bitmap.container." + std::to_string(i) +
                       ".type=" +
                       CandidateSetCompressedBitmapContainerTypeName(
                           container.type));
    evidence.push_back("compressed_bitmap.container." + std::to_string(i) +
                       ".cardinality=" +
                       std::to_string(container.cardinality));
  }
  return Finish(CandidateSetOperationKind::build_compressed_bitmap,
                std::move(set), std::move(evidence));
}

CandidateSetSerializedResult SerializeCompressedBitmapCandidateSet(
    const CandidateSet& set) {
  if (!ValidateCompressedBitmapRepresentation(set)) {
    return RefuseSerialized(
        "SB_CANDIDATE_SET.COMPRESSED_FORMAT_INVALID",
        "candidate_set.compressed_format_invalid",
        "invalid_compressed_bitmap_container_representation");
  }
  std::vector<byte> serialized(kCompressedBitmapMagic.begin(),
                               kCompressedBitmapMagic.end());
  Append16(&serialized, kCompressedBitmapFormatVersion);
  Append16(&serialized, CompressedBitmapFlags(set));
  Append32(&serialized,
           static_cast<u32>(set.compressed_bitmap_containers.size()));
  Append64(&serialized, set.compressed_bitmap_cardinality);
  Append64(&serialized, set.compressed_bitmap_min_row_ordinal);
  Append64(&serialized, set.compressed_bitmap_max_row_ordinal);

  for (const auto& container : set.compressed_bitmap_containers) {
    Append16(&serialized, static_cast<u16>(container.type));
    Append16(&serialized, 0);
    Append64(&serialized, container.base_row_ordinal);
    Append32(&serialized, container.ordinal_span);
    Append32(&serialized, container.cardinality);
    switch (container.type) {
      case CandidateSetCompressedBitmapContainerType::array_sparse:
        Append32(&serialized, static_cast<u32>(container.array_offsets.size()));
        for (const auto offset : container.array_offsets) {
          Append16(&serialized, offset);
        }
        break;
      case CandidateSetCompressedBitmapContainerType::run:
        Append32(&serialized, static_cast<u32>(container.runs.size()));
        for (const auto& run : container.runs) {
          Append16(&serialized, run.start_offset);
          Append32(&serialized, run.run_length);
        }
        break;
      case CandidateSetCompressedBitmapContainerType::dense_bitmap:
        Append32(&serialized, static_cast<u32>(container.bitmap_words.size()));
        for (const auto word : container.bitmap_words) {
          Append64(&serialized, word);
        }
        break;
    }
  }

  Append64(&serialized, Fnv1a64(serialized.data(), serialized.size()));
  CandidateSetSerializedResult result;
  result.status = OkStatus();
  result.serialized = std::move(serialized);
  result.evidence = BaseCompressedBitmapEvidence(
      CandidateSetOperationKind::build_compressed_bitmap,
      set.compressed_bitmap_cardinality);
  result.evidence.push_back("compressed_bitmap.serialization=deterministic");
  result.evidence.push_back("compressed_bitmap.layout_magic=SBCBM070");
  result.evidence.push_back("compressed_bitmap.layout_version=1");
  result.evidence.push_back(CountEvidence("compressed_bitmap.serialized_bytes",
                                          result.serialized.size()));
  result.diagnostic = MakeCandidateSetDiagnostic(
      result.status, "SB_CANDIDATE_SET.OK", "candidate_set.ok",
      "serialize_compressed_bitmap");
  return result;
}

CandidateSetResult DeserializeCompressedBitmapCandidateSet(
    const std::vector<byte>& serialized,
    const CandidateSetAuthorityContext& authority) {
  const auto authority_result =
      ValidateAuthority(CandidateSetOperationKind::build_compressed_bitmap,
                        authority);
  if (!authority_result.ok()) {
    return authority_result;
  }
  if (serialized.size() < kCompressedBitmapMagic.size() + 2 + 2 + 4 + 8 + 8 +
                              8 + 8) {
    return Refuse(CandidateSetOperationKind::build_compressed_bitmap,
                  "SB_CANDIDATE_SET.COMPRESSED_FORMAT_CORRUPT",
                  "candidate_set.compressed_format_corrupt",
                  "compressed_bitmap_truncated",
                  CandidateSetEncoding::compressed_bitmap);
  }
  const u64 stored_checksum =
      scratchbird::core::platform::LoadLittle64(serialized.data() +
                                                serialized.size() -
                                                sizeof(u64));
  const u64 computed_checksum =
      Fnv1a64(serialized.data(), serialized.size() - sizeof(u64));
  if (stored_checksum != computed_checksum) {
    return Refuse(CandidateSetOperationKind::build_compressed_bitmap,
                  "SB_CANDIDATE_SET.COMPRESSED_FORMAT_CORRUPT",
                  "candidate_set.compressed_format_corrupt",
                  "compressed_bitmap_checksum_mismatch",
                  CandidateSetEncoding::compressed_bitmap);
  }
  if (!std::equal(kCompressedBitmapMagic.begin(), kCompressedBitmapMagic.end(),
                  serialized.begin())) {
    return Refuse(CandidateSetOperationKind::build_compressed_bitmap,
                  "SB_CANDIDATE_SET.COMPRESSED_FORMAT_CORRUPT",
                  "candidate_set.compressed_format_corrupt",
                  "compressed_bitmap_bad_magic",
                  CandidateSetEncoding::compressed_bitmap);
  }

  std::size_t offset = kCompressedBitmapMagic.size();
  u16 version = 0;
  u16 flags = 0;
  u32 container_count = 0;
  u64 cardinality = 0;
  u64 min_row_ordinal = 0;
  u64 max_row_ordinal = 0;
  if (!Read16(serialized, &offset, &version) ||
      !Read16(serialized, &offset, &flags) ||
      !Read32(serialized, &offset, &container_count) ||
      !Read64(serialized, &offset, &cardinality) ||
      !Read64(serialized, &offset, &min_row_ordinal) ||
      !Read64(serialized, &offset, &max_row_ordinal)) {
    return Refuse(CandidateSetOperationKind::build_compressed_bitmap,
                  "SB_CANDIDATE_SET.COMPRESSED_FORMAT_CORRUPT",
                  "candidate_set.compressed_format_corrupt",
                  "compressed_bitmap_truncated_header",
                  CandidateSetEncoding::compressed_bitmap);
  }
  if (version != kCompressedBitmapFormatVersion) {
    return Refuse(CandidateSetOperationKind::build_compressed_bitmap,
                  "SB_CANDIDATE_SET.COMPRESSED_FORMAT_CORRUPT",
                  "candidate_set.compressed_format_corrupt",
                  "compressed_bitmap_incompatible_version",
                  CandidateSetEncoding::compressed_bitmap);
  }
  const u16 required_flags = kFlagRequiresExactRecheck |
                             kFlagRequiresMgaRecheck |
                             kFlagRequiresSecurityRecheck |
                             kFlagNonAuthorityEvidence;
  if ((flags & required_flags) != required_flags) {
    return Refuse(CandidateSetOperationKind::build_compressed_bitmap,
                  "SB_CANDIDATE_SET.RECHECK_CONTRACT_REQUIRED",
                  "candidate_set.recheck_contract_required",
                  "compressed_bitmap_missing_recheck_or_non_authority_contract",
                  CandidateSetEncoding::compressed_bitmap);
  }
  if (container_count == 0 || cardinality == 0 ||
      min_row_ordinal > max_row_ordinal) {
    return Refuse(CandidateSetOperationKind::build_compressed_bitmap,
                  "SB_CANDIDATE_SET.COMPRESSED_FORMAT_CORRUPT",
                  "candidate_set.compressed_format_corrupt",
                  "compressed_bitmap_invalid_header_range",
                  CandidateSetEncoding::compressed_bitmap);
  }

  CandidateSet set;
  set.encoding = CandidateSetEncoding::compressed_bitmap;
  set.compressed = true;
  set.approximate = true;
  set.requires_exact_recheck = true;
  set.requires_mga_visibility_recheck = true;
  set.requires_security_authorization_recheck = true;
  set.deleted_overlay_present = (flags & kFlagDeletedOverlayPresent) != 0;
  set.non_authority_evidence_present = true;
  set.compressed_bitmap_cardinality = cardinality;
  set.compressed_bitmap_row_ordinal_range_present = true;
  set.compressed_bitmap_min_row_ordinal = min_row_ordinal;
  set.compressed_bitmap_max_row_ordinal = max_row_ordinal;

  u64 computed_cardinality = 0;
  u64 previous_base = 0;
  for (u32 i = 0; i < container_count; ++i) {
    u16 raw_type = 0;
    u16 reserved = 0;
    CandidateSetCompressedBitmapContainer container;
    if (!Read16(serialized, &offset, &raw_type) ||
        !Read16(serialized, &offset, &reserved) ||
        !Read64(serialized, &offset, &container.base_row_ordinal) ||
        !Read32(serialized, &offset, &container.ordinal_span) ||
        !Read32(serialized, &offset, &container.cardinality)) {
      return Refuse(CandidateSetOperationKind::build_compressed_bitmap,
                    "SB_CANDIDATE_SET.COMPRESSED_FORMAT_CORRUPT",
                    "candidate_set.compressed_format_corrupt",
                    "compressed_bitmap_truncated_container",
                    CandidateSetEncoding::compressed_bitmap);
    }
    if (reserved != 0 || container.cardinality == 0 ||
        container.ordinal_span == 0 ||
        container.ordinal_span > kCompressedBitmapContainerOrdinalSpan ||
        container.base_row_ordinal % kCompressedBitmapContainerOrdinalSpan != 0 ||
        (i != 0 && container.base_row_ordinal <= previous_base)) {
      return Refuse(CandidateSetOperationKind::build_compressed_bitmap,
                    "SB_CANDIDATE_SET.COMPRESSED_FORMAT_CORRUPT",
                    "candidate_set.compressed_format_corrupt",
                    "compressed_bitmap_invalid_container_header",
                    CandidateSetEncoding::compressed_bitmap);
    }
    previous_base = container.base_row_ordinal;
    container.type =
        static_cast<CandidateSetCompressedBitmapContainerType>(raw_type);
    u32 payload_count = 0;
    if (!Read32(serialized, &offset, &payload_count)) {
      return Refuse(CandidateSetOperationKind::build_compressed_bitmap,
                    "SB_CANDIDATE_SET.COMPRESSED_FORMAT_CORRUPT",
                    "candidate_set.compressed_format_corrupt",
                    "compressed_bitmap_truncated_payload_count",
                    CandidateSetEncoding::compressed_bitmap);
    }
    u64 container_cardinality = 0;
    switch (container.type) {
      case CandidateSetCompressedBitmapContainerType::array_sparse:
        for (u32 j = 0; j < payload_count; ++j) {
          u16 value = 0;
          if (!Read16(serialized, &offset, &value) ||
              (!container.array_offsets.empty() &&
               value <= container.array_offsets.back())) {
            return Refuse(CandidateSetOperationKind::build_compressed_bitmap,
                          "SB_CANDIDATE_SET.COMPRESSED_FORMAT_CORRUPT",
                          "candidate_set.compressed_format_corrupt",
                          "compressed_bitmap_invalid_sparse_payload",
                          CandidateSetEncoding::compressed_bitmap);
          }
          container.array_offsets.push_back(value);
        }
        container_cardinality = container.array_offsets.size();
        break;
      case CandidateSetCompressedBitmapContainerType::run:
        for (u32 j = 0; j < payload_count; ++j) {
          CandidateSetCompressedBitmapRun run;
          if (!Read16(serialized, &offset, &run.start_offset) ||
              !Read32(serialized, &offset, &run.run_length) ||
              run.run_length == 0 ||
              run.start_offset + run.run_length >
                  kCompressedBitmapContainerOrdinalSpan) {
            return Refuse(CandidateSetOperationKind::build_compressed_bitmap,
                          "SB_CANDIDATE_SET.COMPRESSED_FORMAT_CORRUPT",
                          "candidate_set.compressed_format_corrupt",
                          "compressed_bitmap_invalid_run_payload",
                          CandidateSetEncoding::compressed_bitmap);
          }
          if (!container.runs.empty()) {
            const auto& previous = container.runs.back();
            if (run.start_offset <
                previous.start_offset + previous.run_length) {
              return Refuse(CandidateSetOperationKind::build_compressed_bitmap,
                            "SB_CANDIDATE_SET.COMPRESSED_FORMAT_CORRUPT",
                            "candidate_set.compressed_format_corrupt",
                            "compressed_bitmap_overlapping_run_payload",
                            CandidateSetEncoding::compressed_bitmap);
            }
          }
          container_cardinality += run.run_length;
          container.runs.push_back(run);
        }
        break;
      case CandidateSetCompressedBitmapContainerType::dense_bitmap:
        if (payload_count != kDenseBitmapWordCount) {
          return Refuse(CandidateSetOperationKind::build_compressed_bitmap,
                        "SB_CANDIDATE_SET.COMPRESSED_FORMAT_CORRUPT",
                        "candidate_set.compressed_format_corrupt",
                        "compressed_bitmap_invalid_dense_word_count",
                        CandidateSetEncoding::compressed_bitmap);
        }
        for (u32 j = 0; j < payload_count; ++j) {
          u64 word = 0;
          if (!Read64(serialized, &offset, &word)) {
            return Refuse(CandidateSetOperationKind::build_compressed_bitmap,
                          "SB_CANDIDATE_SET.COMPRESSED_FORMAT_CORRUPT",
                          "candidate_set.compressed_format_corrupt",
                          "compressed_bitmap_truncated_dense_payload",
                          CandidateSetEncoding::compressed_bitmap);
          }
          container.bitmap_words.push_back(word);
        }
        container_cardinality = CountDenseCardinality(container.bitmap_words);
        break;
      default:
        return Refuse(CandidateSetOperationKind::build_compressed_bitmap,
                      "SB_CANDIDATE_SET.COMPRESSED_FORMAT_CORRUPT",
                      "candidate_set.compressed_format_corrupt",
                      "compressed_bitmap_unknown_container_type",
                      CandidateSetEncoding::compressed_bitmap);
    }
    if (container_cardinality != container.cardinality ||
        AddOverflows(computed_cardinality, container_cardinality)) {
      return Refuse(CandidateSetOperationKind::build_compressed_bitmap,
                    "SB_CANDIDATE_SET.COMPRESSED_FORMAT_CORRUPT",
                    "candidate_set.compressed_format_corrupt",
                    "compressed_bitmap_cardinality_mismatch",
                    CandidateSetEncoding::compressed_bitmap);
    }
    computed_cardinality += container_cardinality;
    set.compressed_bitmap_containers.push_back(std::move(container));
  }
  if (offset != serialized.size() - sizeof(u64) ||
      computed_cardinality != cardinality ||
      !ValidateCompressedBitmapRepresentation(set)) {
    return Refuse(CandidateSetOperationKind::build_compressed_bitmap,
                  "SB_CANDIDATE_SET.COMPRESSED_FORMAT_CORRUPT",
                  "candidate_set.compressed_format_corrupt",
                  "compressed_bitmap_layout_validation_failed",
                  CandidateSetEncoding::compressed_bitmap);
  }

  auto evidence = BaseCompressedBitmapEvidence(
      CandidateSetOperationKind::build_compressed_bitmap,
      set.compressed_bitmap_cardinality);
  evidence.push_back("compressed_bitmap.deserialization=accepted");
  evidence.push_back("compressed_bitmap.layout_magic=SBCBM070");
  evidence.push_back("compressed_bitmap.layout_version=1");
  evidence.push_back("compressed_bitmap.materialized_row_expansion=false");
  evidence.push_back(CountEvidence("compressed_bitmap.container_count",
                                   set.compressed_bitmap_containers.size()));
  return Finish(CandidateSetOperationKind::build_compressed_bitmap,
                std::move(set), std::move(evidence));
}

std::vector<std::string> InspectCompressedBitmapCandidateSet(
    const CandidateSet& set) {
  if (!ValidateCompressedBitmapRepresentation(set)) {
    return {"compressed_bitmap.inspection=refused",
            "compressed_bitmap.format_valid=false",
            "fail_closed=true"};
  }
  std::vector<std::string> evidence = BaseCompressedBitmapEvidence(
      CandidateSetOperationKind::build_compressed_bitmap,
      set.compressed_bitmap_cardinality);
  evidence.push_back("compressed_bitmap.inspection=container_level");
  evidence.push_back("compressed_bitmap.materialized_row_expansion=false");
  evidence.push_back(CountEvidence("compressed_bitmap.container_count",
                                   set.compressed_bitmap_containers.size()));
  evidence.push_back("compressed_bitmap.row_ordinal_min=" +
                     std::to_string(set.compressed_bitmap_min_row_ordinal));
  evidence.push_back("compressed_bitmap.row_ordinal_max=" +
                     std::to_string(set.compressed_bitmap_max_row_ordinal));
  evidence.push_back("compressed_bitmap.deleted_overlay_present=" +
                     std::string(set.deleted_overlay_present ? "true"
                                                             : "false"));
  for (std::size_t i = 0; i < set.compressed_bitmap_containers.size(); ++i) {
    const auto& container = set.compressed_bitmap_containers[i];
    evidence.push_back("compressed_bitmap.container." + std::to_string(i) +
                       ".type=" +
                       CandidateSetCompressedBitmapContainerTypeName(
                           container.type));
    evidence.push_back("compressed_bitmap.container." + std::to_string(i) +
                       ".base_row_ordinal=" +
                       std::to_string(container.base_row_ordinal));
    evidence.push_back("compressed_bitmap.container." + std::to_string(i) +
                       ".cardinality=" +
                       std::to_string(container.cardinality));
  }
  return evidence;
}

std::vector<CandidateSetRow> MaterializeCandidateSetRowsForCompatibilityBridge(
    const CandidateSet& set) {
  return MaterializedRowsForCompatibilityBridge(set);
}

CandidateSetResult UnionCandidateSets(const CandidateSet& left,
                                      const CandidateSet& right,
                                      const CandidateSetAuthorityContext& authority) {
  const auto authority_result =
      ValidateAuthority(CandidateSetOperationKind::union_sets, authority);
  if (!authority_result.ok()) {
    return authority_result;
  }
  const auto left_check = ValidateInputSet(CandidateSetOperationKind::union_sets, left);
  if (!left_check.ok()) {
    return left_check;
  }
  const auto right_check = ValidateInputSet(CandidateSetOperationKind::union_sets, right);
  if (!right_check.ok()) {
    return right_check;
  }
  if (BothCompressedBitmaps(left, right)) {
    return CompressedBitmapBinaryAlgebra(CandidateSetOperationKind::union_sets,
                                         left, right);
  }
  if (left.encoding == CandidateSetEncoding::compressed_bitmap ||
      right.encoding == CandidateSetEncoding::compressed_bitmap) {
    if (!MixedCompressedExactCanUseMaterializedRows(left, right)) {
      return RefuseMixedCompressedExact(CandidateSetOperationKind::union_sets);
    }
  }

  auto rows = MaterializedRowsForCompatibilityBridge(left);
  auto right_rows = MaterializedRowsForCompatibilityBridge(right);
  rows.insert(rows.end(), right_rows.begin(), right_rows.end());
  auto set = NormalizeRows(std::move(rows),
                           CandidateSetEncoding::exact_row_uuid_ordered_stream);
  set.approximate = left.approximate || right.approximate;
  auto evidence = BaseEvidence(CandidateSetOperationKind::union_sets,
                               set.encoding, set.rows.size());
  evidence.push_back(CountEvidence("row_count.left", left.rows.size()));
  evidence.push_back(CountEvidence("row_count.right", right.rows.size()));
  evidence.push_back("candidate_stream_only=true");
  if (left.encoding == CandidateSetEncoding::compressed_bitmap ||
      right.encoding == CandidateSetEncoding::compressed_bitmap) {
    evidence.push_back("mixed_compressed_exact.materialized_row_bridge=true");
  }
  return Finish(CandidateSetOperationKind::union_sets, std::move(set),
                std::move(evidence));
}

CandidateSetResult IntersectCandidateSets(const CandidateSet& left,
                                          const CandidateSet& right,
                                          const CandidateSetAuthorityContext& authority) {
  const auto authority_result =
      ValidateAuthority(CandidateSetOperationKind::intersect_sets, authority);
  if (!authority_result.ok()) {
    return authority_result;
  }
  const auto left_check = ValidateInputSet(CandidateSetOperationKind::intersect_sets, left);
  if (!left_check.ok()) {
    return left_check;
  }
  const auto right_check = ValidateInputSet(CandidateSetOperationKind::intersect_sets, right);
  if (!right_check.ok()) {
    return right_check;
  }
  if (BothCompressedBitmaps(left, right)) {
    return CompressedBitmapBinaryAlgebra(
        CandidateSetOperationKind::intersect_sets, left, right);
  }
  if (left.encoding == CandidateSetEncoding::compressed_bitmap ||
      right.encoding == CandidateSetEncoding::compressed_bitmap) {
    if (!MixedCompressedExactCanUseMaterializedRows(left, right)) {
      return RefuseMixedCompressedExact(
          CandidateSetOperationKind::intersect_sets);
    }
  }

  std::map<RowKey, CandidateSetRow> right_map;
  const auto right_rows = MaterializedRowsForCompatibilityBridge(right);
  for (const auto& row : right_rows) {
    right_map.emplace(RowKey{row.row_uuid}, row);
  }
  std::vector<CandidateSetRow> rows;
  const auto left_rows = MaterializedRowsForCompatibilityBridge(left);
  for (const auto& left_row : left_rows) {
    const auto found = right_map.find(RowKey{left_row.row_uuid});
    if (found == right_map.end()) {
      continue;
    }
    auto row = left_row;
    row.score = std::max(row.score, found->second.score);
    row.exact_predicate_match =
        row.exact_predicate_match && found->second.exact_predicate_match;
    row.mga_visible = row.mga_visible && found->second.mga_visible;
    row.security_authorized =
        row.security_authorized && found->second.security_authorized;
    row.exact_payload_available =
        row.exact_payload_available && found->second.exact_payload_available;
    rows.push_back(std::move(row));
  }
  auto set = NormalizeRows(std::move(rows),
                           CandidateSetEncoding::exact_row_uuid_ordered_stream);
  set.approximate = left.approximate || right.approximate;
  auto evidence = BaseEvidence(CandidateSetOperationKind::intersect_sets,
                               set.encoding, set.rows.size());
  evidence.push_back(CountEvidence("row_count.left", left_rows.size()));
  evidence.push_back(CountEvidence("row_count.right", right_rows.size()));
  evidence.push_back("candidate_stream_only=true");
  if (left.encoding == CandidateSetEncoding::compressed_bitmap ||
      right.encoding == CandidateSetEncoding::compressed_bitmap) {
    evidence.push_back("mixed_compressed_exact.materialized_row_bridge=true");
  }
  return Finish(CandidateSetOperationKind::intersect_sets, std::move(set),
                std::move(evidence));
}

CandidateSetResult SubtractCandidateSets(const CandidateSet& left,
                                         const CandidateSet& right,
                                         const CandidateSetAuthorityContext& authority) {
  const auto authority_result =
      ValidateAuthority(CandidateSetOperationKind::subtract_sets, authority);
  if (!authority_result.ok()) {
    return authority_result;
  }
  const auto left_check = ValidateInputSet(CandidateSetOperationKind::subtract_sets, left);
  if (!left_check.ok()) {
    return left_check;
  }
  const auto right_check = ValidateInputSet(CandidateSetOperationKind::subtract_sets, right);
  if (!right_check.ok()) {
    return right_check;
  }
  if (BothCompressedBitmaps(left, right)) {
    return CompressedBitmapBinaryAlgebra(
        CandidateSetOperationKind::subtract_sets, left, right);
  }
  if (left.encoding == CandidateSetEncoding::compressed_bitmap ||
      right.encoding == CandidateSetEncoding::compressed_bitmap) {
    if (!MixedCompressedExactCanUseMaterializedRows(left, right)) {
      return RefuseMixedCompressedExact(
          CandidateSetOperationKind::subtract_sets);
    }
  }

  std::map<RowKey, bool> right_map;
  const auto right_rows = MaterializedRowsForCompatibilityBridge(right);
  for (const auto& row : right_rows) {
    right_map.emplace(RowKey{row.row_uuid}, true);
  }
  std::vector<CandidateSetRow> rows;
  const auto left_rows = MaterializedRowsForCompatibilityBridge(left);
  for (const auto& row : left_rows) {
    if (right_map.find(RowKey{row.row_uuid}) == right_map.end()) {
      rows.push_back(row);
    }
  }
  auto set = NormalizeRows(std::move(rows),
                           CandidateSetEncoding::exact_row_uuid_ordered_stream);
  set.approximate = left.approximate;
  auto evidence = BaseEvidence(CandidateSetOperationKind::subtract_sets,
                               set.encoding, set.rows.size());
  evidence.push_back(CountEvidence("row_count.left", left_rows.size()));
  evidence.push_back(CountEvidence("row_count.right", right_rows.size()));
  evidence.push_back("candidate_stream_only=true");
  if (left.encoding == CandidateSetEncoding::compressed_bitmap ||
      right.encoding == CandidateSetEncoding::compressed_bitmap) {
    evidence.push_back("mixed_compressed_exact.materialized_row_bridge=true");
  }
  return Finish(CandidateSetOperationKind::subtract_sets, std::move(set),
                std::move(evidence));
}

CandidateSetResult ComplementCandidateSetWithinUniverse(
    const CandidateSet& input,
    u64 universe_min_row_ordinal,
    u64 universe_max_row_ordinal,
    const CandidateSetAuthorityContext& authority) {
  const auto authority_result =
      ValidateAuthority(CandidateSetOperationKind::complement, authority);
  if (!authority_result.ok()) {
    return authority_result;
  }
  const auto input_check =
      ValidateInputSet(CandidateSetOperationKind::complement, input);
  if (!input_check.ok()) {
    return input_check;
  }
  if (input.encoding != CandidateSetEncoding::compressed_bitmap) {
    return Refuse(CandidateSetOperationKind::complement,
                  "SB_CANDIDATE_SET.UNSUPPORTED_ENCODING",
                  "candidate_set.unsupported_encoding",
                  "compressed_bitmap_complement_required", input.encoding);
  }
  if (universe_min_row_ordinal > universe_max_row_ordinal) {
    return Refuse(CandidateSetOperationKind::complement,
                  "SB_CANDIDATE_SET.INVALID_UNIVERSE",
                  "candidate_set.invalid_universe",
                  "compressed_bitmap_invalid_universe",
                  CandidateSetEncoding::compressed_bitmap);
  }
  if (input.compressed_bitmap_cardinality != 0 &&
      (universe_min_row_ordinal > input.compressed_bitmap_min_row_ordinal ||
       universe_max_row_ordinal < input.compressed_bitmap_max_row_ordinal)) {
    return Refuse(CandidateSetOperationKind::complement,
                  "SB_CANDIDATE_SET.INVALID_UNIVERSE",
                  "candidate_set.invalid_universe",
                  "compressed_bitmap_universe_does_not_cover_input",
                  CandidateSetEncoding::compressed_bitmap);
  }

  const u64 first_base =
      (universe_min_row_ordinal / kCompressedBitmapContainerOrdinalSpan) *
      kCompressedBitmapContainerOrdinalSpan;
  const u64 last_base =
      (universe_max_row_ordinal / kCompressedBitmapContainerOrdinalSpan) *
      kCompressedBitmapContainerOrdinalSpan;

  std::vector<CandidateSetCompressedBitmapContainer> containers;
  std::size_t input_index = 0;
  for (u64 base = first_base;; base += kCompressedBitmapContainerOrdinalSpan) {
    std::vector<u64> words(kDenseBitmapWordCount,
                           std::numeric_limits<u64>::max());
    KeepOnlyUniverseRange(&words, base, universe_min_row_ordinal,
                          universe_max_row_ordinal);
    while (input_index < input.compressed_bitmap_containers.size() &&
           input.compressed_bitmap_containers[input_index].base_row_ordinal <
               base) {
      ++input_index;
    }
    if (input_index < input.compressed_bitmap_containers.size() &&
        input.compressed_bitmap_containers[input_index].base_row_ordinal == base) {
      auto input_words =
          DenseWordsFromContainer(input.compressed_bitmap_containers[input_index]);
      for (std::size_t i = 0; i < words.size(); ++i) {
        words[i] &= ~input_words[i];
      }
    }
    if (auto container =
            BuildContainerFromWords(base,
                                    static_cast<u32>(
                                        kCompressedBitmapContainerOrdinalSpan),
                                    std::move(words))) {
      containers.push_back(std::move(*container));
    }
    if (base == last_base ||
        base > std::numeric_limits<u64>::max() -
                   kCompressedBitmapContainerOrdinalSpan) {
      break;
    }
  }

  auto set = BuildCompressedBitmapSetFromContainers(
      std::move(containers), input.deleted_overlay_present);
  auto evidence = CompressedBitmapAlgebraEvidence(
      CandidateSetOperationKind::complement, set,
      input.compressed_bitmap_containers.size(), "not_complement");
  evidence.push_back("compressed_bitmap.universe_min=" +
                     std::to_string(universe_min_row_ordinal));
  evidence.push_back("compressed_bitmap.universe_max=" +
                     std::to_string(universe_max_row_ordinal));
  if (input.deleted_overlay_present) {
    evidence.push_back(
        "compressed_bitmap.deleted_overlay_algebra=preserved_for_exact_recheck");
  }
  return Finish(CandidateSetOperationKind::complement, std::move(set),
                std::move(evidence));
}

CandidateSetResult TopKCandidateSet(const CandidateSet& input,
                                    u64 k,
                                    const CandidateSetAuthorityContext& authority) {
  const auto authority_result =
      ValidateAuthority(CandidateSetOperationKind::top_k, authority);
  if (!authority_result.ok()) {
    return authority_result;
  }
  const auto input_check = ValidateInputSet(CandidateSetOperationKind::top_k, input);
  if (!input_check.ok()) {
    return input_check;
  }
  if (k == 0) {
    return Refuse(CandidateSetOperationKind::top_k,
                  "SB_CANDIDATE_SET.TOP_K_REQUIRED",
                  "candidate_set.top_k_required", "top_k_zero", input.encoding);
  }
  if (input.encoding == CandidateSetEncoding::compressed_bitmap) {
    return CompressedBitmapFirstK(input, k);
  }

  auto rows = MaterializedRowsForCompatibilityBridge(input);
  std::stable_sort(rows.begin(), rows.end(),
                   [](const CandidateSetRow& left, const CandidateSetRow& right) {
                     if (left.score != right.score) {
                       return left.score > right.score;
                     }
                     return RowUuidLess(left, right);
                   });
  if (rows.size() > k) {
    rows.resize(static_cast<std::size_t>(k));
  }
  std::sort(rows.begin(), rows.end(), RowUuidLess);
  auto set = NormalizeRows(std::move(rows),
                           CandidateSetEncoding::exact_row_uuid_ordered_stream);
  set.approximate = input.approximate;
  auto evidence = BaseEvidence(CandidateSetOperationKind::top_k, set.encoding,
                               set.rows.size());
  evidence.push_back("top_k.action=score_prune");
  evidence.push_back("top_k.k=" + std::to_string(k));
  evidence.push_back(CountEvidence("row_count.input", input.rows.size()));
  evidence.push_back("candidate_stream_only=true");
  return Finish(CandidateSetOperationKind::top_k, std::move(set),
                std::move(evidence));
}

CandidateSetCardinalityResult CandidateSetPopcount(
    const CandidateSet& input,
    const CandidateSetAuthorityContext& authority) {
  const auto authority_result =
      ValidateAuthority(CandidateSetOperationKind::popcount, authority);
  if (!authority_result.ok()) {
    return RefuseCardinality(
        CandidateSetOperationKind::popcount,
        authority_result.diagnostic.diagnostic_code,
        authority_result.diagnostic.message_key,
        authority_result.refusal_reasons.empty()
            ? "candidate_set_authority_refused"
            : authority_result.refusal_reasons.front(),
        input.encoding);
  }
  const auto input_check =
      ValidateInputSet(CandidateSetOperationKind::popcount, input);
  if (!input_check.ok()) {
    return RefuseCardinality(
        CandidateSetOperationKind::popcount,
        input_check.diagnostic.diagnostic_code,
        input_check.diagnostic.message_key,
        input_check.refusal_reasons.empty()
            ? "candidate_set_input_refused"
            : input_check.refusal_reasons.front(),
        input.encoding);
  }

  CandidateSetCardinalityResult result;
  result.status = OkStatus();
  result.operation = CandidateSetOperationKind::popcount;
  if (input.encoding == CandidateSetEncoding::compressed_bitmap) {
    u64 cardinality = 0;
    for (const auto& container : input.compressed_bitmap_containers) {
      const auto words = DenseWordsFromContainer(container);
      cardinality += CountWordsCardinality(words);
    }
    result.cardinality = cardinality;
    result.evidence = BaseCompressedBitmapEvidence(
        CandidateSetOperationKind::popcount, cardinality);
    result.evidence.push_back("compressed_bitmap.algebra=container_level");
    result.evidence.push_back("compressed_bitmap.operation_kind=popcount");
    result.evidence.push_back("compressed_bitmap.materialized_row_expansion=false");
    result.evidence.push_back("compatibility_bridge.used=false");
    result.evidence.push_back(CountEvidence(
        "compressed_bitmap.input_container_count",
        input.compressed_bitmap_containers.size()));
    result.evidence.push_back("compressed_bitmap.deleted_overlay_present=" +
                              std::string(input.deleted_overlay_present
                                              ? "true"
                                              : "false"));
  } else {
    result.cardinality = input.rows.size();
    result.evidence = BaseEvidence(CandidateSetOperationKind::popcount,
                                   input.encoding, input.rows.size());
    result.evidence.push_back("compatibility_bridge.used=false");
  }
  result.diagnostic = MakeCandidateSetDiagnostic(
      result.status, "SB_CANDIDATE_SET.OK", "candidate_set.ok", "popcount");
  return result;
}

CandidateSetResult RerankCandidateSet(
    const CandidateSet& input,
    const std::function<double(const CandidateSetRow&)>& scorer,
    const CandidateSetAuthorityContext& authority) {
  const auto authority_result =
      ValidateAuthority(CandidateSetOperationKind::rerank, authority);
  if (!authority_result.ok()) {
    return authority_result;
  }
  if (!authority.exact_rerank_source_available) {
    return Refuse(CandidateSetOperationKind::rerank,
                  "SB_CANDIDATE_SET.EXACT_RERANK_REQUIRED",
                  "candidate_set.exact_rerank_required",
                  "missing_exact_rerank_source", input.encoding);
  }
  const auto input_check = ValidateInputSet(CandidateSetOperationKind::rerank, input);
  if (!input_check.ok()) {
    return input_check;
  }
  if (input.encoding == CandidateSetEncoding::compressed_bitmap) {
    return Refuse(CandidateSetOperationKind::rerank,
                  "SB_CANDIDATE_SET.EXACT_RERANK_REQUIRED",
                  "candidate_set.exact_rerank_required",
                  "compressed_bitmap_rerank_requires_exact_payload_stream",
                  input.encoding);
  }
  if (!scorer) {
    return Refuse(CandidateSetOperationKind::rerank,
                  "SB_CANDIDATE_SET.EXACT_RERANK_REQUIRED",
                  "candidate_set.exact_rerank_required",
                  "missing_exact_rerank_scorer", input.encoding);
  }

  auto rows = MaterializedRowsForCompatibilityBridge(input);
  for (auto& row : rows) {
    if (!row.exact_payload_available) {
      return Refuse(CandidateSetOperationKind::rerank,
                    "SB_CANDIDATE_SET.EXACT_RERANK_REQUIRED",
                    "candidate_set.exact_rerank_required",
                    "missing_exact_rerank_payload", input.encoding);
    }
    row.score = scorer(row);
  }
  std::sort(rows.begin(), rows.end(), RowUuidLess);
  auto set = NormalizeRows(std::move(rows),
                           CandidateSetEncoding::exact_row_uuid_ordered_stream);
  set.approximate = input.approximate;
  auto evidence = BaseEvidence(CandidateSetOperationKind::rerank, set.encoding,
                               set.rows.size());
  evidence.push_back("rerank.action=exact_payload_score");
  evidence.push_back(CountEvidence("row_count.input", input.rows.size()));
  evidence.push_back("candidate_stream_only=true");
  return Finish(CandidateSetOperationKind::rerank, std::move(set),
                std::move(evidence));
}

CandidateSetResult ExactRecheckCandidateSet(
    const CandidateSet& input,
    const CandidateSetAuthorityContext& authority) {
  const auto authority_result =
      ValidateAuthority(CandidateSetOperationKind::exact_recheck, authority);
  if (!authority_result.ok()) {
    return authority_result;
  }
  const auto input_check =
      ValidateInputSet(CandidateSetOperationKind::exact_recheck, input);
  if (!input_check.ok()) {
    return input_check;
  }
  if (input.encoding == CandidateSetEncoding::compressed_bitmap) {
    return Refuse(CandidateSetOperationKind::exact_recheck,
                  "SB_CANDIDATE_SET.EXACT_ROW_UUID_REQUIRED",
                  "candidate_set.exact_row_uuid_required",
                  "compressed_bitmap_exact_recheck_requires_row_locator_stream",
                  input.encoding);
  }

  std::vector<CandidateSetRow> rows;
  for (const auto& row : input.rows) {
    if (!ValidRowUuid(row.row_uuid)) {
      return Refuse(CandidateSetOperationKind::exact_recheck,
                    "SB_CANDIDATE_SET.EXACT_ROW_UUID_REQUIRED",
                    "candidate_set.exact_row_uuid_required",
                    "missing_exact_row_uuid", input.encoding);
    }
    if (row.exact_predicate_match && row.mga_visible &&
        row.security_authorized) {
      rows.push_back(row);
    }
  }
  auto set = NormalizeRows(std::move(rows),
                           CandidateSetEncoding::exact_row_uuid_ordered_stream);
  set.approximate = false;
  set.requires_exact_recheck = false;
  set.requires_mga_visibility_recheck = false;
  set.requires_security_authorization_recheck = false;
  set.final_rows_authorized = true;
  auto evidence = BaseEvidence(CandidateSetOperationKind::exact_recheck,
                               set.encoding, set.rows.size());
  evidence.push_back(CountEvidence("row_count.input", input.rows.size()));
  evidence.push_back("exact_recheck.action=predicate_mga_security");
  evidence.push_back("mga_finality_authority=engine_transaction_inventory");
  evidence.push_back("final_rows_authorized=true");
  return Finish(CandidateSetOperationKind::exact_recheck, std::move(set),
                std::move(evidence));
}

DiagnosticRecord MakeCandidateSetDiagnostic(Status status,
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
  record.source_component = "core.index.candidate_set";
  return record;
}

}  // namespace scratchbird::core::index
