// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "catalog_evidence_policy.hpp"
#include "uuid.hpp"
#include <algorithm>
#include <cstring>
#include <limits>

namespace scratchbird::core::catalog {
namespace {
using T=CatalogValueType;using E=CatalogValueError;
constexpr std::size_t set_max_bytes=4+64*(4+128);
constexpr CatalogValueFieldSchema fields[]{
  {1,T::engine_identity,true,16,UuidKind::object},{2,T::engine_identity,true,16,UuidKind::database},
  {3,T::unsigned_integer,true,8},{4,T::utf8_text_list,true,set_max_bytes},{5,T::utf8_text_list,true,set_max_bytes},
  {6,T::engine_identity,true,16,UuidKind::filespace},
  {7,T::unsigned_integer,true,8},{8,T::unsigned_integer,true,8},{9,T::unsigned_integer,true,8},
  {10,T::unsigned_integer,true,8},{11,T::unsigned_integer,true,8},{12,T::unsigned_integer,true,8},
  {13,T::engine_identity,false,16,UuidKind::cluster},
  {14,T::unsigned_integer,true,8},{15,T::unsigned_integer,true,8},{16,T::unsigned_integer,true,8},
  {17,T::engine_identity,true,16,UuidKind::transaction},{18,T::unsigned_integer,true,8},
  {19,T::engine_identity,true,16,UuidKind::transaction},{20,T::unsigned_integer,true,8}};
constexpr CatalogValueSchemaView schema{kCatalogEvidencePolicySchemaId,1,fields};
bool Identity(const Uuid& id){return uuid::IsEngineIdentityUuid(id);}
bool Transaction(const TypedUuid& id){return id.kind==UuidKind::transaction&&Identity(id.value);}
bool Region(const void* data,std::size_t size){return (!size||data)&&size<=std::numeric_limits<std::uintptr_t>::max()-reinterpret_cast<std::uintptr_t>(data);}
bool Utf8(std::string_view text){
  if(text.empty()||text.size()>128||!Region(text.data(),text.size()))return false;
  for(std::size_t at=0;at<text.size();){const auto first=static_cast<byte>(text[at++]);u32 value=first;unsigned extra=0;
    if(!first)return false;
    if(first>=0xc2&&first<=0xdf){value=first&31;extra=1;}
    else if(first>=0xe0&&first<=0xef){value=first&15;extra=2;}
    else if(first>=0xf0&&first<=0xf4){value=first&7;extra=3;}
    else if(first>=0x80)return false;
    if(extra>text.size()-at)return false;
    for(unsigned i=0;i<extra;++i){const auto next=static_cast<byte>(text[at++]);if((next&0xc0)!=0x80)return false;value=(value<<6)|(next&63);}
    if((extra==1&&value<0x80)||(extra==2&&value<0x800)||(extra==3&&value<0x10000)||
        value>0x10ffff||(value>=0xd800&&value<=0xdfff))return false;
  }
  return true;
}
bool Before(std::string_view a,std::string_view b){return std::lexicographical_compare(a.begin(),a.end(),b.begin(),b.end(),
    [](char x,char y){return static_cast<byte>(x)<static_cast<byte>(y);});}
bool ValidSet(const CatalogEvidenceStringSet& set){
  if(!set.count||set.count>set.entries.size())return false;
  for(std::size_t i=0;i<set.count;++i)if(!Utf8(set.entries[i])||(i&&!Before(set.entries[i-1],set.entries[i])))return false;
  return true;
}
bool Valid(const CatalogEvidencePolicyBody& r){
  if(!Identity(r.body_uuid)||!Identity(r.database_uuid)||!Identity(r.storage_filespace_uuid)||
      (r.cluster_uuid&&!Identity(*r.cluster_uuid))||!Transaction(r.origin_transaction_uuid)||!Transaction(r.creating_transaction_uuid)||
      !r.generation||!r.origin_local_transaction_id||r.creating_local_transaction_id<r.origin_local_transaction_id||
      ((r.origin_transaction_uuid.value==r.creating_transaction_uuid.value)!=(r.origin_local_transaction_id==r.creating_local_transaction_id))||
      (r.generation==1&&r.creating_local_transaction_id!=r.origin_local_transaction_id)||
      u64(r.retention_class)<1||u64(r.retention_class)>10||u64(r.redaction_rule)<1||u64(r.redaction_rule)>6||
      u64(r.digest_rule)<1||u64(r.digest_rule)>2||u64(r.external_export_rule)<1||u64(r.external_export_rule)>2||
      u64(r.chain_of_custody_rule)<1||u64(r.chain_of_custody_rule)>2||
      u64(r.cluster_replication_rule)<1||u64(r.cluster_replication_rule)>3||
      (r.cluster_replication_rule!=EvidenceReplicationRule::standalone)!=r.cluster_uuid.has_value()||
      r.maximum_record_bytes<256||r.maximum_batch_bytes<r.maximum_record_bytes||
      (r.flags&~u64{31})||(r.flags&24)!=24||
      (r.redaction_rule==EvidenceRedactionRule::digest_only&&r.digest_rule!=EvidenceDigestRule::sha256)||
      (r.redaction_rule==EvidenceRedactionRule::external_reference&&
        (r.external_export_rule!=EvidenceExportRule::authorized_request_only||r.chain_of_custody_rule!=EvidenceCustodyRule::record_every_transfer))||
      !ValidSet(r.required_evidence_classes)||!ValidSet(r.required_search_keys))return false;
  const std::array ids{r.body_uuid,r.database_uuid,r.storage_filespace_uuid,r.cluster_uuid.value_or(Uuid{})};
  const auto count=r.cluster_uuid?4u:3u;
  for(unsigned i=0;i<count;++i){if(ids[i]==r.origin_transaction_uuid.value||ids[i]==r.creating_transaction_uuid.value)return false;
    for(unsigned j=0;j<i;++j)if(ids[i]==ids[j])return false;}
  return true;
}
struct Backing {
  std::array<CatalogValueFieldView,20> views{};
  std::array<std::array<byte,8>,20> numbers{};
  std::array<byte,set_max_bytes> classes{},keys{};
  std::size_t count=0;
  void Id(u16 tag,const Uuid& id,UuidKind kind){views[count++]={tag,T::engine_identity,kind,id.bytes};}
  void Number(u16 tag,u64 n){auto& bytes=numbers[tag-1];platform::StoreLittle64(bytes.data(),n);views[count++]={tag,T::unsigned_integer,UuidKind::unknown,bytes};}
  void Set(u16 tag,const CatalogEvidenceStringSet& s,std::span<byte> bytes){
    platform::StoreLittle32(bytes.data(),static_cast<u32>(s.count));std::size_t at=4;
    for(std::size_t i=0;i<s.count;++i){const auto text=s.entries[i];platform::StoreLittle32(bytes.data()+at,static_cast<u32>(text.size()));at+=4;
      std::memcpy(bytes.data()+at,text.data(),text.size());at+=text.size();}
    views[count++]={tag,T::utf8_text_list,UuidKind::unknown,bytes.first(at)};
  }
  explicit Backing(const CatalogEvidencePolicyBody& r){
    Id(1,r.body_uuid,UuidKind::object);Id(2,r.database_uuid,UuidKind::database);Number(3,r.generation);
    Set(4,r.required_evidence_classes,classes);Set(5,r.required_search_keys,keys);Id(6,r.storage_filespace_uuid,UuidKind::filespace);
    Number(7,u64(r.retention_class));Number(8,u64(r.redaction_rule));Number(9,u64(r.digest_rule));Number(10,u64(r.external_export_rule));
    Number(11,u64(r.chain_of_custody_rule));Number(12,u64(r.cluster_replication_rule));if(r.cluster_uuid)Id(13,*r.cluster_uuid,UuidKind::cluster);
    Number(14,r.maximum_record_bytes);Number(15,r.maximum_batch_bytes);Number(16,r.flags);
    Id(17,r.origin_transaction_uuid.value,UuidKind::transaction);Number(18,r.origin_local_transaction_id);
    Id(19,r.creating_transaction_uuid.value,UuidKind::transaction);Number(20,r.creating_local_transaction_id);
  }
  auto View()const{return std::span<const CatalogValueFieldView>(views).first(count);}
};
bool ReadSet(std::span<const byte> bytes,CatalogEvidenceStringSet& set){
  if(bytes.size()<4)return false;const auto count=platform::LoadLittle32(bytes.data());
  if(!count||count>set.entries.size())return false;std::size_t at=4;
  for(unsigned i=0;i<count;++i){if(bytes.size()-at<4)return false;const auto n=platform::LoadLittle32(bytes.data()+at);at+=4;
    if(n>bytes.size()-at)return false;set.entries[i]={reinterpret_cast<const char*>(bytes.data()+at),n};at+=n;}
  set.count=count;return at==bytes.size()&&ValidSet(set);
}
bool Header(const CatalogTypedRecordView& m,const CatalogEvidencePolicyBody& r){
  for(const auto& id:{r.body_uuid,r.database_uuid,r.storage_filespace_uuid,r.cluster_uuid.value_or(Uuid{}),
      r.origin_transaction_uuid.value,r.creating_transaction_uuid.value})if(m.header.row_uuid.value==id)return false;
  return m.header.kind==CatalogRecordKind::policy&&m.header.object_uuid.kind==UuidKind::object&&m.header.object_uuid.value==r.body_uuid&&
      m.header.row_uuid.kind==UuidKind::row&&Identity(m.header.row_uuid.value);
}
bool Metadata(const CatalogMetadataVersionView& m,const CatalogEvidencePolicyBody& r){
  return Header(m.record,r)&&m.object_subtype==CatalogObjectSubtype::evidence_policy_body&&m.definition_version==r.generation&&
      m.creator_transaction_uuid.kind==UuidKind::transaction&&m.creator_transaction_uuid.value==r.creating_transaction_uuid.value&&
      m.creator_local_transaction_id==r.creating_local_transaction_id&&
      (m.authority_scope==CatalogAuthorityScope::cluster)==r.cluster_uuid.has_value();
}
bool Family(const CatalogMetadataVersionView& m){return m.object_subtype==CatalogObjectSubtype::evidence_policy_body||IsCatalogEvidencePolicyPayload(m.record.payload);}
}
std::optional<std::size_t> CatalogEvidencePolicyEncodedBytes(const CatalogEvidencePolicyBody& r) noexcept {
  if(!Valid(r))return {};const Backing b(r);if(ValidateCatalogValueFields(schema,b.View())!=E::none)return {};
  std::size_t bytes=kCatalogValueBlockHeaderBytes;for(const auto& field:b.View())bytes+=8+field.bytes.size();return bytes;
}
CatalogValueEncodeIntoResult EncodeCatalogEvidencePolicyInto(const CatalogEvidencePolicyBody& r,std::span<byte> output) noexcept {
  if(!Valid(r))return {E::invalid_value,{}};
  if(!Region(output.data(),output.size()))return {E::invalid_backing,{}};
  const auto dest=reinterpret_cast<std::uintptr_t>(output.data());
  const auto overlap=[&](const void* data,std::size_t size){const auto at=reinterpret_cast<std::uintptr_t>(data);
    return size&&output.size()&&dest<at+size&&at<dest+output.size();};
  if(overlap(&r,sizeof(r)))return {E::invalid_backing,{}};
  for(const auto* set:{&r.required_evidence_classes,&r.required_search_keys})
    for(std::size_t i=0;i<set->count;++i)if(overlap(set->entries[i].data(),set->entries[i].size()))return {E::invalid_backing,{}};
  const Backing b(r);return EncodeCatalogValueBlockInto(schema,b.View(),output);
}
CatalogEvidencePolicyResult DecodeCatalogEvidencePolicy(std::string_view bytes) noexcept {
  std::array<CatalogValueFieldView,20> backing;
  const auto parsed=DecodeCatalogValueBlockInto(schema,{reinterpret_cast<const byte*>(bytes.data()),bytes.size()},backing);
  if(!parsed.ok())return {parsed.error,{}};CatalogEvidencePolicyBody r;
  for(const auto& f:parsed.fields)switch(f.id){
    case 1:r.body_uuid=f.identity()->value;break;case 2:r.database_uuid=f.identity()->value;break;case 3:r.generation=*f.unsigned_value();break;
    case 4:if(!ReadSet(f.bytes,r.required_evidence_classes))return {E::invalid_value,{}};break;
    case 5:if(!ReadSet(f.bytes,r.required_search_keys))return {E::invalid_value,{}};break;
    case 6:r.storage_filespace_uuid=f.identity()->value;break;case 7:r.retention_class=EvidenceRetentionClass(*f.unsigned_value());break;
    case 8:r.redaction_rule=EvidenceRedactionRule(*f.unsigned_value());break;case 9:r.digest_rule=EvidenceDigestRule(*f.unsigned_value());break;
    case 10:r.external_export_rule=EvidenceExportRule(*f.unsigned_value());break;case 11:r.chain_of_custody_rule=EvidenceCustodyRule(*f.unsigned_value());break;
    case 12:r.cluster_replication_rule=EvidenceReplicationRule(*f.unsigned_value());break;case 13:r.cluster_uuid=f.identity()->value;break;
    case 14:r.maximum_record_bytes=*f.unsigned_value();break;case 15:r.maximum_batch_bytes=*f.unsigned_value();break;case 16:r.flags=*f.unsigned_value();break;
    case 17:r.origin_transaction_uuid=*f.identity();break;case 18:r.origin_local_transaction_id=*f.unsigned_value();break;
    case 19:r.creating_transaction_uuid=*f.identity();break;case 20:r.creating_local_transaction_id=*f.unsigned_value();break;
  }
  if(!Valid(r))return {E::invalid_value,{}};return {E::none,r};
}
bool IsCatalogEvidencePolicyPayload(std::string_view bytes) noexcept {return bytes.size()>=kCatalogValueBlockHeaderBytes&&bytes.substr(0,4)=="SBCV"&&
    platform::LoadLittle32(reinterpret_cast<const byte*>(bytes.data())+16)==kCatalogEvidencePolicySchemaId;}
bool CatalogEvidencePolicyMatchesHeader(const CatalogTypedRecordView& m) noexcept {const auto r=DecodeCatalogEvidencePolicy(m.payload);return r.ok()&&Header(m,*r.record);}
bool CatalogEvidencePolicyMatchesMetadata(const CatalogMetadataVersionView& m) noexcept {const auto r=DecodeCatalogEvidencePolicy(m.record.payload);return r.ok()&&Metadata(m,*r.record);}
bool CatalogEvidencePolicyMatchesDatabase(const CatalogMetadataVersionView& m,const Uuid& db) noexcept {
  if(!Family(m))return true;const auto r=DecodeCatalogEvidencePolicy(m.record.payload);return Identity(db)&&r.ok()&&Metadata(m,*r.record)&&r.record->database_uuid==db;
}
bool CatalogEvidencePolicyPreservesOrigin(const CatalogMetadataVersionView& a,const CatalogMetadataVersionView& b) noexcept {
  if(!Family(a)&&!Family(b))return true;const auto x=DecodeCatalogEvidencePolicy(a.record.payload),y=DecodeCatalogEvidencePolicy(b.record.payload);
  if(!x.ok()||!y.ok()||!Metadata(a,*x.record)||!Metadata(b,*y.record))return false;const auto& p=*x.record;const auto& q=*y.record;
  return p.body_uuid==q.body_uuid&&p.database_uuid==q.database_uuid&&p.origin_transaction_uuid.value==q.origin_transaction_uuid.value&&
      p.origin_local_transaction_id==q.origin_local_transaction_id&&((p.generation==q.generation&&a.record.payload==b.record.payload)||q.generation>p.generation);
}
bool CatalogEvidencePolicyMatchesDescriptor(const CatalogEvidencePolicyBody& body,const CatalogPolicyDescriptor& descriptor) noexcept {
  return Valid(body)&&CatalogPolicyDescriptorEncodedBytes(descriptor).has_value()&&descriptor.policy_class==CatalogPolicyClass::evidence&&
      descriptor.database_uuid==body.database_uuid&&descriptor.body_uuid==body.body_uuid&&descriptor.body_generation==body.generation&&
      descriptor.cluster_uuid==body.cluster_uuid;
}
} // namespace scratchbird::core::catalog
