// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "cluster/cluster_insert_route_api.hpp"
#include "cluster_transaction_fail_closed.hpp"
#include "database_dirty_manifest.hpp"
#include "database_format.hpp"
#include "database_lifecycle.hpp"
#include "disk_device.hpp"
#include "firebird_dialect.hpp"
#include "memory.hpp"
#include "runtime_platform.hpp"
#include "sblr_admission.hpp"
#include "sblr_dispatch_server.hpp"
#include "security/security_crypto_policy.hpp"
#include "security/security_principal_lifecycle.hpp"
#include "session_registry.hpp"
#include "startup_state.hpp"
#include "uuid.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

namespace {

namespace api = scratchbird::engine::internal_api;
namespace db = scratchbird::storage::database;
namespace disk = scratchbird::storage::disk;
namespace fb = scratchbird::parser::firebird;
namespace memory = scratchbird::core::memory;
namespace mga = scratchbird::transaction::mga;
namespace server = scratchbird::server;
namespace uuid = scratchbird::core::uuid;
namespace sbps = scratchbird::server::sbps;

using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;
using scratchbird::core::platform::TypedUuid;
using scratchbird::core::platform::UuidKind;
using scratchbird::core::platform::u64;

constexpr std::string_view kDatabaseUuid = "019e108d-1700-7000-8000-000000000017";
constexpr std::string_view kAlicePrincipalUuid = "019e108d-1700-7000-8000-0000000000aa";
constexpr std::string_view kVerifier =
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
constexpr std::string_view kWrongVerifier =
    "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";

struct Cleanup {
  std::vector<std::filesystem::path> database_paths;
  std::vector<std::filesystem::path> directories;

  ~Cleanup() {
    for (const auto& path : database_paths) {
      std::error_code ignored;
      std::filesystem::remove(path, ignored);
      std::filesystem::remove(path.string() + ".sb.owner.lock", ignored);
      std::filesystem::remove(path.string() + ".dirty.manifest", ignored);
      std::filesystem::remove(path.string() + ".recovery.evidence", ignored);
      std::filesystem::remove(path.string() + ".sb.local_password_auth", ignored);
      std::filesystem::remove(path.string() + ".sb.security_principal_events", ignored);
      std::filesystem::remove(path.string() + ".sb.txn_publish", ignored);
      std::filesystem::remove(path.string() + ".sb.txn_publish.tmp", ignored);
    }
    for (const auto& directory : directories) {
      std::error_code ignored;
      std::filesystem::remove_all(directory, ignored);
    }
  }
};

struct Fixture {
  std::filesystem::path path;
  TypedUuid database_uuid;
  TypedUuid filespace_uuid;
  std::uint32_t page_size = 0;
};

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

void PrintDiagnostic(const scratchbird::core::platform::DiagnosticRecord& diagnostic) {
  if (!diagnostic.diagnostic_code.empty()) {
    std::cerr << diagnostic.diagnostic_code << ':'
              << diagnostic.message_key << '\n';
  }
}

bool Contains(std::string_view haystack, std::string_view needle) {
  return haystack.find(needle) != std::string_view::npos;
}

u64 CurrentUnixMillis() {
  return static_cast<u64>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

u64 UniqueMillis() {
  static u64 counter = 0;
  return CurrentUnixMillis() + (++counter * 1000);
}

std::string UuidString(const TypedUuid& value) {
  return uuid::UuidToString(value.value);
}

std::filesystem::path TestDatabasePath(std::string_view label) {
  return std::filesystem::temp_directory_path() /
         ("sb_dblc017_" + std::string(label) + "_" + std::to_string(UniqueMillis()) + ".sbdb");
}

std::filesystem::path MakeTempDir(Cleanup* cleanup) {
  std::string tmpl = "/tmp/sb_dblc017_auth.XXXXXX";
  std::vector<char> writable(tmpl.begin(), tmpl.end());
  writable.push_back('\0');
  char* made = ::mkdtemp(writable.data());
  Require(made != nullptr, "mkdtemp failed for DBLC-017 auth test");
  cleanup->directories.emplace_back(made);
  return std::filesystem::path(made);
}

memory::AllocationPolicy MemoryPolicy() {
  memory::AllocationPolicy policy;
  policy.policy_name = "database_lifecycle_fault_injection_conformance";
  policy.hard_limit_bytes = 64ull * 1024ull * 1024ull;
  policy.soft_limit_bytes = 48ull * 1024ull * 1024ull;
  policy.per_context_limit_bytes = 32ull * 1024ull * 1024ull;
  policy.page_buffer_pool_limit_bytes = 16ull * 1024ull * 1024ull;
  policy.track_allocations = true;
  policy.zero_memory_on_release = true;
  return policy;
}

void ConfigureMemoryFixture() {
  const auto configured =
      memory::ConfigureDefaultMemoryManagerForFixture(
          MemoryPolicy(),
          "database_lifecycle_fault_injection_conformance");
  if (!configured.ok()) {
    PrintDiagnostic(configured.diagnostic);
  }
  Require(configured.ok(), "DBLC-017 memory fixture should configure");
  Require(configured.fixture_mode,
          "DBLC-017 memory fixture should run in fixture mode");
}

void RequireOk(const db::DatabaseLifecycleResult& result, std::string_view message) {
  if (!result.ok()) {
    std::cerr << result.diagnostic.diagnostic_code << ":"
              << result.diagnostic.message_key << '\n';
  }
  Require(result.ok(), message);
}

void RequireFailureCode(const db::DatabaseLifecycleResult& result,
                        std::string_view expected_code,
                        std::string_view message) {
  Require(!result.ok(), message);
  if (result.diagnostic.diagnostic_code != expected_code) {
    std::cerr << "expected=" << expected_code
              << " actual=" << result.diagnostic.diagnostic_code << '\n';
  }
  Require(result.diagnostic.diagnostic_code == expected_code, message);
}

Fixture CreateFixture(Cleanup* cleanup, std::string_view label) {
  Require(cleanup != nullptr, "cleanup registry is required");
  const auto now = UniqueMillis();
  const auto database_uuid = uuid::GenerateEngineIdentityV7(UuidKind::database, now);
  const auto filespace_uuid = uuid::GenerateEngineIdentityV7(UuidKind::filespace, now + 1);
  Require(database_uuid.ok() && filespace_uuid.ok(), "DBLC-017 UUID generation failed");

  const auto path = TestDatabasePath(label);
  cleanup->database_paths.push_back(path);

  db::DatabaseCreateConfig create;
  create.path = path.string();
  create.database_uuid = database_uuid.value;
  create.filespace_uuid = filespace_uuid.value;
  create.creation_unix_epoch_millis = now;
  create.resource_seed_pack_root = SB_BOOTSTRAP_SEED_PACK_ROOT;
  create.require_resource_seed_pack = true;
  create.allow_minimal_resource_bootstrap = false;
  create.allow_overwrite = true;

  const auto created = db::CreateDatabaseFile(create);
  RequireOk(created, "DBLC-017 CreateDatabaseFile failed");

  Fixture fixture;
  fixture.path = path;
  fixture.database_uuid = database_uuid.value;
  fixture.filespace_uuid = filespace_uuid.value;
  fixture.page_size = created.state.header.page_size;
  return fixture;
}

db::DatabaseLifecycleResult OpenFixture(const Fixture& fixture, bool read_only = false) {
  db::DatabaseOpenConfig open;
  open.path = fixture.path.string();
  open.read_only = read_only;
  return db::OpenDatabaseFile(open);
}

template <typename Mutator>
void MutateStartup(const Fixture& fixture, Mutator mutator) {
  disk::FileDevice device;
  const auto opened = device.Open(fixture.path.string(), disk::FileOpenMode::open_existing);
  Require(opened.ok(), "DBLC-017 startup mutation open failed");
  auto startup = db::ReadStartupStatePageBody(&device, fixture.page_size);
  Require(startup.ok(), "DBLC-017 startup mutation read failed");
  mutator(&startup.state);
  const auto written = db::WriteStartupStatePageBody(&device, startup.state);
  Require(written.ok(), "DBLC-017 startup mutation write failed");
  const auto synced = device.Sync();
  Require(synced.ok(), "DBLC-017 startup mutation sync failed");
}

std::string ReadTextFile(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  Require(static_cast<bool>(in), "DBLC-017 text artifact open failed");
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void WriteTextFile(const std::filesystem::path& path, const std::string& text) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  Require(static_cast<bool>(out), "DBLC-017 text artifact write open failed");
  out << text;
  out.flush();
  Require(static_cast<bool>(out), "DBLC-017 text artifact write failed");
}

void WriteRecoverableDirtyManifest(const Fixture& fixture) {
  const auto object_uuid = uuid::GenerateEngineIdentityV7(UuidKind::object, UniqueMillis());
  Require(object_uuid.ok(), "DBLC-017 dirty manifest object UUID generation failed");

  db::DirtyObjectManifest manifest;
  manifest.checkpoint_generation = 1;
  manifest.completed = true;
  manifest.classification_only = true;

  db::DirtyObjectManifestEntry entry;
  entry.kind = db::DirtyObjectKind::catalog_page;
  entry.object_uuid = object_uuid.value;
  entry.page_number = db::kCatalogPageNumber;
  entry.page_generation = 1;
  entry.object_checksum = 177;
  entry.local_transaction_id = 2;
  entry.operation_envelope_checksum = 277;
  entry.transaction_evidence_checksum = 377;
  entry.dirty = true;
  entry.authoritative = true;
  manifest.entries.push_back(entry);

  const auto built = db::BuildDirtyObjectManifest(manifest);
  Require(built.ok(), "DBLC-017 recoverable dirty manifest build failed");
  WriteTextFile(fixture.path.string() + ".dirty.manifest", built.serialized);
}

void TestPartialCreateTx1EvidenceFailsClosed(Cleanup* cleanup) {
  const auto fixture = CreateFixture(cleanup, "partial_tx1");
  MutateStartup(fixture, [](db::StartupStateRecord* startup) {
    startup->bootstrap_local_transaction_id = 0;
    startup->durable_evidence_flags &= ~db::StartupLifecycleEvidenceFlag::bootstrap_tx1_committed;
  });

  const auto failed = OpenFixture(fixture);
  RequireFailureCode(failed,
                     "SB-DB-LIFECYCLE-BOOTSTRAP-EVIDENCE-MISSING",
                     "DBLC-017 partial create tx1 evidence was admitted");
}

void TestInterruptedTx2EvidenceFailsClosed(Cleanup* cleanup) {
  const auto fixture = CreateFixture(cleanup, "interrupted_tx2");
  RequireOk(OpenFixture(fixture), "DBLC-017 first open for interrupted tx2 failed");
  MutateStartup(fixture, [](db::StartupStateRecord* startup) {
    startup->durable_evidence_flags &= ~db::StartupLifecycleEvidenceFlag::first_open_tx2_committed;
  });

  const auto failed = OpenFixture(fixture);
  RequireFailureCode(failed,
                     "SB-DB-LIFECYCLE-ACTIVATION-EVIDENCE-MISSING",
                     "DBLC-017 interrupted tx2 evidence was admitted");
}

void TestUncleanShutdownRecoveryUsesMGAEvidence(Cleanup* cleanup) {
  const auto fixture = CreateFixture(cleanup, "unclean_recovery");
  RequireOk(OpenFixture(fixture), "DBLC-017 first open for recovery failed");
  WriteRecoverableDirtyManifest(fixture);

  const auto recovered = OpenFixture(fixture);
  RequireOk(recovered, "DBLC-017 unclean startup did not recover through MGA evidence");
  Require(recovered.state.startup_recovery_classification == "repaired_recovery",
          "DBLC-017 unclean startup did not classify repaired_recovery");
  bool stale_owner_classified = false;
  for (const auto& phase : recovered.state.startup_state.completed_phases) {
    if (phase == "open.stale_owner_evidence_classified") {
      stale_owner_classified = true;
    }
  }
  Require(stale_owner_classified, "DBLC-017 stale owner evidence was not classified");

  const auto evidence = ReadTextFile(fixture.path.string() + ".recovery.evidence");
  Require(Contains(evidence, "SBRECOVERY1"), "DBLC-017 recovery evidence marker missing");
  Require(!Contains(evidence, "WAL") && !Contains(evidence, "wal") &&
              !Contains(evidence, "write-ahead"),
          "DBLC-017 recovery evidence used non-MGA write-ahead terminology");
}

void TestIdentityMismatchFailsClosed(Cleanup* cleanup) {
  const auto fixture = CreateFixture(cleanup, "identity_mismatch");
  const auto wrong_filespace_uuid = uuid::GenerateEngineIdentityV7(UuidKind::filespace, UniqueMillis());
  Require(wrong_filespace_uuid.ok(), "DBLC-017 wrong filespace UUID generation failed");
  MutateStartup(fixture, [&](db::StartupStateRecord* startup) {
    startup->first_filespace_uuid = wrong_filespace_uuid.value;
  });

  const auto failed = OpenFixture(fixture);
  RequireFailureCode(failed,
                     "SB-DB-LIFECYCLE-STARTUP-PAGE-FILESPACE-UUID-MISMATCH",
                     "DBLC-017 stale identity mismatch was admitted");
}

void PutU16(std::vector<std::uint8_t>* out, std::uint16_t value) {
  out->push_back(static_cast<std::uint8_t>(value & 0xffu));
  out->push_back(static_cast<std::uint8_t>((value >> 8u) & 0xffu));
}

void PutUuid(std::vector<std::uint8_t>* out, const std::array<std::uint8_t, 16>& value) {
  out->insert(out->end(), value.begin(), value.end());
}

void PutString(std::vector<std::uint8_t>* out, std::string_view value) {
  PutU16(out, static_cast<std::uint16_t>(value.size()));
  out->insert(out->end(), value.begin(), value.end());
}

std::string AuthEvidence(std::string_view principal, std::string_view verifier) {
  std::string evidence = "scheme=local_password_v1;principal=";
  evidence += principal;
  evidence += ";principal_uuid=";
  evidence += kAlicePrincipalUuid;
  evidence += ";storage_authority=durable_security_catalog;authorization_tags=right:CONNECT;verifier=";
  evidence += verifier;
  return evidence;
}

std::string LocalPasswordFingerprint(std::string_view verifier) {
  return "local-password-verifier:v1:sha256:" + api::SecuritySha256Hex(verifier);
}

std::vector<std::uint8_t> AuthPayloadWithEvidence(
    const std::array<std::uint8_t, 16>& connection_uuid,
    std::string_view principal,
    std::string_view evidence,
    bool credential_invalid) {
  std::vector<std::uint8_t> out;
  PutUuid(&out, connection_uuid);
  out.push_back(1);
  out.push_back(credential_invalid ? 1 : 0);
  out.push_back(0);
  out.push_back(0);
  PutString(&out, "local_password");
  PutString(&out, principal);
  PutString(&out, "default");
  PutString(&out, "en");
  PutString(&out, evidence);
  return out;
}

std::vector<std::uint8_t> AuthPayload(const std::array<std::uint8_t, 16>& connection_uuid,
                                      std::string_view principal,
                                      std::string_view verifier,
                                      bool credential_invalid) {
  return AuthPayloadWithEvidence(connection_uuid,
                                 principal,
                                 AuthEvidence(principal, verifier),
                                 credential_invalid);
}

std::vector<std::uint8_t> AttachPayload(
    const std::array<std::uint8_t, 16>& connection_uuid,
    const std::array<std::uint8_t, 16>& auth_context_uuid) {
  std::vector<std::uint8_t> out;
  PutUuid(&out, connection_uuid);
  PutUuid(&out, auth_context_uuid);
  PutString(&out, "default");
  PutString(&out, "read_write");
  return out;
}

sbps::Frame MakeFrame(sbps::MessageType type,
                      std::vector<std::uint8_t> payload,
                      const std::array<std::uint8_t, 16>& connection_uuid) {
  sbps::Frame frame;
  frame.header.message_type = static_cast<std::uint16_t>(type);
  frame.header.request_uuid = sbps::MakeUuidV7Bytes();
  frame.header.connection_uuid = connection_uuid;
  frame.header.payload_schema_id = type == sbps::MessageType::kAuthHandoff ? 3001 : 0;
  frame.payload = std::move(payload);
  return frame;
}

bool HasDiagnostic(const server::SessionOperationResult& result, std::string_view code) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.code == code) return true;
  }
  return false;
}

void PrintSessionDiagnostics(const server::SessionOperationResult& result) {
  for (const auto& diagnostic : result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message_key;
    for (const auto& field : diagnostic.fields) {
      std::cerr << ' ' << field.key << '=' << field.value;
    }
    std::cerr << '\n';
  }
}

api::EngineUuid EngineUuidText(std::string_view uuid_text) {
  api::EngineUuid uuid_value;
  uuid_value.canonical = std::string(uuid_text);
  return uuid_value;
}

server::HostedEngineState MakeEngineState(const std::filesystem::path& database_path) {
  server::HostedEngineState engine_state;
  engine_state.engine_context_active = true;
  server::HostedDatabaseSnapshot database;
  database.state = server::HostedDatabaseState::kOpen;
  database.database_open = true;
  database.database_path = database_path.string();
  database.database_uuid = std::string(kDatabaseUuid);
  database.read_only = false;
  database.write_admission_fenced = false;
  engine_state.databases.push_back(std::move(database));
  return engine_state;
}

api::EngineRequestContext SecurityAdminContext(const std::filesystem::path& database_path,
                                               std::uint64_t local_tx) {
  api::EngineRequestContext context;
  context.trust_mode = api::EngineTrustMode::server_isolated;
  context.database_path = database_path.string();
  context.database_uuid = EngineUuidText(kDatabaseUuid);
  context.session_uuid = EngineUuidText("019e108d-1700-7000-8000-000000000011");
  context.transaction_uuid = EngineUuidText("019e108d-1700-7000-8000-000000000012");
  context.security_context_present = true;
  context.trace_tags.push_back("security.bootstrap");
  context.local_transaction_id = local_tx;
  context.snapshot_visible_through_local_transaction_id = local_tx;
  context.catalog_generation_id = 7;
  context.security_epoch = 7;
  return context;
}

void WriteAuthStore(const std::filesystem::path& database_path) {
  api::EngineSecurityCreatePrincipalRequest request;
  request.context = SecurityAdminContext(database_path, 17);
  request.target_object.uuid = EngineUuidText(kAlicePrincipalUuid);
  request.target_object.object_kind = "security_principal";
  request.principal_uuid = std::string(kAlicePrincipalUuid);
  request.principal_name = "alice";
  request.credential_fingerprint = LocalPasswordFingerprint(kVerifier);
  const auto created = api::EngineSecurityCreatePrincipal(request);
  if (!created.ok) {
    for (const auto& diagnostic : created.diagnostics) {
      std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
    }
  }
  Require(created.ok && created.principal_created,
          "DBLC-017 durable auth store write failed");
}

void TestEngineAuthDenialFailsClosed(Cleanup* cleanup) {
  const auto dir = MakeTempDir(cleanup);
  const auto database_path = dir / "auth.sbdb";
  cleanup->database_paths.push_back(database_path);
  WriteAuthStore(database_path);

  server::ServerSessionRegistry registry;
  const auto connection_uuid = sbps::MakeUuidV7Bytes();
  const auto frame = MakeFrame(sbps::MessageType::kAuthHandoff,
                               AuthPayload(connection_uuid, "alice", kWrongVerifier, true),
                               connection_uuid);
  const auto result = server::HandleAuthHandoff(&registry, MakeEngineState(database_path), frame);
  Require(!result.accepted, "DBLC-017 invalid engine credentials were accepted");
  Require(HasDiagnostic(result, "SECURITY.AUTHENTICATION.FAILED"),
          "DBLC-017 invalid auth did not return engine security diagnostic");
  Require(registry.auth_contexts_by_uuid.empty() && registry.sessions_by_uuid.empty(),
          "DBLC-017 rejected auth created session or auth context state");
}

void TestAuthReplayAndTlsDowngradeFailClosed(Cleanup* cleanup) {
  const auto fixture = CreateFixture(cleanup, "auth_replay_tls");
  WriteAuthStore(fixture.path);
  const auto engine_state = MakeEngineState(fixture.path);

  server::ServerSessionRegistry registry;
  const auto connection_uuid = sbps::MakeUuidV7Bytes();
  const auto auth = server::HandleAuthHandoff(
      &registry,
      engine_state,
      MakeFrame(sbps::MessageType::kAuthHandoff,
                AuthPayload(connection_uuid, "alice", kVerifier, false),
                connection_uuid));
  if (!auth.accepted) {
    PrintSessionDiagnostics(auth);
  }
  Require(auth.accepted, "DBLC-017 valid auth for replay test failed");
  const auto auth_context = server::DecodeAuthContextUuidForTest(auth.payload);
  Require(auth_context.has_value(), "DBLC-017 auth context decode failed");

  const auto attach = server::HandleAttachDatabase(
      &registry,
      engine_state,
      MakeFrame(sbps::MessageType::kAttachDatabase,
                AttachPayload(connection_uuid, *auth_context),
                connection_uuid));
  if (!attach.accepted) {
    PrintSessionDiagnostics(attach);
  }
  Require(attach.accepted, "DBLC-017 first attach for replay test failed");
  const auto replay = server::HandleAttachDatabase(
      &registry,
      engine_state,
      MakeFrame(sbps::MessageType::kAttachDatabase,
                AttachPayload(connection_uuid, *auth_context),
                connection_uuid));
  Require(!replay.accepted &&
              HasDiagnostic(replay, "PARSER_SERVER_IPC.AUTH_CONTEXT_REPLAY_REFUSED"),
          "DBLC-017 auth context replay was not refused");

  server::ServerSessionRegistry tls_registry;
  const auto downgrade_connection = sbps::MakeUuidV7Bytes();
  const std::string downgrade_evidence =
      AuthEvidence("alice", kVerifier) +
      ";tls_required=true;tls_negotiated=cleartext;tls_downgrade=true";
  const auto tls = server::HandleAuthHandoff(
      &tls_registry,
      engine_state,
      MakeFrame(sbps::MessageType::kAuthHandoff,
                AuthPayloadWithEvidence(downgrade_connection,
                                        "alice",
                                        downgrade_evidence,
                                        false),
                downgrade_connection));
  Require(!tls.accepted &&
              HasDiagnostic(tls, "SECURITY.AUTHENTICATION.TLS_DOWNGRADE_REFUSED"),
          "DBLC-017 TLS downgrade evidence was not refused");
  Require(tls_registry.auth_contexts_by_uuid.empty() &&
              tls_registry.sessions_by_uuid.empty(),
          "DBLC-017 TLS downgrade created session or auth context state");
}

bool HasAdmissionDiagnostic(const server::ServerSblrAdmissionResult& result,
                            std::string_view code) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.code == code) return true;
  }
  return false;
}

void TestParserBypassAndTransactionFinalityAreRejectedOrEngineRouted() {
  const auto raw_sql =
      server::AdmitServerSblrEnvelope(server::ServerSblrAdmissionRequest{"CREATE DATABASE x", false});
  Require(!raw_sql.admitted && HasAdmissionDiagnostic(raw_sql, "SBLR.SQL_TEXT_FORBIDDEN"),
          "DBLC-017 raw SQL bypass was admitted to the engine boundary");

  const auto embedded_sql = server::AdmitServerSblrEnvelope(server::ServerSblrAdmissionRequest{
      "operation_id=dml.select\n"
      "sblr_operation_family=sblr.query.relational.v3\n"
      "result_shape=rowset\n"
      "diagnostic_shape=diagnostic_vector\n"
      "parser_resolved_names_to_uuids=true\n"
      "contains_sql_text=true\n",
      false});
  Require(!embedded_sql.admitted && HasAdmissionDiagnostic(embedded_sql, "SBLR.SQL_TEXT_FORBIDDEN"),
          "DBLC-017 parser envelope containing SQL text was admitted");

  const auto transaction = server::AdmitServerSblrEnvelope(server::ServerSblrAdmissionRequest{
      "operation_id=transaction.commit\n"
      "sblr_operation_family=sblr.transaction.control.v3\n"
      "result_shape=engine.api.result.v1\n"
      "diagnostic_shape=engine.diagnostic.v1\n"
      "parser_resolved_names_to_uuids=true\n"
      "requires_transaction_context=true\n",
      false});
  Require(transaction.admitted, "DBLC-017 canonical transaction SBLR was not admitted");
  Require(transaction.requires_public_abi_dispatch,
          "DBLC-017 transaction SBLR did not require engine/public-ABI dispatch");
}

void TestReferenceBypassIsDiagnosticOnly() {
  const auto backup = fb::ParseStatement("BACKUP DATABASE 'safe.fdb' TO 'safe.fbk'");
  Require(backup.ok, "DBLC-017 Firebird backup parse failed");
  Require(backup.exact_emulated_diagnostic,
          "DBLC-017 Firebird backup was not represented as exact diagnostic");
  Require(!backup.real_firebird_file_effects && !backup.reference_engine_sql_executed,
          "DBLC-017 Firebird backup admitted reference file or SQL authority");
  Require(Contains(backup.message_vector_json, "FIREBIRD.EMULATION.NON_FILE_SURFACE"),
          "DBLC-017 Firebird backup diagnostic code missing");
  Require(!Contains(backup.sblr_envelope, "BACKUP DATABASE"),
          "DBLC-017 Firebird diagnostic leaked reference SQL text");

  const auto create_database = fb::ParseStatement("CREATE DATABASE 'safe.fdb'");
  Require(create_database.ok && create_database.scratchbird_lifecycle_api,
          "DBLC-017 Firebird create did not map to ScratchBird lifecycle API");
  Require(!create_database.real_firebird_file_effects &&
              !create_database.reference_engine_sql_executed,
          "DBLC-017 Firebird create admitted reference side effects");
  Require(Contains(create_database.sblr_envelope, "\"sql_text_included\":false"),
          "DBLC-017 Firebird lifecycle envelope did not exclude reference SQL text");
}

api::EngineUuid TestEngineUuid(std::string_view suffix) {
  api::EngineUuid uuid_value;
  uuid_value.canonical = "019e108d-1700-7000-8000-" + std::string(suffix);
  return uuid_value;
}

api::EngineRequestContext StandaloneContext() {
  api::EngineRequestContext context;
  context.trust_mode = api::EngineTrustMode::server_isolated;
  context.database_uuid = TestEngineUuid("000000000001");
  context.transaction_uuid = TestEngineUuid("000000000002");
  context.security_context_present = true;
  context.cluster_authority_available = false;
  return context;
}

bool HasDiagnostic(const api::EngineApiResult& result, std::string_view code) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.code == code || diagnostic.detail == code) return true;
    if (diagnostic.detail.size() > code.size() &&
        diagnostic.detail.compare(diagnostic.detail.size() - code.size(), code.size(), code) == 0) {
      return true;
    }
  }
  return false;
}

TypedUuid TestTypedUuid(UuidKind kind) {
  TypedUuid value;
  value.kind = kind;
  value.value.bytes[0] = 0x17;
  value.value.bytes[6] = 0x70;
  value.value.bytes[8] = 0x80;
  return value;
}

void TestClusterPathFailsClosed() {
  api::EngineClusterInsertRouteFenceRequest route;
  route.context = StandaloneContext();
  const auto route_result = api::EngineValidateClusterInsertRouteFence(route);
  Require(!route_result.ok && route_result.cluster_authority_required,
          "DBLC-017 standalone cluster route was admitted");
  Require(route_result.refusal_reason == "cluster_authority_unavailable",
          "DBLC-017 cluster route did not fail before route details");
  Require(HasDiagnostic(route_result, "cluster_authority_unavailable"),
          "DBLC-017 cluster route diagnostic missing");

  mga::ClusterTransactionMetadata metadata;
  metadata.transaction_uuid = TestTypedUuid(UuidKind::transaction);
  metadata.cluster_authority_active = false;
  const auto begin = mga::BeginClusterTransactionFailClosed(metadata);
  Require(!begin.ok(), "DBLC-017 cluster transaction begin succeeded standalone");
  Require(begin.status.code == StatusCode::platform_required_feature_missing &&
              begin.status.subsystem == Subsystem::cluster_private,
          "DBLC-017 cluster transaction used wrong fail-closed status");
}

}  // namespace

int main() {
  ConfigureMemoryFixture();

  Cleanup cleanup;
  TestPartialCreateTx1EvidenceFailsClosed(&cleanup);
  TestInterruptedTx2EvidenceFailsClosed(&cleanup);
  TestUncleanShutdownRecoveryUsesMGAEvidence(&cleanup);
  TestIdentityMismatchFailsClosed(&cleanup);
  TestEngineAuthDenialFailsClosed(&cleanup);
  TestAuthReplayAndTlsDowngradeFailClosed(&cleanup);
  TestParserBypassAndTransactionFinalityAreRejectedOrEngineRouted();
  TestReferenceBypassIsDiagnosticOnly();
  TestClusterPathFailsClosed();
  std::cout << "DBLC_P17_HARDENED=passed\n";
  std::cout << "database_lifecycle_fault_injection=passed\n";
  return EXIT_SUCCESS;
}
