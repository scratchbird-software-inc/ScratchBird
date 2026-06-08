// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "catalog/catalog_object_lifecycle.hpp"
#include "catalog/descriptor_api.hpp"
#include "catalog/name_resolution_api.hpp"
#include "database_lifecycle.hpp"
#include "database_format.hpp"
#include "query/plan_api.hpp"
#include "transaction/transaction_api.hpp"
#include "uuid.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>

namespace {

namespace api = scratchbird::engine::internal_api;
namespace db = scratchbird::storage::database;
namespace disk = scratchbird::storage::disk;
namespace uuid = scratchbird::core::uuid;
using scratchbird::core::platform::TypedUuid;
using scratchbird::core::platform::UuidKind;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) Fail(message);
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

std::uint64_t NowMillis() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

struct UuidFactory {
  std::uint64_t base_millis = NowMillis();

  TypedUuid Typed(UuidKind kind, std::uint64_t salt) const {
    const auto generated =
        uuid::GenerateEngineIdentityV7(kind, base_millis + salt);
    Require(generated.ok(), "CDP-024 UUID generation failed");
    return generated.value;
  }

  std::string Text(UuidKind kind, std::uint64_t salt) const {
    return uuid::UuidToString(Typed(kind, salt).value);
  }
};

struct TempFixture {
  std::filesystem::path dir;
  std::filesystem::path database_path;
  UuidFactory uuids;
  TypedUuid database_uuid;
  TypedUuid filespace_uuid;
  std::string database_uuid_text;
  std::string filespace_uuid_text;
  std::string principal_uuid;
  std::string schema_uuid;
  std::string table_uuid;
  std::string schema_name;
  std::string table_name;
  std::uint64_t table_metadata_epoch = 0;

  ~TempFixture() {
    std::error_code ignored;
    if (!dir.empty()) std::filesystem::remove_all(dir, ignored);
  }
};

bool HasEvidence(const api::EngineApiResult& result,
                 std::string_view kind,
                 std::string_view value) {
  for (const auto& evidence : result.evidence) {
    if (evidence.evidence_kind == kind && evidence.evidence_id == value) {
      return true;
    }
  }
  return false;
}

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

api::EngineRequestContext BaseContext(const TempFixture& fixture,
                                      std::string request_id,
                                      std::uint64_t epoch) {
  api::EngineRequestContext context;
  context.trust_mode = api::EngineTrustMode::server_isolated;
  context.request_id = std::move(request_id);
  context.database_path = fixture.database_path.string();
  context.database_uuid.canonical = fixture.database_uuid_text;
  context.principal_uuid.canonical = fixture.principal_uuid;
  context.session_uuid.canonical = fixture.uuids.Text(UuidKind::object, 1000 + epoch);
  context.security_context_present = true;
  context.identifier_profile_uuid = "sbsql_v3";
  context.language_context.language_tag = "en";
  context.language_context.default_language_tag = "en";
  context.catalog_generation_id = epoch;
  context.security_epoch = epoch;
  context.resource_epoch = epoch;
  context.name_resolution_epoch = epoch;
  return context;
}

api::EngineRequestContext Begin(const TempFixture& fixture,
                                std::string request_id,
                                std::uint64_t epoch) {
  api::EngineBeginTransactionRequest request;
  request.context = BaseContext(fixture, std::move(request_id), epoch);
  request.isolation_level = "read_committed";
  const auto begun = api::EngineBeginTransaction(request);
  RequireEngineOk(begun, "CDP-024 begin transaction failed");
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
                  "CDP-024 commit transaction failed");
}

void Rollback(const api::EngineRequestContext& context) {
  api::EngineRollbackTransactionRequest request;
  request.context = context;
  RequireEngineOk(api::EngineRollbackTransaction(request),
                  "CDP-024 rollback transaction failed");
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
    const std::string& table_uuid,
    std::string cache_option) {
  api::EngineGetDescriptorRequest request;
  request.context = context;
  request.target_object.uuid.canonical = table_uuid;
  request.target_object.object_kind = "table";
  request.option_envelopes.push_back(std::move(cache_option));
  return request;
}

api::EngineTypedValue Int64Value(std::int64_t value) {
  api::EngineTypedValue typed;
  typed.descriptor.descriptor_kind = "scalar";
  typed.descriptor.canonical_type_name = "int64";
  typed.descriptor.encoded_descriptor = "canonical=int64";
  typed.encoded_value = std::to_string(value);
  return typed;
}

api::EngineQueryRelation Relation(const std::string& table_uuid,
                                  const std::string& table_name) {
  api::EngineQueryRelation relation;
  relation.relation_name = table_name;
  relation.source_object.uuid.canonical = table_uuid;
  relation.source_object.object_kind = "table";
  relation.descriptor_digest = "descriptor:" + table_uuid;
  for (int value : {10, 20, 30}) {
    api::EngineRowValue row;
    row.fields.push_back({"id", Int64Value(value)});
    relation.rows.push_back(std::move(row));
  }
  return relation;
}

api::EnginePlanOperationRequest PlanRequest(api::EngineRequestContext context,
                                            const std::string& table_uuid,
                                            const std::string& table_name,
                                            std::string cache_option,
                                            std::string sblr_digest,
                                            std::string stats_snapshot) {
  api::EnginePlanOperationRequest request;
  request.context = std::move(context);
  request.execute = true;
  request.query_operation = "count";
  request.target_object.uuid.canonical = table_uuid;
  request.target_object.object_kind = "table";
  request.relations.push_back(Relation(table_uuid, table_name));
  request.option_envelopes.push_back(std::move(cache_option));
  request.option_envelopes.push_back("sblr_digest:" + std::move(sblr_digest));
  request.option_envelopes.push_back("statistics_snapshot_id:" +
                                     std::move(stats_snapshot));
  return request;
}

std::string CountValue(const api::EnginePlanOperationResult& result) {
  Require(!result.result_shape.rows.empty(), "CDP-024 plan result had no rows");
  Require(!result.result_shape.rows.front().fields.empty(),
          "CDP-024 plan result had no fields");
  return result.result_shape.rows.front().fields.front().second.encoded_value;
}

void RequirePlanCacheEvidence(const api::EnginePlanOperationResult& result,
                              std::string_view cache_state) {
  Require(HasEvidence(result, "optimizer_live_plan_cache", cache_state),
          "CDP-024 optimizer live plan cache evidence mismatch");
  Require(HasEvidence(result, "parser_executes_sql", "false"),
          "CDP-024 plan cache evidence omitted parser_executes_sql=false");
  Require(HasEvidence(result, "parser_claims_transaction_finality", "false"),
          "CDP-024 plan cache evidence omitted parser finality refusal");
}

db::DatabaseArtifactVersionCompatibilityRequest VersionRequest(
    std::string artifact_kind,
    std::uint32_t major,
    std::uint32_t minor,
    std::uint32_t min_major,
    std::uint32_t min_minor,
    std::uint32_t current_major,
    std::uint32_t current_minor,
    std::uint32_t max_major,
    std::uint32_t max_minor) {
  db::DatabaseArtifactVersionCompatibilityRequest request;
  request.artifact_kind = std::move(artifact_kind);
  request.format_major = major;
  request.format_minor = minor;
  request.min_supported_major = min_major;
  request.min_supported_minor = min_minor;
  request.current_major = current_major;
  request.current_minor = current_minor;
  request.max_supported_major = max_major;
  request.max_supported_minor = max_minor;
  return request;
}

void RequireCompatibilityClass(const db::DatabaseArtifactCompatibilityResult& result,
                               db::DatabaseOpenCompatibilityClass expected,
                               std::string_view expected_code,
                               std::string_view message) {
  Require(!result.ok(), message);
  if (result.compatibility_class != expected ||
      result.diagnostic.diagnostic_code != expected_code) {
    std::cerr << "expected_class="
              << db::DatabaseOpenCompatibilityClassName(expected)
              << " actual_class="
              << db::DatabaseOpenCompatibilityClassName(result.compatibility_class)
              << " expected_code=" << expected_code
              << " actual_code=" << result.diagnostic.diagnostic_code << '\n';
  }
  Require(result.compatibility_class == expected, message);
  Require(result.diagnostic.diagnostic_code == expected_code, message);
}

TempFixture CreateDatabaseFixture() {
  TempFixture fixture;
  fixture.dir = std::filesystem::temp_directory_path() /
                ("scratchbird_cdp024_" + std::to_string(NowMillis()));
  std::filesystem::create_directories(fixture.dir);
  fixture.database_path = fixture.dir / "cdp024.sbdb";
  fixture.database_uuid = fixture.uuids.Typed(UuidKind::database, 10);
  fixture.filespace_uuid = fixture.uuids.Typed(UuidKind::filespace, 11);
  fixture.database_uuid_text = uuid::UuidToString(fixture.database_uuid.value);
  fixture.filespace_uuid_text = uuid::UuidToString(fixture.filespace_uuid.value);
  fixture.principal_uuid = fixture.uuids.Text(UuidKind::principal, 12);
  fixture.schema_uuid = fixture.uuids.Text(UuidKind::object, 13);
  fixture.table_uuid = fixture.uuids.Text(UuidKind::object, 14);
  fixture.schema_name = "cdp024_schema_" + std::to_string(fixture.uuids.base_millis);
  fixture.table_name = "cdp024_table_" + std::to_string(fixture.uuids.base_millis);

  db::DatabaseCreateConfig create;
  create.path = fixture.database_path.string();
  create.database_uuid = fixture.database_uuid;
  create.filespace_uuid = fixture.filespace_uuid;
  create.page_size = 16384;
  create.creation_unix_epoch_millis = fixture.uuids.base_millis;
  create.allow_minimal_resource_bootstrap = true;
  create.require_resource_seed_pack = false;
  create.allow_overwrite = true;
  const auto created = db::CreateDatabaseFile(create);
  RequireLifecycleOk(created, "CDP-024 database create failed");
  return fixture;
}

db::DatabaseLifecycleResult ReopenReadOnly(const TempFixture& fixture) {
  db::DatabaseOpenConfig open;
  open.path = fixture.database_path.string();
  open.read_only = true;
  return db::OpenDatabaseFile(open);
}

void ProvePersistentHeaderCompatibility(const TempFixture& fixture) {
  const auto reopened = ReopenReadOnly(fixture);
  RequireLifecycleOk(reopened, "CDP-024 current database reopen failed");
  Require(reopened.state.database_open_compatibility_class ==
              db::DatabaseOpenCompatibilityClass::current,
          "CDP-024 current database did not report current compatibility");
  Require(reopened.state.header.format_major ==
              disk::kScratchBirdDatabaseFormatMajor,
          "CDP-024 reopened header major format mismatch");
  Require(reopened.state.header.format_minor ==
              disk::kScratchBirdDatabaseFormatMinor,
          "CDP-024 reopened header minor format mismatch");
  Require(uuid::UuidToString(reopened.state.database_uuid.value) ==
              fixture.database_uuid_text,
          "CDP-024 reopened database UUID drifted");
  Require(uuid::UuidToString(reopened.state.filespace_uuid.value) ==
              fixture.filespace_uuid_text,
          "CDP-024 reopened filespace UUID drifted");
  Require(db::ClassifyDatabaseOpenCompatibility(reopened.state.header, true) ==
              db::DatabaseOpenCompatibilityClass::current,
          "CDP-024 reopened header classifier did not report current");
}

void ExerciseCompatibilityClassifiers() {
  auto current = VersionRequest("database_header", 1, 0, 1, 0, 1, 0, 1, 0);
  const auto current_result = db::ClassifyDatabaseArtifactVersionCompatibility(current);
  Require(current_result.ok() &&
              current_result.compatibility_class ==
                  db::DatabaseOpenCompatibilityClass::current,
          "CDP-024 current database artifact version was not accepted");

  auto old_without_plan =
      VersionRequest("database_header", 1, 0, 1, 0, 1, 1, 1, 1);
  RequireCompatibilityClass(
      db::ClassifyDatabaseArtifactVersionCompatibility(old_without_plan),
      db::DatabaseOpenCompatibilityClass::migration_required_without_plan_refused,
      "ENGINE.DBLC_MIGRATION_REQUIRED_WITHOUT_PLAN",
      "CDP-024 old database artifact did not require explicit upgrade plan");

  auto old_with_plan = old_without_plan;
  old_with_plan.migration_plan_id =
      "database_header_v1_0_to_v1_1_explicit_plan_v1";
  const auto old_plan_result =
      db::ClassifyDatabaseArtifactVersionCompatibility(old_with_plan);
  Require(old_plan_result.ok() &&
              old_plan_result.compatibility_class ==
                  db::DatabaseOpenCompatibilityClass::supported_migration &&
              old_plan_result.migration_required,
          "CDP-024 explicit old-format upgrade plan was not classified");

  auto future_unsupported =
      VersionRequest("database_header", 1, 1, 1, 0, 1, 0, 1, 1);
  RequireCompatibilityClass(
      db::ClassifyDatabaseArtifactVersionCompatibility(future_unsupported),
      db::DatabaseOpenCompatibilityClass::unsupported_new,
      "ENGINE.DBLC_MIGRATION_UNSUPPORTED_NEW_ARTIFACT",
      "CDP-024 future unsupported database artifact was not refused");

  auto downgrade = current;
  downgrade.downgrade_requested = true;
  RequireCompatibilityClass(
      db::ClassifyDatabaseArtifactVersionCompatibility(downgrade),
      db::DatabaseOpenCompatibilityClass::downgrade_refused,
      "ENGINE.DBLC_FORMAT_DOWNGRADE_REFUSED",
      "CDP-024 downgrade database artifact was not refused");

  db::DatabaseCatalogMigrationEvidence catalog_current;
  const auto catalog_current_result =
      db::ClassifyDatabaseCatalogMigrationEvidence(catalog_current);
  Require(catalog_current_result.ok() &&
              catalog_current_result.compatibility_class ==
                  db::DatabaseOpenCompatibilityClass::current,
          "CDP-024 current catalog/resource manifest evidence was not accepted");

  auto old_catalog_manifest = catalog_current;
  old_catalog_manifest.filespace_catalog_manifest_format_version = 0;
  RequireCompatibilityClass(
      db::ClassifyDatabaseCatalogMigrationEvidence(old_catalog_manifest),
      db::DatabaseOpenCompatibilityClass::migration_required_without_plan_refused,
      "ENGINE.DBLC_MIGRATION_REQUIRED_WITHOUT_PLAN",
      "CDP-024 old filespace catalog manifest did not require migration plan");

  old_catalog_manifest.migration_plan_id =
      "filespace_catalog_manifest_v0_0_to_v1_0_explicit_plan_v1";
  const auto old_catalog_plan_result =
      db::ClassifyDatabaseCatalogMigrationEvidence(old_catalog_manifest);
  Require(old_catalog_plan_result.ok() &&
              old_catalog_plan_result.compatibility_class ==
                  db::DatabaseOpenCompatibilityClass::supported_migration &&
              old_catalog_plan_result.migration_required,
          "CDP-024 old catalog manifest explicit migration plan was not accepted");

  auto future_resource_manifest = catalog_current;
  future_resource_manifest.resource_seed_manifest_format_version = 2;
  RequireCompatibilityClass(
      db::ClassifyDatabaseCatalogMigrationEvidence(future_resource_manifest),
      db::DatabaseOpenCompatibilityClass::newer_than_supported_refused,
      "ENGINE.DBLC_FORMAT_NEWER_THAN_SUPPORTED",
      "CDP-024 future resource seed manifest was not refused");

  auto ambiguous_catalog = catalog_current;
  ambiguous_catalog.database_catalog_record_count = 2;
  RequireCompatibilityClass(
      db::ClassifyDatabaseCatalogMigrationEvidence(ambiguous_catalog),
      db::DatabaseOpenCompatibilityClass::ambiguous_identity_refused,
      "ENGINE.DBLC_MIGRATION_AMBIGUOUS_IDENTITY_REFUSED",
      "CDP-024 ambiguous catalog identity was not refused");
}

void CreateCatalogObjects(TempFixture* fixture) {
  auto schema_context = Begin(*fixture, "cdp024-create-schema", 1);
  const auto created_schema = api::EngineCatalogCreateObject(
      CreateObjectRequest(schema_context,
                          fixture->schema_uuid,
                          "schema",
                          {},
                          fixture->schema_name));
  RequireEngineOk(created_schema, "CDP-024 schema create failed");
  Commit(schema_context);

  auto table_context = Begin(*fixture,
                             "cdp024-create-table",
                             created_schema.metadata_cache_epoch);
  const auto created_table = api::EngineCatalogCreateObject(
      CreateObjectRequest(table_context,
                          fixture->table_uuid,
                          "table",
                          fixture->schema_uuid,
                          fixture->table_name));
  RequireEngineOk(created_table, "CDP-024 table create failed");
  Require(created_table.primary_object.uuid.canonical == fixture->table_uuid,
          "CDP-024 table create did not preserve generated table UUID");
  fixture->table_metadata_epoch = created_table.metadata_cache_epoch;
  Commit(table_context);
}

void ExerciseDescriptorCacheEpoch(TempFixture& fixture,
                                  std::uint64_t epoch,
                                  std::string_view label,
                                  std::string* encoded_descriptor) {
  auto read_context = Begin(fixture, std::string(label) + "-descriptor", epoch);
  const auto resolved = api::EngineResolveName(
      ResolveRequest(read_context, fixture.schema_uuid, "table", fixture.table_name));
  RequireEngineOk(resolved, "CDP-024 reopened resolver lookup failed");
  Require(resolved.bound_object_identity.object_uuid.canonical == fixture.table_uuid,
          "CDP-024 resolver did not return generated table UUID");

  const auto miss = api::EngineGetDescriptor(
      DescriptorRequest(read_context, fixture.table_uuid, "descriptor_cache:enabled"));
  RequireEngineOk(miss, "CDP-024 descriptor cache miss lookup failed");
  Require(HasEvidence(miss, "descriptor_metadata_cache", "miss"),
          "CDP-024 descriptor cache did not record first miss");
  Require(miss.descriptor.descriptor_uuid.canonical == fixture.table_uuid,
          "CDP-024 descriptor did not bind generated table UUID");
  if (encoded_descriptor != nullptr && encoded_descriptor->empty()) {
    *encoded_descriptor = miss.descriptor.encoded_descriptor;
  } else if (encoded_descriptor != nullptr) {
    Require(*encoded_descriptor == miss.descriptor.encoded_descriptor,
            "CDP-024 descriptor rebuild changed descriptor payload");
  }

  const auto hit = api::EngineGetDescriptor(
      DescriptorRequest(read_context, fixture.table_uuid, "descriptor_cache:enabled"));
  RequireEngineOk(hit, "CDP-024 descriptor cache hit lookup failed");
  Require(HasEvidence(hit, "descriptor_metadata_cache", "hit"),
          "CDP-024 descriptor cache did not record second hit");
  Require(hit.descriptor.encoded_descriptor == miss.descriptor.encoded_descriptor,
          "CDP-024 descriptor cache hit changed descriptor payload");
  Rollback(read_context);
}

void ExercisePlanCacheEpoch(TempFixture& fixture,
                            std::uint64_t epoch,
                            std::string_view label,
                            std::string* count_value) {
  auto plan_context = Begin(fixture, std::string(label) + "-plan", epoch);
  const std::string digest =
      "cdp024-count-" + std::to_string(fixture.uuids.base_millis);
  const std::string stats_snapshot = "cdp024-statistics-snapshot";
  const auto miss = api::EnginePlanOperation(
      PlanRequest(plan_context,
                  fixture.table_uuid,
                  fixture.table_name,
                  "optimizer_plan_cache:enabled",
                  digest,
                  stats_snapshot));
  RequireEngineOk(miss, "CDP-024 optimizer plan cache miss request failed");
  RequirePlanCacheEvidence(miss, "miss");
  Require(HasEvidence(miss, "optimizer_live_plan_cache_binding",
                      "descriptor:" + fixture.table_uuid),
          "CDP-024 plan cache did not bind descriptor UUID");
  Require(CountValue(miss) == "3",
          "CDP-024 optimizer plan cache miss changed query result");
  if (count_value != nullptr && count_value->empty()) {
    *count_value = CountValue(miss);
  } else if (count_value != nullptr) {
    Require(*count_value == CountValue(miss),
            "CDP-024 plan cache rebuild changed query result");
  }

  const auto hit = api::EnginePlanOperation(
      PlanRequest(plan_context,
                  fixture.table_uuid,
                  fixture.table_name,
                  "optimizer_plan_cache:enabled",
                  digest,
                  stats_snapshot));
  RequireEngineOk(hit, "CDP-024 optimizer plan cache hit request failed");
  RequirePlanCacheEvidence(hit, "hit");
  Require(CountValue(hit) == CountValue(miss),
          "CDP-024 optimizer plan cache hit changed query result");
  Rollback(plan_context);
}

void ProveTransientRebuildAfterReopen(TempFixture& fixture) {
  CreateCatalogObjects(&fixture);

  std::string descriptor_payload;
  std::string count_value;
  ExerciseDescriptorCacheEpoch(fixture,
                               fixture.table_metadata_epoch,
                               "cdp024-before-reopen",
                               &descriptor_payload);
  ExercisePlanCacheEpoch(fixture,
                         fixture.table_metadata_epoch,
                         "cdp024-before-reopen",
                         &count_value);

  ProvePersistentHeaderCompatibility(fixture);

  const std::uint64_t reopen_epoch = fixture.table_metadata_epoch + 101;
  ExerciseDescriptorCacheEpoch(fixture,
                               reopen_epoch,
                               "cdp024-after-reopen",
                               &descriptor_payload);
  ExercisePlanCacheEpoch(fixture,
                         reopen_epoch,
                         "cdp024-after-reopen",
                         &count_value);
}

}  // namespace

int main() {
  // Search key: CDP_PERSISTED_FORMAT_TRANSIENT_REBUILD_GATE
  auto fixture = CreateDatabaseFixture();
  ProvePersistentHeaderCompatibility(fixture);
  ExerciseCompatibilityClassifiers();
  ProveTransientRebuildAfterReopen(fixture);
  return EXIT_SUCCESS;
}
