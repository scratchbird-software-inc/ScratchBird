// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "../api_types.hpp"
#include "retained_row_value.hpp"
#include "datatype_type_codec_identity_v3.hpp"
#include <bit>
#include <optional>
#include <stdexcept>

namespace scratchbird::engine::internal_api {
// The retained MGA row carrier owns strings of bytes. UUID and binary data
// must survive conversion to that carrier without formatting or emptying them.
inline CrudStoredValue CrudTypedValuePayload(const EngineTypedValue& value) {
  if (static_cast<unsigned>(value.state) > static_cast<unsigned>(EngineValueState::protected_value))
    throw std::invalid_argument("invalid value state");
  if (value.isSqlNull()) {
    if (!value.encoded_value.empty() || !value.binary_value.empty())
      throw std::invalid_argument("SQL NULL has a payload");
    return CrudStoredValue::SqlNull();
  }
  if (!EngineValueStateHasPayload(value.state)) {
    if (!value.encoded_value.empty() || !value.binary_value.empty())
      throw std::invalid_argument("payload-free value state has a payload");
    return {value.state, {}};
  }
  const auto& type = value.descriptor.canonical_type_name;
  const bool uuid = type == "uuid" || type == "uuid16" || type == "uuidv7";
  // Storage adaptation preserves the admitted carrier, not a display name.
  // Named domains and other descriptor-bound types can own native bytes too.
  // Their owning operation validates the exact type before calling this helper.
  if (!value.binary_value.empty()) {
    if (!value.encoded_value.empty() || (uuid && value.binary_value.size() != 16))
      throw std::invalid_argument("ambiguous or malformed native value carrier");
    return {value.state, std::string(reinterpret_cast<const char*>(value.binary_value.data()), value.binary_value.size())};
  }
  // Retained columnar strings already contain octets, not UUID spellings.
  if (uuid && value.encoded_value.size() != 16)
    throw std::invalid_argument("UUID data requires binary16");
  return {value.state, value.encoded_value};
}

inline bool CrudUuidValue(const EngineTypedValue& value) {
  const auto& type = value.descriptor.canonical_type_name;
  return type == "uuid" || type == "uuid16" || type == "uuidv7";
}

// A NULL comparison is UNKNOWN. Malformed carriers are refused by the API;
// internal predicates also fail closed if invoked without that preflight.
inline std::optional<std::string> CrudPredicateValuePayload(const EngineTypedValue& value) {
  if (value.isSqlNull() || value.state != EngineValueState::value) return std::nullopt;
  try { return CrudTypedValuePayload(value).bytes; }
  catch (const std::invalid_argument&) { return std::nullopt; }
}

template <typename LegacyComparator>
std::optional<int> CompareCrudPredicateValue(const CrudStoredValue& left,
                                            const EngineTypedValue& right,
                                            LegacyComparator legacy_compare) {
  if (!left.isPresent() || right.isSqlNull() || right.state != EngineValueState::value) return std::nullopt;
  if (!right.binary_value.empty() || !right.descriptor.datatype_descriptor_uuid.is_nil() ||
      !right.descriptor.type_uuid.is_nil()) {
    const auto& descriptor = right.descriptor;
    const auto* codec = core::datatypes::FindDatatypeTypeCodecIdentityV3(
        descriptor.datatype_cohort.catalog_snapshot_uuid,
        descriptor.datatype_cohort.catalog_generation,
        descriptor.datatype_cohort.registry_generation,
        descriptor.datatype_descriptor_uuid, descriptor.datatype_descriptor_generation);
    const auto int32_code = static_cast<std::uint32_t>(core::datatypes::CanonicalTypeId::int32);
    bool int32_identity = codec && codec->legacy_fields.canonical_binary_type_code == int32_code;
    // Failed/conflicting tuples cannot be reclassified by their type label.
    // A coherent non-INT32 tuple needs no registry scan.
    if (!int32_identity && (!codec || descriptor.type_uuid != codec->legacy_fields.type_uuid) &&
        (!descriptor.datatype_descriptor_uuid.is_nil() || !descriptor.type_uuid.is_nil())) {
      for (const auto& registered : core::datatypes::CurrentDatatypeTypeCodecIdentityRowsV3()) {
        const auto& identity = registered.legacy_fields;
        if (identity.canonical_binary_type_code == int32_code &&
            (descriptor.datatype_descriptor_uuid == identity.descriptor_uuid ||
             descriptor.type_uuid == identity.type_uuid)) { int32_identity = true; break; }
      }
    }
    if (int32_identity) {
      if (!codec || codec->legacy_fields.canonical_binary_type_code != int32_code ||
          descriptor.type_uuid != codec->legacy_fields.type_uuid || !right.encoded_value.empty() ||
          left.bytes.size() != 4 || right.binary_value.size() != 4) return std::nullopt;
      const auto decode = [](const auto& bytes) {
        std::uint32_t bits = 0;
        for (unsigned i = 0; i < 4; ++i)
          bits |= static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[i])) << (8 * i);
        return std::bit_cast<std::int32_t>(bits);
      };
      const auto a = decode(left.bytes), b = decode(right.binary_value);
      return a < b ? -1 : a > b ? 1 : 0;
    }
    if (!right.binary_value.empty() && (descriptor.canonical_type_name == "integer" || descriptor.canonical_type_name == "int" ||
        descriptor.canonical_type_name == "int32")) return std::nullopt;
  }
  const auto payload = CrudPredicateValuePayload(right);
  if (!payload) return std::nullopt;
  if (CrudUuidValue(right)) {
    if (left.bytes.size() != 16) return std::nullopt;
    return left.bytes.compare(*payload);
  }
  if (!right.binary_value.empty() || right.descriptor.canonical_type_name == "binary")
    return left.bytes.compare(*payload);
  return legacy_compare(left.bytes, *payload);
}
} // namespace scratchbird::engine::internal_api
