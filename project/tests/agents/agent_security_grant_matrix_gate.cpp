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

agents::AgentRuntimeContext Context(std::vector<std::string> rights = {},
                                    std::vector<std::string> groups = {}) {
  agents::AgentRuntimeContext context;
  context.security_context_present = true;
  context.private_features_available = true;
  context.standalone_edition = true;
  context.database_uuid = "019f006e-0000-7000-8000-000000000001";
  context.principal_uuid = "019f006e-0000-7000-8000-000000000002";
  context.groups = std::move(groups);
  context.rights = std::move(rights);
  context.wall_now_microseconds = 1700000000000060ull;
  context.monotonic_now_microseconds = 6000;
  return context;
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

std::vector<std::string> MetricFamiliesFor(
    const agents::AgentActionContractDescriptor& contract) {
  return contract.metric_families;
}

agents::AgentActionContractEvaluationRequest RequestFor(
    const agents::AgentActionContractDescriptor& contract,
    const agents::AgentRuntimeContext& context,
    agents::AgentPolicy* policy) {
  auto request = agents::AgentActionContractEvaluationRequest{};
  request.context = context;
  request.policy = policy;
  request.policy_present = true;
  request.policy_gate_present = true;
  request.evidence_store_available = true;
  request.available_metric_families = MetricFamiliesFor(contract);
  return request;
}

agents::AgentPolicy PolicyFor(const agents::AgentActionContractDescriptor& contract) {
  const auto owner = agents::FindAgentType(contract.owning_agent);
  Require(owner.has_value(), "contract owner missing: " + contract.owning_agent);
  return agents::BaselinePolicyForAgentFamily(
      *owner, PolicyFamilyForGate(contract.policy_gate), 1);
}

const agents::AgentActionContractDescriptor& Contract(
    const std::string& owning_agent,
    const std::string& action_id) {
  static std::vector<agents::AgentActionContractDescriptor> contracts;
  contracts = agents::AgentActionContractRegistry();
  for (const auto& contract : contracts) {
    if (contract.owning_agent == owning_agent && contract.action_id == action_id) {
      return contract;
    }
  }
  Fail("missing contract: " + owning_agent + ":" + action_id);
}

void TestGroupAndExplicitRightMapping() {
  Require(!agents::AgentContextHasRight(Context({}, {"PUBLIC"}),
                                        "OBS_AGENT_STATE_READ"),
          "PUBLIC received agent state right by group default");
  Require(!agents::AgentContextHasRight(Context({}, {"APP"}),
                                        "OBS_AGENT_CONTROL"),
          "APP received operational control by group default");
  Require(agents::AgentContextHasRight(Context({"OBS_AGENT_CONTROL"}, {"APP"}),
                                       "OBS_AGENT_CONTROL"),
          "explicit APP right was not accepted as authority");
  Require(agents::AgentContextHasRight(Context({}, {"SUP"}),
                                       "OBS_SUPPORT_BUNDLE_READ"),
          "SUP support-bundle read default missing");
  Require(!agents::AgentContextHasRight(Context({}, {"SEC"}),
                                        "OBS_AGENT_CONTROL"),
          "SEC received generic platform control by default");
  Require(agents::AgentContextHasRight(Context({}, {"SEC"}),
                                       "SEC_AUTH_METRICS_READ"),
          "SEC security metric default missing");
  Require(agents::AgentContextHasRight(Context({}, {"OPS"}),
                                       "OBS_CLUSTER_CONTROL"),
          "OPS cluster control default missing");
}

void TestCommandFamilyRequiredRights() {
  auto control = agents::EvaluateAgentCommandGrant(
      Context({"OBS_AGENT_STATE_READ"}), agents::AgentSecurityCommandFamily::control,
      "alter-agent");
  Require(!control.allowed, "control command allowed with only state read");
  Require(control.diagnostic_code == "AGENT.PERMISSION_DENIED",
          "control denial diagnostic mismatch: " + control.diagnostic_code);
  Require(control.missing_right == "OBS_AGENT_CONTROL",
          "control missing right mismatch: " + control.missing_right);

  auto approve = agents::EvaluateAgentCommandGrant(
      Context({"OBS_AGENT_CONTROL"}), agents::AgentSecurityCommandFamily::action_approve,
      "approve-action");
  Require(!approve.allowed, "action approve allowed with generic control");
  Require(approve.missing_right == "OBS_AGENT_ACTION_APPROVE",
          "action approve required right mismatch");

  auto policy_apply = agents::EvaluateAgentCommandGrant(
      Context({"OBS_AGENT_CONTROL"}),
      agents::AgentSecurityCommandFamily::policy_apply,
      "policy-apply");
  Require(!policy_apply.allowed, "policy apply allowed without policy apply right");
  Require(policy_apply.missing_right == "OBS_POLICY_APPLY",
          "policy apply missing right mismatch");

  auto cluster = agents::EvaluateAgentCommandGrant(
      Context({"OBS_CLUSTER_HEALTH_INSPECT", "OBS_AGENT_STATE_READ"}),
      agents::AgentSecurityCommandFamily::cluster_inspect,
      "cluster-inspect",
      false,
      true);
  Require(!cluster.allowed, "cluster inspect allowed without provider authority");
  Require(cluster.diagnostic_code == "CLUSTER.NOT_AVAILABLE",
          "cluster authority diagnostic mismatch");
}

void TestDenyWithoutActionRowLeakage() {
  auto no_context = Context();
  no_context.security_context_present = false;
  const auto grant = agents::EvaluateAgentCommandGrant(
      no_context,
      agents::AgentSecurityCommandFamily::recommendation_read,
      "show-actions");
  Require(!grant.allowed, "missing security context allowed action read");
  Require(grant.diagnostic_code == "AGENT.SECURITY_CONTEXT_REQUIRED",
          "missing context diagnostic mismatch");

  agents::AgentActionInspectionRecord action;
  action.action_uuid = "action-006e-hidden";
  action.action_id = "recommend_index_rebuild";
  action.owning_agent = "index_health_manager";
  action.actor_principal_uuid = "principal-secret";
  action.detail = "hidden recommendation";
  const auto redacted = agents::RedactAgentActionForSecurity(action, Context({}, {"APP"}));
  Require(!redacted.visible, "APP saw hidden action row");
  Require(redacted.redacted, "hidden action row was not marked redacted");
  Require(redacted.action.action_uuid.empty(), "hidden action uuid leaked");
  Require(redacted.grant.hides_candidate_rows, "hidden action row flag missing");
}

void TestActionContractPermissionEnforcement() {
  const auto& approve_contract =
      Contract("filespace_capacity_manager", "request_filespace_shrink");
  auto approve_policy = PolicyFor(approve_contract);
  auto approve_request = RequestFor(
      approve_contract,
      Context({"OBS_AGENT_CONTROL"}),
      &approve_policy);
  const auto approve_denied =
      agents::EvaluateAgentActionContract(approve_contract, approve_request);
  Require(approve_denied.result_class == agents::AgentActionResultClass::failed_closed,
          "approve contract did not fail closed");
  Require(approve_denied.diagnostic_code == "ACTION.PERMISSION_DENIED",
          "approve contract diagnostic mismatch: " + approve_denied.diagnostic_code);
  Require(approve_denied.detail == "OBS_AGENT_ACTION_APPROVE",
          "approve contract missing right mismatch: " + approve_denied.detail);

  const auto& expand_contract =
      Contract("filespace_capacity_manager", "request_filespace_expand");
  auto expand_policy = PolicyFor(expand_contract);
  auto expand_request = RequestFor(
      expand_contract,
      Context({"OBS_AGENT_ACTION_APPROVE"}),
      &expand_policy);
  const auto expand_denied =
      agents::EvaluateAgentActionContract(expand_contract, expand_request);
  Require(expand_denied.diagnostic_code == "ACTION.PERMISSION_DENIED",
          "filespace expand did not require external lifecycle right");
  Require(expand_denied.detail == "FILESPACE_LIFECYCLE_CONTROL",
          "filespace expand missing right mismatch: " + expand_denied.detail);

  expand_request.context = Context({"OBS_AGENT_ACTION_APPROVE",
                                    "FILESPACE_LIFECYCLE_CONTROL"});
  const auto expand_authorized =
      agents::EvaluateAgentActionContract(expand_contract, expand_request);
  Require(expand_authorized.diagnostic_code != "ACTION.PERMISSION_DENIED",
          "filespace lifecycle right did not satisfy expand permission");

  const auto& cancel_job_contract = Contract("job_control_manager", "cancel_job");
  auto cancel_job_policy = PolicyFor(cancel_job_contract);
  cancel_job_policy.activation = agents::AgentActivationProfile::live_action;
  cancel_job_policy.allow_live_action = true;
  auto cancel_job_request = RequestFor(
      cancel_job_contract,
      Context({"OBS_AGENT_ACTION_APPROVE"}),
      &cancel_job_policy);
  const auto cancel_job_denied =
      agents::EvaluateAgentActionContract(cancel_job_contract, cancel_job_request);
  Require(cancel_job_denied.diagnostic_code == "ACTION.PERMISSION_DENIED",
          "job cancel did not require job-domain right: " +
              cancel_job_denied.diagnostic_code + " / " + cancel_job_denied.detail);
  Require(cancel_job_denied.detail == "JOB_EXECUTE",
          "job cancel missing right mismatch: " + cancel_job_denied.detail);

  cancel_job_request.context = Context({"OBS_AGENT_ACTION_APPROVE", "JOB_ADMIN"});
  const auto cancel_job_authorized =
      agents::EvaluateAgentActionContract(cancel_job_contract, cancel_job_request);
  Require(cancel_job_authorized.diagnostic_code != "ACTION.PERMISSION_DENIED",
          "job admin alternative did not satisfy job cancel permission");

  const auto& recommendation_contract =
      Contract("index_health_manager", "recommend_index_rebuild");
  auto recommendation_policy = PolicyFor(recommendation_contract);
  auto recommendation_request = RequestFor(
      recommendation_contract,
      Context({"OBS_AGENT_STATE_READ"}),
      &recommendation_policy);
  const auto recommendation_denied =
      agents::EvaluateAgentActionContract(recommendation_contract,
                                          recommendation_request);
  Require(recommendation_denied.diagnostic_code == "ACTION.PERMISSION_DENIED",
          "recommendation contract did not enforce recommendation right");
  Require(recommendation_denied.detail == "OBS_AGENT_RECOMMENDATION_READ",
          "recommendation missing right mismatch");

  const auto& override_contract = Contract("alert_manager", "silence_alert");
  auto override_policy = PolicyFor(override_contract);
  auto override_request = RequestFor(
      override_contract,
      Context({"OBS_AGENT_RECOMMENDATION_READ"}),
      &override_policy);
  const auto override_denied =
      agents::EvaluateAgentActionContract(override_contract, override_request);
  Require(override_denied.diagnostic_code == "ACTION.PERMISSION_DENIED",
          "override contract did not enforce override right");
  Require(override_denied.detail == "OBS_AGENT_OVERRIDE",
          "override missing right mismatch");

  const auto& internal_contract =
      Contract("page_allocation_manager", "publish_shrink_ready");
  auto internal_policy = PolicyFor(internal_contract);
  auto internal_context = Context();
  internal_context.trace_tags.push_back("engine.internal");
  auto internal_request = RequestFor(internal_contract, internal_context,
                                     &internal_policy);
  const auto internal_allowed =
      agents::EvaluateAgentActionContract(internal_contract, internal_request);
  Require(internal_allowed.diagnostic_code != "ACTION.PERMISSION_DENIED",
          "internal trace tag did not satisfy internal action permission");
}

void TestEvidencePolicyMetricsAndSupportRedaction() {
  agents::AgentEvidenceRecord evidence;
  evidence.evidence_uuid = "evidence-006e-restricted";
  evidence.evidence_kind = "agent_audit";
  evidence.detail = "secret payload";
  evidence.redaction_class = "restricted";

  const auto evidence_redacted =
      agents::RedactAgentEvidenceForSecurity(evidence, Context({"OBS_AGENT_EVIDENCE_READ"}));
  Require(evidence_redacted.visible, "evidence reader lost row visibility");
  Require(evidence_redacted.redacted, "restricted evidence payload was not redacted");
  Require(evidence_redacted.evidence.detail == "redacted",
          "restricted evidence detail leaked");
  Require(evidence_redacted.grant.diagnostic_code == "AGENT.EVIDENCE_REDACTED",
          "restricted evidence diagnostic mismatch");

  evidence.redaction_class = "support_safe";
  const auto support_visible =
      agents::RedactAgentEvidenceForSecurity(evidence, Context({}, {"SUP"}), true);
  Require(support_visible.visible, "support-safe evidence hidden from SUP");
  Require(!support_visible.redacted, "support-safe evidence was redacted");

  agents::AgentPolicyInspectionRecord policy;
  policy.policy_uuid = "policy-006e";
  policy.policy_family = "storage_health_policy";
  policy.policy_body = "threshold=secret";
  const auto policy_redacted =
      agents::RedactAgentPolicyForSecurity(policy, Context({"OBS_POLICY_READ"}));
  Require(policy_redacted.visible, "policy identity hidden from policy reader");
  Require(policy_redacted.redacted, "policy body was not redacted");
  Require(policy_redacted.policy.policy_body == "redacted",
          "policy body leaked");
  Require(policy_redacted.grant.diagnostic_code == "POLICY.BODY_REDACTED",
          "policy redaction diagnostic mismatch");

  agents::AgentMetricInspectionRecord metric;
  metric.metric_family = "sb_identity_sessions_active";
  metric.raw_value = "42";
  metric.security_sensitive = true;
  const auto metric_denied =
      agents::RedactAgentMetricForSecurity(metric, Context({"OBS_AGENT_STATE_READ"}));
  Require(!metric_denied.visible, "security metric visible without SEC right");
  Require(metric_denied.grant.missing_right == "SEC_AUTH_METRICS_READ",
          "security metric missing right mismatch");
}

void TestSupportBundleAndPublicAppDenial() {
  const auto& support_contract =
      Contract("support_bundle_triage_agent", "prepare_redacted_bundle");
  auto support_policy = PolicyFor(support_contract);
  auto support_request = RequestFor(
      support_contract,
      Context({"OBS_SUPPORT_BUNDLE_READ"}),
      &support_policy);
  const auto support_denied =
      agents::EvaluateAgentActionContract(support_contract, support_request);
  Require(support_denied.diagnostic_code == "ACTION.PERMISSION_DENIED",
          "support bundle contract did not require redaction right");
  Require(support_denied.detail == "SEC_REDACTION_POLICY_EDIT",
          "support bundle missing right mismatch: " + support_denied.detail);

  const auto public_state = agents::EvaluateAgentCommandGrant(
      Context({}, {"PUBLIC"}),
      agents::AgentSecurityCommandFamily::state_read,
      "show-agents");
  Require(!public_state.allowed, "PUBLIC command grant unexpectedly allowed");
  Require(public_state.diagnostic_code == "AGENT.PERMISSION_DENIED",
          "PUBLIC denial diagnostic mismatch");

  const auto app_control = agents::EvaluateAgentCommandGrant(
      Context({}, {"APP"}),
      agents::AgentSecurityCommandFamily::control,
      "alter-agent");
  Require(!app_control.allowed, "APP control grant unexpectedly allowed");
  Require(app_control.missing_right == "OBS_AGENT_CONTROL",
          "APP control missing right mismatch");
}

}  // namespace

int main() {
  TestGroupAndExplicitRightMapping();
  TestCommandFamilyRequiredRights();
  TestDenyWithoutActionRowLeakage();
  TestActionContractPermissionEnforcement();
  TestEvidencePolicyMetricsAndSupportRedaction();
  TestSupportBundleAndPublicAppDenial();
  return EXIT_SUCCESS;
}
