// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "database_lifecycle.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <set>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

namespace {

namespace db = scratchbird::storage::database;
namespace uuid = scratchbird::core::uuid;
using scratchbird::core::platform::TypedUuid;
using scratchbird::core::platform::UuidKind;
using scratchbird::core::platform::u32;

constexpr std::string_view kGateSearchKey = "DPC_PERSISTED_FORMAT_GATE";
constexpr std::string_view kFixtureSearchKey = "DPC_PERSISTED_FORMAT_FIXTURE";

struct FormatVersion {
  u32 major = 0;
  u32 minor = 0;
};

struct PersistedSurfaceContract {
  std::string surface_key;
  std::string feature_map_key;
  std::string artifact_kind;
  FormatVersion min_supported;
  FormatVersion current;
  FormatVersion max_supported;
  std::string relation_key;
  std::string index_key;
  std::uint64_t generation = 0;
  std::string recovery_classification;
  std::string rebuild_policy;
  std::string backup_restore_policy;
  std::string management_surface;
  std::string support_bundle_field;
  std::string repair_plan_id;
  std::vector<std::string_view> restricted_open_actions;
  bool no_hardcoded_catalog_uuid_dependency = false;
  bool live_runtime_recovery_claim = false;
};

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) Fail(message);
}

bool Contains(std::string_view haystack, std::string_view needle) {
  return haystack.find(needle) != std::string_view::npos;
}

bool StartsWith(std::string_view value, std::string_view prefix) {
  return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
}

bool LooksLikeUuid(std::string_view value) {
  if (value.size() != 36) return false;
  for (std::size_t index : {8u, 13u, 18u, 23u}) {
    if (value[index] != '-') return false;
  }
  return true;
}

std::uint64_t CurrentUnixMillis() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

std::filesystem::path MakeTempDir() {
  std::string tmpl = "/tmp/sb_dpc009_persisted_format.XXXXXX";
  std::vector<char> writable(tmpl.begin(), tmpl.end());
  writable.push_back('\0');
  char* made = ::mkdtemp(writable.data());
  Require(made != nullptr, "DPC-009 mkdtemp failed");
  return std::filesystem::path(made);
}

std::string UuidText(const TypedUuid& value) {
  return uuid::UuidToString(value.value);
}

db::DatabaseArtifactVersionCompatibilityRequest VersionRequest(
    const PersistedSurfaceContract& surface,
    FormatVersion format) {
  db::DatabaseArtifactVersionCompatibilityRequest request;
  request.artifact_kind = surface.artifact_kind;
  request.format_major = format.major;
  request.format_minor = format.minor;
  request.min_supported_major = surface.min_supported.major;
  request.min_supported_minor = surface.min_supported.minor;
  request.current_major = surface.current.major;
  request.current_minor = surface.current.minor;
  request.max_supported_major = surface.max_supported.major;
  request.max_supported_minor = surface.max_supported.minor;
  return request;
}

std::string MigrationPlanId(const PersistedSurfaceContract& surface,
                            FormatVersion from) {
  return surface.artifact_kind + "_v" + std::to_string(from.major) + "_" +
         std::to_string(from.minor) + "_to_v" +
         std::to_string(surface.current.major) + "_" +
         std::to_string(surface.current.minor) + "_explicit_plan_v1";
}

void RequireCompatibilityCode(const db::DatabaseArtifactCompatibilityResult& result,
                              db::DatabaseOpenCompatibilityClass expected_class,
                              std::string_view expected_code,
                              std::string_view message) {
  if (result.compatibility_class != expected_class ||
      result.diagnostic.diagnostic_code != expected_code) {
    std::cerr << "expected_class="
              << db::DatabaseOpenCompatibilityClassName(expected_class)
              << " actual_class="
              << db::DatabaseOpenCompatibilityClassName(result.compatibility_class)
              << " expected_code=" << expected_code
              << " actual_code=" << result.diagnostic.diagnostic_code << '\n';
  }
  Require(!result.ok(), message);
  Require(result.compatibility_class == expected_class, message);
  Require(result.diagnostic.diagnostic_code == expected_code, message);
}

const std::vector<PersistedSurfaceContract>& PersistedFormatContracts() {
  static const std::vector<PersistedSurfaceContract> contracts = {
      {"page_extent_summaries",
       "optimizer.persisted.page_extent_summaries",
       "dpc_page_extent_summary",
       {1, 0},
       {2, 0},
       {3, 0},
       "relation.logical.page_extent_summary_source",
       "index.logical.page_extent_summary_access_path",
       1001,
       "mga_rebuild_from_authoritative_pages",
       "rebuild_from_base_pages_after_restricted_validation",
       "rebuild_transient",
       "sys.management.optimization_persisted_features",
       "persisted_feature_map",
       "dpc_persisted_format.page_extent_summaries.repair_v1",
       {"inspect", "validate", "repair", "rebuild", "backup", "export"},
       true,
       false},
      {"secondary_index_delta_ledgers",
       "optimizer.persisted.secondary_index_delta_ledgers",
       "dpc_secondary_index_delta_ledger",
       {1, 0},
       {2, 0},
       {3, 0},
       "relation.logical.indexed_table",
       "index.logical.secondary_delta_ledger",
       1002,
       "mga_preserve_committed_delta_classify_unmerged",
       "replay_committed_delta_or_rebuild_overlay_after_mga_classification",
       "preserve_committed",
       "sys.management.optimization_persisted_features",
       "persisted_feature_map",
       "dpc_persisted_format.secondary_index_delta_ledgers.repair_v1",
       {"inspect", "validate", "repair", "rebuild", "backup", "export"},
       true,
       false},
      {"deferred_index_merge_state",
       "optimizer.persisted.deferred_index_merge_state",
       "dpc_deferred_index_merge_state",
       {1, 0},
       {2, 0},
       {3, 0},
       "relation.logical.indexed_table",
       "index.logical.deferred_merge_cursor",
       1003,
       "mga_rebuild_merge_cursor_from_committed_delta",
       "discard_merge_cursor_and_rebuild_from_committed_ledgers",
       "rebuild_transient",
       "sys.management.optimization_persisted_features",
       "persisted_feature_map",
       "dpc_persisted_format.deferred_index_merge_state.repair_v1",
       {"inspect", "validate", "repair", "rebuild", "backup", "export"},
       true,
       false},
      {"cleanup_horizon_markers",
       "optimizer.persisted.cleanup_horizon_markers",
       "dpc_cleanup_horizon_marker",
       {1, 0},
       {2, 0},
       {3, 0},
       "relation.logical.mga_version_family",
       "index.logical.cleanup_horizon_marker",
       1004,
       "mga_recompute_from_transaction_inventory_horizons",
       "recompute_from_oit_oat_ost_and_block_regression",
       "rebuild_transient",
       "sys.management.optimization_persisted_features",
       "persisted_feature_map",
       "dpc_persisted_format.cleanup_horizon_markers.repair_v1",
       {"inspect", "validate", "repair", "rebuild", "backup", "export"},
       true,
       false},
      {"shadow_index_build_state",
       "optimizer.persisted.shadow_index_build_state",
       "dpc_shadow_index_build_state",
       {1, 0},
       {2, 0},
       {3, 0},
       "relation.logical.shadow_build_source",
       "index.logical.shadow_build_target",
       1005,
       "mga_restrict_until_shadow_generation_validated",
       "discard_incomplete_shadow_generation_or_resume_validated_phase",
       "rebuild_transient",
       "sys.management.optimization_persisted_features",
       "persisted_feature_map",
       "dpc_persisted_format.shadow_index_build_state.repair_v1",
       {"inspect", "validate", "repair", "rebuild", "backup", "export"},
       true,
       false},
      {"search_inverted_segments",
       "optimizer.persisted.search_inverted_segments",
       "dpc_search_inverted_segment",
       {1, 0},
       {2, 0},
       {3, 0},
       "relation.logical.search_source",
       "index.logical.inverted_segment_generation",
       1006,
       "mga_preserve_sealed_segment_rebuild_pending_segment",
       "preserve_sealed_segments_and_rebuild_unsealed_segments",
       "preserve_committed",
       "sys.management.optimization_persisted_features",
       "persisted_feature_map",
       "dpc_persisted_format.search_inverted_segments.repair_v1",
       {"inspect", "validate", "repair", "rebuild", "backup", "export"},
       true,
       false},
      {"vector_generations",
       "optimizer.persisted.vector_generations",
       "dpc_vector_generation",
       {1, 0},
       {2, 0},
       {3, 0},
       "relation.logical.vector_source",
       "index.logical.vector_generation",
       1007,
       "mga_preserve_sealed_vector_generation_rebuild_pending_generation",
       "preserve_sealed_generation_and_rebuild_unsealed_generation",
       "preserve_committed",
       "sys.management.optimization_persisted_features",
       "persisted_feature_map",
       "dpc_persisted_format.vector_generations.repair_v1",
       {"inspect", "validate", "repair", "rebuild", "backup", "export"},
       true,
       false},
      {"optimization_management_metadata",
       "optimizer.persisted.management_metadata",
       "dpc_optimization_management_metadata",
       {1, 0},
       {2, 0},
       {3, 0},
       "relation.logical.sys_management_optimization_features",
       "index.logical.sys_management_feature_map_view",
       1008,
       "mga_rebuild_management_metadata_from_catalog_and_feature_map",
       "rebuild_management_projection_from_feature_map_and_repair_evidence",
       "preserve_committed",
       "sys.management.optimization_persisted_features",
       "persisted_feature_map",
       "dpc_persisted_format.optimization_management_metadata.repair_v1",
       {"inspect", "validate", "repair", "rebuild", "backup", "export"},
       true,
       false},
  };
  return contracts;
}

void ValidateVersionContracts(const PersistedSurfaceContract& surface) {
  Require(surface.min_supported.major <= surface.current.major,
          "DPC-009 min supported format is after current");
  Require(surface.current.major <= surface.max_supported.major,
          "DPC-009 current format is after max supported");

  const auto current =
      db::ClassifyDatabaseArtifactVersionCompatibility(VersionRequest(surface, surface.current));
  Require(current.ok() &&
              current.compatibility_class == db::DatabaseOpenCompatibilityClass::current,
          "DPC-009 current persisted artifact format was not accepted");

  const FormatVersion old_format{surface.current.major - 1, 0};
  auto old_without_plan = VersionRequest(surface, old_format);
  RequireCompatibilityCode(
      db::ClassifyDatabaseArtifactVersionCompatibility(old_without_plan),
      db::DatabaseOpenCompatibilityClass::migration_required_without_plan_refused,
      "ENGINE.DBLC_MIGRATION_REQUIRED_WITHOUT_PLAN",
      "DPC-009 old persisted artifact did not require explicit upgrade plan");

  auto old_with_plan = old_without_plan;
  old_with_plan.migration_plan_id = MigrationPlanId(surface, old_format);
  const auto old_plan =
      db::ClassifyDatabaseArtifactVersionCompatibility(old_with_plan);
  Require(old_plan.ok() &&
              old_plan.compatibility_class ==
                  db::DatabaseOpenCompatibilityClass::supported_migration &&
              old_plan.migration_required,
          "DPC-009 old persisted artifact migration plan was not accepted");

  const FormatVersion unsupported_old{surface.min_supported.major - 1, 0};
  RequireCompatibilityCode(
      db::ClassifyDatabaseArtifactVersionCompatibility(
          VersionRequest(surface, unsupported_old)),
      db::DatabaseOpenCompatibilityClass::unsupported_old,
      "ENGINE.DBLC_MIGRATION_UNSUPPORTED_OLD_ARTIFACT",
      "DPC-009 unsupported-old persisted artifact was not refused");

  const FormatVersion future_format{surface.current.major + 1, 0};
  RequireCompatibilityCode(
      db::ClassifyDatabaseArtifactVersionCompatibility(
          VersionRequest(surface, future_format)),
      db::DatabaseOpenCompatibilityClass::unsupported_new,
      "ENGINE.DBLC_MIGRATION_UNSUPPORTED_NEW_ARTIFACT",
      "DPC-009 future persisted artifact inside max range was not refused");

  const FormatVersion too_new{surface.max_supported.major + 1, 0};
  RequireCompatibilityCode(
      db::ClassifyDatabaseArtifactVersionCompatibility(
          VersionRequest(surface, too_new)),
      db::DatabaseOpenCompatibilityClass::newer_than_supported_refused,
      "ENGINE.DBLC_FORMAT_NEWER_THAN_SUPPORTED",
      "DPC-009 newer-than-supported persisted artifact was not refused");

  auto ambiguous_identity = VersionRequest(surface, surface.current);
  ambiguous_identity.identity_proven = false;
  RequireCompatibilityCode(
      db::ClassifyDatabaseArtifactVersionCompatibility(ambiguous_identity),
      db::DatabaseOpenCompatibilityClass::ambiguous_identity_refused,
      "ENGINE.DBLC_MIGRATION_AMBIGUOUS_IDENTITY_REFUSED",
      "DPC-009 ambiguous persisted artifact identity was not refused");
}

void ValidateFeatureMapAndRepairContracts(const PersistedSurfaceContract& surface,
                                          std::set<std::string>* feature_keys,
                                          std::set<std::string>* artifact_kinds) {
  Require(!surface.surface_key.empty(), "DPC-009 surface key missing");
  Require(!surface.feature_map_key.empty(), "DPC-009 feature-map key missing");
  Require(!surface.artifact_kind.empty(), "DPC-009 artifact kind missing");
  Require(feature_keys->insert(surface.feature_map_key).second,
          "DPC-009 duplicate persisted feature-map key");
  Require(artifact_kinds->insert(surface.artifact_kind).second,
          "DPC-009 duplicate persisted artifact kind");

  Require(surface.generation != 0, "DPC-009 repair diagnostic generation missing");
  Require(!surface.relation_key.empty(), "DPC-009 repair diagnostic relation missing");
  Require(!surface.index_key.empty(), "DPC-009 repair diagnostic index missing");
  Require(!surface.recovery_classification.empty(),
          "DPC-009 repair diagnostic recovery classification missing");
  Require(StartsWith(surface.recovery_classification, "mga_"),
          "DPC-009 repair diagnostic must classify through MGA-owned recovery");
  Require(!LooksLikeUuid(surface.relation_key) && !LooksLikeUuid(surface.index_key) &&
              !LooksLikeUuid(surface.feature_map_key),
          "DPC-009 contract used hard-coded catalog UUID-looking keys");
  Require(surface.no_hardcoded_catalog_uuid_dependency,
          "DPC-009 no-hard-coded-catalog-UUID proof missing");

  Require(StartsWith(surface.repair_plan_id, "dpc_persisted_format."),
          "DPC-009 repair plan id missing DPC persisted-format namespace");
  Require(Contains(surface.repair_plan_id, surface.surface_key),
          "DPC-009 repair plan id does not bind the surface key");
  Require(Contains(surface.rebuild_policy, "rebuild") ||
              Contains(surface.rebuild_policy, "preserve") ||
              Contains(surface.rebuild_policy, "recompute") ||
              Contains(surface.rebuild_policy, "discard"),
          "DPC-009 rebuild/transient policy missing");

  const std::set<std::string> backup_policies = {
      "preserve_committed",
      "rebuild_transient",
  };
  Require(backup_policies.count(surface.backup_restore_policy) == 1,
          "DPC-009 backup/restore policy missing or invalid");

  Require(StartsWith(surface.management_surface, "sys."),
          "DPC-009 management surface must be a sys management projection");
  Require(surface.support_bundle_field == "persisted_feature_map",
          "DPC-009 support bundle field must expose persisted feature map");
  Require(!surface.live_runtime_recovery_claim,
          "DPC-009 fixture must not claim live runtime recovery for future structures");
}

void ValidateRestrictedOpenContract(const PersistedSurfaceContract& surface) {
  const std::array<std::string_view, 6> required_actions = {
      "inspect", "validate", "repair", "rebuild", "backup", "export"};
  for (const auto action : required_actions) {
    Require(std::find(surface.restricted_open_actions.begin(),
                      surface.restricted_open_actions.end(),
                      action) != surface.restricted_open_actions.end(),
            "DPC-009 restricted-open safe action missing");
  }
  Require(std::find(surface.restricted_open_actions.begin(),
                    surface.restricted_open_actions.end(),
                    "ordinary_write") == surface.restricted_open_actions.end(),
          "DPC-009 restricted-open admitted ordinary writes");
}

void ValidateManagementPayload(const std::vector<PersistedSurfaceContract>& surfaces) {
  std::string payload;
  payload += std::string(kFixtureSearchKey);
  payload += ";contract_only_no_live_recovery_claim;";
  for (const auto& surface : surfaces) {
    payload += "feature=" + surface.feature_map_key;
    payload += ";format=" + std::to_string(surface.current.major) + "." +
               std::to_string(surface.current.minor);
    payload += ";min=" + std::to_string(surface.min_supported.major) + "." +
               std::to_string(surface.min_supported.minor);
    payload += ";max=" + std::to_string(surface.max_supported.major) + "." +
               std::to_string(surface.max_supported.minor);
    payload += ";repair=" + surface.repair_plan_id;
    payload += ";classification=" + surface.recovery_classification + ";";
  }

  Require(Contains(payload, kFixtureSearchKey),
          "DPC-009 management payload missing fixture search key");
  Require(Contains(payload, "contract_only_no_live_recovery_claim"),
          "DPC-009 management payload missing runtime-claim boundary");
  for (const auto& surface : surfaces) {
    Require(Contains(payload, surface.feature_map_key),
            "DPC-009 management payload omitted persisted feature-map entry");
    Require(Contains(payload, surface.repair_plan_id),
            "DPC-009 management payload omitted repair recommendation");
    Require(Contains(payload, surface.recovery_classification),
            "DPC-009 management payload omitted recovery classification");
  }
}

struct LifecycleFixture {
  std::filesystem::path temp_dir;
  std::filesystem::path database_path;
  std::string database_uuid;
  std::string filespace_uuid;
};

LifecycleFixture CreateLifecycleFixture() {
  const auto temp_dir = MakeTempDir();
  const auto now = CurrentUnixMillis();
  const auto database_uuid =
      uuid::GenerateEngineIdentityV7(UuidKind::database, now);
  const auto filespace_uuid =
      uuid::GenerateEngineIdentityV7(UuidKind::filespace, now + 1);
  Require(database_uuid.ok() && filespace_uuid.ok(),
          "DPC-009 lifecycle fixture UUID generation failed");

  LifecycleFixture fixture;
  fixture.temp_dir = temp_dir;
  fixture.database_path = temp_dir / "dpc009_restricted_open.sbdb";
  fixture.database_uuid = UuidText(database_uuid.value);
  fixture.filespace_uuid = UuidText(filespace_uuid.value);

  db::DatabaseCreateConfig create;
  create.path = fixture.database_path.string();
  create.database_uuid = database_uuid.value;
  create.filespace_uuid = filespace_uuid.value;
  create.page_size = 16384;
  create.creation_unix_epoch_millis = now + 2;
  create.allow_minimal_resource_bootstrap = true;
  create.require_resource_seed_pack = false;
  create.allow_overwrite = true;
  const auto created = db::CreateDatabaseFile(create);
  if (!created.ok()) {
    std::cerr << created.diagnostic.diagnostic_code << ':'
              << created.diagnostic.message_key << '\n';
  }
  Require(created.ok(), "DPC-009 lifecycle database create failed");

  const auto opened =
      db::OpenDatabaseFile({fixture.database_path.string(), false, false, false});
  Require(opened.ok(), "DPC-009 lifecycle first open failed");
  const auto clean = db::MarkDatabaseCleanShutdown(fixture.database_path.string());
  Require(clean.ok(), "DPC-009 lifecycle clean shutdown mark failed");
  return fixture;
}

db::DatabaseLifecycleOperationConfig OperationConfig(
    const LifecycleFixture& fixture) {
  db::DatabaseLifecycleOperationConfig config;
  config.path = fixture.database_path.string();
  config.operation_uuid = "dpc009-restricted-open-operation";
  config.actor_uuid = "dpc009-actor";
  config.write_evidence = true;
  return config;
}

db::DatabaseLifecycleRepairConfig RepairConfig(
    const LifecycleFixture& fixture,
    std::string repair_plan_id,
    bool admitted) {
  db::DatabaseLifecycleRepairConfig config;
  config.path = fixture.database_path.string();
  config.operation_uuid = "dpc009-restricted-open-repair";
  config.actor_uuid = "dpc009-actor";
  config.repair_plan_id = std::move(repair_plan_id);
  config.expected_database_uuid = fixture.database_uuid;
  config.expected_filespace_uuid = fixture.filespace_uuid;
  config.repair_admission_proven = admitted;
  config.allow_mutation = admitted;
  return config;
}

void ValidateRestrictedOpenLifecycleHelpers() {
  const auto fixture = CreateLifecycleFixture();
  const auto entered = db::EnterDatabaseRestrictedOpenMode(OperationConfig(fixture));
  Require(entered.ok() &&
              entered.state.phase == db::DatabaseLifecyclePhase::restricted_open &&
              entered.state.write_admission_fenced,
          "DPC-009 restricted-open lifecycle helper did not fence writes");

  const auto ordinary_open =
      db::OpenDatabaseFile({fixture.database_path.string(), false, false, false});
  Require(!ordinary_open.ok() &&
              ordinary_open.diagnostic.diagnostic_code ==
                  "SB-DB-LIFECYCLE-RESTRICTED-OPEN-REQUIRED",
          "DPC-009 ordinary open was not refused during restricted-open");

  const auto verified = db::VerifyDatabaseLifecycle(OperationConfig(fixture));
  Require(verified.ok(), "DPC-009 restricted-open verify helper failed");

  const auto refused =
      db::RepairDatabaseLifecycle(RepairConfig(fixture, "", false));
  Require(!refused.ok() &&
              refused.diagnostic.diagnostic_code == "ENGINE.DBLC_REPAIR_REFUSED",
          "DPC-009 repair without restricted admission was not refused");

  const auto repaired = db::RepairDatabaseLifecycle(
      RepairConfig(fixture, "clear_verified_write_fence", true));
  Require(repaired.ok() &&
              repaired.state.phase == db::DatabaseLifecyclePhase::repaired,
          "DPC-009 admitted restricted-open repair failed");

  const auto opened_after_repair =
      db::OpenDatabaseFile({fixture.database_path.string(), false, false, false});
  Require(opened_after_repair.ok(),
          "DPC-009 ordinary open after restricted repair failed");

  std::filesystem::remove_all(fixture.temp_dir);
}

void ValidatePersistedSurfaceContracts() {
  const auto& surfaces = PersistedFormatContracts();
  Require(surfaces.size() == 8,
          "DPC-009 fixture must cover all eight persisted optimization surfaces");

  std::set<std::string> feature_keys;
  std::set<std::string> artifact_kinds;
  for (const auto& surface : surfaces) {
    ValidateVersionContracts(surface);
    ValidateFeatureMapAndRepairContracts(surface, &feature_keys, &artifact_kinds);
    ValidateRestrictedOpenContract(surface);
  }
  ValidateManagementPayload(surfaces);
}

}  // namespace

int main() {
  // Search keys: DPC_PERSISTED_FORMAT_GATE, DPC_PERSISTED_FORMAT_FIXTURE.
  ValidatePersistedSurfaceContracts();
  ValidateRestrictedOpenLifecycleHelpers();
  std::cout << kGateSearchKey << "=passed " << kFixtureSearchKey
            << "=contract_only_no_live_recovery_claim\n";
  return EXIT_SUCCESS;
}
