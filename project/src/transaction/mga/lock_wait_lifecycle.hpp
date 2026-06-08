// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// DBLC-013W: MGA-authoritative lock/latch wait lifecycle primitives.
// This layer gates resource admission and waits only; transaction finality
// remains owned by the MGA transaction inventory.

#include "transaction_state.hpp"

#include <map>
#include <string>
#include <vector>

namespace scratchbird::transaction::mga {

enum class MGAWaitResourceKind : u16 {
  logical_lock,
  physical_latch,
  unknown
};

enum class MGALockOwnerKind : u16 {
  transaction,
  statement,
  cursor,
  operation,
  recovery,
  cleanup,
  archive,
  backup_restore,
  cluster_participant,
  engine_coordinator,
  unknown
};

enum class MGALockScopeKind : u16 {
  database,
  filespace,
  page,
  relation,
  record_lineage,
  record_version,
  index,
  index_key,
  index_key_range,
  predicate_guard,
  catalog_object_uuid,
  metadata_name_binding,
  domain_object,
  udr_package,
  security_policy_generation,
  archive_descriptor,
  backup_boundary,
  cluster_transaction_identifier,
  cluster_member_epoch,
  cluster_routing_or_placement_unit,
  unknown
};

enum class MGALockMode : u16 {
  intent_shared,
  intent_exclusive,
  shared_read,
  shared_stability,
  update_intent,
  exclusive_write,
  schema_stability,
  schema_modify,
  range_shared,
  range_update,
  range_insert_guard,
  range_exclusive,
  maintenance_shared,
  maintenance_exclusive,
  recovery_exclusive,
  archive_shared,
  archive_exclusive,
  cluster_prepare,
  cluster_decision,
  unknown
};

enum class MGAWaitPolicy : u16 {
  no_wait,
  wait_forever,
  wait_timeout,
  wait_until_statement_end,
  wait_until_transaction_end,
  wait_with_deadlock_detection,
  wait_with_priority,
  donor_compatible_wait,
  maintenance_window_wait,
  unknown
};

enum class MGAPriorityClass : u16 {
  normal,
  foreground,
  maintenance,
  recovery,
  emergency,
  cluster_decision,
  unknown
};

enum class MGALockReleaseCondition : u16 {
  statement_end,
  transaction_end,
  operation_phase_end,
  operation_end,
  recovery_end,
  explicit_release,
  unknown
};

enum class MGADurableRecoverySource : u16 {
  transaction_inventory,
  operation_envelope,
  cluster_decision,
  none,
  unknown
};

enum class MGALockLifecycleDecision : u16 {
  granted,
  already_owned,
  wait_queued,
  lock_refused,
  wait_timeout,
  wait_cancelled,
  owner_disconnected,
  deadlock_detected,
  deadlock_victim,
  released,
  shutdown_cleanup,
  admission_rejected,
  cluster_absent,
  cluster_decision_required,
  invalid_request
};

struct MGALockRequest {
  std::string request_id;
  MGALockOwnerKind owner_kind = MGALockOwnerKind::unknown;
  std::string owner_uuid;
  std::string transaction_uuid = "none";
  std::string statement_uuid = "none";
  std::string operation_id = "none";
  MGAWaitResourceKind resource_kind = MGAWaitResourceKind::logical_lock;
  MGALockScopeKind scope_kind = MGALockScopeKind::unknown;
  std::string scope_uuid = "none";
  std::string record_uuid = "none";
  std::string table_uuid = "none";
  std::string database_uuid;
  std::string cluster_uuid = "none";
  u64 route_generation = 0;
  bool route_generation_present = false;
  std::string route_owner_token = "none";
  MGALockMode mode = MGALockMode::unknown;
  MGAWaitPolicy wait_policy = MGAWaitPolicy::wait_timeout;
  u64 timeout_millis = 0;
  MGAPriorityClass priority_class = MGAPriorityClass::normal;
  std::string donor_profile = "none";
  u64 policy_epoch = 0;
  u64 requested_at_millis = 0;
  bool cluster_exists = false;
  bool distributed_wait = false;
  bool recovery_critical = false;
  u64 durable_work_completed = 0;
  u64 transaction_age_millis = 0;
  u64 locks_held_hint = 0;
  u64 retry_cost = 0;
  MGALockReleaseCondition release_condition = MGALockReleaseCondition::transaction_end;
  MGADurableRecoverySource durable_recovery_source = MGADurableRecoverySource::none;
};

struct MGALockGrant {
  std::string grant_id;
  std::string request_id;
  std::string owner_uuid;
  MGALockOwnerKind owner_kind = MGALockOwnerKind::unknown;
  MGAWaitResourceKind resource_kind = MGAWaitResourceKind::logical_lock;
  MGALockScopeKind scope_kind = MGALockScopeKind::unknown;
  std::string scope_uuid = "none";
  std::string database_uuid;
  MGALockMode granted_mode = MGALockMode::unknown;
  u64 granted_at_millis = 0;
  u64 grant_generation = 0;
  MGALockReleaseCondition release_condition = MGALockReleaseCondition::unknown;
  MGADurableRecoverySource durable_recovery_source = MGADurableRecoverySource::unknown;
};

struct MGALockWait {
  std::string wait_id;
  std::string request_id;
  std::string owner_uuid;
  MGAWaitResourceKind resource_kind = MGAWaitResourceKind::logical_lock;
  MGALockScopeKind scope_kind = MGALockScopeKind::unknown;
  std::string scope_uuid = "none";
  std::string database_uuid;
  MGALockMode mode = MGALockMode::unknown;
  std::vector<std::string> blocked_by_grant_ids;
  u64 wait_started_at_millis = 0;
  u64 deadline_millis = 0;
  MGAWaitPolicy wait_policy = MGAWaitPolicy::unknown;
  bool deadlock_detection_required = false;
  bool distributed_wait = false;
  std::string public_summary;
  std::string protected_summary;
};

struct MGADeadlockRecord {
  std::vector<std::string> cycle_owner_uuids;
  std::string victim_owner_uuid;
  std::string victim_wait_id;
  bool distributed = false;
  DiagnosticRecord diagnostic;
};

struct MGALockLifecycleMetrics {
  u64 lock_requests = 0;
  u64 lock_grants = 0;
  u64 lock_waits = 0;
  u64 lock_timeouts = 0;
  u64 lock_cancellations = 0;
  u64 disconnect_cleanups = 0;
  u64 deadlocks_detected = 0;
  u64 deadlock_victims = 0;
  u64 shutdown_cleanups = 0;
  u64 cluster_state_records = 0;
};

struct MGALockLifecycleResult {
  Status status;
  MGALockLifecycleDecision decision = MGALockLifecycleDecision::invalid_request;
  MGALockGrant grant;
  MGALockWait wait;
  std::string victim_owner_uuid;
  std::string victim_wait_id;
  u64 cleanup_count = 0;
  DiagnosticRecord diagnostic;

  bool ok() const {
    return status.ok() && (decision == MGALockLifecycleDecision::granted ||
                           decision == MGALockLifecycleDecision::already_owned ||
                           decision == MGALockLifecycleDecision::released ||
                           decision == MGALockLifecycleDecision::owner_disconnected ||
                           decision == MGALockLifecycleDecision::shutdown_cleanup);
  }
};

class MGALockWaitLifecycle {
 public:
  MGALockLifecycleResult Acquire(MGALockRequest request, u64 now_millis);
  MGALockLifecycleResult ReleaseGrant(std::string grant_id, u64 now_millis);
  MGALockLifecycleResult ReleaseOwner(std::string owner_uuid, u64 now_millis);
  MGALockLifecycleResult CancelWait(std::string wait_id, u64 now_millis);
  MGALockLifecycleResult DisconnectOwner(std::string owner_uuid, u64 now_millis);
  std::vector<MGALockLifecycleResult> ProcessTimeouts(u64 now_millis);
  MGALockLifecycleResult DetectAndResolveDeadlock(u64 now_millis);
  MGALockLifecycleResult Shutdown(u64 now_millis);

  u64 grant_count() const;
  u64 wait_count() const;
  MGALockLifecycleMetrics metrics() const;
  std::vector<MGALockGrant> grants() const;
  std::vector<MGALockWait> waits() const;
  std::vector<MGADeadlockRecord> deadlock_history() const;

  struct GrantRecord {
    MGALockGrant grant;
    MGALockRequest request;
    std::string resource_key;
  };

  struct WaitRecord {
    MGALockWait wait;
    MGALockRequest request;
    std::string resource_key;
    u64 queue_sequence = 0;
  };

 private:
  std::map<std::string, GrantRecord> grants_;
  std::map<std::string, WaitRecord> waits_;
  std::vector<std::string> wait_order_;
  std::vector<MGADeadlockRecord> deadlock_history_;
  MGALockLifecycleMetrics metrics_;
  u64 generation_ = 0;
  u64 wait_sequence_ = 0;
  bool accepting_new_requests_ = true;

  std::vector<MGALockGrant> DrainGrantableWaits(u64 now_millis);
};

const char* MGAWaitResourceKindName(MGAWaitResourceKind resource_kind);
const char* MGALockOwnerKindName(MGALockOwnerKind owner_kind);
const char* MGALockScopeKindName(MGALockScopeKind scope_kind);
const char* MGALockModeName(MGALockMode mode);
const char* MGAWaitPolicyName(MGAWaitPolicy policy);
const char* MGAPriorityClassName(MGAPriorityClass priority_class);
const char* MGALockLifecycleDecisionName(MGALockLifecycleDecision decision);

bool MGALockModesCompatible(MGALockMode requested, MGALockMode existing);
DiagnosticRecord MakeMGALockLifecycleDiagnostic(Status status,
                                                std::string diagnostic_code,
                                                std::string message_key,
                                                const MGALockRequest* request = nullptr,
                                                const MGALockWait* wait = nullptr,
                                                std::string required_action = {},
                                                bool protected_detail = true);

}  // namespace scratchbird::transaction::mga
