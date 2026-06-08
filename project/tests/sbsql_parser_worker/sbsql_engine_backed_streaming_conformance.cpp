// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "scratchbird/engine/sblr/lowering.hpp"

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
using scratchbird::server::ServerSessionRecord;
using scratchbird::server::ServerSessionRegistry;
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

std::string TextOperationEnvelope(std::string_view operation_id, std::string_view opcode) {
  std::string out;
  out += "operation_id=";
  out += operation_id;
  out += "\n";
  out += "opcode=";
  out += opcode;
  out += "\n";
  out += "sblr_operation_family=sblr.observability.inspect.v3\n";
  out += "result_shape=engine.api.result.v1\n";
  out += "diagnostic_shape=engine.diagnostic.v1\n";
  out += "trace_key=FSPE-010B1\n";
  out += "contains_sql_text=false\n";
  out += "parser_resolved_names_to_uuids=true\n";
  out += "requires_security_context=true\n";
  out += "requires_transaction_context=false\n";
  out += "requires_cluster_authority=false\n";
  return out;
}

std::string BinaryOperationEnvelope(std::string_view operation_id, std::string_view opcode) {
  const auto text = TextOperationEnvelope(operation_id, opcode);
  const auto binary = scratchbird::engine::sblr::EnvelopeBuilder()
                          .operation(scratchbird::engine::SblrOperationFamily::management_inspect, 1)
                          .append_bytes(reinterpret_cast<const std::uint8_t*>(text.data()), text.size())
                          .encode();
  return std::string(reinterpret_cast<const char*>(binary.data()), binary.size());
}

sbps::Frame ExecuteFrame(const std::array<std::uint8_t, 16>& session_uuid,
                         const std::string& encoded,
                         bool cursor_requested) {
  sbps::Frame frame;
  frame.header.message_type = static_cast<std::uint16_t>(sbps::MessageType::kExecuteSblr);
  frame.header.request_uuid = sbps::MakeUuidV7Bytes();
  frame.header.session_uuid = session_uuid;
  frame.payload = scratchbird::server::EncodeExecuteSblrPayloadForTest(session_uuid, {}, encoded, cursor_requested);
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

ServerSessionRegistry MakeRegistry(std::array<std::uint8_t, 16>* session_uuid) {
  ServerSessionRegistry registry;
  ServerSessionRecord session;
  session.session_uuid = sbps::MakeUuidV7Bytes();
  session.auth_context_uuid = sbps::MakeUuidV7Bytes();
  session.principal_uuid = sbps::MakeUuidV7Bytes();
  session.effective_user_uuid = session.principal_uuid;
  session.database_path = "/tmp/sb_engine_backed_streaming_conformance.sbdb";
  session.database_uuid = "019e05df-f010-7000-8000-0000000000b1";
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
  database.database_path = "/tmp/sb_engine_backed_streaming_conformance.sbdb";
  database.database_uuid = "019e05df-f010-7000-8000-0000000000b1";
  state.databases.push_back(database);
  return state;
}

struct EngineBackedFixture {
  std::string_view operation_id;
  std::string_view opcode;
  std::string_view expected_field;
};

void VerifyEngineBackedCursor(ServerSessionRegistry* registry,
                              const HostedEngineState& engine_state,
                              const std::array<std::uint8_t, 16>& session_uuid,
                              const EngineBackedFixture& fixture) {
  const auto execute = scratchbird::server::HandleExecuteSblr(
      registry, engine_state, ExecuteFrame(session_uuid, BinaryOperationEnvelope(fixture.operation_id,
                                                                                 fixture.opcode), true));
  Require(execute.accepted, "engine-backed cursor execute was rejected");
  const auto cursor_uuid = scratchbird::server::DecodeCursorUuidForTest(execute.payload);
  Require(cursor_uuid.has_value(), "engine-backed execute did not return a cursor UUID");

  const auto fetch = scratchbird::server::HandleFetch(registry, FetchFrame(session_uuid, *cursor_uuid, 1));
  Require(fetch.accepted, "engine-backed fetch was rejected");
  const auto fetch_payload = scratchbird::server::DecodeFetchResultForTest(fetch.payload);
  Require(fetch_payload.has_value(), "engine-backed fetch payload malformed");
  Require(fetch_payload->row_count == 1 && fetch_payload->end_of_cursor,
          "engine-backed fetch did not return the row batch");
  Require(Contains(fetch_payload->row_packet, std::string("operation_id=") +
                                                std::string(fixture.operation_id)) &&
              Contains(fetch_payload->row_packet, fixture.expected_field) &&
              !Contains(fetch_payload->row_packet, "\"row_index\""),
          "engine-backed fetch did not expose the engine batch payload");

  const auto eos = scratchbird::server::HandleFetch(registry, FetchFrame(session_uuid, *cursor_uuid, 1));
  Require(eos.accepted, "engine-backed EOS fetch was rejected");
  const auto eos_payload = scratchbird::server::DecodeFetchResultForTest(eos.payload);
  Require(eos_payload.has_value() && eos_payload->row_count == 0 && eos_payload->end_of_cursor,
          "engine-backed cursor did not remain at EOS after final batch");

  const auto close = scratchbird::server::HandleCloseCursor(registry, CloseFrame(session_uuid, *cursor_uuid));
  Require(close.accepted, "engine-backed cursor close was rejected");
}

}  // namespace

int main() {
  std::array<std::uint8_t, 16> session_uuid{};
  auto registry = MakeRegistry(&session_uuid);
  const auto engine_state = MakeEngineState();

  constexpr std::array<EngineBackedFixture, 2> kFixtures{{
      {"observability.show_version",
       "SBLR_OBSERVABILITY_SHOW_VERSION",
       "product=ScratchBird"},
      {"observability.show_database",
       "SBLR_OBSERVABILITY_SHOW_DATABASE",
       "database_uuid="},
  }};
  for (const auto& fixture : kFixtures) {
    VerifyEngineBackedCursor(&registry, engine_state, session_uuid, fixture);
  }

  std::cout << "sb_engine_backed_streaming_conformance=passed\n";
  return EXIT_SUCCESS;
}
