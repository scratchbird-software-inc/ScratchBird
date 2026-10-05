// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
void ReservationSource(unsigned primary,unsigned secondary){
 using RE=db::NativePageReservationReadError;
 DirectoryHistoryFixture f(primary,secondary,false,2,false,false,true,false);
 auto& files=f.t.fixture.devices;
 const u64 allowance=16*1024*std::max<u64>(f.t.fixture.size,d::kCanonicalFilespacePageProfiles[secondary].page_size_bytes);
 auto selected=db::ReadNativeBoundCheckpointSelectionFromOpenDevices(Id(1),files,Id(2),allowance);
 Check(selected.ok(),"actual reservation source genesis");
 auto inventory=selected.checkpoint_inventory.inventory;
 for(unsigned n=0;n<4;++n){auto begun=mga::BeginLocalTransaction(inventory,{UuidKind::transaction,Id(65000+n)},4000+n);
  Check(begun.ok(),"actual new local transaction identity");inventory=std::move(begun.inventory);
  inventory.entries.back().state=mga::TransactionState::created;}
 // Both starting and activation are actually published before inspection.
 for(unsigned phase=0;phase<2;++phase){
  if(phase){inventory.entries[1].state=mga::TransactionState::active;
   inventory.entries[2].state=mga::TransactionState::read_only_active;
   inventory.entries[3].state=mga::TransactionState::active;inventory.entries[3].rollback_only=true;}
  auto o=records::Example(phase+1);o.uuid=Id(64900);o.bootstrap_uuid=selected.selection->bootstrap_uuid;
  o.revision=phase+1;o.updated_at=Id(64901+phase);o.idempotency_key="reservation-source-inventory";
  if(!phase){o.security_snapshot_uuid={};o.generation_guards={};}
  db::NativePublicationIntent intent{o.initiator_uuid,o.request_context_uuid,o.policy_snapshot_uuid,o.normalized_request_sha256,o.initiator_kind};
  intent.recovery_profile=2;
  auto snapshot=db::InspectNativePublicationGenerationOnOpenDevices(Id(1),files,Id(2),allowance);
  Check(snapshot.ok(),"reservation source actual publication base");
  auto held=db::ReserveNativePublicationGenerationOnOpenDevices(Id(1),files,Id(2),*snapshot.snapshot,Id(64910+phase),allowance,&intent);
  Check(held.ok(),"reservation source inventory publication fence");
  auto published=db::PublishNativeInventoryOnLease(*held.lease,o,inventory,allowance,*f.t.fixture.issuer);
  if(!published.ok())std::cerr<<"reservation inventory phase="<<phase<<" error="<<int(published.error)<<'\n';
  Check(published.ok(),"reservation source inventory durable publication");
 }
 constexpr usize map_bytes=4*1024*1024;
 usize source_bytes=0;
 {owned_source_memory::Meter meter;
  std::vector<std::unique_ptr<d::FileDevice::ReadLatencyBatch>> owners;
  std::vector<d::FileDevice::ReadLatencyBatch*> batches;
  for(const auto& file:files){owners.push_back(std::make_unique<d::FileDevice::ReadLatencyBatch>(*file.device));batches.push_back(owners.back().get());}
  const auto selected=db::detail::ReadNativeBoundCheckpointSelectionBacked(Id(1),files,Id(2),allowance,batches,meter);
  Check(selected.ok(),"measure published source backing before memory admission");
  const auto& s=*selected.selection.selection;
  const d::FilespaceRootReference root{9,0x300,s.checkpoint.filespace_uuid,s.checkpoint.page_number,
    s.checkpoint.page_generation,s.checkpoint.page_size_profile_uuid,s.checkpoint_object_uuid};
  const auto directory=db::detail::VerifyCurrentNativeCheckpointDirectoryBacked(Id(1),files,root,
    allowance-selected.selection.retained_image_bytes,batches,meter);
  Check(directory.ok(),"measure published directory backing before memory admission");
  source_bytes=meter.used+sizeof(db::NativeSelectedCheckpointMemoryLease)+
    files.size()*(sizeof(d::FileDevice::ReadLatencyBatch)+sizeof(d::FileDevice::ReadLatencyBatch*)+
      sizeof(std::unique_lock<std::recursive_mutex>)+sizeof(d::NativeFilespaceDevice)+4*alignof(std::max_align_t))+
    selected.selection.allocation.pages.size()*sizeof(db::NativeReadAllocationPage)+
    directory.directory.directory.pages.size()*sizeof(db::NativeReadDirectoryPage)+1024;
 }
 const auto probe_bytes=db::NativePageReservationReadBytes(2,0);
 checkpoint_inventory_memory::Grant memory(map_bytes+probe_bytes),source_memory(source_bytes+map_bytes);
 for(unsigned n=0;n<4;++n){auto wrong=memory.binding;
  const std::array<Uuid*,4> fields{&wrong.database_uuid,&wrong.operation_uuid,&wrong.owner_uuid,&wrong.context_uuid};*fields[n]=Id(64000);
  const auto r=db::PrepareNativePageReservationRead(2,0,map_bytes,memory.memory,wrong);
  Check(!r.ok()&&!r.workspace&&r.memory_error==db::NativeStorageMemoryError::invalid_binding,"reservation grant exact four binary bindings");}
 {checkpoint_inventory_memory::Grant short_memory(map_bytes+probe_bytes-1);
  const auto r=db::PrepareNativePageReservationRead(2,0,map_bytes,short_memory.memory,short_memory.binding);
  Check(!r.ok()&&!r.workspace,"reservation actual grant one byte short");short_memory.memory={};short_memory.Empty();}
 Check(!db::NativePageReservationReadBytes(0,0)&&!db::NativePageReservationReadBytes(std::numeric_limits<usize>::max(),0)&&
   !db::NativePageReservationReadBytes(1,std::numeric_limits<usize>::max()),"reservation memory checked bounds");
 auto prepared=db::PrepareNativePageReservationRead(2,0,map_bytes,memory.memory,memory.binding);
 Check(prepared.ok(),"reservation actual work backing before source guard");
 auto oracle_backing=db::PrepareNativeAllocationRead(map_bytes,source_memory.memory,source_memory.binding);
 Check(oracle_backing.ok(),"independent request source backing");
 std::array<std::pair<Uuid,u64>,2> dirty_slots{};
 const auto exercise=[&](bool faults,bool dirty=false){
  auto source=db::AcquireNativeSelectedCheckpointMemoryLease(Id(1),files,Id(2),
    {allowance,source_bytes},source_memory.memory,source_memory.binding);
  if(!source.ok())std::cerr<<"reservation source error="<<int(source.error)<<" memory="<<int(source.memory_error)
    <<" selection="<<int(source.selection_error)<<" checkpoint="<<int(source.checkpoint_error)
    <<" directory="<<int(source.directory_error)<<" inventory="<<int(source.inventory_error)<<'\n';
  Check(source.ok(),"retained actual reservation source");
  const auto& s=*source.lease->selection().selection;
  for(const auto target:{Id(2),f.t.other}){
   const page::NativeFilespaceDirectoryRecord* member=nullptr;
   for(const auto& p:source.lease->directory().directory.pages)for(const auto& r:p.directory.records)
    if(r.bootstrap.filespace_uuid==target)member=&r;
   Check(member,"actual reservation target directory member");
   const auto map=db::NativeAllocationLeaseReader::Read(*source.lease,target,allowance,oracle_backing.workspace);
   Check(map.ok(),"actual reservation target map oracle");
   std::array<db::NativePageReservationAssignment,2> pages{};usize n=0;
   for(const auto& p:map.chain.pages)for(usize j=0;j<p.map.states.size()&&n<2;++j)
    if(p.map.first_page+j>300&&p.map.states[j]==State::free){pages[n]={p.map.first_page+j,Id(64010+2*n),Id(64011+2*n),1,6};++n;}
   Check(n==2,"two actual original free pages");
   dirty_slots[target==Id(2)?0:1]={target,pages[0].page_number*member->bootstrap.page_size_bytes};
   db::NativePageReservationIntent i;
   i.request_uuid=Id(64001);i.operation_uuid=memory.binding.operation_uuid;i.database_uuid=Id(1);i.filespace_uuid=target;
   i.page_size_profile_uuid=member->bootstrap.page_size_profile_uuid;i.locator_uuid=member->locator_uuid;i.page_zero_uuid=member->page_zero_uuid;
   i.allocation_object_uuid=map.root->object_uuid;i.transaction_uuid=inventory.entries[1].identity.transaction_uuid.value;
   i.local_transaction_id=inventory.entries[1].identity.local_id.value;i.owner_uuid=Id(64002);
   i.initiator_uuid=memory.binding.owner_uuid;i.request_context_uuid=memory.binding.context_uuid;
   i.policy_snapshot_uuid=Id(10);i.security_snapshot_uuid=Id(64003);
   i.checkpoint=s.checkpoint;i.checkpoint_object_uuid=s.checkpoint_object_uuid;i.checkpoint_sha256=s.checkpoint_sha256;
   i.allocation_sha256=map.root->sha256;i.allocation_page_number=map.root->page.page_number;i.allocation_page_generation=map.root->page.page_generation;
   i.selection_generation=s.selection_generation;i.checkpoint_generation=s.checkpoint_generation;i.checkpoint_root_set_generation=s.root_set_generation;
   i.directory_generation=source.lease->directory().directory.pages.front().directory.directory_generation;
   i.page_zero_generation=member->page_zero_generation;i.filespace_root_set_generation=member->root_set_generation;
   i.map_generation=map.root->map_generation;i.capacity_generation=map.root->capacity_generation;i.total_pages=member->total_pages;
   i.maximum_work_bytes=2*u64{member->bootstrap.page_size_bytes};i.pages=pages;
   std::array<byte,672> bytes{};std::array<Uuid,4> scratch{};
   db::NativeManagementOperationView operation;operation.database_uuid=Id(1);operation.bootstrap_uuid=s.bootstrap_uuid;
   operation.descriptor_uuid=Id(64020);operation.family_uuid=Id(64021);operation.target_type_uuid=Id(64022);
   operation.created_at=Id(64023);operation.updated_at=Id(64024);operation.idempotency_key="reservation-inspection";operation.initiator_kind=4;
   const auto seal=[&](const auto& request){
    const auto encoded=db::EncodeNativePageReservationIntentInto(request,bytes,scratch,bytes.size());Check(encoded.ok(),"canonical reservation test proposal");
    operation.uuid=request.operation_uuid;operation.target_uuid=request.filespace_uuid;operation.initiator_uuid=request.initiator_uuid;
    operation.request_context_uuid=request.request_context_uuid;operation.policy_snapshot_uuid=request.policy_snapshot_uuid;
    operation.security_snapshot_uuid=request.security_snapshot_uuid;operation.generation_guards={request.catalog_generation,request.configuration_generation,request.security_generation,{}};
    operation.normalized_request_bytes=bytes;operation.normalized_request_sha256=scratchbird::core::hash::ComputeSha256DigestNative(bytes.data(),bytes.size()).digest;
   };
   const auto call=[&](){return db::NativePageReservationLeaseReader::Read(*source.lease,operation,allowance,prepared.workspace);};
   const auto failed=[&](const auto& r,RE e){Check(!r.ok()&&!r.pages_observed&&r.error==e,"exact reservation observation refusal without successful prefix");};
   seal(i);allocation_budget=0;reads=writes=syncs=0;history_observed_read_bytes=0;io_counting=true;hash_counting=true;hash_seen=0;
   auto result=call();const auto heap=allocation_budget;const auto sites=reads,hashes=hash_seen;const auto read_bytes=history_observed_read_bytes;
   allocation_budget=-1;io_counting=hash_counting=false;
   if(dirty){failed(result,RE::page_not_zero);Check(heap==0&&!writes&&!syncs&&result.physical_bytes_read==read_bytes,
     "actual persisted dirty free page has no reservation observation and exact read accounting");continue;}
   if(!result.ok())std::cerr<<"reservation read error="<<int(result.error)<<" intent="<<int(result.intent_error)<<" alloc="<<int(result.allocation_error)<<'\n';
   Check(heap==0&&result.ok()&&result.pages_observed==2&&result.physical_bytes_read==read_bytes&&!writes&&!syncs,
    "real free-page observation without heap writes or unaccounted reads");
   if(!faults)continue;
   for(auto field:{&db::NativePageReservationIntent::selection_generation,&db::NativePageReservationIntent::checkpoint_generation,
       &db::NativePageReservationIntent::checkpoint_root_set_generation,&db::NativePageReservationIntent::directory_generation,
       &db::NativePageReservationIntent::page_zero_generation,&db::NativePageReservationIntent::filespace_root_set_generation,
       &db::NativePageReservationIntent::map_generation,&db::NativePageReservationIntent::capacity_generation}){
    auto changed=i;++(changed.*field);seal(changed);failed(call(),RE::source_mismatch);}
   for(unsigned tx:{0u,2u,3u,4u}){auto changed=i;changed.transaction_uuid=inventory.entries[tx].identity.transaction_uuid.value;
    changed.local_transaction_id=inventory.entries[tx].identity.local_id.value;seal(changed);failed(call(),RE::transaction_not_writable);}
   auto changed=i;changed.transaction_uuid=Id(64030);seal(changed);failed(call(),RE::transaction_not_writable);
   changed=i;++changed.local_transaction_id;seal(changed);failed(call(),RE::transaction_not_writable);
   for(auto field:{&db::NativePageReservationIntent::operation_uuid,&db::NativePageReservationIntent::initiator_uuid,
       &db::NativePageReservationIntent::request_context_uuid}){changed=i;changed.*field=Id(64031);seal(changed);failed(call(),RE::binding_mismatch);}
   seal(i);auto moved=std::move(prepared.workspace);failed(call(),RE::invalid_request);prepared.workspace=std::move(moved);
   // Collision in the third, non-target member must also be rejected.
   const auto saved=pages[0];pages[0].allocation_uuid=Id(29012);seal(i);failed(call(),RE::identity_collision);pages[0]=saved;seal(i);
   for(const auto& p:map.chain.pages){bool checked=false;
    for(const auto& r:p.map.records)if(r.page_number&&r.page_number<pages[1].page_number&&
       r.page_number!=i.allocation_page_number&&(target!=i.checkpoint.filespace_uuid||r.page_number!=i.checkpoint.page_number)){
      pages[0].page_number=r.page_number;seal(i);failed(call(),RE::page_not_free);pages[0]=saved;seal(i);checked=true;break;}
    if(checked)break;}
   for(auto field:{&db::NativePageReservationIntent::locator_uuid,&db::NativePageReservationIntent::page_zero_uuid,
       &db::NativePageReservationIntent::checkpoint_object_uuid,&db::NativePageReservationIntent::allocation_object_uuid}){
    changed=i;changed.*field=Id(64032);seal(changed);failed(call(),RE::source_mismatch);}
   changed=i;changed.checkpoint_sha256[0]^=1;seal(changed);failed(call(),RE::source_mismatch);
   changed=i;changed.allocation_sha256[0]^=1;seal(changed);failed(call(),RE::source_mismatch);seal(i);
   for(unsigned at=1;at<=sites;++at){reads=0;read_fault=at;io_counting=true;const auto r=call();io_counting=false;read_fault=0;
    Check(!r.ok()&&!r.pages_observed&&reads>=at&&r.physical_bytes_read<=read_bytes,"every physical read failure has no observation receipt");}
   for(unsigned mode=1;mode<=5;++mode)for(unsigned at=1;at<=hashes;++at){hash_target=at;hash_seen=0;hash_active=false;hash_fault=mode;
    const auto r=call();const bool consumed=!hash_fault;hash_fault=0;hash_active=false;
    Check(consumed&&!r.ok()&&!r.pages_observed,"every actual hash provider failure closes observation");}
   Check(call().ok()&&!writes&&!syncs,"same retained source succeeds after injected failures");
   for(unsigned at=sites-1;at<=sites;++at){reads=0;corrupt_read=at;io_counting=true;auto r=call();io_counting=false;corrupt_read=0;
    failed(r,RE::page_not_zero);Check(corrupt_was_zero,"nonzero page probe is rejected even when allocation map says free");}
#ifdef NATIVE_HISTORICAL_BUNDLE_SIZE_FAULTS
   historical_stat_counting=true;historical_stats=0;Check(call().ok(),"size observation baseline");
   historical_stat_counting=false;const auto sizes=historical_stats;
   for(unsigned at=1;at<=sizes;++at){historical_stat_counting=true;historical_stats=0;historical_stat_fault=at;
    const auto r=call();historical_stat_counting=false;historical_stat_fault=0;
    Check(!r.ok()&&!r.pages_observed&&historical_stats>=at,"every actual extent size error closes observation");}
#endif
   Check(call().ok(),"retry after corrupt-page and size failures");
  }
 };
 exercise(true);
 for(const auto& [id,at]:dirty_slots){const byte nonzero=1;
  Check(f.File(id)->WriteAt(at,&nonzero,1).ok()&&f.File(id)->Sync().ok(),"persist actual dirt in original free pages");}
 f.Reopen();exercise(false,true);
 f.Reopen(true);for(const auto& [id,at]:dirty_slots){const byte zero=0;
  Check(f.File(id)->WriteAt(at,&zero,1).ok()&&f.File(id)->Sync().ok(),"restore exact free physical bytes");}
 f.Reopen();exercise(false);
 prepared.workspace={};oracle_backing.workspace={};memory.memory={};source_memory.memory={};memory.Empty();source_memory.Empty();
}
