// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "agent_runtime.hpp"
#include "agent_runtime_manager.hpp"
#include "metric_registry.hpp"
#include "uuid.hpp"

#include <cstdlib>
#include <iostream>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace agents = scratchbird::core::agents;
namespace metrics = scratchbird::core::metrics;
namespace platform = scratchbird::core::platform;
namespace uuid = scratchbird::core::uuid;

[[noreturn]] void Fail(const std::string& message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, const std::string& message) {
  if (!condition) { Fail(message); }
}

bool Contains(std::string_view haystack, std::string_view needle) {
  return haystack.find(needle) != std::string_view::npos;
}

std::string Id(platform::UuidKind kind, platform::u64 seed) {
  static std::map<std::pair<int, platform::u64>, std::string> generated_ids;
  const auto key = std::make_pair(static_cast<int>(kind), seed);
  const auto found = generated_ids.find(key);
  if (found != generated_ids.end()) { return found->second; }
  const auto generated = uuid::GenerateEngineIdentityV7(kind, 1917017000000ull + seed);
  Require(generated.ok(), "fixture UUID generation failed");
  const auto [inserted, _] =
      generated_ids.emplace(key, uuid::UuidToString(generated.value.value));
  return inserted->second;
}

void RequireNoLabelPrefix(const std::string& value, const std::string& field_name) {
  Require(!Contains(value, "policy:"), field_name + " leaked policy label prefix");
  Require(!Contains(value, "agent-"), field_name + " leaked agent label prefix");
  Require(!Contains(value, "engine:"), field_name + " leaked engine label prefix");
  Require(!Contains(value, "database:"), field_name + " leaked database label prefix");
  Require(!Contains(value, "replay:"), field_name + " leaked replay label prefix");
}

void RequireUuid(const std::string& value,
                 platform::UuidKind kind,
                 const std::string& field_name) {
  Require(!value.empty(), field_name + " is empty");
  RequireNoLabelPrefix(value, field_name);
  Require(uuid::ParseDurableEngineIdentityUuid(kind, value).ok(),
          field_name + " is not a typed durable engine UUID: " + value);
}

void RequireObjectUuid(const std::string& value, const std::string& field_name) {
  RequireUuid(value, platform::UuidKind::object, field_name);
}

void TestCatalogPolicyAndAttachmentUuidAuthority() {
  agents::InMemoryAgentRuntimeCatalog catalog;
  const auto database_uuid = Id(platform::UuidKind::database, 10);
  catalog.BootstrapDatabasePolicies(database_uuid, 9);
  Require(!catalog.policies().empty(), "catalog did not bootstrap policies");
  Require(catalog.policies().size() == catalog.attachments().size(),
          "catalog policies and attachments mismatch");

  for (const auto& policy : catalog.policies()) {
    RequireObjectUuid(policy.policy_uuid, "policy_uuid");
    Require(!policy.policy_name.empty(), "policy display name missing");
    Require(!policy.policy_family.empty(), "policy family metadata missing");
  }
  for (const auto& attachment : catalog.attachments()) {
    RequireObjectUuid(attachment.attachment_uuid, "attachment_uuid");
    RequireObjectUuid(attachment.policy_uuid, "attachment.policy_uuid");
    RequireObjectUuid(attachment.evidence_uuid, "attachment.evidence_uuid");
    Require(!attachment.agent_type_id.empty(), "attachment agent type metadata missing");
    Require(!attachment.policy_family.empty(), "attachment policy family metadata missing");
  }

  const auto descriptor = agents::FindAgentType("page_allocation_manager");
  Require(descriptor.has_value(), "page_allocation_manager descriptor missing");
  const auto baseline = agents::BaselinePolicyForAgent(*descriptor);
  RequireObjectUuid(baseline.policy_uuid, "baseline.policy_uuid");
  Require(Contains(baseline.policy_name, descriptor->type_id),
          "policy display label was not preserved outside UUID field");
}

void TestRuntimeManagerDoesNotParsePolicyUuidLabels() {
  agents::AgentRuntimeActivationEvidence evidence;
  evidence.database_uuid = Id(platform::UuidKind::database, 20);
  evidence.engine_instance_uuid = Id(platform::UuidKind::object, 21);
  evidence.policy_generation = 11;
  evidence.catalog_generation = 12;
  evidence.security_generation = 13;
  evidence.filespace_generation = 14;
  evidence.agent_set_generation = 15;
  evidence.health_generation = 16;
  evidence.tx1_bootstrap_visible = true;
  evidence.tx2_activation_committed = true;
  evidence.startup_admitted = true;
  evidence.health_publication_allowed = true;
  evidence.health_publication_persisted = true;

  agents::AgentRuntimeManagerConfig config;
  const auto snapshot = agents::SelectStandaloneDatabaseLocalAgents(evidence, config);
  Require(snapshot.status.ok, "runtime manager refused valid activation evidence");
  Require(!snapshot.policy_bootstrap_records.empty(), "runtime manager omitted policy bootstrap rows");
  Require(!snapshot.supervised_agents.empty(), "runtime manager omitted supervised agents");

  for (const auto& record : snapshot.policy_bootstrap_records) {
    RequireObjectUuid(record.policy_uuid, "manager.policy_uuid");
    Require(!record.agent_type_id.empty(),
            "manager did not resolve agent type from metadata");
    Require(!record.policy_family.empty(), "manager policy family metadata missing");
  }
  for (const auto& attachment : snapshot.policy_attachments) {
    RequireObjectUuid(attachment.policy_uuid, "manager.attachment.policy_uuid");
    RequireObjectUuid(attachment.evidence_uuid, "manager.attachment.evidence_uuid");
    Require(!attachment.agent_type_id.empty(), "manager attachment agent type missing");
  }
  for (const auto& instance : snapshot.supervised_agents) {
    RequireObjectUuid(instance.instance_uuid, "manager.instance_uuid");
    RequireObjectUuid(instance.policy_uuid, "manager.instance.policy_uuid");
    Require(!instance.agent_type_id.empty(), "manager instance agent type label missing");
  }
}

void TestTickSecurityActionAndFaultEvidenceUuidAuthority() {
  agents::AgentTickHealthRequest tick;
  tick.context.security_context_present = true;
  tick.context.database_uuid = Id(platform::UuidKind::database, 30);
  tick.context.principal_uuid = Id(platform::UuidKind::principal, 31);
  tick.context.wall_now_microseconds = 1000;
  tick.policy_generation = 17;
  for (const auto& descriptor : agents::CanonicalAgentRegistry()) {
    if (descriptor.deployment != agents::AgentDeployment::cluster &&
        !descriptor.cluster_only) {
      tick.suppressed_agent_type_ids.push_back(descriptor.type_id);
    }
  }
  metrics::MetricRegistry registry;
  const auto health = agents::BuildNonClusterAgentTickHealthSnapshot(tick, registry);
  Require(health.status.ok, "tick health snapshot failed");
  Require(!health.records.empty(), "tick health snapshot produced no rows");
  for (const auto& record : health.records) {
    RequireObjectUuid(record.policy_uuid, "tick.policy_uuid");
    RequireObjectUuid(record.health_evidence_uuid, "tick.health_evidence_uuid");
    RequireObjectUuid(record.action_evidence_uuid, "tick.action_evidence_uuid");
    Require(!record.agent_type_id.empty(), "tick agent type label missing");
  }

  agents::AgentRuntimeContext context;
  context.security_context_present = true;
  context.database_uuid = Id(platform::UuidKind::database, 40);
  context.principal_uuid = Id(platform::UuidKind::principal, 41);
  context.rights.push_back("OBS_AGENT_STATE_READ");
  context.rights.push_back("OBS_AGENT_CONTROL");
  const auto grant = agents::EvaluateAgentSecurityGrant(
      context,
      {agents::AgentSecurityGrantRequirement{agents::AgentSecurityRight::obs_agent_control}},
      "agent runtime uuid authority gate",
      true,
      false,
      false);
  RequireObjectUuid(grant.evidence_uuid, "grant.evidence_uuid");

  const auto descriptor = agents::FindAgentType("page_allocation_manager");
  Require(descriptor.has_value(), "page_allocation_manager descriptor missing for action");
  auto policy = agents::BaselinePolicyForAgent(*descriptor);
  policy.allow_live_action = true;
  policy.require_manual_approval = false;
  policy.require_dry_run_before_live = false;
  policy.activation = agents::AgentActivationProfile::live_action;
  agents::AgentActionRequest action;
  action.action_uuid = Id(platform::UuidKind::object, 50);
  action.agent_type_id = descriptor->type_id;
  action.instance_uuid = Id(platform::UuidKind::object, 51);
  action.actuator_id = descriptor->type_id;
  action.operation_id = "preallocate_page_family";
  action.idempotency_key = "uuid-authority-action";
  action.dry_run = false;
  action.inputs["policy_uuid"] = policy.policy_uuid;
  action.inputs["scope_uuid"] = tick.context.database_uuid;
  const auto decision = agents::EvaluateAgentAction(context, *descriptor, policy, action);
  RequireObjectUuid(decision.evidence_uuid, "action.evidence_uuid");
  const auto candidate = agents::NormalizeAgentActionForArbitration(context, action);
  RequireObjectUuid(candidate.action_uuid, "candidate.action_uuid");
  RequireObjectUuid(candidate.instance_uuid, "candidate.instance_uuid");
  RequireObjectUuid(candidate.policy_uuid, "candidate.policy_uuid");
  RequireUuid(candidate.scope_uuid, platform::UuidKind::database, "candidate.scope_uuid");
  RequireObjectUuid(candidate.evidence_uuid, "candidate.evidence_uuid");

  const auto outcome = agents::VerifyActionOutcome(action, true, true);
  RequireObjectUuid(outcome.evidence_uuid, "outcome.evidence_uuid");

  const auto fault = agents::EvaluateAgentFaultInjectionScenarioDetailed("disk_full");
  Require(!fault.status.ok, "fault injection did not fail closed");
  RequireObjectUuid(fault.evidence_uuid, "fault.evidence_uuid");
  if (fault.uses_arbitration) {
    RequireObjectUuid(fault.arbitration.evidence_uuid, "fault.arbitration.evidence_uuid");
  }
}

}  // namespace

int main() {
  TestCatalogPolicyAndAttachmentUuidAuthority();
  TestRuntimeManagerDoesNotParsePolicyUuidLabels();
  TestTickSecurityActionAndFaultEvidenceUuidAuthority();
  return EXIT_SUCCESS;
}
