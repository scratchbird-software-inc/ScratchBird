// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// CEIC-013: reservation-backed typed slab pools and size-class allocators.
#include "reservation_backed_memory_resource.hpp"

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace scratchbird::core::memory {

using scratchbird::core::platform::DiagnosticRecord;
using scratchbird::core::platform::Status;
using scratchbird::core::platform::u64;
using scratchbird::core::platform::usize;

enum class TypedSlabPoolObjectKind {
  // CEIC-028_TYPED_POOL_ARENA_OBJECT_COVERAGE
  planner_node,
  plan_node,
  expression_node,
  predicate_node,
  executor_frame,
  row_batch,
  row_locator,
  index_cursor,
  candidate_chunk,
  candidate_set,
  posting_list_chunk,
  hash_bucket,
  sort_descriptor,
  vector_scratch,
  diagnostic_record,
  metric_label,
  page_cache_metadata
};

struct TypedSlabPoolAuthority {
  bool engine_mga_authoritative = true;
  bool transaction_inventory_authoritative = true;
  bool security_or_policy_checked = true;
  bool parser_or_donor_finality_authority = false;
  bool memory_visibility_or_finality_authority = false;
  bool memory_recovery_authority = false;
  bool memory_authorization_authority = false;
  bool benchmark_authority = false;
  bool cluster_authority = false;
  bool debug_or_relaxed_path = false;
  bool optimizer_plan_authority = false;
  bool index_finality_authority = false;
  bool agent_action_authority = false;
};

struct SizeClassConfig {
  usize payload_bytes = 0;
  usize slots_per_slab = 0;
};

struct SizeClassAllocatorRequest {
  HierarchicalMemoryBudgetLedger* reservation_ledger = nullptr;
  MemoryManager* memory_manager = nullptr;
  std::vector<HierarchicalMemoryScopeRef> scope_chain;
  MemoryCategory category = MemoryCategory::unknown;
  std::string memory_class = "typed_slab_pool";
  u64 reservation_bytes = 0;
  std::string owner_id;
  std::string route_label;
  std::string operation_id;
  std::string purpose = "typed_slab_pool";
  TypedSlabPoolObjectKind object_kind = TypedSlabPoolObjectKind::executor_frame;
  std::vector<SizeClassConfig> size_classes;
  bool spillable = false;
  bool cancelable = true;
  int priority = 0;
  u64 weight = 1;
  u64 lease_expires_at_ms = 0;
  bool production_like = true;
  HierarchicalMemoryBudgetProvenance provenance;
  TypedSlabPoolAuthority authority;
};

struct SizeClassAllocationRequest {
  usize bytes = 0;
  usize alignment = 0;
  std::string purpose;
};

struct SizeClassSnapshot {
  usize payload_bytes = 0;
  usize slot_stride_bytes = 0;
  usize slots_per_slab = 0;
  u64 slab_count = 0;
  u64 total_slots = 0;
  u64 active_slots = 0;
  u64 free_slots = 0;
  u64 quarantined_slots = 0;
  u64 allocation_count = 0;
  u64 deallocation_count = 0;
  u64 reuse_count = 0;
  u64 allocation_refusal_count = 0;
  u64 corruption_refusal_count = 0;
  u64 slab_allocation_count = 0;
  u64 high_watermark_active_slots = 0;
  u64 retained_bytes = 0;
  u64 active_requested_bytes = 0;
  u64 active_size_class_bytes = 0;
  u64 reusable_payload_bytes = 0;
  u64 internal_fragmentation_bytes = 0;
  u64 occupancy_basis_points = 0;
  u64 fragmentation_basis_points = 0;
};

struct SizeClassPoolSnapshot {
  std::string owner_id;
  std::string route_label;
  std::string operation_id;
  std::string memory_class;
  MemoryCategory category = MemoryCategory::unknown;
  TypedSlabPoolObjectKind object_kind = TypedSlabPoolObjectKind::executor_frame;
  u64 reservation_bytes = 0;
  u64 retained_bytes = 0;
  u64 active_requested_bytes = 0;
  u64 active_size_class_bytes = 0;
  u64 reusable_payload_bytes = 0;
  u64 internal_fragmentation_bytes = 0;
  u64 allocation_count = 0;
  u64 deallocation_count = 0;
  u64 reuse_count = 0;
  u64 allocation_refusal_count = 0;
  u64 corruption_refusal_count = 0;
  u64 reset_count = 0;
  u64 release_count = 0;
  u64 active_slots = 0;
  u64 total_slots = 0;
  u64 free_slots = 0;
  u64 quarantined_slots = 0;
  u64 occupancy_basis_points = 0;
  u64 fragmentation_basis_points = 0;
  u64 allocation_latency_p50_ns = 0;
  u64 allocation_latency_p95_ns = 0;
  u64 allocation_latency_p99_ns = 0;
  bool active = false;
  std::string authority_scope;
  std::vector<SizeClassSnapshot> classes;
};

struct SizeClassAllocationResult {
  Status status;
  bool fail_closed = false;
  void* pointer = nullptr;
  usize requested_bytes = 0;
  usize size_class_bytes = 0;
  u64 allocation_id = 0;
  bool reused = false;
  DiagnosticRecord diagnostic;
  std::vector<std::string> evidence;

  bool ok() const {
    return status.ok() && pointer != nullptr && !fail_closed;
  }
};

struct SizeClassFreeResult {
  Status status;
  bool fail_closed = false;
  bool freed = false;
  bool corruption_refused = false;
  DiagnosticRecord diagnostic;
  std::vector<std::string> evidence;

  bool ok() const {
    return status.ok() && freed && !fail_closed && !corruption_refused;
  }
};

struct SizeClassAllocatorAcquireResult {
  Status status;
  bool fail_closed = false;
  std::unique_ptr<class SizeClassAllocator> allocator;
  DiagnosticRecord diagnostic;
  std::vector<std::string> evidence;

  bool ok() const {
    return status.ok() && allocator != nullptr && !fail_closed;
  }
};

const char* TypedSlabPoolObjectKindName(TypedSlabPoolObjectKind kind);
MemoryCategory TypedSlabPoolDefaultCategory(TypedSlabPoolObjectKind kind);
ReservationBackedMemoryConsumerKind TypedSlabPoolConsumerKind(
    TypedSlabPoolObjectKind kind);
std::vector<SizeClassConfig> DefaultTypedSlabSizeClasses();
SizeClassAllocatorAcquireResult CreateSizeClassAllocator(
    SizeClassAllocatorRequest request);

class SizeClassAllocator {
 public:
  SizeClassAllocator(const SizeClassAllocator&) = delete;
  SizeClassAllocator& operator=(const SizeClassAllocator&) = delete;
  ~SizeClassAllocator();

  SizeClassAllocationResult Allocate(SizeClassAllocationRequest request);
  SizeClassFreeResult Free(void* pointer);
  SizeClassFreeResult Reset();
  SizeClassFreeResult Release();
  SizeClassPoolSnapshot Snapshot() const;

 private:
  struct SlotHeader {
    std::uint64_t magic = 0;
    std::uint32_t class_index = 0;
    std::uint32_t slab_index = 0;
    std::uint32_t slot_index = 0;
    std::uint32_t state = 0;
    std::uint64_t requested_bytes = 0;
    std::uint64_t payload_bytes = 0;
    std::uint64_t allocation_id = 0;
    std::uint64_t generation = 0;
    std::uint64_t canary = 0;
  };

  struct SlotRef {
    usize class_index = 0;
    usize slab_index = 0;
    usize slot_index = 0;
  };

  struct Slab {
    void* pointer = nullptr;
    usize bytes = 0;
    usize class_index = 0;
    usize slot_count = 0;
    std::vector<usize> free_slots;
    u64 active_slots = 0;
    u64 quarantined_slots = 0;
  };

  struct SizeClassState {
    SizeClassConfig config;
    usize slot_stride_bytes = 0;
    std::vector<Slab> slabs;
    u64 allocation_count = 0;
    u64 deallocation_count = 0;
    u64 reuse_count = 0;
    u64 allocation_refusal_count = 0;
    u64 corruption_refusal_count = 0;
    u64 slab_allocation_count = 0;
    u64 high_watermark_active_slots = 0;
    u64 active_requested_bytes = 0;
    u64 active_size_class_bytes = 0;
    u64 internal_fragmentation_bytes = 0;
  };

  SizeClassAllocator(SizeClassAllocatorRequest request,
                     std::unique_ptr<ReservationBackedMemoryResource> resource);

  SizeClassAllocationResult RefuseAllocation(SizeClassAllocationRequest request,
                                             std::string diagnostic_code,
                                             std::string message_key,
                                             std::string reason,
                                             scratchbird::core::platform::StatusCode code);
  SizeClassFreeResult RefuseFree(std::string diagnostic_code,
                                 std::string message_key,
                                 std::string reason,
                                 scratchbird::core::platform::StatusCode code) const;
  usize FindSizeClass(usize bytes) const;
  bool EnsureSlab(usize class_index, SizeClassAllocationResult* result);
  SizeClassAllocationResult AllocateFromClass(usize class_index,
                                              SizeClassAllocationRequest request);
  SizeClassFreeResult FreeValidated(void* pointer, SlotRef ref);
  bool ValidateSlotCanaries(const SlotRef& ref, std::string* reason) const;
  void InitializeSlabSlots(Slab* slab);
  SlotHeader* HeaderFor(const SlotRef& ref) const;
  unsigned char* PayloadFor(const SlotRef& ref) const;
  unsigned char* TailCanaryFor(const SlotRef& ref) const;
  std::uint64_t ReadTailCanary(const SlotRef& ref) const;
  void WriteTailCanary(const SlotRef& ref, std::uint64_t value);
  std::uint64_t SlotCanary(const SlotHeader& header, const void* payload) const;
  SizeClassPoolSnapshot SnapshotLocked() const;
  void RecordLatency(u64 latency_ns);
  void AppendBaseEvidence(std::vector<std::string>* evidence) const;

  SizeClassAllocatorRequest request_;
  std::unique_ptr<ReservationBackedMemoryResource> resource_;
  mutable std::mutex mutex_;
  std::vector<SizeClassState> classes_;
  std::unordered_map<void*, SlotRef> active_;
  std::vector<u64> allocation_latencies_ns_;
  u64 allocation_sequence_ = 0;
  u64 reset_count_ = 0;
  u64 release_count_ = 0;
  bool released_ = false;

  friend SizeClassAllocatorAcquireResult CreateSizeClassAllocator(
      SizeClassAllocatorRequest request);
};

template <typename T>
struct TypedSlabAllocationResult {
  Status status;
  bool fail_closed = false;
  T* pointer = nullptr;
  DiagnosticRecord diagnostic;
  std::vector<std::string> evidence;

  bool ok() const {
    return status.ok() && pointer != nullptr && !fail_closed;
  }
};

template <typename T>
class TypedSlabPool {
 public:
  TypedSlabPool(SizeClassAllocator* allocator,
                TypedSlabPoolObjectKind object_kind,
                std::string purpose)
      : allocator_(allocator),
        object_kind_(object_kind),
        purpose_(std::move(purpose)) {}
  TypedSlabPool(const TypedSlabPool&) = delete;
  TypedSlabPool& operator=(const TypedSlabPool&) = delete;
  ~TypedSlabPool() {
    (void)Reset();
  }

  template <typename... Args>
  TypedSlabAllocationResult<T> Make(Args&&... args) {
    TypedSlabAllocationResult<T> result;
    if (allocator_ == nullptr) {
      result.status = {scratchbird::core::platform::StatusCode::memory_invalid_request,
                       scratchbird::core::platform::Severity::error,
                       scratchbird::core::platform::Subsystem::memory};
      result.fail_closed = true;
      return result;
    }
    SizeClassAllocationRequest request;
    request.bytes = sizeof(T);
    request.alignment = alignof(T);
    request.purpose = purpose_;
    auto allocation = allocator_->Allocate(std::move(request));
    result.status = allocation.status;
    result.diagnostic = allocation.diagnostic;
    result.evidence = allocation.evidence;
    result.fail_closed = allocation.fail_closed;
    if (!allocation.ok()) {
      return result;
    }

    T* pointer = static_cast<T*>(allocation.pointer);
    try {
      ::new (static_cast<void*>(pointer)) T(std::forward<Args>(args)...);
    } catch (...) {
      auto released = allocator_->Free(pointer);
      result.status = {scratchbird::core::platform::StatusCode::memory_allocation_failed,
                       scratchbird::core::platform::Severity::error,
                       scratchbird::core::platform::Subsystem::memory};
      result.fail_closed = true;
      result.diagnostic = released.diagnostic;
      result.pointer = nullptr;
      return result;
    }
    active_.insert(pointer);
    result.pointer = pointer;
    result.evidence.push_back("typed_slab_pool.object_kind=" +
                              std::string(TypedSlabPoolObjectKindName(object_kind_)));
    result.evidence.push_back("typed_slab_pool.typed_constructed=true");
    return result;
  }

  SizeClassFreeResult Free(T* pointer) {
    if (pointer == nullptr) {
      SizeClassFreeResult result;
      result.status = {scratchbird::core::platform::StatusCode::ok,
                       scratchbird::core::platform::Severity::info,
                       scratchbird::core::platform::Subsystem::memory};
      result.freed = true;
      return result;
    }
    auto it = active_.find(pointer);
    if (it != active_.end()) {
      if constexpr (!std::is_trivially_destructible<T>::value) {
        pointer->~T();
      }
      active_.erase(it);
    }
    return allocator_ == nullptr ? SizeClassFreeResult{} : allocator_->Free(pointer);
  }

  SizeClassFreeResult Reset() {
    std::vector<T*> active(active_.begin(), active_.end());
    SizeClassFreeResult last;
    last.status = {scratchbird::core::platform::StatusCode::ok,
                   scratchbird::core::platform::Severity::info,
                   scratchbird::core::platform::Subsystem::memory};
    last.freed = true;
    for (T* pointer : active) {
      last = Free(pointer);
    }
    active_.clear();
    return last;
  }

  usize active_count() const {
    return active_.size();
  }

 private:
  SizeClassAllocator* allocator_ = nullptr;
  TypedSlabPoolObjectKind object_kind_ = TypedSlabPoolObjectKind::executor_frame;
  std::string purpose_;
  std::unordered_set<T*> active_;
};

}  // namespace scratchbird::core::memory
