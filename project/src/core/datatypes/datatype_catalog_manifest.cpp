// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "datatype_catalog_manifest.hpp"

#include <algorithm>
#include <array>
#include <set>
#include <utility>

namespace scratchbird::core::datatypes {
namespace {

using scratchbird::core::platform::DiagnosticArgument;
using scratchbird::core::platform::MakeDiagnostic;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;
using scratchbird::core::platform::UuidKind;
using scratchbird::core::platform::byte;

Status CatalogOkStatus() {
  return {StatusCode::ok, Severity::info, Subsystem::datatypes};
}

Status CatalogErrorStatus() {
  return {StatusCode::platform_required_feature_missing,
          Severity::error,
          Subsystem::datatypes};
}

DiagnosticRecord MakeCatalogDiagnostic(Status status,
                                       std::string diagnostic_code,
                                       std::string message_key,
                                       std::string detail = {}) {
  std::vector<DiagnosticArgument> arguments;
  if (!detail.empty()) {
    arguments.push_back({"detail", std::move(detail)});
  }
  return MakeDiagnostic(status.code,
                        status.severity,
                        status.subsystem,
                        std::move(diagnostic_code),
                        std::move(message_key),
                        std::move(arguments),
                        {},
                        "core.datatypes.catalog_manifest");
}

void AddFailure(DatatypeCatalogManifestResult* result,
                std::string diagnostic_code,
                std::string message_key,
                std::string detail = {}) {
  result->status = CatalogErrorStatus();
  DiagnosticRecord diagnostic = MakeCatalogDiagnostic(
      result->status, std::move(diagnostic_code), std::move(message_key),
      std::move(detail));
  if (result->diagnostics.empty()) {
    result->diagnostic = diagnostic;
  }
  result->diagnostics.push_back(std::move(diagnostic));
}

bool HasTraceRow(const DatatypeCatalogManifest& manifest,
                 CanonicalTypeId type_id) {
  for (const DatatypeImplementationTraceRow& row : manifest.trace_rows) {
    if (row.type_id == type_id) {
      return true;
    }
  }
  return false;
}

bool HasLayoutRow(const DatatypeCatalogManifest& manifest,
                  CanonicalTypeId type_id) {
  for (const DatatypeCatalogLayoutRow& row : manifest.layout_rows) {
    if (row.type_id == type_id) {
      return true;
    }
  }
  return false;
}

bool TypedUuidEquals(const TypedUuid& left, const TypedUuid& right) {
  return left.kind == right.kind && left.value == right.value;
}

TypedUuid AuthoritativeDatatypeDescriptorUuid(const CanonicalTypeId type_id,
                                              const std::string& stable_name) {
  if (type_id == CanonicalTypeId::int32) {
    TypedUuid uuid;
    uuid.kind = UuidKind::object;
    uuid.value.bytes = {0x01, 0x9d, 0x00, 0x00, 0x00, 0x00, 0x70, 0x00,
                        0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0xd7, 0x16};
    return uuid;
  }
  if (type_id == CanonicalTypeId::int64) {
    TypedUuid uuid;
    uuid.kind = UuidKind::object;
    uuid.value.bytes = {0x01, 0x9d, 0x00, 0x00, 0x00, 0x00, 0x70, 0x00,
                        0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0xd7, 0x11};
    return uuid;
  }
  if (type_id == CanonicalTypeId::int128) {
    TypedUuid uuid;
    uuid.kind = UuidKind::object;
    uuid.value.bytes = {0x01, 0x9d, 0x00, 0x00, 0x00, 0x00, 0x70, 0x00,
                        0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0xd7, 0x14};
    return uuid;
  }
  if (type_id == CanonicalTypeId::character) {
    TypedUuid uuid;
    uuid.kind = UuidKind::object;
    uuid.value.bytes = {0x01, 0x9d, 0x00, 0x00, 0x00, 0x00, 0x70, 0x00,
                        0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0xd7, 0x18};
    return uuid;
  }
  return StableDatatypeDescriptorUuid(type_id, stable_name);
}

}  // namespace

TypedUuid StableDatatypeDescriptorUuid(CanonicalTypeId type_id,
                                       const std::string& stable_name) {
  TypedUuid uuid;
  uuid.kind = UuidKind::object;
  const u32 value = static_cast<u32>(type_id);
  uuid.value.bytes[0] = static_cast<byte>(value & 0xffu);
  uuid.value.bytes[1] = static_cast<byte>((value >> 8) & 0xffu);
  uuid.value.bytes[2] = static_cast<byte>((value >> 16) & 0xffu);
  uuid.value.bytes[3] = static_cast<byte>((value >> 24) & 0xffu);
  for (std::size_t index = 0; index < stable_name.size(); ++index) {
    uuid.value.bytes[(index % 12) + 4] ^=
        static_cast<byte>(stable_name[index]);
  }
  uuid.value.bytes[6] =
      static_cast<byte>((uuid.value.bytes[6] & 0x0fu) | 0x70u);
  uuid.value.bytes[8] =
      static_cast<byte>((uuid.value.bytes[8] & 0x3fu) | 0x80u);
  if (uuid.value.is_nil()) {
    uuid.value.bytes[0] = 1;
  }
  return uuid;
}

DatatypeCatalogManifestResult LoadCurrentCoreDatatypeCatalogManifest() {
  DatatypeCatalogManifestResult result;
  result.status = CatalogOkStatus();
  result.manifest.manifest_key = kCurrentCoreDatatypeCatalogManifestKey;
  result.manifest.catalog_epoch = 1;

  for (const DatatypeDescriptor& descriptor : BuiltinDatatypeDescriptors()) {
    const auto validated = ValidateDatatypeDescriptor(descriptor);
    if (!validated.ok()) {
      AddFailure(&result,
                 "SB-DATATYPE-CATALOG-DESCRIPTOR-INVALID",
                 "datatype.catalog.descriptor_invalid",
                 descriptor.stable_name);
      continue;
    }
    const auto layout = LookupDatatypeStorageLayout(descriptor.type_id);
    if (!layout.ok()) {
      AddFailure(&result,
                 "SB-DATATYPE-CATALOG-LAYOUT-ROW-MISSING",
                 "datatype.catalog.layout_row_missing",
                 descriptor.stable_name);
      continue;
    }

    DatatypeCatalogDescriptorRow descriptor_row;
    descriptor_row.descriptor_uuid = AuthoritativeDatatypeDescriptorUuid(
        descriptor.type_id, descriptor.stable_name);
    descriptor_row.type_id = descriptor.type_id;
    descriptor_row.family = descriptor.family;
    descriptor_row.stable_name = descriptor.stable_name;
    descriptor_row.sys_table_name = "sys.datatype_descriptor";
    descriptor_row.descriptor_epoch = 1;
    descriptor_row.descriptor_authoritative =
        descriptor.descriptor_authoritative;
    descriptor_row.reference_name_is_alias_only =
        descriptor.reference_name_is_alias_only;
    result.manifest.descriptor_rows.push_back(descriptor_row);

    DatatypeCatalogLayoutRow layout_row;
    layout_row.type_id = layout.layout.type_id;
    layout_row.sys_table_name = "sys.datatype_storage_layout";
    layout_row.storage_class = layout.layout.storage_class;
    layout_row.encoding = layout.layout.encoding;
    layout_row.inline_bytes = layout.layout.inline_bytes;
    layout_row.may_overflow_to_toast = layout.layout.may_overflow_to_toast;
    result.manifest.layout_rows.push_back(layout_row);

    DatatypeImplementationTraceRow trace_row;
    trace_row.type_id = descriptor.type_id;
    trace_row.family = descriptor.family;
    trace_row.trace_key = std::string("DEFER-DTYPE-CLOSURE-MATRIX-TRACE:") +
                          descriptor.stable_name;
    trace_row.implementation_source_path =
        "project/src/core/datatypes/datatype_descriptor.cpp";
    trace_row.descriptor_api_exercised = true;
    trace_row.layout_api_exercised = true;
    trace_row.parser_spelling_is_authority = false;
    result.manifest.trace_rows.push_back(trace_row);
  }

  return result;
}

DatatypeCatalogManifestResult ValidateDatatypeCatalogManifest(
    const DatatypeCatalogManifest& manifest) {
  DatatypeCatalogManifestResult result;
  result.status = CatalogOkStatus();
  result.manifest = manifest;

  if (manifest.manifest_key != kCurrentCoreDatatypeCatalogManifestKey) {
    AddFailure(&result,
               "SB-DATATYPE-CATALOG-MANIFEST-UNKNOWN",
               "datatype.catalog.manifest_unknown",
               manifest.manifest_key);
  }
  if (manifest.catalog_epoch == 0) {
    AddFailure(&result,
               "SB-DATATYPE-CATALOG-EPOCH-MISSING",
               "datatype.catalog.epoch_missing",
               manifest.manifest_key);
  }

  std::set<CanonicalTypeId> seen_descriptors;
  for (const DatatypeCatalogDescriptorRow& row : manifest.descriptor_rows) {
    if (!row.descriptor_uuid.valid() || row.descriptor_epoch == 0 ||
        row.sys_table_name != "sys.datatype_descriptor") {
      AddFailure(&result,
                 "SB-DATATYPE-CATALOG-DESCRIPTOR-ROW-INCOMPLETE",
                 "datatype.catalog.descriptor_row_incomplete",
                 row.stable_name);
      continue;
    }
    if (!row.descriptor_authoritative || !row.reference_name_is_alias_only) {
      AddFailure(&result,
                 "SB-DATATYPE-CATALOG-AUTHORITY-VIOLATION",
                 "datatype.catalog.authority_violation",
                 row.stable_name);
      continue;
    }
    if (!seen_descriptors.insert(row.type_id).second) {
      AddFailure(&result,
                 "SB-DATATYPE-CATALOG-DUPLICATE-DESCRIPTOR-ROW",
                 "datatype.catalog.duplicate_descriptor_row",
                 row.stable_name);
      continue;
    }
    const auto descriptor = LookupDatatypeDescriptor(row.type_id);
    if (!descriptor.ok() ||
        descriptor.descriptor.stable_name != row.stable_name ||
        descriptor.descriptor.family != row.family) {
      AddFailure(&result,
                 "SB-DATATYPE-CATALOG-DESCRIPTOR-ROW-MISMATCH",
                 "datatype.catalog.descriptor_row_mismatch",
                 row.stable_name);
      continue;
    }
    const auto expected_uuid =
        AuthoritativeDatatypeDescriptorUuid(row.type_id, row.stable_name);
    if (!TypedUuidEquals(row.descriptor_uuid, expected_uuid)) {
      AddFailure(&result,
                 "SB-DATATYPE-CATALOG-UUID-STABILITY-VIOLATION",
                 "datatype.catalog.uuid_stability_violation",
                 row.stable_name);
    }
  }

  for (const DatatypeDescriptor& descriptor : BuiltinDatatypeDescriptors()) {
    if (seen_descriptors.find(descriptor.type_id) == seen_descriptors.end()) {
      AddFailure(&result,
                 "SB-DATATYPE-CATALOG-DESCRIPTOR-ROW-MISSING",
                 "datatype.catalog.descriptor_row_missing",
                 descriptor.stable_name);
    }
    if (!HasLayoutRow(manifest, descriptor.type_id)) {
      AddFailure(&result,
                 "SB-DATATYPE-CATALOG-LAYOUT-ROW-MISSING",
                 "datatype.catalog.layout_row_missing",
                 descriptor.stable_name);
    }
    if (!HasTraceRow(manifest, descriptor.type_id)) {
      AddFailure(&result,
                 "SB-DATATYPE-CATALOG-TRACE-ROW-MISSING",
                 "datatype.catalog.trace_row_missing",
                 descriptor.stable_name);
    }
  }

  for (const DatatypeCatalogLayoutRow& row : manifest.layout_rows) {
    if (row.sys_table_name != "sys.datatype_storage_layout") {
      AddFailure(&result,
                 "SB-DATATYPE-CATALOG-LAYOUT-ROW-INCOMPLETE",
                 "datatype.catalog.layout_row_incomplete",
                 CanonicalTypeName(row.type_id));
    }
  }

  for (const DatatypeImplementationTraceRow& row : manifest.trace_rows) {
    if (row.trace_key.find("DEFER-DTYPE-CLOSURE-MATRIX-TRACE") != 0 ||
        row.implementation_source_path.rfind("project/src/core/datatypes/", 0) !=
            0 ||
        !row.descriptor_api_exercised || !row.layout_api_exercised ||
        row.parser_spelling_is_authority) {
      AddFailure(&result,
                 "SB-DATATYPE-CATALOG-TRACE-ROW-INCOMPLETE",
                 "datatype.catalog.trace_row_incomplete",
                 CanonicalTypeName(row.type_id));
    }
  }

  return result;
}

DatatypeCatalogManifestResult LookupDatatypeCatalogRow(
    const DatatypeCatalogManifest& manifest,
    CanonicalTypeId type_id) {
  DatatypeCatalogManifestResult validation =
      ValidateDatatypeCatalogManifest(manifest);
  if (!validation.ok()) {
    return validation;
  }
  for (const DatatypeCatalogDescriptorRow& row : manifest.descriptor_rows) {
    if (row.type_id == type_id) {
      DatatypeCatalogManifestResult result;
      result.status = CatalogOkStatus();
      result.manifest = manifest;
      result.manifest.descriptor_rows = {row};
      return result;
    }
  }
  DatatypeCatalogManifestResult result;
  result.status = CatalogErrorStatus();
  result.manifest = manifest;
  result.diagnostic = MakeCatalogDiagnostic(
      result.status,
      "SB-DATATYPE-CATALOG-UNSUPPORTED-CANONICAL-TYPE",
      "datatype.catalog.unsupported_canonical_type",
      CanonicalTypeName(type_id));
  result.diagnostics.push_back(result.diagnostic);
  return result;
}

DatatypeCatalogManifestResult DatatypeCatalogCache::Load(
    DatatypeCatalogManifest manifest) {
  DatatypeCatalogManifestResult validation =
      ValidateDatatypeCatalogManifest(manifest);
  if (!validation.ok()) {
    return validation;
  }
  manifest_ = std::move(manifest);
  valid_ = true;
  ++generation_;
  validation.manifest = manifest_;
  return validation;
}

void DatatypeCatalogCache::Invalidate() {
  valid_ = false;
  ++generation_;
}

DatatypeCatalogManifestResult DatatypeCatalogCache::Lookup(
    CanonicalTypeId type_id) const {
  if (!valid_) {
    DatatypeCatalogManifestResult result;
    result.status = CatalogErrorStatus();
    result.diagnostic = MakeCatalogDiagnostic(
        result.status,
        "SB-DATATYPE-CATALOG-CACHE-INVALIDATED",
        "datatype.catalog.cache_invalidated",
        CanonicalTypeName(type_id));
    result.diagnostics.push_back(result.diagnostic);
    return result;
  }
  return LookupDatatypeCatalogRow(manifest_, type_id);
}

DatatypeTypeCodecIdentityLookupV1 LookupDatatypeTypeCodecIdentityV1(
    const std::string& catalog_snapshot_uuid,
    u64 catalog_generation,
    u64 registry_generation,
    const std::string& descriptor_uuid,
    u64 descriptor_generation) {
  // DATATYPE-TYPE-CODEC-IDENTITY-REGISTRY-V1 is a manifest-admitted exact
  // identity table.  This lookup deliberately does not accept a stable name,
  // CanonicalTypeId, or caller-selected codec.
  static const std::array<DatatypeTypeCodecIdentityRowV1, 6> rows{{
      {"019d0000-0000-7000-8000-00000000d701", 1, 1,
       "019d0000-0000-7000-8000-00000000d716", 1,
       "019d0000-0000-7000-8000-00000000d717", 1,
       "datatype.int32.le.v1", 1, 1, 4, true,
       "int32", 2, 1, 2, true, 2, 4, 4, 4,
       static_cast<u32>(CanonicalTypeId::int32)},
      {"019d0000-0000-7000-8000-00000000d701", 1, 1,
       "019d0000-0000-7000-8000-00000000d711", 1,
       "019d0000-0000-7000-8000-00000000d712", 1,
       "datatype.int64.le.v1", 1, 1, 8, false,
       "bigint", 3, 2, 2, true, 2, 8, 8, 8,
       static_cast<u32>(CanonicalTypeId::int64)},
      {"019d0000-0000-7000-8000-00000000d701", 1, 1,
       "a0000000-6465-7369-ad61-6c0000000000", 1,
       "019d0000-0000-7000-8000-00000000d713", 1,
       "datatype.decimal.base1e9.le.v1", 1, 1, 24, false,
       "decimal", 4, 2, 2, true, 3, 24, 24, 24,
       static_cast<u32>(CanonicalTypeId::decimal)},
      {"019d0000-0000-7000-8000-00000000d701", 1, 1,
       "019d0000-0000-7000-8000-00000000d714", 1,
       "019d0000-0000-7000-8000-00000000d715", 1,
       "datatype.int128.le.v1", 1, 1, 16, true,
       "int128", 5, 1, 2, true, 2, 16, 16, 16,
       static_cast<u32>(CanonicalTypeId::int128)},
      {"019d0000-0000-7000-8000-00000000d701", 1, 1,
       "01000000-626f-7f6c-a561-6e0000000000", 1,
       "01000000-626f-7f6c-a561-6e0000000000", 1,
       "datatype.boolean.u8.v1", 1, 1, 1, true,
       "boolean", 1, 1, 1, false, 1, 1, 1, 1,
       static_cast<u32>(CanonicalTypeId::boolean)},
      {"019d0000-0000-7000-8000-00000000d701", 1, 1,
       "019d0000-0000-7000-8000-00000000d718", 1,
       "019d0000-0000-7000-8000-00000000d719", 1,
       "datatype.text.utf8.v1", 1, 1, 0, true,
       "text", 0, 1, 0, false, 0, 0, 16777216, 0,
       static_cast<u32>(CanonicalTypeId::character),
       "019d0000-0000-7000-8000-00000000d71a", true, true,
       "byte_sequence",
       "exact_well_formed_UTF8_scalar_sequence_without_implicit_normalization",
       "UTF-8", true, false, true, true, true, true,
       "CTB.TEXT.INVALID_ENCODING"},
  }};
  DatatypeTypeCodecIdentityLookupV1 result;
  result.diagnostic_id = "DATATYPE.DESCRIPTOR_INVALID";
  const auto row = std::find_if(rows.begin(), rows.end(), [&](const auto& candidate) {
    return catalog_snapshot_uuid == candidate.catalog_snapshot_uuid &&
           catalog_generation == candidate.catalog_generation &&
           registry_generation == candidate.registry_generation &&
           descriptor_uuid == candidate.descriptor_uuid &&
           descriptor_generation == candidate.descriptor_generation;
  });
  if (row == rows.end()) {
    return result;
  }
  result.ok = true;
  result.row = *row;
  result.diagnostic_id.clear();
  return result;
}

DatatypeTypeCodecIdentityLookupV1 LookupCanonicalBooleanTypeCodecIdentityV1(
    const std::string& catalog_snapshot_uuid,
    u64 catalog_generation,
    u64 registry_generation) {
  return LookupDatatypeTypeCodecIdentityV1(
      catalog_snapshot_uuid, catalog_generation, registry_generation,
      "01000000-626f-7f6c-a561-6e0000000000", 1);
}

bool IsExactCanonicalTextTypeCodecIdentityV1(
    const DatatypeTypeCodecIdentityRowV1& row) {
  return row.catalog_snapshot_uuid ==
             "019d0000-0000-7000-8000-00000000d701" &&
         row.catalog_generation == 1 && row.registry_generation == 1 &&
         row.descriptor_uuid ==
             "019d0000-0000-7000-8000-00000000d718" &&
         row.descriptor_generation == 1 &&
         row.type_uuid == "019d0000-0000-7000-8000-00000000d719" &&
         row.type_generation == 1 &&
         row.codec_uuid == "019d0000-0000-7000-8000-00000000d71a" &&
         row.codec_id == "datatype.text.utf8.v1" &&
         row.codec_version == 1 && row.codec_generation == 1 &&
         row.canonical_value_bytes == 0 && row.null_supported &&
         row.canonical_name == "text" && row.datatype_identity_code == 0 &&
         row.null_encoding_code == 1 && row.byte_order_code == 0 &&
         !row.signed_code && row.representation_code == 0 &&
         row.canonical_value_minimum_bytes == 0 &&
         row.canonical_value_maximum_bytes == 16777216 &&
         row.canonical_value_exact_bytes == 0 &&
         row.canonical_binary_type_code ==
             static_cast<u32>(CanonicalTypeId::character) &&
         row.canonical_value_variable_width &&
         row.canonical_value_exact_zero_is_width_marker &&
         row.canonical_byte_order == "byte_sequence" &&
         row.canonical_representation ==
             "exact_well_formed_UTF8_scalar_sequence_without_implicit_"
             "normalization" &&
         row.canonical_charset == "UTF-8" &&
         row.shortest_form_utf8_required &&
         !row.implicit_normalization_allowed &&
         row.descriptor_bound_collation_required &&
         row.empty_value_distinct_from_sql_null &&
         row.sql_null_requires_zero_payload &&
         row.variable_width_storage_without_truncation &&
         row.invalid_encoding_diagnostic_id == "CTB.TEXT.INVALID_ENCODING";
}

bool IsExactCanonicalBooleanDescriptorTypeAliasV1(
    const std::string& descriptor_uuid,
    const u64 descriptor_generation,
    const std::string& type_uuid,
    const u64 type_generation,
    const std::string& codec_id,
    const u16 codec_version,
    const u64 codec_generation,
    const bool containing_slot_nullability_authoritative) {
  if (!containing_slot_nullability_authoritative) return false;
  const auto boolean = LookupCanonicalBooleanTypeCodecIdentityV1(
      "019d0000-0000-7000-8000-00000000d701", 1, 1);
  return boolean.ok && descriptor_uuid == boolean.row.descriptor_uuid &&
         descriptor_generation == boolean.row.descriptor_generation &&
         type_uuid == boolean.row.type_uuid &&
         type_generation == boolean.row.type_generation &&
         codec_id == boolean.row.codec_id &&
         codec_version == boolean.row.codec_version &&
         codec_generation == boolean.row.codec_generation &&
         boolean.row.null_supported && boolean.row.null_encoding_code == 1 &&
         boolean.row.canonical_value_minimum_bytes == 1 &&
         boolean.row.canonical_value_maximum_bytes == 1 &&
         boolean.row.canonical_value_exact_bytes == 1 &&
         boolean.row.canonical_binary_type_code ==
             static_cast<u32>(CanonicalTypeId::boolean);
}

BuiltinOperatorTypeCodecIdentityLookupV1
LookupBuiltinOperatorTypeCodecIdentityV1(
    const std::string& operator_snapshot_uuid,
    u64 operator_registry_generation,
    const std::string& operator_uuid,
    u64 operator_generation,
    const std::string& left_descriptor_uuid,
    u64 left_descriptor_generation,
    const std::string& left_type_uuid,
    u64 left_type_generation,
    const std::string& right_descriptor_uuid,
    u64 right_descriptor_generation,
    const std::string& right_type_uuid,
    u64 right_type_generation) {
  // SB_REG_BUILTIN_OPERATOR_REGISTRY contributes one accepted comparison row
  // to the narrow typed UPDATE profile.  The exact operand identity is bound
  // into the returned row; no spelling, overload ranking, or host type enum is
  // accepted as authority.
  BuiltinOperatorTypeCodecIdentityLookupV1 result;
  result.diagnostic_id = "DATATYPE.DESCRIPTOR_INVALID";
  constexpr const char* kSnapshotUuid =
      "019d0000-0000-7000-8000-00000000d720";
  constexpr const char* kEqualUuid =
      "019de5fc-2400-7b73-9c38-dcf10204dbde";
  if (operator_snapshot_uuid != kSnapshotUuid ||
      operator_registry_generation != 1 || operator_uuid != kEqualUuid ||
      operator_generation != 1 || left_descriptor_generation != 1 ||
      left_type_generation != 1 || right_descriptor_generation != 1 ||
      right_type_generation != 1 ||
      left_descriptor_uuid != right_descriptor_uuid ||
      left_type_uuid != right_type_uuid) {
    return result;
  }
  const auto operand = LookupDatatypeTypeCodecIdentityV1(
      "019d0000-0000-7000-8000-00000000d701", 1, 1,
      left_descriptor_uuid, left_descriptor_generation);
  const auto boolean = LookupDatatypeTypeCodecIdentityV1(
      "019d0000-0000-7000-8000-00000000d701", 1, 1,
      "01000000-626f-7f6c-a561-6e0000000000", 1);
  if (!operand.ok || operand.row.type_uuid != left_type_uuid || !boolean.ok) {
    return result;
  }
  result.row.operator_snapshot_uuid = kSnapshotUuid;
  result.row.operator_registry_generation = 1;
  result.row.operator_uuid = kEqualUuid;
  result.row.operator_generation = 1;
  result.row.semantic_code = 1;
  result.row.operand_arity = 2;
  result.row.null_behavior_code = 1;
  result.row.accepted_state = 1;
  result.row.left_descriptor_uuid = left_descriptor_uuid;
  result.row.left_descriptor_generation = left_descriptor_generation;
  result.row.left_type_uuid = left_type_uuid;
  result.row.left_type_generation = left_type_generation;
  result.row.right_descriptor_uuid = right_descriptor_uuid;
  result.row.right_descriptor_generation = right_descriptor_generation;
  result.row.right_type_uuid = right_type_uuid;
  result.row.right_type_generation = right_type_generation;
  result.row.result_descriptor_uuid = boolean.row.descriptor_uuid;
  result.row.result_descriptor_generation = boolean.row.descriptor_generation;
  result.row.result_type_uuid = boolean.row.type_uuid;
  result.row.result_type_generation = boolean.row.type_generation;
  result.row.result_codec_id = boolean.row.codec_id;
  result.row.result_codec_version = boolean.row.codec_version;
  result.row.result_codec_generation = boolean.row.codec_generation;
  result.row.operator_family_code = 1;
  result.row.operand_identity_rule = 1;
  result.row.result_null_encoding_code = 1;
  result.ok = true;
  result.diagnostic_id.clear();
  return result;
}

BuiltinOperatorRegistrySnapshotIdentityLookupV1
LoadCurrentBuiltinOperatorRegistrySnapshotIdentityV1() {
  BuiltinOperatorRegistrySnapshotIdentityLookupV1 result;
  result.ok = true;
  result.snapshot_uuid = "019d0000-0000-7000-8000-00000000d720";
  result.registry_generation = 1;
  result.equality_operator_uuid =
      "019de5fc-2400-7b73-9c38-dcf10204dbde";
  result.equality_operator_generation = 1;
  return result;
}

}  // namespace scratchbird::core::datatypes
