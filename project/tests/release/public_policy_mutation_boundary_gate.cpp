// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "database_lifecycle.hpp"
#include "memory.hpp"
#include "public_release_authz_fixture.hpp"
#include "security/policy_api.hpp"
#include "uuid.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>

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

void ConfigureMemoryFixture() {
  auto policy = memory::DefaultLocalEngineMemoryPolicy();
  policy.policy_name = "public_policy_mutation_boundary_gate";
  policy.hard_limit_bytes = 16 * 1024 * 1024;
  policy.soft_limit_bytes = 16 * 1024 * 1024;
  policy.per_context_limit_bytes = 16 * 1024 * 1024;
  policy.page_buffer_pool_limit_bytes = 16 * 1024 * 1024;
  policy.track_allocations = true;
  policy.zero_memory_on_release = true;
  const auto configured =
      memory::ConfigureDefaultMemoryManagerForFixture(policy,
                                                      "public_policy_mutation_boundary_gate");
  Require(configured.ok(), "policy mutation boundary memory fixture should configure");
  Require(configured.fixture_mode, "policy mutation boundary memory must be fixture mode");
}

struct TempFixture {
  std::filesystem::path database_path;
  std::filesystem::path policy_pack_root;

  ~TempFixture() {
    std::error_code ignored;
    if (!database_path.empty()) {
      std::filesystem::remove(database_path, ignored);
      std::filesystem::remove(database_path.string() + ".sb.owner.lock", ignored);
      std::filesystem::remove(database_path.string() + ".sb.api_events", ignored);
      std::filesystem::remove(database_path.string() + ".sb.crud_events", ignored);
    }
    if (!policy_pack_root.empty()) {
      std::filesystem::remove_all(policy_pack_root, ignored);
    }
  }
};

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

void RequireSamePolicyImage(const db::PolicySeedPackCatalogImage& expected,
                            const db::PolicySeedPackCatalogImage& actual) {
  Require(actual.active, "opened database did not report durable policy catalog image");
  Require(actual.policy_pack_id == expected.policy_pack_id, "policy pack id changed after reopen");
  Require(actual.policy_pack_uuid == expected.policy_pack_uuid, "policy pack UUID changed after reopen");
  Require(actual.policy_pack_version == expected.policy_pack_version,
          "policy pack version changed after reopen");
  Require(actual.content_sha256 == expected.content_sha256,
          "policy pack content hash changed after post-create filesystem mutation");
  Require(actual.policy_profile_records == expected.policy_profile_records,
          "policy profile count changed after post-create filesystem mutation");
  Require(actual.local_password_only == expected.local_password_only,
          "local-password-only policy changed after reopen");
  Require(!actual.post_create_filesystem_authority,
          "opened policy image allowed post-create filesystem authority");
}

TempFixture CreateDatabaseThenCorruptFilesystemPack() {
  TempFixture fixture;
  fixture.database_path =
      UniquePath("sb_pcr131_policy_mutation_boundary").replace_extension(".sbdb");
  fixture.policy_pack_root = UniquePath("sb_pcr131_policy_pack_copy");
  std::filesystem::copy(SB_DEFAULT_POLICY_PACK_ROOT,
                        fixture.policy_pack_root,
                        std::filesystem::copy_options::recursive);

  const auto created = db::CreateDatabaseFile(
      CreateConfig(fixture.database_path, fixture.policy_pack_root, CurrentUnixMillis()));
  if (!created.ok()) {
    std::cerr << created.diagnostic.diagnostic_code << '\n';
  }
  Require(created.ok(), "CreateDatabaseFile failed for PCR-131 policy boundary");
  Require(created.state.policy_seed_catalog_present,
          "created database did not report policy seed catalog");

  {
    std::ofstream profiles(fixture.policy_pack_root / "policies/policy_profiles.json",
                           std::ios::trunc);
    profiles << "{\"schema_version\":1,\"policy_generation\":1,\"profiles\":[]}\n";
  }

  db::DatabaseOpenConfig open;
  open.path = fixture.database_path.string();
  open.read_only = true;
  const auto opened = db::OpenDatabaseFile(open);
  if (!opened.ok()) {
    std::cerr << opened.diagnostic.diagnostic_code << '\n';
  }
  Require(opened.ok(), "OpenDatabaseFile failed after post-create policy pack mutation");
  RequireSamePolicyImage(created.state.policy_seed_catalog, opened.state.policy_seed_catalog);
  return fixture;
}

api::EnginePolicyMutationRequest BaseMutationRequest(const std::filesystem::path& database_path) {
  api::EnginePolicyMutationRequest request;
  request.context.database_path = database_path.string();
  request.context.local_transaction_id = 42;
  request.context.security_context_present = true;
  request.context.security_epoch = 7;
  request.context.catalog_generation_id = 9;
  request.context.principal_uuid.canonical = "018f7a10-1280-7000-8000-000000000050";
  request.context.trace_tags.push_back("right:POLICY_ADMIN");
  scratchbird::tests::release::GrantMaterializedRight(
      &request.context, "POLICY_ADMIN");
  request.target_object.uuid.canonical = "018f7a10-1280-7000-8000-000000000409";
  request.target_object.object_kind = "policy";
  request.mutation_kind = "modify";
  request.policy_area = "diagnostics";
  request.policy_mode = "stable_redacted_diagnostics";
  request.canonical_policy_envelope = "SBLR_POLICY_PROFILE_V1:diagnostics";
  return request;
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

bool HasResultField(const api::EngineApiResult& result,
                    std::string_view key,
                    std::string_view value) {
  for (const auto& row : result.result_shape.rows) {
    for (const auto& field : row.fields) {
      if (field.first == key && field.second.encoded_value == value) {
        return true;
      }
    }
  }
  return false;
}

std::string ApiEventsText(const std::filesystem::path& database_path) {
  std::ifstream input(database_path.string() + ".sb.api_events", std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(input),
                     std::istreambuf_iterator<char>());
}

void RequirePolicyMutationCommandBoundary(const std::filesystem::path& database_path) {
  auto filesystem_pack = BaseMutationRequest(database_path);
  filesystem_pack.option_envelopes.push_back("policy_pack_root:/tmp/not-authority");
  const auto filesystem_refused = api::EngineMutatePolicy(filesystem_pack);
  Require(!filesystem_refused.ok, "filesystem policy pack mutation unexpectedly succeeded");
  Require(filesystem_refused.filesystem_pack_rejected,
          "filesystem policy pack refusal did not report rejected pack");

  auto missing_tx = BaseMutationRequest(database_path);
  missing_tx.context.local_transaction_id = 0;
  const auto missing_tx_refused = api::EngineMutatePolicy(missing_tx);
  Require(!missing_tx_refused.ok, "policy mutation without MGA transaction unexpectedly succeeded");

  auto missing_security = BaseMutationRequest(database_path);
  missing_security.context.security_context_present = false;
  const auto missing_security_refused = api::EngineMutatePolicy(missing_security);
  Require(!missing_security_refused.ok,
          "policy mutation without security context unexpectedly succeeded");

  auto missing_admin = BaseMutationRequest(database_path);
  missing_admin.context.trace_tags.clear();
  missing_admin.context.authorization_context = {};
  const auto missing_admin_refused = api::EngineMutatePolicy(missing_admin);
  Require(!missing_admin_refused.ok,
          "policy mutation without POLICY_ADMIN unexpectedly succeeded");

  auto read_only = BaseMutationRequest(database_path);
  read_only.context.read_only_mode = true;
  const auto read_only_refused = api::EngineMutatePolicy(read_only);
  Require(!read_only_refused.ok, "policy mutation in read-only context unexpectedly succeeded");

  const auto success = api::EngineMutatePolicy(BaseMutationRequest(database_path));
  Require(success.ok, "authorized policy mutation command failed");
  Require(success.mutation_performed, "policy mutation did not report mutation_performed");
  Require(success.mga_catalog_commit_required,
          "policy mutation did not require MGA catalog commit");
  Require(success.audit_evidence_recorded,
          "policy mutation did not record audit evidence");
  Require(success.generation_invalidated,
          "policy mutation did not invalidate policy generation");
  Require(success.new_policy_epoch > success.previous_policy_epoch,
          "policy mutation did not advance policy epoch evidence");
  Require(HasEvidence(success, "database_policy_command", "modify"),
          "policy mutation missing database command evidence");
  Require(HasEvidence(success, "mga_catalog_commit", "42"),
          "policy mutation missing MGA catalog commit evidence");
  Require(HasEvidence(success, "policy_audit_event", "recorded"),
          "policy mutation missing audit evidence");
  Require(HasEvidence(success, "filesystem_policy_pack_authority", "false_after_create"),
          "policy mutation did not reject filesystem authority after create");
  Require(HasResultField(success, "parser_sql_text_authority", "false"),
          "policy mutation result did not deny parser SQL text authority");
  Require(HasResultField(success, "database_command_authority", "true"),
          "policy mutation result did not prove database command authority");

  const std::string events = ApiEventsText(database_path);
  Require(events.find("SBSEC1\tEVIDENCE\t42\tsecurity.policy_mutation\tpolicy_mutation") !=
              std::string::npos,
          "policy mutation audit evidence was not appended before success");
  Require(events.find("SBAPI1\tRECORD\t42\tsecurity.policy_mutation") != std::string::npos,
          "policy mutation catalog event was not appended");
}

}  // namespace

int main() {
  ConfigureMemoryFixture();
  auto fixture = CreateDatabaseThenCorruptFilesystemPack();
  RequirePolicyMutationCommandBoundary(fixture.database_path);
  return EXIT_SUCCESS;
}
