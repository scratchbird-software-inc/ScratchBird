// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "agent_runtime.hpp"

#include <cstdlib>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace agents = scratchbird::core::agents;

[[noreturn]] void Fail(const std::string& message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, const std::string& message) {
  if (!condition) { Fail(message); }
}

bool IsNonClusterRuntimeAgent(const agents::AgentTypeDescriptor& descriptor) {
  return !descriptor.cluster_only &&
         descriptor.deployment != agents::AgentDeployment::cluster &&
         (descriptor.deployment == agents::AgentDeployment::local ||
          descriptor.deployment == agents::AgentDeployment::both);
}

agents::AgentRuntimeContext ValidContext() {
  agents::AgentRuntimeContext context;
  context.security_context_present = true;
  context.private_features_available = true;
  context.standalone_edition = true;
  context.cluster_authority_available = false;
  context.principal_uuid = "agent-runtime-tick-health-gate";
  context.database_uuid = "019e0f2a-006a-7000-8000-000000000001";
  context.wall_now_microseconds = 6001000;
  context.monotonic_now_microseconds = 6001000;
  context.rights.push_back("OBS_AGENT_STATE_READ");
  return context;
}

agents::AgentTickHealthRequest ValidRequest() {
  agents::AgentTickHealthRequest request;
  request.context = ValidContext();
  request.policy_generation = 6;
  return request;
}

std::map<std::string, const agents::AgentTickHealthRecord*> RecordMap(
    const agents::AgentTickHealthResult& result) {
  std::map<std::string, const agents::AgentTickHealthRecord*> by_type;
  for (const auto& record : result.records) {
    Require(by_type.emplace(record.agent_type_id, &record).second,
            "duplicate tick/health record for " + record.agent_type_id);
  }
  return by_type;
}

void RequireEvidence(const agents::AgentTickHealthRecord& record) {
  Require(record.tick_produced, "tick not produced for " + record.agent_type_id);
  Require(record.health_published, "health not published for " + record.agent_type_id);
  Require(!record.health_evidence_uuid.empty(),
          "missing health evidence for " + record.agent_type_id);
  if (record.action_class != agents::AgentActionClass::none ||
      record.action_result_class != agents::AgentActionResultClass::accepted) {
    Require(record.action_evidence_published,
            "action/refusal evidence not published for " + record.agent_type_id);
    Require(!record.action_evidence_uuid.empty(),
            "missing action evidence for " + record.agent_type_id);
  }
  Require(!record.diagnostic_code.empty(),
          "missing diagnostic for " + record.agent_type_id);
}

std::vector<agents::AgentPolicy> DisabledPoliciesForAllNonClusterAgents() {
  std::vector<agents::AgentPolicy> policies;
  for (const auto& descriptor : agents::CanonicalAgentRegistry()) {
    if (!IsNonClusterRuntimeAgent(descriptor)) { continue; }
    auto policy = agents::BaselinePolicyForAgent(descriptor);
    policy.enabled = false;
    policy.activation = agents::AgentActivationProfile::disabled;
    policy.action_mode = "disabled";
    policy.policy_generation = 6;
    policies.push_back(std::move(policy));
  }
  return policies;
}

void TestEveryNonClusterAgentRepresentedExactlyOnce() {
  const auto result =
      agents::BuildNonClusterAgentTickHealthSnapshot(ValidRequest());
  Require(result.status.ok,
          "tick/health snapshot failed: " + result.status.diagnostic_code);
  const auto by_type = RecordMap(result);

  std::set<std::string> expected;
  std::set<std::string> cluster_only;
  for (const auto& descriptor : agents::CanonicalAgentRegistry()) {
    if (IsNonClusterRuntimeAgent(descriptor)) {
      expected.insert(descriptor.type_id);
    } else {
      cluster_only.insert(descriptor.type_id);
    }
  }
  Require(!expected.empty(), "no non-cluster runtime agents found");
  Require(result.records.size() == expected.size(),
          "non-cluster record count mismatch");
  for (const auto& agent_type_id : expected) {
    Require(by_type.find(agent_type_id) != by_type.end(),
            "missing non-cluster tick/health record for " + agent_type_id);
  }
  for (const auto& agent_type_id : cluster_only) {
    Require(by_type.find(agent_type_id) == by_type.end(),
            "cluster-only descriptor appeared in non-cluster tick/health: " +
                agent_type_id);
  }
}

void TestDefaultPoliciesPublishExactHealthClasses() {
  const auto result =
      agents::BuildNonClusterAgentTickHealthSnapshot(ValidRequest());
  Require(result.status.ok, "default tick/health snapshot failed");
  int observe_only = 0;
  int recommend_only = 0;
  int dry_run = 0;
  int policy_disabled = 0;
  int operator_only = 0;
  int local_projection = 0;

  for (const auto& record : result.records) {
    RequireEvidence(record);
    Require(!record.failed_closed,
            "default policy unexpectedly failed closed for " +
                record.agent_type_id + ": " + record.diagnostic_code);
    if (record.cluster_path_failed_closed) { ++local_projection; }
    switch (record.tick_class) {
      case agents::AgentTickHealthClass::observe_only:
        ++observe_only;
        Require(record.selected && record.runnable,
                "observe-only agent not selected/runnable");
        Require(record.lifecycle_state == agents::AgentLifecycleState::observe_only,
                "observe-only lifecycle mismatch");
        break;
      case agents::AgentTickHealthClass::recommend_only:
        ++recommend_only;
        Require(record.selected && record.runnable,
                "recommend-only agent not selected/runnable");
        Require(record.action_class == agents::AgentActionClass::recommendation,
                "recommend-only action class mismatch");
        break;
      case agents::AgentTickHealthClass::dry_run:
        ++dry_run;
        Require(record.selected && record.runnable,
                "dry-run agent not selected/runnable");
        Require(record.action_result_class ==
                    agents::AgentActionResultClass::dry_run_only,
                "dry-run result mismatch");
        break;
      case agents::AgentTickHealthClass::policy_disabled:
        ++policy_disabled;
        Require(record.policy_disabled,
                "policy-disabled class did not mark policy_disabled");
        Require(record.diagnostic_code ==
                    "SB_AGENT_TICK_HEALTH.POLICY_DISABLED",
                "policy-disabled diagnostic mismatch");
        break;
      case agents::AgentTickHealthClass::manual_approval_operator_only:
        ++operator_only;
        Require(record.manual_approval_required,
                "operator-only class did not require manual approval");
        Require(record.action_result_class ==
                    agents::AgentActionResultClass::approval_required,
                "operator-only action result mismatch");
        break;
      case agents::AgentTickHealthClass::selected_running:
      case agents::AgentTickHealthClass::suppressed:
      case agents::AgentTickHealthClass::failed_closed:
        Fail("unexpected default tick class for " + record.agent_type_id +
             ": " + agents::AgentTickHealthClassName(record.tick_class));
    }
  }

  Require(observe_only > 0, "default matrix lacked observe-only evidence");
  Require(recommend_only > 0, "default matrix lacked recommend-only evidence");
  Require(dry_run > 0, "default matrix lacked dry-run evidence");
  Require(policy_disabled > 0, "default matrix lacked policy-disabled evidence");
  Require(operator_only > 0, "default matrix lacked operator-only evidence");
  Require(local_projection > 0,
          "default matrix lacked both-deployment local projection evidence");
}

void TestDisabledPoliciesProduceExactRefusalForEveryNonClusterAgent() {
  auto request = ValidRequest();
  request.use_explicit_policy_state = true;
  request.policies = DisabledPoliciesForAllNonClusterAgents();

  const auto result =
      agents::BuildNonClusterAgentTickHealthSnapshot(request);
  Require(result.status.ok, "disabled-policy snapshot failed");
  Require(!result.records.empty(), "disabled-policy snapshot was empty");
  for (const auto& record : result.records) {
    RequireEvidence(record);
    Require(record.tick_class == agents::AgentTickHealthClass::policy_disabled,
            "disabled policy did not produce policy_disabled for " +
                record.agent_type_id);
    Require(record.policy_disabled, "policy_disabled flag absent");
    Require(!record.selected && !record.runnable,
            "disabled policy was selected/runnable");
    Require(record.action_result_class == agents::AgentActionResultClass::refused,
            "disabled policy did not publish refusal");
    Require(record.diagnostic_code == "SB_AGENT_TICK_HEALTH.POLICY_DISABLED",
            "disabled-policy diagnostic mismatch");
  }
}

void TestLiveActionPolicyPublishesSelectedRunningEvidence() {
  auto descriptor = agents::FindAgentType("metrics_registry_manager");
  Require(descriptor.has_value(), "metrics_registry_manager descriptor missing");
  auto policy = agents::BaselinePolicyForAgent(*descriptor);
  policy.policy_generation = 6;
  policy.activation = agents::AgentActivationProfile::live_action;
  policy.action_mode = "live_action";
  policy.allow_live_action = true;
  policy.require_manual_approval = false;

  auto request = ValidRequest();
  request.use_explicit_policy_state = true;
  request.policies.push_back(policy);

  const auto result =
      agents::BuildNonClusterAgentTickHealthSnapshot(request);
  Require(result.status.ok, "live-action snapshot failed");
  const auto by_type = RecordMap(result);
  const auto found = by_type.find("metrics_registry_manager");
  Require(found != by_type.end(), "metrics_registry_manager live-action record missing");
  const auto& record = *found->second;
  RequireEvidence(record);
  Require(record.tick_class == agents::AgentTickHealthClass::selected_running,
          "live-action policy did not publish selected_running");
  Require(record.selected && record.runnable,
          "selected_running record was not selected/runnable");
  Require(record.lifecycle_state == agents::AgentLifecycleState::running,
          "selected_running lifecycle mismatch");
  Require(record.action_class == agents::AgentActionClass::direct_bounded_action,
          "selected_running action class mismatch");
  Require(record.action_result_class == agents::AgentActionResultClass::accepted,
          "selected_running action result mismatch");
  Require(!record.failed_closed, "selected_running was marked failed_closed");
}

void TestMissingSecurityAndRightsFailClosedWithoutSilentSkips() {
  auto missing_context = ValidRequest();
  missing_context.context.security_context_present = false;
  const auto missing_context_result =
      agents::BuildNonClusterAgentTickHealthSnapshot(missing_context);
  Require(missing_context_result.status.ok,
          "missing-security snapshot did not return inspectable records");
  for (const auto& record : missing_context_result.records) {
    RequireEvidence(record);
    Require(record.tick_class == agents::AgentTickHealthClass::failed_closed,
            "missing security did not fail closed for " + record.agent_type_id);
    Require(record.diagnostic_code == "SB_AGENT_SECURITY.CONTEXT_REQUIRED",
            "missing-security diagnostic mismatch: " + record.diagnostic_code);
  }

  auto missing_right = ValidRequest();
  missing_right.context.rights.clear();
  const auto missing_right_result =
      agents::BuildNonClusterAgentTickHealthSnapshot(missing_right);
  Require(missing_right_result.status.ok,
          "missing-right snapshot did not return inspectable records");
  for (const auto& record : missing_right_result.records) {
    RequireEvidence(record);
    Require(record.tick_class == agents::AgentTickHealthClass::failed_closed,
            "missing right did not fail closed for " + record.agent_type_id);
    Require(record.diagnostic_code == "SB_AGENT_SECURITY.RIGHT_REQUIRED",
            "missing-right diagnostic mismatch: " + record.diagnostic_code);
  }
}

void TestMissingMetricAndSuppressionPublishExactEvidence() {
  auto missing_metric = ValidRequest();
  missing_metric.missing_metric_families.push_back("sb_page_free_count");
  const auto metric_result =
      agents::BuildNonClusterAgentTickHealthSnapshot(missing_metric);
  Require(metric_result.status.ok, "missing-metric snapshot failed");
  const auto by_type = RecordMap(metric_result);
  const auto page = by_type.find("page_allocation_manager");
  Require(page != by_type.end(), "page_allocation_manager record missing");
  Require(page->second->tick_class == agents::AgentTickHealthClass::failed_closed,
          "missing required page metric did not fail closed");
  Require(page->second->diagnostic_code ==
              "SB_AGENT_METRICS.REQUIRED_METRIC_MISSING",
          "missing metric diagnostic mismatch");
  Require(page->second->detail == "sb_page_free_count",
          "missing metric detail mismatch");
  RequireEvidence(*page->second);

  auto suppressed = ValidRequest();
  suppressed.suppressed_agent_type_ids.push_back("page_allocation_manager");
  const auto suppressed_result =
      agents::BuildNonClusterAgentTickHealthSnapshot(suppressed);
  Require(suppressed_result.status.ok, "suppressed snapshot failed");
  const auto suppressed_by_type = RecordMap(suppressed_result);
  const auto suppressed_page = suppressed_by_type.find("page_allocation_manager");
  Require(suppressed_page != suppressed_by_type.end(),
          "suppressed page_allocation_manager record missing");
  Require(suppressed_page->second->tick_class ==
              agents::AgentTickHealthClass::suppressed,
          "suppressed agent did not publish suppressed class");
  Require(suppressed_page->second->suppressed,
          "suppressed flag missing from suppressed record");
  Require(suppressed_page->second->action_result_class ==
              agents::AgentActionResultClass::suppressed,
          "suppressed action result mismatch");
  Require(suppressed_page->second->diagnostic_code ==
              "SB_AGENT_TICK_HEALTH.SUPPRESSED",
          "suppressed diagnostic mismatch");
  RequireEvidence(*suppressed_page->second);
}

}  // namespace

int main() {
  TestEveryNonClusterAgentRepresentedExactlyOnce();
  TestDefaultPoliciesPublishExactHealthClasses();
  TestDisabledPoliciesProduceExactRefusalForEveryNonClusterAgent();
  TestLiveActionPolicyPublishesSelectedRunningEvidence();
  TestMissingSecurityAndRightsFailClosedWithoutSilentSkips();
  TestMissingMetricAndSuppressionPublishExactEvidence();
  return EXIT_SUCCESS;
}
