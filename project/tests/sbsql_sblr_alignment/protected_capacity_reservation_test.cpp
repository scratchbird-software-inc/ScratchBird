// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "memory.hpp"
#include "../support/binary_uuid_fixture.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#include <type_traits>
#include <utility>
#if defined(_WIN32)
#include <malloc.h>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__linux__) || defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
#include <unistd.h>
#endif

#ifdef SB_PROTECTED_PAGE_QUERY_OBSERVATION
unsigned protected_page_queries = 0;
extern "C" long __real_sysconf(int);
extern "C" long __wrap_sysconf(int name) {
  if (name == _SC_PAGESIZE) ++protected_page_queries;
  return __real_sysconf(name);
}
#endif

namespace fault {
thread_local long remaining = -1;
thread_local bool hit = false;
thread_local void* watched = nullptr;
thread_local std::size_t watched_bytes = 0;
thread_local bool watched_released = false;
thread_local bool watched_zero = false;
void Arm(long count) { remaining = count; hit = false; }
void Off() { remaining = -1; }
void Admit() {
  if (remaining >= 0 && remaining-- == 0) {
    remaining = 0;
    hit = true;
    throw std::bad_alloc();
  }
}
void ObserveRelease(void* pointer) noexcept {
  if (pointer != watched || pointer == nullptr) return;
  watched_released = true;
  watched_zero = true;
  for (std::size_t i = 0; i != watched_bytes; ++i)
    watched_zero &= static_cast<const unsigned char*>(pointer)[i] == 0;
  watched = nullptr;
}
void Watch(void* pointer, std::size_t bytes) {
  watched = pointer;
  watched_bytes = bytes;
  watched_released = watched_zero = false;
}
}

// Exercise metadata AND physical allocation failures; inspect the final bytes
// at the real aligned-delete boundary, never after storage has been released.
void* operator new(std::size_t bytes) {
  fault::Admit();
  if (auto* pointer = std::malloc(bytes ? bytes : 1)) return pointer;
  throw std::bad_alloc();
}
void* operator new[](std::size_t bytes) { return ::operator new(bytes); }
void* operator new(std::size_t bytes, const std::nothrow_t&) noexcept {
  try { return ::operator new(bytes); } catch (...) { return nullptr; }
}
void* operator new[](std::size_t bytes, const std::nothrow_t&) noexcept {
  return ::operator new(bytes, std::nothrow);
}
void operator delete(void* pointer) noexcept { std::free(pointer); }
void operator delete[](void* pointer) noexcept { std::free(pointer); }
void operator delete(void* pointer, std::size_t) noexcept { std::free(pointer); }
void operator delete[](void* pointer, std::size_t) noexcept { std::free(pointer); }
void* operator new(std::size_t bytes, std::align_val_t alignment) {
  fault::Admit();
  const auto align = static_cast<std::size_t>(alignment);
  if (bytes > std::numeric_limits<std::size_t>::max() - (align - 1)) throw std::bad_alloc();
  const auto physical = ((bytes ? bytes : 1) + align - 1) & ~(align - 1);
#if defined(_WIN32)
  auto* pointer = ::_aligned_malloc(physical, align);
#else
  auto* pointer = std::aligned_alloc(align, physical);
#endif
  if (!pointer) throw std::bad_alloc();
  std::memset(pointer, 0xa5, physical);
  return pointer;
}
void* operator new[](std::size_t bytes, std::align_val_t alignment) {
  return ::operator new(bytes, alignment);
}
void* operator new(std::size_t bytes, std::align_val_t alignment, const std::nothrow_t&) noexcept {
  try { return ::operator new(bytes, alignment); } catch (...) { return nullptr; }
}
void* operator new[](std::size_t bytes, std::align_val_t alignment, const std::nothrow_t&) noexcept {
  return ::operator new(bytes, alignment, std::nothrow);
}
void operator delete(void* pointer, std::align_val_t) noexcept {
  fault::ObserveRelease(pointer);
#if defined(_WIN32)
  ::_aligned_free(pointer);
#else
  std::free(pointer);
#endif
}
void operator delete[](void* pointer, std::align_val_t alignment) noexcept {
  ::operator delete(pointer, alignment);
}
void operator delete(void* pointer, std::size_t, std::align_val_t alignment) noexcept {
  ::operator delete(pointer, alignment);
}
void operator delete[](void* pointer, std::size_t, std::align_val_t alignment) noexcept {
  ::operator delete(pointer, alignment);
}

namespace {
namespace mem = scratchbird::core::memory;
unsigned checks = 0, failures = 0;
void Check(bool good, const char* why) {
  ++checks;
  if (!good) { ++failures; std::fprintf(stderr, "FAIL: %s\n", why); }
}
mem::MemoryTag Tag() {
  mem::MemoryTag tag;
  tag.purpose = "admitted protected capacity payload";
  tag.callsite = "protected.capacity.fixture";
  // Deliberately choose values the direct protected API normalizes, to prove
  // the reserved path never changes the admitted accounting bucket or label.
  tag.category = mem::MemoryCategory::core_runtime;
  tag.lifetime = mem::MemoryLifetime::statement;
  for (unsigned i = 0; i != mem::MemoryBinaryOwnership::scope_count; ++i)
    tag.binary_ownership.scopes[i] = scratchbird::tests::FixtureUuid(2421, i + 1).bytes;
  return tag;
}
mem::AllocationPolicy Policy() {
  auto policy = mem::DefaultLocalEngineMemoryPolicy();
  policy.hard_limit_bytes = policy.soft_limit_bytes = policy.per_context_limit_bytes = 1024;
  policy.page_buffer_pool_limit_bytes = 0;
  policy.reject_over_soft_limit = true;
  policy.zero_memory_on_allocate = policy.zero_memory_on_release = false;
  return policy;
}
void Counts(mem::BoundedAllocator& allocator, mem::u64 live, mem::u64 unused,
            mem::u64 owners, mem::u64 allocations) {
  const auto snapshot = allocator.Snapshot();
  Check(snapshot.current_bytes == live && snapshot.sharded_accounting_current_bytes == live,
        "logical payload/sharded bytes differ from protected ownership");
  Check(snapshot.active_allocation_count == allocations &&
        snapshot.sharded_accounting_active_allocation_count == allocations &&
        snapshot.reserved_capacity_bytes == unused &&
        snapshot.active_capacity_reservation_count == owners,
        "capacity credits were lost/double-admitted/resurrected");
  for (const auto& context : snapshot.contexts) {
    Check(context.binary_scope.has_value() && context.scope_id.empty(),
          "binary owner became a textual identity");
    Check(context.current_bytes == live && context.reserved_capacity_bytes == unused,
          "immutable admitted binary scope not charged/refunded");
  }
}
void CheckZero(const mem::ScopedProtectedBuffer& buffer) {
  bool zero = true;
  for (std::size_t i = 0; i != buffer.size(); ++i)
    zero &= static_cast<const unsigned char*>(buffer.data())[i] == 0;
  Check(zero, "protected physical memory was not zeroed");
}
void ReleaseBlackout(mem::ScopedProtectedBuffer& buffer) {
  fault::Watch(buffer.data(), buffer.size());
  fault::Arm(0);
  const auto status = buffer.ResetNoAlloc();
  fault::Off();
  Check(status.ok() && !fault::hit && !buffer.valid() && buffer.size() == 0 &&
        buffer.alignment() == 0,
        "noalloc protected release allocated/refused/retained successful ownership");
  Check(fault::watched_released && fault::watched_zero,
        "protected release did not zero actual storage before aligned delete");
}
void SaturatedCapacity() {
  mem::BoundedAllocator allocator(Policy());
  auto tag = Tag();
  mem::MemoryLimitReduction activation;
  activation.policy_uuid = scratchbird::tests::FixtureUuid(2421, 100).bytes;
  activation.hard_limit_bytes = activation.soft_limit_bytes =
      activation.per_context_limit_bytes = 1024;
  Check(allocator.ReduceLimits(activation).ok(), "first binary policy activation");
  auto invalid_tag = Tag();
  invalid_tag.binary_ownership[mem::MemoryBinaryScopeKind::owner][6] = 0x40;
  Check(!allocator.ReserveCapacity(1, invalid_tag).ok(),
        "invalid binary system identity admitted reserved memory");
  auto capacity = allocator.ReserveCapacity(1024, tag);
  Check(capacity.ok(), "exact capacity admission");
  if (!capacity.ok()) return;
  const auto original_binding = capacity.reservation->policy_binding();
  Check(original_binding.generation == 1 && original_binding.policy_uuid == activation.policy_uuid,
        "reservation has no actual binary generation binding");
  tag.purpose = "caller mutation must not relabel an admitted owner";
  tag.binary_ownership.scopes[0] = {};
  Check(!allocator.Allocate(1, 0, Tag()).ok(), "ordinary allocation stole reserved credit");
  activation.expected_generation = 1;
  activation.policy_uuid = scratchbird::tests::FixtureUuid(2421, 101).bytes;
  activation.hard_limit_bytes = activation.soft_limit_bytes =
      activation.per_context_limit_bytes = 512;
  Check(allocator.ReduceLimits(activation).ok(), "grandfathered reduction activation");
  auto allocated = capacity.reservation->AllocateProtected(1024, 64);
  Check(allocated.ok(), "reserved protected allocation double-admitted at saturation");
  if (!allocated.ok()) return;
  Check(allocated.buffer.policy_binding() == original_binding &&
        allocated.policy_binding == original_binding &&
        allocator.PolicyBinding().generation == 2,
        "protected conversion invented/currentized its original admission binding");
  const auto expected = Tag();
  const auto& actual = allocated.buffer.tag();
  Check(actual.binary_ownership == expected.binary_ownership && actual.owner.empty() &&
        actual.database_id.empty() && actual.context_id.empty() &&
        actual.session_id.empty() && actual.transaction_id.empty() &&
        actual.statement_id.empty() && actual.query_id.empty() &&
        actual.purpose == expected.purpose && actual.callsite == expected.callsite &&
        actual.category == expected.category && actual.lifetime == expected.lifetime,
        "protected API rewrote admitted immutable owner tag");
  Check(reinterpret_cast<std::uintptr_t>(allocated.buffer.data()) % 64 == 0,
        "reserved protected allocation lost requested alignment");
  Check(allocated.buffer.alignment() >= 64,
        "protected owner lost actual allocation alignment evidence");
  Check(allocated.evidence.zero_on_allocate && allocated.evidence.zero_on_release &&
        allocated.evidence.protected_material_redacted &&
        allocated.evidence.platform_lock_attempted && allocated.evidence.no_dump_attempted &&
        !allocated.evidence.platform_name.empty() && !allocated.evidence.authority_scope.empty(),
        "real protected platform/zeroization evidence missing");
  CheckZero(allocated.buffer);
  Counts(allocator, 1024, 0, 1, 1);
  std::memset(allocated.buffer.data(), 0x69, allocated.buffer.size());
  ReleaseBlackout(allocated.buffer);
  Counts(allocator, 0, 1024, 1, 0);
  auto again = capacity.reservation->AllocateProtected(1024);
  Check(again.ok(), "released reserved protected credit not reusable");
  if (!again.ok()) return;
  CheckZero(again.buffer);
  capacity.reservation->Close();
  Counts(allocator, 1024, 0, 0, 1);
  Check(!capacity.reservation->AllocateProtected(1).ok(), "closed lease allocated protected storage");
  std::memset(again.buffer.data(), 0x71, again.buffer.size());
  Check(static_cast<unsigned char*>(again.buffer.data())[1023] == 0x71,
        "lease close freed held protected buffer");
  ReleaseBlackout(again.buffer);
  Counts(allocator, 0, 0, 0, 0);
}
void InvalidRequestsAndPlatformModes() {
  mem::BoundedAllocator allocator(Policy());
  auto capacity = allocator.ReserveCapacity(1024, Tag());
  Check(capacity.ok(), "invalid request reservation setup");
  if (!capacity.ok()) return;
  for (unsigned invalid = 0; invalid != 5; ++invalid) {
    auto result = capacity.reservation->AllocateProtected(
        invalid == 0 ? 0 : invalid == 1 ? 1025 : invalid == 2 ?
            std::numeric_limits<std::size_t>::max() : 64,
        invalid == 3 ? 3 : 0,
        invalid == 4 ? static_cast<mem::ProtectedMemoryPlatformPolicy>(255) :
            mem::ProtectedMemoryPlatformPolicy::best_effort);
    Check(!result.ok() && !result.buffer.valid() && !result.policy_binding,
          "invalid protected request published ownership/policy evidence");
    Counts(allocator, 0, 1024, 1, 0);
  }
  for (const auto mode : {mem::ProtectedMemoryPlatformPolicy::best_effort,
                          mem::ProtectedMemoryPlatformPolicy::require_platform_lock,
                          mem::ProtectedMemoryPlatformPolicy::require_no_dump,
                          mem::ProtectedMemoryPlatformPolicy::require_lock_and_no_dump}) {
    auto result = capacity.reservation->AllocateProtected(64, 64, mode);
    const bool require_lock = mode == mem::ProtectedMemoryPlatformPolicy::require_platform_lock ||
                             mode == mem::ProtectedMemoryPlatformPolicy::require_lock_and_no_dump;
    const bool require_no_dump = mode == mem::ProtectedMemoryPlatformPolicy::require_no_dump ||
                                mode == mem::ProtectedMemoryPlatformPolicy::require_lock_and_no_dump;
    const bool lock_ok = result.evidence.platform_lock_supported && result.evidence.platform_lock_succeeded;
    const bool dump_ok = result.evidence.no_dump_supported && result.evidence.no_dump_succeeded;
    Check(result.ok() == ((!require_lock || lock_ok) && (!require_no_dump || dump_ok)),
          "platform policy outcome differs from actual OS protection evidence");
    if (result.ok()) {
      Counts(allocator, 64, 960, 1, 1);
      CheckZero(result.buffer);
      std::memset(result.buffer.data(), 0xee, 64);
      ReleaseBlackout(result.buffer);
    } else {
      Check(!result.buffer.valid() && !result.policy_binding &&
            result.diagnostic.diagnostic_code == "SB-MEMORY-PROTECTED-PLATFORM-PROTECTION-REQUIRED",
            "required platform protection refusal not explicit/atomic");
    }
    Counts(allocator, 0, 1024, 1, 0);
  }
  capacity.reservation.reset();
  Counts(allocator, 0, 0, 0, 0);
}
void InjectionAndNoAllocCleanup() {
  mem::BoundedAllocator allocator(Policy());
  auto capacity = allocator.ReserveCapacity(1024, Tag());
  Check(capacity.ok(), "injection reservation setup");
  if (!capacity.ok()) return;
  mem::MemoryFailureInjectionConfiguration configuration;
  configuration.test_guard = mem::MakeMemoryFailureInjectionTestGuard();
  configuration.fixture_enabled = true;
  configuration.fixture_name = "protected-capacity-exact-owner";
  mem::MemoryFailureInjectionRule rule;
  rule.rule_id = "protected-capacity-exact-owner";
  rule.callsite = Tag().callsite;
  rule.fail_on_matched_sequence = 1;
  configuration.rules.push_back(rule);
  const auto enabled = allocator.EnableAllocationFailureInjection(configuration);
  Check(enabled.ok() && enabled.enabled, "guarded exact-callsite injection unavailable");
  if (!enabled.enabled) return;
  const auto refused = capacity.reservation->AllocateProtected(64);
  Check(!refused.ok(), "reserved protected allocation bypassed existing injection");
  Counts(allocator, 0, 1024, 1, 0);
  Check(allocator.DisableAllocationFailureInjection().ok(), "disable allocation injection");
  auto live = capacity.reservation->AllocateProtected(64);
  Check(live.ok(), "post-injection live buffer setup");
  if (!live.ok()) return;
  std::memset(live.buffer.data(), 0x68, 64);
  Check(allocator.EnableAllocationFailureInjection(std::move(configuration)).enabled,
        "release injection setup");
  const auto pointer = live.buffer.data();
  const auto denied_release = live.buffer.Reset();
  Check(!denied_release.ok() && live.buffer.valid() && live.buffer.data() == pointer &&
        live.buffer.size() == 64 && static_cast<unsigned char*>(pointer)[63] == 0x68,
        "diagnostic/injected release failure lost protected ownership/data");
  Counts(allocator, 64, 960, 1, 1);
  ReleaseBlackout(live.buffer);
  Counts(allocator, 0, 1024, 1, 0);
  allocator.DisableAllocationFailureInjection();
}
void AllocationFaultSweep() {
  bool complete = false;
  unsigned failures_seen = 0;
  for (long index = 0; index != 4096 && !complete; ++index) {
    mem::BoundedAllocator allocator(Policy());
    auto capacity = allocator.ReserveCapacity(1024, Tag());
    Check(capacity.ok(), "fault sweep reservation setup");
    if (!capacity.ok()) return;
    mem::ProtectedBufferResult result;
    bool threw = false;
    fault::Arm(index);
    try { result = capacity.reservation->AllocateProtected(137, 64); }
    catch (...) { threw = true; }
    fault::Off();
    Check(!threw, "allocation failure escaped protected reservation API");
    if (fault::hit) ++failures_seen;
    else { complete = true; Check(result.ok(), "uninjected protected allocation failed"); }
    if (result.ok()) {
      Counts(allocator, 137, 887, 1, 1);
      Check(result.policy_binding == capacity.reservation->policy_binding(),
            "fault path published non-authoritative admission binding");
      CheckZero(result.buffer);
      std::memset(result.buffer.data(), 0x9c, 137);
      ReleaseBlackout(result.buffer);
    } else Check(!result.buffer.valid() && !result.policy_binding,
                 "failed allocation published partial protected owner");
    Counts(allocator, 0, 1024, 1, 0);
    capacity.reservation.reset();
    Counts(allocator, 0, 0, 0, 0);
  }
  Check(complete && failures_seen != 0, "metadata/physical failure sweep incomplete/vacuous");
}
void MoveAndDirectCompatibility() {
  static_assert(!std::is_constructible_v<mem::ScopedProtectedBuffer,
                mem::BoundedAllocator*, void*, mem::usize, mem::usize,
                mem::MemoryTag, mem::ProtectedMemoryEvidence>);
  static_assert(!std::is_copy_constructible_v<mem::ScopedProtectedBuffer>);
  static_assert(!std::is_constructible_v<mem::ScopedProtectedBuffer,
                mem::BoundedAllocator*, void*, mem::usize, mem::usize,
                mem::MemoryTag, mem::ProtectedMemoryEvidence,
                std::optional<mem::MemoryPolicyBinding>>);
  mem::BoundedAllocator allocator(Policy());
  auto capacity = allocator.ReserveCapacity(1024, Tag());
  Check(capacity.ok(), "move owner reservation setup");
  if (!capacity.ok()) return;
  auto result = capacity.reservation->AllocateProtected(64);
  Check(result.ok(), "move owner allocation setup");
  if (!result.ok()) return;
  auto* pointer = result.buffer.data();
  mem::ScopedProtectedBuffer moved(std::move(result.buffer));
  Check(moved.data() == pointer && !result.buffer.valid() && result.buffer.size() == 0 &&
        !result.buffer.policy_binding() && moved.policy_binding() == capacity.reservation->policy_binding(),
        "move did not transfer unique protected custody/binding");
  mem::ScopedProtectedBuffer assigned;
  assigned = std::move(moved);
  Check(assigned.data() == pointer && !moved.valid() && !moved.policy_binding(),
        "move assignment did not transfer unique protected custody");
  ReleaseBlackout(assigned);
  Check(assigned.ResetNoAlloc().ok() && result.buffer.ResetNoAlloc().ok(),
        "empty/moved-from cleanup is not idempotent");
  capacity.reservation.reset();
  mem::ProtectedMemoryRequest direct;
  direct.bytes = 64;
  direct.tag = Tag();
  auto ordinary = allocator.AllocateProtected(direct);
  Check(ordinary.ok(), "legacy direct protected path no longer works");
  if (ordinary.ok()) {
    Check(ordinary.buffer.tag().category == mem::MemoryCategory::resource_seed &&
          ordinary.buffer.tag().purpose == "protected_material" &&
          ordinary.buffer.tag().callsite == "core.memory.protected.allocate" &&
          ordinary.policy_binding == allocator.PolicyBinding(),
          "direct path normalization/current policy behavior changed");
    ReleaseBlackout(ordinary.buffer);
  }
  direct.zero_on_allocate = false;
  auto nonzero = allocator.AllocateProtected(direct);
  Check(nonzero.ok(), "direct nonzero-initialized allocation compatibility");
  if (nonzero.ok()) {
    Check(!nonzero.evidence.zero_on_allocate &&
          static_cast<unsigned char*>(nonzero.buffer.data())[0] == 0xa5,
          "direct zero-on-allocate override behavior changed");
    ReleaseBlackout(nonzero.buffer);
  }
  direct.platform_policy = static_cast<mem::ProtectedMemoryPlatformPolicy>(255);
  auto invalid = allocator.AllocateProtected(direct);
  Check(!invalid.ok() && invalid.diagnostic.diagnostic_code ==
        "SB-MEMORY-PROTECTED-PLATFORM-POLICY-INVALID",
        "invalid direct platform policy silently became best-effort");
  // A stale externally released pointer is a deliberately invalid low-level
  // fixture. Failed actual noalloc release must not erase the owner's evidence.
  auto stale = allocator.AllocateProtected({64, 0, Tag()});
  Check(stale.ok(), "failed noalloc release fixture setup");
  if (stale.ok()) {
    const auto stale_pointer = stale.buffer.data();
    const auto stale_alignment = stale.buffer.alignment();
    Check(allocator.DeallocateProtectedNoAlloc(stale_pointer, stale.buffer.evidence()).ok(),
          "external low-level release fixture failed");
    const auto status = stale.buffer.ResetNoAlloc();
    Check(status.code == scratchbird::core::platform::StatusCode::memory_unknown_pointer &&
          stale.buffer.data() == stale_pointer && stale.buffer.size() == 64 &&
          stale.buffer.alignment() == stale_alignment &&
          stale.buffer.policy_binding().has_value(),
          "failed actual noalloc release silently cleared ownership");
    // Destructor retries the same unknown-pointer result without dereferencing
    // or freeing the already-released storage. No pointer is adopted or forged.
  }
  auto held_capacity = allocator.ReserveCapacity(1024, Tag());
  Check(held_capacity.ok(), "held owner teardown fixture admission");
  if (held_capacity.ok()) {
    auto held = held_capacity.reservation->AllocateProtected(17, 1);
    Check(held.ok(), "small alignment protected allocation");
    held_capacity.reservation.reset();
    if (held.ok()) {
      Counts(allocator, 17, 0, 0, 1);
      CheckZero(held.buffer);
      std::memset(held.buffer.data(), 0xa1, 17);
      ReleaseBlackout(held.buffer);
      Counts(allocator, 0, 0, 0, 0);
    }
  }
}

std::size_t ActualPageSize() {
#if defined(_WIN32)
  SYSTEM_INFO info{};
  ::GetSystemInfo(&info);
  return static_cast<std::size_t>(info.dwPageSize);
#elif defined(__linux__) || defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
  const auto value = ::sysconf(_SC_PAGESIZE);
  return value > 0 ? static_cast<std::size_t>(value) : 0;
#else
  return 0;
#endif
}

#if defined(__linux__)
struct LinuxProtection {
  bool readable = false;
  bool found = false;
  bool locked = false;
  bool no_dump = false;
};
bool HasVmFlag(const char* text, const char* flag) {
  while (*text) {
    while (*text == ' ' || *text == '\t') ++text;
    const auto* begin = text;
    while (*text && *text != ' ' && *text != '\t' && *text != '\n') ++text;
    if (text - begin == 2 && begin[0] == flag[0] && begin[1] == flag[1]) return true;
    if (*text == '\n') break;
  }
  return false;
}
LinuxProtection ReadLinuxProtection(void* pointer) {
  LinuxProtection result;
  auto* source = std::fopen("/proc/self/smaps", "r");
  if (!source) return result;
  result.readable = true;
  char line[1024];
  const auto address = reinterpret_cast<std::uintptr_t>(pointer);
  while (std::fgets(line, sizeof(line), source)) {
    unsigned long long begin = 0, end = 0;
    if (std::sscanf(line, "%llx-%llx", &begin, &end) == 2) {
      if (result.found) break;
      result.found = address >= begin && address < end;
    }
    if (result.found && std::strncmp(line, "VmFlags:", 8) == 0) {
      result.locked = HasVmFlag(line + 8, "lo");
      result.no_dump = HasVmFlag(line + 8, "dd");
    }
  }
  std::fclose(source);
  return result;
}
#endif

void SimultaneousPageProtection() {
  const auto page_size = ActualPageSize();
#if defined(_WIN32) || defined(__linux__) || defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
  Check(page_size && mem::IsPowerOfTwo(page_size), "actual platform page size unavailable/invalid");
  if (!page_size || !mem::IsPowerOfTwo(page_size)) return;
#endif
  // Both paths use the same isolation rule. Short and cross-page payloads
  // retain exact logical charging, not a claimed OS-resident-footprint value.
#ifdef SB_PROTECTED_PAGE_QUERY_OBSERVATION
  protected_page_queries = 0; // The earlier actual allocations warmed Core.
#endif
  for (unsigned path = 0; path != 2; ++path) {
    auto policy = Policy();
    const auto first_size = page_size ? page_size + 17 : 17;
    const auto second_size = std::size_t{33};
    const auto total = first_size + second_size;
    policy.hard_limit_bytes = policy.soft_limit_bytes = policy.per_context_limit_bytes = total;
    mem::BoundedAllocator allocator(policy);
    mem::MemoryCapacityReservationResult capacity;
    if (path == 0) {
      capacity = allocator.ReserveCapacity(total, Tag());
      Check(capacity.ok(), "simultaneous protected page reservation setup");
      if (!capacity.ok()) continue;
    }
    auto allocate = [&](std::size_t bytes) {
      if (path == 0) return capacity.reservation->AllocateProtected(bytes);
      mem::ProtectedMemoryRequest request;
      request.bytes = bytes;
      request.tag = Tag();
      return allocator.AllocateProtected(std::move(request));
    };
    auto first = allocate(first_size);
    auto second = allocate(second_size);
    Check(first.ok() && second.ok(), "simultaneous page-isolated protected owners failed");
    if (!first.ok() || !second.ok()) continue;
    Check(first.buffer.size() == first_size && second.buffer.size() == second_size,
          "page isolation changed logical protected payload size");
    if (page_size) {
      const auto a = reinterpret_cast<std::uintptr_t>(first.buffer.data());
      const auto b = reinterpret_cast<std::uintptr_t>(second.buffer.data());
      Check(a % page_size == 0 && b % page_size == 0 &&
            first.buffer.alignment() >= page_size && second.buffer.alignment() >= page_size,
            "protected owners have no actual stronger page-alignment evidence");
      const auto a_end = a + ((first_size + page_size - 1) & ~(page_size - 1));
      const auto b_end = b + ((second_size + page_size - 1) & ~(page_size - 1));
      Check(a_end <= b || b_end <= a, "simultaneous protected owner page spans overlap");
    }
    auto check_survivor = [&] {
#if defined(__linux__)
      const auto observed = ReadLinuxProtection(second.buffer.data());
      if (observed.readable) {
        Check(observed.found, "live protected owner absent from kernel mapping");
        if (second.evidence.platform_lock_succeeded)
          Check(observed.locked, "sibling release unlocked surviving protected page");
        if (second.evidence.no_dump_succeeded)
          Check(observed.no_dump, "sibling release re-enabled dumps for surviving protected page");
      }
#endif
    };
    check_survivor();
    std::memset(first.buffer.data(), 0x75, first_size);
    std::memset(second.buffer.data(), 0x76, second_size);
    ReleaseBlackout(first.buffer);
    check_survivor();
    Check(static_cast<unsigned char*>(second.buffer.data())[32] == 0x76,
          "sibling release changed surviving protected data");
    const auto snapshot = allocator.Snapshot();
    Check(snapshot.current_bytes == second_size && snapshot.sharded_accounting_current_bytes == second_size &&
          snapshot.reserved_capacity_bytes == (path == 0 ? first_size : 0),
          "page isolation changed exact logical payload charging/credit refund");
    ReleaseBlackout(second.buffer);
  }
#ifdef SB_PROTECTED_PAGE_QUERY_OBSERVATION
  Check(protected_page_queries == 0,
        "protected allocation/protection/release repeatedly queried invariant OS page size");
#endif
}
}

int main() {
  // Warm best-effort telemetry before deterministic fault injection sweeps.
  {
    mem::BoundedAllocator allocator(Policy());
    auto warm = allocator.Allocate(1, 0, Tag());
    Check(warm.ok(), "metric warmup");
    if (warm.ok()) allocator.DeallocateNoAlloc(warm.pointer);
  }
  SaturatedCapacity();
  InvalidRequestsAndPlatformModes();
  InjectionAndNoAllocCleanup();
  AllocationFaultSweep();
  MoveAndDirectCompatibility();
  SimultaneousPageProtection();
  std::printf("protected capacity reservation: %u checks, %u failures; owner_field_bytes=%zu; sparse_pointer_field_bytes=%zu\n",
      checks, failures, sizeof(mem::ScopedProtectedBuffer), sizeof(std::unique_ptr<mem::ScopedProtectedBuffer>));
  return failures ? 1 : 0;
}
