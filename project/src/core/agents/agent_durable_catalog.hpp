// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SEARCH_KEY: ARHC_DURABLE_AGENT_CATALOG_STATE
// SEARCH_KEY: ARHC_IN_MEMORY_SIDECAR_PRODUCTION_REMOVAL
// SEARCH_KEY: ARHC_DURABLE_SCHEDULER_LEASES_HEARTBEATS

#include "agent_runtime.hpp"

#include <string>
#include <vector>

namespace scratchbird::core::agents {

enum class AgentCatalogStateSource {
  durable_catalog_image,
  in_memory_bootstrap,
  sidecar_legacy,
  pipe_delimited_legacy
};

enum class DurableAgentLeaseState {
  none,
  acquired,
  draining,
  cancelled,
  quarantined,
  replay_pending,
  expired
};

enum class DurableAgentActionState {
  pending,
  running,
  completed,
  cancelled,
  replay_pending,
  quarantined
};

enum class DurableAgentResourceReservationState {
  acquired,
  released,
  cancelled,
  timeout,
  shutdown
};

enum class DurableAgentReplayState {
  none,
  replay_pending,
  retry_scheduled,
  compensation_required,
  compensated,
  quarantined,
  quarantine_review_pending,
  quarantine_released,
  refused
};

struct DurableCatalogAuthorityEvidence {
  bool durable_catalog_authority = false;
  bool mga_transaction_evidence = false;
  std::string mga_transaction_uuid;
  u64 transaction_generation = 0;
  std::string evidence_uuid;
  std::string database_uuid;
  std::string catalog_storage_uuid;
  std::string catalog_root_digest;
  std::string previous_catalog_root_digest;
  std::string storage_commit_evidence_uuid;
  u64 catalog_generation = 0;
  u64 local_transaction_id = 0;
  bool storage_catalog_record_evidence = false;
  bool transaction_inventory_bound = false;
  bool fsync_or_checkpoint_evidence = false;
  bool sidecar_storage = false;
  bool in_memory_only = false;
};

struct DurableAgentApprovalRecord {
  std::string approval_uuid;
  std::string action_uuid;
  std::string principal_uuid;
  std::string evidence_uuid;
  u64 approved_at_microseconds = 0;
  bool approved = false;
};

struct DurableAgentOverrideRecord {
  std::string override_uuid;
  std::string agent_type_id;
  std::string scope;
  std::string principal_uuid;
  std::string evidence_uuid;
  u64 expires_at_microseconds = 0;
  bool active = false;
};

struct DurableAgentLeaseRecord {
  std::string lease_uuid;
  std::string instance_uuid;
  std::string owner_uuid;
  DurableAgentLeaseState state = DurableAgentLeaseState::none;
  u64 acquired_at_microseconds = 0;
  u64 expires_at_microseconds = 0;
  u64 heartbeat_generation = 0;
  u64 last_heartbeat_microseconds = 0;
  u64 replay_generation = 0;
  std::string evidence_uuid;
};

struct DurableAgentHealthRecord {
  std::string instance_uuid;
  std::string health_state;
  std::string diagnostic_code;
  std::string evidence_uuid;
  u64 observed_at_microseconds = 0;
};

struct DurableAgentHistoryRecord {
  std::string history_uuid;
  std::string subject_uuid;
  std::string event_kind;
  std::string diagnostic_code;
  std::string evidence_uuid;
  u64 recorded_at_microseconds = 0;
};

struct DurableAgentActionRecord {
  std::string action_uuid;
  std::string instance_uuid;
  std::string owner_uuid;
  std::string operation_id;
  std::string actuator_provider_id;
  DurableAgentActionState state = DurableAgentActionState::pending;
  std::string idempotency_key;
  std::string input_evidence_digest;
  std::string evidence_uuid;
  std::string verification_evidence_uuid;
  std::string diagnostic_code;
  u64 generation = 0;
  u64 retry_count = 0;
  bool outcome_verified = false;
  bool compensation_required = false;
  bool compensation_attempted = false;
  bool retry_scheduled = false;
  u64 retry_after_microseconds = 0;
  std::string retry_evidence_uuid;
  std::string compensation_executor_id;
  std::string compensation_evidence_uuid;
  bool parser_authority = false;
  bool client_authority = false;
  bool reference_authority = false;
  bool sidecar_authority = false;
};

struct DurableAgentResourceReservationRecord {
  std::string reservation_uuid;
  std::string reservation_key;
  std::string owner_scope;
  std::string agent_type_id;
  std::string operation_id;
  DurableAgentResourceReservationState state =
      DurableAgentResourceReservationState::acquired;
  u64 acquired_at_microseconds = 0;
  u64 released_at_microseconds = 0;
  u64 memory_bytes = 0;
  u64 worker_slots = 0;
  u64 overhead_microseconds = 0;
  std::string evidence_uuid;
  std::string release_evidence_uuid;
  std::string release_reason;
  bool parser_authority = false;
  bool client_authority = false;
  bool reference_authority = false;
  bool benchmark_authority = false;
};

struct DurableAgentReplayRecord {
  std::string replay_uuid;
  std::string action_uuid;
  std::string instance_uuid;
  std::string operation_id;
  std::string idempotency_key;
  DurableAgentReplayState state = DurableAgentReplayState::none;
  u64 replay_generation = 0;
  u64 retry_count = 0;
  u64 max_retry_count = 0;
  u64 retry_after_microseconds = 0;
  u64 recorded_at_microseconds = 0;
  std::string policy_digest;
  u64 policy_generation = 0;
  std::string metric_digest;
  std::string catalog_root_digest;
  std::string security_digest;
  u64 security_epoch = 0;
  std::string resource_reservation_digest;
  std::string binary_package_digest;
  std::string action_input_digest;
  std::string action_evidence_digest;
  std::string action_record_digest;
  std::string evidence_chain_digest;
  std::string evidence_uuid;
  std::string review_evidence_uuid;
  std::string compensation_evidence_uuid;
  std::string diagnostic_code;
  bool review_required = false;
  bool review_approved = false;
  bool compensation_required = false;
  bool compensation_attempted = false;
  bool retry_scheduled = false;
  bool cluster_route_requested = false;
  bool external_cluster_provider_attested = false;
  bool parser_authority = false;
  bool client_authority = false;
  bool reference_authority = false;
  bool wal_authority = false;
  bool benchmark_authority = false;
  bool optimizer_plan_authority = false;
  bool index_finality_authority = false;
  bool provider_finality_authority = false;
  bool memory_authority = false;
  bool agent_action_authority = false;
};

struct DurableAgentStateMigrationRecord {
  std::string migration_uuid;
  u64 from_schema_version = 0;
  u64 to_schema_version = 0;
  std::string result;
  std::string evidence_uuid;
  std::string source_root_digest;
  std::string target_root_digest;
  u64 recorded_at_microseconds = 0;
};

struct DurableAgentCatalogImage {
  AgentCatalogStateSource source = AgentCatalogStateSource::in_memory_bootstrap;
  u64 schema_version = 1;
  DurableCatalogAuthorityEvidence authority;
  std::vector<AgentInstanceRecord> instances;
  std::vector<AgentPolicy> policies;
  std::vector<AgentPolicyAttachmentRecord> attachments;
  std::vector<AgentEvidenceRecord> evidence;
  std::vector<DurableAgentActionRecord> actions;
  std::vector<DurableAgentApprovalRecord> approvals;
  std::vector<DurableAgentOverrideRecord> overrides;
  std::vector<DurableAgentLeaseRecord> leases;
  std::vector<DurableAgentResourceReservationRecord> resource_reservations;
  std::vector<DurableAgentReplayRecord> replay_records;
  std::vector<DurableAgentHealthRecord> health;
  std::vector<DurableAgentHistoryRecord> retained_history;
  std::vector<DurableAgentStateMigrationRecord> migrations;
};

struct DurableCatalogValidationResult {
  AgentRuntimeStatus status;
  DurableAgentCatalogImage image;
  std::string checksum;
  bool migrated = false;
};

struct DurableCatalogMigrationResult {
  AgentRuntimeStatus status;
  DurableAgentCatalogImage image;
  DurableAgentStateMigrationRecord migration;
  bool migrated = false;
};

struct DurableLeaseRequest {
  std::string lease_uuid;
  std::string instance_uuid;
  std::string owner_uuid;
  u64 now_microseconds = 0;
  u64 lease_duration_microseconds = 0;
  std::string evidence_uuid;
};

struct DurableAgentResourceReservationRequest {
  std::string reservation_uuid;
  std::string reservation_key;
  std::string owner_scope;
  std::string agent_type_id;
  std::string operation_id;
  u64 now_microseconds = 0;
  u64 memory_bytes = 0;
  u64 worker_slots = 0;
  u64 overhead_microseconds = 0;
  u64 max_active_reservations = 0;
  u64 max_memory_bytes = 0;
  u64 max_worker_slots = 0;
  u64 max_overhead_microseconds = 0;
  std::string evidence_uuid;
};

struct DurableAgentApprovalRequest {
  std::string approval_uuid;
  std::string action_uuid;
  std::string principal_uuid;
  std::string evidence_uuid;
  u64 approved_at_microseconds = 0;
  bool approved = false;
};

struct DurableAgentActionCancellationRequest {
  std::string action_uuid;
  std::string principal_uuid;
  std::string evidence_uuid;
  std::string reason;
  u64 cancelled_at_microseconds = 0;
  bool operator_approved = false;
};

struct DurableAgentPolicyUpdateRequest {
  AgentPolicy policy;
  std::string agent_type_id;
  std::string principal_uuid;
  std::string evidence_uuid;
  u64 expected_previous_generation = 0;
  u64 updated_at_microseconds = 0;
  bool operator_approved = false;
};

std::string DurableAgentLeaseStateName(DurableAgentLeaseState state);
std::string DurableAgentActionStateName(DurableAgentActionState state);
std::string DurableAgentResourceReservationStateName(
    DurableAgentResourceReservationState state);
std::string DurableAgentReplayStateName(DurableAgentReplayState state);
std::string AgentCatalogStateSourceName(AgentCatalogStateSource source);

std::string SerializeDurableAgentCatalogImage(const DurableAgentCatalogImage& image);
std::string DurableAgentCatalogRootDigest(DurableAgentCatalogImage image);
AgentRuntimeStatus RefreshDurableAgentCatalogAuthorityDigest(
    DurableAgentCatalogImage* image,
    const std::string& evidence_uuid);
DurableCatalogMigrationResult MigrateDurableAgentCatalogImageForProduction(
    DurableAgentCatalogImage image,
    const std::string& evidence_uuid,
    u64 recorded_at_microseconds);
DurableCatalogValidationResult ValidateDurableAgentCatalogImage(
    const std::string& encoded,
    bool production_live_path);
AgentRuntimeStatus ValidateDurableAgentCatalogForProduction(
    const DurableAgentCatalogImage& image);

AgentRuntimeStatus AcquireDurableAgentLease(DurableAgentCatalogImage* image,
                                            const DurableLeaseRequest& request);
AgentRuntimeStatus HeartbeatDurableAgentLease(DurableAgentCatalogImage* image,
                                              const DurableLeaseRequest& request);
AgentRuntimeStatus CancelDurableAgentLease(DurableAgentCatalogImage* image,
                                           const DurableLeaseRequest& request,
                                           DurableAgentLeaseState terminal_state);
AgentRuntimeStatus AcquireDurableAgentResourceReservation(
    DurableAgentCatalogImage* image,
    const DurableAgentResourceReservationRequest& request);
AgentRuntimeStatus ReleaseDurableAgentResourceReservation(
    DurableAgentCatalogImage* image,
    const std::string& reservation_uuid,
    const std::string& release_evidence_uuid,
    u64 now_microseconds,
    DurableAgentResourceReservationState terminal_state =
        DurableAgentResourceReservationState::released);
AgentRuntimeStatus RecordDurableAgentApproval(
    DurableAgentCatalogImage* image,
    const DurableAgentApprovalRequest& request);
AgentRuntimeStatus CancelDurableAgentAction(
    DurableAgentCatalogImage* image,
    const DurableAgentActionCancellationRequest& request);
AgentRuntimeStatus ApplyDurableAgentPolicyUpdate(
    DurableAgentCatalogImage* image,
    const DurableAgentPolicyUpdateRequest& request);
AgentRuntimeStatus RecoverDurableAgentCatalogAfterCrash(DurableAgentCatalogImage* image,
                                                        u64 now_microseconds,
                                                        const std::string& evidence_uuid);

}  // namespace scratchbird::core::agents
