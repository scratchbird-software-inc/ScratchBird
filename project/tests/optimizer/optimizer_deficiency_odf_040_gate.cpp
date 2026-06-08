// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "database_lifecycle.hpp"
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
  Require(generated.ok(), "ODF-040 UUID generation failed");
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
  std::string index_uuid;
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
    if (item.evidence_kind == kind && item.evidence_id == id) { return true; }
  }
  return false;
}

bool EvidenceContains(const std::vector<api::EngineEvidenceReference>& evidence,
                      std::string_view kind,
                      std::string_view token) {
  for (const auto& item : evidence) {
    if (item.evidence_kind == kind &&
        item.evidence_id.find(token) != std::string::npos) {
      return true;
    }
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
  context.catalog_generation_id = 40;
  context.security_epoch = 41;
  context.resource_epoch = 42;
  context.name_resolution_epoch = 43;
  return context;
}

api::EngineRequestContext Begin(const Fixture& fixture, std::string request_id) {
  api::EngineBeginTransactionRequest request;
  request.context = BaseContext(fixture, std::move(request_id));
  request.isolation_level = "read_committed";
  const auto begun = api::EngineBeginTransaction(request);
  RequireOk(begun, "ODF-040 begin transaction failed");
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
  RequireOk(api::EngineCommitTransaction(request), "ODF-040 commit failed");
}

void Rollback(const api::EngineRequestContext& context) {
  api::EngineRollbackTransactionRequest request;
  request.context = context;
  RequireOk(api::EngineRollbackTransaction(request), "ODF-040 rollback failed");
}

api::CrudTableRecord Table(const Fixture& fixture,
                           const api::EngineRequestContext& context) {
  api::CrudTableRecord table;
  table.creator_tx = context.local_transaction_id;
  table.table_uuid = fixture.table_uuid;
  table.default_name = "odf040_direct_bulk";
  table.columns.push_back({"id", "canonical=character;primary_key=true"});
  table.columns.push_back({"note", "canonical=character;not_null=true"});
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

Fixture MakeFixture(std::string name, platform::u64 salt) {
  Fixture fixture;
  fixture.salt = salt;
  fixture.dir = std::filesystem::temp_directory_path() /
                ("scratchbird_odf040_" + name + "_" +
                 std::to_string(UniqueSeed()));
  std::filesystem::create_directories(fixture.dir);
  fixture.database_path = fixture.dir / "odf040.sbdb";

  db::DatabaseCreateConfig create;
  create.path = fixture.database_path.string();
  create.database_uuid = NewUuid(platform::UuidKind::database, salt + 1);
  create.filespace_uuid = NewUuid(platform::UuidKind::filespace, salt + 2);
  create.creation_unix_epoch_millis = UniqueSeed();
  create.require_resource_seed_pack = false;
  create.allow_minimal_resource_bootstrap = true;
  create.allow_overwrite = true;
  const auto created = db::CreateDatabaseFile(create);
  Require(created.ok(), "ODF-040 database create failed");

  fixture.database_uuid = uuid::UuidToString(create.database_uuid.value);
  fixture.table_uuid = NewUuidText(platform::UuidKind::object, salt + 10);
  fixture.index_uuid = NewUuidText(platform::UuidKind::object, salt + 11);

  auto metadata = Begin(fixture, "odf040-metadata");
  RequireDiagnosticOk(api::AppendMgaTableMetadata(metadata, Table(fixture, metadata)),
                      "ODF-040 table metadata append failed");
  RequireDiagnosticOk(api::AppendMgaIndexMetadata(metadata,
                                                  UniqueIdIndex(fixture, metadata)),
                      "ODF-040 index metadata append failed");
  Commit(metadata);
  return fixture;
}

std::vector<api::EngineRowValue> Rows(std::string prefix, int count) {
  std::vector<api::EngineRowValue> rows;
  rows.reserve(static_cast<std::size_t>(count));
  for (int index = 0; index < count; ++index) {
    rows.push_back(Row(prefix + "-id-" + std::to_string(index + 1),
                       prefix + "-note-" + std::to_string(index + 1)));
  }
  return rows;
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
  request.checkpoint_policy.checkpoint_mode = "disabled";
  request.canonical_rows = std::move(rows);
  request.estimated_row_count =
      static_cast<api::EngineApiU64>(request.canonical_rows.size());
  request.option_envelopes.push_back("copy_append_batching=enabled");
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
  RequireOk(selected, "ODF-040 select failed");
  return selected.visible_count;
}

void AssertDirectCounters(const api::EngineApiResult& result,
                          std::string_view label,
                          std::string_view expected_rows) {
  Require(result.dml_summary.rows_changed != 0, "ODF-040 rows_changed counter missing");
  Require(result.dml_summary.append_calls >= 2, "ODF-040 append counter too small");
  Require(result.dml_summary.file_opens >= 2, "ODF-040 file-open counter too small");
  Require(result.dml_summary.flushes >= 2, "ODF-040 flush counter too small");
  Require(result.dml_summary.page_reservations != 0,
          "ODF-040 page reservation counter missing");
  Require(HasEvidence(result.evidence, "mga_hot_append_row_versions", expected_rows),
          "ODF-040 row hot append evidence missing");
  Require(EvidenceContains(result.evidence, "mga_hot_append_index_entries", ""),
          "ODF-040 index hot append evidence missing");
  Require(EvidenceContains(result.evidence, "constraint_proof_store", "unique_preflight:"),
          "ODF-040 unique preflight proof store missing");
  Require(EvidenceContains(result.evidence, "constraint_proof_hit", "unique_preflight:"),
          "ODF-040 unique preflight proof hit missing");
  Require(EvidenceContains(result.evidence, "constraint_proof_store", "not_null_descriptor:"),
          "ODF-040 not-null proof store missing");
  Require(EvidenceContains(result.evidence, "constraint_proof_hit", "not_null_descriptor:"),
          "ODF-040 not-null proof hit missing");
  Require(!AnyEvidenceContains(result.evidence, "delegated_to_dml.insert_rows"),
          "ODF-040 import delegated to EngineInsertRows");
  if (label == "native") {
    Require(!AnyEvidenceContains(result.evidence, "dml.execute_import_rows"),
            "ODF-040 native bulk delegated to import execution");
  }
}

void AssertNoRuntimeDocLeaks(const api::EngineApiResult& result) {
  const std::vector<std::string_view> forbidden = {
      "docs" "/execution-plans",
      "execution_plan",
      "findings",
      "contracts",
      "references"};
  for (const auto& evidence : result.evidence) {
    for (const auto token : forbidden) {
      Require(evidence.evidence_kind.find(token) == std::string::npos &&
                  evidence.evidence_id.find(token) == std::string::npos,
              "ODF-040 runtime evidence leaked documentation token");
    }
  }
}

void DirectImportAndNativeBulkAreVisible() {
  auto fixture = MakeFixture("direct", 40000);
  auto context = Begin(fixture, "odf040-direct");

  const auto imported =
      api::EngineExecuteImportRows(ImportRequest(fixture, context, Rows("copy", 2)));
  RequireOk(imported, "ODF-040 direct COPY import failed");
  Require(imported.accepted_rows == 2 && imported.inserted_rows == 2,
          "ODF-040 direct COPY row counts drifted");
  Require(HasEvidence(imported.evidence, "import_execution", "direct_physical"),
          "ODF-040 COPY did not select direct physical lane");
  Require(HasEvidence(imported.evidence, "import_execution_delegate", "none"),
          "ODF-040 COPY delegate evidence drifted");
  Require(HasEvidence(imported.evidence, "direct_physical_bulk_operation", "copy_import"),
          "ODF-040 COPY direct operation evidence missing");
  AssertDirectCounters(imported, "copy", "2");
  AssertNoRuntimeDocLeaks(imported);
  Require(SelectCount(fixture, context) == 2,
          "ODF-040 COPY rows not visible in writer transaction");

  const auto native = api::EngineExecuteNativeBulkIngest(
      NativeRequest(fixture, context, Rows("native", 2)));
  RequireOk(native, "ODF-040 direct native bulk failed");
  Require(native.accepted_rows == 2 && native.inserted_rows == 2,
          "ODF-040 native row counts drifted");
  Require(HasEvidence(native.evidence, "native_bulk_ingest_lane", "direct_physical"),
          "ODF-040 native bulk did not select direct physical lane");
  Require(HasEvidence(native.evidence, "native_bulk_ingest_delegate", "none"),
          "ODF-040 native bulk delegated");
  Require(HasEvidence(native.evidence, "direct_physical_bulk_operation", "native_bulk"),
          "ODF-040 native direct operation evidence missing");
  AssertDirectCounters(native, "native", "2");
  AssertNoRuntimeDocLeaks(native);
  Require(SelectCount(fixture, context) == 4,
          "ODF-040 native rows not visible in writer transaction");
  Rollback(context);
}

void DirectLaneDisabledFailsClosed() {
  auto fixture = MakeFixture("disabled", 41000);
  auto context = Begin(fixture, "odf040-disabled");

  auto import_request = ImportRequest(fixture, context, Rows("disabled-copy", 1));
  import_request.option_envelopes.push_back("direct_physical_lane=false");
  const auto imported = api::EngineExecuteImportRows(import_request);
  Require(!imported.ok, "ODF-040 disabled COPY direct lane was accepted");
  Require(!imported.diagnostics.empty(), "ODF-040 disabled COPY lacked diagnostic");
  Require(imported.diagnostics.front().detail.find("direct_physical_lane_disabled") !=
              std::string::npos,
          "ODF-040 disabled COPY diagnostic detail drifted");
  Require(HasEvidence(imported.evidence,
                      "direct_physical_bulk_refused",
                      "direct_physical_lane_disabled"),
          "ODF-040 disabled COPY refusal evidence missing");
  Require(HasEvidence(imported.evidence, "direct_physical_bulk_fail_closed", "true"),
          "ODF-040 disabled COPY did not fail closed");

  auto native_request = NativeRequest(fixture, context, Rows("disabled-native", 1));
  native_request.option_envelopes.push_back("direct_physical_lane:false");
  const auto native = api::EngineExecuteNativeBulkIngest(native_request);
  Require(!native.ok, "ODF-040 disabled native direct lane was accepted");
  Require(!native.diagnostics.empty(), "ODF-040 disabled native lacked diagnostic");
  Require(native.diagnostics.front().detail.find("direct_physical_lane_disabled") !=
              std::string::npos,
          "ODF-040 disabled native diagnostic detail drifted");
  Require(HasEvidence(native.evidence,
                      "native_bulk_ingest_refused_by",
                      "dml.direct_physical_bulk_append"),
          "ODF-040 disabled native refusal boundary evidence missing");
  Require(HasEvidence(native.evidence, "native_bulk_ingest_delegate", "none"),
          "ODF-040 disabled native delegated during refusal");
  Require(SelectCount(fixture, context) == 0,
          "ODF-040 disabled direct lane changed rows");
  Rollback(context);
}

}  // namespace

int main() {
  DirectImportAndNativeBulkAreVisible();
  DirectLaneDisabledFailsClosed();
  return EXIT_SUCCESS;
}
