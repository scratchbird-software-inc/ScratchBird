// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
// Share the independent SBCV oracle and allocation-denial instrumentation,
// not the production encoders. This executable exercises family first use.
#define main NativeCatalogValueRegressionMain
#include "native_catalog_value_memory_gate.cpp"
#undef main
#include "catalog_schema_definition.hpp"
#include "catalog_storage_action_policy.hpp"
#include "catalog_metric_visibility_policy.hpp"

namespace {
std::string_view View(const Bytes& b) {
  return {reinterpret_cast<const char*>(b.data()), b.size()};
}
std::vector<WireField> SchemaFields(unsigned mask, unsigned type, p::u64 local) {
  std::vector<WireField> f{{1,T::engine_identity,Identity(1)}, {2,T::engine_identity,Identity(2)}};
  if(mask&1)f.push_back({3,T::engine_identity,Identity(3)});
  f.push_back({4,T::unsigned_integer,Integer(type)});
  for(unsigned i=0;i<4;++i)if(mask&(2u<<i))f.push_back({static_cast<p::u16>(5+i),T::engine_identity,Identity(4+i)});
  f.push_back({9,T::engine_identity,Identity(8)});f.push_back({10,T::unsigned_integer,Integer(local)});
  return f;
}
// Independent Core profile registry octets, not production profile lookup.
Bytes Profile(unsigned i) {
  constexpr std::array<std::array<p::byte,3>,5> tail{{{0,0x81,0x92},{1,0x63,0x84},
      {3,0x27,0x68},{6,0x55,0x36},{0x13,0x10,0x72}}};
  Bytes b(16);b[6]=0x70;b[8]=0x80;std::copy(tail[i].begin(),tail[i].end(),b.begin()+13);return b;
}
std::vector<WireField> PolicyFields(unsigned profile=0,unsigned flags=7,unsigned approval=0,unsigned pressure=1) {
  std::vector<WireField> f{{1,T::engine_identity,Identity(1)},{2,T::unsigned_integer,Integer(1)},
      {3,T::engine_identity,Identity(2)},{4,T::engine_identity,Identity(3)},
      {5,T::engine_identity,Identity(4)},{6,T::engine_identity,Profile(profile)},
      {7,T::engine_identity,Identity(8)},{8,T::unsigned_integer,Integer(1)}};
  for(unsigned i=0;i<3;++i)f.push_back({static_cast<p::u16>(9+i),T::boolean,{static_cast<p::byte>((flags>>i)&1)}});
  const std::array<p::u64,11> numbers{approval,0,8,8,100,8,8ull*(8192u<<profile),768,0,1,pressure};
  for(unsigned i=0;i<numbers.size();++i)f.push_back({static_cast<p::u16>(12+i),T::unsigned_integer,Integer(numbers[i])});
  return f;
}
std::vector<WireField> AttachmentFields(unsigned profile=0) {
  return {{1,T::engine_identity,Identity(1)},{2,T::unsigned_integer,Integer(1)},
      {3,T::engine_identity,Identity(2)},{4,T::engine_identity,Identity(3)},
      {5,T::engine_identity,Identity(4)},{6,T::engine_identity,Profile(profile)},
      {7,T::engine_identity,Identity(5)},{8,T::engine_identity,Identity(8)},
      {9,T::unsigned_integer,Integer(1)}};
}
std::vector<WireField> VisibilityFields(std::string_view read="READ",std::string_view sensitive="") {
  return {{1,T::engine_identity,Identity(1)},{2,T::unsigned_integer,Integer(1)},
      {3,T::engine_identity,Identity(2)},{4,T::engine_identity,Identity(3)},
      {5,T::utf8_text,Bytes(read.begin(),read.end())},{6,T::utf8_text,Bytes(sensitive.begin(),sensitive.end())},
      {7,T::engine_identity,Identity(8)},{8,T::unsigned_integer,Integer(1)}};
}
c::CatalogMetadataVersion Metadata(const Bytes& b,c::CatalogRecordKind kind,std::string_view subtype) {
  c::CatalogMetadataVersion m;m.record.header.kind=kind;m.record.header.object_uuid={p::UuidKind::object,Id(1)};
  m.record.header.parent_uuid={p::UuidKind::object,Id(20)};m.owning_schema_uuid={p::UuidKind::schema,Id(20)};
  m.record.payload=View(b);m.object_subtype=subtype;m.authority_scope=c::CatalogAuthorityScope::local;
  m.default_name_uuid={p::UuidKind::object,Id(21)};m.name_vector_uuid={p::UuidKind::object,Id(22)};
  m.creator_transaction_uuid={p::UuidKind::transaction,Id(8)};m.creator_local_transaction_id=1;
  m.definition_version=1;m.status=c::CatalogObjectStatus::active;return m;
}
template<typename Decode> void Malformed(std::vector<WireField> fields,p::u32 schema,Decode decode) {
  const auto wire=Wire(fields,schema);
  auto reject=[&](const Bytes& b){const auto r=NoHeap([&]{return decode(View(b));});Check(!r.ok(),"malformed family accepted");
    if constexpr(requires{r.definition;})Check(!r.definition,"failed schema exposed partial result");
    else Check(!r.record,"failed family exposed partial result");};
  for(std::size_t n=0;n<wire.size();++n)reject(Bytes(wire.begin(),wire.begin()+n));
  auto extra=wire;extra.push_back(0);reject(extra);
  for(auto at:{0u,4u,6u,8u,12u,16u,20u,22u,24u,26u,27u,28u}){auto b=wire;b[at]^=1;reject(b);}
  for(std::size_t i=0;i<fields.size();++i){
    auto f=fields;f[i].type=static_cast<T>(255);reject(Wire(f,schema));
    if(fields[i].type==T::engine_identity){
      for(unsigned version=0;version<16;++version)if(version!=7){f=fields;f[i].value[6]=version<<4;reject(Wire(f,schema));}
      for(unsigned variant:{0u,1u,3u}){f=fields;f[i].value[8]=variant<<6;reject(Wire(f,schema));}
      f=fields;f[i].value.assign(16,0);reject(Wire(f,schema));
    }
  }
}
void Schemas() {
  const std::array names{"system","user_home","remote_native","remote_emulated","public_compat","application","cluster"};
  for(unsigned mask=0;mask<32;++mask)for(unsigned type=1;type<=7;++type)for(auto local:{p::u64{1},~p::u64{0}}){
    auto bytes=Wire(SchemaFields(mask,type,local),65540);
    const auto r=NoHeap([&]{return c::DecodeCatalogSchemaDefinition(View(bytes));});
    Check(r.ok(),"schema first-use/all masks/type/counter without heap");
    const auto& d=*r.definition;
    Check(d.schema_object_uuid.value==Id(1)&&d.database_catalog_object_uuid.value==Id(2)&&
        d.origin_transaction_uuid.value==Id(8)&&d.origin_local_transaction_id==local,"schema native values");
    const auto encoded=c::EncodeCatalogSchemaDefinition(d);Check(encoded.ok()&&encoded.bytes==bytes,"schema exact independent bytes");
    auto m=Metadata(bytes,c::CatalogRecordKind::schema,names[type-1]);m.creator_local_transaction_id=local;
    m.record.header.parent_uuid={p::UuidKind::object,Id(mask&1?3:2)};
    m.owning_schema_uuid=mask&1?p::TypedUuid{p::UuidKind::schema,Id(3)}:p::TypedUuid{};
    if(mask&16)m.security_policy_uuid={p::UuidKind::object,Id(7)};
    if(type==7)m.authority_scope=c::CatalogAuthorityScope::cluster;
    Check(NoHeap([&]{return c::CatalogSchemaDefinitionMatchesMetadata(m)&&c::CatalogSchemaDefinitionPreservesOrigin(m,m);}),"schema metadata/origin no heap");
    auto bad=m;bad.record.header.object_uuid.value=Id(99);
    Check(!NoHeap([&]{return c::CatalogSchemaDefinitionMatchesMetadata(bad)||c::CatalogSchemaDefinitionPreservesOrigin(m,bad);}),"schema wrong header refused");
    bytes.clear();Check(d.schema_object_uuid.value==Id(1)&&d.origin_local_transaction_id==local,"fixed schema survives source release");
  }
  Malformed(SchemaFields(31,6,1),65540,c::DecodeCatalogSchemaDefinition);
}
void Policies() {
  for(unsigned profile=0;profile<5;++profile)for(unsigned flags=0;flags<8;++flags)
    for(unsigned approval=0;approval<4;++approval)for(unsigned pressure=1;pressure<=4;++pressure){
    const auto bytes=Wire(PolicyFields(profile,flags,approval,pressure),65548);
    const auto r=NoHeap([&]{return c::DecodeCatalogStorageActionPolicy(View(bytes));});Check(r.ok(),"policy dimensions without heap");
    const auto encoded=c::EncodeCatalogStorageActionPolicy(*r.record);Check(encoded.ok()&&encoded.bytes==bytes,"policy every field exact bytes");
    auto m=Metadata(bytes,c::CatalogRecordKind::policy,"storage_action");
    Check(NoHeap([&]{return c::CatalogStorageActionPolicyMatchesMetadata(m)&&c::CatalogStorageActionPolicyPreservesOrigin(m,m);}),"policy metadata origin no heap");
    m.creator_transaction_uuid.value=Id(99);Check(!NoHeap([&]{return c::CatalogStorageActionPolicyMatchesMetadata(m);}),"policy version1 origin mismatch");
  }
  // Each independent limit failure, including disabled policy validation.
  const std::array<std::pair<unsigned,p::u64>,19> bad{{{2,0},{8,0},{12,4},{13,9},{14,0x80000000ull},
      {15,0},{15,9},{16,0},{16,~p::u64{0}},{17,7},{17,101},{18,65535},{19,767},
      {20,86400000001ull},{21,0},{21,86400000001ull},{22,0},{22,5},{15,0x80000000ull}}};
  for(unsigned profile=0;profile<5;++profile)for(const auto& [id,value]:bad){auto f=PolicyFields(profile,0);f[id-1].value=Integer(value);
    auto b=Wire(f,65548);const auto r=NoHeap([&]{return c::DecodeCatalogStorageActionPolicy(View(b));});Check(r.error==E::invalid_value&&!r.record,"policy exact invalid-value refusal");}
  for(unsigned profile=0;profile<5;++profile){
    auto f=PolicyFields(profile);f[1].value=Integer(~p::u64{0});f[7].value=Integer(~p::u64{0});
    f[15].value=Integer((~p::u64{0})/(8192u<<profile));f[19].value=Integer(86400000000ull);f[20].value=Integer(86400000000ull);
    const auto b=Wire(f,65548);const auto r=NoHeap([&]{return c::DecodeCatalogStorageActionPolicy(View(b));});Check(r.ok(),"policy inclusive maximum bounds");
    auto m=Metadata(b,c::CatalogRecordKind::policy,"storage_action");m.definition_version=~p::u64{0};
    Check(NoHeap([&]{return c::CatalogStorageActionPolicyMatchesMetadata(m);}),"policy origin local number is not commit order");
  }
  for(unsigned field:{6u,9u,10u,11u}){auto f=PolicyFields();
    f[field-1].value=field==6?Identity(99):Bytes{2};const auto b=Wire(f,65548);
    const auto r=NoHeap([&]{return c::DecodeCatalogStorageActionPolicy(View(b));});
    Check(r.error==E::invalid_value&&!r.record,"policy unknown profile/noncanonical boolean");}
  Malformed(PolicyFields(),65548,c::DecodeCatalogStorageActionPolicy);
}
void Attachments() {
  for(unsigned profile=0;profile<5;++profile){auto fields=AttachmentFields(profile);const auto b=Wire(fields,65549);
    const auto r=NoHeap([&]{return c::DecodeCatalogStorageActionAttachment(View(b));});Check(r.ok(),"attachment profile no heap");
    Check(c::EncodeCatalogStorageActionAttachment(*r.record).bytes==b,"attachment exact bytes");
    const auto m=Metadata(b,c::CatalogRecordKind::config_profile,"storage_action_attachment");
    Check(NoHeap([&]{return c::CatalogStorageActionAttachmentMatchesMetadata(m)&&c::CatalogStorageActionAttachmentPreservesOrigin(m,m);}),"attachment metadata origin no heap");
    fields[1].value=Integer(2);fields[6].value=Identity(99);
    auto next=m;next.record.payload=View(Wire(fields,65549));next.definition_version=2;
    Check(NoHeap([&]{return c::CatalogStorageActionAttachmentPreservesOrigin(m,next);}),"attachment can replace selected policy");
    for(unsigned index:{0u,2u,3u,4u,5u,7u,8u}){auto changed=fields;
      changed[index].value=index==8?Integer(2):index==5?Profile((profile+1)%5):Identity(100);
      next.record.payload=View(Wire(changed,65549));
      Check(!NoHeap([&]{return c::CatalogStorageActionAttachmentPreservesOrigin(m,next);}),"attachment immutable reference replacement refused");}
  }
  Malformed(AttachmentFields(),65549,c::DecodeCatalogStorageActionAttachment);
}
void Visibility() {
  for(unsigned n:{1u,16u,128u})for(unsigned s:{0u,1u,128u}){
    const std::string read(n,'R'),sensitive(s,'S');auto b=Wire(VisibilityFields(read,sensitive),65559);
    const auto r=NoHeap([&]{return c::DecodeCatalogMetricVisibilityPolicyView(View(b));});Check(r.ok(),"visibility first-use/bounds no heap");
    Check(r.record->read_right==read&&r.record->sensitive_read_right==sensitive,"visibility rights exact views");
    Check(r.record->read_right.data()==reinterpret_cast<const char*>(b.data()+120),"read right aliases exact input bytes");
    const auto owned=c::DecodeCatalogMetricVisibilityPolicy(View(b));Check(owned.ok(),"visibility owning parity");
    Check(c::EncodeCatalogMetricVisibilityPolicy(*owned.record).bytes==b,"visibility exact independent bytes");
    auto m=Metadata(b,c::CatalogRecordKind::policy,"metric_visibility");
    Check(NoHeap([&]{return c::CatalogMetricVisibilityPolicyMatchesMetadata(m)&&c::CatalogMetricVisibilityPolicyPreservesOrigin(m,m);}),"visibility metadata origin no heap");
    auto fields=VisibilityFields(read,sensitive);fields[1].value=Integer(2);
    auto next=m;next.record.payload=View(Wire(fields,65559));next.definition_version=2;
    next.creator_transaction_uuid.value=Id(99);next.creator_local_transaction_id=2;
    Check(NoHeap([&]{return c::CatalogMetricVisibilityPolicyPreservesOrigin(m,next);}),"visibility independent successor creator");
    for(unsigned index:{0u,2u,3u,6u,7u}){auto changed=fields;changed[index].value=index==7?Integer(2):Identity(100);
      next.record.payload=View(Wire(changed,65559));
      Check(!NoHeap([&]{return c::CatalogMetricVisibilityPolicyPreservesOrigin(m,next);}),"visibility changed identity/origin refused");}
    m.creator_local_transaction_id=0;Check(!NoHeap([&]{return c::CatalogMetricVisibilityPolicyMatchesMetadata(m);}),"visibility original local bound");
    std::fill(b.begin(),b.end(),0);Check(owned.record->read_right==read&&owned.record->sensitive_read_right==sensitive,"owning rights survive source mutation");
  }
  for(unsigned ch=0;ch<256;++ch)for(unsigned position:{0u,1u}){
    std::string right="AA";right[position]=static_cast<char>(ch);auto b=Wire(VisibilityFields(right),65559);
    const bool valid=(ch>='A'&&ch<='Z')||(position==1&&((ch>='0'&&ch<='9')||ch=='_'));
    const auto r=NoHeap([&]{return c::DecodeCatalogMetricVisibilityPolicyView(View(b));});Check(r.ok()==valid,"visibility exhaustive right octets");
    const auto owned=c::DecodeCatalogMetricVisibilityPolicy(View(b));Check(owned.error==r.error,"visibility refusal parity");
    if(!valid)Check(!r.record&&!owned.record,"visibility failure has no partial result");
  }
  for(unsigned which=0;which<2;++which)for(unsigned n:{0u,129u}){
    auto f=VisibilityFields();f[4+which].value.assign(n,'A');auto b=Wire(f,65559);
    const auto r=NoHeap([&]{return c::DecodeCatalogMetricVisibilityPolicyView(View(b));});
    Check(r.ok()==(which==1&&n==0),"visibility required/optional length bounds");
  }
  Malformed(VisibilityFields(),65559,c::DecodeCatalogMetricVisibilityPolicyView);
}
}
int main() {
  try {Schemas();Policies();Attachments();Visibility();Check(attempted_allocations==0,"borrowed family attempted heap allocation");
    std::cout<<"native fixed family checks="<<checks<<" attempted_allocations="<<attempted_allocations<<'\n';return 0;
  }catch(...){deny_heap=false;std::cerr<<"native fixed family failed; attempted_allocations="<<attempted_allocations<<'\n';return 1;}
}
