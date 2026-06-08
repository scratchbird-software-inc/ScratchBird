// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "agent_runtime.hpp"
#include "metric_contracts.hpp"

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
  if (!condition) { Fail(message); }
}

std::string PolicyFamilyForGate(const std::string& policy_gate) {
  const auto and_pos = policy_gate.find(" and ");
  std::string first = and_pos == std::string::npos
      ? policy_gate
      : policy_gate.substr(0, and_pos);
  const auto dot_pos = first.find('.');
  if (dot_pos != std::string::npos) { first = first.substr(0, dot_pos); }
  return first;
}

agents::AgentRuntimeContext Context() {
  agents::AgentRuntimeContext context;
  context.security_context_present = true;
  context.private_features_available = true;
  context.standalone_edition = true;
  context.database_uuid = "019f006f-0000-7000-8000-000000000001";
  context.principal_uuid = "019f006f-0000-7000-8000-000000000002";
  context.groups = {"ROOT", "OPS", "SEC", "DBA"};
  context.rights = {
      "OBS_AGENT_STATE_READ",
      "OBS_AGENT_CONTROL",
      "OBS_AGENT_ACTION_APPROVE",
      "OBS_AGENT_ACTION_CANCEL",
      "OBS_AGENT_RECOMMENDATION_READ",
      "OBS_AGENT_OVERRIDE",
      "OBS_SUPPORT_BUNDLE_READ",
      "SEC_REDACTION_POLICY_EDIT",
      "SEC_IDENTITY_ADMIN",
      "SEC_AUTH_METRICS_READ",
      "FILESPACE_LIFECYCLE_CONTROL",
      "FILESPACE_LIFECYCLE_TRUNCATE",
      "JOB_ADMIN",
      "BACKUP_CONTROL",
      "RESTORE_DRILL_CONTROL",
      "RESTORE_PLAN_CONTROL",
      "SESSION_KILL",
      "SESSION_CONTROL",
      "EXPORT_CONTROL"};
  context.wall_now_microseconds = 1700000000000060ull;
  context.monotonic_now_microseconds = 1000000;
  return context;
}

agents::AgentPolicy PolicyFor(
    const agents::AgentActionContractDescriptor& contract) {
  const auto owner = agents::FindAgentType(contract.owning_agent);
  Require(owner.has_value(), "contract owner missing: " + contract.owning_agent);
  auto policy = agents::BaselinePolicyForAgentFamily(
      *owner, PolicyFamilyForGate(contract.policy_gate), 6);
  policy.activation = agents::AgentActivationProfile::live_action;
  policy.allow_live_action = true;
  policy.require_dry_run_before_live = false;
  policy.require_manual_approval = false;
  return policy;
}

agents::AgentResourceBudgetEvaluationInput AllowingBudget(
    const agents::AgentPolicy& policy) {
  agents::AgentResourceBudgetEvaluationInput input;
  input.budget = agents::DefaultAgentResourceBudgetForPolicy(policy);
  input.budget.max_cpu_time_microseconds = 1000;
  input.budget.max_memory_bytes = 1000000;
  input.budget.max_io_bytes = 1000000;
  input.budget.max_io_ops = 1000;
  input.budget.max_thread_slots = 4;
  input.budget.max_queue_depth = 8;
  input.budget.min_run_interval_microseconds = 0;
  input.budget.retry_backoff_microseconds = 0;
  input.budget.watchdog_timeout_microseconds = 1000;
  input.budget.max_history_query_rows = policy.max_history_query_rows;
  input.budget.max_evidence_fanout = policy.max_evidence_fanout;
  input.budget.max_label_cardinality = policy.max_label_cardinality;
  input.usage.cpu_time_microseconds = 10;
  input.usage.memory_bytes = 10;
  input.usage.io_bytes = 10;
  input.usage.io_ops = 1;
  input.usage.thread_slots = 1;
  input.usage.queue_depth = 1;
  input.usage.runtime_microseconds = 10;
  input.usage.history_query_rows = 1;
  input.usage.evidence_fanout = 1;
  input.usage.label_cardinality = 1;
  return input;
}

std::vector<agents::AgentMetricObservation> ObservationsFor(
    const agents::AgentActionContractDescriptor& contract) {
  std::vector<agents::AgentMetricObservation> observations;
  for (const auto& family : contract.metric_families) {
    agents::AgentMetricObservation observation;
    observation.metric_family = family;
    const auto* descriptor =
        scratchbird::core::metrics::DefaultMetricRegistry().FindDescriptorOrAlias(
            family);
    observation.namespace_path =
        descriptor == nullptr ? "sys.metrics.test" : descriptor->namespace_path;
    observation.age_microseconds = 1;
    observation.present = true;
    observation.trusted = true;
    observation.schema_compatible = true;
    observation.scope_compatible = true;
    observation.evidence_uuid = "metric-evidence:" + family;
    observation.snapshot_id = "metric-snapshot:" + family;
    observations.push_back(std::move(observation));
  }
  return observations;
}

agents::AgentActionContractEvaluationRequest ValidLiveRequest(
    const agents::AgentActionContractDescriptor& contract,
    agents::AgentPolicy* policy) {
  agents::AgentActionContractEvaluationRequest request;
  request.context = Context();
  request.policy = policy;
  request.policy_present = true;
  request.policy_gate_present = true;
  request.evidence_store_available = true;
  request.live_prerequisites_enabled = true;
  request.actuator_route_available = true;
  request.actuator_route_id = contract.actuator;
  request.available_metric_families = contract.metric_families;
  request.enforce_metric_observation_dependencies = true;
  request.metric_observations = ObservationsFor(contract);
  request.resource_budget = AllowingBudget(*policy);
  request.arbitration_passed = true;
  request.arbitration_evidence_present = true;
  return request;
}

agents::AgentActionContractDescriptor Contract(
    const std::string& owning_agent,
    const std::string& action_id) {
  const auto contract = agents::FindAgentActionContract(owning_agent, action_id);
  Require(contract.has_value(), "missing contract: " + owning_agent + ":" + action_id);
  return *contract;
}

void TestAuthorityRegistryCoverage() {
  const auto status = agents::ValidateAgentActuatorAuthorityRegistry();
  Require(status.ok, "authority registry did not validate: " + status.diagnostic_code);

  int covered = 0;
  for (const auto& contract : agents::AgentActionContractRegistry()) {
    const auto authority =
        agents::FindAgentActuatorAuthority(contract.owning_agent, contract.action_id);
    Require(authority.has_value(),
            "authority missing for " + contract.owning_agent + ":" +
                contract.action_id);
    Require(authority->actuator_id == contract.actuator,
            "authority actuator mismatch for " + contract.owning_agent + ":" +
                contract.action_id);
    Require(authority->route_registered,
            "authority route not registered for " + contract.owning_agent + ":" +
                contract.action_id);
    Require(!authority->owns_forbidden_engine_authority,
            "authority row directly owns forbidden engine authority: " +
                authority->actuator_id);
    ++covered;
  }
  Require(covered > 0, "no non-cluster contracts covered");

  const auto cluster = agents::FindAgentActuatorAuthority(
      "cluster_scheduler_manager", "route_cluster_job");
  Require(cluster.has_value(), "representative cluster authority row missing");
  Require(cluster->cluster_scoped, "cluster authority row not marked cluster scoped");
  Require(cluster->actuator_id == "scheduler",
          "cluster scheduler authority actuator mismatch");
}

void TestValidLiveRouteAndDryRunOrder() {
  auto contract = Contract("page_allocation_manager", "preallocate_page_family");
  auto policy = PolicyFor(contract);
  auto request = ValidLiveRequest(contract, &policy);
  const auto live = agents::EvaluateAgentActionContract(contract, request);
  Require(live.diagnostic_code ==
              "SB_AGENT_ACTION_CONTRACT.LIVE_DISPATCH_READY",
          "valid actuator route did not reach live dispatch admission: " +
              live.diagnostic_code);
  Require(!live.mutates_state, "live route authority check mutated state");

  request.live_prerequisites_enabled = false;
  request.actuator_route_available = false;
  request.actuator_route_id = "memory_allocator";
  const auto dry_run = agents::EvaluateAgentActionContract(contract, request);
  Require(dry_run.diagnostic_code == "SB_AGENT_ACTION_CONTRACT.DRY_RUN_ONLY",
          "dry-run behavior did not precede live route checks: " +
              dry_run.diagnostic_code);
}

void TestWrongUnknownAndForbiddenActuatorsDenied() {
  auto contract = Contract("page_allocation_manager", "preallocate_page_family");
  auto policy = PolicyFor(contract);
  auto request = ValidLiveRequest(contract, &policy);

  request.actuator_route_id = "memory_allocator";
  const auto wrong = agents::EvaluateAgentActionContract(contract, request);
  Require(wrong.diagnostic_code ==
              "SB_AGENT_ACTION_ACTUATOR_AUTHORITY.ACTUATOR_MISMATCH",
          "wrong actuator diagnostic mismatch: " + wrong.diagnostic_code);

  request.actuator_route_id = "unregistered_actuator";
  const auto unknown = agents::EvaluateAgentActionContract(contract, request);
  Require(unknown.diagnostic_code ==
              "SB_AGENT_ACTION_ACTUATOR_AUTHORITY.ACTUATOR_UNKNOWN",
          "unknown actuator diagnostic mismatch: " + unknown.diagnostic_code);

  request.actuator_route_id = "transaction_finality";
  const auto forbidden = agents::EvaluateAgentActionContract(contract, request);
  Require(forbidden.diagnostic_code ==
              "SB_AGENT_ACTION_ACTUATOR_AUTHORITY.FORBIDDEN_DIRECT_ENGINE_AUTHORITY",
          "forbidden direct authority diagnostic mismatch: " +
              forbidden.diagnostic_code);
}

void TestPrerequisitesDenyBeforeRoute() {
  auto contract = Contract("page_allocation_manager", "preallocate_page_family");
  auto policy = PolicyFor(contract);

  auto evidence = ValidLiveRequest(contract, &policy);
  evidence.evidence_store_available = false;
  evidence.actuator_route_id = "memory_allocator";
  const auto evidence_denied =
      agents::EvaluateAgentActionContract(contract, evidence);
  Require(evidence_denied.diagnostic_code ==
              "SB_AGENT_ACTION_CONTRACT.EVIDENCE_REQUIRED",
          "evidence did not deny before route");

  auto security = ValidLiveRequest(contract, &policy);
  security.context.security_context_present = false;
  security.actuator_route_id = "memory_allocator";
  const auto security_denied =
      agents::EvaluateAgentActionContract(contract, security);
  Require(security_denied.diagnostic_code == "AGENT.SECURITY_CONTEXT_REQUIRED",
          "security did not deny before route");

  auto policy_request = ValidLiveRequest(contract, &policy);
  policy_request.policy = nullptr;
  policy_request.policy_present = false;
  policy_request.actuator_route_id = "memory_allocator";
  const auto policy_denied =
      agents::EvaluateAgentActionContract(contract, policy_request);
  Require(policy_denied.diagnostic_code ==
              "SB_AGENT_ACTION_CONTRACT.POLICY_REQUIRED",
          "policy did not deny before route");

  auto gate = ValidLiveRequest(contract, &policy);
  gate.policy_gate_present = false;
  gate.actuator_route_id = "memory_allocator";
  const auto gate_denied = agents::EvaluateAgentActionContract(contract, gate);
  Require(gate_denied.diagnostic_code ==
              "SB_AGENT_ACTION_CONTRACT.POLICY_GATE_REQUIRED",
          "policy gate did not deny before route");

  auto metrics = ValidLiveRequest(contract, &policy);
  metrics.enforce_metric_observation_dependencies = false;
  metrics.available_metric_families.clear();
  metrics.actuator_route_id = "memory_allocator";
  const auto metrics_denied =
      agents::EvaluateAgentActionContract(contract, metrics);
  Require(metrics_denied.diagnostic_code ==
              "SB_AGENT_ACTION_CONTRACT.METRIC_REQUIRED",
          "metrics did not deny before route");

  auto budget = ValidLiveRequest(contract, &policy);
  auto limited_budget = *budget.resource_budget;
  limited_budget.foreground_database_work_active = true;
  budget.resource_budget = limited_budget;
  budget.actuator_route_id = "memory_allocator";
  const auto budget_denied = agents::EvaluateAgentActionContract(contract, budget);
  Require(budget_denied.diagnostic_code ==
              "SB_AGENT_RESOURCE_BUDGET.FOREGROUND_PROTECTION",
          "resource budget did not deny before route");

  auto arbitration = ValidLiveRequest(contract, &policy);
  arbitration.arbitration_evidence_present = false;
  arbitration.actuator_route_id = "memory_allocator";
  const auto arbitration_denied =
      agents::EvaluateAgentActionContract(contract, arbitration);
  Require(arbitration_denied.diagnostic_code ==
              "SB_AGENT_ACTION_ACTUATOR_AUTHORITY.ARBITRATION_REQUIRED",
          "arbitration evidence did not deny before route");
}

void TestCanonicalContractAndClusterBoundary() {
  auto contract = Contract("page_allocation_manager", "preallocate_page_family");
  auto policy = PolicyFor(contract);
  auto request = ValidLiveRequest(contract, &policy);

  auto modified = contract;
  modified.actuator = "memory_allocator";
  request.actuator_route_id = "memory_allocator";
  const auto mismatch = agents::EvaluateAgentActionContract(modified, request);
  Require(mismatch.diagnostic_code ==
              "SB_AGENT_ACTION_ACTUATOR_AUTHORITY.CONTRACT_MISMATCH",
          "modified contract did not fail canonical authority check: " +
              mismatch.diagnostic_code);

  agents::AgentActionContractDescriptor cluster_contract;
  cluster_contract.owning_agent = "cluster_scheduler_manager";
  cluster_contract.action_id = "route_cluster_job";
  cluster_contract.actuator = "scheduler";
  cluster_contract.cluster_scoped = true;
  agents::AgentActionContractEvaluationRequest cluster_request;
  cluster_request.context = Context();
  cluster_request.actuator_route_available = true;
  cluster_request.actuator_route_id = "scheduler";
  const auto cluster =
      agents::EvaluateAgentActionActuatorAuthority(cluster_contract,
                                                   cluster_request);
  Require(!cluster.status.ok, "cluster authority accepted standalone route");
  Require(cluster.status.diagnostic_code == "SBLR.CLUSTER.SUPPORT_NOT_ENABLED",
          "cluster no-provider diagnostic mismatch: " +
              cluster.status.diagnostic_code);
}

}  // namespace

int main() {
  TestAuthorityRegistryCoverage();
  TestValidLiveRouteAndDryRunOrder();
  TestWrongUnknownAndForbiddenActuatorsDenied();
  TestPrerequisitesDenyBeforeRoute();
  TestCanonicalContractAndClusterBoundary();
  return EXIT_SUCCESS;
}
