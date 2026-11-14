// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
// Uses the real-file construction fixture, not a serving/SQL bootstrap claim.
#include "catalog_metric_config_materialization.hpp"
#include "metric_builtin_definitions.hpp"
#include "native_creation_catalog_layout.hpp"
#include "native_catalog_relation_lease_reader.hpp"
namespace {
// Four identity ranges of `stride` ids from `base`: object, row, default name,
// name vector. The defaults hold up to 1000 rows; a three-level tree needs a
// wider stride, above every other fixture identity (Id() carries 32 bits).
db::NativeCreationCatalogSeed DenseIndexedCatalogSeed(const db::NativeFilespaceInitializationRequest& request,unsigned count,
    unsigned base=30000,unsigned stride=1000) {
  namespace c=scratchbird::core::catalog;
  Check(count<=stride&&base+4*u64{stride}<=(u64{1}<<31),"dense fixture identity ranges stay distinct");
  const auto fixture=MetricSeed(request);
  const auto descriptor=c::DecodeCatalogMetricDescriptor(fixture[0][0].metadata.record.payload);
  Check(descriptor.ok(),"dense fixture retains exact typed labels");
  db::NativeCreationCatalogSeed seed;
  for(unsigned i=0;i<count;++i) {
    auto row=fixture[0][1];row.metadata.record.header.object_uuid={UuidKind::object,Id(base+i)};
    row.metadata.record.header.row_uuid={UuidKind::row,Id(base+stride+i)};
    row.metadata.default_name_uuid={UuidKind::object,Id(base+2*stride+i)};
    row.metadata.name_vector_uuid={UuidKind::object,Id(base+3*stride+i)};
    c::CatalogMetricLabelSchema labels{Id(base+i),1,descriptor.record->definition.labels,false,request.creator.transaction_uuid,1};
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
  Check(batch.ok()&&batch.definitions->size()==loaded.config->families.size(),"complete selected set materializes atomically");
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
  // A catalog relation is either one direct leaf or an index head over several
  // complete base leaves (NATIVE-CATALOG-RELATION-IMAGE-BINDING-001); there is
  // no unindexed multi-leaf form. The one-leaf construction refuses a set that
  // does not fit, before any I/O; the complete set is then built through the
  // indexed construction with the catalog's own index binding. Either way the
  // whole configured set must reopen and resolve exactly.
  const auto verify=[&](const db::NativeCreationWorkspaceResult& made,disk::FileDevice& device,
                        const std::filesystem::path& path,const auto& r,u64 budget,
                        const db::NativeCatalogRelationBinding& relation) {
      Check(device.Close().ok()&&device.Open(path.string(),disk::FileOpenMode::open_existing,disk::DatabaseFileOwnership::native_data_file).ok(),"complete batch reopens");
      const std::vector<disk::NativeFilespaceDevice> devices{{r.bootstrap.filespace_uuid,r.bootstrap.page_size_profile_uuid,&device}};
      const auto read=db::ReadNativeCommittedCatalogVersionsFromOpenDevices(r.bootstrap.database_uuid,devices,
          db::NativeCatalogSource::PageZero(made.receipt->filespace_uuid),2,1,relation,budget);
      Check(read.ok()&&read.rows.size()==4*loaded.config->families.size(),"all selected families and their dependencies survive reopen together");
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
  };
  unsigned whole_persisted=0;
  for(unsigned profile=0;profile<5;++profile) {
    const auto r=Request(profile);page_bytes=r.bootstrap.page_size_bytes;total_pages=r.total_pages;
    const auto budget=256*page_bytes;disk::FileDevice device;const auto path=fixture.Next();
    Check(device.Open(path.string(),disk::FileOpenMode::create_new,disk::DatabaseFileOwnership::native_data_file).ok(),"complete batch actual owned device");
    Arm();const auto made=db::InitializePopulatedNativeCreationWorkspaceOnOpenDevice(device,r,complete,budget,*fixture.issuer);Disarm();
    if(batch_bytes<=page_bytes-128-32-96) {
      Check(made.ok(),"all selected metric dependencies publish in one actual construction");
      verify(made,device,path,r,budget,{made.receipt->relations[0].object_uuid,{}});
      ++whole_persisted;
      continue;
    }
    Check(!made.ok()&&made.error==db::NativeCreationWorkspaceError::catalog_capacity_exceeded&&
        device.Size().ok()&&device.Size().size_bytes==0&&calls[write_call]==0,
        "complete batch overcapacity refuses one-leaf construction before any physical effects");
    const auto indexed_request=Request(profile,1024);const auto indexed_budget=256*page_bytes;
    total_pages=indexed_request.total_pages;
    const auto indexes=IndexedConstructionBindings();
    disk::FileDevice indexed_device;const auto indexed_path=fixture.Next();
    Check(indexed_device.Open(indexed_path.string(),disk::FileOpenMode::create_new,disk::DatabaseFileOwnership::native_data_file).ok(),"complete indexed batch actual owned device");
    const auto indexed=db::InitializeIndexedNativeCreationWorkspaceOnOpenDevice(
        indexed_device,indexed_request,complete,indexes,indexed_budget,*fixture.issuer);
    Check(indexed.ok(),"complete selected set publishes through multi-leaf indexed construction");
    if(!indexed.ok())continue;
    verify(indexed,indexed_device,indexed_path,indexed_request,indexed_budget,
           {indexes[0]->relation_uuid,indexes[0]->dependencies});
    ++whole_persisted;
  }
  Check(whole_persisted==5,"every actual supported profile persists the whole configured set");
  for(unsigned profile=0;profile<5;++profile) {
    const auto r=Request(profile,96);page_bytes=r.bootstrap.page_size_bytes;total_pages=r.total_pages;
    u64 coverage=(page_bytes-384)/128;
    while(((384+(coverage+1)/2+7)&~u64{7})+128*coverage>page_bytes)--coverage;
    
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
    Check(device.Open(path.string(),disk::FileOpenMode::create_new,disk::DatabaseFileOwnership::native_data_file).ok(),"indexed complete metric actual device");
    Arm();const auto made=db::InitializeIndexedNativeCreationWorkspaceOnOpenDevice(device,r,complete,indexes,budget,*fixture.issuer);Disarm();
    if(!made.ok())std::cerr<<"indexed creation profile="<<profile<<" error="<<static_cast<unsigned>(made.error)
        
        <<" writes="<<calls[write_call]<<" failed_page="<<made.failed_page_number.value_or(~u64{0})
        <<" catalog_error="<<(made.catalog_read_error?static_cast<unsigned>(*made.catalog_read_error):~0u)
        <<" relation_error="<<(made.catalog_relation_error?static_cast<unsigned>(*made.catalog_relation_error):~0u)
        <<" tree_error="<<static_cast<unsigned>(made.btree_error)<<'\n';
    Check(made.ok(),"complete configured batch fits actual indexed construction on every profile");
    Check(made.receipt->relations[0].page_type==0x200&&made.receipt->relation_uuids[0]==Id(20000)&&
        made.receipt->index_dependencies[0]==deps,"exact distinct relation/index custody in receipt");
    Check(device.Close().ok()&&device.Open(path.string(),disk::FileOpenMode::open_existing,disk::DatabaseFileOwnership::native_data_file).ok(),"indexed complete metric reopen");
    const std::vector<disk::NativeFilespaceDevice> devices{{r.bootstrap.filespace_uuid,r.bootstrap.page_size_profile_uuid,&device}};
    const auto read=db::ReadNativeCommittedCatalogVersionsFromOpenDevices(r.bootstrap.database_uuid,devices,
        db::NativeCatalogSource::PageZero(made.receipt->filespace_uuid),2,1,{Id(20000),deps},budget);
    Check(read.ok()&&read.rows.size()==4*loaded.config->families.size(),"all selected dependencies resolve from actual indexed selected graph");
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
        db::NativeCatalogSource::PageZero(made.receipt->filespace_uuid),2,1,{Id(20000),stale},budget);
    Check(!refused.ok(),"stale navigation binding cannot read indexed catalog");
    static_assert(![]<class R>() { return requires(const R& r) { r.checkpoint; }; }
                   .template operator()<db::NativeCreationWorkspaceReceipt>(),
                "a fixed-root create carries no checkpoint");
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
  
  // SB_NATIVE_BTREE_COMPRESSED_PAGE (owner Q37 = 1): SBBTP002 cells vary; the
  // create plans each at the largest a row-UUID tree cell can be: flags,
  // shared count and suffix count 1 each, the whole 16-byte key (a restart
  // cell), the version UUID 16, a creator of up to 10 LEB128 bytes, a compact
  // reference 16, and a quarter-byte jump-table share rounded up (SBBTP001's
  // fixed cell was 164).
  constexpr unsigned worst_cell=1+1+1+16+16+10+16+1;
  const unsigned root_capacity=(page_bytes-640)/worst_cell,child_capacity=(page_bytes-640-112)/worst_cell;
  // Three-level trees need wider identity ranges and a larger file.
  constexpr unsigned deep_base=70000,deep_stride=16000;
  constexpr u64 deep_pages=2048;
  // (R+1)*L rows fill a root over full leaves; one more row needs a branch
  // level (SB_CATALOG_BTREE_N_LEVEL). Those two cases need more pages and image
  // budget than the dense request, and identities past the dense seed's ranges.
  const unsigned two_level_maximum=(root_capacity+1)*child_capacity;
  const auto deep_request=Request(0,deep_pages);u64 deep_non_map_pages=0;
  Check(two_level_maximum+child_capacity+1<=deep_stride,"the deep identity ranges hold every deep row");
  // (R+1)*L+L+1 rows give an odd leaf count over two branches: an uneven split.
  for(unsigned count:{0u,1u,root_capacity,root_capacity+1,child_capacity*3+1,two_level_maximum,two_level_maximum+1,
      two_level_maximum+child_capacity+1}) {
    const bool deep=count>=two_level_maximum;
    const auto& boundary_request=deep?deep_request:dense_request;const unsigned row_base=deep?deep_base+deep_stride:31000;
    const u64 budget=(deep?deep_pages:256)*page_bytes;
    total_pages=boundary_request.total_pages;
    const auto seed=deep?DenseIndexedCatalogSeed(request,count,deep_base,deep_stride):dense_seed(count);
    disk::FileDevice device;const auto path=fixture.Next();
    Check(device.Open(path.string(),disk::FileOpenMode::create_new,disk::DatabaseFileOwnership::native_data_file).ok(),"actual boundary construction owner");
    Arm();const auto made=db::InitializeIndexedNativeCreationWorkspaceOnOpenDevice(device,boundary_request,seed,
        construction_indexes,budget,*fixture.issuer);Disarm();
    Check(made.ok(),"exact empty/root-full/split/multiple-child/branch-level indexed construction");
    if(count==two_level_maximum+1)deep_non_map_pages=made.receipt->total_pages-made.receipt->free_pages-made.receipt->map_pages;
    Check(device.Close().ok()&&device.Open(path.string(),disk::FileOpenMode::open_existing,disk::DatabaseFileOwnership::native_data_file).ok(),"actual indexed boundary reopen");
    const std::vector<disk::NativeFilespaceDevice> devices{{boundary_request.bootstrap.filespace_uuid,boundary_request.bootstrap.page_size_profile_uuid,&device}};
    const auto tree=page::ReadNativeBtreeTreeFromOpenDevices(boundary_request.bootstrap.database_uuid,devices,
        made.receipt->relations[0].page,construction_deps,budget);
    // Expected depth from the cell counts alone, not from the shape function:
    // the root alone, a root over leaves, or a root over branches over leaves.
    const unsigned leaves=count>root_capacity?(count+child_capacity-1)/child_capacity:0;
    const unsigned root_level=count<=root_capacity?0:leaves<=root_capacity+1?1:2;
    Check(tree.ok()&&tree.pages.front().page->tree_level==root_level,
        "independent whole-tree traversal proves actual split levels/fences/sibling reciprocity");
    unsigned branch_pages=0;std::size_t fewest_children=~std::size_t{0},most_children=0;
    for(const auto& node:tree.pages)if(node.page->header.page_type==0x201) {
      ++branch_pages;const auto children=node.page->cells.size()+1;
      fewest_children=std::min(fewest_children,children);most_children=std::max(most_children,children);
    }
    Check(tree.leaves.size()==std::max(leaves,1u)&&
        branch_pages==(root_level==2?(leaves+child_capacity)/(child_capacity+1):0),
        "actual leaf and 0x201 branch page counts for the tree depth");
    Check(!branch_pages||(fewest_children>=2&&most_children-fewest_children<=1&&
        most_children==(leaves+branch_pages-1)/branch_pages),
        "children spread evenly over branches: none empty, counts differ by at most one");
    unsigned seen=0;
    for(auto at:tree.leaves)for(const auto& cell:tree.pages[at].page->cells) {
      const auto id=Id(row_base+seen++);
      Check(cell.key.row_uuid==id&&cell.key.encoded_key==std::vector<byte>(id.bytes.begin(),id.bytes.end()),
          "binary ascending canonical row UUID keys despite reversed physical input");
    }
    Check(seen==count,"no indexed row is dropped or duplicated at a split boundary");
    const auto read=db::ReadNativeCommittedCatalogVersionsFromOpenDevices(boundary_request.bootstrap.database_uuid,devices,
        db::NativeCatalogSource::PageZero(made.receipt->filespace_uuid),2,1,{Id(20000),construction_deps},budget);
    Check(read.ok()&&read.rows.size()==count,"every split base row has actual committed native custody");
  }
  total_pages=dense_request.total_pages;
  // Case 4 was the two-level cap, (R+1)*L+1 rows. Construction now builds that
  // as a three-level tree, so the case keeps that tree and refuses it for
  // pages instead: the smallest file it fits, less one page. Its last branch
  // allocation is the one refused.
  Check(deep_non_map_pages>0,"three-level boundary construction measured its pages");
  u64 exhausted_total=deep_non_map_pages;
  while(exhausted_total<(exhausted_total+coverage-1)/coverage+deep_non_map_pages)++exhausted_total;
  const auto exhausted_request=Request(0,exhausted_total-1);
  {
    // The planner fills that smallest file exactly and is one page short of
    // the next smaller one, whose last allocation is the last branch page.
    const auto deep_seed=DenseIndexedCatalogSeed(request,two_level_maximum+1,deep_base,deep_stride);
    // The create has no inventory-format choice: it always writes the in-place
    // inventory and plans its header page (W4d-3).
    static_assert(![]<class R>() { return requires(const R& r) { r.inventory_format; }; }
        .template operator()<db::NativeFilespaceInitializationRequest>(),
        "a create request carries no inventory-format choice");
    const auto plan_for=[&](u64 total){return db::detail::PlanNativeCreationCatalogLayout(page_bytes,
        (total+coverage-1)/coverage,total,deep_pages*page_bytes,deep_seed,construction_indexes);};
    const auto fits=plan_for(exhausted_total),short_one=plan_for(exhausted_total-1);
    Check(fits.ok()&&fits.layout->image_count==exhausted_total&&fits.layout->relations[0].branches.size()==1&&
        short_one.error==db::detail::NativeCreationLayoutError::capacity_exceeded,
        "page exhaustion case is exactly one page short of its three-level tree");
  }
  for(unsigned fault=0;fault<6;++fault) {
    auto indices=construction_indexes;auto seed=dense_seed(root_capacity+1);const auto path=fixture.Next();disk::FileDevice device;
    Check(device.Open(path.string(),disk::FileOpenMode::create_new,disk::DatabaseFileOwnership::native_data_file).ok(),"invalid index construction owner");
    switch(fault) {
      case 0:indices[0]->relation_uuid=indices[0]->dependencies.index_uuid;break;
      case 1:indices[0]->dependencies.descriptor_generation=2;break;
      case 2:indices[0]->dependencies.dependency_map_sha256.fill(0);break;
      case 3:indices[0]->dependencies.key_profile_uuid={};break;
      case 4:seed=DenseIndexedCatalogSeed(request,two_level_maximum+1,deep_base,deep_stride);break;
      case 5:indices[0]->relation_uuid=dense_request.bootstrap.database_uuid;break;
    }
    // Case 4 gets the image budget of a file it would fit, so pages run out first.
    Arm();const auto refused=db::InitializeIndexedNativeCreationWorkspaceOnOpenDevice(device,fault==4?exhausted_request:dense_request,seed,
        indices,(fault==4?deep_pages:256)*page_bytes,*fixture.issuer);Disarm();
    Check(!refused.ok()&&!refused.receipt&&!calls[write_call]&&device.Size().ok()&&!device.Size().size_bytes,
        "invalid identity/generation/map or page exhaustion leaves owned file untouched");
    if(fault==4)Check(refused.error==db::NativeCreationWorkspaceError::catalog_capacity_exceeded,
        "a three-level tree one page short of the file is refused for capacity");
  }
  // The read bounds size exactly the tree create builds, at every page size
  // and for both tiers' row bounds. The planner sizes rows; it does not admit
  // identities, so one repeated row stands for each.
  for(unsigned profile=0;profile<5;++profile) {
    const u64 size=disk::kCanonicalFilespacePageProfiles[profile].page_size_bytes;
    for(const auto rows:{db::kNativeHostedCatalogRelationMaximumRows,db::kNativeCatalogScanRelationMaximumRows}) {
      db::NativeCreationCatalogSeed tier;tier[0].assign(rows,fixture_seed[0][1]);
      const auto plan=db::detail::PlanNativeCreationCatalogLayout(size,1,u64{1}<<20,(u64{1}<<20)*size,tier,construction_indexes);
      Check(plan.ok(),"tier row bound plans at every page size");
      const auto& layout=plan.layout->relations[0];
      u64 planned=1+layout.navigation.size();
      for(const auto& level:layout.branches)planned+=level.size();
      const auto limits=db::NativeCatalogNavigationLimitsFor(size,rows);
      Check(planned==limits.maximum_pages&&limits.maximum_retained_image_bytes==planned*size,
          "planned navigation pages equal the reader's navigation bound");
    }
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
    // Page zero, the maps, 13 fixed-root controls and the transaction header
    // page, then the catalog pages and two images (the drift-era graph had six
    // more control pages).
    // step4 (SB_NATIVE_COMMENT_RELATION): + one page, the head leaf of role 7
    // (the comments relation; unindexed here, only role 1 is), allocated after
    // the header page; the comment tree is not requested here.
    const u64 exact=(maps+15+1+base_pages+navigation+2)*page_bytes;
    const auto path=fixture.Next();disk::FileDevice device;
    Check(device.Open(path.string(),disk::FileOpenMode::create_new,disk::DatabaseFileOwnership::native_data_file).ok(),"exact indexed image budget owner");
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
      const auto r=Request(profile);page_bytes=r.bootstrap.page_size_bytes;total_pages=r.total_pages;
      const auto budget=256*page_bytes;auto path=fixture.Next();disk::FileDevice one_leaf_device;
      Check(one_leaf_device.Open(path.string(),disk::FileOpenMode::create_new,disk::DatabaseFileOwnership::native_data_file).ok(),"configured owned file");
      // A family that does not fit one leaf on this profile is refused by the
      // one-leaf construction before any I/O and built through the indexed
      // construction instead (NATIVE-CATALOG-RELATION-IMAGE-BINDING-001).
      const bool one_leaf=required<=page_bytes-128-32-96;
      Arm();auto created=db::InitializePopulatedNativeCreationWorkspaceOnOpenDevice(one_leaf_device,r,seed,budget,*fixture.issuer);Disarm();
      disk::FileDevice indexed_device;disk::FileDevice* owner=&one_leaf_device;
      const auto indexes=IndexedConstructionBindings();
      db::NativeCatalogRelationBinding relation{};
      if(one_leaf) {
        Check(created.ok(),"loaded policy definitions durably populate native construction");
        relation={created.receipt->relations[0].object_uuid,{}};
      } else {
        Check(!created.ok()&&created.error==db::NativeCreationWorkspaceError::catalog_capacity_exceeded&&
            one_leaf_device.Size().ok()&&one_leaf_device.Size().size_bytes==0,
            "a family over one leaf refuses one-leaf construction before any physical effects");
        path=fixture.Next();owner=&indexed_device;
        Check(indexed_device.Open(path.string(),disk::FileOpenMode::create_new,disk::DatabaseFileOwnership::native_data_file).ok(),"configured indexed owned file");
        const auto indexed_request=Request(profile,128);
        created=db::InitializeIndexedNativeCreationWorkspaceOnOpenDevice(indexed_device,indexed_request,seed,indexes,budget,*fixture.issuer);
        if(!created.ok())std::cerr<<"indexed family error="<<static_cast<int>(created.error)<<'\n';
        Check(created.ok(),"a family over one leaf publishes through indexed construction");
        relation={indexes[0]->relation_uuid,indexes[0]->dependencies};
      }
      if(!created.ok())continue;
      auto& device=*owner;
      Check(device.Close().ok()&&device.Open(path.string(),disk::FileOpenMode::open_existing,disk::DatabaseFileOwnership::native_data_file).ok(),"close and reopen configured native owner");
      const std::vector<disk::NativeFilespaceDevice> devices{{r.bootstrap.filespace_uuid,r.bootstrap.page_size_profile_uuid,&device}};
      // SB_NATIVE_FIXED_ROOT_READ (drift removal R3b3): the reopened file is
      // read from page zero's fixed roots; it has no checkpoint or selector.
      static_assert(![]<class R>() { return requires(const R& r) { r.checkpoint; }; }
                   .template operator()<db::NativeCreationWorkspaceReceipt>(),
                "a fixed-root create carries no checkpoint");
      const auto read=db::ReadNativeCommittedCatalogVersionsFromOpenDevices(r.bootstrap.database_uuid,devices,
          db::NativeCatalogSource::PageZero(r.bootstrap.filespace_uuid),2,1,relation,budget);
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
  Check(persisted==5*loaded.config->families.size(),"every selected real family persists across all five native profiles");
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
