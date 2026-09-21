// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
// Reuse independent byte packing and allocation-fault oracles; each production
// codec is compiled separately and the descriptor regression remains active.
#define main MetricDescriptorRegressionMain
#include "catalog_metric_descriptor_test.cpp"
#undef main
#include "catalog_metric_label_schema.hpp"

namespace {
c::CatalogMetricLabelSchema LabelSchema() {
  const auto descriptor=Descriptor();
  c::CatalogMetricLabelSchema r;
  r.label_schema_uuid=descriptor.binding.label_schema_uuid;r.generation=1;
  r.labels=descriptor.definition.labels;r.origin_transaction_uuid=descriptor.origin_transaction_uuid;
  r.origin_local_transaction_id=descriptor.origin_local_transaction_id;return r;
}
std::string LabelGolden(const c::CatalogMetricLabelSchema& r) {
  std::string s(24,0);s.replace(0,4,"SBCV");Put(s,4,1,2);Put(s,6,24,2);Put(s,12,8,4);
  Put(s,16,65545,4);Put(s,20,1,2);
  Field(s,1,5,Uuid(r.label_schema_uuid));Field(s,2,1,Number(r.generation));
  std::vector<std::string> keys;std::string flags,types;
  for(const auto& label:r.labels){
    keys.push_back(label.key);flags+=char(label.required+2*label.sensitive);
    types+=char(label.value_type==m::MetricLabelType::text?1:label.value_type==m::MetricLabelType::system_uuid?2:3);
  }
  Field(s,3,8,List(keys));Field(s,4,4,flags);Field(s,5,4,types);
  Field(s,6,2,Number(r.cluster_only,1));Field(s,7,5,Uuid(r.origin_transaction_uuid.value));
  Field(s,8,1,Number(r.origin_local_transaction_id));Put(s,8,s.size(),4);return s;
}
c::CatalogMetadataVersion LabelMetadata(const c::CatalogMetricLabelSchema& r) {
  auto m=Metadata(Descriptor());m.record.header.kind=c::CatalogRecordKind::metric_label_schema;
  m.record.header.object_uuid={p::UuidKind::object,r.label_schema_uuid};
  m.record.payload=LabelGolden(r);m.definition_version=r.generation;
  m.creator_transaction_uuid=r.origin_transaction_uuid;m.creator_local_transaction_id=r.origin_local_transaction_id;
  m.object_subtype="metric_label_schema";
  m.authority_scope=r.cluster_only?c::CatalogAuthorityScope::cluster:c::CatalogAuthorityScope::local;return m;
}
bool SharedLabelOrigin(const c::CatalogMetadataVersion& a,const c::CatalogMetadataVersion& b) {
  const bool family=c::CatalogMetricLabelSchemaPreservesOrigin(a,b);
  const bool shared=c::CatalogMetadataPreservesFamilyOrigin(a,b);
  Check(shared==family,"shared native origin dispatcher differs from family contract");
  return family;
}

void LabelRefused(std::string_view bytes) {
  const auto result=c::DecodeCatalogMetricLabelSchema(bytes);
  Check(!result.ok()&&!result.record,"malformed label schema published");
}
void LabelRoundTrip(const c::CatalogMetricLabelSchema& r) {
  const auto golden=LabelGolden(r);const auto encoded=c::EncodeCatalogMetricLabelSchema(r);
  Check(encoded.ok()&&std::string(encoded.bytes.begin(),encoded.bytes.end())==golden,"label schema independent byte oracle mismatch");
  const auto decoded=c::DecodeCatalogMetricLabelSchema(golden);
  Check(decoded.ok()&&LabelGolden(*decoded.record)==golden,"label schema decode differs from oracle");
  const auto metadata=LabelMetadata(r);
  Check(c::CatalogMetricLabelSchemaMatchesMetadata(metadata),"valid label schema common binding refused");
  const auto envelope=c::EncodeCatalogMetadataVersion(metadata);Check(envelope.ok(),"label schema native envelope refused");
  if(envelope.ok()){
    const auto reread=c::DecodeCatalogMetadataVersion(envelope.bytes);
    Check(reread.ok()&&reread.record.record.payload==golden,"label schema envelope lost payload");
  }
  const auto typed=c::EncodeCatalogTypedRecord(metadata.record,3);
  Check(typed.ok()&&c::DecodeCatalogTypedRecord(typed.row).ok(),"label schema typed record failed");
}
void LabelInvalid(const c::CatalogMetricLabelSchema& r) {
  const auto result=c::EncodeCatalogMetricLabelSchema(r);
  Check(!result.ok()&&result.bytes.empty(),"invalid label schema encoded");
}
void LabelShapesAndLimits() {
  const auto base=LabelSchema();LabelRoundTrip(base);
  Check(static_cast<unsigned>(c::CatalogRecordKind::metric_label_schema)==98,"label schema kind not registered as98");
  Check(std::string(c::CatalogRecordKindName(c::CatalogRecordKind::metric_label_schema))=="metric_label_schema","label schema kind name missing");
  const auto& kinds=c::BuiltinCatalogRecordDescriptors();
  Check(std::count_if(kinds.begin(),kinds.end(),[](const auto& d){return d.kind==c::CatalogRecordKind::metric_label_schema;})==1,"label schema registered zero or multiple times");
  for(auto type:{m::MetricLabelType::text,m::MetricLabelType::system_uuid,m::MetricLabelType::uuid_value})
    for(unsigned flags=0;flags<4;++flags){
      auto r=base;r.labels={{"key-\xc3\xa9",bool(flags&1),bool(flags&2),type}};LabelRoundTrip(r);
    }
  auto r=base;r.labels.clear();LabelRoundTrip(r);r.cluster_only=true;LabelRoundTrip(r);
  r=base;r.generation=std::numeric_limits<p::u64>::max();r.origin_local_transaction_id=std::numeric_limits<p::u64>::max();LabelRoundTrip(r);
  r=base;r.labels={{std::string(4096,'k'),false,false,m::MetricLabelType::text}};LabelRoundTrip(r);
  r.labels[0].key+='k';LabelInvalid(r);LabelRefused(LabelGolden(r));
  r=base;r.labels.clear();
  for(unsigned i=0;i<1024;++i){
    auto name=std::to_string(i);name+=std::string((i==1023?56:60)-name.size(),'k');
    r.labels.push_back({name,false,true,m::MetricLabelType::text});
  }
  Check(LabelGolden(r).size()==67721,"independent maximum layout size");
  LabelRoundTrip(r);r.labels.back().key+='k';LabelInvalid(r);LabelRefused(LabelGolden(r));
  r.labels.back().key.pop_back();r.labels.push_back({"extra",false,false,m::MetricLabelType::text});LabelInvalid(r);
  for(unsigned which=0;which<7;++which){
    r=base;
    if(which==0)r.label_schema_uuid={};
    if(which==1)r.generation=0;
    if(which==2)r.origin_transaction_uuid.kind=p::UuidKind::object;
    if(which==3)r.origin_local_transaction_id=0;
    if(which==4)r.labels[0].value_type=static_cast<m::MetricLabelType>(255);
    if(which==5)r.labels[0].key="";
    if(which==6)r.labels.push_back(r.labels[0]);
    LabelInvalid(r);
  }
}
void LabelMalformed() {
  const auto golden=LabelGolden(LabelSchema());
  for(std::size_t size=0;size<golden.size();++size)LabelRefused(std::string_view(golden).substr(0,size));
  LabelRefused(golden+"x");LabelRefused("label_schema_uuid=seed");LabelRefused(std::string(67722,'x'));
  for(unsigned at:{0u,4u,6u,8u,12u,16u,20u,22u}){auto b=golden;b[at]^=1;LabelRefused(b);}
  for(unsigned id=1;id<=8;++id){
    const auto at=Offset(golden,id);auto b=golden;Put(b,at,99,2);LabelRefused(b);
    b=golden;Put(b,at+2,255,1);LabelRefused(b);b=golden;Put(b,at+3,1,1);LabelRefused(b);
    b=golden;Put(b,at+4,0xffffffff,4);LabelRefused(b);
    b=golden;b.erase(at,8+Get(b,at+4,4));Put(b,8,b.size(),4);Put(b,12,7,4);LabelRefused(b);
    b=golden;b.insert(at,b.substr(at,8+Get(b,at+4,4)));Put(b,8,b.size(),4);Put(b,12,9,4);LabelRefused(b);
  }
  for(unsigned id:{1u,7u}){
    for(unsigned version=0;version<16;++version)if(version!=7){
      auto b=golden;b[Offset(b,id)+14]=char(version<<4);LabelRefused(b);
    }
    for(unsigned variant:{0u,0x40u,0xc0u}){auto b=golden;b[Offset(b,id)+16]=char(variant);LabelRefused(b);}
    LabelRefused(Replace(golden,id,std::string(16,0)));
    LabelRefused(Replace(golden,id,"00000000-0000-7000-8000-000000000001"));
  }
  for(unsigned id:{2u,8u})LabelRefused(Replace(golden,id,Number(0)));
  LabelRefused(Replace(golden,6,std::string(1,char(2))));
  for(unsigned flag=4;flag<256;++flag)LabelRefused(Replace(golden,4,std::string(3,char(flag))));
  for(unsigned type=0;type<256;++type)if(type<1||type>3)LabelRefused(Replace(golden,5,std::string(3,char(type))));
  for(unsigned id:{4u,5u}){LabelRefused(Replace(golden,id,std::string(2,1)));LabelRefused(Replace(golden,id,std::string(4,1)));}
  for(auto key:{std::string(),std::string(1,'\0'),std::string(1,char(0xff)),std::string("\xc0\x80")}){
    LabelRefused(Replace(golden,3,List({key,"two","three"})));
  }
  LabelRefused(Replace(golden,3,List({"one","one","three"})));
}
void LabelBinding() {
  const auto initial=LabelMetadata(LabelSchema());
  for(unsigned which=0;which<15;++which){auto m=initial;
    switch(which){
      case 0:m.record.header.object_uuid.value.bytes[15]++;break;
      case 1:m.record.header.kind=c::CatalogRecordKind::table_descriptor;break;
      case 2:m.object_subtype="table";break;case 3:m.definition_version++;break;
      case 4:m.record.header.parent_uuid.value.bytes[15]++;break;case 5:m.owning_schema_uuid={};break;
      case 6:m.default_name_uuid={};break;case 7:m.name_vector_uuid={};break;
      case 8:m.security_policy_uuid={};break;case 9:m.authority_scope=c::CatalogAuthorityScope::cluster;break;
      case 10:m.creator_transaction_uuid.value.bytes[15]++;break;case 11:m.creator_local_transaction_id--;break;
      case 12:m.creator_local_transaction_id++;break;case 13:m.record.payload="schema=CommonLabels";break;
      case 14:m.record.header.parent_uuid.kind=p::UuidKind::schema;break;
    }
    Check(!c::CatalogMetricLabelSchemaMatchesMetadata(m),"label schema common mismatch accepted");
    const auto encoded=c::EncodeCatalogMetadataVersion(m);Check(!encoded.ok()&&encoded.bytes.empty(),"envelope bypassed label schema binding");
  }
  auto next=LabelSchema();next.generation=2;next.labels[0].required=!next.labels[0].required;
  auto successor=LabelMetadata(next);successor.creator_transaction_uuid=Id(p::UuidKind::transaction,22);successor.creator_local_transaction_id=12;
  Check(c::EncodeCatalogMetadataVersion(successor).ok()&&SharedLabelOrigin(initial,successor),"valid schema generation mutation refused");
  successor.record.header.deleted=true;successor.lifecycle=c::CatalogObjectLifecycle::dropped;successor.status=c::CatalogObjectStatus::retired;
  successor.retired_transaction_uuid=successor.creator_transaction_uuid;
  Check(c::EncodeCatalogMetadataVersion(successor).ok()&&SharedLabelOrigin(initial,successor),"label schema retirement lost origin");
  for(unsigned which=0;which<4;++which){
    auto changed=next;
    if(which==0)changed.origin_transaction_uuid.value.bytes[15]++;
    if(which==1)changed.origin_local_transaction_id--;
    if(which==2)changed.label_schema_uuid.bytes[15]++;
    if(which==3)changed.cluster_only=true;
    auto m=LabelMetadata(changed);m.creator_transaction_uuid=successor.creator_transaction_uuid;m.creator_local_transaction_id=12;
    Check(c::EncodeCatalogMetadataVersion(m).ok(),"changed origin fixture not individually valid");
    Check(!SharedLabelOrigin(initial,m),"label schema changed immutable origin");
  }
  auto other=initial;other.record.header.kind=c::CatalogRecordKind::table_descriptor;other.object_subtype="other";other.record.payload="unrelated";
  Check(SharedLabelOrigin(other,other),"label schema redefined unrelated family");
  Check(!SharedLabelOrigin(initial,other)&&!SharedLabelOrigin(other,initial),"schema family swap bypassed origin");
  const auto wrapped=c::EncodeCatalogMetadataVersion(initial);
  if(wrapped.ok())for(std::size_t at:{std::size_t(32),std::size_t(127)}){
    auto bytes=wrapped.bytes;bytes[at]++;std::fill(bytes.begin()+320,bytes.begin()+352,0);
    std::array<unsigned char,32> hash{};SHA256(bytes.data(),bytes.size(),hash.data());std::copy(hash.begin(),hash.end(),bytes.begin()+320);
    Check(!c::DecodeCatalogMetadataVersion(bytes).ok(),"rehash bypassed label schema binding");
  }
  for(unsigned which=0;which<3;++which){auto r=initial.record;
    if(which==0)r.header.kind=c::CatalogRecordKind::metric_descriptor;
    if(which==1)r.header.object_uuid.value.bytes[15]++;
    if(which==2)r.payload="schema=CommonLabels";
    Check(!c::EncodeCatalogTypedRecord(r,3).ok(),"typed record bypassed label schema binding");
  }
}
void LabelDescriptorMatching() {
  const auto schema=LabelSchema();auto descriptor=Descriptor();descriptor.binding.label_schema_generation=schema.generation;
  Check(c::CatalogMetricLabelSchemaMatchesDescriptor(schema,descriptor.definition,descriptor.binding),"exact descriptor snapshot refused");
  for(unsigned which=0;which<10;++which){auto d=descriptor;
    if(which==0)d.binding.label_schema_uuid.bytes[15]++;
    if(which==1)d.binding.label_schema_generation++;
    if(which==2)d.definition.cluster_only=true;
    if(which==3)d.definition.labels[0].key+="x";
    if(which==4)d.definition.labels[0].required=!d.definition.labels[0].required;
    if(which==5)d.definition.labels[0].sensitive=!d.definition.labels[0].sensitive;
    if(which==6)d.definition.labels[0].value_type=m::MetricLabelType::uuid_value;
    if(which==7)std::reverse(d.definition.labels.begin(),d.definition.labels.end());
    if(which==8)d.definition.labels.pop_back();
    if(which==9)d.binding.label_schema_generation=0;
    Check(!c::CatalogMetricLabelSchemaMatchesDescriptor(schema,d.definition,d.binding),"descriptor snapshot mismatch accepted");
  }
  auto empty=schema;empty.labels.clear();descriptor.definition.labels.clear();
  Check(c::CatalogMetricLabelSchemaMatchesDescriptor(empty,descriptor.definition,descriptor.binding),"empty schema confused with absent schema");
  descriptor.binding.label_schema_uuid={};descriptor.binding.label_schema_generation=0;
  Check(!c::CatalogMetricLabelSchemaMatchesDescriptor(empty,descriptor.definition,descriptor.binding),"absent reference substituted with empty schema");
}
void LabelAllocationFailures() {
  auto schema=LabelSchema();schema.labels[0].key=std::string(256,'k');const auto golden=LabelGolden(schema);
  for(unsigned operation=0;operation<2;++operation){
    unsigned injected=0;bool completed=false;
    for(long index=0;index<2048;++index){
      descriptor_allocation_fault::remaining=index;descriptor_allocation_fault::fired=false;
      bool success=false,partial=false;
      try{
        if(operation==0){const auto r=c::EncodeCatalogMetricLabelSchema(schema);success=r.ok();partial=!success&&!r.bytes.empty();}
        else{const auto r=c::DecodeCatalogMetricLabelSchema(golden);success=r.ok();partial=!success&&r.record.has_value();}
      }catch(const std::bad_alloc&){}
      descriptor_allocation_fault::remaining=-1;const bool fired=descriptor_allocation_fault::fired;
      Check(!partial&&LabelGolden(schema)==golden,"label schema allocation failure mutated or partially published");
      if(fired){++injected;Check(!success,"label schema allocation failure succeeded");}
      else{completed=true;Check(success,"label schema failed allocation recovery");break;}
    }
    Check(completed&&injected>10,"label schema allocation sites not reached");
    std::cout<<"label schema allocation operation="<<operation<<" injected="<<injected<<'\n';
  }
}
}
int main(){
  const int descriptor_result=MetricDescriptorRegressionMain();
  const auto before=checks;
  LabelShapesAndLimits();LabelMalformed();LabelBinding();LabelDescriptorMatching();LabelAllocationFailures();
  std::cout<<"label schema checks="<<checks-before<<" combined="<<checks<<" failures="<<failures<<'\n';
  return descriptor_result||failures?1:0;
}
