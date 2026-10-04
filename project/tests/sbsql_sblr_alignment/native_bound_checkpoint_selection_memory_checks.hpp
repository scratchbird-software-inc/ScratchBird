// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
// Fixture-only: shared independent source oracles remain in each owning test.
namespace bound_selection_memory {
namespace d=scratchbird::storage::disk;
namespace c=checkpoint_inventory_memory;
using E=db::NativeCheckpointSelectionError;
using M=db::NativeBoundCheckpointSelectionMemoryError;
void Empty(const db::NativeBoundCheckpointSelectionView& v){
 Check(!v.ok()&&!v.selection&&v.slots[0].empty()&&v.slots[1].empty()&&
  !v.checkpoint_inventory.checkpoint&&!v.predecessor.checkpoint&&v.allocation.pages.empty()&&
  !v.retained_image_bytes&&!v.backing_bytes_used,"failed full selector has no usable prefix");
 c::Empty(v.checkpoint_inventory);c::Empty(v.predecessor);
}
void Same(const db::NativeBoundCheckpointSelection& a,const db::NativeBoundCheckpointSelectionView& b){
 Check(a.checkpoint_inventory.inventory_error==b.checkpoint_inventory.inventory_error,"complete selector concrete inventory failure parity");
 Check(a.error==b.error&&a.checkpoint_error==b.checkpoint_error&&a.allocation_error==b.allocation_error,
  "complete selector and nested failure parity");
 if(!a.ok()){Empty(b);return;}
 Check(b.ok()&&a.retained_image_bytes==b.retained_image_bytes,"complete original selector verification allowance");
 const auto x=db::EncodeNativeCheckpointSelection(*a.selection),y=db::EncodeNativeCheckpointSelection(*b.selection);
 Check(x.ok()&&y.ok()&&x.bytes==y.bytes,"every canonical selected field");
 for(unsigned n=0;n<2;++n)Check(std::equal(a.slots[n].begin(),a.slots[n].end(),b.slots[n].begin(),b.slots[n].end()),"complete actual selector image bytes");
 c::Same(a.checkpoint_inventory,b.checkpoint_inventory);c::Same(a.predecessor,b.predecessor);
 Check(a.allocation.error==b.allocation.error&&a.allocation.state_counts==b.allocation.state_counts&&
  a.allocation.retained_image_bytes==b.allocation.retained_image_bytes&&a.allocation.pages.size()==b.allocation.pages.size(),
  "all selected allocation dimensions and counts");
 for(std::size_t n=0;n<a.allocation.pages.size();++n){
  const auto& old=a.allocation.pages[n];const auto& now=b.allocation.pages[n];const auto& p=*old.map;const auto& q=now.map;
  const auto ph=d::EncodeNativeCommonPageHeader(p.header),qh=d::EncodeNativeCommonPageHeader(q.header);
  Check(ph.ok()&&qh.ok()&&ph.bytes==qh.bytes&&p.object_uuid==q.object_uuid&&p.map_generation==q.map_generation&&
   p.capacity_generation==q.capacity_generation&&p.total_pages==q.total_pages&&p.first_page==q.first_page&&
   p.creator_transaction_uuid==q.creator_transaction_uuid&&p.creator_local_transaction_id==q.creator_local_transaction_id&&
   p.creator_operation_uuid==q.creator_operation_uuid&&p.next==q.next&&p.next_sha256==q.next_sha256&&
   std::equal(p.states.begin(),p.states.end(),q.states.begin(),q.states.end())&&
   std::equal(p.records.begin(),p.records.end(),q.records.begin(),q.records.end())&&
   std::equal(old.bytes.begin(),old.bytes.end(),now.image.begin(),now.image.end()),
   "every allocation header record state creator successor and physical image");
 }
}
using Grant=c::Grant;
void Checks(std::span<const d::NativeFilespaceDevice> files,const db::NativeBoundCheckpointSelection& expected,u64 allowance,bool deep=false,int fault_route=-1,unsigned fault_shard=0,unsigned fault_shards=1){
 const auto database=Id(1),primary=Id(2);
 const auto backing_capacity=static_cast<std::size_t>((expected.ok()?expected.retained_image_bytes:std::min<u64>(allowance,16*1024*1024))*4+1024*1024);
 Bytes backing(backing_capacity);std::vector<std::unique_ptr<d::FileDevice::ReadLatencyBatch>> owners;
 std::vector<d::FileDevice::ReadLatencyBatch*> observations;
 for(const auto& file:files){owners.push_back(std::make_unique<d::FileDevice::ReadLatencyBatch>(*file.device));observations.push_back(owners.back().get());}
 const auto direct=[&](std::span<byte> region){allocation_budget=0;
  auto result=db::ReadNativeBoundCheckpointSelectionInto(database,files,primary,allowance,observations,region);
  const bool untouched=allocation_budget==0;allocation_budget=-1;
  Check(untouched,"complete actual selector has no ordinary or aligned heap fallback");return result;};
 const auto full=direct(backing);Same(expected,full.selection);if(!expected.ok())return;
 const auto used=full.selection.backing_bytes_used;Check(used&&used<=backing_capacity&&full.io_status.ok()&&full.physical_bytes_read,"bounded selector backing and observations");
 Same(expected,direct(std::span(backing).first(used)).selection);
 const auto short_read=direct(std::span(backing).first(used-1));
 Check(short_read.selection.error==E::resource_exhausted,"one byte short full selector backing refuses");Empty(short_read.selection);
 const auto alias=[&](std::span<byte> bytes){const auto r=direct(bytes);
  Check(r.selection.error==E::invalid_backing&&!r.physical_bytes_read,"whole input alias refuses before source effects");Empty(r.selection);};
 alias({reinterpret_cast<byte*>(const_cast<Uuid*>(&database)),sizeof(database)});
 alias({reinterpret_cast<byte*>(const_cast<Uuid*>(&primary)),sizeof(primary)});
 alias({reinterpret_cast<byte*>(const_cast<d::NativeFilespaceDevice*>(files.data())),files.size()*sizeof(files.front())});
 alias({reinterpret_cast<byte*>(files.front().device),sizeof(d::FileDevice)});
 alias({reinterpret_cast<byte*>(observations.data()),observations.size()*sizeof(observations.front())});
 alias({reinterpret_cast<byte*>(observations.front()),sizeof(*observations.front())});
 d::FileDevice foreign;d::FileDevice::ReadLatencyBatch wrong(foreign);auto* saved=observations.front();observations.front()=&wrong;
 const auto denied=direct(backing);observations.front()=saved;
 Check(denied.selection.error==E::invalid_filespace&&!denied.physical_bytes_read,"exact-device selector observations required");Empty(denied.selection);
 // The image allowance deliberately counts repeated proof work. It is not
 // physical backing demand. Use the measured complete decoded workspace plus
 // the wrapper's batch storage/alignment, retaining the real 512 MiB policy.
 using Batch=d::FileDevice::ReadLatencyBatch;
 const auto capacity=used+files.size()*(sizeof(Batch)+sizeof(Batch*)+alignof(Batch))+
  2*alignof(std::max_align_t);
 Grant grant(capacity);const db::NativeBoundCheckpointSelectionMemoryLimits limits{allowance,capacity};
 const auto call=[&](const db::NativeStorageMemoryBinding& binding){
  return db::ReadNativeBoundCheckpointSelectionWithMemoryFromOpenDevices(database,files,primary,limits,grant.memory,binding);};
 auto result=call(grant.binding);Check(result.ok(),"actual binary memory grant backs complete selector");Same(expected,result.selection);
 Check(result.physical_bytes_read==full.physical_bytes_read&&result.io_status.ok(),"actual grant exact transferred-byte receipt");
 Check(grant.manager.Snapshot().current_bytes==capacity&&grant.ledger.Snapshot().current_bytes==capacity&&
  grant.memory.Snapshot().allocated_bytes==capacity&&result.arena.Snapshot().retained_bytes==capacity,"whole selector retained payload charged at every scope");
 const auto failed=[&](const auto& r){Check(!r.ok()&&!r.arena,"failed selector exposes no retained owner");Empty(r.selection);};
 const auto exhausted=call(grant.binding);failed(exhausted);
 Check(exhausted.error==M::memory_allocation_failure&&!exhausted.physical_bytes_read,"live result prevents uncharged concurrent source work");
 for(unsigned i=0;i<4;++i){auto wrong=grant.binding;const std::array<Uuid*,4> ids{&wrong.database_uuid,&wrong.operation_uuid,&wrong.owner_uuid,&wrong.context_uuid};
  *ids[i]=Id(219);const auto r=call(wrong);failed(r);Check(r.error==M::memory_binding_failure&&!r.physical_bytes_read,"every binary grant identity admitted before I/O");}
 Check(grant.ledger.CleanupOwner(grant.binding.owner_uuid.bytes).retained_bytes==capacity,"owner revocation keeps complete result charged");
 if(deep){std::vector<std::pair<std::string,bool>> paths;
  for(const auto& file:files){auto fence=file.device->AcquireOperationGuard();paths.emplace_back(file.device->path(),file.device->read_only());}
  for(const auto& file:files)Check(file.device->Close().ok(),"close all source files while full selector remains owned");
  Same(expected,result.selection);
  for(std::size_t n=0;n<files.size();++n)Check(files[n].device->Open(paths[n].first,paths[n].second?d::FileOpenMode::open_existing_read_only:d::FileOpenMode::open_existing).ok(),"reopen exact retained source routes");
 }
 const auto revoked=call(grant.binding);failed(revoked);Check(revoked.error==M::memory_binding_failure&&!revoked.physical_bytes_read,"revoked memory cannot read another selector");
 grant.memory={};Same(expected,result.selection);Check(grant.manager.Snapshot().current_bytes==capacity,"retained selector survives wrapper release");
 result={};grant.Empty();
#ifdef SB_BOUND_SELECTION_HASH_PROBE
 if(!deep||fault_route==8)return;
 Grant valid(capacity);
 const auto retained_grant_bytes=valid.ledger.Snapshot().current_bytes;
 const auto released_payload=[&]{const auto state=valid.memory.Snapshot();
  Check(!valid.manager.Snapshot().current_bytes&&!state.allocated_bytes&&
   state.allocation_count==state.release_count&&valid.ledger.Snapshot().current_bytes==retained_grant_bytes,
   "all failed payload allocations released while the reusable grant remains reserved");};
 const auto granted=[&]{return db::ReadNativeBoundCheckpointSelectionWithMemoryFromOpenDevices(database,files,primary,limits,valid.memory,valid.binding);};
 reads=writes=syncs=0;history_observed_read_bytes=0;io_counting=true;hash_seen=0;hash_counting=true;
 {const auto r=granted();Check(r.ok(),"all physical history fault sites baseline");}
 io_counting=false;hash_counting=false;const auto read_sites=reads,hash_sites=hash_seen;
 Check(read_sites&&hash_sites&&!writes&&!syncs,"native selector inspection never writes or synchronizes");
 if(fault_route<0||fault_route==0)for(unsigned site=1;site<=read_sites;++site){if((site-1)%fault_shards!=fault_shard)continue;reads=0;history_observed_read_bytes=0;read_fault=site;io_counting=true;const auto r=granted();io_counting=false;read_fault=0;
  failed(r);Check(r.selection_error==E::io_failure&&!r.io_status.ok()&&
   r.io_diagnostic.diagnostic_code=="SB-STORAGE-DISK-READ-SHORT"&&r.physical_bytes_read==history_observed_read_bytes,
   "complete selector preserves every actual read diagnostic and independent transferred-byte total");
  if(r.checkpoint_error==db::NativeCheckpointError::inventory_failure)
   Check(r.inventory_error==page::NativeInventoryError::io_failure,"selector receipt preserves concrete inventory read error");
  released_payload();}
 for(unsigned mode=1;mode<=5;++mode)if(fault_route<0||fault_route==int(mode))for(unsigned site=1;site<=hash_sites;++site){
  if((site-1)%fault_shards!=fault_shard)continue;
  hash_fault=mode;hash_target=site;hash_seen=0;hash_active=false;const auto r=granted();
  const bool consumed=!hash_fault;hash_fault=0;hash_active=false;failed(r);
  Check(consumed&&r.selection_error==E::hash_failure,"every full-selector provider failure retains exact typed refusal");
  if(r.checkpoint_error==db::NativeCheckpointError::inventory_failure)
   Check(r.inventory_error==page::NativeInventoryError::hash_failure,"selector receipt preserves concrete inventory hash error");
  released_payload();}
#ifdef NATIVE_HISTORICAL_BUNDLE_SIZE_FAULTS
 if(fault_route<0||fault_route==6){historical_stats=0;historical_stat_counting=true;{const auto r=granted();Check(r.ok(),"complete selector extent fault baseline");}
 historical_stat_counting=false;const auto extent_sites=historical_stats;Check(extent_sites,"actual selector size observations");
 for(unsigned site=1;site<=extent_sites;++site){
  historical_stats=0;historical_stat_fault=site;historical_stat_counting=true;
  history_observed_read_bytes=0;reads=0;io_counting=true;const auto r=granted();
  io_counting=false;historical_stat_counting=false;historical_stat_fault=0;failed(r);
  Check(r.selection_error==E::io_failure&&!r.io_status.ok()&&r.physical_bytes_read==history_observed_read_bytes&&
   r.io_diagnostic.diagnostic_code=="SB-STORAGE-DISK-SIZE-FAILED","every actual extent refusal retains backend diagnostic and preceding reads");
  released_payload();
 }
 }
 if(fault_route<0||fault_route==7)for(const auto& file:files){
  std::recursive_mutex* mutex=nullptr;{auto guard=file.device->AcquireOperationGuard();mutex=guard.mutex();}
  memory_probes=memory_locked_probes=0;memory_probe_mutex=mutex;
  {const auto r=granted();memory_probe_mutex=nullptr;Check(r.ok(),"selector ownership lifetime fence probe");memory_probe_mutex=mutex;}
  memory_probe_mutex=nullptr;Check(memory_probes&&!memory_locked_probes,"allocation observations and cleanup remain outside every source fence");
  memory_probes=memory_locked_probes=0;memory_probe_backing_only=true;memory_probe_mutex=mutex;
  reads=0;read_fault=1;io_counting=true;const auto failed_read=granted();io_counting=false;read_fault=0;
  memory_probe_mutex=nullptr;memory_probe_backing_only=false;failed(failed_read);
  Check(memory_probes&&!memory_locked_probes,"failed whole-selector backing releases only after every source fence");
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
  Check(entered&&blocked&&closed.ok()&&retained.ok(),"Close on each participating file waits for complete grant-backed selector");
  Same(expected,retained.selection);
  Check(file.device->Open(path,readonly?d::FileOpenMode::open_existing_read_only:d::FileOpenMode::open_existing).ok(),"restore exact source after concurrent Close");
  retained={};released_payload();
 }
#endif
 valid.memory={};valid.Empty();
 Grant short_grant(capacity-1);reads=0;io_counting=true;
 const auto short_denied=db::ReadNativeBoundCheckpointSelectionWithMemoryFromOpenDevices(database,files,primary,limits,short_grant.memory,short_grant.binding);
 io_counting=false;failed(short_denied);Check(short_denied.error==M::memory_allocation_failure&&!reads,"one byte short actual selector grant refuses before source reads");
 short_grant.memory={};short_grant.Empty();
#else
 (void)deep;
#endif
}
} // namespace bound_selection_memory
