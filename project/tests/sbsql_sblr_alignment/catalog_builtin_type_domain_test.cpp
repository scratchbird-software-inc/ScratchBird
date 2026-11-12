// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
// SB_NATIVE_BUILTIN_DATATYPE_DOMAIN_ROWS: the binary, name-free catalog rows
// for built-in datatypes and system domains. Codec, common-dispatcher binding,
// issuer reservation, the constant system-domain identities, and one row per
// actual built-in datatype. Native create's use of these rows is tested with
// native create.
#include "catalog_builtin_type_domain.hpp"
#include "native_creation_catalog_input.hpp"
#include "bootstrap_policy_fixture.hpp"
#include "datatype_descriptor.hpp"
#include "datatype_catalog_manifest.hpp"
#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>
#include <new>
#include <set>
#include <source_location>
#include <stdexcept>
thread_local long allocation_budget=-1;
thread_local unsigned allocation_count=0;
void* operator new(std::size_t size){++allocation_count;if(allocation_budget>=0&&allocation_budget--==0)throw std::bad_alloc();
  if(auto* p=std::malloc(size?size:1))return p;throw std::bad_alloc();}
void* operator new[](std::size_t size){return ::operator new(size);}
void operator delete(void* p)noexcept{std::free(p);}void operator delete[](void* p)noexcept{std::free(p);}
void operator delete(void* p,std::size_t)noexcept{std::free(p);}void operator delete[](void* p,std::size_t)noexcept{std::free(p);}
namespace c=scratchbird::core::catalog;namespace p=scratchbird::core::platform;namespace f=scratchbird::tests::bootstrap_policy;
namespace db=scratchbird::storage::database;namespace u=scratchbird::core::uuid;namespace dt=scratchbird::core::datatypes;
namespace {
using Datatype=c::CatalogBuiltinDatatypeDescriptor;using Domain=c::CatalogSystemDomain;using u64=p::u64;
unsigned checks=0;
void Check(bool ok,const char* why,std::source_location loc=std::source_location::current()){++checks;
 if(!ok){allocation_budget=-1;throw std::runtime_error(std::string(why)+" line="+std::to_string(loc.line()));}}
template<class Call>auto NoHeap(Call&& call){allocation_budget=0;allocation_count=0;auto result=call();const auto calls=allocation_count;allocation_budget=-1;
 Check(!calls,"bounded built-in type/domain codecs never allocate");return result;}
const p::TypedUuid tx{p::UuidKind::transaction,f::Id(5)};
Datatype DatatypeValue(){Datatype r;r.descriptor_uuid=f::Id(100);r.database_uuid=f::Id(1);r.generation=1;r.catalog_epoch=1;
 r.type_code=0x66;r.family_code=3;r.width_class_code=2;r.bit_width=32;r.default_precision=10;r.default_scale=0;
 r.flags=u64(c::BuiltinDatatypeFlag::nullable_allowed)|u64(c::BuiltinDatatypeFlag::descriptor_authoritative);
 r.origin_transaction_uuid=r.creating_transaction_uuid=tx;r.origin_local_transaction_id=r.creating_local_transaction_id=1;return r;}
Domain DomainValue(){Domain r;r.domain_uuid=c::SystemDomainUuid(0x66);r.database_uuid=f::Id(1);r.generation=1;
 r.kind=c::SystemDomainKind::system_domain;r.base_descriptor_uuid=f::Id(100);r.base_descriptor_generation=1;
 r.flags=u64(c::SystemDomainFlag::descriptor_authoritative);
 r.origin_transaction_uuid=r.creating_transaction_uuid=tx;r.origin_local_transaction_id=r.creating_local_transaction_id=1;return r;}
// Independent SBCV construction (bootstrap_policy_fixture.hpp), not the production schema.
std::string Golden(const Datatype& r){f::Golden g(65632);g.Identity(1,r.descriptor_uuid);g.Identity(2,r.database_uuid);
 g.Integer(3,r.generation);g.Integer(4,r.catalog_epoch);g.Integer(5,r.type_code);g.Integer(6,r.family_code);
 g.Integer(7,r.width_class_code);g.Integer(8,r.bit_width);g.Integer(9,r.default_precision);g.Integer(10,r.default_scale);
 g.Integer(11,r.flags);g.Identity(12,r.origin_transaction_uuid.value);g.Integer(13,r.origin_local_transaction_id);
 g.Identity(14,r.creating_transaction_uuid.value);g.Integer(15,r.creating_local_transaction_id);return g.Finish();}
std::string Golden(const Domain& r){f::Golden g(65633);g.Identity(1,r.domain_uuid);g.Identity(2,r.database_uuid);
 g.Integer(3,r.generation);g.Integer(4,u64(r.kind));g.Identity(5,r.base_descriptor_uuid);g.Integer(6,r.base_descriptor_generation);
 g.Integer(7,r.flags);g.Identity(8,r.origin_transaction_uuid.value);g.Integer(9,r.origin_local_transaction_id);
 g.Identity(10,r.creating_transaction_uuid.value);g.Integer(11,r.creating_local_transaction_id);return g.Finish();}
struct DatatypeCodec{static constexpr std::size_t bytes=c::kCatalogValueBlockHeaderBytes+15*8+4*16+11*8;
 static auto Size(const Datatype& r){return c::CatalogBuiltinDatatypeDescriptorEncodedBytes(r);}
 static auto Into(const Datatype& r,std::span<p::byte> o){return c::EncodeCatalogBuiltinDatatypeDescriptorInto(r,o);}
 static auto Decode(std::string_view b){return c::DecodeCatalogBuiltinDatatypeDescriptor(b);}};
struct DomainCodec{static constexpr std::size_t bytes=c::kCatalogValueBlockHeaderBytes+11*8+5*16+6*8;
 static auto Size(const Domain& r){return c::CatalogSystemDomainEncodedBytes(r);}
 static auto Into(const Domain& r,std::span<p::byte> o){return c::EncodeCatalogSystemDomainInto(r,o);}
 static auto Decode(std::string_view b){return c::DecodeCatalogSystemDomain(b);}};
template<class Codec,class R> void Roundtrip(const R& value){const auto expected=Golden(value);
 Check(expected.size()==Codec::bytes,"independent block has the expected size");
 std::array<p::byte,Codec::bytes+2> output;output.fill(0xa7);
 const auto size=NoHeap([&]{return Codec::Size(value);});Check(size&&*size==expected.size(),"exact byte count");
 const auto encoded=NoHeap([&]{return Codec::Into(value,std::span(output).subspan(1));});
 Check(encoded.ok()&&std::string_view(reinterpret_cast<const char*>(encoded.bytes.data()),encoded.bytes.size())==expected,"exact independent SBCV fields");
 Check(output.front()==0xa7&&std::all_of(output.begin()+expected.size()+1,output.end(),[](auto b){return b==0xa7;}),"unaligned destination preserves guards");
 const auto decoded=NoHeap([&]{return Codec::Decode(expected);});Check(decoded.ok()&&Golden(*decoded.record)==expected,"all fields retained");
 Check(c::IsCatalogBuiltinTypeDomainPayload(expected),"family recognised by schema");}
template<class Codec,class R> void Refuse(const R& value,bool wire=true){std::array<p::byte,Codec::bytes> output;output.fill(0xa7);const auto before=output;
 const auto encoded=NoHeap([&]{return Codec::Into(value,output);});
 Check(!encoded.ok()&&encoded.bytes.empty()&&output==before&&!NoHeap([&]{return Codec::Size(value);}),"invalid record leaves output unchanged");
 if(wire){const auto raw=Golden(value);Check(!NoHeap([&]{return Codec::Decode(raw);}).ok(),"invalid record bytes refused");}}
template<class Codec,class R> void Framing(const R& value){const auto raw=Golden(value);
 std::array<p::byte,Codec::bytes> out;out.fill(0xab);const auto before=out;
 for(std::size_t n=0;n<raw.size();++n){Check(!NoHeap([&]{return Codec::Decode(std::string_view(raw).substr(0,n));}).ok(),"every truncated wire refuses");
   const auto encoded=NoHeap([&]{return Codec::Into(value,std::span(out).first(n));});
   Check(!encoded.ok()&&encoded.bytes.empty()&&out==before,"short output never writes a successful prefix");}
 for(const unsigned offset:{0u,4u,6u,8u,12u,16u,20u,24u,26u,28u}){auto invalid=raw;invalid[offset]^=0x7f;
   Check(!NoHeap([&]{return Codec::Decode(invalid);}).ok(),"invalid framing/schema/type/tag refused");}
 auto trailing=raw;trailing+='x';Check(!Codec::Decode(trailing).ok(),"trailing bytes refused");
 auto overlap=value;Check(NoHeap([&]{return Codec::Into(overlap,{reinterpret_cast<p::byte*>(&overlap),sizeof(overlap)});}).error==
   c::CatalogValueError::invalid_backing&&Golden(overlap)==Golden(value),"encoding cannot overwrite its input object");}
void Values(){
 Roundtrip<DatatypeCodec>(DatatypeValue());Roundtrip<DomainCodec>(DomainValue());
 Check(!c::DecodeCatalogSystemDomain(Golden(DatatypeValue())).ok()&&!c::DecodeCatalogBuiltinDatatypeDescriptor(Golden(DomainValue())).ok(),
   "the two families are not interchangeable");
 Framing<DatatypeCodec>(DatatypeValue());Framing<DomainCodec>(DomainValue());
 for(unsigned n=0;n<6;++n){auto r=DatatypeValue();switch(n){
   case 0:r.generation=0;break;case 1:r.origin_local_transaction_id=0;break;case 2:r.creating_local_transaction_id=2;break;
   case 3:r.creating_transaction_uuid.value=f::Id(8);break;case 4:r.origin_transaction_uuid.kind=p::UuidKind::object;break;
   case 5:r.generation=2;r.origin_local_transaction_id=2;r.creating_transaction_uuid.value=f::Id(8);r.creating_local_transaction_id=1;break;}
   Refuse<DatatypeCodec>(r,n!=4);}
 {auto r=DatatypeValue();r.generation=2;r.creating_transaction_uuid.value=f::Id(8);r.creating_local_transaction_id=2;Roundtrip<DatatypeCodec>(r);}
 for(unsigned n=0;n<7;++n){auto r=DatatypeValue();switch(n){
   case 0:r.descriptor_uuid={};break;case 1:r.database_uuid.bytes[6]=0x40;break;case 2:r.catalog_epoch=0;break;
   case 3:r.type_code=0;break;case 4:r.type_code=0x10000;break;case 5:r.flags=16;break;case 6:r.database_uuid=r.descriptor_uuid;break;}
   Refuse<DatatypeCodec>(r);}
 for(unsigned n=0;n<7;++n){auto r=DomainValue();switch(n){
   case 0:r.kind=c::SystemDomainKind(0);break;case 1:r.kind=c::SystemDomainKind(2);break;case 2:r.base_descriptor_generation=0;break;
   case 3:r.flags=2;break;case 4:r.base_descriptor_uuid=r.domain_uuid;break;case 5:r.base_descriptor_uuid={};break;
   case 6:r.generation=0;break;}
   Refuse<DomainCodec>(r);}
}
void Constants(){
 const auto a=c::SystemDomainUuid(0x66);
 Check(u::IsEngineIdentityUuid(a)&&a.bytes[0]==0x01&&a.bytes[1]==0x9d&&a.bytes[12]==0x0d&&a.bytes[14]==0x00&&a.bytes[15]==0x66,
   "system domain identity is the registered constant pattern 019d0000-0000-7000-8000-00000d00XXXX");
 Check(c::SystemDomainUuid(0x0102).bytes[14]==0x01&&c::SystemDomainUuid(0x0102).bytes[15]==0x02,"the type code fills the last two bytes");
 Check(c::SystemDomainUuid(0).is_nil()&&c::SystemDomainUuid(0x10000).is_nil(),"no constant outside the 16-bit code range");
}
c::CatalogMetadataVersion Metadata(const std::string& payload,c::CatalogRecordKind kind,const p::Uuid& object,scratchbird::core::catalog::CatalogObjectSubtype subtype,
    u64 generation,const p::TypedUuid& creating,u64 creating_local,unsigned n){
 auto m=f::Metadata(0,creating,f::Id(60),payload);m.record.header.kind=kind;
 m.record.header.object_uuid={p::UuidKind::object,object};m.record.header.row_uuid={p::UuidKind::row,f::Id(1000+3*n)};
 m.default_name_uuid={p::UuidKind::object,f::Id(2000+2*n)};m.name_vector_uuid={p::UuidKind::object,f::Id(2001+2*n)};
 m.object_subtype=subtype;m.creator_local_transaction_id=creating_local;m.definition_version=generation;return m;}
c::CatalogMetadataVersion DatatypeMetadata(const Datatype& r){return Metadata(Golden(r),c::CatalogRecordKind::datatype_descriptor,
  r.descriptor_uuid,c::kBuiltinDatatypeDescriptorSubtype,r.generation,r.creating_transaction_uuid,r.creating_local_transaction_id,0);}
c::CatalogMetadataVersion DomainMetadata(const Domain& r){return Metadata(Golden(r),c::CatalogRecordKind::domain,
  r.domain_uuid,c::kSystemDomainSubtype,r.generation,r.creating_transaction_uuid,r.creating_local_transaction_id,1);}
void Bindings(){
 for(const auto& m:{DatatypeMetadata(DatatypeValue()),DomainMetadata(DomainValue())}){
   Check(c::CatalogMetadataVersionEncodedBytes(c::BorrowCatalogMetadataVersion(m)).has_value(),"common dispatcher accepts exact metadata");
   Check(c::CatalogBuiltinTypeDomainMatchesDatabase(c::BorrowCatalogMetadataVersion(m),f::Id(1)),"actual database binding");
   Check(!c::CatalogBuiltinTypeDomainMatchesDatabase(c::BorrowCatalogMetadataVersion(m),f::Id(9)),"foreign database refused");
   Check(c::CatalogMetadataPreservesFamilyOrigin(m,m),"exact same-generation replay");
   for(unsigned n=0;n<7;++n){auto bad=m;switch(n){
     case 0:bad.record.header.object_uuid.value=f::Id(8);break;case 1:bad.record.header.row_uuid.value=f::Id(1);break;
     case 2:bad.definition_version=2;break;case 3:bad.creator_local_transaction_id=2;break;case 4:bad.object_subtype=scratchbird::core::catalog::CatalogObjectSubtype::schema_application;break;
     case 5:bad.record.header.kind=c::CatalogRecordKind::policy;break;case 6:bad.authority_scope=c::CatalogAuthorityScope::cluster;break;}
     Check(!c::CatalogMetadataVersionEncodedBytes(c::BorrowCatalogMetadataVersion(bad)),"header/creator/generation/subtype/kind/scope bound");}
 }
 auto swapped=DatatypeMetadata(DatatypeValue());swapped.object_subtype=c::kSystemDomainSubtype;
 Check(!c::CatalogMetadataVersionEncodedBytes(c::BorrowCatalogMetadataVersion(swapped)),"subtype and payload family agree");
 Check(c::CatalogBuiltinTypeDomainMatchesDatabase(c::BorrowCatalogMetadataVersion(f::Metadata(0,tx,f::Id(60),"")),f::Id(9)),
   "records outside the family pass the family's database check");
 const auto m=DomainMetadata(DomainValue());
 auto changed=DomainValue();changed.flags=0;
 Check(!c::CatalogMetadataPreservesFamilyOrigin(m,DomainMetadata(changed)),"same-generation content immutable");
 changed.generation=2;changed.creating_transaction_uuid.value=f::Id(8);changed.creating_local_transaction_id=2;
 Check(c::CatalogMetadataPreservesFamilyOrigin(m,DomainMetadata(changed)),"new generation retains original provenance");
 changed.origin_transaction_uuid=changed.creating_transaction_uuid;changed.origin_local_transaction_id=2;
 Check(!c::CatalogMetadataPreservesFamilyOrigin(m,DomainMetadata(changed)),"new generation cannot replace origin");
 Check(!c::CatalogMetadataPreservesFamilyOrigin(DatatypeMetadata(DatatypeValue()),m),"a datatype row cannot become a domain row");
 const auto domain=DomainValue();std::vector<p::Uuid> reserved;
 db::ReserveNativeCreationCatalogReferences({m,{}},[&](const auto& id){reserved.push_back(id);});
 for(const auto& id:{domain.domain_uuid,domain.database_uuid,domain.base_descriptor_uuid,domain.origin_transaction_uuid.value})
   Check(std::find(reserved.begin(),reserved.end(),id)!=reserved.end(),"native issuer excludes every domain reference");
}
// One row of each family per actual built-in datatype, from the real
// descriptor list and the datatype catalog manifest.
void Builtins(){
 const auto& builtins=dt::BuiltinDatatypeDescriptors();
 const auto manifest=dt::LoadCurrentCoreDatatypeCatalogManifest();
 Check(manifest.ok()&&manifest.manifest.descriptor_rows.size()==builtins.size()&&builtins.size()==85,
   "85 built-in datatypes, each with a manifest descriptor row");
 std::set<p::Uuid> descriptors,domains;
 for(std::size_t i=0;i<builtins.size();++i){
   const auto& b=builtins[i];const auto& row=manifest.manifest.descriptor_rows[i];
   Check(row.type_id==b.type_id,"manifest rows follow the built-in order");
   Datatype d;d.descriptor_uuid=row.descriptor_uuid.value;d.database_uuid=f::Id(1);d.generation=row.descriptor_epoch;
   d.catalog_epoch=manifest.manifest.catalog_epoch;d.type_code=u64(b.type_id);d.family_code=u64(b.family);
   d.width_class_code=u64(b.width_class);d.bit_width=b.bit_width;d.default_precision=b.default_precision;d.default_scale=b.default_scale;
   d.flags=(b.nullable_allowed?u64(c::BuiltinDatatypeFlag::nullable_allowed):0)|
       (b.descriptor_authoritative?u64(c::BuiltinDatatypeFlag::descriptor_authoritative):0)|
       (b.reference_name_is_alias_only?u64(c::BuiltinDatatypeFlag::reference_name_is_alias_only):0)|
       (b.requires_mandatory_library?u64(c::BuiltinDatatypeFlag::requires_mandatory_library):0);
   d.origin_transaction_uuid=d.creating_transaction_uuid=tx;d.origin_local_transaction_id=d.creating_local_transaction_id=1;
   Domain s;s.domain_uuid=c::SystemDomainUuid(static_cast<p::u32>(b.type_id));s.database_uuid=f::Id(1);s.generation=1;
   s.kind=c::SystemDomainKind::system_domain;s.base_descriptor_uuid=d.descriptor_uuid;s.base_descriptor_generation=d.generation;
   s.flags=u64(c::SystemDomainFlag::descriptor_authoritative);
   s.origin_transaction_uuid=s.creating_transaction_uuid=tx;s.origin_local_transaction_id=s.creating_local_transaction_id=1;
   Roundtrip<DatatypeCodec>(d);Roundtrip<DomainCodec>(s);
   descriptors.insert(d.descriptor_uuid);domains.insert(s.domain_uuid);
 }
 Check(descriptors.size()==85&&domains.size()==85,"every built-in has a distinct descriptor and a distinct domain identity");
 for(const auto& id:domains)Check(!descriptors.count(id),"no system domain identity equals a datatype descriptor identity");
}
} // namespace
int main(){try{Values();Constants();Bindings();Builtins();
 std::cout<<checks<<" built-in datatype and system domain row checks passed (codec and binding only)\n";return 0;}
 catch(const std::exception& e){allocation_budget=-1;std::cerr<<e.what()<<'\n';return 1;}}
