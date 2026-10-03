// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#define main NativeCatalogValueRegressionMain
#include "native_catalog_value_memory_gate.cpp"
#undef main
#include "catalog_metric_label_schema.hpp"

namespace {
namespace m = scratchbird::core::metrics;
static_assert(std::forward_iterator<c::CatalogMetricLabelSequenceView::Iterator>);
std::string_view View(const Bytes& b) { return {reinterpret_cast<const char*>(b.data()),b.size()}; }
Bytes Keys(const std::vector<std::string>& keys) {
  Bytes out(4); Num(out,0,4,keys.size());
  for (const auto& key : keys) {
    const auto at=out.size(); out.resize(at+4+key.size()); Num(out,at,4,key.size());
    std::copy(key.begin(),key.end(),out.begin()+at+4);
  }
  return out;
}
std::vector<WireField> LabelFields(const std::vector<std::string>& keys={"key"},unsigned flags=0,unsigned type=1) {
  return {{1,T::engine_identity,Identity(1)},{2,T::unsigned_integer,Integer(1)},
      {3,T::utf8_text_list,Keys(keys)},{4,T::opaque_bytes,Bytes(keys.size(),flags)},
      {5,T::opaque_bytes,Bytes(keys.size(),type)},{6,T::boolean,{0}},
      {7,T::engine_identity,Identity(8)},{8,T::unsigned_integer,Integer(1)}};
}
auto Decode(const Bytes& b) { return NoHeap([&]{return c::DecodeCatalogMetricLabelSchemaView(View(b));}); }
void Refuse(const Bytes& b,E error) {
  const auto v=Decode(b); Check(v.error==error&&!v.record,"borrowed exact refusal no partial labels");
  const auto own=c::DecodeCatalogMetricLabelSchema(View(b));
  Check(own.error==error&&!own.record,"owning exact refusal parity");
}
c::CatalogMetadataVersion Metadata(const Bytes& b,bool cluster=false) {
  c::CatalogMetadataVersion x; x.record.header.kind=c::CatalogRecordKind::metric_label_schema;
  x.record.header.object_uuid={p::UuidKind::object,Id(1)};
  x.record.header.parent_uuid={p::UuidKind::object,Id(3)}; x.owning_schema_uuid={p::UuidKind::schema,Id(3)};
  x.default_name_uuid={p::UuidKind::object,Id(4)}; x.name_vector_uuid={p::UuidKind::object,Id(5)};
  x.security_policy_uuid={p::UuidKind::object,Id(6)};
  x.creator_transaction_uuid={p::UuidKind::transaction,Id(8)}; x.creator_local_transaction_id=1;
  x.definition_version=1; x.object_subtype="metric_label_schema"; x.record.payload=View(b);
  x.authority_scope=cluster?c::CatalogAuthorityScope::cluster:c::CatalogAuthorityScope::local; return x;
}
void Matrix() {
  const c::CatalogMetricLabelSequenceView empty;
  Check(empty.size()==0&&empty.begin()==empty.end(),"default empty range");
  // Must precede first use of the owning static schema.
  Check(Decode(Wire(LabelFields({}),65545)).ok(),"first use empty schema no heap");
  for(unsigned count:{0u,1u,2u,17u,1024u}) for(unsigned flags=0;flags<4;++flags)
    for(unsigned type=1;type<=3;++type) for(bool cluster:{false,true}) {
    std::vector<std::string> keys; for(unsigned i=0;i<count;++i) keys.push_back("key-\xc3\xa9-"+std::to_string(i));
    auto fields=LabelFields(keys,flags,type); fields[5].value={static_cast<p::byte>(cluster)};
    const auto b=Wire(fields,65545); const auto v=Decode(b);
    Check(v.ok()&&v.record->labels.size()==count,"all counts flags types scopes");
    Check(v.record->label_schema_uuid==Id(1)&&v.record->origin_transaction_uuid.value==Id(8)&&
        v.record->generation==1&&v.record->origin_local_transaction_id==1&&v.record->cluster_only==cluster,"native scalar identity fields");
    const auto copy=*v.record;
    NoHeap([&]{
      auto it=copy.labels.begin();
      for(unsigned i=0;i<count;++i) {
        const auto previous=it++; const auto a=*previous;
        Check(a.key==keys[i]&&a.required==bool(flags&1)&&a.sensitive==bool(flags&2),"ordered exact label and flags");
        Check(a.value_type==(type==1?m::MetricLabelType::text:type==2?m::MetricLabelType::system_uuid:m::MetricLabelType::uuid_value),"exact label type");
        Check(a.key.data()>=View(b).data()&&a.key.data()+a.key.size()<=View(b).data()+b.size(),"label borrows input not temporary result");
        auto independent=previous; ++independent; Check(independent==it,"multipass forward iterator");
      }
      Check(it==copy.labels.end(),"exact end after all labels"); return true;
    });
    const auto own=c::DecodeCatalogMetricLabelSchema(View(b)); Check(own.ok(),"owning matrix decode");
    Check(c::EncodeCatalogMetricLabelSchema(*own.record).bytes==b,"independent canonical byte oracle");
    auto meta=Metadata(b,cluster);
    Check(NoHeap([&]{return c::CatalogMetricLabelSchemaMatchesMetadata(meta)&&c::CatalogMetadataPreservesFamilyOrigin(meta,meta);}),"metadata and shared origin no hidden allocations");
    meta.authority_scope=cluster?c::CatalogAuthorityScope::local:c::CatalogAuthorityScope::cluster;
    Check(!NoHeap([&]{return c::CatalogMetricLabelSchemaMatchesMetadata(meta);}),"scope mismatch rejected");
  }
}
void Bounds() {
  auto fields=LabelFields({std::string(4096,'K')}); fields[1].value=Integer(~p::u64{0});fields[7].value=Integer(~p::u64{0});
  auto b=Wire(fields,65545); auto v=Decode(b);
  Check(v.ok()&&(*v.record->labels.begin()).key.size()==4096&&v.record->generation==~p::u64{0}&&
      v.record->origin_local_transaction_id==~p::u64{0},"maximum key and counters");
  const auto owned=c::DecodeCatalogMetricLabelSchema(View(b));std::fill(b.begin(),b.end(),0);
  Check(owned.ok()&&owned.record->labels[0].key==std::string(4096,'K'),"owning result independent from input");
  std::vector<std::string> keys;
  for(unsigned i=0;i<1024;++i){auto key=std::to_string(i);key.resize(i==1023?56:60,'K');keys.push_back(key);}
  b=Wire(LabelFields(keys),65545);Check(b.size()==67721&&Decode(b).ok(),"exact total and nested-list maximum");
  keys.back()+='K';Refuse(Wire(LabelFields(keys),65545),E::size_limit);
  keys={"a","b","c"};
  for(unsigned i=0;i<3;++i)for(unsigned j=0;j<3;++j)if(i!=j){auto duplicate=keys;duplicate[j]=duplicate[i];Refuse(Wire(LabelFields(duplicate),65545),E::invalid_value);}
  Refuse(Wire(LabelFields(std::vector<std::string>(1025,"K")),65545),E::size_limit);
  Refuse(Wire(LabelFields({std::string(4097,'K')}),65545),E::invalid_value);
  for(const auto& key:std::vector<std::string>{"",std::string("a\0b",3),"\xc0\x80","\xed\xa0\x80","\xf4\x90\x80\x80","\xe2\x82"})
    Refuse(Wire(LabelFields({key}),65545),E::invalid_value);
  for(const auto& key:std::vector<std::string>{"\x7f","\xc2\x80","\xdf\xbf","\xe0\xa0\x80","\xed\x9f\xbf","\xef\xbf\xbf","\xf0\x90\x80\x80","\xf4\x8f\xbf\xbf"})
    Check(Decode(Wire(LabelFields({key}),65545)).ok(),"UTF8 scalar boundaries accepted");
  for(unsigned octet=0;octet<256;++octet){
    const auto one=Wire(LabelFields({"key"},octet),65545);
    if(octet<4)Check(Decode(one).ok(),"valid flag byte");else Refuse(one,E::invalid_value);
    const auto two=Wire(LabelFields({"key"},0,octet),65545);
    if(octet>=1&&octet<=3)Check(Decode(two).ok(),"valid type byte");else Refuse(two,E::invalid_value);
    const auto key=std::string(1,static_cast<char>(octet)); const auto three=Wire(LabelFields({key}),65545);
    if(octet>0&&octet<128)Check(Decode(three).ok(),"single ASCII key");else Refuse(three,E::invalid_value);
  }
}
void MalformedAndOrigin() {
  const auto good=Wire(LabelFields(),65545);
  for(std::size_t n=0;n<good.size();++n){const auto r=Decode(Bytes(good.begin(),good.begin()+n));Check(!r.ok()&&!r.record,"every truncation no partial result");}
  for(unsigned field=0;field<8;++field){
    auto f=LabelFields();f.erase(f.begin()+field);Refuse(Wire(f,65545),E::missing_field);
    f=LabelFields();f[field].type=T::user_uuid_data;Refuse(Wire(f,65545),E::type_mismatch);
  }
  for(unsigned field:{1u,7u}){auto f=LabelFields();f[field].value=Integer(0);Refuse(Wire(f,65545),E::invalid_value);}
  for(unsigned field:{3u,4u})for(unsigned count:{0u,2u}){auto f=LabelFields();f[field].value=Bytes(count,1);Refuse(Wire(f,65545),E::invalid_value);}
  for(unsigned field:{0u,6u})for(unsigned version=0;version<16;++version)if(version!=7){
    auto f=LabelFields();f[field].value[6]=version<<4;Refuse(Wire(f,65545),E::invalid_value);}
  auto f=LabelFields();f[5].value={2};Refuse(Wire(f,65545),E::invalid_value);
  f=LabelFields();Num(f[2].value,0,4,2);Refuse(Wire(f,65545),E::invalid_value);
  auto a=Metadata(good),b=a; b.definition_version=2;b.creator_local_transaction_id=2;b.creator_transaction_uuid.value=Id(9);
  f=LabelFields({"replacement-key"});f[1].value=Integer(2);b.record.payload=View(Wire(f,65545));
  Check(NoHeap([&]{return c::CatalogMetadataPreservesFamilyOrigin(a,b);}),"label replacement retains original creator");
  f[6].value=Identity(9);b.record.payload=View(Wire(f,65545));
  Check(!NoHeap([&]{return c::CatalogMetadataPreservesFamilyOrigin(a,b);}),"origin substitution refused without heap");
}
}
int main() {
  try { Matrix(); Bounds(); MalformedAndOrigin();
    Check(attempted_allocations==0,"borrowed path attempted heap allocation");
    std::cout<<"native label schema memory: "<<checks<<" checks, "<<attempted_allocations<<" allocation attempts\n";
    return 0;
  } catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
    catch(...){return 1;}
}
