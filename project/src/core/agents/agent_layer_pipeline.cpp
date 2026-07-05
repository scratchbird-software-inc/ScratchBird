// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "agent_layer_pipeline.hpp"

#include <algorithm>
#include <utility>

namespace scratchbird::core::agents {

namespace {

bool ContainsLayer(const std::vector<AgentRuntimeLayer>& layers,
                   AgentRuntimeLayer layer) {
  return std::find(layers.begin(), layers.end(), layer) != layers.end();
}

bool ContainsTable(const std::vector<std::string>& table_ids,
                   const std::string& table_id) {
  return std::find(table_ids.begin(), table_ids.end(), table_id) != table_ids.end();
}

}  // namespace

bool AgentLayer5MaySchedule(AgentRuntimeLayer layer,
                            const AgentLayerCoordinatorPolicy& policy) {
  if (layer == AgentRuntimeLayer::l4_worker && policy.worker_launch_forbidden) {
    return false;
  }
  return ContainsLayer(policy.managed_layers, layer);
}

AgentRuntimeStatus PushLayer1Observation(const AgentTypeDescriptor& descriptor,
                                         AgentObservationRecord record,
                                         AgentLayerPipelineState* state) {
  if (state == nullptr) {
    return AgentError("SB_AGENT_LAYER.PIPELINE_STATE_REQUIRED");
  }
  if (descriptor.layer != AgentRuntimeLayer::l1_observation) {
    return AgentError("SB_AGENT_LAYER.L1_REQUIRED", descriptor.type_id);
  }
  if (descriptor.authority != AgentAuthorityClass::observe_only) {
    return AgentError("SB_AGENT_LAYER.L1_OBSERVE_ONLY_REQUIRED",
                      descriptor.type_id);
  }
  if (!record.below_mga_observation) {
    return AgentError("SB_AGENT_LAYER.L1_BELOW_MGA_REQUIRED",
                      descriptor.type_id);
  }
  record.producer_agent_type_id = descriptor.type_id;
  if (state->observation_queue.records.size() >=
      state->observation_queue.capacity) {
    ++state->observation_queue.dropped_records;
    return {true, "SB_AGENT_LAYER.L1_OBSERVATION_DROPPED", "queue full"};
  }
  state->observation_queue.records.push_back(std::move(record));
  return AgentOk();
}

AgentRuntimeStatus DrainLayer2Observations(const AgentTypeDescriptor& descriptor,
                                           const std::string& findings_table_id,
                                           const std::string& commit_transaction_uuid,
                                           std::size_t max_records,
                                           AgentLayerPipelineState* state) {
  if (state == nullptr) {
    return AgentError("SB_AGENT_LAYER.PIPELINE_STATE_REQUIRED");
  }
  if (descriptor.layer != AgentRuntimeLayer::l2_recorder) {
    return AgentError("SB_AGENT_LAYER.L2_REQUIRED", descriptor.type_id);
  }
  if (findings_table_id.empty() || commit_transaction_uuid.empty()) {
    return AgentError("SB_AGENT_LAYER.L2_COMMIT_CONTEXT_REQUIRED",
                      descriptor.type_id);
  }
  if (max_records == 0) { return AgentOk(); }

  std::size_t drained = 0;
  while (!state->observation_queue.records.empty() && drained < max_records) {
    auto record = std::move(state->observation_queue.records.front());
    state->observation_queue.records.pop_front();

    AgentCommittedFinding finding;
    finding.findings_table_id = findings_table_id;
    finding.recorder_agent_type_id = descriptor.type_id;
    finding.source_agent_type_id = std::move(record.producer_agent_type_id);
    finding.entity_type = std::move(record.entity_type);
    finding.entity_uuid = std::move(record.entity_uuid);
    finding.record_type = std::move(record.record_type);
    finding.payload = std::move(record.payload);
    finding.commit_transaction_uuid = commit_transaction_uuid;
    finding.observed_monotonic_timestamp_microseconds =
        record.monotonic_timestamp_microseconds;
    finding.mga_committed = true;
    state->findings.push_back(std::move(finding));
    ++drained;
  }
  state->evidence.push_back("l2_drained:" + descriptor.type_id + ":" +
                            findings_table_id + ":" + std::to_string(drained));
  return AgentOk();
}

AgentLayer3DispatchDecision DispatchLayer3FromFindings(
    const AgentTypeDescriptor& dispatcher,
    const AgentTypeDescriptor& worker,
    const AgentLayer3DispatchRequest& request,
    AgentLayerPipelineState* state) {
  AgentLayer3DispatchDecision decision;
  if (state == nullptr) {
    decision.status = AgentError("SB_AGENT_LAYER.PIPELINE_STATE_REQUIRED");
    return decision;
  }
  if (dispatcher.layer != AgentRuntimeLayer::l3_dispatcher) {
    decision.status = AgentError("SB_AGENT_LAYER.L3_REQUIRED",
                                 dispatcher.type_id);
    return decision;
  }
  if (worker.layer != AgentRuntimeLayer::l4_worker) {
    decision.status = AgentError("SB_AGENT_LAYER.L4_WORKER_REQUIRED",
                                 worker.type_id);
    return decision;
  }
  if (request.attempted_raw_queue_read) {
    decision.status = AgentError("SB_AGENT_LAYER.L3_RAW_QUEUE_FORBIDDEN",
                                 dispatcher.type_id);
    decision.diagnostics.push_back(decision.status.diagnostic_code);
    return decision;
  }

  for (const auto& finding : state->findings) {
    if (!finding.mga_committed) { continue; }
    if (!ContainsTable(request.findings_table_ids, finding.findings_table_id)) {
      continue;
    }
    ++decision.findings_read;
  }
  if (decision.findings_read > 0 && request.launch_worker_on_finding) {
    state->launched_workers.push_back(worker.type_id);
    decision.launched_worker_type_ids.push_back(worker.type_id);
    state->evidence.push_back("l3_launched:" + dispatcher.type_id + ":" +
                              worker.type_id + ":" + request.dispatch_reason);
  }
  decision.status = AgentOk();
  return decision;
}

AgentLayer5ScheduleDecision RunLayer5CoordinatorCycle(
    const AgentTypeDescriptor& coordinator,
    const AgentLayerCoordinatorPolicy& policy,
    const std::vector<AgentTypeDescriptor>& candidate_agents,
    AgentLayerPipelineState* state) {
  AgentLayer5ScheduleDecision decision;
  if (state == nullptr) {
    decision.status = AgentError("SB_AGENT_LAYER.PIPELINE_STATE_REQUIRED");
    return decision;
  }
  if (coordinator.layer != AgentRuntimeLayer::l5_coordinator) {
    decision.status = AgentError("SB_AGENT_LAYER.L5_REQUIRED",
                                 coordinator.type_id);
    return decision;
  }
  if (!policy.startup_launch_enabled || !policy.go_no_go) {
    for (const auto& candidate : candidate_agents) {
      decision.refused_agent_type_ids.push_back(candidate.type_id);
      state->refused_agents.push_back(candidate.type_id);
    }
    decision.status = AgentError("SB_AGENT_LAYER.L5_GO_NO_GO_REFUSED",
                                 coordinator.type_id);
    decision.evidence.push_back("l5_refused:go_no_go");
    state->evidence.push_back("l5_refused:go_no_go");
    return decision;
  }

  std::size_t scheduled = 0;
  for (const auto& candidate : candidate_agents) {
    const bool may_schedule = AgentLayer5MaySchedule(candidate.layer, policy);
    if (!may_schedule || scheduled >= policy.max_agents_per_cycle) {
      decision.refused_agent_type_ids.push_back(candidate.type_id);
      state->refused_agents.push_back(candidate.type_id);
      continue;
    }
    decision.scheduled_agent_type_ids.push_back(candidate.type_id);
    state->scheduled_agents.push_back(candidate.type_id);
    ++scheduled;
  }
  decision.evidence.push_back("l5_scheduled:" + std::to_string(scheduled));
  state->evidence.push_back("l5_scheduled:" + std::to_string(scheduled));
  decision.status = AgentOk();
  return decision;
}

}  // namespace scratchbird::core::agents
