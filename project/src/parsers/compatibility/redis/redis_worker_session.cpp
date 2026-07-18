// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "redis_worker_session.hpp"

#include "redis_dialect.hpp"

#include <cerrno>
#include <cctype>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

#ifndef _WIN32
#include <unistd.h>
#endif

namespace scratchbird::parser::redis {
namespace {

#ifndef _WIN32
bool ReadByte(int fd, char* ch) {
  for (;;) {
    const auto rc = ::read(fd, ch, 1);
    if (rc == 1) return true;
    if (rc < 0 && errno == EINTR) continue;
    return false;
  }
}

bool WriteAll(int fd, std::string_view text) {
  std::size_t written = 0;
  while (written < text.size()) {
    const auto rc = ::write(fd, text.data() + written, text.size() - written);
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

std::string ToUpper(std::string_view text) {
  std::string out(text);
  for (char& ch : out) {
    ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
  }
  return out;
}

std::string EscapeRespError(std::string_view value) {
  std::string out;
  out.reserve(value.size());
  for (const char ch : value) {
    if (ch == '\r' || ch == '\n') {
      out.push_back(' ');
    } else {
      out.push_back(ch);
    }
  }
  return out;
}

std::string ExtractDiagnosticMessage(std::string_view json) {
  const std::string marker = "\"message\":\"";
  const auto pos = json.find(marker);
  if (pos == std::string_view::npos) return "Redis compatibility parser refused the command";
  const auto start = pos + marker.size();
  const auto end = json.find('"', start);
  if (end == std::string_view::npos) return std::string(json.substr(start));
  return std::string(json.substr(start, end - start));
}

std::string ParserTextForCommand(const std::vector<std::string>& parts) {
  if (parts.empty()) return {};
  std::string text;
  for (const auto& part : parts) {
    if (!text.empty()) text.push_back(' ');
    text.append(part);
  }
  return text;
}

#ifndef _WIN32
bool ReadLine(int fd, std::string* line) {
  line->clear();
  char ch = 0;
  for (;;) {
    if (!ReadByte(fd, &ch)) return false;
    if (ch == '\n') {
      if (!line->empty() && line->back() == '\r') line->pop_back();
      return true;
    }
    if (line->size() > 16 * 1024 * 1024) return false;
    line->push_back(ch);
  }
}

bool ReadBulkString(int fd, std::string* out) {
  std::string len_line;
  if (!ReadLine(fd, &len_line)) return false;
  char* end = nullptr;
  const auto len = std::strtoll(len_line.c_str(), &end, 10);
  if (end == nullptr || *end != '\0' || len < 0 || len > 16 * 1024 * 1024) return false;
  out->assign(static_cast<std::size_t>(len), '\0');
  std::size_t read_total = 0;
  while (read_total < out->size()) {
    const auto rc = ::read(fd, out->data() + read_total, out->size() - read_total);
    if (rc > 0) {
      read_total += static_cast<std::size_t>(rc);
      continue;
    }
    if (rc < 0 && errno == EINTR) continue;
    return false;
  }
  char cr = 0;
  char lf = 0;
  return ReadByte(fd, &cr) && ReadByte(fd, &lf) && cr == '\r' && lf == '\n';
}

bool ReadRespCommand(int fd, std::vector<std::string>* parts) {
  parts->clear();
  char prefix = 0;
  if (!ReadByte(fd, &prefix)) return false;
  if (prefix != '*') {
    std::string rest;
    if (!ReadLine(fd, &rest)) return false;
    std::string inline_command(1, prefix);
    inline_command += rest;
    std::size_t start = 0;
    while (start < inline_command.size()) {
      while (start < inline_command.size() &&
             std::isspace(static_cast<unsigned char>(inline_command[start]))) {
        ++start;
      }
      std::size_t end = start;
      while (end < inline_command.size() &&
             !std::isspace(static_cast<unsigned char>(inline_command[end]))) {
        ++end;
      }
      if (end > start) parts->push_back(inline_command.substr(start, end - start));
      start = end;
    }
    return !parts->empty();
  }

  std::string count_line;
  if (!ReadLine(fd, &count_line)) return false;
  char* end = nullptr;
  const auto count = std::strtoll(count_line.c_str(), &end, 10);
  if (end == nullptr || *end != '\0' || count <= 0 || count > 1024) return false;
  for (long long i = 0; i < count; ++i) {
    char bulk = 0;
    if (!ReadByte(fd, &bulk) || bulk != '$') return false;
    std::string part;
    if (!ReadBulkString(fd, &part)) return false;
    parts->push_back(std::move(part));
  }
  return true;
}

bool SendBulk(int fd, std::string_view value) {
  return WriteAll(fd, "$" + std::to_string(value.size()) + "\r\n" + std::string(value) + "\r\n");
}

bool SendArrayOfBulks(int fd, const std::vector<std::string_view>& values) {
  if (!WriteAll(fd, "*" + std::to_string(values.size()) + "\r\n")) return false;
  for (const auto value : values) {
    if (!SendBulk(fd, value)) return false;
  }
  return true;
}

bool SendError(int fd, std::string_view message) {
  return WriteAll(fd, "-ERR " + EscapeRespError(message) + "\r\n");
}

bool HandleCommand(int fd, const std::vector<std::string>& parts) {
  if (parts.empty()) return SendError(fd, "empty Redis command");
  const auto command = ToUpper(parts[0]);
  const auto parser_text = ParserTextForCommand(parts);
  const auto parsed = ParseStatement(parser_text);
  if (!parsed.ok) return SendError(fd, ExtractDiagnosticMessage(parsed.message_vector_json));

  if (command == "PING") return WriteAll(fd, "+PONG\r\n");
  if (command == "HELLO") {
    return SendArrayOfBulks(fd, {"server", "scratchbird-reference-redis-parser",
                                 "proto", "3", "mode", "standalone", "role", "master"});
  }
  if (command == "INFO") {
    return SendBulk(fd,
                    "# Server\r\nredis_version:8.6.2-scratchbird-reference\r\n"
                    "# ScratchBird\r\nparser_listener_only_engine_mga_final:1\r\n");
  }
  if (command == "COMMAND") return WriteAll(fd, "*0\r\n");
  if (command == "SELECT") return WriteAll(fd, "+OK\r\n");
  if (command == "GET") return "$-1\r\n";
  if (command == "DBSIZE") return WriteAll(fd, ":0\r\n");
  if (command == "SCAN") return SendArrayOfBulks(fd, {"0"});
  return WriteAll(fd, "+OK\r\n");
}
#endif

} // namespace

int ServeRedisWorkerSession(int fd) {
#ifdef _WIN32
  (void)fd;
  return 1;
#else
  for (;;) {
    std::vector<std::string> parts;
    if (!ReadRespCommand(fd, &parts)) return 0;
    if (!HandleCommand(fd, parts)) return 1;
  }
#endif
}

} // namespace scratchbird::parser::redis
