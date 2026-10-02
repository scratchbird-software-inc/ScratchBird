// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#define main NativeCatalogValueRegressionMain
#include "native_catalog_value_memory_gate.cpp"
#undef main
#include "catalog_security_record_codec.hpp"
#include "catalog_storage_record_codec.hpp"

namespace {
constexpr std::array<std::string_view,10> ids{"principal_uuid","group_uuid","role_uuid","grant_uuid",
    "member_uuid","parent_uuid","subject_uuid","target_uuid","policy_pack_uuid","membership_uuid"};
constexpr std::array<std::string_view,37> attributes{"creator_tx","created_txn","policy_generation",
    "loaded_at_database_create","identity_authority","engine_owned","security_generation",
    "security_context_authority_version","security_context_generation","active","immutable","create_time_only",
    "role_code","role_name","description","default_assignment","group_code","principal_name",
    "credential_fingerprint","kind","principal_kind","bootstrap_principal","grant_class","member_kind",
    "parent_kind","subject_kind","effect","right","grant_name","target","ambient","authority_class",
    "group_name","ambient_rights","connect_only","operational_role","created_disabled"};
std::string_view View(const Bytes& b){return {reinterpret_cast<const char*>(b.data()),b.size()};}
auto Kind(unsigned n){return static_cast<c::CatalogRecordKind>(n);}
auto Security(unsigned kind,const Bytes& b){return NoHeap([&]{return c::DecodeCatalogSecurityRecordView(Kind(kind),View(b));});}
auto Storage(const Bytes& b){return NoHeap([&]{return c::DecodeCatalogStorageRecordView(View(b));});}
std::vector<WireField> SecurityFields(unsigned kind,unsigned mask=0){
  const unsigned primary=kind-59;std::vector<WireField> f;unsigned bit=0;
  for(unsigned id=1;id<=10;++id){
    const bool admitted=id==primary||id==9||(kind==63&&id!=2);
    if(!admitted)continue;
    if(id==primary||(mask&(1u<<bit)))f.push_back({static_cast<p::u16>(id),T::engine_identity,Identity(id)});
    if(id!=primary)++bit;
  }
  f.push_back({32,T::utf8_text,{'1'}});return f;
}
void SecurityRefused(unsigned kind,const Bytes& b,E error){
  const auto v=Security(kind,b);Check(v.error==error&&!v.record,"security exact borrowed refusal");
  const auto own=c::DecodeCatalogSecurityRecord(Kind(kind),View(b));Check(own.error==error&&!own.record,"security owning refusal parity");
}
c::CatalogTypedRecord Header(const Bytes& b,unsigned kind,unsigned owner){
  c::CatalogTypedRecord r;r.header.kind=Kind(kind);r.header.object_uuid={p::UuidKind::object,Id(owner)};
  r.header.row_uuid={p::UuidKind::row,Id(40)};r.payload=View(b);return r;
}
void SecurityMatrices(){
  // First use of every kind must precede all owning schema initialization.
  for(unsigned kind=60;kind<=63;++kind){const auto b=Wire(SecurityFields(kind),65536+kind);
    Check(Security(kind,b).ok(),"security first-use static schema without heap");}
  for(unsigned kind=60;kind<=63;++kind)for(unsigned mask=0;mask<(kind==63?256u:2u);++mask){
    const auto fields=SecurityFields(kind,mask);const auto b=Wire(fields,65536+kind);const auto v=Security(kind,b);
    Check(v.ok()&&v.record->kind==Kind(kind),"security all optional identity masks");
    for(unsigned index=0;index<10;++index){const bool present=std::any_of(fields.begin(),fields.end(),[&](const auto& f){return f.id==index+1;});
      Check(v.record->identities[index].has_value()==present,"native identity presence");
      const auto identity=NoHeap([&]{return v.record->Identity(ids[index]);});
      Check(identity==(present?Id(index+1):p::Uuid{}),"native identity exact bytes and canonical field names");}
    const auto own=c::DecodeCatalogSecurityRecord(Kind(kind),View(b));Check(own.ok(),"security owning matrix parity");
    Check(c::EncodeCatalogSecurityRecord(*own.record).bytes==b,"security independent canonical bytes");
    auto header=Header(b,kind,kind-59);Check(NoHeap([&]{return c::CatalogSecurityPayloadMatchesHeader(header);}),"security exact header binding no heap");
    header.header.row_uuid.value=header.header.object_uuid.value;
    Check(!NoHeap([&]{return c::CatalogSecurityPayloadMatchesHeader(header);}),"security row is not owner");
  }
  for(unsigned kind=60;kind<=63;++kind)for(unsigned index=0;index<attributes.size();++index)for(bool empty:{false,true}){
    auto fields=SecurityFields(kind);const Bytes text=empty?Bytes{}:Bytes{'A',0,10,0xc3,0xa9};
    if(index==0)fields.back().value=text;
    else fields.push_back({static_cast<p::u16>(32+index),T::utf8_text,text});
    const auto b=Wire(fields,65536+kind);const auto v=Security(kind,b);Check(v.ok(),"security optional attributes and empty values");
    for(unsigned j=0;j<attributes.size();++j){const auto a=NoHeap([&]{return v.record->Attribute(attributes[j]);});
      Check(a.has_value()==(j==0||j==index),"attribute absence differs from empty presence");
      if(j==index)Check(*a==View(text),"attribute exact UTF8/NUL/newline bytes");}
    Check(!NoHeap([&]{return v.record->Attribute("not_a_registered_attribute");}),"unknown attribute absent");
    Check(NoHeap([&]{return v.record->Identity("not_a_registered_identity");}).is_nil(),"unknown identity absent");
    const auto own=c::DecodeCatalogSecurityRecord(Kind(kind),View(b));Check(own.ok()&&c::EncodeCatalogSecurityRecord(*own.record).bytes==b,"all attribute names exact mapping");
  }
  for(unsigned kind=60;kind<=63;++kind)for(unsigned pos=0;pos<16;++pos)for(unsigned octet=0;octet<256;++octet){
    auto fields=SecurityFields(kind);fields[0].value[pos]=octet;const auto b=Wire(fields,65536+kind);
    const bool valid=(pos!=6||(octet>>4)==7)&&(pos!=8||(octet>>6)==2);const auto v=Security(kind,b);
    Check(v.ok()==valid,"security every UUID octet and value");
    if(valid){const auto actual=v.record->Identity(ids[kind-60]);Check(std::equal(actual.bytes.begin(),actual.bytes.end(),fields[0].value.begin()),"security UUID lossless native result");}
    else Check(!v.record,"invalid UUID no partial security record");
  }
}
void SecurityBounds(){
  for(unsigned kind=60;kind<=63;++kind){
    auto f=SecurityFields(kind);f.push_back({46,T::utf8_text,Bytes(130000,'D')});f.push_back({47,T::utf8_text,Bytes(903,'A')});
    auto b=Wire(f,65536+kind);Check(b.size()==130976,"independent maximum security frame");
    const auto v=Security(kind,b);Check(v.ok()&&v.record->attributes[14]->size()==130000,"maximum security payload without heap");
    Check(v.record->Attribute("creator_tx")->data()==reinterpret_cast<const char*>(b.data()+56),"security text aliases source");
    const auto own=c::DecodeCatalogSecurityRecord(Kind(kind),View(b));Check(own.ok(),"maximum owning security payload");
    std::fill(b.begin(),b.end(),0);Check(own.record->attributes.at("description")==std::string(130000,'D'),"owning security attributes survive source mutation");
    f.back().value.push_back('A');SecurityRefused(kind,Wire(f,65536+kind),E::invalid_framing);
    f=SecurityFields(kind);f.push_back({46,T::utf8_text,Bytes(130001,'D')});SecurityRefused(kind,Wire(f,65536+kind),E::size_limit);
    f=SecurityFields(kind);f.pop_back();SecurityRefused(kind,Wire(f,65536+kind),E::missing_field);
    f=SecurityFields(kind);f.erase(f.begin());SecurityRefused(kind,Wire(f,65536+kind),E::missing_field);
    f=SecurityFields(kind);f.back().value={0xc0,0x80};SecurityRefused(kind,Wire(f,65536+kind),E::invalid_value);
    const auto good=Wire(SecurityFields(kind),65536+kind);
    for(std::size_t n=0;n<good.size();++n){const auto r=Security(kind,Bytes(good.begin(),good.begin()+n));Check(!r.ok()&&!r.record,"security every truncation");}
    for(unsigned at:{0u,4u,6u,8u,12u,16u,20u,22u,24u,26u,27u,28u}){auto bad=good;bad[at]^=1;
      const auto r=Security(kind,bad);Check(!r.ok()&&!r.record,"security malformed header or TLV");}
    for(unsigned other=60;other<=63;++other)if(kind!=other)SecurityRefused(other,good,E::invalid_schema);
    for(unsigned field=1;field<=10;++field){if(field==kind-59||field==9||(kind==63&&field!=2))continue;
      f=SecurityFields(kind);f.push_back({static_cast<p::u16>(field),T::engine_identity,Identity(field)});
      std::sort(f.begin(),f.end(),[](const auto&a,const auto&b){return a.id<b.id;});SecurityRefused(kind,Wire(f,65536+kind),E::unknown_field);}
  }
  SecurityRefused(0,{},E::invalid_schema);
}
std::vector<WireField> StorageFields(p::u64 page=8192,p::u64 creator=1,Bytes name={'S'}){
  return {{1,T::engine_identity,Identity(1)},{2,T::engine_identity,Identity(2)},
      {3,T::unsigned_integer,Integer(page)},{4,T::unsigned_integer,Integer(creator)},{5,T::utf8_text,std::move(name)}};
}
void StorageMatrices(){
  for(unsigned page:{8192u,16384u,32768u,65536u,131072u})for(auto creator:{p::u64{1},~p::u64{0}})
    for(unsigned size:{1u,16u,4096u}){
    auto b=Wire(StorageFields(page,creator,Bytes(size,'S')),65616);const auto v=Storage(b);
    Check(v.ok()&&v.record->descriptor_uuid.value==Id(1)&&v.record->filespace_uuid.value==Id(2)&&
        v.record->page_size==page&&v.record->creator_transaction_number==creator,"storage all profiles/counters/names no heap");
    Check(v.record->descriptor_uuid.kind==p::UuidKind::object&&v.record->filespace_uuid.kind==p::UuidKind::filespace,"storage typed native identities");
    Check(v.record->descriptor_name.data()==reinterpret_cast<const char*>(b.data()+112),"storage borrowed exact name");
    auto header=Header(b,80,1);Check(NoHeap([&]{return c::CatalogStoragePayloadMatchesHeader(header);}),"storage header binding no heap");
    for(unsigned fault=0;fault<3;++fault){auto bad=header;
      if(fault==0)bad.header.object_uuid.value=Id(99);
      if(fault==1)bad.header.row_uuid.value=Id(1);
      if(fault==2)bad.header.kind=Kind(60);
      Check(!NoHeap([&]{return c::CatalogStoragePayloadMatchesHeader(bad);}),"storage mismatched header refused");}
    const auto own=c::DecodeCatalogStorageRecord(View(b));Check(own.ok()&&c::EncodeCatalogStorageRecord(*own.record).bytes==b,"storage exact owning byte parity");
    b.clear();Check(own.record->descriptor_name==std::string(size,'S')&&v.record->filespace_uuid.value==Id(2),"native identities and owning text survive source release");
  }
  for(auto page:{p::u64{0},p::u64{4096},p::u64{8191},p::u64{8193},p::u64{262144},~p::u64{0}}){
    const auto r=Storage(Wire(StorageFields(page),65616));Check(r.error==E::invalid_value&&!r.record,"storage unsupported profile refused");}
  auto r=Storage(Wire(StorageFields(8192,0),65616));Check(r.error==E::invalid_value&&!r.record,"storage zero creator refused");
  for(unsigned size:{0u,4097u}){r=Storage(Wire(StorageFields(8192,1,Bytes(size,'S')),65616));Check(r.error==E::invalid_framing&&!r.record,"storage outer frame precedence");}
  const auto good=Wire(StorageFields(),65616);
  for(std::size_t n=0;n<good.size();++n){r=Storage(Bytes(good.begin(),good.begin()+n));Check(!r.ok()&&!r.record,"storage every truncation");}
  for(unsigned at:{0u,4u,6u,8u,12u,16u,20u,22u,24u,26u,27u,28u}){auto bad=good;bad[at]^=1;
    r=Storage(bad);Check(!r.ok()&&!r.record,"storage malformed framing");}
}
}
int main(){try{SecurityMatrices();SecurityBounds();StorageMatrices();Check(attempted_allocations==0,"bootstrap borrowed route allocated");
  std::cout<<"native bootstrap family checks="<<checks<<" attempted_allocations="<<attempted_allocations<<'\n';return 0;
}catch(...){deny_heap=false;std::cerr<<"bootstrap family failed; attempted_allocations="<<attempted_allocations<<'\n';return 1;}}
