// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "catalog_security_record_codec.hpp"

#include <algorithm>
#include <array>

namespace scratchbird::core::catalog {
namespace {
constexpr std::size_t kMaximumPayloadBytes = 130976;
constexpr std::array<std::string_view, 10> kIdentityNames{
    "principal_uuid", "group_uuid", "role_uuid", "grant_uuid", "member_uuid",
    "parent_uuid", "subject_uuid", "target_uuid", "policy_pack_uuid", "membership_uuid"};
constexpr std::array<std::string_view, 37> kAttributeNames{
    "creator_tx", "created_txn", "policy_generation", "loaded_at_database_create",
    "identity_authority", "engine_owned", "security_generation",
    "security_context_authority_version", "security_context_generation", "active",
    "immutable", "create_time_only", "role_code", "role_name", "description",
    "default_assignment", "group_code", "principal_name", "credential_fingerprint",
    "kind", "principal_kind", "bootstrap_principal", "grant_class", "member_kind",
    "parent_kind", "subject_kind", "effect", "right", "grant_name", "target",
    "ambient", "authority_class", "group_name", "ambient_rights", "connect_only",
    "operational_role", "created_disabled"};

constexpr u16 PrimaryField(CatalogRecordKind kind) {
  switch (kind) {
    case CatalogRecordKind::user_account: return 1;
    case CatalogRecordKind::group_account: return 2;
    case CatalogRecordKind::role_account: return 3;
    case CatalogRecordKind::grant_record: return 4;
    default: return 0;
  }
}

struct FixedSchema {
  u32 id = 0;
  u16 version = 0;
  std::array<CatalogValueFieldSchema, 47> fields{};
  std::size_t count = 0;
  constexpr CatalogValueSchemaView View() const { return {id, version, std::span(fields).first(count)}; }
};
constexpr FixedSchema Schema(CatalogRecordKind kind) {
  FixedSchema schema;
  const auto primary = PrimaryField(kind);
  if (!primary) return schema;
  schema.id = 65536 + static_cast<u32>(kind);
  schema.version = 1;
  for (u16 id = 1; id <= kIdentityNames.size(); ++id) {
    const bool admitted = id == primary || id == 9 ||
        (kind == CatalogRecordKind::grant_record && id != 2);
    if (admitted) schema.fields[schema.count++] =
        {id, CatalogValueType::engine_identity, id == primary, 16, UuidKind::object};
  }
  for (u16 index = 0; index < kAttributeNames.size(); ++index) {
    schema.fields[schema.count++] = {static_cast<u16>(32 + index), CatalogValueType::utf8_text,
                             index == 0, 130000};
  }
  return schema;
}

constexpr CatalogValueSchemaView SchemaView(CatalogRecordKind kind) {
  static constexpr auto user = Schema(CatalogRecordKind::user_account);
  static constexpr auto group = Schema(CatalogRecordKind::group_account);
  static constexpr auto role = Schema(CatalogRecordKind::role_account);
  static constexpr auto grant = Schema(CatalogRecordKind::grant_record);
  switch (kind) {
    case CatalogRecordKind::user_account: return user.View();
    case CatalogRecordKind::group_account: return group.View();
    case CatalogRecordKind::role_account: return role.View();
    case CatalogRecordKind::grant_record: return grant.View();
    default: return {};
  }
}
CatalogValueSchema OwningSchema(CatalogRecordKind kind) {
  const auto view = SchemaView(kind);
  return {view.id, view.version, {view.fields.begin(), view.fields.end()}};
}

template <std::size_t N>
std::optional<u16> FieldId(const std::array<std::string_view, N>& names,
                         std::string_view name, u16 first) {
  const auto found = std::find(names.begin(), names.end(), name);
  if (found == names.end()) return std::nullopt;
  return static_cast<u16>(first + std::distance(names.begin(), found));
}
}  // namespace

Uuid CatalogSecurityRecord::Identity(std::string_view name) const {
  const auto found = identities.find(std::string(name));
  return found == identities.end() ? Uuid{} : found->second;
}

bool IsCatalogSecurityRecordKind(CatalogRecordKind kind) { return PrimaryField(kind) != 0; }

std::string_view CatalogSecurityPrimaryIdentityName(CatalogRecordKind kind) {
  const auto id = PrimaryField(kind);
  return id ? kIdentityNames[id - 1] : std::string_view{};
}

const CatalogValueSchema& CatalogSecurityRecordSchema(CatalogRecordKind kind) {
  static const auto user = OwningSchema(CatalogRecordKind::user_account);
  static const auto group = OwningSchema(CatalogRecordKind::group_account);
  static const auto role = OwningSchema(CatalogRecordKind::role_account);
  static const auto grant = OwningSchema(CatalogRecordKind::grant_record);
  static const CatalogValueSchema invalid;
  switch (kind) {
    case CatalogRecordKind::user_account: return user;
    case CatalogRecordKind::group_account: return group;
    case CatalogRecordKind::role_account: return role;
    case CatalogRecordKind::grant_record: return grant;
    default: return invalid;
  }
}

CatalogValueEncodeResult EncodeCatalogSecurityRecord(const CatalogSecurityRecord& record) {
  if (!IsCatalogSecurityRecordKind(record.kind)) return {CatalogValueError::invalid_schema, {}};
  std::vector<CatalogValueField> fields;
  // The admitted schemas bound the entry count before copying any values.
  if (record.identities.size() > kIdentityNames.size() ||
      record.attributes.size() > kAttributeNames.size()) return {CatalogValueError::unknown_field, {}};
  std::size_t total_bytes = kCatalogValueBlockHeaderBytes + record.identities.size() * 24;
  for (const auto& [name, identity] : record.identities) {
    const auto id = FieldId(kIdentityNames, name, 1);
    if (!id) return {CatalogValueError::unknown_field, {}};
    TypedUuid typed;
    typed.kind = UuidKind::object;
    typed.value = identity;
    fields.push_back({*id, typed});
  }
  for (const auto& [name, value] : record.attributes) {
    const auto id = FieldId(kAttributeNames, name, 32);
    if (!id) return {CatalogValueError::unknown_field, {}};
    if (value.size() > 130000 || total_bytes > kMaximumPayloadBytes - 8 ||
        value.size() > kMaximumPayloadBytes - total_bytes - 8)
      return {CatalogValueError::size_limit, {}};
    total_bytes += 8 + value.size();
    fields.push_back({*id, value});
  }
  std::sort(fields.begin(), fields.end(), [](const auto& left, const auto& right) {
    return left.id < right.id;
  });
  auto encoded = EncodeCatalogValueBlock(CatalogSecurityRecordSchema(record.kind), fields);
  if (encoded.ok() && encoded.bytes.size() > kMaximumPayloadBytes)
    return {CatalogValueError::size_limit, {}};
  return encoded;
}

Uuid CatalogSecurityRecordView::Identity(std::string_view name) const {
  const auto id = FieldId(kIdentityNames, name, 1);
  return id && identities[*id - 1] ? *identities[*id - 1] : Uuid{};
}
std::optional<std::string_view> CatalogSecurityRecordView::Attribute(std::string_view name) const {
  const auto id = FieldId(kAttributeNames, name, 32);
  return id ? attributes[*id - 32] : std::nullopt;
}
CatalogSecurityRecordViewResult DecodeCatalogSecurityRecordView(
    CatalogRecordKind kind, std::string_view bytes) {
  if (!IsCatalogSecurityRecordKind(kind)) return {CatalogValueError::invalid_schema, {}};
  if (bytes.size() < kCatalogValueBlockHeaderBytes || bytes.size() > kMaximumPayloadBytes)
    return {CatalogValueError::invalid_framing, {}};
  std::array<CatalogValueFieldView, 47> fields;
  const auto decoded = DecodeCatalogValueBlockInto(SchemaView(kind),
      {reinterpret_cast<const byte*>(bytes.data()), bytes.size()}, fields);
  if (!decoded.ok()) return {decoded.error, {}};
  CatalogSecurityRecordView record;
  record.kind = kind;
  for (const auto& field : decoded.fields) {
    if (field.id <= kIdentityNames.size()) record.identities[field.id - 1] = field.identity()->value;
    else record.attributes[field.id - 32] = std::string_view(
        reinterpret_cast<const char*>(field.bytes.data()), field.bytes.size());
  }
  return {CatalogValueError::none, record};
}
CatalogSecurityRecordDecodeResult DecodeCatalogSecurityRecord(
    CatalogRecordKind kind, std::string_view bytes) {
  const auto decoded = DecodeCatalogSecurityRecordView(kind, bytes);
  if (!decoded.ok()) return {decoded.error, {}};
  CatalogSecurityRecord record;
  record.kind = kind;
  const auto& v = *decoded.record;
  for (std::size_t i = 0; i < v.identities.size(); ++i)
    if (v.identities[i]) record.identities.emplace(std::string(kIdentityNames[i]), *v.identities[i]);
  for (std::size_t i = 0; i < v.attributes.size(); ++i)
    if (v.attributes[i]) record.attributes.emplace(std::string(kAttributeNames[i]), std::string(*v.attributes[i]));
  return {CatalogValueError::none, std::move(record)};
}

bool CatalogSecurityPayloadMatchesHeader(const CatalogTypedRecordView& record) {
  const auto decoded = DecodeCatalogSecurityRecordView(record.header.kind, record.payload);
  return decoded.ok() && record.header.object_uuid.kind == UuidKind::object &&
      record.header.object_uuid.value == decoded.record->Identity(
          CatalogSecurityPrimaryIdentityName(record.header.kind)) &&
      record.header.row_uuid.value != record.header.object_uuid.value;
}
bool CatalogSecurityPayloadMatchesHeader(const CatalogTypedRecord& record) {
  return CatalogSecurityPayloadMatchesHeader(BorrowCatalogTypedRecord(record));
}
}  // namespace scratchbird::core::catalog
