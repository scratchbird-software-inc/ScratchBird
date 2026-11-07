// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
// Included after the real-file fixture and indexed seed builders.
namespace {
void NativeIndexedCreationFailureChecks(unsigned shard,unsigned first_allocation=1) {
  Check(shard<4,"indexed fault shard");
  Check(first_allocation>=1,"positive diagnostic allocation range");
  const auto request=Request(0,128);const auto seed=DenseIndexedCatalogSeed(request,47);
  const auto indexes=IndexedConstructionBindings();const u64 size=request.bootstrap.page_size_bytes,budget=256*size;
  page_bytes=size;total_pages=request.total_pages;
  u64 coverage=(size-384)/128;
  while(((384+(coverage+1)/2+7)&~u64{7})+128*coverage>size)--coverage;
  
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
          db::NativeCatalogSource::PageZero(result.receipt->filespace_uuid),2,1,{indexes[0]->relation_uuid,indexes[0]->dependencies},budget);
      Check(read.ok()&&read.rows.size()==47,"telemetry loss cannot hide incomplete construction");
    }else {
      if(result.error!=db::NativeCreationWorkspaceError::resource_exhausted)
        std::cerr<<"creation allocation site="<<at<<" error="<<static_cast<unsigned>(result.error)
            <<" checkpoint="<<static_cast<unsigned>(result.checkpoint_error)<<'\n';
      Check(!result.receipt&&result.error==db::NativeCreationWorkspaceError::resource_exhausted,
          "required construction allocation failure is a typed refusal, never a partial receipt");
    }
  }
}
} // namespace
