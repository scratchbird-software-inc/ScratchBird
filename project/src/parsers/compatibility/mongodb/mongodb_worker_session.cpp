// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "mongodb_worker_session.hpp"

#include "mongodb_dialect.hpp"

#include <algorithm>
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

namespace scratchbird::parser::mongodb {
namespace {

constexpr std::int32_t kOpReply = 1;
constexpr std::int32_t kOpQuery = 2004;
constexpr std::int32_t kOpMsg = 2013;

struct WireHeader {
  std::int32_t message_length{0};
  std::int32_t request_id{0};
  std::int32_t response_to{0};
  std::int32_t opcode{0};
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
#endif

std::int32_t ReadI32(const std::uint8_t* data) {
  std::int32_t out = 0;
  std::memcpy(&out, data, sizeof(out));
  return out;
}

void AppendI32(std::vector<std::uint8_t>* out, std::int32_t value) {
  for (int shift = 0; shift < 32; shift += 8) {
    out->push_back(static_cast<std::uint8_t>((static_cast<std::uint32_t>(value) >> shift) & 0xff));
  }
}

void AppendI64(std::vector<std::uint8_t>* out, std::int64_t value) {
  for (int shift = 0; shift < 64; shift += 8) {
    out->push_back(static_cast<std::uint8_t>((static_cast<std::uint64_t>(value) >> shift) & 0xff));
  }
}

void AppendDoubleRaw(std::vector<std::uint8_t>* out, double value) {
  std::uint64_t raw = 0;
  static_assert(sizeof(raw) == sizeof(value));
  std::memcpy(&raw, &value, sizeof(raw));
  for (int shift = 0; shift < 64; shift += 8) {
    out->push_back(static_cast<std::uint8_t>((raw >> shift) & 0xff));
  }
}

void AppendCString(std::vector<std::uint8_t>* out, std::string_view value) {
  out->insert(out->end(), value.begin(), value.end());
  out->push_back(0);
}

void AppendBsonDouble(std::vector<std::uint8_t>* out, std::string_view name, double value) {
  out->push_back(0x01);
  AppendCString(out, name);
  AppendDoubleRaw(out, value);
}

void AppendBsonString(std::vector<std::uint8_t>* out,
                      std::string_view name,
                      std::string_view value) {
  out->push_back(0x02);
  AppendCString(out, name);
  AppendI32(out, static_cast<std::int32_t>(value.size() + 1));
  out->insert(out->end(), value.begin(), value.end());
  out->push_back(0);
}

void AppendBsonDocument(std::vector<std::uint8_t>* out,
                        std::string_view name,
                        const std::vector<std::uint8_t>& document) {
  out->push_back(0x03);
  AppendCString(out, name);
  out->insert(out->end(), document.begin(), document.end());
}

void AppendBsonArray(std::vector<std::uint8_t>* out,
                     std::string_view name,
                     const std::vector<std::uint8_t>& array) {
  out->push_back(0x04);
  AppendCString(out, name);
  out->insert(out->end(), array.begin(), array.end());
}

void AppendBsonBool(std::vector<std::uint8_t>* out, std::string_view name, bool value) {
  out->push_back(0x08);
  AppendCString(out, name);
  out->push_back(value ? 1 : 0);
}

void AppendBsonI32(std::vector<std::uint8_t>* out, std::string_view name, std::int32_t value) {
  out->push_back(0x10);
  AppendCString(out, name);
  AppendI32(out, value);
}

void AppendBsonI64(std::vector<std::uint8_t>* out, std::string_view name, std::int64_t value) {
  out->push_back(0x12);
  AppendCString(out, name);
  AppendI64(out, value);
}

std::vector<std::uint8_t> BsonDocument(const std::vector<std::uint8_t>& elements) {
  std::vector<std::uint8_t> out;
  out.reserve(elements.size() + 5);
  AppendI32(&out, static_cast<std::int32_t>(elements.size() + 5));
  out.insert(out.end(), elements.begin(), elements.end());
  out.push_back(0);
  return out;
}

std::vector<std::uint8_t> EmptyArray() {
  return BsonDocument({});
}

std::string ToLower(std::string_view text) {
  std::string out(text);
  std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return out;
}

std::string ExtractFirstBsonKey(std::span<const std::uint8_t> document) {
  if (document.size() < 6) return {};
  const auto declared_size = ReadI32(document.data());
  if (declared_size < 5 || static_cast<std::size_t>(declared_size) > document.size()) return {};
  std::size_t pos = 5;
  while (pos < document.size() && document[pos] != 0) ++pos;
  if (pos <= 5 || pos >= document.size()) return {};
  return std::string(reinterpret_cast<const char*>(document.data() + 5), pos - 5);
}

std::span<const std::uint8_t> OpQueryDocument(const std::vector<std::uint8_t>& body) {
  if (body.size() < 4) return {};
  std::size_t pos = 4;
  while (pos < body.size() && body[pos] != 0) ++pos;
  if (pos >= body.size()) return {};
  pos += 1;
  if (pos + 8 > body.size()) return {};
  pos += 8;
  return std::span<const std::uint8_t>(body.data() + pos, body.size() - pos);
}

std::span<const std::uint8_t> OpMsgDocument(const std::vector<std::uint8_t>& body) {
  if (body.size() < 5 || body[4] != 0) return {};
  return std::span<const std::uint8_t>(body.data() + 5, body.size() - 5);
}

std::string ParserTextForCommand(std::string_view command) {
  const auto lower = ToLower(command);
  if (lower == "ismaster" || lower == "isMaster" || lower == "hello" ||
      lower == "ping" || lower == "buildinfo" || lower == "getparameter" ||
      lower == "atlasversion" || lower == "endsessions") {
    return "SERVERSTATUS";
  }
  if (lower == "currentop") return "CURRENTOP";
  if (lower == "aggregate") return "AGGREGATE {}";
  if (lower == "find") return "FIND {}";
  if (lower == "count") return "COUNT {}";
  if (lower == "distinct") return "DISTINCT {}";
  if (lower == "insert") return "INSERT {}";
  if (lower == "update") return "UPDATE {}";
  if (lower == "delete") return "DELETE {}";
  if (lower == "findandmodify") return "FINDANDMODIFY {}";
  if (lower == "createindexes") return "CREATEINDEXES {}";
  if (lower == "dropindexes") return "DROPINDEXES {}";
  if (lower == "create" || lower == "createcollection") return "CREATECOLLECTION {}";
  return std::string(command);
}

std::string ExtractDiagnosticMessage(std::string_view json) {
  const std::string marker = "\"message\":\"";
  const auto pos = json.find(marker);
  if (pos == std::string_view::npos) return "MongoDB compatibility parser refused the command";
  const auto start = pos + marker.size();
  const auto end = json.find('"', start);
  if (end == std::string_view::npos) return std::string(json.substr(start));
  return std::string(json.substr(start, end - start));
}

std::vector<std::uint8_t> CursorDocument(std::string_view collection) {
  std::vector<std::uint8_t> elements;
  AppendBsonI64(&elements, "id", 0);
  AppendBsonString(&elements, "ns", collection.empty() ? "default.$cmd" : collection);
  AppendBsonArray(&elements, "firstBatch", EmptyArray());
  return BsonDocument(elements);
}

std::vector<std::uint8_t> OkDocument(std::string_view command) {
  const auto lower = ToLower(command);
  std::vector<std::uint8_t> elements;
  AppendBsonDouble(&elements, "ok", 1.0);
  if (lower == "ismaster" || lower == "hello") {
    AppendBsonBool(&elements, "ismaster", true);
    AppendBsonBool(&elements, "isWritablePrimary", true);
    AppendBsonBool(&elements, "helloOk", true);
    AppendBsonI32(&elements, "minWireVersion", 0);
    AppendBsonI32(&elements, "maxWireVersion", 25);
    AppendBsonI32(&elements, "logicalSessionTimeoutMinutes", 30);
    AppendBsonI32(&elements, "maxBsonObjectSize", 16 * 1024 * 1024);
    AppendBsonI32(&elements, "maxMessageSizeBytes", 48 * 1000 * 1000);
    AppendBsonI32(&elements, "maxWriteBatchSize", 100000);
  } else if (lower == "buildinfo") {
    AppendBsonString(&elements, "version", "8.2.0-scratchbird-reference");
  } else if (lower == "aggregate" || lower == "find" || lower == "listcollections" ||
             lower == "listindexes") {
    AppendBsonDocument(&elements, "cursor", CursorDocument("default.scratchbird_reference_probe"));
  } else if (lower == "ping") {
    AppendBsonI32(&elements, "scratchbird_reference_probe", 1);
    AppendBsonString(&elements, "authority", "parser_listener_only_engine_mga_final");
  }
  return BsonDocument(elements);
}

std::vector<std::uint8_t> ErrorDocument(std::string_view message) {
  std::vector<std::uint8_t> elements;
  AppendBsonDouble(&elements, "ok", 0.0);
  AppendBsonI32(&elements, "code", 9);
  AppendBsonString(&elements, "codeName", "FailedToParse");
  AppendBsonString(&elements, "errmsg", message);
  return BsonDocument(elements);
}

std::vector<std::uint8_t> Header(std::int32_t request_id,
                                 std::int32_t response_to,
                                 std::int32_t opcode,
                                 std::size_t body_size) {
  std::vector<std::uint8_t> out;
  out.reserve(16 + body_size);
  AppendI32(&out, static_cast<std::int32_t>(16 + body_size));
  AppendI32(&out, request_id);
  AppendI32(&out, response_to);
  AppendI32(&out, opcode);
  return out;
}

#ifndef _WIN32
bool SendOpReply(int fd, const WireHeader& request, const std::vector<std::uint8_t>& document) {
  std::vector<std::uint8_t> body;
  AppendI32(&body, 0);
  AppendI64(&body, 0);
  AppendI32(&body, 0);
  AppendI32(&body, 1);
  body.insert(body.end(), document.begin(), document.end());
  auto packet = Header(request.request_id + 1000, request.request_id, kOpReply, body.size());
  packet.insert(packet.end(), body.begin(), body.end());
  return WriteAll(fd, packet.data(), packet.size());
}

bool SendOpMsg(int fd, const WireHeader& request, const std::vector<std::uint8_t>& document) {
  std::vector<std::uint8_t> body;
  AppendI32(&body, 0);
  body.push_back(0);
  body.insert(body.end(), document.begin(), document.end());
  auto packet = Header(request.request_id + 1000, request.request_id, kOpMsg, body.size());
  packet.insert(packet.end(), body.begin(), body.end());
  return WriteAll(fd, packet.data(), packet.size());
}

bool ReadHeader(int fd, WireHeader* header) {
  std::uint8_t bytes[16] = {};
  if (!ReadExact(fd, bytes, sizeof(bytes))) return false;
  header->message_length = ReadI32(bytes);
  header->request_id = ReadI32(bytes + 4);
  header->response_to = ReadI32(bytes + 8);
  header->opcode = ReadI32(bytes + 12);
  return header->message_length >= 16 && header->message_length <= 16 * 1024 * 1024;
}

bool HandleCommand(int fd,
                   const WireHeader& header,
                   std::string_view command,
                   bool op_msg_response) {
  const auto parser_text = ParserTextForCommand(command);
  const auto parsed = ParseStatement(parser_text);
  const auto document = parsed.ok ? OkDocument(command)
                                  : ErrorDocument(ExtractDiagnosticMessage(parsed.message_vector_json));
  return op_msg_response ? SendOpMsg(fd, header, document) : SendOpReply(fd, header, document);
}
#endif

} // namespace

int ServeMongodbWorkerSession(int fd) {
#ifdef _WIN32
  (void)fd;
  return 1;
#else
  for (;;) {
    WireHeader header;
    if (!ReadHeader(fd, &header)) return 0;
    const auto body_size = static_cast<std::size_t>(header.message_length - 16);
    std::vector<std::uint8_t> body(body_size);
    if (body_size != 0 && !ReadExact(fd, body.data(), body.size())) return 1;

    if (header.opcode == kOpQuery) {
      const auto command = ExtractFirstBsonKey(OpQueryDocument(body));
      if (!HandleCommand(fd, header, command.empty() ? "serverStatus" : command, false)) return 1;
      continue;
    }
    if (header.opcode == kOpMsg) {
      const auto command = ExtractFirstBsonKey(OpMsgDocument(body));
      if (!HandleCommand(fd, header, command.empty() ? "serverStatus" : command, true)) return 1;
      continue;
    }
    return 1;
  }
#endif
}

} // namespace scratchbird::parser::mongodb
