// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "index_optimizer_integration.hpp"
#include "database_lifecycle_test_memory.hpp"
#include "observability/performance_optimization_surface.hpp"
#include "optimizer_explain.hpp"
#include "uuid.hpp"

#include <chrono>
#include <cstdlib>
#include <iostream>
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

constexpr std::string_view kGateSearchKey =
    "DPC_PAGE_EXTENT_SUMMARY_PRUNE_OBSERVABILITY_GATE";
constexpr std::string_view kPlannerSearchKey =
    "DPC_PAGE_EXTENT_SUMMARY_PRUNE_PLANNER";

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
    Require(generated.ok(), "DPC-013 UUID generation failed");
    return generated.value;
  }

  std::string Text(UuidKind kind, std::uint64_t salt) const {
    return uuid::UuidToString(Typed(kind, salt).value);
  }
};

idx::PageExtentSummaryFormatCompatibility CurrentFormat() {
  const auto contract = idx::PageExtentSummaryPersistedFormatContract();
  idx::PageExtentSummaryFormatCompatibility format;
  format.observed = contract.current;
  format.open_class = idx::PageExtentSummaryFormatOpenClass::current;
  format.compatible = true;
  format.migration_required = false;
  format.diagnostic_code = "DPC-013.current_format";
  return format;
}

idx::PageExtentSummaryMetadata Summary(const UuidFactory& uuids,
                                       std::uint64_t salt,
                                       std::uint64_t first_page,
                                       std::uint32_t page_count,
                                       std::string min_value,
                                       std::string max_value) {
  const auto contract = idx::PageExtentSummaryPersistedFormatContract();
  idx::PageExtentSummaryMetadata metadata;
  metadata.relation_uuid = uuids.Text(UuidKind::object, 1000);
  metadata.summary_uuid = uuids.Text(UuidKind::object, salt);
  metadata.range.kind = idx::PageExtentSummaryRangeKind::page_range;
  metadata.range.first_page_id = first_page;
  metadata.range.page_count = page_count;
  metadata.boundary.scalar_type_key = "int64_lex";
  metadata.boundary.encoded_min = std::move(min_value);
  metadata.boundary.encoded_max = std::move(max_value);
  metadata.boundary.min_present = true;
  metadata.boundary.max_present = true;
  metadata.row_count = page_count * 32;
  metadata.status = idx::PageExtentSummaryStatus::current;
  metadata.format_version = contract.current;
  metadata.generation = 90 + salt;
  metadata.persisted_record_present = true;
  metadata.checksum_valid = true;
  return metadata;
}

idx::PageExtentSummaryPrunePredicate Predicate() {
  idx::PageExtentSummaryPrunePredicate predicate;
  predicate.scalar_type_key = "int64_lex";
  predicate.encoded_lower = "045";
  predicate.encoded_upper = "060";
  predicate.lower_present = true;
  predicate.upper_present = true;
  return predicate;
}

idx::PageExtentSummaryPrunePlan Plan(
    std::vector<idx::PageExtentSummaryMetadata> summaries,
    idx::PageExtentSummaryFormatCompatibility format = CurrentFormat()) {
  idx::PageExtentSummaryPruneRequest request;
  request.summaries = std::move(summaries);
  request.format = std::move(format);
  request.predicate = Predicate();
  return idx::PlanPageExtentSummaryPrune(request);
}

void RequireFallback(const idx::PageExtentSummaryPrunePlan& plan,
                     std::string_view reason,
                     std::string_view message) {
  if (!plan.full_scan_fallback || plan.fallback_reason != reason) {
    std::cerr << "fallback=" << plan.fallback_reason
              << " selected_access=" << plan.selected_access
              << " diagnostic=" << plan.diagnostic.diagnostic_code << '\n';
  }
  Require(plan.full_scan_fallback, message);
  Require(plan.selected_access == "full_scan", message);
  Require(plan.fallback_reason == reason, message);
  Require(!plan.summary_metadata_visibility_authority &&
              !plan.summary_metadata_finality_authority,
          "DPC-013 fallback made summary metadata authoritative");
}

void TestPositiveSummaryPrune() {
  const UuidFactory uuids;
  const auto first = Summary(uuids, 1, 200, 4, "001", "010");
  const auto second = Summary(uuids, 2, 204, 6, "050", "080");
  const auto plan = Plan({first, second});
  Require(plan.ok(), "DPC-013 summary prune was not selected");
  Require(plan.selected_category == idx::IndexPlanCategory::summary_prune,
          "DPC-013 selected wrong plan category");
  Require(plan.selected_access == "summary_prune",
          "DPC-013 selected access missing summary_prune");
  Require(plan.prune_reason == "summary_bounds_current",
          "DPC-013 prune reason missing");
  Require(plan.fallback_reason == "none",
          "DPC-013 positive prune carried fallback reason");
  Require(plan.summary_status == "current",
          "DPC-013 summary status missing");
  Require(plan.summary_generation == second.generation,
          "DPC-013 generation did not expose current summary generation");
  Require(plan.counters.candidate_ranges == 2 &&
              plan.counters.ranges_pruned == 1 &&
              plan.counters.ranges_scanned == 1,
          "DPC-013 range counters incorrect");
  Require(plan.counters.pages_considered == 10 &&
              plan.counters.pages_pruned == 4 &&
              plan.counters.pages_scanned == 6,
          "DPC-013 page counters incorrect");
  Require(plan.base_row_mga_recheck_required &&
              plan.base_row_security_recheck_required,
          "DPC-013 prune did not preserve base-row recheck");
  Require(!plan.summary_metadata_visibility_authority &&
              !plan.summary_metadata_finality_authority,
          "DPC-013 made summary metadata visibility/finality authority");
}

void TestFallbacks() {
  const UuidFactory uuids;
  auto metadata = Summary(uuids, 10, 300, 4, "001", "010");

  auto stale = metadata;
  stale.status = idx::PageExtentSummaryStatus::stale;
  RequireFallback(Plan({stale}),
                  "stale_summary_full_scan",
                  "DPC-013 stale summary did not force full scan fallback");

  RequireFallback(Plan({}),
                  "missing_summary_full_scan",
                  "DPC-013 missing summary did not force full scan fallback");

  auto corrupt = metadata;
  corrupt.checksum_valid = false;
  corrupt.status = idx::PageExtentSummaryStatus::corrupt;
  const auto corrupt_plan = Plan({corrupt});
  RequireFallback(corrupt_plan,
                  "corrupt_summary_full_scan",
                  "DPC-013 corrupt summary did not force full scan fallback");
  Require(corrupt_plan.summary_status == "corrupt",
          "DPC-013 corrupt fallback did not expose summary status");

  auto incompatible_format = CurrentFormat();
  incompatible_format.compatible = false;
  incompatible_format.open_class = idx::PageExtentSummaryFormatOpenClass::refused;
  incompatible_format.diagnostic_code = "DPC-013.incompatible_format";
  RequireFallback(Plan({metadata}, incompatible_format),
                  "incompatible_summary_full_scan",
                  "DPC-013 incompatible summary did not force full scan fallback");

  auto invalid_identity = metadata;
  invalid_identity.summary_uuid = "00000000-0000-0000-0000-000000000000";
  RequireFallback(Plan({invalid_identity}),
                  "invalid_identity_full_scan",
                  "DPC-013 invalid summary identity did not force fallback");

  auto parser_authority = metadata;
  parser_authority.parser_finality_authority_claimed = true;
  RequireFallback(Plan({parser_authority}),
                  "external_finality_authority_full_scan",
                  "DPC-013 parser finality claim was not refused");

  auto donor_authority = metadata;
  donor_authority.donor_finality_authority_claimed = true;
  RequireFallback(Plan({donor_authority}),
                  "external_finality_authority_full_scan",
                  "DPC-013 donor finality claim was not refused");

  auto write_ahead_authority = metadata;
  write_ahead_authority.write_ahead_log_finality_authority_claimed = true;
  RequireFallback(Plan({write_ahead_authority}),
                  "external_finality_authority_full_scan",
                  "DPC-013 write-ahead finality claim was not refused");

  idx::PageExtentSummaryPruneRequest unsafe_recheck;
  unsafe_recheck.summaries = {metadata};
  unsafe_recheck.format = CurrentFormat();
  unsafe_recheck.predicate = Predicate();
  unsafe_recheck.base_row_mga_recheck_required = false;
  RequireFallback(idx::PlanPageExtentSummaryPrune(unsafe_recheck),
                  "external_finality_authority_full_scan",
                  "DPC-013 missing MGA recheck did not refuse summary prune");
}

opt::BoundOptimizerRequest ExplainRequest() {
  opt::BoundOptimizerRequest request;
  request.context.request_uuid = "request-dpc013-redacted";
  request.context.operation_id = "dml.select_rows";
  request.context.sblr_digest = "sblr:dpc013-bound-read";
  request.context.descriptor_set_digest = "descriptor:dpc013";
  request.context.statistics_snapshot_id = "stats:dpc013";
  request.context.executor_capability_set_id = "executor:local";
  request.context.catalog_epoch = 13;
  request.context.security_epoch = 17;
  request.context.security_context_present = true;
  request.context.transaction_context_present = true;
  request.logical_plan.ok = true;
  request.logical_plan.plan_id = "dpc013-summary-prune-plan";
  request.logical_plan.nodes.push_back(planner::MakeLogicalPlanNode(
      planner::LogicalPlanNodeKind::kDmlRead,
      planner::PhysicalAccessKind::kBitmapSummaryScan,
      "dml.select_rows",
      "summary_prune"));
  return request;
}

void TestExplainRendering() {
  const UuidFactory uuids;
  const auto first = Summary(uuids, 20, 400, 4, "001", "010");
  const auto second = Summary(uuids, 21, 404, 6, "050", "080");
  const auto plan = Plan({first, second});

  opt::PlanCandidate candidate;
  candidate.candidate_id = "CAND-DPC-013-SUMMARY-PRUNE";
  candidate.access_kind = planner::PhysicalAccessKind::kBitmapSummaryScan;
  candidate.cost.selectable = true;
  candidate.cost.total_cost = 42;
  candidate.selected = true;
  candidate.summary_prune_evidence = opt::BuildPlanSummaryPruneEvidence(plan);

  opt::BoundOptimizerResult result;
  result.ok = true;
  result.diagnostic_code = "SB_OPT_OK";
  result.plan_id = "dpc013-summary-prune-plan";
  result.candidates.push_back(candidate);
  result.diagnostics.push_back(std::string(kPlannerSearchKey));

  auto document = opt::BuildOptimizerExplainDocument(ExplainRequest(), result);
  document.redactions.push_back("catalog_summary_identity_redacted");
  const auto json = opt::RenderOptimizerExplainJson(document);
  Require(Contains(json, "\"summary_prune\""),
          "DPC-013 EXPLAIN missing summary prune block");
  Require(Contains(json, "\"selected_access\":\"summary_prune\""),
          "DPC-013 EXPLAIN missing selected summary prune access");
  Require(Contains(json, "\"prune_reason\":\"summary_bounds_current\""),
          "DPC-013 EXPLAIN missing prune reason");
  Require(Contains(json, "\"fallback_reason\":\"none\""),
          "DPC-013 EXPLAIN missing fallback reason");
  Require(Contains(json, "\"pages_considered\":10") &&
              Contains(json, "\"pages_pruned\":4") &&
              Contains(json, "\"pages_scanned\":6"),
          "DPC-013 EXPLAIN missing page counters");
  Require(Contains(json, "\"base_row_mga_recheck_required\":true"),
          "DPC-013 EXPLAIN missing MGA recheck proof");
  Require(Contains(json, "\"redaction_state\":\"catalog_identity_redacted\""),
          "DPC-013 EXPLAIN missing redaction state");
  Require(!Contains(json, first.summary_uuid) &&
              !Contains(json, second.summary_uuid),
          "DPC-013 EXPLAIN leaked raw summary UUID");
  Require(!Contains(json, "SELECT ") && !Contains(json, "select "),
          "DPC-013 EXPLAIN leaked SQL text");
}

api::PerformanceOptimizationSurfaceSnapshot SurfaceFromPlan(
    const idx::PageExtentSummaryPrunePlan& plan) {
  auto snapshot = api::DefaultPerformanceOptimizationSurfaceSnapshot();
  snapshot.optimization_profile = "dpc013_summary_prune_observability";
  snapshot.summary_prune_enabled = true;
  snapshot.summary_prune_status = plan.selected_access;
  snapshot.summary_prune_last_reason = plan.prune_reason;
  snapshot.summary_prune_fallback_reason = plan.fallback_reason;
  snapshot.summary_prune_summary_status = plan.summary_status;
  snapshot.summary_prune_generation = plan.summary_generation;
  snapshot.summary_prune_ranges_considered = plan.counters.candidate_ranges;
  snapshot.summary_prune_ranges_pruned = plan.counters.ranges_pruned;
  snapshot.summary_prune_ranges_scanned = plan.counters.ranges_scanned;
  snapshot.summary_prune_pages_considered = plan.counters.pages_considered;
  snapshot.summary_prune_pages_pruned = plan.counters.pages_pruned;
  snapshot.summary_prune_pages_scanned = plan.counters.pages_scanned;
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

std::string FieldValue(const api::EngineApiResult& result,
                       std::string_view field_name) {
  for (const auto& row : result.result_shape.rows) {
    for (const auto& field : row.fields) {
      if (field.first == field_name) {
        return field.second.encoded_value;
      }
    }
  }
  return {};
}

void TestManagementAndSupportBundleSurface() {
  for (const auto& field : api::PerformanceOptimizationSurfaceSchema()) {
    Require(!Contains(field.name, "uuid"),
            "DPC-013 user-facing schema exposed raw UUID field");
  }

  const UuidFactory uuids;
  const auto plan = Plan({Summary(uuids, 30, 500, 4, "001", "010"),
                          Summary(uuids, 31, 504, 6, "050", "080")});
  const auto snapshot = SurfaceFromPlan(plan);
  const auto validation =
      api::ValidatePerformanceOptimizationSurfaceSnapshot(snapshot);
  Require(validation.ok,
          "DPC-013 performance optimization snapshot failed validation");
  const auto serialized =
      api::SerializePerformanceOptimizationSurfaceJson(snapshot);
  Require(Contains(serialized, "\"summary_prune_status\":\"summary_prune\""),
          "DPC-013 management JSON missing summary prune status");
  Require(Contains(serialized, "\"summary_prune_pages_pruned\":4"),
          "DPC-013 management JSON missing pruned pages");
  Require(Contains(serialized,
                   "\"summary_prune_base_row_mga_recheck_required\":true"),
          "DPC-013 management JSON missing MGA recheck field");

  api::EngineRequestContext context;
  context.security_context_present = true;
  context.request_id = "dpc013-performance-surface";
  context.catalog_generation_id = 13;
  context.security_epoch = 17;
  context.resource_epoch = 19;
  context.trace_tags = {"DPC-013",
                        "DPC_TEST_PAGE_SUMMARY",
                        "right:OBS_INDEX_PROFILE_READ",
                        std::string(kGateSearchKey)};
  scratchbird::tests::database_lifecycle::MaterializeAuthorizationRights(
      &context,
      "dpc_page_extent_summary_prune_observability_gate",
      {"OBS_INDEX_PROFILE_READ",
       "OBS_MANAGEMENT_INSPECT",
       "MGA_CLEANUP_INSPECT"});

  api::EngineInspectPerformanceOptimizationSurfaceRequest request;
  request.context = context;
  request.snapshot = snapshot;
  request.snapshot_present = true;
  const auto result = api::EngineInspectPerformanceOptimizationSurface(request);
  Require(result.ok, "DPC-013 performance optimization surface failed");
  Require(result.management_api_ready && result.support_bundle_ready,
          "DPC-013 management/support bundle surface not ready");
  Require(FieldValue(result, "summary_prune_pages_pruned") == "4",
          "DPC-013 behavior row missing summary prune pages pruned");
  Require(Contains(result.management_api_json, "\"summary_prune_generation\":"),
          "DPC-013 management surface missing generation");
  Require(Contains(result.support_bundle_json, "\"summary_prune_fallback_reason\""),
          "DPC-013 support bundle missing fallback reason");
  Require(Contains(result.support_bundle_json, "\"forbidden_fields_absent\":true"),
          "DPC-013 support bundle redaction proof missing");
  Require(!Contains(result.management_api_json, "docs" "/execution-plans") &&
              !Contains(result.support_bundle_json, "docs" "/execution-plans"),
          "DPC-013 observability surface depends on execution_plan artifacts");
  Require(!result.parser_finality_authority && !result.donor_finality_authority,
          "DPC-013 management surface claimed parser or donor finality");

  auto invalid = snapshot;
  invalid.summary_prune_pages_scanned = 7;
  const auto invalid_validation =
      api::ValidatePerformanceOptimizationSurfaceSnapshot(invalid);
  Require(!invalid_validation.ok,
          "DPC-013 invalid summary prune counters were accepted");
}

}  // namespace

int main() {
  TestPositiveSummaryPrune();
  TestFallbacks();
  TestExplainRendering();
  TestManagementAndSupportBundleSurface();
  std::cout << kGateSearchKey << "=passed " << kPlannerSearchKey << "=covered\n";
  return EXIT_SUCCESS;
}
