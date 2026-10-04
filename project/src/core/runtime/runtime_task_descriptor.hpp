// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "runtime_identity_records.hpp"
#include "runtime_task_state.hpp"
#include <string_view>

namespace scratchbird::core::runtime {
// Process-local ordinals only; no durable/wire encoding is established here.
enum class RuntimeTaskFamily : std::uint8_t {
  checkpoint_task, recovery_task, sweep_task, garbage_collection_task, archive_task,
  backup_task, restore_task, backup_forward_delta_task, verification_task, repair_task,
  quarantine_task, statistics_refresh_task, optimizer_feedback_task,
  policy_enforcement_task, evidence_retention_task, filespace_task, placement_task,
  cluster_catchup_task, cluster_reconciliation_task, udr_maintenance_task,
  parser_cache_task, plan_cache_task, temporary_object_cleanup_task, metrics_collection_task
};
enum class RuntimeTaskClass : std::uint8_t {
  foreground_assist, background_low_priority, background_normal, background_high_priority,
  maintenance_window, recovery_critical, cluster_coordinated, emergency,
  administrator_requested, policy_scheduled
};
enum class RuntimeTaskPriority : std::uint8_t {
  idle, low, normal, high, foreground_protective, maintenance, recovery, emergency,
  cluster_authority
};

// SEARCH_KEY: SHARED_TASK_DESCRIPTOR_RECORD
// TaskDescriptorRecord from SB-MGA-SCHED: task UUID and policy generation are
// bound once through the common header, not duplicated in independently mutable
// fields. Canonical target packet encoding and hash validation remain with the
// owning subsystem, not a scheduler-local codec or a hash guessed from a name.
// Real grants, authority leases and queue membership are deliberately not
// constructible from these serializable references. No private task catalog.
struct TaskDescriptorRecord {
  explicit TaskDescriptorRecord(memory::ReservationBackedPmrMemoryResource& memory)
      : header(memory), policy_uuids(&memory), target_descriptor(&memory), resource_budget_class(&memory) {
    header.lifecycle_state = concurrency::RuntimeTaskState::created;
  }
  CdeRuntimeObjectHeader<concurrency::RuntimeTaskState> header;
  Uuid scheduler_uuid;
  std::optional<RuntimeTaskFamily> task_family;
  std::optional<RuntimeTaskClass> task_class;
  std::optional<Uuid> operation_uuid;
  Uuid authority_uuid;
  std::pmr::vector<Uuid> policy_uuids;
  std::pmr::vector<std::byte> target_descriptor;
  std::optional<RuntimeDependencyHash> target_descriptor_hash;
  RuntimeTaskPriority priority_class = RuntimeTaskPriority::normal;
  Uuid budget_uuid;
  std::pmr::string resource_budget_class;
  std::optional<Uuid> queue_uuid;
  std::optional<Uuid> durable_state_uuid;
  std::optional<Uuid> evidence_root_uuid;
  std::optional<RuntimeDependencyHash> idempotency_key_hash;

  RuntimeTaskIdentityRecord Identity() const noexcept { return {header.identity, scheduler_uuid}; }
};

// These requirements are derived by the owning operation/policy contract, not
// accepted as untrusted request switches. Shape checking never proves that the
// caller derived them correctly. Queue-history requirements cannot be inferred
// solely from a waiting/cancelled/failed state: those may occur before or after
// initial queue admission. The owning scheduler must retain that history.
struct TaskDescriptorShapeRequirements {
  bool management_operation_bound = false;
  bool durable_progress_required = false;
  bool evidence_required = false;
  bool retryable_durable = false;
  bool has_been_queued = false;
};

enum class TaskDescriptorShapeError : std::uint8_t {
  none, task_invalid, scheduler_invalid, family_invalid, class_invalid,
  operation_missing, authority_missing, target_invalid, priority_invalid,
  budget_missing, queue_invalid, state_invalid, durable_state_missing,
  evidence_missing, idempotency_key_missing, policy_missing
};

inline bool OptionalIdentityShape(const std::optional<Uuid>& identity) noexcept {
  return !identity || uuid::IsEngineIdentityUuid(*identity);
}

// Validate only the required record's shape. In particular this does not check
// current policy generation, authenticated authority, budget availability,
// cluster existence, descriptor content, completion receipts or transition
// guards. A valid budget binding with unavailable grants may wait; an absent
// binding is an invalid descriptor, never a reason to enter waiting_resource.
inline TaskDescriptorShapeError CheckTaskDescriptorShape(
    const TaskDescriptorRecord& task, const TaskDescriptorShapeRequirements& requirements) noexcept {
  using E = TaskDescriptorShapeError;
  using S = concurrency::RuntimeTaskState;
  const auto identity = CheckTaskIdentityShape(task.Identity());
  if (identity == RuntimeIdentityShapeError::invalid_scheduler) return E::scheduler_invalid;
  if (identity != RuntimeIdentityShapeError::none) return E::task_invalid;
  if (!task.task_family || static_cast<std::uint8_t>(*task.task_family) >
      static_cast<std::uint8_t>(RuntimeTaskFamily::metrics_collection_task)) return E::family_invalid;
  if (!task.task_class || static_cast<std::uint8_t>(*task.task_class) >
      static_cast<std::uint8_t>(RuntimeTaskClass::policy_scheduled)) return E::class_invalid;
  if (!OptionalIdentityShape(task.operation_uuid) ||
      (requirements.management_operation_bound && !task.operation_uuid)) return E::operation_missing;
  if (!uuid::IsEngineIdentityUuid(task.authority_uuid)) return E::authority_missing;
  if (task.policy_uuids.empty()) return E::policy_missing;
  for (const auto& policy : task.policy_uuids)
    if (!uuid::IsEngineIdentityUuid(policy)) return E::policy_missing;
  if (!task.target_descriptor_hash || task.target_descriptor.empty()) return E::target_invalid;
  if (static_cast<std::uint8_t>(task.priority_class) >
      static_cast<std::uint8_t>(RuntimeTaskPriority::cluster_authority)) return E::priority_invalid;
  if (!uuid::IsEngineIdentityUuid(task.budget_uuid) || task.resource_budget_class.empty()) return E::budget_missing;
  if (!task.header.lifecycle_state || !concurrency::RuntimeTaskStateKnown(*task.header.lifecycle_state))
    return E::state_invalid;
  const auto state = *task.header.lifecycle_state;
  const bool needs_queue = requirements.has_been_queued || state == S::queued || state == S::running ||
      state == S::yielding || state == S::throttled || state == S::paused || state == S::completed;
  if (!OptionalIdentityShape(task.queue_uuid) || (needs_queue && !task.queue_uuid)) return E::queue_invalid;
  if (!OptionalIdentityShape(task.durable_state_uuid) ||
      ((requirements.durable_progress_required || requirements.retryable_durable) && !task.durable_state_uuid))
    return E::durable_state_missing;
  if (!OptionalIdentityShape(task.evidence_root_uuid) ||
      (requirements.evidence_required && !task.evidence_root_uuid)) return E::evidence_missing;
  if (requirements.retryable_durable && !task.idempotency_key_hash) return E::idempotency_key_missing;
  return E::none;
}

// Canonical diagnostic code selection only, not an emitted diagnostic/evidence
// vector. The caller adds original binary IDs, state, request and actual guard
// failure details through the owning diagnostic service.
constexpr std::string_view TaskDescriptorShapeDiagnostic(TaskDescriptorShapeError error) noexcept {
  using E = TaskDescriptorShapeError;
  switch (error) {
    case E::none: return {};
    case E::task_invalid: return "MGA.SCHED.TASK_INVALID";
    case E::scheduler_invalid: return "MGA.SCHED.SCHEDULER_INVALID";
    case E::family_invalid: return "MGA.SCHED.TASK_FAMILY_INVALID";
    case E::class_invalid: return "MGA.SCHED.TASK_CLASS_INVALID";
    case E::operation_missing: return "MGA.SCHED.OPERATION_MISSING";
    case E::authority_missing: return "MGA.SCHED.AUTHORITY_MISSING";
    case E::target_invalid: return "MGA.SCHED.TARGET_INVALID";
    case E::priority_invalid: return "MGA.SCHED.PRIORITY_INVALID";
    case E::budget_missing: return "MGA.SCHED.BUDGET_MISSING";
    case E::queue_invalid: return "MGA.SCHED.QUEUE_INVALID";
    case E::state_invalid: return "MGA.SCHED.TASK_STATE_INVALID";
    case E::durable_state_missing: return "MGA.SCHED.DURABLE_STATE_MISSING";
    case E::evidence_missing: return "MGA.SCHED.EVIDENCE_MISSING";
    case E::idempotency_key_missing: return "MGA.SCHED.IDEMPOTENCY_KEY_MISSING";
    case E::policy_missing: return "MGA.SCHED.POLICY_MISSING";
  }
  return "MGA.SCHED.TASK_INVALID";
}
} // namespace scratchbird::core::runtime
