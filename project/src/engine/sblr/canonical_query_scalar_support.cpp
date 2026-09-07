// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "canonical_query_scalar_support.hpp"

#include "engine/executor/descriptor_value_runtime.hpp"

#include <array>
#include <bit>
#include <charconv>
#include <cctype>
#include <iomanip>
#include <limits>
#include <sstream>

namespace scratchbird::engine::sblr {
namespace api = scratchbird::engine::internal_api;
namespace exec = scratchbird::engine::executor;

// SEARCH_KEY: SB_ENGINE_CANONICAL_QUERY_SCALAR_SUPPORT_AUTHORITY
bool CanonicalUuidText(const std::string_view value) {
  if (value.size() != 36 || value[8] != '-' || value[13] != '-' ||
      value[18] != '-' || value[23] != '-' ||
      value == "00000000-0000-0000-0000-000000000000") {
    return false;
  }
  for (std::size_t index = 0; index < value.size(); ++index) {
    if (index == 8 || index == 13 || index == 18 || index == 23) continue;
    const auto ch = static_cast<unsigned char>(value[index]);
    if (!std::isxdigit(ch) || std::isupper(ch)) return false;
  }
  return true;
}

namespace {

std::uint64_t Fnv1a64(const std::string_view value,
                      std::uint64_t hash = 14695981039346656037ull) {
  for (const auto byte : value) {
    hash ^= static_cast<std::uint8_t>(byte);
    hash *= 1099511628211ull;
  }
  return hash;
}

}  // namespace

std::string DerivedCanonicalUuid(const std::string_view scope,
                                 const std::string_view purpose) {
  const auto first = Fnv1a64(purpose, Fnv1a64(scope));
  const auto second = Fnv1a64(scope, Fnv1a64(purpose));
  std::array<std::uint8_t, 16> bytes{};
  for (std::size_t index = 0; index < 8; ++index) {
    bytes[index] = static_cast<std::uint8_t>(first >> ((7 - index) * 8));
    bytes[8 + index] =
        static_cast<std::uint8_t>(second >> ((7 - index) * 8));
  }
  bytes[6] = static_cast<std::uint8_t>((bytes[6] & 0x0f) | 0x50);
  bytes[8] = static_cast<std::uint8_t>((bytes[8] & 0x3f) | 0x80);
  std::ostringstream out;
  out << std::hex << std::setfill('0');
  for (std::size_t index = 0; index < bytes.size(); ++index) {
    if (index == 4 || index == 6 || index == 8 || index == 10) out << '-';
    out << std::setw(2) << static_cast<unsigned>(bytes[index]);
  }
  return out.str();
}

bool EncodeCanonicalScalarEqualityKey(const api::EngineTypedValue& value,
                                      std::string* key,
                                      std::string* refusal_detail) {
  if (key == nullptr || refusal_detail == nullptr) return false;
  key->clear();
  refusal_detail->clear();
  if (value.state != api::EngineValueState::value || value.is_null) {
    *refusal_detail =
        "aggregate DISTINCT received a non-value scalar payload";
    return false;
  }
  const auto append = [key](const std::string_view field) {
    const auto size = static_cast<std::uint64_t>(field.size());
    for (unsigned shift = 0; shift < 64; shift += 8) {
      key->push_back(static_cast<char>(size >> shift));
    }
    key->append(field);
  };
  append(value.descriptor.canonical_type_name);
  std::string canonical_payload = value.encoded_value;
  std::string_view auxiliary_binary;
  const auto& type = value.descriptor.canonical_type_name;
  if (type == "int8" || type == "int16" || type == "int32" ||
      type == "int64") {
    const auto decoded = exec::DecodeInt64Value(value);
    if (!decoded.ok()) {
      *refusal_detail = decoded.diagnostic.detail;
      return false;
    }
    canonical_payload = std::to_string(decoded.value);
  } else if (type == "boolean" || type == "bool") {
    const auto decoded = exec::DecodeBoolValue(value);
    if (!decoded.ok()) {
      *refusal_detail = decoded.diagnostic.detail;
      return false;
    }
    canonical_payload = decoded.value ? "true" : "false";
  } else if (type == "real64" || type == "double" ||
             type == "double precision") {
    const auto decoded = exec::DecodeReal64Value(value);
    if (!decoded.ok()) {
      *refusal_detail = decoded.diagnostic.detail;
      return false;
    }
    if (decoded.value == 0.0) {
      canonical_payload = "0";
    } else {
      std::array<char, 128> buffer{};
      const auto encoded = std::to_chars(
          buffer.data(), buffer.data() + buffer.size(), decoded.value,
          std::chars_format::general,
          std::numeric_limits<double>::max_digits10);
      if (encoded.ec != std::errc{}) {
        *refusal_detail = "aggregate DISTINCT real64 key encoding failed";
        return false;
      }
      canonical_payload.assign(buffer.data(), encoded.ptr);
    }
  } else if ((type == "binary" || type == "blob" || type == "bytes") &&
             !value.binary_value.empty()) {
    canonical_payload.assign(
        reinterpret_cast<const char*>(value.binary_value.data()),
        value.binary_value.size());
  } else if (!value.binary_value.empty()) {
    auxiliary_binary = std::string_view(
        reinterpret_cast<const char*>(value.binary_value.data()),
        value.binary_value.size());
  }
  append(canonical_payload);
  append(auxiliary_binary);
  return true;
}

bool DecodeCanonicalInt64Scalar(const api::EngineTypedValue& value,
                                std::int64_t* decoded,
                                std::string* refusal_detail) {
  if (decoded == nullptr || refusal_detail == nullptr) return false;
  const bool textual = !value.encoded_value.empty();
  const bool binary = !value.binary_value.empty();
  if (value.state != api::EngineValueState::value || value.is_null ||
      value.descriptor.canonical_type_name != "int64" || textual == binary) {
    *refusal_detail = "scalar is not a non-NULL canonical int64 value";
    return false;
  }
  if (textual) {
    const auto [end, error] = std::from_chars(
        value.encoded_value.data(),
        value.encoded_value.data() + value.encoded_value.size(), *decoded);
    if (error != std::errc{} ||
        end != value.encoded_value.data() + value.encoded_value.size()) {
      *refusal_detail = "scalar is outside exact int64 admission";
      return false;
    }
  } else {
    if (value.binary_value.size() != sizeof(std::int64_t)) {
      *refusal_detail = "scalar binary int64 payload is malformed";
      return false;
    }
    std::uint64_t encoded = 0;
    for (std::size_t index = 0; index < sizeof(encoded); ++index) {
      encoded |= static_cast<std::uint64_t>(value.binary_value[index])
                 << (index * 8);
    }
    *decoded = std::bit_cast<std::int64_t>(encoded);
  }
  return true;
}

}  // namespace scratchbird::engine::sblr
