// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "runtime_platform.hpp"
#include "uuid.hpp"
#include <algorithm>
#include <array>
#include <optional>
#include <span>

namespace scratchbird::storage::page {
// Physical indirection only. The containing column owns the logical datatype;
// this locator is neither a canonical datatype value nor a client handle.
struct RowExternalValueLocator {
  core::platform::Uuid object_uuid;
  core::platform::u64 content_checksum = 0;
  core::platform::u64 logical_bytes = 0;
  bool operator==(const RowExternalValueLocator&) const = default;
};
inline constexpr std::array<core::platform::byte, 8> kRowExternalValueMagic =
    {'S','B','M','G','L','V','0','2'};
inline constexpr std::size_t kRowExternalValueBytes = 40;
inline std::optional<RowExternalValueLocator> DecodeRowExternalValueLocator(
    std::span<const core::platform::byte> bytes) noexcept {
  if (bytes.size() != kRowExternalValueBytes ||
      !std::equal(kRowExternalValueMagic.begin(), kRowExternalValueMagic.end(), bytes.begin()))
    return std::nullopt;
  RowExternalValueLocator value;
  std::copy_n(bytes.begin() + 8, 16, value.object_uuid.bytes.begin());
  if (!core::uuid::IsEngineIdentityUuid(value.object_uuid)) return std::nullopt;
  value.content_checksum = core::platform::LoadLittle64(bytes.data() + 24);
  value.logical_bytes = core::platform::LoadLittle64(bytes.data() + 32);
  return value;
}
inline std::optional<std::array<core::platform::byte, kRowExternalValueBytes>>
EncodeRowExternalValueLocator(const RowExternalValueLocator& value) noexcept {
  if (!core::uuid::IsEngineIdentityUuid(value.object_uuid)) return std::nullopt;
  std::array<core::platform::byte, kRowExternalValueBytes> bytes{};
  std::copy(kRowExternalValueMagic.begin(), kRowExternalValueMagic.end(), bytes.begin());
  std::copy(value.object_uuid.bytes.begin(), value.object_uuid.bytes.end(), bytes.begin() + 8);
  core::platform::StoreLittle64(bytes.data() + 24, value.content_checksum);
  core::platform::StoreLittle64(bytes.data() + 32, value.logical_bytes);
  return bytes;
}
} // namespace scratchbird::storage::page
