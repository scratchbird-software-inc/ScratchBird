// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "http_worker_session.hpp"

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cctype>
#include <sstream>
#include <string>
#include <string_view>

#ifndef _WIN32
#include <unistd.h>
#endif

namespace scratchbird::parser::compatibility {
namespace {

#ifndef _WIN32
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

bool ReadSome(int fd, std::string* buffer) {
  char chunk[4096] = {};
  const auto rc = ::read(fd, chunk, sizeof(chunk));
  if (rc > 0) {
    buffer->append(chunk, static_cast<std::size_t>(rc));
    return true;
  }
  return rc < 0 && errno == EINTR;
}
#endif

std::string ToLower(std::string_view value) {
  std::string out(value);
  std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return out;
}

std::size_t ContentLength(std::string_view headers) {
  std::istringstream input{std::string(headers)};
  std::string line;
  while (std::getline(input, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    const auto colon = line.find(':');
    if (colon == std::string::npos) continue;
    if (ToLower(std::string_view(line).substr(0, colon)) != "content-length") continue;
    const auto value_view = TrimAscii(std::string_view(line).substr(colon + 1));
    const std::string value(value_view);
    char* end = nullptr;
    const auto parsed = std::strtoull(value.c_str(), &end, 10);
    return end != nullptr && *end == '\0' ? static_cast<std::size_t>(parsed) : 0;
  }
  return 0;
}

std::string JsonEscape(std::string_view value) {
  std::string out;
  out.reserve(value.size() + 8);
  for (const char c : value) {
    switch (c) {
      case '\\': out += "\\\\"; break;
      case '"': out += "\\\""; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default: out.push_back(c); break;
    }
  }
  return out;
}

std::string ExtractDiagnosticMessage(std::string_view json) {
  const std::string marker = "\"message\":\"";
  const auto pos = json.find(marker);
  if (pos == std::string_view::npos) return "compatibility HTTP parser refused the request";
  const auto start = pos + marker.size();
  const auto end = json.find('"', start);
  if (end == std::string_view::npos) return std::string(json.substr(start));
  return std::string(json.substr(start, end - start));
}

std::string RequestLine(const std::string& request) {
  const auto end = request.find("\r\n");
  return end == std::string::npos ? request : request.substr(0, end);
}

std::string ParserText(const std::string& request, std::string_view body) {
  std::istringstream input(RequestLine(request));
  std::string method;
  std::string path;
  input >> method >> path;
  if (method.empty()) method = "GET";
  if (path.empty()) path = "/";
  std::string text = method + " " + path;
  if (!body.empty()) {
    text.push_back(' ');
    text.append(body);
  }
  return text;
}

#ifndef _WIN32
bool SendHttp(int fd, int status, std::string_view reason, std::string_view body) {
  std::ostringstream out;
  out << "HTTP/1.1 " << status << ' ' << reason << "\r\n"
      << "Content-Type: application/json\r\n"
      << "Content-Length: " << body.size() << "\r\n"
      << "Connection: close\r\n\r\n"
      << body;
  const auto response = out.str();
  return WriteAll(fd, response.data(), response.size());
}
#endif

} // namespace

int ServeHttpWorkerSession(int fd, HttpParseFn parse_statement) {
#ifdef _WIN32
  (void)fd;
  (void)parse_statement;
  return 1;
#else
  std::string request;
  std::size_t header_end = std::string::npos;
  while ((header_end = request.find("\r\n\r\n")) == std::string::npos) {
    if (!ReadSome(fd, &request)) return 1;
    if (request.size() > 1024 * 1024) return 1;
  }
  const auto body_start = header_end + 4;
  const auto expected_body = ContentLength(std::string_view(request).substr(0, header_end));
  while (request.size() < body_start + expected_body) {
    if (!ReadSome(fd, &request)) return 1;
    if (request.size() > 16 * 1024 * 1024) return 1;
  }

  const std::string_view body(request.data() + body_start, expected_body);
  const auto parser_text = ParserText(request, body);
  const auto parsed = parse_statement != nullptr ? parse_statement(parser_text) : ParseResult{};
  if (!parsed.ok) {
    const auto diagnostic = JsonEscape(ExtractDiagnosticMessage(parsed.message_vector_json));
    const auto response =
        std::string("{\"status\":\"error\",\"diagnostic\":\"") + diagnostic + "\"}\n";
    return SendHttp(fd, 400, "Bad Request", response) ? 0 : 1;
  }

  const auto response =
      std::string("{\"status\":\"ok\",\"scratchbird_reference_probe\":1,"
                  "\"authority\":\"parser_listener_only_engine_mga_final\"}\n");
  return SendHttp(fd, 200, "OK", response) ? 0 : 1;
#endif
}

} // namespace scratchbird::parser::compatibility
