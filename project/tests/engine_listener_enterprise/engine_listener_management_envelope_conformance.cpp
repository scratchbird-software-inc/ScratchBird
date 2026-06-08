// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "control_plane.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <iostream>
#include <netinet/in.h>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace proto = scratchbird::manager::protocol;

namespace {

int FindFreePort() {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return 0;
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;
  if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    ::close(fd);
    return 0;
  }
  socklen_t len = sizeof(addr);
  if (::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
    ::close(fd);
    return 0;
  }
  const int port = ntohs(addr.sin_port);
  ::close(fd);
  return port;
}

std::filesystem::path MakeTempDir() {
  std::string tmpl = "/tmp/sb_eler061.XXXXXX";
  std::vector<char> writable(tmpl.begin(), tmpl.end());
  writable.push_back('\0');
  char* made = ::mkdtemp(writable.data());
  return made == nullptr ? std::filesystem::path{} : std::filesystem::path(made);
}

std::filesystem::path FindManagementSocket(const std::filesystem::path& control_dir) {
  std::error_code ec;
  if (!std::filesystem::exists(control_dir, ec)) return {};
  for (const auto& entry : std::filesystem::directory_iterator(control_dir, ec)) {
    if (ec) return {};
    const auto path = entry.path();
    const auto name = path.filename().string();
    if (name.size() >= std::string(".management.sock").size() &&
        name.ends_with(".management.sock")) {
      return path;
    }
  }
  return {};
}

int ConnectUnix(const std::filesystem::path& path) {
  const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  const auto text = path.string();
  if (text.size() >= sizeof(addr.sun_path)) {
    ::close(fd);
    return -1;
  }
  std::strncpy(addr.sun_path, text.c_str(), sizeof(addr.sun_path) - 1);
  if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    ::close(fd);
    return -1;
  }
  return fd;
}

struct CommandResult {
  bool got_frame{false};
  bool ok{false};
  std::string body;
};

CommandResult SendPayload(const std::filesystem::path& socket_path,
                          const std::vector<std::uint8_t>& payload,
                          std::uint64_t sequence) {
  CommandResult result;
  const int fd = ConnectUnix(socket_path);
  if (fd < 0) return result;
  scratchbird::listener::ListenerControlFrame frame;
  frame.opcode = scratchbird::listener::ListenerControlOpcode::kManagementCommand;
  frame.sequence = sequence;
  frame.payload = payload;
  if (!scratchbird::listener::SendControlFrame(fd, frame)) {
    ::close(fd);
    return result;
  }
  scratchbird::listener::ListenerControlDecodeResult decoded;
  int received_fd = -1;
  if (!scratchbird::listener::ReadControlFrame(fd, &decoded, &received_fd, 3000)) {
    if (received_fd >= 0) ::close(received_fd);
    ::close(fd);
    return result;
  }
  if (received_fd >= 0) ::close(received_fd);
  ::close(fd);
  result.got_frame = decoded.frame.opcode == scratchbird::listener::ListenerControlOpcode::kManagementResponse &&
                     !decoded.frame.payload.empty();
  if (!result.got_frame) return result;
  result.ok = decoded.frame.payload[0] == 0;
  result.body.assign(decoded.frame.payload.begin() + 1, decoded.frame.payload.end());
  return result;
}

std::vector<std::uint8_t> RawPayload(const std::string& command) {
  return std::vector<std::uint8_t>(command.begin(), command.end());
}

std::vector<std::uint8_t> EnvelopePayload(const std::string& command,
                                          std::uint64_t sequence,
                                          std::uint64_t issued_at_ms,
                                          std::uint64_t expires_at_ms,
                                          const std::string& role = "manager",
                                          const std::string& authority_class = "listener_manager",
                                          std::uint32_t rights = scratchbird::listener::kListenerManagementRightsAll) {
  auto envelope = scratchbird::listener::BuildListenerManagementEnvelopeFromCommand(
      command,
      sequence,
      issued_at_ms,
      expires_at_ms,
      role,
      authority_class,
      scratchbird::listener::kListenerManagementAuthPeerOwner);
  if (!envelope) return {};
  envelope->rights = rights;
  return scratchbird::listener::EncodeListenerManagementEnvelope(*envelope);
}

proto::Bytes TestDbbtKey() {
  const std::string text = "scratchbird-listener-test-dbbt-key-v1";
  return proto::Bytes(text.begin(), text.end());
}

std::vector<std::uint8_t> HmacEnvelopePayload(const std::string& command,
                                              std::uint64_t sequence,
                                              std::uint64_t issued_at_ms,
                                              std::uint64_t expires_at_ms,
                                              bool tamper = false) {
  auto envelope = scratchbird::listener::BuildListenerManagementEnvelopeFromCommand(
      command,
      sequence,
      issued_at_ms,
      expires_at_ms,
      "manager",
      "listener_manager",
      scratchbird::listener::kListenerManagementAuthHmacSha256);
  if (!envelope) return {};
  scratchbird::listener::SignListenerManagementEnvelopeHmacSha256(&*envelope, TestDbbtKey());
  if (tamper && !envelope->authenticator.empty()) {
    envelope->authenticator[0] ^= 0x80u;
  }
  return scratchbird::listener::EncodeListenerManagementEnvelope(*envelope);
}

proto::Bytes MakeDbbt(std::uint64_t issued_at_ms, std::uint64_t expires_at_ms) {
  proto::DbbtToken token;
  for (std::size_t i = 0; i < token.db_uuid.size(); ++i) {
    token.db_uuid[i] = static_cast<std::uint8_t>(0x20 + i);
  }
  for (std::size_t i = 0; i < token.manager_session_id.size(); ++i) {
    token.manager_session_id[i] = static_cast<std::uint8_t>(0x90 + i);
  }
  token.listener_id = 1;
  token.issued_at_ms = issued_at_ms;
  token.expires_at_ms = expires_at_ms;
  token.client_nonce = {1, 2, 3, 4, 5, 6, 7, 8,
                        9, 10, 11, 12, 13, 14, 15, 16};
  token.server_nonce = {16, 15, 14, 13, 12, 11, 10, 9,
                        8, 7, 6, 5, 4, 3, 2, 1};
  return proto::EncodeDbbt(token, TestDbbtKey());
}

void Require(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(EXIT_FAILURE);
  }
}

void RequireBodyContains(const CommandResult& result,
                         const std::string& needle,
                         const std::string& message) {
  if (result.body.find(needle) == std::string::npos) {
    std::cerr << message << "\nbody=" << result.body << '\n';
    std::exit(EXIT_FAILURE);
  }
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 3) {
    std::cerr << "usage: engine_listener_management_envelope_conformance <sb_listener> <sb_parser_dummy>\n";
    return EXIT_FAILURE;
  }
  const std::filesystem::path listener = argv[1];
  const std::filesystem::path parser = argv[2];
  const auto work = MakeTempDir();
  Require(!work.empty(), "could not create temp dir");
  const int port = FindFreePort();
  Require(port > 0, "could not allocate listener port");

  const auto control_dir = work / "control";
  const auto runtime_dir = work / "runtime";
  const auto stdout_path = work / "listener.out";
  const auto stderr_path = work / "listener.err";

  const pid_t pid = ::fork();
  if (pid == 0) {
    int out = ::creat(stdout_path.c_str(), 0600);
    int err = ::creat(stderr_path.c_str(), 0600);
    if (out >= 0) {
      ::dup2(out, STDOUT_FILENO);
      ::close(out);
    }
    if (err >= 0) {
      ::dup2(err, STDERR_FILENO);
      ::close(err);
    }
    const std::string parser_arg = "--parser-executable=" + parser.string();
    const std::string control_arg = "--control-dir=" + control_dir.string();
    const std::string runtime_arg = "--runtime-dir=" + runtime_dir.string();
    const std::string port_arg = "--port=" + std::to_string(port);
    ::execl(listener.c_str(),
            listener.c_str(),
            "--foreground",
            "--protocol-family=sbsql",
            "--listener-profile=default",
            "--bundle-contract-id=bundle.default@1",
            "--database-selector=dev_bootstrap_path:/tmp/sb_eler061.sbdb",
            "--server-endpoint=unix:/tmp/sb_eler061.sbps.sock",
            parser_arg.c_str(),
            control_arg.c_str(),
            runtime_arg.c_str(),
            "--bind-address=127.0.0.1",
            port_arg.c_str(),
            "--warm-pool-min=0",
            "--warm-pool-max=1",
            "--dbbt-key-source=test_builtin",
            "--allow-test-dbbt-builtin=true",
            nullptr);
    _exit(127);
  }
  Require(pid > 0, "could not fork listener");

  auto cleanup = [&] {
    ::kill(pid, SIGTERM);
    int status = 0;
    for (int i = 0; i < 100; ++i) {
      const auto rc = ::waitpid(pid, &status, WNOHANG);
      if (rc == pid) return;
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    ::kill(pid, SIGKILL);
    ::waitpid(pid, &status, 0);
  };
  auto cleanup_workdir = [&] {
    std::error_code ec;
    std::filesystem::remove_all(work, ec);
  };

  std::filesystem::path management_socket;
  for (int i = 0; i < 100; ++i) {
    management_socket = FindManagementSocket(control_dir);
    if (!management_socket.empty()) {
      auto probe = SendPayload(management_socket, RawPayload("STATUS"), 1);
      if (probe.got_frame && probe.ok) break;
    }
    int status = 0;
    if (::waitpid(pid, &status, WNOHANG) == pid) {
      std::cerr << "listener exited before management socket became reachable\n";
      return EXIT_FAILURE;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  Require(!management_socket.empty(), "management socket was not created");

  auto raw_status = SendPayload(management_socket, RawPayload("STATUS"), 2);
  Require(raw_status.got_frame && raw_status.ok, "raw STATUS must remain compatible");
  auto raw_health = SendPayload(management_socket, RawPayload("HEALTH"), 3);
  Require(raw_health.got_frame && raw_health.ok, "raw HEALTH must remain compatible");

  auto raw_drain = SendPayload(management_socket, RawPayload("DRAIN"), 4);
  Require(raw_drain.got_frame && !raw_drain.ok, "raw privileged DRAIN must be refused");
  RequireBodyContains(raw_drain,
                      "LISTENER.MANAGEMENT.ENVELOPE_REQUIRED",
                      "raw privileged refusal must require SBME envelope");

  const auto now = proto::CurrentEpochMilliseconds();
  auto hmac_drain = SendPayload(management_socket,
                                HmacEnvelopePayload("DRAIN", 5, now - 100, now + 30000),
                                5);
  Require(hmac_drain.got_frame && hmac_drain.ok,
          "hmac-sha256 DRAIN envelope must succeed with listener DBBT key material");

  auto tampered_hmac = SendPayload(management_socket,
                                   HmacEnvelopePayload("UNDRAIN",
                                                       6,
                                                       now - 100,
                                                       now + 30000,
                                                       true),
                                   6);
  Require(tampered_hmac.got_frame && !tampered_hmac.ok,
          "tampered hmac-sha256 SBME envelope must be refused");
  RequireBodyContains(tampered_hmac,
                      "LISTENER.MANAGEMENT.AUTHENTICATOR_INVALID",
                      "tampered hmac-sha256 SBME envelope must report authenticator failure");

  auto drain = SendPayload(management_socket,
                           EnvelopePayload("DRAIN", 7, now - 100, now + 30000),
                           7);
  Require(drain.got_frame && drain.ok, "peer-owner-v1 DRAIN envelope must succeed");

  const auto replay_payload = EnvelopePayload("UNDRAIN", 8, now, now + 30000);
  auto undrain = SendPayload(management_socket, replay_payload, 8);
  Require(undrain.got_frame && undrain.ok, "first peer-owner-v1 UNDRAIN envelope must succeed");
  auto replay = SendPayload(management_socket, replay_payload, 8);
  Require(replay.got_frame && !replay.ok, "duplicate live SBME envelope must be refused as replay");
  RequireBodyContains(replay,
                      "LISTENER.MANAGEMENT.REPLAY_DETECTED",
                      "duplicate SBME envelope must report replay diagnostic");

  auto expired = SendPayload(management_socket,
                             EnvelopePayload("DRAIN", 9, now - 60000, now - 30000),
                             9);
  Require(expired.got_frame && !expired.ok, "expired SBME envelope must be refused");
  RequireBodyContains(expired,
                      "LISTENER.MANAGEMENT.WINDOW_EXPIRED",
                      "expired SBME envelope must report window diagnostic");

  auto insufficient = SendPayload(management_socket,
                                  EnvelopePayload("DRAIN",
                                                  10,
                                                  now,
                                                  now + 30000,
                                                  "reader",
                                                  "listener_reader",
                                                  scratchbird::listener::kListenerManagementRightRead),
                                  10);
  Require(insufficient.got_frame && !insufficient.ok,
          "SBME envelope without lifecycle right must be refused");
  RequireBodyContains(insufficient,
                      "LISTENER.MANAGEMENT.RIGHT_DENIED",
                      "insufficient-rights SBME envelope must report right denial");

  const std::vector<std::uint8_t> malformed = {'S', 'B', 'M', 'E', 1};
  auto malformed_result = SendPayload(management_socket, malformed, 11);
  Require(malformed_result.got_frame && !malformed_result.ok,
          "malformed SBME payload must be refused");
  RequireBodyContains(malformed_result,
                      "LISTENER.MANAGEMENT.ENVELOPE_MALFORMED",
                      "malformed SBME payload must report malformed diagnostic");

  const auto dbbt = MakeDbbt(now - 1000, now + 30000);
  auto dbbt_result = SendPayload(management_socket,
                                 EnvelopePayload("DBBT_VALIDATE " + proto::Hex(dbbt),
                                                 12,
                                                 now,
                                                 now + 30000),
                                 12);
  cleanup();
  cleanup_workdir();
  Require(dbbt_result.got_frame && dbbt_result.ok,
          "DBBT_VALIDATE must pass through authenticated SBME management path");

  std::cout << "ELER-061 management envelope conformance passed\n";
  return EXIT_SUCCESS;
}
