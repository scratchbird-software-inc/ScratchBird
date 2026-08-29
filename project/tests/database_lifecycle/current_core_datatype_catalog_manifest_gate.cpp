// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "datatype_catalog_manifest.hpp"
#include <array>
#include <cstdlib>
#include <iostream>
#include <set>
#include <string_view>

namespace {

namespace dt = scratchbird::core::datatypes;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

bool HasDiagnostic(const dt::DatatypeCatalogManifestResult& result,
                   std::string_view diagnostic_code) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.diagnostic_code == diagnostic_code) {
      return true;
    }
  }
  return false;
}

bool TypedUuidEquals(const scratchbird::core::platform::TypedUuid& left,
                     const scratchbird::core::platform::TypedUuid& right) {
  return left.kind == right.kind && left.value == right.value;
}

void TestDescriptorCatalogLoadsAllCanonicalRows() {
  const auto loaded = dt::LoadCurrentCoreDatatypeCatalogManifest();
  Require(loaded.ok(), "MDF-012 catalog loader failed");
  Require(loaded.manifest.manifest_key ==
              dt::kCurrentCoreDatatypeCatalogManifestKey,
          "MDF-012 catalog manifest key mismatch");
  Require(loaded.manifest.descriptor_rows.size() ==
              dt::BuiltinDatatypeDescriptors().size(),
          "MDF-012 descriptor catalog row count mismatch");
  Require(loaded.manifest.layout_rows.size() ==
              dt::BuiltinDatatypeDescriptors().size(),
          "MDF-012 layout catalog row count mismatch");
  Require(loaded.manifest.trace_rows.size() ==
              dt::BuiltinDatatypeDescriptors().size(),
          "MDF-012 trace row count mismatch");

  std::set<std::string> sys_tables;
  for (const auto& row : loaded.manifest.descriptor_rows) {
    sys_tables.insert(row.sys_table_name);
    Require(row.descriptor_uuid.valid(), "MDF-012 descriptor UUID was nil");
    Require(row.descriptor_authoritative,
            "MDF-012 descriptor row must be authoritative");
    Require(row.reference_name_is_alias_only,
            "MDF-012 reference names must remain alias-only");
  }
  for (const auto& row : loaded.manifest.layout_rows) {
    sys_tables.insert(row.sys_table_name);
  }
  Require(sys_tables.count("sys.datatype_descriptor") == 1,
          "MDF-012 sys.datatype_descriptor missing");
  Require(sys_tables.count("sys.datatype_storage_layout") == 1,
          "MDF-012 sys.datatype_storage_layout missing");
}

void TestStableUuidAndCacheInvalidation() {
  const auto first = dt::LoadCurrentCoreDatatypeCatalogManifest();
  const auto second = dt::LoadCurrentCoreDatatypeCatalogManifest();
  Require(TypedUuidEquals(first.manifest.descriptor_rows.front().descriptor_uuid,
                          second.manifest.descriptor_rows.front().descriptor_uuid),
          "MDF-012 stable_descriptor_uuid changed across reload");

  dt::DatatypeCatalogCache cache;
  const auto loaded = cache.Load(first.manifest);
  Require(loaded.ok(), "MDF-012 catalog cache load failed");
  const auto generation = cache.generation();
  const auto looked_up = cache.Lookup(dt::CanonicalTypeId::uuid);
  Require(looked_up.ok(), "MDF-012 catalog cache lookup failed");
  cache.Invalidate();
  Require(cache.generation() == generation + 1,
          "MDF-012 catalog cache generation did not advance");
  const auto invalidated = cache.Lookup(dt::CanonicalTypeId::uuid);
  Require(!invalidated.ok(), "MDF-012 invalidated catalog cache was readable");
  Require(HasDiagnostic(invalidated,
                        "SB-DATATYPE-CATALOG-CACHE-INVALIDATED"),
          "MDF-012 cache invalidation diagnostic not emitted");
}

void TestFailClosedCatalogValidation() {
  auto loaded = dt::LoadCurrentCoreDatatypeCatalogManifest();
  loaded.manifest.descriptor_rows[0].descriptor_authoritative = false;
  const auto authority =
      dt::ValidateDatatypeCatalogManifest(loaded.manifest);
  Require(!authority.ok(),
          "MDF-012 accepted non-authoritative descriptor catalog row");
  Require(HasDiagnostic(authority,
                        "SB-DATATYPE-CATALOG-AUTHORITY-VIOLATION"),
          "MDF-012 authority diagnostic not emitted");

  loaded = dt::LoadCurrentCoreDatatypeCatalogManifest();
  loaded.manifest.trace_rows.pop_back();
  const auto missing_trace =
      dt::ValidateDatatypeCatalogManifest(loaded.manifest);
  Require(!missing_trace.ok(), "MDF-012 accepted missing trace row");
  Require(HasDiagnostic(missing_trace,
                        "SB-DATATYPE-CATALOG-TRACE-ROW-MISSING"),
          "MDF-012 missing trace diagnostic not emitted");

  const auto unknown =
      dt::LookupDatatypeCatalogRow(
          dt::LoadCurrentCoreDatatypeCatalogManifest().manifest,
          dt::CanonicalTypeId::unknown);
  Require(!unknown.ok(), "MDF-012 accepted unknown canonical type");
  Require(HasDiagnostic(
              unknown,
              "SB-DATATYPE-CATALOG-UNSUPPORTED-CANONICAL-TYPE"),
          "MDF-012 unsupported canonical type diagnostic not emitted");
}

void TestInt32ExactDescriptorTypeCodecIdentity() {
  constexpr std::string_view kDescriptorUuid =
      "019d0000-0000-7000-8000-00000000d716";
  constexpr std::string_view kTypeUuid =
      "019d0000-0000-7000-8000-00000000d717";
  const auto manifest = dt::LoadCurrentCoreDatatypeCatalogManifest();
  Require(manifest.ok(), "MDF-012 int32 catalog load failed");
  const auto int32_row = dt::LookupDatatypeCatalogRow(
      manifest.manifest, dt::CanonicalTypeId::int32);
  Require(int32_row.ok() && int32_row.manifest.descriptor_rows.size() == 1,
          "MDF-012 int32 descriptor row missing");
  const std::array<scratchbird::core::platform::byte, 16>
      expected_descriptor_bytes{0x01, 0x9d, 0x00, 0x00, 0x00, 0x00, 0x70,
                                0x00, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00,
                                0xd7, 0x16};
  Require(int32_row.manifest.descriptor_rows.front()
              .descriptor_uuid.value.bytes == expected_descriptor_bytes,
          "MDF-012 int32 descriptor UUID drifted");

  const auto identity = dt::LookupDatatypeTypeCodecIdentityV1(
      "019d0000-0000-7000-8000-00000000d701", 1, 1,
      std::string(kDescriptorUuid), 1);
  Require(identity.ok && identity.row.type_uuid == kTypeUuid &&
              identity.row.codec_id == "datatype.int32.le.v1" &&
              identity.row.codec_version == 1 &&
              identity.row.codec_generation == 1 &&
              identity.row.canonical_value_bytes == 4 &&
              identity.row.null_supported,
          "MDF-012 int32 descriptor/type/codec identity drifted");
  Require(!dt::LookupDatatypeTypeCodecIdentityV1(
               "019d0000-0000-7000-8000-00000000d701", 2, 1,
               std::string(kDescriptorUuid), 1)
               .ok,
          "MDF-012 stale int32 catalog generation was admitted");
  Require(!dt::LookupDatatypeTypeCodecIdentityV1(
               "019d0000-0000-7000-8000-00000000d701", 1, 1,
               std::string(kTypeUuid), 1)
               .ok,
          "MDF-012 int32 type UUID was accepted as descriptor authority");
}

void TestTextExactDescriptorTypeCodecIdentity() {
  constexpr std::string_view kSnapshotUuid =
      "019d0000-0000-7000-8000-00000000d701";
  constexpr std::string_view kDescriptorUuid =
      "019d0000-0000-7000-8000-00000000d718";
  constexpr std::string_view kTypeUuid =
      "019d0000-0000-7000-8000-00000000d719";
  constexpr std::string_view kCodecUuid =
      "019d0000-0000-7000-8000-00000000d71a";
  constexpr std::string_view kProvisionalUuid =
      "2c010000-6368-7172-a163-746572000000";

  const auto identity = dt::LookupDatatypeTypeCodecIdentityV1(
      std::string(kSnapshotUuid), 1, 1, std::string(kDescriptorUuid), 1);
  Require(identity.ok && identity.diagnostic_id.empty(),
          "MDF-012 canonical text identity lookup failed");
  const auto& row = identity.row;
  Require(row.canonical_name == "text" &&
              row.descriptor_uuid == kDescriptorUuid &&
              row.descriptor_generation == 1 && row.type_uuid == kTypeUuid &&
              row.type_generation == 1 && row.codec_uuid == kCodecUuid &&
              row.codec_id == "datatype.text.utf8.v1" &&
              row.codec_version == 1 && row.codec_generation == 1,
          "MDF-012 canonical text identity tuple drifted");
  Require(row.canonical_binary_type_code ==
                  static_cast<std::uint32_t>(dt::CanonicalTypeId::character) &&
              row.datatype_identity_code == 0 && row.byte_order_code == 0 &&
              row.representation_code == 0,
          "MDF-012 text binary type code was conflated with DUDV codes");
  Require(row.canonical_value_bytes == 0 &&
              row.canonical_value_minimum_bytes == 0 &&
              row.canonical_value_maximum_bytes == 16777216 &&
              row.canonical_value_exact_bytes == 0 &&
              row.canonical_value_variable_width &&
              row.canonical_value_exact_zero_is_width_marker &&
              row.canonical_byte_order == "byte_sequence" &&
              row.canonical_representation ==
                  "exact_well_formed_UTF8_scalar_sequence_without_implicit_normalization",
          "MDF-012 canonical text variable-width metadata drifted");
  Require(row.null_supported && row.null_encoding_code == 1 &&
              row.canonical_charset == "UTF-8" &&
              row.shortest_form_utf8_required &&
              !row.implicit_normalization_allowed &&
              row.descriptor_bound_collation_required &&
              row.empty_value_distinct_from_sql_null &&
              row.sql_null_requires_zero_payload &&
              row.variable_width_storage_without_truncation &&
              row.invalid_encoding_diagnostic_id ==
                  "CTB.TEXT.INVALID_ENCODING",
          "MDF-012 canonical text UTF-8/null/resource metadata drifted");
  Require(dt::IsExactCanonicalTextTypeCodecIdentityV1(row),
          "MDF-012 exact canonical text identity predicate refused the live row");

  const auto reject_text_lookalike = [&](auto mutate,
                                         std::string_view message) {
    auto lookalike = row;
    mutate(lookalike);
    Require(!dt::IsExactCanonicalTextTypeCodecIdentityV1(lookalike), message);
  };
  reject_text_lookalike(
      [](auto& candidate) {
        candidate.canonical_value_variable_width = false;
      },
      "MDF-012 text row without variable-width authority was admitted");
  reject_text_lookalike(
      [](auto& candidate) {
        candidate.canonical_value_exact_zero_is_width_marker = false;
      },
      "MDF-012 text row without the exact zero-width marker was admitted");
  reject_text_lookalike(
      [](auto& candidate) { candidate.canonical_value_maximum_bytes = 0; },
      "MDF-012 text row with a zero maximum width was admitted");
  reject_text_lookalike(
      [](auto& candidate) { candidate.implicit_normalization_allowed = true; },
      "MDF-012 text row with implicit normalization was admitted");
  reject_text_lookalike(
      [](auto& candidate) { candidate.null_encoding_code = 2; },
      "MDF-012 text row with a mismatched null encoding was admitted");
  reject_text_lookalike(
      [](auto& candidate) { candidate.catalog_generation = 2; },
      "MDF-012 stale text registry tuple was admitted");
  reject_text_lookalike(
      [](auto& candidate) {
        candidate.type_uuid = "019d0000-0000-7000-8000-00000000d71b";
      },
      "MDF-012 text type lookalike was admitted");

  const auto fixed = dt::LookupDatatypeTypeCodecIdentityV1(
      std::string(kSnapshotUuid), 1, 1,
      "019d0000-0000-7000-8000-00000000d716", 1);
  Require(fixed.ok, "MDF-012 fixed-width control identity lookup failed");
  auto fixed_zero = fixed.row;
  fixed_zero.canonical_value_bytes = 0;
  Require(!dt::IsExactCanonicalTextTypeCodecIdentityV1(fixed_zero),
          "MDF-012 fixed-width zero was admitted as the text width marker");

  const auto refuse = [&](std::string snapshot, std::uint64_t catalog_generation,
                          std::uint64_t registry_generation,
                          std::string descriptor,
                          std::uint64_t descriptor_generation,
                          std::string_view message) {
    const auto rejected = dt::LookupDatatypeTypeCodecIdentityV1(
        snapshot, catalog_generation, registry_generation, descriptor,
        descriptor_generation);
    Require(!rejected.ok &&
                rejected.diagnostic_id == "DATATYPE.DESCRIPTOR_INVALID",
            message);
  };
  refuse(std::string(kSnapshotUuid), 1, 1, std::string(kProvisionalUuid), 1,
         "MDF-012 provisional text identity was admitted");
  refuse(std::string(kSnapshotUuid), 1, 1, std::string(kTypeUuid), 1,
         "MDF-012 text type UUID was accepted as descriptor authority");
  refuse(std::string(kSnapshotUuid), 1, 1, std::string(kCodecUuid), 1,
         "MDF-012 text codec UUID was accepted as descriptor authority");
  refuse(std::string(kSnapshotUuid), 2, 1, std::string(kDescriptorUuid), 1,
         "MDF-012 stale text catalog generation was admitted");
  refuse(std::string(kSnapshotUuid), 1, 2, std::string(kDescriptorUuid), 1,
         "MDF-012 stale text registry generation was admitted");
  refuse(std::string(kSnapshotUuid), 1, 1, std::string(kDescriptorUuid), 2,
         "MDF-012 stale text descriptor generation was admitted");
  refuse("019d0000-0000-7000-8000-00000000d702", 1, 1,
         std::string(kDescriptorUuid), 1,
         "MDF-012 wrong text snapshot UUID was admitted");
}

}  // namespace

int main() {
  // MDF-012-CURRENT-CORE-DATATYPE-CATALOG-MANIFEST
  // DEFER-DTYPE-DESCRIPTOR-IMPLEMENTATION
  // DEFER-DTYPE-CATALOG-DDL
  // DEFER-DTYPE-CLOSURE-MATRIX-TRACE
  TestDescriptorCatalogLoadsAllCanonicalRows();
  TestStableUuidAndCacheInvalidation();
  TestFailClosedCatalogValidation();
  TestInt32ExactDescriptorTypeCodecIdentity();
  TestTextExactDescriptorTypeCodecIdentity();
  std::cout << "current_core_datatype_catalog_manifest_gate=passed\n";
  return EXIT_SUCCESS;
}
