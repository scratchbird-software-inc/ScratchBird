// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "vitess_dialect.hpp"

#include "control_plane.hpp"
#include "compatibility_worker_session.hpp"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
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

std::string Env(const char* env_name, std::string fallback = {}) {
  const char* value = std::getenv(env_name);
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
  hello.protocol = Env("SB_PROTOCOL_FAMILY", "vitess");
  hello.pid = static_cast<std::uint32_t>(::getpid());
  hello.worker_id = ParseU64(std::getenv("SB_PARSER_WORKER_NUMERIC_ID"), 1);
  hello.dialect_protocol_version = 1;
  hello.parser_api_major = ParseU32(std::getenv("SB_PARSER_API_MAJOR"), 1);
  hello.profile_id = Env("SB_PARSER_PROFILE_ID", "vitess.default");
  hello.bundle_contract_id = Env("SB_PARSER_BUNDLE_CONTRACT_ID", "vitess.bundle.default@1");

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
  if (!ack || !ack->accepted) return EXIT_FAILURE;

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
                               : (connection_id == 0 ? "vitess_handoff_payload_invalid"
                                                     : "vitess_client_fd_missing");
      scratchbird::listener::ListenerControlFrame response;
      response.opcode = scratchbird::listener::ListenerControlOpcode::kHandoffAck;
      response.sequence = inbound.frame.sequence;
      response.payload = scratchbird::listener::EncodeHandoffAckPayload(handoff_ack);
      scratchbird::listener::SendControlFrame(control_fd, response);
      if (handoff_ack.accepted) {
        const int rc = scratchbird::parser::compatibility::ServeTextWorkerSession(
            handoff_fd, scratchbird::parser::vitess::Profile());
        CloseFd(handoff_fd);
        return rc == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
      }
      CloseFd(handoff_fd);
    }
  }
}
#endif

} // namespace

int main(int argc, char** argv) {
  for (int i = 1; i < argc; ++i) {
    if (std::string(argv[i]) == "--listener-worker") {
#ifdef _WIN32
      std::cerr << "sbp_vitess listener-worker mode is not attached on Windows.\n";
      return EXIT_FAILURE;
#else
      return RunListenerWorker();
#endif
    }
    if (std::string(argv[i]) == "--package-identity") {
      std::cout << scratchbird::parser::vitess::VitessPackageIdentityJson() << '\n';
      return EXIT_SUCCESS;
    }
    if (std::string(argv[i]) == "--surface-report") {
      std::cout << scratchbird::parser::vitess::VitessSurfaceReportJson() << '\n';
      return EXIT_SUCCESS;
    }
  }

  const std::string sql = argc > 1 ? argv[1] : "select 1";
  const auto result = scratchbird::parser::vitess::ParseStatement(sql);
  if (!result.ok) {
    std::cerr << result.message_vector_json << '\n';
    return EXIT_FAILURE;
  }
  if (result.fail_closed_refusal && !result.message_vector_json.empty()) {
    std::cerr << result.message_vector_json << '\n';
  }
  std::cout << result.sblr_envelope << '\n';
  return EXIT_SUCCESS;
}
