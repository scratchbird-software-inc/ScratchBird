// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// MMCH_TYPED_ARENA_ADAPTERS
#include "memory.hpp"

#include <limits>
#include <new>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace scratchbird::core::memory {

namespace typed_arena_detail {

using scratchbird::core::platform::MakeDiagnostic;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;

inline Status OkStatus() {
  return {StatusCode::ok, Severity::info, Subsystem::memory};
}

inline Status ErrorStatus(StatusCode code) {
  return {code, Severity::error, Subsystem::memory};
}

inline constexpr const char* kAuthorityScope =
    "typed_arena_evidence_only_not_transaction_finality_visibility_authorization_recovery_parser_reference_wal_or_benchmark_authority";

inline DiagnosticRecord MakeTypedArenaDiagnostic(Status status,
                                                 std::string diagnostic_code,
                                                 std::string message_key,
                                                 const char* operation,
                                                 usize requested_bytes,
                                                 usize element_count,
                                                 std::string underlying = {}) {
  std::vector<DiagnosticArgument> arguments;
  arguments.push_back({"operation", operation});
  arguments.push_back({"requested_bytes", std::to_string(requested_bytes)});
  arguments.push_back({"element_count", std::to_string(element_count)});
  arguments.push_back({"authority_scope", kAuthorityScope});
  if (!underlying.empty()) {
    arguments.push_back({"underlying_diagnostic", std::move(underlying)});
  }
  return MakeDiagnostic(status.code,
                        status.severity,
                        status.subsystem,
                        std::move(diagnostic_code),
                        std::move(message_key),
                        std::move(arguments),
                        {},
                        "core.memory.typed_arena",
                        "Use MemoryManager-backed arenas for hot executor and planner temporary objects only.");
}

template <typename T>
void DestroyRange(void* pointer, usize count) noexcept {
  if constexpr (!std::is_trivially_destructible<T>::value) {
    T* typed = static_cast<T*>(pointer);
    for (usize i = count; i > 0; --i) {
      typed[i - 1].~T();
    }
  } else {
    (void)pointer;
    (void)count;
  }
}

struct DestructorNode {
  void (*destroy)(void*, usize) noexcept = nullptr;
  void* pointer = nullptr;
  usize count = 0;
  usize* external_count = nullptr;
  DestructorNode* next = nullptr;
};

}  // namespace typed_arena_detail

template <typename T>
struct TypedArenaAllocationResult {
  Status status;
  T* pointer = nullptr;
  usize count = 0;
  bool fail_closed = false;
  DiagnosticRecord diagnostic;

  bool ok() const {
    return status.ok() && pointer != nullptr && !fail_closed;
  }
};

class TypedArena;

template <typename T>
class TypedArenaVector {
 public:
  TypedArenaVector() = default;
  TypedArenaVector(const TypedArenaVector&) = delete;
  TypedArenaVector& operator=(const TypedArenaVector&) = delete;
  TypedArenaVector(TypedArenaVector&& other) noexcept
      : arena_(other.arena_), data_(other.data_), size_(other.size_), capacity_(other.capacity_), destructor_(other.destructor_) {
    other.arena_ = nullptr;
    other.data_ = nullptr;
    other.size_ = nullptr;
    other.capacity_ = 0;
    other.destructor_ = nullptr;
  }
  TypedArenaVector& operator=(TypedArenaVector&& other) noexcept {
    if (this != &other) {
      arena_ = other.arena_;
      data_ = other.data_;
      size_ = other.size_;
      capacity_ = other.capacity_;
      destructor_ = other.destructor_;
      other.arena_ = nullptr;
      other.data_ = nullptr;
      other.size_ = nullptr;
      other.capacity_ = 0;
      other.destructor_ = nullptr;
    }
    return *this;
  }

  T* data() const { return data_; }
  usize size() const { return size_ == nullptr ? 0 : *size_; }
  usize capacity() const { return capacity_; }
  bool valid() const { return arena_ != nullptr && data_ != nullptr && size_ != nullptr; }
  T& operator[](usize index) { return data_[index]; }
  const T& operator[](usize index) const { return data_[index]; }

  template <typename... Args>
  TypedArenaAllocationResult<T> EmplaceBack(Args&&... args);

 private:
  TypedArenaVector(TypedArena* arena, T* data, usize* size, usize capacity, typed_arena_detail::DestructorNode* destructor)
      : arena_(arena), data_(data), size_(size), capacity_(capacity), destructor_(destructor) {}

  TypedArena* arena_ = nullptr;
  T* data_ = nullptr;
  usize* size_ = nullptr;
  usize capacity_ = 0;
  typed_arena_detail::DestructorNode* destructor_ = nullptr;

  friend class TypedArena;
};

template <typename T>
struct TypedArenaVectorResult {
  Status status;
  TypedArenaVector<T> vector;
  bool fail_closed = false;
  DiagnosticRecord diagnostic;

  bool ok() const {
    return status.ok() && vector.valid() && !fail_closed;
  }
};

class TypedArena {
 public:
  explicit TypedArena(ArenaAllocator arena) : arena_(std::move(arena)) {}
  TypedArena(const TypedArena&) = delete;
  TypedArena& operator=(const TypedArena&) = delete;
  TypedArena(TypedArena&& other) noexcept
      : arena_(std::move(other.arena_)),
        destructors_(other.destructors_),
        reset_(other.reset_) {
    other.destructors_ = nullptr;
    other.reset_ = true;
  }
  TypedArena& operator=(TypedArena&& other) noexcept {
    if (this != &other) {
      Reset();
      arena_ = std::move(other.arena_);
      destructors_ = other.destructors_;
      reset_ = other.reset_;
      other.destructors_ = nullptr;
      other.reset_ = true;
    }
    return *this;
  }
  ~TypedArena() {
    (void)Reset();
  }

  template <typename T, typename... Args>
  TypedArenaAllocationResult<T> Make(Args&&... args) {
    auto storage = AllocateStorage<T>(1, "make");
    if (!storage.ok()) {
      return storage;
    }

    T* object = storage.pointer;
    try {
      ::new (static_cast<void*>(object)) T(std::forward<Args>(args)...);
    } catch (...) {
      storage.status = typed_arena_detail::ErrorStatus(typed_arena_detail::StatusCode::memory_allocation_failed);
      storage.fail_closed = true;
      storage.pointer = nullptr;
      storage.diagnostic = typed_arena_detail::MakeTypedArenaDiagnostic(
          storage.status,
          "SB-TYPED-ARENA-CONSTRUCT-FAILED",
          "memory.typed_arena.construct_failed",
          "make",
          sizeof(T),
          1);
      return storage;
    }

    auto registered = RegisterDestructor<T>(object, 1, "make");
    if (!registered.ok()) {
      object->~T();
      registered.pointer = nullptr;
      return registered;
    }
    return storage;
  }

  template <typename T>
  TypedArenaAllocationResult<T> MakeArray(usize count) {
    auto storage = AllocateStorage<T>(count, "array");
    if (!storage.ok()) {
      return storage;
    }

    T* data = storage.pointer;
    usize constructed = 0;
    try {
      for (; constructed < count; ++constructed) {
        ::new (static_cast<void*>(data + constructed)) T();
      }
    } catch (...) {
      typed_arena_detail::DestroyRange<T>(data, constructed);
      storage.status = typed_arena_detail::ErrorStatus(typed_arena_detail::StatusCode::memory_allocation_failed);
      storage.fail_closed = true;
      storage.pointer = nullptr;
      storage.count = 0;
      storage.diagnostic = typed_arena_detail::MakeTypedArenaDiagnostic(
          storage.status,
          "SB-TYPED-ARENA-ARRAY-CONSTRUCT-FAILED",
          "memory.typed_arena.array_construct_failed",
          "array",
          sizeof(T) * count,
          count);
      return storage;
    }

    auto registered = RegisterDestructor<T>(data, count, "array");
    if (!registered.ok()) {
      typed_arena_detail::DestroyRange<T>(data, count);
      registered.pointer = nullptr;
      registered.count = 0;
      return registered;
    }
    return storage;
  }

  template <typename T>
  TypedArenaVectorResult<T> MakeVector(usize capacity) {
    TypedArenaVectorResult<T> result;
    if (capacity == 0) {
      result.status = typed_arena_detail::ErrorStatus(typed_arena_detail::StatusCode::memory_invalid_request);
      result.fail_closed = true;
      result.diagnostic = typed_arena_detail::MakeTypedArenaDiagnostic(
          result.status,
          "SB-TYPED-ARENA-VECTOR-ZERO-CAPACITY",
          "memory.typed_arena.vector_zero_capacity",
          "vector",
          0,
          capacity);
      return result;
    }

    auto storage = AllocateStorage<T>(capacity, "vector");
    result.status = storage.status;
    result.diagnostic = storage.diagnostic;
    result.fail_closed = storage.fail_closed;
    if (!storage.ok()) {
      return result;
    }

    auto size_storage = AllocateStorage<usize>(1, "vector_size");
    if (!size_storage.ok()) {
      result.status = size_storage.status;
      result.diagnostic = size_storage.diagnostic;
      result.fail_closed = true;
      return result;
    }
    *size_storage.pointer = 0;

    auto node = AllocateDestructorNode("vector");
    if (node == nullptr) {
      result.status = typed_arena_detail::ErrorStatus(typed_arena_detail::StatusCode::memory_allocation_failed);
      result.fail_closed = true;
      result.diagnostic = typed_arena_detail::MakeTypedArenaDiagnostic(
          result.status,
          "SB-TYPED-ARENA-DESTRUCTOR-REGISTRATION-FAILED",
          "memory.typed_arena.destructor_registration_failed",
          "vector",
          sizeof(T) * capacity,
          capacity);
      return result;
    }
    node->destroy = &typed_arena_detail::DestroyRange<T>;
    node->pointer = storage.pointer;
    node->count = 0;
    node->external_count = size_storage.pointer;
    node->next = destructors_;
    destructors_ = node;

    result.status = typed_arena_detail::OkStatus();
    result.vector = TypedArenaVector<T>(this, storage.pointer, size_storage.pointer, capacity, node);
    return result;
  }

  DeallocationResult Reset() {
    if (!reset_) {
      for (typed_arena_detail::DestructorNode* node = destructors_; node != nullptr; node = node->next) {
        const usize count = node->external_count == nullptr ? node->count : *node->external_count;
        node->destroy(node->pointer, count);
        node->count = 0;
        if (node->external_count != nullptr) {
          *node->external_count = 0;
        }
      }
      destructors_ = nullptr;
      reset_ = true;
    }
    return arena_.Reset();
  }

  MemoryAccountingSnapshot Snapshot() const {
    return arena_.Snapshot();
  }

 private:
  template <typename T>
  TypedArenaAllocationResult<T> AllocateStorage(usize count, const char* operation) {
    TypedArenaAllocationResult<T> result;
    if (reset_) {
      result.status = typed_arena_detail::ErrorStatus(typed_arena_detail::StatusCode::memory_invalid_request);
      result.fail_closed = true;
      result.diagnostic = typed_arena_detail::MakeTypedArenaDiagnostic(
          result.status,
          "SB-TYPED-ARENA-RESET",
          "memory.typed_arena.reset",
          operation,
          0,
          count);
      return result;
    }
    if (count == 0) {
      result.status = typed_arena_detail::ErrorStatus(typed_arena_detail::StatusCode::memory_invalid_request);
      result.fail_closed = true;
      result.diagnostic = typed_arena_detail::MakeTypedArenaDiagnostic(
          result.status,
          "SB-TYPED-ARENA-ZERO-COUNT",
          "memory.typed_arena.zero_count",
          operation,
          0,
          count);
      return result;
    }
    if (count > std::numeric_limits<usize>::max() / sizeof(T)) {
      result.status = typed_arena_detail::ErrorStatus(typed_arena_detail::StatusCode::memory_limit_exceeded);
      result.fail_closed = true;
      result.diagnostic = typed_arena_detail::MakeTypedArenaDiagnostic(
          result.status,
          "SB-TYPED-ARENA-SIZE-OVERFLOW",
          "memory.typed_arena.size_overflow",
          operation,
          0,
          count);
      return result;
    }

    const usize bytes = sizeof(T) * count;
    AllocationResult allocation = arena_.Allocate(bytes, alignof(T));
    result.status = allocation.status;
    result.diagnostic = allocation.diagnostic;
    if (!allocation.ok()) {
      result.fail_closed = true;
      return result;
    }
    result.pointer = static_cast<T*>(allocation.pointer);
    result.count = count;
    return result;
  }

  typed_arena_detail::DestructorNode* AllocateDestructorNode(const char* operation) {
    AllocationResult allocation = arena_.Allocate(sizeof(typed_arena_detail::DestructorNode),
                                                  alignof(typed_arena_detail::DestructorNode));
    if (!allocation.ok()) {
      (void)operation;
      return nullptr;
    }
    return ::new (allocation.pointer) typed_arena_detail::DestructorNode();
  }

  template <typename T>
  TypedArenaAllocationResult<T> RegisterDestructor(T* pointer, usize count, const char* operation) {
    TypedArenaAllocationResult<T> result;
    result.status = typed_arena_detail::OkStatus();
    result.pointer = pointer;
    result.count = count;
    if constexpr (std::is_trivially_destructible<T>::value) {
      return result;
    }

    typed_arena_detail::DestructorNode* node = AllocateDestructorNode(operation);
    if (node == nullptr) {
      result.status = typed_arena_detail::ErrorStatus(typed_arena_detail::StatusCode::memory_allocation_failed);
      result.fail_closed = true;
      result.diagnostic = typed_arena_detail::MakeTypedArenaDiagnostic(
          result.status,
          "SB-TYPED-ARENA-DESTRUCTOR-REGISTRATION-FAILED",
          "memory.typed_arena.destructor_registration_failed",
          operation,
          sizeof(T) * count,
          count);
      return result;
    }
    node->destroy = &typed_arena_detail::DestroyRange<T>;
    node->pointer = pointer;
    node->count = count;
    node->next = destructors_;
    destructors_ = node;
    return result;
  }

  ArenaAllocator arena_;
  typed_arena_detail::DestructorNode* destructors_ = nullptr;
  bool reset_ = false;

  template <typename T>
  friend class TypedArenaVector;
};

template <typename T>
template <typename... Args>
TypedArenaAllocationResult<T> TypedArenaVector<T>::EmplaceBack(Args&&... args) {
  TypedArenaAllocationResult<T> result;
  if (!valid()) {
    result.status = typed_arena_detail::ErrorStatus(typed_arena_detail::StatusCode::memory_invalid_request);
    result.fail_closed = true;
    result.diagnostic = typed_arena_detail::MakeTypedArenaDiagnostic(
        result.status,
        "SB-TYPED-ARENA-VECTOR-INVALID",
        "memory.typed_arena.vector_invalid",
        "vector.emplace_back",
        0,
        0);
    return result;
  }
  if (*size_ >= capacity_) {
    result.status = typed_arena_detail::ErrorStatus(typed_arena_detail::StatusCode::memory_limit_exceeded);
    result.fail_closed = true;
    result.diagnostic = typed_arena_detail::MakeTypedArenaDiagnostic(
        result.status,
        "SB-TYPED-ARENA-VECTOR-CAPACITY-EXCEEDED",
        "memory.typed_arena.vector_capacity_exceeded",
        "vector.emplace_back",
        sizeof(T),
        *size_ + 1);
    return result;
  }

  T* slot = data_ + *size_;
  try {
    ::new (static_cast<void*>(slot)) T(std::forward<Args>(args)...);
  } catch (...) {
    result.status = typed_arena_detail::ErrorStatus(typed_arena_detail::StatusCode::memory_allocation_failed);
    result.fail_closed = true;
    result.diagnostic = typed_arena_detail::MakeTypedArenaDiagnostic(
        result.status,
        "SB-TYPED-ARENA-VECTOR-CONSTRUCT-FAILED",
        "memory.typed_arena.vector_construct_failed",
        "vector.emplace_back",
        sizeof(T),
        *size_ + 1);
    return result;
  }
  ++(*size_);
  if (destructor_ != nullptr) {
    destructor_->count = *size_;
  }
  result.status = typed_arena_detail::OkStatus();
  result.pointer = slot;
  result.count = 1;
  return result;
}

}  // namespace scratchbird::core::memory
