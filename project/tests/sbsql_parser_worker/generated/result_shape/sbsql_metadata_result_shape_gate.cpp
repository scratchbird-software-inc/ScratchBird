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
#include "hash_digest.hpp"
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

bool HasDiagnostic(const sbsql::MessageVectorSet& messages,
                   std::string_view code) {
  for (const auto& diagnostic : messages.diagnostics) {
    if (diagnostic.code == code) return true;
  }
  return false;
}

void PrintServerDiagnostics(
    const scratchbird::server::SessionOperationResult& result) {
  if (result.accepted) return;
  for (const auto& diagnostic : result.diagnostics) {
    std::cerr << "server diagnostic: " << diagnostic.code;
    for (const auto& field : diagnostic.fields) {
      std::cerr << ' ' << field.key << '=' << field.value;
    }
    std::cerr << '\n';
  }
}

void ValidateBinderResultShapes(Harness* harness) {
  struct Case {
    std::string_view sql;
    std::string_view expected_result_shape;
    std::string_view expected_right;
    std::vector<std::string> resolved_object_uuids;
    bool requires_native_engine_context{false};
  };

  const std::vector<Case> cases = {
      {"SELECT 1", "result.shape.rowset", "right.read", {}, true},
      {"VALUES (1)", "result.shape.rowset", "right.read", {}, true},
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
    if (item.requires_native_engine_context) {
      harness->Check(!bound.bound && bound.messages.has_errors() &&
                         HasDiagnostic(bound.messages,
                                       "QOW-DIAG-BOUNDAST-SCOPE"),
                     std::string("native statement did not require its exact "
                                 "engine-issued binding context: ") +
                         std::string(item.sql));
    } else {
      harness->Check(bound.bound,
                     std::string("statement did not bind: ") +
                         std::string(item.sql));
      harness->Check(!bound.messages.has_errors(),
                     std::string("statement produced binder diagnostics: ") +
                         std::string(item.sql));
    }
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

void PutU16(std::vector<std::uint8_t>* out, std::uint16_t value) {
  out->push_back(static_cast<std::uint8_t>(value));
  out->push_back(static_cast<std::uint8_t>(value >> 8));
}

void PutU64(std::vector<std::uint8_t>* out, std::uint64_t value) {
  for (unsigned shift = 0; shift != 64; shift += 8) {
    out->push_back(static_cast<std::uint8_t>(value >> shift));
  }
}

void PutUuid(std::vector<std::uint8_t>* out,
             const std::array<std::uint8_t, 16>& value) {
  out->insert(out->end(), value.begin(), value.end());
}

sbps::Frame FetchFrame(const std::array<std::uint8_t, 16>& session_uuid,
                       const scratchbird::server::ServerCursorRecord& cursor,
                       std::uint64_t max_rows,
                       std::uint64_t max_bytes = 65536) {
  sbps::Frame frame;
  frame.header.message_type = static_cast<std::uint16_t>(sbps::MessageType::kFetch);
  frame.header.request_uuid = sbps::MakeUuidV7Bytes();
  frame.header.session_uuid = session_uuid;
  frame.payload = scratchbird::server::EncodeFetchPayloadForTest(
      session_uuid, cursor.cursor_uuid, max_rows, max_bytes);
  frame.payload.insert(frame.payload.end(), cursor.stream_descriptor_uuid.begin(),
                       cursor.stream_descriptor_uuid.end());
  PutU16(&frame.payload, cursor.stream_descriptor_version);
  PutU64(&frame.payload, cursor.stream_descriptor_generation);
  return frame;
}

std::array<std::uint8_t, 16> InstallMetadataCursor(
    scratchbird::server::ServerSessionRegistry* registry,
    const std::array<std::uint8_t, 16>& session_uuid,
    std::uint64_t total_rows,
    std::uint64_t multi_result_count = 0,
    std::uint64_t partial_result_rows = 0,
    std::uint64_t warning_count = 0) {
  scratchbird::server::ServerCursorRecord cursor;
  cursor.cursor_uuid = sbps::MakeUuidV7Bytes();
  cursor.request_uuid = sbps::MakeUuidV7Bytes();
  cursor.session_uuid = session_uuid;
  cursor.operation_id = "query.execute";
  cursor.total_row_count = total_rows;
  cursor.multi_result_count = multi_result_count;
  cursor.multi_result_kind = multi_result_count == 0 ? "" : "multi_result_sequence";
  cursor.partial_result_rows = partial_result_rows;
  cursor.warning_count = warning_count;
  cursor.warning_stream_kind = warning_count == 0
                                   ? ""
                                   : "partial_result_warning_chain";
  cursor.max_chunk_rows = 4;
  cursor.max_chunk_bytes = 65536;
  cursor.stream_descriptor_uuid = sbps::MakeUuidV7Bytes();
  cursor.stream_descriptor_version = 1;
  cursor.stream_descriptor_generation = 1;
  cursor.execution_uuid = sbps::MakeUuidV7Bytes();
  cursor.result_set_uuid = sbps::MakeUuidV7Bytes();
  cursor.row_descriptor_uuid = sbps::MakeUuidV7Bytes();
  cursor.snapshot_uuid = sbps::MakeUuidV7Bytes();
  cursor.statement_context_statement_uuid =
      scratchbird::server::UuidBytesToText(sbps::MakeUuidV7Bytes());

  scratchbird::server::ServerStatementContextRecord statement_context;
  statement_context.session_uuid = session_uuid;
  statement_context.statement_uuid = cursor.statement_context_statement_uuid;
  statement_context.receipt.opaque_id = 42;
  registry->statement_contexts_by_statement_uuid.emplace(
      statement_context.statement_uuid, statement_context);

  std::vector<std::uint8_t> binding;
  constexpr std::string_view kDomain =
      "ScratchBird.CursorStreamDescriptor.ReceiptBinding.V1";
  binding.insert(binding.end(), kDomain.begin(), kDomain.end());
  PutU64(&binding, statement_context.receipt.opaque_id);
  PutUuid(&binding, cursor.stream_descriptor_uuid);
  PutU16(&binding, cursor.stream_descriptor_version);
  PutU64(&binding, cursor.stream_descriptor_generation);
  PutUuid(&binding, cursor.cursor_uuid);
  PutUuid(&binding, cursor.execution_uuid);
  PutUuid(&binding, cursor.result_set_uuid);
  PutUuid(&binding, cursor.row_descriptor_uuid);
  PutUuid(&binding, cursor.snapshot_uuid);
  PutU64(&binding, cursor.max_chunk_rows);
  PutU64(&binding, cursor.max_chunk_bytes);
  const auto digest = scratchbird::core::hash::ComputeSha256Digest(binding);
  if (!digest.ok()) return {};
  cursor.stream_descriptor_receipt_binding_sha256 = digest.digest;
  cursor.stream_descriptor_live = true;
  const auto cursor_uuid = cursor.cursor_uuid;
  registry->cursors_by_uuid.emplace(
      scratchbird::server::UuidBytesToText(cursor_uuid), std::move(cursor));
  return cursor_uuid;
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
  const auto cursor_uuid = InstallMetadataCursor(&registry, session_uuid, 3);
  harness->Check(!sbps::IsZeroUuid(cursor_uuid),
                 "descriptor-bound rowset cursor fixture was not installed");
  const auto cursor = registry.cursors_by_uuid.at(
      scratchbird::server::UuidBytesToText(cursor_uuid));

  const auto fetch = scratchbird::server::HandleFetch(
      &registry, FetchFrame(session_uuid, cursor, 2, 4096));
  PrintServerDiagnostics(fetch);
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
  const auto cursor_uuid = InstallMetadataCursor(&registry, session_uuid, 5, 2);
  harness->Check(!sbps::IsZeroUuid(cursor_uuid),
                 "descriptor-bound multi-result cursor fixture was not installed");
  const auto cursor = registry.cursors_by_uuid.at(
      scratchbird::server::UuidBytesToText(cursor_uuid));

  const auto fetch = scratchbird::server::HandleFetch(
      &registry, FetchFrame(session_uuid, cursor, 4));
  PrintServerDiagnostics(fetch);
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
      &registry, FetchFrame(session_uuid, cursor, 1));
  PrintServerDiagnostics(final_fetch);
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
  const auto cursor_uuid = InstallMetadataCursor(&registry, session_uuid, 4, 0, 1, 2);
  harness->Check(!sbps::IsZeroUuid(cursor_uuid),
                 "descriptor-bound warning cursor fixture was not installed");
  const auto cursor = registry.cursors_by_uuid.at(
      scratchbird::server::UuidBytesToText(cursor_uuid));

  const auto fetch = scratchbird::server::HandleFetch(
      &registry, FetchFrame(session_uuid, cursor, 4));
  PrintServerDiagnostics(fetch);
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
