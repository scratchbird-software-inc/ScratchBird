// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "datatype_operations.hpp"
#include "datatype_document.hpp"

#include <cstdlib>
#include <initializer_list>
#include <iostream>
#include <vector>
#include <string>
#include <string_view>
#include <utility>

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

dt::DatatypeOperationValue Value(dt::CanonicalTypeId type,
                                 std::string encoded) {
  return {type, std::move(encoded), false};
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
  seed.active = true;
  seed.seed_pack_name = "initial-resource-pack";
  seed.seed_pack_version = "1";
  seed.charset_name = std::move(charset_name);
  seed.collation_name = std::move(collation_name);
  seed.collation_case_insensitive = case_insensitive;
  seed.collation_accent_insensitive = accent_insensitive;
  return seed;
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
      Value(dt::CanonicalTypeId::int64, "-257"),
      Value(dt::CanonicalTypeId::int64, "-1"),
      Value(dt::CanonicalTypeId::int64, "0"),
      Value(dt::CanonicalTypeId::int64, "2"),
      Value(dt::CanonicalTypeId::int64, "256")};
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
      Value(dt::CanonicalTypeId::uint64, "2"),
      Value(dt::CanonicalTypeId::uint64, "256"),
      Value(dt::CanonicalTypeId::uint64, "18446744073709551615")};
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
      Value(dt::CanonicalTypeId::decimal, "-1.20"),
      Value(dt::CanonicalTypeId::decimal, "-1.10"),
      Value(dt::CanonicalTypeId::decimal, "0"),
      Value(dt::CanonicalTypeId::decimal, "2.10"),
      Value(dt::CanonicalTypeId::decimal, "10.01")};
  for (std::size_t index = 1; index < decimal_values.size(); ++index) {
    const auto compare = dt::CompareDatatypeValues(
        {decimal_values[index - 1], decimal_values[index]});
    Require(compare.ok() && compare.comparison < 0,
            "MDF-014 decimal comparison order drifted");
    const auto left = dt::MakeDatatypeSortKey({decimal_values[index - 1]});
    const auto right = dt::MakeDatatypeSortKey({decimal_values[index]});
    Require(left.ok() && right.ok() && left.sort_key < right.sort_key,
            "MDF-014 decimal sort key order drifted");
  }

  dt::DatatypeOperationValue null_value;
  null_value.type_id = dt::CanonicalTypeId::int64;
  null_value.is_null = true;
  const auto nulls_first = dt::CompareDatatypeValues(
      {null_value, Value(dt::CanonicalTypeId::int64, "0"),
       dt::DatatypeNullOrdering::nulls_first});
  Require(nulls_first.ok() && nulls_first.comparison < 0,
          "MDF-014 nulls-first comparison order drifted");
  const auto nulls_last = dt::CompareDatatypeValues(
      {null_value, Value(dt::CanonicalTypeId::int64, "0"),
       dt::DatatypeNullOrdering::nulls_last});
  Require(nulls_last.ok() && nulls_last.comparison > 0,
          "MDF-014 nulls-last comparison order drifted");

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
  Require(sort_key.ok() &&
              sort_key.sort_key.find("initial-resource-pack:1:UNICODE_CI") !=
                  std::string::npos,
          "MDF-014 text sort key did not bind resource identity");
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
  Require(sort_a.sort_key.find("initial-resource-pack:1:UNICODE_CI_AI:UTF8:ci:ai") !=
              std::string::npos,
          "MDF-014 accent-insensitive sort key did not bind seed flags");

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
  add.left = Value(dt::CanonicalTypeId::int128,
                   "170141183460469231731687303715884105726");
  add.right = Value(dt::CanonicalTypeId::int128, "1");
  const auto added = dt::ApplyNumericOperation(add);
  Require(added.ok() &&
              added.value.encoded_value ==
                  "170141183460469231731687303715884105727",
          "MDF-014 int128 numeric add drifted");

  dt::DatatypeNumericOperationRequest compare;
  compare.operation = dt::DatatypeNumericOperationKind::compare;
  compare.type_id = dt::CanonicalTypeId::uint128;
  compare.left = Value(dt::CanonicalTypeId::uint128, "255");
  compare.right = Value(dt::CanonicalTypeId::uint128, "256");
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
  const auto int128_cast = dt::CastDatatypeValue(cast);
  Require(int128_cast.ok(), "MDF-014 int128 cast failed");

  const auto serialized =
      dt::SerializeDatatypeValue({int128_cast.value});
  Require(serialized.ok(), "MDF-014 cast serialization failed");
  const auto deserialized =
      dt::DeserializeDatatypeValue({dt::CanonicalTypeId::int128,
                                    serialized.serialized_value});
  Require(deserialized.ok(), "MDF-014 cast persistence decode failed");
  Require(deserialized.value.type_id == dt::CanonicalTypeId::int128,
          "MDF-014 persisted cast target type mismatch");
  Require(deserialized.value.encoded_value == int128_cast.value.encoded_value,
          "MDF-014 persisted cast payload mismatch");

  cast.value = Value(dt::CanonicalTypeId::int32, "1000");
  cast.target_type_id = dt::CanonicalTypeId::int8;
  cast.explicit_cast = true;
  const auto precision_loss = dt::CastDatatypeValue(cast);
  Require(!precision_loss.ok(), "MDF-014 accepted precision-losing int8 cast");
  Require(precision_loss.diagnostic.diagnostic_code == "SB_DATATYPE_CAST_REJECTED",
          "MDF-014 precision-loss diagnostic mismatch");

  cast.value = Value(dt::CanonicalTypeId::real128, "1.25");
  cast.target_type_id = dt::CanonicalTypeId::real64;
  cast.explicit_cast = false;
  const auto silent_downgrade = dt::CastDatatypeValue(cast);
  Require(!silent_downgrade.ok(),
          "MDF-014 accepted silent real128 downgrade");
  Require(silent_downgrade.diagnostic.diagnostic_code ==
              "SB_DATATYPE_CAST_REJECTED",
          "MDF-014 silent downgrade diagnostic mismatch");
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
                  "SB_DATATYPE_CAST_REJECTED",
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
                  "SB_DATATYPE_CAST_REJECTED",
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
      {Value(dt::CanonicalTypeId::int32, "2"),
       Value(dt::CanonicalTypeId::int32, "1"),
       Value(dt::CanonicalTypeId::int32, "2")});
  Require(encoded_left.ok(), "MDF-014 set encoding failed");
  const auto encoded_right = dt::EncodeSetValue(
      set_descriptor,
      {Value(dt::CanonicalTypeId::int32, "1"),
       Value(dt::CanonicalTypeId::int32, "2")});
  Require(encoded_right.ok(), "MDF-014 set equality fixture encoding failed");

  dt::DatatypeSetOperationRequest set_operation;
  set_operation.descriptor = set_descriptor;
  set_operation.left_encoded_set = encoded_left.encoded_set;
  set_operation.operation = dt::DatatypeSetOperationKind::cardinality;
  auto set_result = dt::ApplySetOperation(set_operation);
  Require(set_result.ok() && set_result.value.encoded_value == "2",
          "MDF-014 set cardinality drifted");

  set_operation.operation = dt::DatatypeSetOperationKind::membership;
  set_operation.right_encoded_set_or_value = "1";
  set_result = dt::ApplySetOperation(set_operation);
  Require(set_result.ok() && set_result.value.encoded_value == "true",
          "MDF-014 set membership failed");
  set_operation.right_encoded_set_or_value = "3";
  set_result = dt::ApplySetOperation(set_operation);
  Require(set_result.ok() && set_result.value.encoded_value == "false",
          "MDF-014 set non-membership failed");

  set_operation.operation = dt::DatatypeSetOperationKind::equals;
  set_operation.right_encoded_set_or_value = encoded_right.encoded_set;
  set_result = dt::ApplySetOperation(set_operation);
  Require(set_result.ok() && set_result.value.encoded_value == "true",
          "MDF-014 set equality failed");
  set_operation.operation = dt::DatatypeSetOperationKind::subset;
  set_result = dt::ApplySetOperation(set_operation);
  Require(set_result.ok() && set_result.value.encoded_value == "true",
          "MDF-014 set subset failed");
  set_operation.operation = dt::DatatypeSetOperationKind::superset;
  set_result = dt::ApplySetOperation(set_operation);
  Require(set_result.ok() && set_result.value.encoded_value == "true",
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
  hash_request.value = Value(dt::CanonicalTypeId::uuid,
                             "018f8a2a-1b2c-7def-8123-456789abcdef");
  const auto first = dt::HashDatatypeValue(hash_request);
  const auto second = dt::HashDatatypeValue(hash_request);
  Require(first.ok() && second.ok() &&
              first.stable_hash_hex == second.stable_hash_hex,
          "MDF-014 stable hash changed");

  const auto serialized = dt::SerializeDatatypeValue({hash_request.value});
  Require(serialized.ok(), "MDF-014 UUID serialization failed");
  const auto wrong_type =
      dt::DeserializeDatatypeValue({dt::CanonicalTypeId::int64,
                                    serialized.serialized_value});
  Require(!wrong_type.ok(), "MDF-014 accepted mismatched deserialization type");
  Require(wrong_type.diagnostic.diagnostic_code ==
              "SB_DATATYPE_DESERIALIZATION_REJECTED",
          "MDF-014 mismatched deserialization diagnostic mismatch");
}

void TestExplicitDisplayBoundaryRendering() {
  dt::DatatypeOperationValue null_value;
  null_value.type_id = dt::CanonicalTypeId::int64;
  null_value.is_null = true;
  const auto rendered_null = dt::RenderDatatypeValueForDisplay({null_value});
  Require(rendered_null.ok() && rendered_null.explicit_display_boundary &&
              rendered_null.display_value == "NULL",
          "MDF-014 null display boundary drifted");

  const auto rendered_binary = dt::RenderDatatypeValueForDisplay(
      {Value(dt::CanonicalTypeId::binary, std::string("A\0B", 3))});
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
                  "SB_DATATYPE_DISPLAY_RENDER_REJECTED",
          "MDF-014 unknown display boundary did not fail closed");
}

}  // namespace

int main() {
  // MDF-014-CURRENT-CORE-DATATYPE-COMPARISON-CASTS
  // CURRENT-CORE-DATATYPE-COMPARISON-KEYS
  // CURRENT-CORE-DATATYPE-CAST-STORAGE
  // CURRENT-CORE-DATATYPE-DISPLAY-BOUNDARY
  // CURRENT-CORE-DATATYPE-LOCALE-COLLATION
  // CURRENT-CORE-DATATYPE-NONSCALAR-OPERATORS
  TestOrderedKeysAndResourceBoundComparison();
  TestLocaleSpecificCharacterCollationProof();
  TestNumericOperationsUseTypedSemantics();
  TestCastPersistenceAndSilentDowngradeRefusal();
  TestNonScalarOperatorCastProof();
  TestStableHashAndDeserializationRefusals();
  TestExplicitDisplayBoundaryRendering();
  std::cout << "current_core_datatype_comparison_cast_gate=passed\n";
  return EXIT_SUCCESS;
}
