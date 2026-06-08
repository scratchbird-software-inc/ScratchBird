// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "index_recheck.hpp"

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

void AddEvidence(EngineOwnedExactRecheckResult* result,
                 std::string key,
                 std::string value) {
  result->evidence.push_back(std::move(key) + "=" + std::move(value));
}

void AddBoolEvidence(EngineOwnedExactRecheckResult* result,
                     const char* key,
                     bool value) {
  AddEvidence(result, key, BoolText(value));
}

void AddU64Evidence(EngineOwnedExactRecheckResult* result,
                    const char* key,
                    u64 value) {
  AddEvidence(result, key, std::to_string(value));
}

bool LocatorUuidsValid(const IndexRowLocator& locator) {
  return locator.table_uuid.valid() && locator.row_uuid.valid() &&
         locator.version_uuid.valid();
}

bool LossyOrApproximateCandidate(
    const EngineOwnedExactRecheckRequest& request) {
  return request.candidate.key.lossy ||
         request.route_evidence.lossy_candidate ||
         request.route_evidence.approximate_candidate;
}

bool ExactSourceProofPresent(
    const EngineOwnedExactRecheckRouteEvidence& route,
    const EngineOwnedExactRecheckProof& proof) {
  if (!proof.exact_source_row_verified ||
      !proof.exact_source_payload_verified ||
      proof.exact_source_proof_id.empty()) {
    return false;
  }
  if (route.vector_payload_required && !proof.exact_vector_payload_verified) {
    return false;
  }
  if (route.document_payload_required && !proof.exact_document_payload_verified) {
    return false;
  }
  if (route.text_payload_required && !proof.exact_text_payload_verified) {
    return false;
  }
  if (route.graph_payload_required && !proof.exact_graph_payload_verified) {
    return false;
  }
  return true;
}

DiagnosticRecord MakeExactRecheckDiagnostic(
    Status status,
    EngineOwnedExactRecheckStatus recheck_status,
    const EngineOwnedExactRecheckRequest& request,
    std::string detail) {
  DiagnosticRecord record;
  record.status = status;
  record.diagnostic_code = std::string("INDEX.EXACT_RECHECK.") +
                           EngineOwnedExactRecheckStatusName(recheck_status);
  record.message_key = std::string("index.exact_recheck.") +
                       EngineOwnedExactRecheckStatusName(recheck_status);
  record.arguments.push_back(
      {"family", IndexFamilyName(request.route_evidence.family)});
  record.arguments.push_back(
      {"route", IndexRouteKindName(request.route_evidence.route)});
  if (!detail.empty()) {
    record.arguments.push_back({"detail", std::move(detail)});
  }
  record.source_component = "sb_core_index.engine_owned_exact_recheck";
  return record;
}

void AddBaseEvidence(EngineOwnedExactRecheckResult* result,
                     const EngineOwnedExactRecheckRequest& request) {
  AddEvidence(result, "ceic_search_key",
              "CEIC_037_ENGINE_OWNED_EXACT_RECHECK_SERVICES");
  AddEvidence(result, "family", IndexFamilyName(request.route_evidence.family));
  AddEvidence(result, "route", IndexRouteKindName(request.route_evidence.route));
  AddBoolEvidence(result, "candidate_route",
                  request.route_evidence.candidate_route);
  AddBoolEvidence(result, "lossy_candidate",
                  request.route_evidence.lossy_candidate ||
                      request.candidate.key.lossy);
  AddBoolEvidence(result, "approximate_candidate",
                  request.route_evidence.approximate_candidate);
  AddBoolEvidence(result, "exact_fallback_available",
                  request.route_evidence.exact_fallback_available);
  AddBoolEvidence(result, "exact_rerank_required",
                  request.route_evidence.exact_rerank_required);
  AddBoolEvidence(result, "exact_rerank_proven",
                  request.route_evidence.exact_rerank_proven);
  AddBoolEvidence(result, "mga_visibility_proven",
                  request.proof.mga_visibility_proven);
  AddBoolEvidence(result, "mga_inventory_proof_present",
                  request.proof.mga_inventory_proof_present);
  AddBoolEvidence(result, "mga_snapshot_proof_present",
                  request.proof.mga_snapshot_proof_present);
  AddBoolEvidence(result, "mga_snapshot_fresh",
                  request.proof.mga_snapshot_fresh);
  AddEvidence(result, "mga_inventory_proof_id",
              request.proof.mga_inventory_proof_id);
  AddEvidence(result, "mga_snapshot_proof_id",
              request.proof.mga_snapshot_proof_id);
  AddBoolEvidence(result, "security_context_present",
                  request.proof.security_context_present);
  AddBoolEvidence(result, "authorization_proven",
                  request.proof.authorization_proven);
  AddEvidence(result, "security_context_id",
              request.proof.security_context_id);
  AddEvidence(result, "authorization_proof_id",
              request.proof.authorization_proof_id);
  AddBoolEvidence(result, "predicate_exactness_proven",
                  request.proof.predicate_exactness_proven);
  AddEvidence(result, "predicate_proof_id", request.proof.predicate_proof_id);
  AddBoolEvidence(result, "exact_source_row_verified",
                  request.proof.exact_source_row_verified);
  AddBoolEvidence(result, "exact_source_payload_verified",
                  request.proof.exact_source_payload_verified);
  AddBoolEvidence(result, "exact_vector_payload_verified",
                  request.proof.exact_vector_payload_verified);
  AddBoolEvidence(result, "exact_document_payload_verified",
                  request.proof.exact_document_payload_verified);
  AddBoolEvidence(result, "exact_text_payload_verified",
                  request.proof.exact_text_payload_verified);
  AddBoolEvidence(result, "exact_graph_payload_verified",
                  request.proof.exact_graph_payload_verified);
  AddEvidence(result, "exact_source_proof_id",
              request.proof.exact_source_proof_id);
  AddEvidence(result, "exact_fallback_proof_id",
              request.proof.exact_fallback_proof_id);
  AddEvidence(result, "exact_rerank_proof_id",
              request.proof.exact_rerank_proof_id);
  AddU64Evidence(result, "local_transaction_id",
                 request.candidate.locator.local_transaction_id);
  AddBoolEvidence(result, "engine_owned_evidence",
                  result->engine_owned_evidence);
  AddBoolEvidence(result, "transaction_finality_authority",
                  result->transaction_finality_authority);
  AddBoolEvidence(result, "visibility_authority",
                  result->visibility_authority);
  AddBoolEvidence(result, "authorization_authority",
                  result->authorization_authority);
  AddBoolEvidence(result, "security_authority",
                  result->security_authority);
  AddBoolEvidence(result, "recovery_authority", result->recovery_authority);
  AddBoolEvidence(result, "parser_authority", result->parser_authority);
  AddBoolEvidence(result, "donor_authority", result->donor_authority);
  AddBoolEvidence(result, "wal_authority", result->wal_authority);
  AddBoolEvidence(result, "provider_authority", result->provider_authority);
  AddBoolEvidence(result, "benchmark_authority", result->benchmark_authority);
  AddBoolEvidence(result, "optimizer_plan_authority",
                  result->optimizer_plan_authority);
  AddBoolEvidence(result, "index_finality_authority",
                  result->index_finality_authority);
  AddBoolEvidence(result, "cluster_action_authority",
                  result->cluster_action_authority);
  AddBoolEvidence(result, "agent_action_authority",
                  result->agent_action_authority);
  AddBoolEvidence(result, "ceic_038_family_classification_claimed",
                  result->ceic_038_family_classification_claimed);
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
  AddBoolEvidence(result, "enterprise_readiness_claimed",
                  result->enterprise_readiness_claimed);
}

EngineOwnedExactRecheckResult BaseResult(
    const EngineOwnedExactRecheckRequest& request) {
  EngineOwnedExactRecheckResult result;
  result.status = OkStatus();
  result.engine_owned_evidence = true;
  result.transaction_finality_authority = false;
  result.visibility_authority = false;
  result.authorization_authority = false;
  result.security_authority = false;
  result.recovery_authority = false;
  result.parser_authority = false;
  result.donor_authority = false;
  result.wal_authority = false;
  result.provider_authority = false;
  result.benchmark_authority = false;
  result.optimizer_plan_authority = false;
  result.index_finality_authority = false;
  result.cluster_action_authority = false;
  result.agent_action_authority = false;
  result.ceic_038_family_classification_claimed = false;
  result.ceic_039_specialized_provider_closure_claimed = false;
  result.ceic_040_runtime_metrics_claimed = false;
  result.ceic_041_crash_matrix_claimed = false;
  result.ceic_042_readiness_drift_claimed = false;
  result.all_index_readiness_claimed = false;
  result.enterprise_readiness_claimed = false;
  AddBaseEvidence(&result, request);
  return result;
}

EngineOwnedExactRecheckResult RefuseExactRecheck(
    const EngineOwnedExactRecheckRequest& request,
    EngineOwnedExactRecheckStatus recheck_status,
    std::string detail) {
  auto result = BaseResult(request);
  result.status = RefuseStatus();
  result.admitted = false;
  result.fail_closed = true;
  result.row_admitted_to_executor = false;
  result.recheck_status = recheck_status;
  result.diagnostic = MakeExactRecheckDiagnostic(
      result.status, recheck_status, request, std::move(detail));
  AddEvidence(&result, "row_admitted_to_executor", "false");
  AddEvidence(&result, "recheck_status",
              EngineOwnedExactRecheckStatusName(recheck_status));
  AddEvidence(&result, "diagnostic_code", result.diagnostic.diagnostic_code);
  return result;
}
}  // namespace

IndexCandidatePipelineResult ApplyIndexRecheckPolicy(std::vector<IndexCandidate> candidates,
                                                     const IndexRecheckPolicy& policy) {
  IndexCandidatePipelineResult result;
  result.status = OkStatus();
  for (auto& candidate : candidates) {
    result.metrics.candidates++;
    const bool needs_recheck = candidate.key.requires_recheck || candidate.key.lossy ||
                               policy.require_mga_visibility || policy.require_predicate_match ||
                               policy.require_security_visibility;
    if (needs_recheck) {
      result.metrics.rechecks++;
    }
    const bool mga_ok = !policy.require_mga_visibility || candidate.mga_visible;
    const bool predicate_ok = !policy.require_predicate_match || candidate.predicate_exact ||
                              (policy.accept_lossy_without_exact_predicate && candidate.key.lossy);
    const bool security_ok = !policy.require_security_visibility || candidate.security_visible;
    if (mga_ok && predicate_ok && security_ok) {
      result.metrics.visible++;
      result.accepted.push_back(std::move(candidate));
    } else {
      result.rejected.push_back(std::move(candidate));
    }
  }
  return result;
}

const char* EngineOwnedExactRecheckStatusName(
    EngineOwnedExactRecheckStatus status) {
  switch (status) {
    case EngineOwnedExactRecheckStatus::admitted_to_executor:
      return "ADMITTED_TO_EXECUTOR";
    case EngineOwnedExactRecheckStatus::missing_locator_uuid:
      return "MISSING_LOCATOR_UUID";
    case EngineOwnedExactRecheckStatus::missing_local_transaction_id:
      return "MISSING_LOCAL_TRANSACTION_ID";
    case EngineOwnedExactRecheckStatus::missing_mga_inventory_proof:
      return "MISSING_MGA_INVENTORY_PROOF";
    case EngineOwnedExactRecheckStatus::missing_mga_snapshot_proof:
      return "MISSING_MGA_SNAPSHOT_PROOF";
    case EngineOwnedExactRecheckStatus::stale_mga_snapshot:
      return "STALE_MGA_SNAPSHOT";
    case EngineOwnedExactRecheckStatus::missing_security_context:
      return "MISSING_SECURITY_CONTEXT";
    case EngineOwnedExactRecheckStatus::missing_authorization_proof:
      return "MISSING_AUTHORIZATION_PROOF";
    case EngineOwnedExactRecheckStatus::missing_predicate_proof:
      return "MISSING_PREDICATE_PROOF";
    case EngineOwnedExactRecheckStatus::missing_exact_source_proof:
      return "MISSING_EXACT_SOURCE_PROOF";
    case EngineOwnedExactRecheckStatus::
        lossy_or_approximate_without_exact_fallback:
      return "LOSSY_OR_APPROXIMATE_WITHOUT_EXACT_FALLBACK";
    case EngineOwnedExactRecheckStatus::missing_required_exact_rerank:
      return "MISSING_REQUIRED_EXACT_RERANK";
    case EngineOwnedExactRecheckStatus::forbidden_authority_claim:
      return "FORBIDDEN_AUTHORITY_CLAIM";
    case EngineOwnedExactRecheckStatus::external_authority_refused:
      return "EXTERNAL_AUTHORITY_REFUSED";
    case EngineOwnedExactRecheckStatus::successor_or_enterprise_overclaim:
      return "SUCCESSOR_OR_ENTERPRISE_OVERCLAIM";
    case EngineOwnedExactRecheckStatus::missing_route_evidence:
      return "MISSING_ROUTE_EVIDENCE";
    case EngineOwnedExactRecheckStatus::mga_visibility_recheck_failed:
      return "MGA_VISIBILITY_RECHECK_FAILED";
    case EngineOwnedExactRecheckStatus::security_recheck_failed:
      return "SECURITY_RECHECK_FAILED";
    case EngineOwnedExactRecheckStatus::predicate_recheck_failed:
      return "PREDICATE_RECHECK_FAILED";
  }
  return "UNKNOWN";
}

bool EngineOwnedExactRecheckAuthorityBoundaryClear(
    const EngineOwnedExactRecheckAuthorityBoundary& boundary) {
  return !boundary.transaction_finality_authority &&
         !boundary.visibility_authority &&
         !boundary.authorization_authority &&
         !boundary.security_authority &&
         !boundary.recovery_authority &&
         !boundary.parser_authority &&
         !boundary.donor_authority &&
         !boundary.wal_authority &&
         !boundary.provider_authority &&
         !boundary.benchmark_authority &&
         !boundary.optimizer_plan_authority &&
         !boundary.index_finality_authority &&
         !boundary.cluster_action_authority &&
         !boundary.local_cluster_authority &&
         !boundary.agent_action_authority;
}

bool EngineOwnedExactRecheckSuccessorClaimsClear(
    const EngineOwnedExactRecheckSuccessorClaims& claims) {
  return !claims.ceic_038_family_classification_claimed &&
         !claims.ceic_039_specialized_provider_closure_claimed &&
         !claims.ceic_040_runtime_metrics_claimed &&
         !claims.ceic_041_crash_matrix_claimed &&
         !claims.ceic_042_readiness_drift_claimed &&
         !claims.all_index_readiness_claimed &&
         !claims.enterprise_readiness_claimed;
}

EngineOwnedExactRecheckResult ApplyEngineOwnedExactRecheck(
    const EngineOwnedExactRecheckRequest& request) {
  if (request.route_evidence.family == IndexFamily::unknown ||
      request.route_evidence.route == IndexRouteKind::unknown ||
      !request.route_evidence.candidate_route) {
    return RefuseExactRecheck(
        request,
        EngineOwnedExactRecheckStatus::missing_route_evidence,
        "exact recheck admission requires an engine-known candidate route and family");
  }
  if (!LocatorUuidsValid(request.candidate.locator)) {
    return RefuseExactRecheck(
        request,
        EngineOwnedExactRecheckStatus::missing_locator_uuid,
        "table row and version UUIDs are required before row admission");
  }
  if (request.candidate.locator.local_transaction_id == 0) {
    return RefuseExactRecheck(
        request,
        EngineOwnedExactRecheckStatus::missing_local_transaction_id,
        "local transaction id is required for engine MGA visibility recheck");
  }
  if (request.authority_boundary.donor_authority ||
      request.authority_boundary.parser_authority ||
      request.authority_boundary.wal_authority ||
      request.authority_boundary.provider_authority ||
      request.authority_boundary.local_cluster_authority) {
    return RefuseExactRecheck(
        request,
        EngineOwnedExactRecheckStatus::external_authority_refused,
        "donor parser WAL provider and local-cluster surfaces cannot provide exact recheck authority");
  }
  if (!EngineOwnedExactRecheckAuthorityBoundaryClear(
          request.authority_boundary)) {
    return RefuseExactRecheck(
        request,
        EngineOwnedExactRecheckStatus::forbidden_authority_claim,
        "exact recheck evidence must not claim transaction finality visibility authorization security recovery parser donor WAL provider benchmark optimizer plan index finality cluster or agent authority");
  }
  if (!EngineOwnedExactRecheckSuccessorClaimsClear(
          request.successor_claims)) {
    return RefuseExactRecheck(
        request,
        EngineOwnedExactRecheckStatus::successor_or_enterprise_overclaim,
        "CEIC-037 cannot claim family classification specialized provider closure runtime metrics crash matrix drift all-index or enterprise readiness");
  }
  if (!request.proof.mga_visibility_proven ||
      !request.proof.mga_inventory_proof_present ||
      request.proof.mga_inventory_proof_id.empty()) {
    return RefuseExactRecheck(
        request,
        EngineOwnedExactRecheckStatus::missing_mga_inventory_proof,
        "engine MGA visibility and transaction inventory proof are required");
  }
  if (!request.proof.mga_snapshot_proof_present ||
      request.proof.mga_snapshot_proof_id.empty()) {
    return RefuseExactRecheck(
        request,
        EngineOwnedExactRecheckStatus::missing_mga_snapshot_proof,
        "engine MGA snapshot proof is required");
  }
  if (!request.proof.mga_snapshot_fresh) {
    return RefuseExactRecheck(
        request,
        EngineOwnedExactRecheckStatus::stale_mga_snapshot,
        "engine MGA snapshot proof must be fresh for the candidate recheck");
  }
  if (!request.candidate.mga_visible) {
    return RefuseExactRecheck(
        request,
        EngineOwnedExactRecheckStatus::mga_visibility_recheck_failed,
        "candidate must pass engine MGA visibility recheck before row admission");
  }
  if (!request.proof.security_context_present ||
      request.proof.security_context_id.empty()) {
    return RefuseExactRecheck(
        request,
        EngineOwnedExactRecheckStatus::missing_security_context,
        "engine security context is required before row admission");
  }
  if (!request.proof.authorization_proven ||
      request.proof.authorization_proof_id.empty()) {
    return RefuseExactRecheck(
        request,
        EngineOwnedExactRecheckStatus::missing_authorization_proof,
        "engine authorization proof is required before row admission");
  }
  if (!request.candidate.security_visible) {
    return RefuseExactRecheck(
        request,
        EngineOwnedExactRecheckStatus::security_recheck_failed,
        "candidate must pass engine security visibility recheck before row admission");
  }
  if (!request.proof.predicate_exactness_proven ||
      request.proof.predicate_proof_id.empty()) {
    return RefuseExactRecheck(
        request,
        EngineOwnedExactRecheckStatus::missing_predicate_proof,
        "predicate exactness proof is required before row admission");
  }
  if (!request.candidate.predicate_exact) {
    return RefuseExactRecheck(
        request,
        EngineOwnedExactRecheckStatus::predicate_recheck_failed,
        "candidate must pass engine predicate exactness recheck before row admission");
  }
  if (!ExactSourceProofPresent(request.route_evidence, request.proof)) {
    return RefuseExactRecheck(
        request,
        EngineOwnedExactRecheckStatus::missing_exact_source_proof,
        "exact source row and payload proof are required before row admission");
  }
  if (LossyOrApproximateCandidate(request) &&
      (!request.route_evidence.exact_fallback_available ||
       request.proof.exact_fallback_proof_id.empty())) {
    return RefuseExactRecheck(
        request,
        EngineOwnedExactRecheckStatus::
            lossy_or_approximate_without_exact_fallback,
        "lossy or approximate candidates require an exact fallback proof");
  }
  if (request.route_evidence.exact_rerank_required &&
      (!request.route_evidence.exact_rerank_proven ||
       request.proof.exact_rerank_proof_id.empty())) {
    return RefuseExactRecheck(
        request,
        EngineOwnedExactRecheckStatus::missing_required_exact_rerank,
        "approximate candidate route requires exact rerank proof");
  }

  auto result = BaseResult(request);
  result.status = OkStatus();
  result.admitted = true;
  result.fail_closed = false;
  result.row_admitted_to_executor = true;
  result.recheck_status = EngineOwnedExactRecheckStatus::admitted_to_executor;
  result.diagnostic = MakeExactRecheckDiagnostic(
      result.status,
      result.recheck_status,
      request,
      "candidate admitted to executor after engine-owned exact recheck");
  AddEvidence(&result, "row_admitted_to_executor", "true");
  AddEvidence(&result, "recheck_status",
              EngineOwnedExactRecheckStatusName(result.recheck_status));
  AddEvidence(&result, "diagnostic_code", result.diagnostic.diagnostic_code);
  return result;
}

}  // namespace scratchbird::core::index
