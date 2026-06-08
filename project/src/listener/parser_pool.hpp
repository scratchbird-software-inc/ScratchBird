// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <optional>
#include <vector>

#include "control_plane.hpp"
#include "listener_config.hpp"
#include "listener_metrics.hpp"

namespace scratchbird::listener {

#ifdef _WIN32
using ParserControlHandle = std::intptr_t;
#else
using ParserControlHandle = int;
#endif

enum class ParserWorkerState {
  kCold,
  kStarting,
  kIdlePreauth,
  kAssigned,
  kDraining,
  kStopped,
  kQuarantined,
};

struct ParserWorker {
  std::string worker_id;
  ParserWorkerState state{ParserWorkerState::kCold};
  std::uint64_t generation{0};
  std::uint64_t numeric_worker_id{0};
  std::uint64_t served_connections{0};
  std::uint64_t restart_count{0};
  std::string last_diagnostic;
  std::uint64_t last_started_at_ms{0};
  std::uint64_t last_failure_at_ms{0};
  std::uint64_t next_restart_at_ms{0};
  std::uint64_t quarantine_until_ms{0};
  std::uint32_t failure_count{0};
  std::uint32_t last_backoff_ms{0};
  bool hello_accepted{false};
  bool awaiting_ack{false};
  std::uint64_t awaiting_request{0};
  std::string active_client_addr;
  std::uint64_t active_connection_id{0};
#ifndef _WIN32
  int process_id{-1};
#else
  std::uint32_t process_id{0};
  std::intptr_t process_handle{0};
  std::string control_socket_path;
#endif
  ParserControlHandle control_fd{-1};
};

struct ParserPoolFaultEvent {
  std::uint64_t timestamp_ms{0};
  std::string worker_id;
  std::uint64_t generation{0};
  std::uint64_t numeric_worker_id{0};
  std::string event;
  std::string diagnostic;
  std::uint32_t backoff_ms{0};
  std::uint64_t next_retry_at_ms{0};
  bool quarantine_active{false};
  std::uint64_t quarantine_until_ms{0};
  std::uint32_t recent_failure_count{0};
  bool intentional{false};
};

struct ParserPoolStatus {
  bool running{false};
  bool draining{false};
  std::uint32_t target_min{0};
  std::uint32_t target_max{0};
  std::uint32_t active_worker_count{0};
  std::uint32_t busy_worker_count{0};
  std::uint32_t running_worker_count{0};
  std::uint32_t recent_failure_count{0};
  std::uint32_t last_backoff_ms{0};
  std::uint64_t next_restart_at_ms{0};
  bool quarantine_active{false};
  std::uint64_t quarantine_until_ms{0};
  std::vector<ParserWorker> workers;
  std::vector<ParserPoolFaultEvent> fault_history;
};

struct ParserPoolDrainResult {
  bool drained{false};
  bool timed_out{false};
  std::uint32_t active_worker_count{0};
  std::uint32_t busy_worker_count{0};
  std::uint32_t running_worker_count{0};
  std::uint32_t timeout_ms{0};
  std::uint32_t waited_ms{0};
  proto::MessageVectorSet messages;
};

struct ParserHandoffResult {
  bool ok{false};
  std::string reason;
  proto::MessageVectorSet messages;
};

struct ParserHandoffBinding {
  bool present{false};
  std::array<std::uint8_t, 16> db_uuid{};
  std::array<std::uint8_t, 16> dbbt_id{};
  std::array<std::uint8_t, 16> manager_session_id{};
  proto::Bytes client_nonce;
  proto::Bytes server_nonce;
  bool has_expected_client_endpoint{false};
  std::string expected_client_addr;
  std::uint16_t expected_client_port{0};
  std::uint32_t listener_id{1};
  std::uint64_t expires_at_ms{0};
  std::string auth_provider_family;
  std::string auth_principal;
  std::string auth_token;
};

struct ParserHandoffClientEvidence {
  proto::Bytes client_nonce;
  proto::Bytes server_nonce;
  bool has_client_endpoint{false};
  std::string client_addr;
  std::uint16_t client_port{0};
};

class ParserPool {
 public:
  explicit ParserPool(ListenerConfig config, ListenerMetrics* metrics);

  proto::MessageVectorSet Start();
  proto::MessageVectorSet Stop(bool force);
  proto::MessageVectorSet Drain();
  ParserPoolDrainResult DrainAndWait(std::uint32_t timeout_ms);
  proto::MessageVectorSet Undrain();
  proto::MessageVectorSet Resize(std::uint32_t min_workers, std::uint32_t max_workers);
  proto::MessageVectorSet RestartWorker(const std::string& worker_id);
  proto::MessageVectorSet KillWorker(const std::string& worker_id);
  proto::MessageVectorSet KillConnection(std::uint64_t connection_id);
  ParserHandoffResult HandoffClient(std::intptr_t client_fd,
                                    const std::string& client_addr,
                                    std::uint16_t client_port,
                                    std::uint64_t connection_id,
                                    const ParserHandoffBinding& binding);
  std::vector<std::string> CollectCompletedClientSessions();
  ParserPoolStatus Status() const;
  std::string StatusJson() const;

 private:
  ParserWorker MakeWorker(std::uint64_t ordinal) const;
  void EnsureWarmWorkersLocked();
  ParserWorker* FindWorkerLocked(const std::string& worker_id);
  ParserWorker* FindWorkerByConnectionLocked(std::uint64_t connection_id);
  ParserWorker* FindIdleWorkerLocked();
  std::size_t ActiveWorkerCountLocked() const;
  std::size_t BusyWorkerCountLocked() const;
  std::size_t RunningWorkerCountLocked() const;
  bool RetryBlockedLocked(std::uint64_t now_ms, std::string* reason) const;
  ParserWorker* SpawnWorkerLocked(std::uint64_t now_ms);
  bool LaunchWorkerLocked(ParserWorker* worker, std::uint64_t now_ms);
  bool AdmitWorkerLocked(ParserWorker* worker);
  void StopWorkerLocked(ParserWorker* worker, bool force);
  void ReapWorkerLocked(ParserWorker* worker);
  void ReapExitedWorkersLocked();
  void PurgeTerminalWorkersLocked();
  void RecordCompletedClientSessionLocked(ParserWorker* worker);
  void PruneRecentFailuresLocked(std::uint64_t now_ms);
  std::uint32_t ComputeRestartBackoffMsLocked(std::uint32_t recent_failure_count) const;
  void RecordFaultLocked(ParserWorker* worker,
                         std::string event,
                         std::string diagnostic,
                         bool intentional = false);

  ListenerConfig config_;
  ListenerMetrics* metrics_;
  mutable std::mutex mutex_;
  bool running_{false};
  bool draining_{false};
  std::uint64_t next_worker_ordinal_{1};
  std::vector<ParserWorker> workers_;
  std::vector<std::string> completed_client_sessions_;
  std::deque<std::uint64_t> recent_failure_timestamps_ms_;
  std::deque<ParserPoolFaultEvent> fault_history_;
  std::uint64_t next_restart_at_ms_{0};
  std::uint64_t quarantine_until_ms_{0};
  std::uint32_t last_backoff_ms_{0};
};

std::string WorkerStateName(ParserWorkerState state);
std::string WorkerStateClassName(ParserWorkerState state);

} // namespace scratchbird::listener
