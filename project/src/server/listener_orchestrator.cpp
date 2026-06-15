// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SEARCH_KEY: SB_SERVER_MANAGEMENT_LISTENER_COORDINATION

#include "listener_orchestrator.hpp"

#include "control_plane.hpp"
#include "manager_protocol.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <limits>
#include <sstream>
#include <string_view>
#include <thread>
#include <utility>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <windows.h>
#include <afunix.h>
#else
#include <cerrno>
#include <signal.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace scratchbird::server {

namespace {

namespace proto = scratchbird::manager::protocol;

#ifdef _WIN32
using ListenerManagementSocketHandle = std::intptr_t;
#else
using ListenerManagementSocketHandle = int;
#endif

std::uint64_t NextListenerManagementRequestId() {
  static std::atomic<std::uint64_t> next{1};
  const auto ordinal = next.fetch_add(1, std::memory_order_relaxed) % 1000000u;
  return proto::CurrentEpochMilliseconds() * 1000000u + ordinal;
}

std::string JsonEscape(const std::string& value) {
  return EscapeMessageVectorText(value);
}

std::string DefaultListenerUuid(std::uint64_t generation) {
  std::ostringstream out;
  out << "server-native-listener-" << generation;
  return out.str();
}

ServerListenerProfileRuntime* FindTarget(ServerListenerOrchestrator* orchestrator,
                                         const std::string& target_uuid) {
  if (orchestrator->profiles.empty()) return nullptr;
  if (target_uuid.empty()) return &orchestrator->profiles.front();
  const auto found = std::find_if(orchestrator->profiles.begin(),
                                  orchestrator->profiles.end(),
                                  [&](const ServerListenerProfileRuntime& profile) {
                                    return profile.listener_uuid == target_uuid ||
                                           profile.profile_name == target_uuid;
                                  });
  if (found == orchestrator->profiles.end()) return nullptr;
  return &*found;
}

bool ValidBindHost(const std::string& host) {
  return host == "127.0.0.1" || host == "localhost" || host == "::1";
}

bool RawListenerCommandAllowed(std::string_view command) {
  return command == "PING" || command == "STATUS" || command == "HEALTH" ||
         command == "POOL_STATUS" || command == "POOL STATUS";
}

#ifdef _WIN32
proto::Bytes ServerManagedListenerDbbtKey() {
  static constexpr std::string_view kServerManagedTestKey =
      "scratchbird-listener-test-dbbt-key-v1";
  return proto::Bytes(kServerManagedTestKey.begin(), kServerManagedTestKey.end());
}
#endif

std::string HexDigestPrefix(const proto::Sha256Digest& digest, std::size_t bytes) {
  static constexpr char hex[] = "0123456789abcdef";
  std::string out;
  out.reserve(bytes * 2);
  for (std::size_t i = 0; i < bytes && i < digest.size(); ++i) {
    const auto byte = digest[i];
    out.push_back(hex[(byte >> 4u) & 0x0fu]);
    out.push_back(hex[byte & 0x0fu]);
  }
  return out;
}

std::string StableHash(const std::string& value) {
  proto::Bytes bytes(value.begin(), value.end());
  return HexDigestPrefix(proto::Sha256(bytes), 16);
}

std::string Sanitize(std::string value) {
  for (char& ch : value) {
    const bool ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                    (ch >= '0' && ch <= '9') || ch == '_' || ch == '-';
    if (!ok) ch = '_';
  }
  return value;
}

std::string ListenerManagementSocketPath(const ServerListenerProfileRuntime& profile) {
  const auto endpoint_hash = StableHash(profile.engine_endpoint + "|" +
                                        profile.database_selector + "|" +
                                        std::string("native"));
  const auto stem = Sanitize(std::string("native_") + endpoint_hash);
  return (std::filesystem::path(profile.control_dir) / (stem + ".management.sock")).string();
}

std::string ListenerControlDir(const ServerBootstrapConfig& config) {
  return config.listener_native_control_dir.string();
}

std::string ListenerRuntimeDir(const ServerBootstrapConfig& config) {
  return config.listener_native_runtime_dir.string();
}

std::string DatabaseSelector(const ServerBootstrapConfig& config) {
  if (!config.database_default_path.empty()) {
    return std::string("server_database_path:") + config.database_default_path.string();
  }
  return "server_database_default";
}

std::string SiblingExecutable(const std::string& name) {
#ifndef _WIN32
  char buffer[4096];
  const auto count = ::readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
  if (count > 0) {
    buffer[count] = '\0';
    return (std::filesystem::path(buffer).parent_path() / name).string();
  }
#endif
  return name;
}

std::string ListenerExecutablePath(const ServerBootstrapConfig& config) {
  if (!config.listener_native_executable_path.empty()) {
    return config.listener_native_executable_path.string();
  }
  return SiblingExecutable("SBgate");
}

std::string ParserExecutablePath(const ServerBootstrapConfig& config) {
  if (!config.listener_native_parser_executable_path.empty()) {
    return config.listener_native_parser_executable_path.string();
  }
  return SiblingExecutable("sbp_native");
}

std::string ProfileUuid(std::uint64_t generation) {
  return "server-native-listener-profile-" + std::to_string(generation);
}

#ifdef _WIN32
std::string QuoteWindowsCommandLineArgument(const std::string& arg) {
  if (arg.empty()) return "\"\"";
  bool needs_quotes = false;
  for (char ch : arg) {
    if (ch == ' ' || ch == '\t' || ch == '"') {
      needs_quotes = true;
      break;
    }
  }
  if (!needs_quotes) return arg;

  std::string quoted;
  quoted.push_back('"');
  std::size_t backslashes = 0;
  for (char ch : arg) {
    if (ch == '\\') {
      ++backslashes;
      continue;
    }
    if (ch == '"') {
      quoted.append(backslashes * 2 + 1, '\\');
      quoted.push_back('"');
      backslashes = 0;
      continue;
    }
    quoted.append(backslashes, '\\');
    backslashes = 0;
    quoted.push_back(ch);
  }
  quoted.append(backslashes * 2, '\\');
  quoted.push_back('"');
  return quoted;
}

std::string BuildWindowsCommandLine(const std::vector<std::string>& args) {
  std::ostringstream out;
  for (std::size_t i = 0; i < args.size(); ++i) {
    if (i != 0) out << ' ';
    out << QuoteWindowsCommandLineArgument(args[i]);
  }
  return out.str();
}

HANDLE OpenListenerProcessHandle(std::int64_t pid, DWORD access) {
  if (pid <= 0 || pid > static_cast<std::int64_t>(std::numeric_limits<DWORD>::max())) {
    return nullptr;
  }
  return ::OpenProcess(access, FALSE, static_cast<DWORD>(pid));
}
#endif

#ifdef _WIN32
bool EnsureWinsockInitialized() {
  static const bool initialized = [] {
    WSADATA data{};
    return ::WSAStartup(MAKEWORD(2, 2), &data) == 0;
  }();
  return initialized;
}
#endif

void CloseListenerManagementSocket(ListenerManagementSocketHandle fd) {
  if (fd < 0) return;
#ifdef _WIN32
  ::closesocket(static_cast<SOCKET>(fd));
#else
  ::close(fd);
#endif
}

std::string NormalizeListenerManagementSocketPath(const std::string& socket_path) {
#ifdef _WIN32
  return std::filesystem::absolute(socket_path).string();
#else
  return socket_path;
#endif
}

bool ConnectListenerManagementSocket(const std::string& socket_path,
                                     ListenerManagementSocketHandle* out_fd,
                                     ServerListenerOperationResult* result,
                                     ServerListenerProfileRuntime* profile) {
  *out_fd = -1;
#ifdef _WIN32
  if (!EnsureWinsockInitialized()) {
    result->diagnostics.push_back(ListenerDiagnostic(
        "LISTENER.MANAGEMENT_SOCKET_STACK_UNAVAILABLE",
        "Winsock initialization failed for listener management client.",
        {{"listener_uuid", profile->listener_uuid}}));
    result->state_after = profile->state;
    return false;
  }
  SOCKET fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd == INVALID_SOCKET) {
#else
  int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) {
#endif
    result->diagnostics.push_back(ListenerDiagnostic(
        "LISTENER.MANAGEMENT_CONNECT_FAILED",
        "The server could not create a listener management socket client.",
        {{"listener_uuid", profile->listener_uuid}}));
    result->state_after = profile->state;
    return false;
  }

  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  const auto path = NormalizeListenerManagementSocketPath(socket_path);
  if (path.size() >= sizeof(addr.sun_path)) {
    CloseListenerManagementSocket(static_cast<ListenerManagementSocketHandle>(fd));
    result->diagnostics.push_back(ListenerDiagnostic(
        "LISTENER.MANAGEMENT_SOCKET_INVALID",
        "The listener management socket path is too long.",
        {{"listener_uuid", profile->listener_uuid},
         {"management_socket", path}}));
    result->state_after = profile->state;
    return false;
  }
  std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
  if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    CloseListenerManagementSocket(static_cast<ListenerManagementSocketHandle>(fd));
    result->diagnostics.push_back(ListenerDiagnostic(
        "LISTENER.MANAGEMENT_CONNECT_FAILED",
        "The server could not connect to the listener management socket.",
        {{"listener_uuid", profile->listener_uuid},
         {"management_socket", path}}));
    result->state_after = profile->state;
    return false;
  }
  *out_fd = static_cast<ListenerManagementSocketHandle>(fd);
  return true;
}

bool ProcessAlive(std::int64_t pid) {
#ifdef _WIN32
  HANDLE handle = OpenListenerProcessHandle(pid, SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION);
  if (handle == nullptr) return false;
  DWORD exit_code = 0;
  const bool active = ::GetExitCodeProcess(handle, &exit_code) != 0 && exit_code == STILL_ACTIVE;
  ::CloseHandle(handle);
  return active;
#else
  const auto native_pid = static_cast<pid_t>(pid);
  return native_pid > 0 && (::kill(native_pid, 0) == 0 || errno == EPERM);
#endif
}

bool ReapIfExited(std::int64_t pid) {
#ifdef _WIN32
  HANDLE handle = OpenListenerProcessHandle(pid, SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION);
  if (handle == nullptr) return true;
  const DWORD wait = ::WaitForSingleObject(handle, 0);
  ::CloseHandle(handle);
  return wait == WAIT_OBJECT_0;
#else
  const auto native_pid = static_cast<pid_t>(pid);
  if (pid <= 0) return true;
  int status = 0;
  const auto rc = ::waitpid(native_pid, &status, WNOHANG);
  return rc == native_pid;
#endif
}

bool TerminateListenerProcess(std::int64_t pid) {
#ifdef _WIN32
  HANDLE handle = OpenListenerProcessHandle(pid, SYNCHRONIZE | PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION);
  if (handle == nullptr) return true;
  const BOOL terminated = ::TerminateProcess(handle, 1);
  if (terminated) {
    (void)::WaitForSingleObject(handle, 1000);
  }
  ::CloseHandle(handle);
  return terminated != 0;
#else
  const auto native_pid = static_cast<pid_t>(pid);
  if (native_pid <= 0) return true;
  if (::kill(native_pid, SIGKILL) != 0 && errno != ESRCH) return false;
  int status = 0;
  (void)::waitpid(native_pid, &status, 0);
  return true;
#endif
}

ServerListenerOperationResult ListenerProcessDiagnostic(std::string code,
                                                        std::string message,
                                                        const ServerListenerProfileRuntime& profile) {
  ServerListenerOperationResult result;
  result.target_uuid = profile.listener_uuid;
  result.state_before = profile.state;
  result.state_after = profile.state;
  result.diagnostics.push_back(ListenerDiagnostic(std::move(code),
                                                  std::move(message),
                                                  {{"listener_uuid", profile.listener_uuid},
                                                   {"management_socket", profile.management_socket_path}}));
  return result;
}

ServerListenerOperationResult SendManagementCommand(ServerListenerProfileRuntime* profile,
                                                    const std::string& command,
                                                    std::uint32_t timeout_ms) {
  ServerListenerOperationResult result;
  result.target_uuid = profile->listener_uuid;
  result.state_before = profile->state;
  result.generation = 0;

  ListenerManagementSocketHandle fd = -1;
  if (!ConnectListenerManagementSocket(profile->management_socket_path, &fd, &result, profile)) {
    result.state_after = profile->state;
    return result;
  }

  scratchbird::listener::ListenerControlFrame frame;
  frame.opcode = scratchbird::listener::ListenerControlOpcode::kManagementCommand;
  frame.sequence = NextListenerManagementRequestId();
  if (RawListenerCommandAllowed(command)) {
    frame.payload.assign(command.begin(), command.end());
  } else {
    const auto now_ms = proto::CurrentEpochMilliseconds();
    auto envelope = scratchbird::listener::BuildListenerManagementEnvelopeFromCommand(
        command,
        frame.sequence,
        now_ms,
        now_ms + 30000,
        "manager",
        "listener_manager",
#ifdef _WIN32
        scratchbird::listener::kListenerManagementAuthHmacSha256
#else
        scratchbird::listener::kListenerManagementAuthPeerOwner
#endif
        );
    if (!envelope) {
      CloseListenerManagementSocket(fd);
      result.diagnostics.push_back(ListenerDiagnostic(
          "LISTENER.MANAGEMENT_ENVELOPE_ENCODE_FAILED",
          "The server could not encode a listener management command envelope.",
          {{"listener_uuid", profile->listener_uuid}, {"command", command}}));
      result.state_after = profile->state;
      return result;
    }
#ifdef _WIN32
    scratchbird::listener::SignListenerManagementEnvelopeHmacSha256(
        &*envelope,
        ServerManagedListenerDbbtKey());
#endif
    frame.payload = scratchbird::listener::EncodeListenerManagementEnvelope(*envelope);
  }
  if (!scratchbird::listener::SendControlFrame(fd, frame)) {
    CloseListenerManagementSocket(fd);
    result.diagnostics.push_back(ListenerDiagnostic(
        "LISTENER.MANAGEMENT_SEND_FAILED",
        "The server could not send a listener management command.",
        {{"listener_uuid", profile->listener_uuid}, {"command", command}}));
    result.state_after = profile->state;
    return result;
  }
  scratchbird::listener::ListenerControlDecodeResult decoded;
  ListenerManagementSocketHandle received_fd = -1;
  if (!scratchbird::listener::ReadControlFrame(fd, &decoded, &received_fd, timeout_ms)) {
    CloseListenerManagementSocket(received_fd);
    CloseListenerManagementSocket(fd);
    result.diagnostics.push_back(ListenerDiagnostic(
        "LISTENER.MANAGEMENT_TIMEOUT",
        "The listener did not answer a management command before the configured timeout.",
        {{"listener_uuid", profile->listener_uuid}, {"command", command}}));
    result.state_after = profile->state;
    return result;
  }
  CloseListenerManagementSocket(received_fd);
  CloseListenerManagementSocket(fd);
  if (decoded.frame.opcode != scratchbird::listener::ListenerControlOpcode::kManagementResponse) {
    result.diagnostics.push_back(ListenerDiagnostic(
        "LISTENER.MANAGEMENT_RESPONSE_INVALID",
        "The listener returned an invalid management response frame.",
        {{"listener_uuid", profile->listener_uuid}, {"command", command}}));
    result.state_after = profile->state;
    return result;
  }
  if (decoded.frame.payload.empty()) {
    result.diagnostics.push_back(ListenerDiagnostic(
        "LISTENER.MANAGEMENT_RESPONSE_INVALID",
        "The listener returned an empty management response frame.",
        {{"listener_uuid", profile->listener_uuid}, {"command", command}}));
    result.state_after = profile->state;
    return result;
  }
  const auto status = decoded.frame.payload[0];
  profile->last_management_response.assign(decoded.frame.payload.begin() + 1, decoded.frame.payload.end());
  if (status != 0) {
    result.diagnostics.push_back(ListenerDiagnostic(
        "LISTENER.MANAGEMENT_COMMAND_REFUSED",
        "The listener refused a management command.",
        {{"listener_uuid", profile->listener_uuid},
         {"command", command},
         {"listener_response", profile->last_management_response}}));
    result.state_after = profile->state;
    return result;
  }
  result.ok = true;
  result.outcome = "completed";
  result.state_after = profile->state;
  return result;
}

ServerListenerOperationResult LaunchListener(ServerListenerProfileRuntime* profile,
                                             const ServerBootstrapConfig& config,
                                             const ServerLifecycleArtifacts& artifacts) {
  ServerListenerOperationResult result;
  result.target_uuid = profile->listener_uuid;
  result.state_before = profile->state;
  result.generation = artifacts.generation;
  if (!profile->enabled) {
    result.ok = true;
    result.outcome = "disabled";
    result.state_after = profile->state;
    return result;
  }
  if (profile->state == "failed") {
    result.diagnostics.push_back(ListenerDiagnostic(
        profile->diagnostic_code.empty() ? "LISTENER.START_FAILED" : profile->diagnostic_code,
        "The listener profile is failed and cannot be launched.",
        {{"listener_uuid", profile->listener_uuid}}));
    result.state_after = profile->state;
    return result;
  }
  if (profile->pid > 0 && ProcessAlive(profile->pid)) {
    profile->state = "running";
    result.ok = true;
    result.outcome = "already_running";
    result.state_after = profile->state;
    return result;
  }

  std::error_code ec;
  std::filesystem::create_directories(profile->control_dir, ec);
  if (ec) {
    profile->state = "failed";
    profile->diagnostic_code = "LISTENER.CONTROL_DIR_INVALID";
    result.diagnostics.push_back(ListenerDiagnostic(
        profile->diagnostic_code,
        "The server could not create the listener control directory.",
        {{"listener_uuid", profile->listener_uuid}, {"control_dir", profile->control_dir}, {"os_error", ec.message()}}));
    result.state_after = profile->state;
    return result;
  }
  std::filesystem::create_directories(profile->runtime_dir, ec);
  if (ec) {
    profile->state = "failed";
    profile->diagnostic_code = "LISTENER.RUNTIME_DIR_INVALID";
    result.diagnostics.push_back(ListenerDiagnostic(
        profile->diagnostic_code,
        "The server could not create the listener runtime directory.",
        {{"listener_uuid", profile->listener_uuid}, {"runtime_dir", profile->runtime_dir}, {"os_error", ec.message()}}));
    result.state_after = profile->state;
    return result;
  }

  const auto port_arg = "--port=" + std::to_string(profile->port);
  const auto listener_uuid_arg = "--listener-uuid=" + profile->listener_uuid;
  const auto listener_profile_uuid_arg = "--listener-profile-uuid=" + ProfileUuid(artifacts.generation);
  const auto lifecycle_arg = "--lifecycle-generation=" + std::to_string(artifacts.generation);
  const std::string controller_arg = "--controller-uuid=sb_server";
  const auto server_arg = "--server-endpoint=" + profile->engine_endpoint;
  const auto database_arg = "--database-selector=" + DatabaseSelector(config);
  const auto parser_arg = "--parser-executable=" + profile->parser_executable_path;
  const auto control_arg = "--control-dir=" + profile->control_dir;
  const auto runtime_arg = "--runtime-dir=" + profile->runtime_dir;
  const auto bind_arg = "--bind-address=" + profile->bind_host;
  const std::string ready_min_arg = "--warm-pool-min=2";
  const std::string ready_max_arg = "--warm-pool-max=8";
  std::vector<std::string> args{
      profile->listener_executable_path,
      "--managed",
      "--protocol-family=native",
      "--listener-profile=native",
      listener_uuid_arg,
      listener_profile_uuid_arg,
      lifecycle_arg,
      "--controller-type=server",
      controller_arg,
      "--parser-package=sbp_native",
      "--parser-package-uuid=builtin-test-package",
      "--dialect-profile-uuid=sbwp-v1",
      "--bundle-contract-id=bundle.default@1",
      server_arg,
      database_arg,
      parser_arg,
      control_arg,
      runtime_arg,
      bind_arg,
      port_arg,
      ready_min_arg,
      ready_max_arg,
      "--dbbt-key-source=test_builtin",
      "--allow-test-dbbt-builtin=true",
  };
  if (profile->tls_required) {
    args.push_back("--tls-required=true");
    args.push_back("--tls-cert-file=" + profile->tls_cert_file);
    args.push_back("--tls-key-file=" + profile->tls_key_file);
    if (!profile->tls_ca_file.empty()) {
      args.push_back("--tls-ca-file=" + profile->tls_ca_file);
    }
  } else {
    args.push_back("--tls-required=false");
  }

  profile->state = "starting";
  profile->last_transition = "launch";

#ifdef _WIN32
  const auto command_line = BuildWindowsCommandLine(args);
  std::vector<char> mutable_command_line(command_line.begin(), command_line.end());
  mutable_command_line.push_back('\0');
  STARTUPINFOA startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process{};
  if (::CreateProcessA(nullptr,
                       mutable_command_line.data(),
                       nullptr,
                       nullptr,
                       FALSE,
                       0,
                       nullptr,
                       nullptr,
                       &startup,
                       &process) == 0) {
    profile->state = "failed";
    profile->diagnostic_code = "LISTENER.START_FAILED";
    result.diagnostics.push_back(ListenerDiagnostic(
        profile->diagnostic_code,
        "The server could not create the listener process.",
        {{"listener_uuid", profile->listener_uuid},
         {"windows_error", std::to_string(::GetLastError())}}));
    result.state_after = profile->state;
    return result;
  }
  ::CloseHandle(process.hThread);
  profile->pid = static_cast<std::int64_t>(::GetProcessId(process.hProcess));

  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(profile->ready_timeout_ms);
  while (std::chrono::steady_clock::now() < deadline) {
    const DWORD wait = ::WaitForSingleObject(process.hProcess, 0);
    if (wait == WAIT_OBJECT_0) {
      profile->state = "failed";
      profile->diagnostic_code = "LISTENER.START_FAILED";
      result.diagnostics.push_back(ListenerDiagnostic(
          profile->diagnostic_code,
          "The listener process exited before publishing management readiness.",
          {{"listener_uuid", profile->listener_uuid}, {"pid", std::to_string(profile->pid)}}));
      result.state_after = profile->state;
      ::CloseHandle(process.hProcess);
      return result;
    }
    auto status = SendManagementCommand(profile, "STATUS", 250);
    if (status.ok) {
      profile->state = "running";
      profile->last_transition = "ready";
      result.ok = true;
      result.outcome = "completed";
      result.state_after = profile->state;
      ::CloseHandle(process.hProcess);
      return result;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  (void)::TerminateProcess(process.hProcess, 1);
  (void)::WaitForSingleObject(process.hProcess, 1000);
  ::CloseHandle(process.hProcess);
  profile->state = "failed";
  profile->diagnostic_code = "LISTENER.START_TIMEOUT";
  result.diagnostics.push_back(ListenerDiagnostic(
      profile->diagnostic_code,
      "The listener did not become ready before the configured timeout.",
      {{"listener_uuid", profile->listener_uuid},
       {"ready_timeout_ms", std::to_string(profile->ready_timeout_ms)}}));
  result.state_after = profile->state;
  return result;
#else
  std::vector<char*> argv;
  argv.reserve(args.size() + 1);
  for (auto& arg : args) {
    argv.push_back(arg.data());
  }
  argv.push_back(nullptr);

  const pid_t pid = ::fork();
  if (pid == 0) {
    if (profile->listener_executable_path.find('/') == std::string::npos) {
      ::execvp(profile->listener_executable_path.c_str(), argv.data());
    } else {
      ::execv(profile->listener_executable_path.c_str(), argv.data());
    }
    _exit(127);
  }
  if (pid <= 0) {
    profile->state = "failed";
    profile->diagnostic_code = "LISTENER.START_FAILED";
    result.diagnostics.push_back(ListenerDiagnostic(
        profile->diagnostic_code,
        "The server could not fork the listener process.",
        {{"listener_uuid", profile->listener_uuid}}));
    result.state_after = profile->state;
    return result;
  }
  profile->pid = static_cast<std::int64_t>(pid);

  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(profile->ready_timeout_ms);
  while (std::chrono::steady_clock::now() < deadline) {
    if (ReapIfExited(static_cast<std::int64_t>(pid))) {
      profile->state = "failed";
      profile->diagnostic_code = "LISTENER.START_FAILED";
      result.diagnostics.push_back(ListenerDiagnostic(
          profile->diagnostic_code,
          "The listener process exited before publishing management readiness.",
          {{"listener_uuid", profile->listener_uuid}, {"pid", std::to_string(pid)}}));
      result.state_after = profile->state;
      return result;
    }
    auto status = SendManagementCommand(profile, "STATUS", 250);
    if (status.ok) {
      profile->state = "running";
      profile->last_transition = "ready";
      result.ok = true;
      result.outcome = "completed";
      result.state_after = profile->state;
      return result;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  ::kill(pid, SIGTERM);
  for (int i = 0; i < 50; ++i) {
    if (ReapIfExited(static_cast<std::int64_t>(pid))) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  if (ProcessAlive(static_cast<std::int64_t>(pid))) {
    (void)TerminateListenerProcess(static_cast<std::int64_t>(pid));
  }
  profile->state = "failed";
  profile->diagnostic_code = "LISTENER.START_TIMEOUT";
  result.diagnostics.push_back(ListenerDiagnostic(
      profile->diagnostic_code,
      "The listener did not become ready before the configured timeout.",
      {{"listener_uuid", profile->listener_uuid},
       {"ready_timeout_ms", std::to_string(profile->ready_timeout_ms)}}));
  result.state_after = profile->state;
  return result;
#endif
}

ServerListenerOperationResult StopListenerProcess(ServerListenerProfileRuntime* profile,
                                                  const std::string& mode) {
  ServerListenerOperationResult result;
  result.target_uuid = profile->listener_uuid;
  result.state_before = profile->state;
  result.state_after = profile->state;
  const auto command = mode == "force" ? "STOP FORCE" : "STOP GRACEFUL";
  auto stop = SendManagementCommand(profile, command, 1000);
  if (profile->pid > 0) {
    const std::int64_t pid = profile->pid;
    if (!stop.ok && mode != "force") {
      profile->state = "draining";
      profile->diagnostic_code = "LISTENER.GRACEFUL_STOP_REFUSED";
      profile->last_transition = command;
      result.ok = false;
      result.outcome = "drain_timeout_or_refused";
      result.state_after = profile->state;
      result.diagnostics = std::move(stop.diagnostics);
      if (result.diagnostics.empty()) {
        result.diagnostics.push_back(ListenerDiagnostic(
            "LISTENER.GRACEFUL_STOP_REFUSED",
            "The listener refused graceful stop and force escalation was not requested.",
            {{"listener_uuid", profile->listener_uuid},
             {"listener_response", profile->last_management_response}}));
      }
      return result;
    }
    for (int i = 0; i < 100; ++i) {
      if (ReapIfExited(pid)) {
        profile->pid = -1;
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    if (profile->pid > 0 && ProcessAlive(pid) && mode != "force") {
      profile->state = "draining";
      profile->diagnostic_code = "LISTENER.GRACEFUL_STOP_EXIT_TIMEOUT";
      profile->last_transition = command;
      result.ok = false;
      result.outcome = "drain_exit_timeout";
      result.state_after = profile->state;
      result.diagnostics = std::move(stop.diagnostics);
      result.diagnostics.push_back(ListenerDiagnostic(
          "LISTENER.GRACEFUL_STOP_EXIT_TIMEOUT",
          "The listener did not exit after graceful stop and force escalation was not requested.",
          {{"listener_uuid", profile->listener_uuid},
           {"pid", std::to_string(profile->pid)},
           {"listener_response", profile->last_management_response}}));
      return result;
    }
    if (profile->pid > 0 && ProcessAlive(pid)) {
      if (TerminateListenerProcess(pid)) {
        profile->pid = -1;
      }
    }
  }
  profile->enabled = false;
  profile->state = "stopped";
  profile->last_transition = command;
  result.ok = stop.ok || mode == "force" || profile->pid < 0;
  result.outcome = result.ok ? "completed" : "completed_with_management_warning";
  result.state_after = profile->state;
  result.diagnostics = std::move(stop.diagnostics);
  return result;
}

}  // namespace

std::string ListenerStateName(ServerListenerState state) {
  switch (state) {
    case ServerListenerState::kDisabled: return "disabled";
    case ServerListenerState::kStopped: return "stopped";
    case ServerListenerState::kStarting: return "starting";
    case ServerListenerState::kRunning: return "running";
    case ServerListenerState::kDraining: return "draining";
    case ServerListenerState::kFailed: return "failed";
  }
  return "failed";
}

ServerDiagnostic ListenerDiagnostic(std::string code,
                                    std::string safe_message,
                                    std::vector<ServerDiagnosticField> fields) {
  return ServerDiagnostic{std::move(code),
                          std::move(code),
                          ServerDiagnosticSeverity::kError,
                          std::move(safe_message),
                          std::move(fields)};
}

ServerListenerOrchestrator BuildListenerOrchestrator(const ServerBootstrapConfig& config,
                                                     const ServerLifecycleArtifacts& artifacts) {
  ServerListenerOrchestrator orchestrator;
  orchestrator.generation = artifacts.generation == 0 ? 1 : artifacts.generation;
  orchestrator.engine_endpoint = config.sbps_endpoint.string();

  ServerListenerProfileRuntime profile;
  profile.listener_uuid = DefaultListenerUuid(orchestrator.generation);
  profile.profile_name = "native";
  profile.bind_host = config.listener_native_bind_host;
  profile.port = config.listener_native_port;
  profile.enabled = config.listener_native_enabled;
  profile.listener_executable_path = ListenerExecutablePath(config);
  profile.parser_executable_path = ParserExecutablePath(config);
  profile.control_dir = ListenerControlDir(config);
  profile.runtime_dir = ListenerRuntimeDir(config);
  profile.tls_required = config.listener_native_tls_required;
  profile.tls_cert_file = config.listener_native_tls_cert_file.string();
  profile.tls_key_file = config.listener_native_tls_key_file.string();
  profile.tls_ca_file = config.listener_native_tls_ca_file.string();
  profile.ready_timeout_ms = config.listener_native_ready_timeout_ms;
  profile.engine_endpoint = orchestrator.engine_endpoint;
  profile.database_selector = DatabaseSelector(config);
  profile.management_socket_path = ListenerManagementSocketPath(profile);
  profile.state = config.listener_native_enabled ? "stopped" : "disabled";

  if (!ValidBindHost(profile.bind_host)) {
    profile.state = "failed";
    profile.diagnostic_code = "LISTENER.BIND_POLICY_INVALID";
    orchestrator.diagnostics.push_back(ListenerDiagnostic(
        "LISTENER.BIND_POLICY_INVALID",
        "The listener bind host is not permitted by standalone server policy.",
        {{"listener_uuid", profile.listener_uuid}, {"bind_host", profile.bind_host}}));
  }
  if (profile.port == 0 || profile.port > 65535) {
    profile.state = "failed";
    profile.diagnostic_code = "LISTENER.BIND_POLICY_INVALID";
    orchestrator.diagnostics.push_back(ListenerDiagnostic(
        "LISTENER.BIND_POLICY_INVALID",
        "The listener bind port is outside the permitted TCP port range.",
        {{"listener_uuid", profile.listener_uuid}, {"port", std::to_string(profile.port)}}));
  }
  if (profile.engine_endpoint.empty()) {
    profile.state = "failed";
    profile.diagnostic_code = "LISTENER.ENGINE_ENDPOINT_INVALID";
    orchestrator.diagnostics.push_back(ListenerDiagnostic(
        "LISTENER.ENGINE_ENDPOINT_INVALID",
        "The listener profile has no parser-server engine endpoint to publish.",
        {{"listener_uuid", profile.listener_uuid}}));
  }
  if (profile.enabled && profile.tls_required &&
      (profile.tls_cert_file.empty() || profile.tls_key_file.empty())) {
    profile.state = "failed";
    profile.diagnostic_code = "LISTENER.TLS_POLICY_FAILED";
    orchestrator.diagnostics.push_back(ListenerDiagnostic(
        "LISTENER.TLS_POLICY_FAILED",
        "The native SBWP listener requires tls_cert_file and tls_key_file when TLS is required.",
        {{"listener_uuid", profile.listener_uuid}}));
  }
  orchestrator.profiles.push_back(std::move(profile));
  return orchestrator;
}

std::string ListenerOrchestratorStatusJson(const ServerListenerOrchestrator& orchestrator) {
  std::ostringstream out;
  out << "{\"listener_orchestrator\":{\"generation\":" << orchestrator.generation
      << ",\"engine_endpoint\":\"" << JsonEscape(orchestrator.engine_endpoint)
      << "\",\"diagnostic_count\":" << orchestrator.diagnostics.size()
      << ",\"listeners\":[";
  for (std::size_t i = 0; i < orchestrator.profiles.size(); ++i) {
    if (i != 0) out << ',';
    const auto& profile = orchestrator.profiles[i];
    out << "{\"listener_uuid\":\"" << JsonEscape(profile.listener_uuid)
        << "\",\"profile_name\":\"" << JsonEscape(profile.profile_name)
        << "\",\"state\":\"" << JsonEscape(profile.state)
        << "\",\"bind_host\":\"" << JsonEscape(profile.bind_host)
        << "\",\"port\":" << profile.port
        << ",\"enabled\":" << (profile.enabled ? "true" : "false")
        << ",\"parser_package_ref\":\"" << JsonEscape(profile.parser_package_ref)
        << "\",\"engine_endpoint\":\"" << JsonEscape(profile.engine_endpoint)
        << "\",\"listener_executable_path\":\"" << JsonEscape(profile.listener_executable_path)
        << "\",\"parser_executable_path\":\"" << JsonEscape(profile.parser_executable_path)
        << "\",\"tls_required\":" << (profile.tls_required ? "true" : "false")
        << ",\"management_socket_path\":\"" << JsonEscape(profile.management_socket_path)
        << "\",\"pid\":" << profile.pid
        << "\",\"last_transition\":\"" << JsonEscape(profile.last_transition)
        << "\",\"diagnostic_code\":\"" << JsonEscape(profile.diagnostic_code) << "\"}";
  }
  out << "]}}\n";
  return out.str();
}

ServerListenerOperationResult StartEnabledServerListeners(ServerListenerOrchestrator* orchestrator,
                                                          const ServerBootstrapConfig& config,
                                                          const ServerLifecycleArtifacts& artifacts) {
  ServerListenerOperationResult aggregate;
  aggregate.ok = true;
  aggregate.outcome = "completed";
  aggregate.generation = orchestrator == nullptr ? 0 : orchestrator->generation;
  if (orchestrator == nullptr) {
    aggregate.ok = false;
    aggregate.outcome = "refused";
    aggregate.diagnostics.push_back(ListenerDiagnostic(
        "LISTENER.ORCHESTRATOR_MISSING",
        "The server listener orchestrator is not available."));
    return aggregate;
  }
  for (auto& profile : orchestrator->profiles) {
    if (!profile.enabled) continue;
    auto launched = LaunchListener(&profile, config, artifacts);
    if (!launched.ok) {
      aggregate.ok = false;
      aggregate.outcome = "refused";
      aggregate.diagnostics.insert(aggregate.diagnostics.end(),
                                   launched.diagnostics.begin(),
                                   launched.diagnostics.end());
    }
  }
  return aggregate;
}

ServerListenerOperationResult StopManagedServerListeners(ServerListenerOrchestrator* orchestrator,
                                                         const std::string& mode) {
  ServerListenerOperationResult aggregate;
  aggregate.ok = true;
  aggregate.outcome = "completed";
  aggregate.generation = orchestrator == nullptr ? 0 : orchestrator->generation;
  if (orchestrator == nullptr) return aggregate;
  for (auto& profile : orchestrator->profiles) {
    if (profile.pid <= 0 && profile.state != "running" && profile.state != "draining") continue;
    auto stopped = StopListenerProcess(&profile, mode);
    if (!stopped.ok) {
      aggregate.ok = false;
      aggregate.outcome = "completed_with_management_warning";
      aggregate.diagnostics.insert(aggregate.diagnostics.end(),
                                   stopped.diagnostics.begin(),
                                   stopped.diagnostics.end());
    }
  }
  return aggregate;
}

ServerListenerOperationResult ApplyListenerOperation(ServerListenerOrchestrator* orchestrator,
                                                     const ServerBootstrapConfig& config,
                                                     const ServerLifecycleArtifacts& artifacts,
                                                     const std::string& operation_key,
                                                     const std::string& target_uuid,
                                                     const std::string& mode) {
  ServerListenerOperationResult result;
  result.generation = ++orchestrator->generation;
  auto* profile = FindTarget(orchestrator, target_uuid);
  if (profile == nullptr) {
    result.diagnostics.push_back(ListenerDiagnostic(
        "LISTENER.NOT_FOUND",
        "The requested listener profile does not exist or is not visible.",
        {{"target_uuid", target_uuid}}));
    return result;
  }

  result.target_uuid = profile->listener_uuid;
  result.state_before = profile->state;
  if (operation_key == "listener_proxy_execute") {
    result.diagnostics.push_back(ListenerDiagnostic(
        "LISTENER.EXECUTION_PROXY_FORBIDDEN",
        "The listener control plane must not proxy execution traffic to the engine.",
        {{"listener_uuid", profile->listener_uuid}}));
    result.state_after = profile->state;
    return result;
  }
  if (profile->state == "failed") {
    result.diagnostics.push_back(ListenerDiagnostic(
        profile->diagnostic_code.empty() ? "LISTENER.START_FAILED" : profile->diagnostic_code,
        "The listener profile is failed and cannot accept lifecycle control.",
        {{"listener_uuid", profile->listener_uuid}}));
    result.state_after = profile->state;
    return result;
  }

  if (operation_key == "start_listener") {
    profile->enabled = true;
    return LaunchListener(profile, config, artifacts);
  } else if (operation_key == "stop_listener") {
    return StopListenerProcess(profile, mode == "force" ? "force" : "graceful");
  } else if (operation_key == "restart_listener") {
    profile->enabled = true;
    StopListenerProcess(profile, "force");
    profile->enabled = true;
    profile->state = "stopped";
    return LaunchListener(profile, config, artifacts);
  } else if (operation_key == "drain_listener") {
    auto drain = SendManagementCommand(profile, "DRAIN", 1000);
    if (!drain.ok) {
      result.diagnostics = std::move(drain.diagnostics);
      result.state_after = profile->state;
      return result;
    }
    profile->state = "draining";
  } else {
    result.diagnostics.push_back(ListenerDiagnostic(
        "SERVER.MANAGEMENT.OPERATION_UNKNOWN",
        "The listener operation key is not supported by this server.",
        {{"operation_key", operation_key}, {"listener_uuid", profile->listener_uuid}}));
    result.state_after = profile->state;
    return result;
  }
  profile->last_transition = operation_key + (mode.empty() ? std::string{} : ":" + mode);
  result.state_after = profile->state;
  result.ok = true;
  result.outcome = "completed";
  return result;
}

}  // namespace scratchbird::server
