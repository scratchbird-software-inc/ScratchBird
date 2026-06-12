// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "database_lifecycle.hpp"
#include "database_lifecycle_test_memory.hpp"
#include "event_notification_router.hpp"
#include "lifecycle/engine_lifecycle_api.hpp"
#include "local_transaction_store.hpp"
#include "notification/notification_api.hpp"
#include "parser_server_event_ipc.hpp"
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

namespace api = scratchbird::engine::internal_api;
namespace db = scratchbird::storage::database;
namespace tx = scratchbird::transaction::mga;
namespace uuid = scratchbird::core::uuid;
using scratchbird::core::platform::UuidKind;
using scratchbird::server::HostedDatabaseSnapshot;
using scratchbird::server::HostedDatabaseState;
using scratchbird::server::HostedEngineState;
using scratchbird::server::ServerSessionRecord;
using scratchbird::server::ServerSessionRegistry;
using scratchbird::server::SessionOperationResult;
namespace sbps = scratchbird::server::sbps;

constexpr std::string_view kVerifier =
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
constexpr std::string_view kAlicePrincipalUuid =
    "019f0a11-ce00-7000-8000-000000000009";

struct AttachedSession {
  std::array<std::uint8_t, 16> connection_uuid{};
  std::array<std::uint8_t, 16> auth_context_uuid{};
  std::array<std::uint8_t, 16> session_uuid{};
};

struct DisconnectDecoded {
  std::string outcome;
  std::array<std::uint8_t, 16> session_uuid{};
  std::string detail;
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

std::uint16_t GetU16(const std::vector<std::uint8_t>& data, std::size_t offset) {
  return static_cast<std::uint16_t>(data[offset]) |
         static_cast<std::uint16_t>(static_cast<std::uint16_t>(data[offset + 1]) << 8u);
}

std::array<std::uint8_t, 16> GetUuid(const std::vector<std::uint8_t>& data, std::size_t offset) {
  std::array<std::uint8_t, 16> uuid_bytes{};
  std::copy_n(data.data() + offset, uuid_bytes.size(), uuid_bytes.data());
  return uuid_bytes;
}

bool ReadString(const std::vector<std::uint8_t>& data, std::size_t* offset, std::string* out) {
  if (*offset + 2 > data.size()) return false;
  const auto length = GetU16(data, *offset);
  *offset += 2;
  if (*offset + length > data.size()) return false;
  out->assign(reinterpret_cast<const char*>(data.data() + *offset), length);
  *offset += length;
  return true;
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

bool HasDiagnostic(const api::EngineApiResult& result, std::string_view code) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.code == code) return true;
  }
  return false;
}

std::filesystem::path MakeTempDir() {
  std::string tmpl = "/tmp/sb_dblc009_detach_cleanup.XXXXXX";
  std::vector<char> writable(tmpl.begin(), tmpl.end());
  writable.push_back('\0');
  char* made = ::mkdtemp(writable.data());
  Require(made != nullptr, "mkdtemp failed for DBLC-009 detach cleanup test");
  return std::filesystem::path(made);
}

std::string CreateOpenDatabase(const std::filesystem::path& path) {
  db::DatabaseCreateConfig create;
  create.path = path.string();
  create.database_uuid = uuid::GenerateEngineIdentityV7(UuidKind::database, 1779200001000).value;
  create.filespace_uuid = uuid::GenerateEngineIdentityV7(UuidKind::filespace, 1779200001001).value;
  create.page_size = 16384;
  create.creation_unix_epoch_millis = 1779200001002;
  create.allow_minimal_resource_bootstrap = true;
  create.require_resource_seed_pack = false;
  create.allow_overwrite = true;
  const auto created = db::CreateDatabaseFile(create);
  Require(created.ok(), "DBLC-009 database create failed");
  const auto opened = db::OpenDatabaseFile({path.string(), false, false, false});
  Require(opened.ok(), "DBLC-009 first open activation failed");
  const auto clean = db::MarkDatabaseCleanShutdown(path.string());
  Require(clean.ok(), "DBLC-009 clean shutdown marker failed");
  return uuid::UuidToString(create.database_uuid.value);
}

void WriteAuthStore(const std::filesystem::path& database_path,
                    const std::string& database_uuid) {
  scratchbird::tests::database_lifecycle::CreateDurableLocalPasswordPrincipal(
      database_path,
      database_uuid,
      kAlicePrincipalUuid,
      "alice",
      kVerifier,
      9,
      "DBLC-009");
}

HostedEngineState MakeEngineState(const std::filesystem::path& database_path,
                                  const std::string& database_uuid,
                                  HostedDatabaseState state = HostedDatabaseState::kOpen) {
  HostedEngineState engine_state;
  engine_state.engine_context_active = true;
  HostedDatabaseSnapshot database;
  database.state = state;
  database.database_open = true;
  database.database_path = database_path.string();
  database.database_uuid = database_uuid;
  database.read_only = state == HostedDatabaseState::kReadOnly;
  database.write_admission_fenced = state == HostedDatabaseState::kReadOnly ||
                                    state == HostedDatabaseState::kMaintenance ||
                                    state == HostedDatabaseState::kRestrictedOpen;
  database.policy_generation = 1;
  database.capability_policy_generation = 1;
  database.security_epoch = 1;
  database.security_provider_generation = 1;
  database.security_provider_family = "local_password";
  database.security_provider_state = "healthy";
  database.default_policy_installed = true;
  database.config_source_epoch = 1;
  database.config_reload_generation = 1;
  database.cache_invalidation_epoch = 1;
  engine_state.databases.push_back(std::move(database));
  return engine_state;
}

std::string Evidence(std::string_view principal) {
  return scratchbird::tests::database_lifecycle::DurableLocalPasswordEvidence(
      principal,
      kAlicePrincipalUuid,
      kVerifier,
      "right:CONNECT,right:OBS_RUNTIME_ALL");
}

std::vector<std::uint8_t> AuthPayload(const std::array<std::uint8_t, 16>& connection_uuid) {
  std::vector<std::uint8_t> out;
  PutUuid(&out, connection_uuid);
  out.push_back(1);
  out.push_back(0);
  out.push_back(0);
  out.push_back(0);
  PutString(&out, "local_password");
  PutString(&out, "alice");
  PutString(&out, "default");
  PutString(&out, "en");
  PutString(&out, Evidence("alice"));
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
  frame.header.request_uuid = sbps::MakeUuidV7Bytes();
  frame.header.connection_uuid = connection_uuid;
  frame.header.session_uuid = session_uuid;
  frame.payload = std::move(payload);
  return frame;
}

AttachedSession AttachAuthenticatedSession(ServerSessionRegistry* registry,
                                           const HostedEngineState& engine_state) {
  AttachedSession attached;
  attached.connection_uuid = sbps::MakeUuidV7Bytes();
  auto auth_frame = Frame(sbps::MessageType::kAuthHandoff,
                          AuthPayload(attached.connection_uuid),
                          attached.connection_uuid);
  const auto auth = scratchbird::server::HandleAuthHandoff(registry, engine_state, auth_frame);
  Require(auth.accepted, "DBLC-009 auth handoff failed");
  const auto auth_context = scratchbird::server::DecodeAuthContextUuidForTest(auth.payload);
  Require(auth_context.has_value(), "DBLC-009 auth context decode failed");
  attached.auth_context_uuid = *auth_context;

  auto attach_frame = Frame(sbps::MessageType::kAttachDatabase,
                            AttachPayload(attached.connection_uuid, attached.auth_context_uuid),
                            attached.connection_uuid);
  const auto attach = scratchbird::server::HandleAttachDatabase(registry, engine_state, attach_frame);
  Require(attach.accepted, "DBLC-009 attach failed");
  const auto session_uuid = scratchbird::server::DecodeSessionUuidForTest(attach.payload);
  Require(session_uuid.has_value(), "DBLC-009 session UUID decode failed");
  attached.session_uuid = *session_uuid;
  return attached;
}

sbps::Frame PrepareFrame(const std::array<std::uint8_t, 16>& session_uuid,
                         std::string encoded) {
  return Frame(sbps::MessageType::kPrepareSblr,
               scratchbird::server::EncodePrepareSblrPayloadForTest(session_uuid, encoded),
               {},
               session_uuid);
}

sbps::Frame ExecuteFrame(const std::array<std::uint8_t, 16>& session_uuid,
                         const std::array<std::uint8_t, 16>& prepared_statement_uuid,
                         std::string encoded,
                         bool cursor_requested) {
  return Frame(sbps::MessageType::kExecuteSblr,
               scratchbird::server::EncodeExecuteSblrPayloadForTest(
                   session_uuid, prepared_statement_uuid, encoded, cursor_requested),
               {},
               session_uuid);
}

sbps::Frame FetchFrame(const std::array<std::uint8_t, 16>& session_uuid,
                       const std::array<std::uint8_t, 16>& cursor_uuid) {
  return Frame(sbps::MessageType::kFetch,
               scratchbird::server::EncodeFetchPayloadForTest(session_uuid, cursor_uuid),
               {},
               session_uuid);
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

DisconnectDecoded DecodeDisconnect(const SessionOperationResult& result) {
  DisconnectDecoded decoded;
  std::size_t offset = 0;
  Require(ReadString(result.payload, &offset, &decoded.outcome), "disconnect outcome decode failed");
  Require(offset + 16 <= result.payload.size(), "disconnect session UUID decode failed");
  decoded.session_uuid = GetUuid(result.payload, offset);
  offset += 16;
  Require(ReadString(result.payload, &offset, &decoded.detail), "disconnect detail decode failed");
  return decoded;
}

bool TransactionHasState(const std::filesystem::path& database_path,
                         std::uint64_t local_transaction_id,
                         tx::TransactionState state) {
  const auto loaded = db::LoadLocalTransactionInventoryFromDatabase(database_path.string());
  Require(loaded.ok(), "DBLC-009 transaction inventory load failed");
  for (const auto& entry : loaded.inventory.entries) {
    if (entry.identity.local_id.value == local_transaction_id) {
      return entry.state == state;
    }
  }
  return false;
}

scratchbird::server::ParserServerEventSession EventSessionFor(
    const AttachedSession& attached,
    const ServerSessionRecord& session) {
  scratchbird::server::ParserServerEventSession event_session;
  event_session.parser_channel_uuid = scratchbird::server::UuidBytesToText(attached.connection_uuid);
  event_session.engine_context.request_id = "dblc-009-event-disconnect";
  event_session.engine_context.database_path = session.database_path;
  event_session.engine_context.database_uuid.canonical = session.database_uuid;
  event_session.engine_context.principal_uuid.canonical =
      scratchbird::server::UuidBytesToText(session.effective_user_uuid);
  event_session.engine_context.session_uuid.canonical =
      scratchbird::server::UuidBytesToText(session.session_uuid);
  event_session.engine_context.local_transaction_id = session.local_transaction_id;
  event_session.engine_context.snapshot_visible_through_local_transaction_id =
      session.snapshot_visible_through_local_transaction_id;
  event_session.engine_context.security_context_present = true;
  event_session.engine_context.catalog_generation_id = session.catalog_generation;
  event_session.engine_context.security_epoch = session.security_epoch;
  event_session.engine_context.resource_epoch = session.resource_epoch;
  event_session.engine_context.name_resolution_epoch = session.name_resolution_epoch;
  event_session.engine_context.trust_mode =
      scratchbird::server::ParserServerEventTrustMode::embedded_in_process;
  event_session.engine_context.trace_tags.push_back("security.fixture_trace_authority");
  event_session.engine_context.trace_tags.push_back("group:DBA");
  event_session.session_bound = true;
  return event_session;
}

void SeedActiveEventTransaction(const std::filesystem::path& database_path, std::uint64_t tx_id) {
  std::ofstream out(database_path.string() + ".sb.crud_events", std::ios::binary | std::ios::app);
  out << "SBCRUD1\tTX_BEGIN\t" << tx_id << "\tdblc009_event_disconnect\n";
  Require(static_cast<bool>(out), "DBLC-009 failed to seed event transaction evidence");
}

void CreateEventChannel(const scratchbird::server::ParserServerEventSession& event_session,
                        const std::string& channel_uuid) {
  api::EngineCreateEventChannelRequest request;
  request.context.request_id = "dblc-009-event-channel-create";
  request.context.database_path = event_session.engine_context.database_path;
  request.context.database_uuid.canonical = event_session.engine_context.database_uuid.canonical;
  request.context.principal_uuid.canonical = event_session.engine_context.principal_uuid.canonical;
  request.context.session_uuid.canonical = event_session.engine_context.session_uuid.canonical;
  request.context.local_transaction_id = event_session.engine_context.local_transaction_id;
  request.context.security_context_present = event_session.engine_context.security_context_present;
  request.context.trust_mode =
      event_session.engine_context.trust_mode ==
              scratchbird::server::ParserServerEventTrustMode::
                  embedded_in_process
          ? api::EngineTrustMode::embedded_in_process
          : api::EngineTrustMode::server_isolated;
  request.context.trace_tags = event_session.engine_context.trace_tags;
  request.target_object = {{channel_uuid}, "event_channel"};
  request.option_envelopes.push_back("channel_uuid:" + channel_uuid);
  request.option_envelopes.push_back("channel:dblc009_event_channel");
  const auto created = api::EngineCreateEventChannel(request);
  Require(created.ok, "DBLC-009 failed to create engine-authorized event channel");
}

void VerifyEventDisconnectCleanup(const AttachedSession& attached,
                                  const ServerSessionRecord& session) {
  scratchbird::server::ParserEventNotificationRouter router;
  scratchbird::server::ParserServerEventIpcRuntime runtime(&router);
  const auto event_session = EventSessionFor(attached, session);
  const std::string channel_uuid = "event.channel.dblc009";
  SeedActiveEventTransaction(std::filesystem::path(session.database_path), session.local_transaction_id);
  CreateEventChannel(event_session, channel_uuid);

  scratchbird::server::PsEventSubscribeRequest subscribe;
  subscribe.request_uuid = "event-subscribe-dblc-009";
  subscribe.session = event_session;
  subscribe.channel_uuid = channel_uuid;
  subscribe.rendering_profile_uuid = "rendering.default";
  const auto subscribed = runtime.HandleSubscribe(subscribe);
  Require(subscribed.outcome == "accepted", "DBLC-009 event subscribe failed");
  Require(router.ActiveSubscriptionCount() == 1, "DBLC-009 event subscription was not registered");

  const auto enqueued = router.EnqueueCommittedEvent("event.channel.dblc009",
                                                     "event-001",
                                                     "payload.text",
                                                     "payload");
  Require(enqueued.ok, "DBLC-009 event enqueue failed");
  Require(router.QueuedEventCount(event_session.parser_channel_uuid) == 1,
          "DBLC-009 event queue was not populated");

  scratchbird::server::PsEventDisconnectRequest disconnect;
  disconnect.session = event_session;
  disconnect.disconnect_reason = "parser_killed";
  const auto cleaned = runtime.HandleDisconnect(disconnect);
  Require(cleaned.outcome == "accepted", "DBLC-009 event disconnect cleanup failed");
  Require(cleaned.removed_count == 1, "DBLC-009 event disconnect removed wrong count");
  Require(router.ActiveSubscriptionCount() == 0, "DBLC-009 event subscription leaked");
  Require(router.QueuedEventCount(event_session.parser_channel_uuid) == 0,
          "DBLC-009 event queue leaked");
}

api::EngineRequestContext EngineContext(const std::filesystem::path& database_path,
                                        const std::string& database_uuid,
                                        const std::array<std::uint8_t, 16>& session_uuid) {
  api::EngineRequestContext context;
  context.trust_mode = api::EngineTrustMode::server_isolated;
  context.request_id = "dblc-009-lifecycle-detach";
  context.database_path = database_path.string();
  context.database_uuid.canonical = database_uuid;
  context.principal_uuid.canonical = "019e0f09-a100-7000-8000-000000000901";
  context.session_uuid.canonical = scratchbird::server::UuidBytesToText(session_uuid);
  context.security_context_present = true;
  context.catalog_generation_id = 1;
  context.security_epoch = 1;
  context.resource_epoch = 1;
  context.name_resolution_epoch = 1;
  return context;
}

void VerifyLifecycleDetachClusterFailClosed(const std::filesystem::path& database_path,
                                            const std::string& database_uuid,
                                            const std::array<std::uint8_t, 16>& session_uuid) {
  api::EngineDetachLifecycleRequest request;
  request.context = EngineContext(database_path, database_uuid, session_uuid);
  request.option_envelopes.push_back("cluster_lifecycle_detach:true");
  const auto denied = api::EngineDetachLifecycle(request);
  Require(!denied.ok, "DBLC-009 lifecycle detach admitted standalone cluster path");
  Require(HasDiagnostic(denied, "SB_ENGINE_API_CLUSTER_AUTHORITY_UNAVAILABLE"),
          "DBLC-009 lifecycle detach did not fail closed with cluster diagnostic");
}

void VerifyDetachCleanup(const std::filesystem::path& database_path,
                         const std::string& database_uuid) {
  ServerSessionRegistry registry;
  const auto engine_state = MakeEngineState(database_path, database_uuid);
  const auto session_a = AttachAuthenticatedSession(&registry, engine_state);
  const auto session_b = AttachAuthenticatedSession(&registry, engine_state);
  Require(registry.sessions_by_uuid.size() == 2, "DBLC-009 did not create two sessions");
  Require(registry.auth_contexts_by_uuid.size() == 2, "DBLC-009 did not retain two auth contexts");

  const auto prepare = scratchbird::server::HandlePrepareSblr(
      &registry, engine_state, PrepareFrame(session_a.session_uuid,
                                            scratchbird::server::EncodeShowVersionSblrForTest()));
  Require(prepare.accepted, "DBLC-009 prepare failed");
  const auto prepared_uuid = scratchbird::server::DecodePreparedStatementUuidForTest(prepare.payload);
  Require(prepared_uuid.has_value(), "DBLC-009 prepared UUID decode failed");

  const auto begin = scratchbird::server::HandleExecuteSblr(
      &registry,
      engine_state,
      ExecuteFrame(session_a.session_uuid,
                   {},
                   scratchbird::server::EncodeBeginTransactionSblrForTest(),
                   true));
  Require(begin.accepted, "DBLC-009 transaction begin cursor execute failed");
  const auto cursor_uuid = scratchbird::server::DecodeCursorUuidForTest(begin.payload);
  Require(cursor_uuid.has_value(), "DBLC-009 cursor UUID decode failed");

  auto session_it = registry.sessions_by_uuid.find(scratchbird::server::UuidBytesToText(session_a.session_uuid));
  Require(session_it != registry.sessions_by_uuid.end(), "DBLC-009 session A missing before detach");
  const auto active_local_transaction_id = session_it->second.local_transaction_id;
  Require(active_local_transaction_id != 0, "DBLC-009 transaction begin did not bind local transaction id");
  Require(TransactionHasState(database_path, active_local_transaction_id, tx::TransactionState::active),
          "DBLC-009 transaction was not active before detach");
  VerifyEventDisconnectCleanup(session_a, session_it->second);

  const auto disconnect_frame = DisconnectFrame(session_a.session_uuid, "parser_killed");
  const auto disconnect = scratchbird::server::HandleDisconnectNotice(&registry, disconnect_frame);
  Require(disconnect.accepted, "DBLC-009 disconnect did not detach session A");
  Require(HasDiagnostic(disconnect, "ENGINE.DBLC_DETACH_CLEANUP_COMPLETE"),
          "DBLC-009 cleanup evidence diagnostic missing");
  Require(HasDiagnostic(disconnect, "ENGINE.DBLC_TRANSACTION_OUTCOME_UNKNOWN"),
          "DBLC-009 active transaction unknown-outcome diagnostic missing");
  const auto decoded = DecodeDisconnect(disconnect);
  Require(decoded.outcome == "detached", "DBLC-009 disconnect outcome mismatch");
  Require(Contains(decoded.detail, "sessions_removed=1"), "DBLC-009 session cleanup count missing");
  Require(Contains(decoded.detail, "auth_contexts_removed=1"), "DBLC-009 auth cleanup count missing");
  Require(Contains(decoded.detail, "prepared_tombstoned=1"), "DBLC-009 prepared cleanup count missing");
  Require(Contains(decoded.detail, "cursors_tombstoned=1"), "DBLC-009 cursor cleanup count missing");
  Require(Contains(decoded.detail, "engine_results_released=0"),
          "DBLC-009 active transaction adoption should not retain an engine cursor result");
  Require(Contains(decoded.detail, "disconnect_does_not_commit=true"),
          "DBLC-009 no-commit evidence missing");
  Require(Contains(decoded.detail, "disconnect_does_not_rollback=true"),
          "DBLC-009 no-rollback evidence missing");
  Require(Contains(decoded.detail, "active_transaction_outcome=unknown_preserved"),
          "DBLC-009 unknown transaction outcome evidence missing");
  Require(TransactionHasState(database_path, active_local_transaction_id, tx::TransactionState::active),
          "DBLC-009 detach changed MGA transaction finality");

  Require(registry.sessions_by_uuid.count(scratchbird::server::UuidBytesToText(session_a.session_uuid)) == 0,
          "DBLC-009 session A remained active after detach");
  Require(registry.auth_contexts_by_uuid.count(scratchbird::server::UuidBytesToText(session_a.auth_context_uuid)) == 0,
          "DBLC-009 auth context A remained active after detach");
  Require(registry.sessions_by_uuid.count(scratchbird::server::UuidBytesToText(session_b.session_uuid)) == 1,
          "DBLC-009 session B was affected by session A detach");
  Require(registry.auth_contexts_by_uuid.count(scratchbird::server::UuidBytesToText(session_b.auth_context_uuid)) == 1,
          "DBLC-009 auth context B was affected by session A detach");

  const auto prepared_it = registry.prepared_by_uuid.find(scratchbird::server::UuidBytesToText(*prepared_uuid));
  Require(prepared_it != registry.prepared_by_uuid.end() && prepared_it->second.closed,
          "DBLC-009 prepared statement was not tombstoned");
  const auto cursor_it = registry.cursors_by_uuid.find(scratchbird::server::UuidBytesToText(*cursor_uuid));
  Require(cursor_it != registry.cursors_by_uuid.end() &&
              cursor_it->second.closed &&
              cursor_it->second.exhausted &&
              cursor_it->second.engine_result == nullptr &&
              cursor_it->second.finality_state == "parser_killed",
          "DBLC-009 cursor was not tombstoned with parser_killed finality");

  const auto execute_after_detach = scratchbird::server::HandleExecuteSblr(
      &registry, engine_state, ExecuteFrame(session_a.session_uuid, *prepared_uuid, "", false));
  Require(!execute_after_detach.accepted &&
              HasDiagnostic(execute_after_detach, "PARSER_SERVER_IPC.SESSION_REQUIRED"),
          "DBLC-009 detached session executed a prepared statement");
  const auto fetch_after_detach = scratchbird::server::HandleFetch(
      &registry, FetchFrame(session_a.session_uuid, *cursor_uuid));
  Require(!fetch_after_detach.accepted &&
              HasDiagnostic(fetch_after_detach, "PARSER_SERVER_IPC.CURSOR_NOT_FOUND"),
          "DBLC-009 detached session fetched a tombstoned cursor");

  const auto finality_it = registry.finality_by_request_uuid.find(
      scratchbird::server::UuidBytesToText(disconnect_frame.header.request_uuid));
  Require(finality_it != registry.finality_by_request_uuid.end(),
          "DBLC-009 detach finality record missing");
  Require(finality_it->second.state == "detached" &&
              Contains(finality_it->second.detail, "active_transaction_outcome=unknown_preserved"),
          "DBLC-009 detach finality did not preserve unknown transaction outcome");

  const auto unknown_uuid = sbps::MakeUuidV7Bytes();
  const auto sessions_before = registry.sessions_by_uuid.size();
  const auto auth_before = registry.auth_contexts_by_uuid.size();
  const auto unknown = scratchbird::server::HandleDisconnectNotice(
      &registry, DisconnectFrame(unknown_uuid, "parser_disconnect_notice"));
  Require(!unknown.accepted, "DBLC-009 unknown session detach was accepted");
  const auto unknown_decoded = DecodeDisconnect(unknown);
  Require(unknown_decoded.outcome == "session_not_found", "DBLC-009 unknown detach outcome mismatch");
  Require(registry.sessions_by_uuid.size() == sessions_before &&
              registry.auth_contexts_by_uuid.size() == auth_before,
          "DBLC-009 unknown detach mutated unrelated session state");

  VerifyLifecycleDetachClusterFailClosed(database_path, database_uuid, session_b.session_uuid);
}

}  // namespace

int main() {
  const auto temp_dir = MakeTempDir();
  const auto database_path = temp_dir / "dblc009_detach_cleanup.sbdb";
  const std::string database_uuid = CreateOpenDatabase(database_path);
  WriteAuthStore(database_path, database_uuid);

  VerifyDetachCleanup(database_path, database_uuid);

  std::filesystem::remove_all(temp_dir);
  return EXIT_SUCCESS;
}
