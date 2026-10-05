// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
// Included after actual directory fixtures and common catalog envelope builders.
void DirectoryCatalogRelationMemory(unsigned primary,unsigned secondary,bool indexed,unsigned fault=0){
 using E=db::NativeCatalogRelationLeaseError;
 const bool refusal=fault>=1&&fault<=6,empty=fault==9,known_outcome=fault==7||fault==8;
 const unsigned expected_rows=empty?0:indexed?4:2;
 DirectoryHistoryFixture f(primary,secondary,false,2,false,false,true,false);
 auto& storage=f.t.fixture;auto cp=*db::DecodeNativeCheckpointRoot(f.t.base_cp).root;
 const auto cp_catalog=std::find_if(cp.roots.begin(),cp.roots.end(),[](const auto& r){return r.role==5;});
 Check(cp_catalog!=cp.roots.end(),"real selected catalog root for relation fixture");
 const d::FilespaceRootReference catalog_ref{2,5,cp_catalog->page.filespace_uuid,cp_catalog->page.page_number,
   cp_catalog->page.page_generation,cp_catalog->page.page_size_profile_uuid,cp_catalog->object_uuid};
 auto catalog_image=page::ReadNativeCatalogRootFromOpenDevice(storage.device,Id(1),catalog_ref);
 Check(catalog_image.ok(),"actual catalog relation heads");auto catalog=*catalog_image.root;
 auto head=std::find_if(catalog.roots.begin(),catalog.roots.end(),[](const auto& r){return r.role==1;});
 Check(head!=catalog.roots.end()&&head->page_type==6,"real object-catalog direct head");
 const auto original_head=*head;
 const auto leaf=db::ReadNativeCatalogLeafFromOpenDevice(storage.device,Id(1),*head);
 Check(leaf.ok()&&leaf.page->body.rows.empty(),"original empty catalog leaf");
 const mga::TransactionIdentity creator{{catalog.creator_local_transaction_id},
   {UuidKind::transaction,catalog.creator_transaction_uuid},mga::TransactionScope::local_node};
 auto first=empty?db::EncodeNativeCatalogLeaf(*leaf.page):owned_source_memory::PopulatedLeaf(*leaf.page,creator);
 Check(first.ok(),"complete first real common-metadata base image");f.Write(first.bytes);
 const auto& q=d::kCanonicalFilespacePageProfiles[secondary];
 auto second_page=*leaf.page;second_page.header.filespace_uuid=f.t.other;second_page.header.page_number=249;
 second_page.header.page_size_bytes=q.page_size_bytes;second_page.header.page_size_profile_uuid=q.uuid;
 second_page.header.page_uuid=Id(62001);second_page.header.page_generation=1;
 second_page.body.page_number=249;second_page.body.page_generation=1;
 auto second=owned_source_memory::PopulatedLeaf(second_page,creator,fault==2?0:2);
 Check(second.ok(),"complete second mixed-profile base image");
 auto row_creator=creator;
 if(known_outcome){
  auto ref=std::find_if(cp.roots.begin(),cp.roots.end(),[](const auto& r){return r.role==1;});
  Check(ref!=cp.roots.end(),"actual inventory root for retained creator outcome");
  const auto decoded=page::DecodeNativeTransactionInventoryPage(storage.Read(ref->page.page_number));
  Check(decoded.ok(),"original creator inventory");auto inventory=*decoded.page;
  auto begun=mga::BeginLocalTransaction(inventory.inventory,{UuidKind::transaction,Id(62098)},2000000000000ULL);
  Check(begun.ok(),"real candidate active transaction outcome");row_creator=begun.inventory.entries.back().identity;
  if(fault==8){auto rolled=mga::RollbackLocalTransaction(begun.inventory,row_creator.local_id,2000000000001ULL);
   Check(rolled.ok(),"real candidate rolled-back outcome");begun.inventory=std::move(rolled.inventory);}
  inventory.inventory=std::move(begun.inventory);const auto raw=InventoryOracle(inventory);
  Check(page::EncodeNativeTransactionInventoryPage(inventory).bytes==raw,"independent complete retained creator inventory image");
  f.Write(raw);ref->sha256=Sha(raw);cp.selected_local_transaction_id=inventory.inventory.next_local_transaction_id-1;
 }
 if(fault==1||known_outcome){auto missing=row_creator;if(fault==1)missing.transaction_uuid.value=Id(62099);
  auto bad=owned_source_memory::PopulatedLeaf(second_page,missing,2);Check(bad.ok(),"well-framed unresolved hidden creator");
  auto mixed=*second.page;mixed.body.rows[1]=bad.page->body.rows[1];second=db::EncodeNativeCatalogLeaf(mixed);
  Check(second.ok(),"only unindexed row carries missing creator");}
 page::NativeBtreePage tree;
 tree.header={q.page_size_bytes,0x200,Id(1),f.t.other,Id(62002),248,1,0,q.uuid};
 tree.dependencies={Id(62003),1,1,Id(62004),Id(62005),Id(62006),{}};tree.dependencies.dependency_map_sha256.fill(39);
 tree.creator_transaction_uuid=row_creator.transaction_uuid.value;tree.creator_local_transaction_id=row_creator.local_id.value;
 tree.maintenance_state=1;
 // Two different keys share each first row. The second row on each page is
 // deliberately unindexed but must still be checked against actual inventory.
 for(unsigned i=0;!empty&&i<4;++i){const auto& row=(i<2?first:second).page->body.rows.front();
  tree.cells.push_back({{{static_cast<byte>(i+1)},row.row_uuid.value,row.version_uuid},i==3,{},
    i<2?original_head.page:Self(second.page->header)});}
 if(fault==3)tree.cells.back().key.row_uuid=Id(62090);
 if(fault==4)tree.cells.back().key.version_uuid=Id(62091);
 if(fault==5)++tree.cells.back().base_page->page_generation;
 if(fault==6)tree.cells.back().base_page.reset();
 db::NativeCatalogRelationBinding binding{original_head.object_uuid,{}};
 if(indexed){
  const auto encoded=page::EncodeNativeBtreePage(tree);Check(encoded.ok(),"complete catalog index fixture including deleted entry");
  f.Write(encoded.bytes);f.Write(second.bytes);binding.index_dependencies=tree.dependencies;
  *head={1,0x200,Self(tree.header),tree.dependencies.index_uuid};
  auto maps=f.t.before.at(f.t.other);
  for(const auto& h:{tree.header,second.page->header}){auto& map=Cover(maps,h.page_number);
   Check(map.states[h.page_number-map.first_page]==State::free,"actual free catalog fixture slot");
   map.states[h.page_number-map.first_page]=State::allocated;
   map.records.push_back({h.page_number,Id(62100+h.page_number),h.page_uuid,h.page_type==6?binding.relation_uuid:tree.dependencies.index_uuid,
     creator.transaction_uuid.value,creator.local_id.value,1,0,h.page_type,{}});}
  for(auto& map:maps)std::sort(map.records.begin(),map.records.end(),[](const auto& a,const auto& b){return a.page_number<b.page_number;});
  for(const auto& raw:EncodeMaps(maps))f.Write(raw);
  auto zero=f.t.zeros.at(f.t.other);zero.free_pages-=2;const auto encoded_zero=d::EncodeFilespacePageZero(zero);
  Check(encoded_zero.ok()&&storage.secondary.WriteAt(0,encoded_zero.bytes->data(),encoded_zero.bytes->size()).ok(),"complete indexed-member pagezero");
 }
 const auto changed=page::EncodeNativeCatalogRoot(catalog);Check(changed.ok(),"complete selected catalog successor fixture");f.Write(changed.bytes);
 for(auto& r:cp.roots)if(r.role==5||r.role==9){Check(r.page==Self(catalog.header),"fixture catalog and feature share exact root");r.sha256=Sha(changed.bytes);}
 const auto checkpoint=db::EncodeNativeCheckpointRoot(cp);Check(checkpoint.ok(),"actual rebound catalog checkpoint");f.Write(checkpoint.bytes);
 const auto digest=Sha(checkpoint.bytes);
 for(const auto& ref:f.t.graph.zero.roots){
  if(ref.kind==18||ref.kind==19){auto selector=*db::DecodeNativeCheckpointSelection(storage.Read(ref.page_number)).selection;
   selector.checkpoint_sha256=digest;f.Write(SelectorOracle(selector));}
  if(ref.kind==20||ref.kind==21){auto watermark=*db::DecodeNativePublicationWatermark(storage.Read(ref.page_number)).state;
   watermark.base_checkpoint_sha256=digest;const auto encoded=db::EncodeNativePublicationWatermark(watermark);
   Check(encoded.ok(),"selected catalog fixture watermark");f.Write(encoded.bytes);}
 }
 for(const auto& member:storage.devices)Check(member.device->Sync().ok(),"catalog fixture durable barrier");
 const u64 allowance=16*1024*std::max<u64>(storage.size,q.page_size_bytes);
 const d::FilespaceRootReference checkpoint_ref{9,0x300,cp.header.filespace_uuid,cp.header.page_number,cp.header.page_generation,cp.header.page_size_profile_uuid,cp.object_uuid};
 auto oracle=db::ReadNativeCheckpointCatalogRelationFromOpenDevices(Id(1),storage.devices,checkpoint_ref,2,1,binding,allowance);
 Check(oracle.ok()==!refusal,"independent owning relation reader agrees on fixture validity");
 const db::NativeCatalogRelationMemoryLimits limits{d::kCanonicalFilespacePageProfiles.back().uuid,3,2,empty?0u:4u,empty?0u:4u,{1,q.page_size_bytes}};
 const auto bytes=db::NativeCatalogRelationWorkspaceBytes(limits);
 checkpoint_inventory_memory::Grant memory(bytes),source_memory(32*1024*1024);
 if(!refusal){
  for(unsigned field=0;field<4;++field){auto wrong=memory.binding;
   const std::array<Uuid*,4> ids{&wrong.database_uuid,&wrong.operation_uuid,&wrong.owner_uuid,&wrong.context_uuid};*ids[field]=Id(62999);
   const auto refused=db::PrepareNativeCatalogRelationRead(limits,memory.memory,wrong);
   Check(!refused.ok()&&!refused.workspace&&!memory.manager.Snapshot().current_bytes,"composed backing binds every grant identity without partial retention");}
  checkpoint_inventory_memory::Grant short_memory(bytes-1);
  const auto refused=db::PrepareNativeCatalogRelationRead(limits,short_memory.memory,short_memory.binding);
  Check(!refused.ok()&&!refused.workspace&&!short_memory.manager.Snapshot().current_bytes,"late one-byte-short composition backing releases every component");
  short_memory.memory={};short_memory.Empty();
 }
 auto prepared=db::PrepareNativeCatalogRelationRead(limits,memory.memory,memory.binding);
 Check(prepared.ok(),"all catalog composition backing pre-admitted");
 auto moved=std::move(prepared.workspace);
 {
  auto source=db::AcquireNativeSelectedCheckpointMemoryLease(Id(1),storage.devices,Id(2),{allowance,32*1024*1024},source_memory.memory,source_memory.binding);
  Check(source.ok(),"actual source remains admitted independently of catalog row contents");
  const auto empty=[](const auto& r){Check(!r.ok()&&r.roots.catalogs.empty()&&r.tree.pages.empty()&&r.tree.leaves.empty()&&
    r.bases.empty()&&r.bindings.empty()&&r.row_creators.empty()&&r.navigation_creators.empty()&&!r.retained_image_bytes,
    "failed relation discards all verified graph/base/creator prefixes");};
  const auto read=[&](u64 budget,u16 selector=2){return db::NativeCatalogRelationLeaseReader::Read(*source.lease,selector,1,binding,budget,prepared.workspace);};
  const auto absent=read(allowance);empty(absent);Check(!absent.physical_bytes_read,"moved composed backing refuses before I/O");
  prepared.workspace=std::move(moved);
  allocation_budget=0;reads=0;hash_seen=0;io_counting=hash_counting=true;
  auto observed=read(allowance);const auto left=allocation_budget;allocation_budget=-1;io_counting=hash_counting=false;
  const auto read_sites=reads,hash_sites=hash_seen;
  Check(left==0,"complete catalog composition does not allocate under the source lease");
  if(refusal){empty(observed);
   const auto expected=fault==1?E::creator_mismatch:fault==2?E::duplicate_identity:
     fault==3||fault==5?E::binding_mismatch:E::invalid_locator;
   Check(observed.error==expected,"exact late catalog identity/creator failure");
  }else{
   Check(observed.ok()&&observed.bases.size()==(indexed?2:1)&&observed.bindings.size()==expected_rows&&
     observed.row_creators.size()==expected_rows&&observed.navigation_creators.size()==(indexed?1:0),
     "direct/indexed composition checks all rows and preserves duplicate keys and deleted entries");
   for(std::size_t p=0;p<observed.bases.size();++p){const auto& a=observed.bases[p];const auto& b=oracle.relation.catalogs[p];
    Check(std::equal(a.image.begin(),a.image.end(),b.bytes.begin(),b.bytes.end()),"every complete base image matches independent source");}
   for(std::size_t i=0;i<observed.bindings.size();++i){const auto& a=observed.bindings[i];const auto& b=oracle.relation.bindings[i];
    Check(a.catalog_page_index==b.catalog_page_index&&a.catalog_row_index==b.catalog_row_index&&a.index_entry.has_value()==b.index_entry.has_value(),"exact row binding oracle");}
   for(const auto& r:observed.row_creators){const auto& row=observed.bases[r.catalog_page_index].page.body.rows[r.catalog_row_index];
    Check(source.lease->selection().checkpoint_inventory.inventory.entries[r.inventory_entry_index].identity.transaction_uuid.value==row.transaction_uuid.value,"every retained row creator points into actual source inventory");}
   if(known_outcome){const auto& inventory=source.lease->selection().checkpoint_inventory.inventory.entries;
    const auto state=fault==7?mga::TransactionState::active:mga::TransactionState::rolled_back;
    Check(inventory[observed.navigation_creators.front()].state==state&&inventory[observed.row_creators.back().inventory_entry_index].state==state,
      "creator binding retains actual unresolved/rollback outcomes without claiming visibility");}
   const auto total=observed.retained_image_bytes;
   Check(read(total,8).ok(),"shared feature selector and exact total source/image budget");
   const auto short_read=read(total-1);empty(short_read);Check(short_read.error==E::resource_exhausted,"one-short total composition allowance refuses late without prefix");
   for(unsigned mode=0;mode<2;++mode)for(unsigned site=1;site<=read_sites;++site){
    reads=0;history_observed_read_bytes=0;io_counting=true;if(mode)corrupt_read=site;else read_fault=site;
    const auto no=read(allowance);io_counting=false;read_fault=corrupt_read=0;empty(no);
    Check(reads>=site&&no.physical_bytes_read==history_observed_read_bytes,"every composition read/corruption preserves physical effects only");}
   for(unsigned mode=1;mode<=5;++mode)for(unsigned site=1;site<=hash_sites;++site){
    hash_fault=mode;hash_target=site;hash_seen=0;hash_active=false;
    const auto no=read(allowance);const bool consumed=!hash_fault;hash_fault=0;hash_active=false;empty(no);
    Check(consumed,"every catalog composition hash-provider failure exercised");}
   allocation_budget=0;const auto retry=read(allowance);const auto remaining=allocation_budget;allocation_budget=-1;
   Check(retry.ok()&&remaining==0,"complete relation can retry under the same retained source");
  }
 }
 prepared={};memory.memory={};memory.Empty();
 if(!refusal&&!empty)for(unsigned mode=indexed?0:1;mode<3;++mode){auto short_limits=limits;
  if(mode==0)short_limits.maximum_base_pages=1;
  if(mode==1)short_limits.maximum_rows=indexed?3:1;
  if(mode==2)short_limits.maximum_bindings=indexed?3:1;
  checkpoint_inventory_memory::Grant narrow(db::NativeCatalogRelationWorkspaceBytes(short_limits));
  auto backing=db::PrepareNativeCatalogRelationRead(short_limits,narrow.memory,narrow.binding);
  Check(backing.ok(),"bounded composed backing admitted before source");
  {
   auto source=db::AcquireNativeSelectedCheckpointMemoryLease(Id(1),storage.devices,Id(2),{allowance,32*1024*1024},source_memory.memory,source_memory.binding);
   Check(source.ok(),"actual selected source for bounded composition");allocation_budget=0;
   const auto no=db::NativeCatalogRelationLeaseReader::Read(*source.lease,2,1,binding,allowance,backing.workspace);
   const auto remaining=allocation_budget;allocation_budget=-1;
   Check(remaining==0&&no.error==E::resource_exhausted&&no.roots.catalogs.empty()&&no.tree.pages.empty()&&
     no.bases.empty()&&no.bindings.empty()&&no.row_creators.empty()&&!no.retained_image_bytes,"every composed page/row/binding bound refuses without successful prefix");
  }
  backing={};narrow.memory={};narrow.Empty();
 }
 source_memory.memory={};source_memory.Empty();
 if(!refusal){f.Reopen();const auto reopened=db::ReadNativeCheckpointCatalogRelationFromOpenDevices(Id(1),storage.devices,checkpoint_ref,2,1,binding,allowance);
  Check(reopened.ok()&&reopened.relation.bindings.size()==expected_rows,"independent read-only reopen of complete relation fixture");}
}
