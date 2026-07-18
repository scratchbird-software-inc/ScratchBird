// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "postgresql_worker_session.hpp"

#include "postgresql_dialect.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifndef _WIN32
#include <unistd.h>
#endif

namespace scratchbird::parser::postgresql {
namespace {

constexpr std::uint32_t kProtocolV3 = 196608;
constexpr std::uint32_t kSslRequest = 80877103;
constexpr std::uint32_t kGssEncRequest = 80877104;
constexpr std::uint32_t kCancelRequest = 80877102;

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

std::uint32_t ReadBe32(const std::uint8_t* data) {
  return (static_cast<std::uint32_t>(data[0]) << 24) |
         (static_cast<std::uint32_t>(data[1]) << 16) |
         (static_cast<std::uint32_t>(data[2]) << 8) |
         static_cast<std::uint32_t>(data[3]);
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

std::vector<std::uint8_t> Packet(char type, const std::vector<std::uint8_t>& body) {
  std::vector<std::uint8_t> out;
  out.reserve(1 + 4 + body.size());
  out.push_back(static_cast<std::uint8_t>(type));
  AppendBe32(&out, static_cast<std::uint32_t>(body.size() + 4));
  out.insert(out.end(), body.begin(), body.end());
  return out;
}

#ifndef _WIN32
bool SendPacket(int fd, char type, const std::vector<std::uint8_t>& body) {
  const auto packet = Packet(type, body);
  return WriteAll(fd, packet.data(), packet.size());
}
#endif

std::vector<std::uint8_t> AuthenticationOkBody() {
  std::vector<std::uint8_t> body;
  AppendBe32(&body, 0);
  return body;
}

std::vector<std::uint8_t> ParameterStatusBody(std::string_view name,
                                              std::string_view value) {
  std::vector<std::uint8_t> body;
  AppendCString(&body, name);
  AppendCString(&body, value);
  return body;
}

std::vector<std::uint8_t> BackendKeyDataBody() {
  std::vector<std::uint8_t> body;
  AppendBe32(&body, 4242);
  AppendBe32(&body, 0x53425047u);
  return body;
}

std::vector<std::uint8_t> ReadyBody() {
  return {static_cast<std::uint8_t>('I')};
}

std::vector<std::uint8_t> CommandCompleteBody(std::string_view tag) {
  std::vector<std::uint8_t> body;
  AppendCString(&body, tag);
  return body;
}

std::vector<std::uint8_t> RowDescriptionOneTextColumnBody(std::string_view name) {
  std::vector<std::uint8_t> body;
  AppendBe16(&body, 1);
  AppendCString(&body, name);
  AppendBe32(&body, 0);       // table oid
  AppendBe16(&body, 0);       // column attr
  AppendBe32(&body, 25);      // TEXTOID
  AppendBe16(&body, 0xffff);  // typlen -1
  AppendBe32(&body, 0xffffffffu);
  AppendBe16(&body, 0);       // text format
  return body;
}

std::vector<std::uint8_t> DataRowOneTextColumnBody(std::string_view value) {
  std::vector<std::uint8_t> body;
  AppendBe16(&body, 1);
  AppendBe32(&body, static_cast<std::uint32_t>(value.size()));
  body.insert(body.end(), value.begin(), value.end());
  return body;
}

std::vector<std::uint8_t> EmptyRowDescriptionBody() {
  std::vector<std::uint8_t> body;
  AppendBe16(&body, 0);
  return body;
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
  if (pos == std::string_view::npos) return "PostgreSQL compatibility parser refused the statement";
  const auto start = pos + marker.size();
  const auto end = json.find('"', start);
  if (end == std::string_view::npos) return std::string(json.substr(start));
  return std::string(json.substr(start, end - start));
}

std::vector<std::uint8_t> ErrorResponseBody(std::string_view code,
                                            std::string_view message) {
  std::vector<std::uint8_t> body;
  body.push_back(static_cast<std::uint8_t>('S'));
  AppendCString(&body, "ERROR");
  body.push_back(static_cast<std::uint8_t>('V'));
  AppendCString(&body, "ERROR");
  body.push_back(static_cast<std::uint8_t>('C'));
  AppendCString(&body, code);
  body.push_back(static_cast<std::uint8_t>('M'));
  AppendCString(&body, message);
  body.push_back(0);
  return body;
}

#ifndef _WIN32
bool SendReady(int fd) {
  return SendPacket(fd, 'Z', ReadyBody());
}

bool SendStartupOk(int fd) {
  if (!SendPacket(fd, 'R', AuthenticationOkBody())) return false;
  for (const auto& [name, value] : std::array<std::pair<std::string_view, std::string_view>, 7>{
           {{"server_version", "18.3-scratchbird-reference"},
            {"server_encoding", "UTF8"},
            {"client_encoding", "UTF8"},
            {"DateStyle", "ISO, MDY"},
            {"integer_datetimes", "on"},
            {"standard_conforming_strings", "on"},
            {"TimeZone", "UTC"}}}) {
    if (!SendPacket(fd, 'S', ParameterStatusBody(name, value))) return false;
  }
  if (!SendPacket(fd, 'K', BackendKeyDataBody())) return false;
  return SendReady(fd);
}

bool SendParseResponse(int fd, std::string_view sql, PostgresqlWireParseFn parse_statement) {
  const auto parsed = parse_statement != nullptr ? parse_statement(sql) : ParseStatement(sql);
  if (!parsed.ok) {
    if (!SendPacket(fd, 'E', ErrorResponseBody("42601", ExtractDiagnosticMessage(parsed.message_vector_json)))) {
      return false;
    }
    return SendReady(fd);
  }
  if (IsSingletonProbe(sql)) {
    if (!SendPacket(fd, 'T', RowDescriptionOneTextColumnBody("sb_reference_probe"))) return false;
    if (!SendPacket(fd, 'D', DataRowOneTextColumnBody("1"))) return false;
    if (!SendPacket(fd, 'C', CommandCompleteBody("SELECT 1"))) return false;
    return SendReady(fd);
  }
  if (parsed.statement_family == "query") {
    if (!SendPacket(fd, 'T', EmptyRowDescriptionBody())) return false;
    if (!SendPacket(fd, 'C', CommandCompleteBody("SELECT 0"))) return false;
    return SendReady(fd);
  }
  if (!SendPacket(fd, 'C', CommandCompleteBody("SCRATCHBIRD PARSE"))) return false;
  return SendReady(fd);
}

bool ReadTypedFrame(int fd, char* type, std::vector<std::uint8_t>* body) {
  std::uint8_t type_byte = 0;
  std::uint8_t len_bytes[4] = {};
  if (!ReadExact(fd, &type_byte, 1)) return false;
  if (!ReadExact(fd, len_bytes, sizeof(len_bytes))) return false;
  const auto len = ReadBe32(len_bytes);
  if (len < 4 || len > 16 * 1024 * 1024) return false;
  body->assign(len - 4, 0);
  if (!body->empty() && !ReadExact(fd, body->data(), body->size())) return false;
  *type = static_cast<char>(type_byte);
  return true;
}

std::string CStringFromBody(const std::vector<std::uint8_t>& body) {
  const auto nul = std::find(body.begin(), body.end(), 0);
  return std::string(body.begin(), nul);
}
#endif

} // namespace

int ServePostgresqlWireWorkerSession(int fd, PostgresqlWireParseFn parse_statement) {
#ifdef _WIN32
  (void)fd;
  (void)parse_statement;
  return 1;
#else
  for (;;) {
    std::uint8_t len_bytes[4] = {};
    if (!ReadExact(fd, len_bytes, sizeof(len_bytes))) return 1;
    const auto len = ReadBe32(len_bytes);
    if (len < 8 || len > 16 * 1024 * 1024) return 1;
    std::vector<std::uint8_t> startup(len - 4, 0);
    if (!ReadExact(fd, startup.data(), startup.size())) return 1;
    const auto code = ReadBe32(startup.data());
    if (code == kSslRequest || code == kGssEncRequest) {
      const char no = 'N';
      if (!WriteAll(fd, &no, 1)) return 1;
      continue;
    }
    if (code == kCancelRequest) return 0;
    if (code != kProtocolV3) {
      SendPacket(fd, 'E', ErrorResponseBody("08P01", "unsupported PostgreSQL startup protocol"));
      return 1;
    }
    break;
  }

  if (!SendStartupOk(fd)) return 1;
  for (;;) {
    char type = 0;
    std::vector<std::uint8_t> body;
    if (!ReadTypedFrame(fd, &type, &body)) return 0;
    switch (type) {
      case 'Q':
        if (!SendParseResponse(fd, CStringFromBody(body), parse_statement)) return 1;
        break;
      case 'X':
        return 0;
      case 'S':
        if (!SendReady(fd)) return 1;
        break;
      default:
        if (!SendPacket(fd, 'E', ErrorResponseBody("08P01", "unsupported PostgreSQL frontend message"))) {
          return 1;
        }
        if (!SendReady(fd)) return 1;
        break;
    }
  }
#endif
}

int ServePostgresqlWorkerSession(int fd) {
  return ServePostgresqlWireWorkerSession(fd, ParseStatement);
}

} // namespace scratchbird::parser::postgresql
