// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "agent_runtime.hpp"
#include "page_filespace_handoff.hpp"

#include <string>

namespace scratchbird::core::agents::implemented_agents {

using scratchbird::core::platform::DiagnosticRecord;
using scratchbird::core::platform::Status;
using scratchbird::core::platform::TypedUuid;
using scratchbird::core::platform::u32;
using scratchbird::core::platform::u64;

enum class FilespaceCapacityManagerDecisionKind : u32 {
  no_action,
  capacity_window_approved,
  capacity_window_refused,
  action_recommended,
  action_suppressed,
  action_dry_run,
  action_approval_required,
  action_authorized,
  action_refused,
  refused
};

enum class FilespaceCapacityManagerActionKind : u32 {
  request_filespace_expand,
  request_filespace_move,
  request_filespace_shrink,
  request_filespace_truncate,
  request_filespace_quarantine,
  recommend_primary_shadow_promotion,
  forbidden_allocate_page,
  forbidden_relocate_page,
  forbidden_compact_page_family,
  forbidden_rebuild_index,
  forbidden_advance_mga_cleanup
};

enum class FilespaceCapacityHealthState : u32 {
  unknown,
  healthy,
  degraded,
  critical,
  failed
};

enum class FilespaceCapacityRoleState : u32 {
  unknown,
  active_primary,
  primary_shadow,
  primary_candidate,
  secondary_data,
  temporary,
  drop_pending,
  forbidden
};

struct FilespaceCapacityManagerMetricSnapshot {
  TypedUuid database_uuid;
  TypedUuid filespace_uuid;
  TypedUuid policy_uuid;
  u64 total_pages = 0;
  u64 used_pages = 0;
  u64 free_pages = 0;
  u64 reserved_pages = 0;
  u64 available_capacity_window_pages = 0;
  bool expand_capacity_proof_present = true;
  bool expand_capacity_proof_fresh = true;
  bool expand_device_proof_present = true;
  bool expand_device_proof_fresh = true;
  bool metrics_present = true;
  bool metrics_fresh = true;
  bool metrics_trusted = true;
  bool scope_compatible = true;
  FilespaceCapacityHealthState health_state = FilespaceCapacityHealthState::healthy;
  FilespaceCapacityRoleState role_state = FilespaceCapacityRoleState::active_primary;
};

struct FilespaceCapacityManagerPolicy {
  TypedUuid database_uuid;
  TypedUuid filespace_uuid;
  TypedUuid policy_uuid;
  bool valid = true;
  bool scope_compatible = true;
  u64 minimum_free_pages = 4;
  u64 target_free_pages = 8;
  u64 max_capacity_window_pages = 8;
  bool capacity_window_allowed = false;
  bool capacity_processing_policy_explicit = false;
  bool expand_allowed = false;
  bool expand_request_policy_explicit = false;
  bool move_allowed = false;
  bool move_request_policy_explicit = false;
  bool shrink_allowed = false;
  bool shrink_request_policy_explicit = false;
  bool truncate_allowed = false;
  bool truncate_request_policy_explicit = false;
  bool quarantine_allowed = false;
  bool quarantine_request_policy_explicit = false;
  bool critical_quarantine_automatic_allowed = false;
  bool shadow_promotion_allowed = false;
  bool shadow_promotion_policy_explicit = false;
  bool allow_degraded_capacity_window = false;
};

struct FilespaceCapacityManagerSafetyState {
  bool startup_complete = true;
  bool recovery_complete = true;
  bool maintenance_mode = false;
  bool maintenance_allows_capacity_windows = true;
  bool engine_authoritative = true;
};

struct FilespaceCapacityManagerEvidence {
  TypedUuid request_uuid;
  TypedUuid evidence_uuid;
  TypedUuid database_uuid;
  TypedUuid filespace_uuid;
  TypedUuid policy_uuid;
  scratchbird::storage::page::PageFilespaceAgentRequestKind request_kind =
      scratchbird::storage::page::PageFilespaceAgentRequestKind::extend_filespace;
  FilespaceCapacityManagerDecisionKind decision = FilespaceCapacityManagerDecisionKind::no_action;
  u64 requested_pages = 0;
  u64 granted_pages = 0;
  u64 capacity_window_pages = 0;
  u64 free_pages = 0;
  u64 reserved_pages = 0;
  u64 target_free_pages = 0;
  std::string diagnostic_code;
  std::string evidence_state;
  std::string reason;
  bool durable_state_changed = false;
  bool physical_filespace_mutation_attempted = false;
  bool page_ledger_mutation_attempted = false;
};

struct FilespaceCapacityManagerActionRequest {
  FilespaceCapacityManagerActionKind action =
      FilespaceCapacityManagerActionKind::request_filespace_expand;
  TypedUuid request_uuid;
  TypedUuid evidence_uuid;
  TypedUuid database_uuid;
  TypedUuid filespace_uuid;
  TypedUuid source_filespace_uuid;
  TypedUuid target_filespace_uuid;
  TypedUuid policy_uuid;
  TypedUuid capacity_proof_uuid;
  TypedUuid device_proof_uuid;
  TypedUuid object_list_proof_uuid;
  TypedUuid page_relocation_request_uuid;
  TypedUuid shrink_ready_evidence_uuid;
  TypedUuid device_health_evidence_uuid;
  TypedUuid checksum_evidence_uuid;
  TypedUuid unknown_page_evidence_uuid;
  TypedUuid primary_degradation_proof_uuid;
  TypedUuid candidate_readiness_proof_uuid;
  u64 target_bytes = 0;
  u64 safe_tail_bytes = 0;
  u64 blocker_count = 0;
  bool live_action_requested = true;
  bool dry_run = false;
  bool explicit_evidence = true;
  bool capacity_proof_fresh = true;
  bool device_proof_fresh = true;
  bool object_list_proof_fresh = true;
  bool page_agent_proof_fresh = true;
  bool shrink_ready_evidence_fresh = true;
  bool device_health_evidence_fresh = true;
  bool checksum_evidence_fresh = true;
  bool unknown_page_evidence_fresh = true;
  bool primary_degradation_proof_fresh = true;
  bool candidate_readiness_proof_fresh = true;
  bool startup_safety_state_present = true;
  bool operator_review_requested = false;
  bool catalog_persistence_migration_requirement_present = false;
  bool has_obs_agent_action_approve = false;
  bool has_filespace_lifecycle_control = false;
  bool has_lifecycle_truncate_control = false;
  bool has_obs_agent_recommendation_read = false;
};

struct FilespaceCapacityManagerActionResult {
  Status status;
  FilespaceCapacityManagerDecisionKind decision =
      FilespaceCapacityManagerDecisionKind::action_refused;
  DiagnosticRecord diagnostic;
  FilespaceCapacityManagerEvidence evidence;
  FilespaceCapacityManagerActionKind action =
      FilespaceCapacityManagerActionKind::request_filespace_expand;
  bool fail_closed = false;
  bool refused = false;
  bool recommended = false;
  bool suppressed = false;
  bool dry_run = false;
  bool approval_required = false;
  bool authorized = false;
  bool physical_filespace_mutation_attempted = false;
  bool page_ledger_mutation_attempted = false;

  bool ok() const { return status.ok() && !fail_closed; }
};

struct FilespaceCapacityManagerTickResult {
  Status status;
  FilespaceCapacityManagerDecisionKind decision = FilespaceCapacityManagerDecisionKind::refused;
  DiagnosticRecord diagnostic;
  FilespaceCapacityManagerEvidence evidence;
  scratchbird::storage::page::PageFilespaceAgentQueueRecord queue_record;
  u64 requested_pages = 0;
  u64 granted_pages = 0;
  u64 capacity_window_pages = 0;
  u64 processed_records = 0;
  u64 skipped_records = 0;
  bool fail_closed = false;
  bool approved = false;
  bool refused = false;
  bool queue_mutated = false;
  bool physical_filespace_mutation_attempted = false;
  bool page_ledger_mutation_attempted = false;

  bool ok() const { return status.ok() && !fail_closed; }
};

const char* FilespaceCapacityManagerDecisionKindName(FilespaceCapacityManagerDecisionKind kind);
const char* FilespaceCapacityManagerActionKindName(FilespaceCapacityManagerActionKind action);
const char* FilespaceCapacityHealthStateName(FilespaceCapacityHealthState state);
const char* FilespaceCapacityRoleStateName(FilespaceCapacityRoleState state);

FilespaceCapacityManagerPolicy DefaultFilespaceCapacityManagerPolicy();
u64 FilespaceCapacityManagerEffectiveWindowPages(
    const FilespaceCapacityManagerMetricSnapshot& snapshot,
    const FilespaceCapacityManagerPolicy& policy);
FilespaceCapacityManagerTickResult EvaluateFilespaceCapacityManagerTick(
    scratchbird::storage::page::PageFilespaceAgentRequestQueue* queue,
    const FilespaceCapacityManagerMetricSnapshot& snapshot,
    const FilespaceCapacityManagerPolicy& policy);
FilespaceCapacityManagerTickResult EvaluateFilespaceCapacityManagerTick(
    scratchbird::storage::page::PageFilespaceAgentRequestQueue* queue,
    const FilespaceCapacityManagerMetricSnapshot& snapshot,
    const FilespaceCapacityManagerPolicy& policy,
    const FilespaceCapacityManagerSafetyState& safety);
FilespaceCapacityManagerActionResult EvaluateFilespaceCapacityManagerAction(
    const FilespaceCapacityManagerActionRequest& request,
    const FilespaceCapacityManagerMetricSnapshot& snapshot,
    const FilespaceCapacityManagerPolicy& policy,
    const FilespaceCapacityManagerSafetyState& safety);
DiagnosticRecord MakeFilespaceCapacityManagerDiagnostic(Status status,
                                                        std::string diagnostic_code,
                                                        std::string message_key,
                                                        std::string detail = {});

const char* filespace_capacity_manager_implementation_anchor();
StorageSpaceAgentDefaults filespace_capacity_manager_default_space_policy();
bool filespace_capacity_manager_should_request_space(u64 available_pages);
bool filespace_capacity_manager_target_satisfied(u64 available_pages);
u64 filespace_capacity_manager_page_allocation_notify_threshold_pages();

}  // namespace scratchbird::core::agents::implemented_agents
