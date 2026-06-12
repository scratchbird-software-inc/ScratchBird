// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "specialized_persistent_provider_closure.hpp"

// CEIC_039_SPECIALIZED_PERSISTENT_PROVIDER_CLOSURE
#include <utility>

namespace scratchbird::core::index {
namespace {

using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;

Status OkStatus() {
  return Status{StatusCode::ok, Severity::info, Subsystem::engine};
}

Status RefuseStatus() {
  return Status{StatusCode::platform_required_feature_missing,
                Severity::error,
                Subsystem::engine};
}

const char* BoolText(bool value) { return value ? "true" : "false"; }

void AddEvidence(SpecializedPersistentProviderClosureResult* result,
                 std::string key,
                 std::string value) {
  result->evidence.push_back(std::move(key) + "=" + std::move(value));
}

void AddBoolEvidence(SpecializedPersistentProviderClosureResult* result,
                     const char* key,
                     bool value) {
  AddEvidence(result, key, BoolText(value));
}

void AddU64Evidence(SpecializedPersistentProviderClosureResult* result,
                    const char* key,
                    u64 value) {
  AddEvidence(result, key, std::to_string(value));
}

bool EvidenceHasExact(const std::vector<std::string>& evidence,
                      const std::string& key,
                      const std::string& value) {
  const std::string expected = key + "=" + value;
  for (const auto& row : evidence) {
    if (row == expected) {
      return true;
    }
  }
  return false;
}

bool PriorSliceFamily(IndexFamily family) {
  return family == IndexFamily::btree ||
         family == IndexFamily::unique_btree ||
         family == IndexFamily::expression ||
         family == IndexFamily::partial ||
         family == IndexFamily::covering ||
         family == IndexFamily::hash;
}

bool DurableProviderEvidenceValid(
    const SpecializedDurableProviderEvidence& evidence) {
  return evidence.durable_storage_integration_proven &&
         evidence.family_specific_physical_payload_proven &&
         evidence.provider_artifact_format_version_proven &&
         evidence.provider_open_reopen_identity_proven &&
         evidence.provider_payload_integrity_proven &&
         evidence.provider_evidence_only_not_authority &&
         !evidence.durable_storage_evidence_id.empty() &&
         !evidence.provider_payload_evidence_id.empty() &&
         !evidence.artifact_format_evidence_id.empty() &&
         !evidence.durable_evidence_rows.empty();
}

bool GenerationIdentityValid(
    const SpecializedGenerationIdentityProof& proof) {
  return proof.index_uuid.valid() &&
         proof.generation_uuid.valid() &&
         proof.root_or_segment_uuid.valid() &&
         proof.generation_number != 0 &&
         proof.cow_generation_number != 0 &&
         proof.cleanup_generation_floor != 0 &&
         proof.oldest_active_transaction_id != 0 &&
         !proof.provider_generation_id.empty() &&
         !proof.root_or_provider_identity_evidence_id.empty() &&
         proof.cow_generation_publish_proven &&
         proof.root_or_provider_identity_bound &&
         proof.provider_generation_matches_ceic031 &&
         proof.generation_matches_ceic032 &&
         proof.cleanup_identity_matches_ceic031_ceic032 &&
         proof.publish_after_durable_mga_evidence;
}

bool CleanupProofValid(const SpecializedCleanupHorizonProof& proof,
                       const SpecializedGenerationIdentityProof& generation) {
  return proof.oldest_active_transaction_id != 0 &&
         proof.cleanup_generation_floor != 0 &&
         proof.oldest_active_transaction_id ==
             generation.oldest_active_transaction_id &&
         proof.cleanup_generation_floor == generation.cleanup_generation_floor &&
         proof.engine_mga_horizon_bound &&
         proof.cleanup_uses_engine_horizon &&
         proof.cleanup_identity_matches_ceic031_ceic032 &&
         proof.provider_cleanup_evidence_only &&
         !proof.cleanup_evidence_id.empty();
}

bool ValidationRepairRebuildValid(
    const SpecializedValidationRepairRebuildProof& proof,
    const IndexRecoveryRecommendation& recommendation) {
  return proof.validation_proven &&
         proof.repair_supported &&
         proof.rebuild_supported &&
         proof.deterministic_diagnostics &&
         proof.recommendation_matches_ceic032 &&
         proof.evidence_only_not_crash_matrix &&
         !proof.validation_evidence_id.empty() &&
         !proof.repair_rebuild_evidence_id.empty() &&
         recommendation.validate &&
         !recommendation.stable_actions.empty();
}

bool ProviderAdmissionMatchesRequest(
    const IndexProviderAdmissionResult& result,
    const SpecializedPersistentProviderClosureRequest& request) {
  return EvidenceHasExact(result.evidence,
                          "ceic_search_key",
                          "CEIC_031_INDEX_ACCESS_METHOD_PROVIDER_INTERFACE") &&
         EvidenceHasExact(result.evidence,
                          "family",
                          IndexFamilyName(request.family)) &&
         EvidenceHasExact(result.evidence,
                          "route",
                          IndexRouteKindName(request.route)) &&
         EvidenceHasExact(result.evidence,
                          "provider_id",
                          request.provider_id) &&
         EvidenceHasExact(
             result.evidence,
             "provider_generation_id",
             request.generation_identity.provider_generation_id) &&
         EvidenceHasExact(
             result.evidence,
             "generation_number",
             std::to_string(request.generation_identity.generation_number)) &&
         EvidenceHasExact(result.evidence, "cow_generation", "true") &&
         EvidenceHasExact(result.evidence, "root_identity_bound", "true") &&
         EvidenceHasExact(
             result.evidence,
             "cleanup_generation_floor",
             std::to_string(
                 request.generation_identity.cleanup_generation_floor)) &&
         EvidenceHasExact(
             result.evidence,
             "oldest_active_transaction_id",
             std::to_string(
                 request.generation_identity.oldest_active_transaction_id));
}

bool MGARecoveryContractMatchesRequest(
    const IndexMGARecoveryContractResult& result,
    const SpecializedPersistentProviderClosureRequest& request) {
  return EvidenceHasExact(result.evidence,
                          "ceic_search_key",
                          "CEIC_032_INDEX_MGA_RECOVERY_CONTRACT") &&
         EvidenceHasExact(result.evidence,
                          "family",
                          IndexFamilyName(request.family)) &&
         EvidenceHasExact(result.evidence,
                          "route",
                          IndexRouteKindName(request.route)) &&
         EvidenceHasExact(result.evidence,
                          "provider_id",
                          request.provider_id) &&
         EvidenceHasExact(result.evidence,
                          "provider_generation_id",
                          request.generation_identity.provider_generation_id) &&
         EvidenceHasExact(result.evidence,
                          "generation_publish_state",
                          IndexGenerationPublishStateName(
                              IndexGenerationPublishState::published)) &&
         EvidenceHasExact(
             result.evidence,
             "generation_number",
             std::to_string(request.generation_identity.generation_number)) &&
         EvidenceHasExact(
             result.evidence,
             "cow_generation_number",
             std::to_string(
                 request.generation_identity.cow_generation_number)) &&
         EvidenceHasExact(result.evidence, "root_identity_bound", "true") &&
         EvidenceHasExact(result.evidence,
                          "cow_generation_identity_bound",
                          "true") &&
         EvidenceHasExact(result.evidence,
                          "cleanup_horizon_engine_bound",
                          "true") &&
         EvidenceHasExact(result.evidence,
                          "cleanup_horizon_epoch",
                          std::to_string(
                              request.generation_identity.generation_number)) &&
         EvidenceHasExact(result.evidence,
                          "contract_evidence_only",
                          "true");
}

bool RouteClassificationMatchesRequest(
    const IndexRouteFamilyClassificationResult& result,
    const SpecializedPersistentProviderClosureRequest& request) {
  return EvidenceHasExact(result.evidence,
                          "ceic_search_key",
                          "CEIC_038_LOSSY_PRUNING_FAMILY_CLASSIFICATION") &&
         EvidenceHasExact(result.evidence,
                          "family",
                          IndexFamilyName(request.family)) &&
         EvidenceHasExact(result.evidence,
                          "route",
                          IndexRouteKindName(request.route)) &&
         result.semantic == request.declaration.semantic &&
         !result.row_truth_authority &&
         !result.final_row_authority &&
         !result.enterprise_readiness_claimed;
}

bool ExactRecheckMatchesRequest(
    const EngineOwnedExactRecheckResult& result,
    const SpecializedPersistentProviderClosureRequest& request) {
  return EvidenceHasExact(result.evidence,
                          "ceic_search_key",
                          "CEIC_037_ENGINE_OWNED_EXACT_RECHECK_SERVICES") &&
         EvidenceHasExact(result.evidence,
                          "family",
                          IndexFamilyName(request.family)) &&
         EvidenceHasExact(result.evidence,
                          "route",
                          IndexRouteKindName(request.route)) &&
         EvidenceHasExact(result.evidence,
                          "row_admitted_to_executor",
                          "true") &&
         !result.ceic_039_specialized_provider_closure_claimed &&
         !result.ceic_040_runtime_metrics_claimed &&
         !result.ceic_041_crash_matrix_claimed &&
         !result.ceic_042_readiness_drift_claimed &&
         !result.all_index_readiness_claimed &&
         !result.enterprise_readiness_claimed;
}

bool DeclarationValid(const SpecializedProviderFamilyDeclaration& declaration,
                      IndexFamily family) {
  const auto expected = BuildSpecializedProviderFamilyDeclaration(family);
  return declaration.family == family &&
         declaration.provider_class == expected.provider_class &&
         declaration.semantic == expected.semantic &&
         declaration.provider_class_declared &&
         declaration.durable_provider_requirements_declared &&
         declaration.exact_recheck_requirements_declared &&
         declaration.generation_identity_requirements_declared &&
         declaration.cleanup_horizon_requirements_declared &&
         declaration.validation_repair_rebuild_requirements_declared &&
         declaration.candidate_role_declared &&
         declaration.cluster_external_provider_only &&
         !declaration.provider_search_key.empty() &&
         !declaration.provider_artifact_kind.empty();
}

bool CandidateDisciplineValid(
    const SpecializedCandidateDisciplineProof& proof,
    const IndexRouteFamilyClassificationResult& classification) {
  if (proof.final_row_authority ||
      proof.row_truth_authority ||
      proof.result_finality_authority ||
      !proof.exact_recheck_handoff_required) {
    return false;
  }

  if (classification.negative_prune_only) {
    return proof.negative_prune_only &&
           !proof.summary_segment_prune_only &&
           !proof.candidate_set_only &&
           !proof.ranking_producer &&
           !proof.seed_producer &&
           proof.false_positive_accounting_declared;
  }
  if (classification.summary_segment_prune_only) {
    return !proof.negative_prune_only &&
           proof.summary_segment_prune_only &&
           !proof.candidate_set_only &&
           !proof.ranking_producer &&
           !proof.seed_producer &&
           proof.false_positive_accounting_declared;
  }
  if (classification.produces_candidate_set) {
    if (!proof.candidate_set_only ||
        !proof.false_positive_accounting_declared) {
      return false;
    }
    if (classification.produces_ranking && !proof.ranking_producer) {
      return false;
    }
    if (classification.produces_seed_set && !proof.seed_producer) {
      return false;
    }
    return true;
  }
  return false;
}

bool ExactFallbackRecheckValid(
    const SpecializedExactFallbackRecheckProof& proof,
    const SpecializedPersistentProviderClosureRequest& request) {
  const auto& classification = request.route_classification;
  if (!proof.exact_recheck_required ||
      !proof.exact_recheck_result_consumed ||
      !proof.exact_recheck_result.ok() ||
      !ExactRecheckMatchesRequest(proof.exact_recheck_result, request) ||
      !proof.exact_source_payload_proven) {
    return false;
  }
  if (classification.requirements.exact_fallback_required &&
      (!proof.exact_fallback_required ||
       !proof.exact_fallback_proven ||
       proof.fallback_evidence_id.empty())) {
    return false;
  }
  if (!classification.requirements.exact_fallback_required &&
      proof.exact_fallback_required &&
      !proof.exact_fallback_proven) {
    return false;
  }
  if (classification.requirements.exact_rerank_required &&
      (!proof.exact_rerank_required ||
       !proof.exact_rerank_proven ||
       proof.rerank_evidence_id.empty())) {
    return false;
  }
  if (!classification.requirements.exact_rerank_required &&
      proof.exact_rerank_required &&
      !proof.exact_rerank_proven) {
    return false;
  }
  return true;
}

DiagnosticRecord MakeDiagnostic(
    Status status,
    SpecializedPersistentProviderClosureStatus closure_status,
    const SpecializedPersistentProviderClosureRequest& request,
    std::string detail) {
  DiagnosticRecord record;
  record.status = status;
  record.diagnostic_code = std::string("INDEX.SPECIALIZED_CLOSURE.") +
                           SpecializedPersistentProviderClosureStatusName(
                               closure_status);
  record.message_key = std::string("index.specialized_closure.") +
                       SpecializedPersistentProviderClosureStatusName(
                           closure_status);
  record.arguments.push_back({"family", IndexFamilyName(request.family)});
  record.arguments.push_back({"route", IndexRouteKindName(request.route)});
  record.arguments.push_back({"provider_id", request.provider_id});
  if (!detail.empty()) {
    record.arguments.push_back({"detail", std::move(detail)});
  }
  record.source_component = "sb_core_index.specialized_provider_closure";
  return record;
}

void AddBaseEvidence(SpecializedPersistentProviderClosureResult* result,
                     const SpecializedPersistentProviderClosureRequest& request) {
  AddEvidence(result, "ceic_search_key",
              "CEIC_039_SPECIALIZED_PERSISTENT_PROVIDER_CLOSURE");
  AddEvidence(result, "family", IndexFamilyName(request.family));
  AddEvidence(result, "route", IndexRouteKindName(request.route));
  AddEvidence(result, "provider_id", request.provider_id);
  AddEvidence(result, "provider_class",
              SpecializedPersistentProviderClassName(
                  request.declaration.provider_class));
  AddEvidence(result, "semantic",
              IndexFamilyRouteSemanticName(request.declaration.semantic));
  AddEvidence(result, "provider_admission_status",
              IndexProviderAdmissionStatusName(
                  request.provider_admission.admission_status));
  AddBoolEvidence(result, "provider_admission_ok",
                  request.provider_admission.ok());
  AddEvidence(result, "mga_recovery_contract_status",
              IndexMGARecoveryContractStatusName(
                  request.mga_recovery_contract.contract_status));
  AddBoolEvidence(result, "mga_recovery_contract_ok",
                  request.mga_recovery_contract.ok());
  AddEvidence(result, "route_classification_status",
              IndexRouteClassificationStatusName(
                  request.route_classification.classification_status));
  AddBoolEvidence(result, "route_classification_ok",
                  request.route_classification.ok());
  AddBoolEvidence(result, "declaration_valid",
                  DeclarationValid(request.declaration, request.family));
  AddBoolEvidence(result, "durable_provider_valid",
                  DurableProviderEvidenceValid(request.durable_provider));
  AddBoolEvidence(result, "generation_identity_valid",
                  GenerationIdentityValid(request.generation_identity));
  AddBoolEvidence(result, "cleanup_valid",
                  CleanupProofValid(request.cleanup,
                                    request.generation_identity));
  AddBoolEvidence(result, "validation_repair_rebuild_valid",
                  ValidationRepairRebuildValid(
                      request.validation_repair_rebuild,
                      request.mga_recovery_contract.recommendation));
  AddBoolEvidence(result, "candidate_discipline_valid",
                  CandidateDisciplineValid(request.candidate_discipline,
                                           request.route_classification));
  AddBoolEvidence(result, "exact_fallback_recheck_valid",
                  ExactFallbackRecheckValid(request.exact_fallback_recheck,
                                           request));
  AddEvidence(result, "provider_generation_id",
              request.generation_identity.provider_generation_id);
  AddU64Evidence(result, "generation_number",
                 request.generation_identity.generation_number);
  AddU64Evidence(result, "cow_generation_number",
                 request.generation_identity.cow_generation_number);
  AddU64Evidence(result, "cleanup_generation_floor",
                 request.generation_identity.cleanup_generation_floor);
  AddU64Evidence(result, "oldest_active_transaction_id",
                 request.generation_identity.oldest_active_transaction_id);
  AddBoolEvidence(result, "reference_local_participation",
                  request.reference_local_participation);
  AddBoolEvidence(result, "policy_local_participation",
                  request.policy_local_participation);
  AddBoolEvidence(result, "cluster_local_participation",
                  request.cluster_local_participation);
  AddBoolEvidence(result, "authority_boundary_clear",
                  SpecializedProviderAuthorityBoundaryClear(
                      request.authority_boundary));
  AddBoolEvidence(result, "successor_claims_clear",
                  SpecializedProviderSuccessorClaimsClear(
                      request.successor_claims));
  AddBoolEvidence(result, "durable_provider_evidence_claimed",
                  request.durable_provider_evidence_claimed);
  for (const auto& action :
       request.mga_recovery_contract.recommendation.stable_actions) {
    AddEvidence(result, "recommendation_action", action);
  }
  for (const auto& evidence : request.evidence) {
    AddEvidence(result, "closure_evidence", evidence);
  }
}

SpecializedPersistentProviderClosureResult BaseResult(
    const SpecializedPersistentProviderClosureRequest& request) {
  SpecializedPersistentProviderClosureResult result;
  result.status = OkStatus();
  result.durable_provider_evidence = false;
  result.specialized_provider_closure_claimed = false;
  result.ceic_040_runtime_metric_producer_claimed = false;
  result.ceic_041_crash_corruption_matrix_claimed = false;
  result.ceic_042_readiness_drift_claimed = false;
  result.all_index_readiness_claimed = false;
  result.reference_dominance_claimed = false;
  result.enterprise_readiness_claimed = false;
  result.provider_class = request.declaration.provider_class;
  result.recommendation = request.mga_recovery_contract.recommendation;
  AddBaseEvidence(&result, request);
  return result;
}

SpecializedPersistentProviderClosureResult RefuseClosure(
    const SpecializedPersistentProviderClosureRequest& request,
    SpecializedPersistentProviderClosureStatus closure_status,
    std::string detail) {
  auto result = BaseResult(request);
  result.status = RefuseStatus();
  result.admitted = false;
  result.fail_closed = true;
  result.closure_status = closure_status;
  result.diagnostic =
      MakeDiagnostic(result.status, closure_status, request, std::move(detail));
  AddEvidence(&result, "closure_status",
              SpecializedPersistentProviderClosureStatusName(closure_status));
  AddBoolEvidence(&result, "result_specialized_provider_closure_claimed",
                  result.specialized_provider_closure_claimed);
  AddBoolEvidence(&result, "result_durable_provider_evidence",
                  result.durable_provider_evidence);
  AddBoolEvidence(&result, "result_ceic_040_runtime_metric_producer_claimed",
                  result.ceic_040_runtime_metric_producer_claimed);
  AddBoolEvidence(&result, "result_ceic_041_crash_corruption_matrix_claimed",
                  result.ceic_041_crash_corruption_matrix_claimed);
  AddBoolEvidence(&result, "result_ceic_042_readiness_drift_claimed",
                  result.ceic_042_readiness_drift_claimed);
  AddBoolEvidence(&result, "result_all_index_readiness_claimed",
                  result.all_index_readiness_claimed);
  AddBoolEvidence(&result, "result_reference_dominance_claimed",
                  result.reference_dominance_claimed);
  AddBoolEvidence(&result, "result_enterprise_readiness_claimed",
                  result.enterprise_readiness_claimed);
  AddEvidence(&result, "diagnostic_code", result.diagnostic.diagnostic_code);
  return result;
}

}  // namespace

const char* SpecializedPersistentProviderClassName(
    SpecializedPersistentProviderClass provider_class) {
  switch (provider_class) {
    case SpecializedPersistentProviderClass::bitmap_candidate:
      return "bitmap_candidate";
    case SpecializedPersistentProviderClass::brin_zone_summary_prune:
      return "brin_zone_summary_prune";
    case SpecializedPersistentProviderClass::bloom_negative_prune:
      return "bloom_negative_prune";
    case SpecializedPersistentProviderClass::full_text_inverted:
      return "full_text_inverted";
    case SpecializedPersistentProviderClass::gin_multikey:
      return "gin_multikey";
    case SpecializedPersistentProviderClass::inverted_segment:
      return "inverted_segment";
    case SpecializedPersistentProviderClass::ngram_token:
      return "ngram_token";
    case SpecializedPersistentProviderClass::sparse_wand_ranking:
      return "sparse_wand_ranking";
    case SpecializedPersistentProviderClass::spatial_candidate:
      return "spatial_candidate";
    case SpecializedPersistentProviderClass::rtree_spatial:
      return "rtree_spatial";
    case SpecializedPersistentProviderClass::gist_spatial:
      return "gist_spatial";
    case SpecializedPersistentProviderClass::spgist_spatial:
      return "spgist_spatial";
    case SpecializedPersistentProviderClass::vector_exact:
      return "vector_exact";
    case SpecializedPersistentProviderClass::vector_hnsw:
      return "vector_hnsw";
    case SpecializedPersistentProviderClass::vector_ivf:
      return "vector_ivf";
    case SpecializedPersistentProviderClass::columnar_zone_summary_prune:
      return "columnar_zone_summary_prune";
    case SpecializedPersistentProviderClass::document_path:
      return "document_path";
    case SpecializedPersistentProviderClass::graph_adjacency:
      return "graph_adjacency";
    case SpecializedPersistentProviderClass::inherited_exact_provider:
      return "inherited_exact_provider";
    case SpecializedPersistentProviderClass::unsupported:
      return "unsupported";
  }
  return "unsupported";
}

const char* SpecializedPersistentProviderClosureStatusName(
    SpecializedPersistentProviderClosureStatus status) {
  switch (status) {
    case SpecializedPersistentProviderClosureStatus::
        admitted_specialized_provider_evidence:
      return "ADMITTED_SPECIALIZED_PROVIDER_EVIDENCE";
    case SpecializedPersistentProviderClosureStatus::unsupported_family:
      return "UNSUPPORTED_FAMILY";
    case SpecializedPersistentProviderClosureStatus::
        already_closed_by_prior_slice:
      return "ALREADY_CLOSED_BY_PRIOR_SLICE";
    case SpecializedPersistentProviderClosureStatus::
        reference_emulated_non_runtime:
      return "REFERENCE_EMULATED_NON_RUNTIME";
    case SpecializedPersistentProviderClosureStatus::
        policy_blocked_non_runtime:
      return "POLICY_BLOCKED_NON_RUNTIME";
    case SpecializedPersistentProviderClosureStatus::
        cluster_external_provider_only:
      return "CLUSTER_EXTERNAL_PROVIDER_ONLY";
    case SpecializedPersistentProviderClosureStatus::
        provider_admission_not_admitted:
      return "PROVIDER_ADMISSION_NOT_ADMITTED";
    case SpecializedPersistentProviderClosureStatus::
        mga_recovery_contract_not_admitted:
      return "MGA_RECOVERY_CONTRACT_NOT_ADMITTED";
    case SpecializedPersistentProviderClosureStatus::
        route_classification_not_admitted:
      return "ROUTE_CLASSIFICATION_NOT_ADMITTED";
    case SpecializedPersistentProviderClosureStatus::exact_recheck_not_admitted:
      return "EXACT_RECHECK_NOT_ADMITTED";
    case SpecializedPersistentProviderClosureStatus::
        provider_mga_identity_mismatch:
      return "PROVIDER_MGA_IDENTITY_MISMATCH";
    case SpecializedPersistentProviderClosureStatus::provider_class_mismatch:
      return "PROVIDER_CLASS_MISMATCH";
    case SpecializedPersistentProviderClosureStatus::
        durable_provider_evidence_missing:
      return "DURABLE_PROVIDER_EVIDENCE_MISSING";
    case SpecializedPersistentProviderClosureStatus::
        generation_identity_missing:
      return "GENERATION_IDENTITY_MISSING";
    case SpecializedPersistentProviderClosureStatus::
        cleanup_horizon_not_engine_bound:
      return "CLEANUP_HORIZON_NOT_ENGINE_BOUND";
    case SpecializedPersistentProviderClosureStatus::
        validation_repair_rebuild_missing:
      return "VALIDATION_REPAIR_REBUILD_MISSING";
    case SpecializedPersistentProviderClosureStatus::candidate_role_mismatch:
      return "CANDIDATE_ROLE_MISMATCH";
    case SpecializedPersistentProviderClosureStatus::
        exact_fallback_recheck_rerank_missing:
      return "EXACT_FALLBACK_RECHECK_RERANK_MISSING";
    case SpecializedPersistentProviderClosureStatus::forbidden_authority_claim:
      return "FORBIDDEN_AUTHORITY_CLAIM";
    case SpecializedPersistentProviderClosureStatus::successor_scope_overclaim:
      return "SUCCESSOR_SCOPE_OVERCLAIM";
    case SpecializedPersistentProviderClosureStatus::
        enterprise_readiness_overclaim:
      return "ENTERPRISE_READINESS_OVERCLAIM";
  }
  return "UNKNOWN";
}

bool SpecializedProviderAuthorityBoundaryClear(
    const SpecializedProviderAuthorityBoundary& boundary) {
  return !boundary.transaction_finality_authority &&
         !boundary.visibility_authority &&
         !boundary.authorization_authority &&
         !boundary.authorization_security_authority &&
         !boundary.security_authority &&
         !boundary.recovery_authority &&
         !boundary.parser_authority &&
         !boundary.reference_authority &&
         !boundary.wal_authority &&
         !boundary.provider_authority &&
         !boundary.benchmark_authority &&
         !boundary.optimizer_plan_authority &&
         !boundary.index_finality_authority &&
         !boundary.provider_finality_authority &&
         !boundary.row_truth_authority &&
         !boundary.final_row_authority &&
         !boundary.result_finality_authority &&
         !boundary.local_cluster_authority &&
         !boundary.cluster_action_authority &&
         !boundary.agent_action_authority;
}

bool SpecializedProviderSuccessorClaimsClear(
    const SpecializedProviderSuccessorClaims& claims) {
  return !claims.ceic_040_runtime_metric_producer_claimed &&
         !claims.ceic_041_crash_corruption_matrix_claimed &&
         !claims.ceic_042_readiness_drift_claimed &&
         !claims.all_index_readiness_claimed &&
         !claims.reference_dominance_claimed &&
         !claims.enterprise_readiness_claimed;
}

bool SpecializedProviderFamilyInCeic039Scope(IndexFamily family) {
  switch (family) {
    case IndexFamily::bitmap:
    case IndexFamily::brin_zone:
    case IndexFamily::bloom:
    case IndexFamily::full_text:
    case IndexFamily::gin:
    case IndexFamily::inverted:
    case IndexFamily::ngram:
    case IndexFamily::sparse_wand:
    case IndexFamily::spatial:
    case IndexFamily::rtree:
    case IndexFamily::gist:
    case IndexFamily::spgist:
    case IndexFamily::vector_exact:
    case IndexFamily::vector_hnsw:
    case IndexFamily::vector_ivf:
    case IndexFamily::columnar_zone:
    case IndexFamily::document_path:
    case IndexFamily::graph:
      return true;
    default:
      return false;
  }
}

SpecializedProviderFamilyDeclaration
BuildSpecializedProviderFamilyDeclaration(IndexFamily family) {
  SpecializedProviderFamilyDeclaration declaration;
  declaration.family = family;
  declaration.cluster_external_provider_only = true;

  switch (family) {
    case IndexFamily::bitmap:
      declaration.provider_class =
          SpecializedPersistentProviderClass::bitmap_candidate;
      declaration.semantic = IndexFamilyRouteSemantic::bitmap_candidate;
      declaration.provider_search_key = "CEIC039_BITMAP_CANDIDATE_PROVIDER";
      declaration.provider_artifact_kind = "bitmap_candidate_generation";
      break;
    case IndexFamily::brin_zone:
      declaration.provider_class =
          SpecializedPersistentProviderClass::brin_zone_summary_prune;
      declaration.semantic = IndexFamilyRouteSemantic::summary_segment_prune;
      declaration.provider_search_key = "CEIC039_BRIN_ZONE_PROVIDER";
      declaration.provider_artifact_kind = "brin_zone_summary_generation";
      break;
    case IndexFamily::bloom:
      declaration.provider_class =
          SpecializedPersistentProviderClass::bloom_negative_prune;
      declaration.semantic = IndexFamilyRouteSemantic::bloom_negative_prune;
      declaration.provider_search_key = "CEIC039_BLOOM_NEGATIVE_PRUNE_PROVIDER";
      declaration.provider_artifact_kind = "bloom_negative_prune_generation";
      break;
    case IndexFamily::full_text:
      declaration.provider_class =
          SpecializedPersistentProviderClass::full_text_inverted;
      declaration.semantic = IndexFamilyRouteSemantic::token_candidate;
      declaration.provider_search_key = "CEIC039_FULL_TEXT_PROVIDER";
      declaration.provider_artifact_kind = "full_text_segment_generation";
      break;
    case IndexFamily::gin:
      declaration.provider_class =
          SpecializedPersistentProviderClass::gin_multikey;
      declaration.semantic = IndexFamilyRouteSemantic::token_candidate;
      declaration.provider_search_key = "CEIC039_GIN_PROVIDER";
      declaration.provider_artifact_kind = "gin_multikey_generation";
      break;
    case IndexFamily::inverted:
      declaration.provider_class =
          SpecializedPersistentProviderClass::inverted_segment;
      declaration.semantic = IndexFamilyRouteSemantic::token_candidate;
      declaration.provider_search_key = "CEIC039_INVERTED_PROVIDER";
      declaration.provider_artifact_kind = "inverted_segment_generation";
      break;
    case IndexFamily::ngram:
      declaration.provider_class =
          SpecializedPersistentProviderClass::ngram_token;
      declaration.semantic = IndexFamilyRouteSemantic::token_candidate;
      declaration.provider_search_key = "CEIC039_NGRAM_PROVIDER";
      declaration.provider_artifact_kind = "ngram_token_generation";
      break;
    case IndexFamily::sparse_wand:
      declaration.provider_class =
          SpecializedPersistentProviderClass::sparse_wand_ranking;
      declaration.semantic = IndexFamilyRouteSemantic::token_ranking_candidate;
      declaration.provider_search_key = "CEIC039_SPARSE_WAND_PROVIDER";
      declaration.provider_artifact_kind = "sparse_wand_ranking_generation";
      break;
    case IndexFamily::spatial:
      declaration.provider_class =
          SpecializedPersistentProviderClass::spatial_candidate;
      declaration.semantic = IndexFamilyRouteSemantic::spatial_candidate;
      declaration.provider_search_key = "CEIC039_SPATIAL_PROVIDER";
      declaration.provider_artifact_kind = "spatial_candidate_generation";
      break;
    case IndexFamily::rtree:
      declaration.provider_class =
          SpecializedPersistentProviderClass::rtree_spatial;
      declaration.semantic = IndexFamilyRouteSemantic::spatial_candidate;
      declaration.provider_search_key = "CEIC039_RTREE_PROVIDER";
      declaration.provider_artifact_kind = "rtree_spatial_generation";
      break;
    case IndexFamily::gist:
      declaration.provider_class =
          SpecializedPersistentProviderClass::gist_spatial;
      declaration.semantic = IndexFamilyRouteSemantic::spatial_candidate;
      declaration.provider_search_key = "CEIC039_GIST_PROVIDER";
      declaration.provider_artifact_kind = "gist_spatial_generation";
      break;
    case IndexFamily::spgist:
      declaration.provider_class =
          SpecializedPersistentProviderClass::spgist_spatial;
      declaration.semantic = IndexFamilyRouteSemantic::spatial_candidate;
      declaration.provider_search_key = "CEIC039_SPGIST_PROVIDER";
      declaration.provider_artifact_kind = "spgist_spatial_generation";
      break;
    case IndexFamily::vector_exact:
      declaration.provider_class =
          SpecializedPersistentProviderClass::vector_exact;
      declaration.semantic = IndexFamilyRouteSemantic::vector_exact_candidate;
      declaration.provider_search_key = "CEIC039_VECTOR_EXACT_PROVIDER";
      declaration.provider_artifact_kind = "vector_exact_generation";
      break;
    case IndexFamily::vector_hnsw:
      declaration.provider_class =
          SpecializedPersistentProviderClass::vector_hnsw;
      declaration.semantic =
          IndexFamilyRouteSemantic::vector_approximate_candidate;
      declaration.provider_search_key = "CEIC039_VECTOR_HNSW_PROVIDER";
      declaration.provider_artifact_kind = "vector_hnsw_generation";
      break;
    case IndexFamily::vector_ivf:
      declaration.provider_class =
          SpecializedPersistentProviderClass::vector_ivf;
      declaration.semantic =
          IndexFamilyRouteSemantic::vector_approximate_candidate;
      declaration.provider_search_key = "CEIC039_VECTOR_IVF_PROVIDER";
      declaration.provider_artifact_kind = "vector_ivf_generation";
      break;
    case IndexFamily::columnar_zone:
      declaration.provider_class =
          SpecializedPersistentProviderClass::columnar_zone_summary_prune;
      declaration.semantic = IndexFamilyRouteSemantic::summary_segment_prune;
      declaration.provider_search_key = "CEIC039_COLUMNAR_ZONE_PROVIDER";
      declaration.provider_artifact_kind = "columnar_zone_generation";
      break;
    case IndexFamily::document_path:
      declaration.provider_class =
          SpecializedPersistentProviderClass::document_path;
      declaration.semantic = IndexFamilyRouteSemantic::document_path_candidate;
      declaration.provider_search_key = "CEIC039_DOCUMENT_PATH_PROVIDER";
      declaration.provider_artifact_kind = "document_path_generation";
      break;
    case IndexFamily::graph:
      declaration.provider_class =
          SpecializedPersistentProviderClass::graph_adjacency;
      declaration.semantic = IndexFamilyRouteSemantic::graph_seed_candidate;
      declaration.provider_search_key = "CEIC039_GRAPH_PROVIDER";
      declaration.provider_artifact_kind = "graph_adjacency_generation";
      break;
    case IndexFamily::btree:
    case IndexFamily::unique_btree:
    case IndexFamily::expression:
    case IndexFamily::partial:
    case IndexFamily::covering:
    case IndexFamily::hash:
      declaration.provider_class =
          SpecializedPersistentProviderClass::inherited_exact_provider;
      declaration.semantic =
          family == IndexFamily::hash
              ? IndexFamilyRouteSemantic::hash_equality_candidate
              : IndexFamilyRouteSemantic::exact_candidate;
      declaration.provider_search_key = "CEIC039_PRIOR_SLICE_PROVIDER";
      declaration.provider_artifact_kind = "prior_slice_closure";
      return declaration;
    default:
      return declaration;
  }

  declaration.provider_class_declared = true;
  declaration.durable_provider_requirements_declared = true;
  declaration.exact_recheck_requirements_declared = true;
  declaration.generation_identity_requirements_declared = true;
  declaration.cleanup_horizon_requirements_declared = true;
  declaration.validation_repair_rebuild_requirements_declared = true;
  declaration.candidate_role_declared = true;
  return declaration;
}

SpecializedPersistentProviderClosureResult
AdmitSpecializedPersistentProviderClosure(
    const SpecializedPersistentProviderClosureRequest& request) {
  const auto* descriptor = FindBuiltinIndexFamily(request.family);
  if (descriptor == nullptr || request.family == IndexFamily::unknown) {
    return RefuseClosure(
        request,
        SpecializedPersistentProviderClosureStatus::unsupported_family,
        "family is not registered as a built-in index family");
  }
  if (request.family == IndexFamily::reference_emulated ||
      descriptor->persistence == IndexPersistenceClass::reference_emulated) {
    return RefuseClosure(
        request,
        SpecializedPersistentProviderClosureStatus::reference_emulated_non_runtime,
        "reference-emulated index mappings are non-runtime non-authority");
  }
  if (request.family == IndexFamily::policy_blocked ||
      descriptor->persistence == IndexPersistenceClass::policy_blocked) {
    return RefuseClosure(
        request,
        SpecializedPersistentProviderClosureStatus::policy_blocked_non_runtime,
        "policy-blocked index families cannot be runtime providers");
  }
  if (descriptor->persistence != IndexPersistenceClass::persistent) {
    return RefuseClosure(
        request,
        SpecializedPersistentProviderClosureStatus::unsupported_family,
        "CEIC-039 closes persistent specialized providers only");
  }
  if (PriorSliceFamily(request.family) &&
      !SpecializedProviderFamilyInCeic039Scope(request.family)) {
    return RefuseClosure(
        request,
        SpecializedPersistentProviderClosureStatus::already_closed_by_prior_slice,
        "B-tree-backed and hash providers are closed by CEIC-033 CEIC-034 and CEIC-035 rather than CEIC-039");
  }
  if (!SpecializedProviderFamilyInCeic039Scope(request.family)) {
    return RefuseClosure(
        request,
        SpecializedPersistentProviderClosureStatus::unsupported_family,
        "family is not in CEIC-039 specialized persistent provider scope");
  }
  if (request.cluster_local_participation ||
      request.authority_boundary.local_cluster_authority ||
      request.authority_boundary.cluster_action_authority ||
      !request.declaration.cluster_external_provider_only) {
    return RefuseClosure(
        request,
        SpecializedPersistentProviderClosureStatus::
            cluster_external_provider_only,
        "cluster specialized provider closure is external-provider-only and cannot be local runtime authority");
  }
  if (request.reference_local_participation ||
      request.policy_local_participation ||
      !SpecializedProviderAuthorityBoundaryClear(request.authority_boundary)) {
    return RefuseClosure(
        request,
        SpecializedPersistentProviderClosureStatus::forbidden_authority_claim,
        "specialized provider closure evidence must not claim reference policy row-truth result-finality transaction finality visibility authorization security recovery parser WAL provider-finality benchmark optimizer plan index finality local-cluster cluster-action or agent-action authority");
  }
  if (request.successor_claims.enterprise_readiness_claimed) {
    return RefuseClosure(
        request,
        SpecializedPersistentProviderClosureStatus::
            enterprise_readiness_overclaim,
        "CEIC-039 may admit specialized provider evidence but must not claim enterprise readiness");
  }
  if (!SpecializedProviderSuccessorClaimsClear(request.successor_claims)) {
    return RefuseClosure(
        request,
        SpecializedPersistentProviderClosureStatus::successor_scope_overclaim,
        "CEIC-040 runtime metrics CEIC-041 crash/corruption matrix CEIC-042 readiness drift all-index readiness and reference dominance remain separate successor scope");
  }
  if (!DeclarationValid(request.declaration, request.family)) {
    return RefuseClosure(
        request,
        SpecializedPersistentProviderClosureStatus::provider_class_mismatch,
        "specialized provider declaration must match the family semantic class and declare durable recheck generation cleanup validation and candidate role requirements");
  }
  if (!request.provider_admission.ok() ||
      request.provider_admission.admission_status !=
          IndexProviderAdmissionStatus::admitted) {
    return RefuseClosure(
        request,
        SpecializedPersistentProviderClosureStatus::
            provider_admission_not_admitted,
        "CEIC-031 provider admission result must be admitted before CEIC-039 specialized provider closure");
  }
  if (!request.mga_recovery_contract.ok() ||
      request.mga_recovery_contract.contract_status !=
          IndexMGARecoveryContractStatus::admitted_contract_evidence) {
    return RefuseClosure(
        request,
        SpecializedPersistentProviderClosureStatus::
            mga_recovery_contract_not_admitted,
        "CEIC-032 MGA recovery contract result must be admitted before CEIC-039 specialized provider closure");
  }
  if (!request.route_classification.ok() ||
      request.route_classification.classification_status !=
          IndexRouteClassificationStatus::classified) {
    return RefuseClosure(
        request,
        SpecializedPersistentProviderClosureStatus::
            route_classification_not_admitted,
        "CEIC-038 route/family classification must be admitted before CEIC-039 specialized provider closure");
  }
  if (!request.exact_fallback_recheck.exact_recheck_result.ok() ||
      request.exact_fallback_recheck.exact_recheck_result.recheck_status !=
          EngineOwnedExactRecheckStatus::admitted_to_executor) {
    return RefuseClosure(
        request,
        SpecializedPersistentProviderClosureStatus::exact_recheck_not_admitted,
        "CEIC-037 engine-owned exact recheck result must be admitted before CEIC-039 closure");
  }
  if (!ProviderAdmissionMatchesRequest(request.provider_admission, request) ||
      !MGARecoveryContractMatchesRequest(request.mga_recovery_contract,
                                         request) ||
      !RouteClassificationMatchesRequest(request.route_classification,
                                         request)) {
    return RefuseClosure(
        request,
        SpecializedPersistentProviderClosureStatus::
            provider_mga_identity_mismatch,
        "CEIC-031 CEIC-032 and CEIC-038 evidence must match requested family route provider generation and semantic identity");
  }
  if (!request.durable_provider_evidence_claimed ||
      !DurableProviderEvidenceValid(request.durable_provider)) {
    return RefuseClosure(
        request,
        SpecializedPersistentProviderClosureStatus::
            durable_provider_evidence_missing,
        "family-specific persistent physical payload storage open/reopen integrity and artifact-format evidence is required");
  }
  if (!GenerationIdentityValid(request.generation_identity)) {
    return RefuseClosure(
        request,
        SpecializedPersistentProviderClosureStatus::
            generation_identity_missing,
        "specialized provider closure requires COW generation publish root/segment identity cleanup identity and durable MGA publish order evidence");
  }
  if (!CleanupProofValid(request.cleanup, request.generation_identity)) {
    return RefuseClosure(
        request,
        SpecializedPersistentProviderClosureStatus::
            cleanup_horizon_not_engine_bound,
        "cleanup proof must use engine MGA horizon evidence and remain provider evidence only");
  }
  if (!ValidationRepairRebuildValid(
          request.validation_repair_rebuild,
          request.mga_recovery_contract.recommendation)) {
    return RefuseClosure(
        request,
        SpecializedPersistentProviderClosureStatus::
            validation_repair_rebuild_missing,
        "validation repair rebuild and deterministic recommendation evidence must match the CEIC-032 result");
  }
  if (!CandidateDisciplineValid(request.candidate_discipline,
                                request.route_classification)) {
    return RefuseClosure(
        request,
        SpecializedPersistentProviderClosureStatus::candidate_role_mismatch,
        "negative-prune summary-prune candidate ranking and seed roles must match CEIC-038 and must not claim row truth or result finality");
  }
  if (!ExactFallbackRecheckValid(request.exact_fallback_recheck, request)) {
    return RefuseClosure(
        request,
        SpecializedPersistentProviderClosureStatus::
            exact_fallback_recheck_rerank_missing,
        "CEIC-039 requires CEIC-037 exact recheck consumption plus exact fallback and exact rerank proof when CEIC-038 requires them");
  }

  auto result = BaseResult(request);
  result.status = OkStatus();
  result.admitted = true;
  result.fail_closed = false;
  result.specialized_provider_closure_claimed = true;
  result.durable_provider_evidence = true;
  result.ceic_040_runtime_metric_producer_claimed = false;
  result.ceic_041_crash_corruption_matrix_claimed = false;
  result.ceic_042_readiness_drift_claimed = false;
  result.all_index_readiness_claimed = false;
  result.reference_dominance_claimed = false;
  result.enterprise_readiness_claimed = false;
  result.closure_status =
      SpecializedPersistentProviderClosureStatus::
          admitted_specialized_provider_evidence;
  result.diagnostic =
      MakeDiagnostic(result.status,
                     result.closure_status,
                     request,
                     "specialized persistent provider evidence admitted without runtime metrics crash matrix readiness drift all-index reference dominance or enterprise readiness");
  AddEvidence(&result, "closure_status",
              SpecializedPersistentProviderClosureStatusName(
                  result.closure_status));
  AddBoolEvidence(&result, "result_specialized_provider_closure_claimed",
                  result.specialized_provider_closure_claimed);
  AddBoolEvidence(&result, "result_durable_provider_evidence",
                  result.durable_provider_evidence);
  AddBoolEvidence(&result, "result_ceic_040_runtime_metric_producer_claimed",
                  result.ceic_040_runtime_metric_producer_claimed);
  AddBoolEvidence(&result, "result_ceic_041_crash_corruption_matrix_claimed",
                  result.ceic_041_crash_corruption_matrix_claimed);
  AddBoolEvidence(&result, "result_ceic_042_readiness_drift_claimed",
                  result.ceic_042_readiness_drift_claimed);
  AddBoolEvidence(&result, "result_all_index_readiness_claimed",
                  result.all_index_readiness_claimed);
  AddBoolEvidence(&result, "result_reference_dominance_claimed",
                  result.reference_dominance_claimed);
  AddBoolEvidence(&result, "result_enterprise_readiness_claimed",
                  result.enterprise_readiness_claimed);
  AddEvidence(&result, "provider_closure_boundary",
              "specialized_provider_evidence_only_pending_CEIC_040_CEIC_041_CEIC_042");
  AddEvidence(&result, "diagnostic_code", result.diagnostic.diagnostic_code);
  return result;
}

}  // namespace scratchbird::core::index
