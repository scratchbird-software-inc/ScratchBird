// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "agents/transaction_pressure_manager.hpp"
#include "session_registry.hpp"
#include "uuid.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace agents = scratchbird::core::agents::implemented_agents;
namespace mga = scratchbird::transaction::mga;
namespace platform = scratchbird::core::platform;
namespace server = scratchbird::server;
namespace uuid = scratchbird::core::uuid;

constexpr std::string_view kGateSearchKey =
    "DPC_TRANSACTION_PRESSURE_MANAGER_LONG_IDLE_GATE";

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

platform::u64 NextMillis() {
  static platform::u64 next = 1779510000000ull;
  return ++next;
}

platform::TypedUuid NewUuid(platform::UuidKind kind) {
  const auto generated = uuid::GenerateEngineIdentityV7(kind, NextMillis());
  Require(generated.ok(), "DPC-031 generated UUID creation failed");
  return generated.value;
}

mga::TransactionIdentity NewIdentity(platform::u64 local_id) {
  const auto identity = mga::MakeTransactionIdentity(
      mga::MakeLocalTransactionId(local_id),
      NewUuid(platform::UuidKind::transaction),
      mga::TransactionScope::local_node);
  Require(identity.ok(), "DPC-031 transaction identity creation failed");
  return identity.identity;
}

mga::TransactionInventoryEntry Entry(platform::u64 local_id,
                                     mga::TransactionState state) {
  mga::TransactionInventoryEntry entry;
  entry.identity = NewIdentity(local_id);
  entry.state = state;
  entry.begin_unix_epoch_millis = NextMillis();
  if (mga::IsTerminalTransactionState(state)) {
    entry.final_unix_epoch_millis = NextMillis();
    entry.evidence_record_written = true;
  }
  return entry;
}

mga::LocalTransactionInventory Inventory(
    std::vector<mga::TransactionInventoryEntry> entries,
    platform::u64 next_local_transaction_id) {
  mga::LocalTransactionInventory inventory;
  inventory.entries = std::move(entries);
  inventory.next_local_transaction_id = next_local_transaction_id;
  return inventory;
}

mga::AuthoritativeCleanupHorizonRequest HorizonRequest(
    mga::LocalTransactionInventory inventory,
    platform::u64 active_transaction_id = 2) {
  mga::AuthoritativeCleanupHorizonRequest request;
  request.inventory = std::move(inventory);
  request.inventory_authoritative = true;
  request.inventory_complete = true;
  request.active_snapshot_inventory_authoritative = true;
  request.always_in_transaction_policy = true;
  request.always_active_session_inventory_authoritative = true;
  request.always_active_sessions.push_back(
      {"session:primary", mga::MakeLocalTransactionId(active_transaction_id), true});
  return request;
}

mga::AuthoritativeCleanupHorizonRequest StandardHorizonRequest() {
  return HorizonRequest(Inventory({
      Entry(1, mga::TransactionState::committed),
      Entry(2, mga::TransactionState::active),
      Entry(3, mga::TransactionState::committed),
  }, 4));
}

agents::TransactionPressureSessionSnapshot Session(platform::u64 local_id,
                                                   platform::u64 idle_us,
                                                   bool warned = true) {
  agents::TransactionPressureSessionSnapshot session;
  session.stable_session_id = "session:primary";
  session.stable_connection_id = "connection:primary";
  session.stable_principal_id = "principal:primary";
  session.current_local_transaction_id = mga::MakeLocalTransactionId(local_id);
  session.idle_microseconds = idle_us;
  session.warning_already_notified = warned;
  return session;
}

agents::TransactionPressureManagerPolicy Policy() {
  auto policy = agents::DefaultTransactionPressureManagerPolicy();
  policy.warn_after_idle_microseconds = 100;
  policy.request_restart_after_idle_microseconds = 200;
  policy.request_reauth_after_idle_microseconds = 250;
  policy.request_cancel_after_idle_microseconds = 275;
  policy.force_after_idle_microseconds = 300;
  return policy;
}

bool HasEvidence(const agents::TransactionPressureManagerTickResult& result,
                 std::string_view key,
                 std::string_view value) {
  for (const auto& field : result.evidence) {
    if (field.key == key && field.value == value) {
      return true;
    }
  }
  return false;
}

void RequireDecision(const agents::TransactionPressureManagerTickResult& result,
                     agents::TransactionPressureManagerDecisionKind decision,
                     std::string_view code) {
  Require(result.decision == decision, "DPC-031 decision mismatch");
  Require(result.diagnostic.diagnostic_code == code,
          "DPC-031 diagnostic code mismatch");
  Require(HasEvidence(result, "decision",
                      agents::TransactionPressureManagerDecisionKindName(decision)),
          "DPC-031 decision evidence missing");
  Require(HasEvidence(result, "parser_finality_authority", "false"),
          "DPC-031 parser authority evidence missing");
  Require(HasEvidence(result, "client_state_authority", "false"),
          "DPC-031 client authority evidence missing");
  Require(HasEvidence(result, "cleanup_horizon_service",
                      "dpc030_authoritative_cleanup_horizon_v1"),
          "DPC-031 DPC-030 cleanup horizon evidence missing");
}

void TestNoActionAndWarn() {
  const auto no_action = agents::EvaluateTransactionPressureManagerTick(
      StandardHorizonRequest(),
      {Session(2, 50, false)},
      Policy());
  Require(no_action.ok(), "DPC-031 no-action decision failed");
  RequireDecision(no_action,
                  agents::TransactionPressureManagerDecisionKind::no_action,
                  "TX_PRESSURE_MANAGER.NO_ACTION");
  Require(!no_action.notification_required, "DPC-031 no-action notified");

  const auto warn = agents::EvaluateTransactionPressureManagerTick(
      StandardHorizonRequest(),
      {Session(2, 150, false)},
      Policy());
  Require(warn.ok(), "DPC-031 warn decision failed");
  RequireDecision(warn,
                  agents::TransactionPressureManagerDecisionKind::warn_notify,
                  "TX_PRESSURE_MANAGER.WARN_NOTIFY");
  Require(warn.notification_required, "DPC-031 warn did not notify");
  Require(!warn.replacement_transaction_required,
          "DPC-031 warning required replacement transaction");
}

void TestRequestActions() {
  const auto restart = agents::EvaluateTransactionPressureManagerTick(
      StandardHorizonRequest(),
      {Session(2, 225, true)},
      Policy());
  Require(restart.ok(), "DPC-031 restart request failed");
  RequireDecision(restart,
                  agents::TransactionPressureManagerDecisionKind::request_restart,
                  "TX_PRESSURE_MANAGER.REQUEST_RESTART");
  Require(restart.restart_requested, "DPC-031 restart flag missing");

  auto reauth_session = Session(2, 260, true);
  reauth_session.reauth_required_by_policy = true;
  const auto reauth = agents::EvaluateTransactionPressureManagerTick(
      StandardHorizonRequest(),
      {reauth_session},
      Policy());
  Require(reauth.ok(), "DPC-031 reauth request failed");
  RequireDecision(reauth,
                  agents::TransactionPressureManagerDecisionKind::request_reauth,
                  "TX_PRESSURE_MANAGER.REQUEST_REAUTH");
  Require(reauth.reauth_requested, "DPC-031 reauth flag missing");

  auto cancel_policy = Policy();
  cancel_policy.request_cancel_allowed = true;
  auto cancel_session = Session(2, 280, true);
  cancel_session.cancel_requested_by_policy = true;
  const auto cancel = agents::EvaluateTransactionPressureManagerTick(
      StandardHorizonRequest(),
      {cancel_session},
      cancel_policy);
  Require(cancel.ok(), "DPC-031 cancel request failed");
  RequireDecision(cancel,
                  agents::TransactionPressureManagerDecisionKind::request_cancel,
                  "TX_PRESSURE_MANAGER.REQUEST_CANCEL");
  Require(cancel.cancel_requested, "DPC-031 cancel flag missing");
}

void TestForcedActionsRequirePolicyAuthorityAndReplacementRule() {
  auto force = Policy();
  force.force_authority_gate_present = true;
  force.force_authority_gate_allows = true;

  force.force_action = agents::TransactionPressureForceAction::rollback;
  force.force_rollback_allowed = true;
  const auto rollback = agents::EvaluateTransactionPressureManagerTick(
      StandardHorizonRequest(),
      {Session(2, 350, true)},
      force);
  Require(rollback.ok(), "DPC-031 force rollback failed");
  RequireDecision(
      rollback,
      agents::TransactionPressureManagerDecisionKind::force_rollback_replacement,
      "TX_PRESSURE_MANAGER.FORCE_ROLLBACK_REPLACEMENT");
  Require(rollback.replacement_transaction_required,
          "DPC-031 force rollback replacement rule missing");
  Require(HasEvidence(rollback, "always_active_transaction_replacement", "required"),
          "DPC-031 force rollback replacement evidence missing");

  force.force_action = agents::TransactionPressureForceAction::commit;
  force.force_commit_allowed = true;
  const auto commit = agents::EvaluateTransactionPressureManagerTick(
      StandardHorizonRequest(),
      {Session(2, 350, true)},
      force);
  RequireDecision(commit,
                  agents::TransactionPressureManagerDecisionKind::force_commit_replacement,
                  "TX_PRESSURE_MANAGER.FORCE_COMMIT_REPLACEMENT");

  force.force_action = agents::TransactionPressureForceAction::restart;
  force.force_restart_allowed = true;
  const auto restart = agents::EvaluateTransactionPressureManagerTick(
      StandardHorizonRequest(),
      {Session(2, 350, true)},
      force);
  RequireDecision(
      restart,
      agents::TransactionPressureManagerDecisionKind::force_restart_replacement,
      "TX_PRESSURE_MANAGER.FORCE_RESTART_REPLACEMENT");
}

void TestDeniedNonAuthoritativeActions() {
  auto bad_session = Session(2, 350, true);
  bad_session.transaction_binding_authoritative = false;
  const auto denied_session = agents::EvaluateTransactionPressureManagerTick(
      StandardHorizonRequest(),
      {bad_session},
      Policy());
  Require(!denied_session.ok(), "DPC-031 non-authoritative session passed");
  RequireDecision(denied_session,
                  agents::TransactionPressureManagerDecisionKind::denied_non_authoritative,
                  "TX_PRESSURE_MANAGER.DENIED_NON_AUTHORITATIVE_ACTION");
  Require(denied_session.denied_non_authoritative,
          "DPC-031 denied session flag missing");

  auto inactive_session = Session(2, 350, true);
  inactive_session.active_transaction = false;
  const auto denied_inactive_session = agents::EvaluateTransactionPressureManagerTick(
      StandardHorizonRequest(),
      {inactive_session},
      Policy());
  Require(!denied_inactive_session.ok(),
          "DPC-031 inactive transaction session passed");
  RequireDecision(denied_inactive_session,
                  agents::TransactionPressureManagerDecisionKind::denied_non_authoritative,
                  "TX_PRESSURE_MANAGER.DENIED_NON_AUTHORITATIVE_ACTION");

  auto force_denied = Policy();
  force_denied.force_action = agents::TransactionPressureForceAction::rollback;
  force_denied.force_rollback_allowed = true;
  const auto denied_force = agents::EvaluateTransactionPressureManagerTick(
      StandardHorizonRequest(),
      {Session(2, 350, true)},
      force_denied);
  Require(!denied_force.ok(), "DPC-031 force without authority gate passed");
  RequireDecision(denied_force,
                  agents::TransactionPressureManagerDecisionKind::denied_non_authoritative,
                  "TX_PRESSURE_MANAGER.DENIED_NON_AUTHORITATIVE_ACTION");

  auto non_authoritative_horizon = StandardHorizonRequest();
  non_authoritative_horizon.inventory_authoritative = false;
  const auto denied_horizon = agents::EvaluateTransactionPressureManagerTick(
      non_authoritative_horizon,
      {Session(2, 350, true)},
      Policy());
  Require(!denied_horizon.ok(), "DPC-031 non-authoritative horizon passed");
  RequireDecision(denied_horizon,
                  agents::TransactionPressureManagerDecisionKind::denied_non_authoritative,
                  "TX_PRESSURE_MANAGER.DENIED_NON_AUTHORITATIVE_ACTION");
}

server::ServerTransactionPressureControlInput ServerInput(
    server::ServerTransactionPressureAction action) {
  server::ServerTransactionPressureControlInput input;
  input.action = action;
  input.agent_authoritative = true;
  input.policy_authorized = true;
  input.session_authorization_bound = true;
  input.active_transaction = true;
  input.force_authority_gate = true;
  input.current_local_transaction_id = 2;
  input.replacement_transaction_bound = true;
  input.replacement_local_transaction_id = 4;
  input.stable_session_id = "session:primary";
  input.evidence_id = "dpc031-agent-evidence";
  return input;
}

void TestServerAuthorityBoundary() {
  server::ServerSessionRecord session;
  session.local_transaction_id = 2;

  const auto warn = server::ClassifyServerTransactionPressureControl(
      session,
      ServerInput(server::ServerTransactionPressureAction::kWarnNotify));
  Require(warn.accepted && warn.notifies_client && !warn.mutates_transaction,
          "DPC-031 server warn boundary mismatch");
  Require(warn.diagnostic_code == "SERVER.TRANSACTION_PRESSURE.WARN_NOTIFY",
          "DPC-031 server warn diagnostic mismatch");

  auto denied_input = ServerInput(server::ServerTransactionPressureAction::kForceRollback);
  denied_input.agent_authoritative = false;
  const auto denied = server::ClassifyServerTransactionPressureControl(
      session,
      denied_input);
  Require(!denied.accepted && denied.denied_non_authoritative,
          "DPC-031 server non-authoritative force passed");
  Require(denied.diagnostic_code ==
              "SERVER.TRANSACTION_PRESSURE.DENIED_NON_AUTHORITATIVE",
          "DPC-031 server non-authoritative diagnostic mismatch");

  auto missing_replacement =
      ServerInput(server::ServerTransactionPressureAction::kForceRollback);
  missing_replacement.replacement_transaction_bound = false;
  const auto replacement_denied = server::ClassifyServerTransactionPressureControl(
      session,
      missing_replacement);
  Require(!replacement_denied.accepted,
          "DPC-031 server force without replacement passed");
  Require(replacement_denied.diagnostic_code ==
              "SERVER.TRANSACTION_PRESSURE.REPLACEMENT_TRANSACTION_REQUIRED",
          "DPC-031 replacement diagnostic mismatch");

  server::ServerSessionRecord inactive_session;
  auto restart_without_transaction =
      ServerInput(server::ServerTransactionPressureAction::kRequestRestart);
  restart_without_transaction.active_transaction = false;
  restart_without_transaction.current_local_transaction_id = 0;
  const auto inactive_denied = server::ClassifyServerTransactionPressureControl(
      inactive_session,
      restart_without_transaction);
  Require(!inactive_denied.accepted &&
              inactive_denied.diagnostic_code ==
                  "SERVER.TRANSACTION_PRESSURE.ACTIVE_TRANSACTION_REQUIRED",
          "DPC-031 server accepted transaction pressure without active transaction");

  const auto rollback = server::ClassifyServerTransactionPressureControl(
      session,
      ServerInput(server::ServerTransactionPressureAction::kForceRollback));
  Require(rollback.accepted && rollback.mutates_transaction &&
              rollback.opens_replacement_boundary,
          "DPC-031 server force rollback boundary mismatch");
  Require(rollback.evidence.find("always_active_transaction_replacement=true") !=
              std::string::npos,
          "DPC-031 server replacement evidence missing");

  const auto commit = server::ClassifyServerTransactionPressureControl(
      session,
      ServerInput(server::ServerTransactionPressureAction::kForceCommit));
  Require(commit.accepted &&
              commit.diagnostic_code ==
                  "SERVER.TRANSACTION_PRESSURE.FORCE_COMMIT_REPLACEMENT",
          "DPC-031 server force commit diagnostic mismatch");

  const auto restart = server::ClassifyServerTransactionPressureControl(
      session,
      ServerInput(server::ServerTransactionPressureAction::kForceRestart));
  Require(restart.accepted &&
              restart.diagnostic_code ==
                  "SERVER.TRANSACTION_PRESSURE.FORCE_RESTART_REPLACEMENT",
          "DPC-031 server force restart diagnostic mismatch");
}

void TestAutocommitReplacementTransactionPolicy() {
  server::ServerSessionRecord session;
  session.local_transaction_id = 11;

  server::ServerDriverTransactionDecisionInput success;
  success.event = server::ServerDriverTransactionEvent::kAutocommitStatementSucceeded;
  const auto committed =
      server::ClassifyDriverTransactionEvent(session, success);
  Require(committed.accepted && committed.opens_replacement_boundary,
          "DPC-031 autocommit success did not open replacement boundary");
  Require(committed.finality_state == "committed_by_engine_inventory",
          "DPC-031 autocommit success did not commit by engine inventory");

  server::ServerDriverTransactionDecisionInput failure;
  failure.event = server::ServerDriverTransactionEvent::kAutocommitStatementFailed;
  const auto rolled_back =
      server::ClassifyDriverTransactionEvent(session, failure);
  Require(rolled_back.accepted && rolled_back.opens_replacement_boundary,
          "DPC-031 autocommit failure did not open replacement boundary");
  Require(rolled_back.finality_state == "rolled_back_by_engine_inventory",
          "DPC-031 autocommit failure did not roll back by engine inventory");
}

}  // namespace

int main() {
  std::cout << kGateSearchKey << '\n';
  TestNoActionAndWarn();
  TestRequestActions();
  TestForcedActionsRequirePolicyAuthorityAndReplacementRule();
  TestDeniedNonAuthoritativeActions();
  TestServerAuthorityBoundary();
  TestAutocommitReplacementTransactionPolicy();
  return EXIT_SUCCESS;
}
