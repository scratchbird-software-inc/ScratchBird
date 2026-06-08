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
#include "management/support_bundle_api.hpp"
#include "manager_control.hpp"
#include "maintenance_coordinator.hpp"
#include "observability/performance_optimization_surface.hpp"
#include "sblr_dispatch.hpp"
#include "sblr_dispatch_server.hpp"
#include "server_observability.hpp"
#include "session_registry.hpp"
#include "transaction_state.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <array>
#include <chrono>
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
#include <utility>
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
    "019e108d-1700-7000-8000-0000000000af";
constexpr std::string_view kGateSearchKey =
    "DPC_SOAK_LEAK_RESOURCE_STABILITY_GATE";
constexpr std::uint64_t kIterationCount = 36;
constexpr std::uint64_t kFdGrowthAllowance = 8;
constexpr std::uint64_t kResidentSetGrowthAllowanceKiB = 64 * 1024;
constexpr std::uint64_t kThreadGrowthAllowance = 2;
constexpr std::uint64_t kDatabaseTreeGrowthAllowanceBytes = 32 * 1024 * 1024;
constexpr std::uint64_t kDatabaseTreeFileGrowthAllowance = 16;
constexpr double kForegroundP95AllowanceMillis = 5000.0;

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
  std::uint64_t thread_count = 0;
  std::uint64_t database_tree_bytes = 0;
  std::uint64_t database_tree_files = 0;
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

bool HasEvidence(const api::EngineApiResult& result,
                 std::string_view kind,
                 std::string_view id = {}) {
  for (const auto& evidence : result.evidence) {
    if (evidence.evidence_kind == kind &&
        (id.empty() || evidence.evidence_id == id)) {
      return true;
    }
  }
  return false;
}

std::filesystem::path MakeTempDir() {
  std::string tmpl = "/tmp/sb_dpc070_stress_resource.XXXXXX";
  std::vector<char> writable(tmpl.begin(), tmpl.end());
  writable.push_back('\0');
  char* made = ::mkdtemp(writable.data());
  Require(made != nullptr, "mkdtemp failed for DPC-070 stress resource test");
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

std::uint64_t ReadThreadCount() {
  std::ifstream in("/proc/self/status");
  std::string line;
  while (std::getline(in, line)) {
    if (!line.starts_with("Threads:")) continue;
    std::istringstream fields(line);
    std::string key;
    std::uint64_t value = 0;
    if (fields >> key >> value) return value;
  }
  return 0;
}

std::pair<std::uint64_t, std::uint64_t> DatabaseTreeStats(const Fixture& fixture) {
  std::uint64_t bytes = 0;
  std::uint64_t files = 0;
  std::error_code ignored;
  for (const auto& entry : std::filesystem::directory_iterator(fixture.root, ignored)) {
    if (!entry.is_regular_file(ignored)) continue;
    const auto name = entry.path().filename().string();
    if (name.rfind(fixture.database_path.filename().string(), 0) != 0) continue;
    ++files;
    bytes += entry.file_size(ignored);
  }
  return {bytes, files};
}

ResourceSnapshot CaptureResources(const Fixture& fixture) {
  const auto [database_tree_bytes, database_tree_files] = DatabaseTreeStats(fixture);
  return {CountOpenFileDescriptors(),
          ReadResidentSetKiB(),
          ReadThreadCount(),
          database_tree_bytes,
          database_tree_files};
}

double P95Millis(std::vector<double> samples) {
  Require(!samples.empty(), "DPC-070 foreground latency sample set is empty");
  std::sort(samples.begin(), samples.end());
  const std::size_t index = (samples.size() * 95 + 99) / 100 - 1;
  return samples[index < samples.size() ? index : samples.size() - 1];
}

std::string MillisText(double value) {
  const auto rounded = static_cast<std::uint64_t>(value * 100.0 + 0.5);
  return std::to_string(rounded / 100) + "." +
         (rounded % 100 < 10 ? "0" : "") + std::to_string(rounded % 100);
}

Fixture CreateFixture() {
  Fixture fixture;
  fixture.root = MakeTempDir();
  fixture.database_path = fixture.root / "dpc070_stress_resource.sbdb";

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
  Require(created.ok(), "DPC-070 database create failed");
  const auto opened = db::OpenDatabaseFile({fixture.database_path.string(), false, false, false});
  Require(opened.ok(), "DPC-070 database first open failed");
  const auto clean = db::MarkDatabaseCleanShutdown(fixture.database_path.string());
  Require(clean.ok(), "DPC-070 clean shutdown marker failed");
  fixture.database_uuid = uuid::UuidToString(create.database_uuid.value);

  scratchbird::tests::database_lifecycle::CreateDurableLocalPasswordPrincipal(
      fixture.database_path,
      fixture.database_uuid,
      kAdminPrincipalUuid,
      "admin",
      kVerifier,
      17,
      "DPC-070");
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
      "right:OBS_MANAGEMENT_INSPECT,right:OBS_MANAGEMENT_CONTROL,"
      "right:SUPPORT_EXPORT,right:BACKUP_CREATE,right:BACKUP_CONTROL");
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
  Require(auth.accepted, "DPC-070 auth handoff failed");
  const auto auth_context = scratchbird::server::DecodeAuthContextUuidForTest(auth.payload);
  Require(auth_context.has_value(), "DPC-070 auth context decode failed");
  attached.auth_context_uuid = *auth_context;

  const auto attach = scratchbird::server::HandleAttachDatabase(
      registry,
      engine_state,
      Frame(sbps::MessageType::kAttachDatabase,
            AttachPayload(attached.connection_uuid, attached.auth_context_uuid),
            attached.connection_uuid));
  Require(attach.accepted, "DPC-070 attach failed");
  const auto session_uuid = scratchbird::server::DecodeSessionUuidForTest(attach.payload);
  Require(session_uuid.has_value(), "DPC-070 session UUID decode failed");
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
  PutString(&payload, "dpc070_stress_loop_disconnect");
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
  out += "trace_key=DPC_SOAK_LEAK_RESOURCE_STABILITY_GATE\n";
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
                            std::string_view operation_key,
                            std::string_view mode = {}) {
  ServerManagementRequest request;
  request.operation_key = std::string(operation_key);
  request.mode = std::string(mode);
  request.audit_reason = "dpc070_stress_resource_leak";
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
  Require(loaded.ok(), "DPC-070 transaction inventory load failed");
  const auto lookup = tx::LookupLocalTransaction(
      loaded.inventory, tx::MakeLocalTransactionId(local_transaction_id));
  return lookup.ok() && lookup.entry.state == expected;
}

std::string NewUuidText(UuidKind kind, std::uint64_t millis) {
  return uuid::UuidToString(uuid::GenerateEngineIdentityV7(kind, millis).value.value);
}

std::string AnyUuidText(UuidKind kind, std::uint64_t millis) {
  if (uuid::UuidKindAllowsDurableIdentity(kind)) {
    return NewUuidText(kind, millis);
  }
  const auto raw = uuid::GenerateCompatibilityUnixTimeV7(millis);
  Require(raw.ok(), "DPC-070 compatibility UUID generation failed");
  const auto typed = uuid::MakeTypedUuid(kind, raw.value);
  Require(typed.ok(), "DPC-070 typed UUID generation failed");
  return uuid::UuidToString(typed.value.value);
}

api::EngineRequestContext ObservabilityContext(const Fixture& fixture,
                                               std::uint64_t local_transaction_id) {
  api::EngineRequestContext context;
  context.trust_mode = api::EngineTrustMode::server_isolated;
  context.request_id = "dpc070-soak-resource-stability-observability";
  context.database_path = fixture.database_path.string();
  context.database_uuid.canonical = fixture.database_uuid;
  context.node_uuid.canonical = NewUuidText(UuidKind::object, 1782809000001);
  context.session_uuid.canonical = AnyUuidText(UuidKind::session, 1782809000002);
  context.principal_uuid.canonical = NewUuidText(UuidKind::principal, 1782809000003);
  context.transaction_uuid.canonical = NewUuidText(UuidKind::transaction, 1782809000004);
  context.local_transaction_id = local_transaction_id;
  context.catalog_generation_id = 7070;
  context.name_resolution_epoch = 7071;
  context.security_epoch = 7072;
  context.resource_epoch = 7073;
  context.security_context_present = true;
  context.trace_tags = {
      "right:OBS_MANAGEMENT_INSPECT",
      "right:OBS_CONFIG_INSPECT",
      "right:OBS_AGENT_STATE_READ",
      "right:OBS_METRICS_READ_ALL",
      "right:SUPPORT_EXPORT",
      "dpc_soak_leak_resource_stability_gate"};
  scratchbird::tests::database_lifecycle::MaterializeAuthorizationRights(
      &context,
      "dpc070-soak-resource-stability-observability",
      {"OBS_MANAGEMENT_INSPECT",
       "OBS_CONFIG_INSPECT",
       "OBS_AGENT_STATE_READ",
       "OBS_METRICS_READ_ALL",
       "OBS_INDEX_PROFILE_READ",
       "MGA_CLEANUP_INSPECT",
       "SUPPORT_EXPORT"});
  return context;
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
  context.request_id = "dpc070-stress-resource-leak";
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
  context.trace_tags.push_back("DPC_SOAK_LEAK_RESOURCE_STABILITY_GATE");
  context.trace_tags.push_back("right:CATALOG_MUTATE");
  scratchbird::tests::database_lifecycle::MaterializeAuthorizationRights(
      &context,
      "dpc070-stress-resource-leak",
      {"CREATE", "INSERT", "SELECT", "OBS_RUNTIME_ALL"});
  return context;
}

sblr::SblrDispatchResult DispatchExact(api::EngineRequestContext context,
                                       std::string operation_id,
                                       std::string opcode,
                                       api::EngineApiRequest request) {
  auto envelope = sblr::MakeSblrEnvelope(operation_id, opcode, "DPC_SOAK_LEAK_RESOURCE_STABILITY_GATE");
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
  Require(result.envelope_validated, "DPC-070 exact SBLR envelope did not validate");
  Require(result.accepted, "DPC-070 exact SBLR dispatch was not accepted");
  Require(result.dispatched_to_api, "DPC-070 exact SBLR dispatch did not reach engine API");
  Require(result.api_result.ok, "DPC-070 exact SBLR engine API route failed");
  Require(result.api_result.operation_id == operation_id,
          "DPC-070 exact SBLR engine API operation id drifted");
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
  out += "trace_key=DPC_SOAK_LEAK_RESOURCE_STABILITY_GATE\n";
  out += "contains_sql_text=false\n";
  out += "parser_resolved_names_to_uuids=true\n";
  out += "requires_security_context=true\n";
  out += requires_transaction_context ? "requires_transaction_context=true\n"
                                      : "requires_transaction_context=false\n";
  out += "requires_cluster_authority=false\n";
  return out;
}

std::string ServerCreateSequenceEnvelope(std::uint64_t index, bool autocommit_emulation = false) {
  const std::uint64_t base = 1782800500000 + index * 10;
  std::string out = ExactServerEnvelope("ddl.create_sequence",
                                        "SBLR_DDL_CREATE_SEQUENCE",
                                        "sblr.catalog.mutation.v3",
                                        true);
  out += "sequence_object_uuid=";
  out += NewUuidText(UuidKind::object, base);
  out += "\n";
  out += "sequence_name=dpc070_sequence_";
  out += std::to_string(index);
  out += "\n";
  out += "target_schema_uuid=";
  out += NewUuidText(UuidKind::object, base + 1);
  out += "\n";
  if (autocommit_emulation) {
    out += "autocommit_emulation=true\n";
  }
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
  schema.localized_names.push_back(Name("dpc070_schema_" + std::to_string(index)));
  const auto created_schema = DispatchExact(context,
                                            "ddl.create_schema",
                                            "SBLR_DDL_CREATE_SCHEMA",
                                            std::move(schema));
  Require(created_schema.api_result.primary_object.uuid.canonical == schema_uuid,
          "DPC-070 exact schema create did not preserve UUID");

  api::EngineApiRequest table;
  table.target_schema.uuid.canonical = schema_uuid;
  table.target_schema.object_kind = "schema";
  table.target_object.uuid.canonical = table_uuid;
  table.target_object.object_kind = "table";
  table.localized_names.push_back(Name("dpc070_table_" + std::to_string(index)));
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
          "DPC-070 exact table create did not preserve UUID");

  api::EngineApiRequest insert;
  insert.target_object.uuid.canonical = table_uuid;
  insert.target_object.object_kind = "table";
  insert.rows.push_back(Row(row_uuid,
                            std::to_string(index),
                            "dpc070-exact-dml-" + std::to_string(index)));
  const auto inserted = DispatchExact(context,
                                      "dml.insert_rows",
                                      "SBLR_DML_INSERT_ROWS",
                                      std::move(insert));
  Require(inserted.api_result.result_shape.rows.size() == 1,
          "DPC-070 exact dml.insert_rows did not report one row");

  api::EngineApiRequest select;
  select.target_object.uuid.canonical = table_uuid;
  select.target_object.object_kind = "table";
  const auto selected = DispatchExact(context,
                                      "dml.select_rows",
                                      "SBLR_DML_SELECT_ROWS",
                                      std::move(select));
  Require(!selected.api_result.result_shape.rows.empty(),
          "DPC-070 exact dml.select_rows did not return visible rows");
}

void RunStressIteration(const Fixture& fixture, std::uint64_t index) {
  auto engine_state = MakeEngineState(fixture);
  ServerSessionRegistry registry;
  const auto attached = AttachAuthenticatedSession(&registry, engine_state);
  Require(registry.sessions_by_uuid.size() == 1, "DPC-070 attach leaked session count");
  Require(registry.auth_contexts_by_uuid.size() == 1, "DPC-070 attach leaked auth count");

  ServerBootstrapConfig config;
  config.database_default_path = fixture.database_path;
  config.control_dir = fixture.root / "control";
  config.data_dir = fixture.root / "data";
  config.log_file = (config.control_dir / "sb_server.log").string();
  config.sbps_enabled = true;
  config.metrics_enabled = true;
  ServerLifecycleArtifacts artifacts;
  artifacts.generation = index + 1;
  artifacts.state = "dpc070-stress";
  ParserPackageRegistry parser_registry;
  parser_registry.entries.push_back({});
  ServerListenerOrchestrator listeners;
  listeners.profiles.push_back({});
  auto coordinator = scratchbird::server::BuildMaintenanceCoordinator(config, artifacts);
  auto observability =
      scratchbird::server::InitializeServerObservability(config,
                                                         artifacts,
                                                         engine_state,
                                                         parser_registry,
                                                         listeners);
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
  Require(status.accepted && !status.error, "DPC-070 management status failed under loop");
  Require(!HasDiagnostic(status, "SECURITY.ACCESS_DENIED"),
          "DPC-070 admin status was denied during stress loop");

  const auto begin_backup = scratchbird::server::HandleServerManagementRequest(
      context, ManagementFrame(attached.session_uuid, "begin_backup_fence"));
  Require(begin_backup.accepted && !begin_backup.error && coordinator.backup_fence_active,
          "DPC-070 backup-fence maintenance route failed under loop");
  const auto end_backup = scratchbird::server::HandleServerManagementRequest(
      context, ManagementFrame(attached.session_uuid, "end_backup_fence"));
  Require(end_backup.accepted && !end_backup.error && !coordinator.backup_fence_active,
          "DPC-070 backup-fence maintenance route did not clear");

  const auto support_export = scratchbird::server::HandleServerManagementRequest(
      context, ManagementFrame(attached.session_uuid, "export_server_support_bundle"));
  Require(support_export.accepted && !support_export.error,
          "DPC-070 server support-bundle export route failed under loop");
  const std::string support_payload(support_export.payload.begin(), support_export.payload.end());
  Require(Contains(support_payload, "support_bundle_uuid"),
          "DPC-070 server support-bundle export omitted bundle evidence");
  Require(!Contains(support_payload, fixture.root.string()),
          "DPC-070 server support-bundle export leaked local fixture path");

  const auto show = Execute(&registry,
                            engine_state,
                            attached.session_uuid,
                            scratchbird::server::EncodeShowVersionSblrForTest());
  Require(show.accepted, "DPC-070 SBLR show-version execute failed");
  Require(DecodeExecute(show).operation_id == "observability.show_version",
          "DPC-070 show-version operation drifted");

  const auto begin = Execute(&registry,
                             engine_state,
                             attached.session_uuid,
                             scratchbird::server::EncodeBeginTransactionSblrForTest());
  Require(begin.accepted, "DPC-070 transaction begin failed");
  const auto begin_decoded = DecodeExecute(begin);
  Require(begin_decoded.operation_id == "transaction.begin",
          "DPC-070 transaction begin operation drifted");
  const auto local_id = TextU64(begin_decoded.row_packet, "local_transaction_id");
  Require(local_id.has_value() && *local_id != 0,
          "DPC-070 transaction begin did not return local id");
  Require(TransactionHasState(fixture, *local_id, tx::TransactionState::active),
          "DPC-070 MGA transaction was not active after begin");

  const auto sequence = Execute(&registry,
                                engine_state,
                                attached.session_uuid,
                                ServerCreateSequenceEnvelope(index));
  Require(sequence.accepted, "DPC-070 server exact DDL create sequence route failed");
  Require(DecodeExecute(sequence).operation_id == "ddl.create_sequence",
          "DPC-070 server exact DDL operation drifted");

  const auto session_it = registry.sessions_by_uuid.find(
      scratchbird::server::UuidBytesToText(attached.session_uuid));
  Require(session_it != registry.sessions_by_uuid.end(),
          "DPC-070 session missing for exact DDL/DML API routes");
  RunExactDdlDmlApiRoutes(fixture, session_it->second, *local_id, index);

  // The server helper aliases below remain in the loop for route resource
  // stability. The exact public DML operation ids dml.insert_rows and
  // dml.select_rows are exercised above through SBLR dispatch because this
  // server text-envelope fixture does not carry typed table column vectors.
  const auto insert = Execute(&registry,
                              engine_state,
                              attached.session_uuid,
                              scratchbird::server::EncodeCrudInsertSblrForTest());
  Require(insert.accepted, "DPC-070 DML insert SBLR route failed");
  Require(DecodeExecute(insert).operation_id == "dml.insert",
          "DPC-070 DML insert operation drifted");

  const auto select = Execute(&registry,
                              engine_state,
                              attached.session_uuid,
                              scratchbird::server::EncodeCrudSelectSblrForTest());
  Require(select.accepted, "DPC-070 DML select SBLR route failed");
  Require(DecodeExecute(select).operation_id == "dml.select",
          "DPC-070 DML select operation drifted");

  const auto update = Execute(&registry,
                              engine_state,
                              attached.session_uuid,
                              scratchbird::server::EncodeCrudUpdateSblrForTest());
  Require(update.accepted, "DPC-070 DML update SBLR route failed");
  Require(DecodeExecute(update).operation_id == "dml.update",
          "DPC-070 DML update operation drifted");

  const auto delete_result = Execute(&registry,
                                     engine_state,
                                     attached.session_uuid,
                                     scratchbird::server::EncodeCrudDeleteSblrForTest());
  Require(delete_result.accepted, "DPC-070 DML delete SBLR route failed");
  Require(DecodeExecute(delete_result).operation_id == "dml.delete",
          "DPC-070 DML delete operation drifted");

  if (index % 3 == 1) {
    const auto rollback = Execute(&registry,
                                  engine_state,
                                  attached.session_uuid,
                                  TransactionEnvelope("transaction.rollback", true));
    Require(rollback.accepted, "DPC-070 transaction rollback failed");
    Require(DecodeExecute(rollback).operation_id == "transaction.rollback",
            "DPC-070 transaction rollback operation drifted");
    Require(TransactionHasState(fixture, *local_id, tx::TransactionState::rolled_back),
            "DPC-070 MGA transaction was not rolled back after rollback");
  } else if (index % 3 == 2) {
    const auto autocommit = Execute(&registry,
                                    engine_state,
                                    attached.session_uuid,
                                    ServerCreateSequenceEnvelope(index + 10000, true));
    Require(autocommit.accepted, "DPC-070 autocommit-emulated sequence route failed");
    const auto autocommit_decoded = DecodeExecute(autocommit);
    Require(autocommit_decoded.operation_id == "ddl.create_sequence",
            "DPC-070 autocommit sequence operation drifted");
    Require(Contains(autocommit_decoded.row_packet,
                     "evidence=autocommit_statement_succeeded:committed"),
            "DPC-070 autocommit route did not publish commit evidence");
    Require(TransactionHasState(fixture, *local_id, tx::TransactionState::committed),
            "DPC-070 autocommit route did not commit through MGA inventory");
  } else {
    const auto commit = Execute(&registry,
                                engine_state,
                                attached.session_uuid,
                                TransactionEnvelope("transaction.commit", true));
    Require(commit.accepted, "DPC-070 transaction commit failed");
    Require(DecodeExecute(commit).operation_id == "transaction.commit",
            "DPC-070 transaction commit operation drifted");
    Require(TransactionHasState(fixture, *local_id, tx::TransactionState::committed),
            "DPC-070 MGA transaction was not committed after commit");
  }
  Require(registry.sessions_by_uuid.begin()->second.local_transaction_id != 0,
          "DPC-070 session did not publish replacement transaction id after finality");
  Require(registry.sessions_by_uuid.begin()->second.local_transaction_id != *local_id,
          "DPC-070 replacement transaction id did not advance after finality");
  Require(TransactionHasState(fixture,
                              registry.sessions_by_uuid.begin()->second.local_transaction_id,
                              tx::TransactionState::active),
          "DPC-070 replacement transaction is not active in engine inventory");

  const auto disconnect = scratchbird::server::HandleDisconnectNotice(
      &registry, DisconnectFrame(attached.session_uuid));
  Require(disconnect.accepted, "DPC-070 disconnect failed");
  Require(HasDiagnostic(disconnect, "ENGINE.DBLC_DETACH_CLEANUP_COMPLETE"),
          "DPC-070 disconnect cleanup diagnostic missing");
  Require(registry.sessions_by_uuid.empty(), "DPC-070 session leaked after disconnect");
  Require(registry.auth_contexts_by_uuid.empty(), "DPC-070 auth context leaked after disconnect");
  Require(registry.cursors_by_uuid.empty(), "DPC-070 cursor leaked after non-cursor loop");
  Require(registry.job_schedulers_by_database_uuid.empty(),
          "DPC-070 scheduler queue proxy leaked after loop");
  Require(registry.job_quotas_by_database_uuid.empty(),
          "DPC-070 quota reservation proxy leaked after loop");
}

api::PerformanceOptimizationSurfaceSnapshot SoakSurfaceSnapshot(
    const ResourceSnapshot& before,
    const ResourceSnapshot& after,
    double foreground_p95_ms) {
  auto snapshot = api::DefaultPerformanceOptimizationSurfaceSnapshot();
  snapshot.optimization_profile = "dpc070_all_optimizations_soak";
  snapshot.native_ingest_enabled = true;
  snapshot.copy_batching_status = "route_pressure_completed";
  snapshot.copy_batches_started = kIterationCount;
  snapshot.copy_batches_completed = kIterationCount;
  snapshot.copy_rows_batched = kIterationCount;
  snapshot.plan_cache_hits = kIterationCount;
  snapshot.plan_cache_misses = 1;
  snapshot.plan_cache_invalidations = 1;
  snapshot.plan_cache_last_invalidation_reason = "catalog_epoch_changed";
  snapshot.descriptor_metadata_cache_hits = kIterationCount;
  snapshot.descriptor_metadata_cache_epoch = 7070;
  snapshot.statistics_epoch = 7071;
  snapshot.catalog_generation_id = 7070;
  snapshot.name_resolution_epoch = 7071;
  snapshot.security_epoch = 7072;
  snapshot.resource_epoch = 7073;
  snapshot.optimization_state_epoch = 7074;
  snapshot.selected_join_algorithm = "nested_loop";
  snapshot.selected_join_plan_summary = "dpc070_route_pressure_select";
  snapshot.selected_join_left_rows = kIterationCount;
  snapshot.selected_join_right_rows = kIterationCount;
  snapshot.selected_join_from_statistics = true;
  snapshot.selected_join_statistics_version = "statistics_epoch:7071";
  snapshot.summary_prune_status = "summary_prune";
  snapshot.summary_prune_last_reason = "bounded_soak_route_pressure";
  snapshot.summary_prune_summary_status = "fresh";
  snapshot.summary_prune_generation = 7075;
  snapshot.summary_prune_ranges_considered = kIterationCount;
  snapshot.summary_prune_ranges_pruned = kIterationCount / 2;
  snapshot.summary_prune_ranges_scanned =
      kIterationCount - snapshot.summary_prune_ranges_pruned;
  snapshot.summary_prune_pages_considered = kIterationCount * 4;
  snapshot.summary_prune_pages_pruned = kIterationCount * 2;
  snapshot.summary_prune_pages_scanned =
      snapshot.summary_prune_pages_considered - snapshot.summary_prune_pages_pruned;
  snapshot.cleanup_horizon_authority_status = "authoritative";
  snapshot.cleanup_horizon_authoritative = true;
  snapshot.cleanup_horizon_local_transaction_id = 1;
  snapshot.oldest_interesting_transaction_id = 1;
  snapshot.oldest_active_transaction_id = 1;
  snapshot.oldest_snapshot_transaction_id = 1;
  snapshot.oldest_cleanup_transaction_id = 1;
  snapshot.agent_worker_status = "drained";
  snapshot.agent_worker_thread_count = after.thread_count;
  snapshot.agent_worker_actions_accepted = kIterationCount;
  snapshot.agent_worker_actions_refused = 0;
  snapshot.agent_work_backlog_count = 0;
  snapshot.last_agent_type_id = "soak_resource_stability";
  snapshot.last_agent_action = "bounded_route_pressure";
  snapshot.last_agent_decision = "drained_no_growth";
  snapshot.last_agent_diagnostic_code = "DPC.SOAK.RESOURCE_STABLE";
  snapshot.storage_row_version_backlog_count = 0;
  snapshot.index_delta_backlog_count = 0;
  snapshot.index_garbage_backlog_count = 0;
  snapshot.page_summary_backlog_count = 0;
  snapshot.secondary_index_state = "online_delta_overlay";
  snapshot.shadow_index_state = "clean";
  snapshot.summary_index_state = "fresh_authoritative";
  snapshot.specialized_index_state = "ready";
  snapshot.index_state_authority_source = "engine_catalog_and_mga_inventory";
  snapshot.resource_governor_state = "bounded_growth";
  snapshot.resource_quota_grants = kIterationCount;
  snapshot.resource_quota_refusals = 0;
  snapshot.resource_quota_in_use = 0;
  snapshot.page_preallocation_demand_pages = after.database_tree_files;
  snapshot.page_preallocation_granted_pages = after.database_tree_files;
  snapshot.filespace_preallocation_demand_pages = after.database_tree_bytes / 16384;
  snapshot.filespace_preallocation_granted_pages = after.database_tree_bytes / 16384;
  snapshot.preallocation_refusal_count = 0;
  snapshot.backpressure_state = "none";
  snapshot.backpressure_reason = "none";
  snapshot.cancellation_checkpoint_count = kIterationCount;
  snapshot.backpressure_deferral_count = 0;
  snapshot.benchmark_correlation_id =
      "dpc070-diagnostic-soak-p95-ms=" + MillisText(foreground_p95_ms);
  snapshot.support_bundle_correlation_id = "dpc070-support-bundle";
  snapshot.request_correlation_id =
      "dpc070-route-pressure-p95-ms=" + MillisText(foreground_p95_ms);
  snapshot.exact_refusal_diagnostic_code = "DPC.SOAK.NON_AUTHORITATIVE_INPUT_REFUSED";
  snapshot.exact_refusal_message_vector =
      "DPC.SOAK.NON_AUTHORITATIVE_INPUT_REFUSED|"
      "MGA.TRANSACTION_INVENTORY_FINALITY|NO_EXECUTION_PLAN_RUNTIME_DEPENDENCY";
  snapshot.exact_refusal_source = "dpc070.soak_resource_stability";
  snapshot.message_vector_ready = true;
  snapshot.metric_sample_count = kIterationCount;
  snapshot.audit_event_count = kIterationCount;
  snapshot.audit_last_decision = "bounded_soak_completed";
  snapshot.support_bundle_redaction_state = "public_safe_summary";
  snapshot.support_bundle_completeness_state = "complete";
  snapshot.support_bundle_forbidden_fields_absent = true;
  (void)before;
  return snapshot;
}

api::EngineSupportBundleAgentEvidenceSource AgentEvidence(const Fixture& fixture) {
  api::EngineSupportBundleAgentEvidenceSource source;
  source.agent_type_id = "soak_resource_stability";
  source.agent_uuid = NewUuidText(UuidKind::object, 1782809100001);
  source.filespace_uuid = NewUuidText(UuidKind::filespace, 1782809100002);
  source.policy_uuid = NewUuidText(UuidKind::object, 1782809100003);
  source.evidence_uuid = NewUuidText(UuidKind::object, 1782809100004);
  source.evidence_kind = "dpc070_resource_stability";
  source.result_state = "success";
  source.diagnostic_code = "DPC.SOAK.RESOURCE_STABLE";
  source.payload_digest = "sha256:dpc070-resource-stability";
  source.physical_path = fixture.database_path.string();
  source.unsafe_payload = "password=cleartext token=secret-token";
  source.payload_redacted = true;
  return source;
}

void AssertManagementAndSupportEvidence(const Fixture& fixture,
                                        const ResourceSnapshot& before,
                                        const ResourceSnapshot& after,
                                        double foreground_p95_ms) {
  const auto snapshot = SoakSurfaceSnapshot(before, after, foreground_p95_ms);
  const auto validation = api::ValidatePerformanceOptimizationSurfaceSnapshot(snapshot);
  Require(validation.ok, "DPC-070 performance optimization surface snapshot failed validation");

  api::EngineInspectPerformanceOptimizationSurfaceRequest inspect;
  inspect.context = ObservabilityContext(fixture, 1);
  inspect.snapshot = snapshot;
  inspect.snapshot_present = true;
  const auto inspected = api::EngineInspectPerformanceOptimizationSurface(inspect);
  Require(inspected.ok, "DPC-070 management performance surface refused snapshot");
  Require(inspected.management_api_ready && inspected.support_bundle_ready,
          "DPC-070 management/support-bundle readiness flags missing");
  Require(HasEvidence(inspected, "user_observability_surface", "DPC-061"),
          "DPC-070 management surface missing aggregate observability evidence");
  Require(Contains(inspected.management_api_json, "\"resource_governor_state\":\"bounded_growth\""),
          "DPC-070 management JSON missing bounded-growth resource evidence");
  Require(Contains(inspected.support_bundle_json, "\"support_bundle\""),
          "DPC-070 support JSON missing support-bundle section");
  Require(!Contains(inspected.management_api_json, "docs" "/execution-plans") &&
              !Contains(inspected.support_bundle_json, "docs" "/execution-plans"),
          "DPC-070 observability surface depends on execution_plan artifacts at runtime");

  api::EnginePrepareSupportBundleRequest support;
  support.context = ObservabilityContext(fixture, 1);
  support.option_envelopes = {
      "engine_authorized_support_export:true",
      "retention_policy_ref:support.bundle.default_retention.v1",
      "redaction_profile_ref:server.support_bundle.default_redaction.v1",
  };
  support.performance_optimization_snapshot = snapshot;
  support.performance_optimization_snapshot_present = true;
  support.agent_runtime_evidence.push_back(AgentEvidence(fixture));
  const auto prepared = api::EnginePrepareSupportBundle(support);
  Require(prepared.ok, "DPC-070 support-bundle API refused resource snapshot");
  Require(prepared.redaction_applied && prepared.forbidden_fields_absent,
          "DPC-070 support-bundle redaction evidence missing");
  Require(prepared.performance_optimization_surface_collected &&
              prepared.agent_runtime_evidence_collected,
          "DPC-070 support-bundle did not collect performance and agent evidence");
  Require(HasEvidence(prepared, "support_bundle_agent_runtime_evidence", "redacted"),
          "DPC-070 support-bundle missing agent runtime evidence");
  Require(!Contains(prepared.support_bundle_json, fixture.root.string()) &&
              !Contains(prepared.support_bundle_json, "password=cleartext") &&
              !Contains(prepared.support_bundle_json, "secret-token") &&
              !Contains(prepared.support_bundle_json, "docs" "/execution-plans"),
          "DPC-070 support-bundle leaked unsafe or execution_plan runtime data");
}

void AssertResourceGrowthBounded(const ResourceSnapshot& before,
                                 const ResourceSnapshot& after,
                                 double foreground_p95_ms) {
  if (before.fd_count != 0 && after.fd_count != 0) {
    Require(after.fd_count <= before.fd_count + kFdGrowthAllowance,
            "DPC-070 file descriptor growth exceeded bounded allowance");
  }
  if (before.rss_kib != 0 && after.rss_kib != 0) {
    Require(after.rss_kib <= before.rss_kib + kResidentSetGrowthAllowanceKiB,
            "DPC-070 resident set growth exceeded bounded allowance");
  }
  if (before.thread_count != 0 && after.thread_count != 0) {
    Require(after.thread_count <= before.thread_count + kThreadGrowthAllowance,
            "DPC-070 thread growth exceeded bounded allowance");
  }
  Require(after.database_tree_bytes <=
              before.database_tree_bytes + kDatabaseTreeGrowthAllowanceBytes,
          "DPC-070 database tree byte growth exceeded bounded allowance");
  Require(after.database_tree_files <=
              before.database_tree_files + kDatabaseTreeFileGrowthAllowance,
          "DPC-070 database tree file growth exceeded bounded allowance");
  Require(foreground_p95_ms > 0.0 &&
              foreground_p95_ms <= kForegroundP95AllowanceMillis,
          "DPC-070 foreground p95 latency exceeded bounded allowance");
}

void AssertNoRuntimeOrphans(const Fixture& fixture) {
  const auto clean = db::MarkDatabaseCleanShutdown(fixture.database_path.string());
  Require(clean.ok(), "DPC-070 final clean shutdown marker failed");
  const auto reopened = db::OpenDatabaseFile({fixture.database_path.string(), false, true, true});
  Require(reopened.ok(), "DPC-070 clean read-only reopen failed");
  Require(reopened.state.startup_state_present,
          "DPC-070 reopen did not return startup state");
  Require(reopened.state.startup_state.durable_lifecycle_phase ==
              db::StartupLifecycleDurablePhase::clean_shutdown,
          "DPC-070 reopen did not classify clean-shutdown evidence");
  Require(reopened.state.local_transaction_inventory_present,
          "DPC-070 reopen did not expose transaction inventory authority");

  const std::filesystem::path control = fixture.root / "control";
  std::error_code ignored;
  if (std::filesystem::exists(control, ignored)) {
    for (const auto& entry : std::filesystem::directory_iterator(control, ignored)) {
      const auto name = entry.path().filename().string();
      Require(!Contains(name, ".sock") && name != "sb_server.pid",
              "DPC-070 left a server runtime socket or pid artifact");
    }
  }
}

}  // namespace

int main() {
  scratchbird::tests::database_lifecycle::ConfigureLifecycleMemoryFixture(
      "dpc_soak_leak_resource_stability_gate");
  auto fixture = CreateFixture();

  std::vector<double> foreground_latency_ms;
  auto run_measured = [&](std::uint64_t index) {
    const auto start = std::chrono::steady_clock::now();
    RunStressIteration(fixture, index);
    const auto stop = std::chrono::steady_clock::now();
    foreground_latency_ms.push_back(
        std::chrono::duration<double, std::milli>(stop - start).count());
  };

  run_measured(0);
  const auto before = CaptureResources(fixture);
  for (std::uint64_t index = 1; index <= kIterationCount; ++index) {
    run_measured(index);
  }
  const auto after = CaptureResources(fixture);
  const double foreground_p95_ms = P95Millis(foreground_latency_ms);

  AssertResourceGrowthBounded(before, after, foreground_p95_ms);
  AssertManagementAndSupportEvidence(fixture, before, after, foreground_p95_ms);
  AssertNoRuntimeOrphans(fixture);

  const auto fd_delta = after.fd_count >= before.fd_count ? after.fd_count - before.fd_count : 0;
  const auto rss_delta = after.rss_kib >= before.rss_kib ? after.rss_kib - before.rss_kib : 0;
  const auto thread_delta =
      after.thread_count >= before.thread_count ? after.thread_count - before.thread_count : 0;
  const auto disk_delta = after.database_tree_bytes >= before.database_tree_bytes
                              ? after.database_tree_bytes - before.database_tree_bytes
                              : 0;
  std::cout << "DPC_SOAK_LEAK_RESOURCE_STABILITY_GATE=passed iterations=" << kIterationCount
            << " fd_before=" << before.fd_count << " fd_after=" << after.fd_count
            << " fd_delta=" << fd_delta << " rss_kib_before=" << before.rss_kib
            << " rss_kib_after=" << after.rss_kib << " rss_kib_delta=" << rss_delta
            << " threads_before=" << before.thread_count
            << " threads_after=" << after.thread_count
            << " threads_delta=" << thread_delta
            << " database_tree_bytes_before=" << before.database_tree_bytes
            << " database_tree_bytes_after=" << after.database_tree_bytes
            << " database_tree_bytes_delta=" << disk_delta
            << " foreground_p95_ms=" << MillisText(foreground_p95_ms) << '\n';

  std::filesystem::remove_all(fixture.root);
  return EXIT_SUCCESS;
}
