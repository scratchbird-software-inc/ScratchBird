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
#include <string>

namespace {

namespace agents = scratchbird::core::agents;

[[noreturn]] void Fail(const std::string& message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, const std::string& message) {
  if (!condition) { Fail(message); }
}

void RequireStatus(const agents::AgentRuntimeStatus& status,
                   bool ok,
                   const std::string& diagnostic_code,
                   const std::string& label) {
  Require(status.ok == ok, label + " ok mismatch");
  Require(status.diagnostic_code == diagnostic_code,
          label + " diagnostic mismatch: " + status.diagnostic_code);
}

agents::AgentPolicy Policy() {
  const auto descriptor = agents::FindAgentType("page_allocation_manager");
  Require(descriptor.has_value(), "page_allocation_manager descriptor missing");
  auto policy = agents::BaselinePolicyForAgent(*descriptor);
  policy.lease_microseconds = 1000;
  policy.cooldown_microseconds = 2000;
  policy.max_runtime_microseconds = 500;
  policy.max_restart_attempts = 2;
  policy.initial_backoff_microseconds = 100;
  policy.max_backoff_microseconds = 1000;
  return policy;
}

agents::AgentInstanceRecord Instance(agents::AgentLifecycleState state) {
  agents::AgentInstanceRecord instance;
  instance.instance_uuid = "agent-instance:supervision-gate";
  instance.agent_type_id = "page_allocation_manager";
  instance.policy_uuid = "policy:page_allocation_manager:page_preallocation_policy:baseline";
  instance.scope = "database/filespace/page_family/page_type";
  instance.state = state;
  return instance;
}

agents::AgentActionRequest Action(bool dry_run) {
  agents::AgentActionRequest action;
  action.action_uuid = "agent-action:supervision-gate";
  action.agent_type_id = "page_allocation_manager";
  action.instance_uuid = "agent-instance:supervision-gate";
  action.action_class = agents::AgentActionClass::request_action;
  action.actuator_id = "page_allocation_manager";
  action.operation_id = "page_preallocation_request";
  action.idempotency_key = "agent-action:supervision-gate:idempotent";
  action.dry_run = dry_run;
  return action;
}

void TestLeaseAcquisitionAndRefusal() {
  auto instance = Instance(agents::AgentLifecycleState::running);
  const auto policy = Policy();
  const auto acquired = agents::AcquireAgentRunLease(&instance, policy, 1000);
  RequireStatus(acquired, true, "SB_AGENT_SCHEDULER.LEASE_ACQUIRED",
                "lease acquisition");
  Require(instance.lease_until_microseconds == 2000,
          "lease acquisition set wrong lease deadline");
  Require(instance.last_run_start_microseconds == 1000,
          "lease acquisition did not record run start");
  Require(instance.run_generation == 1,
          "lease acquisition did not increment run generation");

  const auto refused = agents::AcquireAgentRunLease(&instance, policy, 1500);
  RequireStatus(refused, false, "SB_AGENT_SCHEDULER.LEASE_HELD",
                "lease refusal");
  Require(instance.lease_until_microseconds == 2000,
          "lease refusal changed existing lease deadline");
}

void TestWatchdogTimeoutAndExceptionContainment() {
  const auto policy = Policy();
  auto watchdog = Instance(agents::AgentLifecycleState::running);
  watchdog.lease_until_microseconds = 9000;
  auto isolated = agents::ValidateExecutionIsolation(watchdog, false, true);
  RequireStatus(isolated, false, "SB_AGENT_ISOLATION.WATCHDOG_EXPIRED",
                "watchdog isolation validation");
  auto watchdog_decision = agents::RecordAgentSupervisionFailure(
      &watchdog, policy, agents::AgentSupervisionFailureKind::watchdog_timeout,
      3000, "watchdog");
  RequireStatus(watchdog_decision.status, false,
                "SB_AGENT_ISOLATION.WATCHDOG_EXPIRED",
                "watchdog supervision failure");
  Require(watchdog.state == agents::AgentLifecycleState::failed,
          "watchdog failure did not move instance to failed");
  Require(watchdog.lease_until_microseconds == 0,
          "watchdog failure did not clear lease");

  auto exception = Instance(agents::AgentLifecycleState::running);
  exception.lease_until_microseconds = 9000;
  isolated = agents::ValidateExecutionIsolation(exception, true, false);
  RequireStatus(isolated, false, "SB_AGENT_ISOLATION.EXCEPTION_CONTAINED",
                "exception isolation validation");
  const auto exception_decision = agents::RecordAgentSupervisionFailure(
      &exception, policy, agents::AgentSupervisionFailureKind::exception,
      4000, "contained_exception");
  RequireStatus(exception_decision.status, false,
                "SB_AGENT_ISOLATION.EXCEPTION_CONTAINED",
                "exception supervision failure");
  Require(exception.state == agents::AgentLifecycleState::failed,
          "exception failure did not move instance to failed");
  Require(exception.lease_until_microseconds == 0,
          "exception failure did not clear lease");
}

void TestRuntimeAndTickTimeouts() {
  const auto policy = Policy();
  auto runtime = Instance(agents::AgentLifecycleState::running);
  runtime.last_run_start_microseconds = 1000;
  runtime.lease_until_microseconds = 10000;
  const auto runtime_decision =
      agents::EvaluateAgentSupervisionTick(&runtime, policy, 1601);
  RequireStatus(runtime_decision.status, false,
                "SB_AGENT_SUPERVISION.RUNTIME_TIMEOUT",
                "runtime timeout");
  Require(runtime.state == agents::AgentLifecycleState::failed,
          "runtime timeout did not move instance to failed");
  Require(runtime.lease_until_microseconds == 0,
          "runtime timeout did not clear lease");
  Require(runtime.last_run_end_microseconds == 1601,
          "runtime timeout did not record run end");

  auto tick = Instance(agents::AgentLifecycleState::running);
  tick.last_run_start_microseconds = 1000;
  tick.lease_until_microseconds = 1800;
  const auto tick_decision =
      agents::EvaluateAgentSupervisionTick(&tick, policy, 1801);
  RequireStatus(tick_decision.status, false,
                "SB_AGENT_SUPERVISION.TICK_TIMEOUT",
                "tick timeout");
  Require(tick.state == agents::AgentLifecycleState::failed,
          "tick timeout did not move instance to failed");
  Require(tick.lease_until_microseconds == 0,
          "tick timeout did not clear lease");
}

void TestFailedActionBackoffAndRestartDeniedInBackoff() {
  auto instance = Instance(agents::AgentLifecycleState::running);
  const auto policy = Policy();
  instance.lease_until_microseconds = 5000;
  const auto failure = agents::RecordAgentSupervisionFailure(
      &instance, policy, agents::AgentSupervisionFailureKind::action_failed,
      2000, "actuator_failed");
  RequireStatus(failure.status, false, "SB_AGENT_ACTION.FAILED_BACKOFF",
                "failed action");
  Require(instance.state == agents::AgentLifecycleState::failed,
          "failed action did not move instance to failed");
  Require(failure.backoff_microseconds == 100,
          "failed action used wrong first backoff");
  Require(instance.restart_not_before_microseconds == 2100,
          "failed action set wrong restart deadline");
  Require(instance.cooldown_until_microseconds == 4000,
          "failed action set wrong retry cooldown");
  Require(instance.lease_until_microseconds == 0,
          "failed action did not clear lease");

  const auto retry = agents::ValidateAgentActionRetry(instance, 3000);
  RequireStatus(retry, false, "SB_AGENT_ACTION.RETRY_COOLDOWN_ACTIVE",
                "action retry cooldown");

  const auto restart = agents::RequestAgentSupervisionRestart(
      &instance, policy, 2050);
  RequireStatus(restart, false, "SB_AGENT_RESTART.BACKOFF_ACTIVE",
                "restart during backoff");
  Require(instance.state == agents::AgentLifecycleState::failed,
          "restart denied in backoff changed failed state");
}

void TestQuarantineAfterRestartLimit() {
  auto instance = Instance(agents::AgentLifecycleState::running);
  const auto policy = Policy();

  auto first = agents::RecordAgentSupervisionFailure(
      &instance, policy, agents::AgentSupervisionFailureKind::exception,
      1000, "first");
  RequireStatus(first.status, false, "SB_AGENT_ISOLATION.EXCEPTION_CONTAINED",
                "first failure before quarantine");
  Require(instance.state == agents::AgentLifecycleState::failed,
          "first failure should be failed");

  auto second = agents::RecordAgentSupervisionFailure(
      &instance, policy, agents::AgentSupervisionFailureKind::exception,
      1200, "second");
  RequireStatus(second.status, false, "SB_AGENT_ISOLATION.EXCEPTION_CONTAINED",
                "second failure before quarantine");
  Require(instance.state == agents::AgentLifecycleState::failed,
          "second failure should still be failed");

  const auto third = agents::RecordAgentSupervisionFailure(
      &instance, policy, agents::AgentSupervisionFailureKind::exception,
      1400, "third");
  RequireStatus(third.status, false, "SB_AGENT_RESTART.LIMIT_EXHAUSTED",
                "quarantine after restart limit");
  Require(instance.state == agents::AgentLifecycleState::quarantined,
          "restart limit did not quarantine instance");
  Require(instance.quarantined, "restart limit did not set quarantine flag");
  Require(instance.lease_until_microseconds == 0,
          "quarantine after restart limit did not clear lease");
  Require(instance.last_failure_diagnostic_code ==
              "SB_AGENT_RESTART.LIMIT_EXHAUSTED",
          "quarantine after restart limit recorded wrong diagnostic");
}

void TestCancellationClearsLeaseAndPauses() {
  auto instance = Instance(agents::AgentLifecycleState::running);
  instance.lease_until_microseconds = 5000;
  instance.last_run_start_microseconds = 1000;
  const auto cancelled = agents::CancelAgentRun(&instance, 2000,
                                                "operator_cancelled");
  RequireStatus(cancelled, true, "SB_AGENT_SUPERVISION.CANCELLED",
                "cancellation");
  Require(instance.state == agents::AgentLifecycleState::paused,
          "cancellation did not pause running instance");
  Require(instance.lease_until_microseconds == 0,
          "cancellation did not clear lease");
  Require(instance.last_run_end_microseconds == 2000,
          "cancellation did not record run end");
  Require(instance.supervision_failure_count == 0,
          "cancellation incremented failure count");
}

void TestActionAndLeaseDeniedWhileQuarantined() {
  auto instance = Instance(agents::AgentLifecycleState::running);
  const auto policy = Policy();
  instance.lease_until_microseconds = 5000;
  const auto quarantined = agents::QuarantineAgentInstance(
      &instance, 2500, "operator_quarantine");
  RequireStatus(quarantined, true, "SB_AGENT_QUARANTINE.APPLIED",
                "operator quarantine");
  Require(instance.state == agents::AgentLifecycleState::quarantined,
          "operator quarantine did not set quarantined state");
  Require(instance.lease_until_microseconds == 0,
          "operator quarantine did not clear lease");

  const auto action_denied = agents::ValidateAgentSafeMode(
      instance, Action(true));
  RequireStatus(action_denied, false, "SB_AGENT_SAFE_MODE.QUARANTINED",
                "quarantined action denial");

  const auto lease_denied = agents::AcquireAgentRunLease(&instance, policy, 3000);
  RequireStatus(lease_denied, false, "SB_AGENT_SCHEDULER.INSTANCE_NOT_RUNNABLE",
                "quarantined lease denial");
  Require(instance.lease_until_microseconds == 0,
          "quarantined lease denial left lease set");
}

}  // namespace

int main() {
  TestLeaseAcquisitionAndRefusal();
  TestWatchdogTimeoutAndExceptionContainment();
  TestRuntimeAndTickTimeouts();
  TestFailedActionBackoffAndRestartDeniedInBackoff();
  TestQuarantineAfterRestartLimit();
  TestCancellationClearsLeaseAndPauses();
  TestActionAndLeaseDeniedWhileQuarantined();
  return EXIT_SUCCESS;
}
