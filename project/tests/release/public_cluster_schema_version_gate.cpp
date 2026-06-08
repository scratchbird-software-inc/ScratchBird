// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SPDX-License-Identifier: Apache-2.0
// Copyright (c) ScratchBird contributors
//
// PUBLIC_CLUSTER_SCHEMA_VERSION_GATE

#include "cluster_catalog_schema_versioning.hpp"
#include "cluster_schema_gating.hpp"
#include "database_format.hpp"
#include "database_lifecycle.hpp"
#include "memory.hpp"
#include "uuid.hpp"

#include <cstdlib>
#include <cstddef>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>

namespace {

namespace catalog = scratchbird::core::catalog;
namespace db = scratchbird::storage::database;
namespace disk = scratchbird::storage::disk;
namespace memory = scratchbird::core::memory;
namespace uuid = scratchbird::core::uuid;

using scratchbird::core::platform::TypedUuid;
using scratchbird::core::platform::UuidKind;
using scratchbird::core::platform::u64;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

TypedUuid MakeUuid(UuidKind kind, u64 offset) {
  const auto generated =
      uuid::GenerateEngineIdentityV7(kind, 1771500000000ull + offset);
  Require(generated.ok(), "cluster schema version UUID generation failed");
  return generated.value;
}

struct CleanupDir {
  std::filesystem::path root;
  ~CleanupDir() {
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
  }
};

void ConfigureMemoryFixture() {
  memory::AllocationPolicy policy;
  policy.policy_name = "public_cluster_schema_version_gate";
  policy.hard_limit_bytes = 64ull * 1024ull * 1024ull;
  policy.soft_limit_bytes = 48ull * 1024ull * 1024ull;
  policy.per_context_limit_bytes = 32ull * 1024ull * 1024ull;
  policy.page_buffer_pool_limit_bytes = 16ull * 1024ull * 1024ull;
  policy.track_allocations = true;
  policy.zero_memory_on_release = true;
  const auto configured = memory::ConfigureDefaultMemoryManagerForFixture(
      policy, "public_cluster_schema_version_gate");
  Require(configured.ok(), "memory fixture configuration failed");
  Require(configured.fixture_mode, "memory manager did not use fixture mode");
}

db::ClusterCatalogCompatibilityEvidence CurrentClusterEvidence() {
  db::ClusterCatalogCompatibilityEvidence evidence;
  evidence.cluster_structures_present = true;
  evidence.external_provider_available = true;
  for (const auto& profile :
       catalog::BuiltinClusterCatalogSchemaVersionProfiles()) {
    db::ClusterCatalogCompatibilityEvidenceEntry entry;
    entry.table_path = profile.table_path;
    entry.schema_version = profile.schema_version_current;
    entry.codec_version = profile.codec_version_current;
    entry.identity_proven = true;
    evidence.entries.push_back(std::move(entry));
  }
  return evidence;
}

void TestBuiltinSchemaProfiles() {
  const auto validated =
      catalog::ValidateBuiltinClusterCatalogSchemaVersionProfiles();
  Require(validated.ok, "built-in cluster schema version profiles failed");

  const std::size_t expected_count =
      catalog::BuiltinClusterCatalogTableManifests().size() +
      catalog::BuiltinClusterRoleProfileManifests().size() +
      catalog::BuiltinClusterCacheProjectionManifests().size();
  Require(catalog::BuiltinClusterCatalogSchemaVersionProfiles().size() ==
              expected_count,
          "cluster schema version profile count drifted");

  for (const auto& profile :
       catalog::BuiltinClusterCatalogSchemaVersionProfiles()) {
    Require(profile.external_provider_bound,
            "cluster schema profile was not external-provider-bound");
    Require(profile.fail_closed_without_external_provider,
            "cluster schema profile did not fail closed without provider");
    Require(!profile.local_runtime_execution_enabled,
            "cluster schema profile enabled local execution");
    Require(!profile.mutable_by_local_core,
            "cluster schema profile allowed local mutation");
    Require(!profile.migration_supported,
            "first release must not overclaim cluster migration support");

    catalog::ClusterCatalogCompatibilityRequest request;
    request.table_path = profile.table_path;
    request.schema_version = profile.schema_version_current;
    request.codec_version = profile.codec_version_current;
    request.external_provider_available = true;
    const auto current = catalog::EvaluateClusterCatalogCompatibility(request);
    Require(current.ok(), "current cluster schema profile was refused");
  }
}

void TestVersionRefusals() {
  const auto& profile =
      catalog::BuiltinClusterCatalogSchemaVersionProfiles().front();

  catalog::ClusterCatalogCompatibilityRequest request;
  request.table_path = profile.table_path;
  request.schema_version = profile.schema_version_current;
  request.codec_version = profile.codec_version_current;
  request.external_provider_available = false;
  const auto no_provider = catalog::EvaluateClusterCatalogCompatibility(request);
  Require(!no_provider.ok() &&
              no_provider.diagnostic.diagnostic_code ==
                  "SB-CLUSTER-CATALOG-SCHEMA-EXTERNAL-PROVIDER-REQUIRED",
          "cluster schema compatibility did not require external provider");

  request.external_provider_available = true;
  request.schema_version = profile.schema_version_max_supported + 1;
  const auto future_schema =
      catalog::EvaluateClusterCatalogCompatibility(request);
  Require(!future_schema.ok() &&
              future_schema.diagnostic.diagnostic_code ==
                  "SB-CLUSTER-CATALOG-SCHEMA-UNSUPPORTED-NEW",
          "future cluster schema version was accepted");

  request.schema_version = 0;
  const auto old_schema = catalog::EvaluateClusterCatalogCompatibility(request);
  Require(!old_schema.ok() &&
              old_schema.diagnostic.diagnostic_code ==
                  "SB-CLUSTER-CATALOG-SCHEMA-UNSUPPORTED-OLD",
          "old cluster schema version was accepted");

  request.schema_version = profile.schema_version_current;
  request.codec_version = profile.codec_version_max_supported + 1;
  const auto future_codec =
      catalog::EvaluateClusterCatalogCompatibility(request);
  Require(!future_codec.ok() &&
              future_codec.diagnostic.diagnostic_code ==
                  "SB-CLUSTER-CATALOG-CODEC-UNSUPPORTED",
          "future cluster codec version was accepted");

  request.codec_version = profile.codec_version_current;
  request.identity_proven = false;
  const auto ambiguous = catalog::EvaluateClusterCatalogCompatibility(request);
  Require(!ambiguous.ok() &&
              ambiguous.diagnostic.diagnostic_code ==
                  "SB-CLUSTER-CATALOG-SCHEMA-IDENTITY-AMBIGUOUS",
          "ambiguous cluster schema identity was accepted");
}

void TestMigrationPlanRefusals() {
  const auto& profile =
      catalog::BuiltinClusterCatalogSchemaVersionProfiles().front();

  catalog::ClusterCatalogMigrationPlanRequest request;
  request.table_path = profile.table_path;
  request.from_schema_version = profile.schema_version_current;
  request.to_schema_version = profile.schema_version_current;
  request.external_provider_available = true;
  Require(catalog::ValidateClusterCatalogMigrationPlan(request).ok(),
          "no-op current cluster schema migration was refused");

  request.from_schema_version = 0;
  const auto missing_plan =
      catalog::ValidateClusterCatalogMigrationPlan(request);
  Require(!missing_plan.ok() &&
              missing_plan.diagnostic.diagnostic_code ==
                  "SB-CLUSTER-CATALOG-MIGRATION-PLAN-MISSING",
          "missing cluster migration plan was accepted");

  request.migration_plan_id = "cluster_catalog_v0_to_v1";
  const auto unsupported =
      catalog::ValidateClusterCatalogMigrationPlan(request);
  Require(!unsupported.ok() &&
              unsupported.diagnostic.diagnostic_code ==
                  "SB-CLUSTER-CATALOG-MIGRATION-UNSUPPORTED",
          "unsupported cluster migration plan was accepted");
}

void TestDatabaseCompatibilityEvidence() {
  db::ClusterCatalogCompatibilityEvidence local_only;
  Require(db::CheckClusterCatalogCompatibilityEvidence(local_only).ok(),
          "local database without cluster catalog evidence was refused");

  auto evidence = CurrentClusterEvidence();
  Require(db::CheckClusterCatalogCompatibilityEvidence(evidence).ok(),
          "current cluster catalog compatibility evidence was refused");

  evidence.external_provider_available = false;
  const auto no_provider =
      db::CheckClusterCatalogCompatibilityEvidence(evidence);
  Require(!no_provider.ok() &&
              no_provider.diagnostic.diagnostic_code ==
                  "ENGINE.DBLC_CLUSTER_CATALOG_EXTERNAL_PROVIDER_REQUIRED",
          "database cluster compatibility did not require external provider");

  evidence = CurrentClusterEvidence();
  evidence.entries.front().schema_version += 1;
  const auto future_schema =
      db::CheckClusterCatalogCompatibilityEvidence(evidence);
  Require(!future_schema.ok() &&
              future_schema.diagnostic.diagnostic_code ==
                  "SB-CLUSTER-CATALOG-SCHEMA-UNSUPPORTED-NEW",
          "database accepted future cluster schema evidence");

  evidence = CurrentClusterEvidence();
  evidence.entries.front().codec_version += 1;
  const auto future_codec =
      db::CheckClusterCatalogCompatibilityEvidence(evidence);
  Require(!future_codec.ok() &&
              future_codec.diagnostic.diagnostic_code ==
                  "SB-CLUSTER-CATALOG-CODEC-UNSUPPORTED",
          "database accepted future cluster codec evidence");
}

void TestClusterOpenFailsClosedWithoutLocalExecution(
    const std::filesystem::path& root) {
  const auto path = root / "cluster_schema_version_open.sbdb";
  db::DatabaseCreateConfig create;
  create.path = path.string();
  create.database_uuid = MakeUuid(UuidKind::database, 1);
  create.filespace_uuid = MakeUuid(UuidKind::filespace, 2);
  create.page_size = 16384;
  create.creation_unix_epoch_millis = 1771500000000ull;
  create.feature_flags = disk::DatabaseFeatureFlag::cluster_structures_present;
  create.require_resource_seed_pack = false;
  create.allow_minimal_resource_bootstrap = true;
  create.allow_overwrite = true;

  const auto created = db::CreateDatabaseFile(create);
  Require(created.ok(), "cluster-flagged database fixture create failed");

  db::DatabaseOpenConfig no_cluster;
  no_cluster.path = path.string();
  const auto refused = db::OpenDatabaseFile(no_cluster);
  Require(!refused.ok() &&
              refused.diagnostic.diagnostic_code ==
                  "SB-DB-LIFECYCLE-CLUSTER-AUTHORITY-REQUIRED",
          "cluster catalog open did not fail closed without provider authority");

  db::DatabaseOpenConfig provider_claim;
  provider_claim.path = path.string();
  provider_claim.cluster_authority_available = true;
  const auto external_only = db::OpenDatabaseFile(provider_claim);
  Require(!external_only.ok() &&
              external_only.diagnostic.diagnostic_code ==
                  "SB-DB-LIFECYCLE-CLUSTER-MAPPING-UNAVAILABLE",
          "local cluster mapping executed instead of external-provider refusal");
}

}  // namespace

int main() {
  ConfigureMemoryFixture();
  CleanupDir cleanup{
      std::filesystem::temp_directory_path() /
      "scratchbird_public_cluster_schema_version_gate"};
  std::filesystem::create_directories(cleanup.root);

  TestBuiltinSchemaProfiles();
  TestVersionRefusals();
  TestMigrationPlanRefusals();
  TestDatabaseCompatibilityEvidence();
  TestClusterOpenFailsClosedWithoutLocalExecution(cleanup.root);
  return EXIT_SUCCESS;
}
