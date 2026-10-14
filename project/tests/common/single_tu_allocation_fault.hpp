// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

// Include in exactly one translation unit per test executable. These global
// allocator replacements must never be linked into a product target.
#include <cstddef>
#include <algorithm>
#include <cstdlib>
#include <new>

#ifdef _MSC_VER
#include <malloc.h>
#endif

// Isolated executable: fail each C++ allocation in the real ledger operation,
// including STL/result bookkeeping. No production fault switch is introduced.
namespace {
thread_local std::ptrdiff_t allocations_before_failure = -1;
thread_local std::size_t allocation_attempts = 0;
void AllocationPoint() {
  if (allocations_before_failure >= 0) ++allocation_attempts;
  if (allocations_before_failure == 0) throw std::bad_alloc();
  if (allocations_before_failure > 0) --allocations_before_failure;
}
}
void* operator new(std::size_t bytes) {
  AllocationPoint();
  if (void* p = std::malloc(bytes ? bytes : 1)) return p;
  throw std::bad_alloc();
}
void* operator new[](std::size_t bytes) { return ::operator new(bytes); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
void* operator new(std::size_t bytes, std::align_val_t alignment) {
  AllocationPoint();
  // PMR may request valid alignments smaller than a pointer (e.g. uint32_t).
  // posix_memalign additionally requires a pointer-size multiple. Increasing
  // that platform minimum preserves the requested alignment; refusing it
  // would inject an allocation failure even when fault injection is disabled.
  const auto platform_alignment = std::max(static_cast<std::size_t>(alignment), sizeof(void*));
#ifdef _MSC_VER
  if (void* p = ::_aligned_malloc(bytes ? bytes : 1, platform_alignment)) return p;
#else
  void* p = nullptr;
  if (::posix_memalign(&p, platform_alignment, bytes ? bytes : 1) == 0) return p;
#endif
  throw std::bad_alloc();
}
void* operator new[](std::size_t bytes, std::align_val_t alignment) {
  return ::operator new(bytes, alignment);
}
void operator delete(void* p, std::align_val_t) noexcept {
#ifdef _MSC_VER
  ::_aligned_free(p);
#else
  std::free(p);
#endif
}
void operator delete[](void* p, std::align_val_t a) noexcept { ::operator delete(p, a); }
void operator delete(void* p, std::size_t, std::align_val_t a) noexcept { ::operator delete(p, a); }
void operator delete[](void* p, std::size_t, std::align_val_t a) noexcept { ::operator delete(p, a); }
