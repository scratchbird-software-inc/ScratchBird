// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
// Retain the qualified real-file/independent-byte and native I/O fault fixture.
#define SB_NATIVE_STORAGE_FIXTURE_WITHOUT_MAIN
#include "filespace_page_zero_test.cpp"
#undef SB_NATIVE_STORAGE_FIXTURE_WITHOUT_MAIN
#include "security/native_runtime_security_observation.hpp"
#include "native_creation_workspace.hpp"
#include "bootstrap_policy_fixture.hpp"
#include "../support/owned_temp_directory.hpp"

namespace {
namespace runtime_security=scratchbird::engine::internal_api;
namespace security_fixture=scratchbird::tests::bootstrap_policy;
using SecurityError=runtime_security::NativeRuntimeSecurityReadError;
using SecurityResult=runtime_security::NativeRuntimeSecurityObservationResult;
const platform::TypedUuid security_tx{platform::UuidKind::transaction,Id(5)};

catalog::CatalogSecurityRecord SecurityRecord(catalog::CatalogRecordKind kind,unsigned id) {
  catalog::CatalogSecurityRecord r;r.kind=kind;
  r.identities.emplace(std::string(catalog::CatalogSecurityPrimaryIdentityName(kind)),Id(id));
  r.identities.emplace("origin_transaction_uuid",security_tx.value);
  r.attributes={{"creator_tx",u64{1}},{"security_context_authority_version",u64{1}},
      {"security_context_generation",u64{9}},{"security_generation",u64{11}},
      {"policy_generation",u64{13}},{"active",true}};
  return r;
}
db::NativeCreationCatalogRecord SecurityMetadata(catalog::CatalogSecurityRecord record,unsigned ordinal) {
  auto metadata=security_fixture::Metadata(0,security_tx,Id(60),{});
  metadata.record.header.kind=record.kind;
  metadata.record.header.object_uuid={platform::UuidKind::object,record.Identity(catalog::CatalogSecurityPrimaryIdentityName(record.kind))};
  metadata.record.header.row_uuid={platform::UuidKind::row,security_fixture::Id(3000+ordinal)};
  metadata.object_subtype=catalog::CatalogRecordKindName(record.kind);
  const auto encoded=catalog::EncodeCatalogSecurityRecord(record);Check(encoded.ok(),"canonical native security payload");
  metadata.record.payload.assign(encoded.bytes.begin(),encoded.bytes.end());
  return {metadata,{}};
}
struct SecurityFixture {
  scratchbird::tests::OwnedTempDirectory artifacts;
  disk::FileDevice device;
  unsigned profile;
  u64 budget;
  db::NativeCreationCatalogIndexes indexes;
  db::NativeCreationWorkspaceReceipt receipt;
  explicit SecurityFixture(unsigned p,unsigned variant=0,bool indexed=false):profile(p),budget(512*u64{sizes[p]}) {
    using K=catalog::CatalogRecordKind;db::NativeCreationCatalogSeed seed;
    auto principal=SecurityRecord(K::user_account,200);
    principal.attributes.emplace("kind","service");principal.attributes.emplace("principal_kind","service");
    principal.attributes.emplace("bootstrap_principal",false);
    principal.attributes.emplace("principal_name","native-service");
    principal.attributes.emplace("credential_fingerprint","structural-only-not-authentication");
    if(variant==1)principal.attributes["active"]=false;
    if(variant==2)principal.attributes["principal_kind"]="user";
    if(variant==3)principal.attributes["security_context_generation"]=u64{0};
    if(variant==8)for(const auto* name:{"security_context_generation","security_generation","policy_generation"})
      principal.attributes[name]=std::numeric_limits<u64>::max();
    if(variant==4){principal.kind=K::role_account;principal.identities.erase("principal_uuid");principal.identities.emplace("role_uuid",Id(200));}
    seed[3].push_back(SecurityMetadata(principal,0));
    auto grant=SecurityRecord(K::grant_record,201);
    grant.identities.emplace("subject_uuid",Id(200));grant.attributes.emplace("grant_class","privilege");
    grant.attributes.emplace("subject_kind","principal");grant.attributes.emplace("right","CONNECT");grant.attributes.emplace("effect","allow");
    seed[3].push_back(SecurityMetadata(grant,1));
    auto group=SecurityRecord(K::group_account,202);group.attributes.emplace("group_name","literal alias");
    seed[3].push_back(SecurityMetadata(group,2));
    auto membership=SecurityRecord(K::grant_record,203);
    membership.identities.emplace("member_uuid",Id(200));membership.identities.emplace("parent_uuid",Id(202));
    membership.identities.emplace("membership_uuid",Id(203));membership.attributes.emplace("grant_class","group_membership");
    membership.attributes.emplace("member_kind","principal");membership.attributes.emplace("parent_kind","group");
    seed[3].push_back(SecurityMetadata(membership,3));
    auto denial=SecurityRecord(K::grant_record,204);
    denial.identities.emplace("subject_uuid",Id(202));denial.identities.emplace("target_uuid",Id(240));
    denial.attributes.emplace("grant_class","privilege");denial.attributes.emplace("subject_kind","group");
    denial.attributes.emplace("right","SELECT");denial.attributes.emplace("effect","deny");
    seed[3].push_back(SecurityMetadata(denial,4));
    auto retired=SecurityRecord(K::user_account,205);retired.attributes["active"]=false;
    retired.attributes.emplace("kind","user");retired.attributes.emplace("principal_kind","user");
    auto retired_metadata=SecurityMetadata(retired,5);retired_metadata.metadata.lifecycle=catalog::CatalogObjectLifecycle::archived;
    retired_metadata.metadata.status=catalog::CatalogObjectStatus::retired;
    retired_metadata.metadata.retired_transaction_uuid=security_tx;
    seed[3].push_back(std::move(retired_metadata));
    catalog::CatalogRuntimeAuthorityBinding binding;
    binding.binding_uuid=Id(180);binding.database_uuid=binding.security_authority_uuid=Id(1);
    binding.service_principal_uuid=variant==5?Id(250):Id(200);
    binding.provider_uuid=Id(210);binding.policy_uuid=Id(52);binding.credential_reference_uuid=Id(211);
    binding.generation=binding.security_epoch=binding.catalog_generation=1;
    binding.policy_epoch=31;binding.provider_generation=37;
    binding.authority_mode=variant==6?catalog::RuntimeAuthorityMode::security_database:catalog::RuntimeAuthorityMode::database_local;
    binding.origin_transaction_uuid=security_tx;binding.origin_local_transaction_id=1;
    const auto encoded=catalog::EncodeCatalogRuntimeAuthorityBinding(binding);Check(encoded.ok(),"actual binary runtime binding");
    auto meta=security_fixture::Metadata(0,security_tx,Id(61),{});
    meta.record.header.kind=K::config_profile;meta.record.header.object_uuid={platform::UuidKind::object,Id(180)};
    meta.record.header.row_uuid={platform::UuidKind::row,security_fixture::Id(3100)};
    meta.security_policy_uuid={platform::UuidKind::object,binding.policy_uuid};
    meta.object_subtype="agent_runtime_authority";meta.record.payload.assign(encoded.bytes.begin(),encoded.bytes.end());
    if(variant==7){meta.lifecycle=catalog::CatalogObjectLifecycle::archived;meta.status=catalog::CatalogObjectStatus::retired;}
    seed[4].push_back({meta,{}});
    if(indexed)for(unsigned role:{3u,4u}){
      page::NativeBtreeDependencies deps;deps.index_uuid=security_fixture::Id(8000+role);
      deps.descriptor_generation=deps.storage_generation=1;deps.key_profile_uuid=security_fixture::Id(8100+role);
      deps.visibility_profile_uuid=security_fixture::Id(8200+role);deps.dependency_map_uuid=security_fixture::Id(8300+role);
      deps.dependency_map_sha256.fill(role+1);indexes[role]=db::NativeCreationCatalogIndexBinding{security_fixture::Id(8400+role),deps};
    }
    db::NativeFilespaceInitializationRequest request;
    request.bootstrap={Id(1),Id(2),Profile(p),disk::kNativeBootstrapIntegrityProfile,{},sizes[p],1,0,1,7};
    request.operation_uuid=Id(3);request.writer_uuid=Id(4);request.policy_snapshot_uuid=Id(10);
    request.creator.transaction_uuid=security_tx;request.creator.local_id=mga::MakeLocalTransactionId(1);
    request.creator.scope=mga::TransactionScope::local_node;request.total_pages=256;request.creation_utc_millis=1789357072000ULL;
    scratchbird::core::uuid::StandaloneUuidV7Issuer issuer({Id(1),Id(10)},{{},0,1000});
    for(const auto& relation:seed)for(const auto& row:relation){
      const auto invalid=catalog::ValidateCatalogMetadataVersionView(catalog::BorrowCatalogMetadataVersion(row.metadata));
      if(invalid)std::cerr<<"native security fixture metadata="<<invalid->detail<<" profile="<<p<<" variant="<<variant<<'\n';
      Check(!invalid,"complete actual family/common metadata before native security fixture construction");
    }
    const auto path=(artifacts.path()/"native-security").string();Check(device.Open(path,disk::FileOpenMode::create_new).ok(),"actual new native security filespace");
    const auto made=db::InitializeIndexedNativeCreationWorkspaceOnOpenDevice(device,request,seed,indexes,budget,issuer);
    if(!made.ok())std::cerr<<"native security fixture creation="<<int(made.error)<<" profile="<<p<<" variant="<<variant<<'\n';
    Check(made.ok(),"actual native security/configuration creation in their declared roles");receipt=*made.receipt;
    Check(device.Close().ok()&&device.Open(path,disk::FileOpenMode::open_existing).ok(),"independent physical native security reopen");
  }
  runtime_security::NativeRuntimeSecurityReadRequest Request()const {
    runtime_security::NativeRuntimeSecurityReadRequest r;
    r.database_uuid=Id(1);r.runtime_binding_uuid=Id(180);r.runtime_binding_generation=1;r.maximum_retained_image_bytes=budget;
    r.configuration_relation={receipt.relation_uuids[4],receipt.index_dependencies[4]};
    r.security_relation={receipt.relation_uuids[3],receipt.index_dependencies[3]};return r;
  }
  // Independent byte-fixture publication, never a production actuator or an
  // admission receipt. Build an actual on-disk inventory and retained chain so
  // the subject reader must use MGA, not a supplied visibility Boolean.
  void RetainedSuccessor(unsigned scenario,bool configuration_phase=false) {
    const unsigned role=configuration_phase?4:3;
    Check(!indexes[role],"MGA fixture uses the actual unindexed owning head");
    const std::vector<disk::NativeFilespaceDevice> devices{{Id(2),Profile(profile),&device}};
    const auto selected=db::ReadNativeBoundCheckpointSelectionFromOpenDevices(Id(1),devices,Id(2),budget);
    Check(selected.ok(),"MGA fixture actual original selection");
    auto begun=mga::BeginLocalTransaction(selected.checkpoint_inventory.inventory,
        {platform::UuidKind::transaction,Id(12)},1789357072001ULL);Check(begun.ok(),"actual MGA candidate transform");
    auto inventory=begun.inventory;
    if(scenario==2){const auto changed=mga::PrepareLocalTransaction(inventory,mga::MakeLocalTransactionId(2));
      Check(changed.ok(),"actual prepared candidate");inventory=changed.inventory;}
    if(scenario==3){const auto changed=mga::RollbackLocalTransaction(inventory,mga::MakeLocalTransactionId(2),1789357072002ULL);
      Check(changed.ok(),"actual rolled-back candidate");inventory=changed.inventory;}
    if(scenario==4||scenario==8){const auto changed=mga::CommitLocalTransaction(inventory,mga::MakeLocalTransactionId(2),1789357072002ULL);
      Check(changed.ok(),"actual committed candidate");inventory=changed.inventory;}
    if(scenario==5)inventory.entries.back().state=mga::TransactionState::limbo;
    const auto loaded=db::ReadNativeCatalogLeafFromOpenDevice(device,Id(1),receipt.relations[role]);
    Check(loaded.ok(),"actual stored owning base leaf before retained version fixture");auto leaf=*loaded.page;
    const auto original=std::find_if(leaf.body.rows.begin(),leaf.body.rows.end(),[&](const auto& row){
      return loaded.metadata.at(row.version_uuid).record.header.object_uuid.value==Id(configuration_phase?180:200);});
    Check(original!=leaf.body.rows.end(),"actual persisted selected object row");
    auto row=*original;auto metadata=loaded.metadata.at(row.version_uuid);
    row.version_uuid=Id(13);row.previous_version_uuid=original->version_uuid;row.previous_row_version=1;
    row.row_version=2;row.transaction_uuid={platform::UuidKind::transaction,Id(12)};row.local_transaction_id=2;
    row.internal_row_ordinal=row.stable_slot_id=leaf.body.rows.size()+1;
    metadata.creator_transaction_uuid=row.transaction_uuid;metadata.creator_local_transaction_id=2;metadata.definition_version=2;
    if(configuration_phase){
      auto binding=catalog::DecodeCatalogRuntimeAuthorityBinding(metadata.record.payload);
      Check(binding.ok(),"retained actual runtime configuration family");binding.record->generation=2;
      const auto payload=catalog::EncodeCatalogRuntimeAuthorityBinding(*binding.record);
      Check(payload.ok(),"retained configuration successor exact native payload");metadata.record.payload.assign(payload.bytes.begin(),payload.bytes.end());
    }else{
      auto principal=catalog::DecodeCatalogSecurityRecord(metadata.record.header.kind,metadata.record.payload);
      Check(principal.ok(),"retained successor family codec");principal.record->attributes["active"]=false;
      const auto payload=catalog::EncodeCatalogSecurityRecord(*principal.record);Check(payload.ok(),"retained successor exact native payload");
      metadata.record.payload.assign(payload.bytes.begin(),payload.bytes.end());
      if(scenario==8){metadata.record.header.deleted=true;metadata.lifecycle=catalog::CatalogObjectLifecycle::dropped;
        metadata.status=catalog::CatalogObjectStatus::retired;metadata.retired_transaction_uuid=row.transaction_uuid;}
      if(scenario==6){
        // An unavailable predecessor is a valid resident chain boundary. A
        // different UUID for a predecessor still resident at the same row and
        // sequence is instead a malformed leaf, before MGA can be observed.
        row.previous_version_uuid=Id(249);
        leaf.body.rows.erase(original);
      }
      if(scenario==7||scenario==9){row.row_uuid={platform::UuidKind::row,Id(248)};metadata.record.header.row_uuid=row.row_uuid;
        row.previous_version_uuid={};row.previous_row_version=0;row.row_version=1;metadata.definition_version=1;
        principal.record->attributes["creator_tx"]=u64{2};principal.record->identities["origin_transaction_uuid"]=Id(12);
        if(scenario==9){metadata.record.header.object_uuid.value=Id(206);principal.record->identities["principal_uuid"]=Id(206);}
        const auto duplicate=catalog::EncodeCatalogSecurityRecord(*principal.record);Check(duplicate.ok(),"duplicate object canonical payload");
        metadata.record.payload.assign(duplicate.bytes.begin(),duplicate.bytes.end());}
    }
    const auto outer=catalog::EncodeCatalogMetadataVersion(metadata);Check(outer.ok(),"retained successor common metadata");
    row.cells.front().value.payload=outer.bytes;leaf.body.rows.push_back(row);
    // Prove framing and capacity BEFORE the independent byte writer, whose
    // intentionally minimal fixed-page fixture assumes every row fits.
    const auto bounded_body=page::BuildRowDataPageBody(leaf.body,leaf.header.page_size_bytes-32);
    if(!bounded_body.ok())std::cerr<<"retained row framing scenario="<<scenario<<" configuration="<<configuration_phase
        <<" code="<<bounded_body.diagnostic.diagnostic_code<<" key="<<bounded_body.diagnostic.message_key<<'\n';
    Check(bounded_body.ok(),"retained history fits actual selected page profile and row framing");
    const auto canonical=db::EncodeNativeCatalogLeaf(leaf);
    if(!canonical.ok())std::cerr<<"retained canonical leaf scenario="<<scenario<<" configuration="<<configuration_phase
        <<" error="<<int(canonical.error)<<'\n';
    Check(canonical.ok(),"retained history satisfies exact family origin and common metadata binding");
    const auto leaf_bytes=LeafOracle(leaf);
    if(leaf_bytes!=canonical.bytes){const auto mismatch=std::mismatch(leaf_bytes.begin(),leaf_bytes.end(),canonical.bytes.begin());
      std::cerr<<"retained independent image scenario="<<scenario<<" configuration="<<configuration_phase
          <<" offset="<<(mismatch.first-leaf_bytes.begin())<<" oracle="<<unsigned(*mismatch.first)
          <<" canonical="<<unsigned(*mismatch.second)<<'\n';}
    Check(leaf_bytes==canonical.bytes,"independent retained history matches the exact canonical image");
    Check(db::DecodeNativeCatalogLeaf(leaf_bytes).ok(),"independent retained leaf image");
    const auto& stored_inventory=selected.checkpoint_inventory.inventory_pages.front();
    page::NativeTransactionInventoryPage inventory_page;
    inventory_page.header=stored_inventory.header;inventory_page.object_uuid=stored_inventory.object_uuid;
    inventory_page.inventory_generation=selected.checkpoint_inventory.inventory_generation;inventory_page.inventory=inventory;
    const auto inventory_bytes=InventoryStateOracle(inventory_page);Check(page::DecodeNativeTransactionInventoryPage(inventory_bytes).ok(),"independent actual MGA inventory image");
    auto checkpoint=*selected.checkpoint_inventory.checkpoint;checkpoint.selected_local_transaction_id=2;
    const auto root=std::find_if(checkpoint.roots.begin(),checkpoint.roots.end(),[](const auto& r){return r.role==1;});
    Check(root!=checkpoint.roots.end(),"actual checkpoint inventory role");root->sha256=WholeRootHash(inventory_bytes);
    const auto checkpoint_bytes=CheckpointOracle(checkpoint);
    Check(db::DecodeNativeCheckpointRoot(checkpoint_bytes).ok(),"independent checkpoint exact selected inventory binding");
    const auto put=[&](u64 number,const Bytes& bytes){const auto io=device.WriteAt(number*u64{sizes[profile]},bytes.data(),bytes.size());
      Check(io.ok()&&io.bytes_transferred==bytes.size(),"actual independent MGA fixture page write");};
    put(leaf.header.page_number,leaf_bytes);put(inventory_page.header.page_number,inventory_bytes);put(checkpoint.header.page_number,checkpoint_bytes);
    for(const auto& slot:selected.slots){const auto decoded=db::DecodeNativeCheckpointSelection(slot);Check(decoded.ok(),"actual selected replica frame");
      auto selection=*decoded.selection;selection.checkpoint_sha256=WholeRootHash(checkpoint_bytes);put(selection.header.page_number,SelectionOracle(selection));}
    Check(device.Sync().ok(),"actual MGA fixture synchronized before source admission");
  }
};
void Empty(const SecurityResult& result,SecurityError error) {
  if(result.error!=error)std::cerr<<"security error="<<int(result.error)<<" expected="<<int(error)<<" source="<<int(result.source_error)
      <<" relation="<<int(result.relation_error)<<" record="<<int(result.record_error)<<'\n';
  Check(result.error==error&&!result.binding&&!result.principal&&result.binding_version_uuid.is_nil()&&result.records.empty(),
      "refusal exposes no usable security/binding prefix");
}
void Governed(unsigned profile,bool indexed,unsigned variant=0,unsigned mga_scenario=0,bool configuration_phase=false) {
  SecurityFixture f(profile,variant,indexed);
  if(mga_scenario)f.RetainedSuccessor(mga_scenario,configuration_phase);
  const db::NativeCatalogRelationMemoryLimits limits{Profile(profile),1,16,64,64,{4,16*static_cast<std::size_t>(sizes[profile])}};
  const auto amount=runtime_security::NativeRuntimeSecurityWorkspaceBytes(limits);Check(amount,"checked governed security backing size");
  LeafMemoryFixture memory(amount),source_memory(32*1024*1024);
  auto prepared=runtime_security::PrepareNativeRuntimeSecurityRead(limits,memory.memory,memory.binding);
  Check(prepared.ok(),"actual binary-bound security workspace preparation before guards");
  const std::array<disk::NativeFilespaceDevice,1> devices{{{Id(2),Profile(profile),&f.device}}};
  auto source=db::AcquireNativeSelectedCheckpointMemoryLease(Id(1),devices,Id(2),{f.budget,32*1024*1024},source_memory.memory,source_memory.binding);
  Check(source.ok(),"actual selected checkpoint and committed inventory lease");
  const auto read=[&](runtime_security::NativeRuntimeSecurityReadRequest r){
    allocation_budget=0;const auto result=runtime_security::ReadCommittedNativeRuntimeSecurityOnLease(*source.lease,r,prepared.workspace);
    const auto remaining=allocation_budget;allocation_budget=-1;
    Check(remaining==0,"native security observation makes no heap allocation under source/device guards");return result;
  };
  auto request=f.Request();const auto writes=stage_writes,syncs=stage_syncs;
  reads=0;track_reads=true;const auto valid=read(request);track_reads=false;const auto read_count=reads;
  if(mga_scenario>=4&&mga_scenario!=9){Empty(valid,configuration_phase?SecurityError::binding_source_failure:
        mga_scenario==4||mga_scenario==8?SecurityError::principal_not_active:SecurityError::security_source_failure);
    if(mga_scenario==5){
      const auto& diagnostic=valid.visibility_diagnostic;
      Check(valid.source_error==db::NativePinnedCatalogReadError::requires_recovery&&
          std::string_view(diagnostic.diagnostic_code)=="SB-ROW-VISIBILITY-REQUIRES-RECOVERY"&&
          std::string_view(diagnostic.message_key)=="row_version.visibility_requires_recovery"&&
          std::string_view(diagnostic.detail)=="limbo"&&diagnostic.status.severity==platform::Severity::warning&&
          diagnostic.origin==mga::MgaObservationOrigin::row_version,
          configuration_phase?"configuration-phase limbo preserves full actual typed MGA cause":"security-phase limbo preserves full actual typed MGA cause");
    }
    if(mga_scenario==6)Check(valid.source_error==db::NativePinnedCatalogReadError::missing_version||
        valid.source_error==db::NativePinnedCatalogReadError::invalid_chain,"missing predecessor is not principal absence");
    if(mga_scenario==7)Check(valid.source_error==db::NativePinnedCatalogReadError::duplicate_identity,"reserved object UUID cannot be reassigned in a second row");
  }else if(variant&&variant!=8){const SecurityError errors[]{SecurityError::none,SecurityError::principal_not_active,SecurityError::principal_not_service,
      SecurityError::principal_generation_invalid,SecurityError::principal_wrong_family,SecurityError::principal_absent,
      SecurityError::external_authority_required,SecurityError::binding_not_active};Empty(valid,errors[variant]);}
  else {
    if(!valid.ok())std::cerr<<"security read="<<int(valid.error)<<" binding="<<int(valid.binding_error)<<" relation="<<int(valid.relation_error)<<'\n';
    Check(valid.ok()&&valid.records.size()==6&&valid.physical_bytes_read,"actual committed native cohort returned");
    Check(valid.principal->principal_uuid==Id(200)&&valid.principal->security_context_generation==(variant==8?UINT64_MAX:9)&&
        valid.principal->security_generation==(variant==8?UINT64_MAX:11)&&valid.principal->policy_generation==(variant==8?UINT64_MAX:13)&&valid.binding->policy_epoch==31&&
        valid.binding->provider_generation==37,"actual source generations remain distinct from configuration epochs");
    const auto find=[&](unsigned id)->const auto&{
      const auto found=std::find_if(valid.records.begin(),valid.records.end(),[&](const auto& r){return r.object_uuid==Id(id);});
      Check(found!=valid.records.end(),"actual retained security identity");return *found;
    };
    Check(find(201).record.Attribute("effect")=="allow"&&find(201).record.Attribute("right")=="CONNECT", "exact direct grant");
    Check(find(203).record.Identity("member_uuid")==Id(200)&&find(203).record.Identity("parent_uuid")==Id(202),"membership graph retained without flattening");
    Check(find(204).record.Attribute("effect")=="deny"&&find(204).record.Identity("target_uuid")==Id(240),"targeted denial retained");
    Check(find(205).lifecycle==catalog::CatalogObjectLifecycle::archived&&find(205).status==catalog::CatalogObjectStatus::retired&&find(205).record.Boolean("active")==false&&
        find(205).retired_transaction_uuid==security_tx.value&&find(205).retired_local_transaction_id==1,"retirement remains real inventory-bound data");
    auto bad=request;bad.runtime_binding_generation=2;Empty(read(bad),SecurityError::binding_generation_mismatch);
    bad=request;bad.runtime_binding_uuid=Id(250);Empty(read(bad),SecurityError::binding_absent);
    bad=request;bad.database_uuid=Id(250);Empty(read(bad),SecurityError::invalid_request);
    bad=request;bad.security_relation=request.configuration_relation;Empty(read(bad),SecurityError::security_source_failure);
    bad=request;bad.configuration_relation=request.security_relation;Empty(read(bad),SecurityError::binding_source_failure);
    bad=request;bad.maximum_retained_image_bytes=1;Empty(read(bad),SecurityError::binding_source_failure);
    if(indexed){bad=request;bad.security_relation.index_dependencies->descriptor_generation=2;Empty(read(bad),SecurityError::security_source_failure);}
    auto saved=std::move(prepared.workspace);Empty(read(request),SecurityError::backing_unavailable);prepared.workspace=std::move(saved);
    const auto copied_binding=*valid.binding;const auto copied_principal=*valid.principal;
    Check(read(request).ok()&&copied_binding.binding_uuid==Id(180)&&copied_principal.security_generation==(variant==8?UINT64_MAX:11),
        "fixed observations survive workspace reuse while borrowed-record lifetime is explicitly bounded");
    for(unsigned fault=1;fault<=read_count;++fault){
      reads=0;read_fault=fault;track_reads=true;const auto failed=read(request);track_reads=false;
      Check(!read_fault,"each actual native security/configuration read position injected");
      Check(!failed.ok()&&!failed.binding&&!failed.principal&&failed.records.empty(),"physical failure returns no usable cohort");
      Check(failed.io_failure&&failed.io_failure->error==disk::BoundedIoError::native_failure&&
          failed.io_failure->native_attempted&&failed.io_failure->native_error==EIO&&failed.io_failure_filespace_uuid==Id(2),
          "preserve typed native I/O cause and owning binary filespace");
    }
    Check(read(request).ok(),"read workspace recovers after every physical failure");
  }
  Check(stage_writes==writes&&stage_syncs==syncs,"security observation has no physical mutation or sync effects");
  source.lease.Reset();prepared.workspace={};source_memory.memory={};memory.memory={};source_memory.Empty();memory.Empty();
  Check(f.device.Close().ok(),"native security device closes after all readers/grants");f.artifacts.Cleanup();
}
void BackingBounds() {
  auto retired=SecurityMetadata(SecurityRecord(catalog::CatalogRecordKind::user_account,205),5).metadata;
  retired.lifecycle=catalog::CatalogObjectLifecycle::archived;
  retired.status=catalog::CatalogObjectStatus::retired;retired.retired_transaction_uuid=security_tx;
  Check(!catalog::ValidateCatalogMetadataVersionView(catalog::BorrowCatalogMetadataVersion(retired)),
      "retirement is bound to the actual common-metadata version creator");
  retired.retired_transaction_uuid={platform::UuidKind::transaction,Id(12)};
  const auto invalid_retirement=catalog::ValidateCatalogMetadataVersionView(catalog::BorrowCatalogMetadataVersion(retired));
  Check(invalid_retirement&&invalid_retirement->detail=="retirement_binding_invalid"&&
      !catalog::EncodeCatalogMetadataVersion(retired).ok(),
      "a foreign retirement identity cannot substitute another inventory entry for the version creator");
  const db::NativeCatalogRelationMemoryLimits limits{Profile(0),1,1,8,8,{1,sizes[0]}};
  const auto amount=runtime_security::NativeRuntimeSecurityWorkspaceBytes(limits);
  Check(amount>1,"positive checked workspace bound");
  LeafMemoryFixture memory(amount-1);
  {const auto refused=runtime_security::PrepareNativeRuntimeSecurityRead(limits,memory.memory,memory.binding);
    Check(!refused.ok()&&!refused.workspace,"one-byte short governed reservation cannot publish a workspace");}
  auto zero=limits;zero.maximum_rows=0;
  Check(!runtime_security::NativeRuntimeSecurityWorkspaceBytes(zero),"zero record capacity is invalid");
  zero=limits;zero.maximum_rows=std::numeric_limits<std::size_t>::max();
  Check(!runtime_security::NativeRuntimeSecurityWorkspaceBytes(zero),"workspace multiplication and total overflow refuse");
  auto foreign=memory.binding;foreign.database_uuid=Id(250);
  {const auto refused=runtime_security::PrepareNativeRuntimeSecurityRead(limits,memory.memory,foreign);
    Check(!refused.ok()&&!refused.workspace,"foreign binary grant binding cannot prepare security backing");}
  memory.memory={};memory.Empty();
}
}
int main()try {
  BackingBounds();
  for(unsigned profile=0;profile<5;++profile){Governed(profile,false);Governed(profile,true);}
  for(unsigned variant=1;variant<=8;++variant)Governed(0,true,variant);
  for(unsigned scenario=1;scenario<=9;++scenario)Governed(0,false,0,scenario);
  Governed(0,false,0,5,true);
  std::cout<<"PASS native runtime security observation "<<checks<<" checks; no authentication or startup authority claimed\n";return 0;
}catch(const std::exception& error){allocation_budget=-1;std::cerr<<"FAIL "<<error.what()<<" checks="<<checks<<'\n';return 1;}
