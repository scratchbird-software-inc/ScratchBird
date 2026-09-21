// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#define main MetricDescriptorRegressionMain
#include "catalog_metric_descriptor_test.cpp"
#undef main
#include "catalog_metric_series.hpp"

namespace {
c::CatalogMetricSeries Series() {
  const auto d=Descriptor();c::CatalogMetricSeries r;
  r.series_uuid=Id(p::UuidKind::object,41).value;r.generation=1;
  static_cast<m::MetricDescriptorBinding&>(r.binding)=d.binding;
  r.binding.database_uuid=Id(p::UuidKind::database,42).value;r.binding.node_uuid=Id(p::UuidKind::object,43).value;
  auto user=Id(p::UuidKind::object,44).value;user.bytes[6]=0x41;
  r.labels={{"tag",m::MetricLabelType::text,std::string("data\0value",10)},
      {"database",m::MetricLabelType::system_uuid,r.binding.database_uuid},{"data-id",m::MetricLabelType::uuid_value,user}};
  r.origin_transaction_uuid=d.origin_transaction_uuid;r.origin_local_transaction_id=d.origin_local_transaction_id;return r;
}
// Independent registry/layout bytes; does not call the production schema,
// encoder, key sorter or scalar packer.
std::string SeriesGolden(const c::CatalogMetricSeries& r,bool sort=true) {
  auto labels=r.labels;
  if(sort)std::sort(labels.begin(),labels.end(),[](const auto& a,const auto& b){return a.key<b.key;});
  std::string s(24,0);s.replace(0,4,"SBCV");Put(s,4,1,2);Put(s,6,24,2);Put(s,12,20,4);Put(s,16,65546,4);Put(s,20,1,2);
  const auto& b=r.binding;
  Field(s,1,5,Uuid(r.series_uuid));Field(s,2,1,Number(r.generation));
  Field(s,3,5,Uuid(b.database_uuid));Field(s,4,5,Uuid(b.node_uuid));Field(s,5,6,Uuid(b.cluster_uuid));
  Field(s,6,5,Uuid(b.metric_uuid));Field(s,7,1,Number(b.descriptor_generation));
  Field(s,8,6,Uuid(b.label_schema_uuid));Field(s,9,1,Number(b.label_schema_generation));
  Field(s,10,5,Uuid(b.retention_policy_uuid));Field(s,11,1,Number(b.retention_policy_generation));
  Field(s,12,5,Uuid(b.visibility_policy_uuid));Field(s,13,1,Number(b.visibility_policy_generation));
  Field(s,14,6,Uuid(b.rate_source_counter_uuid));Field(s,15,1,Number(b.rate_source_counter_generation));
  std::vector<std::string> keys,values;std::string types;
  for(const auto& l:labels) {
    keys.push_back(l.key);types+=char(l.type==m::MetricLabelType::text?1:l.type==m::MetricLabelType::system_uuid?2:3);
    if(const auto* text=std::get_if<std::string>(&l.value))values.push_back(*text);else values.push_back(Uuid(std::get<p::Uuid>(l.value)));
  }
  Field(s,16,8,List(keys));Field(s,17,4,types);Field(s,18,4,List(values));
  Field(s,19,5,Uuid(r.origin_transaction_uuid.value));Field(s,20,1,Number(r.origin_local_transaction_id));Put(s,8,s.size(),4);return s;
}
c::CatalogMetadataVersion SeriesMetadata(const c::CatalogMetricSeries& r) {
  auto v=Metadata(Descriptor());v.record.header.kind=c::CatalogRecordKind::metric_series;
  v.record.header.object_uuid={p::UuidKind::object,r.series_uuid};v.record.payload=SeriesGolden(r);
  v.object_subtype="metric_series";v.definition_version=r.generation;
  v.creator_transaction_uuid=r.origin_transaction_uuid;v.creator_local_transaction_id=r.origin_local_transaction_id;
  v.authority_scope=r.binding.cluster_uuid.is_nil()?c::CatalogAuthorityScope::local:c::CatalogAuthorityScope::cluster;return v;
}
bool SharedSeriesOrigin(const c::CatalogMetadataVersion& a,const c::CatalogMetadataVersion& b) {
  const bool family=c::CatalogMetricSeriesPreservesOrigin(a,b);
  const bool shared=c::CatalogMetadataPreservesFamilyOrigin(a,b);
  Check(shared==family,"shared native origin dispatcher differs from family contract");
  return family;
}

void SeriesRefused(std::string_view bytes) {
  const auto result=c::DecodeCatalogMetricSeries(bytes);Check(!result.ok()&&!result.record,"malformed series decoded");
}
void SeriesInvalid(const c::CatalogMetricSeries& r) {
  const auto result=c::EncodeCatalogMetricSeries(r);Check(!result.ok()&&result.bytes.empty(),"invalid series encoded");
}
void SeriesRoundTrip(const c::CatalogMetricSeries& r) {
  const auto golden=SeriesGolden(r);const auto encoded=c::EncodeCatalogMetricSeries(r);
  Check(encoded.ok()&&std::string(encoded.bytes.begin(),encoded.bytes.end())==golden,"series independent layout differs");
  Check(SeriesGolden(r)==golden,"series input modified");
  const auto decoded=c::DecodeCatalogMetricSeries(golden);
  Check(decoded.ok()&&SeriesGolden(*decoded.record,false)==golden,"series canonical decode differs");
  const auto metadata=SeriesMetadata(r);Check(c::CatalogMetricSeriesMatchesMetadata(metadata),"series metadata refused");
  const auto envelope=c::EncodeCatalogMetadataVersion(metadata);Check(envelope.ok(),"series envelope failed");
  if(envelope.ok()){const auto reread=c::DecodeCatalogMetadataVersion(envelope.bytes);Check(reread.ok()&&reread.record.record.payload==golden,"series envelope reread lost payload");}
  const auto typed=c::EncodeCatalogTypedRecord(metadata.record,3);Check(typed.ok()&&c::DecodeCatalogTypedRecord(typed.row).ok(),"series typed record failed");
}
void SeriesShapes() {
  const auto base=Series();SeriesRoundTrip(base);
  Check(static_cast<unsigned>(c::CatalogRecordKind::metric_series)==99,"series kind must be99");
  Check(std::string(c::CatalogRecordKindName(c::CatalogRecordKind::metric_series))=="metric_series","series kind name missing");
  const auto& kinds=c::BuiltinCatalogRecordDescriptors();Check(std::count_if(kinds.begin(),kinds.end(),[](const auto& d){return d.kind==c::CatalogRecordKind::metric_series;})==1,"series kind registration count");
  for(unsigned v=1;v<=7;++v){auto r=base;std::get<p::Uuid>(r.labels[2].value).bytes[6]=p::byte(v<<4);SeriesRoundTrip(r);}
  auto r=base;r.binding.cluster_uuid=Id(p::UuidKind::object,70).value;SeriesRoundTrip(r);
  r.labels.clear();SeriesRoundTrip(r);r.binding.label_schema_uuid={};r.binding.label_schema_generation=0;SeriesRoundTrip(r);
  r=base;r.binding.rate_source_counter_uuid=Id(p::UuidKind::object,71).value;r.binding.rate_source_counter_generation=5;SeriesRoundTrip(r);
  r=base;r.labels={{"ascii",m::MetricLabelType::text,"x"},{"\xc3\xa9",m::MetricLabelType::text,"y"},{"\xe2\x82\xac",m::MetricLabelType::text,"z"}};SeriesRoundTrip(r);
  r=base;r.generation=std::numeric_limits<p::u64>::max();r.origin_local_transaction_id=r.generation;SeriesRoundTrip(r);
  r=base;r.labels={{std::string(4096,'k'),m::MetricLabelType::text,std::string(65528,'v')}};SeriesRoundTrip(r);
  r.labels[0].key+='k';SeriesInvalid(r);SeriesRefused(SeriesGolden(r));r.labels[0].key.pop_back();
  std::get<std::string>(r.labels[0].value)+='v';SeriesInvalid(r);SeriesRefused(SeriesGolden(r));
  r=base;r.labels.clear();for(unsigned i=0;i<1024;++i)r.labels.push_back({std::to_string(i),m::MetricLabelType::text,"v"});
  SeriesRoundTrip(r);r.labels.push_back({"extra",m::MetricLabelType::text,"v"});SeriesInvalid(r);SeriesRefused(SeriesGolden(r));
  for(unsigned which=0;which<10;++which){r=base;
    if(which==0)r.labels.push_back(r.labels[0]);if(which==1)r.labels[0].key="";
    if(which==2)r.labels[0].key=std::string(1,'\0');if(which==3)r.labels[0].key=std::string(1,char(0xff));
    if(which==4)r.labels[0].value=std::string();if(which==5)r.labels[0].value=std::string(1,char(0xff));
    if(which==6)r.labels[0].value=Id(p::UuidKind::object,80).value;
    if(which==7)r.labels[1].value=std::string("00000000-0000-7000-8000-000000000001");
    if(which==8)r.labels[0].type=static_cast<m::MetricLabelType>(255);
    if(which==9)r.origin_transaction_uuid.kind=p::UuidKind::object;SeriesInvalid(r);
  }
}
void SeriesMalformed() {
  const auto golden=SeriesGolden(Series());
  for(std::size_t size=0;size<golden.size();++size)SeriesRefused(std::string_view(golden).substr(0,size));
  SeriesRefused(golden+"x");SeriesRefused("series_uuid=text");SeriesRefused(std::string(131073,'x'));
  for(unsigned at:{0u,4u,6u,8u,12u,16u,20u,22u}){auto bytes=golden;bytes[at]^=1;SeriesRefused(bytes);}
  for(unsigned id=1;id<=20;++id){const auto at=Offset(golden,id);auto b=golden;Put(b,at,99,2);SeriesRefused(b);
    b=golden;Put(b,at+2,255,1);SeriesRefused(b);b=golden;Put(b,at+3,1,1);SeriesRefused(b);
    b=golden;Put(b,at+4,0xffffffff,4);SeriesRefused(b);
    b=golden;b.erase(at,8+Get(b,at+4,4));Put(b,8,b.size(),4);Put(b,12,19,4);SeriesRefused(b);
    b=golden;b.insert(at,b.substr(at,8+Get(b,at+4,4)));Put(b,8,b.size(),4);Put(b,12,21,4);SeriesRefused(b);
  }
  for(unsigned id:{1u,3u,4u,5u,6u,8u,10u,12u,14u,19u}){
    for(unsigned version=0;version<16;++version)if(version!=7){auto b=Replace(golden,id,Uuid(Id(p::UuidKind::object,90).value));b[Offset(b,id)+14]=char(version<<4);SeriesRefused(b);}
    SeriesRefused(Replace(golden,id,"00000000-0000-7000-8000-000000000001"));
  }
  for(unsigned id:{1u,3u,4u,6u,8u,10u,12u,19u})SeriesRefused(Replace(golden,id,std::string(16,0)));
  for(unsigned id:{2u,7u,9u,11u,13u,20u})SeriesRefused(Replace(golden,id,Number(0)));
  SeriesRefused(Replace(golden,15,Number(1)));SeriesRefused(SeriesGolden(Series(),false));
  SeriesRefused(Replace(golden,16,List({"key","key","other"})));
  SeriesRefused(Replace(golden,17,std::string(2,1)));
  for(unsigned code=0;code<256;++code)if(code<1||code>3)SeriesRefused(Replace(golden,17,std::string(3,char(code))));
  for(const auto& value:{std::string(),Number(0,4),Number(0xffffffff,4),Number(3,4)+Number(0xffffffff,4)})SeriesRefused(Replace(golden,18,value));
  auto values=golden.substr(Offset(golden,18)+8,Get(golden,Offset(golden,18)+4,4));
  for(std::size_t i=0;i<values.size();++i)SeriesRefused(Replace(golden,18,values.substr(0,i)));
  SeriesRefused(Replace(golden,18,values+"x"));
  for(unsigned version=0;version<16;++version)if(version<1||version>7){auto r=Series();std::get<p::Uuid>(r.labels[2].value).bytes[6]=p::byte(version<<4);SeriesInvalid(r);SeriesRefused(SeriesGolden(r));}
}
void SeriesBindings() {
  const auto base=Series();const auto metadata=SeriesMetadata(base);
  for(unsigned which=0;which<13;++which){auto v=metadata;
    if(which==0)v.record.header.kind=c::CatalogRecordKind::table_descriptor;if(which==1)v.record.header.object_uuid.value.bytes[15]++;
    if(which==2)v.object_subtype="table";if(which==3)v.definition_version++;if(which==4)v.record.header.parent_uuid.value.bytes[15]++;
    if(which==5)v.owning_schema_uuid={};if(which==6)v.default_name_uuid={};if(which==7)v.name_vector_uuid={};
    if(which==8)v.security_policy_uuid={};if(which==9)v.creator_transaction_uuid.value.bytes[15]++;
    if(which==10)v.creator_local_transaction_id++;if(which==11)v.authority_scope=c::CatalogAuthorityScope::cluster;
    if(which==12)v.record.payload="series_uuid=text";
    Check(!c::CatalogMetricSeriesMatchesMetadata(v)&&!c::EncodeCatalogMetadataVersion(v).ok(),"series common mismatch admitted");
  }
  auto next=base;next.generation=2;next.binding.retention_policy_generation++;
  auto successor=SeriesMetadata(next);successor.creator_transaction_uuid=Id(p::UuidKind::transaction,80);successor.creator_local_transaction_id=12;
  Check(c::EncodeCatalogMetadataVersion(successor).ok()&&SharedSeriesOrigin(metadata,successor),"policy generation incorrectly replaced series identity");
  successor.record.header.deleted=true;successor.lifecycle=c::CatalogObjectLifecycle::dropped;successor.status=c::CatalogObjectStatus::retired;successor.retired_transaction_uuid=successor.creator_transaction_uuid;
  Check(c::EncodeCatalogMetadataVersion(successor).ok()&&SharedSeriesOrigin(metadata,successor),"series retirement lost origin");
  for(unsigned which=0;which<11;++which){auto r=next;
    if(which==0)r.series_uuid.bytes[15]++;if(which==1)r.binding.database_uuid.bytes[15]++;
    if(which==2)r.binding.node_uuid.bytes[15]++;if(which==3)r.binding.cluster_uuid=Id(p::UuidKind::object,90).value;
    if(which==4)r.binding.metric_uuid.bytes[15]++;if(which==5)r.binding.label_schema_uuid.bytes[15]++;
    if(which==6)r.labels[0].key+="x";if(which==7)r.labels[0].value=std::string("changed");
    if(which==8)r.labels[1].type=m::MetricLabelType::uuid_value;
    if(which==9)r.origin_transaction_uuid.value.bytes[15]++;if(which==10)r.origin_local_transaction_id--;
    auto v=SeriesMetadata(r);v.creator_transaction_uuid=successor.creator_transaction_uuid;v.creator_local_transaction_id=12;
    Check(c::EncodeCatalogMetadataVersion(v).ok(),"series changed-origin fixture invalid");
    Check(!SharedSeriesOrigin(metadata,v),"series immutable key or origin changed");
  }
  auto other=metadata;other.record.header.kind=c::CatalogRecordKind::table_descriptor;other.object_subtype="other";other.record.payload="unrelated";
  Check(SharedSeriesOrigin(other,other),"series changed unrelated family");
  Check(!SharedSeriesOrigin(metadata,other)&&!SharedSeriesOrigin(other,metadata),"series family swap admitted");
  auto d0=Descriptor();m::MetricDescriptor d;static_cast<m::MetricDescriptorDefinition&>(d)=d0.definition;static_cast<m::MetricDescriptorBinding&>(d)=d0.binding;
  m::MetricRetentionPolicy policy;policy.policy_name="fixture";policy.policy_uuid=base.binding.retention_policy_uuid;policy.generation=base.binding.retention_policy_generation;
  const auto bound=c::BindCatalogMetricSeries(base,d,policy);
  Check(bound.ok()&&bound.record->series_uuid==base.series_uuid&&bound.record->labels.size()==3,"catalog series not bound to existing identity");
  for(unsigned which=0;which<9;++which){auto r=base;auto descriptor=d;auto p=policy;
    if(which==0)descriptor.metric_uuid.bytes[15]++;if(which==1)descriptor.descriptor_generation++;
    if(which==2)descriptor.labels[0].value_type=m::MetricLabelType::uuid_value;if(which==3)r.labels.erase(r.labels.begin()+1);
    if(which==4)p.generation++;if(which==5)p.policy_uuid.bytes[15]++;if(which==6)descriptor.cluster_only=true;
    if(which==7)descriptor.labels[1].key="unknown";if(which==8)r.binding.visibility_policy_generation++;
    const auto result=c::BindCatalogMetricSeries(r,descriptor,p);Check(!result.ok()&&!result.record,"series descriptor mismatch bound");
  }
}
void SeriesAllocations() {
  auto r=Series();r.labels[0].value=std::string(512,'v');const auto golden=SeriesGolden(r);
  for(unsigned operation=0;operation<2;++operation){unsigned injected=0;bool completed=false;
    for(long index=0;index<2048;++index){descriptor_allocation_fault::remaining=index;descriptor_allocation_fault::fired=false;
      bool success=false,partial=false;try {
        if(operation==0){const auto x=c::EncodeCatalogMetricSeries(r);success=x.ok();partial=!success&&!x.bytes.empty();}
        else {const auto x=c::DecodeCatalogMetricSeries(golden);success=x.ok();partial=!success&&x.record.has_value();}
      }catch(const std::bad_alloc&){}
      descriptor_allocation_fault::remaining=-1;const bool fired=descriptor_allocation_fault::fired;
      Check(!partial&&SeriesGolden(r)==golden,"series allocation failure mutated or published partial state");
      if(fired){++injected;Check(!success,"series allocation failure succeeded");}else{completed=true;Check(success,"series failed recovery");break;}
    }Check(completed&&injected>10,"series allocation fault sweep incomplete");std::cout<<"series allocation operation="<<operation<<" injected="<<injected<<'\n';
  }
}
}
int main(){MetricDescriptorRegressionMain();const auto before=checks;SeriesShapes();SeriesMalformed();SeriesBindings();SeriesAllocations();
  std::cout<<"metric series checks="<<checks-before<<" combined="<<checks<<" failures="<<failures<<'\n';return failures?1:0;}
