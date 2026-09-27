// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include <array>
#include <charconv>
#include <cstdint>
#include <optional>
#include <string_view>

namespace scratchbird::parser::sbsql {
// SQL spelling is consumed only at the parser boundary. UUID data, unlike
// system identity handles, admits every possible 128-bit value.
inline std::optional<std::array<std::uint8_t, 16>> NativeUuidLiteralBytes(
    std::string_view spelling) {
  if (spelling.size() != 36) return std::nullopt;
  std::array<std::uint8_t, 16> bytes{};
  std::size_t offset = 0;
  for (std::size_t ordinal = 0; ordinal < bytes.size(); ++ordinal) {
    if ((ordinal == 4 || ordinal == 6 || ordinal == 8 || ordinal == 10) &&
        spelling[offset++] != '-') return std::nullopt;
    unsigned value = 0;
    const auto parsed = std::from_chars(spelling.data() + offset,
        spelling.data() + offset + 2, value, 16);
    if (parsed.ec != std::errc{} || parsed.ptr != spelling.data() + offset + 2)
      return std::nullopt;
    bytes[ordinal] = static_cast<std::uint8_t>(value);
    offset += 2;
  }
  return bytes;
}
}  // namespace scratchbird::parser::sbsql
