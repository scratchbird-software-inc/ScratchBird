// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "database_lifecycle.hpp"
#include "local_transaction_store.hpp"
#include "manager_control.hpp"
#include "sblr_dispatch_server.hpp"
#include "session_registry.hpp"
#include "transaction_state.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

namespace {

namespace db = scratchbird::storage::database;
namespace tx = scratchbird::transaction::mga;
namespace uuid = scratchbird::core::uuid;
using scratchbird::core::platform::UuidKind;
using scratchbird::server::HostedDatabaseSnapshot;
using scratchbird::server::HostedDatabaseState;
using scratchbird::server::HostedEngineState;
using scratchbird::server::ServerBootstrapConfig;
using scratchbird::server::ServerLifecycleArtifacts;
using scratchbird::server::ServerMaintenanceCoordinator;
using scratchbird::server::ServerManagementContext;
using scratchbird::server::ServerManagementRequest;
using scratchbird::server::ServerRequestLifecycleState;
using scratchbird::server::ServerSessionRegistry;
using scratchbird::server::SessionOperationResult;
namespace sbps = scratchbird::server::sbps;

constexpr std::string_view kVerifier =
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";

struct AttachedSession {
  std::array<std::uint8_t, 16> connection_uuid{};
  std::array<std::uint8_t, 16> auth_context_uuid{};
  std::array<std::uint8_t, 16> session_uuid{};
};

void Require(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(EXIT_FAILURE);
  }
}

void PutU16(std::vector<std::uint8_t>* out, std::uint16_t value) {
  out->push_back(static_cast<std::uint8_t>(value & 0xffu));
  out->push_back(static_cast<std::uint8_t>((value >> 8u) & 0xffu));
}

void PutUuid(std::vector<std::uint8_t>* out, const std::array<std::uint8_t, 16>& uuid_bytes) {
  out->insert(out->end(), uuid_bytes.begin(), uuid_bytes.end());
}

void PutString(std::vector<std::uint8_t>* out, std::string_view value) {
  PutU16(out, static_cast<std::uint16_t>(value.size()));
  out->insert(out->end(), value.begin(), value.end());
}

bool Contains(std::string_view haystack, std::string_view needle) {
  return haystack.find(needle) != std::string_view::npos;
}

bool HasDiagnostic(const SessionOperationResult& result, std::string_view code) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.code == code) return true;
  }
  return false;
}

bool HasDiagnostic(const scratchbird::server::ServerManagementResponse& result,
                   std::string_view code) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.code == code) return true;
  }
  return false;
}

std::filesystem::path MakeTempDir() {
  std::string tmpl = "/tmp/sb_dblc013g_session_request_cursor.XXXXXX";
  std::vector<char> writable(tmpl.begin(), tmpl.end());
  writable.push_back('\0');
  char* made = ::mkdtemp(writable.data());
  Require(made != nullptr, "mkdtemp failed for DBLC-013G test");
  return std::filesystem::path(made);
}

std::string CreateOpenDatabase(const std::filesystem::path& path) {
  db::DatabaseCreateConfig create;
  create.path = path.string();
  create.database_uuid = uuid::GenerateEngineIdentityV7(UuidKind::database, 1780000001000).value;
  create.filespace_uuid = uuid::GenerateEngineIdentityV7(UuidKind::filespace, 1780000001001).value;
  create.page_size = 16384;
  create.creation_unix_epoch_millis = 1780000001002;
  create.allow_minimal_resource_bootstrap = true;
  create.require_resource_seed_pack = false;
  create.allow_overwrite = true;
  const auto created = db::CreateDatabaseFile(create);
  Require(created.ok(), "DBLC-013G database create failed");
  const auto opened = db::OpenDatabaseFile({path.string(), false, false, false});
  Require(opened.ok(), "DBLC-013G database first-open failed");
  const auto clean = db::MarkDatabaseCleanShutdown(path.string());
  Require(clean.ok(), "DBLC-013G clean shutdown marker failed");
  return uuid::UuidToString(create.database_uuid.value);
}

void WriteAuthStore(const std::filesystem::path& database_path) {
  std::ofstream out(database_path.string() + ".sb.local_password_auth", std::ios::trunc);
  out << "admin\tlocal_password\t" << kVerifier << '\n';
  out << "alice\tlocal_password\t" << kVerifier << '\n';
  Require(static_cast<bool>(out), "DBLC-013G auth store write failed");
}

HostedEngineState MakeEngineState(const std::filesystem::path& database_path,
                                  const std::string& database_uuid) {
  HostedEngineState engine_state;
  engine_state.engine_context_active = true;
  HostedDatabaseSnapshot database;
  database.state = HostedDatabaseState::kOpen;
  database.database_open = true;
  database.database_path = database_path.string();
  database.database_uuid = database_uuid;
  database.read_only = false;
  database.write_admission_fenced = false;
  engine_state.databases.push_back(std::move(database));
  return engine_state;
}

std::string Evidence(std::string_view principal) {
  std::string evidence = "scheme=local_password_v1;principal=";
  evidence += principal;
  evidence += ";verifier=";
  evidence += kVerifier;
  return evidence;
}

std::vector<std::uint8_t> AuthPayload(const std::array<std::uint8_t, 16>& connection_uuid,
                                      std::string_view principal) {
  std::vector<std::uint8_t> out;
  PutUuid(&out, connection_uuid);
  out.push_back(1);
  out.push_back(0);
  out.push_back(0);
  out.push_back(0);
  PutString(&out, "local_password");
  PutString(&out, principal);
  PutString(&out, "default");
  PutString(&out, "en");
  PutString(&out, Evidence(principal));
  return out;
}

std::vector<std::uint8_t> AttachPayload(const std::array<std::uint8_t, 16>& connection_uuid,
                                        const std::array<std::uint8_t, 16>& auth_context_uuid) {
  std::vector<std::uint8_t> out;
  PutUuid(&out, connection_uuid);
  PutUuid(&out, auth_context_uuid);
  PutString(&out, "default");
  PutString(&out, "read_write");
  return out;
}

sbps::Frame Frame(sbps::MessageType type,
                  std::vector<std::uint8_t> payload,
                  const std::array<std::uint8_t, 16>& connection_uuid = {},
                  const std::array<std::uint8_t, 16>& session_uuid = {}) {
  sbps::Frame frame;
  frame.header.message_type = static_cast<std::uint16_t>(type);
  frame.header.payload_schema_id = sbps::kSchemaNone;
  frame.header.request_uuid = sbps::MakeUuidV7Bytes();
  frame.header.connection_uuid = connection_uuid;
  frame.header.session_uuid = session_uuid;
  frame.payload = std::move(payload);
  return frame;
}

AttachedSession AttachAuthenticatedSession(ServerSessionRegistry* registry,
                                           const HostedEngineState& engine_state,
                                           std::string_view principal) {
  AttachedSession attached;
  attached.connection_uuid = sbps::MakeUuidV7Bytes();
  const auto auth = scratchbird::server::HandleAuthHandoff(
      registry,
      engine_state,
      Frame(sbps::MessageType::kAuthHandoff,
            AuthPayload(attached.connection_uuid, principal),
            attached.connection_uuid));
  Require(auth.accepted, "DBLC-013G auth handoff failed");
  const auto auth_context = scratchbird::server::DecodeAuthContextUuidForTest(auth.payload);
  Require(auth_context.has_value(), "DBLC-013G auth context decode failed");
  attached.auth_context_uuid = *auth_context;

  const auto attach = scratchbird::server::HandleAttachDatabase(
      registry,
      engine_state,
      Frame(sbps::MessageType::kAttachDatabase,
            AttachPayload(attached.connection_uuid, attached.auth_context_uuid),
            attached.connection_uuid));
  Require(attach.accepted, "DBLC-013G attach failed");
  const auto session_uuid = scratchbird::server::DecodeSessionUuidForTest(attach.payload);
  Require(session_uuid.has_value(), "DBLC-013G session UUID decode failed");
  attached.session_uuid = *session_uuid;
  return attached;
}

sbps::Frame PrepareFrame(const std::array<std::uint8_t, 16>& session_uuid,
                         const std::string& encoded) {
  return Frame(sbps::MessageType::kPrepareSblr,
               scratchbird::server::EncodePrepareSblrPayloadForTest(session_uuid, encoded),
               {},
               session_uuid);
}

sbps::Frame ExecuteFrame(const std::array<std::uint8_t, 16>& session_uuid,
                         const std::array<std::uint8_t, 16>& prepared_statement_uuid,
                         const std::string& encoded,
                         bool cursor_requested) {
  return Frame(sbps::MessageType::kExecuteSblr,
               scratchbird::server::EncodeExecuteSblrPayloadForTest(
                   session_uuid, prepared_statement_uuid, encoded, cursor_requested),
               {},
               session_uuid);
}

sbps::Frame FetchFrame(const std::array<std::uint8_t, 16>& session_uuid,
                       const std::array<std::uint8_t, 16>& cursor_uuid,
                       std::uint64_t max_rows = 1,
                       std::uint64_t max_bytes = 0) {
  return Frame(sbps::MessageType::kFetch,
               scratchbird::server::EncodeFetchPayloadForTest(
                   session_uuid, cursor_uuid, max_rows, max_bytes),
               {},
               session_uuid);
}

sbps::Frame CloseFrame(const std::array<std::uint8_t, 16>& session_uuid,
                       const std::array<std::uint8_t, 16>& cursor_uuid,
                       bool cancel = false) {
  return Frame(sbps::MessageType::kCloseCursor,
               cancel ? scratchbird::server::EncodeCancelCursorPayloadForTest(session_uuid, cursor_uuid)
                      : scratchbird::server::EncodeCloseCursorPayloadForTest(session_uuid, cursor_uuid),
               {},
               session_uuid);
}

sbps::Frame DisconnectFrame(const std::array<std::uint8_t, 16>& session_uuid,
                            std::string_view reason) {
  sbps::Frame frame = Frame(sbps::MessageType::kDisconnectNotice, {}, {}, session_uuid);
  PutUuid(&frame.payload, session_uuid);
  PutString(&frame.payload, reason);
  return frame;
}

std::string StreamEnvelope(std::string_view operation_id,
                           std::uint64_t rows,
                           std::string_view finality_mode = {},
                           std::uint64_t finality_after_fetches = 0) {
  std::string out =
      "{\"envelope\":\"SBLRExecutionEnvelope.v3\","
      "\"operation_family\":\"sblr.query.relational.v3\","
      "\"operation_id\":\"";
  out += operation_id;
  out += "\",\"result_shape\":\"canonical.rowset.v1\","
         "\"diagnostic_shape\":\"engine.diagnostic.v1\","
         "\"stream_row_count\":";
  out += std::to_string(rows);
  if (!finality_mode.empty()) {
    out += ",\"stream_finality_mode\":\"";
    out += finality_mode;
    out += "\",\"stream_finality_after_fetches\":";
    out += std::to_string(finality_after_fetches);
  }
  out += "}";
  return out;
}

std::array<std::uint8_t, 16> ExecuteCursor(ServerSessionRegistry* registry,
                                          const HostedEngineState& engine_state,
                                          const std::array<std::uint8_t, 16>& session_uuid,
                                          const std::string& encoded) {
  const auto execute = scratchbird::server::HandleExecuteSblr(
      registry, engine_state, ExecuteFrame(session_uuid, {}, encoded, true));
  if (!execute.accepted) {
    for (const auto& diagnostic : execute.diagnostics) {
      std::cerr << diagnostic.code << ": " << diagnostic.safe_message << '\n';
    }
  }
  Require(execute.accepted, "DBLC-013G execute cursor failed");
  const auto cursor_uuid = scratchbird::server::DecodeCursorUuidForTest(execute.payload);
  Require(cursor_uuid.has_value(), "DBLC-013G cursor UUID decode failed");
  return *cursor_uuid;
}

std::string UuidText(const std::array<std::uint8_t, 16>& value) {
  return scratchbird::server::UuidBytesToText(value);
}

scratchbird::server::ServerRequestRecord RequireRequest(
    const ServerSessionRegistry& registry,
    const std::array<std::uint8_t, 16>& target_uuid,
    ServerRequestLifecycleState expected_state,
    std::string_view message) {
  const auto record = scratchbird::server::FindServerRequestLifecycle(registry, UuidText(target_uuid));
  Require(record.has_value(), message);
  Require(record->state == expected_state, message);
  const auto finality = registry.finality_by_request_uuid.find(UuidText(record->request_uuid));
  Require(finality != registry.finality_by_request_uuid.end(), "DBLC-013G finality record missing");
  Require(finality->second.state == scratchbird::server::ServerRequestLifecycleStateName(expected_state),
          "DBLC-013G finality state mismatch");
  return *record;
}

bool TransactionHasState(const std::filesystem::path& database_path,
                         std::uint64_t local_transaction_id,
                         tx::TransactionState state) {
  const auto loaded = db::LoadLocalTransactionInventoryFromDatabase(database_path.string());
  Require(loaded.ok(), "DBLC-013G transaction inventory load failed");
  for (const auto& entry : loaded.inventory.entries) {
    if (entry.identity.local_id.value == local_transaction_id) {
      return entry.state == state;
    }
  }
  return false;
}

sbps::Frame ManagementFrame(const std::array<std::uint8_t, 16>& session_uuid,
                            std::string operation_key,
                            std::string target_uuid = {},
                            std::uint64_t timeout_ms = 5000,
                            bool include_history = true) {
  ServerManagementRequest request;
  request.operation_key = std::move(operation_key);
  request.target_uuid = std::move(target_uuid);
  request.timeout_ms = timeout_ms;
  request.include_history = include_history;
  auto frame = Frame(sbps::MessageType::kManagementRequest, {}, {}, session_uuid);
  frame.header.payload_schema_id = sbps::kSchemaManagementRequestV1;
  frame.payload = scratchbird::server::EncodeServerManagementRequestForTest(request);
  return frame;
}

ServerManagementContext ManagementContext(ServerBootstrapConfig* config,
                                          ServerLifecycleArtifacts* artifacts,
                                          HostedEngineState* engine_state,
                                          ServerSessionRegistry* registry,
                                          ServerMaintenanceCoordinator* coordinator) {
  ServerManagementContext context;
  context.config = config;
  context.artifacts = artifacts;
  context.engine_state = engine_state;
  context.session_registry = registry;
  context.maintenance_coordinator = coordinator;
  return context;
}

void TestPrepareFetchCloseLifecycle(const HostedEngineState& engine_state,
                                    const AttachedSession& admin,
                                    ServerSessionRegistry* registry) {
  const auto prepare = scratchbird::server::HandlePrepareSblr(
      registry, engine_state, PrepareFrame(admin.session_uuid,
                                           scratchbird::server::EncodeShowVersionSblrForTest()));
  Require(prepare.accepted, "DBLC-013G prepare was not accepted");
  const auto prepared_uuid = scratchbird::server::DecodePreparedStatementUuidForTest(prepare.payload);
  Require(prepared_uuid.has_value(), "DBLC-013G prepared UUID decode failed");
  (void)RequireRequest(*registry, *prepared_uuid, ServerRequestLifecycleState::kCompleted,
                       "DBLC-013G prepare request lifecycle missing");

  const auto cursor_uuid = ExecuteCursor(registry,
                                         engine_state,
                                         admin.session_uuid,
                                         StreamEnvelope("dml.select", 3));
  auto execute_request = RequireRequest(*registry,
                                        cursor_uuid,
                                        ServerRequestLifecycleState::kCursorOpen,
                                        "DBLC-013G execute cursor lifecycle missing");
  Require(!scratchbird::server::UuidBytesToText(execute_request.finality_token_uuid).empty(),
          "DBLC-013G execute finality token missing");

  const auto fetch_frame = FetchFrame(admin.session_uuid, cursor_uuid, 2, 4096);
  const auto fetch = scratchbird::server::HandleFetch(registry, fetch_frame);
  Require(fetch.accepted, "DBLC-013G fetch failed");
  (void)RequireRequest(*registry,
                       fetch_frame.header.request_uuid,
                       ServerRequestLifecycleState::kCompleted,
                       "DBLC-013G fetch request lifecycle missing");
  (void)RequireRequest(*registry,
                       cursor_uuid,
                       ServerRequestLifecycleState::kCursorOpen,
                       "DBLC-013G cursor should remain open after partial fetch");

  const auto close = scratchbird::server::HandleCloseCursor(
      registry, CloseFrame(admin.session_uuid, cursor_uuid));
  Require(close.accepted, "DBLC-013G close cursor failed");
  (void)RequireRequest(*registry,
                       cursor_uuid,
                       ServerRequestLifecycleState::kCompleted,
                       "DBLC-013G cursor close did not complete lifecycle");
  const auto cursor_it = registry->cursors_by_uuid.find(UuidText(cursor_uuid));
  Require(cursor_it != registry->cursors_by_uuid.end() && cursor_it->second.closed,
          "DBLC-013G cursor was not marked closed");
}

void TestCancelAndFinalityManagement(const HostedEngineState& engine_state,
                                     const AttachedSession& admin,
                                     const AttachedSession& alice,
                                     ServerBootstrapConfig* config,
                                     ServerLifecycleArtifacts* artifacts,
                                     ServerMaintenanceCoordinator* coordinator,
                                     ServerSessionRegistry* registry) {
  const auto cursor_uuid = ExecuteCursor(registry,
                                         engine_state,
                                         admin.session_uuid,
                                         StreamEnvelope("dml.select", 4));
  const auto request = RequireRequest(*registry,
                                      cursor_uuid,
                                      ServerRequestLifecycleState::kCursorOpen,
                                      "DBLC-013G cancellable request missing");
  const std::string finality_token = UuidText(request.finality_token_uuid);
  HostedEngineState mutable_engine_state = engine_state;
  auto context = ManagementContext(config, artifacts, &mutable_engine_state, registry, coordinator);

  const auto denied = scratchbird::server::HandleServerManagementRequest(
      context, ManagementFrame(alice.session_uuid, "cancel_request", finality_token));
  Require(denied.error, "DBLC-013G alice cancel was not denied by engine auth");
  Require(HasDiagnostic(denied, "SECURITY.AUTHORIZATION.DENIED"),
          "DBLC-013G cancel denial did not come from engine authorization");
  (void)RequireRequest(*registry,
                       cursor_uuid,
                       ServerRequestLifecycleState::kCursorOpen,
                       "DBLC-013G denied cancel mutated request state");

  const auto cancelled = scratchbird::server::HandleServerManagementRequest(
      context, ManagementFrame(admin.session_uuid, "cancel_request", finality_token));
  Require(cancelled.accepted && !cancelled.error, "DBLC-013G admin cancel failed");
  const std::string cancel_payload(cancelled.payload.begin(), cancelled.payload.end());
  Require(Contains(cancel_payload, "\"state\":\"cancelled\""),
          "DBLC-013G cancel payload missing cancelled state");
  (void)RequireRequest(*registry,
                       cursor_uuid,
                       ServerRequestLifecycleState::kCancelled,
                       "DBLC-013G cancelled request lifecycle state missing");

  const auto shown = scratchbird::server::HandleServerManagementRequest(
      context, ManagementFrame(admin.session_uuid, "show_finality", finality_token));
  Require(shown.accepted && !shown.error, "DBLC-013G show_finality failed");
  const std::string finality_payload(shown.payload.begin(), shown.payload.end());
  Require(Contains(finality_payload, finality_token),
          "DBLC-013G show_finality did not include finality token");
  Require(Contains(finality_payload, "\"state\":\"cancelled\""),
          "DBLC-013G show_finality did not render cancelled finality");
}

void TestTimeoutLifecycle(const HostedEngineState& engine_state,
                          const AttachedSession& admin,
                          ServerSessionRegistry* registry) {
  const auto cursor_uuid = ExecuteCursor(registry,
                                         engine_state,
                                         admin.session_uuid,
                                         StreamEnvelope("dml.select", 2, "timeout", 0));
  const auto timed_out = scratchbird::server::HandleFetch(
      registry, FetchFrame(admin.session_uuid, cursor_uuid, 1, 4096));
  Require(!timed_out.accepted, "DBLC-013G timeout fetch was accepted");
  Require(HasDiagnostic(timed_out, "SERVER.STREAM.TIMEOUT"),
          "DBLC-013G timeout diagnostic missing");
  (void)RequireRequest(*registry,
                       cursor_uuid,
                       ServerRequestLifecycleState::kTimedOut,
                       "DBLC-013G timeout did not update cursor request lifecycle");
}

void TestDisconnectUnknownOutcome(const std::filesystem::path& database_path,
                                  const HostedEngineState& engine_state,
                                  const AttachedSession& admin,
                                  ServerSessionRegistry* registry) {
  const auto cursor_uuid = ExecuteCursor(registry,
                                         engine_state,
                                         admin.session_uuid,
                                         scratchbird::server::EncodeBeginTransactionSblrForTest());
  auto request = RequireRequest(*registry,
                                cursor_uuid,
                                ServerRequestLifecycleState::kCursorOpen,
                                "DBLC-013G begin transaction cursor request missing");
  const auto session_it = registry->sessions_by_uuid.find(UuidText(admin.session_uuid));
  Require(session_it != registry->sessions_by_uuid.end(), "DBLC-013G active session missing");
  const auto active_local_transaction_id = session_it->second.local_transaction_id;
  Require(active_local_transaction_id != 0, "DBLC-013G begin did not bind transaction id");

  const auto disconnect = scratchbird::server::HandleDisconnectNotice(
      registry, DisconnectFrame(admin.session_uuid, "parser_killed"));
  Require(disconnect.accepted, "DBLC-013G disconnect failed");
  Require(HasDiagnostic(disconnect, "ENGINE.DBLC_TRANSACTION_OUTCOME_UNKNOWN"),
          "DBLC-013G disconnect did not emit unknown transaction outcome");
  request = RequireRequest(*registry,
                           cursor_uuid,
                           ServerRequestLifecycleState::kUnknownOutcome,
                           "DBLC-013G disconnect did not preserve unknown request outcome");
  Require(Contains(request.detail, "unknown_preserved"),
          "DBLC-013G unknown outcome detail missing");
  Require(TransactionHasState(database_path, active_local_transaction_id, tx::TransactionState::active),
          "DBLC-013G disconnect changed MGA transaction finality");
}

}  // namespace

int main() {
  const auto temp_dir = MakeTempDir();
  const auto database_path = temp_dir / "dblc013g_session_request_cursor.sbdb";
  const std::string database_uuid = CreateOpenDatabase(database_path);
  WriteAuthStore(database_path);
  const auto engine_state = MakeEngineState(database_path, database_uuid);

  ServerSessionRegistry registry;
  const auto admin = AttachAuthenticatedSession(&registry, engine_state, "admin");
  const auto alice = AttachAuthenticatedSession(&registry, engine_state, "alice");

  ServerBootstrapConfig config;
  config.database_default_path = database_path;
  ServerLifecycleArtifacts artifacts;
  artifacts.state = "dblc013g-test";
  ServerMaintenanceCoordinator coordinator =
      scratchbird::server::BuildMaintenanceCoordinator(config, artifacts);

  TestPrepareFetchCloseLifecycle(engine_state, admin, &registry);
  TestCancelAndFinalityManagement(engine_state,
                                  admin,
                                  alice,
                                  &config,
                                  &artifacts,
                                  &coordinator,
                                  &registry);
  TestTimeoutLifecycle(engine_state, admin, &registry);
  TestDisconnectUnknownOutcome(database_path, engine_state, admin, &registry);

  std::filesystem::remove_all(temp_dir);
  return EXIT_SUCCESS;
}
