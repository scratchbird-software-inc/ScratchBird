// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "internal_api/api_types.hpp"
#include <string_view>

namespace scratchbird::engine {
inline bool PublicReal64ScalarTypeV1(std::string_view type) noexcept {
  constexpr std::string_view expected = "real64";
  if (type.size() != expected.size()) return false;
  for (std::size_t i = 0; i < type.size(); ++i) {
    const auto ch = type[i];
    if ((ch >= 'A' && ch <= 'Z' ? ch + ('a' - 'A') : ch) != expected[i])
      return false;
  }
  return true;
}
// Already-admitted IEEE754 LE8, including signed zero and non-finite values.
// This is a transport projection, not a second descriptor admission policy.
inline bool PublicReal64ScalarPayloadV1(const internal_api::EngineTypedValue& value,
                                      std::string_view* payload) noexcept {
  if (!payload || !PublicReal64ScalarTypeV1(value.descriptor.canonical_type_name) ||
      !value.encoded_value.empty()) return false;
  using State = internal_api::EngineValueState;
  if (value.state == State::sql_null) {
    if (!value.is_null || !value.binary_value.empty()) return false;
    *payload = {};
    return true;
  }
  if (value.state != State::value || value.is_null || value.binary_value.size() != 8)
    return false;
  *payload = {reinterpret_cast<const char*>(value.binary_value.data()), 8};
  return true;
}
}  // namespace scratchbird::engine
