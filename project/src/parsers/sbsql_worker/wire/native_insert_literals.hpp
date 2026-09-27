// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include <optional>
#include <string>
#include <string_view>

namespace scratchbird::parser::sbsql {
inline int InsertLiteralHexDigit(char value) noexcept {
  if (value >= '0' && value <= '9') return value - '0';
  if (value >= 'a' && value <= 'f') return value - 'a' + 10;
  if (value >= 'A' && value <= 'F') return value - 'A' + 10;
  return -1;
}

// Parser-only SQL spelling conversion. The result owns native value bytes,
// never a system identity. Nil, all-ones and every UUID data version are valid.
inline std::optional<std::string> DecodeInsertUuidDataLiteral(std::string_view text) {
  if (text.size() != 36 || text[8] != '-' || text[13] != '-' ||
      text[18] != '-' || text[23] != '-') return std::nullopt;
  std::string bytes;
  bytes.reserve(16);
  for (std::size_t index = 0; index < text.size();) {
    if (index == 8 || index == 13 || index == 18 || index == 23) { ++index; continue; }
    const int high = InsertLiteralHexDigit(text[index++]);
    const int low = InsertLiteralHexDigit(text[index++]);
    if (high < 0 || low < 0) return std::nullopt;
    bytes.push_back(static_cast<char>((high << 4) | low));
  }
  return bytes;
}

inline std::optional<std::string> DecodeInsertBinaryLiteral(std::string_view text) {
  if (text.size() % 2) return std::nullopt;
  std::string bytes;
  bytes.reserve(text.size() / 2);
  for (std::size_t index = 0; index < text.size(); index += 2) {
    const int high = InsertLiteralHexDigit(text[index]);
    const int low = InsertLiteralHexDigit(text[index + 1]);
    if (high < 0 || low < 0) return std::nullopt;
    bytes.push_back(static_cast<char>((high << 4) | low));
  }
  return bytes;
}
} // namespace scratchbird::parser::sbsql
