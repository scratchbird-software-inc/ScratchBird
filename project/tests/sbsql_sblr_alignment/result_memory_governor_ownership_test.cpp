// SPDX-License-Identifier: MPL-2.0
#include "result_cursor_plan_memory_governance.hpp"
#include "uuid.hpp"
#include <algorithm>
#include <array>
#include <type_traits>
#include <atomic>
#include <barrier>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <new>
#include <string_view>
#include <thread>

namespace fault {
thread_local long remaining = -1;
thread_local bool hit = false;
void Arm(long n) { remaining = n; hit = false; }
void Off() { remaining = -1; }
}
#ifndef SB_GOVERNOR_NO_ALLOC_OVERRIDE
void* operator new(std::size_t n) {
  if (fault::remaining >= 0 && fault::remaining-- == 0) {
    fault::remaining = 0; fault::hit = true; throw std::bad_alloc();
  }
  if (void* p = std::malloc(n ? n : 1)) return p;
  throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void* operator new(std::size_t n, const std::nothrow_t&) noexcept {
  try { return ::operator new(n); } catch (...) { return nullptr; }
}
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept { return ::operator new(n, std::nothrow); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
#endif

namespace {
namespace mem = scratchbird::core::memory;
using Surface = mem::ResultCursorPlanMemorySurface;
using Reason = mem::ResultCursorPlanMemoryReleaseReason;
using Code = scratchbird::core::platform::StatusCode;
using Uuid = mem::ResultCursorPlanMemoryUuid;
static_assert(sizeof(Uuid) == 16 && !std::is_constructible_v<Uuid, std::string>);
static_assert(std::is_same_v<decltype(mem::ResultCursorPlanMemoryLeaseRecord::lease_id), Uuid>);
static_assert(std::is_same_v<decltype(mem::ResultCursorPlanMemoryScope::database_id), Uuid>);
constexpr Uuid Identity(unsigned char suffix) {
  return {{1,2,3,4,5,6,0x77,8,0x89,10,11,12,13,14,15,suffix}};
}
constexpr Surface surfaces[] = {Surface::streaming_result, Surface::cursor,
  Surface::result_frame, Surface::plan_cache_entry, Surface::prepared_statement,
  Surface::descriptor_snapshot};
unsigned checks = 0, failures = 0, faults = 0;
void Check(bool good, std::string_view why) {
  ++checks;
  if (!good) { ++failures; std::cerr << "FAIL " << why << '\n'; }
}
mem::ResultCursorPlanMemoryLeaseRequest Request(
    mem::HierarchicalMemoryBudgetLedger& l, Surface surface, mem::u64 bytes = 128) {
  mem::ResultCursorPlanMemoryLeaseRequest r;
  r.ledger = &l; r.surface = surface; r.requested_bytes = bytes;
  r.scope.database_id = Identity(1);
  r.scope.session_id = Identity(2);
  r.scope.connection_id = Identity(3);
  r.scope.transaction_id = Identity(4);
  r.scope.query_id = Identity(5);
  r.scope.cursor_id = Identity(6);
  r.scope.plan_cache_key = "opaque-content-key-not-an-identity";
  r.scope.plan_cache_entry_id = Identity(10);
  r.scope.process_id = Identity(11);
  r.scope.prepared_statement_id = Identity(7);
  r.scope.descriptor_snapshot_id = Identity(8);
  r.owner_id = Identity(9);
  r.route_label = "governor-real-ledger-test";
  r.epochs = {1,2,3,4,5,6,7};
  r.provenance.source = mem::HierarchicalMemoryBudgetProvenanceSource::server_runtime_api;
  r.provenance.source_label = "governor-test-runtime-policy";
  r.lease_expires_at_ms = 42;
  return r;
}
void CheckLedger(const mem::HierarchicalMemoryBudgetLedger& l, mem::u64 expected,
                 mem::u64 owners = std::numeric_limits<mem::u64>::max()) {
  if (owners == std::numeric_limits<mem::u64>::max()) owners = expected ? 1 : 0;
  const auto s = l.Snapshot();
  Check(s.current_bytes == expected && s.reserved_bytes == 0, "ledger and governor owner bytes diverged");
  Check(s.active_allocation_count == owners && s.active_reservation_count == 0,
        "ledger and governor live owner counts diverged");
  for (const auto& p : s.scopes)
    Check(p.active_bytes == expected && p.reserved_bytes == 0, "parent owner charge diverged");
}
#ifndef SB_GOVERNOR_NO_ALLOC_OVERRIDE
void AllocationSweeps() {
  for (auto surface : surfaces) for (bool live_owner : {false, true}) {
    bool complete = false;
    for (long n = 0; n < 4096; ++n) {
      mem::HierarchicalMemoryBudgetLedger l(3,5);
      mem::ResultCursorPlanMemoryGovernor g;
      mem::ResultCursorPlanMemoryDecision seed;
      if (live_owner) {
        seed = g.Acquire(Request(l, surface, 17));
        Check(seed.ok(), "existing governor owner before allocation fault");
      }
      auto q = Request(l, surface);
      mem::ResultCursorPlanMemoryDecision r;
      bool threw = false;
      fault::Arm(n);
      try { r = g.Acquire(std::move(q)); } catch (const std::bad_alloc&) { threw = true; }
      fault::Off();
      if (fault::hit) {
        ++faults;
        Check(!threw && !r.ok() && r.status.code == Code::memory_allocation_failed,
              "governor OOM must return typed failure");
        Check(g.Snapshot().active_lease_count == (live_owner ? 1u : 0u),
              "failed acquire published an ownerless lease or consumed existing owner");
        Check(live_owner || g.Snapshot().quota_counter_count == 0,
              "failed acquire leaked empty quota map nodes");
        CheckLedger(l, live_owner ? 17 : 0);
      } else {
        Check(r.ok(), "uninjected governor acquire");
        Check(g.Snapshot().active_lease_count == (live_owner ? 2u : 1u),
              "successful acquire has no actual lease");
        CheckLedger(l, live_owner ? 145 : 128, live_owner ? 2 : 1);
        Check(g.Release(r.lease_id, Reason::close).ok(), "successful grant release");
        CheckLedger(l, live_owner ? 17 : 0);
        complete = true;
      }
      if (live_owner) Check(g.ReleaseNoAlloc(seed.lease_id).ok(), "existing owner was lost under OOM");
      CheckLedger(l, 0);
      if (complete) break;
    }
    Check(complete, "governor allocation sweep incomplete");
  }
}
void ReleaseSweeps() {
  for (auto surface : surfaces) {
    bool complete = false;
    for (long n = 0; n < 1024; ++n) {
      mem::HierarchicalMemoryBudgetLedger l(3,5);
      mem::ResultCursorPlanMemoryGovernor g;
      auto a = g.Acquire(Request(l, surface));
      Check(a.ok(), "release sweep acquire");
      bool threw = false;
      mem::ResultCursorPlanMemoryDecision r;
      fault::Arm(n);
      try { r = g.Release(a.lease_id, Reason::close); } catch (const std::bad_alloc&) { threw = true; }
      fault::Off();
      const auto owned = g.Snapshot().active_lease_count;
      CheckLedger(l, owned ? 128 : 0);
      Check(!threw, "release allocation failure escaped API");
      if (fault::hit) {
        ++faults;
        Check(!r.ok() && r.status.code == Code::memory_allocation_failed,
              "release OOM must return typed failure");
        Check(owned == 1, "failed release lost the caller-owned lease");
        Check(g.Release(a.lease_id, Reason::close).ok(), "release failure is not retryable");
        CheckLedger(l, 0);
      } else { Check(r.ok() && owned == 0, "uninjected release incomplete"); complete = true; break; }
    }
    Check(complete, "release allocation sweep incomplete");
  }
}
#endif
mem::ResultCursorPlanMemoryDecision Bulk(mem::ResultCursorPlanMemoryGovernor& g,
    unsigned mode, const mem::ResultCursorPlanMemoryLeaseRequest& q) {
  switch (mode) {
    case 0: return g.ReleaseByCursor(q.scope.cursor_id, Reason::close);
    case 1: return g.ReleaseResultFramesByCursor(q.scope.cursor_id, Reason::cancel);
    case 2: return g.ReleaseBySession(q.scope.session_id, Reason::disconnect);
    case 3: return g.ReleaseByConnection(q.scope.connection_id, Reason::disconnect);
    case 4: return g.ReleaseByQuery(q.scope.query_id, Reason::cancel);
    case 5: return g.ReleaseByTransaction(q.scope.transaction_id, Reason::rollback);
    case 6: return g.InvalidateByEpoch({2,3,4,5,6,7,8});
    case 7: return g.ShrinkPlanCache(q.scope.database_id, 128);
    case 8: return g.ForceCloseCursorUnderPressure(q.scope.cursor_id);
    default: return g.CleanupExpiredLeases(42);
  }
}
Surface BulkSurface(unsigned mode) {
  return mode == 1 ? Surface::result_frame :
      mode == 6 || mode == 7 ? Surface::plan_cache_entry : Surface::cursor;
}
#ifndef SB_GOVERNOR_NO_ALLOC_OVERRIDE
void BulkSweeps() {
  for (unsigned mode = 0; mode != 10; ++mode) {
    bool complete = false;
    for (long n = 0; n < 1024; ++n) {
      mem::HierarchicalMemoryBudgetLedger l(3,5);
      mem::ResultCursorPlanMemoryGovernor g;
      auto q = Request(l, BulkSurface(mode));
      auto first = g.Acquire(q), second = g.Acquire(q);
      Check(first.ok() && second.ok(), "bulk sweep real owners");
      bool threw = false;
      mem::ResultCursorPlanMemoryDecision r;
      fault::Arm(n);
      try { r = Bulk(g, mode, q); } catch (const std::bad_alloc&) { threw = true; }
      fault::Off();
      Check(!threw, "bulk teardown allocation failure escaped");
      const auto expected = mode == 7 ? 1u : 2u;
      if (fault::hit) {
        ++faults;
        Check(!r.ok() && r.status.code == Code::memory_allocation_failed,
              "bulk OOM did not return typed failure");
        Check(g.Snapshot().active_lease_count == 2, "bulk OOM partially consumed owners");
        CheckLedger(l, 256, 2);
        r = Bulk(g, mode, q);
        Check(r.ok() && r.released_lease_count == expected && r.released_bytes == expected * 128,
              "bulk OOM not retryable with exact effects");
      } else {
        Check(r.ok() && r.released_lease_count == expected && r.released_bytes == expected * 128,
              "bulk exact real cleanup counts");
        complete = true;
      }
      CheckLedger(l, (2 - expected) * 128, 2 - expected);
      Check(g.ReleaseBySession(q.scope.session_id, Reason::close).ok(), "bulk remaining cleanup");
      CheckLedger(l, 0);
      if (complete) break;
    }
    Check(complete, "bulk allocation sweep incomplete");
  }
}
#endif
void MixedSelection() {
  for (unsigned mode = 0; mode != 10; ++mode) {
    mem::HierarchicalMemoryBudgetLedger l(3,5);
    mem::ResultCursorPlanMemoryGovernor g;
    auto q = Request(l, BulkSurface(mode)), other = q;
    other.scope.database_id.bytes[15] ^= 0x40; other.scope.session_id.bytes[15] ^= 0x40;
    other.scope.connection_id.bytes[15] ^= 0x40; other.scope.query_id.bytes[15] ^= 0x40;
    other.scope.transaction_id.bytes[15] ^= 0x40; other.scope.cursor_id.bytes[15] ^= 0x40;
    other.epochs = {2,3,4,5,6,7,8}; other.lease_expires_at_ms = 100;
    auto first = g.Acquire(q), second = g.Acquire(q), foreign = g.Acquire(other);
    Check(first.ok() && second.ok() && foreign.ok(), "mixed-selection actual grants");
    auto r = Bulk(g, mode, q);
    const auto count = mode == 7 ? 1u : 2u;
    Check(r.ok() && r.released_lease_count == count && r.released_bytes == count * 128,
          "bulk selected the wrong number of owners");
    const auto s = g.Snapshot();
    bool found_foreign = false, found_second = false, found_first = false;
    for (const auto& lease : s.active_leases) {
      found_foreign |= lease.lease_id == foreign.lease_id;
      found_second |= lease.lease_id == second.lease_id;
      found_first |= lease.lease_id == first.lease_id;
    }
    Check(found_foreign && !found_first && found_second == (mode == 7),
          "bulk consumed unrelated owner or violated oldest-first shrink");
    const auto accounted = l.Snapshot();
    Check(accounted.current_bytes == (3 - count) * 128 &&
          accounted.active_allocation_count == 3 - count, "bulk aggregate real charges mismatch");
  }
}
void NonallocatingTerminal() {
  for (auto surface : surfaces) {
    mem::HierarchicalMemoryBudgetLedger l(3,5);
    mem::ResultCursorPlanMemoryGovernor g;
    auto a = g.Acquire(Request(l, surface));
    Check(a.ok(), "noalloc governor acquire");
    auto wrong = a.lease_id;
    wrong.bytes[15] ^= 0x40;
    bool threw = false, outcomes = false;
    fault::Arm(0);
    try {
      outcomes = !g.ReleaseNoAlloc(wrong).ok() && g.ReleaseNoAlloc(a.lease_id).ok() &&
          !g.ReleaseNoAlloc(a.lease_id).ok();
    } catch (const std::bad_alloc&) { threw = true; }
    fault::Off();
    Check(!threw && !fault::hit && outcomes, "governor live/stale/unknown cleanup allocated or lied");
    CheckLedger(l, 0);
  }
  for (bool commit : {false, true}) {
    mem::HierarchicalMemoryBudgetLedger l(3,5);
    auto q = Request(l, Surface::cursor);
    mem::HierarchicalMemoryReservationRequest request;
    request.scope_chain = {{mem::HierarchicalMemoryScopeKind::process, "noalloc-parent-test"}};
    request.provenance = q.provenance; request.requested_bytes = 128;
    auto a = l.Reserve(request);
    Check(a.ok() && (!commit || l.Commit(a.token).ok()), "noalloc parent owner");
    auto bad = a.token; ++bad.bytes;
    bool threw = false, outcomes = false;
    fault::Arm(0);
    try {
      outcomes = !l.ReleaseNoAlloc({}).ok() && !l.ReleaseNoAlloc(bad).ok() &&
          l.ReleaseNoAlloc(a.token).ok() && !l.ReleaseNoAlloc(a.token).ok();
    } catch (const std::bad_alloc&) { threw = true; }
    fault::Off();
    Check(!threw && !fault::hit && outcomes, "parent noalloc terminal accepted foreign token or allocated");
    CheckLedger(l, 0);
    mem::ShardedMemoryAccountingLedger accounting(3);
    mem::ShardedMemoryAccountingEvent event; event.bytes = 128;
    auto r = accounting.Reserve(event);
    Check(r.ok() && (!commit || accounting.Commit(r.token).ok()), "noalloc accounting owner");
    auto wrong = r.token; ++wrong.bytes;
    auto wrong_shard = r.token; wrong_shard.shard_index = 99;
    threw = false; outcomes = false;
    fault::Arm(0);
    try {
      outcomes = !accounting.ReleaseNoAlloc({}).ok() && !accounting.ReleaseNoAlloc(wrong).ok() &&
          !accounting.ReleaseNoAlloc(wrong_shard).ok() && accounting.ReleaseNoAlloc(r.token).ok() &&
          !accounting.ReleaseNoAlloc(r.token).ok();
    } catch (const std::bad_alloc&) { threw = true; }
    fault::Off();
    Check(!threw && !fault::hit && outcomes, "backend noalloc terminal accepted foreign token or allocated");
    const auto s = accounting.Snapshot();
    Check(s.current_bytes == 0 && s.reserved_bytes == 0, "backend noalloc cleanup leaked");
  }
}

void Destruction() {
  for (auto surface : surfaces) for (bool already_drained : {false, true}) {
    mem::HierarchicalMemoryBudgetLedger l(3,5);
    {
      mem::ResultCursorPlanMemoryGovernor g;
      auto r = g.Acquire(Request(l, surface));
      Check(r.ok(), "destructor grant acquire");
      CheckLedger(l, 128);
      if (already_drained) {
        const auto owner = Request(l, surface).owner_id;
        const auto result = l.CleanupOwner(owner.bytes);
        Check(result.ok() && result.cleaned_bytes == 128, "external owner drain before destructor");
      }
      fault::Arm(0);
    }
    fault::Off();
    Check(!fault::hit, "governor destructor allocated for live or stale owner");
    CheckLedger(l, 0);
  }
}
void ClosedInputs() {
  mem::HierarchicalMemoryBudgetLedger l(3,5);
  mem::ResultCursorPlanMemoryGovernor g;
  for (bool epochs : {false, true}) for (bool ledger : {false, true}) {
    auto q = Request(l, Surface::cursor);
    q.policy.require_epoch_evidence = epochs;
    q.policy.require_ledger_reservation = ledger;
    auto r = g.Acquire(q);
    Check(r.ok() == (epochs && ledger), "mandatory grant policy was silently disabled");
    if (r.ok()) Check(g.ReleaseNoAlloc(r.lease_id).ok(), "policy matrix owner cleanup");
  }
  for (int value = -1; value < 256; ++value) {
    if (value >= 0 && value < 6) continue;
    auto q = Request(l, static_cast<Surface>(value));
    auto r = g.Acquire(q);
    Check(!r.ok(), "unknown memory surface bypassed profile admission");
    if (r.ok()) (void)g.ReleaseNoAlloc(r.lease_id);
  }
  for (bool require : {false, true}) for (unsigned missing = 0; missing != 7; ++missing) {
    auto q = Request(l, Surface::cursor);
    q.policy.require_epoch_evidence = require;
    mem::u64* epochs[] = {&q.epochs.catalog_epoch, &q.epochs.security_epoch,
      &q.epochs.redaction_epoch, &q.epochs.policy_epoch, &q.epochs.resource_epoch,
      &q.epochs.descriptor_epoch, &q.epochs.memory_policy_epoch};
    *epochs[missing] = 0;
    auto r = g.Acquire(q);
    Check(!r.ok(), "optional epoch flag bypassed immutable grant authority");
    if (r.ok()) (void)g.ReleaseNoAlloc(r.lease_id);
  }
  for (int value = -1; value < 256; ++value) {
    if (value >= 0 && value < 11) continue;
    auto a = g.Acquire(Request(l, Surface::cursor));
    Check(a.ok(), "unknown release reason owner");
    auto r = g.Release(a.lease_id, static_cast<Reason>(value));
    Check(!r.ok() && g.Snapshot().active_lease_count == 1,
          "unknown release reason consumed a valid owner");
    Check(g.ReleaseNoAlloc(a.lease_id).ok(), "invalid-reason owner preservation");
  }
  CheckLedger(l, 0);
}

void BinaryIdentityAdmission() {
  using Scope = mem::ResultCursorPlanMemoryScope;
  const auto fields = std::to_array<Uuid Scope::*>({
      &Scope::process_id, &Scope::database_id, &Scope::tenant_id,
      &Scope::user_id, &Scope::role_id, &Scope::session_id,
      &Scope::connection_id, &Scope::transaction_id, &Scope::statement_id,
      &Scope::query_id, &Scope::cursor_id, &Scope::plan_cache_entry_id,
      &Scope::prepared_statement_id, &Scope::descriptor_snapshot_id});
  for (auto surface : surfaces) {
    mem::HierarchicalMemoryBudgetLedger l(3,5);
    mem::ResultCursorPlanMemoryGovernor g;
    auto base = Request(l, surface);
    for (unsigned i = 0; i != fields.size(); ++i) base.scope.*fields[i] = Identity(32 + i);
    auto same_bytes = base;
    for (auto field : fields) same_bytes.scope.*field = Identity(73);
    auto same_scope = g.Acquire(same_bytes);
    Check(same_scope.ok(), "same UUID bytes in distinct scope kinds were conflated");
    if (same_scope.ok()) {
      unsigned owned_scopes = 0;
      for (const auto& scope : l.Snapshot().scopes) {
        if (scope.active_bytes != 0) {
          ++owned_scopes;
          Check(scope.scope_id.empty() && scope.binary_scope_uuid == Identity(73).bytes && scope.active_bytes == 128,
                "equal-byte scope retained the wrong identity or charge");
        }
      }
      Check(owned_scopes == fields.size(), "equal-byte scope kind collapsed in actual ledger");
      Check(g.ReleaseNoAlloc(same_scope.lease_id).ok(), "same-byte scope cleanup");
    }
    for (auto field : fields) {
      for (unsigned value = 0; value != 21; ++value) {
        auto q = base;
        auto& id = q.scope.*field;
        bool expected = false;
        if (value < 16) {
          id.bytes[6] = static_cast<unsigned char>((value << 4) | 7);
          expected = value == 7;
        } else if (value < 20) {
          id.bytes[8] = static_cast<unsigned char>((value - 16) << 6);
          expected = value == 18;
        } else {
          id = {};
          const bool cursor_result = surface == Surface::cursor ||
              surface == Surface::result_frame || surface == Surface::streaming_result;
          expected = field != &Scope::process_id && field != &Scope::database_id &&
              field != &Scope::session_id &&
              !(cursor_result && (field == &Scope::connection_id || field == &Scope::query_id)) &&
              !((surface == Surface::cursor || surface == Surface::result_frame) && field == &Scope::cursor_id) &&
              !(surface == Surface::plan_cache_entry && field == &Scope::plan_cache_entry_id) &&
              !(surface == Surface::prepared_statement && field == &Scope::prepared_statement_id) &&
              !(surface == Surface::descriptor_snapshot && field == &Scope::descriptor_snapshot_id);
        }
        auto result = g.Acquire(q);
        Check(result.ok() == expected, "binary scope version/variant/requiredness policy");
        if (result.ok()) {
          Check(scratchbird::core::uuid::IsEngineIdentityUuid(result.lease_id),
                "lease was not issued as a system binary UUIDv7");
          const auto snapshot = g.Snapshot();
          Check(snapshot.active_leases.size() == 1 && snapshot.active_leases[0].owner_id == q.owner_id &&
                snapshot.active_leases[0].scope.*field == id,
                "actual typed owner/scope bytes not retained");
          for (const auto& scope : l.Snapshot().scopes)
            Check(scope.scope_id.empty(), "binary governor converted ledger scope to text");
          auto released = g.Release(result.lease_id, Reason::close);
          Check(released.ok() && released.released_lease_ids == std::vector<Uuid>{result.lease_id},
                "cleanup receipt omitted exact retired binary identity");
        }
        Check(g.Snapshot().active_lease_count == 0 && g.Snapshot().quota_counter_count == 0,
              "identity refusal/cleanup leaked governor state");
        CheckLedger(l, 0);
      }
    }
    for (unsigned version = 0; version != 16; ++version) {
      auto q = base;
      q.owner_id.bytes[6] = static_cast<unsigned char>((version << 4) | 7);
      const auto result = g.Acquire(q);
      Check(result.ok() == (version == 7), "reservation owner version not checked");
      if (result.ok()) Check(g.ReleaseNoAlloc(result.lease_id).ok(), "owner version cleanup");
    }
    auto q = base;
    q.owner_id = {};
    Check(!g.Acquire(q).ok(), "nil reservation owner was invented from scope labels");
    auto a = g.Acquire(base), b = g.Acquire(base);
    Check(a.ok() && b.ok() && a.lease_id != b.lease_id,
          "identical request reused a content/counter-shaped lease identity");
    const auto size = g.Snapshot().active_lease_count;
    for (const auto bad : {Uuid{}, Uuid{{1,2,3,4,5,6,0x47,8,0x89,10,11,12,13,14,15,99}}}) {
      Check(!g.Release(bad, Reason::close).ok() && !g.ReleaseNoAlloc(bad).ok() &&
            !g.ReleaseByCursor(bad, Reason::close).ok() &&
            !g.ReleaseResultFramesByCursor(bad, Reason::close).ok() &&
            !g.ReleaseBySession(bad, Reason::close).ok() &&
            !g.ReleaseByConnection(bad, Reason::close).ok() &&
            !g.ReleaseByQuery(bad, Reason::close).ok() &&
            !g.ReleaseByTransaction(bad, Reason::close).ok() &&
            !g.ShrinkPlanCache(bad, 0).ok() && !g.ForceCloseCursorUnderPressure(bad).ok(),
            "nil or malformed cleanup selector was accepted");
      Check(g.Snapshot().active_lease_count == size, "invalid selector consumed owners");
    }
    auto released = g.ReleaseBySession(base.scope.session_id, Reason::close);
    Check(released.ok() && released.released_lease_ids.size() == 2 &&
          std::find(released.released_lease_ids.begin(), released.released_lease_ids.end(), a.lease_id) != released.released_lease_ids.end() &&
          std::find(released.released_lease_ids.begin(), released.released_lease_ids.end(), b.lease_id) != released.released_lease_ids.end(),
          "bulk release did not publish exact retired identities");
    CheckLedger(l, 0);
  }
}

void BinaryScopeBudgetIsolation() {
  using Kind = mem::HierarchicalMemoryScopeKind;
  const auto kinds = {Kind::connection, Kind::cursor, Kind::plan_cache_entry,
                     Kind::prepared_statement, Kind::descriptor_snapshot};
  for (const auto limited : kinds) {
    mem::HierarchicalMemoryBudgetLedger l(3,5);
    mem::ResultCursorPlanMemoryGovernor g;
    auto q = Request(l, Surface::cursor, 64);
    q.scope.connection_id = q.scope.cursor_id = q.scope.plan_cache_entry_id =
        q.scope.prepared_statement_id = q.scope.descriptor_snapshot_id = Identity(75);
    for (auto kind : kinds) {
      mem::HierarchicalMemoryBudget budget;
      budget.scope = {kind, {}, Identity(75).bytes};
      budget.hard_limit_bytes = kind == limited ? 64 : 128;
      budget.provenance = q.provenance;
      Check(l.SetBudget(budget).ok(), "new binary scope kind rejected by actual budget activation");
    }
    auto first = g.Acquire(q);
    Check(first.ok(), "exact binary scope limit denied");
    q.requested_bytes = 1;
    auto second = g.Acquire(q);
    Check(!second.ok(), "same UUID different scope kind bypassed its own limit");
    unsigned found = 0;
    for (const auto& scope : l.Snapshot().scopes) {
      if (scope.binary_scope_uuid == Identity(75).bytes) {
        ++found;
        Check(scope.current_bytes == 64 && scope.active_bytes == 64 &&
              scope.hard_limit_bytes == (scope.kind == limited ? 64u : 128u),
              "distinct same-byte scope budgets were merged or mischarged");
      }
    }
    Check(found == kinds.size(), "missing independently governed binary scope kind");
    Check(g.ReleaseNoAlloc(first.lease_id).ok(), "binary scope quota cleanup");
    CheckLedger(l, 0);
  }
}
void OverflowPolicy() {
  constexpr auto max = std::numeric_limits<mem::u64>::max();
  for (auto surface : surfaces) {
    if (surface == Surface::result_frame) continue;
    mem::HierarchicalMemoryBudgetLedger l(3,5);
    mem::ResultCursorPlanMemoryGovernor g;
    auto q = Request(l, surface, max - 1);
    using P = mem::ResultCursorPlanMemoryPolicy;
    for (auto member : {&P::max_cursor_bytes_per_connection, &P::max_cursor_bytes_per_session,
        &P::max_cursor_bytes_per_query, &P::max_plan_cache_bytes_per_database,
        &P::max_plan_cache_bytes_per_tenant, &P::max_plan_cache_bytes_per_user,
        &P::max_plan_cache_bytes_per_session, &P::max_prepared_statement_bytes_per_database,
        &P::max_prepared_statement_bytes_per_tenant, &P::max_prepared_statement_bytes_per_user,
        &P::max_prepared_statement_bytes_per_session, &P::max_descriptor_snapshot_bytes_per_database,
        &P::max_descriptor_snapshot_bytes_per_session}) q.policy.*member = max;
    auto a = g.Acquire(q);
    Check(a.ok(), "governor near-UINT64_MAX real reservation");
    q.requested_bytes = 2;
    auto b = g.Acquire(q);
    const auto expected = surface == Surface::cursor || surface == Surface::streaming_result
        ? "SB_CEIC_020_MEMORY_GOVERNANCE.CURSOR_BYTES_LIMIT"
        : "SB_CEIC_020_MEMORY_GOVERNANCE.CACHE_BYTES_LIMIT";
    Check(!b.ok() && b.diagnostic.diagnostic_code == expected,
          "governor policy sum wrapped or depended on downstream ledger refusal");
    CheckLedger(l, max - 1);
    Check(g.ReleaseNoAlloc(a.lease_id).ok(), "governor boundary owner release");
    CheckLedger(l, 0);
  }
}
void Concurrent() {
  for (auto surface : surfaces) for (unsigned iter = 0; iter < 16; ++iter) {
    mem::HierarchicalMemoryBudgetLedger l(3,5);
    mem::ResultCursorPlanMemoryGovernor g;
    std::barrier gate(5);
    std::atomic<unsigned> admitted = 0, errors = 0;
    std::thread threads[4];
    for (auto& t : threads) t = std::thread([&] {
      auto q = Request(l, surface);
      q.policy.max_cursor_bytes_per_connection = 128;
      q.policy.max_outstanding_frames_per_connection = 1;
      q.policy.max_plan_cache_bytes_per_database = 128;
      q.policy.max_prepared_statement_bytes_per_database = 128;
      q.policy.max_descriptor_snapshot_bytes_per_database = 128;
      gate.arrive_and_wait();
      auto r = g.Acquire(std::move(q));
      if (r.ok()) ++admitted;
      gate.arrive_and_wait();
      if (r.ok() && !g.Release(r.lease_id, Reason::close).ok()) ++errors;
    });
    gate.arrive_and_wait(); gate.arrive_and_wait();
    for (auto& t : threads) t.join();
    Check(admitted == 1 && errors == 0, "governor concurrent admission exceeded actual limit");
    Check(g.Snapshot().active_lease_count == 0, "concurrent grant leaked");
    CheckLedger(l, 0);
  }
}
int Finish() {
  std::cout << checks << " checks; " << faults << " allocation faults; " << failures << " failures\n";
  return failures ? 1 : 0;
}
}
int main(int argc, char**) {
  if (argc > 1) {
    mem::HierarchicalMemoryBudgetLedger l;
    mem::ResultCursorPlanMemoryGovernor g;
    auto q = Request(l, Surface::cursor);
    q.ledger = nullptr; q.policy.require_ledger_reservation = false;
    auto r = g.Acquire(std::move(q));
    Check(!r.ok(), "null ledger policy must not authorize an unbacked grant");
    return Finish();
  }
#ifndef SB_GOVERNOR_NO_ALLOC_OVERRIDE
  AllocationSweeps(); ReleaseSweeps(); BulkSweeps();
#endif
  Destruction(); NonallocatingTerminal(); MixedSelection(); ClosedInputs(); BinaryIdentityAdmission(); BinaryScopeBudgetIsolation(); OverflowPolicy(); Concurrent();
  return Finish();
}
