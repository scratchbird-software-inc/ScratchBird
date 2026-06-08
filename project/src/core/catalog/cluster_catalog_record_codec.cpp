// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "cluster_catalog_record_codec.hpp"

#include "uuid.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <map>
#include <set>
#include <utility>
#include <vector>

namespace scratchbird::core::catalog {
namespace {

using scratchbird::core::platform::DiagnosticArgument;
using scratchbird::core::platform::LoadLittle16;
using scratchbird::core::platform::LoadLittle32;
using scratchbird::core::platform::LoadLittle64;
using scratchbird::core::platform::MakeDiagnostic;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::StoreLittle16;
using scratchbird::core::platform::StoreLittle32;
using scratchbird::core::platform::StoreLittle64;
using scratchbird::core::platform::Subsystem;
using scratchbird::core::platform::TypedUuid;
using scratchbird::core::platform::UuidKind;
using scratchbird::core::platform::u16;
using scratchbird::core::platform::u64;
using scratchbird::core::uuid::ParseDurableEngineIdentityUuid;
using scratchbird::storage::page::CatalogPageRowKind;

inline constexpr std::array<byte, 8> kClusterRecordMagic = {
    'S', 'B', 'C', 'C', 'A', 'T', '0', '1'};
inline constexpr u32 kHeaderBytes = 32;
inline constexpr u32 kOffsetMagic = 0;
inline constexpr u32 kOffsetCodecVersion = 8;
inline constexpr u32 kOffsetSchemaVersion = 12;
inline constexpr u32 kOffsetFieldCount = 16;
inline constexpr u32 kOffsetPayloadBytes = 20;
inline constexpr u32 kOffsetPayloadChecksum = 24;

Status CodecOkStatus() {
  return {StatusCode::ok, Severity::info, Subsystem::catalog};
}

Status CodecErrorStatus() {
  return {StatusCode::platform_required_feature_missing,
          Severity::error,
          Subsystem::catalog};
}

ClusterCatalogRecordCodecResult CodecError(std::string diagnostic_code,
                                           std::string message_key,
                                           std::string detail = {}) {
  ClusterCatalogRecordCodecResult result;
  result.status = CodecErrorStatus();
  result.diagnostic = MakeClusterCatalogRecordCodecDiagnostic(
      result.status,
      std::move(diagnostic_code),
      std::move(message_key),
      std::move(detail));
  return result;
}

ClusterCatalogRecordSetValidationResult SetError(std::string diagnostic_code,
                                                 std::string message_key,
                                                 std::string detail = {}) {
  ClusterCatalogRecordSetValidationResult result;
  result.status = CodecErrorStatus();
  result.diagnostic = MakeClusterCatalogRecordCodecDiagnostic(
      result.status,
      std::move(diagnostic_code),
      std::move(message_key),
      std::move(detail));
  return result;
}

std::string Lower(std::string value) {
  for (char& ch : value) {
    ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  }
  return value;
}

bool ContainsForbiddenUserTextColumn(std::string_view column_name) {
  const std::string lower = Lower(std::string(column_name));
  return lower.find("name") != std::string::npos ||
         lower.find("comment") != std::string::npos ||
         lower.find("description") != std::string::npos;
}

bool ContainsPropertyBagColumn(std::string_view column_name) {
  const std::string lower = Lower(std::string(column_name));
  return lower.find("property_bag") != std::string::npos ||
         lower.find("properties") != std::string::npos;
}

bool IsPropertyBagType(std::string_view type_name) {
  const std::string lower = Lower(std::string(type_name));
  return lower == "property_bag" || lower == "json" || lower == "jsonb";
}

const ClusterCatalogColumnManifest* FindColumn(
    const ClusterCatalogTableManifest& table,
    std::string_view column_name) {
  return FindClusterCatalogColumn(table, std::string(column_name));
}

bool HasField(const ClusterCatalogRecord& record, std::string_view column_name) {
  return std::any_of(record.fields.begin(),
                     record.fields.end(),
                     [column_name](const ClusterCatalogFieldValue& field) {
                       return field.column_name == column_name;
                     });
}

std::string FieldValue(const ClusterCatalogRecord& record,
                       std::string_view column_name) {
  for (const ClusterCatalogFieldValue& field : record.fields) {
    if (field.column_name == column_name) {
      return field.value;
    }
  }
  return {};
}

UuidKind ExpectedUuidKindForColumn(std::string_view column_name) {
  if (column_name == "cluster_uuid") {
    return UuidKind::cluster;
  }
  return UuidKind::object;
}

bool ParseRequiredUuid(std::string_view column_name,
                       const std::string& value,
                       TypedUuid* out) {
  const auto parsed = ParseDurableEngineIdentityUuid(
      ExpectedUuidKindForColumn(column_name),
      value);
  if (!parsed.ok()) {
    return false;
  }
  if (out != nullptr) {
    *out = parsed.value;
  }
  return true;
}

u64 Fnv1a64(const byte* data, std::size_t size) {
  u64 hash = 1469598103934665603ull;
  for (std::size_t i = 0; i < size; ++i) {
    hash ^= static_cast<u64>(data[i]);
    hash *= 1099511628211ull;
  }
  return hash;
}

u64 Fnv1a64(const std::vector<byte>& bytes) {
  return bytes.empty() ? Fnv1a64(nullptr, 0) : Fnv1a64(bytes.data(), bytes.size());
}

void AppendLittle16(std::vector<byte>* out, u16 value) {
  const std::size_t offset = out->size();
  out->resize(offset + sizeof(value));
  StoreLittle16(out->data() + offset, value);
}

void AppendLittle32(std::vector<byte>* out, u32 value) {
  const std::size_t offset = out->size();
  out->resize(offset + sizeof(value));
  StoreLittle32(out->data() + offset, value);
}

void AppendLittle64(std::vector<byte>* out, u64 value) {
  const std::size_t offset = out->size();
  out->resize(offset + sizeof(value));
  StoreLittle64(out->data() + offset, value);
}

bool AppendSizedString(std::vector<byte>* out, const std::string& value) {
  if (value.size() > 0xffffu) {
    return false;
  }
  AppendLittle16(out, static_cast<u16>(value.size()));
  out->insert(out->end(), value.begin(), value.end());
  return true;
}

bool ReadLittle16(const std::vector<byte>& bytes, std::size_t* offset, u16* out) {
  if (*offset + sizeof(u16) > bytes.size()) {
    return false;
  }
  *out = LoadLittle16(bytes.data() + *offset);
  *offset += sizeof(u16);
  return true;
}

bool ReadLittle32(const std::vector<byte>& bytes, std::size_t* offset, u32* out) {
  if (*offset + sizeof(u32) > bytes.size()) {
    return false;
  }
  *out = LoadLittle32(bytes.data() + *offset);
  *offset += sizeof(u32);
  return true;
}

bool ReadSizedString(const std::vector<byte>& bytes,
                     std::size_t* offset,
                     std::string* out) {
  u16 length = 0;
  if (!ReadLittle16(bytes, offset, &length)) {
    return false;
  }
  if (*offset + length > bytes.size()) {
    return false;
  }
  out->assign(reinterpret_cast<const char*>(bytes.data() + *offset), length);
  *offset += length;
  return true;
}

ClusterCatalogRecordCodecResult ValidateResolverUuid(std::string_view diagnostic_detail,
                                                     UuidKind kind,
                                                     const std::string& value) {
  const auto parsed = ParseDurableEngineIdentityUuid(kind, value);
  if (parsed.ok()) {
    ClusterCatalogRecordCodecResult result;
    result.status = CodecOkStatus();
    return result;
  }
  return CodecError("SB-CLUSTER-CATALOG-RECORD-UUID-INVALID",
                    "catalog.cluster_record_codec.uuid_invalid",
                    std::string(diagnostic_detail));
}

}  // namespace

const ClusterCatalogTableManifest* FindBuiltinClusterCatalogTableManifestByPath(
    std::string_view table_path) {
  for (const ClusterCatalogTableManifest& table :
       BuiltinClusterCatalogTableManifests()) {
    if (ClusterCatalogFullTablePath(table) == table_path) {
      return &table;
    }
  }
  for (const ClusterRoleProfileManifest& role_profile :
       BuiltinClusterRoleProfileManifests()) {
    if (ClusterCatalogFullTablePath(role_profile.table) == table_path) {
      return &role_profile.table;
    }
  }
  return nullptr;
}

std::string ClusterCatalogRecordPrimaryUuidColumn(
    const ClusterCatalogRecord& record) {
  const auto* table = FindBuiltinClusterCatalogTableManifestByPath(record.table_path);
  if (table == nullptr || table->primary_key_columns.empty()) {
    return {};
  }
  return table->primary_key_columns.front();
}

std::string ClusterCatalogRecordPrimaryUuidValue(
    const ClusterCatalogRecord& record) {
  const std::string column = ClusterCatalogRecordPrimaryUuidColumn(record);
  return column.empty() ? std::string{} : FieldValue(record, column);
}

ClusterCatalogRecordCodecResult ValidateClusterCatalogRecord(
    const ClusterCatalogRecord& record) {
  const auto* table = FindBuiltinClusterCatalogTableManifestByPath(record.table_path);
  if (table == nullptr) {
    return CodecError("SB-CLUSTER-CATALOG-RECORD-TABLE-UNKNOWN",
                      "catalog.cluster_record_codec.table_unknown",
                      record.table_path);
  }
  if (record.codec_version < kClusterCatalogRecordCodecVersionMinSupported ||
      record.codec_version > kClusterCatalogRecordCodecVersionMaxSupported ||
      record.schema_version != table->manifest_version) {
    return CodecError("SB-CLUSTER-CATALOG-RECORD-VERSION-UNSUPPORTED",
                      "catalog.cluster_record_codec.version_unsupported",
                      record.table_path);
  }
  if (!table->external_provider_bound || table->local_runtime_execution_enabled ||
      table->mutable_by_local_core || !table->uuid_only_identity) {
    return CodecError("SB-CLUSTER-CATALOG-RECORD-AUTHORITY-BOUNDARY-INVALID",
                      "catalog.cluster_record_codec.authority_boundary_invalid",
                      record.table_path);
  }

  std::set<std::string> field_names;
  for (const ClusterCatalogFieldValue& field : record.fields) {
    if (field.column_name.empty()) {
      return CodecError("SB-CLUSTER-CATALOG-RECORD-FIELD-NAME-REQUIRED",
                        "catalog.cluster_record_codec.field_name_required",
                        record.table_path);
    }
    if (ContainsPropertyBagColumn(field.column_name)) {
      return CodecError("SB-CLUSTER-CATALOG-RECORD-PROPERTY-BAG-REFUSED",
                        "catalog.cluster_record_codec.property_bag_refused",
                        field.column_name);
    }
    if (ContainsForbiddenUserTextColumn(field.column_name)) {
      return CodecError("SB-CLUSTER-CATALOG-RECORD-USER-TEXT-REFUSED",
                        "catalog.cluster_record_codec.user_text_refused",
                        field.column_name);
    }
    if (!field_names.insert(field.column_name).second) {
      return CodecError("SB-CLUSTER-CATALOG-RECORD-DUPLICATE-FIELD",
                        "catalog.cluster_record_codec.duplicate_field",
                        field.column_name);
    }
    const auto* column = FindColumn(*table, field.column_name);
    if (column == nullptr) {
      return CodecError("SB-CLUSTER-CATALOG-RECORD-FIELD-UNKNOWN",
                        "catalog.cluster_record_codec.field_unknown",
                        field.column_name);
    }
    if (IsPropertyBagType(column->type_name)) {
      return CodecError("SB-CLUSTER-CATALOG-RECORD-PROPERTY-BAG-REFUSED",
                        "catalog.cluster_record_codec.property_bag_refused",
                        field.column_name);
    }
    if (column->required && field.value.empty()) {
      return CodecError("SB-CLUSTER-CATALOG-RECORD-FIELD-REQUIRED",
                        "catalog.cluster_record_codec.field_required",
                        field.column_name);
    }
    if (column->type_name == "uuid" &&
        !ParseRequiredUuid(field.column_name, field.value, nullptr)) {
      return CodecError("SB-CLUSTER-CATALOG-RECORD-UUID-INVALID",
                        "catalog.cluster_record_codec.uuid_invalid",
                        field.column_name);
    }
  }

  for (const std::string& column_name : table->required_columns) {
    if (!HasField(record, column_name) || FieldValue(record, column_name).empty()) {
      return CodecError("SB-CLUSTER-CATALOG-RECORD-REQUIRED-FIELD-MISSING",
                        "catalog.cluster_record_codec.required_field_missing",
                        column_name);
    }
  }
  if (table->primary_key_columns.size() != 1 ||
      FieldValue(record, table->primary_key_columns.front()).empty()) {
    return CodecError("SB-CLUSTER-CATALOG-RECORD-PRIMARY-UUID-MISSING",
                      "catalog.cluster_record_codec.primary_uuid_missing",
                      record.table_path);
  }

  ClusterCatalogRecordCodecResult result;
  result.status = CodecOkStatus();
  result.record = record;
  return result;
}

ClusterCatalogRecordCodecResult EncodeClusterCatalogRecord(
    const ClusterCatalogRecord& record) {
  const auto validated = ValidateClusterCatalogRecord(record);
  if (!validated.ok()) {
    return validated;
  }

  std::vector<byte> payload;
  if (!AppendSizedString(&payload, record.table_path)) {
    return CodecError("SB-CLUSTER-CATALOG-RECORD-LENGTH-INVALID",
                      "catalog.cluster_record_codec.length_invalid",
                      record.table_path);
  }
  AppendLittle32(&payload, static_cast<u32>(record.fields.size()));
  for (const ClusterCatalogFieldValue& field : record.fields) {
    if (!AppendSizedString(&payload, field.column_name) ||
        field.value.size() > kClusterCatalogRecordMaxPayloadBytes) {
      return CodecError("SB-CLUSTER-CATALOG-RECORD-LENGTH-INVALID",
                        "catalog.cluster_record_codec.length_invalid",
                        field.column_name);
    }
    AppendLittle32(&payload, static_cast<u32>(field.value.size()));
    payload.insert(payload.end(), field.value.begin(), field.value.end());
  }
  if (payload.size() > kClusterCatalogRecordMaxPayloadBytes) {
    return CodecError("SB-CLUSTER-CATALOG-RECORD-LENGTH-INVALID",
                      "catalog.cluster_record_codec.length_invalid",
                      record.table_path);
  }

  ClusterCatalogRecordCodecResult result;
  result.status = CodecOkStatus();
  result.record = record;
  result.encoded.reserve(kHeaderBytes + payload.size());
  result.encoded.insert(result.encoded.end(),
                        kClusterRecordMagic.begin(),
                        kClusterRecordMagic.end());
  AppendLittle32(&result.encoded, record.codec_version);
  AppendLittle32(&result.encoded, record.schema_version);
  AppendLittle32(&result.encoded, static_cast<u32>(record.fields.size()));
  AppendLittle32(&result.encoded, static_cast<u32>(payload.size()));
  AppendLittle64(&result.encoded, Fnv1a64(payload));
  result.encoded.insert(result.encoded.end(), payload.begin(), payload.end());
  return result;
}

ClusterCatalogRecordCodecResult DecodeClusterCatalogRecord(
    const std::vector<byte>& encoded) {
  if (encoded.size() < kHeaderBytes) {
    return CodecError("SB-CLUSTER-CATALOG-RECORD-SHORT",
                      "catalog.cluster_record_codec.short");
  }
  if (!std::equal(kClusterRecordMagic.begin(),
                  kClusterRecordMagic.end(),
                  encoded.begin() + kOffsetMagic)) {
    return CodecError("SB-CLUSTER-CATALOG-RECORD-MAGIC-INVALID",
                      "catalog.cluster_record_codec.magic_invalid");
  }

  ClusterCatalogRecord record;
  record.codec_version = LoadLittle32(encoded.data() + kOffsetCodecVersion);
  record.schema_version = LoadLittle32(encoded.data() + kOffsetSchemaVersion);
  const u32 header_field_count = LoadLittle32(encoded.data() + kOffsetFieldCount);
  const u32 payload_bytes = LoadLittle32(encoded.data() + kOffsetPayloadBytes);
  const u64 payload_checksum = LoadLittle64(encoded.data() + kOffsetPayloadChecksum);
  if (record.codec_version < kClusterCatalogRecordCodecVersionMinSupported ||
      record.codec_version > kClusterCatalogRecordCodecVersionMaxSupported) {
    return CodecError("SB-CLUSTER-CATALOG-RECORD-VERSION-UNSUPPORTED",
                      "catalog.cluster_record_codec.version_unsupported",
                      std::to_string(record.codec_version));
  }
  if (payload_bytes > kClusterCatalogRecordMaxPayloadBytes ||
      encoded.size() != kHeaderBytes + payload_bytes) {
    return CodecError("SB-CLUSTER-CATALOG-RECORD-LENGTH-MISMATCH",
                      "catalog.cluster_record_codec.length_mismatch");
  }

  std::vector<byte> payload(encoded.begin() + kHeaderBytes, encoded.end());
  if (payload_checksum != Fnv1a64(payload)) {
    return CodecError("SB-CLUSTER-CATALOG-RECORD-CHECKSUM-MISMATCH",
                      "catalog.cluster_record_codec.checksum_mismatch");
  }

  std::size_t offset = 0;
  if (!ReadSizedString(payload, &offset, &record.table_path)) {
    return CodecError("SB-CLUSTER-CATALOG-RECORD-LENGTH-MISMATCH",
                      "catalog.cluster_record_codec.length_mismatch",
                      "table_path");
  }
  u32 payload_field_count = 0;
  if (!ReadLittle32(payload, &offset, &payload_field_count) ||
      payload_field_count != header_field_count) {
    return CodecError("SB-CLUSTER-CATALOG-RECORD-FIELD-COUNT-MISMATCH",
                      "catalog.cluster_record_codec.field_count_mismatch",
                      record.table_path);
  }
  for (u32 i = 0; i < payload_field_count; ++i) {
    ClusterCatalogFieldValue field;
    if (!ReadSizedString(payload, &offset, &field.column_name)) {
      return CodecError("SB-CLUSTER-CATALOG-RECORD-LENGTH-MISMATCH",
                        "catalog.cluster_record_codec.length_mismatch",
                        "field_name");
    }
    u32 value_bytes = 0;
    if (!ReadLittle32(payload, &offset, &value_bytes) ||
        offset + value_bytes > payload.size()) {
      return CodecError("SB-CLUSTER-CATALOG-RECORD-LENGTH-MISMATCH",
                        "catalog.cluster_record_codec.length_mismatch",
                        field.column_name);
    }
    field.value.assign(reinterpret_cast<const char*>(payload.data() + offset),
                       value_bytes);
    offset += value_bytes;
    record.fields.push_back(std::move(field));
  }
  if (offset != payload.size()) {
    return CodecError("SB-CLUSTER-CATALOG-RECORD-TRAILING-BYTES",
                      "catalog.cluster_record_codec.trailing_bytes",
                      record.table_path);
  }

  auto result = EncodeClusterCatalogRecord(record);
  if (result.ok()) {
    result.encoded = encoded;
  }
  return result;
}

ClusterCatalogRecordCodecResult EncodeClusterCatalogRecordPageRow(
    const ClusterCatalogRecord& record,
    u32 ordinal) {
  auto result = EncodeClusterCatalogRecord(record);
  if (!result.ok()) {
    return result;
  }
  result.row.kind = CatalogPageRowKind::cluster_catalog_record;
  result.row.ordinal = ordinal;
  result.row.payload.assign(reinterpret_cast<const char*>(result.encoded.data()),
                            result.encoded.size());
  return result;
}

ClusterCatalogRecordCodecResult DecodeClusterCatalogRecordPageRow(
    const CatalogPageRow& row) {
  if (row.kind != CatalogPageRowKind::cluster_catalog_record) {
    return CodecError("SB-CLUSTER-CATALOG-RECORD-ROW-KIND-INVALID",
                      "catalog.cluster_record_codec.row_kind_invalid");
  }
  std::vector<byte> encoded(row.payload.begin(), row.payload.end());
  auto result = DecodeClusterCatalogRecord(encoded);
  if (result.ok()) {
    result.row = row;
  }
  return result;
}

ClusterCatalogRecordSetValidationResult ValidateClusterCatalogRecordSet(
    const ClusterCatalogRecordSet& record_set) {
  std::map<std::string, std::string> record_table_by_uuid;
  std::set<std::string> resolver_row_uuids;
  std::set<std::string> comment_uuids;

  for (const ClusterCatalogRecord& record : record_set.records) {
    const auto validated = ValidateClusterCatalogRecord(record);
    if (!validated.ok()) {
      ClusterCatalogRecordSetValidationResult result;
      result.status = validated.status;
      result.diagnostic = validated.diagnostic;
      return result;
    }
    const std::string primary_uuid = ClusterCatalogRecordPrimaryUuidValue(record);
    if (!record_table_by_uuid.emplace(primary_uuid, record.table_path).second) {
      return SetError("SB-CLUSTER-CATALOG-RECORD-UUID-REUSED",
                      "catalog.cluster_record_codec.uuid_reused",
                      primary_uuid);
    }
  }

  std::set<std::string> named_targets;
  for (const ClusterCatalogNameResolverRow& row :
       record_set.name_resolver_rows) {
    if (row.row_uuid.empty() || row.target_record_uuid.empty() ||
        row.target_table_path.empty() || row.language_tag.empty() ||
        row.identifier_profile_uuid.empty() || row.name_class.empty() ||
        row.raw_name_text.empty() || row.display_name.empty() ||
        row.normalized_lookup_key.empty() || row.exact_lookup_key.empty() ||
        row.full_path_lookup_key.empty() || row.catalog_generation == 0) {
      return SetError("SB-CLUSTER-CATALOG-RECORD-RESOLVER-INCOMPLETE",
                      "catalog.cluster_record_codec.resolver_incomplete",
                      row.target_record_uuid);
    }
    auto uuid_result = ValidateResolverUuid(row.row_uuid, UuidKind::row, row.row_uuid);
    if (!uuid_result.ok()) {
      return SetError(uuid_result.diagnostic.diagnostic_code,
                      uuid_result.diagnostic.message_key,
                      row.row_uuid);
    }
    uuid_result = ValidateResolverUuid(row.identifier_profile_uuid,
                                       UuidKind::object,
                                       row.identifier_profile_uuid);
    if (!uuid_result.ok()) {
      return SetError(uuid_result.diagnostic.diagnostic_code,
                      uuid_result.diagnostic.message_key,
                      row.identifier_profile_uuid);
    }
    if (!resolver_row_uuids.insert(row.row_uuid).second) {
      return SetError("SB-CLUSTER-CATALOG-RECORD-UUID-REUSED",
                      "catalog.cluster_record_codec.uuid_reused",
                      row.row_uuid);
    }
    const auto record_it = record_table_by_uuid.find(row.target_record_uuid);
    if (record_it == record_table_by_uuid.end() ||
        record_it->second != row.target_table_path) {
      return SetError("SB-CLUSTER-CATALOG-RECORD-RESOLVER-TARGET-INVALID",
                      "catalog.cluster_record_codec.resolver_target_invalid",
                      row.target_record_uuid);
    }
    named_targets.insert(row.target_record_uuid);
  }

  std::set<std::string> commented_targets;
  for (const ClusterCatalogCommentResolverRow& row :
       record_set.comment_resolver_rows) {
    if (row.row_uuid.empty() || row.comment_uuid.empty() ||
        row.target_record_uuid.empty() || row.target_table_path.empty() ||
        row.language_tag.empty() || row.comment_text.empty() ||
        row.catalog_generation == 0) {
      return SetError("SB-CLUSTER-CATALOG-RECORD-COMMENT-INCOMPLETE",
                      "catalog.cluster_record_codec.comment_incomplete",
                      row.target_record_uuid);
    }
    auto uuid_result = ValidateResolverUuid(row.row_uuid, UuidKind::row, row.row_uuid);
    if (!uuid_result.ok()) {
      return SetError(uuid_result.diagnostic.diagnostic_code,
                      uuid_result.diagnostic.message_key,
                      row.row_uuid);
    }
    uuid_result = ValidateResolverUuid(row.comment_uuid, UuidKind::object, row.comment_uuid);
    if (!uuid_result.ok()) {
      return SetError(uuid_result.diagnostic.diagnostic_code,
                      uuid_result.diagnostic.message_key,
                      row.comment_uuid);
    }
    if (!resolver_row_uuids.insert(row.row_uuid).second ||
        !comment_uuids.insert(row.comment_uuid).second) {
      return SetError("SB-CLUSTER-CATALOG-RECORD-UUID-REUSED",
                      "catalog.cluster_record_codec.uuid_reused",
                      row.row_uuid);
    }
    const auto record_it = record_table_by_uuid.find(row.target_record_uuid);
    if (record_it == record_table_by_uuid.end() ||
        record_it->second != row.target_table_path) {
      return SetError("SB-CLUSTER-CATALOG-RECORD-COMMENT-TARGET-INVALID",
                      "catalog.cluster_record_codec.comment_target_invalid",
                      row.target_record_uuid);
    }
    commented_targets.insert(row.target_record_uuid);
  }

  ClusterCatalogRecordSetValidationResult result;
  result.status = CodecOkStatus();
  for (const auto& [primary_uuid, ignored_table_path] : record_table_by_uuid) {
    if (named_targets.count(primary_uuid) == 0) {
      return SetError("SB-CLUSTER-CATALOG-RECORD-RESOLVER-MISSING",
                      "catalog.cluster_record_codec.resolver_missing",
                      primary_uuid);
    }
    if (commented_targets.count(primary_uuid) == 0) {
      return SetError("SB-CLUSTER-CATALOG-RECORD-COMMENT-MISSING",
                      "catalog.cluster_record_codec.comment_missing",
                      primary_uuid);
    }
    result.accepted_record_uuids.push_back(primary_uuid);
  }
  return result;
}

DiagnosticRecord MakeClusterCatalogRecordCodecDiagnostic(
    Status status,
    std::string diagnostic_code,
    std::string message_key,
    std::string detail) {
  std::vector<DiagnosticArgument> arguments;
  if (!detail.empty()) {
    arguments.push_back({"detail", detail});
  }

  return MakeDiagnostic(status.code,
                        status.severity,
                        status.subsystem,
                        std::move(diagnostic_code),
                        std::move(message_key),
                        std::move(arguments),
                        {},
                        "core.catalog.cluster_record_codec");
}

}  // namespace scratchbird::core::catalog
