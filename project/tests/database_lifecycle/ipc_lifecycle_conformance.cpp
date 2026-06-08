// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "database_lifecycle.hpp"
#include "parser_server_event_ipc.hpp"
#include "parser_server_ipc.hpp"
#include "notification/notification_api.hpp"
#include "sbps.hpp"
#include "server_ipc_lifecycle.hpp"
#include "uuid.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

#ifndef _WIN32
#include <sys/stat.h>
#endif

namespace {

using scratchbird::server::BuildParserServerEndpointDescriptor;
using scratchbird::server::EvaluateServerIpcEndpointLifecycle;
using scratchbird::server::HostedDatabaseSnapshot;
using scratchbird::server::HostedDatabaseState;
using scratchbird::server::HostedEngineState;
using scratchbird::server::ParserEventNotificationRouter;
using scratchbird::server::ParserServerEventIpcRuntime;
using scratchbird::server::ParserServerEventSession;
using scratchbird::server::PsEventSubscribeRequest;
using scratchbird::server::ServerBootstrapConfig;
using scratchbird::server::ServerIpcEndpointDescriptor;
using scratchbird::server::ServerIpcEndpointOperation;
using scratchbird::server::ServerLifecycleArtifacts;
using scratchbird::server::WriteServerIpcEndpointDescriptor;
namespace ps = scratchbird::server::sbps;
namespace legacy = scratchbird::parser::sbsql;
namespace api = scratchbird::engine::internal_api;
namespace db = scratchbird::storage::database;
namespace uuid = scratchbird::core::uuid;
using scratchbird::core::platform::UuidKind;

void Require(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(EXIT_FAILURE);
  }
}

bool Contains(std::string_view haystack, std::string_view needle) {
  return haystack.find(needle) != std::string_view::npos;
}

std::filesystem::path MakeTempDir() {
  std::string tmpl = "/tmp/sb_dblc013f_ipc.XXXXXX";
  std::vector<char> writable(tmpl.begin(), tmpl.end());
  writable.push_back('\0');
  char* made = ::mkdtemp(writable.data());
  Require(made != nullptr, "mkdtemp failed for DBLC-013F IPC lifecycle test");
  return std::filesystem::path(made);
}

std::string CreateOpenDatabase(const std::filesystem::path& path) {
  db::DatabaseCreateConfig create;
  create.path = path.string();
  create.database_uuid = uuid::GenerateEngineIdentityV7(UuidKind::database, 1779130601000).value;
  create.filespace_uuid = uuid::GenerateEngineIdentityV7(UuidKind::filespace, 1779130601001).value;
  create.page_size = 16384;
  create.creation_unix_epoch_millis = 1779130601002;
  create.allow_minimal_resource_bootstrap = true;
  create.require_resource_seed_pack = false;
  create.allow_overwrite = true;
  const auto created = db::CreateDatabaseFile(create);
  Require(created.ok(), "DBLC-013F database create failed for event IPC");
  const auto opened = db::OpenDatabaseFile({path.string(), false, false, false});
  Require(opened.ok(), "DBLC-013F first open activation failed for event IPC");
  const auto clean = db::MarkDatabaseCleanShutdown(path.string());
  Require(clean.ok(), "DBLC-013F clean shutdown marker failed for event IPC");
  return uuid::UuidToString(create.database_uuid.value);
}

ServerLifecycleArtifacts Artifacts() {
  ServerLifecycleArtifacts artifacts;
  artifacts.generation = 1306;
  artifacts.state = "service_ready";
  return artifacts;
}

HostedEngineState EngineState(HostedDatabaseState state = HostedDatabaseState::kOpen,
                              bool open = true) {
  HostedDatabaseSnapshot database;
  database.state = state;
  database.database_uuid = "018f58bd-98f0-7000-8000-00000013060f";
  database.database_path = "/tmp/sb_dblc013f_ipc.sbdb";
  database.database_open = open;
  HostedEngineState engine;
  engine.engine_context_active = true;
  engine.databases.push_back(database);
  return engine;
}

ServerBootstrapConfig Config(const std::filesystem::path& work) {
  ServerBootstrapConfig config;
  config.control_dir = work / "control";
  config.sbps_endpoint = work / "control" / "sbps.sock";
  config.sbps_max_frame_bytes = 4096;
  config.sbps_max_streams = 4;
  return config;
}

bool HasDiagnostic(const std::vector<scratchbird::server::ServerDiagnostic>& diagnostics,
                   std::string_view code) {
  for (const auto& diagnostic : diagnostics) {
    if (diagnostic.code == code) return true;
  }
  return false;
}

std::string ReadFile(const std::filesystem::path& path) {
  std::ifstream in(path);
  Require(static_cast<bool>(in), "failed to read DBLC-013F descriptor file");
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void TestDescriptorWriteAndAdmission() {
  const auto work = MakeTempDir();
  const auto config = Config(work);
  const auto artifacts = Artifacts();
  const auto engine = EngineState();
  auto descriptor = BuildParserServerEndpointDescriptor(config, artifacts, engine);
  std::vector<scratchbird::server::ServerDiagnostic> diagnostics;
  Require(WriteServerIpcEndpointDescriptor(descriptor, &diagnostics),
          "DBLC-013F descriptor write failed");
  Require(diagnostics.empty(), "DBLC-013F descriptor write produced diagnostics");
  const auto descriptor_text = ReadFile(descriptor.descriptor_path);
  Require(Contains(descriptor_text, "endpoint_class=parser_server"),
          "DBLC-013F descriptor missing endpoint class");
  Require(Contains(descriptor_text, "protocol_family=parser_server_ipc"),
          "DBLC-013F descriptor missing protocol family");
  Require(Contains(descriptor_text, "database_uuid=018f58bd-98f0-7000-8000-00000013060f"),
          "DBLC-013F descriptor missing hosted durable database UUID");
  Require(Contains(descriptor_text, "service_ready=true"),
          "DBLC-013F descriptor missing service-ready proof");
#ifndef _WIN32
  struct stat descriptor_stat {};
  Require(::stat(descriptor.descriptor_path.c_str(), &descriptor_stat) == 0,
          "DBLC-013F descriptor stat failed");
  Require((descriptor_stat.st_mode & 0777u) == 0600u,
          "DBLC-013F descriptor permissions are not 0600");
#endif

  auto connect = EvaluateServerIpcEndpointLifecycle(descriptor, ServerIpcEndpointOperation::kConnect);
  Require(connect.admitted && connect.descriptor_valid,
          "DBLC-013F valid connect was not admitted");
  auto hello = EvaluateServerIpcEndpointLifecycle(descriptor, ServerIpcEndpointOperation::kParserHello);
  Require(hello.admitted, "DBLC-013F parser HELLO should be admitted before auth");
  descriptor.authenticated = true;
  descriptor.authorized = true;
  descriptor.session_bound = true;
  auto management =
      EvaluateServerIpcEndpointLifecycle(descriptor, ServerIpcEndpointOperation::kManagementRequest);
  Require(management.admitted, "DBLC-013F authorized management request was refused");
}

void TestLifecycleFailClosedCases() {
  const auto work = MakeTempDir();
  const auto base = BuildParserServerEndpointDescriptor(Config(work), Artifacts(), EngineState());

  auto missing = base;
  missing.descriptor_present = false;
  auto missing_result = EvaluateServerIpcEndpointLifecycle(missing, ServerIpcEndpointOperation::kConnect);
  Require(!missing_result.admitted && missing_result.stale_cleanup_required,
          "DBLC-013F missing descriptor did not require stale cleanup");
  Require(HasDiagnostic(missing_result.diagnostics, "IPC.LIFECYCLE.DESCRIPTOR_MISSING"),
          "DBLC-013F missing descriptor diagnostic absent");

  auto stale = base;
  stale.descriptor_generation = 1;
  auto stale_result = EvaluateServerIpcEndpointLifecycle(stale, ServerIpcEndpointOperation::kConnect);
  Require(!stale_result.admitted && stale_result.stale_cleanup_required,
          "DBLC-013F stale descriptor was admitted");

  auto unsafe = base;
  unsafe.file_mode = 0644;
  auto unsafe_result = EvaluateServerIpcEndpointLifecycle(unsafe, ServerIpcEndpointOperation::kConnect);
  Require(!unsafe_result.admitted &&
              HasDiagnostic(unsafe_result.diagnostics, "IPC.LIFECYCLE.PERMISSION_INVALID"),
          "DBLC-013F unsafe IPC permissions were admitted");

  auto cluster_private = base;
  cluster_private.cluster_private = true;
  cluster_private.cluster_authority_available = false;
  auto cluster_result =
      EvaluateServerIpcEndpointLifecycle(cluster_private, ServerIpcEndpointOperation::kConnect);
  Require(!cluster_result.admitted &&
              HasDiagnostic(cluster_result.diagnostics, "IPC.LIFECYCLE.CLUSTER_AUTHORITY_REQUIRED"),
          "DBLC-013F cluster-private IPC endpoint did not fail closed");

  auto bad_frame = base;
  bad_frame.frame_valid = false;
  auto bad_frame_result =
      EvaluateServerIpcEndpointLifecycle(bad_frame, ServerIpcEndpointOperation::kParserHello);
  Require(!bad_frame_result.admitted &&
              HasDiagnostic(bad_frame_result.diagnostics, "IPC.LIFECYCLE.FRAME_INVALID"),
          "DBLC-013F invalid physical frame was admitted");

  auto bad_schema = base;
  bad_schema.frame_schema_valid = false;
  auto bad_schema_result =
      EvaluateServerIpcEndpointLifecycle(bad_schema, ServerIpcEndpointOperation::kParserHello);
  Require(!bad_schema_result.admitted &&
              HasDiagnostic(bad_schema_result.diagnostics, "IPC.LIFECYCLE.FRAME_SCHEMA_INVALID"),
          "DBLC-013F invalid schema frame was admitted");

  auto unauth = base;
  auto unauth_result =
      EvaluateServerIpcEndpointLifecycle(unauth, ServerIpcEndpointOperation::kAuthenticatedRequest);
  Require(!unauth_result.admitted &&
              HasDiagnostic(unauth_result.diagnostics, "IPC.LIFECYCLE.AUTHENTICATION_REQUIRED"),
          "DBLC-013F unauthenticated request was admitted");

  auto unauthorized = base;
  unauthorized.authenticated = true;
  unauthorized.session_bound = true;
  auto unauthorized_result =
      EvaluateServerIpcEndpointLifecycle(unauthorized, ServerIpcEndpointOperation::kManagementRequest);
  Require(!unauthorized_result.admitted &&
              HasDiagnostic(unauthorized_result.diagnostics, "IPC.LIFECYCLE.AUTHORIZATION_REQUIRED"),
          "DBLC-013F unauthorized management request was admitted");

  auto event_no_session = base;
  event_no_session.authenticated = true;
  auto event_no_session_result =
      EvaluateServerIpcEndpointLifecycle(event_no_session, ServerIpcEndpointOperation::kEventRequest);
  Require(!event_no_session_result.admitted &&
              HasDiagnostic(event_no_session_result.diagnostics, "IPC.LIFECYCLE.SESSION_REQUIRED"),
          "DBLC-013F event request without session was admitted");

  auto backpressure = base;
  backpressure.active_channels = backpressure.max_active_channels;
  auto backpressure_result =
      EvaluateServerIpcEndpointLifecycle(backpressure, ServerIpcEndpointOperation::kParserHello);
  Require(!backpressure_result.admitted && backpressure_result.backpressure_required &&
              HasDiagnostic(backpressure_result.diagnostics, "IPC.LIFECYCLE.BACKPRESSURE"),
          "DBLC-013F backpressure did not refuse new work");

  auto draining = base;
  draining.authenticated = true;
  draining.session_bound = true;
  draining.draining = true;
  auto draining_result =
      EvaluateServerIpcEndpointLifecycle(draining, ServerIpcEndpointOperation::kEventRequest);
  Require(!draining_result.admitted && draining_result.drain_required &&
              HasDiagnostic(draining_result.diagnostics, "IPC.LIFECYCLE.DRAINING"),
          "DBLC-013F draining endpoint admitted new event work");

  auto shutting_down = base;
  shutting_down.shutting_down = true;
  auto shutdown_refusal =
      EvaluateServerIpcEndpointLifecycle(shutting_down, ServerIpcEndpointOperation::kParserHello);
  Require(!shutdown_refusal.admitted && shutdown_refusal.shutdown_required,
          "DBLC-013F shutdown endpoint admitted new parser work");
  auto shutdown_ack =
      EvaluateServerIpcEndpointLifecycle(shutting_down, ServerIpcEndpointOperation::kShutdown);
  Require(shutdown_ack.admitted && shutdown_ack.shutdown_required,
          "DBLC-013F shutdown operation itself was not admitted");

  const auto failed_engine = EngineState(HostedDatabaseState::kFailed, false);
  auto failed_descriptor =
      BuildParserServerEndpointDescriptor(Config(work), Artifacts(), failed_engine);
  auto failed_result =
      EvaluateServerIpcEndpointLifecycle(failed_descriptor, ServerIpcEndpointOperation::kConnect);
  Require(!failed_result.admitted && failed_result.quarantine_required &&
              HasDiagnostic(failed_result.diagnostics, "IPC.LIFECYCLE.QUARANTINE_REQUIRED"),
          "DBLC-013F hosted database failure did not require IPC quarantine");
}

void TestSbpsFrameValidation() {
  ps::FrameHeader header;
  header.message_type = static_cast<std::uint16_t>(ps::MessageType::kPing);
  header.flags = ps::kFlagFinal;
  header.payload_schema_id = ps::kSchemaNone;
  header.request_uuid = ps::MakeUuidV7Bytes();
  const std::vector<std::uint8_t> payload{'o', 'k'};
  const auto encoded = ps::EncodeFrame(header, payload);
  Require(ps::DecodeFrameBytes(encoded, 4096).ok(),
          "DBLC-013F valid SBPS frame did not decode");

  auto bad_magic = encoded;
  bad_magic[0] ^= 0xffu;
  Require(HasDiagnostic(ps::DecodeFrameBytes(bad_magic, 4096).diagnostics,
                        "PARSER_SERVER_IPC.FRAME_MAGIC_INVALID"),
          "DBLC-013F bad SBPS magic diagnostic absent");

  auto bad_flags = encoded;
  bad_flags[15] = 0x80u;
  Require(HasDiagnostic(ps::DecodeFrameBytes(bad_flags, 4096).diagnostics,
                        "PARSER_SERVER_IPC.FRAME_FLAGS_INVALID"),
          "DBLC-013F bad SBPS flags diagnostic absent");

  auto bad_length = encoded;
  bad_length[20] = 0xffu;
  Require(HasDiagnostic(ps::DecodeFrameBytes(bad_length, 4096).diagnostics,
                        "PARSER_SERVER_IPC.FRAME_LENGTH_INVALID"),
          "DBLC-013F bad SBPS length diagnostic absent");

  auto bad_crc = encoded;
  bad_crc.back() ^= 0xffu;
  Require(HasDiagnostic(ps::DecodeFrameBytes(bad_crc, 4096).diagnostics,
                        "PARSER_SERVER_IPC.FRAME_PAYLOAD_CRC_INVALID"),
          "DBLC-013F bad SBPS payload CRC diagnostic absent");
}

bool HasLegacyDiagnostic(const legacy::MessageVectorSet& messages, std::string_view code) {
  for (const auto& diagnostic : messages.diagnostics) {
    if (diagnostic.code == code) return true;
  }
  return false;
}

void TestLegacyParserWorkerPacketValidation() {
  legacy::ParserServerPacket packet;
  packet.opcode = legacy::ParserServerOpcode::kParserHello;
  packet.protocol_version = 1;
  packet.request_id = 42;
  packet.payload = {'o', 'k'};
  legacy::MessageVectorSet messages;
  Require(legacy::DecodePacket(legacy::EncodePacket(packet), &messages).has_value(),
          "DBLC-013F legacy parser worker IPC packet did not decode");

  std::vector<std::uint8_t> truncated{1, 2, 3};
  messages.diagnostics.clear();
  Require(!legacy::DecodePacket(truncated, &messages).has_value(),
          "DBLC-013F truncated parser worker packet was decoded");
  Require(HasLegacyDiagnostic(messages, "PARSER_IPC.FRAME.TRUNCATED"),
          "DBLC-013F truncated parser worker diagnostic absent");

  auto bad_magic = legacy::EncodePacket(packet);
  bad_magic[0] ^= 0xffu;
  messages.diagnostics.clear();
  Require(!legacy::DecodePacket(bad_magic, &messages).has_value(),
          "DBLC-013F bad-magic parser worker packet was decoded");
  Require(HasLegacyDiagnostic(messages, "PARSER_IPC.FRAME.BAD_MAGIC"),
          "DBLC-013F bad-magic parser worker diagnostic absent");

  auto bad_length = legacy::EncodePacket(packet);
  bad_length[20] = 0xffu;
  messages.diagnostics.clear();
  Require(!legacy::DecodePacket(bad_length, &messages).has_value(),
          "DBLC-013F bad-length parser worker packet was decoded");
  Require(HasLegacyDiagnostic(messages, "PARSER_IPC.FRAME.LENGTH_INVALID"),
          "DBLC-013F bad-length parser worker diagnostic absent");
}

bool HasEventVector(const std::vector<scratchbird::server::ParserServerMessageVector>& vectors,
                    std::string_view code) {
  for (const auto& vector : vectors) {
    if (vector.diagnostic_code == code) return true;
  }
  return false;
}

void PrintEventVectors(const std::vector<scratchbird::server::ParserServerMessageVector>& vectors) {
  for (const auto& vector : vectors) {
    std::cerr << vector.diagnostic_code << ':' << vector.safe_message_key << ':' << vector.detail << '\n';
  }
}

ParserServerEventSession EventSession(const std::filesystem::path& database_path,
                                      bool session_bound,
                                      bool draining = false,
                                      std::uint64_t local_transaction_id = 0) {
  ParserServerEventSession session;
  session.parser_channel_uuid = "018f58bd-98f0-7000-8000-0000001306aa";
  session.session_bound = session_bound;
  session.draining = draining;
  session.engine_context.database_path = database_path.string();
  session.engine_context.session_uuid.canonical = "018f58bd-98f0-7000-8000-0000001306bb";
  session.engine_context.principal_uuid.canonical = "018f58bd-98f0-7000-8000-0000001306cc";
  session.engine_context.local_transaction_id = local_transaction_id;
  session.engine_context.security_context_present = true;
  session.engine_context.trust_mode = api::EngineTrustMode::embedded_in_process;
  session.engine_context.trace_tags.push_back("security.fixture_trace_authority");
  session.engine_context.trace_tags.push_back("group:DBA");
  return session;
}

void SeedActiveTransaction(const std::filesystem::path& database_path, std::uint64_t tx) {
  std::ofstream out(database_path.string() + ".sb.crud_events", std::ios::binary | std::ios::app);
  out << "SBCRUD1\tTX_BEGIN\t" << tx << "\tdblc013f_event_ipc\n";
  out << "SBCRUD1\tTX_COMMIT\t" << tx << "\n";
  Require(static_cast<bool>(out), "DBLC-013F failed to seed event IPC transaction evidence");
}

void CreateEventChannel(const ParserServerEventSession& session,
                        const std::string& channel_uuid) {
  api::EngineCreateEventChannelRequest request;
  request.context.request_id = "dblc013f-event-channel-create";
  request.context.database_path = session.engine_context.database_path;
  request.context.session_uuid.canonical = session.engine_context.session_uuid.canonical;
  request.context.principal_uuid.canonical = session.engine_context.principal_uuid.canonical;
  request.context.local_transaction_id = session.engine_context.local_transaction_id;
  request.context.security_context_present = session.engine_context.security_context_present;
  request.context.trust_mode = session.engine_context.trust_mode;
  request.context.trace_tags = session.engine_context.trace_tags;
  request.target_object = {{channel_uuid}, "event_channel"};
  request.option_envelopes.push_back("channel_uuid:" + channel_uuid);
  request.option_envelopes.push_back("channel:dblc013f_event_channel");
  const auto created = api::EngineCreateEventChannel(request);
  Require(created.ok, "DBLC-013F failed to create engine-authorized event channel");
}

void TestEventIpcSessionDrainAndCleanup() {
  const auto work = MakeTempDir();
  const auto database_path = work / "event_ipc.sbdb";
  (void)CreateOpenDatabase(database_path);
  ParserEventNotificationRouter router;
  ParserServerEventIpcRuntime runtime(&router);
  const std::string channel_uuid = "018f58bd-98f0-7000-8000-0000001306ff";

  PsEventSubscribeRequest unbound;
  unbound.request_uuid = "unbound-request";
  unbound.session = EventSession(database_path, false);
  unbound.channel_uuid = "018f58bd-98f0-7000-8000-0000001306dd";
  auto unbound_result = runtime.HandleSubscribe(unbound);
  Require(unbound_result.outcome == "rejected" &&
              HasEventVector(unbound_result.message_vector_set, "PARSER_SERVER_IPC.SESSION_REQUIRED"),
          "DBLC-013F event subscribe without session was not rejected");

  PsEventSubscribeRequest draining;
  draining.request_uuid = "draining-request";
  draining.session = EventSession(database_path, true, true);
  draining.channel_uuid = "018f58bd-98f0-7000-8000-0000001306ee";
  auto draining_result = runtime.HandleSubscribe(draining);
  Require(draining_result.outcome == "rejected" &&
              HasEventVector(draining_result.message_vector_set, "PARSER_SERVER_IPC.DRAINING"),
          "DBLC-013F event subscribe while draining was not rejected");

  SeedActiveTransaction(database_path, 1306);
  CreateEventChannel(EventSession(database_path, true, false, 1306), channel_uuid);

  PsEventSubscribeRequest accepted;
  accepted.request_uuid = "accepted-request";
  accepted.session = EventSession(database_path, true);
  accepted.channel_uuid = channel_uuid;
  auto accepted_result = runtime.HandleSubscribe(accepted);
  if (accepted_result.outcome != "accepted") { PrintEventVectors(accepted_result.message_vector_set); }
  Require(accepted_result.outcome == "accepted",
          "DBLC-013F event subscribe for bound session failed");
  Require(router.ActiveSubscriptionCount() == 1,
          "DBLC-013F event subscription was not registered");

  scratchbird::server::PsEventDisconnectRequest disconnect;
  disconnect.session = accepted.session;
  disconnect.disconnect_reason = "ipc_lifecycle_test";
  auto disconnect_result = runtime.HandleDisconnect(disconnect);
  Require(disconnect_result.outcome == "accepted" && disconnect_result.removed_count == 1,
          "DBLC-013F event disconnect cleanup failed");
  Require(router.ActiveSubscriptionCount() == 0,
          "DBLC-013F event disconnect left stale subscriptions");
}

}  // namespace

int main() {
  TestDescriptorWriteAndAdmission();
  TestLifecycleFailClosedCases();
  TestSbpsFrameValidation();
  TestLegacyParserWorkerPacketValidation();
  TestEventIpcSessionDrainAndCleanup();
  std::cout << "database_lifecycle_ipc_conformance=passed\n";
  return EXIT_SUCCESS;
}
