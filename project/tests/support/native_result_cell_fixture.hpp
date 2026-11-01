// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
// Lightweight native int64 carrier helpers for tests that link only the
// internal API (no datatype catalog). They mirror, byte for byte:
//   - src/engine/executor/native_int64_payload.hpp EncodeInt64Value
//     (8-byte little-endian two's-complement in binary_value, no text), and
//   - src/engine/public_abi_int64_payload.hpp PublicInt64ScalarPayloadV1
//     (int64 cell admitted only with empty encoded_value and 8 binary bytes).
// The engine never carries int64 as text; these helpers never read or write
// encoded_value except to require that it is empty.
#include "api_types.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <utility>

namespace scratchbird::tests {

inline engine::internal_api::EngineTypedValue NativeInt64CellFixture(
    engine::internal_api::EngineDescriptor descriptor, std::int64_t number) {
  engine::internal_api::EngineTypedValue value;
  value.descriptor = std::move(descriptor);
  value.setState(engine::internal_api::EngineValueState::value);
  const auto bits = static_cast<std::uint64_t>(number);
  for (unsigned byte = 0; byte < 8; ++byte)
    value.binary_value.push_back(static_cast<std::uint8_t>(bits >> (8 * byte)));
  return value;
}

// Returns the decoded int64 only for a well-formed native int64 cell.
inline std::optional<std::int64_t> NativeInt64CellValue(
    const engine::internal_api::EngineTypedValue& value) {
  if (value.is_null || value.state != engine::internal_api::EngineValueState::value ||
      value.descriptor.canonical_type_name != "int64" || !value.encoded_value.empty() ||
      value.binary_value.size() != 8) return std::nullopt;
  std::uint64_t bits = 0;
  for (unsigned byte = 0; byte < 8; ++byte)
    bits |= std::uint64_t(value.binary_value[byte]) << (8 * byte);
  return static_cast<std::int64_t>(bits);
}

}  // namespace scratchbird::tests
