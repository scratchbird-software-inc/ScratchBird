// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "../support/binary_uuid_fixture.hpp"
#include "../support/engine_statement_fixture.hpp"
#include "../support/native_int64_fixture.hpp"
#include "memory.hpp"
#include "database_lifecycle.hpp"
#include <map>
#include "catalog/column_metadata_codec.hpp"
#include "dml/delete_api.hpp"
#include "dml/insert_api.hpp"
#include "dml/update_api.hpp"
#include "dml/constraint_enforcement.hpp"
#include "dml/mga_relation_read_view.hpp"
#include "datatype_type_codec_identity_v3.hpp"
#include "datatype_operations.hpp"
#include "mga_relation_store/mga_relation_store.hpp"
#include "transaction/transaction_api.hpp"
#include "uuid.hpp"

#include <chrono>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <tuple>
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
    // The private returning-row carrier retains native octets, not display
    // digits. The public parser owns rendering of the stored INT64 value.
    if (field == "amount" && typed.state == api::EngineValueState::value &&
        !typed.isSqlNull() && typed.binary_value.empty() &&
        typed.encoded_value == std::string("\x01\0\0\0\0\0\0\0", 8)) {
      saw_default_amount = true;
    }
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

  const auto child_storage = api::LoadMgaRelationStorageDescriptor(writer, kChildTableUuid);
  Require(child_storage.ok, "CHECK input requires actual stored column binding");
  const api::EngineDescriptor* amount_descriptor = nullptr;
  for (const auto& column : child_storage.descriptor.columns)
    if (column.canonical_name_key == "amount") amount_descriptor = &column.value_descriptor;
  Require(amount_descriptor, "CHECK amount column missing");
  const auto bad_check = Insert(fixture, writer,
                                kChildTableUuid,
                                Row(scratchbird::tests::FixtureUuidLiteral("019f1000-0000-7000-8000-000000000504"),
                                    {{"id", TextValue("o3")},
                                     {"customer_id", TextValue("c1")},
                                     {"amount", scratchbird::tests::NativeInt64Fixture(*amount_descriptor, "0")},
                                     {"note", TextValue("bad")}}));
  Require(HasDiagnostic(bad_check, "CLI.CONSTRAINT_CHECK_VIOLATION"),
          "CHECK violation did not emit canonical diagnostic");
  bool predicate_failed = false;
  for (const auto& diagnostic : bad_check.diagnostics)
    predicate_failed |= diagnostic.detail.find("detail=check_gt_failed") != std::string::npos;
  Require(predicate_failed, "CHECK zero test did not reach the actual predicate");

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

// Exercise the real non-mutating row validator without a store fallback. This
// is representation/CHECK conformance, not a fabricated write capability.
void VerifyNativeIntegerCheckCohorts() {
  namespace dt = scratchbird::core::datatypes;
  unsigned profiles = 0;
  for (const auto& [snapshot, generation] : std::array{
           std::pair{dt::kDatatypeCohortV5, std::uint64_t{5}},
           std::pair{dt::kDatatypeCohortV10, std::uint64_t{10}},
           std::pair{dt::kDatatypeCohortV11, std::uint64_t{11}}}) {
    for (const auto& [type, width, signed_value] : std::array{
        std::tuple{dt::CanonicalTypeId::int8, 1u, true},
        std::tuple{dt::CanonicalTypeId::int16, 2u, true},
        std::tuple{dt::CanonicalTypeId::int32, 4u, true},
        std::tuple{dt::CanonicalTypeId::int64, 8u, true},
        std::tuple{dt::CanonicalTypeId::int128, 16u, true},
        std::tuple{dt::CanonicalTypeId::uint8, 1u, false},
        std::tuple{dt::CanonicalTypeId::uint16, 2u, false},
        std::tuple{dt::CanonicalTypeId::uint32, 4u, false},
        std::tuple{dt::CanonicalTypeId::uint64, 8u, false},
        std::tuple{dt::CanonicalTypeId::uint128, 16u, false}}) {
      const dt::DatatypeTypeCodecIdentityRowV3* selected = nullptr;
      for (const auto& candidate : dt::CurrentDatatypeTypeCodecIdentityRowsV3()) {
        if (candidate.legacy_fields.catalog_snapshot_uuid == snapshot &&
            candidate.legacy_fields.canonical_binary_type_code == static_cast<std::uint32_t>(type)) {
          Require(!selected, "ambiguous integer CHECK fixture type");
          selected = &candidate;
        }
      }
      Require(selected && selected->legacy_fields.canonical_value_exact_bytes == width,
              "integer CHECK fixture requires exact registry width");
      const auto& identity = selected->legacy_fields;
      api::EngineRequestContext context;
      context.datatype_catalog_snapshot_uuid = snapshot;
      context.datatype_catalog_generation = context.datatype_registry_generation = generation;
      api::ConstraintDmlValidationCache context_cache;
      api::StoreConstraintDmlProof(&context_cache, context, "cohort_control", "same_identity", "retained");
      Require(api::FindConstraintDmlProofPayload(&context_cache, context, "cohort_control", "same_identity") ==
                  std::optional<std::string>{"retained"}, "unchanged proof context missed");
      api::EngineColumnDefinition column;
      column.ordinal = 0;
      column.requested_column_uuid = scratchbird::tests::FixtureUuid(1547, 901);
      auto& descriptor = column.descriptor;
      descriptor.descriptor_uuid = scratchbird::tests::FixtureUuid(1547, 902);
      descriptor.descriptor_kind = "scalar";
      descriptor.canonical_type_name = dt::CanonicalTypeName(type);
      descriptor.datatype_descriptor_uuid = identity.descriptor_uuid;
      descriptor.datatype_descriptor_generation = identity.descriptor_generation;
      descriptor.type_uuid = identity.type_uuid;
      descriptor.datatype_cohort = {snapshot, generation, generation};
      const auto encoded = ColumnMetadata("check=gt:0", scratchbird::tests::FixtureUuid(1547, 903));
      auto table = Table("integer_check", kChildTableUuid, {{"amount", encoded}});
      table.bound_columns.push_back(column);
      const auto check = [&](const auto& owner, const auto& source, api::CrudStoredValue bytes,
                             api::ConstraintDmlValidationCache* cache = nullptr) {
        return api::ValidateImmediateRowConstraints(owner, api::MgaRelationReadView{}, source,
            scratchbird::tests::FixtureUuid(1547, 904), {{"amount", std::move(bytes)}}, "insert", cache);
      };
      std::string one(width, '\0'); one[0] = 1;
      std::string maximum(width, static_cast<char>(0xff));
      if (signed_value) maximum.back() = 0x7f;
      Require(check(context, table, one).ok && check(context, table, maximum).ok,
              "native integer CHECK rejected positive or maximum value");
      api::ConstraintDmlValidationCache warm_cache;
      Require(check(context, table, one, &warm_cache).ok &&
              !warm_cache.validation_proof_payloads.empty(), "integer CHECK did not cache proof");
      const auto proof_count = warm_cache.validation_proof_payloads.size();
      Require(check(context, table, one, &warm_cache).ok &&
              warm_cache.validation_proof_payloads.size() == proof_count,
              "unchanged integer CHECK proof was not reusable");
      const auto null = api::CrudStoredValue::SqlNull();
      api::ConstraintDmlValidationCache null_cache;
      Require(check(context, table, null).ok && check(context, table, null, &null_cache).ok &&
              !null_cache.validation_proof_payloads.empty(), "valid NULL CHECK lost UNKNOWN-pass semantics");
      std::string minimum(width, '\0');
      if (signed_value) minimum.back() = static_cast<char>(0x80);
      const auto default_value = [&](const auto& owner, const auto& source, const std::string& literal,
                                     api::CrudValueFields input = {}) {
        auto target = source;
        target.columns[0].second = ColumnMetadata("default=literal:" + literal,
            scratchbird::tests::FixtureUuid(1547, 903));
        return api::ApplyConstraintDefaultsForInsert(owner, target, input);
      };
      const auto decimal = [&](const std::string& bytes) {
        std::string full = bytes;
        full.resize(16, signed_value && (static_cast<unsigned char>(bytes.back()) & 0x80)
            ? static_cast<char>(0xff) : '\0');
        std::string text;
        Require(signed_value ? dt::DecodeCanonicalInt128Value(full, &text)
                             : dt::DecodeCanonicalUint128Value(full, &text),
                "integer default fixture decimal oracle failed");
        return text;
      };
      for (const auto& bytes : {one, std::string(width, '\0'), minimum, maximum}) {
        for (bool explicit_default : {false, true}) {
          const auto result = default_value(context, table, decimal(bytes), explicit_default
              ? api::CrudValueFields{{"amount", api::CrudStoredValue::DefaultRequested()}}
              : api::CrudValueFields{});
          Require(result.ok && result.values.size() == 1 && result.values[0].second.isPresent() &&
                  result.values[0].second.bytes == bytes, "integer default lost exact native bytes");
        }
      }
      const auto increment_magnitude = [](std::string value) {
        for (std::size_t i = value.size(); i > 0; --i) {
          if (value[i - 1] == '-') { value.insert(i, "1"); return value; }
          if (value[i - 1] != '9') { ++value[i - 1]; return value; }
          value[i - 1] = '0';
        }
        return "1" + value;
      };
      for (const auto& invalid : {increment_magnitude(decimal(maximum)),
                                 signed_value ? increment_magnitude(decimal(minimum)) : std::string("-1"),
                                 std::string("not-a-number")}) {
        const auto refused = default_value(context, table, invalid);
        Require(!refused.ok && refused.diagnostic.error && refused.values.empty(),
                "integer default overflow/malformed input admitted or partially published");
      }
      const auto retained = default_value(context, table, "not-a-number", {{"amount", one}});
      Require(retained.ok && retained.values[0].second.bytes == one,
              "default replaced an explicit value");
      for (const auto& bytes : {std::string(width, '\0'), minimum,
                               std::string(width - 1, '\0'), std::string(width + 1, '\0')}) {
        const auto refused = check(context, table, bytes);
        Require(!refused.ok && refused.diagnostic.code == "CLI.CONSTRAINT_CHECK_VIOLATION",
                "native integer CHECK admitted zero/minimum or malformed width");
      }
      for (unsigned field = 0; field < 6; ++field) {
        auto stale = table;
        auto& binding = stale.bound_columns[0].descriptor;
        if (field == 0) binding.datatype_cohort.catalog_snapshot_uuid = {};
        if (field == 1) ++binding.datatype_cohort.catalog_generation;
        if (field == 2) ++binding.datatype_cohort.registry_generation;
        if (field == 3) binding.datatype_cohort = generation == 5
            ? api::EngineDatatypeCohort{dt::kDatatypeCohortV10, 10, 10}
            : api::EngineDatatypeCohort{dt::kDatatypeCohortV5, 5, 5};
        if (field == 4) binding.type_uuid = kChildTableUuid;
        if (field == 5) ++binding.datatype_descriptor_generation;
        const auto refused = check(context, stale, one);
        Require(!refused.ok && refused.diagnostic.error,
                "stale integer CHECK binding admitted");
        auto cached = warm_cache;
        const auto cached_refusal = check(context, stale, one, &cached);
        Require(!cached_refusal.ok && cached_refusal.diagnostic.error,
                "cached integer CHECK admitted stale source binding");
        auto cached_null = null_cache;
        Require(!check(context, stale, null).ok && !check(context, stale, null, &cached_null).ok,
                "NULL CHECK admitted stale source binding");
        Require(!default_value(context, stale, "1").ok, "default admitted stale source binding");
      }
      for (unsigned field = 0; field < 4; ++field) {
        auto stale = context;
        if (field == 0) stale.datatype_catalog_snapshot_uuid = {};
        if (field == 1) ++stale.datatype_catalog_generation;
        if (field == 2) ++stale.datatype_registry_generation;
        if (field == 3) {
          stale.datatype_catalog_snapshot_uuid = generation == 5
              ? dt::kDatatypeCohortV10 : dt::kDatatypeCohortV5;
          stale.datatype_catalog_generation = stale.datatype_registry_generation = generation == 5 ? 10 : 5;
        }
        std::vector<api::EngineEvidenceReference> refused_evidence;
        Require(!api::FindConstraintDmlProofPayload(&context_cache, stale, "cohort_control", "same_identity",
                    &refused_evidence).has_value(), "proof cache ignored changed request datatype cohort");
        Require(std::any_of(refused_evidence.begin(), refused_evidence.end(), [](const auto& evidence) {
                  const auto* text = std::get_if<std::string>(&evidence.evidence_id);
                  return evidence.evidence_kind == "constraint_proof_refusal" && text &&
                         *text == "cohort_control:datatype_cohort_mismatch";
                }), "proof cache omitted datatype cohort mismatch evidence");
        auto cached = warm_cache;
        const auto refused = check(stale, table, one, &cached);
        Require(!refused.ok && refused.diagnostic.error,
                "cached integer CHECK admitted stale request cohort");
        auto cached_null = null_cache;
        Require(!check(stale, table, null).ok && !check(stale, table, null, &cached_null).ok,
                "NULL CHECK admitted stale request cohort");
        Require(!default_value(stale, table, "1").ok, "default admitted stale request cohort");
      }
      ++profiles;
    }
  }
  Require(profiles == 30, "integer CHECK cohort matrix incomplete");
}

}  // namespace

int main() {
  VerifyNativeIntegerCheckCohorts();
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
