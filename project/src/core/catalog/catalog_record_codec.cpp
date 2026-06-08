// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "catalog_record_codec.hpp"

#include "uuid.hpp"

#include <cstdlib>
#include <map>
#include <sstream>
#include <utility>
#include <vector>

namespace scratchbird::core::catalog {
namespace {

using scratchbird::core::platform::DiagnosticArgument;
using scratchbird::core::platform::MakeDiagnostic;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;
using scratchbird::core::platform::UuidKind;
using scratchbird::core::uuid::IsEngineIdentityUuid;
using scratchbird::core::uuid::ParseTypedUuid;
using scratchbird::core::uuid::UuidToString;
using scratchbird::storage::page::CatalogPageRowKind;

Status CodecOkStatus() {
  return {StatusCode::ok, Severity::info, Subsystem::catalog};
}

Status CodecErrorStatus() {
  return {StatusCode::platform_required_feature_missing, Severity::error, Subsystem::catalog};
}

CatalogRecordCodecResult CodecError(std::string diagnostic_code,
                                    std::string message_key,
                                    std::string detail = {}) {
  CatalogRecordCodecResult result;
  result.status = CodecErrorStatus();
  result.diagnostic = MakeCatalogRecordCodecDiagnostic(result.status,
                                                       std::move(diagnostic_code),
                                                       std::move(message_key),
                                                       std::move(detail));
  return result;
}

std::string Escape(std::string value) {
  for (char& ch : value) {
    if (ch == '\n' || ch == '\r') {
      ch = ' ';
    }
  }
  return value;
}

std::string Field(std::string key, std::string value) {
  return std::move(key) + "=" + Escape(std::move(value)) + "\n";
}

std::map<std::string, std::string> ParseFields(const std::string& payload) {
  std::map<std::string, std::string> fields;
  std::stringstream stream(payload);
  std::string line;
  while (std::getline(stream, line)) {
    const std::size_t pos = line.find('=');
    if (pos == std::string::npos) {
      continue;
    }
    fields[line.substr(0, pos)] = line.substr(pos + 1);
  }
  return fields;
}

bool IsTypedIdentity(const scratchbird::core::platform::TypedUuid& uuid, UuidKind expected) {
  return uuid.kind == expected && uuid.valid() && IsEngineIdentityUuid(uuid.value);
}

std::string MaybeUuidString(const scratchbird::core::platform::TypedUuid& uuid) {
  return uuid.valid() ? UuidToString(uuid.value) : "";
}

CatalogRecordKind ParseKind(const std::string& value) {
  return static_cast<CatalogRecordKind>(std::strtoul(value.c_str(), nullptr, 10));
}

}  // namespace

CatalogRecordCodecResult EncodeCatalogTypedRecord(const CatalogTypedRecord& record, u32 ordinal) {
  const auto descriptor = LookupCatalogRecordDescriptor(record.header.kind);
  if (!descriptor.ok()) {
    CatalogRecordCodecResult result;
    result.status = descriptor.status;
    result.diagnostic = descriptor.diagnostic;
    return result;
  }
  if (record.header.record_version < kCatalogRecordSchemaVersionMinSupported ||
      record.header.record_version > kCatalogRecordSchemaVersionMaxSupported) {
    return CodecError("SB-CATALOG-RECORD-CODEC-VERSION-UNSUPPORTED",
                      "catalog.record_codec.version_unsupported",
                      CatalogRecordKindName(record.header.kind));
  }
  if (descriptor.descriptor.requires_row_uuid && !IsTypedIdentity(record.header.row_uuid, UuidKind::row)) {
    return CodecError("SB-CATALOG-RECORD-CODEC-ROW-UUID-MUST-BE-V7",
                      "catalog.record_codec.row_uuid_must_be_v7",
                      CatalogRecordKindName(record.header.kind));
  }
  if (descriptor.descriptor.requires_object_uuid && !IsTypedIdentity(record.header.object_uuid, UuidKind::object)) {
    return CodecError("SB-CATALOG-RECORD-CODEC-OBJECT-UUID-MUST-BE-V7",
                      "catalog.record_codec.object_uuid_must_be_v7",
                      CatalogRecordKindName(record.header.kind));
  }
  if (descriptor.descriptor.requires_parent_uuid && record.header.parent_uuid.valid() &&
      !IsEngineIdentityUuid(record.header.parent_uuid.value)) {
    return CodecError("SB-CATALOG-RECORD-CODEC-PARENT-UUID-MUST-BE-V7",
                      "catalog.record_codec.parent_uuid_must_be_v7",
                      CatalogRecordKindName(record.header.kind));
  }
  if (descriptor.descriptor.requires_parent_uuid && !record.header.parent_uuid.valid()) {
    return CodecError("SB-CATALOG-RECORD-CODEC-PARENT-UUID-REQUIRED",
                      "catalog.record_codec.parent_uuid_required",
                      CatalogRecordKindName(record.header.kind));
  }

  CatalogRecordCodecResult result;
  result.status = CodecOkStatus();
  result.record = record;
  result.row.kind = CatalogPageRowKind::typed_catalog_record;
  result.row.ordinal = ordinal;
  result.row.payload = Field("kind", std::to_string(static_cast<u16>(record.header.kind))) +
                       Field("record_version", std::to_string(record.header.record_version)) +
                       Field("deleted", record.header.deleted ? "1" : "0") +
                       Field("row_uuid", MaybeUuidString(record.header.row_uuid)) +
                       Field("object_uuid", MaybeUuidString(record.header.object_uuid)) +
                       Field("parent_uuid", MaybeUuidString(record.header.parent_uuid)) +
                       Field("payload", record.payload);
  return result;
}

CatalogRecordCodecResult DecodeCatalogTypedRecord(const CatalogPageRow& row) {
  if (row.kind != CatalogPageRowKind::typed_catalog_record) {
    return CodecError("SB-CATALOG-RECORD-CODEC-ROW-KIND-INVALID",
                      "catalog.record_codec.row_kind_invalid");
  }
  const auto fields = ParseFields(row.payload);
  if (fields.count("kind") == 0 || fields.count("row_uuid") == 0) {
    return CodecError("SB-CATALOG-RECORD-CODEC-FIELDS-MISSING",
                      "catalog.record_codec.fields_missing");
  }

  CatalogTypedRecord record;
  record.header.kind = ParseKind(fields.at("kind"));
  record.header.record_version = fields.count("record_version") == 0
                                     ? 1
                                     : static_cast<u32>(std::strtoul(fields.at("record_version").c_str(), nullptr, 10));
  record.header.deleted = fields.count("deleted") != 0 && fields.at("deleted") == "1";

  const auto row_uuid = ParseTypedUuid(UuidKind::row, fields.at("row_uuid"));
  if (!row_uuid.ok()) {
    CatalogRecordCodecResult result;
    result.status = row_uuid.status;
    result.diagnostic = row_uuid.diagnostic;
    return result;
  }
  record.header.row_uuid = row_uuid.value;

  if (fields.count("object_uuid") != 0 && !fields.at("object_uuid").empty()) {
    const auto object_uuid = ParseTypedUuid(UuidKind::object, fields.at("object_uuid"));
    if (!object_uuid.ok()) {
      CatalogRecordCodecResult result;
      result.status = object_uuid.status;
      result.diagnostic = object_uuid.diagnostic;
      return result;
    }
    record.header.object_uuid = object_uuid.value;
  }
  if (fields.count("parent_uuid") != 0 && !fields.at("parent_uuid").empty()) {
    const auto parent_uuid = ParseTypedUuid(UuidKind::object, fields.at("parent_uuid"));
    if (!parent_uuid.ok()) {
      CatalogRecordCodecResult result;
      result.status = parent_uuid.status;
      result.diagnostic = parent_uuid.diagnostic;
      return result;
    }
    record.header.parent_uuid = parent_uuid.value;
  }
  record.payload = fields.count("payload") == 0 ? "" : fields.at("payload");

  return EncodeCatalogTypedRecord(record, row.ordinal);
}

DiagnosticRecord MakeCatalogRecordCodecDiagnostic(Status status,
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
                        "core.catalog.record_codec");
}

}  // namespace scratchbird::core::catalog
