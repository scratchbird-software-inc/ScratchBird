// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "catalog_builtin_type_domain.hpp"
#include "uuid.hpp"
#include <array>
#include <limits>

namespace scratchbird::core::catalog {
namespace {
using T=CatalogValueType;using E=CatalogValueError;
using Datatype=CatalogBuiltinDatatypeDescriptor;using Domain=CatalogSystemDomain;
constexpr CatalogValueFieldSchema Id(u16 id,UuidKind kind){return {id,T::engine_identity,true,16,kind};}
constexpr CatalogValueFieldSchema Number(u16 id){return {id,T::unsigned_integer,true,8};}
constexpr auto O=UuidKind::object;constexpr auto D=UuidKind::database;constexpr auto X=UuidKind::transaction;
constexpr std::array datatype_fields{Id(1,O),Id(2,D),Number(3),Number(4),Number(5),Number(6),Number(7),Number(8),
    Number(9),Number(10),Number(11),Id(12,X),Number(13),Id(14,X),Number(15)};
constexpr std::array domain_fields{Id(1,O),Id(2,D),Number(3),Number(4),Id(5,O),Number(6),Number(7),Id(8,X),Number(9),
    Id(10,X),Number(11)};
constexpr CatalogValueSchemaView datatype_schema{kCatalogBuiltinDatatypeDescriptorSchemaId,1,datatype_fields};
constexpr CatalogValueSchemaView domain_schema{kCatalogSystemDomainSchemaId,1,domain_fields};
// Encoded sizes: header plus, per field, 8 bytes of framing and its value.
constexpr std::size_t kDatatypeBytes=kCatalogValueBlockHeaderBytes+15*8+4*16+11*8;
constexpr std::size_t kDomainBytes=kCatalogValueBlockHeaderBytes+11*8+5*16+6*8;

bool Identity(const Uuid& id){return uuid::IsEngineIdentityUuid(id);}
bool Transaction(const TypedUuid& id){return id.kind==UuidKind::transaction&&Identity(id.value);}
template<class R> bool Provenance(const R& r) noexcept {
  return r.generation&&Transaction(r.origin_transaction_uuid)&&Transaction(r.creating_transaction_uuid)&&
      r.origin_local_transaction_id&&r.creating_local_transaction_id>=r.origin_local_transaction_id&&
      ((r.origin_transaction_uuid.value==r.creating_transaction_uuid.value)==
          (r.origin_local_transaction_id==r.creating_local_transaction_id))&&
      (r.generation!=1||r.creating_local_transaction_id==r.origin_local_transaction_id);
}
// Every control identity is a real engine identity and pairwise distinct; the
// last two are the origin and creating transactions, which may be equal.
template<std::size_t N> bool Distinct(const std::array<Uuid,N>& control) noexcept {
  for(std::size_t i=0;i<N;++i){
    if(!Identity(control[i]))return false;
    for(std::size_t j=0;j<i;++j)if(control[i]==control[j]&&!(i==N-1&&j==N-2))return false;
  }
  return true;
}
auto Controls(const Datatype& r){return std::array{r.descriptor_uuid,r.database_uuid,r.origin_transaction_uuid.value,
    r.creating_transaction_uuid.value};}
auto Controls(const Domain& r){return std::array{r.domain_uuid,r.database_uuid,r.base_descriptor_uuid,
    r.origin_transaction_uuid.value,r.creating_transaction_uuid.value};}
bool Valid(const Datatype& r) noexcept {
  return Provenance(r)&&Distinct(Controls(r))&&r.catalog_epoch&&r.type_code&&r.type_code<=0xffff&&
      !(r.flags&~kBuiltinDatatypeFlagMask);
}
// A system domain's identity is fixed by its base datatype's code; the code is
// not stored, so the binding is checked where the base row is in hand.
bool Valid(const Domain& r) noexcept {
  return Provenance(r)&&Distinct(Controls(r))&&r.kind==SystemDomainKind::system_domain&&r.base_descriptor_generation&&
      !(r.flags&~kSystemDomainFlagMask);
}
template<std::size_t N> struct Backing {
  std::array<CatalogValueFieldView,N> views{};
  std::array<std::array<byte,8>,N> numbers{};
  std::size_t count=0;
  void Id(u16 tag,const Uuid& id,UuidKind kind){views[count++]={tag,T::engine_identity,kind,id.bytes};}
  void Number(u16 tag,u64 n){auto& bytes=numbers[tag-1];platform::StoreLittle64(bytes.data(),n);
    views[count++]={tag,T::unsigned_integer,UuidKind::unknown,bytes};}
  auto View()const noexcept{return std::span<const CatalogValueFieldView>{views}.first(count);}
};
Backing<15> Fields(const Datatype& r){Backing<15> b;
  b.Id(1,r.descriptor_uuid,O);b.Id(2,r.database_uuid,D);b.Number(3,r.generation);b.Number(4,r.catalog_epoch);
  b.Number(5,r.type_code);b.Number(6,r.family_code);b.Number(7,r.width_class_code);b.Number(8,r.bit_width);
  b.Number(9,r.default_precision);b.Number(10,r.default_scale);b.Number(11,r.flags);
  b.Id(12,r.origin_transaction_uuid.value,X);b.Number(13,r.origin_local_transaction_id);
  b.Id(14,r.creating_transaction_uuid.value,X);b.Number(15,r.creating_local_transaction_id);
  return b;}
Backing<11> Fields(const Domain& r){Backing<11> b;
  b.Id(1,r.domain_uuid,O);b.Id(2,r.database_uuid,D);b.Number(3,r.generation);b.Number(4,u64(r.kind));
  b.Id(5,r.base_descriptor_uuid,O);b.Number(6,r.base_descriptor_generation);b.Number(7,r.flags);
  b.Id(8,r.origin_transaction_uuid.value,X);b.Number(9,r.origin_local_transaction_id);
  b.Id(10,r.creating_transaction_uuid.value,X);b.Number(11,r.creating_local_transaction_id);
  return b;}
CatalogValueSchemaView Schema(const Datatype&){return datatype_schema;}
CatalogValueSchemaView Schema(const Domain&){return domain_schema;}
template<class R> std::optional<std::size_t> Size(const R& r) noexcept {
  if(!Valid(r))return {};const auto b=Fields(r);if(ValidateCatalogValueFields(Schema(r),b.View())!=E::none)return {};
  std::size_t bytes=kCatalogValueBlockHeaderBytes;for(const auto& field:b.View())bytes+=8+field.bytes.size();return bytes;
}
template<class R> CatalogValueEncodeIntoResult Into(const R& r,std::span<byte> output) noexcept {
  if(!Valid(r))return {E::invalid_value,{}};
  const auto dest=reinterpret_cast<std::uintptr_t>(output.data()),source=reinterpret_cast<std::uintptr_t>(&r);
  if((output.size()&&!output.data())||output.size()>std::numeric_limits<std::uintptr_t>::max()-dest||
      (dest<source+sizeof(r)&&source<dest+output.size()))return {E::invalid_backing,{}};
  const auto b=Fields(r);return EncodeCatalogValueBlockInto(Schema(r),b.View(),output);
}
template<class R,std::size_t Bytes> CatalogValueEncodeResult Owning(const R& r) {
  std::array<byte,Bytes> backing;const auto encoded=Into(r,backing);
  if(!encoded.ok())return {encoded.error,{}};
  return {E::none,{encoded.bytes.begin(),encoded.bytes.end()}};
}
template<std::size_t N> CatalogValueDecodeViewResult Parse(CatalogValueSchemaView schema,std::string_view bytes,
    std::array<CatalogValueFieldView,N>& backing) noexcept {
  return DecodeCatalogValueBlockInto(schema,{reinterpret_cast<const byte*>(bytes.data()),bytes.size()},backing);
}
u32 SchemaOf(std::string_view bytes) noexcept {
  if(bytes.size()<kCatalogValueBlockHeaderBytes||bytes.substr(0,4)!="SBCV")return 0;
  return platform::LoadLittle32(reinterpret_cast<const byte*>(bytes.data())+16);
}
struct Binding {
  Uuid object;CatalogRecordKind kind;std::string_view subtype;u64 generation=0;TypedUuid creating;u64 creating_local=0;
};
Binding Bind(const Datatype& r){return {r.descriptor_uuid,CatalogRecordKind::datatype_descriptor,kBuiltinDatatypeDescriptorSubtype,
    r.generation,r.creating_transaction_uuid,r.creating_local_transaction_id};}
Binding Bind(const Domain& r){return {r.domain_uuid,CatalogRecordKind::domain,kSystemDomainSubtype,
    r.generation,r.creating_transaction_uuid,r.creating_local_transaction_id};}
template<class R> bool Header(const CatalogTypedRecordView& m,const R& r) {
  const auto b=Bind(r);
  if(m.header.kind!=b.kind||m.header.object_uuid.kind!=UuidKind::object||m.header.object_uuid.value!=b.object||
      m.header.row_uuid.kind!=UuidKind::row||!Identity(m.header.row_uuid.value))return false;
  for(const auto& id:Controls(r))if(m.header.row_uuid.value==id)return false;
  return true;
}
template<class R> bool Metadata(const CatalogMetadataVersionView& m,const R& r) {
  const auto b=Bind(r);
  return Header(m.record,r)&&m.object_subtype==b.subtype&&m.definition_version==b.generation&&
      m.creator_transaction_uuid.kind==UuidKind::transaction&&m.creator_transaction_uuid.value==b.creating.value&&
      m.creator_local_transaction_id==b.creating_local&&m.authority_scope==CatalogAuthorityScope::local;
}
enum class Family { none,datatype,domain };
Family FamilyOfPayload(std::string_view payload) noexcept {
  switch(SchemaOf(payload)){
    case kCatalogBuiltinDatatypeDescriptorSchemaId:return Family::datatype;
    case kCatalogSystemDomainSchemaId:return Family::domain;
  }
  return Family::none;
}
Family FamilyOf(const CatalogMetadataVersionView& m) {
  if(m.object_subtype==kBuiltinDatatypeDescriptorSubtype)return Family::datatype;
  if(m.object_subtype==kSystemDomainSubtype)return Family::domain;
  return FamilyOfPayload(m.record.payload);
}
template<class Check> bool WithRecord(Family family,std::string_view payload,Check&& check) {
  switch(family){
    case Family::datatype:{const auto r=DecodeCatalogBuiltinDatatypeDescriptor(payload);return r.ok()&&check(*r.record);}
    case Family::domain:{const auto r=DecodeCatalogSystemDomain(payload);return r.ok()&&check(*r.record);}
    case Family::none:break;
  }
  return false;
}
Uuid DatabaseOf(const Datatype& r){return r.database_uuid;}
Uuid DatabaseOf(const Domain& r){return r.database_uuid;}
} // namespace

Uuid SystemDomainUuid(u32 code) noexcept {
  if(!code||code>0xffff)return {};
  return Uuid{{0x01,0x9d,0,0,0,0,0x70,0,0x80,0,0,0,0x0d,0,static_cast<byte>(code>>8),static_cast<byte>(code&0xff)}};
}
std::optional<std::size_t> CatalogBuiltinDatatypeDescriptorEncodedBytes(const Datatype& r) noexcept {return Size(r);}
CatalogValueEncodeIntoResult EncodeCatalogBuiltinDatatypeDescriptorInto(const Datatype& r,std::span<byte> out) noexcept {return Into(r,out);}
CatalogValueEncodeResult EncodeCatalogBuiltinDatatypeDescriptor(const Datatype& r) {return Owning<Datatype,kDatatypeBytes>(r);}
CatalogBuiltinTypeDomainResult<Datatype> DecodeCatalogBuiltinDatatypeDescriptor(std::string_view bytes) noexcept {
  std::array<CatalogValueFieldView,15> backing;const auto parsed=Parse(datatype_schema,bytes,backing);
  if(!parsed.ok())return {parsed.error,{}};Datatype r;
  for(const auto& f:parsed.fields)switch(f.id){
    case 1:r.descriptor_uuid=f.identity()->value;break;case 2:r.database_uuid=f.identity()->value;break;
    case 3:r.generation=*f.unsigned_value();break;case 4:r.catalog_epoch=*f.unsigned_value();break;
    case 5:r.type_code=*f.unsigned_value();break;case 6:r.family_code=*f.unsigned_value();break;
    case 7:r.width_class_code=*f.unsigned_value();break;case 8:r.bit_width=*f.unsigned_value();break;
    case 9:r.default_precision=*f.unsigned_value();break;case 10:r.default_scale=*f.unsigned_value();break;
    case 11:r.flags=*f.unsigned_value();break;case 12:r.origin_transaction_uuid=*f.identity();break;
    case 13:r.origin_local_transaction_id=*f.unsigned_value();break;case 14:r.creating_transaction_uuid=*f.identity();break;
    case 15:r.creating_local_transaction_id=*f.unsigned_value();break;
  }
  if(!Valid(r))return {E::invalid_value,{}};return {E::none,r};
}
std::optional<std::size_t> CatalogSystemDomainEncodedBytes(const Domain& r) noexcept {return Size(r);}
CatalogValueEncodeIntoResult EncodeCatalogSystemDomainInto(const Domain& r,std::span<byte> out) noexcept {return Into(r,out);}
CatalogValueEncodeResult EncodeCatalogSystemDomain(const Domain& r) {return Owning<Domain,kDomainBytes>(r);}
CatalogBuiltinTypeDomainResult<Domain> DecodeCatalogSystemDomain(std::string_view bytes) noexcept {
  std::array<CatalogValueFieldView,11> backing;const auto parsed=Parse(domain_schema,bytes,backing);
  if(!parsed.ok())return {parsed.error,{}};Domain r;
  for(const auto& f:parsed.fields)switch(f.id){
    case 1:r.domain_uuid=f.identity()->value;break;case 2:r.database_uuid=f.identity()->value;break;
    case 3:r.generation=*f.unsigned_value();break;case 4:r.kind=SystemDomainKind(*f.unsigned_value());break;
    case 5:r.base_descriptor_uuid=f.identity()->value;break;case 6:r.base_descriptor_generation=*f.unsigned_value();break;
    case 7:r.flags=*f.unsigned_value();break;case 8:r.origin_transaction_uuid=*f.identity();break;
    case 9:r.origin_local_transaction_id=*f.unsigned_value();break;case 10:r.creating_transaction_uuid=*f.identity();break;
    case 11:r.creating_local_transaction_id=*f.unsigned_value();break;
  }
  if(!Valid(r))return {E::invalid_value,{}};return {E::none,r};
}
bool IsCatalogBuiltinTypeDomainPayload(std::string_view bytes) noexcept {return FamilyOfPayload(bytes)!=Family::none;}
bool CatalogBuiltinTypeDomainMatchesHeader(const CatalogTypedRecordView& m) noexcept {
  return WithRecord(FamilyOfPayload(m.payload),m.payload,[&](const auto& r){return Header(m,r);});
}
bool CatalogBuiltinTypeDomainMatchesMetadata(const CatalogMetadataVersionView& m) noexcept {
  return WithRecord(FamilyOf(m),m.record.payload,[&](const auto& r){return Metadata(m,r);});
}
bool CatalogBuiltinTypeDomainMatchesDatabase(const CatalogMetadataVersionView& m,const Uuid& db) noexcept {
  const auto family=FamilyOf(m);if(family==Family::none)return true;
  return Identity(db)&&WithRecord(family,m.record.payload,[&](const auto& r){return Metadata(m,r)&&DatabaseOf(r)==db;});
}
bool CatalogBuiltinTypeDomainPreservesOrigin(const CatalogMetadataVersionView& a,const CatalogMetadataVersionView& b) noexcept {
  const auto x=FamilyOf(a),y=FamilyOf(b);
  if(x==Family::none&&y==Family::none)return true;
  if(x!=y)return false;
  return WithRecord(x,a.record.payload,[&](const auto& p){
    using R=std::decay_t<decltype(p)>;
    return WithRecord(y,b.record.payload,[&](const auto& q_any){
      if constexpr(!std::is_same_v<std::decay_t<decltype(q_any)>,R>)return false;
      else {
        const auto& q=q_any;
        return Metadata(a,p)&&Metadata(b,q)&&Bind(p).object==Bind(q).object&&DatabaseOf(p)==DatabaseOf(q)&&
            p.origin_transaction_uuid.value==q.origin_transaction_uuid.value&&
            p.origin_local_transaction_id==q.origin_local_transaction_id&&
            ((p.generation==q.generation&&a.record.payload==b.record.payload)||q.generation>p.generation);
      }
    });
  });
}
} // namespace scratchbird::core::catalog
