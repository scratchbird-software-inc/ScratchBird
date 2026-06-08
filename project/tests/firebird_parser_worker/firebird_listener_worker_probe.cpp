// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "control_plane.hpp"

#include <cstdlib>
#include <cerrno>
#include <iostream>
#include <string>

#ifndef _WIN32
#include <csignal>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

#ifndef _WIN32
void CloseFd(int* fd) {
  if (fd != nullptr && *fd >= 0) {
    ::close(*fd);
    *fd = -1;
  }
}
#endif

bool Expect(bool condition, const char* message) {
  if (condition) return true;
  std::cerr << message << '\n';
  return false;
}

#ifndef _WIN32
bool WriteText(int fd, const std::string& text) {
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

bool ReadLine(int fd, std::string* line) {
  line->clear();
  char ch = 0;
  for (;;) {
    const auto rc = ::read(fd, &ch, 1);
    if (rc == 1) {
      if (ch == '\n') return true;
      if (ch != '\r') line->push_back(ch);
      continue;
    }
    if (rc < 0 && errno == EINTR) continue;
    return !line->empty();
  }
}

bool Contains(const std::string& haystack, const std::string& needle) {
  return haystack.find(needle) != std::string::npos;
}
#endif

} // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
  (void)argc;
  (void)argv;
  return EXIT_SUCCESS;
#else
  if (argc != 2) {
    std::cerr << "usage: firebird_listener_worker_probe <sbp_firebird>\n";
    return EXIT_FAILURE;
  }

  int sockets[2] = {-1, -1};
  if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0) {
    std::cerr << "socketpair failed\n";
    return EXIT_FAILURE;
  }

  const pid_t pid = ::fork();
  if (pid == 0) {
    CloseFd(&sockets[0]);
    const std::string fd_text = std::to_string(sockets[1]);
    ::setenv("SB_LISTENER_CONTROL_FD", fd_text.c_str(), 1);
    ::setenv("SB_PROTOCOL_FAMILY", "firebird", 1);
    ::setenv("SB_PARSER_WORKER_NUMERIC_ID", "77", 1);
    ::setenv("SB_PARSER_PROFILE_ID", "default", 1);
    ::setenv("SB_PARSER_BUNDLE_CONTRACT_ID", "bundle.default@1", 1);
    ::setenv("SB_PARSER_API_MAJOR", "1", 1);
    ::execl(argv[1], argv[1], "--listener-worker", nullptr);
    _exit(127);
  }

  CloseFd(&sockets[1]);
  if (!Expect(pid > 0, "fork failed")) {
    CloseFd(&sockets[0]);
    return EXIT_FAILURE;
  }

  scratchbird::listener::ListenerControlDecodeResult decoded;
  int received_fd = -1;
  if (!Expect(scratchbird::listener::ReadControlFrame(sockets[0], &decoded, &received_fd, 30000),
              "failed to read Firebird HELLO")) {
    CloseFd(&received_fd);
    ::kill(pid, SIGKILL);
    return EXIT_FAILURE;
  }
  CloseFd(&received_fd);
  if (!Expect(decoded.frame.opcode == scratchbird::listener::ListenerControlOpcode::kHello,
              "first Firebird worker frame was not HELLO")) {
    ::kill(pid, SIGKILL);
    return EXIT_FAILURE;
  }
  auto hello = scratchbird::listener::DecodeHelloPayload(decoded.frame.payload, &decoded.messages);
  if (!Expect(hello.has_value(), "Firebird HELLO did not decode")) {
    ::kill(pid, SIGKILL);
    return EXIT_FAILURE;
  }
  if (!Expect(hello->protocol == "firebird", "Firebird HELLO protocol mismatch") ||
      !Expect(hello->dialect_protocol_version == 13, "Firebird dialect version mismatch") ||
      !Expect(hello->bundle_contract_id == "bundle.default@1", "Firebird bundle mismatch")) {
    ::kill(pid, SIGKILL);
    return EXIT_FAILURE;
  }

  scratchbird::listener::HelloAckPayload ack;
  ack.accepted = true;
  scratchbird::listener::ListenerControlFrame ack_frame;
  ack_frame.opcode = scratchbird::listener::ListenerControlOpcode::kHelloAck;
  ack_frame.sequence = decoded.frame.sequence;
  ack_frame.payload = scratchbird::listener::EncodeHelloAckPayload(ack);
  if (!Expect(scratchbird::listener::SendControlFrame(sockets[0], ack_frame),
              "failed to send Firebird HELLO_ACK")) {
    ::kill(pid, SIGKILL);
    return EXIT_FAILURE;
  }

  scratchbird::listener::ListenerControlFrame health;
  health.opcode = scratchbird::listener::ListenerControlOpcode::kHealthCheck;
  health.sequence = 8801;
  if (!Expect(scratchbird::listener::SendControlFrame(sockets[0], health),
              "failed to send Firebird health check")) {
    ::kill(pid, SIGKILL);
    return EXIT_FAILURE;
  }
  scratchbird::listener::ListenerControlDecodeResult health_decoded;
  if (!Expect(scratchbird::listener::ReadControlFrame(sockets[0], &health_decoded, &received_fd, 30000),
              "failed to read Firebird health report")) {
    CloseFd(&received_fd);
    ::kill(pid, SIGKILL);
    return EXIT_FAILURE;
  }
  CloseFd(&received_fd);
  if (!Expect(health_decoded.frame.opcode == scratchbird::listener::ListenerControlOpcode::kHealthReport,
              "Firebird worker did not send HEALTH_REPORT")) {
    ::kill(pid, SIGKILL);
    return EXIT_FAILURE;
  }
  const auto health_report =
      scratchbird::listener::DecodeHealthReportPayload(health_decoded.frame.payload, &health_decoded.messages);
  if (!Expect(health_report.has_value(), "Firebird health report did not decode") ||
      !Expect(health_report->request_id_echo == 8801, "Firebird health report echo mismatch") ||
      !Expect(health_report->state == 0, "Firebird health report state mismatch")) {
    ::kill(pid, SIGKILL);
    return EXIT_FAILURE;
  }

  scratchbird::listener::HandoffSocketPayload handoff;
  handoff.connection_id = 99123;
  handoff.protocol = "firebird";
  handoff.client_addr = "127.0.0.1";
  handoff.client_port = 3050;
  scratchbird::listener::ListenerControlFrame handoff_frame;
  handoff_frame.opcode = scratchbird::listener::ListenerControlOpcode::kHandoffSocket;
  handoff_frame.sequence = 8802;
  handoff_frame.payload = scratchbird::listener::EncodeHandoffSocketPayload(handoff);
  if (!Expect(scratchbird::listener::SendControlFrame(sockets[0], handoff_frame),
              "failed to send Firebird handoff")) {
    ::kill(pid, SIGKILL);
    return EXIT_FAILURE;
  }
  scratchbird::listener::ListenerControlDecodeResult handoff_decoded;
  if (!Expect(scratchbird::listener::ReadControlFrame(sockets[0], &handoff_decoded, &received_fd, 30000),
              "failed to read Firebird handoff ack")) {
    CloseFd(&received_fd);
    ::kill(pid, SIGKILL);
    return EXIT_FAILURE;
  }
  CloseFd(&received_fd);
  if (!Expect(handoff_decoded.frame.opcode == scratchbird::listener::ListenerControlOpcode::kHandoffAck,
              "Firebird worker did not send HANDOFF_ACK")) {
    ::kill(pid, SIGKILL);
    return EXIT_FAILURE;
  }
  const auto handoff_ack =
      scratchbird::listener::DecodeHandoffAckPayload(handoff_decoded.frame.payload, &handoff_decoded.messages);
  if (!Expect(handoff_ack.has_value(), "Firebird handoff ack did not decode") ||
      !Expect(handoff_ack->connection_id_echo == handoff.connection_id,
              "Firebird handoff ack connection echo mismatch") ||
      !Expect(!handoff_ack->accepted, "Firebird handoff ack should fail closed") ||
      !Expect(handoff_ack->reason == "firebird_wire_client_fd_missing",
              "Firebird handoff ack reason mismatch")) {
    ::kill(pid, SIGKILL);
    return EXIT_FAILURE;
  }

  int client_pair[2] = {-1, -1};
  if (!Expect(::socketpair(AF_UNIX, SOCK_STREAM, 0, client_pair) == 0,
              "client socketpair failed")) {
    ::kill(pid, SIGKILL);
    return EXIT_FAILURE;
  }
  handoff.connection_id = 99124;
  scratchbird::listener::ListenerControlFrame accepted_handoff_frame;
  accepted_handoff_frame.opcode = scratchbird::listener::ListenerControlOpcode::kHandoffSocket;
  accepted_handoff_frame.flags = scratchbird::listener::kControlFlagHasHandle;
  accepted_handoff_frame.sequence = 8803;
  accepted_handoff_frame.payload = scratchbird::listener::EncodeHandoffSocketPayload(handoff);
  if (!Expect(scratchbird::listener::SendControlFrame(sockets[0], accepted_handoff_frame, client_pair[1]),
              "failed to send Firebird accepted handoff")) {
    CloseFd(&client_pair[0]);
    CloseFd(&client_pair[1]);
    ::kill(pid, SIGKILL);
    return EXIT_FAILURE;
  }
  CloseFd(&client_pair[1]);
  scratchbird::listener::ListenerControlDecodeResult accepted_handoff_decoded;
  if (!Expect(scratchbird::listener::ReadControlFrame(sockets[0], &accepted_handoff_decoded, &received_fd, 30000),
              "failed to read accepted Firebird handoff ack")) {
    CloseFd(&received_fd);
    CloseFd(&client_pair[0]);
    ::kill(pid, SIGKILL);
    return EXIT_FAILURE;
  }
  CloseFd(&received_fd);
  const auto accepted_handoff_ack = scratchbird::listener::DecodeHandoffAckPayload(
      accepted_handoff_decoded.frame.payload, &accepted_handoff_decoded.messages);
  if (!Expect(accepted_handoff_decoded.frame.opcode == scratchbird::listener::ListenerControlOpcode::kHandoffAck,
              "Firebird worker did not ack accepted handoff") ||
      !Expect(accepted_handoff_ack.has_value(), "accepted Firebird handoff ack did not decode") ||
      !Expect(accepted_handoff_ack->connection_id_echo == handoff.connection_id,
              "accepted Firebird handoff echo mismatch") ||
      !Expect(accepted_handoff_ack->accepted,
              "Firebird handoff with client fd was not accepted")) {
    CloseFd(&client_pair[0]);
    ::kill(pid, SIGKILL);
    return EXIT_FAILURE;
  }

  std::string line;
  if (!Expect(WriteText(client_pair[0], "PING\n"), "failed to write PING") ||
      !Expect(ReadLine(client_pair[0], &line), "Firebird PING response missing") ||
      !Expect(line == "OK PONG", "Firebird PING response mismatch")) {
    CloseFd(&client_pair[0]);
    ::kill(pid, SIGKILL);
    return EXIT_FAILURE;
  }
  if (!Expect(WriteText(client_pair[0], "DECODE_PARAMETER_BUFFER DPB 01:1c:06:53:59:53:44:42:41\n"),
              "failed to write DPB decode command") ||
      !Expect(ReadLine(client_pair[0], &line), "Firebird DPB response missing") ||
      !Expect(Contains(line, "PARAMETER_BUFFER") && Contains(line, "isc_dpb_user_name"),
              "Firebird DPB response mismatch")) {
    CloseFd(&client_pair[0]);
    ::kill(pid, SIGKILL);
    return EXIT_FAILURE;
  }
  if (!Expect(WriteText(client_pair[0], "PARSE select 1\n"),
              "failed to write parse command") ||
      !Expect(ReadLine(client_pair[0], &line), "Firebird parse response missing") ||
      !Expect(Contains(line, "SBLR") && Contains(line, "\"dialect\":\"firebird\""),
              "Firebird parse response mismatch")) {
    CloseFd(&client_pair[0]);
    ::kill(pid, SIGKILL);
    return EXIT_FAILURE;
  }
  if (!Expect(WriteText(client_pair[0], "QUIT\n"), "failed to write QUIT") ||
      !Expect(ReadLine(client_pair[0], &line), "Firebird QUIT response missing") ||
      !Expect(line == "OK BYE", "Firebird QUIT response mismatch")) {
    CloseFd(&client_pair[0]);
    ::kill(pid, SIGKILL);
    return EXIT_FAILURE;
  }
  CloseFd(&client_pair[0]);

  int status = 0;
  if (!Expect(::waitpid(pid, &status, 0) == pid, "waitpid failed")) {
    CloseFd(&sockets[0]);
    return EXIT_FAILURE;
  }
  CloseFd(&sockets[0]);
  if (!Expect(WIFEXITED(status) && WEXITSTATUS(status) == 0,
              "Firebird listener worker did not exit cleanly")) {
    return EXIT_FAILURE;
  }

  return EXIT_SUCCESS;
#endif
}
