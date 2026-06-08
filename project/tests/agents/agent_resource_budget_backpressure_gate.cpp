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

agents::AgentRuntimeContext Context() {
  agents::AgentRuntimeContext context;
  context.security_context_present = true;
  context.private_features_available = true;
  context.standalone_edition = true;
  context.database_uuid = "019f006d-0000-7000-8000-000000000001";
  context.principal_uuid = "019f006d-0000-7000-8000-000000000002";
  context.groups = {"ROOT", "OPS", "DBA"};
  context.rights = {
      "OBS_AGENT_STATE_READ",
      "OBS_AGENT_CONTROL",
      "OBS_AGENT_EVIDENCE_READ"};
  context.wall_now_microseconds = 1700000000000060ull;
  context.monotonic_now_microseconds = 1000000;
  return context;
}

agents::AgentMetricObservation Observation(
    const agents::AgentMetricDependency& dep) {
  agents::AgentMetricObservation observation;
  observation.metric_family = dep.metric_family;
  observation.namespace_path = dep.namespace_prefix;
  observation.age_microseconds = 1;
  observation.present = true;
  observation.trusted = true;
  observation.schema_compatible = true;
  observation.scope_compatible = true;
  observation.evidence_uuid = "metric-evidence:" + dep.metric_family;
  observation.snapshot_id = "metric-snapshot:" + dep.metric_family;
  return observation;
}

std::vector<agents::AgentMetricObservation> ObservationsFor(
    const agents::AgentTypeDescriptor& descriptor) {
  std::vector<agents::AgentMetricObservation> observations;
  for (const auto& dep : descriptor.metric_dependencies) {
    if (dep.cluster_only || !dep.required) { continue; }
    observations.push_back(Observation(dep));
  }
  return observations;
}

agents::AgentResourceBudgetEvaluationInput BudgetInput(
    const agents::AgentPolicy& policy) {
  agents::AgentResourceBudgetEvaluationInput input;
  input.budget = agents::DefaultAgentResourceBudgetForPolicy(policy);
  input.budget.max_cpu_time_microseconds = 100;
  input.budget.max_memory_bytes = 1000;
  input.budget.max_io_bytes = 1000;
  input.budget.max_io_ops = 10;
  input.budget.max_thread_slots = 1;
  input.budget.max_queue_depth = 2;
  input.budget.min_run_interval_microseconds = 100;
  input.budget.retry_backoff_microseconds = 200;
  input.budget.watchdog_timeout_microseconds = 500;
  input.budget.max_history_query_rows = policy.max_history_query_rows;
  input.budget.max_evidence_fanout = policy.max_evidence_fanout;
  input.budget.max_label_cardinality = policy.max_label_cardinality;
  input.usage.cpu_time_microseconds = 50;
  input.usage.memory_bytes = 500;
  input.usage.io_bytes = 500;
  input.usage.io_ops = 5;
  input.usage.thread_slots = 1;
  input.usage.queue_depth = 1;
  input.usage.runtime_microseconds = 50;
  input.usage.history_query_rows = 1;
  input.usage.evidence_fanout = 1;
  input.usage.label_cardinality = 1;
  return input;
}

void RequireDecision(
    const agents::AgentResourceBudgetDecision& decision,
    agents::AgentResourceBudgetDecisionKind kind,
    agents::AgentResourceBudgetDimension dimension,
    const std::string& code) {
  Require(decision.decision == kind,
          "resource budget decision mismatch: " +
              std::string(agents::AgentResourceBudgetDecisionKindName(
                  decision.decision)));
  Require(decision.status.diagnostic_code == code,
          "resource budget diagnostic mismatch: " +
              decision.status.diagnostic_code);
  Require(!decision.evidence_uuid.empty(), "missing budget evidence UUID");
  Require(!decision.diagnostics.empty(), "missing budget diagnostics");
  Require(decision.diagnostics.front().dimension == dimension,
          "budget diagnostic dimension mismatch");
  Require(decision.diagnostics.front().evidence_uuid == decision.evidence_uuid,
          "budget diagnostic evidence mismatch");
  Require(decision.action_allowed ==
              (kind == agents::AgentResourceBudgetDecisionKind::allow),
          "budget action_allowed flag mismatch");
  Require(decision.mutation_allowed ==
              (kind == agents::AgentResourceBudgetDecisionKind::allow),
          "budget mutation_allowed flag mismatch");
  Require(decision.failed_closed ==
              (kind == agents::AgentResourceBudgetDecisionKind::fail_closed),
          "budget failed_closed flag mismatch");
  Require(decision.diagnostics.front().suppresses_mutation ==
              (kind != agents::AgentResourceBudgetDecisionKind::allow),
          "budget suppresses_mutation flag mismatch");
  Require(decision.diagnostics.front().protects_foreground ==
              (dimension ==
               agents::AgentResourceBudgetDimension::foreground_protection),
          "budget foreground protection flag mismatch");
}

void TestBudgetEvaluatorDimensions() {
  const auto descriptor = agents::FindAgentType("page_allocation_manager");
  Require(descriptor.has_value(), "page_allocation_manager descriptor missing");
  auto policy = agents::BaselinePolicyForAgent(*descriptor);
  const auto context = Context();

  auto allowed = BudgetInput(policy);
  RequireDecision(
      agents::EvaluateAgentResourceBudget(*descriptor, policy, context, allowed),
      agents::AgentResourceBudgetDecisionKind::allow,
      agents::AgentResourceBudgetDimension::foreground_protection,
      "SB_AGENT_RESOURCE_BUDGET.ALLOW");

  auto foreground = allowed;
  foreground.foreground_database_work_active = true;
  RequireDecision(
      agents::EvaluateAgentResourceBudget(*descriptor, policy, context, foreground),
      agents::AgentResourceBudgetDecisionKind::foreground_protection,
      agents::AgentResourceBudgetDimension::foreground_protection,
      "SB_AGENT_RESOURCE_BUDGET.FOREGROUND_PROTECTION");

  auto cpu = allowed;
  cpu.usage.cpu_time_microseconds = 101;
  RequireDecision(
      agents::EvaluateAgentResourceBudget(*descriptor, policy, context, cpu),
      agents::AgentResourceBudgetDecisionKind::throttle_defer,
      agents::AgentResourceBudgetDimension::cpu_time,
      "SB_AGENT_RESOURCE_BUDGET.CPU_TIME_EXCEEDED");

  auto memory = allowed;
  memory.usage.memory_bytes = 1001;
  RequireDecision(
      agents::EvaluateAgentResourceBudget(*descriptor, policy, context, memory),
      agents::AgentResourceBudgetDecisionKind::shed_refuse,
      agents::AgentResourceBudgetDimension::memory_bytes,
      "SB_AGENT_RESOURCE_BUDGET.MEMORY_BYTES_EXCEEDED");

  auto io_bytes = allowed;
  io_bytes.usage.io_bytes = 1001;
  RequireDecision(
      agents::EvaluateAgentResourceBudget(*descriptor, policy, context, io_bytes),
      agents::AgentResourceBudgetDecisionKind::throttle_defer,
      agents::AgentResourceBudgetDimension::io_bytes,
      "SB_AGENT_RESOURCE_BUDGET.IO_BYTES_EXCEEDED");

  auto io_ops = allowed;
  io_ops.usage.io_ops = 11;
  RequireDecision(
      agents::EvaluateAgentResourceBudget(*descriptor, policy, context, io_ops),
      agents::AgentResourceBudgetDecisionKind::throttle_defer,
      agents::AgentResourceBudgetDimension::io_ops,
      "SB_AGENT_RESOURCE_BUDGET.IO_OPS_EXCEEDED");

  auto threads = allowed;
  threads.usage.thread_slots = 2;
  RequireDecision(
      agents::EvaluateAgentResourceBudget(*descriptor, policy, context, threads),
      agents::AgentResourceBudgetDecisionKind::shed_refuse,
      agents::AgentResourceBudgetDimension::thread_slots,
      "SB_AGENT_RESOURCE_BUDGET.THREAD_SLOTS_EXHAUSTED");

  auto queue = allowed;
  queue.usage.queue_depth = 3;
  RequireDecision(
      agents::EvaluateAgentResourceBudget(*descriptor, policy, context, queue),
      agents::AgentResourceBudgetDecisionKind::shed_refuse,
      agents::AgentResourceBudgetDimension::queue_depth,
      "SB_AGENT_RESOURCE_BUDGET.QUEUE_DEPTH_EXCEEDED");

  auto cadence = allowed;
  cadence.usage.last_run_end_microseconds = context.monotonic_now_microseconds - 50;
  RequireDecision(
      agents::EvaluateAgentResourceBudget(*descriptor, policy, context, cadence),
      agents::AgentResourceBudgetDecisionKind::throttle_defer,
      agents::AgentResourceBudgetDimension::cadence,
      "SB_AGENT_RESOURCE_BUDGET.CADENCE_NOT_DUE");

  auto retry = allowed;
  retry.usage.last_failure_microseconds =
      context.monotonic_now_microseconds - 100;
  RequireDecision(
      agents::EvaluateAgentResourceBudget(*descriptor, policy, context, retry),
      agents::AgentResourceBudgetDecisionKind::throttle_defer,
      agents::AgentResourceBudgetDimension::retry_backoff,
      "SB_AGENT_RESOURCE_BUDGET.RETRY_BACKOFF_ACTIVE");

  auto timeout = allowed;
  timeout.usage.runtime_microseconds = 501;
  RequireDecision(
      agents::EvaluateAgentResourceBudget(*descriptor, policy, context, timeout),
      agents::AgentResourceBudgetDecisionKind::fail_closed,
      agents::AgentResourceBudgetDimension::runtime_timeout,
      "SB_AGENT_RESOURCE_BUDGET.RUNTIME_TIMEOUT_EXCEEDED");

  auto cancel = allowed;
  cancel.cancellation_requested = true;
  RequireDecision(
      agents::EvaluateAgentResourceBudget(*descriptor, policy, context, cancel),
      agents::AgentResourceBudgetDecisionKind::cancel_drain,
      agents::AgentResourceBudgetDimension::cancellation_drain,
      "SB_AGENT_RESOURCE_BUDGET.CANCEL_DRAIN_REQUESTED");

  auto history = allowed;
  history.usage.history_query_rows = policy.max_history_query_rows + 1;
  RequireDecision(
      agents::EvaluateAgentResourceBudget(*descriptor, policy, context, history),
      agents::AgentResourceBudgetDecisionKind::fail_closed,
      agents::AgentResourceBudgetDimension::history_rows,
      "SB_AGENT_RESOURCE_BUDGET.HISTORY_ROWS_EXCEEDED");

  auto fanout = allowed;
  fanout.usage.evidence_fanout = policy.max_evidence_fanout + 1;
  RequireDecision(
      agents::EvaluateAgentResourceBudget(*descriptor, policy, context, fanout),
      agents::AgentResourceBudgetDecisionKind::fail_closed,
      agents::AgentResourceBudgetDimension::evidence_fanout,
      "SB_AGENT_RESOURCE_BUDGET.EVIDENCE_FANOUT_EXCEEDED");

  auto cardinality = allowed;
  cardinality.usage.label_cardinality = policy.max_label_cardinality + 1;
  RequireDecision(
      agents::EvaluateAgentResourceBudget(*descriptor, policy, context, cardinality),
      agents::AgentResourceBudgetDecisionKind::fail_closed,
      agents::AgentResourceBudgetDimension::label_cardinality,
      "SB_AGENT_RESOURCE_BUDGET.LABEL_CARDINALITY_EXCEEDED");
}

void TestPolicyConfigOverridesDefaultBudget() {
  const auto descriptor = agents::FindAgentType("page_allocation_manager");
  Require(descriptor.has_value(), "page_allocation_manager descriptor missing");
  auto policy = agents::BaselinePolicyForAgent(*descriptor);
  policy.config_fields["protect_foreground_work"] = "false";
  policy.config_fields["max_cpu_time_microseconds"] = "11";
  policy.config_fields["max_memory_bytes"] = "22";
  policy.config_fields["max_io_bytes"] = "33";
  policy.config_fields["max_io_ops"] = "44";
  policy.config_fields["max_thread_slots"] = "2";
  policy.config_fields["max_queue_depth"] = "3";
  policy.config_fields["min_run_interval_microseconds"] = "55";
  policy.config_fields["retry_backoff_microseconds"] = "66";
  policy.config_fields["watchdog_timeout_microseconds"] = "77";
  policy.config_fields["max_history_query_rows"] = "88";
  policy.config_fields["max_evidence_fanout"] = "99";
  policy.config_fields["max_label_cardinality"] = "111";

  const auto budget = agents::DefaultAgentResourceBudgetForPolicy(policy);
  Require(!budget.protect_foreground_work,
          "foreground protection config override ignored");
  Require(budget.max_cpu_time_microseconds == 11,
          "CPU config override ignored");
  Require(budget.max_memory_bytes == 22,
          "memory config override ignored");
  Require(budget.max_io_bytes == 33, "IO bytes config override ignored");
  Require(budget.max_io_ops == 44, "IO ops config override ignored");
  Require(budget.max_thread_slots == 2, "thread config override ignored");
  Require(budget.max_queue_depth == 3, "queue config override ignored");
  Require(budget.min_run_interval_microseconds == 55,
          "cadence config override ignored");
  Require(budget.retry_backoff_microseconds == 66,
          "retry config override ignored");
  Require(budget.watchdog_timeout_microseconds == 77,
          "watchdog config override ignored");
  Require(budget.max_history_query_rows == 88,
          "history config override ignored");
  Require(budget.max_evidence_fanout == 99,
          "evidence fanout config override ignored");
  Require(budget.max_label_cardinality == 111,
          "label cardinality config override ignored");
}

void TestTickHealthBudgetIntegration() {
  const auto descriptor = agents::FindAgentType("page_allocation_manager");
  Require(descriptor.has_value(), "page_allocation_manager descriptor missing");
  auto policy = agents::BaselinePolicyForAgent(*descriptor);
  policy.policy_generation = 6;

  auto request = agents::AgentTickHealthRequest{};
  request.context = Context();
  request.policy_generation = 6;
  request.use_explicit_policy_state = true;
  request.enforce_metric_observation_dependencies = true;
  request.policies.push_back(policy);
  request.metric_observations = ObservationsFor(*descriptor);

  auto budget = BudgetInput(policy);
  budget.usage.queue_depth = budget.budget.max_queue_depth + 1;
  request.resource_budget = budget;

  const auto result = agents::BuildNonClusterAgentTickHealthSnapshot(request);
  Require(result.status.ok, "budget tick/health snapshot failed");
  bool found = false;
  for (const auto& record : result.records) {
    if (record.agent_type_id != descriptor->type_id) { continue; }
    found = true;
    Require(record.resource_budget_limited,
            "tick/health did not mark resource budget limit");
    Require(record.tick_class == agents::AgentTickHealthClass::suppressed,
            "queue pressure did not suppress tick/health action");
    Require(record.action_result_class == agents::AgentActionResultClass::refused,
            "queue pressure did not refuse action evidence");
    Require(record.diagnostic_code ==
                "SB_AGENT_RESOURCE_BUDGET.QUEUE_DEPTH_EXCEEDED",
            "tick/health budget diagnostic mismatch");
    Require(!record.resource_budget_diagnostics.empty(),
            "tick/health budget diagnostics missing");
  }
  Require(found, "page_allocation_manager budget record missing");
}

void TestActionContractBudgetIntegration() {
  const auto contract = agents::FindAgentActionContract(
      "page_allocation_manager", "preallocate_page_family");
  Require(contract.has_value(), "preallocate_page_family contract missing");
  const auto descriptor = agents::FindAgentType(contract->owning_agent);
  Require(descriptor.has_value(), "contract owner descriptor missing");
  auto policy = agents::BaselinePolicyForAgentFamily(
      *descriptor, "page_preallocation_policy", 6);

  agents::AgentActionContractEvaluationRequest request;
  request.context = Context();
  request.policy = &policy;
  request.policy_present = true;
  request.policy_gate_present = true;
  request.evidence_store_available = true;
  request.live_prerequisites_enabled = true;
  request.actuator_route_available = true;
  request.available_metric_families = contract->metric_families;

  auto budget = BudgetInput(policy);
  budget.foreground_database_work_active = true;
  request.resource_budget = budget;
  const auto foreground =
      agents::EvaluateAgentActionContract(*contract, request);
  Require(foreground.result_class == agents::AgentActionResultClass::refused,
          "foreground protection did not refuse action contract");
  Require(foreground.diagnostic_code ==
              "SB_AGENT_RESOURCE_BUDGET.FOREGROUND_PROTECTION",
          "foreground protection diagnostic mismatch");
  Require(!foreground.mutates_state,
          "foreground protection allowed mutation");

  budget = BudgetInput(policy);
  budget.usage.runtime_microseconds =
      budget.budget.watchdog_timeout_microseconds + 1;
  request.resource_budget = budget;
  const auto timeout = agents::EvaluateAgentActionContract(*contract, request);
  Require(timeout.result_class == agents::AgentActionResultClass::failed_closed,
          "watchdog timeout did not fail closed");
  Require(timeout.diagnostic_code ==
              "SB_AGENT_RESOURCE_BUDGET.RUNTIME_TIMEOUT_EXCEEDED",
          "watchdog timeout diagnostic mismatch");
  Require(!timeout.mutates_state, "watchdog timeout allowed mutation");
}

}  // namespace

int main() {
  TestBudgetEvaluatorDimensions();
  TestPolicyConfigOverridesDefaultBudget();
  TestTickHealthBudgetIntegration();
  TestActionContractBudgetIntegration();
  return EXIT_SUCCESS;
}
