// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "sblr_runtime.hpp"
#include "../internal_api/query/projection_api.hpp"

namespace scratchbird::engine::sblr {
// ENGINE-PROJECTION-UINT16-LE-V1: immutable codec identity, not live statement
// admission. Core's datatype.uint16.le.v1 remains the storage codec authority.
namespace uint16_projection_detail {
inline constexpr SblrUuid type{{
      0x01,0x9d,0,0,0,0,0x70,0,0x80,0,0,0,0,0,0xd8,0x06}};
inline constexpr SblrUuid datatype{{
      0x79,0,0,0,0x75,0x69,0x7e,0x74,0xb1,0x36,0,0,0,0,0,0}};
}  // namespace uint16_projection_detail

// Any uint16 identity routes to strict validation, including a conflicting
// name. This is refusal routing, not admission by name or by partial identity.
inline bool ReferencesUint16Projection(
    const internal_api::EngineDescriptor& descriptor) noexcept {
  return descriptor.canonical_type_name == "uint16" ||
      descriptor.type_uuid == uint16_projection_detail::type ||
      descriptor.datatype_descriptor_uuid == uint16_projection_detail::datatype;
}

inline bool Uint16ProjectionDescriptorValid(
    const internal_api::EngineDescriptor& descriptor) noexcept {
  const auto& occurrence = descriptor.descriptor_uuid;
  return descriptor.descriptor_kind == "scalar" &&
      descriptor.canonical_type_name == "uint16" &&
      (occurrence.bytes[6] & 0xf0) == 0x70 &&
      (occurrence.bytes[8] & 0xc0) == 0x80 &&
      descriptor.type_uuid == uint16_projection_detail::type &&
      descriptor.datatype_descriptor_uuid == uint16_projection_detail::datatype &&
      descriptor.datatype_descriptor_generation == 1 &&
      descriptor.charset_uuid.is_nil() && descriptor.collation_uuid.is_nil();
}

inline bool Uint16ProjectionArgumentValid(
    const internal_api::EngineProjectionFunctionArgument& argument) noexcept {
  using State = internal_api::EngineValueState;
  if (argument.type_name != "uint16" ||
      !Uint16ProjectionDescriptorValid(argument.descriptor) ||
      !argument.encoded_value.empty() ||
      (argument.state != State::value && argument.state != State::sql_null))
    return false;
  return argument.binary_value.size() ==
      (argument.is_null || argument.state == State::sql_null ? 0u : 2u);
}

// Validate the callable's native unsigned representation independently of
// projection binding. No decimal string or second byte carrier is permitted.
inline bool SblrUint16PayloadValid(const SblrValue& value) noexcept {
  if (value.descriptor_id != "uint16" || !value.text_value.empty() ||
      !value.encoded_value.empty() || !value.binary_value.empty() ||
      !value.uuid_value.is_nil() || !value.uuid_array_value.empty() ||
      !value.charset_name.empty() || !value.collation_name.empty() ||
      value.has_int64_value || value.has_real64_value ||
      value.int64_value != 0 || value.real64_value != 0.0)
    return false;
  if (value.is_null)
    return value.payload_kind == SblrValuePayloadKind::none &&
        !value.has_uint64_value && value.uint64_value == 0;
  return value.payload_kind == SblrValuePayloadKind::unsigned_integer &&
      value.has_uint64_value && value.uint64_value <= 65535;
}

inline bool SblrUint16ProjectionResolved(const SblrValue& value) noexcept {
  return SblrUint16PayloadValid(value) && value.projection_descriptor &&
      Uint16ProjectionDescriptorValid(*value.projection_descriptor);
}
}  // namespace scratchbird::engine::sblr
