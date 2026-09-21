// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "catalog_schema_definition.hpp"
#include "catalog_schema_record_codec.hpp"
#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <new>
#include <string>

namespace { long allocation_budget=-1; }
void* operator new(std::size_t size) {
  if (allocation_budget==0) { allocation_budget=-1; throw std::bad_alloc(); }
  if (allocation_budget>0) --allocation_budget;
  if (auto* p=std::malloc(size?size:1)) return p;
  throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return ::operator new(n); }

void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p,std::size_t) noexcept { std::free(p); }
void operator delete[](void* p,std::size_t) noexcept { std::free(p); }

namespace c=scratchbird::core::catalog;
namespace p=scratchbird::core::platform;
unsigned checks=0,failures=0;
void Check(bool ok,const char* why) { ++checks; if(!ok) { ++failures; std::cerr<<"FAIL "<<why<<'\n'; } }
bool SharedSchemaOrigin(const c::CatalogMetadataVersion& a,const c::CatalogMetadataVersion& b) {
  const bool family=c::CatalogSchemaDefinitionPreservesOrigin(a,b);
  const bool shared=c::CatalogMetadataPreservesFamilyOrigin(a,b);
  Check(shared==family,"shared native origin dispatcher differs from family contract");
  return family;
}
p::TypedUuid Id(p::UuidKind kind,unsigned char tag) {
  return {kind,p::Uuid{{1,2,3,4,5,6,0x71,8,0x89,10,11,12,13,14,15,tag}}};
}
void Put(std::string& s,std::size_t at,p::u64 value,unsigned width) {
  for(unsigned i=0;i<width;++i) s.at(at+i)=char(value>>(8*i));
}
std::string Number(p::u64 n) { std::string s(8,0); Put(s,0,n,8); return s; }
std::string UuidBytes(const p::TypedUuid& id) { return {reinterpret_cast<const char*>(id.value.bytes.data()),16}; }
void Field(std::string& s,unsigned id,unsigned type,const std::string& value) {
  const auto at=s.size();s.resize(at+8,0);Put(s,at,id,2);Put(s,at+2,type,1);Put(s,at+4,value.size(),4);s+=value;
}
// Exact independent schema/TLV oracle. No production schema or encoder calls.
std::string Golden(const c::CatalogSchemaDefinition& d) {
  std::string s(24,0);s.replace(0,4,"SBCV");Put(s,4,1,2);Put(s,6,24,2);Put(s,16,65540,4);Put(s,20,1,2);
  unsigned count=5;
  Field(s,1,5,UuidBytes(d.schema_object_uuid));Field(s,2,5,UuidBytes(d.database_catalog_object_uuid));
  if(!d.parent_schema_uuid.value.is_nil()){Field(s,3,5,UuidBytes(d.parent_schema_uuid));++count;}
  Field(s,4,1,Number(static_cast<p::u64>(d.schema_type)));
  for(const auto [tag,ref]:std::array<std::pair<unsigned,p::TypedUuid>,4>{{
      {5,d.default_filespace_uuid},{6,d.default_charset_uuid},{7,d.default_collation_uuid},{8,d.permissions_policy_uuid}}})
    if(!ref.value.is_nil()){Field(s,tag,5,UuidBytes(ref));++count;}
  Field(s,9,5,UuidBytes(d.origin_transaction_uuid));Field(s,10,1,Number(d.origin_local_transaction_id));
  Put(s,8,s.size(),4);Put(s,12,count,4);return s;
}
c::CatalogSchemaDefinition Definition(unsigned options=31,unsigned type=6) {
  c::CatalogSchemaDefinition d;
  d.schema_object_uuid=Id(p::UuidKind::object,1);d.database_catalog_object_uuid=Id(p::UuidKind::object,2);
  if(options&1)d.parent_schema_uuid=Id(p::UuidKind::object,3);
  if(options&2)d.default_filespace_uuid=Id(p::UuidKind::filespace,4);
  if(options&4)d.default_charset_uuid=Id(p::UuidKind::object,5);
  if(options&8)d.default_collation_uuid=Id(p::UuidKind::object,6);
  if(options&16)d.permissions_policy_uuid=Id(p::UuidKind::object,7);
  d.schema_type=static_cast<c::CatalogSchemaType>(type);
  d.origin_transaction_uuid=Id(p::UuidKind::transaction,8);d.origin_local_transaction_id=11;
  return d;
}
c::CatalogMetadataVersion Metadata(const c::CatalogSchemaDefinition& d) {
  c::CatalogMetadataVersion m;
  m.record.header.kind=c::CatalogRecordKind::schema;m.record.header.row_uuid=Id(p::UuidKind::row,9);
  m.record.header.object_uuid=d.schema_object_uuid;
  m.record.header.parent_uuid=d.parent_schema_uuid.value.is_nil()?d.database_catalog_object_uuid:d.parent_schema_uuid;
  m.record.payload=Golden(d);m.owner_uuid=Id(p::UuidKind::principal,10);m.audit_uuid=Id(p::UuidKind::object,11);
  m.default_name_uuid=Id(p::UuidKind::object,12);m.name_vector_uuid=Id(p::UuidKind::object,13);
  m.security_policy_uuid=d.permissions_policy_uuid;
  if(!d.parent_schema_uuid.value.is_nil()){m.owning_schema_uuid=d.parent_schema_uuid;m.owning_schema_uuid.kind=p::UuidKind::schema;}
  m.creator_transaction_uuid=d.origin_transaction_uuid;m.creator_local_transaction_id=d.origin_local_transaction_id;
  m.definition_version=m.schema_epoch=m.security_epoch=m.catalog_generation=m.dependency_generation=m.invalidation_generation=1;
  m.lifecycle=c::CatalogObjectLifecycle::active;m.status=c::CatalogObjectStatus::active;
  m.trace_search_key="SCHEMA-DEFINITION-ORACLE";m.retention_class="catalog_history";
  const std::array names{"system","user_home","remote_native","remote_emulated","public_compat","application","cluster"};
  m.object_subtype=names[static_cast<unsigned>(d.schema_type)-1];
  if(d.schema_type==c::CatalogSchemaType::cluster)m.authority_scope=c::CatalogAuthorityScope::cluster;
  return m;
}
void Refused(std::string_view bytes) {
  const auto r=c::DecodeCatalogSchemaDefinition(bytes);Check(!r.ok()&&!r.definition,"malformed input exposed definition");
}
int main() {
  for(unsigned options=0;options<32;++options)for(unsigned type=1;type<=7;++type) {
    const auto d=Definition(options,type);const auto expected=Golden(d);const auto encoded=c::EncodeCatalogSchemaDefinition(d);
    Check(encoded.ok()&&std::string(encoded.bytes.begin(),encoded.bytes.end())==expected,"independent full byte oracle");
    const auto decoded=c::DecodeCatalogSchemaDefinition(expected);
    Check(decoded.ok()&&Golden(*decoded.definition)==expected,"all defaults/types and original identity preserved");
    const auto m=Metadata(d);Check(c::CatalogSchemaDefinitionMatchesMetadata(m),"complete family/common binding");
    const auto wrapped=c::EncodeCatalogMetadataVersion(m);Check(wrapped.ok(),"native common envelope admits schema");
    if(wrapped.ok()){const auto reread=c::DecodeCatalogMetadataVersion(wrapped.bytes);Check(reread.ok()&&reread.record.record.payload==expected,"native envelope preserves binary definition");}
  }
  const auto d=Definition();const auto golden=Golden(d);const auto metadata=Metadata(d);
  for(std::size_t n=0;n<golden.size();++n)Refused(std::string_view(golden).substr(0,n));
  auto excess=golden;excess.push_back(0);Refused(excess);
  for(const auto at:{0u,4u,6u,8u,12u,16u,20u,22u,24u,26u,27u,28u}){auto bad=golden;bad[at]^=1;Refused(bad);}
  for(auto field:{&c::CatalogSchemaDefinition::schema_object_uuid,&c::CatalogSchemaDefinition::database_catalog_object_uuid,
      &c::CatalogSchemaDefinition::parent_schema_uuid,&c::CatalogSchemaDefinition::default_filespace_uuid,
      &c::CatalogSchemaDefinition::default_charset_uuid,&c::CatalogSchemaDefinition::default_collation_uuid,
      &c::CatalogSchemaDefinition::permissions_policy_uuid,&c::CatalogSchemaDefinition::origin_transaction_uuid}) {
    for(unsigned version=0;version<16;++version)if(version!=7){auto bad=d;(bad.*field).value.bytes[6]=static_cast<unsigned char>(version<<4);
      Check(!c::EncodeCatalogSchemaDefinition(bad).ok(),"non-v7 system identity refused");Refused(Golden(bad));}
    for(unsigned kind=0;kind<256;++kind)if(kind!=static_cast<unsigned>((d.*field).kind)){
      auto bad=d;(bad.*field).kind=static_cast<p::UuidKind>(kind);const auto r=c::EncodeCatalogSchemaDefinition(bad);
      Check(!r.ok()&&r.bytes.empty(),"wrong typed identity refused");}
    auto nil=d;(nil.*field).value={};Check(!c::EncodeCatalogSchemaDefinition(nil).ok(),"typed nil is not absence");
  }
  for(unsigned type:{0u,8u,255u}){auto bad=d;bad.schema_type=static_cast<c::CatalogSchemaType>(type);Check(!c::EncodeCatalogSchemaDefinition(bad).ok(),"unknown schema enum refused");Refused(Golden(bad));}
  for(unsigned fault=0;fault<4;++fault){auto bad=d;
    if(fault==0)bad.schema_object_uuid=bad.database_catalog_object_uuid;
    if(fault==1)bad.parent_schema_uuid=bad.schema_object_uuid;
    if(fault==2)bad.parent_schema_uuid=bad.database_catalog_object_uuid;
    if(fault==3)bad.origin_local_transaction_id=0;
    Check(!c::EncodeCatalogSchemaDefinition(bad).ok(),"identity collision or zero origin refused");Refused(Golden(bad));
  }
  for(auto counter:{p::u64{1},std::numeric_limits<p::u64>::max()}){auto edge=d;edge.origin_local_transaction_id=counter;
    const auto encoded=c::EncodeCatalogSchemaDefinition(edge);Check(encoded.ok(),"full nonzero origin counter range");
    const auto decoded=c::DecodeCatalogSchemaDefinition(Golden(edge));Check(decoded.ok()&&decoded.definition->origin_local_transaction_id==counter,"origin counter exact width");}
  for(unsigned fault=0;fault<15;++fault){auto bad=metadata;
    if(fault==0)bad.record.header.object_uuid=Id(p::UuidKind::object,20);
    if(fault==1)bad.record.header.parent_uuid=Id(p::UuidKind::object,20);
    if(fault==2)bad.default_name_uuid={};if(fault==3)bad.name_vector_uuid={};
    if(fault==4)bad.security_policy_uuid={};if(fault==5)bad.object_subtype="wrong";
    if(fault==6)bad.owning_schema_uuid={};if(fault==7)bad.owning_schema_uuid.kind=p::UuidKind::object;
    if(fault==8)bad.creator_local_transaction_id=10;if(fault==9)bad.creator_local_transaction_id=12;
    if(fault==10)bad.creator_transaction_uuid=Id(p::UuidKind::transaction,20);
    if(fault==11)bad.status=c::CatalogObjectStatus::deferred;
    if(fault==12)bad.status=c::CatalogObjectStatus::bridge_only;
    if(fault==13)bad.record.payload="SBAPI1\tRECORD\tschema";
    if(fault==14){auto root=Definition(30);bad.record.payload=Golden(root);}
    Check(!c::CatalogSchemaDefinitionMatchesMetadata(bad),"mismatched family/common fields refused");
    const auto encoded=c::EncodeCatalogMetadataVersion(bad);Check(!encoded.ok()&&encoded.bytes.empty(),"invalid schema cannot enter native envelope");
  }
  {auto cluster=Metadata(Definition(31,7));cluster.authority_scope=c::CatalogAuthorityScope::local;
    Check(!c::EncodeCatalogMetadataVersion(cluster).ok(),"cluster schema cannot claim local authority");}
  for(unsigned bits=0;bits<16;++bits){auto m=metadata;
    if(bits&1)m.record.header.deleted=true;
    if(bits&2)m.status=c::CatalogObjectStatus::retired;
    if(bits&4)m.lifecycle=c::CatalogObjectLifecycle::dropped;
    if(bits&8)m.retired_transaction_uuid=m.creator_transaction_uuid;
    const auto encoded=c::EncodeCatalogMetadataVersion(m);
    if(bits&1)Check(encoded.ok()==(bits==15),"deleted schema requires actual common retirement fields");
    else if((bits&8)&&!(bits&2))Check(!encoded.ok(),"common retirement transaction requires retirement status");}
  for(unsigned life=1;life<=10;++life)if(life!=6){auto m=metadata;m.lifecycle=static_cast<c::CatalogObjectLifecycle>(life);
    Check(c::EncodeCatalogMetadataVersion(m).ok(),"schema family preserves admitted common lifecycle representations");}
  {auto m=metadata;m.status=c::CatalogObjectStatus::quarantined;m.lifecycle=c::CatalogObjectLifecycle::quarantined;
    m.retired_transaction_uuid=m.creator_transaction_uuid;
    Check(c::EncodeCatalogMetadataVersion(m).ok(),"common quarantined retirement state remains representable");}
  auto successor=metadata;successor.definition_version=2;successor.creator_local_transaction_id=12;successor.creator_transaction_uuid=Id(p::UuidKind::transaction,21);
  Check(SharedSchemaOrigin(metadata,successor),"successor preserves original creation independently of version creator");
  {auto retired=successor;retired.record.header.deleted=true;retired.lifecycle=c::CatalogObjectLifecycle::dropped;
    retired.status=c::CatalogObjectStatus::retired;retired.retired_transaction_uuid=retired.creator_transaction_uuid;
    Check(c::EncodeCatalogMetadataVersion(retired).ok()&&SharedSchemaOrigin(metadata,retired),"schema retirement preserves original creation");}
  {auto other=metadata;other.record.header.kind=c::CatalogRecordKind::table_descriptor;other.object_subtype="table";other.record.payload="unrelated";
    Check(!SharedSchemaOrigin(metadata,other)&&!SharedSchemaOrigin(other,metadata),"shared schema origin refuses family swaps");
    Check(SharedSchemaOrigin(other,other),"shared schema origin preserves unrelated families");}
  unsigned origin_faults=0;
  for(long n=0;n<1000;++n){bool threw=false;allocation_budget=n;
    try {const bool admitted=c::CatalogMetadataPreservesFamilyOrigin(metadata,successor);allocation_budget=-1;
      Check(admitted,"shared origin rejects unchanged schema after allocation recovery");}
    catch(const std::bad_alloc&){allocation_budget=-1;threw=true;++origin_faults;}
    if(!threw)break;
  }
  Check(origin_faults>0&&origin_faults<999,"shared origin propagates every injected allocation failure");
  for(unsigned fault=0;fault<3;++fault){auto changed=d;
    if(fault==0)changed.database_catalog_object_uuid=Id(p::UuidKind::object,20);
    if(fault==1)changed.origin_transaction_uuid=Id(p::UuidKind::transaction,20);
    if(fault==2)changed.origin_local_transaction_id=10;
    successor.record.payload=Golden(changed);
    Check(c::CatalogSchemaDefinitionMatchesMetadata(successor),"individually valid successor needs history check");
    Check(!SharedSchemaOrigin(metadata,successor),"database/origin replacement refused");
  }
  c::CatalogSchemaRecord seed;seed.schema_object_uuid=d.schema_object_uuid;seed.parent_object_uuid=d.database_catalog_object_uuid;
  seed.root_schema=true;seed.path_cache="users";seed.name_cache="users";
  const auto bootstrap=c::EncodeCatalogSchemaRecord(seed);Check(bootstrap.ok(),"unchanged bootstrap codec remains usable");
  auto counterfeit=metadata;counterfeit.record.payload.assign(bootstrap.bytes.begin(),bootstrap.bytes.end());
  Check(!c::EncodeCatalogMetadataVersion(counterfeit).ok(),"bootstrap seed is not a mutable schema definition");
  unsigned faults=0;
  for(long n=0;n<1000;++n){bool threw=false;allocation_budget=n;
    try { const auto e=c::EncodeCatalogMetadataVersion(metadata);allocation_budget=-1;Check(e.ok(),"allocation sweep returned invalid success"); }
    catch(const std::bad_alloc&){allocation_budget=-1;threw=true;++faults;}
    if(!threw)break;
  }
  Check(faults>0&&faults<999,"allocation failures exhausted and recovered");
  Check(c::EncodeCatalogMetadataVersion(metadata).ok(),"independent call after faults succeeds");
  std::cout<<"schema_definition checks="<<checks<<" faults="<<faults<<" failures="<<failures<<'\n';
  return failures?1:0;
}
