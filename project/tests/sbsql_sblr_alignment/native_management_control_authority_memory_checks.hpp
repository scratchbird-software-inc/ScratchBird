// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
// Included in native conformance fixtures after their Check/Id and fault probes.
// All original independent-image, mutation and recovery fixtures stay active.
namespace control_authority_memory {
namespace m=scratchbird::core::memory;
using C=db::NativeManagementHistoryReadContext;
using H=db::NativeManagementControlAuthorityError;
using M=db::NativeManagementControlAuthorityMemoryError;
void Empty(const db::NativeManagementControlAuthorityView& v){
 Check(!v.ok()&&!v.anchor&&!v.selection&&v.publications.empty()&&v.allocations.empty()&&v.preallocations.empty()&&
  !v.verified_image_bytes&&!v.backing_bytes_used,"failed complete authority has no prefix");
}
void Same(const db::NativeManagementControlGraph& a,const db::NativeManagementControlAuthorityView& b,C context){
 Check(a.error==b.error,"bounded and owning complete control failure parity");
 if(!a.ok()){Empty(b);return;}
 Check(b.ok()&&a.anchor==b.anchor&&a.verified_image_bytes==b.verified_image_bytes&&
  a.publications.size()==b.publications.size()&&a.allocations.size()==b.allocations.size()&&a.preallocations.size()==b.preallocations.size(),
  "all complete authority dimensions anchor and allowance");
 Check(b.selection.has_value()==(context==C::selected),"only actual selected authority carries selection");
 std::size_t n=0;for(const auto& [id,p]:a.publications){const auto& v=b.publications[n++];
  Check(v.first==id&&v.second.page==p.page&&v.second.object_uuid==p.object_uuid&&v.second.sha256==p.sha256&&
   v.second.directory_root==p.directory_root,"every complete publication key checkpoint and directory field");}
 const auto records=[](const auto& owned,const auto& view){std::size_t n=0;for(const auto& [key,value]:owned){
   Check(view[n].first==key&&view[n].second==value,"every exact complete allocation field");++n;}};
 records(a.allocations,b.allocations);records(a.preallocations,b.preallocations);
 for(const auto& [key,value]:a.allocations)Check(db::MatchesNativeManagementControlAllocation(b,key.first,value,
   scratchbird::storage::page::NativeAllocationState::allocated),"retained binary allocation lookup");
 for(const auto& [key,value]:a.preallocations)Check(db::MatchesNativeManagementControlAllocation(b,key.first,value,
   scratchbird::storage::page::NativeAllocationState::preallocated),"retained binary preallocation lookup");
 if(context==C::selected){const auto& selected=static_cast<const db::NativeManagementControlAuthority&>(a);
  const auto x=db::EncodeNativeCheckpointSelection(*selected.selection),y=db::EncodeNativeCheckpointSelection(*b.selection);
  Check(x.ok()&&y.ok()&&x.bytes==y.bytes,"every canonical selection field");}
}
struct Grant {
 m::MemoryManager manager;
 m::HierarchicalMemoryBudgetLedger ledger{3,5};
 db::NativeStorageMemoryBinding binding{Id(1),Id(60011),Id(60012),Id(60013)};
 db::NativeStorageMemory memory;
 static auto Policy(){auto p=m::DefaultLocalEngineMemoryPolicy();
  p.hard_limit_bytes=p.per_context_limit_bytes=512ULL*1024*1024;return p;}
 explicit Grant(u64 bytes):manager(Policy()){
  m::ReservationBackedMemoryResourceRequest request;request.memory_manager=&manager;request.reservation_ledger=&ledger;
  request.consumer_kind=m::ReservationBackedMemoryConsumerKind::background_maintenance;
  request.category=m::MemoryCategory::page_buffer;request.requested_bytes=bytes;request.memory_class="page_buffer";
  request.route_label="storage.authority.conformance";request.purpose="complete native control authority source";
  request.binary_operation_uuid=binding.operation_uuid.bytes;
  request.binary_ownership[m::MemoryBinaryScopeKind::database]=binding.database_uuid.bytes;
  request.binary_ownership[m::MemoryBinaryScopeKind::owner]=binding.owner_uuid.bytes;
  request.binary_ownership[m::MemoryBinaryScopeKind::context]=binding.context_uuid.bytes;
  request.scope_chain={{m::HierarchicalMemoryScopeKind::process,{},Id(60014).bytes},
    {m::HierarchicalMemoryScopeKind::database,{},binding.database_uuid.bytes}};
  request.provenance.source=m::HierarchicalMemoryBudgetProvenanceSource::server_runtime_api;
  request.provenance.source_label="actual complete control authority backing conformance";
  for(const auto& scope:request.scope_chain){m::HierarchicalMemoryBudget budget;budget.scope=scope;
   budget.hard_limit_bytes=bytes;budget.provenance=request.provenance;Check(ledger.SetBudget(budget).ok(),"actual history parent limit");}
  auto grant=m::AcquireReservationBackedMemoryResource(request);Check(grant.ok(),"actual control authority grant issued");
  auto adopted=db::AdoptNativeStorageMemory(binding,grant.resource);Check(adopted.ok()&&!grant.resource,"exclusive binary control authority grant adoption");
  memory=std::move(adopted.memory);
 }
 void Empty(){const auto state=manager.Snapshot();Check(!state.current_bytes&&!state.reserved_capacity_bytes&&
  !state.active_capacity_reservation_count&&!ledger.Snapshot().current_bytes,"all control authority memory charges released");}
};
void Checks(const std::vector<d::NativeFilespaceDevice>& files,const db::NativeManagementControlGraph& expected,u64 allowance,
 C context=C::selected,const db::NativeManagementCheckpointAnchor* anchor=nullptr,
 const std::map<Uuid,Bytes>* historical=nullptr,bool deep=false){
 Check(expected.ok(),"control authority memory fixture has independently verified complete source");
 std::vector<db::NativeManagementHistoricalPageZero> zeros;
 if(historical)for(const auto& [id,bytes]:*historical)zeros.push_back({id,bytes});
 const auto capacity=static_cast<std::size_t>(expected.verified_image_bytes*4+1024*1024);
 Bytes backing(capacity);std::vector<std::unique_ptr<d::FileDevice::ReadLatencyBatch>> owners;
 std::vector<d::FileDevice::ReadLatencyBatch*> observations;
 for(const auto& file:files){owners.push_back(std::make_unique<d::FileDevice::ReadLatencyBatch>(*file.device));observations.push_back(owners.back().get());}
 const auto database=Id(1),primary=Id(2);
 const auto direct=[&](std::span<byte> region,bool deny=true){
  if(deny)allocation_budget=0;
  auto result=db::ReadNativeManagementControlAuthorityInto(database,files,primary,allowance,context,anchor,zeros,observations,region);
  const bool untouched=allocation_budget==0;if(deny)allocation_budget=-1;
  if(deny)Check(untouched,"complete actual history read has no payload or metadata heap fallback");
  return result;
 };
 const auto full=direct(backing);Same(expected,full.authority,context);const auto used=full.authority.backing_bytes_used;
 Check(used&&used<=capacity&&full.io_status.ok()&&full.physical_bytes_read,"actual complete read observations");
 Same(expected,direct(std::span(backing).first(used)).authority,context);
 const auto short_read=direct(std::span(backing).first(used-1));
 Check(short_read.authority.error==H::resource_exhausted,"one byte short complete history refuses");Empty(short_read.authority);
 if(deep){
  const auto invalid_region=[&](auto region){const auto r=direct(region);
   Check(r.authority.error==H::invalid_workspace&&!r.physical_bytes_read,"whole-input alias refuses before I/O or scratch writes");Empty(r.authority);};
  invalid_region(std::span<byte>{reinterpret_cast<byte*>(const_cast<Uuid*>(&database)),sizeof(database)});
  invalid_region(std::span<byte>{reinterpret_cast<byte*>(const_cast<d::NativeFilespaceDevice*>(files.data())),files.size()*sizeof(files.front())});
  invalid_region(std::span<byte>{reinterpret_cast<byte*>(files.front().device),sizeof(d::FileDevice)});
  invalid_region(std::span<byte>{reinterpret_cast<byte*>(observations.data()),observations.size()*sizeof(observations.front())});
  invalid_region(std::span<byte>{reinterpret_cast<byte*>(observations.front()),sizeof(*observations.front())});
  if(anchor)invalid_region(std::span<byte>{reinterpret_cast<byte*>(const_cast<db::NativeManagementCheckpointAnchor*>(anchor)),sizeof(*anchor)});
  if(!zeros.empty()){
   invalid_region(std::span<byte>{reinterpret_cast<byte*>(zeros.data()),zeros.size()*sizeof(zeros.front())});
   invalid_region(std::span<byte>{const_cast<byte*>(zeros.front().image.data()),zeros.front().image.size()});}
  d::FileDevice foreign;d::FileDevice::ReadLatencyBatch wrong(foreign);const auto saved=observations.front();observations.front()=&wrong;
  const auto rejected=direct(backing);observations.front()=saved;
  Check(rejected.authority.error==H::invalid_request&&!rejected.physical_bytes_read,"foreign batch cannot redirect reads");Empty(rejected.authority);
 }
 Grant grant(capacity);const db::NativeManagementControlAuthorityMemoryLimits limits{allowance,capacity};
 const auto call=[&](const db::NativeStorageMemoryBinding& binding){
  return db::ReadNativeManagementControlAuthorityWithMemoryFromOpenDevices(database,files,primary,limits,grant.memory,binding,context,anchor,zeros);};
 auto result=call(grant.binding);Check(result.ok(),"actual binary memory grant backs complete native control authority");Same(expected,result.authority,context);
 Check(result.physical_bytes_read==full.physical_bytes_read&&result.io_status.ok(),"actual grant preserves exact physical observations");
 Check(grant.manager.Snapshot().current_bytes==capacity&&grant.ledger.Snapshot().current_bytes==capacity&&
  grant.memory.Snapshot().allocated_bytes==capacity&&result.arena.Snapshot().retained_bytes==capacity,"every history ledger charges the whole retained arena");
 const auto failed=[&](const auto& r){Check(!r.ok()&&!r.arena,"no failed history owner");Empty(r.authority);};
 reads=0;io_counting=true;auto exhausted=call(grant.binding);io_counting=false;
 failed(exhausted);Check(exhausted.error==M::memory_allocation_failure&&!reads,"held history prevents uncharged concurrent allocation");
 for(unsigned field=0;field<4;++field){auto wrong=grant.binding;const std::array<Uuid*,4> ids{&wrong.database_uuid,&wrong.operation_uuid,&wrong.owner_uuid,&wrong.context_uuid};*ids[field]=Id(60019);
  reads=0;io_counting=true;const auto r=call(wrong);io_counting=false;failed(r);
  Check(r.error==M::memory_binding_failure&&!reads,"all four binary control authority grant identities checked before effects");}
 Check(grant.ledger.CleanupOwner(grant.binding.owner_uuid.bytes).retained_bytes==capacity,"history revocation retains live payload charges");
 if(deep){
  std::vector<std::pair<std::string,bool>> paths;
  for(const auto& file:files){const auto fence=file.device->AcquireOperationGuard();paths.emplace_back(file.device->path(),file.device->read_only());}
  for(const auto& file:files)Check(file.device->Close().ok(),"close all source files while complete history remains owned");
  Same(expected,result.authority,context);
  for(std::size_t i=0;i<files.size();++i)Check(files[i].device->Open(paths[i].first,
    paths[i].second?d::FileOpenMode::open_existing_read_only:d::FileOpenMode::open_existing).ok(),"reopen exact source routes after retained history");
 }
 const auto revoked=call(grant.binding);failed(revoked);Check(revoked.error==M::memory_binding_failure,"revoked control authority grants refuse new work");
 grant.memory={};Same(expected,result.authority,context);
 Check(grant.manager.Snapshot().current_bytes==capacity,"complete retained history survives workspace destruction");
 result={};grant.Empty();
 if(!deep)return;
 Grant valid(capacity);
 const auto retained_grant_bytes=valid.ledger.Snapshot().current_bytes;
 const auto released_payload=[&]{const auto state=valid.memory.Snapshot();
  Check(!valid.manager.Snapshot().current_bytes&&!state.allocated_bytes&&
   state.allocation_count==state.release_count&&valid.ledger.Snapshot().current_bytes==retained_grant_bytes,
   "all failed payload allocations released while the reusable grant remains reserved");};
 const auto granted=[&]{return db::ReadNativeManagementControlAuthorityWithMemoryFromOpenDevices(database,files,primary,limits,valid.memory,valid.binding,context,anchor,zeros);};
 reads=writes=syncs=0;history_observed_read_bytes=0;io_counting=true;hash_seen=0;hash_counting=true;
 {const auto r=granted();Check(r.ok(),"all physical history fault sites baseline");}
 io_counting=false;hash_counting=false;const auto read_sites=reads,hash_sites=hash_seen;
 Check(read_sites&&hash_sites&&!writes&&!syncs,"native history inspection never writes or synchronizes");
 for(unsigned site=1;site<=read_sites;++site){reads=0;history_observed_read_bytes=0;read_fault=site;io_counting=true;const auto r=granted();io_counting=false;read_fault=0;
  failed(r);Check(r.authority_error==H::io_failure&&!r.io_status.ok()&&
   r.io_diagnostic.diagnostic_code=="SB-STORAGE-DISK-READ-SHORT"&&r.physical_bytes_read==history_observed_read_bytes,
   "complete history preserves every actual read diagnostic and independent transferred-byte total");
  released_payload();}
 for(unsigned mode=1;mode<=5;++mode)for(unsigned site=1;site<=hash_sites;++site){
  hash_fault=mode;hash_target=site;hash_seen=0;hash_active=false;const auto r=granted();
  const bool consumed=!hash_fault;hash_fault=0;hash_active=false;failed(r);
  Check(consumed&&r.authority_error==H::hash_failure,"every full-control-authority provider failure retains exact typed refusal");
  released_payload();}
#ifdef NATIVE_HISTORICAL_BUNDLE_SIZE_FAULTS
 historical_stats=0;historical_stat_counting=true;{const auto r=granted();Check(r.ok(),"complete history extent fault baseline");}
 historical_stat_counting=false;const auto extent_sites=historical_stats;Check(extent_sites,"actual history size observations");
 for(unsigned site=1;site<=extent_sites;++site){
  historical_stats=0;historical_stat_fault=site;historical_stat_counting=true;
  history_observed_read_bytes=0;reads=0;io_counting=true;const auto r=granted();
  io_counting=false;historical_stat_counting=false;historical_stat_fault=0;failed(r);
  Check(r.authority_error==H::io_failure&&!r.io_status.ok()&&r.physical_bytes_read==history_observed_read_bytes&&
   r.io_diagnostic.diagnostic_code=="SB-STORAGE-DISK-SIZE-FAILED","every actual extent refusal retains backend diagnostic and preceding reads");
  released_payload();
 }
 for(const auto& file:files){
  std::recursive_mutex* mutex=nullptr;{auto guard=file.device->AcquireOperationGuard();mutex=guard.mutex();}
  memory_probes=memory_locked_probes=0;memory_probe_mutex=mutex;
  {const auto r=granted();memory_probe_mutex=nullptr;Check(r.ok(),"history ownership lifetime fence probe");memory_probe_mutex=mutex;}
  memory_probe_mutex=nullptr;Check(memory_probes&&!memory_locked_probes,"allocation observations and cleanup remain outside every source fence");
  memory_probes=memory_locked_probes=0;memory_probe_backing_only=true;memory_probe_mutex=mutex;
  reads=0;read_fault=1;io_counting=true;const auto failed_read=granted();io_counting=false;read_fault=0;
  memory_probe_mutex=nullptr;memory_probe_backing_only=false;failed(failed_read);
  Check(memory_probes&&!memory_locked_probes,"failed whole-control authority backing releases only after every source fence");
  std::string path;bool readonly=false;{const auto guard=file.device->AcquireOperationGuard();path=file.device->path();readonly=file.device->read_only();}
  historical_read_pause=1;
  auto reader=std::async(std::launch::async,[&]{return granted();});
  const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
  while(historical_read_pause!=2&&std::chrono::steady_clock::now()<deadline)std::this_thread::yield();
  const bool entered=historical_read_pause==2;
  // Raw fixture handles retain a thread-affine route guard from Open. Close
  // stays on that opening thread; the concurrent reader runs on the worker.
  // Runtime cross-thread ownership uses NativeOwnedCheckpointSource adoption.
  std::promise<void> started;auto ready=started.get_future();std::atomic<bool> close_returned=false;
  bool blocked=false;
  auto release=std::async(std::launch::async,[&]{ready.wait();std::this_thread::sleep_for(std::chrono::milliseconds(30));
    blocked=!close_returned.load();historical_read_pause=3;});
  started.set_value();const auto closed=file.device->Close();close_returned=true;
  release.get();auto retained=reader.get();historical_read_pause=0;
  Check(entered&&blocked&&closed.ok()&&retained.ok(),"Close on each participating file waits for complete grant-backed history");
  Same(expected,retained.authority,context);
  Check(file.device->Open(path,readonly?d::FileOpenMode::open_existing_read_only:d::FileOpenMode::open_existing).ok(),"restore exact source after concurrent Close");
  retained={};released_payload();
 }
#endif
 valid.memory={};valid.Empty();
 Grant short_grant(capacity-1);reads=0;io_counting=true;
 const auto denied=db::ReadNativeManagementControlAuthorityWithMemoryFromOpenDevices(database,files,primary,limits,short_grant.memory,short_grant.binding,context,anchor,zeros);
 io_counting=false;failed(denied);Check(denied.error==M::memory_allocation_failure&&!reads,"one byte short actual control authority grant refuses before source reads");
 short_grant.memory={};short_grant.Empty();
}
} // namespace control_authority_memory
