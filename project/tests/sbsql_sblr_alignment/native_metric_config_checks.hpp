// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
// Uses the real-file construction fixture, not a serving/SQL bootstrap claim.
#include "catalog_metric_config_materialization.hpp"
#include "metric_builtin_definitions.hpp"
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
