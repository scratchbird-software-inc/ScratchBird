// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "catalog_schema_definition.hpp"
#include "uuid.hpp"

namespace scratchbird::core::catalog {
namespace {
bool Absent(const TypedUuid& id) {
  return id.kind == UuidKind::unknown && id.value.is_nil();
}
bool Same(const TypedUuid& a, const TypedUuid& b) {
  return a.kind == b.kind && a.value == b.value;
}
bool Identity(const TypedUuid& id, UuidKind kind) {
  return id.kind == kind && uuid::IsEngineIdentityUuid(id.value);
}
bool Optional(const TypedUuid& id, UuidKind kind) {
  return Absent(id) || Identity(id, kind);
}
bool Valid(const CatalogSchemaDefinition& d) {
  return Identity(d.schema_object_uuid, UuidKind::object) &&
      Identity(d.database_catalog_object_uuid, UuidKind::object) &&
      Optional(d.parent_schema_uuid, UuidKind::object) &&
      Optional(d.default_filespace_uuid, UuidKind::filespace) &&
      Optional(d.default_charset_uuid, UuidKind::object) &&
      Optional(d.default_collation_uuid, UuidKind::object) &&
      Optional(d.permissions_policy_uuid, UuidKind::object) &&
      Identity(d.origin_transaction_uuid, UuidKind::transaction) &&
      d.origin_local_transaction_id != 0 && CatalogSchemaTypeName(d.schema_type) &&
      d.schema_object_uuid.value != d.database_catalog_object_uuid.value &&
      d.schema_object_uuid.value != d.parent_schema_uuid.value &&
      d.database_catalog_object_uuid.value != d.parent_schema_uuid.value;
}
}  // namespace

const char* CatalogSchemaTypeName(CatalogSchemaType type) {
  switch (type) {
    case CatalogSchemaType::system: return "system";
    case CatalogSchemaType::user_home: return "user_home";
    case CatalogSchemaType::remote_native: return "remote_native";
    case CatalogSchemaType::remote_emulated: return "remote_emulated";
    case CatalogSchemaType::public_compat: return "public_compat";
    case CatalogSchemaType::application: return "application";
    case CatalogSchemaType::cluster: return "cluster";
  }
  return nullptr;
}
const CatalogValueSchema& CatalogSchemaDefinitionSchema() {
  using T = CatalogValueType;
  static const CatalogValueSchema schema{65540, 1, {
      {1,T::engine_identity,true,16,UuidKind::object},
      {2,T::engine_identity,true,16,UuidKind::object},
      {3,T::engine_identity,false,16,UuidKind::object},
      {4,T::unsigned_integer,true,8},
      {5,T::engine_identity,false,16,UuidKind::filespace},
      {6,T::engine_identity,false,16,UuidKind::object},
      {7,T::engine_identity,false,16,UuidKind::object},
      {8,T::engine_identity,false,16,UuidKind::object},
      {9,T::engine_identity,true,16,UuidKind::transaction},
      {10,T::unsigned_integer,true,8}}};
  return schema;
}
CatalogValueEncodeResult EncodeCatalogSchemaDefinition(const CatalogSchemaDefinition& d) {
  if (!Valid(d)) return {CatalogValueError::invalid_value,{}};
  std::vector<CatalogValueField> fields{{1,d.schema_object_uuid},{2,d.database_catalog_object_uuid}};
  if (!Absent(d.parent_schema_uuid)) fields.push_back({3,d.parent_schema_uuid});
  fields.push_back({4,static_cast<u64>(d.schema_type)});
  if (!Absent(d.default_filespace_uuid)) fields.push_back({5,d.default_filespace_uuid});
  if (!Absent(d.default_charset_uuid)) fields.push_back({6,d.default_charset_uuid});
  if (!Absent(d.default_collation_uuid)) fields.push_back({7,d.default_collation_uuid});
  if (!Absent(d.permissions_policy_uuid)) fields.push_back({8,d.permissions_policy_uuid});
  fields.push_back({9,d.origin_transaction_uuid});
  fields.push_back({10,d.origin_local_transaction_id});
  return EncodeCatalogValueBlock(CatalogSchemaDefinitionSchema(),fields);
}
CatalogSchemaDefinitionResult DecodeCatalogSchemaDefinition(std::string_view bytes) {
  if (bytes.size() < 128 || bytes.size() > 248) return {CatalogValueError::invalid_framing,{}};
  const auto decoded = DecodeCatalogValueBlock(CatalogSchemaDefinitionSchema(),
      std::vector<byte>(bytes.begin(),bytes.end()));
  if (!decoded.ok()) return {decoded.error,{}};
  CatalogSchemaDefinition d;
  for (const auto& field : decoded.fields) {
    switch (field.id) {
      case 1: d.schema_object_uuid=std::get<TypedUuid>(field.value); break;
      case 2: d.database_catalog_object_uuid=std::get<TypedUuid>(field.value); break;
      case 3: d.parent_schema_uuid=std::get<TypedUuid>(field.value); break;
      case 4: d.schema_type=static_cast<CatalogSchemaType>(std::get<u64>(field.value)); break;
      case 5: d.default_filespace_uuid=std::get<TypedUuid>(field.value); break;
      case 6: d.default_charset_uuid=std::get<TypedUuid>(field.value); break;
      case 7: d.default_collation_uuid=std::get<TypedUuid>(field.value); break;
      case 8: d.permissions_policy_uuid=std::get<TypedUuid>(field.value); break;
      case 9: d.origin_transaction_uuid=std::get<TypedUuid>(field.value); break;
      case 10: d.origin_local_transaction_id=std::get<u64>(field.value); break;
      default: return {CatalogValueError::unknown_field,{}};
    }
  }
  if (!Valid(d)) return {CatalogValueError::invalid_value,{}};
  return {CatalogValueError::none,std::move(d)};
}
bool CatalogSchemaDefinitionMatchesMetadata(const CatalogMetadataVersion& m) {
  if (m.record.header.kind != CatalogRecordKind::schema) return false;
  const auto decoded=DecodeCatalogSchemaDefinition(m.record.payload);
  if (!decoded.ok()) return false;
  const auto& d=*decoded.definition;
  const auto& parent=Absent(d.parent_schema_uuid)?d.database_catalog_object_uuid:d.parent_schema_uuid;
  if (!Same(d.schema_object_uuid,m.record.header.object_uuid) || !Same(parent,m.record.header.parent_uuid) ||
      !Identity(m.default_name_uuid,UuidKind::object) || !Identity(m.name_vector_uuid,UuidKind::object) ||
      !Same(d.permissions_policy_uuid,m.security_policy_uuid) ||
      m.object_subtype != CatalogSchemaTypeName(d.schema_type) ||
      d.origin_local_transaction_id > m.creator_local_transaction_id ||
      (d.schema_type == CatalogSchemaType::cluster && m.authority_scope != CatalogAuthorityScope::cluster)) return false;
  if (Absent(d.parent_schema_uuid)) {
    if (!Absent(m.owning_schema_uuid)) return false;
  } else if (!Identity(m.owning_schema_uuid,UuidKind::schema) || m.owning_schema_uuid.value != d.parent_schema_uuid.value) return false;
  if (m.definition_version == 1 && (!Same(d.origin_transaction_uuid,m.creator_transaction_uuid) ||
      d.origin_local_transaction_id != m.creator_local_transaction_id)) return false;
  switch (m.status) {
    case CatalogObjectStatus::proposed:
    case CatalogObjectStatus::active:
    case CatalogObjectStatus::disabled_by_policy:
    case CatalogObjectStatus::retired:
    case CatalogObjectStatus::quarantined: return true;
    default: return false;
  }
}
bool CatalogSchemaDefinitionPreservesOrigin(const CatalogMetadataVersion& a,const CatalogMetadataVersion& b) {
  if (a.record.header.kind != CatalogRecordKind::schema && b.record.header.kind != CatalogRecordKind::schema) return true;
  if (!CatalogSchemaDefinitionMatchesMetadata(a) || !CatalogSchemaDefinitionMatchesMetadata(b)) return false;
  const auto before=DecodeCatalogSchemaDefinition(a.record.payload);
  const auto after=DecodeCatalogSchemaDefinition(b.record.payload);
  return Same(before.definition->schema_object_uuid,after.definition->schema_object_uuid) &&
      Same(before.definition->database_catalog_object_uuid,after.definition->database_catalog_object_uuid) &&
      Same(before.definition->origin_transaction_uuid,after.definition->origin_transaction_uuid) &&
      before.definition->origin_local_transaction_id == after.definition->origin_local_transaction_id;
}
}  // namespace scratchbird::core::catalog
