// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "database_lifecycle.hpp"
#include "database_lifecycle_test_memory.hpp"
#include "sblr_dispatch_server.hpp"
#include "session_registry.hpp"
#include "transaction/transaction_api.hpp"
#include "uuid.hpp"

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
namespace uuid = scratchbird::core::uuid;
using scratchbird::core::platform::UuidKind;
using scratchbird::server::HostedDatabaseSnapshot;
using scratchbird::server::HostedDatabaseState;
using scratchbird::server::HostedEngineState;
using scratchbird::server::ServerSessionRegistry;
using scratchbird::server::SessionOperationResult;
namespace sbps = scratchbird::server::sbps;

constexpr std::string_view kVerifier =
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
constexpr std::string_view kAlicePrincipalUuid =
    "019e108d-1700-7000-8000-0000000008aa";

struct AuthFixture {
  std::array<std::uint8_t, 16> connection_uuid{};
  std::array<std::uint8_t, 16> auth_context_uuid{};
};

struct ExecuteDecoded {
  std::string outcome;
  std::uint64_t row_count = 0;
  std::string operation_id;
  std::string row_packet;
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

bool Contains(std::string_view haystack, std::string_view needle) {
  return haystack.find(needle) != std::string_view::npos;
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

std::filesystem::path MakeTempDir() {
  std::string tmpl = "/tmp/sb_dblc008_tx_admission.XXXXXX";
  std::vector<char> writable(tmpl.begin(), tmpl.end());
  writable.push_back('\0');
  char* made = ::mkdtemp(writable.data());
  Require(made != nullptr, "mkdtemp failed for DBLC-008 transaction admission test");
  return std::filesystem::path(made);
}

std::string NewUuid(UuidKind kind, std::uint64_t millis) {
  const auto generated = uuid::GenerateEngineIdentityV7(kind, millis);
  Require(generated.ok(), "UUID generation failed");
  return uuid::UuidToString(generated.value.value);
}

std::string CreateOpenDatabase(const std::filesystem::path& path) {
  db::DatabaseCreateConfig create;
  create.path = path.string();
  create.database_uuid = uuid::GenerateEngineIdentityV7(UuidKind::database, 1779100001000).value;
  create.filespace_uuid = uuid::GenerateEngineIdentityV7(UuidKind::filespace, 1779100001001).value;
  create.page_size = 16384;
  create.creation_unix_epoch_millis = 1779100001002;
  create.allow_minimal_resource_bootstrap = true;
  create.require_resource_seed_pack = false;
  create.allow_overwrite = true;
  const auto created = db::CreateDatabaseFile(create);
  if (!created.ok()) {
    std::cerr << created.diagnostic.diagnostic_code << ":"
              << created.diagnostic.message_key << '\n';
  }
  Require(created.ok(), "DBLC-008 database create failed");
  const auto opened = db::OpenDatabaseFile({path.string(), false, false, false});
  Require(opened.ok(), "DBLC-008 first open tx2 activation failed");
  const auto clean = db::MarkDatabaseCleanShutdown(path.string());
  Require(clean.ok(), "DBLC-008 clean shutdown marker failed");
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
      17,
      "DBLC-008");
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
                                        const std::array<std::uint8_t, 16>& auth_context_uuid,
                                        std::string_view mode = "read_write") {
  std::vector<std::uint8_t> out;
  PutUuid(&out, connection_uuid);
  PutUuid(&out, auth_context_uuid);
  PutString(&out, "default");
  PutString(&out, mode);
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

AuthFixture AttachAuthenticatedSession(ServerSessionRegistry* registry,
                                       const HostedEngineState& engine_state,
                                       std::string_view mode = "read_write") {
  AuthFixture fixture;
  fixture.connection_uuid = sbps::MakeUuidV7Bytes();
  auto auth_frame = Frame(sbps::MessageType::kAuthHandoff,
                          AuthPayload(fixture.connection_uuid),
                          fixture.connection_uuid);
  const auto auth = scratchbird::server::HandleAuthHandoff(registry, engine_state, auth_frame);
  Require(auth.accepted, "DBLC-008 auth handoff failed");
  const auto auth_context = scratchbird::server::DecodeAuthContextUuidForTest(auth.payload);
  Require(auth_context.has_value(), "DBLC-008 auth context decode failed");
  fixture.auth_context_uuid = *auth_context;

  auto attach_frame = Frame(sbps::MessageType::kAttachDatabase,
                            AttachPayload(fixture.connection_uuid, fixture.auth_context_uuid, mode),
                            fixture.connection_uuid);
  const auto attach = scratchbird::server::HandleAttachDatabase(registry, engine_state, attach_frame);
  if (!attach.accepted) {
    for (const auto& diagnostic : attach.diagnostics) {
      std::cerr << diagnostic.code << ": " << diagnostic.safe_message << '\n';
    }
  }
  Require(attach.accepted, "DBLC-008 attach failed");
  Require(registry->sessions_by_uuid.size() == 1, "DBLC-008 attach did not create one session");
  return fixture;
}

std::string TransactionEnvelope(std::string_view operation_id,
                                bool requires_transaction,
                                bool read_only = false) {
  std::string out;
  out += "operation_id=";
  out += operation_id;
  out += "\n";
  out += "sblr_operation_family=sblr.transaction.control.v3\n";
  out += "result_shape=engine.api.result.v1\n";
  out += "diagnostic_shape=engine.diagnostic.v1\n";
  out += "trace_key=DBLC-008\n";
  out += "contains_sql_text=false\n";
  out += "parser_resolved_names_to_uuids=true\n";
  out += "requires_security_context=true\n";
  out += requires_transaction ? "requires_transaction_context=true\n"
                              : "requires_transaction_context=false\n";
  out += "requires_cluster_authority=false\n";
  if (read_only) out += "read_only:true\n";
  return out;
}

SessionOperationResult Execute(ServerSessionRegistry* registry,
                               const HostedEngineState& engine_state,
                               const std::array<std::uint8_t, 16>& session_uuid,
                               std::string encoded) {
  auto frame = Frame(sbps::MessageType::kExecuteSblr,
                     scratchbird::server::EncodeExecuteSblrPayloadForTest(session_uuid, {}, encoded),
                     {},
                     session_uuid);
  return scratchbird::server::HandleExecuteSblr(registry, engine_state, frame);
}

ExecuteDecoded DecodeExecute(const SessionOperationResult& result) {
  ExecuteDecoded decoded;
  std::size_t offset = 0;
  Require(ReadString(result.payload, &offset, &decoded.outcome), "execute outcome decode failed");
  offset += 16;
  offset += 16;
  Require(offset + 8 <= result.payload.size(), "execute row_count decode failed");
  decoded.row_count = GetU64(result.payload, offset);
  offset += 8;
  if (!ReadString(result.payload, &offset, &decoded.operation_id)) {
    std::cerr << "execute operation decode failed size=" << result.payload.size()
              << " offset=" << offset << '\n';
    std::exit(EXIT_FAILURE);
  }
  if (!ReadString(result.payload, &offset, &decoded.row_packet)) {
    std::cerr << "execute row packet decode failed size=" << result.payload.size()
              << " offset=" << offset << '\n';
    std::exit(EXIT_FAILURE);
  }
  if (!ReadString(result.payload, &offset, &decoded.detail)) {
    std::cerr << "execute detail decode failed size=" << result.payload.size()
              << " offset=" << offset << '\n';
    std::exit(EXIT_FAILURE);
  }
  return decoded;
}

std::string DecodeRejectionDetail(const SessionOperationResult& result) {
  std::size_t offset = 0;
  std::string outcome;
  std::string operation;
  std::string detail;
  Require(ReadString(result.payload, &offset, &outcome), "rejection outcome decode failed");
  Require(offset + 16 <= result.payload.size(), "rejection uuid decode failed");
  offset += 16;
  Require(ReadString(result.payload, &offset, &operation), "rejection operation decode failed");
  Require(ReadString(result.payload, &offset, &detail), "rejection detail decode failed");
  return detail;
}

std::array<std::uint8_t, 16> ActiveSessionUuid(const ServerSessionRegistry& registry) {
  Require(registry.sessions_by_uuid.size() == 1, "active session registry size mismatch");
  return registry.sessions_by_uuid.begin()->second.session_uuid;
}

api::EngineRequestContext EngineContext(const std::filesystem::path& database_path,
                                        const std::string& database_uuid) {
  api::EngineRequestContext context;
  context.trust_mode = api::EngineTrustMode::server_isolated;
  context.request_id = "dblc-008-direct-engine";
  context.database_path = database_path.string();
  context.database_uuid.canonical = database_uuid;
  context.principal_uuid.canonical = "019e0f08-a100-7000-8000-000000000101";
  context.session_uuid.canonical = "019e0f08-a100-7000-8000-000000000102";
  context.security_context_present = true;
  context.catalog_generation_id = 1;
  context.security_epoch = 1;
  context.resource_epoch = 1;
  context.name_resolution_epoch = 1;
  return context;
}

void TestDirectEngineAdmission(const std::filesystem::path& database_path,
                               const std::string& database_uuid) {
  api::EngineBeginTransactionRequest missing_security;
  missing_security.context = EngineContext(database_path, database_uuid);
  missing_security.context.security_context_present = false;
  const auto denied = api::EngineBeginTransaction(missing_security);
  Require(!denied.ok, "engine admitted transaction without security context");
  Require(HasDiagnostic(denied, "ENGINE.DBLC_TRANSACTION_ADMISSION_DENIED"),
          "engine missing-security denial did not use DBLC transaction diagnostic");

  api::EngineBeginTransactionRequest cluster;
  cluster.context = EngineContext(database_path, database_uuid);
  cluster.option_envelopes.push_back("requires_cluster_authority:true");
  const auto cluster_denied = api::EngineBeginTransaction(cluster);
  Require(!cluster_denied.ok, "engine admitted standalone cluster transaction");
  Require(HasDiagnostic(cluster_denied, "ENGINE.DBLC_STANDALONE_CLUSTER_FAIL_CLOSED"),
          "engine cluster transaction did not fail closed");

  api::EngineBeginTransactionRequest fenced;
  fenced.context = EngineContext(database_path, database_uuid);
  fenced.option_envelopes.push_back("write_admission_fenced:true");
  const auto fenced_denied = api::EngineBeginTransaction(fenced);
  Require(!fenced_denied.ok, "engine admitted transaction through write fence");
  Require(HasDiagnostic(fenced_denied, "ENGINE.DBLC_TRANSACTION_ADMISSION_DENIED"),
          "engine write-fence denial did not use DBLC transaction diagnostic");
}

void TestServerTransactionLifecycle(const std::filesystem::path& database_path,
                                    const std::string& database_uuid) {
  ServerSessionRegistry registry;
  const auto engine_state = MakeEngineState(database_path, database_uuid);
  AttachAuthenticatedSession(&registry, engine_state);
  const auto session_uuid = ActiveSessionUuid(registry);
  const auto initial_local_id = registry.sessions_by_uuid.begin()->second.local_transaction_id;
  Require(initial_local_id != 0, "attach did not publish the required active transaction");

  auto begin = Execute(&registry,
                       engine_state,
                       session_uuid,
                       scratchbird::server::EncodeBeginTransactionSblrForTest());
  Require(begin.accepted, "server rejected valid transaction begin");
  const auto begin_decoded = DecodeExecute(begin);
  Require(begin_decoded.operation_id == "transaction.begin", "begin operation id mismatch");
  const auto local_id = TextU64(begin_decoded.row_packet, "local_transaction_id");
  Require(local_id.has_value() && *local_id != 0, "MGA begin did not return local transaction id");
  Require(*local_id == initial_local_id, "begin did not adopt the active attach transaction");
  Require(registry.sessions_by_uuid.begin()->second.local_transaction_id == *local_id,
          "server session did not bind admitted transaction id");
  Require(Contains(begin_decoded.row_packet, "evidence=always_active_session:true"),
          "transaction adoption evidence missing from server result");

  auto second_begin = Execute(&registry,
                              engine_state,
                              session_uuid,
                              scratchbird::server::EncodeBeginTransactionSblrForTest());
  Require(second_begin.accepted, "server rejected active transaction adoption for one session");
  Require(TextU64(DecodeExecute(second_begin).row_packet, "local_transaction_id") == local_id,
          "second begin did not adopt the current active transaction");

  auto commit = Execute(&registry,
                        engine_state,
                        session_uuid,
                        TransactionEnvelope("transaction.commit", true));
  Require(commit.accepted, "server rejected valid transaction commit");
  const auto replacement_id = registry.sessions_by_uuid.begin()->second.local_transaction_id;
  Require(replacement_id != 0, "server session did not publish replacement transaction after commit");
  Require(replacement_id != *local_id, "replacement transaction did not advance after commit");

  auto second_commit = Execute(&registry,
                               engine_state,
                               session_uuid,
                               TransactionEnvelope("transaction.commit", true));
  Require(second_commit.accepted, "server rejected commit of replacement transaction");
  Require(registry.sessions_by_uuid.begin()->second.local_transaction_id != replacement_id,
          "server did not replace the transaction after second commit");
}

void TestServerAdmissionFences(const std::filesystem::path& database_path,
                               const std::string& database_uuid) {
  {
    ServerSessionRegistry registry;
    const auto read_only_state = MakeEngineState(database_path, database_uuid, HostedDatabaseState::kReadOnly);
    AttachAuthenticatedSession(&registry, read_only_state, "read_only");
    const auto session_uuid = ActiveSessionUuid(registry);
    auto write_begin = Execute(&registry,
                               read_only_state,
                               session_uuid,
                               TransactionEnvelope("transaction.begin", false));
    Require(!write_begin.accepted, "server admitted write-capable transaction through read-only fence");
    Require(HasDiagnostic(write_begin, "ENGINE.DBLC_TRANSACTION_ADMISSION_DENIED"),
            "read-only fence did not use DBLC transaction diagnostic");
    auto read_begin = Execute(&registry,
                              read_only_state,
                              session_uuid,
                              TransactionEnvelope("transaction.begin", false, true));
    Require(read_begin.accepted, "server rejected read-only transaction on read-only attachment");
  }
  {
    ServerSessionRegistry registry;
    const auto open_state = MakeEngineState(database_path, database_uuid);
    AttachAuthenticatedSession(&registry, open_state);
    const auto session_uuid = ActiveSessionUuid(registry);
    const auto restricted = MakeEngineState(database_path, database_uuid, HostedDatabaseState::kRestrictedOpen);
    auto begin = Execute(&registry,
                         restricted,
                         session_uuid,
                         TransactionEnvelope("transaction.begin", false, true));
    Require(!begin.accepted, "server admitted ordinary transaction in restricted-open");
    Require(HasDiagnostic(begin, "ENGINE.DBLC_TRANSACTION_ADMISSION_DENIED"),
            "restricted-open transaction denial missing DBLC diagnostic");
  }
  {
    ServerSessionRegistry registry;
    const auto open_state = MakeEngineState(database_path, database_uuid);
    AttachAuthenticatedSession(&registry, open_state);
    const auto session_uuid = ActiveSessionUuid(registry);
    auto clustered = MakeEngineState(database_path, database_uuid);
    clustered.databases.front().cluster_structures_present = true;
    auto begin = Execute(&registry,
                         clustered,
                         session_uuid,
                         TransactionEnvelope("transaction.begin", false));
    Require(!begin.accepted, "server admitted standalone cluster transaction path");
    Require(HasDiagnostic(begin, "ENGINE.DBLC_STANDALONE_CLUSTER_FAIL_CLOSED"),
            "standalone cluster transaction did not fail closed");
  }
}

void TestServerResourceHooks(const std::filesystem::path& database_path,
                             const std::string& database_uuid) {
  const std::vector<std::pair<std::string, std::string>> hooks = {
      {"tx_admission:filespace_unavailable", "filespace_unavailable"},
      {"tx_admission:memory_denied", "memory_admission_denied"},
      {"tx_admission:lock_denied", "lock_admission_denied"},
      {"tx_admission:policy_stale", "authority_epoch_stale"},
  };
  for (const auto& [tag, expected_detail] : hooks) {
    ServerSessionRegistry registry;
    const auto engine_state = MakeEngineState(database_path, database_uuid);
    AttachAuthenticatedSession(&registry, engine_state);
    auto& session = registry.sessions_by_uuid.begin()->second;
    session.engine_authorization_trace_tags.push_back(tag);
    auto begin = Execute(&registry,
                         engine_state,
                         session.session_uuid,
                         TransactionEnvelope("transaction.begin", false));
    Require(!begin.accepted, std::string("server admitted transaction despite hook ") + tag);
    Require(HasDiagnostic(begin, "ENGINE.DBLC_TRANSACTION_ADMISSION_DENIED"),
            "resource hook denial missing DBLC transaction diagnostic");
    Require(Contains(DecodeRejectionDetail(begin), expected_detail), "resource hook detail mismatch");
  }
}

}  // namespace

int main() {
  scratchbird::tests::database_lifecycle::ConfigureLifecycleMemoryFixture(
      "database_lifecycle_transaction_admission_conformance");
  const auto temp_dir = MakeTempDir();
  const auto database_path = temp_dir / "dblc008_transaction_admission.sbdb";
  const std::string database_uuid = CreateOpenDatabase(database_path);
  WriteAuthStore(database_path, database_uuid);

  TestDirectEngineAdmission(database_path, database_uuid);
  TestServerTransactionLifecycle(database_path, database_uuid);
  TestServerAdmissionFences(database_path, database_uuid);
  TestServerResourceHooks(database_path, database_uuid);

  std::filesystem::remove_all(temp_dir);
  return EXIT_SUCCESS;
}
