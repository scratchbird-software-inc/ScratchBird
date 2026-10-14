// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "internal_api/api_types.hpp"
#include <string_view>

namespace scratchbird::engine {
inline bool PublicBooleanScalarTypeV1(std::string_view type) noexcept {
  const auto matches = [type](std::string_view expected) {
    if (type.size() != expected.size()) return false;
    for (std::size_t i = 0; i < type.size(); ++i) {
      const char ch = type[i];
      if ((ch >= 'A' && ch <= 'Z' ? ch + ('a' - 'A') : ch) != expected[i]) return false;
    }
    return true;
  };
  return matches("boolean") || matches("bool");
}
// Borrow an already-admitted native value without formatting or allocation.
// This does not confer descriptor, transaction or publication authority.
// Invalid input leaves the destination unchanged; NULL is zero payload.
inline bool PublicBooleanScalarPayloadV1(const internal_api::EngineTypedValue& value,
                                        std::string_view* payload) noexcept {
  if (!payload || !PublicBooleanScalarTypeV1(value.descriptor.canonical_type_name) ||
      !value.encoded_value.empty()) return false;
  using State = internal_api::EngineValueState;
  if (value.state == State::sql_null) {
    if (!value.is_null || !value.binary_value.empty()) return false;
    *payload = {};
    return true;
  }
  if (value.state != State::value || value.is_null ||
      value.binary_value.size() != 1 || value.binary_value[0] > 1) return false;
  *payload = {reinterpret_cast<const char*>(value.binary_value.data()), 1};
  return true;
}
}  // namespace scratchbird::engine
