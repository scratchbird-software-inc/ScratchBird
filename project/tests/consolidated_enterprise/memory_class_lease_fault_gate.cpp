// SPDX-License-Identifier: MPL-2.0
#define main HistoricalClassLeaseMain
#include "ceic_026_memory_class_policy_lease_gate.cpp"
#undef main
#include <new>

namespace { long allocation_boundary=-1; bool allocation_failed=false; }
void* operator new(std::size_t size) {
  if(allocation_boundary==0) { allocation_failed=true; throw std::bad_alloc(); }
  if(allocation_boundary>0) --allocation_boundary;
  if(auto* p=std::malloc(size ? size : 1)) return p;
  throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p,std::size_t) noexcept { std::free(p); }
void operator delete[](void* p,std::size_t) noexcept { std::free(p); }

namespace {
void CleanupFailureBoundaries() {
  using Code = scratchbird::core::platform::StatusCode;
  for (unsigned mode = 0; mode != 3; ++mode) {
    bool completed = false;
    bool partial = false;
    for (long boundary = 0; boundary != 2048 && !completed; ++boundary) {
      memory::HierarchicalMemoryBudgetLedger ledger;
      memory::MemoryClassPolicyLeaseManager manager(&ledger);
      std::array<memory::MemoryBudgetLeaseToken, 3> tokens;
      for (auto& token : tokens) {
        const auto acquired = manager.AcquireLease(Request(
            memory::MemoryClassKind::query_scratch, 4096, "cleanup-fault-owner"));
        Require(acquired.ok(), "cleanup fault setup");
        token = acquired.lease;
      }
      auto other_request = Request(memory::MemoryClassKind::query_scratch,
                                   1024, "unrelated-owner");
      other_request.deadline_ms = 9000;
      const auto other = manager.AcquireLease(std::move(other_request));
      Require(other.ok(), "unrelated cleanup owner setup");
      std::string owner = "cleanup-fault-owner";
      allocation_boundary = boundary;
      allocation_failed = false;
      const auto result = mode == 0 ? manager.CancelLease(tokens[0]) :
          mode == 1 ? manager.CleanupExpiredLeases(5000) :
                      manager.CleanupOwner(std::move(owner));
      const bool failed = allocation_failed;
      allocation_boundary = -1;
      const auto state = manager.Snapshot();
      const auto lower = ledger.Snapshot();
      Require(result.cleaned_lease_count <= (mode == 0 ? 1u : 3u) &&
              result.cleaned_bytes == result.cleaned_lease_count * 4096 &&
              state.leases.size() == 4 - result.cleaned_lease_count &&
              state.active_bytes == 13312 - result.cleaned_bytes &&
              lower.current_bytes == state.active_bytes,
              "cleanup receipt hid effects or lost surviving ownership");
      Require(result.metrics.empty() ||
              result.metrics.front().value == result.cleaned_lease_count,
              "cleanup metric disagrees with actual completed count");
      Require(state.cancel_cleanup_count + state.expiry_cleanup_count +
              state.owner_cleanup_count == result.cleaned_lease_count,
              "cleanup counters disagree with completed effects");
      if (failed) {
        Require(!result.ok() && result.status.code == Code::memory_allocation_failed,
                "cleanup allocation failure escaped typed receipt");
        if (mode == 0) Require(result.cleaned_lease_count == 0,
                              "single cleanup failed after irreversible effects");
        partial = partial || result.cleaned_lease_count != 0;
      } else {
        Require(result.ok() && result.cleaned_lease_count == (mode == 0 ? 1u : 3u),
                "unfailed cleanup did not finish the selected leases");
        completed = true;
      }
      for (const auto& token : tokens) (void)manager.CancelLeaseNoAlloc(token);
      Require(manager.CancelLeaseNoAlloc(other.lease).ok(),
              "cleanup touched unrelated unexpired owner");
      Require(manager.Snapshot().active_bytes == 0 && ledger.Snapshot().current_bytes == 0,
              "cleanup failure prevented exact-owner retry drain");
    }
    Require(completed && (mode == 0 || partial),
            "cleanup sweep missed completion or partial-batch failure");
  }
}
}  // namespace

int main() {
  CleanupFailureBoundaries();
  using Code=scratchbird::core::platform::StatusCode;
  const auto cancel_without_allocation=[](auto& manager,auto token) {
    auto wrong=token;++wrong.bytes;
    allocation_boundary=0;allocation_failed=false;
    const auto invalid=manager.CancelLeaseNoAlloc(wrong);
    const auto cancelled=manager.CancelLeaseNoAlloc(token);
    const auto repeated=manager.CancelLeaseNoAlloc(token);
    const bool allocated=allocation_failed;allocation_boundary=-1;
    Require(!allocated && !invalid.ok() && cancelled.ok() && !repeated.ok(),
            "cancellation allocated or admitted a mismatched/stale receipt");
  };
  bool completed=false;
  for(long boundary=0;boundary<2048 && !completed;++boundary) {
    memory::HierarchicalMemoryBudgetLedger ledger;
    memory::MemoryClassPolicyLeaseManager manager(&ledger);
    auto request=Request(memory::MemoryClassKind::query_scratch,4096,"fault-acquire-owner");
    allocation_boundary=boundary;allocation_failed=false;
    auto result=manager.AcquireLease(std::move(request));
    const bool failed=allocation_failed;allocation_boundary=-1;
    const auto budgets=ledger.Snapshot();const auto leases=manager.Snapshot();
    if(failed) {
      Require(!result.ok() && result.status.code==Code::memory_allocation_failed,
              "acquire allocation failure escaped typed result");
      Require(budgets.current_bytes==0 && budgets.active_reservation_count==0 &&
              budgets.active_allocation_count==0 && leases.active_bytes==0 && leases.leases.empty() &&
              leases.created_lease_count==0,"failed lease publication leaked ledger ownership");
      const auto retry=manager.AcquireLease(Request(memory::MemoryClassKind::query_scratch,4096,"fault-acquire-owner"));
      Require(retry.ok(),"failed acquire prevented retry");
      cancel_without_allocation(manager,retry.lease);
    } else {
      Require(result.ok() && budgets.current_bytes==4096 && leases.active_bytes==4096 &&
              leases.leases.size()==1,"successful acquisition did not publish exact ownership");
      cancel_without_allocation(manager,result.lease);
      completed=true;
    }
    Require(ledger.Snapshot().current_bytes==0,"acquisition test leaked a charge");
  }
  Require(completed,"acquisition allocation sweep did not reach unfailed completion");
  completed=false;
  for(long boundary=0;boundary<1024 && !completed;++boundary) {
    memory::HierarchicalMemoryBudgetLedger ledger;
    memory::MemoryClassPolicyLeaseManager manager(&ledger);
    const auto lease=manager.AcquireLease(Request(memory::MemoryClassKind::query_scratch,4096,"fault-renew-owner"));
    Require(lease.ok(),"renewal setup failed");
    memory::MemoryBudgetLeaseRenewalRequest request;
    request.lease=lease.lease;request.now_ms=2000;request.extend_by_ms=1000;
    request.provenance=Provenance();
    allocation_boundary=boundary;allocation_failed=false;
    const auto result=manager.RenewLease(std::move(request));
    const bool failed=allocation_failed;allocation_boundary=-1;
    const auto snapshot=manager.Snapshot();
    Require(snapshot.leases.size()==1 && snapshot.active_bytes==4096 &&
            ledger.Snapshot().current_bytes==4096,"renewal changed grant ownership");
    if(failed) {
      Require(!result.ok() && result.status.code==Code::memory_allocation_failed &&
              snapshot.leases[0].deadline_ms==5000 && snapshot.leases[0].renewal_count==0 &&
              snapshot.renewal_count==0,"failed renewal changed deadline or consumed renewal quota");
    } else {
      Require(result.ok() && result.deadline_ms==6000 && snapshot.leases[0].deadline_ms==6000 &&
              snapshot.renewal_count==1,"renewal result disagrees with publication");
      completed=true;
    }
    cancel_without_allocation(manager,lease.lease);
    Require(ledger.Snapshot().current_bytes==0,"renewal fault prevented drain");
  }
  Require(completed,"renewal allocation sweep did not reach unfailed completion");
  {
    memory::HierarchicalMemoryBudgetLedger ledger;
    memory::HierarchicalMemoryReservationRequest request;
    request.scope_chain=ChainForClass(memory::MemoryClassKind::query_scratch,"retained-owner");
    request.owner_id="retained-owner";request.requested_bytes=4096;
    request.provenance=Provenance();
    const auto reserved=ledger.Reserve(std::move(request));
    Require(reserved.ok() && ledger.Commit(reserved.token).ok(),"retained setup failed");
    auto retained=ledger.Retain(reserved.token);
    Require(retained.ok(),"retained owner setup failed");
    allocation_boundary=0;allocation_failed=false;
    const auto cancelled=ledger.CancelNoAlloc(reserved.token);
    const auto repeated=ledger.CancelNoAlloc(reserved.token);
    const bool allocated=allocation_failed;allocation_boundary=-1;
    Require(!allocated && !cancelled.ok() && cancelled.retained && cancelled.newly_revoked &&
            cancelled.retained_bytes==4096 && repeated.retained && !repeated.newly_revoked &&
            ledger.Snapshot().current_bytes==4096 && !retained.lease.live(),
            "retained cancellation refunded live ownership or allocated");
    allocation_boundary=0;allocation_failed=false;
    const auto released=retained.lease.Reset();
    const auto stale=ledger.CancelNoAlloc(reserved.token);
    const bool drain_allocated=allocation_failed;allocation_boundary=-1;
    Require(!drain_allocated && released.ok() && !stale.ok() &&
            ledger.Snapshot().current_bytes==0 && ledger.Snapshot().cancel_cleanup_count==1,
            "retained drain lost cancellation accounting or allocated");
  }
  return EXIT_SUCCESS;
}
