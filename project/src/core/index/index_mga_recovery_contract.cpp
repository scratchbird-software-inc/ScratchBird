// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "index_mga_recovery_contract.hpp"

// CEIC_032_INDEX_MGA_RECOVERY_CONTRACT

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

void AddEvidence(IndexMGARecoveryContractResult* result,
                 std::string key,
                 std::string value) {
  result->evidence.push_back(std::move(key) + "=" + std::move(value));
}

void AddBoolEvidence(IndexMGARecoveryContractResult* result,
                     const char* key,
                     bool value) {
  AddEvidence(result, key, BoolText(value));
}

void AddU64Evidence(IndexMGARecoveryContractResult* result,
                    const char* key,
                    u64 value) {
  AddEvidence(result, key, std::to_string(value));
}

bool GenerationIdentityValid(const IndexCOWGenerationIdentity& generation) {
  return generation.index_uuid.valid() &&
         generation.generation_uuid.valid() &&
         generation.generation_number != 0 &&
         generation.cow_generation_number != 0 &&
         !generation.provider_generation_id.empty() &&
         generation.root_identity_bound &&
         generation.cow_generation_identity_bound &&
         generation.publish_state != IndexGenerationPublishState::unknown;
}

bool MGAEvidenceFresh(const IndexMGARecoveryAuthorityEvidence& evidence) {
  if (evidence.required_engine_evidence_epoch == 0) {
    return true;
  }
  return evidence.inventory_epoch >= evidence.required_engine_evidence_epoch &&
         evidence.snapshot_epoch >= evidence.required_engine_evidence_epoch &&
         evidence.cleanup_horizon_epoch >= evidence.required_engine_evidence_epoch;
}

bool RecoveryEvidenceDurableEnough(
    const IndexRecoveryClassificationEvidence& recovery) {
  return !recovery.recovery_evidence_id.empty() &&
         recovery.durable_recovery_evidence &&
         recovery.provider_evidence_only &&
         recovery.crash_classification !=
             IndexCrashRecoveryClassification::unknown &&
         recovery.corruption_classification !=
             IndexCorruptionClassification::unknown &&
         (recovery.crash_classification !=
              IndexCrashRecoveryClassification::provider_replay_required ||
          recovery.replay_idempotent);
}

DiagnosticRecord MakeIndexMGARecoveryDiagnostic(
    Status status,
    IndexMGARecoveryContractStatus contract_status,
    const IndexMGARecoveryContract& contract,
    std::string detail) {
  DiagnosticRecord record;
  record.status = status;
  record.diagnostic_code = std::string("INDEX.MGA_RECOVERY.") +
                           IndexMGARecoveryContractStatusName(contract_status);
  record.message_key = std::string("index.mga_recovery.") +
                       IndexMGARecoveryContractStatusName(contract_status);
  record.arguments.push_back(
      {"family", IndexFamilyName(contract.identity.family)});
  record.arguments.push_back(
      {"route", IndexRouteKindName(contract.identity.route)});
  record.arguments.push_back({"provider_id", contract.identity.provider_id});
  if (!detail.empty()) {
    record.arguments.push_back({"detail", std::move(detail)});
  }
  record.source_component = "sb_core_index.mga_recovery_contract";
  return record;
}

void AddBaseEvidence(IndexMGARecoveryContractResult* result,
                     const IndexMGARecoveryContract& contract) {
  AddEvidence(result, "ceic_search_key",
              "CEIC_032_INDEX_MGA_RECOVERY_CONTRACT");
  AddEvidence(result, "family", IndexFamilyName(contract.identity.family));
  AddEvidence(result, "route", IndexRouteKindName(contract.identity.route));
  AddEvidence(result, "provider_id", contract.identity.provider_id);
  AddEvidence(result,
              "provider_contract_version",
              contract.identity.provider_contract_version);
  AddBoolEvidence(result, "persistent_provider",
                  contract.identity.persistent_provider);
  AddBoolEvidence(result, "donor_route_requested",
                  contract.identity.donor_route_requested);
  AddBoolEvidence(result, "policy_route_requested",
                  contract.identity.policy_route_requested);
  AddBoolEvidence(result, "cluster_path_requested",
                  contract.identity.cluster_path_requested);
  AddBoolEvidence(result, "external_cluster_provider_only",
                  contract.identity.external_cluster_provider_only);
  AddBoolEvidence(result, "mga_inventory_present",
                  contract.mga_authority.inventory_present);
  AddBoolEvidence(result, "mga_inventory_authoritative",
                  contract.mga_authority.inventory_authoritative);
  AddBoolEvidence(result, "mga_inventory_durable",
                  contract.mga_authority.inventory_durable);
  AddBoolEvidence(result, "mga_snapshot_present",
                  contract.mga_authority.snapshot_present);
  AddBoolEvidence(result, "mga_snapshot_authoritative",
                  contract.mga_authority.snapshot_authoritative);
  AddBoolEvidence(result, "cleanup_horizon_present",
                  contract.mga_authority.cleanup_horizon_present);
  AddBoolEvidence(result, "cleanup_horizon_authoritative",
                  contract.mga_authority.cleanup_horizon_authoritative);
  AddBoolEvidence(result, "cleanup_horizon_engine_bound",
                  contract.mga_authority.cleanup_horizon_engine_bound);
  AddU64Evidence(result, "mga_inventory_epoch",
                 contract.mga_authority.inventory_epoch);
  AddU64Evidence(result, "mga_snapshot_epoch",
                 contract.mga_authority.snapshot_epoch);
  AddU64Evidence(result, "cleanup_horizon_epoch",
                 contract.mga_authority.cleanup_horizon_epoch);
  AddU64Evidence(result, "required_engine_evidence_epoch",
                 contract.mga_authority.required_engine_evidence_epoch);
  AddEvidence(result, "inventory_evidence_id",
              contract.mga_authority.inventory_evidence_id);
  AddEvidence(result, "snapshot_evidence_id",
              contract.mga_authority.snapshot_evidence_id);
  AddEvidence(result, "cleanup_horizon_evidence_id",
              contract.mga_authority.cleanup_horizon_evidence_id);
  AddEvidence(result, "provider_generation_id",
              contract.generation.provider_generation_id);
  AddEvidence(result, "generation_publish_state",
              IndexGenerationPublishStateName(contract.generation.publish_state));
  AddU64Evidence(result, "generation_number",
                 contract.generation.generation_number);
  AddU64Evidence(result, "cow_generation_number",
                 contract.generation.cow_generation_number);
  AddBoolEvidence(result, "root_identity_bound",
                  contract.generation.root_identity_bound);
  AddBoolEvidence(result, "cow_generation_identity_bound",
                  contract.generation.cow_generation_identity_bound);
  AddEvidence(result, "crash_classification",
              IndexCrashRecoveryClassificationName(
                  contract.recovery.crash_classification));
  AddEvidence(result, "corruption_classification",
              IndexCorruptionClassificationName(
                  contract.recovery.corruption_classification));
  AddBoolEvidence(result, "durable_recovery_evidence",
                  contract.recovery.durable_recovery_evidence);
  AddBoolEvidence(result, "replay_idempotent",
                  contract.recovery.replay_idempotent);
  AddBoolEvidence(result, "provider_evidence_only",
                  contract.recovery.provider_evidence_only);
  AddBoolEvidence(result, "authority_boundary_clear",
                  IndexMGARecoveryAuthorityBoundaryClear(
                      contract.authority_boundary));
  AddBoolEvidence(result, "contract_evidence_only",
                  result->contract_evidence_only);
  AddBoolEvidence(result, "durable_family_closure_claimed",
                  result->durable_family_closure_claimed);
  AddBoolEvidence(result, "enterprise_ready_claimed",
                  result->enterprise_ready_claimed);
  for (const auto& evidence : contract.provider_evidence) {
    AddEvidence(result, "provider_evidence", evidence);
  }
}

IndexMGARecoveryContractResult BaseResult(
    const IndexMGARecoveryContract& contract) {
  IndexMGARecoveryContractResult result;
  result.status = OkStatus();
  result.contract_evidence_only = true;
  result.durable_family_closure_claimed = false;
  result.enterprise_ready_claimed = false;
  result.recommendation = RecommendIndexRecoveryAction(
      contract.recovery.crash_classification,
      contract.recovery.corruption_classification);
  AddBaseEvidence(&result, contract);
  AddBoolEvidence(&result, "recommend_validate",
                  result.recommendation.validate);
  AddBoolEvidence(&result, "recommend_rebuild",
                  result.recommendation.rebuild);
  AddBoolEvidence(&result, "recommend_replay",
                  result.recommendation.replay);
  for (const auto& action : result.recommendation.stable_actions) {
    AddEvidence(&result, "recommendation_action", action);
  }
  return result;
}

IndexMGARecoveryContractResult RefuseContract(
    const IndexMGARecoveryContract& contract,
    IndexMGARecoveryContractStatus contract_status,
    std::string detail) {
  auto result = BaseResult(contract);
  result.status = RefuseStatus();
  result.admitted = false;
  result.fail_closed = true;
  result.contract_status = contract_status;
  result.diagnostic = MakeIndexMGARecoveryDiagnostic(
      result.status, contract_status, contract, std::move(detail));
  AddEvidence(&result, "contract_status",
              IndexMGARecoveryContractStatusName(contract_status));
  AddEvidence(&result, "diagnostic_code", result.diagnostic.diagnostic_code);
  return result;
}

}  // namespace

const char* IndexGenerationPublishStateName(
    IndexGenerationPublishState state) {
  switch (state) {
    case IndexGenerationPublishState::unpublished:
      return "UNPUBLISHED";
    case IndexGenerationPublishState::publish_prepared:
      return "PUBLISH_PREPARED";
    case IndexGenerationPublishState::published:
      return "PUBLISHED";
    case IndexGenerationPublishState::superseded:
      return "SUPERSEDED";
    case IndexGenerationPublishState::quarantined:
      return "QUARANTINED";
    case IndexGenerationPublishState::unknown:
      return "UNKNOWN";
  }
  return "UNKNOWN";
}

const char* IndexCrashRecoveryClassificationName(
    IndexCrashRecoveryClassification classification) {
  switch (classification) {
    case IndexCrashRecoveryClassification::clean_reopen:
      return "CLEAN_REOPEN";
    case IndexCrashRecoveryClassification::crash_before_generation_publish:
      return "CRASH_BEFORE_GENERATION_PUBLISH";
    case IndexCrashRecoveryClassification::crash_after_generation_publish:
      return "CRASH_AFTER_GENERATION_PUBLISH";
    case IndexCrashRecoveryClassification::orphan_generation:
      return "ORPHAN_GENERATION";
    case IndexCrashRecoveryClassification::provider_replay_required:
      return "PROVIDER_REPLAY_REQUIRED";
    case IndexCrashRecoveryClassification::unknown:
      return "UNKNOWN";
  }
  return "UNKNOWN";
}

const char* IndexCorruptionClassificationName(
    IndexCorruptionClassification classification) {
  switch (classification) {
    case IndexCorruptionClassification::none:
      return "NONE";
    case IndexCorruptionClassification::generation_identity_mismatch:
      return "GENERATION_IDENTITY_MISMATCH";
    case IndexCorruptionClassification::provider_payload_corrupt:
      return "PROVIDER_PAYLOAD_CORRUPT";
    case IndexCorruptionClassification::checksum_mismatch:
      return "CHECKSUM_MISMATCH";
    case IndexCorruptionClassification::route_family_mismatch:
      return "ROUTE_FAMILY_MISMATCH";
    case IndexCorruptionClassification::unknown:
      return "UNKNOWN";
  }
  return "UNKNOWN";
}

const char* IndexMGARecoveryContractStatusName(
    IndexMGARecoveryContractStatus status) {
  switch (status) {
    case IndexMGARecoveryContractStatus::admitted_contract_evidence:
      return "ADMITTED_CONTRACT_EVIDENCE";
    case IndexMGARecoveryContractStatus::unsupported_family:
      return "UNSUPPORTED_FAMILY";
    case IndexMGARecoveryContractStatus::non_persistent_family:
      return "NON_PERSISTENT_FAMILY";
    case IndexMGARecoveryContractStatus::donor_policy_local_route_blocked:
      return "DONOR_POLICY_LOCAL_ROUTE_BLOCKED";
    case IndexMGARecoveryContractStatus::cluster_external_provider_only:
      return "CLUSTER_EXTERNAL_PROVIDER_ONLY";
    case IndexMGARecoveryContractStatus::missing_provider_evidence:
      return "MISSING_PROVIDER_EVIDENCE";
    case IndexMGARecoveryContractStatus::missing_mga_inventory:
      return "MISSING_MGA_INVENTORY";
    case IndexMGARecoveryContractStatus::missing_mga_snapshot:
      return "MISSING_MGA_SNAPSHOT";
    case IndexMGARecoveryContractStatus::missing_cleanup_horizon:
      return "MISSING_CLEANUP_HORIZON";
    case IndexMGARecoveryContractStatus::stale_mga_evidence:
      return "STALE_MGA_EVIDENCE";
    case IndexMGARecoveryContractStatus::forbidden_authority_claim:
      return "FORBIDDEN_AUTHORITY_CLAIM";
    case IndexMGARecoveryContractStatus::missing_generation_identity:
      return "MISSING_GENERATION_IDENTITY";
    case IndexMGARecoveryContractStatus::cleanup_horizon_not_engine_bound:
      return "CLEANUP_HORIZON_NOT_ENGINE_BOUND";
    case IndexMGARecoveryContractStatus::recovery_evidence_not_durable:
      return "RECOVERY_EVIDENCE_NOT_DURABLE";
    case IndexMGARecoveryContractStatus::enterprise_readiness_overclaim:
      return "ENTERPRISE_READINESS_OVERCLAIM";
  }
  return "UNKNOWN";
}

bool IndexMGARecoveryAuthorityBoundaryClear(
    const IndexMGARecoveryAuthorityBoundary& boundary) {
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

IndexRecoveryRecommendation RecommendIndexRecoveryAction(
    IndexCrashRecoveryClassification crash_classification,
    IndexCorruptionClassification corruption_classification) {
  IndexRecoveryRecommendation recommendation;
  recommendation.validate = true;
  recommendation.stable_actions.push_back("validate");

  switch (crash_classification) {
    case IndexCrashRecoveryClassification::crash_before_generation_publish:
    case IndexCrashRecoveryClassification::orphan_generation:
    case IndexCrashRecoveryClassification::provider_replay_required:
      recommendation.replay = true;
      recommendation.stable_actions.push_back("replay");
      break;
    case IndexCrashRecoveryClassification::clean_reopen:
    case IndexCrashRecoveryClassification::crash_after_generation_publish:
    case IndexCrashRecoveryClassification::unknown:
      break;
  }

  switch (corruption_classification) {
    case IndexCorruptionClassification::generation_identity_mismatch:
    case IndexCorruptionClassification::provider_payload_corrupt:
    case IndexCorruptionClassification::checksum_mismatch:
    case IndexCorruptionClassification::route_family_mismatch:
      recommendation.rebuild = true;
      recommendation.stable_actions.push_back("rebuild");
      break;
    case IndexCorruptionClassification::none:
    case IndexCorruptionClassification::unknown:
      break;
  }
  return recommendation;
}

IndexMGARecoveryContractResult AdmitIndexMGARecoveryContract(
    const IndexMGARecoveryContract& contract) {
  const auto* descriptor = FindBuiltinIndexFamily(contract.identity.family);
  if (descriptor == nullptr || contract.identity.family == IndexFamily::unknown) {
    return RefuseContract(
        contract,
        IndexMGARecoveryContractStatus::unsupported_family,
        "family is not registered as a built-in index family");
  }
  if (contract.identity.family == IndexFamily::donor_emulated ||
      contract.identity.family == IndexFamily::policy_blocked ||
      descriptor->persistence == IndexPersistenceClass::donor_emulated ||
      descriptor->persistence == IndexPersistenceClass::policy_blocked ||
      contract.identity.donor_route_requested ||
      contract.identity.policy_route_requested) {
    return RefuseContract(
        contract,
        IndexMGARecoveryContractStatus::donor_policy_local_route_blocked,
        "donor and policy index routes cannot participate in local MGA recovery contracts");
  }
  if (descriptor->persistence != IndexPersistenceClass::persistent) {
    return RefuseContract(
        contract,
        IndexMGARecoveryContractStatus::non_persistent_family,
        "CEIC-032 admits common MGA recovery evidence for persistent index families only");
  }
  if (contract.identity.cluster_path_requested ||
      !contract.identity.external_cluster_provider_only ||
      contract.authority_boundary.cluster_authority) {
    return RefuseContract(
        contract,
        IndexMGARecoveryContractStatus::cluster_external_provider_only,
        "cluster index recovery paths are external-provider-only and cannot be local recovery authority");
  }
  if (contract.durable_family_closure_claimed ||
      contract.enterprise_ready_claimed) {
    return RefuseContract(
        contract,
        IndexMGARecoveryContractStatus::enterprise_readiness_overclaim,
        "CEIC-032 is common contract evidence only and must not claim durable family closure or enterprise readiness");
  }
  if (!IndexMGARecoveryAuthorityBoundaryClear(contract.authority_boundary)) {
    return RefuseContract(
        contract,
        IndexMGARecoveryContractStatus::forbidden_authority_claim,
        "index provider evidence must not claim transaction finality visibility security recovery parser donor WAL provider finality benchmark optimizer plan index finality cluster or agent-action authority");
  }
  if (!contract.identity.persistent_provider ||
      contract.identity.provider_id.empty() ||
      contract.identity.provider_contract_version.empty() ||
      contract.provider_evidence.empty()) {
    return RefuseContract(
        contract,
        IndexMGARecoveryContractStatus::missing_provider_evidence,
        "provider identity, persistent provider flag, contract version, and provider evidence are required");
  }
  if (!contract.mga_authority.inventory_present ||
      !contract.mga_authority.inventory_authoritative ||
      !contract.mga_authority.inventory_durable ||
      contract.mga_authority.inventory_evidence_id.empty()) {
    return RefuseContract(
        contract,
        IndexMGARecoveryContractStatus::missing_mga_inventory,
        "engine MGA inventory evidence must be present authoritative durable and identified");
  }
  if (!contract.mga_authority.snapshot_present ||
      !contract.mga_authority.snapshot_authoritative ||
      contract.mga_authority.snapshot_evidence_id.empty()) {
    return RefuseContract(
        contract,
        IndexMGARecoveryContractStatus::missing_mga_snapshot,
        "engine MGA snapshot evidence must be present authoritative and identified");
  }
  if (!contract.mga_authority.cleanup_horizon_present ||
      !contract.mga_authority.cleanup_horizon_authoritative ||
      contract.mga_authority.cleanup_horizon_evidence_id.empty()) {
    return RefuseContract(
        contract,
        IndexMGARecoveryContractStatus::missing_cleanup_horizon,
        "engine cleanup horizon evidence must be present authoritative and identified");
  }
  if (!contract.mga_authority.cleanup_horizon_engine_bound) {
    return RefuseContract(
        contract,
        IndexMGARecoveryContractStatus::cleanup_horizon_not_engine_bound,
        "cleanup horizon must be bound to engine MGA horizon evidence");
  }
  if (!MGAEvidenceFresh(contract.mga_authority)) {
    return RefuseContract(
        contract,
        IndexMGARecoveryContractStatus::stale_mga_evidence,
        "MGA inventory snapshot and cleanup horizon evidence epochs must satisfy the required engine evidence epoch");
  }
  if (!GenerationIdentityValid(contract.generation)) {
    return RefuseContract(
        contract,
        IndexMGARecoveryContractStatus::missing_generation_identity,
        "index UUID generation UUID generation number COW generation provider generation root binding and publish state are required");
  }
  if (!RecoveryEvidenceDurableEnough(contract.recovery)) {
    return RefuseContract(
        contract,
        IndexMGARecoveryContractStatus::recovery_evidence_not_durable,
        "recovery classification evidence must be durable provider-evidence-only classified and replay-idempotent when replay is required");
  }

  auto result = BaseResult(contract);
  result.status = OkStatus();
  result.admitted = true;
  result.fail_closed = false;
  result.contract_status =
      IndexMGARecoveryContractStatus::admitted_contract_evidence;
  result.diagnostic = MakeIndexMGARecoveryDiagnostic(
      result.status,
      result.contract_status,
      contract,
      "common index MGA recovery contract admitted as evidence only");
  AddEvidence(&result, "contract_status",
              IndexMGARecoveryContractStatusName(result.contract_status));
  AddEvidence(&result, "diagnostic_code", result.diagnostic.diagnostic_code);
  return result;
}

}  // namespace scratchbird::core::index
