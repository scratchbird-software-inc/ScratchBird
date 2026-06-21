// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "wire/sbsql_test_wire.hpp"
#include "datatype_wire_metadata.hpp"
#include "sbps.hpp"

#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

constexpr std::size_t kHeaderSize = 40;
constexpr std::uint8_t kStartup = 0x01;
constexpr std::uint8_t kAuthResponse = 0x02;
constexpr std::uint8_t kParse = 0x04;
constexpr std::uint8_t kBind = 0x05;
constexpr std::uint8_t kPing = 0x1b;
constexpr std::uint8_t kResetSession = 0x21;
constexpr std::uint8_t kAuthRequest = 0x40;
constexpr std::uint8_t kAuthOk = 0x41;
constexpr std::uint8_t kReady = 0x43;
constexpr std::uint8_t kError = 0x48;
constexpr std::uint8_t kParseComplete = 0x4a;
constexpr std::uint8_t kBindComplete = 0x4b;
constexpr std::uint8_t kPong = 0x5d;
constexpr std::uint8_t kExtension = 0x81;
constexpr std::uint8_t kFrameFlagCompressed = 1u << 0;
constexpr std::uint16_t kVersionP1Current = 0x0101;
constexpr std::uint64_t kFeatureSblr = 1ull << 2u;
constexpr std::uint64_t kFeatureArrayBind = 1ull << 16u;
constexpr std::uint32_t kOidInt8 = 20;
constexpr std::uint32_t kOidText = 25;

namespace datatypes = scratchbird::core::datatypes;

struct Frame {
  std::uint8_t type{0};
  std::vector<std::uint8_t> payload;
};

[[noreturn]] void Fail(const std::string& message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, const std::string& message) {
  if (!condition) Fail(message);
}

void PutU16(std::vector<std::uint8_t>* out, std::uint16_t value) {
  out->push_back(static_cast<std::uint8_t>(value & 0xffu));
  out->push_back(static_cast<std::uint8_t>((value >> 8u) & 0xffu));
}

void PutU32(std::vector<std::uint8_t>* out, std::uint32_t value) {
  for (int shift = 0; shift < 32; shift += 8) {
    out->push_back(static_cast<std::uint8_t>((value >> shift) & 0xffu));
  }
}

void PutU64(std::vector<std::uint8_t>* out, std::uint64_t value) {
  for (int shift = 0; shift < 64; shift += 8) {
    out->push_back(static_cast<std::uint8_t>((value >> shift) & 0xffu));
  }
}

std::uint32_t ReadU32(const std::array<std::uint8_t, kHeaderSize>& bytes,
                      std::size_t offset) {
  return static_cast<std::uint32_t>(bytes[offset]) |
         (static_cast<std::uint32_t>(bytes[offset + 1]) << 8u) |
         (static_cast<std::uint32_t>(bytes[offset + 2]) << 16u) |
         (static_cast<std::uint32_t>(bytes[offset + 3]) << 24u);
}

void PutLpStr(std::vector<std::uint8_t>* out, std::string_view value) {
  PutU32(out, static_cast<std::uint32_t>(value.size()));
  out->insert(out->end(), value.begin(), value.end());
}

std::vector<std::uint8_t> EncodeFrame(std::uint8_t type,
                                      const std::vector<std::uint8_t>& payload,
                                      std::uint8_t flags = 0) {
  std::vector<std::uint8_t> out;
  out.reserve(kHeaderSize + payload.size());
  out.insert(out.end(), {'S', 'B', 'W', 'P', 1, 1, type, flags});
  PutU32(&out, static_cast<std::uint32_t>(payload.size()));
  PutU32(&out, 1);
  out.insert(out.end(), 16, 0);
  PutU64(&out, 0);
  out.insert(out.end(), payload.begin(), payload.end());
  return out;
}

std::vector<std::uint8_t> StartupPayload(std::uint16_t min_version,
                                         std::uint16_t max_version,
                                         std::uint32_t connect_flags,
                                         std::uint64_t client_features,
                                         std::uint64_t required_features,
                                         std::uint64_t optional_features,
                                         bool required_extension = false) {
  std::vector<std::uint8_t> out;
  PutU16(&out, min_version);
  PutU16(&out, max_version);
  PutU32(&out, connect_flags);
  PutU64(&out, client_features);
  PutU64(&out, required_features);
  PutU64(&out, optional_features);
  out.insert(out.end(), 16, 0x11);
  out.insert(out.end(), 16, 0);
  out.insert(out.end(), 16, 0);
  PutU32(&out, 0);
  PutU32(&out, required_extension ? 1 : 0);
  if (required_extension) {
    PutLpStr(&out, "com.example.future");
    PutU16(&out, 1);
    PutU16(&out, 0);
    PutU16(&out, 0);
    PutU32(&out, 1);
  }
  return out;
}

bool WriteAll(int fd, const std::vector<std::uint8_t>& bytes) {
  std::size_t written = 0;
  while (written < bytes.size()) {
    const auto rc = ::write(fd, bytes.data() + written, bytes.size() - written);
    if (rc > 0) {
      written += static_cast<std::size_t>(rc);
      continue;
    }
    if (rc < 0 && errno == EINTR) continue;
    return false;
  }
  return true;
}

bool ReadExact(int fd, void* buffer, std::size_t size) {
  auto* out = static_cast<std::uint8_t*>(buffer);
  std::size_t read_total = 0;
  while (read_total < size) {
    pollfd pfd{fd, POLLIN, 0};
    const int ready = ::poll(&pfd, 1, 2000);
    if (ready <= 0 || (pfd.revents & POLLIN) == 0) return false;
    const auto rc = ::read(fd, out + read_total, size - read_total);
    if (rc > 0) {
      read_total += static_cast<std::size_t>(rc);
      continue;
    }
    if (rc < 0 && errno == EINTR) continue;
    return false;
  }
  return true;
}

Frame ReadFrame(int fd) {
  std::array<std::uint8_t, kHeaderSize> header{};
  Require(ReadExact(fd, header.data(), header.size()), "SBWP response header was not readable");
  Require(std::memcmp(header.data(), "SBWP", 4) == 0, "SBWP response magic mismatch");
  const auto length = ReadU32(header, 8);
  Frame frame;
  frame.type = header[6];
  frame.payload.assign(length, 0);
  if (length != 0) {
    Require(ReadExact(fd, frame.payload.data(), frame.payload.size()),
            "SBWP response payload was not readable");
  }
  return frame;
}

Frame ExchangeOne(const std::vector<std::uint8_t>& request) {
  int fds[2]{-1, -1};
  Require(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0, "socketpair failed");
  Require(WriteAll(fds[0], request), "SBWP request write failed");
  (void)::shutdown(fds[0], SHUT_WR);

  scratchbird::parser::sbsql::ParserConfig config;
  config.probe_mode = true;
  scratchbird::parser::sbsql::ParserMetrics metrics;
  scratchbird::parser::sbsql::SblrTemplateCache cache;
  scratchbird::parser::sbsql::SbsqlTestWireSession session(config, &metrics, &cache);

  std::thread worker([&session, server_fd = fds[1]]() {
    (void)session.ServeFd(server_fd);
    (void)::close(server_fd);
  });
  const Frame response = ReadFrame(fds[0]);
  worker.join();
  (void)::close(fds[0]);
  return response;
}

std::vector<Frame> ExchangeConversation(const std::vector<std::vector<std::uint8_t>>& requests) {
  int fds[2]{-1, -1};
  Require(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0, "socketpair failed");

  scratchbird::parser::sbsql::ParserConfig config;
  config.probe_mode = true;
  scratchbird::parser::sbsql::ParserMetrics metrics;
  scratchbird::parser::sbsql::SblrTemplateCache cache;
  scratchbird::parser::sbsql::SbsqlTestWireSession session(config, &metrics, &cache);

  std::thread worker([&session, server_fd = fds[1]]() {
    (void)session.ServeFd(server_fd);
    (void)::close(server_fd);
  });

  std::vector<Frame> responses;
  responses.reserve(requests.size());
  for (const auto& request : requests) {
    Require(WriteAll(fds[0], request), "SBWP request write failed");
    responses.push_back(ReadFrame(fds[0]));
  }
  (void)::shutdown(fds[0], SHUT_WR);
  worker.join();
  (void)::close(fds[0]);
  return responses;
}

std::string MakeTempDatabasePath() {
  std::string tmpl = "/tmp/sb_p1_wire_array_bind.XXXXXX";
  std::vector<char> writable(tmpl.begin(), tmpl.end());
  writable.push_back('\0');
  char* made = ::mkdtemp(writable.data());
  Require(made != nullptr, "array-bind temp database directory was not created");
  return std::string(made) + "/array_bind.sbdb";
}

std::vector<Frame> ExchangeAuthenticatedArrayBindConversation(
    const std::vector<std::vector<std::uint8_t>>& requests) {
  int fds[2]{-1, -1};
  Require(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0, "socketpair failed");

  scratchbird::parser::sbsql::ParserConfig config;
  config.probe_mode = true;
  config.embedded_engine_direct = true;
  config.embedded_auth_bypass_sysarch = true;
  config.embedded_database_path = MakeTempDatabasePath();
  scratchbird::parser::sbsql::ParserMetrics metrics;
  scratchbird::parser::sbsql::SblrTemplateCache cache;
  scratchbird::parser::sbsql::SbsqlTestWireSession session(config, &metrics, &cache);

  std::thread worker([&session, server_fd = fds[1]]() {
    (void)session.ServeFd(server_fd);
    (void)::close(server_fd);
  });

  std::vector<Frame> responses;
  responses.reserve(4);
  Require(WriteAll(fds[0], requests[0]), "SBWP startup write failed");
  responses.push_back(ReadFrame(fds[0]));
  Require(responses.back().type == kAuthRequest, "array-bind startup did not request auth");

  Require(WriteAll(fds[0], EncodeFrame(kAuthResponse, {})), "SBWP auth response write failed");
  bool saw_auth_ok = false;
  for (;;) {
    Frame frame = ReadFrame(fds[0]);
    if (frame.type == kAuthOk) saw_auth_ok = true;
    if (frame.type == kError) {
      responses.push_back(std::move(frame));
      break;
    }
    if (frame.type == kReady) break;
  }
  Require(saw_auth_ok, "array-bind embedded auth did not produce AuthOk");

  for (std::size_t i = 1; i < requests.size(); ++i) {
    Require(WriteAll(fds[0], requests[i]), "SBWP authenticated request write failed");
    responses.push_back(ReadFrame(fds[0]));
  }
  (void)::shutdown(fds[0], SHUT_WR);
  worker.join();
  (void)::close(fds[0]);
  return responses;
}

bool PayloadContains(const Frame& frame, std::string_view needle) {
  const std::string payload(reinterpret_cast<const char*>(frame.payload.data()),
                            frame.payload.size());
  return payload.find(needle) != std::string::npos;
}

std::vector<std::uint8_t> ParsePayload(std::string_view name,
                                       std::string_view sql,
                                       const std::vector<std::uint32_t>& param_types) {
  std::vector<std::uint8_t> out;
  PutLpStr(&out, name);
  PutLpStr(&out, sql);
  PutU16(&out, static_cast<std::uint16_t>(param_types.size()));
  PutU16(&out, 0);
  for (std::uint32_t type : param_types) {
    PutU32(&out, type);
  }
  return out;
}

datatypes::ParameterValueCell TextCell(std::uint32_t ordinal, std::string_view value) {
  datatypes::ParameterValueCell cell;
  cell.slot_ordinal = ordinal;
  cell.value_state = datatypes::WireValueState::value_present;
  cell.payload_encoding = datatypes::WirePayloadEncoding::utf8_text;
  cell.payload.assign(value.begin(), value.end());
  return cell;
}

std::vector<std::uint8_t> ArrayBindPayload() {
  datatypes::ParameterDataPacket packet;
  packet.bind_shape = datatypes::WireParameterBindShape::array_bind_rows;
  for (std::uint32_t ordinal : {1u, 2u}) {
    datatypes::ParameterBindingSlotDescriptor slot;
    slot.ordinal = ordinal;
    packet.slot_descriptors.push_back(slot);
  }
  datatypes::ParameterRowValueFrame first;
  first.row_ordinal = 1;
  first.values.push_back(TextCell(1, "1"));
  first.values.push_back(TextCell(2, "alice"));
  packet.row_value_frames.push_back(std::move(first));

  datatypes::ParameterRowValueFrame second;
  second.row_ordinal = 2;
  second.values.push_back(TextCell(1, "2"));
  second.values.push_back(TextCell(2, "bob"));
  packet.row_value_frames.push_back(std::move(second));

  auto encoded = datatypes::EncodeParameterDataPacket(packet);
  Require(encoded.ok, "array-bind parameter data packet did not encode");
  return std::move(encoded.bytes);
}

void ExpectError(const std::vector<std::uint8_t>& request,
                 std::string_view diagnostic_code) {
  const auto response = ExchangeOne(request);
  Require(response.type == kError, "SBWP request did not return Error");
  Require(PayloadContains(response, diagnostic_code),
          "SBWP Error did not include expected diagnostic " + std::string(diagnostic_code));
}

void CheckStartupNegotiationFailures() {
  ExpectError(
      EncodeFrame(kStartup,
                  StartupPayload(0x0102, 0x0102, 0, 0, 0, 0)),
      "SBWP.VERSION.NO_COMMON_VERSION");

  const std::uint64_t unknown_bit = 1ull << 40u;
  ExpectError(
      EncodeFrame(kStartup,
                  StartupPayload(kVersionP1Current,
                                 kVersionP1Current,
                                 0,
                                 unknown_bit,
                                 unknown_bit,
                                 0)),
      "SBWP.FEATURE.UNKNOWN_REQUIRED");

  ExpectError(
      EncodeFrame(kStartup,
                  StartupPayload(kVersionP1Current,
                                 kVersionP1Current,
                                 1u << 31u,
                                 0,
                                 0,
                                 0)),
      "NATIVE_WIRE.CONNECT_INVALID_PAYLOAD");

  ExpectError(
      EncodeFrame(kStartup,
                  StartupPayload(kVersionP1Current,
                                 kVersionP1Current,
                                 0,
                                 0,
                                 0,
                                 0,
                                 true)),
      "SBWP.EXTENSION.UNKNOWN_REQUIRED");
}

void CheckSblrFeatureNegotiates() {
  const auto response =
      ExchangeOne(EncodeFrame(kStartup,
                              StartupPayload(kVersionP1Current,
                                             kVersionP1Current,
                                             0,
                                             kFeatureSblr,
                                             kFeatureSblr,
                                             0)));
  Require(response.type == kAuthRequest,
          "SBWP startup with required SBLR feature did not reach authentication");
}

void CheckFrameFailClosedPaths() {
  ExpectError(EncodeFrame(kResetSession, {}), "SBWP.FEATURE.NOT_NEGOTIATED");
  ExpectError(EncodeFrame(kExtension, {}), "SBWP.FEATURE.NOT_NEGOTIATED");
  ExpectError(EncodeFrame(kPing, {1, 2, 3}, kFrameFlagCompressed),
              "SBWP.COMPRESSION.UNNEGOTIATED_FRAME");
  ExpectError(EncodeFrame(0x7f, {}), "NATIVE_WIRE.PROTOCOL_FATAL");
}

void CheckPingPongEcho() {
  const std::vector<std::uint8_t> payload{0x10, 0x20, 0x30, 0x40};
  const auto response = ExchangeOne(EncodeFrame(kPing, payload));
  Require(response.type == kPong, "SBWP Ping did not return Pong");
  Require(response.payload == payload, "SBWP Pong did not echo Ping payload");
}

void CheckSbpsUnknownCapabilityBits() {
  namespace sbps = scratchbird::server::sbps;

  const auto decoded = sbps::DecodeHelloRequest(sbps::EncodeHelloRequestForTest());
  Require(decoded.has_value(), "built-in SBPS hello did not decode");
  Require(!sbps::HasUnknownCapabilityBits(decoded->capability_bitmap),
          "built-in SBPS hello unexpectedly has unknown capability bits");
  Require(sbps::IsBuiltInTestHello(*decoded),
          "built-in SBPS hello was not accepted by the test profile gate");

  auto unknown_low = *decoded;
  unknown_low.capability_bitmap[0] |= 0x02u;
  Require(sbps::HasUnknownCapabilityBits(unknown_low.capability_bitmap),
          "SBPS low-byte unknown capability bit was not detected");
  Require(!sbps::IsBuiltInTestHello(unknown_low),
          "SBPS hello with low-byte unknown capability bit was not refused");

  auto unknown_high = *decoded;
  unknown_high.capability_bitmap[7] = 0x80u;
  Require(sbps::HasUnknownCapabilityBits(unknown_high.capability_bitmap),
          "SBPS high-byte unknown capability bit was not detected");
  Require(!sbps::IsBuiltInTestHello(unknown_high),
          "SBPS hello with high-byte unknown capability bit was not refused");
}

void CheckArrayBindPacketNegotiated() {
  const auto responses = ExchangeAuthenticatedArrayBindConversation({
      EncodeFrame(kStartup,
                  StartupPayload(kVersionP1Current,
                                 kVersionP1Current,
                                 0,
                                 kFeatureArrayBind,
                                 kFeatureArrayBind,
                                 0)),
      EncodeFrame(kParse,
                  ParsePayload("stmt_array",
                               "INSERT INTO users.public.array_bind_probe (id, name) VALUES (?, ?)",
                               {kOidInt8, kOidText})),
      EncodeFrame(kBind, ArrayBindPayload()),
  });
  Require(responses.size() == 3, "array-bind conversation response count mismatch");
  Require(responses[0].type == kAuthRequest, "array-bind startup did not negotiate P1");
  Require(responses[1].type == kParseComplete, "array-bind prepared parse failed");
  Require(responses[2].type == kBindComplete,
          "array-bind packet did not bind through the prepared rowset route");
}

}  // namespace

int main() {
  ::signal(SIGPIPE, SIG_IGN);
  CheckStartupNegotiationFailures();
  CheckSblrFeatureNegotiates();
  CheckFrameFailClosedPaths();
  CheckPingPongEcho();
  CheckSbpsUnknownCapabilityBits();
  CheckArrayBindPacketNegotiated();
  std::cout << "sbsql_native_wire_p1_conformance ok\n";
  return EXIT_SUCCESS;
}
