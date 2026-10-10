// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "runtime_crypto_pool_owner.hpp"
#include "uuid.hpp"
#include "openpgp_seipd.hpp"
#include <openssl/crypto.h>
#include <algorithm>
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
namespace crypto = scratchbird::core::crypto;
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
  explicit Fixture(unsigned binding_seed=20, bool process_scope=false,
                   unsigned charged_process=90,
                   const h::CryptoMemoryBinding* issued=nullptr):binding(Binding(binding_seed)) {
    if (process_scope) {binding.database={}; binding.process=Id(90);}
    if (issued) binding=*issued;
    m::ReservationBackedMemoryResourceRequest q;
    q.memory_manager=&manager; q.reservation_ledger=&ledger; q.requested_bytes=bytes;
    q.category=m::MemoryCategory::core_runtime; q.memory_class="crypto_provider";
    q.consumer_kind=m::ReservationBackedMemoryConsumerKind::background_maintenance;
    q.route_label="runtime crypto component"; q.purpose="actual runtime custody fixture";
    q.binary_operation_uuid=binding.operation.bytes;
    q.binary_ownership[m::MemoryBinaryScopeKind::database]=binding.database.bytes;
    q.binary_ownership[m::MemoryBinaryScopeKind::process]=binding.process.bytes;
    q.binary_ownership[m::MemoryBinaryScopeKind::owner]=binding.owner.bytes;
    q.binary_ownership[m::MemoryBinaryScopeKind::context]=binding.context.bytes;
    q.scope_chain={{m::HierarchicalMemoryScopeKind::process,{},
                   issued?binding.process.bytes:Id(charged_process).bytes}};
    if (!process_scope) q.scope_chain.push_back(
        {m::HierarchicalMemoryScopeKind::database,{},binding.database.bytes});
    q.provenance.source=m::HierarchicalMemoryBudgetProvenanceSource::server_runtime_api;
    q.provenance.source_label="runtime crypto owner conformance";
    for (const auto& scope:q.scope_chain) { m::HierarchicalMemoryBudget budget;
      budget.scope=scope; budget.hard_limit_bytes=bytes; budget.provenance=q.provenance;
      Check(ledger.SetBudget(budget).ok(),"set admitted budget"); }
    auto acquired=m::AcquireReservationBackedMemoryResource(q);
    if (process_scope && !issued && charged_process!=90) {
      Check(!acquired.ok() && !acquired.resource &&
            acquired.diagnostic.diagnostic_code=="SB_CEIC_012_MEMORY_RESOURCE.IDENTITY_REQUIRED",
            "shared issuer refuses mismatched process root before reservation");
      return;
    }
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
  Check(owner.InstallProcessAdapter().error==r::RuntimeCryptoPoolError::invalid_binding &&
        !h::SnapshotCryptoMemoryAdapter().installed,
        "database owner cannot impersonate process bootstrap");
  Check(f.manager.Snapshot().current_bytes==f.bytes,"actual backing charged");
  bool entered=false;
  Check(owner.WithMemoryScope(f.binding,[&] {entered=true;}).adapter_error==
            h::CryptoMemoryError::not_installed && !entered,
        "uninstalled process adapter refuses callback");
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
void ScopedProviderOperations(r::RuntimeCryptoPoolOwner& owner,
                              const h::CryptoMemoryBinding& binding) {
  bool entered = false;
  for (unsigned dim=0;dim<5;++dim) {
    auto wrong=binding;
    std::array<Uuid*,5> ids{&wrong.database,&wrong.operation,&wrong.owner,&wrong.context,&wrong.process};
    *ids[dim]=Id(99);
    Check(owner.WithMemoryScope(wrong,[&] { entered=true; }).error ==
              r::RuntimeCryptoPoolError::invalid_binding && !entered,
          "scope mismatch never executes callback");
  }
  constexpr std::array<unsigned char,5> plaintext{0,0xff,1,2,3};
  const std::array<unsigned char,32> key{}, salt{};
  const auto size=crypto::SeipdEncryptedSize({},plaintext.size());
  Check(size.code==crypto::PgpCode::ok,"authenticated message extent");
  struct PrivateBuffer {
    unsigned char* data; std::size_t bytes;
    explicit PrivateBuffer(std::size_t n)
        :data(static_cast<unsigned char*>(OPENSSL_zalloc(n))),bytes(n) {}
    ~PrivateBuffer() { OPENSSL_clear_free(data,bytes); }
  };
  bool verified=false, tampering_refused=false;
  const auto admitted=owner.WithMemoryScope(binding,[&] {
    PrivateBuffer body(size.bytes), recovered(plaintext.size());
    Check(body.data && recovered.data,"private stages use admitted pool");
    Check(owner.Snapshot().scopes==1 && owner.Snapshot().live_blocks>=2,
          "scope and actual stage allocations remain charged");
    const auto encrypted=crypto::EncryptSeipdV2({}, {key.data(),key.size()},
        {salt.data(),salt.size()}, {plaintext.data(),plaintext.size()},
        {body.data,body.bytes});
    Check(encrypted==crypto::PgpCode::ok,"real provider authenticated encryption");
    const auto decrypted=crypto::DecryptSeipdV2({key.data(),key.size()},
        {body.data,body.bytes},{recovered.data,recovered.bytes});
    verified=decrypted==crypto::PgpCode::ok &&
        std::equal(plaintext.begin(),plaintext.end(),recovered.data);
    body.data[body.bytes-1]^=1;
    const auto invalid=crypto::DecryptSeipdV2({key.data(),key.size()},
        {body.data,body.bytes},{recovered.data,recovered.bytes});
    tampering_refused=invalid==crypto::PgpCode::authentication_failed &&
        std::all_of(recovered.data,recovered.data+recovered.bytes,
                    [](unsigned char byte) {return byte==0;});
  });
  Check(admitted.ok() && verified && tampering_refused,
        "memory admission does not conceal authentication failure");
  Check(owner.Snapshot().scopes==0,"normal provider operation unpins");
  const auto before=owner.Snapshot();
  bool caught=false;
  try {
    owner.WithMemoryScope(binding,[&] {
      PrivateBuffer scratch(97);
      Check(scratch.data,"throwing callback uses actual backing");
      throw 42;
    });
  } catch (int value) {caught=value==42;}
  Check(caught && owner.Snapshot().scopes==before.scopes &&
        owner.Snapshot().live_blocks==before.live_blocks &&
        owner.Snapshot().live_bytes==before.live_bytes,
        "callback exception propagates after private cleanup and unpin");
  fail_allocation=0;
  const auto no_heap=owner.WithMemoryScope(binding,[&] {
    PrivateBuffer scratch(37);
    entered=scratch.data!=nullptr;
  });
  fail_allocation=-1;
  Check(no_heap.ok() && entered,"scope entry and provider allocation need no C++ heap");
}
int main(int argc,char** argv) {
  const std::string_view mode=argc>1?argv[1]:"lifecycle";
  CustodyFailures();
  if (mode=="cleanup_fault") { CleanupLockFailures(); return 0; }
  {
    Fixture mislabeled(20,true,91); r::RuntimeCryptoPoolOwner rejected;
    Check(!mislabeled.grant &&
          rejected.Adopt(mislabeled.binding,mislabeled.grant,mislabeled.bytes).error==
              r::RuntimeCryptoPoolError::invalid_grant &&
              !rejected.has_custody(),"process tag cannot relabel another charged root");
    mislabeled.Empty();
  }
  const auto issued=scratchbird::core::uuid::IssueCryptoBootstrapIdentitiesV7();
  Check(bool(issued),"real OS bootstrap UUIDs before provider use");
  const h::CryptoMemoryBinding process_binding{{},issued->operation,issued->owner,issued->context,issued->process};
  Fixture process(20,true,90,&process_binding); r::RuntimeCryptoPoolOwner owner;
  for (unsigned dimension=0;dimension<5;++dimension) {
    auto wrong=process.binding;
    std::array<Uuid*,5> ids{&wrong.database,&wrong.operation,&wrong.owner,&wrong.context,&wrong.process};
    *ids[dimension]=Id(99);
    Check(owner.Adopt(wrong,process.grant,process.bytes).error==r::RuntimeCryptoPoolError::invalid_binding &&
          process.grant && !owner.has_custody(),"process binding refusal preserves real grant");
  }
  Check(owner.Adopt(process.binding,process.grant,process.bytes).ok(),"process backing owner");
  Check(owner.CheckProcessAdapter(process.binding).adapter_error==h::CryptoMemoryError::not_installed,
        "process backing is not installed provider authority");
  if (mode=="late" || mode=="custom") {
    if (mode=="late") { auto* p=OPENSSL_malloc(32); Check(p,"early crypto"); OPENSSL_free(p); }
    else Check(CRYPTO_set_mem_functions(CustomMalloc,CustomRealloc,CustomFree)==1,"custom hook fixture");
    const auto installed=owner.InstallProcessAdapter();
    Check(installed.adapter_error==(mode=="late"?h::CryptoMemoryError::late_installation:
          h::CryptoMemoryError::custom_allocator),"real installation refusal preserved");
    Check(owner.Close().ok(),"failed installation permits safe backing release"); process.Empty();
  } else {
    Check(owner.InstallProcessAdapter().ok(),"explicit early installation");
    Check(owner.CheckProcessAdapter(process.binding).ok(),"exact installed process binding");
    std::optional<Uuid> ordinary;
    Check(owner.WithMemoryScope(process.binding,[&] {
      ordinary=scratchbird::core::uuid::IssueRuntimeIdentityV7();
    }).ok() && ordinary && scratchbird::core::uuid::IsEngineIdentityUuid(*ordinary),
          "ordinary OpenSSL UUID generation works after OS-only bootstrap and actual hook installation");
    Check(owner.Snapshot().live_blocks>0 && process.manager.Snapshot().current_bytes==process.bytes,
          "real RNG provider allocations remain physically charged to process");
    Check(h::SnapshotCryptoMemoryAdapter().process_binding.database==Uuid{} &&
          h::SnapshotCryptoMemoryAdapter().process_binding.process==process.binding.process,
          "bootstrap records genuine process identity without database");
    for (unsigned dimension=0;dimension<5;++dimension) {
      auto wrong=process.binding;
      std::array<Uuid*,5> ids{&wrong.database,&wrong.operation,&wrong.owner,&wrong.context,&wrong.process};
      *ids[dimension]=Id(99);
      Check(owner.CheckProcessAdapter(wrong).error==r::RuntimeCryptoPoolError::invalid_binding,
            "ready observation requires all exact identities");
    }
    lock_calls=0;
    const auto baseline=owner.CheckProcessAdapter(process.binding);
    const auto observation_locks=lock_calls;
    Check(baseline.ok() && observation_locks>=3,"measure full startup observation lock path");
    for (unsigned at=0;at<observation_locks;++at) {
      lock_injected=false; fail_lock=at;
      const auto observation=owner.CheckProcessAdapter(process.binding);
      fail_lock=-1;
      Check(lock_injected && !observation.ok() && owner.has_custody() &&
            (observation.error==r::RuntimeCryptoPoolError::synchronization_failure ||
             observation.adapter_error==h::CryptoMemoryError::busy),
            "every failed startup observation preserves custody and refuses readiness");
      Check(owner.CheckProcessAdapter(process.binding).ok(),"startup observation retry");
    }
    {
      Fixture duplicate(20,true,90,&process_binding); r::RuntimeCryptoPoolOwner other;
      Check(other.Adopt(duplicate.binding,duplicate.grant,duplicate.bytes).ok(),"separate real grant with same tuple");
      Check(other.CheckProcessAdapter(duplicate.binding).adapter_error==h::CryptoMemoryError::invalid_binding,
            "duplicate tuple cannot impersonate installed backing owner");
      Check(other.Close().ok(),"uninstalled duplicate backing can close"); duplicate.Empty();
    }
    Check(owner.Close().adapter_error==h::CryptoMemoryError::busy && owner.has_custody(),
          "installed pool cannot free backing");
    // Admit the selected provider profiles under the process owner before
    // worker scopes; cached provider objects have process, not statement, life.
    ScopedProviderOperations(owner,process.binding);
    OPENSSL_thread_stop();
    Fixture operation(40); r::RuntimeCryptoPoolOwner operation_owner;
    Check(operation_owner.Adopt(operation.binding,operation.grant,operation.bytes).ok(),"operation backing");
    ScopedProviderOperations(operation_owner,operation.binding);
    OPENSSL_thread_stop();
    void* retained=nullptr;
    const auto before_retained=operation_owner.Snapshot();
    Check(operation_owner.WithMemoryScope(operation.binding,[&] {
      retained=OPENSSL_malloc(83);
    }).ok() && retained && operation_owner.Snapshot().scopes==0 &&
        operation_owner.Snapshot().live_blocks==before_retained.live_blocks+1,
        "provider allocation outlives scope without losing charge");
    Check(operation_owner.Close().adapter_error==h::CryptoMemoryError::busy &&
              operation.manager.Snapshot().current_bytes==operation.bytes,
          "ended scope does not authorize retained provider backing release");
    OPENSSL_clear_free(retained,83);
    Check(operation_owner.Snapshot().live_blocks==before_retained.live_blocks &&
              operation_owner.Snapshot().live_bytes==before_retained.live_bytes,
          "retained provider allocation drains exactly once");
    const auto process_before=owner.Snapshot();
    Check(operation_owner.WithMemoryScope(operation.binding,[&] {
      Check(owner.WithMemoryScope(process.binding,[&] {
        retained=OPENSSL_malloc(53);
      }).ok() && retained && owner.Snapshot().live_blocks==process_before.live_blocks+1,
          "nested process scope assigns allocation to process backing");
      auto* restored=OPENSSL_malloc(61);
      Check(restored && operation_owner.Snapshot().live_blocks==before_retained.live_blocks+1,
            "nested scope restores outer operation binding");
      OPENSSL_clear_free(restored,61);
      OPENSSL_clear_free(retained,53);
    }).ok(),"nested owners execute under distinct binary bindings");
    {
      std::barrier entered(2), leave(2);
      bool callback_ok=false;
      std::thread callback_worker([&] {
        const auto admitted=operation_owner.WithMemoryScope(operation.binding,[&] {
          auto* allocation=OPENSSL_malloc(193);
          callback_ok=allocation!=nullptr;
          entered.arrive_and_wait(); leave.arrive_and_wait();
          OPENSSL_clear_free(allocation,193);
        });
        callback_ok=callback_ok && admitted.ok();
        OPENSSL_thread_stop();
      });
      entered.arrive_and_wait();
      Check(operation_owner.Close().adapter_error==h::CryptoMemoryError::busy &&
                operation.manager.Snapshot().current_bytes==operation.bytes,
            "active callback prevents backing release");
      leave.arrive_and_wait(); callback_worker.join();
      Check(callback_ok && operation_owner.Snapshot().scopes==0,
            "joined callback drains scope and provider data");
    }
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
    bool entered=false;
    Check(operation_owner.WithMemoryScope(operation.binding,[&] {entered=true;}).adapter_error==
              h::CryptoMemoryError::revoked && !entered,
          "revoked owner does not enter provider callback");
    h::PreparedSha256 rejected;
    Check(operation_owner.Prepare(rejected,operation.binding).adapter_error==h::CryptoMemoryError::revoked,
          "revoked pool refuses new preparation");
    Check(operation_owner.Close().ok(),"joined operation pool closes"); operation.Empty();
    OPENSSL_thread_stop(); OPENSSL_cleanup();
    Check(h::StopCryptoMemoryAdapter()==h::CryptoMemoryError::none,"explicit process cleanup before stop");
    Check(owner.CheckProcessAdapter(process.binding).adapter_error==h::CryptoMemoryError::closed &&
          owner.InstallProcessAdapter().adapter_error==h::CryptoMemoryError::already_installed,
          "terminal process provider cannot become ready or restart");
    fail_allocation=0; const auto closed=owner.Close(); fail_allocation=-1;
    Check(closed.ok(),"process backing released only after terminal stop"); process.Empty();
    Check(!OPENSSL_malloc(1),"terminal hook refuses late allocation");
  }
  std::cout << "PASS runtime crypto custody " << mode << " checks=" << checks << '\n';
}
