#include "../../support/binary_uuid_fixture.hpp"
#include "../../support/engine_evidence_fixture.hpp"
// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "catalog/catalog_object_lifecycle.hpp"
#include "catalog/name_resolution_api.hpp"
#include "catalog/sys_information_projection.hpp"
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

std::uint64_t CurrentUnixMillis() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

std::filesystem::path TestPath(std::string_view label) {
  return std::filesystem::temp_directory_path() /
         ("sb_catalog_synonym_" + std::string(label) + "_" +
          std::to_string(CurrentUnixMillis()) + ".sbdb");
}

catalog_api::EngineUuid CreateDatabase(const std::filesystem::path& path, std::uint64_t seed) {
  db::DatabaseCreateConfig create;
  create.path = path.string();
  create.database_uuid = uuid::GenerateEngineIdentityV7(UuidKind::database, seed).value;
  create.filespace_uuid = uuid::GenerateEngineIdentityV7(UuidKind::filespace, seed + 1).value;
  create.page_size = 16384;
  create.creation_unix_epoch_millis = seed + 2;
  create.allow_minimal_resource_bootstrap = true;
  create.require_resource_seed_pack = false;
  create.allow_overwrite = true;
  const auto created = db::CreateDatabaseFile(create);
  if (!created.ok()) {
    std::cerr << created.diagnostic.diagnostic_code << ":" << created.diagnostic.message_key << '\n';
  }
  Require(created.ok(), "catalog synonym database create failed");
  return create.database_uuid.value;
}

catalog_api::EngineRequestContext BaseContext(const std::filesystem::path& path,
                                              const catalog_api::EngineUuid& database_uuid) {
  catalog_api::EngineRequestContext context;
  context.trust_mode = catalog_api::EngineTrustMode::server_isolated;
  context.request_id = "catalog-synonym-conformance";
  context.database_path = path.string();
  context.database_uuid = database_uuid;
  context.principal_uuid = scratchbird::tests::FixtureUuid(1208, 901);
  const auto session = uuid::GenerateEngineIdentityV7(UuidKind::object, CurrentUnixMillis());
  Require(session.ok(), "session identity generation failed");
  context.session_uuid = session.value.value;
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

catalog_api::EngineRequestContext Begin(const std::filesystem::path& path,
                                        const catalog_api::EngineUuid& database_uuid) {
  catalog_api::EngineBeginTransactionRequest request;
  request.context = BaseContext(path, database_uuid);
  request.isolation_level = "read_committed";
  const auto begun = catalog_api::EngineBeginTransaction(request);
  if (!begun.ok) {
    for (const auto& diagnostic : begun.diagnostics) {
      std::cerr << diagnostic.code << ":" << diagnostic.detail << '\n';
    }
  }
  Require(begun.ok, "begin transaction failed");
  auto context = request.context;
  context.local_transaction_id = begun.local_transaction_id;
  context.transaction_uuid = begun.transaction_uuid;
  context.snapshot_visible_through_local_transaction_id = begun.snapshot_visible_through_local_transaction_id;
  context.transaction_isolation_level = begun.isolation_level;
  return context;
}

void Commit(const catalog_api::EngineRequestContext& context) {
  catalog_api::EngineCommitTransactionRequest request;
  request.context = context;
  const auto committed = catalog_api::EngineCommitTransaction(request);
  Require(committed.ok, "commit transaction failed");
}

void Rollback(const catalog_api::EngineRequestContext& context) {
  catalog_api::EngineRollbackTransactionRequest request;
  request.context = context;
  const auto rolled_back = catalog_api::EngineRollbackTransaction(request);
  Require(rolled_back.ok, "rollback transaction failed");
}

catalog_api::EngineLocalizedName Name(std::string value) {
  catalog_api::EngineLocalizedName name;
  name.language_tag = "en";
  name.name_class = "primary";
  name.name = value;
  name.raw_name_text = value;
  name.display_name = value;
  name.default_name = true;
  return name;
}

catalog_api::EngineCatalogCreateObjectRequest CreateRequest(
    const catalog_api::EngineRequestContext& context,
    catalog_api::EngineUuid object_uuid,
    std::string object_kind,
    catalog_api::EngineUuid parent_uuid,
    std::string name) {
  catalog_api::EngineCatalogCreateObjectRequest request;
  request.context = context;
  request.target_object.uuid = std::move(object_uuid);
  request.target_object.object_kind = std::move(object_kind);
  request.target_schema.uuid = std::move(parent_uuid);
  request.localized_names.push_back(Name(std::move(name)));
  return request;
}

catalog_api::EngineCatalogCreateObjectRequest SynonymRequest(
    const catalog_api::EngineRequestContext& context,
    catalog_api::EngineUuid synonym_uuid,
    catalog_api::EngineUuid parent_uuid,
    std::string name,
    catalog_api::EngineUuid target_uuid,
    std::string target_class) {
  auto request = CreateRequest(context, std::move(synonym_uuid), "synonym", std::move(parent_uuid), std::move(name));
  request.related_objects.push_back({{std::move(target_uuid)}, std::move(target_class)});
  return request;
}

void CommitCreate(const std::filesystem::path& path,
                  const catalog_api::EngineUuid& database_uuid,
                  catalog_api::EngineCatalogCreateObjectRequest request) {
  auto context = Begin(path, database_uuid);
  request.context = context;
  RequireOk(catalog_api::EngineCatalogCreateObject(request), "catalog object create failed");
  Commit(context);
}

catalog_api::EngineCatalogResolveObjectNameRequest ResolveRequest(
    const catalog_api::EngineRequestContext& context,
    std::string object_kind,
    catalog_api::EngineUuid parent_uuid,
    std::string name) {
  catalog_api::EngineCatalogResolveObjectNameRequest request;
  request.context = context;
  request.target_object.object_kind = std::move(object_kind);
  request.target_schema.uuid = std::move(parent_uuid);
  request.localized_names.push_back(Name(std::move(name)));
  return request;
}

bool HasEvidence(const catalog_api::EngineApiResult& result,
                 std::string_view kind,
                 const catalog_api::EngineUuid& value) {
  for (const auto& evidence : result.evidence) {
    if (evidence.evidence_kind == kind && evidence.evidence_id == catalog_api::EngineEvidenceValue{value}) { return true; }
  }
  return false;
}

void TestFirstClassSynonymResolutionDepthCycleAndParentRemap() {
  const auto path = TestPath("primary");
  const auto database_uuid = CreateDatabase(path, 1779820001000);
  auto initial = Begin(path, database_uuid);
  Commit(initial);

  CommitCreate(path, database_uuid, CreateRequest(BaseContext(path, database_uuid), scratchbird::tests::FixtureUuid(1330, 1), "schema", {}, "sys"));
  CommitCreate(path, database_uuid, CreateRequest(BaseContext(path, database_uuid), scratchbird::tests::FixtureUuid(1330, 2), "schema", {}, "app"));
  CommitCreate(path, database_uuid, CreateRequest(BaseContext(path, database_uuid), scratchbird::tests::FixtureUuid(1330, 3), "schema", scratchbird::tests::FixtureUuid(1330, 1), "information"));
  CommitCreate(path, database_uuid, CreateRequest(BaseContext(path, database_uuid), scratchbird::tests::FixtureUuid(1330, 4), "table", scratchbird::tests::FixtureUuid(1330, 2), "customers"));
  CommitCreate(path, database_uuid, SynonymRequest(BaseContext(path, database_uuid), scratchbird::tests::FixtureUuid(1330, 5), scratchbird::tests::FixtureUuid(1330, 1),
                                                   "information_schema", scratchbird::tests::FixtureUuid(1330, 3), "schema"));

  auto info_context = Begin(path, database_uuid);
  const auto info = catalog_api::EngineCatalogResolveObjectName(
      ResolveRequest(info_context, "schema", scratchbird::tests::FixtureUuid(1330, 1), "information_schema"));
  RequireOk(info, "sys.information_schema synonym did not resolve");
  Require(info.bound_object_identity.object_uuid == scratchbird::tests::FixtureUuid(1330, 3),
          "sys.information_schema did not dereference to sys.information");
  Require(HasEvidence(info, "synonym_chain", scratchbird::tests::FixtureUuid(1330, 5)),
          "synonym chain evidence was not retained");
  Rollback(info_context);

  auto child_context = Begin(path, database_uuid);
  auto child = CreateRequest(child_context, scratchbird::tests::FixtureUuid(1330, 6), "table", scratchbird::tests::FixtureUuid(1330, 5), "columns");
  RequireOk(catalog_api::EngineCatalogCreateObject(child), "child create through synonym failed");
  Commit(child_context);

  auto load_context = Begin(path, database_uuid);
  const auto loaded = catalog_api::LoadCatalogObjectLifecycleState(load_context);
  Require(loaded.ok, "catalog lifecycle state did not load");
  bool saw_remap = false;
  for (const auto& object : loaded.state.objects) {
    if (object.object_uuid == scratchbird::tests::FixtureUuid(1330, 6)) {
      saw_remap = true;
      Require(object.schema_uuid == scratchbird::tests::FixtureUuid(1330, 3),
              "child created through synonym was not parented to final schema UUID");
    }
  }
  Require(saw_remap, "remapped child object was missing");
  Rollback(load_context);

  CommitCreate(path, database_uuid, SynonymRequest(BaseContext(path, database_uuid), scratchbird::tests::FixtureUuid(1330, 7), scratchbird::tests::FixtureUuid(1330, 2), "customers_alias_1", scratchbird::tests::FixtureUuid(1330, 4), "table"));
  CommitCreate(path, database_uuid, SynonymRequest(BaseContext(path, database_uuid), scratchbird::tests::FixtureUuid(1330, 8), scratchbird::tests::FixtureUuid(1330, 2), "customers_alias_2", scratchbird::tests::FixtureUuid(1330, 7), "synonym"));
  CommitCreate(path, database_uuid, SynonymRequest(BaseContext(path, database_uuid), scratchbird::tests::FixtureUuid(1330, 9), scratchbird::tests::FixtureUuid(1330, 2), "customers_alias_3", scratchbird::tests::FixtureUuid(1330, 8), "synonym"));
  CommitCreate(path, database_uuid, SynonymRequest(BaseContext(path, database_uuid), scratchbird::tests::FixtureUuid(1330, 10), scratchbird::tests::FixtureUuid(1330, 2), "customers_alias_4", scratchbird::tests::FixtureUuid(1330, 9), "synonym"));
  CommitCreate(path, database_uuid, SynonymRequest(BaseContext(path, database_uuid), scratchbird::tests::FixtureUuid(1330, 11), scratchbird::tests::FixtureUuid(1330, 2), "customers_alias_5", scratchbird::tests::FixtureUuid(1330, 10), "synonym"));

  auto depth_context = Begin(path, database_uuid);
  const auto depth_ok = catalog_api::EngineCatalogResolveObjectName(
      ResolveRequest(depth_context, "table", scratchbird::tests::FixtureUuid(1330, 2), "customers_alias_5"));
  RequireOk(depth_ok, "five-hop synonym chain was rejected");
  Require(depth_ok.bound_object_identity.object_uuid == scratchbird::tests::FixtureUuid(1330, 4),
          "five-hop chain did not resolve to final target");
  Rollback(depth_context);

  CommitCreate(path, database_uuid, SynonymRequest(BaseContext(path, database_uuid), scratchbird::tests::FixtureUuid(1330, 12), scratchbird::tests::FixtureUuid(1330, 2), "customers_alias_6", scratchbird::tests::FixtureUuid(1330, 11), "synonym"));
  auto over_depth_context = Begin(path, database_uuid);
  RequireDiagnostic(catalog_api::EngineCatalogResolveObjectName(
                        ResolveRequest(over_depth_context, "table", scratchbird::tests::FixtureUuid(1330, 2), "customers_alias_6")),
                    catalog_api::kCatalogSynonymDiagnosticDepthExceeded,
                    "six-hop synonym chain did not fail closed");
  Rollback(over_depth_context);

  CommitCreate(path, database_uuid, SynonymRequest(BaseContext(path, database_uuid), scratchbird::tests::FixtureUuid(1330, 13), scratchbird::tests::FixtureUuid(1330, 2), "cycle_a", scratchbird::tests::FixtureUuid(1330, 4), "table"));
  CommitCreate(path, database_uuid, SynonymRequest(BaseContext(path, database_uuid), scratchbird::tests::FixtureUuid(1330, 14), scratchbird::tests::FixtureUuid(1330, 2), "cycle_b", scratchbird::tests::FixtureUuid(1330, 13), "synonym"));

  auto alter_context = Begin(path, database_uuid);
  catalog_api::EngineCatalogAlterObjectRequest alter;
  alter.context = alter_context;
  alter.target_object.uuid = scratchbird::tests::FixtureUuid(1330, 13);
  alter.target_object.object_kind = "synonym";
  alter.related_objects.push_back({{scratchbird::tests::FixtureUuid(1330, 14)}, "synonym"});
  RequireOk(catalog_api::EngineCatalogAlterObject(alter), "synonym retarget alter failed");
  Commit(alter_context);

  auto cycle_context = Begin(path, database_uuid);
  RequireDiagnostic(catalog_api::EngineCatalogResolveObjectName(
                        ResolveRequest(cycle_context, "table", scratchbird::tests::FixtureUuid(1330, 2), "cycle_a")),
                    catalog_api::kCatalogSynonymDiagnosticCycle,
                    "synonym cycle did not fail closed");
  Rollback(cycle_context);
}

void TestSynonymRollbackAndDependencyEdges() {
  const auto path = TestPath("rollback");
  const auto database_uuid = CreateDatabase(path, 1779830001000);
  CommitCreate(path, database_uuid, CreateRequest(BaseContext(path, database_uuid), scratchbird::tests::FixtureUuid(1330, 2), "schema", {}, "app"));
  CommitCreate(path, database_uuid, CreateRequest(BaseContext(path, database_uuid), scratchbird::tests::FixtureUuid(1330, 15), "table", scratchbird::tests::FixtureUuid(1330, 2), "orders"));

  auto synonym_context = Begin(path, database_uuid);
  auto synonym = SynonymRequest(synonym_context, scratchbird::tests::FixtureUuid(1330, 16), scratchbird::tests::FixtureUuid(1330, 2), "orders_alias", scratchbird::tests::FixtureUuid(1330, 15), "table");
  RequireOk(catalog_api::EngineCatalogCreateObject(synonym), "provisional synonym create failed");
  Rollback(synonym_context);

  auto load_context = Begin(path, database_uuid);
  const auto loaded = catalog_api::LoadCatalogObjectLifecycleState(load_context);
  Require(loaded.ok, "catalog lifecycle state did not load after rollback");
  for (const auto& object : loaded.state.objects) {
    Require(object.object_uuid != scratchbird::tests::FixtureUuid(1330, 16),
            "rolled-back synonym descriptor remained visible");
  }
  for (const auto& name : loaded.state.names) {
    Require(name.object_uuid != scratchbird::tests::FixtureUuid(1330, 16),
            "rolled-back synonym name remained visible");
  }
  for (const auto& dependency : loaded.state.dependencies) {
    Require(dependency.source_uuid != scratchbird::tests::FixtureUuid(1330, 16),
            "rolled-back synonym dependency remained visible");
  }
  Rollback(load_context);

  CommitCreate(path, database_uuid, SynonymRequest(BaseContext(path, database_uuid), scratchbird::tests::FixtureUuid(1330, 17), scratchbird::tests::FixtureUuid(1330, 2),
                                                   "orders_alias_committed", scratchbird::tests::FixtureUuid(1330, 15), "table"));
  auto drop_context = Begin(path, database_uuid);
  catalog_api::EngineCatalogDropObjectRequest drop;
  drop.context = drop_context;
  drop.target_object.uuid = scratchbird::tests::FixtureUuid(1330, 15);
  drop.target_object.object_kind = "table";
  RequireDiagnostic(catalog_api::EngineCatalogDropObject(drop),
                    catalog_api::kCatalogObjectDiagnosticDependencyBlockedDrop,
                    "target drop was not blocked by synonym dependency");
  Rollback(drop_context);
}

void TestSysInformationCanonicalAndLegacyPaths() {
  catalog_api::SysInformationProjectionContext context;
  context.catalog_display_name = "CustomerDB";
  context.session_language = "en";
  context.default_language = "en";
  context.visible_catalog_generation_id = 2;

  const std::vector<catalog_api::SysInformationCatalogObjectSource> objects = {
      {.object_uuid = scratchbird::tests::FixtureUuid(1330, 2), .object_class = "schema", .catalog_generation_id = 1},
      {.object_uuid = scratchbird::tests::FixtureUuid(1330, 4), .object_class = "table", .schema_uuid = scratchbird::tests::FixtureUuid(1330, 2),
       .table_type = "BASE TABLE", .catalog_generation_id = 2},
  };
  const std::vector<catalog_api::SysInformationResolverNameSource> names = {
      {.object_uuid = scratchbird::tests::FixtureUuid(1330, 2), .object_class = "schema", .language_tag = "en",
       .name_class = "primary", .display_name = "app", .catalog_generation_id = 1},
      {.object_uuid = scratchbird::tests::FixtureUuid(1330, 4), .object_class = "table", .scope_uuid = scratchbird::tests::FixtureUuid(1330, 2),
       .language_tag = "en", .name_class = "primary", .display_name = "customers",
       .catalog_generation_id = 2},
  };

  const auto canonical = catalog_api::BuildSysInformationProjection(
      "sys.information.tables", context, objects, names);
  Require(canonical.ok, "canonical sys.information projection failed");
  const auto legacy = catalog_api::BuildSysInformationProjection(
      "sys.information_schema.tables", context, objects, names);
  Require(legacy.ok, "legacy sys.information_schema synonym projection failed");
  Require(canonical.rows.size() == legacy.rows.size(),
          "legacy information_schema projection diverged from canonical sys.information");
  Require(catalog_api::FindSysInformationProjectionDefinition("sys.information.tables") != nullptr,
          "canonical sys.information definition missing");
  Require(catalog_api::FindSysInformationProjectionDefinition("sys.information_schema.tables") != nullptr,
          "legacy information_schema synonym definition did not map to canonical projection");
}

}  // namespace

int main() {
  TestFirstClassSynonymResolutionDepthCycleAndParentRemap();
  TestSynonymRollbackAndDependencyEdges();
  TestSysInformationCanonicalAndLegacyPaths();
  return EXIT_SUCCESS;
}
