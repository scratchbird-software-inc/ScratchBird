// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "sblr_dispatch_server.hpp"
#include "session_registry.hpp"

#include <array>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>

namespace {

namespace server = scratchbird::server;
namespace sbps = scratchbird::server::sbps;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) Fail(message);
}

std::array<std::uint8_t, 16> Uuid(std::uint8_t seed) {
  std::array<std::uint8_t, 16> uuid{};
  for (std::size_t i = 0; i < uuid.size(); ++i) {
    uuid[i] = static_cast<std::uint8_t>(seed + i);
  }
  uuid[6] = static_cast<std::uint8_t>((uuid[6] & 0x0fu) | 0x70u);
  uuid[8] = static_cast<std::uint8_t>((uuid[8] & 0x3fu) | 0x80u);
  return uuid;
}

server::HostedEngineState MakeEngineState() {
  server::HostedEngineState state;
  state.engine_context_active = true;
  server::HostedDatabaseSnapshot database;
  database.state = server::HostedDatabaseState::kOpen;
  database.database_open = true;
  database.database_path =
      "/tmp/ipar_prepared_handle_authority_gate.sbdb";
  database.database_uuid = "database-ipar-prepared-authority";
  state.databases.push_back(std::move(database));
  return state;
}

server::ServerSessionRecord MakeSession() {
  server::ServerSessionRecord session;
  session.session_uuid = Uuid(0x60);
  session.connection_uuid = session.session_uuid;
  session.auth_context_uuid = Uuid(0x70);
  session.principal_uuid = Uuid(0x80);
  session.effective_user_uuid = session.principal_uuid;
  session.principal_claim = "ipar-prepared-user";
  session.database_path =
      "/tmp/ipar_prepared_handle_authority_gate.sbdb";
  session.database_uuid = "database-ipar-prepared-authority";
  session.catalog_generation = 3;
  session.security_epoch = 5;
  session.descriptor_epoch = 7;
  session.grant_epoch = 11;
  session.policy_generation = 13;
  session.role_set_hash = "roles/ipar-prepared";
  session.group_set_hash = "groups/ipar-prepared";
  session.search_path_hash = "search/ipar-prepared";
  session.local_transaction_id = 17;
  session.snapshot_visible_through_local_transaction_id = 17;
  session.transaction_uuid = server::UuidBytesToText(Uuid(0x90));
  session.default_local_transaction_id = session.local_transaction_id;
  server::ServerTransactionState transaction;
  transaction.local_transaction_id = session.local_transaction_id;
  transaction.snapshot_visible_through_local_transaction_id =
      session.snapshot_visible_through_local_transaction_id;
  transaction.transaction_uuid = session.transaction_uuid;
  transaction.transaction_timestamp = "2026-08-03T00:00:00Z";
  transaction.begin_ordinal = 1;
  session.transactions_by_local_id.emplace(transaction.local_transaction_id,
                                           std::move(transaction));
  session.next_transaction_begin_ordinal = 2;
  return session;
}

sbps::Frame PrepareFrame(const server::ServerSessionRecord& session,
                         const std::string& retired_input) {
  sbps::Frame frame;
  frame.header.message_type =
      static_cast<std::uint16_t>(sbps::MessageType::kPrepareSblr);
  frame.header.request_uuid = sbps::MakeUuidV7Bytes();
  frame.header.connection_uuid = session.connection_uuid;
  frame.header.session_uuid = session.session_uuid;
  frame.header.payload_schema_id = 4001;
  frame.payload = server::EncodePrepareSblrPayloadForTest(
      session.session_uuid, retired_input);
  return frame;
}

bool HasDiagnostic(const server::SessionOperationResult& result,
                   std::string_view code) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.code == code) return true;
  }
  return false;
}

}  // namespace

int main() {
  server::ServerSessionRegistry registry;
  const auto engine_state = MakeEngineState();
  const auto session = MakeSession();
  const std::string session_key = server::UuidBytesToText(session.session_uuid);
  registry.sessions_by_uuid.emplace(session_key, session);
  const auto before = registry.sessions_by_uuid.at(session_key);
  const auto next_handle_before = registry.next_session_object_handle_id;
  const auto next_cache_before = registry.next_authority_cache_generation;

  const auto frame =
      PrepareFrame(before, server::EncodeShowVersionSblrForTest());
  const auto result =
      server::HandlePrepareSblr(&registry, engine_state, frame);
  Require(!result.accepted,
          "retired textual prepare input was accepted");
  Require(HasDiagnostic(result, "SBLR.OPERATION.NONCANONICAL"),
          "retired textual prepare input diagnostic mismatch");

  Require(registry.prepared_by_uuid.empty(),
          "retired prepare input published a prepared metadata receipt");
  Require(registry.prepared_execution_contexts_by_uuid.empty(),
          "retired prepare input published an executable context");
  Require(registry.object_handles_by_key.empty() &&
              registry.next_session_object_handle_id == next_handle_before,
          "retired prepare input published or consumed a session handle");
  Require(registry.authority_cache_by_key.empty() &&
              registry.next_authority_cache_generation == next_cache_before,
          "retired prepare input published or consumed cache authority");
  Require(registry.cursors_by_uuid.empty(),
          "retired prepare input published a result handle");
  Require(registry.statement_contexts_by_statement_uuid.empty(),
          "retired prepare input acquired a statement-use receipt");
  Require(registry.public_abi_sessions_by_session_uuid.empty(),
          "retired prepare input opened an engine result route");

  const auto request = server::FindServerRequestLifecycle(
      registry, server::UuidBytesToText(frame.header.request_uuid));
  Require(request.has_value(), "retired prepare refusal lifecycle is missing");
  Require(request->state == server::ServerRequestLifecycleState::kFailed,
          "retired prepare refusal lifecycle did not fail");
  Require(request->operation_id == "sblr.prepare.pending",
          "retired prepare input reached an admitted operation");
  Require(!request->engine_result_retained,
          "retired prepare input retained an engine result");

  const auto& after = registry.sessions_by_uuid.at(session_key);
  Require(after.local_transaction_id == before.local_transaction_id &&
              after.snapshot_visible_through_local_transaction_id ==
                  before.snapshot_visible_through_local_transaction_id &&
              after.transaction_uuid == before.transaction_uuid &&
              after.default_local_transaction_id ==
                  before.default_local_transaction_id &&
              after.next_transaction_begin_ordinal ==
                  before.next_transaction_begin_ordinal &&
              after.transactions_by_local_id.size() ==
                  before.transactions_by_local_id.size(),
          "retired prepare input mutated transaction authority");
  const auto before_transaction =
      before.transactions_by_local_id.find(before.local_transaction_id);
  const auto after_transaction =
      after.transactions_by_local_id.find(after.local_transaction_id);
  Require(before_transaction != before.transactions_by_local_id.end() &&
              after_transaction != after.transactions_by_local_id.end() &&
              after_transaction->second.transaction_uuid ==
                  before_transaction->second.transaction_uuid &&
              after_transaction->second.lifecycle_state ==
                  before_transaction->second.lifecycle_state &&
              after_transaction->second.deferred_catalog_cache_mutations ==
                  before_transaction->second.deferred_catalog_cache_mutations,
          "retired prepare input changed the active transaction");
  return EXIT_SUCCESS;
}
