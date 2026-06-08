// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "control_plane.hpp"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <cerrno>
#include <fcntl.h>
#include <sys/socket.h>
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

bool ModeIs(const std::string& mode, const char* expected) {
  return mode == expected;
}

std::uint64_t ReadU64(const std::vector<std::uint8_t>& data, std::size_t off) {
  std::uint64_t out = 0;
  for (int shift = 0; shift < 64; shift += 8) {
    out |= static_cast<std::uint64_t>(data[off + static_cast<std::size_t>(shift / 8)]) << shift;
  }
  return out;
}

#ifndef _WIN32
void CloseFd(int fd) {
  if (fd >= 0) ::close(fd);
}

void ServeClient(int fd) {
  const char greeting[] = "ScratchBird dummy parser ready\n";
  (void)::write(fd, greeting, sizeof(greeting) - 1);
  char buffer[1024];
  for (;;) {
    const ssize_t rc = ::read(fd, buffer, sizeof(buffer));
    if (rc > 0) {
      (void)::write(fd, buffer, static_cast<std::size_t>(rc));
      continue;
    }
    if (rc < 0 && errno == EINTR) continue;
    break;
  }
}

void SlowServeClient(int fd) {
  std::this_thread::sleep_for(std::chrono::milliseconds(750));
  ServeClient(fd);
}

bool ShouldCrashOnceAfterHandoff(const std::string& behavior) {
  if (!ModeIs(behavior, "crash_once_after_handoff")) return false;
  const auto marker = Env("SB_PARSER_DUMMY_CRASH_ONCE_FILE");
  if (marker.empty()) return true;
  const int fd = ::open(marker.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
  if (fd < 0) return false;
  ::close(fd);
  return true;
}
#endif

} // namespace

int main(int argc, char** argv) {
  bool listener_worker = false;
  std::string behavior = Env("SB_PARSER_DUMMY_BEHAVIOR", "normal");
  for (int i = 1; i < argc; ++i) {
    if (std::string(argv[i]) == "--listener-worker") listener_worker = true;
    if (std::string(argv[i]).starts_with("--dummy-behavior=")) {
      behavior = std::string(argv[i]).substr(std::string("--dummy-behavior=").size());
    }
    if (std::string(argv[i]) == "--help") {
      std::cout << "sb_parser_dummy --listener-worker [--dummy-behavior=normal|timeout_hello|bad_hello|bad_bundle|refuse_handoff|crash_after_handoff|crash_once_after_handoff|slow_read]\n";
      return EXIT_SUCCESS;
    }
  }
  if (!listener_worker) {
    std::cout << "sb_parser_dummy is a listener-interface test parser; use --listener-worker.\n";
    return EXIT_SUCCESS;
  }

#ifdef _WIN32
  std::cerr << "sb_parser_dummy listener-worker mode is not attached on Windows.\n";
  return EXIT_FAILURE;
#else
  const int control_fd = static_cast<int>(ParseU64(std::getenv("SB_LISTENER_CONTROL_FD"), 0));
  if (control_fd <= 0) {
    std::cerr << "SB_LISTENER_CONTROL_FD is required.\n";
    return EXIT_FAILURE;
  }

  if (ModeIs(behavior, "timeout_hello")) {
    std::this_thread::sleep_for(std::chrono::milliseconds(35000));
    return EXIT_SUCCESS;
  }

  scratchbird::listener::ParserHelloPayload hello;
  hello.protocol = Env("SB_PROTOCOL_FAMILY", "sbsql");
  hello.pid = static_cast<std::uint32_t>(::getpid());
  hello.worker_id = ParseU64(std::getenv("SB_PARSER_WORKER_NUMERIC_ID"), 1);
  hello.dialect_protocol_version = 1;
  hello.parser_api_major = ParseU32(std::getenv("SB_PARSER_API_MAJOR"), 1);
  hello.profile_id = Env("SB_PARSER_PROFILE_ID", "default");
  hello.bundle_contract_id = Env("SB_PARSER_BUNDLE_CONTRACT_ID", "bundle.default@1");
  if (ModeIs(behavior, "bad_hello")) {
    hello.parser_api_major = 0;
  }
  if (ModeIs(behavior, "bad_bundle")) {
    hello.bundle_contract_id = "bundle.invalid@0";
  }

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
    int client_fd = -1;
    if (!scratchbird::listener::ReadControlFrame(control_fd, &inbound, &client_fd, 300000)) {
      CloseFd(client_fd);
      return EXIT_SUCCESS;
    }
    if (inbound.frame.opcode == scratchbird::listener::ListenerControlOpcode::kHandoffSocket) {
      const std::uint64_t connection_id = inbound.frame.payload.size() >= 8 ? ReadU64(inbound.frame.payload, 0) : inbound.frame.sequence;
      if (ModeIs(behavior, "crash_after_handoff") || ShouldCrashOnceAfterHandoff(behavior)) {
        CloseFd(client_fd);
        return EXIT_FAILURE;
      }
      if (ModeIs(behavior, "error_message")) {
        scratchbird::listener::ListenerControlFrame error;
        error.opcode = scratchbird::listener::ListenerControlOpcode::kErrorMessage;
        error.sequence = inbound.frame.sequence;
        error.payload = scratchbird::listener::EncodeErrorMessagePayload({"dummy parser fatal"});
        scratchbird::listener::SendControlFrame(control_fd, error);
        CloseFd(client_fd);
        return EXIT_FAILURE;
      }
      scratchbird::listener::HandoffAckPayload handoff_ack;
      handoff_ack.connection_id_echo = connection_id;
      handoff_ack.accepted = client_fd >= 0 && !ModeIs(behavior, "refuse_handoff");
      handoff_ack.reason = handoff_ack.accepted ? "" : (ModeIs(behavior, "refuse_handoff") ? "dummy_refused_handoff" : "client_fd_missing");
      scratchbird::listener::ListenerControlFrame response;
      response.opcode = scratchbird::listener::ListenerControlOpcode::kHandoffAck;
      response.sequence = inbound.frame.sequence;
      response.payload = scratchbird::listener::EncodeHandoffAckPayload(handoff_ack);
      scratchbird::listener::SendControlFrame(control_fd, response);
      if (client_fd >= 0) {
        if (handoff_ack.accepted) {
          if (ModeIs(behavior, "slow_read")) {
            SlowServeClient(client_fd);
          } else {
            ServeClient(client_fd);
          }
        }
        CloseFd(client_fd);
      }
      return EXIT_SUCCESS;
    }
    CloseFd(client_fd);
    if (inbound.frame.opcode == scratchbird::listener::ListenerControlOpcode::kHealthCheck) {
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
      if (inbound.frame.opcode == scratchbird::listener::ListenerControlOpcode::kRecycle) {
        auto messages = scratchbird::listener::MakeMessageVectorSet({});
        if (!scratchbird::listener::DecodeRecyclePayload(inbound.frame.payload, &messages)) {
          return EXIT_FAILURE;
        }
      }
      return EXIT_SUCCESS;
    }
  }
#endif
}
