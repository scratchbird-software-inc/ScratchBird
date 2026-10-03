// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
// Included only after each real-file fixture's independent source oracles.
namespace selected_lease_memory {
namespace d=scratchbird::storage::disk;
namespace c=checkpoint_inventory_memory;
using E=db::NativeSelectedCheckpointMemoryError;
static_assert(!std::is_default_constructible_v<db::NativeSelectedCheckpointMemoryLease>);
static_assert(!std::is_copy_constructible_v<db::NativeSelectedCheckpointMemoryHandle>);
static_assert(std::is_move_constructible_v<db::NativeSelectedCheckpointMemoryHandle>);
static_assert(std::is_const_v<typename decltype(db::NativeReadCheckpointRoot{}.roots)::element_type>);
static_assert(std::is_const_v<typename decltype(db::NativeReadAllocationMap{}.states)::element_type>);
static_assert(std::is_const_v<typename decltype(db::NativeReadAllocationMap{}.records)::element_type>);
static_assert(std::is_const_v<typename decltype(db::NativeReadDirectory{}.records)::element_type>);
static_assert(std::is_const_v<typename decltype(db::NativeReadTransactionInventory{}.entries)::element_type>);
static_assert(std::is_const_v<typename decltype(db::NativeReadCheckpointInventory{}.inventory_pages)::element_type>);
static_assert(std::is_const_v<typename decltype(db::NativeReadAllocationChain{}.pages)::element_type>);
static_assert(std::is_const_v<typename decltype(db::NativeReadDirectoryChain{}.pages)::element_type>);
void SameInventory(const db::NativeCheckpointInventoryResult& x,const db::NativeReadCheckpointInventory& y){
 Check(x.error==y.error&&x.inventory_error==y.inventory_error&&bool(x.checkpoint)==bool(y.checkpoint),
  "lease preserves exact inventory outcome and presence");
 if(!x.ok()){
  Check(!y.ok()&&!y.checkpoint&&y.inventory.entries.empty()&&y.inventory_pages.empty()&&!y.retained_image_bytes,
   "absent predecessor has no inspection prefix");return;
 }
 Check(y.ok()&&x.checkpoint_sha256==y.checkpoint_sha256&&x.inventory_generation==y.inventory_generation&&
  x.retained_image_bytes==y.retained_image_bytes&&x.inventory_pages.size()==y.inventory_pages.size()&&
  x.inventory.next_local_transaction_id==y.inventory.next_local_transaction_id&&
  x.inventory.next_commit_sequence==y.inventory.next_commit_sequence&&x.inventory.entries.size()==y.inventory.entries.size(),
  "all checkpoint inventory counters generations hashes and dimensions");
 const auto& a=*x.checkpoint;const auto& b=*y.checkpoint;
 const auto ah=d::EncodeNativeCommonPageHeader(a.header),bh=d::EncodeNativeCommonPageHeader(b.header);
 Check(ah.ok()&&bh.ok()&&ah.bytes==bh.bytes&&a.object_uuid==b.object_uuid&&
  a.checkpoint_generation==b.checkpoint_generation&&a.root_set_generation==b.root_set_generation&&
  a.selected_local_transaction_id==b.selected_local_transaction_id&&a.stable_local_transaction_id==b.stable_local_transaction_id&&
  a.local_durable_transaction_id==b.local_durable_transaction_id&&a.cluster_quorum_transaction_id==b.cluster_quorum_transaction_id&&
  a.timeline_uuid==b.timeline_uuid&&a.creator_transaction_uuid==b.creator_transaction_uuid&&
  a.creator_local_transaction_id==b.creator_local_transaction_id&&a.flags==b.flags&&
  a.predecessor==b.predecessor&&a.predecessor_sha256==b.predecessor_sha256&&a.completed==b.completed&&
  a.creator_operation_uuid==b.creator_operation_uuid&&std::equal(a.roots.begin(),a.roots.end(),b.roots.begin(),b.roots.end()),
  "every immutable checkpoint field and binary root identity");
 for(std::size_t i=0;i<x.inventory_pages.size();++i){
  const auto& p=x.inventory_pages[i];const auto& q=y.inventory_pages[i];
  const auto ph=d::EncodeNativeCommonPageHeader(p.header),qh=d::EncodeNativeCommonPageHeader(q.header);
  Check(ph.ok()&&qh.ok()&&ph.bytes==qh.bytes&&p.object_uuid==q.object_uuid,"every immutable inventory page binding");
 }
 for(std::size_t i=0;i<x.inventory.entries.size();++i){
  const auto& p=x.inventory.entries[i];const auto& q=y.inventory.entries[i];
  Check(p.identity.local_id.value==q.identity.local_id.value&&p.identity.transaction_uuid.kind==q.identity.transaction_uuid.kind&&
   p.identity.transaction_uuid.value==q.identity.transaction_uuid.value&&p.identity.scope==q.identity.scope&&
   p.state==q.state&&p.archived_from_state==q.archived_from_state&&p.begin_unix_epoch_millis==q.begin_unix_epoch_millis&&
   p.final_unix_epoch_millis==q.final_unix_epoch_millis&&p.begin_visible_through_local_transaction_id==q.begin_visible_through_local_transaction_id&&
   p.begin_visible_through_commit_sequence==q.begin_visible_through_commit_sequence&&p.commit_sequence==q.commit_sequence&&
   p.evidence_record_required==q.evidence_record_required&&p.evidence_record_written==q.evidence_record_written&&
   p.rollback_only==q.rollback_only&&p.stable_snapshot==q.stable_snapshot,"every immutable inventory entry field");
 }
}
void SameAllocation(const page::NativeAllocationChainResult& a,const db::NativeReadAllocationChain& b){
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
void SameDirectory(const page::NativeFilespaceDirectoryChainResult& a,const db::NativeReadDirectoryChain& b){
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

void Same(const db::NativeBoundCheckpointSelection& a,const db::NativeCheckpointDirectoryResult& b,
 const d::FilespaceRootReference& root,u64 images,const db::NativeSelectedCheckpointMemoryLease& lease){
 const auto& selected=lease.selection();const auto& directory=lease.directory();
 Check(selected.ok()&&directory.ok()&&lease.retained_image_bytes()==images&&lease.backing_bytes_used(),
  "retained lease owns complete immutable selected source");
 const auto& actual=lease.checkpoint();
 Check(actual.kind==root.kind&&actual.page_type==root.page_type&&actual.filespace_uuid==root.filespace_uuid&&
  actual.page_number==root.page_number&&actual.page_generation==root.page_generation&&
  actual.page_size_profile_uuid==root.page_size_profile_uuid&&actual.object_uuid==root.object_uuid,
  "lease checkpoint derives from actual paired selectors");
 const auto x=db::EncodeNativeCheckpointSelection(*a.selection),y=db::EncodeNativeCheckpointSelection(*selected.selection);
 Check(x.ok()&&y.ok()&&x.bytes==y.bytes&&a.retained_image_bytes==selected.retained_image_bytes,
  "every selected field and full original verification charge");
 for(unsigned i=0;i<2;++i)Check(std::equal(a.slots[i].begin(),a.slots[i].end(),selected.slots[i].begin(),selected.slots[i].end()),
  "both retained physical selector images");
 SameInventory(a.checkpoint_inventory,selected.checkpoint_inventory);SameInventory(a.predecessor,selected.predecessor);
 SameAllocation(a.allocation,selected.allocation);
 Check(b.retained_image_bytes==directory.retained_image_bytes,"complete current-directory verification charge");
 SameInventory(b.checkpoint_inventory,directory.checkpoint_inventory);SameDirectory(b.directory,directory.directory);
}

void Checks(std::span<const d::NativeFilespaceDevice> files,u64 allowance,bool deep=false,
 int fault_route=8,unsigned fault_shard=0,unsigned fault_shards=1){
 const auto database=Id(1),primary=Id(2);
 const std::vector<d::NativeFilespaceDevice> supplied(files.begin(),files.end());
 db::NativeBoundCheckpointSelection expected_selection;
 db::NativeCheckpointDirectoryResult expected_directory;
 d::FilespaceRootReference root;u64 images=0;
 {
  auto original=db::AcquireNativeSelectedCheckpointReadLease(database,supplied,primary,allowance);
  Check(original.ok(),"actual full selected lease independently verified before memory admission");
  expected_selection=original.lease->selection();expected_directory=original.lease->directory();
  root=original.lease->checkpoint();images=original.lease->retained_image_bytes();
 } // Release every legacy guard before admitting the actual grant.
 // Measure actual decoded work, not four times a repeated proof-image charge.
 // The temporary measurement is test-only and is gone before real admission.
 std::size_t measured=0;
 {
  // Test-only meter: count the exact aligned bump demand while allocating
  // only requested chunks. Avoid a multi-gigabyte 4x-image fixture allocation.
  struct Meter final:std::pmr::memory_resource {
   std::pmr::monotonic_buffer_resource pool;std::size_t used=0;
   void* do_allocate(std::size_t bytes,std::size_t alignment) override {
    if(used>std::numeric_limits<std::size_t>::max()-(alignment-1))throw std::bad_alloc();
    const auto at=(used+alignment-1)&~(alignment-1);
    if(bytes>std::numeric_limits<std::size_t>::max()-at)throw std::bad_alloc();
    auto* p=pool.allocate(bytes,alignment);used=at+bytes;return p;
   }
   void do_deallocate(void*,std::size_t,std::size_t) override {}
   bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override{return this==&other;}
  } resource;
  std::vector<std::unique_ptr<d::FileDevice::ReadLatencyBatch>> owners;
  std::vector<d::FileDevice::ReadLatencyBatch*> batches;
  for(const auto& file:files){owners.push_back(std::make_unique<d::FileDevice::ReadLatencyBatch>(*file.device));batches.push_back(owners.back().get());}
  const auto selected=db::detail::ReadNativeBoundCheckpointSelectionBacked(database,files,primary,allowance,batches,resource);
  const auto directory=db::detail::VerifyCurrentNativeCheckpointDirectoryBacked(database,files,root,allowance-selected.selection.retained_image_bytes,batches,resource);
  Check(selected.ok()&&directory.ok(),"measure complete lease dependencies through the explicit test resource");
  measured=resource.used;
 }
 using Batch=d::FileDevice::ReadLatencyBatch;using Guard=std::unique_lock<std::recursive_mutex>;
 const auto capacity=measured+sizeof(db::NativeSelectedCheckpointMemoryLease)+
  files.size()*(sizeof(Batch)+sizeof(Batch*)+sizeof(Guard)+sizeof(d::NativeFilespaceDevice)+4*alignof(std::max_align_t))+
  expected_selection.allocation.pages.size()*sizeof(db::NativeReadAllocationPage)+
  expected_directory.directory.pages.size()*sizeof(db::NativeReadDirectoryPage)+1024;
 c::Grant grant(capacity);db::NativeSelectedCheckpointMemoryLimits limits{allowance,capacity};
 const auto call=[&](db::NativeStorageMemory& memory,const db::NativeStorageMemoryBinding& binding){
  return db::AcquireNativeSelectedCheckpointMemoryLease(database,files,primary,limits,memory,binding);
 };
 const auto same=[&](const auto& result){Check(result.ok(),"actual binary-bound selected read lease");
  Same(expected_selection,expected_directory,root,images,*result.lease);};
 const auto failed=[&](const auto& r){Check(!r.ok()&&!r.lease,"failed admission exposes neither lease nor source prefix");};
 std::size_t exact=0;u64 physical=0;
 {
  auto result=call(grant.memory,grant.binding);same(result);
  exact=result.lease->backing_bytes_used();physical=result.physical_bytes_read;
  Check(physical&&result.io_status.ok()&&exact<=capacity&&grant.manager.Snapshot().current_bytes==capacity&&
   grant.ledger.Snapshot().current_bytes==capacity&&grant.memory.Snapshot().allocated_bytes==capacity,
   "complete retained lease backing charged at every real scope");
 }
 const auto released=[&]{const auto s=grant.memory.Snapshot();
  Check(!grant.manager.Snapshot().current_bytes&&!s.allocated_bytes&&s.allocation_count==s.release_count&&
   grant.ledger.Snapshot().current_bytes==capacity,"released payload leaves only the reusable reservation");
 };
 released();
 limits.maximum_backing_bytes=exact;{const auto r=call(grant.memory,grant.binding);same(r);}released();
 --limits.maximum_backing_bytes;{const auto r=call(grant.memory,grant.binding);failed(r);
  Check(r.error==E::resource_exhausted||
   (r.error==E::selection_failure&&r.selection_error==db::NativeCheckpointSelectionError::resource_exhausted)||
   (r.error==E::directory_failure&&r.checkpoint_error==db::NativeCheckpointError::resource_exhausted),
   "one byte short retained lease backing refuses");}released();

 limits.maximum_backing_bytes=capacity;
 for(unsigned bad=0;bad<7;++bad){
  auto wrong=supplied;auto owner=primary;auto altered=limits;
  if(bad==0)wrong.clear();
  if(bad==1)wrong.front().device=nullptr;
  if(bad==2)wrong.front().filespace_uuid={};
  if(bad==3)wrong.front().page_size_profile_uuid=Id(219);
  if(bad==4)owner=Id(219);
  if(bad==5)altered.maximum_verification_image_bytes=0;
  if(bad==6)altered.maximum_backing_bytes=0;
  const auto denied=db::AcquireNativeSelectedCheckpointMemoryLease(database,wrong,owner,altered,grant.memory,grant.binding);
  failed(denied);Check(!denied.physical_bytes_read,"invalid selected lease input refuses before I/O");released();
 }
 if(files.size()>1)for(unsigned bad=0;bad<2;++bad){
  auto wrong=supplied;
  if(bad==0)wrong.back().filespace_uuid=wrong.front().filespace_uuid;else wrong.back().device=wrong.front().device;
  const auto denied=db::AcquireNativeSelectedCheckpointMemoryLease(database,wrong,primary,limits,grant.memory,grant.binding);
  failed(denied);Check(denied.error==E::invalid_device&&!denied.physical_bytes_read,
   "duplicate selected-source UUID or device refuses before I/O");released();
 }
 {c::Grant short_grant(capacity-1);const auto r=call(short_grant.memory,short_grant.binding);failed(r);
  Check(r.error==E::memory_allocation_failure&&!r.physical_bytes_read,"one-byte-short actual grant refuses before fences");
  short_grant.memory={};short_grant.Empty();}
 limits.maximum_verification_image_bytes=images;
 {const auto r=call(grant.memory,grant.binding);same(r);}released();
 --limits.maximum_verification_image_bytes;{const auto r=call(grant.memory,grant.binding);failed(r);
  Check(r.error==E::resource_exhausted||r.error==E::directory_failure,"one byte short complete source image budget refuses");
 }released();limits.maximum_verification_image_bytes=allowance;
 {
  auto retained=call(grant.memory,grant.binding);same(retained);
  // Issuance occurs on another thread: no governor call under this thread's
  // retained source fences. Refusals occur before any device I/O.
  auto admission=std::async(std::launch::async,[&]{
   const auto exhausted=call(grant.memory,grant.binding);failed(exhausted);
   Check(exhausted.error==E::memory_allocation_failure&&!exhausted.physical_bytes_read,
    "live lease consumes actual complete grant and prevents excess work");
   for(unsigned n=0;n<4;++n){auto wrong=grant.binding;
    const std::array<Uuid*,4> fields{&wrong.database_uuid,&wrong.operation_uuid,&wrong.owner_uuid,&wrong.context_uuid};
    *fields[n]=Id(219);const auto denied=call(grant.memory,wrong);failed(denied);
    Check(denied.error==E::memory_binding_failure&&!denied.physical_bytes_read,"every binary lease grant binding enforced");
   }
   Check(grant.ledger.CleanupOwner(grant.binding.owner_uuid.bytes).retained_bytes==capacity,
    "revocation retains live lease storage and charge");
   const auto revoked=call(grant.memory,grant.binding);failed(revoked);
   Check(revoked.error==E::memory_binding_failure&&!revoked.physical_bytes_read,"revocation forbids further source admission");
  });admission.get();
  same(retained);auto moved=std::move(retained.lease);
  Check(!retained.lease&&bool(moved),"same-thread handle move retains inseparable lease and backing");
  grant.memory={};Same(expected_selection,expected_directory,root,images,*moved);
  Check(grant.manager.Snapshot().current_bytes==capacity,"retired grant wrapper cannot free live lease");
 }grant.Empty();
 if(!deep)return;
 c::Grant valid(capacity);
 const auto granted=[&]{return call(valid.memory,valid.binding);};
 // Independently owned admission contexts acquire reversed cohorts without
 // lock-order inversion. The lease is consumed and destroyed on its worker.
 auto reversed=supplied;std::reverse(reversed.begin(),reversed.end());
 c::Grant second(capacity);
 std::mutex assertions;
 const auto verify=[&](const auto& r){std::lock_guard guard(assertions);same(r);};
 auto one=std::async(std::launch::async,[&]{const auto r=granted();verify(r);});
 auto two=std::async(std::launch::async,[&]{
  const auto r=db::AcquireNativeSelectedCheckpointMemoryLease(database,reversed,primary,limits,second.memory,second.binding);
  verify(r);
 });one.get();two.get();second.memory={};second.Empty();

#ifdef SB_SELECTED_LEASE_HASH_PROBE
 const auto reservation=valid.ledger.Snapshot().current_bytes;
 const auto released_payload=[&]{const auto state=valid.memory.Snapshot();
  Check(!valid.manager.Snapshot().current_bytes&&!state.allocated_bytes&&state.allocation_count==state.release_count&&
   valid.ledger.Snapshot().current_bytes==reservation,"all refused lease payloads release while grant remains reserved");};
 if(fault_route!=8){
  reads=writes=syncs=0;history_observed_read_bytes=0;io_counting=true;hash_seen=0;hash_counting=true;
  {const auto r=granted();Check(r.ok(),"complete retained lease fault baseline");}
  io_counting=false;hash_counting=false;const auto read_sites=reads,hash_sites=hash_seen;
  Check(read_sites&&hash_sites&&!writes&&!syncs,"retained lease has no storage effects");
  const auto io_error=[](const auto& r){return r.selection_error==db::NativeCheckpointSelectionError::io_failure||
   r.checkpoint_error==db::NativeCheckpointError::io_failure||
   (r.checkpoint_error==db::NativeCheckpointError::directory_failure&&r.directory_error==page::NativeDirectoryError::io_failure);};
  const auto hash_error=[](const auto& r){return r.selection_error==db::NativeCheckpointSelectionError::hash_failure||
   r.checkpoint_error==db::NativeCheckpointError::hash_failure||
   (r.checkpoint_error==db::NativeCheckpointError::directory_failure&&r.directory_error==page::NativeDirectoryError::hash_failure);};
  if(fault_route<0||fault_route==0)for(unsigned site=1;site<=read_sites;++site){
   if((site-1)%fault_shards!=fault_shard)continue;
   reads=0;history_observed_read_bytes=0;read_fault=site;io_counting=true;
   const auto r=granted();io_counting=false;read_fault=0;failed(r);
   Check(io_error(r)&&!r.io_status.ok()&&r.io_diagnostic.diagnostic_code=="SB-STORAGE-DISK-READ-SHORT"&&
    r.physical_bytes_read==history_observed_read_bytes,"every lease read failure preserves exact physical receipt");
   if(r.inventory_error!=page::NativeInventoryError::none)
    Check(r.inventory_error==page::NativeInventoryError::io_failure,"exact retained lease inventory read diagnostic");
   released_payload();
  }
  for(unsigned mode=1;mode<=5;++mode)if(fault_route<0||fault_route==int(mode))for(unsigned site=1;site<=hash_sites;++site){
   if((site-1)%fault_shards!=fault_shard)continue;
   hash_fault=mode;hash_target=site;hash_seen=0;hash_active=false;const auto r=granted();
   const bool consumed=!hash_fault;hash_fault=0;hash_active=false;failed(r);
   Check(consumed&&hash_error(r),"every retained lease digest provider failure is consumed and typed");
   if(r.inventory_error!=page::NativeInventoryError::none)
    Check(r.inventory_error==page::NativeInventoryError::hash_failure,"exact retained lease inventory hash diagnostic");
   released_payload();
  }
#ifdef NATIVE_HISTORICAL_BUNDLE_SIZE_FAULTS
  if(fault_route<0||fault_route==6){
   historical_stats=0;historical_stat_counting=true;{const auto r=granted();Check(r.ok(),"lease extent failure baseline");}
   historical_stat_counting=false;const auto sites=historical_stats;Check(sites,"lease actual extent sites");
   for(unsigned site=1;site<=sites;++site){
    historical_stats=0;historical_stat_fault=site;historical_stat_counting=true;
    history_observed_read_bytes=0;reads=0;io_counting=true;const auto r=granted();
    io_counting=false;historical_stat_counting=false;historical_stat_fault=0;failed(r);
    Check(historical_stats>=site&&io_error(r)&&!r.io_status.ok()&&r.physical_bytes_read==history_observed_read_bytes,
     "every lease size failure preserves physical observations");released_payload();
   }
  }
#endif
  if(fault_route<0||fault_route==7)for(const auto& file:files){
   std::recursive_mutex* mutex=nullptr;{auto guard=file.device->AcquireOperationGuard();mutex=guard.mutex();}
   memory_probes=memory_locked_probes=0;memory_probe_mutex=mutex;
   {auto r=granted();memory_probe_mutex=nullptr;same(r);memory_probe_mutex=mutex;r.lease.Reset();}
   memory_probe_mutex=nullptr;
   Check(memory_probes&&!memory_locked_probes,"no lease admission observation or cleanup provider runs under a source fence");
   memory_probes=memory_locked_probes=0;memory_probe_backing_only=true;memory_probe_mutex=mutex;
   reads=0;read_fault=1;io_counting=true;const auto refused=granted();io_counting=false;read_fault=0;
   memory_probe_mutex=nullptr;memory_probe_backing_only=false;failed(refused);
   Check(memory_probes&&!memory_locked_probes,"failed lease releases every fence before provider cleanup");
   released_payload();
   std::string path;bool readonly=false;{auto guard=file.device->AcquireOperationGuard();path=file.device->path();readonly=file.device->read_only();}
   std::promise<void> admitted,release;auto entered=admitted.get_future();auto finish=release.get_future();
   std::atomic<bool> close_returned=false;
   auto reader=std::async(std::launch::async,[&]{
    auto r=granted();const bool ok=r.ok();admitted.set_value();
    finish.wait();if(ok)same(r);
    return ok; // Destruction occurs on the acquiring worker, before completion.
   });
   const auto ready=entered.wait_for(std::chrono::seconds(5))==std::future_status::ready;
   bool blocked=false;
   auto unpin=std::async(std::launch::async,[&]{
    std::this_thread::sleep_for(std::chrono::milliseconds(30));blocked=!close_returned.load();release.set_value();
   });
   // Close remains on the thread that originally opened the fixture device.
   const auto closed=file.device->Close();close_returned=true;unpin.get();const bool ok=reader.get();
   Check(ready&&blocked&&ok&&closed.ok(),"every member stays fenced throughout actual lease consumption");
   Check(file.device->Open(path,readonly?d::FileOpenMode::open_existing_read_only:d::FileOpenMode::open_existing).ok(),
    "reopen exact member after retained-lease Close");released_payload();
  }
 }
#else
 (void)fault_route;(void)fault_shard;(void)fault_shards;
#endif
 valid.memory={};valid.Empty();
}

void NoSelector(std::span<const d::NativeFilespaceDevice> files,u64 allowance){
 constexpr std::size_t capacity=2*1024*1024;c::Grant grant(capacity);
 const auto r=db::AcquireNativeSelectedCheckpointMemoryLease(Id(1),files,Id(2),{allowance,capacity},grant.memory,grant.binding);
 Check(!r.ok()&&!r.lease&&r.error==E::selection_failure&&
  r.selection_error==db::NativeCheckpointSelectionError::slot_binding_mismatch&&r.physical_bytes_read,
  "bootstrap without paired selectors cannot manufacture a selected read lease");
 Check(!grant.memory.Snapshot().allocated_bytes&&!grant.manager.Snapshot().current_bytes,
  "missing-selector refusal releases actual payload");grant.memory={};grant.Empty();
}
} // namespace selected_lease_memory
