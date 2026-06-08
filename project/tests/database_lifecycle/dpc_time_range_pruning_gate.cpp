// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "observability/performance_optimization_surface.hpp"
#include "optimizer_explain.hpp"
#include "time_range_summary_pruning.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace api = scratchbird::engine::internal_api;
namespace idx = scratchbird::core::index;
namespace opt = scratchbird::engine::optimizer;
namespace planner = scratchbird::engine::planner;
namespace uuid = scratchbird::core::uuid;
using scratchbird::core::platform::TypedUuid;
using scratchbird::core::platform::UuidKind;

constexpr std::string_view kGateSearchKey = "DPC_TIME_RANGE_PRUNING_GATE";
constexpr std::string_view kImplementationSearchKey =
    "DPC_TIME_RANGE_SUMMARY_PRUNING";

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

bool Contains(std::string_view haystack, std::string_view needle) {
  return haystack.find(needle) != std::string_view::npos;
}

std::uint64_t NowMillis() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

struct UuidFactory {
  std::uint64_t base_millis = NowMillis();

  TypedUuid Typed(UuidKind kind, std::uint64_t salt) const {
    const auto generated =
        uuid::GenerateEngineIdentityV7(kind, base_millis + salt);
    Require(generated.ok(), "DPC-042 UUID generation failed");
    return generated.value;
  }

  std::string Text(std::uint64_t salt) const {
    return uuid::UuidToString(Typed(UuidKind::object, salt).value);
  }
};

std::string EncodeTime(std::uint64_t hour, std::uint64_t minute) {
  std::ostringstream out;
  out << "2026-05-23T" << std::setw(2) << std::setfill('0') << hour
      << ":" << std::setw(2) << std::setfill('0') << minute << ":00.000000Z";
  return out.str();
}

idx::PageExtentSummaryFormatCompatibility CurrentFormat() {
  const auto contract = idx::PageExtentSummaryPersistedFormatContract();
  idx::PageExtentSummaryFormatCompatibility format;
  format.observed = contract.current;
  format.open_class = idx::PageExtentSummaryFormatOpenClass::current;
  format.compatible = true;
  format.migration_required = false;
  format.diagnostic_code = "DPC-042.current_format";
  return format;
}

struct SyntheticRow {
  std::string row_id;
  std::uint64_t page_id = 0;
  std::string encoded_time;
  bool engine_mga_visible = true;
  bool security_visible = true;
};

struct SyntheticRange {
  std::uint64_t first_page_id = 0;
  std::uint32_t page_count = 0;
  std::vector<SyntheticRow> rows;
};

std::vector<SyntheticRange> Corpus() {
  std::vector<SyntheticRange> ranges;
  for (std::uint64_t range_id = 0; range_id < 5; ++range_id) {
    SyntheticRange range;
    range.first_page_id = range_id * 10;
    range.page_count = 2;
    for (std::uint64_t offset = 0; offset < 4; ++offset) {
      SyntheticRow row;
      row.row_id = "R" + std::to_string(range_id) + ":" +
                   std::to_string(offset);
      row.page_id = range.first_page_id + (offset / 2);
      row.encoded_time = EncodeTime(range_id, offset * 12);
      range.rows.push_back(row);
    }
    ranges.push_back(range);
  }

  ranges[2].rows[1].engine_mga_visible = false;
  ranges[3].rows[2].security_visible = false;
  return ranges;
}

idx::TimeRangeSummaryDescriptor DescriptorForRange(
    const UuidFactory& uuids,
    const std::string& table_uuid,
    const std::string& index_uuid,
    const std::string& range_family_uuid,
    const SyntheticRange& range,
    std::uint64_t salt) {
  const auto minmax = std::minmax_element(
      range.rows.begin(),
      range.rows.end(),
      [](const SyntheticRow& left, const SyntheticRow& right) {
        return left.encoded_time < right.encoded_time;
      });
  const auto contract = idx::PageExtentSummaryPersistedFormatContract();
  idx::TimeRangeSummaryDescriptor descriptor;
  descriptor.table_uuid = table_uuid;
  descriptor.index_uuid = index_uuid;
  descriptor.range_family_uuid = range_family_uuid;
  descriptor.summary_uuid = uuids.Text(4000 + salt);
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
  descriptor.generation = 4200 + salt;
  descriptor.persisted_record_present = true;
  descriptor.checksum_valid = true;
  descriptor.authority_source = idx::kTimeRangeSummaryAuthoritySource;
  return descriptor;
}

std::vector<idx::TimeRangeSummaryDescriptor> BuildSummaries(
    const UuidFactory& uuids,
    const std::vector<SyntheticRange>& ranges) {
  const auto table_uuid = uuids.Text(40);
  const auto index_uuid = uuids.Text(41);
  const auto range_family_uuid = uuids.Text(42);
  std::vector<idx::TimeRangeSummaryDescriptor> summaries;
  summaries.reserve(ranges.size());
  for (std::size_t i = 0; i < ranges.size(); ++i) {
    summaries.push_back(DescriptorForRange(uuids,
                                           table_uuid,
                                           index_uuid,
                                           range_family_uuid,
                                           ranges[i],
                                           i));
  }
  return summaries;
}

idx::TimeRangeSummaryPredicate Predicate() {
  idx::TimeRangeSummaryPredicate predicate;
  predicate.time_scalar_type_key = "utc_time_usec_lex";
  predicate.encoded_lower_time = EncodeTime(2, 0);
  predicate.encoded_upper_time = EncodeTime(3, 59);
  predicate.lower_present = true;
  predicate.upper_present = true;
  predicate.lower_inclusive = true;
  predicate.upper_inclusive = true;
  return predicate;
}

idx::TimeRangeSummaryPrunePlan Plan(
    std::vector<idx::TimeRangeSummaryDescriptor> summaries,
    bool enabled = true) {
  idx::TimeRangeSummaryPruneRequest request;
  request.summaries = std::move(summaries);
  request.format = CurrentFormat();
  request.predicate = Predicate();
  request.time_range_prune_enabled = enabled;
  request.base_row_mga_recheck_required = true;
  request.base_row_security_recheck_required = true;
  return idx::PlanTimeRangeSummaryPrune(request);
}

bool RangeMayMatch(const idx::TimeRangeSummaryDescriptor& descriptor,
                   const idx::TimeRangeSummaryPredicate& predicate) {
  return !(descriptor.encoded_max_time < predicate.encoded_lower_time ||
           descriptor.encoded_min_time > predicate.encoded_upper_time);
}

bool RowMatches(const SyntheticRow& row,
                const idx::TimeRangeSummaryPredicate& predicate) {
  return row.engine_mga_visible && row.security_visible &&
         row.encoded_time >= predicate.encoded_lower_time &&
         row.encoded_time <= predicate.encoded_upper_time;
}

std::vector<std::string> Execute(
    const std::vector<SyntheticRange>& ranges,
    const std::vector<idx::TimeRangeSummaryDescriptor>& summaries,
    const idx::TimeRangeSummaryPredicate& predicate,
    const idx::TimeRangeSummaryPrunePlan& plan) {
  Require(ranges.size() == summaries.size(),
          "DPC-042 corpus and summary shape diverged");
  std::vector<std::string> row_ids;
  for (std::size_t i = 0; i < ranges.size(); ++i) {
    if (plan.summary_prune_selected && !RangeMayMatch(summaries[i], predicate)) {
      continue;
    }
    for (const auto& row : ranges[i].rows) {
      if (RowMatches(row, predicate)) {
        row_ids.push_back(row.row_id);
      }
    }
  }
  return row_ids;
}

void RequireFallback(const idx::TimeRangeSummaryPrunePlan& plan,
                     std::string_view fallback_reason,
                     std::string_view diagnostic_code,
                     std::string_view message) {
  if (!plan.exact_fallback_required ||
      plan.fallback_reason != fallback_reason ||
      plan.diagnostic.diagnostic_code != diagnostic_code) {
    std::cerr << "fallback=" << plan.fallback_reason
              << " diagnostic=" << plan.diagnostic.diagnostic_code
              << " selected_access=" << plan.selected_access << '\n';
  }
  Require(plan.exact_fallback_required, message);
  Require(plan.selected_access == "full_scan", message);
  Require(plan.fallback_reason == fallback_reason, message);
  Require(plan.diagnostic.diagnostic_code == diagnostic_code, message);
  Require(!plan.summary_metadata_visibility_authority &&
              !plan.summary_metadata_finality_authority,
          "DPC-042 fallback made summary metadata authoritative");
}

void ValidateGeneratedUuidSafeDescriptors() {
  const UuidFactory uuids;
  const auto ranges = Corpus();
  auto summaries = BuildSummaries(uuids, ranges);
  const auto& descriptor = summaries.front();
  Require(idx::TimeRangeSummaryDescriptorIdentityValid(descriptor),
          "DPC-042 generated descriptor UUID identity was rejected");
  Require(idx::PageExtentSummaryUuidTextValid(descriptor.table_uuid) &&
              idx::PageExtentSummaryUuidTextValid(descriptor.index_uuid) &&
              idx::PageExtentSummaryUuidTextValid(descriptor.range_family_uuid) &&
              idx::PageExtentSummaryUuidTextValid(descriptor.summary_uuid),
          "DPC-042 descriptor UUID fields are not generated-UUID-safe");

  auto invalid = descriptor;
  invalid.range_family_uuid = "catalog.time_range_family";
  Require(!idx::TimeRangeSummaryDescriptorIdentityValid(invalid),
          "DPC-042 non-UUID range-family identity was accepted");
  const auto invalid_plan = Plan({invalid});
  RequireFallback(invalid_plan,
                  "invalid_identity_exact_fallback",
                  "INDEX.TIME_RANGE_SUMMARY.INVALID_IDENTITY_EXACT_FALLBACK",
                  "DPC-042 invalid identity did not force exact fallback");
}

void ValidatePositivePruneAndEquality() {
  const UuidFactory uuids;
  const auto ranges = Corpus();
  const auto summaries = BuildSummaries(uuids, ranges);
  const auto predicate = Predicate();
  const idx::TimeRangeSummaryPrunePlan full_scan_plan;
  const auto baseline = Execute(ranges, summaries, predicate, full_scan_plan);
  const auto plan = Plan(summaries);
  Require(plan.ok(), "DPC-042 positive time-range prune was not selected");
  Require(plan.selected_access == "time_range_summary_prune",
          "DPC-042 selected access missing time_range_summary_prune");
  Require(plan.prune_reason == "time_bounds_current" &&
              plan.fallback_reason == "none",
          "DPC-042 selected plan reason fields incorrect");
  Require(plan.counters.prune_candidates == 5 &&
              plan.counters.ranges_pruned == 3 &&
              plan.counters.ranges_scanned == 2,
          "DPC-042 range prune counters incorrect");
  Require(plan.counters.pages_considered == 10 &&
              plan.counters.pages_pruned == 6 &&
              plan.counters.pages_scanned == 4,
          "DPC-042 page prune counters incorrect");
  Require(plan.summary_generation == summaries.back().generation,
          "DPC-042 summary generation did not report latest generation");
  Require(plan.authority_source == idx::kTimeRangeSummaryAuthoritySource,
          "DPC-042 authority source missing engine MGA base-page source");
  Require(plan.base_row_mga_recheck_required &&
              plan.base_row_security_recheck_required,
          "DPC-042 selected plan did not preserve base-row rechecks");
  Require(!plan.summary_metadata_visibility_authority &&
              !plan.summary_metadata_finality_authority,
          "DPC-042 selected plan made summary metadata authoritative");

  const auto pruned = Execute(ranges, summaries, predicate, plan);
  const auto equality =
      idx::BuildTimeRangeSummaryResultEqualityEvidence(baseline, pruned);
  Require(equality.exact_match,
          "DPC-042 pruned time-range rows diverged from baseline");
  Require(equality.deterministic_row_ids == baseline &&
              equality.baseline_row_count == 6 &&
              equality.planned_row_count == 6,
          "DPC-042 deterministic row-id equality evidence incorrect");
}

void ValidateExactFallbacks() {
  const UuidFactory uuids;
  const auto ranges = Corpus();
  const auto predicate = Predicate();
  auto summaries = BuildSummaries(uuids, ranges);
  const idx::TimeRangeSummaryPrunePlan full_scan_plan;
  const auto baseline = Execute(ranges, summaries, predicate, full_scan_plan);

  const auto disabled = Plan(summaries, false);
  RequireFallback(disabled,
                  "disabled_summary_exact_fallback",
                  "INDEX.TIME_RANGE_SUMMARY.DISABLED_EXACT_FALLBACK",
                  "DPC-042 disabled summaries did not force exact fallback");

  const auto missing = Plan({});
  RequireFallback(missing,
                  "missing_summary_exact_fallback",
                  "INDEX.TIME_RANGE_SUMMARY.MISSING_EXACT_FALLBACK",
                  "DPC-042 missing summaries did not force exact fallback");

  auto stale = summaries;
  stale[2].status = idx::PageExtentSummaryStatus::stale;
  const auto stale_plan = Plan(stale);
  RequireFallback(stale_plan,
                  "stale_summary_exact_fallback",
                  "INDEX.TIME_RANGE_SUMMARY.STALE_EXACT_FALLBACK",
                  "DPC-042 stale summary did not force exact fallback");
  Require(stale_plan.summary_status == "stale" &&
              stale_plan.summary_generation == stale[2].generation,
          "DPC-042 stale diagnostics did not report exact status/generation");

  auto corrupt = summaries;
  corrupt[1].checksum_valid = false;
  corrupt[1].status = idx::PageExtentSummaryStatus::corrupt;
  RequireFallback(Plan(corrupt),
                  "corrupt_summary_exact_fallback",
                  "INDEX.TIME_RANGE_SUMMARY.CORRUPT_EXACT_FALLBACK",
                  "DPC-042 corrupt summary did not force exact fallback");

  auto incompatible = summaries;
  incompatible[0].time_scalar_type_key = "timestamp_client_order";
  RequireFallback(Plan(incompatible),
                  "incompatible_summary_exact_fallback",
                  "INDEX.TIME_RANGE_SUMMARY.INCOMPATIBLE_EXACT_FALLBACK",
                  "DPC-042 incompatible summary did not force exact fallback");

  for (auto authority_case = 0; authority_case < 7; ++authority_case) {
    auto non_authoritative = summaries;
    if (authority_case == 0) {
      non_authoritative[0].parser_finality_authority_claimed = true;
    } else if (authority_case == 1) {
      non_authoritative[0].client_finality_authority_claimed = true;
    } else if (authority_case == 2) {
      non_authoritative[0].timestamp_finality_authority_claimed = true;
    } else if (authority_case == 3) {
      non_authoritative[0].uuid_ordering_finality_authority_claimed = true;
    } else if (authority_case == 4) {
      non_authoritative[0].event_stream_finality_authority_claimed = true;
    } else if (authority_case == 5) {
      non_authoritative[0].donor_finality_authority_claimed = true;
    } else {
      non_authoritative[0].write_ahead_log_finality_authority_claimed = true;
    }
    const auto plan = Plan(non_authoritative);
    RequireFallback(plan,
                    "non_authoritative_summary_exact_fallback",
                    "INDEX.TIME_RANGE_SUMMARY.NON_AUTHORITATIVE_EXACT_FALLBACK",
                    "DPC-042 non-authoritative summary did not force fallback");
    const auto actual = Execute(ranges, summaries, predicate, plan);
    const auto equality =
        idx::BuildTimeRangeSummaryResultEqualityEvidence(baseline, actual);
    Require(equality.exact_match,
            "DPC-042 non-authoritative exact fallback diverged");
  }

  const std::vector<idx::TimeRangeSummaryPrunePlan> fallback_plans = {
      disabled,
      missing,
      stale_plan,
      Plan(corrupt),
      Plan(incompatible),
  };
  for (const auto& plan : fallback_plans) {
    const auto actual = Execute(ranges, summaries, predicate, plan);
    const auto equality =
        idx::BuildTimeRangeSummaryResultEqualityEvidence(baseline, actual);
    Require(equality.exact_match,
            "DPC-042 exact fallback rows diverged from baseline");
  }

  idx::TimeRangeSummaryPruneRequest unsafe_request;
  unsafe_request.summaries = summaries;
  unsafe_request.format = CurrentFormat();
  unsafe_request.predicate = predicate;
  unsafe_request.base_row_mga_recheck_required = false;
  RequireFallback(idx::PlanTimeRangeSummaryPrune(unsafe_request),
                  "non_authoritative_summary_exact_fallback",
                  "INDEX.TIME_RANGE_SUMMARY.BASE_ROW_RECHECK_REQUIRED",
                  "DPC-042 missing base-row MGA recheck was not refused");
}

opt::BoundOptimizerRequest ExplainRequest() {
  opt::BoundOptimizerRequest request;
  request.context.request_uuid = "request-dpc042-redacted";
  request.context.operation_id = "dml.select_rows";
  request.context.sblr_digest = "sblr:dpc042-bound-time-read";
  request.context.descriptor_set_digest = "descriptor:dpc042";
  request.context.statistics_snapshot_id = "stats:dpc042";
  request.context.executor_capability_set_id = "executor:local";
  request.context.catalog_epoch = 42;
  request.context.security_epoch = 43;
  request.context.security_context_present = true;
  request.context.transaction_context_present = true;
  request.logical_plan.ok = true;
  request.logical_plan.plan_id = "dpc042-time-range-prune-plan";
  request.logical_plan.nodes.push_back(planner::MakeLogicalPlanNode(
      planner::LogicalPlanNodeKind::kDmlRead,
      planner::PhysicalAccessKind::kBitmapSummaryScan,
      "dml.select_rows",
      "time_range_summary_prune"));
  return request;
}

api::PerformanceOptimizationSurfaceSnapshot SurfaceFromPlan(
    const idx::TimeRangeSummaryPrunePlan& plan) {
  auto snapshot = api::DefaultPerformanceOptimizationSurfaceSnapshot();
  snapshot.optimization_profile = "dpc042_time_range_pruning";
  snapshot.summary_prune_enabled = true;
  snapshot.summary_prune_status = plan.selected_access;
  snapshot.summary_prune_last_reason = plan.prune_reason;
  snapshot.summary_prune_fallback_reason = plan.fallback_reason;
  snapshot.summary_prune_summary_status = plan.summary_status;
  snapshot.summary_prune_generation = plan.summary_generation;
  snapshot.summary_prune_ranges_considered = plan.counters.prune_candidates;
  snapshot.summary_prune_ranges_pruned = plan.counters.ranges_pruned;
  snapshot.summary_prune_ranges_scanned = plan.counters.ranges_scanned;
  snapshot.summary_prune_pages_considered = plan.counters.pages_considered;
  snapshot.summary_prune_pages_pruned = plan.counters.pages_pruned;
  snapshot.summary_prune_pages_scanned = plan.counters.pages_scanned;
  snapshot.summary_prune_authority_source = plan.authority_source;
  snapshot.summary_prune_base_row_mga_recheck_required =
      plan.base_row_mga_recheck_required;
  snapshot.summary_prune_base_row_security_recheck_required =
      plan.base_row_security_recheck_required;
  snapshot.support_bundle_correlation_id =
      std::string(kGateSearchKey) + ":support";
  snapshot.request_correlation_id = std::string(kGateSearchKey) + ":request";
  snapshot.benchmark_correlation_id = "none";
  return snapshot;
}

void ValidateExplainAndManagementEvidence() {
  const UuidFactory uuids;
  const auto ranges = Corpus();
  const auto summaries = BuildSummaries(uuids, ranges);
  const auto plan = Plan(summaries);

  opt::PlanCandidate candidate;
  candidate.candidate_id = "CAND-DPC-042-TIME-RANGE";
  candidate.access_kind = planner::PhysicalAccessKind::kBitmapSummaryScan;
  candidate.cost.selectable = true;
  candidate.cost.total_cost = 17;
  candidate.selected = true;
  candidate.summary_prune_evidence = opt::BuildPlanSummaryPruneEvidence(plan);

  opt::BoundOptimizerResult result;
  result.ok = true;
  result.diagnostic_code = "SB_OPT_OK";
  result.plan_id = "dpc042-time-range-prune-plan";
  result.candidates.push_back(candidate);
  result.diagnostics.push_back(std::string(kImplementationSearchKey));

  auto document = opt::BuildOptimizerExplainDocument(ExplainRequest(), result);
  document.redactions.push_back("catalog_summary_identity_redacted");
  const auto json = opt::RenderOptimizerExplainJson(document);
  Require(Contains(json, "\"selected_access\":\"time_range_summary_prune\""),
          "DPC-042 EXPLAIN missing time-range selected access");
  Require(Contains(json, "\"candidate_ranges\":5") &&
              Contains(json, "\"ranges_pruned\":3"),
          "DPC-042 EXPLAIN missing prune candidate counters");
  Require(Contains(json, "\"fallback_reason\":\"none\""),
          "DPC-042 EXPLAIN missing fallback reason");
  Require(Contains(json, "\"summary_generation\":4204"),
          "DPC-042 EXPLAIN missing summary generation");
  Require(Contains(json, "\"authority_source\":\"engine_mga_base_pages\""),
          "DPC-042 EXPLAIN missing authority source");
  for (const auto& summary : summaries) {
    Require(!Contains(json, summary.summary_uuid) &&
                !Contains(json, summary.table_uuid) &&
                !Contains(json, summary.index_uuid),
            "DPC-042 EXPLAIN leaked raw descriptor UUID");
  }

  const auto snapshot = SurfaceFromPlan(plan);
  const auto validation =
      api::ValidatePerformanceOptimizationSurfaceSnapshot(snapshot);
  Require(validation.ok,
          "DPC-042 performance optimization snapshot failed validation");
  const auto serialized =
      api::SerializePerformanceOptimizationSurfaceJson(snapshot);
  Require(Contains(serialized,
                   "\"summary_prune_status\":\"time_range_summary_prune\""),
          "DPC-042 management JSON missing time-range prune status");
  Require(Contains(serialized, "\"summary_prune_ranges_pruned\":3"),
          "DPC-042 management JSON missing pruned ranges");
  Require(Contains(serialized,
                   "\"summary_prune_authority_source\":\"engine_mga_base_pages\""),
          "DPC-042 management JSON missing authority source");
  auto invalid = snapshot;
  invalid.summary_prune_authority_source.clear();
  const auto invalid_validation =
      api::ValidatePerformanceOptimizationSurfaceSnapshot(invalid);
  Require(!invalid_validation.ok,
          "DPC-042 empty summary-prune authority source was accepted");
}

}  // namespace

int main() {
  ValidateGeneratedUuidSafeDescriptors();
  ValidatePositivePruneAndEquality();
  ValidateExactFallbacks();
  ValidateExplainAndManagementEvidence();
  std::cout << kGateSearchKey << "=passed "
            << kImplementationSearchKey << "=covered "
            << "exact_fallback=true "
            << "base_row_mga_security_recheck=true\n";
  return EXIT_SUCCESS;
}
