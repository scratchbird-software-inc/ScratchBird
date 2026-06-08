// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "agent_runtime_manager.hpp"

#include <algorithm>
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

const agents::AgentRuntimeSelectionDecision* FindDecision(
    const agents::AgentRuntimeManagerSnapshot& snapshot,
    const std::string& type_id) {
  for (const auto& decision : snapshot.selection_decisions) {
    if (decision.agent_type_id == type_id) { return &decision; }
  }
  return nullptr;
}

void RequirePolicyDisabledDecision(const agents::AgentRuntimeManagerSnapshot& snapshot,
                                   const std::string& type_id) {
  const auto* decision = FindDecision(snapshot, type_id);
  Require(decision != nullptr, "missing decision for " + type_id);
  Require(!decision->selected, type_id + " was selected despite disabled/operator-only baseline mode");
  Require(decision->policy_disabled, type_id + " was not marked policy-disabled");
  Require(!decision->failed_closed, type_id + " failed closed instead of remaining inspectable");
  Require(decision->diagnostic_code == "ENGINE.AGENT_RUNTIME_MANAGER.POLICY_DISABLED",
          type_id + " policy-disabled diagnostic mismatch: " + decision->diagnostic_code);
}

agents::AgentRuntimeActivationEvidence ValidEvidence() {
  agents::AgentRuntimeActivationEvidence evidence;
  evidence.database_uuid = "019e0f2a-002e-7000-8000-000000000002";
  evidence.engine_instance_uuid = "engine-instance:019e0f2a-002e";
  evidence.lifecycle_mode = agents::AgentLifecycleMode::database_open;
  evidence.policy_generation = 5;
  evidence.catalog_generation = 1;
  evidence.security_generation = 1;
  evidence.filespace_generation = 1;
  evidence.agent_set_generation = 1;
  evidence.health_generation = 1;
  evidence.tx1_bootstrap_visible = true;
  evidence.tx2_activation_committed = true;
  evidence.startup_admitted = true;
  evidence.health_publication_allowed = true;
  evidence.health_publication_persisted = true;
  return evidence;
}

agents::AgentRuntimeManagerConfig ExplicitConfigFromCatalog() {
  agents::InMemoryAgentRuntimeCatalog catalog;
  catalog.BootstrapDatabasePolicies(ValidEvidence().database_uuid, ValidEvidence().policy_generation);

  agents::AgentRuntimeManagerConfig config;
  config.standalone_edition = true;
  config.cluster_authority_available = false;
  config.use_explicit_policy_state = true;
  config.policy_records = catalog.policies();
  config.policy_attachments = catalog.attachments();
  return config;
}

void TestValidAttachmentAllowsBaselineSelection() {
  auto config = ExplicitConfigFromCatalog();
  const auto snapshot = agents::SelectStandaloneDatabaseLocalAgents(ValidEvidence(), config);
  Require(snapshot.status.ok, "valid explicit policy state failed manager selection");

  const auto* decision = FindDecision(snapshot, "memory_governor");
  Require(decision != nullptr, "missing memory_governor decision");
  Require(decision->selected, "valid memory_governor policy attachment did not select agent");
  Require(!decision->failed_closed, "valid memory_governor policy attachment failed closed");
}

void TestDisabledAndOperatorOnlyBaselinesAreInspectableButNotSelected() {
  auto config = ExplicitConfigFromCatalog();
  const auto snapshot = agents::SelectStandaloneDatabaseLocalAgents(ValidEvidence(), config);
  Require(snapshot.status.ok, "valid explicit policy state failed manager selection");

  RequirePolicyDisabledDecision(snapshot, "export_adapter_manager");
  RequirePolicyDisabledDecision(snapshot, "job_control_manager");
  RequirePolicyDisabledDecision(snapshot, "restore_drill_manager");
  RequirePolicyDisabledDecision(snapshot, "identity_manager");
  RequirePolicyDisabledDecision(snapshot, "session_control_manager");
}

void TestMissingAttachmentFailsClosedButInspectable() {
  auto config = ExplicitConfigFromCatalog();
  config.policy_attachments.erase(
      std::remove_if(config.policy_attachments.begin(), config.policy_attachments.end(),
                     [](const agents::AgentPolicyAttachmentRecord& attachment) {
                       return attachment.agent_type_id == "memory_governor";
                     }),
      config.policy_attachments.end());

  const auto snapshot = agents::SelectStandaloneDatabaseLocalAgents(ValidEvidence(), config);
  Require(snapshot.status.ok, "missing attachment should leave manager inspectable");
  Require(!snapshot.policy_bootstrap_records.empty(),
          "missing attachment removed policy inspectability");

  const auto* decision = FindDecision(snapshot, "memory_governor");
  Require(decision != nullptr, "missing memory_governor decision");
  Require(!decision->selected, "memory_governor selected without attachment");
  Require(decision->failed_closed, "missing attachment did not fail closed");
  Require(decision->diagnostic_code == "SB_AGENT_POLICY_ATTACHMENT.MISSING",
          "missing attachment diagnostic mismatch: " + decision->diagnostic_code);
}

void TestInvalidStaleWrongFamilyAndScopeIncompatiblePolicyFailClosed() {
  auto descriptor = agents::FindAgentType("memory_governor");
  Require(descriptor.has_value(), "memory_governor descriptor missing");

  auto policy = agents::BaselinePolicyForAgentFamily(*descriptor, "memory_governor_policy", 5);
  agents::AgentPolicyAttachmentRecord attachment;
  attachment.attachment_uuid = "policy-attachment:memory-governor";
  attachment.agent_type_id = descriptor->type_id;
  attachment.policy_family = policy.policy_family;
  attachment.policy_uuid = policy.policy_uuid;
  attachment.scope = descriptor->scope;
  attachment.policy_generation = 5;
  attachment.attachment_generation = 5;
  attachment.evidence_uuid = "agent-policy-attach-evidence:memory-governor";

  auto status = agents::ValidateAgentPolicyStateForMutation(&policy, &attachment, *descriptor, 5);
  Require(status.ok, "valid policy attachment was rejected: " + status.diagnostic_code);

  auto stale = attachment;
  stale.policy_generation = 4;
  status = agents::ValidateAgentPolicyStateForMutation(&policy, &stale, *descriptor, 5);
  Require(!status.ok && status.diagnostic_code == "SB_AGENT_POLICY_ATTACHMENT.STALE_GENERATION",
          "stale attachment did not fail closed");

  auto wrong_family = policy;
  wrong_family.policy_family = "storage_health_policy";
  status = agents::ValidateAgentPolicyStateForMutation(&wrong_family, &attachment, *descriptor, 5);
  Require(!status.ok && status.diagnostic_code == "SB_AGENT_POLICY_ATTACHMENT.WRONG_FAMILY",
          "wrong-family policy did not fail closed");

  auto wrong_scope = policy;
  wrong_scope.scope = "cluster";
  status = agents::ValidateAgentPolicyStateForMutation(&wrong_scope, &attachment, *descriptor, 5);
  Require(!status.ok && status.diagnostic_code == "SB_AGENT_POLICY_ATTACHMENT.SCOPE_INCOMPATIBLE",
          "scope-incompatible policy did not fail closed");

  policy.config_fields.erase("emergency_reserve_percent");
  status = agents::ValidateAgentPolicyStateForMutation(&policy, &attachment, *descriptor, 5);
  Require(!status.ok && status.diagnostic_code == "SB_AGENT_POLICY.REQUIRED_FIELD_MISSING",
          "invalid policy missing required config did not fail closed");
}

void TestActionWithInvalidPolicyFailsClosed() {
  auto descriptor = agents::FindAgentType("memory_governor");
  Require(descriptor.has_value(), "memory_governor descriptor missing");

  auto policy = agents::BaselinePolicyForAgentFamily(*descriptor, "memory_governor_policy", 5);
  policy.config_fields.erase("spill_threshold");

  agents::AgentRuntimeContext context;
  context.security_context_present = true;
  context.database_uuid = ValidEvidence().database_uuid;
  context.groups.push_back("OPS");

  agents::AgentActionRequest action;
  action.action_uuid = "action:memory-governor:invalid-policy";
  action.agent_type_id = descriptor->type_id;
  action.action_class = agents::AgentActionClass::direct_bounded_action;
  action.actuator_id = "memory_governor";
  action.operation_id = "tighten_budget";
  action.idempotency_key = "idempotent:memory-governor:invalid-policy";
  action.dry_run = false;
  action.manual_approval_present = true;

  const auto decision = agents::EvaluateAgentAction(context, *descriptor, policy, action);
  Require(decision.result_class == agents::AgentActionResultClass::failed_closed,
          "invalid policy action did not fail closed");
  Require(decision.diagnostic_code == "SB_AGENT_POLICY.REQUIRED_FIELD_MISSING",
          "invalid policy action diagnostic mismatch: " + decision.diagnostic_code);
}

}  // namespace

int main() {
  TestValidAttachmentAllowsBaselineSelection();
  TestDisabledAndOperatorOnlyBaselinesAreInspectableButNotSelected();
  TestMissingAttachmentFailsClosedButInspectable();
  TestInvalidStaleWrongFamilyAndScopeIncompatiblePolicyFailClosed();
  TestActionWithInvalidPolicyFailsClosed();
  return EXIT_SUCCESS;
}
