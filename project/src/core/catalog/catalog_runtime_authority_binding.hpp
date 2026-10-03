// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "catalog_record_codec.hpp"
#include "catalog_value_codec.hpp"
#include "uuid.hpp"
#include <optional>
#include <string_view>

namespace scratchbird::core::catalog {

enum class RuntimeAuthorityMode : u64 { database_local = 1, security_database = 2, cluster = 3 };

// Persisted selection only: never authentication, a grant, or an activation receipt.
struct CatalogRuntimeAuthorityBinding {
  Uuid binding_uuid, database_uuid, service_principal_uuid, security_authority_uuid;
  Uuid provider_uuid, policy_uuid;
  std::optional<Uuid> credential_reference_uuid;
  u64 generation = 0;
  RuntimeAuthorityMode authority_mode = static_cast<RuntimeAuthorityMode>(0);
  u64 security_epoch = 0, policy_epoch = 0, provider_generation = 0, catalog_generation = 0;
  TypedUuid origin_transaction_uuid;
  u64 origin_local_transaction_id = 0;
};

struct CatalogRuntimeAuthorityBindingResult {
  CatalogValueError error = CatalogValueError::invalid_value;
  std::optional<CatalogRuntimeAuthorityBinding> record;
  bool ok() const { return error == CatalogValueError::none && record.has_value(); }
};

namespace runtime_binding_detail {
inline bool Identity(const TypedUuid& id, UuidKind kind) {
  return id.kind == kind && uuid::IsEngineIdentityUuid(id.value);
}
inline bool Valid(const CatalogRuntimeAuthorityBinding& r) {
  for (const auto* id : {&r.binding_uuid, &r.database_uuid, &r.service_principal_uuid,
                        &r.security_authority_uuid, &r.provider_uuid, &r.policy_uuid})
    if (!uuid::IsEngineIdentityUuid(*id)) return false;
  if (r.credential_reference_uuid && !uuid::IsEngineIdentityUuid(*r.credential_reference_uuid))
    return false;
  const auto mode = static_cast<u64>(r.authority_mode);
  return mode >= 1 && mode <= 3 &&
      (r.authority_mode != RuntimeAuthorityMode::database_local ||
       r.security_authority_uuid == r.database_uuid) &&
      r.generation && r.security_epoch && r.policy_epoch && r.provider_generation &&
      r.catalog_generation && r.origin_local_transaction_id &&
      Identity(r.origin_transaction_uuid, UuidKind::transaction);
}
}  // namespace runtime_binding_detail

inline constexpr CatalogValueSchemaView CatalogRuntimeAuthorityBindingSchemaView() {
  using T = CatalogValueType;
  static constexpr CatalogValueFieldSchema fields[]{
      {1, T::engine_identity, true, 16, UuidKind::object},
      {2, T::unsigned_integer, true, 8},
      {3, T::engine_identity, true, 16, UuidKind::database},
      {4, T::engine_identity, true, 16, UuidKind::principal},
      {5, T::engine_identity, true, 16, UuidKind::database},
      {6, T::engine_identity, true, 16, UuidKind::object},
      {7, T::engine_identity, true, 16, UuidKind::object},
      {8, T::engine_identity, false, 16, UuidKind::object},
      {9, T::unsigned_integer, true, 8}, {10, T::unsigned_integer, true, 8},
      {11, T::unsigned_integer, true, 8}, {12, T::unsigned_integer, true, 8},
      {13, T::unsigned_integer, true, 8},
      {14, T::engine_identity, true, 16, UuidKind::transaction},
      {15, T::unsigned_integer, true, 8}};
  return {65587, 1, fields};
}

inline const CatalogValueSchema& CatalogRuntimeAuthorityBindingSchema() {
  static const CatalogValueSchema schema = [] {
    const auto view = CatalogRuntimeAuthorityBindingSchemaView();
    return CatalogValueSchema{view.id, view.version, {view.fields.begin(), view.fields.end()}};
  }();
  return schema;
}

inline CatalogValueEncodeResult EncodeCatalogRuntimeAuthorityBinding(const CatalogRuntimeAuthorityBinding& r) {
  if (!runtime_binding_detail::Valid(r)) return {CatalogValueError::invalid_value, {}};
  std::vector<CatalogValueField> fields{
      {1, TypedUuid{UuidKind::object, r.binding_uuid}}, {2, r.generation},
      {3, TypedUuid{UuidKind::database, r.database_uuid}},
      {4, TypedUuid{UuidKind::principal, r.service_principal_uuid}},
      {5, TypedUuid{UuidKind::database, r.security_authority_uuid}},
      {6, TypedUuid{UuidKind::object, r.provider_uuid}},
      {7, TypedUuid{UuidKind::object, r.policy_uuid}}};
  if (r.credential_reference_uuid)
    fields.push_back({8, TypedUuid{UuidKind::object, *r.credential_reference_uuid}});
  fields.insert(fields.end(), {{9, static_cast<u64>(r.authority_mode)}, {10, r.security_epoch},
      {11, r.policy_epoch}, {12, r.provider_generation}, {13, r.catalog_generation},
      {14, r.origin_transaction_uuid}, {15, r.origin_local_transaction_id}});
  return EncodeCatalogValueBlock(CatalogRuntimeAuthorityBindingSchema(), fields);
}

inline CatalogRuntimeAuthorityBindingResult DecodeCatalogRuntimeAuthorityBinding(std::string_view bytes) {
  // Exactly fourteen required fixed-width fields, plus one optional identity.
  if (bytes.size() != 304 && bytes.size() != 328) return {CatalogValueError::invalid_framing, {}};
  std::array<CatalogValueFieldView, 15> fields;
  const auto decoded = DecodeCatalogValueBlockInto(CatalogRuntimeAuthorityBindingSchemaView(),
      {reinterpret_cast<const byte*>(bytes.data()), bytes.size()}, fields);
  if (!decoded.ok()) return {decoded.error, {}};
  CatalogRuntimeAuthorityBinding r;
  for (const auto& f : decoded.fields) {
    switch (f.id) {
      case 1: r.binding_uuid = f.identity()->value; break;
      case 2: r.generation = *f.unsigned_value(); break;
      case 3: r.database_uuid = f.identity()->value; break;
      case 4: r.service_principal_uuid = f.identity()->value; break;
      case 5: r.security_authority_uuid = f.identity()->value; break;
      case 6: r.provider_uuid = f.identity()->value; break;
      case 7: r.policy_uuid = f.identity()->value; break;
      case 8: r.credential_reference_uuid = f.identity()->value; break;
      case 9: r.authority_mode = static_cast<RuntimeAuthorityMode>(*f.unsigned_value()); break;
      case 10: r.security_epoch = *f.unsigned_value(); break;
      case 11: r.policy_epoch = *f.unsigned_value(); break;
      case 12: r.provider_generation = *f.unsigned_value(); break;
      case 13: r.catalog_generation = *f.unsigned_value(); break;
      case 14: r.origin_transaction_uuid = *f.identity(); break;
      case 15: r.origin_local_transaction_id = *f.unsigned_value(); break;
    }
  }
  if (!runtime_binding_detail::Valid(r)) return {CatalogValueError::invalid_value, {}};
  return {CatalogValueError::none, std::move(r)};
}

inline bool IsCatalogRuntimeAuthorityBindingPayload(std::string_view bytes) {
  return bytes.size() >= kCatalogValueBlockHeaderBytes && bytes.substr(0, 4) == "SBCV" &&
      platform::LoadLittle32(reinterpret_cast<const byte*>(bytes.data()) + 16) == 65587;
}

inline bool CatalogRuntimeAuthorityBindingMatchesHeader(const CatalogTypedRecordView& r) {
  if (r.header.kind != CatalogRecordKind::config_profile ||
      !runtime_binding_detail::Identity(r.header.object_uuid, UuidKind::object)) return false;
  const auto decoded = DecodeCatalogRuntimeAuthorityBinding(r.payload);
  return decoded.ok() && decoded.record->binding_uuid == r.header.object_uuid.value;
}
inline bool CatalogRuntimeAuthorityBindingMatchesHeader(const CatalogTypedRecord& r) {
  return CatalogRuntimeAuthorityBindingMatchesHeader(BorrowCatalogTypedRecord(r));
}

inline bool CatalogRuntimeAuthorityBindingMatchesMetadata(const CatalogMetadataVersionView& m) {
  using runtime_binding_detail::Identity;
  if (!CatalogRuntimeAuthorityBindingMatchesHeader(m.record) ||
      m.object_subtype != "agent_runtime_authority" || m.authority_scope != CatalogAuthorityScope::local ||
      m.visibility != CatalogVisibilityClass::internal_only ||
      !Identity(m.owning_schema_uuid, UuidKind::schema) ||
      !Identity(m.record.header.parent_uuid, UuidKind::object) ||
      m.owning_schema_uuid.value != m.record.header.parent_uuid.value ||
      !Identity(m.owner_uuid, UuidKind::principal) || !Identity(m.audit_uuid, UuidKind::object) ||
      !Identity(m.security_policy_uuid, UuidKind::object) ||
      !Identity(m.creator_transaction_uuid, UuidKind::transaction)) return false;
  const auto decoded = DecodeCatalogRuntimeAuthorityBinding(m.record.payload);
  const auto& r = *decoded.record;
  return r.generation == m.definition_version && r.catalog_generation == m.catalog_generation &&
      r.security_epoch == m.security_epoch && r.policy_uuid == m.security_policy_uuid.value &&
      (m.definition_version != 1 || (r.origin_transaction_uuid.value == m.creator_transaction_uuid.value &&
                                   r.origin_local_transaction_id == m.creator_local_transaction_id));
}

inline bool CatalogRuntimeAuthorityBindingPreservesOrigin(
    const CatalogMetadataVersionView& a, const CatalogMetadataVersionView& b) {
  const auto family = [](const auto& m) {
    return m.object_subtype == "agent_runtime_authority" || IsCatalogRuntimeAuthorityBindingPayload(m.record.payload);
  };
  if (!family(a) && !family(b)) return true;
  if (!CatalogRuntimeAuthorityBindingMatchesMetadata(a) || !CatalogRuntimeAuthorityBindingMatchesMetadata(b)) return false;
  const auto x = DecodeCatalogRuntimeAuthorityBinding(a.record.payload);
  const auto y = DecodeCatalogRuntimeAuthorityBinding(b.record.payload);
  return x.record->binding_uuid == y.record->binding_uuid && x.record->database_uuid == y.record->database_uuid &&
      x.record->origin_transaction_uuid.value == y.record->origin_transaction_uuid.value &&
      x.record->origin_local_transaction_id == y.record->origin_local_transaction_id;
}
inline bool CatalogRuntimeAuthorityBindingMatchesMetadata(const CatalogMetadataVersion& m) {
  return CatalogRuntimeAuthorityBindingMatchesMetadata(BorrowCatalogMetadataVersion(m));
}
inline bool CatalogRuntimeAuthorityBindingPreservesOrigin(const CatalogMetadataVersion& a, const CatalogMetadataVersion& b) {
  return CatalogRuntimeAuthorityBindingPreservesOrigin(BorrowCatalogMetadataVersion(a), BorrowCatalogMetadataVersion(b));
}
}  // namespace scratchbird::core::catalog
