// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "database_dirty_manifest.hpp"
#include "../support/binary_uuid_fixture.hpp"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <chrono>
#include <iostream>
#include <source_location>
namespace db = scratchbird::storage::database;
namespace p = scratchbird::core::platform;
static void Check(bool value, std::source_location at = std::source_location::current()) {
  if (!value) { std::cerr << "dirty manifest binary failure at " << at.line() << '\n'; std::abort(); }
}
int main() {
  db::DirtyObjectManifest manifest;
  manifest.checkpoint_generation = 1; manifest.completed = true;
  db::DirtyObjectManifestEntry entry;
  entry.kind = db::DirtyObjectKind::catalog_page;
  auto identity = scratchbird::tests::FixtureUuidLiteral("019d0000-0000-7000-8000-000000000901");
  identity.bytes[9] = '\n'; identity.bytes[10] = '\t'; identity.bytes[11] = 0;
  identity.bytes[12] = 'W'; identity.bytes[13] = 'A'; identity.bytes[14] = 'L';
  entry.object_uuid = scratchbird::core::uuid::MakeDurableEngineIdentityUuid(p::UuidKind::page, identity).value;
  entry.page_number = 4; entry.page_generation = 2; entry.object_checksum = 3;
  entry.local_transaction_id = 1; entry.operation_envelope_checksum = 4; entry.transaction_evidence_checksum = 5;
  manifest.entries.push_back(entry);
  const auto built = db::BuildDirtyObjectManifest(manifest);
  Check(built.ok() && built.serialized.size() == 112 && built.serialized.starts_with("SBDIRTY2"));
  const std::string binary(reinterpret_cast<const char*>(identity.bytes.data()), 16);
  Check(built.serialized.substr(44, 16) == binary);
  const auto parsed = db::ParseDirtyObjectManifest(built.serialized);
  Check(parsed.ok() && parsed.serialized == built.serialized && parsed.manifest.entries.size() == 1);
  Check(parsed.manifest.entries[0].object_uuid.value == identity);
  Check(!db::ParseDirtyObjectManifest("SBDIRTY1\t1\tclassification_only\n").ok());
  for (std::size_t size = 0; size < built.serialized.size(); ++size)
    Check(!db::ParseDirtyObjectManifest(built.serialized.substr(0, size)).ok());
  Check(!db::ParseDirtyObjectManifest(built.serialized + "x").ok());
  for (const std::size_t offset : {12u, 24u, 32u, 40u, 44u, 108u}) {
    auto corrupted = built.serialized; corrupted[offset] ^= 0x40;
    Check(!db::ParseDirtyObjectManifest(corrupted).ok());
  }
  auto invalid = manifest; invalid.entries[0].object_uuid.value.bytes[6] = 0x40;
  Check(!db::BuildDirtyObjectManifest(invalid).ok());
  invalid = manifest; invalid.classification_only = false;
  Check(!db::BuildDirtyObjectManifest(invalid).ok());
  invalid = manifest; invalid.entries[0].authoritative = false;
  Check(!db::BuildDirtyObjectManifest(invalid).ok());
  const auto directory = std::filesystem::temp_directory_path() /
      ("sb_dirty_binary_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  std::filesystem::create_directory(directory);
  const auto evidence_path = directory / "recovery.sbrev";
  const auto recovery = db::ClassifyDirtyObjectManifestForRecovery(built.manifest);
  Check(recovery.ok());
  const auto run = scratchbird::core::uuid::MakeDurableEngineIdentityUuid(p::UuidKind::object, identity);
  Check(run.ok());
  const auto first = db::PersistDirtyManifestRecoveryRunEvidence(evidence_path.string(), built.manifest, recovery, run.value);
  Check(first.ok() && !first.already_recorded && first.serialized.size() == 64);
  Check(first.serialized.substr(8, 16) == binary && first.evidence.recovery_run_uuid.value == identity);
  const auto repeated = db::PersistDirtyManifestRecoveryRunEvidence(evidence_path.string(), built.manifest, recovery, run.value);
  Check(repeated.ok() && repeated.already_recorded && std::filesystem::file_size(evidence_path) == 64);
  auto next = manifest; next.checkpoint_generation = 2;
  const auto next_built = db::BuildDirtyObjectManifest(next);
  const auto next_recovery = db::ClassifyDirtyObjectManifestForRecovery(next_built.manifest);
  Check(next_built.ok() && next_recovery.ok());
  {
    std::ofstream out(evidence_path, std::ios::binary | std::ios::app); out.put('x');
  }
  Check(!db::PersistDirtyManifestRecoveryRunEvidence(evidence_path.string(), next_built.manifest, next_recovery, run.value).ok());
  Check(!db::PersistDirtyManifestRecoveryRunEvidence(evidence_path.string(), built.manifest, recovery, run.value).ok());
  Check(std::filesystem::file_size(evidence_path) == 65);
  invalid = manifest; invalid.entries[0].object_uuid.kind = static_cast<p::UuidKind>(65535);
  Check(!db::BuildDirtyObjectManifest(invalid).ok());
  std::filesystem::remove_all(directory);
  return EXIT_SUCCESS;
}
