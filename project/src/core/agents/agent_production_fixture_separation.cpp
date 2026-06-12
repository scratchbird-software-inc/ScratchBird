// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "agent_production_fixture_separation.hpp"

#include <utility>

namespace scratchbird::core::agents {
namespace {

void Add(std::vector<std::string>* evidence, std::string value) {
  evidence->push_back(std::move(value));
}

std::string BoolText(bool value) {
  return value ? "true" : "false";
}

AgentProductionFixtureSeparationResult Finish(
    const AgentProductionFixtureSeparationInput& input,
    bool ok,
    std::string code,
    std::string detail) {
  AgentProductionFixtureSeparationResult result;
  result.ok = ok;
  result.fail_closed = !ok;
  result.status = {ok, std::move(code), std::move(detail)};
  Add(&result.evidence, "ARHC_PRODUCTION_PROBE_FIXTURE_SEPARATION");
  Add(&result.evidence,
      "production_build=" + BoolText(input.production_build));
  Add(&result.evidence,
      "production_live_path=" + BoolText(input.production_live_path));
  Add(&result.evidence, "fixture_auth=" + BoolText(input.fixture_auth));
  Add(&result.evidence, "test_fixture_mode=" + BoolText(input.test_fixture_mode));
  Add(&result.evidence, "fixture_policy=" + BoolText(input.fixture_policy));
  Add(&result.evidence,
      "relaxed_metric_path=" + BoolText(input.relaxed_metric_path));
  Add(&result.evidence,
      "observed_metric_snapshot_required=" +
          BoolText(input.observed_metric_snapshot_required));
  Add(&result.evidence,
      "test_seed_material=" + BoolText(input.test_seed_material));
  Add(&result.evidence,
      "forced_collision_hooks=" + BoolText(input.forced_collision_hooks));
  Add(&result.evidence,
      "probe_only_catalog=" + BoolText(input.probe_only_catalog));
  Add(&result.evidence,
      "durable_runtime_catalog=" + BoolText(input.durable_runtime_catalog));
  Add(&result.evidence,
      "sidecar_only_evidence=" + BoolText(input.sidecar_only_evidence));
  Add(&result.evidence,
      "durable_evidence_store=" + BoolText(input.durable_evidence_store));
  Add(&result.evidence,
      "simulated_actuator_provider=" +
          BoolText(input.simulated_actuator_provider));
  Add(&result.evidence,
      "debug_only_paths_enabled=" + BoolText(input.debug_only_paths_enabled));
  Add(&result.evidence,
      "synthetic_live_management_state=" +
          BoolText(input.synthetic_live_management_state));
  Add(&result.evidence,
      "management_state_durable=" + BoolText(input.management_state_durable));
  Add(&result.evidence,
      "live_agent_surface=" + BoolText(input.live_agent_surface));
  Add(&result.evidence,
      "cluster_stub_live_claim=" + BoolText(input.cluster_stub_live_claim));
  Add(&result.evidence,
      "agent_fixture_separation.diagnostic_code=" +
          result.status.diagnostic_code);
  Add(&result.evidence,
      "agent_fixture_separation.transaction_finality_authority=false");
  Add(&result.evidence,
      "agent_fixture_separation.visibility_authority=false");
  Add(&result.evidence,
      "agent_fixture_separation.recovery_authority=false");
  Add(&result.evidence,
      "agent_fixture_separation.security_authority=false");
  Add(&result.evidence,
      "agent_fixture_separation.parser_authority=false");
  Add(&result.evidence,
      "agent_fixture_separation.reference_authority=false");
  Add(&result.evidence,
      "agent_fixture_separation.client_authority=false");
  return result;
}

}  // namespace

AgentProductionFixtureSeparationResult ValidateAgentProductionFixtureSeparation(
    const AgentProductionFixtureSeparationInput& input) {
  const bool strict_production_path =
      input.production_live_path || input.production_build;
  if (!strict_production_path) {
    return Finish(input, true,
                  "SB_AGENT_PRODUCTION_FIXTURE.TEST_BOOTSTRAP_ACCEPTED",
                  "non_production_test_or_probe_path");
  }
  if (input.fixture_auth) {
    return Finish(input, false,
                  "SB_AGENT_PRODUCTION_FIXTURE.FIXTURE_AUTH_REFUSED",
                  "fixture authentication cannot back production agents");
  }
  if (input.test_fixture_mode) {
    return Finish(input, false,
                  "SB_AGENT_PRODUCTION_FIXTURE.TEST_FIXTURE_MODE_REFUSED",
                  "test fixture mode cannot back a production live path");
  }
  if (input.fixture_policy) {
    return Finish(input, false,
                  "SB_AGENT_PRODUCTION_FIXTURE.FIXTURE_POLICY_REFUSED",
                  "fixture policies are test-only");
  }
  if (input.relaxed_metric_path || !input.observed_metric_snapshot_required) {
    return Finish(input, false,
                  "SB_AGENT_PRODUCTION_FIXTURE.RELAXED_METRIC_REFUSED",
                  "relaxed registry-only metric paths are test-only");
  }
  if (input.test_seed_material) {
    return Finish(input, false,
                  "SB_AGENT_PRODUCTION_FIXTURE.TEST_SEED_REFUSED",
                  "test seed material is not production authority");
  }
  if (input.forced_collision_hooks) {
    return Finish(input, false,
                  "SB_AGENT_PRODUCTION_FIXTURE.FORCED_HOOK_REFUSED",
                  "forced collision/test hooks are not production authority");
  }
  if (input.probe_only_catalog || !input.durable_runtime_catalog) {
    return Finish(input, false,
                  "SB_AGENT_PRODUCTION_FIXTURE.PROBE_CATALOG_REFUSED",
                  "probe-only catalogs are not production authority");
  }
  if (input.sidecar_only_evidence || !input.durable_evidence_store) {
    return Finish(input, false,
                  "SB_AGENT_PRODUCTION_FIXTURE.SIDECAR_EVIDENCE_REFUSED",
                  "sidecar-only evidence is not production authority");
  }
  if (input.simulated_actuator_provider) {
    return Finish(input, false,
                  "SB_AGENT_PRODUCTION_FIXTURE.SIMULATED_ACTUATOR_REFUSED",
                  "simulated/default-only actuators cannot back live agents");
  }
  if (input.debug_only_paths_enabled) {
    return Finish(input, false,
                  "SB_AGENT_PRODUCTION_FIXTURE.DEBUG_PATH_REFUSED",
                  "debug-only agent paths cannot back production agents");
  }
  if ((input.synthetic_live_management_state || !input.management_state_durable) &&
      input.live_agent_surface) {
    return Finish(input, false,
                  "SB_AGENT_PRODUCTION_FIXTURE.SYNTHETIC_LIVE_STATE_REFUSED",
                  "live agents require durable runtime state");
  }
  if (input.cluster_stub_live_claim) {
    return Finish(input, false,
                  "SB_AGENT_PRODUCTION_FIXTURE.CLUSTER_STUB_LIVE_REFUSED",
                  "cluster stub providers cannot claim live production authority");
  }
  return Finish(input, true,
                "SB_AGENT_PRODUCTION_FIXTURE.PRODUCTION_PATH_ACCEPTED",
                "durable_production_agent_path");
}

}  // namespace scratchbird::core::agents
