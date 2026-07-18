// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "clickhouse_worker_session.hpp"

#include "clickhouse_dialect.hpp"

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#ifndef _WIN32
#include <unistd.h>
#endif

namespace scratchbird::parser::clickhouse {
namespace {

constexpr std::uint64_t kClientHello = 0;
constexpr std::uint64_t kClientQuery = 1;
constexpr std::uint64_t kClientPing = 4;

constexpr std::uint64_t kServerHello = 0;
constexpr std::uint64_t kServerData = 1;
constexpr std::uint64_t kServerException = 2;
constexpr std::uint64_t kServerPong = 4;
constexpr std::uint64_t kServerEndOfStream = 5;

constexpr std::uint64_t kServerRevision = 54429;
constexpr std::uint64_t kMinRevisionWithClientInfo = 54032;
constexpr std::uint64_t kMinRevisionWithServerTimezone = 54058;
constexpr std::uint64_t kMinRevisionWithQuotaKeyInClientInfo = 54060;
constexpr std::uint64_t kMinRevisionWithServerDisplayName = 54372;
constexpr std::uint64_t kMinRevisionWithVersionPatch = 54401;
constexpr std::size_t kMaxStringBytes = 16 * 1024 * 1024;

struct ClientHello {
  std::string client_name;
  std::uint64_t version_major{0};
  std::uint64_t version_minor{0};
  std::uint64_t revision{0};
  std::string database;
  std::string user;
  std::string password;
};

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
#else
bool ReadExact(int, void*, std::size_t) {
  return false;
}

bool WriteAll(int, const void*, std::size_t) {
  return false;
}
#endif

void AppendU8(std::vector<std::uint8_t>* out, std::uint8_t value) {
  out->push_back(value);
}

void AppendI32(std::vector<std::uint8_t>* out, std::int32_t value) {
  std::uint32_t raw = static_cast<std::uint32_t>(value);
  for (int shift = 0; shift < 32; shift += 8) {
    out->push_back(static_cast<std::uint8_t>((raw >> shift) & 0xff));
  }
}

void AppendVarUInt(std::vector<std::uint8_t>* out, std::uint64_t value) {
  while (value > 0x7f) {
    out->push_back(static_cast<std::uint8_t>(0x80 | (value & 0x7f)));
    value >>= 7;
  }
  out->push_back(static_cast<std::uint8_t>(value));
}

void AppendString(std::vector<std::uint8_t>* out, std::string_view value) {
  AppendVarUInt(out, static_cast<std::uint64_t>(value.size()));
  out->insert(out->end(), value.begin(), value.end());
}

bool ReadVarUInt(int fd, std::uint64_t* value) {
  *value = 0;
  for (std::size_t i = 0; i < 10; ++i) {
    std::uint8_t byte = 0;
    if (!ReadExact(fd, &byte, 1)) return false;
    *value |= static_cast<std::uint64_t>(byte & 0x7f) << (7 * i);
    if ((byte & 0x80) == 0) return true;
  }
  return false;
}

bool ReadString(int fd, std::string* value) {
  std::uint64_t size = 0;
  if (!ReadVarUInt(fd, &size) || size > kMaxStringBytes) return false;
  value->assign(static_cast<std::size_t>(size), '\0');
  return size == 0 || ReadExact(fd, value->data(), value->size());
}

bool SkipString(int fd) {
  std::string ignored;
  return ReadString(fd, &ignored);
}

bool ReadClientInfoForAdvertisedRevision(int fd) {
  std::uint8_t query_kind = 0;
  if (!ReadExact(fd, &query_kind, 1)) return false;
  if (query_kind == 0) return true;

  std::string ignored;
  if (!ReadString(fd, &ignored)) return false;  // initial_user
  if (!ReadString(fd, &ignored)) return false;  // initial_query_id
  if (!ReadString(fd, &ignored)) return false;  // initial_address

  std::uint8_t interface = 0;
  if (!ReadExact(fd, &interface, 1)) return false;
  if (interface == 1) {
    if (!ReadString(fd, &ignored)) return false;  // os_user
    if (!ReadString(fd, &ignored)) return false;  // client_hostname
    if (!ReadString(fd, &ignored)) return false;  // client_name
    std::uint64_t unused = 0;
    if (!ReadVarUInt(fd, &unused)) return false;  // major
    if (!ReadVarUInt(fd, &unused)) return false;  // minor
    if (!ReadVarUInt(fd, &unused)) return false;  // tcp protocol revision
  } else if (interface == 2) {
    std::uint8_t http_method = 0;
    if (!ReadExact(fd, &http_method, 1)) return false;
    if (!ReadString(fd, &ignored)) return false;  // http_user_agent
  }
  if (kServerRevision >= kMinRevisionWithQuotaKeyInClientInfo &&
      !ReadString(fd, &ignored)) {
    return false;
  }
  if (interface == 1 && kServerRevision >= kMinRevisionWithVersionPatch) {
    std::uint64_t unused = 0;
    if (!ReadVarUInt(fd, &unused)) return false;  // client_version_patch
  }
  return true;
}

bool ReadSettingsStream(int fd) {
  for (;;) {
    std::string name;
    if (!ReadString(fd, &name)) return false;
    if (name.empty()) return true;
    std::uint64_t flags = 0;
    std::string value;
    if (!ReadVarUInt(fd, &flags)) return false;
    if (!ReadString(fd, &value)) return false;
  }
}

bool ReadClientHello(int fd, ClientHello* hello) {
  std::uint64_t packet = 0;
  if (!ReadVarUInt(fd, &packet) || packet != kClientHello) return false;
  if (!ReadString(fd, &hello->client_name)) return false;
  if (!ReadVarUInt(fd, &hello->version_major)) return false;
  if (!ReadVarUInt(fd, &hello->version_minor)) return false;
  if (!ReadVarUInt(fd, &hello->revision)) return false;
  if (!ReadString(fd, &hello->database)) return false;
  if (!ReadString(fd, &hello->user)) return false;
  if (!ReadString(fd, &hello->password)) return false;
  return true;
}

bool ReadQueryPacket(int fd, std::string* query) {
  std::string query_id;
  if (!ReadString(fd, &query_id)) return false;
  if (kServerRevision >= kMinRevisionWithClientInfo &&
      !ReadClientInfoForAdvertisedRevision(fd)) {
    return false;
  }
  if (!ReadSettingsStream(fd)) return false;

  std::uint64_t stage = 0;
  std::uint64_t compression = 0;
  if (!ReadVarUInt(fd, &stage)) return false;
  if (!ReadVarUInt(fd, &compression)) return false;
  return ReadString(fd, query);
}

bool SendPacket(int fd, std::uint64_t packet, const std::vector<std::uint8_t>& body) {
  std::vector<std::uint8_t> out;
  out.reserve(10 + body.size());
  AppendVarUInt(&out, packet);
  out.insert(out.end(), body.begin(), body.end());
  return WriteAll(fd, out.data(), out.size());
}

bool SendHello(int fd) {
  std::vector<std::uint8_t> body;
  AppendString(&body, "ScratchBird ClickHouse reference parser");
  AppendVarUInt(&body, 25);
  AppendVarUInt(&body, 12);
  AppendVarUInt(&body, kServerRevision);
  if (kServerRevision >= kMinRevisionWithServerTimezone) {
    AppendString(&body, "UTC");
  }
  if (kServerRevision >= kMinRevisionWithServerDisplayName) {
    AppendString(&body, "ScratchBird reference parser listener");
  }
  if (kServerRevision >= kMinRevisionWithVersionPatch) {
    AppendVarUInt(&body, 10);
  }
  return SendPacket(fd, kServerHello, body);
}

bool SendPong(int fd) {
  return SendPacket(fd, kServerPong, {});
}

std::string ExtractDiagnosticMessage(std::string_view json) {
  const std::string marker = "\"message\":\"";
  const auto pos = json.find(marker);
  if (pos == std::string_view::npos) return "ClickHouse compatibility parser refused the query";
  const auto start = pos + marker.size();
  const auto end = json.find('"', start);
  if (end == std::string_view::npos) return std::string(json.substr(start));
  return std::string(json.substr(start, end - start));
}

bool SendException(int fd, std::string_view message) {
  std::vector<std::uint8_t> body;
  AppendI32(&body, 62);
  AppendString(&body, "SCRATCHBIRD_REFERENCE_PARSER_EXCEPTION");
  AppendString(&body, message);
  AppendString(&body, "");
  AppendU8(&body, 0);
  return SendPacket(fd, kServerException, body);
}

std::vector<std::uint8_t> ProbeBlock(std::string_view column_name) {
  std::vector<std::uint8_t> body;
  AppendString(&body, "");     // external table name
  AppendVarUInt(&body, 0);     // BlockInfo terminator
  AppendVarUInt(&body, 1);     // columns
  AppendVarUInt(&body, 1);     // rows
  AppendString(&body, column_name.empty() ? "sb_reference_probe" : column_name);
  AppendString(&body, "UInt8");
  AppendU8(&body, 1);
  return body;
}

std::string ResultColumnName(std::string_view query) {
  const auto upper = ToUpperAscii(query);
  const auto as_pos = upper.find(" AS ");
  if (as_pos == std::string::npos) return "sb_reference_probe";
  std::string tail = std::string(query.substr(as_pos + 4));
  tail = TrimAscii(tail);
  while (!tail.empty() && (tail.back() == ';' || tail.back() == '\n' || tail.back() == '\r')) {
    tail.pop_back();
  }
  tail = TrimAscii(tail);
  if (tail.empty()) return "sb_reference_probe";
  std::string out;
  for (char c : tail) {
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
        (c >= '0' && c <= '9') || c == '_') {
      out.push_back(c);
      continue;
    }
    break;
  }
  return out.empty() ? "sb_reference_probe" : out;
}

bool SendQueryResult(int fd, std::string_view query) {
  const auto parsed = ParseStatement(query);
  if (!parsed.ok) return SendException(fd, ExtractDiagnosticMessage(parsed.message_vector_json));
  if (!SendPacket(fd, kServerData, ProbeBlock(ResultColumnName(query)))) return false;
  return SendPacket(fd, kServerEndOfStream, {});
}

#ifndef _WIN32
bool HandlePacket(int fd, std::uint64_t packet) {
  if (packet == kClientPing) return SendPong(fd);
  if (packet != kClientQuery) {
    return SendException(fd, "unsupported ClickHouse native protocol packet");
  }
  std::string query;
  if (!ReadQueryPacket(fd, &query)) {
    return SendException(fd, "malformed ClickHouse query packet");
  }
  return SendQueryResult(fd, query);
}
#endif

} // namespace

int ServeClickhouseWorkerSession(int fd) {
#ifdef _WIN32
  (void)fd;
  return 1;
#else
  ClientHello hello;
  if (!ReadClientHello(fd, &hello)) return 1;
  if (!SendHello(fd)) return 1;
  for (;;) {
    std::uint64_t packet = 0;
    if (!ReadVarUInt(fd, &packet)) return 0;
    if (!HandlePacket(fd, packet)) return 1;
  }
#endif
}

} // namespace scratchbird::parser::clickhouse
