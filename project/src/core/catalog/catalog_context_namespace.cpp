// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "catalog_context_namespace.hpp"
#include "uuid.hpp"
#include <array>

namespace scratchbird::core::catalog {
namespace {
constexpr u32 kSchema = 65560;
constexpr std::size_t kBytes = 200;
constexpr CatalogValueFieldSchema kFields[]{
    {1,CatalogValueType::engine_identity,true,16,UuidKind::object},
    {2,CatalogValueType::unsigned_integer,true,8},
    {3,CatalogValueType::engine_identity,true,16,UuidKind::database},
    {4,CatalogValueType::engine_identity,true,16,UuidKind::schema},
    {5,CatalogValueType::engine_identity,true,16,UuidKind::principal},
    {6,CatalogValueType::engine_identity,true,16,UuidKind::object},
    {7,CatalogValueType::engine_identity,true,16,UuidKind::transaction},
    {8,CatalogValueType::unsigned_integer,true,8}};
bool Valid(const CatalogContextNamespace& r) {
  return uuid::IsEngineIdentityUuid(r.namespace_uuid) && r.generation &&
      uuid::IsEngineIdentityUuid(r.database_uuid) &&
      uuid::IsEngineIdentityUuid(r.schema_uuid) &&
      uuid::IsEngineIdentityUuid(r.owner_uuid) &&
      uuid::IsEngineIdentityUuid(r.security_label_uuid) &&
      uuid::IsEngineIdentityUuid(r.origin_transaction_uuid) && r.origin_local_transaction_id;
}
bool Identity(const TypedUuid& id, UuidKind kind, const Uuid& expected) {
  return id.kind == kind && id.value == expected && uuid::IsEngineIdentityUuid(id.value);
}
bool Family(const CatalogMetadataVersionView& m) {
  return m.object_subtype == "application_context_namespace" ||
      IsCatalogContextNamespacePayload(m.record.payload);
}
bool HeaderMatches(const CatalogTypedRecordView& header, const CatalogContextNamespace& r) {
  return header.header.kind == CatalogRecordKind::sql_object &&
      Identity(header.header.object_uuid,UuidKind::object,r.namespace_uuid) &&
      Identity(header.header.parent_uuid,UuidKind::object,r.schema_uuid);
}
bool MetadataMatches(const CatalogMetadataVersionView& m, const CatalogContextNamespace& r) {
  return m.object_subtype == "application_context_namespace" &&
      m.authority_scope == CatalogAuthorityScope::local && HeaderMatches(m.record,r) &&
      r.generation == m.definition_version &&
      Identity(m.owning_schema_uuid,UuidKind::schema,r.schema_uuid) &&
      Identity(m.owner_uuid,UuidKind::principal,r.owner_uuid) &&
      m.default_name_uuid.kind == UuidKind::object && uuid::IsEngineIdentityUuid(m.default_name_uuid.value) &&
      m.name_vector_uuid.kind == UuidKind::object && uuid::IsEngineIdentityUuid(m.name_vector_uuid.value) &&
      m.creator_transaction_uuid.kind == UuidKind::transaction &&
      uuid::IsEngineIdentityUuid(m.creator_transaction_uuid.value) &&
      r.origin_local_transaction_id <= m.creator_local_transaction_id &&
      (m.definition_version != 1 ||
       (r.origin_local_transaction_id == m.creator_local_transaction_id &&
        r.origin_transaction_uuid == m.creator_transaction_uuid.value));
}
}  // namespace

CatalogContextNamespaceResult DecodeCatalogContextNamespace(std::string_view bytes) {
  if (bytes.size() != kBytes) return {CatalogValueError::invalid_framing,{}};
  std::array<CatalogValueFieldView, 8> backing;
  const auto decoded = DecodeCatalogValueBlockInto({kSchema,1,kFields},
      {reinterpret_cast<const byte*>(bytes.data()), bytes.size()}, backing);
  if (!decoded.ok()) return {decoded.error,{}};
  const auto f = decoded.fields;
  CatalogContextNamespace r{f[0].identity()->value, *f[1].unsigned_value(),
      f[2].identity()->value, f[3].identity()->value, f[4].identity()->value,
      f[5].identity()->value, f[6].identity()->value, *f[7].unsigned_value()};
  if (!Valid(r)) return {};
  return {CatalogValueError::none,r};
}
CatalogValueEncodeResult EncodeCatalogContextNamespace(const CatalogContextNamespace& r) {
  if (!Valid(r)) return {CatalogValueError::invalid_value,{}};
  const CatalogValueSchema schema{kSchema,1,{std::begin(kFields),std::end(kFields)}};
  const std::vector<CatalogValueField> fields{
      {1,TypedUuid{UuidKind::object,r.namespace_uuid}}, {2,r.generation},
      {3,TypedUuid{UuidKind::database,r.database_uuid}},
      {4,TypedUuid{UuidKind::schema,r.schema_uuid}},
      {5,TypedUuid{UuidKind::principal,r.owner_uuid}},
      {6,TypedUuid{UuidKind::object,r.security_label_uuid}},
      {7,TypedUuid{UuidKind::transaction,r.origin_transaction_uuid}},
      {8,r.origin_local_transaction_id}};
  return EncodeCatalogValueBlock(schema,fields);
}
bool IsCatalogContextNamespacePayload(std::string_view bytes) {
  return bytes.size() >= kCatalogValueBlockHeaderBytes && bytes.substr(0,4) == "SBCV" &&
      platform::LoadLittle32(reinterpret_cast<const byte*>(bytes.data())+16) == kSchema;
}
bool CatalogContextNamespaceMatchesHeader(const CatalogTypedRecordView& r) {
  if (r.header.kind != CatalogRecordKind::sql_object) return false;
  const auto decoded = DecodeCatalogContextNamespace(r.payload);
  return decoded.ok() && HeaderMatches(r,*decoded.record);
}
bool CatalogContextNamespaceMatchesMetadata(const CatalogMetadataVersionView& m) {
  const auto decoded = DecodeCatalogContextNamespace(m.record.payload);
  return decoded.ok() && MetadataMatches(m,*decoded.record);
}
bool CatalogContextNamespaceMatchesDatabase(const CatalogMetadataVersionView& m, const Uuid& database) {
  if (!Family(m)) return true;
  if (!uuid::IsEngineIdentityUuid(database)) return false;
  const auto decoded = DecodeCatalogContextNamespace(m.record.payload);
  return decoded.ok() && decoded.record->database_uuid == database && MetadataMatches(m,*decoded.record);
}
bool CatalogContextNamespacePreservesOrigin(const CatalogMetadataVersionView& before,
                                          const CatalogMetadataVersionView& after) {
  if (!Family(before) && !Family(after)) return true;
  const auto original = DecodeCatalogContextNamespace(before.record.payload);
  const auto successor = DecodeCatalogContextNamespace(after.record.payload);
  if (!original.ok() || !successor.ok() || !MetadataMatches(before,*original.record) ||
      !MetadataMatches(after,*successor.record)) return false;
  const auto& a = *original.record;
  const auto& b = *successor.record;
  return a.namespace_uuid == b.namespace_uuid && a.database_uuid == b.database_uuid &&
      a.origin_transaction_uuid == b.origin_transaction_uuid &&
      a.origin_local_transaction_id == b.origin_local_transaction_id;
}
}  // namespace scratchbird::core::catalog
