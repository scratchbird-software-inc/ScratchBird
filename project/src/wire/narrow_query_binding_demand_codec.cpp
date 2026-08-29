// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "narrow_query_binding_demand_codec.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <set>
#include <string_view>
#include <tuple>
#include <utility>

namespace scratchbird::wire {
namespace {

using scratchbird::core::platform::LoadLittle16;
using scratchbird::core::platform::LoadLittle32;
using scratchbird::core::platform::LoadLittle64;
using scratchbird::core::platform::StoreLittle16;
using scratchbird::core::platform::StoreLittle32;
using scratchbird::core::platform::StoreLittle64;

constexpr std::array<byte, 8> kMagic{
    {'S', 'B', 'Q', 'N', 'D', 'R', '0', '1'}};
constexpr u32 kRowLimitPresentFlag = 1u << 0u;
constexpr u32 kRowOffsetPresentFlag = 1u << 1u;
constexpr u32 kResultBoundFlags =
    kRowLimitPresentFlag | kRowOffsetPresentFlag;
constexpr u32 kRelationHintPresentFlag = 1u << 0u;
constexpr u32 kExplicitAliasFlag = 1u << 1u;
constexpr u32 kSourceFlags =
    kRelationHintPresentFlag | kExplicitAliasFlag;
constexpr u32 kOutputNamePresentFlag = 1u << 0u;

constexpr const char* kOperandInvalid = "SBLR.OPERAND.INVALID";
constexpr const char* kTransactionStale = "MGA.TRANSACTION.STALE";
constexpr const char* kOrderingInvalid = "SORT.ORDERING_VECTOR_INVALID";
constexpr const char* kProjectionInvalid =
    "PROJECTION.EXPRESSION_VECTOR_INVALID";
constexpr const char* kPlanInvalid = "SBLR.PLAN_TREE.INVALID_HANDLE";
constexpr const char* kResourceExceeded = "RESOURCE.BUDGET_EXCEEDED";

bool Fail(NarrowQueryBindingDemandError* error,
          NarrowQueryBindingDemandErrorCode code,
          std::string diagnostic,
          std::string field,
          u32 record_index,
          std::string detail) {
  if (error != nullptr) {
    error->code = code;
    error->diagnostic_code = std::move(diagnostic);
    error->field = std::move(field);
    error->record_index = record_index;
    error->detail = std::move(detail);
  }
  return false;
}

void ClearError(NarrowQueryBindingDemandError* error) {
  if (error != nullptr) {
    *error = {};
  }
}

bool UuidPresent(const NarrowQueryUuid& value) {
  return std::any_of(value.begin(), value.end(),
                     [](byte octet) { return octet != 0; });
}

bool ValidProfile(NarrowQueryProfile profile) {
  switch (profile) {
    case NarrowQueryProfile::ordered_projection:
    case NarrowQueryProfile::projection_occurrence:
    case NarrowQueryProfile::alias_distinct_self_join:
      return true;
  }
  return false;
}

bool ValidDirection(NarrowQueryDirection direction) {
  return direction == NarrowQueryDirection::ascending ||
         direction == NarrowQueryDirection::descending;
}

bool ValidNullPlacement(NarrowQueryNullPlacement placement) {
  return placement == NarrowQueryNullPlacement::first ||
         placement == NarrowQueryNullPlacement::last;
}

// This validates the canonical byte spelling admitted by this carrier:
// shortest-form UTF-8 scalar values and no embedded NUL.  Identifier
// case-folding, catalog resolution, and normalization authority remain outside
// this syntax-only carrier.
bool ValidCanonicalUtf8Spelling(std::string_view value) {
  if (value.empty()) {
    return false;
  }
  std::size_t offset = 0;
  while (offset < value.size()) {
    const auto first = static_cast<unsigned char>(value[offset]);
    if (first == 0) {
      return false;
    }
    if (first <= 0x7fu) {
      ++offset;
      continue;
    }
    std::size_t continuation_count = 0;
    std::uint32_t code_point = 0;
    if (first >= 0xc2u && first <= 0xdfu) {
      continuation_count = 1;
      code_point = first & 0x1fu;
    } else if (first >= 0xe0u && first <= 0xefu) {
      continuation_count = 2;
      code_point = first & 0x0fu;
    } else if (first >= 0xf0u && first <= 0xf4u) {
      continuation_count = 3;
      code_point = first & 0x07u;
    } else {
      return false;
    }
    if (continuation_count > value.size() - offset - 1u) {
      return false;
    }
    for (std::size_t index = 0; index < continuation_count; ++index) {
      const auto next =
          static_cast<unsigned char>(value[offset + index + 1u]);
      if ((next & 0xc0u) != 0x80u) {
        return false;
      }
      code_point = (code_point << 6u) | (next & 0x3fu);
    }
    if ((continuation_count == 2u && code_point < 0x800u) ||
        (continuation_count == 3u && code_point < 0x10000u) ||
        (code_point >= 0xd800u && code_point <= 0xdfffu) ||
        code_point > 0x10ffffu) {
      return false;
    }
    offset += continuation_count + 1u;
  }
  return true;
}

bool AddWithin(u64 left, u64 right, u64 maximum, u64* result) {
  if (right > maximum || left > maximum - right) {
    return false;
  }
  *result = left + right;
  return true;
}

bool ValidMgaRelationDecodedBytesPerPass(u64 value) {
  return value >= kNarrowQueryMinimumMgaRelationDecodedBytesPerPass &&
         value <= kNarrowQueryMaximumMgaRelationDecodedBytesPerPass;
}

void StoreUuid(std::vector<byte>* bytes,
               std::size_t offset,
               const NarrowQueryUuid& value) {
  std::copy(value.begin(), value.end(), bytes->begin() + offset);
}

NarrowQueryUuid LoadUuid(std::span<const byte> bytes, std::size_t offset) {
  NarrowQueryUuid value{};
  std::copy_n(bytes.begin() + offset, value.size(), value.begin());
  return value;
}

void AppendString(std::vector<byte>* bytes, std::string_view value) {
  bytes->insert(bytes->end(), value.begin(), value.end());
}

std::string_view StringAt(std::span<const byte> bytes,
                          std::size_t offset,
                          std::size_t length) {
  return std::string_view(
      reinterpret_cast<const char*>(bytes.data() + offset), length);
}

bool ValidateCounts(const NarrowQueryBindingDemand& demand,
                    NarrowQueryBindingDemandError* error) {
  if (demand.sources.empty() ||
      demand.sources.size() > kNarrowQueryBindingDemandMaximumSources ||
      demand.outputs.empty() ||
      demand.outputs.size() > kNarrowQueryBindingDemandMaximumOutputs ||
      demand.ordering_terms.size() >
          kNarrowQueryBindingDemandMaximumOrderingTerms) {
    return Fail(error, NarrowQueryBindingDemandErrorCode::count_invalid,
                kOperandInvalid, "vector_counts", 0,
                "source, output, or ordering count is outside the admitted "
                "bounds");
  }
  return true;
}

bool ComputeEncodedExtent(const NarrowQueryBindingDemand& demand,
                          u64* total_bytes,
                          u32* source_bytes,
                          u32* output_bytes,
                          u32* ordering_bytes,
                          NarrowQueryBindingDemandError* error) {
  if (!ValidateCounts(demand, error)) {
    return false;
  }

  u64 source_extent = 0;
  for (const auto& source : demand.sources) {
    u64 record_bytes = kNarrowQueryBindingDemandSourcePrefixBytes;
    if (!AddWithin(record_bytes, source.alias_spelling.size(),
                   kNarrowQueryBindingDemandMaximumCarrierBytes,
                   &record_bytes) ||
        !AddWithin(source_extent, record_bytes,
                   kNarrowQueryBindingDemandMaximumCarrierBytes,
                   &source_extent)) {
      return Fail(error,
                  NarrowQueryBindingDemandErrorCode::resource_limit_exceeded,
                  kResourceExceeded, "source_record_extent",
                  source.source_ordinal,
                  "source vector exceeds the 16 MiB carrier ceiling");
    }
  }

  u64 output_extent = 0;
  for (const auto& output : demand.outputs) {
    u64 record_bytes = kNarrowQueryBindingDemandOutputPrefixBytes;
    if (!AddWithin(record_bytes, output.source_column_spelling.size(),
                   kNarrowQueryBindingDemandMaximumCarrierBytes,
                   &record_bytes) ||
        !AddWithin(record_bytes, output.output_name_spelling.size(),
                   kNarrowQueryBindingDemandMaximumCarrierBytes,
                   &record_bytes) ||
        !AddWithin(output_extent, record_bytes,
                   kNarrowQueryBindingDemandMaximumCarrierBytes,
                   &output_extent)) {
      return Fail(error,
                  NarrowQueryBindingDemandErrorCode::resource_limit_exceeded,
                  kResourceExceeded, "output_record_extent",
                  output.output_ordinal,
                  "output vector exceeds the 16 MiB carrier ceiling");
    }
  }

  u64 ordering_extent = 0;
  for (const auto& term : demand.ordering_terms) {
    u64 record_bytes = kNarrowQueryBindingDemandOrderingPrefixBytes;
    if (!AddWithin(record_bytes, term.source_column_spelling.size(),
                   kNarrowQueryBindingDemandMaximumCarrierBytes,
                   &record_bytes) ||
        !AddWithin(ordering_extent, record_bytes,
                   kNarrowQueryBindingDemandMaximumCarrierBytes,
                   &ordering_extent)) {
      return Fail(error,
                  NarrowQueryBindingDemandErrorCode::resource_limit_exceeded,
                  kResourceExceeded, "ordering_record_extent",
                  term.term_ordinal,
                  "ordering vector exceeds the 16 MiB carrier ceiling");
    }
  }

  // The engine-issued scan-byte ceiling is a mandatory fixed-position extent
  // immediately after the 72-byte header.  It remains outside the header so
  // the established header offsets do not move.
  u64 total = kNarrowQueryBindingDemandHeaderBytes + sizeof(u64);
  if ((demand.row_limit_present || demand.row_offset_present) &&
      !AddWithin(total, kNarrowQueryBindingDemandResultBoundBytes,
                 kNarrowQueryBindingDemandMaximumCarrierBytes, &total)) {
    return Fail(error,
                NarrowQueryBindingDemandErrorCode::resource_limit_exceeded,
                kResourceExceeded, "post_projection_result_bound", 0,
                "result-bound extent exceeds the carrier ceiling");
  }
  if (!AddWithin(total, source_extent,
                 kNarrowQueryBindingDemandMaximumCarrierBytes, &total) ||
      !AddWithin(total, output_extent,
                 kNarrowQueryBindingDemandMaximumCarrierBytes, &total) ||
      !AddWithin(total, ordering_extent,
                 kNarrowQueryBindingDemandMaximumCarrierBytes, &total)) {
    return Fail(error,
                NarrowQueryBindingDemandErrorCode::resource_limit_exceeded,
                kResourceExceeded, "carrier_extent", 0,
                "carrier exceeds the 16 MiB ceiling");
  }

  *total_bytes = total;
  *source_bytes = static_cast<u32>(source_extent);
  *output_bytes = static_cast<u32>(output_extent);
  *ordering_bytes = static_cast<u32>(ordering_extent);
  return true;
}

using SourceSpellingKey = std::pair<u32, std::string_view>;

bool ValidateDemandStructure(const NarrowQueryBindingDemand& demand,
                             NarrowQueryBindingDemandError* error) {
  if (!UuidPresent(demand.statement_receipt_uuid)) {
    return Fail(error, NarrowQueryBindingDemandErrorCode::receipt_invalid,
                kTransactionStale, "statement_receipt_uuid", 0,
                "authenticated statement receipt UUID is zero");
  }
  if (!ValidProfile(demand.requested_profile)) {
    return Fail(error, NarrowQueryBindingDemandErrorCode::profile_invalid,
                kOperandInvalid, "requested_profile", 0,
                "requested narrow-query profile code is unknown");
  }
  if (!ValidMgaRelationDecodedBytesPerPass(
          demand.maximum_mga_relation_decoded_bytes_per_pass)) {
    return Fail(error,
                NarrowQueryBindingDemandErrorCode::resource_limit_exceeded,
                kResourceExceeded,
                "maximum_mga_relation_decoded_bytes_per_pass", 0,
                "engine-issued MGA relation decoded-byte ceiling is outside "
                "the inclusive 64 KiB through 1 TiB range");
  }
  if (!ValidateCounts(demand, error)) {
    return false;
  }
  if ((!demand.row_limit_present && demand.row_limit != 0) ||
      (!demand.row_offset_present && demand.row_offset != 0)) {
    return Fail(error,
                NarrowQueryBindingDemandErrorCode::result_bound_invalid,
                kOperandInvalid, "post_projection_result_bound", 0,
                "an absent result-bound field has a nonzero value");
  }
  if (demand.row_limit_present && demand.row_offset_present &&
      demand.row_limit >
          std::numeric_limits<u64>::max() - demand.row_offset) {
    return Fail(error,
                NarrowQueryBindingDemandErrorCode::result_bound_invalid,
                kOperandInvalid, "post_projection_result_bound", 0,
                "row offset plus row limit overflows unsigned 64-bit");
  }

  for (std::size_t index = 0; index < demand.sources.size(); ++index) {
    const auto& source = demand.sources[index];
    if (source.source_ordinal != index) {
      return Fail(error,
                  NarrowQueryBindingDemandErrorCode::source_ordinal_invalid,
                  kPlanInvalid, "sources.source_ordinal",
                  static_cast<u32>(index),
                  "source ordinals must be dense and zero based");
    }
    if (source.relation_object_hint_present !=
        UuidPresent(source.relation_object_uuid_hint)) {
      return Fail(
          error,
          NarrowQueryBindingDemandErrorCode::source_relation_hint_invalid,
          kPlanInvalid, "sources.relation_object_uuid_hint",
          static_cast<u32>(index),
          "relation-object hint presence must exactly match a nonzero UUID");
    }
    if (source.explicit_alias != !source.alias_spelling.empty() ||
        source.alias_spelling.size() >
            kNarrowQueryBindingDemandMaximumAliasBytes ||
        (source.explicit_alias &&
         !ValidCanonicalUtf8Spelling(source.alias_spelling))) {
      return Fail(error,
                  NarrowQueryBindingDemandErrorCode::source_alias_invalid,
                  kPlanInvalid, "sources.alias_spelling",
                  static_cast<u32>(index),
                  "explicit-alias presence or canonical UTF-8 spelling is "
                  "invalid");
    }
  }

  std::set<SourceSpellingKey> output_references;
  bool repeated_output_reference = false;
  for (std::size_t index = 0; index < demand.outputs.size(); ++index) {
    const auto& output = demand.outputs[index];
    if (output.output_ordinal != index) {
      return Fail(error,
                  NarrowQueryBindingDemandErrorCode::output_ordinal_invalid,
                  kProjectionInvalid, "outputs.output_ordinal",
                  static_cast<u32>(index),
                  "output ordinals must be dense and zero based");
    }
    if (output.source_ordinal >= demand.sources.size()) {
      return Fail(error,
                  NarrowQueryBindingDemandErrorCode::output_source_invalid,
                  kProjectionInvalid, "outputs.source_ordinal",
                  static_cast<u32>(index),
                  "output source ordinal is outside the source vector");
    }
    if (output.source_column_spelling.size() >
            kNarrowQueryBindingDemandMaximumSpellingBytes ||
        !ValidCanonicalUtf8Spelling(output.source_column_spelling)) {
      return Fail(
          error,
          NarrowQueryBindingDemandErrorCode::output_column_spelling_invalid,
          kProjectionInvalid, "outputs.source_column_spelling",
          static_cast<u32>(index),
          "source-column spelling is not nonempty canonical UTF-8");
    }
    if (output.output_name_present !=
            !output.output_name_spelling.empty() ||
        output.output_name_spelling.size() >
            kNarrowQueryBindingDemandMaximumSpellingBytes ||
        (output.output_name_present &&
         !ValidCanonicalUtf8Spelling(output.output_name_spelling))) {
      return Fail(error,
                  NarrowQueryBindingDemandErrorCode::output_name_invalid,
                  kProjectionInvalid, "outputs.output_name_spelling",
                  static_cast<u32>(index),
                  "output-name presence or canonical UTF-8 spelling is "
                  "invalid");
    }
    const auto inserted = output_references.emplace(
        output.source_ordinal, output.source_column_spelling);
    repeated_output_reference = repeated_output_reference || !inserted.second;
  }

  std::set<SourceSpellingKey> ordering_references;
  for (std::size_t index = 0; index < demand.ordering_terms.size(); ++index) {
    const auto& term = demand.ordering_terms[index];
    if (term.term_ordinal != index) {
      return Fail(
          error,
          NarrowQueryBindingDemandErrorCode::ordering_ordinal_invalid,
          kOrderingInvalid, "ordering_terms.term_ordinal",
          static_cast<u32>(index),
          "ordering ordinals must be dense and zero based");
    }
    if (term.source_ordinal >= demand.sources.size()) {
      return Fail(error,
                  NarrowQueryBindingDemandErrorCode::ordering_source_invalid,
                  kOrderingInvalid, "ordering_terms.source_ordinal",
                  static_cast<u32>(index),
                  "ordering source ordinal is outside the source vector");
    }
    if (term.source_column_spelling.size() >
            kNarrowQueryBindingDemandMaximumSpellingBytes ||
        !ValidCanonicalUtf8Spelling(term.source_column_spelling)) {
      return Fail(
          error,
          NarrowQueryBindingDemandErrorCode::ordering_column_spelling_invalid,
          kOrderingInvalid, "ordering_terms.source_column_spelling",
          static_cast<u32>(index),
          "ordering source-column spelling is not canonical UTF-8");
    }
    if (!ValidDirection(term.direction)) {
      return Fail(
          error,
          NarrowQueryBindingDemandErrorCode::ordering_direction_invalid,
          kOrderingInvalid, "ordering_terms.direction",
          static_cast<u32>(index), "ordering direction code is unknown");
    }
    if (!ValidNullPlacement(term.null_placement)) {
      return Fail(
          error,
          NarrowQueryBindingDemandErrorCode::ordering_null_placement_invalid,
          kOrderingInvalid, "ordering_terms.null_placement",
          static_cast<u32>(index), "null-placement code is unknown");
    }
    if (!ordering_references
             .emplace(term.source_ordinal, term.source_column_spelling)
             .second) {
      return Fail(error,
                  NarrowQueryBindingDemandErrorCode::profile_shape_invalid,
                  kOrderingInvalid, "ordered_projection_shape",
                  static_cast<u32>(index),
                  "ordering source-column demand is duplicated");
    }
  }

  if (demand.requested_profile ==
      NarrowQueryProfile::ordered_projection) {
    if (demand.sources.size() != 1 || demand.ordering_terms.size() < 2 ||
        repeated_output_reference) {
      return Fail(error,
                  NarrowQueryBindingDemandErrorCode::profile_shape_invalid,
                  kOrderingInvalid, "ordered_projection_shape", 0,
                  "ordered projection requires one source, at least two "
                  "distinct ordering terms, and distinct projected columns");
    }
  } else if (demand.requested_profile ==
             NarrowQueryProfile::projection_occurrence) {
    if (demand.sources.size() != 1 || demand.outputs.size() < 2 ||
        !demand.ordering_terms.empty() || !repeated_output_reference) {
      return Fail(error,
                  NarrowQueryBindingDemandErrorCode::profile_shape_invalid,
                  kProjectionInvalid, "projection_occurrence_shape", 0,
                  "projection occurrence requires one source, repeated "
                  "projected column demand, and no ordering vector");
    }
  } else {
    if (demand.sources.size() < 2 || !demand.ordering_terms.empty()) {
      return Fail(error,
                  NarrowQueryBindingDemandErrorCode::profile_shape_invalid,
                  kPlanInvalid, "alias_distinct_self_join_shape", 0,
                  "alias-distinct self join requires two through nine sources "
                  "and no ordering vector");
    }
    std::set<std::string_view> aliases;
    for (std::size_t index = 0; index < demand.sources.size(); ++index) {
      const auto& source = demand.sources[index];
      if (!source.explicit_alias ||
          !aliases.insert(source.alias_spelling).second) {
        return Fail(error,
                    NarrowQueryBindingDemandErrorCode::profile_shape_invalid,
                    kPlanInvalid, "alias_distinct_self_join_aliases",
                    static_cast<u32>(index),
                    "every self-join source requires a distinct explicit "
                    "alias spelling");
      }
      const bool contributes_output = std::any_of(
          demand.outputs.begin(), demand.outputs.end(),
          [index](const NarrowQueryOutputDemand& output) {
            return output.source_ordinal == index;
          });
      if (!contributes_output) {
        return Fail(error,
                    NarrowQueryBindingDemandErrorCode::profile_shape_invalid,
                    kProjectionInvalid,
                    "alias_distinct_self_join_output_coverage",
                    static_cast<u32>(index),
                    "every self-join source must contribute an output demand");
      }
    }
  }
  return true;
}

void EncodeSource(const NarrowQuerySourceDemand& source,
                  std::vector<byte>* bytes) {
  const auto start = bytes->size();
  const auto record_bytes = static_cast<u32>(
      kNarrowQueryBindingDemandSourcePrefixBytes +
      source.alias_spelling.size());
  bytes->resize(start + kNarrowQueryBindingDemandSourcePrefixBytes, 0);
  StoreLittle32(bytes->data() + start, record_bytes);
  StoreLittle32(bytes->data() + start + 4u, source.source_ordinal);
  const u32 flags =
      (source.relation_object_hint_present ? kRelationHintPresentFlag : 0u) |
      (source.explicit_alias ? kExplicitAliasFlag : 0u);
  StoreLittle32(bytes->data() + start + 8u, flags);
  StoreLittle32(bytes->data() + start + 12u,
                static_cast<u32>(source.alias_spelling.size()));
  StoreUuid(bytes, start + 16u, source.relation_object_uuid_hint);
  AppendString(bytes, source.alias_spelling);
}

void EncodeOutput(const NarrowQueryOutputDemand& output,
                  std::vector<byte>* bytes) {
  const auto start = bytes->size();
  const auto record_bytes = static_cast<u32>(
      kNarrowQueryBindingDemandOutputPrefixBytes +
      output.source_column_spelling.size() +
      output.output_name_spelling.size());
  bytes->resize(start + kNarrowQueryBindingDemandOutputPrefixBytes, 0);
  StoreLittle32(bytes->data() + start, record_bytes);
  StoreLittle32(bytes->data() + start + 4u, output.output_ordinal);
  StoreLittle32(bytes->data() + start + 8u, output.source_ordinal);
  StoreLittle32(bytes->data() + start + 12u,
                output.output_name_present ? kOutputNamePresentFlag : 0u);
  StoreLittle32(bytes->data() + start + 16u,
                static_cast<u32>(output.source_column_spelling.size()));
  StoreLittle32(bytes->data() + start + 20u,
                static_cast<u32>(output.output_name_spelling.size()));
  AppendString(bytes, output.source_column_spelling);
  AppendString(bytes, output.output_name_spelling);
}

void EncodeOrdering(const NarrowQueryOrderingDemand& term,
                    std::vector<byte>* bytes) {
  const auto start = bytes->size();
  const auto record_bytes = static_cast<u32>(
      kNarrowQueryBindingDemandOrderingPrefixBytes +
      term.source_column_spelling.size());
  bytes->resize(start + kNarrowQueryBindingDemandOrderingPrefixBytes, 0);
  StoreLittle32(bytes->data() + start, record_bytes);
  StoreLittle32(bytes->data() + start + 4u, term.term_ordinal);
  StoreLittle32(bytes->data() + start + 8u, term.source_ordinal);
  StoreLittle32(bytes->data() + start + 12u,
                static_cast<u32>(term.source_column_spelling.size()));
  (*bytes)[start + 16u] = static_cast<u8>(term.direction);
  (*bytes)[start + 17u] = static_cast<u8>(term.null_placement);
  AppendString(bytes, term.source_column_spelling);
}

bool PreflightSources(std::span<const byte> records,
                      u32 count,
                      NarrowQueryBindingDemandError* error) {
  std::size_t offset = 0;
  for (u32 index = 0; index < count; ++index) {
    const auto remaining = records.size() - offset;
    if (remaining < kNarrowQueryBindingDemandSourcePrefixBytes) {
      return Fail(error,
                  NarrowQueryBindingDemandErrorCode::source_extent_invalid,
                  kOperandInvalid, "sources.record_bytes", index,
                  "source record prefix is truncated");
    }
    const auto record_bytes = LoadLittle32(records.data() + offset);
    const auto alias_bytes = LoadLittle32(records.data() + offset + 12u);
    if (record_bytes < kNarrowQueryBindingDemandSourcePrefixBytes ||
        record_bytes > remaining ||
        alias_bytes !=
            record_bytes - kNarrowQueryBindingDemandSourcePrefixBytes) {
      return Fail(error,
                  NarrowQueryBindingDemandErrorCode::source_extent_invalid,
                  kOperandInvalid, "sources.record_bytes", index,
                  "source record and alias extents are not exact");
    }
    if (LoadLittle32(records.data() + offset + 4u) != index) {
      return Fail(error,
                  NarrowQueryBindingDemandErrorCode::source_ordinal_invalid,
                  kPlanInvalid, "sources.source_ordinal", index,
                  "source ordinals must be dense and zero based");
    }
    const auto flags = LoadLittle32(records.data() + offset + 8u);
    if ((flags & ~kSourceFlags) != 0) {
      return Fail(error, NarrowQueryBindingDemandErrorCode::flag_invalid,
                  kOperandInvalid, "sources.flags", index,
                  "source record contains an unknown presence flag");
    }
    const bool relation_present =
        (flags & kRelationHintPresentFlag) != 0;
    const bool relation_nonzero =
        UuidPresent(LoadUuid(records, offset + 16u));
    if (relation_present != relation_nonzero) {
      return Fail(
          error,
          NarrowQueryBindingDemandErrorCode::source_relation_hint_invalid,
          kPlanInvalid, "sources.relation_object_uuid_hint", index,
          "relation-object hint presence does not match the encoded UUID");
    }
    const bool explicit_alias = (flags & kExplicitAliasFlag) != 0;
    const auto alias = StringAt(
        records, offset + kNarrowQueryBindingDemandSourcePrefixBytes,
        alias_bytes);
    if (explicit_alias != !alias.empty() ||
        alias.size() > kNarrowQueryBindingDemandMaximumAliasBytes ||
        (explicit_alias && !ValidCanonicalUtf8Spelling(alias))) {
      return Fail(error,
                  NarrowQueryBindingDemandErrorCode::source_alias_invalid,
                  kPlanInvalid, "sources.alias_spelling", index,
                  "explicit-alias presence or canonical UTF-8 spelling is "
                  "invalid");
    }
    offset += record_bytes;
  }
  if (offset != records.size()) {
    return Fail(error,
                NarrowQueryBindingDemandErrorCode::source_extent_invalid,
                kOperandInvalid, "sources.extent", count,
                "source vector has unclaimed trailing bytes");
  }
  return true;
}

bool PreflightOutputs(std::span<const byte> records,
                      u32 count,
                      u32 source_count,
                      NarrowQueryBindingDemandError* error) {
  std::size_t offset = 0;
  for (u32 index = 0; index < count; ++index) {
    const auto remaining = records.size() - offset;
    if (remaining < kNarrowQueryBindingDemandOutputPrefixBytes) {
      return Fail(error,
                  NarrowQueryBindingDemandErrorCode::output_extent_invalid,
                  kOperandInvalid, "outputs.record_bytes", index,
                  "output record prefix is truncated");
    }
    const auto record_bytes = LoadLittle32(records.data() + offset);
    const auto column_bytes = LoadLittle32(records.data() + offset + 16u);
    const auto name_bytes = LoadLittle32(records.data() + offset + 20u);
    const u64 expected_bytes =
        static_cast<u64>(kNarrowQueryBindingDemandOutputPrefixBytes) +
        column_bytes + name_bytes;
    if (record_bytes < kNarrowQueryBindingDemandOutputPrefixBytes ||
        record_bytes > remaining || expected_bytes != record_bytes) {
      return Fail(error,
                  NarrowQueryBindingDemandErrorCode::output_extent_invalid,
                  kOperandInvalid, "outputs.record_bytes", index,
                  "output record and spelling extents are not exact");
    }
    if (LoadLittle32(records.data() + offset + 4u) != index) {
      return Fail(error,
                  NarrowQueryBindingDemandErrorCode::output_ordinal_invalid,
                  kProjectionInvalid, "outputs.output_ordinal", index,
                  "output ordinals must be dense and zero based");
    }
    if (LoadLittle32(records.data() + offset + 8u) >= source_count) {
      return Fail(error,
                  NarrowQueryBindingDemandErrorCode::output_source_invalid,
                  kProjectionInvalid, "outputs.source_ordinal", index,
                  "output source ordinal is outside the source vector");
    }
    const auto flags = LoadLittle32(records.data() + offset + 12u);
    if ((flags & ~kOutputNamePresentFlag) != 0) {
      return Fail(error, NarrowQueryBindingDemandErrorCode::flag_invalid,
                  kOperandInvalid, "outputs.flags", index,
                  "output record contains an unknown presence flag");
    }
    const auto column = StringAt(
        records, offset + kNarrowQueryBindingDemandOutputPrefixBytes,
        column_bytes);
    if (column.size() > kNarrowQueryBindingDemandMaximumSpellingBytes ||
        !ValidCanonicalUtf8Spelling(column)) {
      return Fail(
          error,
          NarrowQueryBindingDemandErrorCode::output_column_spelling_invalid,
          kProjectionInvalid, "outputs.source_column_spelling", index,
          "source-column spelling is not nonempty canonical UTF-8");
    }
    const auto name = StringAt(
        records,
        offset + kNarrowQueryBindingDemandOutputPrefixBytes + column_bytes,
        name_bytes);
    const bool name_present = (flags & kOutputNamePresentFlag) != 0;
    if (name_present != !name.empty() ||
        name.size() > kNarrowQueryBindingDemandMaximumSpellingBytes ||
        (name_present && !ValidCanonicalUtf8Spelling(name))) {
      return Fail(error,
                  NarrowQueryBindingDemandErrorCode::output_name_invalid,
                  kProjectionInvalid, "outputs.output_name_spelling", index,
                  "output-name presence or canonical UTF-8 spelling is "
                  "invalid");
    }
    offset += record_bytes;
  }
  if (offset != records.size()) {
    return Fail(error,
                NarrowQueryBindingDemandErrorCode::output_extent_invalid,
                kOperandInvalid, "outputs.extent", count,
                "output vector has unclaimed trailing bytes");
  }
  return true;
}

bool PreflightOrdering(std::span<const byte> records,
                       u32 count,
                       u32 source_count,
                       NarrowQueryBindingDemandError* error) {
  std::size_t offset = 0;
  for (u32 index = 0; index < count; ++index) {
    const auto remaining = records.size() - offset;
    if (remaining < kNarrowQueryBindingDemandOrderingPrefixBytes) {
      return Fail(error,
                  NarrowQueryBindingDemandErrorCode::ordering_extent_invalid,
                  kOperandInvalid, "ordering_terms.record_bytes", index,
                  "ordering record prefix is truncated");
    }
    const auto record_bytes = LoadLittle32(records.data() + offset);
    const auto column_bytes = LoadLittle32(records.data() + offset + 12u);
    if (record_bytes < kNarrowQueryBindingDemandOrderingPrefixBytes ||
        record_bytes > remaining ||
        static_cast<u64>(kNarrowQueryBindingDemandOrderingPrefixBytes) +
                column_bytes !=
            record_bytes) {
      return Fail(error,
                  NarrowQueryBindingDemandErrorCode::ordering_extent_invalid,
                  kOperandInvalid, "ordering_terms.record_bytes", index,
                  "ordering record and spelling extents are not exact");
    }
    if (LoadLittle32(records.data() + offset + 4u) != index) {
      return Fail(
          error,
          NarrowQueryBindingDemandErrorCode::ordering_ordinal_invalid,
          kOrderingInvalid, "ordering_terms.term_ordinal", index,
          "ordering ordinals must be dense and zero based");
    }
    if (LoadLittle32(records.data() + offset + 8u) >= source_count) {
      return Fail(error,
                  NarrowQueryBindingDemandErrorCode::ordering_source_invalid,
                  kOrderingInvalid, "ordering_terms.source_ordinal", index,
                  "ordering source ordinal is outside the source vector");
    }
    if (LoadLittle16(records.data() + offset + 18u) != 0 ||
        LoadLittle32(records.data() + offset + 20u) != 0) {
      return Fail(error, NarrowQueryBindingDemandErrorCode::reserved_invalid,
                  kOperandInvalid, "ordering_terms.reserved", index,
                  "ordering record reserved bytes are nonzero");
    }
    const auto direction = static_cast<NarrowQueryDirection>(
        records[offset + 16u]);
    if (!ValidDirection(direction)) {
      return Fail(
          error,
          NarrowQueryBindingDemandErrorCode::ordering_direction_invalid,
          kOrderingInvalid, "ordering_terms.direction", index,
          "ordering direction code is unknown");
    }
    const auto null_placement = static_cast<NarrowQueryNullPlacement>(
        records[offset + 17u]);
    if (!ValidNullPlacement(null_placement)) {
      return Fail(
          error,
          NarrowQueryBindingDemandErrorCode::ordering_null_placement_invalid,
          kOrderingInvalid, "ordering_terms.null_placement", index,
          "null-placement code is unknown");
    }
    const auto column = StringAt(
        records, offset + kNarrowQueryBindingDemandOrderingPrefixBytes,
        column_bytes);
    if (column.size() > kNarrowQueryBindingDemandMaximumSpellingBytes ||
        !ValidCanonicalUtf8Spelling(column)) {
      return Fail(
          error,
          NarrowQueryBindingDemandErrorCode::ordering_column_spelling_invalid,
          kOrderingInvalid, "ordering_terms.source_column_spelling", index,
          "ordering source-column spelling is not canonical UTF-8");
    }
    offset += record_bytes;
  }
  if (offset != records.size()) {
    return Fail(error,
                NarrowQueryBindingDemandErrorCode::ordering_extent_invalid,
                kOperandInvalid, "ordering_terms.extent", count,
                "ordering vector has unclaimed trailing bytes");
  }
  return true;
}

void DecodeSources(std::span<const byte> records,
                   u32 count,
                   std::vector<NarrowQuerySourceDemand>* sources) {
  sources->reserve(count);
  std::size_t offset = 0;
  for (u32 index = 0; index < count; ++index) {
    const auto record_bytes = LoadLittle32(records.data() + offset);
    const auto flags = LoadLittle32(records.data() + offset + 8u);
    const auto alias_bytes = LoadLittle32(records.data() + offset + 12u);
    NarrowQuerySourceDemand source;
    source.source_ordinal = index;
    source.relation_object_hint_present =
        (flags & kRelationHintPresentFlag) != 0;
    source.relation_object_uuid_hint = LoadUuid(records, offset + 16u);
    source.explicit_alias = (flags & kExplicitAliasFlag) != 0;
    source.alias_spelling = StringAt(
        records, offset + kNarrowQueryBindingDemandSourcePrefixBytes,
        alias_bytes);
    sources->push_back(std::move(source));
    offset += record_bytes;
  }
}

void DecodeOutputs(std::span<const byte> records,
                   u32 count,
                   std::vector<NarrowQueryOutputDemand>* outputs) {
  outputs->reserve(count);
  std::size_t offset = 0;
  for (u32 index = 0; index < count; ++index) {
    const auto record_bytes = LoadLittle32(records.data() + offset);
    const auto column_bytes = LoadLittle32(records.data() + offset + 16u);
    const auto name_bytes = LoadLittle32(records.data() + offset + 20u);
    NarrowQueryOutputDemand output;
    output.output_ordinal = index;
    output.source_ordinal = LoadLittle32(records.data() + offset + 8u);
    output.output_name_present =
        (LoadLittle32(records.data() + offset + 12u) &
         kOutputNamePresentFlag) != 0;
    output.source_column_spelling = StringAt(
        records, offset + kNarrowQueryBindingDemandOutputPrefixBytes,
        column_bytes);
    output.output_name_spelling = StringAt(
        records,
        offset + kNarrowQueryBindingDemandOutputPrefixBytes + column_bytes,
        name_bytes);
    outputs->push_back(std::move(output));
    offset += record_bytes;
  }
}

void DecodeOrdering(std::span<const byte> records,
                    u32 count,
                    std::vector<NarrowQueryOrderingDemand>* ordering_terms) {
  ordering_terms->reserve(count);
  std::size_t offset = 0;
  for (u32 index = 0; index < count; ++index) {
    const auto record_bytes = LoadLittle32(records.data() + offset);
    const auto column_bytes = LoadLittle32(records.data() + offset + 12u);
    NarrowQueryOrderingDemand term;
    term.term_ordinal = index;
    term.source_ordinal = LoadLittle32(records.data() + offset + 8u);
    term.direction =
        static_cast<NarrowQueryDirection>(records[offset + 16u]);
    term.null_placement =
        static_cast<NarrowQueryNullPlacement>(records[offset + 17u]);
    term.source_column_spelling = StringAt(
        records, offset + kNarrowQueryBindingDemandOrderingPrefixBytes,
        column_bytes);
    ordering_terms->push_back(std::move(term));
    offset += record_bytes;
  }
}

}  // namespace

bool EncodeNarrowQueryBindingDemand(
    const NarrowQueryBindingDemand& demand,
    std::vector<byte>* encoded,
    NarrowQueryBindingDemandError* error) {
  ClearError(error);
  if (encoded == nullptr) {
    return Fail(error, NarrowQueryBindingDemandErrorCode::invalid_argument,
                kOperandInvalid, "encoded", 0,
                "encoded output pointer is null");
  }

  u64 total_bytes = 0;
  u32 source_bytes = 0;
  u32 output_bytes = 0;
  u32 ordering_bytes = 0;
  if (!ComputeEncodedExtent(demand, &total_bytes, &source_bytes,
                            &output_bytes, &ordering_bytes, error) ||
      !ValidateDemandStructure(demand, error)) {
    return false;
  }

  std::vector<byte> bytes(kNarrowQueryBindingDemandHeaderBytes, 0);
  std::copy(kMagic.begin(), kMagic.end(), bytes.begin());
  StoreLittle16(bytes.data() + 8u, kNarrowQueryBindingDemandVersion);
  StoreLittle16(bytes.data() + 10u,
                kNarrowQueryBindingDemandHeaderBytes);
  const u32 result_bound_flags =
      (demand.row_limit_present ? kRowLimitPresentFlag : 0u) |
      (demand.row_offset_present ? kRowOffsetPresentFlag : 0u);
  StoreLittle32(bytes.data() + 12u, result_bound_flags);
  StoreLittle64(bytes.data() + 16u, total_bytes);
  StoreLittle16(bytes.data() + 24u,
                static_cast<u16>(demand.requested_profile));
  StoreLittle16(bytes.data() + 26u,
                static_cast<u16>(demand.sources.size()));
  StoreLittle32(bytes.data() + 28u,
                static_cast<u32>(demand.outputs.size()));
  StoreLittle32(bytes.data() + 32u,
                static_cast<u32>(demand.ordering_terms.size()));
  StoreLittle32(bytes.data() + 36u, source_bytes);
  StoreLittle32(bytes.data() + 40u, output_bytes);
  StoreLittle32(bytes.data() + 44u, ordering_bytes);
  StoreUuid(&bytes, 48u, demand.statement_receipt_uuid);
  const bool result_bound_present =
      demand.row_limit_present || demand.row_offset_present;
  StoreLittle32(bytes.data() + 64u,
                result_bound_present
                    ? kNarrowQueryBindingDemandResultBoundBytes
                    : 0u);

  bytes.resize(kNarrowQueryBindingDemandHeaderBytes + sizeof(u64), 0);
  StoreLittle64(
      bytes.data() + kNarrowQueryBindingDemandHeaderBytes,
      demand.maximum_mga_relation_decoded_bytes_per_pass);

  if (result_bound_present) {
    const auto offset = bytes.size();
    bytes.resize(offset + kNarrowQueryBindingDemandResultBoundBytes, 0);
    StoreLittle64(bytes.data() + offset, demand.row_limit);
    StoreLittle64(bytes.data() + offset + 8u, demand.row_offset);
  }
  for (const auto& source : demand.sources) {
    EncodeSource(source, &bytes);
  }
  for (const auto& output : demand.outputs) {
    EncodeOutput(output, &bytes);
  }
  for (const auto& term : demand.ordering_terms) {
    EncodeOrdering(term, &bytes);
  }
  if (bytes.size() != total_bytes) {
    return Fail(error, NarrowQueryBindingDemandErrorCode::extent_invalid,
                kOperandInvalid, "carrier_extent", 0,
                "encoded extent differs from the checked preflight extent");
  }
  if (!demand.exact_bytes.empty() && demand.exact_bytes != bytes) {
    return Fail(error,
                NarrowQueryBindingDemandErrorCode::exact_bytes_mismatch,
                kOperandInvalid, "exact_bytes", 0,
                "retained exact carrier differs from canonical re-encoding");
  }
  *encoded = std::move(bytes);
  return true;
}

bool DecodeAndValidateNarrowQueryBindingDemand(
    std::span<const byte> encoded,
    const NarrowQueryBindingDemandValidationContext& context,
    NarrowQueryBindingDemand* decoded,
    NarrowQueryBindingDemandError* error) {
  ClearError(error);
  if (decoded == nullptr) {
    return Fail(error, NarrowQueryBindingDemandErrorCode::invalid_argument,
                kOperandInvalid, "decoded", 0,
                "decoded output pointer is null");
  }
  if (!UuidPresent(context.authenticated_statement_receipt_uuid)) {
    return Fail(error, NarrowQueryBindingDemandErrorCode::receipt_invalid,
                kTransactionStale,
                "context.authenticated_statement_receipt_uuid", 0,
                "authenticated statement receipt UUID is zero");
  }
  if (!ValidMgaRelationDecodedBytesPerPass(
          context.maximum_mga_relation_decoded_bytes_per_pass)) {
    return Fail(error, NarrowQueryBindingDemandErrorCode::invalid_argument,
                kResourceExceeded,
                "context.maximum_mga_relation_decoded_bytes_per_pass", 0,
                "authenticated statement resource policy is absent or "
                "outside the inclusive 64 KiB through 1 TiB range");
  }
  if (context.maximum_total_bytes == 0 ||
      context.maximum_total_bytes >
          kNarrowQueryBindingDemandMaximumCarrierBytes) {
    return Fail(error, NarrowQueryBindingDemandErrorCode::invalid_argument,
                kOperandInvalid, "context.maximum_total_bytes", 0,
                "decode byte ceiling is zero or exceeds 16 MiB");
  }
  if (encoded.size() <
      kNarrowQueryBindingDemandHeaderBytes + sizeof(u64)) {
    return Fail(error, NarrowQueryBindingDemandErrorCode::extent_invalid,
                kOperandInvalid, "carrier_extent", 0,
                "carrier is shorter than the fixed 72-byte header plus the "
                "mandatory scan-byte resource extent");
  }
  if (encoded.size() > kNarrowQueryBindingDemandMaximumCarrierBytes ||
      encoded.size() > context.maximum_total_bytes) {
    return Fail(error,
                NarrowQueryBindingDemandErrorCode::resource_limit_exceeded,
                kResourceExceeded, "carrier_extent", 0,
                "carrier exceeds the Core or live decode byte ceiling");
  }
  if (!std::equal(kMagic.begin(), kMagic.end(), encoded.begin())) {
    return Fail(error, NarrowQueryBindingDemandErrorCode::magic_invalid,
                kOperandInvalid, "magic", 0,
                "carrier magic is not SBQNDR01");
  }
  if (LoadLittle16(encoded.data() + 8u) !=
      kNarrowQueryBindingDemandVersion) {
    return Fail(error, NarrowQueryBindingDemandErrorCode::version_invalid,
                kOperandInvalid, "layout_version", 0,
                "carrier version is not 1");
  }
  if (LoadLittle16(encoded.data() + 10u) !=
          kNarrowQueryBindingDemandHeaderBytes ||
      LoadLittle64(encoded.data() + 16u) != encoded.size()) {
    return Fail(error, NarrowQueryBindingDemandErrorCode::extent_invalid,
                kOperandInvalid, "header_or_total_bytes", 0,
                "header or total carrier extent is not exact");
  }
  const auto result_bound_flags = LoadLittle32(encoded.data() + 12u);
  if ((result_bound_flags & ~kResultBoundFlags) != 0 ||
      LoadLittle32(encoded.data() + 68u) != 0) {
    return Fail(error, NarrowQueryBindingDemandErrorCode::reserved_invalid,
                kOperandInvalid, "header_reserved", 0,
                "header has an unknown presence flag or nonzero reserved "
                "bytes");
  }

  const auto encoded_receipt = LoadUuid(encoded, 48u);
  if (!UuidPresent(encoded_receipt)) {
    return Fail(error, NarrowQueryBindingDemandErrorCode::receipt_invalid,
                kTransactionStale, "statement_receipt_uuid", 0,
                "carrier statement receipt UUID is zero");
  }
  if (encoded_receipt != context.authenticated_statement_receipt_uuid) {
    return Fail(error, NarrowQueryBindingDemandErrorCode::receipt_mismatch,
                kTransactionStale, "statement_receipt_uuid", 0,
                "carrier receipt differs from the authenticated statement "
                "receipt");
  }

  const auto maximum_mga_relation_decoded_bytes_per_pass =
      LoadLittle64(encoded.data() + kNarrowQueryBindingDemandHeaderBytes);
  if (!ValidMgaRelationDecodedBytesPerPass(
          maximum_mga_relation_decoded_bytes_per_pass)) {
    return Fail(error,
                NarrowQueryBindingDemandErrorCode::resource_limit_exceeded,
                kResourceExceeded,
                "maximum_mga_relation_decoded_bytes_per_pass", 0,
                "encoded MGA relation decoded-byte ceiling is outside the "
                "inclusive 64 KiB through 1 TiB range");
  }
  if (maximum_mga_relation_decoded_bytes_per_pass !=
      context.maximum_mga_relation_decoded_bytes_per_pass) {
    return Fail(error,
                NarrowQueryBindingDemandErrorCode::resource_limit_exceeded,
                kResourceExceeded,
                "maximum_mga_relation_decoded_bytes_per_pass", 0,
                "encoded scan-byte resource policy differs from the exact "
                "authenticated statement receipt");
  }

  const auto profile =
      static_cast<NarrowQueryProfile>(LoadLittle16(encoded.data() + 24u));
  if (!ValidProfile(profile)) {
    return Fail(error, NarrowQueryBindingDemandErrorCode::profile_invalid,
                kOperandInvalid, "requested_profile", 0,
                "requested narrow-query profile code is unknown");
  }
  const auto source_count = LoadLittle16(encoded.data() + 26u);
  const auto output_count = LoadLittle32(encoded.data() + 28u);
  const auto ordering_count = LoadLittle32(encoded.data() + 32u);
  if (source_count == 0 ||
      source_count > kNarrowQueryBindingDemandMaximumSources ||
      output_count == 0 ||
      output_count > kNarrowQueryBindingDemandMaximumOutputs ||
      ordering_count > kNarrowQueryBindingDemandMaximumOrderingTerms) {
    return Fail(error, NarrowQueryBindingDemandErrorCode::count_invalid,
                kOperandInvalid, "vector_counts", 0,
                "source, output, or ordering count is outside the admitted "
                "bounds");
  }

  const bool row_limit_present =
      (result_bound_flags & kRowLimitPresentFlag) != 0;
  const bool row_offset_present =
      (result_bound_flags & kRowOffsetPresentFlag) != 0;
  const bool result_bound_present =
      row_limit_present || row_offset_present;
  const auto result_bound_bytes = LoadLittle32(encoded.data() + 64u);
  if (result_bound_bytes !=
      (result_bound_present
           ? kNarrowQueryBindingDemandResultBoundBytes
           : 0u)) {
    return Fail(error,
                NarrowQueryBindingDemandErrorCode::result_bound_invalid,
                kOperandInvalid, "post_projection_result_bound.extent", 0,
                "result-bound presence and extent do not agree");
  }

  const auto source_bytes = LoadLittle32(encoded.data() + 36u);
  const auto output_bytes = LoadLittle32(encoded.data() + 40u);
  const auto ordering_bytes = LoadLittle32(encoded.data() + 44u);
  u64 checked_total = kNarrowQueryBindingDemandHeaderBytes + sizeof(u64);
  if (!AddWithin(checked_total, result_bound_bytes,
                 kNarrowQueryBindingDemandMaximumCarrierBytes,
                 &checked_total) ||
      !AddWithin(checked_total, source_bytes,
                 kNarrowQueryBindingDemandMaximumCarrierBytes,
                 &checked_total) ||
      !AddWithin(checked_total, output_bytes,
                 kNarrowQueryBindingDemandMaximumCarrierBytes,
                 &checked_total) ||
      !AddWithin(checked_total, ordering_bytes,
                 kNarrowQueryBindingDemandMaximumCarrierBytes,
                 &checked_total) ||
      checked_total != encoded.size()) {
    return Fail(error, NarrowQueryBindingDemandErrorCode::extent_invalid,
                kOperandInvalid, "vector_extents", 0,
                "checked vector extents do not exactly claim the carrier");
  }

  u64 row_limit = 0;
  u64 row_offset = 0;
  if (result_bound_present) {
    row_limit =
        LoadLittle64(encoded.data() +
                     kNarrowQueryBindingDemandHeaderBytes + sizeof(u64));
    row_offset = LoadLittle64(
        encoded.data() + kNarrowQueryBindingDemandHeaderBytes +
        sizeof(u64) + 8u);
    if ((!row_limit_present && row_limit != 0) ||
        (!row_offset_present && row_offset != 0)) {
      return Fail(error,
                  NarrowQueryBindingDemandErrorCode::result_bound_invalid,
                  kOperandInvalid, "post_projection_result_bound", 0,
                  "an absent result-bound field has a nonzero encoded value");
    }
    if (row_limit_present && row_offset_present &&
        row_limit > std::numeric_limits<u64>::max() - row_offset) {
      return Fail(error,
                  NarrowQueryBindingDemandErrorCode::result_bound_invalid,
                  kOperandInvalid, "post_projection_result_bound", 0,
                  "row offset plus row limit overflows unsigned 64-bit");
    }
  }

  std::size_t offset =
      kNarrowQueryBindingDemandHeaderBytes + sizeof(u64) +
      result_bound_bytes;
  const auto source_records = encoded.subspan(offset, source_bytes);
  offset += source_bytes;
  const auto output_records = encoded.subspan(offset, output_bytes);
  offset += output_bytes;
  const auto ordering_records = encoded.subspan(offset, ordering_bytes);

  // All header, aggregate, and per-record extents are proven before the first
  // decoded vector/string allocation below.
  if (!PreflightSources(source_records, source_count, error) ||
      !PreflightOutputs(output_records, output_count, source_count, error) ||
      !PreflightOrdering(ordering_records, ordering_count, source_count,
                         error)) {
    return false;
  }

  NarrowQueryBindingDemand value;
  value.statement_receipt_uuid = encoded_receipt;
  value.requested_profile = profile;
  value.maximum_mga_relation_decoded_bytes_per_pass =
      maximum_mga_relation_decoded_bytes_per_pass;
  value.row_limit_present = row_limit_present;
  value.row_limit = row_limit;
  value.row_offset_present = row_offset_present;
  value.row_offset = row_offset;
  DecodeSources(source_records, source_count, &value.sources);
  DecodeOutputs(output_records, output_count, &value.outputs);
  DecodeOrdering(ordering_records, ordering_count, &value.ordering_terms);
  if (!ValidateDemandStructure(value, error)) {
    return false;
  }

  std::vector<byte> canonical;
  NarrowQueryBindingDemandError canonical_error;
  if (!EncodeNarrowQueryBindingDemand(value, &canonical, &canonical_error)) {
    if (error != nullptr) {
      *error = std::move(canonical_error);
    }
    return false;
  }
  if (!std::equal(canonical.begin(), canonical.end(), encoded.begin(),
                  encoded.end())) {
    return Fail(error,
                NarrowQueryBindingDemandErrorCode::exact_bytes_mismatch,
                kOperandInvalid, "exact_bytes", 0,
                "carrier is structurally valid but not the unique canonical "
                "encoding");
  }
  value.exact_bytes.assign(encoded.begin(), encoded.end());
  *decoded = std::move(value);
  return true;
}

const char* NarrowQueryBindingDemandErrorCodeName(
    NarrowQueryBindingDemandErrorCode code) {
  switch (code) {
    case NarrowQueryBindingDemandErrorCode::ok:
      return "ok";
    case NarrowQueryBindingDemandErrorCode::invalid_argument:
      return "invalid_argument";
    case NarrowQueryBindingDemandErrorCode::magic_invalid:
      return "magic_invalid";
    case NarrowQueryBindingDemandErrorCode::version_invalid:
      return "version_invalid";
    case NarrowQueryBindingDemandErrorCode::extent_invalid:
      return "extent_invalid";
    case NarrowQueryBindingDemandErrorCode::reserved_invalid:
      return "reserved_invalid";
    case NarrowQueryBindingDemandErrorCode::receipt_invalid:
      return "receipt_invalid";
    case NarrowQueryBindingDemandErrorCode::receipt_mismatch:
      return "receipt_mismatch";
    case NarrowQueryBindingDemandErrorCode::profile_invalid:
      return "profile_invalid";
    case NarrowQueryBindingDemandErrorCode::flag_invalid:
      return "flag_invalid";
    case NarrowQueryBindingDemandErrorCode::count_invalid:
      return "count_invalid";
    case NarrowQueryBindingDemandErrorCode::source_extent_invalid:
      return "source_extent_invalid";
    case NarrowQueryBindingDemandErrorCode::source_ordinal_invalid:
      return "source_ordinal_invalid";
    case NarrowQueryBindingDemandErrorCode::source_relation_hint_invalid:
      return "source_relation_hint_invalid";
    case NarrowQueryBindingDemandErrorCode::source_alias_invalid:
      return "source_alias_invalid";
    case NarrowQueryBindingDemandErrorCode::output_extent_invalid:
      return "output_extent_invalid";
    case NarrowQueryBindingDemandErrorCode::output_ordinal_invalid:
      return "output_ordinal_invalid";
    case NarrowQueryBindingDemandErrorCode::output_source_invalid:
      return "output_source_invalid";
    case NarrowQueryBindingDemandErrorCode::output_column_spelling_invalid:
      return "output_column_spelling_invalid";
    case NarrowQueryBindingDemandErrorCode::output_name_invalid:
      return "output_name_invalid";
    case NarrowQueryBindingDemandErrorCode::ordering_extent_invalid:
      return "ordering_extent_invalid";
    case NarrowQueryBindingDemandErrorCode::ordering_ordinal_invalid:
      return "ordering_ordinal_invalid";
    case NarrowQueryBindingDemandErrorCode::ordering_source_invalid:
      return "ordering_source_invalid";
    case NarrowQueryBindingDemandErrorCode::ordering_column_spelling_invalid:
      return "ordering_column_spelling_invalid";
    case NarrowQueryBindingDemandErrorCode::ordering_direction_invalid:
      return "ordering_direction_invalid";
    case NarrowQueryBindingDemandErrorCode::ordering_null_placement_invalid:
      return "ordering_null_placement_invalid";
    case NarrowQueryBindingDemandErrorCode::profile_shape_invalid:
      return "profile_shape_invalid";
    case NarrowQueryBindingDemandErrorCode::result_bound_invalid:
      return "result_bound_invalid";
    case NarrowQueryBindingDemandErrorCode::resource_limit_exceeded:
      return "resource_limit_exceeded";
    case NarrowQueryBindingDemandErrorCode::exact_bytes_mismatch:
      return "exact_bytes_mismatch";
  }
  return "unknown";
}

}  // namespace scratchbird::wire
