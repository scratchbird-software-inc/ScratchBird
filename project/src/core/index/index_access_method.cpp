// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "index_access_method.hpp"

// CEIC_031_INDEX_ACCESS_METHOD_PROVIDER_INTERFACE

#include <utility>

namespace scratchbird::core::index {
namespace {
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;
Status OkStatus() { return Status{StatusCode::ok, Severity::info, Subsystem::engine}; }
Status RefuseStatus() {
  return Status{StatusCode::platform_required_feature_missing,
                Severity::error,
                Subsystem::engine};
}

const char* BoolText(bool value) { return value ? "true" : "false"; }

bool PersistentMutationOrMaintenanceRoute(IndexRouteKind route) {
  return route == IndexRouteKind::dml_insert ||
         route == IndexRouteKind::dml_update ||
         route == IndexRouteKind::dml_delete ||
         route == IndexRouteKind::bulk_build ||
         route == IndexRouteKind::maintenance ||
         route == IndexRouteKind::validate_repair;
}

bool GenerationHandleValid(const IndexProviderGenerationHandle& generation) {
  return generation.generation_uuid.valid() &&
         generation.generation_number != 0 &&
         !generation.provider_generation_id.empty() &&
         generation.root_identity_bound &&
         generation.cow_generation;
}

bool MutationBatchAdmissionValid(
    const IndexProviderMutationBatchAdmission& mutation_batch) {
  return mutation_batch.batch_uuid.valid() &&
         mutation_batch.operation_count != 0 &&
         mutation_batch.batch_admission_requested &&
         mutation_batch.provider_batch_admission_supported &&
         mutation_batch.deterministic_batch_order &&
         mutation_batch.idempotent_replay_safe;
}

bool RecoveryContextValid(const IndexProviderRecoveryContext& recovery) {
  return !recovery.recovery_context_id.empty() &&
         recovery.recovery_reopen_supported &&
         recovery.crash_classification_supported &&
         recovery.corruption_classification_supported &&
         recovery.mga_recovery_evidence_only;
}

bool CleanupHorizonValid(const IndexProviderCleanupHorizon& cleanup) {
  return cleanup.oldest_active_transaction_id != 0 &&
         cleanup.cleanup_generation_floor != 0 &&
         cleanup.engine_mga_horizon_bound &&
         cleanup.provider_cleanup_supported;
}

bool ValidationRepairValid(
    const IndexProviderValidationRepairSupport& validation_repair) {
  return validation_repair.validate_supported &&
         validation_repair.repair_supported &&
         validation_repair.rebuild_supported &&
         validation_repair.deterministic_diagnostics;
}

void AddEvidence(IndexProviderAdmissionResult* result,
                 std::string key,
                 std::string value) {
  result->evidence.push_back(std::move(key) + "=" + std::move(value));
}

void AddBoolEvidence(IndexProviderAdmissionResult* result,
                     const char* key,
                     bool value) {
  AddEvidence(result, key, BoolText(value));
}

DiagnosticRecord MakeIndexProviderDiagnostic(
    Status status,
    IndexProviderAdmissionStatus admission_status,
    const IndexProviderAccessMethodContract& contract,
    std::string detail) {
  DiagnosticRecord record;
  record.status = status;
  record.diagnostic_code = std::string("INDEX.PROVIDER_ACCESS_METHOD.") +
                           IndexProviderAdmissionStatusName(admission_status);
  record.message_key = std::string("index.provider_access_method.") +
                       IndexProviderAdmissionStatusName(admission_status);
  record.arguments.push_back({"family", IndexFamilyName(contract.family)});
  record.arguments.push_back({"route", IndexRouteKindName(contract.route)});
  record.arguments.push_back({"provider_id", contract.provider.provider_id});
  if (!detail.empty()) {
    record.arguments.push_back({"detail", std::move(detail)});
  }
  record.source_component = "sb_core_index.provider_access_method";
  return record;
}

IndexProviderAdmissionResult BaseProviderAdmissionResult(
    const IndexProviderAccessMethodContract& contract) {
  IndexProviderAdmissionResult result;
  result.status = OkStatus();
  result.provider_contract_only = true;
  result.durable_family_closure_claimed = false;
  result.enterprise_ready_claimed = false;
  AddEvidence(&result, "ceic_search_key",
              "CEIC_031_INDEX_ACCESS_METHOD_PROVIDER_INTERFACE");
  AddEvidence(&result, "family", IndexFamilyName(contract.family));
  AddEvidence(&result, "route", IndexRouteKindName(contract.route));
  AddEvidence(&result, "provider_id", contract.provider.provider_id);
  AddEvidence(&result, "provider_contract_version",
              contract.provider.provider_contract_version);
  AddBoolEvidence(&result, "provider_backed",
                  contract.provider.provider_backed);
  AddBoolEvidence(&result, "persistent_access_method",
                  contract.provider.persistent_access_method);
  AddBoolEvidence(&result, "route_capability_present",
                  contract.route_boundary.route_capability_present);
  AddBoolEvidence(&result, "provider_route_supported",
                  contract.route_boundary.provider_route_supported);
  AddBoolEvidence(
      &result,
      "static_registry_complete_capability_seen",
      contract.route_boundary.static_registry_complete_capability_seen);
  AddBoolEvidence(&result, "cluster_path_requested",
                  contract.route_boundary.cluster_path_requested);
  AddBoolEvidence(&result, "external_cluster_provider_only",
                  contract.route_boundary.external_cluster_provider_only);
  AddBoolEvidence(&result, "route_specific_boundary_declared",
                  contract.route_boundary.route_specific_boundary_declared);
  AddBoolEvidence(&result, "batch_admission_requested",
                  contract.mutation_batch.batch_admission_requested);
  AddBoolEvidence(&result, "provider_batch_admission_supported",
                  contract.mutation_batch.provider_batch_admission_supported);
  AddBoolEvidence(&result, "deterministic_batch_order",
                  contract.mutation_batch.deterministic_batch_order);
  AddBoolEvidence(&result, "idempotent_replay_safe",
                  contract.mutation_batch.idempotent_replay_safe);
  AddEvidence(&result, "provider_generation_id",
              contract.generation.provider_generation_id);
  AddEvidence(&result, "generation_number",
              std::to_string(contract.generation.generation_number));
  AddBoolEvidence(&result, "cow_generation",
                  contract.generation.cow_generation);
  AddBoolEvidence(&result, "root_identity_bound",
                  contract.generation.root_identity_bound);
  AddEvidence(&result, "oldest_active_transaction_id",
              std::to_string(
                  contract.cleanup.oldest_active_transaction_id));
  AddEvidence(&result, "cleanup_generation_floor",
              std::to_string(contract.cleanup.cleanup_generation_floor));
  AddBoolEvidence(&result, "cleanup_horizon_engine_bound",
                  contract.cleanup.engine_mga_horizon_bound);
  AddBoolEvidence(&result, "authority_boundary_clear",
                  IndexProviderAuthorityBoundaryClear(
                      contract.authority_boundary));
  AddBoolEvidence(&result, "provider_contract_only",
                  result.provider_contract_only);
  AddBoolEvidence(&result, "durable_family_closure_claimed",
                  result.durable_family_closure_claimed);
  AddBoolEvidence(&result, "enterprise_ready_claimed",
                  result.enterprise_ready_claimed);
  for (const auto& evidence : contract.provider_evidence) {
    AddEvidence(&result, "provider_evidence", evidence);
  }
  return result;
}

IndexProviderAdmissionResult RefuseProviderContract(
    const IndexProviderAccessMethodContract& contract,
    IndexProviderAdmissionStatus admission_status,
    std::string detail) {
  auto result = BaseProviderAdmissionResult(contract);
  result.status = RefuseStatus();
  result.admitted = false;
  result.fail_closed = true;
  result.admission_status = admission_status;
  result.diagnostic = MakeIndexProviderDiagnostic(
      result.status, admission_status, contract, std::move(detail));
  AddEvidence(&result, "admission_status",
              IndexProviderAdmissionStatusName(admission_status));
  AddEvidence(&result, "diagnostic_code", result.diagnostic.diagnostic_code);
  return result;
}
}  // namespace

IndexAccessMethodCapabilities CapabilitiesForFamily(IndexFamily family) {
  const auto* descriptor = FindBuiltinIndexFamily(family);
  const auto* state = FindBuiltinIndexFamilyPhysicalCapabilityState(family);
  IndexAccessMethodCapabilities caps;
  caps.family = family;
  if (!descriptor || !state || !state->declared_capability ||
      descriptor->completion != IndexCompletionStatus::accepted_requires_full_implementation) {
    return caps;
  }
  caps.returns_lossy_candidates = true;
  caps.requires_mga_recheck = true;
  caps.requires_security_recheck = true;
  if (family == IndexFamily::bitmap) {
    return caps;
  }
  caps.supports_insert = state->runtime_available && state->physical_writer;
  caps.supports_delete = caps.supports_insert;
  caps.supports_update = caps.supports_insert;
  caps.supports_scan = state->runtime_available && state->physical_reader;
  caps.supports_verify = state->runtime_available && state->validate;
  caps.supports_rebuild = state->runtime_available && state->rebuild;
  caps.can_satisfy_order = caps.supports_scan && descriptor->supports_ordering;
  caps.can_be_unique = caps.supports_insert && descriptor->supports_uniqueness;
  return caps;
}

IndexCandidatePipelineResult ApplyIndexCandidateAuthorityPipeline(std::vector<IndexCandidate> candidates) {
  IndexCandidatePipelineResult result;
  result.status = OkStatus();
  for (auto& candidate : candidates) {
    result.metrics.candidates++;
    if (candidate.key.requires_recheck || candidate.key.lossy) result.metrics.rechecks++;
    if (candidate.mga_visible && candidate.predicate_exact && candidate.security_visible) {
      result.metrics.visible++;
      result.accepted.push_back(std::move(candidate));
    } else {
      result.rejected.push_back(std::move(candidate));
    }
  }
  return result;
}

const char* IndexProviderAdmissionStatusName(
    IndexProviderAdmissionStatus status) {
  switch (status) {
    case IndexProviderAdmissionStatus::admitted:
      return "ADMITTED";
    case IndexProviderAdmissionStatus::missing_provider_evidence:
      return "MISSING_PROVIDER_EVIDENCE";
    case IndexProviderAdmissionStatus::static_capability_only:
      return "STATIC_CAPABILITY_ONLY";
    case IndexProviderAdmissionStatus::unsupported_family:
      return "UNSUPPORTED_FAMILY";
    case IndexProviderAdmissionStatus::non_persistent_family:
      return "NON_PERSISTENT_FAMILY";
    case IndexProviderAdmissionStatus::reference_emulated_non_runtime:
      return "REFERENCE_EMULATED_NON_RUNTIME";
    case IndexProviderAdmissionStatus::policy_blocked_non_runtime:
      return "POLICY_BLOCKED_NON_RUNTIME";
    case IndexProviderAdmissionStatus::route_capability_required:
      return "ROUTE_CAPABILITY_REQUIRED";
    case IndexProviderAdmissionStatus::route_not_supported:
      return "ROUTE_NOT_SUPPORTED";
    case IndexProviderAdmissionStatus::mutation_batch_admission_required:
      return "MUTATION_BATCH_ADMISSION_REQUIRED";
    case IndexProviderAdmissionStatus::generation_handle_required:
      return "GENERATION_HANDLE_REQUIRED";
    case IndexProviderAdmissionStatus::recovery_context_required:
      return "RECOVERY_CONTEXT_REQUIRED";
    case IndexProviderAdmissionStatus::cleanup_horizon_required:
      return "CLEANUP_HORIZON_REQUIRED";
    case IndexProviderAdmissionStatus::validation_repair_required:
      return "VALIDATION_REPAIR_REQUIRED";
    case IndexProviderAdmissionStatus::authority_boundary_refused:
      return "AUTHORITY_BOUNDARY_REFUSED";
    case IndexProviderAdmissionStatus::cluster_external_provider_only:
      return "CLUSTER_EXTERNAL_PROVIDER_ONLY";
  }
  return "UNKNOWN";
}

bool IndexProviderAuthorityBoundaryClear(
    const IndexProviderAuthorityBoundary& boundary) {
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
         !boundary.cluster_authority &&
         !boundary.agent_action_authority;
}

bool IndexProviderRouteRequiresGeneration(IndexRouteKind route) {
  return PersistentMutationOrMaintenanceRoute(route);
}

bool IndexProviderRouteRequiresMutationBatchAdmission(IndexRouteKind route) {
  return PersistentMutationOrMaintenanceRoute(route);
}

bool IndexProviderRouteRequiresRecoveryContext(IndexRouteKind route) {
  return PersistentMutationOrMaintenanceRoute(route);
}

bool IndexProviderRouteRequiresCleanupHorizon(IndexRouteKind route) {
  return PersistentMutationOrMaintenanceRoute(route);
}

bool IndexProviderRouteRequiresValidationRepair(IndexRouteKind route) {
  return route == IndexRouteKind::maintenance ||
         route == IndexRouteKind::validate_repair;
}

IndexProviderAdmissionResult AdmitIndexProviderAccessMethod(
    const IndexProviderAccessMethodContract& contract) {
  const auto* descriptor = FindBuiltinIndexFamily(contract.family);
  if (descriptor == nullptr || contract.family == IndexFamily::unknown) {
    return RefuseProviderContract(
        contract,
        IndexProviderAdmissionStatus::unsupported_family,
        "family is not registered as a built-in index family");
  }
  if (contract.family == IndexFamily::reference_emulated ||
      descriptor->persistence == IndexPersistenceClass::reference_emulated) {
    return RefuseProviderContract(
        contract,
        IndexProviderAdmissionStatus::reference_emulated_non_runtime,
        "reference-emulated index mappings are non-runtime non-authority");
  }
  if (contract.family == IndexFamily::policy_blocked ||
      descriptor->persistence == IndexPersistenceClass::policy_blocked) {
    return RefuseProviderContract(
        contract,
        IndexProviderAdmissionStatus::policy_blocked_non_runtime,
        "policy-blocked index families cannot be runtime providers");
  }
  if (descriptor->persistence != IndexPersistenceClass::persistent) {
    return RefuseProviderContract(
        contract,
        IndexProviderAdmissionStatus::non_persistent_family,
        "CEIC-031 provider contract admits persistent index families only");
  }
  if (contract.route_boundary.cluster_path_requested ||
      !contract.route_boundary.external_cluster_provider_only ||
      contract.authority_boundary.cluster_authority) {
    return RefuseProviderContract(
        contract,
        IndexProviderAdmissionStatus::cluster_external_provider_only,
        "cluster index paths are external-provider-only and cannot be local runtime authority");
  }
  if (!IndexProviderAuthorityBoundaryClear(contract.authority_boundary)) {
    return RefuseProviderContract(
        contract,
        IndexProviderAdmissionStatus::authority_boundary_refused,
        "provider evidence must not claim transaction visibility security recovery parser reference WAL benchmark optimizer plan index finality cluster or agent-action authority");
  }
  if (!contract.provider.provider_backed ||
      !contract.provider.persistent_access_method ||
      contract.provider.provider_id.empty() ||
      contract.provider.provider_name.empty() ||
      contract.provider.provider_contract_version.empty() ||
      contract.provider_evidence.empty()) {
    if (contract.route_boundary.static_registry_complete_capability_seen) {
      return RefuseProviderContract(
          contract,
          IndexProviderAdmissionStatus::static_capability_only,
          "static CompleteCapability or registry flags are not provider evidence");
    }
    return RefuseProviderContract(
        contract,
        IndexProviderAdmissionStatus::missing_provider_evidence,
        "provider identity, persistent provider backing, contract version, and evidence are required");
  }
  if (!contract.route_boundary.route_capability_present ||
      !contract.route_boundary.route_specific_boundary_declared ||
      contract.route == IndexRouteKind::unknown) {
    return RefuseProviderContract(
        contract,
        IndexProviderAdmissionStatus::route_capability_required,
        "route capability and route-specific boundary are required");
  }
  const auto* route_state =
      FindBuiltinIndexRouteCapabilityState(contract.route, contract.family);
  if (route_state == nullptr || !route_state->route_complete() ||
      !contract.route_boundary.provider_route_supported) {
    return RefuseProviderContract(
        contract,
        IndexProviderAdmissionStatus::route_not_supported,
        "route must be complete in route capability state and explicitly supported by the provider");
  }
  if (IndexProviderRouteRequiresMutationBatchAdmission(contract.route) &&
      !MutationBatchAdmissionValid(contract.mutation_batch)) {
    return RefuseProviderContract(
        contract,
        IndexProviderAdmissionStatus::mutation_batch_admission_required,
        "persistent mutation and maintenance routes require provider mutation-batch admission evidence");
  }
  if (IndexProviderRouteRequiresGeneration(contract.route) &&
      !GenerationHandleValid(contract.generation)) {
    return RefuseProviderContract(
        contract,
        IndexProviderAdmissionStatus::generation_handle_required,
        "persistent mutation and maintenance routes require generation handle identity");
  }
  if (IndexProviderRouteRequiresRecoveryContext(contract.route) &&
      !RecoveryContextValid(contract.recovery)) {
    return RefuseProviderContract(
        contract,
        IndexProviderAdmissionStatus::recovery_context_required,
        "persistent mutation and maintenance routes require recovery context evidence");
  }
  if (IndexProviderRouteRequiresCleanupHorizon(contract.route) &&
      !CleanupHorizonValid(contract.cleanup)) {
    return RefuseProviderContract(
        contract,
        IndexProviderAdmissionStatus::cleanup_horizon_required,
        "persistent mutation and maintenance routes require engine-bound cleanup horizon evidence");
  }
  if (IndexProviderRouteRequiresValidationRepair(contract.route) &&
      !ValidationRepairValid(contract.validation_repair)) {
    return RefuseProviderContract(
        contract,
        IndexProviderAdmissionStatus::validation_repair_required,
        "maintenance and validate-repair routes require validate repair rebuild support with deterministic diagnostics");
  }

  auto result = BaseProviderAdmissionResult(contract);
  result.status = OkStatus();
  result.admitted = true;
  result.fail_closed = false;
  result.admission_status = IndexProviderAdmissionStatus::admitted;
  result.diagnostic = MakeIndexProviderDiagnostic(
      result.status,
      result.admission_status,
      contract,
      "provider access-method contract admitted without durable family closure");
  AddEvidence(&result, "admission_status",
              IndexProviderAdmissionStatusName(result.admission_status));
  AddBoolEvidence(&result, "route_requires_mutation_batch_admission",
                  IndexProviderRouteRequiresMutationBatchAdmission(
                      contract.route));
  AddBoolEvidence(&result, "route_requires_generation",
                  IndexProviderRouteRequiresGeneration(contract.route));
  AddBoolEvidence(&result, "route_requires_recovery_context",
                  IndexProviderRouteRequiresRecoveryContext(contract.route));
  AddBoolEvidence(&result, "route_requires_cleanup_horizon",
                  IndexProviderRouteRequiresCleanupHorizon(contract.route));
  AddBoolEvidence(&result, "route_requires_validation_repair",
                  IndexProviderRouteRequiresValidationRepair(contract.route));
  AddEvidence(&result, "diagnostic_code", result.diagnostic.diagnostic_code);
  return result;
}

}  // namespace scratchbird::core::index
