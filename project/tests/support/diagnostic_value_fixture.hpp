// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "../../src/core/platform/runtime_platform.hpp"
#include <stdexcept>
#include <string_view>

namespace scratchbird::tests {

// Privacy/token oracles inspect both alternatives without silently skipping a
// native UUID. This view is test data only, never a system identity key.
inline std::string_view DiagnosticValueBytes(
    const core::platform::DiagnosticArgumentValue& value) {
  if (const auto* text = std::get_if<std::string>(&value)) return *text;
  if (const auto* uuid = std::get_if<core::platform::Uuid>(&value))
    return {reinterpret_cast<const char*>(uuid->bytes.data()), uuid->bytes.size()};
  throw std::logic_error("valueless diagnostic argument in fixture");
}

inline bool DiagnosticTextEquals(const core::platform::DiagnosticArgumentValue& value,
                                 std::string_view expected) {
  const auto* text = std::get_if<std::string>(&value);
  return text && *text == expected;
}

// Deliberate human-readable test failure output, not engine processing.
inline std::string DiagnosticArgumentDisplay(const core::platform::DiagnosticArgument& argument) {
  if (const auto* text = argument.text()) return *text;
  if (const auto* uuid = argument.uuid()) {
    constexpr char digits[] = "0123456789abcdef";
    std::string out;
    for (std::size_t n = 0; n < uuid->bytes.size(); ++n) {
      if (n == 4 || n == 6 || n == 8 || n == 10) out += '-';
      out += digits[uuid->bytes[n] >> 4];
      out += digits[uuid->bytes[n] & 15];
    }
    return out;
  }
  throw std::logic_error("valueless diagnostic argument in fixture");
}

}  // namespace scratchbird::tests
