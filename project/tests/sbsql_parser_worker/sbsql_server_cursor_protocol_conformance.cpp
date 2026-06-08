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
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

namespace {

using scratchbird::server::HostedDatabaseSnapshot;
using scratchbird::server::HostedDatabaseState;
using scratchbird::server::HostedEngineState;
using scratchbird::server::ServerDiagnostic;
using scratchbird::server::ServerSessionRecord;
using scratchbird::server::ServerSessionRegistry;
using scratchbird::server::SessionOperationResult;
namespace sbps = scratchbird::server::sbps;

constexpr std::uint32_t kFetchFlagScroll = 1;

void Require(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(EXIT_FAILURE);
  }
}

bool HasDiagnostic(const SessionOperationResult& result, std::string_view code) {
  for (const ServerDiagnostic& diagnostic : result.diagnostics) {
    if (diagnostic.code == code) return true;
  }
  return false;
}

bool Contains(std::string_view haystack, std::string_view needle) {
  return haystack.find(needle) != std::string_view::npos;
}

std::string ParserJsonEnvelope(std::uint64_t stream_rows) {
  std::string out = "{\"envelope\":\"SBLRExecutionEnvelope.v3\",";
  out += "\"operation_family\":\"sblr.query.relational.v3\",";
  out += "\"surface_key\":\"fspe010b2.fixture\",";
  out += "\"sblr_operation_key\":\"op.fspe010b2.fixture\",";
  out += "\"result_shape\":\"rs.fspe010b2.stream.v1\",";
  out += "\"diagnostic_shape\":\"diag.fspe010b2.v1\",";
  out += "\"resource_contract\":\"resource.fspe010b2.v1\",";
  out += "\"trace_key\":\"FSPE-010B2\",";
  out += "\"stream_row_count\":";
  out += std::to_string(stream_rows);
  out += ",\"source_payload_embedded\":false,";
  out += "\"resolved_object_uuids\":[],\"descriptor_refs\":[],\"policy_refs\":[]}";
  return out;
}

sbps::Frame ExecuteFrame(const std::array<std::uint8_t, 16>& session_uuid, std::uint64_t rows) {
  sbps::Frame frame;
  frame.header.message_type = static_cast<std::uint16_t>(sbps::MessageType::kExecuteSblr);
  frame.header.request_uuid = sbps::MakeUuidV7Bytes();
  frame.header.session_uuid = session_uuid;
  frame.payload = scratchbird::server::EncodeExecuteSblrPayloadForTest(
      session_uuid, {}, ParserJsonEnvelope(rows), true);
  return frame;
}

sbps::Frame FetchFrame(const std::array<std::uint8_t, 16>& session_uuid,
                       const std::array<std::uint8_t, 16>& cursor_uuid,
                       std::uint64_t max_rows,
                       std::uint64_t max_bytes = 0,
                       std::uint32_t fetch_flags = 0) {
  sbps::Frame frame;
  frame.header.message_type = static_cast<std::uint16_t>(sbps::MessageType::kFetch);
  frame.header.request_uuid = sbps::MakeUuidV7Bytes();
  frame.header.session_uuid = session_uuid;
  frame.payload = scratchbird::server::EncodeFetchPayloadForTest(
      session_uuid, cursor_uuid, max_rows, max_bytes, fetch_flags);
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

ServerSessionRegistry MakeRegistry(std::array<std::uint8_t, 16>* session_uuid) {
  ServerSessionRegistry registry;
  ServerSessionRecord session;
  session.session_uuid = sbps::MakeUuidV7Bytes();
  session.auth_context_uuid = sbps::MakeUuidV7Bytes();
  session.principal_uuid = sbps::MakeUuidV7Bytes();
  session.effective_user_uuid = session.principal_uuid;
  session.database_path = "/tmp/sb_server_cursor_protocol_conformance.sbdb";
  session.database_uuid = "019e05df-f010-7000-8000-0000000000b2";
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
  database.database_path = "/tmp/sb_server_cursor_protocol_conformance.sbdb";
  database.database_uuid = "019e05df-f010-7000-8000-0000000000b2";
  state.databases.push_back(database);
  return state;
}

std::array<std::uint8_t, 16> OpenCursor(ServerSessionRegistry* registry,
                                        const HostedEngineState& engine_state,
                                        const std::array<std::uint8_t, 16>& session_uuid,
                                        std::uint64_t rows) {
  const auto execute = scratchbird::server::HandleExecuteSblr(
      registry, engine_state, ExecuteFrame(session_uuid, rows));
  Require(execute.accepted, "cursor execute rejected");
  const auto cursor_uuid = scratchbird::server::DecodeCursorUuidForTest(execute.payload);
  Require(cursor_uuid.has_value(), "execute did not return cursor UUID");
  return *cursor_uuid;
}

}  // namespace

int main() {
  std::array<std::uint8_t, 16> session_uuid{};
  auto registry = MakeRegistry(&session_uuid);
  const auto engine_state = MakeEngineState();

  const auto cursor_uuid = OpenCursor(&registry, engine_state, session_uuid, 3);
  const auto fetch1 = scratchbird::server::HandleFetch(
      &registry, FetchFrame(session_uuid, cursor_uuid, 2, 4096));
  Require(fetch1.accepted, "metadata fetch rejected");
  const auto fetch1_payload = scratchbird::server::DecodeFetchResultForTest(fetch1.payload);
  Require(fetch1_payload.has_value(), "metadata fetch payload malformed");
  Require(fetch1_payload->row_count == 2 && !fetch1_payload->end_of_cursor,
          "metadata fetch did not return first bounded chunk");
  Require(Contains(fetch1_payload->detail, "\"fetch_count\":1") &&
              Contains(fetch1_payload->detail, "\"next_row_index\":2") &&
              Contains(fetch1_payload->detail, "\"total_rows\":3") &&
              Contains(fetch1_payload->detail, "\"capability\":\"forward_only\""),
          "metadata fetch did not include cursor metadata");

  const auto too_small = scratchbird::server::HandleFetch(
      &registry, FetchFrame(session_uuid, cursor_uuid, 1, 8));
  Require(!too_small.accepted && HasDiagnostic(too_small, "SERVER.STREAM.BYTES_TOO_SMALL"),
          "too-small max_bytes did not fail before advancing cursor");

  const auto fetch2 = scratchbird::server::HandleFetch(
      &registry, FetchFrame(session_uuid, cursor_uuid, 1, 4096));
  Require(fetch2.accepted, "final fetch rejected after too-small byte limit");
  const auto fetch2_payload = scratchbird::server::DecodeFetchResultForTest(fetch2.payload);
  Require(fetch2_payload.has_value() && fetch2_payload->row_count == 1 && fetch2_payload->end_of_cursor,
          "final fetch did not preserve cursor position after byte-limit refusal");
  Require(Contains(fetch2_payload->detail, "\"fetch_count\":2") &&
              Contains(fetch2_payload->detail, "\"end_of_cursor\":true"),
          "final fetch metadata missing exhausted state");

  const auto eos = scratchbird::server::HandleFetch(&registry, FetchFrame(session_uuid, cursor_uuid, 1, 4096));
  const auto eos_payload = scratchbird::server::DecodeFetchResultForTest(eos.payload);
  Require(eos.accepted && eos_payload.has_value() && eos_payload->row_count == 0 && eos_payload->end_of_cursor,
          "post-EOS fetch did not return deterministic empty EOS");

  const auto bytes_cursor = OpenCursor(&registry, engine_state, session_uuid, 1);
  const auto too_large_bytes = scratchbird::server::HandleFetch(
      &registry, FetchFrame(session_uuid, bytes_cursor, 1, 65537));
  Require(!too_large_bytes.accepted && HasDiagnostic(too_large_bytes, "SERVER.STREAM.BYTES_TOO_LARGE"),
          "oversized max_bytes did not fail closed");

  const auto scroll_cursor = OpenCursor(&registry, engine_state, session_uuid, 1);
  const auto scroll = scratchbird::server::HandleFetch(
      &registry, FetchFrame(session_uuid, scroll_cursor, 1, 4096, kFetchFlagScroll));
  Require(!scroll.accepted && HasDiagnostic(scroll, "SERVER.CURSOR.SCROLL_UNSUPPORTED"),
          "unsupported cursor scroll flag did not fail closed");

  const auto rows_cursor = OpenCursor(&registry, engine_state, session_uuid, 1);
  const auto too_many_rows = scratchbird::server::HandleFetch(
      &registry, FetchFrame(session_uuid, rows_cursor, 5, 4096));
  Require(!too_many_rows.accepted && HasDiagnostic(too_many_rows, "SERVER.STREAM.CHUNK_TOO_LARGE"),
          "oversized max_rows did not fail closed");

  const auto close = scratchbird::server::HandleCloseCursor(&registry, CloseFrame(session_uuid, cursor_uuid));
  Require(close.accepted, "close rejected");

  std::cout << "sb_server_cursor_protocol_conformance=passed\n";
  return EXIT_SUCCESS;
}
