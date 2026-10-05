// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
// Storage fixtures publish control transitions only; this is not an ordinary
// table allocation or a page reservation integration test.
void DirectoryAllocationLease(unsigned scenario,unsigned primary,unsigned secondary){
 constexpr usize capacity=4*1024*1024;
 DirectoryHistoryFixture f(primary,secondary,false,scenario?scenario+1:2,scenario>=2,true,true,scenario!=0);
 auto& files=f.t.fixture.devices;
 const u64 allowance=16*1024*std::max<u64>(f.t.fixture.size,d::kCanonicalFilespacePageProfiles[secondary].page_size_bytes);
 usize source_bytes=0;
 {
  owned_source_memory::Meter meter;
  std::vector<std::unique_ptr<d::FileDevice::ReadLatencyBatch>> owners;
  std::vector<d::FileDevice::ReadLatencyBatch*> batches;
  for(const auto& file:files){owners.push_back(std::make_unique<d::FileDevice::ReadLatencyBatch>(*file.device));batches.push_back(owners.back().get());}
  const auto selected=db::detail::ReadNativeBoundCheckpointSelectionBacked(Id(1),files,Id(2),allowance,batches,meter);
  Check(selected.ok(),"measure actual complete source backing before admission");
  const auto& s=*selected.selection.selection;
  const d::FilespaceRootReference root{9,0x300,s.checkpoint.filespace_uuid,s.checkpoint.page_number,
    s.checkpoint.page_generation,s.checkpoint.page_size_profile_uuid,s.checkpoint_object_uuid};
  const auto directory=db::detail::VerifyCurrentNativeCheckpointDirectoryBacked(Id(1),files,root,
    allowance-selected.selection.retained_image_bytes,batches,meter);
  Check(directory.ok(),"measure actual directory backing before admission");
  source_bytes=meter.used+sizeof(db::NativeSelectedCheckpointMemoryLease)+
    files.size()*(sizeof(d::FileDevice::ReadLatencyBatch)+sizeof(d::FileDevice::ReadLatencyBatch*)+
      sizeof(std::unique_lock<std::recursive_mutex>)+sizeof(d::NativeFilespaceDevice)+4*alignof(std::max_align_t))+
    selected.selection.allocation.pages.size()*sizeof(db::NativeReadAllocationPage)+
    directory.directory.directory.pages.size()*sizeof(db::NativeReadDirectoryPage)+1024;
 }
 checkpoint_inventory_memory::Grant memory(capacity),source_memory(source_bytes);
 usize exact_backing=0;Uuid largest_member;
 for(unsigned n=0;n<4;++n){auto wrong=memory.binding;
  const std::array<Uuid*,4> ids{&wrong.database_uuid,&wrong.operation_uuid,&wrong.owner_uuid,&wrong.context_uuid};*ids[n]=Id(63900);
  auto refused=db::PrepareNativeAllocationRead(capacity,memory.memory,wrong);
  Check(!refused.ok()&&!refused.workspace&&refused.memory_error==db::NativeStorageMemoryError::invalid_binding,
    "allocation chain grant binds all four binary identities");
 }
 {checkpoint_inventory_memory::Grant short_grant(capacity-1);
  const auto refused=db::PrepareNativeAllocationRead(capacity,short_grant.memory,short_grant.binding);
  Check(!refused.ok()&&!refused.workspace,"allocation backing requires complete actual grant");
  short_grant.memory={};short_grant.Empty();}
 auto prepared=db::PrepareNativeAllocationRead(capacity,memory.memory,memory.binding);
 Check(prepared.ok(),"actual allocation backing before source guards");
 const auto failed=[](const auto& r){Check(!r.ok()&&!r.root&&r.chain.pages.empty()&&!r.retained_image_bytes,
  "allocation failure exposes no successful chain or root prefix");};
 const auto run=[&](bool faults){
  std::array<page::NativeAllocationChainResult,3> expected;
  std::array<Uuid,3> members{Id(2),f.t.other,Id(11)};
  // Independent owning chain readers choose the same actual selected roots.
  {auto source=db::AcquireNativeSelectedCheckpointReadLease(Id(1),files,Id(2),allowance);
   Check(source.ok(),"independent selected allocation source");
   const auto& cp=*source.lease->selection().checkpoint_inventory.checkpoint;
   for(usize n=0;n<members.size();++n){
    const page::NativeFilespaceDirectoryRecord* member=nullptr;
    for(const auto& p:source.lease->directory().directory.pages)for(const auto& r:p.directory->records)
     if(r.bootstrap.filespace_uuid==members[n])member=&r;
    Check(member,"actual directory membership for allocation oracle");
    d::FilespaceRootReference root;bool selected=false;
    if(n==0){const auto r=std::find_if(cp.roots.begin(),cp.roots.end(),[](const auto& r){return r.role==4;});
     Check(r!=cp.roots.end(),"selected primary allocation oracle root");
     root={3,3,r->page.filespace_uuid,r->page.page_number,r->page.page_generation,r->page.page_size_profile_uuid,r->object_uuid};selected=true;
    }else if(member->allocation_root){const auto& r=*member->allocation_root;
     root={3,3,r.page.filespace_uuid,r.page.page_number,r.page.page_generation,r.page.page_size_profile_uuid,r.object_uuid};selected=true;}
    const d::FilespaceBootstrapBinding binding{Id(1),members[n],member->bootstrap.page_size_profile_uuid};
    expected[n]=selected?page::ReadNativeAllocationChainAtRootFromOpenDevice(*f.File(members[n]),binding,root,capacity):
     page::ReadNativeAllocationChainFromOpenDevice(*f.File(members[n]),binding,capacity);
    Check(expected[n].ok(),"independent owning complete allocation chain");
   }
  }
  Bytes resealed;
  if(scenario){
   auto changed=*expected[1].pages.front().map;
   Check(!changed.records.empty(),"head range includes page-zero allocation record");
   changed.records.front().allocation_uuid=Id(63902);
   resealed=MapOracle(changed);
   Check(page::EncodeNativeAllocationMap(changed).bytes==resealed,
     "independent resealed valid map with different binary allocation identity");
  }
  auto source=db::AcquireNativeSelectedCheckpointMemoryLease(Id(1),files,Id(2),
    {allowance,source_bytes},source_memory.memory,source_memory.binding);
  Check(source.ok(),"actual retained allocation source");
  for(usize n=0;n<members.size();++n){
   const auto budget=source.lease->retained_image_bytes()+(n?expected[n].retained_image_bytes:0);
   const auto invoke=[&](u64 limit){return db::NativeAllocationLeaseReader::Read(*source.lease,members[n],limit,prepared.workspace);};
   allocation_budget=0;reads=writes=syncs=0;history_observed_read_bytes=0;io_counting=true;
   hash_counting=true;hash_seen=0;
   const auto actual=invoke(budget);const auto untouched=allocation_budget;
   const auto read_count=reads,hash_count=hash_seen;const auto bytes=history_observed_read_bytes;
   allocation_budget=-1;io_counting=hash_counting=false;
   Check(untouched==0&&actual.ok()&&actual.retained_image_bytes==budget&&
     actual.physical_bytes_read==bytes&&!writes&&!syncs,"complete allocation inspection without heap or writes");
   if(actual.backing_bytes_used>exact_backing){exact_backing=actual.backing_bytes_used;largest_member=members[n];}
   Check(actual.chain.state_counts==expected[n].state_counts&&actual.chain.pages.size()==expected[n].pages.size(),
     "complete independent allocation counts and page coverage");
   for(usize j=0;j<actual.chain.pages.size();++j){const auto& a=actual.chain.pages[j];const auto& b=expected[n].pages[j];
    Check(std::equal(a.image.begin(),a.image.end(),b.bytes.begin(),b.bytes.end())&&
      std::equal(a.map.states.begin(),a.map.states.end(),b.map->states.begin(),b.map->states.end())&&
      std::equal(a.map.records.begin(),a.map.records.end(),b.map->records.begin(),b.map->records.end()),
      "exact images states and binary owner records match independent oracle");}
   Check(actual.root->sha256==Sha(expected[n].pages.front().bytes)&&actual.root->map_generation==expected[n].pages.front().map->map_generation&&
     actual.root->capacity_generation==expected[n].pages.front().map->capacity_generation,"exact resolved allocation generations and root hash");
   failed(invoke(budget-1));
   if(!faults)continue;
   auto moved=std::move(prepared.workspace);failed(invoke(budget));prepared.workspace=std::move(moved);
   failed(db::NativeAllocationLeaseReader::Read(*source.lease,Id(63901),budget,prepared.workspace));
   if(!n){Check(!read_count&&!hash_count,"primary allocation reuses verified retained source without rereading");continue;}
   if(n==1&&!resealed.empty()){
    replacement_bytes=resealed.data();replacement_length=resealed.size();
    replacement_offset=expected[n].pages.front().map->header.page_number*replacement_length;
    replacement_seen=0;replacement_at=1;
    const auto refused=invoke(budget);replacement_bytes=nullptr;failed(refused);
    Check(replacement_seen&&refused.error==db::NativeAllocationLeaseError::integrity_failure,
      "valid resealed allocation map cannot replace the selected directory digest");
   }
   for(unsigned kind=0;kind<2;++kind)for(unsigned site=1;site<=read_count;++site){
    reads=writes=syncs=0;history_observed_read_bytes=0;io_counting=true;
    if(kind)corrupt_read=site;else read_fault=site;
    const auto refused=invoke(budget);io_counting=false;read_fault=corrupt_read=0;failed(refused);
    Check(reads>=site&&!writes&&!syncs&&refused.physical_bytes_read==history_observed_read_bytes,
      "each allocation read failure/corruption preserves exact physical effects");
   }
   for(unsigned mode=1;mode<=5;++mode)for(unsigned site=1;site<=hash_count;++site){
    hash_fault=mode;hash_target=site;hash_seen=0;hash_active=false;
    const auto refused=invoke(budget);const bool consumed=!hash_fault;hash_fault=0;hash_active=false;failed(refused);
    Check(consumed,"each allocation hash fault is actually exercised");
   }
#ifdef NATIVE_HISTORICAL_BUNDLE_SIZE_FAULTS
   for(unsigned site=1;site<=2;++site){
    historical_stat_counting=true;historical_stats=0;historical_stat_fault=site;
    const auto refused=invoke(budget);historical_stat_counting=false;historical_stat_fault=0;failed(refused);
    Check(historical_stats>=site&&refused.chain_error==page::NativeAllocationError::io_failure,
      "allocation capacity observation failures preserve underlying I/O classification");
   }
#endif
   Check(invoke(budget).ok(),"allocation same-lease retry after faults");
  }
 };
 run(true);
 Check(exact_backing>1&&exact_backing<capacity,"complete chain exposes exact bounded backing consumption");
 prepared.workspace={};prepared=db::PrepareNativeAllocationRead(exact_backing-1,memory.memory,memory.binding);
 Check(prepared.ok(),"short chain backing is itself an actual admitted arena");
 {
  auto source=db::AcquireNativeSelectedCheckpointMemoryLease(Id(1),files,Id(2),
    {allowance,source_bytes},source_memory.memory,source_memory.binding);
  Check(source.ok(),"source for one-byte-short chain test");
  allocation_budget=0;
  const auto refused=db::NativeAllocationLeaseReader::Read(*source.lease,largest_member,allowance,prepared.workspace);
  const auto untouched=allocation_budget;allocation_budget=-1;failed(refused);
  Check(untouched==0&&(refused.error==db::NativeAllocationLeaseError::resource_exhausted||
    refused.chain_error==page::NativeAllocationError::resource_exhausted),"one-byte-short chain has no heap escape or partial success");
 }
 prepared.workspace={};prepared=db::PrepareNativeAllocationRead(exact_backing,memory.memory,memory.binding);
 Check(prepared.ok(),"exact-sized actual backing");
 f.Reopen();run(false);
 prepared.workspace={};memory.memory={};memory.Empty();source_memory.memory={};source_memory.Empty();
}
