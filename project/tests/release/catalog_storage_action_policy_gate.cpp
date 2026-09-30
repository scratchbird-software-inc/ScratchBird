// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "catalog_storage_action_policy.hpp"
#include <cstdlib>
#include <iostream>
#include <new>
#include <source_location>
#include <stdexcept>

namespace {long allocation_budget=-1;bool counting=false;unsigned long allocations=0;}
void* operator new(std::size_t n){if(counting)++allocations;if(allocation_budget==0){allocation_budget=-1;throw std::bad_alloc();}
  if(allocation_budget>0)--allocation_budget;if(auto* p=std::malloc(n?n:1))return p;throw std::bad_alloc();}
void* operator new[](std::size_t n){return ::operator new(n);}
void operator delete(void* p) noexcept{std::free(p);}void operator delete[](void* p) noexcept{std::free(p);}
void operator delete(void* p,std::size_t) noexcept{std::free(p);}void operator delete[](void* p,std::size_t) noexcept{std::free(p);}
namespace {
namespace c=scratchbird::core::catalog;
namespace d=scratchbird::storage::disk;
using namespace scratchbird::core::platform;
using P=c::CatalogStorageActionPolicy;
unsigned checks=0;
void Check(bool condition,const char* why,std::source_location at=std::source_location::current()){
  ++checks;if(!condition)throw std::runtime_error(std::string(why)+" line="+std::to_string(at.line()));
}
TypedUuid Id(UuidKind kind,unsigned tag){return {kind,Uuid{{1,2,3,4,5,6,0x70,8,0x80,10,11,12,13,14,15,static_cast<byte>(tag)}}};}
void Put(std::string& b,std::size_t at,u64 n,unsigned size){for(unsigned j=0;j<size;++j){b.at(at+j)=static_cast<char>(n%256);n/=256;}}
u64 Get(const std::string& b,std::size_t at,unsigned size){u64 n=0;for(unsigned j=0;j<size;++j)n|=u64(static_cast<byte>(b.at(at+j)))<<(8*j);return n;}
std::string Number(u64 n){std::string b(8,0);Put(b,0,n,8);return b;}
std::string Identity(const Uuid& id){return {reinterpret_cast<const char*>(id.bytes.data()),16};}
void Field(std::string& b,u16 id,byte type,const std::string& value){const auto at=b.size();b.resize(at+8,0);
  Put(b,at,id,2);Put(b,at+2,type,1);Put(b,at+4,value.size(),4);b+=value;}
// Independent field ordering/type/width oracle from the owning contract.
std::string Oracle(const P& p){
  std::string b(24,0);b.replace(0,4,"SBCV");Put(b,4,1,2);Put(b,6,24,2);Put(b,12,22,4);Put(b,16,65548,4);Put(b,20,1,2);
  Field(b,1,5,Identity(p.policy_uuid));Field(b,2,1,Number(p.generation));Field(b,3,5,Identity(p.database_uuid));
  Field(b,4,5,Identity(p.filespace_uuid));Field(b,5,5,Identity(p.storage_profile_uuid));Field(b,6,5,Identity(p.page_size_profile_uuid));
  Field(b,7,5,Identity(p.origin_transaction_uuid.value));Field(b,8,1,Number(p.origin_local_transaction_id));
  Field(b,9,2,std::string(1,p.enabled));Field(b,10,2,std::string(1,p.growth_allowed));Field(b,11,2,std::string(1,p.preallocation_allowed));
  u16 field=12;for(const auto value:{static_cast<u64>(p.approval),p.minimum_free_pages,p.target_free_pages,p.growth_increment_pages,
    p.maximum_total_pages,p.maximum_pages_per_action,p.maximum_work_bytes,p.maximum_retained_image_bytes,
    p.cooldown_microseconds,p.maximum_runtime_microseconds,static_cast<u64>(p.refusal_pressure)})Field(b,field++,1,Number(value));
  Put(b,8,b.size(),4);return b;
}
std::size_t Offset(const std::string& b,unsigned field){for(std::size_t at=24;at<b.size();at+=8+Get(b,at+4,4))if(Get(b,at,2)==field)return at;throw std::runtime_error("oracle field absent");}
P Example(unsigned profile){P p;p.policy_uuid=Id(UuidKind::object,1).value;p.generation=1;
  p.database_uuid=Id(UuidKind::database,2).value;p.filespace_uuid=Id(UuidKind::filespace,3).value;
  p.storage_profile_uuid=Id(UuidKind::object,4).value;p.page_size_profile_uuid=d::kCanonicalFilespacePageProfiles[profile].uuid;
  p.origin_transaction_uuid=Id(UuidKind::transaction,5);p.origin_local_transaction_id=1;
  p.maximum_total_pages=1024;p.maximum_pages_per_action=8;p.maximum_work_bytes=8*u64{d::kCanonicalFilespacePageProfiles[profile].page_size_bytes};
  p.maximum_retained_image_bytes=1<<26;p.maximum_runtime_microseconds=5000000;return p;
}
c::CatalogMetadataVersion Metadata(const P& p){c::CatalogMetadataVersion m;
  m.record.header={c::CatalogRecordKind::policy,Id(UuidKind::row,10),{UuidKind::object,p.policy_uuid},Id(UuidKind::object,11),1,false};
  m.record.payload=Oracle(p);m.owning_schema_uuid=Id(UuidKind::schema,11);m.owner_uuid=Id(UuidKind::principal,12);
  m.audit_uuid=Id(UuidKind::object,13);m.default_name_uuid=Id(UuidKind::object,14);m.name_vector_uuid=Id(UuidKind::object,15);
  m.creator_transaction_uuid=p.origin_transaction_uuid;m.creator_local_transaction_id=p.origin_local_transaction_id;
  m.definition_version=p.generation;m.schema_epoch=m.security_epoch=m.catalog_generation=m.dependency_generation=m.invalidation_generation=1;
  m.lifecycle=c::CatalogObjectLifecycle::active;m.status=c::CatalogObjectStatus::active;m.object_subtype="storage_action";
  m.trace_search_key="STORAGE-POLICY-ORACLE";m.retention_class="catalog_history";return m;
}
void Good(const P& p){const auto oracle=Oracle(p);const auto encoded=c::EncodeCatalogStorageActionPolicy(p);
  Check(encoded.ok()&&std::string(encoded.bytes.begin(),encoded.bytes.end())==oracle,"exact independent policy byte oracle");
  const auto decoded=c::DecodeCatalogStorageActionPolicy(oracle);Check(decoded.ok()&&Oracle(*decoded.record)==oracle,"all policy fields round trip");
  const auto m=Metadata(p);Check(c::CatalogStorageActionPolicyMatchesMetadata(m),"policy identity and metadata binding");
  const auto native=c::EncodeCatalogMetadataVersion(m);Check(native.ok(),"actual common metadata encoder");
  const auto read=c::DecodeCatalogMetadataVersion(native.bytes);Check(read.ok()&&read.record.record.payload==oracle,"actual native metadata preserves full typed policy");
}
void Refused(std::string_view b){const auto r=c::DecodeCatalogStorageActionPolicy(b);Check(!r.ok()&&!r.record,"malformed policy returns no partial definition");}
void Invalid(const P& p){const auto r=c::EncodeCatalogStorageActionPolicy(p);Check(!r.ok()&&r.bytes.empty(),"invalid typed policy refused before bytes");Refused(Oracle(p));}
void Malformed(const P& p){const auto original=Oracle(p);
  for(std::size_t n=0;n<original.size();++n)Refused(std::string_view(original).substr(0,n));
  auto b=original;b.push_back(0);Refused(b);Put(b,8,b.size(),4);Refused(b);
  for(unsigned field=1;field<=22;++field){const auto at=Offset(original,field);const auto length=Get(original,at+4,4);
    b=original;b.erase(at,8+length);Put(b,8,b.size(),4);Put(b,12,21,4);Refused(b);
    b=original;Put(b,at,100,2);Refused(b);b=original;Put(b,at+2,4,1);Refused(b);
    b=original;Put(b,at+3,1,1);Refused(b);b=original;Put(b,at+4,length+1,4);Refused(b);
  }
  for(unsigned field:{1u,3u,4u,5u,6u,7u})for(unsigned mode=0;mode<3;++mode){b=original;const auto at=Offset(b,field)+8;
    if(mode==0)std::fill_n(b.begin()+at,16,0);else if(mode==1)b[at+6]=0x40;else b[at+8]=0;Refused(b);}
  for(unsigned field:{9u,10u,11u}){b=original;b[Offset(b,field)+8]=2;Refused(b);}
  for(std::size_t at:{0u,4u,6u,16u,20u,22u}){b=original;b[at]^=0x7f;Refused(b);}
}
void Boundaries(const P& p){auto x=p;
  for(auto member:{&P::generation,&P::origin_local_transaction_id,&P::growth_increment_pages,&P::maximum_total_pages,
    &P::maximum_pages_per_action,&P::maximum_work_bytes,&P::maximum_retained_image_bytes,&P::maximum_runtime_microseconds}){
    x=p;x.*member=0;Invalid(x);}
  x=p;x.minimum_free_pages=x.target_free_pages+1;Invalid(x);
  x=p;x.target_free_pages=0x80000000ULL;Invalid(x);x=p;x.growth_increment_pages=0x80000000ULL;Invalid(x);
  x=p;x.maximum_total_pages=~u64{0};Invalid(x);x=p;x.maximum_pages_per_action=x.maximum_total_pages+1;Invalid(x);
  const auto profile=std::find_if(d::kCanonicalFilespacePageProfiles.begin(),d::kCanonicalFilespacePageProfiles.end(),
    [&](const auto& value){return value.uuid==p.page_size_profile_uuid;});
  x=p;x.maximum_total_pages=~u64{0}/profile->page_size_bytes;Good(x);
  ++x.maximum_total_pages;Invalid(x);
  x=p;x.maximum_total_pages=~u64{0}/profile->page_size_bytes;x.maximum_pages_per_action=x.maximum_total_pages;
  x.maximum_work_bytes=x.maximum_pages_per_action*profile->page_size_bytes;
  x.minimum_free_pages=x.target_free_pages=x.growth_increment_pages=0x7fffffff;Good(x);
  --x.maximum_work_bytes;Invalid(x);
  x=p;x.maximum_pages_per_action=7;Invalid(x);x=p;--x.maximum_work_bytes;Invalid(x);
  x=p;x.maximum_retained_image_bytes=639;Invalid(x);x=p;x.maximum_retained_image_bytes=640;Good(x);
  x=p;x.cooldown_microseconds=86400000001ULL;Invalid(x);x=p;x.maximum_runtime_microseconds=86400000001ULL;Invalid(x);
  x=p;x.minimum_free_pages=x.target_free_pages=0;x.cooldown_microseconds=0;x.maximum_runtime_microseconds=1;Good(x);
  x=p;x.cooldown_microseconds=x.maximum_runtime_microseconds=86400000000ULL;Good(x);
  x=p;x.approval=static_cast<c::StorageActionApproval>(4);Invalid(x);
  x=p;x.refusal_pressure=static_cast<c::StorageActionPressure>(0);Invalid(x);x.refusal_pressure=static_cast<c::StorageActionPressure>(5);Invalid(x);
  x=p;x.origin_transaction_uuid.kind=UuidKind::object;Check(!c::EncodeCatalogStorageActionPolicy(x).ok(),"wrong typed origin kind rejected");
  // Flags never grant permission, but every valid combination must survive storage.
  for(unsigned flags=0;flags<8;++flags){x=p;x.enabled=flags&1;x.growth_allowed=flags&2;x.preallocation_allowed=flags&4;Good(x);}
  for(unsigned approval=0;approval<=3;++approval)for(unsigned pressure=1;pressure<=4;++pressure){x=p;
    x.approval=static_cast<c::StorageActionApproval>(approval);x.refusal_pressure=static_cast<c::StorageActionPressure>(pressure);Good(x);}
}
void Evolution(const P& p){const auto first=Metadata(p);auto next=p;++next.generation;next.maximum_runtime_microseconds++;
  auto second=Metadata(next);second.creator_transaction_uuid=Id(UuidKind::transaction,20);second.creator_local_transaction_id=2;
  Check(c::EncodeCatalogMetadataVersion(second).ok()&&c::CatalogMetadataPreservesFamilyOrigin(first,second),"actual common dispatcher admits bounded update preserving origin");
  auto earlier=second;earlier.creator_transaction_uuid=Id(UuidKind::transaction,21);earlier.creator_local_transaction_id=1;
  auto later_origin=p;later_origin.origin_local_transaction_id=20;const auto original_later=Metadata(later_origin);
  later_origin.generation=2;earlier.record.payload=Oracle(later_origin);
  Check(c::EncodeCatalogMetadataVersion(earlier).ok()&&c::CatalogMetadataPreservesFamilyOrigin(original_later,earlier),
    "creator number is not commit order; live MGA must independently admit visibility and mutation");
  auto deleted=second;deleted.record.header.deleted=true;deleted.lifecycle=c::CatalogObjectLifecycle::dropped;
  deleted.status=c::CatalogObjectStatus::retired;deleted.retired_transaction_uuid=deleted.creator_transaction_uuid;
  const auto tombstone=c::EncodeCatalogMetadataVersion(deleted);Check(tombstone.ok()&&c::DecodeCatalogMetadataVersion(tombstone.bytes).ok()&&
    c::CatalogMetadataPreservesFamilyOrigin(first,deleted),"native tombstone retains valid typed definition and origin");
  for(auto member:{&P::policy_uuid,&P::database_uuid,&P::filespace_uuid,&P::storage_profile_uuid,&P::page_size_profile_uuid}){
    auto changed=next;changed.*member=member==&P::page_size_profile_uuid?d::kCanonicalFilespacePageProfiles[(p.page_size_profile_uuid==d::kCanonicalFilespacePageProfiles[0].uuid)?1:0].uuid:Id(UuidKind::object,99).value;
    if(member==&P::page_size_profile_uuid)changed.maximum_work_bytes=8*131072;
    auto m=Metadata(changed);m.creator_transaction_uuid=second.creator_transaction_uuid;m.creator_local_transaction_id=2;
    Check(!c::CatalogMetadataPreservesFamilyOrigin(first,m),"identity/profile cannot be rewritten through update");
  }
  for(unsigned fault=0;fault<9;++fault){auto m=second;
    switch(fault){case 0:m.object_subtype="other";break;case 1:m.record.header.kind=c::CatalogRecordKind::sql_object;break;
      case 2:m.record.header.object_uuid=Id(UuidKind::object,99);break;case 3:m.definition_version++;break;
      case 4:m.authority_scope=c::CatalogAuthorityScope::cluster;break;case 5:m.owning_schema_uuid=Id(UuidKind::schema,99);break;
      case 6:m.default_name_uuid={};break;case 7:m.name_vector_uuid={};break;case 8:m.record.payload="caller supplied text policy";break;}
    Check(!c::CatalogStorageActionPolicyMatchesMetadata(m)&&!c::EncodeCatalogMetadataVersion(m).ok()&&
      !c::CatalogMetadataPreservesFamilyOrigin(first,m),"shared family validation rejects spoofed policy metadata");
  }
  auto changed=next;changed.origin_transaction_uuid=Id(UuidKind::transaction,22);auto m=Metadata(changed);
  Check(!c::CatalogMetadataPreservesFamilyOrigin(first,m),"immutable origin UUID");changed=next;changed.origin_local_transaction_id=2;
  m=Metadata(changed);Check(!c::CatalogMetadataPreservesFamilyOrigin(first,m),"immutable origin local number");
}
void Allocations(const P& p){const auto bytes=Oracle(p);const auto metadata=Metadata(p);
  for(unsigned mode=0;mode<4;++mode){const auto call=[&](){
    if(mode==0)return c::EncodeCatalogStorageActionPolicy(p).ok();
    if(mode==1)return c::DecodeCatalogStorageActionPolicy(bytes).ok();
    if(mode==2)return c::EncodeCatalogMetadataVersion(metadata).ok();
    return c::CatalogMetadataPreservesFamilyOrigin(metadata,metadata);
  };
    counting=true;allocations=0;const bool good=call();counting=false;Check(good,"allocation baseline");const auto count=allocations;
    for(unsigned long n=0;n<count;++n){allocation_budget=n;bool threw=false,accepted=false;try{accepted=call();}catch(const std::bad_alloc&){threw=true;}
      const bool consumed=allocation_budget==-1;allocation_budget=-1;Check(consumed&&(threw||!accepted),"allocator failure never becomes successful family admission");}
  }
}
void Attachment(const P& p){using A=c::CatalogStorageActionAttachment;
  const A a{Id(UuidKind::object,30).value,p.database_uuid,p.filespace_uuid,p.storage_profile_uuid,p.page_size_profile_uuid,
    p.policy_uuid,1,p.origin_transaction_uuid,p.origin_local_transaction_id};
  const auto oracle=[](const A& v){std::string b(24,0);b.replace(0,4,"SBCV");Put(b,4,1,2);Put(b,6,24,2);Put(b,12,9,4);Put(b,16,65549,4);Put(b,20,1,2);
    Field(b,1,5,Identity(v.attachment_uuid));Field(b,2,1,Number(v.generation));Field(b,3,5,Identity(v.database_uuid));
    Field(b,4,5,Identity(v.filespace_uuid));Field(b,5,5,Identity(v.storage_profile_uuid));Field(b,6,5,Identity(v.page_size_profile_uuid));
    Field(b,7,5,Identity(v.policy_uuid));Field(b,8,5,Identity(v.origin_transaction_uuid.value));Field(b,9,1,Number(v.origin_local_transaction_id));Put(b,8,b.size(),4);return b;};
  const auto metadata=[&](const A& v){auto m=Metadata(p);m.record.header.kind=c::CatalogRecordKind::config_profile;
    m.record.header.object_uuid.value=v.attachment_uuid;m.object_subtype="storage_action_attachment";
    m.definition_version=v.generation;m.record.payload=oracle(v);return m;};
  const auto bytes=oracle(a);const auto encoded=c::EncodeCatalogStorageActionAttachment(a);
  Check(encoded.ok()&&std::string(encoded.bytes.begin(),encoded.bytes.end())==bytes,"independent attachment byte oracle");
  const auto decoded=c::DecodeCatalogStorageActionAttachment(bytes);Check(decoded.ok()&&oracle(*decoded.record)==bytes,"all attachment fields round trip");
  const auto m=metadata(a);const auto native=c::EncodeCatalogMetadataVersion(m);Check(native.ok()&&c::DecodeCatalogMetadataVersion(native.bytes).ok(),"actual attachment common metadata round trip");
  const auto bad=[](std::string_view b){const auto r=c::DecodeCatalogStorageActionAttachment(b);Check(!r.ok()&&!r.record,"invalid attachment returns no prefix");};
  for(std::size_t n=0;n<bytes.size();++n)bad(std::string_view(bytes).substr(0,n));bad(bytes+"x");
  for(unsigned field=1;field<=9;++field){const auto at=Offset(bytes,field),length=Get(bytes,at+4,4);auto b=bytes;
    b.erase(at,8+length);Put(b,8,b.size(),4);Put(b,12,8,4);bad(b);
    b=bytes;Put(b,at,99,2);bad(b);b=bytes;Put(b,at+2,4,1);bad(b);b=bytes;Put(b,at+3,1,1);bad(b);
    b=bytes;Put(b,at+4,length+1,4);bad(b);
  }
  for(unsigned field:{1u,3u,4u,5u,6u,7u,8u})for(unsigned mode=0;mode<3;++mode){auto b=bytes;const auto at=Offset(b,field)+8;
    if(mode==0)std::fill_n(b.begin()+at,16,0);else if(mode==1)b[at+6]=0x40;else b[at+8]=0;bad(b);}
  for(unsigned field:{2u,9u}){auto b=bytes;Put(b,Offset(b,field)+8,0,8);bad(b);}
  auto successor=a;successor.generation=2;successor.policy_uuid=Id(UuidKind::object,31).value;
  const auto next=metadata(successor);Check(c::EncodeCatalogMetadataVersion(next).ok()&&c::CatalogMetadataPreservesFamilyOrigin(m,next),"attachment may select another policy through native version evolution");
  for(auto member:{&A::attachment_uuid,&A::database_uuid,&A::filespace_uuid,&A::storage_profile_uuid,&A::page_size_profile_uuid}){
    auto changed=successor;changed.*member=member==&A::page_size_profile_uuid?d::kCanonicalFilespacePageProfiles[(a.page_size_profile_uuid==d::kCanonicalFilespacePageProfiles[0].uuid)?1:0].uuid:Id(UuidKind::object,32).value;
    Check(!c::CatalogMetadataPreservesFamilyOrigin(m,metadata(changed)),"attachment cannot retarget immutable identity/profile");}
  auto changed_origin=successor;changed_origin.origin_transaction_uuid=Id(UuidKind::transaction,32);
  Check(!c::CatalogMetadataPreservesFamilyOrigin(m,metadata(changed_origin)),"attachment origin UUID is immutable");
  changed_origin=successor;++changed_origin.origin_local_transaction_id;
  Check(!c::CatalogMetadataPreservesFamilyOrigin(m,metadata(changed_origin)),"attachment origin transaction number is immutable");
  for(unsigned mode=0;mode<9;++mode){auto changed=next;
    if(mode==0)changed.object_subtype="generic";if(mode==1)changed.record.header.kind=c::CatalogRecordKind::policy;
    if(mode==2)changed.record.header.object_uuid=Id(UuidKind::object,33);if(mode==3)changed.definition_version++;
    if(mode==4)changed.authority_scope=c::CatalogAuthorityScope::cluster;if(mode==5)changed.owning_schema_uuid=Id(UuidKind::schema,33);
    if(mode==6)changed.default_name_uuid={};if(mode==7)changed.name_vector_uuid={};if(mode==8)changed.record.payload="text attachment";
    Check(!c::EncodeCatalogMetadataVersion(changed).ok()&&!c::CatalogMetadataPreservesFamilyOrigin(m,changed),"native dispatcher refuses forged attachment bindings");}
  auto retired=next;retired.record.header.deleted=true;retired.lifecycle=c::CatalogObjectLifecycle::dropped;
  retired.status=c::CatalogObjectStatus::retired;retired.retired_transaction_uuid=retired.creator_transaction_uuid;
  Check(c::EncodeCatalogMetadataVersion(retired).ok()&&c::CatalogMetadataPreservesFamilyOrigin(m,retired),"attachment tombstone retains definition");
  for(unsigned mode=0;mode<4;++mode){const auto call=[&](){if(mode==0)return c::EncodeCatalogStorageActionAttachment(a).ok();
    if(mode==1)return c::DecodeCatalogStorageActionAttachment(bytes).ok();if(mode==2)return c::EncodeCatalogMetadataVersion(m).ok();return c::CatalogMetadataPreservesFamilyOrigin(m,next);};
    counting=true;allocations=0;const bool good=call();counting=false;const auto count=allocations;Check(good,"attachment allocation baseline");
    for(unsigned long n=0;n<count;++n){allocation_budget=n;bool threw=false,accepted=false;try{accepted=call();}catch(const std::bad_alloc&){threw=true;}
      const bool consumed=allocation_budget==-1;allocation_budget=-1;Check(consumed&&(threw||!accepted),"attachment allocation failure refuses complete admission");}
  }
}
}
int main(){try{for(unsigned profile=0;profile<5;++profile){const auto p=Example(profile);Good(p);Malformed(p);Boundaries(p);Evolution(p);Allocations(p);Attachment(p);}
  std::cout<<"PASS native storage policy checks="<<checks<<'\n';return 0;
}catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}}
