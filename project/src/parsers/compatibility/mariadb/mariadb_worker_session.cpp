// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "mariadb_worker_session.hpp"
#include "mariadb_dialect.hpp"
#include "mywire_frame_codec.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#ifndef _WIN32
#include <unistd.h>
#endif

namespace scratchbird::parser::mariadb {
namespace {
namespace wire = scratchbird::parser::compatibility::mywire;

constexpr std::uint32_t kProtocol41 = 0x00000200U;
constexpr std::uint32_t kTransactions = 0x00002000U;
constexpr std::uint32_t kSecureConnection = 0x00008000U;
constexpr std::uint32_t kMultiResults = 0x00020000U;
constexpr std::uint32_t kPluginAuth = 0x00080000U;
constexpr std::uint32_t kConnectAttrs = 0x00100000U;
constexpr std::uint32_t kDeprecateEof = 0x01000000U;
constexpr std::uint32_t kCapabilities =
    0x00000001U | 0x00000004U | 0x00000008U | kProtocol41 |
    kTransactions | kSecureConnection | 0x00010000U | kMultiResults |
    kPluginAuth | kConnectAttrs | 0x00200000U;

std::vector<std::uint8_t> Handshake() {
  std::vector<std::uint8_t> out;
  out.push_back(10);
  wire::AppendNullString(&out, "11.8.0-ScratchBird-MariaDB");
#ifdef _WIN32
  wire::AppendU32(&out, 0);
#else
  wire::AppendU32(&out, static_cast<std::uint32_t>(::getpid()));
#endif
  const std::string first = "sbmaria1";
  out.insert(out.end(), first.begin(), first.end());
  out.push_back(0);
  wire::AppendU16(&out, static_cast<std::uint16_t>(kCapabilities));
  out.push_back(45);
  wire::AppendU16(&out, 2);
  wire::AppendU16(&out, static_cast<std::uint16_t>(kCapabilities >> 16));
  out.push_back(21);
  out.insert(out.end(), 10, 0);
  const std::string second = "sbmaria2auth";
  out.insert(out.end(), second.begin(), second.end());
  out.push_back(0);
  wire::AppendNullString(&out, "caching_sha2_password");
  return out;
}

std::vector<std::uint8_t> Ok() {
  std::vector<std::uint8_t> out{0};
  wire::AppendLengthEncodedInteger(&out, 0);
  wire::AppendLengthEncodedInteger(&out, 0);
  wire::AppendU16(&out, 2);
  wire::AppendU16(&out, 0);
  return out;
}

std::vector<std::uint8_t> Eof() {
  std::vector<std::uint8_t> out{0xfe};
  wire::AppendU16(&out, 0);
  wire::AppendU16(&out, 2);
  return out;
}

std::vector<std::uint8_t> Error(std::string_view message) {
  std::vector<std::uint8_t> out{0xff};
  wire::AppendU16(&out, 1064);
  out.push_back('#');
  out.insert(out.end(), {'4', '2', '0', '0', '0'});
  out.insert(out.end(), message.begin(), message.end());
  return out;
}

std::string Message(std::string_view json) {
  constexpr std::string_view marker = "\"message\":\"";
  const auto at = json.find(marker);
  if (at == std::string_view::npos) return "MariaDB parser refused the statement";
  const auto begin = at + marker.size();
  const auto end = json.find('"', begin);
  return std::string(json.substr(begin, end == std::string_view::npos ?
                                           std::string_view::npos : end - begin));
}

bool Probe(std::string_view sql) {
  auto text = ToUpperAscii(NormalizeWhitespace(TrimAscii(sql)));
  while (!text.empty() && text.back() == ';') text.pop_back();
  return text == "SELECT 1" || text == "SELECT 1 AS SB_REFERENCE_PROBE" ||
         text == "SELECT 1 AS SCRATCHBIRD_REFERENCE_PROBE";
}

std::vector<std::uint8_t> Column() {
  std::vector<std::uint8_t> out;
  for (auto value : {"def", "", "", "", "sb_reference_probe", ""})
    wire::AppendLengthEncodedString(&out, value);
  out.push_back(0x0c);
  wire::AppendU16(&out, 45);
  wire::AppendU32(&out, 1024);
  out.push_back(0xfd);
  wire::AppendU16(&out, 0);
  out.push_back(0);
  out.push_back(0);
  return out;
}

bool Render(int fd, std::string_view sql, bool deprecate_eof) {
  const auto parsed = ParseStatement(sql);
  if (!parsed.ok || parsed.fail_closed_refusal)
    return wire::WritePacket(fd, 1, Error(Message(parsed.message_vector_json)));
  if (!Probe(sql)) return wire::WritePacket(fd, 1, Ok());
  const auto terminator = deprecate_eof ? Ok() : Eof();
  std::vector<std::uint8_t> row;
  wire::AppendLengthEncodedString(&row, "1");
  return wire::WritePacket(fd, 1, {1}) && wire::WritePacket(fd, 2, Column()) &&
         wire::WritePacket(fd, 3, terminator) && wire::WritePacket(fd, 4, row) &&
         wire::WritePacket(fd, 5, terminator);
}
} // namespace

int ServeMariadbWorkerSession(int fd) {
#ifdef _WIN32
  (void)fd;
  return 1;
#else
  if (!wire::WritePacket(fd, 0, Handshake())) return 1;
  wire::Packet packet;
  if (!wire::ReadPacket(fd, &packet)) return 1;
  const bool deprecate_eof =
      (wire::DecodeU32(packet.payload.data(), packet.payload.size()) & kDeprecateEof) != 0;
  if (!wire::WritePacket(fd, 2, Ok())) return 1;
  for (;;) {
    if (!wire::ReadPacket(fd, &packet)) return 0;
    if (packet.payload.empty()) continue;
    if (packet.payload[0] == 0x01) return 0;
    if (packet.payload[0] == 0x0e) {
      if (!wire::WritePacket(fd, 1, Ok())) return 1;
    } else if (packet.payload[0] == 0x03) {
      const std::string sql(packet.payload.begin() + 1, packet.payload.end());
      if (!Render(fd, sql, deprecate_eof)) return 1;
    } else if (!wire::WritePacket(fd, 1, Error("unsupported MariaDB frontend command"))) {
      return 1;
    }
  }
#endif
}
} // namespace scratchbird::parser::mariadb
