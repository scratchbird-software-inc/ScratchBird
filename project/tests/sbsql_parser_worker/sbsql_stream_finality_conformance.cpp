// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "sblr_dispatch_server.hpp"
#include "session_registry.hpp"
#include "database_lifecycle.hpp"
#include "transaction/transaction_api.hpp"
#include "uuid.hpp"

#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>

namespace {

namespace sbps = scratchbird::server::sbps;
namespace api = scratchbird::engine::internal_api;
namespace database = scratchbird::storage::database;
namespace platform = scratchbird::core::platform;
namespace uuid = scratchbird::core::uuid;

constexpr std::uint32_t kCursorCloseFlagCancel = 1u << 0;
constexpr std::uint32_t kSchemaExecuteSblrV1 = 4003;

void Require(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(EXIT_FAILURE);
  }
}

bool Contains(std::string_view haystack, std::string_view needle) {
  return haystack.find(needle) != std::string_view::npos;
}

template <typename TResult>
void RequireEngineOk(const TResult& result, std::string_view message) {
  if (result.ok) return;
  for (const auto& diagnostic : result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
  }
  Require(false, message);
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

struct EngineFixture {
  std::filesystem::path directory;
  std::filesystem::path database_path;
  std::string database_uuid;

  EngineFixture() = default;
  EngineFixture(const EngineFixture&) = delete;
  EngineFixture& operator=(const EngineFixture&) = delete;
  EngineFixture(EngineFixture&& other) noexcept
      : directory(std::move(other.directory)),
        database_path(std::move(other.database_path)),
        database_uuid(std::move(other.database_uuid)) {
    other.directory.clear();
  }

  ~EngineFixture() {
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
  }
};

struct RouteBinding {
  std::array<std::uint8_t, 16> connection_uuid{};
  std::array<std::uint8_t, 16> session_uuid{};
};

EngineFixture MakeEngineFixture() {
  EngineFixture fixture;
  const auto now_millis = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
  fixture.directory = std::filesystem::temp_directory_path() /
                      ("sb_stream_finality_conformance_" +
                       scratchbird::server::UuidBytesToText(
                           sbps::MakeUuidV7Bytes()));
  std::error_code directory_error;
  std::filesystem::create_directories(fixture.directory, directory_error);
  Require(!directory_error, "stream finality fixture directory creation failed");
  fixture.database_path = fixture.directory / "stream_finality.sbdb";

  const auto database_uuid = uuid::GenerateEngineIdentityV7(
      platform::UuidKind::database, now_millis);
  const auto filespace_uuid = uuid::GenerateEngineIdentityV7(
      platform::UuidKind::filespace, now_millis + 1);
  Require(database_uuid.ok() && filespace_uuid.ok(),
          "stream finality fixture UUID generation failed");

  database::DatabaseCreateConfig create;
  create.path = fixture.database_path.string();
  create.database_uuid = database_uuid.value;
  create.filespace_uuid = filespace_uuid.value;
  create.creation_unix_epoch_millis = now_millis;
  create.page_size = 8192;
  create.require_resource_seed_pack = false;
  create.allow_minimal_resource_bootstrap = true;
  const auto created = database::CreateDatabaseFile(create);
  Require(created.ok(), "stream finality fixture database creation failed");
  fixture.database_uuid = uuid::UuidToString(database_uuid.value.value);
  return fixture;
}

scratchbird::server::HostedEngineState MakeEngineState(
    const EngineFixture& fixture) {
  scratchbird::server::HostedEngineState state;
  state.engine_context_active = true;
  scratchbird::server::HostedDatabaseSnapshot database;
  database.state = scratchbird::server::HostedDatabaseState::kOpen;
  database.database_open = true;
  database.database_path = fixture.database_path.string();
  database.database_uuid = fixture.database_uuid;
  state.databases.push_back(database);
  return state;
}

scratchbird::server::ServerSessionRegistry MakeRegistry(
    const EngineFixture& fixture,
    RouteBinding* route,
    api::EngineRequestContext* transaction_context) {
  Require(route != nullptr && transaction_context != nullptr,
          "stream finality route and transaction context are required");
  scratchbird::server::ServerSessionRecord session;
  session.session_uuid = sbps::MakeUuidV7Bytes();
  session.connection_uuid = sbps::MakeUuidV7Bytes();
  session.auth_context_uuid = sbps::MakeUuidV7Bytes();
  session.principal_uuid = sbps::MakeUuidV7Bytes();
  session.effective_user_uuid = session.principal_uuid;
  session.database_path = fixture.database_path.string();
  session.database_uuid = fixture.database_uuid;
  route->connection_uuid = session.connection_uuid;
  route->session_uuid = session.session_uuid;

  api::EngineBeginTransactionRequest begin;
  begin.context.trust_mode = api::EngineTrustMode::server_isolated;
  begin.context.request_id = "stream-finality-engine-begin";
  begin.context.database_path = fixture.database_path.string();
  begin.context.database_uuid.canonical = fixture.database_uuid;
  begin.context.principal_uuid.canonical =
      scratchbird::server::UuidBytesToText(session.principal_uuid);
  begin.context.session_uuid.canonical =
      scratchbird::server::UuidBytesToText(session.session_uuid);
  begin.context.security_context_present = true;
  begin.context.catalog_generation_id = 1;
  begin.context.security_epoch = 1;
  begin.context.resource_epoch = 1;
  begin.context.name_resolution_epoch = 1;
  begin.isolation_level = "read_committed";
  const auto begun = api::EngineBeginTransaction(begin);
  RequireEngineOk(begun, "stream finality engine transaction begin failed");
  Require(scratchbird::server::IsCompleteEngineTransactionIdentity(
              begun.local_transaction_id, begun.transaction_uuid.canonical),
          "stream finality begin did not return a composite transaction identity");

  session.local_transaction_id = begun.local_transaction_id;
  session.snapshot_visible_through_local_transaction_id =
      begun.snapshot_visible_through_local_transaction_id;
  session.transaction_uuid = begun.transaction_uuid.canonical;
  *transaction_context = begin.context;
  transaction_context->local_transaction_id = begun.local_transaction_id;
  transaction_context->snapshot_visible_through_local_transaction_id =
      begun.snapshot_visible_through_local_transaction_id;
  transaction_context->transaction_uuid = begun.transaction_uuid;

  scratchbird::server::ServerSessionRegistry registry;
  registry.sessions_by_uuid[scratchbird::server::UuidBytesToText(session.session_uuid)] = session;
  registry.channel_state = scratchbird::server::ServerChannelState::kReady;
  return registry;
}

sbps::Frame ExecuteFrame(const RouteBinding& route,
                         const std::string& encoded) {
  sbps::Frame frame;
  frame.header.message_type = static_cast<std::uint16_t>(sbps::MessageType::kExecuteSblr);
  frame.header.payload_schema_id = kSchemaExecuteSblrV1;
  frame.header.request_uuid = sbps::MakeUuidV7Bytes();
  frame.header.connection_uuid = route.connection_uuid;
  frame.header.session_uuid = route.session_uuid;
  frame.payload = scratchbird::server::EncodeExecuteSblrPayloadForTest(
      route.session_uuid, {}, encoded, true);
  return frame;
}

sbps::Frame FetchFrame(const RouteBinding& route,
                       const std::array<std::uint8_t, 16>& cursor_uuid,
                       std::uint64_t max_rows) {
  sbps::Frame frame;
  frame.header.message_type = static_cast<std::uint16_t>(sbps::MessageType::kFetch);
  frame.header.request_uuid = sbps::MakeUuidV7Bytes();
  frame.header.connection_uuid = route.connection_uuid;
  frame.header.session_uuid = route.session_uuid;
  frame.payload = scratchbird::server::EncodeFetchPayloadForTest(
      route.session_uuid, cursor_uuid, max_rows);
  return frame;
}

sbps::Frame CloseFrame(const RouteBinding& route,
                       const std::array<std::uint8_t, 16>& cursor_uuid,
                       std::uint32_t flags = 0) {
  sbps::Frame frame;
  frame.header.message_type = static_cast<std::uint16_t>(sbps::MessageType::kCloseCursor);
  frame.header.request_uuid = sbps::MakeUuidV7Bytes();
  frame.header.connection_uuid = route.connection_uuid;
  frame.header.session_uuid = route.session_uuid;
  frame.payload = scratchbird::server::EncodeFetchPayloadForTest(
      route.session_uuid, cursor_uuid, 1, 0, flags);
  return frame;
}

sbps::Frame DisconnectFrame(const RouteBinding& route,
                            std::string_view reason) {
  sbps::Frame frame;
  frame.header.message_type = static_cast<std::uint16_t>(sbps::MessageType::kDisconnectNotice);
  frame.header.request_uuid = sbps::MakeUuidV7Bytes();
  frame.header.connection_uuid = route.connection_uuid;
  frame.header.session_uuid = route.session_uuid;
  PutUuid(&frame.payload, route.session_uuid);
  PutString(&frame.payload, reason);
  return frame;
}

std::array<std::uint8_t, 16> OpenCursor(scratchbird::server::ServerSessionRegistry* registry,
                                        const scratchbird::server::HostedEngineState& engine_state,
                                        const RouteBinding& route,
                                        std::string_view mode,
                                        std::uint64_t after_fetches) {
  const auto execute = scratchbird::server::HandleExecuteSblr(
      registry, engine_state, ExecuteFrame(route, FinalityEnvelope(mode, 2, after_fetches)));
  if (!execute.accepted) {
    for (const auto& diagnostic : execute.diagnostics) {
      std::cerr << diagnostic.code << ':' << diagnostic.safe_message << '\n';
    }
  }
  Require(execute.accepted, "stream finality execute was rejected");
  const auto cursor_uuid = scratchbird::server::DecodeCursorUuidForTest(execute.payload);
  Require(cursor_uuid.has_value(), "stream finality execute did not return cursor UUID");
  return *cursor_uuid;
}

}  // namespace

int main() {
  const auto fixture = MakeEngineFixture();
  const auto engine_state = MakeEngineState(fixture);
  RouteBinding route;
  api::EngineRequestContext transaction_context;
  auto registry = MakeRegistry(fixture,
                               &route,
                               &transaction_context);

  const auto timeout_cursor = OpenCursor(&registry, engine_state, route, "timeout", 1);
  const auto timeout_fetch1 = scratchbird::server::HandleFetch(
      &registry, FetchFrame(route, timeout_cursor, 1));
  const auto timeout_payload1 = scratchbird::server::DecodeFetchResultForTest(timeout_fetch1.payload);
  Require(timeout_fetch1.accepted && timeout_payload1.has_value() &&
              timeout_payload1->row_count == 1 && !timeout_payload1->end_of_cursor,
          "timeout stream first fetch did not deliver initial row");
  const auto timeout_fetch2 = scratchbird::server::HandleFetch(
      &registry, FetchFrame(route, timeout_cursor, 1));
  Require(!timeout_fetch2.accepted && !timeout_fetch2.diagnostics.empty() &&
              timeout_fetch2.diagnostics.front().code == "SERVER.STREAM.TIMEOUT",
          "timeout stream did not fail closed with deterministic timeout diagnostic");
  const auto timeout_it = registry.cursors_by_uuid.find(scratchbird::server::UuidBytesToText(timeout_cursor));
  Require(timeout_it != registry.cursors_by_uuid.end() &&
              timeout_it->second.closed &&
              timeout_it->second.finality_state == "timed_out",
          "timeout stream cursor finality was not recorded");

  const auto drain_cursor = OpenCursor(&registry, engine_state, route, "drain", 0);
  registry.sessions_by_uuid
      .at(scratchbird::server::UuidBytesToText(route.session_uuid))
      .channel_state = scratchbird::server::ServerChannelState::kDraining;
  const auto drain_fetch = scratchbird::server::HandleFetch(
      &registry, FetchFrame(route, drain_cursor, 1));
  const auto drain_payload = scratchbird::server::DecodeFetchResultForTest(drain_fetch.payload);
  Require(drain_fetch.accepted && drain_payload.has_value() &&
              drain_payload->row_count == 1 && drain_payload->end_of_cursor,
          "drain stream did not return accepted deterministic finality");
  Require(Contains(drain_payload->row_packet, "\"stream_finality\"") &&
              Contains(drain_payload->row_packet, "\"state\":\"drained\"") &&
              Contains(drain_payload->detail, "\"state\":\"drained\""),
          "drain stream finality packet or metadata is missing");
  registry.sessions_by_uuid
      .at(scratchbird::server::UuidBytesToText(route.session_uuid))
      .channel_state = scratchbird::server::ServerChannelState::kReady;

  const auto cancel_cursor = OpenCursor(&registry, engine_state, route, "cancel", 0);
  const auto cancel_close = scratchbird::server::HandleCloseCursor(
      &registry, CloseFrame(route, cancel_cursor, kCursorCloseFlagCancel));
  const auto cancel_it = registry.cursors_by_uuid.find(scratchbird::server::UuidBytesToText(cancel_cursor));
  Require(cancel_close.accepted && cancel_it != registry.cursors_by_uuid.end() &&
              cancel_it->second.closed &&
              cancel_it->second.finality_state == "cancelled",
          "cancel stream did not record cancelled finality");

  const auto killed_cursor = OpenCursor(&registry, engine_state, route, "cancel", 0);
  const auto disconnect = scratchbird::server::HandleDisconnectNotice(
      &registry, DisconnectFrame(route, "parser_killed"));
  const auto killed_it = registry.cursors_by_uuid.find(scratchbird::server::UuidBytesToText(killed_cursor));
  Require(disconnect.accepted && killed_it != registry.cursors_by_uuid.end() &&
              killed_it->second.closed &&
              killed_it->second.finality_state == "parser_killed",
          "parser kill disconnect did not close cursor with parser_killed finality");

  api::EngineRollbackTransactionRequest recovery_rollback;
  recovery_rollback.context = transaction_context;
  const auto recovered = api::EngineRollbackTransaction(recovery_rollback);
  Require(recovered.engine_finality_known &&
              (recovered.rollback_finality_state ==
                   "rolled_back_by_engine_inventory" ||
               recovered.rollback_finality_state ==
                   "rolled_back_post_inventory_secondary_failure") &&
              recovered.local_transaction_id ==
                  transaction_context.local_transaction_id &&
              recovered.transaction_uuid.canonical ==
                  transaction_context.transaction_uuid.canonical,
          "parser kill transaction was not resolved by exact engine MGA identity");

  std::cout << "sbsql_stream_finality_conformance=passed\n";
  return EXIT_SUCCESS;
}
