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
  Require(generated.ok(), "ODF-041 UUID generation failed");
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
  context.catalog_generation_id = 41;
  context.security_epoch = 42;
  context.resource_epoch = 43;
  context.name_resolution_epoch = 44;
  return context;
}

api::EngineRequestContext Begin(const Fixture& fixture, std::string request_id) {
  api::EngineBeginTransactionRequest request;
  request.context = BaseContext(fixture, std::move(request_id));
  request.isolation_level = "read_committed";
  const auto begun = api::EngineBeginTransaction(request);
  RequireOk(begun, "ODF-041 begin transaction failed");
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
  RequireOk(api::EngineCommitTransaction(request), "ODF-041 commit failed");
}

void Rollback(const api::EngineRequestContext& context) {
  api::EngineRollbackTransactionRequest request;
  request.context = context;
  RequireOk(api::EngineRollbackTransaction(request), "ODF-041 rollback failed");
}

api::CrudTableRecord Table(const Fixture& fixture,
                           const api::EngineRequestContext& context) {
  api::CrudTableRecord table;
  table.creator_tx = context.local_transaction_id;
  table.table_uuid = fixture.table_uuid;
  table.default_name = "odf041_strict_bulk";
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
                ("scratchbird_odf041_" + name + "_" +
                 std::to_string(UniqueSeed()));
  std::filesystem::create_directories(fixture.dir);
  fixture.database_path = fixture.dir / "odf041.sbdb";

  db::DatabaseCreateConfig create;
  create.path = fixture.database_path.string();
  create.database_uuid = NewUuid(platform::UuidKind::database, salt + 1);
  create.filespace_uuid = NewUuid(platform::UuidKind::filespace, salt + 2);
  create.creation_unix_epoch_millis = UniqueSeed();
  create.require_resource_seed_pack = false;
  create.allow_minimal_resource_bootstrap = true;
  create.allow_overwrite = true;
  const auto created = db::CreateDatabaseFile(create);
  Require(created.ok(), "ODF-041 database create failed");

  fixture.database_uuid = uuid::UuidToString(create.database_uuid.value);
  fixture.table_uuid = NewUuidText(platform::UuidKind::object, salt + 10);
  fixture.index_uuid = NewUuidText(platform::UuidKind::object, salt + 11);

  auto metadata = Begin(fixture, "odf041-metadata");
  RequireDiagnosticOk(api::AppendMgaTableMetadata(metadata, Table(fixture, metadata)),
                      "ODF-041 table metadata append failed");
  RequireDiagnosticOk(api::AppendMgaIndexMetadata(metadata,
                                                  UniqueIdIndex(fixture, metadata)),
                      "ODF-041 index metadata append failed");
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
    std::vector<api::EngineRowValue> rows,
    bool strict_enabled = true) {
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
  if (strict_enabled) {
    request.option_envelopes.push_back("feature.strict_bulk_load=enabled");
  }
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
  RequireOk(selected, "ODF-041 select failed");
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
              "ODF-041 runtime evidence leaked documentation token");
    }
  }
}

void AssertDirectDmlCounters(const api::EngineApiResult& result,
                             std::string_view expected_rows) {
  Require(result.dml_summary.rows_changed != 0,
          "ODF-041 rows_changed counter missing");
  Require(result.dml_summary.append_calls >= 2,
          "ODF-041 append counter too small");
  Require(result.dml_summary.file_opens >= 2,
          "ODF-041 file-open counter too small");
  Require(result.dml_summary.flushes >= 2,
          "ODF-041 flush counter too small");
  Require(result.dml_summary.page_reservations != 0,
          "ODF-041 page reservation counter missing");
  Require(HasEvidence(result.evidence, "mga_hot_append_row_versions", expected_rows),
          "ODF-041 hot append row evidence missing");
}

void AssertStrictAcceptedEvidence(const api::EngineApiResult& result,
                                  std::string_view expected_rows,
                                  bool native_path) {
  Require(HasEvidence(result.evidence, "strict_bulk_load_state", "begun"),
          "ODF-041 strict begun evidence missing");
  Require(HasEvidence(result.evidence, "strict_bulk_load_state", "appending"),
          "ODF-041 strict appending evidence missing");
  Require(HasEvidence(result.evidence,
                      "strict_bulk_load_state",
                      "finalize_evidence_durable"),
          "ODF-041 strict finalize-durable evidence missing");
  Require(HasEvidence(result.evidence, "strict_bulk_load_state", "published_visible"),
          "ODF-041 strict published-visible evidence missing");
  Require(HasEvidence(result.evidence,
                      "strict_bulk_load_finalize_evidence_durable",
                      "true"),
          "ODF-041 strict durable finalize fence missing");
  Require(HasEvidence(result.evidence, "strict_bulk_load_published_visible", "true"),
          "ODF-041 strict visible publication evidence missing");
  Require(HasEvidence(result.evidence,
                      "strict_bulk_load_direct_publication_fence",
                      "finalize_evidence_durable_before_mga_visibility"),
          "ODF-041 strict direct publication fence missing");
  Require(HasEvidence(result.evidence,
                      "strict_bulk_load_physical_publication_succeeded",
                      "row_index_append_flush"),
          "ODF-041 physical publication success fence missing");
  Require(HasEvidence(result.evidence,
                      "strict_bulk_load_direct_lane_published_after",
                      "strict_bulk_load_physical_publication_succeeded"),
          "ODF-041 strict publish-after-physical evidence missing");
  Require(EvidenceIndex(result.evidence,
                        "strict_bulk_load_state",
                        "finalize_evidence_durable") <
              EvidenceIndex(result.evidence, "mga_row_version", "row_insert"),
          "ODF-041 strict finalize evidence did not precede row visibility evidence");
  Require(EvidenceIndex(result.evidence,
                        "strict_bulk_load_physical_publication_succeeded",
                        "row_index_append_flush") <
              EvidenceIndex(result.evidence,
                            "strict_bulk_load_published_visible",
                            "true"),
          "ODF-041 strict published-visible evidence preceded physical success");
  Require(!AnyEvidenceContains(result.evidence, "delegated_to_dml.insert_rows"),
          "ODF-041 strict direct path delegated to EngineInsertRows");
  if (native_path) {
    Require(!AnyEvidenceContains(result.evidence, "dml.execute_import_rows"),
            "ODF-041 strict native path delegated to import execution");
  }
  AssertDirectDmlCounters(result, expected_rows);
  AssertNoRuntimeDocLeaks(result);
}

void StrictImportAndNativePublishVisible() {
  auto fixture = MakeFixture("publish", 41000);
  auto context = Begin(fixture, "odf041-publish");

  const auto imported =
      api::EngineExecuteImportRows(ImportRequest(fixture,
                                                 context,
                                                 Rows("copy-strict", 2)));
  RequireOk(imported, "ODF-041 strict COPY import failed");
  Require(imported.accepted_rows == 2 && imported.inserted_rows == 2,
          "ODF-041 strict COPY row counts drifted");
  Require(HasEvidence(imported.evidence, "import_execution", "direct_physical"),
          "ODF-041 strict COPY did not select direct physical lane");
  Require(HasEvidence(imported.evidence, "import_execution_delegate", "none"),
          "ODF-041 strict COPY delegated");
  AssertStrictAcceptedEvidence(imported, "2", false);
  Require(SelectCount(fixture, context) == 2,
          "ODF-041 strict COPY rows not visible after publication");

  const auto native = api::EngineExecuteNativeBulkIngest(
      NativeRequest(fixture, context, Rows("native-strict", 2)));
  RequireOk(native, "ODF-041 strict native bulk failed");
  Require(native.accepted_rows == 2 && native.inserted_rows == 2,
          "ODF-041 strict native row counts drifted");
  Require(HasEvidence(native.evidence, "native_bulk_ingest_lane", "direct_physical"),
          "ODF-041 strict native did not select direct physical lane");
  Require(HasEvidence(native.evidence, "native_bulk_ingest_delegate", "none"),
          "ODF-041 strict native delegated");
  AssertStrictAcceptedEvidence(native, "2", true);
  Require(SelectCount(fixture, context) == 4,
          "ODF-041 strict native rows not visible after publication");
  Rollback(context);
}

void StrictNotEnabledFailsClosed() {
  auto fixture = MakeFixture("not-enabled", 42000);
  auto context = Begin(fixture, "odf041-not-enabled");
  const auto imported = api::EngineExecuteImportRows(
      ImportRequest(fixture, context, Rows("not-enabled", 1), false));
  Require(!imported.ok, "ODF-041 strict not-enabled request was accepted");
  Require(!imported.diagnostics.empty(), "ODF-041 not-enabled lacked diagnostic");
  Require(imported.diagnostics.front().detail.find(
              "strict_bulk_load_policy_not_enabled") != std::string::npos,
          "ODF-041 not-enabled diagnostic drifted");
  Require(HasEvidence(imported.evidence,
                      "direct_physical_bulk_refused",
                      "strict_bulk_load_policy_not_enabled"),
          "ODF-041 not-enabled direct refusal evidence missing");
  Require(HasEvidence(imported.evidence, "direct_physical_bulk_fail_closed", "true"),
          "ODF-041 not-enabled did not fail closed");
  Require(HasEvidence(imported.evidence,
                      "insert_feature_gate.strict_bulk_load",
                      "disabled"),
          "ODF-041 not-enabled feature evidence missing");
  Require(SelectCount(fixture, context) == 0,
          "ODF-041 not-enabled failure changed visibility");
  AssertNoRuntimeDocLeaks(imported);
  Rollback(context);
}

void StrictFinalizeRecoveryHasNoVisibleRows() {
  auto fixture = MakeFixture("recovery", 43000);
  auto context = Begin(fixture, "odf041-recovery");
  auto request = ImportRequest(fixture, context, Rows("recovery", 1));
  request.option_envelopes.push_back(
      "strict_bulk_load.simulate_finalize_failure_after_evidence=true");
  const auto imported = api::EngineExecuteImportRows(request);
  Require(!imported.ok, "ODF-041 simulated finalize failure was accepted");
  Require(!imported.diagnostics.empty(), "ODF-041 recovery lacked diagnostic");
  Require(imported.diagnostics.front().code == "strict_bulk_load_publish_recovery",
          "ODF-041 recovery diagnostic code drifted");
  Require(HasEvidence(imported.evidence,
                      "strict_bulk_load_state",
                      "finalize_evidence_durable"),
          "ODF-041 recovery lacks finalize-durable evidence");
  Require(HasEvidence(imported.evidence,
                      "strict_bulk_load_state",
                      "recovery_required"),
          "ODF-041 recovery-required state missing");
  Require(HasEvidence(imported.evidence,
                      "strict_bulk_load_recovery_action",
                      "complete_publication"),
          "ODF-041 recovery action mismatch");
  Require(HasEvidence(imported.evidence,
                      "strict_bulk_load_recovery_fail_closed",
                      "false"),
          "ODF-041 recoverable finalize failure marked fail-closed");
  Require(!AnyEvidenceContains(imported.evidence, "mga_row_version"),
          "ODF-041 recovery failure wrote row visibility evidence");
  Require(SelectCount(fixture, context) == 0,
          "ODF-041 recovery failure published visible rows");
  AssertNoRuntimeDocLeaks(imported);
  Rollback(context);
}

void StrictPhysicalPublicationFailureDoesNotPublishVisible() {
  auto fixture = MakeFixture("physical-failure", 43500);
  auto context = Begin(fixture, "odf041-physical-failure");
  auto request = ImportRequest(fixture, context, Rows("physical-failure", 1));
  request.option_envelopes.push_back(
      "strict_bulk_load.simulate_physical_publication_failure_after_evidence=true");
  const auto imported = api::EngineExecuteImportRows(request);
  Require(!imported.ok, "ODF-041 simulated physical publication failure was accepted");
  Require(!imported.diagnostics.empty(),
          "ODF-041 physical publication failure lacked diagnostic");
  Require(imported.diagnostics.front().detail.find(
              "strict_bulk_load_physical_publication_row_append_failed") !=
              std::string::npos,
          "ODF-041 physical publication failure diagnostic drifted");
  Require(HasEvidence(imported.evidence,
                      "strict_bulk_load_state",
                      "finalize_evidence_durable"),
          "ODF-041 physical failure lacks finalize-durable evidence");
  Require(HasEvidence(imported.evidence,
                      "strict_bulk_load_physical_publication_failed",
                      "row_append"),
          "ODF-041 physical failure stage evidence missing");
  Require(HasEvidence(imported.evidence, "strict_bulk_load_state", "quarantine"),
          "ODF-041 physical failure quarantine state missing");
  Require(HasEvidence(imported.evidence,
                      "strict_bulk_load_recovery_action",
                      "fail_closed"),
          "ODF-041 physical failure recovery classification mismatch");
  Require(HasEvidence(imported.evidence,
                      "strict_bulk_load_recovery_fail_closed",
                      "true"),
          "ODF-041 physical failure was not fail-closed");
  Require(!HasEvidence(imported.evidence,
                       "strict_bulk_load_published_visible",
                       "true"),
          "ODF-041 physical failure emitted published-visible marker");
  Require(!HasEvidence(imported.evidence,
                       "strict_bulk_load_state",
                       "published_visible"),
          "ODF-041 physical failure reached published-visible state");
  Require(!AnyEvidenceContains(imported.evidence, "mga_row_version"),
          "ODF-041 simulated physical failure wrote row visibility evidence");
  Require(SelectCount(fixture, context) == 0,
          "ODF-041 physical publication failure published visible rows");
  AssertNoRuntimeDocLeaks(imported);
  Rollback(context);
}

void StrictRollbackQuarantineAndRefusedStates() {
  auto rollback_fixture = MakeFixture("rollback", 44000);
  auto rollback_context = Begin(rollback_fixture, "odf041-rollback");
  auto rollback_request =
      ImportRequest(rollback_fixture, rollback_context, Rows("rollback", 1));
  rollback_request.option_envelopes.push_back(
      "strict_bulk_load.simulate_rollback_before_publication=true");
  const auto rolled_back = api::EngineExecuteImportRows(rollback_request);
  Require(!rolled_back.ok, "ODF-041 simulated rollback was accepted");
  Require(HasEvidence(rolled_back.evidence, "strict_bulk_load_state", "rolled_back"),
          "ODF-041 rollback state evidence missing");
  Require(HasEvidence(rolled_back.evidence, "strict_bulk_load_rollback", "true"),
          "ODF-041 rollback action evidence missing");
  Require(HasEvidence(rolled_back.evidence,
                      "strict_bulk_load_recovery_action",
                      "no_action"),
          "ODF-041 rolled-back recovery classification mismatch");
  Require(SelectCount(rollback_fixture, rollback_context) == 0,
          "ODF-041 rollback changed visibility");
  AssertNoRuntimeDocLeaks(rolled_back);
  Rollback(rollback_context);

  auto quarantine_fixture = MakeFixture("quarantine", 45000);
  auto quarantine_context = Begin(quarantine_fixture, "odf041-quarantine");
  auto quarantine_request =
      ImportRequest(quarantine_fixture, quarantine_context, Rows("quarantine", 1));
  quarantine_request.option_envelopes.push_back(
      "strict_bulk_load.simulate_quarantine_before_publication=true");
  const auto quarantined = api::EngineExecuteImportRows(quarantine_request);
  Require(!quarantined.ok, "ODF-041 simulated quarantine was accepted");
  Require(HasEvidence(quarantined.evidence, "strict_bulk_load_state", "quarantine"),
          "ODF-041 quarantine state evidence missing");
  Require(HasEvidence(quarantined.evidence, "strict_bulk_load_quarantine", "true"),
          "ODF-041 quarantine action evidence missing");
  Require(HasEvidence(quarantined.evidence,
                      "strict_bulk_load_recovery_action",
                      "fail_closed"),
          "ODF-041 quarantine recovery classification mismatch");
  Require(HasEvidence(quarantined.evidence,
                      "strict_bulk_load_recovery_fail_closed",
                      "true"),
          "ODF-041 quarantine did not fail closed");
  Require(SelectCount(quarantine_fixture, quarantine_context) == 0,
          "ODF-041 quarantine changed visibility");
  AssertNoRuntimeDocLeaks(quarantined);
  Rollback(quarantine_context);

  auto refused_fixture = MakeFixture("refused", 46000);
  auto refused_context = Begin(refused_fixture, "odf041-refused");
  auto refused_request =
      ImportRequest(refused_fixture, refused_context, Rows("refused", 1));
  refused_request.option_envelopes.push_back(
      "strict_bulk_load.simulate_begin_refused=true");
  const auto refused = api::EngineExecuteImportRows(refused_request);
  Require(!refused.ok, "ODF-041 simulated strict begin refusal was accepted");
  Require(HasEvidence(refused.evidence, "strict_bulk_load_state", "refused"),
          "ODF-041 refused state evidence missing");
  Require(HasEvidence(refused.evidence,
                      "strict_bulk_load_refused_state",
                      "true"),
          "ODF-041 refused-state marker missing");
  Require(HasEvidence(refused.evidence,
                      "strict_bulk_load_diagnostic_code",
                      "strict_bulk_load_policy_not_enabled"),
          "ODF-041 strict begin refusal diagnostic evidence mismatch");
  Require(SelectCount(refused_fixture, refused_context) == 0,
          "ODF-041 refused strict lifecycle changed visibility");
  AssertNoRuntimeDocLeaks(refused);
  Rollback(refused_context);
}

}  // namespace

int main() {
  StrictImportAndNativePublishVisible();
  StrictNotEnabledFailsClosed();
  StrictFinalizeRecoveryHasNoVisibleRows();
  StrictPhysicalPublicationFailureDoesNotPublishVisible();
  StrictRollbackQuarantineAndRefusedStates();
  return EXIT_SUCCESS;
}
