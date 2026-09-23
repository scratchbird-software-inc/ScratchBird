// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "../support/binary_uuid_fixture.hpp"
#include "database_lifecycle.hpp"
#include "catalog/column_metadata_codec.hpp"
#include "dml/delete_api.hpp"
#include "dml/insert_api.hpp"
#include "dml/update_api.hpp"
#include "mga_relation_store/mga_relation_store.hpp"
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
namespace uuid = scratchbird::core::uuid;
using scratchbird::core::platform::UuidKind;

constexpr auto kSchemaUuid = scratchbird::tests::FixtureUuidLiteral("019f1000-0000-7000-8000-000000000001");
constexpr auto kParentTableUuid = scratchbird::tests::FixtureUuidLiteral("019f1000-0000-7000-8000-000000000101");
constexpr auto kChildTableUuid = scratchbird::tests::FixtureUuidLiteral("019f1000-0000-7000-8000-000000000102");
constexpr auto kNoIndexTableUuid = scratchbird::tests::FixtureUuidLiteral("019f1000-0000-7000-8000-000000000103");
constexpr auto kExclusionTableUuid = scratchbird::tests::FixtureUuidLiteral("019f1000-0000-7000-8000-000000000104");
constexpr auto kDeferredTableUuid = scratchbird::tests::FixtureUuidLiteral("019f1000-0000-7000-8000-000000000105");
constexpr auto kMalformedFkTableUuid = scratchbird::tests::FixtureUuidLiteral("019f1000-0000-7000-8000-000000000106");
constexpr auto kParentPkIndexUuid = scratchbird::tests::FixtureUuidLiteral("019f1000-0000-7000-8000-000000000201");
constexpr auto kChildPkIndexUuid = scratchbird::tests::FixtureUuidLiteral("019f1000-0000-7000-8000-000000000202");
constexpr auto kDeferredPkIndexUuid = scratchbird::tests::FixtureUuidLiteral("019f1000-0000-7000-8000-000000000203");
constexpr const char* kNoIndexName = "no_backing_index";

void Require(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(EXIT_FAILURE);
  }
}

template <typename TResult>
bool HasDiagnostic(const TResult& result, std::string_view code) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.code == code) { return true; }
  }
  return false;
}

bool HasEvidence(const api::EngineApiResult& result,
                 std::string_view kind,
                 std::string_view id = {}) {
  for (const auto& evidence : result.evidence) {
    if (evidence.evidence_kind == kind && (id.empty() || (std::holds_alternative<std::string>(evidence.evidence_id) && std::get<std::string>(evidence.evidence_id) == id))) {
      return true;
    }
  }
  return false;
}

bool HasEvidence(const api::EngineApiResult& result,
                 std::string_view kind, const api::EngineUuid& identity) {
  for (const auto& evidence : result.evidence) {
    const auto* value = std::get_if<api::EngineUuid>(&evidence.evidence_id);
    if (evidence.evidence_kind == kind && value && *value == identity) return true;
  }
  return false;
}

std::string ColumnMetadata(std::string_view scalar_attributes,
                           const api::EngineUuid& constraint,
                           const api::EngineUuid& referenced_table = {}) {
  api::CatalogColumnMetadata fields;
  Require(api::AdmitCatalogColumnMetadata(scalar_attributes, &fields),
          "invalid scalar constraint fixture metadata");
  fields.identities.emplace("constraint_uuid", constraint);
  if (!referenced_table.is_nil()) {
    fields.identities.emplace("referenced_table_uuid", referenced_table);
    fields.text.emplace("referenced_column", "id");
  }
  std::string encoded;
  Require(api::EncodeCatalogColumnMetadata(fields, &encoded),
          "constraint fixture metadata encoding failed");
  return encoded;
}

std::uint64_t CurrentUnixMillis() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

std::filesystem::path MakeTempPath() {
  std::string pattern = (std::filesystem::temp_directory_path() /
                         "sb_prf_constraint_dml_XXXXXX").string();
  std::vector<char> writable(pattern.begin(), pattern.end());
  writable.push_back('\0');
  const auto made = ::mkdtemp(writable.data());
  Require(made != nullptr, "constraint fixture directory creation failed");
  return std::filesystem::path(made) / "constraint.sbdb";
}

api::EngineUuid CreateDatabase(const std::filesystem::path& path) {
  db::DatabaseCreateConfig create;
  create.path = path.string();
  create.database_uuid = uuid::GenerateEngineIdentityV7(UuidKind::database, 1779800001000).value;
  create.filespace_uuid = uuid::GenerateEngineIdentityV7(UuidKind::filespace, 1779800001001).value;
  create.page_size = 16384;
  create.creation_unix_epoch_millis = 1779800001002;
  create.allow_minimal_resource_bootstrap = true;
  create.require_resource_seed_pack = false;
  create.allow_overwrite = true;
  const auto created = db::CreateDatabaseFile(create);
  if (!created.ok()) {
    std::cerr << created.diagnostic.diagnostic_code << ":" << created.diagnostic.message_key << '\n';
  }
  Require(created.ok(), "constraint DML database create failed");
  return create.database_uuid.value;
}

api::EngineRequestContext BaseContext(const std::filesystem::path& path,
                                      const api::EngineUuid& database_uuid,
                                      std::uint32_t session_ordinal) {
  api::EngineRequestContext context;
  context.trust_mode = api::EngineTrustMode::server_isolated;
  context.request_id = "prf-constraint-dml";
  context.database_path = path.string();
  context.database_uuid = database_uuid;
  context.principal_uuid = scratchbird::tests::FixtureUuidLiteral("019f1000-0000-7000-8000-000000000301");
  context.session_uuid = scratchbird::tests::FixtureUuid(1547, session_ordinal);
  context.current_schema_uuid = scratchbird::tests::FixtureUuidLiteral("019f1000-0000-7000-8000-000000000001");
  context.default_root_uuid = scratchbird::tests::FixtureUuidLiteral("019f1000-0000-7000-8000-000000000302");
  context.security_context_present = true;
  context.catalog_generation_id = 1;
  context.security_epoch = 1;
  context.resource_epoch = 1;
  context.name_resolution_epoch = 1;
  context.trace_tags.push_back("PRF-030");
  context.trace_tags.push_back("PRF-035");
  return context;
}

api::EngineRequestContext Begin(const std::filesystem::path& path,
                                const api::EngineUuid& database_uuid,
                                std::uint32_t session_ordinal) {
  api::EngineBeginTransactionRequest request;
  request.context = BaseContext(path, database_uuid, session_ordinal);
  request.isolation_level = "read_committed";
  const auto begun = api::EngineBeginTransaction(request);
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

void Commit(const api::EngineRequestContext& context) {
  api::EngineCommitTransactionRequest request;
  request.context = context;
  const auto committed = api::EngineCommitTransaction(request);
  if (!committed.ok) {
    for (const auto& diagnostic : committed.diagnostics) {
      std::cerr << diagnostic.code << ":" << diagnostic.detail << '\n';
    }
  }
  Require(committed.ok, "commit transaction failed");
}

void Rollback(const api::EngineRequestContext& context) {
  api::EngineRollbackTransactionRequest request;
  request.context = context;
  const auto rolled_back = api::EngineRollbackTransaction(request);
  Require(rolled_back.ok, "rollback transaction failed");
}

api::EngineTypedValue TextValue(std::string value, bool is_null = false) {
  api::EngineTypedValue typed;
  typed.descriptor.descriptor_kind = "scalar";
  typed.descriptor.canonical_type_name = "text";
  typed.encoded_value = std::move(value);
  typed.is_null = is_null;
  return typed;
}

api::EngineRowValue Row(api::EngineUuid row_uuid,
                        std::vector<std::pair<std::string, api::EngineTypedValue>> fields) {
  api::EngineRowValue row;
  row.requested_row_uuid = std::move(row_uuid);
  row.fields = std::move(fields);
  return row;
}

api::CrudTableRecord Table(api::EngineUuid table_uuid,
                           std::vector<std::pair<std::string, std::string>> columns) {
  api::CrudTableRecord table;
  table.table_uuid = std::move(table_uuid);
  table.default_name = "constraint_fixture_table";
  table.columns = std::move(columns);
  return table;
}

api::CrudIndexRecord UniqueIndex(api::EngineUuid index_uuid,
                                 api::EngineUuid table_uuid,
                                 std::string column_name) {
  api::CrudIndexRecord index;
  index.index_uuid = std::move(index_uuid);
  index.table_uuid = std::move(table_uuid);
  index.column_name = std::move(column_name);
  index.key_envelopes.push_back(index.column_name);
  index.profile = api::kCrudIndexProfileRowStoreScalarBtreeV1;
  index.family = api::kCrudIndexFamilyBtree;
  index.unique = true;
  index.default_name = "constraint_fixture_index";
  return index;
}

void SeedConstraintMetadata(const api::EngineRequestContext& context) {
  const auto parent = Table(kParentTableUuid,
                            {{"id", ColumnMetadata("type=text;nullable=false;primary_key=true", scratchbird::tests::FixtureUuid(1547, 810))},
                             {"name", "type=text;nullable=false"}});
  const auto child = Table(
      kChildTableUuid,
      {{"id", ColumnMetadata("type=text;nullable=false;primary_key=true", scratchbird::tests::FixtureUuid(1547, 811))},
       {"customer_id",
        ColumnMetadata("type=text;nullable=false", scratchbird::tests::FixtureUuid(1547, 803), kParentTableUuid)},
       {"amount", ColumnMetadata("type=int;default=literal:1;check=gt:0", scratchbird::tests::FixtureUuid(1547, 812))},
       {"note", ColumnMetadata("type=text;nullable=false", scratchbird::tests::FixtureUuid(1547, 813))}});
  const auto no_index = Table(kNoIndexTableUuid,
                              {{"id", ColumnMetadata("type=text;nullable=false;primary_key=true", scratchbird::tests::FixtureUuid(1547, 814))}});
  const auto exclusion = Table(kExclusionTableUuid,
                               {{"span", ColumnMetadata("type=text;exclusion=true", scratchbird::tests::FixtureUuid(1547, 815))}});
  const auto deferred = Table(kDeferredTableUuid,
                              {{"id", ColumnMetadata("type=text;nullable=false;primary_key=true;deferrable=true", scratchbird::tests::FixtureUuid(1547, 816))}});
  const auto malformed_fk = Table(kMalformedFkTableUuid,
                                  {{"parent_id", ColumnMetadata("type=text;foreign_key=true", scratchbird::tests::FixtureUuid(1547, 817))}});

  Require(!api::AppendMgaTableMetadata(context, parent).error, "append parent table metadata failed");
  Require(!api::AppendMgaIndexMetadata(context, UniqueIndex(kParentPkIndexUuid, kParentTableUuid, "id")).error,
          "append parent pk index metadata failed");
  Require(!api::AppendMgaTableMetadata(context, child).error, "append child table metadata failed");
  Require(!api::AppendMgaIndexMetadata(context, UniqueIndex(kChildPkIndexUuid, kChildTableUuid, "id")).error,
          "append child pk index metadata failed");
  Require(!api::AppendMgaTableMetadata(context, no_index).error, "append no-index table metadata failed");
  Require(!api::AppendMgaTableMetadata(context, exclusion).error, "append exclusion table metadata failed");
  Require(!api::AppendMgaTableMetadata(context, deferred).error, "append deferred table metadata failed");
  Require(!api::AppendMgaIndexMetadata(context, UniqueIndex(kDeferredPkIndexUuid, kDeferredTableUuid, "id")).error,
          "append deferred pk index metadata failed");
  Require(!api::AppendMgaTableMetadata(context, malformed_fk).error, "append malformed-fk table metadata failed");
}

api::EngineInsertRowsResult Insert(const api::EngineRequestContext& context,
                                   api::EngineUuid table_uuid,
                                   api::EngineRowValue row) {
  api::EngineInsertRowsRequest request;
  request.context = context;
  request.target_table.uuid = std::move(table_uuid);
  request.target_table.object_kind = "table";
  request.input_rows.push_back(std::move(row));
  return api::EngineInsertRows(request);
}

api::EngineDeleteRowsResult DeleteParent(const api::EngineRequestContext& context, std::string id) {
  api::EngineDeleteRowsRequest request;
  request.context = context;
  request.target_table.uuid = scratchbird::tests::FixtureUuidLiteral("019f1000-0000-7000-8000-000000000101");
  request.target_table.object_kind = "table";
  request.delete_predicate.predicate_kind = "column_equals";
  request.delete_predicate.canonical_predicate_envelope = "id";
  request.delete_predicate.bound_values.push_back(TextValue(std::move(id)));
  return api::EngineDeleteRows(request);
}

api::EngineUpdateRowsResult UpdateRow(const api::EngineRequestContext& context,
                                      api::EngineUuid table_uuid,
                                      api::EngineUuid row_uuid,
                                      std::vector<std::pair<std::string, api::EngineTypedValue>> assignments) {
  api::EngineUpdateRowsRequest request;
  request.context = context;
  request.target_table.uuid = std::move(table_uuid);
  request.target_table.object_kind = "table";
  request.update_predicate.predicate_kind = "row_uuid_match";
  request.update_predicate.row_uuid = row_uuid;
  request.assignments = std::move(assignments);
  return api::EngineUpdateRows(request);
}

void VerifyConstraintEnforcement(const std::filesystem::path& path, const api::EngineUuid& database_uuid) {
  auto setup = Begin(path, database_uuid, 401);
  SeedConstraintMetadata(setup);
  Commit(setup);

  auto writer = Begin(path, database_uuid, 402);
  auto parent = Insert(writer,
                       kParentTableUuid,
                       Row(scratchbird::tests::FixtureUuidLiteral("019f1000-0000-7000-8000-000000000501"),
                           {{"id", TextValue("c1")}, {"name", TextValue("customer")}}));
  Require(parent.ok, "parent insert failed");

  auto child = Insert(writer,
                      kChildTableUuid,
                      Row(scratchbird::tests::FixtureUuidLiteral("019f1000-0000-7000-8000-000000000502"),
                          {{"id", TextValue("o1")},
                           {"customer_id", TextValue("c1")},
                           {"note", TextValue("created")}}));
  if (!child.ok) {
    for (const auto& diagnostic : child.diagnostics) {
      std::cerr << diagnostic.code << ":" << diagnostic.detail << '\n';
    }
  }
  Require(child.ok, "child insert with default/check/fk failed");
  Require(child.result_shape.rows.size() == 1, "child insert did not return row");
  Require(HasEvidence(child, "constraint_default", "amount"),
          "default constraint evidence missing");
  Require(HasEvidence(child, "constraint_foreign_key", "customer_id"),
          "foreign key evidence missing");
  Require(HasEvidence(child, "constraint_key_support", kChildPkIndexUuid),
          "primary key support evidence missing");
  bool saw_default_amount = false;
  for (const auto& [field, typed] : child.result_shape.rows.front().fields) {
    if (field == "amount" && typed.encoded_value == "1") { saw_default_amount = true; }
  }
  Require(saw_default_amount, "default value was not materialized into returned row");

  const auto update_not_null = UpdateRow(writer,
                                         kChildTableUuid,
                                         scratchbird::tests::FixtureUuidLiteral("019f1000-0000-7000-8000-000000000502"),
                                         {{"note", TextValue({}, true)}});
  Require(HasDiagnostic(update_not_null, "CLI.CONSTRAINT_NOT_NULL_VIOLATION"),
          "UPDATE did not enforce canonical NOT NULL diagnostics");

  const auto missing_not_null = Insert(writer,
                                       kChildTableUuid,
                                       Row(scratchbird::tests::FixtureUuidLiteral("019f1000-0000-7000-8000-000000000503"),
                                           {{"id", TextValue("o2")},
                                            {"customer_id", TextValue("c1")}}));
  Require(HasDiagnostic(missing_not_null, "CLI.CONSTRAINT_NOT_NULL_VIOLATION"),
          "missing NOT NULL value did not emit canonical diagnostic");

  const auto bad_check = Insert(writer,
                                kChildTableUuid,
                                Row(scratchbird::tests::FixtureUuidLiteral("019f1000-0000-7000-8000-000000000504"),
                                    {{"id", TextValue("o3")},
                                     {"customer_id", TextValue("c1")},
                                     {"amount", TextValue("0")},
                                     {"note", TextValue("bad")}}));
  Require(HasDiagnostic(bad_check, "CLI.CONSTRAINT_CHECK_VIOLATION"),
          "CHECK violation did not emit canonical diagnostic");

  const auto bad_fk = Insert(writer,
                             kChildTableUuid,
                             Row(scratchbird::tests::FixtureUuidLiteral("019f1000-0000-7000-8000-000000000505"),
                                 {{"id", TextValue("o4")},
                                  {"customer_id", TextValue("missing")},
                                  {"note", TextValue("bad")}}));
  Require(HasDiagnostic(bad_fk, "CLI.CONSTRAINT_FOREIGN_KEY_VIOLATION"),
          "foreign key miss did not emit canonical diagnostic");

  const auto duplicate_pk = Insert(writer,
                                   kChildTableUuid,
                                   Row(scratchbird::tests::FixtureUuidLiteral("019f1000-0000-7000-8000-000000000506"),
                                       {{"id", TextValue("o1")},
                                        {"customer_id", TextValue("c1")},
                                        {"note", TextValue("dup")}}));
  Require(HasDiagnostic(duplicate_pk, "CLI.CONSTRAINT_PRIMARY_KEY_VIOLATION"),
          "duplicate primary key did not emit canonical diagnostic");

  const auto update_parent_key = UpdateRow(writer,
                                           kParentTableUuid,
                                           scratchbird::tests::FixtureUuidLiteral("019f1000-0000-7000-8000-000000000501"),
                                           {{"id", TextValue("c2")}});
  Require(HasDiagnostic(update_parent_key, "CLI.CONSTRAINT_FOREIGN_KEY_VIOLATION"),
          "referenced parent key update was not restricted");

  const auto delete_parent = DeleteParent(writer, "c1");
  Require(HasDiagnostic(delete_parent, "CLI.CONSTRAINT_FOREIGN_KEY_VIOLATION"),
          "referenced parent delete was not restricted");

  const auto no_support = Insert(writer,
                                 kNoIndexTableUuid,
                                 Row(scratchbird::tests::FixtureUuidLiteral("019f1000-0000-7000-8000-000000000507"),
                                     {{"id", TextValue("n1")}}));
  Require(HasDiagnostic(no_support, "CLI.SUPPORT_STRUCTURE_UNAVAILABLE"),
          "primary key without backing index did not fail closed");

  const auto exclusion = Insert(writer,
                                kExclusionTableUuid,
                                Row(scratchbird::tests::FixtureUuidLiteral("019f1000-0000-7000-8000-000000000508"),
                                    {{"span", TextValue("1,10")}}));
  Require(exclusion.ok, "exclusion constraint first interval was rejected");
  Require(HasEvidence(exclusion, "constraint_exclusion", "span"),
          "exclusion constraint evidence missing");

  const auto exclusion_conflict = Insert(writer,
                                         kExclusionTableUuid,
                                         Row(scratchbird::tests::FixtureUuidLiteral("019f1000-0000-7000-8000-000000000511"),
                                             {{"span", TextValue("5,15")}}));
  Require(HasDiagnostic(exclusion_conflict, "CLI.CONSTRAINT_EXCLUSION_VIOLATION"),
          "overlapping exclusion value did not emit canonical diagnostic");

  const auto deferred = Insert(writer,
                               kDeferredTableUuid,
                               Row(scratchbird::tests::FixtureUuidLiteral("019f1000-0000-7000-8000-000000000509"),
                                   {{"id", TextValue("d1")}}));
  Require(deferred.ok, "deferred constraint valid row was rejected before commit");
  Require(HasEvidence(deferred, "constraint_deferred_pending_check", scratchbird::tests::FixtureUuid(1547, 816)),
          "deferred constraint pending-check evidence missing");

  const auto malformed_fk = Insert(writer,
                                   kMalformedFkTableUuid,
                                   Row(scratchbird::tests::FixtureUuidLiteral("019f1000-0000-7000-8000-000000000510"),
                                       {{"parent_id", TextValue("c1")}}));
  Require(HasDiagnostic(malformed_fk, "CLI.CONSTRAINT_DESCRIPTOR_INVALID"),
          "malformed foreign key descriptor did not fail closed");

  Commit(writer);
}

void VerifyDeferredCommitViolation(const std::filesystem::path& path,
                                   const api::EngineUuid& database_uuid) {
  auto writer = Begin(path, database_uuid, 406);
  const auto first = Insert(writer,
                            kDeferredTableUuid,
                            Row(scratchbird::tests::FixtureUuidLiteral("019f1000-0000-7000-8000-000000000701"),
                                {{"id", TextValue("d2")}}));
  Require(first.ok, "first deferred duplicate fixture insert failed");
  const auto second = Insert(writer,
                             kDeferredTableUuid,
                             Row(scratchbird::tests::FixtureUuidLiteral("019f1000-0000-7000-8000-000000000702"),
                                 {{"id", TextValue("d2")}}));
  Require(second.ok, "second deferred duplicate fixture was enforced before commit");
  api::EngineCommitTransactionRequest commit;
  commit.context = writer;
  const auto committed = api::EngineCommitTransaction(commit);
  Require(HasDiagnostic(committed, "CLI.CONSTRAINT_PRIMARY_KEY_VIOLATION"),
          "deferred duplicate key did not fail at commit");
  Rollback(writer);
}

void VerifyRollbackAndSavepointVisibility(const std::filesystem::path& path,
                                          const api::EngineUuid& database_uuid) {
  auto rollback_writer = Begin(path, database_uuid, 403);
  const auto rolled_insert = Insert(rollback_writer,
                                    kChildTableUuid,
                                    Row(scratchbird::tests::FixtureUuidLiteral("019f1000-0000-7000-8000-000000000601"),
                                        {{"id", TextValue("rollback-id")},
                                         {"customer_id", TextValue("c1")},
                                         {"note", TextValue("rolled")}}));
  Require(rolled_insert.ok, "rollback fixture insert failed");
  Rollback(rollback_writer);

  auto after_rollback = Begin(path, database_uuid, 404);
  const auto reinsert = Insert(after_rollback,
                               kChildTableUuid,
                               Row(scratchbird::tests::FixtureUuidLiteral("019f1000-0000-7000-8000-000000000602"),
                                   {{"id", TextValue("rollback-id")},
                                    {"customer_id", TextValue("c1")},
                                    {"note", TextValue("visible")}}));
  Require(reinsert.ok, "rolled-back key remained visible to constraint enforcement");
  Commit(after_rollback);

  auto savepoint_writer = Begin(path, database_uuid, 405);
  Require(!api::CreateMgaSavepointMarker(savepoint_writer, "sp_constraints").error,
          "create MGA savepoint marker failed");
  const auto savepoint_insert = Insert(savepoint_writer,
                                       kChildTableUuid,
                                       Row(scratchbird::tests::FixtureUuidLiteral("019f1000-0000-7000-8000-000000000603"),
                                           {{"id", TextValue("savepoint-id")},
                                            {"customer_id", TextValue("c1")},
                                            {"note", TextValue("discarded")}}));
  Require(savepoint_insert.ok, "savepoint fixture insert failed");
  Require(!api::RollbackToMgaSavepointMarker(savepoint_writer, "sp_constraints").error,
          "rollback to MGA savepoint marker failed");
  const auto after_savepoint = Insert(savepoint_writer,
                                      kChildTableUuid,
                                      Row(scratchbird::tests::FixtureUuidLiteral("019f1000-0000-7000-8000-000000000604"),
                                          {{"id", TextValue("savepoint-id")},
                                           {"customer_id", TextValue("c1")},
                                           {"note", TextValue("kept")}}));
  Require(after_savepoint.ok, "savepoint-rolled-back key remained visible to constraint enforcement");
  Commit(savepoint_writer);
}

}  // namespace

int main() {
  const auto path = MakeTempPath();
  std::cout << "constraint_artifacts=" << path.parent_path() << std::endl;
  const auto database_uuid = CreateDatabase(path);
  VerifyConstraintEnforcement(path, database_uuid);
  VerifyDeferredCommitViolation(path, database_uuid);
  VerifyRollbackAndSavepointVisibility(path, database_uuid);
  // The uniquely allocated fixture directory owns all database companions,
  // including nested relation/index segments and filespace growth artifacts.
  std::filesystem::remove_all(path.parent_path());
  std::cout << "constraint_dml_enforcement_conformance=passed\n";
  return EXIT_SUCCESS;
}
