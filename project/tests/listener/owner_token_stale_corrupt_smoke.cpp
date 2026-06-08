// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "listener_socket_identity.hpp"

#include <arpa/inet.h>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <netinet/in.h>
#include <poll.h>
#include <sstream>
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
  std::string tmpl = "/tmp/sb_lost.XXXXXX";
  std::vector<char> writable(tmpl.begin(), tmpl.end());
  writable.push_back('\0');
  char* made = ::mkdtemp(writable.data());
  return made == nullptr ? std::filesystem::path{} : std::filesystem::path(made);
}

std::filesystem::path FindOwnerToken(const std::filesystem::path& control_dir) {
  std::error_code ec;
  if (!std::filesystem::exists(control_dir, ec)) return {};
  for (const auto& entry : std::filesystem::directory_iterator(control_dir, ec)) {
    if (ec) return {};
    const auto path = entry.path();
    const auto name = path.filename().string();
    if (name.size() >= std::string(".owner").size() && name.ends_with(".owner")) {
      return path;
    }
  }
  return {};
}

std::string ReadFile(const std::filesystem::path& path) {
  std::ifstream in(path);
  std::ostringstream out;
  out << in.rdbuf();
  return out.str();
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
    std::cerr << "usage: sb_listener_owner_token_stale_corrupt_smoke <sb_listener> <sb_parser_dummy>\n";
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
  std::filesystem::create_directories(control_dir);
  scratchbird::listener::ListenerConfig identity_config;
  identity_config.protocol_family = "sbsql";
  identity_config.database_selector = "dev_bootstrap_path:/tmp/sb_lost.sbdb";
  identity_config.server_endpoint = "unix:/tmp/sb_lost.sbps.sock";
  identity_config.control_dir = control_dir.string();
  identity_config.runtime_dir = runtime_dir.string();
  const auto corrupt_owner = scratchbird::listener::BuildSocketIdentity(identity_config).owner_file;
  {
    std::ofstream out(corrupt_owner, std::ios::trunc);
    out << "this is not a valid live owner token\n";
  }

  const pid_t pid = ::fork();
  if (pid == 0) {
    int out = ::creat((work / "listener.out").c_str(), 0600);
    int err = ::creat((work / "listener.err").c_str(), 0600);
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
            "--database-selector=dev_bootstrap_path:/tmp/sb_lost.sbdb",
            "--server-endpoint=unix:/tmp/sb_lost.sbps.sock",
            parser_arg.c_str(),
            control_arg.c_str(),
            runtime_arg.c_str(),
            "--bind-address=127.0.0.1",
            port_arg.c_str(),
            "--warm-pool-min=1",
            "--warm-pool-max=1",
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

  int fd = -1;
  for (int i = 0; i < 100; ++i) {
    fd = ConnectLoopback(port);
    if (fd >= 0) break;
    int status = 0;
    if (::waitpid(pid, &status, WNOHANG) == pid) {
      std::cerr << "listener exited before accepting after stale/corrupt owner token\n";
      return EXIT_FAILURE;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  if (fd < 0) {
    cleanup();
    std::cerr << "listener did not accept after stale/corrupt owner token\n";
    return EXIT_FAILURE;
  }

  std::string line;
  if (!ReadLineWithTimeout(fd, &line, 2000) || line != "ScratchBird dummy parser ready") {
    ::close(fd);
    cleanup();
    std::cerr << "listener greeting failed after stale/corrupt owner token: " << line << '\n';
    return EXIT_FAILURE;
  }
  if (!WriteAll(fd, "stale-owner-token\n") || !ReadLineWithTimeout(fd, &line, 1000) ||
      line != "stale-owner-token") {
    ::close(fd);
    cleanup();
    std::cerr << "listener echo failed after stale/corrupt owner token: " << line << '\n';
    return EXIT_FAILURE;
  }
  ::close(fd);

  const auto owner = FindOwnerToken(control_dir);
  const auto owner_text = owner.empty() ? std::string{} : ReadFile(owner);
  cleanup();
  Require(!owner.empty(), "listener did not write an owner token");
  Require(owner_text.find("listener_uuid=") != std::string::npos &&
              owner_text.find("signature_sha256_128=") != std::string::npos,
          "listener did not replace stale/corrupt owner token with signed owner evidence");
  std::cout << "owner_token_stale_corrupt_smoke=passed work=" << work << '\n';
  return EXIT_SUCCESS;
}
