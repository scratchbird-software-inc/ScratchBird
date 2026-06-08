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
  Require(generated.ok(), "ODF-042 UUID generation failed");
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

std::size_t EvidenceIndex(const std::vector<api::EngineEvidenceReference>& evidence,
                          std::string_view kind,
                          std::string_view token) {
  for (std::size_t index = 0; index < evidence.size(); ++index) {
    if (evidence[index].evidence_kind == kind &&
        evidence[index].evidence_id.find(token) != std::string::npos) {
      return index;
    }
  }
  return evidence.size();
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
  context.catalog_generation_id = 42;
  context.security_epoch = 43;
  context.resource_epoch = 44;
  context.name_resolution_epoch = 45;
  return context;
}

api::EngineRequestContext Begin(const Fixture& fixture, std::string request_id) {
  api::EngineBeginTransactionRequest request;
  request.context = BaseContext(fixture, std::move(request_id));
  request.isolation_level = "read_committed";
  const auto begun = api::EngineBeginTransaction(request);
  RequireOk(begun, "ODF-042 begin transaction failed");
  auto context = request.context;
  context.local_transaction_id = begun.local_transaction_id;
  context.transaction_uuid = begun.transaction_uuid;
  context.snapshot_visible_through_local_transaction_id =
      begun.snapshot_visible_through_local_transaction_id;
  context.transaction_isolation_level = begun.isolation_level;
  return context;
}

void Rollback(const api::EngineRequestContext& context) {
  api::EngineRollbackTransactionRequest request;
  request.context = context;
  RequireOk(api::EngineRollbackTransaction(request), "ODF-042 rollback failed");
}

void Commit(const api::EngineRequestContext& context) {
  api::EngineCommitTransactionRequest request;
  request.context = context;
  RequireOk(api::EngineCommitTransaction(request), "ODF-042 commit failed");
}

api::CrudTableRecord Table(const Fixture& fixture,
                           const api::EngineRequestContext& context) {
  api::CrudTableRecord table;
  table.creator_tx = context.local_transaction_id;
  table.table_uuid = fixture.table_uuid;
  table.default_name = "odf042_extent_preallocation";
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
                ("scratchbird_odf042_" + name + "_" +
                 std::to_string(UniqueSeed()));
  std::filesystem::create_directories(fixture.dir);
  fixture.database_path = fixture.dir / "odf042.sbdb";

  db::DatabaseCreateConfig create;
  create.path = fixture.database_path.string();
  create.database_uuid = NewUuid(platform::UuidKind::database, salt + 1);
  create.filespace_uuid = NewUuid(platform::UuidKind::filespace, salt + 2);
  create.creation_unix_epoch_millis = UniqueSeed();
  create.require_resource_seed_pack = false;
  create.allow_minimal_resource_bootstrap = true;
  create.allow_overwrite = true;
  const auto created = db::CreateDatabaseFile(create);
  Require(created.ok(), "ODF-042 database create failed");

  fixture.database_uuid = uuid::UuidToString(create.database_uuid.value);
  fixture.table_uuid = NewUuidText(platform::UuidKind::object, salt + 10);
  fixture.index_uuid = NewUuidText(platform::UuidKind::object, salt + 11);

  auto metadata = Begin(fixture, "odf042-metadata");
  RequireDiagnosticOk(api::AppendMgaTableMetadata(metadata, Table(fixture, metadata)),
                      "ODF-042 table metadata append failed");
  RequireDiagnosticOk(api::AppendMgaIndexMetadata(metadata,
                                                  UniqueIdIndex(fixture, metadata)),
                      "ODF-042 index metadata append failed");
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

std::vector<std::string> PreallocationOptions(platform::u64 max_pages,
                                              platform::u64 capacity_pages) {
  return {"copy_append_batching=enabled",
          "feature.strict_bulk_load=enabled",
          "page_extent_preallocation=required",
          "page_allocation.runtime=enabled",
          "dml_demand_hints=enabled",
          "dml_demand_hints.max_pages=" + std::to_string(max_pages),
          "dml_demand_hints.available_capacity_pages=" +
              std::to_string(capacity_pages)};
}

api::EngineExecuteImportRowsRequest ImportRequest(
    const Fixture& fixture,
    const api::EngineRequestContext& context,
    std::vector<api::EngineRowValue> rows,
    std::vector<std::string> options) {
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
  request.option_envelopes = std::move(options);
  return request;
}

api::EngineExecuteNativeBulkIngestRequest NativeRequest(
    const Fixture& fixture,
    const api::EngineRequestContext& context,
    std::vector<api::EngineRowValue> rows,
    std::vector<std::string> options) {
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
  request.option_envelopes = std::move(options);
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
  RequireOk(selected, "ODF-042 select failed");
  return selected.visible_count;
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
              "ODF-042 runtime evidence leaked documentation token");
    }
  }
}

void AssertPreallocationAccepted(const api::EngineApiResult& result,
                                 std::string_view expected_rows) {
  Require(HasEvidence(result.evidence, "page_extent_preallocation_requested", "true"),
          "ODF-042 preallocation request evidence missing");
  Require(HasEvidence(result.evidence, "page_extent_preallocation_granted", "true"),
          "ODF-042 preallocation grant evidence missing");
  Require(HasEvidence(result.evidence, "page_extent_preallocation_capped", "false"),
          "ODF-042 success unexpectedly capped");
  Require(HasEvidence(result.evidence, "page_extent_preallocation_refused", "false"),
          "ODF-042 success unexpectedly refused");
  Require(HasEvidence(result.evidence, "row_extent_reservation_count",
                      expected_rows),
          "ODF-042 row extent reservation count missing");
  Require(HasEvidence(result.evidence, "version_extent_reservation_count",
                      expected_rows),
          "ODF-042 version extent reservation count missing");
  Require(HasEvidence(result.evidence, "page_agent_demand_decision",
                      "preallocation_completed"),
          "ODF-042 page agent did not preallocate");
  Require(HasEvidence(result.evidence, "filespace_agent_demand_decision",
                      "capacity_window_approved"),
          "ODF-042 filespace agent did not approve capacity before preallocation");
  Require(HasEvidence(result.evidence, "row_page_allocation_source",
                      "SB-STORAGE-PAGE-ALLOCATION-PREALLOCATED-POOL-HIT"),
          "ODF-042 row allocation did not consume preallocated pages");
  Require(HasEvidence(result.evidence, "index_page_allocation_source",
                      "SB-STORAGE-PAGE-ALLOCATION-PREALLOCATED-POOL-HIT"),
          "ODF-042 index allocation did not consume preallocated pages");
  Require(!HasEvidence(result.evidence, "row_page_allocation_source",
                       "SB-STORAGE-PAGE-ALLOCATION-FREE-EXTENT-FALLBACK"),
          "ODF-042 row allocation fell back to foreground free extent");
  Require(result.dml_summary.row_extent_reservations != 0,
          "ODF-042 row extent summary missing");
  Require(result.dml_summary.version_extent_reservations != 0,
          "ODF-042 version extent summary missing");
  Require(result.dml_summary.page_extent_reservations != 0,
          "ODF-042 page extent summary missing");
  Require(result.dml_summary.index_extent_reservations != 0,
          "ODF-042 index extent summary missing");
  Require(result.dml_summary.preallocation_requests >= 2,
          "ODF-042 preallocation request summary missing");
  Require(result.dml_summary.preallocation_granted_pages != 0,
          "ODF-042 preallocation grant summary missing");
  Require(EvidenceIndex(result.evidence, "page_agent_demand_decision",
                        "preallocation_completed") <
              EvidenceIndex(result.evidence, "mga_hot_append_row_versions",
                            expected_rows),
          "ODF-042 preallocation did not precede row append evidence");
  Require(EvidenceIndex(result.evidence, "row_extent_reservation_id", "") <
              EvidenceIndex(result.evidence, "mga_hot_append_row_versions",
                            expected_rows),
          "ODF-042 extent reservation did not precede row append evidence");
  Require(EvidenceIndex(result.evidence, "page_agent_demand_decision",
                        "preallocation_completed") <
              EvidenceIndex(result.evidence, "strict_bulk_load_published_visible",
                            "true"),
          "ODF-042 preallocation did not precede published-visible evidence");
  Require(!AnyEvidenceContains(result.evidence, "delegated_to_dml.insert_rows"),
          "ODF-042 direct path delegated to EngineInsertRows");
  AssertNoRuntimeDocLeaks(result);
}

void CopyAndNativeReserveBeforeAppend() {
  auto fixture = MakeFixture("success", 42000);
  auto context = Begin(fixture, "odf042-success");

  const auto imported = api::EngineExecuteImportRows(
      ImportRequest(fixture, context, Rows("copy", 6),
                    PreallocationOptions(16, 16)));
  RequireOk(imported, "ODF-042 COPY preallocation failed");
  Require(imported.accepted_rows == 6 && imported.inserted_rows == 6,
          "ODF-042 COPY row count drifted");
  Require(HasEvidence(imported.evidence, "import_execution", "direct_physical"),
          "ODF-042 COPY did not use direct physical lane");
  AssertPreallocationAccepted(imported, "6");
  Require(SelectCount(fixture, context) == 6,
          "ODF-042 COPY rows not visible after strict publication");

  const auto native = api::EngineExecuteNativeBulkIngest(
      NativeRequest(fixture, context, Rows("native", 6),
                    PreallocationOptions(16, 16)));
  RequireOk(native, "ODF-042 native preallocation failed");
  Require(native.accepted_rows == 6 && native.inserted_rows == 6,
          "ODF-042 native row count drifted");
  Require(HasEvidence(native.evidence, "native_bulk_ingest_lane", "direct_physical"),
          "ODF-042 native did not use direct physical lane");
  AssertPreallocationAccepted(native, "6");
  Require(SelectCount(fixture, context) == 12,
          "ODF-042 native rows not visible after strict publication");
  Rollback(context);
}

void DisabledRequiredPreallocationFailsClosed() {
  auto fixture = MakeFixture("disabled", 43000);
  auto context = Begin(fixture, "odf042-disabled");
  const auto refused = api::EngineExecuteImportRows(
      ImportRequest(fixture, context, Rows("disabled", 2),
                    {"copy_append_batching=enabled",
                     "feature.strict_bulk_load=enabled",
                     "page_extent_preallocation=required"}));
  Require(!refused.ok, "ODF-042 disabled preallocation was accepted");
  Require(HasEvidence(refused.evidence, "direct_physical_bulk_refused",
                      "page_extent_preallocation_disabled"),
          "ODF-042 disabled refusal evidence missing");
  Require(HasEvidence(refused.evidence, "direct_physical_bulk_fail_closed", "true"),
          "ODF-042 disabled refusal did not fail closed");
  Require(refused.dml_summary.preallocation_refused != 0,
          "ODF-042 disabled refusal summary missing");
  Require(SelectCount(fixture, context) == 0,
          "ODF-042 disabled preallocation published rows");
  AssertNoRuntimeDocLeaks(refused);
  Rollback(context);
}

void InvalidFilespaceFailsClosed() {
  auto fixture = MakeFixture("invalid-filespace", 44000);
  auto context = Begin(fixture, "odf042-invalid-filespace");
  auto options = PreallocationOptions(8, 8);
  options.push_back("page_allocation.filespace_uuid=not-a-filespace-uuid");
  const auto refused = api::EngineExecuteImportRows(
      ImportRequest(fixture, context, Rows("invalid-filespace", 2),
                    std::move(options)));
  Require(!refused.ok, "ODF-042 invalid filespace was accepted");
  Require(HasEvidence(refused.evidence, "direct_physical_bulk_refused",
                      "page_extent_preallocation_invalid_filespace"),
          "ODF-042 invalid filespace evidence missing");
  Require(SelectCount(fixture, context) == 0,
          "ODF-042 invalid filespace published rows");
  AssertNoRuntimeDocLeaks(refused);
  Rollback(context);
}

void AuthorityMissingFailsClosed() {
  auto fixture = MakeFixture("authority", 45000);
  auto context = Begin(fixture, "odf042-authority");
  auto bad_context = context;
  bad_context.transaction_uuid.canonical = "not-a-transaction-uuid";
  const auto refused = api::EngineExecuteImportRows(
      ImportRequest(fixture, bad_context, Rows("authority", 2),
                    PreallocationOptions(8, 8)));
  Require(!refused.ok, "ODF-042 missing authority was accepted");
  Require(HasEvidence(refused.evidence, "direct_physical_bulk_refused",
                      "page_extent_preallocation_authority_missing"),
          "ODF-042 missing authority evidence missing");
  Require(SelectCount(fixture, context) == 0,
          "ODF-042 missing authority published rows");
  AssertNoRuntimeDocLeaks(refused);
  Rollback(context);
}

void CapExceededFailsClosedWithEvidence() {
  auto fixture = MakeFixture("cap", 46000);
  auto context = Begin(fixture, "odf042-cap");
  const auto refused = api::EngineExecuteImportRows(
      ImportRequest(fixture, context, Rows("cap", 6),
                    PreallocationOptions(2, 16)));
  Require(!refused.ok, "ODF-042 capped preallocation was accepted");
  Require(HasEvidence(refused.evidence, "dml_demand_hint_decision", "capped"),
          "ODF-042 cap decision evidence missing");
  Require(HasEvidence(refused.evidence, "direct_physical_bulk_refused",
                      "row_page_extent_preallocation_cap_exceeded"),
          "ODF-042 cap refusal reason missing");
  Require(HasEvidence(refused.evidence, "page_extent_preallocation_refused", "true") ||
              refused.dml_summary.preallocation_refused != 0,
          "ODF-042 cap refusal summary missing");
  Require(!AnyEvidenceContains(refused.evidence, "mga_row_version"),
          "ODF-042 capped preallocation wrote row evidence");
  Require(SelectCount(fixture, context) == 0,
          "ODF-042 capped preallocation published rows");
  AssertNoRuntimeDocLeaks(refused);
  Rollback(context);
}

}  // namespace

int main() {
  CopyAndNativeReserveBeforeAppend();
  DisabledRequiredPreallocationFailsClosed();
  InvalidFilespaceFailsClosed();
  AuthorityMissingFailsClosed();
  CapExceededFailsClosedWithEvidence();
  return EXIT_SUCCESS;
}
