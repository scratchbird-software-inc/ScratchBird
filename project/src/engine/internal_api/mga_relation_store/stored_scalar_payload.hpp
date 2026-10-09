// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "../api_types.hpp"

#include <algorithm>
#include <string_view>

namespace scratchbird::engine::internal_api {

enum class StoredScalarPayloadKindV1 { encoded, uuid16, binary, int32_le4, scalar_le8, timestamp16 };

inline StoredScalarPayloadKindV1 StoredScalarPayloadKindForV1(
    std::string_view type) noexcept {
  const auto matches = [type](std::string_view expected) {
    if (type.size() != expected.size()) return false;
    for (std::size_t i = 0; i != type.size(); ++i) {
      const char ch = type[i];
      if ((ch >= 'A' && ch <= 'Z' ? ch + ('a' - 'A') : ch) != expected[i])
        return false;
    }
    return true;
  };
  if (matches("uuid") || matches("uuidv7")) return StoredScalarPayloadKindV1::uuid16;
  if (matches("int64") || matches("uint64") || matches("real64"))
    return StoredScalarPayloadKindV1::scalar_le8;
  if (matches("int32")) return StoredScalarPayloadKindV1::int32_le4;
  if (matches("timestamp")) return StoredScalarPayloadKindV1::timestamp16;
  if (matches("binary") || matches("bytes") || matches("blob"))
    return StoredScalarPayloadKindV1::binary;
  return StoredScalarPayloadKindV1::encoded;
}

// The caller owns descriptor admission and supplies the stored value state.
// This is payload projection, not system-identity admission: UUID data retains
// every bit, including nil and earlier versions. No text parsing or formatting.
// Failed validation/allocation leaves the destination and its descriptor intact.
inline bool RestoreStoredScalarPayloadV1(std::string_view bytes,
                                         EngineValueState state,
                                         EngineTypedValue* value) {
  if (!value || value->descriptor.canonical_type_name.empty() ||
      (state != EngineValueState::value && state != EngineValueState::sql_null) ||
      (state == EngineValueState::sql_null && !bytes.empty())) return false;
  const auto kind = StoredScalarPayloadKindForV1(value->descriptor.canonical_type_name);
  if (state == EngineValueState::value && kind == StoredScalarPayloadKindV1::uuid16 &&
      bytes.size() != 16) return false;
  if (state == EngineValueState::value && kind == StoredScalarPayloadKindV1::scalar_le8 &&
      bytes.size() != 8) return false;
  if (state == EngineValueState::value && kind == StoredScalarPayloadKindV1::int32_le4 &&
      bytes.size() != 4) return false;
  if (state == EngineValueState::value && kind == StoredScalarPayloadKindV1::timestamp16 &&
      bytes.size() != 16) return false;
  std::string encoded;
  std::vector<std::uint8_t> binary;
  if (state == EngineValueState::value) {
    if (kind == StoredScalarPayloadKindV1::encoded) encoded.assign(bytes);
    else binary.assign(bytes.begin(), bytes.end());
  }
  // Stage before replacing either arm, including when bytes aliases an arm.
  value->encoded_value = std::move(encoded);
  value->binary_value = std::move(binary);
  value->setState(state);
  return true;
}

// Call only after exact descriptor/column binding has been checked. Receipt
// replay must compare the selected carrier and every stored octet, not render
// UUID text or accept a value in an alternative payload arm.
inline bool StoredScalarPayloadMatchesV1(const EngineTypedValue& value,
                                         std::string_view bytes,
                                         EngineValueState state) noexcept {
  if (value.descriptor.canonical_type_name.empty() || value.state != state) return false;
  if (state == EngineValueState::sql_null)
    return bytes.empty() && value.is_null && value.encoded_value.empty() &&
           value.binary_value.empty();
  if (state != EngineValueState::value || value.is_null) return false;
  const auto kind = StoredScalarPayloadKindForV1(value.descriptor.canonical_type_name);
  if (kind == StoredScalarPayloadKindV1::encoded)
    return value.binary_value.empty() && value.encoded_value == bytes;
  if (!value.encoded_value.empty() || value.binary_value.size() != bytes.size() ||
      (kind == StoredScalarPayloadKindV1::uuid16 && bytes.size() != 16) ||
      (kind == StoredScalarPayloadKindV1::int32_le4 && bytes.size() != 4) ||
      (kind == StoredScalarPayloadKindV1::timestamp16 && bytes.size() != 16) ||
      (kind == StoredScalarPayloadKindV1::scalar_le8 && bytes.size() != 8)) return false;
  return std::equal(value.binary_value.begin(), value.binary_value.end(), bytes.begin(),
                    [](std::uint8_t left, char right) {
                      return left == static_cast<std::uint8_t>(right);
                    });
}

}  // namespace scratchbird::engine::internal_api
