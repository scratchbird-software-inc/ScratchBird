// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "../support/binary_uuid_fixture.hpp"
#include "../support/engine_statement_fixture.hpp"
#include "memory.hpp"
#include "database_lifecycle.hpp"
#include <map>
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

struct Fixture {
  std::filesystem::path path;
  api::EngineRequestContext owner;
  std::map<api::EngineUuid, std::shared_ptr<scratchbird::tests::FixtureEngineSession>> sessions;
};

std::string ColumnMetadata(std::string_view scalar_attributes,
                           const api::EngineUuid& constraint,
                           const api::EngineUuid& referenced_table = {},
                           const api::EngineUuid& support = {}) {
  api::CatalogColumnMetadata fields;
  Require(api::AdmitCatalogColumnMetadata(scalar_attributes, &fields),
          "invalid scalar constraint fixture metadata");
  fields.identities.emplace("constraint_uuid", constraint);
  if (fields.text.contains("primary_key") && fields.text.at("primary_key") == "true") {
    fields.identities.emplace("candidate_key_constraint_uuid", constraint);
    if (!support.is_nil()) fields.identities.emplace("support_uuid", support);
  }
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

api::EngineRequestContext CreateDatabase(const std::filesystem::path& path) {
  db::DatabaseCreateConfig create;
  create.path = path.string();
  create.database_uuid = uuid::GenerateEngineIdentityV7(UuidKind::database, 1779800001000).value;
  create.filespace_uuid = uuid::GenerateEngineIdentityV7(UuidKind::filespace, 1779800001001).value;
  create.page_size = 16384;
  create.creation_unix_epoch_millis = 1779800001002;
  scratchbird::tests::ConfigureCredentialedFixtureBootstrap(create);
  create.allow_overwrite = true;
  const auto created = db::CreateDatabaseFile(create);
  if (!created.ok()) {
    std::cerr << created.diagnostic.diagnostic_code << ":" << created.diagnostic.message_key << '\n';
  }
  Require(created.ok(), "constraint DML database create failed");
  return scratchbird::tests::BootstrapFixtureOwnerContext(create);
}

api::EngineRequestContext BaseContext(const Fixture& fixture,
                                      std::uint32_t session_ordinal) {
  auto context = fixture.owner;
  context.request_id = "prf-constraint-dml";
  context.session_uuid = scratchbird::tests::FixtureUuid(1547, session_ordinal);
  context.trace_tags.push_back("PRF-030");
  context.trace_tags.push_back("PRF-035");
  return context;
}

api::EngineRequestContext Begin(Fixture& fixture,
                                std::uint32_t session_ordinal) {
  api::EngineBeginTransactionRequest request;
  request.context = BaseContext(fixture, session_ordinal);
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
  if (!fixture.sessions.contains(context.session_uuid)) {
    fixture.sessions.emplace(context.session_uuid,
        std::make_shared<scratchbird::tests::FixtureEngineSession>(context));
  }
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
  typed.descriptor.canonical_type_name = "character";
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

api::CrudTableRecord Table(std::string name, api::EngineUuid table_uuid,
                           std::vector<std::pair<std::string, std::string>> columns) {
  api::CrudTableRecord table;
  table.table_uuid = std::move(table_uuid);
  table.default_name = std::move(name);
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

void SeedConstraintMetadata(api::EngineRequestContext& context) {
  const auto parent = Table("constraint_parent", kParentTableUuid,
                            {{"id", ColumnMetadata("canonical=character;nullable=false;primary_key=true", scratchbird::tests::FixtureUuid(1547, 810), {}, kParentPkIndexUuid)},
                             {"name", "canonical=character;nullable=false"}});
  const auto child = Table("constraint_child", kChildTableUuid,
      {{"id", ColumnMetadata("canonical=character;nullable=false;primary_key=true", scratchbird::tests::FixtureUuid(1547, 811), {}, kChildPkIndexUuid)},
       {"customer_id",
        ColumnMetadata("canonical=character;nullable=false", scratchbird::tests::FixtureUuid(1547, 803), kParentTableUuid)},
       {"amount", ColumnMetadata("canonical=int64;default=literal:1;check=gt:0", scratchbird::tests::FixtureUuid(1547, 812))},
       {"note", ColumnMetadata("canonical=character;nullable=false", scratchbird::tests::FixtureUuid(1547, 813))}});
  const auto no_index = Table("constraint_noindex", kNoIndexTableUuid,
                              {{"id", ColumnMetadata("canonical=character;nullable=false;primary_key=true", scratchbird::tests::FixtureUuid(1547, 814))}});
  const auto exclusion = Table("constraint_exclusion", kExclusionTableUuid,
                               {{"span", ColumnMetadata("canonical=character;exclusion=true", scratchbird::tests::FixtureUuid(1547, 815))}});
  const auto deferred = Table("constraint_deferred", kDeferredTableUuid,
                              {{"id", ColumnMetadata("canonical=character;nullable=false;primary_key=true;deferrable=true", scratchbird::tests::FixtureUuid(1547, 816), {}, kDeferredPkIndexUuid)}});
  const auto malformed_fk = Table("constraint_malformedfk", kMalformedFkTableUuid,
                                  {{"parent_id", ColumnMetadata("canonical=character;foreign_key=true", scratchbird::tests::FixtureUuid(1547, 817))}});

  Require(!scratchbird::tests::PublishMgaTableFixture(context, parent, {"character", "character"}, {UniqueIndex(kParentPkIndexUuid, kParentTableUuid, "id")}).error, "append parent table metadata failed");
  Require(!api::AppendMgaIndexMetadata(context, UniqueIndex(kParentPkIndexUuid, kParentTableUuid, "id")).error,
          "append parent pk index metadata failed");
  Require(!scratchbird::tests::PublishMgaTableFixture(context, child, {"character", "character", "int64", "character"}, {UniqueIndex(kChildPkIndexUuid, kChildTableUuid, "id")}).error, "append child table metadata failed");
  Require(!api::AppendMgaIndexMetadata(context, UniqueIndex(kChildPkIndexUuid, kChildTableUuid, "id")).error,
          "append child pk index metadata failed");
  Require(!scratchbird::tests::PublishMgaTableFixture(context, no_index, {"character"}, {}).error, "append no-index table metadata failed");
  Require(!scratchbird::tests::PublishMgaTableFixture(context, exclusion, {"character"}, {}).error, "append exclusion table metadata failed");
  Require(!scratchbird::tests::PublishMgaTableFixture(context, deferred, {"character"}, {UniqueIndex(kDeferredPkIndexUuid, kDeferredTableUuid, "id")}).error, "append deferred table metadata failed");
  Require(!api::AppendMgaIndexMetadata(context, UniqueIndex(kDeferredPkIndexUuid, kDeferredTableUuid, "id")).error,
          "append deferred pk index metadata failed");
  Require(!scratchbird::tests::PublishMgaTableFixture(context, malformed_fk, {"character"}, {}).error, "append malformed-fk table metadata failed");
}

api::EngineInsertRowsResult Insert(Fixture& fixture, const api::EngineRequestContext& context,
                                   api::EngineUuid table_uuid,
                                   api::EngineRowValue row) {
  scratchbird::tests::FixtureEngineRequest<api::EngineInsertRowsRequest> request(
      *fixture.sessions.at(context.session_uuid), context);
  request.target_table.uuid = std::move(table_uuid);
  request.target_table.object_kind = "table";
  request.input_rows.push_back(std::move(row));
  return api::EngineInsertRows(request);
}

api::EngineDeleteRowsResult DeleteParent(Fixture& fixture, const api::EngineRequestContext& context, std::string id) {
  scratchbird::tests::FixtureEngineRequest<api::EngineDeleteRowsRequest> request(
      *fixture.sessions.at(context.session_uuid), context);
  request.target_table.uuid = scratchbird::tests::FixtureUuidLiteral("019f1000-0000-7000-8000-000000000101");
  request.target_table.object_kind = "table";
  request.delete_predicate.predicate_kind = "column_equals";
  request.delete_predicate.canonical_predicate_envelope = "id";
  request.delete_predicate.bound_values.push_back(TextValue(std::move(id)));
  return api::EngineDeleteRows(request);
}

api::EngineUpdateRowsResult UpdateRow(Fixture& fixture, const api::EngineRequestContext& context,
                                      api::EngineUuid table_uuid,
                                      api::EngineUuid row_uuid,
                                      std::vector<std::pair<std::string, api::EngineTypedValue>> assignments) {
  scratchbird::tests::FixtureEngineRequest<api::EngineUpdateRowsRequest> request(
      *fixture.sessions.at(context.session_uuid), context);
  request.target_table.uuid = std::move(table_uuid);
  request.target_table.object_kind = "table";
  request.update_predicate.predicate_kind = "row_uuid_match";
  request.update_predicate.row_uuid = row_uuid;
  request.assignments = std::move(assignments);
  return api::EngineUpdateRows(request);
}

void VerifyConstraintEnforcement(Fixture& fixture) {
  auto setup = Begin(fixture, 401);
  SeedConstraintMetadata(setup);
  fixture.owner.current_schema_uuid = setup.current_schema_uuid;
  Commit(setup);

  auto writer = Begin(fixture, 402);
  auto parent = Insert(fixture, writer,
                       kParentTableUuid,
                       Row(scratchbird::tests::FixtureUuidLiteral("019f1000-0000-7000-8000-000000000501"),
                           {{"id", TextValue("c1")}, {"name", TextValue("customer")}}));
  Require(parent.ok, "parent insert failed");

  auto child = Insert(fixture, writer,
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

  const auto update_not_null = UpdateRow(fixture, writer,
                                         kChildTableUuid,
                                         scratchbird::tests::FixtureUuidLiteral("019f1000-0000-7000-8000-000000000502"),
                                         {{"note", TextValue({}, true)}});
  Require(HasDiagnostic(update_not_null, "CLI.CONSTRAINT_NOT_NULL_VIOLATION"),
          "UPDATE did not enforce canonical NOT NULL diagnostics");

  const auto missing_not_null = Insert(fixture, writer,
                                       kChildTableUuid,
                                       Row(scratchbird::tests::FixtureUuidLiteral("019f1000-0000-7000-8000-000000000503"),
                                           {{"id", TextValue("o2")},
                                            {"customer_id", TextValue("c1")}}));
  Require(HasDiagnostic(missing_not_null, "CLI.CONSTRAINT_NOT_NULL_VIOLATION"),
          "missing NOT NULL value did not emit canonical diagnostic");

  const auto bad_check = Insert(fixture, writer,
                                kChildTableUuid,
                                Row(scratchbird::tests::FixtureUuidLiteral("019f1000-0000-7000-8000-000000000504"),
                                    {{"id", TextValue("o3")},
                                     {"customer_id", TextValue("c1")},
                                     {"amount", TextValue("0")},
                                     {"note", TextValue("bad")}}));
  Require(HasDiagnostic(bad_check, "CLI.CONSTRAINT_CHECK_VIOLATION"),
          "CHECK violation did not emit canonical diagnostic");

  const auto bad_fk = Insert(fixture, writer,
                             kChildTableUuid,
                             Row(scratchbird::tests::FixtureUuidLiteral("019f1000-0000-7000-8000-000000000505"),
                                 {{"id", TextValue("o4")},
                                  {"customer_id", TextValue("missing")},
                                  {"note", TextValue("bad")}}));
  Require(HasDiagnostic(bad_fk, "CLI.CONSTRAINT_FOREIGN_KEY_VIOLATION"),
          "foreign key miss did not emit canonical diagnostic");

  const auto duplicate_pk = Insert(fixture, writer,
                                   kChildTableUuid,
                                   Row(scratchbird::tests::FixtureUuidLiteral("019f1000-0000-7000-8000-000000000506"),
                                       {{"id", TextValue("o1")},
                                        {"customer_id", TextValue("c1")},
                                        {"note", TextValue("dup")}}));
  Require(HasDiagnostic(duplicate_pk, "CLI.CONSTRAINT_PRIMARY_KEY_VIOLATION"),
          "duplicate primary key did not emit canonical diagnostic");

  const auto update_parent_key = UpdateRow(fixture, writer,
                                           kParentTableUuid,
                                           scratchbird::tests::FixtureUuidLiteral("019f1000-0000-7000-8000-000000000501"),
                                           {{"id", TextValue("c2")}});
  Require(HasDiagnostic(update_parent_key, "CLI.CONSTRAINT_FOREIGN_KEY_VIOLATION"),
          "referenced parent key update was not restricted");

  const auto delete_parent = DeleteParent(fixture, writer, "c1");
  Require(HasDiagnostic(delete_parent, "CLI.CONSTRAINT_FOREIGN_KEY_VIOLATION"),
          "referenced parent delete was not restricted");

  const auto no_support = Insert(fixture, writer,
                                 kNoIndexTableUuid,
                                 Row(scratchbird::tests::FixtureUuidLiteral("019f1000-0000-7000-8000-000000000507"),
                                     {{"id", TextValue("n1")}}));
  Require(HasDiagnostic(no_support, "CLI.SUPPORT_STRUCTURE_UNAVAILABLE"),
          "primary key without backing index did not fail closed");

  const auto exclusion = Insert(fixture, writer,
                                kExclusionTableUuid,
                                Row(scratchbird::tests::FixtureUuidLiteral("019f1000-0000-7000-8000-000000000508"),
                                    {{"span", TextValue("1,10")}}));
  Require(exclusion.ok, "exclusion constraint first interval was rejected");
  Require(HasEvidence(exclusion, "constraint_exclusion", "span"),
          "exclusion constraint evidence missing");

  const auto exclusion_conflict = Insert(fixture, writer,
                                         kExclusionTableUuid,
                                         Row(scratchbird::tests::FixtureUuidLiteral("019f1000-0000-7000-8000-000000000511"),
                                             {{"span", TextValue("5,15")}}));
  Require(HasDiagnostic(exclusion_conflict, "CLI.CONSTRAINT_EXCLUSION_VIOLATION"),
          "overlapping exclusion value did not emit canonical diagnostic");

  const auto deferred = Insert(fixture, writer,
                               kDeferredTableUuid,
                               Row(scratchbird::tests::FixtureUuidLiteral("019f1000-0000-7000-8000-000000000509"),
                                   {{"id", TextValue("d1")}}));
  Require(deferred.ok, "deferred constraint valid row was rejected before commit");
  Require(HasEvidence(deferred, "constraint_deferred_pending_check", scratchbird::tests::FixtureUuid(1547, 816)),
          "deferred constraint pending-check evidence missing");

  const auto malformed_fk = Insert(fixture, writer,
                                   kMalformedFkTableUuid,
                                   Row(scratchbird::tests::FixtureUuidLiteral("019f1000-0000-7000-8000-000000000510"),
                                       {{"parent_id", TextValue("c1")}}));
  Require(HasDiagnostic(malformed_fk, "CLI.CONSTRAINT_DESCRIPTOR_INVALID"),
          "malformed foreign key descriptor did not fail closed");

  Commit(writer);
}

void VerifyDeferredCommitViolation(Fixture& fixture) {
  auto writer = Begin(fixture, 406);
  const auto first = Insert(fixture, writer,
                            kDeferredTableUuid,
                            Row(scratchbird::tests::FixtureUuidLiteral("019f1000-0000-7000-8000-000000000701"),
                                {{"id", TextValue("d2")}}));
  Require(first.ok, "first deferred duplicate fixture insert failed");
  const auto second = Insert(fixture, writer,
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

void VerifyRollbackAndSavepointVisibility(Fixture& fixture) {
  auto rollback_writer = Begin(fixture, 403);
  const auto rolled_insert = Insert(fixture, rollback_writer,
                                    kChildTableUuid,
                                    Row(scratchbird::tests::FixtureUuidLiteral("019f1000-0000-7000-8000-000000000601"),
                                        {{"id", TextValue("rollback-id")},
                                         {"customer_id", TextValue("c1")},
                                         {"note", TextValue("rolled")}}));
  Require(rolled_insert.ok, "rollback fixture insert failed");
  Rollback(rollback_writer);

  auto after_rollback = Begin(fixture, 404);
  const auto reinsert = Insert(fixture, after_rollback,
                               kChildTableUuid,
                               Row(scratchbird::tests::FixtureUuidLiteral("019f1000-0000-7000-8000-000000000602"),
                                   {{"id", TextValue("rollback-id")},
                                    {"customer_id", TextValue("c1")},
                                    {"note", TextValue("visible")}}));
  Require(reinsert.ok, "rolled-back key remained visible to constraint enforcement");
  Commit(after_rollback);

  auto savepoint_writer = Begin(fixture, 405);
  Require(!api::CreateMgaSavepointMarker(savepoint_writer, "sp_constraints").error,
          "create MGA savepoint marker failed");
  const auto savepoint_insert = Insert(fixture, savepoint_writer,
                                       kChildTableUuid,
                                       Row(scratchbird::tests::FixtureUuidLiteral("019f1000-0000-7000-8000-000000000603"),
                                           {{"id", TextValue("savepoint-id")},
                                            {"customer_id", TextValue("c1")},
                                            {"note", TextValue("discarded")}}));
  Require(savepoint_insert.ok, "savepoint fixture insert failed");
  Require(!api::RollbackToMgaSavepointMarker(savepoint_writer, "sp_constraints").error,
          "rollback to MGA savepoint marker failed");
  const auto after_savepoint = Insert(fixture, savepoint_writer,
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
  auto policy = scratchbird::core::memory::DefaultLocalEngineMemoryPolicy();
  policy.policy_name = "constraint_dml_fixture";
  Require(scratchbird::core::memory::ConfigureDefaultMemoryManagerForFixture(
              policy, "constraint_dml_fixture").ok(), "constraint memory configuration failed");
  Fixture fixture;
  fixture.path = MakeTempPath();
  std::cout << "constraint_artifacts=" << fixture.path.parent_path() << std::endl;
  fixture.owner = CreateDatabase(fixture.path);
  VerifyConstraintEnforcement(fixture);
  VerifyDeferredCommitViolation(fixture);
  VerifyRollbackAndSavepointVisibility(fixture);
  fixture.sessions.clear();
  // The uniquely allocated fixture directory owns all database companions,
  // including nested relation/index segments and filespace growth artifacts.
  std::filesystem::remove_all(fixture.path.parent_path());
  std::cout << "constraint_dml_enforcement_conformance=passed\n";
  return EXIT_SUCCESS;
}
