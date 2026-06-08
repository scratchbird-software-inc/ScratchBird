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

namespace sbps = scratchbird::server::sbps;

constexpr std::uint32_t kCursorCloseFlagCancel = 1u << 0;

void Require(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(EXIT_FAILURE);
  }
}

bool Contains(std::string_view haystack, std::string_view needle) {
  return haystack.find(needle) != std::string_view::npos;
}

void PutU16(std::vector<std::uint8_t>* out, std::uint16_t value) {
  out->push_back(static_cast<std::uint8_t>(value & 0xffu));
  out->push_back(static_cast<std::uint8_t>((value >> 8u) & 0xffu));
}

void PutUuid(std::vector<std::uint8_t>* out, const std::array<std::uint8_t, 16>& uuid) {
  out->insert(out->end(), uuid.begin(), uuid.end());
}

void PutString(std::vector<std::uint8_t>* out, std::string_view value) {
  PutU16(out, static_cast<std::uint16_t>(value.size()));
  out->insert(out->end(), value.begin(), value.end());
}

std::string FinalityEnvelope(std::string_view mode,
                             std::uint64_t rows,
                             std::uint64_t after_fetches) {
  std::string out = "{\"envelope\":\"SBLRExecutionEnvelope.v3\",";
  out += "\"operation_family\":\"sblr.query.relational.v3\",";
  out += "\"surface_key\":\"fspe010b8.stream_finality\",";
  out += "\"sblr_operation_key\":\"op.fspe010b8.stream_finality\",";
  out += "\"result_shape\":\"rs.fspe010b8.stream_finality.v1\",";
  out += "\"diagnostic_shape\":\"diag.fspe010b8.v1\",";
  out += "\"resource_contract\":\"resource.fspe010b8.v1\",";
  out += "\"trace_key\":\"FSPE-010B8\",";
  out += "\"source_payload_embedded\":false,";
  out += "\"resolved_object_uuids\":[\"019e05df-f010-7000-8000-000000000088\"],";
  out += "\"descriptor_refs\":[\"descriptor.stream.finality\"],";
  out += "\"policy_refs\":[\"policy.stream.finality.forward_only\"],";
  out += "\"stream_row_count\":";
  out += std::to_string(rows);
  out += ",\"stream_finality_mode\":\"";
  out += mode;
  out += "\",\"stream_finality_after_fetches\":";
  out += std::to_string(after_fetches);
  out += "}";
  return out;
}

scratchbird::server::HostedEngineState MakeEngineState() {
  scratchbird::server::HostedEngineState state;
  state.engine_context_active = true;
  scratchbird::server::HostedDatabaseSnapshot database;
  database.state = scratchbird::server::HostedDatabaseState::kOpen;
  database.database_open = true;
  database.database_path = "/tmp/sb_stream_finality_conformance.sbdb";
  database.database_uuid = "019e05df-f010-7000-8000-000000000018";
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
  session.database_path = "/tmp/sb_stream_finality_conformance.sbdb";
  session.database_uuid = "019e05df-f010-7000-8000-000000000018";
  *session_uuid = session.session_uuid;
  scratchbird::server::ServerSessionRegistry registry;
  registry.sessions_by_uuid[scratchbird::server::UuidBytesToText(session.session_uuid)] = session;
  registry.channel_state = scratchbird::server::ServerChannelState::kReady;
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
                       std::uint64_t max_rows) {
  sbps::Frame frame;
  frame.header.message_type = static_cast<std::uint16_t>(sbps::MessageType::kFetch);
  frame.header.request_uuid = sbps::MakeUuidV7Bytes();
  frame.header.session_uuid = session_uuid;
  frame.payload = scratchbird::server::EncodeFetchPayloadForTest(session_uuid, cursor_uuid, max_rows);
  return frame;
}

sbps::Frame CloseFrame(const std::array<std::uint8_t, 16>& session_uuid,
                       const std::array<std::uint8_t, 16>& cursor_uuid,
                       std::uint32_t flags = 0) {
  sbps::Frame frame;
  frame.header.message_type = static_cast<std::uint16_t>(sbps::MessageType::kCloseCursor);
  frame.header.request_uuid = sbps::MakeUuidV7Bytes();
  frame.header.session_uuid = session_uuid;
  frame.payload = scratchbird::server::EncodeFetchPayloadForTest(session_uuid, cursor_uuid, 1, 0, flags);
  return frame;
}

sbps::Frame DisconnectFrame(const std::array<std::uint8_t, 16>& session_uuid,
                            std::string_view reason) {
  sbps::Frame frame;
  frame.header.message_type = static_cast<std::uint16_t>(sbps::MessageType::kDisconnectNotice);
  frame.header.request_uuid = sbps::MakeUuidV7Bytes();
  frame.header.session_uuid = session_uuid;
  PutUuid(&frame.payload, session_uuid);
  PutString(&frame.payload, reason);
  return frame;
}

std::array<std::uint8_t, 16> OpenCursor(scratchbird::server::ServerSessionRegistry* registry,
                                        const scratchbird::server::HostedEngineState& engine_state,
                                        const std::array<std::uint8_t, 16>& session_uuid,
                                        std::string_view mode,
                                        std::uint64_t after_fetches) {
  const auto execute = scratchbird::server::HandleExecuteSblr(
      registry, engine_state, ExecuteFrame(session_uuid, FinalityEnvelope(mode, 2, after_fetches)));
  Require(execute.accepted, "stream finality execute was rejected");
  const auto cursor_uuid = scratchbird::server::DecodeCursorUuidForTest(execute.payload);
  Require(cursor_uuid.has_value(), "stream finality execute did not return cursor UUID");
  return *cursor_uuid;
}

}  // namespace

int main() {
  std::array<std::uint8_t, 16> session_uuid{};
  auto registry = MakeRegistry(&session_uuid);
  const auto engine_state = MakeEngineState();

  const auto timeout_cursor = OpenCursor(&registry, engine_state, session_uuid, "timeout", 1);
  const auto timeout_fetch1 = scratchbird::server::HandleFetch(
      &registry, FetchFrame(session_uuid, timeout_cursor, 1));
  const auto timeout_payload1 = scratchbird::server::DecodeFetchResultForTest(timeout_fetch1.payload);
  Require(timeout_fetch1.accepted && timeout_payload1.has_value() &&
              timeout_payload1->row_count == 1 && !timeout_payload1->end_of_cursor,
          "timeout stream first fetch did not deliver initial row");
  const auto timeout_fetch2 = scratchbird::server::HandleFetch(
      &registry, FetchFrame(session_uuid, timeout_cursor, 1));
  Require(!timeout_fetch2.accepted && !timeout_fetch2.diagnostics.empty() &&
              timeout_fetch2.diagnostics.front().code == "SERVER.STREAM.TIMEOUT",
          "timeout stream did not fail closed with deterministic timeout diagnostic");
  const auto timeout_it = registry.cursors_by_uuid.find(scratchbird::server::UuidBytesToText(timeout_cursor));
  Require(timeout_it != registry.cursors_by_uuid.end() &&
              timeout_it->second.closed &&
              timeout_it->second.finality_state == "timed_out",
          "timeout stream cursor finality was not recorded");

  const auto drain_cursor = OpenCursor(&registry, engine_state, session_uuid, "drain", 0);
  registry.channel_state = scratchbird::server::ServerChannelState::kDraining;
  const auto drain_fetch = scratchbird::server::HandleFetch(
      &registry, FetchFrame(session_uuid, drain_cursor, 1));
  const auto drain_payload = scratchbird::server::DecodeFetchResultForTest(drain_fetch.payload);
  Require(drain_fetch.accepted && drain_payload.has_value() &&
              drain_payload->row_count == 1 && drain_payload->end_of_cursor,
          "drain stream did not return accepted deterministic finality");
  Require(Contains(drain_payload->row_packet, "\"stream_finality\"") &&
              Contains(drain_payload->row_packet, "\"state\":\"drained\"") &&
              Contains(drain_payload->detail, "\"state\":\"drained\""),
          "drain stream finality packet or metadata is missing");
  registry.channel_state = scratchbird::server::ServerChannelState::kReady;

  const auto cancel_cursor = OpenCursor(&registry, engine_state, session_uuid, "cancel", 0);
  const auto cancel_close = scratchbird::server::HandleCloseCursor(
      &registry, CloseFrame(session_uuid, cancel_cursor, kCursorCloseFlagCancel));
  const auto cancel_it = registry.cursors_by_uuid.find(scratchbird::server::UuidBytesToText(cancel_cursor));
  Require(cancel_close.accepted && cancel_it != registry.cursors_by_uuid.end() &&
              cancel_it->second.closed &&
              cancel_it->second.finality_state == "cancelled",
          "cancel stream did not record cancelled finality");

  const auto killed_cursor = OpenCursor(&registry, engine_state, session_uuid, "cancel", 0);
  const auto disconnect = scratchbird::server::HandleDisconnectNotice(
      &registry, DisconnectFrame(session_uuid, "parser_killed"));
  const auto killed_it = registry.cursors_by_uuid.find(scratchbird::server::UuidBytesToText(killed_cursor));
  Require(disconnect.accepted && killed_it != registry.cursors_by_uuid.end() &&
              killed_it->second.closed &&
              killed_it->second.finality_state == "parser_killed",
          "parser kill disconnect did not close cursor with parser_killed finality");

  std::cout << "sbsql_stream_finality_conformance=passed\n";
  return EXIT_SUCCESS;
}
