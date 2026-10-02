// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include <cstdlib>
#include <cstdio>
#include <new>
#include <utility>
namespace record_memory_fault {
thread_local long remaining=-1;
thread_local std::size_t allocations=0;
thread_local bool fired=false;
void* Allocate(std::size_t n){
  ++allocations;if(remaining==0){fired=true;throw std::bad_alloc();}
  if(remaining>0)--remaining;
  if(void* p=std::malloc(n?n:1))return p;throw std::bad_alloc();
}
}
void* operator new(std::size_t n){return record_memory_fault::Allocate(n);}
void* operator new[](std::size_t n){return record_memory_fault::Allocate(n);}
void operator delete(void* p) noexcept{std::free(p);}
void operator delete[](void* p) noexcept{std::free(p);}
void operator delete(void* p,std::size_t) noexcept{std::free(p);}
void operator delete[](void* p,std::size_t) noexcept{std::free(p);}
void* operator new(std::size_t n,std::align_val_t a){
  ++record_memory_fault::allocations;if(record_memory_fault::remaining==0){record_memory_fault::fired=true;throw std::bad_alloc();}
  if(record_memory_fault::remaining>0)--record_memory_fault::remaining;
  void* p=nullptr;if(posix_memalign(&p,static_cast<std::size_t>(a),n?n:1)==0)return p;throw std::bad_alloc();
}
void* operator new[](std::size_t n,std::align_val_t a){return ::operator new(n,a);}
void operator delete(void* p,std::align_val_t) noexcept{std::free(p);}
void operator delete[](void* p,std::align_val_t) noexcept{std::free(p);}
void operator delete(void* p,std::size_t,std::align_val_t) noexcept{std::free(p);}
void operator delete[](void* p,std::size_t,std::align_val_t) noexcept{std::free(p);}
template<class F> auto WithoutRecordHeap(F action){
  record_memory_fault::remaining=0;record_memory_fault::fired=false;
  try{auto result=action();record_memory_fault::remaining=-1;
    if(record_memory_fault::fired){std::fputs("FAIL borrowed record attempted allocation\n",stderr);std::abort();}
    return result;
  }catch(...){record_memory_fault::remaining=-1;throw;}
}

#include "catalog_record_codec.hpp"
#include "catalog_runtime_authority_binding.hpp"
#include "hash_digest_parts.hpp"
#include <openssl/evp.h>
#include <algorithm>
#include <array>
#include <limits>
#include <source_location>
namespace c=scratchbird::core::catalog;
namespace p=scratchbird::core::platform;
namespace hash=scratchbird::core::hash;
using Bytes=std::vector<p::byte>;
using M=c::CatalogMetadataVersion;
using K=p::UuidKind;
unsigned checks=0;
void Check(bool yes,const char* why,std::source_location at=std::source_location::current()){
  ++checks;if(!yes){std::fprintf(stderr,"FAIL %u: %s\n",at.line(),why);std::abort();}
}
p::TypedUuid Id(K kind,unsigned id){
  p::TypedUuid r;r.kind=kind;r.value.bytes[6]=0x70;r.value.bytes[8]=0x80;r.value.bytes[15]=id;return r;
}
constexpr std::array refs{&M::owner_uuid,&M::creator_transaction_uuid,&M::retired_transaction_uuid,
  &M::default_name_uuid,&M::name_vector_uuid,&M::security_policy_uuid,&M::dependency_group_uuid,
  &M::storage_binding_uuid,&M::donor_overlay_uuid,&M::audit_uuid,&M::owning_schema_uuid};
constexpr std::array counters{&M::definition_version,&M::schema_epoch,&M::security_epoch,&M::resource_epoch,
  &M::catalog_generation,&M::dependency_generation,&M::invalidation_generation,&M::creator_local_transaction_id};
void Put(Bytes& b,std::size_t at,p::u64 value,unsigned size){
  for(unsigned i=0;i<size;++i)b.at(at+i)=static_cast<p::byte>(value>>(8*i));
}
hash::Digest256 Digest(const p::byte* b,std::size_t size){
  hash::Digest256 d{};unsigned n=0;
  Check(EVP_Digest(b,size,d.data(),&n,EVP_sha256(),nullptr)==1&&n==32,"independent provider oracle");return d;
}
void Seal(Bytes& b){
  std::fill(b.begin()+320,b.begin()+352,0);const auto d=Digest(b.data(),b.size());
  std::copy(d.begin(),d.end(),b.begin()+320);
}
M Fixture(){
  M m;m.record.header.kind=c::CatalogRecordKind::sql_object;m.record.header.row_uuid=Id(K::row,1);
  m.record.header.object_uuid=Id(K::object,2);m.record.header.parent_uuid=Id(K::schema,3);
  for(auto member:counters)m.*member=1;m.resource_epoch=0;
  m.owner_uuid=Id(K::principal,4);m.creator_transaction_uuid=Id(K::transaction,5);
  m.audit_uuid=Id(K::object,6);m.owning_schema_uuid=Id(K::schema,3);
  m.default_name_uuid=Id(K::object,7);m.name_vector_uuid=Id(K::object,8);
  m.security_policy_uuid=Id(K::object,9);m.dependency_group_uuid=Id(K::object,10);
  m.storage_binding_uuid=Id(K::object,11);m.donor_overlay_uuid=Id(K::object,12);
  m.lifecycle=c::CatalogObjectLifecycle::active;m.status=c::CatalogObjectStatus::active;
  m.trace_search_key=std::string(32,'T');m.object_subtype="application";m.retention_class=std::string(32,'R');
  m.record.payload=std::string("opaque\0\r\n",9)+std::string(40,'x');return m;
}
// Offset oracle from SBCMV001/SBCTREC2. No production serializers or integer helpers.
Bytes Golden(const M& m){
  const auto text=m.trace_search_key.size()+m.object_subtype.size()+m.retention_class.size();
  const auto start=384+text;Bytes b(start+96+m.record.payload.size());
  std::copy_n("SBCMV001",8,b.begin());Put(b,8,1,2);Put(b,10,384,2);Put(b,12,b.size(),4);
  Put(b,16,static_cast<unsigned>(m.authority_scope),2);Put(b,18,static_cast<unsigned>(m.lifecycle),2);
  Put(b,20,static_cast<unsigned>(m.status),2);Put(b,22,static_cast<unsigned>(m.visibility),2);
  for(unsigned i=0;i<8;++i)Put(b,32+8*i,m.*counters[i],8);
  for(unsigned i=0;i<11;++i){const auto& id=m.*refs[i];b[272+i]=static_cast<unsigned>(id.kind);
    std::copy(id.value.bytes.begin(),id.value.bytes.end(),b.begin()+96+16*i);}
  Put(b,352,m.trace_search_key.size(),4);Put(b,356,96+m.record.payload.size(),4);
  Put(b,360,m.object_subtype.size(),4);Put(b,364,m.retention_class.size(),4);
  auto at=384u;for(const auto* s:{&m.trace_search_key,&m.object_subtype,&m.retention_class}){
    std::copy(s->begin(),s->end(),b.begin()+at);at+=s->size();}
  std::copy_n("SBCTREC2",8,b.begin()+start);Put(b,start+8,2,2);Put(b,start+10,96,2);
  Put(b,start+12,96+m.record.payload.size(),4);Put(b,start+16,static_cast<unsigned>(m.record.header.kind),2);
  Put(b,start+20,m.record.header.record_version,4);Put(b,start+24,m.record.header.deleted,4);
  Put(b,start+28,m.record.payload.size(),4);
  unsigned i=0;for(const auto* id:{&m.record.header.row_uuid,&m.record.header.object_uuid,&m.record.header.parent_uuid}){
    b[start+32+i]=static_cast<unsigned>(id->kind);
    std::copy(id->value.bytes.begin(),id->value.bytes.end(),b.begin()+start+40+16*i);++i;}
  std::copy(m.record.payload.begin(),m.record.payload.end(),b.begin()+start+96);
  const auto d=Digest(b.data()+start,b.size()-start);std::copy(d.begin(),d.end(),b.begin()+288);Seal(b);return b;
}
auto Decode(std::span<const p::byte> b){return WithoutRecordHeap([&]{return c::DecodeCatalogMetadataVersionView(b);});}
void Parity(const c::CatalogRecordDiagnosticView& a,const p::DiagnosticRecord& b){
  Check(a.status.code==b.status.code&&a.status.severity==b.status.severity&&a.status.subsystem==b.status.subsystem,"status parity");
  Check(a.diagnostic_code==b.diagnostic_code&&a.message_key==b.message_key&&a.origin==b.source_component,"error identity parity");
  Check(b.arguments.size()==(a.detail.empty()?0u:1u),"argument count");
  if(!a.detail.empty())Check(b.arguments[0].key=="detail"&&b.arguments[0].text()&&*b.arguments[0].text()==a.detail,"argument detail");
}
void Refused(const Bytes& b,std::string_view detail={}){
  const auto v=Decode(b);Check(!v.ok()&&!v.record,"no partial borrowed metadata");
  Check(std::all_of(v.definition_sha256.begin(),v.definition_sha256.end(),[](auto x){return x==0;}),"no partial definition digest");
  if(!detail.empty())Check(v.diagnostic.detail==detail,"exact diagnostic detail");
  const auto o=c::DecodeCatalogMetadataVersion(b);Check(!o.ok()&&o.bytes.empty()&&o.record.record.payload.empty(),"no partial owning metadata");
  Parity(v.diagnostic,o.diagnostic);
}
void Framing(){
  const auto m=Fixture();const auto good=Golden(m);
  // First metadata decode before owning schema/registry initialization.
  const auto view=Decode(good);Check(view.ok(),"independent golden admitted");
  Check(c::EncodeCatalogMetadataVersion(m).bytes==good,"writer exact oracle");
  const auto own=c::DecodeCatalogMetadataVersion(good);Check(own.ok()&&own.bytes==good,"owning byte preservation");
  for(std::size_t size=0;size<good.size();++size)Check(!Decode(std::span(good).first(size)).ok(),"every truncation");
  for(std::size_t at=0;at<good.size();++at){auto b=good;b[at]^=1;Refused(b);}
  auto bad=good;bad.push_back(0);Refused(bad,"header_invalid");
  for(auto [begin,end]:{std::pair{24,32},std::pair{283,288},std::pair{368,384}})
    for(int at=begin;at<end;++at)for(unsigned value=1;value<256;++value){
      bad=good;bad[at]=value;Seal(bad);Refused(bad,"digest_reserved_or_canonical_mismatch");}
  bad=good;bad[288]^=1;Seal(bad);Refused(bad,"digest_reserved_or_canonical_mismatch");
  bad=good;Put(bad,352,4097,4);Refused(bad,"length_invalid");
  bad=good;Put(bad,356,0xffffffffu,4);Refused(bad,"length_invalid");
  bad=Bytes(262145,0);Refused(bad,"size_invalid");
  auto copy=*view.record;
  Check(copy.trace_search_key.data()==reinterpret_cast<const char*>(good.data()+384),"text lifetime input-backed");
  Check(copy.record.payload.data()==reinterpret_cast<const char*>(good.data()+good.size()-m.record.payload.size()),"payload input-backed");
  auto disposable=good;const auto materialized=c::DecodeCatalogMetadataVersion(disposable);std::fill(disposable.begin(),disposable.end(),0);
  Check(materialized.record.record.payload==m.record.payload&&materialized.record.trace_search_key==m.trace_search_key,"owning independent lifetime");
}
void Semantics(){
  const auto base=Fixture();
  for(unsigned index=0;index<8;++index){auto m=base;m.*counters[index]=0;
    const auto b=Golden(m);if(index==3)Check(Decode(b).ok(),"resource zero allowed");else Refused(b,"counter_zero");}
  for(unsigned index=0;index<4;++index)for(unsigned n=0;n<12;++n){
    auto b=Golden(base);Put(b,16+index*2,n,2);Seal(b);
    const unsigned maximum[]={7,10,9,7};const auto v=Decode(b);
    Check(v.ok()==(n>=1&&n<=maximum[index]),"all metadata enum boundaries");
    if(!v.ok())Refused(b,"enum_invalid");
  }
  for(unsigned slot=0;slot<11;++slot)for(unsigned kind=0;kind<256;++kind){
    auto m=base;if(slot==2){m.retired_transaction_uuid=m.creator_transaction_uuid;m.status=c::CatalogObjectStatus::retired;}
    auto& id=m.*refs[slot];id.kind=static_cast<K>(kind);
    // Native registry: database through transaction are codes0..7; principal9.
    // Session8 is not durable; unknown10 and all unregistered codes refuse.
    const bool durable=kind<=7||kind==9;
    const bool valid=durable&&((slot==0||slot==9)||((slot==1||slot==2)?id.kind==K::transaction:
      (slot==10?id.kind==K::schema:id.kind==K::object)));
    const auto b=Golden(m);Check(Decode(b).ok()==valid,"every binary reference kind");
    if(!valid)Refused(b);
  }
  for(unsigned slot=0;slot<11;++slot)for(unsigned version=0;version<16;++version)for(unsigned variant=0;variant<4;++variant){
    if(version==7&&variant==2)continue;auto m=base;
    if(slot==2){m.retired_transaction_uuid=m.creator_transaction_uuid;m.status=c::CatalogObjectStatus::retired;}
    auto& id=m.*refs[slot];id.value.bytes[6]=version<<4;id.value.bytes[8]=variant<<6;Refused(Golden(m),"reference_invalid");
  }
  for(unsigned slot=0;slot<11;++slot){auto m=base;m.*refs[slot]={};const auto v=Decode(Golden(m));
    const bool required=slot==0||slot==1||slot==9;const bool paired=slot==3||slot==4;
    Check(v.ok()==(!required&&!paired),"exact optional absence and name pairing");}
  for(auto member:{&M::trace_search_key,&M::object_subtype,&M::retention_class}){
    const auto cap=member==&M::trace_search_key?4096u:256u;
    auto m=base;m.*member=std::string(cap,'Z');Check(Decode(Golden(m)).ok(),"text exact cap");
    m.*member="";Refused(Golden(m),"trace_key_invalid");
    m.*member=std::string(cap+1,'Z');Refused(Golden(m),"length_invalid");
    for(unsigned octet=0;octet<256;++octet){m.*member=std::string(1,static_cast<char>(octet));
      Check(Decode(Golden(m)).ok()==(octet>=33&&octet<=126),"exact trace/subtype/retention byte alphabet");}
  }
  auto retired=base;retired.record.header.deleted=true;Refused(Golden(retired),"retirement_binding_invalid");
  retired.retired_transaction_uuid=retired.creator_transaction_uuid;retired.status=c::CatalogObjectStatus::retired;
  retired.lifecycle=c::CatalogObjectLifecycle::dropped;Check(Decode(Golden(retired)).ok(),"complete retirement binding");
  for(const auto* subtype:{"metric_series","metric_label_schema","metric_descriptor","storage_action_attachment","storage_action",
      "agent_runtime_authority","metric_visibility","metric_retention"}){
    auto m=base;m.object_subtype=subtype;Refused(Golden(m));
  }
  auto both=Golden(base);both[16]=0;both[24]=1;Seal(both);Refused(both,"enum_invalid");
  both=Golden(base);both[16]=0;both.back()^=1;Seal(both);Refused(both,"enum_invalid");
  auto maximum=base;maximum.record.payload=std::string(131072-96,'x');
  Check(Decode(Golden(maximum)).ok(),"maximum nested record");
  maximum.record.payload.push_back('x');Refused(Golden(maximum),"binary_record_size_invalid");
}
unsigned hash_fault=0,hash_target=0,hash_seen=0,hash_active=0,update_target=1,updates=0,contexts=0;
extern "C" EVP_MD_CTX* __real_EVP_MD_CTX_new();
extern "C" EVP_MD_CTX* __wrap_EVP_MD_CTX_new(){
  hash_active=(++hash_seen==hash_target)?hash_fault:0;updates=0;
  if(hash_active==1){hash_fault=0;return nullptr;}auto* p=__real_EVP_MD_CTX_new();if(p)++contexts;return p;
}
extern "C" void __real_EVP_MD_CTX_free(EVP_MD_CTX*);
extern "C" void __wrap_EVP_MD_CTX_free(EVP_MD_CTX* p){if(p)--contexts;__real_EVP_MD_CTX_free(p);}
extern "C" int __real_EVP_DigestInit_ex(EVP_MD_CTX*,const EVP_MD*,ENGINE*);
extern "C" int __wrap_EVP_DigestInit_ex(EVP_MD_CTX* c,const EVP_MD* m,ENGINE* e){
  if(hash_active==2){hash_fault=0;return 0;}return __real_EVP_DigestInit_ex(c,m,e);}
extern "C" int __real_EVP_DigestUpdate(EVP_MD_CTX*,const void*,size_t);
extern "C" int __wrap_EVP_DigestUpdate(EVP_MD_CTX* c,const void* p,size_t n){
  if(hash_active==3&&++updates==update_target){hash_fault=0;return 0;}return __real_EVP_DigestUpdate(c,p,n);}
extern "C" int __real_EVP_DigestFinal_ex(EVP_MD_CTX*,unsigned char*,unsigned int*);
extern "C" int __wrap_EVP_DigestFinal_ex(EVP_MD_CTX* c,unsigned char* b,unsigned int* n){
  if(hash_active==4){hash_fault=0;return 0;}const auto r=__real_EVP_DigestFinal_ex(c,b,n);
  if(hash_active==5){hash_fault=0;*n=31;}return r;}
void ProviderFaults(){
  const auto b=Golden(Fixture());
  for(unsigned context=1;context<=2;++context)for(unsigned fault=1;fault<=5;++fault)
    for(unsigned update=1;update<=(context==1&&fault==3?3u:1u);++update){
      hash_target=context;hash_fault=fault;hash_seen=0;update_target=update;
      const auto v=Decode(b);Check(!v.ok()&&hash_fault==0&&!contexts,"provider failure observed and context released");
      Check(v.diagnostic.diagnostic_code=="SB-CORE-HASH-SHA256-FAILED"&&v.diagnostic.origin=="core.hash.digest"&&
          v.diagnostic.message_key=="core.hash.sha256_failed"&&v.diagnostic.detail.empty()&&
          v.diagnostic.status.code==p::StatusCode::platform_required_feature_missing&&
          v.diagnostic.status.severity==p::Severity::error&&v.diagnostic.status.subsystem==p::Subsystem::platform,
          "exact provider failure vector");
      Check(std::all_of(v.definition_sha256.begin(),v.definition_sha256.end(),[](auto x){return !x;}),"no partial failure digest");
      hash_fault=fault;hash_seen=0;
      const auto own=c::DecodeCatalogMetadataVersion(b);
      Check(!own.ok()&&hash_fault==0&&own.bytes.empty()&&!contexts,"owning provider failure no partial output");
      Parity(v.diagnostic,own.diagnostic);
      hash_fault=0;hash_target=0;hash_active=0;Check(Decode(b).ok()&&!contexts,"provider recovery");
    }
  hash::HashDigestSegment bad{nullptr,1};
  auto r=WithoutRecordHeap([&]{return hash::ComputeSha256DigestPartsNative(nullptr,1);});
  Check(r.error==hash::Sha256PartsError::segments_missing,"fixed missing segments");
  r=WithoutRecordHeap([&]{return hash::ComputeSha256DigestPartsNative(&bad,1);});
  Check(r.error==hash::Sha256PartsError::segment_extent_invalid,"fixed invalid segment");
  bad={reinterpret_cast<const p::byte*>(1),std::numeric_limits<std::size_t>::max()};
  r=WithoutRecordHeap([&]{return hash::ComputeSha256DigestPartsNative(&bad,1);});
  Check(r.error==hash::Sha256PartsError::segment_extent_invalid,"length overflow before access");
  constexpr std::array<p::byte,8> sample{0,1,2,0,255,13,10,42};
  const auto expected=Digest(sample.data(),sample.size());
  for(unsigned boundary=0;boundary<=sample.size();++boundary){
    const hash::HashDigestSegment parts[]{{sample.data(),boundary},{nullptr,0},
      {sample.data()+boundary,sample.size()-boundary}};
    const auto segmented=WithoutRecordHeap([&]{return hash::ComputeSha256DigestPartsNative(parts,3);});
    Check(segmented.ok()&&segmented.digest==expected&&!contexts,"every segment boundary exact concatenation");
  }
}
void OwningFaults(){
  const auto m=Fixture();const auto b=Golden(m);
  for(bool decode:{false,true}){
    auto action=[&]{return decode?c::DecodeCatalogMetadataVersion(b):c::EncodeCatalogMetadataVersion(m);};
    const auto before=record_memory_fault::allocations;Check(action().ok(),"owning measure");
    const auto count=record_memory_fault::allocations-before;Check(count>0,"nonvacuous owning sweep");
    for(std::size_t point=0;point<count;++point){
      record_memory_fault::remaining=point;record_memory_fault::fired=false;bool success=false;
      try{success=action().ok();}catch(const std::bad_alloc&){}
      record_memory_fault::remaining=-1;Check(record_memory_fault::fired&&!success&&!contexts,"every owning allocation failure");
    }
    record_memory_fault::remaining=count;const auto r=action();record_memory_fault::remaining=-1;
    Check(r.ok()&&r.bytes==b,"exact allocation boundary recovery");
    std::printf("metadata owning decode=%u allocation_sites=%zu\n",decode,count);
  }
}
void RuntimeFamily(){
  auto m=Fixture();m.record.header.kind=c::CatalogRecordKind::config_profile;
  m.record.header.parent_uuid.kind=K::object;m.object_subtype="agent_runtime_authority";
  c::CatalogRuntimeAuthorityBinding b;b.binding_uuid=m.record.header.object_uuid.value;b.database_uuid=Id(K::database,20).value;
  b.service_principal_uuid=m.owner_uuid.value;b.security_authority_uuid=b.database_uuid;b.provider_uuid=Id(K::object,21).value;
  b.policy_uuid=m.security_policy_uuid.value;b.generation=b.security_epoch=b.policy_epoch=b.provider_generation=b.catalog_generation=1;
  b.authority_mode=c::RuntimeAuthorityMode::database_local;b.origin_transaction_uuid=m.creator_transaction_uuid;b.origin_local_transaction_id=1;
  const auto payload=c::EncodeCatalogRuntimeAuthorityBinding(b);Check(payload.ok(),"runtime binding fixture");
  m.record.payload.assign(payload.bytes.begin(),payload.bytes.end());
  const auto bytes=Golden(m);const auto v=Decode(bytes);Check(v.ok(),"runtime full envelope");
  Check(WithoutRecordHeap([&]{return c::CatalogMetadataPreservesFamilyOrigin(*v.record,*v.record);}),"borrowed runtime origin");
  auto changed=m;changed.definition_version=2;changed.creator_local_transaction_id=2;
  changed.creator_transaction_uuid=Id(K::transaction,30);b.generation=2;b.origin_transaction_uuid=changed.creator_transaction_uuid;b.origin_local_transaction_id=2;
  const auto p=c::EncodeCatalogRuntimeAuthorityBinding(b);changed.record.payload.assign(p.bytes.begin(),p.bytes.end());
  const auto successor_bytes=Golden(changed);const auto next=Decode(successor_bytes);Check(next.ok(),"individually valid changed origin");
  Check(!WithoutRecordHeap([&]{return c::CatalogMetadataPreservesFamilyOrigin(*v.record,*next.record);}),"changed family origin refused");
  changed=m;changed.security_epoch=2;Refused(Golden(changed),"runtime_authority_definition_binding_invalid");
}
int main(){
  Framing();Semantics();RuntimeFamily();ProviderFaults();OwningFaults();
  std::printf("metadata memory checks=%u failures=0\n",checks);
}
