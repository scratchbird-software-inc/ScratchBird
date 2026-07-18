// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "xtdb_worker_session.hpp"

#include "pgwire_frame_codec.hpp"
#include "xtdb_dialect.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifndef _WIN32
#include <unistd.h>
#endif

namespace scratchbird::parser::xtdb {
namespace {

namespace wire = scratchbird::parser::compatibility::pgwire;

void AddErrorField(std::vector<std::uint8_t>* body, char field,
                   std::string_view value) {
  body->push_back(static_cast<std::uint8_t>(field));
  wire::AppendCString(body, value);
}

std::vector<std::uint8_t> MakeError(std::string_view code,
                                    std::string_view message) {
  std::vector<std::uint8_t> body;
  AddErrorField(&body, 'S', "ERROR");
  AddErrorField(&body, 'V', "ERROR");
  AddErrorField(&body, 'C', code);
  AddErrorField(&body, 'M', message);
  body.push_back(0);
  return body;
}

std::vector<std::uint8_t> MakeCString(std::string_view value) {
  std::vector<std::uint8_t> body;
  wire::AppendCString(&body, value);
  return body;
}

bool SendReady(int fd) {
  constexpr std::array<std::uint8_t, 1> idle{{'I'}};
  return wire::WriteTypedFrame(fd, 'Z', idle);
}

bool SendError(int fd, std::string_view code, std::string_view message) {
  return wire::WriteTypedFrame(fd, 'E', MakeError(code, message));
}

std::string ExtractMessage(std::string_view json) {
  constexpr std::string_view marker = "\"message\":\"";
  const auto marker_at = json.find(marker);
  if (marker_at == std::string_view::npos) return "XTDB parser refused the statement";
  const auto start = marker_at + marker.size();
  const auto end = json.find('"', start);
  return std::string(json.substr(start, end == std::string_view::npos
                                          ? std::string_view::npos
                                          : end - start));
}

bool IsProbe(std::string_view sql) {
  auto normalized = ToUpperAscii(NormalizeWhitespace(TrimAscii(sql)));
  while (!normalized.empty() && normalized.back() == ';') normalized.pop_back();
  return normalized == "SELECT 1" ||
         normalized == "SELECT 1 AS SB_REFERENCE_PROBE" ||
         normalized == "SELECT 1 AS SCRATCHBIRD_REFERENCE_PROBE";
}

bool SendProbeResult(int fd) {
  std::vector<std::uint8_t> description;
  wire::AppendBe16(&description, 1);
  wire::AppendCString(&description, "sb_reference_probe");
  wire::AppendBe32(&description, 0);
  wire::AppendBe16(&description, 0);
  wire::AppendBe32(&description, 25);
  wire::AppendBe16(&description, 0xffff);
  wire::AppendBe32(&description, 0xffffffffU);
  wire::AppendBe16(&description, 0);
  if (!wire::WriteTypedFrame(fd, 'T', description)) return false;

  std::vector<std::uint8_t> row;
  wire::AppendBe16(&row, 1);
  wire::AppendBe32(&row, 1);
  row.push_back('1');
  return wire::WriteTypedFrame(fd, 'D', row) &&
         wire::WriteTypedFrame(fd, 'C', MakeCString("SELECT 1")) &&
         SendReady(fd);
}

bool RenderXtdbResult(int fd, std::string_view input) {
  const auto result = ParseStatement(input);
  if (!result.ok || result.fail_closed_refusal) {
    return SendError(fd,
                     result.emulation_diagnostic_code.empty() ? "42601" : "0A000",
                     ExtractMessage(result.message_vector_json)) &&
           SendReady(fd);
  }
  if (IsProbe(input)) return SendProbeResult(fd);
  if (result.statement_family == "query" || result.statement_family == "datalog") {
    std::vector<std::uint8_t> empty_description;
    wire::AppendBe16(&empty_description, 0);
    return wire::WriteTypedFrame(fd, 'T', empty_description) &&
           wire::WriteTypedFrame(fd, 'C', MakeCString("SELECT 0")) &&
           SendReady(fd);
  }
  return wire::WriteTypedFrame(fd, 'C', MakeCString("XTDB PARSE")) && SendReady(fd);
}

bool AcceptStartup(int fd, bool* cancel_request) {
  for (;;) {
    wire::StartupFrame frame;
    if (!wire::ReadStartupFrame(fd, &frame)) return false;
    switch (frame.request_code) {
      case wire::kSslRequest:
      case wire::kGssEncryptionRequest:
        if (!wire::WriteByte(fd, 'N')) return false;
        break;
      case wire::kCancelRequest:
        *cancel_request = true;
        return true;
      case wire::kProtocolV3:
        return true;
      default:
        SendError(fd, "08P01", "unsupported XTDB wire startup protocol");
        return false;
    }
  }
}

bool SendStartupAccepted(int fd) {
  std::vector<std::uint8_t> authentication;
  wire::AppendBe32(&authentication, 0);
  if (!wire::WriteTypedFrame(fd, 'R', authentication)) return false;

  constexpr std::array<std::pair<std::string_view, std::string_view>, 7> statuses{{
      {"server_version", "2.1-scratchbird-xtdb"},
      {"server_encoding", "UTF8"},
      {"client_encoding", "UTF8"},
      {"DateStyle", "ISO, MDY"},
      {"integer_datetimes", "on"},
      {"standard_conforming_strings", "on"},
      {"TimeZone", "UTC"},
  }};
  for (const auto& [name, value] : statuses) {
    std::vector<std::uint8_t> body;
    wire::AppendCString(&body, name);
    wire::AppendCString(&body, value);
    if (!wire::WriteTypedFrame(fd, 'S', body)) return false;
  }

  std::vector<std::uint8_t> backend_key;
#ifdef _WIN32
  wire::AppendBe32(&backend_key, 0);
#else
  wire::AppendBe32(&backend_key, static_cast<std::uint32_t>(::getpid()));
#endif
  wire::AppendBe32(&backend_key, 0x53425854U);
  return wire::WriteTypedFrame(fd, 'K', backend_key) && SendReady(fd);
}

} // namespace

int ServeXtdbWorkerSession(int fd) {
#ifdef _WIN32
  (void)fd;
  return 1;
#else
  bool cancel_request = false;
  if (!AcceptStartup(fd, &cancel_request)) return 1;
  if (cancel_request) return 0;
  if (!SendStartupAccepted(fd)) return 1;

  for (;;) {
    wire::TypedFrame frame;
    if (!wire::ReadTypedFrame(fd, &frame)) return 0;
    if (frame.type == 'X') return 0;
    if (frame.type == 'Q') {
      if (!RenderXtdbResult(fd, wire::FirstCString(frame.body))) return 1;
    } else if (frame.type == 'S') {
      if (!SendReady(fd)) return 1;
    } else {
      if (!SendError(fd, "08P01", "unsupported XTDB frontend message") ||
          !SendReady(fd)) {
        return 1;
      }
    }
  }
#endif
}

} // namespace scratchbird::parser::xtdb
