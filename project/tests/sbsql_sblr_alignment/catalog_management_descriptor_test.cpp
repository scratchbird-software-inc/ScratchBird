// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "catalog_management_descriptor.hpp"
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <new>

namespace c=scratchbird::core::catalog;
using namespace scratchbird::core::platform;
namespace {
bool forbid_heap=false;unsigned checks=0;
void Check(bool good,const char* why){++checks;if(!good){forbid_heap=false;std::cerr<<why<<'\n';std::abort();}}
Uuid Id(unsigned n){Uuid id;id.bytes[0]=1;id.bytes[6]=0x70;id.bytes[8]=0x80;id.bytes[14]=n>>8;id.bytes[15]=n;return id;}
std::string_view Text(std::span<const byte> bytes){return {reinterpret_cast<const char*>(bytes.data()),bytes.size()};}
struct Oracle {
  std::array<byte,440> bytes{};std::size_t at=0;
  void Number(u64 value,unsigned count){for(unsigned i=0;i<count;++i)bytes[at++]=byte(value>>(8*i));}
  Oracle(unsigned size,unsigned count,unsigned schema){for(char x:std::string_view("SBCV"))bytes[at++]=x;
    Number(1,2);Number(24,2);Number(size,4);Number(count,4);Number(schema,4);Number(1,2);Number(0,2);}
  void Identity(unsigned tag,unsigned value){Number(tag,2);Number(5,1);Number(0,1);Number(16,4);
    const auto id=Id(value);for(byte b:id.bytes)bytes[at++]=b;}
  void Scalar(unsigned tag,u64 value){Number(tag,2);Number(1,1);Number(0,1);Number(8,4);Number(value,8);}
};
c::CatalogMetadataVersionView Metadata(std::string_view payload,Uuid object,scratchbird::core::catalog::CatalogObjectSubtype subtype){
  c::CatalogMetadataVersionView m;m.record.header.kind=c::CatalogRecordKind::management_operation;
  m.record.header.object_uuid={UuidKind::object,object};m.record.header.row_uuid={UuidKind::row,Id(90)};
  m.record.header.parent_uuid={UuidKind::object,Id(91)};m.record.payload=payload;m.object_subtype=subtype;m.definition_version=1;
  m.owning_schema_uuid={UuidKind::schema,Id(91)};m.owner_uuid={UuidKind::principal,Id(92)};
  m.audit_uuid={UuidKind::object,Id(93)};m.default_name_uuid={UuidKind::object,Id(94)};m.name_vector_uuid={UuidKind::object,Id(95)};
  m.creator_transaction_uuid={UuidKind::transaction,Id(3)};m.creator_local_transaction_id=1;return m;
}
}
void* operator new(std::size_t n){if(forbid_heap)std::abort();if(auto* p=std::malloc(n?n:1))return p;throw std::bad_alloc();}
void* operator new[](std::size_t n){return ::operator new(n);}
void operator delete(void* p)noexcept{std::free(p);}void operator delete[](void* p)noexcept{std::free(p);}
void operator delete(void* p,std::size_t)noexcept{std::free(p);}void operator delete[](void* p,std::size_t)noexcept{std::free(p);}
int main(){
  c::CatalogManagementProfile p;p.profile_uuid=Id(1);p.database_uuid=Id(2);p.generation=1;
  p.code=c::CatalogManagementProfileCode::verification_family;
  p.origin_transaction_uuid=p.creating_transaction_uuid={UuidKind::transaction,Id(3)};
  p.origin_local_transaction_id=p.creating_local_transaction_id=1;
  c::CatalogManagementOperationDescriptor d;d.descriptor_uuid=Id(4);d.database_uuid=Id(2);d.generation=1;
  d.code=c::CatalogManagementOperationCode::verify_start;d.flags=31;
  d.origin_transaction_uuid=d.creating_transaction_uuid=p.origin_transaction_uuid;d.origin_local_transaction_id=d.creating_local_transaction_id=1;
  for(unsigned i=0;i<10;++i)d.profiles[i]=Id(10+i);
  Oracle po(184,8,65825);po.Identity(1,1);po.Identity(2,2);po.Scalar(3,1);po.Scalar(4,1);
  po.Identity(5,3);po.Scalar(6,1);po.Identity(7,3);po.Scalar(8,1);
  Oracle op(440,19,65826);op.Identity(1,4);op.Identity(2,2);op.Scalar(3,1);op.Scalar(4,1);
  for(unsigned i=0;i<10;++i)op.Identity(5+i,10+i);op.Scalar(15,31);op.Identity(16,3);op.Scalar(17,1);op.Identity(18,3);op.Scalar(19,1);
  std::array<byte,440> out{},unchanged{};out.fill(0xa5);unchanged=out;
  forbid_heap=true;
  Check(c::CatalogManagementProfileEncodedBytes(p)==184&&c::CatalogManagementDescriptorEncodedBytes(d)==440,"exact canonical widths");
  auto profile=c::EncodeCatalogManagementProfileInto(p,out);
  Check(profile.ok()&&profile.bytes.size()==po.at&&std::equal(profile.bytes.begin(),profile.bytes.end(),po.bytes.begin()),"independent profile wire oracle");
  auto descriptor=c::EncodeCatalogManagementDescriptorInto(d,out);
  Check(descriptor.ok()&&descriptor.bytes.size()==op.at&&out==op.bytes,"independent descriptor wire oracle");
  const auto decoded=c::DecodeCatalogManagementDescriptor(Text(op.bytes));
  Check(decoded.ok()&&decoded.record->profiles==d.profiles&&decoded.record->origin_local_transaction_id==1&&decoded.record->flags==31,"exact descriptor fields");
  for(unsigned code=0;code<=11;++code){auto r=p;r.code=c::CatalogManagementProfileCode(code);
    const auto image=c::EncodeCatalogManagementProfileInto(r,out);
    Check(image.ok()==(code>=1&&code<=10),"closed registered profile codes");
    if(image.ok())Check(c::DecodeCatalogManagementProfile(Text(image.bytes)).record->code==r.code,"all registered profiles preserve code");
  }
  for(unsigned flags=0;flags<64;++flags){auto r=d;r.flags=flags;
    Check(c::EncodeCatalogManagementDescriptorInto(r,out).ok()==((flags&3)==3&&flags<32),"closed flags require actual durable and dry-run declaration");}
  for(std::size_t n=0;n<184;++n){out=unchanged;Check(!c::EncodeCatalogManagementProfileInto(p,std::span{out}.first(n)).ok()&&out==unchanged,"short profile output remains untouched");
    Check(!c::DecodeCatalogManagementProfile(Text(std::span{po.bytes}.first(n))).ok(),"every profile truncation refuses");}
  for(std::size_t n=0;n<440;++n){out=unchanged;Check(!c::EncodeCatalogManagementDescriptorInto(d,std::span{out}.first(n)).ok()&&out==unchanged,"short descriptor output remains untouched");
    Check(!c::DecodeCatalogManagementDescriptor(Text(std::span{op.bytes}.first(n))).ok(),"every descriptor truncation refuses");}
  for(unsigned fault=0;fault<12;++fault){auto r=d;
    if(fault==0)r.descriptor_uuid={};if(fault==1)r.database_uuid=r.descriptor_uuid;if(fault==2)r.generation=0;
    if(fault==3)r.code=c::CatalogManagementOperationCode(2);if(fault==4)r.profiles[3]=r.profiles[0];
    if(fault==5)r.profiles[2]=r.database_uuid;if(fault==6)r.profiles[4]=r.origin_transaction_uuid.value;
    if(fault==7)r.origin_local_transaction_id=0;if(fault==8)r.creating_local_transaction_id=2;
    if(fault==9)r.creating_transaction_uuid.value=Id(5);if(fault==10)r.origin_transaction_uuid.kind=UuidKind::object;
    if(fault==11)r.profiles[9]={};out=unchanged;
    Check(!c::EncodeCatalogManagementDescriptorInto(r,out).ok()&&out==unchanged,"invalid descriptor has no encoded prefix");}
  auto pm=Metadata(Text(std::span{po.bytes}.first(po.at)),p.profile_uuid,scratchbird::core::catalog::CatalogObjectSubtype::management_profile);
  auto dm=Metadata(Text(op.bytes),d.descriptor_uuid,scratchbird::core::catalog::CatalogObjectSubtype::management_descriptor);
  Check(c::CatalogManagementProfileMatchesMetadata(pm)&&c::CatalogManagementDescriptorMatchesMetadata(dm)&&
      c::CatalogManagementDefinitionMatchesDatabase(dm,Id(2))&&!c::CatalogManagementDefinitionMatchesDatabase(dm,Id(20)),"native metadata and database binding");
  for(unsigned fault=0;fault<8;++fault){auto m=dm;
    if(fault==0)m.object_subtype=scratchbird::core::catalog::CatalogObjectSubtype::management_profile;if(fault==1)m.definition_version=2;
    if(fault==2)m.creator_local_transaction_id=2;if(fault==3)m.creator_transaction_uuid.value=Id(5);
    if(fault==4)m.record.header.object_uuid.value=Id(5);if(fault==5)m.owning_schema_uuid.value=Id(5);
    if(fault==6)m.authority_scope=c::CatalogAuthorityScope::cluster;if(fault==7)m.record.header.kind=c::CatalogRecordKind::policy;
    Check(!c::CatalogManagementDescriptorMatchesMetadata(m),"metadata substitution refuses");}
  auto next=d;next.generation=2;next.creating_transaction_uuid.value=Id(5);next.creating_local_transaction_id=2;
  auto successor=c::EncodeCatalogManagementDescriptorInto(next,out);Check(successor.ok(),"later generation has exact separate creating transaction");
  auto nm=dm;nm.record.payload=Text(successor.bytes);nm.definition_version=2;nm.creator_transaction_uuid=next.creating_transaction_uuid;nm.creator_local_transaction_id=2;
  Check(c::CatalogManagementDefinitionPreservesOrigin(dm,nm)&&!c::CatalogManagementDefinitionPreservesOrigin(nm,dm),"native versions preserve immutable origin and advance semantic generation");
  Check(c::EncodeCatalogManagementDescriptorInto(d,{reinterpret_cast<byte*>(&d),sizeof(d)}).error==c::CatalogValueError::invalid_backing,"descriptor cannot overwrite its source object");
  forbid_heap=false;std::cout<<checks<<" native management declaration checks\n";
}
