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

bool Contains(const std::vector<std::string>& values, const std::string& value) {
  for (const auto& candidate : values) {
    if (candidate == value) { return true; }
  }
  return false;
}

agents::AgentRuntimeContext ContextWithOverrideRight() {
  agents::AgentRuntimeContext context;
  context.security_context_present = true;
  context.database_uuid = "database:agent-arbitration-gate";
  context.wall_now_microseconds = 1000000;
  context.rights.push_back("OBS_AGENT_OVERRIDE");
  return context;
}

agents::AgentRuntimeContext ContextWithoutOverrideRight() {
  agents::AgentRuntimeContext context;
  context.security_context_present = true;
  context.database_uuid = "database:agent-arbitration-gate";
  context.wall_now_microseconds = 1000000;
  context.rights.push_back("OBS_AGENT_STATE_READ");
  return context;
}

agents::AgentActionRequest Action(const std::string& uuid,
                                  const std::string& action_class,
                                  scratchbird::core::platform::u64 evidence_quality,
                                  const std::string& scope = "scope:database/filespace",
                                  bool safety_passed = true) {
  agents::AgentActionRequest action;
  action.action_uuid = uuid;
  action.agent_type_id = "memory_governor";
  action.instance_uuid = "agent-instance:arbitration-gate";
  action.action_class = agents::AgentActionClass::request_action;
  action.actuator_id = "memory_governor";
  action.operation_id = "tighten_budget";
  action.idempotency_key = uuid + ":idempotent";
  action.dry_run = false;
  action.inputs["arbitration_action_class"] = action_class;
  action.inputs["scope_uuid"] = scope;
  action.inputs["policy_uuid"] = "policy:agent-arbitration-gate";
  action.inputs["risk"] = "high";
  action.inputs["reversibility"] = "bounded_reversible";
  action.inputs["evidence_quality"] = std::to_string(evidence_quality);
  action.inputs["evidence_uuid"] = "evidence:" + uuid;
  action.inputs["safety_preconditions_passed"] = safety_passed ? "true" : "false";
  return action;
}

agents::AgentArbitrationOverride SuppressionOverride(const std::string& suppressed_action_uuid,
                                                     scratchbird::core::platform::u64 expires_at = 2000000) {
  agents::AgentArbitrationOverride override_record;
  override_record.override_uuid = "override:agent-arbitration-gate";
  override_record.scope_uuid = "scope:database/filespace";
  override_record.created_by = "sysarch";
  override_record.reason_code = "operator_suppression";
  override_record.suppressed_action_uuids.push_back(suppressed_action_uuid);
  override_record.expires_at_microseconds = expires_at;
  override_record.renewal_rule = "manual";
  override_record.rollback_rule = "expire";
  override_record.evidence_uuid = "evidence:override:agent-arbitration-gate";
  return override_record;
}

void RequireRecordHasCoreEvidence(const agents::AgentArbitrationRecord& record,
                                  const std::string& policy_uuid) {
  Require(record.policy_uuid == policy_uuid,
          "arbitration record policy uuid mismatch: " + record.policy_uuid);
  Require(!record.evidence_uuid.empty(), "arbitration record missing evidence uuid");
  Require(std::string(agents::AgentArbitrationOutcomeName(record.outcome)) != "unknown",
          "arbitration record has unknown outcome");
  Require(std::string(agents::AgentArbitrationPriorityRuleName(record.priority_rule)) != "unknown",
          "arbitration record has unknown priority rule");
}

void TestOutcomeNamesAreExact() {
  Require(std::string(agents::AgentArbitrationOutcomeName(
              agents::AgentArbitrationOutcome::winner_executes)) == "winner_executes",
          "winner outcome name mismatch");
  Require(std::string(agents::AgentArbitrationOutcomeName(
              agents::AgentArbitrationOutcome::both_denied)) == "both_denied",
          "both_denied outcome name mismatch");
  Require(std::string(agents::AgentArbitrationOutcomeName(
              agents::AgentArbitrationOutcome::operator_review_required)) == "operator_review_required",
          "operator_review_required outcome name mismatch");
  Require(std::string(agents::AgentArbitrationOutcomeName(
              agents::AgentArbitrationOutcome::suppressed_by_override)) == "suppressed_by_override",
          "suppressed_by_override outcome name mismatch");
}

void TestNoActionsDenied() {
  const auto record = agents::ArbitrateAgentActionsDetailed(ContextWithOverrideRight(), {});
  Require(record.outcome == agents::AgentArbitrationOutcome::both_denied,
          "no actions did not deny");
  Require(record.priority_rule == agents::AgentArbitrationPriorityRule::no_actions,
          "no actions priority rule mismatch");
  Require(record.diagnostic_code == "SB_AGENT_ARBITRATION.NO_ACTIONS",
          "no actions diagnostic mismatch");
  Require(record.winning_action_uuid.empty(), "no actions produced a winner");
}

void TestSingleActionWinsAndNormalizes() {
  const auto action = Action("action:single", "reduce_pressure", 55);
  const auto record = agents::ArbitrateAgentActionsDetailed(ContextWithOverrideRight(), {action});
  Require(record.outcome == agents::AgentArbitrationOutcome::winner_executes,
          "single action did not execute");
  Require(record.priority_rule == agents::AgentArbitrationPriorityRule::single_action,
          "single action priority rule mismatch");
  Require(record.winning_action_uuid == action.action_uuid,
          "single action winner mismatch");
  Require(record.normalized_actions.size() == 1, "single action was not normalized");
  const auto& normalized = record.normalized_actions.front();
  Require(normalized.action_class == agents::AgentArbitrationActionClass::reduce_pressure,
          "normalized action class mismatch");
  Require(normalized.scope_uuid == "scope:database/filespace",
          "normalized scope mismatch");
  Require(normalized.actuator_id == "memory_governor",
          "normalized actuator mismatch");
  Require(normalized.risk == agents::AgentArbitrationRisk::high,
          "normalized risk mismatch");
  Require(normalized.reversibility == agents::AgentArbitrationReversibility::bounded_reversible,
          "normalized reversibility mismatch");
  Require(normalized.evidence_quality == 55,
          "normalized evidence quality mismatch");
  RequireRecordHasCoreEvidence(record, "policy:agent-arbitration-gate");
}

void TestHigherPriorityWins() {
  const auto lower = Action("action:lower-priority", "reduce_cost", 99);
  const auto higher = Action("action:higher-priority", "protect_correctness", 1);
  const auto record = agents::ArbitrateAgentActionsDetailed(
      ContextWithOverrideRight(), {lower, higher});
  Require(record.outcome == agents::AgentArbitrationOutcome::winner_executes,
          "higher priority action did not execute");
  Require(record.priority_rule == agents::AgentArbitrationPriorityRule::action_class_priority,
          "higher priority rule mismatch");
  Require(record.winning_action_uuid == higher.action_uuid,
          "higher priority winner mismatch");
  Require(Contains(record.losing_action_uuids, lower.action_uuid),
          "higher priority loser missing");
  RequireRecordHasCoreEvidence(record, "policy:agent-arbitration-gate");
}

void TestHigherEvidenceQualityWinsSamePriority() {
  const auto lower = Action("action:lower-evidence", "protect_availability", 10);
  const auto higher = Action("action:higher-evidence", "protect_availability", 90);
  const auto record = agents::ArbitrateAgentActionsDetailed(
      ContextWithOverrideRight(), {lower, higher});
  Require(record.outcome == agents::AgentArbitrationOutcome::winner_executes,
          "higher evidence action did not execute");
  Require(record.priority_rule == agents::AgentArbitrationPriorityRule::evidence_quality,
          "higher evidence rule mismatch");
  Require(record.winning_action_uuid == higher.action_uuid,
          "higher evidence winner mismatch");
  Require(Contains(record.losing_action_uuids, lower.action_uuid),
          "higher evidence loser missing");
}

void TestExactTieCreatesOperatorReview() {
  const auto left = Action("action:tie-left", "protect_durability", 77);
  const auto right = Action("action:tie-right", "protect_durability", 77);
  const auto record = agents::ArbitrateAgentActionsDetailed(
      ContextWithOverrideRight(), {left, right});
  Require(record.outcome == agents::AgentArbitrationOutcome::operator_review_required,
          "exact tie did not require operator review");
  Require(record.priority_rule == agents::AgentArbitrationPriorityRule::exact_tie_operator_review,
          "exact tie rule mismatch");
  Require(record.winning_action_uuid.empty(), "exact tie produced a winner");
  Require(record.operator_review_action_created, "operator review action was not created");
  Require(!record.operator_review_action_uuid.empty(), "operator review action uuid missing");
  Require(Contains(record.losing_action_uuids, left.action_uuid),
          "tie left loser missing");
  Require(Contains(record.losing_action_uuids, right.action_uuid),
          "tie right loser missing");
}

void TestActiveOverrideSuppressesMatchingAction() {
  const auto suppressed = Action("action:suppressed", "protect_correctness", 100);
  const auto allowed = Action("action:allowed", "reduce_pressure", 10);
  const auto override_record = SuppressionOverride(suppressed.action_uuid);
  const auto record = agents::ArbitrateAgentActionsDetailed(
      ContextWithOverrideRight(), {suppressed, allowed}, {override_record});
  Require(record.outcome == agents::AgentArbitrationOutcome::suppressed_by_override,
          "active override did not suppress");
  Require(record.priority_rule == agents::AgentArbitrationPriorityRule::override_suppression,
          "override suppression rule mismatch");
  Require(record.override_uuid == override_record.override_uuid,
          "override uuid mismatch");
  Require(record.winning_action_uuid == allowed.action_uuid,
          "override suppression remaining winner mismatch");
  Require(Contains(record.losing_action_uuids, suppressed.action_uuid),
          "suppressed action not recorded as loser");
  RequireRecordHasCoreEvidence(record, "policy:agent-arbitration-gate");
}

void TestExpiredOverrideIgnored() {
  const auto suppressed = Action("action:expired-override-target", "protect_correctness", 100);
  const auto allowed = Action("action:expired-override-peer", "reduce_pressure", 10);
  const auto expired = SuppressionOverride(suppressed.action_uuid, 999999);
  const auto record = agents::ArbitrateAgentActionsDetailed(
      ContextWithoutOverrideRight(), {suppressed, allowed}, {expired});
  Require(record.outcome == agents::AgentArbitrationOutcome::winner_executes,
          "expired override changed outcome");
  Require(record.priority_rule == agents::AgentArbitrationPriorityRule::action_class_priority,
          "expired override priority rule mismatch");
  Require(record.winning_action_uuid == suppressed.action_uuid,
          "expired override should allow priority winner");
  Require(record.override_uuid.empty(), "expired override was recorded as active");
}

void TestOverrideWithoutRightDenied() {
  const auto suppressed = Action("action:no-right-target", "protect_correctness", 100);
  const auto allowed = Action("action:no-right-peer", "reduce_pressure", 10);
  const auto override_record = SuppressionOverride(suppressed.action_uuid);
  const auto record = agents::ArbitrateAgentActionsDetailed(
      ContextWithoutOverrideRight(), {suppressed, allowed}, {override_record});
  Require(record.outcome == agents::AgentArbitrationOutcome::both_denied,
          "override without right did not deny");
  Require(record.priority_rule == agents::AgentArbitrationPriorityRule::override_right_required,
          "override without right rule mismatch");
  Require(record.diagnostic_code == "SB_AGENT_OVERRIDE.RIGHT_REQUIRED",
          "override without right diagnostic mismatch");
  Require(record.override_uuid == override_record.override_uuid,
          "override without right uuid mismatch");
  Require(record.winning_action_uuid.empty(), "override without right produced winner");
  Require(Contains(record.losing_action_uuids, suppressed.action_uuid),
          "override without right missing target loser");
  Require(Contains(record.losing_action_uuids, allowed.action_uuid),
          "override without right missing peer loser");
}

void TestFailedSafetyPreconditionExcludedAndDenied() {
  const auto unsafe = Action("action:unsafe", "protect_correctness", 100,
                            "scope:database/filespace", false);
  const auto safe = Action("action:safe", "reduce_cost", 1);
  const auto record = agents::ArbitrateAgentActionsDetailed(
      ContextWithOverrideRight(), {unsafe, safe});
  Require(record.outcome == agents::AgentArbitrationOutcome::winner_executes,
          "safe action did not execute after unsafe action exclusion");
  Require(record.winning_action_uuid == safe.action_uuid,
          "safety exclusion winner mismatch");
  Require(Contains(record.losing_action_uuids, unsafe.action_uuid),
          "unsafe action was not denied");
  Require(!record.normalized_actions.front().safety_preconditions_passed,
          "unsafe action safety precondition did not normalize");
}

void TestOverrideCannotGrantForbiddenAuthority() {
  auto catalog_action = Action("action:catalog-authority", "protect_correctness", 100,
                              "scope:database/catalog");
  catalog_action.operation_id = "catalog_mutation";
  auto override_record = SuppressionOverride("unused");
  override_record.scope_uuid = "scope:database/catalog";
  override_record.allowed_action_uuids.push_back(catalog_action.action_uuid);
  const auto record = agents::ArbitrateAgentActionsDetailed(
      ContextWithOverrideRight(), {catalog_action}, {override_record});
  Require(record.outcome == agents::AgentArbitrationOutcome::both_denied,
          "forbidden override grant did not deny");
  Require(record.priority_rule == agents::AgentArbitrationPriorityRule::override_authority_forbidden,
          "forbidden override grant rule mismatch");
  Require(record.diagnostic_code == "SB_AGENT_OVERRIDE.AUTHORITY_GRANT_FORBIDDEN",
          "forbidden override grant diagnostic mismatch");
}

void TestDiagnosticCodesRegistered() {
  Require(agents::IsKnownAgentDiagnosticCode("SB_AGENT_ARBITRATION.NO_ACTIONS"),
          "no actions diagnostic not registered");
  Require(agents::IsKnownAgentDiagnosticCode("SB_AGENT_ARBITRATION.WINNER_EXECUTES"),
          "winner diagnostic not registered");
  Require(agents::IsKnownAgentDiagnosticCode("SB_AGENT_ARBITRATION.OPERATOR_REVIEW_REQUIRED"),
          "operator review diagnostic not registered");
  Require(agents::IsKnownAgentDiagnosticCode("SB_AGENT_ARBITRATION.SUPPRESSED_BY_OVERRIDE"),
          "override suppression diagnostic not registered");
  Require(agents::IsKnownAgentDiagnosticCode("SB_AGENT_OVERRIDE.RIGHT_REQUIRED"),
          "override right diagnostic not registered");
  Require(agents::IsKnownAgentDiagnosticCode("SB_AGENT_OVERRIDE.AUTHORITY_GRANT_FORBIDDEN"),
          "override authority diagnostic not registered");
}

}  // namespace

int main() {
  TestOutcomeNamesAreExact();
  TestNoActionsDenied();
  TestSingleActionWinsAndNormalizes();
  TestHigherPriorityWins();
  TestHigherEvidenceQualityWinsSamePriority();
  TestExactTieCreatesOperatorReview();
  TestActiveOverrideSuppressesMatchingAction();
  TestExpiredOverrideIgnored();
  TestOverrideWithoutRightDenied();
  TestFailedSafetyPreconditionExcludedAndDenied();
  TestOverrideCannotGrantForbiddenAuthority();
  TestDiagnosticCodesRegistered();
  return EXIT_SUCCESS;
}
