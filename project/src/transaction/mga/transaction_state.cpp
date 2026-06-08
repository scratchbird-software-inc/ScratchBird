// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "transaction_state.hpp"

#include <utility>
#include <vector>

namespace scratchbird::transaction::mga {
namespace {

using scratchbird::core::platform::DiagnosticArgument;
using scratchbird::core::platform::MakeDiagnostic;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;

Status TransactionOkStatus() {
  return {StatusCode::ok, Severity::info, Subsystem::transaction_mga};
}

Status TransactionErrorStatus() {
  return {StatusCode::platform_required_feature_missing, Severity::error, Subsystem::transaction_mga};
}

TransactionStateTransition Transition(TransactionState from,
                                      TransactionState to,
                                      TransactionTransitionClass transition_class,
                                      bool recovery_only,
                                      const char* stable_name) {
  TransactionStateTransition transition;
  transition.from = from;
  transition.to = to;
  transition.transition_class = transition_class;
  transition.recovery_only = recovery_only;
  transition.stable_name = stable_name;
  return transition;
}

}  // namespace

const char* TransactionScopeName(TransactionScope scope) {
  switch (scope) {
    case TransactionScope::local_node: return "local_node";
    case TransactionScope::cluster_global: return "cluster_global";
    case TransactionScope::unknown: return "unknown";
  }
  return "unknown";
}

const char* TransactionStateName(TransactionState state) {
  switch (state) {
    case TransactionState::none: return "none";
    case TransactionState::created: return "created";
    case TransactionState::active: return "active";
    case TransactionState::preparing: return "preparing";
    case TransactionState::prepared: return "prepared";
    case TransactionState::committing: return "committing";
    case TransactionState::committed: return "committed";
    case TransactionState::rolling_back: return "rolling_back";
    case TransactionState::rolled_back: return "rolled_back";
    case TransactionState::limbo: return "limbo";
    case TransactionState::recovering: return "recovering";
    case TransactionState::failed_terminal: return "failed_terminal";
    case TransactionState::archived: return "archived";
    case TransactionState::read_only_active: return "read_only_active";
  }
  return "unknown";
}

const char* TransactionTransitionClassName(TransactionTransitionClass transition_class) {
  switch (transition_class) {
    case TransactionTransitionClass::user: return "user";
    case TransactionTransitionClass::coordinator: return "coordinator";
    case TransactionTransitionClass::recovery: return "recovery";
    case TransactionTransitionClass::cleanup: return "cleanup";
    case TransactionTransitionClass::invalid: return "invalid";
  }
  return "invalid";
}

LocalTransactionId MakeLocalTransactionId(u64 value) {
  return {value};
}

bool IsTerminalTransactionState(TransactionState state) {
  return state == TransactionState::committed || state == TransactionState::rolled_back ||
         state == TransactionState::failed_terminal || state == TransactionState::archived;
}

const std::vector<TransactionStateTransition>& BuiltinTransactionStateTransitions() {
  static const std::vector<TransactionStateTransition> transitions = {
      Transition(TransactionState::none,
                 TransactionState::created,
                 TransactionTransitionClass::coordinator,
                 false,
                 "allocate_transaction"),
      Transition(TransactionState::created,
                 TransactionState::active,
                 TransactionTransitionClass::user,
                 false,
                 "begin_transaction"),
      Transition(TransactionState::created,
                 TransactionState::read_only_active,
                 TransactionTransitionClass::user,
                 false,
                 "begin_read_only_transaction"),
      Transition(TransactionState::created,
                 TransactionState::rolling_back,
                 TransactionTransitionClass::user,
                 false,
                 "abort_created_transaction"),
      Transition(TransactionState::read_only_active,
                 TransactionState::committing,
                 TransactionTransitionClass::coordinator,
                 false,
                 "commit_read_only_transaction"),
      Transition(TransactionState::read_only_active,
                 TransactionState::rolling_back,
                 TransactionTransitionClass::user,
                 false,
                 "rollback_read_only_transaction"),
      Transition(TransactionState::active,
                 TransactionState::preparing,
                 TransactionTransitionClass::coordinator,
                 false,
                 "enter_prepare"),
      Transition(TransactionState::preparing,
                 TransactionState::prepared,
                 TransactionTransitionClass::coordinator,
                 false,
                 "finish_prepare"),
      Transition(TransactionState::prepared,
                 TransactionState::committing,
                 TransactionTransitionClass::coordinator,
                 false,
                 "commit_decision_known"),
      Transition(TransactionState::committing,
                 TransactionState::committed,
                 TransactionTransitionClass::coordinator,
                 false,
                 "publish_commit"),
      Transition(TransactionState::active,
                 TransactionState::rolling_back,
                 TransactionTransitionClass::user,
                 false,
                 "rollback_active"),
      Transition(TransactionState::preparing,
                 TransactionState::rolling_back,
                 TransactionTransitionClass::coordinator,
                 false,
                 "rollback_preparing"),
      Transition(TransactionState::prepared,
                 TransactionState::rolling_back,
                 TransactionTransitionClass::coordinator,
                 false,
                 "rollback_prepared"),
      Transition(TransactionState::rolling_back,
                 TransactionState::rolled_back,
                 TransactionTransitionClass::coordinator,
                 false,
                 "publish_rollback"),
      Transition(TransactionState::prepared,
                 TransactionState::limbo,
                 TransactionTransitionClass::recovery,
                 false,
                 "prepared_enter_limbo"),
      Transition(TransactionState::committing,
                 TransactionState::limbo,
                 TransactionTransitionClass::recovery,
                 false,
                 "committing_enter_limbo"),
      Transition(TransactionState::limbo,
                 TransactionState::recovering,
                 TransactionTransitionClass::recovery,
                 true,
                 "recover_limbo"),
      Transition(TransactionState::recovering,
                 TransactionState::committed,
                 TransactionTransitionClass::recovery,
                 true,
                 "recovered_commit"),
      Transition(TransactionState::recovering,
                 TransactionState::rolled_back,
                 TransactionTransitionClass::recovery,
                 true,
                 "recovered_rollback"),
      Transition(TransactionState::recovering,
                 TransactionState::failed_terminal,
                 TransactionTransitionClass::recovery,
                 true,
                 "recovery_failed_terminal"),
      Transition(TransactionState::committed,
                 TransactionState::archived,
                 TransactionTransitionClass::cleanup,
                 false,
                 "archive_committed_transaction"),
      Transition(TransactionState::rolled_back,
                 TransactionState::archived,
                 TransactionTransitionClass::cleanup,
                 false,
                 "archive_rolled_back_transaction"),
      Transition(TransactionState::failed_terminal,
                 TransactionState::archived,
                 TransactionTransitionClass::cleanup,
                 false,
                 "archive_failed_terminal_transaction"),
  };
  return transitions;
}

TransactionIdentityResult MakeTransactionIdentity(LocalTransactionId local_id,
                                                  TypedUuid transaction_uuid,
                                                  TransactionScope scope) {
  TransactionIdentity identity;
  identity.local_id = local_id;
  identity.transaction_uuid = transaction_uuid;
  identity.scope = scope;
  return ValidateTransactionIdentity(identity);
}

TransactionIdentityResult ValidateTransactionIdentity(const TransactionIdentity& identity) {
  TransactionIdentityResult result;
  result.status = TransactionOkStatus();
  result.identity = identity;

  if (!identity.local_id.valid()) {
    result.status = TransactionErrorStatus();
    result.diagnostic = MakeTransactionDiagnostic(result.status,
                                                  "SB-TXN-INVALID-LOCAL-TRANSACTION-ID",
                                                  "transaction.invalid_local_transaction_id");
    return result;
  }

  if (identity.transaction_uuid.kind != UuidKind::transaction || !identity.transaction_uuid.valid()) {
    result.status = TransactionErrorStatus();
    result.diagnostic = MakeTransactionDiagnostic(result.status,
                                                  "SB-TXN-INVALID-TRANSACTION-UUID-KIND",
                                                  "transaction.invalid_transaction_uuid_kind");
    return result;
  }

  if (!scratchbird::core::uuid::IsEngineIdentityUuid(identity.transaction_uuid.value)) {
    result.status = TransactionErrorStatus();
    result.diagnostic = MakeTransactionDiagnostic(result.status,
                                                  "SB-TXN-UUID-MUST-BE-V7",
                                                  "transaction.transaction_uuid_must_be_v7");
    return result;
  }

  if (identity.scope == TransactionScope::unknown) {
    result.status = TransactionErrorStatus();
    result.diagnostic = MakeTransactionDiagnostic(result.status,
                                                  "SB-TXN-UNKNOWN-TRANSACTION-SCOPE",
                                                  "transaction.unknown_transaction_scope");
    return result;
  }

  return result;
}

TransactionTransitionResult CheckTransactionStateTransition(TransactionState from,
                                                           TransactionState to,
                                                           bool recovery_context) {
  TransactionTransitionResult result;
  result.status = TransactionErrorStatus();
  result.allowed = false;

  for (const TransactionStateTransition& transition : BuiltinTransactionStateTransitions()) {
    if (transition.from == from && transition.to == to) {
      result.transition = transition;
      if (transition.recovery_only && !recovery_context) {
        result.diagnostic = MakeTransactionDiagnostic(result.status,
                                                     "SB-TXN-TRANSITION-REQUIRES-RECOVERY-CONTEXT",
                                                     "transaction.transition_requires_recovery_context",
                                                     transition.stable_name);
        return result;
      }

      result.status = TransactionOkStatus();
      result.allowed = true;
      return result;
    }
  }

  result.diagnostic = MakeTransactionDiagnostic(result.status,
                                               "SB-TXN-STATE-INVALID-TRANSITION",
                                               "transaction.illegal_state_transition",
                                               std::string(TransactionStateName(from)) + "->" +
                                                   TransactionStateName(to));
  return result;
}

DiagnosticRecord MakeTransactionDiagnostic(Status status,
                                           std::string diagnostic_code,
                                           std::string message_key,
                                           std::string detail) {
  std::vector<DiagnosticArgument> arguments;
  if (!detail.empty()) {
    arguments.push_back({"detail", detail});
  }

  return MakeDiagnostic(status.code,
                        status.severity,
                        status.subsystem,
                        std::move(diagnostic_code),
                        std::move(message_key),
                        std::move(arguments),
                        {},
                        "transaction.mga.state");
}

}  // namespace scratchbird::transaction::mga
