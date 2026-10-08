// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "datatype_conformance_manifest.hpp"

#include <algorithm>
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

template <typename Receipt>
bool IdentityMatchesReceipt(const DatatypeTypeCodecIdentityRowV3& identity,
                            const Receipt& receipt) noexcept {
  const auto& row = identity.legacy_fields;
  return row.catalog_snapshot_uuid == receipt.catalog_snapshot_uuid &&
         row.catalog_generation == receipt.catalog_generation &&
         row.registry_generation == receipt.registry_generation;
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

void AddOwnedFailure(DatatypeConformanceManifestResult* result,
                     const DiagnosticRecord& diagnostic) {
  result->status = diagnostic.status;
  if (result->status.ok()) {
    result->status = ManifestErrorStatus();
  }
  if (result->diagnostics.empty()) {
    result->diagnostic = diagnostic;
  }
  result->diagnostics.push_back(diagnostic);
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

DatatypeConformanceManifestResult LoadCurrentCoreDatatypeConformanceManifest(
    const BitStringAuthorityReceiptV3& bit_string_receipt,
    bool bit_string_null_allowed,
    const DateAuthorityReceiptV3& date_receipt,
    bool date_null_allowed,
    const TimeAuthorityReceiptV3& time_receipt,
    bool time_null_allowed,
    const TimestampAuthorityReceiptV3& timestamp_receipt,
    bool timestamp_null_allowed,
    const IntervalAuthorityReceiptV3& interval_receipt,
    bool interval_null_allowed,
    const BlobAuthorityReceiptV3& blob_receipt,
    bool blob_null_allowed) {
  DatatypeConformanceManifestResult result;
  result.status = ManifestOkStatus();
  result.manifest.manifest_key = kCurrentCoreDatatypeConformanceManifestKey;
  result.manifest.inventory_source_path =
      "project/src/core/datatypes/datatype_descriptor.cpp";
  result.manifest.parser_authority_allowed = false;

  for (const DatatypeDescriptor& descriptor : BuiltinDatatypeDescriptors()) {
    if (descriptor.type_id == CanonicalTypeId::bit_string ||
        descriptor.type_id == CanonicalTypeId::date ||
        descriptor.type_id == CanonicalTypeId::time ||
        descriptor.type_id == CanonicalTypeId::timestamp ||
        descriptor.type_id == CanonicalTypeId::interval ||
        descriptor.type_id == CanonicalTypeId::blob) {
      continue;
    }
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

  const auto current_v3 = CurrentDatatypeTypeCodecIdentityRowsV3();
  const auto bit_identity = std::find_if(
      current_v3.begin(), current_v3.end(),
      [&](const DatatypeTypeCodecIdentityRowV3& row) {
        return IdentityMatchesReceipt(row, bit_string_receipt) &&
               IsExactCanonicalBitStringTypeCodecIdentityV3(row);
      });
  if (bit_identity == current_v3.end()) {
    AddFailure(&result,
               "CTB.BIT.SERIALIZATION_PROFILE_MISSING",
               "datatype.conformance.bit_string_v3_identity_missing");
    return result;
  }

  BitStringProfileRequestV3 request;
  request.receipt = bit_string_receipt;
  request.identity = *bit_identity;
  request.kind = BitStringSurfaceProfileKindV3::unqualified;
  request.length_bits = kBitStringMaximumLogicalBitsV3;
  const auto profile = BuildBitStringDescriptorProfileV3(request);
  if (!profile.ok()) {
    AddOwnedFailure(&result, profile.diagnostic);
    return result;
  }

  BitStringConformanceExampleV3 bit_example;
  bit_example.receipt = request.receipt;
  bit_example.identity = *bit_identity;
  bit_example.profile = profile.profile;
  bit_example.null_allowed = bit_string_null_allowed;
  bit_example.state = BitStringValueStateV3::present;
  bit_example.canonical_component = {0, 0, 0, 0};
  bit_example.source = DatatypeConformanceExampleSource::current_core_registry;
  bit_example.evidence_path =
      "project/src/core/datatypes/datatype_bit_string.cpp";
  bit_example.source_marker = "BASE-BIT-STRING-CONFORMANCE-V1";
  result.manifest.bit_string_examples.push_back(std::move(bit_example));

  const auto date_identity = std::find_if(
      current_v3.begin(), current_v3.end(),
      [&](const DatatypeTypeCodecIdentityRowV3& row) {
        return IdentityMatchesReceipt(row, date_receipt) &&
               IsExactCanonicalDateTypeCodecIdentityV3(row);
      });
  if (date_identity == current_v3.end()) {
    AddFailure(&result,
               "CTI.TEMPORAL.DESCRIPTOR_INVALID",
               "datatype.conformance.date_v3_identity_missing");
    return result;
  }
  const auto date_profile =
      BuildDateValidatedProfileHandleV3(date_receipt, *date_identity);
  if (!date_profile.ok()) {
    AddOwnedFailure(
        &result,
        MakeDateDiagnosticV3(
            date_profile.status,
            std::string(date_profile.diagnostic.diagnostic_code),
            "datatype.conformance.date_profile_build_refused",
            std::string(date_profile.diagnostic.detail)));
    return result;
  }
  DateConformanceExampleV3 date_example;
  date_example.receipt = date_receipt;
  date_example.identity = *date_identity;
  date_example.profile = date_profile.profile;
  date_example.null_allowed = date_null_allowed;
  date_example.state = DateValueStateV3::value;
  date_example.canonical_component = {0, 0, 0, 0};
  date_example.source = DatatypeConformanceExampleSource::current_core_registry;
  date_example.evidence_path =
      "project/src/core/datatypes/datatype_date.cpp";
  date_example.source_marker = "BASE-DATE-CONFORMANCE-V1";
  result.manifest.date_examples.push_back(std::move(date_example));

  const auto time_identity = std::find_if(
      current_v3.begin(), current_v3.end(),
      [&](const DatatypeTypeCodecIdentityRowV3& row) {
        return IdentityMatchesReceipt(row, time_receipt) &&
               IsExactCanonicalTimeTypeCodecIdentityV3(row);
      });
  if (time_identity == current_v3.end()) {
    AddFailure(&result,
               "CTI.TEMPORAL.DESCRIPTOR_INVALID",
               "datatype.conformance.time_v3_identity_missing");
    return result;
  }
  const auto time_profile =
      BuildTimeValidatedProfileHandleV3(time_receipt, *time_identity);
  if (!time_profile.ok()) {
    AddOwnedFailure(
        &result,
        MakeTimeDiagnosticV3(
            time_profile.status,
            std::string(time_profile.diagnostic.diagnostic_code),
            "datatype.conformance.time_profile_build_refused",
            std::string(time_profile.diagnostic.detail)));
    return result;
  }
  TimeConformanceExampleV3 time_example;
  time_example.receipt = time_receipt;
  time_example.identity = *time_identity;
  time_example.profile = time_profile.profile;
  time_example.null_allowed = time_null_allowed;
  time_example.state = TimeValueStateV3::value;
  time_example.canonical_component.assign(kTimeComponentBytesV3, byte{0});
  time_example.source = DatatypeConformanceExampleSource::current_core_registry;
  time_example.evidence_path =
      "project/src/core/datatypes/datatype_time.cpp";
  time_example.source_marker = "BASE-TIME-CONFORMANCE-V3";
  result.manifest.time_examples.push_back(std::move(time_example));

  const auto timestamp_identity = std::find_if(
      current_v3.begin(), current_v3.end(),
      [&](const DatatypeTypeCodecIdentityRowV3& row) {
        return IdentityMatchesReceipt(row, timestamp_receipt) &&
               IsExactCanonicalTimestampTypeCodecIdentityV3(row);
      });
  if (timestamp_identity == current_v3.end()) {
    AddFailure(&result,
               "CTI.TEMPORAL.DESCRIPTOR_INVALID",
               "datatype.conformance.timestamp_v3_identity_missing");
    return result;
  }
  const auto timestamp_profile = BuildTimestampValidatedProfileHandleV3(
      timestamp_receipt, *timestamp_identity);
  if (!timestamp_profile.ok()) {
    AddOwnedFailure(
        &result,
        MakeTimestampDiagnosticV3(
            timestamp_profile.status,
            std::string(timestamp_profile.diagnostic.diagnostic_code),
            "datatype.conformance.timestamp_profile_build_refused",
            std::string(timestamp_profile.diagnostic.detail)));
    return result;
  }
  const auto timestamp_profile_owner =
      std::make_shared<const TimestampValidatedProfileHandleV3>(
          timestamp_profile.profile);
  const TimestampOwnedValueV3 timestamp_value{
      timestamp_profile_owner, TimestampValueStateV3::value, 0, 0};
  const auto timestamp_component =
      EncodeCanonicalTimestampComponentV3(timestamp_value);
  if (!timestamp_component.ok()) {
    AddOwnedFailure(
        &result,
        MakeTimestampDiagnosticV3(
            timestamp_component.status,
            std::string(timestamp_component.diagnostic.diagnostic_code),
            "datatype.conformance.timestamp_component_encode_refused",
            std::string(timestamp_component.diagnostic.detail)));
    return result;
  }
  TimestampConformanceExampleV3 timestamp_example;
  timestamp_example.receipt = timestamp_receipt;
  timestamp_example.identity = *timestamp_identity;
  timestamp_example.profile = timestamp_profile.profile;
  timestamp_example.null_allowed = timestamp_null_allowed;
  timestamp_example.state = TimestampValueStateV3::value;
  timestamp_example.canonical_component = timestamp_component.bytes;
  timestamp_example.source =
      DatatypeConformanceExampleSource::current_core_registry;
  timestamp_example.evidence_path =
      "project/src/core/datatypes/datatype_timestamp.cpp";
  timestamp_example.source_marker = "BASE-TIMESTAMP-CONFORMANCE-V3";
  result.manifest.timestamp_examples.push_back(std::move(timestamp_example));

  const auto interval_identity = std::find_if(
      current_v3.begin(), current_v3.end(),
      [&](const DatatypeTypeCodecIdentityRowV3& row) {
        return IdentityMatchesReceipt(row, interval_receipt) &&
               IsExactCanonicalIntervalTypeCodecIdentityV3(row);
      });
  if (interval_identity == current_v3.end()) {
    AddFailure(&result, "CTI.INTERVAL.DESCRIPTOR_INVALID",
               "datatype.conformance.interval_v3_identity_missing");
    return result;
  }
  const auto interval_profile = BuildIntervalValidatedProfileHandleV3(
      interval_receipt, *interval_identity);
  if (!interval_profile.ok()) {
    AddFailure(&result,
               std::string(interval_profile.diagnostic.diagnostic_code),
               "datatype.conformance.interval_profile_build_refused",
               std::string(interval_profile.diagnostic.detail));
    return result;
  }
  const auto interval_profile_owner =
      std::make_shared<const IntervalValidatedProfileHandleV3>(
          interval_profile.profile);
  const IntervalOwnedValueV3 interval_value{
      interval_profile_owner, IntervalValueStateV3::value, 0, 0, 0};
  const auto interval_component =
      EncodeCanonicalIntervalComponentV3(interval_value);
  if (!interval_component.ok() || interval_component.bytes.size() != 16 ||
      !std::all_of(interval_component.bytes.begin(),
                   interval_component.bytes.end(),
                   [](byte value) { return value == 0; })) {
    AddFailure(
        &result,
        interval_component.ok()
            ? "CTI.TEMPORAL.CANONICAL_ENCODING_INVALID"
            : std::string(interval_component.diagnostic.diagnostic_code),
        "datatype.conformance.interval_component_encode_refused",
        interval_component.ok()
            ? "zero_tuple_not_exact_le16"
            : std::string(interval_component.diagnostic.detail));
    return result;
  }
  IntervalConformanceExampleV3 interval_example;
  interval_example.receipt = interval_receipt;
  interval_example.identity = *interval_identity;
  interval_example.profile = interval_profile.profile;
  interval_example.null_allowed = interval_null_allowed;
  interval_example.state = IntervalValueStateV3::value;
  interval_example.canonical_component = interval_component.bytes;
  interval_example.source =
      DatatypeConformanceExampleSource::current_core_registry;
  interval_example.evidence_path =
      "project/src/core/datatypes/datatype_interval.cpp";
  interval_example.source_marker = "BASE-INTERVAL-CONFORMANCE-V3";
  result.manifest.interval_examples.push_back(std::move(interval_example));

  const auto blob_identity = std::find_if(current_v3.begin(), current_v3.end(),
      [&](const auto& row) {
        return IdentityMatchesReceipt(row, blob_receipt) &&
            IsExactCanonicalBlobTypeCodecIdentityV3(row);
      });
  if (blob_identity == current_v3.end()) {
    AddFailure(&result, "BLOB.DESCRIPTOR_INVALID",
               "datatype.conformance.blob_identity_refused");
    return result;
  }
  const auto blob_profile = BuildBlobValidatedProfileHandleV3(blob_receipt, *blob_identity);
  if (!blob_profile.ok()) {
    AddFailure(&result, "BLOB.DESCRIPTOR_INVALID",
               "datatype.conformance.blob_profile_refused");
    return result;
  }
  BlobConformanceExampleV3 blob_example;
  blob_example.receipt = blob_receipt;
  blob_example.identity = *blob_identity;
  blob_example.profile = blob_profile.profile;
  blob_example.null_allowed = blob_null_allowed;
  // Empty VALUE validates the structural profile without pretending that a
  // raw nonempty span has receiver-authenticated lifetime authority.
  blob_example.logical_length = 0;
  blob_example.source = DatatypeConformanceExampleSource::current_core_registry;
  blob_example.evidence_path = "project/src/core/datatypes/datatype_blob.cpp";
  blob_example.source_marker = "BASE-BLOB-STRUCTURAL-CONFORMANCE-V3";
  result.manifest.blob_examples.push_back(std::move(blob_example));

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
    if (descriptor.type_id != CanonicalTypeId::bit_string &&
        descriptor.type_id != CanonicalTypeId::date &&
        descriptor.type_id != CanonicalTypeId::time &&
        descriptor.type_id != CanonicalTypeId::timestamp &&
        descriptor.type_id != CanonicalTypeId::interval &&
        descriptor.type_id != CanonicalTypeId::blob) {
      required.insert(descriptor.type_id);
    }
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

  if (manifest.bit_string_examples.size() != 1) {
    AddFailure(&result,
               "SB-DATATYPE-CONFORMANCE-MANIFEST-ROW-MISSING",
               "datatype.conformance.bit_string_v3_example_count",
               std::to_string(manifest.bit_string_examples.size()));
  }
  for (const BitStringConformanceExampleV3& example :
       manifest.bit_string_examples) {
    if (example.source !=
        DatatypeConformanceExampleSource::current_core_registry) {
      AddFailure(&result,
                 "SB-DATATYPE-CONFORMANCE-DOCS-ONLY-EXAMPLE-REFUSED",
                 "datatype.conformance.bit_string_docs_only_refused",
                 DatatypeConformanceExampleSourceName(example.source));
      continue;
    }
    if (EvidencePathForbidden(example.evidence_path)) {
      AddFailure(&result,
                 "SB-DATATYPE-CONFORMANCE-EVIDENCE-PATH-REFUSED",
                 "datatype.conformance.bit_string_evidence_path_refused",
                 example.evidence_path);
      continue;
    }
    if (!IdentityMatchesReceipt(example.identity, example.receipt) ||
        !IsExactCanonicalBitStringTypeCodecIdentityV3(example.identity)) {
      AddFailure(&result,
                 "CTB.BIT.DESCRIPTOR_INVALID",
                 "datatype.conformance.bit_string_identity_refused");
      continue;
    }
    const auto profile = ValidateBitStringDescriptorProfileV3(example.profile);
    if (!profile.ok()) {
      AddOwnedFailure(&result, profile.diagnostic);
      continue;
    }
    if (example.receipt.statement_receipt_uuid !=
            example.profile.receipt.statement_receipt_uuid ||
        example.receipt.catalog_snapshot_uuid !=
            example.profile.receipt.catalog_snapshot_uuid ||
        example.receipt.catalog_generation !=
            example.profile.receipt.catalog_generation ||
        example.receipt.registry_generation !=
            example.profile.receipt.registry_generation) {
      AddFailure(&result,
                 "CTB.BIT.DESCRIPTOR_INVALID",
                 "datatype.conformance.bit_string_receipt_refused");
      continue;
    }
    const auto decoded = DecodeCanonicalBitStringComponentNoAllocV3(
        example.profile, example.state, example.null_allowed,
        example.canonical_component);
    if (!decoded.ok()) {
      AddOwnedFailure(
          &result,
          MakeBitStringDiagnosticV3(
              decoded.status,
              std::string(decoded.diagnostic.diagnostic_code),
              "datatype.conformance.bit_string_component_refused",
              std::string(decoded.diagnostic.detail)));
      continue;
    }
    if (decoded.value.profile != &example.profile ||
        decoded.value.state != example.state) {
      AddFailure(&result,
                 "CTB.BIT.CANONICAL_ENCODING_INVALID",
                 "datatype.conformance.bit_string_component_refused");
      continue;
    }
    ++result.executed_examples;
    ++result.executed_bit_string_examples;
  }

  if (manifest.date_examples.size() != 1) {
    AddFailure(&result,
               "SB-DATATYPE-CONFORMANCE-MANIFEST-ROW-MISSING",
               "datatype.conformance.date_v3_example_count",
               std::to_string(manifest.date_examples.size()));
  }
  for (const DateConformanceExampleV3& example : manifest.date_examples) {
    if (example.source !=
        DatatypeConformanceExampleSource::current_core_registry) {
      AddFailure(&result,
                 "SB-DATATYPE-CONFORMANCE-DOCS-ONLY-EXAMPLE-REFUSED",
                 "datatype.conformance.date_docs_only_refused",
                 DatatypeConformanceExampleSourceName(example.source));
      continue;
    }
    if (EvidencePathForbidden(example.evidence_path)) {
      AddFailure(&result,
                 "SB-DATATYPE-CONFORMANCE-EVIDENCE-PATH-REFUSED",
                 "datatype.conformance.date_evidence_path_refused",
                 example.evidence_path);
      continue;
    }
    if (!IdentityMatchesReceipt(example.identity, example.receipt) ||
        !IsExactCanonicalDateTypeCodecIdentityV3(example.identity)) {
      AddFailure(&result,
                 "CTI.TEMPORAL.DESCRIPTOR_INVALID",
                 "datatype.conformance.date_identity_refused");
      continue;
    }
    const auto profile = ValidateDateProfileHandleV3(example.profile);
    if (!profile.ok()) {
      AddOwnedFailure(
          &result,
          MakeDateDiagnosticV3(
              profile.status,
              std::string(profile.diagnostic.diagnostic_code),
              "datatype.conformance.date_profile_validation_refused",
              std::string(profile.diagnostic.detail)));
      continue;
    }
    if (example.receipt.statement_receipt_uuid !=
            example.profile.receipt.statement_receipt_uuid ||
        example.receipt.catalog_snapshot_uuid !=
            example.profile.receipt.catalog_snapshot_uuid ||
        example.receipt.catalog_generation !=
            example.profile.receipt.catalog_generation ||
        example.receipt.registry_generation !=
            example.profile.receipt.registry_generation) {
      AddFailure(&result,
                 "CTI.TEMPORAL.DESCRIPTOR_INVALID",
                 "datatype.conformance.date_receipt_refused");
      continue;
    }
    const auto decoded = DecodeCanonicalDateComponentNoAllocV3(
        example.profile, example.state, example.null_allowed,
        example.canonical_component);
    if (!decoded.ok()) {
      AddOwnedFailure(
          &result,
          MakeDateDiagnosticV3(
              decoded.status,
              std::string(decoded.diagnostic.diagnostic_code),
              "datatype.conformance.date_component_refused",
              std::string(decoded.diagnostic.detail)));
      continue;
    }
    if (decoded.value.profile != &example.profile ||
        decoded.value.state != example.state || decoded.value.day != 0) {
      AddFailure(&result,
                 "CTI.TEMPORAL.CANONICAL_ENCODING_INVALID",
                 "datatype.conformance.date_component_refused");
      continue;
    }
    ++result.executed_examples;
    ++result.executed_date_examples;
  }

  if (manifest.time_examples.size() != 1) {
    AddFailure(&result,
               "SB-DATATYPE-CONFORMANCE-MANIFEST-ROW-MISSING",
               "datatype.conformance.time_v3_example_count",
               std::to_string(manifest.time_examples.size()));
  }
  for (const TimeConformanceExampleV3& example : manifest.time_examples) {
    if (example.source !=
        DatatypeConformanceExampleSource::current_core_registry) {
      AddFailure(&result,
                 "SB-DATATYPE-CONFORMANCE-DOCS-ONLY-EXAMPLE-REFUSED",
                 "datatype.conformance.time_docs_only_refused",
                 DatatypeConformanceExampleSourceName(example.source));
      continue;
    }
    if (EvidencePathForbidden(example.evidence_path)) {
      AddFailure(&result,
                 "SB-DATATYPE-CONFORMANCE-EVIDENCE-PATH-REFUSED",
                 "datatype.conformance.time_evidence_path_refused",
                 example.evidence_path);
      continue;
    }
    if (!IdentityMatchesReceipt(example.identity, example.receipt) ||
        !IsExactCanonicalTimeTypeCodecIdentityV3(example.identity)) {
      AddFailure(&result,
                 "CTI.TEMPORAL.DESCRIPTOR_INVALID",
                 "datatype.conformance.time_identity_refused");
      continue;
    }
    const auto profile = ValidateTimeProfileHandleV3(example.profile);
    if (!profile.ok()) {
      AddOwnedFailure(
          &result,
          MakeTimeDiagnosticV3(
              profile.status,
              std::string(profile.diagnostic.diagnostic_code),
              "datatype.conformance.time_profile_validation_refused",
              std::string(profile.diagnostic.detail)));
      continue;
    }
    if (example.receipt.statement_receipt_uuid !=
            example.profile.receipt.statement_receipt_uuid ||
        example.receipt.catalog_snapshot_uuid !=
            example.profile.receipt.catalog_snapshot_uuid ||
        example.receipt.catalog_generation !=
            example.profile.receipt.catalog_generation ||
        example.receipt.registry_generation !=
            example.profile.receipt.registry_generation) {
      AddFailure(&result,
                 "CTI.TEMPORAL.DESCRIPTOR_INVALID",
                 "datatype.conformance.time_receipt_refused");
      continue;
    }
    const auto decoded = DecodeCanonicalTimeComponentNoAllocV3(
        example.profile, example.state, example.null_allowed,
        example.canonical_component);
    if (!decoded.ok()) {
      AddOwnedFailure(
          &result,
          MakeTimeDiagnosticV3(
              decoded.status,
              std::string(decoded.diagnostic.diagnostic_code),
              "datatype.conformance.time_component_refused",
              std::string(decoded.diagnostic.detail)));
      continue;
    }
    if (decoded.value.profile != &example.profile ||
        decoded.value.state != example.state ||
        decoded.value.nanoseconds_since_midnight != 0) {
      AddFailure(&result,
                 "CTI.TEMPORAL.CANONICAL_ENCODING_INVALID",
                 "datatype.conformance.time_component_refused");
      continue;
    }
    ++result.executed_examples;
    ++result.executed_time_examples;
  }

  if (manifest.timestamp_examples.size() != 1) {
    AddFailure(&result,
               "SB-DATATYPE-CONFORMANCE-MANIFEST-ROW-MISSING",
               "datatype.conformance.timestamp_v3_example_count",
               std::to_string(manifest.timestamp_examples.size()));
  }
  for (const TimestampConformanceExampleV3& example :
       manifest.timestamp_examples) {
    if (example.source !=
        DatatypeConformanceExampleSource::current_core_registry) {
      AddFailure(&result,
                 "SB-DATATYPE-CONFORMANCE-DOCS-ONLY-EXAMPLE-REFUSED",
                 "datatype.conformance.timestamp_docs_only_refused",
                 DatatypeConformanceExampleSourceName(example.source));
      continue;
    }
    if (EvidencePathForbidden(example.evidence_path)) {
      AddFailure(&result,
                 "SB-DATATYPE-CONFORMANCE-EVIDENCE-PATH-REFUSED",
                 "datatype.conformance.timestamp_evidence_path_refused",
                 example.evidence_path);
      continue;
    }
    if (!IdentityMatchesReceipt(example.identity, example.receipt) ||
        !IsExactCanonicalTimestampTypeCodecIdentityV3(example.identity)) {
      AddFailure(&result,
                 "CTI.TEMPORAL.DESCRIPTOR_INVALID",
                 "datatype.conformance.timestamp_identity_refused");
      continue;
    }
    const auto profile = ValidateTimestampProfileHandleV3(example.profile);
    if (!profile.ok()) {
      AddOwnedFailure(
          &result,
          MakeTimestampDiagnosticV3(
              profile.status,
              std::string(profile.diagnostic.diagnostic_code),
              "datatype.conformance.timestamp_profile_validation_refused",
              std::string(profile.diagnostic.detail)));
      continue;
    }
    if (example.receipt.statement_receipt_uuid !=
            example.profile.receipt.statement_receipt_uuid ||
        example.receipt.catalog_snapshot_uuid !=
            example.profile.receipt.catalog_snapshot_uuid ||
        example.receipt.catalog_generation !=
            example.profile.receipt.catalog_generation ||
        example.receipt.registry_generation !=
            example.profile.receipt.registry_generation) {
      AddFailure(&result,
                 "CTI.TEMPORAL.DESCRIPTOR_INVALID",
                 "datatype.conformance.timestamp_receipt_refused");
      continue;
    }
    const auto decoded = DecodeCanonicalTimestampComponentNoAllocV3(
        example.profile, example.state, example.null_allowed,
        example.canonical_component);
    if (!decoded.ok()) {
      AddOwnedFailure(
          &result,
          MakeTimestampDiagnosticV3(
              decoded.status,
              std::string(decoded.diagnostic.diagnostic_code),
              "datatype.conformance.timestamp_component_refused",
              std::string(decoded.diagnostic.detail)));
      continue;
    }
    if (decoded.value.profile != &example.profile ||
        decoded.value.state != example.state ||
        decoded.value.civil_day != 0 ||
        decoded.value.nanoseconds_since_midnight != 0) {
      AddFailure(&result,
                 "CTI.TEMPORAL.CANONICAL_ENCODING_INVALID",
                 "datatype.conformance.timestamp_component_refused");
      continue;
    }
    ++result.executed_examples;
    ++result.executed_timestamp_examples;
  }

  if (manifest.interval_examples.size() != 1) {
    AddFailure(&result,
               "SB-DATATYPE-CONFORMANCE-MANIFEST-ROW-MISSING",
               "datatype.conformance.interval_v3_example_count",
               std::to_string(manifest.interval_examples.size()));
  }
  for (const IntervalConformanceExampleV3& example :
       manifest.interval_examples) {
    if (example.source !=
        DatatypeConformanceExampleSource::current_core_registry) {
      AddFailure(&result,
                 "SB-DATATYPE-CONFORMANCE-DOCS-ONLY-EXAMPLE-REFUSED",
                 "datatype.conformance.interval_docs_only_refused",
                 DatatypeConformanceExampleSourceName(example.source));
      continue;
    }
    if (EvidencePathForbidden(example.evidence_path)) {
      AddFailure(&result,
                 "SB-DATATYPE-CONFORMANCE-EVIDENCE-PATH-REFUSED",
                 "datatype.conformance.interval_evidence_path_refused",
                 example.evidence_path);
      continue;
    }
    if (!IdentityMatchesReceipt(example.identity, example.receipt) ||
        !IsExactCanonicalIntervalTypeCodecIdentityV3(example.identity)) {
      AddFailure(&result,
                 "CTI.INTERVAL.DESCRIPTOR_INVALID",
                 "datatype.conformance.interval_identity_refused");
      continue;
    }
    const auto profile = ValidateIntervalProfileHandleV3(example.profile);
    if (!profile.ok()) {
      AddFailure(&result,
                 std::string(profile.diagnostic.diagnostic_code),
                 "datatype.conformance.interval_profile_validation_refused",
                 std::string(profile.diagnostic.detail));
      continue;
    }
    if (example.receipt.statement_receipt_uuid !=
            example.profile.receipt.statement_receipt_uuid ||
        example.receipt.catalog_snapshot_uuid !=
            example.profile.receipt.catalog_snapshot_uuid ||
        example.receipt.catalog_generation !=
            example.profile.receipt.catalog_generation ||
        example.receipt.registry_generation !=
            example.profile.receipt.registry_generation) {
      AddFailure(&result,
                 "CTI.INTERVAL.DESCRIPTOR_INVALID",
                 "datatype.conformance.interval_receipt_refused");
      continue;
    }
    const auto decoded = DecodeCanonicalIntervalComponentNoAllocV3(
        example.profile, example.state, example.null_allowed,
        example.canonical_component);
    if (!decoded.ok()) {
      AddFailure(&result,
                 std::string(decoded.diagnostic.diagnostic_code),
                 "datatype.conformance.interval_component_refused",
                 std::string(decoded.diagnostic.detail));
      continue;
    }
    if (decoded.value.profile != &example.profile ||
        decoded.value.state != example.state || decoded.value.months != 0 ||
        decoded.value.civil_days != 0 ||
        decoded.value.fixed_nanoseconds != 0) {
      AddFailure(&result,
                 "CTI.TEMPORAL.CANONICAL_ENCODING_INVALID",
                 "datatype.conformance.interval_component_refused");
      continue;
    }
    ++result.executed_examples;
    ++result.executed_interval_examples;
  }

  if (manifest.blob_examples.size() != 1) {
    AddFailure(&result, "SB-DATATYPE-CONFORMANCE-MANIFEST-ROW-MISSING",
               "datatype.conformance.blob_v3_example_count",
               std::to_string(manifest.blob_examples.size()));
  }
  for (const auto& example : manifest.blob_examples) {
    if (example.source != DatatypeConformanceExampleSource::current_core_registry) {
      AddFailure(&result, "SB-DATATYPE-CONFORMANCE-DOCS-ONLY-EXAMPLE-REFUSED",
                 "datatype.conformance.blob_docs_only_refused");
      continue;
    }
    if (EvidencePathForbidden(example.evidence_path)) {
      AddFailure(&result, "SB-DATATYPE-CONFORMANCE-EVIDENCE-PATH-REFUSED",
                 "datatype.conformance.blob_evidence_path_refused", example.evidence_path);
      continue;
    }
    if (!IdentityMatchesReceipt(example.identity, example.receipt) ||
        !IsExactCanonicalBlobTypeCodecIdentityV3(example.identity)) {
      AddFailure(&result, "BLOB.DESCRIPTOR_INVALID",
                 "datatype.conformance.blob_identity_refused");
      continue;
    }
    const auto expected = BuildBlobValidatedProfileHandleV3(example.receipt, example.identity);
    if (!expected.ok() || !ValidateBlobProfileHandleV3(example.profile).ok() ||
        example.receipt.receipt_uuid != example.profile.receipt.receipt_uuid ||
        example.receipt.catalog_snapshot_uuid != example.profile.receipt.catalog_snapshot_uuid ||
        example.receipt.catalog_generation != example.profile.receipt.catalog_generation ||
        example.receipt.registry_generation != example.profile.receipt.registry_generation) {
      AddFailure(&result, "BLOB.DESCRIPTOR_INVALID",
                 "datatype.conformance.blob_profile_refused");
      continue;
    }
    const BlobMaterializedValueViewV3 view{&example.profile, example.state,
        example.logical_length, example.canonical_component};
    const auto checked = ValidateBlobMaterializedValueViewNoAllocV3(view, example.null_allowed);
    if (!checked.ok()) {
      AddFailure(&result, "SB-DATATYPE-CONFORMANCE-ENCODED-EXAMPLE-REFUSED",
                 "datatype.conformance.blob_component_refused",
                 std::to_string(static_cast<unsigned>(checked.diagnostic)));
      continue;
    }
    ++result.executed_examples;
    ++result.executed_blob_examples;
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
