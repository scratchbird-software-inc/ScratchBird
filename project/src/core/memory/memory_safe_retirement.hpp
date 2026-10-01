// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "reservation_backed_memory_resource.hpp"

#include <chrono>
#include <condition_variable>
#include <limits>
#include <mutex>
#include <new>
#include <span>
#include <stop_token>
#include <type_traits>
#include <utility>

namespace scratchbird::core::memory {

// Bounded, mutex-serialized reader protection, not a lock-free epoch manager.
// Memory lifetime only: this service cannot authorize storage cleanup or MGA
// visibility/finality. Payload mutation needs its own synchronization.
enum class SafeRetirementStatus {
  ok, invalid_request, not_initialized, closed, exhausted, stale_handle,
  allocation_failed, construction_failed, release_failed, timed_out, cancelled
};

struct SafeRetirementHandle {
  MemoryBinaryUuid domain{};
  usize slot = 0;
  u64 generation = 0;
  bool operator==(const SafeRetirementHandle&) const = default;
};

enum class SafeRetirementObjectKind {
  record_version, index_node, catalog_descriptor, plan_cache_entry,
  parser_cache_entry, temporary_descriptor, archive_stub
};
enum class SafeRetirementBoundary { operation_completion, task_completion, runtime_shutdown };

struct SafeRetirementHazard {
  MemoryBinaryUuid hazard_id{};
  MemoryBinaryUuid owner_task{};
  SafeRetirementBoundary release_required_by = SafeRetirementBoundary::operation_completion;
};

struct SafeRetirementSnapshot {
  usize constructing = 0;
  usize published = 0;
  usize retired = 0;
  usize reclaiming = 0;
  usize readers = 0;
  u64 retained_payload_bytes = 0;
  u64 metadata_bytes = 0;
  bool closed = false;
  bool initialized = false;
};

// Protected internal lifecycle evidence. No payload pointer or value is
// exposed. The authorized owning layer controls diagnostic visibility.
struct SafeRetirementReaderRecord {
  SafeRetirementHandle object;
  MemoryBinaryUuid object_uuid{};
  SafeRetirementObjectKind object_kind = SafeRetirementObjectKind::temporary_descriptor;
  SafeRetirementHazard hazard;
  // Per object, not additive across multiple hazards protecting that object.
  usize object_bytes = 0;
  bool retirement_requested = false;
};

struct SafeRetirementReaderInspection {
  SafeRetirementStatus status = SafeRetirementStatus::invalid_request;
  usize matching_readers = 0;
  usize records_written = 0;
  bool truncated = false;
};

class MemorySafeRetirement;
class SafeRetirementGuard {
 public:
  SafeRetirementGuard() = default;
  ~SafeRetirementGuard();
  SafeRetirementGuard(const SafeRetirementGuard&) = delete;
  SafeRetirementGuard& operator=(const SafeRetirementGuard&) = delete;
  SafeRetirementGuard(SafeRetirementGuard&& other) noexcept;
  SafeRetirementGuard& operator=(SafeRetirementGuard&& other) noexcept;
  void Reset() noexcept;
  void* get() const noexcept { return pointer_; }
  explicit operator bool() const noexcept { return pointer_ != nullptr; }

 private:
  friend class MemorySafeRetirement;
  MemorySafeRetirement* domain_ = nullptr;
  usize reader_slot_ = 0;
  void* pointer_ = nullptr;
};

struct SafeRetirementCreateResult {
  SafeRetirementStatus status = SafeRetirementStatus::invalid_request;
  SafeRetirementHandle handle;
  bool ok() const { return status == SafeRetirementStatus::ok; }
};

class MemorySafeRetirement {
 public:
  // The resource/ledger/manager must outlive this domain. Its owner must not
  // explicitly Release the resource while the domain exists. Domain UUIDs are
  // fresh engine-issued incarnations, never reused on replacement.
  // Close fences creation/protection, not existing guards' release paths.
  // Before destruction: close, drain, join ALL callers (including callbacks).
  // Destruction with live readers/constructions terminates instead of freeing
  // reachable storage. Drain success alone does not join external callers.
  MemorySafeRetirement(ReservationBackedMemoryResource& resource,
                       MemoryBinaryUuid domain, usize object_limit, usize reader_limit);
  ~MemorySafeRetirement();
  MemorySafeRetirement(const MemorySafeRetirement&) = delete;
  MemorySafeRetirement& operator=(const MemorySafeRetirement&) = delete;

  SafeRetirementStatus Initialize();

  template <typename T, typename... Args>
  SafeRetirementCreateResult Emplace(MemoryBinaryUuid object,
                                    SafeRetirementObjectKind kind, Args&&... args) {
    static_assert(std::is_nothrow_destructible_v<T>);
    void* pointer = nullptr;
    auto result = BeginCreate(object, kind, sizeof(T), alignof(T), pointer);
    if (!result.ok()) return result;
    try {
      ::new (pointer) T(std::forward<Args>(args)...);
    } catch (...) {
      FinishCreate(result.handle, nullptr, false);
      result.status = SafeRetirementStatus::construction_failed;
      return result;
    }
    result.status = FinishCreate(result.handle,
        [](void* p) noexcept { static_cast<T*>(p)->~T(); }, true);
    return result;
  }

  SafeRetirementStatus Protect(const SafeRetirementHandle& handle,
                               SafeRetirementHazard hazard, SafeRetirementGuard& guard);
  SafeRetirementStatus Retire(const SafeRetirementHandle& handle);
  void Close();
  // One bounded scan; destructors run outside the domain lock, exactly once.
  // Failed physical release stays retired/charged and may be retried.
  // ok means no release error in this scan, NOT that readers have drained or
  // every retired object was freed. Drain/Snapshot distinguish those effects.
  SafeRetirementStatus Collect();
  // Deadline/cancellation bound the quiescence wait, not client destruction.
  // Object destructors must be bounded and must not block on this domain.
  SafeRetirementStatus Drain(std::chrono::steady_clock::time_point deadline,
                             std::stop_token cancellation = {});
  SafeRetirementSnapshot Snapshot() const;
  // One consistent bounded scan into caller-owned storage; no allocation,
  // callback, release, revocation or forced reclamation. Select only this task's
  // still-live hazards required by or before the supplied completed boundary
  // (operation -> task -> runtime). Invalid/uninitialized calls write nothing.
  // Selection alone does not prove a hold is overdue: the lifecycle owner must
  // establish the exact completed operation/task scope before reporting a leak.
  // An empty span still counts all matches and explicitly reports truncation.
  // Only the first records_written output elements belong to this result;
  // remaining elements are untouched and must not be emitted as current data.
  // ok means inspection succeeded, NOT that the lifecycle has drained. A zero
  // count is only an observation: fence new references and join all users at
  // the owning boundary. No caller assertion here grants recovery authority.
  SafeRetirementReaderInspection InspectRetainedReaders(
      MemoryBinaryUuid owner_task, SafeRetirementBoundary completed_boundary,
      std::span<SafeRetirementReaderRecord> output) const;

 private:
  friend class SafeRetirementGuard;
  enum class State { empty, constructing, published, retired, reclaiming };
  struct Object {
    State state = State::empty;
    MemoryBinaryUuid identity{};
    SafeRetirementObjectKind kind = SafeRetirementObjectKind::temporary_descriptor;
    u64 generation = 0;
    void* pointer = nullptr;
    usize bytes = 0;
    usize alignment = 0;
    usize readers = 0;
    void (*destroy)(void*) noexcept = nullptr;
  };
  struct Reader {
    bool active = false;
    SafeRetirementHandle object;
    SafeRetirementHazard hazard;
  };
  SafeRetirementCreateResult BeginCreate(MemoryBinaryUuid object,
      SafeRetirementObjectKind kind, usize bytes, usize alignment, void*& pointer);
  SafeRetirementStatus FinishCreate(const SafeRetirementHandle& handle,
                                    void (*destroy)(void*) noexcept, bool constructed);
  bool Matches(const SafeRetirementHandle& handle) const;
  bool Empty() const;
  bool Collectable() const;
  SafeRetirementStatus CollectImpl(bool allocation_free);
  void ReleaseReader(usize slot) noexcept;

  // Lock order: domain -> reservation resource -> physical allocator/ledger.
  // No client constructor/destructor executes under these domain operations'
  // lock. Callbacks must not synchronously Drain their own constructing or
  // reclaiming object; that operation necessarily waits for the callback.
  ReservationBackedMemoryResource& resource_;
  const MemoryBinaryUuid domain_;
  const usize object_limit_;
  const usize reader_limit_;
  mutable std::mutex mutex_;
  // Unlike condition_variable_any, this carries no hidden shared-mutex heap
  // allocation. Cancellation notification takes the same predicate mutex.
  std::condition_variable changed_;
  Object* objects_ = nullptr;
  Reader* readers_ = nullptr;
  u64 generation_ = 0;
  bool closed_ = false;
  bool initialized_ = false;
};

}  // namespace scratchbird::core::memory
