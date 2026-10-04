// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "runtime_object_header.hpp"

namespace scratchbird::core::runtime {
// Fixed-size binary identity projections, not full task/worker descriptors.
// Store/bind these alongside the common header; callers retain the descriptor,
// target, budgets, real authority leases and completion evidence separately.
// Copies are observations, never transferable execution/worker permits.
struct RuntimeTaskIdentityRecord {
  RuntimeIdentityGeneration identity;
  Uuid scheduler_uuid;
  bool operator==(const RuntimeTaskIdentityRecord&) const noexcept = default;
};

struct RuntimeWorkerIdentityRecord {
  RuntimeIdentityGeneration identity;
  // The single L3 manager is identity.parent_runtime_uuid. Do not keep a second
  // mutable parent UUID which can diverge. Its generation is bound explicitly.
  RuntimeIdentityGeneration manager;
  RuntimeTaskIdentityRecord task;
  bool operator==(const RuntimeWorkerIdentityRecord&) const noexcept = default;
};

enum class RuntimeIdentityShapeError : std::uint8_t {
  none, invalid_task, invalid_scheduler, invalid_worker, invalid_manager,
  aliased_identity, wrong_parent, wrong_node, wrong_open_generation
};

// Structural checks only, including binary UUIDv7 representation. A successful
// result does not prove freshness, L3 role, subtype registration, policy, resource
// admission or an allowed lifecycle transition. Those require the live owners.
inline RuntimeIdentityShapeError CheckTaskIdentityShape(
    const RuntimeTaskIdentityRecord& task) noexcept {
  if (!NodeChildIdentityShape(task.identity)) return RuntimeIdentityShapeError::invalid_task;
  if (!uuid::IsEngineIdentityUuid(task.scheduler_uuid))
    return RuntimeIdentityShapeError::invalid_scheduler;
  if (task.identity.runtime_object_uuid == task.scheduler_uuid)
    return RuntimeIdentityShapeError::aliased_identity;
  return RuntimeIdentityShapeError::none;
}

inline RuntimeIdentityShapeError CheckWorkerIdentityShape(
    const RuntimeWorkerIdentityRecord& worker) noexcept {
  using E = RuntimeIdentityShapeError;
  if (!NodeChildIdentityShape(worker.identity)) return E::invalid_worker;
  if (!NodeChildIdentityShape(worker.manager)) return E::invalid_manager;
  const auto task_error = CheckTaskIdentityShape(worker.task);
  if (task_error != E::none) return task_error;
  if (worker.identity.runtime_object_uuid == worker.task.identity.runtime_object_uuid ||
      worker.manager.runtime_object_uuid == worker.task.identity.runtime_object_uuid ||
      worker.identity.runtime_object_uuid == worker.task.scheduler_uuid ||
      worker.manager.runtime_object_uuid == worker.task.scheduler_uuid)
    return E::aliased_identity;
  if (worker.identity.parent_runtime_uuid != worker.manager.runtime_object_uuid)
    return E::wrong_parent;
  if (worker.identity.owner_node_uuid != worker.manager.owner_node_uuid ||
      worker.identity.owner_node_uuid != worker.task.identity.owner_node_uuid)
    return E::wrong_node;
  if (worker.identity.open_generation_uuid != worker.manager.open_generation_uuid ||
      worker.identity.open_generation_uuid != worker.task.identity.open_generation_uuid)
    return E::wrong_open_generation;
  return E::none;
}

// SEARCH_KEY: RUNTIME_TASK_WORKER_EXACT_GENERATION_BINDING
// Compare snapshots under the caller's retained owner fence. Even matching
// values do not acquire a fence or authorize publication, retry, or dispatch.
inline bool WorkerMatchesCurrentIdentities(
    const RuntimeWorkerIdentityRecord& worker,
    const RuntimeIdentityGeneration& current_worker,
    const RuntimeIdentityGeneration& current_manager,
    const RuntimeTaskIdentityRecord& current_task) noexcept {
  return CheckWorkerIdentityShape(worker) == RuntimeIdentityShapeError::none &&
      worker.identity == current_worker && worker.manager == current_manager &&
      worker.task == current_task;
}

template <typename LifecycleState>
bool HeaderMatchesIdentity(const CdeRuntimeObjectHeader<LifecycleState>& header,
                           const RuntimeIdentityGeneration& identity) noexcept {
  return NodeChildIdentityShape(identity) && header.identity == identity;
}
} // namespace scratchbird::core::runtime
