// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "sblr_runtime.hpp"
#include "../executor/descriptor_value_runtime.hpp"
#include "../internal_api/catalog/column_metadata_codec.hpp"
#include "../internal_api/query/projection_api.hpp"
#include "datatype_operations.hpp"
#include "sbl_numeric.hpp"

namespace scratchbird::engine::sblr {
// Refusal routing only. Full admission below still checks the registry tuple,
// occurrence, modifiers and selected codec; names never supply authority.
inline bool ReferencesDecimalProjection(const internal_api::EngineDescriptor& d) noexcept {
  constexpr SblrUuid type{{0x01,0x9d,0,0,0,0,0x70,0,0x80,0,0,0,0,0,0xd7,0x13}};
  constexpr SblrUuid datatype{{0xa0,0,0,0,0x64,0x65,0x73,0x69,0xad,0x61,0x6c,0,0,0,0,0}};
  return d.canonical_type_name == "decimal" || d.type_uuid == type ||
      d.datatype_descriptor_uuid == datatype;
}

inline bool BindDecimalProjectionDescriptor(const internal_api::EngineDescriptor& binding,
    ExecutionTypeDescriptor* descriptor,
    core::datatypes::DatatypeDecimalCodecBindingV1* codec) {
  return executor::BuildBoundDecimalExecutionTypeDescriptor(binding, descriptor, codec);
}

inline bool DecimalProjectionBytesValid(const ExecutionTypeDescriptor& descriptor,
    bool is_null, const std::vector<std::uint8_t>& bytes) noexcept {
  namespace n = libraries::sbl_numeric;
  if (is_null) return descriptor.nullable_allowed && bytes.empty();
  return n::ValidateExactDecimal(bytes.data(), bytes.size(),
      {descriptor.precision <= 38 ? n::ExactDecimalCodec::le24_v1 : n::ExactDecimalCodec::le40_v1,
       descriptor.precision, descriptor.scale}) == n::ExactDecimalError::none;
}

inline bool DecimalProjectionArgumentValid(const internal_api::EngineProjectionFunctionArgument& a) {
  using State = internal_api::EngineValueState;
  ExecutionTypeDescriptor descriptor;
  core::datatypes::DatatypeDecimalCodecBindingV1 codec;
  return a.type_name == "decimal" && a.encoded_value.empty() &&
      (a.state == State::value || a.state == State::sql_null) &&
      a.is_null == (a.state == State::sql_null) &&
      BindDecimalProjectionDescriptor(a.descriptor, &descriptor, &codec) &&
      DecimalProjectionBytesValid(descriptor, a.is_null, a.binary_value);
}

inline bool SblrDecimalProjectionResolved(const SblrValue& value) {
  if (value.descriptor_id != "decimal" || !value.projection_descriptor ||
      !value.text_value.empty() || !value.encoded_value.empty() ||
      !value.charset_name.empty() || !value.collation_name.empty() ||
      !value.uuid_value.is_nil() || !value.uuid_array_value.empty() ||
      value.has_int64_value || value.has_uint64_value || value.has_real64_value ||
      value.int64_value != 0 || value.uint64_value != 0 || value.real64_value != 0.0 ||
      value.payload_kind != (value.is_null ? SblrValuePayloadKind::none : SblrValuePayloadKind::binary))
    return false;
  ExecutionTypeDescriptor descriptor;
  core::datatypes::DatatypeDecimalCodecBindingV1 codec;
  return BindDecimalProjectionDescriptor(*value.projection_descriptor, &descriptor, &codec) &&
      DecimalProjectionBytesValid(descriptor, value.is_null, value.binary_value);
}
}  // namespace scratchbird::engine::sblr
