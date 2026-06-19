// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "database_lifecycle.hpp"
#include "memory.hpp"
#include "security/policy_api.hpp"
#include "uuid.hpp"

#include <openssl/sha.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace api = scratchbird::engine::internal_api;
namespace db = scratchbird::storage::database;
namespace memory = scratchbird::core::memory;
namespace uuid = scratchbird::core::uuid;
using scratchbird::core::platform::UuidKind;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

std::uint64_t CurrentUnixMillis() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

std::filesystem::path UniquePath(std::string_view stem) {
  return std::filesystem::temp_directory_path() /
         (std::string(stem) + "_" + std::to_string(CurrentUnixMillis()));
}

std::string ReadText(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  Require(static_cast<bool>(input), "failed to read text fixture");
  return std::string(std::istreambuf_iterator<char>(input),
                     std::istreambuf_iterator<char>());
}

void WriteText(const std::filesystem::path& path, const std::string& text) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  Require(static_cast<bool>(output), "failed to open text fixture for write");
  output << text;
  Require(static_cast<bool>(output), "failed to write text fixture");
}

void ReplaceOnce(std::string* text,
                 const std::string& needle,
                 const std::string& replacement) {
  const auto pos = text->find(needle);
  Require(pos != std::string::npos, "fixture replacement needle missing");
  text->replace(pos, needle.size(), replacement);
}

std::string Sha256Hex(const std::string& payload) {
  unsigned char digest[SHA256_DIGEST_LENGTH];
  SHA256(reinterpret_cast<const unsigned char*>(payload.data()), payload.size(), digest);
  std::ostringstream stream;
  stream << std::hex << std::setfill('0');
  for (unsigned char byte : digest) {
    stream << std::setw(2) << static_cast<unsigned int>(byte);
  }
  return stream.str();
}

std::string Sha256File(const std::filesystem::path& path) {
  return Sha256Hex(ReadText(path));
}

void ReplaceManifestSha(std::string* manifest,
                        const std::string& rel_path,
                        const std::string& digest) {
  const std::string path_marker = "\"path\": \"" + rel_path + "\"";
  const auto path_pos = manifest->find(path_marker);
  Require(path_pos != std::string::npos, "manifest content path missing");
  const std::string sha_marker = "\"sha256\": \"";
  const auto sha_pos = manifest->find(sha_marker, path_pos);
  Require(sha_pos != std::string::npos, "manifest sha marker missing");
  const auto value_begin = sha_pos + sha_marker.size();
  const auto value_end = manifest->find('"', value_begin);
  Require(value_end != std::string::npos, "manifest sha value unterminated");
  manifest->replace(value_begin, value_end - value_begin, digest);
}

void ReplaceManifestContentSha(std::string* manifest, const std::string& digest) {
  const std::string marker = "\"content_sha256\": \"";
  const auto pos = manifest->find(marker);
  Require(pos != std::string::npos, "manifest aggregate sha marker missing");
  const auto value_begin = pos + marker.size();
  const auto value_end = manifest->find('"', value_begin);
  Require(value_end != std::string::npos, "manifest aggregate sha value unterminated");
  manifest->replace(value_begin, value_end - value_begin, digest);
}

void RehashPolicyPackManifest(const std::filesystem::path& pack_root) {
  const std::vector<std::string> rel_paths = {
      "policies/security_providers.json",
      "policies/roles.json",
      "policies/groups.json",
      "policies/grants.json",
      "policies/policy_profiles.json",
      "policies/server_memory_cache_policy.json",
      "policies/default_policy_catalog.json",
      "catalog_materialization.json",
  };
  std::string manifest = ReadText(pack_root / "POLICY_PACK_MANIFEST.json");
  std::string aggregate_payload;
  for (const auto& rel_path : rel_paths) {
    const std::string digest = Sha256File(pack_root / rel_path);
    ReplaceManifestSha(&manifest, rel_path, digest);
    aggregate_payload += rel_path;
    aggregate_payload.push_back('\0');
    aggregate_payload += digest;
    aggregate_payload.push_back('\n');
  }
  ReplaceManifestContentSha(&manifest, Sha256Hex(aggregate_payload));
  WriteText(pack_root / "POLICY_PACK_MANIFEST.json", manifest);
}

void ConfigureMemoryFixture() {
  auto policy = memory::DefaultLocalEngineMemoryPolicy();
  policy.policy_name = "public_custom_policy_pack_gate";
  policy.hard_limit_bytes = 16 * 1024 * 1024;
  policy.soft_limit_bytes = 16 * 1024 * 1024;
  policy.per_context_limit_bytes = 16 * 1024 * 1024;
  policy.page_buffer_pool_limit_bytes = 16 * 1024 * 1024;
  policy.track_allocations = true;
  policy.zero_memory_on_release = true;
  const auto configured =
      memory::ConfigureDefaultMemoryManagerForFixture(policy,
                                                      "public_custom_policy_pack_gate");
  Require(configured.ok(), "custom policy pack memory fixture should configure");
  Require(configured.fixture_mode, "custom policy pack memory must be fixture mode");
}

struct TempRoot {
  std::filesystem::path root;
  ~TempRoot() {
    std::error_code ignored;
    if (!root.empty()) {
      std::filesystem::remove_all(root, ignored);
    }
  }
};

std::filesystem::path CopyPack(const std::filesystem::path& root,
                               std::string_view name) {
  const auto pack_root = root / std::string(name);
  std::filesystem::copy(SB_DEFAULT_POLICY_PACK_ROOT,
                        pack_root,
                        std::filesystem::copy_options::recursive);
  return pack_root;
}

db::DatabaseCreateConfig CreateConfig(const std::filesystem::path& database_path,
                                      const std::filesystem::path& policy_pack_root,
                                      std::uint64_t now) {
  const auto database_uuid = uuid::GenerateEngineIdentityV7(UuidKind::database, now);
  const auto filespace_uuid = uuid::GenerateEngineIdentityV7(UuidKind::filespace, now + 1);
  Require(database_uuid.ok() && filespace_uuid.ok(), "test UUID generation failed");

  db::DatabaseCreateConfig create;
  create.path = database_path.string();
  create.database_uuid = database_uuid.value;
  create.filespace_uuid = filespace_uuid.value;
  create.creation_unix_epoch_millis = now;
  create.require_resource_seed_pack = false;
  create.allow_minimal_resource_bootstrap = true;
  create.policy_seed_pack_root = policy_pack_root.string();
  create.require_policy_seed_pack = true;
  create.allow_overwrite = true;
  return create;
}

void ExpectCreateDiagnostic(const std::filesystem::path& database_path,
                            const std::filesystem::path& policy_pack_root,
                            std::string_view expected) {
  const auto created = db::CreateDatabaseFile(
      CreateConfig(database_path, policy_pack_root, CurrentUnixMillis()));
  if (created.ok()) {
    Fail("policy pack failure case unexpectedly created database");
  }
  Require(created.diagnostic.diagnostic_code == expected,
          "policy pack failure case returned unexpected diagnostic");
}

void ProveCustomPackCreateAndReopen(const std::filesystem::path& root) {
  const auto pack_root = CopyPack(root, "custom-pack");
  std::string manifest = ReadText(pack_root / "POLICY_PACK_MANIFEST.json");
  ReplaceOnce(&manifest,
              "\"policy_pack_id\": \"default-local-password\"",
              "\"policy_pack_id\": \"custom-local-password\"");
  ReplaceOnce(&manifest,
              "\"policy_pack_uuid\": \"018f7a10-1280-7000-8000-000000000001\"",
              "\"policy_pack_uuid\": \"018f7a10-1280-7000-8000-000000000002\"");
  ReplaceOnce(&manifest,
              "\"policy_pack_version\": \"1.0.0\"",
              "\"policy_pack_version\": \"1.0.1\"");
  WriteText(pack_root / "POLICY_PACK_MANIFEST.json", manifest);

  const auto database_path = root / "custom_policy_pack.sbdb";
  const auto created = db::CreateDatabaseFile(
      CreateConfig(database_path, pack_root, CurrentUnixMillis()));
  if (!created.ok()) {
    std::cerr << created.diagnostic.diagnostic_code << '\n';
  }
  Require(created.ok(), "custom policy pack create failed");
  Require(created.state.policy_seed_catalog.policy_pack_id == "custom-local-password",
          "custom policy pack id was not materialized");
  Require(created.state.policy_seed_catalog.policy_pack_uuid ==
              "018f7a10-1280-7000-8000-000000000002",
          "custom policy pack UUID was not materialized");
  Require(created.state.policy_seed_catalog.policy_pack_version == "1.0.1",
          "custom policy pack version was not materialized");

  db::DatabaseOpenConfig open;
  open.path = database_path.string();
  open.read_only = true;
  const auto opened = db::OpenDatabaseFile(open);
  if (!opened.ok()) {
    std::cerr << opened.diagnostic.diagnostic_code << '\n';
  }
  Require(opened.ok(), "custom policy pack database did not reopen");
  Require(opened.state.policy_seed_catalog.policy_pack_id == "custom-local-password",
          "custom policy pack id was not durable after reopen");
}

void ProveFailClosedDiagnostics(const std::filesystem::path& root) {
  std::filesystem::create_directories(root / "missing-pack");
  ExpectCreateDiagnostic(root / "missing.sbdb",
                         root / "missing-pack",
                         "SB-POLICY-PACK-MANIFEST-MISSING");

  const auto malformed = CopyPack(root, "malformed-pack");
  WriteText(malformed / "POLICY_PACK_MANIFEST.json", "{\n");
  ExpectCreateDiagnostic(root / "malformed.sbdb",
                         malformed,
                         "SB-POLICY-PACK-SCHEMA-UNSUPPORTED");

  const auto unsigned_pack = CopyPack(root, "unsigned-pack");
  {
    std::string manifest = ReadText(unsigned_pack / "POLICY_PACK_MANIFEST.json");
    ReplaceOnce(&manifest, "signature-ready-unsigned", "unsigned");
    WriteText(unsigned_pack / "POLICY_PACK_MANIFEST.json", manifest);
  }
  ExpectCreateDiagnostic(root / "unsigned.sbdb",
                         unsigned_pack,
                         "SB-POLICY-PACK-MANIFEST-FIELD-INVALID");

  const auto incompatible = CopyPack(root, "incompatible-pack");
  {
    std::string manifest = ReadText(incompatible / "POLICY_PACK_MANIFEST.json");
    ReplaceOnce(&manifest, "\"schema_version\": 1", "\"schema_version\": 2");
    WriteText(incompatible / "POLICY_PACK_MANIFEST.json", manifest);
  }
  ExpectCreateDiagnostic(root / "incompatible.sbdb",
                         incompatible,
                         "SB-POLICY-PACK-SCHEMA-UNSUPPORTED");

  const auto private_provenance = CopyPack(root, "private-provenance-pack");
  {
    std::string manifest = ReadText(private_provenance / "POLICY_PACK_MANIFEST.json");
    ReplaceOnce(&manifest, "\"private_inputs_required\": false",
                "\"private_inputs_required\": true");
    WriteText(private_provenance / "POLICY_PACK_MANIFEST.json", manifest);
  }
  ExpectCreateDiagnostic(root / "private_provenance.sbdb",
                         private_provenance,
                         "SB-POLICY-PACK-PROVENANCE-INVALID");

  const auto private_path = CopyPack(root, "private-path-pack");
  {
    std::string manifest = ReadText(private_path / "POLICY_PACK_MANIFEST.json");
    ReplaceOnce(&manifest, "\"path\": \"policies/roles.json\"",
                "\"path\": \"../private/roles.json\"");
    WriteText(private_path / "POLICY_PACK_MANIFEST.json", manifest);
  }
  ExpectCreateDiagnostic(root / "private_path.sbdb",
                         private_path,
                         "SB-POLICY-PACK-CONTENT-MANIFEST-INVALID");

  const auto unknown_policy = CopyPack(root, "unknown-policy-pack");
  {
    std::string profiles = ReadText(unknown_policy / "policies/policy_profiles.json");
    ReplaceOnce(&profiles, "\"area\": \"diagnostics\"",
                "\"area\": \"unknown_policy_area\"");
    WriteText(unknown_policy / "policies/policy_profiles.json", profiles);
    RehashPolicyPackManifest(unknown_policy);
  }
  ExpectCreateDiagnostic(root / "unknown_policy.sbdb",
                         unknown_policy,
                         "SB-POLICY-PACK-PROFILES-UNKNOWN");

  const auto unknown_default_policy = CopyPack(root, "unknown-default-policy-pack");
  {
    std::string defaults = ReadText(unknown_default_policy / "policies/default_policy_catalog.json");
    ReplaceOnce(&defaults,
                "\"policy_key\": \"diagnostics.message_vector\"",
                "\"policy_key\": \"diagnostics.unknown_policy\"");
    WriteText(unknown_default_policy / "policies/default_policy_catalog.json", defaults);
    RehashPolicyPackManifest(unknown_default_policy);
  }
  ExpectCreateDiagnostic(root / "unknown_default_policy.sbdb",
                         unknown_default_policy,
                         "SB-POLICY-PACK-DEFAULT-POLICIES-UNKNOWN");
}

void ProvePostCreatePackMutationStillRefuses(const std::filesystem::path& database_path) {
  api::EnginePolicyMutationRequest request;
  request.context.database_path = database_path.string();
  request.context.local_transaction_id = 99;
  request.context.security_context_present = true;
  request.context.security_epoch = 7;
  request.context.catalog_generation_id = 9;
  request.context.trace_tags.push_back("right:POLICY_ADMIN");
  request.option_envelopes.push_back("filesystem_policy_pack:/tmp/not-authority");
  const auto result = api::EngineMutatePolicy(request);
  Require(!result.ok, "post-create filesystem policy pack mutation unexpectedly succeeded");
  Require(result.filesystem_pack_rejected,
          "post-create filesystem policy pack mutation was not explicitly rejected");
}

}  // namespace

int main() {
  ConfigureMemoryFixture();
  TempRoot temp{UniquePath("sb_pcr132_custom_policy_pack")};
  std::filesystem::create_directories(temp.root);
  ProveCustomPackCreateAndReopen(temp.root);
  ProveFailClosedDiagnostics(temp.root);
  ProvePostCreatePackMutationStillRefuses(temp.root / "custom_policy_pack.sbdb");
  return EXIT_SUCCESS;
}
