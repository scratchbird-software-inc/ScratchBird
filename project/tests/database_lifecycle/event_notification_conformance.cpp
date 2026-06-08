// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "database_lifecycle.hpp"
#include "notification/notification_api.hpp"
#include "parser_server_event_frame_dispatcher.hpp"
#include "parser_server_event_ipc.hpp"
#include "transaction/transaction_api.hpp"
#include "uuid.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

namespace {

namespace api = scratchbird::engine::internal_api;
namespace db = scratchbird::storage::database;
namespace server = scratchbird::server;
namespace uuid = scratchbird::core::uuid;
using scratchbird::core::platform::UuidKind;

struct Fixture {
  std::filesystem::path temp_dir;
  std::filesystem::path database_path;
  std::string database_uuid;
};

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) { Fail(message); }
}

bool Contains(std::string_view haystack, std::string_view needle) {
  return haystack.find(needle) != std::string_view::npos;
}

bool HasDiagnostic(const api::EngineApiResult& result, std::string_view code) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.code == code) return true;
  }
  return false;
}

bool HasVector(const std::vector<server::ParserServerMessageVector>& vectors,
               std::string_view code) {
  for (const auto& vector : vectors) {
    if (vector.diagnostic_code == code) return true;
  }
  return false;
}

std::filesystem::path MakeTempDir() {
  std::string tmpl = "/tmp/sb_dblc013y_event.XXXXXX";
  std::vector<char> writable(tmpl.begin(), tmpl.end());
  writable.push_back('\0');
  char* made = ::mkdtemp(writable.data());
  Require(made != nullptr, "mkdtemp failed for DBLC-013Y event notification test");
  return std::filesystem::path(made);
}

std::string NewUuid(UuidKind kind, std::uint64_t millis) {
  const auto generated = uuid::GenerateEngineIdentityV7(kind, millis);
  Require(generated.ok(), "DBLC-013Y UUID generation failed");
  return uuid::UuidToString(generated.value.value);
}

Fixture MakeFixture(std::string_view name) {
  Fixture fixture;
  fixture.temp_dir = MakeTempDir();
  fixture.database_path = fixture.temp_dir / (std::string(name) + ".sbdb");
  db::DatabaseCreateConfig create;
  create.path = fixture.database_path.string();
  create.database_uuid = uuid::GenerateEngineIdentityV7(UuidKind::database, 1779130901000).value;
  create.filespace_uuid = uuid::GenerateEngineIdentityV7(UuidKind::filespace, 1779130901001).value;
  create.page_size = 16384;
  create.creation_unix_epoch_millis = 1779130901002;
  create.allow_minimal_resource_bootstrap = true;
  create.require_resource_seed_pack = false;
  create.allow_overwrite = true;
  const auto created = db::CreateDatabaseFile(create);
  Require(created.ok(), "DBLC-013Y database create failed");
  const auto opened = db::OpenDatabaseFile({fixture.database_path.string(), false, false, false});
  Require(opened.ok(), "DBLC-013Y first-open activation failed");
  const auto clean = db::MarkDatabaseCleanShutdown(fixture.database_path.string());
  Require(clean.ok(), "DBLC-013Y clean shutdown marker failed");
  fixture.database_uuid = uuid::UuidToString(create.database_uuid.value);
  return fixture;
}

api::EngineRequestContext Context(const Fixture& fixture,
                                  std::string session_uuid,
                                  std::string principal_uuid,
                                  bool authorized = true) {
  api::EngineRequestContext context;
  context.request_id = "dblc-013y-event";
  context.database_path = fixture.database_path.string();
  context.database_uuid.canonical = fixture.database_uuid;
  context.session_uuid.canonical = std::move(session_uuid);
  context.principal_uuid.canonical = std::move(principal_uuid);
  context.security_context_present = true;
  context.catalog_generation_id = 1;
  context.security_epoch = 1;
  context.resource_epoch = 1;
  context.name_resolution_epoch = 1;
  if (authorized) {
    context.trust_mode = api::EngineTrustMode::embedded_in_process;
    context.trace_tags.push_back("security.fixture_trace_authority");
    context.trace_tags.push_back("group:DBA");
  }
  return context;
}

api::EngineRequestContext BeginTx(const api::EngineRequestContext& base) {
  api::EngineBeginTransactionRequest begin;
  begin.context = base;
  begin.isolation_level = "read_committed";
  const auto begun = api::EngineBeginTransaction(begin);
  if (!begun.ok) {
    for (const auto& diagnostic : begun.diagnostics) {
      std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
    }
  }
  Require(begun.ok, "DBLC-013Y begin transaction failed");
  auto tx_context = base;
  tx_context.local_transaction_id = begun.local_transaction_id;
  tx_context.transaction_uuid = begun.transaction_uuid;
  return tx_context;
}

void CommitTx(const api::EngineRequestContext& tx_context) {
  api::EngineCommitTransactionRequest commit;
  commit.context = tx_context;
  const auto committed = api::EngineCommitTransaction(commit);
  Require(committed.ok, "DBLC-013Y commit transaction failed");
}

void RollbackTx(const api::EngineRequestContext& tx_context) {
  api::EngineRollbackTransactionRequest rollback;
  rollback.context = tx_context;
  const auto rolled_back = api::EngineRollbackTransaction(rollback);
  Require(rolled_back.ok, "DBLC-013Y rollback transaction failed");
}

void CreateChannel(const api::EngineRequestContext& base,
                   const std::string& channel_uuid,
                   const std::string& channel_name,
                   std::vector<std::string> options = {}) {
  const auto tx_context = BeginTx(base);
  api::EngineCreateEventChannelRequest request;
  request.context = tx_context;
  request.target_object = {{channel_uuid}, "event_channel"};
  request.option_envelopes.push_back("channel_uuid:" + channel_uuid);
  request.option_envelopes.push_back("channel:" + channel_name);
  request.option_envelopes.insert(request.option_envelopes.end(), options.begin(), options.end());
  const auto created = api::EngineCreateEventChannel(request);
  Require(created.ok, "DBLC-013Y create event channel failed");
  CommitTx(tx_context);
}

std::string NotifyCommitted(const api::EngineRequestContext& base,
                            const std::string& channel_uuid,
                            const std::string& payload,
                            std::vector<std::string> options = {}) {
  const auto tx_context = BeginTx(base);
  api::EngineNotifyEventChannelRequest request;
  request.context = tx_context;
  request.target_object = {{channel_uuid}, "event_channel"};
  request.option_envelopes.push_back("channel_uuid:" + channel_uuid);
  request.option_envelopes.push_back("payload:" + payload);
  request.option_envelopes.insert(request.option_envelopes.end(), options.begin(), options.end());
  const auto notified = api::EngineNotifyEventChannel(request);
  Require(notified.ok, "DBLC-013Y committed event notify failed");
  const std::string event_uuid = notified.publication.event_uuid;
  CommitTx(tx_context);
  return event_uuid;
}

void NotifyRolledBack(const api::EngineRequestContext& base,
                      const std::string& channel_uuid,
                      const std::string& payload) {
  const auto tx_context = BeginTx(base);
  api::EngineNotifyEventChannelRequest request;
  request.context = tx_context;
  request.target_object = {{channel_uuid}, "event_channel"};
  request.option_envelopes.push_back("channel_uuid:" + channel_uuid);
  request.option_envelopes.push_back("payload:" + payload);
  const auto notified = api::EngineNotifyEventChannel(request);
  Require(notified.ok, "DBLC-013Y rollback-path event notify failed before rollback");
  RollbackTx(tx_context);
}

server::ParserServerEventSession EventSession(const Fixture& fixture,
                                              const std::string& parser_channel_uuid,
                                              const std::string& session_uuid,
                                              bool authorized = true) {
  server::ParserServerEventSession session;
  session.parser_channel_uuid = parser_channel_uuid;
  session.engine_context.request_id = "dblc-013y-parser-event";
  session.engine_context.database_path = fixture.database_path.string();
  session.engine_context.database_uuid.canonical = fixture.database_uuid;
  session.engine_context.session_uuid.canonical = session_uuid;
  session.engine_context.principal_uuid.canonical = "019e13c0-0000-7000-8000-0000000000aa";
  session.engine_context.security_context_present = true;
  session.engine_context.catalog_generation_id = 1;
  session.engine_context.security_epoch = 1;
  session.engine_context.resource_epoch = 1;
  session.engine_context.name_resolution_epoch = 1;
  if (authorized) {
    session.engine_context.trust_mode = api::EngineTrustMode::embedded_in_process;
    session.engine_context.trace_tags.push_back("security.fixture_trace_authority");
    session.engine_context.trace_tags.push_back("group:DBA");
  }
  session.session_bound = true;
  return session;
}

server::PsEventSubscribeResult Subscribe(server::ParserServerEventIpcRuntime* runtime,
                                         const server::ParserServerEventSession& session,
                                         const std::string& channel_uuid) {
  server::PsEventSubscribeRequest request;
  request.request_uuid = "subscribe-" + channel_uuid;
  request.session = session;
  request.channel_uuid = channel_uuid;
  request.rendering_profile_uuid = "rendering.default";
  return runtime->HandleSubscribe(request);
}

void TestParserIpcEngineAuthorizedDeliveryAckAndDisconnect() {
  const auto fixture = MakeFixture("delivery");
  const auto admin = Context(fixture,
                             "019e13c0-0000-7000-8000-000000000001",
                             "019e13c0-0000-7000-8000-000000000002");
  const std::string channel_uuid = "019e13c0-0000-7000-8000-000000000101";
  CreateChannel(admin, channel_uuid, "orders_ready");

  server::ParserEventNotificationRouter router;
  server::ParserServerEventIpcRuntime runtime(&router);

  const auto denied_session =
      EventSession(fixture,
                   "019e13c0-0000-7000-8000-000000000201",
                   "019e13c0-0000-7000-8000-000000000301",
                   false);
  const auto denied = Subscribe(&runtime, denied_session, channel_uuid);
  Require(denied.outcome == "rejected" &&
              HasVector(denied.message_vector_set, "EVENT.AUTHORIZATION_DENIED"),
          "DBLC-013Y unauthorized subscribe was not rejected by engine");
  Require(router.ActiveSubscriptionCount() == 0,
          "DBLC-013Y unauthorized subscribe mutated router state");

  const auto session =
      EventSession(fixture,
                   "019e13c0-0000-7000-8000-000000000202",
                   "019e13c0-0000-7000-8000-000000000302");
  const auto subscribed = Subscribe(&runtime, session, channel_uuid);
  Require(subscribed.outcome == "accepted" && !subscribed.subscription_uuid.empty(),
          "DBLC-013Y authorized subscribe failed");
  Require(router.ActiveSubscriptionCount() == 1,
          "DBLC-013Y authorized subscribe did not register router subscription");

  const std::string event_uuid = NotifyCommitted(admin, channel_uuid, "payload-1");
  server::PsEventDeliveryPumpRequest pump;
  pump.request_uuid = "pump-1";
  pump.session = session;
  pump.max_events = 16;
  auto delivered = runtime.PumpCommittedEvents(pump);
  Require(delivered.outcome == "accepted" && delivered.notifications.size() == 1,
          "DBLC-013Y committed event was not delivered exactly once");
  Require(delivered.notifications.front().event_uuid == event_uuid,
          "DBLC-013Y delivered event UUID mismatch");

  server::PsEventAckRequest ack;
  ack.request_uuid = "ack-1";
  ack.session = session;
  ack.subscription_uuid = subscribed.subscription_uuid;
  ack.event_uuid = event_uuid;
  ack.delivery_sequence = delivered.notifications.front().delivery_sequence;
  auto acked = runtime.HandleAck(ack);
  Require(acked.outcome == "accepted" && !acked.acknowledgement_uuid.empty(),
          "DBLC-013Y valid ACK was rejected");

  auto delivered_again = runtime.PumpCommittedEvents(pump);
  Require(delivered_again.outcome == "accepted" && delivered_again.notifications.empty(),
          "DBLC-013Y ACKed event was delivered again");

  server::PsEventAckRequest bad_ack = ack;
  bad_ack.request_uuid = "bad-ack";
  bad_ack.event_uuid = "019e13c0-0000-7000-8000-00000000dead";
  const auto invalid = runtime.HandleAck(bad_ack);
  Require(invalid.outcome == "rejected" &&
              HasVector(invalid.message_vector_set, "EVENT.ACK_INVALID"),
          "DBLC-013Y invalid ACK was not rejected by engine");

  server::PsEventDisconnectRequest disconnect;
  disconnect.session = session;
  disconnect.disconnect_reason = "test_disconnect";
  const auto cleaned = runtime.HandleDisconnect(disconnect);
  Require(cleaned.outcome == "accepted" && router.ActiveSubscriptionCount() == 0,
          "DBLC-013Y disconnect did not clean engine/router subscription state");
}

void TestRollbackSavepointRedactionBackpressureAndClusterRefusal() {
  const auto fixture = MakeFixture("edge");
  const auto admin = Context(fixture,
                             "019e13c0-0000-7000-8000-000000000401",
                             "019e13c0-0000-7000-8000-000000000402");
  const std::string channel_uuid = "019e13c0-0000-7000-8000-000000000501";
  CreateChannel(admin, channel_uuid, "edge_events");

  server::ParserEventNotificationRouter router;
  server::ParserServerEventIpcRuntime runtime(&router);
  const auto session =
      EventSession(fixture,
                   "019e13c0-0000-7000-8000-000000000601",
                   "019e13c0-0000-7000-8000-000000000701");
  const auto subscribed = Subscribe(&runtime, session, channel_uuid);
  Require(subscribed.outcome == "accepted", "DBLC-013Y edge subscribe failed");

  NotifyRolledBack(admin, channel_uuid, "rolled-back-payload");
  server::PsEventDeliveryPumpRequest pump;
  pump.request_uuid = "pump-rollback";
  pump.session = session;
  pump.max_events = 16;
  auto rollback_delivery = runtime.PumpCommittedEvents(pump);
  Require(rollback_delivery.outcome == "accepted" && rollback_delivery.notifications.empty(),
          "DBLC-013Y rolled-back event became deliverable");

  const auto tx_context = BeginTx(admin);
  api::EngineNotifyEventChannelRequest savepoint_request;
  savepoint_request.context = tx_context;
  savepoint_request.target_object = {{channel_uuid}, "event_channel"};
  savepoint_request.option_envelopes.push_back("channel_uuid:" + channel_uuid);
  savepoint_request.option_envelopes.push_back("payload:savepoint-payload");
  savepoint_request.option_envelopes.push_back("savepoint_rolled_back:true");
  const auto savepoint_refused = api::EngineNotifyEventChannel(savepoint_request);
  Require(!savepoint_refused.ok &&
              HasDiagnostic(savepoint_refused, "EVENT.SAVEPOINT_ROLLED_BACK"),
          "DBLC-013Y savepoint-rolled-back notification was not refused");
  RollbackTx(tx_context);

  const std::string redacted_event =
      NotifyCommitted(admin, channel_uuid, "secret-payload", {"redact_payload:true"});
  auto redacted_delivery = runtime.PumpCommittedEvents(pump);
  Require(redacted_delivery.outcome == "accepted" && redacted_delivery.notifications.size() == 1,
          "DBLC-013Y redacted committed event was not delivered");
  Require(redacted_delivery.notifications.front().event_uuid == redacted_event,
          "DBLC-013Y redacted event UUID mismatch");
  Require(redacted_delivery.notifications.front().notification_vector.fields.size() > 0,
          "DBLC-013Y redacted notification had no fields");
  bool redacted_payload_seen = false;
  for (const auto& [key, value] : redacted_delivery.notifications.front().notification_vector.fields) {
    if (key == "payload" && value == "<redacted>") { redacted_payload_seen = true; }
    Require(value != "secret-payload", "DBLC-013Y protected event payload leaked");
  }
  Require(redacted_payload_seen, "DBLC-013Y redacted payload marker was absent");

  const auto event_a = NotifyCommitted(admin, channel_uuid, "a");
  const auto event_b = NotifyCommitted(admin, channel_uuid, "b");
  (void)event_a;
  (void)event_b;
  server::PsEventDeliveryPumpRequest pressure = pump;
  pressure.request_uuid = "pump-pressure";
  pressure.queue_policy.max_queued_events = 1;
  pressure.queue_policy.overflow_behavior = "backpressure";
  auto pressured = runtime.PumpCommittedEvents(pressure);
  Require(pressured.outcome == "accepted" && !pressured.backpressure_frames.empty(),
          "DBLC-013Y queue backpressure frame was not emitted");
  Require(pressured.backpressure_frames.front().message_vector.diagnostic_code ==
              "EVENT.BACKPRESSURE",
          "DBLC-013Y backpressure diagnostic mismatch");

  api::EngineNotifyEventChannelRequest cluster_request;
  cluster_request.context = admin;
  cluster_request.context.local_transaction_id = 1;
  cluster_request.target_object = {{channel_uuid}, "event_channel"};
  cluster_request.option_envelopes.push_back("channel_uuid:" + channel_uuid);
  cluster_request.option_envelopes.push_back("payload:cluster");
  cluster_request.option_envelopes.push_back("cluster_event_route:true");
  const auto cluster_refused = api::EngineNotifyEventChannel(cluster_request);
  Require(!cluster_refused.ok &&
              HasDiagnostic(cluster_refused, "EVENT.CLUSTER_UNAVAILABLE"),
          "DBLC-013Y standalone cluster event path did not fail closed");

  const std::string events_path = fixture.database_path.string() + ".sb.notification_events";
  std::ifstream in(events_path, std::ios::binary);
  Require(static_cast<bool>(in), "DBLC-013Y notification event file missing");
  const std::string event_log{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
  Require(!Contains(event_log, "PRAGMA") && !Contains(event_log, "journal_mode") &&
              !Contains(event_log, "WAL"),
          "DBLC-013Y notification lifecycle persisted forbidden WAL/PRAGMA text");
}

void TestParserOwnedServerFramesRejected() {
  server::ParserEventNotificationRouter router;
  server::ParserServerEventIpcRuntime runtime(&router);
  server::ParserServerEventFrameDispatcher dispatcher(&runtime);
  server::ParserServerEventFrame frame;
  frame.message_type = server::ParserServerEventMessageType::kEventNotification;
  frame.request_uuid = "bad-direction";
  const auto dispatched = dispatcher.DispatchParserFrame(frame);
  Require(!dispatched.ok &&
              HasVector(dispatched.message_vector_set,
                        "PARSER_SERVER_IPC.MESSAGE_DIRECTION_INVALID"),
          "DBLC-013Y parser-owned server notification frame was admitted");
}

}  // namespace

int main() {
  TestParserIpcEngineAuthorizedDeliveryAckAndDisconnect();
  TestRollbackSavepointRedactionBackpressureAndClusterRefusal();
  TestParserOwnedServerFramesRejected();
  std::cout << "database_lifecycle_event_notification_conformance=passed\n";
  return EXIT_SUCCESS;
}
