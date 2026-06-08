// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "ast/ast.hpp"
#include "binder/binder.hpp"
#include "cst/cst.hpp"
#include "lowering/lowering.hpp"
#include "registry/generated/sbsql_generated_registry.hpp"
#include "sblr_admission.hpp"
#include "sblr_dispatch_server.hpp"
#include "session_registry.hpp"

#include <array>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace scratchbird::parser::sbsql;
using scratchbird::server::HostedDatabaseSnapshot;
using scratchbird::server::HostedDatabaseState;
using scratchbird::server::HostedEngineState;
using scratchbird::server::ServerSessionRecord;
using scratchbird::server::ServerSessionRegistry;
namespace sbps = scratchbird::server::sbps;

struct PipelineArtifacts {
  CstDocument cst;
  AstDocument ast;
  BoundStatement bound;
  SblrEnvelope envelope;
  SblrVerifierResult verifier;
};

void Require(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(EXIT_FAILURE);
  }
}

bool Contains(std::string_view haystack, std::string_view needle) {
  return haystack.find(needle) != std::string_view::npos;
}

bool HasValue(const std::vector<std::string>& values, std::string_view expected) {
  for (const auto& value : values) {
    if (value == expected) return true;
  }
  return false;
}

SessionContext ParserSession() {
  SessionContext session;
  session.authenticated = true;
  session.session_uuid = "019f6800-0000-7000-8000-000000000301";
  session.connection_uuid = "019f6800-0000-7000-8000-000000000302";
  session.database_uuid = "019f6800-0000-7000-8000-000000000303";
  session.dialect_profile_uuid = "sbsql_v3";
  session.catalog_epoch = 67;
  session.security_policy_epoch = 68;
  session.descriptor_epoch = 69;
  return session;
}

ParserConfig ParserConfigForTest() {
  ParserConfig config;
  config.probe_mode = true;
  config.server_endpoint = "sb_server_sbsfc_068_prepared_statement_control";
  config.parser_uuid = "019f6800-0000-7000-8000-000000000304";
  config.bundle_contract_id = "sbp_sbsql@sbsfc-068-prepared-statement-control";
  config.build_id = "sbsql-sbsfc-068-prepared-statement-control";
  return config;
}

PipelineArtifacts RunPipeline(std::string_view sql) {
  PipelineArtifacts artifacts;
  const auto session = ParserSession();
  artifacts.cst = BuildCst(std::string(sql));
  artifacts.ast = BuildAst(artifacts.cst);
  artifacts.bound = BindAst(artifacts.ast, artifacts.cst, ParserConfigForTest(), session, {});
  artifacts.envelope = LowerToSblr(artifacts.bound, artifacts.cst, session);
  artifacts.verifier = VerifySblrEnvelope(artifacts.envelope);
  return artifacts;
}

void PrintMessages(const MessageVectorSet& messages) {
  for (const auto& diagnostic : messages.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
}

void RequireCleanPipeline(const PipelineArtifacts& artifacts, std::string_view label) {
  PrintMessages(artifacts.cst.messages);
  PrintMessages(artifacts.ast.messages);
  PrintMessages(artifacts.bound.messages);
  PrintMessages(artifacts.envelope.messages);
  PrintMessages(artifacts.verifier.messages);
  Require(!artifacts.cst.messages.has_errors(), std::string(label) + " CST failed");
  Require(!artifacts.ast.messages.has_errors(), std::string(label) + " AST failed");
  Require(artifacts.bound.bound, std::string(label) + " bind failed");
  Require(artifacts.verifier.admitted, std::string(label) + " SBLR verifier rejected");
  Require(artifacts.envelope.operation_family == "sblr.general.operation.v3",
          std::string(label) + " operation family mismatch");
  Require(!artifacts.envelope.parser_executes_sql,
          std::string(label) + " allowed parser SQL execution");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.server.prepared_statement_session_registry_required"),
          std::string(label) + " missing prepared session registry authority");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.parser.no_sql_text_execution"),
          std::string(label) + " missing no SQL execution authority");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.parser.no_storage_or_finality"),
          std::string(label) + " missing no storage/finality authority");
  Require(Contains(artifacts.envelope.payload, "\"prepared_statement_control\":true"),
          std::string(label) + " missing prepared control payload");
  Require(!Contains(artifacts.envelope.payload, "PREPARE prep_one AS SELECT 7"),
          std::string(label) + " embedded source SQL text");
}

void RequireRegistryEvidence() {
  struct Row {
    std::string_view surface_id;
    std::string_view canonical_name;
  };
  constexpr Row rows[] = {
      {"SBSQL-5535E9A48BE4", "prepare_stmt"},
      {"SBSQL-414E9A624B34", "execute_prepared_stmt"},
      {"SBSQL-6677B188A72E", "execute_stmt"},
      {"SBSQL-FB03794952FB", "deallocate_stmt"},
  };
  for (const auto& row : rows) {
    const auto* registry_row = FindGeneratedSurfaceRegistryRowById(row.surface_id);
    Require(registry_row != nullptr, "SBSFC-068 generated registry row missing");
    Require(registry_row->canonical_name == row.canonical_name,
            "SBSFC-068 generated registry canonical name drifted");
    Require(registry_row->sblr_operation_family == "sblr.general.operation.v3",
            "SBSFC-068 generated registry SBLR family drifted");
  }
}

ServerSessionRegistry MakeRegistry(std::array<std::uint8_t, 16>* session_uuid) {
  ServerSessionRegistry registry;
  ServerSessionRecord session;
  session.session_uuid = sbps::MakeUuidV7Bytes();
  session.auth_context_uuid = sbps::MakeUuidV7Bytes();
  session.principal_uuid = sbps::MakeUuidV7Bytes();
  session.effective_user_uuid = session.principal_uuid;
  session.database_path = "/tmp/sb_sbsfc_068_prepared_statement_control.sbdb";
  session.database_uuid = "019f6800-0000-7000-8000-000000000401";
  session.catalog_generation = 67;
  session.security_epoch = 68;
  session.descriptor_epoch = 69;
  session.grant_epoch = 1;
  session.policy_generation = 1;
  session.local_transaction_id = 1;
  session.snapshot_visible_through_local_transaction_id = 1;
  *session_uuid = session.session_uuid;
  registry.sessions_by_uuid[scratchbird::server::UuidBytesToText(session.session_uuid)] = session;
  return registry;
}

HostedEngineState MakeEngineState() {
  HostedEngineState state;
  state.engine_context_active = true;
  HostedDatabaseSnapshot database;
  database.state = HostedDatabaseState::kOpen;
  database.database_open = true;
  database.database_path = "/tmp/sb_sbsfc_068_prepared_statement_control.sbdb";
  database.database_uuid = "019f6800-0000-7000-8000-000000000401";
  state.databases.push_back(database);
  return state;
}

sbps::Frame ExecuteFrame(const std::array<std::uint8_t, 16>& session_uuid,
                         const std::string& encoded) {
  sbps::Frame frame;
  frame.header.message_type = static_cast<std::uint16_t>(sbps::MessageType::kExecuteSblr);
  frame.header.request_uuid = sbps::MakeUuidV7Bytes();
  frame.header.session_uuid = session_uuid;
  frame.payload = scratchbird::server::EncodeExecuteSblrPayloadForTest(session_uuid, {}, encoded);
  return frame;
}

sbps::Frame CloseCursorFrame(const std::array<std::uint8_t, 16>& session_uuid,
                             const std::array<std::uint8_t, 16>& cursor_uuid) {
  sbps::Frame frame;
  frame.header.message_type = static_cast<std::uint16_t>(sbps::MessageType::kCloseCursor);
  frame.header.request_uuid = sbps::MakeUuidV7Bytes();
  frame.header.session_uuid = session_uuid;
  frame.payload = scratchbird::server::EncodeCloseCursorPayloadForTest(session_uuid, cursor_uuid);
  return frame;
}

void RequireServerRoute() {
  std::array<std::uint8_t, 16> session_uuid{};
  auto registry = MakeRegistry(&session_uuid);
  const auto engine_state = MakeEngineState();

  const auto prepare = RunPipeline("PREPARE prep_one AS SELECT 7 AS value;");
  const auto execute = RunPipeline("EXECUTE prep_one;");
  const auto cursor_execute = RunPipeline("EXECUTE prep_one WITH CURSOR;");
  const auto deallocate = RunPipeline("DEALLOCATE PREPARE prep_one;");

  const auto prepare_result = scratchbird::server::HandleExecuteSblr(
      &registry, engine_state, ExecuteFrame(session_uuid, prepare.envelope.payload));
  Require(prepare_result.accepted, "SBSFC-068 server prepare control was not accepted");
  Require(registry.prepared_by_uuid.size() == 1,
          "SBSFC-068 server did not create one prepared statement record");
  const auto prepared_it = registry.prepared_by_uuid.begin();
  Require(prepared_it->second.statement_name == "prep_one",
          "SBSFC-068 prepared statement name was not session-bound");
  Require(prepared_it->second.operation_id == "query.evaluate_projection",
          "SBSFC-068 prepared statement did not store the inner operation id");
  Require(Contains(prepared_it->second.encoded_sblr_envelope, "\"operation_id\":\"query.evaluate_projection\""),
          "SBSFC-068 prepared statement did not store executable canonical SBLR");
  Require(!Contains(prepared_it->second.encoded_sblr_envelope, "PREPARE prep_one AS SELECT"),
          "SBSFC-068 prepared statement stored raw SQL text");

  const auto execute_result = scratchbird::server::HandleExecuteSblr(
      &registry, engine_state, ExecuteFrame(session_uuid, execute.envelope.payload));
  Require(execute_result.accepted, "SBSFC-068 server execute prepared was not accepted");
  const std::string execute_payload(execute_result.payload.begin(), execute_result.payload.end());
  Require(Contains(execute_payload, "query.evaluate_projection"),
          "SBSFC-068 execute prepared did not dispatch the stored inner SBLR operation");

  const auto cursor_execute_result = scratchbird::server::HandleExecuteSblr(
      &registry, engine_state, ExecuteFrame(session_uuid, cursor_execute.envelope.payload));
  Require(cursor_execute_result.accepted,
          "SBSFC-068 server execute prepared WITH CURSOR was not accepted");
  const auto cursor_uuid =
      scratchbird::server::DecodeCursorUuidForTest(cursor_execute_result.payload);
  Require(cursor_uuid.has_value(),
          "SBSFC-068 execute prepared WITH CURSOR did not return a cursor UUID");
  const auto close_cursor_result = scratchbird::server::HandleCloseCursor(
      &registry, CloseCursorFrame(session_uuid, *cursor_uuid));
  Require(close_cursor_result.accepted,
          "SBSFC-068 close cursor after prepared execute was not accepted");

  const auto deallocate_result = scratchbird::server::HandleExecuteSblr(
      &registry, engine_state, ExecuteFrame(session_uuid, deallocate.envelope.payload));
  Require(deallocate_result.accepted, "SBSFC-068 server deallocate was not accepted");
  Require(prepared_it->second.closed, "SBSFC-068 deallocate did not close prepared state");

  const auto execute_after_deallocate = scratchbird::server::HandleExecuteSblr(
      &registry, engine_state, ExecuteFrame(session_uuid, execute.envelope.payload));
  Require(!execute_after_deallocate.accepted,
          "SBSFC-068 execute after deallocate unexpectedly succeeded");
}

}  // namespace

int main() {
  RequireRegistryEvidence();

  const auto prepare = RunPipeline("PREPARE prep_one AS SELECT 7 AS value;");
  RequireCleanPipeline(prepare, "PREPARE");
  Require(prepare.envelope.operation_id == "session.prepare_statement",
          "SBSFC-068 PREPARE operation id mismatch");
  Require(prepare.envelope.sblr_opcode == "SBLR_SESSION_PREPARE_STATEMENT",
          "SBSFC-068 PREPARE opcode mismatch");
  Require(Contains(prepare.envelope.payload, "SBSQL-5535E9A48BE4"),
          "SBSFC-068 PREPARE payload missing row id");
  Require(Contains(prepare.envelope.payload, "\"prepared_inner_operation_id\":\"query.evaluate_projection\""),
          "SBSFC-068 PREPARE payload missing inner operation descriptor");

  const auto execute = RunPipeline("EXECUTE prep_one;");
  RequireCleanPipeline(execute, "EXECUTE");
  Require(execute.envelope.operation_id == "session.execute_prepared_statement",
          "SBSFC-068 EXECUTE operation id mismatch");
  Require(execute.envelope.sblr_opcode == "SBLR_SESSION_EXECUTE_PREPARED_STATEMENT",
          "SBSFC-068 EXECUTE opcode mismatch");
  Require(Contains(execute.envelope.payload, "SBSQL-414E9A624B34"),
          "SBSFC-068 EXECUTE payload missing execute_prepared_stmt row id");
  Require(Contains(execute.envelope.payload, "SBSQL-6677B188A72E"),
          "SBSFC-068 EXECUTE payload missing execute_stmt row id");

  const auto deallocate = RunPipeline("DEALLOCATE PREPARE prep_one;");
  RequireCleanPipeline(deallocate, "DEALLOCATE");
  Require(deallocate.envelope.operation_id == "session.deallocate_prepared_statement",
          "SBSFC-068 DEALLOCATE operation id mismatch");
  Require(deallocate.envelope.sblr_opcode == "SBLR_SESSION_DEALLOCATE_PREPARED_STATEMENT",
          "SBSFC-068 DEALLOCATE opcode mismatch");
  Require(Contains(deallocate.envelope.payload, "SBSQL-FB03794952FB"),
          "SBSFC-068 DEALLOCATE payload missing row id");

  const auto cursor_execute = RunPipeline("EXECUTE prep_one WITH CURSOR;");
  RequireCleanPipeline(cursor_execute, "EXECUTE WITH CURSOR");
  Require(Contains(cursor_execute.envelope.payload, "\"prepared_cursor_requested\":true"),
          "SBSFC-068 EXECUTE WITH CURSOR payload missing cursor option");
  Require(Contains(cursor_execute.envelope.payload, "SBSQL-3F4B1406188A"),
          "SBSFC-068 EXECUTE WITH CURSOR payload missing option row id");

  RequireServerRoute();
  return EXIT_SUCCESS;
}
