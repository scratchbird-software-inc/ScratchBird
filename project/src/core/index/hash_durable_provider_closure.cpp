// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "hash_durable_provider_closure.hpp"

// CEIC_035_HASH_DURABLE_PROVIDER_CLOSURE

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

void AddEvidence(HashDurableProviderClosureResult* result,
                 std::string key,
                 std::string value) {
  result->evidence.push_back(std::move(key) + "=" + std::move(value));
}

void AddBoolEvidence(HashDurableProviderClosureResult* result,
                     const char* key,
                     bool value) {
  AddEvidence(result, key, BoolText(value));
}

void AddU64Evidence(HashDurableProviderClosureResult* result,
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

bool ProviderAdmissionMatchesRequest(
    const IndexProviderAdmissionResult& result,
    const HashDurableProviderClosureRequest& request) {
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
         EvidenceHasExact(result.evidence,
                          "provider_generation_id",
                          request.generation_directory.provider_generation_id) &&
         EvidenceHasExact(
             result.evidence,
             "generation_number",
             std::to_string(request.generation_directory.generation_number)) &&
         EvidenceHasExact(result.evidence, "cow_generation", "true") &&
         EvidenceHasExact(result.evidence, "root_identity_bound", "true") &&
         EvidenceHasExact(
             result.evidence,
             "cleanup_generation_floor",
             std::to_string(
                 request.generation_directory.cleanup_generation_floor)) &&
         EvidenceHasExact(
             result.evidence,
             "oldest_active_transaction_id",
             std::to_string(
                 request.generation_directory.oldest_active_transaction_id));
}

bool MGARecoveryContractMatchesRequest(
    const IndexMGARecoveryContractResult& result,
    const HashDurableProviderClosureRequest& request) {
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
         EvidenceHasExact(
             result.evidence,
             "generation_number",
             std::to_string(request.generation_directory.generation_number)) &&
         EvidenceHasExact(
             result.evidence,
             "cow_generation_number",
             std::to_string(
                 request.generation_directory.cow_generation_number)) &&
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
                              request.generation_directory.generation_number)) &&
         EvidenceHasExact(result.evidence,
                          "cleanup_horizon_evidence_id",
                          request.generation_directory
                              .cleanup_horizon_evidence_id) &&
         EvidenceHasExact(result.evidence,
                          "provider_generation_id",
                          request.generation_directory.provider_generation_id) &&
         EvidenceHasExact(result.evidence,
                          "contract_evidence_only",
                          "true");
}

bool GenerationDirectoryProofValid(
    const HashGenerationDirectoryProof& proof) {
  return proof.index_uuid.valid() &&
         proof.generation_uuid.valid() &&
         proof.directory_page_number != 0 &&
         proof.generation_number != 0 &&
         proof.cow_generation_number != 0 &&
         proof.cleanup_generation_floor != 0 &&
         proof.oldest_active_transaction_id != 0 &&
         !proof.provider_generation_id.empty() &&
         !proof.cleanup_horizon_evidence_id.empty() &&
         proof.cow_generation_publish_proven &&
         proof.directory_root_identity_bound &&
         proof.directory_reopen_identity_stable &&
         proof.provider_generation_matches_ceic031 &&
         proof.generation_matches_mga_contract &&
         proof.cleanup_identity_matches_ceic031_ceic032 &&
         proof.publish_after_durable_mga_evidence;
}

bool PhysicalStructureProofValid(
    const HashPhysicalStructureProof& proof,
    const HashGenerationDirectoryProof& generation) {
  const auto& report = proof.physical_report;
  return proof.physical_report_valid &&
         report.valid &&
         report.directory_page_number == generation.directory_page_number &&
         report.bucket_count != 0 &&
         report.bucket_page_count == report.bucket_count &&
         report.page_count >= report.bucket_page_count + 1 &&
         report.encoded_key_compare_count != 0 &&
         proof.directory_page_proof_present &&
         proof.bucket_page_proof_present &&
         proof.overflow_page_proof_present &&
         proof.bucket_split_or_directory_growth_proof_present &&
         proof.bucket_merge_or_compaction_proof_present &&
         proof.collision_chain_metadata_proof_present &&
         proof.deterministic_page_format &&
         proof.route_hash_proof_present &&
         !proof.structural_evidence_id.empty();
}

bool SeedFingerprintProofValid(
    const HashSeedAndFingerprintProof& proof,
    const scratchbird::storage::page::IndexHashPhysicalReport& report) {
  const bool supported_algorithm =
      proof.hash_algorithm_version ==
          scratchbird::storage::page::kIndexHashAlgorithmVersion2KeyedHash64 ||
      proof.hash_algorithm_version ==
          scratchbird::storage::page::
              kIndexHashAlgorithmVersion3KeyedHash128Fingerprint;
  const bool report_matches =
      report.valid &&
      report.hash_algorithm_version == proof.hash_algorithm_version &&
      report.hash_seed_engine_generated == proof.hash_seed_engine_generated &&
      report.hash_seed_protected == proof.hash_seed_protected &&
      report.hash_seed != 0 &&
      report.hash_seed_high64 != 0 &&
      report.hash_seed_entropy_source == proof.hash_seed_entropy_source &&
      report.high_assurance_fingerprint_present ==
          (proof.hash_algorithm_version ==
           scratchbird::storage::page::
               kIndexHashAlgorithmVersion3KeyedHash128Fingerprint);
  return supported_algorithm &&
         report_matches &&
         proof.hash_seed_engine_generated &&
         proof.hash_seed_protected &&
         proof.hash_seed_high64_protected &&
         proof.hash_seed_key_material_128_bits &&
         proof.hash_seed_redacted_from_diagnostics &&
         proof.hash_seed_entropy_source_recorded &&
         !proof.hash_seed_entropy_source.empty() &&
         proof.keyed_v2_hash_supported &&
         proof.keyed_v3_128bit_fingerprint_supported &&
         proof.high_assurance_fingerprint_proof_present &&
         !proof.seed_evidence_id.empty();
}

bool ForcedCollisionFixtureValid(
    const HashSeedAndFingerprintProof& proof) {
  if (!proof.forced_route_collision_hook_used &&
      !proof.forced_fingerprint_collision_hook_used) {
    return true;
  }
  return proof.test_fixture_forced_collision_evidence;
}

bool FullKeyRecheckProofValid(const HashFullKeyRecheckProof& proof) {
  const auto decision = DecideHashEqualityProbe(proof.equality_probe);
  return proof.equality_decision_checked &&
         proof.mandatory_full_encoded_key_recheck &&
         proof.hash_match_is_candidate_only &&
         proof.collision_requires_full_key_compare &&
         proof.fingerprint_is_filter_not_authority &&
         proof.mga_recheck_required &&
         proof.security_recheck_required &&
         proof.candidate_set_only &&
         decision.hash_matches &&
         decision.decision == HashCollisionDecision::reject_mismatched_key &&
         decision.reason_code == "hash_collision_key_mismatch" &&
         !proof.recheck_evidence_id.empty();
}

bool CollisionChainProofValid(
    const HashCollisionChainProof& proof,
    const scratchbird::storage::page::IndexHashPhysicalReport& report) {
  return proof.directory_route_hash_consumed &&
         proof.bucket_index_matches_route_hash &&
         proof.collision_root_metadata_checked &&
         proof.collision_chain_traversal_proven &&
         proof.collision_chain_cycle_refusal_proven &&
         proof.tombstones_excluded_from_probe &&
         proof.overflow_chain_metadata_checked &&
         report.collision_root_count != 0 &&
         report.max_collision_chain_length > 1 &&
         report.max_overflow_depth > 0 &&
         report.overflow_page_count > 0 &&
         report.encoded_key_compare_count != 0 &&
         proof.max_collision_chain_length ==
             report.max_collision_chain_length &&
         proof.max_overflow_depth == report.max_overflow_depth &&
         !proof.collision_evidence_id.empty();
}

bool CleanupProofValid(const HashCleanupCompactionProof& proof,
                       const HashGenerationDirectoryProof& generation) {
  return proof.oldest_active_transaction_id != 0 &&
         proof.cleanup_generation_floor != 0 &&
         proof.oldest_active_transaction_id ==
             generation.oldest_active_transaction_id &&
         proof.cleanup_generation_floor == generation.cleanup_generation_floor &&
         proof.engine_mga_horizon_bound &&
         proof.cleanup_uses_engine_horizon &&
         proof.tombstone_cleanup_compaction_proven &&
         proof.overflow_compaction_proven &&
         proof.collision_chains_rebuilt_after_compaction &&
         proof.provider_cleanup_evidence_only &&
         !proof.cleanup_evidence_id.empty();
}

bool CrashCorruptionRepairProofValid(
    const HashCrashCorruptionRepairProof& proof,
    const IndexRecoveryRecommendation& recommendation) {
  return proof.crash_reopen_classification_present &&
         proof.corruption_classification_present &&
         proof.durable_provider_evidence_only &&
         proof.crash_classification !=
             IndexCrashRecoveryClassification::unknown &&
         proof.corruption_classification !=
             IndexCorruptionClassification::unknown &&
         proof.physical_corruption_class !=
             scratchbird::storage::page::IndexHashPhysicalCorruptionClass::
                 unknown &&
         proof.validate_before_repair &&
         proof.repair_supported &&
         proof.rebuild_supported &&
         proof.deterministic_recommendation_present &&
         proof.recommendation_matches_mga_recovery_contract &&
         recommendation.validate &&
         !recommendation.stable_actions.empty() &&
         !proof.classification_evidence_id.empty() &&
         !proof.recommendation_evidence_id.empty();
}

bool AdversarialBenchmarkProofValid(
    const HashAdversarialCollisionBenchmarkProof& proof) {
  return proof.adversarial_collision_benchmark_present &&
         proof.deterministic_collision_fixture_isolated &&
         proof.benchmark_evidence_only &&
         !proof.benchmark_clean_capability_claimed &&
         proof.collision_thresholds_recorded &&
         proof.rebuild_or_reseed_recommendation_recorded &&
         proof.support_bundle_rows_deterministic &&
         !proof.benchmark_evidence_id.empty();
}

DiagnosticRecord MakeDiagnostic(
    Status status,
    HashDurableProviderClosureStatus closure_status,
    const HashDurableProviderClosureRequest& request,
    std::string detail) {
  DiagnosticRecord record;
  record.status = status;
  record.diagnostic_code = std::string("INDEX.HASH_CLOSURE.") +
                           HashDurableProviderClosureStatusName(
                               closure_status);
  record.message_key = std::string("index.hash_closure.") +
                       HashDurableProviderClosureStatusName(closure_status);
  record.arguments.push_back({"family", IndexFamilyName(request.family)});
  record.arguments.push_back({"route", IndexRouteKindName(request.route)});
  record.arguments.push_back({"provider_id", request.provider_id});
  if (!detail.empty()) {
    record.arguments.push_back({"detail", std::move(detail)});
  }
  record.source_component = "sb_core_index.hash_provider_closure";
  return record;
}

void AddBaseEvidence(HashDurableProviderClosureResult* result,
                     const HashDurableProviderClosureRequest& request) {
  const auto& report = request.physical_structure.physical_report;
  AddEvidence(result, "ceic_search_key",
              "CEIC_035_HASH_DURABLE_PROVIDER_CLOSURE");
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
  AddBoolEvidence(result, "generation_directory_valid",
                  GenerationDirectoryProofValid(
                      request.generation_directory));
  AddBoolEvidence(result, "physical_structure_valid",
                  PhysicalStructureProofValid(
                      request.physical_structure,
                      request.generation_directory));
  AddBoolEvidence(result, "seed_fingerprint_valid",
                  SeedFingerprintProofValid(
                      request.seed_fingerprint,
                      request.physical_structure.physical_report));
  AddBoolEvidence(result, "forced_collision_fixture_valid",
                  ForcedCollisionFixtureValid(request.seed_fingerprint));
  AddBoolEvidence(result, "full_key_recheck_valid",
                  FullKeyRecheckProofValid(request.full_key_recheck));
  AddBoolEvidence(result, "collision_chain_valid",
                  CollisionChainProofValid(
                      request.collision_chain,
                      request.physical_structure.physical_report));
  AddBoolEvidence(result, "cleanup_valid",
                  CleanupProofValid(request.cleanup,
                                    request.generation_directory));
  AddBoolEvidence(result, "crash_corruption_repair_valid",
                  CrashCorruptionRepairProofValid(
                      request.crash_corruption_repair,
                      request.mga_recovery_contract.recommendation));
  AddBoolEvidence(result, "adversarial_benchmark_valid",
                  AdversarialBenchmarkProofValid(
                      request.adversarial_benchmark));
  AddEvidence(result, "hash_algorithm_version",
              scratchbird::storage::page::IndexHashAlgorithmVersionName(
                  request.seed_fingerprint.hash_algorithm_version));
  AddBoolEvidence(result, "hash_seed_engine_generated",
                  request.seed_fingerprint.hash_seed_engine_generated);
  AddBoolEvidence(result, "hash_seed_protected",
                  request.seed_fingerprint.hash_seed_protected);
  AddBoolEvidence(result, "hash_seed_high64_protected",
                  request.seed_fingerprint.hash_seed_high64_protected);
  AddBoolEvidence(result, "hash_seed_key_material_128_bits",
                  request.seed_fingerprint.hash_seed_key_material_128_bits);
  AddBoolEvidence(result, "full_encoded_key_compare_mandatory",
                  request.full_key_recheck
                      .mandatory_full_encoded_key_recheck);
  AddBoolEvidence(result, "candidate_set_only",
                  request.full_key_recheck.candidate_set_only);
  AddBoolEvidence(result, "mga_recheck_required",
                  request.full_key_recheck.mga_recheck_required);
  AddBoolEvidence(result, "security_recheck_required",
                  request.full_key_recheck.security_recheck_required);
  AddEvidence(result, "crash_classification",
              IndexCrashRecoveryClassificationName(
                  request.crash_corruption_repair.crash_classification));
  AddEvidence(result, "corruption_classification",
              IndexCorruptionClassificationName(
                  request.crash_corruption_repair.corruption_classification));
  AddEvidence(result, "physical_corruption_class",
              scratchbird::storage::page::
                  IndexHashPhysicalCorruptionClassName(
                      request.crash_corruption_repair
                          .physical_corruption_class));
  AddU64Evidence(result, "directory_page_number",
                 request.generation_directory.directory_page_number);
  AddU64Evidence(result, "generation_number",
                 request.generation_directory.generation_number);
  AddU64Evidence(result, "cow_generation_number",
                 request.generation_directory.cow_generation_number);
  AddU64Evidence(result, "cleanup_generation_floor",
                 request.generation_directory.cleanup_generation_floor);
  AddU64Evidence(result, "oldest_active_transaction_id",
                 request.generation_directory.oldest_active_transaction_id);
  AddU64Evidence(result, "report_bucket_count", report.bucket_count);
  AddU64Evidence(result, "report_overflow_page_count",
                 report.overflow_page_count);
  AddU64Evidence(result, "report_max_collision_chain_length",
                 report.max_collision_chain_length);
  AddU64Evidence(result, "report_encoded_key_compare_count",
                 report.encoded_key_compare_count);
  AddBoolEvidence(result, "reference_local_participation",
                  request.reference_local_participation);
  AddBoolEvidence(result, "policy_local_participation",
                  request.policy_local_participation);
  AddBoolEvidence(result, "cluster_local_participation",
                  request.cluster_local_participation);
  AddBoolEvidence(result, "authority_boundary_clear",
                  HashClosureAuthorityBoundaryClear(
                      request.authority_boundary));
  AddBoolEvidence(result, "durable_provider_evidence_claimed",
                  request.durable_provider_evidence_claimed);
  AddBoolEvidence(result, "ceic_040_runtime_metric_producer_claimed",
                  request.ceic_040_runtime_metric_producer_claimed);
  AddBoolEvidence(result, "ceic_041_crash_matrix_claimed",
                  request.ceic_041_crash_matrix_claimed);
  AddBoolEvidence(result, "ceic_042_readiness_drift_gate_claimed",
                  request.ceic_042_readiness_drift_gate_claimed);
  AddBoolEvidence(result, "enterprise_ready_claimed",
                  request.enterprise_ready_claimed);
  AddBoolEvidence(result, "all_index_readiness_claimed",
                  request.all_index_readiness_claimed);
  for (const auto& action :
       request.mga_recovery_contract.recommendation.stable_actions) {
    AddEvidence(result, "recommendation_action", action);
  }
  for (const auto& evidence : request.evidence) {
    AddEvidence(result, "closure_evidence", evidence);
  }
}

HashDurableProviderClosureResult BaseResult(
    const HashDurableProviderClosureRequest& request) {
  HashDurableProviderClosureResult result;
  result.status = OkStatus();
  result.durable_provider_evidence = false;
  result.hash_provider_closure_claimed = false;
  result.ceic_040_runtime_metric_producer_claimed = false;
  result.ceic_041_crash_matrix_claimed = false;
  result.ceic_042_readiness_drift_gate_claimed = false;
  result.enterprise_ready_claimed = false;
  result.all_index_readiness_claimed = false;
  result.recommendation = request.mga_recovery_contract.recommendation;
  AddBaseEvidence(&result, request);
  return result;
}

HashDurableProviderClosureResult RefuseClosure(
    const HashDurableProviderClosureRequest& request,
    HashDurableProviderClosureStatus closure_status,
    std::string detail) {
  auto result = BaseResult(request);
  result.status = RefuseStatus();
  result.admitted = false;
  result.fail_closed = true;
  result.closure_status = closure_status;
  result.diagnostic =
      MakeDiagnostic(result.status, closure_status, request, std::move(detail));
  AddEvidence(&result, "closure_status",
              HashDurableProviderClosureStatusName(closure_status));
  AddBoolEvidence(&result, "result_durable_provider_evidence",
                  result.durable_provider_evidence);
  AddBoolEvidence(&result, "result_hash_provider_closure_claimed",
                  result.hash_provider_closure_claimed);
  AddBoolEvidence(&result, "result_ceic_040_runtime_metric_producer_claimed",
                  result.ceic_040_runtime_metric_producer_claimed);
  AddBoolEvidence(&result, "result_ceic_041_crash_matrix_claimed",
                  result.ceic_041_crash_matrix_claimed);
  AddBoolEvidence(&result, "result_ceic_042_readiness_drift_gate_claimed",
                  result.ceic_042_readiness_drift_gate_claimed);
  AddBoolEvidence(&result, "result_enterprise_ready_claimed",
                  result.enterprise_ready_claimed);
  AddBoolEvidence(&result, "result_all_index_readiness_claimed",
                  result.all_index_readiness_claimed);
  AddEvidence(&result, "diagnostic_code", result.diagnostic.diagnostic_code);
  return result;
}

}  // namespace

const char* HashDurableProviderClosureStatusName(
    HashDurableProviderClosureStatus status) {
  switch (status) {
    case HashDurableProviderClosureStatus::admitted_durable_provider_evidence:
      return "ADMITTED_DURABLE_PROVIDER_EVIDENCE";
    case HashDurableProviderClosureStatus::unsupported_family:
      return "UNSUPPORTED_FAMILY";
    case HashDurableProviderClosureStatus::reference_policy_cluster_participation:
      return "REFERENCE_POLICY_CLUSTER_PARTICIPATION";
    case HashDurableProviderClosureStatus::provider_admission_not_admitted:
      return "PROVIDER_ADMISSION_NOT_ADMITTED";
    case HashDurableProviderClosureStatus::mga_recovery_contract_not_admitted:
      return "MGA_RECOVERY_CONTRACT_NOT_ADMITTED";
    case HashDurableProviderClosureStatus::provider_mga_identity_mismatch:
      return "PROVIDER_MGA_IDENTITY_MISMATCH";
    case HashDurableProviderClosureStatus::missing_generation_directory_identity:
      return "MISSING_GENERATION_DIRECTORY_IDENTITY";
    case HashDurableProviderClosureStatus::missing_hash_physical_report:
      return "MISSING_HASH_PHYSICAL_REPORT";
    case HashDurableProviderClosureStatus::missing_directory_bucket_overflow_proof:
      return "MISSING_DIRECTORY_BUCKET_OVERFLOW_PROOF";
    case HashDurableProviderClosureStatus::missing_hash_seed_provenance:
      return "MISSING_HASH_SEED_PROVENANCE";
    case HashDurableProviderClosureStatus::missing_v2_v3_hash_proof:
      return "MISSING_V2_V3_HASH_PROOF";
    case HashDurableProviderClosureStatus::fixture_forced_collision_without_test_evidence:
      return "FIXTURE_FORCED_COLLISION_WITHOUT_TEST_EVIDENCE";
    case HashDurableProviderClosureStatus::missing_full_key_recheck:
      return "MISSING_FULL_KEY_RECHECK";
    case HashDurableProviderClosureStatus::missing_collision_chain_route_proof:
      return "MISSING_COLLISION_CHAIN_ROUTE_PROOF";
    case HashDurableProviderClosureStatus::cleanup_horizon_not_engine_bound:
      return "CLEANUP_HORIZON_NOT_ENGINE_BOUND";
    case HashDurableProviderClosureStatus::crash_corruption_classification_absent:
      return "CRASH_CORRUPTION_CLASSIFICATION_ABSENT";
    case HashDurableProviderClosureStatus::repair_rebuild_recommendation_missing:
      return "REPAIR_REBUILD_RECOMMENDATION_MISSING";
    case HashDurableProviderClosureStatus::adversarial_collision_benchmark_missing:
      return "ADVERSARIAL_COLLISION_BENCHMARK_MISSING";
    case HashDurableProviderClosureStatus::deterministic_diagnostics_missing:
      return "DETERMINISTIC_DIAGNOSTICS_MISSING";
    case HashDurableProviderClosureStatus::forbidden_authority_claim:
      return "FORBIDDEN_AUTHORITY_CLAIM";
    case HashDurableProviderClosureStatus::durable_provider_evidence_missing:
      return "DURABLE_PROVIDER_EVIDENCE_MISSING";
    case HashDurableProviderClosureStatus::successor_scope_overclaim:
      return "SUCCESSOR_SCOPE_OVERCLAIM";
    case HashDurableProviderClosureStatus::enterprise_readiness_overclaim:
      return "ENTERPRISE_READINESS_OVERCLAIM";
  }
  return "UNKNOWN";
}

bool HashClosureAuthorityBoundaryClear(
    const HashClosureAuthorityBoundary& boundary) {
  return !boundary.transaction_finality_authority &&
         !boundary.visibility_authority &&
         !boundary.authorization_security_authority &&
         !boundary.security_authority &&
         !boundary.recovery_authority &&
         !boundary.parser_authority &&
         !boundary.reference_authority &&
         !boundary.wal_authority &&
         !boundary.benchmark_authority &&
         !boundary.optimizer_plan_authority &&
         !boundary.index_finality_authority &&
         !boundary.provider_finality_authority &&
         !boundary.cluster_authority &&
         !boundary.agent_action_authority;
}

HashDurableProviderClosureResult AdmitHashDurableProviderClosure(
    const HashDurableProviderClosureRequest& request) {
  const auto* descriptor = FindBuiltinIndexFamily(request.family);
  if (descriptor == nullptr || request.family != IndexFamily::hash) {
    return RefuseClosure(
        request,
        HashDurableProviderClosureStatus::unsupported_family,
        "CEIC-035 closes only the physical hash durable provider");
  }
  if (request.reference_local_participation ||
      request.policy_local_participation ||
      request.cluster_local_participation) {
    return RefuseClosure(
        request,
        HashDurableProviderClosureStatus::reference_policy_cluster_participation,
        "reference policy and cluster paths cannot participate in local hash durable provider closure");
  }
  if (request.enterprise_ready_claimed ||
      request.all_index_readiness_claimed) {
    return RefuseClosure(
        request,
        HashDurableProviderClosureStatus::enterprise_readiness_overclaim,
        "CEIC-035 may admit hash durable provider evidence but must not claim enterprise or all-index readiness");
  }
  if (request.ceic_040_runtime_metric_producer_claimed ||
      request.ceic_041_crash_matrix_claimed ||
      request.ceic_042_readiness_drift_gate_claimed) {
    return RefuseClosure(
        request,
        HashDurableProviderClosureStatus::successor_scope_overclaim,
        "CEIC-040 runtime metrics CEIC-041 crash matrix and CEIC-042 readiness drift proof remain separate pending successor scope");
  }
  if (!HashClosureAuthorityBoundaryClear(request.authority_boundary)) {
    return RefuseClosure(
        request,
        HashDurableProviderClosureStatus::forbidden_authority_claim,
        "hash provider closure evidence must not claim transaction finality visibility security recovery parser reference WAL provider finality benchmark optimizer plan index finality cluster or agent-action authority");
  }
  if (!request.provider_admission.ok() ||
      request.provider_admission.admission_status !=
          IndexProviderAdmissionStatus::admitted) {
    return RefuseClosure(
        request,
        HashDurableProviderClosureStatus::provider_admission_not_admitted,
        "CEIC-031 provider admission result must be admitted before hash durable provider closure");
  }
  if (!request.mga_recovery_contract.ok() ||
      request.mga_recovery_contract.contract_status !=
          IndexMGARecoveryContractStatus::admitted_contract_evidence) {
    return RefuseClosure(
        request,
        HashDurableProviderClosureStatus::mga_recovery_contract_not_admitted,
        "CEIC-032 MGA recovery contract result must be admitted before hash durable provider closure");
  }
  if (!request.durable_provider_evidence_claimed) {
    return RefuseClosure(
        request,
        HashDurableProviderClosureStatus::durable_provider_evidence_missing,
        "hash durable provider closure cannot admit without durable provider evidence");
  }
  if (!GenerationDirectoryProofValid(request.generation_directory)) {
    return RefuseClosure(
        request,
        HashDurableProviderClosureStatus::missing_generation_directory_identity,
        "COW generation publish directory-root identity reopen stability cleanup identity and MGA-bound provider generation evidence are required");
  }
  if (!ProviderAdmissionMatchesRequest(request.provider_admission, request) ||
      !MGARecoveryContractMatchesRequest(request.mga_recovery_contract,
                                         request)) {
    return RefuseClosure(
        request,
        HashDurableProviderClosureStatus::provider_mga_identity_mismatch,
        "CEIC-031 provider and CEIC-032 MGA/COW evidence must exactly match family route provider generation COW root and cleanup identity");
  }
  if (!request.physical_structure.physical_report_valid ||
      !request.physical_structure.physical_report.valid) {
    return RefuseClosure(
        request,
        HashDurableProviderClosureStatus::missing_hash_physical_report,
        "hash physical validation report must be present and valid");
  }
  if (!PhysicalStructureProofValid(request.physical_structure,
                                   request.generation_directory)) {
    return RefuseClosure(
        request,
        HashDurableProviderClosureStatus::
            missing_directory_bucket_overflow_proof,
        "directory bucket overflow split merge compaction collision metadata deterministic page-format and route-hash proof are required");
  }
  if (!SeedFingerprintProofValid(request.seed_fingerprint,
                                 request.physical_structure.physical_report)) {
    const bool missing_algorithm =
        !request.seed_fingerprint.keyed_v2_hash_supported ||
        !request.seed_fingerprint.keyed_v3_128bit_fingerprint_supported;
    return RefuseClosure(
        request,
        missing_algorithm
            ? HashDurableProviderClosureStatus::missing_v2_v3_hash_proof
            : HashDurableProviderClosureStatus::missing_hash_seed_provenance,
        "hash closure requires protected engine-generated 128-bit keyed seed provenance plus v2 keyed hash and v3 128-bit fingerprint proof");
  }
  if (!ForcedCollisionFixtureValid(request.seed_fingerprint)) {
    return RefuseClosure(
        request,
        HashDurableProviderClosureStatus::
            fixture_forced_collision_without_test_evidence,
        "forced route or fingerprint collision hooks are admitted only as isolated test fixture evidence");
  }
  if (!FullKeyRecheckProofValid(request.full_key_recheck)) {
    return RefuseClosure(
        request,
        HashDurableProviderClosureStatus::missing_full_key_recheck,
        "hash hits and fingerprint matches remain candidates until mandatory full encoded-key MGA and security recheck proof is present");
  }
  if (!CollisionChainProofValid(request.collision_chain,
                                request.physical_structure.physical_report)) {
    return RefuseClosure(
        request,
        HashDurableProviderClosureStatus::missing_collision_chain_route_proof,
        "directory route hash collision-root overflow-chain traversal tombstone exclusion and cycle-refusal proof are required");
  }
  if (!CleanupProofValid(request.cleanup, request.generation_directory)) {
    return RefuseClosure(
        request,
        HashDurableProviderClosureStatus::cleanup_horizon_not_engine_bound,
        "hash cleanup and compaction proof must be bound to engine MGA horizon evidence and matching cleanup identity");
  }
  if (!request.crash_corruption_repair.crash_reopen_classification_present ||
      !request.crash_corruption_repair.corruption_classification_present ||
      request.crash_corruption_repair.crash_classification ==
          IndexCrashRecoveryClassification::unknown ||
      request.crash_corruption_repair.corruption_classification ==
          IndexCorruptionClassification::unknown) {
    return RefuseClosure(
        request,
        HashDurableProviderClosureStatus::
            crash_corruption_classification_absent,
        "hash crash reopen and corruption classifications must be present durable provider evidence only");
  }
  if (!CrashCorruptionRepairProofValid(
          request.crash_corruption_repair,
          request.mga_recovery_contract.recommendation)) {
    return RefuseClosure(
        request,
        HashDurableProviderClosureStatus::
            repair_rebuild_recommendation_missing,
        "hash validate repair rebuild recommendations must be deterministic and derived from the CEIC-032 result");
  }
  if (!AdversarialBenchmarkProofValid(request.adversarial_benchmark)) {
    return RefuseClosure(
        request,
        request.adversarial_benchmark.support_bundle_rows_deterministic
            ? HashDurableProviderClosureStatus::
                  adversarial_collision_benchmark_missing
            : HashDurableProviderClosureStatus::
                  deterministic_diagnostics_missing,
        "hash adversarial collision benchmark evidence and deterministic diagnostics must be present as evidence only");
  }

  auto result = BaseResult(request);
  result.status = OkStatus();
  result.admitted = true;
  result.fail_closed = false;
  result.durable_provider_evidence = request.durable_provider_evidence_claimed;
  result.hash_provider_closure_claimed = true;
  result.ceic_040_runtime_metric_producer_claimed = false;
  result.ceic_041_crash_matrix_claimed = false;
  result.ceic_042_readiness_drift_gate_claimed = false;
  result.enterprise_ready_claimed = false;
  result.all_index_readiness_claimed = false;
  result.closure_status =
      HashDurableProviderClosureStatus::admitted_durable_provider_evidence;
  result.diagnostic =
      MakeDiagnostic(result.status,
                     result.closure_status,
                     request,
                     "hash durable provider evidence admitted without CEIC-040 CEIC-041 CEIC-042 enterprise or all-index readiness");
  AddEvidence(&result, "closure_status",
              HashDurableProviderClosureStatusName(result.closure_status));
  AddBoolEvidence(&result, "result_durable_provider_evidence",
                  result.durable_provider_evidence);
  AddBoolEvidence(&result, "result_hash_provider_closure_claimed",
                  result.hash_provider_closure_claimed);
  AddBoolEvidence(&result, "result_ceic_040_runtime_metric_producer_claimed",
                  result.ceic_040_runtime_metric_producer_claimed);
  AddBoolEvidence(&result, "result_ceic_041_crash_matrix_claimed",
                  result.ceic_041_crash_matrix_claimed);
  AddBoolEvidence(&result, "result_ceic_042_readiness_drift_gate_claimed",
                  result.ceic_042_readiness_drift_gate_claimed);
  AddBoolEvidence(&result, "result_enterprise_ready_claimed",
                  result.enterprise_ready_claimed);
  AddBoolEvidence(&result, "result_all_index_readiness_claimed",
                  result.all_index_readiness_claimed);
  AddEvidence(&result, "diagnostic_code", result.diagnostic.diagnostic_code);
  return result;
}

}  // namespace scratchbird::core::index
