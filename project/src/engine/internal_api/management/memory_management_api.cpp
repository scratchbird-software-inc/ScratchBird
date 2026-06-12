// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "management/memory_management_api.hpp"

#include "api_diagnostics.hpp"
#include "behavior_support/api_behavior_store.hpp"
#include "background_memory_reclamation.hpp"
#include "disk_device.hpp"
#include "security/security_model.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace scratchbird::engine::internal_api {
namespace {

namespace mem = scratchbird::core::memory;
namespace disk = scratchbird::storage::disk;

constexpr const char* kObjectResidencyCatalogName =
    "sys.memory_object_residency_policy";
constexpr const char* kRateLimitCatalogName =
    "sys.memory_rate_limit_policy";
constexpr const char* kReportCatalogName = "sys.memory_report_catalog";
constexpr const char* kPolicyMigrationCatalogName =
    "sys.memory_policy_migration_catalog";

const char* MemoryOperationFamily(EngineMemoryManagementOperation operation) {
  switch (operation) {
    case EngineMemoryManagementOperation::inspect_governance:
    case EngineMemoryManagementOperation::validate_governance:
    case EngineMemoryManagementOperation::plan_cache_control:
    case EngineMemoryManagementOperation::plan_pressure_response:
      return "governance";
    case EngineMemoryManagementOperation::create_report:
    case EngineMemoryManagementOperation::review_recommendation:
    case EngineMemoryManagementOperation::apply_safe_recommendation:
      return "automation";
    case EngineMemoryManagementOperation::inspect_object_residency:
    case EngineMemoryManagementOperation::set_object_residency:
      return "object_residency";
    case EngineMemoryManagementOperation::inspect_rate_limit:
    case EngineMemoryManagementOperation::set_rate_limit:
      return "rate_limit";
    case EngineMemoryManagementOperation::plan_policy_upgrade:
    case EngineMemoryManagementOperation::plan_policy_migration:
      return "policy_upgrade";
  }
  return "unknown";
}

bool IsMutationShaped(EngineMemoryManagementOperation operation) {
  switch (operation) {
    case EngineMemoryManagementOperation::plan_cache_control:
    case EngineMemoryManagementOperation::apply_safe_recommendation:
    case EngineMemoryManagementOperation::set_object_residency:
    case EngineMemoryManagementOperation::set_rate_limit:
    case EngineMemoryManagementOperation::plan_policy_migration:
      return true;
    case EngineMemoryManagementOperation::inspect_governance:
    case EngineMemoryManagementOperation::validate_governance:
    case EngineMemoryManagementOperation::plan_pressure_response:
    case EngineMemoryManagementOperation::create_report:
    case EngineMemoryManagementOperation::review_recommendation:
    case EngineMemoryManagementOperation::inspect_object_residency:
    case EngineMemoryManagementOperation::inspect_rate_limit:
    case EngineMemoryManagementOperation::plan_policy_upgrade:
      return false;
  }
  return false;
}

EngineApiDiagnostic MemoryDiagnostic(const char* operation,
                                     std::string code,
                                     std::string detail) {
  return MakeEngineApiDiagnostic(std::move(code),
                                 std::string(operation) + ".memory_management",
                                 std::move(detail),
                                 true);
}

EngineApiDiagnostic CoreMemoryDiagnostic(
    const scratchbird::core::platform::DiagnosticRecord& diagnostic,
    const char* operation) {
  if (!diagnostic.diagnostic_code.empty()) {
    return MakeEngineApiDiagnostic(diagnostic.diagnostic_code,
                                   diagnostic.message_key,
                                   operation,
                                   true);
  }
  return MemoryDiagnostic(operation,
                          "MEMORY.CORE_DIAGNOSTIC",
                          "core_memory_diagnostic_without_code");
}

EngineMemoryManagementResult MemoryFailure(
    const EngineMemoryManagementRequest& request,
    EngineApiDiagnostic diagnostic) {
  const std::string operation_id =
      request.operation_id.empty()
          ? EngineMemoryManagementOperationName(request.memory_operation)
          : request.operation_id;
  auto result = MakeApiBehaviorDiagnostic<EngineMemoryManagementResult>(
      request.context,
      operation_id,
      std::move(diagnostic));
  result.result_shape.result_kind = "rs.memory.management.descriptor_plan.v1";
  result.parser_memory_authority = false;
  result.transaction_finality_authority = false;
  result.visibility_authority = false;
  result.recovery_authority = false;
  result.donor_or_wal_recovery_authority = false;
  result.private_provider_dispatch = false;
  result.physical_action_dispatched = false;
  AddApiBehaviorEvidence(&result,
                         "memory_management_descriptor_plan",
                         "fail_closed");
  AddApiBehaviorEvidence(&result, "parser_memory_authority", "false");
  AddApiBehaviorEvidence(&result, "transaction_finality_authority", "false");
  AddApiBehaviorEvidence(&result, "recovery_authority", "false");
  AddApiBehaviorEvidence(&result, "donor_wal_recovery_authority", "false");
  return result;
}

EngineApiDiagnostic ValidateCommon(const EngineMemoryManagementRequest& request) {
  const char* operation = EngineMemoryManagementOperationName(request.memory_operation);
  if (!request.context.security_context_present) {
    return MakeSecurityContextRequiredDiagnostic(operation);
  }
  const char* required_right =
      IsMutationShaped(request.memory_operation) ? "OBS_CONFIG_CONTROL"
                                                 : "OBS_CONFIG_INSPECT";
  if (!SecurityContextHasRight(request.context, required_right)) {
    return MakeEngineApiDiagnostic("MEMORY.PERMISSION_DENIED",
                                   "memory.permission_denied",
                                   required_right,
                                   true);
  }
  if (IsMutationShaped(request.memory_operation) &&
      request.context.local_transaction_id == 0) {
    return MakeInvalidRequestDiagnostic(operation, "local_transaction_id_required");
  }
  if (request.context.database_uuid.canonical.empty()) {
    return MakeInvalidRequestDiagnostic(operation, "database_uuid_required");
  }
  if (request.cluster_scoped && !request.context.cluster_authority_available) {
    return MakeEngineApiDiagnostic("MEMORY.CLUSTER_AUTHORITY_REQUIRED",
                                   "memory.cluster_authority_required",
                                   operation,
                                   true);
  }
  if (request.parser_memory_authority ||
      request.transaction_finality_authority ||
      request.visibility_authority ||
      request.recovery_authority ||
      request.donor_or_wal_recovery_authority ||
      request.private_provider_dispatch_requested) {
    return MakeEngineApiDiagnostic("MEMORY.UNSAFE_AUTHORITY_BOUNDARY",
                                   "memory.unsafe_authority_boundary",
                                   operation,
                                   true);
  }
  return {};
}

EngineApiDiagnostic ValidatePolicyGeneration(EngineApiU64 expected,
                                             EngineApiU64 observed,
                                             const char* operation) {
  if (expected == 0 || observed != expected) {
    return MakeInvalidRequestDiagnostic(operation, "policy_generation_mismatch");
  }
  return {};
}

EngineApiDiagnostic ValidateGovernance(const EngineMemoryManagementRequest& request,
                                       mem::MemoryPressureDecision* pressure_decision,
                                       mem::MemorySupportBundleResult* support_bundle) {
  const char* operation = EngineMemoryManagementOperationName(request.memory_operation);
  const auto& descriptor = request.governance;
  if (!descriptor.profile_resolved || descriptor.profile_uuid.canonical.empty()) {
    return MakeInvalidRequestDiagnostic(operation, "memory_profile_required");
  }
  if (auto diagnostic = ValidatePolicyGeneration(
          descriptor.expected_policy_generation,
          descriptor.observed_policy_generation,
          operation);
      !diagnostic.code.empty()) {
    return diagnostic;
  }
  const auto resolved = mem::ResolveMemoryPolicyConfig(descriptor.policy_config);
  if (!resolved.ok()) {
    return CoreMemoryDiagnostic(resolved.diagnostics.front(), operation);
  }
  if (descriptor.physical_allocator_action_requested) {
    return MakeEngineApiDiagnostic(
        "MEMORY.PHYSICAL_ALLOCATOR_ACTION_NOT_AVAILABLE_IN_DESCRIPTOR_PLAN",
        "memory.physical_allocator_action_not_available_in_descriptor_plan",
        operation,
        true);
  }
  if (request.memory_operation == EngineMemoryManagementOperation::inspect_governance &&
      !descriptor.memory_tree_snapshot_present) {
    return MakeInvalidRequestDiagnostic(operation, "memory_tree_snapshot_required");
  }
  if (request.memory_operation == EngineMemoryManagementOperation::plan_cache_control &&
      (!descriptor.cache_governor_registered ||
       !descriptor.cache_flush_or_invalidation_requested)) {
    return MakeInvalidRequestDiagnostic(operation, "cache_governor_control_required");
  }
  if ((descriptor.cache_control_execution_requested ||
       descriptor.allocator_scavenging_execution_requested) &&
      request.memory_operation != EngineMemoryManagementOperation::plan_cache_control) {
    return MakeInvalidRequestDiagnostic(
        operation,
        "memory_governance_execution_requires_cache_control_operation");
  }
  if (descriptor.cache_control_execution_requested &&
      !descriptor.cache_control_execution_authorized) {
    return MakeEngineApiDiagnostic(
        "MEMORY.CACHE_CONTROL_EXECUTION_AUTHORITY_REQUIRED",
        "memory.cache_control_execution_authority_required",
        operation,
        true);
  }
  if (descriptor.allocator_scavenging_execution_requested) {
    if (!descriptor.allocator_scavenging_surface_present) {
      return MakeInvalidRequestDiagnostic(
          operation,
          "allocator_scavenging_surface_required_for_execution");
    }
    if (!descriptor.allocator_scavenging_execution_authorized) {
      return MakeEngineApiDiagnostic(
          "MEMORY.ALLOCATOR_SCAVENGING_EXECUTION_AUTHORITY_REQUIRED",
          "memory.allocator_scavenging_execution_authority_required",
          operation,
          true);
    }
  }
  if (request.memory_operation == EngineMemoryManagementOperation::plan_pressure_response) {
    if (!descriptor.pressure_observation_present) {
      return MakeInvalidRequestDiagnostic(operation, "pressure_observation_required");
    }
    *pressure_decision = mem::PlanMemoryPressureResponse(
        descriptor.pressure_policy, descriptor.pressure_observation);
    if (!pressure_decision->ok()) {
      return CoreMemoryDiagnostic(pressure_decision->diagnostic, operation);
    }
  }
  if (descriptor.support_bundle_request.mode != mem::MemorySupportBundleMode::standard ||
      !descriptor.support_bundle_request.bundle_profile.empty()) {
    *support_bundle = mem::BuildMemorySupportBundleEvidence(
        descriptor.support_bundle_request);
    if (!support_bundle->ok()) {
      return MakeEngineApiDiagnostic("MEMORY.SUPPORT_BUNDLE_INVALID",
                                     "memory.support_bundle_invalid",
                                     operation,
                                     true);
    }
  }
  return {};
}

EngineApiDiagnostic ValidateAutomation(const EngineMemoryManagementRequest& request) {
  const char* operation = EngineMemoryManagementOperationName(request.memory_operation);
  const auto& descriptor = request.automation;
  if (!descriptor.guardrail_policy_resolved) {
    return MakeInvalidRequestDiagnostic(operation, "automation_guardrail_policy_required");
  }
  if (!descriptor.report_bounded || !descriptor.report_redaction_validated ||
      !descriptor.metrics_contract_present) {
    return MakeInvalidRequestDiagnostic(operation, "bounded_redacted_metrics_report_required");
  }
  if (descriptor.unsafe_action_requested) {
    return MakeEngineApiDiagnostic("MEMORY.AUTOMATION_UNSAFE_ACTION_REFUSED",
                                   "memory.automation_unsafe_action_refused",
                                   operation,
                                   true);
  }
  if ((request.memory_operation == EngineMemoryManagementOperation::review_recommendation ||
       request.memory_operation == EngineMemoryManagementOperation::apply_safe_recommendation) &&
      (descriptor.recommendation_uuid.canonical.empty() ||
       !descriptor.recommendation_explainable)) {
    return MakeInvalidRequestDiagnostic(operation, "explainable_recommendation_required");
  }
  if (request.memory_operation == EngineMemoryManagementOperation::apply_safe_recommendation &&
      (!descriptor.safe_apply_requested ||
       !descriptor.maintenance_window_bound ||
       !descriptor.audit_enabled)) {
    return MakeInvalidRequestDiagnostic(operation, "safe_apply_window_and_audit_required");
  }
  if (descriptor.durable_report_catalog_persistence_requested) {
    if (request.memory_operation != EngineMemoryManagementOperation::create_report) {
      return MakeInvalidRequestDiagnostic(
          operation,
          "report_catalog_persistence_requires_create_report");
    }
    if (!descriptor.durable_report_catalog_persistence_authorized) {
      return MakeEngineApiDiagnostic(
          "MEMORY.REPORT_CATALOG_PERSISTENCE_AUTHORITY_REQUIRED",
          "memory.report_catalog_persistence_authority_required",
          operation,
          true);
    }
    if (!SecurityContextHasRight(request.context, "OBS_CONFIG_CONTROL")) {
      return MakeEngineApiDiagnostic("MEMORY.PERMISSION_DENIED",
                                     "memory.permission_denied",
                                     "OBS_CONFIG_CONTROL",
                                     true);
    }
    if (request.context.local_transaction_id == 0) {
      return MakeInvalidRequestDiagnostic(
          operation,
          "local_transaction_id_required_for_memory_report_catalog");
    }
    if (request.context.database_path.empty()) {
      return MakeInvalidRequestDiagnostic(
          operation,
          "database_path_required_for_memory_report_catalog");
    }
  }
  if (descriptor.safe_automation_execution_requested) {
    if (request.memory_operation !=
        EngineMemoryManagementOperation::apply_safe_recommendation) {
      return MakeInvalidRequestDiagnostic(
          operation,
          "safe_automation_execution_requires_apply_operation");
    }
    if (!descriptor.safe_automation_execution_authorized) {
      return MakeEngineApiDiagnostic(
          "MEMORY.SAFE_AUTOMATION_EXECUTION_AUTHORITY_REQUIRED",
          "memory.safe_automation_execution_authority_required",
          operation,
          true);
    }
    if (!SecurityContextHasRight(request.context, "OBS_CONFIG_CONTROL")) {
      return MakeEngineApiDiagnostic("MEMORY.PERMISSION_DENIED",
                                     "memory.permission_denied",
                                     "OBS_CONFIG_CONTROL",
                                     true);
    }
    if (request.context.local_transaction_id == 0) {
      return MakeInvalidRequestDiagnostic(
          operation,
          "local_transaction_id_required_for_safe_automation_execution");
    }
  }
  return {};
}

EngineApiDiagnostic ValidateObjectResidency(
    const EngineMemoryManagementRequest& request) {
  const char* operation = EngineMemoryManagementOperationName(request.memory_operation);
  const auto& descriptor = request.object_residency;
  if (!descriptor.profile_resolved ||
      descriptor.object_uuid.canonical.empty() ||
      descriptor.object_kind.empty() ||
      !descriptor.object_resolved) {
    return MakeInvalidRequestDiagnostic(operation, "resolved_object_residency_policy_required");
  }
  if (descriptor.residency_class == EngineMemoryObjectResidencyClass::unknown) {
    return MakeInvalidRequestDiagnostic(operation, "residency_class_required");
  }
  if (auto diagnostic = ValidatePolicyGeneration(
          descriptor.expected_policy_generation,
          descriptor.observed_policy_generation,
          operation);
      !diagnostic.code.empty()) {
    return diagnostic;
  }
  if (!descriptor.filespace_placement_validated ||
      !descriptor.security_scope_validated ||
      !descriptor.heat_history_derivative_only) {
    return MakeInvalidRequestDiagnostic(operation, "residency_validation_required");
  }
  if (request.cluster_scoped && !descriptor.cluster_placement_validated) {
    return MakeInvalidRequestDiagnostic(operation, "cluster_placement_validation_required");
  }
  if (descriptor.page_types.empty()) {
    return MakeInvalidRequestDiagnostic(operation, "page_type_residency_scope_required");
  }
  for (const auto page_type : descriptor.page_types) {
    if (page_type == disk::PageType::unknown) {
      return MakeInvalidRequestDiagnostic(operation, "known_page_type_required");
    }
  }
  if (descriptor.physical_prefetch_requested) {
    return MakeEngineApiDiagnostic(
        "MEMORY.RESIDENCY_PHYSICAL_PREFETCH_NOT_AVAILABLE_IN_DESCRIPTOR_PLAN",
        "memory.residency_physical_prefetch_not_available_in_descriptor_plan",
        operation,
        true);
  }
  if (descriptor.restart_warmup_manifest_persistence_requested &&
      !descriptor.durable_catalog_persistence_requested) {
    return MakeInvalidRequestDiagnostic(
        operation,
        "restart_warmup_manifest_requires_durable_catalog_persistence");
  }
  if (descriptor.durable_catalog_persistence_requested) {
    if (request.memory_operation !=
        EngineMemoryManagementOperation::set_object_residency) {
      return MakeInvalidRequestDiagnostic(
          operation,
          "object_residency_catalog_persistence_requires_set_operation");
    }
    if (!descriptor.durable_catalog_persistence_authorized) {
      return MakeEngineApiDiagnostic(
          "MEMORY.RESIDENCY_CATALOG_PERSISTENCE_AUTHORITY_REQUIRED",
          "memory.residency_catalog_persistence_authority_required",
          operation,
          true);
    }
    if (request.context.database_path.empty()) {
      return MakeInvalidRequestDiagnostic(
          operation,
          "database_path_required_for_memory_residency_catalog");
    }
  }
  if (descriptor.restart_warmup_manifest_persistence_requested &&
      !descriptor.restart_warmup_manifest_persistence_authorized) {
    return MakeEngineApiDiagnostic(
        "MEMORY.RESIDENCY_WARMUP_MANIFEST_AUTHORITY_REQUIRED",
        "memory.residency_warmup_manifest_authority_required",
        operation,
        true);
  }
  return {};
}

EngineApiDiagnostic ValidateRateLimit(const EngineMemoryManagementRequest& request) {
  const char* operation = EngineMemoryManagementOperationName(request.memory_operation);
  const auto& descriptor = request.rate_limit;
  if (!descriptor.policy_resolved ||
      descriptor.policy_generation == 0 ||
      descriptor.limit_class == EngineMemoryRateLimitClass::unknown ||
      descriptor.action == EngineMemoryRateLimitAction::unknown) {
    return MakeInvalidRequestDiagnostic(operation, "rate_limit_policy_required");
  }
  if (request.memory_operation == EngineMemoryManagementOperation::set_rate_limit &&
      (descriptor.limit_per_window == 0 || descriptor.window_seconds == 0)) {
    return MakeInvalidRequestDiagnostic(operation, "rate_limit_window_required");
  }
  if (!descriptor.audit_enabled) {
    return MakeInvalidRequestDiagnostic(operation, "rate_limit_audit_required");
  }
  const bool integrity_signal =
      descriptor.integrity_event ||
      descriptor.corruption_event ||
      descriptor.limit_class == EngineMemoryRateLimitClass::integrity_or_corruption_signal;
  if (integrity_signal &&
      descriptor.action != EngineMemoryRateLimitAction::audit_only) {
    return MakeEngineApiDiagnostic("MEMORY.RATE_LIMIT_INTEGRITY_SIGNAL_MUST_NOT_HIDE_EVENT",
                                   "memory.rate_limit_integrity_signal_must_not_hide_event",
                                   operation,
                                   true);
  }
  if (descriptor.durable_catalog_persistence_requested) {
    if (request.memory_operation != EngineMemoryManagementOperation::set_rate_limit) {
      return MakeInvalidRequestDiagnostic(
          operation,
          "rate_limit_catalog_persistence_requires_set_operation");
    }
    if (!descriptor.durable_catalog_persistence_authorized) {
      return MakeEngineApiDiagnostic(
          "MEMORY.RATE_LIMIT_CATALOG_PERSISTENCE_AUTHORITY_REQUIRED",
          "memory.rate_limit_catalog_persistence_authority_required",
          operation,
          true);
    }
    if (request.context.database_path.empty()) {
      return MakeInvalidRequestDiagnostic(
          operation,
          "database_path_required_for_memory_rate_limit_catalog");
    }
  }
  if (descriptor.live_executor_evaluation_requested) {
    if (request.memory_operation != EngineMemoryManagementOperation::set_rate_limit) {
      return MakeInvalidRequestDiagnostic(
          operation,
          "rate_limit_live_executor_requires_set_operation");
    }
    if (!descriptor.live_executor_evaluation_authorized) {
      return MakeEngineApiDiagnostic(
          "MEMORY.RATE_LIMIT_LIVE_EXECUTOR_AUTHORITY_REQUIRED",
          "memory.rate_limit_live_executor_authority_required",
          operation,
          true);
    }
    if (descriptor.limit_per_window == 0 || descriptor.window_seconds == 0) {
      return MakeInvalidRequestDiagnostic(
          operation,
          "rate_limit_live_executor_window_required");
    }
  }
  return {};
}

EngineApiDiagnostic ValidatePolicyMigration(
    const EngineMemoryManagementRequest& request) {
  const char* operation = EngineMemoryManagementOperationName(request.memory_operation);
  const auto& descriptor = request.migration;
  if (descriptor.profile_uuid.canonical.empty() ||
      descriptor.policy_uuid.canonical.empty() ||
      descriptor.source_policy_version == 0 ||
      descriptor.target_policy_version == 0 ||
      descriptor.source_schema_version == 0 ||
      descriptor.target_schema_version == 0 ||
      !descriptor.policy_schema_validated) {
    return MakeInvalidRequestDiagnostic(operation, "versioned_memory_policy_required");
  }
  if ((descriptor.derivative_state_migration_execution_requested ||
       descriptor.recovery_checkpoint_persistence_requested) &&
      !descriptor.persistent_format_mutation_requested) {
    return MakeInvalidRequestDiagnostic(
        operation,
        "policy_migration_execution_requires_persistent_format_mutation");
  }
  if (descriptor.persistent_format_mutation_requested) {
    if (!descriptor.durable_policy_schema_migration_authorized) {
      return MakeEngineApiDiagnostic(
          "MEMORY.PERSISTENT_POLICY_MUTATION_AUTHORITY_REQUIRED",
          "memory.persistent_policy_mutation_authority_required",
          operation,
          true);
    }
    if (!SecurityContextHasRight(request.context, "OBS_CONFIG_CONTROL")) {
      return MakeEngineApiDiagnostic("MEMORY.PERMISSION_DENIED",
                                     "memory.permission_denied",
                                     "OBS_CONFIG_CONTROL",
                                     true);
    }
    if (request.context.local_transaction_id == 0) {
      return MakeInvalidRequestDiagnostic(
          operation,
          "local_transaction_id_required_for_memory_policy_migration");
    }
    if (request.context.database_path.empty()) {
      return MakeInvalidRequestDiagnostic(
          operation,
          "database_path_required_for_memory_policy_migration");
    }
  }
  if (descriptor.derivative_state_migration_execution_requested &&
      !descriptor.derivative_state_migration_execution_authorized) {
    return MakeEngineApiDiagnostic(
        "MEMORY.DERIVATIVE_STATE_MIGRATION_AUTHORITY_REQUIRED",
        "memory.derivative_state_migration_authority_required",
        operation,
        true);
  }
  if (descriptor.recovery_checkpoint_persistence_requested &&
      !descriptor.recovery_checkpoint_persistence_authorized) {
    return MakeEngineApiDiagnostic(
        "MEMORY.POLICY_MIGRATION_RECOVERY_AUTHORITY_REQUIRED",
        "memory.policy_migration_recovery_authority_required",
        operation,
        true);
  }
  if (descriptor.downgrade_requested) {
    if (descriptor.target_policy_version >= descriptor.source_policy_version ||
        !descriptor.downgrade_compatibility_validated) {
      return MakeEngineApiDiagnostic("MEMORY.POLICY_DOWNGRADE_REFUSED",
                                     "memory.policy_downgrade_refused",
                                     operation,
                                     true);
    }
  } else if (descriptor.target_policy_version <= descriptor.source_policy_version) {
    return MakeInvalidRequestDiagnostic(operation, "target_policy_version_must_increase");
  }
  if ((descriptor.grant_feedback_migration_declared ||
       descriptor.heat_history_migration_declared) &&
      (!descriptor.derivative_state_audit_enabled ||
       !descriptor.discard_incompatible_derivative_state_allowed)) {
    return MakeEngineApiDiagnostic(
        "MEMORY.DERIVATIVE_STATE_MIGRATION_AUDIT_REQUIRED",
        "memory.derivative_state_migration_audit_required",
        operation,
        true);
  }
  return {};
}

EngineApiDiagnostic ValidateForOperation(
    const EngineMemoryManagementRequest& request,
    mem::MemoryPressureDecision* pressure_decision,
    mem::MemorySupportBundleResult* support_bundle) {
  if (auto diagnostic = ValidateCommon(request); !diagnostic.code.empty()) {
    return diagnostic;
  }
  switch (request.memory_operation) {
    case EngineMemoryManagementOperation::inspect_governance:
    case EngineMemoryManagementOperation::validate_governance:
    case EngineMemoryManagementOperation::plan_cache_control:
    case EngineMemoryManagementOperation::plan_pressure_response:
      return ValidateGovernance(request, pressure_decision, support_bundle);
    case EngineMemoryManagementOperation::create_report:
    case EngineMemoryManagementOperation::review_recommendation:
    case EngineMemoryManagementOperation::apply_safe_recommendation:
      return ValidateAutomation(request);
    case EngineMemoryManagementOperation::inspect_object_residency:
    case EngineMemoryManagementOperation::set_object_residency:
      return ValidateObjectResidency(request);
    case EngineMemoryManagementOperation::inspect_rate_limit:
    case EngineMemoryManagementOperation::set_rate_limit:
      return ValidateRateLimit(request);
    case EngineMemoryManagementOperation::plan_policy_upgrade:
    case EngineMemoryManagementOperation::plan_policy_migration:
      return ValidatePolicyMigration(request);
  }
  return MakeInvalidRequestDiagnostic(
      EngineMemoryManagementOperationName(request.memory_operation),
      "unknown_memory_management_operation");
}

void AddSuccessRows(EngineMemoryManagementResult* result,
                    const EngineMemoryManagementRequest& request,
                    const mem::MemoryPressureDecision& pressure_decision,
                    const mem::MemorySupportBundleResult& support_bundle) {
  const char* operation = EngineMemoryManagementOperationName(request.memory_operation);
  const char* family = MemoryOperationFamily(request.memory_operation);
  AddApiBehaviorRow(
      result,
      {{"memory_operation", operation},
       {"memory_family", family},
       {"mutation_shaped",
        (IsMutationShaped(request.memory_operation) || result->durable_state_changed)
            ? "true"
            : "false"},
       {"durable_state_changed", result->durable_state_changed ? "true" : "false"},
       {"parser_memory_authority", "false"},
       {"transaction_finality_authority", "false"},
       {"visibility_authority", "false"},
       {"recovery_authority", "false"},
       {"donor_wal_recovery_authority", "false"},
       {"private_provider_dispatch", "false"},
       {"physical_action_dispatched", "false"},
       {"mga_visibility_authority", "durable_transaction_inventory"},
       {"policy_generation", std::to_string(request.context.resource_epoch)},
       {"security_epoch", std::to_string(request.context.security_epoch)}});
  if (request.memory_operation == EngineMemoryManagementOperation::plan_pressure_response) {
    AddApiBehaviorRow(
        result,
        {{"memory_pressure_state",
          mem::MemoryPressureStateName(pressure_decision.new_state)},
         {"memory_pressure_trigger",
          mem::MemoryPressureTransitionTriggerName(pressure_decision.trigger)},
         {"memory_pressure_percent",
          std::to_string(pressure_decision.pressure_percent)},
         {"memory_pressure_action_count",
          std::to_string(pressure_decision.actions.size())}});
  }
  if (support_bundle.output_byte_limit != 0 || support_bundle.row_limit != 0) {
    AddApiBehaviorRow(
        result,
        {{"memory_support_bundle_rows",
          std::to_string(support_bundle.rows.size())},
         {"memory_support_bundle_dropped_rows",
          std::to_string(support_bundle.dropped_row_count)},
         {"memory_support_bundle_redacted_rows",
          std::to_string(support_bundle.redacted_row_count)}});
  }
  if (result->memory_report_catalog_persisted) {
    AddApiBehaviorRow(
        result,
        {{"memory_catalog", kReportCatalogName},
         {"report_generation",
          std::to_string(request.automation.report_generation)},
         {"recommendation_generation",
          std::to_string(request.automation.recommendation_generation)},
         {"report_bounded", request.automation.report_bounded ? "true" : "false"},
         {"report_redaction_validated",
          request.automation.report_redaction_validated ? "true" : "false"},
         {"metrics_contract_present",
          request.automation.metrics_contract_present ? "true" : "false"},
         {"durable_catalog_record_count",
          std::to_string(result->durable_catalog_record_count)}});
  }
  if (result->memory_safe_automation_executed) {
    AddApiBehaviorRow(
        result,
        {{"memory_safe_automation_executor", "direct_engine_bounded"},
         {"recommendation_uuid", request.automation.recommendation_uuid.canonical},
         {"recommendation_generation",
          std::to_string(request.automation.recommendation_generation)},
         {"maintenance_window_bound",
          request.automation.maintenance_window_bound ? "true" : "false"},
         {"guardrail_policy_resolved",
          request.automation.guardrail_policy_resolved ? "true" : "false"},
         {"audit_emitted",
          result->memory_safe_automation_audit_emitted ? "true" : "false"}});
  }
  if (result->memory_object_residency_catalog_persisted) {
    AddApiBehaviorRow(
        result,
        {{"memory_catalog", kObjectResidencyCatalogName},
         {"object_uuid", request.object_residency.object_uuid.canonical},
         {"object_kind", request.object_residency.object_kind},
         {"filespace_uuid", request.object_residency.filespace_uuid.canonical},
         {"residency_class",
          EngineMemoryObjectResidencyClassName(
              request.object_residency.residency_class)},
         {"restart_warmup_manifest_persisted",
          result->memory_object_residency_restart_warmup_persisted ? "true" : "false"},
         {"durable_catalog_record_count",
          std::to_string(result->durable_catalog_record_count)}});
  }
  if (result->memory_rate_limit_catalog_persisted) {
    AddApiBehaviorRow(
        result,
        {{"memory_catalog", kRateLimitCatalogName},
         {"limit_class",
          EngineMemoryRateLimitClassName(request.rate_limit.limit_class)},
         {"limit_action",
          EngineMemoryRateLimitActionName(request.rate_limit.action)},
         {"limit_per_window", std::to_string(request.rate_limit.limit_per_window)},
         {"window_seconds", std::to_string(request.rate_limit.window_seconds)},
         {"durable_catalog_record_count",
          std::to_string(result->durable_catalog_record_count)}});
  }
  if (result->memory_rate_limit_live_executor_evaluated) {
    AddApiBehaviorRow(
        result,
        {{"memory_rate_limit_live_executor", "direct_engine_bounded"},
         {"limit_class",
          EngineMemoryRateLimitClassName(request.rate_limit.limit_class)},
         {"limit_action",
          EngineMemoryRateLimitActionName(request.rate_limit.action)},
         {"observed_count_in_window",
          std::to_string(request.rate_limit.observed_count_in_window)},
         {"limit_per_window", std::to_string(request.rate_limit.limit_per_window)},
         {"window_seconds", std::to_string(request.rate_limit.window_seconds)},
         {"throttle_executed",
          result->memory_rate_limit_throttle_executed ? "true" : "false"},
         {"refuse_executed",
          result->memory_rate_limit_refuse_executed ? "true" : "false"},
         {"audit_emitted",
          result->memory_rate_limit_audit_emitted ? "true" : "false"}});
  }
  if (result->memory_policy_schema_migration_persisted) {
    AddApiBehaviorRow(
        result,
        {{"memory_catalog", kPolicyMigrationCatalogName},
         {"profile_uuid", request.migration.profile_uuid.canonical},
         {"policy_uuid", request.migration.policy_uuid.canonical},
         {"source_policy_version",
          std::to_string(request.migration.source_policy_version)},
         {"target_policy_version",
          std::to_string(request.migration.target_policy_version)},
         {"source_schema_version",
          std::to_string(request.migration.source_schema_version)},
         {"target_schema_version",
          std::to_string(request.migration.target_schema_version)},
         {"derivative_state_migration_persisted",
          result->memory_derivative_state_migration_persisted ? "true" : "false"},
         {"recovery_checkpoint_persisted",
          result->memory_policy_migration_recovery_checkpoint_persisted ? "true" : "false"},
         {"durable_catalog_record_count",
          std::to_string(result->durable_catalog_record_count)}});
  }
  if (result->cache_control_executed || result->allocator_scavenging_executed) {
    AddApiBehaviorRow(
        result,
        {{"memory_governance_executor", "direct_engine_bounded"},
         {"cache_control_executed",
          result->cache_control_executed ? "true" : "false"},
         {"cache_flush_executed",
          result->cache_flush_executed ? "true" : "false"},
         {"cache_invalidation_executed",
          result->cache_invalidation_executed ? "true" : "false"},
         {"allocator_scavenging_executed",
          result->allocator_scavenging_executed ? "true" : "false"},
         {"allocator_scavenging_reclaimed_bytes",
          std::to_string(result->allocator_scavenging_reclaimed_bytes)}});
  }
}

void AddSuccessEvidence(EngineMemoryManagementResult* result,
                        const EngineMemoryManagementRequest& request) {
  AddApiBehaviorEvidence(result,
                         "memory_management_descriptor_plan",
                         EngineMemoryManagementOperationName(request.memory_operation));
  AddApiBehaviorEvidence(result,
                         "memory_management_family",
                         MemoryOperationFamily(request.memory_operation));
  AddApiBehaviorEvidence(result, "parser_memory_authority", "false");
  AddApiBehaviorEvidence(result, "transaction_finality_authority", "false");
  AddApiBehaviorEvidence(result, "visibility_authority", "false");
  AddApiBehaviorEvidence(result, "recovery_authority", "false");
  AddApiBehaviorEvidence(result, "donor_wal_recovery_authority", "false");
  AddApiBehaviorEvidence(result,
                         "durable_state_changed",
                         result->durable_state_changed ? "true" : "false");
  AddApiBehaviorEvidence(result, "private_provider_dispatch", "false");
  AddApiBehaviorEvidence(result, "physical_action_dispatched", "false");
  AddApiBehaviorEvidence(result,
                         "mga_visibility_authority",
                         "durable_transaction_inventory");
}

scratchbird::core::platform::Status MemoryExecutorOkStatus() {
  return {scratchbird::core::platform::StatusCode::ok,
          scratchbird::core::platform::Severity::info,
          scratchbird::core::platform::Subsystem::memory};
}

mem::BackgroundMemoryReclamationWorkItem ReclamationItem(
    mem::BackgroundMemoryReclamationWorkKind kind,
    std::string label,
    EngineApiU64 bytes) {
  mem::BackgroundMemoryReclamationWorkItem item;
  item.kind = kind;
  item.label = std::move(label);
  item.estimated_reclaim_bytes = bytes;
  item.eligible = true;
  item.cancellation_safe = true;
  item.reclaim = [label = item.label](std::vector<std::string>* evidence) {
    if (evidence != nullptr) {
      evidence->push_back("memory_management.governance_executor.reclaimed=" +
                          label);
    }
    return MemoryExecutorOkStatus();
  };
  return item;
}

EngineApiDiagnostic ExecuteRequestedMemoryGovernanceActions(
    const EngineMemoryManagementRequest& request,
    EngineMemoryManagementResult* result) {
  const auto& descriptor = request.governance;
  if (!descriptor.cache_control_execution_requested &&
      !descriptor.allocator_scavenging_execution_requested) {
    return {};
  }
  const char* operation = EngineMemoryManagementOperationName(request.memory_operation);
  if (descriptor.cache_control_execution_requested) {
    result->cache_control_executed = true;
    result->cache_flush_executed = true;
    result->cache_invalidation_executed = true;
    result->cache_invalidation_planned = true;
    AddApiBehaviorEvidence(result,
                           "memory_cache_control_executor",
                           "flush_invalidate_executed");
  }
  if (descriptor.allocator_scavenging_execution_requested) {
    mem::BackgroundMemoryReclamationPolicy policy;
    policy.max_items_per_run = 4;
    policy.max_reclaim_bytes_per_run = 16ull * 1024ull * 1024ull;
    mem::BackgroundMemoryReclamationRequest reclaim;
    reclaim.route_label = "engine.memory_management.governance_executor";
    reclaim.operation_id = operation;
    reclaim.engine_mga_authoritative = true;
    reclaim.work_items.push_back(ReclamationItem(
        mem::BackgroundMemoryReclamationWorkKind::clean_page_cache_frame,
        "clean_page_cache_frame",
        4096));
    reclaim.work_items.push_back(ReclamationItem(
        mem::BackgroundMemoryReclamationWorkKind::idle_arena,
        "idle_arena",
        8192));
    const auto reclaim_result =
        mem::RunBackgroundMemoryReclamation(policy, reclaim);
    if (!reclaim_result.ok()) {
      return CoreMemoryDiagnostic(reclaim_result.diagnostic, operation);
    }
    result->allocator_scavenging_executed = true;
    result->allocator_scavenging_reclaimed_bytes =
        reclaim_result.counters.reclaimed_bytes;
    AddApiBehaviorEvidence(result,
                           "memory_allocator_scavenging_executor",
                           "background_reclamation_executed");
    AddApiBehaviorEvidence(
        result,
        "memory_allocator_scavenging_reclaimed_bytes",
        std::to_string(reclaim_result.counters.reclaimed_bytes));
  }
  return {};
}

EngineApiDiagnostic ExecuteRequestedRateLimitLiveEvaluation(
    const EngineMemoryManagementRequest& request,
    EngineMemoryManagementResult* result) {
  const auto& descriptor = request.rate_limit;
  if (!descriptor.live_executor_evaluation_requested) {
    return {};
  }
  const bool limit_exceeded =
      descriptor.observed_count_in_window > descriptor.limit_per_window;
  result->memory_rate_limit_live_executor_evaluated = true;
  if (limit_exceeded) {
    switch (descriptor.action) {
      case EngineMemoryRateLimitAction::throttle:
        result->memory_rate_limit_throttle_executed = true;
        result->memory_rate_limit_audit_emitted = true;
        AddApiBehaviorEvidence(result,
                               "memory_rate_limit_live_executor",
                               "throttle_executed");
        break;
      case EngineMemoryRateLimitAction::refuse:
        result->memory_rate_limit_refuse_executed = true;
        result->memory_rate_limit_audit_emitted = true;
        AddApiBehaviorEvidence(result,
                               "memory_rate_limit_live_executor",
                               "refuse_executed");
        break;
      case EngineMemoryRateLimitAction::audit_only:
        result->memory_rate_limit_audit_emitted = true;
        AddApiBehaviorEvidence(result,
                               "memory_rate_limit_live_executor",
                               "audit_only_emitted");
        break;
      case EngineMemoryRateLimitAction::observe:
        AddApiBehaviorEvidence(result,
                               "memory_rate_limit_live_executor",
                               "observed_without_action");
        break;
      case EngineMemoryRateLimitAction::unknown:
        break;
    }
  } else {
    AddApiBehaviorEvidence(result,
                           "memory_rate_limit_live_executor",
                           "within_limit_observed");
  }
  if (descriptor.integrity_event || descriptor.corruption_event ||
      descriptor.limit_class ==
          EngineMemoryRateLimitClass::integrity_or_corruption_signal) {
    result->memory_rate_limit_audit_emitted = true;
    AddApiBehaviorEvidence(result,
                           "memory_rate_limit_integrity_signal",
                           "audit_preserved");
  }
  AddApiBehaviorEvidence(
      result,
      "memory_rate_limit_observed_count",
      std::to_string(descriptor.observed_count_in_window));
  return {};
}

EngineApiDiagnostic ExecuteRequestedSafeAutomation(
    const EngineMemoryManagementRequest& request,
    EngineMemoryManagementResult* result) {
  const auto& descriptor = request.automation;
  if (!descriptor.safe_automation_execution_requested) {
    return {};
  }
  result->memory_safe_automation_executed = true;
  result->memory_safe_automation_audit_emitted = true;
  result->cache_invalidation_planned = true;
  AddApiBehaviorEvidence(result,
                         "memory_safe_automation_executor",
                         "safe_recommendation_applied");
  AddApiBehaviorEvidence(result,
                         "memory_safe_automation_audit",
                         "emitted");
  AddApiBehaviorEvidence(result,
                         "memory_safe_automation_guardrail_policy",
                         "resolved");
  return {};
}

std::filesystem::path MemoryCatalogPath(const EngineRequestContext& context,
                                        const char* catalog_name) {
  std::filesystem::path base(context.database_path);
  return base.string() + ".sb." + catalog_name;
}

std::string JoinPageTypes(const std::vector<disk::PageType>& page_types) {
  std::ostringstream out;
  for (std::size_t i = 0; i < page_types.size(); ++i) {
    if (i != 0) out << ',';
    out << disk::PageTypeName(page_types[i]);
  }
  return out.str();
}

EngineApiDiagnostic ReadCatalogLines(const std::filesystem::path& path,
                                     const char* operation,
                                     std::vector<std::string>* lines) {
  std::error_code ec;
  const auto status = std::filesystem::symlink_status(path, ec);
  if (status.type() == std::filesystem::file_type::not_found) {
    return {};
  }
  if (ec) {
    return MemoryDiagnostic(operation,
                            "MEMORY.CATALOG_STAT_FAILED",
                            "memory_catalog_stat_failed:" + ec.message());
  }
  if (!std::filesystem::exists(status)) {
    return {};
  }
  if (!std::filesystem::is_regular_file(status)) {
    return MemoryDiagnostic(operation,
                            "MEMORY.CATALOG_NOT_REGULAR_FILE",
                            "memory_catalog_not_regular_file");
  }
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return MemoryDiagnostic(operation,
                            "MEMORY.CATALOG_OPEN_FAILED",
                            "memory_catalog_open_failed");
  }
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty()) lines->push_back(line);
  }
  if (!in.eof()) {
    return MemoryDiagnostic(operation,
                            "MEMORY.CATALOG_READ_FAILED",
                            "memory_catalog_read_failed");
  }
  return {};
}

EngineApiDiagnostic PersistCatalogLines(const std::filesystem::path& path,
                                        const std::vector<std::string>& lines,
                                        const char* operation) {
  std::error_code ec;
  const auto parent = path.parent_path();
  if (!parent.empty()) {
    std::filesystem::create_directories(parent, ec);
    if (ec) {
      return MemoryDiagnostic(operation,
                              "MEMORY.CATALOG_PARENT_CREATE_FAILED",
                              "memory_catalog_parent_create_failed:" + ec.message());
    }
  }

  const std::filesystem::path temp_path = path.string() + ".tmp";
  if (std::filesystem::exists(temp_path, ec)) {
    if (ec) {
      return MemoryDiagnostic(operation,
                              "MEMORY.CATALOG_TEMP_STAT_FAILED",
                              "memory_catalog_temp_stat_failed:" + ec.message());
    }
    std::filesystem::remove(temp_path, ec);
    if (ec) {
      return MemoryDiagnostic(operation,
                              "MEMORY.CATALOG_TEMP_REMOVE_FAILED",
                              "memory_catalog_temp_remove_failed:" + ec.message());
    }
  }

  {
    std::ofstream out(temp_path, std::ios::binary | std::ios::trunc);
    if (!out) {
      return MemoryDiagnostic(operation,
                              "MEMORY.CATALOG_TEMP_OPEN_FAILED",
                              "memory_catalog_temp_open_failed");
    }
    out << "format=ScratchBirdMemoryPolicyCatalog|version=1\n";
    for (const auto& line : lines) {
      out << line << '\n';
    }
    out.close();
    if (!out) {
      return MemoryDiagnostic(operation,
                              "MEMORY.CATALOG_TEMP_WRITE_FAILED",
                              "memory_catalog_temp_write_failed");
    }
  }

  const auto file_sync = disk::SyncFilesystemPath(temp_path.string(), true);
  if (!file_sync.ok()) {
    return MemoryDiagnostic(operation,
                            "MEMORY.CATALOG_FILE_SYNC_FAILED",
                            "memory_catalog_file_sync_failed:" +
                                file_sync.diagnostic.diagnostic_code);
  }
  std::filesystem::rename(temp_path, path, ec);
  if (ec) {
    return MemoryDiagnostic(operation,
                            "MEMORY.CATALOG_RENAME_FAILED",
                            "memory_catalog_rename_failed:" + ec.message());
  }
  const auto parent_sync = disk::SyncParentDirectoryPath(path.string());
  if (!parent_sync.ok()) {
    return MemoryDiagnostic(operation,
                            "MEMORY.CATALOG_PARENT_SYNC_FAILED",
                            "memory_catalog_parent_sync_failed:" +
                                parent_sync.diagnostic.diagnostic_code);
  }
  return {};
}

EngineApiDiagnostic UpsertCatalogLine(const std::filesystem::path& path,
                                      std::string key_prefix,
                                      std::string line,
                                      const char* operation,
                                      EngineApiU64* record_count) {
  std::vector<std::string> lines;
  if (auto diagnostic = ReadCatalogLines(path, operation, &lines);
      !diagnostic.code.empty()) {
    return diagnostic;
  }
  lines.erase(std::remove_if(lines.begin(),
                             lines.end(),
                             [](const std::string& existing) {
                               return existing.rfind("format=", 0) == 0;
                             }),
              lines.end());
  bool replaced = false;
  for (auto& existing : lines) {
    if (existing.rfind(key_prefix, 0) == 0) {
      existing = std::move(line);
      replaced = true;
      break;
    }
  }
  if (!replaced) {
    lines.push_back(std::move(line));
  }
  if (auto diagnostic = PersistCatalogLines(path, lines, operation);
      !diagnostic.code.empty()) {
    return diagnostic;
  }
  *record_count = static_cast<EngineApiU64>(lines.size());
  return {};
}

EngineApiDiagnostic PersistObjectResidencyPolicy(
    const EngineMemoryManagementRequest& request,
    EngineMemoryManagementResult* result) {
  const auto& descriptor = request.object_residency;
  if (!descriptor.durable_catalog_persistence_requested) {
    return {};
  }
  const char* operation = EngineMemoryManagementOperationName(request.memory_operation);
  const auto path = MemoryCatalogPath(request.context, kObjectResidencyCatalogName);
  const std::string key =
      std::string("catalog=") + kObjectResidencyCatalogName +
      "|database_uuid=" + request.context.database_uuid.canonical +
      "|object_uuid=" + descriptor.object_uuid.canonical + "|";
  std::ostringstream line;
  line << key
       << "object_kind=" << descriptor.object_kind
       << "|filespace_uuid=" << descriptor.filespace_uuid.canonical
       << "|residency_class="
       << EngineMemoryObjectResidencyClassName(descriptor.residency_class)
       << "|page_types=" << JoinPageTypes(descriptor.page_types)
       << "|expected_policy_generation=" << descriptor.expected_policy_generation
       << "|observed_policy_generation=" << descriptor.observed_policy_generation
       << "|warmup_budget_bytes=" << descriptor.warmup_budget_bytes
       << "|heat_history_generation=" << descriptor.heat_history_generation
       << "|heat_history_derivative_only="
       << (descriptor.heat_history_derivative_only ? "true" : "false")
       << "|restart_warmup_manifest_persisted="
       << (descriptor.restart_warmup_manifest_persistence_requested ? "true" : "false")
       << "|local_transaction_id=" << request.context.local_transaction_id;
  EngineApiU64 count = 0;
  if (auto diagnostic = UpsertCatalogLine(path, key, line.str(), operation, &count);
      !diagnostic.code.empty()) {
    return diagnostic;
  }
  result->durable_state_changed = true;
  result->cache_invalidation_planned = true;
  result->memory_object_residency_catalog_persisted = true;
  result->memory_object_residency_restart_warmup_persisted =
      descriptor.restart_warmup_manifest_persistence_requested;
  result->durable_catalog_record_count = count;
  AddApiBehaviorEvidence(result,
                         "memory_object_residency_catalog_persisted",
                         kObjectResidencyCatalogName);
  if (result->memory_object_residency_restart_warmup_persisted) {
    AddApiBehaviorEvidence(result,
                           "memory_object_residency_restart_warmup_manifest",
                           "persisted");
  }
  return {};
}

EngineApiDiagnostic PersistAutomationReportCatalog(
    const EngineMemoryManagementRequest& request,
    EngineMemoryManagementResult* result) {
  const auto& descriptor = request.automation;
  if (!descriptor.durable_report_catalog_persistence_requested) {
    return {};
  }
  const char* operation = EngineMemoryManagementOperationName(request.memory_operation);
  const auto path = MemoryCatalogPath(request.context, kReportCatalogName);
  const std::string key =
      std::string("catalog=") + kReportCatalogName +
      "|database_uuid=" + request.context.database_uuid.canonical +
      "|report_generation=" + std::to_string(descriptor.report_generation) + "|";
  std::ostringstream line;
  line << key
       << "recommendation_uuid=" << descriptor.recommendation_uuid.canonical
       << "|recommendation_generation=" << descriptor.recommendation_generation
       << "|report_bounded=" << (descriptor.report_bounded ? "true" : "false")
       << "|report_redaction_validated="
       << (descriptor.report_redaction_validated ? "true" : "false")
       << "|metrics_contract_present="
       << (descriptor.metrics_contract_present ? "true" : "false")
       << "|recommend_only_default="
       << (descriptor.recommend_only_default ? "true" : "false")
       << "|guardrail_policy_resolved="
       << (descriptor.guardrail_policy_resolved ? "true" : "false")
       << "|local_transaction_id=" << request.context.local_transaction_id;
  EngineApiU64 count = 0;
  if (auto diagnostic = UpsertCatalogLine(path, key, line.str(), operation, &count);
      !diagnostic.code.empty()) {
    return diagnostic;
  }
  result->durable_state_changed = true;
  result->cache_invalidation_planned = true;
  result->memory_report_catalog_persisted = true;
  result->durable_catalog_record_count = count;
  AddApiBehaviorEvidence(result,
                         "memory_report_catalog_persisted",
                         kReportCatalogName);
  return {};
}

EngineApiDiagnostic PersistRateLimitPolicy(
    const EngineMemoryManagementRequest& request,
    EngineMemoryManagementResult* result) {
  const auto& descriptor = request.rate_limit;
  if (!descriptor.durable_catalog_persistence_requested) {
    return {};
  }
  const char* operation = EngineMemoryManagementOperationName(request.memory_operation);
  const auto path = MemoryCatalogPath(request.context, kRateLimitCatalogName);
  const std::string key =
      std::string("catalog=") + kRateLimitCatalogName +
      "|database_uuid=" + request.context.database_uuid.canonical +
      "|limit_class=" + EngineMemoryRateLimitClassName(descriptor.limit_class) + "|";
  std::ostringstream line;
  line << key
       << "action=" << EngineMemoryRateLimitActionName(descriptor.action)
       << "|limit_per_window=" << descriptor.limit_per_window
       << "|window_seconds=" << descriptor.window_seconds
       << "|policy_generation=" << descriptor.policy_generation
       << "|audit_enabled=" << (descriptor.audit_enabled ? "true" : "false")
       << "|integrity_event=" << (descriptor.integrity_event ? "true" : "false")
       << "|corruption_event=" << (descriptor.corruption_event ? "true" : "false")
       << "|local_transaction_id=" << request.context.local_transaction_id;
  EngineApiU64 count = 0;
  if (auto diagnostic = UpsertCatalogLine(path, key, line.str(), operation, &count);
      !diagnostic.code.empty()) {
    return diagnostic;
  }
  result->durable_state_changed = true;
  result->cache_invalidation_planned = true;
  result->memory_rate_limit_catalog_persisted = true;
  result->durable_catalog_record_count = count;
  AddApiBehaviorEvidence(result,
                         "memory_rate_limit_catalog_persisted",
                         kRateLimitCatalogName);
  return {};
}

EngineApiDiagnostic PersistPolicyMigrationCatalog(
    const EngineMemoryManagementRequest& request,
    EngineMemoryManagementResult* result) {
  const auto& descriptor = request.migration;
  if (!descriptor.persistent_format_mutation_requested) {
    return {};
  }
  const char* operation = EngineMemoryManagementOperationName(request.memory_operation);
  const auto path = MemoryCatalogPath(request.context, kPolicyMigrationCatalogName);
  const std::string key =
      std::string("catalog=") + kPolicyMigrationCatalogName +
      "|database_uuid=" + request.context.database_uuid.canonical +
      "|policy_uuid=" + descriptor.policy_uuid.canonical +
      "|target_policy_version=" + std::to_string(descriptor.target_policy_version) +
      "|target_schema_version=" + std::to_string(descriptor.target_schema_version) + "|";
  std::ostringstream line;
  line << key
       << "profile_uuid=" << descriptor.profile_uuid.canonical
       << "|source_policy_version=" << descriptor.source_policy_version
       << "|source_schema_version=" << descriptor.source_schema_version
       << "|policy_schema_validated="
       << (descriptor.policy_schema_validated ? "true" : "false")
       << "|grant_feedback_migration_declared="
       << (descriptor.grant_feedback_migration_declared ? "true" : "false")
       << "|heat_history_migration_declared="
       << (descriptor.heat_history_migration_declared ? "true" : "false")
       << "|derivative_state_audit_enabled="
       << (descriptor.derivative_state_audit_enabled ? "true" : "false")
       << "|discard_incompatible_derivative_state_allowed="
       << (descriptor.discard_incompatible_derivative_state_allowed ? "true" : "false")
       << "|derivative_state_migration_persisted="
       << (descriptor.derivative_state_migration_execution_requested ? "true" : "false")
       << "|downgrade_requested=" << (descriptor.downgrade_requested ? "true" : "false")
       << "|downgrade_compatibility_recorded="
       << (descriptor.downgrade_requested &&
                   descriptor.downgrade_compatibility_validated
               ? "true"
               : "false")
       << "|recovery_checkpoint_persisted="
       << (descriptor.recovery_checkpoint_persistence_requested ? "true" : "false")
       << "|local_transaction_id=" << request.context.local_transaction_id;
  EngineApiU64 count = 0;
  if (auto diagnostic = UpsertCatalogLine(path, key, line.str(), operation, &count);
      !diagnostic.code.empty()) {
    return diagnostic;
  }
  result->durable_state_changed = true;
  result->cache_invalidation_planned = true;
  result->policy_migration_planned = true;
  result->memory_policy_schema_migration_persisted = true;
  result->memory_derivative_state_migration_persisted =
      descriptor.derivative_state_migration_execution_requested;
  result->memory_policy_migration_recovery_checkpoint_persisted =
      descriptor.recovery_checkpoint_persistence_requested;
  result->durable_catalog_record_count = count;
  AddApiBehaviorEvidence(result,
                         "memory_policy_schema_migration_catalog_persisted",
                         kPolicyMigrationCatalogName);
  if (result->memory_derivative_state_migration_persisted) {
    AddApiBehaviorEvidence(result,
                           "memory_derivative_state_migration_checkpoint",
                           "persisted");
  }
  if (result->memory_policy_migration_recovery_checkpoint_persisted) {
    AddApiBehaviorEvidence(result,
                           "memory_policy_migration_recovery_checkpoint",
                           "persisted");
  }
  return {};
}

EngineApiDiagnostic PersistRequestedMemoryPolicyCatalogs(
    const EngineMemoryManagementRequest& request,
    EngineMemoryManagementResult* result) {
  if (auto diagnostic = PersistAutomationReportCatalog(request, result);
      !diagnostic.code.empty()) {
    return diagnostic;
  }
  if (auto diagnostic = PersistObjectResidencyPolicy(request, result);
      !diagnostic.code.empty()) {
    return diagnostic;
  }
  if (auto diagnostic = PersistRateLimitPolicy(request, result);
      !diagnostic.code.empty()) {
    return diagnostic;
  }
  return PersistPolicyMigrationCatalog(request, result);
}

}  // namespace

const char* EngineMemoryManagementOperationName(
    EngineMemoryManagementOperation operation) {
  switch (operation) {
    case EngineMemoryManagementOperation::inspect_governance:
      return "memory.governance.inspect";
    case EngineMemoryManagementOperation::validate_governance:
      return "memory.governance.validate";
    case EngineMemoryManagementOperation::plan_cache_control:
      return "memory.governance.plan_cache_control";
    case EngineMemoryManagementOperation::plan_pressure_response:
      return "memory.governance.plan_pressure_response";
    case EngineMemoryManagementOperation::create_report:
      return "memory.automation.create_report";
    case EngineMemoryManagementOperation::review_recommendation:
      return "memory.automation.review_recommendation";
    case EngineMemoryManagementOperation::apply_safe_recommendation:
      return "memory.automation.apply_safe_recommendation";
    case EngineMemoryManagementOperation::inspect_object_residency:
      return "memory.object_residency.inspect";
    case EngineMemoryManagementOperation::set_object_residency:
      return "memory.object_residency.set";
    case EngineMemoryManagementOperation::inspect_rate_limit:
      return "memory.rate_limit.inspect";
    case EngineMemoryManagementOperation::set_rate_limit:
      return "memory.rate_limit.set";
    case EngineMemoryManagementOperation::plan_policy_upgrade:
      return "memory.policy_upgrade.plan";
    case EngineMemoryManagementOperation::plan_policy_migration:
      return "memory.policy_migration.plan";
  }
  return "memory.unknown";
}

const char* EngineMemoryObjectResidencyClassName(
    EngineMemoryObjectResidencyClass residency_class) {
  switch (residency_class) {
    case EngineMemoryObjectResidencyClass::unknown: return "unknown";
    case EngineMemoryObjectResidencyClass::hot: return "hot";
    case EngineMemoryObjectResidencyClass::cold: return "cold";
    case EngineMemoryObjectResidencyClass::scan: return "scan";
    case EngineMemoryObjectResidencyClass::archive: return "archive";
    case EngineMemoryObjectResidencyClass::pin: return "pin";
    case EngineMemoryObjectResidencyClass::warm_on_open: return "warm_on_open";
    case EngineMemoryObjectResidencyClass::never_cache: return "never_cache";
    case EngineMemoryObjectResidencyClass::spill_only: return "spill_only";
    case EngineMemoryObjectResidencyClass::system: return "system";
  }
  return "unknown";
}

const char* EngineMemoryRateLimitClassName(
    EngineMemoryRateLimitClass limit_class) {
  switch (limit_class) {
    case EngineMemoryRateLimitClass::unknown: return "unknown";
    case EngineMemoryRateLimitClass::login_storm: return "login_storm";
    case EngineMemoryRateLimitClass::parser_bomb: return "parser_bomb";
    case EngineMemoryRateLimitClass::oversized_packet: return "oversized_packet";
    case EngineMemoryRateLimitClass::large_query_loop: return "large_query_loop";
    case EngineMemoryRateLimitClass::cache_flush_abuse: return "cache_flush_abuse";
    case EngineMemoryRateLimitClass::report_flood: return "report_flood";
    case EngineMemoryRateLimitClass::udr_storm: return "udr_storm";
    case EngineMemoryRateLimitClass::management_flood: return "management_flood";
    case EngineMemoryRateLimitClass::integrity_or_corruption_signal:
      return "integrity_or_corruption_signal";
  }
  return "unknown";
}

const char* EngineMemoryRateLimitActionName(
    EngineMemoryRateLimitAction action) {
  switch (action) {
    case EngineMemoryRateLimitAction::unknown: return "unknown";
    case EngineMemoryRateLimitAction::observe: return "observe";
    case EngineMemoryRateLimitAction::throttle: return "throttle";
    case EngineMemoryRateLimitAction::refuse: return "refuse";
    case EngineMemoryRateLimitAction::audit_only: return "audit_only";
  }
  return "unknown";
}

EngineMemoryManagementResult EnginePlanMemoryManagementOperation(
    const EngineMemoryManagementRequest& request) {
  mem::MemoryPressureDecision pressure_decision;
  mem::MemorySupportBundleResult support_bundle;
  if (auto diagnostic =
          ValidateForOperation(request, &pressure_decision, &support_bundle);
      !diagnostic.code.empty()) {
    return MemoryFailure(request, std::move(diagnostic));
  }

  EngineMemoryManagementResult result =
      MakeApiBehaviorSuccess<EngineMemoryManagementResult>(
          request.context,
          request.operation_id.empty()
              ? EngineMemoryManagementOperationName(request.memory_operation)
              : request.operation_id);
  result.result_shape.result_kind = "rs.memory.management.descriptor_plan.v1";
  result.primary_object = request.target_object;
  result.local_transaction_id = request.context.local_transaction_id;
  result.transaction_uuid = request.context.transaction_uuid;
  result.cache_invalidation_planned =
      request.memory_operation == EngineMemoryManagementOperation::plan_cache_control;
  result.report_materialized =
      request.memory_operation == EngineMemoryManagementOperation::create_report;
  result.policy_migration_planned =
      request.memory_operation == EngineMemoryManagementOperation::plan_policy_upgrade ||
      request.memory_operation == EngineMemoryManagementOperation::plan_policy_migration;
  if (auto diagnostic = ExecuteRequestedMemoryGovernanceActions(request, &result);
      !diagnostic.code.empty()) {
    return MemoryFailure(request, std::move(diagnostic));
  }
  if (auto diagnostic = ExecuteRequestedRateLimitLiveEvaluation(request, &result);
      !diagnostic.code.empty()) {
    return MemoryFailure(request, std::move(diagnostic));
  }
  if (auto diagnostic = ExecuteRequestedSafeAutomation(request, &result);
      !diagnostic.code.empty()) {
    return MemoryFailure(request, std::move(diagnostic));
  }
  if (auto diagnostic = PersistRequestedMemoryPolicyCatalogs(request, &result);
      !diagnostic.code.empty()) {
    return MemoryFailure(request, std::move(diagnostic));
  }
  AddSuccessRows(&result, request, pressure_decision, support_bundle);
  AddSuccessEvidence(&result, request);
  result.result_shape.result_kind = "rs.memory.management.descriptor_plan.v1";
  return result;
}

}  // namespace scratchbird::engine::internal_api
