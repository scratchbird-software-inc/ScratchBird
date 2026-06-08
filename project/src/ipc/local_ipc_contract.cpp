// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "local_ipc_contract.hpp"

#include <algorithm>
#include <limits>

namespace scratchbird::ipc {
namespace {

void PutU16(std::vector<std::uint8_t>* out, std::uint16_t value) {
  out->push_back(static_cast<std::uint8_t>(value & 0xffu));
  out->push_back(static_cast<std::uint8_t>((value >> 8u) & 0xffu));
}

void PutU32(std::vector<std::uint8_t>* out, std::uint32_t value) {
  for (int shift = 0; shift < 32; shift += 8) {
    out->push_back(static_cast<std::uint8_t>((value >> shift) & 0xffu));
  }
}

void PutU64(std::vector<std::uint8_t>* out, std::uint64_t value) {
  for (int shift = 0; shift < 64; shift += 8) {
    out->push_back(static_cast<std::uint8_t>((value >> shift) & 0xffu));
  }
}

std::uint16_t ReadU16(const std::vector<std::uint8_t>& data, std::size_t off) {
  return static_cast<std::uint16_t>(data[off]) |
         static_cast<std::uint16_t>(static_cast<std::uint16_t>(data[off + 1]) << 8u);
}

std::uint32_t ReadU32(const std::vector<std::uint8_t>& data, std::size_t off) {
  std::uint32_t out = 0;
  for (int shift = 0; shift < 32; shift += 8) {
    out |= static_cast<std::uint32_t>(data[off + static_cast<std::size_t>(shift / 8)]) << shift;
  }
  return out;
}

std::uint64_t ReadU64(const std::vector<std::uint8_t>& data, std::size_t off) {
  std::uint64_t out = 0;
  for (int shift = 0; shift < 64; shift += 8) {
    out |= static_cast<std::uint64_t>(data[off + static_cast<std::size_t>(shift / 8)]) << shift;
  }
  return out;
}

void PutUuid(std::vector<std::uint8_t>* out, const std::array<std::uint8_t, 16>& uuid) {
  out->insert(out->end(), uuid.begin(), uuid.end());
}

std::array<std::uint8_t, 16> ReadUuid(const std::vector<std::uint8_t>& data, std::size_t off) {
  std::array<std::uint8_t, 16> uuid{};
  std::copy_n(data.begin() + static_cast<std::ptrdiff_t>(off), uuid.size(), uuid.begin());
  return uuid;
}

LocalIpcDecodeResult Fail(std::string code, std::string message) {
  LocalIpcDecodeResult result;
  result.diagnostic_code = std::move(code);
  result.diagnostic_message = std::move(message);
  return result;
}

std::vector<std::uint8_t> EncodeHeaderWithCrc(const LocalIpcHeader& header,
                                              std::uint32_t payload_crc,
                                              std::uint32_t header_crc) {
  std::vector<std::uint8_t> out;
  out.reserve(kLocalIpcHeaderLength);
  PutU32(&out, header.magic);
  PutU16(&out, header.major);
  PutU16(&out, header.minor);
  PutU16(&out, header.header_length);
  PutU16(&out, static_cast<std::uint16_t>(header.message_type));
  PutU32(&out, header.flags);
  PutU64(&out, header.payload_length);
  PutU32(&out, payload_crc);
  PutU32(&out, header_crc);
  PutUuid(&out, header.correlation_uuid);
  PutUuid(&out, header.session_uuid);
  PutUuid(&out, header.transaction_uuid);
  PutU64(&out, 0);
  return out;
}

}  // namespace

bool LocalIpcVersionCompatible(std::uint16_t major, std::uint16_t minor) {
  return major == kLocalIpcMajor && minor <= kLocalIpcMinor;
}

std::uint32_t LocalIpcCrc32c(const std::uint8_t* data, std::size_t size) {
  std::uint32_t crc = 0xffffffffu;
  for (std::size_t i = 0; i < size; ++i) {
    crc ^= data[i];
    for (int bit = 0; bit < 8; ++bit) {
      const std::uint32_t mask = static_cast<std::uint32_t>(0u - (crc & 1u));
      crc = (crc >> 1u) ^ (0x82f63b78u & mask);
    }
  }
  return ~crc;
}

std::vector<std::uint8_t> EncodeLocalIpcFrame(const LocalIpcFrame& frame) {
  if (frame.payload.size() > kLocalIpcMaxPayloadBytes) return {};
  LocalIpcHeader header = frame.header;
  header.magic = kLocalIpcMagic;
  header.header_length = kLocalIpcHeaderLength;
  header.payload_length = static_cast<std::uint64_t>(frame.payload.size());
  const std::uint32_t payload_crc =
      frame.payload.empty() ? 0 : LocalIpcCrc32c(frame.payload.data(), frame.payload.size());
  auto header_bytes = EncodeHeaderWithCrc(header, payload_crc, 0);
  const std::uint32_t header_crc = LocalIpcCrc32c(header_bytes.data(), header_bytes.size());
  header_bytes = EncodeHeaderWithCrc(header, payload_crc, header_crc);
  header_bytes.insert(header_bytes.end(), frame.payload.begin(), frame.payload.end());
  return header_bytes;
}

LocalIpcDecodeResult DecodeLocalIpcFrame(const std::vector<std::uint8_t>& bytes) {
  if (bytes.size() < kLocalIpcHeaderLength) {
    return Fail("IPC.FRAME.TRUNCATED", "local IPC frame header is truncated");
  }
  LocalIpcFrame frame;
  frame.header.magic = ReadU32(bytes, 0);
  frame.header.major = ReadU16(bytes, 4);
  frame.header.minor = ReadU16(bytes, 6);
  frame.header.header_length = ReadU16(bytes, 8);
  frame.header.message_type = static_cast<LocalIpcMessageType>(ReadU16(bytes, 10));
  frame.header.flags = ReadU32(bytes, 12);
  frame.header.payload_length = ReadU64(bytes, 16);
  frame.header.payload_crc32c = ReadU32(bytes, 24);
  frame.header.header_crc32c = ReadU32(bytes, 28);
  frame.header.correlation_uuid = ReadUuid(bytes, 32);
  frame.header.session_uuid = ReadUuid(bytes, 48);
  frame.header.transaction_uuid = ReadUuid(bytes, 64);

  if (frame.header.magic != kLocalIpcMagic) {
    return Fail("IPC.FRAME.MAGIC", "local IPC frame magic is not SBIP");
  }
  if (!LocalIpcVersionCompatible(frame.header.major, frame.header.minor)) {
    return Fail("IPC.FRAME.VERSION", "local IPC frame version is not compatible");
  }
  if (frame.header.header_length != kLocalIpcHeaderLength) {
    return Fail("IPC.FRAME.HEADER_LENGTH", "local IPC frame header length is invalid");
  }
  if (frame.header.payload_length > kLocalIpcMaxPayloadBytes ||
      frame.header.payload_length > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
    return Fail("IPC.FRAME.PAYLOAD_TOO_LARGE", "local IPC frame payload is too large");
  }
  const auto payload_size = static_cast<std::size_t>(frame.header.payload_length);
  if (bytes.size() != kLocalIpcHeaderLength + payload_size) {
    return Fail("IPC.FRAME.LENGTH", "local IPC frame length does not match payload length");
  }
  auto header_bytes = bytes;
  header_bytes.resize(kLocalIpcHeaderLength);
  for (std::size_t i = 28; i < 32; ++i) header_bytes[i] = 0;
  const auto expected_header_crc = LocalIpcCrc32c(header_bytes.data(), header_bytes.size());
  if (expected_header_crc != frame.header.header_crc32c) {
    return Fail("IPC.FRAME.HEADER_CRC", "local IPC frame header CRC32C mismatch");
  }
  frame.payload.assign(bytes.begin() + kLocalIpcHeaderLength, bytes.end());
  const auto expected_payload_crc =
      frame.payload.empty() ? 0 : LocalIpcCrc32c(frame.payload.data(), frame.payload.size());
  if (expected_payload_crc != frame.header.payload_crc32c) {
    return Fail("IPC.FRAME.PAYLOAD_CRC", "local IPC frame payload CRC32C mismatch");
  }
  LocalIpcDecodeResult result;
  result.ok = true;
  result.frame = std::move(frame);
  return result;
}

LocalIpcFrame MakeLocalIpcHelloFrame(std::array<std::uint8_t, 16> correlation_uuid,
                                     std::string profile,
                                     std::string capability_set) {
  LocalIpcFrame frame;
  frame.header.message_type = LocalIpcMessageType::kHello;
  frame.header.correlation_uuid = correlation_uuid;
  const std::string payload = "profile=" + std::move(profile) + "\ncapabilities=" + std::move(capability_set) + "\n";
  frame.payload.assign(payload.begin(), payload.end());
  return frame;
}

}  // namespace scratchbird::ipc
