// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
// Included after the independent selected-source and actual-grant test helpers.
namespace owned_source_memory {
namespace d=scratchbird::storage::disk;
namespace c=checkpoint_inventory_memory;
using Source=db::NativeOwnedCheckpointSource;
using E=db::NativeOwnedSourceError;
static_assert(!std::is_copy_constructible_v<db::NativeOwnedCheckpointMemoryHandle>);
static_assert(std::is_move_constructible_v<db::NativeOwnedCheckpointMemoryHandle>);
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
};
// Common-envelope storage fixture only, not ordinary-table family admission or
// a catalog publication operation. Install before source ownership is acquired.
db::NativeCatalogLeafResult PopulatedLeaf(db::NativeCatalogLeafPage leaf,const mga::TransactionIdentity& creator){
 namespace catalog=scratchbird::core::catalog;
 Check(leaf.body.rows.empty(),"populate only a fresh empty fixture leaf");
 for(unsigned i=0;i<2;++i){
  page::RowDataRecord row;row.storage_generation=1;
  row.row_uuid={UuidKind::row,Id(60000+i)};row.version_uuid=Id(60010+i);
  row.transaction_uuid=creator.transaction_uuid;row.local_transaction_id=creator.local_id.value;
  row.internal_row_ordinal=row.stable_slot_id=i+1;
  catalog::CatalogMetadataVersion metadata;
  metadata.record.header.kind=catalog::CatalogRecordKind::sql_object;
  metadata.record.header.row_uuid=row.row_uuid;
  metadata.record.header.object_uuid={UuidKind::object,Id(60020+i)};
  metadata.record.header.parent_uuid={UuidKind::schema,Id(60030)};
  metadata.owning_schema_uuid=metadata.record.header.parent_uuid;
  metadata.owner_uuid={UuidKind::principal,Id(60031)};metadata.audit_uuid={UuidKind::object,Id(60032)};
  metadata.creator_transaction_uuid=creator.transaction_uuid;
  metadata.creator_local_transaction_id=creator.local_id.value;
  metadata.definition_version=metadata.schema_epoch=metadata.security_epoch=metadata.catalog_generation=1;
  metadata.dependency_generation=metadata.invalidation_generation=1;
  metadata.lifecycle=catalog::CatalogObjectLifecycle::active;metadata.status=catalog::CatalogObjectStatus::active;
  metadata.trace_search_key="GUARDED-LEAF-STORAGE-FIXTURE";
  metadata.object_subtype="application";metadata.retention_class="catalog_history";
  metadata.record.payload="guarded-leaf-common-envelope";
  const auto encoded=catalog::EncodeCatalogMetadataVersion(metadata);
  Check(encoded.ok(),"complete populated common metadata envelope");
  page::RowDataCell cell;cell.column_ordinal=1;
  cell.value.type_id=scratchbird::core::datatypes::CanonicalTypeId::binary;
  cell.value.payload=encoded.bytes;row.cells.push_back(std::move(cell));leaf.body.rows.push_back(std::move(row));
 }
 return db::EncodeNativeCatalogLeaf(leaf);
}
void Checks(std::span<const d::NativeFilespaceDevice> borrowed,u64 allowance,bool deep=false,bool via_route=false,int fault_route=9,unsigned fault_shard=0,unsigned fault_shards=1,bool populated=false){
 std::vector<d::NativeFilespaceDevice> original(borrowed.begin(),borrowed.end());
 if(via_route){const auto primary=std::find_if(original.begin(),original.end(),[](const auto& f){return f.filespace_uuid==Id(2);});
  Check(primary!=original.end(),"route cohort includes the actual primary");std::rotate(original.begin(),primary,primary+1);}
 db::NativeBoundCheckpointSelection expected_selection;db::NativeCheckpointDirectoryResult expected_directory;
 d::FilespaceRootReference root;u64 images=0;
 {
  auto result=db::AcquireNativeSelectedCheckpointReadLease(Id(1),original,Id(2),allowance);
  Check(result.ok(),"independent full selected source before exclusive memory ownership");
  expected_selection=result.lease->selection();expected_directory=result.lease->directory();
  root=result.lease->checkpoint();images=result.lease->retained_image_bytes();
 }
 std::vector<page::NativeCatalogRootReference> leaf_refs;
 std::vector<Bytes> leaf_images;
 if(fault_route==9){
  const auto& roots=expected_selection.checkpoint_inventory.checkpoint->roots;
  const auto catalog=std::find_if(roots.begin(),roots.end(),[](const auto& r){return r.role==5;});
  Check(catalog!=roots.end(),"actual selected checkpoint has a catalog root");
  const auto member=std::find_if(original.begin(),original.end(),[&](const auto& f){return f.filespace_uuid==catalog->page.filespace_uuid;});
  Check(member!=original.end(),"actual catalog filespace is retained");
  const d::FilespaceRootReference reference{2,catalog->page_type,catalog->page.filespace_uuid,catalog->page.page_number,
    catalog->page.page_generation,catalog->page.page_size_profile_uuid,catalog->object_uuid};
  const auto observed=page::ReadNativeCatalogRootFromOpenDevice(*member->device,Id(1),reference);
  Check(observed.ok(),"actual catalog root before governed source adoption");
  for(const auto& ref:observed.root->roots){
   Check(ref.page_type==6,"genesis catalog fixture uses actual direct heads");
   auto leaf=db::ReadNativeCatalogLeafFromOpenDevice(*member->device,Id(1),ref);
   Check(leaf.ok(),"independent owning leaf read before guarded composition");
   if(populated&&ref.role==1){
    leaf=PopulatedLeaf(*leaf.page,expected_selection.checkpoint_inventory.inventory.entries.front().identity);
    Check(leaf.ok()&&leaf.metadata.size()==2,"canonical populated object-catalog fixture image");
    const auto write=member->device->WriteAt(ref.page.page_number*leaf.bytes.size(),leaf.bytes.data(),leaf.bytes.size());
    Check(write.ok()&&write.bytes_transferred==leaf.bytes.size()&&member->device->Sync().ok(),
      "persist populated fixture before acquiring owned source");
    const auto reread=db::ReadNativeCatalogLeafFromOpenDevice(*member->device,Id(1),ref);
    Check(reread.ok()&&reread.bytes==leaf.bytes&&reread.metadata.size()==2,"independent owning decode of persisted populated leaf");
   }
   leaf_refs.push_back(ref);leaf_images.push_back(leaf.bytes);
  }
 }
 // Some later operation proofs require the omitted member themselves. Preserve
 // their earlier exact source failure; only a fully valid partial reader may
 // reach the owning layer's separate complete-directory count check.
 std::vector<d::NativeFilespaceDevice> partial_files(original.begin(),original.end()-1);
 auto partial=db::AcquireNativeSelectedCheckpointReadLease(Id(1),partial_files,Id(2),allowance);
 const bool partial_valid=partial.ok();partial.lease.reset();
 Check(partial_valid||partial.error==db::NativeSelectedCheckpointReadError::selection_failure||
  partial.error==db::NativeSelectedCheckpointReadError::directory_failure,
  "independent missing-member fixture has a valid prefix or exact source proof refusal");
 std::size_t source_bytes=0;
 {
  Meter meter;std::vector<std::unique_ptr<d::FileDevice::ReadLatencyBatch>> owners;
  std::vector<d::FileDevice::ReadLatencyBatch*> batches;
  for(const auto& f:original){owners.push_back(std::make_unique<d::FileDevice::ReadLatencyBatch>(*f.device));batches.push_back(owners.back().get());}
  const auto selected=db::detail::ReadNativeBoundCheckpointSelectionBacked(Id(1),original,Id(2),allowance,batches,meter);
  const auto directory=db::detail::VerifyCurrentNativeCheckpointDirectoryBacked(Id(1),original,root,
      allowance-selected.selection.retained_image_bytes,batches,meter);
  Check(selected.ok()&&directory.ok(),"independent bounded workspace measurement for complete owner reads");
  source_bytes=meter.used+sizeof(db::NativeSelectedCheckpointMemoryLease)+
    original.size()*(sizeof(d::FileDevice::ReadLatencyBatch)+sizeof(d::FileDevice::ReadLatencyBatch*)+
      sizeof(std::unique_lock<std::recursive_mutex>)+sizeof(d::NativeFilespaceDevice)+4*alignof(std::max_align_t))+
    expected_selection.allocation.pages.size()*sizeof(db::NativeReadAllocationPage)+
    expected_directory.directory.pages.size()*sizeof(db::NativeReadDirectoryPage)+1024;
 }
 std::vector<std::string> paths;std::vector<bool> readonly;
 for(const auto& f:original){
  {auto guard=f.device->AcquireOperationGuard();paths.push_back(f.device->path());readonly.push_back(f.device->read_only());}
  Check(f.device->Close().ok(),"close fixture before independent exclusive adoption");
 }
 std::vector<std::unique_ptr<d::FileDevice>> files;
#ifdef SB_NATIVE_ROUTE_SOURCE_TESTS
 namespace server=scratchbird::server;
 server::DatabaseOwnershipRequest route_request;route_request.database_path=paths.front();
 server::DatabaseOwnershipResult route_owner;d::RouteSourceTransitionResult transition;
 std::shared_ptr<d::RouteOwnershipLease> legacy;bool first_open=true;
 Check(original.front().filespace_uuid==Id(2),"actual fixture primary is first for private route handoff");
#else
 Check(!via_route,"route-owned memory fixture requires actual route issuer linkage");
#endif
 const auto caller_count=original.size()-(via_route?1:0);
 const auto open=[&]{Check(files.empty(),"previous native owned handles released");
#ifdef SB_NATIVE_ROUTE_SOURCE_TESTS
  if(via_route){
   transition.transition.reset();route_owner.lock.reset();
   route_owner=server::AcquireDatabaseOwnership(route_request);
   Check(route_owner.acquired&&route_owner.lock,"actual primary route owner for governed native source");
   if(deep&&first_open&&fault_route==9)legacy=d::RouteOwnershipLease::Borrow(paths.front()+".sb.route.owner.lock");
  }
#endif
  for(std::size_t i=via_route?1:0;i<paths.size();++i){auto file=std::make_unique<d::FileDevice>();
   Check(file->Open(paths[i],d::FileOpenMode::open_existing).ok(),"open actual native owner member");
   files.push_back(std::move(file));}
#ifdef SB_NATIVE_ROUTE_SOURCE_TESTS
  if(via_route){transition=route_owner.lock->BeginNativeSourceTransition();
   Check(transition.ok(),"actual private route transition for memory-backed source");first_open=false;}
#endif
 };
 const auto restore=[&]{files.clear();
#ifdef SB_NATIVE_ROUTE_SOURCE_TESTS
  transition.transition.reset();route_owner.lock.reset();
#endif
  for(std::size_t i=0;i<original.size();++i)
  Check(original[i].device->Open(paths[i],readonly[i]?d::FileOpenMode::open_existing_read_only:d::FileOpenMode::open_existing).ok(),
   "original actual fixture restored after owned-source release");
 };
 const auto adopt=[&](const Uuid& database,const Uuid& primary,
     std::vector<std::unique_ptr<d::FileDevice>>& members,db::NativeOwnedCheckpointMemoryLimits limits,
     db::NativeStorageMemory& persistent,const db::NativeStorageMemoryBinding& persistent_binding,
     db::NativeStorageMemory& transient,const db::NativeStorageMemoryBinding& transient_binding){
#ifdef SB_NATIVE_ROUTE_SOURCE_TESTS
  if(via_route)return Source::AdoptRouteWithMemory(transition.transition,database,primary,members,limits,
    persistent,persistent_binding,transient,transient_binding);
#endif
  return Source::AdoptWithMemory(database,primary,members,limits,persistent,persistent_binding,transient,transient_binding);
 };
 if(!fault_shards||fault_shard>=fault_shards||fault_route<0||fault_route>9)throw std::invalid_argument("owned source fault shard");
 constexpr std::size_t owner_bytes=4096;
 db::NativeOwnedCheckpointMemoryLimits limits{owner_bytes,{allowance,source_bytes}};
 open();
 c::Grant transient(source_bytes);
 const auto read=[&](const auto& owner){return Source::ReadWithMemory(owner,limits.source,transient.memory,transient.binding);};
 const auto parity=[&](const auto& result){
  Check(result.ok(),"actual-memory retained owned source");
  selected_lease_memory::Same(expected_selection,expected_directory,root,images,result.lease.source());
  Check(result.lease.devices().size()==original.size(),"owned read retains exact complete member cohort");
  for(std::size_t i=0;i<original.size();++i)
   Check(result.lease.devices()[i].filespace_uuid==original[i].filespace_uuid&&
    result.lease.devices()[i].page_size_profile_uuid==original[i].page_size_profile_uuid,
    "all persistent native identities derived from actual bootstraps");
 };
 const auto no_transient_payload=[&]{
  Check(!transient.manager.Snapshot().current_bytes&&!transient.memory.Snapshot().allocated_bytes,
   "every released or refused selected read returns physical transient bytes");
 };

 if(fault_route!=9){
  struct Counts {unsigned long allocations=0;unsigned reads=0,hashes=0,sizes=0;
   u64 memory_loss=0,device_loss=0;};
  Counts baseline;
  const auto resource_failure=[](const auto& r){
   return r.error==E::resource_exhausted||
    (r.error==E::memory_failure&&r.memory_error==db::NativeStorageMemoryError::resource_exhausted)||
    (r.error==E::route_admission&&r.route_error==d::RouteSourceTransitionError::resource_exhausted)||
    (r.error==E::source_failure&&
      (r.memory_source_error==db::NativeSelectedCheckpointMemoryError::resource_exhausted||
       r.memory_source_error==db::NativeSelectedCheckpointMemoryError::memory_allocation_failure||
       r.selection_error==db::NativeCheckpointSelectionError::resource_exhausted||
       r.checkpoint_error==db::NativeCheckpointError::resource_exhausted));
  };
  const auto one=[&](int kind,unsigned site,std::recursive_mutex* fence=nullptr){
   c::Grant persistent(owner_bytes);Counts counts;
   unsigned long old_loss=0;for(const auto& f:files)old_loss+=f->failed_io_latency_observations();
   const auto old_memory_loss=persistent.manager.Snapshot().telemetry_truncation_count+
     transient.manager.Snapshot().telemetry_truncation_count;
   allocations=reads=writes=syncs=0;history_observed_read_bytes=0;hash_seen=0;
   hash_fault=0;hash_active=false;read_fault=0;allocation_budget=-1;
#ifdef NATIVE_HISTORICAL_BUNDLE_SIZE_FAULTS
   historical_stats=0;historical_stat_fault=0;historical_stat_counting=true;
#endif
   io_counting=true;hash_counting=true;counting=true;
   if(kind==0)allocation_budget=site;
   if(kind==1)read_fault=site;
   if(kind>=2&&kind<=6){hash_fault=kind-1;hash_target=site;}
#ifdef NATIVE_HISTORICAL_BUNDLE_SIZE_FAULTS
   if(kind==7)historical_stat_fault=site;
#endif
   memory_probes=memory_locked_probes=0;
   // Physical failure diagnostics may allocate control-plane strings under
   // their I/O guard. Backing/provider lifetime is the cleanup invariant.
   memory_probe_backing_only=fence&&kind==1;memory_probe_mutex=fence;
   auto result=adopt(Id(1),Id(2),files,limits,persistent.memory,persistent.binding,transient.memory,transient.binding);
   memory_probe_mutex=nullptr;memory_probe_backing_only=false;
   counts.allocations=allocations;counts.reads=reads;counts.hashes=hash_seen;
   const auto remaining=allocation_budget;const bool hash_consumed=!hash_fault;
   const auto actual_bytes=history_observed_read_bytes;
   const bool no_effects=!writes&&!syncs;
#ifdef NATIVE_HISTORICAL_BUNDLE_SIZE_FAULTS
   counts.sizes=historical_stats;historical_stat_counting=false;historical_stat_fault=0;
#endif
   counting=io_counting=hash_counting=false;allocation_budget=-1;read_fault=hash_fault=0;hash_active=false;
   Check(no_effects&&result.physical_bytes_read==actual_bytes,"owner admission preserves exact physical bytes and has no write/sync effects");
   if(kind<0)Check(result.ok(),"complete real-file owner fault baseline succeeds");
   if(kind==0){
    Check(remaining<0,"every indexed actual owner allocation fault is consumed");
    if(!result.ok())Check(resource_failure(result),"required owner allocation failure retains its typed resource classification");
   }
   if(kind==1)Check(!result.ok()&&!result.owner&&counts.reads>=site&&!result.io_status.ok()&&
     result.io_diagnostic.diagnostic_code=="SB-STORAGE-DISK-READ-SHORT"&&
     ((result.error==E::bootstrap_failure&&result.bootstrap_error==d::FilespaceBootstrapError::io_failure)||
      result.error==E::source_failure),"every bootstrap or selected read failure preserves its exact physical receipt");
   if(kind>=2&&kind<=6){
    Check(hash_consumed&&!result.ok()&&!result.owner,"every digest-provider phase is consumed without partial ownership");
    if(site<=original.size())
     Check(result.error==E::bootstrap_failure&&result.bootstrap_error==d::FilespaceBootstrapError::hash_provider_failure,
       "bootstrap digest failure preserves its exact physical framing diagnostic");
    else Check(result.error==E::source_failure&&
      (result.selection_error==db::NativeCheckpointSelectionError::hash_failure||
       result.checkpoint_error==db::NativeCheckpointError::hash_failure||
       (result.checkpoint_error==db::NativeCheckpointError::directory_failure&&result.directory_error==page::NativeDirectoryError::hash_failure)),
       "selected-source digest failure preserves its exact nested hash diagnostic");
   }
   if(kind==7)Check(!result.ok()&&!result.owner&&counts.sizes>=site&&
     result.error==E::source_failure&&!result.io_status.ok(),"every actual size failure is retained without an owner prefix");
   if(fence)Check(memory_probes&&!memory_locked_probes,"actual owner admission and refused cleanup callbacks occur outside source guards");
   if(result.ok()){
    Check(files.empty()&&!persistent.memory.Matches(persistent.binding),"successful fault baseline consumes exclusive devices and real persistent grant");
    {
     const auto held=read(result.owner);parity(held);
     unsigned long loss=0;for(const auto& f:held.lease.devices())loss+=f.device->failed_io_latency_observations();
     counts.device_loss=loss-old_loss;
     counts.memory_loss=persistent.manager.Snapshot().telemetry_truncation_count+
       transient.manager.Snapshot().telemetry_truncation_count-old_memory_loss;
     if(kind==0){
      // This component fixture has no activated node metric registry. Each
      // memory allocation already records a refused observation. An injected
      // snapshot failure can replace that same recorded rejection, rather
      // than adding a second event. Compare complete baseline deltas, not a
      // fictitious requirement that every optional failure adds one counter.
      Check(counts.memory_loss>=baseline.memory_loss&&counts.memory_loss<=baseline.memory_loss+1&&
       counts.device_loss>=baseline.device_loss&&counts.device_loss<=baseline.device_loss+1&&
       counts.memory_loss+counts.device_loss>0&&
       counts.memory_loss-baseline.memory_loss+counts.device_loss-baseline.device_loss<=1,
       "successful allocation fault preserves bounded recorded memory/device observation loss");
      Check(counts.reads==baseline.reads&&counts.hashes==baseline.hashes&&counts.sizes==baseline.sizes&&
       persistent.manager.Snapshot().current_bytes==owner_bytes&&
       transient.manager.Snapshot().current_bytes==source_bytes,
       "optional observation failure cannot skip validation or required charged ownership");
     }
    }
    result.owner.reset();persistent.Empty();open();
   }else{
    Check(!result.owner&&files.size()==caller_count&&persistent.memory.Matches(persistent.binding)&&
     !persistent.manager.Snapshot().current_bytes,"failed admission preserves original files and grant with no owned payload");
    for(const auto& f:files)Check(f&&f->is_open(),"failed admission retains every original member handle");
    persistent.memory={};persistent.Empty();
   }
   no_transient_payload();return counts;
  };
  (void)one(-1,0); // Warm legitimate shared observation registrations.
  const auto measured=one(-1,0);baseline=measured;
  Check(measured.allocations&&measured.reads&&measured.hashes,"measure every actual owner source fault dimension");
  const auto included=[&](unsigned long site){return site%fault_shards==fault_shard;};
  if(fault_route==0)for(unsigned long site=0;site<measured.allocations;++site)
    if(included(site))one(0,static_cast<unsigned>(site));
  if(fault_route==1)for(unsigned site=1;site<=measured.reads;++site)
    if(included(site-1))one(1,site);
  if(fault_route>=2&&fault_route<=6)for(unsigned site=1;site<=measured.hashes;++site)
    if(included(site-1))one(fault_route,site);
#ifdef NATIVE_HISTORICAL_BUNDLE_SIZE_FAULTS
  if(fault_route==7){Check(measured.sizes,"actual size observations exist");
   for(unsigned site=1;site<=measured.sizes;++site)if(included(site-1))one(7,site);}
#else
  Check(fault_route!=7,"size-fault tests require actual native stat wrapping");
#endif
  if(fault_route==8){
   for(std::size_t i=0;i<caller_count;++i){
    // Success consumes/reopens the cohort, so resolve the next actual mutex
    // afresh rather than retaining a pointer across source destruction.
    std::recursive_mutex* mutex=nullptr;{auto guard=files[i]->AcquireOperationGuard();mutex=guard.mutex();}
    one(-1,0,mutex);
    {auto guard=files[i]->AcquireOperationGuard();mutex=guard.mutex();}
    one(1,1,mutex);
   }
  }
  std::cout<<"owned source memory fault sites allocations="<<measured.allocations<<" reads="<<measured.reads
   <<" hashes="<<measured.hashes<<" sizes="<<measured.sizes<<'\n';
  transient.memory={};transient.Empty();restore();return;
 }

 std::size_t exact_owner=0;
 {
  c::Grant persistent(owner_bytes);
#ifdef SB_NATIVE_ROUTE_SOURCE_TESTS
  if(legacy){
   const auto busy=adopt(Id(1),Id(2),files,limits,persistent.memory,persistent.binding,transient.memory,transient.binding);
   Check(busy.error==E::route_admission&&busy.route_error==d::RouteSourceTransitionError::not_drained&&
    !busy.owner&&!busy.physical_bytes_read&&files.size()==caller_count&&persistent.memory.Matches(persistent.binding),
    "actual legacy borrower prevents route-owned source admission without consuming grants");
   legacy.reset();
  }
#endif
  auto admitted=adopt(Id(1),Id(2),files,limits,persistent.memory,persistent.binding,transient.memory,transient.binding);
  Check(admitted.ok()&&files.empty()&&admitted.owner->memory_backed(),"complete native source takes all devices and persistent memory once");
  Check(persistent.memory.CheckBinding(persistent.binding)==db::NativeStorageMemoryError::invalid_grant&&
   persistent.manager.Snapshot().current_bytes==owner_bytes&&persistent.ledger.Snapshot().current_bytes==owner_bytes,
   "persistent owner payload actually charged and original wrapper consumed");
  exact_owner=admitted.owner->owner_backing_bytes_used();
  Check(exact_owner&&exact_owner<=owner_bytes,"actual ownership and descriptor layout bounded");
#ifdef SB_NATIVE_ROUTE_SOURCE_TESTS
  if(via_route){
   const auto duplicate=adopt(Id(1),Id(2),files,limits,persistent.memory,persistent.binding,transient.memory,transient.binding);
   Check(duplicate.error==E::route_admission&&duplicate.route_error==d::RouteSourceTransitionError::already_transferred&&!duplicate.owner,
    "actual route source transition consumes once even after the grant wrapper moved");
   route_owner.lock->release();
  }
#endif
  Check(Source::Read(admitted.owner,allowance).error==E::memory_required,"legacy read cannot bypass actual-grant source admission");
  {const auto held=read(admitted.owner);parity(held);}
  for(std::size_t i=0;i<leaf_refs.size();++i){
   const auto& ref=leaf_refs[i];
   const auto bytes=db::NativeCatalogLeafWorkspaceBytes(ref.page.page_size_profile_uuid);
   c::Grant catalog_memory(bytes);
   for(unsigned field=0;field<4;++field){
    auto wrong=catalog_memory.binding;
    const std::array<Uuid*,4> ids{&wrong.database_uuid,&wrong.operation_uuid,&wrong.owner_uuid,&wrong.context_uuid};
    *ids[field]=Id(65502);
    const auto rejected=db::PrepareNativeCatalogLeafRead(ref.page.page_size_profile_uuid,catalog_memory.memory,wrong);
    Check(!rejected.ok()&&!rejected.workspace&&rejected.memory_error==db::NativeStorageMemoryError::invalid_binding&&
      !catalog_memory.manager.Snapshot().current_bytes,"every binary grant ownership dimension checked before leaf backing admission");
   }
   {
    c::Grant short_grant(bytes-1);
    const auto rejected=db::PrepareNativeCatalogLeafRead(ref.page.page_size_profile_uuid,short_grant.memory,short_grant.binding);
    Check(!rejected.ok()&&!rejected.workspace,"one-byte-short actual catalog grant has no fallback");
    short_grant.memory={};short_grant.Empty();
   }
   {
    allocation_budget=0;
    const auto rejected=db::PrepareNativeCatalogLeafRead(ref.page.page_size_profile_uuid,catalog_memory.memory,catalog_memory.binding);
    const auto consumed=allocation_budget;allocation_budget=-1;
    Check(consumed<0&&!rejected.ok()&&!rejected.workspace&&!catalog_memory.manager.Snapshot().current_bytes,
      "failed catalog backing allocation leaves no retained memory or fabricated workspace");
   }
   auto prepared=db::PrepareNativeCatalogLeafRead(ref.page.page_size_profile_uuid,catalog_memory.memory,catalog_memory.binding);
   Check(prepared.ok(),"catalog backing admitted before owned source guard");
   auto moved=std::move(prepared.workspace);
   {
    const auto held=read(admitted.owner);parity(held);
    const auto moved_from=db::NativeCatalogLeafLeaseReader::Read(held.lease.source(),ref,prepared.workspace);
    Check(!moved_from.ok()&&!moved_from.bytes_read,"moved-from admitted backing refuses before I/O");
    prepared.workspace=std::move(moved);
    allocation_budget=0;
    const auto observed=db::NativeCatalogLeafLeaseReader::Read(held.lease.source(),ref,prepared.workspace);
    const auto remaining=allocation_budget;allocation_budget=-1;
    Check(remaining==0&&observed.ok()&&observed.image.size()==leaf_images[i].size()&&
      std::equal(observed.image.begin(),observed.image.end(),leaf_images[i].begin()),
      "complete actual catalog leaf under retained owned source uses only pre-admitted memory");
    Check(observed.bytes_read==d::kFilespaceBootstrapBytes+2*observed.image.size(),
      "guarded reader reports exact physical source bytes");
    if(populated&&ref.role==1){
     Check(observed.leaf.page->metadata.size()==2&&observed.leaf.page->body.rows.size()==2,
       "guarded populated reader retains every metadata row without heap fallback");
     for(const auto& record:observed.leaf.page->metadata){
      Check(record.row_index<2&&record.version_uuid==Id(60010+record.row_index)&&
        record.metadata.record.header.object_uuid.value==Id(60020+record.row_index)&&
        record.metadata.record.payload=="guarded-leaf-common-envelope",
        "populated borrowed metadata preserves exact binary identity and complete payload");
     }
    }
    for(unsigned fault=0;fault<4;++fault){
     auto wrong=ref;
     if(fault==0)++wrong.page.page_generation;
     if(fault==1)wrong.page.filespace_uuid=Id(65500);
     if(fault==2)wrong.object_uuid=Id(65501);
     if(fault==3)wrong.page.page_size_profile_uuid={};
     allocation_budget=0;
     const auto refused=db::NativeCatalogLeafLeaseReader::Read(held.lease.source(),wrong,prepared.workspace);
     const auto left=allocation_budget;allocation_budget=-1;
     Check(left==0&&!refused.ok()&&refused.image.empty()&&!refused.leaf.page,
       "wrong guarded leaf identity/profile has no partial image or allocating fallback");
    }
    db::NativeCatalogLeafPreparedMemory absent;
    const auto missing=db::NativeCatalogLeafLeaseReader::Read(held.lease.source(),ref,absent);
    Check(!missing.ok()&&!missing.bytes_read,"missing admitted catalog backing refuses before I/O");
    for(unsigned fault=0;fault<2;++fault)for(unsigned site=1;site<=3;++site){
     reads=writes=syncs=0;history_observed_read_bytes=0;io_counting=true;
     if(fault==0)read_fault=site;else corrupt_read=site;
     const auto failed=db::NativeCatalogLeafLeaseReader::Read(held.lease.source(),ref,prepared.workspace);
     io_counting=false;read_fault=corrupt_read=0;
     Check(reads>=site&&!failed.ok()&&failed.image.empty()&&!failed.leaf.page&&
       !writes&&!syncs&&failed.bytes_read==history_observed_read_bytes,
       "every physical read/corruption failure has exact effects and no successful prefix under the same lease");
     if(fault==0)Check(failed.error==db::NativeCatalogLeafMemoryError::io_failure&&!failed.io_status.ok()&&
       failed.io_diagnostic.diagnostic_code=="SB-STORAGE-DISK-READ-SHORT",
       "guarded read preserves full physical I/O diagnostic");
    }
    allocation_budget=0;
    const auto retry=db::NativeCatalogLeafLeaseReader::Read(held.lease.source(),ref,prepared.workspace);
    const auto left=allocation_budget;allocation_budget=-1;
    Check(left==0&&retry.ok(),"same retained source remains usable after failed leaf observations");
   }
   prepared={};catalog_memory.memory={};catalog_memory.Empty();
  }
  no_transient_payload();admitted.owner.reset();persistent.Empty();
 }
 open();
 {
  c::Grant persistent(owner_bytes);
  const auto fail=[&](auto result){
   Check(!result.ok()&&!result.owner&&files.size()==caller_count&&
    persistent.memory.Matches(persistent.binding)&&!persistent.manager.Snapshot().current_bytes,
    "refused owner preserves all original devices and grant without retained payload");
   for(const auto& f:files)Check(f&&f->is_open(),"every refused native member remains open and caller-owned");
   no_transient_payload();return result;
  };
  for(unsigned field=0;field<3;++field){
   auto zero=limits;
   if(field==0)zero.maximum_owner_backing_bytes=0;
   if(field==1)zero.source.maximum_verification_image_bytes=0;
   if(field==2)zero.source.maximum_backing_bytes=0;
   const auto result=fail(adopt(Id(1),Id(2),files,zero,persistent.memory,persistent.binding,transient.memory,transient.binding));
   Check(result.error==E::invalid_request&&!result.physical_bytes_read,"zero owner or selected allowance refuses before effects");
  }
  for(unsigned field=0;field<2;++field){
   const auto result=fail(adopt(field?Id(1):Uuid{},field?Uuid{}:Id(2),files,limits,
     persistent.memory,persistent.binding,transient.memory,transient.binding));
   Check(result.error==E::invalid_request&&!result.physical_bytes_read,"nil native ownership identity is not admitted");
  }
  {
   byte original_byte=0;auto& member=*files.back();
   Check(member.ReadAt(0,&original_byte,1).ok(),"actual bootstrap byte before corruption");
   const byte damaged=original_byte^1;
   Check(member.WriteAt(0,&damaged,1).ok()&&member.Sync().ok(),"corrupt actual owned member bootstrap");
   const auto result=fail(adopt(Id(1),Id(2),files,limits,persistent.memory,persistent.binding,transient.memory,transient.binding));
   Check(result.error==E::bootstrap_failure&&result.bootstrap_error!=d::FilespaceBootstrapError::none&&result.physical_bytes_read,
    "malformed actual bootstrap preserves typed failure without ownership transfer");
   byte retained=0;Check(member.ReadAt(0,&retained,1).ok()&&retained==damaged,"refused admission does not repair source");
   Check(member.WriteAt(0,&original_byte,1).ok()&&member.Sync().ok(),"restore exact member bootstrap");
  }
  Check(files.back()->Close().ok(),"closed owned member fixture");
  {
   const auto result=adopt(Id(1),Id(2),files,limits,persistent.memory,persistent.binding,transient.memory,transient.binding);
   Check(!result.ok()&&!result.owner&&result.error==E::device_ownership&&!result.physical_bytes_read&&
    files.size()==caller_count&&persistent.memory.Matches(persistent.binding),
    "closed independent member refuses without partial adoption");
  }
  Check(files.back()->Open(paths.back(),d::FileOpenMode::open_existing).ok(),"reopen actual closed member");
  auto altered=limits;altered.maximum_owner_backing_bytes=exact_owner-1;
  auto short_owner=fail(adopt(Id(1),Id(2),files,altered,persistent.memory,persistent.binding,transient.memory,transient.binding));
  Check(short_owner.error==E::resource_exhausted&&!short_owner.physical_bytes_read,"one-byte-short real ownership layout refuses before I/O");
  for(unsigned side=0;side<2;++side)for(unsigned field=0;field<4;++field){
   auto binding=persistent.binding;const std::array<Uuid*,4> ids{&binding.database_uuid,&binding.operation_uuid,&binding.owner_uuid,&binding.context_uuid};
   *ids[field]=Id(249);
   auto result=fail(adopt(Id(1),Id(2),files,limits,persistent.memory,
     side?persistent.binding:binding,transient.memory,side?binding:transient.binding));
   Check(result.error==E::memory_failure&&!result.physical_bytes_read,"all persistent and transient binary ownership dimensions checked before I/O");
  }
  auto invalid=fail(adopt(Id(1),Id(2),files,limits,persistent.memory,persistent.binding,persistent.memory,persistent.binding));
  Check(invalid.error==E::invalid_request&&!invalid.physical_bytes_read,"persistent and transient work require distinct actual grants");
  {auto missing=std::move(files.back());files.pop_back();
   const auto result=adopt(Id(1),Id(2),files,limits,persistent.memory,persistent.binding,transient.memory,transient.binding);
   Check(!result.ok()&&!result.owner,"missing member never produces a complete owned source");
   if(partial_valid)Check(result.error==E::incomplete_cohort,"valid partial inspection cannot become an owning source");
   else {
    const auto expected=partial.error==db::NativeSelectedCheckpointReadError::selection_failure?
      db::NativeSelectedCheckpointMemoryError::selection_failure:db::NativeSelectedCheckpointMemoryError::directory_failure;
    Check(result.error==E::source_failure&&result.memory_source_error==expected&&
     result.selection_error==partial.selection_error&&result.checkpoint_error==partial.checkpoint_error&&
     result.allocation_error==partial.allocation_error&&result.directory_error==partial.directory_error,
     "missing proof member preserves the exact earlier independent source diagnostic");
   }
   files.push_back(std::move(missing));no_transient_payload();}
  {auto saved=std::move(files.back());
   auto result=adopt(Id(1),Id(2),files,limits,persistent.memory,persistent.binding,transient.memory,transient.binding);
   Check(!result.ok()&&!result.owner&&result.error==E::device_ownership&&!result.physical_bytes_read&&
    persistent.memory.Matches(persistent.binding)&&!persistent.manager.Snapshot().current_bytes,
    "null owned member refuses before locks or grants");
   files.back()=std::move(saved);}
  {E result=E::none;std::thread wrong([&]{
    result=adopt(Id(1),Id(2),files,limits,persistent.memory,persistent.binding,transient.memory,transient.binding).error;
   });wrong.join();Check(result==E::device_ownership,"wrong opening thread cannot move independently locked devices");}
  {
   c::Grant short_owner_grant(owner_bytes-1);
   auto result=fail(adopt(Id(1),Id(2),files,limits,short_owner_grant.memory,short_owner_grant.binding,transient.memory,transient.binding));
   Check(result.error==E::memory_failure&&!result.physical_bytes_read&&!short_owner_grant.manager.Snapshot().current_bytes,
    "one byte short actual persistent grant refuses before source I/O");
   short_owner_grant.memory={};short_owner_grant.Empty();
  }
  {
   c::Grant short_source_grant(source_bytes-1);
   auto result=fail(adopt(Id(1),Id(2),files,limits,persistent.memory,persistent.binding,short_source_grant.memory,short_source_grant.binding));
   Check(result.error==E::source_failure&&result.memory_source_error==db::NativeSelectedCheckpointMemoryError::memory_allocation_failure&&
    result.physical_bytes_read==original.size()*d::SerializedFilespaceBootstrap{}.size(),
    "short actual transient grant retains exact completed bootstrap reads and no selected source");
   short_source_grant.memory={};short_source_grant.Empty();
  }
#ifdef SB_NATIVE_ROUTE_SOURCE_TESTS
  if(deep&&via_route){
   historical_read_pause=1;bool entered=false;d::RouteSourceTransitionError competing=d::RouteSourceTransitionError::none;
   std::thread withdrawer([&]{
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
    while(historical_read_pause!=2&&std::chrono::steady_clock::now()<deadline)std::this_thread::yield();
    entered=historical_read_pause==2;
    if(entered){
     std::vector<std::unique_ptr<d::FileDevice>> none;
     competing=adopt(Id(1),Id(2),none,limits,persistent.memory,persistent.binding,transient.memory,transient.binding).route_error;
     route_owner.lock->release();
    }
    historical_read_pause=3;
   });
   const auto withdrawn=adopt(Id(1),Id(2),files,limits,persistent.memory,persistent.binding,transient.memory,transient.binding);
   withdrawer.join();historical_read_pause=0;fail(std::move(withdrawn));
   Check(entered&&competing==d::RouteSourceTransitionError::admission_busy&&
    withdrawn.error==E::route_admission&&withdrawn.route_error==d::RouteSourceTransitionError::withdrawn,
    "actual issuer withdrawal during I/O and competing admission cannot publish a stale native owner");
   files.clear();open();
  }
#endif
  // Exact real owner layout, without changing the actual grant size.
  limits.maximum_owner_backing_bytes=exact_owner;
  auto admitted=adopt(Id(1),Id(2),files,limits,persistent.memory,persistent.binding,transient.memory,transient.binding);
  Check(admitted.ok()&&files.empty()&&persistent.manager.Snapshot().current_bytes==exact_owner,
   "exact persistent backing succeeds without hidden device-array storage");
  auto owner=std::move(admitted.owner);
  {const auto held=read(owner);parity(held);}
  limits.source.maximum_verification_image_bytes=images;
  {const auto held=read(owner);parity(held);}
  --limits.source.maximum_verification_image_bytes;
  {const auto denied=read(owner);Check(!denied.ok()&&!denied.lease,"one-byte-short owned selected source refuses without a prefix");}
  limits.source.maximum_verification_image_bytes=allowance;no_transient_payload();
  {
   auto held=read(owner);parity(held);d::FileDevice* physical=nullptr;
   for(const auto& member:held.lease.devices())if(member.filespace_uuid==root.filespace_uuid)physical=member.device;
   Check(physical,"actual selected checkpoint member retained");
   const auto* profile=d::FindCanonicalFilespacePageProfile(root.page_size_profile_uuid);
   Check(profile,"actual selected checkpoint page profile");
   u64 offset=(root.page_number+1)*u64{profile->page_size_bytes}-1;
   // The route-owned primary is genuinely readonly. Mutate a writable
   // secondary bootstrap instead; never bypass that native capability.
   if(via_route){physical=held.lease.devices().back().device;offset=0;}
   held.lease.Reset();byte original_byte=0;
   Check(physical->ReadAt(offset,&original_byte,1).ok(),"selected source corruption baseline");
   const byte damaged=original_byte^1;
   Check(physical->WriteAt(offset,&damaged,1).ok()&&physical->Sync().ok(),"explicit actual source mutation after adoption");
   const auto denied=read(owner);
   Check(!denied.ok()&&!denied.lease&&denied.error==E::source_failure&&
    denied.memory_source_error!=db::NativeSelectedCheckpointMemoryError::none,
    "memory-owned adoption is not cached source-currentness authority");
   byte observed=0;Check(physical->ReadAt(offset,&observed,1).ok()&&observed==damaged,"failed read has no repair effects");
   Check(physical->WriteAt(offset,&original_byte,1).ok()&&physical->Sync().ok(),"restore original selected source");
   const auto restored=read(owner);parity(restored);
  }

  if(deep){
   const auto replacement=paths.back()+".memory-owned-original";
   Check(!std::filesystem::exists(replacement),"private path-replacement fixture is absent");
   struct RestorePath {
    const std::string& original;const std::string& retained;bool renamed=false;
    ~RestorePath(){if(renamed){std::error_code ec;std::filesystem::remove(original,ec);
      ec.clear();std::filesystem::rename(retained,original,ec);}}
   } restore_path{paths.back(),replacement};
   std::filesystem::rename(paths.back(),replacement);restore_path.renamed=true;
   std::filesystem::copy_file(replacement,paths.back());
   std::filesystem::resize_file(paths.back(),0);
   {const auto held=read(owner);parity(held);
    Check(held.lease.devices().back().device->Size().size_bytes>0&&
      std::filesystem::file_size(paths.back())==0,
      "retained owner uses the original physical object rather than a replacement path");}
   std::filesystem::remove(paths.back());std::filesystem::rename(replacement,paths.back());
   restore_path.renamed=false;
  }

  if(deep){
   // A copied owner and live read fence in a child must not unlock the parent
   // or enter its inherited allocator/device mutexes during destruction.
   auto held=read(owner);parity(held);
   std::vector<int> inherited_descriptors;
   for(const auto& entry:std::filesystem::directory_iterator("/proc/self/fd")){
    std::error_code ec;const auto target=std::filesystem::read_symlink(entry.path(),ec).string();
    if(!ec&&std::any_of(paths.begin(),paths.end(),[&](const auto& path){
       return target==path||target.starts_with(path+".sb.");}))
     inherited_descriptors.push_back(std::stoi(entry.path().filename().string()));
   }
   Check(inherited_descriptors.size()>=paths.size(),"enumerate real inherited member descriptors");
   const auto child=::fork();Check(child>=0,"fork retained memory-owned source");
   if(child==0){
    ::alarm(5);
    const auto denied=read(owner);
    if(denied.error!=E::wrong_process||denied.ok())::_exit(71);
    held.lease.Reset();owner.reset();
#ifdef SB_NATIVE_ROUTE_SOURCE_TESTS
    transition.transition.reset();route_owner.lock.reset();
#endif
    // Check while still alive: process exit itself must not supply the proof.
    for(const auto fd:inherited_descriptors){struct stat observation{};
     errno=0;if(::fstat(fd,&observation)!=-1||errno!=EBADF)::_exit(72);}
    ::_exit(0);
   }
   int status=0;Check(::waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==0,
    "inherited owner and live read cleanup avoids parent synchronization");
   parity(held);held.lease.Reset();
   Check(persistent.manager.Snapshot().current_bytes==exact_owner,"child cleanup cannot release parent's persistent charge");
   for(const auto& path:paths){d::FileDevice competing;
    Check(!competing.Open(path,d::FileOpenMode::open_existing_read_only).ok(),"child cleanup preserves every actual parent OS lock");}
  }
  {
   auto held=read(owner);parity(held);
   // Revoke while the other thread owns the source guards. The revoker never
   // calls a provider under a guard acquired by that same thread.
   auto revoked=std::async(std::launch::async,[&]{
    const auto receipt=persistent.ledger.CleanupOwner(persistent.binding.owner_uuid.bytes);
    const auto denied=read(owner);
    return receipt.retained_bytes==owner_bytes&&persistent.manager.Snapshot().current_bytes==exact_owner&&
     denied.error==E::memory_failure&&!denied.lease&&!denied.physical_bytes_read;
   });
   Check(revoked.get(),"persistent grant revocation denies fresh reads without invalidating an admitted one");
   parity(held);
  }
  owner.reset();persistent.Empty();no_transient_payload();
 }
 open();
 {
  c::Grant persistent(owner_bytes);
  limits.maximum_owner_backing_bytes=owner_bytes;
  auto admitted=adopt(Id(1),Id(2),files,limits,persistent.memory,persistent.binding,transient.memory,transient.binding);
  Check(admitted.ok(),"fresh owner after prior final release");
  auto owner=std::move(admitted.owner);std::weak_ptr<Source> weak=owner;
  std::atomic<unsigned> phase{0};bool worker_ok=false;
  std::thread worker([retained=owner,&phase,&worker_ok,&read,&expected_selection]() mutable {
   auto held=read(retained);retained.reset();phase=1;phase.notify_all();
   while(phase.load()==1)phase.wait(1);
   worker_ok=held.ok()&&held.lease.source().selection().selection->checkpoint_sha256==expected_selection.selection->checkpoint_sha256;
   held.lease.Reset();
  });
  while(!phase.load())phase.wait(0);
  owner->Withdraw();const auto denied=read(owner);owner.reset();
  const bool pinned=!weak.expired();phase=2;phase.notify_all();worker.join();
  Check(pinned&&denied.error==E::withdrawn&&!denied.physical_bytes_read&&worker_ok&&weak.expired(),
   "last worker releases source guards and backing before closing owned devices");
  persistent.Empty();no_transient_payload();
 }
 transient.memory={};transient.Empty();restore();
}
} // namespace owned_source_memory
