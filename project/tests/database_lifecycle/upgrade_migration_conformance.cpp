// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "config.hpp"
#include "database_format.hpp"
#include "database_lifecycle.hpp"
#include "disk_device.hpp"
#include "memory.hpp"
#include "server_ipc_lifecycle.hpp"
#include "startup_state.hpp"
#include "uuid.hpp"

#include <chrono>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

namespace {

namespace db = scratchbird::storage::database;
namespace disk = scratchbird::storage::disk;
namespace memory = scratchbird::core::memory;
namespace server = scratchbird::server;
namespace uuid = scratchbird::core::uuid;
using scratchbird::core::platform::TypedUuid;
using scratchbird::core::platform::UuidKind;
using scratchbird::core::platform::HostToLittle32;
using scratchbird::core::platform::HostToLittle64;
using scratchbird::core::platform::u32;
using scratchbird::core::platform::u64;

constexpr u32 kSerializedHeaderFormatMajorOffset = 8;
constexpr u32 kSerializedHeaderFormatMinorOffset = 12;
constexpr u32 kSerializedHeaderCompatibilityFlagsOffset = 64;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

u64 CurrentUnixMillis() {
  return static_cast<u64>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

void ConfigureMemoryFixture() {
  memory::AllocationPolicy policy;
  policy.policy_name = "database_lifecycle_upgrade_migration_conformance";
  policy.hard_limit_bytes = 64ull * 1024ull * 1024ull;
  policy.soft_limit_bytes = 48ull * 1024ull * 1024ull;
  policy.per_context_limit_bytes = 32ull * 1024ull * 1024ull;
  policy.page_buffer_pool_limit_bytes = 16ull * 1024ull * 1024ull;
  policy.track_allocations = true;
  policy.zero_memory_on_release = true;
  const auto configured = memory::ConfigureDefaultMemoryManagerForFixture(
      policy, "database_lifecycle_upgrade_migration_conformance");
  Require(configured.ok(), "memory fixture configuration failed");
  Require(configured.fixture_mode, "memory manager did not use fixture mode");
}

u64 UniqueMillis() {
  static u64 counter = 0;
  return CurrentUnixMillis() + (++counter * 1000);
}

std::filesystem::path MakeTempDir() {
  std::string tmpl = "/tmp/sb_dblc013s_migration.XXXXXX";
  std::vector<char> writable(tmpl.begin(), tmpl.end());
  writable.push_back('\0');
  char* made = ::mkdtemp(writable.data());
  Require(made != nullptr, "mkdtemp failed for DBLC-013S migration test");
  return std::filesystem::path(made);
}

void WriteTextFile(const std::filesystem::path& path, const std::string& text) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  Require(static_cast<bool>(out), "could not open test text file");
  out << text;
  out.flush();
  Require(static_cast<bool>(out), "could not write test text file");
}

bool HasDiagnostic(const std::vector<server::ServerDiagnostic>& diagnostics,
                   std::string_view code) {
  for (const auto& diagnostic : diagnostics) {
    if (diagnostic.code == code) return true;
  }
  return false;
}

void RequireCompatibilityCode(const db::DatabaseArtifactCompatibilityResult& result,
                              db::DatabaseOpenCompatibilityClass expected_class,
                              std::string_view expected_code,
                              std::string_view message) {
  Require(!result.ok(), message);
  Require(result.compatibility_class == expected_class, message);
  if (result.diagnostic.diagnostic_code != expected_code) {
    std::cerr << "expected=" << expected_code
              << " actual=" << result.diagnostic.diagnostic_code << '\n';
  }
  Require(result.diagnostic.diagnostic_code == expected_code, message);
}

db::DatabaseArtifactVersionCompatibilityRequest VersionRequest(
    std::string artifact_kind,
    u32 major,
    u32 minor,
    u32 min_major,
    u32 min_minor,
    u32 current_major,
    u32 current_minor,
    u32 max_major,
    u32 max_minor) {
  db::DatabaseArtifactVersionCompatibilityRequest request;
  request.artifact_kind = std::move(artifact_kind);
  request.format_major = major;
  request.format_minor = minor;
  request.min_supported_major = min_major;
  request.min_supported_minor = min_minor;
  request.current_major = current_major;
  request.current_minor = current_minor;
  request.max_supported_major = max_major;
  request.max_supported_minor = max_minor;
  return request;
}

void TestDatabaseHeaderVersionClassification() {
  const auto database_uuid = uuid::GenerateEngineIdentityV7(UuidKind::database, UniqueMillis());
  Require(database_uuid.ok(), "database UUID generation failed");
  const auto made_header =
      disk::MakeDatabaseHeader(database_uuid.value.value, 16384, UniqueMillis(), 0, 1);
  Require(made_header.ok(), "current database header build failed");
  Require(db::ClassifyDatabaseOpenCompatibility(made_header.header, false) ==
              db::DatabaseOpenCompatibilityClass::current,
          "current database header was not classified as current");

  auto unsupported_old = VersionRequest("database_header", 0, 0, 1, 0, 1, 0, 1, 0);
  RequireCompatibilityCode(db::ClassifyDatabaseArtifactVersionCompatibility(unsupported_old),
                           db::DatabaseOpenCompatibilityClass::unsupported_old,
                           "ENGINE.DBLC_MIGRATION_UNSUPPORTED_OLD_ARTIFACT",
                           "unsupported old database header was not refused");

  auto newer = VersionRequest("database_header", 2, 0, 1, 0, 1, 0, 1, 0);
  RequireCompatibilityCode(db::ClassifyDatabaseArtifactVersionCompatibility(newer),
                           db::DatabaseOpenCompatibilityClass::newer_than_supported_refused,
                           "ENGINE.DBLC_FORMAT_NEWER_THAN_SUPPORTED",
                           "newer-than-supported database header was not refused");

  auto unsupported_new = VersionRequest("database_header", 1, 1, 1, 0, 1, 0, 1, 1);
  RequireCompatibilityCode(db::ClassifyDatabaseArtifactVersionCompatibility(unsupported_new),
                           db::DatabaseOpenCompatibilityClass::unsupported_new,
                           "ENGINE.DBLC_MIGRATION_UNSUPPORTED_NEW_ARTIFACT",
                           "unsupported new database header was not refused");

  auto migration_without_plan = VersionRequest("database_header", 1, 0, 1, 0, 1, 1, 1, 1);
  RequireCompatibilityCode(db::ClassifyDatabaseArtifactVersionCompatibility(migration_without_plan),
                           db::DatabaseOpenCompatibilityClass::migration_required_without_plan_refused,
                           "ENGINE.DBLC_MIGRATION_REQUIRED_WITHOUT_PLAN",
                           "migration-required-without-plan database header was not refused");

  auto missing_plan = VersionRequest("database_header", 1, 0, 1, 0, 1, 0, 1, 0);
  missing_plan.migration_plan_required = true;
  RequireCompatibilityCode(db::ClassifyDatabaseArtifactVersionCompatibility(missing_plan),
                           db::DatabaseOpenCompatibilityClass::missing_migration_plan_refused,
                           "ENGINE.DBLC_MIGRATION_PLAN_MISSING",
                           "missing migration plan was not refused");

  auto ambiguous = VersionRequest("database_header", 1, 0, 1, 0, 1, 0, 1, 0);
  ambiguous.identity_proven = false;
  RequireCompatibilityCode(db::ClassifyDatabaseArtifactVersionCompatibility(ambiguous),
                           db::DatabaseOpenCompatibilityClass::ambiguous_identity_refused,
                           "ENGINE.DBLC_MIGRATION_AMBIGUOUS_IDENTITY_REFUSED",
                           "ambiguous identity was not refused");

  auto downgrade = VersionRequest("database_header", 1, 0, 1, 0, 1, 0, 1, 0);
  downgrade.downgrade_requested = true;
  RequireCompatibilityCode(db::ClassifyDatabaseArtifactVersionCompatibility(downgrade),
                           db::DatabaseOpenCompatibilityClass::downgrade_refused,
                           "ENGINE.DBLC_FORMAT_DOWNGRADE_REFUSED",
                           "unsafe downgrade was not refused");
}

void TestStartupStateFormatClassification() {
  const auto current = db::ClassifyStartupStateFormatCompatibility(
      db::kStartupStateFormatMajorCurrent,
      db::kStartupStateFormatMinorCurrent);
  Require(current.ok() &&
              current.compatibility_class ==
                  db::StartupStateFormatCompatibilityClass::supported_current,
          "current startup state format was not accepted");

  const auto old = db::ClassifyStartupStateFormatCompatibility(0, 0);
  Require(!old.ok() &&
              old.compatibility_class ==
                  db::StartupStateFormatCompatibilityClass::unsupported_old &&
              old.diagnostic.diagnostic_code == "SB-STARTUP-STATE-FORMAT-TOO-OLD",
          "unsupported old startup state format was not refused");

  const auto future = db::ClassifyStartupStateFormatCompatibility(
      db::kStartupStateFormatMajorMaxSupported + 1,
      0);
  Require(!future.ok() &&
              future.compatibility_class ==
                  db::StartupStateFormatCompatibilityClass::newer_than_supported_refused &&
              future.diagnostic.diagnostic_code == "SB-STARTUP-STATE-FORMAT-FUTURE",
          "future startup state format was not refused");

  const auto missing_plan = db::ClassifyStartupStateFormatCompatibility(
      db::kStartupStateFormatMajorCurrent,
      db::kStartupStateFormatMinorCurrent,
      {},
      false,
      true);
  Require(!missing_plan.ok() &&
              missing_plan.diagnostic.diagnostic_code ==
                  "SB-STARTUP-STATE-MIGRATION-PLAN-MISSING",
          "startup state missing migration plan was not refused");
}

void TestServerConfigMigrationRefusals() {
  const auto current = server::ClassifyServerConfigFormat(server::kServerConfigFormatCurrent);
  Require(current.accepted &&
              current.compatibility_class ==
                  server::ServerConfigCompatibilityClass::kSupportedCurrent,
          "current server config format was not accepted");

  const auto old = server::ClassifyServerConfigFormat("SBCD0");
  Require(!old.accepted &&
              old.compatibility_class ==
                  server::ServerConfigCompatibilityClass::kMigrationRequiredWithoutPlanRefused &&
              old.diagnostic.code == "CONFIG.MIGRATION_REQUIRED_WITHOUT_PLAN",
          "old server config format did not require explicit migration plan");

  const auto future = server::ClassifyServerConfigFormat("SBCD2");
  Require(!future.accepted &&
              future.compatibility_class ==
                  server::ServerConfigCompatibilityClass::kNewerThanSupportedRefused &&
              future.diagnostic.code == "CONFIG.VERSION_NEWER_THAN_SUPPORTED",
          "future server config format was not refused");

  const auto missing_plan = server::ClassifyServerConfigFormat("SBCD1", {}, false, true);
  Require(!missing_plan.accepted &&
              missing_plan.diagnostic.code == "CONFIG.MIGRATION_PLAN_MISSING",
          "server config missing migration plan was not refused");

  const auto work = MakeTempDir();
  const auto config_path = work / "sb_server.conf";
  WriteTextFile(config_path, "[config]\nformat=SBCD0\n");
  server::ServerCliOptions cli;
  cli.config_path = config_path.string();
  const auto loaded = server::ResolveServerBootstrapConfig(cli);
  Require(!loaded.ok() &&
              HasDiagnostic(loaded.diagnostics, "CONFIG.MIGRATION_REQUIRED_WITHOUT_PLAN"),
          "old server config file was not refused through ResolveServerBootstrapConfig");
}

server::ServerIpcEndpointDescriptor CurrentEndpointDescriptor() {
  server::ServerIpcEndpointDescriptor descriptor;
  descriptor.endpoint_id = "parser_server_ipc:/tmp/sb_dblc013s.sock";
  descriptor.protocol_family = "parser_server_ipc";
  descriptor.transport = "af_unix";
  descriptor.endpoint_path = "/tmp/sb_dblc013s.sock";
  descriptor.descriptor_format_version =
      server::kServerIpcEndpointDescriptorFormatCurrent;
  descriptor.protocol_major = 1;
  descriptor.protocol_minor = 0;
  descriptor.lifecycle_generation = 10;
  descriptor.descriptor_generation = 10;
  descriptor.file_mode = 0600;
  descriptor.bound = true;
  descriptor.service_ready = true;
  return descriptor;
}

void TestLifecycleStateAndProtocolDescriptorMigrationRefusals() {
  server::ServerLifecycleArtifactMigrationRequest state_file;
  state_file.artifact_kind = "server_lifecycle_state_file";
  state_file.format_version = server::kServerLifecycleStateFileFormatCurrent;
  state_file.min_supported_version = server::kServerLifecycleStateFileFormatMinSupported;
  state_file.current_version = server::kServerLifecycleStateFileFormatCurrent;
  state_file.max_supported_version = server::kServerLifecycleStateFileFormatMaxSupported;
  Require(server::EvaluateServerLifecycleArtifactMigration(state_file).accepted,
          "current server lifecycle state file format was not accepted");

  state_file.format_version = 0;
  state_file.min_supported_version = 0;
  auto old_state = server::EvaluateServerLifecycleArtifactMigration(state_file);
  Require(!old_state.accepted &&
              old_state.compatibility_class ==
                  server::ServerLifecycleArtifactCompatibilityClass::
                      kMigrationRequiredWithoutPlanRefused &&
              old_state.diagnostic.code == "IPC.LIFECYCLE.MIGRATION_REQUIRED_WITHOUT_PLAN",
          "old lifecycle state file did not require explicit migration plan");

  state_file.format_version = server::kServerLifecycleStateFileFormatMaxSupported + 1;
  state_file.min_supported_version = server::kServerLifecycleStateFileFormatMinSupported;
  auto future_state = server::EvaluateServerLifecycleArtifactMigration(state_file);
  Require(!future_state.accepted &&
              future_state.diagnostic.code == "IPC.LIFECYCLE.VERSION_NEWER_THAN_SUPPORTED",
          "future lifecycle state file was not refused");

  state_file.format_version = server::kServerLifecycleStateFileFormatCurrent;
  state_file.identity_proven = false;
  auto ambiguous_state = server::EvaluateServerLifecycleArtifactMigration(state_file);
  Require(!ambiguous_state.accepted &&
              ambiguous_state.diagnostic.code == "IPC.LIFECYCLE.AMBIGUOUS_IDENTITY_REFUSED",
          "ambiguous lifecycle state file identity was not refused");

  auto descriptor = CurrentEndpointDescriptor();
  auto descriptor_ok = server::EvaluateServerIpcEndpointLifecycle(
      descriptor, server::ServerIpcEndpointOperation::kParserHello);
  Require(descriptor_ok.admitted && descriptor_ok.descriptor_valid,
          "current IPC descriptor was not admitted");

  descriptor.descriptor_format_version =
      server::kServerIpcEndpointDescriptorFormatMaxSupported + 1;
  auto descriptor_future = server::EvaluateServerIpcEndpointLifecycle(
      descriptor, server::ServerIpcEndpointOperation::kParserHello);
  Require(!descriptor_future.admitted &&
              HasDiagnostic(descriptor_future.diagnostics,
                            "IPC.LIFECYCLE.VERSION_NEWER_THAN_SUPPORTED"),
          "future IPC descriptor did not emit P13S newer-than-supported refusal");
}

void TestFilespaceManifestAndCatalogEvidence() {
  db::DatabaseCatalogMigrationEvidence current;
  Require(db::ClassifyDatabaseCatalogMigrationEvidence(current).ok(),
          "current catalog/filespace migration evidence was not accepted");

  auto old_manifest = current;
  old_manifest.filespace_catalog_manifest_format_version = 0;
  RequireCompatibilityCode(db::ClassifyDatabaseCatalogMigrationEvidence(old_manifest),
                           db::DatabaseOpenCompatibilityClass::migration_required_without_plan_refused,
                           "ENGINE.DBLC_MIGRATION_REQUIRED_WITHOUT_PLAN",
                           "old filespace manifest did not require migration plan");

  auto future_resource = current;
  future_resource.resource_seed_manifest_format_version = 2;
  RequireCompatibilityCode(db::ClassifyDatabaseCatalogMigrationEvidence(future_resource),
                           db::DatabaseOpenCompatibilityClass::newer_than_supported_refused,
                           "ENGINE.DBLC_FORMAT_NEWER_THAN_SUPPORTED",
                           "future resource seed manifest was not refused");

  auto ambiguous = current;
  ambiguous.database_catalog_record_count = 2;
  RequireCompatibilityCode(db::ClassifyDatabaseCatalogMigrationEvidence(ambiguous),
                           db::DatabaseOpenCompatibilityClass::ambiguous_identity_refused,
                           "ENGINE.DBLC_MIGRATION_AMBIGUOUS_IDENTITY_REFUSED",
                           "ambiguous catalog identity evidence was not refused");
}

struct Fixture {
  std::filesystem::path path;
  TypedUuid database_uuid;
  TypedUuid filespace_uuid;
  u32 page_size = 0;
};

Fixture CreateFixture(const std::filesystem::path& work,
                      std::string_view filename = "upgrade_migration.sbdb") {
  const auto now = UniqueMillis();
  const auto database_uuid = uuid::GenerateEngineIdentityV7(UuidKind::database, now);
  const auto filespace_uuid = uuid::GenerateEngineIdentityV7(UuidKind::filespace, now + 1);
  Require(database_uuid.ok() && filespace_uuid.ok(), "fixture UUID generation failed");

  Fixture fixture;
  fixture.path = work / filename;
  fixture.database_uuid = database_uuid.value;
  fixture.filespace_uuid = filespace_uuid.value;

  db::DatabaseCreateConfig create;
  create.path = fixture.path.string();
  create.database_uuid = fixture.database_uuid;
  create.filespace_uuid = fixture.filespace_uuid;
  create.creation_unix_epoch_millis = now;
  create.require_resource_seed_pack = false;
  create.allow_minimal_resource_bootstrap = true;
  create.allow_overwrite = true;
  const auto created = db::CreateDatabaseFile(create);
  if (!created.ok()) {
    std::cerr << created.diagnostic.diagnostic_code << '\n';
  }
  Require(created.ok(), "CreateDatabaseFile failed for P13S fixture");
  fixture.page_size = created.state.header.page_size;
  return fixture;
}

disk::SerializedDatabaseHeader ReadSerializedDatabaseHeader(const Fixture& fixture) {
  disk::FileDevice device;
  Require(device.Open(fixture.path.string(), disk::FileOpenMode::open_existing).ok(),
          "database header read open failed");
  disk::SerializedDatabaseHeader serialized{};
  const auto read = device.ReadAt(0, serialized.data(), serialized.size());
  Require(read.ok(), "database header read failed");
  return serialized;
}

void WriteSerializedDatabaseHeader(const Fixture& fixture,
                                   const disk::SerializedDatabaseHeader& serialized) {
  disk::FileDevice device;
  Require(device.Open(fixture.path.string(), disk::FileOpenMode::open_existing).ok(),
          "database header write open failed");
  const auto write = device.WriteAt(0, serialized.data(), serialized.size());
  Require(write.ok(), "database header write failed");
  Require(device.Sync().ok(), "database header write sync failed");
}

void StoreU32(disk::SerializedDatabaseHeader* serialized, u32 offset, u32 value) {
  const u32 stored = HostToLittle32(value);
  std::memcpy(serialized->data() + offset, &stored, sizeof(stored));
}

void StoreU64(disk::SerializedDatabaseHeader* serialized, u32 offset, u64 value) {
  const u64 stored = HostToLittle64(value);
  std::memcpy(serialized->data() + offset, &stored, sizeof(stored));
}

void MutateStartupDatabaseUuid(const Fixture& fixture) {
  disk::FileDevice device;
  Require(device.Open(fixture.path.string(), disk::FileOpenMode::open_existing).ok(),
          "startup mutation open failed");
  auto startup = db::ReadStartupStatePageBody(&device, fixture.page_size);
  Require(startup.ok(), "startup mutation read failed");
  const auto replacement_uuid = uuid::GenerateEngineIdentityV7(
      UuidKind::database,
      UniqueMillis());
  Require(replacement_uuid.ok(), "startup replacement UUID generation failed");
  startup.state.database_uuid = replacement_uuid.value;
  const auto written = db::WriteStartupStatePageBody(&device, startup.state);
  if (!written.ok()) {
    std::cerr << written.diagnostic.diagnostic_code << '\n';
  }
  Require(written.ok(), "startup mutation write failed");
  Require(device.Sync().ok(), "startup mutation sync failed");
}

void RewriteHeaderWithClusterAuthorityRequired(const Fixture& fixture) {
  const auto serialized = ReadSerializedDatabaseHeader(fixture);
  const auto parsed = disk::ParseDatabaseHeader(serialized);
  Require(parsed.ok(), "current header parse failed before cluster mutation");
  auto header = parsed.header;
  header.compatibility_flags |= disk::DatabaseCompatibilityFlag::requires_cluster_authority;
  const auto rewritten = disk::SerializeDatabaseHeader(header);
  Require(rewritten.ok(), "cluster-required header serialization failed");
  WriteSerializedDatabaseHeader(fixture, rewritten.serialized);
}

void RewriteHeaderWithFutureFormat(const Fixture& fixture) {
  auto serialized = ReadSerializedDatabaseHeader(fixture);
  StoreU32(&serialized,
           kSerializedHeaderFormatMajorOffset,
           disk::kScratchBirdDatabaseFormatMajor + 1);
  StoreU32(&serialized, kSerializedHeaderFormatMinorOffset, 0);
  WriteSerializedDatabaseHeader(fixture, serialized);
}

void RewriteHeaderWithUnknownRequiredFlag(const Fixture& fixture) {
  auto serialized = ReadSerializedDatabaseHeader(fixture);
  StoreU64(&serialized, kSerializedHeaderCompatibilityFlagsOffset, 1ull << 40);
  WriteSerializedDatabaseHeader(fixture, serialized);
}

void TestRealDatabaseOpenFailClosedDiagnostics() {
  const auto work = MakeTempDir();
  const auto fixture = CreateFixture(work, "current.sbdb");
  db::DatabaseOpenConfig open;
  open.path = fixture.path.string();
  open.read_only = true;
  const auto current = db::OpenDatabaseFile(open);
  if (!current.ok()) {
    std::cerr << current.diagnostic.diagnostic_code << '\n';
  }
  Require(current.ok(), "current database did not open read-only");
  Require(current.state.database_open_compatibility_class ==
              db::DatabaseOpenCompatibilityClass::current,
          "current database open did not report current compatibility");

  MutateStartupDatabaseUuid(fixture);
  const auto ambiguous = db::OpenDatabaseFile(open);
  Require(!ambiguous.ok() &&
              ambiguous.diagnostic.diagnostic_code ==
                  "ENGINE.DBLC_MIGRATION_AMBIGUOUS_IDENTITY_REFUSED",
          "ambiguous startup/database identity did not fail closed");

  const auto future_fixture = CreateFixture(work, "future_format.sbdb");
  RewriteHeaderWithFutureFormat(future_fixture);
  db::DatabaseOpenConfig future_open;
  future_open.path = future_fixture.path.string();
  future_open.read_only = true;
  const auto future = db::OpenDatabaseFile(future_open);
  Require(!future.ok() &&
              future.diagnostic.diagnostic_code == "FORMAT.VERSION_UNSUPPORTED",
          "future persisted database format did not fail closed");

  const auto unknown_flag_fixture = CreateFixture(work, "unknown_required_flag.sbdb");
  RewriteHeaderWithUnknownRequiredFlag(unknown_flag_fixture);
  db::DatabaseOpenConfig unknown_open;
  unknown_open.path = unknown_flag_fixture.path.string();
  unknown_open.read_only = true;
  const auto unknown_flag = db::OpenDatabaseFile(unknown_open);
  Require(!unknown_flag.ok() &&
              unknown_flag.diagnostic.diagnostic_code == "FORMAT.UNKNOWN_REQUIRED_FLAG",
          "unknown required compatibility flag did not fail closed");

  const auto cluster_fixture = CreateFixture(work, "cluster_required.sbdb");
  RewriteHeaderWithClusterAuthorityRequired(cluster_fixture);
  db::DatabaseOpenConfig cluster_open;
  cluster_open.path = cluster_fixture.path.string();
  cluster_open.read_only = true;
  const auto cluster_without_authority = db::OpenDatabaseFile(cluster_open);
  Require(!cluster_without_authority.ok() &&
              cluster_without_authority.diagnostic.diagnostic_code ==
                  "SB-DB-LIFECYCLE-CLUSTER-AUTHORITY-REQUIRED",
          "cluster-required database did not refuse without cluster authority");
  cluster_open.cluster_authority_available = true;
  const auto cluster_mapping_missing = db::OpenDatabaseFile(cluster_open);
  Require(!cluster_mapping_missing.ok() &&
              cluster_mapping_missing.diagnostic.diagnostic_code ==
                  "SB-DB-LIFECYCLE-CLUSTER-MAPPING-UNAVAILABLE",
          "cluster-required database did not refuse missing core cluster mapping");

  std::filesystem::remove_all(work);
}

}  // namespace

int main() {
  ConfigureMemoryFixture();
  TestDatabaseHeaderVersionClassification();
  TestStartupStateFormatClassification();
  TestServerConfigMigrationRefusals();
  TestLifecycleStateAndProtocolDescriptorMigrationRefusals();
  TestFilespaceManifestAndCatalogEvidence();
  TestRealDatabaseOpenFailClosedDiagnostics();
  return EXIT_SUCCESS;
}
