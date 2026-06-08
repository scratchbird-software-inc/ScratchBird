// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "physical_zone_summary.hpp"

#include "index_key_encoding.hpp"

#include <algorithm>
#include <array>
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

inline constexpr std::array<byte, 8> kMagic = {'S', 'B', 'P', 'Z', 'S', 'U', 'M', '1'};
inline constexpr u32 kHeaderBytes = 24;
inline constexpr u64 kFnvOffset = 14695981039346656037ull;
inline constexpr u64 kFnvPrime = 1099511628211ull;

Status OkStatus() { return {StatusCode::ok, Severity::info, Subsystem::engine}; }
Status WarnStatus() { return {StatusCode::ok, Severity::warning, Subsystem::engine}; }
Status ErrorStatus() {
  return {StatusCode::platform_required_feature_missing, Severity::error, Subsystem::engine};
}

u64 NextGeneration(u64 value) {
  return value == 0 ? 1 : value + 1;
}

PhysicalZoneSummaryBuildResult BuildFailure(std::string code,
                                            std::string key,
                                            std::string detail = {}) {
  PhysicalZoneSummaryBuildResult result;
  result.status = ErrorStatus();
  result.diagnostic = MakePhysicalZoneSummaryDiagnostic(
      result.status, std::move(code), std::move(key), std::move(detail));
  return result;
}

PhysicalZoneSummaryMutationResult MutationFailure(PhysicalZoneSummaryPage page,
                                                 std::string code,
                                                 std::string key,
                                                 std::string detail = {}) {
  PhysicalZoneSummaryMutationResult result;
  result.status = WarnStatus();
  result.page = std::move(page);
  result.full_scan_required = true;
  result.summary_invalidated = true;
  result.diagnostic = MakePhysicalZoneSummaryDiagnostic(
      result.status, std::move(code), std::move(key), std::move(detail));
  return result;
}

bool SameFormatVersion(PageExtentSummaryFormatVersion left,
                       PageExtentSummaryFormatVersion right) {
  return left.major == right.major && left.minor == right.minor;
}

bool RangeSizingValid(const PhysicalZoneRangeSizingMetadata& sizing) {
  return sizing.min_pages_per_range > 0 &&
         sizing.target_pages_per_range >= sizing.min_pages_per_range &&
         sizing.max_pages_per_range >= sizing.target_pages_per_range &&
         sizing.summary_generation > 0;
}

bool AuthorityClean(const PhysicalZoneRowEvidence& row) {
  return !row.parser_finality_authority_claimed &&
         !row.donor_finality_authority_claimed &&
         !row.write_ahead_log_finality_authority_claimed;
}

bool ColumnValueValid(const PhysicalZoneColumnValueEvidence& value) {
  if (value.value_is_null) {
    return true;
  }
  return !value.scalar_type_key.empty() &&
         !value.encoded_scalar.empty() &&
         IsOrderPreservingIndexKeyEncoding(value.encoded_scalar) &&
         !IsUnsafeLegacyIndexKeyEncoding(value.encoded_scalar);
}

bool RowEvidenceValid(const PhysicalZoneRowEvidence& row) {
  if (!AuthorityClean(row)) {
    return false;
  }
  std::set<u32> ordinals;
  for (const auto& column : row.columns) {
    if (!ColumnValueValid(column) || !ordinals.insert(column.column_ordinal).second) {
      return false;
    }
  }
  return true;
}

bool KeyLess(std::string_view left, std::string_view right) {
  const auto compared = CompareEncodedIndexKeyBytes(left, right);
  return compared.ok() && compared.comparison < 0;
}

bool KeyGreater(std::string_view left, std::string_view right) {
  const auto compared = CompareEncodedIndexKeyBytes(left, right);
  return compared.ok() && compared.comparison > 0;
}

bool SameScalarTypeOrUnset(const PhysicalZoneColumnSummary& summary,
                           const PhysicalZoneColumnValueEvidence& value) {
  return value.value_is_null || summary.scalar_type_key.empty() ||
         summary.scalar_type_key == value.scalar_type_key;
}

void AddSmallSetValue(PhysicalZoneColumnSummary* summary,
                      const std::string& encoded_scalar,
                      u32 small_set_limit) {
  if (!summary->small_set_exact) {
    return;
  }
  if (std::find(summary->small_set_values.begin(),
                summary->small_set_values.end(),
                encoded_scalar) != summary->small_set_values.end()) {
    return;
  }
  if (summary->small_set_values.size() >= small_set_limit) {
    summary->small_set_exact = false;
    summary->small_set_overflow = true;
    summary->small_set_values.clear();
    return;
  }
  summary->small_set_values.push_back(encoded_scalar);
  std::sort(summary->small_set_values.begin(),
            summary->small_set_values.end(),
            [](const std::string& left, const std::string& right) {
              return KeyLess(left, right);
            });
}

void AddSmallSetValue(PhysicalZoneMultiColumnSummary* summary,
                      const std::string& encoded_tuple,
                      u32 small_set_limit) {
  if (!summary->small_set_exact) {
    return;
  }
  if (std::find(summary->small_set_values.begin(),
                summary->small_set_values.end(),
                encoded_tuple) != summary->small_set_values.end()) {
    return;
  }
  if (summary->small_set_values.size() >= small_set_limit) {
    summary->small_set_exact = false;
    summary->small_set_overflow = true;
    summary->small_set_values.clear();
    return;
  }
  summary->small_set_values.push_back(encoded_tuple);
  std::sort(summary->small_set_values.begin(),
            summary->small_set_values.end(),
            [](const std::string& left, const std::string& right) {
              return KeyLess(left, right);
            });
}

bool ApplyColumnValue(PhysicalZoneColumnSummary* summary,
                      const PhysicalZoneColumnValueEvidence& value,
                      u32 small_set_limit) {
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
  AddSmallSetValue(summary, value.encoded_scalar, small_set_limit);
  return true;
}

std::string CompositeEncodedKey(
    const std::vector<PhysicalZoneColumnValueEvidence>& sorted_columns,
    std::vector<u32>* ordinals,
    bool* any_null) {
  if (ordinals != nullptr) {
    ordinals->clear();
  }
  if (any_null != nullptr) {
    *any_null = false;
  }
  if (sorted_columns.size() < 2) {
    return {};
  }
  std::string encoded;
  for (const auto& value : sorted_columns) {
    if (ordinals != nullptr) {
      ordinals->push_back(value.column_ordinal);
    }
    if (value.value_is_null) {
      if (any_null != nullptr) {
        *any_null = true;
      }
      continue;
    }
    if (encoded.empty()) {
      encoded.assign(value.encoded_scalar.data(), 4);
    }
    encoded.append(value.encoded_scalar.data() + 4, value.encoded_scalar.size() - 4);
  }
  return any_null != nullptr && *any_null ? std::string{} : encoded;
}

bool ApplyMultiColumnValue(PhysicalZoneRangeSummaryRecord* range,
                           const PhysicalZoneRowEvidence& row,
                           u32 small_set_limit) {
  if (row.columns.size() < 2) {
    return true;
  }
  std::vector<PhysicalZoneColumnValueEvidence> sorted_columns = row.columns;
  std::sort(sorted_columns.begin(),
            sorted_columns.end(),
            [](const PhysicalZoneColumnValueEvidence& left,
               const PhysicalZoneColumnValueEvidence& right) {
              return left.column_ordinal < right.column_ordinal;
            });
  std::vector<u32> ordinals;
  bool any_null = false;
  const auto encoded_tuple = CompositeEncodedKey(sorted_columns, &ordinals, &any_null);
  if (ordinals.size() < 2) {
    return true;
  }
  auto iter = std::find_if(range->multi_columns.begin(),
                           range->multi_columns.end(),
                           [&](const PhysicalZoneMultiColumnSummary& summary) {
                             return summary.column_ordinals == ordinals;
                           });
  if (iter == range->multi_columns.end()) {
    PhysicalZoneMultiColumnSummary summary;
    summary.column_ordinals = std::move(ordinals);
    range->multi_columns.push_back(std::move(summary));
    iter = std::prev(range->multi_columns.end());
  }
  ++iter->row_count;
  if (any_null) {
    ++iter->null_tuple_count;
    return true;
  }
  if (!IsOrderPreservingIndexKeyEncoding(encoded_tuple) ||
      IsUnsafeLegacyIndexKeyEncoding(encoded_tuple)) {
    return false;
  }
  if (!iter->min_present || KeyLess(encoded_tuple, iter->encoded_min_tuple)) {
    iter->encoded_min_tuple = encoded_tuple;
    iter->min_present = true;
  }
  if (!iter->max_present || KeyGreater(encoded_tuple, iter->encoded_max_tuple)) {
    iter->encoded_max_tuple = encoded_tuple;
    iter->max_present = true;
  }
  AddSmallSetValue(&(*iter), encoded_tuple, small_set_limit);
  return true;
}

bool ColumnSummaryValid(const PhysicalZoneColumnSummary& summary) {
  if (summary.null_count > summary.row_count) {
    return false;
  }
  const bool has_non_null = summary.row_count > summary.null_count;
  if (!has_non_null) {
    return !summary.min_present && !summary.max_present &&
           summary.encoded_min.empty() && summary.encoded_max.empty();
  }
  if (summary.scalar_type_key.empty() ||
      !summary.min_present ||
      !summary.max_present ||
      !IsOrderPreservingIndexKeyEncoding(summary.encoded_min) ||
      !IsOrderPreservingIndexKeyEncoding(summary.encoded_max) ||
      IsUnsafeLegacyIndexKeyEncoding(summary.encoded_min) ||
      IsUnsafeLegacyIndexKeyEncoding(summary.encoded_max) ||
      KeyGreater(summary.encoded_min, summary.encoded_max)) {
    return false;
  }
  if (summary.small_set_overflow && summary.small_set_exact) {
    return false;
  }
  for (const auto& value : summary.small_set_values) {
    if (!IsOrderPreservingIndexKeyEncoding(value) ||
        IsUnsafeLegacyIndexKeyEncoding(value)) {
      return false;
    }
  }
  return true;
}

bool MultiColumnSummaryValid(const PhysicalZoneMultiColumnSummary& summary) {
  if (summary.column_ordinals.size() < 2 ||
      !std::is_sorted(summary.column_ordinals.begin(),
                      summary.column_ordinals.end()) ||
      std::adjacent_find(summary.column_ordinals.begin(),
                         summary.column_ordinals.end()) !=
          summary.column_ordinals.end() ||
      summary.null_tuple_count > summary.row_count) {
    return false;
  }
  const bool has_non_null = summary.row_count > summary.null_tuple_count;
  if (!has_non_null) {
    return !summary.min_present && !summary.max_present &&
           summary.encoded_min_tuple.empty() &&
           summary.encoded_max_tuple.empty();
  }
  if (!summary.min_present ||
      !summary.max_present ||
      !IsOrderPreservingIndexKeyEncoding(summary.encoded_min_tuple) ||
      !IsOrderPreservingIndexKeyEncoding(summary.encoded_max_tuple) ||
      IsUnsafeLegacyIndexKeyEncoding(summary.encoded_min_tuple) ||
      IsUnsafeLegacyIndexKeyEncoding(summary.encoded_max_tuple) ||
      KeyGreater(summary.encoded_min_tuple, summary.encoded_max_tuple)) {
    return false;
  }
  if (summary.small_set_overflow && summary.small_set_exact) {
    return false;
  }
  for (const auto& value : summary.small_set_values) {
    if (!IsOrderPreservingIndexKeyEncoding(value) ||
        IsUnsafeLegacyIndexKeyEncoding(value)) {
      return false;
    }
  }
  return true;
}

u64 RangeStartForPage(u64 page_id, u32 pages_per_range) {
  return page_id - (page_id % pages_per_range);
}

PageExtentSummaryRange PageRange(u64 first_page_id, u32 page_count) {
  PageExtentSummaryRange range;
  range.kind = PageExtentSummaryRangeKind::page_range;
  range.first_page_id = first_page_id;
  range.page_count = page_count;
  return range;
}

bool RangeContainsPage(const PageExtentSummaryRange& range, u64 page_id) {
  return range.kind == PageExtentSummaryRangeKind::page_range &&
         range.page_count > 0 &&
         page_id >= range.first_page_id &&
         page_id < range.first_page_id + range.page_count;
}

bool RowInRange(const PhysicalZoneRowEvidence& row,
                const PageExtentSummaryRange& range) {
  if (range.kind == PageExtentSummaryRangeKind::page_range) {
    return RangeContainsPage(range, row.page_id);
  }
  return range.extent_count > 0 &&
         row.extent_id >= range.first_extent_id &&
         row.extent_id < range.first_extent_id + range.extent_count;
}

bool RangeRecordValid(const PhysicalZoneRangeSummaryRecord& record) {
  if (!PageExtentSummaryRangeValid(record.range) ||
      !RangeSizingValid(record.range_sizing) ||
      record.status != PageExtentSummaryStatus::current ||
      !record.persisted_record_present ||
      !record.checksum_valid ||
      !record.mga_recheck_required ||
      !record.security_recheck_required ||
      record.parser_finality_authority_claimed ||
      record.donor_finality_authority_claimed ||
      record.write_ahead_log_finality_authority_claimed ||
      record.base_generation == 0 ||
      record.summary_generation == 0) {
    return false;
  }
  std::set<u32> ordinals;
  for (const auto& column : record.columns) {
    if (!ordinals.insert(column.column_ordinal).second ||
        !ColumnSummaryValid(column)) {
      return false;
    }
  }
  std::set<std::vector<u32>> multi_column_shapes;
  for (const auto& summary : record.multi_columns) {
    if (!multi_column_shapes.insert(summary.column_ordinals).second ||
        !MultiColumnSummaryValid(summary)) {
      return false;
    }
  }
  return true;
}

bool PageValid(const PhysicalZoneSummaryPage& page) {
  if (!PageExtentSummaryUuidTextValid(page.relation_uuid) ||
      !PageExtentSummaryUuidTextValid(page.summary_uuid) ||
      !SameFormatVersion(page.format_version,
                         {kPhysicalZoneSummaryCurrentMajor,
                          kPhysicalZoneSummaryCurrentMinor}) ||
      !RangeSizingValid(page.range_sizing) ||
      page.base_generation == 0 ||
      page.summary_generation == 0) {
    return false;
  }
  u64 last_first_page = 0;
  bool seen = false;
  for (const auto& range : page.ranges) {
    if (!RangeRecordValid(range)) {
      return false;
    }
    if (seen && range.range.kind == PageExtentSummaryRangeKind::page_range &&
        range.range.first_page_id <= last_first_page) {
      return false;
    }
    if (range.range.kind == PageExtentSummaryRangeKind::page_range) {
      last_first_page = range.range.first_page_id;
      seen = true;
    }
  }
  return true;
}

void SortPage(PhysicalZoneSummaryPage* page) {
  std::sort(page->ranges.begin(),
            page->ranges.end(),
            [](const PhysicalZoneRangeSummaryRecord& left,
               const PhysicalZoneRangeSummaryRecord& right) {
              if (left.range.kind != right.range.kind) {
                return static_cast<u32>(left.range.kind) <
                       static_cast<u32>(right.range.kind);
              }
              if (left.range.kind == PageExtentSummaryRangeKind::page_range) {
                return left.range.first_page_id < right.range.first_page_id;
              }
              return left.range.first_extent_id < right.range.first_extent_id;
            });
  for (auto& range : page->ranges) {
    std::sort(range.columns.begin(),
              range.columns.end(),
              [](const PhysicalZoneColumnSummary& left,
                 const PhysicalZoneColumnSummary& right) {
                return left.column_ordinal < right.column_ordinal;
              });
    std::sort(range.multi_columns.begin(),
              range.multi_columns.end(),
              [](const PhysicalZoneMultiColumnSummary& left,
                 const PhysicalZoneMultiColumnSummary& right) {
                return left.column_ordinals < right.column_ordinals;
              });
    for (auto& summary : range.multi_columns) {
      std::sort(summary.small_set_values.begin(),
                summary.small_set_values.end(),
                [](const std::string& left, const std::string& right) {
                  return KeyLess(left, right);
                });
    }
  }
  std::sort(page->evidence.begin(), page->evidence.end());
}

PhysicalZoneRangeSummaryRecord SeedRange(const PhysicalZoneSummaryBuildRequest& request,
                                         u64 first_page_id) {
  PhysicalZoneRangeSummaryRecord record;
  record.range = PageRange(first_page_id, request.range_sizing.target_pages_per_range);
  record.range_sizing = request.range_sizing;
  record.row_count = 0;
  record.base_generation = request.range_sizing.base_generation;
  record.summary_generation = request.range_sizing.summary_generation;
  record.status = PageExtentSummaryStatus::current;
  record.persisted_record_present = true;
  record.checksum_valid = true;
  return record;
}

bool ApplyRowToRange(PhysicalZoneRangeSummaryRecord* range,
                     const PhysicalZoneRowEvidence& row,
                     u32 small_set_limit) {
  ++range->row_count;
  range->base_generation = std::max(range->base_generation, row.base_generation);
  for (const auto& value : row.columns) {
    auto iter = std::find_if(range->columns.begin(),
                             range->columns.end(),
                             [&](const PhysicalZoneColumnSummary& column) {
                               return column.column_ordinal == value.column_ordinal;
                             });
    if (iter == range->columns.end()) {
      PhysicalZoneColumnSummary summary;
      summary.column_ordinal = value.column_ordinal;
      range->columns.push_back(std::move(summary));
      iter = std::prev(range->columns.end());
    }
    if (!ApplyColumnValue(&(*iter), value, small_set_limit)) {
      return false;
    }
  }
  return ApplyMultiColumnValue(range, row, small_set_limit);
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

void AppendString(std::vector<byte>* out, std::string_view value) {
  AppendU32(out, static_cast<u32>(value.size()));
  out->insert(out->end(), value.begin(), value.end());
}

void AppendRange(std::vector<byte>* out, const PageExtentSummaryRange& range) {
  AppendU32(out, static_cast<u32>(range.kind));
  AppendU64(out, range.first_page_id);
  AppendU32(out, range.page_count);
  AppendU64(out, range.first_extent_id);
  AppendU32(out, range.extent_count);
}

void AppendRangeSizing(std::vector<byte>* out,
                       const PhysicalZoneRangeSizingMetadata& sizing) {
  AppendU32(out, sizing.min_pages_per_range);
  AppendU32(out, sizing.target_pages_per_range);
  AppendU32(out, sizing.max_pages_per_range);
  AppendU64(out, sizing.base_generation);
  AppendU64(out, sizing.summary_generation);
  AppendU8(out, sizing.adaptive ? 1 : 0);
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

bool ReadRange(Reader* reader, PageExtentSummaryRange* range) {
  u32 kind = 0;
  if (!reader->ReadU32(&kind) ||
      !reader->ReadU64(&range->first_page_id) ||
      !reader->ReadU32(&range->page_count) ||
      !reader->ReadU64(&range->first_extent_id) ||
      !reader->ReadU32(&range->extent_count)) {
    return false;
  }
  range->kind = static_cast<PageExtentSummaryRangeKind>(kind);
  return true;
}

bool ReadRangeSizing(Reader* reader, PhysicalZoneRangeSizingMetadata* sizing) {
  byte adaptive = 0;
  if (!reader->ReadU32(&sizing->min_pages_per_range) ||
      !reader->ReadU32(&sizing->target_pages_per_range) ||
      !reader->ReadU32(&sizing->max_pages_per_range) ||
      !reader->ReadU64(&sizing->base_generation) ||
      !reader->ReadU64(&sizing->summary_generation) ||
      !reader->ReadU8(&adaptive)) {
    return false;
  }
  sizing->adaptive = adaptive != 0;
  return true;
}

PhysicalZoneSummaryOpenResult OpenFailure(PhysicalZoneSummaryOpenClass open_class,
                                          std::string code,
                                          std::string key,
                                          std::string detail = {}) {
  PhysicalZoneSummaryOpenResult result;
  result.status = open_class == PhysicalZoneSummaryOpenClass::stale_generation ||
                          open_class == PhysicalZoneSummaryOpenClass::stale_format
                      ? WarnStatus()
                      : ErrorStatus();
  result.open_class = open_class;
  result.full_scan_required = true;
  result.restricted_repair_required =
      open_class == PhysicalZoneSummaryOpenClass::bad_checksum ||
      open_class == PhysicalZoneSummaryOpenClass::corrupt_payload;
  result.diagnostic = MakePhysicalZoneSummaryDiagnostic(
      result.status, std::move(code), std::move(key), std::move(detail));
  result.actions.push_back("physical_zone_summary_full_scan_fallback");
  if (result.restricted_repair_required) {
    result.actions.push_back("repair_requires_admitted_authoritative_base_evidence");
  }
  return result;
}

PhysicalZoneColumnSummary* FindColumn(PhysicalZoneRangeSummaryRecord* range,
                                      u32 ordinal) {
  auto iter = std::find_if(range->columns.begin(),
                           range->columns.end(),
                           [ordinal](const PhysicalZoneColumnSummary& column) {
                             return column.column_ordinal == ordinal;
                           });
  return iter == range->columns.end() ? nullptr : &(*iter);
}

const PhysicalZoneColumnSummary* FindColumn(const PhysicalZoneRangeSummaryRecord& range,
                                            u32 ordinal) {
  auto iter = std::find_if(range.columns.begin(),
                           range.columns.end(),
                           [ordinal](const PhysicalZoneColumnSummary& column) {
                             return column.column_ordinal == ordinal;
                           });
  return iter == range.columns.end() ? nullptr : &(*iter);
}

PhysicalZoneRangeSummaryRecord* FindRangeForRow(PhysicalZoneSummaryPage* page,
                                                const PhysicalZoneRowEvidence& row) {
  auto iter = std::find_if(page->ranges.begin(),
                           page->ranges.end(),
                           [&](const PhysicalZoneRangeSummaryRecord& range) {
                             return RowInRange(row, range.range);
                           });
  return iter == page->ranges.end() ? nullptr : &(*iter);
}

bool RowTouchesBoundary(const PhysicalZoneRangeSummaryRecord& range,
                        const PhysicalZoneRowEvidence& row) {
  for (const auto& value : row.columns) {
    if (value.value_is_null) {
      continue;
    }
    const auto* summary = FindColumn(range, value.column_ordinal);
    if (summary == nullptr) {
      continue;
    }
    if ((summary->min_present && summary->encoded_min == value.encoded_scalar) ||
        (summary->max_present && summary->encoded_max == value.encoded_scalar)) {
      return true;
    }
    if (summary->small_set_exact &&
        std::find(summary->small_set_values.begin(),
                  summary->small_set_values.end(),
                  value.encoded_scalar) != summary->small_set_values.end()) {
      return true;
    }
  }
  return false;
}

void MarkRangeStale(PhysicalZoneRangeSummaryRecord* range) {
  range->status = PageExtentSummaryStatus::stale;
  range->summary_generation = NextGeneration(range->summary_generation);
}

PhysicalZoneSummaryBuildRequest RebuildRequestForPage(
    const PhysicalZoneSummaryPage& page,
    std::vector<PhysicalZoneRowEvidence> rows,
    u32 small_set_limit) {
  PhysicalZoneSummaryBuildRequest request;
  request.relation_uuid = page.relation_uuid;
  request.summary_uuid = page.summary_uuid;
  request.range_sizing = page.range_sizing;
  request.range_sizing.summary_generation = NextGeneration(page.summary_generation);
  request.small_set_limit = small_set_limit;
  request.base_page_rows = std::move(rows);
  return request;
}

bool PredicateValueOverlap(const PhysicalZonePredicate& predicate,
                           const std::string& encoded_value) {
  if (predicate.lower_present && KeyLess(encoded_value, predicate.encoded_lower)) {
    return false;
  }
  if (predicate.upper_present && KeyGreater(encoded_value, predicate.encoded_upper)) {
    return false;
  }
  return true;
}

bool PredicateOutsideMinMax(const PhysicalZonePredicate& predicate,
                            const PhysicalZoneColumnSummary& summary) {
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

const PhysicalZonePredicate* FindPredicate(
    const std::vector<PhysicalZonePredicate>& predicates,
    u32 ordinal) {
  auto iter = std::find_if(predicates.begin(),
                           predicates.end(),
                           [ordinal](const PhysicalZonePredicate& predicate) {
                             return predicate.column_ordinal == ordinal;
                           });
  return iter == predicates.end() ? nullptr : &(*iter);
}

bool CompositePredicateKey(const PhysicalZoneRangeSummaryRecord& range,
                           const PhysicalZoneMultiColumnSummary& summary,
                           const std::vector<PhysicalZonePredicate>& predicates,
                           bool use_lower_bound,
                           bool require_exact_equality,
                           std::string* encoded_key) {
  std::vector<PhysicalZoneColumnValueEvidence> values;
  values.reserve(summary.column_ordinals.size());
  for (u32 ordinal : summary.column_ordinals) {
    const auto* predicate = FindPredicate(predicates, ordinal);
    const auto* column = FindColumn(range, ordinal);
    if (predicate == nullptr || column == nullptr ||
        predicate->scalar_type_key != column->scalar_type_key) {
      return false;
    }
    if (require_exact_equality &&
        (!predicate->lower_present ||
         !predicate->upper_present ||
         predicate->encoded_lower != predicate->encoded_upper)) {
      return false;
    }
    if (use_lower_bound && !predicate->lower_present) {
      return false;
    }
    if (!use_lower_bound && !predicate->upper_present) {
      return false;
    }
    PhysicalZoneColumnValueEvidence value;
    value.column_ordinal = ordinal;
    value.scalar_type_key = predicate->scalar_type_key;
    value.encoded_scalar =
        use_lower_bound ? predicate->encoded_lower : predicate->encoded_upper;
    if (!ColumnValueValid(value)) {
      return false;
    }
    values.push_back(std::move(value));
  }
  bool any_null = false;
  std::vector<u32> ordinals;
  *encoded_key = CompositeEncodedKey(values, &ordinals, &any_null);
  return !any_null &&
         ordinals == summary.column_ordinals &&
         IsOrderPreservingIndexKeyEncoding(*encoded_key) &&
         !IsUnsafeLegacyIndexKeyEncoding(*encoded_key);
}

bool TryCompositePrune(const PhysicalZoneRangeSummaryRecord& range,
                       const std::vector<PhysicalZonePredicate>& predicates,
                       PhysicalZoneRangePruneEvidence* evidence) {
  for (const auto& summary : range.multi_columns) {
    std::string lower_key;
    if (summary.max_present &&
        CompositePredicateKey(range, summary, predicates, true, false, &lower_key) &&
        KeyGreater(lower_key, summary.encoded_max_tuple)) {
      evidence->decision = PhysicalZoneSummaryRangeDecision::pruned_by_min_max;
      evidence->scan_required = false;
      evidence->reason = "composite_predicate_outside_min_max";
      return true;
    }
    std::string upper_key;
    if (summary.min_present &&
        CompositePredicateKey(range, summary, predicates, false, false, &upper_key) &&
        KeyLess(upper_key, summary.encoded_min_tuple)) {
      evidence->decision = PhysicalZoneSummaryRangeDecision::pruned_by_min_max;
      evidence->scan_required = false;
      evidence->reason = "composite_predicate_outside_min_max";
      return true;
    }
    std::string exact_key;
    if (summary.small_set_exact &&
        !summary.small_set_values.empty() &&
        CompositePredicateKey(range, summary, predicates, true, true, &exact_key) &&
        std::find(summary.small_set_values.begin(),
                  summary.small_set_values.end(),
                  exact_key) == summary.small_set_values.end()) {
      evidence->decision = PhysicalZoneSummaryRangeDecision::pruned_by_small_set;
      evidence->scan_required = false;
      evidence->reason = "composite_predicate_absent_from_exact_small_set";
      return true;
    }
  }
  return false;
}

}  // namespace

PhysicalZoneSummaryBuildResult BuildPhysicalZoneSummaryFromBasePageEvidence(
    const PhysicalZoneSummaryBuildRequest& request) {
  if (!PageExtentSummaryUuidTextValid(request.relation_uuid) ||
      !PageExtentSummaryUuidTextValid(request.summary_uuid)) {
    return BuildFailure("INDEX.PHYSICAL_ZONE_SUMMARY.INVALID_IDENTITY",
                        "index.physical_zone_summary.invalid_identity");
  }
  if (!RangeSizingValid(request.range_sizing)) {
    return BuildFailure("INDEX.PHYSICAL_ZONE_SUMMARY.INVALID_RANGE_SIZING",
                        "index.physical_zone_summary.invalid_range_sizing");
  }

  PhysicalZoneSummaryPage page;
  page.relation_uuid = request.relation_uuid;
  page.summary_uuid = request.summary_uuid;
  page.range_sizing = request.range_sizing;
  page.base_generation = request.range_sizing.base_generation;
  page.summary_generation = request.range_sizing.summary_generation;
  page.evidence.push_back("built_from_authoritative_base_page_evidence");
  page.evidence.push_back("mga_recheck_required=true");
  page.evidence.push_back("security_recheck_required=true");
  page.evidence.push_back("summary_visibility_authority=false");
  page.evidence.push_back("parser_donor_provider_finality_authority=false");
  page.evidence.push_back("write_ahead_log_finality_authority=false");

  std::map<u64, PhysicalZoneRangeSummaryRecord> ranges;
  std::vector<PhysicalZoneRowEvidence> rows = request.base_page_rows;
  std::sort(rows.begin(),
            rows.end(),
            [](const PhysicalZoneRowEvidence& left,
               const PhysicalZoneRowEvidence& right) {
              if (left.page_id != right.page_id) {
                return left.page_id < right.page_id;
              }
              return left.extent_id < right.extent_id;
            });

  for (const auto& row : rows) {
    if (!row.engine_mga_visible) {
      continue;
    }
    if (!RowEvidenceValid(row)) {
      return BuildFailure("INDEX.PHYSICAL_ZONE_SUMMARY.INVALID_BASE_EVIDENCE",
                          "index.physical_zone_summary.invalid_base_evidence");
    }
    const u64 first_page_id =
        RangeStartForPage(row.page_id, request.range_sizing.target_pages_per_range);
    auto inserted = ranges.emplace(first_page_id, SeedRange(request, first_page_id));
    if (!ApplyRowToRange(&inserted.first->second, row, request.small_set_limit)) {
      return BuildFailure("INDEX.PHYSICAL_ZONE_SUMMARY.COLUMN_TYPE_MISMATCH",
                          "index.physical_zone_summary.column_type_mismatch");
    }
    page.base_generation = std::max(page.base_generation, row.base_generation);
  }

  for (auto& entry : ranges) {
    auto& range = entry.second;
    range.base_generation = std::max(range.base_generation, page.base_generation);
    range.range_sizing.base_generation = page.base_generation;
    if (!RangeRecordValid(range)) {
      return BuildFailure("INDEX.PHYSICAL_ZONE_SUMMARY.INVALID_RANGE_RECORD",
                          "index.physical_zone_summary.invalid_range_record");
    }
    page.ranges.push_back(std::move(range));
  }
  page.range_sizing.base_generation = page.base_generation;
  SortPage(&page);

  if (!PageValid(page)) {
    return BuildFailure("INDEX.PHYSICAL_ZONE_SUMMARY.INVALID_PAGE",
                        "index.physical_zone_summary.invalid_page");
  }

  PhysicalZoneSummaryBuildResult result;
  result.status = OkStatus();
  result.page = std::move(page);
  result.built = true;
  result.evidence = result.page.evidence;
  result.diagnostic = MakePhysicalZoneSummaryDiagnostic(
      result.status,
      "INDEX.PHYSICAL_ZONE_SUMMARY.BUILT",
      "index.physical_zone_summary.built");
  return result;
}

PhysicalZoneSummarySerializeResult SerializePhysicalZoneSummaryPage(
    const PhysicalZoneSummaryPage& input_page) {
  PhysicalZoneSummarySerializeResult result;
  auto page = input_page;
  SortPage(&page);
  if (!PageValid(page)) {
    result.status = ErrorStatus();
    result.diagnostic = MakePhysicalZoneSummaryDiagnostic(
        result.status,
        "INDEX.PHYSICAL_ZONE_SUMMARY.SERIALIZE_REFUSED",
        "index.physical_zone_summary.serialize_refused");
    return result;
  }

  auto& out = result.bytes;
  out.insert(out.end(), kMagic.begin(), kMagic.end());
  AppendU32(&out, page.format_version.major);
  AppendU32(&out, page.format_version.minor);
  AppendU64(&out, 0);
  AppendString(&out, page.relation_uuid);
  AppendString(&out, page.summary_uuid);
  AppendU64(&out, page.base_generation);
  AppendU64(&out, page.summary_generation);
  AppendRangeSizing(&out, page.range_sizing);
  AppendU32(&out, static_cast<u32>(page.ranges.size()));
  for (const auto& range : page.ranges) {
    AppendRange(&out, range.range);
    AppendRangeSizing(&out, range.range_sizing);
    AppendU64(&out, range.row_count);
    AppendU64(&out, range.base_generation);
    AppendU64(&out, range.summary_generation);
    AppendU32(&out, static_cast<u32>(range.status));
    AppendU8(&out, range.persisted_record_present ? 1 : 0);
    AppendU8(&out, range.checksum_valid ? 1 : 0);
    AppendU8(&out, range.mga_recheck_required ? 1 : 0);
    AppendU8(&out, range.security_recheck_required ? 1 : 0);
    AppendU8(&out, range.parser_finality_authority_claimed ? 1 : 0);
    AppendU8(&out, range.donor_finality_authority_claimed ? 1 : 0);
    AppendU8(&out, range.write_ahead_log_finality_authority_claimed ? 1 : 0);
    AppendU32(&out, static_cast<u32>(range.columns.size()));
    for (const auto& column : range.columns) {
      AppendU32(&out, column.column_ordinal);
      AppendString(&out, column.scalar_type_key);
      AppendU64(&out, column.row_count);
      AppendU64(&out, column.null_count);
      AppendU8(&out, column.min_present ? 1 : 0);
      AppendU8(&out, column.max_present ? 1 : 0);
      AppendString(&out, column.encoded_min);
      AppendString(&out, column.encoded_max);
      AppendU8(&out, column.small_set_exact ? 1 : 0);
      AppendU8(&out, column.small_set_overflow ? 1 : 0);
      AppendU32(&out, static_cast<u32>(column.small_set_values.size()));
      for (const auto& value : column.small_set_values) {
        AppendString(&out, value);
      }
    }
    AppendU32(&out, static_cast<u32>(range.multi_columns.size()));
    for (const auto& summary : range.multi_columns) {
      AppendU32(&out, static_cast<u32>(summary.column_ordinals.size()));
      for (u32 ordinal : summary.column_ordinals) {
        AppendU32(&out, ordinal);
      }
      AppendU64(&out, summary.row_count);
      AppendU64(&out, summary.null_tuple_count);
      AppendU8(&out, summary.min_present ? 1 : 0);
      AppendU8(&out, summary.max_present ? 1 : 0);
      AppendString(&out, summary.encoded_min_tuple);
      AppendString(&out, summary.encoded_max_tuple);
      AppendU8(&out, summary.small_set_exact ? 1 : 0);
      AppendU8(&out, summary.small_set_overflow ? 1 : 0);
      AppendU32(&out, static_cast<u32>(summary.small_set_values.size()));
      for (const auto& value : summary.small_set_values) {
        AppendString(&out, value);
      }
    }
  }
  AppendU32(&out, static_cast<u32>(page.evidence.size()));
  for (const auto& evidence : page.evidence) {
    AppendString(&out, evidence);
  }

  result.checksum = ComputeChecksum(out);
  scratchbird::core::platform::StoreLittle64(out.data() + 16, result.checksum);
  result.status = OkStatus();
  result.diagnostic = MakePhysicalZoneSummaryDiagnostic(
      result.status,
      "INDEX.PHYSICAL_ZONE_SUMMARY.SERIALIZED",
      "index.physical_zone_summary.serialized");
  return result;
}

PhysicalZoneSummaryOpenResult OpenPhysicalZoneSummaryPage(
    const PhysicalZoneSummaryOpenRequest& request) {
  if (request.bytes.size() < kHeaderBytes ||
      !std::equal(kMagic.begin(), kMagic.end(), request.bytes.begin())) {
    return OpenFailure(PhysicalZoneSummaryOpenClass::corrupt_payload,
                       "INDEX.PHYSICAL_ZONE_SUMMARY.BAD_MAGIC",
                       "index.physical_zone_summary.bad_magic");
  }

  Reader reader(request.bytes);
  reader.SetOffset(8);
  u32 major = 0;
  u32 minor = 0;
  u64 stored_checksum = 0;
  if (!reader.ReadU32(&major) ||
      !reader.ReadU32(&minor) ||
      !reader.ReadU64(&stored_checksum)) {
    return OpenFailure(PhysicalZoneSummaryOpenClass::corrupt_payload,
                       "INDEX.PHYSICAL_ZONE_SUMMARY.TRUNCATED_HEADER",
                       "index.physical_zone_summary.truncated_header");
  }
  if (major != kPhysicalZoneSummaryCurrentMajor ||
      minor != kPhysicalZoneSummaryCurrentMinor) {
    return OpenFailure(PhysicalZoneSummaryOpenClass::stale_format,
                       "INDEX.PHYSICAL_ZONE_SUMMARY.STALE_FORMAT",
                       "index.physical_zone_summary.stale_format");
  }
  if (stored_checksum == 0 || ComputeChecksum(request.bytes) != stored_checksum) {
    return OpenFailure(PhysicalZoneSummaryOpenClass::bad_checksum,
                       "INDEX.PHYSICAL_ZONE_SUMMARY.BAD_CHECKSUM",
                       "index.physical_zone_summary.bad_checksum");
  }

  PhysicalZoneSummaryPage page;
  page.format_version = {major, minor};
  if (!reader.ReadString(&page.relation_uuid) ||
      !reader.ReadString(&page.summary_uuid) ||
      !reader.ReadU64(&page.base_generation) ||
      !reader.ReadU64(&page.summary_generation) ||
      !ReadRangeSizing(&reader, &page.range_sizing)) {
    return OpenFailure(PhysicalZoneSummaryOpenClass::corrupt_payload,
                       "INDEX.PHYSICAL_ZONE_SUMMARY.TRUNCATED_PAYLOAD",
                       "index.physical_zone_summary.truncated_payload");
  }
  u32 range_count = 0;
  if (!reader.ReadU32(&range_count)) {
    return OpenFailure(PhysicalZoneSummaryOpenClass::corrupt_payload,
                       "INDEX.PHYSICAL_ZONE_SUMMARY.TRUNCATED_RANGES",
                       "index.physical_zone_summary.truncated_ranges");
  }
  for (u32 range_index = 0; range_index < range_count; ++range_index) {
    PhysicalZoneRangeSummaryRecord range;
    u32 status = 0;
    byte persisted = 0;
    byte checksum_valid = 0;
    byte mga_recheck = 0;
    byte security_recheck = 0;
    byte parser_authority = 0;
    byte donor_authority = 0;
    byte log_authority = 0;
    u32 column_count = 0;
    if (!ReadRange(&reader, &range.range) ||
        !ReadRangeSizing(&reader, &range.range_sizing) ||
        !reader.ReadU64(&range.row_count) ||
        !reader.ReadU64(&range.base_generation) ||
        !reader.ReadU64(&range.summary_generation) ||
        !reader.ReadU32(&status) ||
        !reader.ReadU8(&persisted) ||
        !reader.ReadU8(&checksum_valid) ||
        !reader.ReadU8(&mga_recheck) ||
        !reader.ReadU8(&security_recheck) ||
        !reader.ReadU8(&parser_authority) ||
        !reader.ReadU8(&donor_authority) ||
        !reader.ReadU8(&log_authority) ||
        !reader.ReadU32(&column_count)) {
      return OpenFailure(PhysicalZoneSummaryOpenClass::corrupt_payload,
                         "INDEX.PHYSICAL_ZONE_SUMMARY.TRUNCATED_RANGE",
                         "index.physical_zone_summary.truncated_range");
    }
    range.status = static_cast<PageExtentSummaryStatus>(status);
    range.persisted_record_present = persisted != 0;
    range.checksum_valid = checksum_valid != 0;
    range.mga_recheck_required = mga_recheck != 0;
    range.security_recheck_required = security_recheck != 0;
    range.parser_finality_authority_claimed = parser_authority != 0;
    range.donor_finality_authority_claimed = donor_authority != 0;
    range.write_ahead_log_finality_authority_claimed = log_authority != 0;
    for (u32 column_index = 0; column_index < column_count; ++column_index) {
      PhysicalZoneColumnSummary column;
      byte min_present = 0;
      byte max_present = 0;
      byte small_set_exact = 0;
      byte small_set_overflow = 0;
      u32 small_set_count = 0;
      if (!reader.ReadU32(&column.column_ordinal) ||
          !reader.ReadString(&column.scalar_type_key) ||
          !reader.ReadU64(&column.row_count) ||
          !reader.ReadU64(&column.null_count) ||
          !reader.ReadU8(&min_present) ||
          !reader.ReadU8(&max_present) ||
          !reader.ReadString(&column.encoded_min) ||
          !reader.ReadString(&column.encoded_max) ||
          !reader.ReadU8(&small_set_exact) ||
          !reader.ReadU8(&small_set_overflow) ||
          !reader.ReadU32(&small_set_count)) {
        return OpenFailure(PhysicalZoneSummaryOpenClass::corrupt_payload,
                           "INDEX.PHYSICAL_ZONE_SUMMARY.TRUNCATED_COLUMN",
                           "index.physical_zone_summary.truncated_column");
      }
      column.min_present = min_present != 0;
      column.max_present = max_present != 0;
      column.small_set_exact = small_set_exact != 0;
      column.small_set_overflow = small_set_overflow != 0;
      for (u32 value_index = 0; value_index < small_set_count; ++value_index) {
        std::string value;
        if (!reader.ReadString(&value)) {
          return OpenFailure(PhysicalZoneSummaryOpenClass::corrupt_payload,
                             "INDEX.PHYSICAL_ZONE_SUMMARY.TRUNCATED_SMALL_SET",
                             "index.physical_zone_summary.truncated_small_set");
        }
        column.small_set_values.push_back(std::move(value));
      }
      range.columns.push_back(std::move(column));
    }
    u32 multi_column_count = 0;
    if (!reader.ReadU32(&multi_column_count)) {
      return OpenFailure(PhysicalZoneSummaryOpenClass::corrupt_payload,
                         "INDEX.PHYSICAL_ZONE_SUMMARY.TRUNCATED_MULTI_COLUMNS",
                         "index.physical_zone_summary.truncated_multi_columns");
    }
    for (u32 summary_index = 0; summary_index < multi_column_count; ++summary_index) {
      PhysicalZoneMultiColumnSummary summary;
      u32 ordinal_count = 0;
      byte min_present = 0;
      byte max_present = 0;
      byte small_set_exact = 0;
      byte small_set_overflow = 0;
      u32 small_set_count = 0;
      if (!reader.ReadU32(&ordinal_count)) {
        return OpenFailure(PhysicalZoneSummaryOpenClass::corrupt_payload,
                           "INDEX.PHYSICAL_ZONE_SUMMARY.TRUNCATED_MULTI_SHAPE",
                           "index.physical_zone_summary.truncated_multi_shape");
      }
      for (u32 ordinal_index = 0; ordinal_index < ordinal_count; ++ordinal_index) {
        u32 ordinal = 0;
        if (!reader.ReadU32(&ordinal)) {
          return OpenFailure(PhysicalZoneSummaryOpenClass::corrupt_payload,
                             "INDEX.PHYSICAL_ZONE_SUMMARY.TRUNCATED_MULTI_ORDINAL",
                             "index.physical_zone_summary.truncated_multi_ordinal");
        }
        summary.column_ordinals.push_back(ordinal);
      }
      if (!reader.ReadU64(&summary.row_count) ||
          !reader.ReadU64(&summary.null_tuple_count) ||
          !reader.ReadU8(&min_present) ||
          !reader.ReadU8(&max_present) ||
          !reader.ReadString(&summary.encoded_min_tuple) ||
          !reader.ReadString(&summary.encoded_max_tuple) ||
          !reader.ReadU8(&small_set_exact) ||
          !reader.ReadU8(&small_set_overflow) ||
          !reader.ReadU32(&small_set_count)) {
        return OpenFailure(PhysicalZoneSummaryOpenClass::corrupt_payload,
                           "INDEX.PHYSICAL_ZONE_SUMMARY.TRUNCATED_MULTI_COLUMN",
                           "index.physical_zone_summary.truncated_multi_column");
      }
      summary.min_present = min_present != 0;
      summary.max_present = max_present != 0;
      summary.small_set_exact = small_set_exact != 0;
      summary.small_set_overflow = small_set_overflow != 0;
      for (u32 value_index = 0; value_index < small_set_count; ++value_index) {
        std::string value;
        if (!reader.ReadString(&value)) {
          return OpenFailure(PhysicalZoneSummaryOpenClass::corrupt_payload,
                             "INDEX.PHYSICAL_ZONE_SUMMARY.TRUNCATED_MULTI_SMALL_SET",
                             "index.physical_zone_summary.truncated_multi_small_set");
        }
        summary.small_set_values.push_back(std::move(value));
      }
      range.multi_columns.push_back(std::move(summary));
    }
    page.ranges.push_back(std::move(range));
  }

  u32 evidence_count = 0;
  if (!reader.ReadU32(&evidence_count)) {
    return OpenFailure(PhysicalZoneSummaryOpenClass::corrupt_payload,
                       "INDEX.PHYSICAL_ZONE_SUMMARY.TRUNCATED_EVIDENCE",
                       "index.physical_zone_summary.truncated_evidence");
  }
  for (u32 index = 0; index < evidence_count; ++index) {
    std::string evidence;
    if (!reader.ReadString(&evidence)) {
      return OpenFailure(PhysicalZoneSummaryOpenClass::corrupt_payload,
                         "INDEX.PHYSICAL_ZONE_SUMMARY.TRUNCATED_EVIDENCE_ENTRY",
                         "index.physical_zone_summary.truncated_evidence_entry");
    }
    page.evidence.push_back(std::move(evidence));
  }
  if (!reader.Done() || !PageValid(page)) {
    return OpenFailure(PhysicalZoneSummaryOpenClass::corrupt_payload,
                       "INDEX.PHYSICAL_ZONE_SUMMARY.INVALID_PAYLOAD",
                       "index.physical_zone_summary.invalid_payload");
  }
  if (request.expected_base_generation_present &&
      page.base_generation != request.expected_base_generation) {
    auto result = OpenFailure(PhysicalZoneSummaryOpenClass::stale_generation,
                              "INDEX.PHYSICAL_ZONE_SUMMARY.STALE_GENERATION",
                              "index.physical_zone_summary.stale_generation");
    result.page = std::move(page);
    result.actions.push_back("rebuild_summary_from_authoritative_base_pages");
    return result;
  }

  PhysicalZoneSummaryOpenResult result;
  result.status = OkStatus();
  result.open_class = PhysicalZoneSummaryOpenClass::current;
  result.full_scan_required = false;
  result.page = std::move(page);
  result.actions.push_back("physical_zone_summary_opened_clean");
  result.actions.push_back("candidate_rows_require_mga_and_security_recheck");
  result.diagnostic = MakePhysicalZoneSummaryDiagnostic(
      result.status,
      "INDEX.PHYSICAL_ZONE_SUMMARY.OPENED",
      "index.physical_zone_summary.opened");
  return result;
}

PhysicalZonePruneResult PrunePhysicalZoneSummaryRanges(
    const PhysicalZonePruneRequest& request) {
  PhysicalZonePruneResult result;
  if (!PageValid(request.page)) {
    result.status = WarnStatus();
    result.full_scan_required = true;
    result.diagnostic = MakePhysicalZoneSummaryDiagnostic(
        result.status,
        "INDEX.PHYSICAL_ZONE_SUMMARY.PRUNE_FULL_SCAN",
        "index.physical_zone_summary.prune_full_scan");
    result.evidence.push_back("invalid_summary_full_scan_required");
    return result;
  }
  for (const auto& predicate : request.predicates) {
    if ((predicate.lower_present &&
         (!IsOrderPreservingIndexKeyEncoding(predicate.encoded_lower) ||
          IsUnsafeLegacyIndexKeyEncoding(predicate.encoded_lower))) ||
        (predicate.upper_present &&
         (!IsOrderPreservingIndexKeyEncoding(predicate.encoded_upper) ||
          IsUnsafeLegacyIndexKeyEncoding(predicate.encoded_upper)))) {
      result.status = WarnStatus();
      result.full_scan_required = true;
      result.diagnostic = MakePhysicalZoneSummaryDiagnostic(
          result.status,
          "INDEX.PHYSICAL_ZONE_SUMMARY.UNSAFE_PREDICATE_KEY",
          "index.physical_zone_summary.unsafe_predicate_key");
      result.evidence.push_back("unsafe_predicate_key_full_scan_required");
      return result;
    }
  }

  result.status = OkStatus();
  result.diagnostic = MakePhysicalZoneSummaryDiagnostic(
      result.status,
      "INDEX.PHYSICAL_ZONE_SUMMARY.PRUNE_CLASSIFIED",
      "index.physical_zone_summary.prune_classified");
  result.evidence.push_back("summary_prunes_candidate_ranges_only");
  result.evidence.push_back("mga_recheck_required=true");
  result.evidence.push_back("security_recheck_required=true");
  result.evidence.push_back("visibility_finality_authority=false");
  result.evidence.push_back("parser_donor_provider_finality_authority=false");
  result.evidence.push_back("write_ahead_log_finality_authority=false");

  for (const auto& range : request.page.ranges) {
    PhysicalZoneRangePruneEvidence evidence;
    evidence.range = range.range;
    evidence.mga_recheck_required = range.mga_recheck_required;
    evidence.security_recheck_required = range.security_recheck_required;
    if (range.status != PageExtentSummaryStatus::current) {
      evidence.decision = PhysicalZoneSummaryRangeDecision::full_scan_fallback;
      evidence.reason = "range_not_current";
      result.full_scan_required = true;
      result.ranges.push_back(std::move(evidence));
      continue;
    }
    for (const auto& predicate : request.predicates) {
      const auto* column = FindColumn(range, predicate.column_ordinal);
      if (column == nullptr || column->scalar_type_key != predicate.scalar_type_key) {
        continue;
      }
      if (PredicateOutsideMinMax(predicate, *column)) {
        evidence.decision = PhysicalZoneSummaryRangeDecision::pruned_by_min_max;
        evidence.scan_required = false;
        evidence.reason = "predicate_outside_min_max";
        result.any_pruned = true;
        break;
      }
      if (column->small_set_exact && !column->small_set_values.empty()) {
        const bool any_overlap = std::any_of(
            column->small_set_values.begin(),
            column->small_set_values.end(),
            [&](const std::string& value) {
              return PredicateValueOverlap(predicate, value);
            });
        if (!any_overlap) {
          evidence.decision = PhysicalZoneSummaryRangeDecision::pruned_by_small_set;
          evidence.scan_required = false;
          evidence.reason = "predicate_absent_from_exact_small_set";
          result.any_pruned = true;
          break;
        }
      }
    }
    if (evidence.scan_required &&
        TryCompositePrune(range, request.predicates, &evidence)) {
      result.any_pruned = true;
    }
    if (evidence.scan_required) {
      evidence.decision = PhysicalZoneSummaryRangeDecision::scan_with_recheck;
      evidence.reason = "range_may_match_recheck_required";
    }
    result.ranges.push_back(std::move(evidence));
  }
  return result;
}

PhysicalZoneSummaryMutationResult ApplyPhysicalZoneSummaryMutation(
    const PhysicalZoneSummaryPage& page,
    const PhysicalZoneSummaryMutation& mutation,
    u32 small_set_limit) {
  if (!PageValid(page)) {
    return MutationFailure(page,
                           "INDEX.PHYSICAL_ZONE_SUMMARY.MUTATION_INVALID_PAGE",
                           "index.physical_zone_summary.mutation_invalid_page");
  }
  if ((mutation.before_row_present && !RowEvidenceValid(mutation.before_row)) ||
      (mutation.after_row_present && !RowEvidenceValid(mutation.after_row))) {
    return MutationFailure(page,
                           "INDEX.PHYSICAL_ZONE_SUMMARY.MUTATION_INVALID_EVIDENCE",
                           "index.physical_zone_summary.mutation_invalid_evidence");
  }

  auto working = page;
  auto rebuild_if_admitted = [&]() -> PhysicalZoneSummaryMutationResult {
    if (!mutation.rebuild_admitted || mutation.authoritative_base_page_rows.empty()) {
      PhysicalZoneSummaryMutationResult result = MutationFailure(
          working,
          "INDEX.PHYSICAL_ZONE_SUMMARY.MUTATION_STALE_REBUILD_REQUIRED",
          "index.physical_zone_summary.mutation_stale_rebuild_required");
      result.actions.push_back("summary_marked_stale_full_scan_until_rebuild");
      return result;
    }
    return RepairPhysicalZoneSummaryFromBasePageEvidence(
        working, mutation.authoritative_base_page_rows, true, small_set_limit);
  };

  switch (mutation.kind) {
    case PhysicalZoneSummaryMutationKind::append_row: {
      if (!mutation.after_row_present || !mutation.after_row.engine_mga_visible) {
        return MutationFailure(working,
                               "INDEX.PHYSICAL_ZONE_SUMMARY.APPEND_EVIDENCE_MISSING",
                               "index.physical_zone_summary.append_evidence_missing");
      }
      auto* range = FindRangeForRow(&working, mutation.after_row);
      if (range == nullptr) {
        PhysicalZoneSummaryBuildRequest seed;
        seed.relation_uuid = working.relation_uuid;
        seed.summary_uuid = working.summary_uuid;
        seed.range_sizing = working.range_sizing;
        seed.small_set_limit = small_set_limit;
        const u64 first_page_id =
            RangeStartForPage(mutation.after_row.page_id,
                              working.range_sizing.target_pages_per_range);
        working.ranges.push_back(SeedRange(seed, first_page_id));
        range = &working.ranges.back();
      }
      if (!ApplyRowToRange(range, mutation.after_row, small_set_limit)) {
        MarkRangeStale(range);
        return rebuild_if_admitted();
      }
      range->summary_generation = NextGeneration(range->summary_generation);
      working.base_generation = std::max(working.base_generation,
                                         mutation.after_row.base_generation);
      working.summary_generation = NextGeneration(working.summary_generation);
      working.range_sizing.base_generation = working.base_generation;
      SortPage(&working);
      break;
    }
    case PhysicalZoneSummaryMutationKind::delete_row: {
      if (!mutation.before_row_present) {
        return MutationFailure(working,
                               "INDEX.PHYSICAL_ZONE_SUMMARY.DELETE_EVIDENCE_MISSING",
                               "index.physical_zone_summary.delete_evidence_missing");
      }
      auto* range = FindRangeForRow(&working, mutation.before_row);
      if (range == nullptr) {
        break;
      }
      if (RowTouchesBoundary(*range, mutation.before_row)) {
        MarkRangeStale(range);
        return rebuild_if_admitted();
      }
      MarkRangeStale(range);
      return rebuild_if_admitted();
    }
    case PhysicalZoneSummaryMutationKind::update_row: {
      if (!mutation.before_row_present || !mutation.after_row_present) {
        return MutationFailure(working,
                               "INDEX.PHYSICAL_ZONE_SUMMARY.UPDATE_EVIDENCE_MISSING",
                               "index.physical_zone_summary.update_evidence_missing");
      }
      auto* before_range = FindRangeForRow(&working, mutation.before_row);
      auto* after_range = FindRangeForRow(&working, mutation.after_row);
      if (before_range == nullptr && after_range == nullptr) {
        break;
      }
      if (before_range != nullptr &&
          (before_range != after_range || RowTouchesBoundary(*before_range, mutation.before_row))) {
        MarkRangeStale(before_range);
        return rebuild_if_admitted();
      }
      if (after_range == nullptr) {
        return rebuild_if_admitted();
      }
      MarkRangeStale(after_range);
      return rebuild_if_admitted();
    }
  }

  if (!PageValid(working)) {
    return MutationFailure(working,
                           "INDEX.PHYSICAL_ZONE_SUMMARY.MUTATION_RESULT_INVALID",
                           "index.physical_zone_summary.mutation_result_invalid");
  }
  PhysicalZoneSummaryMutationResult result;
  result.status = OkStatus();
  result.page = std::move(working);
  result.applied = true;
  result.actions.push_back("physical_zone_summary_mutation_applied");
  result.actions.push_back("candidate_rows_require_mga_and_security_recheck");
  result.diagnostic = MakePhysicalZoneSummaryDiagnostic(
      result.status,
      "INDEX.PHYSICAL_ZONE_SUMMARY.MUTATION_APPLIED",
      "index.physical_zone_summary.mutation_applied");
  return result;
}

PhysicalZoneSummaryMutationResult RepairPhysicalZoneSummaryFromBasePageEvidence(
    const PhysicalZoneSummaryPage& stale_or_corrupt_page,
    const std::vector<PhysicalZoneRowEvidence>& authoritative_base_page_rows,
    bool repair_admitted,
    u32 small_set_limit) {
  if (!repair_admitted || authoritative_base_page_rows.empty()) {
    PhysicalZoneSummaryMutationResult result = MutationFailure(
        stale_or_corrupt_page,
        "INDEX.PHYSICAL_ZONE_SUMMARY.REPAIR_REFUSED",
        "index.physical_zone_summary.repair_refused");
    result.actions.push_back("repair_requires_explicit_admission_and_base_evidence");
    return result;
  }
  auto request = RebuildRequestForPage(
      stale_or_corrupt_page, authoritative_base_page_rows, small_set_limit);
  auto rebuilt = BuildPhysicalZoneSummaryFromBasePageEvidence(request);
  PhysicalZoneSummaryMutationResult result;
  result.status = rebuilt.status;
  result.diagnostic = rebuilt.diagnostic;
  result.page = std::move(rebuilt.page);
  result.applied = rebuilt.ok();
  result.rebuild_performed = rebuilt.ok();
  result.full_scan_required = !rebuilt.ok();
  result.actions.push_back(rebuilt.ok()
                               ? "physical_zone_summary_rebuilt_from_authoritative_base_evidence"
                               : "physical_zone_summary_rebuild_refused");
  return result;
}

const char* PhysicalZoneSummaryOpenClassName(PhysicalZoneSummaryOpenClass open_class) {
  switch (open_class) {
    case PhysicalZoneSummaryOpenClass::current: return "current";
    case PhysicalZoneSummaryOpenClass::stale_format: return "stale_format";
    case PhysicalZoneSummaryOpenClass::stale_generation: return "stale_generation";
    case PhysicalZoneSummaryOpenClass::bad_checksum: return "bad_checksum";
    case PhysicalZoneSummaryOpenClass::corrupt_payload: return "corrupt_payload";
    case PhysicalZoneSummaryOpenClass::refused: return "refused";
  }
  return "unknown";
}

const char* PhysicalZoneSummaryRangeDecisionName(
    PhysicalZoneSummaryRangeDecision decision) {
  switch (decision) {
    case PhysicalZoneSummaryRangeDecision::scan_with_recheck:
      return "scan_with_recheck";
    case PhysicalZoneSummaryRangeDecision::pruned_by_min_max:
      return "pruned_by_min_max";
    case PhysicalZoneSummaryRangeDecision::pruned_by_small_set:
      return "pruned_by_small_set";
    case PhysicalZoneSummaryRangeDecision::full_scan_fallback:
      return "full_scan_fallback";
  }
  return "unknown";
}

DiagnosticRecord MakePhysicalZoneSummaryDiagnostic(Status status,
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
                        "core.index.physical_zone_summary");
}

}  // namespace scratchbird::core::index
