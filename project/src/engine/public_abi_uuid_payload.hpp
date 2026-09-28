// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "internal_api/api_types.hpp"

#include <string_view>

namespace scratchbird::engine {

inline bool PublicUuidScalarTypeV1(std::string_view type) noexcept {
  const auto matches = [type](std::string_view expected) {
    if (type.size() != expected.size()) return false;
    for (std::size_t i = 0; i != type.size(); ++i) {
      const auto ch = type[i];
      if ((ch >= 'A' && ch <= 'Z' ? ch + ('a' - 'A') : ch) != expected[i])
        return false;
    }
    return true;
  };
  return matches("uuid") || matches("uuid16") || matches("uuidv7");
}

// Payload projection only: the caller retains descriptor/authority admission.
// UUID data is not a system identity and preserves every bit, including nil.
// A successful view borrows the native arm; no text decoding or allocation.
// Failure leaves the destination unchanged. SQL NULL is empty, never nil UUID.
inline bool PublicUuidScalarPayloadV1(
    const internal_api::EngineTypedValue& value,
    std::string_view* payload) noexcept {
  if (!payload || !PublicUuidScalarTypeV1(value.descriptor.canonical_type_name) ||
      !value.encoded_value.empty()) return false;
  using State = internal_api::EngineValueState;
  if (value.state == State::sql_null) {
    if (!value.is_null || !value.binary_value.empty()) return false;
    *payload = {};
    return true;
  }
  if (value.state != State::value || value.is_null ||
      value.binary_value.size() != 16) return false;
  *payload = {reinterpret_cast<const char*>(value.binary_value.data()), 16};
  return true;
}

}  // namespace scratchbird::engine
