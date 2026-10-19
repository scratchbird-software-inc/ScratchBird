// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "engine/internal_api/transaction/native_startup_transaction_history.hpp"
#include <array>
#include <cstdlib>
#include <iostream>
#include <new>
#include <source_location>
#include <stdexcept>
namespace api=scratchbird::engine::internal_api;
namespace mga=scratchbird::transaction::mga;
namespace db=scratchbird::storage::database;
namespace page=scratchbird::storage::page;
namespace p=scratchbird::core::platform;
static bool forbid_heap=false;
void* operator new(std::size_t n){if(forbid_heap)std::abort();if(auto* out=std::malloc(n?n:1))return out;throw std::bad_alloc();}
void* operator new[](std::size_t n){return ::operator new(n);}
void operator delete(void* p)noexcept{std::free(p);}void operator delete[](void* p)noexcept{std::free(p);}
void operator delete(void* p,std::size_t)noexcept{std::free(p);}void operator delete[](void* p,std::size_t)noexcept{std::free(p);}
namespace {
using E=api::NativeStartupTransactionHistoryError;using O=api::NativeStartupInventoryOutcome;using S=mga::TransactionState;
unsigned checks=0;
void Check(bool good,const char* why,std::source_location where=std::source_location::current()){
  ++checks;if(!good)throw std::runtime_error(std::string(why)+" line="+std::to_string(where.line()));}
p::Uuid Id(unsigned n){p::Uuid id;id.bytes[0]=1;id.bytes[6]=0x70;id.bytes[8]=0x80;
  id.bytes[12]=p::byte(n>>24);id.bytes[13]=p::byte(n>>16);id.bytes[14]=p::byte(n>>8);id.bytes[15]=p::byte(n);return id;}
struct Fixture {
  std::array<mga::TransactionInventoryEntry,2> entries;
  page::NativeTransactionInventoryConstView inventory;
  db::NativeCheckpointSelection selection;
  db::NativePublicationSnapshot publication;
  std::array<p::byte,64> request{};
  std::array<db::NativeManagementHistoryEntryView,3> records;
  std::array<db::NativeManagementHistoryLatest,2> latest;
  db::NativeManagementHistoryView history;
  alignas(std::max_align_t) std::array<p::byte,4096> scratch;
  Fixture(){
    selection.header.database_uuid=Id(100);selection.header.filespace_uuid=Id(101);selection.object_uuid=Id(102);
    selection.bootstrap_uuid=Id(103);selection.publication_uuid=Id(104);selection.timeline_uuid=Id(105);
    selection.selection_generation=selection.checkpoint_generation=selection.root_set_generation=3;
    selection.checkpoint={Id(101),10,3,Id(106)};selection.checkpoint_object_uuid=Id(107);selection.checkpoint_sha256[7]=19;
    selection.previous_selection_generation=2;selection.previous_checkpoint={Id(101),9,2,Id(106)};
    selection.previous_checkpoint_object_uuid=Id(108);selection.previous_checkpoint_sha256[7]=18;
    publication.selection=selection;publication.watermark.header=selection.header;publication.watermark.bootstrap_uuid=selection.bootstrap_uuid;
    publication.watermark.timeline_uuid=selection.timeline_uuid;publication.watermark.watermark=3;
    for(unsigned n=0;n<2;++n){auto& e=entries[n];const unsigned local=2*n+1;
      e.identity={{local},{p::UuidKind::transaction,Id(local)},mga::TransactionScope::local_node};e.state=n?S::created:S::committed;
      e.begin_visible_through_local_transaction_id=local-1;if(!n){e.commit_sequence=1;e.evidence_record_written=true;}}
    inventory={4,3,entries};
    for(unsigned n=0;n<3;++n){auto& r=records[n];const bool second=n==2;
      r.record.uuid=Id(second?30:20);r.record.database_uuid=selection.header.database_uuid;r.record.revision=n==1?2:1;
      r.record.normalized_request_bytes=request;
      r.plan.intent.startup_binding=db::NativeStartupBinding{r.record.uuid,Id(second?31:21),Id(second?3:1),second?3u:1u,second?8u:7u};
    }
    latest={db::NativeManagementHistoryLatest{Id(20),1},db::NativeManagementHistoryLatest{Id(30),2}};
    history.error=db::NativeManagementHistoryError::none;history.anchor.emplace();history.selection=selection;history.entries=records;history.latest=latest;
  }
  auto Run(std::span<p::byte> backing){forbid_heap=true;auto r=api::InspectNativeStartupTransactionHistory(history,selection,inventory,publication,backing);forbid_heap=false;return r;}
  auto Run(){return Run(scratch);}
  void Refuse(E error){auto r=Run();Check(!r.ok()&&r.error==error&&!r.selection&&r.transactions.empty()&&!r.pending_binding,
    "failure exposes no selected history or partial transaction prefix");}
};
void Outcomes(){Fixture f;auto r=f.Run();Check(r.ok()&&r.transactions.size()==2&&!r.pending_binding&&r.workspace_bytes<=f.scratch.size(),"complete bounded latest-operation join");
  Check(r.transactions[0].binding==*f.records[1].plan.intent.startup_binding&&r.transactions[0].management_revision==2&&
    r.transactions[0].outcome==O::committed&&r.transactions[0].commit_sequence==1&&r.transactions[0].inventory_state==S::committed,
    "selected history uses latest revision and actual commit sequence");
  Check(r.transactions[1].binding==*f.records[2].plan.intent.startup_binding&&r.transactions[1].outcome==O::unresolved,
    "created inventory never masquerades as a completed startup");
  for(unsigned n=1;n<=13;++n){Fixture one;auto& row=one.entries[1];row.state=static_cast<S>(n);
    O expected=O::unresolved;
    if(row.state==S::committed){row.commit_sequence=2;expected=O::committed;}
    if(row.state==S::rolled_back)expected=O::rolled_back;
    if(row.state==S::failed_terminal)expected=O::failed_terminal;
    if(row.state==S::archived){row.archived_from_state=S::rolled_back;expected=O::rolled_back;}
    auto result=one.Run();Check(result.ok()&&result.transactions[1].inventory_state==row.state&&result.transactions[1].outcome==expected,
      "all retained inventory states have exact observation semantics without recovery authority");
  }
  Fixture cluster;cluster.entries[1].identity.scope=mga::TransactionScope::cluster_global;
  auto global=cluster.Run();Check(global.ok()&&global.transactions[1].outcome==O::cluster_owned,"local startup cannot settle cluster-owned work");
  Fixture missing;missing.inventory.entries=missing.inventory.entries.first(1);
  auto unknown=missing.Run();Check(unknown.ok()&&unknown.transactions[1].outcome==O::missing&&unknown.transactions[1].inventory_state==S::none,
    "a missing historical transaction is unknown, never evidence that no effects occurred");
  Fixture empty;empty.history.entries={};empty.history.latest={};auto none=empty.Run();Check(none.ok()&&none.transactions.empty(),"genesis does not invent a startup operation");
  Fixture unbound;for(auto& entry:unbound.records)entry.plan.intent.startup_binding.reset();
  auto other=unbound.Run();Check(other.ok()&&other.transactions.empty(),"ordinary management records have no inferred startup binding");
}
void Pending(){Fixture f;f.publication.watermark.watermark=4;f.publication.watermark.operation_uuid=Id(900);
  f.publication.watermark.intent.emplace();f.publication.watermark.intent->recovery_profile=2;
  f.publication.watermark.intent->startup_binding=db::NativeStartupBinding{Id(40),Id(41),Id(4),4,9};
  auto result=f.Run();Check(result.ok()&&result.transactions.size()==2&&result.pending_binding==f.publication.watermark.intent->startup_binding&&
    result.pending_publication_uuid==Id(900)&&result.pending_publication_generation==4&&f.inventory.next_local_transaction_id==4&&f.inventory.entries.size()==2,
    "unselected intent survives even before inventory allocation or management extent publication");
  f.publication.watermark.intent->startup_binding->local_transaction_id=3;f.Refuse(E::identity_mismatch);
  f.publication.watermark.intent->startup_binding->transaction_uuid=Id(3);Check(f.Run().ok(),"pending activation retains its already allocated transaction");
  f.publication.watermark.intent->recovery_profile=1;f.Refuse(E::invalid_binding);
  f.publication.watermark.intent->recovery_profile=2;f.publication.watermark.abandonment.emplace();f.Refuse(E::invalid_binding);
}
void Bindings(){for(unsigned fault=0;fault<18;++fault){Fixture f;E error=E::invalid_history;
    switch(fault){case 0:f.history.selection.reset();error=E::invalid_source;break;
      case 1:f.latest[1].entry=99;break;case 2:f.latest[0].entry=0;break;
      case 3:std::swap(f.latest[0],f.latest[1]);break;case 4:f.latest[1].operation_uuid=f.latest[0].operation_uuid;break;
      case 5:f.records[0].plan.intent.startup_binding.reset();break;
      case 6:f.records[1].plan.intent.startup_binding->session_uuid=Id(800);break;
      case 7:f.records[2].plan.intent.startup_binding->operation_uuid=Id(800);error=E::invalid_binding;break;
      case 8:f.records[2].record.normalized_request_bytes={};error=E::invalid_binding;break;
      case 9:f.records[2].plan.intent.startup_binding->fence_generation=0;error=E::invalid_binding;break;
      case 10:f.records[2].plan.intent.startup_binding->local_transaction_id=UINT64_MAX;error=E::invalid_binding;break;
      case 11:f.records[2].plan.intent.startup_binding->transaction_uuid=Id(800);error=E::identity_mismatch;break;
      case 12:f.records[2].plan.intent.startup_binding->local_transaction_id=2;error=E::identity_mismatch;break;
      case 13:f.entries[1].identity.transaction_uuid=f.entries[0].identity.transaction_uuid;error=E::invalid_inventory;break;
      case 14:f.entries[1].identity.local_id.value=1;error=E::invalid_inventory;break;
      case 15:f.inventory.next_commit_sequence=0;error=E::invalid_inventory;break;
      case 16:f.records[2].record.database_uuid=Id(800);break;
      case 17:f.publication.watermark.watermark=2;error=E::source_mismatch;break;}
    f.Refuse(error);
  }
}
void Sources(){for(unsigned field=0;field<16;++field){Fixture f;auto& a=f.selection;
    switch(field){case 0:a.header.database_uuid=Id(700);break;case 1:a.header.filespace_uuid=Id(700);break;
      case 2:a.object_uuid=Id(700);break;case 3:a.bootstrap_uuid=Id(700);break;case 4:a.publication_uuid=Id(700);break;
      case 5:++a.selection_generation;break;case 6:++a.checkpoint.page_number;break;case 7:a.checkpoint_object_uuid=Id(700);break;
      case 8:++a.checkpoint_sha256[0];break;case 9:++a.checkpoint_generation;break;case 10:++a.root_set_generation;break;
      case 11:a.timeline_uuid=Id(700);break;case 12:++a.previous_selection_generation;break;case 13:a.previous_checkpoint.reset();break;
      case 14:a.previous_checkpoint_object_uuid=Id(700);break;case 15:++a.previous_checkpoint_sha256[0];break;}
    f.Refuse(E::source_mismatch);
  }
  Fixture f;f.publication.selection.checkpoint_sha256[0]=33;f.Refuse(E::source_mismatch);
  Fixture w;w.publication.watermark.bootstrap_uuid=Id(700);w.Refuse(E::source_mismatch);
}
void Memory(){Fixture f;auto valid=f.Run();Check(valid.ok(),"sizing observation");
  Check(f.Run(std::span{f.scratch}.first(valid.workspace_bytes)).ok(),"exact bounded backing");
  auto short_result=f.Run(std::span{f.scratch}.first(valid.workspace_bytes-1));
  Check(short_result.error==E::resource_exhausted&&!short_result.selection&&short_result.transactions.empty(),"short scratch returns no prefix");
  for(auto input:{std::span<p::byte>{reinterpret_cast<p::byte*>(f.entries.data()),1},
      std::span<p::byte>{reinterpret_cast<p::byte*>(f.latest.data()),1},std::span<p::byte>{f.request.data(),1},
      std::span<p::byte>{reinterpret_cast<p::byte*>(&f.selection),1},std::span<p::byte>{reinterpret_cast<p::byte*>(&f.publication),1}}){
    auto refusal=f.Run(input);Check(refusal.error==E::invalid_workspace&&refusal.transactions.empty(),"scratch cannot overlap any retained source");
  }
}
}
int main(){try{Outcomes();Pending();Bindings();Sources();Memory();std::cout<<checks<<" checks\n";return 0;}
  catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
