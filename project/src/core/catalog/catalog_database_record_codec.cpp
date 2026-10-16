// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "catalog_database_record_codec.hpp"
#include "../datatypes/admitted_datatype_cohort.hpp"
#include "uuid.hpp"
#include <array>
#include <cstdint>
#include <limits>
namespace scratchbird::core::catalog {
namespace {
using T = CatalogValueType;
constexpr std::array<CatalogValueFieldSchema, 21> kDatabaseFields{{
    {1,T::engine_identity,true,16,UuidKind::database},
    {2,T::unsigned_integer,true,8}, {3,T::unsigned_integer,true,8},
    {4,T::unsigned_integer,true,8}, {5,T::unsigned_integer,true,8},
    {6,T::unsigned_integer,true,8}, {7,T::unsigned_integer,true,8},
    {8,T::unsigned_integer,true,8}, {9,T::unsigned_integer,true,8},
    {10,T::engine_identity,true,16,UuidKind::object},
    {11,T::unsigned_integer,true,8}, {12,T::unsigned_integer,true,8},
    {13,T::engine_identity,true,16,UuidKind::object},
    {14,T::engine_identity,true,16,UuidKind::object},
    {15,T::unsigned_integer,true,8}, {16,T::unsigned_integer,true,8},
    {17,T::unsigned_integer,true,8}, {18,T::unsigned_integer,true,8},
    {19,T::engine_identity,true,16,UuidKind::object},
    {20,T::unsigned_integer,true,8},
    {21,T::engine_identity,false,16,UuidKind::cluster}}};
constexpr CatalogValueSchemaView kDatabaseSchemaV1{
    65537, 1, std::span(kDatabaseFields).first<9>()};
constexpr CatalogValueSchemaView kDatabaseSchemaV2{65537, 2, std::span(kDatabaseFields).first<12>()};
constexpr CatalogValueSchemaView kDatabaseSchemaV3{65537, 3, kDatabaseFields};
bool ValidTextDefaults(const CatalogDatabaseTextDefaults& d) {
  const auto object=[](const TypedUuid& id) {
    return id.kind==UuidKind::object&&uuid::IsEngineIdentityUuid(id.value);
  };
  if(!object(d.charset_uuid)||!object(d.collation_uuid)||!object(d.source_object_uuid)||
      d.charset_uuid.value==d.collation_uuid.value||d.charset_uuid.value==d.source_object_uuid.value||
      d.collation_uuid.value==d.source_object_uuid.value||!d.resource_activation_epoch||
      !d.charset_family_epoch||!d.collation_family_epoch||!d.source_generation)return false;
  switch(d.source) {
    case CatalogDatabaseTextDefaultSource::explicit_selection:
    case CatalogDatabaseTextDefaultSource::creation_policy:return !d.cluster_uuid;
    case CatalogDatabaseTextDefaultSource::cluster_default:
      return d.cluster_uuid&&d.cluster_uuid->kind==UuidKind::cluster&&
          uuid::IsEngineIdentityUuid(d.cluster_uuid->value);
  }
  return false;
}
bool Valid(const CatalogDatabaseRecord& r) {
  constexpr auto max = std::numeric_limits<u32>::max();
  return r.database_uuid.kind == UuidKind::database && uuid::IsEngineIdentityUuid(r.database_uuid.value) &&
      r.database_header_format_major > 0 && r.database_header_format_major <= max &&
      r.database_header_format_minor <= max &&
      r.catalog_manifest_format_version > 0 && r.catalog_manifest_format_version <= max &&
      r.page_size > 0 && r.page_size <= max && r.creator_transaction_number > 0 &&
      (!r.text_defaults||(r.datatype_cohort&&ValidTextDefaults(*r.text_defaults)))&&
      (!r.datatype_cohort ||
       (r.datatype_cohort->catalog_snapshot_uuid.kind == UuidKind::object &&
        datatypes::IsAdmittedDatatypeCohort(
            r.datatype_cohort->catalog_snapshot_uuid.value,
            r.datatype_cohort->catalog_generation,
            r.datatype_cohort->registry_generation)));
}
}
const CatalogValueSchema& CatalogDatabaseRecordSchema() {
  static const CatalogValueSchema schema{65537, 1,
      {kDatabaseFields.begin(), kDatabaseFields.begin() + 9}};
  return schema;
}
const CatalogValueSchema& CatalogDatabaseRecordSchemaV2() {
  static const CatalogValueSchema schema{65537, 2,
      {kDatabaseFields.begin(), kDatabaseFields.begin()+12}};
  return schema;
}
const CatalogValueSchema& CatalogDatabaseRecordSchemaV3() {
  static const CatalogValueSchema schema{65537, 3,
      {kDatabaseFields.begin(), kDatabaseFields.end()}};
  return schema;
}
std::optional<std::size_t> CatalogDatabaseRecordEncodedBytes(const CatalogDatabaseRecord& r) {
  if(!Valid(r))return {};
  return r.text_defaults?(r.text_defaults->cluster_uuid?408:384):r.datatype_cohort?232:176;
}
CatalogValueEncodeIntoResult EncodeCatalogDatabaseRecordInto(
    const CatalogDatabaseRecord& r,std::span<byte> backing) {
  if(!Valid(r))return {CatalogValueError::invalid_value,{}};
  // Scalars are serialized from stack backing below; still reject overlap
  // with the complete input, including scalar/padding/optional storage.
  const auto out=reinterpret_cast<std::uintptr_t>(backing.data());
  const auto input=reinterpret_cast<std::uintptr_t>(&r);
  if((backing.size()&&!backing.data())||
      backing.size()>std::numeric_limits<std::uintptr_t>::max()-out||
      (out<input+sizeof(r)&&input<out+backing.size()))
    return {CatalogValueError::invalid_backing,{}};
  std::array<std::array<byte,8>,15> scalars{};
  const std::array<u64,15> values{r.database_header_format_major,r.database_header_format_minor,
      r.catalog_manifest_format_version,r.page_size,r.creation_unix_epoch_millis,
      r.feature_flags,r.compatibility_flags,r.creator_transaction_number,
      r.datatype_cohort?r.datatype_cohort->catalog_generation:0,
      r.datatype_cohort?r.datatype_cohort->registry_generation:0,
      r.text_defaults?r.text_defaults->resource_activation_epoch:0,
      r.text_defaults?r.text_defaults->charset_family_epoch:0,
      r.text_defaults?r.text_defaults->collation_family_epoch:0,
      r.text_defaults?static_cast<u64>(r.text_defaults->source):0,
      r.text_defaults?r.text_defaults->source_generation:0};
  for(std::size_t i=0;i<values.size();++i)for(unsigned j=0;j<8;++j)
    scalars[i][j]=static_cast<byte>(values[i]>>(8*j));
  std::array<CatalogValueFieldView,21> fields{};
  fields[0]={1,T::engine_identity,r.database_uuid.kind,r.database_uuid.value.bytes};
  for(unsigned i=0;i<8;++i)
    fields[i+1]={static_cast<u16>(i+2),T::unsigned_integer,UuidKind::unknown,scalars[i]};
  if(r.datatype_cohort) {
    fields[9]={10,T::engine_identity,r.datatype_cohort->catalog_snapshot_uuid.kind,
        r.datatype_cohort->catalog_snapshot_uuid.value.bytes};
    fields[10]={11,T::unsigned_integer,UuidKind::unknown,scalars[8]};
    fields[11]={12,T::unsigned_integer,UuidKind::unknown,scalars[9]};
  }
  std::size_t count=r.datatype_cohort?12:9;
  if(r.text_defaults) {
    const auto& d=*r.text_defaults;
    fields[12]={13,T::engine_identity,d.charset_uuid.kind,d.charset_uuid.value.bytes};
    fields[13]={14,T::engine_identity,d.collation_uuid.kind,d.collation_uuid.value.bytes};
    for(unsigned i=0;i<4;++i)
      fields[14+i]={static_cast<u16>(15+i),T::unsigned_integer,UuidKind::unknown,scalars[10+i]};
    fields[18]={19,T::engine_identity,d.source_object_uuid.kind,d.source_object_uuid.value.bytes};
    fields[19]={20,T::unsigned_integer,UuidKind::unknown,scalars[14]};
    count=20;
    if(d.cluster_uuid) {
      fields[20]={21,T::engine_identity,d.cluster_uuid->kind,d.cluster_uuid->value.bytes};
      ++count;
    }
  }
  return EncodeCatalogValueBlockInto(r.text_defaults?kDatabaseSchemaV3:
      r.datatype_cohort?kDatabaseSchemaV2:kDatabaseSchemaV1,
      std::span<const CatalogValueFieldView>(fields).first(count),backing);
}
CatalogValueEncodeResult EncodeCatalogDatabaseRecord(const CatalogDatabaseRecord& r) {
  std::array<byte,kCatalogDatabaseRecordMaximumBytes> backing;
  const auto encoded=EncodeCatalogDatabaseRecordInto(r,backing);
  if(!encoded.ok())return {encoded.error,{}};
  return {CatalogValueError::none,{encoded.bytes.begin(),encoded.bytes.end()}};
}
CatalogDatabaseRecordDecodeResult DecodeCatalogDatabaseRecord(std::string_view bytes) {
  // Fixed stack-backed structural admission: neither a payload copy nor a
  // heap-allocated field vector is needed. Only native values escape the view.
  if (bytes.size() != 176 && bytes.size() != 232 && bytes.size() != 384 && bytes.size() != 408)
    return {CatalogValueError::invalid_framing, {}};
  const bool v2 = bytes.size() >= 232;
  const bool v3 = bytes.size() >= 384;
  std::array<CatalogValueFieldView, 21> fields{};
  const auto decoded = DecodeCatalogValueBlockInto(
      v3 ? kDatabaseSchemaV3 : v2 ? kDatabaseSchemaV2 : kDatabaseSchemaV1,
      {reinterpret_cast<const byte*>(bytes.data()), bytes.size()}, fields);
  if (!decoded.ok()) return {decoded.error, {}};
  CatalogDatabaseRecord r;
  r.database_uuid = *decoded.fields[0].identity();
  r.database_header_format_major = *decoded.fields[1].unsigned_value();
  r.database_header_format_minor = *decoded.fields[2].unsigned_value();
  r.catalog_manifest_format_version = *decoded.fields[3].unsigned_value();
  r.page_size = *decoded.fields[4].unsigned_value();
  r.creation_unix_epoch_millis = *decoded.fields[5].unsigned_value();
  r.feature_flags = *decoded.fields[6].unsigned_value();
  r.compatibility_flags = *decoded.fields[7].unsigned_value();
  r.creator_transaction_number = *decoded.fields[8].unsigned_value();
  if (v2) {
    r.datatype_cohort = CatalogDatabaseDatatypeCohort{
        *decoded.fields[9].identity(),
        *decoded.fields[10].unsigned_value(),
        *decoded.fields[11].unsigned_value()};
  }
  if(v3) {
    CatalogDatabaseTextDefaults d;
    d.charset_uuid=*decoded.fields[12].identity();
    d.collation_uuid=*decoded.fields[13].identity();
    d.resource_activation_epoch=*decoded.fields[14].unsigned_value();
    d.charset_family_epoch=*decoded.fields[15].unsigned_value();
    d.collation_family_epoch=*decoded.fields[16].unsigned_value();
    d.source=static_cast<CatalogDatabaseTextDefaultSource>(*decoded.fields[17].unsigned_value());
    d.source_object_uuid=*decoded.fields[18].identity();
    d.source_generation=*decoded.fields[19].unsigned_value();
    if(decoded.fields.size()==21)d.cluster_uuid=*decoded.fields[20].identity();
    r.text_defaults=d;
  }
  if (!Valid(r)) return {CatalogValueError::invalid_value, {}};
  return {CatalogValueError::none, r};
}
}  // namespace scratchbird::core::catalog
