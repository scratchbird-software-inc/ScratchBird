// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "datatype_conformance_manifest.hpp"

#include <set>
#include <utility>

namespace scratchbird::core::datatypes {
namespace {

using scratchbird::core::platform::DiagnosticArgument;
using scratchbird::core::platform::MakeDiagnostic;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;
using scratchbird::core::platform::TypedUuid;
using scratchbird::core::platform::UuidKind;
using scratchbird::core::platform::byte;
using scratchbird::core::platform::u32;

Status ManifestOkStatus() {
  return {StatusCode::ok, Severity::info, Subsystem::datatypes};
}

Status ManifestErrorStatus() {
  return {StatusCode::platform_required_feature_missing,
          Severity::error,
          Subsystem::datatypes};
}

DiagnosticRecord MakeManifestDiagnostic(Status status,
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
                        "core.datatypes.conformance_manifest");
}

void AddFailure(DatatypeConformanceManifestResult* result,
                std::string diagnostic_code,
                std::string message_key,
                std::string detail = {}) {
  result->status = ManifestErrorStatus();
  DiagnosticRecord diagnostic = MakeManifestDiagnostic(
      result->status, std::move(diagnostic_code), std::move(message_key),
      std::move(detail));
  if (result->diagnostics.empty()) {
    result->diagnostic = diagnostic;
  }
  result->diagnostics.push_back(std::move(diagnostic));
}

TypedUuid ExampleDescriptorUuid(CanonicalTypeId type_id) {
  TypedUuid uuid;
  uuid.kind = UuidKind::object;
  const u32 value = static_cast<u32>(type_id);
  uuid.value.bytes[0] = static_cast<byte>(value & 0xffu);
  uuid.value.bytes[1] = static_cast<byte>((value >> 8) & 0xffu);
  uuid.value.bytes[2] = static_cast<byte>((value >> 16) & 0xffu);
  uuid.value.bytes[3] = static_cast<byte>((value >> 24) & 0xffu);
  for (std::size_t index = 4; index < uuid.value.bytes.size(); ++index) {
    uuid.value.bytes[index] =
        static_cast<byte>((value + (index * 37u) + 0x5du) & 0xffu);
  }
  uuid.value.bytes[6] =
      static_cast<byte>((uuid.value.bytes[6] & 0x0fu) | 0x70u);
  uuid.value.bytes[8] =
      static_cast<byte>((uuid.value.bytes[8] & 0x3fu) | 0x80u);
  return uuid;
}

CatalogExecutionTypeMetadata ExampleCatalogMetadata(
    const DatatypeDescriptor& descriptor,
    const DatatypeStorageLayout& layout) {
  CatalogExecutionTypeMetadata metadata;
  metadata.descriptor_uuid = ExampleDescriptorUuid(descriptor.type_id);
  metadata.descriptor_epoch = 1;
  metadata.precision = descriptor.default_precision;
  metadata.scale = descriptor.default_scale;
  if (descriptor.width_class != TypeWidthClass::fixed) {
    metadata.length = layout.inline_bytes != 0 ? layout.inline_bytes : 256;
  }
  if (descriptor.family == TypeFamily::vector) {
    metadata.vector_dimensions = 16;
    metadata.element_descriptor_uuid =
        ExampleDescriptorUuid(CanonicalTypeId::real32);
  }
  if (descriptor.family == TypeFamily::structured ||
      descriptor.family == TypeFamily::range) {
    metadata.container_rank = 1;
    metadata.element_descriptor_uuid =
        ExampleDescriptorUuid(CanonicalTypeId::int64);
  }
  if (layout.requires_charset) {
    metadata.charset_uuid = ExampleDescriptorUuid(CanonicalTypeId::character);
  }
  if (layout.requires_collation) {
    metadata.collation_uuid = ExampleDescriptorUuid(CanonicalTypeId::binary);
  }
  if (layout.requires_timezone) {
    metadata.timezone_uuid = ExampleDescriptorUuid(CanonicalTypeId::timestamp);
  }
  return metadata;
}

bool EvidencePathForbidden(const std::string& path) {
  if (path.empty() || path[0] == '/') {
    return true;
  }
  const std::string private_tracker_repo = std::string("ScratchBird") + "-Private";
  if (path.find(private_tracker_repo) != std::string::npos) {
    return true;
  }
  if (path.rfind("docs/", 0) == 0 || path.rfind("project/docs/", 0) == 0) {
    return true;
  }
  return path.rfind("project/", 0) != 0;
}

bool LayoutMatches(const DatatypeStorageLayout& left,
                   const DatatypeStorageLayout& right) {
  return left.type_id == right.type_id &&
         left.storage_class == right.storage_class &&
         left.encoding == right.encoding &&
         left.inline_bytes == right.inline_bytes &&
         left.alignment_bytes == right.alignment_bytes &&
         left.requires_descriptor == right.requires_descriptor &&
         left.requires_charset == right.requires_charset &&
         left.requires_collation == right.requires_collation &&
         left.requires_timezone == right.requires_timezone &&
         left.may_overflow_to_toast == right.may_overflow_to_toast &&
         left.fixed_sort_key == right.fixed_sort_key;
}

}  // namespace

const char* DatatypeConformanceExampleSourceName(
    DatatypeConformanceExampleSource source) {
  switch (source) {
    case DatatypeConformanceExampleSource::current_core_registry:
      return "current_core_registry";
    case DatatypeConformanceExampleSource::documentation_only:
      return "documentation_only";
    case DatatypeConformanceExampleSource::private_tracker:
      return "private_tracker";
    case DatatypeConformanceExampleSource::unknown:
      return "unknown";
  }
  return "unknown";
}

DatatypeConformanceManifestResult LoadCurrentCoreDatatypeConformanceManifest() {
  DatatypeConformanceManifestResult result;
  result.status = ManifestOkStatus();
  result.manifest.manifest_key = kCurrentCoreDatatypeConformanceManifestKey;
  result.manifest.inventory_source_path =
      "project/src/core/datatypes/datatype_descriptor.cpp";
  result.manifest.parser_authority_allowed = false;

  for (const DatatypeDescriptor& descriptor : BuiltinDatatypeDescriptors()) {
    DatatypeConformanceExample example;
    example.type_id = descriptor.type_id;
    example.stable_name = descriptor.stable_name;
    example.source = DatatypeConformanceExampleSource::current_core_registry;
    example.evidence_path = "project/src/core/datatypes/datatype_descriptor.cpp";
    example.source_marker = "DEFER-DPE-EXAMPLE-CORPUS";

    const auto serialized = SerializeDatatypeDescriptor(descriptor);
    if (!serialized.ok()) {
      AddFailure(&result,
                 "SB-DATATYPE-CONFORMANCE-ENCODED-EXAMPLE-REFUSED",
                 "datatype.conformance.encoded_example_refused",
                 descriptor.stable_name);
      continue;
    }
    example.encoded_descriptor = serialized.serialized;

    const auto layout = LookupDatatypeStorageLayout(descriptor.type_id);
    if (!layout.ok()) {
      AddFailure(&result,
                 "SB-DATATYPE-CONFORMANCE-LAYOUT-EXAMPLE-MISSING",
                 "datatype.conformance.layout_example_missing",
                 descriptor.stable_name);
      continue;
    }
    example.storage_layout = layout.layout;
    result.manifest.examples.push_back(std::move(example));
  }

  return result;
}

DatatypeConformanceManifestResult ExecuteDatatypeConformanceManifest(
    const DatatypeConformanceManifest& manifest) {
  DatatypeConformanceManifestResult result;
  result.status = ManifestOkStatus();
  result.manifest = manifest;

  if (manifest.manifest_key != kCurrentCoreDatatypeConformanceManifestKey) {
    AddFailure(&result,
               "SB-DATATYPE-CONFORMANCE-MANIFEST-UNKNOWN",
               "datatype.conformance.manifest_unknown",
               manifest.manifest_key);
  }

  if (manifest.parser_authority_allowed) {
    AddFailure(&result,
               "SB-DATATYPE-CONFORMANCE-PARSER-AUTHORITY-REFUSED",
               "datatype.conformance.parser_authority_refused",
               manifest.manifest_key);
  }

  std::set<CanonicalTypeId> required;
  for (const DatatypeDescriptor& descriptor : BuiltinDatatypeDescriptors()) {
    required.insert(descriptor.type_id);
  }

  std::set<CanonicalTypeId> seen;
  for (const DatatypeConformanceExample& example : manifest.examples) {
    if (example.source !=
        DatatypeConformanceExampleSource::current_core_registry) {
      AddFailure(&result,
                 "SB-DATATYPE-CONFORMANCE-DOCS-ONLY-EXAMPLE-REFUSED",
                 "datatype.conformance.docs_only_example_refused",
                 DatatypeConformanceExampleSourceName(example.source));
      continue;
    }
    if (EvidencePathForbidden(example.evidence_path)) {
      AddFailure(&result,
                 "SB-DATATYPE-CONFORMANCE-EVIDENCE-PATH-REFUSED",
                 "datatype.conformance.evidence_path_refused",
                 example.evidence_path);
      continue;
    }
    if (!seen.insert(example.type_id).second) {
      AddFailure(&result,
                 "SB-DATATYPE-CONFORMANCE-MANIFEST-DUPLICATE-ROW",
                 "datatype.conformance.manifest_duplicate_row",
                 CanonicalTypeName(example.type_id));
      continue;
    }

    const auto parsed = ParseDatatypeDescriptor(example.encoded_descriptor);
    if (!parsed.ok() || parsed.descriptor.type_id != example.type_id ||
        parsed.descriptor.stable_name != example.stable_name) {
      AddFailure(&result,
                 "SB-DATATYPE-CONFORMANCE-ENCODED-EXAMPLE-REFUSED",
                 "datatype.conformance.encoded_example_refused",
                 example.stable_name);
      continue;
    }

    const auto descriptor = LookupDatatypeDescriptor(example.type_id);
    if (!descriptor.ok()) {
      AddFailure(&result,
                 "SB-DATATYPE-CONFORMANCE-MANIFEST-ROW-UNKNOWN",
                 "datatype.conformance.manifest_row_unknown",
                 example.stable_name);
      continue;
    }

    const auto layout = LookupDatatypeStorageLayout(example.type_id);
    if (!layout.ok() || !LayoutMatches(layout.layout, example.storage_layout)) {
      AddFailure(&result,
                 "SB-DATATYPE-CONFORMANCE-LAYOUT-EXAMPLE-MISMATCH",
                 "datatype.conformance.layout_example_mismatch",
                 example.stable_name);
      continue;
    }

    const auto execution_descriptor = BuildExecutionTypeDescriptorFromCatalog(
        descriptor.descriptor,
        ExampleCatalogMetadata(descriptor.descriptor, layout.layout));
    if (!execution_descriptor.ok() ||
        !execution_descriptor.descriptor.parser_independent ||
        execution_descriptor.descriptor.canonical_type_id !=
            static_cast<u32>(example.type_id)) {
      AddFailure(&result,
                 "SB-DATATYPE-CONFORMANCE-EXECUTION-EXAMPLE-REFUSED",
                 "datatype.conformance.execution_example_refused",
                 example.stable_name);
      continue;
    }

    const auto conversion =
        DescribeDatatypeConversion(example.type_id, example.type_id);
    if (!conversion.ok() ||
        conversion.kind != ConversionDiagnosticKind::exact) {
      AddFailure(&result,
                 "SB-DATATYPE-CONFORMANCE-CONVERSION-EXAMPLE-REFUSED",
                 "datatype.conformance.conversion_example_refused",
                 example.stable_name);
      continue;
    }

    ++result.executed_examples;
  }

  for (const CanonicalTypeId type_id : required) {
    if (seen.find(type_id) == seen.end()) {
      AddFailure(&result,
                 "SB-DATATYPE-CONFORMANCE-MANIFEST-ROW-MISSING",
                 "datatype.conformance.manifest_row_missing",
                 CanonicalTypeName(type_id));
    }
  }

  return result;
}

}  // namespace scratchbird::core::datatypes
