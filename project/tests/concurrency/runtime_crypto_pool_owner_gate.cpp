// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "runtime_crypto_pool_owner.hpp"
#include <openssl/crypto.h>
#include <array>
#include <barrier>
#include <cerrno>
#include <cstdlib>
#include <iostream>
#include <pthread.h>
#include <thread>

namespace h = scratchbird::core::hash;
namespace m = scratchbird::core::memory;
namespace r = scratchbird::core::runtime;
using scratchbird::core::platform::Uuid;
thread_local long fail_allocation = -1, fail_lock = -1;
thread_local unsigned lock_calls = 0;
thread_local bool lock_injected = false;
void* operator new(std::size_t n) {
  if (fail_allocation == 0) throw std::bad_alloc();
  if (fail_allocation > 0) --fail_allocation;
  if (auto* p = std::malloc(n ? n : 1)) return p;
  throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
extern "C" int __real_pthread_mutex_lock(pthread_mutex_t*);
extern "C" int __wrap_pthread_mutex_lock(pthread_mutex_t* mutex) {
  ++lock_calls;
  if (fail_lock == 0) { fail_lock = -1; lock_injected = true; return EAGAIN; }
  if (fail_lock > 0) --fail_lock;
  return __real_pthread_mutex_lock(mutex);
}
unsigned checks = 0;
void Check(bool value, const char* label) {
  ++checks;
  if (!value) { fail_allocation = -1; std::cerr << "FAIL " << label << '\n'; std::abort(); }
}
Uuid Id(unsigned n) { Uuid value{}; value.bytes[6]=0x70; value.bytes[8]=0x80; value.bytes[15]=n; return value; }
h::CryptoMemoryBinding Binding(unsigned n) { return {Id(1),Id(n),Id(n+1),Id(n+2)}; }
struct Fixture {
  static constexpr std::size_t bytes = 4*1024*1024;
  static m::AllocationPolicy Policy() { auto p=m::DefaultLocalEngineMemoryPolicy(); p.hard_limit_bytes=bytes*2;
    p.per_context_limit_bytes=bytes*2; return p; }
  m::MemoryManager manager{Policy()};
  m::HierarchicalMemoryBudgetLedger ledger;
  h::CryptoMemoryBinding binding = Binding(20);
  std::unique_ptr<m::ReservationBackedMemoryResource> grant;
  Fixture() {
    m::ReservationBackedMemoryResourceRequest q;
    q.memory_manager=&manager; q.reservation_ledger=&ledger; q.requested_bytes=bytes;
    q.category=m::MemoryCategory::core_runtime; q.memory_class="crypto_provider";
    q.consumer_kind=m::ReservationBackedMemoryConsumerKind::background_maintenance;
    q.route_label="runtime crypto component"; q.purpose="actual runtime custody fixture";
    q.binary_operation_uuid=binding.operation.bytes;
    q.binary_ownership[m::MemoryBinaryScopeKind::database]=binding.database.bytes;
    q.binary_ownership[m::MemoryBinaryScopeKind::owner]=binding.owner.bytes;
    q.binary_ownership[m::MemoryBinaryScopeKind::context]=binding.context.bytes;
    q.scope_chain={{m::HierarchicalMemoryScopeKind::process,{},Id(90).bytes},
                   {m::HierarchicalMemoryScopeKind::database,{},binding.database.bytes}};
    q.provenance.source=m::HierarchicalMemoryBudgetProvenanceSource::server_runtime_api;
    q.provenance.source_label="runtime crypto owner conformance";
    for (const auto& scope:q.scope_chain) { m::HierarchicalMemoryBudget budget;
      budget.scope=scope; budget.hard_limit_bytes=bytes; budget.provenance=q.provenance;
      Check(ledger.SetBudget(budget).ok(),"set admitted budget"); }
    auto acquired=m::AcquireReservationBackedMemoryResource(q);
    Check(acquired.ok(),"actual grant"); grant=std::move(acquired.resource);
  }
  void Empty() { Check(manager.Snapshot().current_bytes==0 &&
    manager.Snapshot().reserved_capacity_bytes==0 && ledger.Snapshot().current_bytes==0,
    "physical and logical charges drained"); }
};
void CustodyFailures() {
  Fixture f; r::RuntimeCryptoPoolOwner owner;
  for (unsigned dim=0;dim<4;++dim) {
    auto wrong=f.binding;
    std::array<Uuid*,4> ids{&wrong.database,&wrong.operation,&wrong.owner,&wrong.context};
    *ids[dim]=Id(99);
    Check(owner.Adopt(wrong,f.grant,f.bytes).error==r::RuntimeCryptoPoolError::invalid_binding &&
          f.grant && !owner.has_custody(),"wrong binding preserves caller grant");
  }
  Check(!owner.Adopt(f.binding,f.grant,f.bytes+1).ok() && f.grant,"oversized backing preserves grant");
  Check(owner.Adopt(f.binding,f.grant,f.bytes).ok() && !f.grant,"exact custody transferred");
  Check(f.manager.Snapshot().current_bytes==f.bytes,"actual backing charged");
  fail_allocation=0; const auto closed=owner.Close(); fail_allocation=-1;
  Check(closed.ok() && !owner.has_custody(),"allocation-free close"); f.Empty();
  bool passed=false;
  for (long at=0;at<256 && !passed;++at) {
    Fixture next; r::RuntimeCryptoPoolOwner pending;
    fail_allocation=at; const auto result=pending.Adopt(next.binding,next.grant,next.bytes); fail_allocation=-1;
    passed=result.ok();
    Check(bool(next.grant)!=pending.has_custody(),"exactly one grant owner after allocation fault");
    Check(pending.Close().ok(),"allocation failure retains close path");
    if (next.grant) { Check(next.grant->ReleaseNoAlloc().ok(),"untransferred grant cleanup"); next.grant.reset(); }
    next.Empty();
  }
  Check(passed,"complete allocation sweep reached successful adoption");
}
void CleanupLockFailures() {
  unsigned boundaries = 0;
  {
    Fixture baseline; r::RuntimeCryptoPoolOwner owner;
    Check(owner.Adopt(baseline.binding,baseline.grant,baseline.bytes).ok(),"lock baseline setup");
    lock_calls=0; const auto closed=owner.Close(); boundaries=lock_calls;
    Check(closed.ok() && boundaries>3,"measured complete cleanup lock denominator"); baseline.Empty();
  }
  for (unsigned at=0;at<boundaries;++at) {
    Fixture next; r::RuntimeCryptoPoolOwner pending;
    Check(pending.Adopt(next.binding,next.grant,next.bytes).ok(),"cleanup fault setup");
    lock_injected=false; fail_lock=at; const auto result=pending.Close(); fail_lock=-1;
    Check(lock_injected,"each measured cleanup lock fault reached");
    Check(result.ok() || pending.has_custody(),"failed cleanup retains grant owner");
    Check(next.manager.Snapshot().current_bytes==pending.retained_backing_bytes(),
          "failed cleanup physical receipt remains exact");
    if (at<=3) Check(pending.retained_backing_bytes()==next.bytes,
                    "precommit lock refusal cannot free backing or refund capacity");
    const auto retry=pending.Close();
    if (!retry.ok()) std::cerr << "cleanup_fault_index=" << at << " first=" << unsigned(result.error)
      << " retry=" << unsigned(retry.error) << " memory=" << unsigned(retry.memory_status.code)
      << " retained=" << pending.retained_backing_bytes() << '\n';
    Check(retry.ok(),"cleanup retry"); next.Empty();
  }
  std::cout << "cleanup_lock_boundaries=" << boundaries << '\n';
}
void* CustomMalloc(std::size_t n,const char*,int) { return std::malloc(n); }
void* CustomRealloc(void* p,std::size_t n,const char*,int) { return std::realloc(p,n); }
void CustomFree(void* p,const char*,int) { std::free(p); }
int main(int argc,char** argv) {
  const std::string_view mode=argc>1?argv[1]:"lifecycle";
  CustodyFailures();
  if (mode=="cleanup_fault") { CleanupLockFailures(); return 0; }
  Fixture process; r::RuntimeCryptoPoolOwner owner;
  Check(owner.Adopt(process.binding,process.grant,process.bytes).ok(),"process backing owner");
  if (mode=="late" || mode=="custom") {
    if (mode=="late") { auto* p=OPENSSL_malloc(32); Check(p,"early crypto"); OPENSSL_free(p); }
    else Check(CRYPTO_set_mem_functions(CustomMalloc,CustomRealloc,CustomFree)==1,"custom hook fixture");
    const auto installed=owner.InstallProcessAdapter();
    Check(installed.adapter_error==(mode=="late"?h::CryptoMemoryError::late_installation:
          h::CryptoMemoryError::custom_allocator),"real installation refusal preserved");
    Check(owner.Close().ok(),"failed installation permits safe backing release"); process.Empty();
  } else {
    Check(owner.InstallProcessAdapter().ok(),"explicit early installation");
    Check(owner.Close().adapter_error==h::CryptoMemoryError::busy && owner.has_custody(),
          "installed pool cannot free backing");
    Fixture operation; r::RuntimeCryptoPoolOwner operation_owner;
    Check(operation_owner.Adopt(operation.binding,operation.grant,operation.bytes).ok(),"operation backing");
    std::barrier ready(2), finish(2);
    bool worker_ok=false;
    std::thread worker([&] {
      h::PreparedSha256 digest;
      const auto prepared=operation_owner.Prepare(digest,operation.binding);
      const unsigned char input='a'; const h::HashDigestSegment part{&input,1};
      const auto computed=digest.Compute(&part,1);
      worker_ok=prepared.ok() && computed.ok() && computed.digest[0]==0xca && computed.digest[31]==0xbb;
      ready.arrive_and_wait(); finish.arrive_and_wait();
      worker_ok=worker_ok && digest.Close()==h::CryptoMemoryError::none;
      OPENSSL_thread_stop();
    });
    ready.arrive_and_wait();
    Check(operation_owner.Close().adapter_error==h::CryptoMemoryError::busy &&
          operation.manager.Snapshot().current_bytes==operation.bytes,"live worker retains actual backing");
    finish.arrive_and_wait(); worker.join(); Check(worker_ok,"real SHA256 and worker cleanup");
    Check(operation_owner.Revoke().ok(),"explicit admission revocation");
    h::PreparedSha256 rejected;
    Check(operation_owner.Prepare(rejected,operation.binding).adapter_error==h::CryptoMemoryError::revoked,
          "revoked pool refuses new preparation");
    Check(operation_owner.Close().ok(),"joined operation pool closes"); operation.Empty();
    OPENSSL_thread_stop(); OPENSSL_cleanup();
    Check(h::StopCryptoMemoryAdapter()==h::CryptoMemoryError::none,"explicit process cleanup before stop");
    fail_allocation=0; const auto closed=owner.Close(); fail_allocation=-1;
    Check(closed.ok(),"process backing released only after terminal stop"); process.Empty();
    Check(!OPENSSL_malloc(1),"terminal hook refuses late allocation");
  }
  std::cout << "PASS runtime crypto custody " << mode << " checks=" << checks << '\n';
}
