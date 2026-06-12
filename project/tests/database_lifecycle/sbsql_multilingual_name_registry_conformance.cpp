// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "catalog/catalog_object_lifecycle.hpp"
#include "catalog/name_registry.hpp"
#include "database_lifecycle.hpp"
#include "transaction/transaction_api.hpp"
#include "uuid.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace catalog_api = scratchbird::engine::internal_api;
namespace db = scratchbird::storage::database;
namespace uuid = scratchbird::core::uuid;
using scratchbird::core::platform::UuidKind;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) { Fail(message); }
}

template <typename TResult>
std::string DiagnosticCode(const TResult& result) {
  if (result.diagnostics.empty()) { return {}; }
  return result.diagnostics.front().code;
}

template <typename TResult>
void RequireOk(const TResult& result, std::string_view message) {
  if (!result.ok) { std::cerr << DiagnosticCode(result) << '\n'; }
  Require(result.ok, message);
}

template <typename TResult>
void RequireDiagnostic(const TResult& result,
                       std::string_view expected,
                       std::string_view message) {
  Require(!result.ok, message);
  if (DiagnosticCode(result) != expected) {
    std::cerr << "expected=" << expected << " actual=" << DiagnosticCode(result) << '\n';
  }
  Require(DiagnosticCode(result) == expected, message);
}

void RequireNameOk(const catalog_api::NameRegistryNameResult& result,
                   std::string_view message) {
  if (!result.ok) { std::cerr << result.diagnostic.code << ':' << result.diagnostic.detail << '\n'; }
  Require(result.ok, message);
}

std::uint64_t CurrentUnixMillis() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

std::filesystem::path TestPath() {
  return std::filesystem::temp_directory_path() /
         ("sb_sml_004_name_registry_" + std::to_string(CurrentUnixMillis()) + ".sbdb");
}

std::string CreateDatabase(const std::filesystem::path& path) {
  db::DatabaseCreateConfig create;
  create.path = path.string();
  create.database_uuid = uuid::GenerateEngineIdentityV7(UuidKind::database, 1779810040000).value;
  create.filespace_uuid = uuid::GenerateEngineIdentityV7(UuidKind::filespace, 1779810040001).value;
  create.page_size = 16384;
  create.creation_unix_epoch_millis = 1779810040002;
  create.allow_minimal_resource_bootstrap = true;
  create.require_resource_seed_pack = false;
  create.allow_overwrite = true;
  const auto created = db::CreateDatabaseFile(create);
  if (!created.ok()) {
    std::cerr << created.diagnostic.diagnostic_code << ':' << created.diagnostic.message_key << '\n';
  }
  Require(created.ok(), "SML-004 database create failed");
  return uuid::UuidToString(create.database_uuid.value);
}

catalog_api::EngineRequestContext BaseContext(const std::filesystem::path& path,
                                              const std::string& database_uuid,
                                              std::string principal,
                                              std::string language,
                                              std::string default_language) {
  catalog_api::EngineRequestContext context;
  context.trust_mode = catalog_api::EngineTrustMode::server_isolated;
  context.request_id = "sml-004-name-registry-conformance";
  context.database_path = path.string();
  context.database_uuid.canonical = database_uuid;
  context.principal_uuid.canonical = std::move(principal);
  context.session_uuid.canonical = "session-" + std::to_string(CurrentUnixMillis());
  context.security_context_present = true;
  context.identifier_profile_uuid = "sbsql_v3";
  context.language_context.language_tag = std::move(language);
  context.language_context.default_language_tag = std::move(default_language);
  context.catalog_generation_id = 1;
  context.security_epoch = 1;
  context.resource_epoch = 1;
  context.name_resolution_epoch = 1;
  return context;
}

catalog_api::EngineRequestContext Begin(const std::filesystem::path& path,
                                        const std::string& database_uuid,
                                        std::string principal,
                                        std::string language = "en",
                                        std::string default_language = "en") {
  catalog_api::EngineBeginTransactionRequest request;
  request.context = BaseContext(path,
                                database_uuid,
                                std::move(principal),
                                std::move(language),
                                std::move(default_language));
  request.isolation_level = "read_committed";
  const auto begun = catalog_api::EngineBeginTransaction(request);
  if (!begun.ok) {
    for (const auto& diagnostic : begun.diagnostics) {
      std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
    }
  }
  Require(begun.ok, "begin transaction failed");
  auto context = request.context;
  context.local_transaction_id = begun.local_transaction_id;
  context.transaction_uuid = begun.transaction_uuid;
  context.snapshot_visible_through_local_transaction_id =
      begun.snapshot_visible_through_local_transaction_id;
  context.transaction_isolation_level = begun.isolation_level;
  return context;
}

void Commit(const catalog_api::EngineRequestContext& context) {
  catalog_api::EngineCommitTransactionRequest request;
  request.context = context;
  const auto committed = catalog_api::EngineCommitTransaction(request);
  if (!committed.ok) {
    for (const auto& diagnostic : committed.diagnostics) {
      std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
    }
  }
  Require(committed.ok, "commit transaction failed");
}

void Rollback(const catalog_api::EngineRequestContext& context) {
  catalog_api::EngineRollbackTransactionRequest request;
  request.context = context;
  const auto rolled_back = catalog_api::EngineRollbackTransaction(request);
  Require(rolled_back.ok, "rollback transaction failed");
}

catalog_api::EngineLocalizedName LocalizedName(std::string language,
                                               std::string name_class,
                                               std::string value,
                                               bool default_name) {
  catalog_api::EngineLocalizedName name;
  name.language_tag = std::move(language);
  name.name_class = std::move(name_class);
  name.name = value;
  name.raw_name_text = value;
  name.display_name = std::move(value);
  name.default_name = default_name;
  return name;
}

catalog_api::EngineLocalizedName PrimaryName(std::string language, std::string value) {
  return LocalizedName(std::move(language), "primary", std::move(value), true);
}

catalog_api::EngineLocalizedName AliasName(std::string language, std::string value) {
  return LocalizedName(std::move(language), "alias", std::move(value), false);
}

catalog_api::EngineCatalogCreateObjectRequest CreateRequest(
    const catalog_api::EngineRequestContext& context,
    std::string object_uuid,
    std::string object_kind,
    std::string schema_uuid,
    std::vector<catalog_api::EngineLocalizedName> names) {
  catalog_api::EngineCatalogCreateObjectRequest request;
  request.context = context;
  request.target_object.uuid.canonical = std::move(object_uuid);
  request.target_object.object_kind = std::move(object_kind);
  request.target_schema.uuid.canonical = std::move(schema_uuid);
  request.localized_names = std::move(names);
  return request;
}

catalog_api::EngineCatalogResolveObjectNameRequest ResolveRequest(
    const catalog_api::EngineRequestContext& context,
    std::string object_kind,
    std::string schema_uuid,
    std::string language,
    std::string name) {
  catalog_api::EngineCatalogResolveObjectNameRequest request;
  request.context = context;
  request.target_object.object_kind = std::move(object_kind);
  request.target_schema.uuid.canonical = std::move(schema_uuid);
  request.localized_names.push_back(LocalizedName(std::move(language), "primary", std::move(name), true));
  return request;
}

void CreateSchema(const std::filesystem::path& path, const std::string& database_uuid) {
  auto context = Begin(path, database_uuid, "principal-owner");
  const auto created = catalog_api::EngineCatalogCreateObject(
      CreateRequest(context, "schema-app", "schema", "", {PrimaryName("en", "app")}));
  RequireOk(created, "schema create failed");
  Commit(context);
}

void TestDefaultLanguageCollisionPolicy(const std::filesystem::path& path,
                                        const std::string& database_uuid) {
  auto create_orders_context = Begin(path, database_uuid, "principal-owner");
  const auto created_orders = catalog_api::EngineCatalogCreateObject(
      CreateRequest(create_orders_context,
                    "table-orders",
                    "table",
                    "schema-app",
                    {PrimaryName("en", "orders"),
                     AliasName("fr", "commandes"),
                     AliasName("es", "orders")}));
  RequireOk(created_orders, "same-object localized alias reuse was rejected");
  Commit(create_orders_context);

  auto resolve_same_object_context = Begin(path, database_uuid, "principal-owner", "es", "en");
  const auto resolved_same_object = catalog_api::EngineCatalogResolveObjectName(
      ResolveRequest(resolve_same_object_context, "table", "schema-app", "es", "orders"));
  RequireOk(resolved_same_object, "same-object alternate alias did not resolve");
  Require(resolved_same_object.bound_object_identity.object_uuid.canonical == "table-orders",
          "same-object alternate alias resolved to the wrong object");
  Rollback(resolve_same_object_context);

  auto blocked_default_context = Begin(path, database_uuid, "principal-owner");
  RequireDiagnostic(
      catalog_api::EngineCatalogCreateObject(
          CreateRequest(blocked_default_context,
                        "table-default-collision",
                        "table",
                        "schema-app",
                        {PrimaryName("en", "commandes")})),
      catalog_api::kCatalogObjectDiagnosticDuplicateName,
      "default-language canonical name was allowed to collide with an alternate alias");
  Rollback(blocked_default_context);

  auto blocked_alias_context = Begin(path, database_uuid, "principal-owner", "fr", "en");
  RequireDiagnostic(
      catalog_api::EngineCatalogCreateObject(
          CreateRequest(blocked_alias_context,
                        "table-alias-collision",
                        "table",
                        "schema-app",
                        {PrimaryName("en", "invoices"), AliasName("fr", "orders")})),
      catalog_api::kCatalogObjectDiagnosticDuplicateName,
      "alternate-language alias was allowed to bind another object's canonical name");
  Rollback(blocked_alias_context);

  auto blocked_cross_language_alias_context =
      Begin(path, database_uuid, "principal-owner", "de", "en");
  RequireDiagnostic(
      catalog_api::EngineCatalogCreateObject(
          CreateRequest(blocked_cross_language_alias_context,
                        "table-cross-language-alias-collision",
                        "table",
                        "schema-app",
                        {PrimaryName("en", "shipments"),
                         AliasName("de", "commandes")})),
      catalog_api::kCatalogObjectDiagnosticDuplicateName,
      "alternate-language alias was allowed to bind a different object identity");
  Rollback(blocked_cross_language_alias_context);
}

void TestDeterministicFallbackDisplay(const std::filesystem::path& path,
                                      const std::string& database_uuid) {
  auto create_context = Begin(path, database_uuid, "principal-owner");
  const auto created = catalog_api::EngineCatalogCreateObject(
      CreateRequest(create_context,
                    "table-fallback",
                    "table",
                    "schema-app",
                    {LocalizedName("fr", "primary", "nom_fr", false),
                     LocalizedName("es", "primary", "nombre_es", false)}));
  RequireOk(created, "fallback display probe create failed");
  Commit(create_context);

  auto map_context = Begin(path, database_uuid, "principal-owner", "de", "en");
  catalog_api::EngineApiRequest map_request;
  map_request.context = map_context;
  map_request.target_object.uuid.canonical = "table-fallback";
  map_request.target_object.object_kind = "table";
  const auto mapped = catalog_api::MapNameRegistryUuidToName(map_request, "table-fallback", "table");
  RequireNameOk(mapped, "fallback UUID-to-name mapping failed");
  Require(mapped.entry.language_tag == "es", "fallback display did not choose deterministic language order");
  Require(mapped.entry.display_name == "nombre_es", "fallback display returned the wrong localized name");
  Rollback(map_context);
}

void Cleanup(const std::filesystem::path& path) {
  std::error_code ignored;
  std::filesystem::remove(path, ignored);
  std::filesystem::remove(path.string() + ".sb.api_events", ignored);
  std::filesystem::remove(path.string() + ".sb.catalog_object_events", ignored);
  std::filesystem::remove(path.string() + ".sb.crud_events", ignored);
  std::filesystem::remove(path.string() + ".sb.name_events", ignored);
}

}  // namespace

int main() {
  const auto path = TestPath();
  const auto database_uuid = CreateDatabase(path);
  CreateSchema(path, database_uuid);
  TestDefaultLanguageCollisionPolicy(path, database_uuid);
  TestDeterministicFallbackDisplay(path, database_uuid);
  Cleanup(path);
  return EXIT_SUCCESS;
}
