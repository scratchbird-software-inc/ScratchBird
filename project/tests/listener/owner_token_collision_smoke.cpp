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
#include <fstream>
#include <iostream>
#include <netinet/in.h>
#include <poll.h>
#include <sstream>
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
  std::string tmpl = "/tmp/sb_lown.XXXXXX";
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

std::string ReadFile(const std::filesystem::path& path) {
  std::ifstream in(path);
  std::ostringstream out;
  out << in.rdbuf();
  return out.str();
}

pid_t StartListener(const std::filesystem::path& listener,
                    const std::filesystem::path& parser,
                    const std::filesystem::path& control_dir,
                    const std::filesystem::path& runtime_dir,
                    const std::filesystem::path& stdout_path,
                    const std::filesystem::path& stderr_path,
                    int port) {
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
            "--database-selector=dev_bootstrap_path:/tmp/sb_lown.sbdb",
            "--server-endpoint=unix:/tmp/sb_lown.sbps.sock",
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
  return pid;
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
    std::cerr << "usage: sb_listener_owner_token_collision_smoke <sb_listener> <sb_parser_dummy>\n";
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
  const pid_t first_pid = StartListener(listener,
                                        parser,
                                        control_dir,
                                        runtime_dir,
                                        work / "first.out",
                                        work / "first.err",
                                        port);
  Require(first_pid > 0, "could not fork first listener");

  auto cleanup_first = [&] {
    ::kill(first_pid, SIGTERM);
    int status = 0;
    for (int i = 0; i < 100; ++i) {
      const auto rc = ::waitpid(first_pid, &status, WNOHANG);
      if (rc == first_pid) return;
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    ::kill(first_pid, SIGKILL);
    ::waitpid(first_pid, &status, 0);
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
    if (::waitpid(first_pid, &status, WNOHANG) == first_pid) {
      std::cerr << "first listener exited before management socket became reachable\n";
      return EXIT_FAILURE;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  Require(!management_socket.empty(), "first listener management socket was not created");

  const pid_t second_pid = StartListener(listener,
                                         parser,
                                         control_dir,
                                         runtime_dir,
                                         work / "second.out",
                                         work / "second.err",
                                         port);
  if (second_pid <= 0) {
    cleanup_first();
    std::cerr << "could not fork second listener\n";
    return EXIT_FAILURE;
  }
  int second_status = 0;
  bool second_exited = false;
  for (int i = 0; i < 100; ++i) {
    const auto rc = ::waitpid(second_pid, &second_status, WNOHANG);
    if (rc == second_pid) {
      second_exited = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  if (!second_exited) {
    ::kill(second_pid, SIGKILL);
    ::waitpid(second_pid, &second_status, 0);
    cleanup_first();
    std::cerr << "second listener did not refuse the live owner token\n";
    return EXIT_FAILURE;
  }
  if (WIFEXITED(second_status) && WEXITSTATUS(second_status) == 0) {
    cleanup_first();
    std::cerr << "second listener exited successfully despite live owner token\n";
    return EXIT_FAILURE;
  }
  const auto second_err = ReadFile(work / "second.err");
  if (second_err.find("LISTENER.RUNTIME.OWNER_TOKEN_FAILED") == std::string::npos ||
      second_err.find("live owner token exists") == std::string::npos) {
    cleanup_first();
    std::cerr << "second listener did not report live owner-token refusal: " << second_err << '\n';
    return EXIT_FAILURE;
  }

  int fd = ConnectLoopback(port);
  if (fd < 0) {
    cleanup_first();
    std::cerr << "first listener stopped accepting after second owner-token refusal\n";
    return EXIT_FAILURE;
  }
  std::string line;
  if (!ReadLineWithTimeout(fd, &line, 2000) || line != "ScratchBird dummy parser ready") {
    ::close(fd);
    cleanup_first();
    std::cerr << "first listener greeting failed after collision refusal: " << line << '\n';
    return EXIT_FAILURE;
  }
  if (!WriteAll(fd, "owner-token-collision\n") || !ReadLineWithTimeout(fd, &line, 1000) ||
      line != "owner-token-collision") {
    ::close(fd);
    cleanup_first();
    std::cerr << "first listener echo failed after collision refusal: " << line << '\n';
    return EXIT_FAILURE;
  }
  ::close(fd);
  cleanup_first();
  std::cout << "owner_token_collision_smoke=passed work=" << work << '\n';
  return EXIT_SUCCESS;
}
