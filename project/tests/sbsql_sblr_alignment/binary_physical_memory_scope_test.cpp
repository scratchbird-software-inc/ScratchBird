// SPDX-License-Identifier: MPL-2.0
#define SCRATCHBIRD_MEMORY_FAILURE_INJECTION_TEST_GUARD 1
#include "memory.hpp"
#include "sharded_memory_accounting_ledger.hpp"
#include <atomic>
#include <barrier>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <thread>
#include <type_traits>
namespace fault {
thread_local long remaining = -1;
thread_local bool hit = false;
void Tick() {
  if (remaining >= 0 && remaining-- == 0) {
    remaining = 0; hit = true; throw std::bad_alloc();
  }
}
}
#ifndef SB_BINARY_MEMORY_NO_FAULTS
void* operator new(std::size_t n) {
  fault::Tick();
  if (auto p = std::malloc(n ? n : 1)) return p;
  throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void* operator new(std::size_t n, const std::nothrow_t&) noexcept {
  try { return ::operator new(n); } catch (...) { return nullptr; }
}
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept { return ::operator new(n, std::nothrow); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { ::operator delete(p); }
void operator delete(void* p, std::size_t) noexcept { ::operator delete(p); }
void operator delete[](void* p, std::size_t) noexcept { ::operator delete(p); }
void* operator new(std::size_t n, std::align_val_t a) {
  fault::Tick(); void* p = nullptr;
  if (posix_memalign(&p, static_cast<std::size_t>(a), n ? n : 1) == 0) return p;
  throw std::bad_alloc();
}
void* operator new[](std::size_t n, std::align_val_t a) { return ::operator new(n, a); }
void operator delete(void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void* p, std::align_val_t a) noexcept { ::operator delete(p, a); }
void operator delete(void* p, std::size_t, std::align_val_t a) noexcept { ::operator delete(p, a); }
void operator delete[](void* p, std::size_t, std::align_val_t a) noexcept { ::operator delete(p, a); }
#endif
namespace {
namespace m = scratchbird::core::memory;
using Kind = m::MemoryBinaryScopeKind;
using Code = scratchbird::core::platform::StatusCode;
unsigned checks = 0, failures = 0, injected = 0;
void Check(bool ok, const char* why) {
  ++checks; if (!ok) { ++failures; std::fprintf(stderr, "FAIL %s\n", why); }
}
m::MemoryBinaryUuid Id(unsigned n) {
  m::MemoryBinaryUuid id{0x01, 0x9d};
  id[6] = 0x70; id[8] = 0x80;
  id[14] = static_cast<unsigned char>(n >> 8); id[15] = static_cast<unsigned char>(n);
  return id;
}
m::MemoryTag Tag(unsigned n, unsigned session = 100) {
  m::MemoryTag tag;
  tag.purpose = "binary scope physical allocation";
  tag.category = m::MemoryCategory::executor_query_reserved;
  tag.binary_ownership[Kind::context] = Id(n);
  tag.binary_ownership[Kind::owner] = Id(n + 1000);
  tag.binary_ownership[Kind::session] = Id(session);
  return tag;
}
m::AllocationPolicy Policy(m::u64 hard = 1024, m::u64 scope = 256) {
  m::AllocationPolicy p;
  p.hard_limit_bytes = hard; p.soft_limit_bytes = hard; p.per_context_limit_bytes = scope;
  return p;
}
m::u64 ScopeBytes(const m::MemoryAccountingSnapshot& s, Kind kind, const m::MemoryBinaryUuid& id) {
  for (const auto& scope : s.contexts) {
    if (scope.binary_scope && *scope.binary_scope == m::MemoryBinaryScopeKey{kind, id}) {
      Check(scope.scope_id.empty(), "binary identity materialized as text");
      return scope.current_bytes;
    }
  }
  return 0;
}
void LimitsAndOwnership() {
  m::MemoryManager manager(Policy());
  auto a = manager.Allocate(64, 16, Tag(1));
  auto b = manager.Allocate(128, 16, Tag(2));
  Check(a.ok() && b.ok(), "binary physical allocations refused");
  if (a.pointer) std::memset(a.pointer, 0xa5, 64);
  auto denied = manager.Allocate(80, 16, Tag(3));
  Check(!denied.ok() && denied.status.code == Code::memory_limit_exceeded, "shared session limit bypassed");
  auto s = manager.Snapshot();
  Check(s.current_bytes == 192 && s.sharded_accounting_current_bytes == 192 &&
        s.active_allocation_count == 2 && s.sharded_accounting_active_allocation_count == 2,
        "physical and sharded accounting disagree");
  Check(ScopeBytes(s, Kind::session, Id(100)) == 192 &&
        ScopeBytes(s, Kind::context, Id(1)) == 64 &&
        ScopeBytes(s, Kind::context, Id(2)) == 128, "binary owner scope attribution incorrect");
  auto changed = Tag(1); changed.binary_ownership[Kind::owner] = Id(999);
  if (a.pointer) {
    auto moved = manager.allocator()->Reallocate(a.pointer, 16, 16, changed);
    Check(!moved.ok() && manager.Snapshot().current_bytes == 192 &&
          static_cast<unsigned char*>(a.pointer)[0] == 0xa5, "reallocation changed binary owner");
    auto grown = manager.allocator()->Reallocate(a.pointer, 80, 16, Tag(1));
    Check(!grown.ok() && manager.Snapshot().current_bytes == 192,
          "reallocation ignored simultaneous old/replacement parent charge");
    Check(manager.allocator()->DeallocateNoAlloc(b.pointer).ok(), "sibling cleanup failed");
    b.pointer = nullptr;
    grown = manager.allocator()->Reallocate(a.pointer, 80, 16, Tag(1));
    Check(grown.ok() && static_cast<unsigned char*>(grown.pointer)[0] == 0xa5, "same-owner reallocation lost data");
    if (grown.ok()) a = std::move(grown);
  }
  fault::remaining = 0;
  const auto ar = manager.allocator()->DeallocateNoAlloc(a.pointer);
  const auto br = b.pointer ? manager.allocator()->DeallocateNoAlloc(b.pointer) : ar;
  fault::remaining = -1;
  Check(ar.ok() && br.ok(), "binary physical teardown allocated or failed");
  s = manager.Snapshot();
  Check(s.current_bytes == 0 && s.sharded_accounting_current_bytes == 0 &&
        ScopeBytes(s, Kind::session, Id(100)) == 0, "binary parent charge survived teardown");

  m::MemoryManager shared(Policy(1024, 0));
  auto x = shared.Allocate(700, 16, Tag(10));
  m::MemoryTag legacy; legacy.context_id = "legacy-fixture-label";
  auto y = shared.Allocate(300, 16, legacy);
  Check(x.ok() && y.ok() && !shared.Allocate(25, 16, Tag(11)).ok(),
        "binary owner received a second process budget");
  if (x.pointer) shared.allocator()->DeallocateNoAlloc(x.pointer);
  if (y.pointer) shared.allocator()->DeallocateNoAlloc(y.pointer);
}
void InvalidAndUserData() {
  m::MemoryManager manager(Policy());
  for (unsigned field = 0; field != 7; ++field) {
    auto tag = Tag(1);
    std::string* fields[] = {&tag.context_id, &tag.owner, &tag.database_id, &tag.session_id,
                            &tag.transaction_id, &tag.statement_id, &tag.query_id};
    *fields[field] = "mixed-owner";
    Check(!manager.Allocate(16, 16, std::move(tag)).ok(), "mixed legacy/binary owner admitted");
  }
  for (unsigned field = 0; field != 7; ++field) {
    auto tag = Tag(1);
    tag.binary_ownership.scopes[field] = Id(33);
    tag.binary_ownership.scopes[field][6] = 0x40;
    Check(!manager.Allocate(16, 16, tag).ok(), "non-v7 system scope admitted");
    tag.binary_ownership.scopes[field][6] = 0x70;
    tag.binary_ownership.scopes[field][8] = 0xc0;
    Check(!manager.Allocate(16, 16, tag).ok(), "invalid system UUID variant admitted");
  }
  for (auto kind : {Kind::context, Kind::owner}) {
    auto tag = Tag(1); tag.binary_ownership[kind] = {};
    Check(!manager.Allocate(16, 16, tag).ok(), "missing binary owner/context admitted");
  }
  auto payload = manager.Allocate(16, 16, Tag(1));
  auto v4 = Id(44); v4[6] = 0x40;
  Check(payload.ok(), "UUID user data storage refused");
  if (payload.pointer) {
    std::memcpy(payload.pointer, v4.data(), 16);
    Check(std::memcmp(payload.pointer, v4.data(), 16) == 0, "user UUID version altered");
    manager.allocator()->DeallocateNoAlloc(payload.pointer);
  }
  Check(manager.Snapshot().current_bytes == 0, "invalid tags changed current bytes");
}
void LedgerProjection() {
  m::ShardedMemoryAccountingLedger ledger(4);
  m::ShardedMemoryAccountingEvent a; a.bytes = 64; a.tag = Tag(1);
  a.page_buffer_bytes = true;
  auto b = a; b.bytes = 32; b.tag = Tag(2);
  auto ar = ledger.Reserve(a), br = ledger.Reserve(b);
  Check(ar.ok() && br.ok() && ledger.Commit(ar.token).ok() && ledger.Commit(br.token).ok(),
        "binary ledger actual reservation/commit refused");
  auto snapshot = ledger.SnapshotForContext(m::MemoryBinaryScopeKey{Kind::session, Id(100)});
  Check(snapshot.context_filter.empty() && snapshot.binary_context_filter.has_value() &&
        snapshot.current_bytes == 96 && snapshot.active_allocation_count == 2 &&
        snapshot.page_buffer_current_bytes == 96 && snapshot.owners.size() == 2,
        "binary filtered ledger projection omitted bytes/owners/page buffers");
  for (const auto& scope : snapshot.contexts)
    Check(scope.scope_id.empty() && scope.binary_scope.has_value(), "binary filter formatted scope as text");
  Check(ledger.SnapshotForContext(m::MemoryBinaryScopeKey{Kind::query, Id(100)}).current_bytes == 0,
        "same UUID under different scope kind aliased");
  auto bad = a; bad.scope_ids = {"legacy-overlay"};
  Check(!ledger.Reserve(bad).ok(), "binary ledger accepted legacy overlay");
  bad = a; bad.tag.owner = "legacy-owner";
  Check(!ledger.Reserve(bad).ok(), "binary ledger accepted mixed owner");
  fault::remaining = 0;
  auto released_a = ledger.ReleaseNoAlloc(ar.token), released_b = ledger.ReleaseNoAlloc(br.token);
  fault::remaining = -1;
  Check(released_a.ok() && released_b.ok() && ledger.Snapshot().current_bytes == 0,
        "binary ledger terminal charge cleanup failed");
}
void ScopedInjection() {
  m::MemoryManager manager(Policy());
  m::MemoryFailureInjectionConfiguration config;
  config.test_guard = m::MakeMemoryFailureInjectionTestGuard();
  config.fixture_enabled = true;
  config.fixture_name = "binary-physical-scope";
  m::MemoryFailureInjectionRule rule;
  rule.rule_id = "exact-binary-session";
  rule.scope_kind = m::MemoryFailureInjectionScopeKind::session;
  rule.binary_scope_uuid = Id(100);
  config.rules.push_back(rule);
  Check(manager.EnableAllocationFailureInjection(config).ok(), "binary failure scope configuration refused");
  auto unrelated = manager.Allocate(16, 16, Tag(1, 101));
  Check(unrelated.ok(), "binary failure rule matched unrelated session");
  auto refused = manager.Allocate(16, 16, Tag(2, 100));
  Check(!refused.ok() && refused.status.code == Code::memory_allocation_failed,
        "binary failure rule did not match exact session");
  Check(manager.DisableAllocationFailureInjection().ok(), "binary injection disable failed");
  auto retry = manager.Allocate(16, 16, Tag(2, 100));
  Check(retry.ok(), "binary scope injection prevented retry");
  if (unrelated.pointer) manager.allocator()->DeallocateNoAlloc(unrelated.pointer);
  if (retry.pointer) manager.allocator()->DeallocateNoAlloc(retry.pointer);
  config.rules[0].scope_id = "mixed";
  Check(!manager.EnableAllocationFailureInjection(config).ok(), "mixed binary/text injection scope admitted");
  config.rules[0].scope_id.clear();
  config.rules[0].binary_scope_uuid[6] = 0x40;
  Check(!manager.EnableAllocationFailureInjection(config).ok(), "non-v7 injection scope admitted");
  config.rules[0].binary_scope_uuid = Id(100);
  config.rules[0].scope_kind = static_cast<m::MemoryFailureInjectionScopeKind>(255);
  Check(!manager.EnableAllocationFailureInjection(config).ok(), "unknown injection scope kind admitted");
}
#ifndef SB_BINARY_MEMORY_NO_FAULTS
void AllocationFaults() {
  bool complete = false;
  for (long n = 0; n != 8192 && !complete; ++n) {
    m::MemoryManager manager(Policy());
    auto tag = Tag(1);
    m::AllocationResult result; bool escaped = false;
    fault::remaining = n; fault::hit = false;
    try { result = manager.Allocate(32, 16, std::move(tag)); }
    catch (const std::bad_alloc&) { escaped = true; }
    fault::remaining = -1;
    if (fault::hit) ++injected; else complete = true;
    Check(!escaped, "binary physical allocation failure escaped");
    auto s = manager.Snapshot();
    Check(s.current_bytes == (result.ok() ? 32u : 0u) &&
          s.sharded_accounting_current_bytes == s.current_bytes &&
          s.active_allocation_count == (result.ok() ? 1u : 0u),
          "failed binary allocation published partial charges");
    Check(ScopeBytes(s, Kind::session, Id(100)) == s.current_bytes, "binary context charge mismatch under fault");
    if (result.pointer) {
      fault::remaining = 0;
      auto released = manager.allocator()->DeallocateNoAlloc(result.pointer);
      fault::remaining = -1;
      Check(released.ok(), "binary fault-result forced cleanup failed");
    }
    s = manager.Snapshot();
    Check(s.current_bytes == 0 && s.sharded_accounting_current_bytes == 0, "binary fault-result leaked bytes");
    auto retry = manager.Allocate(32, 16, Tag(1));
    Check(retry.ok(), "failed binary allocation prevented retry");
    if (retry.pointer) manager.allocator()->DeallocateNoAlloc(retry.pointer);
  }
  Check(complete, "binary allocation fault sweep incomplete");
}
#endif
void Concurrent() {
  m::MemoryManager manager(Policy(1024, 128));
  constexpr unsigned count = 8;
  std::barrier start(count), retained(count);
  std::array<m::AllocationResult, count> allocations;
  std::vector<std::thread> threads;
  std::atomic<unsigned> winners{0}, errors{0};
  for (unsigned i = 0; i != count; ++i) threads.emplace_back([&, i] {
    auto tag = Tag(i + 1);
    start.arrive_and_wait();
    allocations[i] = manager.Allocate(32, 16, std::move(tag));
    if (allocations[i].ok()) ++winners;
    retained.arrive_and_wait();
    if (allocations[i].pointer && !manager.allocator()->DeallocateNoAlloc(allocations[i].pointer).ok()) ++errors;
  });
  for (auto& thread : threads) thread.join();
  Check(winners == 4 && errors == 0, "concurrent binary session quota did not admit exactly four");
  Check(manager.Snapshot().current_bytes == 0 && manager.Snapshot().sharded_accounting_current_bytes == 0,
        "concurrent binary teardown retained bytes");
}
}
int main() {
  static_assert(sizeof(m::MemoryBinaryUuid) == 16 && std::is_trivially_copyable_v<m::MemoryBinaryUuid>);
  LimitsAndOwnership(); InvalidAndUserData(); LedgerProjection(); ScopedInjection();
#ifndef SB_BINARY_MEMORY_NO_FAULTS
  AllocationFaults();
#endif
  Concurrent();
  std::printf("binary physical memory checks=%u allocation_faults=%u failures=%u\n", checks, injected, failures);
  return failures ? 1 : 0;
}
