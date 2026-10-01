// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "memory_safe_retirement.hpp"

#include <exception>

namespace scratchbird::core::memory {

SafeRetirementGuard::~SafeRetirementGuard() { Reset(); }
SafeRetirementGuard::SafeRetirementGuard(SafeRetirementGuard&& other) noexcept
    : domain_(std::exchange(other.domain_, nullptr)), reader_slot_(other.reader_slot_),
      pointer_(std::exchange(other.pointer_, nullptr)) {}
SafeRetirementGuard& SafeRetirementGuard::operator=(SafeRetirementGuard&& other) noexcept {
  if (this != &other) {
    Reset();
    domain_ = std::exchange(other.domain_, nullptr);
    reader_slot_ = other.reader_slot_;
    pointer_ = std::exchange(other.pointer_, nullptr);
  }
  return *this;
}
void SafeRetirementGuard::Reset() noexcept {
  auto* domain = std::exchange(domain_, nullptr);
  pointer_ = nullptr;
  if (domain) domain->ReleaseReader(reader_slot_);
}

MemorySafeRetirement::MemorySafeRetirement(ReservationBackedMemoryResource& resource,
    MemoryBinaryUuid domain, usize object_limit, usize reader_limit)
    : resource_(resource), domain_(domain), object_limit_(object_limit), reader_limit_(reader_limit) {}

SafeRetirementStatus MemorySafeRetirement::Initialize() {
  std::lock_guard lock(mutex_);
  if (closed_) return SafeRetirementStatus::closed;
  if (initialized_) return SafeRetirementStatus::ok;
  const auto& request = resource_.request();
  if (!MemorySystemUuidValid(domain_) || !object_limit_ || !reader_limit_ ||
      object_limit_ > std::numeric_limits<usize>::max() / sizeof(Object) ||
      reader_limit_ > std::numeric_limits<usize>::max() / sizeof(Reader) ||
      request.binary_ownership.empty() || !MemorySystemUuidValid(request.binary_operation_uuid))
    return SafeRetirementStatus::invalid_request;
  // Metadata is actual governed storage, not an uncharged growing worklist.
  if (!objects_) {
    const auto allocation = resource_.Allocate(
        {object_limit_ * sizeof(Object), alignof(Object), "safe retirement objects"});
    if (!allocation.ok()) return SafeRetirementStatus::allocation_failed;
    objects_ = static_cast<Object*>(allocation.pointer);
    for (usize i = 0; i < object_limit_; ++i) ::new (objects_ + i) Object;
  }
  if (!readers_) {
    const auto allocation = resource_.Allocate(
        {reader_limit_ * sizeof(Reader), alignof(Reader), "safe retirement readers"});
    if (!allocation.ok()) return SafeRetirementStatus::allocation_failed;
    readers_ = static_cast<Reader*>(allocation.pointer);
    for (usize i = 0; i < reader_limit_; ++i) ::new (readers_ + i) Reader;
  }
  initialized_ = true;
  return SafeRetirementStatus::ok;
}

MemorySafeRetirement::~MemorySafeRetirement() {
  Close();
  if (CollectImpl(true) != SafeRetirementStatus::ok || !Empty()) std::terminate();
  if (readers_ && !resource_.DeallocateNoAlloc(readers_, reader_limit_ * sizeof(Reader),
                                             alignof(Reader)).ok()) std::terminate();
  if (objects_ && !resource_.DeallocateNoAlloc(objects_, object_limit_ * sizeof(Object),
                                             alignof(Object)).ok()) std::terminate();
}

bool MemorySafeRetirement::Matches(const SafeRetirementHandle& handle) const {
  return initialized_ && handle.domain == domain_ && handle.slot < object_limit_ &&
      handle.generation != 0 && objects_[handle.slot].state != State::empty &&
      objects_[handle.slot].generation == handle.generation;
}

SafeRetirementCreateResult MemorySafeRetirement::BeginCreate(MemoryBinaryUuid identity,
    SafeRetirementObjectKind kind, usize bytes, usize alignment, void*& pointer) {
  std::lock_guard lock(mutex_);
  SafeRetirementCreateResult result;
  if (!initialized_) { result.status = SafeRetirementStatus::not_initialized; return result; }
  if (closed_) { result.status = SafeRetirementStatus::closed; return result; }
  if (!MemorySystemUuidValid(identity) || kind < SafeRetirementObjectKind::record_version ||
      kind > SafeRetirementObjectKind::archive_stub) return result;
  usize slot = object_limit_;
  for (usize i = 0; i < object_limit_; ++i) {
    if (objects_[i].state == State::empty) { if (slot == object_limit_) slot = i; }
    else if (objects_[i].identity == identity) return result;
  }
  if (generation_ == std::numeric_limits<u64>::max()) {
    closed_ = true;
    for (usize i = 0; i < object_limit_; ++i)
      if (objects_[i].state == State::published) objects_[i].state = State::retired;
    if (!changed_.NotifyAll()) std::terminate();
    result.status = SafeRetirementStatus::exhausted;
    return result;
  }
  if (slot == object_limit_) { result.status = SafeRetirementStatus::exhausted; return result; }
  const auto allocation = resource_.Allocate({bytes, alignment, "safe retirement payload"});
  if (!allocation.ok()) { result.status = SafeRetirementStatus::allocation_failed; return result; }
  auto& object = objects_[slot];
  object = Object{};
  object.state = State::constructing;
  object.identity = identity;
  object.kind = kind;
  object.generation = ++generation_;
  object.pointer = pointer = allocation.pointer;
  object.bytes = bytes;
  object.alignment = allocation.alignment;
  result.handle = {domain_, slot, object.generation};
  result.status = SafeRetirementStatus::ok;
  return result;
}

SafeRetirementStatus MemorySafeRetirement::FinishCreate(const SafeRetirementHandle& handle,
    void (*destroy)(void*) noexcept, bool constructed) {
  std::lock_guard lock(mutex_);
  auto& object = objects_[handle.slot];  // private construction token cannot be stale
  object.destroy = destroy;
  object.state = constructed && !closed_ ? State::published : State::retired;
  if (!changed_.NotifyAll()) std::terminate();
  return closed_ ? SafeRetirementStatus::closed : SafeRetirementStatus::ok;
}

SafeRetirementStatus MemorySafeRetirement::Protect(const SafeRetirementHandle& handle,
    SafeRetirementHazard hazard, SafeRetirementGuard& guard) {
  std::lock_guard lock(mutex_);
  if (guard || !MemorySystemUuidValid(hazard.hazard_id) || !MemorySystemUuidValid(hazard.owner_task) ||
      hazard.release_required_by < SafeRetirementBoundary::operation_completion ||
      hazard.release_required_by > SafeRetirementBoundary::runtime_shutdown)
    return SafeRetirementStatus::invalid_request;
  if (!initialized_) return SafeRetirementStatus::not_initialized;
  if (closed_) return SafeRetirementStatus::closed;
  if (!Matches(handle) || objects_[handle.slot].state != State::published)
    return SafeRetirementStatus::stale_handle;
  usize slot = reader_limit_;
  for (usize i = 0; i < reader_limit_; ++i) {
    if (!readers_[i].active) { if (slot == reader_limit_) slot = i; }
    else if (readers_[i].hazard.hazard_id == hazard.hazard_id)
      return SafeRetirementStatus::invalid_request;
  }
  if (slot == reader_limit_) return SafeRetirementStatus::exhausted;
  readers_[slot] = {true, handle, hazard};
  ++objects_[handle.slot].readers;
  guard.domain_ = this;
  guard.reader_slot_ = slot;
  guard.pointer_ = objects_[handle.slot].pointer;
  return SafeRetirementStatus::ok;
}

void MemorySafeRetirement::ReleaseReader(usize slot) noexcept {
  std::lock_guard lock(mutex_);
  auto& reader = readers_[slot];
  --objects_[reader.object.slot].readers;
  reader = Reader{};
  if (!changed_.NotifyAll()) std::terminate();
}

SafeRetirementStatus MemorySafeRetirement::Retire(const SafeRetirementHandle& handle) {
  std::lock_guard lock(mutex_);
  if (!Matches(handle)) return SafeRetirementStatus::stale_handle;
  auto& object = objects_[handle.slot];
  if (object.state == State::published) object.state = State::retired;
  else if (object.state != State::retired && object.state != State::reclaiming)
    return SafeRetirementStatus::stale_handle;
  if (!changed_.NotifyAll()) std::terminate();
  return SafeRetirementStatus::ok;
}

void MemorySafeRetirement::Close() {
  std::lock_guard lock(mutex_);
  closed_ = true;
  if (objects_) for (usize i = 0; i < object_limit_; ++i)
    if (objects_[i].state == State::published) objects_[i].state = State::retired;
  if (!changed_.NotifyAll()) std::terminate();
}

SafeRetirementStatus MemorySafeRetirement::Collect() {
  return CollectImpl(false);
}

SafeRetirementStatus MemorySafeRetirement::CollectImpl(bool allocation_free) {
  std::unique_lock lock(mutex_);
  if (!objects_) return SafeRetirementStatus::ok;
  auto result = SafeRetirementStatus::ok;
  for (usize i = 0; i < object_limit_; ++i) {
    auto& object = objects_[i];
    if (object.state != State::retired || object.readers) continue;
    object.state = State::reclaiming;
    const auto destroy = std::exchange(object.destroy, nullptr);
    const auto pointer = object.pointer;
    const auto bytes = object.bytes;
    const auto alignment = object.alignment;
    lock.unlock();
    if (destroy) destroy(pointer);
    const auto released = allocation_free
        ? resource_.DeallocateNoAlloc(pointer, bytes, alignment)
        : resource_.Deallocate(pointer, bytes, alignment).status;
    lock.lock();
    if (released.ok()) object = Object{};
    else { object.state = State::retired; result = SafeRetirementStatus::release_failed; }
    if (!changed_.NotifyAll()) std::terminate();
  }
  return result;
}

bool MemorySafeRetirement::Empty() const {
  if (objects_) for (usize i = 0; i < object_limit_; ++i)
    if (objects_[i].state != State::empty) return false;
  return true;
}
bool MemorySafeRetirement::Collectable() const {
  if (objects_) for (usize i = 0; i < object_limit_; ++i)
    if (objects_[i].state == State::retired && !objects_[i].readers) return true;
  return false;
}

SafeRetirementStatus MemorySafeRetirement::Drain(std::chrono::steady_clock::time_point deadline,
    std::stop_token cancellation) {
  Close();
  // Register outside the predicate lock: an already-requested stop invokes
  // this callback inline. The callback's destructor joins an in-flight call;
  // all inner locks are destroyed before it, including on return.
  std::stop_callback wake(cancellation, [this] {
    std::lock_guard lock(mutex_);
    if (!changed_.NotifyAll()) std::terminate();
  });
  for (;;) {
    if (cancellation.stop_requested()) return SafeRetirementStatus::cancelled;
    const auto collected = Collect();
    if (collected != SafeRetirementStatus::ok) return collected;
    std::unique_lock lock(mutex_);
    if (Empty()) return SafeRetirementStatus::ok;
    try {
      while (!cancellation.stop_requested() && !Empty() && !Collectable()) {
        if (std::chrono::steady_clock::now() >= deadline)
          return SafeRetirementStatus::timed_out;
        if (!changed_.Wait(lock, deadline)) return SafeRetirementStatus::wait_failed;
      }
    } catch (...) {
      if (!lock.owns_lock()) std::terminate();
      return SafeRetirementStatus::wait_failed;
    }
  }
}

SafeRetirementSnapshot MemorySafeRetirement::Snapshot() const {
  std::lock_guard lock(mutex_);
  SafeRetirementSnapshot snapshot;
  snapshot.closed = closed_;
  snapshot.initialized = initialized_;
  if (objects_) {
    snapshot.metadata_bytes += object_limit_ * sizeof(Object);
    for (usize i = 0; i < object_limit_; ++i) {
      const auto& object = objects_[i];
      if (object.state == State::empty) continue;
      const auto count = [&](auto& gauge) {
        switch (object.state) {
          case State::empty: break;
          case State::constructing: ++gauge.constructing; break;
          case State::published: ++gauge.published; break;
          case State::retired: ++gauge.retired; break;
          case State::reclaiming: ++gauge.reclaiming; break;
        }
        gauge.retained_payload_bytes += object.bytes;
        gauge.readers += object.readers;
        if (object.state == State::retired && object.readers != 0) {
          ++gauge.reclamation_blocked_objects;
          gauge.reclamation_blocked_bytes += object.bytes;
        }
      };
      count(snapshot);
      count(snapshot.by_kind[static_cast<usize>(object.kind)]);
    }
  }
  if (readers_) snapshot.metadata_bytes += reader_limit_ * sizeof(Reader);
  return snapshot;
}

SafeRetirementReaderInspection MemorySafeRetirement::InspectRetainedReaders(
    MemoryBinaryUuid owner_task, SafeRetirementBoundary completed_boundary,
    std::span<SafeRetirementReaderRecord> output) const {
  SafeRetirementReaderInspection result;
  if (!MemorySystemUuidValid(owner_task) ||
      completed_boundary < SafeRetirementBoundary::operation_completion ||
      completed_boundary > SafeRetirementBoundary::runtime_shutdown)
    return result;
  std::lock_guard lock(mutex_);
  if (!initialized_) {
    result.status = SafeRetirementStatus::not_initialized;
    return result;
  }
  for (usize i = 0; i < reader_limit_; ++i) {
    const auto& reader = readers_[i];
    if (!reader.active || reader.hazard.owner_task != owner_task ||
        reader.hazard.release_required_by > completed_boundary)
      continue;
    ++result.matching_readers;
    if (result.records_written == output.size()) continue;
    const auto& object = objects_[reader.object.slot];
    output[result.records_written++] = {
        reader.object, object.identity, object.kind, reader.hazard,
        object.bytes, object.state == State::retired};
  }
  result.truncated = result.records_written < result.matching_readers;
  result.status = SafeRetirementStatus::ok;
  return result;
}

}  // namespace scratchbird::core::memory
