// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <netinet/in.h>
#include <poll.h>
#include <string>
#include <string_view>
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
  std::string tmpl = "/tmp/sb_ssd.XXXXXX";
  std::vector<char> writable(tmpl.begin(), tmpl.end());
  writable.push_back('\0');
  char* made = ::mkdtemp(writable.data());
  return made == nullptr ? std::filesystem::path{} : std::filesystem::path(made);
}

bool WriteConfig(const std::filesystem::path& config_path,
                 const std::filesystem::path& work,
                 const std::filesystem::path& listener,
                 const std::filesystem::path& parser,
                 int port) {
  const auto data_dir = work / "d";
  const auto control_dir = work / "c";
  const auto listener_control_dir = work / "lc";
  const auto listener_runtime_dir = work / "lr";
  std::error_code ec;
  std::filesystem::create_directories(data_dir, ec);
  if (ec) return false;
  std::filesystem::create_directories(control_dir, ec);
  if (ec) return false;
  std::ofstream out(config_path, std::ios::trunc);
  if (!out) return false;
  out << "[config]\n";
  out << "format=SBCD1\n\n";
  out << "[server]\n";
  out << "mode=foreground\n\n";
  out << "[server.runtime]\n";
  out << "data_dir=" << data_dir.string() << "\n";
  out << "control_dir=" << control_dir.string() << "\n\n";
  out << "[server.database]\n";
  out << "default_path=" << (work / "t.sbdb").string() << "\n";
  out << "auto_create=true\n";
  out << "open_mode=normal\n\n";
  out << "[server.parser]\n";
  out << "sbps_enabled=true\n";
  out << "sbps_endpoint=" << (control_dir / "sb_server.sbps.sock").string() << "\n\n";
  out << "[server.listener.native]\n";
  out << "enabled=true\n";
  out << "bind_host=127.0.0.1\n";
  out << "port=" << port << "\n";
  out << "executable_path=" << listener.string() << "\n";
  out << "parser_executable_path=" << parser.string() << "\n";
  out << "control_dir=" << listener_control_dir.string() << "\n";
  out << "runtime_dir=" << listener_runtime_dir.string() << "\n";
  out << "tls_required=false\n";
  out << "ready_timeout_ms=8000\n";
  return static_cast<bool>(out);
}

std::filesystem::path FindOwnerFile(const std::filesystem::path& listener_control_dir) {
  std::error_code ec;
  if (!std::filesystem::exists(listener_control_dir, ec)) return {};
  for (const auto& entry : std::filesystem::directory_iterator(listener_control_dir, ec)) {
    if (ec) return {};
    const auto path = entry.path();
    if (path.filename().string().ends_with(".owner")) return path;
  }
  return {};
}

pid_t ReadOwnerPid(const std::filesystem::path& owner_file) {
  std::ifstream in(owner_file);
  std::string line;
  while (std::getline(in, line)) {
    constexpr std::string_view prefix = "pid=";
    if (line.rfind(prefix, 0) != 0) continue;
    char* end = nullptr;
    const auto text = line.substr(prefix.size());
    const long pid = std::strtol(text.c_str(), &end, 10);
    if (end != nullptr && *end == '\0' && pid > 0) return static_cast<pid_t>(pid);
  }
  return -1;
}

bool ConnectAndEcho(int port, const std::string& text, std::string* error) {
  int fd = -1;
  for (int i = 0; i < 100; ++i) {
    fd = ConnectLoopback(port);
    if (fd >= 0) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  if (fd < 0) {
    *error = "connect failed";
    return false;
  }
  std::string line;
  if (!ReadLineWithTimeout(fd, &line, 2500) || line != "ScratchBird dummy parser ready") {
    ::close(fd);
    *error = "unexpected greeting: " + line;
    return false;
  }
  if (!WriteAll(fd, text + "\n") || !ReadLineWithTimeout(fd, &line, 1000) || line != text) {
    ::close(fd);
    *error = "echo failed: " + line;
    return false;
  }
  ::close(fd);
  return true;
}

bool WaitPidGone(pid_t pid, int attempts, int sleep_ms) {
  for (int i = 0; i < attempts; ++i) {
    errno = 0;
    if (::kill(pid, 0) != 0 && errno == ESRCH) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
  }
  errno = 0;
  return ::kill(pid, 0) != 0 && errno == ESRCH;
}

bool WaitServerExit(pid_t pid, int* status) {
  for (int i = 0; i < 150; ++i) {
    const auto rc = ::waitpid(pid, status, WNOHANG);
    if (rc == pid) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  return false;
}

bool PortRefusesConnections(int port) {
  for (int i = 0; i < 20; ++i) {
    const int fd = ConnectLoopback(port);
    if (fd >= 0) {
      ::close(fd);
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  return true;
}

void Require(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(EXIT_FAILURE);
  }
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 4) {
    std::cerr << "usage: sb_server_shutdown_stops_listener_smoke <sb_server> <sb_listener> <sb_parser_dummy>\n";
    return EXIT_FAILURE;
  }
  const std::filesystem::path server = argv[1];
  const std::filesystem::path listener = argv[2];
  const std::filesystem::path parser = argv[3];
  const auto work = MakeTempDir();
  Require(!work.empty(), "could not create temp dir");
  const int port = FindFreePort();
  Require(port > 0, "could not allocate test port");

  const auto config_path = work / "sb_server.conf";
  Require(WriteConfig(config_path, work, listener, parser, port), "could not write server config");
  const auto listener_control_dir = work / "lc";
  const auto stdout_path = work / "server.out";
  const auto stderr_path = work / "server.err";

  const pid_t server_pid = ::fork();
  if (server_pid == 0) {
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
    ::execl(server.c_str(), server.c_str(), "--config", config_path.c_str(), "--foreground", nullptr);
    _exit(127);
  }
  Require(server_pid > 0, "could not fork sb_server");

  auto cleanup = [&] {
    int status = 0;
    if (::waitpid(server_pid, &status, WNOHANG) == server_pid) return;
    (void)::kill(server_pid, SIGTERM);
    if (WaitServerExit(server_pid, &status)) return;
    (void)::kill(server_pid, SIGKILL);
    (void)::waitpid(server_pid, &status, 0);
  };

  std::string error;
  if (!ConnectAndEcho(port, "before-server-shutdown", &error)) {
    cleanup();
    std::cerr << error << '\n';
    return EXIT_FAILURE;
  }

  std::filesystem::path owner_file;
  pid_t listener_pid = -1;
  for (int i = 0; i < 100; ++i) {
    owner_file = FindOwnerFile(listener_control_dir);
    if (!owner_file.empty()) {
      listener_pid = ReadOwnerPid(owner_file);
      if (listener_pid > 0) break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  if (listener_pid <= 0) {
    cleanup();
    std::cerr << "could not determine managed listener pid\n";
    return EXIT_FAILURE;
  }

  (void)::kill(server_pid, SIGTERM);
  int server_status = 0;
  if (!WaitServerExit(server_pid, &server_status)) {
    (void)::kill(server_pid, SIGKILL);
    (void)::waitpid(server_pid, &server_status, 0);
    std::cerr << "sb_server did not exit after SIGTERM\n";
    return EXIT_FAILURE;
  }
  if (!WIFEXITED(server_status) || WEXITSTATUS(server_status) != 0) {
    std::cerr << "sb_server did not exit cleanly after SIGTERM status=" << server_status << '\n';
    return EXIT_FAILURE;
  }
  if (!WaitPidGone(listener_pid, 100, 20)) {
    std::cerr << "managed listener process survived server shutdown pid=" << listener_pid << '\n';
    return EXIT_FAILURE;
  }
  if (!PortRefusesConnections(port)) {
    std::cerr << "managed listener TCP port still accepts connections after server shutdown\n";
    return EXIT_FAILURE;
  }

  std::cout << "server_shutdown_stops_listener_smoke=passed work=" << work << '\n';
  return EXIT_SUCCESS;
}
