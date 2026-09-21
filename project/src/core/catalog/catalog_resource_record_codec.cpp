// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "catalog_resource_record_codec.hpp"
#include "uuid.hpp"
#include <algorithm>
#include <limits>
#include <map>

namespace scratchbird::core::catalog {
namespace {
using T = CatalogValueType;
using E = CatalogValueError;
constexpr u32 kTextMax = static_cast<u32>(kCatalogResourcePayloadMaxBytes);
bool Same(const TypedUuid& a, const TypedUuid& b) {
  return a.kind == b.kind && a.value == b.value;
}
template <typename Record> bool CommonValid(const Record& r) {
  return !r.canonical_name.empty() && r.resource_epoch != 0 && r.family_epoch != 0 &&
      !r.family_version.empty() && !r.resource_seed_pack.empty() &&
      !r.resource_seed_version.empty() && r.creator_transaction_number != 0;
}
bool Valid(const CatalogCharsetRecord& r) {
  return CommonValid(r) && r.min_bytes != 0 && r.max_bytes >= r.min_bytes &&
      r.max_bytes <= std::numeric_limits<u32>::max() &&
      r.variable_width == (r.min_bytes != r.max_bytes) &&
      r.default_collation_uuid.has_value() == !r.default_collation_name.empty();
}
bool Valid(const CatalogCollationRecord& r) {
  return CommonValid(r) && !r.charset_name.empty() &&
      resources::ValidCollationProfile(r.comparison_profile, r.case_insensitive, r.accent_insensitive) &&
      (!r.default_for_charset || !r.default_authority.empty());
}
CatalogValueEncodeResult Encode(const CatalogValueSchema& schema,
                                const std::vector<CatalogValueField>& fields) {
  auto result = EncodeCatalogValueBlock(schema, fields);
  if (result.ok() && result.bytes.size() > kCatalogResourcePayloadMaxBytes)
    return {E::size_limit, {}};
  return result;
}
CatalogValueDecodeResult Decode(const CatalogValueSchema& schema, std::string_view bytes) {
  if (bytes.size() > kCatalogResourcePayloadMaxBytes) return {E::size_limit, {}};
  return DecodeCatalogValueBlock(schema, {bytes.begin(), bytes.end()});
}
template <typename V> const V& Get(const CatalogValueDecodeResult& result, u16 id) {
  // The selected owning schema has already proved exact types and required
  // fields; field13 is queried only when its explicit optional entry exists.
  const auto it = std::lower_bound(result.fields.begin(), result.fields.end(), id,
      [](const CatalogValueField& field, u16 key) { return field.id < key; });
  return std::get<V>(it->value);
}
bool ResourceKind(CatalogRecordKind kind) {
  return kind == CatalogRecordKind::charset || kind == CatalogRecordKind::charset_alias ||
      kind == CatalogRecordKind::collation || kind == CatalogRecordKind::timezone;
}
}  // namespace

const CatalogValueSchema& CatalogCharsetRecordSchema() {
  static const CatalogValueSchema schema{65556, 1, {
      {1,T::unsigned_integer,true,8}, {2,T::utf8_text,true,kTextMax},
      {3,T::engine_identity,true,16,UuidKind::object},
      {4,T::utf8_text_list,true,kTextMax}, {5,T::utf8_text,true,kTextMax},
      {6,T::unsigned_integer,true,8}, {7,T::unsigned_integer,true,8},
      {8,T::boolean,true,1}, {9,T::utf8_text,true,kTextMax},
      {10,T::utf8_text,true,kTextMax}, {11,T::utf8_text_list,true,kTextMax},
      {12,T::utf8_text,true,kTextMax}, {13,T::engine_identity,false,16,UuidKind::object},
      {14,T::utf8_text,true,kTextMax}, {15,T::unsigned_integer,true,8},
      {16,T::unsigned_integer,true,8}, {17,T::utf8_text,true,kTextMax},
      {18,T::utf8_text,true,kTextMax}, {19,T::utf8_text,true,kTextMax},
      {20,T::boolean,true,1}, {21,T::boolean,true,1}, {22,T::unsigned_integer,true,8}}};
  return schema;
}
const CatalogValueSchema& CatalogResourceAliasRecordSchema() {
  static const CatalogValueSchema schema{65557,1,{
      {1,T::unsigned_integer,true,8}, {2,T::utf8_text,true,kTextMax},
      {3,T::utf8_text,true,kTextMax}, {4,T::engine_identity,true,16,UuidKind::object},
      {5,T::utf8_text,true,kTextMax}, {6,T::unsigned_integer,true,8},
      {7,T::unsigned_integer,true,8}, {8,T::utf8_text,true,kTextMax},
      {9,T::utf8_text,true,kTextMax}, {10,T::utf8_text,true,kTextMax},
      {11,T::boolean,true,1}, {12,T::boolean,true,1}, {13,T::unsigned_integer,true,8}}};
  return schema;
}
namespace {
bool Valid(const CatalogResourceAliasRecord& r) {
  return (r.family == 0 || r.family == 3 || r.family == 9 || r.family == 10) && !r.alias.empty() &&
      !r.canonical_name.empty() && !r.source_path.empty() && !r.seed_pack_name.empty() &&
      !r.seed_pack_version.empty() && r.resource_epoch != 0 && r.creator_transaction_number != 0 &&
      r.target_uuid.has_value() && r.family_epoch != 0 && !r.family_version.empty();
}
}
CatalogValueEncodeResult EncodeCatalogResourceAliasRecord(const CatalogResourceAliasRecord& r) {
  if (!Valid(r)) return {E::invalid_value,{}};
  std::vector<CatalogValueField> fields{{1,r.family},{2,r.alias},{3,r.canonical_name}};
  if (r.target_uuid) fields.push_back({4,*r.target_uuid});
  const std::vector<CatalogValueField> tail{{5,r.source_path},{6,r.resource_epoch},{7,r.family_epoch},
      {8,r.family_version},{9,r.seed_pack_name},{10,r.seed_pack_version},
      {11,r.loaded_at_database_create},{12,r.engine_owned},{13,r.creator_transaction_number}};
  fields.insert(fields.end(),tail.begin(),tail.end());
  return Encode(CatalogResourceAliasRecordSchema(),fields);
}
CatalogResourceDecodeResult<CatalogResourceAliasRecord> DecodeCatalogResourceAliasRecord(std::string_view bytes) {
  const auto d=Decode(CatalogResourceAliasRecordSchema(),bytes);
  if (!d.ok()) return {d.error,{}};
  CatalogResourceAliasRecord r;
  r.family=Get<u64>(d,1); r.alias=Get<std::string>(d,2); r.canonical_name=Get<std::string>(d,3);
  for (const auto& field:d.fields) if (field.id==4) r.target_uuid=std::get<TypedUuid>(field.value);
  r.source_path=Get<std::string>(d,5); r.resource_epoch=Get<u64>(d,6); r.family_epoch=Get<u64>(d,7);
  r.family_version=Get<std::string>(d,8); r.seed_pack_name=Get<std::string>(d,9);
  r.seed_pack_version=Get<std::string>(d,10); r.loaded_at_database_create=Get<bool>(d,11);
  r.engine_owned=Get<bool>(d,12); r.creator_transaction_number=Get<u64>(d,13);
  if (!Valid(r)) return {E::invalid_value,{}};
  return {E::none,std::move(r)};
}
const CatalogValueSchema& CatalogCollationRecordSchema() {
  static const CatalogValueSchema schema{65558, 2, {
      {1,T::unsigned_integer,true,8}, {2,T::utf8_text,true,kTextMax},
      {3,T::engine_identity,true,16,UuidKind::object}, {4,T::utf8_text,true,kTextMax},
      {5,T::engine_identity,true,16,UuidKind::object}, {6,T::boolean,true,1},
      {7,T::utf8_text,true,kTextMax}, {8,T::boolean,true,1}, {9,T::boolean,true,1},
      {10,T::utf8_text,true,kTextMax}, {11,T::utf8_text,true,kTextMax},
      {12,T::utf8_text_list,true,kTextMax}, {13,T::utf8_text,true,kTextMax},
      {14,T::unsigned_integer,true,8}, {15,T::unsigned_integer,true,8},
      {16,T::utf8_text,true,kTextMax}, {17,T::utf8_text,true,kTextMax},
      {18,T::utf8_text,true,kTextMax}, {19,T::boolean,true,1},
      {20,T::boolean,true,1}, {21,T::unsigned_integer,true,8}, {22,T::unsigned_integer,true,8}}};
  return schema;
}
CatalogValueEncodeResult EncodeCatalogCharsetRecord(const CatalogCharsetRecord& r) {
  if (!Valid(r)) return {E::invalid_value, {}};
  std::vector<CatalogValueField> fields{
      {1,u64{1}}, {2,r.canonical_name}, {3,r.resource_uuid}, {4,r.aliases},
      {5,r.description}, {6,r.min_bytes}, {7,r.max_bytes}, {8,r.variable_width},
      {9,r.encoding_type}, {10,r.iana_name}, {11,r.supported_by}, {12,r.default_collation_name}};
  if (r.default_collation_uuid) fields.push_back({13,*r.default_collation_uuid});
  const std::vector<CatalogValueField> tail{
      {14,r.source_path}, {15,r.resource_epoch}, {16,r.family_epoch}, {17,r.family_version},
      {18,r.resource_seed_pack}, {19,r.resource_seed_version}, {20,r.loaded_at_database_create},
      {21,r.engine_owned}, {22,r.creator_transaction_number}};
  fields.insert(fields.end(), tail.begin(), tail.end());
  return Encode(CatalogCharsetRecordSchema(), fields);
}
CatalogValueEncodeResult EncodeCatalogCollationRecord(const CatalogCollationRecord& r) {
  if (!Valid(r)) return {E::invalid_value, {}};
  return Encode(CatalogCollationRecordSchema(), {
      {1,u64{2}}, {2,r.canonical_name}, {3,r.resource_uuid}, {4,r.charset_name},
      {5,r.charset_uuid}, {6,r.default_for_charset}, {7,r.default_authority},
      {8,r.case_insensitive}, {9,r.accent_insensitive}, {10,r.language},
      {11,r.description}, {12,r.supported_by}, {13,r.source_path}, {14,r.resource_epoch},
      {15,r.family_epoch}, {16,r.family_version}, {17,r.resource_seed_pack},
      {18,r.resource_seed_version}, {19,r.loaded_at_database_create}, {20,r.engine_owned},
      {21,r.creator_transaction_number}, {22,static_cast<u64>(r.comparison_profile)}});
}
CatalogResourceDecodeResult<CatalogCharsetRecord> DecodeCatalogCharsetRecord(std::string_view bytes) {
  const auto d = Decode(CatalogCharsetRecordSchema(), bytes);
  if (!d.ok()) return {d.error, {}};
  if (Get<u64>(d,1) != 1) return {E::invalid_value, {}};
  CatalogCharsetRecord r;
  r.canonical_name=Get<std::string>(d,2); r.resource_uuid=Get<TypedUuid>(d,3);
  r.aliases=Get<std::vector<std::string>>(d,4); r.description=Get<std::string>(d,5);
  r.min_bytes=Get<u64>(d,6); r.max_bytes=Get<u64>(d,7); r.variable_width=Get<bool>(d,8);
  r.encoding_type=Get<std::string>(d,9); r.iana_name=Get<std::string>(d,10);
  r.supported_by=Get<std::vector<std::string>>(d,11); r.default_collation_name=Get<std::string>(d,12);
  if (std::any_of(d.fields.begin(), d.fields.end(), [](const auto& f) { return f.id == 13; }))
    r.default_collation_uuid=Get<TypedUuid>(d,13);
  r.source_path=Get<std::string>(d,14); r.resource_epoch=Get<u64>(d,15); r.family_epoch=Get<u64>(d,16);
  r.family_version=Get<std::string>(d,17); r.resource_seed_pack=Get<std::string>(d,18);
  r.resource_seed_version=Get<std::string>(d,19); r.loaded_at_database_create=Get<bool>(d,20);
  r.engine_owned=Get<bool>(d,21); r.creator_transaction_number=Get<u64>(d,22);
  if (!Valid(r)) return {E::invalid_value, {}};
  return {E::none,std::move(r)};
}
CatalogResourceDecodeResult<CatalogCollationRecord> DecodeCatalogCollationRecord(std::string_view bytes) {
  const auto d = Decode(CatalogCollationRecordSchema(), bytes);
  if (!d.ok()) return {d.error, {}};
  if (Get<u64>(d,1) != 2) return {E::invalid_value, {}};
  CatalogCollationRecord r;
  r.canonical_name=Get<std::string>(d,2); r.resource_uuid=Get<TypedUuid>(d,3);
  r.charset_name=Get<std::string>(d,4); r.charset_uuid=Get<TypedUuid>(d,5);
  r.default_for_charset=Get<bool>(d,6); r.default_authority=Get<std::string>(d,7);
  r.case_insensitive=Get<bool>(d,8); r.accent_insensitive=Get<bool>(d,9);
  r.language=Get<std::string>(d,10); r.description=Get<std::string>(d,11);
  r.supported_by=Get<std::vector<std::string>>(d,12); r.source_path=Get<std::string>(d,13);
  r.resource_epoch=Get<u64>(d,14); r.family_epoch=Get<u64>(d,15);
  r.family_version=Get<std::string>(d,16); r.resource_seed_pack=Get<std::string>(d,17);
  r.resource_seed_version=Get<std::string>(d,18); r.loaded_at_database_create=Get<bool>(d,19);
  r.engine_owned=Get<bool>(d,20); r.creator_transaction_number=Get<u64>(d,21);
  r.comparison_profile=static_cast<resources::CollationProfile>(Get<u64>(d,22));
  if (!Valid(r)) return {E::invalid_value, {}};
  return {E::none,std::move(r)};
}
bool CatalogResourcePayloadMatchesHeader(const CatalogTypedRecord& record) {
  if (record.header.deleted || record.header.parent_uuid.kind != UuidKind::object ||
      !uuid::IsEngineIdentityUuid(record.header.parent_uuid.value)) return false;
  if (record.header.kind == CatalogRecordKind::charset_alias) {
    const auto d=DecodeCatalogResourceAliasRecord(record.payload);
    return record.header.object_uuid.kind == UuidKind::unknown && record.header.object_uuid.value.is_nil() &&
        d.ok() && d.record->family == 0 && d.record->target_uuid &&
        Same(*d.record->target_uuid,record.header.parent_uuid);
  }
  if (record.header.object_uuid.kind != UuidKind::object) return false;
  if (record.header.kind == CatalogRecordKind::charset) {
    const auto d=DecodeCatalogCharsetRecord(record.payload);
    return d.ok() && Same(d.record->resource_uuid,record.header.object_uuid);
  }
  if (record.header.kind == CatalogRecordKind::collation) {
    const auto d=DecodeCatalogCollationRecord(record.payload);
    return d.ok() && Same(d.record->resource_uuid,record.header.object_uuid) &&
        Same(d.record->charset_uuid,record.header.parent_uuid);
  }
  if (record.header.kind == CatalogRecordKind::timezone) {
    const auto d=DecodeCatalogResourceAliasRecord(record.payload);
    return d.ok() && d.record->family==9 && d.record->alias==d.record->canonical_name &&
        d.record->target_uuid && Same(*d.record->target_uuid,record.header.object_uuid);
  }
  return false;
}
bool ValidateCatalogResourceGraph(const std::vector<CatalogTypedRecord>& records) {
  std::map<std::array<byte,16>,const CatalogTypedRecord*> objects;
  for (const auto& r:records) {
    if (!uuid::IsEngineIdentityUuid(r.header.object_uuid.value)) {
      if (ResourceKind(r.header.kind) && r.header.kind != CatalogRecordKind::charset_alias) return false;
      continue;
    }
    const auto inserted=objects.emplace(r.header.object_uuid.value.bytes,&r);
    if (!inserted.second && (ResourceKind(r.header.kind) || ResourceKind(inserted.first->second->header.kind)))
      return false;
  }
  const auto find=[&](const TypedUuid& id,CatalogRecordKind kind) -> const CatalogTypedRecord* {
    const auto it=objects.find(id.value.bytes);
    if (it==objects.end() || it->second->header.kind!=kind || it->second->header.deleted ||
        !Same(it->second->header.object_uuid,id)) return nullptr;
    return it->second;
  };
  for (const auto& raw:records) {
    if (!ResourceKind(raw.header.kind)) continue;
    if (!CatalogResourcePayloadMatchesHeader(raw)) return false;
    if (raw.header.kind==CatalogRecordKind::charset) {
      if (!find(raw.header.parent_uuid,CatalogRecordKind::resource_bundle)) return false;
      const auto charset=DecodeCatalogCharsetRecord(raw.payload);
      if (charset.record->default_collation_uuid) {
        const auto* target=find(*charset.record->default_collation_uuid,CatalogRecordKind::collation);
        if (!target) return false;
        const auto collation=DecodeCatalogCollationRecord(target->payload);
        if (!collation.ok() || !collation.record->default_for_charset ||
            collation.record->canonical_name!=charset.record->default_collation_name ||
            !Same(collation.record->charset_uuid,charset.record->resource_uuid)) return false;
      }
    } else if (raw.header.kind == CatalogRecordKind::collation) {
      const auto collation=DecodeCatalogCollationRecord(raw.payload);
      const auto* parent=find(collation.record->charset_uuid,CatalogRecordKind::charset);
      if (!parent) return false;
      const auto charset=DecodeCatalogCharsetRecord(parent->payload);
      if (!charset.ok() || charset.record->canonical_name!=collation.record->charset_name ||
          charset.record->resource_epoch!=collation.record->resource_epoch ||
          charset.record->resource_seed_pack!=collation.record->resource_seed_pack ||
          charset.record->resource_seed_version!=collation.record->resource_seed_version) return false;
      if (collation.record->default_for_charset &&
          (!charset.record->default_collation_uuid ||
           !Same(*charset.record->default_collation_uuid,collation.record->resource_uuid))) return false;
    } else if (raw.header.kind==CatalogRecordKind::timezone) {
      if (!find(raw.header.parent_uuid,CatalogRecordKind::resource_bundle))return false;
    } else {
      const auto alias=DecodeCatalogResourceAliasRecord(raw.payload);
      const auto* parent=find(*alias.record->target_uuid,CatalogRecordKind::charset);
      if (!parent) return false;
      const auto charset=DecodeCatalogCharsetRecord(parent->payload);
      if (!charset.ok() || charset.record->resource_epoch != alias.record->resource_epoch ||
          charset.record->family_epoch != alias.record->family_epoch ||
          charset.record->family_version != alias.record->family_version ||
          charset.record->resource_seed_pack != alias.record->seed_pack_name ||
          charset.record->resource_seed_version != alias.record->seed_pack_version ||
          charset.record->creator_transaction_number != alias.record->creator_transaction_number ||
          charset.record->loaded_at_database_create != alias.record->loaded_at_database_create ||
          charset.record->engine_owned != alias.record->engine_owned) return false;
    }
  }
  return true;
}
}  // namespace scratchbird::core::catalog
