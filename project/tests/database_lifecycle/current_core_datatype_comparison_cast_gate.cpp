// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "datatype_operations.hpp"
#include "datatype_catalog_manifest.hpp"
#include "datatype_document.hpp"
#include "datatype_exchange.hpp"
#include "datatype_timestamp.hpp"
#include "resource_seed_pack.hpp"
#include "sbl_numeric.hpp"
#include "../support/binary_uuid_fixture.hpp"

#include <algorithm>
#include <cstdlib>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <vector>
#include <string>
#include <string_view>
#include <utility>

namespace {

namespace dt = scratchbird::core::datatypes;
namespace numeric = scratchbird::libraries::sbl_numeric;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

std::string DiagnosticDetail(
    const scratchbird::core::platform::DiagnosticRecord& diagnostic) {
  for (const auto& argument : diagnostic.arguments) {
    if (argument.key == "detail") {
      const auto* value = argument.text();
      return value == nullptr ? std::string{} : *value;
    }
  }
  return {};
}

scratchbird::engine::ExecutionTypeDescriptor Descriptor(
    dt::CanonicalTypeId type_id) {
  static const auto manifest = dt::LoadCurrentCoreDatatypeCatalogManifest();
  Require(manifest.ok(), "MDF-014 datatype catalog authority unavailable");
  const auto row = dt::LookupDatatypeCatalogRow(manifest.manifest, type_id);
  Require(row.ok() && row.manifest.descriptor_rows.size() == 1,
          "MDF-014 datatype descriptor authority unavailable");
  dt::CatalogExecutionTypeMetadata metadata;
  metadata.descriptor_uuid = row.manifest.descriptor_rows.front().descriptor_uuid;
  metadata.descriptor_epoch =
      row.manifest.descriptor_rows.front().descriptor_epoch;
  const auto descriptor =
      dt::LookupExecutionTypeDescriptorFromCatalog(type_id, metadata);
  Require(descriptor.ok(), "MDF-014 execution descriptor build failed");
  return descriptor.descriptor;
}

dt::DatatypeOperationValue TypedNull(dt::CanonicalTypeId type_id) {
  dt::DatatypeOperationValue value{type_id, {}, true};
  value.descriptor = Descriptor(type_id);
  return value;
}

dt::DatatypeOperationValue Value(dt::CanonicalTypeId type,
                                 std::string encoded) {
  return {type, std::move(encoded), false};
}

dt::DatatypeOperationValue UuidValue(std::string encoded) {
  auto value = Value(dt::CanonicalTypeId::uuid, std::move(encoded));
  static const auto descriptor = Descriptor(dt::CanonicalTypeId::uuid);
  value.descriptor = descriptor;
  return value;
}

dt::DatatypeOperationValue Int32Value(std::int64_t number) {
  std::string encoded;
  Require(dt::EncodeCanonicalInt32Value(number, &encoded),
          "MDF-014 int32 fixture encoding failed");
  return Value(dt::CanonicalTypeId::int32, std::move(encoded));
}

dt::DatatypeOperationValue Int64Value(std::int64_t number) {
  std::string encoded;
  Require(dt::EncodeCanonicalInt64Value(number, &encoded),
          "MDF-014 int64 fixture encoding failed");
  auto value = Value(dt::CanonicalTypeId::int64, std::move(encoded));
  static const auto descriptor = Descriptor(dt::CanonicalTypeId::int64);
  value.descriptor = descriptor;
  return value;
}

dt::DatatypeOperationValue Uint64Value(std::uint64_t number) {
  std::string encoded;
  Require(dt::EncodeCanonicalUint64Value(number, &encoded),
          "MDF-014 uint64 fixture encoding failed");
  auto value = Value(dt::CanonicalTypeId::uint64, std::move(encoded));
  static const auto descriptor = Descriptor(dt::CanonicalTypeId::uint64);
  value.descriptor = descriptor;
  return value;
}

dt::DatatypeOperationValue Int128Value(std::string_view number) {
  std::string encoded;
  Require(dt::EncodeCanonicalInt128Value(number, &encoded),
          "MDF-014 int128 fixture encoding failed");
  auto value = Value(dt::CanonicalTypeId::int128, std::move(encoded));
  static const auto descriptor = Descriptor(dt::CanonicalTypeId::int128);
  value.descriptor = descriptor;
  return value;
}

bool IsCanonicalInt128(const dt::DatatypeOperationValue& value,
                       std::string_view expected) {
  std::string decoded;
  return value.type_id == dt::CanonicalTypeId::int128 && !value.is_null &&
      dt::DecodeCanonicalInt128Value(value.encoded_value, &decoded) &&
      decoded == expected;
}

dt::DatatypeOperationValue Uint128Value(std::string_view number) {
  std::string encoded;
  Require(dt::EncodeCanonicalUint128Value(number, &encoded),
          "MDF-014 uint128 fixture encoding failed");
  auto value = Value(dt::CanonicalTypeId::uint128, std::move(encoded));
  static const auto descriptor = Descriptor(dt::CanonicalTypeId::uint128);
  value.descriptor = descriptor;
  return value;
}

dt::DatatypeOperationValue DecimalValue(std::string_view lexical) {
  const auto encoded = numeric::EncodeExactDecimalLittleEndian(lexical);
  Require(encoded.ok, "MDF-014 exact decimal fixture encoding failed");
  auto value = Value(
      dt::CanonicalTypeId::decimal,
      std::string(reinterpret_cast<const char*>(encoded.canonical_bytes.data()),
                  encoded.canonical_bytes.size()));
  static const auto descriptor = Descriptor(dt::CanonicalTypeId::decimal);
  value.descriptor = descriptor;
  return value;
}

bool IsCanonicalUint64(const dt::DatatypeOperationValue& value,
                       std::uint64_t expected) {
  std::uint64_t decoded = 0;
  return value.type_id == dt::CanonicalTypeId::uint64 && !value.is_null &&
         dt::DecodeCanonicalUint64Value(value.encoded_value, &decoded) &&
         decoded == expected;
}

bool IsCanonicalBoolean(const dt::DatatypeOperationValue& value,
                        bool expected) {
  return value.type_id == dt::CanonicalTypeId::boolean && !value.is_null &&
         value.encoded_value.size() == 1 &&
         static_cast<unsigned char>(value.encoded_value[0]) ==
             (expected ? 1u : 0u);
}

std::string Utf8Bytes(std::initializer_list<unsigned char> bytes) {
  std::string out;
  out.reserve(bytes.size());
  for (const unsigned char byte : bytes) {
    out.push_back(static_cast<char>(byte));
  }
  return out;
}

dt::DatatypeTextSeedAuthority TextSeed(
    std::string collation_name = "UNICODE_CI",
    bool case_insensitive = true,
    bool accent_insensitive = false,
    std::string charset_name = "UTF8") {
  dt::DatatypeTextSeedAuthority seed;
  namespace resources = scratchbird::core::resources;
  static const auto loaded = [] {
    resources::ResourceSeedLoadConfig config;
    config.seed_pack_root = SB_BOOTSTRAP_SEED_PACK_ROOT;
    return resources::LoadResourceSeedPack(config);
  }();
  Require(loaded.ok() && loaded.image.unicode_collation,
          "MDF-014 actual admitted Unicode collation seed unavailable");
  const auto profile = case_insensitive
      ? (accent_insensitive ? resources::CollationProfile::uca17_root_primary
                            : resources::CollationProfile::uca17_root_secondary)
      : resources::CollationProfile::utf8_binary;
  const auto recipe = std::find_if(loaded.image.collations.begin(), loaded.image.collations.end(),
      [&](const auto& row) { return row.comparison_profile == profile; });
  Require(recipe != loaded.image.collations.end() &&
              recipe->case_insensitive == case_insensitive &&
              recipe->accent_insensitive == accent_insensitive,
          "MDF-014 collation recipe disagrees with fixture semantics");
  // Component-level bound cohort, with actual admitted UCA data. Display
  // names below deliberately carry no comparison or sort-key authority.
  seed.database_uuid = scratchbird::tests::FixtureUuid(2056, 1);
  seed.charset_uuid = scratchbird::tests::FixtureUuid(2056, 2);
  seed.collation_uuid = scratchbird::tests::FixtureUuid(2056, 10 + static_cast<unsigned>(profile));
  seed.resource_epoch = loaded.image.resource_epoch;
  seed.collation_epoch = loaded.image.collation_epoch;
  seed.comparison_profile = profile;
  if (resources::UsesUnicodeRoot(profile)) seed.unicode_collation = loaded.image.unicode_collation;
  seed.active = true;
  seed.seed_pack_name = "initial-resource-pack";
  seed.seed_pack_version = "1";
  seed.charset_name = std::move(charset_name);
  seed.collation_name = std::move(collation_name);
  seed.collation_case_insensitive = case_insensitive;
  seed.collation_accent_insensitive = accent_insensitive;
  return seed;
}

void RequireBinaryComparisonCohort(const std::string& key,
                                   const dt::DatatypeTextSeedAuthority& seed) {
  std::string expected = "20:";
  for (const auto& id : {seed.database_uuid, seed.charset_uuid, seed.collation_uuid})
    expected.append(reinterpret_cast<const char*>(id.bytes.data()), id.bytes.size());
  for (const auto value : {seed.resource_epoch, seed.collation_epoch,
                           static_cast<std::uint64_t>(seed.comparison_profile)})
    for (unsigned byte = 0; byte < 8; ++byte)
      expected.push_back(static_cast<char>(value >> (byte * 8)));
  Require(key.size() > expected.size() && key.compare(0, expected.size(), expected) == 0,
          "MDF-014 sort key lost exact binary resource cohort");
  Require(key.find("initial-resource-pack") == std::string::npos &&
              key.find("UNICODE_CI") == std::string::npos,
          "MDF-014 sort key used display names as resource authority");
}

void RequireCompareEqual(const dt::DatatypeTextSeedAuthority& seed,
                         const std::string& left,
                         const std::string& right,
                         std::string_view message) {
  dt::DatatypeComparisonRequest compare;
  compare.left = Value(dt::CanonicalTypeId::character, left);
  compare.right = Value(dt::CanonicalTypeId::character, right);
  compare.case_insensitive_character_compare = seed.collation_case_insensitive;
  compare.text_seed = seed;
  const auto result = dt::CompareDatatypeValues(compare);
  Require(result.ok() && result.comparison == 0, message);
}

void TestOrderedKeysAndResourceBoundComparison() {
  const std::vector<dt::DatatypeOperationValue> signed_values = {
      Int64Value(-257), Int64Value(-1), Int64Value(0), Int64Value(2),
      Int64Value(256)};
  for (std::size_t index = 1; index < signed_values.size(); ++index) {
    const auto compare = dt::CompareDatatypeValues(
        {signed_values[index - 1], signed_values[index]});
    Require(compare.ok() && compare.comparison < 0,
            "MDF-014 signed integer comparison order drifted");
    const auto left = dt::MakeDatatypeSortKey({signed_values[index - 1]});
    const auto right = dt::MakeDatatypeSortKey({signed_values[index]});
    Require(left.ok() && right.ok() && left.sort_key < right.sort_key,
            "MDF-014 signed integer sort key order drifted");
  }

  const std::vector<dt::DatatypeOperationValue> unsigned_values = {
      Uint64Value(2), Uint64Value(256),
      Uint64Value(std::numeric_limits<std::uint64_t>::max())};
  for (std::size_t index = 1; index < unsigned_values.size(); ++index) {
    const auto compare = dt::CompareDatatypeValues(
        {unsigned_values[index - 1], unsigned_values[index]});
    Require(compare.ok() && compare.comparison < 0,
            "MDF-014 unsigned integer comparison order drifted");
    const auto left = dt::MakeDatatypeSortKey({unsigned_values[index - 1]});
    const auto right = dt::MakeDatatypeSortKey({unsigned_values[index]});
    Require(left.ok() && right.ok() && left.sort_key < right.sort_key,
            "MDF-014 unsigned integer sort key order drifted");
  }

  const std::vector<dt::DatatypeOperationValue> decimal_values = {
      DecimalValue("-1.20"), DecimalValue("-1.10"), DecimalValue("0"),
      DecimalValue("2.10"), DecimalValue("10.01")};
  for (std::size_t index = 1; index < decimal_values.size(); ++index) {
    const auto compare = dt::CompareDatatypeValues(
        {decimal_values[index - 1], decimal_values[index]});
    Require(!compare.ok() &&
                compare.diagnostic.diagnostic_code ==
                    "SB_DATATYPE_COMPARISON_REJECTED" &&
                DiagnosticDetail(compare.diagnostic) ==
                    "decimal_comparison_policy_unresolved",
            "MDF-014 decimal comparison did not fail at unresolved policy");
    const auto left = dt::MakeDatatypeSortKey({decimal_values[index - 1]});
    const auto right = dt::MakeDatatypeSortKey({decimal_values[index]});
    Require(!left.ok() && !right.ok() &&
                left.diagnostic.diagnostic_code ==
                    "SB_DATATYPE_SORT_KEY_REJECTED" &&
                right.diagnostic.diagnostic_code ==
                    "SB_DATATYPE_SORT_KEY_REJECTED" &&
                DiagnosticDetail(left.diagnostic) ==
                    "decimal_sort_key_policy_unresolved" &&
                DiagnosticDetail(right.diagnostic) ==
                    "decimal_sort_key_policy_unresolved",
            "MDF-014 decimal sort key did not fail at unresolved policy");
  }

  dt::DatatypeOperationValue null_value;
  null_value.type_id = dt::CanonicalTypeId::int64;
  null_value.is_null = true;
  null_value.descriptor = Descriptor(dt::CanonicalTypeId::int64);
  const auto nulls_first = dt::CompareDatatypeValues(
      {null_value, Int64Value(0),
       dt::DatatypeNullOrdering::nulls_first});
  Require(!nulls_first.ok(),
          "MDF-014 unresolved int64 NULL comparison did not fail closed");
  const auto nulls_last = dt::CompareDatatypeValues(
      {null_value, Int64Value(0),
       dt::DatatypeNullOrdering::nulls_last});
  Require(!nulls_last.ok(),
          "MDF-014 unresolved int64 NULL ordering did not infer policy");

  dt::DatatypeComparisonRequest compare;
  compare.left = Value(dt::CanonicalTypeId::character, "Alpha");
  compare.right = Value(dt::CanonicalTypeId::character, "alpha");
  compare.case_insensitive_character_compare = true;
  compare.text_seed = TextSeed();
  const auto equal = dt::CompareDatatypeValues(compare);
  Require(equal.ok() && equal.comparison == 0,
          "MDF-014 resource-bound collation compare failed");

  compare.text_seed.active = false;
  const auto missing_seed = dt::CompareDatatypeValues(compare);
  Require(!missing_seed.ok(),
          "MDF-014 accepted collation compare without resource seed");
  Require(missing_seed.diagnostic.diagnostic_code ==
              "SB_DATATYPE_COMPARISON_REJECTED",
          "MDF-014 missing collation diagnostic mismatch");

  dt::DatatypeSortKeyRequest text_key;
  text_key.value = Value(dt::CanonicalTypeId::character, "Zulu");
  text_key.case_insensitive_character_compare = true;
  text_key.text_seed = TextSeed();
  const auto sort_key = dt::MakeDatatypeSortKey(text_key);
  Require(sort_key.ok(), "MDF-014 text sort key failed");
  RequireBinaryComparisonCohort(sort_key.sort_key, text_key.text_seed);
  auto renamed = text_key;
  renamed.text_seed.seed_pack_name = "untrusted display label";
  renamed.text_seed.charset_name = "not a charset authority";
  renamed.text_seed.collation_name = "not a collation authority";
  const auto unchanged = dt::MakeDatatypeSortKey(renamed);
  Require(unchanged.ok() && unchanged.sort_key == sort_key.sort_key,
          "MDF-014 display name changed bound comparison semantics");
  auto next_epoch = text_key;
  ++next_epoch.text_seed.collation_epoch;
  const auto rekeyed = dt::MakeDatatypeSortKey(next_epoch);
  Require(rekeyed.ok() && rekeyed.sort_key != sort_key.sort_key,
          "MDF-014 collation epoch failed to separate binary sort-key cohorts");
  for (unsigned mutation = 0; mutation < 8; ++mutation) {
    auto invalid = text_key;
    switch (mutation) {
      case 0: invalid.text_seed.database_uuid = {}; break;
      case 1: invalid.text_seed.charset_uuid = {}; break;
      case 2: invalid.text_seed.collation_uuid.bytes[6] = 0x40; break;
      case 3: invalid.text_seed.resource_epoch = 0; break;
      case 4: invalid.text_seed.collation_epoch = 0; break;
      case 5: invalid.text_seed.comparison_profile = scratchbird::core::resources::CollationProfile::unbound; break;
      case 6: invalid.text_seed.unicode_collation.reset(); break;
      case 7: invalid.text_seed.collation_accent_insensitive = true; break;
    }
    Require(!dt::MakeDatatypeSortKey(invalid).ok(),
            "MDF-014 incomplete or crossed binary collation authority accepted");
  }
}

void TestLocaleSpecificCharacterCollationProof() {
  const auto unicode_ci = TextSeed("UNICODE_CI", true, false);
  RequireCompareEqual(unicode_ci,
                      Utf8Bytes({0xc3, 0x89}) + "cole",
                      Utf8Bytes({0xc3, 0xa9}) + "cole",
                      "MDF-014 Unicode CI did not fold accented case");

  dt::DatatypeComparisonRequest accent_sensitive;
  accent_sensitive.left =
      Value(dt::CanonicalTypeId::character, Utf8Bytes({0xc3, 0x89}) + "cole");
  accent_sensitive.right = Value(dt::CanonicalTypeId::character, "Ecole");
  accent_sensitive.case_insensitive_character_compare = true;
  accent_sensitive.text_seed = unicode_ci;
  const auto accent_sensitive_result =
      dt::CompareDatatypeValues(accent_sensitive);
  Require(accent_sensitive_result.ok() &&
              accent_sensitive_result.comparison != 0,
          "MDF-014 Unicode CI incorrectly folded accents");

  const auto unicode_ci_ai = TextSeed("UNICODE_CI_AI", true, true);
  RequireCompareEqual(unicode_ci_ai,
                      Utf8Bytes({0xc3, 0x89}) + "cole",
                      "ECOLE",
                      "MDF-014 Unicode CI_AI did not fold French accents");
  RequireCompareEqual(unicode_ci_ai,
                      "Stra" + Utf8Bytes({0xc3, 0x9f}) + "e",
                      "STRASSE",
                      "MDF-014 Unicode CI_AI did not fold German sharp-s");
  RequireCompareEqual(TextSeed("ES_ES_CI_AI", true, true, "ISO8859_1"),
                      "ca" + Utf8Bytes({0xc3, 0xb1}) + Utf8Bytes({0xc3, 0xb3}) + "n",
                      "CANON",
                      "MDF-014 Spanish CI_AI did not fold tilde/accent");

  dt::DatatypeSortKeyRequest accent_key_a;
  accent_key_a.value =
      Value(dt::CanonicalTypeId::character, Utf8Bytes({0xc3, 0x89}) + "cole");
  accent_key_a.case_insensitive_character_compare = true;
  accent_key_a.text_seed = unicode_ci_ai;
  dt::DatatypeSortKeyRequest accent_key_b = accent_key_a;
  accent_key_b.value = Value(dt::CanonicalTypeId::character, "ECOLE");
  const auto sort_a = dt::MakeDatatypeSortKey(accent_key_a);
  const auto sort_b = dt::MakeDatatypeSortKey(accent_key_b);
  Require(sort_a.ok() && sort_b.ok() && sort_a.sort_key == sort_b.sort_key,
          "MDF-014 accent-insensitive sort key mismatch");
  RequireBinaryComparisonCohort(sort_a.sort_key, accent_key_a.text_seed);

  dt::DatatypeComparisonRequest mode_mismatch;
  mode_mismatch.left = Value(dt::CanonicalTypeId::character, "Alpha");
  mode_mismatch.right = Value(dt::CanonicalTypeId::character, "alpha");
  mode_mismatch.case_insensitive_character_compare = true;
  mode_mismatch.text_seed = TextSeed("BINARY", false, false);
  const auto mismatch = dt::CompareDatatypeValues(mode_mismatch);
  Require(!mismatch.ok() &&
              mismatch.diagnostic.diagnostic_code ==
                  "SB_DATATYPE_COMPARISON_REJECTED",
          "MDF-014 accepted case-insensitive compare with binary collation");
}

void TestNumericOperationsUseTypedSemantics() {
  dt::DatatypeNumericOperationRequest add;
  add.operation = dt::DatatypeNumericOperationKind::add;
  add.type_id = dt::CanonicalTypeId::int128;
  add.left = Int128Value("170141183460469231731687303715884105726");
  add.right = Int128Value("1");
  const auto added = dt::ApplyNumericOperation(add);
  Require(added.ok() && IsCanonicalInt128(
              added.value, "170141183460469231731687303715884105727"),
          "MDF-014 int128 numeric add drifted");

  dt::DatatypeNumericOperationRequest compare;
  compare.operation = dt::DatatypeNumericOperationKind::compare;
  compare.type_id = dt::CanonicalTypeId::uint128;
  compare.left = Uint128Value("255");
  compare.right = Uint128Value("256");
  const auto compared = dt::ApplyNumericOperation(compare);
  Require(compared.ok() && compared.comparison < 0,
          "MDF-014 uint128 numeric compare drifted");
}

void TestCastPersistenceAndSilentDowngradeRefusal() {
  dt::DatatypeCastRequest cast;
  cast.value = Value(dt::CanonicalTypeId::character,
                     "170141183460469231731687303715884105727");
  cast.target_type_id = dt::CanonicalTypeId::int128;
  cast.explicit_cast = true;
  cast.target_descriptor = Descriptor(dt::CanonicalTypeId::int128);
  const auto int128_cast = dt::CastDatatypeValue(cast);
  Require(int128_cast.ok() && IsCanonicalInt128(
              int128_cast.value,
              "170141183460469231731687303715884105727"),
          "MDF-014 int128 cast failed");

  const auto serialized =
      dt::SerializeDatatypeValue({int128_cast.value});
  Require(serialized.ok(), "MDF-014 cast serialization failed");
  const auto deserialized =
      dt::DeserializeDatatypeValue({dt::CanonicalTypeId::int128,
                                    serialized.serialized_value,
                                    int128_cast.value.descriptor});
  Require(deserialized.ok(), "MDF-014 cast persistence decode failed");
  Require(deserialized.value.type_id == dt::CanonicalTypeId::int128,
          "MDF-014 persisted cast target type mismatch");
  Require(deserialized.value.encoded_value == int128_cast.value.encoded_value,
          "MDF-014 persisted cast payload mismatch");

  cast.value = Value(dt::CanonicalTypeId::int32, "1000");
  cast.target_type_id = dt::CanonicalTypeId::int8;
  cast.target_descriptor = {};
  cast.explicit_cast = true;
  const auto precision_loss = dt::CastDatatypeValue(cast);
  Require(!precision_loss.ok(), "MDF-014 accepted precision-losing int8 cast");
  Require(precision_loss.diagnostic.diagnostic_code == "DATATYPE.CAST_FORBIDDEN",
          "MDF-014 precision-loss diagnostic mismatch");

  const auto real128_source = numeric::EncodeReal128LittleEndian("1.25");
  Require(real128_source.bytes.has_value(),
          "MDF-014 real128 source fixture encoding failed");
  cast.value = Value(
      dt::CanonicalTypeId::real128,
      std::string(real128_source.bytes->begin(), real128_source.bytes->end()));
  cast.value.descriptor = Descriptor(dt::CanonicalTypeId::real128);
  cast.target_type_id = dt::CanonicalTypeId::real64;
  cast.target_descriptor = Descriptor(dt::CanonicalTypeId::real64);
  cast.explicit_cast = false;
  const auto silent_downgrade = dt::CastDatatypeValue(cast);
  Require(!silent_downgrade.ok(),
          "MDF-014 accepted silent real128 downgrade");
  Require(silent_downgrade.diagnostic.diagnostic_code ==
              "DATATYPE.CAST_FORBIDDEN",
          "MDF-014 silent downgrade diagnostic mismatch");
  Require(DiagnosticDetail(silent_downgrade.diagnostic) ==
              "real64_present_cast_policy_unresolved",
          "MDF-014 silent downgrade refusal detail mismatch");
  Require(silent_downgrade.value.type_id == dt::CanonicalTypeId::unknown &&
              silent_downgrade.value.encoded_value.empty(),
          "MDF-014 silent downgrade returned a value");
}

void TestNonScalarOperatorCastProof() {
  dt::DatatypeCastRequest cast;
  cast.value = Value(dt::CanonicalTypeId::character, "{\"a\":[1,true]}");
  cast.target_type_id = dt::CanonicalTypeId::json_document;
  cast.explicit_cast = true;
  const auto json_cast = dt::CastDatatypeValue(cast);
  Require(json_cast.ok() &&
              json_cast.value.type_id == dt::CanonicalTypeId::json_document &&
              json_cast.value.encoded_value == "{\"a\":[1,true]}",
          "MDF-014 JSON document cast failed");

  cast.value = Value(dt::CanonicalTypeId::character, "{\"a\":}");
  const auto bad_json_cast = dt::CastDatatypeValue(cast);
  Require(!bad_json_cast.ok() &&
              bad_json_cast.diagnostic.diagnostic_code ==
                  "DATATYPE.CAST_FORBIDDEN",
          "MDF-014 invalid JSON document cast was accepted");

  cast.value = Value(dt::CanonicalTypeId::character, "<root/>");
  cast.target_type_id = dt::CanonicalTypeId::xml_document;
  const auto xml_cast = dt::CastDatatypeValue(cast);
  Require(xml_cast.ok() &&
              xml_cast.value.type_id == dt::CanonicalTypeId::xml_document,
          "MDF-014 XML document cast failed");

  cast.value = Value(dt::CanonicalTypeId::character, "<root>");
  const auto bad_xml_cast = dt::CastDatatypeValue(cast);
  Require(!bad_xml_cast.ok(),
          "MDF-014 invalid XML document cast was accepted");

  cast.value = Value(dt::CanonicalTypeId::character, "SBHSTORE1;items=61:31");
  cast.target_type_id = dt::CanonicalTypeId::hstore_document;
  const auto hstore_cast = dt::CastDatatypeValue(cast);
  Require(!hstore_cast.ok() &&
              hstore_cast.diagnostic.diagnostic_code ==
                  "DATATYPE.CAST_FORBIDDEN",
          "MDF-014 hstore cast bypassed required domain/profile authority");

  dt::DocumentCanonicalizationRequest document;
  document.type_id = dt::CanonicalTypeId::document;
  document.encoded_value = " { \"a\" : [ 1, true ] } ";
  const auto canonical_json = dt::CanonicalizeDocumentValue(document);
  Require(canonical_json.ok() &&
              canonical_json.canonical_type_id ==
                  dt::CanonicalTypeId::json_document &&
              canonical_json.canonical_format == "json_text" &&
              canonical_json.canonical_value == "{\"a\":[1,true]}",
          "MDF-014 generic document JSON canonicalization failed");

  document.encoded_value = "<root/>";
  const auto canonical_xml = dt::CanonicalizeDocumentValue(document);
  Require(canonical_xml.ok() &&
              canonical_xml.canonical_type_id ==
                  dt::CanonicalTypeId::xml_document &&
              canonical_xml.canonical_format == "xml_text",
          "MDF-014 generic document XML canonicalization failed");

  document.type_id = dt::CanonicalTypeId::json_document;
  document.encoded_value = "{\"a\":1,\"a\":2}";
  const auto duplicate_key = dt::CanonicalizeDocumentValue(document);
  Require(!duplicate_key.ok() &&
              duplicate_key.diagnostic.diagnostic_code ==
                  "SB_DATATYPE_DOCUMENT_CANONICALIZATION_REJECTED",
          "MDF-014 JSON duplicate-key canonicalization was accepted");

  document.type_id = dt::CanonicalTypeId::binary_json_document;
  document.encoded_value = "SBBJSON1;payload=0102";
  const auto binary_json = dt::CanonicalizeDocumentValue(document);
  Require(binary_json.ok() &&
              binary_json.canonical_format == "binary_json_envelope" &&
              binary_json.canonical_value == "SBBJSON1;payload=0102",
          "MDF-014 binary JSON envelope canonicalization failed");

  document.type_id = dt::CanonicalTypeId::hstore_document;
  document.encoded_value = "SBHSTORE1;items=61:31";
  document.allow_hstore_domain = false;
  const auto hstore_without_domain = dt::CanonicalizeDocumentValue(document);
  Require(!hstore_without_domain.ok(),
          "MDF-014 hstore canonicalization ignored domain/profile authority");
  document.allow_hstore_domain = true;
  const auto hstore_with_domain = dt::CanonicalizeDocumentValue(document);
  Require(hstore_with_domain.ok() &&
              hstore_with_domain.canonical_format == "hstore_domain_envelope",
          "MDF-014 hstore domain canonicalization failed");

  dt::DatatypeSetDescriptor set_descriptor;
  set_descriptor.element_type_id = dt::CanonicalTypeId::int32;
  set_descriptor.ordered = false;
  set_descriptor.allow_duplicates = false;
  const auto encoded_left = dt::EncodeSetValue(
      set_descriptor,
      {Int32Value(2), Int32Value(1), Int32Value(2)});
  Require(encoded_left.ok(), "MDF-014 set encoding failed");
  const auto encoded_right = dt::EncodeSetValue(
      set_descriptor,
      {Int32Value(1), Int32Value(2)});
  Require(encoded_right.ok(), "MDF-014 set equality fixture encoding failed");

  dt::DatatypeSetOperationRequest set_operation;
  set_operation.descriptor = set_descriptor;
  set_operation.left_encoded_set = encoded_left.encoded_set;
  set_operation.operation = dt::DatatypeSetOperationKind::cardinality;
  auto set_result = dt::ApplySetOperation(set_operation);
  Require(set_result.ok() && IsCanonicalUint64(set_result.value, 2),
          "MDF-014 set cardinality drifted");

  set_operation.operation = dt::DatatypeSetOperationKind::membership;
  set_operation.right_value = Int32Value(1);
  set_result = dt::ApplySetOperation(set_operation);
  Require(set_result.ok() && IsCanonicalBoolean(set_result.value, true),
          "MDF-014 set membership failed");
  set_operation.right_value = Int32Value(3);
  set_result = dt::ApplySetOperation(set_operation);
  Require(set_result.ok() && IsCanonicalBoolean(set_result.value, false),
          "MDF-014 set non-membership failed");

  set_operation.operation = dt::DatatypeSetOperationKind::equals;
  set_operation.right_encoded_set = encoded_right.encoded_set;
  set_result = dt::ApplySetOperation(set_operation);
  Require(set_result.ok() && IsCanonicalBoolean(set_result.value, true),
          "MDF-014 set equality failed");
  set_operation.operation = dt::DatatypeSetOperationKind::subset;
  set_result = dt::ApplySetOperation(set_operation);
  Require(set_result.ok() && IsCanonicalBoolean(set_result.value, true),
          "MDF-014 set subset failed");
  set_operation.operation = dt::DatatypeSetOperationKind::superset;
  set_result = dt::ApplySetOperation(set_operation);
  Require(set_result.ok() && IsCanonicalBoolean(set_result.value, true),
          "MDF-014 set superset failed");

  set_descriptor.element_type_id = dt::CanonicalTypeId::opaque_extension;
  set_operation.descriptor = set_descriptor;
  set_operation.left_encoded_set = encoded_left.encoded_set;
  set_operation.operation = dt::DatatypeSetOperationKind::cardinality;
  const auto opaque_set = dt::ApplySetOperation(set_operation);
  Require(!opaque_set.ok() &&
              opaque_set.diagnostic.diagnostic_code ==
                  "SB_DATATYPE_SET_OPERATION_REJECTED",
          "MDF-014 opaque set operation was accepted");

  const auto json_compare = dt::CompareDatatypeValues(
      {Value(dt::CanonicalTypeId::json_document, "{\"a\":1}"),
       Value(dt::CanonicalTypeId::json_document, "{\"b\":1}")});
  Require(json_compare.ok() && json_compare.comparison < 0,
          "MDF-014 non-scalar document comparison drifted");

  const auto array_key_left = dt::MakeDatatypeSortKey(
      {Value(dt::CanonicalTypeId::array, "[1]")});
  const auto array_key_right = dt::MakeDatatypeSortKey(
      {Value(dt::CanonicalTypeId::array, "[2]")});
  Require(array_key_left.ok() && array_key_right.ok() &&
              array_key_left.sort_key < array_key_right.sort_key,
          "MDF-014 non-scalar array sort-key order drifted");
}

void TestStableHashAndDeserializationRefusals() {
  dt::DatatypeHashRequest hash_request;
  const std::string bytes(
      "\x01\x8f\x8a\x2a\x1b\x2c\x7d\xef\x81\x23\x45\x67\x89\xab\xcd\xef",
      16);
  hash_request.value = UuidValue(bytes);
  const auto first = dt::HashDatatypeValue(hash_request);
  const auto second = dt::HashDatatypeValue(hash_request);
  Require(!first.ok() && !second.ok() &&
              first.diagnostic.diagnostic_code ==
                  "SB_DATATYPE_HASH_REJECTED" &&
              second.diagnostic.diagnostic_code ==
                  "SB_DATATYPE_HASH_REJECTED" &&
              DiagnosticDetail(first.diagnostic) ==
                  "uuid_hash_policy_unresolved" &&
              DiagnosticDetail(second.diagnostic) ==
                  "uuid_hash_policy_unresolved" &&
              first.stable_hash_hex.empty() &&
              second.stable_hash_hex.empty(),
          "MDF-014 UUID hash did not fail closed at unresolved policy");

  const auto serialized = dt::SerializeDatatypeValue({hash_request.value});
  Require(!serialized.ok() &&
              serialized.diagnostic.diagnostic_code ==
                  "SB_DATATYPE_SERIALIZATION_REJECTED" &&
              DiagnosticDetail(serialized.diagnostic) ==
                  "uuid_serialization_policy_unresolved" &&
              serialized.serialized_value.empty(),
          "MDF-014 UUID serialization did not fail closed at unresolved policy");
  const std::string private_frame = std::string("SBDVUUID\1\20", 10) + bytes;
  const auto wrong_type =
      dt::DeserializeDatatypeValue({dt::CanonicalTypeId::int64,
                                    private_frame});
  Require(!wrong_type.ok(), "MDF-014 accepted mismatched deserialization type");
  Require(wrong_type.diagnostic.diagnostic_code ==
              "DATATYPE.DESCRIPTOR.INVALID",
          "MDF-014 mismatched deserialization diagnostic mismatch");
}

void TestExplicitDisplayBoundaryRendering() {
  dt::DatatypeOperationValue null_value;
  null_value.type_id = dt::CanonicalTypeId::int64;
  null_value.is_null = true;
  null_value.descriptor = Descriptor(dt::CanonicalTypeId::int64);
  const auto rendered_null = dt::RenderDatatypeValueForDisplay({null_value});
  Require(rendered_null.ok() && rendered_null.explicit_display_boundary &&
              rendered_null.display_value == "NULL",
          "MDF-014 null display boundary drifted");

  auto binary_display_value =
      Value(dt::CanonicalTypeId::binary, std::string("A\0B", 3));
  binary_display_value.descriptor = Descriptor(dt::CanonicalTypeId::binary);
  const auto rendered_binary =
      dt::RenderDatatypeValueForDisplay({binary_display_value});
  Require(rendered_binary.ok() &&
              rendered_binary.canonical_type_name == "binary" &&
              rendered_binary.display_value == "0x410042",
          "MDF-014 binary display boundary did not render hex");

  auto rendered_character = dt::RenderDatatypeValueForDisplay(
      {Value(dt::CanonicalTypeId::character, "O'Brien"), true});
  Require(rendered_character.ok() &&
              rendered_character.display_value == "'O''Brien'",
          "MDF-014 character export literal escaping drifted");

  const auto rendered_opaque = dt::RenderDatatypeValueForDisplay(
      {Value(dt::CanonicalTypeId::opaque_extension, "secret")});
  Require(rendered_opaque.ok() && rendered_opaque.payload_redacted &&
              rendered_opaque.display_value.find("secret") == std::string::npos,
          "MDF-014 opaque display boundary leaked payload");

  const auto rendered_json = dt::RenderDatatypeValueForDisplay(
      {Value(dt::CanonicalTypeId::json_document, "{\"k\":1}")});
  Require(rendered_json.ok() && rendered_json.explicit_display_boundary &&
              !rendered_json.payload_redacted &&
              rendered_json.display_value == "{\"k\":1}",
          "MDF-014 JSON display boundary drifted");

  const auto rendered_array = dt::RenderDatatypeValueForDisplay(
      {Value(dt::CanonicalTypeId::array, "[1,2,3]")});
  Require(rendered_array.ok() && !rendered_array.payload_redacted &&
              rendered_array.display_value == "[1,2,3]",
          "MDF-014 array display boundary drifted");

  const auto rendered_spatial = dt::RenderDatatypeValueForDisplay(
      {Value(dt::CanonicalTypeId::geometry, "POINT(1 2)")});
  Require(rendered_spatial.ok() && !rendered_spatial.payload_redacted &&
              rendered_spatial.display_value == "POINT(1 2)",
          "MDF-014 spatial display boundary drifted");

  const auto rendered_vector = dt::RenderDatatypeValueForDisplay(
      {Value(dt::CanonicalTypeId::dense_vector, "[0.1,0.2]")});
  Require(rendered_vector.ok() && !rendered_vector.payload_redacted &&
              rendered_vector.display_value == "[0.1,0.2]",
          "MDF-014 vector display boundary drifted");

  const auto rendered_binary_json = dt::RenderDatatypeValueForDisplay(
      {Value(dt::CanonicalTypeId::binary_json_document, "binary-json-payload")});
  Require(rendered_binary_json.ok() && rendered_binary_json.payload_redacted &&
              rendered_binary_json.display_value == "<binary_json_document:19 bytes>" &&
              rendered_binary_json.display_value.find("binary-json-payload") ==
                  std::string::npos,
          "MDF-014 binary document display boundary leaked payload");

  const auto rendered_sketch = dt::RenderDatatypeValueForDisplay(
      {Value(dt::CanonicalTypeId::bloom_filter, "sketch-payload")});
  Require(rendered_sketch.ok() && rendered_sketch.payload_redacted &&
              rendered_sketch.display_value == "<bloom_filter:14 bytes>" &&
              rendered_sketch.display_value.find("sketch-payload") == std::string::npos,
          "MDF-014 sketch display boundary leaked payload");

  const auto rendered_locator = dt::RenderDatatypeValueForDisplay(
      {Value(dt::CanonicalTypeId::lob_locator, "locator-payload")});
  Require(rendered_locator.ok() && rendered_locator.payload_redacted &&
              rendered_locator.display_value == "<lob_locator:15 bytes>" &&
              rendered_locator.display_value.find("locator-payload") == std::string::npos,
          "MDF-014 locator display boundary leaked payload");

  const auto rendered_result_set = dt::RenderDatatypeValueForDisplay(
      {Value(dt::CanonicalTypeId::result_set, "result-set-descriptor")});
  Require(rendered_result_set.ok() && rendered_result_set.payload_redacted &&
              rendered_result_set.display_value == "<result_set:21 bytes>" &&
              rendered_result_set.display_value.find("result-set-descriptor") ==
                  std::string::npos,
          "MDF-014 result-set display boundary leaked payload");

  const auto unknown = dt::RenderDatatypeValueForDisplay(
      {Value(dt::CanonicalTypeId::unknown, "payload")});
  Require(!unknown.ok() &&
              unknown.diagnostic.diagnostic_code ==
                  "DATATYPE.DESCRIPTOR.INVALID",
          "MDF-014 unknown display boundary did not fail closed");
}

void TestBitStringRequiresSpecializedV3Carrier() {
  dt::DatatypeOperationValue raw{
      dt::CanonicalTypeId::bit_string,
      std::string(1, static_cast<char>(0x80)), false};
  dt::DatatypeCastRequest cast;
  cast.value = raw;
  cast.target_type_id = dt::CanonicalTypeId::character;
  cast.context = dt::DatatypeCastContext::explicit_cast;
  const auto cast_result = dt::CastDatatypeValue(cast);
  Require(!cast_result.ok() &&
              cast_result.diagnostic.diagnostic_code ==
                  "CTB.BIT.DESCRIPTOR_INVALID" &&
              dt::ClassifyDatatypeCast(dt::CanonicalTypeId::bit_string,
                                       dt::CanonicalTypeId::bit_string) ==
                  dt::DatatypeCastCategory::forbidden,
          "raw bit-string cast/catch-all bypassed V3 profile authority");
  Require(!dt::CompareDatatypeValues({raw, raw}).ok() &&
              !dt::MakeDatatypeSortKey({raw}).ok() &&
              !dt::HashDatatypeValue({raw}).ok() &&
              !dt::RenderDatatypeValueForDisplay({raw}).ok(),
          "raw bit-string generic operation bypassed specialized carrier");
  const auto serialized = dt::SerializeDatatypeValue({raw});
  Require(!serialized.ok() &&
              serialized.diagnostic.diagnostic_code ==
                  "CTB.BIT.SERIALIZATION_PROFILE_MISSING",
          "raw bit-string SBDV1 serialization was admitted");
  auto dirty_null = raw;
  dirty_null.is_null = true;
  dirty_null.descriptor = Descriptor(dt::CanonicalTypeId::bit_string);
  Require(dt::CastDatatypeValue(
              {dirty_null, dt::CanonicalTypeId::character,
               dt::DatatypeCastContext::explicit_cast})
                  .diagnostic.diagnostic_code ==
              "DATATYPE.NULL_STATE.INVALID" &&
              dt::CompareDatatypeValues({dirty_null, raw})
                      .diagnostic.diagnostic_code ==
                  "DATATYPE.NULL_STATE.INVALID" &&
              dt::MakeDatatypeSortKey({dirty_null})
                      .diagnostic.diagnostic_code ==
                  "DATATYPE.NULL_STATE.INVALID" &&
              dt::HashDatatypeValue({dirty_null})
                      .diagnostic.diagnostic_code ==
                  "DATATYPE.NULL_STATE.INVALID" &&
              dt::SerializeDatatypeValue({dirty_null})
                      .diagnostic.diagnostic_code ==
                  "DATATYPE.NULL_STATE.INVALID" &&
              dt::RenderDatatypeValueForDisplay({dirty_null})
                      .diagnostic.diagnostic_code ==
                  "DATATYPE.NULL_STATE.INVALID",
          "valid raw bit descriptor did not preserve dirty-NULL precedence");
  ++dirty_null.descriptor.descriptor_epoch;
  Require(dt::CastDatatypeValue(
              {dirty_null, dt::CanonicalTypeId::character,
               dt::DatatypeCastContext::explicit_cast})
                  .diagnostic.diagnostic_code ==
              "CTB.BIT.DESCRIPTOR_INVALID" &&
              dt::CompareDatatypeValues({dirty_null, raw})
                      .diagnostic.diagnostic_code ==
                  "CTB.BIT.DESCRIPTOR_INVALID" &&
              dt::MakeDatatypeSortKey({dirty_null})
                      .diagnostic.diagnostic_code ==
                  "CTB.BIT.DESCRIPTOR_INVALID" &&
              dt::HashDatatypeValue({dirty_null})
                      .diagnostic.diagnostic_code ==
                  "CTB.BIT.DESCRIPTOR_INVALID" &&
              dt::SerializeDatatypeValue({dirty_null})
                      .diagnostic.diagnostic_code ==
                  "CTB.BIT.DESCRIPTOR_INVALID" &&
              dt::RenderDatatypeValueForDisplay({dirty_null})
                      .diagnostic.diagnostic_code ==
                  "CTB.BIT.DESCRIPTOR_INVALID",
          "invalid raw bit descriptor did not precede dirty-NULL state");
}

void TestDateRequiresSpecializedD710Carrier() {
  dt::DatatypeOperationValue raw{
      dt::CanonicalTypeId::date, std::string("\0\0\0\0", 4), false};
  dt::DatatypeCastRequest cast;
  cast.value = raw;
  cast.target_type_id = dt::CanonicalTypeId::character;
  cast.context = dt::DatatypeCastContext::explicit_cast;
  const auto cast_result = dt::CastDatatypeValue(cast);
  Require(!cast_result.ok() &&
              cast_result.diagnostic.diagnostic_code ==
                  "CTI.TEMPORAL.DESCRIPTOR_INVALID" &&
              dt::ClassifyDatatypeCast(dt::CanonicalTypeId::date,
                                       dt::CanonicalTypeId::date) ==
                  dt::DatatypeCastCategory::forbidden,
          "raw date cast/catch-all bypassed d710 profile authority");
  Require(!dt::CompareDatatypeValues({raw, raw}).ok() &&
              !dt::MakeDatatypeSortKey({raw}).ok() &&
              !dt::HashDatatypeValue({raw}).ok() &&
              !dt::RenderDatatypeValueForDisplay({raw}).ok() &&
              !dt::ExtractDatatypeField({raw, "year"}).ok(),
          "raw date generic operation bypassed specialized carrier");
  const auto serialized = dt::SerializeDatatypeValue({raw});
  Require(!serialized.ok() &&
              serialized.diagnostic.diagnostic_code ==
                  "CTI.TEMPORAL.SERIALIZATION_PROFILE_MISSING",
          "raw date SBDV1 serialization was admitted");
  dt::DatatypeDeserializationRequest decode;
  decode.expected_type_id = dt::CanonicalTypeId::date;
  decode.serialized_value = "SBDV1;type=date;state=value;payload=00000000";
  const auto deserialized = dt::DeserializeDatatypeValue(decode);
  Require(!deserialized.ok() &&
              deserialized.diagnostic.diagnostic_code ==
                  "CTI.TEMPORAL.SERIALIZATION_PROFILE_MISSING",
          "raw date SBDV1 deserialization was admitted");

  auto dirty_null = raw;
  dirty_null.is_null = true;
  dirty_null.descriptor = Descriptor(dt::CanonicalTypeId::date);
  Require(dt::CastDatatypeValue(
              {dirty_null, dt::CanonicalTypeId::character,
               dt::DatatypeCastContext::explicit_cast})
                  .diagnostic.diagnostic_code ==
              "DATATYPE.NULL_STATE.INVALID" &&
              dt::CompareDatatypeValues({dirty_null, raw})
                      .diagnostic.diagnostic_code ==
                  "DATATYPE.NULL_STATE.INVALID" &&
              dt::MakeDatatypeSortKey({dirty_null})
                      .diagnostic.diagnostic_code ==
                  "DATATYPE.NULL_STATE.INVALID" &&
              dt::HashDatatypeValue({dirty_null})
                      .diagnostic.diagnostic_code ==
                  "DATATYPE.NULL_STATE.INVALID" &&
              dt::SerializeDatatypeValue({dirty_null})
                      .diagnostic.diagnostic_code ==
                  "DATATYPE.NULL_STATE.INVALID" &&
              dt::RenderDatatypeValueForDisplay({dirty_null})
                      .diagnostic.diagnostic_code ==
                  "DATATYPE.NULL_STATE.INVALID",
          "raw date dirty-NULL precedence drifted");
  ++dirty_null.descriptor.descriptor_epoch;
  Require(dt::CastDatatypeValue(
              {dirty_null, dt::CanonicalTypeId::character,
               dt::DatatypeCastContext::explicit_cast})
                  .diagnostic.diagnostic_code ==
              "CTI.TEMPORAL.DESCRIPTOR_INVALID" &&
              dt::CompareDatatypeValues({dirty_null, raw})
                      .diagnostic.diagnostic_code ==
                  "CTI.TEMPORAL.DESCRIPTOR_INVALID",
          "invalid raw date descriptor did not precede dirty-NULL state");
}

void TestTimeRequiresSpecializedD710Carrier() {
  dt::DatatypeOperationValue raw{
      dt::CanonicalTypeId::time, std::string(8, '\0'), false};
  dt::DatatypeCastRequest cast;
  cast.value = raw;
  cast.target_type_id = dt::CanonicalTypeId::character;
  cast.context = dt::DatatypeCastContext::explicit_cast;
  const auto cast_result = dt::CastDatatypeValue(cast);
  Require(!cast_result.ok() &&
              cast_result.diagnostic.diagnostic_code ==
                  "CTI.TEMPORAL.DESCRIPTOR_INVALID" &&
              dt::ClassifyDatatypeCast(dt::CanonicalTypeId::time,
                                       dt::CanonicalTypeId::time) ==
                  dt::DatatypeCastCategory::forbidden,
          "raw time cast/catch-all bypassed d710 profile authority");
  Require(!dt::CompareDatatypeValues({raw, raw}).ok() &&
              !dt::MakeDatatypeSortKey({raw}).ok() &&
              !dt::HashDatatypeValue({raw}).ok() &&
              !dt::RenderDatatypeValueForDisplay({raw}).ok() &&
              !dt::ExtractDatatypeField({raw, "hour"}).ok(),
          "raw time generic operation bypassed specialized carrier");
  const auto serialized = dt::SerializeDatatypeValue({raw});
  Require(!serialized.ok() &&
              serialized.diagnostic.diagnostic_code ==
                  "CTI.TEMPORAL.SERIALIZATION_PROFILE_MISSING",
          "raw time SBDV1 serialization was admitted");
  dt::DatatypeDeserializationRequest decode;
  decode.expected_type_id = dt::CanonicalTypeId::time;
  decode.serialized_value = "SBDV1;type=time;state=value;payload=0000000000000000";
  const auto deserialized = dt::DeserializeDatatypeValue(decode);
  Require(!deserialized.ok() &&
              deserialized.diagnostic.diagnostic_code ==
                  "CTI.TEMPORAL.SERIALIZATION_PROFILE_MISSING",
          "raw time SBDV1 deserialization was admitted");

  auto dirty_null = raw;
  dirty_null.is_null = true;
  dirty_null.descriptor = Descriptor(dt::CanonicalTypeId::time);
  Require(dt::CastDatatypeValue(
              {dirty_null, dt::CanonicalTypeId::character,
               dt::DatatypeCastContext::explicit_cast})
                  .diagnostic.diagnostic_code ==
              "DATATYPE.NULL_STATE.INVALID" &&
              dt::CompareDatatypeValues({dirty_null, raw})
                      .diagnostic.diagnostic_code ==
                  "DATATYPE.NULL_STATE.INVALID" &&
              dt::MakeDatatypeSortKey({dirty_null})
                      .diagnostic.diagnostic_code ==
                  "DATATYPE.NULL_STATE.INVALID" &&
              dt::HashDatatypeValue({dirty_null})
                      .diagnostic.diagnostic_code ==
                  "DATATYPE.NULL_STATE.INVALID" &&
              dt::SerializeDatatypeValue({dirty_null})
                      .diagnostic.diagnostic_code ==
                  "DATATYPE.NULL_STATE.INVALID" &&
              dt::RenderDatatypeValueForDisplay({dirty_null})
                      .diagnostic.diagnostic_code ==
                  "DATATYPE.NULL_STATE.INVALID",
          "raw time dirty-NULL precedence drifted");
  ++dirty_null.descriptor.descriptor_epoch;
  Require(dt::CastDatatypeValue(
              {dirty_null, dt::CanonicalTypeId::character,
               dt::DatatypeCastContext::explicit_cast})
                  .diagnostic.diagnostic_code ==
              "CTI.TEMPORAL.DESCRIPTOR_INVALID" &&
              dt::CompareDatatypeValues({dirty_null, raw})
                      .diagnostic.diagnostic_code ==
                  "CTI.TEMPORAL.DESCRIPTOR_INVALID",
          "invalid raw time descriptor did not precede dirty-NULL state");
}

void TestTimestampRequiresSpecializedD710Carrier() {
  dt::DatatypeOperationValue raw{
      dt::CanonicalTypeId::timestamp, std::string(16, '\0'), false};
  dt::DatatypeCastRequest cast;
  cast.value = raw;
  cast.target_type_id = dt::CanonicalTypeId::character;
  cast.context = dt::DatatypeCastContext::explicit_cast;
  const auto cast_result = dt::CastDatatypeValue(cast);
  Require(!cast_result.ok() &&
              cast_result.diagnostic.diagnostic_code ==
                  "CTI.TEMPORAL.DESCRIPTOR_INVALID" &&
              dt::ClassifyDatatypeCast(dt::CanonicalTypeId::timestamp,
                                       dt::CanonicalTypeId::timestamp) ==
                  dt::DatatypeCastCategory::forbidden,
          "raw timestamp cast/catch-all bypassed d710 profile authority");
  Require(!dt::CompareDatatypeValues({raw, raw}).ok() &&
              !dt::MakeDatatypeSortKey({raw}).ok() &&
              !dt::HashDatatypeValue({raw}).ok() &&
              !dt::RenderDatatypeValueForDisplay({raw}).ok() &&
              !dt::ExtractDatatypeField({raw, "year"}).ok(),
          "raw timestamp generic operation bypassed specialized carrier");
  const auto serialized = dt::SerializeDatatypeValue({raw});
  Require(!serialized.ok() &&
              serialized.diagnostic.diagnostic_code ==
                  "CTI.TEMPORAL.SERIALIZATION_PROFILE_MISSING" &&
              serialized.serialized_value.empty(),
          "raw timestamp SBDV1 serialization was admitted");
  dt::DatatypeDeserializationRequest decode;
  decode.expected_type_id = dt::CanonicalTypeId::timestamp;
  decode.serialized_value =
      "SBDV1;type=timestamp;state=value;payload="
      "00000000000000000000000000000000";
  const auto deserialized = dt::DeserializeDatatypeValue(decode);
  Require(!deserialized.ok() &&
              deserialized.diagnostic.diagnostic_code ==
                  "CTI.TEMPORAL.SERIALIZATION_PROFILE_MISSING" &&
              deserialized.value.type_id == dt::CanonicalTypeId::unknown &&
              deserialized.value.encoded_value.empty(),
          "raw timestamp SBDV1 deserialization was admitted");

  const auto timestamp_descriptor = Descriptor(dt::CanonicalTypeId::timestamp);
  for (auto raw_source : {
           Value(dt::CanonicalTypeId::character,
                 "2024-02-29T06:07:08.9"),
           Value(dt::CanonicalTypeId::binary, std::string(16, '\0'))}) {
    raw_source.descriptor = Descriptor(raw_source.type_id);
    dt::DatatypeCastRequest incoming;
    incoming.value = raw_source;
    incoming.target_type_id = dt::CanonicalTypeId::timestamp;
    incoming.target_descriptor = timestamp_descriptor;
    incoming.context = dt::DatatypeCastContext::explicit_cast;
    const auto refused = dt::CastDatatypeValue(incoming);
    Require(!refused.ok() &&
                refused.diagnostic.diagnostic_code ==
                    "CTI.TEMPORAL.DESCRIPTOR_INVALID" &&
                refused.value.type_id == dt::CanonicalTypeId::unknown &&
                refused.value.encoded_value.empty(),
            "raw text/bytes entered timestamp without the V3 cast carrier");
  }

  for (const std::string_view alias : {
           "time_tz", "timetz", "time with time zone", "timestamp_tz",
           "timestamptz", "timestamp with time zone"}) {
    Require(dt::CanonicalTypeIdFromStableName(std::string(alias)) ==
                dt::CanonicalTypeId::unknown,
            "zoned temporal alias collapsed into a local-civil base type");
    const auto decoded = dt::DeserializeDatatypeValue(
        {dt::CanonicalTypeId::unknown,
         std::string("SBDV1;type=") + std::string(alias) +
             ";state=value;payload=00"});
    Require(!decoded.ok() &&
                decoded.diagnostic.diagnostic_code ==
                    "DATATYPE.DESCRIPTOR.INVALID" &&
                decoded.value.type_id == dt::CanonicalTypeId::unknown &&
                decoded.value.encoded_value.empty(),
            "zoned temporal SBDV1 alias was admitted");
  }

  auto dirty_null = raw;
  dirty_null.is_null = true;
  dirty_null.descriptor = timestamp_descriptor;
  Require(dt::CastDatatypeValue(
              {dirty_null, dt::CanonicalTypeId::character,
               dt::DatatypeCastContext::explicit_cast})
                  .diagnostic.diagnostic_code ==
              "DATATYPE.NULL_STATE.INVALID" &&
              dt::CompareDatatypeValues({dirty_null, raw})
                      .diagnostic.diagnostic_code ==
                  "DATATYPE.NULL_STATE.INVALID" &&
              dt::MakeDatatypeSortKey({dirty_null})
                      .diagnostic.diagnostic_code ==
                  "DATATYPE.NULL_STATE.INVALID" &&
              dt::HashDatatypeValue({dirty_null})
                      .diagnostic.diagnostic_code ==
                  "DATATYPE.NULL_STATE.INVALID" &&
              dt::SerializeDatatypeValue({dirty_null})
                      .diagnostic.diagnostic_code ==
                  "DATATYPE.NULL_STATE.INVALID" &&
              dt::RenderDatatypeValueForDisplay({dirty_null})
                      .diagnostic.diagnostic_code ==
                  "DATATYPE.NULL_STATE.INVALID" &&
              dt::ExtractDatatypeField({dirty_null, "year"})
                      .diagnostic.diagnostic_code ==
                  "DATATYPE.NULL_STATE.INVALID",
          "valid raw timestamp descriptor did not preserve dirty-NULL precedence");
  ++dirty_null.descriptor.descriptor_epoch;
  Require(dt::CastDatatypeValue(
              {dirty_null, dt::CanonicalTypeId::character,
               dt::DatatypeCastContext::explicit_cast})
                  .diagnostic.diagnostic_code ==
              "CTI.TEMPORAL.DESCRIPTOR_INVALID" &&
              dt::CompareDatatypeValues({dirty_null, raw})
                      .diagnostic.diagnostic_code ==
                  "CTI.TEMPORAL.DESCRIPTOR_INVALID" &&
              dt::MakeDatatypeSortKey({dirty_null})
                      .diagnostic.diagnostic_code ==
                  "CTI.TEMPORAL.DESCRIPTOR_INVALID" &&
              dt::HashDatatypeValue({dirty_null})
                      .diagnostic.diagnostic_code ==
                  "CTI.TEMPORAL.DESCRIPTOR_INVALID" &&
              dt::SerializeDatatypeValue({dirty_null})
                      .diagnostic.diagnostic_code ==
                  "CTI.TEMPORAL.DESCRIPTOR_INVALID" &&
              dt::RenderDatatypeValueForDisplay({dirty_null})
                      .diagnostic.diagnostic_code ==
                  "CTI.TEMPORAL.DESCRIPTOR_INVALID" &&
              dt::ExtractDatatypeField({dirty_null, "year"})
                      .diagnostic.diagnostic_code ==
                  "CTI.TEMPORAL.DESCRIPTOR_INVALID",
          "invalid raw timestamp descriptor did not precede dirty-NULL state");
}

void TestIntervalRequiresSpecializedD710Carrier() {
  auto descriptor = Descriptor(dt::CanonicalTypeId::interval);
  dt::DatatypeOperationValue present{
      dt::CanonicalTypeId::interval, std::string(16, '\0'), false};
  present.descriptor = descriptor;

  dt::DatatypeCastRequest cast;
  cast.value = present;
  cast.target_type_id = dt::CanonicalTypeId::character;
  cast.context = dt::DatatypeCastContext::explicit_cast;
  const auto cast_result = dt::CastDatatypeValue(cast);
  Require(!cast_result.ok() &&
              cast_result.category == dt::DatatypeCastCategory::forbidden &&
              cast_result.diagnostic.diagnostic_code ==
                  "CTI.INTERVAL.DESCRIPTOR_INVALID" &&
              cast_result.value.type_id == dt::CanonicalTypeId::unknown &&
              cast_result.value.encoded_value.empty() &&
              dt::ClassifyDatatypeCast(dt::CanonicalTypeId::interval,
                                       dt::CanonicalTypeId::interval) ==
                  dt::DatatypeCastCategory::forbidden &&
              dt::ClassifyDatatypeCast(dt::CanonicalTypeId::character,
                                       dt::CanonicalTypeId::interval) ==
                  dt::DatatypeCastCategory::forbidden,
          "legacy carrier admitted an interval cast path");

  const auto comparison = dt::CompareDatatypeValues({present, present});
  const auto sort_key = dt::MakeDatatypeSortKey({present});
  const auto hash = dt::HashDatatypeValue({present});
  const auto extract = dt::ExtractDatatypeField({present, "months"});
  const auto display = dt::RenderDatatypeValueForDisplay({present});
  Require(!comparison.ok() &&
              comparison.diagnostic.diagnostic_code ==
                  "CTI.INTERVAL.DESCRIPTOR_INVALID" &&
              !sort_key.ok() && sort_key.sort_key.empty() &&
              sort_key.diagnostic.diagnostic_code ==
                  "CTI.TEMPORAL.ORDERING_REFUSED" &&
              !hash.ok() && hash.stable_hash_hex.empty() &&
              hash.diagnostic.diagnostic_code ==
                  "CTI.INTERVAL.DESCRIPTOR_INVALID" &&
              !extract.ok() &&
              extract.diagnostic.diagnostic_code ==
                  "CTI.INTERVAL.DESCRIPTOR_INVALID" &&
              !display.ok() && display.display_value.empty() &&
              display.diagnostic.diagnostic_code ==
                  "CTI.INTERVAL.DESCRIPTOR_INVALID",
          "legacy carrier admitted an interval operation path");

  const auto serialized = dt::SerializeDatatypeValue({present});
  Require(!serialized.ok() && serialized.serialized_value.empty() &&
              serialized.diagnostic.diagnostic_code ==
                  "CTI.TEMPORAL.SERIALIZATION_PROFILE_MISSING",
          "legacy SBDV1 serialized interval without a d710 profile");
  for (const auto& decode : {
           dt::DatatypeDeserializationRequest{
               dt::CanonicalTypeId::interval,
               "SBDV1;type=character;state=value;payload=78", descriptor},
           dt::DatatypeDeserializationRequest{
               dt::CanonicalTypeId::character,
               "SBDV1;type=interval;state=value;payload=00000000000000000000000000000000",
               Descriptor(dt::CanonicalTypeId::character)},
           dt::DatatypeDeserializationRequest{
               dt::CanonicalTypeId::character,
               "SBDV1;state=value;payload=not_hex;type=interval",
               Descriptor(dt::CanonicalTypeId::character)},
           dt::DatatypeDeserializationRequest{
               dt::CanonicalTypeId::character,
               "SBDV1;type=INTERVAL;state=value;payload=not_hex",
               Descriptor(dt::CanonicalTypeId::character)}}) {
    const auto result = dt::DeserializeDatatypeValue(decode);
    Require(!result.ok() &&
                result.diagnostic.diagnostic_code ==
                    "CTI.TEMPORAL.SERIALIZATION_PROFILE_MISSING" &&
                result.value.type_id == dt::CanonicalTypeId::unknown &&
                result.value.encoded_value.empty(),
            "legacy SBDV1 deserialized interval without a d710 profile");
  }

  auto clean_null = present;
  clean_null.is_null = true;
  clean_null.encoded_value.clear();
  Require(dt::HashDatatypeValue({clean_null}).diagnostic.diagnostic_code ==
              "CTI.INTERVAL.DESCRIPTOR_INVALID" &&
              dt::CompareDatatypeValues({clean_null, clean_null})
                      .diagnostic.diagnostic_code ==
                  "CTI.INTERVAL.DESCRIPTOR_INVALID",
          "clean interval NULL bypassed profile authentication");

  auto dirty_null = clean_null;
  dirty_null.encoded_value.assign(16, '\0');
  Require(dt::CastDatatypeValue(
              {dirty_null, dt::CanonicalTypeId::character,
               dt::DatatypeCastContext::explicit_cast})
                  .diagnostic.diagnostic_code ==
              "DATATYPE.NULL_STATE.INVALID" &&
              dt::HashDatatypeValue({dirty_null}).diagnostic.diagnostic_code ==
                  "DATATYPE.NULL_STATE.INVALID" &&
              dt::ExtractDatatypeField({dirty_null, "months"})
                      .diagnostic.diagnostic_code ==
                  "DATATYPE.NULL_STATE.INVALID" &&
              dt::SerializeDatatypeValue({dirty_null})
                      .diagnostic.diagnostic_code ==
                  "DATATYPE.NULL_STATE.INVALID" &&
              dt::MakeDatatypeSortKey({dirty_null})
                      .diagnostic.diagnostic_code ==
                  "CTI.TEMPORAL.ORDERING_REFUSED",
          "interval dirty-NULL diagnostic precedence drifted");

  auto disallowed_null = clean_null;
  disallowed_null.descriptor.nullable_allowed = false;
  Require(dt::HashDatatypeValue({disallowed_null})
                  .diagnostic.diagnostic_code ==
              "DATATYPE.NULL_NOT_ADMITTED",
          "interval disallowed-NULL diagnostic precedence drifted");

  auto invalid_descriptor = dirty_null;
  ++invalid_descriptor.descriptor.descriptor_epoch;
  Require(dt::HashDatatypeValue({invalid_descriptor})
                  .diagnostic.diagnostic_code ==
              "CTI.INTERVAL.DESCRIPTOR_INVALID" &&
              dt::CompareDatatypeValues({invalid_descriptor, present})
                      .diagnostic.diagnostic_code ==
                  "CTI.INTERVAL.DESCRIPTOR_INVALID",
          "interval descriptor mismatch did not precede dirty NULL");

  Require(dt::CompareDatatypeValues({dirty_null, invalid_descriptor})
                  .diagnostic.diagnostic_code ==
              "CTI.INTERVAL.DESCRIPTOR_INVALID" &&
              dt::CompareDatatypeValues({disallowed_null, dirty_null})
                      .diagnostic.diagnostic_code ==
                  "DATATYPE.NULL_STATE.INVALID",
          "interval comparison did not preserve request-level structural precedence");

  const auto unknown = static_cast<dt::CanonicalTypeId>(0xffffffffu);
  for (const auto& conversion : {
           dt::DescribeDatatypeConversion(dt::CanonicalTypeId::interval,
                                          unknown),
           dt::DescribeDatatypeConversion(unknown,
                                          dt::CanonicalTypeId::interval)}) {
    Require(!conversion.ok() &&
                conversion.kind == dt::ConversionDiagnosticKind::unsupported &&
                conversion.diagnostic.diagnostic_code ==
                    "CTI.TEMPORAL.SERIALIZATION_PROFILE_MISSING",
            "interval-incident conversion lost d710 diagnostic ownership");
  }
}

void TestBinaryUuidOperations() {
  const std::string bytes("\x01\x02\x03\x04\x05\x06\x70\x00\x80\x00\x09\x0a\x3b\x7c\x00\xff", 16);
  const auto value = UuidValue(bytes);
  const auto serialized = dt::SerializeDatatypeValue({value});
  const std::string expected = std::string("SBDVUUID\1\20", 10) + bytes;
  Require(!serialized.ok() &&
              serialized.diagnostic.diagnostic_code ==
                  "SB_DATATYPE_SERIALIZATION_REJECTED" &&
              DiagnosticDetail(serialized.diagnostic) ==
                  "uuid_serialization_policy_unresolved" &&
              serialized.serialized_value.empty(),
          "private UUID serializer did not fail closed");
  dt::DatatypeDeserializationRequest decode_present;
  decode_present.expected_type_id = dt::CanonicalTypeId::uuid;
  decode_present.serialized_value = expected;
  decode_present.expected_descriptor = value.descriptor;
  const auto decoded = dt::DeserializeDatatypeValue(decode_present);
  Require(!decoded.ok() &&
              decoded.diagnostic.diagnostic_code ==
                  "SB_DATATYPE_DESERIALIZATION_REJECTED" &&
              DiagnosticDetail(decoded.diagnostic) ==
                  "uuid_deserialization_policy_unresolved" &&
              decoded.value.type_id == dt::CanonicalTypeId::unknown &&
              decoded.value.encoded_value.empty(),
          "private UUID frame deserialization did not fail closed");
  for (std::size_t size = 0; size < expected.size(); ++size)
    Require(!dt::DeserializeDatatypeValue({dt::CanonicalTypeId::uuid, expected.substr(0, size)}).ok(),
            "truncated UUID frame admitted");
  for (const auto& invalid :
       {expected + "x", std::string("SBDVUUID\1\17", 10) + bytes}) {
    const auto malformed =
        dt::DeserializeDatatypeValue({dt::CanonicalTypeId::uuid, invalid});
    Require(!malformed.ok() &&
                malformed.diagnostic.diagnostic_code ==
                    "SB_DATATYPE_DESERIALIZATION_REJECTED" &&
                DiagnosticDetail(malformed.diagnostic) ==
                    "uuid_binary_frame_invalid",
            "malformed UUID frame did not precede descriptor/policy checks");
  }
  auto unsupported_state = decode_present;
  unsupported_state.serialized_value = std::string("SBDVUUID\2\20", 10) + bytes;
  const auto unsupported_state_result =
      dt::DeserializeDatatypeValue(unsupported_state);
  Require(!unsupported_state_result.ok() &&
              unsupported_state_result.diagnostic.diagnostic_code ==
                  "DTYPE.VALUE.STATE_UNHANDLED" &&
              DiagnosticDetail(unsupported_state_result.diagnostic) ==
                  "value_state_invalid",
          "UUID frame state validation did not precede policy refusal");
  dt::DatatypeDeserializationRequest generic_uuid;
  generic_uuid.expected_type_id = dt::CanonicalTypeId::uuid;
  generic_uuid.expected_descriptor = value.descriptor;
  generic_uuid.serialized_value =
      "SBDV1;type=uuid;state=value;payload="
      "01020304050670008000090a3b7c00ff";
  const auto generic_uuid_result =
      dt::DeserializeDatatypeValue(generic_uuid);
  Require(!generic_uuid_result.ok() &&
              generic_uuid_result.diagnostic.diagnostic_code ==
                  "SB_DATATYPE_DESERIALIZATION_REJECTED" &&
              DiagnosticDetail(generic_uuid_result.diagnostic) ==
                  "uuid_deserialization_policy_unresolved",
          "generic UUID frame bypassed unresolved codec policy");
  Require(!dt::DeserializeDatatypeValue({dt::CanonicalTypeId::int64, expected}).ok(),
          "UUID frame decoded as another type");
  const auto null_value = TypedNull(dt::CanonicalTypeId::uuid);
  const auto null_frame = dt::SerializeDatatypeValue({null_value});
  dt::DatatypeDeserializationRequest decode_null;
  decode_null.expected_type_id = dt::CanonicalTypeId::uuid;
  decode_null.serialized_value = std::string("SBDVUUID\0\0", 10);
  decode_null.expected_descriptor = null_value.descriptor;
  const auto decoded_null = dt::DeserializeDatatypeValue(decode_null);
  Require(!null_frame.ok() &&
              null_frame.diagnostic.diagnostic_code ==
                  "SB_DATATYPE_SERIALIZATION_REJECTED" &&
              DiagnosticDetail(null_frame.diagnostic) ==
                  "uuid_serialization_policy_unresolved" &&
              null_frame.serialized_value.empty() &&
              !decoded_null.ok() &&
              decoded_null.diagnostic.diagnostic_code ==
                  "SB_DATATYPE_DESERIALIZATION_REJECTED" &&
              DiagnosticDetail(decoded_null.diagnostic) ==
                  "uuid_deserialization_policy_unresolved" &&
              decoded_null.value.type_id == dt::CanonicalTypeId::unknown,
          "private UUID typed-NULL framing did not fail closed");
  const auto nil = UuidValue(std::string(16, '\0'));
  const auto nil_serialized = dt::SerializeDatatypeValue({nil});
  Require(!nil_serialized.ok() &&
              DiagnosticDetail(nil_serialized.diagnostic) ==
                  "uuid_serialization_policy_unresolved",
          "nil UUID bypassed unresolved serialization policy");
  const auto sorted = dt::MakeDatatypeSortKey({value});
  const auto compared = dt::CompareDatatypeValues({nil, value});
  const auto hashed = dt::HashDatatypeValue({value});
  Require(!sorted.ok() && sorted.sort_key.empty() &&
              DiagnosticDetail(sorted.diagnostic) ==
                  "uuid_sort_key_policy_unresolved" &&
              !compared.ok() &&
              DiagnosticDetail(compared.diagnostic) ==
                  "uuid_comparison_policy_unresolved" &&
              !hashed.ok() && hashed.stable_hash_hex.empty() &&
              DiagnosticDetail(hashed.diagnostic) ==
                  "uuid_hash_policy_unresolved",
          "UUID order/comparison/hash did not fail at unresolved policy");
  for (auto invalid : {UuidValue(bytes.substr(1)),
                       UuidValue("01020304-0506-7000-8000-090a3b7c00ff")}) {
    const auto invalid_serialized = dt::SerializeDatatypeValue({invalid});
    const auto invalid_hash = dt::HashDatatypeValue({invalid});
    const auto invalid_key = dt::MakeDatatypeSortKey({invalid});
    const auto invalid_compare =
        dt::CompareDatatypeValues({invalid, value});
    Require(!invalid_serialized.ok() &&
                DiagnosticDetail(invalid_serialized.diagnostic) ==
                    "uuid_serialization_value_invalid" &&
                !invalid_hash.ok() &&
                DiagnosticDetail(invalid_hash.diagnostic) ==
                    "uuid_hash_value_invalid" &&
                !invalid_key.ok() &&
                DiagnosticDetail(invalid_key.diagnostic) ==
                    "uuid_sort_key_value_invalid" &&
                !invalid_compare.ok() &&
                DiagnosticDetail(invalid_compare.diagnostic) ==
                    "uuid_comparison_value_invalid",
            "UUID operation did not validate binary16 before policy refusal");
  }
  auto dirty_null = TypedNull(dt::CanonicalTypeId::uuid);
  dirty_null.encoded_value = bytes;
  dt::DatatypeCastRequest invalid_null_cast;
  invalid_null_cast.value = dirty_null;
  invalid_null_cast.target_type_id = dt::CanonicalTypeId::uuid;
  invalid_null_cast.target_descriptor = Descriptor(dt::CanonicalTypeId::uuid);
  Require(!dt::SerializeDatatypeValue({dirty_null}).ok() &&
          !dt::HashDatatypeValue({dirty_null}).ok() &&
          !dt::MakeDatatypeSortKey({dirty_null}).ok() &&
          !dt::CompareDatatypeValues({dirty_null,value}).ok() &&
          !dt::CastDatatypeValue(invalid_null_cast).ok(), "UUID SQL NULL retained payload bytes");
  dt::DatatypeCastRequest cast;
  cast.value = UuidValue(bytes);
  cast.explicit_cast = true;
  cast.context = dt::DatatypeCastContext::explicit_cast;
  cast.target_type_id = dt::CanonicalTypeId::uuid;
  cast.target_descriptor = Descriptor(dt::CanonicalTypeId::uuid);
  const auto uuid_identity = dt::CastDatatypeValue(cast);
  Require(!uuid_identity.ok() &&
              uuid_identity.diagnostic.diagnostic_code ==
                  "DATATYPE.CAST_FORBIDDEN" &&
              DiagnosticDetail(uuid_identity.diagnostic) ==
                  "uuid_present_cast_policy_unresolved" &&
              uuid_identity.value.type_id == dt::CanonicalTypeId::unknown &&
              uuid_identity.value.encoded_value.empty(),
          "UUID identity did not retain its unresolved owner policy");

  cast.target_type_id = dt::CanonicalTypeId::binary;
  cast.target_descriptor = Descriptor(dt::CanonicalTypeId::binary);
  const auto uuid_to_binary = dt::CastDatatypeValue(cast);
  Require(uuid_to_binary.ok() &&
              uuid_to_binary.category ==
                  dt::DatatypeCastCategory::lossless_explicit &&
              uuid_to_binary.value.type_id == dt::CanonicalTypeId::binary &&
              !uuid_to_binary.value.is_null &&
              uuid_to_binary.value.encoded_value == bytes,
          "explicit UUID-to-binary cast did not preserve raw16 bytes");

  for (const auto context : {dt::DatatypeCastContext::implicit,
                             dt::DatatypeCastContext::assignment}) {
    cast.context = context;
    cast.explicit_cast = true;
    const auto refused_outgoing = dt::CastDatatypeValue(cast);
    auto refused_incoming_request = cast;
    refused_incoming_request.value =
        Value(dt::CanonicalTypeId::binary, bytes);
    refused_incoming_request.value.descriptor =
        Descriptor(dt::CanonicalTypeId::binary);
    refused_incoming_request.target_type_id = dt::CanonicalTypeId::uuid;
    refused_incoming_request.target_descriptor =
        Descriptor(dt::CanonicalTypeId::uuid);
    const auto refused_incoming =
        dt::CastDatatypeValue(refused_incoming_request);
    Require(!refused_outgoing.ok() && !refused_incoming.ok() &&
                DiagnosticDetail(refused_outgoing.diagnostic) ==
                    "explicit_cast_required" &&
                DiagnosticDetail(refused_incoming.diagnostic) ==
                    "explicit_cast_required" &&
                refused_outgoing.value.type_id ==
                    dt::CanonicalTypeId::unknown &&
                refused_incoming.value.type_id ==
                    dt::CanonicalTypeId::unknown &&
                refused_outgoing.value.encoded_value.empty() &&
                refused_incoming.value.encoded_value.empty(),
            "legacy explicit flag upgraded an implicit/assignment UUID-binary cast");
  }

  cast.value = Value(dt::CanonicalTypeId::binary, bytes);
  cast.value.descriptor = Descriptor(dt::CanonicalTypeId::binary);
  cast.target_type_id = dt::CanonicalTypeId::uuid;
  cast.target_descriptor = Descriptor(dt::CanonicalTypeId::uuid);
  cast.context = dt::DatatypeCastContext::explicit_cast;
  cast.explicit_cast = true;
  const auto binary_to_uuid = dt::CastDatatypeValue(cast);
  Require(binary_to_uuid.ok() &&
              binary_to_uuid.category ==
                  dt::DatatypeCastCategory::lossless_explicit &&
              binary_to_uuid.value.type_id == dt::CanonicalTypeId::uuid &&
              !binary_to_uuid.value.is_null &&
              binary_to_uuid.value.encoded_value == bytes,
          "explicit binary16-to-UUID cast did not preserve raw16 bytes");
  cast.value.encoded_value.pop_back();
  const auto truncated_uuid = dt::CastDatatypeValue(cast);
  Require(!truncated_uuid.ok() &&
              truncated_uuid.diagnostic.diagnostic_code ==
                  "DATATYPE.CAST_FORBIDDEN" &&
              DiagnosticDetail(truncated_uuid.diagnostic) ==
                  "binary_uuid_requires_exactly_16_octets" &&
              truncated_uuid.value.type_id == dt::CanonicalTypeId::unknown &&
              truncated_uuid.value.encoded_value.empty(),
          "truncated binary-to-UUID cast did not refuse atomically");
  cast.value = Value(dt::CanonicalTypeId::character, "01020304-0506-7000-8000-090a3b7c00ff");
  cast.value.descriptor = Descriptor(dt::CanonicalTypeId::character);
  const auto text_to_uuid = dt::CastDatatypeValue(cast);
  Require(!text_to_uuid.ok() &&
              text_to_uuid.diagnostic.diagnostic_code ==
                  "DATATYPE.CAST_FORBIDDEN" &&
              DiagnosticDetail(text_to_uuid.diagnostic) ==
                  "uuid_present_cast_policy_unresolved",
          "text-to-UUID cast did not fail at unresolved policy");
  cast.value = UuidValue(bytes);
  cast.target_type_id = dt::CanonicalTypeId::character;
  cast.target_descriptor = Descriptor(dt::CanonicalTypeId::character);
  const auto uuid_to_text = dt::CastDatatypeValue(cast);
  Require(!uuid_to_text.ok() &&
              uuid_to_text.diagnostic.diagnostic_code ==
                  "DATATYPE.CAST_FORBIDDEN" &&
              DiagnosticDetail(uuid_to_text.diagnostic) ==
                  "uuid_present_cast_policy_unresolved",
          "UUID-to-text cast did not fail at unresolved policy");
  dt::DatatypeExtractRequest extract;
  extract.value = cast.value;
  extract.field = "version";
  auto result = dt::ExtractDatatypeField(extract);
  Require(!result.ok() &&
              result.diagnostic.diagnostic_code ==
                  "SB_DATATYPE_EXTRACT_REJECTED" &&
              DiagnosticDetail(result.diagnostic) ==
                  "uuid_extract_policy_unresolved" &&
              result.value.type_id == dt::CanonicalTypeId::unknown &&
              result.value.encoded_value.empty(),
          "binary UUID version extraction did not fail closed");
  extract.field = "uuidv7_unix_millis";
  result = dt::ExtractDatatypeField(extract);
  const bool unresolved_uuidv7_profile = std::any_of(
      result.diagnostic.arguments.begin(), result.diagnostic.arguments.end(),
      [](const auto& argument) {
        return argument.key == "detail" && argument.text() &&
               *argument.text() == "uuid_extract_policy_unresolved";
      });
  Require(!result.ok() &&
              result.diagnostic.diagnostic_code ==
                  "SB_DATATYPE_EXTRACT_REJECTED" &&
              result.diagnostic.message_key == "datatype.extract.rejected" &&
              unresolved_uuidv7_profile,
          "binary UUID timestamp extraction did not fail closed at the "
          "unresolved UUID extraction policy");
}

void TestIntegerPhysicalSortKeyBytes() {
  const auto int8_value = [](unsigned raw) {
    return Value(dt::CanonicalTypeId::int8,
                 std::string(1, static_cast<char>(raw)));
  };
  const auto int8_minimum = dt::MakeDatatypeSortKey({int8_value(0x80)});
  const auto int8_zero = dt::MakeDatatypeSortKey({int8_value(0x00)});
  const auto int8_maximum = dt::MakeDatatypeSortKey({int8_value(0x7f)});
  Require(int8_minimum.ok() && int8_zero.ok() && int8_maximum.ok() &&
              int8_minimum.sort_key == std::string({char(1), char(0x00)}) &&
              int8_zero.sort_key == std::string({char(1), char(0x80)}) &&
              int8_maximum.sort_key == std::string({char(1), char(0xff)}),
          "int8 canonical-byte sort keys drifted");
  Require(dt::CompareDatatypeValues({int8_value(0x80), int8_value(0x00)}).comparison < 0 &&
              dt::CompareDatatypeValues({int8_value(0x00), int8_value(0x7f)}).comparison < 0,
          "int8 canonical-byte signed comparison drifted");
  Require(!dt::MakeDatatypeSortKey(
               {Value(dt::CanonicalTypeId::int8, {})}).ok() &&
              !dt::MakeDatatypeSortKey(
               {Value(dt::CanonicalTypeId::int8, std::string(2, '\0'))}).ok(),
          "int8 admitted a non-one-byte operation value");

  const auto uint8_value = [](unsigned raw) {
    return Value(dt::CanonicalTypeId::uint8,
                 std::string(1, static_cast<char>(raw)));
  };
  const auto uint8_minimum = dt::MakeDatatypeSortKey({uint8_value(0x00)});
  const auto uint8_maximum = dt::MakeDatatypeSortKey({uint8_value(0xff)});
  Require(uint8_minimum.ok() && uint8_maximum.ok() &&
              uint8_minimum.sort_key == std::string({char(1), char(0x00)}) &&
              uint8_maximum.sort_key == std::string({char(1), char(0xff)}) &&
              uint8_minimum.sort_key < uint8_maximum.sort_key,
          "uint8 canonical-byte sort keys drifted");
  Require(dt::CompareDatatypeValues(
              {uint8_value(0x00), uint8_value(0xff)}).comparison < 0,
          "uint8 canonical-byte unsigned comparison drifted");
  Require(!dt::MakeDatatypeSortKey(
               {Value(dt::CanonicalTypeId::uint8, {})}).ok() &&
              !dt::MakeDatatypeSortKey(
               {Value(dt::CanonicalTypeId::uint8, std::string(2, '\0'))}).ok(),
          "uint8 admitted a non-one-byte operation value");

  const auto uint32_value = [](std::uint64_t raw) {
    std::string encoded;
    Require(dt::EncodeCanonicalUint32Value(raw, &encoded),
            "uint32 canonical fixture encoding failed");
    return Value(dt::CanonicalTypeId::uint32, std::move(encoded));
  };
  const auto uint32_key = [&](std::uint64_t raw) {
    const auto encoded = dt::MakeDatatypeSortKey({uint32_value(raw)});
    Require(encoded.ok(), "uint32 canonical comparison key refused");
    return encoded.sort_key;
  };
  Require(uint32_key(0) == std::string({char(1), char(0), char(0), char(0), char(0)}) &&
              uint32_key(1) == std::string({char(1), char(0), char(0), char(0), char(1)}) &&
              uint32_key(2147483648ULL) ==
                  std::string({char(1), char(0x80), char(0), char(0), char(0)}) &&
              uint32_key(4294967295ULL) ==
                  std::string({char(1), char(0xff), char(0xff), char(0xff), char(0xff)}),
          "uint32 key is not state plus unsigned big-endian bytes");
  const std::array<std::uint64_t, 5> uint32_ordered{
      0, 1, 2147483647ULL, 2147483648ULL, 4294967295ULL};
  for (std::size_t left = 0; left < uint32_ordered.size(); ++left) {
    for (std::size_t right = 0; right < uint32_ordered.size(); ++right) {
      const int expected = left < right ? -1 : (left > right ? 1 : 0);
      const auto compared = dt::CompareDatatypeValues(
          {uint32_value(uint32_ordered[left]), uint32_value(uint32_ordered[right])});
      const auto left_key = uint32_key(uint32_ordered[left]);
      const auto right_key = uint32_key(uint32_ordered[right]);
      const int key_comparison =
          left_key < right_key ? -1 : (left_key > right_key ? 1 : 0);
      Require(compared.ok() && compared.comparison == expected &&
                  key_comparison == expected,
              "uint32 comparison and physical key disagree with unsigned order");
    }
  }
  for (const auto width : {0U, 1U, 2U, 3U, 5U}) {
    const auto malformed = Value(dt::CanonicalTypeId::uint32,
                                 std::string(width, '\0'));
    Require(!dt::CompareDatatypeValues({malformed, uint32_value(0)}).ok() &&
                !dt::CompareDatatypeValues({uint32_value(0), malformed}).ok() &&
                !dt::HashDatatypeValue({malformed}).ok() &&
                !dt::MakeDatatypeSortKey({malformed}).ok(),
            "uint32 operation admitted a malformed canonical width");
  }
  const auto uint32_null = TypedNull(dt::CanonicalTypeId::uint32);
  auto uint32_described_zero = uint32_value(0);
  uint32_described_zero.descriptor = uint32_null.descriptor;
  const auto uint32_null_first = dt::MakeDatatypeSortKey({uint32_null});
  dt::DatatypeSortKeyRequest uint32_null_last_request;
  uint32_null_last_request.value = uint32_null;
  uint32_null_last_request.null_ordering = dt::DatatypeNullOrdering::nulls_last;
  const auto uint32_null_last = dt::MakeDatatypeSortKey(uint32_null_last_request);
  Require(uint32_null_first.ok() && uint32_null_last.ok() &&
              uint32_null_first.sort_key == std::string(1, '\0') &&
              uint32_null_last.sort_key == std::string(1, '\2') &&
              !dt::CompareDatatypeValues(
                  {uint32_null, uint32_described_zero}).ok() &&
              !dt::HashDatatypeValue({uint32_value(0)}).ok() &&
              !dt::HashDatatypeValue({uint32_null}).ok() &&
              !dt::RenderDatatypeValueForDisplay({uint32_value(0)}).ok(),
          "uint32 unresolved NULL comparison, hash, or display policy did not fail closed");

  const auto int64_key = [&](std::int64_t raw) {
    const auto encoded = dt::MakeDatatypeSortKey({Int64Value(raw)});
    Require(encoded.ok(), "int64 canonical comparison key refused");
    return encoded.sort_key;
  };
  Require(int64_key(std::numeric_limits<std::int64_t>::min()) ==
              std::string({char(1), char(0), char(0), char(0), char(0),
                           char(0), char(0), char(0), char(0)}) &&
              int64_key(0) ==
              std::string({char(1), char(0x80), char(0), char(0), char(0),
                           char(0), char(0), char(0), char(0)}) &&
              int64_key(std::numeric_limits<std::int64_t>::max()) ==
              std::string({char(1), char(0xff), char(0xff), char(0xff),
                           char(0xff), char(0xff), char(0xff), char(0xff),
                           char(0xff)}),
          "int64 key is not state plus sign-transformed big-endian bytes");
  const std::array<std::int64_t, 5> int64_ordered{
      std::numeric_limits<std::int64_t>::min(), -1, 0, 1,
      std::numeric_limits<std::int64_t>::max()};
  for (std::size_t left = 0; left < int64_ordered.size(); ++left) {
    for (std::size_t right = 0; right < int64_ordered.size(); ++right) {
      const int expected = left < right ? -1 : (left > right ? 1 : 0);
      const auto compared = dt::CompareDatatypeValues(
          {Int64Value(int64_ordered[left]), Int64Value(int64_ordered[right])});
      const auto left_key = int64_key(int64_ordered[left]);
      const auto right_key = int64_key(int64_ordered[right]);
      const int key_comparison =
          left_key < right_key ? -1 : (left_key > right_key ? 1 : 0);
      Require(compared.ok() && compared.comparison == expected &&
                  key_comparison == expected,
              "int64 comparison and physical key disagree with signed order");
    }
  }
  for (const auto width : {0U, 1U, 4U, 7U, 9U}) {
    const auto malformed = Value(dt::CanonicalTypeId::int64,
                                 std::string(width, '\0'));
    Require(!dt::CompareDatatypeValues({malformed, Int64Value(0)}).ok() &&
                !dt::CompareDatatypeValues({Int64Value(0), malformed}).ok() &&
                !dt::HashDatatypeValue({malformed}).ok() &&
                !dt::MakeDatatypeSortKey({malformed}).ok(),
            "int64 operation admitted a malformed canonical width");
  }
  const auto int64_null = TypedNull(dt::CanonicalTypeId::int64);
  auto int64_described_zero = Int64Value(0);
  int64_described_zero.descriptor = int64_null.descriptor;
  Require(!dt::CompareDatatypeValues(
              {int64_null, int64_described_zero}).ok() &&
              !dt::HashDatatypeValue({Int64Value(0)}).ok() &&
              !dt::HashDatatypeValue({int64_null}).ok() &&
              !dt::RenderDatatypeValueForDisplay({Int64Value(0)}).ok(),
          "int64 unresolved NULL comparison, hash, or display policy did not fail closed");

  const auto uint64_key = [&](std::uint64_t raw) {
    const auto encoded = dt::MakeDatatypeSortKey({Uint64Value(raw)});
    Require(encoded.ok(), "uint64 canonical comparison key refused");
    return encoded.sort_key;
  };
  Require(uint64_key(0) ==
              std::string({char(1), char(0), char(0), char(0), char(0),
                           char(0), char(0), char(0), char(0)}) &&
              uint64_key(1) ==
              std::string({char(1), char(0), char(0), char(0), char(0),
                           char(0), char(0), char(0), char(1)}) &&
              uint64_key(std::numeric_limits<std::uint64_t>::max()) ==
              std::string({char(1), char(0xff), char(0xff), char(0xff),
                           char(0xff), char(0xff), char(0xff), char(0xff),
                           char(0xff)}),
          "uint64 key is not state plus unsigned big-endian bytes");
  const std::array<std::uint64_t, 5> uint64_ordered{
      0, 1, 0x7fffffffffffffffULL, 0x8000000000000000ULL,
      std::numeric_limits<std::uint64_t>::max()};
  for (std::size_t left = 0; left < uint64_ordered.size(); ++left) {
    for (std::size_t right = 0; right < uint64_ordered.size(); ++right) {
      const int expected = left < right ? -1 : (left > right ? 1 : 0);
      const auto compared = dt::CompareDatatypeValues(
          {Uint64Value(uint64_ordered[left]), Uint64Value(uint64_ordered[right])});
      const auto left_key = uint64_key(uint64_ordered[left]);
      const auto right_key = uint64_key(uint64_ordered[right]);
      const int key_comparison =
          left_key < right_key ? -1 : (left_key > right_key ? 1 : 0);
      Require(compared.ok() && compared.comparison == expected &&
                  key_comparison == expected,
              "uint64 comparison and physical key disagree with unsigned order");
    }
  }
  for (const auto width : {0U, 1U, 4U, 7U, 9U}) {
    const auto malformed = Value(dt::CanonicalTypeId::uint64,
                                 std::string(width, '\0'));
    Require(!dt::CompareDatatypeValues({malformed, Uint64Value(0)}).ok() &&
                !dt::CompareDatatypeValues({Uint64Value(0), malformed}).ok() &&
                !dt::HashDatatypeValue({malformed}).ok() &&
                !dt::MakeDatatypeSortKey({malformed}).ok(),
            "uint64 operation admitted a malformed canonical width");
  }
  const auto uint64_null = TypedNull(dt::CanonicalTypeId::uint64);
  auto uint64_described_zero = Uint64Value(0);
  uint64_described_zero.descriptor = uint64_null.descriptor;
  const auto uint64_null_first = dt::MakeDatatypeSortKey({uint64_null});
  dt::DatatypeSortKeyRequest uint64_null_last_request;
  uint64_null_last_request.value = uint64_null;
  uint64_null_last_request.null_ordering = dt::DatatypeNullOrdering::nulls_last;
  const auto uint64_null_last =
      dt::MakeDatatypeSortKey(uint64_null_last_request);
  Require(uint64_null_first.ok() && uint64_null_last.ok() &&
              uint64_null_first.sort_key == std::string(1, '\0') &&
              uint64_null_last.sort_key == std::string(1, '\2') &&
              !dt::CompareDatatypeValues(
                  {uint64_null, uint64_described_zero}).ok() &&
              !dt::HashDatatypeValue({Uint64Value(0)}).ok() &&
              !dt::HashDatatypeValue({uint64_null}).ok() &&
              !dt::RenderDatatypeValueForDisplay({Uint64Value(0)}).ok(),
          "uint64 unresolved NULL comparison, hash, or display policy did not fail closed");

  const std::vector<std::string> int128_ordered{
      "-170141183460469231731687303715884105728", "-1", "0", "1",
      "170141183460469231731687303715884105727"};
  for (std::size_t left = 0; left < int128_ordered.size(); ++left) {
    for (std::size_t right = 0; right < int128_ordered.size(); ++right) {
      const int expected = left < right ? -1 : left > right ? 1 : 0;
      const auto compared = dt::CompareDatatypeValues(
          {Int128Value(int128_ordered[left]),
           Int128Value(int128_ordered[right])});
      Require(compared.ok() && compared.comparison == expected,
              "int128 LE16 comparison disagrees with signed order");
    }
  }
  for (const auto width : {0U, 1U, 8U, 15U, 17U}) {
    auto malformed = Value(dt::CanonicalTypeId::int128,
                           std::string(width, '\0'));
    malformed.descriptor = Descriptor(dt::CanonicalTypeId::int128);
    Require(!dt::CompareDatatypeValues(
                 {malformed, Int128Value("0")}).ok() &&
                !dt::CompareDatatypeValues(
                 {Int128Value("0"), malformed}).ok() &&
                !dt::HashDatatypeValue({malformed}).ok() &&
                !dt::MakeDatatypeSortKey({malformed}).ok(),
            "int128 operation admitted malformed LE16");
  }
  const auto int128_zero = Int128Value("0");
  const auto int128_null = TypedNull(dt::CanonicalTypeId::int128);
  const auto int128_null_first = dt::MakeDatatypeSortKey({int128_null});
  dt::DatatypeSortKeyRequest int128_null_last_request;
  int128_null_last_request.value = int128_null;
  int128_null_last_request.null_ordering =
      dt::DatatypeNullOrdering::nulls_last;
  const auto int128_null_last =
      dt::MakeDatatypeSortKey(int128_null_last_request);
  Require(!dt::MakeDatatypeSortKey({int128_zero}).ok() &&
              !dt::HashDatatypeValue({int128_zero}).ok() &&
              int128_null_first.ok() && int128_null_last.ok() &&
              int128_null_first.sort_key == std::string(1, '\0') &&
              int128_null_last.sort_key == std::string(1, '\2') &&
              !dt::CompareDatatypeValues({int128_null, int128_zero}).ok(),
          "int128 unresolved key/hash/NULL comparison policy did not fail closed");

  const auto uint128_key = [](std::string_view decimal) {
    const auto key = dt::MakeDatatypeSortKey({Uint128Value(decimal)});
    Require(key.ok(), "uint128 boundary comparison key refused");
    return key.sort_key;
  };
  const auto smallest = std::string(1, '\1') + std::string(16, '\0');
  const auto largest = std::string(1, '\1') + std::string(16, static_cast<char>(255));
  Require(uint128_key("0") == smallest &&
              uint128_key("340282366920938463463374607431768211455") == largest,
          "uint128 key is not state plus unsigned big-endian bytes");
  std::string one = smallest;
  one.back() = '\1';
  Require(uint128_key("1") == one && smallest < one,
          "uint128 one is not encoded in the least significant key byte");

  std::string staged = "sentinel";
  for (const auto invalid : {"-1", "-0", "+1", "00", " 1", "1.0",
                             "340282366920938463463374607431768211456"}) {
    const auto before = staged;
    Require(!dt::EncodeCanonicalUint128Value(invalid, &staged) &&
                staged == before,
            "uint128 lexical boundary admitted invalid text");
  }
  for (const auto width : {0u, 1u, 8u, 15u, 17u}) {
    auto malformed = Uint128Value("0");
    malformed.encoded_value.resize(width, '\0');
    Require(!dt::CompareDatatypeValues({malformed, Uint128Value("0")}).ok() &&
                !dt::HashDatatypeValue({malformed}).ok() &&
                !dt::MakeDatatypeSortKey({malformed}).ok(),
            "uint128 operations admitted malformed LE16");
  }
  const std::vector<std::string> ordered{
      "0", "1", "18446744073709551616",
      "170141183460469231731687303715884105728",
      "340282366920938463463374607431768211455"};
  for (std::size_t left = 0; left < ordered.size(); ++left) {
    for (std::size_t right = 0; right < ordered.size(); ++right) {
      const int expected = left < right ? -1 : (left > right ? 1 : 0);
      const auto compared = dt::CompareDatatypeValues(
          {Uint128Value(ordered[left]), Uint128Value(ordered[right])});
      const auto left_key = uint128_key(ordered[left]);
      const auto right_key = uint128_key(ordered[right]);
      const int key_comparison = left_key < right_key ? -1
          : left_key > right_key ? 1 : 0;
      Require(compared.ok() && compared.comparison == expected &&
                  key_comparison == expected,
              "uint128 comparison and key disagree with unsigned order");
    }
  }
  const auto uint128_zero = Uint128Value("0");
  const auto uint128_null = TypedNull(dt::CanonicalTypeId::uint128);
  dt::DatatypeSortKeyRequest null_request;
  null_request.value = uint128_null;
  const auto first = dt::MakeDatatypeSortKey(null_request);
  null_request.null_ordering = dt::DatatypeNullOrdering::nulls_last;
  const auto last = dt::MakeDatatypeSortKey(null_request);
  Require(first.ok() && last.ok() && first.sort_key == std::string(1, '\0') &&
              last.sort_key == std::string(1, '\2') &&
              first.sort_key < smallest && largest < last.sort_key &&
              !dt::HashDatatypeValue({uint128_zero}).ok() &&
              !dt::CompareDatatypeValues({uint128_null, uint128_zero}).ok(),
          "uint128 state keys or unresolved hash/NULL policy drifted");
}

}  // namespace

int main(int argc, char** argv) {
  const bool uuid_only = argc == 2 && std::string_view(argv[1]) == "--binary-uuid-only";
  Require(argc == 1 || uuid_only, "unknown datatype test mode");
  // MDF-014-CURRENT-CORE-DATATYPE-COMPARISON-CASTS
  // DEFER-DPE-COMPARISON-KEYS
  // DEFER-DPE-CAST-STORAGE
  // CURRENT-CORE-DATATYPE-COMPARISON-KEYS
  // CURRENT-CORE-DATATYPE-CAST-STORAGE
  // CURRENT-CORE-DATATYPE-DISPLAY-BOUNDARY
  // CURRENT-CORE-DATATYPE-LOCALE-COLLATION
  // CURRENT-CORE-DATATYPE-NONSCALAR-OPERATORS
  TestBinaryUuidOperations();
  if (uuid_only) return EXIT_SUCCESS;
  TestIntegerPhysicalSortKeyBytes();
  TestOrderedKeysAndResourceBoundComparison();
  TestLocaleSpecificCharacterCollationProof();
  TestNumericOperationsUseTypedSemantics();
  TestCastPersistenceAndSilentDowngradeRefusal();
  TestNonScalarOperatorCastProof();
  TestStableHashAndDeserializationRefusals();
  TestExplicitDisplayBoundaryRendering();
  TestBitStringRequiresSpecializedV3Carrier();
  TestDateRequiresSpecializedD710Carrier();
  TestTimeRequiresSpecializedD710Carrier();
  TestTimestampRequiresSpecializedD710Carrier();
  TestIntervalRequiresSpecializedD710Carrier();
  std::cout << "current_core_datatype_comparison_cast_gate=passed\n";
  return EXIT_SUCCESS;
}
