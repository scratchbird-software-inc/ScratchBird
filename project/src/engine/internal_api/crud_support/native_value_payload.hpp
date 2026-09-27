// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "../api_types.hpp"
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
} // namespace scratchbird::engine::internal_api
