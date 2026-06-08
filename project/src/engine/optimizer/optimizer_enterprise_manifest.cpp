// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "optimizer_enterprise_manifest.hpp"

#include <set>
#include <string_view>

namespace scratchbird::engine::optimizer {
namespace {

bool Contains(std::string_view value, std::string_view token) {
  return value.find(token) != std::string_view::npos;
}

bool HasSurface(const std::vector<EnterpriseOptimizerSurfaceEntry>& entries,
                std::string_view surface_id) {
  for (const auto& entry : entries) {
    if (entry.surface_id == surface_id) {
      return true;
    }
  }
  return false;
}

}  // namespace

const char* EnterpriseOptimizerSurfaceClassName(EnterpriseOptimizerSurfaceClass value) {
  switch (value) {
    case EnterpriseOptimizerSurfaceClass::noncluster_live: return "noncluster_live";
    case EnterpriseOptimizerSurfaceClass::noncluster_exact_fallback: return "noncluster_exact_fallback";
    case EnterpriseOptimizerSurfaceClass::test_only: return "test_only";
    case EnterpriseOptimizerSurfaceClass::cluster_external: return "cluster_external";
    case EnterpriseOptimizerSurfaceClass::removed_claim: return "removed_claim";
  }
  return "removed_claim";
}

const std::vector<LegacyOptimizerCapabilityClosure>&
LegacyOptimizerCapabilityClosures() {
  // SEARCH_KEY: OEIC_LEGACY_CAPABILITY_MAPPING
  static const std::vector<LegacyOptimizerCapabilityClosure> closures = {
      {"legacy.statement_request", "statement-level planning request richness",
       "mapped_to_sblr_api_request_context", "optimizer_request.*;logical_plan.*", "mapped"},
      {"legacy.session_controls", "optimizer session controls and directives",
       "mapped_to_parser_safe_optimizer_policy", "optimizer_request.*;logical_plan.*", "mapped"},
      {"legacy.join_frontier", "join frontier retention",
       "implemented_in_property_frontier_join_memo", "join_planner_full.*", "implemented"},
      {"legacy.join_strategies", "exhaustive bounded hypergraph heuristic and input-order strategies",
       "mapped_to_policy_scoped_join_strategy_surface", "join_planner_full.*", "implemented"},
      {"legacy.ordering_window", "ordering grouping and window satisfaction",
       "implemented_with_sblr_logical_property_metadata", "logical_plan.*;relational_planner.*", "implemented"},
      {"legacy.statistics_depth", "table column index histogram MCV HLL expression and multivariate stats",
       "mapped_to_enterprise_statistics_lifecycle", "optimizer_statistics_full.*;optimizer_production_analyze.*", "mapped"},
      {"legacy.index_family_metrics", "index-family route and maintenance metrics",
       "mapped_to_oeic_metric_manifest", "optimizer_enterprise_manifest.*", "mapped"},
      {"legacy.adaptive_cardinality", "adaptive cardinality and selectivity feedback",
       "implemented_as_governed_advisory_feedback", "adaptive_cardinality_feedback.*;optimizer_feedback.*", "implemented"},
      {"legacy.memory_feedback", "memory grant and spill feedback",
       "implemented_as_governed_memory_feedback_bridge", "optimizer_memory_feedback_bridge.*", "implemented"},
      {"legacy.runtime_payload", "runtime plan payload and explain diagnostics",
       "implemented_with_runtime_consumption_and_explain_payload", "optimizer_explain.*;runtime_consumption_evidence.*", "implemented"},
      {"legacy.plan_cache", "plan cache reuse metadata and invalidation",
       "implemented_with_generation_aware_plan_cache", "optimizer_plan_cache.*", "implemented"},
      {"legacy.mga_pressure", "dynamic MGA pressure costing",
       "mapped_to_mga_pressure_metric_closure", "mga_*_evidence.*;optimizer_cost_full.*", "mapped"},
      {"legacy.route_labels", "route labels result hashes and route equivalence",
       "mapped_to_runtime_evidence_and_live_route_gates", "runtime_consumption_evidence.*", "mapped"},
      {"legacy.donor_comparison", "donor comparison and oracle evidence",
       "reference_only_no_authority", "runtime_consumption_evidence.*", "mapped"},
      {"legacy.cluster_costing", "cluster route costing and cluster metrics",
       "cluster_external_provider_only", "cluster_candidate.*;cluster_refusal_path.*", "cluster_external"},
      {"legacy.llvm_native", "LLVM/native compilation acceleration",
       "optional_acceleration_no_correctness_authority", "native_compile.*;llvm_api.*", "mapped"},
  };
  return closures;
}

const std::vector<EnterpriseOptimizerSurfaceEntry>&
EnterpriseOptimizerSurfaceManifest() {
  static const std::vector<EnterpriseOptimizerSurfaceEntry> entries = {
      {"table_scan", "relational_scan", EnterpriseOptimizerSurfaceClass::noncluster_live,
       "access_path.*;optimizer_cost_full.*", "OK", true, true, true},
      {"row_uuid_lookup", "relational_lookup", EnterpriseOptimizerSurfaceClass::noncluster_live,
       "access_path.*;optimizer_hot_point_lookup.*", "OK", true, true, true},
      {"btree_point_range", "index_btree", EnterpriseOptimizerSurfaceClass::noncluster_live,
       "access_path_full.*;index_optimizer_integration.cpp", "OK", true, true, true},
      {"hash_equality", "index_hash", EnterpriseOptimizerSurfaceClass::noncluster_live,
       "access_path_full.*;index_optimizer_integration.cpp", "OK", true, true, true},
      {"bitmap_candidate_set", "candidate_set", EnterpriseOptimizerSurfaceClass::noncluster_exact_fallback,
       "access_path_full.*;runtime_filter_pushdown.*", "SB_OPT_BITMAP_RECHECK_REQUIRED", true, true, true},
      {"zone_summary_prune", "summary_prune", EnterpriseOptimizerSurfaceClass::noncluster_exact_fallback,
       "partition_segment_pruning.*;access_path_full.*", "SB_OPT_SUMMARY_RECHECK_REQUIRED", true, true, true},
      {"covering_index", "index_covering", EnterpriseOptimizerSurfaceClass::noncluster_exact_fallback,
       "access_path_full.*", "SB_OPT_COVERING_RECHECK_OR_PAYLOAD_PROOF_REQUIRED", true, true, true},
      {"text_search", "search_text", EnterpriseOptimizerSurfaceClass::noncluster_exact_fallback,
       "specialized_planner.*", "SB_OPT_SEARCH_EXACT_RERANK_REQUIRED", true, true, true},
      {"vector_ann", "search_vector", EnterpriseOptimizerSurfaceClass::noncluster_exact_fallback,
       "specialized_planner.*", "SB_OPT_VECTOR_EXACT_RERANK_REQUIRED", true, true, true},
      {"document_path", "nosql_document", EnterpriseOptimizerSurfaceClass::noncluster_exact_fallback,
       "specialized_planner.*;nosql_statistics_advisor.*", "SB_OPT_DOCUMENT_EXACT_RECHECK_REQUIRED", true, true, true},
      {"graph_seed", "nosql_graph", EnterpriseOptimizerSurfaceClass::noncluster_exact_fallback,
       "specialized_planner.*", "SB_OPT_GRAPH_FRONTIER_RECHECK_REQUIRED", true, true, true},
      {"time_series_append", "time_series", EnterpriseOptimizerSurfaceClass::noncluster_exact_fallback,
       "specialized_planner.*;relational_planner.*", "SB_OPT_TIMESERIES_EXACT_ROUTE_REQUIRED", true, true, true},
      {"temporary_in_memory", "temp_memory", EnterpriseOptimizerSurfaceClass::noncluster_live,
       "relational_planner.*;optimizer_typed_arena_work_area.*", "OK", true, true, true},
      {"join_property_frontier", "join", EnterpriseOptimizerSurfaceClass::noncluster_live,
       "join_planner_full.*", "OK", true, true, true},
      {"adaptive_feedback", "feedback", EnterpriseOptimizerSurfaceClass::noncluster_live,
       "adaptive_cardinality_feedback.*;optimizer_feedback.*", "OK", true, true, true},
      {"memory_spill_feedback", "feedback_memory", EnterpriseOptimizerSurfaceClass::noncluster_live,
       "optimizer_memory_feedback_bridge.*", "OK", true, true, true},
      {"runtime_payload_explain", "observability", EnterpriseOptimizerSurfaceClass::noncluster_live,
       "optimizer_explain.*;runtime_consumption_evidence.*", "OK", true, true, true},
      {"plan_cache", "plan_cache", EnterpriseOptimizerSurfaceClass::noncluster_live,
       "optimizer_plan_cache.*", "OK", true, true, true},
      {"llvm_native_compile", "native_compile", EnterpriseOptimizerSurfaceClass::noncluster_exact_fallback,
       "native_compile.*;llvm_api.*", "SB_OPT_LLVM_UNAVAILABLE_OR_REFUSED", true, false, true},
      {"optimizer_contract_tests", "test_gate", EnterpriseOptimizerSurfaceClass::test_only,
       "project/tests/optimizer;project/tests/performance", "TEST_ONLY", false, false, false},
      {"cluster_fragment", "cluster_route", EnterpriseOptimizerSurfaceClass::cluster_external,
       "cluster_candidate.*;cluster_refusal_path.*", "SB_OPT_CLUSTER_EXTERNAL_PROVIDER_REQUIRED", false, false, false},
      {"remote_pushdown", "cluster_route", EnterpriseOptimizerSurfaceClass::cluster_external,
       "cluster_candidate.*;cluster_refusal_path.*", "SB_OPT_CLUSTER_EXTERNAL_PROVIDER_REQUIRED", false, false, false},
      {"donor_authority", "removed_claim", EnterpriseOptimizerSurfaceClass::removed_claim,
       "runtime_consumption_evidence.*", "SB_OPT_DONOR_AUTHORITY_FORBIDDEN", false, false, false},
      {"parser_execution_authority", "removed_claim", EnterpriseOptimizerSurfaceClass::removed_claim,
       "optimizer_request.*", "SB_OPT_PARSER_EXECUTION_AUTHORITY_FORBIDDEN", false, false, false},
  };
  return entries;
}

EnterpriseOptimizerManifestValidation ValidateEnterpriseOptimizerManifest() {
  EnterpriseOptimizerManifestValidation validation;
  const auto& closures = LegacyOptimizerCapabilityClosures();
  const auto& entries = EnterpriseOptimizerSurfaceManifest();
  std::set<std::string> capability_ids;
  std::set<std::string> surface_ids;

  for (const auto& closure : closures) {
    if (closure.capability_id.empty()) {
      validation.diagnostics.push_back("OEIC.MANIFEST.LEGACY_CAPABILITY_ID_MISSING");
    }
    if (!capability_ids.insert(closure.capability_id).second) {
      validation.diagnostics.push_back("OEIC.MANIFEST.LEGACY_CAPABILITY_DUPLICATE:" + closure.capability_id);
    }
    if (closure.enterprise_outcome.empty() || closure.private_anchor.empty() || closure.closure_status.empty()) {
      validation.diagnostics.push_back("OEIC.MANIFEST.LEGACY_CAPABILITY_INCOMPLETE:" + closure.capability_id);
    }
    if (Contains(closure.closure_status, "pending") || Contains(closure.closure_status, "unknown")) {
      validation.diagnostics.push_back("OEIC.MANIFEST.LEGACY_CAPABILITY_UNMAPPED:" + closure.capability_id);
    }
  }

  for (const auto& entry : entries) {
    if (entry.surface_id.empty()) {
      validation.diagnostics.push_back("OEIC.MANIFEST.SURFACE_ID_MISSING");
    }
    if (!surface_ids.insert(entry.surface_id).second) {
      validation.diagnostics.push_back("OEIC.MANIFEST.SURFACE_DUPLICATE:" + entry.surface_id);
    }
    if (entry.implementation_anchor.empty() || entry.diagnostic_code.empty()) {
      validation.diagnostics.push_back("OEIC.MANIFEST.SURFACE_INCOMPLETE:" + entry.surface_id);
    }
    if (entry.surface_class == EnterpriseOptimizerSurfaceClass::cluster_external) {
      if (entry.production_route_admissible || entry.benchmark_clean_admissible) {
        validation.diagnostics.push_back("OEIC.MANIFEST.CLUSTER_SURFACE_ADMISSIBLE:" + entry.surface_id);
      }
    }
    if (entry.surface_class == EnterpriseOptimizerSurfaceClass::test_only &&
        (entry.production_route_admissible || entry.benchmark_clean_admissible)) {
      validation.diagnostics.push_back("OEIC.MANIFEST.TEST_ONLY_SURFACE_ADMISSIBLE:" + entry.surface_id);
    }
    if (entry.surface_class == EnterpriseOptimizerSurfaceClass::removed_claim &&
        (entry.production_route_admissible || entry.benchmark_clean_admissible)) {
      validation.diagnostics.push_back("OEIC.MANIFEST.REMOVED_CLAIM_ADMISSIBLE:" + entry.surface_id);
    }
    if (entry.benchmark_clean_admissible && !entry.requires_real_metric_producer) {
      validation.diagnostics.push_back("OEIC.MANIFEST.BENCHMARK_CLEAN_WITHOUT_METRIC_PRODUCER:" + entry.surface_id);
    }
  }

  const std::vector<std::string_view> required = {
      "table_scan", "btree_point_range", "hash_equality", "bitmap_candidate_set",
      "covering_index", "text_search", "vector_ann", "document_path",
      "graph_seed", "join_property_frontier", "adaptive_feedback",
      "memory_spill_feedback", "runtime_payload_explain", "plan_cache",
      "llvm_native_compile", "cluster_fragment", "remote_pushdown",
      "donor_authority", "parser_execution_authority"};
  for (const auto required_surface : required) {
    if (!HasSurface(entries, required_surface)) {
      validation.diagnostics.push_back(std::string("OEIC.MANIFEST.REQUIRED_SURFACE_MISSING:") +
                                       std::string(required_surface));
    }
  }

  validation.ok = validation.diagnostics.empty();
  return validation;
}

}  // namespace scratchbird::engine::optimizer
