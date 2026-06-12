// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "api_types.hpp"
#include "memory_policy_config.hpp"
#include "memory_pressure_response.hpp"
#include "memory_support_bundle.hpp"
#include "page_header.hpp"

#include <vector>

namespace scratchbird::engine::internal_api {

// SEARCH_KEY: SB_ENGINE_INTERNAL_API_MEMORY_MANAGEMENT_API
enum class EngineMemoryManagementOperation {
  inspect_governance,
  validate_governance,
  plan_cache_control,
  plan_pressure_response,
  create_report,
  review_recommendation,
  apply_safe_recommendation,
  inspect_object_residency,
  set_object_residency,
  inspect_rate_limit,
  set_rate_limit,
  plan_policy_upgrade,
  plan_policy_migration
};

enum class EngineMemoryObjectResidencyClass {
  unknown,
  hot,
  cold,
  scan,
  archive,
  pin,
  warm_on_open,
  never_cache,
  spill_only,
  system
};

enum class EngineMemoryRateLimitClass {
  unknown,
  login_storm,
  parser_bomb,
  oversized_packet,
  large_query_loop,
  cache_flush_abuse,
  report_flood,
  udr_storm,
  management_flood,
  integrity_or_corruption_signal
};

enum class EngineMemoryRateLimitAction {
  unknown,
  observe,
  throttle,
  refuse,
  audit_only
};

struct EngineMemoryGovernanceDescriptor {
  EngineUuid profile_uuid;
  scratchbird::core::memory::MemoryPolicyConfig policy_config;
  scratchbird::core::memory::MemoryAccountingSnapshot current_snapshot;
  scratchbird::core::memory::MemoryPressurePolicy pressure_policy;
  scratchbird::core::memory::MemoryPressureObservation pressure_observation;
  scratchbird::core::memory::MemorySupportBundleRequest support_bundle_request;
  EngineApiU64 expected_policy_generation = 0;
  EngineApiU64 observed_policy_generation = 0;
  bool profile_resolved = false;
  bool memory_tree_snapshot_present = false;
  bool cache_governor_registered = false;
  bool cache_flush_or_invalidation_requested = false;
  bool pressure_observation_present = false;
  bool grant_feedback_surface_present = false;
  bool parser_front_door_limit_surface_present = false;
  bool udr_limit_surface_present = false;
  bool streaming_window_surface_present = false;
  bool maintenance_budget_surface_present = false;
  bool dump_swap_policy_present = false;
  bool allocator_scavenging_surface_present = false;
  bool platform_capability_matrix_present = false;
  bool protected_material_redaction_validated = false;
  bool activation_timing_declared = false;
  bool physical_allocator_action_requested = false;
  bool cache_control_execution_requested = false;
  bool cache_control_execution_authorized = false;
  bool allocator_scavenging_execution_requested = false;
  bool allocator_scavenging_execution_authorized = false;
};

struct EngineMemoryAutomationDescriptor {
  EngineUuid recommendation_uuid;
  EngineApiU64 report_generation = 0;
  EngineApiU64 recommendation_generation = 0;
  bool report_bounded = false;
  bool report_redaction_validated = false;
  bool metrics_contract_present = false;
  bool recommendation_explainable = false;
  bool recommend_only_default = true;
  bool safe_apply_requested = false;
  bool maintenance_window_bound = false;
  bool audit_enabled = false;
  bool unsafe_action_requested = false;
  bool guardrail_policy_resolved = false;
  bool durable_report_catalog_persistence_requested = false;
  bool durable_report_catalog_persistence_authorized = false;
  bool safe_automation_execution_requested = false;
  bool safe_automation_execution_authorized = false;
};

struct EngineMemoryObjectResidencyDescriptor {
  EngineUuid object_uuid;
  EngineUuid filespace_uuid;
  std::string object_kind;
  EngineMemoryObjectResidencyClass residency_class =
      EngineMemoryObjectResidencyClass::unknown;
  std::vector<scratchbird::storage::disk::PageType> page_types;
  EngineApiU64 expected_policy_generation = 0;
  EngineApiU64 observed_policy_generation = 0;
  EngineApiU64 warmup_budget_bytes = 0;
  EngineApiU64 heat_history_generation = 0;
  bool profile_resolved = false;
  bool object_resolved = false;
  bool filespace_placement_validated = false;
  bool security_scope_validated = false;
  bool cluster_placement_validated = false;
  bool heat_history_derivative_only = true;
  bool physical_prefetch_requested = false;
  bool durable_catalog_persistence_requested = false;
  bool durable_catalog_persistence_authorized = false;
  bool restart_warmup_manifest_persistence_requested = false;
  bool restart_warmup_manifest_persistence_authorized = false;
};

struct EngineMemoryRateLimitDescriptor {
  EngineMemoryRateLimitClass limit_class = EngineMemoryRateLimitClass::unknown;
  EngineMemoryRateLimitAction action = EngineMemoryRateLimitAction::unknown;
  EngineApiU64 limit_per_window = 0;
  EngineApiU64 window_seconds = 0;
  EngineApiU64 observed_count_in_window = 0;
  EngineApiU64 policy_generation = 0;
  bool policy_resolved = false;
  bool audit_enabled = false;
  bool integrity_event = false;
  bool corruption_event = false;
  bool durable_catalog_persistence_requested = false;
  bool durable_catalog_persistence_authorized = false;
  bool live_executor_evaluation_requested = false;
  bool live_executor_evaluation_authorized = false;
};

struct EngineMemoryPolicyMigrationDescriptor {
  EngineUuid profile_uuid;
  EngineUuid policy_uuid;
  EngineApiU64 source_policy_version = 0;
  EngineApiU64 target_policy_version = 0;
  EngineApiU64 source_schema_version = 0;
  EngineApiU64 target_schema_version = 0;
  bool policy_schema_validated = false;
  bool grant_feedback_migration_declared = false;
  bool heat_history_migration_declared = false;
  bool derivative_state_audit_enabled = false;
  bool discard_incompatible_derivative_state_allowed = false;
  bool downgrade_requested = false;
  bool downgrade_compatibility_validated = false;
  bool persistent_format_mutation_requested = false;
  bool durable_policy_schema_migration_authorized = false;
  bool derivative_state_migration_execution_requested = false;
  bool derivative_state_migration_execution_authorized = false;
  bool recovery_checkpoint_persistence_requested = false;
  bool recovery_checkpoint_persistence_authorized = false;
};

struct EngineMemoryManagementRequest : EngineApiRequest {
  EngineMemoryManagementOperation memory_operation =
      EngineMemoryManagementOperation::inspect_governance;
  EngineMemoryGovernanceDescriptor governance;
  EngineMemoryAutomationDescriptor automation;
  EngineMemoryObjectResidencyDescriptor object_residency;
  EngineMemoryRateLimitDescriptor rate_limit;
  EngineMemoryPolicyMigrationDescriptor migration;
  bool cluster_scoped = false;
  bool parser_memory_authority = false;
  bool transaction_finality_authority = false;
  bool visibility_authority = false;
  bool recovery_authority = false;
  bool reference_or_wal_recovery_authority = false;
  bool private_provider_dispatch_requested = false;
};

struct EngineMemoryManagementResult : EngineApiResult {
  bool durable_state_changed = false;
  bool parser_memory_authority = false;
  bool transaction_finality_authority = false;
  bool visibility_authority = false;
  bool recovery_authority = false;
  bool reference_or_wal_recovery_authority = false;
  bool private_provider_dispatch = false;
  bool physical_action_dispatched = false;
  bool cache_invalidation_planned = false;
  bool cache_control_executed = false;
  bool cache_flush_executed = false;
  bool cache_invalidation_executed = false;
  bool allocator_scavenging_executed = false;
  EngineApiU64 allocator_scavenging_reclaimed_bytes = 0;
  bool report_materialized = false;
  bool policy_migration_planned = false;
  bool memory_object_residency_catalog_persisted = false;
  bool memory_object_residency_restart_warmup_persisted = false;
  bool memory_rate_limit_catalog_persisted = false;
  bool memory_rate_limit_live_executor_evaluated = false;
  bool memory_rate_limit_throttle_executed = false;
  bool memory_rate_limit_refuse_executed = false;
  bool memory_rate_limit_audit_emitted = false;
  bool memory_report_catalog_persisted = false;
  bool memory_safe_automation_executed = false;
  bool memory_safe_automation_audit_emitted = false;
  bool memory_policy_schema_migration_persisted = false;
  bool memory_derivative_state_migration_persisted = false;
  bool memory_policy_migration_recovery_checkpoint_persisted = false;
  EngineApiU64 durable_catalog_record_count = 0;
};

const char* EngineMemoryManagementOperationName(
    EngineMemoryManagementOperation operation);
const char* EngineMemoryObjectResidencyClassName(
    EngineMemoryObjectResidencyClass residency_class);
const char* EngineMemoryRateLimitClassName(
    EngineMemoryRateLimitClass limit_class);
const char* EngineMemoryRateLimitActionName(
    EngineMemoryRateLimitAction action);

EngineMemoryManagementResult EnginePlanMemoryManagementOperation(
    const EngineMemoryManagementRequest& request);

}  // namespace scratchbird::engine::internal_api
