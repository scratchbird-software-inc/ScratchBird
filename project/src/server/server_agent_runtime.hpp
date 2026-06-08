// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SEARCH_KEY: SB_SERVER_AGENT_THREAD_RUNTIME

#pragma once

#include "config.hpp"
#include "diagnostics.hpp"
#include "engine_host.hpp"
#include "agents/agent_runtime_service_store_api.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace scratchbird::server {

struct ServerAgentRuntimeSnapshot {
  bool started = false;
  bool stopping = false;
  std::string database_path;
  std::string database_uuid;
  std::string filespace_uuid;
  std::filesystem::path status_path;
  std::uint32_t hardware_concurrency = 0;
  std::uint32_t effective_cpu_count = 0;
  std::uint32_t foreground_reserved_capacity = 0;
  std::uint32_t background_worker_slots = 0;
  std::uint32_t worker_thread_count = 0;
  std::string worker_wake_policy;
  std::uint64_t scheduler_ticks = 0;
  std::uint64_t total_worker_ticks = 0;
  std::uint64_t total_actions_accepted = 0;
  std::uint64_t total_actions_refused = 0;
  std::uint64_t durable_catalog_generation = 0;
  std::uint64_t durable_lease_count = 0;
  std::uint64_t durable_replay_pending_lease_count = 0;
  std::uint64_t durable_action_backlog_count = 0;
  std::uint64_t durable_replay_pending_action_count = 0;
  std::uint64_t durable_service_evidence_count = 0;
  std::uint64_t last_recovery_replayed_count = 0;
  std::string durable_catalog_root_digest;
};

class ServerAgentRuntime {
 public:
  ServerAgentRuntime() = default;
  ServerAgentRuntime(const ServerAgentRuntime&) = delete;
  ServerAgentRuntime& operator=(const ServerAgentRuntime&) = delete;
  ~ServerAgentRuntime();

  bool Start(const ServerBootstrapConfig& config,
             const HostedEngineState& engine_state,
             std::vector<ServerDiagnostic>* diagnostics);
  void Stop();
  ServerAgentRuntimeSnapshot Snapshot() const;

 private:
  struct WorkerEvidence {
    std::string name;
    std::string role;
    std::string agent_type_id;
    std::string instance_uuid;
    std::string lease_uuid;
    std::string lease_owner_uuid;
    std::string native_thread_id;
    bool durable_lease_acquired = false;
    std::uint64_t last_lease_heartbeat_generation = 0;
    std::uint64_t ticks = 0;
    std::uint64_t actions_accepted = 0;
    std::uint64_t actions_refused = 0;
    std::string last_action;
    std::string last_diagnostic_code;
  };

  void SchedulerLoop();
  void WorkerLoop(std::size_t worker_index);
  void RunWorkerTick(std::size_t worker_index, std::uint64_t generation);
  void RecordWorkerTick(std::size_t worker_index,
                        std::string last_action,
                        bool action_attempted,
                        bool action_accepted,
                        std::string diagnostic_code);
  void UpdateRuntimeCatalogSnapshotLocked(
      const scratchbird::core::agents::DurableAgentCatalogImage& catalog);
  bool IsPrimaryWorkerForAgent(std::size_t worker_index,
                               const std::string& agent_type_id) const;
  void WriteStatusSnapshot() const;
  std::string StatusJson() const;

  mutable std::mutex state_mutex_;
  mutable std::mutex file_mutex_;
  std::mutex schedule_mutex_;
  std::condition_variable schedule_cv_;
  bool started_ = false;
  std::atomic_bool stopping_{false};
  std::string database_path_;
  std::string database_uuid_;
  std::string filespace_uuid_;
  std::filesystem::path status_path_;
  std::uint32_t hardware_concurrency_ = 0;
  std::uint32_t effective_cpu_count_ = 0;
  std::uint32_t foreground_reserved_capacity_ = 0;
  std::uint32_t background_worker_slots_ = 0;
  std::string worker_wake_policy_;
  std::uint64_t scheduled_generation_ = 0;
  std::uint64_t scheduler_ticks_ = 0;
  std::vector<std::uint64_t> worker_completed_generations_;
  std::string scheduler_thread_id_;
  std::uint64_t catalog_generation_id_ = 1;
  std::uint64_t security_epoch_ = 1;
  std::uint64_t resource_epoch_ = 1;
  std::uint64_t name_resolution_epoch_ = 1;
  std::vector<std::string> selected_agents_;
  std::vector<WorkerEvidence> worker_evidence_;
  mutable std::mutex database_transaction_mutex_;
  mutable std::mutex runtime_service_mutex_;
  scratchbird::engine::internal_api::AgentRuntimeServiceStore runtime_service_;
  std::uint64_t durable_catalog_generation_ = 0;
  std::uint64_t durable_lease_count_ = 0;
  std::uint64_t durable_replay_pending_lease_count_ = 0;
  std::uint64_t durable_action_backlog_count_ = 0;
  std::uint64_t durable_replay_pending_action_count_ = 0;
  std::uint64_t durable_service_evidence_count_ = 0;
  std::uint64_t last_recovery_replayed_count_ = 0;
  std::string durable_catalog_root_digest_;
  std::thread scheduler_thread_;
  std::vector<std::thread> worker_threads_;
};

}  // namespace scratchbird::server
