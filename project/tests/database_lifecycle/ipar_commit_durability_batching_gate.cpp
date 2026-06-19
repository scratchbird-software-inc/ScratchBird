// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "database_lifecycle.hpp"
#include "transaction/transaction_api.hpp"
#include "uuid.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

namespace {

namespace api = scratchbird::engine::internal_api;
namespace db = scratchbird::storage::database;
namespace platform = scratchbird::core::platform;
namespace uuid = scratchbird::core::uuid;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) { Fail(message); }
}

template <typename TResult>
void RequireOk(const TResult& result, std::string_view message) {
  if (!result.ok) {
    for (const auto& diagnostic : result.diagnostics) {
      std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
    }
    Fail(message);
  }
}

platform::u64 NowMillis() {
  return static_cast<platform::u64>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

platform::TypedUuid NewUuid(platform::UuidKind kind, platform::u64 salt) {
  const auto generated = uuid::GenerateEngineIdentityV7(kind, NowMillis() + salt);
  Require(generated.ok(), "IPAR-P3-05 UUID generation failed");
  return generated.value;
}

std::string NewUuidText(platform::UuidKind kind, platform::u64 salt) {
  return uuid::UuidToString(NewUuid(kind, salt).value);
}

struct Fixture {
  std::filesystem::path dir;
  std::filesystem::path database_path;
  std::string database_uuid;
  platform::u64 salt = 0;

  ~Fixture() {
    std::error_code ignored;
    if (!dir.empty()) { std::filesystem::remove_all(dir, ignored); }
  }
};

Fixture MakeFixture(std::string name, platform::u64 salt) {
  Fixture fixture;
  fixture.salt = NowMillis() + salt;
  fixture.dir = std::filesystem::temp_directory_path() /
                ("scratchbird_ipar_commit_batch_" + name + "_" +
                 std::to_string(fixture.salt) + "_" +
                 std::to_string(static_cast<long long>(getpid())));
  std::filesystem::create_directories(fixture.dir);
  fixture.database_path = fixture.dir / "commit_batch.sbdb";

  db::DatabaseCreateConfig create;
  create.path = fixture.database_path.string();
  create.database_uuid = NewUuid(platform::UuidKind::database, fixture.salt + 1);
  create.filespace_uuid = NewUuid(platform::UuidKind::filespace, fixture.salt + 2);
  create.creation_unix_epoch_millis = NowMillis() + fixture.salt + 3;
  create.require_resource_seed_pack = false;
  create.allow_minimal_resource_bootstrap = true;
  create.allow_overwrite = true;
  Require(db::CreateDatabaseFile(create).ok(),
          "IPAR-P3-05 database create failed");

  fixture.database_uuid = uuid::UuidToString(create.database_uuid.value);
  return fixture;
}

api::EngineRequestContext BaseContext(const Fixture& fixture,
                                      std::string request_id) {
  api::EngineRequestContext context;
  context.trust_mode = api::EngineTrustMode::server_isolated;
  context.request_id = std::move(request_id);
  context.database_path = fixture.database_path.string();
  context.database_uuid.canonical = fixture.database_uuid;
  context.principal_uuid.canonical =
      NewUuidText(platform::UuidKind::principal, fixture.salt + 100);
  context.session_uuid.canonical =
      NewUuidText(platform::UuidKind::object, fixture.salt + 101);
  context.security_context_present = true;
  context.identifier_profile_uuid = "sbsql_v3";
  context.language_context.language_tag = "en";
  context.language_context.default_language_tag = "en";
  context.catalog_generation_id = 1;
  context.security_epoch = 1;
  context.resource_epoch = 1;
  context.name_resolution_epoch = 1;
  return context;
}

api::EngineRequestContext Begin(const Fixture& fixture, std::string request_id) {
  api::EngineBeginTransactionRequest request;
  request.context = BaseContext(fixture, std::move(request_id));
  request.isolation_level = "read_committed";
  const auto begun = api::EngineBeginTransaction(request);
  RequireOk(begun, "IPAR-P3-05 begin transaction failed");

  auto context = request.context;
  context.local_transaction_id = begun.local_transaction_id;
  context.transaction_uuid = begun.transaction_uuid;
  context.snapshot_visible_through_local_transaction_id =
      begun.snapshot_visible_through_local_transaction_id;
  context.transaction_isolation_level = begun.isolation_level;
  return context;
}

api::EngineCommitTransactionResult Commit(
    const api::EngineRequestContext& context,
    std::vector<std::string> options) {
  api::EngineCommitTransactionRequest request;
  request.context = context;
  request.option_envelopes = std::move(options);
  return api::EngineCommitTransaction(request);
}

void Rollback(const api::EngineRequestContext& context) {
  api::EngineRollbackTransactionRequest request;
  request.context = context;
  RequireOk(api::EngineRollbackTransaction(request),
            "IPAR-P3-05 rollback failed");
}

bool HasEvidence(const std::vector<api::EngineEvidenceReference>& evidence,
                 std::string_view kind,
                 std::string_view id) {
  for (const auto& item : evidence) {
    if (item.evidence_kind == kind && item.evidence_id == id) {
      return true;
    }
  }
  return false;
}

bool HasEvidenceContaining(const std::vector<api::EngineEvidenceReference>& evidence,
                           std::string_view kind,
                           std::string_view needle) {
  for (const auto& item : evidence) {
    if (item.evidence_kind == kind &&
        item.evidence_id.find(std::string(needle)) != std::string::npos) {
      return true;
    }
  }
  return false;
}

bool HasDiagnostic(const api::EngineApiResult& result,
                   std::string_view needle) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.code.find(std::string(needle)) != std::string::npos ||
        diagnostic.detail.find(std::string(needle)) != std::string::npos ||
        diagnostic.message_key.find(std::string(needle)) != std::string::npos) {
      return true;
    }
  }
  return false;
}

std::vector<std::string> BatchOptions(const Fixture& fixture,
                                      std::string mode,
                                      std::string scratch_name) {
  return {
      "commit.durability_write_batching:" + std::move(mode),
      "commit.durability_write_batching.scratch:" +
          (fixture.dir / std::move(scratch_name)).string(),
      "commit.durability_write_batching.dirty_pages:6",
      "commit.durability_write_batching.extent_pages:6",
      "commit.durability_write_batching.generation:305",
      "commit.durability_write_batching.expected_generation:305"};
}

void ValidateAcceptedDurabilityBatching() {
  auto fixture = MakeFixture("accepted", 1000);
  auto context = Begin(fixture, "ipar-p305-accepted");
  auto committed = Commit(context, BatchOptions(fixture, "enabled", "accepted"));
  RequireOk(committed, "IPAR-P3-05 accepted commit failed");
  Require(HasEvidence(committed.evidence, "transaction_state", "committed"),
          "IPAR-P3-05 accepted commit missing committed state");
  Require(HasEvidence(committed.evidence,
                      "commit_durability_batching",
                      "accepted"),
          "IPAR-P3-05 accepted batching evidence missing");
  Require(HasEvidenceContaining(committed.evidence,
                                "commit_durability_batching_evidence",
                                "orh284.runtime_consumed=true"),
          "IPAR-P3-05 runtime batching evidence missing");
  Require(HasEvidenceContaining(committed.evidence,
                                "commit_durability_batching_evidence",
                                "orh284.write_batch_metadata.finality_authority=false"),
          "IPAR-P3-05 batching metadata finality boundary missing");
}

void ValidateOptionalFallbackStillCommits() {
  auto fixture = MakeFixture("fallback", 2000);
  auto context = Begin(fixture, "ipar-p305-fallback");
  auto options = BatchOptions(fixture, "enabled", "fallback");
  options.push_back("commit.durability_write_batching.resource_pressure:true");
  auto committed = Commit(context, std::move(options));
  RequireOk(committed, "IPAR-P3-08 fallback commit failed");
  Require(HasEvidence(committed.evidence, "transaction_state", "committed"),
          "IPAR-P3-08 fallback commit missing committed state");
  Require(HasEvidence(committed.evidence,
                      "commit_durability_batching",
                      "fallback"),
          "IPAR-P3-08 fallback evidence missing");
  Require(HasEvidence(committed.evidence,
                      "commit_durability_batching_diagnostic",
                      "ORH_WRITE_BATCHING_RESOURCE_PRESSURE_FALLBACK"),
          "IPAR-P3-08 fallback diagnostic missing");
  Require(HasEvidenceContaining(committed.evidence,
                                "commit_durability_batching_evidence",
                                "orh284.exact_fallback_used=true"),
          "IPAR-P3-08 exact fallback evidence missing");
}

void ValidateRequiredFailurePrecedesCommit() {
  auto fixture = MakeFixture("required_refusal", 3000);
  auto context = Begin(fixture, "ipar-p305-required-refusal");
  auto options = BatchOptions(fixture, "required", "required_refusal");
  options.push_back("commit.durability_write_batching.resource_pressure:true");
  auto committed = Commit(context, std::move(options));
  Require(!committed.ok, "IPAR-P3-05 required batching unexpectedly committed");
  Require(HasDiagnostic(committed, "ORH_WRITE_BATCHING_RESOURCE_PRESSURE_FALLBACK"),
          "IPAR-P3-05 required batching diagnostic missing");

  Rollback(context);
}

void ValidateUnsafeAuthorityFailsClosed() {
  auto fixture = MakeFixture("unsafe_authority", 4000);
  auto context = Begin(fixture, "ipar-p305-unsafe-authority");
  auto options = BatchOptions(fixture, "required", "unsafe_authority");
  options.push_back("commit.durability_write_batching.parser_authority:true");
  auto committed = Commit(context, std::move(options));
  Require(!committed.ok, "IPAR-P3-05 parser-authority batching committed");
  Require(HasDiagnostic(committed, "ORH_WRITE_BATCHING_UNSAFE_AUTHORITY"),
          "IPAR-P3-05 unsafe authority diagnostic missing");

  Rollback(context);
}

}  // namespace

int main() {
  ValidateAcceptedDurabilityBatching();
  ValidateOptionalFallbackStillCommits();
  ValidateRequiredFailurePrecedesCommit();
  ValidateUnsafeAuthorityFailsClosed();
  return EXIT_SUCCESS;
}
