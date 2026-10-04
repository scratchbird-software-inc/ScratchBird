// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "runtime_object_header.hpp"
#include <string_view>

namespace scratchbird::core::runtime {
// Process-local enums, not durable or wire encodings.
enum class RuntimeSchedulerClass : std::uint8_t {
  database_local, security_database, cluster_member, cluster_coordinator, offline_tool, recovery_only
};
enum class RuntimeSchedulerState : std::uint8_t {
  starting, restricted, active, pausing, paused, draining, stopping, stopped, failed
};
enum class RuntimeQueueName : std::uint8_t {
  foreground_assist_queue, short_background_queue, long_background_queue, maintenance_queue,
  io_bound_queue, cpu_bound_queue, archive_queue, backup_restore_queue, verification_queue,
  cluster_queue, emergency_queue, policy_queue
};
enum class RuntimeQueueState : std::uint8_t { active, paused, draining, disabled, failed };

struct SchedulerGenerationBinding {
  RuntimeGeneration startup = 0, recovery = 0, metrics = 0;
  bool operator==(const SchedulerGenerationBinding&) const noexcept = default;
};

// SEARCH_KEY: SHARED_SCHEDULER_QUEUE_DESCRIPTOR_RECORDS
// Node-hosted metadata profile. Header identity supplies the scheduler UUID;
// header policy_uuid supplies policy_profile_uuid, without independently mutable
// duplicates. Common-header subtype, authority and resource references remain
// owning-domain selections. These records create no catalog, lease or issuer.
struct SchedulerDescriptorRecord {
  explicit SchedulerDescriptorRecord(memory::ReservationBackedPmrMemoryResource& memory) : header(memory) {
    header.lifecycle_state = RuntimeSchedulerState::starting;
  }
  CdeRuntimeObjectHeader<RuntimeSchedulerState> header;
  Uuid engine_uuid;
  std::optional<Uuid> database_uuid, cluster_uuid;
  std::optional<RuntimeSchedulerClass> scheduler_class;
  Uuid resource_profile_uuid, queue_profile_uuid;
  SchedulerGenerationBinding generations;
};

// Header identity supplies queue_uuid and header policy_uuid is the selected
// admission_policy_uuid. Numeric maxima retain the canonical uint64 range;
// binding to a narrower physical implementation must reject unsupported values,
// never truncate them. Zero depth/concurrency are valid policy selections.
struct TaskQueueRecord {
  explicit TaskQueueRecord(memory::ReservationBackedPmrMemoryResource& memory) : header(memory) {
    header.lifecycle_state = RuntimeQueueState::active;
  }
  CdeRuntimeObjectHeader<RuntimeQueueState> header;
  std::optional<RuntimeQueueName> queue_name;
  Uuid scheduler_uuid, fairness_profile_uuid;
  std::uint64_t max_depth = 0, max_concurrent = 0;
};

enum class SchedulerDescriptorError : std::uint8_t {
  none, scheduler_invalid, engine_invalid, database_invalid, cluster_path_absent,
  class_invalid, policy_missing, resource_profile_missing, queue_profile_missing,
  state_invalid, startup_generation_stale, recovery_generation_stale, metrics_generation_stale
};
struct SchedulerDescriptorRequirements {
  // Derived from the owning installation/profile; not request-controlled
  // authorization switches. A true value does not authenticate a cluster.
  bool database_required = false;
  bool cluster_exists = false;
};
inline SchedulerDescriptorError CheckSchedulerDescriptorShape(const SchedulerDescriptorRecord& value,
    SchedulerDescriptorRequirements requirements = {}) noexcept {
  using E = SchedulerDescriptorError;
  if (!NodeChildIdentityShape(value.header.identity)) return E::scheduler_invalid;
  if (!uuid::IsEngineIdentityUuid(value.engine_uuid)) return E::engine_invalid;
  if (!value.scheduler_class || static_cast<unsigned>(*value.scheduler_class) >
      static_cast<unsigned>(RuntimeSchedulerClass::recovery_only)) return E::class_invalid;
  const bool local = *value.scheduler_class == RuntimeSchedulerClass::database_local;
  if ((value.database_uuid && !uuid::IsEngineIdentityUuid(*value.database_uuid)) ||
      ((local || requirements.database_required) && !value.database_uuid)) return E::database_invalid;
  const bool cluster_scoped = *value.scheduler_class == RuntimeSchedulerClass::cluster_member ||
      *value.scheduler_class == RuntimeSchedulerClass::cluster_coordinator;
  if ((value.cluster_uuid && (!requirements.cluster_exists || !uuid::IsEngineIdentityUuid(*value.cluster_uuid))) ||
      (cluster_scoped && (!requirements.cluster_exists || !value.cluster_uuid))) return E::cluster_path_absent;
  if (!uuid::IsEngineIdentityUuid(value.header.policy_uuid)) return E::policy_missing;
  if (!uuid::IsEngineIdentityUuid(value.resource_profile_uuid)) return E::resource_profile_missing;
  if (!uuid::IsEngineIdentityUuid(value.queue_profile_uuid)) return E::queue_profile_missing;
  if (!value.header.lifecycle_state || static_cast<unsigned>(*value.header.lifecycle_state) >
      static_cast<unsigned>(RuntimeSchedulerState::failed)) return E::state_invalid;
  return E::none;
}

// Value comparison only, to be called while actual owner/source fences retain
// both records. A caller-supplied matching tuple is not current-generation proof.
inline SchedulerDescriptorError CompareSchedulerGenerations(const SchedulerGenerationBinding& recorded,
    const SchedulerGenerationBinding& current) noexcept {
  using E = SchedulerDescriptorError;
  if (recorded.startup != current.startup) return E::startup_generation_stale;
  if (recorded.recovery != current.recovery) return E::recovery_generation_stale;
  if (recorded.metrics != current.metrics) return E::metrics_generation_stale;
  return E::none;
}

enum class QueueDescriptorError : std::uint8_t {
  none, queue_invalid, name_invalid, scheduler_invalid, admission_policy_missing, fairness_profile_missing, state_invalid
};
inline QueueDescriptorError CheckTaskQueueShape(const TaskQueueRecord& value) noexcept {
  using E = QueueDescriptorError;
  if (!NodeChildIdentityShape(value.header.identity)) return E::queue_invalid;
  if (!value.queue_name || static_cast<unsigned>(*value.queue_name) >
      static_cast<unsigned>(RuntimeQueueName::policy_queue)) return E::name_invalid;
  if (!uuid::IsEngineIdentityUuid(value.scheduler_uuid)) return E::scheduler_invalid;
  if (!uuid::IsEngineIdentityUuid(value.header.policy_uuid)) return E::admission_policy_missing;
  if (!uuid::IsEngineIdentityUuid(value.fairness_profile_uuid)) return E::fairness_profile_missing;
  if (!value.header.lifecycle_state || static_cast<unsigned>(*value.header.lifecycle_state) >
      static_cast<unsigned>(RuntimeQueueState::failed)) return E::state_invalid;
  return E::none;
}

inline std::string_view SchedulerDescriptorDiagnostic(SchedulerDescriptorError error) noexcept {
  using E = SchedulerDescriptorError;
  switch (error) {
    case E::none: return {};
    case E::scheduler_invalid: return "MGA.SCHED.SCHEDULER_INVALID";
    case E::engine_invalid: return "MGA.SCHED.ENGINE_INVALID";
    case E::database_invalid: return "MGA.SCHED.DATABASE_INVALID";
    case E::cluster_path_absent: return "PROCESS.CLUSTER_PATH_ABSENT";
    case E::class_invalid: return "MGA.SCHED.CLASS_INVALID";
    case E::policy_missing: return "MGA.SCHED.POLICY_MISSING";
    case E::resource_profile_missing: return "MGA.SCHED.RESOURCE_PROFILE_MISSING";
    case E::queue_profile_missing: return "MGA.SCHED.QUEUE_PROFILE_MISSING";
    case E::state_invalid: return "MGA.SCHED.SCHEDULER_STATE_INVALID";
    case E::startup_generation_stale: return "MGA.SCHED.STARTUP_GENERATION_STALE";
    case E::recovery_generation_stale: return "MGA.SCHED.RECOVERY_GENERATION_STALE";
    case E::metrics_generation_stale: return "MGA.SCHED.METRICS_GENERATION_STALE";
  }
  return "MGA.SCHED.SCHEDULER_INVALID";
}
inline std::string_view QueueDescriptorDiagnostic(QueueDescriptorError error) noexcept {
  using E = QueueDescriptorError;
  switch (error) {
    case E::none: return {};
    case E::queue_invalid: return "MGA.SCHED.QUEUE_INVALID";
    case E::name_invalid: return "MGA.SCHED.QUEUE_NAME_INVALID";
    case E::scheduler_invalid: return "MGA.SCHED.SCHEDULER_INVALID";
    case E::admission_policy_missing: return "MGA.SCHED.ADMISSION_POLICY_MISSING";
    case E::fairness_profile_missing: return "MGA.SCHED.FAIRNESS_PROFILE_MISSING";
    case E::state_invalid: return "MGA.SCHED.QUEUE_STATE_INVALID";
  }
  return "MGA.SCHED.QUEUE_INVALID";
}
} // namespace scratchbird::core::runtime
