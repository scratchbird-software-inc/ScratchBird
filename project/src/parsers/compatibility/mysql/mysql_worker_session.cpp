// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "mysql_worker_session.hpp"

#include "mysql_dialect.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#ifndef _WIN32
#include <unistd.h>
#endif

namespace scratchbird::parser::mysql {
namespace {

constexpr std::uint32_t kClientLongPassword = 0x00000001u;
constexpr std::uint32_t kClientLongFlag = 0x00000004u;
constexpr std::uint32_t kClientConnectWithDb = 0x00000008u;
constexpr std::uint32_t kClientProtocol41 = 0x00000200u;
constexpr std::uint32_t kClientTransactions = 0x00002000u;
constexpr std::uint32_t kClientSecureConnection = 0x00008000u;
constexpr std::uint32_t kClientMultiStatements = 0x00010000u;
constexpr std::uint32_t kClientMultiResults = 0x00020000u;
constexpr std::uint32_t kClientPluginAuth = 0x00080000u;
constexpr std::uint32_t kClientConnectAttrs = 0x00100000u;
constexpr std::uint32_t kClientPluginAuthLenencClientData = 0x00200000u;
constexpr std::uint32_t kClientDeprecateEof = 0x01000000u;
constexpr std::uint32_t kServerCapabilities =
    kClientLongPassword | kClientLongFlag | kClientConnectWithDb | kClientProtocol41 |
    kClientTransactions | kClientSecureConnection | kClientMultiStatements |
    kClientMultiResults | kClientPluginAuth | kClientConnectAttrs |
    kClientPluginAuthLenencClientData;

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

bool WriteAll(int fd, const void* data, std::size_t size) {
  const auto* bytes = static_cast<const std::uint8_t*>(data);
  std::size_t written = 0;
  while (written < size) {
    const auto rc = ::write(fd, bytes + written, size - written);
    if (rc > 0) {
      written += static_cast<std::size_t>(rc);
      continue;
    }
    if (rc < 0 && errno == EINTR) continue;
    return false;
  }
  return true;
}
#endif

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

void AppendLenencInt(std::vector<std::uint8_t>* out, std::uint64_t value) {
  if (value < 251) {
    out->push_back(static_cast<std::uint8_t>(value));
    return;
  }
  if (value <= 0xffff) {
    out->push_back(0xfc);
    AppendU16(out, static_cast<std::uint16_t>(value));
    return;
  }
  if (value <= 0xffffff) {
    out->push_back(0xfd);
    AppendU24(out, static_cast<std::uint32_t>(value));
    return;
  }
  out->push_back(0xfe);
  for (int shift = 0; shift < 64; shift += 8) {
    out->push_back(static_cast<std::uint8_t>((value >> shift) & 0xff));
  }
}

void AppendLenencString(std::vector<std::uint8_t>* out, std::string_view value) {
  AppendLenencInt(out, value.size());
  out->insert(out->end(), value.begin(), value.end());
}

#ifndef _WIN32
bool SendPacket(int fd, std::uint8_t sequence, const std::vector<std::uint8_t>& payload) {
  std::vector<std::uint8_t> out;
  out.reserve(4 + payload.size());
  AppendU24(&out, static_cast<std::uint32_t>(payload.size()));
  out.push_back(sequence);
  out.insert(out.end(), payload.begin(), payload.end());
  return WriteAll(fd, out.data(), out.size());
}

bool ReadPacket(int fd, std::uint8_t* sequence, std::vector<std::uint8_t>* payload) {
  std::uint8_t header[4] = {};
  if (!ReadExact(fd, header, sizeof(header))) return false;
  const auto size = static_cast<std::uint32_t>(header[0]) |
                    (static_cast<std::uint32_t>(header[1]) << 8) |
                    (static_cast<std::uint32_t>(header[2]) << 16);
  if (size > 16 * 1024 * 1024) return false;
  payload->assign(size, 0);
  if (size != 0 && !ReadExact(fd, payload->data(), payload->size())) return false;
  *sequence = header[3];
  return true;
}
#endif

std::vector<std::uint8_t> HandshakePayload() {
  std::vector<std::uint8_t> payload;
  payload.push_back(10);
  AppendNullString(&payload, "8.4.0-ScratchBird-reference");
  AppendU32(&payload, 4242);
  const std::string auth1 = "sbmysql1";
  payload.insert(payload.end(), auth1.begin(), auth1.end());
  payload.push_back(0);
  AppendU16(&payload, static_cast<std::uint16_t>(kServerCapabilities & 0xffff));
  payload.push_back(33);  // utf8_general_ci
  AppendU16(&payload, 2); // SERVER_STATUS_AUTOCOMMIT
  AppendU16(&payload, static_cast<std::uint16_t>((kServerCapabilities >> 16) & 0xffff));
  payload.push_back(21);
  payload.insert(payload.end(), 10, 0);
  const std::string auth2 = "sbmysql2auth";
  payload.insert(payload.end(), auth2.begin(), auth2.end());
  payload.push_back(0);
  AppendNullString(&payload, "caching_sha2_password");
  return payload;
}

std::vector<std::uint8_t> OkPayload() {
  std::vector<std::uint8_t> payload;
  payload.push_back(0x00);
  AppendLenencInt(&payload, 0);
  AppendLenencInt(&payload, 0);
  AppendU16(&payload, 2);
  AppendU16(&payload, 0);
  return payload;
}

std::vector<std::uint8_t> EofPayload() {
  std::vector<std::uint8_t> payload;
  payload.push_back(0xfe);
  AppendU16(&payload, 0);
  AppendU16(&payload, 2);
  return payload;
}

std::vector<std::uint8_t> ErrorPayload(std::string_view message) {
  std::vector<std::uint8_t> payload;
  payload.push_back(0xff);
  AppendU16(&payload, 1064);
  payload.push_back('#');
  payload.insert(payload.end(), {'4', '2', '0', '0', '0'});
  payload.insert(payload.end(), message.begin(), message.end());
  return payload;
}

std::vector<std::uint8_t> ColumnDefinition(std::string_view name) {
  std::vector<std::uint8_t> payload;
  AppendLenencString(&payload, "def");
  AppendLenencString(&payload, "");
  AppendLenencString(&payload, "");
  AppendLenencString(&payload, "");
  AppendLenencString(&payload, name);
  AppendLenencString(&payload, "");
  payload.push_back(0x0c);
  AppendU16(&payload, 33);
  AppendU32(&payload, 1024);
  payload.push_back(0xfd); // VAR_STRING
  AppendU16(&payload, 0);
  payload.push_back(0);
  payload.push_back(0);
  return payload;
}

std::vector<std::uint8_t> TextRow(std::string_view value) {
  std::vector<std::uint8_t> payload;
  AppendLenencString(&payload, value);
  return payload;
}

std::string StripSemicolon(std::string_view sql) {
  std::string out = TrimAscii(sql);
  while (!out.empty() && out.back() == ';') out.pop_back();
  return NormalizeWhitespace(out);
}

bool IsSingletonProbe(std::string_view sql) {
  const auto upper = ToUpperAscii(StripSemicolon(sql));
  return upper == "SELECT 1" || upper == "SELECT 1 AS SB_REFERENCE_PROBE" ||
         upper == "SELECT 1 AS SCRATCHBIRD_REFERENCE_PROBE";
}

std::string ExtractDiagnosticMessage(std::string_view json) {
  const std::string marker = "\"message\":\"";
  const auto pos = json.find(marker);
  if (pos == std::string_view::npos) return "MySQL compatibility parser refused the statement";
  const auto start = pos + marker.size();
  const auto end = json.find('"', start);
  if (end == std::string_view::npos) return std::string(json.substr(start));
  return std::string(json.substr(start, end - start));
}

#ifndef _WIN32
bool SendTerminatorPacket(int fd, std::uint8_t sequence, bool client_deprecates_eof) {
  return SendPacket(fd, sequence, client_deprecates_eof ? OkPayload() : EofPayload());
}

bool SendResultSetOneRow(int fd,
                         std::string_view column,
                         std::string_view value,
                         bool client_deprecates_eof) {
  if (!SendPacket(fd, 1, std::vector<std::uint8_t>{1})) return false;
  if (!SendPacket(fd, 2, ColumnDefinition(column))) return false;
  if (!SendTerminatorPacket(fd, 3, client_deprecates_eof)) return false;
  if (!SendPacket(fd, 4, TextRow(value))) return false;
  return SendTerminatorPacket(fd, 5, client_deprecates_eof);
}

bool SendEmptyResultSet(int fd) {
  return SendPacket(fd, 1, OkPayload());
}

std::uint32_t ReadClientCapabilities(const std::vector<std::uint8_t>& handshake_response) {
  if (handshake_response.size() < 4) return 0;
  return static_cast<std::uint32_t>(handshake_response[0]) |
         (static_cast<std::uint32_t>(handshake_response[1]) << 8) |
         (static_cast<std::uint32_t>(handshake_response[2]) << 16) |
         (static_cast<std::uint32_t>(handshake_response[3]) << 24);
}

bool HandleQuery(int fd,
                 std::string_view sql,
                 bool client_deprecates_eof,
                 MysqlWireParseFn parse_statement) {
  const auto parsed = parse_statement != nullptr ? parse_statement(sql) : ParseStatement(sql);
  if (!parsed.ok) {
    return SendPacket(fd, 1, ErrorPayload(ExtractDiagnosticMessage(parsed.message_vector_json)));
  }
  if (IsSingletonProbe(sql)) {
    return SendResultSetOneRow(fd, "sb_reference_probe", "1", client_deprecates_eof);
  }
  return SendEmptyResultSet(fd);
}
#endif

} // namespace

int ServeMysqlWireWorkerSession(int fd, MysqlWireParseFn parse_statement) {
#ifdef _WIN32
  (void)fd;
  (void)parse_statement;
  return 1;
#else
  if (!SendPacket(fd, 0, HandshakePayload())) return 1;
  std::uint8_t sequence = 0;
  std::vector<std::uint8_t> payload;
  if (!ReadPacket(fd, &sequence, &payload)) return 1;
  const bool client_deprecates_eof =
      (ReadClientCapabilities(payload) & kServerCapabilities & kClientDeprecateEof) != 0;
  if (!SendPacket(fd, 2, OkPayload())) return 1;

  for (;;) {
    payload.clear();
    if (!ReadPacket(fd, &sequence, &payload)) return 0;
    if (payload.empty()) continue;
    const auto command = payload[0];
    if (command == 0x01) return 0;  // COM_QUIT
    if (command == 0x0e) {          // COM_PING
      if (!SendPacket(fd, 1, OkPayload())) return 1;
      continue;
    }
    if (command == 0x03) {          // COM_QUERY
      const std::string sql(payload.begin() + 1, payload.end());
      if (!HandleQuery(fd, sql, client_deprecates_eof, parse_statement)) return 1;
      continue;
    }
    if (!SendPacket(fd, 1, ErrorPayload("unsupported MySQL frontend command"))) return 1;
  }
#endif
}

int ServeMysqlWorkerSession(int fd) {
  return ServeMysqlWireWorkerSession(fd, ParseStatement);
}

} // namespace scratchbird::parser::mysql
