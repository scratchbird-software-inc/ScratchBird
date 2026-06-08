// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "database_lifecycle.hpp"
#include "dml/delete_api.hpp"
#include "dml/import_execution_api.hpp"
#include "dml/native_bulk_ingest_api.hpp"
#include "dml/select_api.hpp"
#include "mga_relation_store/mga_relation_store.hpp"
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
  if (!condition) { Fail(message); }
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

void RequireDiagnosticOk(const api::EngineApiDiagnostic& diagnostic,
                         std::string_view message) {
  if (diagnostic.error) {
    std::cerr << diagnostic.code << ':' << diagnostic.message_key << ':'
              << diagnostic.detail << '\n';
    Fail(message);
  }
}

platform::u64 NowMillis() {
  return static_cast<platform::u64>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch()).count());
}

platform::u64 UniqueSeed() {
  static platform::u64 counter = 0;
  return NowMillis() + (++counter * 1000);
}

platform::TypedUuid NewUuid(platform::UuidKind kind, platform::u64 salt) {
  const auto generated = uuid::GenerateEngineIdentityV7(kind, UniqueSeed() + salt);
  Require(generated.ok(), "ODF-046 UUID generation failed");
  return generated.value;
}

std::string NewUuidText(platform::UuidKind kind, platform::u64 salt) {
  return uuid::UuidToString(NewUuid(kind, salt).value);
}

struct Fixture {
  std::filesystem::path dir;
  std::filesystem::path database_path;
  std::string database_uuid;
  std::string table_uuid;
  std::string id_index_uuid;
  platform::u64 salt = 0;

  ~Fixture() {
    std::error_code ignored;
    if (!dir.empty()) { std::filesystem::remove_all(dir, ignored); }
  }
};

api::EngineTypedValue TextValue(std::string value) {
  api::EngineTypedValue typed;
  typed.descriptor.descriptor_kind = "scalar";
  typed.descriptor.canonical_type_name = "character";
  typed.descriptor.encoded_descriptor = "canonical=character";
  typed.encoded_value = std::move(value);
  typed.is_null = typed.encoded_value == "<NULL>";
  return typed;
}

api::EngineRowValue Row(std::string id,
                        std::string parent_id,
                        std::string note) {
  api::EngineRowValue row;
  row.fields.push_back({"id", TextValue(std::move(id))});
  row.fields.push_back({"parent_id", TextValue(std::move(parent_id))});
  row.fields.push_back({"note", TextValue(std::move(note))});
  return row;
}

bool HasEvidence(const std::vector<api::EngineEvidenceReference>& evidence,
                 std::string_view kind,
                 std::string_view id) {
  for (const auto& item : evidence) {
    if (item.evidence_kind == kind && item.evidence_id == id) { return true; }
  }
  return false;
}

bool AnyEvidenceContains(const std::vector<api::EngineEvidenceReference>& evidence,
                         std::string_view token) {
  for (const auto& item : evidence) {
    if (item.evidence_kind.find(token) != std::string::npos ||
        item.evidence_id.find(token) != std::string::npos) {
      return true;
    }
  }
  return false;
}

void AssertNoRuntimeDocLeaks(const api::EngineApiResult& result) {
  const std::vector<std::string_view> forbidden = {
      "docs" "/execution-plans", "execution_plan", "findings", "contracts", "references"};
  for (const auto& evidence : result.evidence) {
    for (const auto token : forbidden) {
      Require(evidence.evidence_kind.find(token) == std::string::npos &&
                  evidence.evidence_id.find(token) == std::string::npos,
              "ODF-046 runtime evidence leaked documentation token");
    }
  }
}

void AssertNoPhysicalAppendEvidence(const api::EngineApiResult& result) {
  Require(!AnyEvidenceContains(result.evidence, "mga_row_version"),
          "ODF-046 refusal wrote row-version evidence");
  Require(!AnyEvidenceContains(result.evidence, "mga_index_store"),
          "ODF-046 refusal wrote index-store evidence");
  Require(!AnyEvidenceContains(result.evidence, "mga_hot_append_row_versions"),
          "ODF-046 refusal appended physical rows");
  Require(!AnyEvidenceContains(result.evidence, "strict_bulk_load_state"),
          "ODF-046 refusal entered strict publication lifecycle");
}

api::EngineRequestContext BaseContext(const Fixture& fixture,
                                      std::string request_id) {
  api::EngineRequestContext context;
  context.trust_mode = api::EngineTrustMode::server_isolated;
  context.request_id = std::move(request_id);
  context.database_path = fixture.database_path.string();
  context.database_uuid.canonical = fixture.database_uuid;
  context.principal_uuid.canonical =
      NewUuidText(platform::UuidKind::principal, fixture.salt + 100);
  context.session_uuid.canonical =
      NewUuidText(platform::UuidKind::object, fixture.salt + 101);
  context.security_context_present = true;
  context.identifier_profile_uuid = "sbsql_v3";
  context.language_context.language_tag = "en";
  context.language_context.default_language_tag = "en";
  context.catalog_generation_id = 46;
  context.security_epoch = 47;
  context.resource_epoch = 48;
  context.name_resolution_epoch = 49;
  return context;
}

api::EngineRequestContext Begin(const Fixture& fixture, std::string request_id) {
  api::EngineBeginTransactionRequest request;
  request.context = BaseContext(fixture, std::move(request_id));
  request.isolation_level = "read_committed";
  const auto begun = api::EngineBeginTransaction(request);
  RequireOk(begun, "ODF-046 begin transaction failed");
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
  RequireOk(api::EngineCommitTransaction(request), "ODF-046 commit failed");
}

void Rollback(const api::EngineRequestContext& context) {
  api::EngineRollbackTransactionRequest request;
  request.context = context;
  RequireOk(api::EngineRollbackTransaction(request), "ODF-046 rollback failed");
}

api::CrudTableRecord Table(const Fixture& fixture,
                           const api::EngineRequestContext& context) {
  api::CrudTableRecord table;
  table.creator_tx = context.local_transaction_id;
  table.table_uuid = fixture.table_uuid;
  table.default_name = "odf046_bulk_proof";
  table.columns.push_back({"id", "canonical=character;primary_key=true"});
  table.columns.push_back({"parent_id",
                           "canonical=character;foreign_key=" +
                               fixture.table_uuid + ":id"});
  table.columns.push_back({"note", "canonical=character;not_null=true"});
  return table;
}

api::CrudIndexRecord IdIndex(const Fixture& fixture,
                             const api::EngineRequestContext& context) {
  api::CrudIndexRecord index;
  index.creator_tx = context.local_transaction_id;
  index.index_uuid = fixture.id_index_uuid;
  index.table_uuid = fixture.table_uuid;
  index.column_name = "id";
  index.family = api::kCrudIndexFamilyBtree;
  index.profile = api::kCrudIndexProfileRowStoreScalarBtreeV1;
  index.unique = true;
  index.key_envelopes.push_back("id");
  index.key_envelopes.push_back("unique");
  return index;
}

Fixture MakeFixture(std::string name, platform::u64 salt) {
  Fixture fixture;
  fixture.salt = salt;
  fixture.dir = std::filesystem::temp_directory_path() /
                ("scratchbird_odf046_" + name + "_" +
                 std::to_string(UniqueSeed()));
  std::filesystem::create_directories(fixture.dir);
  fixture.database_path = fixture.dir / "odf046.sbdb";

  db::DatabaseCreateConfig create;
  create.path = fixture.database_path.string();
  create.database_uuid = NewUuid(platform::UuidKind::database, salt + 1);
  create.filespace_uuid = NewUuid(platform::UuidKind::filespace, salt + 2);
  create.creation_unix_epoch_millis = UniqueSeed();
  create.require_resource_seed_pack = false;
  create.allow_minimal_resource_bootstrap = true;
  create.allow_overwrite = true;
  const auto created = db::CreateDatabaseFile(create);
  Require(created.ok(), "ODF-046 database create failed");

  fixture.database_uuid = uuid::UuidToString(create.database_uuid.value);
  fixture.table_uuid = NewUuidText(platform::UuidKind::object, salt + 10);
  fixture.id_index_uuid = NewUuidText(platform::UuidKind::object, salt + 11);

  auto metadata = Begin(fixture, "odf046-metadata");
  RequireDiagnosticOk(api::AppendMgaTableMetadata(metadata, Table(fixture, metadata)),
                      "ODF-046 table metadata append failed");
  RequireDiagnosticOk(api::AppendMgaIndexMetadata(metadata,
                                                  IdIndex(fixture, metadata)),
                      "ODF-046 index metadata append failed");
  Commit(metadata);
  return fixture;
}

api::EngineExecuteImportRowsRequest ImportRequest(
    const Fixture& fixture,
    const api::EngineRequestContext& context,
    std::vector<api::EngineRowValue> rows) {
  api::EngineExecuteImportRowsRequest request;
  request.context = context;
  request.target_table.uuid.canonical = fixture.table_uuid;
  request.target_table.object_kind = "table";
  request.source.source_kind = "csv_stream";
  request.source.source_position = "row:0";
  request.format.format_family = "csv";
  request.import_policy.reject_mode = "fail_fast";
  request.import_policy.reject_payload_policy = "diagnostic_only";
  request.import_policy.resume_policy = "fail_closed";
  request.import_policy.strict_bulk_load_requested = true;
  request.checkpoint_policy.checkpoint_mode = "disabled";
  request.canonical_rows = std::move(rows);
  request.estimated_row_count =
      static_cast<api::EngineApiU64>(request.canonical_rows.size());
  request.option_envelopes.push_back("copy_append_batching=enabled");
  request.option_envelopes.push_back("feature.strict_bulk_load=enabled");
  request.option_envelopes.push_back("sorted_bulk_index_build=enabled");
  return request;
}

api::EngineExecuteNativeBulkIngestRequest NativeRequest(
    const Fixture& fixture,
    const api::EngineRequestContext& context,
    std::vector<api::EngineRowValue> rows) {
  api::EngineExecuteNativeBulkIngestRequest request;
  request.context = context;
  request.target_table.uuid.canonical = fixture.table_uuid;
  request.target_table.object_kind = "table";
  request.canonical_rows = std::move(rows);
  request.estimated_row_count =
      static_cast<api::EngineApiU64>(request.canonical_rows.size());
  request.import_policy.reject_mode = "fail_fast";
  request.import_policy.reject_payload_policy = "diagnostic_only";
  request.import_policy.resume_policy = "fail_closed";
  request.import_policy.strict_bulk_load_requested = true;
  request.option_envelopes.push_back("feature.strict_bulk_load=enabled");
  request.option_envelopes.push_back("sorted_bulk_index_build=enabled");
  return request;
}

api::EngineApiU64 SelectCount(const Fixture& fixture,
                              const api::EngineRequestContext& context) {
  api::EngineSelectRowsRequest request;
  request.context = context;
  request.source_object.uuid.canonical = fixture.table_uuid;
  request.source_object.object_kind = "table";
  request.select_projection.canonical_projection_envelopes.push_back("id");
  const auto selected = api::EngineSelectRows(request);
  RequireOk(selected, "ODF-046 select failed");
  return selected.visible_count;
}

api::EnginePredicateEnvelope EqualsPredicate(std::string column,
                                             std::string value) {
  api::EnginePredicateEnvelope predicate;
  predicate.predicate_kind = "column_equals";
  predicate.canonical_predicate_envelope = std::move(column);
  predicate.bound_values.push_back(TextValue(std::move(value)));
  return predicate;
}

void AssertAcceptedProofEvidence(const api::EngineApiResult& result,
                                 std::string_view direct_kind,
                                 std::string_view direct_id) {
  Require(HasEvidence(result.evidence, direct_kind, direct_id),
          "ODF-046 direct lane evidence missing");
  Require(HasEvidence(result.evidence,
                      "bulk_constraint_proof_route_selected",
                      "direct_physical_bulk"),
          "ODF-046 bulk proof route evidence missing");
  Require(HasEvidence(result.evidence, "bulk_unique_proof_shape", "sorted"),
          "ODF-046 unique proof shape evidence missing");
  Require(HasEvidence(result.evidence, "bulk_fk_proof_shape", "hash"),
          "ODF-046 FK proof shape evidence missing");
  Require(HasEvidence(result.evidence,
                      "bulk_constraint_disabling",
                      "false"),
          "ODF-046 constraint disabling evidence missing");
  Require(HasEvidence(result.evidence,
                      "bulk_constraint_proof_finality_authority",
                      "mga_transaction_inventory"),
          "ODF-046 proof finality authority evidence missing");
  Require(HasEvidence(result.evidence,
                      "bulk_constraint_proof_result",
                      "accepted"),
          "ODF-046 proof acceptance evidence missing");
  Require(HasEvidence(result.evidence,
                      "bulk_fk_proof_parent_existence",
                      "visible_or_batch_parent_hash_hit"),
          "ODF-046 FK parent existence proof evidence missing");
  Require(HasEvidence(result.evidence,
                      "bulk_unique_proof_sorted_duplicate_runs_absent",
                      "true"),
          "ODF-046 unique duplicate absence evidence missing");
  Require(HasEvidence(result.evidence,
                      "strict_bulk_load_state",
                      "finalize_evidence_durable"),
          "ODF-046 strict finalize evidence missing");
  Require(HasEvidence(result.evidence,
                      "strict_bulk_load_physical_publication_succeeded",
                      "row_index_append_flush"),
          "ODF-046 strict physical publication evidence missing");
  Require(HasEvidence(result.evidence,
                      "mga_finality_authority",
                      "engine_transaction_inventory"),
          "ODF-046 MGA finality authority evidence missing");
  Require(!AnyEvidenceContains(result.evidence, "constraint_disabling=true"),
          "ODF-046 accepted path disabled constraints");
  AssertNoRuntimeDocLeaks(result);
}

void AcceptedStrictCopyAndNativeUseProofs() {
  auto fixture = MakeFixture("accepted", 46000);
  auto context = Begin(fixture, "odf046-accepted");

  const auto imported = api::EngineExecuteImportRows(
      ImportRequest(fixture,
                    context,
                    {Row("root", "<NULL>", "root row"),
                     Row("child", "root", "child row")}));
  RequireOk(imported, "ODF-046 strict COPY proof import failed");
  Require(imported.accepted_rows == 2 && imported.inserted_rows == 2,
          "ODF-046 COPY row counts drifted");
  AssertAcceptedProofEvidence(imported,
                              "import_execution",
                              "direct_physical");

  const auto native = api::EngineExecuteNativeBulkIngest(
      NativeRequest(fixture,
                    context,
                    {Row("native-child", "root", "native row")}));
  RequireOk(native, "ODF-046 strict native proof import failed");
  Require(native.accepted_rows == 1 && native.inserted_rows == 1,
          "ODF-046 native row counts drifted");
  AssertAcceptedProofEvidence(native,
                              "native_bulk_ingest_lane",
                              "direct_physical");
  Require(SelectCount(fixture, context) == 3,
          "ODF-046 accepted rows were not visible after publication");
  Rollback(context);
}

void DuplicateBatchUniqueRefusesBeforeAppend() {
  auto fixture = MakeFixture("duplicate-batch", 46100);
  auto context = Begin(fixture, "odf046-duplicate-batch");
  const auto imported = api::EngineExecuteImportRows(
      ImportRequest(fixture,
                    context,
                    {Row("dup", "<NULL>", "first"),
                     Row("dup", "<NULL>", "second")}));
  Require(!imported.ok, "ODF-046 duplicate batch was accepted");
  Require(!imported.diagnostics.empty(), "ODF-046 duplicate lacked diagnostic");
  Require(imported.diagnostics.front().code ==
              "SB-BULK-CONSTRAINT-UNIQUE-DUPLICATE-BATCH",
          "ODF-046 duplicate diagnostic code drifted");
  Require(HasEvidence(imported.evidence,
                      "bulk_constraint_proof_conflict_reason",
                      "bulk_unique_proof_duplicate_in_batch"),
          "ODF-046 duplicate conflict reason evidence missing");
  AssertNoPhysicalAppendEvidence(imported);
  Require(SelectCount(fixture, context) == 0,
          "ODF-046 duplicate batch published rows");
  AssertNoRuntimeDocLeaks(imported);
  Rollback(context);
}

void PersistedUniqueConflictRefusesBeforeAppend() {
  auto fixture = MakeFixture("persisted-conflict", 46200);
  auto seed_context = Begin(fixture, "odf046-persisted-seed");
  const auto seeded = api::EngineExecuteImportRows(
      ImportRequest(fixture,
                    seed_context,
                    {Row("persisted", "<NULL>", "seed")}));
  RequireOk(seeded, "ODF-046 persisted seed insert failed");
  Commit(seed_context);

  auto conflict_context = Begin(fixture, "odf046-persisted-conflict");
  const auto duplicate = api::EngineExecuteImportRows(
      ImportRequest(fixture,
                    conflict_context,
                    {Row("persisted", "<NULL>", "duplicate")}));
  Require(!duplicate.ok, "ODF-046 persisted unique conflict was accepted");
  Require(!duplicate.diagnostics.empty(),
          "ODF-046 persisted conflict lacked diagnostic");
  Require(duplicate.diagnostics.front().code ==
              "SB-BULK-CONSTRAINT-UNIQUE-PERSISTED-CONFLICT",
          "ODF-046 persisted conflict diagnostic code drifted");
  Require(HasEvidence(duplicate.evidence,
                      "bulk_constraint_proof_conflict_reason",
                      "bulk_unique_proof_persisted_conflict"),
          "ODF-046 persisted conflict reason evidence missing");
  AssertNoPhysicalAppendEvidence(duplicate);
  Require(SelectCount(fixture, conflict_context) == 1,
          "ODF-046 persisted conflict changed visibility");
  AssertNoRuntimeDocLeaks(duplicate);
  Rollback(conflict_context);
}

void MissingForeignKeyRefusesBeforeAppend() {
  auto fixture = MakeFixture("missing-fk", 46300);
  auto context = Begin(fixture, "odf046-missing-fk");
  const auto imported = api::EngineExecuteImportRows(
      ImportRequest(fixture,
                    context,
                    {Row("child", "missing-parent", "orphan")}));
  Require(!imported.ok, "ODF-046 missing FK parent was accepted");
  Require(!imported.diagnostics.empty(), "ODF-046 missing FK lacked diagnostic");
  Require(imported.diagnostics.front().code ==
              "SB-BULK-CONSTRAINT-FK-PARENT-MISSING",
          "ODF-046 missing FK diagnostic code drifted");
  Require(HasEvidence(imported.evidence,
                      "bulk_constraint_proof_conflict_reason",
                      "bulk_fk_proof_parent_missing"),
          "ODF-046 FK conflict reason evidence missing");
  Require(HasEvidence(imported.evidence,
                      "bulk_fk_proof_missing_parent_key",
                      "missing-parent"),
          "ODF-046 missing parent key evidence missing");
  AssertNoPhysicalAppendEvidence(imported);
  Require(SelectCount(fixture, context) == 0,
          "ODF-046 missing FK published rows");
  AssertNoRuntimeDocLeaks(imported);
  Rollback(context);
}

void HistoricalIndexEntriesAreMGARecheckedBeforeConflict() {
  auto fixture = MakeFixture("historical-index", 46400);
  auto seed_context = Begin(fixture, "odf046-historical-seed");
  const auto seeded = api::EngineExecuteImportRows(
      ImportRequest(fixture,
                    seed_context,
                    {Row("reusable", "<NULL>", "seed")}));
  RequireOk(seeded, "ODF-046 historical seed insert failed");
  Commit(seed_context);

  auto delete_context = Begin(fixture, "odf046-historical-delete");
  api::EngineDeleteRowsRequest delete_request;
  delete_request.context = delete_context;
  delete_request.target_table.uuid.canonical = fixture.table_uuid;
  delete_request.target_table.object_kind = "table";
  delete_request.delete_predicate = EqualsPredicate("id", "reusable");
  const auto deleted = api::EngineDeleteRows(delete_request);
  RequireOk(deleted, "ODF-046 historical delete failed");
  Require(deleted.deleted_count == 1, "ODF-046 historical delete count drifted");
  Commit(delete_context);

  auto reuse_context = Begin(fixture, "odf046-historical-reuse");
  const auto reused = api::EngineExecuteImportRows(
      ImportRequest(fixture,
                    reuse_context,
                    {Row("reusable", "<NULL>", "reused")}));
  RequireOk(reused,
            "ODF-046 historical index entry caused false unique conflict");
  AssertAcceptedProofEvidence(reused,
                              "import_execution",
                              "direct_physical");
  Require(SelectCount(fixture, reuse_context) == 1,
          "ODF-046 reused key was not visible after stale-entry recheck");
  Rollback(reuse_context);
}

}  // namespace

int main() {
  AcceptedStrictCopyAndNativeUseProofs();
  DuplicateBatchUniqueRefusesBeforeAppend();
  PersistedUniqueConflictRefusesBeforeAppend();
  MissingForeignKeyRefusesBeforeAppend();
  HistoricalIndexEntriesAreMGARecheckedBeforeConflict();
  return EXIT_SUCCESS;
}
