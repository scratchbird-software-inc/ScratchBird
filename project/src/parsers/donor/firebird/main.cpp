// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "firebird_dialect.hpp"
#include "firebird_worker_session.hpp"
#include "firebird_wire_descriptor.hpp"

#include "control_plane.hpp"

#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#ifndef _WIN32
#include <unistd.h>
#endif

namespace {

std::uint64_t ParseU64(const char* value, std::uint64_t fallback) {
  if (value == nullptr || *value == '\0') return fallback;
  char* end = nullptr;
  const auto parsed = std::strtoull(value, &end, 10);
  return end != nullptr && *end == '\0' ? static_cast<std::uint64_t>(parsed) : fallback;
}

std::uint32_t ParseU32(const char* value, std::uint32_t fallback) {
  return static_cast<std::uint32_t>(ParseU64(value, fallback));
}

std::string Env(const char* name, std::string fallback = {}) {
  const char* value = std::getenv(name);
  return value == nullptr || *value == '\0' ? std::move(fallback) : std::string(value);
}

std::uint64_t ReadLittleEndianU64(const std::vector<std::uint8_t>& payload) {
  if (payload.size() < 8) return 0;
  std::uint64_t out = 0;
  for (int shift = 0; shift < 64; shift += 8) {
    out |= static_cast<std::uint64_t>(payload[static_cast<std::size_t>(shift / 8)]) << shift;
  }
  return out;
}

int HexDigit(char ch) {
  if (ch >= '0' && ch <= '9') return ch - '0';
  if (ch >= 'a' && ch <= 'f') return 10 + (ch - 'a');
  if (ch >= 'A' && ch <= 'F') return 10 + (ch - 'A');
  return -1;
}

bool IsHexSeparator(char ch) {
  return std::isspace(static_cast<unsigned char>(ch)) || ch == ':' || ch == '-' || ch == '_';
}

bool ParseHexBytes(std::string_view hex,
                   std::vector<std::uint8_t>* out,
                   std::string* diagnostic) {
  out->clear();
  int high_nibble = -1;
  for (const char ch : hex) {
    if (IsHexSeparator(ch)) continue;
    const int digit = HexDigit(ch);
    if (digit < 0) {
      *diagnostic = "FIREBIRD.WIRE.HEX_INVALID";
      return false;
    }
    if (high_nibble < 0) {
      high_nibble = digit;
      continue;
    }
    out->push_back(static_cast<std::uint8_t>((high_nibble << 4) | digit));
    high_nibble = -1;
  }
  if (high_nibble >= 0) {
    *diagnostic = "FIREBIRD.WIRE.HEX_ODD_LENGTH";
    return false;
  }
  return true;
}

int RunParameterBufferDecodeCli(std::string_view kind, std::string_view hex) {
  std::vector<std::uint8_t> buffer;
  std::string diagnostic;
  if (!ParseHexBytes(hex, &buffer, &diagnostic)) {
    std::cerr << "{\"ok\":false,\"kind\":\"" << kind
              << "\",\"diagnostic_code\":\"" << diagnostic
              << "\",\"runtime_policy\":\"fail_closed\"}\n";
    return EXIT_FAILURE;
  }
  const auto result = scratchbird::parser::firebird::DecodeFirebirdParameterBuffer(kind, buffer);
  (result.ok ? std::cout : std::cerr) << result.json << '\n';
  return result.ok ? EXIT_SUCCESS : EXIT_FAILURE;
}

#ifndef _WIN32
void CloseFd(int fd) {
  if (fd >= 0) ::close(fd);
}

int RunListenerWorker() {
  const int control_fd = static_cast<int>(ParseU64(std::getenv("SB_LISTENER_CONTROL_FD"), 0));
  if (control_fd <= 0) {
    std::cerr << "SB_LISTENER_CONTROL_FD is required.\n";
    return EXIT_FAILURE;
  }

  scratchbird::listener::ParserHelloPayload hello;
  hello.protocol = Env("SB_PROTOCOL_FAMILY", "firebird");
  hello.pid = static_cast<std::uint32_t>(::getpid());
  hello.worker_id = ParseU64(std::getenv("SB_PARSER_WORKER_NUMERIC_ID"), 1);
  hello.dialect_protocol_version = 13;
  hello.parser_api_major = ParseU32(std::getenv("SB_PARSER_API_MAJOR"), 1);
  hello.profile_id = Env("SB_PARSER_PROFILE_ID", "default");
  hello.bundle_contract_id = Env("SB_PARSER_BUNDLE_CONTRACT_ID", "bundle.default@1");

  scratchbird::listener::ListenerControlFrame hello_frame;
  hello_frame.opcode = scratchbird::listener::ListenerControlOpcode::kHello;
  hello_frame.sequence = hello.worker_id;
  hello_frame.payload = scratchbird::listener::EncodeHelloPayload(hello);
  if (!scratchbird::listener::SendControlFrame(control_fd, hello_frame)) {
    return EXIT_FAILURE;
  }

  scratchbird::listener::ListenerControlDecodeResult decoded;
  int received_fd = -1;
  if (!scratchbird::listener::ReadControlFrame(control_fd, &decoded, &received_fd, 30000) ||
      decoded.frame.opcode != scratchbird::listener::ListenerControlOpcode::kHelloAck) {
    CloseFd(received_fd);
    return EXIT_FAILURE;
  }
  CloseFd(received_fd);
  auto ack = scratchbird::listener::DecodeHelloAckPayload(decoded.frame.payload, &decoded.messages);
  if (!ack || !ack->accepted) {
    return EXIT_FAILURE;
  }

  for (;;) {
    scratchbird::listener::ListenerControlDecodeResult inbound;
    int handoff_fd = -1;
    if (!scratchbird::listener::ReadControlFrame(control_fd, &inbound, &handoff_fd, 300000)) {
      CloseFd(handoff_fd);
      return EXIT_SUCCESS;
    }
    if (inbound.frame.opcode == scratchbird::listener::ListenerControlOpcode::kHealthCheck) {
      CloseFd(handoff_fd);
      scratchbird::listener::ListenerControlFrame response;
      response.opcode = scratchbird::listener::ListenerControlOpcode::kHealthReport;
      response.sequence = inbound.frame.sequence;
      response.payload = scratchbird::listener::EncodeHealthReportPayload(
          scratchbird::listener::HealthReportPayload{inbound.frame.sequence, 0, 0});
      scratchbird::listener::SendControlFrame(control_fd, response);
      continue;
    }
    if (inbound.frame.opcode == scratchbird::listener::ListenerControlOpcode::kRecycle ||
        inbound.frame.opcode == scratchbird::listener::ListenerControlOpcode::kShutdown) {
      CloseFd(handoff_fd);
      return EXIT_SUCCESS;
    }
    if (inbound.frame.opcode == scratchbird::listener::ListenerControlOpcode::kHandoffSocket) {
      const auto connection_id = ReadLittleEndianU64(inbound.frame.payload);
      scratchbird::listener::HandoffAckPayload handoff_ack;
      handoff_ack.connection_id_echo = connection_id;
      handoff_ack.accepted = connection_id != 0 && handoff_fd >= 0;
      handoff_ack.reason = handoff_ack.accepted
                               ? ""
                               : (connection_id == 0 ? "firebird_wire_handoff_payload_invalid"
                                                     : "firebird_wire_client_fd_missing");
      scratchbird::listener::ListenerControlFrame response;
      response.opcode = scratchbird::listener::ListenerControlOpcode::kHandoffAck;
      response.sequence = inbound.frame.sequence;
      response.payload = scratchbird::listener::EncodeHandoffAckPayload(handoff_ack);
      scratchbird::listener::SendControlFrame(control_fd, response);
      if (handoff_ack.accepted) {
        const int rc = scratchbird::parser::firebird::ServeFirebirdWorkerSession(handoff_fd);
        CloseFd(handoff_fd);
        return rc == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
      }
      CloseFd(handoff_fd);
      continue;
    }
    CloseFd(handoff_fd);
  }
}
#endif

} // namespace

int main(int argc, char** argv) {
  for (int i = 1; i < argc; ++i) {
    if (std::string(argv[i]) == "--listener-worker") {
#ifdef _WIN32
      std::cerr << "SB_FBSQL_Parser listener-worker mode is not attached on Windows.\n";
      return EXIT_FAILURE;
#else
      return RunListenerWorker();
#endif
    }
    if (std::string(argv[i]) == "--package-identity") {
      std::cout << scratchbird::parser::firebird::FirebirdPackageIdentityJson() << '\n';
      return EXIT_SUCCESS;
    }
    if (std::string(argv[i]) == "--surface-report") {
      std::cout << "{\"dialect\":\"firebird\","
                << "\"datatype_surfaces\":" << scratchbird::parser::firebird::DatatypeSurfaces().size() << ','
                << "\"builtin_function_surfaces\":" << scratchbird::parser::firebird::BuiltinFunctionSurfaces().size() << ','
                << "\"catalog_overlay_surfaces\":" << scratchbird::parser::firebird::CatalogOverlaySurfaces().size() << ','
                << "\"diagnostic_surfaces\":" << scratchbird::parser::firebird::DiagnosticSurfaces().size()
                << "}\n";
      return EXIT_SUCCESS;
    }
  }
  if (argc == 4 && std::string_view(argv[1]) == "--decode-parameter-buffer") {
    return RunParameterBufferDecodeCli(argv[2], argv[3]);
  }

  const std::string sql = argc > 1 ? argv[1] : "select 1";
  const auto result = scratchbird::parser::firebird::ParseStatement(sql);
  if (!result.ok) {
    std::cerr << result.message_vector_json << '\n';
    return EXIT_FAILURE;
  }
  if (result.exact_emulated_diagnostic && !result.message_vector_json.empty()) {
    std::cerr << result.message_vector_json << '\n';
  }
  std::cout << result.sblr_envelope << '\n';
  return EXIT_SUCCESS;
}
