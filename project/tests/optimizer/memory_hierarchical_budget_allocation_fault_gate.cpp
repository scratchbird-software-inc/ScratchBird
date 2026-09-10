// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#include "resource_governance_admission.hpp"
#include <cstdlib>
#include <iostream>
#include <new>
#include <string_view>
#include "../common/single_tu_allocation_fault.hpp"

namespace {
namespace agents = scratchbird::core::agents;
using Snapshots = std::vector<agents::HierarchicalMemoryBudgetScopeSnapshot>;
constexpr auto root = "database-root-long-enough-to-allocate";
constexpr auto left = "statement-left-long-enough-to-allocate";
constexpr auto right = "statement-right-long-enough-to-allocate";
constexpr auto owner = "receipt-owner-long-enough-to-allocate";
constexpr auto other = "different-owner-long-enough-to-allocate";

void Require(bool ok, std::string_view message) {
  if (!ok) { std::cerr << message << '\n'; std::exit(EXIT_FAILURE); }
}
bool Same(const Snapshots& a, const Snapshots& b) {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (a[i].scope_id != b[i].scope_id || a[i].parent_scope_id != b[i].parent_scope_id ||
        a[i].kind != b[i].kind || a[i].limit_bytes != b[i].limit_bytes ||
        a[i].current_bytes != b[i].current_bytes || a[i].peak_bytes != b[i].peak_bytes ||
        a[i].active_reservation_count != b[i].active_reservation_count) return false;
  }
  return true;
}
agents::HierarchicalMemoryBudgetReserveRequest Request(
    const char* leaf, const char* receipt, std::uint64_t bytes) {
  return {"update-operation-long-enough-to-allocate", receipt, leaf, bytes};
}
void Register(agents::HierarchicalMemoryBudgetLedger& ledger, const char* id, const char* parent) {
  Require(ledger.RegisterScope({id, parent, agents::HierarchicalMemoryBudgetScopeKind::kStatement,
                                4096, true}).ok, "fixture scope registration failed");
}
enum class Operation { reserve, release, release_owner, register_scope, quota_refusal, missing_release };

void NoAllocationRelease() {
  using Code = agents::HierarchicalMemoryBudgetReleaseCode;
  agents::HierarchicalMemoryBudgetLedger ledger("no-allocation-ledger");
  Register(ledger, root, ""); Register(ledger, left, root); Register(ledger, right, root);
  const auto a = ledger.Reserve(Request(left, owner, 64));
  const auto b = ledger.Reserve(Request(right, other, 32));
  Require(a.ok && b.ok, "no-allocation fixture reserve failed");
  const std::string missing = "nonexistent-token-long-enough-to-allocate";
  allocation_attempts = 0;
  allocations_before_failure = 0;
  const auto absent = ledger.ReleaseNoAlloc(missing);
  const auto released = ledger.ReleaseNoAlloc(a.reservation.token_id);
  const auto repeated = ledger.ReleaseNoAlloc(a.reservation.token_id);
  allocations_before_failure = -1;
  Require(allocation_attempts == 0 && absent == Code::not_found && released == Code::released &&
              repeated == Code::not_found, "scalar ledger release allocated or returned wrong disposition");
  for (const auto& s : ledger.Snapshot()) {
    Require(s.current_bytes == (s.scope_id == left ? 0 : 32), "scalar release damaged unrelated capacity");
    Require(s.peak_bytes == (s.scope_id == root ? 96 : s.scope_id == left ? 64 : 32),
            "scalar release changed peaks");
  }
  Require(ledger.Release(b.reservation.token_id).released, "unrelated token lost after scalar release");
  std::cout << "scalar ledger release/refusal/retry: zero allocation attempts\n";
}

void Sweep(Operation operation, const char* label) {
  for (std::ptrdiff_t fault = 0; fault < 2048; ++fault) {
    agents::HierarchicalMemoryBudgetLedger ledger("allocation-fault-ledger-long-identity");
    Register(ledger, root, ""); Register(ledger, left, root); Register(ledger, right, root);
    auto a = ledger.Reserve(Request(left, owner, 64));
    auto b = ledger.Reserve(Request(right, owner, 96));
    auto c = ledger.Reserve(Request(right, other, 32));
    Require(a.ok && b.ok && c.ok, "fixture reservations failed");
    const auto before = ledger.Snapshot();
    auto request = Request(left, owner, operation == Operation::quota_refusal ? 4096 : 48);
    agents::HierarchicalMemoryBudgetScope scope{
        "additional-scope-long-enough-to-allocate", root,
        agents::HierarchicalMemoryBudgetScopeKind::kOperator, 4096, true};
    const std::string missing = "nonexistent-token-long-enough-to-allocate";
    agents::HierarchicalMemoryBudgetReserveResult reserved;
    agents::HierarchicalMemoryBudgetReleaseResult released;
    agents::AgentRuntimeStatus registered;
    bool threw = false;
    allocations_before_failure = fault;
    try {
      switch (operation) {
        case Operation::reserve:
        case Operation::quota_refusal: reserved = ledger.Reserve(std::move(request)); break;
        case Operation::release: released = ledger.Release(a.reservation.token_id); break;
        case Operation::release_owner: released = ledger.ReleaseOwnerReservations(owner); break;
        case Operation::register_scope: registered = ledger.RegisterScope(std::move(scope)); break;
        case Operation::missing_release: released = ledger.Release(missing); break;
      }
    } catch (const std::bad_alloc&) { threw = true; }
    allocations_before_failure = -1;

    if (threw) {
      Require(Same(before, ledger.Snapshot()), "allocation exception changed ledger state/peaks");
      auto retry = ledger.Reserve(Request(left, owner, 48));
      Require(retry.ok && retry.reservation.created_sequence == 4,
              "allocation exception consumed sequence or prevented retry");
      Require(ledger.Release(retry.reservation.token_id).released, "retry token lost");
    } else {
      Require(fault > 0, "fault sweep did not intercept any allocation");
      if (operation == Operation::reserve) {
        Require(reserved.ok && reserved.reservation.created_sequence == 4, "successful reserve failed");
        Require(Same(reserved.snapshots, ledger.Snapshot()), "reserve result is not post-transition state");
        Require(ledger.Release(reserved.reservation.token_id).released, "new token not releasable");
      } else if (operation == Operation::quota_refusal) {
        Require(!reserved.ok && reserved.fail_closed && Same(before, ledger.Snapshot()),
                "quota refusal changed ledger");
      } else if (operation == Operation::missing_release) {
        Require(!released.ok && released.not_found && Same(before, ledger.Snapshot()),
                "missing release changed ledger");
      } else if (operation == Operation::register_scope) {
        Require(registered.ok && ledger.Snapshot().size() == before.size() + 1,
                "registration did not add exactly one scope");
      } else {
        Require(released.ok && released.released && Same(released.snapshots, ledger.Snapshot()),
                "release result is not post-transition state");
      }
    }
    const bool removed_a = !threw && (operation == Operation::release || operation == Operation::release_owner);
    const bool removed_b = !threw && operation == Operation::release_owner;
    Require(ledger.Release(a.reservation.token_id).released == !removed_a, "first token ownership changed");
    Require(ledger.Release(b.reservation.token_id).released == !removed_b, "second token ownership changed");
    Require(ledger.Release(c.reservation.token_id).released, "unrelated owner's token changed");
    Require(!ledger.ReleaseOwnerReservations(owner).released, "unexpected owner reservation survived");
    for (const auto& snapshot : ledger.Snapshot())
      Require(snapshot.current_bytes == 0 && snapshot.active_reservation_count == 0, "capacity leaked");
    if (!threw) {
      std::cout << label << ": " << fault << " allocation failures verified; retry and cleanup passed\n";
      return;
    }
  }
  Require(false, "allocation sweep never reached success");
}
}

int main() {
  NoAllocationRelease();
  Sweep(Operation::reserve, "reserve");
  Sweep(Operation::release, "release");
  Sweep(Operation::release_owner, "release_owner");
  Sweep(Operation::register_scope, "register_scope");
  Sweep(Operation::quota_refusal, "quota_refusal");
  Sweep(Operation::missing_release, "missing_release");
}
