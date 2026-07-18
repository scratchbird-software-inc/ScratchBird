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
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace scratchbird::parser::compatibility::pgwire {

// This module owns only wire-v3 byte framing. It intentionally has no parser,
// dialect, binding, lowering, session, result-shape, or diagnostic policy.
constexpr std::uint32_t kProtocolV3 = 196608;
constexpr std::uint32_t kSslRequest = 80877103;
constexpr std::uint32_t kGssEncryptionRequest = 80877104;
constexpr std::uint32_t kCancelRequest = 80877102;
constexpr std::size_t kMaximumFrameBytes = 16U * 1024U * 1024U;

struct StartupFrame {
  std::uint32_t request_code{0};
  std::vector<std::uint8_t> payload;
};

struct TypedFrame {
  char type{0};
  std::vector<std::uint8_t> body;
};

bool ReadStartupFrame(int fd, StartupFrame* frame);
bool ReadTypedFrame(int fd, TypedFrame* frame);
bool WriteByte(int fd, std::uint8_t value);
bool WriteTypedFrame(int fd, char type, std::span<const std::uint8_t> body);

std::uint32_t DecodeBe32(std::span<const std::uint8_t> bytes);
void AppendBe16(std::vector<std::uint8_t>* out, std::uint16_t value);
void AppendBe32(std::vector<std::uint8_t>* out, std::uint32_t value);
void AppendCString(std::vector<std::uint8_t>* out, std::string_view value);
std::string FirstCString(std::span<const std::uint8_t> bytes);

} // namespace scratchbird::parser::compatibility::pgwire
