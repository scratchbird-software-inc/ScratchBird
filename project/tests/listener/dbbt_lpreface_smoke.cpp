// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "control_plane.hpp"
#include "listener_runtime.hpp"

#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <iostream>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace proto = scratchbird::manager::protocol;

namespace {

std::filesystem::path MakeTempDir() {
  std::string tmpl = "/tmp/sb_ldbbt.XXXXXX";
  std::vector<char> writable(tmpl.begin(), tmpl.end());
  writable.push_back('\0');
  char* made = ::mkdtemp(writable.data());
  return made == nullptr ? std::filesystem::path{} : std::filesystem::path(made);
}

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

std::vector<std::uint8_t> ManagementPayload(const std::string& command,
                                            std::uint64_t sequence) {
  if (command == "PING" || command == "STATUS" || command == "HEALTH" ||
      command == "POOL_STATUS" || command == "POOL STATUS") {
    return std::vector<std::uint8_t>(command.begin(), command.end());
  }
  const auto now_ms = proto::CurrentEpochMilliseconds();
  auto envelope = scratchbird::listener::BuildListenerManagementEnvelopeFromCommand(
      command,
      sequence,
      now_ms,
      now_ms + 30000,
      "manager",
      "listener_manager",
      scratchbird::listener::kListenerManagementAuthPeerOwner);
  if (!envelope) return {};
  return scratchbird::listener::EncodeListenerManagementEnvelope(*envelope);
}

CommandResult SendManagementCommand(const std::filesystem::path& socket_path,
                                    const std::string& command,
                                    std::uint64_t sequence) {
  CommandResult result;
  const int fd = ConnectUnix(socket_path);
  if (fd < 0) return result;
  scratchbird::listener::ListenerControlFrame frame;
  frame.opcode = scratchbird::listener::ListenerControlOpcode::kManagementCommand;
  frame.sequence = sequence;
  frame.payload = ManagementPayload(command, sequence);
  if (frame.payload.empty()) {
    ::close(fd);
    return result;
  }
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

proto::Bytes TestDbbtKey() {
  const std::string text = "scratchbird-listener-test-dbbt-key-v1";
  return proto::Bytes(text.begin(), text.end());
}

proto::Bytes MakeDbbt(std::uint32_t listener_id,
                      std::uint64_t issued_at_ms,
                      std::uint64_t expires_at_ms) {
  proto::DbbtToken token;
  for (std::size_t i = 0; i < token.db_uuid.size(); ++i) token.db_uuid[i] = static_cast<std::uint8_t>(0x10 + i);
  for (std::size_t i = 0; i < token.manager_session_id.size(); ++i) {
    token.manager_session_id[i] = static_cast<std::uint8_t>(0x80 + i);
  }
  token.listener_id = listener_id;
  token.issued_at_ms = issued_at_ms;
  token.expires_at_ms = expires_at_ms;
  token.client_nonce = {1, 2, 3, 4, 5, 6, 7, 8,
                        9, 10, 11, 12, 13, 14, 15, 16};
  token.server_nonce = {16, 15, 14, 13, 12, 11, 10, 9,
                        8, 7, 6, 5, 4, 3, 2, 1};
  return proto::EncodeDbbt(token, TestDbbtKey());
}

proto::Bytes MakeLpreface(const proto::Bytes& dbbt) {
  proto::Lpreface preface;
  preface.listener_id = 1;
  preface.dbbt = dbbt;
  preface.db_selector = "dev_bootstrap_path:/tmp/sb_ldbbt.sbdb";
  preface.requested_profile = "SBsql";
  preface.auth_provider_family = "security_database_temporary_token";
  preface.auth_principal = "alice";
  preface.auth_token = "listener-lpreface-token";
  proto::Bytes encoded;
  const auto result = proto::EncodeLpreface(preface, &encoded);
  if (!result.ok) return {};
  return encoded;
}

proto::Bytes MakeLprefaceWithoutAuthToken(const proto::Bytes& dbbt) {
  proto::Lpreface preface;
  preface.listener_id = 1;
  preface.dbbt = dbbt;
  preface.db_selector = "dev_bootstrap_path:/tmp/sb_ldbbt.sbdb";
  preface.requested_profile = "SBsql";
  proto::Bytes encoded;
  const auto result = proto::EncodeLpreface(preface, &encoded);
  if (!result.ok) return {};
  return encoded;
}

void Require(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(EXIT_FAILURE);
  }
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 3) {
    std::cerr << "usage: sb_listener_dbbt_lpreface_smoke <sb_listener> <sb_parser_dummy>\n";
    return EXIT_FAILURE;
  }
  const std::filesystem::path listener = argv[1];
  const std::filesystem::path parser = argv[2];
  const auto work = MakeTempDir();
  Require(!work.empty(), "could not create temp dir");

  const auto control_dir = work / "c";
  const auto runtime_dir = work / "r";
  const auto stdout_path = work / "listener.out";
  const auto stderr_path = work / "listener.err";
  const int port = FindFreePort();
  Require(port > 0, "could not allocate test listener port");

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
            "--database-selector=dev_bootstrap_path:/tmp/sb_ldbbt.sbdb",
            "--server-endpoint=unix:/tmp/sb_ldbbt.sbps.sock",
            parser_arg.c_str(),
            control_arg.c_str(),
            runtime_arg.c_str(),
            "--bind-address=127.0.0.1",
            port_arg.c_str(),
            "--warm-pool-min=1",
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

  std::filesystem::path management_socket;
  for (int i = 0; i < 100; ++i) {
    management_socket = FindManagementSocket(control_dir);
    if (!management_socket.empty()) {
      const int probe_fd = ConnectUnix(management_socket);
      if (probe_fd >= 0) {
        ::close(probe_fd);
        break;
      }
    }
    int status = 0;
    if (::waitpid(pid, &status, WNOHANG) == pid) {
      std::cerr << "listener exited before management socket became reachable\n";
      return EXIT_FAILURE;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  Require(!management_socket.empty(), "management socket was not created");

  const auto now = proto::CurrentEpochMilliseconds();
  const auto valid = MakeDbbt(1, now - 1000, now + 30000);
  const auto valid_lpreface_token = MakeDbbt(1, now - 500, now + 30000);
  const auto future_within_skew = MakeDbbt(1, now + 1000, now + 30000);
  const auto future_outside_skew = MakeDbbt(1, now + 10000, now + 40000);
  const auto wrong_listener = MakeDbbt(7, now - 1000, now + 30000);
  const auto expired = MakeDbbt(1, now - 60000, now - 30000);
  Require(!valid.empty() && !valid_lpreface_token.empty() && !future_within_skew.empty() &&
          !future_outside_skew.empty() && !wrong_listener.empty() && !expired.empty(),
          "DBBT tokens must encode");

  auto valid_dbbt = SendManagementCommand(management_socket, "DBBT_VALIDATE " + proto::Hex(valid), 1);
  Require(valid_dbbt.got_frame && valid_dbbt.ok, "valid DBBT_VALIDATE must succeed");

  auto replay_dbbt = SendManagementCommand(management_socket, "DBBT_VALIDATE " + proto::Hex(valid), 2);
  Require(replay_dbbt.got_frame && !replay_dbbt.ok, "replayed DBBT_VALIDATE must fail");
  Require(replay_dbbt.body.find("MCP.DBBT_REPLAY_DETECTED") != std::string::npos,
          "replayed DBBT must report MCP.DBBT_REPLAY_DETECTED");

  auto skew_ok_dbbt = SendManagementCommand(management_socket, "DBBT_VALIDATE " + proto::Hex(future_within_skew), 3);
  Require(skew_ok_dbbt.got_frame && skew_ok_dbbt.ok, "future DBBT inside clock skew must succeed");

  auto skew_fail_dbbt = SendManagementCommand(management_socket, "DBBT_VALIDATE " + proto::Hex(future_outside_skew), 4);
  Require(skew_fail_dbbt.got_frame && !skew_fail_dbbt.ok, "future DBBT outside clock skew must fail");
  Require(skew_fail_dbbt.body.find("MCP.DBBT_NOT_YET_VALID") != std::string::npos,
          "future DBBT outside clock skew must report MCP.DBBT_NOT_YET_VALID");

  auto wrong_dbbt = SendManagementCommand(management_socket, "DBBT_VALIDATE " + proto::Hex(wrong_listener), 5);
  Require(wrong_dbbt.got_frame && !wrong_dbbt.ok, "wrong-listener DBBT_VALIDATE must fail");
  Require(wrong_dbbt.body.find("MCP.DBBT_LISTENER_MISMATCH") != std::string::npos,
          "wrong-listener DBBT must report MCP.DBBT_LISTENER_MISMATCH");

  auto expired_dbbt = SendManagementCommand(management_socket, "DBBT_VALIDATE " + proto::Hex(expired), 6);
  Require(expired_dbbt.got_frame && !expired_dbbt.ok, "expired DBBT_VALIDATE must fail");

  auto malformed_dbbt = SendManagementCommand(management_socket, "DBBT_VALIDATE nothex", 7);
  Require(malformed_dbbt.got_frame && !malformed_dbbt.ok, "malformed DBBT_VALIDATE must fail");

  const auto valid_lpreface_bytes = MakeLpreface(valid_lpreface_token);
  Require(!valid_lpreface_bytes.empty(), "LPREFACE must encode");
  auto valid_lpreface = SendManagementCommand(management_socket, "LPREFACE_VALIDATE " + proto::Hex(valid_lpreface_bytes), 8);
  if (!valid_lpreface.got_frame || !valid_lpreface.ok) {
    std::cerr << "valid_lpreface_response=" << valid_lpreface.body << '\n';
  }
  Require(valid_lpreface.got_frame && valid_lpreface.ok, "valid LPREFACE_VALIDATE must succeed");
  Require(valid_lpreface.body.find("LISTENER.LPREFACE_ACCEPTED") != std::string::npos,
          "valid LPREFACE must report LISTENER.LPREFACE_ACCEPTED");
  auto status_after_lpreface = SendManagementCommand(management_socket, "STATUS", 11);
  Require(status_after_lpreface.got_frame && status_after_lpreface.ok,
          "STATUS after LPREFACE_VALIDATE must succeed");
  if (status_after_lpreface.body.find("\"pending_handoff_bindings\":1") == std::string::npos) {
    std::cerr << "status_after_lpreface=" << status_after_lpreface.body << '\n';
  }
  Require(status_after_lpreface.body.find("\"pending_handoff_bindings\":1") != std::string::npos,
          "valid LPREFACE must queue one pending handoff binding");

  auto replay_lpreface = SendManagementCommand(management_socket, "LPREFACE_VALIDATE " + proto::Hex(valid_lpreface_bytes), 9);
  Require(replay_lpreface.got_frame && !replay_lpreface.ok, "replayed LPREFACE_VALIDATE must fail");
  Require(replay_lpreface.body.find("MCP.DBBT_REPLAY_DETECTED") != std::string::npos,
          "replayed LPREFACE must report MCP.DBBT_REPLAY_DETECTED");

  auto malformed_lpreface = SendManagementCommand(management_socket, "LPREFACE_VALIDATE 00010203", 10);
  const auto missing_auth_lpreface_bytes = MakeLprefaceWithoutAuthToken(MakeDbbt(1, now - 250, now + 30000));
  auto missing_auth_lpreface = SendManagementCommand(management_socket,
                                                     "LPREFACE_VALIDATE " + proto::Hex(missing_auth_lpreface_bytes),
                                                     12);
  cleanup();
  Require(malformed_lpreface.got_frame && !malformed_lpreface.ok, "malformed LPREFACE_VALIDATE must fail");
  Require(missing_auth_lpreface.got_frame && !missing_auth_lpreface.ok,
          "LPREFACE without security auth token must fail");
  Require(missing_auth_lpreface.body.find("LISTENER.LPREFACE_AUTH_TOKEN_REQUIRED") != std::string::npos,
          "LPREFACE without token must report LISTENER.LPREFACE_AUTH_TOKEN_REQUIRED");

  scratchbird::listener::ListenerConfig firebird_config;
  firebird_config.protocol_family = "firebird";
  firebird_config.database_selector = "dev_bootstrap_path:/tmp/sb_ldbbt_firebird.sbdb";
  firebird_config.server_endpoint = "unix:/tmp/sb_ldbbt_firebird.sbps.sock";
  firebird_config.dbbt_key_source = scratchbird::listener::DbbtKeySource::kTestBuiltin;
  firebird_config.allow_test_dbbt_builtin = true;
  scratchbird::listener::ListenerRuntime firebird_runtime(firebird_config);
  const auto firebird_lpreface = MakeLpreface(MakeDbbt(1,
                                                       proto::CurrentEpochMilliseconds() - 500,
                                                       proto::CurrentEpochMilliseconds() + 30000));
  auto firebird_result = firebird_runtime.HandleManagementCommand("LPREFACE_VALIDATE " + proto::Hex(firebird_lpreface));
  Require(firebird_result.exit_code != 0,
          "raw LPREFACE_VALIDATE must be refused before manager-mediated handoff handling");
  Require(scratchbird::listener::MessageVectorSetJson(firebird_result.messages).find("LISTENER.MANAGEMENT.ENVELOPE_REQUIRED") != std::string::npos,
          "raw LPREFACE_VALIDATE rejection must require a structured management envelope");

  std::cout << "dbbt_lpreface_smoke=passed work=" << work << '\n';
  return EXIT_SUCCESS;
}
