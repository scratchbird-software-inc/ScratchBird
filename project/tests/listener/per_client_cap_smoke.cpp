// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

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
  std::string tmpl = "/tmp/sb_lcap.XXXXXX";
  std::vector<char> writable(tmpl.begin(), tmpl.end());
  writable.push_back('\0');
  char* made = ::mkdtemp(writable.data());
  return made == nullptr ? std::filesystem::path{} : std::filesystem::path(made);
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
    std::cerr << "usage: sb_listener_per_client_cap_smoke <sb_listener> <sb_parser_dummy>\n";
    return EXIT_FAILURE;
  }
  const std::filesystem::path listener = argv[1];
  const std::filesystem::path parser = argv[2];
  const auto work = MakeTempDir();
  Require(!work.empty(), "could not create temp dir");
  const int port = FindFreePort();
  Require(port > 0, "could not allocate test port");

  const auto control_dir = work / "c";
  const auto runtime_dir = work / "r";
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
            "--database-selector=dev_bootstrap_path:/tmp/sb_lcap.sbdb",
            "--server-endpoint=unix:/tmp/sb_lcap.sbps.sock",
            parser_arg.c_str(),
            control_arg.c_str(),
            runtime_arg.c_str(),
            "--bind-address=127.0.0.1",
            port_arg.c_str(),
            "--warm-pool-min=2",
            "--warm-pool-max=2",
            "--per-client-max-connections=1",
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

  int first_fd = -1;
  for (int i = 0; i < 100; ++i) {
    first_fd = ConnectLoopback(port);
    if (first_fd >= 0) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  if (first_fd < 0) {
    cleanup();
    std::cerr << "listener did not accept first connection\n";
    return EXIT_FAILURE;
  }

  int second_fd = ConnectLoopback(port);
  if (second_fd >= 0) {
    std::string line;
    const bool got_line = ReadLineWithTimeout(second_fd, &line, 900);
    ::close(second_fd);
    if (got_line && line == "ScratchBird dummy parser ready") {
      ::close(first_fd);
      cleanup();
      std::cerr << "per-client cap admitted a second client unexpectedly\n";
      return EXIT_FAILURE;
    }
  }

  std::string first_line;
  const bool first_ready = ReadLineWithTimeout(first_fd, &first_line, 2500);
  ::close(first_fd);
  cleanup();
  Require(first_ready && first_line == "ScratchBird dummy parser ready",
          "first slow client should still be served while second is cap-refused");
  std::cout << "per_client_cap_smoke=passed work=" << work << '\n';
  return EXIT_SUCCESS;
}
