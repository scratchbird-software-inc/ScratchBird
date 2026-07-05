// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "agent_runtime.hpp"

#include <cstddef>
#include <deque>
#include <string>
#include <vector>

namespace scratchbird::core::agents {

struct AgentObservationRecord {
  std::string producer_agent_type_id;
  std::string entity_type;
  std::string entity_uuid;
  std::string record_type;
  std::string payload;
  u64 monotonic_timestamp_microseconds = 0;
  bool below_mga_observation = true;
};

struct AgentObservationQueue {
  std::size_t capacity = 1024;
  std::deque<AgentObservationRecord> records;
  std::size_t dropped_records = 0;
};

struct AgentCommittedFinding {
  std::string findings_table_id;
  std::string recorder_agent_type_id;
  std::string source_agent_type_id;
  std::string entity_type;
  std::string entity_uuid;
  std::string record_type;
  std::string payload;
  std::string commit_transaction_uuid;
  u64 observed_monotonic_timestamp_microseconds = 0;
  bool mga_committed = false;
};

struct AgentLayerPipelineState {
  AgentObservationQueue observation_queue;
  std::vector<AgentCommittedFinding> findings;
  std::vector<std::string> scheduled_agents;
  std::vector<std::string> refused_agents;
  std::vector<std::string> launched_workers;
  std::vector<std::string> evidence;
};

struct AgentLayerCoordinatorPolicy {
  bool startup_launch_enabled = true;
  bool go_no_go = true;
  bool worker_launch_forbidden = true;
  std::size_t max_agents_per_cycle = 128;
  std::vector<AgentRuntimeLayer> managed_layers = {
      AgentRuntimeLayer::l1_observation,
      AgentRuntimeLayer::l2_recorder,
      AgentRuntimeLayer::l3_dispatcher};
};

struct AgentLayer3DispatchRequest {
  std::vector<std::string> findings_table_ids;
  std::string target_worker_type_id;
  std::string dispatch_reason;
  bool launch_worker_on_finding = true;
  bool attempted_raw_queue_read = false;
};

struct AgentLayer3DispatchDecision {
  AgentRuntimeStatus status;
  std::size_t findings_read = 0;
  std::vector<std::string> launched_worker_type_ids;
  std::vector<std::string> diagnostics;
};

struct AgentLayer5ScheduleDecision {
  AgentRuntimeStatus status;
  std::vector<std::string> scheduled_agent_type_ids;
  std::vector<std::string> refused_agent_type_ids;
  std::vector<std::string> evidence;
};

bool AgentLayer5MaySchedule(AgentRuntimeLayer layer,
                            const AgentLayerCoordinatorPolicy& policy);

AgentRuntimeStatus PushLayer1Observation(const AgentTypeDescriptor& descriptor,
                                         AgentObservationRecord record,
                                         AgentLayerPipelineState* state);

AgentRuntimeStatus DrainLayer2Observations(const AgentTypeDescriptor& descriptor,
                                           const std::string& findings_table_id,
                                           const std::string& commit_transaction_uuid,
                                           std::size_t max_records,
                                           AgentLayerPipelineState* state);

AgentLayer3DispatchDecision DispatchLayer3FromFindings(
    const AgentTypeDescriptor& dispatcher,
    const AgentTypeDescriptor& worker,
    const AgentLayer3DispatchRequest& request,
    AgentLayerPipelineState* state);

AgentLayer5ScheduleDecision RunLayer5CoordinatorCycle(
    const AgentTypeDescriptor& coordinator,
    const AgentLayerCoordinatorPolicy& policy,
    const std::vector<AgentTypeDescriptor>& candidate_agents,
    AgentLayerPipelineState* state);

}  // namespace scratchbird::core::agents
