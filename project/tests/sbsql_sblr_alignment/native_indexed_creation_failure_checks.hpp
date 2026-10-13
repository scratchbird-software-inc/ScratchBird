// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
// Included after the real-file fixture and indexed seed builders.
namespace {
void NativeIndexedRecoveryNegativeChecks() {
  const auto request=Request(0,128);const auto seed=DenseIndexedCatalogSeed(request,136);
  const auto indexes=IndexedConstructionBindings();const u64 size=request.bootstrap.page_size_bytes,budget=256*size;
  Fixture fixture;disk::FileDevice d;const auto path=fixture.Next();
  Check(d.Open(path.string(),disk::FileOpenMode::create_new).ok(),"indexed negative actual owner");
  const auto created=db::InitializeIndexedNativeCreationWorkspaceOnOpenDevice(d,request,seed,indexes,budget,*fixture.issuer);
  Check(created.ok(),"indexed negative complete graph");
  page_bytes=size;total_pages=request.total_pages;selector_page=created.receipt->map_pages+16;
  const auto original=Read(d,0,total_pages*size);
  const std::vector<disk::NativeFilespaceDevice> devices{{request.bootstrap.filespace_uuid,request.bootstrap.page_size_profile_uuid,&d}};
  const auto tree=page::ReadNativeBtreeTreeFromOpenDevices(request.bootstrap.database_uuid,devices,
      created.receipt->relations[0].page,indexes[0]->dependencies,budget);
  Check(tree.ok()&&tree.leaves.size()>2,"resealed negatives have real multiple-child navigation");
  const auto set_page=[&](auto& bytes,u64 n,const auto& page) {
    Check(page.size()==size,"negative full physical image");std::copy(page.begin(),page.end(),bytes.begin()+n*size);
  };
  for(unsigned fault=0;fault<10;++fault) {
    auto bytes=original;auto bindings=indexes;auto inputs=seed;
    std::fill(bytes.begin()+(selector_page+1)*size,bytes.begin()+(selector_page+2)*size,0);
    if(fault<4) {
      auto node=*tree.pages[tree.leaves.front()].page;
      if(fault==0)++node.cells.front().base_page->page_number;
      if(fault==1)node.cells.front().key.encoded_key.front()=0;
      if(fault==2)node.high_fence->version_uuid=Id(40000);
      if(fault==3)++node.right->page_generation;
      const auto encoded=page::EncodeNativeBtreePage(node);Check(encoded.ok(),"resealed negative is a valid individual B-tree page");
      set_page(bytes,node.header.page_number,encoded.bytes);
    }
    if(fault==4)bytes.back()=1; // Not allocated, but physically nonzero.
    if(fault==5)++bindings[0]->dependencies.storage_generation;
    if(fault==6)bindings[0]->dependencies.dependency_map_sha256.back()^=1;
    if(fault==7)inputs[0][0].metadata.record.payload.back()^=1;
    if(fault>=8) {
      const auto root=created.receipt->relations[0].page.page_number;
      u64 coverage=(size-384)/128;
      while(((384+(coverage+1)/2+7)&~u64{7})+128*coverage>size)--coverage;
      const u64 map_page=root/coverage+1;
      const auto actual=Read(d,map_page,size);auto map=page::DecodeNativeAllocationMap(actual);
      Check(map.ok(),"actual indexed allocation negative");
      auto record=std::find_if(map.map->records.begin(),map.map->records.end(),[&](const auto& r){return r.page_number==root;});
      Check(record!=map.map->records.end(),"actual index-owned allocation present");
      if(fault==8)record->owner_uuid=Id(40001);else record->page_type=6;
      const auto encoded=page::EncodeNativeAllocationMap(*map.map);Check(encoded.ok(),"resealed allocation negative remains structurally valid");
      set_page(bytes,map_page,encoded.bytes);
    }
    Check(d.WriteAt(0,bytes.data(),bytes.size()).ok()&&d.Sync().ok(),"persist resealed interrupted graph");
    Arm();const auto refused=db::RecoverIndexedNativeCreationWorkspaceSelectionOnOpenDevice(d,
        {request.bootstrap.database_uuid,request.bootstrap.filespace_uuid,request.bootstrap.page_size_profile_uuid},
        request.operation_uuid,inputs,bindings,budget);Disarm();
    Check(!refused.ok()&&!refused.receipt&&!refused.repaired_slots&&!calls[write_call]&&Read(d,0,bytes.size())==bytes,
        "resealed topology/identity/allocation/seed corruption prevents every selector write");
    Check(d.WriteAt(0,original.data(),original.size()).ok()&&d.Sync().ok(),"restore original qualified graph between negatives");
  }
  Check(d.Close().ok(),"close indexed negative owner");
}
void NativeIndexedCreationFailureChecks(unsigned shard,unsigned first_allocation=1) {
  Check(shard<4,"indexed fault shard");
  Check(first_allocation>=1,"positive diagnostic allocation range");
  const auto request=Request(0,128);const auto seed=DenseIndexedCatalogSeed(request,47);
  const auto indexes=IndexedConstructionBindings();const u64 size=request.bootstrap.page_size_bytes,budget=256*size;
  page_bytes=size;total_pages=request.total_pages;
  u64 coverage=(size-384)/128;
  while(((384+(coverage+1)/2+7)&~u64{7})+128*coverage>size)--coverage;
  selector_page=(total_pages+coverage-1)/coverage+16;
  const auto create=[&](disk::FileDevice& d,Fixture& f) {
    return db::InitializeIndexedNativeCreationWorkspaceOnOpenDevice(d,request,seed,indexes,budget,*f.issuer);
  };
  // Warm process-owned codecs, then measure on a fresh actual device/issuer.
  for(unsigned i=0;i<2;++i) {Fixture f;disk::FileDevice d;
    Check(d.Open(f.Next().string(),disk::FileOpenMode::create_new).ok(),"warm indexed creation owner");
    Check(create(d,f).ok(),"warm indexed creation actual graph");}
  Fixture measured;disk::FileDevice d;const auto path=measured.Next();
  Check(d.Open(path.string(),disk::FileOpenMode::create_new).ok(),"indexed fault measurement owner");
  Arm();allocations=0;allocation_fault=0;count_allocations=true;
  const auto baseline=create(d,measured);count_allocations=false;Disarm();
  Check(baseline.ok(),"indexed construction fault baseline");
  const auto create_counts=calls;const unsigned create_allocations=allocations;
  Check(first_allocation<=create_allocations,"diagnostic range must exercise real construction sites");
  std::cout<<"indexed fault baseline shard="<<shard<<" create_allocations="<<create_allocations
      <<" diagnostic_range="<<(first_allocation>1)<<std::endl;
  const auto original=Read(d,0,total_pages*size);
  for(unsigned mode=1;mode<=5;++mode) {
    if(mode%4!=shard)continue;
    Fixture f;disk::FileDevice target;Check(target.Open(f.Next().string(),disk::FileOpenMode::create_new).ok(),"owned multipart hash fault");
    Arm();context_fault=mode;const auto refused=create(target,f);context_fault=0;Disarm();
    Check(!refused.ok()&&!refused.receipt&&refused.error==db::NativeCreationWorkspaceError::hash_failure&&
        !calls[write_call]&&!target.Size().size_bytes,"multipart hash backend failure cannot publish an unsigned graph");
  }
  for(unsigned kind=0;kind<call_count;++kind)for(unsigned at=1;at<=create_counts[kind];++at) {
    if(first_allocation>1)continue; // Diagnostic range only; registered gates always start at1.
    if(at%4!=shard)continue;
    Fixture f;disk::FileDevice target;Check(target.Open(f.Next().string(),disk::FileOpenMode::create_new).ok(),"owned construction fault device");
    Arm(kind,at);const auto refused=create(target,f);Disarm();
    const auto expected=(kind==hash_call)?db::NativeCreationWorkspaceError::hash_failure:
        db::NativeCreationWorkspaceError::io_failure;
    if(kind!=random_call&&refused.error!=expected)
      std::cerr<<"construction fault kind="<<kind<<" site="<<at<<" cause="
          <<static_cast<unsigned>(refused.error)<<" selection="<<static_cast<unsigned>(refused.selection_error)
          <<" checkpoint="<<static_cast<unsigned>(refused.checkpoint_error)<<'\n';
    Check(calls[kind]>=at&&!refused.ok()&&!refused.receipt,"every actual construction read/write/sync/random/hash failure prevents receipt");
    if(kind==read_call||kind==write_call||kind==sync_call)
      Check(refused.error==db::NativeCreationWorkspaceError::io_failure,
          "construction physical I/O failure retains its exact typed cause");
    if(kind==hash_call)
      Check(refused.error==db::NativeCreationWorkspaceError::hash_failure,
          "construction hash backend failure retains its exact typed cause");
    if(target.Size().size_bytes) {
      const auto before=Read(target,0,target.Size().size_bytes);
      const auto retry=create(target,f);
      Check(!retry.ok()&&retry.error==db::NativeCreationWorkspaceError::device_not_empty&&
          Read(target,0,before.size())==before,"failed physical construction is neither replayed nor silently truncated");
    }
  }
  for(unsigned at=1;at<=create_allocations;++at) {
    if(at<first_allocation||at%4!=shard)continue;
    Fixture f;disk::FileDevice target;Check(target.Open(f.Next().string(),disk::FileOpenMode::create_new).ok(),"owned creation allocation fault");
    const auto lost=target.failed_io_latency_observations();
    allocations=0;allocation_fault=at;count_allocations=true;
    const auto result=create(target,f);count_allocations=false;allocation_fault=0;
    Check(allocations>=at,"indexed construction allocation site is actually exercised");
    if(result.ok()) {
      Check(target.failed_io_latency_observations()==lost+1,"only explicitly recorded observation loss permits an allocation-fault success");
      const std::vector<disk::NativeFilespaceDevice> files{{request.bootstrap.filespace_uuid,request.bootstrap.page_size_profile_uuid,&target}};
      const auto read=db::ReadNativeCommittedCatalogVersionsFromOpenDevices(request.bootstrap.database_uuid,files,
          result.receipt->checkpoint,2,1,{indexes[0]->relation_uuid,indexes[0]->dependencies},budget);
      Check(read.ok()&&read.rows.size()==47,"telemetry loss cannot hide incomplete construction");
    }else {
      if(result.error!=db::NativeCreationWorkspaceError::resource_exhausted)
        std::cerr<<"creation allocation site="<<at<<" error="<<static_cast<unsigned>(result.error)
            <<" checkpoint="<<static_cast<unsigned>(result.checkpoint_error)<<'\n';
      Check(!result.receipt&&result.error==db::NativeCreationWorkspaceError::resource_exhausted,
          "required construction allocation failure is a typed refusal, never a partial receipt");
    }
  }
  const std::vector<byte> blank(size,0);
  const auto damage=[&] {
    Check(d.WriteAt((selector_page+1)*size,blank.data(),blank.size()).ok()&&d.Sync().ok(),"restore exact interrupted selector preimage");
  };
  const auto recover=[&] {
    return db::RecoverIndexedNativeCreationWorkspaceSelectionOnOpenDevice(d,
        {request.bootstrap.database_uuid,request.bootstrap.filespace_uuid,request.bootstrap.page_size_profile_uuid},
        request.operation_uuid,seed,indexes,budget);
  };
  damage();Check(recover().ok(),"warm original indexed recovery");damage();
  Arm();allocations=0;allocation_fault=0;count_allocations=true;
  const auto repaired=recover();count_allocations=false;Disarm();
  Check(repaired.ok()&&Read(d,0,original.size())==original,"indexed recovery fault baseline restores exact original graph");
  const auto recover_counts=calls;const unsigned recover_allocations=allocations;
  for(unsigned mode=1;mode<=5;++mode) {
    if(mode%4!=shard)continue;
    damage();const auto before=Read(d,0,original.size());Arm();context_fault=mode;
    const auto refused=recover();context_fault=0;Disarm();
    Check(!refused.ok()&&!refused.receipt&&refused.error==db::NativeCreationRecoveryError::hash_failure&&
        !calls[write_call]&&Read(d,0,original.size())==before,"multipart hash refusal preserves original indexed recovery preimage");
  }
  for(unsigned at=1;at<=recover_counts[read_call];++at) {
    if(at%4!=shard)continue;
    damage();Arm();corrupt_at=at;const auto refused=recover();Disarm();corrupt_at=0;
    Check(!refused.ok()&&!refused.receipt,"every corrupted read or readback prevents a recovery completion receipt");
    const auto after=Read(d,0,original.size());const auto begin=(selector_page+1)*size,end=begin+size;
    Check(std::equal(after.begin(),after.begin()+begin,original.begin())&&
        std::equal(after.begin()+end,after.end(),original.begin()+end),"read corruption never alters original non-selector graph");
    Check(recover().ok()&&Read(d,0,original.size())==original,"corruption retry reconciles actual selector effects");
  }
  for(unsigned kind=0;kind<call_count;++kind)for(unsigned at=1;at<=recover_counts[kind];++at) {
    if(at%4!=shard)continue;
    damage();Arm(kind,at);const auto refused=recover();Disarm();
    const auto expected=(kind==hash_call)?db::NativeCreationRecoveryError::hash_failure:
        db::NativeCreationRecoveryError::io_failure;
    if(kind!=random_call&&refused.error!=expected)
      std::cerr<<"recovery fault kind="<<kind<<" site="<<at<<" cause="
          <<static_cast<unsigned>(refused.error)<<'\n';
    Check(calls[kind]>=at&&!refused.ok()&&!refused.receipt,"every actual indexed recovery read/write/sync/hash failure prevents completion receipt");
    if(kind==read_call||kind==write_call||kind==sync_call)
      Check(refused.error==db::NativeCreationRecoveryError::io_failure,
          "recovery physical I/O failure retains its exact typed cause");
    if(kind==hash_call)
      Check(refused.error==db::NativeCreationRecoveryError::hash_failure,
          "recovery hash backend failure retains its exact typed cause");
    const auto after=Read(d,0,original.size());
    const auto begin=(selector_page+1)*size,end=begin+size;
    Check(std::equal(after.begin(),after.begin()+begin,original.begin())&&
        std::equal(after.begin()+end,after.end(),original.begin()+end),"failed recovery never changes non-selector dependencies");
    Check(recover().ok()&&Read(d,0,original.size())==original,"retry reconciles retained selector effects without fresh identities or pages");
  }
  for(unsigned at=1;at<=recover_allocations;++at) {
    if(at%4!=shard)continue;
    damage();const auto lost=d.failed_io_latency_observations();
    allocations=0;allocation_fault=at;count_allocations=true;
    const auto result=recover();count_allocations=false;allocation_fault=0;
    Check(allocations>=at,"indexed recovery allocation site is actually exercised");
    if(result.ok())Check(d.failed_io_latency_observations()==lost+1&&Read(d,0,original.size())==original,
        "only recorded observation loss permits a fully reconciled recovery success");
    else {
      if(result.error!=db::NativeCreationRecoveryError::resource_exhausted)
        std::cerr<<"recovery allocation site="<<at<<" error="<<static_cast<unsigned>(result.error)<<'\n';
      Check(!result.receipt&&result.error==db::NativeCreationRecoveryError::resource_exhausted,
          "every required recovery allocation failure returns no completion receipt");
    }
    const auto after=Read(d,0,original.size());const auto begin=(selector_page+1)*size,end=begin+size;
    Check(std::equal(after.begin(),after.begin()+begin,original.begin())&&
        std::equal(after.begin()+end,after.end(),original.begin()+end),"allocation refusal preserves every dependency image");
    Check(recover().ok()&&Read(d,0,original.size())==original,"allocation-fault recovery retry is exact and idempotent");
  }
  Check(d.Close().ok(),"close indexed fault owner");
  std::cout<<"indexed faults shard="<<shard<<" create_allocations="<<create_allocations
      <<" recovery_allocations="<<recover_allocations<<" first_allocation="<<first_allocation<<'\n';
}
} // namespace
