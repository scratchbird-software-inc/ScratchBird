// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
// Reuse independent native page/inventory byte oracles, not a mock catalog.
#define main UnusedNativeFilespaceRegressionMain
#include "../sbsql_sblr_alignment/filespace_page_zero_test.cpp"
#undef main
#include "management/runtime_authority_binding.hpp"

namespace {
namespace runtime_api=scratchbird::engine::internal_api;
using BindingError=runtime_api::RuntimeAuthorityBindingReadError;
template<class T> concept HasSnapshotIdentity=requires(T value){value.snapshot_uuid;};
static_assert(!HasSnapshotIdentity<runtime_api::RuntimeAuthorityBindingCommittedReadResult>);

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
