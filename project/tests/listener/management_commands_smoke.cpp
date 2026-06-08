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

namespace {

int FindFreePort() {
  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
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

int ConnectLoopback(int port) {
  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons(static_cast<std::uint16_t>(port));
  if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    ::close(fd);
    return -1;
  }
  return fd;
}

bool ReadLineWithTimeout(int fd, std::string* line, int timeout_ms) {
  line->clear();
  char ch = 0;
  for (;;) {
    pollfd pfd{};
    pfd.fd = fd;
    pfd.events = POLLIN;
    const int poll_rc = ::poll(&pfd, 1, timeout_ms);
    if (poll_rc == 0) return false;
    if (poll_rc < 0 && errno == EINTR) continue;
    if (poll_rc < 0) return false;
    if ((pfd.revents & (POLLHUP | POLLERR | POLLNVAL)) != 0 && (pfd.revents & POLLIN) == 0) {
      return false;
    }
    const auto rc = ::read(fd, &ch, 1);
    if (rc == 1) {
      if (ch == '\n') return true;
      line->push_back(ch);
      continue;
    }
    if (rc < 0 && errno == EINTR) continue;
    return false;
  }
}

std::filesystem::path MakeTempDir() {
  std::string tmpl = "/tmp/sb_listener_management_smoke.XXXXXX";
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

std::vector<std::uint8_t> ManagementPayload(const std::string& command,
                                            std::uint64_t sequence) {
  if (command == "PING" || command == "STATUS" || command == "HEALTH" ||
      command == "POOL_STATUS" || command == "POOL STATUS") {
    return std::vector<std::uint8_t>(command.begin(), command.end());
  }
  const auto now_ms = scratchbird::listener::proto::CurrentEpochMilliseconds();
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

std::string ExtractFirstWorkerId(const std::string& status_body) {
  const std::string key = "\"worker_id\":\"";
  const auto start = status_body.find(key);
  if (start == std::string::npos) return {};
  const auto value_start = start + key.size();
  const auto value_end = status_body.find('"', value_start);
  if (value_end == std::string::npos || value_end <= value_start) return {};
  return status_body.substr(value_start, value_end - value_start);
}

std::string ExtractFirstActiveConnectionId(const std::string& status_body) {
  const std::string key = "\"active_connection_id\":";
  std::size_t pos = 0;
  while ((pos = status_body.find(key, pos)) != std::string::npos) {
    const auto value_start = pos + key.size();
    auto value_end = value_start;
    while (value_end < status_body.size() &&
           status_body[value_end] >= '0' &&
           status_body[value_end] <= '9') {
      ++value_end;
    }
    if (value_end > value_start) {
      const auto value = status_body.substr(value_start, value_end - value_start);
      if (value != "0") return value;
    }
    pos = value_end;
  }
  return {};
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
    std::cerr << "usage: sb_listener_management_commands_smoke <sb_listener> <sb_parser_dummy>\n";
    return EXIT_FAILURE;
  }
  const std::filesystem::path listener = argv[1];
  const std::filesystem::path parser = argv[2];
  const auto work = MakeTempDir();
  Require(!work.empty(), "could not create temp dir");
  const int port = FindFreePort();
  Require(port > 0, "could not allocate test port");

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
            "--database-selector=dev_bootstrap_path:/tmp/sb_listener_management_smoke.sbdb",
            "--server-endpoint=unix:/tmp/sb_listener_management_smoke.sbps.sock",
            parser_arg.c_str(),
            control_arg.c_str(),
            runtime_arg.c_str(),
            "--bind-address=127.0.0.1",
            port_arg.c_str(),
            "--warm-pool-min=1",
            "--warm-pool-max=2",
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

  auto status = SendManagementCommand(management_socket, "STATUS", 1);
  Require(status.got_frame && status.ok, "STATUS command must succeed");
  Require(status.body.find("last_accept_stage") != std::string::npos,
          "STATUS must expose accept-loop instrumentation");

  auto ping = SendManagementCommand(management_socket, "PING", 2);
  Require(ping.got_frame && ping.ok, "PING command must succeed");

  auto health = SendManagementCommand(management_socket, "HEALTH", 3);
  Require(health.got_frame && health.ok, "HEALTH command must succeed");

  auto pool_status = SendManagementCommand(management_socket, "POOL_STATUS", 4);
  Require(pool_status.got_frame && pool_status.ok, "POOL_STATUS command must succeed");
  Require(pool_status.body.find("\"pool\"") != std::string::npos,
          "POOL_STATUS response must expose parser pool status");

  auto drain = SendManagementCommand(management_socket, "DRAIN", 5);
  Require(drain.got_frame && drain.ok, "DRAIN command must succeed");

  auto undrain = SendManagementCommand(management_socket, "UNDRAIN", 6);
  Require(undrain.got_frame && undrain.ok, "UNDRAIN command must succeed");

  auto resize = SendManagementCommand(management_socket, "POOL_RESIZE 1 2", 7);
  Require(resize.got_frame && resize.ok, "POOL_RESIZE command must succeed");

  auto post_resize_status = SendManagementCommand(management_socket, "STATUS", 8);
  Require(post_resize_status.got_frame && post_resize_status.ok, "STATUS after POOL_RESIZE must succeed");
  Require(post_resize_status.body.find("\"parser_pool_ready\"") != std::string::npos,
          "STATUS must expose parser_pool_ready state");
  Require(post_resize_status.body.find("\"warm_workers\"") != std::string::npos,
          "STATUS must expose warm worker count");
  Require(post_resize_status.body.find("\"running_workers\"") != std::string::npos,
          "STATUS must expose running worker count");
  Require(post_resize_status.body.find("\"state_class\":\"IDLE\"") != std::string::npos,
          "STATUS must expose P1 parser worker state class");
  const auto worker_id = ExtractFirstWorkerId(post_resize_status.body);
  Require(!worker_id.empty(), "STATUS after POOL_RESIZE must include a worker id");

  auto recycle = SendManagementCommand(management_socket, "POOL_RECYCLE " + worker_id, 9);
  Require(recycle.got_frame && recycle.ok, "POOL_RECYCLE command must succeed");

  auto post_recycle_status = SendManagementCommand(management_socket, "STATUS", 10);
  Require(post_recycle_status.got_frame && post_recycle_status.ok, "STATUS after POOL_RECYCLE must succeed");
  const auto restarted_worker_id = ExtractFirstWorkerId(post_recycle_status.body);
  Require(!restarted_worker_id.empty(), "STATUS after POOL_RECYCLE must include a worker id");

  auto kill_existing = SendManagementCommand(management_socket, "POOL_KILL worker_id=" + restarted_worker_id, 11);
  Require(kill_existing.got_frame && kill_existing.ok, "POOL_KILL worker_id=<id> must succeed");

  auto pool_restart = SendManagementCommand(management_socket, "POOL RESTART", 12);
  Require(pool_restart.got_frame && pool_restart.ok, "POOL RESTART command must succeed");

  int client_fd = -1;
  for (int i = 0; i < 40; ++i) {
    client_fd = ConnectLoopback(port);
    if (client_fd >= 0) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  Require(client_fd >= 0, "listener must accept a client for connection_id kill test");
  std::string greeting;
  Require(ReadLineWithTimeout(client_fd, &greeting, 2500) && greeting == "ScratchBird dummy parser ready",
          "client must receive parser greeting before connection_id kill");

  auto active_status = SendManagementCommand(management_socket, "STATUS", 13);
  Require(active_status.got_frame && active_status.ok, "STATUS with active client must succeed");
  const auto active_connection_id = ExtractFirstActiveConnectionId(active_status.body);
  Require(!active_connection_id.empty(), "STATUS must expose active_connection_id for parser-owned client");

  auto kill_connection = SendManagementCommand(management_socket, "POOL_KILL connection_id=" + active_connection_id, 14);
  Require(kill_connection.got_frame && kill_connection.ok, "POOL_KILL connection_id=<id> must succeed");
  ::close(client_fd);

  auto pool_restart_after_connection_kill = SendManagementCommand(management_socket, "POOL RESTART", 15);
  Require(pool_restart_after_connection_kill.got_frame && pool_restart_after_connection_kill.ok,
          "POOL RESTART after connection_id kill must succeed");

  auto kill_invalid_connection = SendManagementCommand(management_socket, "POOL_KILL connection_id=0", 16);
  Require(kill_invalid_connection.got_frame && !kill_invalid_connection.ok,
          "POOL_KILL connection_id=0 must be refused");
  Require(kill_invalid_connection.body.find("LISTENER.KILL_INVALID_CONNECTION_ID") != std::string::npos,
          "POOL_KILL invalid connection refusal must be structured");

  auto kill_missing = SendManagementCommand(management_socket, "POOL_KILL does-not-exist", 17);
  Require(kill_missing.got_frame && !kill_missing.ok, "POOL_KILL malformed argument must be refused");
  Require(kill_missing.body.find("LISTENER.POOL.KILL_ARGUMENT_INVALID") != std::string::npos,
          "POOL_KILL malformed argument refusal must be structured");

  auto reload = SendManagementCommand(management_socket, "RELOAD", 18);
  Require(reload.got_frame && reload.ok, "RELOAD command must succeed");

  cleanup();
  std::cout << "listener_management_commands_smoke=passed work=" << work << '\n';
  return EXIT_SUCCESS;
}
