// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "database_lifecycle.hpp"
#include "ddl/alter_api.hpp"
#include "ddl/create_api.hpp"
#include "ddl/drop_api.hpp"
#include "transaction/transaction_api.hpp"
#include "uuid.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>

#include <unistd.h>

namespace {

namespace api = scratchbird::engine::internal_api;
namespace db = scratchbird::storage::database;
namespace uuid = scratchbird::core::uuid;
using scratchbird::core::platform::UuidKind;

constexpr std::string_view kPublishOrder =
    "validate.prebuild.persist.name_registry.result_shape.invalidate";
constexpr std::string_view kMgaAuthority = "durable_transaction_inventory";

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

std::uint64_t NowMillis() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

std::string GeneratedUuid(UuidKind kind, std::uint64_t salt) {
  const auto generated = uuid::GenerateEngineIdentityV7(kind, NowMillis() + salt);
  Require(generated.ok(), "IPAR DDL publication UUID generation failed");
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

api::EngineDescriptor ScalarDescriptor(std::string canonical_type_name) {
  api::EngineDescriptor descriptor;
  descriptor.descriptor_kind = "scalar";
  descriptor.canonical_type_name = std::move(canonical_type_name);
  descriptor.encoded_descriptor = "type=" + descriptor.canonical_type_name;
  return descriptor;
}

api::EngineColumnDefinition Column(std::string name,
                                   std::string canonical_type_name,
                                   std::uint32_t ordinal) {
  api::EngineColumnDefinition column;
  column.names.push_back(Name(std::move(name)));
  column.descriptor = ScalarDescriptor(std::move(canonical_type_name));
  column.ordinal = ordinal;
  column.nullable = false;
  return column;
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

std::string FieldValue(const api::EngineRowValue& row, std::string_view field_name) {
  for (const auto& field : row.fields) {
    if (field.first == field_name) { return field.second.encoded_value; }
  }
  return {};
}

void ExpectPublicationRow(const api::EngineApiResult& result,
                          std::string_view operation_id,
                          std::string_view object_kind,
                          std::string_view object_uuid) {
  const std::string expected_packet =
      std::string(operation_id) + ":" + std::string(object_kind) + ":" +
      std::string(object_uuid);
  for (const auto& row : result.result_shape.rows) {
    if (FieldValue(row, "ddl_result_object_uuid") != object_uuid) { continue; }
    Require(FieldValue(row, "ddl_operation_id") == operation_id,
            "DDL publication row operation drifted");
    Require(FieldValue(row, "ddl_result_object_kind") == object_kind,
            "DDL publication row object kind drifted");
    Require(FieldValue(row, "ddl_publish_packet_id") == expected_packet,
            "DDL publication row packet id drifted");
    Require(FieldValue(row, "ddl_publish_order") == kPublishOrder,
            "DDL publication row order drifted");
    Require(FieldValue(row, "ddl_final_publish_short_section") == "true",
            "DDL publication row did not prove short final publish section");
    Require(FieldValue(row, "ddl_partial_state_visible") == "false",
            "DDL publication row exposed partial state");
    Require(FieldValue(row, "ddl_result_buffer_owner") == "engine",
            "DDL publication row result buffer owner drifted");
    Require(FieldValue(row, "ddl_parser_sql_authority") == "false",
            "DDL publication row gave parser SQL authority");
    Require(FieldValue(row, "ddl_mga_finality_authority") == kMgaAuthority,
            "DDL publication row finality authority drifted");
    return;
  }
  Fail("DDL publication row for object UUID was not returned");
}

void ExpectPublication(const api::EngineApiResult& result,
                       std::string_view operation_id,
                       std::string_view object_kind,
                       std::string_view object_uuid,
                       std::string_view invalidation_scope,
                       bool require_catalog_row = true) {
  RequireOk(result, "DDL publication target failed");
  Require(result.operation_id == operation_id, "DDL operation id drifted");
  Require(result.primary_object.uuid.canonical == object_uuid,
          "DDL result did not return the requested object UUID");
  Require(result.primary_object.object_kind == object_kind,
          "DDL result did not return the requested object kind");
  if (require_catalog_row) {
    Require(!result.catalog_row_uuid.canonical.empty(),
            "DDL result did not return a catalog row UUID");
    Require(HasEvidence(result, "ddl_catalog_row_uuid", result.catalog_row_uuid.canonical),
            "DDL publication did not prove catalog row UUID");
  }

  const std::string expected_packet =
      std::string(operation_id) + ":" + std::string(object_kind) + ":" +
      std::string(object_uuid);
  Require(HasEvidence(result, "ddl_publish_packet", expected_packet),
          "DDL publication packet evidence missing");
  Require(HasEvidence(result, "ddl_publish_packet_order", kPublishOrder),
          "DDL publication order evidence missing");
  Require(HasEvidence(result, "ddl_final_publish_short_section", "true"),
          "DDL final short-section evidence missing");
  Require(HasEvidence(result, "ddl_partial_state_visible", "false"),
          "DDL partial-state refusal evidence missing");
  Require(HasEvidence(result, "ddl_uuid_returned", object_uuid),
          "DDL UUID return evidence missing");
  Require(HasEvidence(result, "ddl_result_buffer_owner", "engine"),
          "DDL result buffer owner evidence missing");
  Require(HasEvidence(result, "ddl_result_buffer_reserved", "true"),
          "DDL result buffer reservation evidence missing");
  Require(HasEvidence(result, "ddl_dependency_invalidation_scope", invalidation_scope),
          "DDL dependency invalidation evidence missing");
  Require(HasEvidence(result, "ddl_parser_sql_authority", "false"),
          "DDL parser SQL authority guard evidence missing");
  Require(HasEvidence(result, "ddl_mga_finality_authority", kMgaAuthority),
          "DDL MGA finality authority evidence missing");
  ExpectPublicationRow(result, operation_id, object_kind, object_uuid);
}

std::string CreateDatabase(const std::filesystem::path& path) {
  db::DatabaseCreateConfig create;
  create.path = path.string();
  create.database_uuid =
      uuid::GenerateEngineIdentityV7(UuidKind::database, NowMillis() + 1).value;
  create.filespace_uuid =
      uuid::GenerateEngineIdentityV7(UuidKind::filespace, NowMillis() + 2).value;
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
  Require(created.ok(), "IPAR DDL publication database create failed");
  return uuid::UuidToString(create.database_uuid.value);
}

api::EngineRequestContext BaseContext(const std::filesystem::path& path,
                                      const std::string& database_uuid,
                                      const std::string& principal_uuid,
                                      const std::string& schema_uuid) {
  api::EngineRequestContext context;
  context.trust_mode = api::EngineTrustMode::server_isolated;
  context.request_id = "ipar-ddl-publication-result-gate";
  context.database_path = path.string();
  context.database_uuid.canonical = database_uuid;
  context.principal_uuid.canonical = principal_uuid;
  context.session_uuid.canonical = GeneratedUuid(UuidKind::object, 100);
  context.current_schema_uuid.canonical = schema_uuid;
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
                                const std::string& principal_uuid,
                                const std::string& schema_uuid) {
  api::EngineBeginTransactionRequest request;
  request.context = BaseContext(path, database_uuid, principal_uuid, schema_uuid);
  request.isolation_level = "read_committed";
  const auto begun = api::EngineBeginTransaction(request);
  RequireOk(begun, "IPAR DDL publication transaction begin failed");
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
  RequireOk(api::EngineCommitTransaction(request),
            "IPAR DDL publication transaction commit failed");
}

}  // namespace

int main() {
  const auto path = std::filesystem::temp_directory_path() /
                    ("sb_ipar_ddl_publication_" +
                     std::to_string(NowMillis()) + "_" +
                     std::to_string(static_cast<long long>(getpid())) + ".sbdb");
  const std::string database_uuid = CreateDatabase(path);
  const std::string owner_uuid = GeneratedUuid(UuidKind::object, 10);
  const std::string schema_uuid = GeneratedUuid(UuidKind::schema, 11);
  const std::string table_uuid = GeneratedUuid(UuidKind::object, 12);
  const std::string index_uuid = GeneratedUuid(UuidKind::object, 13);
  const std::string statistics_uuid = GeneratedUuid(UuidKind::object, 14);
  const std::string domain_uuid = GeneratedUuid(UuidKind::object, 15);

  auto context = Begin(path, database_uuid, owner_uuid, schema_uuid);

  api::EngineCreateSchemaRequest schema;
  schema.context = context;
  schema.target_object.uuid.canonical = schema_uuid;
  schema.target_object.object_kind = "schema";
  schema.localized_names.push_back(Name("ipar_publication_schema"));
  const auto created_schema = api::EngineCreateSchema(schema);
  ExpectPublication(created_schema,
                    "ddl.create_schema",
                    "schema",
                    schema_uuid,
                    "schema_tree");

  api::EngineCreateTableRequest table;
  table.context = context;
  table.target_schema.uuid.canonical = schema_uuid;
  table.target_schema.object_kind = "schema";
  table.requested_table_uuid.canonical = table_uuid;
  table.table_names.push_back(Name("ipar_publication_table"));
  table.table_columns.push_back(Column("id", "int64", 0));
  table.table_columns.push_back(Column("payload", "varchar", 1));
  const auto created_table = api::EngineCreateTable(table);
  ExpectPublication(created_table,
                    "ddl.create_table",
                    "table",
                    table_uuid,
                    "mga_relation_metadata");

  api::EngineCreateIndexRequest index;
  index.context = context;
  index.target_object.uuid.canonical = table_uuid;
  index.target_object.object_kind = "table";
  api::EngineIndexDefinition index_definition;
  index_definition.requested_index_uuid.canonical = index_uuid;
  index_definition.names.push_back(Name("ipar_publication_idx"));
  index_definition.index_kind = "btree";
  index_definition.key_envelopes.push_back("id");
  index.indexes.push_back(index_definition);
  const auto created_index = api::EngineCreateIndex(index);
  ExpectPublication(created_index,
                    "ddl.create_index",
                    "index",
                    index_uuid,
                    "mga_relation_metadata.index");

  api::EngineCreateStatisticsRequest statistics;
  statistics.context = context;
  statistics.target_table.uuid.canonical = table_uuid;
  statistics.target_table.object_kind = "table";
  statistics.requested_statistics_uuid.canonical = statistics_uuid;
  statistics.statistics_names.push_back(Name("ipar_publication_stats"));
  statistics.statistics_kinds.push_back("ndistinct");
  statistics.expression_envelopes.push_back("id");
  const auto created_statistics = api::EngineCreateStatistics(statistics);
  ExpectPublication(created_statistics,
                    "ddl.create_statistics",
                    "statistics",
                    statistics_uuid,
                    "optimizer.statistics");

  api::EngineCreateDomainRequest domain;
  domain.context = context;
  domain.target_schema.uuid.canonical = schema_uuid;
  domain.target_schema.object_kind = "schema";
  domain.target_object.uuid.canonical = domain_uuid;
  domain.target_object.object_kind = "domain";
  domain.localized_names.push_back(Name("ipar_publication_domain"));
  domain.descriptors.push_back(ScalarDescriptor("int64"));
  const auto created_domain = api::EngineCreateDomain(domain);
  ExpectPublication(created_domain,
                    "ddl.create_domain",
                    "domain",
                    domain_uuid,
                    "domain_event");

  api::EngineAlterObjectRequest alter_domain;
  alter_domain.context = context;
  alter_domain.target_object.uuid.canonical = domain_uuid;
  alter_domain.target_object.object_kind = "domain";
  alter_domain.option_envelopes.push_back("comment:publication gate update");
  const auto altered_domain = api::EngineAlterObject(alter_domain);
  ExpectPublication(altered_domain,
                    "ddl.alter_object",
                    "domain",
                    domain_uuid,
                    "domain_event");

  api::EngineDropObjectRequest drop_domain;
  drop_domain.context = context;
  drop_domain.target_object.uuid.canonical = domain_uuid;
  drop_domain.target_object.object_kind = "domain";
  const auto dropped_domain = api::EngineDropObject(drop_domain);
  ExpectPublication(dropped_domain,
                    "ddl.drop_object",
                    "domain",
                    domain_uuid,
                    "domain_event",
                    false);

  Commit(context);
  std::filesystem::remove(path);
  std::cout << "ipar_ddl_publication_result_gate=passed\n";
  return EXIT_SUCCESS;
}
