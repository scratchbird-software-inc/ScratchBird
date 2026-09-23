// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "resource_governance_admission.hpp"
#include "../support/binary_uuid_fixture.hpp"
#include <cstdlib>
namespace r = scratchbird::core::agents;
static void Check(bool value) { if (!value) std::abort(); }
int main() {
  r::HierarchicalMemoryBudgetLedger ledger("native-owner-test");
  r::HierarchicalMemoryBudgetScope scope;
  scope.scope_id="values"; scope.limit_bytes=100;
  scope.kind=r::HierarchicalMemoryBudgetScopeKind::kOperator;
  Check(ledger.RegisterScope(scope).ok);
  r::HierarchicalMemoryBudgetReserveRequest request;
  request.operation_id="update";request.leaf_scope_id="values";request.bytes=20;
  const auto first=scratchbird::tests::FixtureUuid(1026,1);
  const auto second=scratchbird::tests::FixtureUuid(1026,2);
  request.owner_uuid=first;
  const auto a=ledger.Reserve(request);
  Check(a.ok && a.reservation.owner_uuid==first && a.reservation.owner_scope.empty());
  request.owner_uuid=second;
  const auto b=ledger.Reserve(request); Check(b.ok);
  Check(!ledger.ReleaseOwnerReservations(std::string{}).released);
  Check(!ledger.ReleaseOwnerReservations(scratchbird::tests::FixtureUuid(1026,3)).released);
  Check(ledger.Snapshot().front().current_bytes==40);
  Check(ledger.ReleaseOwnerReservations(first).released);
  Check(ledger.Snapshot().front().current_bytes==20);
  Check(!ledger.ReleaseOwnerReservations(first).released);
  request.owner_scope="named-operation";
  Check(!ledger.Reserve(request).ok); // Ambiguous dual ownership is refused.
  request.owner_scope.clear(); request.owner_uuid={};
  Check(!ledger.Reserve(request).ok);
  request.owner_uuid=first; request.owner_uuid.bytes[6]=0x40;
  Check(!ledger.Reserve(request).ok);
  request.owner_uuid={}; request.owner_scope="named-operation";
  Check(ledger.Reserve(request).ok);
  Check(ledger.ReleaseOwnerReservations(second).released);
  Check(ledger.Snapshot().front().current_bytes==20);
  Check(ledger.ReleaseOwnerReservations(request.owner_scope).released);
  Check(ledger.Snapshot().front().current_bytes==0);
  Check(ledger.ReleaseNoAlloc(b.reservation.token_id)==r::HierarchicalMemoryBudgetReleaseCode::not_found);
  r::ResourceGovernanceReservationLedger quota("native-quota-owner-test");
  r::ResourceGovernanceReservationAcquireRequest acquire;
  acquire.admission.operation_id="package";
  auto& descriptor = acquire.admission.descriptor;
  descriptor.descriptor_id="package-quota";
  descriptor.family=r::ResourceGovernanceFamily::kQueryMemoryArena;
  descriptor.source=r::ResourceGovernanceDescriptorSource::kRuntimePolicy;
  descriptor.source_path_or_label="runtime-test";
  descriptor.descriptor_generation=descriptor.expected_generation=1;
  descriptor.runtime_dependency_present=descriptor.benchmark_clean=true;
  descriptor.limits={100,100,100,100,100,100,100,100,100,100,100,100,100};
  acquire.admission.requested.memory_bytes=20;
  auto native_first=first; native_first.bytes[9]=0; native_first.bytes[10]=0xff;
  acquire.owner_uuid=native_first;
  const auto q1=quota.Acquire(acquire);
  Check(q1.ok && q1.reservation.owner_uuid==native_first && q1.reservation.owner_scope.empty());
  acquire.owner_uuid=second;
  Check(quota.Acquire(acquire).ok);
  Check(quota.ReleaseOwnerReservations(std::string{}).released_count==0);
  Check(quota.ReleaseOwnerReservations(scratchbird::tests::FixtureUuid(1246,3)).released_count==0);
  Check(quota.Snapshot().active.memory_bytes==40);
  const auto cleanup=quota.ReleaseOwnerReservations(native_first);
  Check(cleanup.ok && cleanup.released_count==1 && cleanup.owner_uuid==native_first);
  Check(quota.ReleaseOwnerReservations(native_first).released_count==0);
  Check(quota.Snapshot().active.memory_bytes==20);
  acquire.owner_scope="named-owner";
  Check(!quota.Acquire(acquire).ok);
  acquire.owner_scope.clear(); acquire.owner_uuid={};
  Check(!quota.Acquire(acquire).ok);
  acquire.owner_uuid=first; acquire.owner_uuid.bytes[6]=0x40;
  Check(!quota.Acquire(acquire).ok);
  acquire.owner_uuid={}; acquire.owner_scope="named-owner";
  Check(quota.Acquire(acquire).ok);
  Check(quota.ReleaseOwnerReservations(second).released_count==1);
  Check(quota.Snapshot().active.memory_bytes==20);
  Check(quota.ReleaseOwnerReservations(acquire.owner_scope).released_count==1);
  Check(quota.Snapshot().active.memory_bytes==0);

}
