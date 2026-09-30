// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "binary_uuid_fixture.hpp"
#include <stdexcept>
#include <string>

namespace scratchbird::tests {
inline std::string IndexFixtureUuidBytes(const core::platform::Uuid& identity) {
  return {reinterpret_cast<const char*>(identity.bytes.data()), identity.bytes.size()};
}

// Preserve the exact UUID bits of the historical fixtures whose final twelve
// hexadecimal positions were populated with zero-padded decimal row digits.
// This constructs binary fixture data, not a production UUID/text converter.
inline std::string IndexFixtureUuidWithDecimalSuffix(core::platform::Uuid prefix,
                                                     std::uint64_t suffix) {
  if (suffix > 999999999999ULL)
    throw std::invalid_argument("index fixture UUID suffix exceeds twelve digits");
  for (unsigned byte = 0; byte < 6; ++byte) {
    const auto low = suffix % 10;
    suffix /= 10;
    const auto high = suffix % 10;
    suffix /= 10;
    prefix.bytes[15 - byte] = static_cast<core::platform::byte>((high << 4) | low);
  }
  return IndexFixtureUuidBytes(prefix);
}
}  // namespace scratchbird::tests
