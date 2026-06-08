// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "ast/ast.hpp"
#include "binder/binder.hpp"
#include "common/common.hpp"
#include "cst/cst.hpp"
#include "sblr_dispatch_server.hpp"
#include "session_registry.hpp"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace sbsql = scratchbird::parser::sbsql;
namespace sbps = scratchbird::server::sbps;

struct Harness {
  bool ok{true};
  std::size_t failures{0};

  void Check(bool condition, std::string_view message) {
    if (condition) return;
    ok = false;
    ++failures;
    if (failures <= 100) std::cerr << message << '\n';
  }
};

bool Contains(std::string_view haystack, std::string_view needle) {
  return haystack.find(needle) != std::string_view::npos;
}

std::size_t CountOccurrences(std::string_view haystack, std::string_view needle) {
  std::size_t count = 0;
  std::size_t pos = 0;
  while ((pos = haystack.find(needle, pos)) != std::string_view::npos) {
    ++count;
    pos += needle.size();
  }
  return count;
}

sbsql::ParserConfig ParserConfig() {
  sbsql::ParserConfig config;
  config.probe_mode = true;
  config.parser_uuid = "00000000-0000-7000-8000-0000000012f6";
  config.bundle_contract_id = "sbp_sbsql@metadata-result-shape-gate";
  config.build_id = "fspe-012f";
  config.server_endpoint = "unix:/tmp/fspe012f-result-shape-resolver.sock";
  return config;
}

sbsql::SessionContext AuthenticatedSession() {
  sbsql::SessionContext session;
  session.authenticated = true;
  session.session_uuid = "00000000-0000-7000-8000-0000000012f0";
  session.connection_uuid = "00000000-0000-7000-8000-0000000012f1";
  session.database_uuid = "00000000-0000-7000-8000-0000000012f2";
  session.catalog_epoch = 12;
  session.security_policy_epoch = 13;
  session.descriptor_epoch = 14;
  return session;
}

sbsql::BoundStatement BindSql(
    std::string_view sql,
    const std::vector<std::string>& resolved_object_uuids = {}) {
  const auto cst = sbsql::BuildCst(sql);
  const auto ast = sbsql::BuildAst(cst);
  return sbsql::BindAst(ast, cst, ParserConfig(), AuthenticatedSession(),
                        resolved_object_uuids);
}

void ValidateBinderResultShapes(Harness* harness) {
  struct Case {
    std::string_view sql;
    std::string_view expected_result_shape;
    std::string_view expected_right;
    std::vector<std::string> resolved_object_uuids;
  };

  const std::vector<Case> cases = {
      {"SELECT 1", "result.shape.rowset", "right.read", {}},
      {"VALUES (1)", "result.shape.rowset", "right.read", {}},
      {"SHOW METRICS", "result.shape.management_report", "right.observe", {}},
      {"CALL p()", "result.shape.routine_result", "right.execute",
       {"00000000-0000-7000-8000-00000000d000"}},
      {"INSERT INTO t VALUES (1)", "result.shape.command_status", "right.write",
       {"00000000-0000-7000-8000-00000000d001"}},
      {"CREATE TABLE t (id int)", "result.shape.command_status", "right.catalog_mutate",
       {"00000000-0000-7000-8000-00000000d002"}},
      {"GRANT SELECT ON t TO r", "result.shape.command_status", "right.security_admin",
       {"00000000-0000-7000-8000-00000000d003"}},
  };

  for (const auto& item : cases) {
    const auto bound = BindSql(item.sql, item.resolved_object_uuids);
    harness->Check(bound.bound, std::string("statement did not bind: ") +
                                   std::string(item.sql));
    harness->Check(!bound.messages.has_errors(),
                   std::string("statement produced binder diagnostics: ") +
                       std::string(item.sql));
    harness->Check(bound.result_shape_key == item.expected_result_shape,
                   std::string("result shape mismatch for ") + std::string(item.sql));
    harness->Check(!bound.required_rights.empty() &&
                       bound.required_rights.front() == item.expected_right,
                   std::string("required right mismatch for ") + std::string(item.sql));
    harness->Check(bound.diagnostic_shape_key == bound.diagnostic_key,
                   std::string("diagnostic shape mismatch for ") + std::string(item.sql));
    harness->Check(!bound.resource_contract_key.empty(),
                   std::string("resource contract missing for ") + std::string(item.sql));
  }
}

std::string BaseEnvelope(std::string_view operation_family,
                         std::string_view result_shape,
                         std::string_view trace_key) {
  std::string out = "{\"envelope\":\"SBLRExecutionEnvelope.v3\",";
  out += "\"operation_family\":\"";
  out += operation_family;
  out += "\",\"surface_key\":\"fspe012f.fixture\",";
  out += "\"sblr_operation_key\":\"op.fspe012f.fixture\",";
  out += "\"result_shape\":\"";
  out += result_shape;
  out += "\",\"diagnostic_shape\":\"diag.fspe012f.v1\",";
  out += "\"resource_contract\":\"resource.fspe012f.v1\",";
  out += "\"trace_key\":\"";
  out += trace_key;
  out += "\",\"source_payload_embedded\":false,";
  out += "\"resolved_object_uuids\":[\"019e05df-f012-7000-8000-0000000000ff\"],";
  out += "\"descriptor_refs\":[\"descriptor.fspe012f.result_shape\"],";
  out += "\"policy_refs\":[\"policy.fspe012f.metadata_visibility\"]";
  return out;
}

std::string SyntheticRowsetEnvelope(std::uint64_t stream_rows) {
  std::string out = BaseEnvelope("sblr.query.relational.v3",
                                 "result.shape.rowset",
                                 "FSPE-012F-ROWSET");
  out += ",\"stream_row_count\":";
  out += std::to_string(stream_rows);
  out += "}";
  return out;
}

std::string MultiResultEnvelope(std::uint64_t result_sets) {
  std::string out = BaseEnvelope("sblr.dml.operation.v3",
                                 "result.shape.multi_result",
                                 "FSPE-012F-MULTI");
  out += ",\"multi_result_count\":";
  out += std::to_string(result_sets);
  out += "}";
  return out;
}

std::string WarningStreamEnvelope(std::uint64_t partial_rows, std::uint64_t warnings) {
  std::string out = BaseEnvelope("sblr.dml.operation.v3",
                                 "result.shape.partial_result_warning_chain",
                                 "FSPE-012F-WARNING");
  out += ",\"partial_result_rows\":";
  out += std::to_string(partial_rows);
  out += ",\"warning_chain_count\":";
  out += std::to_string(warnings);
  out += "}";
  return out;
}

scratchbird::server::HostedEngineState MakeEngineState() {
  scratchbird::server::HostedEngineState state;
  state.engine_context_active = true;
  scratchbird::server::HostedDatabaseSnapshot database;
  database.state = scratchbird::server::HostedDatabaseState::kOpen;
  database.database_open = true;
  database.database_path = "/tmp/sb_metadata_result_shape_gate.sbdb";
  database.database_uuid = "019e05df-f012-7000-8000-0000000000f6";
  state.databases.push_back(database);
  return state;
}

scratchbird::server::ServerSessionRegistry MakeRegistry(
    std::array<std::uint8_t, 16>* session_uuid) {
  scratchbird::server::ServerSessionRecord session;
  session.session_uuid = sbps::MakeUuidV7Bytes();
  session.auth_context_uuid = sbps::MakeUuidV7Bytes();
  session.principal_uuid = sbps::MakeUuidV7Bytes();
  session.effective_user_uuid = session.principal_uuid;
  session.database_path = "/tmp/sb_metadata_result_shape_gate.sbdb";
  session.database_uuid = "019e05df-f012-7000-8000-0000000000f6";
  *session_uuid = session.session_uuid;
  scratchbird::server::ServerSessionRegistry registry;
  registry.sessions_by_uuid[scratchbird::server::UuidBytesToText(session.session_uuid)] =
      session;
  return registry;
}

sbps::Frame ExecuteFrame(const std::array<std::uint8_t, 16>& session_uuid,
                         const std::string& encoded) {
  sbps::Frame frame;
  frame.header.message_type = static_cast<std::uint16_t>(sbps::MessageType::kExecuteSblr);
  frame.header.request_uuid = sbps::MakeUuidV7Bytes();
  frame.header.session_uuid = session_uuid;
  frame.payload = scratchbird::server::EncodeExecuteSblrPayloadForTest(
      session_uuid, {}, encoded, true);
  return frame;
}

sbps::Frame FetchFrame(const std::array<std::uint8_t, 16>& session_uuid,
                       const std::array<std::uint8_t, 16>& cursor_uuid,
                       std::uint64_t max_rows,
                       std::uint64_t max_bytes = 0) {
  sbps::Frame frame;
  frame.header.message_type = static_cast<std::uint16_t>(sbps::MessageType::kFetch);
  frame.header.request_uuid = sbps::MakeUuidV7Bytes();
  frame.header.session_uuid = session_uuid;
  frame.payload = scratchbird::server::EncodeFetchPayloadForTest(
      session_uuid, cursor_uuid, max_rows, max_bytes);
  return frame;
}

std::array<std::uint8_t, 16> OpenCursor(
    scratchbird::server::ServerSessionRegistry* registry,
    const scratchbird::server::HostedEngineState& engine_state,
    const std::array<std::uint8_t, 16>& session_uuid,
    const std::string& encoded,
    Harness* harness) {
  const auto execute = scratchbird::server::HandleExecuteSblr(
      registry, engine_state, ExecuteFrame(session_uuid, encoded));
  harness->Check(execute.accepted, "server execute rejected result-shape fixture");
  const auto cursor_uuid = scratchbird::server::DecodeCursorUuidForTest(execute.payload);
  harness->Check(cursor_uuid.has_value(), "server execute did not return cursor UUID");
  return cursor_uuid.value_or(std::array<std::uint8_t, 16>{});
}

void CheckCanonicalColumns(std::string_view packet, Harness* harness) {
  harness->Check(Contains(packet, "\"columns\":["),
                 "result metadata missing columns array");
  harness->Check(CountOccurrences(packet, "\"ordinal\":") >= 2,
                 "result metadata missing visible and hidden column ordinals");
  harness->Check(Contains(packet, "\"name\":\"value\"") &&
                     Contains(packet, "\"alias\":\"value\""),
                 "visible value column name or alias missing");
  harness->Check(Contains(packet, "\"object_uuid\":\"019e05df-f012-7000-8000-0000000000f0\""),
                 "visible value column UUID missing");
  harness->Check(Contains(packet, "\"type\":\"int64\"") &&
                     Contains(packet, "\"canonical_type\":\"int64\"") &&
                     Contains(packet, "\"domain\":\"sb.int64\""),
                 "visible value column type/domain metadata missing");
  harness->Check(Contains(packet, "\"precision\":19") &&
                     Contains(packet, "\"scale\":0") &&
                     Contains(packet, "\"length\":8"),
                 "visible value column precision/scale/length missing");
  harness->Check(Contains(packet, "\"nullable\":false") &&
                     Contains(packet, "\"generated\":false") &&
                     Contains(packet, "\"hidden\":false") &&
                     Contains(packet, "\"system\":false"),
                 "visible value column visibility/nullability metadata missing");
  harness->Check(Contains(packet, "\"name\":\"_sb_row_version\"") &&
                     Contains(packet, "\"canonical_type\":\"uint64\"") &&
                     Contains(packet, "\"domain\":\"sb.system.row_version\"") &&
                     Contains(packet, "\"generated\":true") &&
                     Contains(packet, "\"computed\":true") &&
                     Contains(packet, "\"hidden\":true") &&
                     Contains(packet, "\"system\":true"),
                 "hidden system row-version metadata missing");
}

void ValidateSyntheticRowsetMetadata(Harness* harness) {
  std::array<std::uint8_t, 16> session_uuid{};
  auto registry = MakeRegistry(&session_uuid);
  const auto engine_state = MakeEngineState();
  const auto cursor_uuid = OpenCursor(
      &registry, engine_state, session_uuid, SyntheticRowsetEnvelope(3), harness);

  const auto fetch = scratchbird::server::HandleFetch(
      &registry, FetchFrame(session_uuid, cursor_uuid, 2, 4096));
  harness->Check(fetch.accepted, "synthetic rowset fetch rejected");
  const auto payload = scratchbird::server::DecodeFetchResultForTest(fetch.payload);
  harness->Check(payload.has_value(), "synthetic rowset fetch payload malformed");
  if (!payload) return;
  harness->Check(payload->row_count == 2 && !payload->end_of_cursor,
                 "synthetic rowset fetch chunk shape mismatch");
  harness->Check(Contains(payload->row_packet, "\"metadata\":"),
                 "rowset packet missing metadata object");
  harness->Check(Contains(payload->row_packet, "\"result_shape\":\"canonical.rowset.v1\""),
                 "rowset packet missing canonical result-shape contract");
  CheckCanonicalColumns(payload->row_packet, harness);
  harness->Check(Contains(payload->row_packet,
                          "\"completion\":{\"command_tag\":\"SELECT 3\","
                          "\"rows_affected\":3,\"returned_rows\":2}"),
                 "rowset completion metadata mismatch");
  harness->Check(Contains(payload->row_packet, "\"warnings\":[]") &&
                     Contains(payload->row_packet, "\"notices\":[]"),
                 "rowset warning/notice metadata missing");
  harness->Check(Contains(payload->row_packet,
                          "\"cursor_metadata\":{\"forward_only\":true,"
                          "\"scrollable\":false,\"updatable\":false,\"holdable\":false}"),
                 "rowset cursor capability metadata missing");
  harness->Check(Contains(payload->detail, "\"metadata_contract\":\"cursor.metadata.v1\"") &&
                     Contains(payload->detail, "\"capability\":\"forward_only\"") &&
                     Contains(payload->detail, "\"scrollable\":false") &&
                     Contains(payload->detail, "\"updatable\":false") &&
                     Contains(payload->detail, "\"holdable\":false") &&
                     Contains(payload->detail, "\"fetch_count\":1") &&
                     Contains(payload->detail, "\"next_row_index\":2"),
                 "fetch detail cursor metadata mismatch");
}

void ValidateMultiResultMetadata(Harness* harness) {
  std::array<std::uint8_t, 16> session_uuid{};
  auto registry = MakeRegistry(&session_uuid);
  const auto engine_state = MakeEngineState();
  const auto cursor_uuid = OpenCursor(
      &registry, engine_state, session_uuid, MultiResultEnvelope(2), harness);

  const auto fetch = scratchbird::server::HandleFetch(
      &registry, FetchFrame(session_uuid, cursor_uuid, 4));
  harness->Check(fetch.accepted, "multi-result metadata fetch rejected");
  const auto payload = scratchbird::server::DecodeFetchResultForTest(fetch.payload);
  harness->Check(payload.has_value(), "multi-result fetch payload malformed");
  if (!payload) return;
  harness->Check(payload->row_count == 4 && !payload->end_of_cursor,
                 "multi-result metadata chunk shape mismatch");
  harness->Check(CountOccurrences(payload->row_packet,
                                  "\"event\":\"result_set_metadata\"") == 2,
                 "multi-result packet missing result-set metadata events");
  harness->Check(Contains(payload->row_packet, "\"result_set_id\":\"rs-0\"") &&
                     Contains(payload->row_packet, "\"result_set_id\":\"rs-1\""),
                 "multi-result result-set IDs missing");
  CheckCanonicalColumns(payload->row_packet, harness);
  harness->Check(Contains(payload->row_packet, "\"command_tag\":\"SELECT 1\"") &&
                     Contains(payload->row_packet, "\"tag\":\"SELECT 1\"") &&
                     Contains(payload->row_packet, "\"rows_affected\":1") &&
                     Contains(payload->row_packet, "\"command_tag\":\"SELECT 2\"") &&
                     Contains(payload->row_packet, "\"tag\":\"SELECT 2\"") &&
                     Contains(payload->row_packet, "\"rows_affected\":2"),
                 "multi-result command tags or affected-row counts missing");

  const auto final_fetch = scratchbird::server::HandleFetch(
      &registry, FetchFrame(session_uuid, cursor_uuid, 1));
  const auto final_payload =
      scratchbird::server::DecodeFetchResultForTest(final_fetch.payload);
  harness->Check(final_fetch.accepted && final_payload.has_value() &&
                     final_payload->end_of_cursor,
                 "multi-result finality fetch failed");
  if (final_payload) {
    harness->Check(Contains(final_payload->row_packet,
                            "\"event\":\"multi_result_finality\"") &&
                       Contains(final_payload->row_packet, "\"result_sets\":2") &&
                       Contains(final_payload->detail, "\"end_of_cursor\":true"),
                   "multi-result finality metadata missing");
  }
}

void ValidateWarningMetadata(Harness* harness) {
  std::array<std::uint8_t, 16> session_uuid{};
  auto registry = MakeRegistry(&session_uuid);
  const auto engine_state = MakeEngineState();
  const auto cursor_uuid = OpenCursor(
      &registry, engine_state, session_uuid, WarningStreamEnvelope(1, 2), harness);

  const auto fetch = scratchbird::server::HandleFetch(
      &registry, FetchFrame(session_uuid, cursor_uuid, 4));
  harness->Check(fetch.accepted, "warning stream fetch rejected");
  const auto payload = scratchbird::server::DecodeFetchResultForTest(fetch.payload);
  harness->Check(payload.has_value(), "warning stream fetch payload malformed");
  if (!payload) return;
  harness->Check(payload->row_count == 4 && payload->end_of_cursor,
                 "warning stream chunk shape mismatch");
  harness->Check(Contains(payload->row_packet, "\"event\":\"partial_result_row\"") &&
                     Contains(payload->row_packet, "\"partial_result\":true"),
                 "warning stream missing partial result row");
  harness->Check(CountOccurrences(payload->row_packet, "\"event\":\"warning\"") == 2 &&
                     Contains(payload->row_packet,
                              "\"diagnostic_code\":\"STREAM.WARNING.0\"") &&
                     Contains(payload->row_packet,
                              "\"diagnostic_code\":\"STREAM.WARNING.1\"") &&
                     Contains(payload->row_packet, "\"severity\":\"WARNING\"") &&
                     Contains(payload->row_packet, "\"does_not_abort\":true"),
                 "warning stream diagnostic metadata missing");
  harness->Check(Contains(payload->row_packet,
                          "\"event\":\"partial_result_finality\"") &&
                     Contains(payload->row_packet,
                              "\"status\":\"completed_with_warnings\"") &&
                     Contains(payload->row_packet, "\"warnings\":2") &&
                     Contains(payload->detail, "\"end_of_cursor\":true"),
                 "warning stream finality metadata missing");
}

}  // namespace

int main() {
  Harness harness;
  ValidateBinderResultShapes(&harness);
  ValidateSyntheticRowsetMetadata(&harness);
  ValidateMultiResultMetadata(&harness);
  ValidateWarningMetadata(&harness);
  if (!harness.ok) {
    std::cerr << "sbsql_metadata_result_shape_gate failures=" << harness.failures << '\n';
    return EXIT_FAILURE;
  }
  std::cout << "sbsql_metadata_result_shape_gate=passed\n";
  return EXIT_SUCCESS;
}
