// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "memory.hpp"
#include <cstdint>
#include <exception>
#include <limits>
#include <optional>

namespace scratchbird::core::concurrency {
// Core's common order, not permission to acquire or an exception registry.
enum class LatchClass : std::uint8_t {
  unspecified, engine_lifecycle, database_state, transaction_inventory,
  catalog_metadata, relation_index_descriptor, filespace_descriptor,
  allocation_map, page_cache_bucket, page, record_lineage, archive_descriptor,
  temporary_storage, metrics_evidence
};
struct LatchOrderConflict {
  memory::MemoryBinaryUuid held_primitive{};
  std::uint64_t held_generation = 0;
  LatchClass held_class = LatchClass::unspecified;
};
namespace detail {
struct HeldLatchNode;
struct HeldLatches {
  HeldLatchNode* head = nullptr;
  std::uint32_t count = 0;
};
inline thread_local HeldLatches held_latches;
// Each node belongs to nonmoving governed storage, never a movable wrapper.
// Native release validates thread affinity before unlink. Only the owning
// native thread accesses this list; other executions of a task are independent.
struct HeldLatchNode {
  LatchOrderConflict identity;
  HeldLatchNode* previous = nullptr;
  HeldLatchNode* next = nullptr;
  bool linked = false;
  HeldLatchNode() = default;
  HeldLatchNode(const HeldLatchNode&) = delete;
  HeldLatchNode& operator=(const HeldLatchNode&) = delete;
  ~HeldLatchNode() { if (linked || previous || next) std::terminate(); }
  void Link(LatchOrderConflict binding) noexcept {
    auto& held = held_latches;
    if (linked || held.count == std::numeric_limits<std::uint32_t>::max()) std::terminate();
    identity = binding;
    previous = nullptr; next = held.head;
    if (next) next->previous = this;
    held.head = this; ++held.count; linked = true;
  }
  void Unlink() noexcept {
    auto& held = held_latches;
    if (!linked || !held.count) std::terminate();
    if (previous) previous->next = next;
    else {
      if (held.head != this) std::terminate();
      held.head = next;
    }
    if (next) next->previous = previous;
    previous = nullptr; next = nullptr; linked = false; --held.count;
  }
};
inline std::optional<LatchOrderConflict> HeldOrderConflict(LatchClass requested) noexcept {
  // Bounded traversal of actual grants; not a polling or waiting loop.
  auto* current = held_latches.head;
  auto remaining = held_latches.count;
  while (current) {
    if (!remaining-- || !current->linked) std::terminate();
    if (current->identity.held_class > requested) return current->identity;
    current = current->next;
  }
  if (remaining) std::terminate();
  return {};
}
} // namespace detail
} // namespace scratchbird::core::concurrency
