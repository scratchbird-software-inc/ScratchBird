// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
// Included after each fixture's independent oracles and allocation probes.
namespace checkpoint_inventory_memory {
namespace d=scratchbird::storage::disk;
namespace m=scratchbird::core::memory;
using E=db::NativeCheckpointError;
using M=db::NativeCheckpointInventoryMemoryError;
void Empty(const db::NativeCheckpointInventoryView& v) {
 Check(!v.ok()&&!v.checkpoint&&v.inventory.entries.empty()&&v.inventory_pages.empty()&&
  !v.inventory_generation&&!v.retained_image_bytes&&!v.backing_bytes_used&&
  std::all_of(v.checkpoint_sha256.begin(),v.checkpoint_sha256.end(),[](byte b){return !b;}),
  "failed complete checkpoint inventory exposes no authority prefix");
}
void Same(const db::NativeCheckpointInventoryResult& a,const db::NativeCheckpointInventoryView& b) {
 Check(a.error==b.error&&a.inventory_error==b.inventory_error,"owning/bounded complete checkpoint inventory diagnostic parity");
 if(!a.ok()){Empty(b);return;}
 Check(b.ok()&&a.checkpoint_sha256==b.checkpoint_sha256&&a.inventory_generation==b.inventory_generation&&
  a.retained_image_bytes==b.retained_image_bytes&&a.inventory_pages.size()==b.inventory_pages.size()&&
  a.inventory.next_local_transaction_id==b.inventory.next_local_transaction_id&&
  a.inventory.next_commit_sequence==b.inventory.next_commit_sequence&&
  a.inventory.entries.size()==b.inventory.entries.size()&&!a.inventory.publication_base,
  "complete checkpoint inventory dimensions counters digest generation and non-CAS boundary");
 const auto ac=db::EncodeNativeCheckpointRoot(*a.checkpoint);Bytes encoded(b.checkpoint->header.page_size_bytes);
 const auto bc=db::EncodeNativeCheckpointRootInto(*b.checkpoint,encoded);
 Check(ac.ok()&&bc.ok()&&std::equal(ac.bytes.begin(),ac.bytes.end(),bc.bytes.begin(),bc.bytes.end()),"all canonical checkpoint fields preserved");
 for(std::size_t i=0;i<a.inventory_pages.size();++i){
  const auto& x=a.inventory_pages[i];const auto& y=b.inventory_pages[i];
  const auto xh=d::EncodeNativeCommonPageHeader(x.header),yh=d::EncodeNativeCommonPageHeader(y.header);
  Check(xh.ok()&&yh.ok()&&xh.bytes==yh.bytes&&x.object_uuid==y.object_uuid,"every actual inventory page identity binding");
 }
 for(std::size_t i=0;i<a.inventory.entries.size();++i){
  const auto& x=a.inventory.entries[i];const auto& y=b.inventory.entries[i];
  Check(x.identity.local_id.value==y.identity.local_id.value&&x.identity.transaction_uuid.kind==y.identity.transaction_uuid.kind&&
   x.identity.transaction_uuid.value==y.identity.transaction_uuid.value&&
   x.identity.scope==y.identity.scope&&x.state==y.state&&x.archived_from_state==y.archived_from_state&&
   x.begin_unix_epoch_millis==y.begin_unix_epoch_millis&&x.final_unix_epoch_millis==y.final_unix_epoch_millis&&
   x.begin_visible_through_local_transaction_id==y.begin_visible_through_local_transaction_id&&
   x.begin_visible_through_commit_sequence==y.begin_visible_through_commit_sequence&&
   x.commit_sequence==y.commit_sequence&&x.evidence_record_required==y.evidence_record_required&&
   x.evidence_record_written==y.evidence_record_written&&x.rollback_only==y.rollback_only&&
   x.stable_snapshot==y.stable_snapshot,"every persisted inventory entry field preserved");
 }
}
struct Grant {
 m::MemoryManager manager;
 m::HierarchicalMemoryBudgetLedger ledger{3,5};
 db::NativeStorageMemoryBinding binding{Id(1),Id(211),Id(212),Id(213)};
 db::NativeStorageMemory memory;
 static auto Policy(){auto p=m::DefaultLocalEngineMemoryPolicy();
  p.hard_limit_bytes=p.per_context_limit_bytes=512ULL*1024*1024;return p;}
 explicit Grant(u64 bytes):manager(Policy()){
  m::ReservationBackedMemoryResourceRequest r;r.memory_manager=&manager;r.reservation_ledger=&ledger;
  r.consumer_kind=m::ReservationBackedMemoryConsumerKind::background_maintenance;
  r.category=m::MemoryCategory::page_buffer;r.requested_bytes=bytes;r.memory_class="page_buffer";
  r.route_label="storage.checkpoint.inventory.conformance";r.purpose="complete native checkpoint inventory source";
  r.binary_operation_uuid=binding.operation_uuid.bytes;
  r.binary_ownership[m::MemoryBinaryScopeKind::database]=binding.database_uuid.bytes;
  r.binary_ownership[m::MemoryBinaryScopeKind::owner]=binding.owner_uuid.bytes;
  r.binary_ownership[m::MemoryBinaryScopeKind::context]=binding.context_uuid.bytes;
  r.scope_chain={{m::HierarchicalMemoryScopeKind::process,{},Id(214).bytes},
   {m::HierarchicalMemoryScopeKind::database,{},binding.database_uuid.bytes}};
  r.provenance.source=m::HierarchicalMemoryBudgetProvenanceSource::server_runtime_api;
  r.provenance.source_label="complete native checkpoint inventory backing conformance";
  for(const auto& scope:r.scope_chain){m::HierarchicalMemoryBudget budget;budget.scope=scope;
   budget.hard_limit_bytes=bytes;budget.provenance=r.provenance;Check(ledger.SetBudget(budget).ok(),"configure actual parent memory limits");}
  auto grant=m::AcquireReservationBackedMemoryResource(r);Check(grant.ok(),"issue actual checkpoint inventory grant");
  auto adopted=db::AdoptNativeStorageMemory(binding,grant.resource);Check(adopted.ok()&&!grant.resource,"exclusive binary-bound grant adoption");
  memory=std::move(adopted.memory);
 }
 void Empty(){const auto s=manager.Snapshot();Check(!s.current_bytes&&!s.reserved_capacity_bytes&&
  !s.active_capacity_reservation_count&&!ledger.Snapshot().current_bytes,"all complete source charges released");}
};
void Checks(std::span<const d::NativeFilespaceDevice> files,const d::FilespaceRootReference& root,
 const db::NativeCheckpointInventoryResult& expected,u64 allowance,bool lifetime=false){
 const auto database=Id(1);
 const auto capacity=static_cast<std::size_t>((expected.ok()?expected.retained_image_bytes:allowance)*4+1024*1024);
 Bytes backing(capacity);
 std::vector<std::unique_ptr<d::FileDevice::ReadLatencyBatch>> owners;
 std::vector<d::FileDevice::ReadLatencyBatch*> batches;
 for(const auto& file:files){owners.push_back(std::make_unique<d::FileDevice::ReadLatencyBatch>(*file.device));batches.push_back(owners.back().get());}
 const auto direct=[&](std::span<byte> bytes,bool deny=true){
  if(deny)allocation_budget=0;
  auto r=db::VerifyNativeCheckpointInventoryInto(database,files,root,allowance,batches,bytes);
  const auto untouched=allocation_budget==0;if(deny)allocation_budget=-1;
  if(deny)Check(untouched,"no ordinary or aligned heap fallback in full actual checkpoint inventory");
  return r;
 };
 const auto complete=direct(backing);Same(expected,complete.inventory);
 if(!expected.ok())return;
 const auto used=complete.inventory.backing_bytes_used;
 Check(used&&used<=capacity&&complete.io_status.ok()&&complete.physical_bytes_read,"bounded complete-source observations and actual backing");
 Same(expected,direct(std::span(backing).first(used)).inventory);
 const auto short_read=direct(std::span(backing).first(used-1));
 Check(short_read.inventory.error==E::resource_exhausted,"one byte short full-source backing refuses");Empty(short_read.inventory);
 const auto overlap=[&](std::span<byte> region){const auto r=direct(region);
  Check(r.inventory.error==E::invalid_backing&&!r.physical_bytes_read,"input or device overlap refuses before effects");Empty(r.inventory);};
 overlap({reinterpret_cast<byte*>(const_cast<Uuid*>(&database)),sizeof(database)});
 overlap({reinterpret_cast<byte*>(const_cast<d::FilespaceRootReference*>(&root)),sizeof(root)});
 overlap({reinterpret_cast<byte*>(const_cast<d::NativeFilespaceDevice*>(files.data())),files.size()*sizeof(files.front())});
 overlap({reinterpret_cast<byte*>(files.front().device),sizeof(d::FileDevice)});
 overlap({reinterpret_cast<byte*>(batches.data()),batches.size()*sizeof(batches.front())});
 overlap({reinterpret_cast<byte*>(batches.front()),sizeof(*batches.front())});
 d::FileDevice foreign;d::FileDevice::ReadLatencyBatch wrong(foreign);auto* saved=batches.front();batches.front()=&wrong;
 const auto denied=direct(backing);batches.front()=saved;
 Check(denied.inventory.error==E::invalid_filespace&&!denied.physical_bytes_read,"exact-device observation admission");Empty(denied.inventory);
 Grant grant(capacity);const db::NativeCheckpointInventoryMemoryLimits limits{allowance,capacity};
 const auto call=[&](const db::NativeStorageMemoryBinding& binding){
  return db::VerifyNativeCheckpointInventoryWithMemoryFromOpenDevices(database,files,root,limits,grant.memory,binding);};
 auto retained=call(grant.binding);Check(retained.ok(),"actual binary grant backs full checkpoint inventory");Same(expected,retained.inventory);
 Check(retained.physical_bytes_read==complete.physical_bytes_read&&retained.io_status.ok(),"actual grant retains complete physical read receipt");
 Check(grant.manager.Snapshot().current_bytes==capacity&&grant.ledger.Snapshot().current_bytes==capacity&&
  grant.memory.Snapshot().allocated_bytes==capacity&&retained.arena.Snapshot().retained_bytes==capacity,
  "all source memory charged in manager ledger owner and retained arena");
 const auto failed=[&](const auto& r){Check(!r.ok()&&!r.arena&&!r.physical_bytes_read,"pre-read refusal has no retained backing or physical effects");Empty(r.inventory);};
 const auto exhausted=call(grant.binding);failed(exhausted);
 Check(exhausted.error==M::memory_allocation_failure,"held payload exhausts real grant before I/O");
 for(unsigned i=0;i<4;++i){auto bad=grant.binding;const std::array<Uuid*,4> ids{&bad.database_uuid,&bad.operation_uuid,&bad.owner_uuid,&bad.context_uuid};
  *ids[i]=Id(219);const auto r=call(bad);failed(r);Check(r.error==M::memory_binding_failure,"all four binary ownership fields enforced");}
 Check(grant.ledger.CleanupOwner(grant.binding.owner_uuid.bytes).retained_bytes==capacity,"revocation retains live payload charges");
 if(lifetime){
  std::vector<std::pair<std::string,bool>> paths;
  for(const auto& file:files){const auto fence=file.device->AcquireOperationGuard();paths.emplace_back(file.device->path(),file.device->read_only());}
  for(const auto& file:files)Check(file.device->Close().ok(),"close sources while full inventory remains owned");
  Same(expected,retained.inventory);
  for(std::size_t i=0;i<files.size();++i)Check(files[i].device->Open(paths[i].first,
   paths[i].second?d::FileOpenMode::open_existing_read_only:d::FileOpenMode::open_existing).ok(),"reopen unchanged actual source routes");
 }
 const auto revoked=call(grant.binding);failed(revoked);Check(revoked.error==M::memory_binding_failure,"revocation prevents new source reads");
 grant.memory={};Same(expected,retained.inventory);Check(grant.manager.Snapshot().current_bytes==capacity,"owned view survives grant-wrapper release");
 retained={};grant.Empty();
}
} // namespace checkpoint_inventory_memory
