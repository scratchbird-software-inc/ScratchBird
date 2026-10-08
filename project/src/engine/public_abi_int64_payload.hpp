// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "internal_api/api_types.hpp"
#include <array>
#include <string_view>

namespace scratchbird::engine {
inline bool PublicInt64ScalarTypeV1(std::string_view type) noexcept {
  return type.size() == 5 && (type[0] == 'i' || type[0] == 'I') &&
         (type[1] == 'n' || type[1] == 'N') &&
         (type[2] == 't' || type[2] == 'T') && type[3] == '6' && type[4] == '4';
}
inline bool PublicInt32ScalarTypeV1(std::string_view type) noexcept {
  return type.size() == 5 && (type[0] == 'i' || type[0] == 'I') &&
         (type[1] == 'n' || type[1] == 'N') &&
         (type[2] == 't' || type[2] == 'T') && type[3] == '3' && type[4] == '2';
}
// Projection of an already admitted scalar only; no descriptor admission or
// text fallback. Borrow native bytes, retain NULL as external state, and leave
// the destination unchanged on malformed state/width/carrier.
inline bool PublicInt64ScalarPayloadV1(const internal_api::EngineTypedValue& value,
                                     std::string_view* payload) noexcept {
  if (!payload || !PublicInt64ScalarTypeV1(value.descriptor.canonical_type_name) ||
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
// The public signed-integer field is fixed LE8. An admitted INT32 retains its
// native LE4 storage/execution carrier and is sign-extended only at this wire
// boundary; column metadata continues to identify INT32. No text conversion.
inline bool PublicSignedIntegerScalarPayloadV1(const internal_api::EngineTypedValue& value,
    std::array<char, 8>* widened, std::string_view* payload) noexcept {
  if (PublicInt64ScalarTypeV1(value.descriptor.canonical_type_name))
    return PublicInt64ScalarPayloadV1(value, payload);
  if (!widened || !payload || !PublicInt32ScalarTypeV1(value.descriptor.canonical_type_name) ||
      !value.encoded_value.empty()) return false;
  using State = internal_api::EngineValueState;
  if (value.state == State::sql_null) {
    if (!value.is_null || !value.binary_value.empty()) return false;
    *payload = {};
    return true;
  }
  if (value.state != State::value || value.is_null || value.binary_value.size() != 4)
    return false;
  std::array<char, 8> bytes;
  bytes.fill(value.binary_value[3] & 0x80 ? static_cast<char>(0xff) : 0);
  for (unsigned i = 0; i < 4; ++i) bytes[i] = static_cast<char>(value.binary_value[i]);
  *widened = bytes;
  *payload = {widened->data(), widened->size()};
  return true;
}
} // namespace scratchbird::engine
