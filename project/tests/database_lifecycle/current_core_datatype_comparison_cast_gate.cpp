// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "datatype_operations.hpp"

#include <cstdlib>
#include <iostream>
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

dt::DatatypeTextSeedAuthority TextSeed() {
  dt::DatatypeTextSeedAuthority seed;
  seed.active = true;
  seed.seed_pack_name = "initial-resource-pack";
  seed.seed_pack_version = "1";
  seed.charset_name = "UTF-8";
  seed.collation_name = "unicode_ci";
  seed.collation_case_insensitive = true;
  return seed;
}

void TestOrderedKeysAndResourceBoundComparison() {
  dt::DatatypeSortKeyRequest left_key;
  left_key.value = Value(dt::CanonicalTypeId::int32, "2");
  dt::DatatypeSortKeyRequest right_key;
  right_key.value = Value(dt::CanonicalTypeId::int32, "10");
  const auto left = dt::MakeDatatypeSortKey(left_key);
  const auto right = dt::MakeDatatypeSortKey(right_key);
  Require(left.ok() && right.ok(), "MDF-014 ordered integer keys failed");
  Require(left.sort_key < right.sort_key,
          "MDF-014 integer sort key order is unstable");

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
              sort_key.sort_key.find("initial-resource-pack:1:unicode_ci") !=
                  std::string::npos,
          "MDF-014 text sort key did not bind resource identity");
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

}  // namespace

int main() {
  // MDF-014-CURRENT-CORE-DATATYPE-COMPARISON-CASTS
  // DEFER-DPE-COMPARISON-KEYS
  // DEFER-DPE-CAST-STORAGE
  TestOrderedKeysAndResourceBoundComparison();
  TestCastPersistenceAndSilentDowngradeRefusal();
  TestStableHashAndDeserializationRefusals();
  std::cout << "current_core_datatype_comparison_cast_gate=passed\n";
  return EXIT_SUCCESS;
}
