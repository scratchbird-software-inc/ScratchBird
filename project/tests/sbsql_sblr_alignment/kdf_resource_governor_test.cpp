// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "kdf_resource_governor.hpp"
#include <atomic>
#include <barrier>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <new>
#include <thread>
#include <type_traits>

thread_local long fail_after = -1;
thread_local bool fault_hit = false;
void* operator new(std::size_t bytes) {
  if (fail_after >= 0 && fail_after-- == 0) { fault_hit=true; throw std::bad_alloc(); }
  if (auto* p = std::malloc(bytes ? bytes : 1)) return p;
  throw std::bad_alloc();
}
void* operator new[](std::size_t bytes) { return ::operator new(bytes); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

namespace m = scratchbird::core::memory;
unsigned checks = 0, failures = 0, injected = 0, allocation_faults = 0;
void Check(bool ok, const char* why) {
  ++checks;
  if (!ok) { ++failures; std::fprintf(stderr,"FAIL %s\n",why); }
}
m::MemoryBinaryUuid Id(unsigned value) {
  m::MemoryBinaryUuid id{}; id[0]=1; id[6]=0x70; id[8]=0x80;
  id[14]=static_cast<unsigned char>(value>>8); id[15]=static_cast<unsigned char>(value);
  return id;
}
m::KdfResourceOwner Owner(unsigned statement=4) { return {Id(1),Id(2),Id(3),Id(statement),Id(statement+100)}; }
m::KdfResourcePolicy Policy() { return {256,1000,1024,4000,10000,4}; }
struct Fixture {
  std::shared_ptr<m::HierarchicalMemoryBudgetLedger> ledger=std::make_shared<m::HierarchicalMemoryBudgetLedger>();
  std::shared_ptr<m::KdfResourceGovernor> governor;
  explicit Fixture(m::KdfResourcePolicy policy=Policy()) {
    governor=m::KdfResourceGovernor::Create(ledger,Id(1),Id(2),policy);
  }
};
void Estimate() {
  m::ScryptWorkEstimate value;
  Check(m::EstimateScryptWork(0,0,16,1,1,64,value)==m::ScryptEstimateCode::ok &&
        value.workspace_bytes==2432 && value.salsa208_calls==64 && value.sha256_blocks==40 && value.work_units==104,
        "empty-input RFC tuple has exact workspace and independently derived work units");
  Check(m::EstimateScryptWork(8,4,1024,8,16,64,value)==m::ScryptEstimateCode::ok &&
        value.workspace_bytes==1067008 && value.salsa208_calls==524288 && value.sha256_blocks==4622 && value.work_units==528910,
        "parallel RFC tuple accounts for both PBKDF2 phases and every Salsa invocation");
  using Wide=unsigned __int128;
  const auto max=std::numeric_limits<m::u64>::max();
  // Independent wider-arithmetic oracle. Exercise every binary N exponent,
  // width boundaries, large inputs and output rounding; no provider invocation.
  for(unsigned exponent=1;exponent<64;++exponent)for(m::u64 r:{1ULL,3ULL,4ULL,8ULL,32767ULL})
    for(m::u64 p:{1ULL,2ULL,31ULL})for(m::u64 length:{0ULL,63ULL,64ULL,65ULL,~0ULL}) {
      const m::u64 n=m::u64{1}<<exponent;
      const bool valid=r*p<=(m::u64{1}<<30)-1 && (r>=4||exponent<16*r);
      const auto code=m::EstimateScryptWork(length,length,n,r,p,65535,value);
      if(!valid) {Check(code==m::ScryptEstimateCode::invalid_parameters,"N structural boundary cannot become resource overflow");continue;}
      Wide rp=Wide(r)*p, wb=Wide(128)*r*(Wide(n)+p+2), salsa=4*Wide(n)*rp;
      Wide b=Wide(length)/64+(length%64!=0);
      Wide sha=4*rp*(2*b+6)+2048*(b+2*rp+6);
      const bool fits=wb<=max&&salsa<=max&&sha<=max&&salsa+sha<=max;
      Check(code==(fits?m::ScryptEstimateCode::ok:m::ScryptEstimateCode::overflow),"checked cost agrees with independent wide arithmetic");
      if(fits)Check(value.workspace_bytes==wb&&value.salsa208_calls==salsa&&value.sha256_blocks==sha&&value.work_units==salsa+sha,"no lost high bits or rounding in cost estimate");
      else Check(value.workspace_bytes==0&&value.salsa208_calls==0&&value.sha256_blocks==0&&value.work_units==0,"overflow never exposes partially admissible cost");
    }
  for(unsigned slot=0;slot<4;++slot) {
    auto n=16ULL,r=1ULL,p=1ULL,length=64ULL;
    if(slot==0)n=3;if(slot==1)r=0;if(slot==2)p=0;if(slot==3)length=65536;
    Check(m::EstimateScryptWork(0,0,n,r,p,length,value)==m::ScryptEstimateCode::invalid_parameters,"invalid structural tuple refused");
  }
}
void IdentityAndPolicy() {
  Fixture fixture;
  for(unsigned field=0;field<6;++field) {
    auto p=Policy();
    switch(field) {case 0:p.call_memory_bytes=0;break;case 1:p.call_work_units=0;break;
      case 2:p.aggregate_memory_bytes=0;break;case 3:p.aggregate_work_units=0;break;
      case 4:p.statement_work_units=0;break;case 5:p.active_calls=0;break;}
    Check(!m::KdfResourceGovernor::Create(fixture.ledger,Id(1),Id(2),p),"zero policy never means unlimited");
  }
  Check(!m::KdfResourceGovernor::Create({},Id(1),Id(2),Policy()),"no private default ledger");
  for(unsigned field=0;field<5;++field)for(unsigned byte=0;byte<16;++byte) {
    auto bad=Owner();m::MemoryBinaryUuid* identities[]={&bad.process,&bad.database,&bad.session,&bad.statement,&bad.receipt};
    (*identities[field])[byte]^=1;
    auto receipt=fixture.governor->Issue(Owner());
    Check(receipt->Acquire(bad,{64,100}).code==m::KdfAdmissionCode::invalid_owner,"every binary owner byte participates in authority binding");
  }
  for(unsigned field=0;field<5;++field) {
    auto bad=Owner();m::MemoryBinaryUuid* identities[]={&bad.process,&bad.database,&bad.session,&bad.statement,&bad.receipt};
    (*identities[field])[6]=0x40;
    Check(!fixture.governor->Issue(bad),"user-compatible UUIDv4 is not an engine resource owner");
  }
  auto receipt=fixture.governor->Issue(Owner());
  Check(receipt&&fixture.governor->Issue(Owner())==receipt,"duplicate issuance shares statement accounting");
  auto bad=Owner();bad.receipt=Id(999);
  Check(!fixture.governor->Issue(bad),"new receipt identity cannot reset a live statement budget");
  bad=Owner();bad.session=Id(999);
  Check(!fixture.governor->Issue(bad),"new session cannot steal live statement accounting");
  auto p=Policy();p.aggregate_memory_bytes=p.call_memory_bytes-1;
  Check(!m::KdfResourceGovernor::Create(fixture.ledger,Id(1),Id(2),p),"inconsistent memory policy rejected");
  p=Policy();p.statement_work_units=p.call_work_units-1;
  Check(!m::KdfResourceGovernor::Create(fixture.ledger,Id(1),Id(2),p),"inconsistent statement policy rejected");
  p=Policy();p.aggregate_work_units=p.call_work_units-1;
  Check(!m::KdfResourceGovernor::Create(fixture.ledger,Id(1),Id(2),p),"inconsistent aggregate work policy rejected");
}
void Accounting() {
  auto policy=Policy();policy.statement_work_units=2000;
  Fixture f(policy);auto receipt=f.governor->Issue(Owner());
  for(auto cost: {m::KdfResourceCost{0,1},{1,0},{257,1},{1,1001},{~m::u64{0},~m::u64{0}}}) {
    Check(receipt->Acquire(Owner(),cost).code!=m::KdfAdmissionCode::ok,"invalid/excessive costs never acquire");
    Check(receipt->consumed_work_units()==0&&f.governor->Observe().active_calls==0&&f.ledger->Snapshot().current_bytes==0,"failed admission charges nothing");
  }
  auto a=receipt->Acquire(Owner(),{256,1000});
  Check(a.code==m::KdfAdmissionCode::ok&&a.grant.live(),"explicit valid admission commits live retained memory");
  Check(f.ledger->Snapshot().active_bytes==256&&f.governor->Observe().memory_bytes==256,"actual shared ledger and governor agree");
  for(const auto& scope:f.ledger->Snapshot().scopes)Check(scope.scope_id.empty()&&scope.binary_scope_uuid!=m::MemoryBinaryUuid{},"scope accounting never materializes text UUIDs");
  auto b=receipt->Acquire(Owner(),{64,1000});
  Check(b.grant.live()&&receipt->consumed_work_units()==2000,"cumulative work charged once per admission");
  a.grant.Reset();
  Check(receipt->Acquire(Owner(),{64,1}).code==m::KdfAdmissionCode::budget_exceeded,"release never refunds consumed statement work");
  auto moved=std::move(b.grant);Check(!b.grant.live()&&moved.live(),"move transfers sole grant ownership");
  fail_after=0;moved.Reset();moved.Reset();const auto allocation_probe=fail_after;fail_after=-1;
  Check(allocation_probe==0&&f.governor->Observe().active_calls==0&&f.ledger->Snapshot().current_bytes==0,"idempotent release is allocation-free and returns actual capacity");
  // A grant retains its statement control owner even after the issuer drops it.
  auto other=f.governor->Issue(Owner(5));auto grant=other->Acquire(Owner(5),{64,1000});other.reset();
  other=f.governor->Issue(Owner(5));
  Check(other->consumed_work_units()==1000,"outstanding grant prevents budget reset by dropping receipt wrapper");
  grant.grant.Reset();
}
void Revocation() {
  for(unsigned route=0;route<3;++route) {
    Fixture f;auto receipt=f.governor->Issue(Owner());auto grant=receipt->Acquire(Owner(),{256,100});
    if(route==0)receipt->Revoke();
    if(route==1)f.governor->StopAdmission();
    if(route==2) {
      const auto cleanup=f.ledger->CleanupOwner(Owner().receipt);
      Check(cleanup.revoked_reservation_count==1&&cleanup.retained_bytes==256&&cleanup.cleaned_bytes==0,
            "parent-ledger cancellation reports revoked-but-retained payload, not completed cleanup");
    }
    Check(!grant.grant.live()&&f.governor->Observe().memory_bytes==256&&f.ledger->Snapshot().current_bytes==256,"revocation cannot erase charged live payload");
    if(route<2)Check(receipt->Acquire(Owner(),{64,1}).code==m::KdfAdmissionCode::revoked,"revoked runtime/receipt cannot admit new work");
    if(route==0)Check(!f.governor->Issue(Owner()),"revoked live statement cannot be reissued");
    if(route==1)Check(!f.governor->Issue(Owner(5)),"stopped node cannot issue new statement receipt");
    grant.grant.Reset();Check(f.ledger->Snapshot().current_bytes==0&&f.governor->Observe().memory_bytes==0,"quiescent cleanup releases revoked retained extent");
  }
}
void SharedParents() {
  Fixture f;
  m::HierarchicalMemoryBudget budget;
  budget.scope={m::HierarchicalMemoryScopeKind::process,{},Id(1)};
  budget.hard_limit_bytes=300;budget.soft_limit_bytes=300;
  budget.provenance.source=m::HierarchicalMemoryBudgetProvenanceSource::runtime_policy;
  budget.provenance.source_label="test owning runtime policy";
  Check(f.ledger->SetBudget(budget).ok(),"owning runtime sets actual process parent limit");
  auto receipt=f.governor->Issue(Owner());auto a=receipt->Acquire(Owner(),{256,100});
  auto second=m::KdfResourceGovernor::Create(f.ledger,Id(1),Id(9),Policy());
  auto second_owner=Owner(6);second_owner.database=Id(9);
  auto second_receipt=second->Issue(second_owner);
  Check(second_receipt->Acquire(second_owner,{64,100}).code==m::KdfAdmissionCode::budget_exceeded,
        "independent governor cannot evade actual shared parent memory policy");
  Check(second_receipt->consumed_work_units()==0,"parent denial consumes no statement work");
  budget.hard_limit_bytes=255;budget.soft_limit_bytes=255;
  Check(!f.ledger->SetBudget(budget).ok(),"live KDF retained extent prevents unsafe parent shrink");
  a.grant.Reset();auto admitted=second_receipt->Acquire(second_owner,{64,100});
  Check(admitted.grant.live(),"quiescent release makes parent capacity available");
}
void IndependentLimits() {
  for(unsigned axis=0;axis<3;++axis) {
    auto policy=Policy();
    if(axis==0)policy.aggregate_memory_bytes=256;
    if(axis==1)policy.aggregate_work_units=1000;
    if(axis==2)policy.active_calls=1;
    Fixture f(policy);auto first=f.governor->Issue(Owner()),second=f.governor->Issue(Owner(5));
    auto a=first->Acquire(Owner(),{256,1000});
    Check(a.grant.live(),"single-axis aggregate fixture admits first call");
    Check(second->Acquire(Owner(5),{1,1}).code==m::KdfAdmissionCode::budget_exceeded,
          "each aggregate memory/work/call limit independently rejects competing statements");
    Check(second->consumed_work_units()==0,"single-axis denial has no partial CPU debit");
    a.grant.Reset();
    auto b=second->Acquire(Owner(5),{1,1});Check(b.grant.live(),"each released aggregate budget is usable again");
  }
  const auto max=std::numeric_limits<m::u64>::max();
  Fixture f({1,max,1,max,max,1});auto receipt=f.governor->Issue(Owner());
  auto a=receipt->Acquire(Owner(),{1,max});
  Check(a.grant.live()&&receipt->consumed_work_units()==max,"maximum finite work is representable without sentinel semantics");
  a.grant.Reset();
  Check(receipt->Acquire(Owner(),{1,1}).code==m::KdfAdmissionCode::budget_exceeded&&receipt->consumed_work_units()==max,
        "cumulative work cannot wrap at uint64 boundary");
  m::ScryptWorkEstimate estimate;
  fail_after=0;const auto code=m::EstimateScryptWork(0,0,16,1,1,64,estimate);const auto probe=fail_after;fail_after=-1;
  Check(code==m::ScryptEstimateCode::ok&&probe==0,"resource cost admission arithmetic allocates nothing");
}
void Concurrency() {
  Fixture f;auto receipt=f.governor->Issue(Owner());
  std::barrier entered(17),release(17);
  std::atomic<unsigned> admitted=0,denied=0;
  std::vector<std::thread> workers;
  for(unsigned i=0;i<16;++i)workers.emplace_back([&]{
    auto grant=receipt->Acquire(Owner(),{256,1000});
    if(grant.code==m::KdfAdmissionCode::ok)++admitted;else if(grant.code==m::KdfAdmissionCode::budget_exceeded)++denied;
    entered.arrive_and_wait();release.arrive_and_wait();
  });
  entered.arrive_and_wait();
  Check(admitted==4&&denied==12&&f.governor->Observe().active_calls==4&&f.ledger->Snapshot().current_bytes==1024,"concurrent admission is atomic across work, call and actual memory limits");
  receipt->Revoke();Check(f.ledger->Snapshot().current_bytes==1024,"revocation retains every concurrent payload owner");
  release.arrive_and_wait();for(auto& worker:workers)worker.join();
  Check(receipt->consumed_work_units()==4000&&f.governor->Observe().active_calls==0&&f.ledger->Snapshot().current_bytes==0,"concurrent cleanup returns capacity without refunding work");
}
void AllocationFailures() {
  for(unsigned operation=0;operation<3;++operation) {
    bool completed=false;
    for(long at=0;at<512&&!completed;++at) {
      Fixture f;std::shared_ptr<m::KdfResourceReceipt> receipt;
      if(operation==2)receipt=f.governor->Issue(Owner());
      fault_hit=false;fail_after=at;
      try {
        if(operation==0) {auto governor=m::KdfResourceGovernor::Create(f.ledger,Id(1),Id(2),Policy());completed=bool(governor);}
        if(operation==1) {receipt=f.governor->Issue(Owner());completed=bool(receipt);}
        if(operation==2) {auto grant=receipt->Acquire(Owner(),{64,100});completed=grant.code==m::KdfAdmissionCode::ok;}
        fail_after=-1;
      }catch(const std::bad_alloc&){fail_after=-1;++injected;}
      if(fault_hit) {
        ++allocation_faults;
        Check(!completed,"handled allocation failure cannot publish a successful admission");
      }
      Check(f.ledger->Snapshot().current_bytes==0&&f.governor->Observe().active_calls==0,"every allocation failure unwinds shared memory and aggregate accounting");
      if(operation==2)Check(receipt->consumed_work_units()==(completed?100u:0u),"only successful admission consumes statement work");
      auto retry=f.governor->Issue(Owner());
      Check(bool(retry),"failed preparation cannot poison later statement issuance");
    }
    Check(completed,"allocation fault sweep reached actual successful operation");
  }
}
int main() {
  static_assert(!std::is_copy_constructible_v<m::KdfResourceGrant>);
  static_assert(!std::is_constructible_v<m::KdfResourceReceipt>);
  Estimate();IdentityAndPolicy();Accounting();Revocation();SharedParents();IndependentLimits();Concurrency();AllocationFailures();
  std::printf("%u checks, %u allocation faults (%u propagated exceptions), %u failures\n",checks,allocation_faults,injected,failures);
  return failures?1:0;
}
