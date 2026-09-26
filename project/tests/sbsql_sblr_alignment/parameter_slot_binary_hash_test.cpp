// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "wire/parameter_slot_table.hpp"
#include <openssl/evp.h>
#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string_view>

namespace {
using Slot = scratchbird::parser::ipc::PreparedParameterSlotReference;
using scratchbird::parser::sbsql::wire_detail::ParameterSlotTableBytesV2;
unsigned checks = 0;
void Require(bool ok, std::string_view reason) {
  ++checks;
  if (!ok) { std::cerr << reason << '\n'; std::exit(1); }
}
std::vector<std::uint8_t> Hex(std::string_view value) {
  std::vector<std::uint8_t> bytes;
  auto nibble = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
  for (std::size_t i = 0; i < value.size(); i += 2)
    bytes.push_back((nibble(value[i]) << 4) | nibble(value[i + 1]));
  return bytes;
}
Slot Fixture() {
  Slot slot;
  slot.slot_uuid = {1,157,0,0,0,0,112,0,128,0,0,0,0,0,0,1};
  slot.datatype_descriptor_uuid = {1,157,0,0,0,0,112,0,128,0,0,0,0,0,215,17};
  slot.datatype_type_uuid = {1,157,0,0,0,0,112,0,128,0,0,0,0,0,215,18};
  slot.datatype_descriptor_generation = 0x0807060504030201ULL;
  slot.direction = 3;
  slot.nullable = 1;
  return slot;
}
}  // namespace

int main() {
  const auto fixture = Fixture();
  const auto expected = Hex(
      "53637261746368426972642e53626c72506172616d65746572536c6f74732e5632"
      "0100000000000000019d0000000070008000000000000001"
      "019d000000007000800000000000d711010203040506070803010000");
  const auto encoded = ParameterSlotTableBytesV2({fixture});
  Require(encoded && *encoded == expected, "independent Core 06 V2 binary slot golden");
  std::array<std::uint8_t, 32> digest{};
  unsigned digest_size = 0;
  Require(EVP_Digest(encoded->data(), encoded->size(), digest.data(),
                     &digest_size, EVP_sha256(), nullptr) == 1 && digest_size == 32,
          "SHA256 calculation");
  Require(std::vector<std::uint8_t>(digest.begin(), digest.end()) ==
              Hex("abecd0edc7afe73b40c7d7413539c661f5eec16be3b8bdf42ff23c35e13a9af7"),
          "independent hashlib SHA256 golden");
  Require(!ParameterSlotTableBytesV2({}), "empty table refused");
  for (unsigned identity = 0; identity < 3; ++identity) {
    for (unsigned version = 0; version < 16; ++version) {
      auto changed = fixture;
      auto* id = identity == 0 ? &changed.slot_uuid : identity == 1 ?
          &changed.datatype_descriptor_uuid : &changed.datatype_type_uuid;
      (*id)[6] = static_cast<std::uint8_t>(version << 4);
      Require(ParameterSlotTableBytesV2({changed}).has_value() == (version == 7),
              "only v7 control identities admitted");
    }
    for (unsigned variant : {0U, 64U, 128U, 192U}) {
      auto changed = fixture;
      auto* id = identity == 0 ? &changed.slot_uuid : identity == 1 ?
          &changed.datatype_descriptor_uuid : &changed.datatype_type_uuid;
      (*id)[8] = static_cast<std::uint8_t>(variant);
      Require(ParameterSlotTableBytesV2({changed}).has_value() == (variant == 128),
              "only RFC control UUID variant admitted");
    }
    auto changed = fixture;
    auto* id = identity == 0 ? &changed.slot_uuid : identity == 1 ?
        &changed.datatype_descriptor_uuid : &changed.datatype_type_uuid;
    id->fill(0);
    Require(!ParameterSlotTableBytesV2({changed}), "nil control identity refused");
  }
  for (unsigned direction = 0; direction <= 255; ++direction) {
    auto changed = fixture;
    changed.direction = direction;
    Require(ParameterSlotTableBytesV2({changed}).has_value() ==
                (direction >= 1 && direction <= 3), "direction domain");
  }
  for (unsigned nullable = 0; nullable <= 255; ++nullable) {
    auto changed = fixture;
    changed.nullable = nullable;
    Require(ParameterSlotTableBytesV2({changed}).has_value() == (nullable <= 1),
            "nullable domain");
  }
  auto changed = fixture;
  changed.datatype_descriptor_generation = 0;
  Require(!ParameterSlotTableBytesV2({changed}), "zero generation refused");
  changed.datatype_descriptor_generation = std::numeric_limits<std::uint64_t>::max();
  Require(ParameterSlotTableBytesV2({changed}).has_value(), "full unsigned generation retained");
  changed = fixture;
  changed.slot_ordinal = 1;
  Require(!ParameterSlotTableBytesV2({changed}), "first ordinal must be zero");
  Require(!ParameterSlotTableBytesV2({fixture, changed}), "duplicate slot identity refused");
  changed.slot_uuid.back() = 2;
  auto pair = ParameterSlotTableBytesV2({fixture, changed});
  Require(pair && pair->size() == 133 && (*pair)[33] == 2 &&
              (*pair)[85] == 1 && (*pair)[104] == 2,
          "multiple dense slot rows and little endian count");
  changed.slot_ordinal = 2;
  Require(!ParameterSlotTableBytesV2({fixture, changed}), "ordinal gap refused");
  changed = fixture;
  changed.datatype_type_uuid.back() ^= 1;
  Require(ParameterSlotTableBytesV2({changed}) == encoded,
          "type identity is validated but not added to normative hash fields");
  std::vector<Slot> maximum(4096, fixture);
  for (std::size_t i = 0; i < maximum.size(); ++i) {
    maximum[i].slot_ordinal = i;
    maximum[i].slot_uuid[14] = i >> 8;
    maximum[i].slot_uuid[15] = i;
  }
  auto large = ParameterSlotTableBytesV2(maximum);
  Require(large && large->size() == 37 + 4096 * 48, "maximum slot count admitted");
  maximum.push_back(fixture);
  Require(!ParameterSlotTableBytesV2(maximum), "oversized slot count refused");
  std::cout << "parameter_slot_binary_hash_checks=" << checks << '\n';
}
