// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "agent_runtime.hpp"
#include "transaction_cleanup_horizon_service.hpp"

#include <string>
#include <vector>

namespace scratchbird::core::agents::implemented_agents {

using scratchbird::core::platform::DiagnosticRecord;
using scratchbird::core::platform::Status;
using scratchbird::core::platform::TypedUuid;
using scratchbird::core::platform::u32;
using scratchbird::core::platform::u64;
using scratchbird::transaction::mga::AuthoritativeCleanupHorizonRequest;
using scratchbird::transaction::mga::AuthoritativeCleanupHorizonResult;
using scratchbird::transaction::mga::LocalTransactionId;

// DPC_TRANSACTION_PRESSURE_MANAGER_LONG_IDLE
enum class TransactionPressureManagerDecisionKind : u32 {
  no_action,
  warn_notify,
  request_restart,
  request_reauth,
  request_cancel,
  force_rollback_replacement,
  force_commit_replacement,
  force_restart_replacement,
  denied_non_authoritative,
  refused
};

enum class TransactionPressureForceAction : u32 {
  none,
  rollback,
  commit,
  restart
};

struct TransactionPressureSessionSnapshot {
  std::string stable_session_id;
  std::string stable_connection_id;
  std::string stable_principal_id;
  LocalTransactionId current_local_transaction_id;
  u64 idle_microseconds = 0;
  bool session_authoritative = true;
  bool transaction_binding_authoritative = true;
  bool authorization_session_bound = true;
  bool active_transaction = true;
  bool engine_owns_transaction = true;
  bool warning_already_notified = false;
  bool reauth_required_by_policy = false;
  bool cancel_requested_by_policy = false;
  bool parser_state_claimed_authority = false;
  bool client_state_claimed_authority = false;
};

struct TransactionPressureManagerPolicy {
  TypedUuid database_uuid;
  TypedUuid policy_uuid;
  bool present = true;
  bool valid = true;
  bool enabled = true;
  bool scope_compatible = true;
  bool always_in_transaction_policy = true;
  bool autocommit_emulation_is_transaction_control_only = true;
  bool notify_allowed = true;
  bool request_restart_allowed = true;
  bool request_reauth_allowed = true;
  bool request_cancel_allowed = false;
  bool force_authority_gate_present = false;
  bool force_authority_gate_allows = false;
  bool force_rollback_allowed = false;
  bool force_commit_allowed = false;
  bool force_restart_allowed = false;
  TransactionPressureForceAction force_action = TransactionPressureForceAction::rollback;
  u64 warn_after_idle_microseconds = 300000000;
  u64 request_restart_after_idle_microseconds = 900000000;
  u64 request_reauth_after_idle_microseconds = 1200000000;
  u64 request_cancel_after_idle_microseconds = 1500000000;
  u64 force_after_idle_microseconds = 1800000000;
};

struct TransactionPressureEvidenceField {
  std::string key;
  std::string value;
};

struct TransactionPressureManagerTickResult {
  Status status;
  TransactionPressureManagerDecisionKind decision =
      TransactionPressureManagerDecisionKind::refused;
  DiagnosticRecord diagnostic;
  AuthoritativeCleanupHorizonResult horizon;
  LocalTransactionId selected_local_transaction_id;
  LocalTransactionId cleanup_horizon;
  std::string selected_session_id;
  std::string selected_principal_id;
  std::vector<TransactionPressureEvidenceField> evidence;
  bool fail_closed = false;
  bool notification_required = false;
  bool restart_requested = false;
  bool reauth_requested = false;
  bool cancel_requested = false;
  bool force_action_requested = false;
  bool replacement_transaction_required = false;
  bool denied_non_authoritative = false;
  bool action_mutates_transaction_if_accepted_by_server = false;
  bool parser_finality_authority = false;
  bool client_state_authority = false;

  bool ok() const { return status.ok() && !fail_closed; }
};

const char* TransactionPressureManagerDecisionKindName(
    TransactionPressureManagerDecisionKind decision);
const char* TransactionPressureForceActionName(TransactionPressureForceAction action);
TransactionPressureManagerPolicy DefaultTransactionPressureManagerPolicy();
TransactionPressureManagerTickResult EvaluateTransactionPressureManagerTick(
    const AuthoritativeCleanupHorizonRequest& horizon_request,
    const std::vector<TransactionPressureSessionSnapshot>& sessions,
    const TransactionPressureManagerPolicy& policy);
DiagnosticRecord MakeTransactionPressureManagerDiagnostic(Status status,
                                                          std::string diagnostic_code,
                                                          std::string message_key,
                                                          std::string detail = {});

const char* transaction_pressure_manager_implementation_anchor();

}  // namespace scratchbird::core::agents::implemented_agents
