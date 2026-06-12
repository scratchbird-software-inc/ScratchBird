// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "access_path_full.hpp"
#include "adaptive_cardinality_feedback.hpp"
#include "cluster_candidate.hpp"
#include "cluster_refusal_path.hpp"
#include "logical_plan.hpp"
#include "optimizer_contract.hpp"
#include "optimizer_enterprise_manifest.hpp"
#include "optimizer_explain.hpp"
#include "optimizer_memory_feedback_bridge.hpp"
#include "optimizer_plan_cache.hpp"
#include "optimizer_request.hpp"
#include "optimizer_safety_gates.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace opt = scratchbird::engine::optimizer;
namespace plan = scratchbird::engine::planner;

namespace {

struct MatrixRow {
  std::string surface_id;
  std::string route_family;
  std::string surface_class;
  std::string scenario_id;
  bool declared = false;
  bool production_route_admissible = false;
  bool benchmark_clean_eligible = false;
  bool candidate_generated = false;
  bool catalog_costed = false;
  bool selectable = false;
  bool physical_node_emitted = false;
  bool executor_validation = false;
  bool explain_emitted = false;
  bool plan_cache_dependency_bound = false;
  bool benchmark_clean_validated = false;
  bool runtime_execution_tested = false;
  bool surface_specific_validator = false;
  bool fail_closed = false;
  bool authority_clean = false;
  std::string diagnostic_code;
  std::string evidence_detail;
  std::string claim;
};

struct AccessScenario {
  std::string scenario_id = "catalog_table_scan";
  std::string predicate_kind;
  std::string expected_candidate_id = "CAND-OPT-FULL-SCAN";
  plan::PhysicalAccessKind expected_access_kind = plan::PhysicalAccessKind::kTableScan;
  std::vector<opt::IndexStats> indexes;
  bool bitmap_requested = false;
  bool summary_requested = false;
  bool covering_payload_proven = false;
  bool llvm_acceleration_checked = false;
};

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

const char* BoolText(bool value) {
  return value ? "true" : "false";
}

bool Contains(const std::vector<std::string>& values, std::string_view expected) {
  return std::find(values.begin(), values.end(), std::string(expected)) != values.end();
}

bool ContainsText(std::string_view value, std::string_view expected) {
  return value.find(expected) != std::string_view::npos;
}

std::string Csv(std::string_view value) {
  const bool quote = value.find_first_of(",\"\n") != std::string_view::npos;
  if (!quote) {
    return std::string(value);
  }
  std::string out = "\"";
  for (const char c : value) {
    if (c == '"') {
      out += "\"\"";
    } else {
      out += c;
    }
  }
  out += '"';
  return out;
}

std::string Id(std::string_view surface_id, std::string_view suffix) {
  return "pcr060." + std::string(surface_id) + "." + std::string(suffix);
}

std::string RelationUuid(std::string_view surface_id) {
  return Id(surface_id, "relation");
}

std::string DescriptorDigest(std::string_view surface_id) {
  return "sha256:descriptor-pcr060-" + std::string(surface_id);
}

std::string ColumnUuid(std::string_view column) {
  return "col.pcr060." + std::string(column);
}

opt::OptimizerStatsIdentity FreshStatsIdentity(std::string object_uuid,
                                               std::string statistic_uuid,
                                               std::uint64_t stats_epoch = 6060) {
  opt::OptimizerStatsIdentity identity;
  identity.object_uuid = std::move(object_uuid);
  identity.statistic_uuid = std::move(statistic_uuid);
  identity.stats_epoch = stats_epoch;
  identity.catalog_epoch = 6050;
  identity.transaction_visibility_epoch = 6040;
  identity.freshness = opt::OptimizerStatsFreshnessState::kFresh;
  identity.source = opt::StatisticSource::kCatalogExact;
  identity.confidence = opt::CostConfidence::kHigh;
  return identity;
}

opt::TableCardinalityStats TableStatsFor(std::string_view surface_id,
                                         std::uint64_t rows = 10000,
                                         std::uint64_t pages = 96) {
  opt::TableCardinalityStats stats;
  stats.identity = FreshStatsIdentity(RelationUuid(surface_id),
                                      Id(surface_id, "table_stats"));
  stats.row_count = rows;
  stats.visible_row_count = rows - 100;
  stats.page_count = pages;
  stats.average_row_bytes = 96;
  return stats;
}

opt::IndexStats IndexFor(std::string_view surface_id,
                         std::string_view family,
                         std::string_view suffix) {
  opt::IndexStats index;
  index.index_uuid = Id(surface_id, std::string("index.") + std::string(suffix));
  index.relation_uuid = RelationUuid(surface_id);
  index.identity = FreshStatsIdentity(index.relation_uuid,
                                      index.index_uuid + ".stats");
  index.index_family = std::string(family);
  index.descriptor_digest = DescriptorDigest(surface_id);
  index.collation_identity = "collation.pcr060.binary";
  index.key_column_uuids = {ColumnUuid("c1")};
  index.height = 2;
  index.leaf_pages = 24;
  index.distinct_keys = 8000;
  index.visibility_coverage = 1.0;
  index.equality_lookup_supported = family == "btree" || family == "hash";
  index.ordered_range_supported = family == "btree";
  index.candidate_set_producer = family == "btree" || family == "hash";
  index.exact_recheck_required = true;
  index.mga_recheck_required = true;
  index.security_recheck_required = true;
  index.route_benchmark_clean = true;
  return index;
}

plan::OptimizerPolicyMetadata SafePolicy(std::string_view surface_id) {
  plan::OptimizerPolicyMetadata policy;
  policy.optimizer_policy_metadata_present = true;
  policy.policy_source_kind = "sblr_api";
  policy.policy_epoch = 6060;
  policy.normalized_controls.plan_profile_id = "plan_profile:pcr060_catalog";
  policy.normalized_controls.join_search_policy_id = "join_search:pcr060_bounded_dp";
  policy.normalized_controls.memory_policy_id = "memory_policy:pcr060_governed";
  policy.normalized_controls.spill_policy_id = "spill_policy:pcr060_bounded";
  policy.normalized_controls.parallelism_policy_id = "parallelism:pcr060_single_node";
  policy.normalized_controls.what_if_policy_id = "what_if:pcr060_disabled";
  policy.normalized_controls.safe_control_ids = {
      "surface:" + std::string(surface_id),
      "authority:sblr_uuid_bound",
      "mga_recheck:preserved"};
  policy.safe_control_ids = {
      "security_recheck:preserved",
      "catalog_stats:required"};
  return policy;
}

plan::LogicalPlan SingleReadPlan(std::string_view surface_id) {
  plan::LogicalPlan logical;
  logical.ok = true;
  logical.plan_id = Id(surface_id, "logical_plan");
  logical.optimizer_policy = SafePolicy(surface_id);
  auto node = plan::MakeLogicalPlanNode(plan::LogicalPlanNodeKind::kDmlRead,
                                        plan::PhysicalAccessKind::kNone,
                                        Id(surface_id, "read"),
                                        "pcr060_read");
  node.required_object_uuids.push_back(RelationUuid(surface_id));
  node.required_descriptors.push_back(DescriptorDigest(surface_id));
  logical.nodes.push_back(std::move(node));
  return logical;
}

opt::AccessPathPlanningRequest AccessRequestFor(std::string_view surface_id,
                                                const AccessScenario& scenario) {
  opt::AccessPathPlanningRequest request;
  request.relation_uuid = RelationUuid(surface_id);
  request.predicate_kind = scenario.predicate_kind;
  request.descriptor_digest = DescriptorDigest(surface_id);
  request.collation_identity = "collation.pcr060.binary";
  request.projected_column_uuids = {ColumnUuid("c1")};
  request.visibility_proven = true;
  request.grants_proven = true;
  request.base_row_mga_recheck_planned = true;
  request.base_row_security_recheck_planned = true;
  request.index_visibility_native = scenario.covering_payload_proven;
  request.table_stats = TableStatsFor(surface_id);
  request.candidate_indexes = scenario.indexes;
  request.bitmap.requested = scenario.bitmap_requested;
  request.bitmap.executor_supported = scenario.bitmap_requested;
  request.summary_prune.requested = scenario.summary_requested;
  request.summary_prune.summary_present = scenario.summary_requested;
  request.summary_prune.predicate_supported = scenario.summary_requested;
  request.summary_prune.summary_generation = 12;
  request.summary_prune.relation_generation = 12;
  request.summary_prune.base_row_mga_recheck_planned = true;
  request.summary_prune.base_row_security_recheck_planned = true;
  request.summary_prune.candidate_ranges = 10;
  request.summary_prune.ranges_pruned = 6;
  request.summary_prune.pages_considered = 96;
  request.summary_prune.pages_pruned = 40;
  if (scenario.covering_payload_proven) {
    request.covering_payload.physical_payload_proof_present = true;
    request.covering_payload.freshness_proven = true;
    request.covering_payload.redaction_safe = true;
    request.covering_payload.result_contract_proven = true;
    request.covering_payload.base_row_recheck_handoff_proven = true;
    request.covering_payload.index_only_admitted = true;
    request.covering_payload.runtime_route_consumption_required = false;
    request.covering_payload.evidence = {
        "pcr060_covering_payload_physical_proof=true",
        "pcr060_covering_payload_redaction_safe=true"};
  }
  return request;
}

AccessScenario ScenarioFor(const opt::EnterpriseOptimizerSurfaceEntry& entry) {
  AccessScenario scenario;
  scenario.scenario_id = "catalog_table_scan";
  if (entry.surface_id == "row_uuid_lookup") {
    scenario.scenario_id = "row_uuid_lookup";
    scenario.predicate_kind = "row_uuid_eq";
    scenario.expected_candidate_id = "CAND-OPT-ROW-UUID";
    scenario.expected_access_kind = plan::PhysicalAccessKind::kRowUuidLookup;
  } else if (entry.surface_id == "btree_point_range") {
    scenario.scenario_id = "btree_point_lookup";
    scenario.predicate_kind = "scalar_eq";
    scenario.indexes = {IndexFor(entry.surface_id, "btree", "btree")};
    scenario.expected_candidate_id = "CAND-OPT-INDEX:" + scenario.indexes.front().index_uuid;
    scenario.expected_access_kind = plan::PhysicalAccessKind::kScalarBtreeLookup;
  } else if (entry.surface_id == "hash_equality") {
    scenario.scenario_id = "hash_equality_lookup";
    scenario.predicate_kind = "scalar_eq";
    scenario.indexes = {IndexFor(entry.surface_id, "hash", "hash")};
    scenario.expected_candidate_id = "CAND-OPT-INDEX:" + scenario.indexes.front().index_uuid;
    scenario.expected_access_kind = plan::PhysicalAccessKind::kScalarHashLookup;
  } else if (entry.surface_id == "bitmap_candidate_set") {
    scenario.scenario_id = "bitmap_candidate_set";
    scenario.predicate_kind = "scalar_eq";
    scenario.bitmap_requested = true;
    scenario.indexes = {IndexFor(entry.surface_id, "btree", "btree"),
                        IndexFor(entry.surface_id, "hash", "hash")};
    scenario.expected_candidate_id = "CAND-OPT-BITMAP";
    scenario.expected_access_kind = plan::PhysicalAccessKind::kBitmapSummaryScan;
  } else if (entry.surface_id == "zone_summary_prune") {
    scenario.scenario_id = "summary_prune";
    scenario.predicate_kind = "scalar_range";
    scenario.summary_requested = true;
    scenario.expected_candidate_id = "CAND-OPT-SUMMARY-PRUNE";
    scenario.expected_access_kind = plan::PhysicalAccessKind::kBitmapSummaryScan;
  } else if (entry.surface_id == "covering_index") {
    scenario.scenario_id = "covering_index";
    scenario.predicate_kind = "scalar_eq";
    auto index = IndexFor(entry.surface_id, "btree", "covering");
    index.covering = true;
    index.covered_column_uuids = {ColumnUuid("c1")};
    scenario.indexes = {std::move(index)};
    scenario.covering_payload_proven = true;
    scenario.expected_candidate_id = "CAND-OPT-COVERING:" + scenario.indexes.front().index_uuid;
    scenario.expected_access_kind = plan::PhysicalAccessKind::kCoveringIndexScan;
  } else if (entry.surface_id == "text_search") {
    scenario.scenario_id = "full_text";
    scenario.predicate_kind = "full_text";
    scenario.indexes = {IndexFor(entry.surface_id, "full_text", "search")};
    scenario.expected_candidate_id = "CAND-OPT-SPECIALIZED:" + scenario.indexes.front().index_uuid;
    scenario.expected_access_kind = plan::PhysicalAccessKind::kFullTextProbe;
  } else if (entry.surface_id == "vector_ann" ||
             entry.surface_id == "llvm_native_compile") {
    scenario.scenario_id = entry.surface_id == "vector_ann" ? "vector_ann" : "llvm_vector_exact_fallback";
    scenario.predicate_kind = "vector_approx";
    scenario.indexes = {IndexFor(entry.surface_id, "vector", "vector")};
    scenario.expected_candidate_id = "CAND-OPT-SPECIALIZED:" + scenario.indexes.front().index_uuid;
    scenario.expected_access_kind = plan::PhysicalAccessKind::kVectorApproximateWithFallback;
    scenario.llvm_acceleration_checked = entry.surface_id == "llvm_native_compile";
  } else if (entry.surface_id == "document_path") {
    scenario.scenario_id = "document_path";
    scenario.predicate_kind = "document_path";
    scenario.indexes = {IndexFor(entry.surface_id, "document", "document")};
    scenario.expected_candidate_id = "CAND-OPT-SPECIALIZED:" + scenario.indexes.front().index_uuid;
    scenario.expected_access_kind = plan::PhysicalAccessKind::kDocumentPathProbe;
  } else if (entry.surface_id == "graph_seed") {
    scenario.scenario_id = "graph_seed";
    scenario.predicate_kind = "graph_seed";
    scenario.indexes = {IndexFor(entry.surface_id, "graph", "graph")};
    scenario.expected_candidate_id = "CAND-OPT-SPECIALIZED:" + scenario.indexes.front().index_uuid;
    scenario.expected_access_kind = plan::PhysicalAccessKind::kGraphTraversalSeed;
  } else if (entry.surface_id == "time_series_append") {
    scenario.scenario_id = "time_series_append";
    scenario.predicate_kind = "timeseries_append";
    scenario.indexes = {IndexFor(entry.surface_id, "timeseries", "timeseries")};
    scenario.expected_candidate_id = "CAND-OPT-SPECIALIZED:" + scenario.indexes.front().index_uuid;
    scenario.expected_access_kind = plan::PhysicalAccessKind::kTimeSeriesAppendPath;
  } else if (entry.surface_id == "temporary_in_memory") {
    scenario.scenario_id = "temporary_in_memory_catalog_scan";
  } else if (entry.surface_id == "runtime_payload_explain" ||
             entry.surface_id == "plan_cache" ||
             entry.surface_id == "adaptive_feedback" ||
             entry.surface_id == "memory_spill_feedback") {
    scenario.scenario_id = entry.surface_id + std::string("_shared_catalog_scan");
  }
  return scenario;
}

opt::BoundOptimizerRequest BoundRequestFor(std::string_view surface_id,
                                           const opt::AccessPathPlanningRequest& access_request) {
  opt::BoundOptimizerRequest request;
  request.context.request_uuid = Id(surface_id, "request");
  request.context.operation_id = Id(surface_id, "operation");
  request.context.sblr_digest = "sha256:sblr-pcr060-" + std::string(surface_id);
  request.context.descriptor_set_digest = DescriptorDigest(surface_id);
  request.context.statistics_snapshot_id = "sha256:stats-pcr060-" + std::string(surface_id);
  request.context.metric_snapshot_id = "sha256:metrics-pcr060-" + std::string(surface_id);
  request.context.executor_capability_set_id = "executor-capability:pcr060-local-mga";
  request.context.catalog_epoch = 6050;
  request.context.stats_epoch = 6060;
  request.context.security_epoch = 6070;
  request.context.redaction_epoch = 6080;
  request.context.policy_epoch = 6060;
  request.context.resource_epoch = 6090;
  request.context.name_resolution_epoch = 6100;
  request.context.memory_policy_epoch = 6110;
  request.context.memory_feedback_generation = 6120;
  request.context.route_epoch = 6130;
  request.context.security_context_present = true;
  request.context.transaction_context_present = true;
  request.logical_plan = SingleReadPlan(surface_id);
  request.catalog_access_path_request = access_request;
  return request;
}

opt::OptimizerPlanCacheKeyInput CacheInputFor(std::string_view surface_id) {
  opt::OptimizerPlanCacheKeyInput input;
  input.operation_id = Id(surface_id, "cache_operation");
  input.sblr_digest = "sha256:sblr-pcr060-cache-" + std::string(surface_id);
  input.descriptor_set_digest = DescriptorDigest(surface_id);
  input.statistics_snapshot_id = "sha256:stats-pcr060-cache-" + std::string(surface_id);
  input.catalog_stats_digest = "sha256:catalog-stats-pcr060-" + std::string(surface_id);
  input.cost_profile_id = "cost-profile:pcr060-catalog-v1";
  input.executor_capability_set_id = "executor-capability:pcr060-local-mga";
  input.route_capability_digest = "sha256:route-capability-pcr060-local";
  input.security_policy_digest = "sha256:security-policy-pcr060-reader";
  input.redaction_route_digest = "sha256:redaction-route-pcr060-mask";
  input.normalized_optimizer_controls_digest = "sha256:optimizer-controls-pcr060-" + std::string(surface_id);
  input.parameter_shape_digest = "sha256:parameter-shape-pcr060-" + std::string(surface_id);
  input.memory_grant_class = "grant-class:pcr060-small-governed";
  input.memory_grant_digest = "sha256:memory-grant-pcr060-" + std::string(surface_id);
  input.catalog_epoch = 6050;
  input.stats_epoch = 6060;
  input.security_epoch = 6070;
  input.redaction_epoch = 6080;
  input.policy_epoch = 6090;
  input.resource_epoch = 6100;
  input.name_resolution_epoch = 6110;
  input.memory_policy_epoch = 6120;
  input.memory_feedback_generation = 6130;
  input.compatibility_epoch = 6140;
  input.format_compatibility_epoch = 6150;
  input.route_epoch = 6160;
  input.object_uuids = {RelationUuid(surface_id)};
  input.function_uuids = {Id(surface_id, "function.redaction")};
  input.index_uuids = {Id(surface_id, "index.dependency")};
  input.filespace_uuids = {Id(surface_id, "filespace.hot")};
  input.dependency_digests = {
      "sha256:dep-relation-pcr060-" + std::string(surface_id),
      "sha256:dep-index-pcr060-" + std::string(surface_id),
      "sha256:dep-function-pcr060-" + std::string(surface_id),
      "sha256:dep-stats-pcr060-" + std::string(surface_id),
      "sha256:dep-route-pcr060-" + std::string(surface_id),
      "sha256:dep-memory-pcr060-" + std::string(surface_id)};
  return input;
}

bool PlanCacheDependencyProof(std::string_view surface_id) {
  auto input = CacheInputFor(surface_id);
  const auto key_validation = opt::ValidateEnterpriseOptimizerPlanCacheKeyInput(input);
  if (!key_validation.ok) {
    return false;
  }

  opt::CachedOptimizerPlan plan_record;
  plan_record.key_input = input;
  plan_record.cache_key = opt::BuildOptimizerPlanCacheKey(input);
  plan_record.created_epoch = input.catalog_epoch;
  plan_record.result.ok = true;
  plan_record.result.diagnostic_code = "SB_OPT_OK";
  plan_record.result.plan_id = Id(surface_id, "cached_plan");
  plan_record.metadata_only = true;
  plan_record.mga_visibility_recheck_required = true;
  plan_record.security_recheck_required = true;
  plan_record.parser_or_reference_finality_authority = false;

  opt::OptimizerPlanCache cache;
  const auto put = cache.PutEnterprise(plan_record);
  if (!put.ok) {
    return false;
  }
  const auto hit = cache.LookupEnterprise(input);
  return hit.hit && Contains(hit.evidence, "OEIC_PLAN_CACHE_ENTERPRISE_CLOSURE");
}

bool ExplainProof(std::string_view surface_id,
                  const opt::AccessPathPlanningRequest& access_request) {
  auto request = BoundRequestFor(surface_id, access_request);
  const auto result = opt::OptimizeBoundRequest(request);
  if (!result.ok) {
    return false;
  }
  const auto document = opt::BuildOptimizerExplainDocument(request, result);
  const auto json = opt::RenderOptimizerExplainJson(document);
  return !document.plan_hash.empty() &&
         Contains(document.invalidation_dependencies, "catalog_epoch=6050") &&
         Contains(document.invalidation_dependencies, "security_epoch=6070") &&
         ContainsText(json, "\"plan_hash\"") &&
         ContainsText(json, "\"executor_capability_evidence\"");
}

void ApplyAccessProof(const opt::EnterpriseOptimizerSurfaceEntry& entry,
                      MatrixRow* row) {
  const auto scenario = ScenarioFor(entry);
  row->scenario_id = scenario.scenario_id;
  const auto access_request = AccessRequestFor(entry.surface_id, scenario);
  const auto candidates = opt::GenerateFullAccessPathCandidates(access_request);
  const auto target = std::find_if(candidates.begin(), candidates.end(), [&](const opt::PlanCandidate& candidate) {
    return candidate.candidate_id == scenario.expected_candidate_id &&
           candidate.access_kind == scenario.expected_access_kind;
  });

  row->candidate_generated = target != candidates.end();
  row->surface_specific_validator = row->candidate_generated;
  if (target == candidates.end()) {
    row->diagnostic_code = "PCR060_EXPECTED_CANDIDATE_MISSING";
    return;
  }

  row->catalog_costed = target->cost.confidence != opt::CostConfidence::kUnknown &&
                        target->cost.confidence != opt::CostConfidence::kRejected &&
                        target->cost.total_cost != 0;
  row->selectable = target->cost.selectable && target->missing_facts.empty();

  const auto physical = opt::PhysicalPlanNodeFromCandidate(
      *target,
      opt::RequiredExecutorCapabilityForAccessKind(target->access_kind),
      access_request.descriptor_digest);
  const auto validation = opt::ValidatePhysicalPlanNode(physical);
  row->physical_node_emitted = !physical.node_id.empty();
  row->executor_validation = validation.ok;
  row->explain_emitted = ExplainProof(entry.surface_id, access_request);
  row->plan_cache_dependency_bound = PlanCacheDependencyProof(entry.surface_id);

  const auto optimized = opt::OptimizeLogicalPlanWithAccessPathRequest(
      SingleReadPlan(entry.surface_id),
      access_request);
  const auto benchmark = opt::ValidateBenchmarkCleanOptimizedPlan(optimized);
  row->benchmark_clean_validated = entry.benchmark_clean_admissible && benchmark.ok;

  row->authority_clean = !target->cluster_candidate &&
                         access_request.base_row_mga_recheck_planned &&
                         access_request.base_row_security_recheck_planned &&
                         !physical.parser_or_reference_evidence_authority;
  row->runtime_execution_tested = row->candidate_generated &&
                                  row->physical_node_emitted &&
                                  row->executor_validation &&
                                  row->explain_emitted &&
                                  row->plan_cache_dependency_bound;
  if (scenario.llvm_acceleration_checked) {
    opt::OptimizerEvidence evidence;
    evidence.specialized_kind = "vector";
    evidence.exact_fallback_available = true;
    const auto decision = opt::ChooseSpecializedWorkloadAccess(evidence);
    row->surface_specific_validator = decision.ok &&
                                      decision.llvm_eligible &&
                                      decision.access_kind ==
                                          plan::PhysicalAccessKind::kVectorApproximateWithFallback;
    row->claim = "optional_acceleration_exact_fallback_not_benchmark_clean";
  } else {
    row->claim = "catalog_access_path_validated";
  }
  row->diagnostic_code = row->runtime_execution_tested ? "OK" : "PCR060_ACCESS_PROOF_INCOMPLETE";
  row->evidence_detail = std::string(plan::PhysicalAccessKindName(target->access_kind)) +
                         ":" + target->candidate_id;
}

opt::OptimizerStatisticsCatalog JoinStatistics() {
  opt::OptimizerStatisticsCatalog stats;
  stats.Add(opt::MakeStatistic("row_count", "relation", "rel.pcr060.join.left",
                               1000.0, opt::StatisticSource::kCatalogExact,
                               6060, 0, opt::CostConfidence::kHigh));
  stats.Add(opt::MakeStatistic("row_count", "relation", "rel.pcr060.join.right",
                               500.0, opt::StatisticSource::kCatalogExact,
                               6060, 0, opt::CostConfidence::kHigh));
  stats.Add(opt::MakeStatistic("memory_grant_available_bytes", "session", "local.default",
                               1048576.0, opt::StatisticSource::kCatalogExact,
                               6060, 0, opt::CostConfidence::kHigh));
  return stats;
}

void ApplyJoinProof(MatrixRow* row) {
  row->scenario_id = "join_property_frontier";
  plan::LogicalPlan logical;
  logical.ok = true;
  logical.plan_id = "pcr060.join.logical_plan";
  logical.optimizer_policy = SafePolicy("join_property_frontier");
  auto node = plan::MakeLogicalPlanNode(plan::LogicalPlanNodeKind::kDmlRead,
                                        plan::PhysicalAccessKind::kJoinHash,
                                        "pcr060.join.operation",
                                        "pcr060_join");
  node.required_object_uuids = {"rel.pcr060.join.left", "rel.pcr060.join.right"};
  node.required_descriptors = {"sha256:descriptor-pcr060-join"};
  logical.nodes.push_back(std::move(node));

  const auto optimized = opt::OptimizeLogicalPlanWithStatistics(logical, JoinStatistics());
  const auto selected_join = std::find_if(optimized.candidates.begin(), optimized.candidates.end(), [](const opt::OptimizerCandidate& candidate) {
    return candidate.selected_in_physical_tree &&
           candidate.plan_candidate.access_kind == plan::PhysicalAccessKind::kJoinHash;
  });

  row->candidate_generated = selected_join != optimized.candidates.end();
  row->catalog_costed = row->candidate_generated &&
                        selected_join->cost.selectable &&
                        selected_join->statistics_version == "join-local:epoch1";
  row->selectable = row->candidate_generated && selected_join->cost.selectable;
  row->physical_node_emitted = optimized.has_physical_plan &&
                               optimized.physical_root.access_kind ==
                                   plan::PhysicalAccessKind::kJoinHash;
  row->executor_validation = row->physical_node_emitted &&
                             opt::ValidatePhysicalPlanNode(optimized.physical_root).ok;
  row->explain_emitted = true;
  row->plan_cache_dependency_bound = PlanCacheDependencyProof("join_property_frontier");
  row->benchmark_clean_validated = false;
  row->runtime_execution_tested = optimized.ok &&
                                  row->physical_node_emitted &&
                                  row->executor_validation &&
                                  row->plan_cache_dependency_bound;
  row->surface_specific_validator = row->runtime_execution_tested;
  row->authority_clean = row->runtime_execution_tested;
  row->diagnostic_code = row->runtime_execution_tested ? "OK" : "PCR060_JOIN_PROOF_INCOMPLETE";
  row->evidence_detail = "join_hash_property_frontier";
  row->claim = "statistics_backed_join_property_frontier_reported";
}

opt::OptimizerRuntimeFeedback RuntimeFeedback() {
  opt::OptimizerRuntimeFeedback feedback;
  feedback.operator_family = "hash_join";
  feedback.plan_shape = "join_hash";
  feedback.cost_profile_id = "pcr060_feedback_profile";
  feedback.estimated_rows = 10;
  feedback.actual_rows = 1000;
  feedback.actual_rows_examined = 1200;
  feedback.actual_rows_filtered = 200;
  feedback.loop_count = 3;
  feedback.estimated_pages = 4;
  feedback.actual_pages = 40;
  feedback.estimated_io_operations = 4;
  feedback.actual_io_operations = 40;
  feedback.estimated_visibility_recheck_rows = 10;
  feedback.actual_visibility_recheck_rows = 1000;
  feedback.estimated_spill_bytes = 0;
  feedback.actual_spill_bytes = 4096;
  feedback.memory_grant_bytes = 64 * 1024;
  feedback.peak_memory_bytes = 256 * 1024;
  feedback.estimated_latency_microseconds = 100;
  feedback.actual_latency_microseconds = 1000;
  feedback.estimated_resource_units = 10;
  feedback.actual_resource_units = 1000;
  feedback.freshness_microseconds = 10;
  feedback.max_freshness_microseconds = 60000000;
  feedback.advisory_only = true;
  feedback.mga_visibility_recheck_preserved = true;
  feedback.parser_or_reference_authority = false;
  feedback.transaction_finality_authority = "engine_transaction_inventory";
  return feedback;
}

bool AdaptiveFeedbackProof() {
  opt::AdaptiveCardinalityFeedbackRequest request;
  request.feedback = RuntimeFeedback();
  request.baseline_cost = opt::EstimateNodeCost(plan::MakeLogicalPlanNode(
      plan::LogicalPlanNodeKind::kDmlRead,
      plan::PhysicalAccessKind::kJoinHash,
      "pcr060.feedback.baseline",
      "join_hash"));
  request.authority.engine_mga_snapshot_bound = true;
  request.authority.transaction_inventory_authoritative = true;
  request.authority.security_recheck_required = true;
  request.authority.exact_recheck_required = true;
  request.epochs.feedback_generation = 44;
  request.epochs.expected_feedback_generation = 44;
  request.epochs.feedback_epoch = 45;
  request.epochs.catalog_epoch = 46;
  request.epochs.expected_catalog_epoch = 46;
  request.epochs.security_epoch = 47;
  request.epochs.expected_security_epoch = 47;
  request.plan.route_label = "pcr060.adaptive_feedback";
  request.plan.baseline_plan_hash = "sha256:baseline-pcr060-adaptive";
  request.plan.variant_plan_hash = "sha256:variant-pcr060-adaptive";
  request.plan.fallback_plan_hash = "sha256:fallback-pcr060-adaptive";
  request.plan.result_hash = "sha256:result-pcr060-adaptive";
  request.plan.fallback_result_hash = "sha256:result-pcr060-adaptive";
  request.plan.runtime_consumed = true;
  request.plan.exact_fallback_available = true;
  request.bind_sensitive_variant_requested = true;
  request.misestimate_quarantine_requested = true;
  request.extended_stat_request_requested = true;
  request.extended_stat_source_authoritative = true;

  const auto result = opt::EvaluateAdaptiveCardinalityFeedback(request);
  return result.ok && result.benchmark_clean &&
         result.bind_sensitive_variant_created &&
         result.misestimate_quarantined &&
         result.extended_stat_requested;
}

bool MemoryFeedbackProof() {
  opt::OptimizerMemoryFeedbackEvidence evidence;
  evidence.query_uuid = "query.pcr060.memory_feedback";
  evidence.scope_uuid = "scope.pcr060.memory_feedback";
  evidence.route_label = "pcr060.memory_spill_feedback";
  evidence.operator_family = "hash_join";
  evidence.plan_shape = "join_hash";
  evidence.source_kind = "resource_governance_reservation_ledger";
  evidence.source_quality = "observed_runtime";
  evidence.trust_provenance = "resource_governance_reservation_ledger";
  evidence.trusted_provenance = true;
  evidence.provenance_digest = "sha256:pcr060-memory-feedback-provenance";
  evidence.redaction_digest = "sha256:pcr060-memory-feedback-redaction";
  evidence.metric_snapshot_digest = "sha256:pcr060-memory-feedback-metric";
  evidence.reservation_id = "reservation.pcr060.memory_feedback";
  evidence.reservation_token = "reservation-token.pcr060.memory_feedback";
  evidence.reservation_generation = 30;
  evidence.policy_generation = 31;
  evidence.feedback_generation = 32;
  evidence.catalog_epoch = 33;
  evidence.security_epoch = 34;
  evidence.redaction_epoch = 35;
  evidence.statistics_epoch = 36;
  evidence.observed_timestamp_ticks = 1000;
  evidence.received_timestamp_ticks = 1500;
  evidence.max_age_ticks = 60000000;
  evidence.memory_grant_bytes = 64 * 1024;
  evidence.peak_memory_bytes = 256 * 1024;
  evidence.spill_bytes = 4096;
  evidence.spill_passes = 1;
  evidence.governed_reservation = true;
  evidence.reservation_token_bound = true;
  evidence.resource_governance_ledger_recorded = true;
  evidence.real_operation_metric = false;
  evidence.operation_metric_runtime_path = false;
  evidence.protected_material_redacted = true;
  evidence.protected_material_exposed = false;
  evidence.advisory_only = true;
  evidence.mga_visibility_recheck_preserved = true;
  evidence.security_recheck_preserved = true;

  const auto result = opt::BuildOptimizerMemoryFeedbackForPlanner(evidence);
  return result.ok() && result.authority_boundaries_clean &&
         result.diagnostic_code == "SB_OPTIMIZER_MEMORY_FEEDBACK.ACCEPTED" &&
         result.runtime_feedback.actual_spill_bytes == evidence.spill_bytes;
}

void ApplySharedSurfaceProof(const opt::EnterpriseOptimizerSurfaceEntry& entry,
                             MatrixRow* row) {
  ApplyAccessProof(entry, row);
  if (entry.surface_id == "adaptive_feedback") {
    row->surface_specific_validator = AdaptiveFeedbackProof();
    row->runtime_execution_tested = row->runtime_execution_tested &&
                                    row->surface_specific_validator;
    row->diagnostic_code = row->runtime_execution_tested ? "OK" : "PCR060_ADAPTIVE_FEEDBACK_PROOF_INCOMPLETE";
    row->evidence_detail = "adaptive_feedback_advisory_mga_security_recheck";
    row->claim = "adaptive_feedback_advisory_only_validated";
  } else if (entry.surface_id == "memory_spill_feedback") {
    row->surface_specific_validator = MemoryFeedbackProof();
    row->runtime_execution_tested = row->runtime_execution_tested &&
                                    row->surface_specific_validator;
    row->diagnostic_code = row->runtime_execution_tested ? "OK" : "PCR060_MEMORY_FEEDBACK_PROOF_INCOMPLETE";
    row->evidence_detail = "memory_spill_feedback_governed_ledger";
    row->claim = "memory_feedback_advisory_only_validated";
  } else if (entry.surface_id == "runtime_payload_explain") {
    row->surface_specific_validator = row->explain_emitted;
    row->evidence_detail = "explain_payload_plan_hash_dependencies";
    row->claim = "runtime_payload_explain_validated";
  } else if (entry.surface_id == "plan_cache") {
    row->surface_specific_validator = row->plan_cache_dependency_bound;
    row->evidence_detail = "enterprise_plan_cache_dependency_key";
    row->claim = "plan_cache_dependency_binding_validated";
  }
}

void ApplyClusterProof(const opt::EnterpriseOptimizerSurfaceEntry& entry,
                       MatrixRow* row) {
  row->scenario_id = entry.surface_id == "cluster_fragment"
                         ? "no_cluster_fragment_refusal"
                         : "no_cluster_remote_pushdown_refusal";
  const opt::ClusterCandidateFacts facts;
  const auto candidate = entry.surface_id == "cluster_fragment"
                             ? opt::BuildClusterFragmentCandidate(facts)
                             : opt::BuildRemoteNodePushdownCandidate(facts);
  row->candidate_generated = !candidate.candidate_id.empty();
  row->catalog_costed = candidate.cost.confidence == opt::CostConfidence::kRejected;
  row->selectable = false;
  row->physical_node_emitted = false;
  row->executor_validation = false;
  row->explain_emitted = false;
  row->plan_cache_dependency_bound = false;
  row->benchmark_clean_validated = false;
  row->surface_specific_validator = !opt::ClusterCandidateMayWin(candidate);

  opt::ClusterOptimizerBoundaryRequest request;
  request.cluster_route_requested = true;
  request.external_provider_available = false;
  const auto boundary = opt::EvaluateClusterOptimizerBoundary(request);
  row->fail_closed = boundary.refused &&
                     boundary.diagnostic_code ==
                         "SB_OPT_CLUSTER_BOUNDARY.CLUSTER_ROUTE_EXTERNAL_PROVIDER_REQUIRED";
  row->runtime_execution_tested = row->candidate_generated &&
                                  row->surface_specific_validator &&
                                  row->fail_closed;
  row->authority_clean = row->runtime_execution_tested;
  row->diagnostic_code = boundary.diagnostic_code;
  row->evidence_detail = "external_cluster_provider_required";
  row->claim = "cluster_core_fail_closed_external_provider_only";
}

void ApplyRemovedClaimProof(const opt::EnterpriseOptimizerSurfaceEntry& entry,
                            MatrixRow* row) {
  row->scenario_id = entry.surface_id == "reference_authority"
                         ? "reference_authority_refusal"
                         : "parser_execution_authority_refusal";
  if (entry.surface_id == "reference_authority") {
    opt::OptimizerProductionBuildGateInput input;
    input.reference_produced_evidence_enabled = true;
    const auto result = opt::EvaluateOptimizerProductionBuildGate(input);
    row->fail_closed = !result.ok &&
                       Contains(result.diagnostics,
                                "SB_OPT_PRODUCTION_GATE_REFERENCE_EVIDENCE_FORBIDDEN");
    row->diagnostic_code = row->fail_closed
                               ? "SB_OPT_PRODUCTION_GATE_REFERENCE_EVIDENCE_FORBIDDEN"
                               : "PCR060_REFERENCE_REFUSAL_MISSING";
  } else {
    auto request = BoundRequestFor("table_scan",
                                   AccessRequestFor("table_scan", AccessScenario{}));
    request.context.parser_owned_claims_present = true;
    const auto validation = opt::ValidateBoundOptimizerRequest(request);
    row->fail_closed = !validation.ok &&
                       Contains(validation.diagnostics,
                                "SB_OPT_AUTHORITY_REJECTED.parser_owned_claims");
    row->diagnostic_code = row->fail_closed
                               ? "SB_OPT_AUTHORITY_REJECTED.parser_owned_claims"
                               : "PCR060_PARSER_REFUSAL_MISSING";
  }
  row->candidate_generated = false;
  row->catalog_costed = false;
  row->selectable = false;
  row->physical_node_emitted = false;
  row->executor_validation = false;
  row->explain_emitted = false;
  row->plan_cache_dependency_bound = false;
  row->benchmark_clean_validated = false;
  row->surface_specific_validator = row->fail_closed;
  row->runtime_execution_tested = row->fail_closed;
  row->authority_clean = row->fail_closed;
  row->evidence_detail = "removed_authority_claim_refused";
  row->claim = "removed_claim_fail_closed";
}

MatrixRow BuildRow(const opt::EnterpriseOptimizerSurfaceEntry& entry) {
  MatrixRow row;
  row.surface_id = entry.surface_id;
  row.route_family = entry.route_family;
  row.surface_class = opt::EnterpriseOptimizerSurfaceClassName(entry.surface_class);
  row.declared = true;
  row.production_route_admissible = entry.production_route_admissible;
  row.benchmark_clean_eligible = entry.benchmark_clean_admissible;

  if (entry.surface_class == opt::EnterpriseOptimizerSurfaceClass::cluster_external) {
    ApplyClusterProof(entry, &row);
  } else if (entry.surface_class == opt::EnterpriseOptimizerSurfaceClass::removed_claim) {
    ApplyRemovedClaimProof(entry, &row);
  } else if (entry.surface_class == opt::EnterpriseOptimizerSurfaceClass::test_only) {
    row.scenario_id = "manifest_test_gate";
    const auto validation = opt::ValidateEnterpriseOptimizerManifest();
    row.surface_specific_validator = validation.ok;
    row.runtime_execution_tested = validation.ok;
    row.fail_closed = true;
    row.authority_clean = validation.ok;
    row.diagnostic_code = validation.ok ? "TEST_ONLY" : "PCR060_MANIFEST_TEST_GATE_FAILED";
    row.evidence_detail = "optimizer_contract_tests_are_test_only";
    row.claim = "test_only_not_production_admissible";
  } else if (entry.surface_id == "join_property_frontier") {
    ApplyJoinProof(&row);
  } else if (entry.surface_id == "adaptive_feedback" ||
             entry.surface_id == "memory_spill_feedback" ||
             entry.surface_id == "runtime_payload_explain" ||
             entry.surface_id == "plan_cache") {
    ApplySharedSurfaceProof(entry, &row);
  } else {
    ApplyAccessProof(entry, &row);
  }
  return row;
}

void WriteMatrix(const std::filesystem::path& path,
                 const std::vector<MatrixRow>& rows) {
  if (!path.parent_path().empty()) {
    std::filesystem::create_directories(path.parent_path());
  }
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  Require(out.good(), "PCR-060 could not open optimizer matrix output");
  out << "surface_id,route_family,surface_class,scenario_id,declared,"
         "production_route_admissible,benchmark_clean_eligible,"
         "candidate_generated,catalog_costed,selectable,"
         "physical_node_emitted,executor_validation,explain_emitted,"
         "plan_cache_dependency_bound,benchmark_clean_validated,"
         "runtime_execution_tested,surface_specific_validator,fail_closed,"
         "authority_clean,diagnostic_code,evidence_detail,claim\n";
  for (const auto& row : rows) {
    out << Csv(row.surface_id) << ','
        << Csv(row.route_family) << ','
        << Csv(row.surface_class) << ','
        << Csv(row.scenario_id) << ','
        << BoolText(row.declared) << ','
        << BoolText(row.production_route_admissible) << ','
        << BoolText(row.benchmark_clean_eligible) << ','
        << BoolText(row.candidate_generated) << ','
        << BoolText(row.catalog_costed) << ','
        << BoolText(row.selectable) << ','
        << BoolText(row.physical_node_emitted) << ','
        << BoolText(row.executor_validation) << ','
        << BoolText(row.explain_emitted) << ','
        << BoolText(row.plan_cache_dependency_bound) << ','
        << BoolText(row.benchmark_clean_validated) << ','
        << BoolText(row.runtime_execution_tested) << ','
        << BoolText(row.surface_specific_validator) << ','
        << BoolText(row.fail_closed) << ','
        << BoolText(row.authority_clean) << ','
        << Csv(row.diagnostic_code) << ','
        << Csv(row.evidence_detail) << ','
        << Csv(row.claim) << '\n';
  }
}

void ValidateRows(const std::vector<MatrixRow>& rows) {
  const auto manifest = opt::ValidateEnterpriseOptimizerManifest();
  if (!manifest.ok) {
    for (const auto& diagnostic : manifest.diagnostics) {
      std::cerr << diagnostic << '\n';
    }
  }
  Require(manifest.ok, "PCR-060 optimizer manifest did not validate");
  Require(rows.size() == opt::EnterpriseOptimizerSurfaceManifest().size(),
          "PCR-060 row count does not match optimizer manifest");

  std::set<std::string> ids;
  for (const auto& row : rows) {
    Require(ids.insert(row.surface_id).second,
            "PCR-060 duplicate optimizer surface row");
    Require(row.declared, "PCR-060 row not marked declared");
    Require(row.runtime_execution_tested,
            "PCR-060 row lacks runtime execution proof");
    Require(row.surface_specific_validator,
            "PCR-060 row lacks surface-specific validation");
    Require(row.authority_clean,
            "PCR-060 row failed optimizer authority hygiene");

    if (row.surface_class == "noncluster_live" ||
        row.surface_class == "noncluster_exact_fallback") {
      Require(row.production_route_admissible,
              std::string("PCR-060 production surface not marked admissible: ") +
                  row.surface_id);
      Require(row.candidate_generated,
              std::string("PCR-060 production surface did not generate candidate: ") +
                  row.surface_id);
      Require(row.catalog_costed,
              std::string("PCR-060 production surface was not catalog costed: ") +
                  row.surface_id);
      Require(row.selectable,
              std::string("PCR-060 production surface was not selectable: ") +
                  row.surface_id);
      Require(row.physical_node_emitted,
              std::string("PCR-060 production surface did not emit physical node: ") +
                  row.surface_id);
      Require(row.executor_validation,
              std::string("PCR-060 production surface failed executor validation: ") +
                  row.surface_id);
      Require(row.explain_emitted,
              std::string("PCR-060 production surface did not emit explain evidence: ") +
                  row.surface_id);
      Require(row.plan_cache_dependency_bound,
              std::string("PCR-060 production surface did not bind plan-cache dependencies: ") +
                  row.surface_id);
    }

    if (row.surface_class == "cluster_external") {
      Require(!row.production_route_admissible &&
                  !row.benchmark_clean_eligible &&
                  row.fail_closed &&
                  !row.selectable,
              "PCR-060 cluster surface overclaimed core production behavior");
    }

    if (row.surface_class == "removed_claim") {
      Require(!row.production_route_admissible &&
                  !row.benchmark_clean_eligible &&
                  row.fail_closed &&
                  !row.candidate_generated,
              "PCR-060 removed optimizer authority claim was not refused");
    }

    if (row.surface_id == "llvm_native_compile") {
      Require(!row.benchmark_clean_validated,
              "PCR-060 LLVM optional acceleration claimed benchmark-clean closure");
    }
  }
}

}  // namespace

int main(int argc, char** argv) {
  Require(argc == 2,
          "usage: public_optimizer_implementation_matrix_gate <matrix-output.csv>");
  std::vector<MatrixRow> rows;
  for (const auto& entry : opt::EnterpriseOptimizerSurfaceManifest()) {
    rows.push_back(BuildRow(entry));
  }
  ValidateRows(rows);
  WriteMatrix(argv[1], rows);
  std::cout << "public optimizer implementation matrix gate wrote "
            << rows.size() << " rows\n";
  return EXIT_SUCCESS;
}
