// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "agent_runtime.hpp"

#include <string>
#include <vector>

namespace scratchbird::core::agents::implemented_agents {

using scratchbird::core::platform::DiagnosticRecord;
using scratchbird::core::platform::Status;
using scratchbird::core::platform::u32;
using scratchbird::core::platform::u64;

enum class NodeResourceAgentDecisionKind : u32 {
  publish_capability,
  publish_role_suitability,
  refused
};

struct NodeResourceAgentPolicy {
  bool present = true;
  bool valid = true;
  bool scope_compatible = true;
  bool publish_node_capability_allowed = true;
  bool publish_role_suitability_allowed = true;
  u64 max_scheduler_queue_depth_for_primary = 128;
  u64 high_memory_pressure_percent = 90;
};

struct NodeResourceAgentSnapshot {
  u64 cpu_count = 0;
  u64 total_memory_bytes = 0;
  u64 available_memory_bytes = 0;
  u64 page_size_bytes = 0;
  u64 scheduler_queue_depth = 0;
  u64 memory_pressure_percent = 0;
  bool os_probe_authoritative = false;
  bool metric_registry_authoritative = false;
  bool cpu_feature_probe_present = true;
  bool page_size_supported = true;
  bool resource_governance_enabled = true;
  bool cluster_metric_route_requested = false;
  bool parser_authority = false;
  bool client_authority = false;
  bool donor_authority = false;
};

struct NodeResourceAgentEvidenceField {
  std::string key;
  std::string value;
};

struct NodeResourceAgentResult {
  Status status;
  DiagnosticRecord diagnostic;
  NodeResourceAgentDecisionKind decision =
      NodeResourceAgentDecisionKind::refused;
  std::vector<NodeResourceAgentEvidenceField> evidence;
  bool fail_closed = true;
  bool publish_node_capability = false;
  bool publish_role_suitability = false;
  u64 role_suitability_score = 0;

  bool ok() const { return status.ok() && !fail_closed; }
};

const char* NodeResourceAgentDecisionKindName(
    NodeResourceAgentDecisionKind decision);
NodeResourceAgentResult EvaluateNodeResourceAgentSnapshot(
    const NodeResourceAgentSnapshot& snapshot,
    const NodeResourceAgentPolicy& policy = {});
DiagnosticRecord MakeNodeResourceAgentDiagnostic(Status status,
                                                 std::string diagnostic_code,
                                                 std::string message_key,
                                                 std::string detail = {});

const char* node_resource_agent_implementation_anchor();

}  // namespace scratchbird::core::agents::implemented_agents
