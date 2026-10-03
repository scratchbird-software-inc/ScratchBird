// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#define main NativeCatalogValueRegressionMain
#include "native_catalog_value_memory_gate.cpp"
#undef main
#include "catalog_metric_series.hpp"
#include "metric_label_key.hpp"

namespace {
namespace m=scratchbird::core::metrics;
static_assert(std::forward_iterator<c::CatalogMetricSeriesLabelsView::Iterator>);
std::string_view View(const Bytes& b){return {reinterpret_cast<const char*>(b.data()),b.size()};}
struct Label {std::string key;unsigned type;Bytes value;};
Bytes Pack(const std::vector<Bytes>& values){
  Bytes b(4);Num(b,0,4,values.size());
  for(const auto& v:values){const auto at=b.size();b.resize(at+4+v.size());Num(b,at,4,v.size());std::copy(v.begin(),v.end(),b.begin()+at+4);}
  return b;
}
std::vector<WireField> Series(const std::vector<Label>& labels={{"key",1,{'v'}}},bool cluster=false){
  std::vector<Bytes> keys,values;Bytes types;
  for(const auto& l:labels){keys.emplace_back(l.key.begin(),l.key.end());values.push_back(l.value);types.push_back(l.type);}
  return {{1,T::engine_identity,Identity(1)},{2,T::unsigned_integer,Integer(1)},
    {3,T::engine_identity,Identity(3)},{4,T::engine_identity,Identity(4)},{5,T::user_uuid_data,cluster?Identity(5):Bytes(16)},
    {6,T::engine_identity,Identity(6)},{7,T::unsigned_integer,Integer(1)},
    {8,T::user_uuid_data,Identity(8)},{9,T::unsigned_integer,Integer(1)},
    {10,T::engine_identity,Identity(10)},{11,T::unsigned_integer,Integer(1)},
    {12,T::engine_identity,Identity(12)},{13,T::unsigned_integer,Integer(1)},
    {14,T::user_uuid_data,Bytes(16)},{15,T::unsigned_integer,Integer(0)},
    {16,T::utf8_text_list,Pack(keys)},{17,T::opaque_bytes,types},{18,T::opaque_bytes,Pack(values)},
    {19,T::engine_identity,Identity(19)},{20,T::unsigned_integer,Integer(1)}};
}
auto Decode(const Bytes& b){return NoHeap([&]{return c::DecodeCatalogMetricSeriesView(View(b));});}
void Refuse(const Bytes& b,E error=E::invalid_value){
  const auto v=Decode(b);Check(v.error==error&&!v.record,"borrowed exact refusal without partial series");
  const auto own=c::DecodeCatalogMetricSeries(View(b));Check(own.error==error&&!own.record,"owning exact refusal parity");
}
c::CatalogMetadataVersion Metadata(const Bytes& b,bool cluster=false){
  c::CatalogMetadataVersion x;x.record.header.kind=c::CatalogRecordKind::metric_series;
  x.record.header.object_uuid={p::UuidKind::object,Id(1)};
  x.record.header.parent_uuid={p::UuidKind::object,Id(30)};x.owning_schema_uuid={p::UuidKind::schema,Id(30)};
  x.default_name_uuid={p::UuidKind::object,Id(31)};x.name_vector_uuid={p::UuidKind::object,Id(32)};
  x.security_policy_uuid={p::UuidKind::object,Id(12)};x.creator_transaction_uuid={p::UuidKind::transaction,Id(19)};
  x.creator_local_transaction_id=1;x.definition_version=1;x.object_subtype="metric_series";x.record.payload=View(b);
  x.authority_scope=cluster?c::CatalogAuthorityScope::cluster:c::CatalogAuthorityScope::local;return x;
}
void Matrices(){
  c::CatalogMetricSeriesLabelsView empty;
  Check(empty.empty()&&empty.begin()==empty.end(),"default empty series range");
  Check(Decode(Wire(Series({}),65546)).ok(),"first-use schema has no heap");
  for(unsigned count:{0u,1u,2u,16u,1024u})for(unsigned type=1;type<=3;++type)for(bool cluster:{false,true}){
    std::vector<Label> labels;
    for(unsigned i=0;i<count;++i){auto key=std::to_string(i);key=std::string(4-key.size(),'0')+key;
      auto value=type==1?Bytes{'v',0,0xc3,0xa9}:Identity(i);if(type==3)value[6]=(value[6]&15)|0x40;
      labels.push_back({key,type,value});}
    const auto b=Wire(Series(labels,cluster),65546);const auto v=Decode(b);
    Check(v.ok()&&v.record->labels.size()==count&&v.record->series_uuid==Id(1),"count type scope matrix");
    Check(v.record->binding.database_uuid==Id(3)&&v.record->binding.node_uuid==Id(4)&&
      v.record->binding.cluster_uuid==(cluster?Id(5):p::Uuid{})&&v.record->binding.metric_uuid==Id(6)&&
      v.record->binding.label_schema_uuid==Id(8)&&v.record->binding.retention_policy_uuid==Id(10)&&
      v.record->binding.visibility_policy_uuid==Id(12)&&v.record->origin_transaction_uuid.value==Id(19),"exact binary owner bindings");
    const auto copied=*v.record;
    NoHeap([&]{auto it=copied.labels.begin();
      for(const auto& expected:labels){const auto prior=it++;const auto l=*prior;
        Check(l.key==expected.key,"exact ordered key");auto independent=prior;++independent;Check(independent==it,"multipass iterator");
        Check(l.key.data()>=View(b).data()&&l.key.data()+l.key.size()<=View(b).data()+b.size(),"borrowed source key");
        Check(l.type==(type==1?m::MetricLabelType::text:type==2?m::MetricLabelType::system_uuid:m::MetricLabelType::uuid_value),"exact type");
        if(type==1){const auto text=std::get<std::string_view>(l.value);Check(text==View(expected.value),"exact borrowed NUL/UTF8 text");
          Check(text.data()>=View(b).data()&&text.data()+text.size()<=View(b).data()+b.size(),"value aliases source");}
        else {const auto id=std::get<p::Uuid>(l.value);Check(std::equal(id.bytes.begin(),id.bytes.end(),expected.value.begin()),"UUID bytes preserved");}
      }Check(it==copied.labels.end()&&copied.labels==v.record->labels,"exact end and sequence equality");return true;});
    const auto own=c::DecodeCatalogMetricSeries(View(b));Check(own.ok()&&c::EncodeCatalogMetricSeries(*own.record).bytes==b,"owning canonical parity");
    auto meta=Metadata(b,cluster);
    Check(NoHeap([&]{return c::CatalogMetricSeriesMatchesMetadata(meta)&&c::CatalogMetadataPreservesFamilyOrigin(meta,meta);}),"metadata shared origin no allocations");
  }
  for(unsigned type:{2u,3u})for(unsigned version=0;version<16;++version)for(unsigned variant=0;variant<4;++variant){
    auto id=Identity(7);id[6]=version<<4;id[8]=variant<<6;
    const auto b=Wire(Series({{"key",type,id}}),65546);
    const bool valid=variant==2&&(type==2?version==7:version>=1&&version<=7);
    if(valid)Check(Decode(b).ok(),"native/user UUID version and variant");else Refuse(b);
  }
  for(unsigned present=0;present<2;++present)for(p::u64 gen:{p::u64{0},p::u64{1},~p::u64{0}})
    for(unsigned field:{7u,13u}){
    auto f=Series({});f[field].value=present?Identity(field+1):Bytes(16);f[field+1].value=Integer(gen);
    const auto b=Wire(f,65546);if(bool(present)==bool(gen))Check(Decode(b).ok(),"optional identity generation pairing");else Refuse(b);
  }
}
void OwnersAndOrigin(){
  for(const auto& pair:std::array<std::pair<std::string,unsigned>,3>{{{"database_uuid",3},{"node_uuid",4},{"cluster_uuid",5}}})
    for(unsigned type=1;type<=3;++type)for(unsigned which=0;which<3;++which){
    const auto value=type==1?Bytes{'n','a','m','e'}:which==0?Identity(pair.second):which==1?Identity(99):Bytes(16);
    const auto b=Wire(Series({{pair.first,type,value}},true),65546);
    if(type==2&&which==0)Check(Decode(b).ok(),"reserved owner exact native scope");else Refuse(b);
  }
  Refuse(Wire(Series({{"cluster_uuid",2,Identity(5)}}),65546));
  const auto raw=Wire(Series(),65546);auto a=Metadata(raw),b=a;
  auto f=Series();f[1].value=Integer(2);f[10].value=Integer(2);b.record.payload=View(Wire(f,65546));
  b.definition_version=2;b.creator_local_transaction_id=2;b.creator_transaction_uuid.value=Id(20);
  Check(NoHeap([&]{return c::CatalogMetadataPreservesFamilyOrigin(a,b);}),"mutable generations preserve immutable series");
  for(unsigned field:{0u,2u,3u,5u,7u,18u}){
    auto changed=f;changed[field].value=Identity(77);auto other=b;other.record.payload=View(Wire(changed,65546));
    if(field==0)other.record.header.object_uuid.value=Id(77);
    Check(!NoHeap([&]{return c::CatalogMetadataPreservesFamilyOrigin(a,other);}),"changed immutable series binding refused");
  }
  f[17].value=Pack({Bytes{'x'}});b.record.payload=View(Wire(f,65546));
  Check(!NoHeap([&]{return c::CatalogMetadataPreservesFamilyOrigin(a,b);}),"changed label values refused");
}
void BoundsAndErrors(){
  auto f=Series({{std::string(4096,'K'),1,Bytes(65528,'V')}});
  for(unsigned i:{1u,6u,8u,10u,12u,19u})f[i].value=Integer(~p::u64{0});
  auto b=Wire(f,65546);Check(Decode(b).ok(),"maximum individual key and packed value counters");
  const auto own=c::DecodeCatalogMetricSeries(View(b));std::fill(b.begin(),b.end(),0);
  Check(own.ok()&&own.record->labels[0].key==std::string(4096,'K')&&
      std::get<std::string>(own.record->labels[0].value)==std::string(65528,'V'),"owning result source independence");
  std::vector<Label> labels;
  for(unsigned i=0;i<1024;++i){auto k=std::to_string(i);k=std::string(4-k.size(),'0')+k;k.resize(i==1023?56:60,'K');labels.push_back({k,1,{'v'}});}
  b=Wire(Series(labels),65546);labels.back().value.resize(1+131072-b.size(),'V');b=Wire(Series(labels),65546);
  Check(b.size()==131072&&Decode(b).ok(),"exact maximum whole block");
  labels.back().value.push_back('V');Refuse(Wire(Series(labels),65546),E::size_limit);
  for(const auto& key:std::vector<std::string>{"",std::string(4097,'K'),std::string("k\0x",3),"\xc0\x80","\xed\xa0\x80"})
    Refuse(Wire(Series({{key,1,{'v'}}}),65546));
  for(const auto& value:std::vector<Bytes>{{},{0xff},{0xf4,0x90,0x80,0x80},Bytes(65529,'V')})
    Refuse(Wire(Series({{"key",1,value}}),65546),value.size()>65528?E::size_limit:E::invalid_value);
  for(const auto& labels:std::vector<std::vector<Label>>{{{"a",1,{'v'}},{"a",1,{'w'}}},{{"b",1,{'v'}},{"a",1,{'w'}}}})
    Refuse(Wire(Series(labels),65546));
  Check(Decode(Wire(Series({{"z",1,{'v'}},{"\xc2\x80",1,{'w'}}}),65546)).ok(),"unsigned canonical UTF8 ordering");
  const auto good=Wire(Series(),65546);
  for(std::size_t n=0;n<good.size();++n){const auto v=Decode(Bytes(good.begin(),good.begin()+n));Check(!v.ok()&&!v.record,"every truncation no partial output");}
  for(unsigned i=0;i<20;++i){auto bad=Series();bad.erase(bad.begin()+i);Refuse(Wire(bad,65546),E::missing_field);}
  for(unsigned i:{1u,6u,10u,12u,19u}){auto bad=Series();bad[i].value=Integer(0);Refuse(Wire(bad,65546));}
  for(unsigned type=0;type<256;++type)if(type<1||type>3)Refuse(Wire(Series({{"key",type,Identity(1)}}),65546));
  for(unsigned size:{0u,1u,3u,4u,7u,8u,9u}){
    auto bad=Series();bad[17].value.resize(size);if(size!=9)Refuse(Wire(bad,65546));
  }
  f=Series();Num(f[17].value,0,4,2);Refuse(Wire(f,65546));
  f=Series();Num(f[17].value,4,4,0xffffffff);Refuse(Wire(f,65546));
  f=Series();f[17].value.push_back(0);Refuse(Wire(f,65546));
  f=Series();f[7].value=Bytes(16);f[8].value=Integer(0);Refuse(Wire(f,65546));
}
void SharedText(){
  for(unsigned a=0;a<256;++a)for(unsigned b=0;b<256;++b){
    const std::array<char,2> text{static_cast<char>(a),static_cast<char>(b)};
    const bool valid=(a<128&&b<128)||(a>=0xc2&&a<=0xdf&&b>=0x80&&b<=0xbf);
    Check(NoHeap([&]{return m::MetricTextValid({text.data(),2});})==valid,"independent exhaustive two-byte UTF8 predicate");
    const m::MetricScalar owned(std::string(text.data(),2));
    Check(m::MetricScalarValid(owned)==valid,"owning text scalar shares predicate");
  }
}
}
int main(){
  try{Matrices();OwnersAndOrigin();BoundsAndErrors();SharedText();Check(attempted_allocations==0,"borrowed series allocated");
    std::cout<<"native series memory: "<<checks<<" checks, "<<attempted_allocations<<" allocation attempts\n";return 0;
  }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}catch(...){return 1;}
}
