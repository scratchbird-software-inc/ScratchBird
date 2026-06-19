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
#include <vector>

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
  database.database_path = "/tmp/ipar_prepared_handle_authority_gate.sbdb";
  database.database_uuid = "database-ipar-prepared-authority";
  database.read_only = false;
  database.write_admission_fenced = false;
  state.databases.push_back(std::move(database));
  return state;
}

server::ServerSessionRecord MakeSession(std::uint8_t seed,
                                        std::string database_uuid =
                                            "database-ipar-prepared-authority") {
  server::ServerSessionRecord session;
  session.connection_uuid = Uuid(seed);
  session.session_uuid = Uuid(static_cast<std::uint8_t>(seed + 16));
  session.auth_context_uuid = Uuid(static_cast<std::uint8_t>(seed + 32));
  session.principal_uuid = Uuid(static_cast<std::uint8_t>(seed + 48));
  session.effective_user_uuid = session.principal_uuid;
  session.principal_claim = "ipar-prepared-user";
  session.database_path = "/tmp/ipar_prepared_handle_authority_gate.sbdb";
  session.database_uuid = std::move(database_uuid);
  session.catalog_generation = 1;
  session.security_epoch = 1;
  session.descriptor_epoch = 1;
  session.grant_epoch = 1;
  session.policy_generation = 1;
  session.role_set_hash = "roles/ipar-prepared";
  session.group_set_hash = "groups/ipar-prepared";
  session.search_path_hash = "search/ipar-prepared";
  return session;
}

void AddSession(server::ServerSessionRegistry* registry,
                const server::ServerSessionRecord& session) {
  registry->sessions_by_uuid[server::UuidBytesToText(session.session_uuid)] = session;
}

sbps::Frame Frame(sbps::MessageType type,
                  std::vector<std::uint8_t> payload,
                  const std::array<std::uint8_t, 16>& session_uuid) {
  sbps::Frame frame;
  frame.header.message_type = static_cast<std::uint16_t>(type);
  frame.header.request_uuid = sbps::MakeUuidV7Bytes();
  frame.header.session_uuid = session_uuid;
  frame.payload = std::move(payload);
  return frame;
}

sbps::Frame PrepareFrame(const std::array<std::uint8_t, 16>& session_uuid,
                         const std::string& encoded) {
  return Frame(sbps::MessageType::kPrepareSblr,
               server::EncodePrepareSblrPayloadForTest(session_uuid, encoded),
               session_uuid);
}

std::string NativeBulkIngestTemplateEnvelope() {
  return "operation_id=dml.execute_native_bulk_ingest\n"
         "opcode=SBLR_DML_EXECUTE_NATIVE_BULK_INGEST\n"
         "sblr_operation_family=sblr.dml.operation.v3\n"
         "result_shape=engine.api.result.v1\n"
         "diagnostic_shape=engine.diagnostic.v1\n"
         "contains_sql_text=false\n"
         "parser_resolved_names_to_uuids=true\n"
         "requires_security_context=true\n"
         "requires_transaction_context=true\n"
         "requires_cluster_authority=false\n"
         "target_object_uuid=018f0000-0000-7000-8000-000000000101\n"
         "target_object_kind=table\n"
         "dml_surface_variant=sbwp_prepared_rowset_template\n"
         "source_kind=binary_typed_rows\n"
         "format_family=binary_typed_rows\n"
         "estimated_row_count=0\n"
         "native_bulk_ingest=true\n"
         "native_bulk_ingest_enabled=true\n"
         "reject_mode=fail_fast\n"
         "reject_limit_rows=0\n"
         "reject_payload_policy=diagnostic_only\n"
         "result_payload_policy=summary_only\n"
         "resume_policy=fail_closed\n"
         "checkpoint_mode=disabled\n"
         "duplicate_mode=error\n"
         "require_generated_row_uuid=true\n";
}

sbps::Frame ExecutePreparedFrame(
    const std::array<std::uint8_t, 16>& session_uuid,
    const std::array<std::uint8_t, 16>& prepared_statement_uuid) {
  return Frame(sbps::MessageType::kExecuteSblr,
               server::EncodeExecuteSblrPayloadForTest(
                   session_uuid, prepared_statement_uuid, ""),
               session_uuid);
}

sbps::Frame ExecutePreparedFrameWithEnvelope(
    const std::array<std::uint8_t, 16>& session_uuid,
    const std::array<std::uint8_t, 16>& prepared_statement_uuid,
    const std::string& encoded) {
  return Frame(sbps::MessageType::kExecuteSblr,
               server::EncodeExecuteSblrPayloadForTest(
                   session_uuid, prepared_statement_uuid, encoded),
               session_uuid);
}

bool HasDiagnostic(const server::SessionOperationResult& result,
                   std::string_view code) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.code == code) return true;
  }
  return false;
}

server::ServerRequestRecord RequireRequest(
    const server::ServerSessionRegistry& registry,
    const std::array<std::uint8_t, 16>& request_uuid) {
  const auto record =
      server::FindServerRequestLifecycle(registry, server::UuidBytesToText(request_uuid));
  Require(record.has_value(), "IPAR prepared handle request lifecycle missing");
  return *record;
}

void RequirePreparedRefusalBeforeDispatch(
    server::ServerSessionRegistry* registry,
    const server::HostedEngineState& engine_state,
    const sbps::Frame& frame,
    std::string_view diagnostic_code,
    std::string_view detail) {
  const auto cursor_count = registry->cursors_by_uuid.size();
  const auto result = server::HandleExecuteSblr(registry, engine_state, frame);
  Require(!result.accepted, "IPAR prepared stale handle was accepted");
  Require(HasDiagnostic(result, diagnostic_code),
          "IPAR prepared stale handle diagnostic mismatch");
  const auto request = RequireRequest(*registry, frame.header.request_uuid);
  Require(request.state == server::ServerRequestLifecycleState::kFailed,
          "IPAR prepared stale handle request was not failed");
  Require(request.detail == detail, "IPAR prepared stale handle detail mismatch");
  Require(request.operation_id == "sblr.dispatch.pending",
          "IPAR prepared stale handle reached dispatch/admission operation update");
  Require(registry->cursors_by_uuid.size() == cursor_count,
          "IPAR prepared stale handle opened a cursor");
}

void ValidateServerPreparedHandlesBindAuthority() {
  server::ServerSessionRegistry registry;
  const auto engine_state = MakeEngineState();
  const auto primary = MakeSession(0x10);
  const auto other_session = MakeSession(0x80);
  AddSession(&registry, primary);
  AddSession(&registry, other_session);

  const auto prepare = server::HandlePrepareSblr(
      &registry,
      engine_state,
      PrepareFrame(primary.session_uuid, server::EncodeShowVersionSblrForTest()));
  Require(prepare.accepted, "IPAR prepared handle baseline prepare failed");
  const auto prepared_uuid = server::DecodePreparedStatementUuidForTest(prepare.payload);
  Require(prepared_uuid.has_value(), "IPAR prepared handle UUID decode failed");

  const auto prepared_it =
      registry.prepared_by_uuid.find(server::UuidBytesToText(*prepared_uuid));
  Require(prepared_it != registry.prepared_by_uuid.end(),
          "IPAR prepared handle record missing");
  Require(prepared_it->second.session_uuid == primary.session_uuid,
          "IPAR prepared handle did not bind session UUID");
  Require(prepared_it->second.principal_uuid == primary.principal_uuid &&
              prepared_it->second.effective_user_uuid == primary.effective_user_uuid,
          "IPAR prepared handle did not bind user UUIDs");
  Require(prepared_it->second.database_uuid == primary.database_uuid,
          "IPAR prepared handle did not bind database UUID");
  Require(prepared_it->second.catalog_generation == primary.catalog_generation &&
              prepared_it->second.security_epoch == primary.security_epoch &&
              prepared_it->second.policy_generation == primary.policy_generation,
          "IPAR prepared handle did not bind authority epochs");

  const auto accepted_frame = ExecutePreparedFrame(primary.session_uuid, *prepared_uuid);
  const auto accepted = server::HandleExecuteSblr(&registry, engine_state, accepted_frame);
  Require(accepted.accepted, "IPAR prepared handle same-session execute failed");
  const auto accepted_request = RequireRequest(registry, accepted_frame.header.request_uuid);
  Require(accepted_request.state == server::ServerRequestLifecycleState::kCompleted,
          "IPAR prepared handle accepted request did not complete");

  RequirePreparedRefusalBeforeDispatch(
      &registry,
      engine_state,
      ExecutePreparedFrameWithEnvelope(primary.session_uuid,
                                       *prepared_uuid,
                                       server::EncodeBeginTransactionSblrForTest()),
      "PARSER_SERVER_IPC.PREPARED_STATEMENT_SHAPE_MISMATCH",
      "prepared_statement_shape_mismatch");

  RequirePreparedRefusalBeforeDispatch(
      &registry,
      engine_state,
      ExecutePreparedFrame(other_session.session_uuid, *prepared_uuid),
      "PARSER_SERVER_IPC.PREPARED_STATEMENT_NOT_FOUND",
      "prepared_statement_cross_session");

  auto& session = registry.sessions_by_uuid[server::UuidBytesToText(primary.session_uuid)];
  const auto original = session;

  session.principal_uuid = Uuid(0xd0);
  session.effective_user_uuid = session.principal_uuid;
  RequirePreparedRefusalBeforeDispatch(
      &registry,
      engine_state,
      ExecutePreparedFrame(primary.session_uuid, *prepared_uuid),
      "PARSER_SERVER_IPC.PREPARED_STATEMENT_STALE",
      "prepared_statement_cross_user");
  session = original;

  session.database_uuid = "database-ipar-prepared-authority-other";
  RequirePreparedRefusalBeforeDispatch(
      &registry,
      engine_state,
      ExecutePreparedFrame(primary.session_uuid, *prepared_uuid),
      "PARSER_SERVER_IPC.PREPARED_STATEMENT_STALE",
      "prepared_statement_cross_database");
  session = original;

  ++session.catalog_generation;
  RequirePreparedRefusalBeforeDispatch(
      &registry,
      engine_state,
      ExecutePreparedFrame(primary.session_uuid, *prepared_uuid),
      "PARSER_SERVER_IPC.PREPARED_STATEMENT_STALE",
      "prepared_statement_epoch_stale");
  session = original;

  ++session.security_epoch;
  RequirePreparedRefusalBeforeDispatch(
      &registry,
      engine_state,
      ExecutePreparedFrame(primary.session_uuid, *prepared_uuid),
      "PARSER_SERVER_IPC.PREPARED_STATEMENT_STALE",
      "prepared_statement_epoch_stale");
  session = original;

  ++session.policy_generation;
  RequirePreparedRefusalBeforeDispatch(
      &registry,
      engine_state,
      ExecutePreparedFrame(primary.session_uuid, *prepared_uuid),
      "PARSER_SERVER_IPC.PREPARED_STATEMENT_STALE",
      "prepared_statement_epoch_stale");
  session = original;

  RequirePreparedRefusalBeforeDispatch(
      &registry,
      engine_state,
      ExecutePreparedFrame(primary.session_uuid, Uuid(0xf0)),
      "PARSER_SERVER_IPC.PREPARED_STATEMENT_NOT_FOUND",
      "prepared_statement_not_found");
}

void ValidateNativeBulkIngestTemplateCanBePrepared() {
  server::ServerSessionRegistry registry;
  const auto engine_state = MakeEngineState();
  const auto primary = MakeSession(0x22);
  AddSession(&registry, primary);

  const auto prepare = server::HandlePrepareSblr(
      &registry,
      engine_state,
      PrepareFrame(primary.session_uuid, NativeBulkIngestTemplateEnvelope()));
  Require(prepare.accepted, "native bulk ingest template prepare failed");
  const auto prepared_uuid = server::DecodePreparedStatementUuidForTest(prepare.payload);
  Require(prepared_uuid.has_value(), "native bulk ingest template UUID decode failed");
  const auto prepared_it =
      registry.prepared_by_uuid.find(server::UuidBytesToText(*prepared_uuid));
  Require(prepared_it != registry.prepared_by_uuid.end(),
          "native bulk ingest template prepared record missing");
  Require(prepared_it->second.operation_id == "dml.execute_native_bulk_ingest",
          "native bulk ingest template operation id mismatch");
}

}  // namespace

int main() {
  ValidateServerPreparedHandlesBindAuthority();
  ValidateNativeBulkIngestTemplateCanBePrepared();
  return EXIT_SUCCESS;
}
