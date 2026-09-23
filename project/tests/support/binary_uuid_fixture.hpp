// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "../../src/core/platform/runtime_platform.hpp"

#include <cstdint>

namespace scratchbird::tests {

// Explicit compile-time fixture input only. This preserves all 128 bits and
// UUID versions, including intentionally invalid system identities in tests.
// It does not add runtime text construction to the production UUID type.
consteval core::platform::Uuid FixtureUuidLiteral(const char (&text)[37]) {
  core::platform::Uuid value{};
  const auto hex = [](char c) -> unsigned {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    throw "non-hexadecimal UUID fixture";
  };
  unsigned nibble = 0;
  for (unsigned index = 0; index < 36; ++index) {
    if (index == 8 || index == 13 || index == 18 || index == 23) {
      if (text[index] != '-') throw "malformed UUID fixture separator";
      continue;
    }
    const auto digit = hex(text[index]);
    value.bytes[nibble / 2] |= static_cast<core::platform::byte>(
        digit << ((nibble % 2) == 0 ? 4 : 0));
    ++nibble;
  }
  if (text[36] != '\0') throw "unterminated UUID fixture";
  return value;
}

static_assert(FixtureUuidLiteral("00000000-0000-0000-0000-000000000000").is_nil());
static_assert(FixtureUuidLiteral("01234567-89ab-cdef-0123-456789abcdef").bytes ==
              std::array<core::platform::byte, 16>{
                  0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef,
                  0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef});

// Fixed binary test identities, not a runtime UUID generator. The namespace and
// ordinal are explicit fixture coordinates, never hashes of identity labels.
constexpr core::platform::Uuid FixtureUuid(std::uint32_t domain,
                                          std::uint32_t ordinal) noexcept {
  core::platform::Uuid uuid{};
  uuid.bytes[0] = 0x01;
  uuid.bytes[1] = 0x9d;
  uuid.bytes[6] = 0x70;
  uuid.bytes[8] = 0x80;
  for (unsigned i = 0; i < 4; ++i) {
    uuid.bytes[2 + i] = static_cast<core::platform::byte>(domain >> (24 - 8 * i));
    uuid.bytes[12 + i] = static_cast<core::platform::byte>(ordinal >> (24 - 8 * i));
  }
  return uuid;
}

static_assert(!FixtureUuid(1, 1).is_nil());
static_assert(FixtureUuid(1, 1) != FixtureUuid(1, 2));
static_assert(FixtureUuid(1, 1) != FixtureUuid(2, 1));

}  // namespace scratchbird::tests
