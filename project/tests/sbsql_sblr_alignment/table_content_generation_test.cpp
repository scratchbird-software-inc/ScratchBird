// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "table_content_generation.hpp"
#include "database_lifecycle.hpp"
#include "local_transaction_store.hpp"
#include "uuid.hpp"
#include "../common/single_tu_allocation_fault.hpp"
#include <algorithm>
#include <filesystem>
#include <iostream>
#include <source_location>
#include <stdexcept>
#include <unistd.h>

namespace {
namespace mga=scratchbird::transaction::mga;
namespace db=scratchbird::storage::database;
namespace uuid=scratchbird::core::uuid;
using Kind=scratchbird::core::platform::UuidKind;
using Id=mga::ContentGenerationUuid;
using Version=mga::TableContentGenerationVersion;
using Rollback=mga::TableContentGenerationRollbackInterval;
using Code=mga::TableContentGenerationSelectionStatus;
unsigned checks=0,failures=0;
void Check(bool ok,const char* why){++checks;if(!ok&&failures++<20)std::cerr<<why<<'\n';}
void Require(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
Id IdOf(unsigned number){Id id{};id[0]=1;id[6]=0x70;id[8]=0x80;id[14]=number>>8;id[15]=number;return id;}
struct Fixture {
  std::filesystem::path root,path;
  Id database;
  mga::LocalTransactionInventory inventory;
  Fixture(){
    std::string pattern=(std::filesystem::temp_directory_path()/"sb_content_generation.XXXXXX").string();
    std::vector<char> writable(pattern.begin(),pattern.end());writable.push_back(0);
    const auto made=::mkdtemp(writable.data());Require(made,"fixture temp directory failed");root=made;
    try {
    path=root/"node.sbdb";
    auto node=uuid::GenerateDurableEngineIdentityV7(Kind::database,1790000000000ull);
    auto space=uuid::GenerateDurableEngineIdentityV7(Kind::filespace,1790000000001ull);
    Require(node.ok()&&space.ok(),"fixture identities failed");database=node.value.value.bytes;
    db::DatabaseCreateConfig c;c.path=path.string();c.database_uuid=node.value;c.filespace_uuid=space.value;
    c.page_size=16384;c.creation_unix_epoch_millis=1790000000002ull;
    c.allow_minimal_resource_bootstrap=true;c.require_resource_seed_pack=false;
    c.require_bootstrap_principal=false;c.allow_uncredentialed_bootstrap=true;
    Require(db::CreateDatabaseFile(c).ok(),"fixture database creation failed");Reload();
    } catch (...) {
      std::error_code error;std::filesystem::remove_all(root,error);throw;
    }
  }
  ~Fixture(){std::error_code e;std::filesystem::remove_all(root,e);}
  void Reload(){auto r=db::LoadLocalTransactionInventoryFromDatabase(path.string());Require(r.ok(),"inventory reload failed");inventory=std::move(r.inventory);}
  void Persist(mga::TransactionInventoryResult r){Require(r.ok(),"MGA transition failed");Require(db::PersistLocalTransactionInventoryToDatabase(path.string(),std::move(r.inventory)).ok(),"inventory persistence failed");Reload();}
  mga::TransactionIdentity Begin(){
    const auto id=uuid::GenerateDurableEngineIdentityV7(Kind::transaction,1790000000100ull+inventory.next_local_transaction_id);
    Require(id.ok(),"transaction identity failed");auto begun=mga::BeginLocalTransaction(inventory,id.value,1790000000200ull+inventory.next_local_transaction_id);
    Require(begun.ok(),"transaction begin failed");auto result=begun.entry.identity;Persist(std::move(begun));return result;
  }
  void Commit(mga::TransactionIdentity t){Persist(mga::CommitLocalTransaction(inventory,t.local_id,1790000001000ull+t.local_id.value));}
  void Abort(mga::TransactionIdentity t){Persist(mga::RollbackLocalTransaction(inventory,t.local_id,1790000001000ull+t.local_id.value));}
};
Version V(const Id& database,Id table,Id generation,Id predecessor,
          mga::TransactionIdentity tx,unsigned seq,unsigned count=1,unsigned ordinal=0){
  Version v;v.database_uuid=database;v.schema_uuid=IdOf(10);v.table_uuid=table;
  v.catalog_row_uuid=IdOf(1000+table[15]);v.generation_uuid=generation;
  v.predecessor_generation_uuid=predecessor;v.root_set_uuid=IdOf(2000+seq*4+ordinal);
  v.statistics_generation_uuid=IdOf(3000+seq*4+ordinal);v.batch_uuid=IdOf(4000+seq);
  v.creator_transaction_uuid=tx.transaction_uuid.value.bytes;v.creator_local_transaction_id=tx.local_id.value;
  v.publication_effect_sequence=seq;v.descriptor_generation=1;v.batch_target_count=count;v.batch_ordinal=ordinal;return v;
}
mga::TableContentGenerationReadContext Read(const Id& db,Id table,mga::u64 bound,mga::TransactionIdentity reader={}){
  mga::TableContentGenerationReadContext c;c.database_uuid=db;c.table_uuid=table;
  c.snapshot.visible_through_local_transaction_id=bound;c.snapshot.visible_through_local_transaction_id_is_boundary=true;
  c.snapshot.reader_transaction=reader.local_id;c.reader_transaction_uuid=reader.transaction_uuid.value.bytes;return c;
}
void Selected(const mga::TableContentGenerationSelection& result,Id expected){
  Check(result.status==Code::selected&&result.binding&&result.binding->generation_uuid==expected,"wrong selected generation");
}
void Refused(const mga::TableContentGenerationSelection& result,Code expected,
             std::source_location at=std::source_location::current()){
  if(result.status!=expected || result.binding || !result.pending_creators.empty())
    std::cerr<<"refusal line="<<at.line()<<" check="<<(checks+1)<<" actual="<<static_cast<int>(result.status)
             <<" expected="<<static_cast<int>(expected)<<" binding="<<result.binding.has_value()<<'\n';
  Check(result.status==expected&&!result.binding&&result.pending_creators.empty(),"failure status/atomic output wrong");
}
void Run(){
  Fixture f;auto birth=f.Begin();const auto a=IdOf(20),b=IdOf(21);
  const auto a0=IdOf(900),b0=IdOf(901),a1=IdOf(800),b1=IdOf(801);
  std::vector<Version> history{V(f.database,a,a0,{},birth,10,2,0),V(f.database,b,b0,{},birth,10,2,1)};
  auto own_birth=Read(f.database,a,0,birth);
  Selected(mga::ResolveTableContentGeneration(own_birth,history,{},f.inventory),a0);
  auto hidden=mga::ResolveTableContentGeneration(Read(f.database,a,0),history,{},f.inventory);
  Check(hidden.status==Code::not_visible&&!hidden.binding&&hidden.pending_creators.size()==1,"uncommitted birth leaked");
  f.Commit(birth);auto truncate=f.Begin();auto reader=f.Begin();
  history.push_back(V(f.database,a,a1,a0,truncate,20,2,0));history.push_back(V(f.database,b,b1,b0,truncate,20,2,1));
  auto own=Read(f.database,a,birth.local_id.value,truncate);
  auto old=Read(f.database,a,birth.local_id.value,reader);
  Selected(mga::ResolveTableContentGeneration(own,history,{},f.inventory),a1);
  {auto isolated=own;isolated.snapshot.allow_reader_own_uncommitted=false;
    const auto result=mga::ResolveTableContentGeneration(isolated,history,{},f.inventory);
    Selected(result,a0);Check(result.pending_creators.size()==1,"disabled own-write visibility lost pending creator");}
  auto independent=mga::ResolveTableContentGeneration(old,history,{},f.inventory);
  Selected(independent,a0);Check(independent.pending_creators.size()==1&&independent.pending_creators.front().transaction_uuid.value.bytes==truncate.transaction_uuid.value.bytes,"pending creator lost");
  own.table_uuid=b;Selected(mga::ResolveTableContentGeneration(own,history,{},f.inventory),b1);own.table_uuid=a;
  f.Commit(truncate);
  Selected(mga::ResolveTableContentGeneration(old,history,{},f.inventory),a0);
  auto latest=Read(f.database,a,f.inventory.next_local_transaction_id-1);
  Selected(mga::ResolveTableContentGeneration(latest,history,{},f.inventory),a1);
  auto rolled=f.Begin();history.push_back(V(f.database,a,IdOf(700),a1,rolled,30));f.Abort(rolled);
  latest.snapshot.visible_through_local_transaction_id=f.inventory.next_local_transaction_id-1;
  Selected(mga::ResolveTableContentGeneration(latest,history,{},f.inventory),a1);
  auto nested=f.Begin();auto a2=IdOf(600),a3=IdOf(500),a4=IdOf(400);
  history.push_back(V(f.database,a,a2,a1,nested,40));history.push_back(V(f.database,a,a3,a2,nested,50));
  own=Read(f.database,a,truncate.local_id.value,nested);
  Selected(mga::ResolveTableContentGeneration(own,history,{},f.inventory),a3);
  Rollback cut;cut.rollback_record_uuid=IdOf(6000);cut.creator_transaction_uuid=nested.transaction_uuid.value.bytes;
  cut.creator_local_transaction_id=nested.local_id.value;cut.effect_sequence_lower_exclusive=40;cut.effect_sequence_upper_inclusive=50;
  std::vector<Rollback> cuts{cut};
  Selected(mga::ResolveTableContentGeneration(own,history,cuts,f.inventory),a2);
  history.push_back(V(f.database,a,a4,a2,nested,60));
  Selected(mga::ResolveTableContentGeneration(own,history,cuts,f.inventory),a4);
  auto overlap=cut;overlap.rollback_record_uuid=IdOf(6001);overlap.effect_sequence_lower_exclusive=45;overlap.effect_sequence_upper_inclusive=55;cuts.push_back(overlap);
  Selected(mga::ResolveTableContentGeneration(own,history,cuts,f.inventory),a4);
  auto outer=cut;outer.rollback_record_uuid=IdOf(6002);outer.effect_sequence_lower_exclusive=0;outer.effect_sequence_upper_inclusive=60;
  Selected(mga::ResolveTableContentGeneration(own,history,{outer},f.inventory),a1);
  Refused(mga::ResolveTableContentGeneration(own,history,{},f.inventory),Code::corrupt_history); // visible fork
  auto malformed=[&](std::vector<Version> h,Code code=Code::corrupt_history){Refused(mga::ResolveTableContentGeneration(own,h,cuts,f.inventory),code);};
  {auto h=history;h.erase(h.begin()+1);malformed(h);}
  {auto h=history;h[1].batch_ordinal=0;malformed(h);}
  {auto h=history;h[1].batch_target_count=1;malformed(h);}
  {auto h=history;h[1].table_uuid=a;malformed(h);}
  {auto h=history;std::swap(h[0],h[1]);malformed(h);}
  {auto h=history;h[2].publication_effect_sequence=10;malformed(h);}
  {auto h=history;h[2].predecessor_generation_uuid=b0;malformed(h);}
  {auto h=history;h[2].generation_uuid=a0;malformed(h);}
  {auto h=history;h[2].catalog_row_uuid=IdOf(5000);malformed(h);}
  {auto h=std::vector<Version>(history.begin(),history.begin()+2);h[1].catalog_row_uuid=h[0].catalog_row_uuid;malformed(h);}
  {auto h=history;h[2].creator_transaction_uuid=birth.transaction_uuid.value.bytes;malformed(h);}
  {auto h=history;h[0].creator_local_transaction_id=999;h[1].creator_local_transaction_id=999;malformed(h,Code::inventory_required);}
  {auto h=history;h.back().predecessor_generation_uuid=IdOf(6553);malformed(h);}
  for(auto member:{&Version::database_uuid,&Version::schema_uuid,&Version::table_uuid,&Version::catalog_row_uuid,&Version::generation_uuid,&Version::root_set_uuid,&Version::statistics_generation_uuid,&Version::batch_uuid,&Version::creator_transaction_uuid}){
    auto h=history;(h[0].*member)[6]=0x40;malformed(h);
    h=history;(h[0].*member)[8]=0;malformed(h);
  }
  {auto invalid=cuts;invalid[0].effect_sequence_lower_exclusive=50;Refused(mga::ResolveTableContentGeneration(own,history,invalid,f.inventory),Code::corrupt_history);}
  {auto invalid=cuts;invalid[0].creator_transaction_uuid=birth.transaction_uuid.value.bytes;Refused(mga::ResolveTableContentGeneration(own,history,invalid,f.inventory),Code::inventory_required);}
  {auto invalid=cuts;invalid.push_back(invalid.front());Refused(mga::ResolveTableContentGeneration(own,history,invalid,f.inventory),Code::corrupt_history);}
  {auto c=own;c.snapshot.visible_through_local_transaction_id_is_boundary=false;Refused(mga::ResolveTableContentGeneration(c,history,cuts,f.inventory),Code::invalid_request);}
  {auto c=own;c.reader_transaction_uuid=birth.transaction_uuid.value.bytes;Refused(mga::ResolveTableContentGeneration(c,history,cuts,f.inventory),Code::inventory_required);}
  {auto c=own;c.snapshot.visible_through_local_transaction_id=f.inventory.next_local_transaction_id;Refused(mga::ResolveTableContentGeneration(c,history,cuts,f.inventory),Code::invalid_request);}
  for(auto state:{mga::TransactionState::limbo,mga::TransactionState::recovering}){
    auto inv=f.inventory;inv.entries.back().state=state;Refused(mga::ResolveTableContentGeneration(own,history,cuts,inv),Code::recovery_required);
  }
  {auto inv=f.inventory;inv.entries.back().identity.scope=mga::TransactionScope::cluster_global;Refused(mga::ResolveTableContentGeneration(own,history,cuts,inv),Code::cluster_authority_required);}
  {auto inv=f.inventory;auto entry=std::find_if(inv.entries.begin(),inv.entries.end(),[&](const auto& e){return e.identity.local_id.value==birth.local_id.value;});
    Require(entry!=inv.entries.end(),"birth inventory entry absent");entry->evidence_record_written=false;
    Refused(mga::ResolveTableContentGeneration(own,history,cuts,inv),Code::inventory_required);}
  {auto inv=f.inventory;inv.entries.push_back(inv.entries.front());Refused(mga::ResolveTableContentGeneration(own,history,cuts,inv),Code::inventory_required);}
  {auto inv=f.inventory;inv.entries.back().state=static_cast<mga::TransactionState>(65535);Refused(mga::ResolveTableContentGeneration(own,history,cuts,inv),Code::inventory_required);}
  // Actual prepared/commit state is persisted and independently reloaded.
  f.Persist(mga::PrepareLocalTransaction(f.inventory,nested.local_id));
  auto waiting=mga::ResolveTableContentGeneration(latest,history,cuts,f.inventory);
  Selected(waiting,a1);Check(waiting.pending_creators.size()==1,"prepared generation lost pending creator");
  f.Commit(nested);latest.snapshot.visible_through_local_transaction_id=f.inventory.next_local_transaction_id-1;
  Selected(mga::ResolveTableContentGeneration(old,history,cuts,f.inventory),a0);
  Selected(mga::ResolveTableContentGeneration(latest,history,cuts,f.inventory),a4);
  for(mga::u64 boundary=0;boundary<f.inventory.next_local_transaction_id;++boundary) {
    const auto result=mga::ResolveTableContentGeneration(Read(f.database,a,boundary),history,cuts,f.inventory);
    if(boundary>=nested.local_id.value)Selected(result,a4);
    else if(boundary>=truncate.local_id.value)Selected(result,a1);
    else if(boundary>=birth.local_id.value)Selected(result,a0);
    else Refused(result,Code::not_visible);
  }
  // Independently enumerate every target in a larger complete publication.
  {
    constexpr unsigned count=128;
    std::vector<Version> batches;
    for(unsigned ordinal=0;ordinal<count;++ordinal)
      batches.push_back(V(f.database,IdOf(100+ordinal),IdOf(10000+ordinal),{},birth,10,count,ordinal));
    for(unsigned ordinal=0;ordinal<count;++ordinal)
      batches.push_back(V(f.database,IdOf(100+ordinal),IdOf(9000+ordinal),IdOf(10000+ordinal),truncate,20,count,ordinal));
    for(unsigned ordinal=0;ordinal<count;++ordinal) {
      auto target=latest;target.table_uuid=IdOf(100+ordinal);
      Selected(mga::ResolveTableContentGeneration(target,batches,{},f.inventory),IdOf(9000+ordinal));
      target.snapshot.visible_through_local_transaction_id=birth.local_id.value;
      Selected(mga::ResolveTableContentGeneration(target,batches,{},f.inventory),IdOf(10000+ordinal));
    }
    batches.pop_back();auto target=latest;target.table_uuid=IdOf(100);
    Refused(mga::ResolveTableContentGeneration(target,batches,{},f.inventory),Code::corrupt_history);
  }
  // A cluster/limbo transaction for another table does not promote this read.
  for(bool cluster:{false,true}) {
    auto h=history;h.push_back(V(f.database,b,IdOf(300),b1,reader,70));
    auto inv=f.inventory;auto entry=std::find_if(inv.entries.begin(),inv.entries.end(),[&](const auto& e){return e.identity.local_id.value==reader.local_id.value;});
    Require(entry!=inv.entries.end(),"independent reader inventory entry absent");
    if(cluster)entry->identity.scope=mga::TransactionScope::cluster_global;else entry->state=mga::TransactionState::limbo;
    Selected(mga::ResolveTableContentGeneration(latest,h,cuts,inv),a4);
    auto other=latest;other.table_uuid=b;
    Refused(mga::ResolveTableContentGeneration(other,h,cuts,inv),cluster?Code::cluster_authority_required:Code::recovery_required);
  }
  {
    auto h=std::vector<Version>(history.begin(),history.begin()+4);
    Rollback both;both.rollback_record_uuid=IdOf(6003);both.creator_transaction_uuid=truncate.transaction_uuid.value.bytes;
    both.creator_local_transaction_id=truncate.local_id.value;both.effect_sequence_lower_exclusive=10;both.effect_sequence_upper_inclusive=20;
    Selected(mga::ResolveTableContentGeneration(latest,h,{both},f.inventory),a0);
    auto other=latest;other.table_uuid=b;Selected(mga::ResolveTableContentGeneration(other,h,{both},f.inventory),b0);
  }
  {auto h=history;h.back().schema_uuid=IdOf(40);Selected(mga::ResolveTableContentGeneration(latest,h,cuts,f.inventory),a4);}
  {auto c=latest;c.table_uuid=IdOf(99);const auto absent=mga::ResolveTableContentGeneration(c,history,cuts,f.inventory);
    Check(absent.status==Code::not_visible&&!absent.binding&&absent.pending_creators.empty(),"unknown table became an empty generation");}
  bool complete=false;unsigned injected=0;
  for(int point=0;point<1024;++point){
    allocations_before_failure=point;auto result=mga::ResolveTableContentGeneration(latest,history,cuts,f.inventory);allocations_before_failure=-1;
    if(result.status==Code::selected){Selected(result,a4);complete=true;break;}
    ++injected;Refused(result,Code::resource_exhausted);
  }
  Check(complete&&injected>10,"allocation sweep did not cover real selection work");
  std::cout<<"generation_selection allocation_points="<<injected<<'\n';
}
}
int main(){try{Run();}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
  std::cout<<"generation_selection checks="<<checks<<" failures="<<failures<<'\n';return failures?1:0;}
