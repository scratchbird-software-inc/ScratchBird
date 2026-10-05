// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "dml/direct_bulk_scalar_projection.hpp"
#include "crud_support/native_value_payload.hpp"
#include "datatype_operations.hpp"
#include <algorithm>
#include <charconv>
#include <cstring>
#include <iterator>
#include <stdexcept>

namespace scratchbird::engine::internal_api::dml::detail {
// SEARCH_KEY: SB_ENGINE_DIRECT_BULK_SCALAR_PROJECTION_AUTHORITY
// Owns scalar byte projection only; callers retain exact datatype admission.
namespace dt = scratchbird::core::datatypes;

std::uint64_t DirectReadLittleEndianU64(std::span<const std::uint8_t> payload) {
  std::uint64_t value = 0;
  const std::size_t bytes = std::min<std::size_t>(payload.size(), 8);
  for (std::size_t index = 0; index < bytes; ++index) {
    value |= static_cast<std::uint64_t>(payload[index]) << (index * 8u);
  }
  return value;
}

std::uint32_t DirectReadLittleEndianU32(std::span<const std::uint8_t> payload) {
  std::uint32_t value = 0;
  const std::size_t bytes = std::min<std::size_t>(payload.size(), 4);
  for (std::size_t index = 0; index < bytes; ++index) {
    value |= static_cast<std::uint32_t>(payload[index]) << (index * 8u);
  }
  return value;
}

std::string DirectI64ToString(std::int64_t value) {
  char buffer[32] = {};
  const auto [ptr, ec] = std::to_chars(std::begin(buffer),
                                       std::end(buffer),
                                       value);
  if (ec != std::errc()) { return std::to_string(value); }
  return std::string(buffer, ptr);
}

std::string DirectU64ToString(std::uint64_t value) {
  char buffer[32] = {};
  const auto [ptr, ec] = std::to_chars(std::begin(buffer),
                                       std::end(buffer),
                                       value);
  if (ec != std::errc()) { return std::to_string(value); }
  return std::string(buffer, ptr);
}

std::string DirectReal64ToString(double value) {
  char buffer[64] = {};
  const auto [ptr, ec] = std::to_chars(std::begin(buffer),
                                       std::end(buffer),
                                       value);
  if (ec != std::errc()) { return std::to_string(value); }
  return std::string(buffer, ptr);
}

std::string DirectTypedValueTextPayload(const EngineTypedValue& typed) {
  if (!typed.encoded_value.empty() || typed.binary_value.empty()) {
    return typed.encoded_value;
  }
  const auto type = dt::CanonicalTypeIdFromStableName(
      typed.descriptor.canonical_type_name);
  if (type == dt::CanonicalTypeId::boolean && typed.binary_value.size() == 1) {
    return typed.binary_value.front() == 0 ? "false" : "true";
  }
  if (type == dt::CanonicalTypeId::int32 && typed.binary_value.size() == 4) {
    const std::uint32_t bits = DirectReadLittleEndianU32(typed.binary_value);
    std::int32_t value = 0;
    std::memcpy(&value, &bits, sizeof(value));
    return DirectI64ToString(value);
  }
  if (type == dt::CanonicalTypeId::int64 && typed.binary_value.size() == 8) {
    const std::uint64_t bits = DirectReadLittleEndianU64(typed.binary_value);
    std::int64_t value = 0;
    std::memcpy(&value, &bits, sizeof(value));
    return DirectI64ToString(value);
  }
  if (type == dt::CanonicalTypeId::uint64 && typed.binary_value.size() == 8) {
    return DirectU64ToString(DirectReadLittleEndianU64(typed.binary_value));
  }
  if (type == dt::CanonicalTypeId::real64 && typed.binary_value.size() == 8) {
    const std::uint64_t bits = DirectReadLittleEndianU64(typed.binary_value);
    double value = 0.0;
    std::memcpy(&value, &bits, sizeof(value));
    return DirectReal64ToString(value);
  }
  return std::string(reinterpret_cast<const char*>(typed.binary_value.data()),
                     typed.binary_value.size());
}

CrudStoredValue DirectTypedStoredValue(const EngineTypedValue& typed) {
  if (typed.isSqlNull() || typed.state != EngineValueState::value)
    return CrudTypedValuePayload(typed);
  if (!typed.encoded_value.empty() && !typed.binary_value.empty())
    throw std::invalid_argument("ambiguous direct row payload");
  const auto type = dt::CanonicalTypeIdFromStableName(
      typed.descriptor.canonical_type_name);
  if (type == dt::CanonicalTypeId::uint16) {
    throw std::invalid_argument(
        "uint16 direct row carrier profile is unresolved");
  }
  // Retained rows and index admission consume native bytes, not display text.
  // In particular, formatting LE integers here loses their canonical width and
  // makes the persisted carrier disagree with the physical row-page payload.
  return CrudTypedValuePayload(typed);
}

EngineApiU64 DirectTypedValuePayloadSize(const EngineTypedValue& typed) {
  if (typed.isSqlNull()) {
    return 0;
  }
  if (!typed.encoded_value.empty()) {
    return static_cast<EngineApiU64>(typed.encoded_value.size());
  }
  return static_cast<EngineApiU64>(typed.binary_value.size());
}

std::string DirectNativePacketTypeName(std::uint8_t tag) {
  switch (tag) {
    case 1: return "text";
    case 2: return "int64";
    case 3: return "boolean";
    case 4: return "int32";
    case 5: return "uint64";
    case 6: return "real64";
    case 7: return "binary";
    default: return {};
  }
}

std::vector<scratchbird::core::platform::byte> DirectBigEndianBytes(
    const std::vector<scratchbird::core::platform::byte>& little_endian) {
  std::vector<scratchbird::core::platform::byte> out = little_endian;
  std::reverse(out.begin(), out.end());
  return out;
}

std::vector<scratchbird::core::platform::byte> DirectLittleEndianBytes32(
    std::uint32_t value) {
  std::vector<scratchbird::core::platform::byte> out;
  out.reserve(4);
  for (unsigned shift = 0; shift < 32; shift += 8) {
    out.push_back(static_cast<scratchbird::core::platform::byte>(
        (value >> shift) & 0xffu));
  }
  return out;
}

std::vector<scratchbird::core::platform::byte> DirectLittleEndianBytes64(
    std::uint64_t value) {
  std::vector<scratchbird::core::platform::byte> out;
  out.reserve(8);
  for (unsigned shift = 0; shift < 64; shift += 8) {
    out.push_back(static_cast<scratchbird::core::platform::byte>(
        (value >> shift) & 0xffu));
  }
  return out;
}

}  // namespace scratchbird::engine::internal_api::dml::detail
