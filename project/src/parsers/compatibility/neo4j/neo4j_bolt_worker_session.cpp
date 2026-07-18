// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "neo4j_bolt_worker_session.hpp"

#include "neo4j_dialect.hpp"

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#ifndef _WIN32
#include <unistd.h>
#endif

namespace scratchbird::parser::neo4j {
namespace {

constexpr std::uint8_t kTagHello = 0x01;
constexpr std::uint8_t kTagGoodbye = 0x02;
constexpr std::uint8_t kTagRun = 0x10;
constexpr std::uint8_t kTagPull = 0x3f;
constexpr std::uint8_t kTagLogon = 0x6a;
constexpr std::uint8_t kTagReset = 0x0f;
constexpr std::uint8_t kTagSuccess = 0x70;
constexpr std::uint8_t kTagRecord = 0x71;
constexpr std::uint8_t kTagFailure = 0x7f;
constexpr std::size_t kMaxBoltMessage = 16 * 1024 * 1024;

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

std::uint16_t ReadU16BE(const std::uint8_t* data) {
  return static_cast<std::uint16_t>((static_cast<std::uint16_t>(data[0]) << 8) |
                                    static_cast<std::uint16_t>(data[1]));
}

void AppendU16BE(std::vector<std::uint8_t>* out, std::uint16_t value) {
  out->push_back(static_cast<std::uint8_t>((value >> 8) & 0xff));
  out->push_back(static_cast<std::uint8_t>(value & 0xff));
}

void AppendU32BE(std::vector<std::uint8_t>* out, std::uint32_t value) {
  out->push_back(static_cast<std::uint8_t>((value >> 24) & 0xff));
  out->push_back(static_cast<std::uint8_t>((value >> 16) & 0xff));
  out->push_back(static_cast<std::uint8_t>((value >> 8) & 0xff));
  out->push_back(static_cast<std::uint8_t>(value & 0xff));
}

bool ReadMessage(int fd, std::vector<std::uint8_t>* message) {
  message->clear();
  for (;;) {
    std::uint8_t header[2] = {};
    if (!ReadExact(fd, header, sizeof(header))) return false;
    const auto size = ReadU16BE(header);
    if (size == 0) return true;
    if (message->size() + size > kMaxBoltMessage) return false;
    const auto old_size = message->size();
    message->resize(old_size + size);
    if (!ReadExact(fd, message->data() + old_size, size)) return false;
  }
}

bool WriteMessage(int fd, const std::vector<std::uint8_t>& message) {
  std::vector<std::uint8_t> out;
  out.reserve(message.size() + 4);
  std::size_t pos = 0;
  while (pos < message.size()) {
    const auto chunk_size = std::min<std::size_t>(message.size() - pos, 0xffff);
    AppendU16BE(&out, static_cast<std::uint16_t>(chunk_size));
    out.insert(out.end(), message.begin() + static_cast<std::ptrdiff_t>(pos),
               message.begin() + static_cast<std::ptrdiff_t>(pos + chunk_size));
    pos += chunk_size;
  }
  AppendU16BE(&out, 0);
  return WriteAll(fd, out.data(), out.size());
}

void AppendPackString(std::vector<std::uint8_t>* out, std::string_view value) {
  if (value.size() <= 15) {
    out->push_back(static_cast<std::uint8_t>(0x80 | value.size()));
  } else if (value.size() <= 0xff) {
    out->push_back(0xd0);
    out->push_back(static_cast<std::uint8_t>(value.size()));
  } else {
    out->push_back(0xd1);
    AppendU16BE(out, static_cast<std::uint16_t>(value.size()));
  }
  out->insert(out->end(), value.begin(), value.end());
}

void AppendTinyMapHeader(std::vector<std::uint8_t>* out, std::uint8_t size) {
  out->push_back(static_cast<std::uint8_t>(0xa0 | size));
}

void AppendTinyListHeader(std::vector<std::uint8_t>* out, std::uint8_t size) {
  out->push_back(static_cast<std::uint8_t>(0x90 | size));
}

void AppendStructHeader(std::vector<std::uint8_t>* out, std::uint8_t size, std::uint8_t tag) {
  out->push_back(static_cast<std::uint8_t>(0xb0 | size));
  out->push_back(tag);
}

std::vector<std::uint8_t> SuccessMessage(std::vector<std::pair<std::string_view,
                                                               std::vector<std::uint8_t>>> fields = {}) {
  std::vector<std::uint8_t> out;
  AppendStructHeader(&out, 1, kTagSuccess);
  AppendTinyMapHeader(&out, static_cast<std::uint8_t>(fields.size()));
  for (const auto& [key, value] : fields) {
    AppendPackString(&out, key);
    out.insert(out.end(), value.begin(), value.end());
  }
  return out;
}

std::vector<std::uint8_t> PackStringValue(std::string_view value) {
  std::vector<std::uint8_t> out;
  AppendPackString(&out, value);
  return out;
}

std::vector<std::uint8_t> PackFieldList(std::string_view field) {
  std::vector<std::uint8_t> out;
  AppendTinyListHeader(&out, 1);
  AppendPackString(&out, field);
  return out;
}

std::vector<std::uint8_t> PackFalse() {
  return {0xc2};
}

std::vector<std::uint8_t> RecordMessage() {
  std::vector<std::uint8_t> out;
  AppendStructHeader(&out, 1, kTagRecord);
  AppendTinyListHeader(&out, 1);
  out.push_back(1);
  return out;
}

std::string ExtractDiagnosticMessage(std::string_view json) {
  const std::string marker = "\"message\":\"";
  const auto pos = json.find(marker);
  if (pos == std::string_view::npos) return "Neo4j compatibility parser refused the query";
  const auto start = pos + marker.size();
  const auto end = json.find('"', start);
  if (end == std::string_view::npos) return std::string(json.substr(start));
  return std::string(json.substr(start, end - start));
}

std::vector<std::uint8_t> FailureMessage(std::string_view message) {
  std::vector<std::uint8_t> out;
  AppendStructHeader(&out, 1, kTagFailure);
  AppendTinyMapHeader(&out, 2);
  AppendPackString(&out, "code");
  AppendPackString(&out, "Neo.ClientError.Statement.SyntaxError");
  AppendPackString(&out, "message");
  AppendPackString(&out, message);
  return out;
}

bool ReadPackString(const std::vector<std::uint8_t>& message,
                    std::size_t* pos,
                    std::string* out) {
  if (*pos >= message.size()) return false;
  const auto marker = message[(*pos)++];
  std::size_t size = 0;
  if ((marker & 0xf0) == 0x80) {
    size = marker & 0x0f;
  } else if (marker == 0xd0) {
    if (*pos >= message.size()) return false;
    size = message[(*pos)++];
  } else if (marker == 0xd1) {
    if (*pos + 2 > message.size()) return false;
    size = ReadU16BE(message.data() + *pos);
    *pos += 2;
  } else if (marker == 0xd2) {
    if (*pos + 4 > message.size()) return false;
    size = (static_cast<std::size_t>(message[*pos]) << 24) |
           (static_cast<std::size_t>(message[*pos + 1]) << 16) |
           (static_cast<std::size_t>(message[*pos + 2]) << 8) |
           static_cast<std::size_t>(message[*pos + 3]);
    *pos += 4;
  } else {
    return false;
  }
  if (*pos + size > message.size()) return false;
  out->assign(reinterpret_cast<const char*>(message.data() + *pos), size);
  *pos += size;
  return true;
}

std::uint8_t MessageTag(const std::vector<std::uint8_t>& message) {
  if (message.size() < 2 || (message[0] & 0xf0) != 0xb0) return 0;
  return message[1];
}

std::string RunQuery(const std::vector<std::uint8_t>& message) {
  if (MessageTag(message) != kTagRun) return {};
  std::size_t pos = 2;
  std::string query;
  return ReadPackString(message, &pos, &query) ? query : std::string{};
}

std::string ResultField(std::string_view query) {
  const auto upper = ToUpperAscii(query);
  const auto as_pos = upper.find(" AS ");
  if (as_pos == std::string::npos) return "sb_reference_probe";
  std::string tail = TrimAscii(query.substr(as_pos + 4));
  while (!tail.empty() && tail.back() == ';') tail.pop_back();
  tail = TrimAscii(tail);
  std::string out;
  for (const char c : tail) {
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
        (c >= '0' && c <= '9') || c == '_') {
      out.push_back(c);
      continue;
    }
    break;
  }
  return out.empty() ? "sb_reference_probe" : out;
}

#ifndef _WIN32
bool HandleMessage(int fd,
                   const std::vector<std::uint8_t>& message,
                   std::string* pending_field) {
  const auto tag = MessageTag(message);
  if (tag == kTagHello) {
    return WriteMessage(fd, SuccessMessage({
                                {"server", PackStringValue("Neo4j/2026.06.0")},
                                {"connection_id", PackStringValue("bolt-1")},
                            }));
  }
  if (tag == kTagLogon || tag == kTagReset) {
    return WriteMessage(fd, SuccessMessage());
  }
  if (tag == kTagRun) {
    const auto query = RunQuery(message);
    const auto parsed = ParseStatement(query);
    if (!parsed.ok) {
      return WriteMessage(fd, FailureMessage(ExtractDiagnosticMessage(parsed.message_vector_json)));
    }
    *pending_field = ResultField(query);
    return WriteMessage(fd, SuccessMessage({{"fields", PackFieldList(*pending_field)}}));
  }
  if (tag == kTagPull) {
    if (!WriteMessage(fd, RecordMessage())) return false;
    return WriteMessage(fd, SuccessMessage({{"has_more", PackFalse()}}));
  }
  if (tag == kTagGoodbye) {
    WriteMessage(fd, SuccessMessage());
    return false;
  }
  return WriteMessage(fd, SuccessMessage());
}
#endif

} // namespace

int ServeNeo4jBoltWorkerSession(int fd) {
#ifdef _WIN32
  (void)fd;
  return 1;
#else
  std::uint8_t handshake[20] = {};
  if (!ReadExact(fd, handshake, sizeof(handshake))) return 1;
  if (handshake[0] != 0x60 || handshake[1] != 0x60 ||
      handshake[2] != 0xb0 || handshake[3] != 0x17) {
    return 1;
  }
  const std::uint8_t chosen_version[4] = {0x00, 0x00, 0x08, 0x05};
  if (!WriteAll(fd, chosen_version, sizeof(chosen_version))) return 1;

  std::string pending_field = "sb_reference_probe";
  for (;;) {
    std::vector<std::uint8_t> message;
    if (!ReadMessage(fd, &message)) return 0;
    if (message.empty()) continue;
    if (!HandleMessage(fd, message, &pending_field)) return 0;
  }
#endif
}

} // namespace scratchbird::parser::neo4j
