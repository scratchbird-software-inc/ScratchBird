// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "exact_datatype_descriptor_fixture.hpp"
#include <bit>
#include <charconv>

namespace scratchbird::tests {
inline engine::internal_api::EngineTypedValue NativeInt64Fixture(
    const engine::internal_api::EngineDescriptor& descriptor, std::string_view decimal) {
  std::int64_t number = 0;
  const auto parsed = std::from_chars(decimal.data(), decimal.data() + decimal.size(), number);
  if (parsed.ec != std::errc{} || parsed.ptr != decimal.data() + decimal.size())
    throw std::runtime_error("invalid integer test fixture");
  engine::internal_api::EngineTypedValue value;
  value.descriptor = descriptor;
  for (unsigned byte = 0; byte < 8; ++byte)
    value.binary_value.push_back(static_cast<std::uint8_t>(static_cast<std::uint64_t>(number) >> (8 * byte)));
  return value;
}
inline bool NativeInt64Equals(const engine::internal_api::EngineTypedValue& value, std::int64_t expected) {
  if (value.is_null || value.state != engine::internal_api::EngineValueState::value ||
      value.descriptor.canonical_type_name != "int64" || !value.encoded_value.empty() ||
      value.binary_value.size() != 8) return false;
  std::uint64_t bits = 0;
  for (unsigned byte = 0; byte < 8; ++byte) bits |= std::uint64_t(value.binary_value[byte]) << (8 * byte);
  return std::bit_cast<std::int64_t>(bits) == expected;
}
}  // namespace scratchbird::tests
