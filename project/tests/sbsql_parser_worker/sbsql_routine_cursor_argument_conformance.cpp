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
#include "sblr_admission.hpp"
#include "sblr_dispatch_server.hpp"
#include "session_registry.hpp"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace scratchbird::parser::sbsql;
using scratchbird::server::HostedDatabaseSnapshot;
using scratchbird::server::HostedDatabaseState;
using scratchbird::server::HostedEngineState;
using scratchbird::server::ServerDiagnostic;
using scratchbird::server::ServerSessionRecord;
using scratchbird::server::ServerSessionRegistry;
using scratchbird::server::SessionOperationResult;
namespace sbps = scratchbird::server::sbps;

void Require(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(EXIT_FAILURE);
  }
}

bool Contains(std::string_view haystack, std::string_view needle) {
  return haystack.find(needle) != std::string_view::npos;
}

bool HasDiagnostic(const SessionOperationResult& result, std::string_view code) {
  for (const ServerDiagnostic& diagnostic : result.diagnostics) {
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
         static_cast<std::uint16_t>(
             static_cast<std::uint16_t>(data[offset + 1]) << 8u);
}

std::uint64_t GetU64(const std::vector<std::uint8_t>& data, std::size_t offset) {
  std::uint64_t value = 0;
  for (int byte = 7; byte >= 0; --byte) {
    value <<= 8u;
    value |= data[offset + static_cast<std::size_t>(byte)];
  }
  return value;
}

std::array<std::uint8_t, 16> GetUuid(const std::vector<std::uint8_t>& data,
                                     std::size_t offset) {
  std::array<std::uint8_t, 16> uuid{};
  for (std::size_t index = 0; index < uuid.size(); ++index) {
    uuid[index] = data[offset + index];
  }
  return uuid;
}

bool ReadString(const std::vector<std::uint8_t>& data,
                std::size_t* offset,
                std::string* out) {
  if (*offset + 2 > data.size()) return false;
  std::uint64_t length = GetU16(data, *offset);
  *offset += 2;
  if (length == 0xffffu) {
    if (*offset + 8 > data.size()) return false;
    length = GetU64(data, *offset);
    *offset += 8;
  }
  if (*offset + length > data.size()) return false;
  out->assign(reinterpret_cast<const char*>(data.data() + *offset),
              static_cast<std::size_t>(length));
  *offset += static_cast<std::size_t>(length);
  return true;
}

struct ExecuteResultForTest {
  std::array<std::uint8_t, 16> request_uuid{};
  std::array<std::uint8_t, 16> cursor_uuid{};
  std::uint64_t row_count = 0;
  std::string operation_id;
  std::string row_packet;
  std::string detail;
};

std::optional<ExecuteResultForTest> DecodeExecuteResultForTest(
    const std::vector<std::uint8_t>& payload) {
  std::size_t offset = 0;
  ExecuteResultForTest result;
  std::string outcome;
  if (!ReadString(payload, &offset, &outcome) || outcome != "accepted") {
    return std::nullopt;
  }
  if (offset + 16 + 16 + 8 > payload.size()) return std::nullopt;
  result.request_uuid = GetUuid(payload, offset);
  offset += 16;
  result.cursor_uuid = GetUuid(payload, offset);
  offset += 16;
  result.row_count = GetU64(payload, offset);
  offset += 8;
  if (!ReadString(payload, &offset, &result.operation_id)) return std::nullopt;
  if (!ReadString(payload, &offset, &result.row_packet)) return std::nullopt;
  if (!ReadString(payload, &offset, &result.detail)) return std::nullopt;
  return result;
}

void PrintMessages(const MessageVectorSet& messages) {
  for (const auto& diagnostic : messages.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message << '\n';
  }
}

ParserConfig ParserConfigForTest() {
  ParserConfig config;
  config.probe_mode = true;
  config.server_endpoint = "sb_server_name_resolver";
  config.parser_uuid = "019f0000-0000-7000-8000-000000450001";
  config.bundle_contract_id = "sbp_sbsql@routine-cursor-proof";
  config.build_id = "routine-cursor-argument-proof";
  return config;
}

SessionContext ParserSession() {
  SessionContext session;
  session.authenticated = true;
  session.session_uuid = "019f0000-0000-7000-8000-000000450003";
  session.connection_uuid = "019f0000-0000-7000-8000-000000450004";
  session.database_uuid = "019f0000-0000-7000-8000-000000450005";
  session.dialect_profile_uuid = "sbsql_v3";
  session.catalog_epoch = 450;
  session.security_policy_epoch = 451;
  session.descriptor_epoch = 452;
  return session;
}

struct PipelineArtifacts {
  CstDocument cst;
  AstDocument ast;
  BoundStatement bound;
  SblrEnvelope envelope;
  SblrVerifierResult verifier;
};

PipelineArtifacts RunPipeline(std::string_view sql) {
  PipelineArtifacts artifacts;
  const auto session = ParserSession();
  artifacts.cst = BuildCst(sql);
  artifacts.ast = BuildAst(artifacts.cst);
  artifacts.bound = BindAst(artifacts.ast, artifacts.cst, ParserConfigForTest(), session, {});
  artifacts.envelope = LowerToSblr(artifacts.bound, artifacts.cst, session);
  artifacts.verifier = VerifySblrEnvelope(artifacts.envelope);
  return artifacts;
}

void RequireCursorRoutineSignature(std::string_view sql,
                                   std::string_view operation_id,
                                   std::string_view opcode) {
  const auto artifacts = RunPipeline(sql);
  PrintMessages(artifacts.cst.messages);
  PrintMessages(artifacts.ast.messages);
  PrintMessages(artifacts.bound.messages);
  PrintMessages(artifacts.envelope.messages);
  PrintMessages(artifacts.verifier.messages);
  Require(!artifacts.cst.messages.has_errors(), "ROUTINE-CURSOR-GATE-001 CST rejected cursor signature");
  Require(!artifacts.ast.messages.has_errors(), "ROUTINE-CURSOR-GATE-001 AST rejected cursor signature");
  Require(artifacts.bound.bound, "ROUTINE-CURSOR-GATE-001 bind rejected cursor signature");
  Require(artifacts.verifier.admitted,
          "ROUTINE-CURSOR-GATE-001 verifier rejected cursor signature");
  Require(artifacts.envelope.operation_id == operation_id,
          "ROUTINE-CURSOR-GATE-001 operation id drifted");
  Require(artifacts.envelope.sblr_opcode == opcode,
          "ROUTINE-CURSOR-GATE-001 opcode drifted");
  Require(HasValue(artifacts.envelope.descriptor_refs, "sys.server.cursor_descriptor"),
          "ROUTINE-CURSOR-GATE-001 cursor descriptor ref missing");
  Require(HasValue(artifacts.envelope.descriptor_refs,
                   "sys.routine.cursor_parameter_descriptor"),
          "ROUTINE-CURSOR-GATE-001 cursor parameter descriptor ref missing");
  Require(!artifacts.envelope.parser_executes_sql,
          "ROUTINE-CURSOR-GATE-001 parser claimed SQL execution authority");
  Require(!artifacts.envelope.real_file_effects,
          "ROUTINE-CURSOR-GATE-001 parser claimed file/storage side effects");
  Require(Contains(artifacts.envelope.payload,
                   "\"routine_parameter_0_descriptor_kind\":\"cursor_handle\""),
          "ROUTINE-CURSOR-GATE-001 payload missing cursor handle descriptor");
  Require(Contains(artifacts.envelope.payload, "\"routine_cursor_argument\":true"),
          "ROUTINE-CURSOR-GATE-001 payload missing cursor argument flag");
  Require(Contains(artifacts.envelope.payload,
                   "\"routine_cursor_argument_binding\":\"descriptor.cursor_handle.session_registry\""),
          "ROUTINE-CURSOR-GATE-001 payload missing session-registry binding");
  Require(Contains(artifacts.envelope.payload,
                   "\"routine_cursor_argument_parser_executes_cursor\":false"),
          "ROUTINE-CURSOR-GATE-001 parser overclaimed cursor execution");
  Require(!Contains(artifacts.envelope.payload, std::string(sql)) &&
              !Contains(artifacts.envelope.payload, "route_cursor"),
          "ROUTINE-CURSOR-GATE-001 payload embedded SQL text or parameter name");

  const auto admission = scratchbird::server::AdmitServerSblrEnvelope(
      scratchbird::server::ServerSblrAdmissionRequest{artifacts.envelope.payload, false});
  Require(admission.admitted, "ROUTINE-CURSOR-GATE-009 server admission rejected cursor signature");
  Require(admission.requires_public_abi_dispatch,
          "ROUTINE-CURSOR-GATE-009 server admission skipped public ABI dispatch");
  Require(admission.operation_id == operation_id,
          "ROUTINE-CURSOR-GATE-009 admission operation id drifted");
}

std::string ParserJsonEnvelope(std::uint64_t stream_rows) {
  std::string out = "{\"envelope\":\"SBLRExecutionEnvelope.v3\",";
  out += "\"operation_family\":\"sblr.query.relational.v3\",";
  out += "\"surface_key\":\"routine_cursor_argument.fixture\",";
  out += "\"sblr_operation_key\":\"routine_cursor_argument.fixture\",";
  out += "\"result_shape\":\"routine_cursor_argument.rows.v1\",";
  out += "\"diagnostic_shape\":\"routine_cursor_argument.diag.v1\",";
  out += "\"resource_contract\":\"routine_cursor_argument.resource.v1\",";
  out += "\"trace_key\":\"ROUTINE-CURSOR-PROOF-CLOSURE\",";
  out += "\"stream_row_count\":";
  out += std::to_string(stream_rows);
  out += ",\"source_payload_embedded\":false,";
  out += "\"resolved_object_uuids\":[],\"descriptor_refs\":[\"sys.server.cursor_descriptor\"],";
  out += "\"policy_refs\":[\"routine_cursor_session_registry_policy\"]}";
  return out;
}

std::string RoutineCursorEnvelope(
    const std::array<std::uint8_t, 16>& cursor_uuid,
    std::string_view context_kind,
    std::string_view action,
    std::string_view borrow_policy = "borrowed_read",
    std::string_view descriptor = "rowshape:int64:value",
    std::string_view expected_descriptor = "rowshape:int64:value",
    bool security_rechecked = true,
    bool protected_material_rechecked = true,
    bool deterministic_context = false,
    std::string_view lifetime = "") {
  std::string out = "{\"envelope\":\"SBLRExecutionEnvelope.v3\",";
  out += "\"operation_family\":\"sblr.routine.execute.v3\",";
  out += "\"operation_id\":\"routine.execute_cursor_argument\",";
  out += "\"surface_key\":\"routine_cursor_argument.runtime\",";
  out += "\"sblr_operation_key\":\"routine_cursor_argument.runtime\",";
  out += "\"result_shape\":\"routine_cursor_argument.rows.v1\",";
  out += "\"diagnostic_shape\":\"routine_cursor_argument.diag.v1\",";
  out += "\"resource_contract\":\"routine_cursor_argument.resource.v1\",";
  out += "\"trace_key\":\"ROUTINE-CURSOR-PROOF-CLOSURE\",";
  out += "\"source_payload_embedded\":false,";
  out += "\"routine_cursor_uuid\":\"";
  out += scratchbird::server::UuidBytesToText(cursor_uuid);
  out += "\",\"routine_context_kind\":\"";
  out += std::string(context_kind);
  out += "\",\"routine_cursor_action\":\"";
  out += std::string(action);
  out += "\",\"routine_cursor_borrow_policy\":\"";
  out += std::string(borrow_policy);
  out += "\",\"routine_cursor_argument_binding\":\"descriptor.cursor_handle.session_registry\",";
  out += "\"routine_cursor_descriptor\":\"";
  out += std::string(descriptor);
  out += "\",\"routine_expected_cursor_descriptor\":\"";
  out += std::string(expected_descriptor);
  out += "\",\"routine_security_recheck\":\"";
  out += security_rechecked ? "passed" : "missing";
  out += "\",\"routine_protected_material_policy\":\"";
  out += protected_material_rechecked ? "rechecked" : "missing";
  out += "\",\"routine_deterministic_context\":";
  out += deterministic_context ? "true" : "false";
  out += ",\"routine_cursor_fetch_max_rows\":1,";
  if (!lifetime.empty()) {
    out += "\"routine_cursor_lifetime\":\"";
    out += std::string(lifetime);
    out += "\",";
  }
  out += "\"resolved_object_uuids\":[],";
  out += "\"descriptor_refs\":[\"sys.server.cursor_descriptor\",";
  out += "\"sys.routine.cursor_parameter_descriptor\"],";
  out += "\"policy_refs\":[\"routine_cursor_session_registry_policy\",";
  out += "\"routine_cursor_security_recheck_policy\"]}";
  return out;
}

sbps::Frame ExecuteFrame(const std::array<std::uint8_t, 16>& session_uuid,
                         std::uint64_t rows) {
  sbps::Frame frame;
  frame.header.message_type = static_cast<std::uint16_t>(sbps::MessageType::kExecuteSblr);
  frame.header.request_uuid = sbps::MakeUuidV7Bytes();
  frame.header.session_uuid = session_uuid;
  frame.payload = scratchbird::server::EncodeExecuteSblrPayloadForTest(
      session_uuid, {}, ParserJsonEnvelope(rows), true);
  return frame;
}

sbps::Frame ExecuteEnvelopeFrame(
    const std::array<std::uint8_t, 16>& session_uuid,
    const std::string& envelope) {
  sbps::Frame frame;
  frame.header.message_type =
      static_cast<std::uint16_t>(sbps::MessageType::kExecuteSblr);
  frame.header.request_uuid = sbps::MakeUuidV7Bytes();
  frame.header.session_uuid = session_uuid;
  frame.payload = scratchbird::server::EncodeExecuteSblrPayloadForTest(
      session_uuid, {}, envelope, false);
  return frame;
}

sbps::Frame FetchFrame(const std::array<std::uint8_t, 16>& session_uuid,
                       const std::array<std::uint8_t, 16>& cursor_uuid,
                       std::uint64_t max_rows = 1) {
  sbps::Frame frame;
  frame.header.message_type = static_cast<std::uint16_t>(sbps::MessageType::kFetch);
  frame.header.request_uuid = sbps::MakeUuidV7Bytes();
  frame.header.session_uuid = session_uuid;
  frame.payload = scratchbird::server::EncodeFetchPayloadForTest(
      session_uuid, cursor_uuid, max_rows, 4096);
  return frame;
}

sbps::Frame CloseFrame(const std::array<std::uint8_t, 16>& session_uuid,
                       const std::array<std::uint8_t, 16>& cursor_uuid) {
  sbps::Frame frame;
  frame.header.message_type = static_cast<std::uint16_t>(sbps::MessageType::kCloseCursor);
  frame.header.request_uuid = sbps::MakeUuidV7Bytes();
  frame.header.session_uuid = session_uuid;
  frame.payload = scratchbird::server::EncodeCloseCursorPayloadForTest(session_uuid, cursor_uuid);
  return frame;
}

void AddSession(ServerSessionRegistry* registry,
                const std::array<std::uint8_t, 16>& session_uuid) {
  ServerSessionRecord session;
  session.session_uuid = session_uuid;
  session.auth_context_uuid = sbps::MakeUuidV7Bytes();
  session.principal_uuid = sbps::MakeUuidV7Bytes();
  session.effective_user_uuid = session.principal_uuid;
  session.database_path = "/tmp/sbsql_routine_cursor_argument_conformance.sbdb";
  session.database_uuid = "019f0000-0000-7000-8000-000000450101";
  registry->sessions_by_uuid[scratchbird::server::UuidBytesToText(session.session_uuid)] = session;
}

HostedEngineState MakeEngineState() {
  HostedEngineState state;
  state.engine_context_active = true;
  HostedDatabaseSnapshot database;
  database.state = HostedDatabaseState::kOpen;
  database.database_open = true;
  database.database_path = "/tmp/sbsql_routine_cursor_argument_conformance.sbdb";
  database.database_uuid = "019f0000-0000-7000-8000-000000450101";
  state.databases.push_back(database);
  return state;
}

std::array<std::uint8_t, 16> OpenSyntheticCursor(
    ServerSessionRegistry* registry,
    const HostedEngineState& engine_state,
    const std::array<std::uint8_t, 16>& session_uuid,
    std::uint64_t rows) {
  const auto execute = scratchbird::server::HandleExecuteSblr(
      registry, engine_state, ExecuteFrame(session_uuid, rows));
  Require(execute.accepted, "ROUTINE-CURSOR-GATE-009 cursor open rejected");
  const auto cursor_uuid =
      scratchbird::server::DecodeCursorUuidForTest(execute.payload);
  Require(cursor_uuid.has_value(), "ROUTINE-CURSOR-GATE-009 cursor UUID missing");
  return *cursor_uuid;
}

SessionOperationResult ExecuteRoutineCursor(
    ServerSessionRegistry* registry,
    const HostedEngineState& engine_state,
    const std::array<std::uint8_t, 16>& session_uuid,
    const std::array<std::uint8_t, 16>& cursor_uuid,
    std::string_view context_kind,
    std::string_view action,
    std::string_view borrow_policy = "borrowed_read",
    std::string_view descriptor = "rowshape:int64:value",
    std::string_view expected_descriptor = "rowshape:int64:value",
    bool security_rechecked = true,
    bool protected_material_rechecked = true,
    bool deterministic_context = false,
    std::string_view lifetime = "") {
  return scratchbird::server::HandleExecuteSblr(
      registry,
      engine_state,
      ExecuteEnvelopeFrame(session_uuid,
                           RoutineCursorEnvelope(cursor_uuid,
                                                 context_kind,
                                                 action,
                                                 borrow_policy,
                                                 descriptor,
                                                 expected_descriptor,
                                                 security_rechecked,
                                                 protected_material_rechecked,
                                                 deterministic_context,
                                                 lifetime)));
}

void RequireRuntimeCursorHandleRegistry() {
  ServerSessionRegistry registry;
  const auto owner_session = sbps::MakeUuidV7Bytes();
  const auto other_session = sbps::MakeUuidV7Bytes();
  AddSession(&registry, owner_session);
  AddSession(&registry, other_session);
  const auto engine_state = MakeEngineState();

  const auto cursor_uuid =
      OpenSyntheticCursor(&registry, engine_state, owner_session, 2);

  const auto first_fetch = scratchbird::server::HandleFetch(
      &registry, FetchFrame(owner_session, cursor_uuid, 1));
  Require(first_fetch.accepted, "ROUTINE-CURSOR-GATE-002 owner fetch rejected");
  const auto decoded_fetch =
      scratchbird::server::DecodeFetchResultForTest(first_fetch.payload);
  Require(decoded_fetch.has_value() && decoded_fetch->row_count == 1,
          "ROUTINE-CURSOR-GATE-002 owner fetch did not advance one row");

  const auto cross_session_fetch = scratchbird::server::HandleFetch(
      &registry, FetchFrame(other_session, cursor_uuid, 1));
  Require(!cross_session_fetch.accepted &&
              HasDiagnostic(cross_session_fetch, "PARSER_SERVER_IPC.CURSOR_NOT_FOUND"),
          "ROUTINE-CURSOR-GATE-007 cross-session fetch did not fail closed");

  const auto cross_session_close = scratchbird::server::HandleCloseCursor(
      &registry, CloseFrame(other_session, cursor_uuid));
  Require(!cross_session_close.accepted &&
              HasDiagnostic(cross_session_close, "PARSER_SERVER_IPC.CURSOR_NOT_FOUND"),
          "ROUTINE-CURSOR-GATE-003 cross-session close did not fail closed");

  const auto owner_close = scratchbird::server::HandleCloseCursor(
      &registry, CloseFrame(owner_session, cursor_uuid));
  Require(owner_close.accepted, "ROUTINE-CURSOR-GATE-008 owner close rejected");

  const auto stale_fetch = scratchbird::server::HandleFetch(
      &registry, FetchFrame(owner_session, cursor_uuid, 1));
  Require(!stale_fetch.accepted &&
              HasDiagnostic(stale_fetch, "PARSER_SERVER_IPC.CURSOR_NOT_FOUND"),
          "ROUTINE-CURSOR-GATE-008 stale cursor fetch did not fail closed");
}

void RequireRoutineCursorBodyFetchExecution() {
  ServerSessionRegistry registry;
  const auto owner_session = sbps::MakeUuidV7Bytes();
  AddSession(&registry, owner_session);
  const auto engine_state = MakeEngineState();
  const auto cursor_uuid =
      OpenSyntheticCursor(&registry, engine_state, owner_session, 3);

  const auto executed = ExecuteRoutineCursor(&registry,
                                             engine_state,
                                             owner_session,
                                             cursor_uuid,
                                             "procedure",
                                             "fetch");
  Require(executed.accepted,
          "ROUTINE-CURSOR-GATE-002 routine cursor fetch rejected");
  const auto decoded = DecodeExecuteResultForTest(executed.payload);
  Require(decoded.has_value() && decoded->row_count == 1,
          "ROUTINE-CURSOR-GATE-002 routine cursor fetch row count drifted");
  Require(decoded->operation_id == "routine.execute_cursor_argument",
          "ROUTINE-CURSOR-GATE-002 routine operation id drifted");
  Require(Contains(decoded->detail, "\"parser_executes_cursor\":false"),
          "ROUTINE-CURSOR-GATE-002 parser cursor execution evidence missing");

  const auto cursor_it =
      registry.cursors_by_uuid.find(scratchbird::server::UuidBytesToText(cursor_uuid));
  Require(cursor_it != registry.cursors_by_uuid.end() &&
              cursor_it->second.fetch_count == 1 &&
              cursor_it->second.next_row_index == 1,
          "ROUTINE-CURSOR-GATE-002 routine fetch did not advance original handle");

  const auto owner_fetch = scratchbird::server::HandleFetch(
      &registry, FetchFrame(owner_session, cursor_uuid, 1));
  Require(owner_fetch.accepted,
          "ROUTINE-CURSOR-GATE-002 owner fetch after routine rejected");
  const auto owner_decoded =
      scratchbird::server::DecodeFetchResultForTest(owner_fetch.payload);
  Require(owner_decoded.has_value() && owner_decoded->row_count == 1,
          "ROUTINE-CURSOR-GATE-002 owner fetch after routine row count drifted");
}

void RequireRoutineCursorBorrowedCloseRefusal() {
  ServerSessionRegistry registry;
  const auto owner_session = sbps::MakeUuidV7Bytes();
  AddSession(&registry, owner_session);
  const auto engine_state = MakeEngineState();
  const auto cursor_uuid =
      OpenSyntheticCursor(&registry, engine_state, owner_session, 1);

  const auto refused = ExecuteRoutineCursor(&registry,
                                            engine_state,
                                            owner_session,
                                            cursor_uuid,
                                            "procedure",
                                            "close",
                                            "borrowed_read");
  Require(!refused.accepted &&
              HasDiagnostic(refused,
                            "PARSER_SERVER_IPC.ROUTINE_CURSOR_BORROWED_CLOSE_REFUSED"),
          "ROUTINE-CURSOR-GATE-003 borrowed close did not fail closed");

  const auto owner_fetch = scratchbird::server::HandleFetch(
      &registry, FetchFrame(owner_session, cursor_uuid, 1));
  Require(owner_fetch.accepted,
          "ROUTINE-CURSOR-GATE-003 borrowed close refusal closed owner handle");
}

void RequireRoutineCursorFunctionPolicy() {
  ServerSessionRegistry registry;
  const auto owner_session = sbps::MakeUuidV7Bytes();
  AddSession(&registry, owner_session);
  const auto engine_state = MakeEngineState();
  const auto cursor_uuid =
      OpenSyntheticCursor(&registry, engine_state, owner_session, 1);

  const auto inspected = ExecuteRoutineCursor(&registry,
                                              engine_state,
                                              owner_session,
                                              cursor_uuid,
                                              "function",
                                              "inspect");
  Require(inspected.accepted,
          "ROUTINE-CURSOR-GATE-004 admitted function cursor inspection rejected");
  const auto cursor_it =
      registry.cursors_by_uuid.find(scratchbird::server::UuidBytesToText(cursor_uuid));
  Require(cursor_it != registry.cursors_by_uuid.end() &&
              cursor_it->second.fetch_count == 0,
          "ROUTINE-CURSOR-GATE-004 metadata inspection advanced cursor");

  const auto refused = ExecuteRoutineCursor(&registry,
                                            engine_state,
                                            owner_session,
                                            cursor_uuid,
                                            "function",
                                            "inspect",
                                            "borrowed_read",
                                            "rowshape:int64:value",
                                            "rowshape:int64:value",
                                            true,
                                            true,
                                            true);
  Require(!refused.accepted &&
              HasDiagnostic(
                  refused,
                  "PARSER_SERVER_IPC.ROUTINE_CURSOR_DETERMINISTIC_CONTEXT_REFUSED"),
          "ROUTINE-CURSOR-GATE-004 deterministic function context not refused");
}

void RequireRoutineCursorDescriptorAndSecurityRefusals() {
  ServerSessionRegistry registry;
  const auto owner_session = sbps::MakeUuidV7Bytes();
  AddSession(&registry, owner_session);
  const auto engine_state = MakeEngineState();
  const auto cursor_uuid =
      OpenSyntheticCursor(&registry, engine_state, owner_session, 1);

  const auto descriptor_refused = ExecuteRoutineCursor(&registry,
                                                       engine_state,
                                                       owner_session,
                                                       cursor_uuid,
                                                       "procedure",
                                                       "fetch",
                                                       "borrowed_read",
                                                       "rowshape:int64:value",
                                                       "rowshape:text:value");
  Require(!descriptor_refused.accepted &&
              HasDiagnostic(
                  descriptor_refused,
                  "PARSER_SERVER_IPC.ROUTINE_CURSOR_DESCRIPTOR_MISMATCH"),
          "ROUTINE-CURSOR-GATE-006 descriptor mismatch not refused");

  const auto security_refused = ExecuteRoutineCursor(&registry,
                                                     engine_state,
                                                     owner_session,
                                                     cursor_uuid,
                                                     "procedure",
                                                     "fetch",
                                                     "borrowed_read",
                                                     "rowshape:int64:value",
                                                     "rowshape:int64:value",
                                                     false,
                                                     true);
  Require(!security_refused.accepted &&
              HasDiagnostic(
                  security_refused,
                  "PARSER_SERVER_IPC.ROUTINE_CURSOR_SECURITY_RECHECK_REQUIRED"),
          "ROUTINE-CURSOR-GATE-007 missing security recheck not refused");

  const auto protected_refused = ExecuteRoutineCursor(&registry,
                                                      engine_state,
                                                      owner_session,
                                                      cursor_uuid,
                                                      "procedure",
                                                      "fetch",
                                                      "borrowed_read",
                                                      "rowshape:int64:value",
                                                      "rowshape:int64:value",
                                                      true,
                                                      false);
  Require(!protected_refused.accepted &&
              HasDiagnostic(
                  protected_refused,
                  "PARSER_SERVER_IPC.ROUTINE_CURSOR_SECURITY_RECHECK_REQUIRED"),
          "ROUTINE-CURSOR-GATE-007 missing protected-material recheck not refused");
}

void RequireTriggerScopedRoutineCursorCleanup() {
  ServerSessionRegistry registry;
  const auto owner_session = sbps::MakeUuidV7Bytes();
  AddSession(&registry, owner_session);
  const auto engine_state = MakeEngineState();
  const auto cursor_uuid =
      OpenSyntheticCursor(&registry, engine_state, owner_session, 2);

  const auto executed = ExecuteRoutineCursor(&registry,
                                             engine_state,
                                             owner_session,
                                             cursor_uuid,
                                             "trigger",
                                             "fetch",
                                             "borrowed_read",
                                             "rowshape:int64:value",
                                             "rowshape:int64:value",
                                             true,
                                             true,
                                             false,
                                             "trigger_event");
  Require(executed.accepted,
          "ROUTINE-CURSOR-GATE-005 trigger routine cursor call rejected");
  const auto decoded = DecodeExecuteResultForTest(executed.payload);
  Require(decoded.has_value() &&
              Contains(decoded->detail, "trigger_event_scope_cleanup"),
          "ROUTINE-CURSOR-GATE-005 trigger cleanup evidence missing");

  const auto after_trigger_fetch = scratchbird::server::HandleFetch(
      &registry, FetchFrame(owner_session, cursor_uuid, 1));
  Require(!after_trigger_fetch.accepted &&
              HasDiagnostic(after_trigger_fetch,
                            "PARSER_SERVER_IPC.CURSOR_NOT_FOUND"),
          "ROUTINE-CURSOR-GATE-005 trigger-scoped cursor was not cleaned up");
}

}  // namespace

int main() {
  // SEARCH_KEY: ROUTINE-CURSOR-PROOF-CLOSURE
  // SEARCH_KEY: EDR-038
  // SEARCH_KEY: ROUTINE-CURSOR-GATE-001 ROUTINE-CURSOR-GATE-002
  // SEARCH_KEY: ROUTINE-CURSOR-GATE-003 ROUTINE-CURSOR-GATE-004
  // SEARCH_KEY: ROUTINE-CURSOR-GATE-005 ROUTINE-CURSOR-GATE-006
  // SEARCH_KEY: ROUTINE-CURSOR-GATE-007
  // SEARCH_KEY: ROUTINE-CURSOR-GATE-008 ROUTINE-CURSOR-GATE-009
  RequireCursorRoutineSignature(
      "CREATE PROCEDURE replay_cursor_procedure(route_cursor cursor);",
      "ddl.create_procedure",
      "SBLR_DDL_CREATE_PROCEDURE");
  RequireCursorRoutineSignature(
      "CREATE FUNCTION inspect_cursor_function(route_cursor cursor);",
      "ddl.create_function",
      "SBLR_DDL_CREATE_FUNCTION");
  RequireCursorRoutineSignature(
      "CREATE TRIGGER cursor_trigger(route_cursor cursor);",
      "ddl.create_trigger",
      "SBLR_DDL_CREATE_TRIGGER");
  RequireRuntimeCursorHandleRegistry();
  RequireRoutineCursorBodyFetchExecution();
  RequireRoutineCursorBorrowedCloseRefusal();
  RequireRoutineCursorFunctionPolicy();
  RequireRoutineCursorDescriptorAndSecurityRefusals();
  RequireTriggerScopedRoutineCursorCleanup();

  std::cout << "sbsql_routine_cursor_argument_conformance=passed\n";
  return EXIT_SUCCESS;
}
