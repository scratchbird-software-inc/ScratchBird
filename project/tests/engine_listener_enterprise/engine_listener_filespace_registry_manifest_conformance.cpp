// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "filespace_lifecycle.hpp"
#include "filespace_registry_binary_codec.hpp"
#include "temp_workspace_lifecycle.hpp"
#include "uuid.hpp"
#include "../support/binary_uuid_fixture.hpp"
#include "../support/owned_temp_directory.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <stdexcept>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace {

namespace filespace = scratchbird::storage::filespace;
namespace memory = scratchbird::core::memory;
namespace platform = scratchbird::core::platform;
namespace uuid = scratchbird::core::uuid;

[[noreturn]] void Fail(std::string_view message) {
  throw std::runtime_error(std::string(message));
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

platform::TypedUuid MakeUuid(platform::UuidKind kind, platform::u64 seed) {
  const auto generated = uuid::GenerateEngineIdentityV7(kind, 1970000000000ull + seed);
  if (!generated.ok()) {
    Fail("uuid generation failed");
  }
  return generated.value;
}

std::string ReadFile(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  Require(static_cast<bool>(in), "read file open failed");
  std::ostringstream out;
  out << in.rdbuf();
  return out.str();
}

void WriteFile(const std::filesystem::path& path, const std::string& content) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  Require(static_cast<bool>(out), "write file open failed");
  out << content;
  out.close();
  Require(static_cast<bool>(out), "write file flush failed");
}

filespace::FilespaceRegistry MakeRegistry(const std::filesystem::path& member_path,
                                          const platform::TypedUuid& writer_uuid) {
  filespace::FilespaceDescriptor descriptor;
  descriptor.database_uuid = MakeUuid(platform::UuidKind::database, 1);
  descriptor.filespace_uuid = MakeUuid(platform::UuidKind::filespace, 2);
  descriptor.path = member_path.string();
  descriptor.role = filespace::FilespaceRole::active_primary;
  descriptor.state = filespace::FilespaceState::online;
  descriptor.page_size = 16384;
  descriptor.generation = 3;
  descriptor.read_only = false;
  descriptor.startup_authority = true;
  descriptor.catalog_persistence_owner = true;
  descriptor.filespace_manifest_owner = true;
  descriptor.recovery_evidence_owner = true;
  descriptor.first_filespace = true;
  descriptor.active = true;
  descriptor.physical_filespace_id = 1;
  descriptor.total_pages = 64;
  descriptor.free_pages = 48;
  descriptor.preallocated_pages = 8;
  descriptor.allocation_root_page = 2;
  descriptor.header_generation = 5;
  descriptor.writer_identity_uuid = writer_uuid;

  filespace::FilespaceRegistry registry;
  registry.next_evidence_sequence = 9;
  registry.filespaces.push_back(std::move(descriptor));
  return registry;
}

void TestFilespaceRegistryManifest(const std::filesystem::path& root) {
  const auto writer_uuid = MakeUuid(platform::UuidKind::object, 3);
  const auto registry_path = root / "registry" / "filespace.registry.manifest";
  const auto member_path = root / "member-001.sbfs";
  auto registry = MakeRegistry(member_path, writer_uuid);
  registry.filespaces[0].pins.push_back({filespace::FilespacePinKind::backup, std::string("a|b\n\0c", 6), UINT64_MAX});
  registry.evidence.push_back({8, filespace::FilespaceOperation::verify_filespace,
      registry.filespaces[0].filespace_uuid, filespace::FilespaceState::online,
      filespace::FilespaceState::online, filespace::FilespaceRole::active_primary,
      filespace::FilespaceRole::active_primary, "reason|with\nframing", "SB-FILESPACE-LIFECYCLE-OK", false});

  const auto encoded = filespace::SerializeFilespaceRegistry(registry);
  Require(encoded.ok(), "binary registry encoding refused valid fixture");
  std::string golden = "SBFSREG3";
  const auto n = [&](std::uint64_t value, unsigned width) {
    for (unsigned i = 0; i < width; ++i) golden.push_back(static_cast<char>(value >> (8 * i)));
  };
  const auto id = [&](const platform::TypedUuid& value) {
    golden.append(reinterpret_cast<const char*>(value.value.bytes.data()), 16);
  };
  const auto text = [&](const std::string& value) { n(value.size(), 4); golden.append(value); };
  n(0, 8); n(9, 8); n(1, 4); n(1, 4);
  const auto& d = registry.filespaces[0];
  id(d.database_uuid); id(d.filespace_uuid); id(d.writer_identity_uuid);
  n(1, 2); n(1, 2); n(16384, 4); n(3, 8); n(0x1f8, 2); n(1, 2);
  n(64, 8); n(48, 8); n(8, 8); n(2, 8); n(5, 8); text(d.path);
  n(1, 4); n(2, 2); text(d.pins[0].owner); n(UINT64_MAX, 8);
  n(8, 8); n(11, 2); id(d.filespace_uuid); n(1, 2); n(1, 2); n(1, 2); n(1, 2); n(0, 1);
  text(registry.evidence[0].reason); text(registry.evidence[0].diagnostic_code);
  for (unsigned i = 0; i < 8; ++i) golden[8+i] = static_cast<char>(std::uint64_t(golden.size()) >> (8*i));
  Require(encoded.payload == golden, "registry differs from independent exact binary wire oracle");
  const auto decoded = filespace::ParseFilespaceRegistry(golden);
  Require(decoded.ok() && decoded.registry.filespaces.size() == 1 && decoded.registry.evidence.size() == 1 &&
              decoded.registry.filespaces[0].database_uuid.value == d.database_uuid.value &&
              decoded.registry.filespaces[0].filespace_uuid.value == d.filespace_uuid.value &&
              decoded.registry.filespaces[0].pins[0].owner == d.pins[0].owner &&
              decoded.registry.filespaces[0].pins[0].count == UINT64_MAX &&
              filespace::SerializeFilespaceRegistry(decoded.registry).payload == golden,
          "binary registry loses IDs, framed strings, bounds or exact bytes");
  for (std::size_t size = 0; size < golden.size(); ++size) {
    const auto rejected = filespace::ParseFilespaceRegistry(golden.substr(0, size));
    Require(!rejected.ok() && rejected.registry.filespaces.empty() && rejected.registry.evidence.empty(),
            "truncated registry published partial state");
  }
  for (const auto* old : {"scratchbird.filespace.registry.v1|1\n", "scratchbird.filespace.registry.v2|1\n"})
    Require(!filespace::ParseFilespaceRegistry(old).ok(), "text UUID registry accepted by native reader");
  auto duplicate = registry; duplicate.filespaces.push_back(d);
  const auto rejected_duplicate = filespace::SerializeFilespaceRegistry(duplicate);
  Require(!rejected_duplicate.ok() && rejected_duplicate.payload.empty(), "duplicate binary identity encoded");
  for (const auto at : {std::size_t{38}, std::size_t{54}, std::size_t{70}}) {
    auto wrong = golden; wrong[at] = 0x40;
    const auto rejected = filespace::ParseFilespaceRegistry(wrong);
    Require(!rejected.ok() && rejected.registry.filespaces.empty(), "non-v7 system identity decoded");
  }
  auto impossible = golden; impossible[24] = impossible[25] = impossible[26] = impossible[27] = char(255);
  Require(!filespace::ParseFilespaceRegistry(impossible).ok(), "unbounded row count accepted");
  auto trailing = golden; trailing.push_back(0);
  Require(!filespace::ParseFilespaceRegistry(trailing).ok(), "trailing registry bytes accepted");
  for (unsigned fault = 0; fault < 9; ++fault) {
    auto invalid = registry;
    auto& descriptor = invalid.filespaces[0];
    if (fault == 0) descriptor.database_uuid.kind = platform::UuidKind::object;
    if (fault == 1) descriptor.filespace_uuid.value = {};
    if (fault == 2) descriptor.writer_identity_uuid.value = {};
    if (fault == 3) descriptor.free_pages = 65;
    if (fault == 4) descriptor.header_generation = 0;
    if (fault == 5) descriptor.state = static_cast<filespace::FilespaceState>(65535);
    if (fault == 6) descriptor.role = static_cast<filespace::FilespaceRole>(65535);
    if (fault == 7) descriptor.page_size = 24576;
    if (fault == 8) invalid.next_evidence_sequence = 8;
    std::string output = "unchanged";
    Require(filespace::registry_binary::Encode(invalid, output) != filespace::registry_binary::Error::none &&
                output == "unchanged", "invalid registry changed encoder destination");
  }
  for (const auto [offset, width] : {std::pair<std::size_t, unsigned>{8,8}, {96,2}, {140,4}}) {
    auto invalid = golden;
    for (unsigned i = 0; i < width; ++i) invalid[offset+i] = char(255);
    auto unchanged = registry;
    Require(filespace::registry_binary::Decode(invalid, unchanged) != filespace::registry_binary::Error::none &&
                filespace::SerializeFilespaceRegistry(unchanged).payload == golden,
            "malformed length or reserved flags changed decoder destination");
  }

  filespace::FilespaceRegistryManifestWriteRequest write_request;
  write_request.path = registry_path;
  write_request.generation = 7;
  write_request.writer_identity_uuid = writer_uuid;

  auto persisted = filespace::PersistFilespaceRegistryManifest(registry, write_request);
  Require(persisted.ok(), "filespace registry manifest persist failed");
  Require(persisted.payload_written, "filespace manifest did not report payload write");
  Require(persisted.file_synced, "filespace manifest did not report file sync");
  Require(persisted.renamed, "filespace manifest did not report atomic rename");
  Require(persisted.parent_synced, "filespace manifest did not report parent sync");
  Require(!persisted.checksum.empty(), "filespace manifest checksum missing");
  Require(std::filesystem::is_regular_file(registry_path), "filespace manifest missing");
  const auto manifest = ReadFile(registry_path);
  Require(manifest.substr(0, 8) == "SBFSRM03" && manifest.substr(48) == golden &&
              manifest.compare(16, 16, reinterpret_cast<const char*>(writer_uuid.value.bytes.data()), 16) == 0 &&
              manifest.find(uuid::UuidToString(writer_uuid.value)) == std::string::npos,
          "manifest does not retain binary writer and binary registry body");
  Require(!std::filesystem::exists(registry_path.string() + ".tmp"),
          "filespace manifest temp survived successful persist");

  filespace::FilespaceRegistryManifestLoadRequest load_request;
  load_request.path = registry_path;
  load_request.expected_writer_identity_uuid = writer_uuid;
  auto loaded = filespace::LoadFilespaceRegistryManifest(load_request);
  Require(loaded.ok(), "filespace registry manifest load failed");
  Require(loaded.generation == 7, "filespace manifest generation mismatch");
  Require(loaded.checksum_verified, "filespace manifest checksum was not verified");
  Require(loaded.writer_identity_verified, "filespace manifest writer was not verified");
  Require(loaded.registry.filespaces.size() == 1, "filespace registry count mismatch");
  Require(loaded.registry.filespaces[0].header_generation == 5,
          "filespace header generation did not survive manifest round trip");
  Require(loaded.registry.filespaces[0].writer_identity_uuid.value == writer_uuid.value,
          "filespace writer uuid did not survive manifest round trip");
  load_request.expected_writer_identity_uuid = MakeUuid(platform::UuidKind::object, 31);
  const auto wrong_owner = filespace::LoadFilespaceRegistryManifest(load_request);
  Require(!wrong_owner.ok() && wrong_owner.registry.filespaces.empty() &&
              wrong_owner.diagnostic.diagnostic_code == "SB-FILESPACE-REGISTRY-MANIFEST-LOAD-WRITER-UUID-MISMATCH",
          "wrong binary writer identity accepted");
  load_request.expected_writer_identity_uuid = {};
  const auto unbound = filespace::LoadFilespaceRegistryManifest(load_request);
  Require(unbound.ok() && !unbound.writer_identity_verified,
          "structural parsing manufactured an expected writer match");
  load_request.expected_writer_identity_uuid.value = writer_uuid.value;
  Require(!filespace::LoadFilespaceRegistryManifest(load_request).ok(),
          "nonempty unknown-kind expected owner was silently treated as absent");
  load_request.expected_writer_identity_uuid = {platform::UuidKind::object, {}};
  Require(!filespace::LoadFilespaceRegistryManifest(load_request).ok(),
          "explicit nil expected owner was silently treated as absent");
  load_request.expected_writer_identity_uuid = writer_uuid;
  auto changed_generation = manifest;
  changed_generation[8] ^= 1;
  WriteFile(registry_path, changed_generation);
  Require(!filespace::LoadFilespaceRegistryManifest(load_request).ok(),
          "manifest checksum did not cover generation binding");
  WriteFile(registry_path, manifest);

  WriteFile(registry_path.string() + ".tmp", "stale-temp");
  write_request.generation = 8;
  persisted = filespace::PersistFilespaceRegistryManifest(registry, write_request);
  Require(persisted.ok(), "filespace registry manifest stale temp persist failed");
  Require(persisted.stale_temp_removed, "filespace stale temp removal was not reported");
  Require(!std::filesystem::exists(registry_path.string() + ".tmp"),
          "filespace stale temp survived persist");

  std::string tampered = ReadFile(registry_path);
  tampered += "#tamper\n";
  WriteFile(registry_path, tampered);
  loaded = filespace::LoadFilespaceRegistryManifest(load_request);
  Require(!loaded.ok(), "tampered filespace manifest loaded successfully");
  Require(loaded.diagnostic.diagnostic_code ==
              "SB-FILESPACE-REGISTRY-MANIFEST-LOAD-CHECKSUM-MISMATCH",
          "tampered filespace manifest did not fail checksum verification");
}

memory::TempWorkspacePolicy TempPolicy(const std::filesystem::path& root) {
  memory::TempWorkspacePolicy policy;
  policy.policy_name = "eler013_temp_workspace";
  policy.database_uuid = scratchbird::tests::FixtureUuid(13, 1);
  policy.engine_uuid = scratchbird::tests::FixtureUuid(13, 2);
  policy.manifest_generation = 13;
  policy.manifest_writer_identity = "eler013-manifest-writer";
  policy.root_path = root;
  policy.filespace_quota_bytes = 4096;
  policy.session_quota_bytes = 4096;
  policy.transaction_quota_bytes = 4096;
  policy.statement_quota_bytes = 4096;
  policy.operation_quota_bytes = 4096;
  policy.create_root_path = true;
  policy.sparse_file_reservation = true;
  return policy;
}

memory::TempWorkspaceAllocationRequest TempRequest(std::uint32_t ordinal) {
  using scratchbird::tests::FixtureUuid;
  memory::TempWorkspaceAllocationRequest request;
  request.owner.temp_object_uuid = FixtureUuid(0x1300, ordinal);
  request.owner.database_id = FixtureUuid(13, 1);
  request.owner.engine_id = FixtureUuid(13, 2);
  request.owner.session_id = FixtureUuid(13, 3);
  request.owner.transaction_id = FixtureUuid(13, 4);
  request.owner.statement_id = FixtureUuid(13, 5);
  request.owner.operation_id = FixtureUuid(13, 6);
  request.owner.policy_generation = 7;
  request.owner.security_generation = 9;
  request.owner.resource_budget_reference = FixtureUuid(13, 7);
  request.owner.snapshot_boundary = FixtureUuid(13, 8);
  request.owner.metadata_boundary = FixtureUuid(13, 9);
  request.bytes = 128;
  request.purpose = "ELER-013 durable temp workspace manifest proof";
  return request;
}

std::filesystem::path TempManifestPath(const std::filesystem::path& root) {
  return root / ".scratchbird_temp_workspace_manifest.v3";
}

void TestTempWorkspaceManifest(const std::filesystem::path& root) {
  const auto temp_root = root / "temp-workspace";
  const auto policy = TempPolicy(temp_root);
  {
    memory::TempWorkspaceLifecycleManager manager(policy);
    auto first = manager.AllocateSpillFile(TempRequest(1));
    Require(first.ok() && first.record.has_value(), "temp workspace first allocation failed");

    const auto manifest_path = TempManifestPath(temp_root);
    Require(std::filesystem::is_regular_file(manifest_path), "temp workspace manifest missing");
    std::string manifest = ReadFile(manifest_path);
    Require(manifest.find("META\t") != std::string::npos,
            "temp workspace manifest metadata row missing");
    Require(manifest.find("fnv1a64-temp-workspace-manifest-v1") != std::string::npos,
            "temp workspace manifest checksum algorithm missing");
    Require(manifest.find("eler013-manifest-writer") != std::string::npos,
            "temp workspace manifest writer identity missing");
    Require(manifest.starts_with("SB_TEMP_WORKSPACE_MANIFEST_V3\n"),
            "temp workspace binary manifest version missing");
    const auto metadata_start = manifest.find('\n') + 1;
    const auto body_start = manifest.find('\n', metadata_start) + 1;
    Require(body_start > metadata_start && manifest.size() >= body_start + 40,
            "temp workspace binary manifest body missing");
    const auto read_u64 = [&manifest](std::size_t offset) {
      std::uint64_t value = 0;
      for (unsigned i = 0; i < 8; ++i)
        value |= std::uint64_t(static_cast<unsigned char>(manifest[offset + i])) << (8 * i);
      return value;
    };
    Require(read_u64(body_start) == 1 && read_u64(body_start + 16) == 3,
            "temp workspace binary record count/version mismatch");
    const auto expected = TempRequest(1).owner.temp_object_uuid;
    Require(manifest.compare(body_start + 24, expected.bytes.size(),
                             reinterpret_cast<const char*>(expected.bytes.data()),
                             expected.bytes.size()) == 0,
            "temp workspace record did not retain binary object identity");

    WriteFile(manifest_path.string() + ".tmp", "stale-temp");
    auto second = manager.AllocateSpillFile(TempRequest(2));
    Require(second.ok() && second.record.has_value(), "temp workspace second allocation failed");
    Require(!std::filesystem::exists(manifest_path.string() + ".tmp"),
            "temp workspace stale manifest temp survived persist");
  }

  {
    memory::TempWorkspaceLifecycleManager reloaded(policy);
    Require(reloaded.ActiveRecords().size() == 2,
            "temp workspace verified manifest did not reload active records");
  }

  const auto manifest_path = TempManifestPath(temp_root);
  std::string tampered = ReadFile(manifest_path);
  tampered += "record_v1\ttampered\n";
  WriteFile(manifest_path, tampered);
  memory::TempWorkspaceLifecycleManager refused(policy);
  Require(refused.ActiveRecords().empty(),
          "tampered temp workspace manifest restored active records");
}

}  // namespace

int main() try {
  scratchbird::tests::OwnedTempDirectory owned;
  const auto root = owned.path();
  TestFilespaceRegistryManifest(root);
  TestTempWorkspaceManifest(root);
  owned.Cleanup();
  return EXIT_SUCCESS;
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return EXIT_FAILURE;
}
