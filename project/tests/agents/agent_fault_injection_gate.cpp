// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "agent_runtime.hpp"
#include "uuid.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

namespace agents = scratchbird::core::agents;
namespace platform = scratchbird::core::platform;
namespace uuid = scratchbird::core::uuid;

[[noreturn]] void Fail(const std::string& message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, const std::string& message) {
  if (!condition) { Fail(message); }
}

bool Contains(const std::vector<std::string>& values, const std::string& value) {
  for (const auto& candidate : values) {
    if (candidate == value) { return true; }
  }
  return false;
}

void RequireObjectUuid(const std::string& value, const std::string& label) {
  Require(!value.empty(), label + " UUID was empty");
  Require(value.find("agent-fault-evidence:") == std::string::npos,
          label + " leaked old label-prefixed evidence UUID");
  Require(uuid::ParseDurableEngineIdentityUuid(platform::UuidKind::object, value).ok(),
          label + " was not a typed durable engine UUID: " + value);
}

struct ExpectedScenario {
  std::string key;
  std::string diagnostic_code;
  std::string evidence_kind;
  agents::AgentFaultInjectionRecoveryResponse recovery_response =
      agents::AgentFaultInjectionRecoveryResponse::fail_closed;
};

std::vector<ExpectedScenario> RequiredScenarios() {
  using Recovery = agents::AgentFaultInjectionRecoveryResponse;
  return {
      {"watchdog_timeout", "SB_AGENT_ISOLATION.WATCHDOG_EXPIRED",
       "supervision_failure", Recovery::supervision_restart_backoff},
      {"runtime_timeout", "SB_AGENT_SUPERVISION.RUNTIME_TIMEOUT",
       "supervision_failure", Recovery::supervision_restart_backoff},
      {"disk_full", "SB_AGENT_FAULT.DISK_FULL_FAIL_CLOSED",
       "storage_actuator_failure", Recovery::fail_closed},
      {"fsync_failure", "SB_AGENT_FAULT.FSYNC_FAILURE_FAIL_CLOSED",
       "durability_fence_failure", Recovery::fail_closed},
      {"permission_denied", "SB_AGENT_FAULT.PERMISSION_DENIED_FAIL_CLOSED",
       "storage_permission_failure", Recovery::fail_closed},
      {"corrupt_metric", "SB_AGENT_FAULT.CORRUPT_METRIC_FAIL_CLOSED",
       "metric_sample_rejected", Recovery::reject_metric_sample},
      {"invalid_policy", "SB_AGENT_FAULT.INVALID_POLICY_FAIL_CLOSED",
       "policy_rejected", Recovery::reject_policy},
      {"queue_corruption", "SB_AGENT_FAULT.QUEUE_CORRUPTION_QUARANTINE",
       "queue_integrity_failure", Recovery::supervision_quarantine},
      {"partial_filespace_growth",
       "SB_AGENT_FAULT.PARTIAL_FILESPACE_GROWTH_FAIL_CLOSED",
       "recovery_required", Recovery::fail_closed},
      {"partial_page_preallocation",
       "SB_AGENT_FAULT.PARTIAL_PAGE_PREALLOCATION_FAIL_CLOSED",
       "recovery_required", Recovery::fail_closed},
      {"restart_mid_action", "SB_AGENT_ACTION.FAILED_BACKOFF",
       "supervision_failure", Recovery::supervision_restart_backoff}};
}

std::string ExpectedEvidenceDetail(const ExpectedScenario& expected) {
  return "scenario=" + expected.key +
         ";diagnostic=" + expected.diagnostic_code +
         ";evidence_before_success=true;success_reported=false;"
         "durable_state_changed=false;recovery=" +
         agents::AgentFaultInjectionRecoveryResponseName(
             expected.recovery_response);
}

void RequireCommonFailClosedEvidence(
    const agents::AgentFaultInjectionResult& result,
    const ExpectedScenario& expected) {
  Require(!result.status.ok, expected.key + " unexpectedly succeeded");
  Require(result.status.diagnostic_code == expected.diagnostic_code,
          expected.key + " status diagnostic mismatch: " +
              result.status.diagnostic_code);
  Require(result.diagnostic_code == expected.diagnostic_code,
          expected.key + " result diagnostic mismatch: " +
              result.diagnostic_code);
  Require(agents::IsKnownAgentDiagnosticCode(expected.diagnostic_code),
          expected.key + " diagnostic was not registered");
  Require(result.result_class == agents::AgentActionResultClass::failed_closed,
          expected.key + " did not report failed_closed result");
  Require(result.failed_closed, expected.key + " failed_closed flag missing");
  Require(!result.durable_state_changed,
          expected.key + " reported durable state mutation");
  Require(!result.success_reported,
          expected.key + " reported partial success");
  Require(result.evidence_recorded_before_success,
          expected.key + " did not preserve evidence-before-success");
  Require(!result.unsafe_state_mutation,
          expected.key + " reported unsafe state mutation");
  Require(result.evidence_kind == expected.evidence_kind,
          expected.key + " evidence kind mismatch: " + result.evidence_kind);
  RequireObjectUuid(result.evidence_uuid, expected.key + " evidence_uuid");
  Require(result.evidence_detail == ExpectedEvidenceDetail(expected),
          expected.key + " evidence detail mismatch: " +
              result.evidence_detail);
  Require(result.recovery_response == expected.recovery_response,
          expected.key + " recovery response mismatch");
}

void TestScenarioRegistryAndExactEvidence() {
  const auto scenarios = agents::AgentFaultInjectionScenarios();
  const auto descriptors = agents::AgentFaultInjectionScenarioDescriptors();
  Require(!descriptors.empty(), "fault injection descriptors missing");

  for (const auto& expected : RequiredScenarios()) {
    Require(Contains(scenarios, expected.key),
            expected.key + " missing from scenario list");
    const auto descriptor =
        agents::FindAgentFaultInjectionScenarioDescriptor(expected.key);
    Require(descriptor.has_value(), expected.key + " descriptor missing");
    Require(descriptor->diagnostic_code == expected.diagnostic_code,
            expected.key + " descriptor diagnostic mismatch");

    const auto result =
        agents::EvaluateAgentFaultInjectionScenarioDetailed(expected.key);
    RequireCommonFailClosedEvidence(result, expected);

    const auto status = agents::EvaluateFaultInjectionScenario(expected.key);
    Require(!status.ok, expected.key + " wrapper unexpectedly succeeded");
    Require(status.diagnostic_code == expected.diagnostic_code,
            expected.key + " wrapper diagnostic mismatch: " +
                status.diagnostic_code);
  }
}

void TestUnknownScenarioFailsExact() {
  const auto result =
      agents::EvaluateAgentFaultInjectionScenarioDetailed("unknown_pfard");
  Require(!result.status.ok, "unknown scenario unexpectedly succeeded");
  Require(result.status.diagnostic_code == "SB_AGENT_FAULT.UNKNOWN_SCENARIO",
          "unknown scenario diagnostic mismatch");
  Require(result.diagnostic_code == "SB_AGENT_FAULT.UNKNOWN_SCENARIO",
          "unknown scenario result diagnostic mismatch");
  Require(result.evidence_kind == "fault_injection_unknown",
          "unknown scenario evidence kind mismatch");
  RequireObjectUuid(result.evidence_uuid, "unknown scenario evidence_uuid");
  Require(result.evidence_detail == "unknown_scenario:unknown_pfard",
          "unknown scenario evidence detail mismatch");

  const auto status = agents::EvaluateFaultInjectionScenario("unknown_pfard");
  Require(!status.ok, "unknown wrapper unexpectedly succeeded");
  Require(status.diagnostic_code == "SB_AGENT_FAULT.UNKNOWN_SCENARIO",
          "unknown wrapper diagnostic mismatch");
}

void TestRestartMidActionUsesSupervisionBackoff() {
  const auto result =
      agents::EvaluateAgentFaultInjectionScenarioDetailed("restart_mid_action");
  Require(result.uses_supervision,
          "restart_mid_action did not use supervision path");
  Require(result.supervision.status.diagnostic_code ==
              "SB_AGENT_ACTION.FAILED_BACKOFF",
          "restart_mid_action supervision diagnostic mismatch");
  Require(result.supervision.state == agents::AgentLifecycleState::failed,
          "restart_mid_action did not move to failed state");
  Require(result.state_after == agents::AgentLifecycleState::failed,
          "restart_mid_action result state mismatch");
  Require(result.supervision.restart_attempts == 1,
          "restart_mid_action restart attempts mismatch");
  Require(result.supervision.backoff_microseconds == 100,
          "restart_mid_action backoff mismatch");
  Require(result.supervision.restart_not_before_microseconds == 1100,
          "restart_mid_action restart deadline mismatch");
  Require(result.supervision.cooldown_until_microseconds == 3000,
          "restart_mid_action cooldown mismatch");
  Require(result.supervision.lease_cleared,
          "restart_mid_action did not clear lease");
  Require(!result.supervision.restart_allowed,
          "restart_mid_action allowed silent continuation");
  Require(!result.success_reported,
          "restart_mid_action reported partial success");
}

void TestTimeoutsUseSupervisionBackoff() {
  const auto watchdog =
      agents::EvaluateAgentFaultInjectionScenarioDetailed("watchdog_timeout");
  Require(watchdog.uses_supervision, "watchdog did not use supervision");
  Require(watchdog.supervision.status.diagnostic_code ==
              "SB_AGENT_ISOLATION.WATCHDOG_EXPIRED",
          "watchdog supervision diagnostic mismatch");
  Require(watchdog.supervision.state == agents::AgentLifecycleState::failed,
          "watchdog did not fail instance");
  Require(watchdog.supervision.backoff_microseconds == 100,
          "watchdog backoff mismatch");
  Require(watchdog.supervision.restart_not_before_microseconds == 1100,
          "watchdog restart deadline mismatch");
  Require(watchdog.supervision.lease_cleared,
          "watchdog did not clear lease");

  const auto runtime =
      agents::EvaluateAgentFaultInjectionScenarioDetailed("runtime_timeout");
  Require(runtime.uses_supervision, "runtime did not use supervision");
  Require(runtime.supervision.status.diagnostic_code ==
              "SB_AGENT_SUPERVISION.RUNTIME_TIMEOUT",
          "runtime supervision diagnostic mismatch");
  Require(runtime.supervision.state == agents::AgentLifecycleState::failed,
          "runtime did not fail instance");
  Require(runtime.supervision.backoff_microseconds == 100,
          "runtime backoff mismatch");
  Require(runtime.supervision.restart_not_before_microseconds == 1101,
          "runtime restart deadline mismatch");
  Require(runtime.supervision.lease_cleared,
          "runtime did not clear lease");
}

void TestQueueCorruptionQuarantines() {
  const auto result =
      agents::EvaluateAgentFaultInjectionScenarioDetailed("queue_corruption");
  Require(result.uses_supervision,
          "queue_corruption did not use supervision evidence");
  Require(result.supervision.quarantined,
          "queue_corruption did not quarantine");
  Require(result.state_after == agents::AgentLifecycleState::quarantined,
          "queue_corruption result state mismatch");
  Require(result.supervision.lease_cleared,
          "queue_corruption did not clear lease");
  Require(!result.success_reported,
          "queue_corruption reported partial success");
}

void RequirePartialActionFailedClosed(const std::string& scenario,
                                      const std::string& diagnostic_code) {
  const auto result =
      agents::EvaluateAgentFaultInjectionScenarioDetailed(scenario);
  Require(result.failed_closed, scenario + " did not fail closed");
  Require(!result.durable_state_changed,
          scenario + " changed durable state");
  Require(!result.success_reported,
          scenario + " reported success");
  Require(result.recovery_response ==
              agents::AgentFaultInjectionRecoveryResponse::fail_closed,
          scenario + " did not require fail_closed recovery");
  Require(result.evidence_kind == "recovery_required",
          scenario + " missing recovery_required evidence");
  Require(result.evidence_detail.find("recovery=fail_closed") !=
              std::string::npos,
          scenario + " evidence detail missing fail_closed recovery");
  Require(result.uses_arbitration, scenario + " did not use arbitration");
  Require(result.arbitration.outcome ==
              agents::AgentArbitrationOutcome::both_denied,
          scenario + " arbitration did not deny both");
  Require(result.arbitration.priority_rule ==
              agents::AgentArbitrationPriorityRule::safety_precondition_failed,
          scenario + " arbitration priority rule mismatch");
  Require(result.arbitration.diagnostic_code ==
              "SB_AGENT_ARBITRATION.SAFETY_PRECONDITION_FAILED",
          scenario + " arbitration diagnostic mismatch");
  Require(result.arbitration.detail == diagnostic_code,
          scenario + " arbitration detail mismatch");
  Require(result.arbitration.winning_action_uuid.empty(),
          scenario + " produced a winning action");
  Require(result.arbitration.normalized_actions.size() == 1,
          scenario + " normalized action count mismatch");
  RequireObjectUuid(result.arbitration.normalized_actions.front().action_uuid,
                    scenario + " normalized action_uuid");
  Require(Contains(result.arbitration.losing_action_uuids,
                   result.arbitration.normalized_actions.front().action_uuid),
          scenario + " missing losing action evidence");
  Require(!result.arbitration.normalized_actions.front()
               .safety_preconditions_passed,
          scenario + " safety precondition was not failed");
  Require(result.arbitration.normalized_actions.front().evidence_uuid ==
              result.evidence_uuid,
          scenario + " normalized action evidence mismatch");
}

void TestPartialActionFailuresFailClosed() {
  RequirePartialActionFailedClosed(
      "partial_filespace_growth",
      "SB_AGENT_FAULT.PARTIAL_FILESPACE_GROWTH_FAIL_CLOSED");
  RequirePartialActionFailedClosed(
      "partial_page_preallocation",
      "SB_AGENT_FAULT.PARTIAL_PAGE_PREALLOCATION_FAIL_CLOSED");
}

}  // namespace

int main() {
  TestScenarioRegistryAndExactEvidence();
  TestUnknownScenarioFailsExact();
  TestRestartMidActionUsesSupervisionBackoff();
  TestTimeoutsUseSupervisionBackoff();
  TestQueueCorruptionQuarantines();
  TestPartialActionFailuresFailClosed();
  return EXIT_SUCCESS;
}
