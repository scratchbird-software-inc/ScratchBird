// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <atomic>
#include <chrono>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "listener_config.hpp"
#include "listener_metrics.hpp"
#include "listener_socket_identity.hpp"
#include "listener_support_bundle.hpp"
#include "parser_pool.hpp"

namespace scratchbird::listener {

struct ListenerManagementEnvelope;

#ifdef _WIN32
using ListenerRuntimeSocketHandle = std::intptr_t;
#else
using ListenerRuntimeSocketHandle = int;
#endif

enum class AcceptLoopStage : std::uint32_t {
  kIdle = 0,
  kAccepted = 1,
  kMetricsRecorded = 2,
  kAcquireBegin = 3,
  kWorkerAcquired = 4,
  kHandoffBegin = 5,
  kHandoffComplete = 6,
  kClientClosed = 7,
};

struct ListenerRuntimeResult {
  int exit_code{0};
  proto::MessageVectorSet messages;
  std::string response_json;
};

class ListenerRuntime {
 public:
  explicit ListenerRuntime(ListenerConfig config);
  ListenerRuntimeResult Run();
  ListenerRuntimeResult HandleManagementCommand(const std::string& command_json);
  void RequestStop();
  void QueuePendingHandoffBindingForTest(const proto::DbbtToken& token,
                                         const proto::Lpreface& preface);
  ParserHandoffBinding TakePendingHandoffBindingForTest(
      const ParserHandoffClientEvidence& evidence);
  std::size_t PendingHandoffBindingCountForTest() const;

 private:
  ListenerRuntimeResult PrepareRuntimeFiles();
  ListenerRuntimeResult BindManagementSocket(ListenerRuntimeSocketHandle* out_fd);
  ListenerRuntimeResult BindNetworkSocket(ListenerRuntimeSocketHandle* out_fd);
  void AcceptLoop(ListenerRuntimeSocketHandle listen_fd);
  void ManagementLoop(ListenerRuntimeSocketHandle management_fd);
  ListenerRuntimeResult HandleManagementPayload(const std::vector<std::uint8_t>& payload,
                                                ListenerRuntimeSocketHandle peer_fd);
  ListenerRuntimeResult HandleManagementEnvelope(const ListenerManagementEnvelope& envelope,
                                                 ListenerRuntimeSocketHandle peer_fd,
                                                 const std::vector<std::uint8_t>& encoded_payload);
  ListenerRuntimeResult ExecuteManagementOperation(const ListenerManagementEnvelope& envelope);
  ListenerRuntimeResult ExecuteManagementCommandText(const std::string& command_json);
  void WriteLifecycleState(const std::string& state);
  std::string StatusJson() const;
  std::string BuildSupportBundleJson() const;
  void RecordSupportEvent(std::string event_type,
                          std::string operation,
                          std::string outcome,
                          std::string diagnostic_code,
                          std::string safe_detail);
  ParserHandoffBinding TakePendingHandoffBinding(const ParserHandoffClientEvidence& evidence);
  void QueuePendingHandoffBinding(const proto::DbbtToken& token,
                                  const proto::Lpreface& preface);
  std::size_t PendingHandoffBindingCount() const;

  ListenerConfig config_;
  ListenerMetrics metrics_;
  ListenerSocketIdentity identity_;
  ParserPool parser_pool_;
  proto::DbbtReplayCache dbbt_replay_cache_{4096};
  proto::DbbtReplayCache management_replay_cache_{4096};
  mutable std::mutex pending_handoff_mutex_;
  std::deque<ParserHandoffBinding> pending_handoff_bindings_;
  mutable std::mutex support_event_mutex_;
  std::deque<ListenerSupportBundleEvent> management_decisions_;
  std::deque<ListenerSupportBundleEvent> runtime_events_;
  std::atomic<bool> stop_requested_{false};
  std::atomic<bool> draining_{false};
  std::atomic<std::uint64_t> next_connection_id_{1};
  std::atomic<std::uint64_t> last_accept_sequence_{0};
  std::atomic<std::uint32_t> last_accept_stage_{static_cast<std::uint32_t>(AcceptLoopStage::kIdle)};
  std::atomic<std::uint64_t> open_connections_{0};
  std::atomic<std::uint64_t> queue_depth_{0};
  std::atomic<std::uint64_t> handoff_complete_total_{0};
  std::atomic<std::uint64_t> reject_total_{0};
  std::map<std::string, std::uint32_t> per_client_active_;
  double accept_rate_tokens_{0.0};
  std::chrono::steady_clock::time_point accept_rate_last_refill_{};
  std::string lifecycle_state_{"created"};
};

ListenerRuntimeResult RunListenerFromArgs(int argc, char** argv);

} // namespace scratchbird::listener
