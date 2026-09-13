// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "catalog_record_codec.hpp"

#include "uuid.hpp"
#include "hash_digest.hpp"

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

namespace {
constexpr std::size_t kMetadataHeaderBytes = 384;
constexpr std::size_t kMetadataMaxBytes = 262144;
using scratchbird::core::platform::LoadLittle64;
using scratchbird::core::platform::StoreLittle64;
using Metadata = CatalogMetadataVersion;
constexpr auto kMetadataReferences = std::array{
    &Metadata::owner_uuid, &Metadata::creator_transaction_uuid,
    &Metadata::retired_transaction_uuid, &Metadata::default_name_uuid,
    &Metadata::name_vector_uuid, &Metadata::security_policy_uuid,
    &Metadata::dependency_group_uuid, &Metadata::storage_binding_uuid,
    &Metadata::donor_overlay_uuid, &Metadata::audit_uuid, &Metadata::owning_schema_uuid};
constexpr auto kMetadataCounters = std::array{
    &Metadata::definition_version, &Metadata::schema_epoch, &Metadata::security_epoch,
    &Metadata::resource_epoch, &Metadata::catalog_generation,
    &Metadata::dependency_generation, &Metadata::invalidation_generation,
    &Metadata::creator_local_transaction_id};

CatalogMetadataVersionCodecResult MetadataError(const char* detail) {
  CatalogMetadataVersionCodecResult result;
  result.status = CodecErrorStatus();
  result.diagnostic = MakeCatalogRecordCodecDiagnostic(result.status,
      "CATALOG.INVALID_INPUT", "catalog.metadata_version.invalid", detail);
  return result;
}
template <typename Enum> bool MetadataEnum(Enum value, u16 maximum) {
  return static_cast<u16>(value) >= 1 && static_cast<u16>(value) <= maximum;
}
}

CatalogMetadataVersionCodecResult EncodeCatalogMetadataVersion(const CatalogMetadataVersion& value) {
  if (!MetadataEnum(value.authority_scope, 7) || !MetadataEnum(value.lifecycle, 10) ||
      !MetadataEnum(value.status, 9) || !MetadataEnum(value.visibility, 7))
    return MetadataError("enum_invalid");
  for (std::size_t i = 0; i < kMetadataCounters.size(); ++i)
    if (i != 3 && value.*kMetadataCounters[i] == 0) return MetadataError("counter_zero");
  const auto valid_key = [](const std::string& key, std::size_t maximum) {
    return !key.empty() && key.size() <= maximum && std::all_of(key.begin(), key.end(),
        [](unsigned char c) { return c >= 33 && c <= 126; });
  };
  if (!valid_key(value.trace_search_key, 4096) || !valid_key(value.object_subtype, 256) ||
      !valid_key(value.retention_class, 256))
    return MetadataError("trace_key_invalid");
  for (std::size_t i = 0; i < kMetadataReferences.size(); ++i) {
    const auto& id = value.*kMetadataReferences[i];
    if (!IsSuppliedIdentity(id)) {
      if (i == 0 || i == 1 || i == 9)
        return MetadataError("required_reference_missing");
    } else if (!scratchbird::core::uuid::MakeDurableEngineIdentityUuid(id.kind, id.value).ok())
      return MetadataError("reference_invalid");
  }
  if (!IsTypedIdentity(value.creator_transaction_uuid, UuidKind::transaction) ||
      (IsSuppliedIdentity(value.retired_transaction_uuid) &&
       !IsTypedIdentity(value.retired_transaction_uuid, UuidKind::transaction)))
    return MetadataError("transaction_reference_kind");
  if (IsSuppliedIdentity(value.default_name_uuid) != IsSuppliedIdentity(value.name_vector_uuid))
    return MetadataError("name_vector_pair_invalid");
  if (IsSuppliedIdentity(value.owning_schema_uuid) && !IsTypedIdentity(value.owning_schema_uuid, UuidKind::schema))
    return MetadataError("owning_schema_kind_invalid");
  for (auto member : {&Metadata::default_name_uuid, &Metadata::name_vector_uuid,
                     &Metadata::security_policy_uuid, &Metadata::dependency_group_uuid,
                     &Metadata::storage_binding_uuid, &Metadata::donor_overlay_uuid})
    if (IsSuppliedIdentity(value.*member) && !IsTypedIdentity(value.*member, UuidKind::object))
      return MetadataError("object_reference_kind_invalid");
  const bool retired = value.record.header.deleted;
  if ((IsSuppliedIdentity(value.retired_transaction_uuid) &&
       ((value.status != CatalogObjectStatus::retired && value.status != CatalogObjectStatus::quarantined) ||
        value.retired_transaction_uuid.value != value.creator_transaction_uuid.value)) ||
      (retired && (!IsSuppliedIdentity(value.retired_transaction_uuid) ||
                   value.lifecycle != CatalogObjectLifecycle::dropped ||
                   value.status != CatalogObjectStatus::retired ||
                   value.retired_transaction_uuid.value != value.creator_transaction_uuid.value)))
    return MetadataError("retirement_binding_invalid");
  const auto inner = EncodeCatalogTypedRecord(value.record, 0);
  if (!inner.ok()) {
    CatalogMetadataVersionCodecResult result;
    result.status = inner.status; result.diagnostic = inner.diagnostic; return result;
  }
  const auto inner_size = inner.row.payload.size();
  const auto text_size = value.trace_search_key.size() + value.object_subtype.size() + value.retention_class.size();
  if (inner_size > kMetadataMaxBytes - kMetadataHeaderBytes - text_size)
    return MetadataError("size_limit");
  CatalogMetadataVersionCodecResult result;
  result.bytes.assign(kMetadataHeaderBytes + text_size + inner_size, 0);
  auto* out = result.bytes.data();
  std::memcpy(out, "SBCMV001", 8);
  StoreLittle16(out + 8, 1); StoreLittle16(out + 10, kMetadataHeaderBytes);
  StoreLittle32(out + 12, static_cast<u32>(result.bytes.size()));
  StoreLittle16(out + 16, static_cast<u16>(value.authority_scope));
  StoreLittle16(out + 18, static_cast<u16>(value.lifecycle));
  StoreLittle16(out + 20, static_cast<u16>(value.status));
  StoreLittle16(out + 22, static_cast<u16>(value.visibility));
  for (std::size_t i = 0; i < kMetadataCounters.size(); ++i)
    StoreLittle64(out + 32 + 8 * i, value.*kMetadataCounters[i]);
  for (std::size_t i = 0; i < kMetadataReferences.size(); ++i) {
    const auto& id = value.*kMetadataReferences[i];
    std::copy(id.value.bytes.begin(), id.value.bytes.end(), out + 96 + 16 * i);
    out[272 + i] = static_cast<byte>(id.kind);
  }
  const auto definition = scratchbird::core::hash::ComputeSha256Digest(
      reinterpret_cast<const byte*>(inner.row.payload.data()), inner_size);
  if (!definition.ok()) {
    CatalogMetadataVersionCodecResult failure;
    failure.status = definition.status; failure.diagnostic = definition.diagnostic; return failure;
  }
  std::copy(definition.digest.begin(), definition.digest.end(), out + 288);
  StoreLittle32(out + 352, static_cast<u32>(value.trace_search_key.size()));
  StoreLittle32(out + 356, static_cast<u32>(inner_size));
  StoreLittle32(out + 360, static_cast<u32>(value.object_subtype.size()));
  StoreLittle32(out + 364, static_cast<u32>(value.retention_class.size()));
  std::copy(value.trace_search_key.begin(), value.trace_search_key.end(), out + kMetadataHeaderBytes);
  std::copy(value.object_subtype.begin(), value.object_subtype.end(), out + kMetadataHeaderBytes + value.trace_search_key.size());
  std::copy(value.retention_class.begin(), value.retention_class.end(), out + kMetadataHeaderBytes + value.trace_search_key.size() + value.object_subtype.size());
  std::copy(inner.row.payload.begin(), inner.row.payload.end(), out + kMetadataHeaderBytes + text_size);
  const auto digest = scratchbird::core::hash::ComputeSha256Digest(result.bytes);
  if (!digest.ok()) {
    CatalogMetadataVersionCodecResult failure;
    failure.status = digest.status; failure.diagnostic = digest.diagnostic; return failure;
  }
  std::copy(digest.digest.begin(), digest.digest.end(), out + 320);
  result.status = CodecOkStatus(); result.record = value; result.definition_sha256 = definition.digest;
  return result;
}

CatalogMetadataVersionCodecResult DecodeCatalogMetadataVersion(const std::vector<byte>& bytes) {
  if (bytes.size() < kMetadataHeaderBytes || bytes.size() > kMetadataMaxBytes)
    return MetadataError("size_invalid");
  const auto* in = bytes.data();
  if (std::memcmp(in, "SBCMV001", 8) || LoadLittle16(in + 8) != 1 ||
      LoadLittle16(in + 10) != kMetadataHeaderBytes || LoadLittle32(in + 12) != bytes.size())
    return MetadataError("header_invalid");
  const auto trace_size = LoadLittle32(in + 352), inner_size = LoadLittle32(in + 356);
  const auto subtype_size = LoadLittle32(in + 360), retention_size = LoadLittle32(in + 364);
  const auto text_size = static_cast<u64>(trace_size) + subtype_size + retention_size;
  if (trace_size > 4096 || subtype_size > 256 || retention_size > 256 ||
      static_cast<u64>(kMetadataHeaderBytes) + text_size + inner_size != bytes.size())
    return MetadataError("length_invalid");
  auto checksum_bytes = bytes;
  std::fill(checksum_bytes.begin() + 320, checksum_bytes.begin() + 352, 0);
  const auto digest = scratchbird::core::hash::ComputeSha256Digest(checksum_bytes);
  if (!digest.ok()) {
    CatalogMetadataVersionCodecResult failure;
    failure.status = digest.status; failure.diagnostic = digest.diagnostic; return failure;
  }
  if (!std::equal(digest.digest.begin(), digest.digest.end(), in + 320))
    return MetadataError("version_digest_mismatch");
  CatalogMetadataVersion value;
  value.authority_scope = static_cast<CatalogAuthorityScope>(LoadLittle16(in + 16));
  value.lifecycle = static_cast<CatalogObjectLifecycle>(LoadLittle16(in + 18));
  value.status = static_cast<CatalogObjectStatus>(LoadLittle16(in + 20));
  value.visibility = static_cast<CatalogVisibilityClass>(LoadLittle16(in + 22));
  for (std::size_t i = 0; i < kMetadataCounters.size(); ++i)
    value.*kMetadataCounters[i] = LoadLittle64(in + 32 + 8 * i);
  for (std::size_t i = 0; i < kMetadataReferences.size(); ++i) {
    auto& id = value.*kMetadataReferences[i]; id.kind = static_cast<UuidKind>(in[272 + i]);
    std::copy(in + 96 + 16 * i, in + 112 + 16 * i, id.value.bytes.begin());
  }
  value.trace_search_key.assign(reinterpret_cast<const char*>(in + kMetadataHeaderBytes), trace_size);
  value.object_subtype.assign(reinterpret_cast<const char*>(in + kMetadataHeaderBytes + trace_size), subtype_size);
  value.retention_class.assign(reinterpret_cast<const char*>(in + kMetadataHeaderBytes + trace_size + subtype_size), retention_size);
  CatalogPageRow row;
  row.kind = CatalogPageRowKind::typed_catalog_record;
  row.payload.assign(reinterpret_cast<const char*>(in + kMetadataHeaderBytes + text_size), inner_size);
  const auto inner = DecodeCatalogTypedRecord(row);
  if (!inner.ok()) {
    CatalogMetadataVersionCodecResult result;
    result.status = inner.status; result.diagnostic = inner.diagnostic; return result;
  }
  value.record = inner.record;
  auto result = EncodeCatalogMetadataVersion(value);
  // Re-encoding checks both digests, reserved bytes and canonical framing.
  if (!result.ok()) return result;
  if (result.bytes != bytes) return MetadataError("digest_reserved_or_canonical_mismatch");
  return result;
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
