// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "../internal_api/api_types.hpp"

namespace scratchbird::engine::executor {
// Payload construction only: the caller owns exact descriptor admission and
// arithmetic bounds. Never reconstructs a binding or renders an integer.
inline internal_api::EngineTypedValue EncodeInt64Value(
    std::int64_t value, const internal_api::EngineDescriptor& descriptor) {
  internal_api::EngineTypedValue result;
  result.descriptor = descriptor;
  result.setState(internal_api::EngineValueState::value);
  result.binary_value.resize(8);
  const auto bits = static_cast<std::uint64_t>(value);
  for (unsigned byte = 0; byte < 8; ++byte)
    result.binary_value[byte] = static_cast<std::uint8_t>(bits >> (byte * 8));
  return result;
}
}  // namespace scratchbird::engine::executor
