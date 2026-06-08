// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "agent_execution_lane_governance.hpp"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

namespace agents = scratchbird::core::agents;

constexpr agents::u64 kNowMicros = 4000000000ull;

[[noreturn]] void Fail(const std::string& message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, const std::string& message) {
  if (!condition) { Fail(message); }
}

const agents::AgentTypeDescriptor& Descriptor(const std::string& id) {
  const auto descriptor = agents::FindAgentType(id);
  Require(descriptor.has_value(), "missing descriptor: " + id);
  static agents::AgentTypeDescriptor storage;
  storage = *descriptor;
  return storage;
}

agents::AgentProductionRouteProofInputs::RouteProof PageProof() {
  agents::AgentProductionRouteProofInputs::RouteProof proof;
  proof.agent_type_id = "page_allocation_manager";
  proof.action_id = "preallocate_page_family";
  proof.provider_id = "page_manager:preallocate_page_family";
  proof.actuator_id = "page_manager";
  proof.authority_domain = agents::AgentActuatorAuthorityDomain::page;
  proof.subsystem_handler_id = "storage.page.preallocate_page_family";
  proof.handler_provenance = "storage_page_preallocation_route";
  proof.handler_evidence_uuid = "018f0000-0000-7000-8000-000000073001";
  proof.live_route_available = true;
  proof.real_subsystem_handler = true;
  proof.idempotent = true;
  proof.supports_retry = true;
  proof.supports_rollback_compensation = true;
  proof.requires_outcome_verification = true;
  proof.physical_mutation_route = true;
  return proof;
}

agents::AgentSystemProfile ProductionProfile(
    const std::string& agent_type_id,
    agents::AgentSystemProfileMetricStrictness strictness =
        agents::AgentSystemProfileMetricStrictness::strict) {
  agents::AgentSystemProfile profile;
  profile.agent_type_id = agent_type_id;
  profile.live_enablement =
      agents::AgentSystemProfileLiveEnablement::production_live;
  profile.public_claim_level =
      agents::AgentSystemProfilePublicClaimLevel::production_live;
  profile.fail_mode = agents::AgentSystemProfileFailMode::fail_closed;
  profile.metric_strictness = strictness;
  profile.durable_profile_evidence_present = true;
  profile.durable_profile_evidence_uuid =
      "018f0000-0000-7000-8000-000000073010";
  profile.durable_profile_storage_digest = "profile-storage-digest";
  profile.evidence_required = true;
  profile.approval_required = true;
  profile.redaction_required = true;
  profile.retention_required = true;
  profile.evidence_policy_id = "agent-system-profile-evidence-policy-v1";
  profile.approval_policy_id = "agent-system-profile-approval-policy-v1";
  profile.redaction_class = "standard";
  profile.retention_class = "audit";
  profile.key_policy_id = "agent-system-profile-key-policy-v1";
  profile.key_policy_provenance = "engine_local_profile_key_policy";
  profile.key_policy_generation = 1;
  profile.signing_key_id = "agent-system-profile-key-v1";
  profile.signing_key_provenance = "engine_local_profile_hmac_key";
  profile.signing_key_generation = 1;
  profile.profile_generation = 9;
  profile.issued_at_microseconds = kNowMicros - 1000;
  profile.expires_at_microseconds = kNowMicros + 5000000;
  profile.max_staleness_microseconds = 1000000;
  agents::FinalizeAgentSystemProfile(&profile);
  return profile;
}

agents::AgentSystemProfileValidationContext ProfileContext() {
  agents::AgentSystemProfileValidationContext context;
  context.now_microseconds = kNowMicros;
  context.route_inputs.route_proofs.push_back(PageProof());
  return context;
}

agents::AgentPolicy PolicyFor(const std::string& agent_type_id,
                              const std::string& family,
                              agents::u64 generation,
                              bool live) {
  agents::AgentPolicy policy = agents::BaselinePolicyForAgentFamily(
      Descriptor(agent_type_id), family, generation);
  if (live) {
    policy.activation = agents::AgentActivationProfile::live_action;
    policy.allow_live_action = true;
    policy.action_mode = "request_action";
    policy.require_manual_approval = true;
    policy.require_dry_run_before_live = true;
  } else {
    policy.activation = agents::AgentActivationProfile::recommend_only;
    policy.allow_live_action = false;
    policy.action_mode = "recommend_only";
  }
  return policy;
}

agents::AgentRuntimeContext RuntimeContext() {
  agents::AgentRuntimeContext context;
  context.security_context_present = true;
  context.private_features_available = true;
  context.standalone_edition = true;
  context.principal_uuid = "018f0000-0000-7000-8000-000000073020";
  context.database_uuid = "018f0000-0000-7000-8000-000000073021";
  context.monotonic_now_microseconds = kNowMicros;
  context.wall_now_microseconds = kNowMicros;
  return context;
}

agents::AgentResourceBudgetEvaluationInput Budget(bool foreground = false) {
  agents::AgentResourceBudgetEvaluationInput input;
  input.budget.protect_foreground_work = true;
  input.budget.max_cpu_time_microseconds = 1000000;
  input.budget.max_memory_bytes = 1024 * 1024;
  input.budget.max_io_bytes = 1024 * 1024;
  input.budget.max_io_ops = 1024;
  input.budget.max_thread_slots = 2;
  input.budget.max_queue_depth = 8;
  input.budget.min_run_interval_microseconds = 1;
  input.budget.retry_backoff_microseconds = 1;
  input.budget.watchdog_timeout_microseconds = 2000000;
  input.budget.max_history_query_rows = 32;
  input.budget.max_evidence_fanout = 16;
  input.budget.max_label_cardinality = 16;
  input.usage.thread_slots = 1;
  input.usage.queue_depth = 1;
  input.foreground_database_work_active = foreground;
  return input;
}

agents::AgentExecutionLaneRequest Request(
    const std::string& agent_type_id,
    const std::string& policy_family,
    agents::AgentExecutionLaneKind lane,
    bool live) {
  agents::AgentExecutionLaneRequest request;
  request.action_id = live ? "preallocate_page_family" : "recommend";
  request.descriptor = Descriptor(agent_type_id);
  request.policy = PolicyFor(agent_type_id, policy_family, 9, live);
  request.runtime_context = RuntimeContext();
  request.system_profile = ProductionProfile(agent_type_id);
  request.profile_context = ProfileContext();
  request.lane_policy = agents::DefaultAgentExecutionLanePolicy(lane);
  request.resource_budget = Budget();
  request.resource_budget_evidence_present = true;
  request.production_environment = true;
  return request;
}

void RequireOk(const agents::AgentExecutionLaneDecision& decision,
               const std::string& message) {
  Require(decision.status.ok,
          message + ": " + decision.status.diagnostic_code + " " +
              decision.status.detail);
  Require(decision.admitted, "not admitted: " + message);
  Require(decision.lane_valid, "lane invalid: " + message);
  Require(decision.slo_valid, "SLO invalid: " + message);
  Require(decision.cost_governance_valid, "cost invalid: " + message);
  Require(decision.resource_budget_valid, "budget invalid: " + message);
  Require(decision.authority_clean, "authority invalid: " + message);
}

void RequireCode(const agents::AgentExecutionLaneRequest& request,
                 const std::string& expected,
                 const std::string& message) {
  const auto decision = agents::EvaluateAgentExecutionLaneAdmission(request);
  Require(!decision.status.ok, "unexpected admission pass: " + message);
  Require(decision.status.diagnostic_code == expected,
          message + ": expected " + expected + " got " +
              decision.status.diagnostic_code + " " + decision.status.detail);
}

void TestPositiveAdmissions() {
  RequireOk(agents::EvaluateAgentExecutionLaneAdmission(Request(
                "page_allocation_manager", "page_preallocation_policy",
                agents::AgentExecutionLaneKind::storage_maintenance, true)),
            "production live storage maintenance");

  RequireOk(agents::EvaluateAgentExecutionLaneAdmission(Request(
                "runtime_learning_agent", "optimizer_learning_policy",
                agents::AgentExecutionLaneKind::optimizer_advisory, false)),
            "optimizer advisory");

  RequireOk(agents::EvaluateAgentExecutionLaneAdmission(Request(
                "support_bundle_triage_agent", "support_bundle_policy",
                agents::AgentExecutionLaneKind::support_observability, false)),
            "support observability");

  RequireOk(agents::EvaluateAgentExecutionLaneAdmission(Request(
                "policy_recommendation_manager", "policy_recommendation_policy",
                agents::AgentExecutionLaneKind::low_priority_background, false)),
            "low priority background");
}

void TestNegativeAdmissions() {
  auto unknown = Request("page_allocation_manager", "page_preallocation_policy",
                         agents::AgentExecutionLaneKind::storage_maintenance,
                         true);
  unknown.lane_policy.lane = agents::AgentExecutionLaneKind::unknown;
  RequireCode(unknown, "SB_AGENT_EXECUTION_LANE.UNKNOWN_LANE",
              "unknown lane");

  auto disabled = Request("page_allocation_manager", "page_preallocation_policy",
                          agents::AgentExecutionLaneKind::storage_maintenance,
                          true);
  disabled.lane_policy.enabled = false;
  RequireCode(disabled, "SB_AGENT_EXECUTION_LANE.DISABLED",
              "disabled lane");

  auto live_in_advisory = Request(
      "page_allocation_manager", "page_preallocation_policy",
      agents::AgentExecutionLaneKind::optimizer_advisory, true);
  RequireCode(live_in_advisory,
              "SB_AGENT_EXECUTION_LANE.LIVE_ACTION_FORBIDDEN",
              "live in advisory-only lane");

  auto missing_cost = Request(
      "page_allocation_manager", "page_preallocation_policy",
      agents::AgentExecutionLaneKind::storage_maintenance, true);
  missing_cost.lane_policy.cost_center.clear();
  RequireCode(missing_cost, "SB_AGENT_EXECUTION_LANE.COST_CENTER_REQUIRED",
              "missing cost center");

  auto missing_tag = Request(
      "page_allocation_manager", "page_preallocation_policy",
      agents::AgentExecutionLaneKind::storage_maintenance, true);
  missing_tag.lane_policy.chargeback_tags.clear();
  RequireCode(missing_tag, "SB_AGENT_EXECUTION_LANE.CHARGEBACK_TAG_REQUIRED",
              "missing chargeback tag");

  auto queue = Request("page_allocation_manager", "page_preallocation_policy",
                       agents::AgentExecutionLaneKind::storage_maintenance,
                       true);
  queue.lane_policy.current_queue_depth = queue.lane_policy.max_queue_depth + 1;
  RequireCode(queue, "SB_AGENT_EXECUTION_LANE.QUEUE_DEPTH_EXCEEDED",
              "queue depth");

  auto concurrency = Request(
      "page_allocation_manager", "page_preallocation_policy",
      agents::AgentExecutionLaneKind::storage_maintenance, true);
  concurrency.lane_policy.active_actions =
      concurrency.lane_policy.max_concurrent_actions + 1;
  RequireCode(concurrency, "SB_AGENT_EXECUTION_LANE.CONCURRENCY_EXCEEDED",
              "concurrency");

  auto schedule = Request("page_allocation_manager", "page_preallocation_policy",
                          agents::AgentExecutionLaneKind::storage_maintenance,
                          true);
  schedule.lane_policy.schedule_latency_microseconds =
      schedule.lane_policy.max_schedule_latency_microseconds + 1;
  RequireCode(schedule, "SB_AGENT_EXECUTION_LANE.SCHEDULE_SLO_EXCEEDED",
              "schedule latency");

  auto action = Request("page_allocation_manager", "page_preallocation_policy",
                        agents::AgentExecutionLaneKind::storage_maintenance,
                        true);
  action.lane_policy.action_latency_microseconds =
      action.lane_policy.max_action_latency_microseconds + 1;
  RequireCode(action, "SB_AGENT_EXECUTION_LANE.ACTION_SLO_EXCEEDED",
              "action latency");

  auto evidence = Request(
      "page_allocation_manager", "page_preallocation_policy",
      agents::AgentExecutionLaneKind::storage_maintenance, true);
  evidence.lane_policy.evidence_latency_microseconds =
      evidence.lane_policy.max_evidence_latency_microseconds + 1;
  RequireCode(evidence, "SB_AGENT_EXECUTION_LANE.EVIDENCE_SLO_EXCEEDED",
              "evidence latency");

  auto heartbeat = Request(
      "page_allocation_manager", "page_preallocation_policy",
      agents::AgentExecutionLaneKind::storage_maintenance, true);
  heartbeat.lane_policy.heartbeat_latency_microseconds =
      heartbeat.lane_policy.max_heartbeat_latency_microseconds + 1;
  RequireCode(heartbeat, "SB_AGENT_EXECUTION_LANE.HEARTBEAT_SLO_EXCEEDED",
              "heartbeat latency");

  auto foreground = Request(
      "page_allocation_manager", "page_preallocation_policy",
      agents::AgentExecutionLaneKind::storage_maintenance, true);
  foreground.lane_policy.foreground_database_work_active = true;
  foreground.resource_budget = Budget(true);
  RequireCode(foreground, "SB_AGENT_EXECUTION_LANE.RESOURCE_BUDGET_REFUSED",
              "foreground protection");

  auto local_cluster = Request(
      "page_allocation_manager", "page_preallocation_policy",
      agents::AgentExecutionLaneKind::storage_maintenance, true);
  local_cluster.lane_policy.local_cluster_lane_claim = true;
  RequireCode(local_cluster, "SB_AGENT_EXECUTION_LANE.LOCAL_CLUSTER_FORBIDDEN",
              "local cluster claim");

  auto authority = Request(
      "page_allocation_manager", "page_preallocation_policy",
      agents::AgentExecutionLaneKind::storage_maintenance, true);
  authority.no_authority.agent_action_authority = true;
  RequireCode(authority, "SB_AGENT_EXECUTION_LANE.FORBIDDEN_AUTHORITY",
              "forbidden authority");

  auto missing_budget = Request(
      "page_allocation_manager", "page_preallocation_policy",
      agents::AgentExecutionLaneKind::storage_maintenance, true);
  missing_budget.resource_budget_evidence_present = false;
  RequireCode(missing_budget, "SB_AGENT_EXECUTION_LANE.RESOURCE_BUDGET_REQUIRED",
              "missing resource budget");

  auto relaxed_profile = Request(
      "page_allocation_manager", "page_preallocation_policy",
      agents::AgentExecutionLaneKind::storage_maintenance, true);
  relaxed_profile.system_profile = ProductionProfile(
      "page_allocation_manager",
      agents::AgentSystemProfileMetricStrictness::relaxed);
  RequireCode(relaxed_profile, "SB_AGENT_EXECUTION_LANE.PROFILE_INVALID",
              "relaxed production profile");
}

}  // namespace

int main() {
  TestPositiveAdmissions();
  TestNegativeAdmissions();
  return EXIT_SUCCESS;
}
