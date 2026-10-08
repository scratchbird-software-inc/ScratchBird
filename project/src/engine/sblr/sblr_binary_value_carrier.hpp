// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "sblr_runtime.hpp"

namespace scratchbird::engine::sblr {
// Native scalar payload validation only; binding admission is separate.
inline bool SblrInt64PayloadValid(const SblrValue& value) noexcept {
  return !value.is_null && value.descriptor_id == "int64" &&
      value.payload_kind == SblrValuePayloadKind::signed_integer &&
      value.has_int64_value && !value.has_uint64_value && !value.has_real64_value &&
      value.encoded_value.empty() && value.text_value.empty() && value.binary_value.empty() &&
      value.uuid_value.is_nil() && value.uuid_array_value.empty() &&
      value.charset_name.empty() && value.collation_name.empty();
}
// Representation validation only. UUID data has no system-identity version or
// variant restriction; descriptor/permission admission belongs to the caller.
inline bool SblrUuidPayloadValid(const SblrValue& value) noexcept {
  return !value.is_null && value.descriptor_id == "uuid" &&
      value.payload_kind == SblrValuePayloadKind::uuid_binary &&
      value.text_value.empty() && value.encoded_value.empty() &&
      value.binary_value.empty() && value.uuid_array_value.empty() &&
      value.charset_name.empty() && value.collation_name.empty() &&
      !value.has_int64_value && !value.has_uint64_value && !value.has_real64_value;
}
inline bool SblrBinaryPayloadValid(const SblrValue& value) noexcept {
  return !value.is_null && value.payload_kind == SblrValuePayloadKind::binary &&
      (value.descriptor_id == "binary" || value.descriptor_id == "varbinary") &&
      value.text_value.empty() && value.encoded_value.empty() &&
      value.charset_name.empty() && value.collation_name.empty() &&
      value.uuid_value.is_nil() && value.uuid_array_value.empty() &&
      !value.has_int64_value && !value.has_uint64_value && !value.has_real64_value;
}
inline bool SblrNullPayloadEmpty(const SblrValue& value) noexcept {
  return value.is_null && value.payload_kind == SblrValuePayloadKind::none &&
      value.binary_value.empty() && value.text_value.empty() && value.encoded_value.empty() &&
      value.uuid_value.is_nil() && value.uuid_array_value.empty() &&
      !value.has_int64_value && !value.has_uint64_value && !value.has_real64_value &&
      value.charset_name.empty() && value.collation_name.empty();
}
inline bool SblrNativeCastCarrierValid(const SblrValue& value) noexcept {
  if (value.descriptor_id == "uint16") return false;
  if (value.is_null) return SblrNullPayloadEmpty(value);
  if (value.descriptor_id == "uuid" || value.payload_kind == SblrValuePayloadKind::uuid_binary ||
      value.payload_kind == SblrValuePayloadKind::uuid_text) return SblrUuidPayloadValid(value);
  if (value.descriptor_id == "binary" || value.descriptor_id == "varbinary" ||
      value.payload_kind == SblrValuePayloadKind::binary) return SblrBinaryPayloadValid(value);
  return value.uuid_value.is_nil() && value.uuid_array_value.empty() && value.binary_value.empty();
}
inline bool SblrHasNativeBinaryCarrier(const SblrValue& value) noexcept {
  return value.descriptor_id == "uint16" || value.descriptor_id == "uuid" ||
      value.descriptor_id == "binary" ||
      value.descriptor_id == "varbinary" || value.payload_kind == SblrValuePayloadKind::uuid_binary ||
      value.payload_kind == SblrValuePayloadKind::uuid_text || value.payload_kind == SblrValuePayloadKind::binary;
}
struct SblrNativeBinaryComparison {
  bool attempted = false;
  bool valid = false;
  int order = 0;
};
inline SblrNativeBinaryComparison CompareSblrNativeBinaryValues(
    const SblrValue& left, const SblrValue& right) noexcept {
  if (!SblrHasNativeBinaryCarrier(left) && !SblrHasNativeBinaryCarrier(right)) return {};
  if (!SblrNativeCastCarrierValid(left) || !SblrNativeCastCarrierValid(right)) return {true, false, 0};
  // The caller applies SQL three-valued NULL semantics, not this byte comparator.
  if (left.is_null || right.is_null) return {true, true, 0};
  if (SblrUuidPayloadValid(left) && SblrUuidPayloadValid(right))
    return {true, true, left.uuid_value.bytes < right.uuid_value.bytes ? -1 :
        left.uuid_value.bytes > right.uuid_value.bytes ? 1 : 0};
  if (SblrBinaryPayloadValid(left) && SblrBinaryPayloadValid(right))
    return {true, true, left.binary_value < right.binary_value ? -1 :
        left.binary_value > right.binary_value ? 1 : 0};
  // UUID versus binary requires an explicit cast. Never compare UUID text or
  // silently fall back to the empty text carriers of a native value.
  return {true, false, 0};
}
}  // namespace scratchbird::engine::sblr
