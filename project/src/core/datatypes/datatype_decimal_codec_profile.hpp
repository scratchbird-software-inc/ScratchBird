// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "uuid.hpp"
#include <cstdint>

namespace scratchbird::core::datatypes {
// Occurrence-local representation binding, shared by parser and engine carriers.
// Presence does not issue statement authority or authorize execution.
struct DecimalCodecProfile {
  platform::Uuid uuid;
  std::uint64_t generation{0};
  bool operator==(const DecimalCodecProfile&) const = default;
};
inline constexpr platform::Uuid kExactDecimal24CodecUuid{{
    0x01,0xa1,0x18,0x6b,0xdf,0x04,0x76,0x0a,0xa3,0xd5,0xd0,0x2d,0x2e,0xe3,0x01,0x54}};
inline constexpr platform::Uuid kExactDecimal40CodecUuid{{
    0x01,0xa1,0x18,0x6b,0xdf,0x04,0x7b,0x1b,0x9c,0xaf,0x87,0xb3,0xbd,0x29,0x94,0x3e}};

// Representation validation only; does not grant occurrence/receipt authority.
inline bool ExactDecimalCodecProfileMatches(const platform::Uuid& uuid,
    std::uint64_t generation, std::uint32_t precision, std::uint32_t scale) noexcept {
  return generation == 1 && precision >= 1 && precision <= 76 && scale <= precision &&
      uuid == (precision <= 38 ? kExactDecimal24CodecUuid : kExactDecimal40CodecUuid);
}
}  // namespace scratchbird::core::datatypes
