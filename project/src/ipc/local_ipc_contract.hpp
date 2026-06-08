// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace scratchbird::ipc {

inline constexpr std::uint32_t kLocalIpcMagic = 0x50494253u;  // "SBIP"
inline constexpr std::uint16_t kLocalIpcMajor = 1;
inline constexpr std::uint16_t kLocalIpcMinor = 0;
inline constexpr std::uint16_t kLocalIpcHeaderLength = 88;
inline constexpr std::uint32_t kLocalIpcMaxPayloadBytes = 16u * 1024u * 1024u;

enum class LocalIpcMessageType : std::uint16_t {
  kHello = 0x0001,
  kHelloAck = 0x0002,
  kAuthHandoff = 0x0010,
  kSessionReady = 0x0011,
  kRouteMessage = 0x0020,
  kDisconnect = 0x0021,
  kHeartbeat = 0x0030,
  kDiagnostic = 0x0040,
};

struct LocalIpcHeader {
  std::uint32_t magic{kLocalIpcMagic};
  std::uint16_t major{kLocalIpcMajor};
  std::uint16_t minor{kLocalIpcMinor};
  std::uint16_t header_length{kLocalIpcHeaderLength};
  LocalIpcMessageType message_type{LocalIpcMessageType::kHello};
  std::uint32_t flags{0};
  std::uint64_t payload_length{0};
  std::uint32_t payload_crc32c{0};
  std::uint32_t header_crc32c{0};
  std::array<std::uint8_t, 16> correlation_uuid{};
  std::array<std::uint8_t, 16> session_uuid{};
  std::array<std::uint8_t, 16> transaction_uuid{};
};

struct LocalIpcFrame {
  LocalIpcHeader header;
  std::vector<std::uint8_t> payload;
};

struct LocalIpcDecodeResult {
  bool ok{false};
  std::string diagnostic_code;
  std::string diagnostic_message;
  LocalIpcFrame frame;
};

bool LocalIpcVersionCompatible(std::uint16_t major, std::uint16_t minor);
std::uint32_t LocalIpcCrc32c(const std::uint8_t* data, std::size_t size);
std::vector<std::uint8_t> EncodeLocalIpcFrame(const LocalIpcFrame& frame);
LocalIpcDecodeResult DecodeLocalIpcFrame(const std::vector<std::uint8_t>& bytes);
LocalIpcFrame MakeLocalIpcHelloFrame(std::array<std::uint8_t, 16> correlation_uuid,
                                     std::string profile,
                                     std::string capability_set);

}  // namespace scratchbird::ipc
