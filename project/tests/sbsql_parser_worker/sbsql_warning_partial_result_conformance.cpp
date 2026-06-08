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

void Require(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(EXIT_FAILURE);
  }
}

bool Contains(std::string_view haystack, std::string_view needle) {
  return haystack.find(needle) != std::string_view::npos;
}

std::string WarningStreamEnvelope(std::uint64_t partial_rows,
                                  std::uint64_t warnings) {
  std::string out = "{\"envelope\":\"SBLRExecutionEnvelope.v3\",";
  out += "\"operation_family\":\"sblr.query.relational.v3\",";
  out += "\"surface_key\":\"fspe010b7.warning_partial\",";
  out += "\"sblr_operation_key\":\"op.fspe010b7.warning_partial\",";
  out += "\"result_shape\":\"rs.fspe010b7.warning_partial.v1\",";
  out += "\"diagnostic_shape\":\"diag.fspe010b7.v1\",";
  out += "\"resource_contract\":\"resource.fspe010b7.v1\",";
  out += "\"trace_key\":\"FSPE-010B7\",";
  out += "\"source_payload_embedded\":false,";
  out += "\"resolved_object_uuids\":[\"019e05df-f010-7000-8000-000000000077\"],";
  out += "\"descriptor_refs\":[\"descriptor.partial_result.warning_chain\"],";
  out += "\"policy_refs\":[\"policy.partial_result.forward_only\"],";
  out += "\"partial_result_rows\":";
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
  database.database_path = "/tmp/sb_warning_partial_result_conformance.sbdb";
  database.database_uuid = "019e05df-f010-7000-8000-000000000017";
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
  session.database_path = "/tmp/sb_warning_partial_result_conformance.sbdb";
  session.database_uuid = "019e05df-f010-7000-8000-000000000017";
  *session_uuid = session.session_uuid;
  scratchbird::server::ServerSessionRegistry registry;
  registry.sessions_by_uuid[scratchbird::server::UuidBytesToText(session.session_uuid)] = session;
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

}  // namespace

int main() {
  std::array<std::uint8_t, 16> session_uuid{};
  auto registry = MakeRegistry(&session_uuid);
  const auto engine_state = MakeEngineState();

  const auto execute = scratchbird::server::HandleExecuteSblr(
      &registry, engine_state, ExecuteFrame(session_uuid, WarningStreamEnvelope(3, 2)));
  Require(execute.accepted, "warning/partial-result execute was rejected");
  const auto cursor_uuid = scratchbird::server::DecodeCursorUuidForTest(execute.payload);
  Require(cursor_uuid.has_value(), "warning/partial-result execute did not return cursor UUID");

  const auto fetch1 = scratchbird::server::HandleFetch(&registry, FetchFrame(session_uuid, *cursor_uuid, 3));
  const auto payload1 = scratchbird::server::DecodeFetchResultForTest(fetch1.payload);
  Require(fetch1.accepted && payload1.has_value() && payload1->row_count == 3 && !payload1->end_of_cursor,
          "first warning stream fetch did not return partial rows");
  Require(Contains(payload1->row_packet, "\"event\":\"partial_result_row\"") &&
              Contains(payload1->row_packet, "\"row_index\":0") &&
              Contains(payload1->row_packet, "\"partial_result\":true"),
          "first warning stream fetch missing partial-result rows");

  const auto fetch2 = scratchbird::server::HandleFetch(&registry, FetchFrame(session_uuid, *cursor_uuid, 2));
  const auto payload2 = scratchbird::server::DecodeFetchResultForTest(fetch2.payload);
  Require(fetch2.accepted && payload2.has_value() && payload2->row_count == 2 && !payload2->end_of_cursor,
          "second warning stream fetch did not return warning chain");
  Require(Contains(payload2->row_packet, "\"event\":\"warning\"") &&
              Contains(payload2->row_packet, "\"diagnostic_code\":\"STREAM.WARNING.0\"") &&
              Contains(payload2->row_packet, "\"does_not_abort\":true"),
          "second warning stream fetch missing non-aborting warning chain");

  const auto fetch3 = scratchbird::server::HandleFetch(&registry, FetchFrame(session_uuid, *cursor_uuid, 1));
  const auto payload3 = scratchbird::server::DecodeFetchResultForTest(fetch3.payload);
  Require(fetch3.accepted && payload3.has_value() && payload3->row_count == 1 && payload3->end_of_cursor,
          "final warning stream fetch did not return finality");
  Require(Contains(payload3->row_packet, "\"event\":\"partial_result_finality\"") &&
              Contains(payload3->row_packet, "\"status\":\"completed_with_warnings\"") &&
              Contains(payload3->detail, "\"end_of_cursor\":true"),
          "final warning stream fetch missing warning finality or cursor metadata");

  std::cout << "sbsql_warning_partial_result_conformance=passed\n";
  return EXIT_SUCCESS;
}
