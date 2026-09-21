// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
// Dependency selection tests use decoded-record fixtures, not native-I/O or
// authorization mocks. The existing independent descriptor regression runs too.
#define main MetricDescriptorBindingBaseMain
#include "catalog_metric_descriptor_test.cpp"
#undef main
#include "catalog_metric_binding.hpp"

namespace {
using BE=c::CatalogMetricBindingError;
struct Fixture {
  c::CatalogMetricDescriptor descriptor=Descriptor();
  std::vector<c::CatalogMetadataVersion> rows;
};
Fixture Catalog() {
  Fixture f;f.rows.push_back(Metadata(f.descriptor));
  const auto& d=f.descriptor;const auto& b=d.binding;
  c::CatalogMetricLabelSchema labels;
  labels.label_schema_uuid=b.label_schema_uuid;labels.generation=b.label_schema_generation;
  labels.labels=d.definition.labels;labels.origin_transaction_uuid=d.origin_transaction_uuid;
  labels.origin_local_transaction_id=d.origin_local_transaction_id;
  auto encoded_labels=c::EncodeCatalogMetricLabelSchema(labels);
  Check(encoded_labels.ok(),"label dependency fixture encoding failed");
  auto lm=Metadata(d);lm.record.header.kind=c::CatalogRecordKind::metric_label_schema;
  lm.record.header.object_uuid={p::UuidKind::object,b.label_schema_uuid};lm.definition_version=b.label_schema_generation;
  lm.object_subtype="metric_label_schema";lm.record.payload.assign(encoded_labels.bytes.begin(),encoded_labels.bytes.end());
  lm.record.header.row_uuid=Id(p::UuidKind::row,50);
  lm.default_name_uuid=Id(p::UuidKind::object,51);lm.name_vector_uuid=Id(p::UuidKind::object,52);
  f.rows.push_back(std::move(lm));
  c::CatalogMetricRetentionPolicy policy;
  policy.policy.policy_uuid=b.retention_policy_uuid;policy.policy.generation=b.retention_policy_generation;
  policy.policy.policy_name="retention-annotation";policy.origin_transaction_uuid=d.origin_transaction_uuid;
  policy.origin_local_transaction_id=d.origin_local_transaction_id;
  auto encoded_policy=c::EncodeCatalogMetricRetentionPolicy(policy);
  Check(encoded_policy.ok(),"retention dependency fixture encoding failed");
  auto pm=Metadata(d);pm.record.header.kind=c::CatalogRecordKind::policy;
  pm.record.header.object_uuid={p::UuidKind::object,b.retention_policy_uuid};pm.definition_version=b.retention_policy_generation;
  pm.object_subtype="metric_retention";pm.record.payload.assign(encoded_policy.bytes.begin(),encoded_policy.bytes.end());
  pm.record.header.row_uuid=Id(p::UuidKind::row,53);
  pm.default_name_uuid=Id(p::UuidKind::object,54);pm.name_vector_uuid=Id(p::UuidKind::object,55);
  f.rows.push_back(std::move(pm));
  // The selector deliberately does not interpret a security policy. Opaque
  // fixture bytes verify preservation only; they are no executable grant.
  auto vm=Metadata(d);vm.record.header.kind=c::CatalogRecordKind::policy;
  vm.record.header.object_uuid={p::UuidKind::object,b.visibility_policy_uuid};vm.definition_version=b.visibility_policy_generation;
  vm.object_subtype="security_owner_fixture";vm.record.payload=std::string(16,char(0xa5));
  vm.record.header.row_uuid=Id(p::UuidKind::row,56);
  vm.default_name_uuid=Id(p::UuidKind::object,57);vm.name_vector_uuid=Id(p::UuidKind::object,58);
  f.rows.push_back(std::move(vm));return f;
}
std::vector<c::CatalogMetricRowView> Views(const Fixture& f) {
  std::vector<c::CatalogMetricRowView> views;
  for(const auto& row:f.rows)views.push_back({&row,false});
  return views;
}
c::CatalogMetricBindingResult Resolve(const Fixture& f) {
  return c::ResolveLocalCatalogMetricBindings(Views(f),f.descriptor.binding.metric_uuid,f.descriptor.binding.descriptor_generation);
}
void BindingRefused(const c::CatalogMetricBindingResult& r,BE error) {
  Check(!r.ok()&&!r.binding&&r.error==error,"wrong dependency failure or partial bundle exposed");
}
void DependencySelection() {
  auto f=Catalog();const auto expected=Golden(f.descriptor);const auto before_policy=f.rows[3].record.payload;
  auto result=Resolve(f);
  Check(result.ok()&&Golden(result.binding->metric.descriptor)==expected&&!result.binding->source_counter,"valid native definition dependency set refused");
  if(result.ok())Check(result.binding->metric.labels->labels.size()==f.descriptor.definition.labels.size()&&
      result.binding->metric.retention.policy.policy_uuid==f.descriptor.binding.retention_policy_uuid&&
      result.binding->metric.visibility_policy.record.payload==before_policy,"dependencies substituted or policy interpreted");
  if(result.ok())Check(result.binding->metric.descriptor_metadata.record.payload==f.rows[0].record.payload&&
      result.binding->metric.descriptor_metadata.owner_uuid.value==f.rows[0].owner_uuid.value&&
      result.binding->metric.label_metadata->record.payload==f.rows[1].record.payload&&
      result.binding->metric.retention_metadata.record.payload==f.rows[2].record.payload,
      "selection lost common ownership or retained definitions");
  std::reverse(f.rows.begin(),f.rows.end());result=Resolve(f);
  Check(result.ok()&&Golden(result.binding->metric.descriptor)==expected,"catalog order became dependency authority");
  BindingRefused(c::ResolveLocalCatalogMetricBindings(Views(f),{},1),BE::invalid_request);
  BindingRefused(c::ResolveLocalCatalogMetricBindings(Views(f),f.descriptor.binding.metric_uuid,0),BE::invalid_request);
  for(unsigned version=0;version<16;++version)if(version!=7){
    auto id=f.descriptor.binding.metric_uuid;id.bytes[6]=version<<4;
    BindingRefused(c::ResolveLocalCatalogMetricBindings(Views(f),id,1),BE::invalid_request);
  }
  BindingRefused(c::ResolveLocalCatalogMetricBindings(Views(f),Id(p::UuidKind::object,99).value,1),BE::missing_object);
  for(unsigned index=0;index<4;++index){
    f=Catalog();f.rows.erase(f.rows.begin()+index);BindingRefused(Resolve(f),BE::missing_object);
    f=Catalog();f.rows.push_back(f.rows[index]);BindingRefused(Resolve(f),BE::duplicate_identity);
    f=Catalog();f.rows[index].definition_version++;BindingRefused(Resolve(f),BE::stale_generation);
    f=Catalog();f.rows[index].record.header.kind=c::CatalogRecordKind::table_descriptor;BindingRefused(Resolve(f),BE::wrong_family);
    f=Catalog();f.rows[index].authority_scope=c::CatalogAuthorityScope::cluster;BindingRefused(Resolve(f),BE::nonlocal_scope);
    f=Catalog();auto views=Views(f);views[index].provisional=true;
    BindingRefused(c::ResolveLocalCatalogMetricBindings(views,f.descriptor.binding.metric_uuid,1),BE::inactive_object);
    for(unsigned status=1;status<=9;++status)if(status!=unsigned(c::CatalogObjectStatus::active)){
      f=Catalog();f.rows[index].status=static_cast<c::CatalogObjectStatus>(status);BindingRefused(Resolve(f),BE::inactive_object);
    }
    for(unsigned lifecycle=1;lifecycle<=10;++lifecycle)if(lifecycle!=unsigned(c::CatalogObjectLifecycle::active)){
      f=Catalog();f.rows[index].lifecycle=static_cast<c::CatalogObjectLifecycle>(lifecycle);BindingRefused(Resolve(f),BE::inactive_object);
    }
    f=Catalog();f.rows[index].record.header.deleted=true;BindingRefused(Resolve(f),BE::inactive_object);
    f=Catalog();f.rows[index].record.header.row_uuid={};BindingRefused(Resolve(f),BE::invalid_catalog);
  }
  f=Catalog();auto views=Views(f);views.push_back({});
  BindingRefused(c::ResolveLocalCatalogMetricBindings(views,f.descriptor.binding.metric_uuid,1),BE::invalid_catalog);
  f=Catalog();auto labels=c::DecodeCatalogMetricLabelSchema(f.rows[1].record.payload);
  Check(labels.ok(),"label fixture decode");
  if(labels.ok()){
    labels.record->labels[0].sensitive=!labels.record->labels[0].sensitive;
    const auto changed=c::EncodeCatalogMetricLabelSchema(*labels.record);
    f.rows[1].record.payload.assign(changed.bytes.begin(),changed.bytes.end());
    BindingRefused(Resolve(f),BE::label_mismatch);
  }
  f=Catalog();f.rows[2].object_subtype="unrelated";f.rows[2].record.payload="opaque unrelated policy";
  BindingRefused(Resolve(f),BE::wrong_family);
  // Annotation changes cannot redirect binary policy selection.
  f=Catalog();f.descriptor.definition.family="unrelated-name";f.descriptor.definition.producer_owner="other";
  f.rows[0]=Metadata(f.descriptor);result=Resolve(f);
  Check(result.ok()&&result.binding->metric.retention.policy.policy_uuid==f.descriptor.binding.retention_policy_uuid,"annotation became lookup authority");
  f=Catalog();f.descriptor.definition.labels.clear();f.descriptor.binding.label_schema_uuid={};f.descriptor.binding.label_schema_generation=0;
  f.rows[0]=Metadata(f.descriptor);f.rows.erase(f.rows.begin()+1);result=Resolve(f);
  Check(result.ok()&&!result.binding->metric.labels,"exact absent label schema not retained");
}
c::CatalogMetadataVersion CounterMetadata(const c::CatalogMetricDescriptor& descriptor) {
  auto row=Metadata(descriptor);row.record.header.row_uuid=Id(p::UuidKind::row,60);
  row.default_name_uuid=Id(p::UuidKind::object,61);row.name_vector_uuid=Id(p::UuidKind::object,62);
  return row;
}
Fixture RateCatalog() {
  auto f=Catalog();auto source=f.descriptor;
  source.binding.metric_uuid=Id(p::UuidKind::object,20).value;source.binding.descriptor_generation=2;
  source.definition.type=m::MetricType::counter;source.definition.family="counter-annotation";
  f.descriptor.definition.type=m::MetricType::rate;f.descriptor.definition.rate_window_nanoseconds=1000000000;
  f.descriptor.binding.rate_source_counter_uuid=source.binding.metric_uuid;
  f.descriptor.binding.rate_source_counter_generation=source.binding.descriptor_generation;
  f.rows[0]=Metadata(f.descriptor);f.rows.push_back(CounterMetadata(source));return f;
}
void RateDependencies() {
  auto f=RateCatalog();auto result=Resolve(f);
  Check(result.ok()&&result.binding->source_counter&&result.binding->source_counter->descriptor.definition.type==m::MetricType::counter,
        "source counter dependencies not resolved");
  if(result.ok())Check(result.binding->source_counter->visibility_policy.record.payload==f.rows[3].record.payload,
                      "source security definition lost");
  f.rows.pop_back();BindingRefused(Resolve(f),BE::missing_object);
  f=RateCatalog();f.rows.back().definition_version++;BindingRefused(Resolve(f),BE::stale_generation);
  f=RateCatalog();auto source=c::DecodeCatalogMetricDescriptor(f.rows.back().record.payload);
  source.record->definition.type=m::MetricType::gauge;f.rows.back()=CounterMetadata(*source.record);
  BindingRefused(Resolve(f),BE::source_not_counter);
  f=RateCatalog();source=c::DecodeCatalogMetricDescriptor(f.rows.back().record.payload);
  source.record->binding.retention_policy_uuid=Id(p::UuidKind::object,88).value;f.rows.back()=CounterMetadata(*source.record);
  BindingRefused(Resolve(f),BE::missing_object);
  f=RateCatalog();f.descriptor.binding.rate_source_counter_uuid=f.descriptor.binding.metric_uuid;
  f.descriptor.binding.rate_source_counter_generation=f.descriptor.binding.descriptor_generation;f.rows[0]=Metadata(f.descriptor);
  BindingRefused(Resolve(f),BE::source_not_counter);
}
void BindingAllocationFailures() {
  auto f=RateCatalog();f.descriptor.definition.help=std::string(256,'h');f.rows[0]=Metadata(f.descriptor);
  const auto views=Views(f);const auto before=f.rows[0].record.payload;
  unsigned injected=0;bool completed=false;
  for(long index=0;index<4096;++index){
    descriptor_allocation_fault::remaining=index;descriptor_allocation_fault::fired=false;
    const auto result=c::ResolveLocalCatalogMetricBindings(views,f.descriptor.binding.metric_uuid,1);
    descriptor_allocation_fault::remaining=-1;const bool fired=descriptor_allocation_fault::fired;
    Check(f.rows[0].record.payload==before,"dependency allocation failure mutated catalog");
    if(fired){++injected;Check(!result.ok()&&!result.binding,"dependency allocation failure exposed bundle");}
    else{completed=true;Check(result.ok(),"dependency selection failed allocation recovery");break;}
  }
  Check(completed&&injected>20,"dependency allocation sweep missed allocation sites");
  std::cout<<"binding allocation injected="<<injected<<'\n';
}
}
int main(){
  const int prior=MetricDescriptorBindingBaseMain();const auto before=checks;
  DependencySelection();RateDependencies();BindingAllocationFailures();
  std::cout<<"metric binding checks="<<checks-before<<" combined="<<checks<<" failures="<<failures<<'\n';
  return prior||failures?1:0;
}
