// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
// Dependency selection tests use decoded-record fixtures, not native-I/O or
// authorization mocks. The existing independent descriptor regression runs too.
#define main MetricDescriptorBindingBaseMain
#include "catalog_metric_descriptor_test.cpp"
#undef main
#include "catalog_metric_binding.hpp"
#include "metric_value_update.hpp"

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
c::CatalogMetadataVersion SeriesRow(const Fixture& f,const c::CatalogMetricSeries& series) {
  auto row=Metadata(f.descriptor);
  row.record.header.kind=c::CatalogRecordKind::metric_series;
  row.record.header.object_uuid={p::UuidKind::object,series.series_uuid};
  row.record.header.row_uuid=Id(p::UuidKind::row,84);
  row.default_name_uuid=Id(p::UuidKind::object,85);row.name_vector_uuid=Id(p::UuidKind::object,86);
  row.object_subtype="metric_series";row.definition_version=series.generation;
  row.authority_scope=series.binding.cluster_uuid.is_nil()?c::CatalogAuthorityScope::local:c::CatalogAuthorityScope::cluster;
  row.creator_transaction_uuid=series.origin_transaction_uuid;row.creator_local_transaction_id=series.origin_local_transaction_id;
  const auto bytes=c::EncodeCatalogMetricSeries(series);Check(bytes.ok(),"series fixture must encode");
  row.record.payload.assign(bytes.bytes.begin(),bytes.bytes.end());return row;
}
Fixture SeriesCatalog(bool rate=false) {
  auto f=rate?RateCatalog():Catalog();
  c::CatalogMetricSeries series;series.series_uuid=Id(p::UuidKind::object,81).value;series.generation=1;
  static_cast<m::MetricDescriptorBinding&>(series.binding)=f.descriptor.binding;
  series.binding.database_uuid=Id(p::UuidKind::database,82).value;series.binding.node_uuid=Id(p::UuidKind::object,83).value;
  auto user=Id(p::UuidKind::object,87).value;user.bytes[6]=0x41;
  series.labels={{"tag",m::MetricLabelType::text,std::string("typed\0value",11)},
    {"database",m::MetricLabelType::system_uuid,series.binding.database_uuid},
    {"data-id",m::MetricLabelType::uuid_value,user}};
  series.origin_transaction_uuid=f.descriptor.origin_transaction_uuid;series.origin_local_transaction_id=f.descriptor.origin_local_transaction_id;
  f.rows.push_back(SeriesRow(f,series));return f;
}
c::CatalogMetricSeriesBindingResult ResolveSeries(const Fixture& f) {
  return c::ResolveLocalCatalogMetricSeriesBinding(Views(f),Id(p::UuidKind::database,82).value,
      Id(p::UuidKind::object,83).value,Id(p::UuidKind::object,81).value,1);
}
void SeriesBindingRefused(const c::CatalogMetricSeriesBindingResult& r,BE error) {
  Check(!r.ok()&&!r.binding&&r.error==error,"series selection wrong failure or partial bundle");
}
void SeriesSelection() {
  auto f=SeriesCatalog();const auto original=f.rows.back().record.payload;
  auto result=ResolveSeries(f);
  Check(result.ok(),"valid series dependency snapshot refused");
  if(result.ok()) {
    const auto& r=*result.binding;
    Check(r.series.series_uuid==Id(p::UuidKind::object,81).value&&r.definition.series_uuid==r.series.series_uuid,
          "series identity synthesized or replaced");
    Check(r.series_metadata.record.payload==original&&r.dependencies.metric.visibility_policy.record.payload==f.rows[3].record.payload,
          "selected series or opaque security metadata replaced");
    Check(r.series.database_uuid==Id(p::UuidKind::database,82).value&&r.series.node_uuid==Id(p::UuidKind::object,83).value&&
          r.series.cluster_uuid.is_nil()&&r.series.labels.size()==3,"series scope or labels lost");
    Check(r.series.metric_family==f.descriptor.definition.family&&!r.dependencies.source_counter,"series annotation or source unexpected");
    f.rows.clear();
    Check(r.series_metadata.record.payload==original,"series bundle did not retain selected snapshot data");
  }
  f=SeriesCatalog();std::reverse(f.rows.begin(),f.rows.end());
  Check(ResolveSeries(f).ok(),"series selection depended on catalog order");
  for(unsigned which=0;which<4;++which) {
    auto db=Id(p::UuidKind::database,82).value,node=Id(p::UuidKind::object,83).value,series=Id(p::UuidKind::object,81).value;
    if(which==0)db={};if(which==1)node={};if(which==2)series={};
    SeriesBindingRefused(c::ResolveLocalCatalogMetricSeriesBinding(Views(f),db,node,series,which==3?0:1),BE::invalid_request);
  }
  for(unsigned version=0;version<16;++version)if(version!=7)for(unsigned which=0;which<3;++which) {
    auto db=Id(p::UuidKind::database,82).value,node=Id(p::UuidKind::object,83).value,series=Id(p::UuidKind::object,81).value;
    (which==0?db:which==1?node:series).bytes[6]=p::byte(version<<4);
    SeriesBindingRefused(c::ResolveLocalCatalogMetricSeriesBinding(Views(f),db,node,series,1),BE::invalid_request);
  }
  f=SeriesCatalog();
  SeriesBindingRefused(c::ResolveLocalCatalogMetricSeriesBinding(Views(f),Id(p::UuidKind::database,82).value,
      Id(p::UuidKind::object,83).value,Id(p::UuidKind::object,99).value,1),BE::missing_object);
  for(unsigned index=0;index<5;++index) {
    f=SeriesCatalog();f.rows.erase(f.rows.begin()+index);SeriesBindingRefused(ResolveSeries(f),BE::missing_object);
    f=SeriesCatalog();f.rows.push_back(f.rows[index]);SeriesBindingRefused(ResolveSeries(f),BE::duplicate_identity);
    f=SeriesCatalog();f.rows[index].definition_version++;SeriesBindingRefused(ResolveSeries(f),BE::stale_generation);
    f=SeriesCatalog();f.rows[index].record.header.kind=c::CatalogRecordKind::table_descriptor;SeriesBindingRefused(ResolveSeries(f),BE::wrong_family);
    f=SeriesCatalog();f.rows[index].authority_scope=c::CatalogAuthorityScope::cluster;SeriesBindingRefused(ResolveSeries(f),BE::nonlocal_scope);
    f=SeriesCatalog();f.rows[index].record.header.deleted=true;SeriesBindingRefused(ResolveSeries(f),BE::inactive_object);
    f=SeriesCatalog();f.rows[index].status=c::CatalogObjectStatus::disabled_by_policy;SeriesBindingRefused(ResolveSeries(f),BE::inactive_object);
    f=SeriesCatalog();f.rows[index].lifecycle=c::CatalogObjectLifecycle::altering;SeriesBindingRefused(ResolveSeries(f),BE::inactive_object);
    f=SeriesCatalog();auto views=Views(f);views[index].provisional=true;
    SeriesBindingRefused(c::ResolveLocalCatalogMetricSeriesBinding(views,Id(p::UuidKind::database,82).value,
      Id(p::UuidKind::object,83).value,Id(p::UuidKind::object,81).value,1),BE::inactive_object);
  }
  f=SeriesCatalog();auto views=Views(f);views.push_back({});
  SeriesBindingRefused(c::ResolveLocalCatalogMetricSeriesBinding(views,Id(p::UuidKind::database,82).value,
      Id(p::UuidKind::object,83).value,Id(p::UuidKind::object,81).value,1),BE::invalid_catalog);
  for(unsigned which=0;which<11;++which) {
    f=SeriesCatalog();auto r=*c::DecodeCatalogMetricSeries(f.rows.back().record.payload).record;
    if(which==0)r.binding.database_uuid.bytes[15]++;
    if(which==1)r.binding.node_uuid.bytes[15]++;
    if(which==2)r.binding.cluster_uuid=Id(p::UuidKind::object,92).value;
    if(which==3)r.binding.label_schema_generation++;
    if(which==4)r.binding.retention_policy_generation++;
    if(which==5)r.binding.visibility_policy_generation++;
    if(which==6)r.labels.erase(r.labels.begin()+1); // decoded order: data-id,database,tag
    if(which==7)r.labels[1].type=m::MetricLabelType::uuid_value;
    if(which==8)r.binding.rate_source_counter_uuid=Id(p::UuidKind::object,90).value,r.binding.rate_source_counter_generation=1;
    if(which==9)r.binding.metric_uuid=Id(p::UuidKind::object,95).value;
    if(which==10)r.binding.descriptor_generation++;
    f.rows.back()=SeriesRow(f,r);
    SeriesBindingRefused(ResolveSeries(f),which<2?BE::scope_mismatch:which==2?BE::nonlocal_scope:
        which==9?BE::missing_object:which==10?BE::stale_generation:BE::series_binding_mismatch);
  }
  for(unsigned version=1;version<=7;++version) {
    f=SeriesCatalog();auto r=*c::DecodeCatalogMetricSeries(f.rows.back().record.payload).record;
    std::get<p::Uuid>(r.labels[0].value).bytes[6]=p::byte(version<<4);f.rows.back()=SeriesRow(f,r);
    result=ResolveSeries(f);Check(result.ok()&&std::get<p::Uuid>(result.binding->series.labels[0].value).bytes[6]==p::byte(version<<4),
                                "series lookup coerced a user UUID");
  }
  f=SeriesCatalog();auto r=*c::DecodeCatalogMetricSeries(f.rows.back().record.payload).record;
  r.labels.clear();r.binding.label_schema_uuid={};r.binding.label_schema_generation=0;
  f.descriptor.definition.labels.clear();f.descriptor.binding.label_schema_uuid={};f.descriptor.binding.label_schema_generation=0;
  f.rows[0]=Metadata(f.descriptor);f.rows.back()=SeriesRow(f,r);f.rows.erase(f.rows.begin()+1);
  Check(ResolveSeries(f).ok(),"unlabelled native series failed selection");
  f=SeriesCatalog();f.descriptor.definition.family="changed annotation";f.rows[0]=Metadata(f.descriptor);
  result=ResolveSeries(f);Check(result.ok()&&result.binding->series.metric_family=="changed annotation",
                              "metric annotation became durable series lookup authority");
  f=SeriesCatalog(true);result=ResolveSeries(f);
  Check(result.ok()&&result.binding->dependencies.source_counter&&
        result.binding->dependencies.source_counter->descriptor.definition.type==m::MetricType::counter,
        "series rate dependencies not resolved from same snapshot");
  if(result.ok()) {
    m::MetricDescriptor descriptor;
    static_cast<m::MetricDescriptorDefinition&>(descriptor)=f.descriptor.definition;
    static_cast<m::MetricDescriptorBinding&>(descriptor)=f.descriptor.binding;
    Check(m::ValidateStoredMetricValueDescriptor(descriptor)&&!m::ValidateMetricValueDescriptor(descriptor),
          "rate identity admission bypassed raw update distinction");
    const auto& series=result.binding->series;
    Check(!m::StageMetricValueUpdate(descriptor,series.labels,nullptr,p::u64(9007199254740993ULL)).ok(),
          "catalog rate binding enabled a raw rate producer update");
    m::MetricValue value;value.family=descriptor.family;value.type=m::MetricType::rate;
    value.labels=series.labels;value.value=p::u64(9007199254740993ULL);
    Check(m::ValidateStoredMetricValueShape(descriptor,value)&&
          !m::MakeMetricRawSampleRecord(descriptor,series,value,1,1,1).ok(),
          "rate identity binding fabricated raw rate evidence");
    descriptor.rate_window_nanoseconds=0;Check(!m::ValidateStoredMetricValueDescriptor(descriptor),"rate missing window admitted");
  }
  f.rows.erase(f.rows.end()-2);SeriesBindingRefused(ResolveSeries(f),BE::missing_object);
}
void SeriesAmbiguity() {
  const auto base=SeriesCatalog();
  const auto original=*c::DecodeCatalogMetricSeries(base.rows.back().record.payload).record;
  for(unsigned which=0;which<4;++which) {
    auto f=base;auto other=original;other.series_uuid=Id(p::UuidKind::object,93).value;
    if(which==1)other.binding.descriptor_generation++;
    if(which==2)other.binding.retention_policy_generation++;
    if(which==3)other.binding.visibility_policy_uuid=Id(p::UuidKind::object,94).value;
    f.rows.push_back(SeriesRow(f,other));SeriesBindingRefused(ResolveSeries(f),BE::duplicate_series);
    std::reverse(f.rows.begin(),f.rows.end());SeriesBindingRefused(ResolveSeries(f),BE::duplicate_series);
  }
  for(unsigned which=0;which<8;++which) {
    auto f=base;auto other=original;other.series_uuid=Id(p::UuidKind::object,93).value;
    if(which==0)other.binding.database_uuid.bytes[15]++;
    if(which==1)other.binding.node_uuid.bytes[15]++;
    if(which==2)other.binding.cluster_uuid=Id(p::UuidKind::object,94).value;
    if(which==3)other.binding.metric_uuid.bytes[15]++;
    if(which==4)other.binding.label_schema_uuid.bytes[15]++;
    if(which==5)other.labels[2].value=std::string("different");
    if(which==6)other.labels[1].type=m::MetricLabelType::uuid_value;
    if(which==7)other.labels[2].key="different";
    f.rows.push_back(SeriesRow(f,other));
    Check(ResolveSeries(f).ok(),"different immutable typed series key collided");
  }
  for(unsigned which=0;which<4;++which) {
    auto f=base;auto other=original;other.series_uuid=Id(p::UuidKind::object,93).value;f.rows.push_back(SeriesRow(f,other));
    if(which==0)f.rows.back().status=c::CatalogObjectStatus::disabled_by_policy;
    if(which==1)f.rows.back().lifecycle=c::CatalogObjectLifecycle::altering;
    if(which==2){f.rows.back().record.header.deleted=true;f.rows.back().lifecycle=c::CatalogObjectLifecycle::dropped;
      f.rows.back().status=c::CatalogObjectStatus::retired;f.rows.back().retired_transaction_uuid=f.rows.back().creator_transaction_uuid;}
    auto views=Views(f);if(which==3)views.back().provisional=true;
    Check(c::ResolveLocalCatalogMetricSeriesBinding(views,Id(p::UuidKind::database,82).value,
      Id(p::UuidKind::object,83).value,Id(p::UuidKind::object,81).value,1).ok(),"inactive competitor supplied an active series");
  }
  for(unsigned which=0;which<5;++which) {
    auto f=base;auto other=original;other.series_uuid=Id(p::UuidKind::object,93).value;
    other.binding.node_uuid.bytes[15]++;f.rows.push_back(SeriesRow(f,other));
    if(which==0)f.rows.back().record.payload="series=legacy";
    if(which==1)f.rows.back().record.header.kind=c::CatalogRecordKind::table_descriptor;
    if(which==2)f.rows.back().object_subtype="table";
    if(which==3)f.rows.back().record.header.row_uuid={};
    if(which==4)f.rows.back().definition_version++;
    SeriesBindingRefused(ResolveSeries(f),BE::invalid_catalog);
  }
}
void SeriesSelectionAllocationFailures() {
  auto f=SeriesCatalog(true);const auto views=Views(f);const auto before=f.rows.back().record.payload;
  unsigned injected=0;bool completed=false;
  for(long index=0;index<4096;++index) {
    descriptor_allocation_fault::remaining=index;descriptor_allocation_fault::fired=false;
    const auto result=c::ResolveLocalCatalogMetricSeriesBinding(views,Id(p::UuidKind::database,82).value,
        Id(p::UuidKind::object,83).value,Id(p::UuidKind::object,81).value,1);
    descriptor_allocation_fault::remaining=-1;const bool fired=descriptor_allocation_fault::fired;
    Check(f.rows.back().record.payload==before,"series selection allocation fault mutated source");
    if(fired){++injected;Check(!result.ok()&&!result.binding&&result.error==BE::resource_exhausted,
                             "series selection allocation fault exposed partial bundle or wrong error");}
    else{completed=true;Check(result.ok(),"series selection did not recover from allocation faults");break;}
  }
  Check(completed&&injected>30,"series selection allocation sweep incomplete");
  std::cout<<"series selection allocation injected="<<injected<<'\n';
}

}
int main(){
  const int prior=MetricDescriptorBindingBaseMain();const auto before=checks;
  DependencySelection();RateDependencies();BindingAllocationFailures();
  SeriesSelection();SeriesAmbiguity();SeriesSelectionAllocationFailures();
  std::cout<<"metric binding checks="<<checks-before<<" combined="<<checks<<" failures="<<failures<<'\n';
  return prior||failures?1:0;
}
