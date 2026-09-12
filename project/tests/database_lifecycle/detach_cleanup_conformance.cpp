// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "database_lifecycle.hpp"
#include "database_lifecycle_test_memory.hpp"
#include "ddl/create_api.hpp"
#include "dml/insert_api.hpp"
#include "dml/mutation_savepoint_capability.hpp"
#include "dml/select_api.hpp"
#include "event_notification_router.hpp"
#include "engine/functions/dispatch/function_dispatch.hpp"
#include "engine/functions/registry/function_seed_registry.hpp"
#include "lifecycle/engine_lifecycle_api.hpp"
#include "local_transaction_store.hpp"
#include "mga_relation_store/mga_relation_store.hpp"
#include "mga_relation_store/mga_relation_metadata_store.hpp"
#include "mga_relation_store/mga_savepoint_store.hpp"
#include "mga_relation_store/mga_savepoint_marker_codec.hpp"
#include "mga_relation_store/mga_large_value_store.hpp"
#include "mga_relation_store/mga_row_codec.hpp"
#include "mga_relation_store/mga_row_version_reader.hpp"
#include "notification/notification_api.hpp"
#include "parser_server_event_ipc.hpp"
#include "sblr_dispatch_server.hpp"
#include "session_registry.hpp"
#include "sblr_savepoint_coordinator.hpp"
#include "engine/sblr/sblr_savepoint_runtime.hpp"
#include "hash_digest.hpp"
#include "transaction/transaction_api.hpp"
#include "transaction/savepoint_api.hpp"
#include "transaction_state.hpp"
#include "uuid.hpp"
#include "wire/parser_server_ipc/disconnect_result_validation.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <sys/stat.h>
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

constexpr std::string_view kPassword = "DBLC013G-fixture-password";
constexpr std::string_view kCredentialFingerprint =
    "local-password-pbkdf2-sha256:v1:iterations=600000:"
    "salt=0123456789abcdef0123456789abcdef:"
    "verifier=4ce03aa5a5657aaf221192635ed9c63a"
    "cdb76d78a0994ec6e6ab55286e29e6a5";

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

void PutU64(std::vector<std::uint8_t>* out, std::uint64_t value) {
  for (std::uint32_t shift = 0; shift < 64; shift += 8) {
    out->push_back(static_cast<std::uint8_t>((value >> shift) & 0xffu));
  }
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
  create.bootstrap_principal_name = "alice";
  create.bootstrap_credential_fingerprint = std::string(kCredentialFingerprint);
  create.require_bootstrap_principal = true;
  create.allow_uncredentialed_bootstrap = false;
  create.allow_overwrite = true;
  const auto created = db::CreateDatabaseFile(create);
  Require(created.ok(), "DBLC-009 database create failed");
  const auto opened = db::OpenDatabaseFile({path.string(), false, false, false});
  Require(opened.ok(), "DBLC-009 first open activation failed");
  const auto clean = db::MarkDatabaseCleanShutdown(path.string());
  Require(clean.ok(), "DBLC-009 clean shutdown marker failed");
  return uuid::UuidToString(create.database_uuid.value);
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
  (void)principal;
  return std::string(kPassword);
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
  if (!auth.accepted) {
    for (const auto& diagnostic : auth.diagnostics) {
      std::cerr << diagnostic.code;
      for (const auto& field : diagnostic.fields) {
        std::cerr << ' ' << field.key << '=' << field.value;
      }
      std::cerr << '\n';
    }
  }
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
  auto frame = Frame(sbps::MessageType::kExecuteSblr,
                     scratchbird::server::EncodeExecuteSblrPayloadForTest(
                         session_uuid, prepared_statement_uuid, encoded,
                         cursor_requested),
                     session_uuid,
                     session_uuid);
  frame.header.payload_schema_id = 4003;
  return frame;
}

sbps::Frame FetchFrame(const std::array<std::uint8_t, 16>& session_uuid,
                       const std::array<std::uint8_t, 16>& cursor_uuid) {
  auto payload = scratchbird::server::EncodeFetchPayloadForTest(
      session_uuid, cursor_uuid, 1, 1);
  PutUuid(&payload, sbps::MakeUuidV7Bytes());
  PutU16(&payload, 1);
  PutU64(&payload, 1);
  return Frame(sbps::MessageType::kFetch,
               std::move(payload),
               {},
               session_uuid);
}

sbps::Frame DisconnectFrame(const std::array<std::uint8_t, 16>& connection_uuid,
                            const std::array<std::uint8_t, 16>& session_uuid,
                            std::string_view reason) {
  sbps::Frame frame;
  frame.header.message_type = static_cast<std::uint16_t>(sbps::MessageType::kDisconnectNotice);
  frame.header.request_uuid = sbps::MakeUuidV7Bytes();
  frame.header.connection_uuid = connection_uuid;
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
  event_session.engine_context.transaction_uuid.canonical =
      session.transaction_uuid;
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
  event_session.engine_context.trace_tags.push_back("right:EVENT_CREATE");
  event_session.engine_context.trace_tags.push_back("right:EVENT_PUBLISH");
  event_session.engine_context.trace_tags.push_back("right:EVENT_SUBSCRIBE");
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

  auto session_it = registry.sessions_by_uuid.find(scratchbird::server::UuidBytesToText(session_a.session_uuid));
  Require(session_it != registry.sessions_by_uuid.end(), "DBLC-009 session A missing before detach");
  api::EngineBeginTransactionRequest begin;
  begin.context = EngineContext(database_path, database_uuid,
                                session_a.session_uuid);
  begin.context.principal_uuid.canonical =
      scratchbird::server::UuidBytesToText(
          session_it->second.effective_user_uuid);
  begin.isolation_level = "read_committed";
  const auto begun = api::EngineBeginTransaction(begin);
  Require(begun.ok && begun.local_transaction_id != 0 &&
              !begun.transaction_uuid.canonical.empty(),
          "DBLC-009 engine MGA transaction begin failed");

  scratchbird::server::ServerTransactionState transaction;
  transaction.local_transaction_id = begun.local_transaction_id;
  transaction.snapshot_visible_through_local_transaction_id =
      begun.snapshot_visible_through_local_transaction_id;
  transaction.transaction_uuid = begun.transaction_uuid.canonical;
  transaction.isolation_level = "read_committed";
  session_it->second.local_transaction_id = transaction.local_transaction_id;
  session_it->second.default_local_transaction_id =
      transaction.local_transaction_id;
  session_it->second.snapshot_visible_through_local_transaction_id =
      transaction.snapshot_visible_through_local_transaction_id;
  session_it->second.transaction_uuid = transaction.transaction_uuid;
  session_it->second.transactions_by_local_id.emplace(
      transaction.local_transaction_id, transaction);

  const auto prepared_uuid = sbps::MakeUuidV7Bytes();
  scratchbird::server::ServerPreparedStatementRecord prepared;
  prepared.prepared_statement_uuid = prepared_uuid;
  prepared.session_uuid = session_a.session_uuid;
  prepared.auth_context_uuid = session_a.auth_context_uuid;
  prepared.principal_uuid = session_it->second.principal_uuid;
  prepared.effective_user_uuid = session_it->second.effective_user_uuid;
  prepared.database_uuid = database_uuid;
  prepared.operation_family = "sblr.query.relational.v3";
  prepared.operation_id = "query.select";
  prepared.prepare_local_transaction_id = transaction.local_transaction_id;
  prepared.prepare_transaction_uuid = transaction.transaction_uuid;
  prepared.prepare_snapshot_visible_through_local_transaction_id =
      transaction.snapshot_visible_through_local_transaction_id;
  registry.prepared_by_uuid.emplace(
      scratchbird::server::UuidBytesToText(prepared_uuid),
      std::move(prepared));

  const auto cursor_uuid = sbps::MakeUuidV7Bytes();
  scratchbird::server::ServerCursorRecord cursor;
  cursor.cursor_uuid = cursor_uuid;
  cursor.session_uuid = session_a.session_uuid;
  cursor.prepared_statement_uuid = prepared_uuid;
  cursor.operation_id = "query.select";
  cursor.owning_local_transaction_id = transaction.local_transaction_id;
  cursor.owning_snapshot_visible_through_local_transaction_id =
      transaction.snapshot_visible_through_local_transaction_id;
  cursor.owning_transaction_uuid = transaction.transaction_uuid;
  registry.cursors_by_uuid.emplace(
      scratchbird::server::UuidBytesToText(cursor_uuid), std::move(cursor));

  const auto active_local_transaction_id = transaction.local_transaction_id;
  Require(TransactionHasState(database_path, active_local_transaction_id, tx::TransactionState::active),
          "DBLC-009 transaction was not active before detach");
  VerifyEventDisconnectCleanup(session_a, session_it->second);

  const auto disconnect_frame = DisconnectFrame(session_a.connection_uuid,
                                                 session_a.session_uuid,
                                                 "parser_disconnect_notice");
  const auto disconnect = scratchbird::server::HandleDisconnectNotice(&registry, disconnect_frame);
  Require(disconnect.accepted, "DBLC-009 disconnect did not detach session A");
  Require(HasDiagnostic(disconnect, "ENGINE.DBLC_DETACH_CLEANUP_COMPLETE"),
          "DBLC-009 cleanup evidence diagnostic missing");
  Require(HasDiagnostic(disconnect, "ENGINE.DBLC_DETACH_TRANSACTION_ROLLED_BACK"),
          "DBLC-009 engine rollback evidence diagnostic missing");
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
  Require(Contains(decoded.detail, "active_transaction_outcome=none"),
          "DBLC-009 completed transaction outcome evidence missing");
  Require(TransactionHasState(database_path, active_local_transaction_id,
                              tx::TransactionState::rolled_back),
          "DBLC-009 engine did not own disconnect rollback finality");

  Require(registry.sessions_by_uuid.count(scratchbird::server::UuidBytesToText(session_a.session_uuid)) == 0,
          "DBLC-009 session A remained active after detach");
  Require(registry.auth_contexts_by_uuid.count(scratchbird::server::UuidBytesToText(session_a.auth_context_uuid)) == 0,
          "DBLC-009 auth context A remained active after detach");
  Require(registry.sessions_by_uuid.count(scratchbird::server::UuidBytesToText(session_b.session_uuid)) == 1,
          "DBLC-009 session B was affected by session A detach");
  Require(registry.auth_contexts_by_uuid.count(scratchbird::server::UuidBytesToText(session_b.auth_context_uuid)) == 1,
          "DBLC-009 auth context B was affected by session A detach");

  const auto prepared_it = registry.prepared_by_uuid.find(scratchbird::server::UuidBytesToText(prepared_uuid));
  Require(prepared_it != registry.prepared_by_uuid.end() && prepared_it->second.closed,
          "DBLC-009 prepared statement was not tombstoned");
  const auto cursor_it = registry.cursors_by_uuid.find(scratchbird::server::UuidBytesToText(cursor_uuid));
  Require(cursor_it != registry.cursors_by_uuid.end() &&
              cursor_it->second.closed &&
              cursor_it->second.exhausted &&
              cursor_it->second.engine_result == nullptr &&
              cursor_it->second.finality_state == "parser_disconnected",
          "DBLC-009 cursor was not tombstoned with disconnect finality");

  const auto execute_after_detach = scratchbird::server::HandleExecuteSblr(
      &registry, engine_state, ExecuteFrame(session_a.session_uuid, prepared_uuid, "", false));
  Require(!execute_after_detach.accepted &&
              HasDiagnostic(execute_after_detach, "PARSER_SERVER_IPC.SESSION_REQUIRED"),
          "DBLC-009 detached session executed a prepared statement");
  const auto fetch_after_detach = scratchbird::server::HandleFetch(
      &registry, FetchFrame(session_a.session_uuid, cursor_uuid));
  Require(!fetch_after_detach.accepted &&
              HasDiagnostic(fetch_after_detach, "SERVER.STREAM.DESCRIPTOR_STALE"),
          "DBLC-009 detached session fetched a tombstoned cursor");

  const auto finality_it = registry.finality_by_request_uuid.find(
      scratchbird::server::UuidBytesToText(disconnect_frame.header.request_uuid));
  Require(finality_it != registry.finality_by_request_uuid.end(),
          "DBLC-009 detach finality record missing");
  Require(finality_it->second.state == "detached" &&
              Contains(finality_it->second.detail, "active_transaction_outcome=none"),
          "DBLC-009 detach finality did not preserve engine rollback outcome");

  const auto unknown_uuid = sbps::MakeUuidV7Bytes();
  const auto sessions_before = registry.sessions_by_uuid.size();
  const auto auth_before = registry.auth_contexts_by_uuid.size();
  const auto unknown = scratchbird::server::HandleDisconnectNotice(
      &registry, DisconnectFrame(unknown_uuid, unknown_uuid,
                                 "parser_disconnect_notice"));
  Require(!unknown.accepted, "DBLC-009 unknown session detach was accepted");
  const auto unknown_decoded = DecodeDisconnect(unknown);
  Require(unknown_decoded.outcome == "binding_mismatch",
          "DBLC-009 unknown detach outcome mismatch");
  Require(registry.sessions_by_uuid.size() == sessions_before &&
              registry.auth_contexts_by_uuid.size() == auth_before,
          "DBLC-009 unknown detach mutated unrelated session state");

}

void FinalizeFixtureTransactions(ServerSessionRecord& session,
                                 const AttachedSession& attached,
                                 const std::filesystem::path& database_path,
                                 const std::string& database_uuid) {
  // Finalize each real attach transaction before testing the separate
  // temporary-session cleanup operation. No forged inventory or row fixtures.
  for (const auto& [_, transaction] : session.transactions_by_local_id) {
    api::EngineRollbackTransactionRequest rollback;
    rollback.context = EngineContext(database_path, database_uuid, attached.session_uuid);
    rollback.context.principal_uuid.canonical =
        scratchbird::server::UuidBytesToText(session.effective_user_uuid);
    rollback.context.local_transaction_id = transaction.local_transaction_id;
    rollback.context.transaction_uuid.canonical = transaction.transaction_uuid;
    rollback.context.snapshot_visible_through_local_transaction_id =
        transaction.snapshot_visible_through_local_transaction_id;
    rollback.context.transaction_timestamp = transaction.transaction_timestamp;
    const auto rolled_back = api::EngineRollbackTransaction(rollback);
    Require(rolled_back.ok && rolled_back.engine_finality_known &&
                TransactionHasState(database_path, transaction.local_transaction_id,
                                    tx::TransactionState::rolled_back),
            "temporary cleanup fixture rollback lacks engine inventory authority");
  }
  session.transactions_by_local_id.clear();
  session.default_local_transaction_id = 0;
  session.local_transaction_id = 0;
  session.transaction_uuid.clear();
  session.transaction_timestamp.clear();
  session.snapshot_visible_through_local_transaction_id = 0;
}

void VerifyTemporaryCleanupFailure(const std::filesystem::path& database_path,
                                   const std::string& database_uuid) {
  ServerSessionRegistry registry;
  const auto engine_state = MakeEngineState(database_path, database_uuid);
  const auto attached = AttachAuthenticatedSession(&registry, engine_state);
  const auto sibling = AttachAuthenticatedSession(&registry, engine_state);
  const auto key = scratchbird::server::UuidBytesToText(attached.session_uuid);
  const auto sibling_key = scratchbird::server::UuidBytesToText(sibling.session_uuid);
  auto& session = registry.sessions_by_uuid.at(key);
  FinalizeFixtureTransactions(session, attached, database_path, database_uuid);
  const auto sibling_transaction = registry.sessions_by_uuid.at(sibling_key).local_transaction_id;
  Require(sibling_transaction != 0 &&
              TransactionHasState(database_path, sibling_transaction, tx::TransactionState::active),
          "sibling transaction is not real active inventory");
  const auto before = db::LoadLocalTransactionInventoryFromDatabase(database_path.string());
  Require(before.ok(), "temporary cleanup baseline inventory unavailable");
  const auto missing = database_path.parent_path() / "absent-cleanup-database.sbdb";
  session.database_path = missing.string();
  const auto request = DisconnectFrame(attached.connection_uuid, attached.session_uuid,
                                        "parser_disconnect_notice");
  const auto failed = scratchbird::server::HandleDisconnectNotice(&registry, request);
  const auto payload = DecodeDisconnect(failed);
  Require(payload.outcome == "recovery_quarantined",
          "failed temporary cleanup falsely reported detached");
  Require(scratchbird::parser::ipc::ValidatePrivateDisconnectResult(
              failed.response_message_type, failed.response_schema_id,
              failed.frame_flags, attached.connection_uuid, failed.session_uuid,
              attached.connection_uuid, attached.session_uuid, failed.payload) ==
              scratchbird::parser::ipc::DisconnectResultDisposition::recovery_quarantined,
          "parser disconnect validator did not preserve cleanup quarantine");
  Require(!HasDiagnostic(failed, "ENGINE.DBLC_DETACH_CLEANUP_COMPLETE"),
          "failed temporary cleanup published complete evidence");
  Require(registry.sessions_by_uuid.contains(key) &&
              registry.sessions_by_uuid.at(key).detached_recovery_quarantined &&
              registry.auth_contexts_by_uuid.contains(
                  scratchbird::server::UuidBytesToText(attached.auth_context_uuid)),
          "failed temporary cleanup lost its owning session or authorization");
  Require(Contains(payload.detail, "temporary_cleanup_state=failed"),
          "temporary cleanup failure detail lost");
  Require(!std::filesystem::exists(missing), "cleanup manufactured missing database");
  const auto after = db::LoadLocalTransactionInventoryFromDatabase(database_path.string());
  Require(after.ok() && after.inventory.entries.size() == before.inventory.entries.size() &&
              after.inventory.next_local_transaction_id == before.inventory.next_local_transaction_id &&
              TransactionHasState(database_path, sibling_transaction, tx::TransactionState::active),
          "failed cleanup modified independent inventory or sibling");

  registry.sessions_by_uuid.at(key).database_path = database_path.string();
  const auto recovered = scratchbird::server::HandleDisconnectNotice(
      &registry, DisconnectFrame(attached.connection_uuid, attached.session_uuid,
                                  "parser_disconnect_notice"));
  const auto recovered_payload = DecodeDisconnect(recovered);
  Require(recovered_payload.outcome == "detached" &&
              HasDiagnostic(recovered, "ENGINE.DBLC_DETACH_CLEANUP_COMPLETE") &&
              Contains(recovered_payload.detail, "temporary_cleanup_state=committed"),
          "restored cleanup did not perform an actual committed cleanup");
  Require(!registry.sessions_by_uuid.contains(key) &&
              registry.sessions_by_uuid.contains(sibling_key) &&
              TransactionHasState(database_path, sibling_transaction, tx::TransactionState::active),
          "cleanup retry lost isolation or retained cleaned session");
  const auto final_inventory = db::LoadLocalTransactionInventoryFromDatabase(database_path.string());
  Require(final_inventory.ok() &&
              final_inventory.inventory.next_local_transaction_id > after.inventory.next_local_transaction_id,
          "cleanup retry did not allocate an engine transaction");
  const auto cleanup_id = final_inventory.inventory.next_local_transaction_id - 1;
  Require(TransactionHasState(database_path, cleanup_id, tx::TransactionState::committed),
          "cleanup retry did not commit in independent inventory");
  const auto sibling_cleanup = scratchbird::server::HandleDisconnectNotice(
      &registry, DisconnectFrame(sibling.connection_uuid, sibling.session_uuid,
                                  "parser_disconnect_notice"));
  Require(DecodeDisconnect(sibling_cleanup).outcome == "detached",
          "sibling cleanup failed");
  std::cout << "temporary_cleanup_failure retained=true retry_committed=true sibling_isolated=true\n";
}

void VerifyTemporaryCleanupIdentity(const std::filesystem::path& database_path,
                                    const std::string& database_uuid) {
  using State = api::EngineTransactionInventoryState;
  ServerSessionRegistry registry;
  const auto engine_state = MakeEngineState(database_path, database_uuid);
  const auto attached = AttachAuthenticatedSession(&registry, engine_state);
  const auto key = scratchbird::server::UuidBytesToText(attached.session_uuid);
  auto& session = registry.sessions_by_uuid.at(key);
  FinalizeFixtureTransactions(session, attached, database_path, database_uuid);
  auto context = EngineContext(database_path, database_uuid, attached.session_uuid);
  context.principal_uuid.canonical =
      scratchbird::server::UuidBytesToText(session.effective_user_uuid);
  api::EngineCleanupTemporarySessionRequest request;
  request.context = context;

  auto malformed = request;
  malformed.context.session_uuid.canonical.clear();
  const auto invalid = api::EngineCleanupTemporarySessionState(malformed);
  Require(!invalid.ok && invalid.cleanup_local_transaction_id == 0 &&
              invalid.cleanup_transaction.state == State::not_started &&
              invalid.cleanup_transaction.transaction_uuid == std::array<std::uint8_t,16>{},
          "pre-begin refusal fabricated a cleanup allocation");

  const auto before = db::LoadLocalTransactionInventoryFromDatabase(database_path.string());
  Require(before.ok(), "cleanup identity baseline unavailable");
  Require(::geteuid() != 0, "permission fault requires unprivileged Linux execution");
  const auto permissions = std::filesystem::status(database_path).permissions();
  std::filesystem::permissions(database_path, std::filesystem::perms::owner_read);
  const auto begin_failed = api::EngineCleanupTemporarySessionState(request);
  std::filesystem::permissions(database_path, permissions);
  Require(!begin_failed.ok && begin_failed.cleanup_local_transaction_id != 0 &&
              begin_failed.cleanup_transaction.state == State::unknown &&
              begin_failed.cleanup_transaction.local_transaction_id == before.inventory.next_local_transaction_id &&
              (begin_failed.cleanup_transaction.transaction_uuid[6] & 0xf0) == 0x70 &&
              (begin_failed.cleanup_transaction.transaction_uuid[8] & 0xc0) == 0x80,
          "begin publication failure lost its binary candidate or fabricated active authority");
  const auto after_begin = db::LoadLocalTransactionInventoryFromDatabase(database_path.string());
  Require(after_begin.ok() && after_begin.inventory.next_local_transaction_id == before.inventory.next_local_transaction_id &&
              after_begin.inventory.entries.size() == before.inventory.entries.size(),
          "denied publication changed independent engine inventory");

  request.option_envelopes = {"ipar.fault_injection.point=commit_fence"};
  const auto failed = api::EngineCleanupTemporarySessionState(request);
  Require(!failed.ok && HasDiagnostic(failed, "SB-IPAR-P7-06-COMMIT-FENCE-INJECTED") &&
              failed.cleanup_transaction.state == State::not_applied &&
              failed.cleanup_local_transaction_id == failed.cleanup_transaction.local_transaction_id &&
              failed.cleanup_local_transaction_id != 0 &&
              !failed.cleanup_transaction.post_inventory_secondary_failure,
          "cleanup commit refusal lost exact identity/finality or ignored fault point");
  const auto after = db::LoadLocalTransactionInventoryFromDatabase(database_path.string());
  Require(after.ok(), "cleanup refusal inventory unavailable");
  const auto identity = std::find_if(after.inventory.entries.begin(), after.inventory.entries.end(),
      [&](const auto& entry) {
        return entry.identity.local_id.value == failed.cleanup_transaction.local_transaction_id &&
               entry.identity.transaction_uuid.value.bytes == failed.cleanup_transaction.transaction_uuid;
      });
  Require(identity != after.inventory.entries.end() && identity->state == tx::TransactionState::active &&
              !failed.cleanup_transaction.transaction_timestamp.empty(),
          "cleanup refusal identity does not match independent active inventory");
  Require(identity->identity.transaction_uuid.value.bytes != begin_failed.cleanup_transaction.transaction_uuid,
          "unpublished UUID candidate became later transaction authority by local-id alias");
  Require(failed.temporary_deleted_rows == 0 && failed.temporary_reclaimed_large_values == 0 &&
              failed.temporary_retired_private_metadata == 0,
          "failed cleanup published success counters");

  api::EngineRollbackTransactionRequest rollback;
  rollback.context = context;
  rollback.context.local_transaction_id = failed.cleanup_transaction.local_transaction_id;
  // Text here belongs to this legacy test API boundary, not the new binary
  // observation. The broader EngineUuid API migration remains required.
  rollback.context.transaction_uuid.canonical = uuid::UuidToString(identity->identity.transaction_uuid.value);
  rollback.context.snapshot_visible_through_local_transaction_id =
      failed.cleanup_transaction.snapshot_visible_through_local_transaction_id;
  rollback.context.transaction_timestamp = failed.cleanup_transaction.transaction_timestamp;
  const auto aborted = api::EngineRollbackTransaction(rollback);
  Require(aborted.ok && aborted.engine_finality_known &&
              TransactionHasState(database_path, failed.cleanup_local_transaction_id, tx::TransactionState::rolled_back),
          "retained cleanup selector could not be finalized by engine rollback");
  request.option_envelopes.clear();
  const auto retried = api::EngineCleanupTemporarySessionState(request);
  Require(retried.ok && retried.cleanup_transaction.state == State::committed &&
              retried.cleanup_local_transaction_id > failed.cleanup_local_transaction_id &&
              TransactionHasState(database_path, retried.cleanup_local_transaction_id, tx::TransactionState::committed),
          "cleanup retry did not publish genuine committed inventory");
  const auto before_server_failure =
      db::LoadLocalTransactionInventoryFromDatabase(database_path.string());
  Require(before_server_failure.ok(), "server cleanup baseline unavailable");
  std::filesystem::permissions(database_path, std::filesystem::perms::owner_read);
  const auto refused = scratchbird::server::HandleDisconnectNotice(
      &registry, DisconnectFrame(attached.connection_uuid, attached.session_uuid, "parser_disconnect_notice"));
  std::filesystem::permissions(database_path, permissions);
  Require(DecodeDisconnect(refused).outcome == "recovery_quarantined" &&
              registry.sessions_by_uuid.count(key) == 1,
          "server discarded failed cleanup ownership");
  const auto pending = registry.sessions_by_uuid.at(key).pending_temporary_cleanup_transaction;
  Require(pending != nullptr && pending->state == State::unknown &&
              pending->local_transaction_id == before_server_failure.inventory.next_local_transaction_id &&
              (pending->transaction_uuid[6] & 0xf0) == 0x70,
          "server failed to retain exact binary cleanup candidate");
  const auto repeated = scratchbird::server::HandleDisconnectNotice(
      &registry, DisconnectFrame(attached.connection_uuid, attached.session_uuid, "parser_disconnect_notice"));
  Require(DecodeDisconnect(repeated).outcome == "recovery_quarantined" &&
              registry.sessions_by_uuid.at(key).pending_temporary_cleanup_transaction == pending &&
              registry.sessions_by_uuid.at(key).transactions_by_local_id.empty() &&
              registry.sessions_by_uuid.at(key).local_transaction_id == 0,
          "repeat disconnect replaced uncertain cleanup identity or adopted it as active");
  const auto after_server_retry =
      db::LoadLocalTransactionInventoryFromDatabase(database_path.string());
  Require(after_server_retry.ok() &&
              after_server_retry.inventory.next_local_transaction_id == before_server_failure.inventory.next_local_transaction_id &&
              after_server_retry.inventory.entries.size() == before_server_failure.inventory.entries.size(),
          "repeat disconnect began a replacement before engine recovery");
  std::cout << "temporary_cleanup_identity binary=true begin_unknown=true commit_not_applied=true rollback=true retry_committed=true server_retained=true\n";
}

void VerifyMetadataReadFailure(const std::filesystem::path& database_path,
                               const std::string& database_uuid,
                               bool canonical_savepoint_effect = false) {
  using State = api::EngineTransactionInventoryState;
  ServerSessionRegistry registry;
  const auto attached = AttachAuthenticatedSession(
      &registry, MakeEngineState(database_path, database_uuid));
  const auto key = scratchbird::server::UuidBytesToText(attached.session_uuid);
  auto& session = registry.sessions_by_uuid.at(key);
  FinalizeFixtureTransactions(session, attached, database_path, database_uuid);
  auto context = EngineContext(database_path, database_uuid, attached.session_uuid);
  context.principal_uuid.canonical =
      scratchbird::server::UuidBytesToText(session.effective_user_uuid);
  const auto name = [](const std::string& text) {
    api::EngineLocalizedName result;
    result.language_tag = "en";
    result.name_class = "primary";
    result.name = result.raw_name_text = result.display_name = text;
    result.default_name = true;
    return result;
  };
  api::EngineBeginTransactionRequest begin;
  begin.context = context;
  begin.isolation_level = "read_committed";
  const auto begun = api::EngineBeginTransaction(begin);
  Require(begun.ok, "metadata I/O fixture begin failed");
  auto create_context = context;
  create_context.local_transaction_id = begun.local_transaction_id;
  create_context.transaction_uuid = begun.transaction_uuid;
  create_context.snapshot_visible_through_local_transaction_id = begun.snapshot_visible_through_local_transaction_id;
  api::EngineCreateSchemaRequest schema;
  schema.context = create_context;
  schema.localized_names.push_back(name("cleanup_io_schema"));
  const auto created_schema = api::EngineCreateSchema(schema);
  Require(created_schema.ok, "metadata I/O fixture schema creation failed");
  api::EngineCreateTableRequest table;
  table.context = create_context;
  // Exact current built-in datatype receipt admitted by the Core registry.
  table.context.datatype_catalog_snapshot_uuid.canonical = "019d0000-0000-7000-8000-00000000d701";
  table.context.datatype_catalog_generation = 1;
  table.context.datatype_registry_generation = 1;
  table.target_schema = created_schema.primary_object;
  table.table_names.push_back(name("cleanup_io_temporary"));
  api::EngineColumnDefinition column;
  column.names.push_back(name("id"));
  column.descriptor.descriptor_kind = "scalar";
  column.descriptor.canonical_type_name = "int64";
  column.descriptor.encoded_descriptor = "type=int64";
  column.nullable = false;
  table.table_columns.push_back(column);
  table.option_envelopes = {"temporary:true", "temporary_scope:private", "on_commit:preserve_rows"};
  const auto created_table = api::EngineCreateTable(table);
  if (!created_table.ok) {
    for (const auto& diagnostic : created_table.diagnostics)
      std::cerr << diagnostic.code << ':' << diagnostic.message_key << ':' << diagnostic.detail << '\n';
  }
  Require(created_table.ok, "metadata I/O fixture table creation failed");
  const auto actual_descriptor = api::LoadMgaRelationStorageDescriptor(
      table.context, created_table.table_object.uuid.canonical);
  Require(actual_descriptor.ok, "actual created relation descriptor missing");
  const auto original_descriptor_fields =
      api::SerializeMgaRelationStorageDescriptor(actual_descriptor.descriptor);
  Require(!api::PersistDescriptorFields(table.context,
      created_table.table_object.uuid.canonical, original_descriptor_fields).error,
      "actual descriptor sidecar publication failed");
  api::EngineCreateSavepointRequest cache_savepoint;
  cache_savepoint.context = table.context;
  cache_savepoint.option_envelopes = {"savepoint_name:cache_savepoint"};
  if (!canonical_savepoint_effect) {
    Require(api::EngineCreateSavepoint(cache_savepoint).ok,
            "actual savepoint cache fixture creation failed");
  }
  api::SblrSavepointCoordinatorResult canonical_savepoint;
  auto canonical_context = table.context;
  scratchbird::engine::sblr::SblrSavepointHandleV1 canonical_handle;
  if (canonical_savepoint_effect) {
    // A trusted component caller exercises the production coordinator over
    // real engine-created state. This is not a parser/IPC admission proof.
    canonical_context.statement_uuid.canonical =
        scratchbird::server::UuidBytesToText(sbps::MakeUuidV7Bytes());
    canonical_context.statement_metadata_snapshot_engine_owned = true;
    canonical_context.trace_tags.push_back("private_savepoint_coordination");
    const auto reserved = api::ReserveSblrSavepoint(canonical_context,
        canonical_context.statement_uuid.canonical,
        api::Sha256Tagged("component_transaction:" + begun.transaction_uuid.canonical +
                         ":" + std::to_string(begun.local_transaction_id)),
        1, api::Sha256Tagged("component_savepoint_symbol"));
    Require(reserved.ok, "canonical component savepoint reservation failed");
    canonical_savepoint = api::ActivateSblrSavepoint(canonical_context,
        canonical_context.statement_uuid.canonical, reserved.snapshot.descriptor_uuid,
        reserved.snapshot.descriptor_generation, reserved.snapshot.descriptor_evidence_sha256, 1);
    Require(canonical_savepoint.ok, "canonical component savepoint activation failed");
    const auto savepoint_uuid = uuid::ParseUuid(canonical_savepoint.snapshot.savepoint_uuid);
    const auto transaction_uuid = uuid::ParseUuid(begun.transaction_uuid.canonical);
    Require(savepoint_uuid.ok() && transaction_uuid.ok(), "canonical savepoint binary identity missing");
    canonical_handle.savepoint_uuid = savepoint_uuid.value.bytes;
    canonical_handle.transaction_uuid = transaction_uuid.value.bytes;
    canonical_handle.savepoint_generation = canonical_savepoint.snapshot.savepoint_generation;
    canonical_handle.local_transaction_id = begun.local_transaction_id;
    canonical_handle.transaction_ordinal = canonical_savepoint.snapshot.transaction_ordinal;
    canonical_handle.stack_generation = canonical_savepoint.snapshot.stack_generation;
    canonical_handle.executor_availability_generation = 1;
    const auto encoded = scratchbird::engine::sblr::EncodeSblrSavepointHandleV1(canonical_handle);
    std::string detail;
    Require(!encoded.empty() && scratchbird::engine::sblr::DecodeSblrSavepointHandleV1(
        encoded.data(), encoded.size(), &canonical_handle, &detail),
        "canonical savepoint handle evidence roundtrip failed");
  }
  // Ordinary mutations retain session authority even under a live savepoint.
  // These are admission checks; the following real INSERT/rollback still
  // supplies the storage-effect oracle and is not replaced by them.
  for (const auto mutation : {api::MgaDmlMutationKind::insert,
       api::MgaDmlMutationKind::update, api::MgaDmlMutationKind::delete_rows}) {
    Require(!api::AdmitMgaDmlSavepointMutation(table.context,
                created_table.table_object.uuid.canonical, mutation, true).error,
            "valid private temporary row mutation admission failed");
    for (const auto& session_identity : {std::string{}, std::string("invalid-session"),
         scratchbird::server::UuidBytesToText(sbps::MakeUuidV7Bytes())}) {
      auto foreign_context = table.context;
      foreign_context.session_uuid.canonical = session_identity;
      Require(api::AdmitMgaDmlSavepointMutation(foreign_context,
                  created_table.table_object.uuid.canonical, mutation, true).error,
              "temporary mutation admitted missing malformed or foreign session");
    }
  }
  api::EngineInsertRowsRequest insert;
  insert.context = table.context;
  insert.target_table = created_table.table_object;
  insert.option_envelopes = {"large_value.force_toast=true"};
  api::EngineTypedValue value;
  value.descriptor = column.descriptor;
  value.encoded_value = "17";
  api::EngineRowValue row;
  row.fields.push_back({"id", value});
  insert.input_rows.push_back(std::move(row));
  value.encoded_value = "18";
  api::EngineRowValue second_row;
  second_row.fields.push_back({"id", value});
  insert.input_rows.push_back(std::move(second_row));
  const auto inserted = api::EngineInsertRows(insert);
  if (!inserted.ok) {
    for (const auto& diagnostic : inserted.diagnostics) {
      std::cerr << "savepoint fixture insert refused code=" << diagnostic.code
                << " detail=" << diagnostic.detail << '\n';
    }
  }
  Require(inserted.ok && inserted.inserted_count == 2,
          "metadata I/O fixture did not create two real temporary rows");
  const auto stored = api::LoadMgaRelationStoreState(table.context);
  Require(stored.ok && inserted.row_uuids.size() == 2,
          "real large-value row inventory unavailable");
  const auto stored_row = std::find_if(stored.state.row_versions.begin(), stored.state.row_versions.end(),
      [&](const auto& r) { return r.row_uuid == inserted.row_uuids.front().canonical && !r.deleted; });
  Require(stored_row != stored.state.row_versions.end(), "actual large-value row version missing");
  const auto stored_second_row = std::find_if(stored.state.row_versions.begin(), stored.state.row_versions.end(),
      [&](const auto& r) { return r.row_uuid == inserted.row_uuids[1].canonical && !r.deleted; });
  Require(stored_second_row != stored.state.row_versions.end(), "second actual large-value row version missing");
  if (canonical_savepoint_effect) {
    api::EngineSelectRowsRequest select;
    select.context = table.context;
    select.source_object = created_table.table_object;
    const auto before_visible = api::EngineSelectRows(select);
    Require(before_visible.ok && before_visible.visible_count == 2,
            "canonical savepoint fixture did not expose both inserted rows");
    const auto rolled = api::RollbackToSblrSavepoint(canonical_context,
        canonical_savepoint.snapshot.savepoint_uuid, canonical_handle.savepoint_generation,
        canonical_handle.transaction_ordinal, canonical_handle.stack_generation,
        "sha256:" + scratchbird::core::hash::HexLower(canonical_handle.savepoint_evidence_sha256), 1);
    const auto after = api::LoadMgaRelationStoreState(table.context);
    const auto after_visible = api::EngineSelectRows(select);
    const auto remaining = std::count_if(after.state.row_versions.begin(), after.state.row_versions.end(),
        [&](const auto& candidate) {
          return candidate.table_uuid == created_table.table_object.uuid.canonical &&
                 std::any_of(inserted.row_uuids.begin(), inserted.row_uuids.end(),
                   [&](const auto& identity) { return identity.canonical == candidate.row_uuid; });
        });
    std::cout << "canonical_savepoint_rollback ok=" << rolled.ok << " before_rows=2 after_rows="
              << remaining << " relation_read_ok=" << after.ok
              << " before_visible=" << before_visible.visible_count
              << " after_visible=" << after_visible.visible_count
              << " select_ok=" << after_visible.ok << '\n';
    // Resolve the actual outer transaction even when the effect oracle fails.
    api::EngineRollbackTransactionRequest outer;
    outer.context = create_context;
    const auto outer_rollback = api::EngineRollbackTransaction(outer);
    Require(outer_rollback.ok && TransactionHasState(database_path, begun.local_transaction_id,
                                                   tx::TransactionState::rolled_back),
            "canonical savepoint fixture outer rollback failed");
    Require(rolled.ok && after.ok && after_visible.ok && after_visible.visible_count == 0,
            "canonical savepoint reported success without undoing actual post-boundary rows");
    return;
  }
  api::EngineCommitTransactionRequest commit;
  commit.context = create_context;
  Require(api::EngineCommitTransaction(commit).ok, "metadata I/O fixture commit failed");
  // Exercise the storage reclaim boundary with the actual engine-created
  // row/version and large-value companion, then roll back the test reclaim.
  // No marker is fabricated and no committed live row loses its payload.
  const auto reclaim_begun = api::EngineBeginTransaction(begin);
  Require(reclaim_begun.ok, "reclaim retry fixture begin failed");
  auto reclaim_context = context;
  reclaim_context.local_transaction_id = reclaim_begun.local_transaction_id;
  reclaim_context.transaction_uuid = reclaim_begun.transaction_uuid;
  reclaim_context.snapshot_visible_through_local_transaction_id = reclaim_begun.snapshot_visible_through_local_transaction_id;
  const auto reclaim_path = database_path.string() + ".sb.mga_large_values";
  const auto reclaim_permissions = std::filesystem::status(reclaim_path).permissions();
  const auto before_append_size = std::filesystem::file_size(reclaim_path);
  std::set<std::string> reclaimed_ids;
  std::uint64_t reclaimed_count = 0;
  std::filesystem::permissions(reclaim_path, std::filesystem::perms::owner_read);
  const auto append_denied = api::AppendMgaLargeValueReclaimMarkersForRowVersion(
      reclaim_context, reclaim_context.local_transaction_id, *stored_row,
      "temporary_session_cleanup", &reclaimed_ids, &reclaimed_count);
  std::filesystem::permissions(reclaim_path, reclaim_permissions);
  Require(append_denied.error && reclaimed_ids.empty() && reclaimed_count == 0 &&
              std::filesystem::file_size(reclaim_path) == before_append_size,
          "failed reclaim append poisoned deduplication state or success counters");
  const auto append_retry = api::AppendMgaLargeValueReclaimMarkersForRowVersion(
      reclaim_context, reclaim_context.local_transaction_id, *stored_row,
      "temporary_session_cleanup", &reclaimed_ids, &reclaimed_count);
  Require(!append_retry.error && reclaimed_ids.size() == 1 && reclaimed_count == 1 &&
              std::filesystem::file_size(reclaim_path) > before_append_size,
          "same-state reclaim retry skipped an unpersisted marker");
  const auto visible_reclaim = api::LoadVisibleMgaLargeValueReclaims(reclaim_context);
  Require(!visible_reclaim.diagnostic.error && visible_reclaim.overflow_uuids == reclaimed_ids,
          "reclaim retry lacks independently loaded marker authority");
  const auto after_retry_size = std::filesystem::file_size(reclaim_path);
  const auto duplicate_reclaim = api::AppendMgaLargeValueReclaimMarkersForRowVersion(
      reclaim_context, reclaim_context.local_transaction_id, *stored_row,
      "temporary_session_cleanup", &reclaimed_ids, &reclaimed_count);
  Require(!duplicate_reclaim.error && reclaimed_count == 1 &&
              std::filesystem::file_size(reclaim_path) == after_retry_size,
          "successful reclaim replay duplicated marker or count");
  const auto first_reclaimed_ids = reclaimed_ids;
  std::filesystem::permissions(reclaim_path, std::filesystem::perms::owner_read);
  const auto partial_batch_denied = api::AppendMgaLargeValueReclaimMarkersForRowVersion(
      reclaim_context, reclaim_context.local_transaction_id, *stored_second_row,
      "temporary_session_cleanup", &reclaimed_ids, &reclaimed_count);
  std::filesystem::permissions(reclaim_path, reclaim_permissions);
  Require(partial_batch_denied.error && reclaimed_ids == first_reclaimed_ids &&
              reclaimed_count == 1 && std::filesystem::file_size(reclaim_path) == after_retry_size,
          "late batch append failure poisoned or discarded prior actual marker authority");
  const auto partial_batch_retry = api::AppendMgaLargeValueReclaimMarkersForRowVersion(
      reclaim_context, reclaim_context.local_transaction_id, *stored_second_row,
      "temporary_session_cleanup", &reclaimed_ids, &reclaimed_count);
  const auto complete_reclaims = api::LoadVisibleMgaLargeValueReclaims(reclaim_context);
  Require(!partial_batch_retry.error && reclaimed_count == 2 && reclaimed_ids.size() == 2 &&
              !complete_reclaims.diagnostic.error && complete_reclaims.overflow_uuids == reclaimed_ids,
          "late batch same-state retry did not publish both exact markers");
  api::EngineRollbackTransactionRequest reclaim_rollback;
  reclaim_rollback.context = reclaim_context;
  Require(api::EngineRollbackTransaction(reclaim_rollback).ok &&
              TransactionHasState(database_path, reclaim_context.local_transaction_id, tx::TransactionState::rolled_back),
          "reclaim retry fixture rollback failed");
  const auto rolled_back_reclaims = api::LoadVisibleMgaLargeValueReclaims(context);
  Require(!rolled_back_reclaims.diagnostic.error && rolled_back_reclaims.overflow_uuids.empty(),
          "rolled-back reclaim marker became committed large-value authority");
  const auto work = api::HasMgaTemporaryCleanupMetadataWork(context, true, true, true);
  Require(work.ok && work.has_work, "real temporary metadata was not published");
  const auto warm_snapshot = api::LoadMgaMetadataSnapshot(context);
  Require(warm_snapshot.snapshot != nullptr && !warm_snapshot.diagnostic.error,
          "real metadata snapshot was not published");
  const auto metadata_path = database_path.string() + ".sb.mga_relation_metadata";
  std::vector<scratchbird::core::index::byte> metadata_bytes;
  Require(api::ReadCompleteMgaBinaryFile(metadata_path,&metadata_bytes) && !metadata_bytes.empty(),
          "metadata cache fixture bytes missing");
  const auto metadata_time=std::filesystem::last_write_time(metadata_path);
  const auto write_same_metadata_identity=[&](const auto& bytes) {
    std::ofstream output(metadata_path,std::ios::binary|std::ios::trunc);
    output.write(reinterpret_cast<const char*>(bytes.data()),static_cast<std::streamsize>(bytes.size()));
    output.close();Require(!output.fail(),"metadata cache fixture write failed");
    std::filesystem::last_write_time(metadata_path,metadata_time);
  };
  const auto hex_name=[](std::string_view value) {
    std::string result;constexpr char digits[]="0123456789abcdef";
    for(unsigned char ch:value) {result.push_back(digits[ch>>4]);result.push_back(digits[ch&15]);}
    return result;
  };
  const auto original_name=hex_name("cleanup_io_temporary");
  const auto changed_name=hex_name("cleanup_io_temporarx");
  std::string changed_metadata(metadata_bytes.begin(),metadata_bytes.end());
  const auto name_offset=changed_metadata.find(original_name);
  Require(name_offset!=std::string::npos,"actual engine-created metadata name not found");
  changed_metadata.replace(name_offset,original_name.size(),changed_name);
  Require(changed_metadata.size()==metadata_bytes.size(),"same-size metadata fixture changed length");
  write_same_metadata_identity(changed_metadata);
  const auto changed_snapshot=api::LoadMgaMetadataSnapshot(context);
  const auto has_name=[&](const auto& snapshot,std::string_view expected) {
    return snapshot && std::any_of(snapshot->tables.begin(),snapshot->tables.end(),[&](const auto& item) {
      return item.table_uuid==created_table.table_object.uuid.canonical && item.default_name==expected;
    });
  };
  Require(changed_snapshot.ok() && has_name(changed_snapshot.snapshot,"cleanup_io_temporarx"),
          "same-size same-mtime metadata rewrite reused stale cached values");
  Require(has_name(warm_snapshot.snapshot,"cleanup_io_temporary"),
          "content refresh mutated the prior immutable metadata snapshot");
  write_same_metadata_identity(metadata_bytes);
  const auto restored_snapshot=api::LoadMgaMetadataSnapshot(context);
  Require(restored_snapshot.ok() && has_name(restored_snapshot.snapshot,"cleanup_io_temporary"),
          "restored metadata bytes did not restore exact original values");
  const auto rewrite_same_identity = [&](const std::string& path, const auto& bytes,
                                          const auto& original_time) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    output.close();
    Require(!output.fail(), "cache dependency fixture rewrite failed");
    std::filesystem::last_write_time(path, original_time);
  };
  const auto descriptor_path = database_path.string() + ".sb.mga_relation_descriptors";
  std::vector<scratchbird::core::index::byte> descriptor_bytes;
  Require(api::ReadCompleteMgaBinaryFile(descriptor_path, &descriptor_bytes) &&
              !descriptor_bytes.empty(), "actual descriptor sidecar bytes missing");
  const auto descriptor_time = std::filesystem::last_write_time(descriptor_path);
  const auto warm_descriptors = api::LoadDescriptorFieldsSnapshot(context);
  const auto relation_uuid = created_table.table_object.uuid.canonical;
  Require(warm_descriptors && warm_descriptors->at(relation_uuid) == original_descriptor_fields,
          "descriptor warm-up lost actual created fields");
  auto altered_descriptor = actual_descriptor.descriptor;
  Require(!altered_descriptor.columns.empty() &&
              altered_descriptor.columns.front().canonical_name_key == "id",
          "actual descriptor column fixture missing");
  altered_descriptor.columns.front().canonical_name_key = "ix";
  const auto altered_fields = api::SerializeMgaRelationStorageDescriptor(altered_descriptor);
  std::string altered_bytes(descriptor_bytes.begin(), descriptor_bytes.end());
  const auto original_pairs = api::EncodeCrudPairs(original_descriptor_fields);
  const auto altered_pairs = api::EncodeCrudPairs(altered_fields);
  const auto pairs_offset = altered_bytes.find(original_pairs);
  Require(pairs_offset != std::string::npos && original_pairs.size() == altered_pairs.size(),
          "same-size descriptor replacement fixture invalid");
  altered_bytes.replace(pairs_offset, original_pairs.size(), altered_pairs);
  rewrite_same_identity(descriptor_path, altered_bytes, descriptor_time);
  const auto changed_descriptors = api::LoadDescriptorFieldsSnapshot(context);
  const auto descriptor_changed_metadata = api::LoadMgaMetadataSnapshot(context);
  Require(changed_descriptors && changed_descriptors->at(relation_uuid) == altered_fields &&
              warm_descriptors->at(relation_uuid) == original_descriptor_fields,
          "descriptor same-identity rewrite reused or mutated cached authority");
  Require(descriptor_changed_metadata.ok() &&
              descriptor_changed_metadata.key.descriptor_content_sha256 !=
                  restored_snapshot.key.descriptor_content_sha256 &&
              descriptor_changed_metadata.snapshot != restored_snapshot.snapshot,
          "metadata cache failed to bind descriptor dependency content");
  rewrite_same_identity(descriptor_path, descriptor_bytes, descriptor_time);
  const auto restored_descriptors = api::LoadDescriptorFieldsSnapshot(context);
  Require(restored_descriptors && restored_descriptors->at(relation_uuid) == original_descriptor_fields,
          "restored descriptor bytes did not restore exact original fields");
  Require(!api::PersistDescriptorFields(context, relation_uuid, original_descriptor_fields).error,
          "descriptor append fixture failed");
  const auto after_descriptor_append = api::LoadDescriptorFieldsSnapshot(context);
  Require(after_descriptor_append && after_descriptor_append->at(relation_uuid) == original_descriptor_fields &&
              after_descriptor_append != restored_descriptors &&
              changed_descriptors->at(relation_uuid) == altered_fields,
          "descriptor append warmed unproved content or mutated held snapshots");

  const auto savepoint_path = database_path.string() + ".sb.mga_savepoints";
  std::vector<scratchbird::core::index::byte> savepoint_bytes;
  Require(api::ReadCompleteMgaBinaryFile(savepoint_path, &savepoint_bytes) && !savepoint_bytes.empty(),
          "actual savepoint bytes missing");
  const auto savepoint_time = std::filesystem::last_write_time(savepoint_path);
  const auto before_savepoint_change = api::LoadMgaMetadataSnapshot(context);
  std::string changed_savepoint(savepoint_bytes.begin(), savepoint_bytes.end());
  bool replaced_savepoint_name = false;
  for (std::size_t at = 0; at < changed_savepoint.size();) {
    const auto remaining = std::string_view(changed_savepoint).substr(at);
    const auto frame_size = api::MgaSavepointMarkerFrameSize(remaining);
    Require(frame_size != 0 && frame_size != UINT32_MAX && frame_size <= remaining.size(),
            "actual savepoint frame boundary invalid");
    api::MgaSavepointMarkerRecord marker;
    Require(api::DecodeMgaSavepointMarker(remaining.substr(0, frame_size), &marker),
            "actual savepoint frame could not be decoded");
    if (!marker.uuid_identity && marker.identity == "cache_savepoint") {
      marker.identity = "cache_savepoinx";
      const auto replacement = api::EncodeMgaSavepointMarker(marker);
      Require(replacement.size() == frame_size,
              "savepoint cache rewrite changed frame length");
      changed_savepoint.replace(at, frame_size, replacement);
      replaced_savepoint_name = true;
    }
    at += frame_size;
  }
  Require(replaced_savepoint_name, "actual savepoint name not found");
  rewrite_same_identity(savepoint_path, changed_savepoint, savepoint_time);
  const auto after_savepoint_change = api::LoadMgaMetadataSnapshot(context);
  Require(before_savepoint_change.ok() && after_savepoint_change.ok() &&
              after_savepoint_change.key.savepoint_content_sha256 !=
                  before_savepoint_change.key.savepoint_content_sha256 &&
              after_savepoint_change.snapshot != before_savepoint_change.snapshot,
          "same-size same-time savepoint change reused old metadata generation");
  rewrite_same_identity(savepoint_path, savepoint_bytes, savepoint_time);
  const auto after_savepoint_restore = api::LoadMgaMetadataSnapshot(context);
  Require(after_savepoint_restore.ok() &&
              after_savepoint_restore.key.savepoint_content_sha256 ==
                  before_savepoint_change.key.savepoint_content_sha256,
          "savepoint restoration did not restore exact cache identity");

  // A previously admitted snapshot never authorizes reuse through a current
  // dangling link, nonregular file, denied read, or partial final record.
  for (const auto& path : {metadata_path, descriptor_path, savepoint_path}) {
    const auto saved = path + ".cache_admission_saved";
    std::filesystem::rename(path, saved);
    std::filesystem::create_symlink(path + ".absent_target", path);
    std::vector<std::string> records{"unproved"};
    Require(!api::ReadCompleteMgaTextRecords(path, &records) && records.empty() &&
                !api::LoadMgaMetadataSnapshot(context).ok(), "dangling dependency reused metadata cache");
    if (path == savepoint_path) {
      api::BoundedScopedRowReadControl control;
      api::SavepointParsedState savepoints;
      savepoints.active_savepoints[1]["unproved"] = {};
      Require(!api::ParseSavepointsBounded(context, &control, 0, &savepoints) &&
                  savepoints.active_savepoints.empty() && savepoints.rollback_ranges.empty(),
              "dangling savepoint authority became a successful empty rollback state");
    }
    std::filesystem::remove(path);
    std::filesystem::create_directory(path);
    Require(!api::LoadMgaMetadataSnapshot(context).ok(), "nonregular dependency reused metadata cache");
    std::filesystem::remove(path);
    std::filesystem::rename(saved, path);
    const auto old_permissions = std::filesystem::status(path).permissions();
    std::filesystem::permissions(path, std::filesystem::perms::none);
    Require(!api::LoadMgaMetadataSnapshot(context).ok(), "denied dependency reused metadata cache");
    std::filesystem::permissions(path, old_permissions);
    std::vector<scratchbird::core::index::byte> complete_bytes;
    Require(api::ReadCompleteMgaBinaryFile(path, &complete_bytes) && !complete_bytes.empty(),
            "cache dependency original bytes missing");
    const auto complete_time = std::filesystem::last_write_time(path);
    std::filesystem::resize_file(path, complete_bytes.size() - 1);
    Require(!api::LoadMgaMetadataSnapshot(context).ok(), "partial dependency reused metadata cache");
    rewrite_same_identity(path, complete_bytes, complete_time);
    Require(api::LoadMgaMetadataSnapshot(context).ok(),
            "restored dependency failed cache admission");
  }
  const auto probe_begun = api::EngineBeginTransaction(begin);
  Require(probe_begun.ok, "savepoint read probe transaction failed");
  auto probe_context = context;
  probe_context.local_transaction_id = probe_begun.local_transaction_id;
  probe_context.transaction_uuid = probe_begun.transaction_uuid;
  probe_context.snapshot_visible_through_local_transaction_id =
      probe_begun.snapshot_visible_through_local_transaction_id;
  api::EngineCreateSavepointRequest probe_create;
  probe_create.context = probe_context;
  probe_create.option_envelopes = {"savepoint_name:savepoint_probe"};
  std::vector<scratchbird::core::index::byte> probe_prefix;
  Require(api::ReadCompleteMgaBinaryFile(savepoint_path, &probe_prefix),
          "savepoint prefix read failed");
  Require(api::EngineCreateSavepoint(probe_create).ok, "real probe savepoint creation failed");
  std::vector<scratchbird::core::index::byte> probe_bytes;
  Require(api::ReadCompleteMgaBinaryFile(savepoint_path, &probe_bytes) && !probe_bytes.empty(),
          "real probe savepoint bytes missing");
  Require(probe_bytes.size() > probe_prefix.size() &&
              std::equal(probe_prefix.begin(), probe_prefix.end(), probe_bytes.begin()),
          "savepoint append replaced prior authority");
  const std::string probe_frame(probe_bytes.begin() + probe_prefix.size(), probe_bytes.end());
  api::MgaSavepointMarkerRecord probe_marker;
  Require(api::DecodeMgaSavepointMarker(probe_frame, &probe_marker) &&
              probe_marker.kind == 1 && !probe_marker.uuid_identity &&
              probe_marker.identity == "savepoint_probe" &&
              probe_marker.transaction == probe_context.local_transaction_id,
          "savepoint writer did not publish the actual binary marker");
  const auto probe_time = std::filesystem::last_write_time(savepoint_path);
  const auto probe_names = api::ActiveMgaSavepointNames(probe_context);
  Require(!probe_names.diagnostic.error && probe_names.names == std::vector<std::string>{"savepoint_probe"},
          "real probe savepoint name not published");
  const auto function_package = scratchbird::engine::functions::BuildStandardFunctionSeedPackage();
  const auto call_savepoint_active = [&](const api::MgaSavepointNamesResult& names) {
    scratchbird::engine::functions::FunctionCallRequest request;
    request.context.function_id = "sb.session.savepoint_active";
    request.context.security_allowed = request.context.policy_allowed = true;
    request.context.engine_request_context = &probe_context;
    request.context.sblr_context.transaction_context_present = true;
    request.context.sblr_context.local_transaction_id = probe_context.local_transaction_id;
    request.context.sblr_context.transaction_uuid = probe_context.transaction_uuid.canonical;
    request.context.sblr_context.active_savepoint_names = names.names;
    if (names.diagnostic.error) request.context.sblr_context.savepoint_authority_diagnostic = {
        names.diagnostic.code, names.diagnostic.message_key, names.diagnostic.detail,
        scratchbird::engine::sblr::SblrDiagnosticSeverity::error, {}};
    return scratchbird::engine::functions::DispatchFunctionCall(function_package.registry, request).result;
  };
  const auto active_function = call_savepoint_active(probe_names);
  Require(active_function.ok() && active_function.scalar_values.size() == 1 &&
              active_function.scalar_values.front().int64_value == 1,
          "real savepoint function positive control failed");
  Require(api::LoadMgaMetadataSnapshot(probe_context).ok(), "probe metadata warm-up failed");
  const auto require_savepoint_refusal = [&] {
    const auto parsed = api::ParseSavepoints(probe_context);
    Require(parsed.diagnostic.error && parsed.active_savepoints.empty() && parsed.rollback_ranges.empty(),
            "failed savepoint read fabricated rollback or active authority");
    api::BoundedScopedRowReadControl control;
    api::SavepointParsedState bounded;
    bounded.active_savepoints[1]["unproved"] = {};
    Require(!api::ParseSavepointsBounded(probe_context, &control, 0, &bounded) &&
                bounded.diagnostic.error && bounded.active_savepoints.empty() && bounded.rollback_ranges.empty(),
            "failed bounded savepoint read published partial authority");
    const auto names = api::ActiveMgaSavepointNames(probe_context);
    Require(names.diagnostic.error && names.names.empty(), "failed savepoint read reported no active savepoints");
    const auto function = call_savepoint_active(names);
    Require(!function.ok() && function.scalar_values.empty() && !function.diagnostics.empty() &&
                function.diagnostics.front().diagnostic_id == names.diagnostic.code,
            "savepoint_active converted actual read failure into false or null success");
    Require(!api::LoadMgaMetadataSnapshot(probe_context).ok(), "warm metadata cache concealed failed savepoint parse");
    const auto relation = api::LoadMgaRelationStoreState(probe_context);
    Require(!relation.ok && relation.diagnostic.error, "relation reader admitted invalid savepoint state");
    Require(!api::EngineCreateSavepoint(probe_create).ok, "create savepoint appended through invalid prior authority");
    api::EngineReleaseSavepointRequest release;
    release.context = probe_context;
    release.option_envelopes = probe_create.option_envelopes;
    Require(!api::EngineReleaseSavepoint(release).ok, "release savepoint succeeded through invalid authority");
    api::EngineRollbackToSavepointRequest rollback;
    rollback.context = probe_context;
    rollback.option_envelopes = probe_create.option_envelopes;
    Require(!api::EngineRollbackToSavepoint(rollback).ok, "rollback savepoint succeeded through invalid authority");
  };
  const std::vector<std::string> malformed_savepoints{
      "\n", "wrong\tSAVEPOINT\t1\t61\t0\t0\t0\n",
      "SBMGA1\tUNKNOWN\t1\t61\t0\t0\t0\n",
      "SBMGA1\tSAVEPOINT\t1\t61\t0\n",
      "SBMGA1\tSAVEPOINT\t1x\t61\t0\t0\t0\n",
      "SBMGA1\tSAVEPOINT\t0\t61\t0\t0\t0\n",
      "SBMGA1\tSAVEPOINT\t01\t61\t0\t0\t0\n",
      "SBMGA1\tSAVEPOINT\t18446744073709551616\t61\t0\t0\t0\n",
      "SBMGA1\tSAVEPOINT\t1\t6x\t0\t0\t0\n",
      "SBMGA1\tSAVEPOINT\t1\t6\t0\t0\t0\n",
      "SBMGA1\tSAVEPOINT\t1\t\t0\t0\t0\n",
      "SBMGA1\tSAVEPOINT\t1\t61\t0\t0\t0\textra\n",
      "SBMGA1\tROLLBACK_TO_SAVEPOINT\t1\t61\t2\t2\t2\t1\t1\t1\n",
      "SBMGA1\tDML_UPDATE_STATEMENT_SAVEPOINT_CREATE_V1\t1\t61\t0\t0\t0\n",
      "SBMGA1\tSAVEPOINT\t1\t61\t0\t0\t0"};
  // The metadata cache and bounded/unbounded readers must all consume the
  // real binary frame, not a newline projection or an old warm snapshot.
  for (std::size_t extent = 1; extent < probe_frame.size(); ++extent) {
    std::vector<scratchbird::core::index::byte> truncated(
        probe_bytes.begin(), probe_bytes.begin() + probe_prefix.size() + extent);
    rewrite_same_identity(savepoint_path, truncated, probe_time);
    require_savepoint_refusal();
  }
  for (std::size_t offset = 0; offset < probe_frame.size(); ++offset) {
    auto corrupted = probe_bytes;
    corrupted[probe_prefix.size() + offset] ^= 1;
    rewrite_same_identity(savepoint_path, corrupted, probe_time);
    require_savepoint_refusal();
  }
  rewrite_same_identity(savepoint_path, probe_bytes, probe_time);
  Require(api::LoadMgaMetadataSnapshot(probe_context).ok(),
          "restored binary marker did not restore metadata admission");
  for (const auto& invalid : malformed_savepoints) {
    std::string corrupted(probe_bytes.begin(), probe_bytes.end());
    corrupted += invalid;
    rewrite_same_identity(savepoint_path, corrupted, probe_time);
    require_savepoint_refusal();
    std::vector<scratchbird::core::index::byte> retained;
    Require(api::ReadCompleteMgaBinaryFile(savepoint_path, &retained) &&
                std::string(retained.begin(), retained.end()) == corrupted,
            "refused savepoint operation changed retained authority bytes");
  }
  rewrite_same_identity(savepoint_path, probe_bytes, probe_time);
  const auto binary_savepoint_path = database_path.string() + ".sb.mga_update_statement_savepoints.v1";
  Require(!std::filesystem::exists(binary_savepoint_path), "binary probe path already exists");
  std::filesystem::create_symlink(binary_savepoint_path + ".missing", binary_savepoint_path);
  require_savepoint_refusal();
  std::filesystem::remove(binary_savepoint_path);
  std::filesystem::create_directories(binary_savepoint_path + "/invalid-entry.dups");
  require_savepoint_refusal();
  std::filesystem::remove(binary_savepoint_path + "/invalid-entry.dups");
  std::filesystem::remove(binary_savepoint_path);
  const auto restored_probe_names = api::ActiveMgaSavepointNames(probe_context);
  Require(!restored_probe_names.diagnostic.error && restored_probe_names.names == probe_names.names &&
              api::LoadMgaMetadataSnapshot(probe_context).ok(), "restored real savepoint authority differs");
  for (int mode = 0; mode < 4; ++mode) {
    api::BoundedScopedRowReadControl control;
    api::HeapReadRuntimeObservation observation;
    control.runtime_observation = &observation;
    const std::function<bool()> cancelled = [] { return true; };
    if (mode == 1) observation.storage_bytes_read = std::numeric_limits<std::uint64_t>::max();
    if (mode == 2) control.maximum_memory_bytes = 1;
    if (mode == 3) control.cancellation_requested = &cancelled;
    api::SavepointParsedState loaded;
    loaded.active_savepoints[1]["unproved"] = {};
    const bool ok = api::ParseSavepointsBounded(probe_context, &control, 0, &loaded);
    if (mode == 0) {
      Require(ok && !loaded.diagnostic.error && observation.storage_bytes_read == probe_bytes.size() &&
                  loaded.active_savepoints.at(probe_context.local_transaction_id).contains("savepoint_probe"),
              "exact bounded savepoint read lost real marker or byte accounting");
    } else {
      Require(!ok && loaded.diagnostic.error && loaded.active_savepoints.empty() &&
                  loaded.rollback_ranges.empty(), "bounded savepoint refusal published partial state");
    }
  }
  api::EngineRollbackTransactionRequest probe_rollback;
  probe_rollback.context = probe_context;
  Require(api::EngineRollbackTransaction(probe_rollback).ok,
          "savepoint read probe transaction rollback failed");
  Require(::geteuid() != 0 && std::filesystem::file_size(metadata_path) != 0,
          "metadata I/O fault requires an unprivileged reader and real metadata");
  const auto permissions = std::filesystem::status(metadata_path).permissions();
  const auto row_path = database_path.string() + ".sb.mga_row_versions";
  Require(std::filesystem::file_size(row_path) != 0,
          "metadata I/O fixture did not publish real row records");
  const auto row_permissions = std::filesystem::status(row_path).permissions();
  std::filesystem::permissions(row_path, std::filesystem::perms::none);
  const auto unreadable_rows = api::ClassifyMgaTemporaryRecoveryState(context);
  const auto unreadable_relation = api::LoadMgaRelationStoreState(context);
  std::filesystem::permissions(row_path, row_permissions);
  Require(!unreadable_relation.ok && unreadable_relation.diagnostic.error,
          "relation load published success from unreadable authoritative rows");
  Require(!unreadable_rows.ok && unreadable_rows.diagnostic.error &&
              unreadable_rows.write_admission_must_remain_fenced &&
              unreadable_rows.orphaned_private_metadata_count == 0 &&
              unreadable_rows.orphaned_row_count == 0,
          "temporary recovery published partial authority from unreadable rows");
  const auto restored_relation = api::LoadMgaRelationStoreState(context);
  Require(restored_relation.ok && restored_relation.state.row_versions.size() == 2,
          "restored row read did not return both actual engine-created rows");
  const auto index_path = database_path.string() + ".sb.mga_index_entries";
  const auto saved_index_path = index_path + ".read_failure_test_saved";
  const bool had_index_file = std::filesystem::exists(index_path);
  if (had_index_file) std::filesystem::rename(index_path, saved_index_path);
  std::filesystem::create_directory(index_path);
  const auto unreadable_index = api::LoadMgaRelationStoreState(context);
  std::filesystem::remove(index_path);
  if (had_index_file) std::filesystem::rename(saved_index_path, index_path);
  Require(!unreadable_index.ok && unreadable_index.diagnostic.error,
          "relation load published success from nonregular index authority");
  const auto row_bytes = std::filesystem::file_size(row_path);
  std::filesystem::resize_file(row_path, row_bytes - 1);
  const auto truncated_rows = api::LoadMgaRelationStoreState(context);
  {
    std::ofstream restore(row_path, std::ios::binary | std::ios::app);
    restore.put('\n');
    restore.flush();
    Require(restore.good(), "row fixture delimiter restoration failed");
  }
  Require(!truncated_rows.ok && truncated_rows.diagnostic.error,
          "unterminated row record was admitted as complete relation state");
  const auto table_uuid = created_table.table_object.uuid.canonical;
  const auto scope_root = database_path.string() + ".sb.mga_relation_scope/";
  const auto scoped_index_path = scope_root + table_uuid + ".indexes";
  const auto saved_scoped_index_path = scoped_index_path + ".read_failure_test_saved";
  const bool had_scoped_index = std::filesystem::exists(scoped_index_path);
  if (had_scoped_index) std::filesystem::rename(scoped_index_path, saved_scoped_index_path);
  std::filesystem::create_directory(scoped_index_path);
  const auto unreadable_scoped_index = api::LoadMgaRelationStoreState(context);
  const auto unreadable_target_index = api::LoadMgaRelationStoreIndexesForRelation(context, table_uuid);
  std::filesystem::remove(scoped_index_path);
  if (had_scoped_index) std::filesystem::rename(saved_scoped_index_path, scoped_index_path);
  Require(!unreadable_scoped_index.ok && unreadable_scoped_index.diagnostic.error &&
              !unreadable_target_index.ok && unreadable_target_index.diagnostic.error,
          "scoped index read failure became global fallback or successful target state");
  const auto summary_path = scope_root + table_uuid + ".summary";
  const auto scoped_row_path = scope_root + table_uuid + ".rows";
  Require(std::filesystem::is_regular_file(scoped_row_path), "real inserted scoped rows missing");
  std::vector<api::CrudRowVersionRecord> scoped_rows;
  bool used_rows = false;
  const std::function<bool()> scoped_cancel = [] { return false; };
  const auto bounded_scoped_read = [&](bool expected_success, std::size_t expected_rows) {
    api::BoundedScopedRowReadControl control;
    control.maximum_bytes = 1024 * 1024;
    control.maximum_row_versions = 100;
    control.cancellation_requested = &scoped_cancel;
    const bool ok = api::LoadDecodedScopedRowsForTableBounded(context, table_uuid,
        &control, &scoped_rows, &used_rows);
    if (ok != expected_success || scoped_rows.size() != expected_rows || (!expected_success && used_rows)) {
      std::cerr << "bounded_rows ok=" << ok << " expected=" << expected_success
                << " rows=" << scoped_rows.size() << " used=" << used_rows
                << " detail=" << control.refusal_detail << '\n';
    }
    Require(ok == expected_success && scoped_rows.size() == expected_rows &&
                (expected_success || !used_rows), "bounded scoped read lost admission or exposed partial rows");
  };
  Require(api::LoadDecodedScopedRowsForTable(context, table_uuid, &scoped_rows, &used_rows) &&
              used_rows && scoped_rows.size() == 2, "real row cache warm-up failed");
  const auto scoped_row_permissions = std::filesystem::status(scoped_row_path).permissions();
  std::filesystem::permissions(scoped_row_path, std::filesystem::perms::none);
  const bool unreadable_cached_rows = api::LoadDecodedScopedRowsForTable(context, table_uuid, &scoped_rows, &used_rows);
  Require(!unreadable_cached_rows && scoped_rows.empty() && !used_rows, "denied text read published cached rows");
  bounded_scoped_read(false, 0);
  std::filesystem::permissions(scoped_row_path, scoped_row_permissions);
  Require(!unreadable_cached_rows && scoped_rows.empty() && !used_rows,
          "warm row cache bypassed denied authoritative file access");
  Require(api::LoadDecodedScopedRowsForTable(context, table_uuid, &scoped_rows, &used_rows) &&
              used_rows && scoped_rows.size() == 2, "restored scoped row access lost committed rows");
  std::vector<scratchbird::core::index::byte> original_scoped_bytes;
  Require(api::ReadCompleteMgaBinaryFile(scoped_row_path, &original_scoped_bytes) &&
              !original_scoped_bytes.empty(), "real scoped row bytes unavailable");
  const auto scoped_time = std::filesystem::last_write_time(scoped_row_path);
  const auto replace_scoped_text = [&](const auto& bytes) {
    std::ofstream out(scoped_row_path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    out.close(); Require(static_cast<bool>(out), "scoped text fixture write failed");
    std::filesystem::last_write_time(scoped_row_path, scoped_time);
  };
  auto corrupt_scoped_bytes = original_scoped_bytes;
  corrupt_scoped_bytes.front() = 'X';
  replace_scoped_text(corrupt_scoped_bytes);
  const bool stale_text_cache = api::LoadDecodedScopedRowsForTable(context, table_uuid, &scoped_rows, &used_rows);
  Require(!stale_text_cache && scoped_rows.empty() && !used_rows, "corrupt text read published cached rows");
  bounded_scoped_read(false, 0);
  replace_scoped_text(original_scoped_bytes);
  Require(!stale_text_cache && scoped_rows.empty() && !used_rows,
          "same-size same-mtime corrupt text reused cached row authority");
  Require(api::LoadDecodedScopedRowsForTable(context, table_uuid, &scoped_rows, &used_rows) &&
              used_rows && scoped_rows.size() == 2, "restored text cache failed after corruption");
  corrupt_scoped_bytes = original_scoped_bytes;
  corrupt_scoped_bytes.pop_back();
  replace_scoped_text(corrupt_scoped_bytes);
  const bool partial_text_cache = api::LoadDecodedScopedRowsForTable(context, table_uuid, &scoped_rows, &used_rows);
  Require(!partial_text_cache && scoped_rows.empty() && !used_rows, "partial text read published cached rows");
  bounded_scoped_read(false, 0);
  replace_scoped_text(original_scoped_bytes);
  Require(!partial_text_cache && scoped_rows.empty() && !used_rows,
          "unterminated scoped text published partial or cached row authority");
  const auto binary_index_path = scope_root + table_uuid + ".indexes.sbnx";
  const auto saved_binary_index_path = binary_index_path + ".read_failure_test_saved";
  const bool had_binary_index = std::filesystem::exists(binary_index_path);
  if (had_binary_index) std::filesystem::rename(binary_index_path, saved_binary_index_path);
  std::filesystem::create_directory(binary_index_path);
  const auto unreadable_binary_index = api::LoadMgaRelationStoreIndexesForRelation(context, table_uuid);
  std::filesystem::remove(binary_index_path);
  if (had_binary_index) std::filesystem::rename(saved_binary_index_path, binary_index_path);
  Require(!unreadable_binary_index.ok && unreadable_binary_index.diagnostic.error,
          "nonregular binary index authority became successful target state");
  const auto binary_row_fault_path = scope_root + "binary_read_fault.rows.sbnr";
  std::filesystem::create_directory(binary_row_fault_path);
  std::vector<api::CrudRowVersionRecord> binary_rows;
  api::ScopedRelationSummary binary_summary;
  const bool binary_read_ok = api::DecodeScopedRowBinaryStore(binary_row_fault_path, &binary_rows, &binary_summary);
  std::filesystem::remove(binary_row_fault_path);
  Require(!binary_read_ok && !binary_summary.trusted && binary_rows.empty(),
          "nonregular binary row authority became a trusted empty segment");
  const auto write_binary_fixture = [&](const std::string& bytes) {
    std::ofstream out(binary_row_fault_path, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    out.close();
    Require(static_cast<bool>(out), "binary row read fixture publication failed");
  };
  // Exercise physical framing with actual engine-created identities and an
  // independent two-value oracle. This is codec/I/O evidence, not SQL E2E.
  std::string binary_fixture;
  const std::vector<api::CrudRowVersionRecord> fixture_rows{*stored_row, *stored_second_row};
  const std::array<std::string, 1> fixture_fields{"id"};
  Require(api::AppendScopedRowBinaryBatch(&binary_fixture, fixture_rows, insert.input_rows,
              fixture_fields, stored_row->event_sequence), "binary row fixture encoding failed");
  const auto verify_binary_rows = [&] {
    binary_rows.clear();
    binary_summary = {};
    Require(api::DecodeScopedRowBinaryStore(binary_row_fault_path, &binary_rows, &binary_summary) &&
                binary_summary.trusted && !binary_summary.malformed &&
                binary_summary.row_version_count == 2 && binary_rows.size() == 2 &&
                binary_rows[0].row_uuid == stored_row->row_uuid &&
                binary_rows[1].row_uuid == stored_second_row->row_uuid &&
                binary_rows[0].values == std::vector<std::pair<std::string, std::string>>{{"id", "17"}} &&
                binary_rows[1].values == std::vector<std::pair<std::string, std::string>>{{"id", "18"}},
            "complete binary segment did not publish exact actual row identities and values");
  };
  const auto verify_binary_refusal = [&] {
    // Existing caller-owned rows survive; newly decoded prefixes do not.
    binary_rows = {*stored_row};
    binary_summary = {};
    binary_summary.row_version_count = 11;
    binary_summary.trusted = true;
    Require(!api::DecodeScopedRowBinaryStore(binary_row_fault_path, &binary_rows, &binary_summary) &&
                !binary_summary.trusted && binary_summary.row_version_count == 11 &&
                binary_rows.size() == 1 && binary_rows[0].row_uuid == stored_row->row_uuid,
            "failed binary decode exposed a partial row prefix or trusted summary");
    std::vector<scratchbird::core::index::byte> bytes{1, 2, 3};
    Require(!api::ReadCompleteMgaBinaryFile(binary_row_fault_path, &bytes) && bytes.empty(),
            "failed binary read exposed partial bytes or fabricated success");
  };
  write_binary_fixture(binary_fixture); verify_binary_rows();
  const auto binary_permissions = std::filesystem::status(binary_row_fault_path).permissions();
  std::filesystem::permissions(binary_row_fault_path, std::filesystem::perms::none);
  Require(!std::ifstream(binary_row_fault_path).is_open(), "binary permission fault requires kernel denial");
  verify_binary_refusal();
  std::filesystem::permissions(binary_row_fault_path, binary_permissions);
  verify_binary_rows();
  std::filesystem::remove(binary_row_fault_path);
  std::filesystem::create_directory(binary_row_fault_path); verify_binary_refusal();
  std::filesystem::remove(binary_row_fault_path);
  std::filesystem::create_symlink(binary_row_fault_path + ".absent", binary_row_fault_path);
  verify_binary_refusal(); std::filesystem::remove(binary_row_fault_path);
  std::filesystem::create_symlink(binary_row_fault_path, binary_row_fault_path);
  verify_binary_refusal(); std::filesystem::remove(binary_row_fault_path);
  Require(::mkfifo(binary_row_fault_path.c_str(), 0600) == 0, "binary FIFO fault setup failed");
  verify_binary_refusal(); std::filesystem::remove(binary_row_fault_path);
  std::filesystem::create_symlink("/proc/self/mem", binary_row_fault_path);
  verify_binary_refusal(); std::filesystem::remove(binary_row_fault_path);
  write_binary_fixture(binary_fixture + "invalid trailing batch");
  binary_rows = {*stored_row}; binary_summary = {}; binary_summary.row_version_count = 11;
  Require(!api::DecodeScopedRowBinaryStore(binary_row_fault_path, &binary_rows, &binary_summary) &&
              !binary_summary.trusted && binary_summary.malformed && binary_rows.size() == 1 &&
              binary_summary.row_version_count == 11,
          "malformed late batch published earlier decoded row versions");
  for (std::size_t length = 1; length < binary_fixture.size(); ++length) {
    write_binary_fixture(binary_fixture.substr(0, length));
    binary_rows.clear(); binary_summary = {};
    Require(!api::DecodeScopedRowBinaryStore(binary_row_fault_path, &binary_rows, &binary_summary) &&
                binary_rows.empty() && !binary_summary.trusted && binary_summary.row_version_count == 0,
            "truncated binary batch published partial row or summary authority");
  }
  write_binary_fixture(binary_fixture); verify_binary_rows();
  write_binary_fixture(""); binary_rows.clear(); binary_summary = {};
  Require(api::DecodeScopedRowBinaryStore(binary_row_fault_path, &binary_rows, &binary_summary) &&
              binary_summary.trusted && binary_rows.empty(), "empty regular binary segment was not admitted");
  std::filesystem::remove(binary_row_fault_path); binary_summary = {};
  Require(api::DecodeScopedRowBinaryStore(binary_row_fault_path, &binary_rows, &binary_summary) &&
              binary_summary.trusted && binary_rows.empty(), "absent optional binary segment was not admitted");
  write_binary_fixture(binary_fixture);
  bool cancel_binary_read = false;
  const std::function<bool()> binary_cancel = [&] { return cancel_binary_read; };
  const auto bounded_binary_read = [&](std::uint64_t expected_bytes, bool should_succeed) {
    api::BoundedScopedRowReadControl control;
    control.maximum_bytes = 1024 * 1024;
    control.maximum_row_versions = 100;
    control.cancellation_requested = &binary_cancel;
    binary_rows.clear(); binary_summary = {};
    const auto ok = api::DecodeScopedRowBinaryStore(binary_row_fault_path, &binary_rows,
        &binary_summary, &control, expected_bytes);
    Require(ok == should_succeed && binary_summary.trusted == should_succeed &&
                binary_rows.size() == (should_succeed ? 2 : 0) &&
                binary_summary.row_version_count == (should_succeed ? 2 : 0),
            "bounded binary read lost exact success or published failed partial rows");
    if (cancel_binary_read) Require(control.cancellation_observed, "binary read ignored cancellation");
  };
  bounded_binary_read(binary_fixture.size(), true);
  bounded_binary_read(binary_fixture.size() - 1, false);
  bounded_binary_read(binary_fixture.size() + 1, false);
  cancel_binary_read = true;
  bounded_binary_read(binary_fixture.size(), false);
  cancel_binary_read = false;
  bounded_binary_read(binary_fixture.size(), true);
  std::filesystem::remove(binary_row_fault_path);
  std::filesystem::create_directory(binary_row_fault_path);
  bounded_binary_read(0, false);
  std::filesystem::remove(binary_row_fault_path);
  const auto scoped_binary_row_path = scope_root + table_uuid + ".rows.sbnr";
  const auto saved_scoped_row_path = scoped_row_path + ".cache_test_saved";
  const auto saved_scoped_binary_path = scoped_binary_row_path + ".cache_test_saved";
  const bool had_scoped_binary = std::filesystem::exists(scoped_binary_row_path);
  std::filesystem::rename(scoped_row_path, saved_scoped_row_path);
  if (had_scoped_binary) std::filesystem::rename(scoped_binary_row_path, saved_scoped_binary_path);
  const auto replace_scoped_binary = [&](const std::string& bytes) {
    std::ofstream out(scoped_binary_row_path, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    out.close(); Require(static_cast<bool>(out), "scoped binary cache fixture write failed");
  };
  const auto verify_scoped_binary = [&](const std::string& second_value) {
    Require(api::LoadDecodedScopedRowsForTable(context, table_uuid, &scoped_rows, &used_rows) &&
                used_rows && scoped_rows.size() == 2 &&
                scoped_rows[0].row_uuid == stored_row->row_uuid &&
                scoped_rows[1].row_uuid == stored_second_row->row_uuid &&
                scoped_rows[1].values == std::vector<std::pair<std::string, std::string>>{{"id", second_value}},
            "row cache did not decode exact current binary bytes");
  };
  replace_scoped_binary(binary_fixture); verify_scoped_binary("18"); verify_scoped_binary("18");
  const auto scoped_binary_time = std::filesystem::last_write_time(scoped_binary_row_path);
  auto changed_values = insert.input_rows;
  changed_values[1].fields[0].second.encoded_value = "19";
  std::string changed_binary;
  Require(api::AppendScopedRowBinaryBatch(&changed_binary, fixture_rows, changed_values,
              fixture_fields, stored_row->event_sequence) && changed_binary.size() == binary_fixture.size(),
          "same-size binary replacement fixture not constructed");
  replace_scoped_binary(changed_binary);
  std::filesystem::last_write_time(scoped_binary_row_path, scoped_binary_time);
  verify_scoped_binary("19"); bounded_scoped_read(true, 2);
  std::filesystem::permissions(scoped_binary_row_path, std::filesystem::perms::none);
  const bool denied_binary_cache = api::LoadDecodedScopedRowsForTable(context, table_uuid, &scoped_rows, &used_rows);
  Require(!denied_binary_cache && scoped_rows.empty() && !used_rows, "denied binary read published cached rows");
  bounded_scoped_read(false, 0);
  std::filesystem::permissions(scoped_binary_row_path, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write);
  Require(!denied_binary_cache && scoped_rows.empty() && !used_rows, "warm binary cache bypassed read denial");
  verify_scoped_binary("19");
  std::filesystem::remove(scoped_binary_row_path);
  std::filesystem::create_directory(scoped_binary_row_path);
  const bool directory_binary_cache = api::LoadDecodedScopedRowsForTable(context, table_uuid, &scoped_rows, &used_rows);
  Require(!directory_binary_cache && scoped_rows.empty() && !used_rows, "nonregular binary read published cached rows");
  bounded_scoped_read(false, 0);
  std::filesystem::remove(scoped_binary_row_path);
  Require(!directory_binary_cache && scoped_rows.empty() && !used_rows, "nonregular binary segment reused cached rows");
  Require(api::LoadDecodedScopedRowsForTable(context, table_uuid, &scoped_rows, &used_rows) &&
              scoped_rows.empty() && !used_rows, "removed segments reused cached rows");
  if (had_scoped_binary) std::filesystem::rename(saved_scoped_binary_path, scoped_binary_row_path);
  std::filesystem::rename(saved_scoped_row_path, scoped_row_path);
  Require(api::LoadDecodedScopedRowsForTable(context, table_uuid, &scoped_rows, &used_rows) &&
              used_rows && scoped_rows.size() == 2, "restored engine-created segments lost rows");
  bounded_scoped_read(true, 2);
  Require(std::filesystem::is_regular_file(summary_path),
          "actual inserted rows did not produce relation summary");
  const auto healthy_summary = api::CanUseMgaRelationIndexOnlyProofForInsertTarget(context, table_uuid);
  Require(healthy_summary.ok && healthy_summary.summary_trusted && healthy_summary.row_version_count == 2,
          "healthy actual relation summary did not account for both rows");
  const auto summary_permissions = std::filesystem::status(summary_path).permissions();
  std::filesystem::permissions(summary_path, std::filesystem::perms::none);
  const auto unreadable_summary = api::CanUseMgaRelationIndexOnlyProofForInsertTarget(context, table_uuid);
  std::filesystem::permissions(summary_path, summary_permissions);
  Require(!unreadable_summary.ok && unreadable_summary.diagnostic.error &&
              !unreadable_summary.summary_trusted && !unreadable_summary.eligible,
          "unreadable summary authorized an index-only optimization");
  const auto final_relation = api::LoadMgaRelationStoreState(context);
  Require(final_relation.ok && final_relation.state.row_versions.size() == 2,
          "restored storage faults lost an actual row");
  // A nonregular companion path is a real filesystem refusal, never an empty
  // large-value inventory. Preserve any engine-created file across the fault.
  const auto large_path = database_path.string() + ".sb.mga_large_values";
  const auto saved_large_path = large_path + ".read_failure_test_saved";
  const bool had_large_file = std::filesystem::exists(large_path);
  if (had_large_file) std::filesystem::rename(large_path, saved_large_path);
  std::filesystem::create_directory(large_path);
  const auto unreadable_large = api::ClassifyMgaTemporaryRecoveryState(context);
  const auto unreadable_reclaims = api::LoadVisibleMgaLargeValueReclaims(context);
  std::vector<std::string> partial_records{"must_be_cleared"};
  const bool complete_large = api::ReadCompleteMgaTextRecords(large_path, &partial_records);
  std::filesystem::remove(large_path);
  if (had_large_file) std::filesystem::rename(saved_large_path, large_path);
  Require(!unreadable_large.ok && unreadable_large.diagnostic.error &&
              unreadable_large.write_admission_must_remain_fenced &&
              unreadable_large.orphaned_private_metadata_count == 0 &&
              unreadable_large.orphaned_row_count == 0 &&
              unreadable_large.orphaned_large_value_count == 0 &&
              unreadable_reclaims.diagnostic.error && unreadable_reclaims.overflow_uuids.empty() &&
              !complete_large && partial_records.empty(),
          "large-value read failure published partial recovery or reclaim authority");
  for (const auto& fault : {std::string{}, std::string("rollback_fence"), std::string("rollback_secondary")}) {
    api::EngineCleanupTemporarySessionRequest request;
    request.context = context;
    if (!fault.empty()) request.option_envelopes = {"ipar.fault_injection.point=" + fault};
    std::filesystem::permissions(metadata_path, std::filesystem::perms::none);
    const auto unreadable_work = api::HasMgaTemporaryCleanupMetadataWork(context, true, true, true);
    const auto unreadable_snapshot = api::LoadMgaMetadataSnapshot(context);
    const auto unreadable_recovery = api::ClassifyMgaTemporaryRecoveryState(context);
    const auto failed = api::EngineCleanupTemporarySessionState(request);
    std::filesystem::permissions(metadata_path, permissions);
    std::cerr << "metadata_fault=" << fault << " cleanup_ok=" << failed.ok
              << " state=" << static_cast<unsigned>(failed.cleanup_transaction.state)
              << " local_id=" << failed.cleanup_local_transaction_id << '\n';
    for (const auto& diagnostic : failed.diagnostics)
      std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
    Require(!unreadable_work.ok && unreadable_work.diagnostic.error &&
                unreadable_snapshot.snapshot == nullptr && unreadable_snapshot.diagnostic.error,
            "warm metadata cache hid current read failure");
    Require(!unreadable_recovery.ok && unreadable_recovery.diagnostic.error &&
                unreadable_recovery.write_admission_must_remain_fenced &&
                unreadable_recovery.classification == "fenced",
            "temporary recovery classified unreadable metadata as open-allowed authority");
    Require(!failed.ok, "unreadable authoritative metadata produced successful cleanup");
    Require(failed.cleanup_local_transaction_id != 0 &&
                failed.cleanup_transaction.state == (fault == "rollback_fence" ? State::not_applied : State::rolled_back) &&
                failed.cleanup_transaction.post_inventory_secondary_failure == (fault == "rollback_secondary"),
            "metadata cleanup failure lost rollback outcome or exact identity");
    const auto inventory = db::LoadLocalTransactionInventoryFromDatabase(database_path.string());
    Require(inventory.ok(), "metadata cleanup rollback inventory unavailable");
    const auto entry = std::find_if(inventory.inventory.entries.begin(), inventory.inventory.entries.end(),
        [&](const auto& e) { return e.identity.local_id.value == failed.cleanup_local_transaction_id &&
                                   e.identity.transaction_uuid.value.bytes == failed.cleanup_transaction.transaction_uuid; });
    Require(entry != inventory.inventory.entries.end() &&
                entry->state == (fault == "rollback_fence" ? tx::TransactionState::active : tx::TransactionState::rolled_back),
            "metadata cleanup rollback result disagrees with independent inventory");
    if (fault == "rollback_fence") {
      api::EngineRollbackTransactionRequest rollback;
      rollback.context = context;
      rollback.context.local_transaction_id = entry->identity.local_id.value;
      rollback.context.transaction_uuid.canonical = uuid::UuidToString(entry->identity.transaction_uuid.value);
      Require(api::EngineRollbackTransaction(rollback).ok, "metadata cleanup refused rollback could not be finalized");
    }
    const auto retained = api::HasMgaTemporaryCleanupMetadataWork(context, true, true, true);
    Require(retained.ok && retained.has_work, "failed cleanup removed actual temporary metadata");
  }
  const auto metadata_size = std::filesystem::file_size(metadata_path);
  std::filesystem::resize_file(metadata_path, metadata_size - 1);
  const auto truncated = api::HasMgaTemporaryCleanupMetadataWork(context, true, true, true);
  {
    std::ofstream restore(metadata_path, std::ios::binary | std::ios::app);
    restore.put('\n');
    restore.flush();
    Require(restore.good(), "metadata fixture delimiter restoration failed");
  }
  Require(!truncated.ok && truncated.diagnostic.error,
          "unterminated metadata record was accepted as complete authority");
  const auto saved_metadata_path = metadata_path + ".read_failure_test_saved";
  std::filesystem::rename(metadata_path, saved_metadata_path);
  std::filesystem::create_directory(metadata_path);
  const auto directory = api::HasMgaTemporaryCleanupMetadataWork(context, true, true, true);
  std::filesystem::remove(metadata_path);
  std::filesystem::rename(saved_metadata_path, metadata_path);
  Require(!directory.ok && directory.diagnostic.error,
          "metadata directory was accepted as empty authority");
  api::EngineCleanupTemporarySessionRequest retry;
  retry.context = context;
  const auto recovered = api::EngineCleanupTemporarySessionState(retry);
  Require(recovered.ok && recovered.temporary_retired_private_metadata == 1 &&
              recovered.temporary_deleted_rows == 2 &&
              recovered.temporary_reclaimed_large_values == 2 &&
              TransactionHasState(database_path, recovered.cleanup_local_transaction_id, tx::TransactionState::committed),
          "read-restored cleanup did not commit actual private metadata retirement");
  const auto after = api::HasMgaTemporaryCleanupMetadataWork(context, true, true, true);
  Require(after.ok && !after.has_work, "successful cleanup left temporary metadata visible");
  std::cout << "metadata_cleanup_read_failure action=true rollback_fence=true rollback_secondary=true real_retirement=true\n";
}

}  // namespace

int main(int argc, char** argv) {
  const auto temp_dir = MakeTempDir();
  const auto database_path = temp_dir / "dblc009_detach_cleanup.sbdb";
  const std::string database_uuid = CreateOpenDatabase(database_path);
  if (argc == 2 && std::string_view(argv[1]) == "--temporary-cleanup-failure") {
    std::cerr << "temporary_cleanup_fixture=" << temp_dir.string() << '\n';
    VerifyTemporaryCleanupFailure(database_path, database_uuid);
  } else if (argc == 2 && std::string_view(argv[1]) == "--temporary-cleanup-identity") {
    std::cerr << "temporary_cleanup_fixture=" << temp_dir.string() << '\n';
    VerifyTemporaryCleanupIdentity(database_path, database_uuid);
  } else if (argc == 2 && std::string_view(argv[1]) == "--metadata-read-failure") {
    std::cerr << "temporary_cleanup_fixture=" << temp_dir.string() << '\n';
    VerifyMetadataReadFailure(database_path, database_uuid);
  } else if (argc == 2 && std::string_view(argv[1]) == "--canonical-savepoint-effects") {
    std::cerr << "temporary_cleanup_fixture=" << temp_dir.string() << '\n';
    VerifyMetadataReadFailure(database_path, database_uuid, true);
  } else {
    Require(argc == 1, "unknown detach fixture arguments");
    VerifyDetachCleanup(database_path, database_uuid);
  }

  std::filesystem::remove_all(temp_dir);
  return EXIT_SUCCESS;
}
