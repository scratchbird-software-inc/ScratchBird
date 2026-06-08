// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "session_registry.hpp"

#include <cstdlib>
#include <iostream>
#include <string_view>

namespace {

namespace server = scratchbird::server;

void Require(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(EXIT_FAILURE);
  }
}

server::ServerSessionRecord ActiveSession() {
  server::ServerSessionRecord session;
  session.local_transaction_id = 42;
  session.snapshot_visible_through_local_transaction_id = 41;
  return session;
}

server::ServerSessionRecord CleanSession() {
  return server::ServerSessionRecord{};
}

server::ServerDriverTransactionDecision Decide(
    const server::ServerSessionRecord& session,
    server::ServerDriverTransactionEvent event,
    server::ServerDriverTransactionDecisionInput input = {}) {
  input.event = event;
  return server::ClassifyDriverTransactionEvent(session, input);
}

bool HasDiagnostic(const server::ServerDriverTransactionDecision& decision,
                   std::string_view code) {
  for (const auto& diagnostic : decision.diagnostics) {
    if (diagnostic.code == code) return true;
  }
  return false;
}

void CheckBoundaryMapping() {
  const auto active = ActiveSession();
  const auto attach = Decide(CleanSession(),
                             server::ServerDriverTransactionEvent::kAttachInitialBoundary);
  Require(attach.accepted && attach.opens_replacement_boundary &&
              attach.boundary_state == "TX_BOUNDARY_ACTIVE",
          "initial attach did not open an active MGA transaction boundary");

  const auto autocommit_success = Decide(
      active, server::ServerDriverTransactionEvent::kAutocommitStatementSucceeded);
  Require(autocommit_success.accepted &&
              autocommit_success.opens_replacement_boundary &&
              autocommit_success.durable_state == "TX_DURABLE_COMMITTED" &&
              !autocommit_success.driver_may_retry,
          "autocommit success did not commit and reopen via engine MGA finality");

  const auto autocommit_failure = Decide(
      active, server::ServerDriverTransactionEvent::kAutocommitStatementFailed);
  Require(autocommit_failure.accepted &&
              autocommit_failure.opens_replacement_boundary &&
              autocommit_failure.durable_state == "TX_DURABLE_ROLLED_BACK",
          "autocommit failure did not rollback and reopen via engine MGA finality");

  const auto commit = Decide(active, server::ServerDriverTransactionEvent::kCommitCompleted);
  Require(commit.accepted && commit.opens_replacement_boundary &&
              commit.finality_state == "committed_by_engine_inventory",
          "commit completion did not immediately open the next boundary");

  const auto rollback = Decide(active, server::ServerDriverTransactionEvent::kRollbackCompleted);
  Require(rollback.accepted && rollback.opens_replacement_boundary &&
              rollback.finality_state == "rolled_back_by_engine_inventory",
          "rollback completion did not immediately open the next boundary");
}

void CheckCancelResetPoolAndSavepoint() {
  const auto active = ActiveSession();
  const auto cancel = Decide(active, server::ServerDriverTransactionEvent::kCancelStatement);
  Require(cancel.accepted && cancel.preserves_current_boundary &&
              cancel.sqlstate == "57014" && !cancel.driver_may_retry,
          "cancel did not preserve current transaction without hidden retry");

  server::ServerDriverTransactionDecisionInput active_cursor;
  active_cursor.active_cursor = true;
  const auto reset = Decide(active,
                            server::ServerDriverTransactionEvent::kResetSession,
                            active_cursor);
  Require(!reset.accepted &&
              HasDiagnostic(reset, "SERVER.DRIVER_TX.RESET_REQUIRES_CLEAN_BOUNDARY"),
          "reset during active transaction/cursor did not fail closed");

  const auto pool_return = Decide(active,
                                  server::ServerDriverTransactionEvent::kPoolReturn,
                                  active_cursor);
  Require(!pool_return.accepted &&
              HasDiagnostic(pool_return, "SERVER.DRIVER_TX.POOL_RETURN_REQUIRES_CLEAN_BOUNDARY"),
          "pool return during active transaction/cursor did not fail closed");

  const auto savepoint = Decide(active,
                                server::ServerDriverTransactionEvent::kSavepointOperation);
  Require(savepoint.accepted && savepoint.preserves_current_boundary &&
              !savepoint.opens_replacement_boundary &&
              savepoint.durable_state == "TX_DURABLE_ACTIVE",
          "savepoint operation created independent transaction authority");

  const auto clean_reset = Decide(CleanSession(),
                                  server::ServerDriverTransactionEvent::kResetSession);
  Require(clean_reset.accepted && clean_reset.opens_replacement_boundary,
          "clean reset did not reopen a normal MGA boundary");
}

void CheckPreparedXaDormantAndReconnect() {
  const auto active = ActiveSession();
  const auto prepared = Decide(
      active, server::ServerDriverTransactionEvent::kPrepareTransactionCompleted);
  Require(prepared.accepted && prepared.opens_replacement_boundary &&
              prepared.boundary_state == "TX_BOUNDARY_PREPARED_HANDOFF" &&
              prepared.durable_state == "TX_DURABLE_PREPARED",
          "prepared transaction handoff did not open the next boundary");

  server::ServerDriverTransactionDecisionInput reconnect_after_prepare;
  reconnect_after_prepare.prepared_transaction_present = true;
  reconnect_after_prepare.engine_finality_known = false;
  const auto reconnect = Decide(
      CleanSession(),
      server::ServerDriverTransactionEvent::kReconnectAfterDisconnect,
      reconnect_after_prepare);
  Require(reconnect.accepted && reconnect.invalidates_session &&
              reconnect.must_query_engine_finality &&
              reconnect.requires_explicit_engine_recovery &&
              !reconnect.driver_may_retry,
          "reconnect after prepared/unknown finality implied hidden recovery or retry");

  server::ServerDriverTransactionDecisionInput xa;
  xa.prepared_transaction_present = true;
  const auto xa_refused = Decide(CleanSession(),
                                 server::ServerDriverTransactionEvent::kXaRecoverPrepared,
                                 xa);
  Require(!xa_refused.accepted &&
              HasDiagnostic(xa_refused, "SERVER.DRIVER_TX.XA_LIMBO_RECOVERY_REQUIRED"),
          "XA limbo recovery without engine authority did not fail closed");

  server::ServerDriverTransactionDecisionInput dormant_detach;
  dormant_detach.dormant_reattach_enabled = true;
  const auto dormant = Decide(active,
                              server::ServerDriverTransactionEvent::kDormantDetach,
                              dormant_detach);
  Require(dormant.accepted &&
              dormant.boundary_state == "TX_BOUNDARY_DORMANT_RETAINED" &&
              dormant.durable_state == "TX_DURABLE_DORMANT",
          "explicit dormant detach did not preserve the engine transaction");

  const auto implicit_reattach = Decide(
      CleanSession(), server::ServerDriverTransactionEvent::kDormantReattach);
  Require(!implicit_reattach.accepted &&
              HasDiagnostic(implicit_reattach,
                            "SERVER.DRIVER_TX.DORMANT_REATTACH_TOKEN_REQUIRED"),
          "dormant reattach without explicit token was admitted");

  server::ServerDriverTransactionDecisionInput explicit_reattach;
  explicit_reattach.explicit_dormant_token = true;
  explicit_reattach.dormant_reattach_enabled = true;
  explicit_reattach.server_admitted_reattach = true;
  const auto reattached = Decide(
      CleanSession(),
      server::ServerDriverTransactionEvent::kDormantReattach,
      explicit_reattach);
  Require(reattached.accepted &&
              reattached.boundary_state == "TX_BOUNDARY_ACTIVE" &&
              reattached.preserves_current_boundary,
          "explicit dormant reattach was not bound to engine admission");
}

void CheckNoHiddenRetry() {
  const auto active = ActiveSession();
  server::ServerDriverTransactionDecisionInput unknown;
  unknown.engine_finality_known = false;
  const auto unknown_retry = Decide(
      active,
      server::ServerDriverTransactionEvent::kRetryAfterUnknownFinality,
      unknown);
  Require(!unknown_retry.accepted && unknown_retry.must_query_engine_finality &&
              !unknown_retry.driver_may_retry &&
              HasDiagnostic(unknown_retry,
                            "SERVER.DRIVER_TX.RETRY_REQUIRES_FINALITY_QUERY"),
          "retry after unknown finality did not require engine finality query");

  server::ServerDriverTransactionDecisionInput side_effect;
  side_effect.statement_has_side_effects = true;
  side_effect.engine_finality_known = true;
  const auto side_effect_retry = Decide(
      active,
      server::ServerDriverTransactionEvent::kRetryAfterUnknownFinality,
      side_effect);
  Require(!side_effect_retry.accepted &&
              HasDiagnostic(side_effect_retry,
                            "SERVER.DRIVER_TX.HIDDEN_RETRY_FORBIDDEN"),
          "side-effecting hidden retry was not refused");

  server::ServerDriverTransactionDecisionInput idempotent;
  idempotent.statement_has_side_effects = false;
  idempotent.engine_reported_idempotent = true;
  idempotent.caller_acknowledged_retry_boundary = true;
  const auto caller_retry = Decide(
      active,
      server::ServerDriverTransactionEvent::kRetryAfterUnknownFinality,
      idempotent);
  Require(caller_retry.accepted && caller_retry.driver_may_retry &&
              caller_retry.opens_replacement_boundary,
          "caller-controlled idempotent retry was not admitted at a fresh boundary");
}

}  // namespace

int main() {
  CheckBoundaryMapping();
  CheckCancelResetPoolAndSavepoint();
  CheckPreparedXaDormantAndReconnect();
  CheckNoHiddenRetry();
  std::cout << "mga_driver_transaction_reconciliation=passed\n";
  return EXIT_SUCCESS;
}
