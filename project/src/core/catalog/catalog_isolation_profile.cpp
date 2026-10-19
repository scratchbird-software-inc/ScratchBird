// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "catalog_isolation_profile.hpp"
#include "uuid.hpp"
#include <array>
#include <limits>

namespace scratchbird::core::catalog {
namespace {
using T=CatalogValueType;using E=CatalogValueError;
constexpr auto Fields(){
  std::array<CatalogValueFieldSchema,22> out{};
  for(u16 i=0;i<out.size();++i)out[i]={u16(i+1),T::unsigned_integer,true,8};
  for(const u16 id:{1,11,12})out[id-1]={id,T::engine_identity,true,16,UuidKind::object};
  out[1]={2,T::engine_identity,true,16,UuidKind::database};
  out[16]={17,T::engine_identity,false,16,UuidKind::object};
  for(const u16 id:{18,20})out[id-1]={id,T::engine_identity,true,16,UuidKind::transaction};
  out[21]={22,T::engine_identity,false,16,UuidKind::cluster};return out;
}
constexpr auto fields=Fields();
constexpr CatalogValueSchemaView schema{kCatalogIsolationProfileSchemaId,1,fields};
bool Identity(const Uuid& id){return uuid::IsEngineIdentityUuid(id);}
bool Transaction(const TypedUuid& id){return id.kind==UuidKind::transaction&&Identity(id.value);}
template<class V>bool Range(V value,u64 maximum){return u64(value)>=1&&u64(value)<=maximum;}
auto Controls(const CatalogIsolationProfile& r){
  return std::array{r.profile_uuid,r.database_uuid,r.wait_policy_uuid,r.deadlock_policy_uuid,
      r.donor_mapping_uuid.value_or(Uuid{}),r.origin_transaction_uuid.value,r.creating_transaction_uuid.value,
      r.cluster_uuid.value_or(Uuid{})};
}
bool Valid(const CatalogIsolationProfile& r) noexcept {
  if(!r.generation||!Transaction(r.origin_transaction_uuid)||!Transaction(r.creating_transaction_uuid)||
      !r.origin_local_transaction_id||r.creating_local_transaction_id<r.origin_local_transaction_id||
      ((r.origin_transaction_uuid.value==r.creating_transaction_uuid.value)!=
          (r.origin_local_transaction_id==r.creating_local_transaction_id))||
      (r.generation==1&&r.creating_local_transaction_id!=r.origin_local_transaction_id)||
      !Range(r.profile_kind,11)||!Range(r.snapshot_construction_point,7)||
      !r.snapshot_refresh_mask||(r.snapshot_refresh_mask&~u64{127})||
      ((r.snapshot_refresh_mask&1)&&r.snapshot_refresh_mask!=1)||
      !Range(r.own_write_visibility,3)||!Range(r.active_other_visibility,3)||!Range(r.limbo_visibility,4)||
      !r.write_conflict_detection_mask||(r.write_conflict_detection_mask&~u64{31})||
      !Range(r.predicate_validation,4)||!Range(r.unique_validation,4)||
      !Range(r.metadata_visibility,5)||!Range(r.cluster_participant_mode,4))return false;
  const auto control=Controls(r);
  for(std::size_t i=0;i<control.size();++i){
    if((i==4&&!r.donor_mapping_uuid)||(i==7&&!r.cluster_uuid))continue;
    if(!Identity(control[i]))return false;
    for(std::size_t j=0;j<i;++j)if(control[i]==control[j]&&!(i==3&&j==2)&&!(i==6&&j==5))return false;
  }
  using K=IsolationProfileKind;using S=IsolationSnapshotPoint;using M=IsolationMetadataVisibility;
  const auto k=r.profile_kind;
  if((k==K::read_uncommitted_emulated||r.unique_validation==IsolationUniqueValidation::donor_profile)&&!r.donor_mapping_uuid)return false;
  if(r.active_other_visibility!=IsolationActiveOthers::never_visible&&
      k!=K::read_uncommitted_emulated&&k!=K::internal_recovery_isolation)return false;
  if(u64(k)>=2&&u64(k)<=7&&r.own_write_visibility!=IsolationOwnWrites::visible_until_savepoint_rollback)return false;
  if(k==K::read_committed_statement&&(r.snapshot_construction_point!=S::statement_start||
      r.snapshot_refresh_mask!=2||r.metadata_visibility!=M::statement_epoch))return false;
  if(k==K::read_committed_cursor_stable&&(r.snapshot_construction_point!=S::statement_start||
      (r.snapshot_refresh_mask!=2&&r.snapshot_refresh_mask!=10)||
      (r.metadata_visibility!=M::statement_epoch&&r.metadata_visibility!=M::cursor_epoch)))return false;
  if(u64(k)>=4&&u64(k)<=7&&(r.snapshot_construction_point!=S::transaction_start||
      r.snapshot_refresh_mask!=1||r.metadata_visibility!=M::transaction_epoch))return false;
  if((k==K::serializable_snapshot||k==K::serializable_validated)&&
      (r.predicate_validation==IsolationPredicateValidation::none||!(r.write_conflict_detection_mask&16)))return false;
  if(k==K::serializable_validated&&r.predicate_validation!=IsolationPredicateValidation::serializable_required)return false;
  if(k==K::read_only_historical_snapshot&&(r.snapshot_construction_point!=S::historical_boundary||
      r.snapshot_refresh_mask!=1||r.metadata_visibility!=M::historical_epoch))return false;
  if((u64(r.cluster_participant_mode)>=3||r.unique_validation==IsolationUniqueValidation::cluster_authority||
      r.metadata_visibility==M::cluster_epoch||(r.snapshot_refresh_mask&64))&&!r.cluster_uuid)return false;
  if(k==K::cluster_participant_isolation&&(!r.cluster_uuid||u64(r.cluster_participant_mode)<3||
      r.snapshot_construction_point!=S::cluster_participant_start||r.metadata_visibility!=M::cluster_epoch))return false;
  return true;
}
struct Backing {
  std::array<CatalogValueFieldView,22> views{};
  std::array<std::array<byte,8>,22> numbers{};
  std::size_t count=0;
  void Id(u16 tag,const Uuid& id,UuidKind kind){views[count++]={tag,T::engine_identity,kind,id.bytes};}
  void Number(u16 tag,u64 n){auto& bytes=numbers[tag-1];platform::StoreLittle64(bytes.data(),n);
    views[count++]={tag,T::unsigned_integer,UuidKind::unknown,bytes};}
  explicit Backing(const CatalogIsolationProfile& r){
    Id(1,r.profile_uuid,UuidKind::object);Id(2,r.database_uuid,UuidKind::database);Number(3,r.generation);
    Number(4,u64(r.profile_kind));Number(5,u64(r.snapshot_construction_point));Number(6,r.snapshot_refresh_mask);
    Number(7,u64(r.own_write_visibility));Number(8,u64(r.active_other_visibility));Number(9,u64(r.limbo_visibility));
    Number(10,r.write_conflict_detection_mask);Id(11,r.wait_policy_uuid,UuidKind::object);Id(12,r.deadlock_policy_uuid,UuidKind::object);
    Number(13,u64(r.predicate_validation));Number(14,u64(r.unique_validation));Number(15,u64(r.metadata_visibility));Number(16,u64(r.cluster_participant_mode));
    if(r.donor_mapping_uuid)Id(17,*r.donor_mapping_uuid,UuidKind::object);
    Id(18,r.origin_transaction_uuid.value,UuidKind::transaction);Number(19,r.origin_local_transaction_id);
    Id(20,r.creating_transaction_uuid.value,UuidKind::transaction);Number(21,r.creating_local_transaction_id);
    if(r.cluster_uuid)Id(22,*r.cluster_uuid,UuidKind::cluster);
  }
  auto View()const noexcept{return std::span<const CatalogValueFieldView>{views}.first(count);}
};
bool Header(const CatalogTypedRecordView& m,const CatalogIsolationProfile& r){
  if(m.header.kind!=CatalogRecordKind::policy||m.header.object_uuid.kind!=UuidKind::object||m.header.object_uuid.value!=r.profile_uuid||
      m.header.row_uuid.kind!=UuidKind::row||!Identity(m.header.row_uuid.value))return false;
  for(const auto& id:Controls(r))if(m.header.row_uuid.value==id)return false;return true;
}
bool Metadata(const CatalogMetadataVersionView& m,const CatalogIsolationProfile& r){
  return Header(m.record,r)&&m.object_subtype=="isolation_profile"&&m.definition_version==r.generation&&
      m.creator_transaction_uuid.kind==UuidKind::transaction&&m.creator_transaction_uuid.value==r.creating_transaction_uuid.value&&
      m.creator_local_transaction_id==r.creating_local_transaction_id&&
      (m.authority_scope==CatalogAuthorityScope::cluster)==r.cluster_uuid.has_value();
}
bool Family(const CatalogMetadataVersionView& m){return m.object_subtype=="isolation_profile"||IsCatalogIsolationProfilePayload(m.record.payload);}
}
std::optional<std::size_t> CatalogIsolationProfileEncodedBytes(const CatalogIsolationProfile& r) noexcept {
  if(!Valid(r))return {};const Backing b(r);if(ValidateCatalogValueFields(schema,b.View())!=E::none)return {};
  std::size_t bytes=kCatalogValueBlockHeaderBytes;for(const auto& field:b.View())bytes+=8+field.bytes.size();return bytes;
}
CatalogValueEncodeIntoResult EncodeCatalogIsolationProfileInto(const CatalogIsolationProfile& r,std::span<byte> output) noexcept {
  if(!Valid(r))return {E::invalid_value,{}};
  const auto dest=reinterpret_cast<std::uintptr_t>(output.data()),source=reinterpret_cast<std::uintptr_t>(&r);
  if((output.size()&&!output.data())||output.size()>std::numeric_limits<std::uintptr_t>::max()-dest||
      (dest<source+sizeof(r)&&source<dest+output.size()))return {E::invalid_backing,{}};
  const Backing b(r);return EncodeCatalogValueBlockInto(schema,b.View(),output);
}
CatalogIsolationProfileResult DecodeCatalogIsolationProfile(std::string_view bytes) noexcept {
  std::array<CatalogValueFieldView,22> backing;
  const auto parsed=DecodeCatalogValueBlockInto(schema,{reinterpret_cast<const byte*>(bytes.data()),bytes.size()},backing);
  if(!parsed.ok())return {parsed.error,{}};CatalogIsolationProfile r;
  for(const auto& f:parsed.fields)switch(f.id){
    case 1:r.profile_uuid=f.identity()->value;break;case 2:r.database_uuid=f.identity()->value;break;case 3:r.generation=*f.unsigned_value();break;
    case 4:r.profile_kind=IsolationProfileKind(*f.unsigned_value());break;case 5:r.snapshot_construction_point=IsolationSnapshotPoint(*f.unsigned_value());break;
    case 6:r.snapshot_refresh_mask=*f.unsigned_value();break;case 7:r.own_write_visibility=IsolationOwnWrites(*f.unsigned_value());break;
    case 8:r.active_other_visibility=IsolationActiveOthers(*f.unsigned_value());break;case 9:r.limbo_visibility=IsolationLimbo(*f.unsigned_value());break;
    case 10:r.write_conflict_detection_mask=*f.unsigned_value();break;case 11:r.wait_policy_uuid=f.identity()->value;break;case 12:r.deadlock_policy_uuid=f.identity()->value;break;
    case 13:r.predicate_validation=IsolationPredicateValidation(*f.unsigned_value());break;case 14:r.unique_validation=IsolationUniqueValidation(*f.unsigned_value());break;
    case 15:r.metadata_visibility=IsolationMetadataVisibility(*f.unsigned_value());break;case 16:r.cluster_participant_mode=IsolationClusterMode(*f.unsigned_value());break;
    case 17:r.donor_mapping_uuid=f.identity()->value;break;case 18:r.origin_transaction_uuid=*f.identity();break;
    case 19:r.origin_local_transaction_id=*f.unsigned_value();break;case 20:r.creating_transaction_uuid=*f.identity();break;
    case 21:r.creating_local_transaction_id=*f.unsigned_value();break;case 22:r.cluster_uuid=f.identity()->value;break;
  }
  if(!Valid(r))return {E::invalid_value,{}};return {E::none,r};
}
bool IsCatalogIsolationProfilePayload(std::string_view bytes) noexcept {
  return bytes.size()>=kCatalogValueBlockHeaderBytes&&bytes.substr(0,4)=="SBCV"&&
      platform::LoadLittle32(reinterpret_cast<const byte*>(bytes.data())+16)==kCatalogIsolationProfileSchemaId;
}
bool CatalogIsolationProfileMatchesHeader(const CatalogTypedRecordView& m) noexcept {const auto r=DecodeCatalogIsolationProfile(m.payload);return r.ok()&&Header(m,*r.record);}
bool CatalogIsolationProfileMatchesMetadata(const CatalogMetadataVersionView& m) noexcept {const auto r=DecodeCatalogIsolationProfile(m.record.payload);return r.ok()&&Metadata(m,*r.record);}
bool CatalogIsolationProfileMatchesDatabase(const CatalogMetadataVersionView& m,const Uuid& db) noexcept {
  if(!Family(m))return true;const auto r=DecodeCatalogIsolationProfile(m.record.payload);return Identity(db)&&r.ok()&&Metadata(m,*r.record)&&r.record->database_uuid==db;
}
bool CatalogIsolationProfilePreservesOrigin(const CatalogMetadataVersionView& a,const CatalogMetadataVersionView& b) noexcept {
  if(!Family(a)&&!Family(b))return true;const auto x=DecodeCatalogIsolationProfile(a.record.payload),y=DecodeCatalogIsolationProfile(b.record.payload);
  if(!x.ok()||!y.ok()||!Metadata(a,*x.record)||!Metadata(b,*y.record))return false;const auto& p=*x.record;const auto& q=*y.record;
  return p.profile_uuid==q.profile_uuid&&p.database_uuid==q.database_uuid&&p.origin_transaction_uuid.value==q.origin_transaction_uuid.value&&
      p.origin_local_transaction_id==q.origin_local_transaction_id&&((p.generation==q.generation&&a.record.payload==b.record.payload)||q.generation>p.generation);
}
} // namespace scratchbird::core::catalog
