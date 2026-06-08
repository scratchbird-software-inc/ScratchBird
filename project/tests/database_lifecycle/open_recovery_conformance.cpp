// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "database_dirty_manifest.hpp"
#include "database_format.hpp"
#include "database_lifecycle.hpp"
#include "disk_device.hpp"
#include "runtime_platform.hpp"
#include "startup_state.hpp"
#include "uuid.hpp"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace db = scratchbird::storage::database;
namespace disk = scratchbird::storage::disk;
namespace uuid = scratchbird::core::uuid;
using scratchbird::core::platform::StoreLittle32;
using scratchbird::core::platform::StoreLittle64;
using scratchbird::core::platform::TypedUuid;
using scratchbird::core::platform::UuidKind;
using scratchbird::core::platform::u32;
using scratchbird::core::platform::u64;

constexpr u32 kHeaderFormatMajorOffset = 8;
constexpr u32 kHeaderCompatibilityFlagsOffset = 64;
constexpr u32 kHeaderChecksumOffset = 72;

struct Cleanup {
  std::vector<std::filesystem::path> database_paths;

  ~Cleanup() {
    for (const auto& path : database_paths) {
      std::error_code ignored;
      std::filesystem::remove(path, ignored);
      std::filesystem::remove(path.string() + ".sb.owner.lock", ignored);
      std::filesystem::remove(path.string() + ".dirty.manifest", ignored);
      std::filesystem::remove(path.string() + ".recovery.evidence", ignored);
    }
  }
};

struct Fixture {
  std::filesystem::path path;
  TypedUuid database_uuid;
  TypedUuid filespace_uuid;
  u32 page_size = 0;
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

u64 StableTextChecksum(const std::string& value) {
  u64 checksum = 1469598103934665603ull;
  for (unsigned char c : value) {
    checksum ^= static_cast<u64>(c);
    checksum *= 1099511628211ull;
  }
  return checksum;
}

std::filesystem::path TestDatabasePath(std::string_view label) {
  return std::filesystem::temp_directory_path() /
         ("sb_dblc_006_" + std::string(label) + "_" + std::to_string(UniqueMillis()) + ".sbdb");
}

std::string UuidString(const TypedUuid& value) {
  return uuid::UuidToString(value.value);
}

void RequireOk(const db::DatabaseLifecycleResult& result, std::string_view message) {
  if (!result.ok()) {
    std::cerr << result.diagnostic.diagnostic_code << '\n';
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

Fixture CreateFixture(Cleanup* cleanup,
                      std::string_view label,
                      u64 feature_flags = 0,
                      u64 compatibility_flags =
                          disk::DatabaseCompatibilityFlag::public_node_safe_header_open |
                          disk::DatabaseCompatibilityFlag::unknown_page_safe_classification_required) {
  Require(cleanup != nullptr, "cleanup registry is required");
  const auto now = UniqueMillis();
  const auto database_uuid = uuid::GenerateEngineIdentityV7(UuidKind::database, now);
  const auto filespace_uuid = uuid::GenerateEngineIdentityV7(UuidKind::filespace, now + 1);
  Require(database_uuid.ok() && filespace_uuid.ok(), "test UUID generation failed");

  const auto path = TestDatabasePath(label);
  cleanup->database_paths.push_back(path);

  db::DatabaseCreateConfig create;
  create.path = path.string();
  create.database_uuid = database_uuid.value;
  create.filespace_uuid = filespace_uuid.value;
  create.creation_unix_epoch_millis = now;
  create.feature_flags = feature_flags;
  create.compatibility_flags = compatibility_flags;
  create.resource_seed_pack_root = SB_BOOTSTRAP_SEED_PACK_ROOT;
  create.require_resource_seed_pack = true;
  create.allow_minimal_resource_bootstrap = false;
  create.allow_overwrite = true;

  const auto created = db::CreateDatabaseFile(create);
  RequireOk(created, "CreateDatabaseFile failed");

  Fixture fixture;
  fixture.path = path;
  fixture.database_uuid = database_uuid.value;
  fixture.filespace_uuid = filespace_uuid.value;
  fixture.page_size = created.state.header.page_size;
  return fixture;
}

db::DatabaseLifecycleResult OpenFixture(const Fixture& fixture, bool read_only) {
  db::DatabaseOpenConfig open;
  open.path = fixture.path.string();
  open.read_only = read_only;
  return db::OpenDatabaseFile(open);
}

db::StartupStateRecord ReadStartup(const Fixture& fixture) {
  disk::FileDevice device;
  const auto opened = device.Open(fixture.path.string(), disk::FileOpenMode::open_existing_read_only);
  if (!opened.ok()) {
    std::cerr << opened.diagnostic.diagnostic_code << '\n';
  }
  Require(opened.ok(), "startup state read open failed");
  const auto startup = db::ReadStartupStatePageBody(&device, fixture.page_size);
  if (!startup.ok()) {
    std::cerr << startup.diagnostic.diagnostic_code << '\n';
  }
  Require(startup.ok(), "startup state read failed");
  return startup.state;
}

template <typename Mutator>
void MutateStartup(const Fixture& fixture, Mutator mutator) {
  disk::FileDevice device;
  const auto opened = device.Open(fixture.path.string(), disk::FileOpenMode::open_existing);
  if (!opened.ok()) {
    std::cerr << opened.diagnostic.diagnostic_code << '\n';
  }
  Require(opened.ok(), "startup state mutation open failed");
  auto startup = db::ReadStartupStatePageBody(&device, fixture.page_size);
  if (!startup.ok()) {
    std::cerr << startup.diagnostic.diagnostic_code << '\n';
  }
  Require(startup.ok(), "startup state mutation read failed");
  mutator(&startup.state);
  const auto written = db::WriteStartupStatePageBody(&device, startup.state);
  if (!written.ok()) {
    std::cerr << written.diagnostic.diagnostic_code << '\n';
  }
  Require(written.ok(), "startup state mutation write failed");
  const auto synced = device.Sync();
  Require(synced.ok(), "startup state mutation sync failed");
}

void RewriteHeaderU32(const Fixture& fixture, u32 offset, u32 value) {
  disk::FileDevice device;
  const auto opened = device.Open(fixture.path.string(), disk::FileOpenMode::open_existing);
  if (!opened.ok()) {
    std::cerr << opened.diagnostic.diagnostic_code << '\n';
  }
  Require(opened.ok(), "header mutation open failed");
  disk::SerializedDatabaseHeader serialized{};
  const auto read = device.ReadAt(0, serialized.data(), serialized.size());
  Require(read.ok(), "header mutation read failed");
  StoreLittle32(serialized.data() + offset, value);
  StoreLittle64(serialized.data() + kHeaderChecksumOffset, 0);
  StoreLittle64(serialized.data() + kHeaderChecksumOffset,
                disk::ComputeDatabaseHeaderChecksum(serialized));
  const auto write = device.WriteAt(0, serialized.data(), serialized.size());
  Require(write.ok(), "header mutation write failed");
  const auto synced = device.Sync();
  Require(synced.ok(), "header mutation sync failed");
}

void RewriteHeaderU64(const Fixture& fixture, u32 offset, u64 value) {
  disk::FileDevice device;
  const auto opened = device.Open(fixture.path.string(), disk::FileOpenMode::open_existing);
  if (!opened.ok()) {
    std::cerr << opened.diagnostic.diagnostic_code << '\n';
  }
  Require(opened.ok(), "header mutation open failed");
  disk::SerializedDatabaseHeader serialized{};
  const auto read = device.ReadAt(0, serialized.data(), serialized.size());
  Require(read.ok(), "header mutation read failed");
  StoreLittle64(serialized.data() + offset, value);
  StoreLittle64(serialized.data() + kHeaderChecksumOffset, 0);
  StoreLittle64(serialized.data() + kHeaderChecksumOffset,
                disk::ComputeDatabaseHeaderChecksum(serialized));
  const auto write = device.WriteAt(0, serialized.data(), serialized.size());
  Require(write.ok(), "header mutation write failed");
  const auto synced = device.Sync();
  Require(synced.ok(), "header mutation sync failed");
}

void WriteTextFile(const std::filesystem::path& path, const std::string& text) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  Require(static_cast<bool>(out), "could not open text artifact for write");
  out << text;
  out.flush();
  Require(static_cast<bool>(out), "could not write text artifact");
}

std::string ManualDirtyManifestChecksumMaterial(const db::DirtyObjectManifest& manifest) {
  std::ostringstream out;
  out << "SBDIRTY1\t"
      << manifest.format_version << "\tclassification_only\t"
      << manifest.checkpoint_generation << '\t'
      << (manifest.completed ? 1 : 0) << '\t'
      << manifest.entries.size() << '\n';
  for (const auto& entry : manifest.entries) {
    out << "ENTRY\t"
        << db::DirtyObjectKindName(entry.kind) << "\tobject\t"
        << UuidString(entry.object_uuid) << '\t'
        << entry.page_number << '\t'
        << entry.page_generation << '\t'
        << entry.object_checksum << '\t'
        << entry.local_transaction_id << '\t'
        << entry.operation_envelope_checksum << '\t'
        << entry.transaction_evidence_checksum << '\t'
        << (entry.dirty ? 1 : 0) << '\t'
        << (entry.authoritative ? 1 : 0) << '\n';
  }
  return out.str();
}

std::string ManualDirtyManifestSerialize(db::DirtyObjectManifest manifest) {
  manifest.manifest_checksum = StableTextChecksum(ManualDirtyManifestChecksumMaterial(manifest));
  std::ostringstream out;
  out << "SBDIRTY1\t"
      << manifest.format_version << "\tclassification_only\t"
      << manifest.checkpoint_generation << '\t'
      << (manifest.completed ? 1 : 0) << '\t'
      << manifest.entries.size() << '\t'
      << manifest.manifest_checksum << '\n';
  for (const auto& entry : manifest.entries) {
    out << "ENTRY\t"
        << db::DirtyObjectKindName(entry.kind) << "\tobject\t"
        << UuidString(entry.object_uuid) << '\t'
        << entry.page_number << '\t'
        << entry.page_generation << '\t'
        << entry.object_checksum << '\t'
        << entry.local_transaction_id << '\t'
        << entry.operation_envelope_checksum << '\t'
        << entry.transaction_evidence_checksum << '\t'
        << (entry.dirty ? 1 : 0) << '\t'
        << (entry.authoritative ? 1 : 0) << '\n';
  }
  return out.str();
}

void WriteRecoverableDirtyManifest(const Fixture& fixture) {
  const auto object_uuid = uuid::GenerateEngineIdentityV7(UuidKind::object, UniqueMillis());
  Require(object_uuid.ok(), "dirty manifest object uuid generation failed");

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
  if (!built.ok()) {
    std::cerr << built.diagnostic.diagnostic_code << '\n';
  }
  Require(built.ok(), "recoverable dirty manifest did not build");
  WriteTextFile(fixture.path.string() + ".dirty.manifest", built.serialized);
}

void WriteQuarantineDirtyManifest(const Fixture& fixture) {
  const auto object_uuid = uuid::GenerateEngineIdentityV7(UuidKind::object, UniqueMillis());
  Require(object_uuid.ok(), "quarantine dirty manifest object uuid generation failed");
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
  entry.authoritative = false;
  manifest.entries.push_back(entry);
  WriteTextFile(fixture.path.string() + ".dirty.manifest",
                ManualDirtyManifestSerialize(manifest));
}

std::string ReadTextFile(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  Require(static_cast<bool>(in), "could not open text artifact for read");
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void TestSeedMismatchRefusesBeforeDirtyOrTx2(Cleanup* cleanup) {
  const auto fixture = CreateFixture(cleanup, "seed_mismatch");

  db::DatabaseOpenConfig open;
  open.path = fixture.path.string();
  open.read_only = false;
  open.expected_resource_seed_pack_content_hash = "not-the-current-seed-pack-hash";
  const auto failed = db::OpenDatabaseFile(open);
  RequireFailureCode(failed,
                     "FORMAT.UPGRADE_REQUIRED",
                     "seed mismatch did not refuse writable open");

  const auto startup = ReadStartup(fixture);
  Require(startup.clean_shutdown, "seed mismatch dirtied startup state before refusal");
  Require(!startup.startup_dirty, "seed mismatch left startup dirty before refusal");
  Require(startup.first_open_activation_local_transaction_id == 0,
          "seed mismatch allocated tx2 before refusal");
}

void TestCleanDirtyAndDirtyManifestRecovery(Cleanup* cleanup) {
  const auto fixture = CreateFixture(cleanup, "dirty_manifest_recovery");

  const auto first_open = OpenFixture(fixture, false);
  RequireOk(first_open, "first writable open failed");
  Require(first_open.state.startup_recovery_classification == "clean_checkpoint_path",
          "first writable open did not classify clean checkpoint path");

  WriteRecoverableDirtyManifest(fixture);

  const auto recovered_open = OpenFixture(fixture, false);
  RequireOk(recovered_open, "dirty manifest recovery open failed");
  Require(recovered_open.state.startup_recovery_classification == "repaired_recovery",
          "dirty manifest recovery did not classify repaired_recovery");

  const auto evidence_path = fixture.path.string() + ".recovery.evidence";
  Require(std::filesystem::exists(evidence_path),
          "dirty manifest recovery did not persist recovery evidence");
  const auto evidence = ReadTextFile(evidence_path);
  Require(evidence.find("SBRECOVERY1") != std::string::npos,
          "dirty manifest recovery evidence missing marker");
  Require(evidence.find("WAL") == std::string::npos &&
              evidence.find("wal") == std::string::npos,
          "dirty manifest recovery evidence contained WAL language");

  const auto second_recovery_open = OpenFixture(fixture, false);
  RequireOk(second_recovery_open, "dirty manifest second recovery open failed");
  const auto evidence_after_second_open = ReadTextFile(evidence_path);
  Require(evidence_after_second_open == evidence,
          "dirty manifest recovery evidence was not idempotent");
}

void TestCleanShutdownEmitsVersionedDirtyManifest(Cleanup* cleanup) {
  const auto fixture = CreateFixture(cleanup, "dirty_manifest_emit");
  const auto opened = OpenFixture(fixture, false);
  RequireOk(opened, "open before manifest emission failed");
  const auto closed = db::MarkDatabaseCleanShutdown(fixture.path.string());
  if (!closed.ok()) {
    std::cerr << closed.diagnostic.diagnostic_code << '\n';
  }
  Require(closed.ok(), "clean shutdown failed before manifest emission");
  const auto manifest_path = fixture.path.string() + ".dirty.manifest";
  Require(std::filesystem::exists(manifest_path),
          "clean shutdown did not emit dirty manifest");
  const auto parsed = db::ParseDirtyObjectManifest(ReadTextFile(manifest_path));
  if (!parsed.ok()) {
    std::cerr << parsed.diagnostic.diagnostic_code << '\n';
  }
  Require(parsed.ok(), "emitted dirty manifest did not parse");
  Require(parsed.manifest.format_version == db::kDirtyObjectManifestFormatVersion,
          "emitted dirty manifest omitted format version");
  Require(!parsed.manifest.entries.empty(),
          "emitted dirty manifest did not contain checkpoint entries");
}

void TestUnprovenDirtyManifestRefuses(Cleanup* cleanup) {
  const auto fixture = CreateFixture(cleanup, "dirty_manifest_unproven");
  const auto first_open = OpenFixture(fixture, false);
  RequireOk(first_open, "first writable open for unproven manifest failed");
  const auto object_uuid = uuid::GenerateEngineIdentityV7(UuidKind::object, UniqueMillis());
  Require(object_uuid.ok(), "unproven dirty manifest object uuid generation failed");
  db::DirtyObjectManifest manifest;
  manifest.checkpoint_generation = 2;
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
  WriteTextFile(fixture.path.string() + ".dirty.manifest",
                ManualDirtyManifestSerialize(manifest));

  const auto failed = OpenFixture(fixture, false);
  RequireFailureCode(failed,
                     "SB-DB-LIFECYCLE-RECOVERY-UNPROVEN-MANIFEST",
                     "unproven dirty manifest did not fail closed");
}

void TestDirtyManifestChecksumMismatchRefuses(Cleanup* cleanup) {
  const auto fixture = CreateFixture(cleanup, "dirty_manifest_checksum");
  const auto first_open = OpenFixture(fixture, false);
  RequireOk(first_open, "first writable open for checksum manifest failed");
  WriteRecoverableDirtyManifest(fixture);
  auto manifest = ReadTextFile(fixture.path.string() + ".dirty.manifest");
  const auto pos = manifest.find("\t177\t");
  Require(pos != std::string::npos, "test manifest checksum field not found");
  manifest.replace(pos, 5, "\t178\t");
  WriteTextFile(fixture.path.string() + ".dirty.manifest", manifest);

  const auto failed = OpenFixture(fixture, false);
  RequireFailureCode(failed,
                     "SB-DIRTY-MANIFEST-CHECKSUM-MISMATCH",
                     "dirty manifest checksum mismatch did not fail closed");
}

void TestReadOnlyDirtyOpenClassifiesWithoutMutation(Cleanup* cleanup) {
  const auto fixture = CreateFixture(cleanup, "readonly_dirty");
  const auto first_open = OpenFixture(fixture, false);
  RequireOk(first_open, "first writable open for read-only dirty test failed");
  const auto before = ReadStartup(fixture);

  const auto read_only_open = OpenFixture(fixture, true);
  RequireOk(read_only_open, "read-only dirty open failed");
  Require(read_only_open.state.read_only_open, "dirty read-only open did not report read-only");
  Require(read_only_open.state.startup_recovery_classification == "repaired_recovery",
          "dirty read-only open did not classify repaired_recovery");

  const auto after = ReadStartup(fixture);
  Require(after.startup_counter == before.startup_counter,
          "read-only dirty open mutated startup counter");
  Require(after.lifecycle_generation == before.lifecycle_generation,
          "read-only dirty open mutated lifecycle generation");
}

void TestQuarantineDirtyManifestRefuses(Cleanup* cleanup) {
  const auto fixture = CreateFixture(cleanup, "dirty_manifest_quarantine");
  const auto first_open = OpenFixture(fixture, false);
  RequireOk(first_open, "first writable open for quarantine test failed");
  WriteQuarantineDirtyManifest(fixture);

  const auto failed = OpenFixture(fixture, false);
  RequireFailureCode(failed,
                     "SB-DB-LIFECYCLE-RECOVERY-QUARANTINE-REQUIRED",
                     "quarantine dirty manifest did not refuse open");
}

void TestStartupIdentityMismatchRefuses(Cleanup* cleanup) {
  const auto fixture = CreateFixture(cleanup, "startup_identity_mismatch");
  const auto wrong_filespace_uuid = uuid::GenerateEngineIdentityV7(UuidKind::filespace, UniqueMillis());
  Require(wrong_filespace_uuid.ok(), "wrong filespace uuid generation failed");
  MutateStartup(fixture, [&](db::StartupStateRecord* startup) {
    startup->first_filespace_uuid = wrong_filespace_uuid.value;
  });

  const auto failed = OpenFixture(fixture, false);
  RequireFailureCode(failed,
                     "SB-DB-LIFECYCLE-STARTUP-PAGE-FILESPACE-UUID-MISMATCH",
                     "startup/page filespace mismatch did not refuse open");
}

void TestRestrictedWritableOpenAndReadOnlyInspection(Cleanup* cleanup) {
  const auto fixture = CreateFixture(cleanup, "restricted_open");
  MutateStartup(fixture, [&](db::StartupStateRecord* startup) {
    startup->clean_shutdown = false;
    startup->startup_dirty = false;
    startup->write_admission_fenced = true;
  });

  const auto failed = OpenFixture(fixture, false);
  RequireFailureCode(failed,
                     "SB-DB-LIFECYCLE-RESTRICTED-OPEN-REQUIRED",
                     "restricted startup state did not refuse writable open");

  const auto read_only = OpenFixture(fixture, true);
  RequireOk(read_only, "restricted startup read-only inspection failed");
  Require(read_only.state.startup_recovery_classification == "fence_writes_until_safe",
          "restricted startup read-only inspection reported wrong classification");
}

void TestClusterAndDecryptionFlagsFailClosed(Cleanup* cleanup) {
  const auto cluster_fixture = CreateFixture(
      cleanup,
      "cluster_feature",
      disk::DatabaseFeatureFlag::cluster_structures_present);

  db::DatabaseOpenConfig no_cluster_auth;
  no_cluster_auth.path = cluster_fixture.path.string();
  const auto no_cluster = db::OpenDatabaseFile(no_cluster_auth);
  RequireFailureCode(no_cluster,
                     "SB-DB-LIFECYCLE-CLUSTER-AUTHORITY-REQUIRED",
                     "cluster feature flag did not fail closed without authority");

  db::DatabaseOpenConfig mapping_unavailable;
  mapping_unavailable.path = cluster_fixture.path.string();
  mapping_unavailable.cluster_authority_available = true;
  const auto no_mapping = db::OpenDatabaseFile(mapping_unavailable);
  RequireFailureCode(no_mapping,
                     "SB-DB-LIFECYCLE-CLUSTER-MAPPING-UNAVAILABLE",
                     "cluster feature flag did not fail closed when mapping is unavailable");

  const auto encrypted_fixture = CreateFixture(
      cleanup,
      "encrypted_required",
      0,
      disk::DatabaseCompatibilityFlag::public_node_safe_header_open |
          disk::DatabaseCompatibilityFlag::unknown_page_safe_classification_required |
          disk::DatabaseCompatibilityFlag::requires_decryption_password);
  const auto encrypted_without_key = OpenFixture(encrypted_fixture, false);
  RequireFailureCode(encrypted_without_key,
                     "SB-DB-LIFECYCLE-DECRYPTION-REQUIRED",
                     "decryption-required database did not fail closed without key authority");
}

void TestFormatAndRequiredFlagRefusals(Cleanup* cleanup) {
  const auto unsupported_fixture = CreateFixture(cleanup, "unsupported_format");
  RewriteHeaderU32(unsupported_fixture, kHeaderFormatMajorOffset, 2);
  const auto unsupported = OpenFixture(unsupported_fixture, false);
  RequireFailureCode(unsupported,
                     "FORMAT.VERSION_UNSUPPORTED",
                     "unsupported major format did not fail closed");

  const auto unknown_flag_fixture = CreateFixture(cleanup, "unknown_required_flag");
  RewriteHeaderU64(unknown_flag_fixture,
                   kHeaderCompatibilityFlagsOffset,
                   disk::DatabaseCompatibilityFlag::public_node_safe_header_open |
                       disk::DatabaseCompatibilityFlag::unknown_page_safe_classification_required |
                       (1ull << 40));
  const auto unknown_flag = OpenFixture(unknown_flag_fixture, false);
  RequireFailureCode(unknown_flag,
                     "FORMAT.UNKNOWN_REQUIRED_FLAG",
                     "unknown required compatibility flag did not fail closed");
}

}  // namespace

int main() {
  Cleanup cleanup;
  TestSeedMismatchRefusesBeforeDirtyOrTx2(&cleanup);
  TestCleanDirtyAndDirtyManifestRecovery(&cleanup);
  TestCleanShutdownEmitsVersionedDirtyManifest(&cleanup);
  TestUnprovenDirtyManifestRefuses(&cleanup);
  TestDirtyManifestChecksumMismatchRefuses(&cleanup);
  TestReadOnlyDirtyOpenClassifiesWithoutMutation(&cleanup);
  TestQuarantineDirtyManifestRefuses(&cleanup);
  TestStartupIdentityMismatchRefuses(&cleanup);
  TestRestrictedWritableOpenAndReadOnlyInspection(&cleanup);
  TestClusterAndDecryptionFlagsFailClosed(&cleanup);
  TestFormatAndRequiredFlagRefusals(&cleanup);
  return EXIT_SUCCESS;
}
