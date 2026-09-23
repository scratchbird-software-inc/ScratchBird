// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "../../src/engine/authority_hash_material.hpp"
#include "../support/binary_uuid_fixture.hpp"
#include <cstdlib>
#include <iostream>
#include <source_location>
using scratchbird::engine::HashAuthorityMaterial;
void Check(bool ok, std::source_location at = std::source_location::current()) {
  if (!ok) { std::cerr << "failure at " << at.line() << '\n'; std::abort(); }
}
int main() {
  auto identity = scratchbird::tests::FixtureUuid(1125, 1);
  identity.bytes[9] = 0; identity.bytes[15] = 0xff;
  const auto original = HashAuthorityMaterial("authority", {identity, "right"}, {UINT64_MAX});
  Check(original == HashAuthorityMaterial("authority", {identity, "right"}, {UINT64_MAX}));
  auto changed = identity; changed.bytes[9] = 1;
  Check(original != HashAuthorityMaterial("authority", {changed, "right"}, {UINT64_MAX}));
  const std::string_view same_bytes(reinterpret_cast<const char*>(identity.bytes.data()), 16);
  Check(original != HashAuthorityMaterial("authority", {same_bytes, "right"}, {UINT64_MAX}));
  const std::string_view left("a\0b", 3), right("b\0c", 3);
  Check(HashAuthorityMaterial("authority", {left, "c"}, {}) !=
        HashAuthorityMaterial("authority", {"a", right}, {}));
  Check(HashAuthorityMaterial("authority", {"a", "b"}, {}) !=
        HashAuthorityMaterial("authority", {"ab"}, {}));
  Check(original != HashAuthorityMaterial("authority2", {identity, "right"}, {UINT64_MAX}));
  Check(original != HashAuthorityMaterial("authority", {identity, "right"}, {UINT64_MAX-1}));
  std::array<std::uint8_t, 32> digest{};
  Check(HashAuthorityMaterial("authority", {}, {}, {digest}) !=
        HashAuthorityMaterial("authority", {}, {0,0,0,0}));
}
