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
#include <vector>

namespace {

namespace agents = scratchbird::core::agents;

[[noreturn]] void Fail(const std::string& message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, const std::string& message) {
  if (!condition) {
    Fail(message);
  }
}

std::string PolicyFamilyForGate(const std::string& policy_gate) {
  const auto and_pos = policy_gate.find(" and ");
  std::string first = and_pos == std::string::npos
      ? policy_gate
      : policy_gate.substr(0, and_pos);
  const auto dot_pos = first.find('.');
  if (dot_pos != std::string::npos) {
    first = first.substr(0, dot_pos);
  }
  return first;
}

std::string PolicyFieldForGate(const std::string& policy_gate) {
  const auto and_pos = policy_gate.find(" and ");
  const std::string first = and_pos == std::string::npos
      ? policy_gate
      : policy_gate.substr(0, and_pos);
  const auto dot_pos = first.find('.');
  if (dot_pos == std::string::npos || dot_pos + 1 >= first.size()) {
    return {};
  }
  return first.substr(dot_pos + 1);
}

agents::AgentRuntimeContext ValidContext() {
  agents::AgentRuntimeContext context;
  context.security_context_present = true;
  context.database_uuid = "019f006b-0000-7000-8000-000000000001";
  context.principal_uuid = "019f006b-0000-7000-8000-000000000002";
  context.groups = {"ROOT", "OPS", "SEC", "DBA"};
  context.rights = {
      "OBS_AGENT_STATE_READ",
      "OBS_AGENT_CONTROL",
      "OBS_AGENT_POLICY_CONTROL",
      "OBS_AGENT_EVIDENCE_READ",
      "OBS_AGENT_OVERRIDE"};
  context.wall_now_microseconds = 1700000000000000ull;
  context.monotonic_now_microseconds = 1000;
  return context;
}

std::vector<std::string> MetricFamiliesForAllContracts() {
  std::set<std::string> families;
  for (const auto& contract : agents::AgentActionContractRegistry()) {
    families.insert(contract.metric_families.begin(), contract.metric_families.end());
  }
  return {families.begin(), families.end()};
}

agents::AgentPolicy PolicyForContract(
    const agents::AgentActionContractDescriptor& contract) {
  const auto owner = agents::FindAgentType(contract.owning_agent);
  Require(owner.has_value(), "owner missing for " + contract.owning_agent);
  const std::string family = PolicyFamilyForGate(contract.policy_gate);
  return agents::BaselinePolicyForAgentFamily(*owner, family, 1);
}

agents::AgentActionContractEvaluationRequest ValidRequestFor(
    const agents::AgentActionContractDescriptor& contract) {
  static std::vector<agents::AgentPolicy> policies;
  policies.push_back(PolicyForContract(contract));

  agents::AgentActionContractEvaluationRequest request;
  request.context = ValidContext();
  request.policy = &policies.back();
  request.policy_present = true;
  request.policy_gate_present = true;
  request.evidence_store_available = true;
  request.live_prerequisites_enabled = false;
  request.actuator_route_available = false;
  request.available_metric_families = MetricFamiliesForAllContracts();
  return request;
}

bool AllowedEvaluationResult(agents::AgentActionResultClass result_class) {
  return result_class == agents::AgentActionResultClass::accepted ||
         result_class == agents::AgentActionResultClass::dry_run_only ||
         result_class == agents::AgentActionResultClass::approval_required ||
         result_class == agents::AgentActionResultClass::refused ||
         result_class == agents::AgentActionResultClass::suppressed ||
         result_class == agents::AgentActionResultClass::failed_closed;
}

void TestRegistryCompleteness() {
  const auto contracts = agents::AgentActionContractRegistry();
  Require(!contracts.empty(), "action contract registry is empty");

  std::set<std::string> pairs;
  std::map<std::string, std::set<std::string>> owners_by_action;
  for (const auto& contract : contracts) {
    const std::string pair = contract.owning_agent + "|" + contract.action_id;
    Require(pairs.insert(pair).second,
            "duplicate action/owner contract pair: " + pair);
    owners_by_action[contract.action_id].insert(contract.owning_agent);
  }
  for (const auto& entry : owners_by_action) {
    Require(!entry.second.empty(), "action has no owner set: " + entry.first);
  }

  int cluster_refusals = 0;
  int noncluster_checked = 0;
  for (const auto& descriptor : agents::CanonicalAgentRegistry()) {
    for (const auto& action : agents::CanonicalAgentAllowedActionIds(descriptor.type_id)) {
      const auto contract = agents::FindAgentActionContract(descriptor.type_id, action);
      const bool cluster_only = descriptor.cluster_only ||
                                descriptor.deployment == agents::AgentDeployment::cluster;
      if (cluster_only) {
        Require(!contract.has_value(),
                "cluster-only action appeared in non-cluster registry: " +
                    descriptor.type_id + ":" + action);
        const auto decision = agents::EvaluateAgentActionContract(
            descriptor.type_id, action, agents::AgentActionContractEvaluationRequest{});
        Require(decision.result_class == agents::AgentActionResultClass::refused,
                "cluster-only action did not return refusal: " + descriptor.type_id + ":" + action);
        Require(decision.diagnostic_code ==
                    "SB_AGENT_ACTION_CONTRACT.CLUSTER_ONLY_EXCLUDED",
                "cluster-only refusal diagnostic mismatch: " +
                    descriptor.type_id + ":" + action);
        ++cluster_refusals;
        continue;
      }
      Require(contract.has_value(),
              "non-cluster allowed action missing contract: " +
                  descriptor.type_id + ":" + action);
      ++noncluster_checked;
    }
  }
  Require(noncluster_checked > 0, "no non-cluster actions were checked");
  Require(cluster_refusals > 0, "no cluster-only actions were refused");
}

void TestDescriptorFieldsAndEvaluation() {
  int filespace_seen = 0;
  int page_seen = 0;
  int storage_seen = 0;
  for (const auto& contract : agents::AgentActionContractRegistry()) {
    Require(!contract.action_id.empty(), "missing action_id");
    Require(!contract.owning_agent.empty(), "missing owning_agent for " + contract.action_id);
    Require(!contract.actuator.empty(), "missing actuator for " + contract.action_id);
    Require(!contract.permission.empty(), "missing permission for " + contract.action_id);
    Require(!contract.policy_gate.empty(), "missing policy_gate for " + contract.action_id);
    Require(!contract.evidence_kind.empty(), "missing evidence for " + contract.action_id);
    Require(!contract.failure_behavior.empty(),
            "missing failure behavior for " + contract.action_id);
    Require(!contract.metric_precondition_text.empty(),
            "missing metric precondition text for " + contract.action_id);
    Require(!contract.metric_families.empty(),
            "missing metric families for " + contract.action_id);
    Require(!contract.cluster_scoped,
            "cluster-scoped contract leaked into non-cluster gate: " + contract.action_id);

    auto request = ValidRequestFor(contract);
    const auto decision = agents::EvaluateAgentActionContract(contract, request);
    Require(AllowedEvaluationResult(decision.result_class),
            "unexpected result class for " + contract.owning_agent + ":" + contract.action_id);
    Require(!decision.evidence_uuid.empty(),
            "evaluation produced no evidence uuid for " +
                contract.owning_agent + ":" + contract.action_id);
    Require(!(decision.result_class == agents::AgentActionResultClass::accepted &&
              decision.diagnostic_code.empty()),
            "accepted contract had empty diagnostic: " +
                contract.owning_agent + ":" + contract.action_id);
    Require(!decision.mutates_state,
            "contract evaluation mutated state before actuator route slice: " +
                contract.owning_agent + ":" + contract.action_id);

    if (contract.owning_agent == "filespace_capacity_manager") {
      ++filespace_seen;
    }
    if (contract.owning_agent == "page_allocation_manager") {
      ++page_seen;
    }
    if (contract.owning_agent == "storage_health_manager") {
      ++storage_seen;
    }
  }
  Require(filespace_seen > 0, "filespace_capacity_manager action coverage missing");
  Require(page_seen > 0, "page_allocation_manager action coverage missing");
  Require(storage_seen > 0, "storage_health_manager action coverage missing");
}

void TestFailClosedDiagnostics() {
  const auto preallocate = agents::FindAgentActionContract(
      "page_allocation_manager", "preallocate_page_family");
  Require(preallocate.has_value(), "preallocate_page_family contract missing");
  auto default_deny_policy = PolicyForContract(*preallocate);
  default_deny_policy.config_fields["preallocation_allowed"] = "false";
  auto default_deny = ValidRequestFor(*preallocate);
  default_deny.policy = &default_deny_policy;
  const auto default_deny_decision =
      agents::EvaluateAgentActionContract(*preallocate, default_deny);
  Require(default_deny_decision.result_class == agents::AgentActionResultClass::refused,
          "default-deny policy did not refuse");
  Require(default_deny_decision.diagnostic_code ==
              "SB_AGENT_ACTION_CONTRACT.POLICY_DEFAULT_DENY",
          "default-deny diagnostic mismatch");

  auto disabled_policy = PolicyForContract(*preallocate);
  disabled_policy.enabled = false;
  disabled_policy.activation = agents::AgentActivationProfile::disabled;
  disabled_policy.action_mode = "disabled";
  auto disabled_request = ValidRequestFor(*preallocate);
  disabled_request.policy = &disabled_policy;
  const auto disabled_decision =
      agents::EvaluateAgentActionContract(*preallocate, disabled_request);
  Require(disabled_decision.result_class == agents::AgentActionResultClass::refused,
          "disabled policy did not refuse");
  Require(disabled_decision.diagnostic_code ==
              "SB_AGENT_ACTION_CONTRACT.POLICY_DISABLED",
          "disabled-policy diagnostic mismatch");

  const auto recommend = agents::FindAgentActionContract(
      "index_health_manager", "recommend_index_rebuild");
  Require(recommend.has_value(), "recommend_index_rebuild contract missing");
  auto missing_security = ValidRequestFor(*recommend);
  missing_security.context.security_context_present = false;
  const auto missing_security_decision =
      agents::EvaluateAgentActionContract(*recommend, missing_security);
  Require(missing_security_decision.result_class ==
              agents::AgentActionResultClass::failed_closed,
          "missing security did not fail closed");
  Require(missing_security_decision.diagnostic_code ==
              "AGENT.SECURITY_CONTEXT_REQUIRED",
          "missing security diagnostic mismatch");

  auto missing_right = ValidRequestFor(*recommend);
  missing_right.context.groups.clear();
  missing_right.context.rights.clear();
  const auto missing_right_decision =
      agents::EvaluateAgentActionContract(*recommend, missing_right);
  Require(missing_right_decision.result_class ==
              agents::AgentActionResultClass::failed_closed,
          "missing right did not fail closed");
  Require(missing_right_decision.diagnostic_code ==
              "ACTION.PERMISSION_DENIED",
          "missing right diagnostic mismatch");

  auto missing_metric = ValidRequestFor(*recommend);
  missing_metric.available_metric_families.clear();
  const auto missing_metric_decision =
      agents::EvaluateAgentActionContract(*recommend, missing_metric);
  Require(missing_metric_decision.result_class ==
              agents::AgentActionResultClass::failed_closed,
          "missing metric did not fail closed");
  Require(missing_metric_decision.diagnostic_code ==
              "SB_AGENT_ACTION_CONTRACT.METRIC_REQUIRED",
          "missing metric diagnostic mismatch");

  auto missing_evidence = ValidRequestFor(*recommend);
  missing_evidence.evidence_store_available = false;
  const auto missing_evidence_decision =
      agents::EvaluateAgentActionContract(*recommend, missing_evidence);
  Require(missing_evidence_decision.result_class ==
              agents::AgentActionResultClass::failed_closed,
          "missing evidence store did not fail closed");
  Require(missing_evidence_decision.diagnostic_code ==
              "SB_AGENT_ACTION_CONTRACT.EVIDENCE_REQUIRED",
          "missing evidence diagnostic mismatch");

  auto missing_policy = ValidRequestFor(*recommend);
  missing_policy.policy = nullptr;
  missing_policy.policy_present = false;
  const auto missing_policy_decision =
      agents::EvaluateAgentActionContract(*recommend, missing_policy);
  Require(missing_policy_decision.result_class ==
              agents::AgentActionResultClass::failed_closed,
          "missing policy did not fail closed");
  Require(missing_policy_decision.diagnostic_code ==
              "SB_AGENT_ACTION_CONTRACT.POLICY_REQUIRED",
          "missing policy diagnostic mismatch");

  auto missing_gate = ValidRequestFor(*preallocate);
  missing_gate.policy_gate_present = false;
  const auto missing_gate_decision =
      agents::EvaluateAgentActionContract(*preallocate, missing_gate);
  Require(missing_gate_decision.result_class ==
              agents::AgentActionResultClass::failed_closed,
          "missing policy gate did not fail closed");
  Require(missing_gate_decision.diagnostic_code ==
              "SB_AGENT_ACTION_CONTRACT.POLICY_GATE_REQUIRED",
          "missing policy gate diagnostic mismatch");
}

}  // namespace

int main() {
  TestRegistryCompleteness();
  TestDescriptorFieldsAndEvaluation();
  TestFailClosedDiagnostics();
  return EXIT_SUCCESS;
}
