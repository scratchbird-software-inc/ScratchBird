// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "catalog_record_codec.hpp"

#include "uuid.hpp"

#include <algorithm>
#include <cstring>
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
using scratchbird::core::platform::byte;
using scratchbird::core::platform::LoadLittle16;
using scratchbird::core::platform::LoadLittle32;
using scratchbird::core::platform::StoreLittle16;
using scratchbird::core::platform::StoreLittle32;
constexpr std::size_t kBinaryHeaderBytes = 96;
constexpr std::size_t kMaxBinaryRecordBytes = 131072;
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

bool IsTypedIdentity(const scratchbird::core::platform::TypedUuid& uuid, UuidKind expected) {
  return uuid.kind == expected && uuid.valid() && IsEngineIdentityUuid(uuid.value);
}

bool IsSuppliedIdentity(const scratchbird::core::platform::TypedUuid& uuid) {
  // Only the default pair denotes absence. A malformed supplied reference
  // must not vanish behind TypedUuid::valid() during catalog serialization.
  return uuid.kind != UuidKind::unknown || !uuid.value.is_nil();
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
  if ((descriptor.descriptor.requires_row_uuid || IsSuppliedIdentity(record.header.row_uuid)) &&
      !IsTypedIdentity(record.header.row_uuid, UuidKind::row)) {
    return CodecError("SB-CATALOG-RECORD-CODEC-ROW-UUID-MUST-BE-V7",
                      "catalog.record_codec.row_uuid_must_be_v7",
                      CatalogRecordKindName(record.header.kind));
  }
  if ((descriptor.descriptor.requires_object_uuid || IsSuppliedIdentity(record.header.object_uuid)) &&
      !IsTypedIdentity(record.header.object_uuid, UuidKind::object)) {
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

  if (IsSuppliedIdentity(record.header.parent_uuid)) {
    const auto parent = scratchbird::core::uuid::MakeDurableEngineIdentityUuid(
        record.header.parent_uuid.kind, record.header.parent_uuid.value);
    if (!parent.ok()) {
      CatalogRecordCodecResult refused;
      refused.status = parent.status;
      refused.diagnostic = parent.diagnostic;
      return refused;
    }
  }

  if (record.payload.size() > kMaxBinaryRecordBytes - kBinaryHeaderBytes) {
    return CodecError("SB-CATALOG-RECORD-CODEC-FIELDS-MISSING",
                      "catalog.record_codec.fields_missing", "binary_record_size_limit");
  }
  CatalogRecordCodecResult result;
  result.status = CodecOkStatus();
  result.record = record;
  result.row.kind = CatalogPageRowKind::typed_catalog_record;
  result.row.ordinal = ordinal;
  result.row.payload.assign(kBinaryHeaderBytes + record.payload.size(), '\0');
  auto* bytes = reinterpret_cast<byte*>(result.row.payload.data());
  std::memcpy(bytes, "SBCTREC2", 8);
  StoreLittle16(bytes + 8, 2);
  StoreLittle16(bytes + 10, kBinaryHeaderBytes);
  StoreLittle32(bytes + 12, static_cast<u32>(result.row.payload.size()));
  StoreLittle16(bytes + 16, static_cast<u16>(record.header.kind));
  StoreLittle32(bytes + 20, record.header.record_version);
  StoreLittle32(bytes + 24, record.header.deleted ? 1 : 0);
  StoreLittle32(bytes + 28, static_cast<u32>(record.payload.size()));
  const scratchbird::core::platform::TypedUuid* identities[] = {
      &record.header.row_uuid, &record.header.object_uuid, &record.header.parent_uuid};
  for (std::size_t i = 0; i < 3; ++i) {
    bytes[32 + i] = static_cast<byte>(identities[i]->kind);
    const auto& id = identities[i]->value.bytes;
    std::copy(id.begin(), id.end(), bytes + 40 + i * 16);
  }
  std::copy(record.payload.begin(), record.payload.end(),
            result.row.payload.begin() + kBinaryHeaderBytes);
  return result;
}

CatalogRecordCodecResult DecodeCatalogTypedRecord(const CatalogPageRow& row) {
  if (row.kind != CatalogPageRowKind::typed_catalog_record) {
    return CodecError("SB-CATALOG-RECORD-CODEC-ROW-KIND-INVALID",
                      "catalog.record_codec.row_kind_invalid");
  }
  const auto malformed = [](const char* detail) {
    return CodecError("SB-CATALOG-RECORD-CODEC-FIELDS-MISSING",
                      "catalog.record_codec.fields_missing", detail);
  };
  if (row.payload.size() < kBinaryHeaderBytes || row.payload.size() > kMaxBinaryRecordBytes) {
    return malformed("binary_record_size_invalid");
  }
  const auto* bytes = reinterpret_cast<const byte*>(row.payload.data());
  if (std::memcmp(bytes, "SBCTREC2", 8) != 0 || LoadLittle16(bytes + 8) != 2 ||
      LoadLittle16(bytes + 10) != kBinaryHeaderBytes) {
    return CodecError("SB-CATALOG-RECORD-CODEC-VERSION-UNSUPPORTED",
                      "catalog.record_codec.version_unsupported", "binary_header_required");
  }
  if (LoadLittle32(bytes + 12) != row.payload.size() ||
      LoadLittle32(bytes + 28) != row.payload.size() - kBinaryHeaderBytes ||
      LoadLittle16(bytes + 18) != 0 || (LoadLittle32(bytes + 24) & ~1u) != 0 ||
      !std::all_of(bytes + 35, bytes + 40, [](byte value) { return value == 0; }) ||
      !std::all_of(bytes + 88, bytes + 96, [](byte value) { return value == 0; })) {
    return malformed("binary_lengths_flags_or_reserved_invalid");
  }
  CatalogTypedRecord record;
  record.header.kind = static_cast<CatalogRecordKind>(LoadLittle16(bytes + 16));
  record.header.record_version = LoadLittle32(bytes + 20);
  record.header.deleted = LoadLittle32(bytes + 24) != 0;
  scratchbird::core::platform::TypedUuid* identities[] = {
      &record.header.row_uuid, &record.header.object_uuid, &record.header.parent_uuid};
  for (std::size_t i = 0; i < 3; ++i) {
    identities[i]->kind = static_cast<UuidKind>(bytes[32 + i]);
    std::copy(bytes + 40 + i * 16, bytes + 56 + i * 16, identities[i]->value.bytes.begin());
  }
  record.payload.assign(row.payload.data() + kBinaryHeaderBytes,
                        row.payload.size() - kBinaryHeaderBytes);
  // The shared admission path enforces descriptor requiredness and exact UUID
  // kind/version/variant policy before publishing any decoded authority.
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
