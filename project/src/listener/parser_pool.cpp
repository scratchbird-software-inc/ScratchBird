// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "parser_pool.hpp"

#include <chrono>
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <map>
#include <sstream>
#include <thread>
#include <utility>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <afunix.h>
#else
#include <csignal>
#include <cstdlib>
#include <sys/socket.h>
#include <sys/wait.h>
#include <sys/un.h>
#include <unistd.h>
#endif

#include "listener_diagnostics.hpp"
#include "listener_tls_policy.hpp"

namespace scratchbird::listener {
namespace {

constexpr std::size_t kParserPoolFaultHistoryMax = 64;

#ifdef _WIN32
bool EnsureWinsockInitialized() {
  static const bool initialized = [] {
    WSADATA data{};
    return ::WSAStartup(MAKEWORD(2, 2), &data) == 0;
  }();
  return initialized;
}

void CloseControlHandle(ParserControlHandle* fd) {
  if (fd != nullptr && *fd >= 0) {
    ::closesocket(static_cast<SOCKET>(*fd));
    *fd = -1;
  }
}

void CloseFd(int* fd) {
  if (fd != nullptr && *fd >= 0) {
    ::closesocket(static_cast<SOCKET>(*fd));
    *fd = -1;
  }
}

void CloseProcessHandle(std::intptr_t* handle) {
  if (handle != nullptr && *handle != 0) {
    ::CloseHandle(reinterpret_cast<HANDLE>(*handle));
    *handle = 0;
  }
}

std::string NormalizeUnixSocketPath(std::filesystem::path path) {
  std::string text = path.generic_string();
  for (char& ch : text) {
    if (ch == '\\') ch = '/';
  }
  return text;
}

std::filesystem::path WindowsWorkerControlSocketPath(const ListenerConfig& config,
                                                     const ParserWorker& worker,
                                                     std::uint64_t now_ms) {
  std::filesystem::path base = config.runtime_dir.empty()
                                   ? std::filesystem::temp_directory_path()
                                   : std::filesystem::path(config.runtime_dir);
  return base / ("sb_listener_worker_" + std::to_string(::GetCurrentProcessId()) +
                 "_" + std::to_string(worker.numeric_worker_id) +
                 "_" + std::to_string(worker.generation) +
                 "_" + std::to_string(now_ms) + ".sock");
}

bool BindWindowsControlListener(const std::string& path, ParserControlHandle* out_fd) {
  if (out_fd == nullptr || !EnsureWinsockInitialized()) return false;
  *out_fd = -1;
  if (path.empty() || path.size() >= sizeof(sockaddr_un::sun_path)) return false;
  SOCKET fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd == INVALID_SOCKET) return false;
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
  (void)::DeleteFileA(path.c_str());
  if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
      ::listen(fd, 1) != 0) {
    ::closesocket(fd);
    (void)::DeleteFileA(path.c_str());
    return false;
  }
  *out_fd = static_cast<ParserControlHandle>(fd);
  return true;
}

ParserControlHandle AcceptWindowsControlSocket(ParserControlHandle listener_fd,
                                               std::uint32_t timeout_ms) {
  SOCKET listener = static_cast<SOCKET>(listener_fd);
  fd_set read_set;
  FD_ZERO(&read_set);
  FD_SET(listener, &read_set);
  timeval timeout{};
  timeout.tv_sec = static_cast<long>(timeout_ms / 1000u);
  timeout.tv_usec = static_cast<long>((timeout_ms % 1000u) * 1000u);
  const int ready = ::select(0, &read_set, nullptr, nullptr, &timeout);
  if (ready <= 0 || !FD_ISSET(listener, &read_set)) return -1;
  const SOCKET accepted = ::accept(listener, nullptr, nullptr);
  return accepted == INVALID_SOCKET ? -1 : static_cast<ParserControlHandle>(accepted);
}

std::string UpperEnvKey(std::string value) {
  for (char& ch : value) {
    if (ch >= 'a' && ch <= 'z') ch = static_cast<char>(ch - 'a' + 'A');
  }
  return value;
}

std::vector<char> BuildWindowsEnvironmentBlock(
    const std::vector<std::pair<std::string, std::string>>& overrides) {
  std::map<std::string, std::string> override_map;
  for (const auto& entry : overrides) {
    override_map.emplace(UpperEnvKey(entry.first), entry.first + "=" + entry.second);
  }

  std::vector<std::string> entries;
  LPCH current = ::GetEnvironmentStringsA();
  if (current != nullptr) {
    for (LPCH it = current; *it != '\0'; it += std::strlen(it) + 1) {
      const std::string row(it);
      const auto eq = row.find('=');
      if (eq == std::string::npos || eq == 0) {
        entries.push_back(row);
        continue;
      }
      if (override_map.find(UpperEnvKey(row.substr(0, eq))) == override_map.end()) {
        entries.push_back(row);
      }
    }
    ::FreeEnvironmentStringsA(current);
  }
  for (const auto& entry : override_map) {
    entries.push_back(entry.second);
  }
  std::sort(entries.begin(), entries.end(), [](const std::string& lhs, const std::string& rhs) {
    return UpperEnvKey(lhs) < UpperEnvKey(rhs);
  });

  std::vector<char> block;
  for (const auto& entry : entries) {
    block.insert(block.end(), entry.begin(), entry.end());
    block.push_back('\0');
  }
  block.push_back('\0');
  return block;
}

std::string QuoteWindowsCommandLineArgument(const std::string& arg) {
  if (arg.empty()) return "\"\"";
  bool needs_quote = false;
  for (const char ch : arg) {
    if (ch == ' ' || ch == '\t' || ch == '"') {
      needs_quote = true;
      break;
    }
  }
  if (!needs_quote) return arg;
  std::string out = "\"";
  std::size_t backslashes = 0;
  for (const char ch : arg) {
    if (ch == '\\') {
      ++backslashes;
      continue;
    }
    if (ch == '"') {
      out.append(backslashes * 2 + 1, '\\');
      out.push_back('"');
      backslashes = 0;
      continue;
    }
    out.append(backslashes, '\\');
    backslashes = 0;
    out.push_back(ch);
  }
  out.append(backslashes * 2, '\\');
  out.push_back('"');
  return out;
}

std::string BuildWindowsParserCommandLine(const std::string& executable) {
  return QuoteWindowsCommandLineArgument(executable) + " --listener-worker";
}

#else
void CloseFd(int* fd) {
  if (fd != nullptr && *fd >= 0) {
    ::close(*fd);
    *fd = -1;
  }
}
#endif

std::uint64_t NowMillis() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

proto::MessageVectorSet ErrorSet(std::string code,
                                 std::string message,
                                 std::string component,
                                 std::vector<proto::Field> fields = {}) {
  return MakeMessageVectorSet({MakeDiagnostic(std::move(code), "ERROR", std::move(message), std::move(component), std::move(fields))});
}

std::string WorkerProtocolForConfig(const ListenerConfig& config) {
  return config.protocol_family.empty() ? "sbsql" : config.protocol_family;
}

#ifndef _WIN32
std::string DescribeExitStatus(int status) {
  if (WIFEXITED(status)) {
    return "worker exited status=" + std::to_string(WEXITSTATUS(status));
  }
  if (WIFSIGNALED(status)) {
    return "worker terminated signal=" + std::to_string(WTERMSIG(status));
  }
  return "worker exited with unclassified wait status=" + std::to_string(status);
}
#endif

} // namespace

ParserPool::ParserPool(ListenerConfig config, ListenerMetrics* metrics) : config_(std::move(config)), metrics_(metrics) {}

proto::MessageVectorSet ParserPool::Start() {
  std::lock_guard lock(mutex_);
  running_ = true;
  draining_ = false;
  EnsureWarmWorkersLocked();
  if (metrics_) {
    metrics_->Increment("sys.metrics.listener.parser_pool.starts_total");
    metrics_->SetGauge("sys.metrics.listener.parser_pool.workers", static_cast<double>(workers_.size()));
  }
  return MakeMessageVectorSet({});
}

proto::MessageVectorSet ParserPool::Stop(bool force) {
  std::lock_guard lock(mutex_);
  draining_ = !force;
  for (auto& worker : workers_) {
    StopWorkerLocked(&worker, force);
  }
  running_ = false;
  draining_ = false;
  if (metrics_) {
    metrics_->Increment(force ? "sys.metrics.listener.parser_pool.force_stops_total" : "sys.metrics.listener.parser_pool.stops_total");
    metrics_->SetGauge("sys.metrics.listener.parser_pool.workers", static_cast<double>(workers_.size()));
  }
  return MakeMessageVectorSet({});
}

proto::MessageVectorSet ParserPool::Drain() {
  std::lock_guard lock(mutex_);
  draining_ = true;
  for (auto& worker : workers_) {
    if (worker.state == ParserWorkerState::kIdlePreauth) worker.state = ParserWorkerState::kDraining;
  }
  if (metrics_) metrics_->Increment("sys.metrics.listener.drain_requests_total");
  return MakeMessageVectorSet({});
}

ParserPoolDrainResult ParserPool::DrainAndWait(std::uint32_t timeout_ms) {
  const auto started = std::chrono::steady_clock::now();
  const auto deadline = started + std::chrono::milliseconds(timeout_ms);
  bool metric_recorded = false;
  for (;;) {
    {
      std::lock_guard lock(mutex_);
      draining_ = true;
      for (auto& worker : workers_) {
        if (worker.state == ParserWorkerState::kIdlePreauth) worker.state = ParserWorkerState::kDraining;
      }
      if (metrics_ && !metric_recorded) {
        metrics_->Increment("sys.metrics.listener.drain_requests_total");
        metric_recorded = true;
      }
      ReapExitedWorkersLocked();
      PurgeTerminalWorkersLocked();
      const auto busy = BusyWorkerCountLocked();
      ParserPoolDrainResult result;
      result.drained = busy == 0;
      result.active_worker_count = static_cast<std::uint32_t>(ActiveWorkerCountLocked());
      result.busy_worker_count = static_cast<std::uint32_t>(busy);
      result.running_worker_count = static_cast<std::uint32_t>(RunningWorkerCountLocked());
      result.timeout_ms = timeout_ms;
      result.waited_ms = static_cast<std::uint32_t>(
          std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::steady_clock::now() - started)
              .count());
      if (result.drained) {
        result.messages = MakeMessageVectorSet({MakeDiagnostic(
            "LISTENER.POOL.DRAIN_COMPLETE",
            "INFO",
            "parser pool drain completed with no active parser-owned clients",
            "sb_listener.pool",
            {{"active_worker_count", std::to_string(result.active_worker_count)},
             {"busy_worker_count", std::to_string(result.busy_worker_count)},
             {"running_worker_count", std::to_string(result.running_worker_count)},
             {"waited_ms", std::to_string(result.waited_ms)}})});
        return result;
      }
      if (std::chrono::steady_clock::now() >= deadline) {
        result.timed_out = true;
        result.messages = ErrorSet(
            "LISTENER.POOL.DRAIN_TIMEOUT",
            "parser pool drain timed out while parser-owned clients remained active",
            "sb_listener.pool",
            {{"active_worker_count", std::to_string(result.active_worker_count)},
             {"busy_worker_count", std::to_string(result.busy_worker_count)},
             {"running_worker_count", std::to_string(result.running_worker_count)},
             {"timeout_ms", std::to_string(result.timeout_ms)},
             {"waited_ms", std::to_string(result.waited_ms)},
             {"force_required", "true"}});
        return result;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
}

proto::MessageVectorSet ParserPool::Undrain() {
  std::lock_guard lock(mutex_);
  draining_ = false;
  ReapExitedWorkersLocked();
  PurgeTerminalWorkersLocked();
  for (auto& worker : workers_) {
    if (worker.state == ParserWorkerState::kDraining &&
        worker.hello_accepted &&
        worker.active_client_addr.empty() &&
        worker.active_connection_id == 0) {
      worker.state = ParserWorkerState::kIdlePreauth;
    }
  }
  EnsureWarmWorkersLocked();
  if (metrics_) metrics_->Increment("sys.metrics.listener.undrain_requests_total");
  return MakeMessageVectorSet({});
}

proto::MessageVectorSet ParserPool::Resize(std::uint32_t min_workers, std::uint32_t max_workers) {
  std::lock_guard lock(mutex_);
  if (min_workers > max_workers) {
    return ErrorSet("LISTENER.POOL.INVALID_RESIZE", "pool minimum cannot exceed maximum", "sb_listener.pool");
  }
  config_.warm_pool_min = min_workers;
  config_.warm_pool_max = max_workers;
  EnsureWarmWorkersLocked();
  while (workers_.size() > config_.warm_pool_max) {
    StopWorkerLocked(&workers_.back(), true);
    workers_.pop_back();
  }
  if (metrics_) metrics_->SetGauge("sys.metrics.listener.parser_pool.workers", static_cast<double>(workers_.size()));
  return MakeMessageVectorSet({});
}

proto::MessageVectorSet ParserPool::RestartWorker(const std::string& worker_id) {
  std::lock_guard lock(mutex_);
  auto* worker = FindWorkerLocked(worker_id);
  if (worker == nullptr) {
    return ErrorSet("LISTENER.POOL.WORKER_NOT_FOUND", "parser worker is not known", "sb_listener.pool", {{"worker_id", worker_id}});
  }
  StopWorkerLocked(worker, true);
  worker->hello_accepted = false;
  worker->active_client_addr.clear();
  worker->active_connection_id = 0;
  if (running_ && !draining_) {
    const auto now_ms = NowMillis();
    std::string block_reason;
    if (RetryBlockedLocked(now_ms, &block_reason)) {
      worker->state = ParserWorkerState::kStopped;
      worker->last_diagnostic = block_reason;
      return ErrorSet("LISTENER.POOL.RESTART_BLOCKED",
                      "parser worker restart is delayed by pool backoff or quarantine",
                      "sb_listener.pool",
                      {{"worker_id", worker_id}, {"reason", block_reason}});
    }
    worker->restart_count += 1;
    worker->generation += 1;
    worker->state = ParserWorkerState::kCold;
    LaunchWorkerLocked(worker, now_ms);
  }
  if (metrics_) metrics_->Increment("sys.metrics.listener.parser_pool.worker_restarts_total");
  return MakeMessageVectorSet({});
}

proto::MessageVectorSet ParserPool::KillWorker(const std::string& worker_id) {
  std::lock_guard lock(mutex_);
  auto* worker = FindWorkerLocked(worker_id);
  if (worker == nullptr) {
    return ErrorSet("LISTENER.POOL.WORKER_NOT_FOUND", "parser worker is not known", "sb_listener.pool", {{"worker_id", worker_id}});
  }
  StopWorkerLocked(worker, true);
  PurgeTerminalWorkersLocked();
  if (running_ && !draining_) EnsureWarmWorkersLocked();
  if (metrics_) metrics_->Increment("sys.metrics.listener.parser_pool.worker_kills_total");
  return MakeMessageVectorSet({});
}

proto::MessageVectorSet ParserPool::KillConnection(std::uint64_t connection_id) {
  std::lock_guard lock(mutex_);
  if (connection_id == 0) {
    return ErrorSet("LISTENER.KILL_INVALID_CONNECTION_ID",
                    "KILL requires a nonzero connection_id",
                    "sb_listener.pool",
                    {{"connection_id", std::to_string(connection_id)}});
  }
  auto* worker = FindWorkerByConnectionLocked(connection_id);
  if (worker == nullptr) {
    return ErrorSet("LISTENER.KILL_NOT_FOUND",
                    "no parser worker owns the requested connection_id",
                    "sb_listener.pool",
                    {{"connection_id", std::to_string(connection_id)}});
  }
  StopWorkerLocked(worker, true);
  PurgeTerminalWorkersLocked();
  if (running_ && !draining_) EnsureWarmWorkersLocked();
  if (metrics_) metrics_->Increment("sys.metrics.listener.parser_pool.connection_kills_total");
  return MakeMessageVectorSet({});
}

ParserHandoffResult ParserPool::HandoffClient(std::intptr_t client_fd,
                                              const std::string& client_addr,
                                              std::uint16_t client_port,
                                              std::uint64_t connection_id,
                                              const ParserHandoffBinding& binding) {
  std::lock_guard lock(mutex_);
  if (!running_ || draining_) {
    return {false, "draining", ErrorSet("LISTENER.HANDOFF_DRAINING", "listener is draining and refuses new handoffs", "sb_listener.handoff")};
  }
  ReapExitedWorkersLocked();
  PurgeTerminalWorkersLocked();
  auto* worker = FindIdleWorkerLocked();
  if (worker == nullptr) {
    EnsureWarmWorkersLocked();
    worker = FindIdleWorkerLocked();
  }
  if (worker == nullptr && config_.spawn_strategy == ParserSpawnStrategy::kOnDemand) {
    const auto now_ms = NowMillis();
    std::string block_reason;
    if (RetryBlockedLocked(now_ms, &block_reason)) {
      if (metrics_) metrics_->Increment("sys.metrics.listener.rejections_total.parser_pool_retry_blocked");
      return {false, block_reason, ErrorSet("LISTENER.HANDOFF_RETRY_BLOCKED", "parser worker spawn is delayed by restart backoff or quarantine", "sb_listener.handoff", {{"reason", block_reason}})};
    }
    worker = SpawnWorkerLocked(now_ms);
  }
  if (worker == nullptr) {
    const auto now_ms = NowMillis();
    std::string block_reason;
    if (RetryBlockedLocked(now_ms, &block_reason)) {
      if (metrics_) metrics_->Increment("sys.metrics.listener.rejections_total.parser_pool_retry_blocked");
      return {false, block_reason, ErrorSet("LISTENER.HANDOFF_RETRY_BLOCKED", "parser worker spawn is delayed by restart backoff or quarantine", "sb_listener.handoff", {{"reason", block_reason}})};
    }
    if (metrics_) metrics_->Increment("sys.metrics.listener.rejections_total.no_idle_parser");
    return {false, "no_idle_parser", ErrorSet("LISTENER.HANDOFF_NO_WORKER", "no admitted parser worker is available", "sb_listener.handoff")};
  }
  HandoffSocketPayload payload;
  payload.connection_id = connection_id;
  payload.protocol = WorkerProtocolForConfig(config_);
  payload.client_addr = client_addr;
  payload.client_port = client_port;
  ApplyListenerTlsHandoffPolicy(config_, &payload);
  payload.db_uuid = binding.db_uuid;
  payload.dbbt_id = binding.dbbt_id;
  payload.manager_session_id = binding.manager_session_id;
  payload.listener_id = binding.listener_id == 0 ? 1 : binding.listener_id;
  payload.auth_provider_family = binding.auth_provider_family;
  payload.auth_principal = binding.auth_principal;
  payload.auth_token = binding.auth_token;

  ListenerControlFrame frame;
  frame.opcode = ListenerControlOpcode::kHandoffSocket;
#ifdef _WIN32
  if (client_fd < 0 || worker->process_id == 0) {
    worker->state = ParserWorkerState::kQuarantined;
    worker->last_diagnostic = "Windows handoff requires a live client socket and target parser PID";
    RecordFaultLocked(worker, "handoff_windows_target_invalid", worker->last_diagnostic);
    worker->active_client_addr.clear();
    worker->active_connection_id = 0;
    if (metrics_) metrics_->Increment("sys.metrics.listener.rejections_total.handoff_windows_target_invalid");
    return {false, "handoff_windows_target_invalid", ErrorSet("LISTENER.HANDOFF_WINDOWS_TARGET_INVALID", worker->last_diagnostic, "sb_listener.handoff")};
  }
  WSAPROTOCOL_INFOA protocol_info{};
  if (::WSADuplicateSocketA(static_cast<SOCKET>(client_fd), worker->process_id, &protocol_info) != 0) {
    worker->state = ParserWorkerState::kQuarantined;
    worker->last_diagnostic = "WSADuplicateSocketA failed: " + std::to_string(::WSAGetLastError());
    RecordFaultLocked(worker, "handoff_windows_duplicate_socket_failed", worker->last_diagnostic);
    worker->active_client_addr.clear();
    worker->active_connection_id = 0;
    if (metrics_) metrics_->Increment("sys.metrics.listener.rejections_total.handoff_windows_duplicate_socket_failed");
    return {false, "handoff_windows_duplicate_socket_failed", ErrorSet("LISTENER.HANDOFF_WINDOWS_DUPLICATE_SOCKET_FAILED", worker->last_diagnostic, "sb_listener.handoff")};
  }
  frame.flags = kControlFlagWindowsSocketInfo;
  frame.sequence = connection_id;
  frame.payload = EncodeWindowsSocketHandoffPayload(
      payload,
      reinterpret_cast<const std::uint8_t*>(&protocol_info),
      sizeof(protocol_info));
  if (frame.payload.empty()) {
    worker->state = ParserWorkerState::kQuarantined;
    worker->last_diagnostic = "Windows socket handoff payload encode failed";
    RecordFaultLocked(worker, "handoff_windows_payload_encode_failed", worker->last_diagnostic);
    worker->active_client_addr.clear();
    worker->active_connection_id = 0;
    if (metrics_) metrics_->Increment("sys.metrics.listener.rejections_total.handoff_windows_payload_encode_failed");
    return {false, "handoff_windows_payload_encode_failed", ErrorSet("LISTENER.HANDOFF_WINDOWS_PAYLOAD_ENCODE_FAILED", worker->last_diagnostic, "sb_listener.handoff")};
  }
#else
  frame.flags = kControlFlagHasHandle;
  frame.sequence = connection_id;
  frame.payload = EncodeHandoffSocketPayload(payload);
#endif

  worker->state = ParserWorkerState::kAssigned;
  worker->awaiting_ack = true;
  worker->awaiting_request = connection_id;
  worker->active_client_addr = client_addr;
  worker->active_connection_id = connection_id;
  if (!SendControlFrame(worker->control_fd, frame,
#ifdef _WIN32
                        static_cast<ParserControlHandle>(-1)
#else
                        static_cast<int>(client_fd)
#endif
                        )) {
    worker->state = ParserWorkerState::kQuarantined;
    worker->last_diagnostic = "handoff send failed";
    RecordFaultLocked(worker, "handoff_send_failed", worker->last_diagnostic);
    worker->active_client_addr.clear();
    worker->active_connection_id = 0;
    if (metrics_) metrics_->Increment("sys.metrics.listener.rejections_total.handoff_send_failed");
    return {false, "handoff_send_failed", ErrorSet("LISTENER.HANDOFF_SEND_FAILED", "failed to transfer client socket to parser worker", "sb_listener.handoff")};
  }

  ListenerControlDecodeResult decoded;
  int received_fd = -1;
  if (!ReadControlFrame(worker->control_fd, &decoded, &received_fd, config_.handoff_ack_timeout_ms) ||
      decoded.frame.opcode != ListenerControlOpcode::kHandoffAck) {
    CloseFd(&received_fd);
    if (decoded.ok && decoded.frame.opcode == ListenerControlOpcode::kErrorMessage) {
      auto error = DecodeErrorMessagePayload(decoded.frame.payload, &decoded.messages);
      worker->state = ParserWorkerState::kQuarantined;
      worker->awaiting_ack = false;
      worker->last_diagnostic = error ? error->reason : "parser ERROR_MESSAGE malformed";
      RecordFaultLocked(worker, "parser_error_message", worker->last_diagnostic);
      worker->active_client_addr.clear();
      worker->active_connection_id = 0;
      if (metrics_) metrics_->Increment("sys.metrics.listener.parser_pool.worker_error_messages_total");
      return {false, "parser_error_message", ErrorSet("LISTENER.PARSER_ERROR_MESSAGE", "parser worker sent ERROR_MESSAGE", "sb_listener.handoff", {{"worker_id", worker->worker_id}, {"reason", worker->last_diagnostic}})};
    }
    worker->state = ParserWorkerState::kQuarantined;
    worker->awaiting_ack = false;
    worker->last_diagnostic = "handoff ack timeout or wrong message";
    RecordFaultLocked(worker, "handoff_ack_failed", worker->last_diagnostic);
    worker->active_client_addr.clear();
    worker->active_connection_id = 0;
    if (metrics_) metrics_->Increment("sys.metrics.listener.rejections_total.handoff_ack_failed");
    return {false, "handoff_ack_failed", ErrorSet("LISTENER.HANDOFF_ACK_FAILED", "parser worker did not acknowledge socket handoff", "sb_listener.handoff")};
  }
  CloseFd(&received_fd);
  auto ack = DecodeHandoffAckPayload(decoded.frame.payload, &decoded.messages);
  if (!ack || ack->connection_id_echo != connection_id || !ack->accepted) {
    worker->state = ParserWorkerState::kQuarantined;
    worker->awaiting_ack = false;
    worker->last_diagnostic = ack ? ack->reason : "handoff ack malformed";
    RecordFaultLocked(worker, "handoff_rejected", worker->last_diagnostic);
    worker->active_client_addr.clear();
    worker->active_connection_id = 0;
    if (metrics_) metrics_->Increment("sys.metrics.listener.rejections_total.handoff_rejected");
    return {false, "handoff_rejected", ErrorSet("LISTENER.HANDOFF_REJECTED", "parser worker rejected socket handoff", "sb_listener.handoff")};
  }
  worker->served_connections += 1;
  worker->awaiting_ack = false;
  worker->state = ParserWorkerState::kDraining;
  if (metrics_) {
    metrics_->Increment("sys.metrics.listener.handoff_complete_total");
    metrics_->SetGauge("sys.metrics.listener.parser_pool.workers", static_cast<double>(workers_.size()));
  }
  EnsureWarmWorkersLocked();
  return {true, "accepted", MakeMessageVectorSet({})};
}

std::vector<std::string> ParserPool::CollectCompletedClientSessions() {
  std::lock_guard lock(mutex_);
  std::vector<std::string> completed;
  completed.swap(completed_client_sessions_);
  for (auto& worker : workers_) {
#ifdef _WIN32
    if (worker.process_handle == 0) continue;
    const DWORD wait_rc = ::WaitForSingleObject(reinterpret_cast<HANDLE>(worker.process_handle), 0);
    if (wait_rc != WAIT_OBJECT_0) continue;
    CloseProcessHandle(&worker.process_handle);
    CloseControlHandle(&worker.control_fd);
    worker.process_id = 0;
#else
    if (worker.process_id <= 0) continue;
    int status = 0;
    const auto rc = ::waitpid(worker.process_id, &status, WNOHANG);
    if (rc != worker.process_id) continue;
    worker.process_id = -1;
    CloseFd(&worker.control_fd);
#endif
    if (!worker.active_client_addr.empty()) {
      completed.push_back(worker.active_client_addr);
    }
    worker.active_client_addr.clear();
    worker.active_connection_id = 0;
    worker.state = ParserWorkerState::kStopped;
    worker.hello_accepted = false;
    worker.awaiting_ack = false;
    if (metrics_) metrics_->Increment("sys.metrics.listener.parser_pool.completed_sessions_total");
  }
  if (running_ && !draining_) EnsureWarmWorkersLocked();
  if (metrics_) metrics_->SetGauge("sys.metrics.listener.parser_pool.workers", static_cast<double>(workers_.size()));
  return completed;
}

ParserPoolStatus ParserPool::Status() const {
  std::lock_guard lock(mutex_);
  const auto now_ms = NowMillis();
  std::uint32_t recent_count = 0;
  const auto window_ms = static_cast<std::uint64_t>(config_.child_quarantine_window_ms);
  for (const auto timestamp_ms : recent_failure_timestamps_ms_) {
    if (window_ms == 0 || timestamp_ms + window_ms >= now_ms) {
      ++recent_count;
    }
  }
  return {running_,
          draining_,
          config_.warm_pool_min,
          config_.warm_pool_max,
          static_cast<std::uint32_t>(ActiveWorkerCountLocked()),
          static_cast<std::uint32_t>(BusyWorkerCountLocked()),
          static_cast<std::uint32_t>(RunningWorkerCountLocked()),
          recent_count,
          last_backoff_ms_,
          next_restart_at_ms_,
          quarantine_until_ms_ > now_ms,
          quarantine_until_ms_,
          workers_,
          std::vector<ParserPoolFaultEvent>(fault_history_.begin(), fault_history_.end())};
}

std::string ParserPool::StatusJson() const {
  const auto status = Status();
  std::uint32_t running_workers = 0;
  std::uint32_t warm_workers = 0;
  std::uint32_t busy_workers = 0;
  std::uint32_t fault_workers = 0;
  std::uint32_t starting_workers = 0;
  for (const auto& worker : status.workers) {
    if (worker.state == ParserWorkerState::kQuarantined || worker.state == ParserWorkerState::kStopped) {
      ++fault_workers;
      continue;
    }
    if (worker.state == ParserWorkerState::kCold) continue;
    ++running_workers;
    if (worker.state == ParserWorkerState::kStarting) ++starting_workers;
    if (worker.state == ParserWorkerState::kIdlePreauth) ++warm_workers;
    if (worker.state == ParserWorkerState::kAssigned ||
        (worker.state == ParserWorkerState::kDraining && worker.active_connection_id != 0)) {
      ++busy_workers;
    }
  }
  const bool pool_warm = config_.spawn_strategy == ParserSpawnStrategy::kOnDemand ||
                         warm_workers >= status.target_min;
  const bool retry_blocked = status.next_restart_at_ms > NowMillis() || status.quarantine_active;
  const bool pool_ready = status.running && !status.draining && pool_warm && !retry_blocked;
  std::ostringstream out;
  out << "{\"running\":" << (status.running ? "true" : "false")
      << ",\"draining\":" << (status.draining ? "true" : "false")
      << ",\"target_min\":" << status.target_min
      << ",\"target_max\":" << status.target_max
      << ",\"active_worker_count\":" << status.active_worker_count
      << ",\"busy_worker_count\":" << status.busy_worker_count
      << ",\"running_worker_count\":" << status.running_worker_count
      << ",\"accepting_new_handoffs\":"
      << (status.running && !status.draining && !retry_blocked ? "true" : "false")
      << ",\"spawn_strategy\":\"" << SpawnStrategyName(config_.spawn_strategy) << "\""
      << ",\"runtime_enabled\":" << (status.running ? "true" : "false")
      << ",\"parser_pool_ready\":" << (pool_ready ? "true" : "false")
      << ",\"parser_pool_warm\":" << (pool_warm ? "true" : "false")
      << ",\"parser_pool_retry_blocked\":" << (retry_blocked ? "true" : "false")
      << ",\"running_workers\":" << running_workers
      << ",\"warm_workers\":" << warm_workers
      << ",\"starting_workers\":" << starting_workers
      << ",\"busy_workers\":" << busy_workers
      << ",\"fault_workers\":" << fault_workers
      << ",\"recent_failure_count\":" << status.recent_failure_count
      << ",\"last_backoff_ms\":" << status.last_backoff_ms
      << ",\"next_restart_at_ms\":" << status.next_restart_at_ms
      << ",\"quarantine_active\":" << (status.quarantine_active ? "true" : "false")
      << ",\"quarantine_until_ms\":" << status.quarantine_until_ms
      << ",\"fault_history_count\":" << status.fault_history.size()
      << ",\"fault_history_max\":" << kParserPoolFaultHistoryMax
      << ",\"pool_min\":" << status.target_min
      << ",\"pool_max\":" << status.target_max
      << ",\"quarantine_class\":\"parser_worker\""
      << ",\"workers\":[";
  for (std::size_t i = 0; i < status.workers.size(); ++i) {
    const auto& worker = status.workers[i];
    std::string state_class = WorkerStateClassName(worker.state);
    if (worker.state == ParserWorkerState::kDraining && worker.active_connection_id != 0) {
      state_class = "BUSY";
    }
    if (i != 0) out << ',';
    out << "{\"worker_id\":\"" << QuoteJson(worker.worker_id) << "\","
        << "\"state\":\"" << WorkerStateName(worker.state) << "\","
        << "\"state_class\":\"" << state_class << "\","
        << "\"generation\":" << worker.generation << ','
        << "\"numeric_worker_id\":" << worker.numeric_worker_id << ','
        << "\"served_connections\":" << worker.served_connections << ','
        << "\"restart_count\":" << worker.restart_count << ','
        << "\"active_connection_id\":" << worker.active_connection_id << ','
        << "\"last_started_at_ms\":" << worker.last_started_at_ms << ','
        << "\"last_failure_at_ms\":" << worker.last_failure_at_ms << ','
        << "\"last_backoff_ms\":" << worker.last_backoff_ms << ','
        << "\"next_restart_at_ms\":" << worker.next_restart_at_ms << ','
        << "\"quarantine_until_ms\":" << worker.quarantine_until_ms << ','
        << "\"failure_count\":" << worker.failure_count << ','
        << "\"last_diagnostic\":\"" << QuoteJson(worker.last_diagnostic) << "\","
        << "\"hello_accepted\":" << (worker.hello_accepted ? "true" : "false");
#ifndef _WIN32
    out << ",\"process_id\":" << worker.process_id;
#endif
    out << '}';
  }
  out << "],\"fault_history\":[";
  for (std::size_t i = 0; i < status.fault_history.size(); ++i) {
    const auto& fault = status.fault_history[i];
    if (i != 0) out << ',';
    out << "{\"timestamp_ms\":" << fault.timestamp_ms << ','
        << "\"worker_id\":\"" << QuoteJson(fault.worker_id) << "\","
        << "\"generation\":" << fault.generation << ','
        << "\"numeric_worker_id\":" << fault.numeric_worker_id << ','
        << "\"event\":\"" << QuoteJson(fault.event) << "\","
        << "\"diagnostic\":\"" << QuoteJson(fault.diagnostic) << "\","
        << "\"backoff_ms\":" << fault.backoff_ms << ','
        << "\"next_retry_at_ms\":" << fault.next_retry_at_ms << ','
        << "\"quarantine_active\":" << (fault.quarantine_active ? "true" : "false") << ','
        << "\"quarantine_until_ms\":" << fault.quarantine_until_ms << ','
        << "\"recent_failure_count\":" << fault.recent_failure_count << ','
        << "\"intentional\":" << (fault.intentional ? "true" : "false")
        << '}';
  }
  out << "]}";
  return out.str();
}

ParserWorker ParserPool::MakeWorker(std::uint64_t ordinal) const {
  ParserWorker worker;
  worker.worker_id = config_.protocol_family + "-parser-" + std::to_string(ordinal);
  worker.numeric_worker_id = ordinal;
  worker.generation = 1;
  return worker;
}

void ParserPool::EnsureWarmWorkersLocked() {
  if (config_.spawn_strategy != ParserSpawnStrategy::kWarmPool) {
    return;
  }
  ReapExitedWorkersLocked();
  PurgeTerminalWorkersLocked();
  if (!running_ || draining_) {
    return;
  }
  const auto now_ms = NowMillis();
  std::string block_reason;
  if (RetryBlockedLocked(now_ms, &block_reason)) {
    return;
  }
  auto warm_count = [&] {
    std::size_t count = 0;
    for (const auto& worker : workers_) {
      if (worker.state == ParserWorkerState::kIdlePreauth ||
          worker.state == ParserWorkerState::kStarting ||
          worker.state == ParserWorkerState::kAssigned) {
        ++count;
      }
    }
    return count;
  };
  while (warm_count() < config_.warm_pool_min && ActiveWorkerCountLocked() < config_.warm_pool_max) {
    if (RetryBlockedLocked(NowMillis(), &block_reason)) {
      break;
    }
    if (SpawnWorkerLocked(NowMillis()) == nullptr) {
      break;
    }
  }
}

ParserWorker* ParserPool::FindWorkerLocked(const std::string& worker_id) {
  for (auto& worker : workers_) {
    if (worker.worker_id == worker_id) return &worker;
  }
  return nullptr;
}

ParserWorker* ParserPool::FindWorkerByConnectionLocked(std::uint64_t connection_id) {
  for (auto& worker : workers_) {
    if (worker.active_connection_id == connection_id) return &worker;
  }
  return nullptr;
}

ParserWorker* ParserPool::FindIdleWorkerLocked() {
  for (auto& worker : workers_) {
    if (worker.state == ParserWorkerState::kIdlePreauth && worker.hello_accepted) return &worker;
  }
  return nullptr;
}

std::size_t ParserPool::ActiveWorkerCountLocked() const {
  std::size_t count = 0;
  for (const auto& worker : workers_) {
    switch (worker.state) {
      case ParserWorkerState::kStarting:
      case ParserWorkerState::kIdlePreauth:
      case ParserWorkerState::kAssigned:
      case ParserWorkerState::kDraining:
        ++count;
        break;
      case ParserWorkerState::kCold:
      case ParserWorkerState::kStopped:
      case ParserWorkerState::kQuarantined:
        break;
    }
  }
  return count;
}

std::size_t ParserPool::BusyWorkerCountLocked() const {
  std::size_t count = 0;
  for (const auto& worker : workers_) {
    if (worker.state == ParserWorkerState::kAssigned || worker.active_connection_id != 0) {
      ++count;
    }
  }
  return count;
}

std::size_t ParserPool::RunningWorkerCountLocked() const {
  std::size_t count = 0;
  for (const auto& worker : workers_) {
    switch (worker.state) {
      case ParserWorkerState::kStarting:
      case ParserWorkerState::kIdlePreauth:
      case ParserWorkerState::kAssigned:
      case ParserWorkerState::kDraining:
        ++count;
        break;
      case ParserWorkerState::kCold:
      case ParserWorkerState::kStopped:
      case ParserWorkerState::kQuarantined:
        break;
    }
  }
  return count;
}

bool ParserPool::RetryBlockedLocked(std::uint64_t now_ms, std::string* reason) const {
  if (quarantine_until_ms_ > now_ms) {
    if (reason != nullptr) *reason = "parser_pool_quarantined";
    return true;
  }
  if (next_restart_at_ms_ > now_ms) {
    if (reason != nullptr) *reason = "parser_pool_backoff";
    return true;
  }
  if (reason != nullptr) reason->clear();
  return false;
}

ParserWorker* ParserPool::SpawnWorkerLocked(std::uint64_t now_ms) {
  if (!running_ || draining_) return nullptr;
  ReapExitedWorkersLocked();
  PurgeTerminalWorkersLocked();
  std::string block_reason;
  if (RetryBlockedLocked(now_ms, &block_reason)) {
    return nullptr;
  }
  if (ActiveWorkerCountLocked() >= config_.warm_pool_max) {
    return nullptr;
  }
  workers_.push_back(MakeWorker(next_worker_ordinal_++));
  auto* worker = &workers_.back();
  if (!LaunchWorkerLocked(worker, now_ms)) {
    return nullptr;
  }
  return worker->state == ParserWorkerState::kIdlePreauth && worker->hello_accepted ? worker : nullptr;
}

bool ParserPool::LaunchWorkerLocked(ParserWorker* worker, std::uint64_t now_ms) {
  if (worker == nullptr) return false;
  worker->last_started_at_ms = now_ms;
  worker->last_diagnostic.clear();
  worker->next_restart_at_ms = 0;
  worker->quarantine_until_ms = 0;
  if (config_.parser_executable.empty()) {
    worker->state = ParserWorkerState::kQuarantined;
    worker->last_diagnostic = "parser_executable is required for interface-complete listener operation";
    RecordFaultLocked(worker, "spawn_configuration_refused", worker->last_diagnostic);
    if (metrics_) metrics_->Increment("sys.metrics.listener.parser_pool.worker_spawn_failed_total");
    return false;
  }
#ifdef _WIN32
  if (!EnsureWinsockInitialized()) {
    worker->state = ParserWorkerState::kQuarantined;
    worker->last_diagnostic = "Winsock initialization failed";
    RecordFaultLocked(worker, "spawn_winsock_initialization_failed", worker->last_diagnostic);
    if (metrics_) metrics_->Increment("sys.metrics.listener.parser_pool.worker_spawn_failed_total");
    return false;
  }
  std::error_code mkdir_ec;
  std::filesystem::create_directories(config_.runtime_dir, mkdir_ec);
  if (mkdir_ec) {
    worker->state = ParserWorkerState::kQuarantined;
    worker->last_diagnostic = "runtime directory create failed: " + mkdir_ec.message();
    RecordFaultLocked(worker, "spawn_runtime_dir_failed", worker->last_diagnostic);
    if (metrics_) metrics_->Increment("sys.metrics.listener.parser_pool.worker_spawn_failed_total");
    return false;
  }
  worker->control_socket_path = NormalizeUnixSocketPath(
      WindowsWorkerControlSocketPath(config_, *worker, now_ms));
  ParserControlHandle control_listener = -1;
  if (!BindWindowsControlListener(worker->control_socket_path, &control_listener)) {
    worker->state = ParserWorkerState::kQuarantined;
    worker->last_diagnostic = "Windows parser control socket bind failed";
    RecordFaultLocked(worker, "spawn_control_socket_bind_failed", worker->last_diagnostic);
    if (metrics_) metrics_->Increment("sys.metrics.listener.parser_pool.worker_spawn_failed_total");
    return false;
  }

  const std::string numeric_worker_id = std::to_string(worker->numeric_worker_id);
  const std::vector<std::pair<std::string, std::string>> environment = {
      {"SB_LISTENER_PREAUTH", "1"},
      {"SB_LISTENER_CONTROL_SOCKET", worker->control_socket_path},
      {"SB_LISTENER_CONTROL_TRANSPORT", "windows-afunix-v1"},
      {"SB_SERVER_ENDPOINT", config_.server_endpoint},
      {"SB_DATABASE_SELECTOR", config_.database_selector},
      {"SB_DATABASE_TOKEN", config_.database_selector},
      {"SB_PROTOCOL_FAMILY", config_.protocol_family},
      {"SB_PARSER_PACKAGE", config_.parser_package},
      {"SB_PARSER_WORKER_ID", worker->worker_id},
      {"SB_PARSER_WORKER_NUMERIC_ID", numeric_worker_id},
      {"SB_PARSER_PROFILE_ID", config_.listener_profile},
      {"SB_PARSER_BUNDLE_CONTRACT_ID", config_.bundle_contract_id},
      {"SB_PARSER_API_MAJOR", std::to_string(config_.parser_api_major)},
      {"SB_TLS_REQUIRED", config_.tls_required ? "1" : "0"},
      {"SB_TLS_CERT_FILE", config_.tls_cert_file},
      {"SB_TLS_KEY_FILE", config_.tls_key_file},
      {"SB_TLS_CA_FILE", config_.tls_ca_file},
  };
  auto env_block = BuildWindowsEnvironmentBlock(environment);
  std::string command_line = BuildWindowsParserCommandLine(config_.parser_executable);
  STARTUPINFOA startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process{};
  worker->state = ParserWorkerState::kStarting;
  const BOOL created = ::CreateProcessA(nullptr,
                                        command_line.data(),
                                        nullptr,
                                        nullptr,
                                        FALSE,
                                        0,
                                        env_block.data(),
                                        nullptr,
                                        &startup,
                                        &process);
  if (!created) {
    CloseControlHandle(&control_listener);
    (void)::DeleteFileA(worker->control_socket_path.c_str());
    worker->state = ParserWorkerState::kQuarantined;
    worker->last_diagnostic = "CreateProcessA failed: " + std::to_string(::GetLastError());
    RecordFaultLocked(worker, "spawn_create_process_failed", worker->last_diagnostic);
    if (metrics_) metrics_->Increment("sys.metrics.listener.parser_pool.worker_spawn_failed_total");
    return false;
  }
  ::CloseHandle(process.hThread);
  worker->process_id = process.dwProcessId;
  worker->process_handle = reinterpret_cast<std::intptr_t>(process.hProcess);
  worker->control_fd = AcceptWindowsControlSocket(control_listener, config_.preauth_timeout_ms);
  CloseControlHandle(&control_listener);
  (void)::DeleteFileA(worker->control_socket_path.c_str());
  if (worker->control_fd < 0) {
    StopWorkerLocked(worker, true);
    worker->state = ParserWorkerState::kQuarantined;
    worker->last_diagnostic = "Windows parser control socket accept timed out";
    RecordFaultLocked(worker, "spawn_control_socket_accept_timeout", worker->last_diagnostic);
    if (metrics_) metrics_->Increment("sys.metrics.listener.parser_pool.worker_spawn_failed_total");
    return false;
  }
  if (!AdmitWorkerLocked(worker)) {
    StopWorkerLocked(worker, true);
    if (metrics_) metrics_->Increment("sys.metrics.listener.parser_pool.hello_rejected_total");
    return false;
  }
  return true;
#else
  int sockets[2] = {-1, -1};
  if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0) {
    worker->state = ParserWorkerState::kQuarantined;
    worker->last_diagnostic = std::string("socketpair failed: ") + std::strerror(errno);
    RecordFaultLocked(worker, "spawn_socketpair_failed", worker->last_diagnostic);
    if (metrics_) metrics_->Increment("sys.metrics.listener.parser_pool.worker_spawn_failed_total");
    return false;
  }
  worker->state = ParserWorkerState::kStarting;
  pid_t pid = ::fork();
  if (pid == 0) {
    ::close(sockets[0]);
    const std::string control_fd = std::to_string(sockets[1]);
    const std::string numeric_worker_id = std::to_string(worker->numeric_worker_id);
    ::setenv("SB_LISTENER_PREAUTH", "1", 1);
    ::setenv("SB_LISTENER_CONTROL_FD", control_fd.c_str(), 1);
    ::setenv("SB_SERVER_ENDPOINT", config_.server_endpoint.c_str(), 1);
    ::setenv("SB_DATABASE_SELECTOR", config_.database_selector.c_str(), 1);
    ::setenv("SB_DATABASE_TOKEN", config_.database_selector.c_str(), 1);
    ::setenv("SB_PROTOCOL_FAMILY", config_.protocol_family.c_str(), 1);
    ::setenv("SB_PARSER_PACKAGE", config_.parser_package.c_str(), 1);
    ::setenv("SB_PARSER_WORKER_ID", worker->worker_id.c_str(), 1);
    ::setenv("SB_PARSER_WORKER_NUMERIC_ID", numeric_worker_id.c_str(), 1);
    ::setenv("SB_PARSER_PROFILE_ID", config_.listener_profile.c_str(), 1);
    ::setenv("SB_PARSER_BUNDLE_CONTRACT_ID", config_.bundle_contract_id.c_str(), 1);
    ::setenv("SB_PARSER_API_MAJOR", std::to_string(config_.parser_api_major).c_str(), 1);
    ::setenv("SB_TLS_REQUIRED", config_.tls_required ? "1" : "0", 1);
    ::setenv("SB_TLS_CERT_FILE", config_.tls_cert_file.c_str(), 1);
    ::setenv("SB_TLS_KEY_FILE", config_.tls_key_file.c_str(), 1);
    ::setenv("SB_TLS_CA_FILE", config_.tls_ca_file.c_str(), 1);
    ::execl(config_.parser_executable.c_str(), config_.parser_executable.c_str(), "--listener-worker", nullptr);
    _exit(127);
  }
  ::close(sockets[1]);
  if (pid <= 0) {
    CloseFd(&sockets[0]);
    worker->state = ParserWorkerState::kQuarantined;
    worker->last_diagnostic = std::string("fork failed: ") + std::strerror(errno);
    RecordFaultLocked(worker, "spawn_fork_failed", worker->last_diagnostic);
    if (metrics_) metrics_->Increment("sys.metrics.listener.parser_pool.worker_spawn_failed_total");
    return false;
  }
  worker->process_id = static_cast<int>(pid);
  worker->control_fd = sockets[0];
  if (!AdmitWorkerLocked(worker)) {
    StopWorkerLocked(worker, true);
    if (metrics_) metrics_->Increment("sys.metrics.listener.parser_pool.hello_rejected_total");
    return false;
  }
  return true;
#endif
}

bool ParserPool::AdmitWorkerLocked(ParserWorker* worker) {
  if (worker == nullptr) return false;
  ListenerControlDecodeResult decoded;
  int received_fd = -1;
  if (!ReadControlFrame(worker->control_fd, &decoded, &received_fd, config_.preauth_timeout_ms)) {
    CloseFd(&received_fd);
    worker->state = ParserWorkerState::kQuarantined;
    worker->last_diagnostic = "HELLO timeout";
    RecordFaultLocked(worker, "hello_timeout", worker->last_diagnostic);
    return false;
  }
  CloseFd(&received_fd);
  if (!decoded.ok || decoded.frame.opcode != ListenerControlOpcode::kHello) {
    worker->state = ParserWorkerState::kQuarantined;
    worker->last_diagnostic = "first worker message was not HELLO";
    RecordFaultLocked(worker, "hello_invalid", worker->last_diagnostic);
    return false;
  }
  auto hello = DecodeHelloPayload(decoded.frame.payload, &decoded.messages);
  std::string reason;
  if (!hello) {
    reason = "LISTENER.HELLO_TRUNCATED";
  } else if (hello->pid == 0) {
    reason = "LISTENER.HELLO_PID_INVALID";
  } else if (hello->parser_api_major == 0) {
    reason = "LISTENER.HELLO_API_MAJOR_REQUIRED";
  } else if (hello->parser_api_major != config_.parser_api_major) {
    reason = "LISTENER.PARSER_API_MISMATCH";
  } else if (hello->protocol != WorkerProtocolForConfig(config_)) {
    reason = "LISTENER.PARSER_FAMILY_MISMATCH";
  } else if (hello->profile_id != config_.listener_profile) {
    reason = "LISTENER.PARSER_PROFILE_MISMATCH";
  } else if (hello->bundle_contract_id != config_.bundle_contract_id) {
    reason = "LISTENER.PARSER_BUNDLE_CONTRACT_MISMATCH";
  }

  HelloAckPayload ack;
  ack.accepted = reason.empty();
  ack.reason = reason;
  ListenerControlFrame ack_frame;
  ack_frame.opcode = ListenerControlOpcode::kHelloAck;
  ack_frame.sequence = decoded.frame.sequence;
  ack_frame.payload = EncodeHelloAckPayload(ack);
  SendControlFrame(worker->control_fd, ack_frame);
  if (!ack.accepted) {
    worker->state = ParserWorkerState::kQuarantined;
    worker->last_diagnostic = reason;
    RecordFaultLocked(worker, "hello_rejected", worker->last_diagnostic);
    return false;
  }
  worker->hello_accepted = true;
  worker->state = ParserWorkerState::kIdlePreauth;
  worker->failure_count = 0;
  worker->last_backoff_ms = 0;
  worker->next_restart_at_ms = 0;
  worker->quarantine_until_ms = 0;
  if (metrics_) metrics_->Increment("sys.metrics.listener.parser_pool.hello_accepted_total");
  return true;
}

void ParserPool::StopWorkerLocked(ParserWorker* worker, bool force) {
  if (worker == nullptr) return;
  if (worker->control_fd >= 0 && !force) {
    ListenerControlFrame recycle;
    recycle.opcode = ListenerControlOpcode::kRecycle;
    recycle.sequence = worker->generation;
    recycle.payload = EncodeRecyclePayload(4);
    SendControlFrame(worker->control_fd, recycle);
  }
#ifdef _WIN32
  if (worker->process_handle != 0) {
    HANDLE process = reinterpret_cast<HANDLE>(worker->process_handle);
    if (force) {
      ::TerminateProcess(process, 1);
    }
    const DWORD timeout = force ? 1000u : static_cast<DWORD>(config_.graceful_drain_timeout_ms);
    DWORD wait_rc = ::WaitForSingleObject(process, timeout);
    if (wait_rc == WAIT_TIMEOUT) {
      ::TerminateProcess(process, 1);
      (void)::WaitForSingleObject(process, 1000);
    }
    CloseProcessHandle(&worker->process_handle);
    worker->process_id = 0;
  }
  CloseControlHandle(&worker->control_fd);
  if (!worker->control_socket_path.empty()) {
    (void)::DeleteFileA(worker->control_socket_path.c_str());
    worker->control_socket_path.clear();
  }
#else
  if (worker->process_id > 0) {
    if (force) {
      ::kill(worker->process_id, SIGKILL);
    } else {
      ::kill(worker->process_id, SIGTERM);
    }
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(force ? 1000 : config_.graceful_drain_timeout_ms);
    for (;;) {
      int status = 0;
      const auto rc = ::waitpid(worker->process_id, &status, WNOHANG);
      if (rc == worker->process_id) break;
      if (rc < 0) break;
      if (std::chrono::steady_clock::now() >= deadline) {
        ::kill(worker->process_id, SIGKILL);
        ::waitpid(worker->process_id, &status, 0);
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    worker->process_id = -1;
  }
  CloseFd(&worker->control_fd);
#endif
  worker->state = ParserWorkerState::kStopped;
  worker->hello_accepted = false;
  worker->awaiting_ack = false;
  worker->active_client_addr.clear();
  worker->active_connection_id = 0;
}

void ParserPool::ReapWorkerLocked(ParserWorker* worker) {
  if (worker == nullptr) return;
#ifdef _WIN32
  if (worker->process_handle != 0) {
    HANDLE process = reinterpret_cast<HANDLE>(worker->process_handle);
    const DWORD wait_rc = ::WaitForSingleObject(process, 0);
    if (wait_rc == WAIT_OBJECT_0) {
      CloseProcessHandle(&worker->process_handle);
      CloseControlHandle(&worker->control_fd);
      worker->process_id = 0;
      const bool normal_session_completion =
          worker->state == ParserWorkerState::kDraining && worker->active_connection_id != 0;
      if (normal_session_completion) {
        RecordCompletedClientSessionLocked(worker);
      } else if (worker->state != ParserWorkerState::kStopped &&
                 worker->state != ParserWorkerState::kQuarantined) {
        worker->last_diagnostic = "worker process exited";
        worker->state = ParserWorkerState::kQuarantined;
        RecordFaultLocked(worker, "worker_exit_unexpected", worker->last_diagnostic);
      }
      if (worker->state != ParserWorkerState::kQuarantined) worker->state = ParserWorkerState::kStopped;
    }
  }
#else
  if (worker->process_id > 0) {
    int status = 0;
    const auto rc = ::waitpid(worker->process_id, &status, WNOHANG);
    if (rc == worker->process_id) {
      worker->process_id = -1;
      CloseFd(&worker->control_fd);
      const bool normal_session_completion =
          worker->state == ParserWorkerState::kDraining && worker->active_connection_id != 0;
      if (normal_session_completion) {
        RecordCompletedClientSessionLocked(worker);
      } else if (worker->state != ParserWorkerState::kStopped &&
                 worker->state != ParserWorkerState::kQuarantined) {
        worker->last_diagnostic = DescribeExitStatus(status);
        worker->state = ParserWorkerState::kQuarantined;
        RecordFaultLocked(worker, "worker_exit_unexpected", worker->last_diagnostic);
      }
      if (worker->state != ParserWorkerState::kQuarantined) worker->state = ParserWorkerState::kStopped;
    }
  }
#endif
}

void ParserPool::ReapExitedWorkersLocked() {
  for (auto& worker : workers_) {
    ReapWorkerLocked(&worker);
  }
}

void ParserPool::PurgeTerminalWorkersLocked() {
  for (auto& worker : workers_) {
    if (worker.state == ParserWorkerState::kQuarantined) {
      StopWorkerLocked(&worker, true);
    }
  }
  workers_.erase(std::remove_if(workers_.begin(),
                                workers_.end(),
                                [](const ParserWorker& worker) {
                                  return worker.state == ParserWorkerState::kStopped ||
                                         worker.state == ParserWorkerState::kQuarantined;
                                }),
                 workers_.end());
}

void ParserPool::RecordCompletedClientSessionLocked(ParserWorker* worker) {
  if (worker == nullptr || worker->active_client_addr.empty()) return;
  completed_client_sessions_.push_back(worker->active_client_addr);
  worker->active_client_addr.clear();
  worker->active_connection_id = 0;
}

void ParserPool::PruneRecentFailuresLocked(std::uint64_t now_ms) {
  const auto window_ms = static_cast<std::uint64_t>(config_.child_quarantine_window_ms);
  if (window_ms == 0) {
    recent_failure_timestamps_ms_.clear();
    return;
  }
  while (!recent_failure_timestamps_ms_.empty() &&
         recent_failure_timestamps_ms_.front() + window_ms < now_ms) {
    recent_failure_timestamps_ms_.pop_front();
  }
}

std::uint32_t ParserPool::ComputeRestartBackoffMsLocked(std::uint32_t recent_failure_count) const {
  const auto base_ms = std::max<std::uint32_t>(1, config_.child_restart_base_ms);
  const auto max_ms = std::max(base_ms, config_.child_restart_max_ms);
  std::uint64_t delay = base_ms;
  const auto exponent = recent_failure_count == 0 ? 0 : recent_failure_count - 1;
  for (std::uint32_t i = 0; i < exponent && delay < max_ms; ++i) {
    delay = std::min<std::uint64_t>(static_cast<std::uint64_t>(max_ms), delay * 2u);
  }
  return static_cast<std::uint32_t>(std::min<std::uint64_t>(delay, max_ms));
}

void ParserPool::RecordFaultLocked(ParserWorker* worker,
                                   std::string event,
                                   std::string diagnostic,
                                   bool intentional) {
  const auto now_ms = NowMillis();
  PruneRecentFailuresLocked(now_ms);
  if (!intentional) {
    recent_failure_timestamps_ms_.push_back(now_ms);
  }
  const auto recent_count = static_cast<std::uint32_t>(recent_failure_timestamps_ms_.size());
  const auto backoff_ms = intentional ? 0 : ComputeRestartBackoffMsLocked(recent_count);
  if (!intentional) {
    last_backoff_ms_ = backoff_ms;
    next_restart_at_ms_ = now_ms + backoff_ms;
    if (config_.child_quarantine_failures != 0 &&
        recent_count >= config_.child_quarantine_failures) {
      quarantine_until_ms_ = std::max<std::uint64_t>(
          quarantine_until_ms_, now_ms + static_cast<std::uint64_t>(config_.child_quarantine_window_ms));
    }
  }
  ParserPoolFaultEvent fault;
  fault.timestamp_ms = now_ms;
  fault.event = std::move(event);
  fault.diagnostic = std::move(diagnostic);
  fault.backoff_ms = backoff_ms;
  fault.next_retry_at_ms = intentional ? 0 : next_restart_at_ms_;
  fault.quarantine_active = !intentional && quarantine_until_ms_ > now_ms;
  fault.quarantine_until_ms = intentional ? 0 : quarantine_until_ms_;
  fault.recent_failure_count = recent_count;
  fault.intentional = intentional;
  if (worker != nullptr) {
    worker->last_failure_at_ms = now_ms;
    worker->failure_count += intentional ? 0 : 1;
    worker->last_backoff_ms = backoff_ms;
    worker->next_restart_at_ms = fault.next_retry_at_ms;
    worker->quarantine_until_ms = fault.quarantine_until_ms;
    worker->last_diagnostic = fault.diagnostic;
    fault.worker_id = worker->worker_id;
    fault.generation = worker->generation;
    fault.numeric_worker_id = worker->numeric_worker_id;
  }
  fault_history_.push_back(std::move(fault));
  while (fault_history_.size() > kParserPoolFaultHistoryMax) {
    fault_history_.pop_front();
  }
  if (metrics_ && !intentional) {
    metrics_->Increment("sys.metrics.listener.parser_pool.worker_faults_total");
  }
}

std::string WorkerStateName(ParserWorkerState state) {
  switch (state) {
    case ParserWorkerState::kCold: return "cold";
    case ParserWorkerState::kStarting: return "starting";
    case ParserWorkerState::kIdlePreauth: return "idle_preauth";
    case ParserWorkerState::kAssigned: return "assigned";
    case ParserWorkerState::kDraining: return "draining";
    case ParserWorkerState::kStopped: return "stopped";
    case ParserWorkerState::kQuarantined: return "quarantined";
  }
  return "unknown";
}

std::string WorkerStateClassName(ParserWorkerState state) {
  switch (state) {
    case ParserWorkerState::kCold:
    case ParserWorkerState::kStarting:
    case ParserWorkerState::kIdlePreauth:
      return "IDLE";
    case ParserWorkerState::kAssigned:
      return "BUSY";
    case ParserWorkerState::kDraining:
      return "DRAINING";
    case ParserWorkerState::kStopped:
    case ParserWorkerState::kQuarantined:
      return "FAULT";
  }
  return "FAULT";
}

} // namespace scratchbird::listener
