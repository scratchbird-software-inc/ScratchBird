// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "database_lifecycle.hpp"
#include "database_lifecycle_test_memory.hpp"
#include "local_transaction_store.hpp"
#include "manager_control.hpp"
#include "maintenance_coordinator.hpp"
#include "sblr_dispatch.hpp"
#include "sblr_dispatch_server.hpp"
#include "session_registry.hpp"
#include "transaction_state.hpp"
#include "uuid.hpp"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

namespace {

namespace db = scratchbird::storage::database;
namespace api = scratchbird::engine::internal_api;
namespace sblr = scratchbird::engine::sblr;
namespace tx = scratchbird::transaction::mga;
namespace uuid = scratchbird::core::uuid;
namespace sbps = scratchbird::server::sbps;
using scratchbird::core::platform::UuidKind;
using scratchbird::server::HostedDatabaseSnapshot;
using scratchbird::server::HostedDatabaseState;
using scratchbird::server::HostedEngineState;
using scratchbird::server::ParserPackageRegistry;
using scratchbird::server::ServerBootstrapConfig;
using scratchbird::server::ServerLifecycleArtifacts;
using scratchbird::server::ServerListenerOrchestrator;
using scratchbird::server::ServerMaintenanceCoordinator;
using scratchbird::server::ServerManagementContext;
using scratchbird::server::ServerManagementRequest;
using scratchbird::server::ServerManagementResponse;
using scratchbird::server::ServerObservabilityState;
using scratchbird::server::ServerSessionRegistry;
using scratchbird::server::SessionOperationResult;

constexpr std::string_view kVerifier =
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
constexpr std::string_view kAdminPrincipalUuid =
    "019e108d-1700-7000-8000-0000000000ad";
constexpr std::uint64_t kIterationCount = 32;
constexpr std::uint64_t kFdGrowthAllowance = 8;
constexpr std::uint64_t kResidentSetGrowthAllowanceKiB = 64 * 1024;

struct AttachedSession {
  std::array<std::uint8_t, 16> connection_uuid{};
  std::array<std::uint8_t, 16> auth_context_uuid{};
  std::array<std::uint8_t, 16> session_uuid{};
};

struct ExecuteDecoded {
  std::string outcome;
  std::uint64_t row_count = 0;
  std::string operation_id;
  std::string row_packet;
  std::string detail;
};

struct Fixture {
  std::filesystem::path root;
  std::filesystem::path database_path;
  std::string database_uuid;
};

struct ResourceSnapshot {
  std::uint64_t fd_count = 0;
  std::uint64_t rss_kib = 0;
};

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

std::uint64_t GetU64(const std::vector<std::uint8_t>& data, std::size_t offset) {
  std::uint64_t value = 0;
  for (int byte = 7; byte >= 0; --byte) {
    value <<= 8u;
    value |= data[offset + static_cast<std::size_t>(byte)];
  }
  return value;
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

std::optional<std::uint64_t> TextU64(std::string_view text, std::string_view key) {
  const std::string prefix = std::string(key) + "=";
  std::size_t start = 0;
  while (start <= text.size()) {
    const std::size_t end = text.find('\n', start);
    const std::string_view line =
        text.substr(start, end == std::string_view::npos ? text.size() - start : end - start);
    if (line.starts_with(prefix)) {
      try {
        return static_cast<std::uint64_t>(std::stoull(std::string(line.substr(prefix.size()))));
      } catch (...) {
        return std::nullopt;
      }
    }
    if (end == std::string_view::npos) break;
    start = end + 1;
  }
  return std::nullopt;
}

bool HasDiagnostic(const SessionOperationResult& result, std::string_view code) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.code == code) return true;
  }
  return false;
}

bool HasDiagnostic(const ServerManagementResponse& result, std::string_view code) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.code == code) return true;
  }
  return false;
}

std::filesystem::path MakeTempDir() {
  std::string tmpl = "/tmp/sb_phase7h_stress_resource.XXXXXX";
  std::vector<char> writable(tmpl.begin(), tmpl.end());
  writable.push_back('\0');
  char* made = ::mkdtemp(writable.data());
  Require(made != nullptr, "mkdtemp failed for Phase 7H stress resource test");
  return std::filesystem::path(made);
}

std::uint64_t CountOpenFileDescriptors() {
  std::error_code ignored;
  const std::filesystem::path fd_root("/proc/self/fd");
  if (!std::filesystem::exists(fd_root, ignored)) return 0;
  std::uint64_t count = 0;
  for (const auto& entry : std::filesystem::directory_iterator(fd_root, ignored)) {
    (void)entry;
    ++count;
  }
  return count;
}

std::uint64_t ReadResidentSetKiB() {
  std::ifstream in("/proc/self/status");
  std::string line;
  while (std::getline(in, line)) {
    if (!line.starts_with("VmRSS:")) continue;
    std::istringstream fields(line);
    std::string key;
    std::uint64_t value = 0;
    std::string unit;
    if (fields >> key >> value >> unit) return value;
  }
  return 0;
}

ResourceSnapshot CaptureResources() {
  return {CountOpenFileDescriptors(), ReadResidentSetKiB()};
}

Fixture CreateFixture() {
  Fixture fixture;
  fixture.root = MakeTempDir();
  fixture.database_path = fixture.root / "phase7h_stress_resource.sbdb";

  db::DatabaseCreateConfig create;
  create.path = fixture.database_path.string();
  create.database_uuid =
      uuid::GenerateEngineIdentityV7(UuidKind::database, 1782800000000).value;
  create.filespace_uuid =
      uuid::GenerateEngineIdentityV7(UuidKind::filespace, 1782800000001).value;
  create.page_size = 16384;
  create.creation_unix_epoch_millis = 1782800000002;
  create.allow_minimal_resource_bootstrap = true;
  create.require_resource_seed_pack = false;
  create.allow_overwrite = true;
  const auto created = db::CreateDatabaseFile(create);
  Require(created.ok(), "Phase 7H database create failed");
  const auto opened = db::OpenDatabaseFile({fixture.database_path.string(), false, false, false});
  Require(opened.ok(), "Phase 7H database first open failed");
  const auto clean = db::MarkDatabaseCleanShutdown(fixture.database_path.string());
  Require(clean.ok(), "Phase 7H clean shutdown marker failed");
  fixture.database_uuid = uuid::UuidToString(create.database_uuid.value);

  scratchbird::tests::database_lifecycle::CreateDurableLocalPasswordPrincipal(
      fixture.database_path,
      fixture.database_uuid,
      kAdminPrincipalUuid,
      "admin",
      kVerifier,
      17,
      "Phase 7H");
  return fixture;
}

HostedEngineState MakeEngineState(const Fixture& fixture) {
  HostedEngineState engine_state;
  engine_state.engine_context_active = true;
  HostedDatabaseSnapshot database;
  database.state = HostedDatabaseState::kOpen;
  database.database_open = true;
  database.database_created = true;
  database.database_path = fixture.database_path.string();
  database.database_uuid = fixture.database_uuid;
  database.read_only = false;
  database.write_admission_fenced = false;
  engine_state.databases.push_back(std::move(database));
  return engine_state;
}

std::string Evidence(std::string_view principal) {
  return scratchbird::tests::database_lifecycle::DurableLocalPasswordEvidence(
      principal,
      kAdminPrincipalUuid,
      kVerifier,
      "right:CONNECT,right:CREATE,right:INSERT,right:SELECT,right:OBS_RUNTIME_ALL,"
      "right:OBS_MANAGEMENT_INSPECT,right:OBS_MANAGEMENT_CONTROL");
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
                                           const HostedEngineState& engine_state) {
  AttachedSession attached;
  attached.connection_uuid = sbps::MakeUuidV7Bytes();
  const auto auth = scratchbird::server::HandleAuthHandoff(
      registry,
      engine_state,
      Frame(sbps::MessageType::kAuthHandoff,
            AuthPayload(attached.connection_uuid, "admin"),
            attached.connection_uuid));
  Require(auth.accepted, "Phase 7H auth handoff failed");
  const auto auth_context = scratchbird::server::DecodeAuthContextUuidForTest(auth.payload);
  Require(auth_context.has_value(), "Phase 7H auth context decode failed");
  attached.auth_context_uuid = *auth_context;

  const auto attach = scratchbird::server::HandleAttachDatabase(
      registry,
      engine_state,
      Frame(sbps::MessageType::kAttachDatabase,
            AttachPayload(attached.connection_uuid, attached.auth_context_uuid),
            attached.connection_uuid));
  Require(attach.accepted, "Phase 7H attach failed");
  const auto session_uuid = scratchbird::server::DecodeSessionUuidForTest(attach.payload);
  Require(session_uuid.has_value(), "Phase 7H session UUID decode failed");
  attached.session_uuid = *session_uuid;
  return attached;
}

sbps::Frame ExecuteFrame(const std::array<std::uint8_t, 16>& session_uuid,
                         std::string encoded) {
  return Frame(sbps::MessageType::kExecuteSblr,
               scratchbird::server::EncodeExecuteSblrPayloadForTest(
                   session_uuid, {}, std::move(encoded), false),
               {},
               session_uuid);
}

sbps::Frame DisconnectFrame(const std::array<std::uint8_t, 16>& session_uuid) {
  std::vector<std::uint8_t> payload;
  PutUuid(&payload, session_uuid);
  PutString(&payload, "phase7h_stress_loop_disconnect");
  return Frame(sbps::MessageType::kDisconnectNotice, std::move(payload), {}, session_uuid);
}

std::string TransactionEnvelope(std::string_view operation_id,
                                bool requires_transaction_context) {
  std::string out;
  out += "operation_id=";
  out += operation_id;
  out += "\n";
  out += "sblr_operation_family=sblr.transaction.control.v3\n";
  out += "result_shape=engine.api.result.v1\n";
  out += "diagnostic_shape=engine.diagnostic.v1\n";
  out += "trace_key=CBQ_GATE_STRESS_SOAK_RESOURCE_LEAK\n";
  out += "contains_sql_text=false\n";
  out += "parser_resolved_names_to_uuids=true\n";
  out += "requires_security_context=true\n";
  out += requires_transaction_context ? "requires_transaction_context=true\n"
                                      : "requires_transaction_context=false\n";
  out += "requires_cluster_authority=false\n";
  return out;
}

ExecuteDecoded DecodeExecute(const SessionOperationResult& result) {
  ExecuteDecoded decoded;
  std::size_t offset = 0;
  Require(ReadString(result.payload, &offset, &decoded.outcome), "execute outcome decode failed");
  Require(offset + 32 <= result.payload.size(), "execute UUID decode failed");
  offset += 32;
  Require(offset + 8 <= result.payload.size(), "execute row count decode failed");
  decoded.row_count = GetU64(result.payload, offset);
  offset += 8;
  Require(ReadString(result.payload, &offset, &decoded.operation_id),
          "execute operation id decode failed");
  Require(ReadString(result.payload, &offset, &decoded.row_packet),
          "execute row packet decode failed");
  if (offset < result.payload.size()) {
    Require(ReadString(result.payload, &offset, &decoded.detail), "execute detail decode failed");
  }
  return decoded;
}

SessionOperationResult Execute(ServerSessionRegistry* registry,
                               const HostedEngineState& engine_state,
                               const std::array<std::uint8_t, 16>& session_uuid,
                               std::string encoded) {
  return scratchbird::server::HandleExecuteSblr(
      registry, engine_state, ExecuteFrame(session_uuid, std::move(encoded)));
}

sbps::Frame ManagementFrame(const std::array<std::uint8_t, 16>& session_uuid,
                            std::string_view operation_key) {
  ServerManagementRequest request;
  request.operation_key = std::string(operation_key);
  request.audit_reason = "phase7h_stress_resource_leak";
  sbps::Frame frame;
  frame.header.message_type = static_cast<std::uint16_t>(sbps::MessageType::kManagementRequest);
  frame.header.payload_schema_id = sbps::kSchemaManagementRequestV1;
  frame.header.request_uuid = sbps::MakeUuidV7Bytes();
  frame.header.session_uuid = session_uuid;
  frame.payload = scratchbird::server::EncodeServerManagementRequestForTest(request);
  return frame;
}

ServerManagementContext Context(ServerBootstrapConfig* config,
                                ServerLifecycleArtifacts* artifacts,
                                HostedEngineState* engine_state,
                                ServerSessionRegistry* registry,
                                ParserPackageRegistry* parser_registry,
                                ServerListenerOrchestrator* listeners,
                                ServerMaintenanceCoordinator* coordinator,
                                ServerObservabilityState* observability) {
  ServerManagementContext context;
  context.config = config;
  context.artifacts = artifacts;
  context.engine_state = engine_state;
  context.session_registry = registry;
  context.parser_registry = parser_registry;
  context.listener_orchestrator = listeners;
  context.maintenance_coordinator = coordinator;
  context.observability = observability;
  return context;
}

bool TransactionHasState(const Fixture& fixture,
                         std::uint64_t local_transaction_id,
                         tx::TransactionState expected) {
  const auto loaded = db::LoadLocalTransactionInventoryFromDatabase(fixture.database_path.string());
  Require(loaded.ok(), "Phase 7H transaction inventory load failed");
  const auto lookup = tx::LookupLocalTransaction(
      loaded.inventory, tx::MakeLocalTransactionId(local_transaction_id));
  return lookup.ok() && lookup.entry.state == expected;
}

std::string NewUuidText(UuidKind kind, std::uint64_t millis) {
  return uuid::UuidToString(uuid::GenerateEngineIdentityV7(kind, millis).value.value);
}

api::EngineLocalizedName Name(std::string value) {
  return {"en", "primary", "", std::move(value), true};
}

api::EngineTypedValue TextValue(std::string value) {
  api::EngineTypedValue typed;
  typed.descriptor.descriptor_kind = "scalar";
  typed.descriptor.canonical_type_name = "text";
  typed.descriptor.encoded_descriptor = "type=text";
  typed.encoded_value = std::move(value);
  return typed;
}

api::EngineColumnDefinition TextColumn(std::uint32_t ordinal,
                                       std::string name,
                                       std::string column_uuid,
                                       std::string descriptor_uuid) {
  api::EngineColumnDefinition column;
  column.ordinal = ordinal;
  column.requested_column_uuid.canonical = std::move(column_uuid);
  column.names.push_back(Name(std::move(name)));
  column.descriptor.descriptor_uuid.canonical = std::move(descriptor_uuid);
  column.descriptor.descriptor_kind = "scalar";
  column.descriptor.canonical_type_name = "text";
  column.descriptor.encoded_descriptor = "type=text";
  column.nullable = true;
  return column;
}

api::EngineRowValue Row(std::string row_uuid, std::string id, std::string note) {
  api::EngineRowValue row;
  row.requested_row_uuid.canonical = std::move(row_uuid);
  row.fields.push_back({"id", TextValue(std::move(id))});
  row.fields.push_back({"note", TextValue(std::move(note))});
  return row;
}

api::EngineRequestContext EngineApiContext(const Fixture& fixture,
                                           const scratchbird::server::ServerSessionRecord& session,
                                           std::uint64_t local_transaction_id) {
  api::EngineRequestContext context;
  context.trust_mode = api::EngineTrustMode::server_isolated;
  context.request_id = "phase7h-stress-resource-leak";
  context.database_path = fixture.database_path.string();
  context.database_uuid.canonical = fixture.database_uuid;
  context.principal_uuid.canonical = scratchbird::server::UuidBytesToText(session.principal_uuid);
  context.session_uuid.canonical = scratchbird::server::UuidBytesToText(session.session_uuid);
  context.transaction_uuid.canonical = session.transaction_uuid;
  context.local_transaction_id = local_transaction_id;
  context.snapshot_visible_through_local_transaction_id =
      session.snapshot_visible_through_local_transaction_id;
  context.security_context_present = true;
  context.catalog_generation_id = session.catalog_generation;
  context.security_epoch = session.security_epoch;
  context.resource_epoch = session.resource_epoch;
  context.name_resolution_epoch = session.name_resolution_epoch;
  context.trace_tags.push_back("CBQ_GATE_STRESS_SOAK_RESOURCE_LEAK");
  context.trace_tags.push_back("right:CATALOG_MUTATE");
  scratchbird::tests::database_lifecycle::MaterializeAuthorizationRights(
      &context,
      "phase7h-stress-resource-leak",
      {"CREATE", "INSERT", "SELECT", "OBS_RUNTIME_ALL"});
  return context;
}

sblr::SblrDispatchResult DispatchExact(api::EngineRequestContext context,
                                       std::string operation_id,
                                       std::string opcode,
                                       api::EngineApiRequest request) {
  auto envelope = sblr::MakeSblrEnvelope(operation_id, opcode, "CBQ_GATE_STRESS_SOAK_RESOURCE_LEAK");
  envelope.contains_sql_text = false;
  envelope.parser_resolved_names_to_uuids = true;
  envelope.requires_security_context = true;
  envelope.requires_transaction_context = true;
  request.operation_id = operation_id;
  request.context = context;
  sblr::SblrDispatchRequest dispatch{std::move(context), std::move(envelope), std::move(request)};
  auto result = sblr::DispatchSblrOperation(dispatch);
  if (!result.accepted || !result.envelope_validated || !result.dispatched_to_api ||
      !result.api_result.ok || result.api_result.operation_id != operation_id) {
    std::cerr << sblr::SerializeSblrDispatchResultToJson(result) << '\n';
  }
  Require(result.envelope_validated, "Phase 7H exact SBLR envelope did not validate");
  Require(result.accepted, "Phase 7H exact SBLR dispatch was not accepted");
  Require(result.dispatched_to_api, "Phase 7H exact SBLR dispatch did not reach engine API");
  Require(result.api_result.ok, "Phase 7H exact SBLR engine API route failed");
  Require(result.api_result.operation_id == operation_id,
          "Phase 7H exact SBLR engine API operation id drifted");
  return result;
}

std::string ExactServerEnvelope(std::string_view operation_id,
                                std::string_view opcode,
                                std::string_view family,
                                bool requires_transaction_context) {
  std::string out;
  out += "operation_id=";
  out += operation_id;
  out += "\n";
  out += "opcode=";
  out += opcode;
  out += "\n";
  out += "sblr_operation_family=";
  out += family;
  out += "\n";
  out += "result_shape=engine.api.result.v1\n";
  out += "diagnostic_shape=engine.diagnostic.v1\n";
  out += "trace_key=CBQ_GATE_STRESS_SOAK_RESOURCE_LEAK\n";
  out += "contains_sql_text=false\n";
  out += "parser_resolved_names_to_uuids=true\n";
  out += "requires_security_context=true\n";
  out += requires_transaction_context ? "requires_transaction_context=true\n"
                                      : "requires_transaction_context=false\n";
  out += "requires_cluster_authority=false\n";
  return out;
}

std::string ServerCreateSequenceEnvelope(std::uint64_t index) {
  const std::uint64_t base = 1782800500000 + index * 10;
  std::string out = ExactServerEnvelope("ddl.create_sequence",
                                        "SBLR_DDL_CREATE_SEQUENCE",
                                        "sblr.catalog.mutation.v3",
                                        true);
  out += "sequence_object_uuid=";
  out += NewUuidText(UuidKind::object, base);
  out += "\n";
  out += "sequence_name=phase7h_sequence_";
  out += std::to_string(index);
  out += "\n";
  out += "target_schema_uuid=";
  out += NewUuidText(UuidKind::object, base + 1);
  out += "\n";
  return out;
}

void RunExactDdlDmlApiRoutes(const Fixture& fixture,
                             const scratchbird::server::ServerSessionRecord& session,
                             std::uint64_t local_transaction_id,
                             std::uint64_t index) {
  const std::uint64_t base = 1782801000000 + index * 100;
  const std::string schema_uuid = NewUuidText(UuidKind::object, base);
  const std::string table_uuid = NewUuidText(UuidKind::object, base + 1);
  const std::string row_uuid = NewUuidText(UuidKind::object, base + 6);
  const auto context = EngineApiContext(fixture, session, local_transaction_id);

  api::EngineApiRequest schema;
  schema.target_object.uuid.canonical = schema_uuid;
  schema.target_object.object_kind = "schema";
  schema.localized_names.push_back(Name("phase7h_schema_" + std::to_string(index)));
  const auto created_schema = DispatchExact(context,
                                            "ddl.create_schema",
                                            "SBLR_DDL_CREATE_SCHEMA",
                                            std::move(schema));
  Require(created_schema.api_result.primary_object.uuid.canonical == schema_uuid,
          "Phase 7H exact schema create did not preserve UUID");

  api::EngineApiRequest table;
  table.target_schema.uuid.canonical = schema_uuid;
  table.target_schema.object_kind = "schema";
  table.target_object.uuid.canonical = table_uuid;
  table.target_object.object_kind = "table";
  table.localized_names.push_back(Name("phase7h_table_" + std::to_string(index)));
  table.columns.push_back(TextColumn(0,
                                     "id",
                                     NewUuidText(UuidKind::object, base + 2),
                                     NewUuidText(UuidKind::object, base + 3)));
  table.columns.push_back(TextColumn(1,
                                     "note",
                                     NewUuidText(UuidKind::object, base + 4),
                                     NewUuidText(UuidKind::object, base + 5)));
  const auto created_table = DispatchExact(context,
                                           "ddl.create_table",
                                           "SBLR_DDL_CREATE_TABLE",
                                           std::move(table));
  Require(created_table.api_result.primary_object.uuid.canonical == table_uuid,
          "Phase 7H exact table create did not preserve UUID");

  api::EngineApiRequest insert;
  insert.target_object.uuid.canonical = table_uuid;
  insert.target_object.object_kind = "table";
  insert.rows.push_back(Row(row_uuid,
                            std::to_string(index),
                            "phase7h-exact-dml-" + std::to_string(index)));
  const auto inserted = DispatchExact(context,
                                      "dml.insert_rows",
                                      "SBLR_DML_INSERT_ROWS",
                                      std::move(insert));
  Require(inserted.api_result.result_shape.rows.size() == 1,
          "Phase 7H exact dml.insert_rows did not report one row");

  api::EngineApiRequest select;
  select.target_object.uuid.canonical = table_uuid;
  select.target_object.object_kind = "table";
  const auto selected = DispatchExact(context,
                                      "dml.select_rows",
                                      "SBLR_DML_SELECT_ROWS",
                                      std::move(select));
  Require(!selected.api_result.result_shape.rows.empty(),
          "Phase 7H exact dml.select_rows did not return visible rows");
}

void RunStressIteration(const Fixture& fixture, std::uint64_t index) {
  auto engine_state = MakeEngineState(fixture);
  ServerSessionRegistry registry;
  const auto attached = AttachAuthenticatedSession(&registry, engine_state);
  Require(registry.sessions_by_uuid.size() == 1, "Phase 7H attach leaked session count");
  Require(registry.auth_contexts_by_uuid.size() == 1, "Phase 7H attach leaked auth count");

  ServerBootstrapConfig config;
  config.database_default_path = fixture.database_path;
  config.control_dir = fixture.root / "control";
  config.sbps_enabled = true;
  ServerLifecycleArtifacts artifacts;
  artifacts.generation = index + 1;
  artifacts.state = "phase7h-stress";
  ParserPackageRegistry parser_registry;
  ServerListenerOrchestrator listeners;
  auto coordinator = scratchbird::server::BuildMaintenanceCoordinator(config, artifacts);
  ServerObservabilityState observability;
  auto context = Context(&config,
                         &artifacts,
                         &engine_state,
                         &registry,
                         &parser_registry,
                         &listeners,
                         &coordinator,
                         &observability);

  const auto status = scratchbird::server::HandleServerManagementRequest(
      context, ManagementFrame(attached.session_uuid, "status"));
  Require(status.accepted && !status.error, "Phase 7H management status failed under loop");
  Require(!HasDiagnostic(status, "SECURITY.ACCESS_DENIED"),
          "Phase 7H admin status was denied during stress loop");

  const auto show = Execute(&registry,
                            engine_state,
                            attached.session_uuid,
                            scratchbird::server::EncodeShowVersionSblrForTest());
  Require(show.accepted, "Phase 7H SBLR show-version execute failed");
  Require(DecodeExecute(show).operation_id == "observability.show_version",
          "Phase 7H show-version operation drifted");

  const auto begin = Execute(&registry,
                             engine_state,
                             attached.session_uuid,
                             scratchbird::server::EncodeBeginTransactionSblrForTest());
  Require(begin.accepted, "Phase 7H transaction begin failed");
  const auto begin_decoded = DecodeExecute(begin);
  Require(begin_decoded.operation_id == "transaction.begin",
          "Phase 7H transaction begin operation drifted");
  const auto local_id = TextU64(begin_decoded.row_packet, "local_transaction_id");
  Require(local_id.has_value() && *local_id != 0,
          "Phase 7H transaction begin did not return local id");
  Require(TransactionHasState(fixture, *local_id, tx::TransactionState::active),
          "Phase 7H MGA transaction was not active after begin");

  const auto sequence = Execute(&registry,
                                engine_state,
                                attached.session_uuid,
                                ServerCreateSequenceEnvelope(index));
  Require(sequence.accepted, "Phase 7H server exact DDL create sequence route failed");
  Require(DecodeExecute(sequence).operation_id == "ddl.create_sequence",
          "Phase 7H server exact DDL operation drifted");

  const auto session_it = registry.sessions_by_uuid.find(
      scratchbird::server::UuidBytesToText(attached.session_uuid));
  Require(session_it != registry.sessions_by_uuid.end(),
          "Phase 7H session missing for exact DDL/DML API routes");
  RunExactDdlDmlApiRoutes(fixture, session_it->second, *local_id, index);

  // The server helper aliases below remain in the loop for route resource
  // stability. The exact public DML operation ids dml.insert_rows and
  // dml.select_rows are exercised above through SBLR dispatch because this
  // server text-envelope fixture does not carry typed table column vectors.
  const auto insert = Execute(&registry,
                              engine_state,
                              attached.session_uuid,
                              scratchbird::server::EncodeCrudInsertSblrForTest());
  Require(insert.accepted, "Phase 7H DML insert SBLR route failed");
  Require(DecodeExecute(insert).operation_id == "dml.insert",
          "Phase 7H DML insert operation drifted");

  const auto select = Execute(&registry,
                              engine_state,
                              attached.session_uuid,
                              scratchbird::server::EncodeCrudSelectSblrForTest());
  Require(select.accepted, "Phase 7H DML select SBLR route failed");
  Require(DecodeExecute(select).operation_id == "dml.select",
          "Phase 7H DML select operation drifted");

  const auto commit = Execute(&registry,
                              engine_state,
                              attached.session_uuid,
                              TransactionEnvelope("transaction.commit", true));
  Require(commit.accepted, "Phase 7H transaction commit failed");
  Require(DecodeExecute(commit).operation_id == "transaction.commit",
          "Phase 7H transaction commit operation drifted");
  Require(TransactionHasState(fixture, *local_id, tx::TransactionState::committed),
          "Phase 7H MGA transaction was not committed after commit");
  Require(registry.sessions_by_uuid.begin()->second.local_transaction_id != 0,
          "Phase 7H session did not publish replacement transaction id after commit");
  Require(registry.sessions_by_uuid.begin()->second.local_transaction_id != *local_id,
          "Phase 7H replacement transaction id did not advance after commit");

  const auto disconnect = scratchbird::server::HandleDisconnectNotice(
      &registry, DisconnectFrame(attached.session_uuid));
  Require(disconnect.accepted, "Phase 7H disconnect failed");
  Require(HasDiagnostic(disconnect, "ENGINE.DBLC_DETACH_CLEANUP_COMPLETE"),
          "Phase 7H disconnect cleanup diagnostic missing");
  Require(registry.sessions_by_uuid.empty(), "Phase 7H session leaked after disconnect");
  Require(registry.auth_contexts_by_uuid.empty(), "Phase 7H auth context leaked after disconnect");
  Require(registry.cursors_by_uuid.empty(), "Phase 7H cursor leaked after non-cursor loop");
}

void AssertResourceGrowthBounded(const ResourceSnapshot& before,
                                 const ResourceSnapshot& after) {
  if (before.fd_count != 0 && after.fd_count != 0) {
    Require(after.fd_count <= before.fd_count + kFdGrowthAllowance,
            "Phase 7H file descriptor growth exceeded bounded allowance");
  }
  if (before.rss_kib != 0 && after.rss_kib != 0) {
    Require(after.rss_kib <= before.rss_kib + kResidentSetGrowthAllowanceKiB,
            "Phase 7H resident set growth exceeded bounded allowance");
  }
}

void AssertNoRuntimeOrphans(const Fixture& fixture) {
  const auto clean = db::MarkDatabaseCleanShutdown(fixture.database_path.string());
  Require(clean.ok(), "Phase 7H final clean shutdown marker failed");

  const std::filesystem::path control = fixture.root / "control";
  std::error_code ignored;
  if (std::filesystem::exists(control, ignored)) {
    for (const auto& entry : std::filesystem::directory_iterator(control, ignored)) {
      const auto name = entry.path().filename().string();
      Require(!Contains(name, ".sock") && name != "sb_server.pid",
              "Phase 7H left a server runtime socket or pid artifact");
    }
  }
}

}  // namespace

int main() {
  scratchbird::tests::database_lifecycle::ConfigureLifecycleMemoryFixture(
      "database_lifecycle_bounded_stress_resource_leak_conformance");
  auto fixture = CreateFixture();

  RunStressIteration(fixture, 0);
  const auto before = CaptureResources();
  for (std::uint64_t index = 1; index <= kIterationCount; ++index) {
    RunStressIteration(fixture, index);
  }
  const auto after = CaptureResources();

  AssertResourceGrowthBounded(before, after);
  AssertNoRuntimeOrphans(fixture);

  const auto fd_delta = after.fd_count >= before.fd_count ? after.fd_count - before.fd_count : 0;
  const auto rss_delta = after.rss_kib >= before.rss_kib ? after.rss_kib - before.rss_kib : 0;
  std::cout << "CBQ_GATE_STRESS_SOAK_RESOURCE_LEAK=passed iterations=" << kIterationCount
            << " fd_before=" << before.fd_count << " fd_after=" << after.fd_count
            << " fd_delta=" << fd_delta << " rss_kib_before=" << before.rss_kib
            << " rss_kib_after=" << after.rss_kib << " rss_kib_delta=" << rss_delta << '\n';

  std::filesystem::remove_all(fixture.root);
  return EXIT_SUCCESS;
}
