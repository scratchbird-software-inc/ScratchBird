// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
// Uses the real-file construction fixture, not a serving/SQL bootstrap claim.
#include "catalog_metric_config_materialization.hpp"
#include "metric_builtin_definitions.hpp"
namespace {
db::NativeCreationCatalogSeed DenseIndexedCatalogSeed(const db::NativeFilespaceInitializationRequest& request,unsigned count) {
  namespace c=scratchbird::core::catalog;
  const auto fixture=MetricSeed(request);
  const auto descriptor=c::DecodeCatalogMetricDescriptor(fixture[0][0].metadata.record.payload);
  Check(descriptor.ok(),"dense fixture retains exact typed labels");
  db::NativeCreationCatalogSeed seed;
  for(unsigned i=0;i<count;++i) {
    auto row=fixture[0][1];row.metadata.record.header.object_uuid={UuidKind::object,Id(30000+i)};
    row.metadata.record.header.row_uuid={UuidKind::row,Id(31000+i)};
    row.metadata.default_name_uuid={UuidKind::object,Id(32000+i)};
    row.metadata.name_vector_uuid={UuidKind::object,Id(33000+i)};
    c::CatalogMetricLabelSchema labels{Id(30000+i),1,descriptor.record->definition.labels,false,request.creator.transaction_uuid,1};
    const auto encoded=c::EncodeCatalogMetricLabelSchema(labels);Check(encoded.ok(),"independent native label definition");
    row.metadata.record.payload.assign(encoded.bytes.begin(),encoded.bytes.end());seed[0].push_back(std::move(row));
  }
  std::reverse(seed[0].begin(),seed[0].end());return seed;
}
db::NativeCreationCatalogIndexes IndexedConstructionBindings() {
  db::NativeCreationCatalogIndexes indexes;
  page::NativeBtreeDependencies deps{Id(20001),1,1,Id(20002),Id(20003),Id(20004),{}};
  const std::array<byte,16> material{'r','o','w','_','u','u','i','d','_','r','a','w','1','6',0,1};
  const auto hash=scratchbird::core::hash::ComputeSha256Digest(material.data(),material.size());
  Check(hash.ok(),"retained construction fixture material digest");deps.dependency_map_sha256=hash.digest;
  indexes[0]=db::NativeCreationCatalogIndexBinding{Id(20000),deps};return indexes;
}
} // namespace
#include "native_named_creation_checks.hpp"
namespace {
void NativeMetricConfigChecks(const fs::path& directory) {
  namespace c=scratchbird::core::catalog;
  namespace m=scratchbird::core::metrics;
  namespace config=scratchbird::core::config;
  using E=c::CatalogMetricMaterializationError;
  const auto loaded=config::LoadMetricPolicyConfig(directory/"metric-policy-defaults.conf",
      directory/"metric-policy-families.conf");
  Check(loaded.ok(),"load distributed policy files for actual native materialization");
  const auto definitions=m::BuiltinMetricDescriptorDefinitions();
  const auto request=Request();const auto fixture_seed=MetricSeed(request);
  const auto fixture_descriptor=c::DecodeCatalogMetricDescriptor(fixture_seed[0][0].metadata.record.payload);
  Check(fixture_descriptor.ok(),"fixture binary bindings");
  const auto binding=fixture_descriptor.record->binding;
  Fixture fixture;
  std::vector<c::CatalogConfiguredMetricInput> batch_inputs;
  unsigned family_index=0;
  for(const auto& [family,policy]:loaded.config->families) {
    const auto found=std::find_if(definitions.begin(),definitions.end(),[&](const auto& d){return d.family==family;});
    Check(found!=definitions.end(),"complete configured batch uses actual compiled definitions");
    auto ids=binding;const unsigned base=1000+100*family_index++;
    ids.metric_uuid=Id(base);ids.label_schema_uuid=Id(base+1);
    ids.retention_policy_uuid=Id(base+2);ids.visibility_policy_uuid=Id(base+3);
    batch_inputs.push_back({&*found,ids});
  }
  const auto materialize_batch=[&](const auto& inputs) {
    return c::MaterializeConfiguredLocalMetricBatch(*loaded.config,inputs,
        request.bootstrap.database_uuid,request.creator.transaction_uuid,1);
  };
  const auto batch=materialize_batch(batch_inputs);
  Check(batch.ok()&&batch.definitions->size()==7,"complete selected set materializes atomically");
  db::NativeCreationCatalogSeed complete;
  for(unsigned i=0;i<batch.definitions->size();++i) {
    const auto& bundle=(*batch.definitions)[i];
    Check(bundle.labels.has_value(),"configured batch label dependencies retained");
    auto records=fixture_seed[0];
    const std::array ids{bundle.descriptor.binding.metric_uuid,bundle.descriptor.binding.label_schema_uuid,
        bundle.descriptor.binding.retention_policy_uuid,bundle.descriptor.binding.visibility_policy_uuid};
    for(unsigned j=0;j<records.size();++j) {
      auto& meta=records[j].metadata;
      meta.record.header.object_uuid={UuidKind::object,ids[j]};
      meta.record.header.row_uuid={UuidKind::row,Id(1020+100*i+j)};
      meta.default_name_uuid={UuidKind::object,Id(1040+100*i+j)};
      meta.name_vector_uuid={UuidKind::object,Id(1060+100*i+j)};
      meta.security_policy_uuid={UuidKind::object,ids[3]};
    }
    const auto replace=[&](unsigned at,const auto& encoded) {
      Check(encoded.ok(),"complete configured batch native codec");
      records[at].metadata.record.payload.assign(encoded.bytes.begin(),encoded.bytes.end());
    };
    replace(0,c::EncodeCatalogMetricDescriptor(bundle.descriptor));
    replace(1,c::EncodeCatalogMetricLabelSchema(*bundle.labels));
    replace(2,c::EncodeCatalogMetricRetentionPolicy(bundle.retention));
    replace(3,c::EncodeCatalogMetricVisibilityPolicy(bundle.visibility));
    for(auto& record:records)complete[0].push_back(std::move(record));
  }
  std::size_t batch_bytes=0;
  NativeNamedCreationChecks(complete);
  for(const auto& row:complete[0]) {
    const auto n=db::NativeCreationCatalogRowBytes(row);Check(n.has_value(),"complete batch structural size");
    batch_bytes+=*n;
  }
  unsigned whole_persisted=0;
  for(unsigned profile=0;profile<5;++profile) {
    const auto r=Request(profile);page_bytes=r.bootstrap.page_size_bytes;total_pages=r.total_pages;selector_page=17;
    const auto budget=256*page_bytes;disk::FileDevice device;const auto path=fixture.Next();
    Check(device.Open(path.string(),disk::FileOpenMode::create_new).ok(),"complete batch actual owned device");
    Arm();const auto made=db::InitializePopulatedNativeCreationWorkspaceOnOpenDevice(device,r,complete,budget,*fixture.issuer);Disarm();
    if(batch_bytes>page_bytes-128-32-96) {
      // Existing one-leaf construction is a known bound, not permission to
      // truncate families or claim ordinary database bootstrap completion.
      Check(!made.ok()&&made.error==db::NativeCreationWorkspaceError::catalog_capacity_exceeded&&
          device.Size().ok()&&device.Size().size_bytes==0&&calls[write_call]==0,
          "complete batch overcapacity refuses before any physical effects");
      continue;
    }
    Check(made.ok(),"all selected metric dependencies publish in one actual construction");
    Check(device.Close().ok()&&device.Open(path.string(),disk::FileOpenMode::open_existing).ok(),"complete batch reopens");
    const std::vector<disk::NativeFilespaceDevice> devices{{r.bootstrap.filespace_uuid,r.bootstrap.page_size_profile_uuid,&device}};
    const auto read=db::ReadNativeCommittedCatalogVersionsFromOpenDevices(r.bootstrap.database_uuid,devices,
        made.receipt->checkpoint,2,1,{made.receipt->relations[0].object_uuid,{}},budget);
    Check(read.ok()&&read.rows.size()==28,"all seven families and their dependencies survive reopen together");
    std::vector<c::CatalogMetricRowView> views;
    for(const auto& row:read.rows)views.push_back({&row.metadata,row.provisional});
    for(const auto& expected:*batch.definitions) {
      const auto resolved=c::ResolveLocalCatalogMetricBindings(views,expected.descriptor.binding.metric_uuid,1);
      Check(resolved.ok(),"complete batch resolves each actual retained family");
      const auto actual_descriptor=c::EncodeCatalogMetricDescriptor(resolved.binding->metric.descriptor);
      const auto expected_descriptor=c::EncodeCatalogMetricDescriptor(expected.descriptor);
      Check(actual_descriptor.ok()&&expected_descriptor.ok()&&actual_descriptor.bytes==expected_descriptor.bytes,
          "complete batch exact descriptor custody");
      const auto actual_retention=c::EncodeCatalogMetricRetentionPolicy(resolved.binding->metric.retention);
      const auto expected_retention=c::EncodeCatalogMetricRetentionPolicy(expected.retention);
      Check(actual_retention.ok()&&expected_retention.ok()&&actual_retention.bytes==expected_retention.bytes,
          "complete batch exact retained policy");
      const auto visibility=c::DecodeCatalogMetricVisibilityPolicy(resolved.binding->metric.visibility_policy.record.payload);
      Check(visibility.ok(),"complete batch retained visibility codec");
      const auto actual_visibility=c::EncodeCatalogMetricVisibilityPolicy(*visibility.record);
      const auto expected_visibility=c::EncodeCatalogMetricVisibilityPolicy(expected.visibility);
      Check(actual_visibility.ok()&&expected_visibility.ok()&&actual_visibility.bytes==expected_visibility.bytes,
          "complete batch exact visibility policy");
    }
    ++whole_persisted;
  }
  Check(whole_persisted>0,"at least one actual supported profile persists the whole configured set");
  for(unsigned profile=0;profile<5;++profile) {
    const auto r=Request(profile,96);page_bytes=r.bootstrap.page_size_bytes;total_pages=r.total_pages;
    u64 coverage=(page_bytes-384)/128;
    while(((384+(coverage+1)/2+7)&~u64{7})+128*coverage>page_bytes)--coverage;
    selector_page=(total_pages+coverage-1)/coverage+16;
    db::NativeCreationCatalogIndexes indexes;
    // Component-owned fixture references, not catalog admission or permission.
    // The map digest is computed from retained fixture material, not a readiness
    // flag. Production must admit the actual profile/map definitions.
    page::NativeBtreeDependencies deps{Id(20001),1,1,Id(20002),Id(20003),Id(20004),{}};
    const std::array<byte,16> key_definition{'r','o','w','_','u','u','i','d','_','r','a','w','1','6',0,1};
    const auto digest=scratchbird::core::hash::ComputeSha256Digest(key_definition.data(),key_definition.size());
    Check(digest.ok(),"actual retained component index material digest");deps.dependency_map_sha256=digest.digest;
    indexes[0]=db::NativeCreationCatalogIndexBinding{Id(20000),deps};
    const auto budget=256*page_bytes;const auto path=fixture.Next();disk::FileDevice device;
    Check(device.Open(path.string(),disk::FileOpenMode::create_new).ok(),"indexed complete metric actual device");
    Arm();const auto made=db::InitializeIndexedNativeCreationWorkspaceOnOpenDevice(device,r,complete,indexes,budget,*fixture.issuer);Disarm();
    if(!made.ok())std::cerr<<"indexed creation profile="<<profile<<" error="<<static_cast<unsigned>(made.error)
        <<" checkpoint="<<static_cast<unsigned>(made.checkpoint_error)<<" selection="<<static_cast<unsigned>(made.selection_error)
        <<" writes="<<calls[write_call]<<" failed_page="<<made.failed_page_number.value_or(~u64{0})
        <<" catalog_error="<<(made.catalog_read_error?static_cast<unsigned>(*made.catalog_read_error):~0u)
        <<" relation_error="<<(made.catalog_relation_error?static_cast<unsigned>(*made.catalog_relation_error):~0u)
        <<" tree_error="<<static_cast<unsigned>(made.btree_error)<<'\n';
    Check(made.ok(),"complete configured batch fits actual indexed construction on every profile");
    Check(made.receipt->relations[0].page_type==0x200&&made.receipt->relation_uuids[0]==Id(20000)&&
        made.receipt->index_dependencies[0]==deps,"exact distinct relation/index custody in receipt");
    Check(device.Close().ok()&&device.Open(path.string(),disk::FileOpenMode::open_existing).ok(),"indexed complete metric reopen");
    const std::vector<disk::NativeFilespaceDevice> devices{{r.bootstrap.filespace_uuid,r.bootstrap.page_size_profile_uuid,&device}};
    const auto read=db::ReadNativeCommittedCatalogVersionsFromOpenDevices(r.bootstrap.database_uuid,devices,
        made.receipt->checkpoint,2,1,{Id(20000),deps},budget);
    Check(read.ok()&&read.rows.size()==28,"all selected dependencies resolve from actual indexed selected graph");
    std::vector<c::CatalogMetricRowView> views;
    for(const auto& row:read.rows)views.push_back({&row.metadata,row.provisional});
    for(const auto& expected:*batch.definitions) {
      const auto resolved=c::ResolveLocalCatalogMetricBindings(views,expected.descriptor.binding.metric_uuid,1);
      Check(resolved.ok(),"every family binds after actual indexed reopen");
      const auto actual=c::EncodeCatalogMetricDescriptor(resolved.binding->metric.descriptor);
      const auto wanted=c::EncodeCatalogMetricDescriptor(expected.descriptor);
      Check(actual.ok()&&wanted.ok()&&actual.bytes==wanted.bytes,"indexed descriptor bytes persist exactly");
    }
    auto stale=deps;++stale.descriptor_generation;
    const auto refused=db::ReadNativeCommittedCatalogVersionsFromOpenDevices(r.bootstrap.database_uuid,devices,
        made.receipt->checkpoint,2,1,{Id(20000),stale},budget);
    Check(!refused.ok(),"stale navigation binding cannot read indexed catalog");
    const auto first=Read(device,selector_page,page_bytes),second=Read(device,selector_page+1,page_bytes);
    for(unsigned lost=0;lost<2;++lost) {
      const std::vector<byte> blank(page_bytes,0);
      Check(device.WriteAt((selector_page+lost)*page_bytes,blank.data(),blank.size()).ok()&&device.Sync().ok(),
          "persist interrupted indexed publication with one actual surviving selector");
      Check(device.Close().ok()&&device.Open(path.string(),disk::FileOpenMode::open_existing).ok(),"reopen interrupted indexed owner");
      Arm();const auto recovered=db::RecoverIndexedNativeCreationWorkspaceSelectionOnOpenDevice(device,
          {r.bootstrap.database_uuid,r.bootstrap.filespace_uuid,r.bootstrap.page_size_profile_uuid},r.operation_uuid,
          complete,indexes,budget);Disarm();
      if(!recovered.ok())std::cerr<<"indexed recovery profile="<<profile<<" error="<<static_cast<unsigned>(recovered.error)<<'\n';
      Check(recovered.ok()&&recovered.repaired_slots==(1u<<lost)&&calls[write_call]==1,
          "complete original indexed graph repairs exactly the lost selector without physical-page allocation or identity reissuance");
      Check(recovered.receipt->relation_uuids[0]==Id(20000)&&recovered.receipt->index_dependencies[0]==deps&&
          recovered.receipt->catalog_rows[0]==28&&recovered.receipt->checkpoint_sha256==made.receipt->checkpoint_sha256&&
          Read(device,selector_page,page_bytes)==first&&Read(device,selector_page+1,page_bytes)==second,
          "indexed recovery preserves all original bindings, root digest and exact selector bytes");
      Arm();const auto retry=db::RecoverIndexedNativeCreationWorkspaceSelectionOnOpenDevice(device,
          {r.bootstrap.database_uuid,r.bootstrap.filespace_uuid,r.bootstrap.page_size_profile_uuid},r.operation_uuid,
          complete,indexes,budget);Disarm();
      Check(retry.ok()&&!retry.repaired_slots&&!calls[write_call],"indexed recovery retry is verified and physically idempotent");
    }
    const auto crash_path=fixture.Next();const auto child=fork();Check(child>=0,"fork actual indexed creation interruption");
    if(child==0) {
      fixture.ResetIssuer();disk::FileDevice interrupted;
      if(!interrupted.Open(crash_path.string(),disk::FileOpenMode::create_new).ok())_exit(88);
      Arm();stop_before_second_selector=true;
      (void)db::InitializeIndexedNativeCreationWorkspaceOnOpenDevice(interrupted,r,complete,indexes,budget,*fixture.issuer);
      _exit(89);
    }
    int status=0;Check(waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==86,
        "indexed constructor process dies only after first selector durability/readback");
    disk::FileDevice interrupted;
    Check(interrupted.Open(crash_path.string(),disk::FileOpenMode::open_existing).ok(),"new process owns actual interrupted indexed file");
    const auto surviving=Read(interrupted,selector_page,page_bytes);
    const auto recovery=db::RecoverIndexedNativeCreationWorkspaceSelectionOnOpenDevice(interrupted,
        {r.bootstrap.database_uuid,r.bootstrap.filespace_uuid,r.bootstrap.page_size_profile_uuid},r.operation_uuid,
        complete,indexes,budget);
    Check(recovery.ok()&&recovery.repaired_slots==2&&Read(interrupted,selector_page,page_bytes)==surviving,
        "actual indexed process-loss recovery preserves original surviving publication");
  }
  for(unsigned which=0;which<8;++which) {
    auto inputs=batch_inputs;auto invalid_definition=*inputs.back().definition;
    switch(which) {
      case 0:inputs.pop_back();break;
      case 1:inputs.back().definition=nullptr;break;
      case 2:inputs.back().binding.metric_uuid=inputs.front().binding.label_schema_uuid;break;
      case 3:inputs.back().binding.visibility_policy_uuid=inputs.front().binding.retention_policy_uuid;break;
      case 4:inputs.back().definition=inputs.front().definition;break;
      case 5:invalid_definition.family="not_selected";inputs.back().definition=&invalid_definition;break;
      case 6:inputs.back().binding.descriptor_generation=2;break;
      case 7:invalid_definition.help.assign(16385,'x');inputs.back().definition=&invalid_definition;break;
    }
    allocations=0;allocation_fault=1;count_allocations=true;
    const auto refused=materialize_batch(inputs);count_allocations=false;allocation_fault=0;
    Check(!refused.ok()&&!refused.definitions&&!allocations,"malformed final batch input refuses before any owning copy");
  }
  // Independent construction boundary controls: row identity is a canonical
  // binary key, not a caller-supplied lexical position or declared tree height.
  const auto dense_seed=[&](unsigned count) {
    // Deliberately reverse physical input order: tree order must derive from
    // actual binary row identities, not input order or decimal text formatting.
    return DenseIndexedCatalogSeed(request,count);
  };
  db::NativeCreationCatalogIndexes construction_indexes;
  page::NativeBtreeDependencies construction_deps{Id(20001),1,1,Id(20002),Id(20003),Id(20004),{}};
  const std::array<byte,16> key_material{'r','o','w','_','u','u','i','d','_','r','a','w','1','6',0,1};
  const auto key_hash=scratchbird::core::hash::ComputeSha256Digest(key_material.data(),key_material.size());
  Check(key_hash.ok(),"retained boundary fixture map material");construction_deps.dependency_map_sha256=key_hash.digest;
  construction_indexes[0]=db::NativeCreationCatalogIndexBinding{Id(20000),construction_deps};
  const auto dense_request=Request(0,128);page_bytes=dense_request.bootstrap.page_size_bytes;total_pages=dense_request.total_pages;
  u64 coverage=(page_bytes-384)/128;
  while(((384+(coverage+1)/2+7)&~u64{7})+128*coverage>page_bytes)--coverage;
  selector_page=(total_pages+coverage-1)/coverage+16;
  const unsigned root_capacity=(page_bytes-640)/164,child_capacity=(page_bytes-640-112)/164;
  for(unsigned count:{0u,1u,root_capacity,root_capacity+1,child_capacity*3+1}) {
    const auto seed=dense_seed(count);disk::FileDevice device;const auto path=fixture.Next();
    Check(device.Open(path.string(),disk::FileOpenMode::create_new).ok(),"actual boundary construction owner");
    Arm();const auto made=db::InitializeIndexedNativeCreationWorkspaceOnOpenDevice(device,dense_request,seed,
        construction_indexes,256*page_bytes,*fixture.issuer);Disarm();
    Check(made.ok(),"exact empty/root-full/split/multiple-child indexed construction");
    Check(device.Close().ok()&&device.Open(path.string(),disk::FileOpenMode::open_existing).ok(),"actual indexed boundary reopen");
    const std::vector<disk::NativeFilespaceDevice> devices{{dense_request.bootstrap.filespace_uuid,dense_request.bootstrap.page_size_profile_uuid,&device}};
    const auto tree=page::ReadNativeBtreeTreeFromOpenDevices(dense_request.bootstrap.database_uuid,devices,
        made.receipt->relations[0].page,construction_deps,256*page_bytes);
    Check(tree.ok()&&tree.pages.front().page->tree_level==(count>root_capacity?1:0),
        "independent whole-tree traversal proves actual split levels/fences/sibling reciprocity");
    unsigned seen=0;
    for(auto at:tree.leaves)for(const auto& cell:tree.pages[at].page->cells) {
      const auto id=Id(31000+seen++);
      Check(cell.key.row_uuid==id&&cell.key.encoded_key==std::vector<byte>(id.bytes.begin(),id.bytes.end()),
          "binary ascending canonical row UUID keys despite reversed physical input");
    }
    Check(seen==count,"no indexed row is dropped or duplicated at a split boundary");
    const auto read=db::ReadNativeCommittedCatalogVersionsFromOpenDevices(dense_request.bootstrap.database_uuid,devices,
        made.receipt->checkpoint,2,1,{Id(20000),construction_deps},256*page_bytes);
    Check(read.ok()&&read.rows.size()==count,"every split base row has actual committed native custody");
    const auto original_selector=Read(device,selector_page+1,page_bytes);
    const std::vector<byte> blank(page_bytes,0);
    Check(device.WriteAt((selector_page+1)*page_bytes,blank.data(),blank.size()).ok()&&device.Sync().ok(),"interrupt actual split-tree publication");
    const auto recovered=db::RecoverIndexedNativeCreationWorkspaceSelectionOnOpenDevice(device,
        {dense_request.bootstrap.database_uuid,dense_request.bootstrap.filespace_uuid,dense_request.bootstrap.page_size_profile_uuid},
        dense_request.operation_uuid,seed,construction_indexes,256*page_bytes);
    Check(recovered.ok()&&recovered.repaired_slots==2&&Read(device,selector_page+1,page_bytes)==original_selector,
        "original split/fence/sibling graph must fully reconstruct before selector repair");
  }
  for(unsigned fault=0;fault<6;++fault) {
    auto indices=construction_indexes;auto seed=dense_seed(root_capacity+1);const auto path=fixture.Next();disk::FileDevice device;
    Check(device.Open(path.string(),disk::FileOpenMode::create_new).ok(),"invalid index construction owner");
    switch(fault) {
      case 0:indices[0]->relation_uuid=indices[0]->dependencies.index_uuid;break;
      case 1:indices[0]->dependencies.descriptor_generation=2;break;
      case 2:indices[0]->dependencies.dependency_map_sha256.fill(0);break;
      case 3:indices[0]->dependencies.key_profile_uuid={};break;
      case 4:seed=dense_seed((root_capacity+1)*child_capacity+1);break;
      case 5:indices[0]->relation_uuid=dense_request.bootstrap.database_uuid;break;
    }
    Arm();const auto refused=db::InitializeIndexedNativeCreationWorkspaceOnOpenDevice(device,dense_request,seed,
        indices,256*page_bytes,*fixture.issuer);Disarm();
    Check(!refused.ok()&&!refused.receipt&&!calls[write_call]&&device.Size().ok()&&!device.Size().size_bytes,
        "invalid identity/generation/map or tree capacity leaves owned file untouched");
  }
  {
    const auto seed=dense_seed(root_capacity+1);
    const u64 base_capacity=page_bytes-128-32-page::kRowDataPageBodyHeaderBytes;
    u64 base_pages=1,remaining=base_capacity;
    for(const auto& row:seed[0]) {
      const auto bytes=db::NativeCreationCatalogRowBytes(row);Check(bytes&&*bytes<=base_capacity,"exact budget row fit");
      if(*bytes>remaining){++base_pages;remaining=base_capacity;}remaining-=*bytes;
    }
    const u64 maps=(total_pages+coverage-1)/coverage;
    const u64 navigation=(seed[0].size()+child_capacity-1)/child_capacity;
    const u64 exact=(maps+20+base_pages+navigation+2)*page_bytes;
    const auto path=fixture.Next();disk::FileDevice device;
    Check(device.Open(path.string(),disk::FileOpenMode::create_new).ok(),"exact indexed image budget owner");
    Arm();auto refused=db::InitializeIndexedNativeCreationWorkspaceOnOpenDevice(device,dense_request,seed,
        construction_indexes,exact-1,*fixture.issuer);Disarm();
    Check(!refused.ok()&&refused.error==db::NativeCreationWorkspaceError::resource_exhausted&&
        !calls[write_call]&&device.Size().ok()&&!device.Size().size_bytes,
        "one-short complete graph budget refuses before physical effects");
    const auto accepted=db::InitializeIndexedNativeCreationWorkspaceOnOpenDevice(device,dense_request,seed,
        construction_indexes,exact,*fixture.issuer);
    Check(accepted.ok(),"exact indexed image budget performs the complete graph and actual admission");
  }
  allocations=0;allocation_fault=0;count_allocations=true;
  const auto counted_batch=materialize_batch(batch_inputs);count_allocations=false;
  const auto batch_allocations=allocations;
  Check(counted_batch.ok()&&batch_allocations>0,"complete batch allocation baseline");
  for(unsigned at=1;at<=batch_allocations;++at) {
    allocations=0;allocation_fault=at;count_allocations=true;
    const auto refused=materialize_batch(batch_inputs);count_allocations=false;allocation_fault=0;
    Check(!refused.ok()&&!refused.definitions&&refused.error==E::resource_exhausted,
        "every complete-batch allocation failure returns no partial definitions");
    Check(materialize_batch(batch_inputs).ok(),"complete batch retry preserves borrowed inputs");
  }
  unsigned persisted=0;
  for(const auto& [family,policy]:loaded.config->families) {
    const auto found=std::find_if(definitions.begin(),definitions.end(),[&](const auto& d){return d.family==family;});
    Check(found!=definitions.end(),"configured exact family has compiled definition");
    auto made=c::MaterializeConfiguredLocalMetricDefinitions(*loaded.config,*found,binding,
        request.bootstrap.database_uuid,request.creator.transaction_uuid,1);
    Check(made.ok(),"actual builtin definition accepts exact configured policies");
    const auto& bundle=*made.definitions;
    if(family=="sb_mga_row_versions_reclaimed_total") {
      Check(bundle.descriptor.definition.unit==m::MetricUnit::versions &&
          bundle.descriptor.definition.value_type==m::MetricScalarType::uint64 &&
          bundle.descriptor.definition.aliases==std::vector<std::string>{"sys.metrics.mga.cleanup.versions_pruned_total"} &&
          bundle.descriptor.definition.labels.size()==1 &&
          bundle.descriptor.definition.labels[0].key=="database_uuid" &&
          bundle.descriptor.definition.labels[0].required &&
          bundle.descriptor.definition.labels[0].value_type==m::MetricLabelType::system_uuid,
          "approved pruning metric definition uses versions and a required binary database identity");
    }
    Check(bundle.labels.has_value()&&bundle.visibility.sensitive_read_right.empty()&&
        bundle.descriptor.definition.security_family==policy.read_right,"no sensitive grant or rewritten security requirement");
    auto seed=fixture_seed;
    const auto replace=[&](unsigned at,const auto& encoded){
      Check(encoded.ok(),"native configured payload codec");
      seed[0][at].metadata.record.payload.assign(encoded.bytes.begin(),encoded.bytes.end());
    };
    replace(0,c::EncodeCatalogMetricDescriptor(bundle.descriptor));
    replace(1,c::EncodeCatalogMetricLabelSchema(*bundle.labels));
    replace(2,c::EncodeCatalogMetricRetentionPolicy(bundle.retention));
    replace(3,c::EncodeCatalogMetricVisibilityPolicy(bundle.visibility));
    std::size_t required=0;
    for(const auto& row:seed[0]){const auto n=db::NativeCreationCatalogRowBytes(row);Check(n.has_value(),"configured seed structural size");required+=*n;}
    for(unsigned profile=0;profile<5;++profile) {
      const auto r=Request(profile);page_bytes=r.bootstrap.page_size_bytes;total_pages=r.total_pages;selector_page=17;
      const auto budget=256*page_bytes;const auto path=fixture.Next();disk::FileDevice device;
      Check(device.Open(path.string(),disk::FileOpenMode::create_new).ok(),"configured owned file");
      Check(required<=page_bytes-128-32-96,"one complete configured family must fit every supported profile");
      Arm();const auto created=db::InitializePopulatedNativeCreationWorkspaceOnOpenDevice(device,r,seed,budget,*fixture.issuer);Disarm();
      Check(created.ok(),"loaded policy definitions durably populate native construction");
      Check(device.Close().ok()&&device.Open(path.string(),disk::FileOpenMode::open_existing).ok(),"close and reopen configured native owner");
      const std::vector<disk::NativeFilespaceDevice> devices{{r.bootstrap.filespace_uuid,r.bootstrap.page_size_profile_uuid,&device}};
      const auto selected=db::ReadNativeBoundCheckpointSelectionFromOpenDevices(r.bootstrap.database_uuid,devices,r.bootstrap.filespace_uuid,budget);
      Check(selected.ok(),"configured native selection after reopen");
      const auto& cp=*selected.checkpoint_inventory.checkpoint;
      const disk::FilespaceRootReference checkpoint{9,0x300,cp.header.filespace_uuid,cp.header.page_number,cp.header.page_generation,cp.header.page_size_profile_uuid,cp.object_uuid};
      const auto read=db::ReadNativeCommittedCatalogVersionsFromOpenDevices(r.bootstrap.database_uuid,devices,checkpoint,2,1,{created.receipt->relations[0].object_uuid,{}},budget);
      Check(read.ok()&&read.rows.size()==4,"actual native configured rows after reopen");
      std::vector<c::CatalogMetricRowView> views;
      for(const auto& row:read.rows)views.push_back({&row.metadata,row.provisional});
      const auto resolved=c::ResolveLocalCatalogMetricBindings(views,binding.metric_uuid,1);
      Check(resolved.ok(),"reopened configured dependency graph");
      const auto reopened_descriptor=c::EncodeCatalogMetricDescriptor(resolved.binding->metric.descriptor);
      const auto expected_descriptor=c::EncodeCatalogMetricDescriptor(bundle.descriptor);
      Check(reopened_descriptor.ok()&&expected_descriptor.ok()&&
          reopened_descriptor.bytes==expected_descriptor.bytes,
          "native descriptor unit, aliases and binary bindings persist exactly");
      Check(c::EncodeCatalogMetricRetentionPolicy(resolved.binding->metric.retention).bytes==
          c::EncodeCatalogMetricRetentionPolicy(bundle.retention).bytes,"all loaded retention values and binary identities persist exactly");
      const auto visibility=c::DecodeCatalogMetricVisibilityPolicy(resolved.binding->metric.visibility_policy.record.payload);
      Check(visibility.ok()&&c::EncodeCatalogMetricVisibilityPolicy(*visibility.record).bytes==
          c::EncodeCatalogMetricVisibilityPolicy(bundle.visibility).bytes,"configured read right and redaction persist exactly");
      auto changed=*loaded.config;changed.families.at(family).retention.raw_retention_seconds+=86400;
      const auto replacement=c::MaterializeConfiguredLocalMetricDefinitions(changed,*found,binding,
          r.bootstrap.database_uuid,r.creator.transaction_uuid,1);
      Check(replacement.ok()&&replacement.definitions->retention.policy.raw_retention_seconds!=
          resolved.binding->metric.retention.policy.raw_retention_seconds,"editing defaults never mutates reopened native policy");
      ++persisted;
    }
  }
  Check(persisted==35,"all seven real families persist across all five native profiles");
  auto definition=fixture_descriptor.record->definition;definition.family="sb_tx_begin_total";
  const auto materialize=[&](const auto& d,const auto& b,const config::MetricPolicyDefinition* governing=nullptr){
    return c::MaterializeConfiguredLocalMetricDefinitions(*loaded.config,d,b,
        request.bootstrap.database_uuid,request.creator.transaction_uuid,1,governing);
  };
  Check(materialize(definition,binding).ok(),"materialization baseline");
  for(unsigned which=0;which<12;++which){auto d=definition;auto b=binding;
    switch(which){
      case 0:d.family="not_selected";break;
      case 1:d.security_family="OBS_METRICS_READ_FAMILY";break;
      case 2:d.cluster_only=true;break;
      case 3:b.metric_uuid={};break;
      case 4:b.retention_policy_uuid=b.metric_uuid;break;
      case 5:b.descriptor_generation=2;break;
      case 6:b.label_schema_generation=0;break;
      case 7:b.label_schema_uuid={};b.label_schema_generation=0;break;
      case 8:d.labels[0].key.clear();break;
      case 9:b.visibility_policy_uuid=request.bootstrap.database_uuid;break;
      case 10:b.metric_uuid.bytes[6]=0x40;break;
      case 11:b.rate_source_counter_uuid=b.metric_uuid;break;
    }
    const auto rejected=materialize(d,b);Check(!rejected.ok()&&!rejected.definitions,"invalid materialization never returns a partial bundle");
  }
  auto governing=loaded.config->families.at(definition.family);governing.retention.raw_retention_seconds=123456;
  const auto governed=materialize(definition,binding,&governing);
  Check(governed.ok()&&governed.definitions->retention.policy.raw_retention_seconds==123456,"whole governing policy takes precedence");
  governing.retention.evidence_required=false;
  Check(!materialize(definition,binding,&governing).ok(),"invalid governing policy never falls back");
  auto no_labels=definition;no_labels.labels.clear();auto no_binding=binding;no_binding.label_schema_uuid={};no_binding.label_schema_generation=0;
  const auto absent=materialize(no_labels,no_binding);Check(absent.ok()&&!absent.definitions->labels,"label-free definition preserves absent schema");
  for(unsigned which=0;which<9;++which) {
    auto d=definition;auto p=loaded.config->families.at(d.family);
    switch(which) {
      case 0:d.help.assign(16385,'x');break;
      case 1:d.labels.resize(1025);break;
      case 2:d.enum_values.resize(4097);break;
      case 3:p.retention.policy_name.assign(4097,'x');break;
      case 4:p.retention.rollup_grains.resize(5);break;
      case 5:d.aliases.assign(40,std::string(4096,'x'));break;
      case 6:d.min_value=std::string(131073,'x');break;
      case 7:d.max_value=std::string(131073,'x');break;
      case 8:d.histogram_buckets={std::string(131073,'x')};break;
    }
    allocations=0;allocation_fault=1;count_allocations=true;
    const auto refused=materialize(d,binding,&p);count_allocations=false;allocation_fault=0;
    Check(!refused.ok()&&!refused.definitions&&refused.error==E::invalid_definition&&!allocations,
        "oversized borrowed definition/governing policy refused before any owned copy");
  }
  count_allocations=true;allocations=0;allocation_fault=0;
  const auto baseline=materialize(definition,binding);count_allocations=false;
  const auto count=allocations;Check(baseline.ok()&&count>0,"materialization allocation baseline");
  for(unsigned at=1;at<=count;++at){
    allocations=0;allocation_fault=at;count_allocations=true;
    const auto result=materialize(definition,binding);count_allocations=false;allocation_fault=0;
    Check(!result.ok()&&!result.definitions&&result.error==E::resource_exhausted,"every allocation failure remains atomic");
    Check(materialize(definition,binding).ok(),"retry after allocation failure preserves original inputs");
  }
  std::error_code error;fs::remove_all(fixture.root,error);Check(!error&&!fs::exists(fixture.root),"configured native fixtures removed after handles close");
  std::cout<<"configured native policies persisted="<<persisted<<" allocation sites="<<count<<'\n';
}
}
