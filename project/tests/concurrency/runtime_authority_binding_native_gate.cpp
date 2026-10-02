// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
// Reuse independent native page/inventory byte oracles, not a mock catalog.
#define main UnusedNativeFilespaceRegressionMain
#include "../sbsql_sblr_alignment/filespace_page_zero_test.cpp"
#undef main
#include "management/runtime_authority_binding.hpp"
#include "../../src/server/database_ownership.hpp"
#include <future>
#include <latch>

namespace {
namespace runtime_api=scratchbird::engine::internal_api;
using BindingError=runtime_api::RuntimeAuthorityBindingReadError;
template<class T> concept HasSnapshotIdentity=requires(T value){value.snapshot_uuid;};
static_assert(!HasSnapshotIdentity<runtime_api::RuntimeAuthorityBindingCommittedReadResult>);
static_assert(!HasSnapshotIdentity<runtime_api::RuntimeAuthorityBindingOwnedReadResult>);
static_assert(!std::is_copy_constructible_v<runtime_api::RuntimeAuthorityBindingOwnedReadResult>);

void VerifyCommittedBinding(const runtime_api::RuntimeAuthorityBindingCommittedReadResult& result) {
  if(!result.ok())std::cerr<<"committed binding error="<<static_cast<unsigned>(result.error)
      <<" source="<<static_cast<unsigned>(result.source_error)<<'\n';
  Check(result.ok()&&result.binding->binding_uuid==Id(180)&&result.binding->database_uuid==Id(1)&&
      result.binding->service_principal_uuid==Id(200)&&result.binding->credential_reference_uuid==Id(203)&&
      result.binding->provider_uuid==Id(201)&&result.binding->policy_uuid==Id(202)&&
      result.binding->generation==1&&result.binding->policy_epoch==3&&result.binding->provider_generation==5&&
      result.native_version_uuid==Id(170),"committed source preserves exact binary definition and version");
}
void EmptyCommittedBinding(const runtime_api::RuntimeAuthorityBindingCommittedReadResult& result,BindingError error) {
  Check(result.error==error&&!result.binding&&result.native_version_uuid.is_nil(),
        "committed source failure exposes no usable binding/version");
}

void BindRuntimeDefinition(db::NativeCatalogLeafPage& leaf) {
  for(auto& row:leaf.body.rows) {
    auto decoded=catalog::DecodeCatalogMetadataVersion(row.cells[0].value.payload);
    Check(decoded.ok(),"read common native metadata fixture"); auto& m=decoded.record;
    catalog::CatalogRuntimeAuthorityBinding binding;
    binding.binding_uuid=m.record.header.object_uuid.value; binding.database_uuid=Id(1);
    binding.service_principal_uuid=Id(200); binding.security_authority_uuid=Id(1);
    binding.provider_uuid=Id(201); binding.policy_uuid=Id(202); binding.credential_reference_uuid=Id(203);
    binding.generation=1; binding.authority_mode=catalog::RuntimeAuthorityMode::database_local;
    binding.security_epoch=m.security_epoch; binding.policy_epoch=3; binding.provider_generation=5;
    binding.catalog_generation=m.catalog_generation; binding.origin_transaction_uuid=m.creator_transaction_uuid;
    binding.origin_local_transaction_id=m.creator_local_transaction_id;
    const auto encoded=catalog::EncodeCatalogRuntimeAuthorityBinding(binding); Check(encoded.ok(),"encode typed native runtime binding");
    m.record.header.kind=catalog::CatalogRecordKind::config_profile;
    m.record.header.parent_uuid.kind=platform::UuidKind::object;
    m.object_subtype="agent_runtime_authority"; m.security_policy_uuid={platform::UuidKind::object,binding.policy_uuid};
    m.record.payload.assign(encoded.bytes.begin(),encoded.bytes.end());
    const auto outer=catalog::EncodeCatalogMetadataVersion(m); Check(outer.ok(),"bind configuration to native metadata");
    row.cells[0].value.payload=outer.bytes;
  }
}
auto ReadRuntimeBinding(disk::FileDevice& device,unsigned profile,const mga::LocalTransactionInventory& inventory,
                        u64 reader=18,u64 generation=1,byte identity=180) {
  CatalogTestPin pin(inventory,reader);
  const auto found=std::find_if(inventory.entries.begin(),inventory.entries.end(),
      [&](const auto& entry){return entry.identity.local_id.value==reader;});
  Check(found!=inventory.entries.end(),"actual inventory reader exists");
  return runtime_api::ReadRuntimeAuthorityBindingFromOpenDevices(Id(1),{{Id(2),Profile(profile),&device}},
      CheckpointRef(CheckpointExample(profile)),2,1,{Id(101),{}},found->identity,pin.pin,Id(identity),generation,8*u64{sizes[profile]});
}

// Add independent selector/directory/allocation bytes to the existing actual
// catalog fixture. No production encoder manufactures the expected selection.
void InstallOwnedSelection(disk::FileDevice& device,unsigned p,disk::FilespacePageZero zero,
    db::NativeCheckpointRoot checkpoint,const page::NativeTransactionInventoryPage& inventory) {
  zero.free_pages=zero.preallocated_pages=0;
  zero.roots.push_back({18,0x30e,Id(2),31,1,Profile(p),Id(154)});
  zero.roots.push_back({19,0x30e,Id(2),32,1,Profile(p),Id(154)});
  page::NativeAllocationMap map;
  map.header={sizes[p],3,Id(1),Id(2),Id(164),13,103,0,Profile(p)};
  map.object_uuid=Id(43);map.map_generation=map.capacity_generation=1;
  map.total_pages=zero.total_pages;map.creator_transaction_uuid=Id(98);map.creator_local_transaction_id=17;
  map.states.assign(map.total_pages,page::NativeAllocationState::quarantined);
  const auto add=[&](const disk::NativeCommonPageHeader& h,const Uuid& object) {
    map.states[h.page_number]=page::NativeAllocationState::allocated;
    map.records.push_back({h.page_number,Id(static_cast<byte>(210+h.page_number)),h.page_uuid,
        object,Id(98),17,h.page_generation,0,h.page_type,{}});
  };
  add({sizes[p],1,Id(1),Id(2),zero.page_uuid,0,zero.page_generation,0,Profile(p)},Id(2));
  add(map.header,map.object_uuid);add(inventory.header,inventory.object_uuid);
  add(checkpoint.header,checkpoint.object_uuid);
  add({sizes[p],0x30e,Id(1),Id(2),Id(155),31,1,0,Profile(p)},Id(154));
  add({sizes[p],0x30e,Id(1),Id(2),Id(156),32,1,0,Profile(p)},Id(154));
  std::sort(map.records.begin(),map.records.end(),[](const auto& a,const auto& b){return a.page_number<b.page_number;});
  page::NativeFilespaceDirectory directory;
  directory.header={sizes[p],9,Id(1),Id(2),Id(165),15,105,0,Profile(p)};
  directory.object_uuid=Id(45);directory.directory_generation=1;
  directory.creator_transaction_uuid=Id(98);directory.creator_local_transaction_id=17;directory.total_records=1;
  directory.records.push_back({zero.bootstrap,Id(166),zero.page_uuid,zero.page_generation,
      zero.root_set_generation,zero.total_pages,0,{}});
  const auto mb=AllocationOracle(map),dbb=DirectoryOracle(directory);
  checkpoint.roots[3]={4,3,{Id(2),13,103,Profile(p)},Id(43),WholeRootHash(mb)};
  checkpoint.roots[2]={3,9,{Id(2),15,105,Profile(p)},Id(45),WholeRootHash(dbb)};
  const auto cp=CheckpointOracle(checkpoint);
  db::NativeCheckpointSelection selection;
  selection.header={sizes[p],0x30e,Id(1),Id(2),Id(155),31,1,0,Profile(p)};
  selection.object_uuid=Id(154);selection.bootstrap_uuid=zero.page_uuid;selection.publication_uuid=Id(153);
  selection.selection_generation=1;selection.checkpoint={Id(2),19,109,Profile(p)};
  selection.checkpoint_object_uuid=checkpoint.object_uuid;selection.checkpoint_sha256=WholeRootHash(cp);
  selection.checkpoint_generation=checkpoint.checkpoint_generation;
  selection.root_set_generation=checkpoint.root_set_generation;selection.timeline_uuid=checkpoint.timeline_uuid;
  const auto put=[&](u64 number,const Bytes& b) {
    const auto io=device.WriteAt(number*sizes[p],b.data(),b.size());
    Check(io.ok()&&io.bytes_transferred==b.size(),"persist independent owned configuration source");
  };
  put(0,Oracle(zero));put(13,mb);put(15,dbb);put(19,cp);put(31,SelectionOracle(selection));
  selection.header.page_number=32;selection.header.page_uuid=Id(156);put(32,SelectionOracle(selection));
  Check(device.Sync().ok(),"durable actual selection fixture");
}

void OwnedRuntimeBinding(const std::string& path,unsigned p) {
  namespace server=scratchbird::server;
  using Source=db::NativeOwnedCheckpointSource;
  const u64 budget=64*u64{sizes[p]};
  server::DatabaseOwnershipRequest request;request.database_path=path;
  auto route=server::AcquireDatabaseOwnership(request);Check(route.acquired&&route.lock,"actual configuration route owner");
  auto transition=route.lock->BeginNativeSourceTransition();Check(transition.ok(),"actual source transition issuer");
  std::vector<std::unique_ptr<disk::FileDevice>> secondary;
  auto source=Source::AdoptRoute(transition.transition,Id(1),Id(2),secondary,budget);
  if(!source.ok())std::cerr<<"owned fixture error="<<int(source.error)<<" selection="<<int(source.selection_error)
      <<" checkpoint="<<int(source.checkpoint_error)<<" directory="<<int(source.directory_error)<<'\n';
  Check(source.ok(),"actual complete route-owned configuration source");
  const auto read=[&](u64 generation=1,byte binding=180,u64 ceiling=0) {
    return runtime_api::ReadCommittedRuntimeAuthorityBindingFromOwnedSource(source.owner,Id(1),2,1,
        {Id(101),{}},Id(binding),generation,ceiling?ceiling:budget);
  };
  const auto empty=[&](const auto& result,BindingError error) {
    EmptyCommittedBinding(result,error);Check(!result.source_lease,"failed owned read releases all source guards");
  };
  const auto writes_before=stage_writes,syncs_before=stage_syncs;
  {auto result=read();VerifyCommittedBinding(result);Check(result.ok()&&result.source_lease,
      "successful configuration observation retains actual current source");}
  empty(read(2),BindingError::generation_mismatch);empty(read(1,250),BindingError::absent);
  empty(read(1,180,1),BindingError::source_failure);
  {const auto bad_relation=runtime_api::ReadCommittedRuntimeAuthorityBindingFromOwnedSource(source.owner,Id(1),2,1,
      {Id(250),{}},Id(180),1,budget);
    empty(bad_relation,BindingError::source_failure);
    Check(bad_relation.source_error!=db::NativeCommittedCatalogReadError::none,
        "owned binding preserves nested committed-catalog refusal");}
  reads=0;track_reads=true;
  empty(runtime_api::ReadCommittedRuntimeAuthorityBindingFromOwnedSource(source.owner,Id(250),2,1,
      {Id(101),{}},Id(180),1,budget),BindingError::invalid_request);
  empty(read(0),BindingError::invalid_request);track_reads=false;Check(!reads,"invalid owned binding refuses before I/O");
  if(p==0) {
    for(unsigned invalid=0;invalid<4;++invalid) {
      std::shared_ptr<const Source> owner=source.owner;auto database=Id(1),binding=Id(180);u64 ceiling=budget;
      if(invalid==0)owner.reset();if(invalid==1)database={};if(invalid==2)binding={};if(invalid==3)ceiling=0;
      reads=0;track_reads=true;
      const auto failed=runtime_api::ReadCommittedRuntimeAuthorityBindingFromOwnedSource(owner,database,2,1,
          {Id(101),{}},binding,1,ceiling);
      track_reads=false;empty(failed,BindingError::invalid_request);Check(!reads,"invalid owner input rejected before I/O");
    }
    reads=observed_full_digests=0;observed_allocations=0;
    track_reads=count_full_digests=count_allocations=true;auto measured=read();
    track_reads=count_full_digests=count_allocations=false;const auto count=reads;
    const auto allocations=observed_allocations;const auto digests=observed_full_digests;
    VerifyCommittedBinding(measured);Check(count>0,"owned binding uses real source I/O");
    Check(allocations>0&&digests>0,"measure actual owned binding allocations and hashes");
    const auto retained=measured.source_lease->source().retained_image_bytes();
    const auto* device=measured.source_lease->devices().front().device;
    u64 combined=0;
    {const auto catalog=db::ReadNativeCommittedCatalogVersionsFromOpenDevices(Id(1),measured.source_lease->devices(),
        measured.source_lease->source().checkpoint(),2,1,{Id(101),{}},budget);
      Check(catalog.ok(),"independent catalog image cost");combined=retained+catalog.source.retained_image_bytes;}
    measured.source_lease.reset();
    empty(read(1,180,retained),BindingError::resource_exhausted);
    {auto exact=read(1,180,combined);VerifyCommittedBinding(exact);Check(exact.ok(),"combined source/catalog images fit exactly");}
    empty(read(1,180,combined-1),BindingError::source_failure);
    for(unsigned fault=1;fault<=count;++fault) {
      reads=0;read_fault=fault;track_reads=true;auto failed=read();track_reads=false;
      Check(!read_fault,"each owned configuration physical read failure consumed");
      empty(failed,BindingError::source_failure);
    }
    for(unsigned fault=1;fault<=digests;++fault) {
      full_digest_fault=fault;auto failed=read();
      Check(!full_digest_fault,"each owned configuration digest failure consumed");
      empty(failed,BindingError::source_failure);
    }
    for(unsigned long fault=0;fault<allocations;++fault) {
      const auto loss=device->failed_io_latency_observations();
      allocation_budget=static_cast<long>(fault);auto failed=read();const bool consumed=allocation_budget==-1;allocation_budget=-1;
      Check(consumed,"owned configuration allocation fault consumed");
      if(failed.ok()) {
        Check(device->failed_io_latency_observations()==loss+1,
            "only independently recorded telemetry loss permits owned-read success after allocation failure");
        VerifyCommittedBinding(failed);continue;
      }
      Check(!failed.ok()&&!failed.binding&&failed.native_version_uuid.is_nil()&&!failed.source_lease,
          "owned configuration allocation failure emits no usable result");
      Check(failed.error==BindingError::source_failure||failed.error==BindingError::resource_exhausted,
          "owned allocation refusal retains typed error");
    }
    std::cout<<"owned binding fault sweep reads="<<count<<" digests="<<digests<<" allocations="<<allocations<<'\n';
    const auto child=fork();Check(child>=0,"fork inherited owned configuration reader");
    if(child==0){auto inherited=read();_exit(!inherited.ok()&&!inherited.binding&&!inherited.source_lease&&
        inherited.owned_source.error==db::NativeOwnedSourceError::wrong_process?0:9);}
    int status=0;Check(waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==0,
        "inherited configuration source refuses before device access");
  }
  // Read guards are acquired and released on the worker. The main thread
  // withdraws new admission but cannot destroy the worker's retained source.
  std::latch ready(1),finish(1);
  std::weak_ptr<Source> weak=source.owner;
  auto worker=std::async(std::launch::async,[owner=source.owner,&ready,&finish,budget]() mutable {
    auto held=runtime_api::ReadCommittedRuntimeAuthorityBindingFromOwnedSource(owner,Id(1),2,1,
        {Id(101),{}},Id(180),1,budget);
    owner.reset();ready.count_down();finish.wait();
    return held.ok()&&held.binding->service_principal_uuid==Id(200)&&held.source_lease!=nullptr;
  });
  // A failed main-thread oracle must still release the worker before future
  // destruction, including negative-control runs that remove the retained pin.
  std::unique_ptr<std::latch,void(*)(std::latch*)> release(&finish,[](auto* latch){latch->count_down();});
  ready.wait();source.owner->Withdraw();
  reads=0;track_reads=true;auto withdrawn=read();track_reads=false;empty(withdrawn,BindingError::source_failure);
  Check(!reads,"withdrawn configuration source performs no new I/O");
  Check(withdrawn.owned_source.error==db::NativeOwnedSourceError::withdrawn,"typed owner withdrawal preserved");
  route.lock->release();route.lock.reset();transition.transition.reset();source.owner.reset();
  Check(!weak.expired(),"worker configuration read retains actual owner after runtime release");
  {disk::FileDevice competing;Check(!competing.Open(path,disk::FileOpenMode::open_existing_read_only).ok(),
      "physical ownership survives withdrawal until worker drains");}
  release.reset();Check(worker.get()&&weak.expired(),"worker releases source guards before final owner destruction");
  Check(stage_writes==writes_before&&stage_syncs==syncs_before,"owned configuration observation is physically read-only");
  disk::FileDevice reopened;Check(reopened.Open(path,disk::FileOpenMode::open_existing_read_only).ok(),
      "independent reopen only after worker source release");
  VerifyCommittedBinding(runtime_api::ReadCommittedRuntimeAuthorityBindingFromOpenDevices(Id(1),{{Id(2),Profile(p),&reopened}},
      CheckpointRef(CheckpointExample(p)),2,1,{Id(101),{}},Id(180),1,budget));
}
void RuntimeBindingFiles() {
  Fixture fixture;
  for(unsigned profile=0;profile<5;++profile) {
    disk::FileDevice device; const auto path=(fixture.root/("runtime-binding-"+std::to_string(profile))).string();
    Check(device.Open(path,disk::FileOpenMode::create_new).ok(),"own native binding fixture");
    const auto zero=Example(profile,1); auto leaf=LeafExample(profile); BindRuntimeDefinition(leaf);
    auto inventory=InventoryExample(profile); inventory.inventory.entries.clear();
    inventory.inventory.next_local_transaction_id=19; inventory.inventory.next_commit_sequence=3;
    for(unsigned i=0;i<3;++i) {
      auto entry=InventoryExample(profile).inventory.entries[0];
      entry.identity.local_id=mga::MakeLocalTransactionId(i==0?13:i==1?17:18);
      entry.identity.transaction_uuid.value=Id(i==0?162:i==1?98:204);
      entry.state=i<2?mga::TransactionState::committed:mga::TransactionState::active;
      entry.commit_sequence=i<2?i+1:0; inventory.inventory.entries.push_back(entry);
    }
    auto root=RootExample(profile); root.creator_transaction_uuid=Id(98);
    auto checkpoint=CheckpointExample(profile); checkpoint.selected_local_transaction_id=18;
    const auto put=[&](u64 number,const auto& bytes){const auto io=device.WriteAt(number*sizes[profile],bytes.data(),bytes.size());
      Check(io.ok()&&io.bytes_transferred==bytes.size(),"persist exact native fixture image");};
    const auto persist=[&] {
      const auto ib=InventoryStateOracle(inventory),rb=RootOracle(root);
      checkpoint.roots[0].page=InventoryRef(inventory); checkpoint.roots[0].object_uuid=inventory.object_uuid;
      checkpoint.roots[0].sha256=WholeRootHash(ib);
      checkpoint.roots[4].page={Id(2),12,102,Profile(profile)}; checkpoint.roots[4].object_uuid=root.object_uuid;
      checkpoint.roots[4].sha256=WholeRootHash(rb); checkpoint.roots[8]=checkpoint.roots[4]; checkpoint.roots[8].role=9;
      put(0,Oracle(zero)); put(12,rb); put(14,ib); put(19,CheckpointOracle(checkpoint)); put(21,LeafOracle(leaf));
      const byte padding=0; Check(device.WriteAt(zero.total_pages*sizes[profile]-1,&padding,1).ok()&&device.Sync().ok(),"sync native fixture");
    };
    persist();
    const std::vector<disk::NativeFilespaceDevice> committed_devices{{Id(2),Profile(profile),&device}};
    const auto read_committed=[&](u64 generation=1,byte identity=180,u64 budget=0) {
      return runtime_api::ReadCommittedRuntimeAuthorityBindingFromOpenDevices(Id(1),committed_devices,
          CheckpointRef(checkpoint),2,1,{Id(101),{}},Id(identity),generation,budget?budget:8*u64{sizes[profile]});
    };
    stage_writes=stage_syncs=0;
    VerifyCommittedBinding(read_committed());
    EmptyCommittedBinding(read_committed(2),BindingError::generation_mismatch);
    EmptyCommittedBinding(read_committed(1,250),BindingError::absent);
    Check(!stage_writes&&!stage_syncs,"committed configuration observation performs no writes or syncs");
    const auto verify=[&](const auto& result) {
      if(!result.ok())std::cerr<<"read error="<<static_cast<unsigned>(result.error)<<" source="<<static_cast<unsigned>(result.source_error)<<'\n';
      Check(result.ok()&&result.binding->binding_uuid==Id(180)&&result.binding->database_uuid==Id(1)&&
          result.binding->service_principal_uuid==Id(200)&&result.binding->credential_reference_uuid==Id(203)&&
          result.native_version_uuid==Id(170)&&!result.snapshot_uuid.is_nil(),"actual committed binary binding and exact native version selected");
    };
    verify(ReadRuntimeBinding(device,profile,inventory.inventory));
    const auto empty=[&](const auto& result,BindingError error) {
      Check(result.error==error&&!result.binding&&result.native_version_uuid.is_nil()&&result.snapshot_uuid.is_nil(),
          "refusal returns no usable binding/version/snapshot");
    };
    empty(ReadRuntimeBinding(device,profile,inventory.inventory,18,2),BindingError::generation_mismatch);
    empty(ReadRuntimeBinding(device,profile,inventory.inventory,18,1,250),BindingError::absent);
    auto saved=inventory;
    inventory.inventory.entries[0].state=mga::TransactionState::active; inventory.inventory.entries[0].commit_sequence=0;
    persist(); empty(ReadRuntimeBinding(device,profile,inventory.inventory),BindingError::absent);
    EmptyCommittedBinding(read_committed(),BindingError::absent);
    empty(ReadRuntimeBinding(device,profile,inventory.inventory,13),BindingError::not_committed_active);
    inventory.inventory.entries[0].state=mga::TransactionState::rolled_back; persist();
    empty(ReadRuntimeBinding(device,profile,inventory.inventory),BindingError::absent);
    EmptyCommittedBinding(read_committed(),BindingError::absent);
    inventory=saved; persist();
    const auto original_leaf=leaf;
    auto changed=catalog::DecodeCatalogMetadataVersion(leaf.body.rows[0].cells[0].value.payload);
    Check(changed.ok(),"decode real row for wrong-database negative");
    auto definition=catalog::DecodeCatalogRuntimeAuthorityBinding(changed.record.record.payload);
    Check(definition.ok(),"decode typed binding for negative");
    definition.record->database_uuid=definition.record->security_authority_uuid=Id(250);
    const auto foreign=catalog::EncodeCatalogRuntimeAuthorityBinding(*definition.record);
    Check(foreign.ok(),"well-formed foreign definition");
    changed.record.record.payload.assign(foreign.bytes.begin(),foreign.bytes.end());
    const auto outer=catalog::EncodeCatalogMetadataVersion(changed.record); Check(outer.ok(),"well-formed foreign metadata");
    leaf.body.rows[0].cells[0].value.payload=outer.bytes; persist();
    empty(ReadRuntimeBinding(device,profile,inventory.inventory),BindingError::invalid_definition);
    EmptyCommittedBinding(read_committed(),BindingError::invalid_definition);
    leaf=original_leaf; persist();
    // A committed source requires no active reader. Do not synthesize or begin
    // a transaction just to load the configuration used to admit that BEGIN.
    inventory.inventory.entries[2].state=mga::TransactionState::committed;
    inventory.inventory.entries[2].commit_sequence=3;inventory.inventory.next_commit_sequence=4;
    persist();VerifyCommittedBinding(read_committed());
    inventory=saved;persist();
    // Navigation ownership is independent of the selected binding row.
    inventory.inventory.entries[1].state=mga::TransactionState::active;
    inventory.inventory.entries[1].commit_sequence=0;persist();
    const auto uncommitted_navigation=read_committed();
    EmptyCommittedBinding(uncommitted_navigation,BindingError::source_failure);
    // This fixture uses a direct catalog root, not an index navigation page.
    // Checkpoint selection rejects its uncommitted creator before row selection.
    Check(uncommitted_navigation.source_error==db::NativeCommittedCatalogReadError::source_failure,
          "committed binding retains checkpoint source refusal");
    inventory=saved;persist();
    if(profile==0) {
      for(unsigned invalid=0;invalid<5;++invalid) {
        auto database=Id(1),binding=Id(180);u64 generation=1,budget=8*u64{sizes[0]};
        if(invalid==0)database={};if(invalid==1)binding={};if(invalid==2)generation=0;
        if(invalid==3)budget=0;if(invalid==4)binding.bytes[6]=0x40;
        reads=0;track_reads=true;
        const auto bad=runtime_api::ReadCommittedRuntimeAuthorityBindingFromOpenDevices(database,{{Id(2),Profile(0),&device}},
            CheckpointRef(checkpoint),2,1,{Id(101),{}},binding,generation,budget);
        track_reads=false;EmptyCommittedBinding(bad,BindingError::invalid_request);
        Check(!reads,"invalid committed request performs no native reads");
      }
      EmptyCommittedBinding(read_committed(1,180,1),BindingError::source_failure);
      reads=0;track_reads=true;auto committed=read_committed();track_reads=false;const auto committed_reads=reads;
      VerifyCommittedBinding(committed);Check(committed_reads>0,"committed source actually reads files");
      for(unsigned fault=1;fault<=committed_reads;++fault) {
        reads=0;read_fault=fault;track_reads=true;committed=read_committed();track_reads=false;
        Check(read_fault==0,"committed source read fault consumed");EmptyCommittedBinding(committed,BindingError::source_failure);
      }
      for(long fault:{0l,1l,5l,20l,50l}) {
        const auto loss=device.failed_io_latency_observations();
        allocation_budget=fault;committed=read_committed();const bool consumed=allocation_budget==-1;allocation_budget=-1;
        Check(consumed,"committed source selected allocation fault consumed");
        if(committed.ok()) {
          Check(device.failed_io_latency_observations()==loss+1,"only recorded telemetry loss preserves committed source success");
          VerifyCommittedBinding(committed);
        }else {
          Check(!committed.binding&&committed.native_version_uuid.is_nil()&&
              (committed.error==BindingError::source_failure||committed.error==BindingError::resource_exhausted),
              "committed allocation failure retains no usable result");
        }
      }
      VerifyCommittedBinding(read_committed());
      CatalogTestPin pin(inventory.inventory,18);
      const std::vector<disk::NativeFilespaceDevice> devices{{Id(2),Profile(profile),&device}};
      const auto read=[&](u64 budget=8*u64{sizes[0]}) {
        return runtime_api::ReadRuntimeAuthorityBindingFromOpenDevices(Id(1),devices,
            CheckpointRef(checkpoint),2,1,{Id(101),{}},inventory.inventory.entries[2].identity,pin.pin,Id(180),1,budget);
      };
      empty(read(1),BindingError::source_failure);
      reads=0;track_reads=true;auto result=read();track_reads=false;const auto read_count=reads;
      verify(result);Check(read_count>0,"measure actual source reads");
      for(unsigned fault=1;fault<=read_count;++fault) {
        reads=0;read_fault=fault;track_reads=true;result=read();track_reads=false;
        Check(read_fault==0,"native read fault consumed");empty(result,BindingError::source_failure);
      }
      for(long fault:{0l,1l,5l,20l,50l}) {
        const auto failed_metrics=device.failed_io_latency_observations();
        allocation_budget=fault;result=read();const bool consumed=allocation_budget==-1;allocation_budget=-1;
        Check(consumed,"selected allocation fault consumed");
        if(result.ok()) {
          // FileDevice intentionally contains a latency-recorder exception.
          // Only this independently observed failure can preserve a good read.
          Check(device.failed_io_latency_observations()==failed_metrics+1,
              "successful read after allocation fault requires actual isolated metric failure");
          verify(result);continue;
        }
        Check(!result.ok()&&!result.binding&&result.native_version_uuid.is_nil()&&result.snapshot_uuid.is_nil(),
            "allocation failure retains no usable binding");
        Check(result.error==BindingError::source_failure||result.error==BindingError::resource_exhausted,"typed allocation/source refusal");
      }
      verify(read());
    }
    CatalogTestPin revoked(inventory.inventory,18);
    mga::RevokePublishedSnapshotVector(revoked.published.descriptor.snapshot_uuid);
    empty(runtime_api::ReadRuntimeAuthorityBindingFromOpenDevices(Id(1),{{Id(2),Profile(profile),&device}},
        CheckpointRef(checkpoint),2,1,{Id(101),{}},inventory.inventory.entries[2].identity,revoked.pin,Id(180),1,8*u64{sizes[profile]}),BindingError::source_failure);
    // A new process must read the persisted catalog and inventory; it cannot use
    // a retained source result or a process-local configuration registry.
    Check(device.Close().ok(),"close before independent reopen");
    const auto child=fork(); Check(child>=0,"spawn cold binding reader");
    const auto argument=std::to_string(profile);
    if(child==0){execl("/proc/self/exe","runtime-binding-native","--read-binding",path.c_str(),argument.c_str(),nullptr);_exit(125);}
    int status=0; Check(waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==0,"fresh executable reads actual committed binding");
    Check(device.Open(path,disk::FileOpenMode::open_existing).ok(),"own fixture before selector installation");
    InstallOwnedSelection(device,profile,zero,checkpoint,inventory);
    Check(device.Close().ok(),"release fixture writer before actual route admission");
    OwnedRuntimeBinding(path,profile);
  }
}
} // namespace

int main(int argc,char** argv) {
  try {
    if(argc==4&&std::string_view(argv[1])=="--read-binding") {
      const auto profile=static_cast<unsigned>(std::stoul(argv[3])); if(profile>=5)return 2;
      disk::FileDevice device; if(!device.Open(argv[2],disk::FileOpenMode::open_existing_read_only).ok())return 3;
      const auto source=db::VerifyNativeCheckpointInventoryFromOpenDevices(Id(1),{{Id(2),Profile(profile),&device}},
          CheckpointRef(CheckpointExample(profile)),8*u64{sizes[profile]});
      if(!source.ok())return 4;
      const auto read=ReadRuntimeBinding(device,profile,source.inventory);
      if(!read.ok()||read.binding->service_principal_uuid!=Id(200)||read.native_version_uuid!=Id(170))return 5;
      const auto committed=runtime_api::ReadCommittedRuntimeAuthorityBindingFromOpenDevices(Id(1),{{Id(2),Profile(profile),&device}},
          CheckpointRef(CheckpointExample(profile)),2,1,{Id(101),{}},Id(180),1,8*u64{sizes[profile]});
      VerifyCommittedBinding(committed);return 0;
    }
    RuntimeBindingFiles(); std::cout<<"native runtime binding checks="<<checks<<" failures=0\n"; return 0;
  }catch(const std::exception& error){allocation_budget=-1;std::cerr<<"FAIL "<<error.what()<<" checks="<<checks<<'\n';return 1;}
}
