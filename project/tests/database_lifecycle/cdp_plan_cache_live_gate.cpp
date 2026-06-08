// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "catalog/catalog_object_lifecycle.hpp"
#include "catalog/name_resolution_api.hpp"
#include "database_lifecycle.hpp"
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
namespace uuid = scratchbird::core::uuid;
using scratchbird::core::platform::UuidKind;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) Fail(message);
}

template <typename TResult>
void RequireOk(const TResult& result, std::string_view message) {
  if (!result.ok) {
    if (!result.diagnostics.empty()) {
      std::cerr << result.diagnostics.front().code << ':'
                << result.diagnostics.front().detail << '\n';
    }
    Fail(message);
  }
}

std::uint64_t NowMillis() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

std::string GeneratedUuid(UuidKind kind, std::uint64_t salt) {
  const auto generated = uuid::GenerateEngineIdentityV7(kind, NowMillis() + salt);
  Require(generated.ok(), "UUID generation failed");
  return uuid::UuidToString(generated.value.value);
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

api::EngineRequestContext BaseContext(const std::filesystem::path& path,
                                      const std::string& database_uuid,
                                      std::string principal_uuid) {
  api::EngineRequestContext context;
  context.trust_mode = api::EngineTrustMode::server_isolated;
  context.request_id = "cdp-plan-cache-live-gate";
  context.database_path = path.string();
  context.database_uuid.canonical = database_uuid;
  context.principal_uuid.canonical = std::move(principal_uuid);
  context.session_uuid.canonical = GeneratedUuid(UuidKind::object, 200);
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

api::EngineRequestContext Begin(const std::filesystem::path& path,
                                const std::string& database_uuid,
                                const std::string& principal_uuid) {
  api::EngineBeginTransactionRequest request;
  request.context = BaseContext(path, database_uuid, principal_uuid);
  request.isolation_level = "read_committed";
  const auto begun = api::EngineBeginTransaction(request);
  RequireOk(begun, "begin transaction failed");
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
  RequireOk(api::EngineCommitTransaction(request), "commit transaction failed");
}

void Rollback(const api::EngineRequestContext& context) {
  api::EngineRollbackTransactionRequest request;
  request.context = context;
  RequireOk(api::EngineRollbackTransaction(request), "rollback transaction failed");
}

std::string CreateDatabase(const std::filesystem::path& path) {
  db::DatabaseCreateConfig create;
  create.path = path.string();
  create.database_uuid =
      uuid::GenerateEngineIdentityV7(UuidKind::database, NowMillis() + 301).value;
  create.filespace_uuid =
      uuid::GenerateEngineIdentityV7(UuidKind::filespace, NowMillis() + 302).value;
  create.page_size = 16384;
  create.creation_unix_epoch_millis = NowMillis();
  create.allow_minimal_resource_bootstrap = true;
  create.require_resource_seed_pack = false;
  create.allow_overwrite = true;
  const auto created = db::CreateDatabaseFile(create);
  if (!created.ok()) {
    std::cerr << created.diagnostic.diagnostic_code << ':'
              << created.diagnostic.message_key << '\n';
  }
  Require(created.ok(), "database create failed");
  return uuid::UuidToString(create.database_uuid.value);
}

api::EngineCatalogCreateObjectRequest CreateRequest(
    const api::EngineRequestContext& context,
    const std::string& object_uuid,
    std::string object_kind,
    const std::string& schema_uuid,
    std::string name) {
  api::EngineCatalogCreateObjectRequest request;
  request.context = context;
  request.target_object.uuid.canonical = object_uuid;
  request.target_object.object_kind = std::move(object_kind);
  request.target_schema.uuid.canonical = schema_uuid;
  request.localized_names.push_back(Name(std::move(name)));
  return request;
}

api::EngineResolveNameRequest ResolveRequest(const api::EngineRequestContext& context,
                                             const std::string& schema_uuid,
                                             std::string object_kind,
                                             std::string name) {
  api::EngineResolveNameRequest request;
  request.context = context;
  request.target_schema.uuid.canonical = schema_uuid;
  request.target_object.object_kind = std::move(object_kind);
  request.localized_names.push_back(Name(std::move(name)));
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
                                  const std::string& relation_name) {
  api::EngineQueryRelation relation;
  relation.relation_name = relation_name;
  relation.source_object.uuid.canonical = table_uuid;
  relation.source_object.object_kind = "table";
  relation.descriptor_digest = "generated-descriptor:" + table_uuid;
  for (int value : {1, 2, 3}) {
    api::EngineRowValue row;
    row.fields.push_back({"id", Int64Value(value)});
    relation.rows.push_back(std::move(row));
  }
  return relation;
}

api::EnginePlanOperationRequest PlanRequest(api::EngineRequestContext context,
                                            const std::string& table_uuid,
                                            const std::string& relation_name,
                                            std::string cache_option,
                                            std::string stats_snapshot) {
  api::EnginePlanOperationRequest request;
  request.context = std::move(context);
  request.execute = true;
  request.query_operation = "count";
  request.target_object.uuid.canonical = table_uuid;
  request.target_object.object_kind = "table";
  request.relations.push_back(Relation(table_uuid, relation_name));
  request.option_envelopes.push_back(std::move(cache_option));
  request.option_envelopes.push_back("sblr_digest:cdp-plan-cache-live-count");
  request.option_envelopes.push_back("statistics_snapshot_id:" + std::move(stats_snapshot));
  return request;
}

std::string CountValue(const api::EnginePlanOperationResult& result) {
  Require(!result.result_shape.rows.empty(), "plan result had no rows");
  Require(!result.result_shape.rows.front().fields.empty(), "plan result had no fields");
  return result.result_shape.rows.front().fields.front().second.encoded_value;
}

void RequireSameResult(const api::EnginePlanOperationResult& lhs,
                       const api::EnginePlanOperationResult& rhs,
                       std::string_view message) {
  Require(lhs.output_row_count == rhs.output_row_count, message);
  Require(CountValue(lhs) == CountValue(rhs), message);
}

}  // namespace

int main() {
  const auto path = std::filesystem::temp_directory_path() /
                    ("sb_cdp_plan_cache_live_" + std::to_string(NowMillis()) + ".sbdb");
  const std::string database_uuid = CreateDatabase(path);
  const std::string owner_uuid = GeneratedUuid(UuidKind::object, 310);
  const std::string schema_uuid = GeneratedUuid(UuidKind::object, 311);
  const std::string table_uuid = GeneratedUuid(UuidKind::object, 312);
  const std::string schema_name = "cdp_plan_schema_" + std::to_string(NowMillis());
  const std::string table_name = "cdp_plan_table_" + std::to_string(NowMillis());
  const std::string renamed_table = table_name + "_renamed";

  auto schema_context = Begin(path, database_uuid, owner_uuid);
  const auto created_schema =
      api::EngineCatalogCreateObject(CreateRequest(schema_context, schema_uuid, "schema", {}, schema_name));
  RequireOk(created_schema, "schema create failed");
  Commit(schema_context);

  auto table_context = Begin(path, database_uuid, owner_uuid);
  table_context.catalog_generation_id = created_schema.metadata_cache_epoch;
  const auto created_table =
      api::EngineCatalogCreateObject(CreateRequest(table_context, table_uuid, "table", schema_uuid, table_name));
  RequireOk(created_table, "table create failed");
  Commit(table_context);

  auto read_context = Begin(path, database_uuid, owner_uuid);
  read_context.catalog_generation_id = created_table.metadata_cache_epoch;
  read_context.name_resolution_epoch = created_table.metadata_cache_epoch;
  const auto resolved =
      api::EngineResolveName(ResolveRequest(read_context, schema_uuid, "table", table_name));
  RequireOk(resolved, "resolver lookup failed");
  Require(resolved.bound_object_identity.object_uuid.canonical == table_uuid,
          "plan cache gate did not resolve generated table UUID");

  const auto first = api::EnginePlanOperation(
      PlanRequest(read_context, table_uuid, table_name, "optimizer_plan_cache:enabled", "stats-a"));
  RequireOk(first, "first cached plan request failed");
  Require(HasEvidence(first, "optimizer_live_plan_cache", "miss"),
          "first cached plan request did not miss");
  Require(HasEvidence(first, "optimizer_live_plan_cache_binding", "descriptor:" + table_uuid),
          "plan cache did not bind through descriptor UUID");
  Require(CountValue(first) == "3", "first cached plan changed query result");

  const auto second = api::EnginePlanOperation(
      PlanRequest(read_context, table_uuid, table_name, "optimizer_plan_cache:enabled", "stats-a"));
  RequireOk(second, "second cached plan request failed");
  Require(HasEvidence(second, "optimizer_live_plan_cache", "hit"),
          "second cached plan request did not hit");
  RequireSameResult(first, second, "plan cache hit was not result-preserving");

  const auto stats_changed = api::EnginePlanOperation(
      PlanRequest(read_context, table_uuid, table_name, "optimizer_plan_cache:enabled", "stats-b"));
  RequireOk(stats_changed, "statistics epoch changed plan request failed");
  Require(HasEvidence(stats_changed, "optimizer_live_plan_cache", "miss"),
          "statistics snapshot change did not invalidate plan cache key");
  RequireSameResult(first, stats_changed, "statistics snapshot change was not result-preserving");

  const auto disabled = api::EnginePlanOperation(
      PlanRequest(read_context, table_uuid, table_name, "optimizer_plan_cache:disabled", "stats-a"));
  RequireOk(disabled, "disabled plan cache fallback failed");
  Require(HasEvidence(disabled, "optimizer_live_plan_cache", "disabled"),
          "disabled plan cache fallback did not record evidence");
  RequireSameResult(first, disabled, "disabled plan cache fallback changed result");
  Rollback(read_context);

  auto rename_context = Begin(path, database_uuid, owner_uuid);
  rename_context.catalog_generation_id = created_table.metadata_cache_epoch;
  api::EngineCatalogRenameObjectRequest rename;
  rename.context = rename_context;
  rename.target_object.uuid.canonical = table_uuid;
  rename.target_object.object_kind = "table";
  rename.localized_names.push_back(Name(renamed_table));
  const auto renamed = api::EngineCatalogRenameObject(rename);
  RequireOk(renamed, "table rename failed");
  Require(renamed.metadata_cache_epoch > created_table.metadata_cache_epoch,
          "DDL rename did not advance catalog epoch");
  Commit(rename_context);

  auto after_ddl_context = Begin(path, database_uuid, owner_uuid);
  after_ddl_context.catalog_generation_id = renamed.metadata_cache_epoch;
  after_ddl_context.name_resolution_epoch = renamed.metadata_cache_epoch;
  const auto after_ddl = api::EnginePlanOperation(
      PlanRequest(after_ddl_context, table_uuid, renamed_table, "optimizer_plan_cache:enabled", "stats-a"));
  RequireOk(after_ddl, "DDL epoch changed plan request failed");
  Require(HasEvidence(after_ddl, "optimizer_live_plan_cache", "miss"),
          "DDL catalog epoch change did not invalidate plan cache key");
  RequireSameResult(first, after_ddl, "DDL catalog epoch change was not result-preserving");
  Rollback(after_ddl_context);

  std::filesystem::remove(path);
  return EXIT_SUCCESS;
}
