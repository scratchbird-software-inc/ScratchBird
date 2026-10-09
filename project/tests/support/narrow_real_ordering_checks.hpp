// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "datatype_operations.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace scratchbird::tests {
template<class Check>
void CheckNarrowRealOrdering(core::datatypes::CanonicalTypeId type,
                            const engine::ExecutionTypeDescriptor& descriptor,
                            Check check) {
  namespace dt = core::datatypes;
  const unsigned width = type == dt::CanonicalTypeId::real32 ? 4 : 2;
  const unsigned fraction = type == dt::CanonicalTypeId::bfloat16 ? 7
      : type == dt::CanonicalTypeId::real16 ? 10 : 23;
  const unsigned exponent_bits = width * 8 - fraction - 1;
  const unsigned exponent_max = (1u << exponent_bits) - 1;
  const int bias = (1 << (exponent_bits - 1)) - 1;
  const std::uint32_t sign = std::uint32_t{1} << (width * 8 - 1);
  const auto value = [&](std::uint32_t bits) {
    dt::DatatypeOperationValue out;
    out.type_id = type; out.descriptor = descriptor;
    for (unsigned i = 0; i < width; ++i)
      out.encoded_value.push_back(static_cast<char>(bits >> (8 * i)));
    return out;
  };
  std::vector<std::pair<double, std::uint32_t>> finite;
  const auto inspect = [&](std::uint32_t bits) {
    const unsigned exponent = (bits >> fraction) & exponent_max;
    const auto operand = value(bits);
    if (exponent == exponent_max) {
      const auto key = dt::MakeDatatypeSortKey({operand});
      const auto hash = dt::HashDatatypeValue({operand});
      const auto compared = dt::CompareDatatypeValues({operand, value(0)});
      check(!key.ok() && key.sort_key.empty() &&
            !hash.ok() && hash.stable_hash_hex.empty() &&
            !compared.ok() && compared.comparison == 0,
            "ordinary narrow-real ordering rejects infinity and every sampled NaN payload");
      return;
    }
    // Independent numeric oracle. Every binary16/bfloat16/binary32 finite
    // value is exactly representable in IEEE binary64; ldexp never rounds here.
    const auto fraction_value = bits & ((std::uint32_t{1} << fraction) - 1);
    double numeric = std::ldexp(double(fraction_value),
        (exponent ? int(exponent) - bias : 1 - bias) - int(fraction));
    if (exponent) numeric += std::ldexp(1.0, int(exponent) - bias);
    if (bits & sign) numeric = -numeric;
    finite.emplace_back(numeric, bits);
  };
  if (width == 2) {
    for (std::uint32_t bits = 0; bits <= 0xffff; ++bits) inspect(bits);
  } else {
    std::uint32_t state = 0x92476ab1u;
    for (unsigned i = 0; i < 10000; ++i) {
      state ^= state << 13; state ^= state >> 17; state ^= state << 5;
      inspect(state);
    }
    for (const auto bits : {0u, 1u, 0x7fffffu, 0x800000u, 0x3f800000u,
                           0x7f7fffffu, 0x7f800000u, 0x7f800001u,
                           0x7fc00000u, 0x7fffffffu}) {
      inspect(bits); inspect(bits | sign);
    }
  }
  std::sort(finite.begin(), finite.end());
  std::string previous_key;
  for (std::size_t i = 0; i < finite.size(); ++i) {
    const auto operand = value(finite[i].second);
    const auto key = dt::MakeDatatypeSortKey({operand});
    check(key.ok() && key.sort_key.size() == width + 1 && key.sort_key[0] == '\1',
          "finite key has explicit PRESENT state and native ordered width");
    if (i) {
      const auto compared = dt::CompareDatatypeValues({value(finite[i - 1].second), operand});
      const bool equal = finite[i - 1].first == finite[i].first;
      check(compared.ok() && compared.comparison == (equal ? 0 : -1) &&
            (equal ? previous_key == key.sort_key : previous_key < key.sort_key),
            "numeric comparison and unsigned canonical-byte key match independent finite oracle");
    }
    previous_key = key.sort_key;
  }
  const auto positive_zero = value(0), negative_zero = value(sign);
  const auto zero_hash = dt::HashDatatypeValue({positive_zero});
  const auto negative_hash = dt::HashDatatypeValue({negative_zero});
  check(zero_hash.ok() && negative_hash.ok() &&
        zero_hash.stable_hash_hex == negative_hash.stable_hash_hex,
        "signed zeros hash equally without rewriting stored bits");
  check(negative_zero.encoded_value.back() == static_cast<char>(0x80),
        "ordering leaves original negative-zero carrier intact");
  auto nonnullable = positive_zero;
  nonnullable.descriptor.nullable_allowed = false;
  const auto present_comparison = dt::CompareDatatypeValues({nonnullable, positive_zero});
  check(present_comparison.ok() && present_comparison.comparison == 0 &&
        dt::MakeDatatypeSortKey({nonnullable}).sort_key == dt::MakeDatatypeSortKey({positive_zero}).sort_key &&
        dt::HashDatatypeValue({nonnullable}).stable_hash_hex == zero_hash.stable_hash_hex,
        "containing-slot nullability does not change PRESENT numeric identity");
  for (unsigned invalid_setting = 0; invalid_setting < 2; ++invalid_setting) {
    dt::DatatypeSortKeyRequest key_request{positive_zero};
    dt::DatatypeComparisonRequest comparison_request{positive_zero, negative_zero};
    if (invalid_setting == 0) {
      key_request.null_ordering = static_cast<dt::DatatypeNullOrdering>(255);
      comparison_request.null_ordering = key_request.null_ordering;
    } else {
      key_request.case_insensitive_character_compare = true;
      comparison_request.case_insensitive_character_compare = true;
    }
    const auto key = dt::MakeDatatypeSortKey(key_request);
    const auto comparison = dt::CompareDatatypeValues(comparison_request);
    check(!key.ok() && key.sort_key.empty() && !comparison.ok() && comparison.comparison == 0,
          "invalid NULL placement and character-only configuration cannot grant numeric ordering");
  }
  auto null = positive_zero;
  null.is_null = true; null.encoded_value.clear(); null.descriptor.nullable_allowed = true;
  for (const auto order : {dt::DatatypeNullOrdering::nulls_first, dt::DatatypeNullOrdering::nulls_last}) {
    dt::DatatypeSortKeyRequest key_request{null}; key_request.null_ordering = order;
    dt::DatatypeComparisonRequest compare_request{null, positive_zero}; compare_request.null_ordering = order;
    const auto key = dt::MakeDatatypeSortKey(key_request);
    const auto compared = dt::CompareDatatypeValues(compare_request);
    check(key.ok() && key.sort_key == std::string(1, order == dt::DatatypeNullOrdering::nulls_first ? '\0' : '\2') &&
          compared.ok() && compared.comparison == (order == dt::DatatypeNullOrdering::nulls_first ? -1 : 1),
          "typed NULL follows explicitly selected NULL ordering");
  }
  const auto refuse = [&](const auto& operand, const char* code) {
    const auto key = dt::MakeDatatypeSortKey({operand});
    const auto hash = dt::HashDatatypeValue({operand});
    const auto compared = dt::CompareDatatypeValues({operand, positive_zero});
    check(!key.ok() && key.sort_key.empty() && key.diagnostic.diagnostic_code == code &&
          !hash.ok() && hash.stable_hash_hex.empty() && hash.diagnostic.diagnostic_code == code &&
          !compared.ok() && compared.comparison == 0 && compared.diagnostic.diagnostic_code == code,
          "invalid input emits exact diagnostic and no partial key/hash/comparison");
  };
  auto dirty_null = null; dirty_null.encoded_value = "x";
  refuse(dirty_null, "DATATYPE.NULL_STATE.INVALID");
  dirty_null = null; dirty_null.descriptor.nullable_allowed = false;
  refuse(dirty_null, "DATATYPE.NULL_NOT_ADMITTED");
  for (unsigned length = 0; length <= 8; ++length) {
    if (length == width) continue;
    auto wrong = positive_zero; wrong.encoded_value.assign(length, '\0');
    refuse(wrong, "NUMERIC.ENCODING.NONCANONICAL");
  }
  for (unsigned mutation = 0; mutation < 8; ++mutation) {
    auto wrong = positive_zero;
    switch (mutation) {
      case 0: wrong.descriptor = {}; break;
      case 1: ++wrong.descriptor.descriptor_epoch; break;
      case 2: wrong.descriptor.descriptor_uuid.bytes[0] ^= 1; break;
      case 3: ++wrong.descriptor.canonical_type_id; break;
      case 4: ++wrong.descriptor.bit_width; break;
      case 5: ++wrong.descriptor.precision; break;
      case 6: wrong.descriptor.descriptor_authoritative = false; break;
      case 7: wrong.descriptor.parser_independent = false; break;
    }
    refuse(wrong, "DATATYPE.DESCRIPTOR.INVALID");
  }
}
} // namespace scratchbird::tests
