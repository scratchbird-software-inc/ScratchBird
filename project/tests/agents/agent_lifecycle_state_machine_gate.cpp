// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "agent_runtime_manager.hpp"

#include <cstdlib>
#include <iostream>
#include <string_view>

namespace {

namespace agents = scratchbird::core::agents;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

void RequireAllowed(agents::AgentLifecycleState from, agents::AgentLifecycleState to) {
  const auto status = agents::ValidateAgentLifecycleTransition(from, to);
  if (!status.ok) {
    std::cerr << "expected lifecycle transition to be allowed: "
              << agents::AgentLifecycleStateName(from) << "->"
              << agents::AgentLifecycleStateName(to) << '\n';
    std::exit(EXIT_FAILURE);
  }
}

void RequireDenied(agents::AgentLifecycleState from, agents::AgentLifecycleState to) {
  const auto status = agents::ValidateAgentLifecycleTransition(from, to);
  if (status.ok) {
    std::cerr << "expected lifecycle transition to be denied: "
              << agents::AgentLifecycleStateName(from) << "->"
              << agents::AgentLifecycleStateName(to) << '\n';
    std::exit(EXIT_FAILURE);
  }
}

agents::AgentInstanceRecord Instance(agents::AgentLifecycleState state) {
  agents::AgentInstanceRecord instance;
  instance.instance_uuid = "agent-instance:lifecycle-gate";
  instance.agent_type_id = "storage_health_manager";
  instance.policy_uuid = "policy:storage_health_manager:baseline";
  instance.scope = "database/filespace";
  instance.state = state;
  return instance;
}

void TestInstallEnableStartPauseResumeDisable() {
  RequireAllowed(agents::AgentLifecycleState::created, agents::AgentLifecycleState::registered);
  RequireAllowed(agents::AgentLifecycleState::registered, agents::AgentLifecycleState::observe_only);
  RequireAllowed(agents::AgentLifecycleState::registered, agents::AgentLifecycleState::recommend_only);
  RequireAllowed(agents::AgentLifecycleState::registered, agents::AgentLifecycleState::dry_run);
  RequireAllowed(agents::AgentLifecycleState::registered, agents::AgentLifecycleState::running);
  RequireAllowed(agents::AgentLifecycleState::running, agents::AgentLifecycleState::paused);
  RequireAllowed(agents::AgentLifecycleState::paused, agents::AgentLifecycleState::running);
  RequireAllowed(agents::AgentLifecycleState::paused, agents::AgentLifecycleState::disabled);
  RequireAllowed(agents::AgentLifecycleState::disabled, agents::AgentLifecycleState::registered);
  RequireDenied(agents::AgentLifecycleState::created, agents::AgentLifecycleState::running);
  RequireDenied(agents::AgentLifecycleState::disabled, agents::AgentLifecycleState::running);
}

void TestSafeModeQuarantineFailureAndStop() {
  RequireAllowed(agents::AgentLifecycleState::running, agents::AgentLifecycleState::safe_mode);
  RequireAllowed(agents::AgentLifecycleState::safe_mode, agents::AgentLifecycleState::paused);
  RequireAllowed(agents::AgentLifecycleState::safe_mode, agents::AgentLifecycleState::quarantined);
  RequireAllowed(agents::AgentLifecycleState::running, agents::AgentLifecycleState::failed);
  RequireAllowed(agents::AgentLifecycleState::failed, agents::AgentLifecycleState::registered);
  RequireAllowed(agents::AgentLifecycleState::failed, agents::AgentLifecycleState::quarantined);
  RequireAllowed(agents::AgentLifecycleState::quarantined, agents::AgentLifecycleState::disabled);
  RequireDenied(agents::AgentLifecycleState::quarantined, agents::AgentLifecycleState::running);
  RequireAllowed(agents::AgentLifecycleState::running, agents::AgentLifecycleState::stopping);
  RequireAllowed(agents::AgentLifecycleState::stopping, agents::AgentLifecycleState::stopped);
  RequireAllowed(agents::AgentLifecycleState::stopped, agents::AgentLifecycleState::registered);
}

void TestRetiredIsExplicitAndTerminal() {
  RequireAllowed(agents::AgentLifecycleState::created, agents::AgentLifecycleState::retired);
  RequireAllowed(agents::AgentLifecycleState::registered, agents::AgentLifecycleState::retired);
  RequireAllowed(agents::AgentLifecycleState::running, agents::AgentLifecycleState::retired);
  RequireAllowed(agents::AgentLifecycleState::paused, agents::AgentLifecycleState::retired);
  RequireAllowed(agents::AgentLifecycleState::disabled, agents::AgentLifecycleState::retired);
  RequireAllowed(agents::AgentLifecycleState::stopped, agents::AgentLifecycleState::retired);
  RequireAllowed(agents::AgentLifecycleState::failed, agents::AgentLifecycleState::retired);
  RequireAllowed(agents::AgentLifecycleState::quarantined, agents::AgentLifecycleState::retired);
  RequireDenied(agents::AgentLifecycleState::retired, agents::AgentLifecycleState::registered);
  RequireDenied(agents::AgentLifecycleState::retired, agents::AgentLifecycleState::running);
  RequireDenied(agents::AgentLifecycleState::retired, agents::AgentLifecycleState::disabled);

  auto retired = Instance(agents::AgentLifecycleState::stopped);
  const auto status = agents::ApplyAgentLifecycleTransition(
      &retired, agents::AgentLifecycleState::retired, "operator_retirement");
  Require(status.ok, "retirement transition failed");
  retired.retirement_evidence_uuid = "agent-evidence:retired:lifecycle-gate";
  retired.retired_generation = 1;
  Require(retired.state == agents::AgentLifecycleState::retired,
          "retirement transition did not set retired state");
  Require(!retired.disabled_by_operator && !retired.safe_mode && !retired.quarantined,
          "retirement transition left stale lifecycle flags");

  const auto encoded = agents::SerializeAgentInstanceRecord(retired);
  agents::AgentInstanceRecord restored;
  const auto restored_status = agents::RestoreAgentInstanceRecord(encoded, &restored);
  Require(restored_status.ok, "retired lifecycle restore failed");
  Require(restored.state == agents::AgentLifecycleState::retired,
          "retired lifecycle state did not round-trip");

  const auto recovery_status = agents::RecoverAgentLifecycleOnOpen(&restored);
  Require(recovery_status.ok, "retired recovery was rejected");
  Require(restored.state == agents::AgentLifecycleState::retired,
          "retired recovery changed terminal retired state");
}

void TestApplyTransitionFlagsAndOpenRecovery() {
  auto instance = Instance(agents::AgentLifecycleState::running);
  auto status = agents::ApplyAgentLifecycleTransition(
      &instance, agents::AgentLifecycleState::safe_mode, "policy");
  Require(status.ok, "safe_mode transition failed");
  Require(instance.safe_mode, "safe_mode transition did not set safe_mode flag");

  status = agents::ApplyAgentLifecycleTransition(
      &instance, agents::AgentLifecycleState::quarantined, "policy");
  Require(status.ok, "quarantine transition failed");
  Require(instance.quarantined, "quarantine transition did not set quarantined flag");

  auto failed = Instance(agents::AgentLifecycleState::failed);
  status = agents::RecoverAgentLifecycleOnOpen(&failed);
  Require(status.ok, "failed recovery transition was denied");
  Require(failed.state == agents::AgentLifecycleState::registered,
          "failed recovery did not return to registered");
  Require(!failed.disabled_by_operator && !failed.safe_mode && !failed.quarantined,
          "failed recovery left stale lifecycle flags");

  auto stopping = Instance(agents::AgentLifecycleState::stopping);
  status = agents::RecoverAgentLifecycleOnOpen(&stopping);
  Require(status.ok, "stopping recovery transition was denied");
  Require(stopping.state == agents::AgentLifecycleState::registered,
          "stopping recovery did not return to registered");

  auto running = Instance(agents::AgentLifecycleState::running);
  status = agents::RecoverAgentLifecycleOnOpen(&running);
  Require(status.ok, "running recovery transition was denied");
  Require(running.state == agents::AgentLifecycleState::paused,
          "running recovery did not pause active instance");
  Require(!running.disabled_by_operator && !running.safe_mode && !running.quarantined,
          "running recovery left stale lifecycle flags");
}

}  // namespace

int main() {
  TestInstallEnableStartPauseResumeDisable();
  TestSafeModeQuarantineFailureAndStop();
  TestRetiredIsExplicitAndTerminal();
  TestApplyTransitionFlagsAndOpenRecovery();
  return EXIT_SUCCESS;
}
