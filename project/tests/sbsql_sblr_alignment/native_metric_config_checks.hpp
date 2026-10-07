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
  unsigned persisted=0;
  for(const auto& [family,policy]:loaded.config->families) {
    const auto found=std::find_if(definitions.begin(),definitions.end(),[&](const auto& d){return d.family==family;});
    Check(found!=definitions.end(),"configured exact family has compiled definition");
    auto made=c::MaterializeConfiguredLocalMetricDefinitions(*loaded.config,*found,binding,
        request.bootstrap.database_uuid,request.creator.transaction_uuid,1);
    Check(made.ok(),"actual builtin definition accepts exact configured policies");
    const auto& bundle=*made.definitions;
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
  Check(persisted==30,"all six real families persist across all five native profiles");
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
