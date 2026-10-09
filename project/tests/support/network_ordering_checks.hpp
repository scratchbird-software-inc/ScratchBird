// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "datatype_operations.hpp"
#include <algorithm>
#include <array>
#include <string>
#include <vector>

namespace scratchbird::tests {
template<class Check>
void CheckNetworkOrdering(core::datatypes::CanonicalTypeId type,
                          const engine::ExecutionTypeDescriptor& descriptor,
                          Check check) {
  namespace dt = core::datatypes;
  const bool prefix = type == dt::CanonicalTypeId::network_prefix;
  const unsigned width = prefix ? 18 : type == dt::CanonicalTypeId::ip_address ? 16 : 8;
  const auto operand = [&](const std::vector<unsigned char>& bytes) {
    dt::DatatypeOperationValue value{type, {bytes.begin(), bytes.end()}, false, descriptor};
    return value;
  };
  std::vector<std::vector<unsigned char>> values;
  const auto refused = [&](const auto& value, const char* code = nullptr) {
    const auto key = dt::MakeDatatypeSortKey({value});
    const auto hash = dt::HashDatatypeValue({value});
    const auto compared = dt::CompareDatatypeValues({value, value});
    check(!key.ok() && key.sort_key.empty() && !hash.ok() && hash.stable_hash_hex.empty() &&
          !compared.ok() && compared.comparison == 0 &&
          (!code || (key.diagnostic.diagnostic_code == code && hash.diagnostic.diagnostic_code == code &&
                     compared.diagnostic.diagnostic_code == code)),
          "network invalid input refuses atomically with the correct diagnostic");
  };
  if (prefix) {
    for (unsigned family = 0; family < 256; ++family) {
      for (unsigned length = 0; length < 256; ++length) {
        std::vector<unsigned char> bytes(18);
        bytes[16] = length; bytes[17] = family;
        if (family == 4) bytes[10] = bytes[11] = 0xff;
        const bool valid = (family == 4 && length <= 32) || (family == 6 && length <= 128);
        if (!valid) { refused(operand(bytes)); continue; }
        values.push_back(bytes);
        const unsigned start = family == 4 ? 96 : 0;
        for (unsigned bit = start; bit < 128; ++bit) {
          auto changed = bytes;
          changed[bit / 8] |= 0x80 >> (bit % 8);
          if (bit < start + length) {
            const auto key = dt::MakeDatatypeSortKey({operand(changed)});
            check(key.ok(), "network-prefix bit inside prefix is admitted");
          } else {
            refused(operand(changed));
          }
        }
        if (family == 4) {
          for (unsigned i = 0; i < 12; ++i) {
            auto changed = bytes; changed[i] ^= 1;
            refused(operand(changed));
          }
        }
      }
    }
    // Opposing addresses establish family precedence independently of address
    // order, while nested prefixes establish the final length discriminator.
    values.push_back({0,0,0,0,0,0,0,0,0,0,255,255,255,255,255,255,32,4});
    values.push_back({0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,128,6});
  } else {
    values.push_back(std::vector<unsigned char>(width, 0));
    values.push_back(std::vector<unsigned char>(width, 255));
    for (unsigned bit = 0; bit < width * 8; ++bit) {
      auto bytes = std::vector<unsigned char>(width, 0);
      bytes[bit / 8] = 0x80 >> (bit % 8);
      values.push_back(bytes);
    }
    // Canonical mapped IPv4 / zero-extended MAC-48 are ordinary native octets.
    if (width == 16) values.push_back({0,0,0,0,0,0,0,0,0,0,255,255,192,0,2,1});
    else values.push_back({0,0,8,0,43,1,2,3});
  }
  const auto material = [&](const auto& bytes) {
    if (!prefix) return bytes;
    std::vector<unsigned char> result{bytes[17]};
    result.insert(result.end(), bytes.begin(), bytes.begin() + 16);
    result.push_back(bytes[16]);
    return result;
  };
  std::sort(values.begin(), values.end(), [&](const auto& a, const auto& b) { return material(a) < material(b); });
  for (std::size_t i = 0; i < values.size(); ++i) {
    const auto value = operand(values[i]);
    const auto key = dt::MakeDatatypeSortKey({value});
    const auto oracle = material(values[i]);
    std::string expected(1, '\1'); expected.append(oracle.begin(), oracle.end());
    check(key.ok() && key.sort_key == expected && value.encoded_value == std::string(values[i].begin(), values[i].end()),
          "network sort key matches independent unsigned-byte oracle without changing stored bits");
    const auto hash = dt::HashDatatypeValue({value});
    auto alias = value; alias.descriptor.stable_name = "display alias";
    alias.descriptor.nullable_allowed = !value.descriptor.nullable_allowed;
    const auto alias_hash = dt::HashDatatypeValue({alias});
    const auto alias_compare = dt::CompareDatatypeValues({value, alias});
    check(hash.ok() && alias_hash.ok() && hash.stable_hash_hex == alias_hash.stable_hash_hex &&
          alias_compare.ok() && alias_compare.comparison == 0,
          "network equality/hash retain identity across aliases and PRESENT slot nullability");
    if (i) {
      const auto previous = operand(values[i - 1]);
      const auto compared = dt::CompareDatatypeValues({previous, value});
      const int expected_order = material(values[i - 1]) == oracle ? 0 : -1;
      check(compared.ok() && compared.comparison == expected_order,
            "network comparison agrees with address/family/prefix ordering");
      const auto reverse = dt::CompareDatatypeValues({value, previous});
      check(reverse.ok() && reverse.comparison == -expected_order,
            "network order is antisymmetric");
    }
  }
  const auto present = operand(values.front());
  for (unsigned setting = 0; setting < 2; ++setting) {
    dt::DatatypeSortKeyRequest key_request{present};
    dt::DatatypeComparisonRequest comparison_request{present, present};
    if (setting == 0) {
      key_request.null_ordering = static_cast<dt::DatatypeNullOrdering>(255);
      comparison_request.null_ordering = key_request.null_ordering;
    } else {
      key_request.case_insensitive_character_compare = true;
      comparison_request.case_insensitive_character_compare = true;
    }
    const auto key = dt::MakeDatatypeSortKey(key_request);
    const auto compared = dt::CompareDatatypeValues(comparison_request);
    check(!key.ok() && key.sort_key.empty() && !compared.ok() && compared.comparison == 0,
          "network ordering rejects invalid NULL placement and character-only settings");
  }
  auto foreign = present;
  foreign.type_id = type == dt::CanonicalTypeId::mac_address
      ? dt::CanonicalTypeId::ip_address : dt::CanonicalTypeId::mac_address;
  const auto mixed = dt::CompareDatatypeValues({present, foreign});
  check(!mixed.ok() && mixed.comparison == 0,
        "network comparison cannot implicitly coerce a different network type");
  for (unsigned size = 0; size <= width + 1; ++size) {
    if (size == width) continue;
    refused(operand(std::vector<unsigned char>(size)));
  }
  auto null = present; null.is_null = true; null.encoded_value.clear(); null.descriptor.nullable_allowed = true;
  for (const auto order : {dt::DatatypeNullOrdering::nulls_first, dt::DatatypeNullOrdering::nulls_last}) {
    dt::DatatypeSortKeyRequest key_request{null}; key_request.null_ordering = order;
    dt::DatatypeComparisonRequest comparison{null, present}; comparison.null_ordering = order;
    const auto key = dt::MakeDatatypeSortKey(key_request);
    const auto compared = dt::CompareDatatypeValues(comparison);
    check(key.ok() && key.sort_key == std::string(1, order == dt::DatatypeNullOrdering::nulls_first ? '\0' : '\2') &&
          compared.ok() && compared.comparison == (order == dt::DatatypeNullOrdering::nulls_first ? -1 : 1),
          "network typed NULL obeys selected ordering");
  }
  auto invalid = null; invalid.encoded_value = "x"; refused(invalid, "DATATYPE.NULL_STATE.INVALID");
  invalid = null; invalid.descriptor.nullable_allowed = false; refused(invalid, "DATATYPE.NULL_NOT_ADMITTED");
  for (unsigned mutation = 0; mutation < 8; ++mutation) {
    invalid = present;
    switch (mutation) {
      case 0: invalid.descriptor = {}; break;
      case 1: ++invalid.descriptor.descriptor_epoch; break;
      case 2: invalid.descriptor.descriptor_uuid.bytes[0] ^= 1; break;
      case 3: ++invalid.descriptor.canonical_type_id; break;
      case 4: ++invalid.descriptor.bit_width; break;
      case 5: ++invalid.descriptor.precision; break;
      case 6: invalid.descriptor.descriptor_authoritative = false; break;
      case 7: invalid.descriptor.parser_independent = false; break;
    }
    refused(invalid, "DATATYPE.DESCRIPTOR.INVALID");
  }
}
} // namespace scratchbird::tests
