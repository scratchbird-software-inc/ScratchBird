// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "foundationdb_flow_worker_session.hpp"

#include "foundationdb_dialect.hpp"

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#ifndef _WIN32
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

namespace scratchbird::parser::foundationdb {
namespace {

constexpr std::size_t kMinFoundationdbConnectPacketSize = 40;
constexpr std::size_t kMaxFoundationdbConnectPacketSize = 128;
constexpr std::uint64_t kWellKnownTokenHigh = UINT64_MAX;
constexpr std::uint64_t kWellKnownPing = 1;
constexpr std::uint64_t kWellKnownOpenDatabase = 4;
constexpr std::uint64_t kWellKnownProtocolInfo = 10;
constexpr std::uint32_t kMaxFlowPacket = 32 * 1024 * 1024;

#ifndef _WIN32
bool ReadExact(int fd, void* out, std::size_t size) {
  auto* bytes = static_cast<std::uint8_t*>(out);
  std::size_t read_total = 0;
  while (read_total < size) {
    const auto rc = ::read(fd, bytes + read_total, size - read_total);
    if (rc > 0) {
      read_total += static_cast<std::size_t>(rc);
      continue;
    }
    if (rc < 0 && errno == EINTR) continue;
    return false;
  }
  return true;
}

void SetShortReadTimeout(int fd) {
  timeval timeout {};
  timeout.tv_sec = 2;
  timeout.tv_usec = 0;
  (void)::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
}
#endif

std::uint32_t ReadU32LE(const std::uint8_t* data) {
  return static_cast<std::uint32_t>(data[0]) |
         (static_cast<std::uint32_t>(data[1]) << 8) |
         (static_cast<std::uint32_t>(data[2]) << 16) |
         (static_cast<std::uint32_t>(data[3]) << 24);
}

std::uint64_t ReadU64LE(const std::uint8_t* data) {
  std::uint64_t out = 0;
  for (int shift = 0; shift < 64; shift += 8) {
    out |= static_cast<std::uint64_t>(data[static_cast<std::size_t>(shift / 8)]) << shift;
  }
  return out;
}

std::string TokenName(std::uint64_t high, std::uint64_t low) {
  if (high != kWellKnownTokenHigh) return "non_well_known";
  if (low == kWellKnownPing) return "ping";
  if (low == kWellKnownOpenDatabase) return "open_database";
  if (low == kWellKnownProtocolInfo) return "protocol_info";
  return "well_known_" + std::to_string(low);
}

bool AdmitStatusProjection() {
  const auto parsed = ParseStatement("STATUS");
  return parsed.ok && parsed.catalog_projection_only;
}

} // namespace

int ServeFoundationdbFlowWorkerSession(int fd) {
#ifdef _WIN32
  (void)fd;
  return 1;
#else
  SetShortReadTimeout(fd);

  std::uint8_t connect_header[4] = {};
  if (!ReadExact(fd, connect_header, sizeof(connect_header))) return 1;
  const auto connect_payload_len = ReadU32LE(connect_header);
  const auto connect_total = connect_payload_len + static_cast<std::uint32_t>(sizeof(connect_header));
  if (connect_total < kMinFoundationdbConnectPacketSize ||
      connect_total > kMaxFoundationdbConnectPacketSize ||
      connect_payload_len == 0) {
    std::cerr << "foundationdb_flow_invalid_connect_packet length=" << connect_total << '\n';
    return 1;
  }

  std::vector<std::uint8_t> connect_payload(connect_payload_len);
  if (!ReadExact(fd, connect_payload.data(), connect_payload.size())) return 1;
  const auto protocol = connect_payload.size() >= 8 ? ReadU64LE(connect_payload.data()) : 0;
  std::cerr << "foundationdb_flow_connect_packet protocol=0x" << std::hex << protocol
            << std::dec << " total=" << connect_total << '\n';

  bool saw_open_database = false;
  bool saw_status_projection = false;
  for (;;) {
    std::uint8_t len_buf[4] = {};
    if (!ReadExact(fd, len_buf, sizeof(len_buf))) break;
    const auto packet_len = ReadU32LE(len_buf);
    if (packet_len < 16 || packet_len > kMaxFlowPacket) {
      std::cerr << "foundationdb_flow_invalid_packet length=" << packet_len << '\n';
      return 1;
    }

    std::uint8_t checksum[8] = {};
    if (!ReadExact(fd, checksum, sizeof(checksum))) break;
    (void)checksum;

    std::vector<std::uint8_t> payload(packet_len);
    if (!ReadExact(fd, payload.data(), payload.size())) break;
    const auto token_high = ReadU64LE(payload.data());
    const auto token_low = ReadU64LE(payload.data() + 8);
    const auto token_name = TokenName(token_high, token_low);
    std::cerr << "foundationdb_flow_packet token=" << token_name
              << " high=0x" << std::hex << token_high
              << " low=0x" << token_low << std::dec
              << " length=" << packet_len << '\n';

    if (token_high == kWellKnownTokenHigh && token_low == kWellKnownOpenDatabase) {
      saw_open_database = true;
      saw_status_projection = AdmitStatusProjection();
      std::cerr << "foundationdb_flow_open_database_cluster_status_refusal"
                << " status_projection=" << (saw_status_projection ? "true" : "false")
                << '\n';
    }
  }

  if (!saw_open_database || !saw_status_projection) {
    std::cerr << "foundationdb_flow_probe_incomplete"
              << " saw_open_database=" << (saw_open_database ? "true" : "false")
              << " status_projection=" << (saw_status_projection ? "true" : "false")
              << '\n';
    return 1;
  }
  return 0;
#endif
}

} // namespace scratchbird::parser::foundationdb
