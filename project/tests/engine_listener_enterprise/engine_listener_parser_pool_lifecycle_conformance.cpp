// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "control_plane.hpp"
#include "listener_config.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
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

int ConnectLoopback(int port) {
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

void RequireContains(const std::string& body, const std::string& needle, const std::string& message) {
  if (body.find(needle) == std::string::npos) {
    Fail(message + "\nneedle=" + needle + "\nbody=" + body);
  }
}

bool HasDiagnosticCode(const scratchbird::listener::proto::MessageVectorSet& messages,
                       const std::string& code) {
  for (const auto& diagnostic : messages.diagnostics) {
    if (diagnostic.code == code) return true;
  }
  return false;
}

std::string DiagnosticCodes(const scratchbird::listener::proto::MessageVectorSet& messages) {
  std::string out;
  for (const auto& diagnostic : messages.diagnostics) {
    if (!out.empty()) out += ",";
    out += diagnostic.code;
  }
  return out;
}

scratchbird::listener::ListenerConfig ValidListenerConfigBase(const std::filesystem::path& work) {
  scratchbird::listener::ListenerConfig base;
  base.protocol_family = "sbsql";
  base.database_selector = "dev_bootstrap_path:" + (work / "listener.sbdb").string();
  base.server_endpoint = "unix:" + (work / "server.sbps.sock").string();
  return base;
}

scratchbird::listener::ConfigResult LoadConfigSnippet(const std::filesystem::path& work,
                                                      const std::string& name,
                                                      const std::string& body) {
  const auto path = work / (name + ".conf");
  {
    std::ofstream out(path, std::ios::trunc);
    Require(static_cast<bool>(out), "could not create listener config snippet: " + path.string());
    out << body;
  }
  return scratchbird::listener::LoadListenerConfigFile(path.string(),
                                                       ValidListenerConfigBase(work));
}

void RequireConfigDiagnostic(const scratchbird::listener::ConfigResult& result,
                             const std::string& code,
                             const std::string& message) {
  Require(!result.ok, message + " must fail validation");
  Require(HasDiagnosticCode(result.messages, code),
          message + " must emit " + code + "; diagnostics=" + DiagnosticCodes(result.messages));
}

void RunConfigValidationProof() {
  const auto work = MakeTempDir("sb_eler062_config_validation");
  Require(!work.empty(), "could not create temp dir for parser pool config validation proof");
  struct Cleanup {
    std::filesystem::path path;
    ~Cleanup() {
      if (!path.empty()) {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
      }
    }
  } cleanup{work};

  auto valid = LoadConfigSnippet(work,
                                 "valid_child_process_policy",
                                 "child_restart_base_ms=125\n"
                                 "child_restart_max_ms=1000\n"
                                 "child_quarantine_failures=3\n"
                                 "child_quarantine_window_ms=5000\n");
  Require(valid.ok, "valid child process restart/quarantine policy must pass listener config validation");

  auto zero_restart = LoadConfigSnippet(work,
                                        "zero_child_restart_backoff",
                                        "child_restart_base_ms=0\n"
                                        "child_restart_max_ms=1000\n");
  RequireConfigDiagnostic(zero_restart,
                          "LISTENER.CONFIG.INVALID_CHILD_RESTART_BACKOFF",
                          "zero child restart backoff");

  auto inverted_restart = LoadConfigSnippet(work,
                                            "inverted_child_restart_backoff",
                                            "child_restart_base_ms=1000\n"
                                            "child_restart_max_ms=125\n");
  RequireConfigDiagnostic(inverted_restart,
                          "LISTENER.CONFIG.INVALID_CHILD_RESTART_BACKOFF",
                          "inverted child restart backoff");

  auto disabled_quarantine = LoadConfigSnippet(work,
                                               "disabled_child_quarantine",
                                               "child_quarantine_failures=0\n"
                                               "child_quarantine_window_ms=5000\n");
  RequireConfigDiagnostic(disabled_quarantine,
                          "LISTENER.CONFIG.INVALID_CHILD_QUARANTINE",
                          "disabled child quarantine");

  auto zero_quarantine_window = LoadConfigSnippet(work,
                                                  "zero_child_quarantine_window",
                                                  "child_quarantine_failures=3\n"
                                                  "child_quarantine_window_ms=0\n");
  RequireConfigDiagnostic(zero_quarantine_window,
                          "LISTENER.CONFIG.INVALID_CHILD_QUARANTINE",
                          "zero child quarantine window");
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

ListenerProcess StartListener(const std::filesystem::path& listener,
                              const std::filesystem::path& parser,
                              const std::string& behavior,
                              const std::string& prefix,
                              const std::vector<std::string>& extra_args) {
  ListenerProcess proc;
  proc.work = MakeTempDir(prefix);
  Require(!proc.work.empty(), "could not create temp dir for " + prefix);
  proc.control_dir = proc.work / "control";
  proc.runtime_dir = proc.work / "runtime";
  proc.stdout_path = proc.work / "listener.out";
  proc.stderr_path = proc.work / "listener.err";
  proc.port = FindFreePort();
  Require(proc.port > 0, "could not allocate listener port for " + prefix);

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
  args.push_back("--bind-address=127.0.0.1");
  args.push_back("--port=" + std::to_string(proc.port));
  for (const auto& arg : extra_args) args.push_back(arg);

  const pid_t child = ::fork();
  if (child == 0) {
    if (!behavior.empty()) {
      ::setenv("SB_PARSER_DUMMY_BEHAVIOR", behavior.c_str(), 1);
    }
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
  Require(child > 0, "could not fork listener for " + prefix);
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
      Fail("listener exited before management socket became reachable for " + prefix);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  Fail("management socket did not become reachable for " + prefix);
}

void RunOnDemandAdmissionProof(const std::filesystem::path& listener,
                               const std::filesystem::path& parser) {
  auto proc = StartListener(listener,
                            parser,
                            "normal",
                            "sb_eler062_on_demand",
                            {"--spawn-strategy=on_demand",
                             "--warm-pool-min=0",
                             "--warm-pool-max=1",
                             "--child-restart-base-ms=200",
                             "--child-restart-max-ms=200",
                             "--child-quarantine-failures=2",
                             "--child-quarantine-window-ms=2000"});
  auto status = SendManagementCommand(proc.management_socket, "STATUS", 1);
  Require(status.got_frame && status.ok, "on-demand STATUS must succeed");
  RequireContains(status.body, "\"spawn_strategy\":\"on_demand\"",
                  "STATUS must expose live on-demand spawn strategy");
  RequireContains(status.body, "\"running_workers\":0",
                  "on-demand listener must not pre-spawn a warm worker");
  RequireContains(status.body, "\"parser_pool_ready\":true",
                  "on-demand listener must be ready before the first handoff");

  int fd = -1;
  for (int i = 0; i < 40; ++i) {
    fd = ConnectLoopback(proc.port);
    if (fd >= 0) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  Require(fd >= 0, "on-demand listener must accept a client");
  std::string line;
  Require(ReadLineWithTimeout(fd, &line, 2500) && line == "ScratchBird dummy parser ready",
          "on-demand worker must be spawned and hand off the client");
  Require(WriteAll(fd, "on-demand-echo\n"), "on-demand client write must succeed");
  Require(ReadLineWithTimeout(fd, &line, 1000) && line == "on-demand-echo",
          "on-demand worker must echo through the handed-off socket");
  ::close(fd);

  status = SendManagementCommand(proc.management_socket, "STATUS", 2);
  Require(status.got_frame && status.ok, "on-demand STATUS after handoff must succeed");
  Require(ExtractUnsignedField(status.body, "handoff_complete_total") >= 1,
          "on-demand handoff completion must be visible in management status");
}

void RequireNoGreetingAfterFault(int port, const std::string& context) {
  int fd = -1;
  for (int i = 0; i < 40; ++i) {
    fd = ConnectLoopback(port);
    if (fd >= 0) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  Require(fd >= 0, "listener must accept client for " + context);
  std::string line;
  const bool got_line = ReadLineWithTimeout(fd, &line, 900);
  ::close(fd);
  Require(!got_line || line != "ScratchBird dummy parser ready",
          context + " must not reach a parser greeting");
}

void RunBackoffQuarantineProof(const std::filesystem::path& listener,
                               const std::filesystem::path& parser) {
  auto proc = StartListener(listener,
                            parser,
                            "crash_after_handoff",
                            "sb_eler062_faults",
                            {"--warm-pool-min=1",
                             "--warm-pool-max=1",
                             "--child-restart-base-ms=500",
                             "--child-restart-max-ms=500",
                             "--child-quarantine-failures=2",
                             "--child-quarantine-window-ms=3000",
                             "--handoff-ack-timeout-ms=300"});

  RequireNoGreetingAfterFault(proc.port, "first crash-after-handoff");
  auto status = SendManagementCommand(proc.management_socket, "STATUS", 10);
  Require(status.got_frame && status.ok, "STATUS after first parser fault must succeed");
  RequireContains(status.body, "\"parser_pool_retry_blocked\":true",
                  "first parser fault must activate restart backoff");
  RequireContains(status.body, "\"handoff_ack_failed\"",
                  "first parser fault must be recorded in fault history");
  Require(ExtractUnsignedField(status.body, "last_backoff_ms") == 500,
          "first parser fault must publish the configured restart backoff");
  Require(ExtractUnsignedField(status.body, "fault_history_count") >= 1,
          "first parser fault must publish durable fault history");

  std::this_thread::sleep_for(std::chrono::milliseconds(650));
  RequireNoGreetingAfterFault(proc.port, "second crash-after-handoff");
  status = SendManagementCommand(proc.management_socket, "STATUS", 11);
  Require(status.got_frame && status.ok, "STATUS after second parser fault must succeed");
  RequireContains(status.body, "\"quarantine_active\":true",
                  "second rapid parser fault must activate quarantine");
  RequireContains(status.body, "\"parser_pool_ready\":false",
                  "quarantined parser pool must fail readiness closed");
  Require(ExtractUnsignedField(status.body, "recent_failure_count") >= 2,
          "quarantine status must expose recent failure count");
  Require(ExtractUnsignedField(status.body, "fault_history_count") >= 2,
          "quarantine status must retain both fault records");

  RequireNoGreetingAfterFault(proc.port, "quarantined retry");
  status = SendManagementCommand(proc.management_socket, "STATUS", 12);
  Require(status.got_frame && status.ok, "STATUS after quarantined retry must succeed");
  RequireContains(status.body, "\"quarantine_active\":true",
                  "quarantined retry must not clear quarantine");
  RequireContains(status.body, "\"fault_history\":[",
                  "fault history must remain available after worker purge");
  Require(ExtractUnsignedField(status.body, "fault_history_count") >= 2,
          "pool-level fault history must survive purging failed worker records");
}

}  // namespace

int main(int argc, char** argv) {
  try {
    if (argc != 3) {
      std::cerr << "usage: engine_listener_parser_pool_lifecycle_conformance <sb_listener> <sb_parser_dummy>\n";
      return EXIT_FAILURE;
    }
    const std::filesystem::path listener = argv[1];
    const std::filesystem::path parser = argv[2];
    RunConfigValidationProof();
    RunOnDemandAdmissionProof(listener, parser);
    RunBackoffQuarantineProof(listener, parser);
    std::cout << "engine_listener_parser_pool_lifecycle_conformance=passed\n";
    return EXIT_SUCCESS;
  } catch (const std::exception& ex) {
    std::cerr << ex.what() << '\n';
    return EXIT_FAILURE;
  }
}
