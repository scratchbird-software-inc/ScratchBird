// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "engine/internal_api/transaction/native_local_transaction_candidate.hpp"
#include <array>
#include <cstdlib>
#include <iostream>
#include <new>
#include <source_location>
#include <stdexcept>
#include <tuple>
#include <vector>
namespace api=scratchbird::engine::internal_api;
namespace mga=scratchbird::transaction::mga;
namespace page=scratchbird::storage::page;
namespace p=scratchbird::core::platform;
static bool forbid_heap=false;
void* operator new(std::size_t n){if(forbid_heap)std::abort();if(auto* out=std::malloc(n?n:1))return out;throw std::bad_alloc();}
void* operator new[](std::size_t n){return ::operator new(n);}
void operator delete(void* p)noexcept{std::free(p);}void operator delete[](void* p)noexcept{std::free(p);}
void operator delete(void* p,std::size_t)noexcept{std::free(p);}void operator delete[](void* p,std::size_t)noexcept{std::free(p);}
namespace {
using E=api::NativeLocalTransactionCandidateError;using C=api::NativeLocalTransactionChange;using S=mga::TransactionState;
unsigned checks=0;
void Check(bool good,const char* why,std::source_location where=std::source_location::current()){
  ++checks;if(!good)throw std::runtime_error(std::string(why)+" line="+std::to_string(where.line()));}
p::Uuid Id(unsigned n){p::Uuid id;id.bytes[0]=1;id.bytes[6]=0x70;id.bytes[8]=0x80;
  id.bytes[12]=p::byte(n>>24);id.bytes[13]=p::byte(n>>16);id.bytes[14]=p::byte(n>>8);id.bytes[15]=p::byte(n);return id;}
auto Fields(const mga::TransactionInventoryEntry& e){return std::tuple{e.identity.local_id.value,
  e.identity.transaction_uuid.kind,e.identity.transaction_uuid.value,e.identity.scope,e.state,e.archived_from_state,
  e.begin_unix_epoch_millis,e.final_unix_epoch_millis,e.begin_visible_through_local_transaction_id,
  e.begin_visible_through_commit_sequence,e.commit_sequence,e.evidence_record_required,e.evidence_record_written,
  e.rollback_only,e.stable_snapshot};}
struct Fixture {
  std::array<mga::TransactionInventoryEntry,3> original;
  std::array<mga::TransactionInventoryEntry,5> output;
  std::array<std::size_t,5> indices{};std::array<p::byte,5> markers{};
  page::NativeTransactionInventoryConstView source;
  api::NativeLocalTransactionCandidateRequest request;
  Fixture(){
    for(unsigned n=0;n<3;++n){auto& e=original[n];const unsigned local=n==0?1:n==1?5:7;
      e.identity={{local},{p::UuidKind::transaction,Id(local)},mga::TransactionScope::local_node};
      e.begin_unix_epoch_millis=100+local;e.begin_visible_through_local_transaction_id=local-1;}
    original[0].state=S::committed;original[0].commit_sequence=3;original[0].final_unix_epoch_millis=150;original[0].evidence_record_written=true;
    original[1].state=S::active;original[1].begin_visible_through_commit_sequence=3;
    original[2].state=S::archived;original[2].archived_from_state=S::rolled_back;original[2].final_unix_epoch_millis=160;
    original[2].evidence_record_written=true;
    source={9,4,original};request={C::allocate_starting,{p::UuidKind::transaction,Id(9)},9,200,true};
    for(auto& e:output)e.identity.local_id.value=987654321;
  }
  auto Workspace(){return api::NativeLocalTransactionCandidateWorkspace{output,indices,markers};}
  auto Run(){forbid_heap=true;auto result=api::BuildNativeLocalTransactionCandidate(source,request,Workspace());forbid_heap=false;return result;}
  void Refuse(E expected){const auto before=output;const auto result=Run();
    Check(!result.ok()&&result.error==expected&&result.inventory.entries.empty(),"refusal contains no partial inventory");
    for(unsigned i=0;i<output.size();++i)Check(Fields(before[i])==Fields(output[i]),"refusal leaves output entries unchanged");}
};
void Allocation(){Fixture f;const auto original=f.original;const auto result=f.Run();
  Check(result.ok()&&result.changed_index==3&&result.inventory.entries.size()==4,"complete allocated candidate");
  Check(result.inventory.next_local_transaction_id==10&&result.inventory.next_commit_sequence==4,"allocation advances only the local counter");
  for(unsigned i=0;i<3;++i)Check(Fields(f.original[i])==Fields(original[i])&&Fields(f.output[i])==Fields(original[i]),"retained holes and all earlier fields preserved");
  const auto& e=f.output[3];
  Check(e.identity.local_id.value==9&&e.identity.transaction_uuid.value==Id(9)&&e.identity.transaction_uuid.kind==p::UuidKind::transaction&&
    e.identity.scope==mga::TransactionScope::local_node&&e.state==S::created&&e.archived_from_state==S::none&&
    e.begin_unix_epoch_millis==200&&e.final_unix_epoch_millis==0&&e.begin_visible_through_local_transaction_id==8&&
    e.begin_visible_through_commit_sequence==3&&e.commit_sequence==0&&e.evidence_record_required&&!e.evidence_record_written&&
    !e.rollback_only&&e.stable_snapshot,"starting records preserve independent allocation/commit visibility boundaries and require real evidence");
  Check(f.output[4].identity.local_id.value==987654321,"unused output capacity untouched");
  Fixture empty;empty.source={1,1,{}};empty.request.local_transaction_id=1;
  auto first=empty.Run();Check(first.ok()&&first.inventory.entries[0].begin_visible_through_local_transaction_id==0&&
    first.inventory.entries[0].begin_visible_through_commit_sequence==0,"empty inventory allocation");
  Fixture max;max.source.next_local_transaction_id=UINT64_MAX-1;max.source.next_commit_sequence=UINT64_MAX;
  max.request.local_transaction_id=UINT64_MAX-1;auto last=max.Run();Check(last.ok()&&last.inventory.next_local_transaction_id==UINT64_MAX&&
    last.inventory.next_commit_sequence==UINT64_MAX&&last.inventory.entries.back().begin_visible_through_commit_sequence==UINT64_MAX-1,
    "last representable local allocation does not consume commit order");
}
void Refusals(){for(unsigned fault=0;fault<17;++fault){Fixture f;E error=E::invalid_request;
  switch(fault){
    case 0:f.request.transaction_uuid.kind=p::UuidKind::object;break;
    case 1:f.request.transaction_uuid.value={};break;
    case 2:f.request.local_transaction_id=0;break;
    case 3:f.request.change=static_cast<C>(255);break;
    case 4:f.request.local_transaction_id=8;error=E::identity_mismatch;break;
    case 5:f.request.local_transaction_id=10;error=E::identity_mismatch;break;
    case 6:f.request.transaction_uuid.value=Id(5);error=E::identity_mismatch;break;
    case 7:f.source.next_local_transaction_id=UINT64_MAX;f.request.local_transaction_id=UINT64_MAX;error=E::counter_exhausted;break;
    case 8:f.original[1].identity.transaction_uuid=f.original[0].identity.transaction_uuid;error=E::invalid_inventory;break;
    case 9:f.original[1].identity.local_id=f.original[0].identity.local_id;error=E::invalid_inventory;break;
    case 10:f.source.next_commit_sequence=0;error=E::invalid_inventory;break;
    case 11:std::swap(f.original[0],f.original[1]);error=E::invalid_inventory;break;
    case 12:f.original[1].state=S::committed;f.original[1].commit_sequence=3;f.original[1].begin_visible_through_commit_sequence=0;error=E::invalid_inventory;break;
    case 13:f.original[1].state=S::none;error=E::invalid_inventory;break;
    case 14:f.original[1].identity.scope=mga::TransactionScope::unknown;error=E::invalid_inventory;break;
    case 15:f.original[0].begin_visible_through_commit_sequence=4;error=E::invalid_inventory;break;
    case 16:f.request.transaction_uuid.value.bytes[6]=0x40;break;
  }f.Refuse(error);
}}
void Transitions(){for(unsigned n=1;n<=13;++n)for(C change:{C::activate,C::activate_read_only,C::mark_rollback_only,C::start_rollback}){
  Fixture f;auto& row=f.original[1];row.state=static_cast<S>(n);row.begin_visible_through_commit_sequence=0;
  if(row.state==S::committed){row.commit_sequence=2;row.evidence_record_written=true;}
  if(row.state==S::archived)row.archived_from_state=S::rolled_back;
  f.request={change,row.identity.transaction_uuid,5,0,false};
  const bool activating=change==C::activate||change==C::activate_read_only;
  const bool allowed=activating?row.state==S::created:row.state==S::created||row.state==S::active||row.state==S::read_only_active||row.state==S::preparing||row.state==S::prepared;
  if(!allowed){f.Refuse(E::state_mismatch);continue;}
  const auto original=row;const auto result=f.Run();Check(result.ok()&&result.changed_index==1&&result.inventory.entries.size()==3&&
    result.inventory.next_local_transaction_id==9&&result.inventory.next_commit_sequence==4,"state changes retain both counters and full inventory");
  auto expected=original;if(activating)expected.state=change==C::activate?S::active:S::read_only_active;
  else{expected.rollback_only=true;if(change==C::start_rollback)expected.state=S::rolling_back;}
  Check(Fields(expected)==Fields(f.output[1])&&Fields(f.original[0])==Fields(f.output[0])&&Fields(f.original[2])==Fields(f.output[2]),"exact transition changes no unrelated field or evidence");
}
  for(unsigned fault=0;fault<6;++fault){Fixture f;f.original[1].state=S::created;f.request={C::activate,f.original[1].identity.transaction_uuid,5,0,false};E error=E::invalid_request;
    switch(fault){case 0:f.request.stable_snapshot=true;break;case 1:f.request.begin_unix_epoch_millis=201;break;
      case 2:f.original[1].rollback_only=true;error=E::state_mismatch;break;
      case 3:f.original[1].identity.scope=mga::TransactionScope::cluster_global;error=E::cluster_requires_authority;break;
      case 4:f.request.local_transaction_id=9;error=E::identity_mismatch;break;
      case 5:f.request.local_transaction_id=8;f.request.transaction_uuid.value=Id(8);error=E::identity_mismatch;break;}
    f.Refuse(error);
  }
}
void Workspaces(){for(unsigned fault=0;fault<9;++fault){Fixture f;auto work=f.Workspace();E expected=E::invalid_workspace;
    switch(fault){case 0:work.entries=f.original;break;
      case 1:work.indices={reinterpret_cast<std::size_t*>(f.original.data()),1};break;
      case 2:work.markers={reinterpret_cast<p::byte*>(f.output.data()),1};break;
      case 3:work.entries=work.entries.first(3);expected=E::resource_exhausted;break;
      case 4:work.indices=work.indices.first(2);expected=E::resource_exhausted;break;
      case 5:work.markers=work.markers.first(2);expected=E::resource_exhausted;break;
      case 6:work.indices={reinterpret_cast<std::size_t*>(reinterpret_cast<p::byte*>(f.indices.data())+1),1};break;
      case 7:work.entries={reinterpret_cast<mga::TransactionInventoryEntry*>(reinterpret_cast<p::byte*>(f.output.data())+1),1};break;
      case 8:work.markers={reinterpret_cast<p::byte*>(&f.request),1};break;}
    const auto before=f.output;forbid_heap=true;const auto result=api::BuildNativeLocalTransactionCandidate(f.source,f.request,work);forbid_heap=false;
    Check(result.error==expected&&!result.ok()&&result.inventory.entries.empty(),"overlap/misalignment/short backing refused before entry writes");
    for(unsigned i=0;i<5;++i)Check(Fields(before[i])==Fields(f.output[i]),"workspace refusal preserves output");
  }
}
}
int main(){try{Allocation();Refusals();Transitions();Workspaces();std::cout<<checks<<" checks\n";return 0;}
  catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
