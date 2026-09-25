// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "../support/engine_evidence_fixture.hpp"
#include "../support/engine_statement_fixture.hpp"
#include "catalog/column_metadata_codec.hpp"
#include "memory.hpp"
#include "database_lifecycle.hpp"
#include "dml/import_execution_api.hpp"
#include "dml/insert_api.hpp"
#include "dml/native_bulk_ingest_api.hpp"
#include "mga_relation_store/mga_relation_store.hpp"
#include "nosql/document_api.hpp"
#include "nosql/graph_api.hpp"
#include "nosql/key_value_api.hpp"
#include "nosql/time_series_api.hpp"
#include "nosql/vector_api.hpp"
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

namespace api = scratchbird::engine::internal_api;
namespace db = scratchbird::storage::database;
namespace platform = scratchbird::core::platform;
namespace uuid = scratchbird::core::uuid;

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
void RequireOk(const TResult& result, std::string_view message) {
  if (!result.ok) {
    if (!result.diagnostics.empty()) {
      std::cerr << result.diagnostics.front().code << ':'
                << result.diagnostics.front().message_key << ':'
                << result.diagnostics.front().detail << '\n';
    }
    Fail(message);
  }
}

platform::u64 NowMillis() {
  return static_cast<platform::u64>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch()).count());
}

platform::TypedUuid NewUuid(platform::UuidKind kind, platform::u64 salt) {
  const auto generated = uuid::GenerateEngineIdentityV7(kind, NowMillis() + salt);
  Require(generated.ok(), "ODF-048 UUID generation failed");
  return generated.value;
}

platform::Uuid NewNativeUuid(platform::UuidKind kind, platform::u64 salt) {
  return NewUuid(kind, salt).value;
}

struct Fixture {
  std::filesystem::path dir;
  std::filesystem::path database_path;
  platform::Uuid database_uuid;
  api::EngineRequestContext owner_context;
  std::shared_ptr<scratchbird::tests::FixtureEngineSession> session;
  platform::Uuid primary_key_uuid;
  platform::Uuid table_uuid;
  platform::Uuid index_uuid;
  platform::u64 salt = 48000;

  ~Fixture() {
    session.reset();
    if (!dir.empty()) {
      std::error_code ignored;
      std::filesystem::remove_all(dir, ignored);
    }
  }
};

api::EngineTypedValue TextValue(std::string value) {
  api::EngineTypedValue typed;
  typed.descriptor.descriptor_kind = "scalar";
  typed.descriptor.canonical_type_name = "character";
  typed.descriptor.encoded_descriptor = "canonical=character";
  typed.encoded_value = std::move(value);
  return typed;
}

api::EngineRowValue Row(std::string id, std::string note) {
  api::EngineRowValue row;
  row.fields.push_back({"id", TextValue(std::move(id))});
  row.fields.push_back({"note", TextValue(std::move(note))});
  return row;
}

bool HasEvidence(const std::vector<api::EngineEvidenceReference>& evidence,
                 std::string_view kind,
                 std::string_view id) {
  for (const auto& item : evidence) {
    if (item.evidence_kind == kind && scratchbird::tests::EvidenceTextFields(item.evidence_id) == id) {
      return true;
    }
  }
  return false;
}

std::string FieldValue(const api::EngineResultShape& shape,
                       std::size_t row_index,
                       std::string_view field_name) {
  Require(row_index < shape.rows.size(), "ODF-048 row index missing");
  for (const auto& [name, value] : shape.rows[row_index].fields) {
    if (name == field_name) {
      return value.encoded_value;
    }
  }
  return {};
}

bool HasField(const api::EngineResultShape& shape,
              std::size_t row_index,
              std::string_view field_name) {
  Require(row_index < shape.rows.size(), "ODF-048 row index missing");
  for (const auto& [name, value] : shape.rows[row_index].fields) {
    (void)value;
    if (name == field_name) {
      return true;
    }
  }
  return false;
}

void RequirePolicyEvidence(const api::EngineApiResult& result,
                           std::string_view policy) {
  Require(HasEvidence(result.evidence, "write_result_policy", policy),
          "ODF-048 write result policy evidence missing");
  Require(HasEvidence(result.evidence, "write_result_policy_applied", "true"),
          "ODF-048 write result policy apply evidence missing");
}

api::EngineRequestContext BaseContext(const Fixture& fixture,
                                      std::string request_id) {
  api::EngineRequestContext context = fixture.owner_context;
  context.trust_mode = api::EngineTrustMode::server_isolated;
  context.request_id = std::move(request_id);
  context.database_path = fixture.database_path.string();
  context.database_uuid = fixture.database_uuid;
  context.security_context_present = true;
  context.identifier_profile_uuid = "sbsql_v3";
  context.language_context.language_tag = "en";
  context.language_context.default_language_tag = "en";
  return context;
}

api::EngineRequestContext Begin(const Fixture& fixture, std::string request_id) {
  api::EngineBeginTransactionRequest request;
  request.context = BaseContext(fixture, std::move(request_id));
  request.isolation_level = "read_committed";
  const auto begun = api::EngineBeginTransaction(request);
  RequireOk(begun, "ODF-048 begin transaction failed");
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
  RequireOk(api::EngineCommitTransaction(request), "ODF-048 commit failed");
}

api::CrudTableRecord Table(const Fixture& fixture,
                           const api::EngineRequestContext& context) {
  api::CrudTableRecord table;
  table.creator_tx = context.local_transaction_id;
  table.table_uuid = fixture.table_uuid;
  table.default_name = "optimizer_deficiency_odf_048";
  api::CatalogColumnMetadata key;
  key.text = {{"canonical", "character"}, {"primary_key", "true"}};
  key.identities = {{"candidate_key_constraint_uuid", fixture.primary_key_uuid},
                    {"support_uuid", fixture.index_uuid}};
  std::string key_metadata;
  Require(api::EncodeCatalogColumnMetadata(key, &key_metadata),
          "ODF-048 primary key metadata encoding failed");
  table.columns.push_back({"id", std::move(key_metadata)});
  table.columns.push_back({"note", "canonical=character"});
  return table;
}

api::CrudIndexRecord UniqueIdIndex(const Fixture& fixture,
                                   const api::EngineRequestContext& context) {
  api::CrudIndexRecord index;
  index.creator_tx = context.local_transaction_id;
  index.index_uuid = fixture.index_uuid;
  index.table_uuid = fixture.table_uuid;
  index.column_name = "id";
  index.family = api::kCrudIndexFamilyBtree;
  index.profile = api::kCrudIndexProfileRowStoreScalarBtreeV1;
  index.unique = true;
  index.key_envelopes.push_back("id");
  index.key_envelopes.push_back("unique");
  return index;
}

Fixture MakeFixture() {
  Fixture fixture;
  fixture.dir = std::filesystem::temp_directory_path() /
                ("scratchbird_odf048_" +
                 std::to_string(NowMillis() + fixture.salt));
  std::filesystem::create_directories(fixture.dir);
  fixture.database_path = fixture.dir / "odf048.sbdb";

  db::DatabaseCreateConfig create;
  create.path = fixture.database_path.string();
  create.database_uuid = NewUuid(platform::UuidKind::database, fixture.salt + 1);
  create.filespace_uuid = NewUuid(platform::UuidKind::filespace, fixture.salt + 2);
  create.creation_unix_epoch_millis = NowMillis() + fixture.salt + 3;
  scratchbird::tests::ConfigureCredentialedFixtureBootstrap(create);
  create.allow_overwrite = true;
  const auto created = db::CreateDatabaseFile(create);
  Require(created.ok(), "ODF-048 database create failed");

  fixture.database_uuid = create.database_uuid.value;
  fixture.owner_context = scratchbird::tests::BootstrapFixtureOwnerContext(create);
  fixture.primary_key_uuid = NewNativeUuid(platform::UuidKind::object, fixture.salt + 12);
  fixture.table_uuid = NewNativeUuid(platform::UuidKind::object, fixture.salt + 10);
  fixture.index_uuid = NewNativeUuid(platform::UuidKind::object, fixture.salt + 11);

  auto metadata = Begin(fixture, "odf048-metadata");
  Require(!scratchbird::tests::PublishMgaTableFixture(metadata, Table(fixture, metadata),
          {"character", "character"}, {UniqueIdIndex(fixture, metadata)}).error,
          "ODF-048 table metadata append failed");
  Require(!api::AppendMgaIndexMetadata(metadata, UniqueIdIndex(fixture, metadata)).error,
          "ODF-048 index metadata append failed");
  Commit(metadata);
  fixture.owner_context.current_schema_uuid = metadata.current_schema_uuid;
  fixture.session = std::make_shared<scratchbird::tests::FixtureEngineSession>(
      BaseContext(fixture, "odf048-session"));
  return fixture;
}

scratchbird::tests::FixtureEngineRequest<api::EngineInsertRowsRequest> InsertRequest(
    const Fixture& fixture,
    const api::EngineRequestContext& context,
    std::string id,
    std::string policy_option) {
  scratchbird::tests::FixtureEngineRequest<api::EngineInsertRowsRequest> request(
      *fixture.session, context);
  request.target_table.uuid = fixture.table_uuid;
  request.target_table.object_kind = "table";
  request.input_rows.push_back(Row(std::move(id), "payload-note"));
  request.estimated_row_count = 1;
  if (!policy_option.empty()) {
    request.option_envelopes.push_back(std::move(policy_option));
  }
  return request;
}

scratchbird::tests::FixtureEngineRequest<api::EngineExecuteImportRowsRequest> ImportRequest(
    const Fixture& fixture,
    const api::EngineRequestContext& context,
    std::vector<api::EngineRowValue> rows,
    std::string policy_option) {
  scratchbird::tests::FixtureEngineRequest<api::EngineExecuteImportRowsRequest> request(
      *fixture.session, context);
  request.target_table.uuid = fixture.table_uuid;
  request.target_table.object_kind = "table";
  request.source.source_kind = "csv_stream";
  request.source.source_position = "row:0";
  request.format.format_family = "csv";
  request.import_policy.reject_mode = "fail_fast";
  request.import_policy.reject_payload_policy = "diagnostic_only";
  request.import_policy.resume_policy = "fail_closed";
  request.checkpoint_policy.checkpoint_mode = "disabled";
  request.canonical_rows = std::move(rows);
  request.estimated_row_count =
      static_cast<api::EngineApiU64>(request.canonical_rows.size());
  request.option_envelopes.push_back("copy_append_batching=enabled");
  if (!policy_option.empty()) {
    request.option_envelopes.push_back(std::move(policy_option));
  }
  return request;
}

scratchbird::tests::FixtureEngineRequest<api::EngineExecuteNativeBulkIngestRequest> NativeBulkRequest(
    const Fixture& fixture,
    const api::EngineRequestContext& context,
    std::vector<api::EngineRowValue> rows,
    std::string policy_option) {
  scratchbird::tests::FixtureEngineRequest<api::EngineExecuteNativeBulkIngestRequest> request(
      *fixture.session, context);
  request.target_table.uuid = fixture.table_uuid;
  request.target_table.object_kind = "table";
  request.canonical_rows = std::move(rows);
  request.estimated_row_count =
      static_cast<api::EngineApiU64>(request.canonical_rows.size());
  request.native_bulk_ingest_enabled = true;
  request.import_policy.reject_mode = "fail_fast";
  request.import_policy.reject_payload_policy = "diagnostic_only";
  request.checkpoint_policy.checkpoint_mode = "disabled";
  if (!policy_option.empty()) {
    request.option_envelopes.push_back(std::move(policy_option));
  }
  return request;
}

template <typename TRequest>
void SetNoSqlWriteRequest(TRequest* request,
                          platform::Uuid object_uuid,
                          std::string policy_option) {
  request->target_object.uuid = std::move(object_uuid);
  request->target_object.object_kind = "nosql_object";
  request->localized_names.push_back({"en", "primary", {}, "odf048", true});
  request->rows.push_back(Row("payload-id", "payload-note"));
  request->option_envelopes.push_back("write_payload=canonical_test_payload");
  if (!policy_option.empty()) {
    request->option_envelopes.push_back(std::move(policy_option));
  }
}

void RequireIdsOnly(const api::EngineApiResult& result) {
  RequireOk(result, "ODF-048 ids_only operation failed");
  RequirePolicyEvidence(result, "ids_only");
  Require(result.result_shape.result_kind == "write_ids_only",
          "ODF-048 ids_only result kind drifted");
  Require(result.result_shape.rows.size() == 1,
          "ODF-048 ids_only row count drifted");
  Require(HasField(result.result_shape, 0, "row_uuid") ||
              HasField(result.result_shape, 0, "object_uuid"),
          "ODF-048 ids_only identifier missing");
  Require(!HasField(result.result_shape, 0, "note") &&
              !HasField(result.result_shape, 0, "payload"),
          "ODF-048 ids_only leaked payload");
}

void RequireSummaryOnly(const api::EngineApiResult& result,
                        api::EngineApiU64 rows_changed) {
  RequireOk(result, "ODF-048 summary_only operation failed");
  RequirePolicyEvidence(result, "summary_only");
  Require(result.result_shape.result_kind == "write_summary_only",
          "ODF-048 summary_only result kind drifted");
  Require(result.result_shape.rows.size() == 1,
          "ODF-048 summary_only row count drifted");
  Require(FieldValue(result.result_shape, 0, "rows_changed") ==
              std::to_string(rows_changed),
          "ODF-048 summary_only rows_changed drifted");
  Require(!HasField(result.result_shape, 0, "note") &&
              !HasField(result.result_shape, 0, "payload"),
          "ODF-048 summary_only leaked payload");
}

void RequireReturnNone(const api::EngineApiResult& result) {
  RequireOk(result, "ODF-048 return_none operation failed");
  RequirePolicyEvidence(result, "return_none");
  Require(result.result_shape.result_kind == "write_return_none",
          "ODF-048 return_none result kind drifted");
  Require(result.result_shape.rows.empty(),
          "ODF-048 return_none did not suppress result rows");
}

void RequireChangedFields(const api::EngineApiResult& result) {
  RequireOk(result, "ODF-048 changed_fields operation failed");
  RequirePolicyEvidence(result, "changed_fields");
  Require(result.result_shape.result_kind == "write_changed_fields",
          "ODF-048 changed_fields result kind drifted");
  Require(result.result_shape.rows.size() == 1,
          "ODF-048 changed_fields row count drifted");
  Require(HasField(result.result_shape, 0, "changed_field_names"),
          "ODF-048 changed field names missing");
  Require(HasField(result.result_shape, 0, "changed.id") ||
              HasField(result.result_shape, 0, "changed.payload"),
          "ODF-048 changed fields payload summary missing");
}

void RequireFullPayload(const api::EngineApiResult& result) {
  RequireOk(result, "ODF-048 full_payload operation failed");
  RequirePolicyEvidence(result, "full_payload");
  Require(!result.result_shape.rows.empty(),
          "ODF-048 full_payload rows missing");
  Require(HasField(result.result_shape, 0, "note") ||
              HasField(result.result_shape, 0, "payload"),
          "ODF-048 full_payload did not expose payload");
}

void ExerciseDmlPolicies(const Fixture& fixture,
                         const api::EngineRequestContext& context) {
  RequireReturnNone(api::EngineInsertRows(
      InsertRequest(fixture, context, "insert-return-none",
                    "write_result_policy=return_none")));

  RequireIdsOnly(api::EngineInsertRows(
      InsertRequest(fixture, context, "insert-ids-only",
                    "write_result_policy=ids_only")));

  RequireSummaryOnly(api::EngineInsertRows(
      InsertRequest(fixture, context, "insert-summary-only",
                    "result_payload_policy=summary_only")),
      1);

  RequireChangedFields(api::EngineInsertRows(
      InsertRequest(fixture, context, "insert-changed-fields",
                    "odf_048.write_result_policy=changed_fields")));

  RequireFullPayload(api::EngineInsertRows(
      InsertRequest(fixture, context, "insert-full-payload",
                    "write_result_policy=full_payload")));

  const auto invalid = api::EngineInsertRows(
      InsertRequest(fixture, context, "insert-invalid-policy",
                    "write_result_policy=payload_everything"));
  Require(!invalid.ok, "ODF-048 invalid policy did not fail closed");
  Require(!invalid.diagnostics.empty() &&
              invalid.diagnostics.front().detail.find(
                  "write_result_policy_unsupported") != std::string::npos,
          "ODF-048 invalid policy diagnostic drifted");
  Require(HasEvidence(invalid.evidence, "write_result_policy_refused", "true"),
          "ODF-048 invalid policy refusal evidence missing");

  RequireSummaryOnly(api::EngineExecuteImportRows(
      ImportRequest(fixture, context,
                    {Row("copy-summary-1", "payload-note"),
                     Row("copy-summary-2", "payload-note")},
                    "result_payload_policy=summary_only")),
      2);

  RequireIdsOnly(api::EngineExecuteNativeBulkIngest(
      NativeBulkRequest(fixture, context,
                        {Row("native-ids-1", "payload-note")},
                        "write_result_policy=ids_only")));
}

void ExerciseNoSqlPolicies(const Fixture& fixture,
                           const api::EngineRequestContext& context) {
  scratchbird::tests::FixtureEngineRequest<api::EngineKeyValuePutRequest> kv(
      *fixture.session, context);
  SetNoSqlWriteRequest(&kv,
                       NewNativeUuid(platform::UuidKind::object, fixture.salt + 200),
                       "write_result_policy=return_none");
  RequireReturnNone(api::EngineKeyValuePut(kv));

  scratchbird::tests::FixtureEngineRequest<api::EngineDocumentInsertRequest> document_insert(
      *fixture.session, context);
  SetNoSqlWriteRequest(
      &document_insert,
      NewNativeUuid(platform::UuidKind::object, fixture.salt + 201),
      "result_payload_policy=ids_only");
  RequireIdsOnly(api::EngineDocumentInsert(document_insert));

  scratchbird::tests::FixtureEngineRequest<api::EngineDocumentUpdateRequest> document_update(
      *fixture.session, context);
  SetNoSqlWriteRequest(
      &document_update,
      NewNativeUuid(platform::UuidKind::object, fixture.salt + 202),
      "write_result_policy=changed_fields");
  RequireChangedFields(api::EngineDocumentUpdate(document_update));

  scratchbird::tests::FixtureEngineRequest<api::EngineVectorWriteRequest> vector_write(
      *fixture.session, context);
  SetNoSqlWriteRequest(
      &vector_write,
      NewNativeUuid(platform::UuidKind::object, fixture.salt + 203),
      "write_result_policy=summary_only");
  RequireSummaryOnly(api::EngineVectorWrite(vector_write), 1);

  scratchbird::tests::FixtureEngineRequest<api::EngineGraphWriteRequest> graph_write(
      *fixture.session, context);
  SetNoSqlWriteRequest(
      &graph_write,
      NewNativeUuid(platform::UuidKind::object, fixture.salt + 204),
      "write_result_policy=full_payload");
  RequireFullPayload(api::EngineGraphWrite(graph_write));

  scratchbird::tests::FixtureEngineRequest<api::EngineTimeSeriesAppendRequest> time_series(
      *fixture.session, context);
  SetNoSqlWriteRequest(
      &time_series,
      NewNativeUuid(platform::UuidKind::object, fixture.salt + 205),
      "odf048.write_result_policy=ids_only");
  RequireIdsOnly(api::EngineTimeSeriesAppend(time_series));
}

}  // namespace

int main() {
  auto policy = scratchbird::core::memory::DefaultLocalEngineMemoryPolicy();
  policy.policy_name = "odf048_statement_fixture";
  Require(scratchbird::core::memory::ConfigureDefaultMemoryManagerForFixture(
              policy, "odf048_statement_fixture").ok(),
          "ODF-048 memory configuration failed");
  auto fixture = MakeFixture();
  auto context = Begin(fixture, "odf048-write-result-policies");
  ExerciseDmlPolicies(fixture, context);
  ExerciseNoSqlPolicies(fixture, context);
  Commit(context);
  return EXIT_SUCCESS;
}
