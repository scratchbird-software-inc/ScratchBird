// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
namespace scratchbird::core::platform {
inline constexpr std::array<std::uint8_t,8> kDatabaseStartupNotificationMagic{'S','B','R','E','A','D','Y',1};
inline constexpr std::size_t kDatabaseStartupNotificationBytes=24;
inline auto EncodeDatabaseStartupNotification(std::span<const std::uint8_t,16> database) noexcept {
    std::array<std::uint8_t,kDatabaseStartupNotificationBytes> packet{};
    std::copy(kDatabaseStartupNotificationMagic.begin(),kDatabaseStartupNotificationMagic.end(),packet.begin());
    std::copy(database.begin(),database.end(),packet.begin()+8);return packet;
}
inline bool DatabaseStartupNotificationMatches(std::span<const std::uint8_t,kDatabaseStartupNotificationBytes> packet,
                                               std::span<const std::uint8_t,16> database) noexcept {
    return std::equal(kDatabaseStartupNotificationMagic.begin(),kDatabaseStartupNotificationMagic.end(),packet.begin())&&
           std::equal(database.begin(),database.end(),packet.begin()+8);
}
} // namespace scratchbird::core::platform
