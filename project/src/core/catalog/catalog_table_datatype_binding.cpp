// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "catalog_table_datatype_binding.hpp"
#include "uuid.hpp"
#include <algorithm>

namespace scratchbird::core::catalog {
namespace {
using E = CatalogIntervalBindingError;
using Profile = datatypes::IntervalValidatedProfileHandleV3;
using Control = datatypes::IntervalExecutionControlV3;
constexpr CatalogValueFieldSchema fields[]{
    {1,CatalogValueType::engine_identity,true,16,UuidKind::object},
    {2,CatalogValueType::unsigned_integer,true,8},
    {3,CatalogValueType::opaque_bytes,true,608},
    {4,CatalogValueType::opaque_bytes,true,32}};
template<class Result> Result Fail(E error) {
  Result out;out.error=error;return out;
}
bool ProfileValid(const Profile& profile,const Control& control,CatalogIntervalBindingFailure& out) {
  const auto result=datatypes::ValidateIntervalProfileHandleV3(profile,control);
  if(result.ok())return true;
  out.error=E::invalid_profile;out.datatype_diagnostic=result.diagnostic;return false;
}
bool OccurrenceValid(const CatalogDefinitionReference& ref,const Profile& profile) {
  const auto& id=profile.identity.legacy_fields;
  return ref.uuid.kind==UuidKind::object&&uuid::IsEngineIdentityUuid(ref.uuid.value)&&ref.generation&&
      ref.uuid.value!=id.descriptor_uuid&&ref.uuid.value!=id.type_uuid&&ref.uuid.value!=id.codec_uuid;
}
bool ProfileMatches(const CatalogIntervalOccurrence& d,const Profile& p) {
  return d.profile_material==p.profile_material&&d.profile_fingerprint==p.profile_fingerprint;
}
CatalogIntervalOccurrenceResult Decode(std::string_view bytes,const Profile& profile) {
  std::array<CatalogValueFieldView,4> backing;
  const auto parsed=DecodeCatalogValueBlockInto(CatalogIntervalOccurrenceSchemaView(),
      {reinterpret_cast<const byte*>(bytes.data()),bytes.size()},backing);
  if(!parsed.ok()) {
    auto out=Fail<CatalogIntervalOccurrenceResult>(E::invalid_payload);
    out.value_error=parsed.error;return out;
  }
  if(backing[2].bytes.size()!=608||backing[3].bytes.size()!=32) {
    auto out=Fail<CatalogIntervalOccurrenceResult>(E::invalid_payload);
    out.value_error=CatalogValueError::invalid_value;return out;
  }
  CatalogIntervalOccurrence d;
  d.occurrence={*backing[0].identity(),*backing[1].unsigned_value()};
  if(!OccurrenceValid(d.occurrence,profile))return Fail<CatalogIntervalOccurrenceResult>(E::invalid_occurrence);
  std::copy(backing[2].bytes.begin(),backing[2].bytes.end(),d.profile_material.begin());
  std::copy(backing[3].bytes.begin(),backing[3].bytes.end(),d.profile_fingerprint.begin());
  if(!ProfileMatches(d,profile))return Fail<CatalogIntervalOccurrenceResult>(E::profile_mismatch);
  CatalogIntervalOccurrenceResult out;out.definition=d;return out;
}
bool Absent(const CatalogDefinitionReference& r) {
  return r.uuid.kind==UuidKind::unknown&&r.uuid.value.is_nil()&&r.generation==0;
}
}  // namespace
CatalogValueSchemaView CatalogIntervalOccurrenceSchemaView() {return {65702,1,fields};}
CatalogIntervalOccurrenceEncodeResult EncodeCatalogIntervalOccurrence(
    const CatalogIntervalOccurrence& d,const Profile& profile,const Control& control) {
  CatalogIntervalOccurrenceEncodeResult out;
  if(!ProfileValid(profile,control,out))return out;
  if(!OccurrenceValid(d.occurrence,profile))return Fail<CatalogIntervalOccurrenceEncodeResult>(E::invalid_occurrence);
  if(!ProfileMatches(d,profile))return Fail<CatalogIntervalOccurrenceEncodeResult>(E::profile_mismatch);
  const auto schema=CatalogIntervalOccurrenceSchemaView();
  std::vector<CatalogValueField> values;
  values.reserve(4);
  values.push_back({1,d.occurrence.uuid});
  values.push_back({2,d.occurrence.generation});
  values.push_back({3,std::vector<byte>(d.profile_material.begin(),d.profile_material.end())});
  values.push_back({4,std::vector<byte>(d.profile_fingerprint.begin(),d.profile_fingerprint.end())});
  auto encoded=EncodeCatalogValueBlock({schema.id,schema.version,{schema.fields.begin(),schema.fields.end()}},values);
  if(!encoded.ok()){out.error=E::invalid_payload;out.value_error=encoded.error;return out;}
  out.bytes=std::move(encoded.bytes);return out;
}
CatalogIntervalOccurrenceResult DecodeCatalogIntervalOccurrence(
    std::string_view bytes,const Profile& profile,const Control& control) {
  CatalogIntervalOccurrenceResult out;
  if(!ProfileValid(profile,control,out))return out;
  return Decode(bytes,profile);
}
CatalogIntervalColumnBindingResult BindCatalogIntervalColumn(
    const CatalogMetadataVersionView& column,const CatalogMetadataVersionView& occurrence,
    const Profile& profile,const Control& control) {
  CatalogIntervalColumnBindingResult out;
  if(!ProfileValid(profile,control,out))return out;
  if(const auto error=ValidateCatalogMetadataVersionView(column)) {
    out.error=E::invalid_column;out.metadata_diagnostic=error;return out;
  }
  if(!CatalogColumnDefinitionMatchesMetadata(column))return Fail<CatalogIntervalColumnBindingResult>(E::invalid_column);
  if(const auto error=ValidateCatalogMetadataVersionView(occurrence)) {
    out.error=E::invalid_occurrence_metadata;out.metadata_diagnostic=error;return out;
  }
  if(occurrence.record.header.kind!=CatalogRecordKind::datatype_descriptor||
      occurrence.object_subtype!="persistent_interval_value_occurrence"||
      occurrence.authority_scope!=CatalogAuthorityScope::local||
      occurrence.owning_schema_uuid.kind!=UuidKind::schema||
      !uuid::IsEngineIdentityUuid(occurrence.owning_schema_uuid.value)||
      occurrence.record.header.parent_uuid.kind!=UuidKind::object||
      occurrence.record.header.parent_uuid.value!=occurrence.owning_schema_uuid.value)
    return Fail<CatalogIntervalColumnBindingResult>(E::invalid_occurrence_metadata);
  const auto decoded=Decode(occurrence.record.payload,profile);
  if(!decoded.ok()) {static_cast<CatalogIntervalBindingFailure&>(out)=decoded;return out;}
  const auto& d=*decoded.definition;
  if(occurrence.record.header.object_uuid.kind!=UuidKind::object||
      occurrence.record.header.object_uuid.value!=d.occurrence.uuid.value||
      occurrence.definition_version!=d.occurrence.generation)
    return Fail<CatalogIntervalColumnBindingResult>(E::invalid_occurrence_metadata);
  const auto c=DecodeCatalogColumnDefinition(column.record.payload);
  const auto& v=*c.definition;const auto& id=profile.identity.legacy_fields;
  if(v.value_descriptor.uuid.value!=d.occurrence.uuid.value||v.value_descriptor.generation!=d.occurrence.generation||
      v.datatype_descriptor.uuid.value!=id.descriptor_uuid||v.datatype_descriptor.generation!=id.descriptor_generation||
      v.type_uuid.value!=id.type_uuid||column.owning_schema_uuid.value!=occurrence.owning_schema_uuid.value||
      !Absent(v.charset)||!Absent(v.collation))
    return Fail<CatalogIntervalColumnBindingResult>(E::column_binding_mismatch);
  out.binding=CatalogIntervalColumnBinding{v,d};return out;
}
}  // namespace scratchbird::core::catalog
