// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include <cstddef>
#include <limits>
#include <memory>
#include <new>

// Test-only replacement operators. Include once in a test executable which
// already replaces ordinary new/delete. PMR's aligned allocations must pass
// through that same fault injector, on every supported platform.
inline void* ScratchBirdTestAlignedAllocate(std::size_t bytes, std::size_t alignment) {
  if (!bytes) bytes = 1;
  if (alignment > std::numeric_limits<std::size_t>::max() - sizeof(void*) ||
      bytes > std::numeric_limits<std::size_t>::max() - alignment - sizeof(void*))
    throw std::bad_alloc();
  const auto extent = bytes + alignment + sizeof(void*);
  auto* base = ::operator new(extent);
  void* result = static_cast<char*>(base) + sizeof(void*);
  auto space = extent - sizeof(void*);
  if (!std::align(alignment, bytes, result, space)) { ::operator delete(base); throw std::bad_alloc(); }
  static_cast<void**>(result)[-1] = base;
  return result;
}
void* operator new(std::size_t bytes, std::align_val_t alignment) {
  return ScratchBirdTestAlignedAllocate(bytes, static_cast<std::size_t>(alignment));
}
void* operator new[](std::size_t bytes, std::align_val_t alignment) { return ::operator new(bytes, alignment); }
void operator delete(void* p, std::align_val_t) noexcept { if (p) ::operator delete(static_cast<void**>(p)[-1]); }
void operator delete[](void* p, std::align_val_t a) noexcept { ::operator delete(p,a); }
void operator delete(void* p, std::size_t, std::align_val_t a) noexcept { ::operator delete(p,a); }
void operator delete[](void* p, std::size_t, std::align_val_t a) noexcept { ::operator delete(p,a); }
void* operator new(std::size_t n, std::align_val_t a, const std::nothrow_t&) noexcept {
  try { return ::operator new(n,a); } catch (...) { return nullptr; }
}
void* operator new[](std::size_t n, std::align_val_t a, const std::nothrow_t&) noexcept {
  try { return ::operator new(n,a); } catch (...) { return nullptr; }
}
void operator delete(void* p, std::align_val_t a, const std::nothrow_t&) noexcept { ::operator delete(p,a); }
void operator delete[](void* p, std::align_val_t a, const std::nothrow_t&) noexcept { ::operator delete(p,a); }
