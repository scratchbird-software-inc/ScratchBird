// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
// Retain the qualified real-file/independent-byte and native I/O fault fixture.
#define SB_NATIVE_STORAGE_FIXTURE_WITHOUT_MAIN
#include "filespace_page_zero_test.cpp"
#undef SB_NATIVE_STORAGE_FIXTURE_WITHOUT_MAIN
#include "security/native_runtime_security_observation.hpp"
#include "native_creation_workspace.hpp"
#include "native_catalog_version_writer.hpp"
#include "bootstrap_policy_fixture.hpp"
#include "../support/owned_temp_directory.hpp"
#include "../support/catalog_change_event_fixture.hpp"

namespace {
namespace runtime_security=scratchbird::engine::internal_api;
namespace security_fixture=scratchbird::tests::bootstrap_policy;
using SecurityError=runtime_security::NativeRuntimeSecurityReadError;
using SecurityResult=runtime_security::NativeRuntimeSecurityObservationResult;
const platform::TypedUuid security_tx{platform::UuidKind::transaction,Id(5)};

// SB_CATALOG_NATIVE_SECURITY_RECORDS (3a-2): the fixture writes the typed
// native security records native create writes (identities and coding-time
// constants; no names or text classifications). The values are the ones the
// attribute-bag fixture used: creator/origin local 1, context version 1,
// context generation 9, security generation 11, policy generation 13, active.
using SecurityRow=catalog::NativeSecurityRecord;
SecurityRow SecurityRecord(catalog::CatalogRecordKind kind,unsigned id) {
  using K=catalog::CatalogRecordKind;SecurityRow r;r.kind=kind;
  const auto common=[&](auto& v){v.origin_transaction_uuid=security_tx;v.origin_local_transaction_id=1;v.creator_tx=1;v.policy_generation=13;};
  if(kind==K::user_account){catalog::NativeSecurityPrincipal v;v.principal_uuid=Id(id);common(v);
    v.principal_kind=catalog::CatalogSecurityPrincipalKind::user;v.active=true;v.security_context_authority_version=1;
    v.security_context_generation=9;v.security_generation=11;r.value=v;}
  else if(kind==K::role_account){catalog::NativeSecurityRole v;v.role_uuid=Id(id);common(v);v.active=true;
    v.security_context_authority_version=1;v.security_context_generation=9;r.value=v;}
  else if(kind==K::group_account){catalog::NativeSecurityGroup v;v.group_uuid=Id(id);common(v);r.value=v;}
  else {catalog::NativeSecurityGrant v;v.grant_uuid=Id(id);common(v);v.security_generation=11;r.value=v;}
  return r;
}
catalog::NativeSecurityPrincipal& AsPrincipal(SecurityRow& r){return std::get<catalog::NativeSecurityPrincipal>(r.value);}
catalog::NativeSecurityRole& AsRole(SecurityRow& r){return std::get<catalog::NativeSecurityRole>(r.value);}
catalog::NativeSecurityGrant& AsGrant(SecurityRow& r){return std::get<catalog::NativeSecurityGrant>(r.value);}
catalog::CatalogValueEncodeResult EncodeSecurityRow(const SecurityRow& r){
  if(const auto* v=r.principal())return catalog::EncodeNativeSecurityPrincipal(*v);
  if(const auto* v=r.role())return catalog::EncodeNativeSecurityRole(*v);
  if(const auto* v=r.group())return catalog::EncodeNativeSecurityGroup(*v);
  if(const auto* v=r.grant())return catalog::EncodeNativeSecurityGrant(*v);
  return {catalog::CatalogValueError::invalid_value,{}};
}
// A structurally valid stored verifier: the observation never authenticates.
catalog::NativeSecurityCredential StructuralCredential(){
  catalog::NativeSecurityCredential c;c.scheme=catalog::CatalogSecurityCredentialScheme::pbkdf2_sha256_v1;c.iterations=600000;
  c.salt.fill(0x5a);c.verifier.fill(0xa5);return c;
}
db::NativeCreationCatalogRecord SecurityMetadata(SecurityRow record,unsigned ordinal) {
  auto metadata=security_fixture::Metadata(0,security_tx,Id(60),{});
  metadata.record.header.kind=record.kind;
  metadata.record.header.object_uuid={platform::UuidKind::object,record.primary()};
  metadata.record.header.row_uuid={platform::UuidKind::row,security_fixture::Id(3000+ordinal)};
  metadata.object_subtype=catalog::CatalogSecuritySubtype(record.kind);
  const auto encoded=EncodeSecurityRow(record);Check(encoded.ok(),"canonical native security payload");
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
  // `audited` (W3b-2b): the fixture's scenarios write a catalog change, so the
  // create's construction intent is laid down (see below).
  explicit SecurityFixture(unsigned p,unsigned variant=0,bool indexed=false,bool audited=false):profile(p),budget(512*u64{sizes[p]}) {
    using K=catalog::CatalogRecordKind;db::NativeCreationCatalogSeed seed;
    // 3a-2: the principal's name ("native-service") is a names-table entry,
    // not a record field, and its stored verifier is the typed binary
    // credential (structural only, never authenticated here).
    auto principal=SecurityRecord(K::user_account,200);
    {auto& p=AsPrincipal(principal);p.principal_kind=catalog::CatalogSecurityPrincipalKind::service;p.bootstrap_principal=false;
     p.credential=StructuralCredential();
     if(variant==1)p.active=false;
     if(variant==2)p.principal_kind=catalog::CatalogSecurityPrincipalKind::user;
     if(variant==3)p.security_context_generation=0;
     if(variant==8)p.security_context_generation=p.security_generation=p.policy_generation=std::numeric_limits<u64>::max();}
    if(variant==4)principal=SecurityRecord(K::role_account,200);
    seed[3].push_back(SecurityMetadata(principal,0));
    using SK=catalog::CatalogSecuritySubjectKind;using GC=catalog::CatalogSecurityGrantClass;
    using EF=catalog::CatalogSecurityEffect;using RT=catalog::CatalogSecurityRight;
    auto grant=SecurityRecord(K::grant_record,201);
    {auto& g=AsGrant(grant);g.subject_uuid=Id(200);g.grant_class=GC::privilege;g.subject_kind=SK::principal;
     g.right=RT::CONNECT;g.effect=EF::allow;}
    seed[3].push_back(SecurityMetadata(grant,1));
    // 3a-2: as the attribute-bag fixture: group 202, the principal a member of
    // it (group_membership, parent_kind group), a targeted denial on the group.
    auto group=SecurityRecord(K::group_account,202);
    seed[3].push_back(SecurityMetadata(group,2));
    auto membership=SecurityRecord(K::grant_record,203);
    {auto& g=AsGrant(membership);g.member_uuid=Id(200);g.parent_uuid=Id(202);g.grant_class=GC::group_membership;
     g.member_kind=SK::principal;g.parent_kind=SK::group;}
    seed[3].push_back(SecurityMetadata(membership,3));
    auto denial=SecurityRecord(K::grant_record,204);
    {auto& g=AsGrant(denial);g.subject_uuid=Id(202);g.target_uuid=Id(240);g.grant_class=GC::privilege;
     g.subject_kind=SK::group;g.right=RT::SELECT;g.effect=EF::deny;}
    seed[3].push_back(SecurityMetadata(denial,4));
    auto retired=SecurityRecord(K::user_account,205);AsPrincipal(retired).active=false;
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
    meta.object_subtype=scratchbird::core::catalog::CatalogObjectSubtype::agent_runtime_authority;meta.record.payload.assign(encoded.bytes.begin(),encoded.bytes.end());
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
    // W3b-2b: the create's construction intent (role 6), as the production
    // create writes it, so a transaction can write its catalog.metadata_change
    // event (the writer builds it from this row) before changing a definition.
    // Only where a scenario writes: the read-only scenarios keep the layout
    // they had.
    if(audited){const auto intent=scratchbird::tests::catalog_change_event::IntentSeedRow(request,security_fixture::Id(9790),issuer,
        security_fixture::Metadata(0,security_tx,Id(61),{}));
     Check(intent.has_value(),"the fixture's construction intent");seed[5].push_back({*intent,{}});}
    for(const auto& relation:seed)for(const auto& row:relation){
      const auto invalid=catalog::ValidateCatalogMetadataVersionView(catalog::BorrowCatalogMetadataVersion(row.metadata));
      if(invalid)std::cerr<<"native security fixture metadata="<<invalid->detail<<" profile="<<p<<" variant="<<variant<<'\n';
      Check(!invalid,"complete actual family/common metadata before native security fixture construction");
    }
    const auto path=(artifacts.path()/"native-security").string();Check(device.Open(path,disk::FileOpenMode::create_new,disk::DatabaseFileOwnership::native_data_file).ok(),"actual new native security filespace");
    // SB_NATIVE_FIXED_ROOT (in-place writes W4b): the production create's
    // fixed-root graph. Scenarios change the file in place as the engine does:
    // the transaction inventory writer (W1), the catalog version writer (W3a)
    // and the post-commit catalog root advance (W4a). States no standalone
    // writer produces are forged with the production codecs, each labelled.
    request.genesis_graph=db::NativeCreationGenesisGraph::fixed_roots;
    const auto made=db::InitializeIndexedNativeCreationWorkspaceOnOpenDevice(device,request,seed,indexes,budget,issuer);
    if(!made.ok())std::cerr<<"native security fixture creation="<<int(made.error)<<" profile="<<p<<" variant="<<variant<<'\n';
    Check(made.ok(),"actual native security/configuration creation in their declared roles");receipt=*made.receipt;
    Check(device.Close().ok()&&device.Open(path,disk::FileOpenMode::open_existing).ok(),"independent physical native security reopen");
  }
  // Page zero's catalog (2), transaction-state (4) and route (6, 7) roots.
  struct FixedRoots{disk::FilespaceRootReference catalog,state,configuration,security;};
  static FixedRoots RootsOf(disk::FileDevice& device) {
    const auto zero=disk::ReadFilespacePageZeroFromOpenDevice(device);Check(zero.ok(),"page zero of the fixed-root fixture");
    FixedRoots r;
    for(const auto& root:zero.record->roots){if(root.kind==2)r.catalog=root;if(root.kind==4)r.state=root;
      if(root.kind==6)r.configuration=root;if(root.kind==7)r.security=root;}
    Check(r.catalog.kind==2&&r.state.kind==4&&r.configuration.kind==6&&r.security.kind==7,"page zero names the catalog, state and route roots");
    return r;
  }
  static page::TransactionWriterBinding TxBinding(const FixedRoots& r) {
    page::TransactionWriterBinding b;b.database_uuid=Id(1);b.state_root=r.state;return b;
  }
  static db::CatalogVersionWriterBinding CvBinding(const FixedRoots& r) {
    db::CatalogVersionWriterBinding b;b.database_uuid=Id(1);b.catalog_root=r.catalog;b.state_root=r.state;
    b.configuration_root=r.configuration;b.security_root=r.security;return b;
  }
  // FORGED (production codec): a state word the standalone writer never
  // writes. prepared needs local prepare enabled; limbo is cluster-only and
  // must fail closed (specification chapter 4). The reader must resolve both.
  static void ForgeStateWord(disk::FileDevice& device,unsigned profile,const FixedRoots& r,u64 order,page::TipStateCode code) {
    const auto at=page::LocateTransactionStateSlot(sizes[profile],order);
    Check(at.has_value()&&at->tip_sequence==0,"the forged slot is on the first state page");
    std::vector<byte> image(sizes[profile]);const auto offset=r.state.page_number*u64{sizes[profile]};
    Check(device.ReadAt(offset,image.data(),image.size()).ok(),"read the state page to forge");
    const auto edit=page::SetTransactionStateCode(image,at->slot,code);Check(edit.ok(),"the forged state word");
    Check(device.WriteAt(offset+edit.unit_offset,image.data()+edit.unit_offset,page::kTransactionStateWriteUnitBytes).ok()&&
        device.Sync().ok(),"the forged state word persists");
  }
  runtime_security::NativeRuntimeSecurityReadRequest Request()const {
    runtime_security::NativeRuntimeSecurityReadRequest r;
    r.database_uuid=Id(1);r.runtime_binding_uuid=Id(180);r.runtime_binding_generation=1;r.maximum_retained_image_bytes=budget;
    r.configuration_relation={receipt.relation_uuids[4],receipt.index_dependencies[4]};
    r.security_relation={receipt.relation_uuids[3],receipt.index_dependencies[3]};return r;
  }
  static void BindingEpochsOnDevice(disk::FileDevice& device,unsigned profile,
      const page::NativeCatalogRootReference& relation,u64 catalog_generation,u64 security_epoch) {
    const auto loaded=db::ReadNativeCatalogLeafFromOpenDevice(device,Id(1),relation);
    Check(loaded.ok(),"actual unindexed binding leaf before future-provenance fixture");
    auto leaf=*loaded.page;bool found=false;
    for(auto& row:leaf.body.rows){auto metadata=loaded.metadata.at(row.version_uuid);
      if(metadata.record.header.object_uuid.value!=Id(180))continue;
      auto binding=catalog::DecodeCatalogRuntimeAuthorityBinding(metadata.record.payload);
      Check(binding.ok(),"actual binding before independent provenance fixture");
      binding.record->catalog_generation=catalog_generation;binding.record->security_epoch=security_epoch;
      const auto encoded=catalog::EncodeCatalogRuntimeAuthorityBinding(*binding.record);
      Check(encoded.ok(),"canonical full-width future provenance fixture fields");
      metadata.catalog_generation=catalog_generation;metadata.security_epoch=security_epoch;
      metadata.record.payload.assign(encoded.bytes.begin(),encoded.bytes.end());
      const auto outer=catalog::EncodeCatalogMetadataVersion(metadata);Check(outer.ok(),"common binding provenance fixture");
      row.cells.front().value.payload=outer.bytes;found=true;
    }
    Check(found,"actual selected binary binding changed in independent fixture");
    const auto canonical=db::EncodeNativeCatalogLeaf(leaf);Check(canonical.ok(),"independent future binding native leaf framing");
    const auto bytes=LeafOracle(leaf);Check(bytes==canonical.bytes,"independent future binding exact byte oracle");
    const auto io=device.WriteAt(leaf.header.page_number*u64{sizes[profile]},bytes.data(),bytes.size());
    Check(io.ok()&&io.bytes_transferred==bytes.size()&&device.Sync().ok(),"future binding fixture persisted before native source admission");
  }
  // SB_CATALOG_ROOT_ADVANCE (owner Q24 = 1): a committed transaction, then the
  // catalog root and both route roots advanced in place, as the engine does
  // after a catalog change commits. The checkpoint era also built a retained
  // predecessor root and checked the root history range; Q24 removed that
  // history (no predecessor is written), so that part is gone by ruling.
  void SelectedCatalogEpochs(u64 catalog_generation,u64 security_epoch,u64 schema_epoch=1,u64 resource_epoch=0) {
    SelectedCatalogEpochsOnDevice(device,profile,budget,catalog_generation,security_epoch,schema_epoch,resource_epoch);
  }
  static void SelectedCatalogEpochsOnDevice(disk::FileDevice& device,unsigned profile,u64 budget,
      u64 catalog_generation,u64 security_epoch,u64 schema_epoch=1,u64 resource_epoch=0) {
    (void)budget;const auto roots=RootsOf(device);
    std::vector<byte> image(sizes[profile]);
    Check(device.ReadAt(roots.catalog.page_number*u64{sizes[profile]},image.data(),image.size()).ok(),"read the catalog root");
    const auto current=page::DecodeNativeCatalogRoot(image);
    Check(current.ok()&&catalog_generation==2&&current.root->catalog_generation==1,
        "the epoch fixture advances generation one to two");
    disk::FileDeviceCarefulWriteSink sink(device);
    page::TransactionInventoryWriter w(TxBinding(roots));Check(w.Restart(sink).ok(),"restart before the epoch fixture");
    page::TransactionHeaderRecord h;h.transaction_uuid=Id(14);
    const auto begun=w.Begin(sink,h);Check(begun.ok(),"a transaction whose COMMIT the root advance follows");
    Check(w.Commit(sink,*begun.transaction_order,1789357073000000000ULL).ok(),"its in-place COMMIT");
    db::CatalogRootAdvanceRequest advance;advance.transaction_order=*begun.transaction_order;advance.transaction_uuid=Id(14);
    advance.catalog_generation=catalog_generation;advance.schema_epoch=schema_epoch;advance.security_epoch=security_epoch;
    advance.resource_epoch=resource_epoch;
    const auto advanced=db::AdvanceCatalogRoot(sink,CvBinding(roots),advance);
    if(!advanced.ok())std::cerr<<"root advance="<<db::CatalogVersionWriteErrorName(advanced.error)<<'\n';
    Check(advanced.ok()&&advanced.writes==3,"the catalog root and both route roots advance in place after the COMMIT");
  }
  // Transaction 2 changes the selected object in place (W1 + W3a) and is left
  // in the scenario's state: 1 active, 2 prepared, 3 rolled back, 4 committed,
  // 5 limbo, 8 committed drop. The subject reader must resolve each by MGA,
  // not by a supplied flag. Scenarios 6, 7 and 9 are FORGED leaves (production
  // leaf codec, independent byte oracle) over the same in-place transaction:
  // 6 a resident chain whose predecessor is gone and 7 a second row for the
  // same object are reader refusals; 9, a new principal inserted by the
  // in-progress transaction, is a positive state the engine will write once
  // inserts exist (W3b) and is forged until then.
  void RetainedSuccessor(unsigned scenario,bool configuration_phase=false) {
    const unsigned role=configuration_phase?4:3;
    Check(!indexes[role],"MGA fixture uses the actual unindexed owning head");
    const auto roots=RootsOf(device);
    disk::FileDeviceCarefulWriteSink sink(device);
    page::TransactionInventoryWriter w(TxBinding(roots));Check(w.Restart(sink).ok(),"restart before the MGA fixture");
    page::TransactionHeaderRecord header;header.transaction_uuid=Id(12);
    const auto begun=w.Begin(sink,header);Check(begun.ok()&&begun.transaction_order==2u,"actual in-place BEGIN of transaction 2");
    const auto loaded=db::ReadNativeCatalogLeafFromOpenDevice(device,Id(1),receipt.relations[role]);
    Check(loaded.ok(),"actual stored owning base leaf before retained version fixture");auto leaf=*loaded.page;
    const auto original=std::find_if(leaf.body.rows.begin(),leaf.body.rows.end(),[&](const auto& row){
      return loaded.metadata.at(row.version_uuid).record.header.object_uuid.value==Id(configuration_phase?180:200);});
    Check(original!=leaf.body.rows.end(),"actual persisted selected object row");
    auto row=*original;auto metadata=loaded.metadata.at(row.version_uuid);const auto predecessor=original->version_uuid;
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
      auto principal=catalog::DecodeNativeSecurityRecord(metadata.record.header.kind,metadata.record.payload);
      Check(principal.ok(),"retained successor family codec");AsPrincipal(*principal.record).active=false;
      const auto payload=EncodeSecurityRow(*principal.record);Check(payload.ok(),"retained successor exact native payload");
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
        {auto& p=AsPrincipal(*principal.record);p.creator_tx=p.origin_local_transaction_id=2;
         p.origin_transaction_uuid={platform::UuidKind::transaction,Id(12)};
         if(scenario==9){metadata.record.header.object_uuid.value=Id(206);p.principal_uuid=Id(206);}}
        const auto duplicate=EncodeSecurityRow(*principal.record);Check(duplicate.ok(),"duplicate object canonical payload");
        metadata.record.payload.assign(duplicate.bytes.begin(),duplicate.bytes.end());}
    }
    if(scenario==6||scenario==7||scenario==9){
      const auto outer=catalog::EncodeCatalogMetadataVersion(metadata);Check(outer.ok(),"forged version common metadata");
      row.cells.front().value.payload=outer.bytes;leaf.body.rows.push_back(row);
      const auto bounded_body=page::BuildRowDataPageBody(leaf.body,leaf.header.page_size_bytes-32);
      Check(bounded_body.ok(),"the forged leaf fits the actual page profile and row framing");
      const auto canonical=db::EncodeNativeCatalogLeaf(leaf);
      Check(canonical.ok(),"the forged leaf satisfies exact family origin and common metadata binding");
      const auto leaf_bytes=LeafOracle(leaf);
      Check(leaf_bytes==canonical.bytes,"independent forged leaf matches the exact canonical image");
      const auto io=device.WriteAt(leaf.header.page_number*u64{sizes[profile]},leaf_bytes.data(),leaf_bytes.size());
      Check(io.ok()&&io.bytes_transferred==leaf_bytes.size()&&device.Sync().ok(),"the forged leaf persists");
    }else{
      // W3b-2b: transaction 2's catalog.metadata_change event, which its successor cites.
      Check(scratchbird::tests::catalog_change_event::WriteEvent(sink,CvBinding(roots),2,Id(12),security_fixture::Id(9791),security_fixture::Id(9792),security_fixture::Id(9793),security_fixture::Id(9790),
          1789357072001000ULL).ok(),"transaction 2's catalog change event");
      metadata.audit_uuid={platform::UuidKind::object,security_fixture::Id(9791)};
      db::CatalogVersionRequest r;r.role=static_cast<platform::u16>(role+1);r.transaction_order=2;r.transaction_uuid=Id(12);
      r.row_uuid=row.row_uuid.value;r.predecessor_version_uuid=predecessor;r.version_uuid=Id(13);r.metadata=metadata;
      const auto written=db::WriteCatalogSuccessorVersion(sink,CvBinding(roots),r);
      if(!written.ok())std::cerr<<"successor="<<db::CatalogVersionWriteErrorName(written.error)<<" scenario="<<scenario<<'\n';
      Check(written.ok(),"the successor version written in place by transaction 2");
    }
    if(scenario==2)ForgeStateWord(device,profile,roots,2,page::TipStateCode::prepared);
    if(scenario==3)Check(w.Rollback(sink,2,1789357072002000000ULL).ok(),"actual in-place ROLLBACK");
    if(scenario==4||scenario==8)Check(w.Commit(sink,2,1789357072002000000ULL).ok(),"actual in-place COMMIT");
    if(scenario==5)ForgeStateWord(device,profile,roots,2,page::TipStateCode::limbo);
  }
};
void Empty(const SecurityResult& result,SecurityError error) {
  if(result.error!=error)std::cerr<<"security error="<<int(result.error)<<" expected="<<int(error)<<" source="<<int(result.source_error)
      <<" relation="<<int(result.relation_error)<<" record="<<int(result.record_error)<<'\n';
  Check(result.error==error&&!result.binding&&!result.principal&&!result.selected_catalog&&result.binding_version_uuid.is_nil()&&result.records.empty(),
      "refusal exposes no usable security/binding prefix");
}
void Governed(unsigned profile,bool indexed,unsigned variant=0,unsigned mga_scenario=0,bool configuration_phase=false,bool discover=false,unsigned epochs=0) {
  SecurityFixture f(profile,variant,indexed,mga_scenario!=0);
  if(epochs&&epochs<=4)SecurityFixture::BindingEpochsOnDevice(f.device,profile,f.receipt.relations[4],
      epochs==1?2:epochs==3?UINT64_MAX:1,epochs==2?2:epochs==4?UINT64_MAX:1);
  if(epochs==5)f.SelectedCatalogEpochs(2,4,19,23);
  if(mga_scenario)f.RetainedSuccessor(mga_scenario,configuration_phase);
  const db::NativeCatalogRelationMemoryLimits limits{Profile(profile),1,16,64,64,{4,16*static_cast<std::size_t>(sizes[profile])}};
  const auto amount=runtime_security::NativeRuntimeSecurityWorkspaceBytes(limits);Check(amount,"checked governed security backing size");
  LeafMemoryFixture memory(amount),source_memory(32*1024*1024);
  auto prepared=runtime_security::PrepareNativeRuntimeSecurityRead(limits,memory.memory,memory.binding);
  Check(prepared.ok(),"actual binary-bound security workspace preparation before guards");
  const std::array<disk::NativeFilespaceDevice,1> devices{{{Id(2),Profile(profile),&f.device}}};
  auto source=db::AcquireNativeFixedRootMemoryLease(Id(1),devices,Id(2),{f.budget,32*1024*1024},source_memory.memory,source_memory.binding);
  Check(source.ok(),"actual fixed-root lease over the in-place inventory");
  const auto read=[&](runtime_security::NativeRuntimeSecurityReadRequest r){
    allocation_budget=0;const auto result=runtime_security::ReadCommittedNativeRuntimeSecurityOnLease(*source.lease,r,prepared.workspace);
    const auto remaining=allocation_budget;allocation_budget=-1;
    Check(remaining==0,"native security observation makes no heap allocation under source/device guards");return result;
  };
  auto request=f.Request();
  if(discover){request.catalog_selection=runtime_security::NativeRuntimeCatalogSelection::discover_selected;
    request.configuration_relation={};request.security_relation={};}
  const auto writes=stage_writes,syncs=stage_syncs;
  reads=0;track_reads=true;const auto valid=read(request);track_reads=false;const auto read_count=reads;
  if(epochs&&epochs<=4){
    Empty(valid,epochs%2?SecurityError::binding_catalog_generation_mismatch:SecurityError::binding_security_epoch_mismatch);
    Check(valid.expected_generation==1&&valid.observed_generation==(epochs<=2?2:UINT64_MAX),
        "actual selected epoch mismatch preserves independent full-width expected/observed values");
  }else if(mga_scenario>=4&&mga_scenario!=9){Empty(valid,configuration_phase?SecurityError::binding_source_failure:
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
    Check(valid.selected_catalog&&valid.selected_catalog->catalog_generation==(epochs==5?2:1)&&valid.selected_catalog->security_epoch==(epochs==5?4:1)&&
        valid.selected_catalog->schema_epoch==(epochs==5?19:1)&&valid.selected_catalog->resource_epoch==(epochs==5?23:0)&&
        !valid.selected_catalog->root_page_uuid.is_nil(),
        "actual root epochs remain independent of unchanged binding birth epochs; absent resource authority is never replaced");
    Check(valid.principal->principal_uuid==Id(200)&&valid.principal->security_context_generation==(variant==8?UINT64_MAX:9)&&
        valid.principal->security_generation==(variant==8?UINT64_MAX:11)&&valid.principal->policy_generation==(variant==8?UINT64_MAX:13)&&valid.binding->policy_epoch==31&&
        valid.binding->provider_generation==37,"actual source generations remain distinct from configuration epochs");
    const auto find=[&](unsigned id)->const auto&{
      const auto found=std::find_if(valid.records.begin(),valid.records.end(),[&](const auto& r){return r.object_uuid==Id(id);});
      Check(found!=valid.records.end(),"actual retained security identity");return *found;
    };
    // 3a-2: compared as typed constants and identities, not text.
    Check(find(201).record.grant()&&find(201).record.grant()->effect==catalog::CatalogSecurityEffect::allow&&
        find(201).record.grant()->right==catalog::CatalogSecurityRight::CONNECT, "exact direct grant");
    Check(find(203).record.grant()&&find(203).record.grant()->member_uuid==Id(200)&&find(203).record.grant()->parent_uuid==Id(202),"membership graph retained without flattening");
    Check(find(204).record.grant()&&find(204).record.grant()->effect==catalog::CatalogSecurityEffect::deny&&find(204).record.grant()->target_uuid==Id(240),"targeted denial retained");
    Check(find(205).lifecycle==catalog::CatalogObjectLifecycle::archived&&find(205).status==catalog::CatalogObjectStatus::retired&&
        find(205).record.principal()&&!find(205).record.principal()->active&&
        find(205).retired_transaction_uuid==security_tx.value&&find(205).retired_local_transaction_id==1,"retirement remains real inventory-bound data");
    auto bad=request;bad.runtime_binding_generation=2;Empty(read(bad),SecurityError::binding_generation_mismatch);
    bad=request;bad.runtime_binding_uuid=Id(250);Empty(read(bad),SecurityError::binding_absent);
    bad=request;bad.database_uuid=Id(250);Empty(read(bad),SecurityError::invalid_request);
    // Expected comparisons remain mandatory in their original API, including
    // when the selected-discovery request itself carries no physical identity.
    bad=f.Request();bad.security_relation=bad.configuration_relation;Empty(read(bad),SecurityError::security_source_failure);
    bad=f.Request();bad.configuration_relation=bad.security_relation;Empty(read(bad),SecurityError::binding_source_failure);
    bad=f.Request();bad.configuration_relation={};Empty(read(bad),SecurityError::binding_source_failure);
    bad=request;bad.catalog_selection=static_cast<runtime_security::NativeRuntimeCatalogSelection>(255);
    Empty(read(bad),SecurityError::invalid_request);
    bad=request;bad.maximum_retained_image_bytes=1;Empty(read(bad),SecurityError::binding_source_failure);
    if(indexed){bad=f.Request();bad.security_relation.index_dependencies->descriptor_generation=2;Empty(read(bad),SecurityError::security_source_failure);}
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
#ifndef SB_NATIVE_RUNTIME_SECURITY_FIXTURE_WITHOUT_MAIN
int main()try {
  BackingBounds();
  for(bool discover:{false,true}){
    for(unsigned profile=0;profile<5;++profile){Governed(profile,false,0,0,false,discover);Governed(profile,true,0,0,false,discover);}
    for(unsigned variant=1;variant<=8;++variant)Governed(0,true,variant,0,false,discover);
    for(unsigned scenario=1;scenario<=9;++scenario)Governed(0,false,0,scenario,false,discover);
    Governed(0,false,0,5,true,discover);
    for(unsigned profile=0;profile<5;++profile){
      for(unsigned epochs=1;epochs<=4;++epochs)Governed(profile,false,0,0,false,discover,epochs);
      for(bool indexed:{false,true})Governed(profile,indexed,0,0,false,discover,5);
    }
  }
  std::cout<<"PASS native runtime security observation "<<checks<<" checks; no authentication or startup authority claimed\n";return 0;
}catch(const std::exception& error){allocation_budget=-1;std::cerr<<"FAIL "<<error.what()<<" checks="<<checks<<'\n';return 1;}
#endif
