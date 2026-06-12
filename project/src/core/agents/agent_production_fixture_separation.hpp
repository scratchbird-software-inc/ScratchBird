// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SEARCH_KEY: ARHC_PRODUCTION_PROBE_FIXTURE_SEPARATION
// Production agent paths must not consume fixture policy, relaxed metric,
// probe-only catalog, sidecar-only evidence, or synthetic live-management
// state. These checks are admission/evidence controls only; they are not
// transaction finality, visibility, recovery, parser, reference, or client
// authority.

#include "agent_runtime.hpp"

#include <string>
#include <vector>

namespace scratchbird::core::agents {

struct AgentProductionFixtureSeparationInput {
  bool production_build = false;
  bool production_live_path = true;
  bool fixture_auth = false;
  bool test_fixture_mode = false;
  bool fixture_policy = false;
  bool relaxed_metric_path = false;
  bool observed_metric_snapshot_required = true;
  bool test_seed_material = false;
  bool forced_collision_hooks = false;
  bool probe_only_catalog = false;
  bool durable_runtime_catalog = true;
  bool sidecar_only_evidence = false;
  bool durable_evidence_store = true;
  bool simulated_actuator_provider = false;
  bool debug_only_paths_enabled = false;
  bool synthetic_live_management_state = false;
  bool management_state_durable = true;
  bool live_agent_surface = false;
  bool cluster_stub_live_claim = false;
};

struct AgentProductionFixtureSeparationResult {
  AgentRuntimeStatus status;
  bool ok = false;
  bool fail_closed = true;
  std::vector<std::string> evidence;
};

AgentProductionFixtureSeparationResult ValidateAgentProductionFixtureSeparation(
    const AgentProductionFixtureSeparationInput& input);

}  // namespace scratchbird::core::agents
