// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// CEIC-060 focused validation for index readiness coupling in optimizer plan admission.
#include "index_optimizer_integration.hpp"
#include "optimizer_index_costing.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace index = scratchbird::core::index;
namespace opt = scratchbird::engine::optimizer;
using scratchbird::core::platform::TypedUuid;
using scratchbird::core::platform::UuidKind;
using scratchbird::core::platform::byte;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

bool HasEvidence(const std::vector<std::string>& evidence,
                 std::string_view token) {
  for (const auto& row : evidence) {
    if (row.find(token) != std::string::npos) {
      return true;
    }
  }
  return false;
}

TypedUuid TestUuid(UuidKind kind, unsigned seed) {
  TypedUuid value;
  value.kind = kind;
  for (std::size_t i = 0; i < value.value.bytes.size(); ++i) {
    value.value.bytes[i] =
        static_cast<byte>((seed * 43u + i * 19u + 0x61u) & 0xffu);
  }
  value.value.bytes[6] =
      static_cast<byte>((value.value.bytes[6] & 0x0fu) | 0x70u);
  value.value.bytes[8] =
      static_cast<byte>((value.value.bytes[8] & 0x3fu) | 0x80u);
  return value;
}

index::IndexReadinessPlanAdmissionEvidence Evidence(
    index::IndexFamily family,
    index::IndexRouteKind route = index::IndexRouteKind::sql_select) {
  const auto* route_state =
      index::FindBuiltinIndexRouteCapabilityState(route, family);
  Require(route_state != nullptr && route_state->route_complete(),
          "CEIC-060 fixture requires a complete runtime route");

  index::IndexReadinessPlanAdmissionEvidence evidence;
  evidence.family = family;
  evidence.route = route;
  evidence.manifest_epoch = 6001;
  evidence.registry_epoch = 6002;
  evidence.route_proof_epoch = 6003;
  evidence.source_evidence_digest = "sha256:ceic060-index-readiness";
  evidence.generated_by =
      "project/tools/ceic_index_readiness_manifest.py#CEIC_030_INDEX_READINESS_MANIFEST_TOOL";
  evidence.generated_manifest_present = true;
  evidence.generated_manifest_current = true;
  evidence.generated_manifest_validated = true;
  evidence.source_digest_matches = true;
  evidence.runtime_registry_family_matches = true;
  evidence.runtime_registry_route_matches = true;
  evidence.runtime_family_available = true;
  evidence.runtime_route_complete = true;
  evidence.supports_read = route_state->supports_read;
  evidence.supports_equality_lookup = route_state->supports_equality_lookup;
  evidence.supports_ordered_range = route_state->supports_ordered_range;
  evidence.supports_negative_prune = route_state->supports_negative_prune;
  evidence.supports_summary_segment_prune =
      route_state->supports_summary_segment_prune;
  evidence.produces_candidate_set = route_state->produces_candidate_set;
  evidence.approximate_candidate_source =
      route_state->approximate_candidate_source;
  evidence.requires_exact_recheck = route_state->requires_exact_recheck;
  evidence.requires_mga_recheck = route_state->requires_mga_recheck;
  evidence.requires_security_recheck = route_state->requires_security_recheck;
  evidence.requires_exact_rerank = route_state->requires_exact_rerank;
  evidence.exact_recheck_proven = true;
  evidence.mga_recheck_proven = true;
  evidence.security_recheck_proven = true;
  evidence.exact_rerank_proven = route_state->requires_exact_rerank;
  evidence.operation_metrics_producer_proven = true;
  evidence.support_bundle_producer_proven = true;
  evidence.crash_reopen_proven = true;
  evidence.corruption_cleanup_proven = true;
  evidence.cleanup_horizon_proven = true;
  evidence.storage_integration_proven = true;
  evidence.external_cluster_provider_only = true;
  return evidence;
}

index::IndexOptimizerRequest Request(
    index::IndexFamily family,
    index::IndexPlanCategory category,
    index::IndexReadinessPlanAdmissionEvidence* evidence,
    index::IndexRouteKind route = index::IndexRouteKind::sql_select) {
  index::IndexOptimizerRequest request;
  request.index_uuid = TestUuid(UuidKind::object, 60);
  request.family = family;
  request.route = route;
  request.category = category;
  request.readiness_evidence = evidence;
  request.selectivity = 0.05;
  request.confidence = 0.98;
  request.stats_available = true;
  request.stats_stale = false;
  request.mga_recheck_required = true;
  request.security_recheck_required = true;
  request.requires_exact_rows = true;
  if (category == index::IndexPlanCategory::point_lookup) {
    request.requires_equality_lookup = true;
  }
  if (category == index::IndexPlanCategory::range_scan) {
    request.requires_range_scan = true;
    request.requires_order = true;
    request.order_proven = true;
  }
  if (category == index::IndexPlanCategory::summary_prune) {
    request.requires_summary_segment_prune = true;
  }
  if (category == index::IndexPlanCategory::bitmap_combine ||
      category == index::IndexPlanCategory::inverted_search ||
      category == index::IndexPlanCategory::spatial_search ||
      category == index::IndexPlanCategory::vector_search ||
      category == index::IndexPlanCategory::graph_search) {
    request.requires_candidate_set = true;
  }
  if (family == index::IndexFamily::vector_hnsw ||
      family == index::IndexFamily::vector_ivf) {
    request.approximate = true;
    request.exact_rerank_available = true;
  }
  return request;
}

void ExpectAccepted(const index::IndexOptimizerRequest& request,
                    std::string_view message) {
  const auto plan = index::PlanIndexOptimizerPath(request);
  Require(plan.ok(), message);
  Require(plan.admitted, "CEIC-060 accepted plan did not mark admitted");
  Require(!plan.fallback_full_scan,
          "CEIC-060 accepted plan unexpectedly selected full scan fallback");
  Require(HasEvidence(plan.steps,
                      "ceic_060_index_readiness_plan_admission=accepted"),
          "CEIC-060 accepted plan missing readiness admission evidence");
  Require(HasEvidence(plan.steps,
                      "index_readiness_route_runtime_proof=true"),
          "CEIC-060 accepted plan missing route runtime proof evidence");
}

void ExpectRejected(index::IndexOptimizerRequest request,
                    std::string_view diagnostic_code,
                    std::string_view message) {
  const auto plan = index::PlanIndexOptimizerPath(request);
  Require(!plan.ok(), message);
  Require(plan.fallback_full_scan,
          "CEIC-060 rejection did not fail closed to full scan");
  Require(plan.diagnostic.diagnostic_code == diagnostic_code,
          std::string("CEIC-060 diagnostic mismatch: expected ") +
              std::string(diagnostic_code) + " got " +
              plan.diagnostic.diagnostic_code);
}

void PositiveCases() {
  auto btree = Evidence(index::IndexFamily::btree);
  ExpectAccepted(Request(index::IndexFamily::btree,
                         index::IndexPlanCategory::point_lookup,
                         &btree),
                 "CEIC-060 rejected B-tree equality route with complete proof");

  auto hash = Evidence(index::IndexFamily::hash);
  ExpectAccepted(Request(index::IndexFamily::hash,
                         index::IndexPlanCategory::point_lookup,
                         &hash),
                 "CEIC-060 rejected hash equality route with complete proof");

  auto vector = Evidence(index::IndexFamily::vector_hnsw,
                         index::IndexRouteKind::nosql_vector);
  ExpectAccepted(Request(index::IndexFamily::vector_hnsw,
                         index::IndexPlanCategory::vector_search,
                         &vector,
                         index::IndexRouteKind::nosql_vector),
                 "CEIC-060 rejected specialized vector route with exact rerank proof");
}

void MissingAndStaleManifestRefusals() {
  auto btree = Evidence(index::IndexFamily::btree);
  auto missing = Request(index::IndexFamily::btree,
                         index::IndexPlanCategory::point_lookup,
                         nullptr);
  ExpectRejected(missing,
                 "INDEX.OPTIMIZER_READINESS_EVIDENCE.MISSING",
                 "CEIC-060 admitted a plan without generated readiness evidence");

  btree.static_registry_only = true;
  ExpectRejected(Request(index::IndexFamily::btree,
                         index::IndexPlanCategory::point_lookup,
                         &btree),
                 "INDEX.OPTIMIZER_READINESS_EVIDENCE.STALE_OR_STATIC",
                 "CEIC-060 admitted static registry-only evidence");

  btree = Evidence(index::IndexFamily::btree);
  btree.smoke_only = true;
  ExpectRejected(Request(index::IndexFamily::btree,
                         index::IndexPlanCategory::point_lookup,
                         &btree),
                 "INDEX.OPTIMIZER_READINESS_EVIDENCE.STALE_OR_STATIC",
                 "CEIC-060 admitted smoke-only evidence");

  btree = Evidence(index::IndexFamily::btree);
  btree.manifest_epoch = 0;
  ExpectRejected(Request(index::IndexFamily::btree,
                         index::IndexPlanCategory::point_lookup,
                         &btree),
                 "INDEX.OPTIMIZER_READINESS_EVIDENCE.STALE_OR_STATIC",
                 "CEIC-060 admitted placeholder epoch evidence");
}

void NonRuntimeMismatchAndRouteSemanticsRefusals() {
  auto evidence = Evidence(index::IndexFamily::btree);
  evidence.reference_emulated = true;
  ExpectRejected(Request(index::IndexFamily::btree,
                         index::IndexPlanCategory::point_lookup,
                         &evidence),
                 "INDEX.OPTIMIZER_READINESS_EVIDENCE.NON_RUNTIME_FAMILY",
                 "CEIC-060 admitted reference-emulated readiness evidence");

  evidence = Evidence(index::IndexFamily::btree);
  evidence.route = index::IndexRouteKind::nosql_vector;
  ExpectRejected(Request(index::IndexFamily::btree,
                         index::IndexPlanCategory::point_lookup,
                         &evidence),
                 "INDEX.OPTIMIZER_READINESS_EVIDENCE.ROUTE_FAMILY_MISMATCH",
                 "CEIC-060 admitted route/family mismatched evidence");

  auto hash = Evidence(index::IndexFamily::hash);
  auto ordered_hash = Request(index::IndexFamily::hash,
                              index::IndexPlanCategory::range_scan,
                              &hash);
  ExpectRejected(ordered_hash,
                 "INDEX.OPTIMIZER_READINESS_EVIDENCE.PLAN_SEMANTIC_MISMATCH",
                 "CEIC-060 admitted hash route as ordered range");
}

void RecheckRerankMetricsAndPersistenceRefusals() {
  auto hash = Evidence(index::IndexFamily::hash);
  hash.mga_recheck_proven = false;
  ExpectRejected(Request(index::IndexFamily::hash,
                         index::IndexPlanCategory::point_lookup,
                         &hash),
                 "INDEX.OPTIMIZER_READINESS_EVIDENCE.RECHECK_PROOF_REQUIRED",
                 "CEIC-060 admitted missing MGA recheck proof");

  auto vector = Evidence(index::IndexFamily::vector_hnsw,
                         index::IndexRouteKind::nosql_vector);
  vector.exact_rerank_proven = false;
  ExpectRejected(Request(index::IndexFamily::vector_hnsw,
                         index::IndexPlanCategory::vector_search,
                         &vector,
                         index::IndexRouteKind::nosql_vector),
                 "INDEX.OPTIMIZER_READINESS_EVIDENCE.EXACT_RERANK_REQUIRED",
                 "CEIC-060 admitted missing exact rerank proof");

  auto btree = Evidence(index::IndexFamily::btree);
  btree.operation_metrics_producer_proven = false;
  ExpectRejected(Request(index::IndexFamily::btree,
                         index::IndexPlanCategory::point_lookup,
                         &btree),
                 "INDEX.OPTIMIZER_READINESS_EVIDENCE.METRICS_PROOF_REQUIRED",
                 "CEIC-060 admitted missing CEIC-040 metrics proof");

  btree = Evidence(index::IndexFamily::btree);
  btree.crash_reopen_proven = false;
  ExpectRejected(Request(index::IndexFamily::btree,
                         index::IndexPlanCategory::point_lookup,
                         &btree),
                 "INDEX.OPTIMIZER_READINESS_EVIDENCE.PERSISTENT_PROOF_REQUIRED",
                 "CEIC-060 admitted missing CEIC-041 crash proof");

  btree = Evidence(index::IndexFamily::btree);
  btree.storage_integration_proven = false;
  ExpectRejected(Request(index::IndexFamily::btree,
                         index::IndexPlanCategory::point_lookup,
                         &btree),
                 "INDEX.OPTIMIZER_READINESS_EVIDENCE.PERSISTENT_PROOF_REQUIRED",
                 "CEIC-060 admitted missing persistent storage proof");
}

void AuthorityAndClusterRefusals() {
  auto evidence = Evidence(index::IndexFamily::btree);
  evidence.parser_authority = true;
  ExpectRejected(Request(index::IndexFamily::btree,
                         index::IndexPlanCategory::point_lookup,
                         &evidence),
                 "INDEX.OPTIMIZER_READINESS_EVIDENCE.UNSAFE_AUTHORITY",
                 "CEIC-060 admitted parser authority drift");

  evidence = Evidence(index::IndexFamily::btree);
  evidence.local_cluster_authority = true;
  ExpectRejected(Request(index::IndexFamily::btree,
                         index::IndexPlanCategory::point_lookup,
                         &evidence),
                 "INDEX.OPTIMIZER_READINESS_EVIDENCE.UNSAFE_AUTHORITY",
                 "CEIC-060 admitted local cluster authority");

  evidence = Evidence(index::IndexFamily::btree);
  evidence.external_cluster_runtime_overclaim = true;
  ExpectRejected(Request(index::IndexFamily::btree,
                         index::IndexPlanCategory::point_lookup,
                         &evidence),
                 "INDEX.OPTIMIZER_READINESS_EVIDENCE.UNSAFE_AUTHORITY",
                 "CEIC-060 admitted external-cluster overclaim");
}

opt::IndexStats CostIndex() {
  opt::IndexStats index;
  index.identity.object_uuid = "rel.ceic060";
  index.identity.statistic_uuid = "idx.ceic060.btree:stats";
  index.identity.stats_epoch = 6004;
  index.identity.catalog_epoch = 6005;
  index.identity.transaction_visibility_epoch = 6006;
  index.identity.source = opt::StatisticSource::kCatalogSample;
  index.identity.freshness = opt::OptimizerStatsFreshnessState::kFresh;
  index.identity.confidence = opt::CostConfidence::kHigh;
  index.index_uuid = "idx.ceic060.btree";
  index.relation_uuid = "rel.ceic060";
  index.index_family = "btree";
  index.descriptor_digest = "descriptor:ceic060";
  index.height = 3;
  index.leaf_pages = 64;
  index.distinct_keys = 10000;
  index.visibility_coverage = 1.0;
  index.predicate_coverage = 0.1;
  index.exact_recheck_required = true;
  index.mga_recheck_required = true;
  index.security_recheck_required = true;
  index.route_benchmark_clean = true;
  index.equality_lookup_supported = true;
  return index;
}

opt::TableCardinalityStats CostTable() {
  opt::TableCardinalityStats table;
  table.identity.object_uuid = "rel.ceic060";
  table.identity.statistic_uuid = "rel.ceic060:table_stats";
  table.identity.stats_epoch = 6007;
  table.identity.catalog_epoch = 6008;
  table.identity.transaction_visibility_epoch = 6009;
  table.identity.source = opt::StatisticSource::kCatalogSample;
  table.identity.freshness = opt::OptimizerStatsFreshnessState::kFresh;
  table.identity.confidence = opt::CostConfidence::kHigh;
  table.row_count = 100000;
  table.page_count = 1000;
  table.average_row_bytes = 256;
  return table;
}

opt::EnterpriseIndexCostAuthority CostAuthority() {
  opt::EnterpriseIndexCostAuthority authority;
  authority.optimizer_scope = true;
  authority.catalog_descriptor_authority = true;
  authority.index_stats_authority = true;
  authority.route_capability_authority = true;
  authority.runtime_metric_authority = true;
  authority.generated_index_readiness_manifest = true;
  authority.readiness_manifest_current = true;
  authority.route_runtime_proof = true;
  authority.operation_metric_producer_proof = true;
  authority.support_bundle_producer_proof = true;
  authority.crash_cleanup_corruption_proof = true;
  authority.storage_integration_proof = true;
  authority.exact_recheck_preserved = true;
  authority.mga_recheck_preserved = true;
  authority.security_recheck_preserved = true;
  return authority;
}

void CostingConsumesReadinessProof() {
  opt::EnterpriseIndexCostRequest request;
  request.index = CostIndex();
  request.table = CostTable();
  request.intent = opt::EnterpriseIndexAccessIntent::kEqualityLookup;
  request.authority = CostAuthority();
  request.environment.memory_budget_bytes = 1024 * 1024;

  auto accepted = opt::EstimateEnterpriseIndexAccessCost(request);
  Require(accepted.accepted, "CEIC-060 optimizer costing rejected complete readiness proof");

  request.authority.generated_index_readiness_manifest = false;
  auto missing = opt::EstimateEnterpriseIndexAccessCost(request);
  Require(!missing.accepted &&
              missing.diagnostic_code == "SB_OPT_INDEX_COST_AUTHORITY_REQUIRED",
          "CEIC-060 optimizer costing admitted missing readiness manifest proof");

  request.authority = CostAuthority();
  request.authority.static_registry_only = true;
  auto unsafe = opt::EstimateEnterpriseIndexAccessCost(request);
  Require(!unsafe.accepted &&
              unsafe.diagnostic_code ==
                  "SB_OPT_INDEX_COST_READINESS_EVIDENCE_UNSAFE",
          "CEIC-060 optimizer costing admitted static readiness proof");
}

}  // namespace

int main() {
  PositiveCases();
  MissingAndStaleManifestRefusals();
  NonRuntimeMismatchAndRouteSemanticsRefusals();
  RecheckRerankMetricsAndPersistenceRefusals();
  AuthorityAndClusterRefusals();
  CostingConsumesReadinessProof();
  return EXIT_SUCCESS;
}
