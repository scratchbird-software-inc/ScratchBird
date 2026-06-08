// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "agents/nosql_backpressure_debt_agent.hpp"

#include <map>
#include <set>
#include <utility>

namespace scratchbird::core::agents::implemented_agents {
namespace {

using scratchbird::core::platform::DiagnosticArgument;
using scratchbird::core::platform::MakeDiagnostic;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;

Status AgentOkStatus() {
  return {StatusCode::ok, Severity::info, Subsystem::engine};
}

Status AgentErrorStatus() {
  return {StatusCode::platform_required_feature_missing,
          Severity::error,
          Subsystem::engine};
}

std::string BoolText(bool value) {
  return value ? "true" : "false";
}

void AddEvidence(NoSqlBackpressureDebtAgentResult* result,
                 std::string key,
                 std::string value) {
  result->evidence.push_back({std::move(key), std::move(value)});
}

bool IsSupportedFamily(NoSqlBackpressureDebtFamily family) {
  return family != NoSqlBackpressureDebtFamily::unknown;
}

bool IsSupportedKind(NoSqlBackpressureDebtKind kind) {
  return kind != NoSqlBackpressureDebtKind::unknown;
}

std::string ActionKind(NoSqlBackpressureDebtKind kind) {
  switch (kind) {
    case NoSqlBackpressureDebtKind::refresh:
      return "schedule_family_refresh";
    case NoSqlBackpressureDebtKind::merge_compaction:
      return "schedule_merge_compaction";
    case NoSqlBackpressureDebtKind::generation_build:
      return "schedule_generation_build";
    case NoSqlBackpressureDebtKind::payload_policy:
      return "enforce_payload_policy_refresh";
    case NoSqlBackpressureDebtKind::slowdown_backpressure_policy:
      return "apply_slowdown_backpressure_policy";
    case NoSqlBackpressureDebtKind::strict_bulk_redirect_policy:
      return "redirect_strict_bulk_to_deferred_bulk";
    case NoSqlBackpressureDebtKind::result_suppression_policy:
      return "suppress_unsafe_result";
    case NoSqlBackpressureDebtKind::unknown:
      return "unsupported";
  }
  return "unsupported";
}

std::string PolicyKind(NoSqlBackpressureDebtKind kind) {
  switch (kind) {
    case NoSqlBackpressureDebtKind::refresh:
      return "refresh_debt_policy";
    case NoSqlBackpressureDebtKind::merge_compaction:
      return "merge_compaction_debt_policy";
    case NoSqlBackpressureDebtKind::generation_build:
      return "generation_build_debt_policy";
    case NoSqlBackpressureDebtKind::payload_policy:
      return "payload_policy_debt_policy";
    case NoSqlBackpressureDebtKind::slowdown_backpressure_policy:
      return "slowdown_backpressure_policy";
    case NoSqlBackpressureDebtKind::strict_bulk_redirect_policy:
      return "strict_bulk_redirect_policy";
    case NoSqlBackpressureDebtKind::result_suppression_policy:
      return "result_suppression_policy";
    case NoSqlBackpressureDebtKind::unknown:
      return "unsupported";
  }
  return "unsupported";
}

bool IsStaleResult(const NoSqlBackpressureDebtEntry& entry) {
  return entry.stale_result ||
         (entry.required_epoch != 0 &&
          entry.evidence_epoch < entry.required_epoch);
}

bool IsOverBudgetResult(const NoSqlBackpressureDebtEntry& entry) {
  return entry.over_budget_result ||
         (entry.budget_cost_units != 0 &&
          entry.observed_cost_units > entry.budget_cost_units);
}

std::string SuppressionDiagnosticCode(
    const NoSqlBackpressureDebtEntry& entry) {
  if (entry.unsafe_result) {
    return kNoSqlBackpressureDebtResultUnsafe;
  }
  if (IsStaleResult(entry)) {
    return kNoSqlBackpressureDebtResultStale;
  }
  if (IsOverBudgetResult(entry)) {
    return kNoSqlBackpressureDebtResultOverBudget;
  }
  return kNoSqlBackpressureDebtPlanned;
}

std::string SuppressionDetail(const NoSqlBackpressureDebtEntry& entry) {
  std::string detail = "family=";
  detail += NoSqlBackpressureDebtFamilyName(entry.family);
  detail += ";result_id=";
  detail += entry.result_id;
  detail += ";stale_result=";
  detail += BoolText(IsStaleResult(entry));
  detail += ";over_budget_result=";
  detail += BoolText(IsOverBudgetResult(entry));
  detail += ";unsafe_result=";
  detail += BoolText(entry.unsafe_result);
  detail += ";evidence_epoch=";
  detail += std::to_string(entry.evidence_epoch);
  detail += ";required_epoch=";
  detail += std::to_string(entry.required_epoch);
  detail += ";observed_cost_units=";
  detail += std::to_string(entry.observed_cost_units);
  detail += ";budget_cost_units=";
  detail += std::to_string(entry.budget_cost_units);
  return detail;
}

bool ShouldSuppressResult(const NoSqlBackpressureDebtEntry& entry) {
  return entry.debt_kind ==
             NoSqlBackpressureDebtKind::result_suppression_policy &&
         (IsStaleResult(entry) || IsOverBudgetResult(entry) ||
          entry.unsafe_result);
}

NoSqlBackpressureDebtAgentResult Finish(
    NoSqlBackpressureDebtAgentResult result,
    NoSqlBackpressureDebtDecisionKind decision,
    std::string diagnostic_code,
    std::string message_key,
    std::string detail,
    bool fail_closed) {
  result.status = fail_closed ? AgentErrorStatus() : AgentOkStatus();
  result.decision = decision;
  result.fail_closed = fail_closed;
  result.diagnostic = MakeNoSqlBackpressureDebtDiagnostic(
      result.status,
      std::move(diagnostic_code),
      std::move(message_key),
      std::move(detail));
  AddEvidence(&result,
              "nosql_backpressure_debt_agent",
              "odf079_backpressure_debt_ledger_v1");
  AddEvidence(&result,
              "authority_source",
              "engine_owned_request_context_and_mga_evidence");
  AddEvidence(&result,
              "decision",
              NoSqlBackpressureDebtDecisionKindName(decision));
  AddEvidence(&result, "fail_closed", BoolText(fail_closed));
  AddEvidence(&result,
              "ledger_row_count",
              std::to_string(result.ledger_rows.size()));
  AddEvidence(&result,
              "planned_action_count",
              std::to_string(result.actions.size()));
  AddEvidence(&result,
              "suppression_count",
              std::to_string(result.suppressions.size()));
  AddEvidence(&result, "diagnostic_code", result.diagnostic.diagnostic_code);
  return result;
}

NoSqlBackpressureDebtAgentResult Fail(
    NoSqlBackpressureDebtAgentResult result,
    std::string diagnostic_code,
    std::string message_key,
    std::string detail) {
  return Finish(std::move(result),
                NoSqlBackpressureDebtDecisionKind::refused_non_authoritative,
                std::move(diagnostic_code),
                std::move(message_key),
                std::move(detail),
                true);
}

bool AuthoritySafe(const NoSqlBackpressureDebtAuthority& authority) {
  return authority.engine_mga_authoritative &&
         authority.request_context_authoritative &&
         authority.security_snapshot_bound &&
         authority.grants_proven &&
         authority.row_mga_recheck_required &&
         authority.row_security_recheck_required &&
         !authority.parser_or_donor_authority &&
         !authority.provider_transaction_finality_authority &&
         !authority.provider_visibility_authority &&
         !authority.client_autocommit_authority &&
         !authority.wal_recovery_authority;
}

void AddAuthorityEvidence(NoSqlBackpressureDebtAgentResult* result,
                          const NoSqlBackpressureDebtAuthority& authority) {
  AddEvidence(result,
              "engine_mga_authoritative",
              BoolText(authority.engine_mga_authoritative));
  AddEvidence(result,
              "request_context_authoritative",
              BoolText(authority.request_context_authoritative));
  AddEvidence(result,
              "security_snapshot_bound",
              BoolText(authority.security_snapshot_bound));
  AddEvidence(result, "grants_proven", BoolText(authority.grants_proven));
  AddEvidence(result,
              "row_mga_recheck_evidence",
              authority.row_mga_recheck_required ? "required" : "missing");
  AddEvidence(result,
              "row_security_recheck_evidence",
              authority.row_security_recheck_required ? "required" : "missing");
  AddEvidence(result,
              "parser_executes_sql",
              authority.parser_or_donor_authority ? "true" : "false");
  AddEvidence(result,
              "wal_recovery_authority",
              authority.wal_recovery_authority ? "true" : "false");
  AddEvidence(result,
              "provider_transaction_finality_authority",
              authority.provider_transaction_finality_authority ? "true"
                                                                : "false");
  AddEvidence(result,
              "provider_visibility_authority",
              authority.provider_visibility_authority ? "true" : "false");
  AddEvidence(result,
              "client_autocommit_authority",
              authority.client_autocommit_authority ? "true" : "false");
}

}  // namespace

const char* NoSqlBackpressureDebtFamilyName(
    NoSqlBackpressureDebtFamily family) {
  switch (family) {
    case NoSqlBackpressureDebtFamily::key_value:
      return "key_value";
    case NoSqlBackpressureDebtFamily::document:
      return "document";
    case NoSqlBackpressureDebtFamily::search:
      return "search";
    case NoSqlBackpressureDebtFamily::vector:
      return "vector";
    case NoSqlBackpressureDebtFamily::graph:
      return "graph";
    case NoSqlBackpressureDebtFamily::time_series:
      return "time_series";
    case NoSqlBackpressureDebtFamily::unknown:
      return "unknown";
  }
  return "unknown";
}

const char* NoSqlBackpressureDebtKindName(NoSqlBackpressureDebtKind kind) {
  switch (kind) {
    case NoSqlBackpressureDebtKind::refresh:
      return "refresh";
    case NoSqlBackpressureDebtKind::merge_compaction:
      return "merge_compaction";
    case NoSqlBackpressureDebtKind::generation_build:
      return "generation_build";
    case NoSqlBackpressureDebtKind::payload_policy:
      return "payload_policy";
    case NoSqlBackpressureDebtKind::slowdown_backpressure_policy:
      return "slowdown_backpressure_policy";
    case NoSqlBackpressureDebtKind::strict_bulk_redirect_policy:
      return "strict_bulk_redirect_policy";
    case NoSqlBackpressureDebtKind::result_suppression_policy:
      return "result_suppression_policy";
    case NoSqlBackpressureDebtKind::unknown:
      return "unknown";
  }
  return "unknown";
}

const char* NoSqlBackpressureDebtDecisionKindName(
    NoSqlBackpressureDebtDecisionKind decision) {
  switch (decision) {
    case NoSqlBackpressureDebtDecisionKind::planned:
      return "planned";
    case NoSqlBackpressureDebtDecisionKind::suppressed_result:
      return "suppressed_result";
    case NoSqlBackpressureDebtDecisionKind::no_op:
      return "no_op";
    case NoSqlBackpressureDebtDecisionKind::refused_non_authoritative:
      return "refused_non_authoritative";
    case NoSqlBackpressureDebtDecisionKind::refused:
      return "refused";
  }
  return "refused";
}

NoSqlBackpressureDebtAgentResult RunNoSqlBackpressureDebtAgent(
    const NoSqlBackpressureDebtAgentRequest& request) {
  NoSqlBackpressureDebtAgentResult result;
  AddAuthorityEvidence(&result, request.authority);

  if (!AuthoritySafe(request.authority)) {
    return Fail(std::move(result),
                kNoSqlBackpressureDebtUnsafeAuthority,
                "agents.nosql_backpressure_debt.unsafe_authority",
                "engine-owned MGA request context, security, grants, and row recheck evidence are required");
  }

  std::set<NoSqlBackpressureDebtFamily> families_seen;
  std::map<NoSqlBackpressureDebtKind, bool> kinds_seen;
  for (const auto& entry : request.entries) {
    families_seen.insert(entry.family);
    kinds_seen[entry.debt_kind] = true;
    AddEvidence(&result,
                std::string("candidate_family:") +
                    NoSqlBackpressureDebtFamilyName(entry.family),
                entry.object_uuid);
    AddEvidence(&result,
                std::string("candidate_debt_kind:") +
                    NoSqlBackpressureDebtKindName(entry.debt_kind),
                entry.object_uuid);

    if (!IsSupportedFamily(entry.family)) {
      return Finish(std::move(result),
                    NoSqlBackpressureDebtDecisionKind::refused,
                    kNoSqlBackpressureDebtUnsupportedFamily,
                    "agents.nosql_backpressure_debt.unsupported_family",
                    entry.object_uuid,
                    true);
    }
    if (!IsSupportedKind(entry.debt_kind)) {
      return Finish(std::move(result),
                    NoSqlBackpressureDebtDecisionKind::refused,
                    kNoSqlBackpressureDebtUnsupportedKind,
                    "agents.nosql_backpressure_debt.unsupported_kind",
                    entry.object_uuid,
                    true);
    }
    if (!entry.evidence_authoritative) {
      return Fail(std::move(result),
                  kNoSqlBackpressureDebtEvidenceNotAuthoritative,
                  "agents.nosql_backpressure_debt.evidence_not_authoritative",
                  entry.object_uuid);
    }

    NoSqlBackpressureDebtLedgerRow row;
    row.family = entry.family;
    row.debt_kind = entry.debt_kind;
    row.object_uuid = entry.object_uuid;
    row.result_id = entry.result_id;
    row.evidence_epoch = entry.evidence_epoch;
    row.required_epoch = entry.required_epoch;
    row.debt_units = entry.debt_units;
    row.observed_cost_units = entry.observed_cost_units;
    row.budget_cost_units = entry.budget_cost_units;
    row.stale_result = IsStaleResult(entry);
    row.over_budget_result = IsOverBudgetResult(entry);
    row.unsafe_result = entry.unsafe_result;
    result.ledger_rows.push_back(std::move(row));

    if (ShouldSuppressResult(entry)) {
      NoSqlBackpressureResultSuppression suppression;
      suppression.family = entry.family;
      suppression.object_uuid = entry.object_uuid;
      suppression.result_id = entry.result_id;
      suppression.diagnostic_code = SuppressionDiagnosticCode(entry);
      suppression.diagnostic_detail = SuppressionDetail(entry);
      suppression.evidence_epoch = entry.evidence_epoch;
      suppression.required_epoch = entry.required_epoch;
      suppression.observed_cost_units = entry.observed_cost_units;
      suppression.budget_cost_units = entry.budget_cost_units;
      suppression.stale_result = IsStaleResult(entry);
      suppression.over_budget_result = IsOverBudgetResult(entry);
      suppression.unsafe_result = entry.unsafe_result;
      result.suppressions.push_back(std::move(suppression));
      continue;
    }

    NoSqlBackpressureDebtPlanAction action;
    action.family = entry.family;
    action.debt_kind = entry.debt_kind;
    action.object_uuid = entry.object_uuid;
    action.action_kind = ActionKind(entry.debt_kind);
    action.policy_kind = PolicyKind(entry.debt_kind);
    action.planned_work_units = entry.debt_units == 0 ? 1 : entry.debt_units;
    result.actions.push_back(std::move(action));
  }

  AddEvidence(&result, "family_count", std::to_string(families_seen.size()));
  AddEvidence(&result, "debt_kind_count", std::to_string(kinds_seen.size()));
  AddEvidence(&result,
              "now_microseconds",
              std::to_string(request.now_microseconds));

  if (!result.suppressions.empty()) {
    return Finish(std::move(result),
                  NoSqlBackpressureDebtDecisionKind::suppressed_result,
                  result.suppressions.front().diagnostic_code,
                  "agents.nosql_backpressure_debt.result_suppressed",
                  result.suppressions.front().diagnostic_detail,
                  false);
  }
  if (!result.actions.empty()) {
    return Finish(std::move(result),
                  NoSqlBackpressureDebtDecisionKind::planned,
                  kNoSqlBackpressureDebtPlanned,
                  "agents.nosql_backpressure_debt.planned",
                  std::to_string(result.actions.size()),
                  false);
  }
  return Finish(std::move(result),
                NoSqlBackpressureDebtDecisionKind::no_op,
                kNoSqlBackpressureDebtNoCandidateWork,
                "agents.nosql_backpressure_debt.no_candidate_work",
                "no authoritative debt entries were supplied",
                false);
}

DiagnosticRecord MakeNoSqlBackpressureDebtDiagnostic(
    Status status,
    std::string diagnostic_code,
    std::string message_key,
    std::string detail) {
  std::vector<DiagnosticArgument> arguments;
  if (!detail.empty()) {
    arguments.push_back({"detail", std::move(detail)});
  }
  return MakeDiagnostic(status.code,
                        status.severity,
                        status.subsystem,
                        std::move(diagnostic_code),
                        std::move(message_key),
                        std::move(arguments),
                        {},
                        "core.agents.nosql_backpressure_debt");
}

// SEARCH_KEY: SB_AGENT_IMPLEMENTATION_nosql_backpressure_debt_agent
const char* nosql_backpressure_debt_agent_implementation_anchor() {
  return "nosql_backpressure_debt_agent";
}

}  // namespace scratchbird::core::agents::implemented_agents
