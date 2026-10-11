// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
void CheckCatalogVisibilitySelection(DirectoryHistoryFixture& f,const d::FilespaceRootReference& checkpoint,
 const db::NativeCatalogRelationBinding& binding,const db::NativeCatalogRelationMemoryLimits& limits,u64 allowance,unsigned scenario,bool context_namespaces=false){
 using E=db::NativePinnedCatalogReadError;
 auto& storage=f.t.fixture;
 mga::TransactionIdentity reader;
 mga::PublishedSnapshotPin pin;
 TypedUuid snapshot_id;
 if(scenario==7||scenario==23){
  auto actual=db::AcquireNativeSelectedCheckpointReadLease(Id(1),storage.devices,Id(2),allowance);
  Check(actual.ok(),"actual retained inventory for pin publication");
  const auto& inventory=actual.lease->selection().checkpoint_inventory.inventory;reader=inventory.entries.back().identity;
  auto published=mga::PublishStatementStableSnapshotVector(inventory,reader.local_id,2000000000002ULL);
  Check(published.ok(),"real pin from actual selected active reader inventory");snapshot_id=published.descriptor.snapshot_uuid;
  pin=mga::RetainPublishedSnapshotVector(snapshot_id);Check(pin.valid(),"retain actual selected reader snapshot");
 }
 struct Release {TypedUuid id;~Release(){if(id.valid())mga::ReleasePublishedSnapshotVector(id);}} release{snapshot_id};
 const auto oracle=pin.valid()?db::ReadNativePinnedCatalogVersionsFromOpenDevices(Id(1),storage.devices,checkpoint,2,1,binding,reader,pin,allowance):
   [&]{auto r=db::ReadNativeCommittedCatalogVersionsFromOpenDevices(Id(1),storage.devices,checkpoint,2,1,binding,allowance);
     db::NativePinnedCatalogReadResult p;p.error=r.error;p.source=std::move(r.source);p.rows=std::move(r.rows);p.observations=std::move(r.observations);return p;}();
 if(scenario>=10&&scenario<=22){const auto expected=scenario==13?E::duplicate_identity:scenario==17?E::missing_version:
    scenario==10||scenario==16||scenario==18||scenario==19||scenario==21?E::none:E::invalid_chain;
  Check(oracle.error==expected,"independent explicit retained chain and reservation outcome");
  if(oracle.ok())Check(oracle.rows.size()==1&&oracle.rows[0].metadata.definition_version==(scenario==18?1u:2u)&&
    oracle.rows[0].metadata.record.header.deleted==(scenario==16),"visible newest fallback and retained retirement are explicit expected rows");}
 if(scenario>=23)Check(oracle.ok()&&oracle.rows.size()==(scenario==23?2u:1u)&&
    std::count_if(oracle.rows.begin(),oracle.rows.end(),[](const auto& r){return r.provisional;})==(scenario==23?1:0),
    "real owning snapshot admits own candidate while committed mode waits and omits it");
 const auto bytes=db::NativeCatalogVisibilityWorkspaceBytes(limits);
 checkpoint_inventory_memory::Grant memory(bytes),source_memory(32*1024*1024);
 for(unsigned field=0;field<4;++field){auto wrong=memory.binding;
  const std::array<Uuid*,4> ids{&wrong.database_uuid,&wrong.operation_uuid,&wrong.owner_uuid,&wrong.context_uuid};*ids[field]=Id(63990);
  const auto rejected=db::PrepareNativeCatalogVisibilityRead(limits,memory.memory,wrong);
  Check(!rejected.ok()&&!rejected.workspace&&!memory.manager.Snapshot().current_bytes,"visibility preparation binds every actual binary grant identity");}
 checkpoint_inventory_memory::Grant short_memory(bytes-1);
 const auto short_grant=db::PrepareNativeCatalogVisibilityRead(limits,short_memory.memory,short_memory.binding);
 Check(!short_grant.ok()&&!short_grant.workspace&&!short_memory.manager.Snapshot().current_bytes,"late insufficient visibility grant releases complete backing");
 short_memory.memory={};short_memory.Empty();
 auto prepared=db::PrepareNativeCatalogVisibilityRead(limits,memory.memory,memory.binding);Check(prepared.ok(),"complete visibility scratch and relation backing admitted before source");
 auto moved=std::move(prepared.workspace);
 {
  auto source=db::AcquireNativeSelectedCheckpointMemoryLease(Id(1),storage.devices,Id(2),{allowance,32*1024*1024},source_memory.memory,source_memory.binding);
  Check(source.ok(),"actual selected source for bounded visibility");
  const auto read=[&](u64 budget){return pin.valid()?db::NativeCatalogVisibilityLeaseReader::ReadPinned(*source.lease,2,1,binding,reader,pin,budget,prepared.workspace):
    db::NativeCatalogVisibilityLeaseReader::ReadCommitted(*source.lease,2,1,binding,budget,prepared.workspace);};
  const auto empty=[](const auto& r){Check(!r.ok()&&r.rows.empty()&&r.observations.empty()&&r.snapshot_uuid.is_nil()&&r.source.bases.empty()&&
    r.source.bindings.empty()&&r.source.row_creators.empty()&&r.source.navigation_creators.empty()&&r.source.roots.catalogs.empty()&&r.source.tree.pages.empty()&&
    !r.source.retained_image_bytes,"failed selection exposes no graph or row prefix");};
  const auto absent=read(allowance);empty(absent);Check(!absent.source.physical_bytes_read,"moved visibility backing refuses before effects");
  if(context_namespaces){
   const auto missing_workspace=db::NativeContextNamespaceLeaseReader::ReadCommitted(*source.lease,2,1,binding,
     {Id(1),Id(60020),1},allowance,prepared.workspace);
   Check(!missing_workspace.ok()&&!missing_workspace.definition&&!missing_workspace.selected&&
     missing_workspace.error==db::NativeContextNamespaceReadError::source_failure&&
     !missing_workspace.source.source.physical_bytes_read,"typed namespace requires actual pre-admitted workspace before catalog I/O");
  }
  prepared.workspace=std::move(moved);
  allocation_budget=0;reads=0;hash_seen=0;io_counting=hash_counting=true;
  const auto observed=read(allowance);const auto left=allocation_budget;allocation_budget=-1;io_counting=hash_counting=false;
  const auto sites=reads,hash_sites=hash_seen;
  Check(left==0,"complete selected catalog visibility performs no heap allocation under source guards");
  Check(observed.ok()==oracle.ok()&&observed.error==oracle.error,"owning and guarded full visibility agree on complete result or exact refusal");
  if(!oracle.ok())empty(observed);
  else{
   Check(observed.rows.size()==oracle.rows.size()&&observed.observations.size()==oracle.observations.size(),"complete selected row/observation cardinality");
   for(usize i=0;i<observed.rows.size();++i){const auto& a=observed.rows[i];const auto& b=oracle.rows[i];
    Check(a.version_uuid==b.version_uuid&&a.previous_version_uuid==b.previous_version_uuid&&a.metadata.record.header.row_uuid.value==b.metadata.record.header.row_uuid.value&&
      a.metadata.record.header.object_uuid.value==b.metadata.record.header.object_uuid.value&&a.provisional==b.provisional&&a.effective_lifecycle==b.effective_lifecycle&&a.effective_status==b.effective_status&&
      a.metadata.record.payload==b.metadata.record.payload&&a.metadata.definition_version==b.metadata.definition_version,"selected exact identities payload and lifecycle agree");}
   if(scenario==19||scenario==21){namespace catalog=scratchbird::core::catalog;Check(observed.rows[0].name_payload.has_value(),"selected resident name payload retained");
    if(scenario==19)Check(std::get<catalog::CatalogNameVectorView>(*observed.rows[0].name_payload).default_language_tag=="de-DE","updated name-vector presentation language retained as data");
    else Check(std::get<catalog::CatalogNameEntryView>(*observed.rows[0].name_payload).raw_name_text=="renamed","updated name-entry display data retained with immutable binary identity");}
   for(usize i=0;i<observed.observations.size();++i)Check(observed.observations[i].retained_row_index==oracle.observations[i].retained_row_index&&
     observed.observations[i].decision==oracle.observations[i].decision,"native traversal observations retained in exact order");
   const auto total=observed.source.retained_image_bytes;Check(read(total).ok(),"exact complete source/image budget");empty(read(total-1));
   for(unsigned mode=0;mode<2;++mode)for(unsigned site=1;site<=sites;++site){reads=0;history_observed_read_bytes=0;io_counting=true;
    if(mode)corrupt_read=site;else read_fault=site;const auto r=read(allowance);io_counting=false;read_fault=corrupt_read=0;
    empty(r);Check(reads>=site&&r.source.physical_bytes_read==history_observed_read_bytes,"complete visibility preserves exact failed-read effects");}
   for(unsigned mode=1;mode<=5;++mode)for(unsigned site=1;site<=hash_sites;++site){hash_fault=mode;hash_target=site;hash_seen=0;hash_active=false;
    const auto r=read(allowance);const bool consumed=!hash_fault;hash_fault=0;hash_active=false;empty(r);Check(consumed,"complete visibility consumes each hash fault");}
   allocation_budget=0;const auto retry=read(allowance);const auto retry_left=allocation_budget;allocation_budget=-1;
   Check(retry.ok()&&retry_left==0,"same-lease full visibility recovers after every physical refusal");
   if(context_namespaces){
    using NE=db::NativeContextNamespaceReadError;
    const auto lookup=[&](const db::NativeContextNamespaceBinding& expected,u64 budget){
     return pin.valid()?db::NativeContextNamespaceLeaseReader::ReadPinned(*source.lease,2,1,binding,reader,pin,expected,budget,prepared.workspace):
      db::NativeContextNamespaceLeaseReader::ReadCommitted(*source.lease,2,1,binding,expected,budget,prepared.workspace);};
    for(const auto& row:oracle.rows){
     const db::NativeContextNamespaceBinding expected{Id(1),row.metadata.record.header.object_uuid.value,row.metadata.definition_version};
     allocation_budget=0;const auto typed=lookup(expected,allowance);const auto remaining=allocation_budget;allocation_budget=-1;
     Check(typed.ok()&&remaining==0&&typed.definition->namespace_uuid==expected.namespace_uuid&&
       typed.definition->generation==expected.definition_generation&&typed.selected->version_uuid==row.version_uuid&&
       typed.selected->provisional==row.provisional&&typed.selected->effective_lifecycle==row.effective_lifecycle&&
       typed.selected->effective_status==row.effective_status&&typed.selected->metadata.record.header.deleted==row.metadata.record.header.deleted&&
       typed.source.snapshot_uuid==oracle.snapshot_uuid,"typed namespace observation preserves exact selected row, lifecycle and pin without heap");
    }
    const auto& row=oracle.rows.front();const db::NativeContextNamespaceBinding expected{Id(1),row.metadata.record.header.object_uuid.value,row.metadata.definition_version};
    const auto refused=[](const auto& r,NE error){Check(!r.ok()&&!r.definition&&!r.selected&&r.error==error,"namespace refusal returns no selected definition");};
    for(unsigned bad=0;bad<7;++bad){auto changed=expected;auto budget=allowance;
     if(bad==0)changed.database_uuid={};if(bad==1)changed.namespace_uuid.bytes[6]=0x40;
     if(bad==2)changed.definition_generation=0;if(bad==3)budget=0;
     if(bad==4)changed.database_uuid=Id(63999);if(bad==5)changed.namespace_uuid=Id(63999);
     if(bad==6)++changed.definition_generation;
     reads=0;io_counting=true;allocation_budget=0;const auto denied=lookup(changed,budget);
     const auto remaining=allocation_budget;allocation_budget=-1;io_counting=false;
     refused(denied,bad<4?NE::invalid_request:bad==4?NE::node_mismatch:bad==5?NE::missing:NE::generation_mismatch);
     Check(remaining==0&&(bad>=5||reads==0),"invalid namespace binding refuses before I/O and every refusal avoids heap");
    }
    refused(lookup(expected,1),NE::source_failure);
    // Sweep the typed entry point too, not only its underlying generic reader.
    for(unsigned site=1;site<=sites;++site){reads=0;history_observed_read_bytes=0;read_fault=site;io_counting=true;
     const auto failed=lookup(expected,allowance);io_counting=false;read_fault=0;
     Check(reads>=site,"typed namespace actual read fault consumed");refused(failed,NE::source_failure);
     Check(!failed.source.ok()&&failed.source.source.physical_bytes_read==history_observed_read_bytes,
       "typed refusal preserves exact source diagnostic/effect evidence including zero transferred bytes");
    }
    Check(lookup(expected,allowance).ok(),"typed namespace same-lease retry after physical refusals");
   }
   if(!context_namespaces&&scenario==0&&!oracle.rows.empty()){
    const auto& row=oracle.rows.front();allocation_budget=0;
    const auto wrong_family=db::NativeContextNamespaceLeaseReader::ReadCommitted(*source.lease,2,1,binding,
      {Id(1),row.metadata.record.header.object_uuid.value,row.metadata.definition_version},allowance,prepared.workspace);
    const auto remaining=allocation_budget;allocation_budget=-1;
    Check(!wrong_family.ok()&&!wrong_family.definition&&!wrong_family.selected&&
      wrong_family.error==db::NativeContextNamespaceReadError::family_mismatch&&remaining==0,
      "generic catalog object with matching UUID and generation cannot masquerade as a namespace");
   }
   if(pin.valid()){
    auto foreign=reader;foreign.transaction_uuid.value=Id(63991);
    const auto refused=db::NativeCatalogVisibilityLeaseReader::ReadPinned(*source.lease,2,1,binding,foreign,pin,allowance,prepared.workspace);
    empty(refused);Check(refused.error==E::reader_mismatch&&!refused.source.physical_bytes_read,"foreign reader fails before physical work");
    std::thread revoke([&]{while(historical_read_pause!=2)std::this_thread::yield();mga::RevokePublishedSnapshotVector(snapshot_id);historical_read_pause=3;});
    historical_read_pause=1;
    db::NativeCatalogVisibilityLeaseResult revoked;
    if(context_namespaces){const auto& row=oracle.rows.front();
     auto typed=db::NativeContextNamespaceLeaseReader::ReadPinned(*source.lease,2,1,binding,reader,pin,
       {Id(1),row.metadata.record.header.object_uuid.value,row.metadata.definition_version},allowance,prepared.workspace);
     // Join before checking to avoid a failed assertion escaping a live thread.
     historical_read_pause=0;revoke.join();
     Check(!typed.ok()&&!typed.definition&&!typed.selected&&typed.error==db::NativeContextNamespaceReadError::source_failure,
       "revocation during typed namespace I/O prevents definition delivery");revoked=std::move(typed.source);
    }else {revoked=read(allowance);historical_read_pause=0;revoke.join();}
    empty(revoked);
    Check(revoked.error==E::snapshot_failure&&revoked.source.physical_bytes_read>0&&
      revoked.snapshot_diagnostic.error==mga::PublishedSnapshotObservationError::revoked,"revocation during actual I/O prevents delivery after real physical reads");
    if(context_namespaces){const auto& row=oracle.rows.front();
     const auto denied=db::NativeContextNamespaceLeaseReader::ReadPinned(*source.lease,2,1,binding,reader,pin,
       {Id(1),row.metadata.record.header.object_uuid.value,row.metadata.definition_version},allowance,prepared.workspace);
     Check(!denied.ok()&&!denied.definition&&!denied.selected&&denied.error==db::NativeContextNamespaceReadError::source_failure&&
       denied.source.error==E::snapshot_failure&&!denied.source.source.physical_bytes_read,"typed namespace cannot bypass revoked pin");}
   }
  }
 }
 prepared.workspace={};memory.memory={};source_memory.memory={};memory.Empty();source_memory.Empty();
 if(!pin.valid()){
  f.Reopen();const auto reopened=db::ReadNativeCommittedCatalogVersionsFromOpenDevices(Id(1),storage.devices,checkpoint,2,1,binding,allowance);
  Check(reopened.error==oracle.error&&reopened.rows.size()==oracle.rows.size(),"read-only reopen preserves committed selection or exact refusal");
 }
}
