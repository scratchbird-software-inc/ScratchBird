// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "catalog_metric_descriptor.hpp"
#include <openssl/sha.h>
#include <algorithm>
#include <array>
#include <bit>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <new>

namespace descriptor_allocation_fault {
thread_local long remaining=-1;
thread_local bool fired=false;
void* Allocate(std::size_t size){
  if(remaining==0){fired=true;throw std::bad_alloc();}
  if(remaining>0)--remaining;
  if(void* p=std::malloc(size?size:1))return p;
  throw std::bad_alloc();
}
}
void* operator new(std::size_t n){return descriptor_allocation_fault::Allocate(n);}
void* operator new[](std::size_t n){return descriptor_allocation_fault::Allocate(n);}
void operator delete(void* p) noexcept{std::free(p);}
void operator delete[](void* p) noexcept{std::free(p);}
void operator delete(void* p,std::size_t) noexcept{std::free(p);}
void operator delete[](void* p,std::size_t) noexcept{std::free(p);}
void* operator new(std::size_t n,const std::nothrow_t&) noexcept{try{return descriptor_allocation_fault::Allocate(n);}catch(...){return nullptr;}}
void* operator new[](std::size_t n,const std::nothrow_t&) noexcept{try{return descriptor_allocation_fault::Allocate(n);}catch(...){return nullptr;}}
void operator delete(void* p,const std::nothrow_t&) noexcept{std::free(p);}
void operator delete[](void* p,const std::nothrow_t&) noexcept{std::free(p);}

namespace c=scratchbird::core::catalog;
namespace m=scratchbird::core::metrics;
namespace p=scratchbird::core::platform;
namespace {
unsigned checks=0,failures=0;

void Check(bool ok,const char* why){++checks;if(!ok){++failures;std::cerr<<"FAIL "<<why<<'\n';}}
bool SharedDescriptorOrigin(const c::CatalogMetadataVersion& a,const c::CatalogMetadataVersion& b) {
  const bool family=c::CatalogMetricDescriptorPreservesOrigin(a,b);
  const bool shared=c::CatalogMetadataPreservesFamilyOrigin(a,b);
  Check(shared==family,"shared native origin dispatcher differs from family contract");
  return family;
}
p::TypedUuid Id(p::UuidKind kind,unsigned tag){return {kind,p::Uuid{{1,2,3,4,5,6,0x71,8,0x89,10,11,12,13,14,15,static_cast<p::byte>(tag)}}};}
void Put(std::string& s,std::size_t at,p::u64 value,unsigned width){for(unsigned i=0;i<width;++i)s.at(at+i)=char(value>>(8*i));}
p::u64 Get(const std::string& s,std::size_t at,unsigned width){p::u64 value=0;for(unsigned i=0;i<width;++i)value|=p::u64(static_cast<unsigned char>(s.at(at+i)))<<(8*i);return value;}
std::string Number(p::u64 v,unsigned width=8){std::string s(width,0);Put(s,0,v,width);return s;}
std::string Uuid(const p::Uuid& id){return {reinterpret_cast<const char*>(id.bytes.data()),16};}
void Field(std::string& s,unsigned id,unsigned type,const std::string& bytes){
  const auto at=s.size();s.resize(at+8,0);Put(s,at,id,2);Put(s,at+2,type,1);Put(s,at+4,bytes.size(),4);s+=bytes;
}
std::string List(const std::vector<std::string>& values){
  auto out=Number(values.size(),4);for(const auto& v:values){out+=Number(v.size(),4);out+=v;}return out;
}
std::string Scalar(const m::MetricScalar& v){
  if(const auto* n=std::get_if<p::u64>(&v))return Number(*n);
  if(const auto* n=std::get_if<std::int64_t>(&v))return Number(static_cast<p::u64>(*n));
  if(const auto* n=std::get_if<double>(&v))return Number(std::bit_cast<p::u64>(*n));
  if(const auto* n=std::get_if<m::MetricFloat128>(&v))return {reinterpret_cast<const char*>(n->bytes.data()),16};
  if(const auto* n=std::get_if<m::MetricDecimal128>(&v))return {reinterpret_cast<const char*>(n->bytes.data()),16};
  throw "not an oracle numeric value";
}
// Independent registry/layout oracle: no production schema, packing or decoding.
std::string Golden(const c::CatalogMetricDescriptor& r){
  const auto& d=r.definition;const auto& b=r.binding;
  std::string s(24,0);s.replace(0,4,"SBCV");Put(s,4,1,2);Put(s,6,24,2);
  Put(s,12,30+bool(b.label_schema_generation)+bool(b.rate_source_counter_generation),4);
  Put(s,16,65544,4);Put(s,20,1,2);
  Field(s,1,5,Uuid(b.metric_uuid));Field(s,2,1,Number(b.descriptor_generation));
  Field(s,3,3,d.family);Field(s,4,3,d.namespace_path);
  unsigned klass=0;
  switch(d.type){case m::MetricType::counter:klass=1;break;case m::MetricType::gauge:klass=2;break;
    case m::MetricType::histogram:klass=3;break;case m::MetricType::rate:klass=4;break;
    case m::MetricType::state:klass=5;break;case m::MetricType::sample:klass=6;break;default:klass=255;}
  Field(s,5,1,Number(klass));Field(s,6,1,Number(static_cast<unsigned>(d.value_type)));
  Field(s,7,1,Number(static_cast<unsigned>(d.unit)+1));Field(s,8,3,d.producer_owner);Field(s,9,3,d.help);
  if(b.label_schema_generation)Field(s,10,5,Uuid(b.label_schema_uuid));
  Field(s,11,1,Number(b.label_schema_generation));Field(s,12,5,Uuid(b.retention_policy_uuid));
  Field(s,13,1,Number(b.retention_policy_generation));Field(s,14,5,Uuid(b.visibility_policy_uuid));
  Field(s,15,1,Number(b.visibility_policy_generation));
  Field(s,16,4,d.min_value?Scalar(*d.min_value):"");Field(s,17,4,d.max_value?Scalar(*d.max_value):"");
  auto bounds=Number(d.histogram_buckets.size(),4);for(const auto& v:d.histogram_buckets)bounds+=Scalar(v);
  Field(s,18,4,bounds);Field(s,19,2,Number(d.histogram_cumulative,1));
  auto codes=Number(d.enum_values.size(),4);for(auto v:d.enum_values)codes+=Number(v);
  Field(s,20,4,codes);Field(s,21,2,Number(d.cluster_only,1));Field(s,22,5,Uuid(r.origin_transaction_uuid.value));
  Field(s,23,1,Number(r.origin_local_transaction_id));
  if(b.rate_source_counter_generation)Field(s,24,5,Uuid(b.rate_source_counter_uuid));
  Field(s,25,1,Number(b.rate_source_counter_generation));Field(s,26,1,Number(d.rate_window_nanoseconds));
  Field(s,27,1,Number(static_cast<unsigned>(d.visibility)+1));Field(s,28,3,d.security_family);
  std::vector<std::string> keys;std::string flags,types;
  for(const auto& l:d.labels){keys.push_back(l.key);flags+=char(l.required+2*l.sensitive);types+=char(static_cast<unsigned>(l.value_type)+1);}
  Field(s,29,8,List(keys));Field(s,30,4,flags);Field(s,31,4,types);Field(s,32,8,List(d.aliases));
  Put(s,8,s.size(),4);return s;
}
std::size_t Offset(const std::string& s,unsigned id){
  for(std::size_t at=24;at<s.size();at+=8+Get(s,at+4,4))if(Get(s,at,2)==id)return at;
  throw "missing field";
}
std::string Replace(std::string s,unsigned id,const std::string& value){
  const auto at=Offset(s,id);s.replace(at+8,Get(s,at+4,4),value);Put(s,at+4,value.size(),4);Put(s,8,s.size(),4);return s;
}
c::CatalogMetricDescriptor Descriptor(){
  c::CatalogMetricDescriptor r;auto& d=r.definition;auto& b=r.binding;
  d.family="cpu-observation";d.namespace_path="sys.metrics.cpu.observation";d.producer_owner="runtime";
  d.help="exact \xc3\xa9";d.security_family="metrics";d.type=m::MetricType::gauge;d.value_type=m::MetricScalarType::uint64;
  d.min_value=p::u64(9007199254740993ULL);d.max_value=std::numeric_limits<p::u64>::max();
  d.labels={{"database",true,true,m::MetricLabelType::system_uuid},{"tag",false,false,m::MetricLabelType::text},
    {"data-id",false,true,m::MetricLabelType::uuid_value}};d.aliases={"alias-one","alias-two"};
  b.metric_uuid=Id(p::UuidKind::object,1).value;b.descriptor_generation=1;
  b.label_schema_uuid=Id(p::UuidKind::object,2).value;b.label_schema_generation=3;
  b.retention_policy_uuid=Id(p::UuidKind::object,3).value;b.retention_policy_generation=4;
  b.visibility_policy_uuid=Id(p::UuidKind::object,4).value;b.visibility_policy_generation=5;
  r.origin_transaction_uuid=Id(p::UuidKind::transaction,5);r.origin_local_transaction_id=11;return r;
}
c::CatalogMetadataVersion Metadata(const c::CatalogMetricDescriptor& r){
  c::CatalogMetadataVersion v;v.record.header.kind=c::CatalogRecordKind::metric_descriptor;
  v.record.header.row_uuid=Id(p::UuidKind::row,6);v.record.header.object_uuid={p::UuidKind::object,r.binding.metric_uuid};
  v.record.header.parent_uuid=Id(p::UuidKind::object,7);v.record.payload=Golden(r);v.owning_schema_uuid=Id(p::UuidKind::schema,7);
  v.owner_uuid=Id(p::UuidKind::principal,8);v.audit_uuid=Id(p::UuidKind::object,9);
  v.default_name_uuid=Id(p::UuidKind::object,10);v.name_vector_uuid=Id(p::UuidKind::object,11);
  v.security_policy_uuid={p::UuidKind::object,r.binding.visibility_policy_uuid};v.definition_version=r.binding.descriptor_generation;
  v.creator_transaction_uuid=r.origin_transaction_uuid;v.creator_local_transaction_id=r.origin_local_transaction_id;
  v.schema_epoch=v.security_epoch=v.catalog_generation=v.dependency_generation=v.invalidation_generation=1;
  v.lifecycle=c::CatalogObjectLifecycle::active;v.status=c::CatalogObjectStatus::active;v.trace_search_key="METRIC-DESCRIPTOR-ORACLE";
  v.object_subtype="metric_descriptor";v.retention_class="catalog_history";
  if(r.definition.cluster_only)v.authority_scope=c::CatalogAuthorityScope::cluster;
  return v;
}
void RoundTrip(const c::CatalogMetricDescriptor& r){
  const auto expected=Golden(r);const auto encoded=c::EncodeCatalogMetricDescriptor(r);
  Check(encoded.ok()&&std::string(encoded.bytes.begin(),encoded.bytes.end())==expected,"independent byte oracle mismatch");
  const auto decoded=c::DecodeCatalogMetricDescriptor(expected);
  Check(decoded.ok()&&Golden(*decoded.record)==expected,"independent descriptor decode mismatch");
  const auto metadata=Metadata(r);Check(c::CatalogMetricDescriptorMatchesMetadata(metadata),"valid metadata refused");
  const auto wrapped=c::EncodeCatalogMetadataVersion(metadata);Check(wrapped.ok(),"native envelope refused descriptor");
  if(wrapped.ok()){const auto reread=c::DecodeCatalogMetadataVersion(wrapped.bytes);Check(reread.ok()&&reread.record.record.payload==expected,"envelope lost descriptor");}
  const auto typed=c::EncodeCatalogTypedRecord(metadata.record,3);Check(typed.ok()&&c::DecodeCatalogTypedRecord(typed.row).ok(),"typed record refused descriptor");
}
void Refused(std::string_view bytes){
  const auto r=c::DecodeCatalogMetricDescriptor(bytes);Check(!r.ok()&&!r.record,"malformed bytes published descriptor");
}
void Invalid(const c::CatalogMetricDescriptor& r){
  const auto encoded=c::EncodeCatalogMetricDescriptor(r);Check(!encoded.ok()&&encoded.bytes.empty(),"invalid definition encoded");
}
void Shapes(){
  const auto baseline=Descriptor();RoundTrip(baseline);
  Check(m::MetricDescriptor{}.readiness==m::MetricReadiness::unvalidated,"runtime defaults to activated");
  for(unsigned unit=0;unit<16;++unit){auto r=baseline;r.definition.unit=static_cast<m::MetricUnit>(unit);RoundTrip(r);}
  for(unsigned visibility=0;visibility<5;++visibility){auto r=baseline;r.definition.visibility=static_cast<m::MetricVisibilityScope>(visibility);RoundTrip(r);}
  for(auto type:{m::MetricType::counter,m::MetricType::gauge,m::MetricType::histogram,m::MetricType::rate,m::MetricType::state,m::MetricType::sample}){
    auto r=baseline;r.definition.type=type;
    if(type==m::MetricType::histogram){r.definition.histogram_buckets={p::u64(1),p::u64(9007199254740993ULL),p::u64(-1)};}
    if(type==m::MetricType::state){r.definition.value_type=m::MetricScalarType::enumeration;r.definition.enum_values={0,1,p::u64(-1)};r.definition.min_value.reset();r.definition.max_value.reset();}
    if(type==m::MetricType::rate){r.binding.rate_source_counter_uuid=Id(p::UuidKind::object,12).value;r.binding.rate_source_counter_generation=2;r.definition.rate_window_nanoseconds=1000000000;}
    RoundTrip(r);
  }
  m::MetricFloat128 binary_min{},binary_max{};binary_min.bytes[15]=0x80;binary_max.bytes[14]=0xff;binary_max.bytes[15]=0x3f;binary_max.bytes[0]=1;
  m::MetricDecimal128 decimal_min{},decimal_max{};
  decimal_min.bytes[15]=0x80;decimal_max.bytes[0]=1;
  for(unsigned i=0;i<14;++i)if(6176u&(1u<<i))decimal_max.bytes[(113+i)/8]|=1u<<((113+i)%8);
  const std::array<std::pair<m::MetricScalar,m::MetricScalar>,5> ranges{{
    {p::u64(0),p::u64(-1)},{std::numeric_limits<std::int64_t>::min(),std::numeric_limits<std::int64_t>::max()},
    {-0.,std::numeric_limits<double>::max()},{binary_min,binary_max},{decimal_min,decimal_max}}};
  for(unsigned i=0;i<ranges.size();++i){
    auto r=baseline;r.definition.value_type=static_cast<m::MetricScalarType>(i+1);
    r.definition.min_value=ranges[i].first;r.definition.max_value=ranges[i].second;RoundTrip(r);
    r.definition.type=m::MetricType::histogram;r.definition.histogram_buckets={ranges[i].first,ranges[i].second};
    RoundTrip(r);r.definition.histogram_cumulative=false;RoundTrip(r);
  }
  for(unsigned type=6;type<=9;++type){
    auto r=baseline;r.definition.value_type=static_cast<m::MetricScalarType>(type);
    r.definition.min_value.reset();r.definition.max_value.reset();
    if(type==9)r.definition.enum_values={1,2};
    RoundTrip(r);
  }
  auto r=baseline;r.definition.labels.clear();r.binding.label_schema_uuid={};r.binding.label_schema_generation=0;RoundTrip(r);
  r=baseline;r.definition.cluster_only=true;r.definition.namespace_path="cluster.sys.metrics.cpu.observation";RoundTrip(r);
}
void Malformed(){
  const auto golden=Golden(Descriptor());
  for(std::size_t n=0;n<golden.size();++n)Refused(std::string_view(golden).substr(0,n));
  Refused(golden+"x");Refused("metric_uuid=seed-metric\n");Refused(std::string(131073,'x'));
  for(unsigned at:{0u,4u,6u,8u,12u,16u,20u,22u}){auto b=golden;b[at]^=1;Refused(b);}
  for(unsigned id=1;id<=32;++id){if(id==24)continue;const auto at=Offset(golden,id);
    auto b=golden;Put(b,at,100,2);Refused(b);b=golden;Put(b,at+2,255,1);Refused(b);
    b=golden;Put(b,at+3,1,1);Refused(b);b=golden;Put(b,at+4,0xffffffff,4);Refused(b);
    b=golden;b.erase(at,8+Get(b,at+4,4));Put(b,8,b.size(),4);Put(b,12,30,4);Refused(b);
    b=golden;b.insert(at,b.substr(at,8+Get(b,at+4,4)));Put(b,8,b.size(),4);Put(b,12,32,4);Refused(b);
  }
  for(unsigned id:{1u,10u,12u,14u,22u}){
    for(unsigned version=0;version<16;++version)if(version!=7){auto b=golden;b[Offset(b,id)+14]=char(version<<4);Refused(b);}
    for(unsigned variant:{0u,0x40u,0xc0u}){auto b=golden;b[Offset(b,id)+16]=char(variant);Refused(b);}
    Refused(Replace(golden,id,std::string(16,0)));
    Refused(Replace(golden,id,"019abcdef-0123-7123-8123-123456789abc"));
  }
  for(unsigned id:{2u,11u,13u,15u,23u})Refused(Replace(golden,id,Number(0)));
  for(unsigned id:{5u,6u,7u,27u})for(auto code:{0u,255u})Refused(Replace(golden,id,Number(code)));
  for(unsigned id:{3u,4u,8u,9u,28u}){Refused(Replace(golden,id,std::string(1,'\0')));Refused(Replace(golden,id,std::string(1,char(0xff))));}
  for(unsigned id:{16u,17u})for(unsigned size:{1u,7u,9u,16u,17u})Refused(Replace(golden,id,std::string(size,0)));
  Refused(Replace(golden,18,Number(0xffffffff,4)));Refused(Replace(golden,20,Number(0xffffffff,4)));
  Refused(Replace(golden,18,Number(0,4)+"x"));Refused(Replace(golden,20,Number(0,4)+"x"));
  Refused(Replace(golden,30,std::string(3,char(4))));Refused(Replace(golden,30,std::string(2,0)));
  Refused(Replace(golden,31,std::string(3,char(4))));Refused(Replace(golden,31,std::string(3,0)));
  Refused(Replace(golden,29,List({"same","same","third"})));Refused(Replace(golden,32,List({"same","same"})));
  Refused(Replace(golden,29,List({"","two","three"})));Refused(Replace(golden,32,List({""})));
  Refused(Replace(golden,25,Number(1)));Refused(Replace(golden,26,Number(1)));
  auto r=Descriptor();r.definition.type=m::MetricType::rate;Invalid(r);
  r.binding.rate_source_counter_uuid=Id(p::UuidKind::object,13).value;r.binding.rate_source_counter_generation=3;r.definition.rate_window_nanoseconds=1;
  const auto rate=Golden(r);RoundTrip(r);
  Refused(Replace(rate,25,Number(0)));Refused(Replace(rate,26,Number(0)));
  auto b=rate;const auto at=Offset(b,24);b.erase(at,24);Put(b,8,b.size(),4);Put(b,12,31,4);Refused(b);
  r=Descriptor();r.definition.unit=m::MetricUnit::count;Invalid(r);r.definition.unit=m::MetricUnit::state;Invalid(r);
  r=Descriptor();r.definition.type=m::MetricType::derived;Invalid(r);
  r=Descriptor();r.definition.namespace_path="cluster.sys.metrics.cpu";Invalid(r);
  r=Descriptor();r.definition.namespace_path="sys.metrics.";Invalid(r);
  r=Descriptor();r.definition.type=m::MetricType::histogram;r.definition.histogram_buckets={p::u64(2),p::u64(1)};Invalid(r);Refused(Golden(r));
  r.definition.histogram_buckets={p::u64(1),p::u64(1)};Invalid(r);Refused(Golden(r));
  r=Descriptor();r.definition.histogram_cumulative=false;Invalid(r);Refused(Golden(r));
  r=Descriptor();r.definition.min_value=std::int64_t(1);Invalid(r);
  r=Descriptor();r.definition.min_value=p::u64(-1);r.definition.max_value=p::u64(0);Invalid(r);Refused(Golden(r));
  r=Descriptor();r.definition.value_type=m::MetricScalarType::float64;r.definition.min_value=0.;r.definition.max_value=std::numeric_limits<double>::infinity();Invalid(r);Refused(Golden(r));
}
void Binding(){
  const auto initial=Metadata(Descriptor());
  for(unsigned which=0;which<17;++which){auto v=initial;
    switch(which){
      case 0:v.record.header.object_uuid.value.bytes[15]++;break;case 1:v.record.header.kind=c::CatalogRecordKind::table_descriptor;break;
      case 2:v.object_subtype="table";break;case 3:v.definition_version++;break;
      case 4:v.record.header.parent_uuid.value.bytes[15]++;break;case 5:v.owning_schema_uuid={};break;
      case 6:v.default_name_uuid={};break;case 7:v.name_vector_uuid={};break;
      case 8:v.security_policy_uuid.value.bytes[15]++;break;case 9:v.authority_scope=c::CatalogAuthorityScope::cluster;break;
      case 10:v.creator_transaction_uuid.value.bytes[15]++;break;case 11:v.creator_local_transaction_id--;break;
      case 12:v.creator_local_transaction_id++;break;case 13:v.record.payload="metric_uuid=seed";break;
      case 14:v.record.header.object_uuid.kind=p::UuidKind::schema;break;case 15:v.record.header.parent_uuid.kind=p::UuidKind::schema;break;
      case 16:v.security_policy_uuid={};break;
    }
    Check(!c::CatalogMetricDescriptorMatchesMetadata(v),"bad common binding accepted");
    const auto out=c::EncodeCatalogMetadataVersion(v);Check(!out.ok()&&out.bytes.empty(),"envelope bypassed common binding");
  }
  auto next=Descriptor();next.binding.descriptor_generation=2;
  auto successor=Metadata(next);successor.creator_transaction_uuid=Id(p::UuidKind::transaction,22);successor.creator_local_transaction_id=12;
  Check(c::EncodeCatalogMetadataVersion(successor).ok()&&SharedDescriptorOrigin(initial,successor),"valid successor refused");
  successor.record.header.deleted=true;successor.lifecycle=c::CatalogObjectLifecycle::dropped;successor.status=c::CatalogObjectStatus::retired;
  successor.retired_transaction_uuid=successor.creator_transaction_uuid;
  Check(c::EncodeCatalogMetadataVersion(successor).ok()&&SharedDescriptorOrigin(initial,successor),"retirement lost origin");
  for(unsigned i=0;i<4;++i){
    auto r=next;if(i==0)r.origin_transaction_uuid.value.bytes[15]++;if(i==1)r.origin_local_transaction_id--;
    if(i==2)r.binding.metric_uuid.bytes[15]++;
    if(i==3){r.definition.cluster_only=true;r.definition.namespace_path="cluster.sys.metrics.changed";}
    auto v=Metadata(r);v.creator_transaction_uuid=successor.creator_transaction_uuid;v.creator_local_transaction_id=12;
    Check(c::EncodeCatalogMetadataVersion(v).ok(),"individually valid changed-origin fixture refused");
    Check(!SharedDescriptorOrigin(initial,v),"version changed immutable origin");
  }
  auto other=initial;other.record.header.kind=c::CatalogRecordKind::table_descriptor;other.object_subtype="table";other.record.payload="unrelated";
  Check(!SharedDescriptorOrigin(initial,other)&&!SharedDescriptorOrigin(other,initial),"family swap bypassed origin");
  Check(SharedDescriptorOrigin(other,other),"unrelated family redefined");
  const auto wrapped=c::EncodeCatalogMetadataVersion(initial);
  if(wrapped.ok())for(std::size_t at:{std::size_t(32),std::size_t(127)}){
    auto bytes=wrapped.bytes;bytes[at]++;std::fill(bytes.begin()+320,bytes.begin()+352,0);
    std::array<unsigned char,32> hash{};SHA256(bytes.data(),bytes.size(),hash.data());std::copy(hash.begin(),hash.end(),bytes.begin()+320);
    Check(!c::DecodeCatalogMetadataVersion(bytes).ok(),"rehash bypassed binding");
  }
  for(unsigned i=0;i<3;++i){auto r=initial.record;
    if(i==0)r.payload="metric_uuid=seed";
    if(i==1)r.header.kind=c::CatalogRecordKind::policy;
    if(i==2)r.header.object_uuid.value.bytes[15]++;
    Check(!c::EncodeCatalogTypedRecord(r,3).ok(),"typed header admitted descriptor confusion");
  }
}
void Limits(){
  auto r=Descriptor();
  for(auto member:{&m::MetricDescriptorDefinition::family,&m::MetricDescriptorDefinition::producer_owner,
                   &m::MetricDescriptorDefinition::security_family}){
    r=Descriptor();r.definition.*member=std::string(4096,'x');RoundTrip(r);
    (r.definition.*member)+='x';Invalid(r);Refused(Golden(r));
  }
  r=Descriptor();r.definition.help=std::string(16384,'h');RoundTrip(r);
  r.definition.help+='h';Invalid(r);Refused(Golden(r));
  r=Descriptor();r.definition.aliases={std::string(4096,'a')};RoundTrip(r);
  r.definition.aliases[0]+='a';Invalid(r);Refused(Golden(r));
  r=Descriptor();r.definition.labels[0].key=std::string(4096,'k');RoundTrip(r);
  r.definition.labels[0].key+='k';Invalid(r);Refused(Golden(r));
  r=Descriptor();r.definition.type=m::MetricType::histogram;
  for(p::u64 i=0;i<4096;++i)r.definition.histogram_buckets.push_back(i);
  RoundTrip(r);r.definition.histogram_buckets.push_back(p::u64(4096));Invalid(r);Refused(Golden(r));
  r=Descriptor();r.definition.type=m::MetricType::state;r.definition.value_type=m::MetricScalarType::enumeration;
  r.definition.min_value.reset();r.definition.max_value.reset();
  for(p::u64 i=0;i<4096;++i)r.definition.enum_values.push_back(i);
  RoundTrip(r);r.definition.enum_values.push_back(4096);Invalid(r);Refused(Golden(r));
  r.definition.enum_values.pop_back();r.definition.enum_values.back()=0;Invalid(r);Refused(Golden(r));
  r=Descriptor();r.definition.labels.clear();
  for(unsigned i=0;i<1024;++i)r.definition.labels.push_back({"key"+std::to_string(i),false,false,m::MetricLabelType::text});
  RoundTrip(r);r.definition.labels.push_back({"extra",false,false,m::MetricLabelType::text});Invalid(r);Refused(Golden(r));
  r=Descriptor();r.definition.aliases.clear();
  for(unsigned i=0;i<1024;++i)r.definition.aliases.push_back("alias"+std::to_string(i));
  RoundTrip(r);r.definition.aliases.push_back("extra");Invalid(r);Refused(Golden(r));
  // Each field below fits its own cap but their combined payload exceeds 128KiB.
  r=Descriptor();r.definition.type=m::MetricType::histogram;r.definition.value_type=m::MetricScalarType::float128;
  r.definition.min_value.reset();r.definition.max_value.reset();r.definition.labels.clear();r.definition.aliases.clear();
  for(unsigned i=0;i<4096;++i){m::MetricFloat128 v;v.bytes[0]=i&255;v.bytes[1]=i>>8;r.definition.histogram_buckets.push_back(v);}
  for(unsigned i=0;i<15;++i)r.definition.labels.push_back({std::string(4090,char('a'+i)),false,false,m::MetricLabelType::text});
  r.definition.aliases={std::string(4096,'a'),std::string(4096,'b')};Invalid(r);Refused(Golden(r));
  // Definition and binding are distinct: absent references never gain IDs from names.
  for(unsigned i=0;i<6;++i){r=Descriptor();
    if(i==0)r.binding.metric_uuid={};
    if(i==1)r.binding.retention_policy_uuid={};
    if(i==2)r.binding.visibility_policy_uuid={};
    if(i==3)r.binding.label_schema_generation=0;
    if(i==4)r.origin_transaction_uuid.kind=p::UuidKind::object;
    if(i==5)r.binding.label_schema_uuid={};
    Invalid(r);
  }
}
void AllocationFailures(){
  auto r=Descriptor();r.definition.family=std::string(256,'f');r.definition.help=std::string(512,'h');
  r.definition.aliases={std::string(256,'a'),std::string(256,'b')};r.definition.labels[0].key=std::string(256,'k');
  const auto golden=Golden(r);
  for(unsigned operation=0;operation<2;++operation){
    unsigned injected=0;bool completed=false;
    for(long index=0;index<2048;++index){
      descriptor_allocation_fault::remaining=index;descriptor_allocation_fault::fired=false;
      bool succeeded=false,partial=false;
      try{
        if(operation==0){const auto result=c::EncodeCatalogMetricDescriptor(r);succeeded=result.ok();partial=!succeeded&&!result.bytes.empty();}
        else{const auto result=c::DecodeCatalogMetricDescriptor(golden);succeeded=result.ok();partial=!succeeded&&result.record.has_value();}
      }catch(const std::bad_alloc&){}
      descriptor_allocation_fault::remaining=-1;
      const bool fired=descriptor_allocation_fault::fired;
      Check(!partial,"allocation failure published partial definition");
      Check(Golden(r)==golden,"allocation failure mutated input definition");
      if(fired){++injected;Check(!succeeded,"allocation failure reported success");}
      else{Check(succeeded,"valid operation did not recover after allocation faults");completed=true;break;}
    }
    Check(completed&&injected>10,"allocation fault sweep did not reach all allocation sites");
    std::cout<<"descriptor allocation operation="<<operation<<" injected="<<injected<<'\n';
  }
}
}
int main(){Shapes();Malformed();Binding();Limits();AllocationFailures();std::cout<<"metric descriptor native checks="<<checks<<" failures="<<failures<<'\n';return failures?1:0;}
