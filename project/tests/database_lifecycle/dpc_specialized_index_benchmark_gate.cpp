// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "inverted_search_segment_publication.hpp"
#include "time_range_summary_pruning.hpp"
#include "uuid.hpp"
#include "vector_index_generation_publication.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace idx = scratchbird::core::index;
namespace platform = scratchbird::core::platform;
namespace uuid = scratchbird::core::uuid;

constexpr std::string_view kGateSearchKey =
    "DPC_SPECIALIZED_INDEX_BENCHMARK_GATE";
constexpr std::string_view kBenchmarkOutputSearchKey =
    "DPC_SPECIALIZED_INDEX_BENCHMARK_OUTPUT";
constexpr platform::u64 kRunCount = 5;

constexpr std::string_view kTimeRangeDisableFlag =
    "SCRATCHBIRD_TIME_RANGE_PRUNING";
constexpr std::string_view kInvertedSegmentsDisableFlag =
    "SCRATCHBIRD_INVERTED_SEGMENTS";
constexpr std::string_view kSearchMergeDisableFlag =
    "SCRATCHBIRD_SEARCH_SEGMENT_MERGE";
constexpr std::string_view kVectorGenerationDisableFlag =
    "SCRATCHBIRD_VECTOR_GENERATION";

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

platform::TypedUuid NewUuid(platform::UuidKind kind, platform::u64 seed) {
  const auto generated = uuid::GenerateEngineIdentityV7(kind, seed);
  Require(generated.ok(), "DPC-045 generated UUID creation failed");
  return generated.value;
}

std::string NewUuidText(platform::u64 seed) {
  return uuid::UuidToString(NewUuid(platform::UuidKind::object, seed).value);
}

bool SameUuid(const platform::TypedUuid& left,
              const platform::TypedUuid& right) {
  return left.kind == right.kind && left.value == right.value;
}

std::string UuidKey(const platform::TypedUuid& value) {
  return uuid::UuidToString(value.value);
}

std::uint64_t MixFnv1a(std::uint64_t hash, std::string_view text) {
  for (const unsigned char ch : text) {
    hash ^= ch;
    hash *= 1099511628211ull;
  }
  return hash;
}

std::string Hex(std::uint64_t value) {
  std::ostringstream out;
  out << "0x" << std::hex << std::nouppercase << value;
  return out.str();
}

std::string HashRows(const std::vector<std::string>& row_ids) {
  std::uint64_t hash = 1469598103934665603ull;
  for (const auto& row_id : row_ids) {
    hash = MixFnv1a(hash, row_id);
    hash = MixFnv1a(hash, ";");
  }
  return Hex(hash);
}

std::string Fixed(double value) {
  std::ostringstream out;
  out << std::fixed << std::setprecision(6) << value;
  return out.str();
}

double ReductionRatio(platform::u64 baseline, platform::u64 optimized) {
  Require(baseline > 0, "DPC-045 baseline counter was zero");
  Require(optimized <= baseline, "DPC-045 optimized counter exceeded baseline");
  return static_cast<double>(baseline - optimized) /
         static_cast<double>(baseline);
}

void RequireAuthorityProof(bool base_mga_recheck,
                           bool base_security_recheck,
                           bool metadata_visibility_authority,
                           bool metadata_finality_authority,
                           std::string_view message) {
  Require(base_mga_recheck, message);
  Require(base_security_recheck, message);
  Require(!metadata_visibility_authority, message);
  Require(!metadata_finality_authority, message);
}

idx::PageExtentSummaryFormatCompatibility CurrentSummaryFormat() {
  const auto contract = idx::PageExtentSummaryPersistedFormatContract();
  idx::PageExtentSummaryFormatCompatibility format;
  format.observed = contract.current;
  format.open_class = idx::PageExtentSummaryFormatOpenClass::current;
  format.compatible = true;
  format.migration_required = false;
  format.diagnostic_code = "DPC-045.current_time_range_format";
  return format;
}

std::string EncodeTime(platform::u64 minute) {
  std::ostringstream out;
  out << "2026-05-23T" << std::setw(2) << std::setfill('0')
      << (minute / 60) << ":" << std::setw(2) << std::setfill('0')
      << (minute % 60) << ":00.000000Z";
  return out.str();
}

struct TimeRow {
  std::string row_id;
  platform::u64 page_id = 0;
  std::string encoded_time;
  bool engine_mga_visible = true;
  bool security_visible = true;
};

struct TimeRange {
  platform::u64 first_page_id = 0;
  platform::u64 page_count = 0;
  std::vector<TimeRow> rows;
};

std::vector<TimeRange> TimeCorpus() {
  std::vector<TimeRange> ranges;
  for (platform::u64 range_id = 0; range_id < 8; ++range_id) {
    TimeRange range;
    range.first_page_id = range_id * 4;
    range.page_count = 2;
    for (platform::u64 offset = 0; offset < 4; ++offset) {
      TimeRow row;
      row.row_id = "T" + std::to_string(range_id) + ":" +
                   std::to_string(offset);
      row.page_id = range.first_page_id + (offset / 2);
      row.encoded_time = EncodeTime(range_id * 60 + offset * 11);
      range.rows.push_back(row);
    }
    ranges.push_back(range);
  }
  ranges[3].rows[1].engine_mga_visible = false;
  ranges[4].rows[2].security_visible = false;
  return ranges;
}

idx::TimeRangeSummaryPredicate TimePredicate() {
  idx::TimeRangeSummaryPredicate predicate;
  predicate.time_scalar_type_key = "utc_time_usec_lex";
  predicate.encoded_lower_time = EncodeTime(2 * 60);
  predicate.encoded_upper_time = EncodeTime(4 * 60 + 59);
  predicate.lower_present = true;
  predicate.upper_present = true;
  predicate.lower_inclusive = true;
  predicate.upper_inclusive = true;
  return predicate;
}

idx::TimeRangeSummaryDescriptor TimeDescriptorForRange(
    const std::string& table_uuid,
    const std::string& index_uuid,
    const std::string& family_uuid,
    const TimeRange& range,
    platform::u64 ordinal) {
  const auto minmax = std::minmax_element(
      range.rows.begin(),
      range.rows.end(),
      [](const TimeRow& left, const TimeRow& right) {
        return left.encoded_time < right.encoded_time;
      });
  const auto contract = idx::PageExtentSummaryPersistedFormatContract();

  idx::TimeRangeSummaryDescriptor descriptor;
  descriptor.table_uuid = table_uuid;
  descriptor.index_uuid = index_uuid;
  descriptor.range_family_uuid = family_uuid;
  descriptor.summary_uuid = NewUuidText(4500000 + ordinal);
  descriptor.range.kind = idx::PageExtentSummaryRangeKind::page_range;
  descriptor.range.first_page_id = range.first_page_id;
  descriptor.range.page_count = range.page_count;
  descriptor.time_scalar_type_key = "utc_time_usec_lex";
  descriptor.encoded_min_time = minmax.first->encoded_time;
  descriptor.encoded_max_time = minmax.second->encoded_time;
  descriptor.min_time_present = true;
  descriptor.max_time_present = true;
  descriptor.row_count = range.rows.size();
  descriptor.status = idx::PageExtentSummaryStatus::current;
  descriptor.format_version = contract.current;
  descriptor.generation = 4501000 + ordinal;
  descriptor.persisted_record_present = true;
  descriptor.checksum_valid = true;
  descriptor.authority_source = idx::kTimeRangeSummaryAuthoritySource;
  return descriptor;
}

std::vector<idx::TimeRangeSummaryDescriptor> BuildTimeSummaries(
    const std::vector<TimeRange>& ranges) {
  const auto table_uuid = NewUuidText(4500100);
  const auto index_uuid = NewUuidText(4500101);
  const auto family_uuid = NewUuidText(4500102);
  std::vector<idx::TimeRangeSummaryDescriptor> summaries;
  summaries.reserve(ranges.size());
  for (std::size_t i = 0; i < ranges.size(); ++i) {
    summaries.push_back(TimeDescriptorForRange(table_uuid,
                                               index_uuid,
                                               family_uuid,
                                               ranges[i],
                                               i));
  }
  return summaries;
}

idx::TimeRangeSummaryPrunePlan PlanTime(
    std::vector<idx::TimeRangeSummaryDescriptor> summaries,
    bool enabled = true,
    bool base_row_mga_recheck_required = true,
    bool base_row_security_recheck_required = true) {
  idx::TimeRangeSummaryPruneRequest request;
  request.summaries = std::move(summaries);
  request.format = CurrentSummaryFormat();
  request.predicate = TimePredicate();
  request.time_range_prune_enabled = enabled;
  request.base_row_mga_recheck_required = base_row_mga_recheck_required;
  request.base_row_security_recheck_required = base_row_security_recheck_required;
  return idx::PlanTimeRangeSummaryPrune(request);
}

bool TimeRangeMayMatch(const idx::TimeRangeSummaryDescriptor& descriptor,
                       const idx::TimeRangeSummaryPredicate& predicate) {
  return !(descriptor.encoded_max_time < predicate.encoded_lower_time ||
           descriptor.encoded_min_time > predicate.encoded_upper_time);
}

bool TimeRowMatches(const TimeRow& row,
                    const idx::TimeRangeSummaryPredicate& predicate) {
  return row.engine_mga_visible && row.security_visible &&
         row.encoded_time >= predicate.encoded_lower_time &&
         row.encoded_time <= predicate.encoded_upper_time;
}

struct TimeExecution {
  std::vector<std::string> row_ids;
  platform::u64 ranges_scanned = 0;
  platform::u64 pages_scanned = 0;
  platform::u64 rows_rechecked = 0;
};

TimeExecution ExecuteTime(
    const std::vector<TimeRange>& ranges,
    const std::vector<idx::TimeRangeSummaryDescriptor>& summaries,
    const idx::TimeRangeSummaryPrunePlan& plan) {
  Require(ranges.size() == summaries.size(),
          "DPC-045 time corpus and summaries diverged");
  TimeExecution result;
  const auto predicate = TimePredicate();
  for (std::size_t i = 0; i < ranges.size(); ++i) {
    if (plan.summary_prune_selected &&
        !TimeRangeMayMatch(summaries[i], predicate)) {
      continue;
    }
    ++result.ranges_scanned;
    result.pages_scanned += ranges[i].page_count;
    for (const auto& row : ranges[i].rows) {
      ++result.rows_rechecked;
      if (TimeRowMatches(row, predicate)) {
        result.row_ids.push_back(row.row_id);
      }
    }
  }
  return result;
}

struct TextRow {
  std::string row_id;
  std::string text;
  bool engine_mga_visible = true;
  bool security_visible = true;
};

std::vector<TextRow> TextCorpus() {
  return {
      {"S00", "alpha ledger pruning", true, true},
      {"S01", "beta search segment", true, true},
      {"S02", "alpha vector bridge", true, true},
      {"S03", "gamma exact fallback", true, true},
      {"S04", "alpha hidden rollback", false, true},
      {"S05", "alpha denied policy", true, false},
      {"S06", "merge beta alpha", true, true},
      {"S07", "planner neutral text", true, true},
      {"S08", "alpha segment duplicate", true, true},
      {"S09", "delta text scan", true, true},
      {"S10", "alpha merge survivor", true, true},
      {"S11", "orphan posting alpha", false, false},
  };
}

bool TextMatches(const TextRow& row, std::string_view term) {
  return row.engine_mga_visible && row.security_visible &&
         row.text.find(term) != std::string::npos;
}

const TextRow* FindTextRow(const std::vector<TextRow>& rows,
                           const std::string& row_id) {
  const auto found = std::find_if(rows.begin(), rows.end(), [&](const auto& row) {
    return row.row_id == row_id;
  });
  return found == rows.end() ? nullptr : &*found;
}

idx::InvertedSearchSegmentBuildRequest SearchSegmentBuildRequest(
    platform::u64 seed,
    platform::TypedUuid index_uuid,
    platform::TypedUuid table_uuid) {
  idx::InvertedSearchSegmentBuildRequest request;
  request.index_uuid = index_uuid;
  request.table_uuid = table_uuid;
  request.generation = seed;
  request.engine_mga_inventory_evidence_ref =
      "engine_mga_inventory:search_segment:dpc045:" + std::to_string(seed);
  request.engine_mga_horizon_evidence_ref =
      "engine_mga_horizon:search_segment:dpc045:" + std::to_string(seed);
  return request;
}

idx::InvertedSearchSegmentDescriptor VisibleSearchSegment(
    idx::InvertedSearchSegmentLedger* ledger,
    const idx::InvertedSearchSegmentBuildRequest& request,
    platform::u64 seed) {
  auto started = idx::StartInvertedSearchSegmentBuild(ledger, request);
  Require(started.ok(), "DPC-045 search segment build failed");
  auto segment = started.segment;

  idx::InvertedSearchSegmentValidationRequest validation;
  validation.validation_succeeded = true;
  validation.validation_evidence_ref =
      "validation:search_segment:dpc045:" + std::to_string(seed);
  validation.engine_mga_inventory_evidence_present = true;
  validation.complete = true;
  Require(idx::ValidateInvertedSearchSegmentBuild(ledger,
                                                  &segment,
                                                  validation)
              .ok(),
          "DPC-045 search segment validation failed");

  idx::InvertedSearchSegmentPublishRequest publish_ready;
  publish_ready.publish_barrier_evidence_ref =
      "publish_barrier:engine_mga:search_segment:dpc045:" +
      std::to_string(seed);
  publish_ready.engine_owned_mga_publish_barrier = true;
  Require(idx::MarkInvertedSearchSegmentPublishReady(ledger,
                                                     &segment,
                                                     publish_ready)
              .ok(),
          "DPC-045 search segment publish-ready failed");

  Require(idx::PublishInvertedSearchSegment(ledger, &segment).ok(),
          "DPC-045 search segment publish failed");
  Require(segment.visible, "DPC-045 search segment not visible after publish");
  return segment;
}

idx::InvertedSearchSegmentAccessPlan PlanSearch(
    std::vector<idx::InvertedSearchSegmentDescriptor> segments,
    bool enabled = true,
    bool exact_fallback_available = true,
    bool base_row_mga_recheck_required = true,
    bool base_row_security_recheck_required = true) {
  idx::InvertedSearchSegmentAccessRequest request;
  request.segments = std::move(segments);
  request.search_segments_enabled = enabled;
  request.exact_base_table_fallback_available = exact_fallback_available;
  request.base_row_mga_recheck_required = base_row_mga_recheck_required;
  request.base_row_security_recheck_required = base_row_security_recheck_required;
  return idx::PlanInvertedSearchSegmentAccess(request);
}

struct SearchExecution {
  std::vector<std::string> row_ids;
  platform::u64 segments_scanned = 0;
  platform::u64 postings_scanned = 0;
  platform::u64 candidate_rows_rechecked = 0;
  platform::u64 base_rows_scanned = 0;
};

SearchExecution ExecuteSearch(
    const std::vector<TextRow>& rows,
    std::string_view term,
    const idx::InvertedSearchSegmentAccessPlan& plan,
    const std::map<std::string, std::vector<std::string>>& postings_by_segment) {
  SearchExecution result;
  if (!plan.segment_search_selected) {
    result.base_rows_scanned = rows.size();
    for (const auto& row : rows) {
      if (TextMatches(row, term)) {
        result.row_ids.push_back(row.row_id);
      }
    }
    return result;
  }

  std::set<std::string> candidates;
  for (const auto& segment_uuid : plan.visible_segment_uuids) {
    const auto found = postings_by_segment.find(UuidKey(segment_uuid));
    Require(found != postings_by_segment.end(),
            "DPC-045 missing postings for visible search segment");
    ++result.segments_scanned;
    result.postings_scanned += found->second.size();
    candidates.insert(found->second.begin(), found->second.end());
  }

  result.candidate_rows_rechecked = candidates.size();
  for (const auto& row : rows) {
    if (candidates.contains(row.row_id) && TextMatches(row, term)) {
      result.row_ids.push_back(row.row_id);
    }
  }
  return result;
}

std::vector<idx::InvertedSearchSegmentDescriptor> VisibleOnly(
    const std::vector<idx::InvertedSearchSegmentDescriptor>& segments) {
  std::vector<idx::InvertedSearchSegmentDescriptor> visible;
  for (const auto& segment : segments) {
    if (segment.visible) {
      visible.push_back(segment);
    }
  }
  return visible;
}

struct VectorRow {
  std::string row_id;
  double x = 0.0;
  double y = 0.0;
  bool engine_mga_visible = true;
  bool security_visible = true;
};

std::vector<VectorRow> VectorCorpus() {
  return {
      {"V00", 0.05, 0.10, true, true},
      {"V01", 0.15, 0.10, true, true},
      {"V02", 0.25, 0.05, false, true},
      {"V03", 0.30, 0.20, true, true},
      {"V04", 0.35, 0.20, true, false},
      {"V05", 0.42, 0.30, true, true},
      {"V06", 0.65, 0.55, true, true},
      {"V07", 0.80, 0.90, true, true},
      {"V08", 1.10, 0.70, true, true},
      {"V09", 1.30, 1.10, true, true},
  };
}

double DistanceSquared(const VectorRow& row, double qx, double qy) {
  const double dx = row.x - qx;
  const double dy = row.y - qy;
  return dx * dx + dy * dy;
}

bool VectorVisible(const VectorRow& row) {
  return row.engine_mga_visible && row.security_visible;
}

const VectorRow* FindVectorRow(const std::vector<VectorRow>& rows,
                               const std::string& row_id) {
  const auto found = std::find_if(rows.begin(), rows.end(), [&](const auto& row) {
    return row.row_id == row_id;
  });
  return found == rows.end() ? nullptr : &*found;
}

idx::VectorGenerationResourceEnvelope VectorResourceEnvelope(platform::u64 seed) {
  idx::VectorGenerationResourceEnvelope envelope;
  envelope.memory_limit_bytes = 64 * 1024 * 1024;
  envelope.memory_observed_bytes = 10 * 1024 * 1024 + seed;
  envelope.temp_space_limit_bytes = 128 * 1024 * 1024;
  envelope.temp_space_observed_bytes = 18 * 1024 * 1024 + seed;
  envelope.worker_limit = 4;
  envelope.workers_used = 2;
  envelope.resource_governor_evidence_present = true;
  envelope.resource_governor_evidence_ref =
      "resource_governor:vector_generation:dpc045:" + std::to_string(seed);
  return envelope;
}

idx::VectorGenerationRecallContract VectorRecallContract(platform::u64 seed) {
  idx::VectorGenerationRecallContract contract;
  contract.top_k = 4;
  contract.exact_sample_rows = 10;
  contract.required_recall = 1.0;
  contract.observed_recall = 1.0;
  contract.deterministic_sample = true;
  contract.evidence_present = true;
  contract.evidence_ref =
      "recall_contract:vector_generation:dpc045:" + std::to_string(seed);
  return contract;
}

idx::VectorGenerationRequest VectorBuildRequest(
    platform::u64 seed,
    platform::TypedUuid index_uuid,
    platform::TypedUuid table_uuid) {
  idx::VectorGenerationRequest request;
  request.index_uuid = index_uuid;
  request.table_uuid = table_uuid;
  request.generation = seed;
  request.algorithm = idx::IndexVectorAlgorithm::hnsw;
  request.engine_mga_inventory_evidence_ref =
      "engine_mga_inventory:vector_generation:dpc045:" + std::to_string(seed);
  request.engine_mga_horizon_evidence_ref =
      "engine_mga_horizon:vector_generation:dpc045:" + std::to_string(seed);
  request.resource_envelope = VectorResourceEnvelope(seed);
  return request;
}

idx::VectorGenerationDescriptor PublishedVectorGeneration(
    idx::VectorGenerationLedger* ledger,
    const idx::VectorGenerationRequest& request,
    platform::u64 seed) {
  auto requested = idx::RequestVectorGeneration(ledger, request);
  Require(requested.ok(), "DPC-045 vector generation request failed");
  auto generation = requested.generation;
  Require(idx::StartVectorGenerationBuild(ledger, &generation).ok(),
          "DPC-045 vector generation build failed");

  idx::VectorGenerationTrainingRequest training;
  training.training_succeeded = true;
  training.complete_training_set = true;
  training.training_evidence_ref =
      "training:vector_generation:dpc045:" + std::to_string(seed);
  Require(idx::MarkVectorGenerationTrained(ledger, &generation, training).ok(),
          "DPC-045 vector generation training failed");

  idx::VectorGenerationValidationRequest validation;
  validation.validation_succeeded = true;
  validation.complete_generation = true;
  validation.validation_evidence_ref =
      "validation:vector_generation:dpc045:" + std::to_string(seed);
  Require(idx::ValidateVectorGeneration(ledger, &generation, validation).ok(),
          "DPC-045 vector generation validation failed");

  idx::VectorGenerationSealRequest seal;
  seal.sealed_bytes_complete = true;
  seal.sealed_generation_evidence_ref =
      "sealed_generation:vector_generation:dpc045:" + std::to_string(seed);
  seal.recall_contract = VectorRecallContract(seed);
  Require(idx::SealVectorGeneration(ledger, &generation, seal).ok(),
          "DPC-045 vector generation seal failed");

  idx::VectorGenerationPublishRequest publish;
  publish.publish_barrier_evidence_ref =
      "publish_barrier:engine_mga:vector_generation:dpc045:" +
      std::to_string(seed);
  publish.engine_owned_mga_publish_barrier = true;
  Require(idx::PublishVectorGeneration(ledger, &generation, publish).ok(),
          "DPC-045 vector generation publish failed");
  Require(generation.visible,
          "DPC-045 vector generation not visible after publish");
  return generation;
}

idx::VectorGenerationAccessPlan PlanVector(
    std::vector<idx::VectorGenerationDescriptor> generations,
    bool enabled = true,
    bool exact_fallback_available = true,
    bool base_row_mga_recheck_required = true,
    bool base_row_security_recheck_required = true) {
  idx::VectorGenerationAccessRequest request;
  request.generations = std::move(generations);
  request.vector_generation_enabled = enabled;
  request.exact_vector_scan_fallback_available = exact_fallback_available;
  request.base_row_mga_recheck_required = base_row_mga_recheck_required;
  request.base_row_security_recheck_required = base_row_security_recheck_required;
  return idx::PlanVectorGenerationAccess(request);
}

struct VectorExecution {
  std::vector<std::string> row_ids;
  platform::u64 exact_comparisons = 0;
  platform::u64 ann_candidate_comparisons = 0;
  platform::u64 rerank_rows_rechecked = 0;
};

VectorExecution ExecuteVectorExact(const std::vector<VectorRow>& rows,
                                   double qx,
                                   double qy,
                                   std::size_t top_k) {
  VectorExecution result;
  std::vector<std::pair<double, std::string>> ranked;
  for (const auto& row : rows) {
    ++result.exact_comparisons;
    if (VectorVisible(row)) {
      ranked.push_back({DistanceSquared(row, qx, qy), row.row_id});
    }
  }
  std::sort(ranked.begin(), ranked.end());
  for (std::size_t i = 0; i < std::min(top_k, ranked.size()); ++i) {
    result.row_ids.push_back(ranked[i].second);
  }
  result.rerank_rows_rechecked = rows.size();
  return result;
}

VectorExecution ExecuteVector(
    const std::vector<VectorRow>& rows,
    double qx,
    double qy,
    std::size_t top_k,
    const idx::VectorGenerationAccessPlan& plan,
    const std::map<std::string, std::vector<std::string>>& candidates_by_generation) {
  if (!plan.ann_generation_selected) {
    return ExecuteVectorExact(rows, qx, qy, top_k);
  }

  Require(plan.published_generation_uuids.size() == 1,
          "DPC-045 expected one published vector generation");
  const auto found =
      candidates_by_generation.find(UuidKey(plan.published_generation_uuids.front()));
  Require(found != candidates_by_generation.end(),
          "DPC-045 missing ANN candidates for published generation");

  VectorExecution result;
  std::vector<std::pair<double, std::string>> ranked;
  for (const auto& row_id : found->second) {
    const auto* row = FindVectorRow(rows, row_id);
    Require(row != nullptr, "DPC-045 ANN candidate row missing");
    ++result.ann_candidate_comparisons;
    ++result.rerank_rows_rechecked;
    if (VectorVisible(*row)) {
      ranked.push_back({DistanceSquared(*row, qx, qy), row->row_id});
    }
  }
  std::sort(ranked.begin(), ranked.end());
  for (std::size_t i = 0; i < std::min(top_k, ranked.size()); ++i) {
    result.row_ids.push_back(ranked[i].second);
  }
  return result;
}

void RequireFallbackSelected(std::string_view actual_reason,
                             std::string_view expected_reason,
                             std::string_view diagnostic_code,
                             std::string_view expected_diagnostic,
                             std::string_view message) {
  if (actual_reason != expected_reason || diagnostic_code != expected_diagnostic) {
    std::cerr << "fallback=" << actual_reason
              << " diagnostic=" << diagnostic_code << '\n';
  }
  Require(actual_reason == expected_reason, message);
  Require(diagnostic_code == expected_diagnostic, message);
}

void PrintProof(std::string_view workload_id,
                std::string_view lane,
                std::string_view disabled_flag,
                platform::u64 baseline_counter,
                platform::u64 enabled_counter,
                std::string_view baseline_hash,
                std::string_view enabled_hash,
                platform::u64 baseline_rows,
                platform::u64 enabled_rows,
                std::string_view extra) {
  std::cout << kBenchmarkOutputSearchKey
            << ",workload_id=" << workload_id
            << ",lane=" << lane
            << ",run_count=" << kRunCount
            << ",disabled_flag=" << disabled_flag
            << ",baseline_counter=" << baseline_counter
            << ",enabled_counter=" << enabled_counter
            << ",improvement_ratio="
            << Fixed(ReductionRatio(baseline_counter, enabled_counter))
            << ",baseline_row_count=" << baseline_rows
            << ",enabled_row_count=" << enabled_rows
            << ",baseline_result_hash=" << baseline_hash
            << ",enabled_result_hash=" << enabled_hash
            << ",result_hash_equal="
            << (baseline_hash == enabled_hash ? "true" : "false")
            << ",base_row_mga_recheck=true"
            << ",base_row_security_recheck=true"
            << ",metadata_visibility_authority=false"
            << ",metadata_finality_authority=false"
            << ",parser_finality_authority=false"
            << ",client_finality_authority=false"
            << ",timestamp_finality_authority=false"
            << ",uuid_ordering_finality_authority=false"
            << ",event_stream_finality_authority=false"
            << ",donor_finality_authority=false"
            << ",write_ahead_finality_authority=false"
            << ",source_state=ctest_runtime_deterministic"
            << ",owner_ctest=dpc_specialized_index_benchmark_gate"
            << extra << '\n';
}

std::string ProveTimeRangeLane(bool emit_proof) {
  const auto ranges = TimeCorpus();
  const auto summaries = BuildTimeSummaries(ranges);
  const idx::TimeRangeSummaryPrunePlan exact_plan;
  const auto baseline = ExecuteTime(ranges, summaries, exact_plan);
  const auto baseline_hash = HashRows(baseline.row_ids);

  const auto enabled_plan = PlanTime(summaries);
  Require(enabled_plan.ok(), "DPC-045 time range prune not selected");
  RequireAuthorityProof(enabled_plan.base_row_mga_recheck_required,
                        enabled_plan.base_row_security_recheck_required,
                        enabled_plan.summary_metadata_visibility_authority,
                        enabled_plan.summary_metadata_finality_authority,
                        "DPC-045 time plan lost base-row authority proof");
  const auto enabled = ExecuteTime(ranges, summaries, enabled_plan);
  const auto enabled_hash = HashRows(enabled.row_ids);
  const auto equality = idx::BuildTimeRangeSummaryResultEqualityEvidence(
      baseline.row_ids,
      enabled.row_ids);
  Require(equality.exact_match,
          "DPC-045 time enabled result diverged from exact baseline");
  Require(enabled.pages_scanned < baseline.pages_scanned,
          "DPC-045 time range lane did not reduce scanned pages");

  const auto disabled_plan = PlanTime(summaries, false);
  RequireFallbackSelected(disabled_plan.fallback_reason,
                          "disabled_summary_exact_fallback",
                          disabled_plan.diagnostic.diagnostic_code,
                          "INDEX.TIME_RANGE_SUMMARY.DISABLED_EXACT_FALLBACK",
                          "DPC-045 disabled time flag did not exact fallback");
  const auto disabled = ExecuteTime(ranges, summaries, disabled_plan);
  Require(HashRows(disabled.row_ids) == baseline_hash,
          "DPC-045 disabled time fallback diverged");

  auto stale = summaries;
  stale[3].status = idx::PageExtentSummaryStatus::stale;
  const auto stale_plan = PlanTime(stale);
  RequireFallbackSelected(stale_plan.fallback_reason,
                          "stale_summary_exact_fallback",
                          stale_plan.diagnostic.diagnostic_code,
                          "INDEX.TIME_RANGE_SUMMARY.STALE_EXACT_FALLBACK",
                          "DPC-045 stale time summary did not fallback");
  Require(HashRows(ExecuteTime(ranges, summaries, stale_plan).row_ids) ==
              baseline_hash,
          "DPC-045 stale time fallback diverged");

  auto corrupt = summaries;
  corrupt[4].checksum_valid = false;
  corrupt[4].status = idx::PageExtentSummaryStatus::corrupt;
  const auto corrupt_plan = PlanTime(corrupt);
  RequireFallbackSelected(corrupt_plan.fallback_reason,
                          "corrupt_summary_exact_fallback",
                          corrupt_plan.diagnostic.diagnostic_code,
                          "INDEX.TIME_RANGE_SUMMARY.CORRUPT_EXACT_FALLBACK",
                          "DPC-045 corrupt time summary did not fallback");
  Require(HashRows(ExecuteTime(ranges, summaries, corrupt_plan).row_ids) ==
              baseline_hash,
          "DPC-045 corrupt time fallback diverged");

  const auto unsafe_plan = PlanTime(summaries, true, false, true);
  RequireFallbackSelected(unsafe_plan.fallback_reason,
                          "non_authoritative_summary_exact_fallback",
                          unsafe_plan.diagnostic.diagnostic_code,
                          "INDEX.TIME_RANGE_SUMMARY.BASE_ROW_RECHECK_REQUIRED",
                          "DPC-045 unsafe time authority did not fallback");

  if (emit_proof) {
    PrintProof("WL45_TIME_RANGE",
               "time_range_summary_prune",
               kTimeRangeDisableFlag,
               baseline.pages_scanned,
               enabled.pages_scanned,
               baseline_hash,
               enabled_hash,
               baseline.row_ids.size(),
               enabled.row_ids.size(),
               ",baseline_ranges_scanned=" + std::to_string(baseline.ranges_scanned) +
                   ",enabled_ranges_scanned=" +
                   std::to_string(enabled.ranges_scanned));
  }
  return "time:" + std::to_string(baseline.pages_scanned) + ":" +
         std::to_string(enabled.pages_scanned) + ":" +
         std::to_string(baseline.row_ids.size()) + ":" +
         std::to_string(enabled.row_ids.size()) + ":" + baseline_hash + ":" +
         enabled_hash;
}

std::string ProveSearchSegmentLane(bool emit_proof) {
  const auto rows = TextCorpus();
  const auto index_uuid = NewUuid(platform::UuidKind::object, 4510001);
  const auto table_uuid = NewUuid(platform::UuidKind::object, 4510002);
  idx::InvertedSearchSegmentLedger ledger;
  const auto left = VisibleSearchSegment(
      &ledger,
      SearchSegmentBuildRequest(4510100, index_uuid, table_uuid),
      4510100);
  const auto right = VisibleSearchSegment(
      &ledger,
      SearchSegmentBuildRequest(4510200, index_uuid, table_uuid),
      4510200);

  const std::vector<idx::InvertedSearchSegmentDescriptor> segments = {left,
                                                                      right};
  std::map<std::string, std::vector<std::string>> postings;
  postings[UuidKey(left.segment_uuid)] = {"S00", "S02", "S04", "S08"};
  postings[UuidKey(right.segment_uuid)] = {"S05", "S06", "S08", "S10", "S11"};

  const auto exact_plan = PlanSearch(segments, false);
  const auto baseline = ExecuteSearch(rows, "alpha", exact_plan, postings);
  const auto baseline_hash = HashRows(baseline.row_ids);

  const auto enabled_plan = PlanSearch(segments);
  Require(enabled_plan.ok(), "DPC-045 search segment plan not selected");
  RequireAuthorityProof(enabled_plan.base_row_mga_recheck_required,
                        enabled_plan.base_row_security_recheck_required,
                        enabled_plan.segment_metadata_visibility_authority,
                        enabled_plan.segment_metadata_finality_authority,
                        "DPC-045 search plan lost base-row authority proof");
  const auto enabled = ExecuteSearch(rows, "alpha", enabled_plan, postings);
  const auto enabled_hash = HashRows(enabled.row_ids);
  Require(enabled_hash == baseline_hash,
          "DPC-045 search enabled hash diverged");
  Require(enabled.candidate_rows_rechecked < baseline.base_rows_scanned,
          "DPC-045 search segment lane did not reduce candidate rows");

  RequireFallbackSelected(exact_plan.fallback_reason,
                          "disabled_segment_exact_fallback",
                          exact_plan.diagnostic.diagnostic_code,
                          "INDEX.SEARCH_SEGMENT.DISABLED_EXACT_FALLBACK",
                          "DPC-045 disabled search flag did not fallback");

  auto stale = segments;
  stale.front().stale = true;
  const auto stale_plan = PlanSearch(stale);
  RequireFallbackSelected(stale_plan.fallback_reason,
                          "stale_segment_exact_fallback",
                          stale_plan.diagnostic.diagnostic_code,
                          "INDEX.SEARCH_SEGMENT.STALE_EXACT_FALLBACK",
                          "DPC-045 stale search segment did not fallback");
  Require(HashRows(ExecuteSearch(rows, "alpha", stale_plan, postings).row_ids) ==
              baseline_hash,
          "DPC-045 stale search fallback diverged");

  auto corrupt = segments;
  corrupt.back().checksum_valid = false;
  const auto corrupt_plan = PlanSearch(corrupt);
  RequireFallbackSelected(corrupt_plan.fallback_reason,
                          "corrupt_segment_exact_fallback",
                          corrupt_plan.diagnostic.diagnostic_code,
                          "INDEX.SEARCH_SEGMENT.CORRUPT_EXACT_FALLBACK",
                          "DPC-045 corrupt search segment did not fallback");
  Require(HashRows(ExecuteSearch(rows, "alpha", corrupt_plan, postings).row_ids) ==
              baseline_hash,
          "DPC-045 corrupt search fallback diverged");

  const auto unsafe_plan = PlanSearch(segments, true, true, false, true);
  RequireFallbackSelected(unsafe_plan.fallback_reason,
                          "non_authoritative_segment_exact_fallback",
                          unsafe_plan.diagnostic.diagnostic_code,
                          "INDEX.SEARCH_SEGMENT.BASE_ROW_RECHECK_REQUIRED",
                          "DPC-045 unsafe search authority did not fallback");

  idx::InvertedSearchSegmentMergeRequest merge;
  merge.merge_id = NewUuid(platform::UuidKind::object, 4510300);
  merge.merged_segment_uuid = NewUuid(platform::UuidKind::object, 4510301);
  merge.index_uuid = index_uuid;
  merge.table_uuid = table_uuid;
  merge.input_segment_uuids = {left.segment_uuid, right.segment_uuid};
  merge.merged_generation = 4510300;
  merge.validation_succeeded = true;
  merge.validation_evidence_ref = "validation:search_segment_merge:dpc045";
  merge.publish_barrier_evidence_ref =
      "publish_barrier:engine_mga:search_segment_merge:dpc045";
  merge.engine_mga_inventory_evidence_ref =
      "engine_mga_inventory:search_segment_merge:dpc045";
  merge.engine_mga_horizon_evidence_ref =
      "engine_mga_horizon:search_segment_merge:dpc045";
  merge.engine_mga_inventory_evidence_present = true;
  merge.engine_owned_mga_publish_barrier = true;
  merge.merge_commit_marker_complete = true;
  const auto merged = idx::ApplyInvertedSearchSegmentMerge(&ledger, merge);
  Require(merged.ok(), "DPC-045 search segment merge failed");
  Require(merged.retired_input_segment_count == 2,
          "DPC-045 search merge did not retire both input segments");

  postings[UuidKey(merged.merged_segment.segment_uuid)] =
      {"S00", "S02", "S04", "S05", "S06", "S08", "S10", "S11"};
  const auto merged_plan = PlanSearch({merged.merged_segment});
  const auto merged_execution =
      ExecuteSearch(rows, "alpha", merged_plan, postings);
  Require(HashRows(merged_execution.row_ids) == baseline_hash,
          "DPC-045 merged search segment hash diverged");
  Require(merged_execution.segments_scanned < enabled.segments_scanned,
          "DPC-045 search merge did not reduce segment count");

  auto no_fallback = corrupt;
  const auto refused = PlanSearch(no_fallback, true, false);
  Require(!refused.status.ok() && refused.selected_access == "refused",
          "DPC-045 unsafe search segment without exact fallback was not refused");

  const auto merged_hash = HashRows(merged_execution.row_ids);
  if (emit_proof) {
    PrintProof("WL45_SEARCH_SEGMENT",
               "inverted_search_segment_scan",
               kInvertedSegmentsDisableFlag,
               baseline.base_rows_scanned,
               enabled.candidate_rows_rechecked,
               baseline_hash,
               enabled_hash,
               baseline.row_ids.size(),
               enabled.row_ids.size(),
               ",baseline_segments_scanned=0,enabled_segments_scanned=" +
                   std::to_string(enabled.segments_scanned) +
                   ",postings_scanned=" +
                   std::to_string(enabled.postings_scanned) +
                   ",merge_disabled_flag=" +
                   std::string(kSearchMergeDisableFlag) +
                   ",merged_segments_scanned=" +
                   std::to_string(merged_execution.segments_scanned) +
                   ",merged_result_hash=" +
                   merged_hash);
  }
  return "search:" + std::to_string(baseline.base_rows_scanned) + ":" +
         std::to_string(enabled.candidate_rows_rechecked) + ":" +
         std::to_string(enabled.segments_scanned) + ":" +
         std::to_string(enabled.postings_scanned) + ":" +
         std::to_string(merged_execution.segments_scanned) + ":" +
         std::to_string(baseline.row_ids.size()) + ":" +
         std::to_string(enabled.row_ids.size()) + ":" + baseline_hash + ":" +
         enabled_hash + ":" + merged_hash;
}

std::string ProveVectorLane(bool emit_proof) {
  const auto rows = VectorCorpus();
  const auto index_uuid = NewUuid(platform::UuidKind::object, 4520001);
  const auto table_uuid = NewUuid(platform::UuidKind::object, 4520002);
  idx::VectorGenerationLedger ledger;
  const auto generation = PublishedVectorGeneration(
      &ledger,
      VectorBuildRequest(4520100, index_uuid, table_uuid),
      4520100);
  const std::vector<idx::VectorGenerationDescriptor> generations = {generation};

  std::map<std::string, std::vector<std::string>> candidates;
  candidates[UuidKey(generation.generation_uuid)] =
      {"V00", "V01", "V02", "V03", "V04", "V05"};

  const auto exact_plan = PlanVector(generations, false);
  const auto baseline = ExecuteVector(rows, 0.0, 0.0, 4, exact_plan, candidates);
  const auto baseline_hash = HashRows(baseline.row_ids);

  const auto enabled_plan = PlanVector(generations);
  Require(enabled_plan.ok(), "DPC-045 vector generation plan not selected");
  RequireAuthorityProof(enabled_plan.base_row_mga_recheck_required,
                        enabled_plan.base_row_security_recheck_required,
                        enabled_plan.generation_metadata_visibility_authority,
                        enabled_plan.generation_metadata_finality_authority,
                        "DPC-045 vector plan lost base-row authority proof");
  const auto enabled = ExecuteVector(rows, 0.0, 0.0, 4, enabled_plan, candidates);
  const auto enabled_hash = HashRows(enabled.row_ids);
  Require(enabled_hash == baseline_hash,
          "DPC-045 vector enabled hash diverged");
  Require(enabled.ann_candidate_comparisons < baseline.exact_comparisons,
          "DPC-045 vector lane did not reduce candidate comparisons");
  Require(generation.recall_contract.required_recall == 1.0 &&
              generation.recall_contract.observed_recall == 1.0,
          "DPC-045 vector recall contract is not exact for deterministic sample");

  RequireFallbackSelected(exact_plan.fallback_reason,
                          "disabled_generation_exact_fallback",
                          exact_plan.diagnostic.diagnostic_code,
                          "INDEX.VECTOR_GENERATION.DISABLED_EXACT_FALLBACK",
                          "DPC-045 disabled vector flag did not fallback");

  auto stale = generations;
  stale.front().stale = true;
  const auto stale_plan = PlanVector(stale);
  RequireFallbackSelected(stale_plan.fallback_reason,
                          "stale_generation_exact_fallback",
                          stale_plan.diagnostic.diagnostic_code,
                          "INDEX.VECTOR_GENERATION.STALE_EXACT_FALLBACK",
                          "DPC-045 stale vector generation did not fallback");
  Require(HashRows(ExecuteVector(rows, 0.0, 0.0, 4, stale_plan, candidates)
                       .row_ids) == baseline_hash,
          "DPC-045 stale vector fallback diverged");

  auto corrupt = generations;
  corrupt.front().checksum_valid = false;
  const auto corrupt_plan = PlanVector(corrupt);
  RequireFallbackSelected(corrupt_plan.fallback_reason,
                          "corrupt_generation_exact_fallback",
                          corrupt_plan.diagnostic.diagnostic_code,
                          "INDEX.VECTOR_GENERATION.CORRUPT_EXACT_FALLBACK",
                          "DPC-045 corrupt vector generation did not fallback");
  Require(HashRows(ExecuteVector(rows, 0.0, 0.0, 4, corrupt_plan, candidates)
                       .row_ids) == baseline_hash,
          "DPC-045 corrupt vector fallback diverged");

  auto recall_failure = generations;
  recall_failure.front().recall_contract.observed_recall = 0.50;
  const auto recall_plan = PlanVector(recall_failure);
  RequireFallbackSelected(recall_plan.fallback_reason,
                          "recall_contract_exact_fallback",
                          recall_plan.diagnostic.diagnostic_code,
                          "INDEX.VECTOR_GENERATION.RECALL_EXACT_FALLBACK",
                          "DPC-045 vector recall failure did not fallback");
  Require(HashRows(ExecuteVector(rows, 0.0, 0.0, 4, recall_plan, candidates)
                       .row_ids) == baseline_hash,
          "DPC-045 recall vector fallback diverged");

  const auto unsafe_plan = PlanVector(generations, true, true, false, true);
  RequireFallbackSelected(unsafe_plan.fallback_reason,
                          "non_authoritative_generation_exact_fallback",
                          unsafe_plan.diagnostic.diagnostic_code,
                          "INDEX.VECTOR_GENERATION.BASE_ROW_RECHECK_REQUIRED",
                          "DPC-045 unsafe vector authority did not fallback");

  auto no_fallback = corrupt;
  const auto refused = PlanVector(no_fallback, true, false);
  Require(!refused.status.ok() && refused.selected_access == "refused",
          "DPC-045 unsafe vector generation without exact fallback was not refused");

  if (emit_proof) {
    PrintProof("WL45_VECTOR_ANN",
               "sealed_ann_vector_scan",
               kVectorGenerationDisableFlag,
               baseline.exact_comparisons,
               enabled.ann_candidate_comparisons,
               baseline_hash,
               enabled_hash,
               baseline.row_ids.size(),
               enabled.row_ids.size(),
               ",exact_sample_rows=" +
                   std::to_string(generation.recall_contract.exact_sample_rows) +
                   ",required_recall=" +
                   Fixed(generation.recall_contract.required_recall) +
                   ",observed_recall=" +
                   Fixed(generation.recall_contract.observed_recall) +
                   ",rerank_rows_rechecked=" +
                   std::to_string(enabled.rerank_rows_rechecked));
  }
  return "vector:" + std::to_string(baseline.exact_comparisons) + ":" +
         std::to_string(enabled.ann_candidate_comparisons) + ":" +
         std::to_string(enabled.rerank_rows_rechecked) + ":" +
         std::to_string(baseline.row_ids.size()) + ":" +
         std::to_string(enabled.row_ids.size()) + ":" +
         Fixed(generation.recall_contract.required_recall) + ":" +
         Fixed(generation.recall_contract.observed_recall) + ":" +
         baseline_hash + ":" + enabled_hash;
}

}  // namespace

int main() {
  Require(!kGateSearchKey.empty(), "DPC-045 gate search key missing");
  Require(!kBenchmarkOutputSearchKey.empty(),
          "DPC-045 benchmark output search key missing");
  Require(!kTimeRangeDisableFlag.empty() &&
              !kInvertedSegmentsDisableFlag.empty() &&
              !kSearchMergeDisableFlag.empty() &&
              !kVectorGenerationDisableFlag.empty(),
          "DPC-045 rollback-disable flag names missing");

  const auto time_signature = ProveTimeRangeLane(true);
  const auto search_signature = ProveSearchSegmentLane(true);
  const auto vector_signature = ProveVectorLane(true);
  for (platform::u64 run = 1; run < kRunCount; ++run) {
    Require(ProveTimeRangeLane(false) == time_signature,
            "DPC-045 time range proof was not deterministic across five runs");
    Require(ProveSearchSegmentLane(false) == search_signature,
            "DPC-045 search segment proof was not deterministic across five runs");
    Require(ProveVectorLane(false) == vector_signature,
            "DPC-045 vector proof was not deterministic across five runs");
  }

  std::cout << kGateSearchKey
            << "=passed run_count=" << kRunCount
            << " deterministic_proxy_counters=true"
            << " exact_fallback_equality=true"
            << " base_row_mga_security_recheck=true\n";
  return EXIT_SUCCESS;
}
