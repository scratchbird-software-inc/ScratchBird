// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace scratchbird::parser::compatibility::mywire {

// Packet framing only. Parser dispatch, authentication policy, session state,
// binding, lowering, result shape, and diagnostics remain family-owned.
constexpr std::size_t kMaximumPayloadBytes = 16U * 1024U * 1024U;

struct Packet {
  std::uint8_t sequence{0};
  std::vector<std::uint8_t> payload;
};

bool ReadPacket(int fd, Packet* packet);
bool WritePacket(int fd, std::uint8_t sequence,
                 const std::vector<std::uint8_t>& payload);

std::uint32_t DecodeU32(const std::uint8_t* bytes, std::size_t byte_count);
void AppendU16(std::vector<std::uint8_t>* out, std::uint16_t value);
void AppendU24(std::vector<std::uint8_t>* out, std::uint32_t value);
void AppendU32(std::vector<std::uint8_t>* out, std::uint32_t value);
void AppendNullString(std::vector<std::uint8_t>* out, std::string_view value);
void AppendLengthEncodedInteger(std::vector<std::uint8_t>* out,
                                std::uint64_t value);
void AppendLengthEncodedString(std::vector<std::uint8_t>* out,
                               std::string_view value);

} // namespace scratchbird::parser::compatibility::mywire
