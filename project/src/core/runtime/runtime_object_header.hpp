// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "reservation_backed_memory_resource.hpp"
#include "uuid.hpp"
#include <array>
#include <chrono>
#include <memory_resource>
#include <optional>
#include <string>
#include <type_traits>
#include <vector>

namespace scratchbird::core::runtime {
using platform::Uuid;
using RuntimeGeneration = std::uint64_t;
using RuntimeDependencyHash = std::array<std::uint8_t, 32>;

// Process-local discriminants, not catalog/wire ordinals. Subtypes and lifecycle
// states remain owned by their manifest-listed domain, not by a private registry.
enum class RuntimeObjectFamily : std::uint8_t {
  environment, node, open_generation, component, service, agent, worker, request,
  execution, session, job, stream, resource_grant, filespace_service, provider,
  continuation
};
enum class RuntimeCancellationState : std::uint8_t {
  none, requested, acknowledged
};

// Exact value key only. UUID issuance, uniqueness, current-open admission and
// serialized replacement belong to the owning runtime. Equality is deliberately
// over the entire key: neither thread IDs nor generation counters alone suffice.
struct RuntimeIdentityGeneration {
  Uuid runtime_object_uuid;
  Uuid owner_node_uuid;
  Uuid parent_runtime_uuid;
  Uuid open_generation_uuid;
  RuntimeGeneration runtime_generation = 0;
  bool operator==(const RuntimeIdentityGeneration&) const noexcept = default;
};

// Node-bound child profile only. Root and explicitly cluster-global objects have
// different nullability rules and must not call this checker. Zero is not an
// invented reserved generation: the owning contract selects the initial value.
inline bool NodeChildIdentityShape(const RuntimeIdentityGeneration& value) noexcept {
  return uuid::IsEngineIdentityUuid(value.runtime_object_uuid) &&
      uuid::IsEngineIdentityUuid(value.owner_node_uuid) &&
      uuid::IsEngineIdentityUuid(value.parent_runtime_uuid) &&
      uuid::IsEngineIdentityUuid(value.open_generation_uuid) &&
      value.runtime_object_uuid != value.parent_runtime_uuid;
}

// SEARCH_KEY: CDE_RUNTIME_OBJECT_HEADER_RECORD
// Shared, owning metadata schema for CDE-DOS-RUNTIME-OBJECT-MODEL. The owning
// subsystem supplies its exact lifecycle enum and canonical subtype. This is
// NOT an authenticated capability or an admission result. UUID references must
// be resolved/revalidated through their real authorities before execution.
//
// Containers use the explicitly supplied governed resource; no default PMR
// fallback or cross-resource copy/move. The grant, adapter and physical manager
// must outlive this record. The containing runtime separately retains actual
// capabilities/leases and synchronizes all mutation/observation. Destroying this
// metadata does not release those capabilities or acknowledge completion.
template <typename LifecycleState>
struct CdeRuntimeObjectHeader {
  static_assert(std::is_enum_v<LifecycleState>);
  explicit CdeRuntimeObjectHeader(memory::ReservationBackedPmrMemoryResource& memory)
      : runtime_subtype(&memory), authority_refs(&memory), resource_grant_refs(&memory) {}
  CdeRuntimeObjectHeader(const CdeRuntimeObjectHeader&) = delete;
  CdeRuntimeObjectHeader& operator=(const CdeRuntimeObjectHeader&) = delete;
  CdeRuntimeObjectHeader(CdeRuntimeObjectHeader&&) = delete;
  CdeRuntimeObjectHeader& operator=(CdeRuntimeObjectHeader&&) = delete;

  RuntimeIdentityGeneration identity;
  std::optional<RuntimeObjectFamily> runtime_kind;
  std::pmr::string runtime_subtype;
  std::optional<LifecycleState> lifecycle_state;
  std::pmr::vector<Uuid> authority_refs;
  Uuid policy_uuid;
  RuntimeGeneration policy_generation = 0;
  Uuid security_principal_uuid;
  Uuid resource_domain_uuid;
  std::pmr::vector<Uuid> resource_grant_refs;
  // steady_clock deadlines are valid only in this live process/open lifetime.
  // Persisted/recovered deadlines must be rebound by their owning time policy.
  std::optional<std::chrono::steady_clock::time_point> deadline;
  RuntimeCancellationState cancellation_state = RuntimeCancellationState::none;
  std::optional<RuntimeDependencyHash> dependency_generation_hash;
  Uuid evidence_uuid;
};
} // namespace scratchbird::core::runtime
