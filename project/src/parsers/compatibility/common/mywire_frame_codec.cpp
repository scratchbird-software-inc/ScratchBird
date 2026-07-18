// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "mywire_frame_codec.hpp"

#include <cerrno>

#ifndef _WIN32
#include <unistd.h>
#endif

namespace scratchbird::parser::compatibility::mywire {
namespace {

#ifndef _WIN32
bool ReadExact(int fd, void* output, std::size_t byte_count) {
  auto* bytes = static_cast<std::uint8_t*>(output);
  std::size_t offset = 0;
  while (offset < byte_count) {
    const auto rc = ::read(fd, bytes + offset, byte_count - offset);
    if (rc > 0) {
      offset += static_cast<std::size_t>(rc);
    } else if (rc < 0 && errno == EINTR) {
      continue;
    } else {
      return false;
    }
  }
  return true;
}

bool WriteAll(int fd, const void* input, std::size_t byte_count) {
  const auto* bytes = static_cast<const std::uint8_t*>(input);
  std::size_t offset = 0;
  while (offset < byte_count) {
    const auto rc = ::write(fd, bytes + offset, byte_count - offset);
    if (rc > 0) {
      offset += static_cast<std::size_t>(rc);
    } else if (rc < 0 && errno == EINTR) {
      continue;
    } else {
      return false;
    }
  }
  return true;
}
#endif

} // namespace

std::uint32_t DecodeU32(const std::uint8_t* bytes, std::size_t byte_count) {
  if (bytes == nullptr || byte_count < 4) return 0;
  return static_cast<std::uint32_t>(bytes[0]) |
         (static_cast<std::uint32_t>(bytes[1]) << 8) |
         (static_cast<std::uint32_t>(bytes[2]) << 16) |
         (static_cast<std::uint32_t>(bytes[3]) << 24);
}

void AppendU16(std::vector<std::uint8_t>* out, std::uint16_t value) {
  out->push_back(static_cast<std::uint8_t>(value & 0xff));
  out->push_back(static_cast<std::uint8_t>((value >> 8) & 0xff));
}

void AppendU24(std::vector<std::uint8_t>* out, std::uint32_t value) {
  out->push_back(static_cast<std::uint8_t>(value & 0xff));
  out->push_back(static_cast<std::uint8_t>((value >> 8) & 0xff));
  out->push_back(static_cast<std::uint8_t>((value >> 16) & 0xff));
}

void AppendU32(std::vector<std::uint8_t>* out, std::uint32_t value) {
  out->push_back(static_cast<std::uint8_t>(value & 0xff));
  out->push_back(static_cast<std::uint8_t>((value >> 8) & 0xff));
  out->push_back(static_cast<std::uint8_t>((value >> 16) & 0xff));
  out->push_back(static_cast<std::uint8_t>((value >> 24) & 0xff));
}

void AppendNullString(std::vector<std::uint8_t>* out, std::string_view value) {
  out->insert(out->end(), value.begin(), value.end());
  out->push_back(0);
}

void AppendLengthEncodedInteger(std::vector<std::uint8_t>* out,
                                std::uint64_t value) {
  if (value < 251) {
    out->push_back(static_cast<std::uint8_t>(value));
  } else if (value <= 0xffff) {
    out->push_back(0xfc);
    AppendU16(out, static_cast<std::uint16_t>(value));
  } else if (value <= 0xffffff) {
    out->push_back(0xfd);
    AppendU24(out, static_cast<std::uint32_t>(value));
  } else {
    out->push_back(0xfe);
    for (int shift = 0; shift < 64; shift += 8) {
      out->push_back(static_cast<std::uint8_t>((value >> shift) & 0xff));
    }
  }
}

void AppendLengthEncodedString(std::vector<std::uint8_t>* out,
                               std::string_view value) {
  AppendLengthEncodedInteger(out, value.size());
  out->insert(out->end(), value.begin(), value.end());
}

bool ReadPacket(int fd, Packet* packet) {
#ifdef _WIN32
  (void)fd;
  (void)packet;
  return false;
#else
  if (packet == nullptr) return false;
  std::uint8_t header[4] = {};
  if (!ReadExact(fd, header, sizeof(header))) return false;
  const auto byte_count = static_cast<std::uint32_t>(header[0]) |
                          (static_cast<std::uint32_t>(header[1]) << 8) |
                          (static_cast<std::uint32_t>(header[2]) << 16);
  if (byte_count > kMaximumPayloadBytes) return false;
  packet->sequence = header[3];
  packet->payload.assign(byte_count, 0);
  return packet->payload.empty() ||
         ReadExact(fd, packet->payload.data(), packet->payload.size());
#endif
}

bool WritePacket(int fd, std::uint8_t sequence,
                 const std::vector<std::uint8_t>& payload) {
#ifdef _WIN32
  (void)fd;
  (void)sequence;
  (void)payload;
  return false;
#else
  if (payload.size() > kMaximumPayloadBytes) return false;
  std::vector<std::uint8_t> frame;
  frame.reserve(payload.size() + 4);
  AppendU24(&frame, static_cast<std::uint32_t>(payload.size()));
  frame.push_back(sequence);
  frame.insert(frame.end(), payload.begin(), payload.end());
  return WriteAll(fd, frame.data(), frame.size());
#endif
}

} // namespace scratchbird::parser::compatibility::mywire
