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
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

int FindFreeDualStackPort() {
  const int fd = ::socket(AF_INET6, SOCK_STREAM, 0);
  if (fd < 0) return 0;
  int one = 1;
  int v6only = 0;
  (void)::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  if (::setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &v6only, sizeof(v6only)) != 0) {
    ::close(fd);
    return 0;
  }
  sockaddr_in6 addr{};
  addr.sin6_family = AF_INET6;
  addr.sin6_addr = in6addr_any;
  addr.sin6_port = 0;
  if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    ::close(fd);
    return 0;
  }
  socklen_t len = sizeof(addr);
  if (::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
    ::close(fd);
    return 0;
  }
  const int port = ntohs(addr.sin6_port);
  ::close(fd);
  return port;
}

int ConnectLoopback4(int port) {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
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

int ConnectLoopback6(int port) {
  const int fd = ::socket(AF_INET6, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  sockaddr_in6 addr{};
  addr.sin6_family = AF_INET6;
  addr.sin6_port = htons(static_cast<std::uint16_t>(port));
  if (::inet_pton(AF_INET6, "::1", &addr.sin6_addr) != 1) {
    ::close(fd);
    return -1;
  }
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

std::filesystem::path MakeTempDir(const std::string& prefix) {
  std::string tmpl = "/tmp/" + prefix + ".XXXXXX";
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

[[noreturn]] void Fail(const std::string& message) {
  throw std::runtime_error(message);
}

void Require(bool condition, const std::string& message) {
  if (!condition) Fail(message);
}

std::uint64_t ExtractUnsignedField(const std::string& json, const std::string& field) {
  const std::string key = "\"" + field + "\":";
  const auto pos = json.find(key);
  if (pos == std::string::npos) Fail("missing JSON field: " + field + "\nbody=" + json);
  std::size_t value_start = pos + key.size();
  while (value_start < json.size() && json[value_start] == ' ') ++value_start;
  std::size_t value_end = value_start;
  while (value_end < json.size() && json[value_end] >= '0' && json[value_end] <= '9') {
    ++value_end;
  }
  if (value_end == value_start) Fail("JSON field is not unsigned: " + field + "\nbody=" + json);
  return std::strtoull(json.substr(value_start, value_end - value_start).c_str(), nullptr, 10);
}

struct CommandResult {
  bool got_frame{false};
  bool ok{false};
  std::string body;
};

CommandResult SendManagementCommand(const std::filesystem::path& socket_path,
                                    const std::string& command,
                                    std::uint64_t sequence) {
  CommandResult result;
  const int fd = ConnectUnix(socket_path);
  if (fd < 0) return result;
  scratchbird::listener::ListenerControlFrame frame;
  frame.opcode = scratchbird::listener::ListenerControlOpcode::kManagementCommand;
  frame.sequence = sequence;
  frame.payload.assign(command.begin(), command.end());
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

struct ListenerProcess {
  std::filesystem::path work;
  std::filesystem::path control_dir;
  std::filesystem::path runtime_dir;
  std::filesystem::path stdout_path;
  std::filesystem::path stderr_path;
  std::filesystem::path management_socket;
  int port{0};
  pid_t pid{-1};

  ListenerProcess() = default;
  ListenerProcess(const ListenerProcess&) = delete;
  ListenerProcess& operator=(const ListenerProcess&) = delete;
  ListenerProcess(ListenerProcess&& other) noexcept {
    *this = std::move(other);
  }
  ListenerProcess& operator=(ListenerProcess&& other) noexcept {
    if (this == &other) return *this;
    Stop();
    work = std::move(other.work);
    control_dir = std::move(other.control_dir);
    runtime_dir = std::move(other.runtime_dir);
    stdout_path = std::move(other.stdout_path);
    stderr_path = std::move(other.stderr_path);
    management_socket = std::move(other.management_socket);
    port = other.port;
    pid = other.pid;
    other.pid = -1;
    return *this;
  }

  ~ListenerProcess() {
    Stop();
    if (!work.empty()) {
      std::error_code ec;
      std::filesystem::remove_all(work, ec);
    }
  }

  void Stop() {
    if (pid <= 0) return;
    ::kill(pid, SIGTERM);
    int status = 0;
    for (int i = 0; i < 100; ++i) {
      const auto rc = ::waitpid(pid, &status, WNOHANG);
      if (rc == pid) {
        pid = -1;
        return;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    ::kill(pid, SIGKILL);
    ::waitpid(pid, &status, 0);
    pid = -1;
  }
};

ListenerProcess StartDualStackListener(const std::filesystem::path& listener,
                                       const std::filesystem::path& parser) {
  ListenerProcess proc;
  proc.work = MakeTempDir("sb_eler065_dual_stack");
  Require(!proc.work.empty(), "could not create temp dir for dual-stack listener");
  proc.control_dir = proc.work / "control";
  proc.runtime_dir = proc.work / "runtime";
  proc.stdout_path = proc.work / "listener.out";
  proc.stderr_path = proc.work / "listener.err";
  proc.port = FindFreeDualStackPort();
  Require(proc.port > 0, "could not allocate an IPv6 dual-stack listener port");

  std::vector<std::string> args;
  args.push_back(listener.string());
  args.push_back("--foreground");
  args.push_back("--protocol-family=sbsql");
  args.push_back("--listener-profile=default");
  args.push_back("--bundle-contract-id=bundle.default@1");
  args.push_back("--database-selector=dev_bootstrap_path:" + (proc.work / "listener.sbdb").string());
  args.push_back("--server-endpoint=unix:" + (proc.work / "server.sbps.sock").string());
  args.push_back("--parser-executable=" + parser.string());
  args.push_back("--control-dir=" + proc.control_dir.string());
  args.push_back("--runtime-dir=" + proc.runtime_dir.string());
  args.push_back("--bind-address=::");
  args.push_back("--port=" + std::to_string(proc.port));
  args.push_back("--spawn-strategy=on_demand");
  args.push_back("--warm-pool-min=0");
  args.push_back("--warm-pool-max=2");

  const pid_t child = ::fork();
  if (child == 0) {
    int out = ::creat(proc.stdout_path.c_str(), 0600);
    int err = ::creat(proc.stderr_path.c_str(), 0600);
    if (out >= 0) {
      ::dup2(out, STDOUT_FILENO);
      ::close(out);
    }
    if (err >= 0) {
      ::dup2(err, STDERR_FILENO);
      ::close(err);
    }
    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (auto& arg : args) argv.push_back(arg.data());
    argv.push_back(nullptr);
    ::execv(listener.c_str(), argv.data());
    _exit(127);
  }
  Require(child > 0, "could not fork dual-stack listener");
  proc.pid = child;

  for (int i = 0; i < 100; ++i) {
    proc.management_socket = FindManagementSocket(proc.control_dir);
    if (!proc.management_socket.empty()) {
      const int probe_fd = ConnectUnix(proc.management_socket);
      if (probe_fd >= 0) {
        ::close(probe_fd);
        return proc;
      }
    }
    int status = 0;
    if (::waitpid(proc.pid, &status, WNOHANG) == proc.pid) {
      proc.pid = -1;
      Fail("listener exited before management socket became reachable");
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  Fail("management socket did not become reachable for dual-stack listener");
}

void ProveEcho(int fd, const std::string& payload, const std::string& family) {
  Require(fd >= 0, family + " connection must be accepted");
  std::string line;
  Require(ReadLineWithTimeout(fd, &line, 2500) && line == "ScratchBird dummy parser ready",
          family + " handoff must reach the parser");
  Require(WriteAll(fd, payload + "\n"), family + " client write must succeed");
  Require(ReadLineWithTimeout(fd, &line, 1000) && line == payload,
          family + " parser echo must return over handed-off socket");
  ::close(fd);
}

}  // namespace

int main(int argc, char** argv) {
  try {
    if (argc != 3) {
      std::cerr << "usage: engine_listener_ipv6_dual_stack_transport_conformance <sb_listener> <sb_parser_dummy>\n";
      return EXIT_FAILURE;
    }
    auto proc = StartDualStackListener(argv[1], argv[2]);

    int fd6 = -1;
    for (int i = 0; i < 40; ++i) {
      fd6 = ConnectLoopback6(proc.port);
      if (fd6 >= 0) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    ProveEcho(fd6, "ipv6-dual-stack-echo", "IPv6");

    int fd4 = -1;
    for (int i = 0; i < 40; ++i) {
      fd4 = ConnectLoopback4(proc.port);
      if (fd4 >= 0) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    ProveEcho(fd4, "ipv4-dual-stack-echo", "IPv4");

    const auto status = SendManagementCommand(proc.management_socket, "STATUS", 100);
    Require(status.got_frame && status.ok, "dual-stack STATUS must succeed");
    Require(ExtractUnsignedField(status.body, "handoff_complete_total") >= 2,
            "dual-stack IPv4 and IPv6 handoffs must be visible in management status");

    std::cout << "engine_listener_ipv6_dual_stack_transport_conformance=passed\n";
    return EXIT_SUCCESS;
  } catch (const std::exception& ex) {
    std::cerr << ex.what() << '\n';
    return EXIT_FAILURE;
  }
}
