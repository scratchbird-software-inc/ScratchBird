// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "physical_columnar_zone.hpp"

#include "index_key_encoding.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <cmath>
#include <map>
#include <set>
#include <utility>

namespace scratchbird::core::index {
namespace {

using scratchbird::core::platform::DiagnosticArgument;
using scratchbird::core::platform::MakeDiagnostic;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;

inline constexpr std::array<byte, 8> kMagic = {'S', 'B', 'P', 'C', 'Z', 'O', 'N', '1'};
inline constexpr u32 kHeaderBytes = 24;
inline constexpr u64 kFnvOffset = 14695981039346656037ull;
inline constexpr u64 kFnvPrime = 1099511628211ull;

enum class SegmentValidationClass {
  valid,
  malformed_column,
  malformed_row_group,
  unsafe_encoding,
  invalid_identity,
  invalid_generation,
  authority_claimed
};

Status OkStatus() { return {StatusCode::ok, Severity::info, Subsystem::engine}; }
Status WarnStatus() { return {StatusCode::ok, Severity::warning, Subsystem::engine}; }
Status ErrorStatus() {
  return {StatusCode::platform_required_feature_missing, Severity::error, Subsystem::engine};
}

u64 NextGeneration(u64 value) {
  return value == 0 ? 1 : value + 1;
}

bool SameFormatVersion(PageExtentSummaryFormatVersion left,
                       PageExtentSummaryFormatVersion right) {
  return left.major == right.major && left.minor == right.minor;
}

bool KeyLess(std::string_view left, std::string_view right) {
  const auto compared = CompareEncodedIndexKeyBytes(left, right);
  return compared.ok() && compared.comparison < 0;
}

bool KeyGreater(std::string_view left, std::string_view right) {
  const auto compared = CompareEncodedIndexKeyBytes(left, right);
  return compared.ok() && compared.comparison > 0;
}

bool EncodedKeySafe(std::string_view value) {
  return !value.empty() &&
         IsOrderPreservingIndexKeyEncoding(value) &&
         !IsUnsafeLegacyIndexKeyEncoding(value);
}

bool AuthorityClean(const PhysicalColumnarZoneRowEvidence& row) {
  return row.authoritative_columnar_page_evidence &&
         !row.parser_finality_authority_claimed &&
         !row.reference_finality_authority_claimed &&
         !row.provider_finality_authority_claimed &&
         !row.write_ahead_log_finality_authority_claimed &&
         !row.summary_visibility_authority_claimed &&
         !row.summary_finality_authority_claimed;
}

bool ColumnValueValid(const PhysicalColumnarZoneColumnValueEvidence& value) {
  if (value.scalar_type_key.empty()) {
    return false;
  }
  return value.value_is_null || EncodedKeySafe(value.encoded_scalar);
}

bool RowEvidenceValid(const PhysicalColumnarZoneRowEvidence& row) {
  if (row.base_generation == 0 || !AuthorityClean(row)) {
    return false;
  }
  std::set<u32> ordinals;
  for (const auto& column : row.columns) {
    if (!ColumnValueValid(column) ||
        !ordinals.insert(column.column_ordinal).second) {
      return false;
    }
  }
  return true;
}

bool CompressionPolicyValid(const PhysicalColumnarZoneCompressionPolicy& policy) {
  return !policy.scalar_type_key.empty() &&
         !policy.codec_id.empty() &&
         policy.uncompressed_bytes > 0 &&
         policy.compressed_bytes > 0 &&
         std::isfinite(policy.estimated_cpu_cost) &&
         std::isfinite(policy.estimated_read_cost) &&
         std::isfinite(policy.estimated_write_cost) &&
         policy.estimated_cpu_cost >= 0.0 &&
         policy.estimated_read_cost >= 0.0 &&
         policy.estimated_write_cost >= 0.0 &&
         policy.exact_fallback_equivalence &&
         policy.storage_runtime_evidence_only &&
         !policy.compression_value_authority_claimed;
}

bool ColumnSummaryEncodingSafe(const PhysicalColumnarZoneColumnSummary& summary) {
  const bool has_non_null = summary.row_count > summary.null_count;
  if (!has_non_null) {
    return summary.scalar_type_key.empty() &&
           !summary.min_present &&
           !summary.max_present &&
           summary.encoded_min.empty() &&
           summary.encoded_max.empty();
  }
  if (summary.scalar_type_key.empty() ||
      !summary.min_present ||
      !summary.max_present ||
      !EncodedKeySafe(summary.encoded_min) ||
      !EncodedKeySafe(summary.encoded_max) ||
      KeyGreater(summary.encoded_min, summary.encoded_max)) {
    return false;
  }
  for (const auto& value : summary.dictionary_values) {
    if (!EncodedKeySafe(value)) {
      return false;
    }
  }
  return true;
}

bool ColumnSummaryValid(const PhysicalColumnarZoneColumnSummary& summary,
                        SegmentValidationClass* validation) {
  if (summary.null_count > summary.row_count ||
      (summary.dictionary_exact && summary.dictionary_overflow) ||
      (!summary.dictionary_exact && !summary.dictionary_values.empty())) {
    *validation = SegmentValidationClass::malformed_column;
    return false;
  }
  if (!ColumnSummaryEncodingSafe(summary)) {
    *validation = SegmentValidationClass::unsafe_encoding;
    return false;
  }
  return true;
}

bool BoundaryValid(const PhysicalColumnarZoneRowGroupBoundary& boundary) {
  return boundary.page_count > 0 && boundary.row_count > 0;
}

bool RowGroupValid(const PhysicalColumnarZoneRowGroupSummary& group,
                   SegmentValidationClass* validation) {
  if (!BoundaryValid(group.boundary) ||
      group.row_count == 0 ||
      group.deleted_row_count > group.row_count ||
      group.boundary.row_count != group.row_count ||
      group.base_generation == 0 ||
      group.summary_generation == 0 ||
      group.status != PageExtentSummaryStatus::current ||
      !group.persisted_record_present ||
      !group.checksum_valid ||
      !group.mga_recheck_required ||
      !group.security_recheck_required ||
      !group.exact_recheck_required ||
      !std::is_sorted(group.candidate_row_ordinals.begin(),
                      group.candidate_row_ordinals.end()) ||
      std::adjacent_find(group.candidate_row_ordinals.begin(),
                         group.candidate_row_ordinals.end()) !=
          group.candidate_row_ordinals.end()) {
    *validation = SegmentValidationClass::malformed_row_group;
    return false;
  }
  const u64 candidate_count = group.row_count - group.deleted_row_count;
  if (group.candidate_row_ordinals.size() != candidate_count) {
    *validation = SegmentValidationClass::malformed_row_group;
    return false;
  }
  const u64 first_candidate = group.boundary.first_row_ordinal;
  const u64 last_candidate_exclusive =
      group.boundary.first_row_ordinal + group.boundary.row_count;
  for (u64 ordinal : group.candidate_row_ordinals) {
    if (ordinal < first_candidate || ordinal >= last_candidate_exclusive) {
      *validation = SegmentValidationClass::malformed_row_group;
      return false;
    }
  }
  std::set<u32> ordinals;
  for (const auto& column : group.columns) {
    if (!ordinals.insert(column.column_ordinal).second ||
        !ColumnSummaryValid(column, validation)) {
      return false;
    }
  }
  return true;
}

bool CompressionPoliciesMatchColumns(
    const std::vector<PhysicalColumnarZoneRowEvidence>& rows,
    const std::vector<PhysicalColumnarZoneCompressionPolicy>& policies) {
  std::map<u32, std::string> column_types;
  for (const auto& row : rows) {
    for (const auto& column : row.columns) {
      auto inserted = column_types.emplace(column.column_ordinal,
                                           column.scalar_type_key);
      if (!inserted.second && inserted.first->second != column.scalar_type_key) {
        return false;
      }
    }
  }
  std::map<u32, std::string> policy_types;
  for (const auto& policy : policies) {
    policy_types.emplace(policy.column_ordinal, policy.scalar_type_key);
  }
  return policy_types == column_types;
}

std::vector<u64> CandidatePages(const PhysicalColumnarZoneRowGroupBoundary& boundary);

const PhysicalColumnarZoneRowGroupSummary* FindGroup(
    const PhysicalColumnarZoneSegment& segment,
    u64 row_group_id) {
  auto iter = std::find_if(segment.row_groups.begin(),
                           segment.row_groups.end(),
                           [row_group_id](const PhysicalColumnarZoneRowGroupSummary& group) {
                             return group.boundary.row_group_id == row_group_id;
                           });
  return iter == segment.row_groups.end() ? nullptr : &(*iter);
}

bool BoundaryEqual(const PhysicalColumnarZoneRowGroupBoundary& left,
                   const PhysicalColumnarZoneRowGroupBoundary& right) {
  return left.row_group_id == right.row_group_id &&
         left.first_page_id == right.first_page_id &&
         left.page_count == right.page_count &&
         left.first_row_ordinal == right.first_row_ordinal &&
         left.row_count == right.row_count;
}

bool CandidateGroupMatchesSegment(
    const PhysicalColumnarZoneSegment& segment,
    const PhysicalColumnarZoneCandidateGroup& candidate) {
  if (candidate.segment_uuid != segment.segment_uuid ||
      !candidate.mga_recheck_required ||
      !candidate.security_recheck_required ||
      !candidate.exact_recheck_required) {
    return false;
  }
  const auto* group = FindGroup(segment, candidate.boundary.row_group_id);
  if (group == nullptr || !BoundaryEqual(group->boundary, candidate.boundary)) {
    return false;
  }
  if (!std::is_sorted(candidate.candidate_row_ordinals.begin(),
                      candidate.candidate_row_ordinals.end()) ||
      std::adjacent_find(candidate.candidate_row_ordinals.begin(),
                         candidate.candidate_row_ordinals.end()) !=
          candidate.candidate_row_ordinals.end()) {
    return false;
  }
  for (u64 ordinal : candidate.candidate_row_ordinals) {
    if (!std::binary_search(group->candidate_row_ordinals.begin(),
                            group->candidate_row_ordinals.end(),
                            ordinal)) {
      return false;
    }
  }
  const auto expected_pages = CandidatePages(group->boundary);
  if (candidate.scan_required &&
      candidate.decision != PhysicalColumnarZonePruneDecision::scan_with_recheck &&
      candidate.decision !=
          PhysicalColumnarZonePruneDecision::exact_dictionary_overflow_scan) {
    return false;
  }
  if (candidate.scan_required && candidate.candidate_page_ids != expected_pages) {
    return false;
  }
  if (!candidate.scan_required &&
      (!candidate.candidate_page_ids.empty() ||
       !candidate.candidate_row_ordinals.empty() ||
       (candidate.decision != PhysicalColumnarZonePruneDecision::pruned_by_min_max &&
        candidate.decision !=
            PhysicalColumnarZonePruneDecision::pruned_by_exact_dictionary))) {
    return false;
  }
  return true;
}

SegmentValidationClass ValidateSegment(const PhysicalColumnarZoneSegment& segment) {
  if (!PageExtentSummaryUuidTextValid(segment.relation_uuid) ||
      !PageExtentSummaryUuidTextValid(segment.index_uuid) ||
      !PageExtentSummaryUuidTextValid(segment.segment_uuid)) {
    return SegmentValidationClass::invalid_identity;
  }
  if (!SameFormatVersion(segment.format_version,
                         {kPhysicalColumnarZoneCurrentMajor,
                          kPhysicalColumnarZoneCurrentMinor}) ||
      segment.base_generation == 0 ||
      segment.summary_generation == 0) {
    return SegmentValidationClass::invalid_generation;
  }
  if (!segment.mga_recheck_required ||
      !segment.security_recheck_required ||
      !segment.exact_recheck_required ||
      segment.visibility_finality_authority ||
      segment.parser_finality_authority_claimed ||
      segment.reference_finality_authority_claimed ||
      segment.provider_finality_authority_claimed ||
      segment.write_ahead_log_finality_authority_claimed) {
    return SegmentValidationClass::authority_claimed;
  }
  u64 last_group = 0;
  bool seen_group = false;
  for (const auto& group : segment.row_groups) {
    SegmentValidationClass validation = SegmentValidationClass::valid;
    if (!RowGroupValid(group, &validation)) {
      return validation;
    }
    if (seen_group && group.boundary.row_group_id <= last_group) {
      return SegmentValidationClass::malformed_row_group;
    }
    last_group = group.boundary.row_group_id;
    seen_group = true;
  }
  std::set<u32> policy_ordinals;
  for (const auto& policy : segment.compression_policies) {
    if (!CompressionPolicyValid(policy) ||
        !policy_ordinals.insert(policy.column_ordinal).second) {
      return SegmentValidationClass::malformed_column;
    }
  }
  return SegmentValidationClass::valid;
}

bool SegmentValid(const PhysicalColumnarZoneSegment& segment) {
  return ValidateSegment(segment) == SegmentValidationClass::valid;
}

PhysicalColumnarZoneBuildResult BuildFailure(std::string code,
                                             std::string key,
                                             std::string detail = {}) {
  PhysicalColumnarZoneBuildResult result;
  result.status = ErrorStatus();
  result.diagnostic = MakePhysicalColumnarZoneDiagnostic(
      result.status, std::move(code), std::move(key), std::move(detail));
  return result;
}

PhysicalColumnarZoneMutationResult MutationFailure(
    PhysicalColumnarZoneSegment segment,
    std::string code,
    std::string key,
    std::string detail = {}) {
  PhysicalColumnarZoneMutationResult result;
  result.status = WarnStatus();
  result.segment = std::move(segment);
  result.segment_invalidated = true;
  result.scan_required = true;
  result.diagnostic = MakePhysicalColumnarZoneDiagnostic(
      result.status, std::move(code), std::move(key), std::move(detail));
  return result;
}

void SortColumnarZoneSegment(PhysicalColumnarZoneSegment* segment) {
  std::sort(segment->row_groups.begin(),
            segment->row_groups.end(),
            [](const PhysicalColumnarZoneRowGroupSummary& left,
               const PhysicalColumnarZoneRowGroupSummary& right) {
              return left.boundary.row_group_id < right.boundary.row_group_id;
            });
  for (auto& group : segment->row_groups) {
    std::sort(group.candidate_row_ordinals.begin(),
              group.candidate_row_ordinals.end());
    std::sort(group.columns.begin(),
              group.columns.end(),
              [](const PhysicalColumnarZoneColumnSummary& left,
                 const PhysicalColumnarZoneColumnSummary& right) {
                return left.column_ordinal < right.column_ordinal;
              });
    for (auto& column : group.columns) {
      std::sort(column.dictionary_values.begin(),
                column.dictionary_values.end(),
                [](const std::string& left, const std::string& right) {
                  return KeyLess(left, right);
                });
    }
  }
  std::sort(segment->compression_policies.begin(),
            segment->compression_policies.end(),
            [](const PhysicalColumnarZoneCompressionPolicy& left,
               const PhysicalColumnarZoneCompressionPolicy& right) {
              return left.column_ordinal < right.column_ordinal;
            });
  std::sort(segment->evidence.begin(), segment->evidence.end());
}

void AddDictionaryValue(PhysicalColumnarZoneColumnSummary* summary,
                        const std::string& encoded_scalar,
                        u32 dictionary_limit) {
  if (!summary->dictionary_exact) {
    return;
  }
  if (std::find(summary->dictionary_values.begin(),
                summary->dictionary_values.end(),
                encoded_scalar) != summary->dictionary_values.end()) {
    return;
  }
  if (summary->dictionary_values.size() >= dictionary_limit) {
    summary->dictionary_exact = false;
    summary->dictionary_overflow = true;
    summary->dictionary_values.clear();
    return;
  }
  summary->dictionary_values.push_back(encoded_scalar);
}

bool SameScalarTypeOrUnset(const PhysicalColumnarZoneColumnSummary& summary,
                           const PhysicalColumnarZoneColumnValueEvidence& value) {
  return summary.scalar_type_key.empty() ||
         summary.scalar_type_key == value.scalar_type_key;
}

bool ApplyColumnValue(PhysicalColumnarZoneColumnSummary* summary,
                      const PhysicalColumnarZoneColumnValueEvidence& value,
                      u32 dictionary_limit) {
  if (!SameScalarTypeOrUnset(*summary, value)) {
    return false;
  }
  ++summary->row_count;
  if (value.value_is_null) {
    ++summary->null_count;
    return true;
  }
  if (summary->scalar_type_key.empty()) {
    summary->scalar_type_key = value.scalar_type_key;
  }
  if (!summary->min_present || KeyLess(value.encoded_scalar, summary->encoded_min)) {
    summary->encoded_min = value.encoded_scalar;
    summary->min_present = true;
  }
  if (!summary->max_present || KeyGreater(value.encoded_scalar, summary->encoded_max)) {
    summary->encoded_max = value.encoded_scalar;
    summary->max_present = true;
  }
  AddDictionaryValue(summary, value.encoded_scalar, dictionary_limit);
  return true;
}

PhysicalColumnarZoneRowGroupSummary SeedRowGroup(u64 row_group_id,
                                                 u64 base_generation,
                                                 u64 summary_generation) {
  PhysicalColumnarZoneRowGroupSummary group;
  group.boundary.row_group_id = row_group_id;
  group.boundary.first_page_id = 0;
  group.boundary.page_count = 0;
  group.boundary.first_row_ordinal = 0;
  group.boundary.row_count = 0;
  group.base_generation = base_generation;
  group.summary_generation = summary_generation;
  group.status = PageExtentSummaryStatus::current;
  group.persisted_record_present = true;
  group.checksum_valid = true;
  return group;
}

bool ApplyRowToGroup(PhysicalColumnarZoneRowGroupSummary* group,
                     const PhysicalColumnarZoneRowEvidence& row,
                     u32 dictionary_limit) {
  if (group->row_count == 0) {
    group->boundary.first_page_id = row.page_id;
    group->boundary.first_row_ordinal = row.row_ordinal;
  } else {
    group->boundary.first_page_id =
        std::min(group->boundary.first_page_id, row.page_id);
    group->boundary.first_row_ordinal =
        std::min(group->boundary.first_row_ordinal, row.row_ordinal);
  }
  const u64 last_page =
      std::max(group->boundary.first_page_id +
                   static_cast<u64>(group->boundary.page_count),
               row.page_id + 1);
  group->boundary.page_count =
      static_cast<u32>(last_page - group->boundary.first_page_id);
  ++group->row_count;
  group->boundary.row_count = group->row_count;
  group->base_generation = std::max(group->base_generation, row.base_generation);
  if (row.physically_deleted) {
    ++group->deleted_row_count;
    return true;
  }
  group->candidate_row_ordinals.push_back(row.row_ordinal);
  for (const auto& value : row.columns) {
    auto iter = std::find_if(group->columns.begin(),
                             group->columns.end(),
                             [&](const PhysicalColumnarZoneColumnSummary& column) {
                               return column.column_ordinal == value.column_ordinal;
                             });
    if (iter == group->columns.end()) {
      PhysicalColumnarZoneColumnSummary summary;
      summary.column_ordinal = value.column_ordinal;
      group->columns.push_back(std::move(summary));
      iter = std::prev(group->columns.end());
    }
    if (!ApplyColumnValue(&(*iter), value, dictionary_limit)) {
      return false;
    }
  }
  return true;
}

bool PredicateValueOverlap(const PhysicalColumnarZonePredicate& predicate,
                           const std::string& encoded_value) {
  if (predicate.lower_present && KeyLess(encoded_value, predicate.encoded_lower)) {
    return false;
  }
  if (predicate.upper_present && KeyGreater(encoded_value, predicate.encoded_upper)) {
    return false;
  }
  return true;
}

bool PredicateOutsideMinMax(const PhysicalColumnarZonePredicate& predicate,
                            const PhysicalColumnarZoneColumnSummary& summary) {
  if (summary.row_count == summary.null_count) {
    return true;
  }
  if (predicate.lower_present && KeyGreater(predicate.encoded_lower, summary.encoded_max)) {
    return true;
  }
  if (predicate.upper_present && KeyLess(predicate.encoded_upper, summary.encoded_min)) {
    return true;
  }
  return false;
}

const PhysicalColumnarZoneColumnSummary* FindColumn(
    const PhysicalColumnarZoneRowGroupSummary& group,
    u32 ordinal) {
  auto iter = std::find_if(group.columns.begin(),
                           group.columns.end(),
                           [ordinal](const PhysicalColumnarZoneColumnSummary& column) {
                             return column.column_ordinal == ordinal;
                           });
  return iter == group.columns.end() ? nullptr : &(*iter);
}

PhysicalColumnarZoneRowGroupSummary* FindGroup(
    PhysicalColumnarZoneSegment* segment,
    u64 row_group_id) {
  auto iter = std::find_if(segment->row_groups.begin(),
                           segment->row_groups.end(),
                           [row_group_id](const PhysicalColumnarZoneRowGroupSummary& group) {
                             return group.boundary.row_group_id == row_group_id;
                           });
  return iter == segment->row_groups.end() ? nullptr : &(*iter);
}

bool RowTouchesBoundaryDictionaryOrCandidate(
    const PhysicalColumnarZoneRowGroupSummary& group,
    const PhysicalColumnarZoneRowEvidence& row) {
  if (std::find(group.candidate_row_ordinals.begin(),
                group.candidate_row_ordinals.end(),
                row.row_ordinal) != group.candidate_row_ordinals.end()) {
    return true;
  }
  for (const auto& value : row.columns) {
    if (value.value_is_null) {
      continue;
    }
    const auto* summary = FindColumn(group, value.column_ordinal);
    if (summary == nullptr) {
      continue;
    }
    if ((summary->min_present && summary->encoded_min == value.encoded_scalar) ||
        (summary->max_present && summary->encoded_max == value.encoded_scalar)) {
      return true;
    }
    if (summary->dictionary_exact &&
        std::find(summary->dictionary_values.begin(),
                  summary->dictionary_values.end(),
                  value.encoded_scalar) != summary->dictionary_values.end()) {
      return true;
    }
  }
  return false;
}

std::vector<u64> CandidatePages(const PhysicalColumnarZoneRowGroupBoundary& boundary) {
  std::vector<u64> pages;
  pages.reserve(boundary.page_count);
  for (u32 index = 0; index < boundary.page_count; ++index) {
    pages.push_back(boundary.first_page_id + index);
  }
  return pages;
}

void AppendU8(std::vector<byte>* out, byte value) {
  out->push_back(value);
}

void AppendU32(std::vector<byte>* out, u32 value) {
  const std::size_t offset = out->size();
  out->resize(offset + sizeof(u32));
  scratchbird::core::platform::StoreLittle32(out->data() + offset, value);
}

void AppendU64(std::vector<byte>* out, u64 value) {
  const std::size_t offset = out->size();
  out->resize(offset + sizeof(u64));
  scratchbird::core::platform::StoreLittle64(out->data() + offset, value);
}

void AppendDouble(std::vector<byte>* out, double value) {
  static_assert(sizeof(double) == sizeof(u64), "unexpected double size");
  u64 bits = 0;
  std::memcpy(&bits, &value, sizeof(bits));
  AppendU64(out, bits);
}

void AppendString(std::vector<byte>* out, std::string_view value) {
  AppendU32(out, static_cast<u32>(value.size()));
  out->insert(out->end(), value.begin(), value.end());
}

u64 ComputeChecksum(std::vector<byte> bytes) {
  if (bytes.size() >= kHeaderBytes) {
    scratchbird::core::platform::StoreLittle64(bytes.data() + 16, 0);
  }
  u64 hash = kFnvOffset;
  for (byte value : bytes) {
    hash ^= value;
    hash *= kFnvPrime;
  }
  return hash == 0 ? 1 : hash;
}

class Reader {
 public:
  explicit Reader(const std::vector<byte>& bytes) : bytes_(bytes) {}

  bool ReadU8(byte* out) {
    if (offset_ + 1 > bytes_.size()) {
      return false;
    }
    *out = bytes_[offset_++];
    return true;
  }

  bool ReadU32(u32* out) {
    if (offset_ + sizeof(u32) > bytes_.size()) {
      return false;
    }
    *out = scratchbird::core::platform::LoadLittle32(bytes_.data() + offset_);
    offset_ += sizeof(u32);
    return true;
  }

  bool ReadU64(u64* out) {
    if (offset_ + sizeof(u64) > bytes_.size()) {
      return false;
    }
    *out = scratchbird::core::platform::LoadLittle64(bytes_.data() + offset_);
    offset_ += sizeof(u64);
    return true;
  }

  bool ReadDouble(double* out) {
    u64 bits = 0;
    if (!ReadU64(&bits)) {
      return false;
    }
    std::memcpy(out, &bits, sizeof(bits));
    return true;
  }

  bool ReadString(std::string* out) {
    u32 size = 0;
    if (!ReadU32(&size) || offset_ + size > bytes_.size()) {
      return false;
    }
    out->assign(reinterpret_cast<const char*>(bytes_.data() + offset_), size);
    offset_ += size;
    return true;
  }

  bool Done() const { return offset_ == bytes_.size(); }
  void SetOffset(std::size_t offset) { offset_ = offset; }

 private:
  const std::vector<byte>& bytes_;
  std::size_t offset_ = 0;
};

PhysicalColumnarZoneOpenResult OpenFailure(PhysicalColumnarZoneOpenClass open_class,
                                           std::string code,
                                           std::string key,
                                           std::string detail = {}) {
  PhysicalColumnarZoneOpenResult result;
  result.status = open_class == PhysicalColumnarZoneOpenClass::stale_generation ||
                          open_class == PhysicalColumnarZoneOpenClass::stale_format
                      ? WarnStatus()
                      : ErrorStatus();
  result.open_class = open_class;
  result.scan_required = true;
  result.rebuild_required = true;
  result.restricted_repair_required =
      open_class == PhysicalColumnarZoneOpenClass::bad_checksum ||
      open_class == PhysicalColumnarZoneOpenClass::malformed_column_payload ||
      open_class == PhysicalColumnarZoneOpenClass::malformed_row_group_payload ||
      open_class == PhysicalColumnarZoneOpenClass::unsafe_encoding ||
      open_class == PhysicalColumnarZoneOpenClass::truncated_payload ||
      open_class == PhysicalColumnarZoneOpenClass::corrupt_payload;
  result.diagnostic = MakePhysicalColumnarZoneDiagnostic(
      result.status, std::move(code), std::move(key), std::move(detail));
  result.actions.push_back("physical_columnar_zone_scan_fallback");
  if (result.restricted_repair_required) {
    result.actions.push_back("repair_requires_admitted_authoritative_columnar_page_evidence");
  }
  return result;
}

PhysicalColumnarZoneOpenClass OpenClassForValidation(SegmentValidationClass validation) {
  switch (validation) {
    case SegmentValidationClass::valid:
      return PhysicalColumnarZoneOpenClass::current;
    case SegmentValidationClass::malformed_column:
      return PhysicalColumnarZoneOpenClass::malformed_column_payload;
    case SegmentValidationClass::malformed_row_group:
      return PhysicalColumnarZoneOpenClass::malformed_row_group_payload;
    case SegmentValidationClass::unsafe_encoding:
      return PhysicalColumnarZoneOpenClass::unsafe_encoding;
    case SegmentValidationClass::invalid_identity:
      return PhysicalColumnarZoneOpenClass::identity_mismatch;
    case SegmentValidationClass::invalid_generation:
      return PhysicalColumnarZoneOpenClass::stale_generation;
    case SegmentValidationClass::authority_claimed:
      return PhysicalColumnarZoneOpenClass::refused;
  }
  return PhysicalColumnarZoneOpenClass::corrupt_payload;
}

const char* MessageKeyForOpenClass(PhysicalColumnarZoneOpenClass open_class) {
  switch (open_class) {
    case PhysicalColumnarZoneOpenClass::malformed_column_payload:
      return "index.physical_columnar_zone.malformed_column_payload";
    case PhysicalColumnarZoneOpenClass::malformed_row_group_payload:
      return "index.physical_columnar_zone.malformed_row_group_payload";
    case PhysicalColumnarZoneOpenClass::unsafe_encoding:
      return "index.physical_columnar_zone.unsafe_encoding";
    case PhysicalColumnarZoneOpenClass::identity_mismatch:
      return "index.physical_columnar_zone.identity_mismatch";
    case PhysicalColumnarZoneOpenClass::stale_generation:
      return "index.physical_columnar_zone.stale_generation";
    case PhysicalColumnarZoneOpenClass::refused:
      return "index.physical_columnar_zone.authority_refused";
    default:
      return "index.physical_columnar_zone.invalid_payload";
  }
}

std::string CodeForOpenClass(PhysicalColumnarZoneOpenClass open_class) {
  switch (open_class) {
    case PhysicalColumnarZoneOpenClass::malformed_column_payload:
      return "INDEX.PHYSICAL_COLUMNAR_ZONE.MALFORMED_COLUMN_PAYLOAD";
    case PhysicalColumnarZoneOpenClass::malformed_row_group_payload:
      return "INDEX.PHYSICAL_COLUMNAR_ZONE.MALFORMED_ROW_GROUP_PAYLOAD";
    case PhysicalColumnarZoneOpenClass::unsafe_encoding:
      return "INDEX.PHYSICAL_COLUMNAR_ZONE.UNSAFE_ENCODING";
    case PhysicalColumnarZoneOpenClass::identity_mismatch:
      return "INDEX.PHYSICAL_COLUMNAR_ZONE.IDENTITY_MISMATCH";
    case PhysicalColumnarZoneOpenClass::stale_generation:
      return "INDEX.PHYSICAL_COLUMNAR_ZONE.STALE_GENERATION";
    case PhysicalColumnarZoneOpenClass::refused:
      return "INDEX.PHYSICAL_COLUMNAR_ZONE.AUTHORITY_REFUSED";
    default:
      return "INDEX.PHYSICAL_COLUMNAR_ZONE.INVALID_PAYLOAD";
  }
}

PhysicalColumnarZoneBuildRequest RebuildRequestForSegment(
    const PhysicalColumnarZoneSegment& segment,
    std::vector<PhysicalColumnarZoneRowEvidence> rows,
    std::vector<PhysicalColumnarZoneCompressionPolicy> policies,
    u32 dictionary_limit) {
  PhysicalColumnarZoneBuildRequest request;
  request.relation_uuid = segment.relation_uuid;
  request.index_uuid = segment.index_uuid;
  request.segment_uuid = segment.segment_uuid;
  request.base_generation = segment.base_generation;
  request.summary_generation = NextGeneration(segment.summary_generation);
  request.dictionary_limit = dictionary_limit;
  request.rows = std::move(rows);
  request.compression_policies = std::move(policies);
  return request;
}

}  // namespace

PhysicalColumnarZoneBuildResult BuildPhysicalColumnarZoneFromPageEvidence(
    const PhysicalColumnarZoneBuildRequest& request) {
  if (!PageExtentSummaryUuidTextValid(request.relation_uuid) ||
      !PageExtentSummaryUuidTextValid(request.index_uuid) ||
      !PageExtentSummaryUuidTextValid(request.segment_uuid)) {
    return BuildFailure("INDEX.PHYSICAL_COLUMNAR_ZONE.INVALID_IDENTITY",
                        "index.physical_columnar_zone.invalid_identity");
  }
  if (request.base_generation == 0 || request.summary_generation == 0) {
    return BuildFailure("INDEX.PHYSICAL_COLUMNAR_ZONE.MISSING_BASE_GENERATION",
                        "index.physical_columnar_zone.missing_base_generation");
  }
  if (request.rows.empty()) {
    return BuildFailure("INDEX.PHYSICAL_COLUMNAR_ZONE.MISSING_PAGE_EVIDENCE",
                        "index.physical_columnar_zone.missing_page_evidence");
  }

  std::set<u32> row_column_ordinals;
  for (const auto& row : request.rows) {
    if (!RowEvidenceValid(row)) {
      return BuildFailure("INDEX.PHYSICAL_COLUMNAR_ZONE.INVALID_PAGE_EVIDENCE",
                          "index.physical_columnar_zone.invalid_page_evidence");
    }
    for (const auto& column : row.columns) {
      row_column_ordinals.insert(column.column_ordinal);
    }
  }

  std::set<u32> policy_ordinals;
  for (const auto& policy : request.compression_policies) {
    if (!CompressionPolicyValid(policy) ||
        !policy_ordinals.insert(policy.column_ordinal).second) {
      return BuildFailure("INDEX.PHYSICAL_COLUMNAR_ZONE.INVALID_COMPRESSION_POLICY",
                          "index.physical_columnar_zone.invalid_compression_policy");
    }
  }
  for (u32 ordinal : row_column_ordinals) {
    if (policy_ordinals.find(ordinal) == policy_ordinals.end()) {
      return BuildFailure("INDEX.PHYSICAL_COLUMNAR_ZONE.MISSING_COMPRESSION_POLICY",
                          "index.physical_columnar_zone.missing_compression_policy");
    }
  }
  if (!CompressionPoliciesMatchColumns(request.rows, request.compression_policies)) {
    return BuildFailure("INDEX.PHYSICAL_COLUMNAR_ZONE.COMPRESSION_POLICY_TYPE_MISMATCH",
                        "index.physical_columnar_zone.compression_policy_type_mismatch");
  }

  PhysicalColumnarZoneSegment segment;
  segment.relation_uuid = request.relation_uuid;
  segment.index_uuid = request.index_uuid;
  segment.segment_uuid = request.segment_uuid;
  segment.base_generation = request.base_generation;
  segment.summary_generation = request.summary_generation;
  segment.compression_policies = request.compression_policies;
  segment.evidence.push_back("built_from_authoritative_columnar_page_evidence");
  segment.evidence.push_back("columnar_zone_candidate_pruning_only=true");
  segment.evidence.push_back("mga_recheck_required=true");
  segment.evidence.push_back("security_recheck_required=true");
  segment.evidence.push_back("exact_recheck_required=true");
  segment.evidence.push_back("summary_visibility_finality_authority=false");
  segment.evidence.push_back("parser_reference_provider_finality_authority=false");
  segment.evidence.push_back("write_ahead_log_finality_authority=false");
  segment.evidence.push_back("compression_policy_storage_runtime_evidence_only=true");
  segment.evidence.push_back("compression_exact_fallback_equivalence_required=true");

  std::map<u64, PhysicalColumnarZoneRowGroupSummary> groups;
  std::vector<PhysicalColumnarZoneRowEvidence> rows = request.rows;
  std::sort(rows.begin(),
            rows.end(),
            [](const PhysicalColumnarZoneRowEvidence& left,
               const PhysicalColumnarZoneRowEvidence& right) {
              if (left.row_group_id != right.row_group_id) {
                return left.row_group_id < right.row_group_id;
              }
              if (left.page_id != right.page_id) {
                return left.page_id < right.page_id;
              }
              return left.row_ordinal < right.row_ordinal;
            });

  for (const auto& row : rows) {
    segment.base_generation = std::max(segment.base_generation, row.base_generation);
    auto inserted = groups.emplace(
        row.row_group_id,
        SeedRowGroup(row.row_group_id,
                     request.base_generation,
                     request.summary_generation));
    if (!ApplyRowToGroup(&inserted.first->second, row, request.dictionary_limit)) {
      return BuildFailure("INDEX.PHYSICAL_COLUMNAR_ZONE.COLUMN_TYPE_MISMATCH",
                          "index.physical_columnar_zone.column_type_mismatch");
    }
  }

  for (auto& entry : groups) {
    auto& group = entry.second;
    group.base_generation = segment.base_generation;
    SegmentValidationClass group_validation = SegmentValidationClass::valid;
    if (!RowGroupValid(group, &group_validation)) {
      return BuildFailure("INDEX.PHYSICAL_COLUMNAR_ZONE.INVALID_ROW_GROUP",
                          "index.physical_columnar_zone.invalid_row_group");
    }
    segment.row_groups.push_back(std::move(group));
  }
  SortColumnarZoneSegment(&segment);

  if (!SegmentValid(segment)) {
    return BuildFailure("INDEX.PHYSICAL_COLUMNAR_ZONE.INVALID_SEGMENT",
                        "index.physical_columnar_zone.invalid_segment");
  }

  PhysicalColumnarZoneBuildResult result;
  result.status = OkStatus();
  result.segment = std::move(segment);
  result.built = true;
  result.evidence = result.segment.evidence;
  result.diagnostic = MakePhysicalColumnarZoneDiagnostic(
      result.status,
      "INDEX.PHYSICAL_COLUMNAR_ZONE.BUILT",
      "index.physical_columnar_zone.built");
  return result;
}

PhysicalColumnarZoneSerializeResult SerializePhysicalColumnarZoneSegment(
    const PhysicalColumnarZoneSegment& input_segment) {
  PhysicalColumnarZoneSerializeResult result;
  auto segment = input_segment;
  SortColumnarZoneSegment(&segment);
  if (!SegmentValid(segment)) {
    result.status = ErrorStatus();
    result.diagnostic = MakePhysicalColumnarZoneDiagnostic(
        result.status,
        "INDEX.PHYSICAL_COLUMNAR_ZONE.SERIALIZE_REFUSED",
        "index.physical_columnar_zone.serialize_refused");
    return result;
  }

  auto& out = result.bytes;
  out.insert(out.end(), kMagic.begin(), kMagic.end());
  AppendU32(&out, segment.format_version.major);
  AppendU32(&out, segment.format_version.minor);
  AppendU64(&out, 0);
  AppendString(&out, segment.relation_uuid);
  AppendString(&out, segment.index_uuid);
  AppendString(&out, segment.segment_uuid);
  AppendU64(&out, segment.base_generation);
  AppendU64(&out, segment.summary_generation);
  AppendU8(&out, segment.mga_recheck_required ? 1 : 0);
  AppendU8(&out, segment.security_recheck_required ? 1 : 0);
  AppendU8(&out, segment.exact_recheck_required ? 1 : 0);
  AppendU8(&out, segment.visibility_finality_authority ? 1 : 0);
  AppendU8(&out, segment.parser_finality_authority_claimed ? 1 : 0);
  AppendU8(&out, segment.reference_finality_authority_claimed ? 1 : 0);
  AppendU8(&out, segment.provider_finality_authority_claimed ? 1 : 0);
  AppendU8(&out, segment.write_ahead_log_finality_authority_claimed ? 1 : 0);

  AppendU32(&out, static_cast<u32>(segment.row_groups.size()));
  for (const auto& group : segment.row_groups) {
    AppendU64(&out, group.boundary.row_group_id);
    AppendU64(&out, group.boundary.first_page_id);
    AppendU32(&out, group.boundary.page_count);
    AppendU64(&out, group.boundary.first_row_ordinal);
    AppendU64(&out, group.boundary.row_count);
    AppendU64(&out, group.row_count);
    AppendU64(&out, group.deleted_row_count);
    AppendU64(&out, group.base_generation);
    AppendU64(&out, group.summary_generation);
    AppendU32(&out, static_cast<u32>(group.status));
    AppendU8(&out, group.persisted_record_present ? 1 : 0);
    AppendU8(&out, group.checksum_valid ? 1 : 0);
    AppendU8(&out, group.mga_recheck_required ? 1 : 0);
    AppendU8(&out, group.security_recheck_required ? 1 : 0);
    AppendU8(&out, group.exact_recheck_required ? 1 : 0);
    AppendU32(&out, static_cast<u32>(group.candidate_row_ordinals.size()));
    for (u64 row_ordinal : group.candidate_row_ordinals) {
      AppendU64(&out, row_ordinal);
    }
    AppendU32(&out, static_cast<u32>(group.columns.size()));
    for (const auto& column : group.columns) {
      AppendU32(&out, column.column_ordinal);
      AppendString(&out, column.scalar_type_key);
      AppendU64(&out, column.row_count);
      AppendU64(&out, column.null_count);
      AppendU8(&out, column.min_present ? 1 : 0);
      AppendU8(&out, column.max_present ? 1 : 0);
      AppendString(&out, column.encoded_min);
      AppendString(&out, column.encoded_max);
      AppendU8(&out, column.dictionary_exact ? 1 : 0);
      AppendU8(&out, column.dictionary_overflow ? 1 : 0);
      AppendU32(&out, static_cast<u32>(column.dictionary_values.size()));
      for (const auto& value : column.dictionary_values) {
        AppendString(&out, value);
      }
    }
  }

  AppendU32(&out, static_cast<u32>(segment.compression_policies.size()));
  for (const auto& policy : segment.compression_policies) {
    AppendU32(&out, policy.column_ordinal);
    AppendString(&out, policy.scalar_type_key);
    AppendString(&out, policy.codec_id);
    AppendU64(&out, policy.uncompressed_bytes);
    AppendU64(&out, policy.compressed_bytes);
    AppendDouble(&out, policy.estimated_cpu_cost);
    AppendDouble(&out, policy.estimated_read_cost);
    AppendDouble(&out, policy.estimated_write_cost);
    AppendU8(&out, policy.exact_fallback_equivalence ? 1 : 0);
    AppendU8(&out, policy.storage_runtime_evidence_only ? 1 : 0);
    AppendU8(&out, policy.compression_value_authority_claimed ? 1 : 0);
  }

  AppendU32(&out, static_cast<u32>(segment.evidence.size()));
  for (const auto& evidence : segment.evidence) {
    AppendString(&out, evidence);
  }

  result.checksum = ComputeChecksum(out);
  scratchbird::core::platform::StoreLittle64(out.data() + 16, result.checksum);
  result.status = OkStatus();
  result.diagnostic = MakePhysicalColumnarZoneDiagnostic(
      result.status,
      "INDEX.PHYSICAL_COLUMNAR_ZONE.SERIALIZED",
      "index.physical_columnar_zone.serialized");
  return result;
}

PhysicalColumnarZoneOpenResult OpenPhysicalColumnarZoneSegment(
    const PhysicalColumnarZoneOpenRequest& request) {
  if (request.bytes.size() < kHeaderBytes ||
      !std::equal(kMagic.begin(), kMagic.end(), request.bytes.begin())) {
    return OpenFailure(PhysicalColumnarZoneOpenClass::corrupt_payload,
                       "INDEX.PHYSICAL_COLUMNAR_ZONE.BAD_MAGIC",
                       "index.physical_columnar_zone.bad_magic");
  }

  Reader reader(request.bytes);
  reader.SetOffset(8);
  u32 major = 0;
  u32 minor = 0;
  u64 stored_checksum = 0;
  if (!reader.ReadU32(&major) ||
      !reader.ReadU32(&minor) ||
      !reader.ReadU64(&stored_checksum)) {
    return OpenFailure(PhysicalColumnarZoneOpenClass::truncated_payload,
                       "INDEX.PHYSICAL_COLUMNAR_ZONE.TRUNCATED_HEADER",
                       "index.physical_columnar_zone.truncated_header");
  }
  if (major != kPhysicalColumnarZoneCurrentMajor ||
      minor != kPhysicalColumnarZoneCurrentMinor) {
    return OpenFailure(PhysicalColumnarZoneOpenClass::stale_format,
                       "INDEX.PHYSICAL_COLUMNAR_ZONE.STALE_FORMAT",
                       "index.physical_columnar_zone.stale_format");
  }
  if (stored_checksum == 0 || ComputeChecksum(request.bytes) != stored_checksum) {
    return OpenFailure(PhysicalColumnarZoneOpenClass::bad_checksum,
                       "INDEX.PHYSICAL_COLUMNAR_ZONE.BAD_CHECKSUM",
                       "index.physical_columnar_zone.bad_checksum");
  }

  PhysicalColumnarZoneSegment segment;
  segment.format_version = {major, minor};
  byte mga = 0;
  byte security = 0;
  byte exact = 0;
  byte visibility = 0;
  byte parser = 0;
  byte reference = 0;
  byte provider = 0;
  byte log = 0;
  if (!reader.ReadString(&segment.relation_uuid) ||
      !reader.ReadString(&segment.index_uuid) ||
      !reader.ReadString(&segment.segment_uuid) ||
      !reader.ReadU64(&segment.base_generation) ||
      !reader.ReadU64(&segment.summary_generation) ||
      !reader.ReadU8(&mga) ||
      !reader.ReadU8(&security) ||
      !reader.ReadU8(&exact) ||
      !reader.ReadU8(&visibility) ||
      !reader.ReadU8(&parser) ||
      !reader.ReadU8(&reference) ||
      !reader.ReadU8(&provider) ||
      !reader.ReadU8(&log)) {
    return OpenFailure(PhysicalColumnarZoneOpenClass::truncated_payload,
                       "INDEX.PHYSICAL_COLUMNAR_ZONE.TRUNCATED_PAYLOAD",
                       "index.physical_columnar_zone.truncated_payload");
  }
  segment.mga_recheck_required = mga != 0;
  segment.security_recheck_required = security != 0;
  segment.exact_recheck_required = exact != 0;
  segment.visibility_finality_authority = visibility != 0;
  segment.parser_finality_authority_claimed = parser != 0;
  segment.reference_finality_authority_claimed = reference != 0;
  segment.provider_finality_authority_claimed = provider != 0;
  segment.write_ahead_log_finality_authority_claimed = log != 0;

  u32 group_count = 0;
  if (!reader.ReadU32(&group_count)) {
    return OpenFailure(PhysicalColumnarZoneOpenClass::truncated_payload,
                       "INDEX.PHYSICAL_COLUMNAR_ZONE.TRUNCATED_ROW_GROUPS",
                       "index.physical_columnar_zone.truncated_row_groups");
  }
  for (u32 group_index = 0; group_index < group_count; ++group_index) {
    PhysicalColumnarZoneRowGroupSummary group;
    u32 status = 0;
    byte persisted = 0;
    byte checksum_valid = 0;
    byte group_mga = 0;
    byte group_security = 0;
    byte group_exact = 0;
    u32 candidate_count = 0;
    if (!reader.ReadU64(&group.boundary.row_group_id) ||
        !reader.ReadU64(&group.boundary.first_page_id) ||
        !reader.ReadU32(&group.boundary.page_count) ||
        !reader.ReadU64(&group.boundary.first_row_ordinal) ||
        !reader.ReadU64(&group.boundary.row_count) ||
        !reader.ReadU64(&group.row_count) ||
        !reader.ReadU64(&group.deleted_row_count) ||
        !reader.ReadU64(&group.base_generation) ||
        !reader.ReadU64(&group.summary_generation) ||
        !reader.ReadU32(&status) ||
        !reader.ReadU8(&persisted) ||
        !reader.ReadU8(&checksum_valid) ||
        !reader.ReadU8(&group_mga) ||
        !reader.ReadU8(&group_security) ||
        !reader.ReadU8(&group_exact) ||
        !reader.ReadU32(&candidate_count)) {
      return OpenFailure(PhysicalColumnarZoneOpenClass::truncated_payload,
                         "INDEX.PHYSICAL_COLUMNAR_ZONE.TRUNCATED_ROW_GROUP",
                         "index.physical_columnar_zone.truncated_row_group");
    }
    group.status = static_cast<PageExtentSummaryStatus>(status);
    group.persisted_record_present = persisted != 0;
    group.checksum_valid = checksum_valid != 0;
    group.mga_recheck_required = group_mga != 0;
    group.security_recheck_required = group_security != 0;
    group.exact_recheck_required = group_exact != 0;
    for (u32 candidate_index = 0; candidate_index < candidate_count; ++candidate_index) {
      u64 ordinal = 0;
      if (!reader.ReadU64(&ordinal)) {
        return OpenFailure(PhysicalColumnarZoneOpenClass::truncated_payload,
                           "INDEX.PHYSICAL_COLUMNAR_ZONE.TRUNCATED_CANDIDATES",
                           "index.physical_columnar_zone.truncated_candidates");
      }
      group.candidate_row_ordinals.push_back(ordinal);
    }
    u32 column_count = 0;
    if (!reader.ReadU32(&column_count)) {
      return OpenFailure(PhysicalColumnarZoneOpenClass::truncated_payload,
                         "INDEX.PHYSICAL_COLUMNAR_ZONE.TRUNCATED_COLUMNS",
                         "index.physical_columnar_zone.truncated_columns");
    }
    for (u32 column_index = 0; column_index < column_count; ++column_index) {
      PhysicalColumnarZoneColumnSummary column;
      byte min_present = 0;
      byte max_present = 0;
      byte dictionary_exact = 0;
      byte dictionary_overflow = 0;
      u32 dictionary_count = 0;
      if (!reader.ReadU32(&column.column_ordinal) ||
          !reader.ReadString(&column.scalar_type_key) ||
          !reader.ReadU64(&column.row_count) ||
          !reader.ReadU64(&column.null_count) ||
          !reader.ReadU8(&min_present) ||
          !reader.ReadU8(&max_present) ||
          !reader.ReadString(&column.encoded_min) ||
          !reader.ReadString(&column.encoded_max) ||
          !reader.ReadU8(&dictionary_exact) ||
          !reader.ReadU8(&dictionary_overflow) ||
          !reader.ReadU32(&dictionary_count)) {
        return OpenFailure(PhysicalColumnarZoneOpenClass::truncated_payload,
                           "INDEX.PHYSICAL_COLUMNAR_ZONE.TRUNCATED_COLUMN",
                           "index.physical_columnar_zone.truncated_column");
      }
      column.min_present = min_present != 0;
      column.max_present = max_present != 0;
      column.dictionary_exact = dictionary_exact != 0;
      column.dictionary_overflow = dictionary_overflow != 0;
      for (u32 value_index = 0; value_index < dictionary_count; ++value_index) {
        std::string value;
        if (!reader.ReadString(&value)) {
          return OpenFailure(PhysicalColumnarZoneOpenClass::truncated_payload,
                             "INDEX.PHYSICAL_COLUMNAR_ZONE.TRUNCATED_DICTIONARY",
                             "index.physical_columnar_zone.truncated_dictionary");
        }
        column.dictionary_values.push_back(std::move(value));
      }
      group.columns.push_back(std::move(column));
    }
    segment.row_groups.push_back(std::move(group));
  }

  u32 policy_count = 0;
  if (!reader.ReadU32(&policy_count)) {
    return OpenFailure(PhysicalColumnarZoneOpenClass::truncated_payload,
                       "INDEX.PHYSICAL_COLUMNAR_ZONE.TRUNCATED_COMPRESSION",
                       "index.physical_columnar_zone.truncated_compression");
  }
  for (u32 policy_index = 0; policy_index < policy_count; ++policy_index) {
    PhysicalColumnarZoneCompressionPolicy policy;
    byte exact_fallback = 0;
    byte storage_only = 0;
    byte value_authority = 0;
    if (!reader.ReadU32(&policy.column_ordinal) ||
        !reader.ReadString(&policy.scalar_type_key) ||
        !reader.ReadString(&policy.codec_id) ||
        !reader.ReadU64(&policy.uncompressed_bytes) ||
        !reader.ReadU64(&policy.compressed_bytes) ||
        !reader.ReadDouble(&policy.estimated_cpu_cost) ||
        !reader.ReadDouble(&policy.estimated_read_cost) ||
        !reader.ReadDouble(&policy.estimated_write_cost) ||
        !reader.ReadU8(&exact_fallback) ||
        !reader.ReadU8(&storage_only) ||
        !reader.ReadU8(&value_authority)) {
      return OpenFailure(PhysicalColumnarZoneOpenClass::truncated_payload,
                         "INDEX.PHYSICAL_COLUMNAR_ZONE.TRUNCATED_COMPRESSION_POLICY",
                         "index.physical_columnar_zone.truncated_compression_policy");
    }
    policy.exact_fallback_equivalence = exact_fallback != 0;
    policy.storage_runtime_evidence_only = storage_only != 0;
    policy.compression_value_authority_claimed = value_authority != 0;
    segment.compression_policies.push_back(std::move(policy));
  }

  u32 evidence_count = 0;
  if (!reader.ReadU32(&evidence_count)) {
    return OpenFailure(PhysicalColumnarZoneOpenClass::truncated_payload,
                       "INDEX.PHYSICAL_COLUMNAR_ZONE.TRUNCATED_EVIDENCE",
                       "index.physical_columnar_zone.truncated_evidence");
  }
  for (u32 index = 0; index < evidence_count; ++index) {
    std::string evidence;
    if (!reader.ReadString(&evidence)) {
      return OpenFailure(PhysicalColumnarZoneOpenClass::truncated_payload,
                         "INDEX.PHYSICAL_COLUMNAR_ZONE.TRUNCATED_EVIDENCE_ENTRY",
                         "index.physical_columnar_zone.truncated_evidence_entry");
    }
    segment.evidence.push_back(std::move(evidence));
  }
  if (!reader.Done()) {
    return OpenFailure(PhysicalColumnarZoneOpenClass::corrupt_payload,
                       "INDEX.PHYSICAL_COLUMNAR_ZONE.TRAILING_BYTES",
                       "index.physical_columnar_zone.trailing_bytes");
  }

  const auto validation = ValidateSegment(segment);
  if (validation != SegmentValidationClass::valid) {
    const auto open_class = OpenClassForValidation(validation);
    auto result = OpenFailure(open_class,
                              CodeForOpenClass(open_class),
                              MessageKeyForOpenClass(open_class));
    result.segment = std::move(segment);
    return result;
  }

  if ((request.expected_relation_uuid_present &&
       segment.relation_uuid != request.expected_relation_uuid) ||
      (request.expected_index_uuid_present &&
       segment.index_uuid != request.expected_index_uuid) ||
      (request.expected_segment_uuid_present &&
       segment.segment_uuid != request.expected_segment_uuid)) {
    auto result = OpenFailure(PhysicalColumnarZoneOpenClass::identity_mismatch,
                              "INDEX.PHYSICAL_COLUMNAR_ZONE.IDENTITY_MISMATCH",
                              "index.physical_columnar_zone.identity_mismatch");
    result.segment = std::move(segment);
    return result;
  }
  if (request.expected_base_generation_present &&
      segment.base_generation != request.expected_base_generation) {
    auto result = OpenFailure(PhysicalColumnarZoneOpenClass::stale_generation,
                              "INDEX.PHYSICAL_COLUMNAR_ZONE.STALE_GENERATION",
                              "index.physical_columnar_zone.stale_generation");
    result.segment = std::move(segment);
    result.actions.push_back("rebuild_columnar_zone_from_authoritative_page_evidence");
    return result;
  }

  PhysicalColumnarZoneOpenResult result;
  result.status = OkStatus();
  result.open_class = PhysicalColumnarZoneOpenClass::current;
  result.scan_required = false;
  result.rebuild_required = false;
  result.segment = std::move(segment);
  result.actions.push_back("physical_columnar_zone_opened_clean");
  result.actions.push_back("candidate_rows_require_mga_security_exact_recheck");
  result.diagnostic = MakePhysicalColumnarZoneDiagnostic(
      result.status,
      "INDEX.PHYSICAL_COLUMNAR_ZONE.OPENED",
      "index.physical_columnar_zone.opened");
  return result;
}

PhysicalColumnarZonePruneResult PrunePhysicalColumnarZone(
    const PhysicalColumnarZonePruneRequest& request) {
  PhysicalColumnarZonePruneResult result;
  if (!SegmentValid(request.segment)) {
    result.status = WarnStatus();
    result.full_scan_required = true;
    result.diagnostic = MakePhysicalColumnarZoneDiagnostic(
        result.status,
        "INDEX.PHYSICAL_COLUMNAR_ZONE.PRUNE_FULL_SCAN",
        "index.physical_columnar_zone.prune_full_scan");
    result.evidence.push_back("invalid_columnar_zone_scan_required");
    return result;
  }
  for (const auto& predicate : request.predicates) {
    if ((predicate.lower_present && !EncodedKeySafe(predicate.encoded_lower)) ||
        (predicate.upper_present && !EncodedKeySafe(predicate.encoded_upper))) {
      result.status = WarnStatus();
      result.full_scan_required = true;
      result.diagnostic = MakePhysicalColumnarZoneDiagnostic(
          result.status,
          "INDEX.PHYSICAL_COLUMNAR_ZONE.UNSAFE_PREDICATE_KEY",
          "index.physical_columnar_zone.unsafe_predicate_key");
      result.evidence.push_back("unsafe_predicate_key_scan_required");
      return result;
    }
  }

  result.status = OkStatus();
  result.diagnostic = MakePhysicalColumnarZoneDiagnostic(
      result.status,
      "INDEX.PHYSICAL_COLUMNAR_ZONE.PRUNE_CLASSIFIED",
      "index.physical_columnar_zone.prune_classified");
  result.evidence.push_back("columnar_zone_candidate_pruning_only=true");
  result.evidence.push_back("summary_proves_absence_or_range_exclusion_only=true");
  result.evidence.push_back("mga_recheck_required=true");
  result.evidence.push_back("security_recheck_required=true");
  result.evidence.push_back("exact_recheck_required=true");
  result.evidence.push_back("visibility_finality_authority=false");
  result.evidence.push_back("parser_reference_provider_finality_authority=false");
  result.evidence.push_back("write_ahead_log_finality_authority=false");

  for (const auto& group : request.segment.row_groups) {
    PhysicalColumnarZoneCandidateGroup candidate;
    candidate.segment_uuid = request.segment.segment_uuid;
    candidate.boundary = group.boundary;
    candidate.mga_recheck_required = group.mga_recheck_required;
    candidate.security_recheck_required = group.security_recheck_required;
    candidate.exact_recheck_required = group.exact_recheck_required;
    candidate.candidate_page_ids = CandidatePages(group.boundary);
    candidate.candidate_row_ordinals = group.candidate_row_ordinals;
    if (group.status != PageExtentSummaryStatus::current) {
      candidate.decision = PhysicalColumnarZonePruneDecision::full_scan_fallback;
      candidate.reason = "row_group_not_current";
      result.full_scan_required = true;
      result.groups.push_back(std::move(candidate));
      continue;
    }
    for (const auto& predicate : request.predicates) {
      const auto* column = FindColumn(group, predicate.column_ordinal);
      if (column == nullptr || column->scalar_type_key != predicate.scalar_type_key) {
        continue;
      }
      if (PredicateOutsideMinMax(predicate, *column)) {
        candidate.decision = PhysicalColumnarZonePruneDecision::pruned_by_min_max;
        candidate.scan_required = false;
        candidate.candidate_page_ids.clear();
        candidate.candidate_row_ordinals.clear();
        candidate.reason = "predicate_outside_columnar_min_max";
        result.any_pruned = true;
        break;
      }
      if (column->dictionary_overflow) {
        candidate.decision =
            PhysicalColumnarZonePruneDecision::exact_dictionary_overflow_scan;
        candidate.reason = "dictionary_overflow_range_or_scan_recheck_required";
        continue;
      }
      if (column->dictionary_exact && !column->dictionary_values.empty()) {
        const bool any_overlap = std::any_of(
            column->dictionary_values.begin(),
            column->dictionary_values.end(),
            [&](const std::string& value) {
              return PredicateValueOverlap(predicate, value);
            });
        if (!any_overlap) {
          candidate.decision =
              PhysicalColumnarZonePruneDecision::pruned_by_exact_dictionary;
          candidate.scan_required = false;
          candidate.candidate_page_ids.clear();
          candidate.candidate_row_ordinals.clear();
          candidate.reason = "predicate_absent_from_bounded_exact_dictionary";
          result.any_pruned = true;
          break;
        }
      }
    }
    if (candidate.scan_required) {
      if (candidate.decision !=
          PhysicalColumnarZonePruneDecision::exact_dictionary_overflow_scan) {
        candidate.decision = PhysicalColumnarZonePruneDecision::scan_with_recheck;
      }
      if (candidate.reason.empty()) {
        candidate.reason = "row_group_may_match_recheck_required";
      }
    }
    result.groups.push_back(std::move(candidate));
  }
  return result;
}

PhysicalColumnarZoneCandidateStream OpenPhysicalColumnarZoneCandidateStream(
    const PhysicalColumnarZoneLateMaterializationRequest& request) {
  PhysicalColumnarZoneCandidateStream stream;
  if (!SegmentValid(request.segment) || !request.prune_result.ok()) {
    stream.status = WarnStatus();
    stream.diagnostic = MakePhysicalColumnarZoneDiagnostic(
        stream.status,
        "INDEX.PHYSICAL_COLUMNAR_ZONE.CANDIDATE_STREAM_REFUSED",
        "index.physical_columnar_zone.candidate_stream_refused");
    stream.evidence.push_back("candidate_stream_requires_valid_prune_result");
    return stream;
  }
  for (const auto& group : request.prune_result.groups) {
    if (!CandidateGroupMatchesSegment(request.segment, group)) {
      stream.status = WarnStatus();
      stream.diagnostic = MakePhysicalColumnarZoneDiagnostic(
          stream.status,
          "INDEX.PHYSICAL_COLUMNAR_ZONE.CANDIDATE_STREAM_REFUSED",
          "index.physical_columnar_zone.candidate_stream_refused",
          "candidate_group_not_proven_from_segment");
      stream.evidence.push_back("candidate_stream_requires_segment_derived_prune_result");
      return stream;
    }
  }
  std::set<u32> projection;
  std::set<u32> available_columns;
  for (const auto& group : request.segment.row_groups) {
    for (const auto& column : group.columns) {
      available_columns.insert(column.column_ordinal);
    }
  }
  for (u32 ordinal : request.projection_column_ordinals) {
    if (!projection.insert(ordinal).second ||
        available_columns.find(ordinal) == available_columns.end()) {
      stream.status = WarnStatus();
      stream.diagnostic = MakePhysicalColumnarZoneDiagnostic(
          stream.status,
          "INDEX.PHYSICAL_COLUMNAR_ZONE.INVALID_PROJECTION",
          "index.physical_columnar_zone.invalid_projection");
      stream.evidence.push_back("projection_columns_must_be_unique_and_summarized");
      return stream;
    }
  }

  std::set<u64> ordinals;
  for (const auto& group : request.prune_result.groups) {
    if (!group.scan_required) {
      continue;
    }
    ordinals.insert(group.candidate_row_ordinals.begin(),
                    group.candidate_row_ordinals.end());
  }
  stream.candidate_row_ordinals.assign(ordinals.begin(), ordinals.end());
  stream.projection_column_ordinals = request.projection_column_ordinals;
  std::sort(stream.projection_column_ordinals.begin(),
            stream.projection_column_ordinals.end());
  stream.status = OkStatus();
  stream.stream_ready = true;
  stream.fetches_non_candidate_rows = false;
  stream.evidence.push_back("late_materialization_candidate_row_ordinal_stream=true");
  stream.evidence.push_back("projection_column_list_runtime_consumable=true");
  stream.evidence.push_back("non_candidate_rows_not_fetched=true");
  stream.evidence.push_back("mga_recheck_required=true");
  stream.evidence.push_back("security_recheck_required=true");
  stream.evidence.push_back("exact_recheck_required=true");
  stream.evidence.push_back("visibility_finality_authority=false");
  stream.diagnostic = MakePhysicalColumnarZoneDiagnostic(
      stream.status,
      "INDEX.PHYSICAL_COLUMNAR_ZONE.CANDIDATE_STREAM_OPENED",
      "index.physical_columnar_zone.candidate_stream_opened");
  return stream;
}

PhysicalColumnarZoneMutationResult ApplyPhysicalColumnarZoneMutation(
    const PhysicalColumnarZoneSegment& segment,
    const PhysicalColumnarZoneMutation& mutation,
    u32 dictionary_limit) {
  if (!SegmentValid(segment)) {
    return MutationFailure(segment,
                           "INDEX.PHYSICAL_COLUMNAR_ZONE.MUTATION_INVALID_SEGMENT",
                           "index.physical_columnar_zone.mutation_invalid_segment");
  }
  if ((mutation.before_row_present && !RowEvidenceValid(mutation.before_row)) ||
      (mutation.after_row_present && !RowEvidenceValid(mutation.after_row))) {
    return MutationFailure(segment,
                           "INDEX.PHYSICAL_COLUMNAR_ZONE.MUTATION_INVALID_EVIDENCE",
                           "index.physical_columnar_zone.mutation_invalid_evidence");
  }

  auto working = segment;
  auto rebuild_if_admitted = [&]() -> PhysicalColumnarZoneMutationResult {
    if (!mutation.rebuild_admitted || mutation.authoritative_base_rows.empty()) {
      auto result = MutationFailure(
          working,
          "INDEX.PHYSICAL_COLUMNAR_ZONE.STALE_REBUILD_REQUIRED",
          "index.physical_columnar_zone.stale_rebuild_required");
      result.actions.push_back("columnar_zone_marked_stale_scan_until_rebuild");
      return result;
    }
    const auto& policies = mutation.compression_policies.empty()
                               ? working.compression_policies
                               : mutation.compression_policies;
    return RepairPhysicalColumnarZoneFromPageEvidence(
        working, mutation.authoritative_base_rows, policies, true, dictionary_limit);
  };

  switch (mutation.kind) {
    case PhysicalColumnarZoneMutationKind::append_row: {
      if (!mutation.after_row_present || mutation.after_row.physically_deleted) {
        return MutationFailure(working,
                               "INDEX.PHYSICAL_COLUMNAR_ZONE.APPEND_EVIDENCE_MISSING",
                               "index.physical_columnar_zone.append_evidence_missing");
      }
      auto* group = FindGroup(&working, mutation.after_row.row_group_id);
      if (group == nullptr) {
        working.row_groups.push_back(SeedRowGroup(mutation.after_row.row_group_id,
                                                 working.base_generation,
                                                 working.summary_generation));
        group = &working.row_groups.back();
      }
      if (!ApplyRowToGroup(group, mutation.after_row, dictionary_limit)) {
        group->status = PageExtentSummaryStatus::stale;
        return rebuild_if_admitted();
      }
      group->summary_generation = NextGeneration(group->summary_generation);
      working.base_generation =
          std::max(working.base_generation, mutation.after_row.base_generation);
      working.summary_generation = NextGeneration(working.summary_generation);
      SortColumnarZoneSegment(&working);
      break;
    }
    case PhysicalColumnarZoneMutationKind::delete_row: {
      if (!mutation.before_row_present) {
        return MutationFailure(working,
                               "INDEX.PHYSICAL_COLUMNAR_ZONE.DELETE_EVIDENCE_MISSING",
                               "index.physical_columnar_zone.delete_evidence_missing");
      }
      auto* group = FindGroup(&working, mutation.before_row.row_group_id);
      if (group != nullptr &&
          RowTouchesBoundaryDictionaryOrCandidate(*group, mutation.before_row)) {
        group->status = PageExtentSummaryStatus::stale;
      }
      return rebuild_if_admitted();
    }
    case PhysicalColumnarZoneMutationKind::update_row: {
      if (!mutation.before_row_present || !mutation.after_row_present) {
        return MutationFailure(working,
                               "INDEX.PHYSICAL_COLUMNAR_ZONE.UPDATE_EVIDENCE_MISSING",
                               "index.physical_columnar_zone.update_evidence_missing");
      }
      auto* group = FindGroup(&working, mutation.before_row.row_group_id);
      if (group != nullptr &&
          RowTouchesBoundaryDictionaryOrCandidate(*group, mutation.before_row)) {
        group->status = PageExtentSummaryStatus::stale;
      }
      return rebuild_if_admitted();
    }
  }

  if (!SegmentValid(working)) {
    return MutationFailure(working,
                           "INDEX.PHYSICAL_COLUMNAR_ZONE.MUTATION_RESULT_INVALID",
                           "index.physical_columnar_zone.mutation_result_invalid");
  }
  PhysicalColumnarZoneMutationResult result;
  result.status = OkStatus();
  result.segment = std::move(working);
  result.applied = true;
  result.scan_required = false;
  result.actions.push_back("physical_columnar_zone_mutation_applied");
  result.actions.push_back("candidate_rows_require_mga_security_exact_recheck");
  result.diagnostic = MakePhysicalColumnarZoneDiagnostic(
      result.status,
      "INDEX.PHYSICAL_COLUMNAR_ZONE.MUTATION_APPLIED",
      "index.physical_columnar_zone.mutation_applied");
  return result;
}

PhysicalColumnarZoneMutationResult RepairPhysicalColumnarZoneFromPageEvidence(
    const PhysicalColumnarZoneSegment& stale_or_corrupt_segment,
    const std::vector<PhysicalColumnarZoneRowEvidence>& authoritative_base_rows,
    const std::vector<PhysicalColumnarZoneCompressionPolicy>& compression_policies,
    bool repair_admitted,
    u32 dictionary_limit) {
  if (!repair_admitted || authoritative_base_rows.empty()) {
    auto result = MutationFailure(
        stale_or_corrupt_segment,
        "INDEX.PHYSICAL_COLUMNAR_ZONE.REPAIR_REFUSED",
        "index.physical_columnar_zone.repair_refused");
    result.actions.push_back("repair_requires_explicit_admission_and_authoritative_base_evidence");
    return result;
  }
  auto request = RebuildRequestForSegment(stale_or_corrupt_segment,
                                          authoritative_base_rows,
                                          compression_policies,
                                          dictionary_limit);
  auto rebuilt = BuildPhysicalColumnarZoneFromPageEvidence(request);
  PhysicalColumnarZoneMutationResult result;
  result.status = rebuilt.status;
  result.diagnostic = rebuilt.diagnostic;
  result.segment = std::move(rebuilt.segment);
  result.applied = rebuilt.ok();
  result.rebuild_performed = rebuilt.ok();
  result.scan_required = !rebuilt.ok();
  result.actions.push_back(rebuilt.ok()
                               ? "physical_columnar_zone_rebuilt_from_authoritative_page_evidence"
                               : "physical_columnar_zone_rebuild_refused");
  return result;
}

const char* PhysicalColumnarZoneOpenClassName(
    PhysicalColumnarZoneOpenClass open_class) {
  switch (open_class) {
    case PhysicalColumnarZoneOpenClass::current: return "current";
    case PhysicalColumnarZoneOpenClass::stale_format: return "stale_format";
    case PhysicalColumnarZoneOpenClass::stale_generation: return "stale_generation";
    case PhysicalColumnarZoneOpenClass::bad_checksum: return "bad_checksum";
    case PhysicalColumnarZoneOpenClass::identity_mismatch: return "identity_mismatch";
    case PhysicalColumnarZoneOpenClass::malformed_column_payload:
      return "malformed_column_payload";
    case PhysicalColumnarZoneOpenClass::malformed_row_group_payload:
      return "malformed_row_group_payload";
    case PhysicalColumnarZoneOpenClass::unsafe_encoding: return "unsafe_encoding";
    case PhysicalColumnarZoneOpenClass::truncated_payload: return "truncated_payload";
    case PhysicalColumnarZoneOpenClass::corrupt_payload: return "corrupt_payload";
    case PhysicalColumnarZoneOpenClass::refused: return "refused";
  }
  return "unknown";
}

const char* PhysicalColumnarZonePruneDecisionName(
    PhysicalColumnarZonePruneDecision decision) {
  switch (decision) {
    case PhysicalColumnarZonePruneDecision::scan_with_recheck:
      return "scan_with_recheck";
    case PhysicalColumnarZonePruneDecision::pruned_by_min_max:
      return "pruned_by_min_max";
    case PhysicalColumnarZonePruneDecision::pruned_by_exact_dictionary:
      return "pruned_by_exact_dictionary";
    case PhysicalColumnarZonePruneDecision::exact_dictionary_overflow_scan:
      return "exact_dictionary_overflow_scan";
    case PhysicalColumnarZonePruneDecision::full_scan_fallback:
      return "full_scan_fallback";
  }
  return "unknown";
}

DiagnosticRecord MakePhysicalColumnarZoneDiagnostic(Status status,
                                                    std::string diagnostic_code,
                                                    std::string message_key,
                                                    std::string detail) {
  std::vector<DiagnosticArgument> arguments;
  if (!detail.empty()) {
    arguments.push_back({"detail", std::move(detail)});
  }
  return MakeDiagnostic(status.code,
                        status.severity,
                        status.subsystem,
                        std::move(diagnostic_code),
                        std::move(message_key),
                        std::move(arguments),
                        {},
                        "core.index.physical_columnar_zone");
}

}  // namespace scratchbird::core::index
