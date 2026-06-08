// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SBSQL source-layout fragment. Included only by sblr_special_forms.cpp.
// This fragment preserves pre-existing behavior; do not add unrelated future work here.
}

datatypes::CanonicalTypeId InferSblrSourceType(const SblrValue& value) {
  const auto descriptor_type = CanonicalTypeFromDescriptor(value.descriptor_id);
  if (descriptor_type != datatypes::CanonicalTypeId::unknown) return descriptor_type;
  switch (value.payload_kind) {
    case SblrValuePayloadKind::boolean:
      return datatypes::CanonicalTypeId::boolean;
    case SblrValuePayloadKind::signed_integer:
      return datatypes::CanonicalTypeId::int64;
    case SblrValuePayloadKind::unsigned_integer:
      return datatypes::CanonicalTypeId::uint64;
    case SblrValuePayloadKind::real64:
      return datatypes::CanonicalTypeId::real64;
    case SblrValuePayloadKind::high_precision_numeric_text:
      return datatypes::CanonicalTypeId::decimal;
    case SblrValuePayloadKind::text:
      return datatypes::CanonicalTypeId::character;
    case SblrValuePayloadKind::binary:
      return datatypes::CanonicalTypeId::binary;
    case SblrValuePayloadKind::uuid_text:
      return datatypes::CanonicalTypeId::uuid;
    case SblrValuePayloadKind::temporal_text:
      return datatypes::CanonicalTypeId::timestamp;
    case SblrValuePayloadKind::descriptor_payload:
    case SblrValuePayloadKind::none:
      return datatypes::CanonicalTypeId::unknown;
  }
  return datatypes::CanonicalTypeId::unknown;
}

std::string EncodedCastInput(const SblrValue& value) {
  if (value.is_null) return {};
  if (!value.encoded_value.empty()) return value.encoded_value;
  if (value.payload_kind == SblrValuePayloadKind::binary) {
    return std::string(value.binary_value.begin(), value.binary_value.end());
  }
  if (value.has_int64_value) return std::to_string(value.int64_value);
  if (value.has_uint64_value) return std::to_string(value.uint64_value);
  if (value.has_real64_value) {
    std::ostringstream out;
    out.precision(std::numeric_limits<double>::max_digits10);
    out << value.real64_value;
    return out.str();
  }
  return value.text_value;
}

bool ParseSignedInt64(std::string_view text, std::int64_t* out) {
  try {
    std::size_t consumed = 0;
    const auto parsed = std::stoll(std::string(text), &consumed, 10);
    if (consumed != text.size()) return false;
    *out = static_cast<std::int64_t>(parsed);
    return true;
  } catch (...) {
    return false;
  }
}

bool ParseUnsignedInt64(std::string_view text, std::uint64_t* out) {
  try {
    std::size_t consumed = 0;
    const auto parsed = std::stoull(std::string(text), &consumed, 10);
    if (consumed != text.size()) return false;
    *out = static_cast<std::uint64_t>(parsed);
    return true;
  } catch (...) {
    return false;
  }
}

bool ParseReal64(std::string_view text, double* out) {
  try {
    std::size_t consumed = 0;
    const auto parsed = std::stod(std::string(text), &consumed);
    if (consumed != text.size()) return false;
    *out = parsed;
    return true;
  } catch (...) {
    return false;
  }
}

SblrValue SblrValueFromDatatypeValue(const datatypes::DatatypeOperationValue& value) {
  SblrValue out;
  out.descriptor_id = datatypes::CanonicalTypeName(value.type_id);
  out.is_null = value.is_null;
  out.encoded_value = value.encoded_value;
  if (value.is_null) {
    out.payload_kind = SblrValuePayloadKind::none;
    return out;
  }

  switch (value.type_id) {
    case datatypes::CanonicalTypeId::boolean:
      out.payload_kind = SblrValuePayloadKind::boolean;
      out.has_int64_value = true;
      out.int64_value = (Lower(value.encoded_value) == "true" || value.encoded_value == "1") ? 1 : 0;
      out.text_value = out.int64_value == 1 ? "TRUE" : "FALSE";
      return out;
    case datatypes::CanonicalTypeId::int8:
    case datatypes::CanonicalTypeId::int16:
    case datatypes::CanonicalTypeId::int32:
    case datatypes::CanonicalTypeId::int64: {
      out.payload_kind = SblrValuePayloadKind::signed_integer;
      std::int64_t parsed = 0;
      if (ParseSignedInt64(value.encoded_value, &parsed)) {
        out.has_int64_value = true;
        out.int64_value = parsed;
      }
      out.text_value = value.encoded_value;
      return out;
    }
