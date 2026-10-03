// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
// Included inside real-file fixtures after their independent byte oracles.
namespace current_source_memory {
namespace d=scratchbird::storage::disk;
namespace c=checkpoint_inventory_memory;
using E=db::NativeCheckpointError;
using M=db::NativeCurrentCheckpointMemoryError;
template<bool allocation,class View> void Empty(const View& v){
 Check(!v.ok()&&!v.retained_image_bytes&&!v.backing_bytes_used,"failed complete current source exposes no prefix");
 c::Empty(v.checkpoint_inventory);
 if constexpr(allocation)Check(v.allocation.pages.empty()&&!v.allocation.retained_image_bytes,"failed current allocation withholds maps");
 else Check(v.directory.pages.empty()&&!v.directory.retained_image_bytes,"failed current directory withholds members");
}
void SameAllocation(const page::NativeAllocationChainResult& a,const page::NativeAllocationChainView& b){
 Check(a.error==b.error&&a.state_counts==b.state_counts&&
  a.retained_image_bytes==b.retained_image_bytes&&a.pages.size()==b.pages.size(),
  "all selected allocation dimensions and counts");
 for(std::size_t n=0;n<a.pages.size();++n){
  const auto& old=a.pages[n];const auto& now=b.pages[n];const auto& p=*old.map;const auto& q=now.map;
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
void SameDirectory(const page::NativeFilespaceDirectoryChainResult& a,const page::NativeDirectoryChainView& b){
 Check(a.error==b.error&&a.retained_image_bytes==b.retained_image_bytes&&a.pages.size()==b.pages.size(),"all directory chain dimensions");
 for(std::size_t n=0;n<a.pages.size();++n){const auto& x=*a.pages[n].directory;const auto& y=b.pages[n].directory;
  const auto xh=d::EncodeNativeCommonPageHeader(x.header),yh=d::EncodeNativeCommonPageHeader(y.header);
  Check(xh.ok()&&yh.ok()&&xh.bytes==yh.bytes&&x.object_uuid==y.object_uuid&&x.directory_generation==y.directory_generation&&
   x.creator_transaction_uuid==y.creator_transaction_uuid&&x.creator_local_transaction_id==y.creator_local_transaction_id&&
   x.creator_operation_uuid==y.creator_operation_uuid&&x.total_records==y.total_records&&x.first_record==y.first_record&&
   x.next==y.next&&x.next_sha256==y.next_sha256&&x.records.size()==y.records.size()&&
   std::equal(a.pages[n].bytes.begin(),a.pages[n].bytes.end(),b.pages[n].image.begin(),b.pages[n].image.end()),
   "every canonical directory header creator generation successor and retained physical image");
  for(std::size_t j=0;j<x.records.size();++j){const auto& p=x.records[j];const auto& q=y.records[j];
   const auto ph=d::EncodeFilespaceBootstrap(p.bootstrap),qh=d::EncodeFilespaceBootstrap(q.bootstrap);
   Check(ph.ok()&&qh.ok()&&ph.bytes==qh.bytes&&p.locator_uuid==q.locator_uuid&&p.page_zero_uuid==q.page_zero_uuid&&
    p.page_zero_generation==q.page_zero_generation&&p.root_set_generation==q.root_set_generation&&p.total_pages==q.total_pages&&
    p.verification_epoch==q.verification_epoch&&p.operation==q.operation&&bool(p.allocation_root)==bool(q.allocation_root),
    "all member bootstrap identity role lifecycle protection location generations capacities and operation fields");
   if(p.allocation_root){const auto& l=*p.allocation_root;const auto& r=*q.allocation_root;
    Check(l.page==r.page&&l.object_uuid==r.object_uuid&&l.sha256==r.sha256&&l.map_generation==r.map_generation&&l.capacity_generation==r.capacity_generation,
     "every member allocation root and generation");}
  }
 }
}

template<bool allocation,class Expected,class View> void Same(const Expected& a,const View& b){
 Check(a.error==b.error,"complete current-source owning/borrowed failure parity");
 if constexpr(allocation)Check(a.allocation_error==b.allocation_error,"exact nested allocation error");
 else Check(a.directory_error==b.directory_error,"exact nested directory error");
 c::Same(a.checkpoint_inventory,b.checkpoint_inventory);
 if(!a.ok()){Empty<allocation>(b);return;}
 Check(b.ok()&&a.retained_image_bytes==b.retained_image_bytes,"complete original current-source verification charge");
 if constexpr(allocation)SameAllocation(a.allocation,b.allocation);else SameDirectory(a.directory,b.directory);
}
template<bool allocation,class Read> const auto& Source(const Read& r){
 if constexpr(allocation)return r.allocation;else return r.directory;
}
template<bool allocation,class Expected> void Checks(std::span<const d::NativeFilespaceDevice> files,
 const d::FilespaceRootReference& root,const Expected& expected,u64 allowance,bool deep=false,
 int fault_route=8,unsigned fault_shard=0,unsigned fault_shards=1){
 const auto database=Id(1);
 const auto backing_capacity=static_cast<std::size_t>((expected.ok()?expected.retained_image_bytes:std::min<u64>(allowance,16*1024*1024))*4+1024*1024);
 Bytes backing(backing_capacity);std::vector<std::unique_ptr<d::FileDevice::ReadLatencyBatch>> owners;
 std::vector<d::FileDevice::ReadLatencyBatch*> batches;
 for(const auto& file:files){owners.push_back(std::make_unique<d::FileDevice::ReadLatencyBatch>(*file.device));batches.push_back(owners.back().get());}
 auto requested_image_bytes=allowance;
 const auto direct=[&](std::span<byte> region){
  allocation_budget=0;
  const auto invoke=[&]{if constexpr(allocation)return db::VerifyCurrentNativeCheckpointAllocationInto(database,files,root,requested_image_bytes,batches,region);
   else return db::VerifyCurrentNativeCheckpointDirectoryInto(database,files,root,requested_image_bytes,batches,region);};
  auto result=invoke();const bool untouched=allocation_budget==0;allocation_budget=-1;
  Check(untouched,"full current source never falls back to ordinary or aligned heap");return result;
 };
 const auto complete=direct(backing);Same<allocation>(expected,Source<allocation>(complete));if(!expected.ok())return;
 const auto used=Source<allocation>(complete).backing_bytes_used;
 Check(used&&used<=backing_capacity&&complete.io_status.ok()&&complete.physical_bytes_read,"actual current-source backing and physical receipt");
 requested_image_bytes=expected.retained_image_bytes;
 Same<allocation>(expected,Source<allocation>(direct(backing)));
 --requested_image_bytes;const auto short_image=direct(backing);const auto& short_image_view=Source<allocation>(short_image);
 bool image_resource=short_image_view.error==E::resource_exhausted||
  (short_image_view.error==E::inventory_failure&&short_image_view.checkpoint_inventory.inventory_error==page::NativeInventoryError::resource_exhausted);
 if constexpr(allocation)image_resource=image_resource||(short_image_view.error==E::allocation_failure&&short_image_view.allocation_error==page::NativeAllocationError::resource_exhausted);
 else image_resource=image_resource||(short_image_view.error==E::directory_failure&&short_image_view.directory_error==page::NativeDirectoryError::resource_exhausted);
 Check(image_resource,"one byte short complete verification-image allowance refuses");Empty<allocation>(short_image_view);
 requested_image_bytes=allowance;
 Same<allocation>(expected,Source<allocation>(direct(std::span(backing).first(used))));
 const auto short_read=direct(std::span(backing).first(used-1));const auto& short_view=Source<allocation>(short_read);
 bool resource=short_view.error==E::resource_exhausted;
 if constexpr(allocation)resource=resource||(short_view.error==E::allocation_failure&&short_view.allocation_error==page::NativeAllocationError::resource_exhausted);
 else resource=resource||(short_view.error==E::directory_failure&&short_view.directory_error==page::NativeDirectoryError::resource_exhausted);
 Check(resource,"one byte short current-source backing retains exact resource refusal");Empty<allocation>(short_view);
 const auto alias=[&](std::span<byte> region){const auto r=direct(region);
  Check(Source<allocation>(r).error==E::invalid_backing&&!r.physical_bytes_read,"whole current-source input alias refused before effects");Empty<allocation>(Source<allocation>(r));};
 alias({reinterpret_cast<byte*>(const_cast<Uuid*>(&database)),sizeof(database)});
 alias({reinterpret_cast<byte*>(const_cast<d::FilespaceRootReference*>(&root)),sizeof(root)});
 alias({reinterpret_cast<byte*>(const_cast<d::NativeFilespaceDevice*>(files.data())),files.size()*sizeof(files.front())});
 alias({reinterpret_cast<byte*>(files.front().device),sizeof(d::FileDevice)});
 alias({reinterpret_cast<byte*>(batches.data()),batches.size()*sizeof(batches.front())});
 alias({reinterpret_cast<byte*>(batches.front()),sizeof(*batches.front())});
 d::FileDevice foreign;d::FileDevice::ReadLatencyBatch wrong(foreign);auto* saved=batches.front();batches.front()=&wrong;
 const auto denied=direct(backing);batches.front()=saved;
 Check(Source<allocation>(denied).error==E::invalid_filespace&&!denied.physical_bytes_read,"exact-device source observations mandatory");Empty<allocation>(Source<allocation>(denied));
 using Batch=d::FileDevice::ReadLatencyBatch;
 // Charge the actual measured decoded workspace and wrapper metadata, not
 // four times an image allowance which includes repeated verification work.
 const auto capacity=used+files.size()*(sizeof(Batch)+sizeof(Batch*)+alignof(Batch))+
  2*alignof(std::max_align_t);
 c::Grant grant(capacity);const db::NativeCurrentCheckpointMemoryLimits limits{allowance,capacity};
 const auto call=[&](db::NativeStorageMemory& memory,const db::NativeStorageMemoryBinding& binding){
  if constexpr(allocation)return db::VerifyCurrentNativeCheckpointAllocationWithMemoryFromOpenDevices(database,files,root,limits,memory,binding);
  else return db::VerifyCurrentNativeCheckpointDirectoryWithMemoryFromOpenDevices(database,files,root,limits,memory,binding);
 };
 auto retained=call(grant.memory,grant.binding);Check(retained.ok(),"real binary grant backs complete current source");Same<allocation>(expected,retained.source);
 Check(retained.physical_bytes_read==complete.physical_bytes_read&&retained.io_status.ok(),"actual grant preserves exact current-source transferred bytes");
 Check(grant.manager.Snapshot().current_bytes==capacity&&grant.ledger.Snapshot().current_bytes==capacity&&
  grant.memory.Snapshot().allocated_bytes==capacity&&retained.arena.Snapshot().retained_bytes==capacity,"complete current-source charges at every scope");
 const auto failed=[&](const auto& r){Check(!r.ok()&&!r.arena,"failure cannot retain current-source owner");Empty<allocation>(r.source);};
 const auto exhausted=call(grant.memory,grant.binding);failed(exhausted);
 Check(exhausted.error==M::memory_allocation_failure&&!exhausted.physical_bytes_read,"held current source prevents over-limit payload before I/O");
 for(unsigned i=0;i<4;++i){auto wrong=grant.binding;const std::array<Uuid*,4> ids{&wrong.database_uuid,&wrong.operation_uuid,&wrong.owner_uuid,&wrong.context_uuid};
  *ids[i]=Id(219);const auto r=call(grant.memory,wrong);failed(r);Check(r.error==M::memory_binding_failure&&!r.physical_bytes_read,"every binary current-source memory identity enforced");}
 Check(grant.ledger.CleanupOwner(grant.binding.owner_uuid.bytes).retained_bytes==capacity,"revocation retains current-source payload and charges");
 if(deep){std::vector<std::pair<std::string,bool>> paths;
  for(const auto& file:files){auto fence=file.device->AcquireOperationGuard();paths.emplace_back(file.device->path(),file.device->read_only());}
  for(const auto& file:files)Check(file.device->Close().ok(),"close sources while current-source data remains owned");
  Same<allocation>(expected,retained.source);
  for(std::size_t n=0;n<files.size();++n)Check(files[n].device->Open(paths[n].first,paths[n].second?d::FileOpenMode::open_existing_read_only:d::FileOpenMode::open_existing).ok(),"reopen exact source after retained-current-source test");
 }
 const auto revoked=call(grant.memory,grant.binding);failed(revoked);Check(revoked.error==M::memory_binding_failure&&!revoked.physical_bytes_read,"revoked current-source grant refuses new I/O");
 grant.memory={};Same<allocation>(expected,retained.source);Check(grant.manager.Snapshot().current_bytes==capacity,"current-source values survive grant-wrapper release");
 retained={};grant.Empty();
#ifdef SB_CURRENT_SOURCE_HASH_PROBE
 if(!deep||fault_route==8)return;
 const auto io_error=[](const auto& r){return r.source_error==E::io_failure||
  (r.source_error==E::allocation_failure&&r.allocation_error==page::NativeAllocationError::io_failure)||
  (r.source_error==E::directory_failure&&r.directory_error==page::NativeDirectoryError::io_failure);};
 const auto hash_error=[](const auto& r){return r.source_error==E::hash_failure||
  (r.source_error==E::allocation_failure&&r.allocation_error==page::NativeAllocationError::hash_failure)||
  (r.source_error==E::directory_failure&&r.directory_error==page::NativeDirectoryError::hash_failure);};
 c::Grant valid(capacity);
 const auto retained_grant_bytes=valid.ledger.Snapshot().current_bytes;
 const auto released_payload=[&]{const auto state=valid.memory.Snapshot();
  Check(!valid.manager.Snapshot().current_bytes&&!state.allocated_bytes&&
   state.allocation_count==state.release_count&&valid.ledger.Snapshot().current_bytes==retained_grant_bytes,
   "all failed payload allocations released while the reusable grant remains reserved");};
 const auto granted=[&]{return call(valid.memory,valid.binding);};
 reads=writes=syncs=0;history_observed_read_bytes=0;io_counting=true;hash_seen=0;hash_counting=true;
 {const auto r=granted();Check(r.ok(),"all physical history fault sites baseline");}
 io_counting=false;hash_counting=false;const auto read_sites=reads,hash_sites=hash_seen;
 Check(read_sites&&hash_sites&&!writes&&!syncs,"native current source inspection never writes or synchronizes");
 if(fault_route<0||fault_route==0)for(unsigned site=1;site<=read_sites;++site){if((site-1)%fault_shards!=fault_shard)continue;reads=0;history_observed_read_bytes=0;read_fault=site;io_counting=true;const auto r=granted();io_counting=false;read_fault=0;
  failed(r);Check(io_error(r)&&!r.io_status.ok()&&
   r.io_diagnostic.diagnostic_code=="SB-STORAGE-DISK-READ-SHORT"&&r.physical_bytes_read==history_observed_read_bytes,
   "complete current source preserves every actual read diagnostic and independent transferred-byte total");
  if(r.inventory_error!=page::NativeInventoryError::none)
   Check(r.inventory_error==page::NativeInventoryError::io_failure,"current source preserves concrete inventory read error");
  released_payload();}
 for(unsigned mode=1;mode<=5;++mode)if(fault_route<0||fault_route==int(mode))for(unsigned site=1;site<=hash_sites;++site){
  if((site-1)%fault_shards!=fault_shard)continue;
  hash_fault=mode;hash_target=site;hash_seen=0;hash_active=false;const auto r=granted();
  const bool consumed=!hash_fault;hash_fault=0;hash_active=false;failed(r);
  Check(consumed&&hash_error(r),"every full-current source provider failure retains exact typed refusal");
  if(r.inventory_error!=page::NativeInventoryError::none)
   Check(r.inventory_error==page::NativeInventoryError::hash_failure,"current source preserves concrete inventory hash error");
  released_payload();}
#ifdef NATIVE_HISTORICAL_BUNDLE_SIZE_FAULTS
 if(fault_route<0||fault_route==6){historical_stats=0;historical_stat_counting=true;{const auto r=granted();Check(r.ok(),"complete current source extent fault baseline");}
 historical_stat_counting=false;const auto extent_sites=historical_stats;Check(extent_sites,"actual current source size observations");
 for(unsigned site=1;site<=extent_sites;++site){
  historical_stats=0;historical_stat_fault=site;historical_stat_counting=true;
  history_observed_read_bytes=0;reads=0;io_counting=true;const auto r=granted();
  io_counting=false;historical_stat_counting=false;historical_stat_fault=0;failed(r);
  Check(io_error(r)&&!r.io_status.ok()&&r.physical_bytes_read==history_observed_read_bytes&&
   r.io_diagnostic.diagnostic_code=="SB-STORAGE-DISK-SIZE-FAILED","every actual extent refusal retains backend diagnostic and preceding reads");
  released_payload();
 }
 }
 if(fault_route<0||fault_route==7)for(const auto& file:files){
  std::recursive_mutex* mutex=nullptr;{auto guard=file.device->AcquireOperationGuard();mutex=guard.mutex();}
  memory_probes=memory_locked_probes=0;memory_probe_mutex=mutex;
  {const auto r=granted();memory_probe_mutex=nullptr;Check(r.ok(),"current source ownership lifetime fence probe");memory_probe_mutex=mutex;}
  memory_probe_mutex=nullptr;Check(memory_probes&&!memory_locked_probes,"allocation observations and cleanup remain outside every source fence");
  memory_probes=memory_locked_probes=0;memory_probe_backing_only=true;memory_probe_mutex=mutex;
  reads=0;read_fault=1;io_counting=true;const auto failed_read=granted();io_counting=false;read_fault=0;
  memory_probe_mutex=nullptr;memory_probe_backing_only=false;failed(failed_read);
  Check(memory_probes&&!memory_locked_probes,"failed whole-current source backing releases only after every source fence");
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
  Check(entered&&blocked&&closed.ok()&&retained.ok(),"Close on each participating file waits for complete grant-backed current source");
  Same<allocation>(expected,retained.source);
  Check(file.device->Open(path,readonly?d::FileOpenMode::open_existing_read_only:d::FileOpenMode::open_existing).ok(),"restore exact source after concurrent Close");
  retained={};released_payload();
 }
#endif
 valid.memory={};valid.Empty();
 c::Grant short_grant(capacity-1);reads=0;io_counting=true;
 const auto short_denied=call(short_grant.memory,short_grant.binding);
 io_counting=false;failed(short_denied);Check(short_denied.error==M::memory_allocation_failure&&!reads,"one byte short actual current source grant refuses before source reads");
 short_grant.memory={};short_grant.Empty();
#else
 (void)deep;
#endif
}
} // namespace current_source_memory
