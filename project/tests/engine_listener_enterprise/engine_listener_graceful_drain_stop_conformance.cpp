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
    if ((pfd.revents & (POLLHUP | POLLERR | POLLNVAL)) != 0 &&
        (pfd.revents & POLLIN) == 0) {
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

bool WriteAll(int fd, const std::string& text) {
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

std::filesystem::path MakeTempDir() {
  std::string tmpl = "/tmp/sb_eler064.XXXXXX";
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
                                    std::uint64_t sequence,
                                    std::uint32_t timeout_ms = 3000) {
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
  if (!scratchbird::listener::ReadControlFrame(fd, &decoded, &received_fd, timeout_ms)) {
    if (received_fd >= 0) ::close(received_fd);
    ::close(fd);
    return result;
  }
  if (received_fd >= 0) ::close(received_fd);
  ::close(fd);
  result.got_frame = decoded.frame.opcode ==
                         scratchbird::listener::ListenerControlOpcode::kManagementResponse &&
                     !decoded.frame.payload.empty();
  if (!result.got_frame) return result;
  result.ok = decoded.frame.payload[0] == 0;
  result.body.assign(decoded.frame.payload.begin() + 1, decoded.frame.payload.end());
  return result;
}

void Require(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(EXIT_FAILURE);
  }
}

bool WaitForStatus(const std::filesystem::path& management_socket,
                   std::uint64_t sequence_base,
                   std::initializer_list<std::string> needles,
                   std::string* last_status) {
  for (int i = 0; i < 80; ++i) {
    auto status = SendManagementCommand(management_socket,
                                        "STATUS",
                                        sequence_base + static_cast<std::uint64_t>(i),
                                        1000);
    if (status.got_frame && status.ok) {
      *last_status = status.body;
      bool all_present = true;
      for (const auto& needle : needles) {
        all_present = all_present && status.body.find(needle) != std::string::npos;
      }
      if (all_present) return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  return false;
}

bool WaitPidExit(pid_t pid, int* status) {
  for (int i = 0; i < 160; ++i) {
    const auto rc = ::waitpid(pid, status, WNOHANG);
    if (rc == pid) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  return false;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 3) {
    std::cerr << "usage: engine_listener_graceful_drain_stop_conformance <sb_listener> <sb_parser_dummy>\n";
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
    ::setenv("SB_PARSER_DUMMY_BEHAVIOR", "slow_read", 1);
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
            "--database-selector=dev_bootstrap_path:/tmp/sb_eler064.sbdb",
            "--server-endpoint=unix:/tmp/sb_eler064.sbps.sock",
            parser_arg.c_str(),
            control_arg.c_str(),
            runtime_arg.c_str(),
            "--bind-address=127.0.0.1",
            port_arg.c_str(),
            "--warm-pool-min=2",
            "--warm-pool-max=2",
            "--handoff-ack-timeout-ms=2000",
            "--graceful-drain-timeout-ms=200",
            nullptr);
    _exit(127);
  }
  Require(pid > 0, "could not fork listener");

  auto force_cleanup = [&] {
    int status = 0;
    if (::waitpid(pid, &status, WNOHANG) == pid) return;
    ::kill(pid, SIGKILL);
    ::waitpid(pid, &status, 0);
  };

  std::filesystem::path management_socket;
  for (int i = 0; i < 100; ++i) {
    management_socket = FindManagementSocket(control_dir);
    if (!management_socket.empty()) {
      auto probe = SendManagementCommand(management_socket, "STATUS", 1, 1000);
      if (probe.got_frame && probe.ok) break;
    }
    int status = 0;
    if (::waitpid(pid, &status, WNOHANG) == pid) {
      std::cerr << "listener exited before management socket became reachable\n";
      return EXIT_FAILURE;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  if (management_socket.empty()) {
    force_cleanup();
    std::cerr << "management socket was not created\n";
    return EXIT_FAILURE;
  }

  int active_fd = -1;
  for (int i = 0; i < 100; ++i) {
    active_fd = ConnectLoopback(port);
    if (active_fd >= 0) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  if (active_fd < 0) {
    force_cleanup();
    std::cerr << "listener did not accept active connection\n";
    return EXIT_FAILURE;
  }

  std::string status_body;
  if (!WaitForStatus(management_socket,
                     20,
                     {"\"handoff_complete_total\":1",
                      "\"busy_worker_count\":1",
                      "\"accepting_new_handoffs\":true"},
                     &status_body)) {
    ::close(active_fd);
    force_cleanup();
    std::cerr << "active parser-owned client was not reflected in status: " << status_body << '\n';
    return EXIT_FAILURE;
  }

  auto drain = SendManagementCommand(management_socket, "DRAIN", 100, 1000);
  if (!drain.got_frame || !drain.ok ||
      drain.body.find("\"draining\":true") == std::string::npos ||
      drain.body.find("\"accepting_new_connections\":false") == std::string::npos ||
      drain.body.find("\"busy_worker_count\":1") == std::string::npos) {
    ::close(active_fd);
    force_cleanup();
    std::cerr << "DRAIN did not publish bounded active-worker status: " << drain.body << '\n';
    return EXIT_FAILURE;
  }

  const int refused_fd = ConnectLoopback(port);
  Require(refused_fd >= 0, "draining listener did not accept a refusal probe");
  std::string refused_line;
  const bool got_refusal = ReadLineWithTimeout(refused_fd, &refused_line, 1000);
  ::close(refused_fd);
  if (!got_refusal ||
      refused_line.find("LISTENER.POOL_DRAIN_REJECT") == std::string::npos) {
    ::close(active_fd);
    force_cleanup();
    std::cerr << "draining listener did not emit structured refusal: " << refused_line << '\n';
    return EXIT_FAILURE;
  }

  auto timeout_stop = SendManagementCommand(management_socket, "STOP GRACEFUL", 200, 3000);
  if (!timeout_stop.got_frame || timeout_stop.ok ||
      timeout_stop.body.find("LISTENER.POOL.DRAIN_TIMEOUT") == std::string::npos ||
      timeout_stop.body.find("force_required") == std::string::npos) {
    ::close(active_fd);
    force_cleanup();
    std::cerr << "STOP GRACEFUL did not time out safely with active work: " << timeout_stop.body << '\n';
    return EXIT_FAILURE;
  }
  if (::kill(pid, 0) != 0) {
    ::close(active_fd);
    force_cleanup();
    std::cerr << "listener exited after graceful timeout instead of staying draining\n";
    return EXIT_FAILURE;
  }

  if (!WaitForStatus(management_socket,
                     300,
                     {"\"draining\":true",
                      "\"stop_requested\":false",
                      "\"busy_worker_count\":1"},
                     &status_body)) {
    ::close(active_fd);
    force_cleanup();
    std::cerr << "post-timeout status did not keep listener draining: " << status_body << '\n';
    return EXIT_FAILURE;
  }

  std::string line;
  if (!ReadLineWithTimeout(active_fd, &line, 2500) ||
      line != "ScratchBird dummy parser ready") {
    ::close(active_fd);
    force_cleanup();
    std::cerr << "active client did not complete after drain timeout: " << line << '\n';
    return EXIT_FAILURE;
  }
  if (!WriteAll(active_fd, "drain-stop\n") ||
      !ReadLineWithTimeout(active_fd, &line, 1000) ||
      line != "drain-stop") {
    ::close(active_fd);
    force_cleanup();
    std::cerr << "active client echo failed after drain timeout: " << line << '\n';
    return EXIT_FAILURE;
  }
  ::close(active_fd);

  if (!WaitForStatus(management_socket,
                     500,
                     {"\"busy_worker_count\":0",
                      "\"draining\":true"},
                     &status_body)) {
    force_cleanup();
    std::cerr << "completed active client was not removed from drain accounting: " << status_body << '\n';
    return EXIT_FAILURE;
  }

  auto graceful_stop = SendManagementCommand(management_socket, "STOP GRACEFUL", 700, 3000);
  if (!graceful_stop.got_frame || !graceful_stop.ok ||
      graceful_stop.body.find("LISTENER.POOL.DRAIN_COMPLETE") == std::string::npos) {
    force_cleanup();
    std::cerr << "STOP GRACEFUL did not complete after active work drained: " << graceful_stop.body << '\n';
    return EXIT_FAILURE;
  }
  int status = 0;
  if (!WaitPidExit(pid, &status)) {
    force_cleanup();
    std::cerr << "listener did not exit after completed graceful stop\n";
    return EXIT_FAILURE;
  }
  Require(WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "listener must exit cleanly after completed graceful stop");
  std::error_code ec;
  std::filesystem::remove_all(work, ec);
  std::cout << "engine_listener_graceful_drain_stop_conformance=passed\n";
  return EXIT_SUCCESS;
}
