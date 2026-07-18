// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "pgwire_frame_codec.hpp"

#include <algorithm>
#include <cerrno>

#ifndef _WIN32
#include <unistd.h>
#endif

namespace scratchbird::parser::compatibility::pgwire {
namespace {

#ifndef _WIN32
bool ReadExact(int fd, void* output, std::size_t byte_count) {
  auto* bytes = static_cast<std::uint8_t*>(output);
  std::size_t consumed = 0;
  while (consumed < byte_count) {
    const auto rc = ::read(fd, bytes + consumed, byte_count - consumed);
    if (rc > 0) {
      consumed += static_cast<std::size_t>(rc);
      continue;
    }
    if (rc < 0 && errno == EINTR) continue;
    return false;
  }
  return true;
}

bool WriteAll(int fd, const void* input, std::size_t byte_count) {
  const auto* bytes = static_cast<const std::uint8_t*>(input);
  std::size_t consumed = 0;
  while (consumed < byte_count) {
    const auto rc = ::write(fd, bytes + consumed, byte_count - consumed);
    if (rc > 0) {
      consumed += static_cast<std::size_t>(rc);
      continue;
    }
    if (rc < 0 && errno == EINTR) continue;
    return false;
  }
  return true;
}
#endif

} // namespace

std::uint32_t DecodeBe32(std::span<const std::uint8_t> bytes) {
  if (bytes.size() < 4) return 0;
  return (static_cast<std::uint32_t>(bytes[0]) << 24) |
         (static_cast<std::uint32_t>(bytes[1]) << 16) |
         (static_cast<std::uint32_t>(bytes[2]) << 8) |
         static_cast<std::uint32_t>(bytes[3]);
}

void AppendBe16(std::vector<std::uint8_t>* out, std::uint16_t value) {
  out->push_back(static_cast<std::uint8_t>((value >> 8) & 0xff));
  out->push_back(static_cast<std::uint8_t>(value & 0xff));
}

void AppendBe32(std::vector<std::uint8_t>* out, std::uint32_t value) {
  out->push_back(static_cast<std::uint8_t>((value >> 24) & 0xff));
  out->push_back(static_cast<std::uint8_t>((value >> 16) & 0xff));
  out->push_back(static_cast<std::uint8_t>((value >> 8) & 0xff));
  out->push_back(static_cast<std::uint8_t>(value & 0xff));
}

void AppendCString(std::vector<std::uint8_t>* out, std::string_view value) {
  out->insert(out->end(), value.begin(), value.end());
  out->push_back(0);
}

std::string FirstCString(std::span<const std::uint8_t> bytes) {
  const auto terminator = std::find(bytes.begin(), bytes.end(), 0);
  return std::string(bytes.begin(), terminator);
}

bool ReadStartupFrame(int fd, StartupFrame* frame) {
#ifdef _WIN32
  (void)fd;
  (void)frame;
  return false;
#else
  if (frame == nullptr) return false;
  std::uint8_t length_bytes[4] = {};
  if (!ReadExact(fd, length_bytes, sizeof(length_bytes))) return false;
  const auto length = DecodeBe32(length_bytes);
  if (length < 8 || length > kMaximumFrameBytes) return false;
  frame->payload.assign(length - 4, 0);
  if (!ReadExact(fd, frame->payload.data(), frame->payload.size())) return false;
  frame->request_code = DecodeBe32(frame->payload);
  return true;
#endif
}

bool ReadTypedFrame(int fd, TypedFrame* frame) {
#ifdef _WIN32
  (void)fd;
  (void)frame;
  return false;
#else
  if (frame == nullptr) return false;
  std::uint8_t type = 0;
  std::uint8_t length_bytes[4] = {};
  if (!ReadExact(fd, &type, 1)) return false;
  if (!ReadExact(fd, length_bytes, sizeof(length_bytes))) return false;
  const auto length = DecodeBe32(length_bytes);
  if (length < 4 || length > kMaximumFrameBytes) return false;
  frame->type = static_cast<char>(type);
  frame->body.assign(length - 4, 0);
  return frame->body.empty() || ReadExact(fd, frame->body.data(), frame->body.size());
#endif
}

bool WriteByte(int fd, std::uint8_t value) {
#ifdef _WIN32
  (void)fd;
  (void)value;
  return false;
#else
  return WriteAll(fd, &value, 1);
#endif
}

bool WriteTypedFrame(int fd, char type, std::span<const std::uint8_t> body) {
#ifdef _WIN32
  (void)fd;
  (void)type;
  (void)body;
  return false;
#else
  std::vector<std::uint8_t> frame;
  frame.reserve(5 + body.size());
  frame.push_back(static_cast<std::uint8_t>(type));
  AppendBe32(&frame, static_cast<std::uint32_t>(body.size() + 4));
  frame.insert(frame.end(), body.begin(), body.end());
  return WriteAll(fd, frame.data(), frame.size());
#endif
}

} // namespace scratchbird::parser::compatibility::pgwire
