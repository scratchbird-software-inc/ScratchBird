// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "datatype_catalog_manifest.hpp"

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
    descriptor_row.descriptor_uuid =
        StableDatatypeDescriptorUuid(descriptor.type_id,
                                     descriptor.stable_name);
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
        StableDatatypeDescriptorUuid(row.type_id, row.stable_name);
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

}  // namespace scratchbird::core::datatypes
