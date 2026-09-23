// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "sblr_runtime.hpp"
#include "../internal_api/query/projection_api.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>

// Payload adaptation, not descriptor, provider, resource or publication authority.
// The remaining legacy nonbinary adapters require their own canonical migration.
namespace scratchbird::engine::sblr {
namespace projection_value_detail {
inline std::string LowerAscii(std::string value) {
  for (char& ch : value) if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch + ('a' - 'A'));
  return value;
}
inline std::string FormatReal64(double value) {
  std::ostringstream encoded;
  encoded << std::setprecision(std::numeric_limits<double>::max_digits10) << value;
  return encoded.str();
}
inline bool BinaryCarrierValid(const SblrValue& value) noexcept {
  return !value.is_null && value.payload_kind == SblrValuePayloadKind::binary &&
      (value.descriptor_id == "binary" || value.descriptor_id == "varbinary") &&
      value.text_value.empty() && value.encoded_value.empty() &&
      value.charset_name.empty() && value.collation_name.empty() &&
      value.uuid_value.is_nil() && value.uuid_array_value.empty() && !value.has_int64_value &&
      !value.has_uint64_value && !value.has_real64_value;
}
inline bool EmptyNullCarrier(const SblrValue& value) noexcept {
  return value.is_null && value.payload_kind == SblrValuePayloadKind::none &&
      value.binary_value.empty() && value.text_value.empty() && value.encoded_value.empty() &&
      value.uuid_value.is_nil() && value.uuid_array_value.empty() && !value.has_int64_value && !value.has_uint64_value &&
      !value.has_real64_value && value.charset_name.empty() && value.collation_name.empty();
}
}  // namespace projection_value_detail

inline SblrValue SblrValueFromProjectionArgument(
    const internal_api::EngineProjectionFunctionArgument& argument) {
  SblrValue value;
  const std::string type_name = projection_value_detail::LowerAscii(argument.type_name);
  value.descriptor_id = type_name;
  // Invalid carriers are unresolved non-NULL values, never manufactured NULLs.
  value.is_null = false;
  if (argument.state != internal_api::EngineValueState::value &&
      argument.state != internal_api::EngineValueState::sql_null) return value;
  const bool is_null = argument.is_null ||
      argument.state == internal_api::EngineValueState::sql_null;
  if (is_null) {
    if (!argument.encoded_value.empty() || !argument.binary_value.empty()) return value;
    value.is_null = true;
    return value;
  }
  if (type_name == "binary" || type_name == "varbinary") {
    if (!argument.encoded_value.empty()) return value;
    value.binary_value = argument.binary_value;
    value.payload_kind = SblrValuePayloadKind::binary;
    return value;
  }
  if (type_name == "uuid_array") {
    if (!argument.encoded_value.empty() || argument.binary_value.size() % 16 != 0)
      return value;
    value.uuid_array_value.resize(argument.binary_value.size() / 16);
    for (std::size_t i = 0; i < value.uuid_array_value.size(); ++i)
      std::copy_n(argument.binary_value.begin() + i * 16, 16,
                  value.uuid_array_value[i].bytes.begin());
    value.payload_kind = SblrValuePayloadKind::uuid_array_binary;
    return value;
  }
  if (type_name == "uuid") {
    if (!argument.encoded_value.empty() || argument.binary_value.size() != 16) return value;
    std::copy(argument.binary_value.begin(), argument.binary_value.end(), value.uuid_value.bytes.begin());
    value.payload_kind = SblrValuePayloadKind::uuid_binary;
    return value;
  }
  if (!argument.binary_value.empty()) return value;
  value.encoded_value = argument.encoded_value;
  value.text_value = argument.encoded_value;
  if (type_name == "bigint" || type_name == "integer" ||
      type_name == "int8" || type_name == "int16" ||
      type_name == "int32" || type_name == "int64") {
    const auto parsed = std::from_chars(
        argument.encoded_value.data(),
        argument.encoded_value.data() + argument.encoded_value.size(),
        value.int64_value);
    if (parsed.ec == std::errc{} &&
        parsed.ptr ==
            argument.encoded_value.data() + argument.encoded_value.size()) {
      value.has_int64_value = true;
      value.payload_kind = SblrValuePayloadKind::signed_integer;
      return value;
    }
    value.payload_kind = SblrValuePayloadKind::none;
    return value;
  }
  if (type_name == "uint8" || type_name == "uint16" ||
      type_name == "uint32" || type_name == "uint64") {
    const auto parsed = std::from_chars(
        argument.encoded_value.data(),
        argument.encoded_value.data() + argument.encoded_value.size(),
        value.uint64_value);
    if (parsed.ec == std::errc{} &&
        parsed.ptr ==
            argument.encoded_value.data() + argument.encoded_value.size()) {
      value.has_uint64_value = true;
      value.payload_kind = SblrValuePayloadKind::unsigned_integer;
      return value;
    }
    value.payload_kind = SblrValuePayloadKind::none;
    return value;
  }
  if (type_name == "boolean" || type_name == "bool") {
    const std::string lowered = projection_value_detail::LowerAscii(argument.encoded_value);
    if (lowered == "true" || lowered == "1") {
      value.int64_value = 1;
      value.has_int64_value = true;
      value.payload_kind = SblrValuePayloadKind::boolean;
      value.descriptor_id = "boolean";
      return value;
    }
    if (lowered == "false" || lowered == "0") {
      value.int64_value = 0;
      value.has_int64_value = true;
      value.payload_kind = SblrValuePayloadKind::boolean;
      value.descriptor_id = "boolean";
      return value;
    }
    value.payload_kind = SblrValuePayloadKind::none;
    return value;
  }
  if (type_name == "real32" || type_name == "real64" ||
      type_name == "real128" || type_name == "double" ||
      type_name == "numeric" || type_name == "decimal" ||
      type_name.rfind("numeric(", 0) == 0 ||
      type_name.rfind("decimal(", 0) == 0) {
    const auto parsed = std::from_chars(
        argument.encoded_value.data(),
        argument.encoded_value.data() + argument.encoded_value.size(),
        value.real64_value,
        std::chars_format::general);
    if (parsed.ec == std::errc{} &&
        parsed.ptr ==
            argument.encoded_value.data() + argument.encoded_value.size() &&
        std::isfinite(value.real64_value)) {
      value.has_real64_value = true;
      value.payload_kind = SblrValuePayloadKind::real64;
      return value;
    }
    value.payload_kind = SblrValuePayloadKind::none;
    return value;
  }
  value.payload_kind = SblrValuePayloadKind::text;
  return value;
}

inline bool ProjectionArgumentEncodingValid(
    const internal_api::EngineProjectionFunctionArgument& argument) {
  if (argument.type_name.empty()) return false;
  const auto type = projection_value_detail::LowerAscii(argument.type_name);
  if (type == "binary" || type == "varbinary" || type == "uuid" || type == "uuid_array") {
    if (argument.state != internal_api::EngineValueState::value &&
        argument.state != internal_api::EngineValueState::sql_null) return false;
    if (!argument.encoded_value.empty()) return false;
    if (argument.is_null || argument.state == internal_api::EngineValueState::sql_null)
      return argument.binary_value.empty();
    // Validate without allocating/copying a secret input merely to inspect it.
    return (type != "uuid" || argument.binary_value.size() == 16) &&
           (type != "uuid_array" || argument.binary_value.size() % 16 == 0);
  }
  const SblrValue value = SblrValueFromProjectionArgument(argument);
  return value.is_null || value.payload_kind != SblrValuePayloadKind::none;
}

inline bool ProjectionSblrValueResolved(const SblrValue& value) {
  if (value.descriptor_id.empty()) return false;
  if (value.is_null && (value.descriptor_id == "binary" ||
      value.descriptor_id == "varbinary" || value.descriptor_id == "uuid" ||
      value.descriptor_id == "uuid_array"))
    return projection_value_detail::EmptyNullCarrier(value);
  if (value.descriptor_id == "uuid_array" ||
      value.payload_kind == SblrValuePayloadKind::uuid_array_binary)
    return SblrUuidArrayPayloadValid(value);
  if (value.payload_kind == SblrValuePayloadKind::binary ||
      value.descriptor_id == "binary" || value.descriptor_id == "varbinary")
    return projection_value_detail::BinaryCarrierValid(value);
  return value.is_null || value.payload_kind != SblrValuePayloadKind::none;
}

inline internal_api::EngineTypedValue EngineTypedValueFromSblrValue(const SblrValue& value) {
  if (!value.is_null && (value.descriptor_id == "binary" || value.descriptor_id == "varbinary") &&
      !projection_value_detail::BinaryCarrierValid(value))
    throw std::invalid_argument("conflicting SBLR binary payload tag");
  if (value.is_null && (value.descriptor_id == "binary" ||
      value.descriptor_id == "varbinary" || value.descriptor_id == "uuid" ||
      value.descriptor_id == "uuid_array") &&
      !projection_value_detail::EmptyNullCarrier(value))
    throw std::invalid_argument("conflicting SBLR NULL payload representations");
  internal_api::EngineTypedValue out;
  out.descriptor.descriptor_kind = "scalar";
  out.descriptor.canonical_type_name = value.descriptor_id;
  out.descriptor.encoded_descriptor = "type=" + out.descriptor.canonical_type_name;
  out.is_null = value.is_null;
  if (out.is_null) {
    out.encoded_value.clear();
    out.binary_value.clear();
    out.setState(internal_api::EngineValueState::sql_null);
    return out;
  }
  out.setState(internal_api::EngineValueState::value);
  if (value.descriptor_id == "uuid_array" ||
      value.payload_kind == SblrValuePayloadKind::uuid_array_binary) {
    if (!CopySblrUuidArrayPayload(value, &out.binary_value))
      throw std::invalid_argument("conflicting SBLR UUID array payload representations");
    out.encoded_value.clear();
    return out;
  }
  if (value.payload_kind == SblrValuePayloadKind::uuid_binary) {
    if (!CopySblrUuidPayload(value, &out.binary_value)) {
      throw std::invalid_argument("conflicting SBLR UUID payload representations");
    }
    // Formatting is a parser/driver render operation, not engine identity.
    out.encoded_value.clear();
    return out;
  }
  if (value.payload_kind == SblrValuePayloadKind::binary) {
    if (!projection_value_detail::BinaryCarrierValid(value))
      throw std::invalid_argument("conflicting SBLR binary payload representations");
    out.binary_value = value.binary_value;
    return out;
  }
  out.encoded_value = !value.encoded_value.empty() ? value.encoded_value : value.text_value;
  if (value.has_int64_value) out.encoded_value = std::to_string(value.int64_value);
  if (value.has_uint64_value) out.encoded_value = std::to_string(value.uint64_value);
  if (value.has_real64_value) out.encoded_value = projection_value_detail::FormatReal64(value.real64_value);
  return out;
}
}  // namespace scratchbird::engine::sblr
