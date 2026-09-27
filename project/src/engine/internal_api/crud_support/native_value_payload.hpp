// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "../api_types.hpp"
#include <optional>
#include <stdexcept>

namespace scratchbird::engine::internal_api {
// The retained MGA row carrier owns strings of bytes. UUID and binary data
// must survive conversion to that carrier without formatting or emptying them.
inline std::string CrudTypedValuePayload(const EngineTypedValue& value) {
  if (value.isSqlNull()) {
    if (!value.encoded_value.empty() || !value.binary_value.empty())
      throw std::invalid_argument("SQL NULL has a payload");
    return "<NULL>";
  }
  const auto& type = value.descriptor.canonical_type_name;
  const bool uuid = type == "uuid" || type == "uuid16" || type == "uuidv7";
  const bool octets = type == "binary" || type == "varbinary" || type == "bytea" ||
                      type == "bytes" || type == "blob";
  if (uuid || octets) {
    if (!value.binary_value.empty()) {
      if (!value.encoded_value.empty() || (uuid && value.binary_value.size() != 16))
        throw std::invalid_argument("ambiguous or malformed native value carrier");
      return {reinterpret_cast<const char*>(value.binary_value.data()), value.binary_value.size()};
    }
    // Retained columnar strings already contain octets, not UUID spellings.
    if (uuid && value.encoded_value.size() != 16)
      throw std::invalid_argument("UUID data requires binary16");
  }
  return value.encoded_value;
}

inline bool CrudUuidValue(const EngineTypedValue& value) {
  const auto& type = value.descriptor.canonical_type_name;
  return type == "uuid" || type == "uuid16" || type == "uuidv7";
}

// A NULL comparison is UNKNOWN. Malformed carriers are refused by the API;
// internal predicates also fail closed if invoked without that preflight.
inline std::optional<std::string> CrudPredicateValuePayload(const EngineTypedValue& value) {
  if (value.isSqlNull()) return std::nullopt;
  try { return CrudTypedValuePayload(value); }
  catch (const std::invalid_argument&) { return std::nullopt; }
}

template <typename LegacyComparator>
std::optional<int> CompareCrudPredicateValue(const std::string& left,
                                            const EngineTypedValue& right,
                                            LegacyComparator legacy_compare) {
  const auto payload = CrudPredicateValuePayload(right);
  if (!payload || left == "<NULL>") return std::nullopt;
  if (CrudUuidValue(right)) {
    if (left.size() != 16) return std::nullopt;
    // Never reinterpret UUID octets that happen to spell digits as a number.
    return left.compare(*payload);
  }
  if (!right.binary_value.empty() || right.descriptor.canonical_type_name == "binary")
    return left.compare(*payload);
  return legacy_compare(left, *payload);
}
} // namespace scratchbird::engine::internal_api
