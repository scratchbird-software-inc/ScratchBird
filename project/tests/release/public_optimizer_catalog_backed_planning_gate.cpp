// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "access_path_full.hpp"
#include "logical_plan.hpp"
#include "optimizer_catalog_backed_planning.hpp"
#include "statistics_catalog.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace opt = scratchbird::engine::optimizer;
namespace plan = scratchbird::engine::planner;

namespace {

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

bool Contains(const std::vector<std::string>& values, std::string_view expected) {
  return std::find(values.begin(), values.end(), std::string(expected)) != values.end();
}

bool ContainsText(const std::vector<std::string>& values, std::string_view expected) {
  return std::any_of(values.begin(), values.end(), [&](const auto& value) {
    return value.find(expected) != std::string::npos;
  });
}

std::string Id(std::string_view suffix) {
  return "pcr061." + std::string(suffix);
}

opt::OptimizerStatsIdentity Identity(std::string object_uuid,
                                     std::string statistic_uuid) {
  opt::OptimizerStatsIdentity identity;
  identity.object_uuid = std::move(object_uuid);
  identity.statistic_uuid = std::move(statistic_uuid);
  identity.stats_epoch = 6101;
  identity.catalog_epoch = 6100;
  identity.transaction_visibility_epoch = 6099;
  identity.freshness = opt::OptimizerStatsFreshnessState::kFresh;
  identity.source = opt::StatisticSource::kCatalogExact;
  identity.confidence = opt::CostConfidence::kHigh;
  return identity;
}

opt::TableCardinalityStats TableStats() {
  opt::TableCardinalityStats stats;
  stats.identity = Identity(Id("relation.customer"), Id("table_stats.customer"));
  stats.row_count = 10000;
  stats.visible_row_count = 9900;
  stats.page_count = 96;
  stats.average_row_bytes = 96;
  return stats;
}

opt::IndexStats IndexStats() {
  opt::IndexStats index;
  index.identity = Identity(Id("relation.customer"), Id("index.customer_pk.stats"));
  index.index_uuid = Id("index.customer_pk");
  index.relation_uuid = Id("relation.customer");
  index.index_family = "btree";
  index.descriptor_digest = Id("descriptor.customer");
  index.collation_identity = "collation.pcr061.binary";
  index.key_column_uuids = {Id("column.customer_id")};
  index.height = 2;
  index.leaf_pages = 24;
  index.distinct_keys = 9000;
  index.visibility_coverage = 1.0;
  index.equality_lookup_supported = true;
  index.ordered_range_supported = true;
  index.exact_recheck_required = true;
  index.mga_recheck_required = true;
  index.security_recheck_required = true;
  index.route_benchmark_clean = true;
  return index;
}

plan::OptimizerPolicyMetadata SafePolicy() {
  plan::OptimizerPolicyMetadata policy;
  policy.optimizer_policy_metadata_present = true;
  policy.policy_source_kind = "sblr_api";
  policy.policy_epoch = 6100;
  policy.normalized_controls.plan_profile_id = "plan_profile:pcr061_catalog";
  policy.normalized_controls.join_search_policy_id = "join_search:pcr061_bounded";
  policy.normalized_controls.memory_policy_id = "memory_policy:pcr061_governed";
  policy.normalized_controls.spill_policy_id = "spill_policy:pcr061_bounded";
  policy.normalized_controls.parallelism_policy_id = "parallelism:pcr061_single_node";
  policy.normalized_controls.what_if_policy_id = "what_if:pcr061_disabled";
  policy.normalized_controls.safe_control_ids = {
      "catalog_stats:required",
      "mga_recheck:preserved"};
  policy.safe_control_ids = {
      "security_recheck:preserved",
      "redaction_digest:required"};
  return policy;
}

plan::LogicalPlan LogicalPlan() {
  plan::LogicalPlan logical;
  logical.ok = true;
  logical.plan_id = Id("logical_plan.customer_lookup");
  logical.optimizer_policy = SafePolicy();
  auto node = plan::MakeLogicalPlanNode(plan::LogicalPlanNodeKind::kDmlRead,
                                        plan::PhysicalAccessKind::kNone,
                                        Id("operation.customer_lookup"),
                                        "customer_lookup");
  node.required_object_uuids.push_back(Id("relation.customer"));
  node.required_descriptors.push_back(Id("descriptor.customer"));
  logical.nodes.push_back(std::move(node));
  return logical;
}

opt::AccessPathPlanningRequest AccessRequest() {
  opt::AccessPathPlanningRequest request;
  request.relation_uuid = Id("relation.customer");
  request.predicate_kind = "scalar_eq";
  request.descriptor_digest = Id("descriptor.customer");
  request.collation_identity = "collation.pcr061.binary";
  request.projected_column_uuids = {Id("column.customer_id")};
  request.visibility_proven = true;
  request.grants_proven = true;
  request.base_row_mga_recheck_planned = true;
  request.base_row_security_recheck_planned = true;
  request.table_stats = TableStats();
  request.candidate_indexes = {IndexStats()};
  return request;
}

opt::BoundOptimizerRequest BoundRequest() {
  opt::BoundOptimizerRequest request;
  request.context.request_uuid = Id("request.customer_lookup");
  request.context.operation_id = Id("operation.customer_lookup");
  request.context.sblr_digest = "sha256:sblr-pcr061-customer-lookup";
  request.context.descriptor_set_digest = Id("descriptor.customer");
  request.context.statistics_snapshot_id = "sha256:stats-pcr061-customer";
  request.context.metric_snapshot_id = "sha256:metrics-pcr061-customer";
  request.context.executor_capability_set_id = "executor-capability:pcr061-local-mga";
  request.context.catalog_epoch = 6100;
  request.context.stats_epoch = 6101;
  request.context.security_epoch = 6102;
  request.context.redaction_epoch = 6103;
  request.context.policy_epoch = 6104;
  request.context.resource_epoch = 6105;
  request.context.name_resolution_epoch = 6106;
  request.context.memory_policy_epoch = 6107;
  request.context.memory_feedback_generation = 6108;
  request.context.route_epoch = 6109;
  request.context.security_context_present = true;
  request.context.transaction_context_present = true;
  request.logical_plan = LogicalPlan();
  request.catalog_access_path_request = AccessRequest();
  return request;
}

opt::OptimizerPlanCacheKeyInput CacheKeyInput() {
  opt::OptimizerPlanCacheKeyInput input;
  input.operation_id = Id("cache.customer_lookup");
  input.sblr_digest = "sha256:sblr-pcr061-cache-customer";
  input.descriptor_set_digest = Id("descriptor.customer");
  input.statistics_snapshot_id = "sha256:stats-pcr061-customer";
  input.catalog_stats_digest = "sha256:catalog-stats-pcr061-customer";
  input.cost_profile_id = "cost-profile:pcr061-catalog";
  input.executor_capability_set_id = "executor-capability:pcr061-local-mga";
  input.route_capability_digest = "sha256:route-capability-pcr061-local-index";
  input.security_policy_digest = "sha256:security-policy-pcr061-reader";
  input.redaction_route_digest = "sha256:redaction-route-pcr061-mask";
  input.normalized_optimizer_controls_digest = "sha256:optimizer-controls-pcr061";
  input.parameter_shape_digest = "sha256:parameter-shape-pcr061";
  input.memory_grant_class = "grant-class:pcr061-small-governed";
  input.memory_grant_digest = "sha256:memory-grant-pcr061";
  input.catalog_epoch = 6100;
  input.stats_epoch = 6101;
  input.security_epoch = 6102;
  input.redaction_epoch = 6103;
  input.policy_epoch = 6104;
  input.resource_epoch = 6105;
  input.name_resolution_epoch = 6106;
  input.memory_policy_epoch = 6107;
  input.memory_feedback_generation = 6108;
  input.compatibility_epoch = 6109;
  input.format_compatibility_epoch = 6110;
  input.route_epoch = 6111;
  input.object_uuids = {Id("relation.customer")};
  input.function_uuids = {Id("function.redaction")};
  input.index_uuids = {Id("index.customer_pk")};
  input.filespace_uuids = {Id("filespace.hot")};
  input.dependency_digests = {
      "sha256:dep-pcr061-relation-customer",
      "sha256:dep-pcr061-index-customer-pk",
      "sha256:dep-pcr061-function-redaction",
      "sha256:dep-pcr061-stats-customer",
      "sha256:dep-pcr061-route-local-index",
      "sha256:dep-pcr061-memory-feedback"};
  return input;
}

opt::CatalogBackedProductionPlanningRequest ProductionRequest() {
  opt::CatalogBackedProductionPlanningRequest request;
  request.bound_request = BoundRequest();
  request.plan_cache_key_input = CacheKeyInput();
  request.production_build = true;
  request.require_index_stats = true;
  return request;
}

void ValidCatalogBackedProductionPlanIsAdmitted() {
  const auto request = ProductionRequest();
  const auto validation =
      opt::ValidateCatalogBackedProductionPlanningRequest(request);
  Require(validation.ok, "PCR-061 valid production request was refused");
  Require(validation.catalog_backed, "PCR-061 catalog-backed evidence missing");
  Require(validation.benchmark_clean_ready,
          "PCR-061 benchmark-clean readiness missing");
  Require(Contains(validation.evidence,
                   "local_default_statistics_diagnostic_only=true"),
          "PCR-061 local-default diagnostic-only evidence missing");
  Require(Contains(validation.evidence,
                   "policy_default_statistics_diagnostic_only=true"),
          "PCR-061 policy-default diagnostic-only evidence missing");

  const auto result = opt::OptimizeCatalogBackedProductionPlan(request);
  Require(result.validation.ok, "PCR-061 admitted request failed optimization");
  Require(result.bound_result.ok, "PCR-061 bound optimizer result failed");
  Require(result.optimized_plan.ok, "PCR-061 optimized plan failed");
  const auto benchmark =
      opt::ValidateBenchmarkCleanOptimizedPlan(result.optimized_plan);
  Require(benchmark.ok, "PCR-061 admitted plan was not benchmark-clean");
}

void RequiredEvidenceFailsClosed() {
  auto request = ProductionRequest();
  request.bound_request.catalog_access_path_request->table_stats = std::nullopt;
  auto validation = opt::ValidateCatalogBackedProductionPlanningRequest(request);
  Require(!validation.ok &&
              Contains(validation.diagnostics,
                       "SB_OPT_CATALOG_BACKED_PLANNING.TABLE_STATS_REQUIRED"),
          "PCR-061 missing table stats admitted");

  request = ProductionRequest();
  request.bound_request.catalog_access_path_request->candidate_indexes.clear();
  validation = opt::ValidateCatalogBackedProductionPlanningRequest(request);
  Require(!validation.ok &&
              Contains(validation.diagnostics,
                       "SB_OPT_CATALOG_BACKED_PLANNING.INDEX_STATS_REQUIRED"),
          "PCR-061 missing index stats admitted");

  request = ProductionRequest();
  request.bound_request.catalog_access_path_request->visibility_proven = false;
  request.bound_request.catalog_access_path_request->grants_proven = false;
  validation = opt::ValidateCatalogBackedProductionPlanningRequest(request);
  Require(!validation.ok &&
              Contains(validation.diagnostics,
                       "SB_OPT_CATALOG_BACKED_PLANNING.VISIBILITY_PROOF_REQUIRED") &&
              Contains(validation.diagnostics,
                       "SB_OPT_CATALOG_BACKED_PLANNING.GRANT_PROOF_REQUIRED"),
          "PCR-061 visibility/grant gaps admitted");

  request = ProductionRequest();
  request.bound_request.catalog_access_path_request->descriptor_digest =
      "sha256:descriptor-mismatch";
  validation = opt::ValidateCatalogBackedProductionPlanningRequest(request);
  Require(!validation.ok &&
              Contains(validation.diagnostics,
                       "SB_OPT_CATALOG_BACKED_PLANNING.DESCRIPTOR_DIGEST_REQUIRED"),
          "PCR-061 descriptor mismatch admitted");

  request = ProductionRequest();
  request.plan_cache_key_input.memory_grant_digest = "memory:default";
  validation = opt::ValidateCatalogBackedProductionPlanningRequest(request);
  Require(!validation.ok &&
              Contains(validation.diagnostics,
                       "SB_OPTIMIZER_PLAN_CACHE_ENTERPRISE_KEY_INCOMPLETE"),
          "PCR-061 placeholder memory grant admitted");

  request = ProductionRequest();
  request.plan_cache_key_input.redaction_route_digest = "redaction:default";
  validation = opt::ValidateCatalogBackedProductionPlanningRequest(request);
  Require(!validation.ok &&
              Contains(validation.diagnostics,
                       "SB_OPTIMIZER_PLAN_CACHE_ENTERPRISE_KEY_INCOMPLETE"),
          "PCR-061 placeholder redaction route admitted");
}

void DefaultStatisticsRemainDiagnosticOnly() {
  auto request = ProductionRequest();
  request.bound_request.catalog_access_path_request->table_stats->identity.source =
      opt::StatisticSource::kPolicyDefault;
  auto validation = opt::ValidateCatalogBackedProductionPlanningRequest(request);
  Require(!validation.ok &&
              validation.local_or_policy_default_diagnostic_only &&
              ContainsText(validation.diagnostics,
                           "LOCAL_DEFAULT_STATS_FORBIDDEN"),
          "PCR-061 policy/default table stats admitted");

  request = ProductionRequest();
  request.bound_request.catalog_access_path_request = std::nullopt;
  validation = opt::ValidateCatalogBackedProductionPlanningRequest(request);
  Require(!validation.ok &&
              validation.local_or_policy_default_diagnostic_only &&
              Contains(validation.diagnostics,
                       "SB_OPT_CATALOG_BACKED_PLANNING.CATALOG_ACCESS_REQUEST_REQUIRED"),
          "PCR-061 statistics-only request admitted");

  const auto statistics_only = opt::OptimizeLogicalPlan(LogicalPlan());
  const auto benchmark =
      opt::ValidateBenchmarkCleanOptimizedPlan(statistics_only);
  Require(!benchmark.ok &&
              benchmark.diagnostic_code ==
                  "SB_ORH_CATALOG_BACKED_PUBLIC_SQL_ROUTE.STATISTICS_ONLY_NOT_BENCHMARK_CLEAN",
          "PCR-061 statistics-only plan became benchmark-clean");
}

void ParserAndDonorAuthorityAreRejected() {
  auto request = ProductionRequest();
  request.bound_request.context.parser_owned_claims_present = true;
  auto validation = opt::ValidateCatalogBackedProductionPlanningRequest(request);
  Require(!validation.ok &&
              Contains(validation.diagnostics,
                       "SB_OPT_AUTHORITY_REJECTED.parser_owned_claims"),
          "PCR-061 parser authority admitted");

  request = ProductionRequest();
  request.bound_request.logical_plan.optimizer_policy.donor_or_legacy_policy_authority_claimed = true;
  validation = opt::ValidateCatalogBackedProductionPlanningRequest(request);
  Require(!validation.ok &&
              Contains(validation.diagnostics,
                       "SB_OPT_AUTHORITY_REJECTED.optimizer_policy_donor_or_legacy_authority"),
          "PCR-061 donor policy authority admitted");
}

}  // namespace

int main() {
  ValidCatalogBackedProductionPlanIsAdmitted();
  RequiredEvidenceFailsClosed();
  DefaultStatisticsRemainDiagnosticOnly();
  ParserAndDonorAuthorityAreRejected();
  std::cout << "public optimizer catalog-backed planning gate passed\n";
  return EXIT_SUCCESS;
}
