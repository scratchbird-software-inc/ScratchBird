// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "filespace_lifecycle.hpp"
#include "temp_workspace_lifecycle.hpp"
#include "uuid.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>

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
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
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

std::filesystem::path TempRoot() {
  std::string scope = std::filesystem::current_path().filename().string();
  if (scope.empty()) {
    scope = "default";
  }
#ifdef _WIN32
  const auto pid = static_cast<unsigned long long>(::GetCurrentProcessId());
#else
  const auto pid = static_cast<unsigned long long>(::getpid());
#endif
  auto root = std::filesystem::temp_directory_path() /
              ("scratchbird_engine_listener_manifest_" + scope + "_" +
               std::to_string(pid));
  std::error_code ignored;
  std::filesystem::remove_all(root, ignored);
  std::filesystem::create_directories(root);
  return root;
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

memory::TempWorkspaceAllocationRequest TempRequest(std::string suffix) {
  memory::TempWorkspaceAllocationRequest request;
  request.owner.temp_object_uuid = "temp-" + suffix;
  request.owner.database_id = "database-a";
  request.owner.engine_id = "engine-a";
  request.owner.session_id = "session-a";
  request.owner.transaction_id = "txn-a";
  request.owner.statement_id = "stmt-a";
  request.owner.operation_id = "op-a";
  request.owner.policy_generation = 7;
  request.owner.security_generation = 9;
  request.owner.resource_budget_reference = "budget-a";
  request.bytes = 128;
  request.purpose = "ELER-013 durable temp workspace manifest proof";
  return request;
}

std::filesystem::path TempManifestPath(const std::filesystem::path& root) {
  return root / ".scratchbird_temp_workspace_manifest.v2";
}

void TestTempWorkspaceManifest(const std::filesystem::path& root) {
  const auto temp_root = root / "temp-workspace";
  const auto policy = TempPolicy(temp_root);
  {
    memory::TempWorkspaceLifecycleManager manager(policy);
    auto first = manager.AllocateSpillFile(TempRequest("first"));
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
    Require(manifest.find("record_v1") != std::string::npos,
            "temp workspace manifest record missing");

    WriteFile(manifest_path.string() + ".tmp", "stale-temp");
    auto second = manager.AllocateSpillFile(TempRequest("second"));
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

int main() {
  const auto root = TempRoot();
  TestFilespaceRegistryManifest(root);
  TestTempWorkspaceManifest(root);
  std::error_code ignored;
  std::filesystem::remove_all(root, ignored);
  return EXIT_SUCCESS;
}
