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

namespace {

using scratchbird::server::HostedDatabaseSnapshot;
using scratchbird::server::HostedDatabaseState;
using scratchbird::server::HostedEngineState;
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

bool HasDiagnostic(const SessionOperationResult& result, std::string_view code) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.code == code) return true;
  }
  return false;
}

bool Contains(std::string_view haystack, std::string_view needle) {
  return haystack.find(needle) != std::string_view::npos;
}

std::string ParserJsonEnvelope(std::string_view family, std::uint64_t stream_rows) {
  std::string out = "{\"envelope\":\"SBLRExecutionEnvelope.v3\",";
  out += "\"operation_family\":\"";
  out += family;
  out += "\",\"surface_key\":\"fspe010b.fixture\",";
  out += "\"sblr_operation_key\":\"op.fspe010b.fixture\",";
  out += "\"result_shape\":\"rs.fspe010b.stream.v1\",";
  out += "\"diagnostic_shape\":\"diag.fspe010b.v1\",";
  out += "\"resource_contract\":\"resource.fspe010b.v1\",";
  out += "\"trace_key\":\"FSPE-010B\",";
  out += "\"stream_row_count\":";
  out += std::to_string(stream_rows);
  out += ",\"source_payload_embedded\":false,";
  out += "\"resolved_object_uuids\":[],\"descriptor_refs\":[],\"policy_refs\":[]}";
  return out;
}

sbps::Frame ExecuteFrame(const std::array<std::uint8_t, 16>& session_uuid,
                         const std::string& encoded,
                         bool cursor_requested) {
  sbps::Frame frame;
  frame.header.message_type = static_cast<std::uint16_t>(sbps::MessageType::kExecuteSblr);
  frame.header.request_uuid = sbps::MakeUuidV7Bytes();
  frame.header.session_uuid = session_uuid;
  frame.payload = scratchbird::server::EncodeExecuteSblrPayloadForTest(
      session_uuid, {}, encoded, cursor_requested);
  return frame;
}

sbps::Frame FetchFrame(const std::array<std::uint8_t, 16>& session_uuid,
                       const std::array<std::uint8_t, 16>& cursor_uuid,
                       std::uint64_t max_rows) {
  sbps::Frame frame;
  frame.header.message_type = static_cast<std::uint16_t>(sbps::MessageType::kFetch);
  frame.header.request_uuid = sbps::MakeUuidV7Bytes();
  frame.header.session_uuid = session_uuid;
  frame.payload = scratchbird::server::EncodeFetchPayloadForTest(session_uuid, cursor_uuid, max_rows);
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

sbps::Frame DisconnectFrame(const std::array<std::uint8_t, 16>& session_uuid) {
  sbps::Frame frame;
  frame.header.message_type = static_cast<std::uint16_t>(sbps::MessageType::kDisconnectNotice);
  frame.header.request_uuid = sbps::MakeUuidV7Bytes();
  frame.header.session_uuid = session_uuid;
  return frame;
}

void AddSession(ServerSessionRegistry* registry, std::array<std::uint8_t, 16>* session_uuid) {
  ServerSessionRecord session;
  session.session_uuid = sbps::MakeUuidV7Bytes();
  session.auth_context_uuid = sbps::MakeUuidV7Bytes();
  session.principal_uuid = sbps::MakeUuidV7Bytes();
  session.effective_user_uuid = session.principal_uuid;
  session.database_path = "/tmp/sb_streaming_result_protocol_conformance.sbdb";
  session.database_uuid = "019e05df-f010-7000-8000-000000000010";
  *session_uuid = session.session_uuid;
  registry->sessions_by_uuid[scratchbird::server::UuidBytesToText(session.session_uuid)] = session;
}

ServerSessionRegistry MakeRegistry(std::array<std::uint8_t, 16>* session_uuid) {
  ServerSessionRegistry registry;
  AddSession(&registry, session_uuid);
  return registry;
}

HostedEngineState MakeEngineState() {
  HostedEngineState state;
  state.engine_context_active = true;
  HostedDatabaseSnapshot database;
  database.state = HostedDatabaseState::kOpen;
  database.database_open = true;
  database.database_path = "/tmp/sb_streaming_result_protocol_conformance.sbdb";
  database.database_uuid = "019e05df-f010-7000-8000-000000000010";
  state.databases.push_back(database);
  return state;
}

std::array<std::uint8_t, 16> OpenCursor(ServerSessionRegistry* registry,
                                        const HostedEngineState& engine_state,
                                        const std::array<std::uint8_t, 16>& session_uuid,
                                        std::uint64_t rows) {
  const auto execute = scratchbird::server::HandleExecuteSblr(
      registry,
      engine_state,
      ExecuteFrame(session_uuid, ParserJsonEnvelope("sblr.query.relational.v3", rows), true));
  Require(execute.accepted, "streaming execute did not accept cursor request");
  auto cursor_uuid = scratchbird::server::DecodeCursorUuidForTest(execute.payload);
  Require(cursor_uuid.has_value(), "streaming execute did not return cursor UUID");
  return *cursor_uuid;
}

}  // namespace

int main() {
  std::array<std::uint8_t, 16> session_uuid{};
  auto registry = MakeRegistry(&session_uuid);
  const auto engine_state = MakeEngineState();

  const auto cursor_uuid = OpenCursor(&registry, engine_state, session_uuid, 5);

  auto fetch1 = scratchbird::server::HandleFetch(&registry, FetchFrame(session_uuid, cursor_uuid, 2));
  Require(fetch1.accepted, "first fetch was rejected");
  const auto fetch1_payload = scratchbird::server::DecodeFetchResultForTest(fetch1.payload);
  Require(fetch1_payload.has_value(), "first fetch payload malformed");
  Require(fetch1_payload->row_count == 2 && !fetch1_payload->end_of_cursor,
          "first fetch did not return first partial chunk");
  Require(Contains(fetch1_payload->row_packet, "\"row_index\":0") &&
              Contains(fetch1_payload->row_packet, "\"row_index\":1") &&
              Contains(fetch1_payload->row_packet, "\"total_rows\":5"),
          "first fetch packet missing stream rows");

  auto fetch2 = scratchbird::server::HandleFetch(&registry, FetchFrame(session_uuid, cursor_uuid, 2));
  Require(fetch2.accepted, "second fetch was rejected");
  const auto fetch2_payload = scratchbird::server::DecodeFetchResultForTest(fetch2.payload);
  Require(fetch2_payload.has_value(), "second fetch payload malformed");
  Require(fetch2_payload->row_count == 2 && !fetch2_payload->end_of_cursor,
          "second fetch did not return middle partial chunk");
  Require(Contains(fetch2_payload->row_packet, "\"row_index\":2") &&
              Contains(fetch2_payload->row_packet, "\"row_index\":3"),
          "second fetch packet missing expected row indexes");

  auto fetch3 = scratchbird::server::HandleFetch(&registry, FetchFrame(session_uuid, cursor_uuid, 2));
  Require(fetch3.accepted, "third fetch was rejected");
  const auto fetch3_payload = scratchbird::server::DecodeFetchResultForTest(fetch3.payload);
  Require(fetch3_payload.has_value(), "third fetch payload malformed");
  Require(fetch3_payload->row_count == 1 && fetch3_payload->end_of_cursor,
          "third fetch did not return final chunk");
  Require(Contains(fetch3_payload->row_packet, "\"row_index\":4") &&
              Contains(fetch3_payload->row_packet, "\"end_of_stream\":true"),
          "third fetch packet missing final stream marker");

  std::array<std::uint8_t, 16> other_session_uuid{};
  AddSession(&registry, &other_session_uuid);
  const auto owned_cursor = OpenCursor(&registry, engine_state, session_uuid, 2);
  const auto cross_fetch = scratchbird::server::HandleFetch(
      &registry, FetchFrame(other_session_uuid, owned_cursor, 1));
  Require(!cross_fetch.accepted && HasDiagnostic(cross_fetch, "PARSER_SERVER_IPC.CURSOR_NOT_FOUND"),
          "cross-session fetch did not fail closed");
  const auto cross_close = scratchbird::server::HandleCloseCursor(
      &registry, CloseFrame(other_session_uuid, owned_cursor));
  Require(!cross_close.accepted && HasDiagnostic(cross_close, "PARSER_SERVER_IPC.CURSOR_NOT_FOUND"),
          "cross-session close did not fail closed");
  const auto owner_close = scratchbird::server::HandleCloseCursor(
      &registry, CloseFrame(session_uuid, owned_cursor));
  Require(owner_close.accepted, "owner close rejected after cross-session refusal");

  const auto too_large_cursor = OpenCursor(&registry, engine_state, session_uuid, 6);
  const auto too_large = scratchbird::server::HandleFetch(
      &registry, FetchFrame(session_uuid, too_large_cursor, 1024));
  Require(!too_large.accepted && HasDiagnostic(too_large, "SERVER.STREAM.CHUNK_TOO_LARGE"),
          "oversized fetch chunk did not fail closed");

  const auto close_cursor = OpenCursor(&registry, engine_state, session_uuid, 2);
  const auto close = scratchbird::server::HandleCloseCursor(&registry, CloseFrame(session_uuid, close_cursor));
  Require(close.accepted, "close cursor rejected");
  const auto fetch_closed = scratchbird::server::HandleFetch(
      &registry, FetchFrame(session_uuid, close_cursor, 1));
  Require(!fetch_closed.accepted && HasDiagnostic(fetch_closed, "PARSER_SERVER_IPC.CURSOR_NOT_FOUND"),
          "fetch after close did not fail closed");

  const auto disconnect_cursor = OpenCursor(&registry, engine_state, session_uuid, 2);
  const auto disconnect = scratchbird::server::HandleDisconnectNotice(&registry, DisconnectFrame(session_uuid));
  Require(disconnect.accepted, "disconnect notice did not detach session");
  const auto fetch_after_disconnect = scratchbird::server::HandleFetch(
      &registry, FetchFrame(session_uuid, disconnect_cursor, 1));
  Require(!fetch_after_disconnect.accepted &&
              HasDiagnostic(fetch_after_disconnect, "PARSER_SERVER_IPC.CURSOR_NOT_FOUND"),
          "fetch after disconnect did not close active cursor");

  std::cout << "sb_streaming_result_protocol_conformance=passed\n";
  return EXIT_SUCCESS;
}
