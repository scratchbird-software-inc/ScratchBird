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
#include "sblr_dispatch_server.hpp"
#include "session_registry.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
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
using scratchbird::server::ServerCursorRecord;
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

struct ExecuteResultForTest {
  std::string outcome;
  std::array<std::uint8_t, 16> request_uuid{};
  std::array<std::uint8_t, 16> cursor_uuid{};
  std::uint64_t row_count = 0;
  std::string operation_id;
  std::string row_packet;
  std::string detail;
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

bool HasServerDiagnostic(
    const scratchbird::server::SessionOperationResult& result,
    std::string_view code) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.code == code) return true;
  }
  return false;
}

bool HasValue(const std::vector<std::string>& values, std::string_view expected) {
  for (const auto& value : values) {
    if (value == expected) return true;
  }
  return false;
}

std::uint16_t GetU16(const std::vector<std::uint8_t>& data, std::size_t offset) {
  return static_cast<std::uint16_t>(data[offset]) |
         static_cast<std::uint16_t>(static_cast<std::uint16_t>(data[offset + 1]) << 8u);
}

std::uint64_t GetU64(const std::vector<std::uint8_t>& data, std::size_t offset) {
  std::uint64_t value = 0;
  for (int byte = 7; byte >= 0; --byte) {
    value <<= 8u;
    value |= data[offset + static_cast<std::size_t>(byte)];
  }
  return value;
}

bool ReadString(const std::vector<std::uint8_t>& data,
                std::size_t* offset,
                std::string* out) {
  if (offset == nullptr || out == nullptr || *offset + 2 > data.size()) return false;
  std::size_t length = GetU16(data, *offset);
  *offset += 2;
  if (*offset + length > data.size()) return false;
  out->assign(reinterpret_cast<const char*>(data.data() + *offset), length);
  *offset += length;
  return true;
}

std::array<std::uint8_t, 16> GetUuid(const std::vector<std::uint8_t>& data,
                                     std::size_t offset) {
  std::array<std::uint8_t, 16> uuid{};
  std::copy_n(data.begin() + static_cast<std::ptrdiff_t>(offset),
              uuid.size(),
              uuid.begin());
  return uuid;
}

ExecuteResultForTest DecodeExecuteResult(const std::vector<std::uint8_t>& payload) {
  ExecuteResultForTest result;
  std::size_t offset = 0;
  Require(ReadString(payload, &offset, &result.outcome), "SBSFC-070 execute outcome missing");
  Require(offset + 16 <= payload.size(), "SBSFC-070 execute request UUID missing");
  result.request_uuid = GetUuid(payload, offset);
  offset += 16;
  Require(offset + 16 <= payload.size(), "SBSFC-070 execute cursor UUID missing");
  result.cursor_uuid = GetUuid(payload, offset);
  offset += 16;
  Require(offset + 8 <= payload.size(), "SBSFC-070 execute row count missing");
  result.row_count = GetU64(payload, offset);
  offset += 8;
  Require(ReadString(payload, &offset, &result.operation_id), "SBSFC-070 execute operation id missing");
  Require(ReadString(payload, &offset, &result.row_packet), "SBSFC-070 execute row packet missing");
  Require(ReadString(payload, &offset, &result.detail), "SBSFC-070 execute detail missing");
  return result;
}

SessionContext ParserSession() {
  SessionContext session;
  session.authenticated = true;
  session.session_uuid = "019f7000-0000-7000-8000-000000000301";
  session.connection_uuid = "019f7000-0000-7000-8000-000000000302";
  session.database_uuid = "019f7000-0000-7000-8000-000000000303";
  session.dialect_profile_uuid = "sbsql_v3";
  session.catalog_epoch = 70;
  session.security_policy_epoch = 71;
  session.descriptor_epoch = 72;
  return session;
}

ParserConfig ParserConfigForTest() {
  ParserConfig config;
  config.probe_mode = true;
  config.server_endpoint = "sb_server_sbsfc_070_psql_cursor_control";
  config.parser_uuid = "019f7000-0000-7000-8000-000000000304";
  config.bundle_contract_id = "sbp_sbsql@sbsfc-070-psql-cursor-control";
  config.build_id = "sbsql-sbsfc-070-psql-cursor-control";
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
  Require(artifacts.envelope.operation_family == "sblr.cursor.operation.v3",
          std::string(label) + " operation family mismatch");
  Require(!artifacts.envelope.parser_executes_sql,
          std::string(label) + " allowed parser SQL execution");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.server.cursor_session_registry_required"),
          std::string(label) + " missing cursor session registry authority");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.parser.no_sql_text_execution"),
          std::string(label) + " missing no SQL execution authority");
  Require(HasValue(artifacts.envelope.required_authority_steps,
                   "authority.parser.no_storage_or_finality"),
          std::string(label) + " missing no storage/finality authority");
  Require(Contains(artifacts.envelope.payload, "\"session_cursor_control\":true"),
          std::string(label) + " missing cursor control payload");
  Require(Contains(artifacts.envelope.payload, "\"cursor_lookup_scope\":\"session\""),
          std::string(label) + " missing session cursor lookup scope");
  Require(Contains(artifacts.envelope.payload, "\"sql_text_included\":false"),
          std::string(label) + " missing no-SQL-text marker");
  Require(!Contains(artifacts.envelope.payload, "OPEN route_cur FOR SELECT"),
          std::string(label) + " embedded OPEN source SQL text");
  Require(!Contains(artifacts.envelope.payload, "SELECT 7 AS value"),
          std::string(label) + " embedded SELECT source SQL text");
}

void RequireStandaloneProcedureIrRefusal(const PipelineArtifacts& artifacts,
                                         std::string_view surface_id,
                                         std::string_view canonical_name,
                                         std::string_view label) {
  Require(!artifacts.cst.messages.has_errors(), std::string(label) + " CST failed");
  Require(!artifacts.ast.messages.has_errors(), std::string(label) + " AST failed");
  Require(artifacts.bound.bound, std::string(label) + " bind failed");
  Require(!artifacts.verifier.admitted,
          std::string(label) + " standalone procedure IR reached admission as " +
              artifacts.envelope.operation_id + "/" +
              artifacts.envelope.sblr_opcode);
  Require(artifacts.envelope.operation_id == "engine.op.diagnostic_refusal" &&
              artifacts.envelope.engine_api_operation_id == "not_admitted" &&
              artifacts.envelope.sblr_opcode == "SBLR_DIAGNOSTIC_REFUSAL" &&
              artifacts.envelope.result_shape_key == "diagnostic_vector.v1" &&
              artifacts.envelope.resource_contract_key ==
                  "sbsql.command.no_execution.v1" &&
              artifacts.envelope.payload.empty() &&
              artifacts.envelope.operands.empty(),
          std::string(label) + " standalone procedure IR refusal tuple drifted");
  Require(artifacts.envelope.messages.diagnostics.size() == 1,
          std::string(label) + " standalone procedure IR diagnostic count drifted");
  const auto& diagnostic = artifacts.envelope.messages.diagnostics.front();
  const auto field = [&diagnostic](std::string_view name) {
    const auto found = std::find_if(
        diagnostic.fields.begin(), diagnostic.fields.end(),
        [name](const auto& candidate) { return candidate.name == name; });
    return found == diagnostic.fields.end() ? std::string_view{}
                                             : std::string_view(found->value);
  };
  Require(diagnostic.code == "SBSQL.IMPL.NOT_AVAILABLE" &&
              field("surface_id") == surface_id &&
              field("canonical_name") == canonical_name &&
              field("executable_sblr_emitted") == "false",
          std::string(label) + " standalone procedure IR diagnostic drifted");
}

void RequireRegistryEvidence() {
  struct Row {
    std::string_view surface_id;
    std::string_view canonical_name;
  };
  constexpr Row rows[] = {
      {"SBSQL-4A41A00C4F5C", "psql_open_cursor_stmt"},
      {"SBSQL-930016752278", "psql_fetch_stmt"},
      {"SBSQL-C78D9C182EC7", "fetch_direction"},
      {"SBSQL-A4F34F00C071", "psql_close_cursor_stmt"},
      {"SBSQL-D6BD1FBB84A3", "cursor_name"},
      {"SBSQL-6E4796473DD3", "declare_cursor"},
      {"SBSQL-CAD2C514F4BF", "cursor_declaration"},
  };
  for (const auto& row : rows) {
    const auto* registry_row = FindGeneratedSurfaceRegistryRowById(row.surface_id);
    Require(registry_row != nullptr, "SBSFC-070 generated registry row missing");
    Require(registry_row->canonical_name == row.canonical_name,
            "SBSFC-070 generated registry canonical name drifted");
    Require(registry_row->sblr_operation_family == "sblr.cursor.operation.v3",
            "SBSFC-070 generated registry SBLR family drifted");
  }
}

ServerSessionRegistry MakeRegistry(std::array<std::uint8_t, 16>* session_uuid) {
  ServerSessionRegistry registry;
  ServerSessionRecord session;
  session.session_uuid = sbps::MakeUuidV7Bytes();
  session.connection_uuid = session.session_uuid;
  session.auth_context_uuid = sbps::MakeUuidV7Bytes();
  session.principal_uuid = sbps::MakeUuidV7Bytes();
  session.effective_user_uuid = session.principal_uuid;
  session.database_path = "/tmp/sb_sbsfc_070_psql_cursor_control.sbdb";
  session.database_uuid = "019f7000-0000-7000-8000-000000000401";
  session.catalog_generation = 70;
  session.security_epoch = 71;
  session.descriptor_epoch = 72;
  session.grant_epoch = 1;
  session.policy_generation = 1;
  session.local_transaction_id = 1;
  session.snapshot_visible_through_local_transaction_id = 1;
  session.transaction_uuid =
      scratchbird::server::UuidBytesToText(sbps::MakeUuidV7Bytes());
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
  database.database_path = "/tmp/sb_sbsfc_070_psql_cursor_control.sbdb";
  database.database_uuid = "019f7000-0000-7000-8000-000000000401";
  state.databases.push_back(database);
  return state;
}

sbps::Frame ExecuteFrame(const std::array<std::uint8_t, 16>& session_uuid,
                         const std::string& encoded) {
  sbps::Frame frame;
  frame.header.message_type = static_cast<std::uint16_t>(sbps::MessageType::kExecuteSblr);
  frame.header.request_uuid = sbps::MakeUuidV7Bytes();
  frame.header.connection_uuid = session_uuid;
  frame.header.session_uuid = session_uuid;
  frame.header.payload_schema_id = 4003;
  frame.payload = scratchbird::server::EncodeExecuteSblrPayloadForTest(session_uuid, {}, encoded);
  return frame;
}

const ServerCursorRecord* OnlyCursor(const ServerSessionRegistry& registry) {
  Require(registry.cursors_by_uuid.size() == 1, "SBSFC-070 expected one cursor record");
  return &registry.cursors_by_uuid.begin()->second;
}

void RequireRetiredServerCarrierRefusal() {
  std::array<std::uint8_t, 16> session_uuid{};
  auto registry = MakeRegistry(&session_uuid);
  const auto engine_state = MakeEngineState();

  const auto declare =
      RunPipeline("DECLARE decl_cur CURSOR FOR SELECT 8 AS value;");
  const auto result = scratchbird::server::HandleExecuteSblr(
      &registry, engine_state,
      ExecuteFrame(session_uuid, declare.envelope.payload));
  Require(!result.accepted,
          "SBSFC-070 retired textual DECLARE CURSOR carrier was accepted");
  Require(HasServerDiagnostic(result, "SBLR.OPERATION.NONCANONICAL"),
          "SBSFC-070 retired textual cursor refusal diagnostic drifted");
  Require(registry.cursors_by_uuid.empty(),
          "SBSFC-070 retired cursor carriers mutated server state");
}

}  // namespace

int main() {
  RequireRegistryEvidence();

  const auto open = RunPipeline("OPEN route_cur FOR SELECT 7 AS value;");
  RequireStandaloneProcedureIrRefusal(
      open, "SBSQL-4A41A00C4F5C", "psql_open_cursor_stmt", "OPEN");

  const auto fetch = RunPipeline("FETCH NEXT FROM route_cur;");
  RequireStandaloneProcedureIrRefusal(
      fetch, "SBSQL-930016752278", "psql_fetch_stmt", "FETCH");

  const auto close = RunPipeline("CLOSE route_cur;");
  RequireStandaloneProcedureIrRefusal(
      close, "SBSQL-A4F34F00C071", "psql_close_cursor_stmt", "CLOSE");

  const auto declare = RunPipeline("DECLARE decl_cur CURSOR FOR SELECT 8 AS value;");
  RequireCleanPipeline(declare, "DECLARE CURSOR");
  Require(declare.envelope.operation_id == "session.cursor_open",
          "SBSFC-070 DECLARE CURSOR operation id mismatch");
  Require(Contains(declare.envelope.payload, "SBSQL-6E4796473DD3"),
          "SBSFC-070 DECLARE payload missing declare cursor row id");
  Require(Contains(declare.envelope.payload, "SBSQL-CAD2C514F4BF"),
          "SBSFC-070 DECLARE payload missing cursor declaration row id");
  Require(Contains(declare.envelope.payload, "\"cursor_declaration\":true"),
          "SBSFC-070 DECLARE payload missing declaration marker");

  RequireRetiredServerCarrierRefusal();
  return EXIT_SUCCESS;
}
