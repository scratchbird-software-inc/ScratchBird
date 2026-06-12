// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// ODF-118 existing database backfill/bootstrap closure gate.

#include "catalog/catalog_object_lifecycle.hpp"
#include "catalog/descriptor_api.hpp"
#include "catalog/name_resolution_api.hpp"
#include "database_lifecycle.hpp"
#include "index_validation_repair_tooling.hpp"
#include "management/support_bundle_api.hpp"
#include "observability/performance_optimization_surface.hpp"
#include "startup_state.hpp"
#include "transaction/transaction_api.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace api = scratchbird::engine::internal_api;
namespace db = scratchbird::storage::database;
namespace idx = scratchbird::core::index;
namespace platform = scratchbird::core::platform;
namespace uuid = scratchbird::core::uuid;

constexpr std::string_view kGateSearchKey =
    "OPTIMIZER_DEFICIENCY_ODF_118_BACKFILL_BOOTSTRAP_GATE";

struct FormatVersion {
  platform::u32 major = 0;
  platform::u32 minor = 0;
};

struct OptimizedStructureContract {
  std::string structure_key;
  std::string feature_key;
  std::string artifact_kind;
  FormatVersion min_supported;
  FormatVersion current;
  FormatVersion max_supported;
  std::string catalog_object_kind;
  std::string catalog_lookup_name;
  std::string fresh_bootstrap_state;
  std::string missing_existing_action;
  std::string partial_state_action;
  std::string feature_disabled_behavior;
  std::string support_bundle_field;
  std::string uuid_resolution_api;
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

template <typename TResult>
void RequireEngineOk(const TResult& result, std::string_view message) {
  if (!result.ok) {
    if (!result.diagnostics.empty()) {
      std::cerr << result.diagnostics.front().code << ':'
                << result.diagnostics.front().detail << '\n';
    }
    Fail(message);
  }
}

void RequireLifecycleOk(const db::DatabaseLifecycleResult& result,
                        std::string_view message) {
  if (!result.ok()) {
    std::cerr << result.diagnostic.diagnostic_code << ':'
              << result.diagnostic.message_key << '\n';
    Fail(message);
  }
}

bool Contains(std::string_view haystack, std::string_view needle) {
  return haystack.find(needle) != std::string_view::npos;
}

bool StartsWith(std::string_view value, std::string_view prefix) {
  return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
}

bool LooksLikeUuid(std::string_view value) {
  if (value.size() != 36) {
    return false;
  }
  for (const std::size_t index : {8u, 13u, 18u, 23u}) {
    if (value[index] != '-') {
      return false;
    }
  }
  return true;
}

platform::u64 NowMillis() {
  return static_cast<platform::u64>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch()).count());
}

struct UuidFactory {
  platform::u64 base_millis = NowMillis();

  platform::TypedUuid Typed(platform::UuidKind kind, platform::u64 salt) const {
    const auto generated =
        uuid::GenerateEngineIdentityV7(kind, base_millis + salt);
    Require(generated.ok(), "ODF-118 UUID generation failed");
    return generated.value;
  }

  std::string Text(platform::UuidKind kind, platform::u64 salt) const {
    return uuid::UuidToString(Typed(kind, salt).value);
  }
};

struct TempDatabase {
  std::filesystem::path dir;
  std::filesystem::path path;
  UuidFactory uuids;
  platform::TypedUuid database_uuid;
  platform::TypedUuid filespace_uuid;
  std::string database_uuid_text;
  std::string filespace_uuid_text;
  std::string principal_uuid_text;

  explicit TempDatabase(std::string_view label) {
    dir = std::filesystem::temp_directory_path() /
          ("scratchbird_odf118_" + std::string(label) + "_" +
           std::to_string(NowMillis()));
    std::filesystem::create_directories(dir);
    path = dir / "odf118.sbdb";
    database_uuid = uuids.Typed(platform::UuidKind::database, 10);
    filespace_uuid = uuids.Typed(platform::UuidKind::filespace, 11);
    database_uuid_text = uuid::UuidToString(database_uuid.value);
    filespace_uuid_text = uuid::UuidToString(filespace_uuid.value);
    principal_uuid_text = uuids.Text(platform::UuidKind::principal, 12);
  }

  TempDatabase(const TempDatabase&) = delete;
  TempDatabase& operator=(const TempDatabase&) = delete;
  TempDatabase(TempDatabase&& other) noexcept
      : dir(std::move(other.dir)),
        path(std::move(other.path)),
        uuids(other.uuids),
        database_uuid(other.database_uuid),
        filespace_uuid(other.filespace_uuid),
        database_uuid_text(std::move(other.database_uuid_text)),
        filespace_uuid_text(std::move(other.filespace_uuid_text)),
        principal_uuid_text(std::move(other.principal_uuid_text)) {
    other.dir.clear();
    other.path.clear();
  }

  TempDatabase& operator=(TempDatabase&& other) noexcept {
    if (this != &other) {
      std::error_code ignored;
      if (!dir.empty()) {
        std::filesystem::remove_all(dir, ignored);
      }
      dir = std::move(other.dir);
      path = std::move(other.path);
      uuids = other.uuids;
      database_uuid = other.database_uuid;
      filespace_uuid = other.filespace_uuid;
      database_uuid_text = std::move(other.database_uuid_text);
      filespace_uuid_text = std::move(other.filespace_uuid_text);
      principal_uuid_text = std::move(other.principal_uuid_text);
      other.dir.clear();
      other.path.clear();
    }
    return *this;
  }

  ~TempDatabase() {
    std::error_code ignored;
    std::filesystem::remove_all(dir, ignored);
  }
};

api::EngineLocalizedName Name(std::string value) {
  api::EngineLocalizedName name;
  name.language_tag = "en";
  name.name_class = "primary";
  name.name = value;
  name.raw_name_text = value;
  name.display_name = value;
  name.default_name = true;
  return name;
}

api::EngineRequestContext BaseContext(const TempDatabase& fixture,
                                      std::string request_id,
                                      platform::u64 epoch) {
  api::EngineRequestContext context;
  context.trust_mode = api::EngineTrustMode::server_isolated;
  context.request_id = std::move(request_id);
  context.database_path = fixture.path.string();
  context.database_uuid.canonical = fixture.database_uuid_text;
  context.principal_uuid.canonical = fixture.principal_uuid_text;
  context.session_uuid.canonical =
      fixture.uuids.Text(platform::UuidKind::object, 200 + epoch);
  context.security_context_present = true;
  context.identifier_profile_uuid = "sbsql_v3";
  context.language_context.language_tag = "en";
  context.language_context.default_language_tag = "en";
  context.catalog_generation_id = epoch;
  context.name_resolution_epoch = epoch;
  context.security_epoch = epoch;
  context.resource_epoch = epoch;
  context.trace_tags = {"optimizer_deficiency_odf_118_gate",
                        "mga_transaction_regression"};
  return context;
}

api::EngineRequestContext Begin(const TempDatabase& fixture,
                                std::string request_id,
                                platform::u64 epoch) {
  api::EngineBeginTransactionRequest request;
  request.context = BaseContext(fixture, std::move(request_id), epoch);
  request.isolation_level = "read_committed";
  const auto begun = api::EngineBeginTransaction(request);
  RequireEngineOk(begun, "ODF-118 begin transaction failed");
  auto context = request.context;
  context.local_transaction_id = begun.local_transaction_id;
  context.transaction_uuid = begun.transaction_uuid;
  context.snapshot_visible_through_local_transaction_id =
      begun.snapshot_visible_through_local_transaction_id;
  context.transaction_isolation_level = begun.isolation_level;
  return context;
}

void Commit(const api::EngineRequestContext& context) {
  api::EngineCommitTransactionRequest request;
  request.context = context;
  RequireEngineOk(api::EngineCommitTransaction(request),
                  "ODF-118 commit transaction failed");
}

void Rollback(const api::EngineRequestContext& context) {
  api::EngineRollbackTransactionRequest request;
  request.context = context;
  RequireEngineOk(api::EngineRollbackTransaction(request),
                  "ODF-118 rollback transaction failed");
}

bool HasEvidence(const api::EngineApiResult& result,
                 std::string_view kind,
                 std::string_view value = {}) {
  for (const auto& evidence : result.evidence) {
    if (evidence.evidence_kind == kind &&
        (value.empty() || evidence.evidence_id == value)) {
      return true;
    }
  }
  return false;
}

bool HasRepairEvidence(const idx::IndexValidationRepairResult& result,
                       std::string_view key,
                       std::string_view value = {}) {
  for (const auto& evidence : result.support_evidence) {
    if (evidence.key == key && (value.empty() || evidence.value == value)) {
      return true;
    }
  }
  return false;
}

std::vector<OptimizedStructureContract> OptimizedStructures() {
  return {
      {"page_extent_summaries",
       "optimizer.persisted.page_extent_summaries",
       "dpc_page_extent_summary",
       {1, 0},
       {2, 0},
       {3, 0},
       "summary_index",
       "sys.management.optimization.page_extent_summaries",
       "bootstrap_current_on_fresh_create",
       "rebuild_from_authoritative_pages",
       "repair_or_safe_fallback",
       "safe_full_scan_fallback_when_disabled",
       "persisted_feature_map.page_extent_summaries",
       "EngineResolveName+EngineGetDescriptor"},
      {"secondary_index_delta_ledgers",
       "optimizer.persisted.secondary_index_delta_ledgers",
       "dpc_secondary_index_delta_ledger",
       {1, 0},
       {2, 0},
       {3, 0},
       "secondary_index",
       "sys.management.optimization.secondary_index_delta_ledgers",
       "bootstrap_current_on_fresh_create",
       "repair_committed_delta_or_rebuild_from_authoritative_base",
       "retain_or_repair_by_mga_commit_state",
       "safe_base_index_probe_fallback_when_disabled",
       "persisted_feature_map.secondary_index_delta_ledgers",
       "EngineResolveName+EngineGetDescriptor"},
      {"deferred_index_merge_state",
       "optimizer.persisted.deferred_index_merge_state",
       "dpc_deferred_index_merge_state",
       {1, 0},
       {2, 0},
       {3, 0},
       "secondary_index",
       "sys.management.optimization.deferred_index_merge_state",
       "bootstrap_current_on_fresh_create",
       "discard_cursor_and_rebuild_from_committed_ledger",
       "discard_unpublished_or_safe_fallback",
       "safe_base_index_probe_fallback_when_disabled",
       "persisted_feature_map.deferred_index_merge_state",
       "EngineResolveName+EngineGetDescriptor"},
      {"cleanup_horizon_markers",
       "optimizer.persisted.cleanup_horizon_markers",
       "dpc_cleanup_horizon_marker",
       {1, 0},
       {2, 0},
       {3, 0},
       "mga_cleanup_marker",
       "sys.management.optimization.cleanup_horizon_markers",
       "bootstrap_current_on_fresh_create",
       "recompute_from_transaction_inventory_horizons",
       "recompute_or_refuse_without_mga_horizon",
       "safe_cleanup_scheduler_fallback_when_disabled",
       "persisted_feature_map.cleanup_horizon_markers",
       "EngineResolveName+EngineGetDescriptor"},
      {"shadow_index_build_state",
       "optimizer.persisted.shadow_index_build_state",
       "dpc_shadow_index_build_state",
       {1, 0},
       {2, 0},
       {3, 0},
       "shadow_index",
       "sys.management.optimization.shadow_index_build_state",
       "bootstrap_current_on_fresh_create",
       "resume_validated_phase_or_discard_unpublished",
       "discard_unpublished_until_publish_barrier",
       "refuse_shadow_visibility_when_disabled",
       "persisted_feature_map.shadow_index_build_state",
       "EngineResolveName+EngineGetDescriptor"},
      {"search_inverted_segments",
       "optimizer.persisted.search_inverted_segments",
       "dpc_search_inverted_segment",
       {1, 0},
       {2, 0},
       {3, 0},
       "search_provider_generation",
       "sys.management.optimization.search_inverted_segments",
       "bootstrap_current_on_fresh_create",
       "preserve_visible_segments_and_rebuild_pending",
       "discard_unpublished_or_exact_fallback",
       "exact_search_fallback_when_disabled",
       "persisted_feature_map.search_inverted_segments",
       "EngineResolveName+EngineGetDescriptor"},
      {"vector_generations",
       "optimizer.persisted.vector_generations",
       "dpc_vector_generation",
       {1, 0},
       {2, 0},
       {3, 0},
       "vector_provider_generation",
       "sys.management.optimization.vector_generations",
       "bootstrap_current_on_fresh_create",
       "preserve_published_generation_and_rebuild_pending",
       "discard_unpublished_or_exact_vector_scan",
       "exact_vector_scan_fallback_when_disabled",
       "persisted_feature_map.vector_generations",
       "EngineResolveName+EngineGetDescriptor"},
      {"optimization_management_metadata",
       "optimizer.persisted.management_metadata",
       "dpc_optimization_management_metadata",
       {1, 0},
       {2, 0},
       {3, 0},
       "management_projection",
       "sys.management.optimization.persisted_features",
       "bootstrap_current_on_fresh_create",
       "rebuild_projection_from_feature_map_and_repair_evidence",
       "rebuild_or_refuse_with_exact_message_vector",
       "support_bundle_reports_disabled_state",
       "persisted_feature_map.management_metadata",
       "EngineResolveName+EngineGetDescriptor"},
  };
}

db::DatabaseArtifactVersionCompatibilityRequest VersionRequest(
    const OptimizedStructureContract& structure,
    FormatVersion version) {
  db::DatabaseArtifactVersionCompatibilityRequest request;
  request.artifact_kind = structure.artifact_kind;
  request.format_major = version.major;
  request.format_minor = version.minor;
  request.min_supported_major = structure.min_supported.major;
  request.min_supported_minor = structure.min_supported.minor;
  request.current_major = structure.current.major;
  request.current_minor = structure.current.minor;
  request.max_supported_major = structure.max_supported.major;
  request.max_supported_minor = structure.max_supported.minor;
  return request;
}

std::string MigrationPlanId(const OptimizedStructureContract& structure,
                            FormatVersion from) {
  return structure.artifact_kind + "_v" + std::to_string(from.major) + "_" +
         std::to_string(from.minor) + "_to_v" +
         std::to_string(structure.current.major) + "_" +
         std::to_string(structure.current.minor) + "_explicit_plan_v1";
}

void RequireCompatibilityRefusal(
    const db::DatabaseArtifactCompatibilityResult& result,
    db::DatabaseOpenCompatibilityClass expected_class,
    std::string_view expected_code,
    std::string_view message) {
  if (result.ok() || result.compatibility_class != expected_class ||
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

void ProveCompatibilityAndBackfillContracts() {
  const auto structures = OptimizedStructures();
  Require(structures.size() == 8,
          "ODF-118 must cover all optimized persisted structure contracts");
  std::set<std::string> feature_keys;
  std::set<std::string> artifact_kinds;
  std::set<std::string> support_fields;

  for (const auto& structure : structures) {
    Require(StartsWith(structure.feature_key, "optimizer.persisted."),
            "ODF-118 feature key missing optimizer persisted namespace");
    Require(StartsWith(structure.artifact_kind, "dpc_"),
            "ODF-118 must classify real persisted artifact kinds");
    Require(!LooksLikeUuid(structure.structure_key) &&
                !LooksLikeUuid(structure.feature_key) &&
                !LooksLikeUuid(structure.catalog_lookup_name),
            "ODF-118 structure contract contains UUID-looking hard-coded key");
    Require(feature_keys.insert(structure.feature_key).second,
            "ODF-118 duplicate feature key");
    Require(artifact_kinds.insert(structure.artifact_kind).second,
            "ODF-118 duplicate artifact kind");
    Require(support_fields.insert(structure.support_bundle_field).second,
            "ODF-118 duplicate support-bundle field");
    Require(structure.uuid_resolution_api ==
                "EngineResolveName+EngineGetDescriptor",
            "ODF-118 structure must use standard UUID-resolution APIs");
    Require(Contains(structure.fresh_bootstrap_state, "bootstrap_current"),
            "ODF-118 fresh bootstrap state missing");
    Require(Contains(structure.missing_existing_action, "rebuild") ||
                Contains(structure.missing_existing_action, "recompute") ||
                Contains(structure.missing_existing_action, "repair") ||
                Contains(structure.missing_existing_action, "preserve") ||
                Contains(structure.missing_existing_action, "discard"),
            "ODF-118 existing missing-structure action missing");
    Require(Contains(structure.partial_state_action, "discard") ||
                Contains(structure.partial_state_action, "repair") ||
                Contains(structure.partial_state_action, "refuse") ||
                Contains(structure.partial_state_action, "fallback") ||
                Contains(structure.partial_state_action, "recompute"),
            "ODF-118 partial-state action missing");
    Require(Contains(structure.feature_disabled_behavior, "fallback") ||
                Contains(structure.feature_disabled_behavior, "refuse") ||
                Contains(structure.feature_disabled_behavior, "disabled"),
            "ODF-118 feature-disabled behavior missing");

    const auto current =
        db::ClassifyDatabaseArtifactVersionCompatibility(
            VersionRequest(structure, structure.current));
    Require(current.ok() &&
                current.compatibility_class ==
                    db::DatabaseOpenCompatibilityClass::current,
            "ODF-118 current optimized structure format was not accepted");

    const FormatVersion older{structure.current.major - 1, 0};
    auto old_without_plan = VersionRequest(structure, older);
    RequireCompatibilityRefusal(
        db::ClassifyDatabaseArtifactVersionCompatibility(old_without_plan),
        db::DatabaseOpenCompatibilityClass::migration_required_without_plan_refused,
        "ENGINE.DBLC_MIGRATION_REQUIRED_WITHOUT_PLAN",
        "ODF-118 older optimized structure did not require a rebuild plan");

    auto old_with_plan = old_without_plan;
    old_with_plan.migration_plan_id = MigrationPlanId(structure, older);
    const auto planned =
        db::ClassifyDatabaseArtifactVersionCompatibility(old_with_plan);
    Require(planned.ok() &&
                planned.compatibility_class ==
                    db::DatabaseOpenCompatibilityClass::supported_migration &&
                planned.migration_required,
            "ODF-118 older optimized structure plan was not admitted");

    RequireCompatibilityRefusal(
        db::ClassifyDatabaseArtifactVersionCompatibility(
            VersionRequest(structure, {structure.current.major + 1, 0})),
        db::DatabaseOpenCompatibilityClass::unsupported_new,
        "ENGINE.DBLC_MIGRATION_UNSUPPORTED_NEW_ARTIFACT",
        "ODF-118 incompatible optimized structure did not refuse exactly");

    auto ambiguous = VersionRequest(structure, structure.current);
    ambiguous.identity_proven = false;
    RequireCompatibilityRefusal(
        db::ClassifyDatabaseArtifactVersionCompatibility(ambiguous),
        db::DatabaseOpenCompatibilityClass::ambiguous_identity_refused,
        "ENGINE.DBLC_MIGRATION_AMBIGUOUS_IDENTITY_REFUSED",
        "ODF-118 ambiguous optimized structure identity did not refuse");
  }
}

TempDatabase CreateAndOpenFreshDatabase(std::string_view label) {
  TempDatabase fixture(label);
  db::DatabaseCreateConfig create;
  create.path = fixture.path.string();
  create.database_uuid = fixture.database_uuid;
  create.filespace_uuid = fixture.filespace_uuid;
  create.page_size = 16384;
  create.creation_unix_epoch_millis = fixture.uuids.base_millis;
  create.allow_minimal_resource_bootstrap = true;
  create.require_resource_seed_pack = false;
  create.allow_overwrite = true;

  const auto created = db::CreateDatabaseFile(create);
  RequireLifecycleOk(created, "ODF-118 fresh database create failed");
  Require(created.state.startup_state_present,
          "ODF-118 fresh create omitted startup-state evidence");
  Require(created.state.local_transaction_inventory_present,
          "ODF-118 fresh create omitted transaction-inventory evidence");
  Require(created.state.startup_state.bootstrap_local_transaction_id == 1,
          "ODF-118 fresh bootstrap did not commit bootstrap transaction 1");
  Require(db::StartupLifecycleEvidencePresent(
              created.state.startup_state,
              db::StartupLifecycleEvidenceFlag::bootstrap_tx1_committed),
          "ODF-118 fresh bootstrap omitted durable bootstrap evidence flag");

  const auto opened = db::OpenDatabaseFile({fixture.path.string(), false, false, false});
  RequireLifecycleOk(opened, "ODF-118 fresh database open failed");
  Require(opened.state.database_open_compatibility_class ==
              db::DatabaseOpenCompatibilityClass::current,
          "ODF-118 fresh database did not open as current");
  const auto clean = db::MarkDatabaseCleanShutdown(fixture.path.string());
  Require(clean.ok(), "ODF-118 clean shutdown mark failed");
  return fixture;
}

api::EngineCatalogCreateObjectRequest CreateObjectRequest(
    const api::EngineRequestContext& context,
    const std::string& object_uuid,
    std::string object_kind,
    const std::string& schema_uuid,
    std::string object_name) {
  api::EngineCatalogCreateObjectRequest request;
  request.context = context;
  request.target_object.uuid.canonical = object_uuid;
  request.target_object.object_kind = std::move(object_kind);
  request.target_schema.uuid.canonical = schema_uuid;
  request.localized_names.push_back(Name(std::move(object_name)));
  return request;
}

api::EngineResolveNameRequest ResolveRequest(const api::EngineRequestContext& context,
                                             const std::string& schema_uuid,
                                             std::string object_kind,
                                             std::string object_name) {
  api::EngineResolveNameRequest request;
  request.context = context;
  request.target_schema.uuid.canonical = schema_uuid;
  request.target_object.object_kind = std::move(object_kind);
  request.localized_names.push_back(Name(std::move(object_name)));
  return request;
}

api::EngineGetDescriptorRequest DescriptorRequest(
    const api::EngineRequestContext& context,
    const std::string& table_uuid) {
  api::EngineGetDescriptorRequest request;
  request.context = context;
  request.target_object.uuid.canonical = table_uuid;
  request.target_object.object_kind = "table";
  request.option_envelopes.push_back("descriptor_cache:enabled");
  return request;
}

std::string CreateResolveAndDescribeNamedTable(TempDatabase* fixture,
                                               std::string schema_name,
                                               std::string table_name,
                                               platform::u64 salt) {
  const std::string schema_uuid =
      fixture->uuids.Text(platform::UuidKind::object, salt + 1);
  const std::string table_uuid =
      fixture->uuids.Text(platform::UuidKind::object, salt + 2);

  auto schema_context = Begin(*fixture, "odf118-create-schema", 1);
  const auto created_schema = api::EngineCatalogCreateObject(
      CreateObjectRequest(schema_context, schema_uuid, "schema", {}, schema_name));
  RequireEngineOk(created_schema, "ODF-118 schema create failed");
  Commit(schema_context);

  auto table_context =
      Begin(*fixture, "odf118-create-table", created_schema.metadata_cache_epoch);
  const auto created_table = api::EngineCatalogCreateObject(
      CreateObjectRequest(table_context,
                          table_uuid,
                          "table",
                          schema_uuid,
                          table_name));
  RequireEngineOk(created_table, "ODF-118 table create failed");
  Require(created_table.primary_object.uuid.canonical == table_uuid,
          "ODF-118 table create did not preserve generated object UUID");
  Commit(table_context);

  auto read_context =
      Begin(*fixture, "odf118-resolve-table", created_table.metadata_cache_epoch);
  read_context.name_resolution_epoch = created_table.metadata_cache_epoch;
  const auto resolved = api::EngineResolveName(
      ResolveRequest(read_context, schema_uuid, "table", table_name));
  RequireEngineOk(resolved, "ODF-118 standard name resolver failed");
  Require(resolved.bound_object_identity.object_uuid.canonical == table_uuid,
          "ODF-118 standard name resolver did not return generated UUID");

  const auto descriptor =
      api::EngineGetDescriptor(DescriptorRequest(read_context, table_uuid));
  RequireEngineOk(descriptor, "ODF-118 descriptor lookup failed");
  Require(descriptor.descriptor.descriptor_uuid.canonical == table_uuid,
          "ODF-118 descriptor did not bind generated UUID");
  Require(HasEvidence(descriptor, "descriptor_metadata_cache"),
          "ODF-118 descriptor cache did not record UUID-bound evidence");
  Rollback(read_context);
  return table_uuid;
}

void ProveUuidResolutionHasNoHardCodedDependency() {
  auto first = CreateAndOpenFreshDatabase("uuid_a");
  auto second = CreateAndOpenFreshDatabase("uuid_b");
  const std::string first_table = CreateResolveAndDescribeNamedTable(
      &first, "odf118_schema", "odf118_table", 300);
  const std::string second_table = CreateResolveAndDescribeNamedTable(
      &second, "odf118_schema", "odf118_table", 300);
  Require(first_table != second_table,
          "ODF-118 same-name objects in separate databases reused a fixed UUID");
  Require(!LooksLikeUuid("odf118_schema") && !LooksLikeUuid("odf118_table"),
          "ODF-118 test names were accidentally UUID-looking");
}

idx::IndexValidationRepairTarget Target(const UuidFactory& uuids,
                                        platform::u64 salt,
                                        idx::IndexFamily family =
                                            idx::IndexFamily::btree) {
  idx::IndexValidationRepairTarget target;
  target.database_uuid = uuids.Typed(platform::UuidKind::database, salt + 1);
  target.table_uuid = uuids.Typed(platform::UuidKind::object, salt + 2);
  target.index_uuid = uuids.Typed(platform::UuidKind::object, salt + 3);
  target.generation_uuid = uuids.Typed(platform::UuidKind::object, salt + 4);
  target.physical_family = family;
  target.names_resolved_to_uuids = true;
  target.catalog_resolution_proven = true;
  target.contains_sql_text = false;
  return target;
}

idx::IndexValidationRepairRequest BaseRepairRequest(
    const UuidFactory& uuids,
    platform::u64 salt,
    idx::IndexValidationRepairFamily family,
    idx::IndexValidationRepairOperation operation) {
  idx::IndexValidationRepairRequest request;
  request.operation = operation;
  request.validation_family = family;
  request.target = Target(uuids, salt);
  request.policy_allows_mutation =
      idx::IndexValidationRepairOperationMutates(operation);
  return request;
}

idx::PageExtentSummaryFormatCompatibility CurrentPageSummaryFormat() {
  const auto contract = idx::PageExtentSummaryPersistedFormatContract();
  idx::PageExtentSummaryFormatCompatibility format;
  format.observed = contract.current;
  format.open_class = idx::PageExtentSummaryFormatOpenClass::current;
  format.compatible = true;
  format.migration_required = false;
  format.diagnostic_code = "ODF-118.current_page_summary_format";
  return format;
}

idx::PageExtentSummaryMetadata MissingPageSummary(const UuidFactory& uuids,
                                                  platform::u64 salt) {
  const auto contract = idx::PageExtentSummaryPersistedFormatContract();
  idx::PageExtentSummaryMetadata metadata;
  metadata.relation_uuid = uuids.Text(platform::UuidKind::object, salt + 1);
  metadata.summary_uuid = uuids.Text(platform::UuidKind::object, salt + 2);
  metadata.range.kind = idx::PageExtentSummaryRangeKind::page_range;
  metadata.range.first_page_id = 10;
  metadata.range.page_count = 4;
  metadata.boundary.scalar_type_key = "int64_lex";
  metadata.boundary.encoded_min = "010";
  metadata.boundary.encoded_max = "030";
  metadata.boundary.min_present = true;
  metadata.boundary.max_present = true;
  metadata.row_count = 0;
  metadata.status = idx::PageExtentSummaryStatus::missing;
  metadata.format_version = contract.current;
  metadata.generation = 0;
  metadata.persisted_record_present = false;
  metadata.checksum_valid = true;
  return metadata;
}

idx::PageExtentSummaryMaintenanceEvent RebuildPageSummaryEvent(
    const idx::PageExtentSummaryMetadata& metadata) {
  idx::PageExtentSummaryMaintenanceEvent event;
  event.kind = idx::PageExtentSummaryMaintenanceEventKind::rebuild;
  event.relation_uuid = metadata.relation_uuid;
  event.summary_uuid = metadata.summary_uuid;
  idx::PageExtentSummaryRowEvidence first;
  first.page_id = 11;
  first.extent_id = 0;
  first.scalar_type_key = "int64_lex";
  first.encoded_scalar = "011";
  first.engine_mga_visible = true;
  event.base_page_rows.push_back(std::move(first));
  idx::PageExtentSummaryRowEvidence second;
  second.page_id = 12;
  second.extent_id = 0;
  second.scalar_type_key = "int64_lex";
  second.encoded_scalar = "025";
  second.engine_mga_visible = true;
  event.base_page_rows.push_back(std::move(second));
  event.caller_allows_transient_rebuild = true;
  return event;
}

idx::SecondaryIndexDeltaLedgerRecord DeltaRecord(
    const UuidFactory& uuids,
    const idx::IndexValidationRepairTarget& target,
    platform::u64 salt,
    idx::SecondaryIndexDeltaLedgerCommitState state) {
  idx::SecondaryIndexDeltaLedgerRecord record;
  record.delta.delta_id = uuids.Typed(platform::UuidKind::object, salt + 1);
  record.delta.index_uuid = target.index_uuid;
  record.delta.table_uuid = target.table_uuid;
  record.delta.row_uuid = uuids.Typed(platform::UuidKind::row, salt + 2);
  record.delta.version_uuid = uuids.Typed(platform::UuidKind::row, salt + 3);
  record.delta.transaction_uuid =
      uuids.Typed(platform::UuidKind::transaction, salt + 4);
  record.delta.local_transaction_id = salt + 20;
  record.delta.delta_kind = idx::SecondaryIndexDeltaKind::insert;
  record.delta.key_payload = "odf118:key:" + std::to_string(salt);
  record.delta.cleanup_horizon_token = "odf118:horizon:" + std::to_string(salt);
  record.delta.committed =
      state != idx::SecondaryIndexDeltaLedgerCommitState::precommit_uncommitted;
  record.commit_state = state;
  record.source_evidence_reference = "odf118:delta:" + std::to_string(salt);
  return record;
}

idx::ShadowIndexBuildRecord ShadowRecord(const UuidFactory& uuids,
                                         platform::u64 salt,
                                         bool published) {
  idx::ShadowIndexBuildRecord record;
  record.build_id = uuids.Typed(platform::UuidKind::object, salt + 1);
  record.shadow_index_uuid = uuids.Typed(platform::UuidKind::object, salt + 2);
  record.table_uuid = uuids.Typed(platform::UuidKind::object, salt + 3);
  record.validation_evidence_present = true;
  record.publish_barrier_evidence_present = published;
  record.publish_barrier_engine_owned_mga = published;
  record.validation_evidence_ref = "odf118:validation:shadow";
  record.publish_barrier_evidence_ref = "odf118:publish:shadow";
  record.engine_mga_inventory_evidence_ref = "odf118:mga_inventory:shadow";
  record.engine_mga_horizon_evidence_ref = "odf118:mga_horizon:shadow";
  record.state = published ? idx::ShadowIndexBuildState::published
                           : idx::ShadowIndexBuildState::validated;
  record.planner_visible = published;
  record.read_visible = published;
  record.published_index_uuid = published ? record.shadow_index_uuid
                                          : platform::TypedUuid{};
  return record;
}

idx::InvertedSearchSegmentDescriptor SearchSegment(
    const UuidFactory& uuids,
    platform::u64 salt,
    idx::InvertedSearchSegmentState state) {
  idx::InvertedSearchSegmentDescriptor segment;
  segment.segment_uuid = uuids.Typed(platform::UuidKind::object, salt + 1);
  segment.index_uuid = uuids.Typed(platform::UuidKind::object, salt + 2);
  segment.table_uuid = uuids.Typed(platform::UuidKind::object, salt + 3);
  segment.generation = salt;
  segment.state = state;
  segment.visible = state == idx::InvertedSearchSegmentState::visible;
  segment.persisted_record_present = segment.visible;
  segment.checksum_valid = true;
  segment.complete = segment.visible;
  segment.validation_evidence_present = segment.visible;
  segment.publish_barrier_evidence_present = segment.visible;
  segment.publish_barrier_engine_owned_mga = segment.visible;
  segment.validation_evidence_ref = "odf118:validation:search";
  segment.publish_barrier_evidence_ref = "odf118:publish:search";
  segment.engine_mga_inventory_evidence_ref = "odf118:mga_inventory:search";
  segment.engine_mga_horizon_evidence_ref = "odf118:mga_horizon:search";
  return segment;
}

idx::VectorGenerationDescriptor VectorGeneration(
    const UuidFactory& uuids,
    platform::u64 salt,
    idx::VectorGenerationState state) {
  idx::VectorGenerationDescriptor generation;
  generation.generation_uuid = uuids.Typed(platform::UuidKind::object, salt + 1);
  generation.index_uuid = uuids.Typed(platform::UuidKind::object, salt + 2);
  generation.table_uuid = uuids.Typed(platform::UuidKind::object, salt + 3);
  generation.generation = salt;
  generation.state = state;
  generation.visible = state == idx::VectorGenerationState::published;
  generation.persisted_record_present = generation.visible;
  generation.checksum_valid = true;
  generation.complete = generation.visible;
  generation.training_evidence_present = true;
  generation.validation_evidence_present = generation.visible;
  generation.sealed_generation_evidence_present = generation.visible;
  generation.recall_contract_evidence_present = generation.visible;
  generation.publish_barrier_evidence_present = generation.visible;
  generation.publish_barrier_engine_owned_mga = generation.visible;
  generation.training_evidence_ref = "odf118:training:vector";
  generation.validation_evidence_ref = "odf118:validation:vector";
  generation.sealed_generation_evidence_ref = "odf118:sealed:vector";
  generation.recall_contract_evidence_ref = "odf118:recall:vector";
  generation.publish_barrier_evidence_ref = "odf118:publish:vector";
  generation.engine_mga_inventory_evidence_ref = "odf118:mga_inventory:vector";
  generation.engine_mga_horizon_evidence_ref = "odf118:mga_horizon:vector";
  generation.resource_envelope.memory_limit_bytes = 1024;
  generation.resource_envelope.memory_observed_bytes = 512;
  generation.resource_envelope.temp_space_limit_bytes = 2048;
  generation.resource_envelope.temp_space_observed_bytes = 1000;
  generation.resource_envelope.worker_limit = 2;
  generation.resource_envelope.workers_used = 1;
  generation.resource_envelope.resource_governor_evidence_present = true;
  generation.resource_envelope.resource_governor_evidence_ref =
      "odf118:resource:vector";
  generation.recall_contract.top_k = 4;
  generation.recall_contract.exact_sample_rows = 16;
  generation.recall_contract.required_recall = 0.90;
  generation.recall_contract.observed_recall = 0.95;
  generation.recall_contract.deterministic_sample = true;
  generation.recall_contract.evidence_present = true;
  generation.recall_contract.evidence_ref =
      generation.recall_contract_evidence_ref;
  return generation;
}

void ProveExistingAndPartialStructureRepairBehavior() {
  const UuidFactory uuids;

  auto page_rebuild = BaseRepairRequest(
      uuids, 1000, idx::IndexValidationRepairFamily::page_extent_summary,
      idx::IndexValidationRepairOperation::rebuild);
  page_rebuild.state.page_extent_summary = MissingPageSummary(uuids, 1010);
  page_rebuild.state.page_extent_summary_format = CurrentPageSummaryFormat();
  page_rebuild.state.page_extent_rebuild_event =
      RebuildPageSummaryEvent(page_rebuild.state.page_extent_summary);
  const auto rebuilt = idx::ExecuteIndexValidationRepairOperation(page_rebuild);
  Require(rebuilt.ok() && rebuilt.mutation_applied && rebuilt.planner_visible,
          "ODF-118 missing page summary did not rebuild deterministically");
  Require(rebuilt.diagnostic.diagnostic_code ==
              "DPC.INDEX_REPAIR.PAGE_SUMMARY_REBUILT",
          "ODF-118 page summary rebuild diagnostic changed");
  Require(HasRepairEvidence(rebuilt, "page_summary.rebuild_classification"),
          "ODF-118 page summary rebuild omitted support evidence");

  auto disabled_safe = page_rebuild;
  disabled_safe.operation = idx::IndexValidationRepairOperation::validate;
  disabled_safe.policy_allows_mutation = false;
  const auto disabled_result =
      idx::ExecuteIndexValidationRepairOperation(disabled_safe);
  Require(disabled_result.classification ==
              idx::IndexValidationRepairClass::safe_fallback,
          "ODF-118 feature-disabled missing summary did not select safe fallback");
  Require(disabled_result.diagnostic.diagnostic_code ==
              "DPC.INDEX_REPAIR.PAGE_SUMMARY_SAFE_FALLBACK",
          "ODF-118 feature-disabled safe fallback diagnostic changed");

  auto read_only_refused = page_rebuild;
  read_only_refused.read_only_database = true;
  const auto read_only_result =
      idx::ExecuteIndexValidationRepairOperation(read_only_refused);
  Require(!read_only_result.ok() &&
              read_only_result.diagnostic.diagnostic_code ==
                  "DPC.INDEX_REPAIR.READ_ONLY_REFUSED",
          "ODF-118 read-only rebuild refusal diagnostic changed");

  auto delta_repair = BaseRepairRequest(
      uuids, 2000, idx::IndexValidationRepairFamily::secondary_delta_ledger,
      idx::IndexValidationRepairOperation::repair);
  delta_repair.state.delta_ledger.records.push_back(DeltaRecord(
      uuids, delta_repair.target, 2010,
      idx::SecondaryIndexDeltaLedgerCommitState::committed_premerge));
  const auto repaired_delta =
      idx::ExecuteIndexValidationRepairOperation(delta_repair);
  Require(repaired_delta.ok() && repaired_delta.mutation_applied,
          "ODF-118 committed delta ledger did not repair");
  Require(repaired_delta.diagnostic.diagnostic_code ==
              "DPC.INDEX_REPAIR.DELTA_LEDGER_REPAIRED",
          "ODF-118 delta repair diagnostic changed");
  Require(HasRepairEvidence(repaired_delta, "delta.recovery_class") &&
              HasRepairEvidence(repaired_delta, "delta.recovery_action"),
          "ODF-118 delta repair support evidence missing");

  auto shadow_discard = BaseRepairRequest(
      uuids, 3000, idx::IndexValidationRepairFamily::shadow_index_build_state,
      idx::IndexValidationRepairOperation::discard_unpublished);
  shadow_discard.state.shadow_build = ShadowRecord(uuids, 3010, false);
  const auto discarded_shadow =
      idx::ExecuteIndexValidationRepairOperation(shadow_discard);
  Require(discarded_shadow.ok() && discarded_shadow.mutation_applied,
          "ODF-118 partially built shadow index was not discarded");
  Require(discarded_shadow.diagnostic.diagnostic_code ==
              "DPC.INDEX_REPAIR.SHADOW_DISCARDED",
          "ODF-118 shadow discard diagnostic changed");

  auto search_discard = BaseRepairRequest(
      uuids, 4000,
      idx::IndexValidationRepairFamily::inverted_search_segment_state,
      idx::IndexValidationRepairOperation::discard_unpublished);
  search_discard.state.inverted_segments.segments.push_back(
      SearchSegment(uuids, 4010, idx::InvertedSearchSegmentState::building));
  const auto discarded_search =
      idx::ExecuteIndexValidationRepairOperation(search_discard);
  Require(discarded_search.ok() && discarded_search.mutation_applied,
          "ODF-118 partially built search segment was not discarded");
  Require(discarded_search.diagnostic.diagnostic_code ==
              "DPC.INDEX_REPAIR.SEARCH_SEGMENT_DISCARDED_UNSAFE",
          "ODF-118 search discard diagnostic changed");

  auto vector_discard = BaseRepairRequest(
      uuids, 5000,
      idx::IndexValidationRepairFamily::vector_generation_state,
      idx::IndexValidationRepairOperation::discard_unpublished);
  vector_discard.state.vector_generations.generations.push_back(
      VectorGeneration(uuids, 5010, idx::VectorGenerationState::building));
  const auto discarded_vector =
      idx::ExecuteIndexValidationRepairOperation(vector_discard);
  Require(discarded_vector.ok() && discarded_vector.mutation_applied,
          "ODF-118 partially built vector generation was not discarded");
  Require(discarded_vector.diagnostic.diagnostic_code ==
              "DPC.INDEX_REPAIR.VECTOR_GENERATION_DISCARDED_UNSAFE",
          "ODF-118 vector discard diagnostic changed");

  auto unresolved = page_rebuild;
  unresolved.target.names_resolved_to_uuids = false;
  const auto unresolved_result =
      idx::ExecuteIndexValidationRepairOperation(unresolved);
  Require(!unresolved_result.ok() &&
              unresolved_result.diagnostic.diagnostic_code ==
                  "DPC.INDEX_REPAIR.IDENTITY_REFUSED",
          "ODF-118 unresolved-name repair did not fail closed");
}

api::PerformanceOptimizationSurfaceSnapshot Odf118SurfaceSnapshot() {
  auto snapshot = api::DefaultPerformanceOptimizationSurfaceSnapshot();
  snapshot.optimization_profile = "odf118_backfill_bootstrap";
  snapshot.catalog_generation_id = 118;
  snapshot.name_resolution_epoch = 118;
  snapshot.security_epoch = 118;
  snapshot.resource_epoch = 118;
  snapshot.optimization_state_epoch = 118;
  snapshot.summary_prune_status = "safe_fallback";
  snapshot.summary_prune_last_reason = "missing_structure_rebuilt";
  snapshot.summary_prune_fallback_reason = "feature_disabled_safe_fallback";
  snapshot.summary_prune_summary_status = "rebuild_required_then_current";
  snapshot.summary_prune_generation = 11801;
  snapshot.cleanup_horizon_authority_status = "authoritative";
  snapshot.cleanup_horizon_authoritative = true;
  snapshot.cleanup_horizon_local_transaction_id = 1;
  snapshot.oldest_interesting_transaction_id = 1;
  snapshot.oldest_active_transaction_id = 1;
  snapshot.oldest_snapshot_transaction_id = 1;
  snapshot.oldest_cleanup_transaction_id = 1;
  snapshot.secondary_index_state = "delta_ledger_repaired";
  snapshot.shadow_index_state = "unpublished_discarded";
  snapshot.summary_index_state = "missing_rebuilt";
  snapshot.specialized_index_state =
      "search_vector_unpublished_generations_discarded";
  snapshot.index_state_authority_source =
      "engine_catalog_uuid_resolution_and_mga_inventory";
  snapshot.exact_refusal_diagnostic_code =
      "ENGINE.DBLC_MIGRATION_UNSUPPORTED_NEW_ARTIFACT";
  snapshot.exact_refusal_message_vector =
      "ENGINE.DBLC_MIGRATION_UNSUPPORTED_NEW_ARTIFACT|"
      "DPC.INDEX_REPAIR.READ_ONLY_REFUSED|"
      "DPC.INDEX_REPAIR.IDENTITY_REFUSED";
  snapshot.exact_refusal_source = "odf118.backfill_bootstrap";
  snapshot.message_vector_ready = true;
  snapshot.audit_event_family = "engine.audit.odf118.backfill_bootstrap";
  snapshot.audit_event_count = 8;
  snapshot.audit_last_decision = "rebuild_or_exact_refusal";
  snapshot.metric_family = "sys.metrics.odf118.backfill_bootstrap";
  snapshot.metric_sample_count = 8;
  snapshot.support_bundle_correlation_id = "odf118-support-bundle";
  snapshot.support_bundle_redaction_state = "public_safe_summary";
  snapshot.support_bundle_completeness_state = "complete";
  snapshot.support_bundle_forbidden_fields_absent = true;

  snapshot.odf108_feature_gates = {
      {"optimizer.persisted.page_extent_summaries",
       false,
       "disabled_safe_fallback",
       "engine_config_policy",
       "DPC.INDEX_REPAIR.PAGE_SUMMARY_SAFE_FALLBACK"},
      {"optimizer.persisted.shadow_index_build_state",
       true,
       "enabled_repair_only",
       "engine_config_policy",
       "none"}};
  snapshot.odf108_runtime_compatibility = {
      {"odf118.database_open",
       "compatible",
       "use_structure",
       118,
       "format_version,feature_bit,uuid_resolution",
       "supported",
       "none",
       "none"},
      {"odf118.incompatible_open",
       "unsupported_new",
       "refuse_exact_message_vector",
       119,
       "format_version,feature_bit,uuid_resolution",
       "future_artifact",
       "ENGINE.DBLC_MIGRATION_UNSUPPORTED_NEW_ARTIFACT",
       "none"}};
  snapshot.odf108_rebuild_states = {
      {"page_extent_summary",
       "completed",
       "rebuild_from_authoritative_base_pages",
       11801,
       2,
       2,
       "none"},
      {"shadow_index_build_state",
       "completed",
       "discard_unpublished",
       11802,
       1,
       1,
       "none"},
      {"vector_generation",
       "completed",
       "discard_unpublished",
       11803,
       1,
       1,
       "none"}};
  snapshot.odf108_exact_refusals = {
      {"database_open",
       "ENGINE.DBLC_MIGRATION_UNSUPPORTED_NEW_ARTIFACT",
       snapshot.exact_refusal_message_vector,
       "refused",
       "public_safe_summary",
       true},
      {"index_repair",
       "DPC.INDEX_REPAIR.IDENTITY_REFUSED",
       snapshot.exact_refusal_message_vector,
       "refused",
       "public_safe_summary",
       true}};
  return snapshot;
}

void RequireNoForbiddenRuntimePayload(std::string_view payload) {
  for (const auto token : {"docs" "/execution-plans",
                           "docs" "/findings",
                           "public_audit_summary",
                           "public_release_evidence",
                           "docs/references",
                           "parser_finality_authority\":true",
                           "reference_finality_authority\":true",
                           "wal_recovery_authority\":true"}) {
    Require(!Contains(payload, token),
            "ODF-118 support payload contains forbidden runtime token");
  }
}

void ProveSupportBundleEvidence() {
  auto fixture = CreateAndOpenFreshDatabase("support");
  api::EnginePrepareSupportBundleRequest request;
  request.context = BaseContext(fixture, "odf118-support", 118);
  request.option_envelopes.push_back("engine_authorized_support_export:true");
  request.performance_optimization_snapshot = Odf118SurfaceSnapshot();
  request.performance_optimization_snapshot_present = true;

  const auto result = api::EnginePrepareSupportBundle(request);
  RequireEngineOk(result, "ODF-118 support bundle refused snapshot");
  Require(result.performance_optimization_surface_collected,
          "ODF-118 support bundle omitted optimization surface");
  Require(result.redaction_applied && result.forbidden_fields_absent,
          "ODF-118 support bundle redaction state incomplete");
  Require(Contains(result.support_bundle_json,
                   "\"support_bundle_completeness_state\":\"complete\""),
          "ODF-118 support bundle missing completeness state");
  Require(Contains(result.support_bundle_json,
                   "ENGINE.DBLC_MIGRATION_UNSUPPORTED_NEW_ARTIFACT"),
          "ODF-118 support bundle missing incompatible-open refusal");
  Require(Contains(result.support_bundle_json, "DPC.INDEX_REPAIR.IDENTITY_REFUSED"),
          "ODF-118 support bundle missing UUID-resolution refusal");
  Require(Contains(result.support_bundle_json, "rebuild_from_authoritative_base_pages"),
          "ODF-118 support bundle missing rebuild phase");
  Require(Contains(result.support_bundle_json, "disabled_safe_fallback"),
          "ODF-118 support bundle missing feature-disabled state");
  RequireNoForbiddenRuntimePayload(result.support_bundle_json);
}

}  // namespace

int main() {
  std::cout << kGateSearchKey << '\n';
  ProveCompatibilityAndBackfillContracts();
  (void)CreateAndOpenFreshDatabase("fresh");
  ProveUuidResolutionHasNoHardCodedDependency();
  ProveExistingAndPartialStructureRepairBehavior();
  ProveSupportBundleEvidence();
  return EXIT_SUCCESS;
}
