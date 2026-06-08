// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "index_family_route_classification.hpp"

// CEIC_038_LOSSY_PRUNING_FAMILY_CLASSIFICATION

#include <utility>

namespace scratchbird::core::index {
namespace {
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;

Status OkStatus() { return {StatusCode::ok, Severity::info, Subsystem::engine}; }

Status RefuseStatus() {
  return {StatusCode::platform_required_feature_missing,
          Severity::error,
          Subsystem::engine};
}

const char* BoolText(bool value) { return value ? "true" : "false"; }

bool OrderedExactFamily(IndexFamily family) {
  return family == IndexFamily::btree ||
         family == IndexFamily::unique_btree ||
         family == IndexFamily::expression ||
         family == IndexFamily::partial ||
         family == IndexFamily::covering;
}

bool TokenFamily(IndexFamily family) {
  return family == IndexFamily::full_text ||
         family == IndexFamily::gin ||
         family == IndexFamily::inverted ||
         family == IndexFamily::ngram ||
         family == IndexFamily::sparse_wand;
}

bool SpatialFamily(IndexFamily family) {
  return family == IndexFamily::spatial ||
         family == IndexFamily::rtree ||
         family == IndexFamily::gist ||
         family == IndexFamily::spgist;
}

bool VectorFamily(IndexFamily family) {
  return family == IndexFamily::vector_exact ||
         family == IndexFamily::vector_hnsw ||
         family == IndexFamily::vector_ivf;
}

bool ApproximateVectorFamily(IndexFamily family) {
  return family == IndexFamily::vector_hnsw ||
         family == IndexFamily::vector_ivf;
}

bool SummaryPruneFamily(IndexFamily family) {
  return family == IndexFamily::brin_zone ||
         family == IndexFamily::columnar_zone;
}

bool CandidateOnlyFamily(IndexFamily family) {
  return family == IndexFamily::bitmap ||
         family == IndexFamily::document_path ||
         family == IndexFamily::graph ||
         TokenFamily(family) ||
         VectorFamily(family) ||
         SpatialFamily(family);
}

bool ReadLikeRoute(IndexRouteKind route) {
  return route == IndexRouteKind::sql_select ||
         route == IndexRouteKind::nosql_document ||
         route == IndexRouteKind::nosql_graph ||
         route == IndexRouteKind::nosql_vector ||
         route == IndexRouteKind::nosql_search ||
         route == IndexRouteKind::maintenance ||
         route == IndexRouteKind::validate_repair;
}

IndexFamilyRouteSemantic SemanticForFamily(IndexFamily family) {
  if (OrderedExactFamily(family)) {
    return IndexFamilyRouteSemantic::exact_candidate;
  }
  if (family == IndexFamily::hash) {
    return IndexFamilyRouteSemantic::hash_equality_candidate;
  }
  if (family == IndexFamily::bitmap) {
    return IndexFamilyRouteSemantic::bitmap_candidate;
  }
  if (family == IndexFamily::sparse_wand) {
    return IndexFamilyRouteSemantic::token_ranking_candidate;
  }
  if (family == IndexFamily::full_text ||
      family == IndexFamily::gin ||
      family == IndexFamily::inverted ||
      family == IndexFamily::ngram) {
    return IndexFamilyRouteSemantic::token_candidate;
  }
  if (SpatialFamily(family)) {
    return IndexFamilyRouteSemantic::spatial_candidate;
  }
  if (family == IndexFamily::vector_exact) {
    return IndexFamilyRouteSemantic::vector_exact_candidate;
  }
  if (ApproximateVectorFamily(family)) {
    return IndexFamilyRouteSemantic::vector_approximate_candidate;
  }
  if (family == IndexFamily::document_path) {
    return IndexFamilyRouteSemantic::document_path_candidate;
  }
  if (family == IndexFamily::graph) {
    return IndexFamilyRouteSemantic::graph_seed_candidate;
  }
  if (family == IndexFamily::bloom) {
    return IndexFamilyRouteSemantic::bloom_negative_prune;
  }
  if (SummaryPruneFamily(family)) {
    return IndexFamilyRouteSemantic::summary_segment_prune;
  }
  if (family == IndexFamily::temporary_work) {
    return IndexFamilyRouteSemantic::temporary_work_candidate;
  }
  if (family == IndexFamily::in_memory) {
    return IndexFamilyRouteSemantic::in_memory_candidate;
  }
  if (family == IndexFamily::donor_emulated) {
    return IndexFamilyRouteSemantic::donor_emulated_non_runtime;
  }
  if (family == IndexFamily::policy_blocked) {
    return IndexFamilyRouteSemantic::policy_blocked_non_runtime;
  }
  return IndexFamilyRouteSemantic::unsupported;
}

IndexRouteClassificationRequirements RequirementsFor(
    IndexFamily family,
    IndexRouteKind route,
    bool route_supported) {
  IndexRouteClassificationRequirements requirements;
  const bool row_path = route_supported && ReadLikeRoute(route);
  requirements.exact_source_required = row_path;
  requirements.mga_visibility_recheck_required = true;
  requirements.security_recheck_required = true;
  requirements.authorization_recheck_required = true;
  requirements.predicate_recheck_required = true;
  requirements.ceic_037_exact_recheck_handoff_required = row_path;
  requirements.exact_fallback_required =
      route_supported &&
      (family == IndexFamily::bloom ||
       SummaryPruneFamily(family) ||
       ApproximateVectorFamily(family) ||
       family == IndexFamily::sparse_wand ||
       family == IndexFamily::bitmap ||
       TokenFamily(family) ||
       SpatialFamily(family) ||
       family == IndexFamily::document_path ||
       family == IndexFamily::graph);
  requirements.exact_rerank_required =
      route_supported &&
      (ApproximateVectorFamily(family) ||
       family == IndexFamily::sparse_wand);
  requirements.false_positive_accounting_required =
      route_supported &&
      (family == IndexFamily::bloom ||
       SummaryPruneFamily(family) ||
       family == IndexFamily::bitmap ||
       TokenFamily(family) ||
       SpatialFamily(family) ||
       VectorFamily(family) ||
       family == IndexFamily::document_path ||
       family == IndexFamily::graph);
  requirements.stale_generation_policy_required =
      route_supported &&
      (family == IndexFamily::bloom ||
       SummaryPruneFamily(family) ||
       CandidateOnlyFamily(family));
  requirements.persistent_provider_closure_required =
      route_supported &&
      family != IndexFamily::temporary_work &&
      family != IndexFamily::in_memory &&
      family != IndexFamily::donor_emulated &&
      family != IndexFamily::policy_blocked;
  requirements.runtime_metrics_future_proof_required = true;
  requirements.crash_matrix_future_proof_required = true;
  return requirements;
}

void AddEvidence(IndexRouteFamilyClassificationResult* result,
                 std::string key,
                 std::string value) {
  result->evidence.push_back(std::move(key) + "=" + std::move(value));
}

void AddBoolEvidence(IndexRouteFamilyClassificationResult* result,
                     const char* key,
                     bool value) {
  AddEvidence(result, key, BoolText(value));
}

DiagnosticRecord MakeClassificationDiagnostic(
    Status status,
    IndexRouteClassificationStatus classification_status,
    const IndexRouteClassificationRequest& request,
    std::string detail) {
  DiagnosticRecord record;
  record.status = status;
  record.diagnostic_code = std::string("INDEX.ROUTE_CLASSIFICATION.") +
                           IndexRouteClassificationStatusName(
                               classification_status);
  record.message_key = std::string("index.route_classification.") +
                       IndexRouteClassificationStatusName(
                           classification_status);
  record.arguments.push_back({"family", IndexFamilyName(request.family)});
  record.arguments.push_back({"route", IndexRouteKindName(request.route)});
  if (!detail.empty()) {
    record.arguments.push_back({"detail", std::move(detail)});
  }
  record.source_component = "sb_core_index.route_family_classification";
  return record;
}

void AddRequirementEvidence(IndexRouteFamilyClassificationResult* result) {
  AddBoolEvidence(result, "exact_source_required",
                  result->requirements.exact_source_required);
  AddBoolEvidence(result, "mga_visibility_recheck_required",
                  result->requirements.mga_visibility_recheck_required);
  AddBoolEvidence(result, "security_recheck_required",
                  result->requirements.security_recheck_required);
  AddBoolEvidence(result, "authorization_recheck_required",
                  result->requirements.authorization_recheck_required);
  AddBoolEvidence(result, "predicate_recheck_required",
                  result->requirements.predicate_recheck_required);
  AddBoolEvidence(
      result,
      "ceic_037_exact_recheck_handoff_required",
      result->requirements.ceic_037_exact_recheck_handoff_required);
  AddBoolEvidence(result, "exact_fallback_required",
                  result->requirements.exact_fallback_required);
  AddBoolEvidence(result, "exact_rerank_required",
                  result->requirements.exact_rerank_required);
  AddBoolEvidence(result, "false_positive_accounting_required",
                  result->requirements.false_positive_accounting_required);
  AddBoolEvidence(result, "stale_generation_policy_required",
                  result->requirements.stale_generation_policy_required);
  AddBoolEvidence(result, "persistent_provider_closure_required",
                  result->requirements.persistent_provider_closure_required);
  AddBoolEvidence(result, "runtime_metrics_future_proof_required",
                  result->requirements.runtime_metrics_future_proof_required);
  AddBoolEvidence(result, "crash_matrix_future_proof_required",
                  result->requirements.crash_matrix_future_proof_required);
}

void AddAuthorityEvidence(IndexRouteFamilyClassificationResult* result) {
  AddBoolEvidence(result, "row_truth_authority", result->row_truth_authority);
  AddBoolEvidence(result, "final_row_authority", result->final_row_authority);
  AddBoolEvidence(result, "transaction_finality_authority",
                  result->transaction_finality_authority);
  AddBoolEvidence(result, "visibility_authority",
                  result->visibility_authority);
  AddBoolEvidence(result, "authorization_authority",
                  result->authorization_authority);
  AddBoolEvidence(result, "authorization_security_authority",
                  result->authorization_security_authority);
  AddBoolEvidence(result, "security_authority", result->security_authority);
  AddBoolEvidence(result, "recovery_authority", result->recovery_authority);
  AddBoolEvidence(result, "parser_authority", result->parser_authority);
  AddBoolEvidence(result, "donor_authority", result->donor_authority);
  AddBoolEvidence(result, "wal_authority", result->wal_authority);
  AddBoolEvidence(result, "benchmark_authority", result->benchmark_authority);
  AddBoolEvidence(result, "optimizer_plan_authority",
                  result->optimizer_plan_authority);
  AddBoolEvidence(result, "optimizer_plan_finality_authority",
                  result->optimizer_plan_finality_authority);
  AddBoolEvidence(result, "index_finality_authority",
                  result->index_finality_authority);
  AddBoolEvidence(result, "result_finality_authority",
                  result->result_finality_authority);
  AddBoolEvidence(result, "local_cluster_authority",
                  result->local_cluster_authority);
  AddBoolEvidence(result, "cluster_action_authority",
                  result->cluster_action_authority);
  AddBoolEvidence(result, "agent_action_authority",
                  result->agent_action_authority);
}

void AddSuccessorEvidence(IndexRouteFamilyClassificationResult* result) {
  AddBoolEvidence(result, "ceic_039_specialized_provider_closure_claimed",
                  result->ceic_039_specialized_provider_closure_claimed);
  AddBoolEvidence(result, "ceic_040_runtime_metrics_claimed",
                  result->ceic_040_runtime_metrics_claimed);
  AddBoolEvidence(result, "ceic_041_crash_matrix_claimed",
                  result->ceic_041_crash_matrix_claimed);
  AddBoolEvidence(result, "ceic_042_readiness_drift_claimed",
                  result->ceic_042_readiness_drift_claimed);
  AddBoolEvidence(result, "all_index_readiness_claimed",
                  result->all_index_readiness_claimed);
  AddBoolEvidence(result, "donor_dominance_claimed",
                  result->donor_dominance_claimed);
  AddBoolEvidence(result, "enterprise_readiness_claimed",
                  result->enterprise_readiness_claimed);
}

IndexRouteFamilyClassificationResult BaseResult(
    const IndexRouteClassificationRequest& request,
    bool route_supported) {
  IndexRouteFamilyClassificationResult result;
  result.status = OkStatus();
  result.semantic = SemanticForFamily(request.family);
  result.requirements =
      RequirementsFor(request.family, request.route, route_supported);
  result.supports_negative_prune =
      route_supported && request.family == IndexFamily::bloom;
  result.negative_prune_only = result.supports_negative_prune;
  result.supports_summary_segment_prune =
      route_supported && SummaryPruneFamily(request.family);
  result.summary_segment_prune_only = result.supports_summary_segment_prune;
  result.approximate_candidate_source =
      route_supported && ApproximateVectorFamily(request.family);
  result.bitmap_candidate_source =
      route_supported && request.family == IndexFamily::bitmap;
  result.token_or_inverted_candidate_source =
      route_supported && TokenFamily(request.family);
  result.vector_candidate_source =
      route_supported && VectorFamily(request.family);
  result.document_candidate_source =
      route_supported && request.family == IndexFamily::document_path;
  result.graph_candidate_source =
      route_supported && request.family == IndexFamily::graph;
  result.spatial_candidate_source =
      route_supported && SpatialFamily(request.family);
  result.hash_equality_only =
      route_supported && request.family == IndexFamily::hash;
  result.supports_ordered_range =
      route_supported && OrderedExactFamily(request.family);
  result.produces_exact_candidates =
      route_supported && ReadLikeRoute(request.route) &&
      (OrderedExactFamily(request.family) ||
       request.family == IndexFamily::hash);
  result.produces_lossy_candidates =
      route_supported &&
      (request.family == IndexFamily::bitmap ||
       TokenFamily(request.family) ||
       SpatialFamily(request.family) ||
       ApproximateVectorFamily(request.family) ||
       request.family == IndexFamily::document_path ||
       request.family == IndexFamily::graph);
  result.produces_candidate_set =
      route_supported && CandidateOnlyFamily(request.family);
  result.produces_ranking =
      route_supported &&
      (VectorFamily(request.family) ||
       request.family == IndexFamily::sparse_wand);
  result.produces_seed_set =
      route_supported &&
      (request.family == IndexFamily::graph ||
       request.family == IndexFamily::document_path ||
       SpatialFamily(request.family));

  AddEvidence(&result, "ceic_search_key",
              "CEIC_038_LOSSY_PRUNING_FAMILY_CLASSIFICATION");
  AddEvidence(&result, "family", IndexFamilyName(request.family));
  AddEvidence(&result, "route", IndexRouteKindName(request.route));
  AddEvidence(&result, "semantic",
              IndexFamilyRouteSemanticName(result.semantic));
  AddBoolEvidence(&result, "route_supported", route_supported);
  AddBoolEvidence(&result, "engine_owned_classification",
                  result.engine_owned_classification);
  AddBoolEvidence(&result, "ceic_038_family_classification_evidence",
                  result.ceic_038_family_classification_evidence);
  AddBoolEvidence(&result, "produces_exact_candidates",
                  result.produces_exact_candidates);
  AddBoolEvidence(&result, "produces_lossy_candidates",
                  result.produces_lossy_candidates);
  AddBoolEvidence(&result, "produces_candidate_set",
                  result.produces_candidate_set);
  AddBoolEvidence(&result, "produces_ranking", result.produces_ranking);
  AddBoolEvidence(&result, "produces_seed_set", result.produces_seed_set);
  AddBoolEvidence(&result, "supports_negative_prune",
                  result.supports_negative_prune);
  AddBoolEvidence(&result, "negative_prune_only",
                  result.negative_prune_only);
  AddBoolEvidence(&result, "supports_summary_segment_prune",
                  result.supports_summary_segment_prune);
  AddBoolEvidence(&result, "summary_segment_prune_only",
                  result.summary_segment_prune_only);
  AddBoolEvidence(&result, "approximate_candidate_source",
                  result.approximate_candidate_source);
  AddBoolEvidence(&result, "hash_equality_only",
                  result.hash_equality_only);
  AddBoolEvidence(&result, "supports_ordered_range",
                  result.supports_ordered_range);
  AddRequirementEvidence(&result);
  AddAuthorityEvidence(&result);
  AddSuccessorEvidence(&result);
  return result;
}

IndexRouteFamilyClassificationResult RefuseClassification(
    const IndexRouteClassificationRequest& request,
    bool route_supported,
    IndexRouteClassificationStatus classification_status,
    std::string detail) {
  auto result = BaseResult(request, route_supported);
  result.status = RefuseStatus();
  result.classified = false;
  result.fail_closed = true;
  result.runtime_admissible = false;
  result.classification_status = classification_status;
  result.diagnostic = MakeClassificationDiagnostic(
      result.status, classification_status, request, std::move(detail));
  AddEvidence(&result, "classification_status",
              IndexRouteClassificationStatusName(classification_status));
  AddEvidence(&result, "diagnostic_code", result.diagnostic.diagnostic_code);
  AddBoolEvidence(&result, "classified", result.classified);
  AddBoolEvidence(&result, "runtime_admissible", result.runtime_admissible);
  AddBoolEvidence(&result, "fail_closed", result.fail_closed);
  return result;
}

}  // namespace

const char* IndexFamilyRouteSemanticName(IndexFamilyRouteSemantic semantic) {
  switch (semantic) {
    case IndexFamilyRouteSemantic::exact_candidate:
      return "exact_candidate";
    case IndexFamilyRouteSemantic::hash_equality_candidate:
      return "hash_equality_candidate";
    case IndexFamilyRouteSemantic::bitmap_candidate:
      return "bitmap_candidate";
    case IndexFamilyRouteSemantic::token_candidate:
      return "token_candidate";
    case IndexFamilyRouteSemantic::token_ranking_candidate:
      return "token_ranking_candidate";
    case IndexFamilyRouteSemantic::spatial_candidate:
      return "spatial_candidate";
    case IndexFamilyRouteSemantic::vector_exact_candidate:
      return "vector_exact_candidate";
    case IndexFamilyRouteSemantic::vector_approximate_candidate:
      return "vector_approximate_candidate";
    case IndexFamilyRouteSemantic::document_path_candidate:
      return "document_path_candidate";
    case IndexFamilyRouteSemantic::graph_seed_candidate:
      return "graph_seed_candidate";
    case IndexFamilyRouteSemantic::bloom_negative_prune:
      return "bloom_negative_prune";
    case IndexFamilyRouteSemantic::summary_segment_prune:
      return "summary_segment_prune";
    case IndexFamilyRouteSemantic::temporary_work_candidate:
      return "temporary_work_candidate";
    case IndexFamilyRouteSemantic::in_memory_candidate:
      return "in_memory_candidate";
    case IndexFamilyRouteSemantic::donor_emulated_non_runtime:
      return "donor_emulated_non_runtime";
    case IndexFamilyRouteSemantic::policy_blocked_non_runtime:
      return "policy_blocked_non_runtime";
    case IndexFamilyRouteSemantic::unsupported:
      return "unsupported";
  }
  return "unsupported";
}

const char* IndexRouteClassificationStatusName(
    IndexRouteClassificationStatus status) {
  switch (status) {
    case IndexRouteClassificationStatus::classified:
      return "CLASSIFIED";
    case IndexRouteClassificationStatus::unsupported_family:
      return "UNSUPPORTED_FAMILY";
    case IndexRouteClassificationStatus::unsupported_route:
      return "UNSUPPORTED_ROUTE";
    case IndexRouteClassificationStatus::donor_emulated_non_runtime:
      return "DONOR_EMULATED_NON_RUNTIME";
    case IndexRouteClassificationStatus::policy_blocked_non_runtime:
      return "POLICY_BLOCKED_NON_RUNTIME";
    case IndexRouteClassificationStatus::route_not_supported:
      return "ROUTE_NOT_SUPPORTED";
    case IndexRouteClassificationStatus::forbidden_authority_claim:
      return "FORBIDDEN_AUTHORITY_CLAIM";
    case IndexRouteClassificationStatus::cluster_external_provider_only:
      return "CLUSTER_EXTERNAL_PROVIDER_ONLY";
    case IndexRouteClassificationStatus::successor_or_enterprise_overclaim:
      return "SUCCESSOR_OR_ENTERPRISE_OVERCLAIM";
  }
  return "UNKNOWN";
}

bool IndexRouteClassificationAuthorityClaimsClear(
    const IndexRouteClassificationAuthorityClaims& claims) {
  return !claims.transaction_finality_authority &&
         !claims.visibility_authority &&
         !claims.authorization_authority &&
         !claims.authorization_security_authority &&
         !claims.security_authority &&
         !claims.recovery_authority &&
         !claims.parser_authority &&
         !claims.donor_authority &&
         !claims.wal_authority &&
         !claims.benchmark_authority &&
         !claims.optimizer_plan_authority &&
         !claims.optimizer_plan_finality_authority &&
         !claims.index_finality_authority &&
         !claims.row_truth_authority &&
         !claims.final_row_authority &&
         !claims.result_finality_authority &&
         !claims.local_cluster_authority &&
         !claims.cluster_action_authority &&
         !claims.agent_action_authority;
}

bool IndexRouteClassificationSuccessorClaimsClear(
    const IndexRouteClassificationSuccessorClaims& claims) {
  return !claims.ceic_039_specialized_provider_closure_claimed &&
         !claims.ceic_040_runtime_metrics_claimed &&
         !claims.ceic_041_crash_matrix_claimed &&
         !claims.ceic_042_readiness_drift_claimed &&
         !claims.all_index_readiness_claimed &&
         !claims.donor_dominance_claimed &&
         !claims.enterprise_readiness_claimed;
}

IndexRouteFamilyClassificationResult ClassifyIndexFamilyRoute(
    const IndexRouteClassificationRequest& request) {
  const auto* descriptor = FindBuiltinIndexFamily(request.family);
  const auto* route_state =
      FindBuiltinIndexRouteCapabilityState(request.route, request.family);
  const bool route_supported =
      route_state != nullptr && route_state->route_supported;

  if (request.cluster_path_requested ||
      !request.external_cluster_provider_only ||
      request.authority_claims.local_cluster_authority ||
      request.authority_claims.cluster_action_authority) {
    return RefuseClassification(
        request,
        route_supported,
        IndexRouteClassificationStatus::cluster_external_provider_only,
        "cluster index route classification is external-provider-only; local cluster APIs or actions are not implemented here");
  }
  if (!IndexRouteClassificationAuthorityClaimsClear(
          request.authority_claims)) {
    return RefuseClassification(
        request,
        route_supported,
        IndexRouteClassificationStatus::forbidden_authority_claim,
        "classification evidence must not claim row truth result finality transaction finality visibility authorization security recovery parser donor WAL benchmark optimizer-plan index-finality local-cluster cluster-action or agent-action authority");
  }
  if (!IndexRouteClassificationSuccessorClaimsClear(
          request.successor_claims)) {
    return RefuseClassification(
        request,
        route_supported,
        IndexRouteClassificationStatus::successor_or_enterprise_overclaim,
        "CEIC-038 classification cannot claim specialized provider closure runtime metrics crash matrix readiness drift all-index readiness donor dominance or enterprise readiness");
  }
  if (descriptor == nullptr || request.family == IndexFamily::unknown) {
    return RefuseClassification(
        request,
        route_supported,
        IndexRouteClassificationStatus::unsupported_family,
        "family is not registered as a built-in index family");
  }
  if (request.family == IndexFamily::donor_emulated ||
      descriptor->persistence == IndexPersistenceClass::donor_emulated) {
    return RefuseClassification(
        request,
        route_supported,
        IndexRouteClassificationStatus::donor_emulated_non_runtime,
        "donor-emulated index mappings are non-runtime semantic mappings and cannot provide row, visibility, finality, or recovery authority");
  }
  if (request.family == IndexFamily::policy_blocked ||
      descriptor->persistence == IndexPersistenceClass::policy_blocked) {
    return RefuseClassification(
        request,
        route_supported,
        IndexRouteClassificationStatus::policy_blocked_non_runtime,
        "policy-blocked index families fail closed and are not runtime route families");
  }
  if (request.route == IndexRouteKind::unknown) {
    return RefuseClassification(
        request,
        route_supported,
        IndexRouteClassificationStatus::unsupported_route,
        "route is not a known index route kind");
  }
  if (!route_supported) {
    return RefuseClassification(
        request,
        route_supported,
        IndexRouteClassificationStatus::route_not_supported,
        "route/family pair is not supported by the index route capability surface");
  }

  auto result = BaseResult(request, route_supported);
  result.status = OkStatus();
  result.classified = true;
  result.fail_closed = false;
  result.runtime_admissible = true;
  result.classification_status = IndexRouteClassificationStatus::classified;
  result.diagnostic = MakeClassificationDiagnostic(
      result.status,
      result.classification_status,
      request,
      "route/family classified as candidate, prune, ranking, seed, exact candidate, non-persistent runtime, or fail-closed semantics without row authority");
  AddEvidence(&result, "classification_status",
              IndexRouteClassificationStatusName(
                  result.classification_status));
  AddEvidence(&result, "diagnostic_code", result.diagnostic.diagnostic_code);
  AddBoolEvidence(&result, "classified", result.classified);
  AddBoolEvidence(&result, "runtime_admissible", result.runtime_admissible);
  AddBoolEvidence(&result, "fail_closed", result.fail_closed);
  AddEvidence(&result, "classification_boundary",
              "candidate_or_prune_only_until_engine_exact_recheck");
  return result;
}

}  // namespace scratchbird::core::index
