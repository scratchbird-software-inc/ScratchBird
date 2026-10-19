// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "catalog_verification_policy.hpp"
#include "uuid.hpp"
#include <array>
#include <limits>

namespace scratchbird::core::catalog {
namespace {
using T=CatalogValueType;using E=CatalogValueError;using R=VerificationPolicyRole;
constexpr CatalogValueFieldSchema fields[]{
  {1,T::engine_identity,true,16,UuidKind::object},{2,T::engine_identity,true,16,UuidKind::database},
  {3,T::unsigned_integer,true,8},{4,T::unsigned_integer,true,8},{5,T::unsigned_integer,true,8},
  {6,T::unsigned_integer,true,8},{7,T::unsigned_integer,true,8},{8,T::unsigned_integer,true,8},
  {9,T::unsigned_integer,true,8},{10,T::unsigned_integer,true,8},{11,T::unsigned_integer,true,8},
  {12,T::unsigned_integer,true,8},{13,T::unsigned_integer,true,8},
  {14,T::engine_identity,true,16,UuidKind::object},{15,T::engine_identity,false,16,UuidKind::object},
  {16,T::unsigned_integer,true,8},{17,T::engine_identity,true,16,UuidKind::transaction},
  {18,T::unsigned_integer,true,8},{19,T::engine_identity,true,16,UuidKind::transaction},
  {20,T::unsigned_integer,true,8}};
constexpr CatalogValueSchemaView schema{kCatalogVerificationPolicySchemaId,kCatalogVerificationPolicySchemaVersion,fields};
bool Identity(const Uuid& id){return uuid::IsEngineIdentityUuid(id);}
bool Transaction(const TypedUuid& id){return id.kind==UuidKind::transaction&&Identity(id.value);}
bool Valid(const CatalogVerificationPolicyBody& r) noexcept {
  if(!Identity(r.body_uuid)||!Identity(r.database_uuid)||!Identity(r.evidence_policy_uuid)||
      !Transaction(r.origin_transaction_uuid)||!Transaction(r.creating_transaction_uuid)||
      (r.access_policy_uuid&&!Identity(*r.access_policy_uuid))||!r.generation||!r.origin_local_transaction_id||
      r.creating_local_transaction_id<r.origin_local_transaction_id||
      ((r.origin_transaction_uuid.value==r.creating_transaction_uuid.value)!=
        (r.origin_local_transaction_id==r.creating_local_transaction_id))||
      (r.generation==1&&(r.origin_transaction_uuid.value!=r.creating_transaction_uuid.value||
        r.origin_local_transaction_id!=r.creating_local_transaction_id))||
      !r.verification_class_mask||(r.verification_class_mask&~u64{0x1ff})||
      !r.authority_class_mask||(r.authority_class_mask&~u64{0x3f})||
      !r.verification_level_mask||(r.verification_level_mask&~u64{0x3ff})||
      !r.allowed_action_mask||(r.allowed_action_mask&~u64{0x3ffff})||
      (r.review_action_mask&~r.allowed_action_mask)||!r.maximum_scan_pages||r.maximum_work_bytes<4096||!r.maximum_findings||
      (r.flags&~kVerificationPolicyFlagMask)||(r.flags&kVerificationPolicyMandatoryFlags)!=kVerificationPolicyMandatoryFlags||
      u64(r.uncertain_action)<16||u64(r.uncertain_action)>18||
      !(r.allowed_action_mask&VerificationActionBit(r.uncertain_action)))return false;
  const std::array<Uuid,4> ids{r.body_uuid,r.database_uuid,r.evidence_policy_uuid,r.access_policy_uuid.value_or(Uuid{})};
  const auto count=r.access_policy_uuid?4u:3u;
  for(unsigned i=0;i<count;++i){
    if(ids[i]==r.origin_transaction_uuid.value||ids[i]==r.creating_transaction_uuid.value)return false;
    for(unsigned j=0;j<i;++j)if(ids[i]==ids[j])return false;
  }
  u64 allowed=kVerificationNonmutatingActions;
  switch(r.role){
    case R::repair:allowed|=0x3fe;break;
    case R::quarantine:allowed|=0x3800;break;
    case R::salvage:allowed|=0x4000;break;
    default:return false;
  }
  if((r.allowed_action_mask&~allowed)||
      (r.role==R::quarantine)!=r.access_policy_uuid.has_value()||
      (r.role==R::salvage)!=bool(r.flags&kVerificationPolicyMarkSalvagedOutput)||
      (r.role!=R::salvage&&(r.flags&kVerificationPolicyAllowUnknownScan))||
      (!(r.flags&kVerificationPolicyAllowMutation)&&(r.allowed_action_mask&~kVerificationNonmutatingActions))||
      ((r.flags&kVerificationPolicyAllowOnlineMutation)&&(!(r.flags&kVerificationPolicyAllowMutation)||r.role!=R::repair)))return false;
  return true;
}
struct Backing {
  std::array<CatalogValueFieldView,20> views{};
  std::array<std::array<byte,8>,20> numbers{};
  std::size_t count=0;
  void Id(u16 tag,const Uuid& id,UuidKind kind){views[count++]={tag,T::engine_identity,kind,id.bytes};}
  void Number(u16 tag,u64 number){auto& bytes=numbers[tag-1];platform::StoreLittle64(bytes.data(),number);
    views[count++]={tag,T::unsigned_integer,UuidKind::unknown,bytes};}
  explicit Backing(const CatalogVerificationPolicyBody& r){
    Id(1,r.body_uuid,UuidKind::object);Id(2,r.database_uuid,UuidKind::database);Number(3,r.generation);Number(4,u64(r.role));
    Number(5,r.verification_class_mask);Number(6,r.authority_class_mask);Number(7,r.verification_level_mask);
    Number(8,r.allowed_action_mask);Number(9,r.review_action_mask);Number(10,u64(r.uncertain_action));
    Number(11,r.maximum_scan_pages);Number(12,r.maximum_work_bytes);Number(13,r.maximum_findings);
    Id(14,r.evidence_policy_uuid,UuidKind::object);if(r.access_policy_uuid)Id(15,*r.access_policy_uuid,UuidKind::object);
    Number(16,r.flags);Id(17,r.origin_transaction_uuid.value,UuidKind::transaction);Number(18,r.origin_local_transaction_id);
    Id(19,r.creating_transaction_uuid.value,UuidKind::transaction);Number(20,r.creating_local_transaction_id);
  }
  auto View()const noexcept{return std::span<const CatalogValueFieldView>(views).first(count);}
};
bool Header(const CatalogTypedRecordView& m,const CatalogVerificationPolicyBody& r){
  for(const auto& id:{r.body_uuid,r.database_uuid,r.evidence_policy_uuid,r.access_policy_uuid.value_or(Uuid{}),
      r.origin_transaction_uuid.value,r.creating_transaction_uuid.value})if(m.header.row_uuid.value==id)return false;
  return m.header.kind==CatalogRecordKind::policy&&m.header.object_uuid.kind==UuidKind::object&&
      m.header.object_uuid.value==r.body_uuid&&m.header.row_uuid.kind==UuidKind::row&&Identity(m.header.row_uuid.value)&&
      m.header.row_uuid.value!=r.body_uuid&&m.header.row_uuid.value!=r.database_uuid;
}
bool Metadata(const CatalogMetadataVersionView& m,const CatalogVerificationPolicyBody& r){
  return Header(m.record,r)&&m.object_subtype=="verification_policy_body"&&m.definition_version==r.generation&&
      m.creator_transaction_uuid.kind==UuidKind::transaction&&m.creator_transaction_uuid.value==r.creating_transaction_uuid.value&&
      m.creator_local_transaction_id==r.creating_local_transaction_id;
}
bool Family(const CatalogMetadataVersionView& m){return m.object_subtype=="verification_policy_body"||IsCatalogVerificationPolicyPayload(m.record.payload);}
}
std::optional<std::size_t> CatalogVerificationPolicyEncodedBytes(const CatalogVerificationPolicyBody& r) noexcept {
  if(!Valid(r))return {};const Backing b(r);if(ValidateCatalogValueFields(schema,b.View())!=E::none)return {};
  std::size_t bytes=kCatalogValueBlockHeaderBytes;for(const auto& field:b.View())bytes+=8+field.bytes.size();return bytes;
}
CatalogValueEncodeIntoResult EncodeCatalogVerificationPolicyInto(const CatalogVerificationPolicyBody& r,std::span<byte> output) noexcept {
  if(!Valid(r))return {E::invalid_value,{}};
  const auto dest=reinterpret_cast<std::uintptr_t>(output.data()),source=reinterpret_cast<std::uintptr_t>(&r);
  if((output.size()&&!output.data())||output.size()>std::numeric_limits<std::uintptr_t>::max()-dest||
      (dest<source+sizeof(r)&&source<dest+output.size()))return {E::invalid_backing,{}};
  const Backing b(r);return EncodeCatalogValueBlockInto(schema,b.View(),output);
}
CatalogVerificationPolicyResult DecodeCatalogVerificationPolicy(std::string_view bytes) noexcept {
  std::array<CatalogValueFieldView,20> backing;
  const auto parsed=DecodeCatalogValueBlockInto(schema,{reinterpret_cast<const byte*>(bytes.data()),bytes.size()},backing);
  if(!parsed.ok())return {parsed.error,{}};CatalogVerificationPolicyBody r;
  for(const auto& f:parsed.fields)switch(f.id){
    case 1:r.body_uuid=f.identity()->value;break;case 2:r.database_uuid=f.identity()->value;break;
    case 3:r.generation=*f.unsigned_value();break;case 4:r.role=R(*f.unsigned_value());break;
    case 5:r.verification_class_mask=*f.unsigned_value();break;case 6:r.authority_class_mask=*f.unsigned_value();break;
    case 7:r.verification_level_mask=*f.unsigned_value();break;case 8:r.allowed_action_mask=*f.unsigned_value();break;
    case 9:r.review_action_mask=*f.unsigned_value();break;case 10:r.uncertain_action=VerificationRepairAction(*f.unsigned_value());break;
    case 11:r.maximum_scan_pages=*f.unsigned_value();break;case 12:r.maximum_work_bytes=*f.unsigned_value();break;
    case 13:r.maximum_findings=*f.unsigned_value();break;case 14:r.evidence_policy_uuid=f.identity()->value;break;
    case 15:r.access_policy_uuid=f.identity()->value;break;case 16:r.flags=*f.unsigned_value();break;
    case 17:r.origin_transaction_uuid=*f.identity();break;case 18:r.origin_local_transaction_id=*f.unsigned_value();break;
    case 19:r.creating_transaction_uuid=*f.identity();break;case 20:r.creating_local_transaction_id=*f.unsigned_value();break;
  }
  if(!Valid(r))return {E::invalid_value,{}};return {E::none,r};
}
bool IsCatalogVerificationPolicyPayload(std::string_view bytes) noexcept {
  return bytes.size()>=kCatalogValueBlockHeaderBytes&&bytes.substr(0,4)=="SBCV"&&
    platform::LoadLittle32(reinterpret_cast<const byte*>(bytes.data())+16)==kCatalogVerificationPolicySchemaId;
}
bool CatalogVerificationPolicyMatchesHeader(const CatalogTypedRecordView& m) noexcept {const auto r=DecodeCatalogVerificationPolicy(m.payload);return r.ok()&&Header(m,*r.record);}
bool CatalogVerificationPolicyMatchesMetadata(const CatalogMetadataVersionView& m) noexcept {const auto r=DecodeCatalogVerificationPolicy(m.record.payload);return r.ok()&&Metadata(m,*r.record);}
bool CatalogVerificationPolicyMatchesDatabase(const CatalogMetadataVersionView& m,const Uuid& db) noexcept {
  if(!Family(m))return true;const auto r=DecodeCatalogVerificationPolicy(m.record.payload);return Identity(db)&&r.ok()&&Metadata(m,*r.record)&&r.record->database_uuid==db;
}
bool CatalogVerificationPolicyPreservesOrigin(const CatalogMetadataVersionView& a,const CatalogMetadataVersionView& b) noexcept {
  if(!Family(a)&&!Family(b))return true;const auto x=DecodeCatalogVerificationPolicy(a.record.payload),y=DecodeCatalogVerificationPolicy(b.record.payload);
  if(!x.ok()||!y.ok()||!Metadata(a,*x.record)||!Metadata(b,*y.record))return false;
  const auto& p=*x.record;const auto& q=*y.record;
  return p.body_uuid==q.body_uuid&&p.database_uuid==q.database_uuid&&p.role==q.role&&
      p.origin_transaction_uuid.value==q.origin_transaction_uuid.value&&p.origin_local_transaction_id==q.origin_local_transaction_id&&
      ((q.generation==p.generation&&a.record.payload==b.record.payload)||q.generation>p.generation);
}
bool CatalogVerificationPolicyMatchesDescriptor(const CatalogVerificationPolicyBody& body,
    const CatalogPolicyDescriptor& descriptor,VerificationPolicyRole expected_role) noexcept {
  return Valid(body)&&CatalogPolicyDescriptorEncodedBytes(descriptor).has_value()&&
      descriptor.policy_class==CatalogPolicyClass::verification_repair&&body.role==expected_role&&
      descriptor.body_uuid==body.body_uuid&&descriptor.body_generation==body.generation&&descriptor.database_uuid==body.database_uuid;
}
} // namespace scratchbird::core::catalog
