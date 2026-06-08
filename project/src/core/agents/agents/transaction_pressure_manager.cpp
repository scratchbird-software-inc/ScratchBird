// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "agents/transaction_pressure_manager.hpp"

#include <algorithm>
#include <utility>

namespace scratchbird::core::agents::implemented_agents {
namespace {

namespace mga = scratchbird::transaction::mga;

using scratchbird::core::platform::DiagnosticArgument;
using scratchbird::core::platform::MakeDiagnostic;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;

Status TxPressureOkStatus() {
  return {StatusCode::ok, Severity::info, Subsystem::engine};
}

Status TxPressureRefuseStatus() {
  return {StatusCode::platform_required_feature_missing,
          Severity::error,
          Subsystem::engine};
}

std::string BoolText(bool value) {
  return value ? "true" : "false";
}

void AddEvidence(TransactionPressureManagerTickResult* result,
                 std::string key,
                 std::string value) {
  result->evidence.push_back({std::move(key), std::move(value)});
}

bool HasBlockingLocalTransaction(const AuthoritativeCleanupHorizonResult& horizon,
                                 LocalTransactionId local_id) {
  for (const auto& blocker : horizon.blockers) {
    if (blocker.local_transaction_id.value != local_id.value) {
      continue;
    }
    switch (blocker.kind) {
      case mga::CleanupHorizonBlockerKind::active_transaction:
      case mga::CleanupHorizonBlockerKind::always_active_session:
      case mga::CleanupHorizonBlockerKind::unresolved_outcome:
        return true;
      case mga::CleanupHorizonBlockerKind::active_snapshot:
      case mga::CleanupHorizonBlockerKind::inventory_authority:
      case mga::CleanupHorizonBlockerKind::unknown:
        break;
    }
  }
  return false;
}

bool SessionShapeValid(const TransactionPressureSessionSnapshot& session) {
  return !session.stable_session_id.empty() &&
         session.current_local_transaction_id.valid() &&
         session.active_transaction &&
         session.engine_owns_transaction;
}

const TransactionPressureSessionSnapshot* SelectOldestIdleBlockingSession(
    const AuthoritativeCleanupHorizonResult& horizon,
    const std::vector<TransactionPressureSessionSnapshot>& sessions) {
  const TransactionPressureSessionSnapshot* selected = nullptr;
  for (const auto& session : sessions) {
    if (!session.active_transaction || !session.engine_owns_transaction) {
      continue;
    }
    if (!HasBlockingLocalTransaction(horizon, session.current_local_transaction_id)) {
      continue;
    }
    if (selected == nullptr ||
        session.idle_microseconds > selected->idle_microseconds) {
      selected = &session;
    }
  }
  return selected;
}

TransactionPressureManagerTickResult Finish(
    Status status,
    TransactionPressureManagerDecisionKind decision,
    std::string diagnostic_code,
    std::string message_key,
    std::string detail,
    bool fail_closed) {
  TransactionPressureManagerTickResult result;
  result.status = status;
  result.decision = decision;
  result.fail_closed = fail_closed;
  result.diagnostic = MakeTransactionPressureManagerDiagnostic(
      status,
      std::move(diagnostic_code),
      std::move(message_key),
      std::move(detail));
  return result;
}

TransactionPressureManagerTickResult Refused(std::string diagnostic_code,
                                             std::string message_key,
                                             std::string detail) {
  return Finish(TxPressureRefuseStatus(),
                TransactionPressureManagerDecisionKind::refused,
                std::move(diagnostic_code),
                std::move(message_key),
                std::move(detail),
                true);
}

TransactionPressureManagerTickResult DeniedNonAuthoritative(
    std::string diagnostic_code,
    std::string message_key,
    std::string detail) {
  auto result = Finish(TxPressureRefuseStatus(),
                       TransactionPressureManagerDecisionKind::denied_non_authoritative,
                       std::move(diagnostic_code),
                       std::move(message_key),
                       std::move(detail),
                       true);
  result.denied_non_authoritative = true;
  return result;
}

bool PolicyShapeValid(const TransactionPressureManagerPolicy& policy) {
  return policy.present &&
         policy.valid &&
         policy.enabled &&
         policy.scope_compatible &&
         policy.always_in_transaction_policy &&
         policy.autocommit_emulation_is_transaction_control_only &&
         policy.warn_after_idle_microseconds <=
             policy.request_restart_after_idle_microseconds &&
         policy.request_restart_after_idle_microseconds <=
             policy.force_after_idle_microseconds;
}

void AddBaseEvidence(TransactionPressureManagerTickResult* result,
                     const TransactionPressureManagerPolicy& policy,
                     const std::vector<TransactionPressureSessionSnapshot>& sessions) {
  AddEvidence(result,
              "transaction_pressure_manager",
              "dpc031_long_idle_policy_v1");
  AddEvidence(result,
              "cleanup_horizon_service",
              "dpc030_authoritative_cleanup_horizon_v1");
  AddEvidence(result, "authority_source", "durable_mga_transaction_inventory");
  AddEvidence(result,
              "cleanup_horizon_authoritative",
              BoolText(result->horizon.cleanup_horizon_authoritative));
  AddEvidence(result,
              "always_in_transaction_policy",
              BoolText(policy.always_in_transaction_policy));
  AddEvidence(result,
              "autocommit_emulation_policy",
              "transaction_control_only");
  AddEvidence(result, "parser_finality_authority", "false");
  AddEvidence(result, "client_state_authority", "false");
  AddEvidence(result, "timestamp_ordering_authority", "false");
  AddEvidence(result, "uuid_ordering_authority", "false");
  AddEvidence(result, "crud_event_stream_authority", "false");
  AddEvidence(result, "cluster_private_implementation", "false");
  AddEvidence(result, "session_snapshot_count", std::to_string(sessions.size()));
  AddEvidence(result,
              "decision",
              TransactionPressureManagerDecisionKindName(result->decision));
  AddEvidence(result,
              "replacement_transaction_required",
              BoolText(result->replacement_transaction_required));
  AddEvidence(result,
              "action_mutates_transaction_if_accepted_by_server",
              BoolText(result->action_mutates_transaction_if_accepted_by_server));
  AddEvidence(result,
              "diagnostic_code",
              result->diagnostic.diagnostic_code);
  if (result->cleanup_horizon.valid()) {
    AddEvidence(result,
                "cleanup_horizon_local_transaction_id",
                std::to_string(result->cleanup_horizon.value));
  }
  if (result->selected_local_transaction_id.valid()) {
    AddEvidence(result,
                "selected_local_transaction_id",
                std::to_string(result->selected_local_transaction_id.value));
  }
  if (!result->selected_session_id.empty()) {
    AddEvidence(result, "selected_session_id", result->selected_session_id);
  }
  if (!result->selected_principal_id.empty()) {
    AddEvidence(result, "selected_principal_id", result->selected_principal_id);
  }
}

void AddSelectedSessionEvidence(TransactionPressureManagerTickResult* result,
                                const TransactionPressureSessionSnapshot& session) {
  result->selected_local_transaction_id = session.current_local_transaction_id;
  result->selected_session_id = session.stable_session_id;
  result->selected_principal_id = session.stable_principal_id;
  AddEvidence(result,
              "selected_idle_microseconds",
              std::to_string(session.idle_microseconds));
  AddEvidence(result,
              "session_authoritative",
              BoolText(session.session_authoritative));
  AddEvidence(result,
              "transaction_binding_authoritative",
              BoolText(session.transaction_binding_authoritative));
  AddEvidence(result,
              "authorization_session_bound",
              BoolText(session.authorization_session_bound));
  AddEvidence(result,
              "engine_owns_transaction",
              BoolText(session.engine_owns_transaction));
}

void ApplyDecisionFlags(TransactionPressureManagerTickResult* result) {
  switch (result->decision) {
    case TransactionPressureManagerDecisionKind::warn_notify:
      result->notification_required = true;
      break;
    case TransactionPressureManagerDecisionKind::request_restart:
      result->restart_requested = true;
      break;
    case TransactionPressureManagerDecisionKind::request_reauth:
      result->reauth_requested = true;
      break;
    case TransactionPressureManagerDecisionKind::request_cancel:
      result->cancel_requested = true;
      break;
    case TransactionPressureManagerDecisionKind::force_rollback_replacement:
    case TransactionPressureManagerDecisionKind::force_commit_replacement:
    case TransactionPressureManagerDecisionKind::force_restart_replacement:
      result->force_action_requested = true;
      result->replacement_transaction_required = true;
      result->action_mutates_transaction_if_accepted_by_server = true;
      break;
    case TransactionPressureManagerDecisionKind::no_action:
    case TransactionPressureManagerDecisionKind::denied_non_authoritative:
    case TransactionPressureManagerDecisionKind::refused:
      break;
  }
}

TransactionPressureManagerTickResult DecisionResult(
    TransactionPressureManagerDecisionKind decision,
    std::string diagnostic_code,
    std::string message_key,
    std::string detail) {
  auto result = Finish(TxPressureOkStatus(),
                       decision,
                       std::move(diagnostic_code),
                       std::move(message_key),
                       std::move(detail),
                       false);
  ApplyDecisionFlags(&result);
  return result;
}

bool ForcePolicyAllows(const TransactionPressureManagerPolicy& policy) {
  switch (policy.force_action) {
    case TransactionPressureForceAction::rollback:
      return policy.force_rollback_allowed;
    case TransactionPressureForceAction::commit:
      return policy.force_commit_allowed;
    case TransactionPressureForceAction::restart:
      return policy.force_restart_allowed;
    case TransactionPressureForceAction::none:
      return false;
  }
  return false;
}

TransactionPressureManagerDecisionKind ForceDecisionKind(
    TransactionPressureForceAction action) {
  switch (action) {
    case TransactionPressureForceAction::rollback:
      return TransactionPressureManagerDecisionKind::force_rollback_replacement;
    case TransactionPressureForceAction::commit:
      return TransactionPressureManagerDecisionKind::force_commit_replacement;
    case TransactionPressureForceAction::restart:
      return TransactionPressureManagerDecisionKind::force_restart_replacement;
    case TransactionPressureForceAction::none:
      return TransactionPressureManagerDecisionKind::denied_non_authoritative;
  }
  return TransactionPressureManagerDecisionKind::denied_non_authoritative;
}

const char* ForceDiagnosticCode(TransactionPressureForceAction action) {
  switch (action) {
    case TransactionPressureForceAction::rollback:
      return "TX_PRESSURE_MANAGER.FORCE_ROLLBACK_REPLACEMENT";
    case TransactionPressureForceAction::commit:
      return "TX_PRESSURE_MANAGER.FORCE_COMMIT_REPLACEMENT";
    case TransactionPressureForceAction::restart:
      return "TX_PRESSURE_MANAGER.FORCE_RESTART_REPLACEMENT";
    case TransactionPressureForceAction::none:
      return "TX_PRESSURE_MANAGER.FORCE_POLICY_DENIED";
  }
  return "TX_PRESSURE_MANAGER.FORCE_POLICY_DENIED";
}

const char* ForceMessageKey(TransactionPressureForceAction action) {
  switch (action) {
    case TransactionPressureForceAction::rollback:
      return "agents.transaction_pressure.force_rollback_replacement";
    case TransactionPressureForceAction::commit:
      return "agents.transaction_pressure.force_commit_replacement";
    case TransactionPressureForceAction::restart:
      return "agents.transaction_pressure.force_restart_replacement";
    case TransactionPressureForceAction::none:
      return "agents.transaction_pressure.force_policy_denied";
  }
  return "agents.transaction_pressure.force_policy_denied";
}

}  // namespace

const char* TransactionPressureManagerDecisionKindName(
    TransactionPressureManagerDecisionKind decision) {
  switch (decision) {
    case TransactionPressureManagerDecisionKind::no_action:
      return "no_action";
    case TransactionPressureManagerDecisionKind::warn_notify:
      return "warn_notify";
    case TransactionPressureManagerDecisionKind::request_restart:
      return "request_restart";
    case TransactionPressureManagerDecisionKind::request_reauth:
      return "request_reauth";
    case TransactionPressureManagerDecisionKind::request_cancel:
      return "request_cancel";
    case TransactionPressureManagerDecisionKind::force_rollback_replacement:
      return "force_rollback_replacement";
    case TransactionPressureManagerDecisionKind::force_commit_replacement:
      return "force_commit_replacement";
    case TransactionPressureManagerDecisionKind::force_restart_replacement:
      return "force_restart_replacement";
    case TransactionPressureManagerDecisionKind::denied_non_authoritative:
      return "denied_non_authoritative";
    case TransactionPressureManagerDecisionKind::refused:
      return "refused";
  }
  return "refused";
}

const char* TransactionPressureForceActionName(TransactionPressureForceAction action) {
  switch (action) {
    case TransactionPressureForceAction::none:
      return "none";
    case TransactionPressureForceAction::rollback:
      return "rollback";
    case TransactionPressureForceAction::commit:
      return "commit";
    case TransactionPressureForceAction::restart:
      return "restart";
  }
  return "none";
}

TransactionPressureManagerPolicy DefaultTransactionPressureManagerPolicy() {
  TransactionPressureManagerPolicy policy;
  return policy;
}

TransactionPressureManagerTickResult EvaluateTransactionPressureManagerTick(
    const AuthoritativeCleanupHorizonRequest& horizon_request,
    const std::vector<TransactionPressureSessionSnapshot>& sessions,
    const TransactionPressureManagerPolicy& policy) {
  auto horizon = mga::ComputeAuthoritativeCleanupHorizon(horizon_request);
  if (!PolicyShapeValid(policy)) {
    auto result = Refused("TX_PRESSURE_MANAGER.POLICY_INVALID",
                          "agents.transaction_pressure.policy_invalid",
                          "valid long transaction policy with always-in-transaction semantics is required");
    result.horizon = std::move(horizon);
    AddBaseEvidence(&result, policy, sessions);
    return result;
  }
  if (!horizon.ok()) {
    auto result = DeniedNonAuthoritative(
        "TX_PRESSURE_MANAGER.DENIED_NON_AUTHORITATIVE_ACTION",
        "agents.transaction_pressure.cleanup_horizon_not_authoritative",
        horizon.diagnostic.diagnostic_code.empty()
            ? "authoritative cleanup horizon is required"
            : horizon.diagnostic.diagnostic_code);
    result.horizon = std::move(horizon);
    AddBaseEvidence(&result, policy, sessions);
    return result;
  }

  for (const auto& session : sessions) {
    if (!SessionShapeValid(session) ||
        !session.session_authoritative ||
        !session.transaction_binding_authoritative ||
        !session.authorization_session_bound ||
        session.parser_state_claimed_authority ||
        session.client_state_claimed_authority) {
      auto result = DeniedNonAuthoritative(
          "TX_PRESSURE_MANAGER.DENIED_NON_AUTHORITATIVE_ACTION",
          "agents.transaction_pressure.session_not_authoritative",
          session.stable_session_id.empty() ? "session_snapshot_invalid"
                                            : session.stable_session_id);
      result.horizon = std::move(horizon);
      AddBaseEvidence(&result, policy, sessions);
      return result;
    }
  }

  const auto* selected = SelectOldestIdleBlockingSession(horizon, sessions);
  TransactionPressureManagerTickResult result;
  if (selected == nullptr ||
      selected->idle_microseconds < policy.warn_after_idle_microseconds) {
    result = DecisionResult(TransactionPressureManagerDecisionKind::no_action,
                            "TX_PRESSURE_MANAGER.NO_ACTION",
                            "agents.transaction_pressure.no_action",
                            "no long idle authoritative transaction blocker reached policy threshold");
  } else {
    if (!selected->warning_already_notified) {
      result = DecisionResult(TransactionPressureManagerDecisionKind::warn_notify,
                              "TX_PRESSURE_MANAGER.WARN_NOTIFY",
                              "agents.transaction_pressure.warn_notify",
                              selected->stable_session_id);
    } else if (selected->idle_microseconds >= policy.force_after_idle_microseconds) {
      if (!policy.force_authority_gate_present ||
          !policy.force_authority_gate_allows ||
          !ForcePolicyAllows(policy)) {
        result = DeniedNonAuthoritative(
            "TX_PRESSURE_MANAGER.DENIED_NON_AUTHORITATIVE_ACTION",
            "agents.transaction_pressure.force_denied_non_authoritative",
            selected->stable_session_id);
      } else {
        result = DecisionResult(ForceDecisionKind(policy.force_action),
                                ForceDiagnosticCode(policy.force_action),
                                ForceMessageKey(policy.force_action),
                                selected->stable_session_id);
      }
    } else if (selected->cancel_requested_by_policy &&
               policy.request_cancel_allowed &&
               selected->idle_microseconds >=
                   policy.request_cancel_after_idle_microseconds) {
      result = DecisionResult(TransactionPressureManagerDecisionKind::request_cancel,
                              "TX_PRESSURE_MANAGER.REQUEST_CANCEL",
                              "agents.transaction_pressure.request_cancel",
                              selected->stable_session_id);
    } else if (selected->reauth_required_by_policy &&
               policy.request_reauth_allowed &&
               selected->idle_microseconds >=
                   policy.request_reauth_after_idle_microseconds) {
      result = DecisionResult(TransactionPressureManagerDecisionKind::request_reauth,
                              "TX_PRESSURE_MANAGER.REQUEST_REAUTH",
                              "agents.transaction_pressure.request_reauth",
                              selected->stable_session_id);
    } else if (policy.request_restart_allowed &&
               selected->idle_microseconds >=
                   policy.request_restart_after_idle_microseconds) {
      result = DecisionResult(TransactionPressureManagerDecisionKind::request_restart,
                              "TX_PRESSURE_MANAGER.REQUEST_RESTART",
                              "agents.transaction_pressure.request_restart",
                              selected->stable_session_id);
    } else {
      result = DecisionResult(TransactionPressureManagerDecisionKind::warn_notify,
                              "TX_PRESSURE_MANAGER.WARN_NOTIFY",
                              "agents.transaction_pressure.warn_notify",
                              selected->stable_session_id);
    }
    AddSelectedSessionEvidence(&result, *selected);
  }

  result.horizon = std::move(horizon);
  result.cleanup_horizon = result.horizon.cleanup_horizon;
  if (result.replacement_transaction_required) {
    AddEvidence(&result,
                "replacement_transaction_rule",
                "must_open_before_client_ready");
    AddEvidence(&result,
                "always_active_transaction_replacement",
                "required");
    AddEvidence(&result,
                "force_action",
                TransactionPressureForceActionName(policy.force_action));
  }
  AddBaseEvidence(&result, policy, sessions);
  return result;
}

DiagnosticRecord MakeTransactionPressureManagerDiagnostic(Status status,
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
                        "core.agents.transaction_pressure_manager");
}

// Canonical transaction_pressure_manager behavior is registered in CanonicalAgentRegistry().
// SEARCH_KEY: SB_AGENT_IMPLEMENTATION_transaction_pressure_manager
// DPC_TRANSACTION_PRESSURE_MANAGER_LONG_IDLE
const char* transaction_pressure_manager_implementation_anchor() {
  return "transaction_pressure_manager";
}

}  // namespace scratchbird::core::agents::implemented_agents
