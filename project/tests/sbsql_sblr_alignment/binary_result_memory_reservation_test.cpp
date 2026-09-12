// SPDX-License-Identifier: MPL-2.0
// Exercise the actual parent ledger and physical allocator, not a grant mock.
#define main LegacyReservationOwnershipFixtureMain
#include "reservation_backed_resource_ownership_test.cpp"
#undef main
#include <algorithm>

namespace {
using Kind = mem::HierarchicalMemoryScopeKind;
using BinaryKind = mem::MemoryBinaryScopeKind;
mem::MemoryBinaryUuid Id(unsigned value) {
  mem::MemoryBinaryUuid uuid{};
  uuid[0] = 1; uuid[6] = 0x70; uuid[8] = 0x80;
  uuid[14] = static_cast<std::uint8_t>(value >> 8);
  uuid[15] = static_cast<std::uint8_t>(value);
  return uuid;
}
mem::ReservationBackedMemoryResourceRequest BinaryRequest(
    mem::HierarchicalMemoryBudgetLedger& ledger, mem::MemoryManager& manager) {
  auto request = Request(ledger, manager);
  request.owner_id.clear(); request.scope_chain.clear();
  request.binary_ownership[BinaryKind::context] = Id(100);
  request.binary_ownership[BinaryKind::owner] = Id(101);
  for (unsigned i = 0; i <= static_cast<unsigned>(Kind::plugin); ++i) {
    auto kind = static_cast<Kind>(i);
    request.scope_chain.push_back({kind, {}, Id(i + 1)});
    auto binary_kind = mem::HierarchicalMemoryBinaryScopeKind(kind);
    if (static_cast<unsigned>(binary_kind) >= 2 && static_cast<unsigned>(binary_kind) <= 6)
      request.binary_ownership[binary_kind] = Id(i + 1);
  }
  // Multiple same-kind scopes and identical bytes under different kinds must
  // remain separate authorities, not overwritten by a single slot or label.
  request.scope_chain.push_back({Kind::role, {}, Id(900)});
  request.scope_chain.push_back({Kind::tenant, {}, Id(900)});
  return request;
}
void ConfigureBinary(mem::HierarchicalMemoryBudgetLedger& ledger,
                     const mem::ReservationBackedMemoryResourceRequest& request,
                     mem::u64 limit = 8192) {
  for (const auto& scope : request.scope_chain)
    Check(ledger.SetBudget({scope, limit, 0, Provenance()}).ok(), "binary scope budget setup");
}
void VerifyScopes(const mem::HierarchicalMemoryBudgetLedger& ledger,
                  const mem::ReservationBackedMemoryResourceRequest& request,
                  mem::u64 bytes) {
  const auto snapshot = ledger.Snapshot();
  Check(snapshot.scopes.size() == request.scope_chain.size(), "binary scope identity was merged or invented");
  for (const auto& wanted : request.scope_chain) {
    auto found = std::find_if(snapshot.scopes.begin(), snapshot.scopes.end(), [&](const auto& scope) {
      return scope.kind == wanted.kind && scope.binary_scope_uuid == wanted.binary_scope_uuid;
    });
    Check(found != snapshot.scopes.end(), "binary scope snapshot missing");
    if (found != snapshot.scopes.end())
      Check(found->scope_id.empty() && found->active_bytes == bytes && found->current_bytes == bytes,
            "binary scope became text or parent/sharded charges disagree");
  }
}
void IdentityAndRevocation() {
  mem::HierarchicalMemoryBudgetLedger ledger(3, 5);
  mem::MemoryManager manager(Policy());
  auto request = BinaryRequest(ledger, manager);
  ConfigureBinary(ledger, request);
  auto acquired = mem::AcquireReservationBackedMemoryResource(request);
  Check(acquired.ok(), "binary reservation acquisition failed");
  if (!acquired.ok()) return;
  VerifyScopes(ledger, request, 4096);
  auto block = acquired.resource->Allocate({137, 256, "binary-live-payload"});
  Check(block.ok(), "binary grant did not allocate actual storage");
  if (!block.ok()) return;
  std::memset(block.pointer, 0xa5, 137);
  Charges(ledger, manager, 4096, 137, 1);
  const auto physical = manager.Snapshot();
  for (unsigned i = 0; i != 7; ++i) {
    const auto key = mem::MemoryBinaryScopeKey{static_cast<BinaryKind>(i), request.binary_ownership.scopes[i]};
    const auto found = std::find_if(physical.contexts.begin(), physical.contexts.end(), [&](const auto& scope) {
      return scope.binary_scope && *scope.binary_scope == key;
    });
    Check(found != physical.contexts.end() && found->scope_id.empty() && found->current_bytes == 137,
          "physical allocator lost exact binary ownership");
  }
  Check(!ledger.SetBudget({request.scope_chain.front(), 4095, 0, Provenance()}).ok(),
        "binary live budget shrink discarded a retained grant");
  Check(ledger.CleanupOwner(Id(102)).ok() && acquired.resource->active(),
        "another binary owner revoked this result");
  Check(ledger.CleanupOwner("01000000-0000-7000-8000-000000000065").ok() && acquired.resource->active(),
        "a text spelling gained binary cleanup authority");
  fault::Arm(0);
  const auto revoked = ledger.CleanupOwner(Id(101));
  const bool allocated = fault::hit;
  fault::Off();
  Check(!allocated && !revoked.ok() && revoked.retained_bytes == 4096 &&
            revoked.revoked_reservation_count == 1, "binary cleanup lost retained charges or allocated");
  Check(!acquired.resource->active(), "revoked binary grant remains usable");
  Charges(ledger, manager, 4096, 137, 1);
  VerifyScopes(ledger, request, 4096);
  Check(!acquired.resource->Allocate({1, 64, {}}).ok(), "revoked binary grant allocated new storage");
  bool intact = true;
  for (unsigned i = 0; i != 137; ++i) intact &= static_cast<unsigned char*>(block.pointer)[i] == 0xa5;
  Check(intact, "revocation freed/changed owned physical bytes");
  fault::Arm(0);
  const auto released = acquired.resource->ReleaseNoAlloc();
  const bool release_allocated = fault::hit;
  fault::Off();
  Check(released.ok() && !release_allocated, "binary owner terminal release allocated or failed");
  Charges(ledger, manager, 0, 0, 0);
  VerifyScopes(ledger, request, 0);
}
void InvalidIdentities() {
  for (unsigned mode = 0; mode != 9; ++mode) {
    mem::HierarchicalMemoryBudgetLedger ledger(3, 5);
    mem::MemoryManager manager(Policy());
    auto request = BinaryRequest(ledger, manager);
    if (mode == 0) request.owner_id = "text-owner";
    if (mode == 1) request.binary_ownership[BinaryKind::owner] = {};
    if (mode == 2) request.binary_ownership[BinaryKind::owner][6] = 0x40;
    if (mode == 3) request.scope_chain.front().scope_id = "text-process";
    if (mode == 4) request.scope_chain.front().binary_scope_uuid[8] = 0;
    if (mode == 5) request.binary_ownership[BinaryKind::session] = Id(400);
    if (mode == 6) request.scope_chain.push_back(request.scope_chain.front());
    if (mode == 7) request.scope_chain.front().kind = static_cast<Kind>(255);
    if (mode == 8) request.scope_chain.front() = {Kind::process, "text-process"};
    auto refused = mem::AcquireReservationBackedMemoryResource(std::move(request));
    Check(!refused.ok() && !refused.resource, "invalid/mixed/crossed binary identity admitted");
    Charges(ledger, manager, 0, 0, 0);
  }
}
void BinaryFaults(std::string_view mode) {
  bool complete = false;
  for (long index = 0; index != 8192 && !complete; ++index) {
    mem::HierarchicalMemoryBudgetLedger ledger(3, 5);
    mem::MemoryManager manager(Policy());
    auto request = BinaryRequest(ledger, manager);
    ConfigureBinary(ledger, request);
    mem::ReservationBackedMemoryResourceAcquireResult acquired;
    if (mode != "acquire") acquired = mem::AcquireReservationBackedMemoryResource(request);
    if (mode != "acquire") Check(acquired.ok(), "binary fault fixture acquisition");
    // A negative-control implementation may refuse the prerequisite. Report
    // that failed oracle instead of dereferencing an absent fixture owner.
    if (mode != "acquire" && !acquired.ok()) return;
    bool escaped = false, success = false;
    mem::AllocationResult allocation;
    fault::Arm(index);
    try {
      if (mode == "acquire") {
        acquired = mem::AcquireReservationBackedMemoryResource(std::move(request));
        success = acquired.ok();
      } else {
        allocation = acquired.resource->Allocate({137, 256, {}});
        success = allocation.ok();
      }
    } catch (...) { escaped = true; }
    fault::Off();
    Check(!escaped, "binary reservation allocation failure escaped");
    if (fault::hit) ++injected;
    else { complete = true; Check(success, "uninjected binary operation refused"); }
    Charges(ledger, manager, mode == "acquire" ? (success ? 4096 : 0) : 4096,
            mode == "allocate" && success ? 137 : 0, mode == "allocate" && success ? 1 : 0);
    acquired.resource.reset();
    Charges(ledger, manager, 0, 0, 0);
  }
  Check(complete, "binary ownership fault sweep did not terminate");
}
void BinaryPmr() {
  mem::HierarchicalMemoryBudgetLedger ledger(3, 5);
  mem::MemoryManager manager(Policy());
  auto request = BinaryRequest(ledger, manager);
  ConfigureBinary(ledger, request);
  auto acquired = mem::AcquireReservationBackedMemoryResource(request);
  Check(acquired.ok(), "binary PMR grant acquisition");
  if (!acquired.ok()) return;
  mem::ReservationBackedPmrMemoryResource pmr(acquired.resource.get(), "typed-result-storage");
  {
    std::pmr::vector<std::uint64_t> rows(&pmr);
    for (unsigned i = 0; i != 128; ++i) rows.push_back(i * 3);
    for (unsigned i = 0; i != 128; ++i) Check(rows[i] == i * 3, "real PMR payload changed");
    Check(manager.Snapshot().current_bytes == rows.capacity() * sizeof(std::uint64_t),
          "PMR vector capacity is not physically charged");
    const auto before = rows;
    bool refused = false;
    try { rows.reserve(4096); } catch (const std::bad_alloc&) { refused = true; }
    Check(refused && rows == before, "PMR exhaustion mutated rows or escaped its grant");
  }
  Charges(ledger, manager, 4096, 0, 0);
  acquired.resource.reset();
  Charges(ledger, manager, 0, 0, 0);
}

void BinarySharedParent() {
  mem::HierarchicalMemoryBudgetLedger ledger(3, 5);
  mem::MemoryManager manager(Policy());
  auto base = BinaryRequest(ledger, manager);
  ConfigureBinary(ledger, base, 4096);
  std::barrier ready(17), release(17);
  std::atomic<unsigned> admitted{0}, worker_failures{0};
  std::vector<std::thread> threads;
  for (unsigned i = 0; i != 16; ++i) threads.emplace_back([&, i] {
    auto request = base;
    request.requested_bytes = 1024;
    request.binary_ownership[BinaryKind::owner] = Id(200 + i);
    request.binary_ownership[BinaryKind::context] = Id(300 + i);
    auto acquired = mem::AcquireReservationBackedMemoryResource(std::move(request));
    if (acquired.ok()) {
      ++admitted;
      auto block = acquired.resource->Allocate({256, 64, {}});
      if (!block.ok()) ++worker_failures;
      else std::memset(block.pointer, static_cast<int>(i), 256);
    }
    ready.arrive_and_wait();
    release.arrive_and_wait();
  });
  ready.arrive_and_wait();
  Check(admitted == 4 && worker_failures == 0, "competing binary consumers bypassed the shared parent");
  Check(ledger.Snapshot().current_bytes == 4096 && manager.Snapshot().current_bytes == 1024,
        "concurrent binary grants or real allocations lost their charges");
  VerifyScopes(ledger, base, 4096);
  release.arrive_and_wait();
  for (auto& thread : threads) thread.join();
  Charges(ledger, manager, 0, 0, 0);
}

void BinaryLedgerAdmission() {
  mem::ShardedMemoryAccountingLedger ledger(3);
  mem::ShardedMemoryAccountingEvent event;
  event.bytes = 17;
  event.tag.binary_ownership[BinaryKind::owner] = Id(100);
  event.tag.binary_ownership[BinaryKind::context] = Id(101);
  event.binary_scope_ids = {{BinaryKind::role, Id(1)}, {BinaryKind::role, Id(2)},
                           {BinaryKind::tenant, Id(1)}, {BinaryKind::role, Id(1)}};
  auto token = ledger.Reserve(event);
  Check(token.ok() && ledger.Commit(token.token).ok(), "binary additional accounting scopes refused");
  for (const auto& scope : event.binary_scope_ids)
    Check(ledger.SnapshotForContext(scope).current_bytes == 17,
          "same-kind/different-kind scope alias or duplicate double charging");
  Check(ledger.SnapshotForContext(std::string("role:01000000-0000-7000-8000-000000000001")).current_bytes == 0,
        "binary accounting identity aliased a text label");
  Check(ledger.Release(token.token).ok(), "binary accounting scope owner release");
  for (unsigned mode = 0; mode != 4; ++mode) {
    auto invalid = event;
    if (mode == 0) invalid.binary_scope_ids[0].uuid[6] = 0x40;
    if (mode == 1) invalid.binary_scope_ids[0].kind = static_cast<BinaryKind>(255);
    if (mode == 2) invalid.tag.binary_ownership = {};
    if (mode == 3) invalid.scope_ids = {"text-scope"};
    Check(!ledger.Reserve(std::move(invalid)).ok(), "invalid/mixed additional binary scope admitted");
  }
  Check(ledger.Snapshot().current_bytes == 0, "invalid binary event published charges");
}
}
int main() {
  WarmMetrics();
  IdentityAndRevocation(); InvalidIdentities();
  BinaryFaults("acquire"); BinaryFaults("allocate"); BinaryPmr();
  BinarySharedParent(); BinaryLedgerAdmission();
  std::cout << "binary result reservation " << checks << " checks; " << injected
            << " allocation faults; " << failures << " failures\n";
  return failures ? 1 : 0;
}
