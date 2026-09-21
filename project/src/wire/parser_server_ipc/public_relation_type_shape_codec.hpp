// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "../../core/datatypes/canonical_utf8.hpp"
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace scratchbird::parser::ipc {

// Descriptor shape only. Object/type/collation identities and nullable state
// belong to the surrounding binary projection, not this record or a display.
struct PublicRelationTypeShapeV1 {
  std::optional<std::uint32_t> width, precision, scale;
  std::optional<std::string> timezone_profile_id;
  bool operator==(const PublicRelationTypeShapeV1&) const = default;
};

inline constexpr std::size_t kPublicRelationTypeShapePrefixBytes = 16;
inline constexpr std::size_t kPublicRelationTimezoneProfileMaximumBytes = 4096;
inline constexpr std::size_t kPublicRelationTypeShapeMaximumBytes = 4116;

namespace relation_shape_detail {
inline bool Timezone(std::span<const std::uint8_t> bytes) noexcept {
  if (bytes.empty() || bytes.size() > kPublicRelationTimezoneProfileMaximumBytes)
    return false;
  for (auto value : bytes) if (value == 0) return false;
  return core::datatypes::ValidateCanonicalUtf8(bytes.data(), bytes.size());
}
inline std::uint32_t Read32(std::span<const std::uint8_t> bytes,
                            std::size_t offset) noexcept {
  return std::uint32_t(bytes[offset]) |
      (std::uint32_t(bytes[offset + 1]) << 8) |
      (std::uint32_t(bytes[offset + 2]) << 16) |
      (std::uint32_t(bytes[offset + 3]) << 24);
}
inline void Write32(std::vector<std::uint8_t>& bytes, std::size_t offset,
                    std::uint32_t value) noexcept {
  for (unsigned i = 0; i != 4; ++i)
    bytes[offset + i] = static_cast<std::uint8_t>(value >> (8 * i));
}
} // namespace relation_shape_detail

inline bool EncodePublicRelationTypeShapeV1(
    const PublicRelationTypeShapeV1& shape, std::size_t maximum_bytes,
    std::vector<std::uint8_t>* output) {
  namespace d = relation_shape_detail;
  if (!output || maximum_bytes < kPublicRelationTypeShapePrefixBytes) return false;
  std::size_t total = kPublicRelationTypeShapePrefixBytes;
  if (shape.timezone_profile_id) {
    const auto& timezone = *shape.timezone_profile_id;
    if (!d::Timezone({reinterpret_cast<const std::uint8_t*>(timezone.data()),
                      timezone.size()})) return false;
    total += 4 + timezone.size(); // bounded to 4116, no size_t overflow
  }
  if (total > maximum_bytes) return false;
  std::vector<std::uint8_t> staged(total, 0);
  staged[0] = 1;
  const std::optional<std::uint32_t>* fields[] = {
      &shape.width, &shape.precision, &shape.scale};
  for (unsigned i = 0; i != 3; ++i) {
    if (*fields[i]) {
      staged[2] |= static_cast<std::uint8_t>(1u << i);
      d::Write32(staged, 4 + 4 * i, **fields[i]);
    }
  }
  if (shape.timezone_profile_id) {
    staged[2] |= 8;
    const auto& timezone = *shape.timezone_profile_id;
    d::Write32(staged, 16, static_cast<std::uint32_t>(timezone.size()));
    for (std::size_t i = 0; i != timezone.size(); ++i)
      staged[20 + i] = static_cast<std::uint8_t>(timezone[i]);
  }
  *output = std::move(staged);
  return true;
}

inline bool ValidatePublicRelationTypeShapeV1(
    std::span<const std::uint8_t> bytes, std::size_t maximum_bytes) noexcept {
  namespace d = relation_shape_detail;
  if (bytes.size() < kPublicRelationTypeShapePrefixBytes ||
      bytes.size() > maximum_bytes ||
      bytes.size() > kPublicRelationTypeShapeMaximumBytes ||
      bytes[0] != 1 || bytes[1] != 0 || bytes[3] != 0 ||
      (bytes[2] & 0xf0u) != 0) return false;
  const auto flags = bytes[2];
  for (unsigned i = 0; i != 3; ++i)
    if (!(flags & (1u << i)) && d::Read32(bytes, 4 + 4 * i) != 0)
      return false;
  if (flags & 8) {
    if (bytes.size() < 20 || d::Read32(bytes, 16) != bytes.size() - 20 ||
        !d::Timezone(bytes.subspan(20))) return false;
  } else if (bytes.size() != kPublicRelationTypeShapePrefixBytes) {
    return false;
  }
  return true;
}

inline bool DecodePublicRelationTypeShapeV1(
    std::span<const std::uint8_t> bytes, std::size_t maximum_bytes,
    PublicRelationTypeShapeV1* output) {
  namespace d = relation_shape_detail;
  if (!output || !ValidatePublicRelationTypeShapeV1(bytes, maximum_bytes)) return false;
  const auto flags = bytes[2];
  // Finish structural validation before allocation or caller publication.
  PublicRelationTypeShapeV1 staged;
  std::optional<std::uint32_t>* fields[] = {
      &staged.width, &staged.precision, &staged.scale};
  for (unsigned i = 0; i != 3; ++i)
    if (flags & (1u << i)) *fields[i] = d::Read32(bytes, 4 + 4 * i);
  if (flags & 8)
    staged.timezone_profile_id.emplace(
        reinterpret_cast<const char*>(bytes.data() + 20), bytes.size() - 20);
  static_assert(std::is_nothrow_move_assignable_v<PublicRelationTypeShapeV1>);
  *output = std::move(staged);
  return true;
}
} // namespace scratchbird::parser::ipc
