// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "yugabytedb_worker_session.hpp"

#include "pgwire_frame_codec.hpp"
#include "yugabytedb_dialect.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifndef _WIN32
#include <unistd.h>
#endif

namespace scratchbird::parser::yugabytedb {
namespace {

namespace wire = scratchbird::parser::compatibility::pgwire;

class YugabytedbSession final {
 public:
  explicit YugabytedbSession(int fd) : fd_(fd) {}

  int Run() {
#ifdef _WIN32
    return 1;
#else
    if (!ReadStartup()) return startup_cancelled_ ? 0 : 1;
    if (!SendStartupAccepted()) return 1;
    for (;;) {
      wire::TypedFrame frame;
      if (!wire::ReadTypedFrame(fd_, &frame)) return 0;
      if (frame.type == 'X') return 0;
      if (frame.type == 'Q') {
        if (!Render(wire::FirstCString(frame.body))) return 1;
        continue;
      }
      if (frame.type == 'S') {
        if (!SendReady()) return 1;
        continue;
      }
      if (!SendError("08P01", "unsupported YugabyteDB frontend message") ||
          !SendReady()) {
        return 1;
      }
    }
#endif
  }

 private:
  static std::vector<std::uint8_t> CStringBody(std::string_view value) {
    std::vector<std::uint8_t> body;
    wire::AppendCString(&body, value);
    return body;
  }

  static std::vector<std::uint8_t> StatusBody(std::string_view name,
                                              std::string_view value) {
    std::vector<std::uint8_t> body;
    wire::AppendCString(&body, name);
    wire::AppendCString(&body, value);
    return body;
  }

  static std::vector<std::uint8_t> ErrorResponse(std::string_view code,
                                                 std::string_view message) {
    std::vector<std::uint8_t> body;
    for (const auto& [field, value] :
         std::array<std::pair<char, std::string_view>, 4>{{
             {'S', "ERROR"}, {'V', "ERROR"}, {'C', code}, {'M', message}}}) {
      body.push_back(static_cast<std::uint8_t>(field));
      wire::AppendCString(&body, value);
    }
    body.push_back(0);
    return body;
  }

  static std::string ParserMessage(std::string_view json) {
    constexpr std::string_view marker = "\"message\":\"";
    const auto marker_at = json.find(marker);
    if (marker_at == std::string_view::npos) {
      return "YugabyteDB parser refused the statement";
    }
    const auto begin = marker_at + marker.size();
    const auto end = json.find('"', begin);
    return std::string(json.substr(begin, end == std::string_view::npos
                                             ? std::string_view::npos
                                             : end - begin));
  }

  static bool SingletonProbe(std::string_view sql) {
    auto text = ToUpperAscii(NormalizeWhitespace(TrimAscii(sql)));
    while (!text.empty() && text.back() == ';') text.pop_back();
    return text == "SELECT 1" || text == "SELECT 1 AS SB_REFERENCE_PROBE" ||
           text == "SELECT 1 AS SCRATCHBIRD_REFERENCE_PROBE";
  }

  bool ReadStartup() {
    for (;;) {
      wire::StartupFrame frame;
      if (!wire::ReadStartupFrame(fd_, &frame)) return false;
      if (frame.request_code == wire::kSslRequest ||
          frame.request_code == wire::kGssEncryptionRequest) {
        if (!wire::WriteByte(fd_, 'N')) return false;
        continue;
      }
      if (frame.request_code == wire::kCancelRequest) {
        startup_cancelled_ = true;
        return false;
      }
      if (frame.request_code != wire::kProtocolV3) {
        SendError("08P01", "unsupported YugabyteDB wire startup protocol");
        return false;
      }
      return true;
    }
  }

  bool SendStartupAccepted() {
    std::vector<std::uint8_t> authentication;
    wire::AppendBe32(&authentication, 0);
    if (!wire::WriteTypedFrame(fd_, 'R', authentication)) return false;
    constexpr std::array<std::pair<std::string_view, std::string_view>, 7> statuses{{
        {"server_version", "15.12-scratchbird-yugabytedb"},
        {"server_encoding", "UTF8"},
        {"client_encoding", "UTF8"},
        {"DateStyle", "ISO, MDY"},
        {"integer_datetimes", "on"},
        {"standard_conforming_strings", "on"},
        {"TimeZone", "UTC"},
    }};
    for (const auto& [name, value] : statuses) {
      if (!wire::WriteTypedFrame(fd_, 'S', StatusBody(name, value))) return false;
    }
    std::vector<std::uint8_t> backend_key;
#ifdef _WIN32
    wire::AppendBe32(&backend_key, 0);
#else
    wire::AppendBe32(&backend_key, static_cast<std::uint32_t>(::getpid()));
#endif
    wire::AppendBe32(&backend_key, 0x53425942U);
    if (!wire::WriteTypedFrame(fd_, 'K', backend_key)) return false;
    return SendReady();
  }

  bool SendReady() {
    const std::array<std::uint8_t, 1> idle{{'I'}};
    return wire::WriteTypedFrame(fd_, 'Z', idle);
  }

  bool SendError(std::string_view code, std::string_view message) {
    return wire::WriteTypedFrame(fd_, 'E', ErrorResponse(code, message));
  }

  bool SendProbeRow() {
    std::vector<std::uint8_t> description;
    wire::AppendBe16(&description, 1);
    wire::AppendCString(&description, "sb_reference_probe");
    wire::AppendBe32(&description, 0);
    wire::AppendBe16(&description, 0);
    wire::AppendBe32(&description, 25);
    wire::AppendBe16(&description, 0xffff);
    wire::AppendBe32(&description, 0xffffffffU);
    wire::AppendBe16(&description, 0);
    if (!wire::WriteTypedFrame(fd_, 'T', description)) return false;

    std::vector<std::uint8_t> row;
    wire::AppendBe16(&row, 1);
    wire::AppendBe32(&row, 1);
    row.push_back('1');
    return wire::WriteTypedFrame(fd_, 'D', row) &&
           wire::WriteTypedFrame(fd_, 'C', CStringBody("SELECT 1")) &&
           SendReady();
  }

  bool Render(std::string_view sql) {
    const auto result = ParseStatement(sql);
    if (!result.ok || result.fail_closed_refusal) {
      return SendError(result.emulation_diagnostic_code.empty() ? "42601" : "0A000",
                       ParserMessage(result.message_vector_json)) &&
             SendReady();
    }
    if (SingletonProbe(sql)) return SendProbeRow();
    if (result.statement_family == "query") {
      std::vector<std::uint8_t> empty_description;
      wire::AppendBe16(&empty_description, 0);
      return wire::WriteTypedFrame(fd_, 'T', empty_description) &&
             wire::WriteTypedFrame(fd_, 'C', CStringBody("SELECT 0")) &&
             SendReady();
    }
    return wire::WriteTypedFrame(fd_, 'C', CStringBody("YUGABYTEDB PARSE")) &&
           SendReady();
  }

  int fd_;
  bool startup_cancelled_{false};
};

} // namespace

int ServeYugabytedbWorkerSession(int fd) {
  return YugabytedbSession(fd).Run();
}

} // namespace scratchbird::parser::yugabytedb
