// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "cockroachdb_worker_session.hpp"

#include "cockroachdb_dialect.hpp"
#include "pgwire_frame_codec.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifndef _WIN32
#include <unistd.h>
#endif

namespace scratchbird::parser::cockroachdb {
namespace {

namespace wire = scratchbird::parser::compatibility::pgwire;

std::vector<std::uint8_t> AuthenticationOk() {
  std::vector<std::uint8_t> body;
  wire::AppendBe32(&body, 0);
  return body;
}

std::vector<std::uint8_t> ParameterStatus(std::string_view name,
                                          std::string_view value) {
  std::vector<std::uint8_t> body;
  wire::AppendCString(&body, name);
  wire::AppendCString(&body, value);
  return body;
}

std::vector<std::uint8_t> BackendKey() {
  std::vector<std::uint8_t> body;
#ifdef _WIN32
  wire::AppendBe32(&body, 0);
#else
  wire::AppendBe32(&body, static_cast<std::uint32_t>(::getpid()));
#endif
  wire::AppendBe32(&body, 0x53424352U);
  return body;
}

std::vector<std::uint8_t> ErrorBody(std::string_view code,
                                    std::string_view message) {
  std::vector<std::uint8_t> body;
  body.push_back('S');
  wire::AppendCString(&body, "ERROR");
  body.push_back('V');
  wire::AppendCString(&body, "ERROR");
  body.push_back('C');
  wire::AppendCString(&body, code);
  body.push_back('M');
  wire::AppendCString(&body, message);
  body.push_back(0);
  return body;
}

std::vector<std::uint8_t> CommandComplete(std::string_view tag) {
  std::vector<std::uint8_t> body;
  wire::AppendCString(&body, tag);
  return body;
}

std::vector<std::uint8_t> ProbeRowDescription() {
  std::vector<std::uint8_t> body;
  wire::AppendBe16(&body, 1);
  wire::AppendCString(&body, "sb_reference_probe");
  wire::AppendBe32(&body, 0);
  wire::AppendBe16(&body, 0);
  wire::AppendBe32(&body, 25);
  wire::AppendBe16(&body, 0xffff);
  wire::AppendBe32(&body, 0xffffffffU);
  wire::AppendBe16(&body, 0);
  return body;
}

std::vector<std::uint8_t> ProbeDataRow() {
  std::vector<std::uint8_t> body;
  wire::AppendBe16(&body, 1);
  wire::AppendBe32(&body, 1);
  body.push_back('1');
  return body;
}

std::string DiagnosticMessage(std::string_view message_vector) {
  constexpr std::string_view marker = "\"message\":\"";
  const auto start = message_vector.find(marker);
  if (start == std::string_view::npos) {
    return "CockroachDB parser refused the statement";
  }
  const auto value_start = start + marker.size();
  const auto value_end = message_vector.find('"', value_start);
  return std::string(message_vector.substr(
      value_start, value_end == std::string_view::npos
                       ? std::string_view::npos
                       : value_end - value_start));
}

bool IsSingletonProbe(std::string_view sql) {
  auto normalized = ToUpperAscii(NormalizeWhitespace(TrimAscii(sql)));
  while (!normalized.empty() && normalized.back() == ';') normalized.pop_back();
  return normalized == "SELECT 1" ||
         normalized == "SELECT 1 AS SB_REFERENCE_PROBE" ||
         normalized == "SELECT 1 AS SCRATCHBIRD_REFERENCE_PROBE";
}

bool SendReady(int fd) {
  const std::array<std::uint8_t, 1> idle{{'I'}};
  return wire::WriteTypedFrame(fd, 'Z', idle);
}

bool SendStartup(int fd) {
  if (!wire::WriteTypedFrame(fd, 'R', AuthenticationOk())) return false;
  constexpr std::array<std::pair<std::string_view, std::string_view>, 7> parameters{{
      {"server_version", "25.2-scratchbird-cockroachdb"},
      {"server_encoding", "UTF8"},
      {"client_encoding", "UTF8"},
      {"DateStyle", "ISO, MDY"},
      {"integer_datetimes", "on"},
      {"standard_conforming_strings", "on"},
      {"TimeZone", "UTC"},
  }};
  for (const auto& [name, value] : parameters) {
    if (!wire::WriteTypedFrame(fd, 'S', ParameterStatus(name, value))) return false;
  }
  if (!wire::WriteTypedFrame(fd, 'K', BackendKey())) return false;
  return SendReady(fd);
}

bool RenderStatement(int fd, std::string_view sql) {
  const auto parsed = ParseStatement(sql);
  if (!parsed.ok || parsed.fail_closed_refusal) {
    const auto code = parsed.emulation_diagnostic_code.empty() ? "42601" : "0A000";
    if (!wire::WriteTypedFrame(
            fd, 'E', ErrorBody(code, DiagnosticMessage(parsed.message_vector_json)))) {
      return false;
    }
    return SendReady(fd);
  }
  if (IsSingletonProbe(sql)) {
    if (!wire::WriteTypedFrame(fd, 'T', ProbeRowDescription())) return false;
    if (!wire::WriteTypedFrame(fd, 'D', ProbeDataRow())) return false;
    if (!wire::WriteTypedFrame(fd, 'C', CommandComplete("SELECT 1"))) return false;
    return SendReady(fd);
  }
  if (parsed.statement_family == "query") {
    std::vector<std::uint8_t> empty_description;
    wire::AppendBe16(&empty_description, 0);
    if (!wire::WriteTypedFrame(fd, 'T', empty_description)) return false;
    if (!wire::WriteTypedFrame(fd, 'C', CommandComplete("SELECT 0"))) return false;
    return SendReady(fd);
  }
  if (!wire::WriteTypedFrame(fd, 'C', CommandComplete("COCKROACHDB PARSE"))) return false;
  return SendReady(fd);
}

bool AcceptStartup(int fd, bool* cancelled) {
  *cancelled = false;
  for (;;) {
    wire::StartupFrame startup;
    if (!wire::ReadStartupFrame(fd, &startup)) return false;
    if (startup.request_code == wire::kSslRequest ||
        startup.request_code == wire::kGssEncryptionRequest) {
      if (!wire::WriteByte(fd, 'N')) return false;
      continue;
    }
    if (startup.request_code == wire::kCancelRequest) {
      *cancelled = true;
      return true;
    }
    if (startup.request_code != wire::kProtocolV3) {
      wire::WriteTypedFrame(fd, 'E',
                            ErrorBody("08P01", "unsupported CockroachDB wire startup protocol"));
      return false;
    }
    return true;
  }
}

} // namespace

int ServeCockroachdbWorkerSession(int fd) {
#ifdef _WIN32
  (void)fd;
  return 1;
#else
  bool cancelled = false;
  if (!AcceptStartup(fd, &cancelled)) return 1;
  if (cancelled) return 0;
  if (!SendStartup(fd)) return 1;

  for (;;) {
    wire::TypedFrame frame;
    if (!wire::ReadTypedFrame(fd, &frame)) return 0;
    switch (frame.type) {
      case 'Q':
        if (!RenderStatement(fd, wire::FirstCString(frame.body))) return 1;
        break;
      case 'S':
        if (!SendReady(fd)) return 1;
        break;
      case 'X':
        return 0;
      default:
        if (!wire::WriteTypedFrame(
                fd, 'E', ErrorBody("08P01", "unsupported CockroachDB frontend message"))) {
          return 1;
        }
        if (!SendReady(fd)) return 1;
        break;
    }
  }
#endif
}

} // namespace scratchbird::parser::cockroachdb
