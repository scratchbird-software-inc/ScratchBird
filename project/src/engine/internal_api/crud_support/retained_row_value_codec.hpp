// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "retained_row_value.hpp"
#include "mga_relation_store/mga_binary_fields.hpp"
#include <optional>
#include <stdexcept>
#include <unordered_set>

namespace scratchbird::engine::internal_api {

// Explicit state framing for retained named values (including covering-index
// payloads). This is not a datatype descriptor or an order-preserving key.
inline constexpr std::string_view kCrudValuesMagic = "SBVALS01";

inline std::string EncodeCrudValues(const CrudValueFields& values) {
  if (values.size() > std::numeric_limits<std::uint32_t>::max())
    throw std::length_error("retained field count exceeds frame capacity");
  std::string output(kCrudValuesMagic);
  AppendBinaryU32(&output, static_cast<std::uint32_t>(values.size()));
  std::unordered_set<std::string_view> names;
  for (const auto& [name, value] : values) {
    if (!value.valid() || !names.insert(name).second)
      throw std::invalid_argument("invalid or duplicate retained field");
    if (!AppendBinaryString(&output, name))
      throw std::length_error("retained field name exceeds frame capacity");
    output.push_back(static_cast<char>(value.state));
    if (!AppendBinaryString(&output, value.bytes))
      throw std::length_error("retained payload exceeds frame capacity");
  }
  return output;
}

inline std::optional<CrudValueFields> DecodeCrudValues(std::string_view encoded) {
  if (!encoded.starts_with(kCrudValuesMagic)) return std::nullopt;
  const auto input = std::span(reinterpret_cast<const std::uint8_t*>(encoded.data()), encoded.size());
  std::size_t cursor = kCrudValuesMagic.size();
  std::uint32_t count = 0;
  if (!ReadBinaryU32(input, &cursor, &count) || count > (input.size() - cursor) / 9)
    return std::nullopt;
  CrudValueFields fields;
  fields.reserve(count);
  std::unordered_set<std::string> names;
  for (std::uint32_t i = 0; i < count; ++i) {
    std::string name;
    std::string payload;
    std::uint8_t state = 0;
    if (!ReadBinaryString(input, &cursor, &name) ||
        !ReadBinaryU8(input, &cursor, &state) ||
        !ReadBinaryString(input, &cursor, &payload) || !names.insert(name).second)
      return std::nullopt;
    CrudStoredValue value{static_cast<EngineValueState>(state), std::move(payload)};
    if (!value.valid()) return std::nullopt;
    fields.emplace_back(std::move(name), std::move(value));
  }
  if (cursor != input.size()) return std::nullopt;
  return fields;
}

}  // namespace scratchbird::engine::internal_api
