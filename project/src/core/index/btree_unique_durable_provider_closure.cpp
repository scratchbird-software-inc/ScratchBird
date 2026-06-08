// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "btree_unique_durable_provider_closure.hpp"

// CEIC_033_BTREE_UNIQUE_DURABLE_PROVIDER_CLOSURE

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

void AddEvidence(BtreeUniqueDurableProviderClosureResult* result,
                 std::string key,
                 std::string value) {
  result->evidence.push_back(std::move(key) + "=" + std::move(value));
}

void AddBoolEvidence(BtreeUniqueDurableProviderClosureResult* result,
                     const char* key,
                     bool value) {
  AddEvidence(result, key, BoolText(value));
}

void AddU64Evidence(BtreeUniqueDurableProviderClosureResult* result,
                    const char* key,
                    u64 value) {
  AddEvidence(result, key, std::to_string(value));
}

bool SupportedBtreeFamily(IndexFamily family) {
  return family == IndexFamily::btree || family == IndexFamily::unique_btree;
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

bool ProviderAdmissionMatchesRequest(
    const IndexProviderAdmissionResult& result,
    const BtreeUniqueDurableProviderClosureRequest& request) {
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
                          request.provider_id);
}

bool MGARecoveryContractMatchesRequest(
    const IndexMGARecoveryContractResult& result,
    const BtreeUniqueDurableProviderClosureRequest& request) {
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
                          "generation_publish_state",
                          IndexGenerationPublishStateName(
                              IndexGenerationPublishState::published)) &&
         EvidenceHasExact(result.evidence,
                          "generation_number",
                          std::to_string(request.generation_root.generation_number)) &&
         EvidenceHasExact(result.evidence,
                          "cow_generation_number",
                          std::to_string(
                              request.generation_root.cow_generation_number)) &&
         EvidenceHasExact(result.evidence, "root_identity_bound", "true") &&
         EvidenceHasExact(result.evidence,
                          "cow_generation_identity_bound",
                          "true") &&
         EvidenceHasExact(result.evidence,
                          "cleanup_horizon_engine_bound",
                          "true") &&
         EvidenceHasExact(result.evidence,
                          "contract_evidence_only",
                          "true");
}

bool GenerationRootValid(const BtreeGenerationRootProof& proof) {
  return proof.index_uuid.valid() &&
         proof.root_page_uuid.valid() &&
         proof.generation_uuid.valid() &&
         proof.root_page_number != 0 &&
         proof.generation_number != 0 &&
         proof.cow_generation_number != 0 &&
         !proof.provider_generation_id.empty() &&
         proof.cow_generation_publish_proven &&
         proof.root_identity_bound &&
         proof.root_reopen_identity_stable &&
         proof.provider_generation_matches_mga_contract &&
         proof.publish_after_durable_mga_evidence;
}

bool SplitMergeScanValid(const BtreeSplitMergeScanProof& proof) {
  return proof.split_capability_present &&
         proof.split_proof_present &&
         proof.merge_capability_present &&
         proof.merge_proof_present &&
         proof.ordered_scan_capability_present &&
         proof.ordered_scan_proof_present &&
         proof.point_lookup_capability_present &&
         proof.deterministic_page_format &&
         proof.page_integrity_proof_present &&
         !proof.structural_evidence_id.empty();
}

bool UniqueProofValid(const UniqueDuplicateReservationProof& proof) {
  return proof.duplicate_preflight_proven &&
         proof.reservation_proof_present &&
         proof.reservation_engine_transaction_bound &&
         proof.duplicate_recheck_engine_bound &&
         proof.ceic_034_unique_finality_protocol_pending &&
         !proof.reservation_evidence_id.empty();
}

bool CleanupProofValid(const BtreeCleanupHorizonProof& proof) {
  return proof.oldest_active_transaction_id != 0 &&
         proof.cleanup_generation_floor != 0 &&
         proof.engine_mga_horizon_bound &&
         proof.cleanup_uses_engine_horizon &&
         proof.provider_cleanup_evidence_only &&
         !proof.cleanup_evidence_id.empty();
}

bool CrashCorruptionProofValid(
    const BtreeCrashCorruptionClosureProof& proof) {
  return proof.crash_reopen_classification_present &&
         proof.corruption_classification_present &&
         proof.durable_provider_evidence_only &&
         proof.crash_classification !=
             IndexCrashRecoveryClassification::unknown &&
         proof.corruption_classification !=
             IndexCorruptionClassification::unknown &&
         !proof.classification_evidence_id.empty();
}

bool RepairRebuildProofValid(const BtreeRepairRebuildProof& proof,
                             const IndexRecoveryRecommendation& recommendation) {
  return proof.validate_before_repair &&
         proof.repair_supported &&
         proof.rebuild_supported &&
         proof.deterministic_recommendation_present &&
         proof.recommendation_matches_mga_recovery_contract &&
         !proof.recommendation_evidence_id.empty() &&
         recommendation.validate &&
         !recommendation.stable_actions.empty();
}

bool ConcurrencyEvidenceValid(const BtreeConcurrencyEvidenceHooks& hooks) {
  return hooks.generation_publish_fence_hook &&
         hooks.root_publish_observer_hook &&
         hooks.split_merge_latch_boundary_hook &&
         hooks.concurrent_scan_mutation_boundary_hook &&
         hooks.evidence_only_not_transaction_finality &&
         !hooks.concurrency_evidence_id.empty();
}

DiagnosticRecord MakeDiagnostic(
    Status status,
    BtreeUniqueDurableProviderClosureStatus closure_status,
    const BtreeUniqueDurableProviderClosureRequest& request,
    std::string detail) {
  DiagnosticRecord record;
  record.status = status;
  record.diagnostic_code = std::string("INDEX.BTREE_UNIQUE_CLOSURE.") +
                           BtreeUniqueDurableProviderClosureStatusName(
                               closure_status);
  record.message_key = std::string("index.btree_unique_closure.") +
                       BtreeUniqueDurableProviderClosureStatusName(
                           closure_status);
  record.arguments.push_back({"family", IndexFamilyName(request.family)});
  record.arguments.push_back({"route", IndexRouteKindName(request.route)});
  record.arguments.push_back({"provider_id", request.provider_id});
  if (!detail.empty()) {
    record.arguments.push_back({"detail", std::move(detail)});
  }
  record.source_component = "sb_core_index.btree_unique_provider_closure";
  return record;
}

void AddBaseEvidence(BtreeUniqueDurableProviderClosureResult* result,
                     const BtreeUniqueDurableProviderClosureRequest& request) {
  AddEvidence(result, "ceic_search_key",
              "CEIC_033_BTREE_UNIQUE_DURABLE_PROVIDER_CLOSURE");
  AddEvidence(result, "family", IndexFamilyName(request.family));
  AddEvidence(result, "route", IndexRouteKindName(request.route));
  AddEvidence(result, "provider_id", request.provider_id);
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
  AddBoolEvidence(result, "generation_root_valid",
                  GenerationRootValid(request.generation_root));
  AddBoolEvidence(result, "split_merge_scan_valid",
                  SplitMergeScanValid(request.split_merge_scan));
  AddBoolEvidence(result, "unique_duplicate_valid",
                  UniqueProofValid(request.unique_duplicate));
  AddBoolEvidence(result, "cleanup_horizon_engine_bound",
                  request.cleanup.engine_mga_horizon_bound);
  AddBoolEvidence(result, "cleanup_uses_engine_horizon",
                  request.cleanup.cleanup_uses_engine_horizon);
  AddEvidence(result, "crash_classification",
              IndexCrashRecoveryClassificationName(
                  request.crash_corruption.crash_classification));
  AddEvidence(result, "corruption_classification",
              IndexCorruptionClassificationName(
                  request.crash_corruption.corruption_classification));
  AddBoolEvidence(result, "crash_corruption_valid",
                  CrashCorruptionProofValid(request.crash_corruption));
  AddBoolEvidence(result, "repair_rebuild_valid",
                  RepairRebuildProofValid(
                      request.repair_rebuild,
                      request.mga_recovery_contract.recommendation));
  AddBoolEvidence(result, "concurrency_evidence_valid",
                  ConcurrencyEvidenceValid(request.concurrency));
  AddBoolEvidence(result, "donor_local_participation",
                  request.donor_local_participation);
  AddBoolEvidence(result, "policy_local_participation",
                  request.policy_local_participation);
  AddBoolEvidence(result, "cluster_local_participation",
                  request.cluster_local_participation);
  AddBoolEvidence(result, "authority_boundary_clear",
                  BtreeUniqueClosureAuthorityBoundaryClear(
                      request.authority_boundary));
  AddBoolEvidence(result, "durable_provider_evidence_claimed",
                  request.durable_provider_evidence_claimed);
  AddBoolEvidence(result, "enterprise_ready_claimed",
                  request.enterprise_ready_claimed);
  AddBoolEvidence(result, "all_index_readiness_claimed",
                  request.all_index_readiness_claimed);
  AddBoolEvidence(result, "ceic_034_unique_protocol_pending",
                  request.ceic_034_unique_protocol_pending);
  AddBoolEvidence(result, "ceic_041_crash_matrix_pending",
                  request.ceic_041_crash_matrix_pending);
  AddU64Evidence(result, "root_page_number",
                 request.generation_root.root_page_number);
  AddU64Evidence(result, "generation_number",
                 request.generation_root.generation_number);
  AddU64Evidence(result, "cow_generation_number",
                 request.generation_root.cow_generation_number);
  AddU64Evidence(result, "cleanup_generation_floor",
                 request.cleanup.cleanup_generation_floor);
  AddU64Evidence(result, "oldest_active_transaction_id",
                 request.cleanup.oldest_active_transaction_id);
  for (const auto& action :
       request.mga_recovery_contract.recommendation.stable_actions) {
    AddEvidence(result, "recommendation_action", action);
  }
  for (const auto& evidence : request.evidence) {
    AddEvidence(result, "closure_evidence", evidence);
  }
}

BtreeUniqueDurableProviderClosureResult BaseResult(
    const BtreeUniqueDurableProviderClosureRequest& request) {
  BtreeUniqueDurableProviderClosureResult result;
  result.status = OkStatus();
  result.durable_provider_evidence = false;
  result.btree_unique_provider_closure_claimed = false;
  result.enterprise_ready_claimed = false;
  result.all_index_readiness_claimed = false;
  result.ceic_034_unique_protocol_pending =
      request.ceic_034_unique_protocol_pending;
  result.ceic_041_crash_matrix_pending =
      request.ceic_041_crash_matrix_pending;
  result.recommendation = request.mga_recovery_contract.recommendation;
  AddBaseEvidence(&result, request);
  return result;
}

BtreeUniqueDurableProviderClosureResult RefuseClosure(
    const BtreeUniqueDurableProviderClosureRequest& request,
    BtreeUniqueDurableProviderClosureStatus closure_status,
    std::string detail) {
  auto result = BaseResult(request);
  result.status = RefuseStatus();
  result.admitted = false;
  result.fail_closed = true;
  result.closure_status = closure_status;
  result.diagnostic =
      MakeDiagnostic(result.status, closure_status, request, std::move(detail));
  AddEvidence(&result, "closure_status",
              BtreeUniqueDurableProviderClosureStatusName(closure_status));
  AddBoolEvidence(&result, "result_durable_provider_evidence",
                  result.durable_provider_evidence);
  AddBoolEvidence(&result, "result_btree_unique_provider_closure_claimed",
                  result.btree_unique_provider_closure_claimed);
  AddBoolEvidence(&result, "result_enterprise_ready_claimed",
                  result.enterprise_ready_claimed);
  AddBoolEvidence(&result, "result_all_index_readiness_claimed",
                  result.all_index_readiness_claimed);
  AddEvidence(&result, "diagnostic_code", result.diagnostic.diagnostic_code);
  return result;
}

}  // namespace

const char* BtreeUniqueDurableProviderClosureStatusName(
    BtreeUniqueDurableProviderClosureStatus status) {
  switch (status) {
    case BtreeUniqueDurableProviderClosureStatus::admitted_durable_provider_evidence:
      return "ADMITTED_DURABLE_PROVIDER_EVIDENCE";
    case BtreeUniqueDurableProviderClosureStatus::unsupported_family:
      return "UNSUPPORTED_FAMILY";
    case BtreeUniqueDurableProviderClosureStatus::donor_policy_cluster_participation:
      return "DONOR_POLICY_CLUSTER_PARTICIPATION";
    case BtreeUniqueDurableProviderClosureStatus::provider_admission_not_admitted:
      return "PROVIDER_ADMISSION_NOT_ADMITTED";
    case BtreeUniqueDurableProviderClosureStatus::mga_recovery_contract_not_admitted:
      return "MGA_RECOVERY_CONTRACT_NOT_ADMITTED";
    case BtreeUniqueDurableProviderClosureStatus::missing_generation_root_identity:
      return "MISSING_GENERATION_ROOT_IDENTITY";
    case BtreeUniqueDurableProviderClosureStatus::missing_split_merge_scan_proof:
      return "MISSING_SPLIT_MERGE_SCAN_PROOF";
    case BtreeUniqueDurableProviderClosureStatus::unique_duplicate_proof_required:
      return "UNIQUE_DUPLICATE_PROOF_REQUIRED";
    case BtreeUniqueDurableProviderClosureStatus::cleanup_horizon_not_engine_bound:
      return "CLEANUP_HORIZON_NOT_ENGINE_BOUND";
    case BtreeUniqueDurableProviderClosureStatus::crash_corruption_classification_absent:
      return "CRASH_CORRUPTION_CLASSIFICATION_ABSENT";
    case BtreeUniqueDurableProviderClosureStatus::repair_rebuild_recommendation_missing:
      return "REPAIR_REBUILD_RECOMMENDATION_MISSING";
    case BtreeUniqueDurableProviderClosureStatus::concurrency_evidence_missing:
      return "CONCURRENCY_EVIDENCE_MISSING";
    case BtreeUniqueDurableProviderClosureStatus::forbidden_authority_claim:
      return "FORBIDDEN_AUTHORITY_CLAIM";
    case BtreeUniqueDurableProviderClosureStatus::enterprise_readiness_overclaim:
      return "ENTERPRISE_READINESS_OVERCLAIM";
    case BtreeUniqueDurableProviderClosureStatus::successor_scope_overclaim:
      return "SUCCESSOR_SCOPE_OVERCLAIM";
    case BtreeUniqueDurableProviderClosureStatus::provider_mga_identity_mismatch:
      return "PROVIDER_MGA_IDENTITY_MISMATCH";
    case BtreeUniqueDurableProviderClosureStatus::durable_provider_evidence_missing:
      return "DURABLE_PROVIDER_EVIDENCE_MISSING";
  }
  return "UNKNOWN";
}

bool BtreeUniqueClosureAuthorityBoundaryClear(
    const BtreeUniqueClosureAuthorityBoundary& boundary) {
  return !boundary.transaction_finality_authority &&
         !boundary.visibility_authority &&
         !boundary.authorization_security_authority &&
         !boundary.security_authority &&
         !boundary.recovery_authority &&
         !boundary.parser_authority &&
         !boundary.donor_authority &&
         !boundary.wal_authority &&
         !boundary.benchmark_authority &&
         !boundary.optimizer_plan_authority &&
         !boundary.index_finality_authority &&
         !boundary.provider_finality_authority &&
         !boundary.cluster_authority &&
         !boundary.agent_action_authority;
}

BtreeUniqueDurableProviderClosureResult
AdmitBtreeUniqueDurableProviderClosure(
    const BtreeUniqueDurableProviderClosureRequest& request) {
  const auto* descriptor = FindBuiltinIndexFamily(request.family);
  if (descriptor == nullptr || !SupportedBtreeFamily(request.family)) {
    return RefuseClosure(
        request,
        BtreeUniqueDurableProviderClosureStatus::unsupported_family,
        "CEIC-033 closes only btree and unique_btree durable providers");
  }
  if (request.donor_local_participation ||
      request.policy_local_participation ||
      request.cluster_local_participation) {
    return RefuseClosure(
        request,
        BtreeUniqueDurableProviderClosureStatus::
            donor_policy_cluster_participation,
        "donor policy and cluster paths cannot participate in local B-tree durable provider closure");
  }
  if (request.enterprise_ready_claimed ||
      request.all_index_readiness_claimed) {
    return RefuseClosure(
        request,
        BtreeUniqueDurableProviderClosureStatus::enterprise_readiness_overclaim,
        "CEIC-033 may admit B-tree durable provider evidence but must not claim enterprise or all-index readiness");
  }
  if (!request.ceic_034_unique_protocol_pending ||
      !request.ceic_041_crash_matrix_pending) {
    return RefuseClosure(
        request,
        BtreeUniqueDurableProviderClosureStatus::successor_scope_overclaim,
        "CEIC-034 unique finality protocol must remain separate successor scope and CEIC-041 crash matrix must remain pending successor scope");
  }
  if (!BtreeUniqueClosureAuthorityBoundaryClear(request.authority_boundary)) {
    return RefuseClosure(
        request,
        BtreeUniqueDurableProviderClosureStatus::forbidden_authority_claim,
        "B-tree provider closure evidence must not claim transaction finality visibility security recovery parser donor WAL provider finality benchmark optimizer plan index finality cluster or agent-action authority");
  }
  if (!request.provider_admission.ok() ||
      request.provider_admission.admission_status !=
          IndexProviderAdmissionStatus::admitted) {
    return RefuseClosure(
        request,
        BtreeUniqueDurableProviderClosureStatus::
            provider_admission_not_admitted,
        "CEIC-031 provider admission result must be admitted before B-tree durable provider closure");
  }
  if (!request.mga_recovery_contract.ok() ||
      request.mga_recovery_contract.contract_status !=
          IndexMGARecoveryContractStatus::admitted_contract_evidence) {
    return RefuseClosure(
        request,
        BtreeUniqueDurableProviderClosureStatus::
            mga_recovery_contract_not_admitted,
        "CEIC-032 MGA recovery contract result must be admitted before B-tree durable provider closure");
  }
  if (!request.durable_provider_evidence_claimed) {
    return RefuseClosure(
        request,
        BtreeUniqueDurableProviderClosureStatus::
            durable_provider_evidence_missing,
        "B-tree durable provider closure cannot admit without durable provider evidence");
  }
  if (!GenerationRootValid(request.generation_root)) {
    return RefuseClosure(
        request,
        BtreeUniqueDurableProviderClosureStatus::
            missing_generation_root_identity,
        "COW generation publish root identity root reopen stability and MGA-bound provider generation evidence are required");
  }
  if (!ProviderAdmissionMatchesRequest(request.provider_admission, request) ||
      !MGARecoveryContractMatchesRequest(request.mga_recovery_contract,
                                         request)) {
    return RefuseClosure(
        request,
        BtreeUniqueDurableProviderClosureStatus::
            provider_mga_identity_mismatch,
        "CEIC-031 provider and CEIC-032 MGA/COW evidence must match the requested family route provider and generation identity");
  }
  if (!SplitMergeScanValid(request.split_merge_scan)) {
    return RefuseClosure(
        request,
        BtreeUniqueDurableProviderClosureStatus::
            missing_split_merge_scan_proof,
        "B-tree split merge ordered scan point lookup deterministic page format and page integrity proof are required");
  }
  if (request.family == IndexFamily::unique_btree &&
      !UniqueProofValid(request.unique_duplicate)) {
    return RefuseClosure(
        request,
        BtreeUniqueDurableProviderClosureStatus::
            unique_duplicate_proof_required,
        "unique_btree requires duplicate preflight and engine-transaction-bound reservation proof without claiming CEIC-034 protocol closure");
  }
  if (!CleanupProofValid(request.cleanup)) {
    return RefuseClosure(
        request,
        BtreeUniqueDurableProviderClosureStatus::
            cleanup_horizon_not_engine_bound,
        "cleanup proof must be bound to engine MGA horizon evidence and remain provider evidence only");
  }
  if (!CrashCorruptionProofValid(request.crash_corruption)) {
    return RefuseClosure(
        request,
        BtreeUniqueDurableProviderClosureStatus::
            crash_corruption_classification_absent,
        "crash reopen and corruption classifications must be present durable provider evidence only");
  }
  if (!RepairRebuildProofValid(
          request.repair_rebuild,
          request.mga_recovery_contract.recommendation)) {
    return RefuseClosure(
        request,
        BtreeUniqueDurableProviderClosureStatus::
            repair_rebuild_recommendation_missing,
        "repair rebuild proof must carry deterministic recommendations derived from the CEIC-032 result");
  }
  if (!ConcurrencyEvidenceValid(request.concurrency)) {
    return RefuseClosure(
        request,
        BtreeUniqueDurableProviderClosureStatus::concurrency_evidence_missing,
        "generation publish fence root observer split/merge latch and concurrent scan/mutation hooks are required as evidence only");
  }

  auto result = BaseResult(request);
  result.status = OkStatus();
  result.admitted = true;
  result.fail_closed = false;
  result.durable_provider_evidence = request.durable_provider_evidence_claimed;
  result.btree_unique_provider_closure_claimed = true;
  result.enterprise_ready_claimed = false;
  result.all_index_readiness_claimed = false;
  result.closure_status =
      BtreeUniqueDurableProviderClosureStatus::
          admitted_durable_provider_evidence;
  result.diagnostic =
      MakeDiagnostic(result.status,
                     result.closure_status,
                     request,
                     "B-tree durable provider evidence admitted without enterprise or all-index readiness");
  AddEvidence(&result, "closure_status",
              BtreeUniqueDurableProviderClosureStatusName(
                  result.closure_status));
  AddBoolEvidence(&result, "result_durable_provider_evidence",
                  result.durable_provider_evidence);
  AddBoolEvidence(&result, "result_btree_unique_provider_closure_claimed",
                  result.btree_unique_provider_closure_claimed);
  AddBoolEvidence(&result, "result_enterprise_ready_claimed",
                  result.enterprise_ready_claimed);
  AddBoolEvidence(&result, "result_all_index_readiness_claimed",
                  result.all_index_readiness_claimed);
  AddEvidence(&result, "diagnostic_code", result.diagnostic.diagnostic_code);
  return result;
}

}  // namespace scratchbird::core::index
