// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "mga_relation_store/mga_binary_fields.hpp"
#include <optional>
#include <stdexcept>
#include <vector>

namespace scratchbird::engine::internal_api {
// Lossless logical tuple projection, not a datatype comparison key or an
// authority receipt. Physical typed keys retain their own descriptor binding.
// Versioned framing deliberately does not admit the ambiguous delimiter form.
inline constexpr std::string_view kCompositeLogicalKeyMagic = "SBCLKEY1";

inline std::string EncodeCompositeLogicalKey(
    const std::vector<std::string_view>& components) {
  if (components.size() < 2 ||
      components.size() > std::numeric_limits<std::uint32_t>::max())
    throw std::length_error("invalid composite logical key arity");
  std::string encoded(kCompositeLogicalKeyMagic);
  AppendBinaryU32(&encoded, static_cast<std::uint32_t>(components.size()));
  for (const auto component : components) {
    if (!AppendBinaryString(&encoded, component))
      throw std::length_error("composite logical key component too large");
  }
  return encoded;
}

inline std::optional<std::vector<std::string_view>> DecodeCompositeLogicalKey(
    std::string_view encoded, std::size_t expected_arity) {
  if (expected_arity < 2 || !encoded.starts_with(kCompositeLogicalKeyMagic))
    return std::nullopt;
  const auto input = std::span(
      reinterpret_cast<const std::uint8_t*>(encoded.data()), encoded.size());
  std::size_t cursor = kCompositeLogicalKeyMagic.size();
  std::uint32_t count = 0;
  if (!ReadBinaryU32(input, &cursor, &count) || count != expected_arity ||
      count > (input.size() - cursor) / sizeof(std::uint32_t))
    return std::nullopt;
  std::vector<std::string_view> result;
  result.reserve(count);
  for (std::uint32_t i = 0; i < count; ++i) {
    std::uint32_t width = 0;
    if (!ReadBinaryU32(input, &cursor, &width) || width > input.size() - cursor)
      return std::nullopt;
    result.push_back(encoded.substr(cursor, width));
    cursor += width;
  }
  if (cursor != input.size()) return std::nullopt;
  return result;
}
}  // namespace scratchbird::engine::internal_api
