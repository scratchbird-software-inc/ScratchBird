#include "../support/binary_uuid_fixture.hpp"
// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "datatype_catalog_manifest.hpp"
#include "datatype_storage_identity.hpp"
#include "datatype_binary.hpp"
#include "datatype_binary_view.hpp"
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
  constexpr auto kDescriptorUuid = scratchbird::tests::FixtureUuidLiteral("019d0000-0000-7000-8000-00000000d716");
  constexpr auto kTypeUuid = scratchbird::tests::FixtureUuidLiteral("019d0000-0000-7000-8000-00000000d717");
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
      scratchbird::tests::FixtureUuidLiteral("019d0000-0000-7000-8000-00000000d701"), 1, 1,
      kDescriptorUuid, 1);
  Require(identity.ok && identity.row.type_uuid == kTypeUuid &&
              identity.row.codec_id == "datatype.int32.le.v1" &&
              identity.row.codec_version == 1 &&
              identity.row.codec_generation == 1 &&
              identity.row.canonical_value_bytes == 4 &&
              identity.row.null_supported,
          "MDF-012 int32 descriptor/type/codec identity drifted");
  Require(!dt::LookupDatatypeTypeCodecIdentityV1(
               scratchbird::tests::FixtureUuidLiteral("019d0000-0000-7000-8000-00000000d701"), 2, 1,
               kDescriptorUuid, 1)
               .ok,
          "MDF-012 stale int32 catalog generation was admitted");
  Require(!dt::LookupDatatypeTypeCodecIdentityV1(
               scratchbird::tests::FixtureUuidLiteral("019d0000-0000-7000-8000-00000000d701"), 1, 1,
               kTypeUuid, 1)
               .ok,
          "MDF-012 int32 type UUID was accepted as descriptor authority");
}

void TestTextExactDescriptorTypeCodecIdentity() {
  constexpr auto kSnapshotUuid = scratchbird::tests::FixtureUuidLiteral("019d0000-0000-7000-8000-00000000d701");
  constexpr auto kDescriptorUuid = scratchbird::tests::FixtureUuidLiteral("019d0000-0000-7000-8000-00000000d718");
  constexpr auto kTypeUuid = scratchbird::tests::FixtureUuidLiteral("019d0000-0000-7000-8000-00000000d719");
  constexpr auto kCodecUuid = scratchbird::tests::FixtureUuidLiteral("019d0000-0000-7000-8000-00000000d71a");
  constexpr auto kProvisionalUuid = scratchbird::tests::FixtureUuidLiteral("2c010000-6368-7172-a163-746572000000");

  const auto identity = dt::LookupDatatypeTypeCodecIdentityV1(
      kSnapshotUuid, 1, 1, kDescriptorUuid, 1);
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
        candidate.type_uuid = scratchbird::tests::FixtureUuidLiteral("019d0000-0000-7000-8000-00000000d71b");
      },
      "MDF-012 text type lookalike was admitted");

  const auto fixed = dt::LookupDatatypeTypeCodecIdentityV1(
      kSnapshotUuid, 1, 1,
      scratchbird::tests::FixtureUuidLiteral("019d0000-0000-7000-8000-00000000d716"), 1);
  reject_text_lookalike([](auto& c) { c.numeric_context_generation=1; },
                       "text admitted decimal numeric policy generation");
  reject_text_lookalike([](auto& c) { c.allow_special_values=true; },
                       "text admitted numeric special value policy");
  reject_text_lookalike([](auto& c) { c.comparison_profile="decimal128_numeric_total_nan_last_v1"; },
                       "text admitted numeric comparison policy");
  Require(fixed.ok, "MDF-012 fixed-width control identity lookup failed");
  auto fixed_zero = fixed.row;
  fixed_zero.canonical_value_bytes = 0;
  Require(!dt::IsExactCanonicalTextTypeCodecIdentityV1(fixed_zero),
          "MDF-012 fixed-width zero was admitted as the text width marker");

  const auto refuse = [&](scratchbird::core::platform::Uuid snapshot, std::uint64_t catalog_generation,
                          std::uint64_t registry_generation,
                          scratchbird::core::platform::Uuid descriptor,
                          std::uint64_t descriptor_generation,
                          std::string_view message) {
    const auto rejected = dt::LookupDatatypeTypeCodecIdentityV1(
        snapshot, catalog_generation, registry_generation, descriptor,
        descriptor_generation);
    Require(!rejected.ok &&
                rejected.diagnostic_id == "DATATYPE.DESCRIPTOR.INVALID",
            message);
  };
  refuse(kSnapshotUuid, 1, 1, kProvisionalUuid, 1,
         "MDF-012 provisional text identity was admitted");
  refuse(kSnapshotUuid, 1, 1, kTypeUuid, 1,
         "MDF-012 text type UUID was accepted as descriptor authority");
  refuse(kSnapshotUuid, 1, 1, kCodecUuid, 1,
         "MDF-012 text codec UUID was accepted as descriptor authority");
  refuse(kSnapshotUuid, 2, 1, kDescriptorUuid, 1,
         "MDF-012 stale text catalog generation was admitted");
  refuse(kSnapshotUuid, 1, 2, kDescriptorUuid, 1,
         "MDF-012 stale text registry generation was admitted");
  refuse(kSnapshotUuid, 1, 1, kDescriptorUuid, 2,
         "MDF-012 stale text descriptor generation was admitted");
  refuse(scratchbird::tests::FixtureUuidLiteral("019d0000-0000-7000-8000-00000000d702"), 1, 1,
         kDescriptorUuid, 1,
         "MDF-012 wrong text snapshot UUID was admitted");
}

void TestFixedScalarSuccessorCohort() {
  using scratchbird::tests::FixtureUuidLiteral;
  constexpr auto snapshot = FixtureUuidLiteral("019d0000-0000-7000-8000-00000000d704");
  struct Expected {
    dt::CanonicalTypeId type;
    scratchbird::core::platform::Uuid descriptor, value_type, codec;
    const char* codec_id;
    std::uint32_t width;
  };
  const std::array<Expected, 18> expected{{
    {dt::CanonicalTypeId::int8,
     FixtureUuidLiteral("64000000-696e-7438-8000-000000000000"),
     FixtureUuidLiteral("019d0000-0000-7000-8000-00000000d800"),
     FixtureUuidLiteral("019d0000-0000-7000-8000-00000000d801"), "datatype.int8.le.v1", 1},
    {dt::CanonicalTypeId::int16,
     FixtureUuidLiteral("65000000-696e-7431-b600-000000000000"),
     FixtureUuidLiteral("019d0000-0000-7000-8000-00000000d802"),
     FixtureUuidLiteral("019d0000-0000-7000-8000-00000000d803"), "datatype.int16.le.v1", 2},
    {dt::CanonicalTypeId::uint8,
     FixtureUuidLiteral("78000000-7569-7e74-b800-000000000000"),
     FixtureUuidLiteral("019d0000-0000-7000-8000-00000000d804"),
     FixtureUuidLiteral("019d0000-0000-7000-8000-00000000d805"), "datatype.uint8.le.v1", 1},
    {dt::CanonicalTypeId::uint16,
     FixtureUuidLiteral("79000000-7569-7e74-b136-000000000000"),
     FixtureUuidLiteral("019d0000-0000-7000-8000-00000000d806"),
     FixtureUuidLiteral("019d0000-0000-7000-8000-00000000d807"), "datatype.uint16.le.v1", 2},
    {dt::CanonicalTypeId::uint32,
     FixtureUuidLiteral("7a000000-7569-7e74-b332-000000000000"),
     FixtureUuidLiteral("019d0000-0000-7000-8000-00000000d808"),
     FixtureUuidLiteral("019d0000-0000-7000-8000-00000000d809"), "datatype.uint32.le.v1", 4},
    {dt::CanonicalTypeId::uint128,
     FixtureUuidLiteral("7c000000-7569-7e74-b132-380000000000"),
     FixtureUuidLiteral("019d0000-0000-7000-8000-00000000d80a"),
     FixtureUuidLiteral("019d0000-0000-7000-8000-00000000d80b"), "datatype.uint128.le.v1", 16},
    {dt::CanonicalTypeId::bfloat16,
     FixtureUuidLiteral("8a000000-6266-7c6f-a174-313600000000"),
     FixtureUuidLiteral("019d0000-0000-7000-8000-00000000d80c"),
     FixtureUuidLiteral("019d0000-0000-7000-8000-00000000d80d"), "datatype.bfloat16.ieee754.le.v1", 2},
    {dt::CanonicalTypeId::real16,
     FixtureUuidLiteral("8b000000-7265-716c-b136-000000000000"),
     FixtureUuidLiteral("019d0000-0000-7000-8000-00000000d80e"),
     FixtureUuidLiteral("019d0000-0000-7000-8000-00000000d80f"), "datatype.real16.ieee754.le.v1", 2},
    {dt::CanonicalTypeId::real32,
     FixtureUuidLiteral("8c000000-7265-716c-b332-000000000000"),
     FixtureUuidLiteral("019d0000-0000-7000-8000-00000000d810"),
     FixtureUuidLiteral("019d0000-0000-7000-8000-00000000d811"), "datatype.real32.ieee754.le.v1", 4},
    {dt::CanonicalTypeId::real128,
     FixtureUuidLiteral("8e000000-7265-716c-b132-380000000000"),
     FixtureUuidLiteral("019d0000-0000-7000-8000-00000000d812"),
     FixtureUuidLiteral("019d0000-0000-7000-8000-00000000d813"), "datatype.real128.ieee754.le.v1", 16},
    {dt::CanonicalTypeId::ip_address,
     FixtureUuidLiteral("d2000000-6970-7f61-a464-726573730000"),
     FixtureUuidLiteral("019d0000-0000-7000-8000-00000000d814"),
     FixtureUuidLiteral("019d0000-0000-7000-8000-00000000d815"), "datatype.ip_address.network.v1", 16},
    {dt::CanonicalTypeId::network_prefix,
     FixtureUuidLiteral("d3000000-071d-7477-af72-6b5f70726566"),
     FixtureUuidLiteral("019d0000-0000-7000-8000-00000000d816"),
     FixtureUuidLiteral("019d0000-0000-7000-8000-00000000d817"), "datatype.network_prefix.network.v1", 18},
    {dt::CanonicalTypeId::mac_address,
     FixtureUuidLiteral("d4000000-6d61-735f-a164-647265737300"),
     FixtureUuidLiteral("019d0000-0000-7000-8000-00000000d818"),
     FixtureUuidLiteral("019d0000-0000-7000-8000-00000000d819"), "datatype.mac_address.network.v1", 8},
    {dt::CanonicalTypeId::enum_value,
     FixtureUuidLiteral("6c020000-656e-756d-9f76-616c75650000"),
     FixtureUuidLiteral("019d0000-0000-7000-8000-00000000d81a"),
     FixtureUuidLiteral("019d0000-0000-7000-8000-00000000d81b"), "datatype.enum_value.binary16.v1", 16},
    {dt::CanonicalTypeId::date,
     FixtureUuidLiteral("90010000-6461-7465-8000-000000000000"),
     FixtureUuidLiteral("019d0000-0000-7000-8000-00000000d81c"),
     FixtureUuidLiteral("019d0000-0000-7000-8000-00000000d81d"), "datatype.date.days.le.v1", 4},
    {dt::CanonicalTypeId::time,
     FixtureUuidLiteral("91010000-7469-7d65-8000-000000000000"),
     FixtureUuidLiteral("019d0000-0000-7000-8000-00000000d81e"),
     FixtureUuidLiteral("019d0000-0000-7000-8000-00000000d81f"), "datatype.time.nanos.le.v1", 8},
    {dt::CanonicalTypeId::timestamp,
     FixtureUuidLiteral("92010000-7469-7d65-b374-616d70000000"),
     FixtureUuidLiteral("019d0000-0000-7000-8000-00000000d820"),
     FixtureUuidLiteral("019d0000-0000-7000-8000-00000000d821"), "datatype.timestamp.utc_tuple.le.v1", 16},
    {dt::CanonicalTypeId::interval,
     FixtureUuidLiteral("93010000-696e-7465-b276-616c00000000"),
     FixtureUuidLiteral("019d0000-0000-7000-8000-00000000d822"),
     FixtureUuidLiteral("019d0000-0000-7000-8000-00000000d823"), "datatype.interval.tuple.le.v1", 16},
  }};
  const auto manifest = dt::LoadCurrentCoreDatatypeCatalogManifest();
  Require(manifest.ok(), "V4 manifest load failed");
  std::array<std::size_t, 5> counts{};
  for (const auto& row : dt::CurrentDatatypeTypeCodecIdentityRowsV1()) {
    Require(row.catalog_generation >= 1 && row.catalog_generation <= 5 &&
                row.registry_generation == row.catalog_generation,
            "codec cohort generation not exact");
    ++counts[row.catalog_generation - 1];
  }
  Require(counts == std::array<std::size_t, 5>{6, 12, 13, 31, 32},
          "successor changed immutable predecessor rows");
  std::set<scratchbird::core::platform::Uuid> identities;
  for (const auto& item : expected) {
    const auto descriptor = dt::LookupDatatypeCatalogRow(manifest.manifest, item.type);
    Require(descriptor.ok() && descriptor.manifest.descriptor_rows.size() == 1 &&
                descriptor.manifest.descriptor_rows.front().descriptor_uuid.value == item.descriptor,
            "V4 manifest descriptor identity differs from registry");
    const auto binding = dt::LookupDatatypeTypeCodecIdentityV1(snapshot, 4, 4, item.descriptor, 1);
    Require(binding.ok && binding.row.type_uuid == item.value_type &&
                binding.row.type_generation == 1 && binding.row.codec_uuid == item.codec &&
                binding.row.codec_id == item.codec_id && binding.row.codec_version == 1 &&
                binding.row.codec_generation == 1 && binding.row.canonical_value_bytes == item.width &&
                binding.row.canonical_value_minimum_bytes == item.width &&
                binding.row.canonical_value_maximum_bytes == item.width &&
                binding.row.canonical_value_exact_bytes == item.width &&
                !binding.row.canonical_value_variable_width && binding.row.null_supported &&
                binding.row.sql_null_requires_zero_payload &&
                binding.row.canonical_binary_type_code == static_cast<std::uint32_t>(item.type),
            "V4 exact scalar tuple or representation differs");
    for (const auto identity : {item.descriptor, item.value_type, item.codec})
      Require(identities.insert(identity).second, "V4 descriptor/type/codec identities alias");
    for (unsigned bit = 0; bit < 128; ++bit) {
      auto altered = item.descriptor;
      altered.bytes[bit / 8] ^= static_cast<std::uint8_t>(1u << (bit % 8));
      Require(!dt::LookupDatatypeTypeCodecIdentityV1(snapshot, 4, 4, altered, 1).ok,
              "V4 altered descriptor admitted");
      auto other_snapshot = snapshot;
      other_snapshot.bytes[bit / 8] ^= static_cast<std::uint8_t>(1u << (bit % 8));
      Require(!dt::LookupDatatypeTypeCodecIdentityV1(other_snapshot, 4, 4, item.descriptor, 1).ok,
              "V4 altered snapshot admitted");
    }
    Require(!dt::LookupDatatypeTypeCodecIdentityV1(snapshot, 3, 4, item.descriptor, 1).ok &&
                !dt::LookupDatatypeTypeCodecIdentityV1(snapshot, 4, 3, item.descriptor, 1).ok &&
                !dt::LookupDatatypeTypeCodecIdentityV1(snapshot, 4, 4, item.descriptor, 2).ok &&
                !dt::LookupDatatypeTypeCodecIdentityV1(snapshot, 4, 4, item.value_type, 1).ok &&
                !dt::LookupDatatypeTypeCodecIdentityV1(snapshot, 4, 4, item.codec, 1).ok,
            "V4 mismatched cohort or substituted identity admitted");
    for (unsigned generation = 1; generation <= 3; ++generation) {
      auto predecessor = snapshot;
      predecessor.bytes.back() = static_cast<std::uint8_t>(generation);
      Require(!dt::LookupDatatypeTypeCodecIdentityV1(predecessor, generation, generation,
                                                    item.descriptor, 1).ok,
              "V4-only scalar leaked into predecessor cohort");
    }
  }
}

void TestDecimal128SuccessorCohort() {
  using scratchbird::tests::FixtureUuidLiteral;
  using Row = dt::DatatypeTypeCodecIdentityRowV1;
  const auto descriptor = FixtureUuidLiteral("a1000000-1065-7369-ad61-6c5f666c6f61");
  const auto type = FixtureUuidLiteral("019d0000-0000-7000-8000-00000000d824");
  const auto lookup = dt::LookupDatatypeTypeCodecIdentityV1(dt::kDatatypeCohortV5,5,5,descriptor,1);
  Require(lookup.ok && dt::IsExactCanonicalDecimal128TypeCodecIdentityV1(lookup.row),
          "decimal128 exact policy row missing");
  const auto& row = lookup.row;
  Require(row.type_uuid == type && row.codec_id == "datatype.decimal128.bid.le.v1" &&
          row.codec_uuid == FixtureUuidLiteral("019d0000-0000-7000-8000-00000000d825") &&
          row.numeric_context_uuid == FixtureUuidLiteral("019d0000-0000-7000-8000-00000000d826") &&
          row.special_value_policy_uuid == FixtureUuidLiteral("019d0000-0000-7000-8000-00000000d827") &&
          row.comparison_policy_uuid == FixtureUuidLiteral("019d0000-0000-7000-8000-00000000d828") &&
          row.comparison_profile == "decimal128_numeric_total_nan_last_v1" &&
          row.allow_special_values && row.canonical_value_bytes == 16,
          "decimal128 identity or policy differs from independent registry oracle");
  for (const auto member : {&Row::catalog_snapshot_uuid, &Row::descriptor_uuid, &Row::type_uuid,
       &Row::codec_uuid, &Row::numeric_context_uuid, &Row::special_value_policy_uuid,
       &Row::comparison_policy_uuid}) {
    for (unsigned bit = 0; bit < 128; ++bit) {
      auto changed = row;
      (changed.*member).bytes[bit/8] ^= static_cast<std::uint8_t>(1u << (bit%8));
      Require(!dt::IsExactCanonicalDecimal128TypeCodecIdentityV1(changed),
              "decimal128 changed binary authority admitted");
    }
  }
  for (const auto member : {&Row::catalog_generation, &Row::registry_generation,
       &Row::descriptor_generation, &Row::type_generation, &Row::codec_generation,
       &Row::numeric_context_generation, &Row::special_value_policy_generation,
       &Row::comparison_policy_generation}) {
    for (const auto bad : {0ULL, 2ULL, 4ULL, 6ULL, ~0ULL}) {
      auto changed = row;
      changed.*member = bad;
      Require(!dt::IsExactCanonicalDecimal128TypeCodecIdentityV1(changed),
              "decimal128 changed generation admitted");
    }
  }
  auto changed = row;
  changed.comparison_profile = "decimal128_ieee_total_order_v1";
  Require(!dt::IsExactCanonicalDecimal128TypeCodecIdentityV1(changed), "unbound order policy accepted");
  changed = row; changed.allow_special_values = false;
  Require(!dt::IsExactCanonicalDecimal128TypeCodecIdentityV1(changed), "unbound special policy accepted");
  dt::DatatypeStorageIdentityV1 storage;
  Require(dt::LookupDatatypeStorageIdentityV1(dt::kDatatypeCohortV5,5,5,descriptor,1,&storage) &&
          storage.type_uuid == type && storage.codec &&
          dt::IsExactCanonicalDecimal128TypeCodecIdentityV1(*storage.codec), "storage lost decimal policy binding");
  Require(dt::LookupDatatypeStorageIdentityV1(dt::kDatatypeCohortV4,4,4,descriptor,1,&storage) &&
          storage.type_uuid == descriptor && !storage.codec, "successor reinterpreted predecessor storage identity");
  for (unsigned generation = 1; generation <= 4; ++generation) {
    auto snapshot = dt::kDatatypeCohortV5;
    snapshot.bytes.back() = generation;
    Require(!dt::LookupDatatypeTypeCodecIdentityV1(snapshot,generation,generation,descriptor,1).ok,
            "decimal codec leaked into predecessor");
  }
  for (const auto& old : dt::CurrentDatatypeTypeCodecIdentityRowsV1()) {
    if (old.catalog_snapshot_uuid != dt::kDatatypeCohortV4) continue;
    const auto inherited = dt::LookupDatatypeTypeCodecIdentityV1(dt::kDatatypeCohortV5,5,5,
        old.descriptor_uuid,old.descriptor_generation);
    Require(inherited.ok && inherited.row.type_uuid == old.type_uuid &&
            inherited.row.codec_uuid == old.codec_uuid && inherited.row.codec_id == old.codec_id &&
            inherited.row.canonical_value_bytes == old.canonical_value_bytes &&
            inherited.row.canonical_representation == old.canonical_representation &&
            inherited.row.numeric_context_uuid.is_nil(), "V5 changed inherited codec semantics");
  }
  std::array<std::uint8_t,16> one{};
  one[0]=1; one[14]=0x40; one[15]=0x30;
  auto valid = [&](const auto& bytes, std::size_t size) {
    return dt::ValidateDatatypeBinaryValueView({dt::CanonicalTypeId::decimal_float,false,false,
        bytes.data(),size}).ok();
  };
  Require(valid(one,16), "canonical decimal128 binary one rejected");
  for (const auto size : {0U,1U,15U}) Require(!valid(one,size), "truncated decimal128 admitted");
  auto bad=one; bad[15]=0x60;
  Require(!valid(bad,16), "noncanonical steering encoding admitted");
  bad.fill(0xff);
  Require(!valid(bad,16), "noncanonical NaN admitted");
  for (const auto special : {0x78,0xf8,0x7c,0xfc,0x7e,0xfe}) {
    std::array<std::uint8_t,16> bytes{}; bytes[15]=special;
    Require(valid(bytes,16), "canonical decimal special value rejected structurally");
  }
  Require(dt::ValidateDatatypeBinaryValueView({dt::CanonicalTypeId::decimal_float,true,false,nullptr,0}).ok() &&
          !dt::ValidateDatatypeBinaryValueView({dt::CanonicalTypeId::decimal_float,true,false,one.data(),16}).ok(),
          "decimal128 NULL state lost");
}

}  // namespace

int main() {
  TestFixedScalarSuccessorCohort();
  TestDecimal128SuccessorCohort();
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
