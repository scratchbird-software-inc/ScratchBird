// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "index_key_encoding.hpp"
#include <algorithm>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace scratchbird::engine::internal_api::dml::detail {
// Canonical binary scalar-key codec. Publication, visibility, and constraint
// ownership stay with the bulk coordinator; this helper only encodes bytes.
using core::platform::TypedUuid;

inline constexpr std::string_view kDirectSbkoBinaryPrefix = "SBKOBIN:";

inline bool DirectEncodedScalarKeyIsNull(std::string_view key) {
  // The producer's explicit canonical NULL rank, never a payload substring.
  constexpr std::size_t rank_offset = kDirectSbkoBinaryPrefix.size() + 4;
  if (key.size() < kDirectSbkoBinaryPrefix.size() + 35 ||
      !key.starts_with("SBKOBIN:SBKO")) return false;
  const auto rank = static_cast<unsigned char>(key[rank_offset]);
  return rank == 0x00 || rank == 0xff;
}

inline int DirectCompareUnsignedText(std::string_view left, std::string_view right) {
  const auto count = std::min(left.size(), right.size());
  for (std::size_t i = 0; i < count; ++i) {
    const auto l = static_cast<unsigned char>(left[i]);
    const auto r = static_cast<unsigned char>(right[i]);
    if (l < r) {
      return -1;
    }
    if (l > r) {
      return 1;
    }
  }
  return left.size() < right.size() ? -1 : (right.size() < left.size() ? 1 : 0);
}

inline void DirectAppendBinaryByte(std::string* out, unsigned char value) {
  if (out == nullptr) {
    return;
  }
  out->push_back(static_cast<char>(value));
}

inline void DirectAppendBinaryU32(std::string* out, std::uint32_t value) {
  for (int shift = 24; shift >= 0; shift -= 8) {
    DirectAppendBinaryByte(out, static_cast<unsigned char>((value >> shift) & 0xffu));
  }
}

inline void DirectAppendBinaryU64(std::string* out, std::uint64_t value) {
  for (int shift = 56; shift >= 0; shift -= 8) {
    DirectAppendBinaryByte(out, static_cast<unsigned char>((value >> shift) & 0xffu));
  }
}

inline void DirectAppendBinaryTypedUuid(std::string* out, const TypedUuid& uuid) {
  DirectAppendBinaryByte(out, static_cast<unsigned char>(uuid.kind));
  for (const auto value : uuid.value.bytes) {
    DirectAppendBinaryByte(out, static_cast<unsigned char>(value));
  }
}

inline void DirectAppendEscapedPayloadBinary(
    std::string* out,
    std::span<const scratchbird::core::platform::byte> payload) {
  for (const auto value : payload) {
    const auto byte_value = static_cast<unsigned char>(value);
    DirectAppendBinaryByte(out, byte_value);
    if (byte_value == 0x00) {
      DirectAppendBinaryByte(out, 0xff);
    }
  }
  DirectAppendBinaryByte(out, 0x00);
  DirectAppendBinaryByte(out, 0x00);
}

inline bool DirectEncodeSingleComponentIndexKeyBinary(
    scratchbird::core::index::IndexKeyComponentKind kind,
    const TypedUuid& type_descriptor_uuid,
    bool is_null,
    std::span<const scratchbird::core::platform::byte> sortable_payload,
    std::string* encoded_key) {
  if (encoded_key == nullptr || !type_descriptor_uuid.valid()) {
    return false;
  }
  encoded_key->clear();
  encoded_key->reserve(kDirectSbkoBinaryPrefix.size() + 96 +
                       sortable_payload.size() * 2);
  encoded_key->append(kDirectSbkoBinaryPrefix);
  DirectAppendBinaryByte(encoded_key, 'S');
  DirectAppendBinaryByte(encoded_key, 'B');
  DirectAppendBinaryByte(encoded_key, 'K');
  DirectAppendBinaryByte(encoded_key, 'O');
  DirectAppendBinaryByte(encoded_key, is_null ? 0x00 : 0x7f);
  DirectAppendBinaryU32(encoded_key, static_cast<std::uint32_t>(kind));
  DirectAppendBinaryTypedUuid(encoded_key, type_descriptor_uuid);
  DirectAppendBinaryU64(encoded_key, 1);
  DirectAppendBinaryByte(encoded_key, 0);
  if (!is_null) {
    DirectAppendEscapedPayloadBinary(encoded_key, sortable_payload);
  }
  return true;
}

inline std::string DirectSingleComponentIndexKeyPrefixBinary(
    scratchbird::core::index::IndexKeyComponentKind kind,
    const TypedUuid& type_descriptor_uuid) {
  if (!type_descriptor_uuid.valid()) {
    return {};
  }
  std::string out;
  out.reserve(kDirectSbkoBinaryPrefix.size() + 96);
  out.append(kDirectSbkoBinaryPrefix);
  DirectAppendBinaryByte(&out, 'S');
  DirectAppendBinaryByte(&out, 'B');
  DirectAppendBinaryByte(&out, 'K');
  DirectAppendBinaryByte(&out, 'O');
  DirectAppendBinaryByte(&out, 0x7f);
  DirectAppendBinaryU32(&out, static_cast<std::uint32_t>(kind));
  DirectAppendBinaryTypedUuid(&out, type_descriptor_uuid);
  DirectAppendBinaryU64(&out, 1);
  DirectAppendBinaryByte(&out, 0);
  return out;
}

inline bool DirectEncodeSingleComponentIndexKeyBinaryFromPrefix(
    std::string_view non_null_prefix,
    bool is_null,
    std::span<const scratchbird::core::platform::byte> sortable_payload,
    std::string* encoded_key) {
  if (encoded_key == nullptr || is_null || non_null_prefix.empty()) {
    return false;
  }
  encoded_key->clear();
  encoded_key->reserve(non_null_prefix.size() + sortable_payload.size() * 2 + 4);
  encoded_key->append(non_null_prefix);
  DirectAppendEscapedPayloadBinary(encoded_key, sortable_payload);
  return true;
}

inline bool DirectEncodeSingleComponentIndexKeyBinaryFromPrefixInline(
    std::string_view non_null_prefix,
    std::span<const scratchbird::core::platform::byte> sortable_payload,
    std::string* encoded_key) {
  if (encoded_key == nullptr || non_null_prefix.empty()) {
    return false;
  }
  encoded_key->clear();
  encoded_key->reserve(non_null_prefix.size() + sortable_payload.size() * 2 + 4);
  encoded_key->append(non_null_prefix);
  DirectAppendEscapedPayloadBinary(encoded_key, sortable_payload);
  return true;
}

inline bool DirectSbkoBinaryPayload(std::string_view key, std::string* payload) {
  if (payload == nullptr || key.rfind(kDirectSbkoBinaryPrefix, 0) != 0) {
    return false;
  }
  *payload = std::string(key.substr(kDirectSbkoBinaryPrefix.size()));
  return true;
}

inline int DirectCompareEncodedIndexKey(std::string_view left,
                                 std::string_view right) {
  if (left.rfind(kDirectSbkoBinaryPrefix, 0) == 0 &&
      right.rfind(kDirectSbkoBinaryPrefix, 0) == 0) {
    return DirectCompareUnsignedText(
        left.substr(kDirectSbkoBinaryPrefix.size()),
        right.substr(kDirectSbkoBinaryPrefix.size()));
  }
  if (scratchbird::core::index::IsOrderPreservingIndexKeyEncoding(left) &&
      scratchbird::core::index::IsOrderPreservingIndexKeyEncoding(right)) {
    const auto compare =
        scratchbird::core::index::CompareEncodedIndexKeyBytes(left, right);
    if (compare.ok()) {
      return compare.comparison;
    }
  }
  std::string left_sbkobin;
  std::string right_sbkobin;
  if (DirectSbkoBinaryPayload(left, &left_sbkobin) &&
      DirectSbkoBinaryPayload(right, &right_sbkobin)) {
    return DirectCompareUnsignedText(left_sbkobin, right_sbkobin);
  }
  return DirectCompareUnsignedText(left, right);
}

}  // namespace scratchbird::engine::internal_api::dml::detail
