// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "apache_ignite_thin_worker_session.hpp"

#include "apache_ignite_dialect.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#ifndef _WIN32
#include <unistd.h>
#endif

namespace scratchbird::parser::apache_ignite {
namespace {

constexpr std::uint8_t kIgniteHandshake = 1;
constexpr std::uint8_t kJdbcClient = 1;
constexpr std::uint8_t kJdbcQueryExecute = 2;
constexpr std::uint8_t kJdbcQueryFetch = 3;
constexpr std::uint8_t kJdbcQueryClose = 4;
constexpr std::uint8_t kJdbcQueryMetadata = 5;
constexpr std::uint8_t kJdbcMetaTables = 7;
constexpr std::uint8_t kJdbcMetaColumns = 8;
constexpr std::uint8_t kJdbcMetaIndexes = 9;
constexpr std::uint8_t kJdbcMetaParams = 10;
constexpr std::uint8_t kJdbcMetaPrimaryKeys = 11;
constexpr std::uint8_t kJdbcMetaSchemas = 12;

constexpr std::uint8_t kBinaryInt = 3;
constexpr std::uint8_t kBinaryString = 9;
constexpr std::uint8_t kBinaryUuid = 10;
constexpr std::uint8_t kBinaryByteArray = 12;
constexpr std::uint8_t kBinaryNull = 101;

constexpr std::int32_t kJdbcStatusSuccess = 0;
constexpr std::int32_t kJdbcStatusFailed = 1;
constexpr std::int64_t kCursorId = 0x534249474e495445LL;

#ifndef _WIN32
bool ReadExact(int fd, void* out, std::size_t size) {
  auto* bytes = static_cast<std::uint8_t*>(out);
  std::size_t total = 0;
  while (total < size) {
    const auto rc = ::read(fd, bytes + total, size - total);
    if (rc > 0) {
      total += static_cast<std::size_t>(rc);
      continue;
    }
    if (rc < 0 && errno == EINTR) continue;
    return false;
  }
  return true;
}

bool WriteAll(int fd, const void* data, std::size_t size) {
  const auto* bytes = static_cast<const std::uint8_t*>(data);
  std::size_t total = 0;
  while (total < size) {
    const auto rc = ::write(fd, bytes + total, size - total);
    if (rc > 0) {
      total += static_cast<std::size_t>(rc);
      continue;
    }
    if (rc < 0 && errno == EINTR) continue;
    return false;
  }
  return true;
}
#endif

void AppendU8(std::vector<std::uint8_t>* out, std::uint8_t value) {
  out->push_back(value);
}

void AppendBool(std::vector<std::uint8_t>* out, bool value) {
  out->push_back(value ? 1 : 0);
}

void AppendI32(std::vector<std::uint8_t>* out, std::int32_t value) {
  const auto raw = static_cast<std::uint32_t>(value);
  for (int shift = 0; shift < 32; shift += 8) {
    out->push_back(static_cast<std::uint8_t>((raw >> shift) & 0xff));
  }
}

void AppendI64(std::vector<std::uint8_t>* out, std::int64_t value) {
  const auto raw = static_cast<std::uint64_t>(value);
  for (int shift = 0; shift < 64; shift += 8) {
    out->push_back(static_cast<std::uint8_t>((raw >> shift) & 0xff));
  }
}

void AppendBinaryString(std::vector<std::uint8_t>* out, std::string_view value) {
  out->push_back(kBinaryString);
  AppendI32(out, static_cast<std::int32_t>(value.size()));
  out->insert(out->end(), value.begin(), value.end());
}

void AppendBinaryByteArray(std::vector<std::uint8_t>* out, std::string_view value) {
  out->push_back(kBinaryByteArray);
  AppendI32(out, static_cast<std::int32_t>(value.size()));
  out->insert(out->end(), value.begin(), value.end());
}

void AppendBinaryUuid(std::vector<std::uint8_t>* out,
                      std::uint64_t most,
                      std::uint64_t least) {
  out->push_back(kBinaryUuid);
  AppendI64(out, static_cast<std::int64_t>(most));
  AppendI64(out, static_cast<std::int64_t>(least));
}

void AppendBinaryInt(std::vector<std::uint8_t>* out, std::int32_t value) {
  out->push_back(kBinaryInt);
  AppendI32(out, value);
}

std::uint32_t ReadU32(const std::uint8_t* data) {
  return static_cast<std::uint32_t>(data[0]) |
         (static_cast<std::uint32_t>(data[1]) << 8) |
         (static_cast<std::uint32_t>(data[2]) << 16) |
         (static_cast<std::uint32_t>(data[3]) << 24);
}

std::int32_t ReadI32(const std::vector<std::uint8_t>& data, std::size_t* pos) {
  if (*pos + 4 > data.size()) {
    *pos = data.size();
    return 0;
  }
  const auto raw = static_cast<std::uint32_t>(data[*pos]) |
                   (static_cast<std::uint32_t>(data[*pos + 1]) << 8) |
                   (static_cast<std::uint32_t>(data[*pos + 2]) << 16) |
                   (static_cast<std::uint32_t>(data[*pos + 3]) << 24);
  *pos += 4;
  return static_cast<std::int32_t>(raw);
}

std::int64_t ReadI64(const std::vector<std::uint8_t>& data, std::size_t* pos) {
  if (*pos + 8 > data.size()) {
    *pos = data.size();
    return 0;
  }
  std::uint64_t raw = 0;
  for (int shift = 0; shift < 64; shift += 8) {
    raw |= static_cast<std::uint64_t>(data[*pos + static_cast<std::size_t>(shift / 8)]) << shift;
  }
  *pos += 8;
  return static_cast<std::int64_t>(raw);
}

std::uint8_t ReadByte(const std::vector<std::uint8_t>& data, std::size_t* pos) {
  if (*pos >= data.size()) return 0;
  return data[(*pos)++];
}

std::string ReadBinaryString(const std::vector<std::uint8_t>& data, std::size_t* pos) {
  const auto type = ReadByte(data, pos);
  if (type == kBinaryNull) return {};
  if (type != kBinaryString) {
    *pos = data.size();
    return {};
  }
  const auto len = ReadI32(data, pos);
  if (len <= 0) return {};
  if (*pos + static_cast<std::size_t>(len) > data.size()) {
    *pos = data.size();
    return {};
  }
  std::string value(reinterpret_cast<const char*>(data.data() + *pos),
                    static_cast<std::size_t>(len));
  *pos += static_cast<std::size_t>(len);
  return value;
}

std::string NormalizeSql(std::string value) {
  auto is_space = [](unsigned char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; };
  while (!value.empty() && is_space(static_cast<unsigned char>(value.front()))) value.erase(value.begin());
  while (!value.empty() && (is_space(static_cast<unsigned char>(value.back())) || value.back() == ';')) {
    value.pop_back();
  }
  std::string out;
  bool in_space = false;
  for (char ch : value) {
    if (is_space(static_cast<unsigned char>(ch))) {
      if (!in_space) out.push_back(' ');
      in_space = true;
      continue;
    }
    out.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(ch))));
    in_space = false;
  }
  return out;
}

std::string ExtractDiagnosticMessage(std::string_view json) {
  const std::string marker = "\"message\":\"";
  const auto pos = json.find(marker);
  if (pos == std::string_view::npos) return "Apache Ignite compatibility parser refused the statement";
  const auto start = pos + marker.size();
  const auto end = json.find('"', start);
  if (end == std::string_view::npos) return std::string(json.substr(start));
  return std::string(json.substr(start, end - start));
}

bool ReadFrame(int fd, std::vector<std::uint8_t>* payload) {
  std::uint8_t len_bytes[4] = {};
  if (!ReadExact(fd, len_bytes, sizeof(len_bytes))) return false;
  const auto len = ReadU32(len_bytes);
  if (len > 16 * 1024 * 1024) return false;
  payload->assign(len, 0);
  if (len != 0 && !ReadExact(fd, payload->data(), payload->size())) return false;
  return true;
}

bool WriteFrame(int fd, const std::vector<std::uint8_t>& payload) {
  std::uint8_t len_bytes[4] = {
      static_cast<std::uint8_t>(payload.size() & 0xff),
      static_cast<std::uint8_t>((payload.size() >> 8) & 0xff),
      static_cast<std::uint8_t>((payload.size() >> 16) & 0xff),
      static_cast<std::uint8_t>((payload.size() >> 24) & 0xff),
  };
  return WriteAll(fd, len_bytes, sizeof(len_bytes)) &&
         (payload.empty() || WriteAll(fd, payload.data(), payload.size()));
}

std::vector<std::uint8_t> HandshakeAcceptedPayload() {
  std::vector<std::uint8_t> out;
  AppendBool(&out, true);
  AppendU8(&out, 2);
  AppendU8(&out, 17);
  AppendU8(&out, 0);
  AppendBinaryString(&out, "scratchbird");
  AppendI64(&out, 0);
  AppendBinaryByteArray(&out, std::string(20, '\0'));
  AppendBinaryUuid(&out, 0x534249474e495445ULL, 0x2026070900000001ULL);
  AppendBinaryByteArray(&out, "");
  return out;
}

std::vector<std::uint8_t> JdbcSuccessPrefix(bool has_result) {
  std::vector<std::uint8_t> out;
  AppendI32(&out, kJdbcStatusSuccess);
  AppendBool(&out, has_result);
  return out;
}

void AppendAffinityTrailer(std::vector<std::uint8_t>* out) {
  AppendBool(out, false);
  AppendBool(out, false);
}

std::vector<std::uint8_t> JdbcFailurePayload(std::string_view message) {
  std::vector<std::uint8_t> out;
  AppendI32(&out, kJdbcStatusFailed);
  AppendBinaryString(&out, message);
  AppendAffinityTrailer(&out);
  return out;
}

std::vector<std::uint8_t> QueryExecuteSelectOnePayload() {
  auto out = JdbcSuccessPrefix(true);
  AppendU8(&out, kJdbcQueryExecute);
  AppendI64(&out, kCursorId);
  AppendBool(&out, true);
  AppendBool(&out, true);
  AppendI32(&out, 1);
  AppendI32(&out, 1);
  AppendBinaryInt(&out, 1);
  AppendBool(&out, false);
  AppendAffinityTrailer(&out);
  return out;
}

std::vector<std::uint8_t> QueryExecuteEmptySelectPayload() {
  auto out = JdbcSuccessPrefix(true);
  AppendU8(&out, kJdbcQueryExecute);
  AppendI64(&out, kCursorId);
  AppendBool(&out, true);
  AppendBool(&out, true);
  AppendI32(&out, 0);
  AppendBool(&out, false);
  AppendAffinityTrailer(&out);
  return out;
}

std::vector<std::uint8_t> QueryUpdateOkPayload() {
  auto out = JdbcSuccessPrefix(true);
  AppendU8(&out, kJdbcQueryExecute);
  AppendI64(&out, kCursorId);
  AppendBool(&out, false);
  AppendI64(&out, 0);
  AppendBool(&out, false);
  AppendAffinityTrailer(&out);
  return out;
}

void AppendColumnMeta(std::vector<std::uint8_t>* out,
                      std::string_view schema,
                      std::string_view table,
                      std::string_view column,
                      std::int32_t data_type,
                      std::string_view type_name,
                      std::string_view class_name) {
  AppendBinaryString(out, schema);
  AppendBinaryString(out, table);
  AppendBinaryString(out, column);
  AppendI32(out, data_type);
  AppendBinaryString(out, type_name);
  AppendBinaryString(out, class_name);
}

std::vector<std::uint8_t> QueryMetadataPayload() {
  auto out = JdbcSuccessPrefix(true);
  AppendU8(&out, kJdbcQueryMetadata);
  AppendI32(&out, 1);
  AppendColumnMeta(&out, "PUBLIC", "", "SB_REFERENCE_PROBE", 4, "INTEGER", "java.lang.Integer");
  AppendAffinityTrailer(&out);
  return out;
}

std::vector<std::uint8_t> EmptyMetadataPayload(std::uint8_t result_type) {
  auto out = JdbcSuccessPrefix(true);
  AppendU8(&out, result_type);
  AppendI32(&out, 0);
  AppendAffinityTrailer(&out);
  return out;
}

std::vector<std::uint8_t> FetchPayload() {
  auto out = JdbcSuccessPrefix(true);
  AppendU8(&out, kJdbcQueryFetch);
  AppendBool(&out, true);
  AppendI32(&out, 0);
  AppendAffinityTrailer(&out);
  return out;
}

std::vector<std::uint8_t> NullSuccessPayload() {
  auto out = JdbcSuccessPrefix(false);
  AppendAffinityTrailer(&out);
  return out;
}

std::string ReadQueryExecuteSql(const std::vector<std::uint8_t>& frame) {
  std::size_t pos = 1;
  ReadI64(frame, &pos);
  (void)ReadBinaryString(frame, &pos);
  (void)ReadI32(frame, &pos);
  (void)ReadI32(frame, &pos);
  return ReadBinaryString(frame, &pos);
}

std::vector<std::uint8_t> ExecuteQueryPayload(std::string_view sql,
                                              ApacheIgniteThinParseFn parse_statement) {
  const auto parsed = parse_statement != nullptr ? parse_statement(sql) : ParseStatement(sql);
  if (!parsed.ok) return JdbcFailurePayload(ExtractDiagnosticMessage(parsed.message_vector_json));

  const auto normalized = NormalizeSql(std::string(sql));
  if (normalized == "SELECT 1" || normalized == "SELECT 1 AS SB_REFERENCE_PROBE" ||
      normalized == "SELECT 1 AS SCRATCHBIRD_REFERENCE_PROBE") {
    return QueryExecuteSelectOnePayload();
  }
  if (parsed.statement_family == "query") return QueryExecuteEmptySelectPayload();
  return QueryUpdateOkPayload();
}

} // namespace

int ServeApacheIgniteThinWorkerSession(int client_fd, ApacheIgniteThinParseFn parse_statement) {
#ifdef _WIN32
  (void)client_fd;
  (void)parse_statement;
  return 1;
#else
  std::vector<std::uint8_t> frame;
  if (!ReadFrame(client_fd, &frame) || frame.empty() || frame.front() != kIgniteHandshake) {
    return 1;
  }
  if (frame.size() >= 8 && frame[7] != kJdbcClient) return 1;
  if (!WriteFrame(client_fd, HandshakeAcceptedPayload())) return 1;

  while (ReadFrame(client_fd, &frame)) {
    if (frame.empty()) continue;
    const auto request_type = frame.front();
    std::vector<std::uint8_t> response;
    switch (request_type) {
      case kJdbcQueryExecute:
        response = ExecuteQueryPayload(ReadQueryExecuteSql(frame), parse_statement);
        break;
      case kJdbcQueryMetadata:
        response = QueryMetadataPayload();
        break;
      case kJdbcQueryFetch:
        response = FetchPayload();
        break;
      case kJdbcQueryClose:
        response = NullSuccessPayload();
        break;
      case kJdbcMetaTables:
      case kJdbcMetaColumns:
      case kJdbcMetaIndexes:
      case kJdbcMetaParams:
      case kJdbcMetaPrimaryKeys:
      case kJdbcMetaSchemas:
        response = EmptyMetadataPayload(request_type);
        break;
      default:
        response = JdbcFailurePayload("Unsupported Apache Ignite JDBC Thin request in ScratchBird replay worker");
        break;
    }
    if (!WriteFrame(client_fd, response)) return 1;
  }
  return 0;
#endif
}

} // namespace scratchbird::parser::apache_ignite
