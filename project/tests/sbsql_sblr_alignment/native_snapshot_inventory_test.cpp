// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "engine/internal_api/transaction/native_snapshot_inventory.hpp"
#include "engine/internal_api/transaction/native_startup_lifetime.hpp"
#include <cstdlib>
#include <iostream>
#include <new>
#include <source_location>
#include <stdexcept>
namespace api=scratchbird::engine::internal_api;
namespace mga=scratchbird::transaction::mga;
namespace page=scratchbird::storage::page;
namespace p=scratchbird::core::platform;
static bool forbid_heap=false;
void* operator new(std::size_t n){if(forbid_heap)std::abort();if(auto* out=std::malloc(n?n:1))return out;throw std::bad_alloc();}
void* operator new[](std::size_t n){return ::operator new(n);}
void operator delete(void* ptr)noexcept{std::free(ptr);}void operator delete[](void* ptr)noexcept{std::free(ptr);}
void operator delete(void* ptr,std::size_t)noexcept{std::free(ptr);}void operator delete[](void* ptr,std::size_t)noexcept{std::free(ptr);}
namespace {
using E=api::NativeSnapshotInventoryError;using S=mga::TransactionState;
unsigned checks=0;
void Check(bool good,const char* why,std::source_location where=std::source_location::current()){
  ++checks;if(!good)throw std::runtime_error(std::string(why)+" line="+std::to_string(where.line()));}
p::Uuid Id(unsigned n){p::Uuid id;id.bytes[0]=1;id.bytes[6]=0x70;id.bytes[8]=0x80;
  id.bytes[12]=p::byte(n>>24);id.bytes[13]=p::byte(n>>16);id.bytes[14]=p::byte(n>>8);id.bytes[15]=p::byte(n);return id;}
struct Fixture {
  std::array<mga::TransactionInventoryEntry,13> entries;
  std::array<p::u64,16> active,in_doubt;
  std::array<std::size_t,13> indices{};std::array<p::byte,13> markers{};
  page::NativeTransactionInventoryConstView source;
  p::TypedUuid owner{p::UuidKind::transaction,Id(3)};p::u64 number=3;
  Fixture(){for(unsigned i=0;i<entries.size();++i){auto& e=entries[i];
    const auto local=2*i+1;e.identity={{local},{p::UuidKind::transaction,Id(local)},mga::TransactionScope::local_node};
    e.state=S(i+1);e.begin_unix_epoch_millis=100+local;e.begin_visible_through_local_transaction_id=local-1;
    if(e.state==S::committed){e.commit_sequence=5;e.evidence_record_written=true;e.final_unix_epoch_millis=200;}
    if(e.state==S::archived){e.archived_from_state=S::committed;e.commit_sequence=2;e.evidence_record_written=true;e.final_unix_epoch_millis=201;}
  }source={30,7,entries};active.fill(987654321);in_doubt.fill(987654321);}
  auto Workspace(){return api::NativeSnapshotInventoryWorkspace{active,in_doubt,indices,markers};}
  auto Run(){forbid_heap=true;auto result=api::ProjectNativeSnapshotInventory(source,owner,number,Workspace());forbid_heap=false;return result;}
  void Refuse(E expected){const auto a=active,d=in_doubt;const auto result=Run();
    Check(result.error==expected&&!result.ok()&&result.active.empty()&&result.in_doubt.empty(),"refusal has no partial snapshot");
    Check(a==active&&d==in_doubt,"refusal leaves output unchanged");}
};
void ExactProjection(){Fixture f;auto r=f.Run();Check(r.ok(),"complete projection");
  const std::array<p::u64,2> active{3,25};const std::array<p::u64,7> doubt{1,5,7,9,13,17,19};
  Check(std::ranges::equal(r.active,active)&&std::ranges::equal(r.in_doubt,doubt),"all thirteen states project independent exact exclusions");
  Check(r.next_transaction_number==30&&r.visible_committed_high_watermark==29&&r.visible_commit_sequence==6,
      "high watermark is next minus one including holes, independent of commit ordering");
  Check(r.owning_transaction_number==3&&r.owning_transaction_uuid.value==Id(3)&&r.horizons.valid&&
      r.horizons.oldest_interesting_transaction.value==1&&r.horizons.oldest_active_transaction.value==3&&
      r.horizons.oldest_snapshot_transaction.value==1&&r.retention_horizon==1,"exact owner and horizons");
  Check(f.active[2]==987654321&&f.in_doubt[7]==987654321,"unused output capacity unchanged");
  for(unsigned local:{1,3,25}){Fixture g;g.number=local;g.owner.value=Id(local);auto s=g.Run();Check(s.ok(),"starting, writable and read-only owners");}
  Fixture end;end.source.next_local_transaction_id=UINT64_MAX;end.source.next_commit_sequence=UINT64_MAX;
  auto last=end.Run();Check(last.ok()&&last.visible_committed_high_watermark==UINT64_MAX-1&&last.visible_commit_sequence==UINT64_MAX-1,
      "independent maximum counters without wrap");
  // An active stable reader's commit boundary can retain an older local number
  // even when OIT and every current exclusion are newer.
  Fixture stable;stable.entries[0].state=S::committed;stable.entries[0].commit_sequence=2;
  stable.entries[1].stable_snapshot=true;stable.entries[1].begin_visible_through_commit_sequence=1;
  stable.source={4,3,std::span{stable.entries}.first(2)};auto kept=stable.Run();
  Check(kept.ok()&&kept.horizons.oldest_interesting_transaction.value==3&&
      kept.horizons.oldest_active_transaction.value==3&&kept.horizons.oldest_snapshot_transaction.value==1&&kept.retention_horizon==1,
      "stable-reader retention includes commits after that reader's independent commit boundary");
  Fixture starting;starting.entries[1].state=S::created;starting.source={4,1,std::span{starting.entries}.first(2)};
  auto pending=starting.Run();Check(pending.ok()&&pending.active.empty()&&pending.in_doubt.size()==2&&
      pending.horizons.oldest_active_transaction.value==4,"starting allocation never enters active accounting");
}
void Refusals(){for(unsigned fault=0;fault<19;++fault){Fixture f;E e=E::invalid_inventory;
  switch(fault){
    case 0:f.owner.kind=p::UuidKind::object;e=E::invalid_request;break;
    case 1:f.owner.value={};e=E::invalid_request;break;
    case 2:f.number=0;e=E::invalid_request;break;
    case 3:f.owner.value=Id(5);e=E::owner_mismatch;break;
    case 4:f.number=4;f.owner.value=Id(4);e=E::owner_mismatch;break;
    case 5:f.entries[1].rollback_only=true;e=E::owner_ineligible;break;
    case 6:f.entries[1].state=S::preparing;e=E::owner_ineligible;break;
    case 7:f.entries[0].identity.scope=mga::TransactionScope::cluster_global;e=E::cluster_requires_authority;break;
    case 8:f.entries[1].identity.scope=mga::TransactionScope::cluster_global;e=E::cluster_requires_authority;break;
    case 9:f.source.next_local_transaction_id=0;break;
    case 10:f.source.next_commit_sequence=0;break;
    case 11:f.entries[2].identity.transaction_uuid=f.entries[0].identity.transaction_uuid;break;
    case 12:f.entries[2].identity.local_id=f.entries[0].identity.local_id;break;
    case 13:std::swap(f.entries[0],f.entries[1]);break;
    case 14:f.entries[0].state=S::none;break;
    case 15:f.entries[11].commit_sequence=5;break;
    case 16:f.entries[11].archived_from_state=S::active;break;
    case 17:f.source.entries={};e=E::owner_mismatch;break;
    case 18:f.owner.value.bytes[6]=0x40;e=E::invalid_request;break;
  }f.Refuse(e);
}
for(unsigned fault=0;fault<8;++fault){Fixture f;auto w=f.Workspace();E expected=E::resource_exhausted;
  switch(fault){case 0:w.active=w.active.first(1);break;case 1:w.in_doubt=w.in_doubt.first(6);break;
    case 2:w.indices=w.indices.first(12);break;case 3:w.markers=w.markers.first(12);break;
    case 4:w.active=w.in_doubt;expected=E::invalid_workspace;break;
    case 5:w.active={reinterpret_cast<p::u64*>(f.entries.data()),2};expected=E::invalid_workspace;break;
    case 6:w.markers={reinterpret_cast<p::byte*>(&f.owner),13};expected=E::invalid_workspace;break;
    case 7:w.indices={reinterpret_cast<std::size_t*>(reinterpret_cast<p::byte*>(f.indices.data())+1),12};expected=E::invalid_workspace;break;
  }
  const auto a=f.active,d=f.in_doubt;forbid_heap=true;const auto r=api::ProjectNativeSnapshotInventory(f.source,f.owner,f.number,w);forbid_heap=false;
  Check(r.error==expected&&!r.ok()&&r.active.empty()&&r.in_doubt.empty()&&a==f.active&&d==f.in_doubt,
      "short and overlapping workspace refuses before changing output");
}}
void LifetimeBounds(){
  using L=api::NativeStartupLifetimeError;
  const auto check=[](p::u64 start,p::u64 last,std::optional<p::u64> snapshot,p::u64 now,p::u64 tx,p::u64 age,L expected){
    forbid_heap=true;const auto result=api::CheckNativeStartupLifetime(start,last,snapshot,now,tx,age);forbid_heap=false;
    Check(result==expected,"native elapsed lifetime boundary");
  };
  check(0,0,{},0,1,1,L::none);
  check(1000,1000,{},1999,1,1,L::none);
  check(1000,1000,{},2000,1,1,L::transaction_expired);
  check(1000,2000,2000,2999,10,1,L::none);
  check(1000,2000,2000,3000,10,1,L::snapshot_expired);
  check(1000,2000,2000,3000,2,1,L::transaction_expired);
  check(1000,2500,2000,2499,10,10,L::clock_regressed);
  check(1000,1000,{},999,10,10,L::clock_regressed);
  check(1000,999,{},1000,10,10,L::invalid_request);
  check(1000,2000,999,3000,10,10,L::invalid_request);
  check(1000,2000,2001,3000,10,10,L::invalid_request);
  // Zero is no limit (owner 2026-11-04, SB_DRIFT_TRANSACTION_POLICY_ZERO_IS_NO_LIMIT);
  // these two zero bounds were refused as invalid_request before.
  check(1000,1000,{},1000,0,1,L::none);
  check(1000,1000,{},1000,1,0,L::none);
  check(0,0,{},UINT64_MAX,0,0,L::none);
  check(0,0,0,UINT64_MAX,0,0,L::none);
  check(1000,2000,2000,UINT64_MAX,0,0,L::none);
  // The other bound still applies when one of them is unlimited.
  check(1000,2000,2000,3000,0,1,L::snapshot_expired);
  check(1000,2000,2000,3000,1,0,L::transaction_expired);
  // Unlimited bounds do not excuse a regressed clock or malformed input.
  check(1000,1000,{},999,0,0,L::clock_regressed);
  check(1000,999,{},1000,0,0,L::invalid_request);
  check(1000,2000,2001,3000,0,0,L::invalid_request);
  check(0,0,0,UINT64_MAX,UINT64_MAX,UINT64_MAX,L::none);
  check(0,0,{},UINT64_MAX,UINT64_MAX/1000,UINT64_MAX,L::transaction_expired);
  check(0,0,{},UINT64_MAX,UINT64_MAX/1000+1,UINT64_MAX,L::none);
  check(UINT64_MAX-1000,UINT64_MAX-1000,{},UINT64_MAX-1,1,1,L::none);
  check(UINT64_MAX-1000,UINT64_MAX-1000,{},UINT64_MAX,1,1,L::transaction_expired);
}
}
int main(){try{ExactProjection();Refusals();LifetimeBounds();std::cout<<"PASS native complete snapshot inventory "<<checks<<" checks\n";return 0;}
catch(const std::exception& e){forbid_heap=false;std::cerr<<e.what()<<'\n';return 1;}}
