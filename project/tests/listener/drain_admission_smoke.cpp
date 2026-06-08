// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "control_plane.hpp"

#include <arpa/inet.h>
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
  std::string tmpl = "/tmp/sb_listener_drain_admission.XXXXXX";
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

bool SendManagementCommand(const std::filesystem::path& socket_path,
                           const std::string& command,
                           std::uint64_t sequence) {
  const int fd = ConnectUnix(socket_path);
  if (fd < 0) return false;
  scratchbird::listener::ListenerControlFrame frame;
  frame.opcode = scratchbird::listener::ListenerControlOpcode::kManagementCommand;
  frame.sequence = sequence;
  if (command == "PING" || command == "STATUS" || command == "HEALTH" ||
      command == "POOL_STATUS" || command == "POOL STATUS") {
    frame.payload.assign(command.begin(), command.end());
  } else {
    const auto now_ms = scratchbird::listener::proto::CurrentEpochMilliseconds();
    auto envelope = scratchbird::listener::BuildListenerManagementEnvelopeFromCommand(
        command,
        sequence,
        now_ms,
        now_ms + 30000,
        "manager",
        "listener_manager",
        scratchbird::listener::kListenerManagementAuthPeerOwner);
    if (!envelope) {
      ::close(fd);
      return false;
    }
    frame.payload = scratchbird::listener::EncodeListenerManagementEnvelope(*envelope);
  }
  if (!scratchbird::listener::SendControlFrame(fd, frame)) {
    ::close(fd);
    return false;
  }
  scratchbird::listener::ListenerControlDecodeResult decoded;
  int received_fd = -1;
  const bool read_ok = scratchbird::listener::ReadControlFrame(fd, &decoded, &received_fd, 3000);
  if (received_fd >= 0) ::close(received_fd);
  ::close(fd);
  return read_ok &&
         decoded.frame.opcode == scratchbird::listener::ListenerControlOpcode::kManagementResponse &&
         !decoded.frame.payload.empty() &&
         decoded.frame.payload[0] == 0;
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
    std::cerr << "usage: sb_listener_drain_admission_smoke <sb_listener> <sb_parser_dummy>\n";
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
    const std::string port_arg = "--port=" + std::to_string(port);
    const std::string parser_arg = "--parser-executable=" + parser.string();
    const std::string control_arg = "--control-dir=" + control_dir.string();
    const std::string runtime_arg = "--runtime-dir=" + runtime_dir.string();
    ::execl(listener.c_str(),
            listener.c_str(),
            "--foreground",
            "--protocol-family=sbsql",
            "--listener-profile=default",
            "--bundle-contract-id=bundle.default@1",
            "--database-selector=dev_bootstrap_path:/tmp/sb_listener_drain_admission.sbdb",
            "--server-endpoint=unix:/tmp/sb_listener_drain_admission.sbps.sock",
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
  Require(SendManagementCommand(management_socket, "DRAIN", 1), "DRAIN command must succeed");

  int drained_fd = ConnectLoopback(port);
  if (drained_fd >= 0) {
    std::string line;
    const bool got_line = ReadLineWithTimeout(drained_fd, &line, 700);
    ::close(drained_fd);
    if (got_line && line == "ScratchBird dummy parser ready") {
      cleanup();
      std::cerr << "drained listener admitted a new client\n";
      return EXIT_FAILURE;
    }
    if (!got_line ||
        line.find("\"message_vector_set\"") == std::string::npos ||
        line.find("LISTENER.POOL_DRAIN_REJECT") == std::string::npos) {
      cleanup();
      std::cerr << "drained listener did not emit structured drain refusal diagnostic; line=" << line << '\n';
      return EXIT_FAILURE;
    }
  }

  Require(SendManagementCommand(management_socket, "UNDRAIN", 2), "UNDRAIN command must succeed");
  int admitted_fd = -1;
  for (int i = 0; i < 40; ++i) {
    admitted_fd = ConnectLoopback(port);
    if (admitted_fd >= 0) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  if (admitted_fd < 0) {
    cleanup();
    std::cerr << "undrained listener did not accept a new connection\n";
    return EXIT_FAILURE;
  }
  std::string line;
  const bool got_line = ReadLineWithTimeout(admitted_fd, &line, 2000);
  ::close(admitted_fd);
  cleanup();
  if (!got_line || line != "ScratchBird dummy parser ready") {
    std::cerr << "undrained listener did not restore admission; greeting=" << line << '\n';
    return EXIT_FAILURE;
  }
  std::cout << "drain_admission_smoke=passed work=" << work << '\n';
  return EXIT_SUCCESS;
}
