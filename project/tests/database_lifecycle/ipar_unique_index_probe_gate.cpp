// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "database_lifecycle.hpp"
#include "dml/insert_api.hpp"
#include "mga_relation_store/mga_relation_store.hpp"
#include "transaction/savepoint_api.hpp"
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

platform::u64 MillisSeed() {
  return static_cast<platform::u64>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

platform::TypedUuid NewUuid(platform::UuidKind kind, platform::u64 salt) {
  const auto generated = uuid::GenerateEngineIdentityV7(kind, MillisSeed() + salt);
  Require(generated.ok(), "IPAR unique-probe UUID generation failed");
  return generated.value;
}

std::string NewUuidText(platform::UuidKind kind, platform::u64 salt) {
  return uuid::UuidToString(NewUuid(kind, salt).value);
}

bool HasEvidence(const std::vector<api::EngineEvidenceReference>& evidence,
                 std::string_view kind,
                 std::string_view id) {
  for (const auto& item : evidence) {
    if (item.evidence_kind == kind && item.evidence_id == id) {
      return true;
    }
  }
  return false;
}

bool HasEvidenceKind(const std::vector<api::EngineEvidenceReference>& evidence,
                     std::string_view kind) {
  for (const auto& item : evidence) {
    if (item.evidence_kind == kind) {
      return true;
    }
  }
  return false;
}

bool HasDiagnostic(const api::EngineApiResult& result, std::string_view needle) {
  const std::string text(needle);
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.code.find(text) != std::string::npos ||
        diagnostic.message_key.find(text) != std::string::npos ||
        diagnostic.detail.find(text) != std::string::npos) {
      return true;
    }
  }
  return false;
}

void DumpDiagnostics(const api::EngineApiResult& result) {
  for (const auto& diagnostic : result.diagnostics) {
    std::cerr << diagnostic.code << ':' << diagnostic.message_key << ':'
              << diagnostic.detail << '\n';
  }
}

struct Fixture {
  std::filesystem::path dir;
  std::filesystem::path database_path;
  std::string database_uuid;
  std::string schema_uuid;
  std::string table_uuid;
  std::string index_uuid;
  platform::u64 salt = 0;

  ~Fixture() {
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
  typed.state = api::EngineValueState::value;
  return typed;
}

api::EngineRowValue Row(std::string payload) {
  api::EngineRowValue row;
  row.fields.push_back({"payload", TextValue(std::move(payload))});
  return row;
}

api::CrudTableRecord Table(const Fixture& fixture, std::uint64_t creator_tx) {
  api::CrudTableRecord table;
  table.creator_tx = creator_tx;
  table.table_uuid = fixture.table_uuid;
  table.default_name = "ipar_unique_probe_target";
  table.columns.push_back({"payload", "canonical=character"});
  return table;
}

api::CrudIndexRecord UniquePayloadIndex(const Fixture& fixture,
                                        std::uint64_t creator_tx) {
  api::CrudIndexRecord index;
  index.creator_tx = creator_tx;
  index.index_uuid = fixture.index_uuid;
  index.table_uuid = fixture.table_uuid;
  index.column_name = "payload";
  index.family = api::kCrudIndexFamilyBtree;
  index.profile = api::kCrudIndexProfileRowStoreScalarBtreeV1;
  index.default_name = "ipar_unique_probe_payload_uidx";
  index.unique = true;
  index.key_envelopes.push_back("payload");
  index.key_envelopes.push_back("unique");
  return index;
}

api::EngineRequestContext BaseContext(const Fixture& fixture, std::string request_id) {
  api::EngineRequestContext context;
  context.trust_mode = api::EngineTrustMode::server_isolated;
  context.request_id = std::move(request_id);
  context.database_path = fixture.database_path.string();
  context.database_uuid.canonical = fixture.database_uuid;
  context.principal_uuid.canonical =
      NewUuidText(platform::UuidKind::principal, fixture.salt + 100);
  context.session_uuid.canonical =
      NewUuidText(platform::UuidKind::object, fixture.salt + 101);
  context.current_schema_uuid.canonical = fixture.schema_uuid;
  context.security_context_present = true;
  context.catalog_generation_id = 1;
  context.security_epoch = 1;
  context.resource_epoch = 1;
  context.name_resolution_epoch = 1;
  return context;
}

api::EngineRequestContext Begin(const Fixture& fixture, std::string request_id) {
  api::EngineBeginTransactionRequest request;
  request.context = BaseContext(fixture, std::move(request_id));
  request.isolation_level = "read_committed";
  const auto begun = api::EngineBeginTransaction(request);
  if (!begun.ok) { DumpDiagnostics(begun); }
  Require(begun.ok, "IPAR unique-probe begin transaction failed");
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
  const auto committed = api::EngineCommitTransaction(request);
  if (!committed.ok) { DumpDiagnostics(committed); }
  Require(committed.ok, "IPAR unique-probe commit failed");
}

void Rollback(const api::EngineRequestContext& context) {
  api::EngineRollbackTransactionRequest request;
  request.context = context;
  const auto rolled_back = api::EngineRollbackTransaction(request);
  if (!rolled_back.ok) { DumpDiagnostics(rolled_back); }
  Require(rolled_back.ok, "IPAR unique-probe rollback failed");
}

void Reopen(const Fixture& fixture) {
  const auto opened =
      db::OpenDatabaseFile({fixture.database_path.string(), false, false, false});
  if (!opened.ok()) {
    std::cerr << opened.diagnostic.diagnostic_code << ':'
              << opened.diagnostic.message_key << '\n';
  }
  Require(opened.ok(), "IPAR unique-probe database reopen failed");
}

void CreateSavepoint(const api::EngineRequestContext& context,
                     std::string savepoint_name) {
  api::EngineCreateSavepointRequest request;
  request.context = context;
  request.option_envelopes.push_back("savepoint_name:" + std::move(savepoint_name));
  const auto created = api::EngineCreateSavepoint(request);
  if (!created.ok) { DumpDiagnostics(created); }
  Require(created.ok, "IPAR unique-probe savepoint create failed");
}

void RollbackToSavepoint(const api::EngineRequestContext& context,
                         std::string savepoint_name) {
  api::EngineRollbackToSavepointRequest request;
  request.context = context;
  request.option_envelopes.push_back("savepoint_name:" + std::move(savepoint_name));
  const auto rolled_back = api::EngineRollbackToSavepoint(request);
  if (!rolled_back.ok) { DumpDiagnostics(rolled_back); }
  Require(rolled_back.ok, "IPAR unique-probe savepoint rollback failed");
}

Fixture MakeFixture() {
  Fixture fixture;
  fixture.salt = MillisSeed();
  const auto steady_suffix = static_cast<long long>(
      std::chrono::steady_clock::now().time_since_epoch().count());
  fixture.dir = std::filesystem::temp_directory_path() /
                ("scratchbird_ipar_unique_probe_" +
                 std::to_string(fixture.salt) + "_" +
                 std::to_string(steady_suffix));
  std::filesystem::create_directories(fixture.dir);
  fixture.database_path = fixture.dir / "ipar_unique_probe.sbdb";

  db::DatabaseCreateConfig create;
  create.path = fixture.database_path.string();
  create.database_uuid = NewUuid(platform::UuidKind::database, fixture.salt + 1);
  create.filespace_uuid = NewUuid(platform::UuidKind::filespace, fixture.salt + 2);
  create.creation_unix_epoch_millis = MillisSeed() + 3;
  create.allow_minimal_resource_bootstrap = true;
  create.require_resource_seed_pack = false;
  create.allow_overwrite = true;
  const auto created = db::CreateDatabaseFile(create);
  if (!created.ok()) {
    std::cerr << created.diagnostic.diagnostic_code << ':'
              << created.diagnostic.message_key << '\n';
  }
  Require(created.ok(), "IPAR unique-probe database create failed");

  fixture.database_uuid = uuid::UuidToString(create.database_uuid.value);
  fixture.schema_uuid = NewUuidText(platform::UuidKind::schema, fixture.salt + 10);
  fixture.table_uuid = NewUuidText(platform::UuidKind::object, fixture.salt + 20);
  fixture.index_uuid = NewUuidText(platform::UuidKind::object, fixture.salt + 21);

  auto metadata = Begin(fixture, "ipar-unique-probe-metadata");
  const auto table = api::AppendMgaTableMetadata(
      metadata,
      Table(fixture, metadata.local_transaction_id));
  Require(!table.error, "IPAR unique-probe table metadata append failed");
  const auto index = api::AppendMgaIndexMetadata(
      metadata,
      UniquePayloadIndex(fixture, metadata.local_transaction_id));
  Require(!index.error, "IPAR unique-probe index metadata append failed");
  Commit(metadata);
  return fixture;
}

api::EngineInsertRowsResult InsertBatchInto(
    const Fixture& fixture,
    const api::EngineRequestContext& context,
    std::vector<std::string> payloads,
    std::vector<std::string> options = {}) {
  api::EngineInsertRowsRequest request;
  request.context = context;
  request.target_schema.uuid.canonical = fixture.schema_uuid;
  request.target_table.uuid.canonical = fixture.table_uuid;
  request.target_table.object_kind = "table";
  request.target_object.uuid.canonical = fixture.table_uuid;
  request.target_object.object_kind = "table";
  request.estimated_row_count = payloads.size();
  request.input_rows.reserve(payloads.size());
  for (auto& payload : payloads) {
    request.input_rows.push_back(Row(std::move(payload)));
  }
  request.option_envelopes = std::move(options);
  return api::EngineInsertRows(request);
}

api::EngineInsertRowsResult InsertInto(const Fixture& fixture,
                                       const api::EngineRequestContext& context,
                                       std::string payload,
                                       std::vector<std::string> options = {}) {
  return InsertBatchInto(fixture,
                         context,
                         std::vector<std::string>{std::move(payload)},
                         std::move(options));
}

void RequireInsertOk(const api::EngineInsertRowsResult& result,
                     std::string_view message) {
  if (!result.ok) { DumpDiagnostics(result); }
  Require(result.ok, message);
}

void RequireUniqueViolation(const api::EngineInsertRowsResult& result,
                            std::string_view message) {
  Require(!result.ok, message);
  if (!HasDiagnostic(result, "CONSTRAINT_UNIQUE_VIOLATION")) {
    DumpDiagnostics(result);
    Fail("IPAR unique-probe duplicate diagnostic mismatch");
  }
}

void RequireSynchronousUniqueInsertEvidence(
    const api::EngineInsertRowsResult& result) {
  Require(HasEvidence(result.evidence,
                      "insert_unique_preflight_path",
                      "index_backed"),
          "IPAR unique-probe insert did not use index-backed preflight");
  Require(HasEvidence(result.evidence,
                      "insert_unique_preflight_route",
                      "accepted"),
          "IPAR unique-probe unique preflight route was not accepted");
  Require(HasEvidence(result.evidence,
                      "insert_unique_authority",
                      "engine_mga"),
          "IPAR unique-probe unique authority was not engine MGA");
  Require(HasEvidence(result.evidence,
                      "unique_index_physical_probe_cache_indexes",
                      "1"),
          "IPAR unique-probe physical cache did not cover one unique index");
  Require(HasEvidence(result.evidence,
                      "unique_index_physical_probe_authority",
                      "candidate_only_mga_visibility_recheck_required"),
          "IPAR unique-probe physical cache authority evidence missing");
  Require(HasEvidence(result.evidence,
                      "insert_hot_append_index_entries",
                      "1"),
          "IPAR unique-probe insert did not synchronously append index entry");
  Require(HasEvidenceKind(result.evidence, "constraint_proof_store"),
          "IPAR unique-probe insert did not store unique preflight proof");
}

void RequirePhysicalProbeEvidence(const api::EngineInsertRowsResult& result,
                                  std::string_view source_kind) {
  Require(HasEvidence(result.evidence,
                      source_kind,
                      "persisted_unique_index_physical_probe"),
          "IPAR unique-probe did not use persisted unique index probe");
  Require(HasEvidence(result.evidence,
                      "physical_unique_index_probe_path",
                      "mga_persisted_index_entry_lookup"),
          "IPAR unique-probe physical lookup path evidence missing");
  Require(HasEvidence(result.evidence,
                      "unique_index_physical_probes",
                      "1"),
          "IPAR unique-probe did not record exactly one physical probe");
  Require(HasEvidence(result.evidence,
                      "unique_index_physical_probe_hits",
                      "1"),
          "IPAR unique-probe did not record physical probe hit");
  Require(HasEvidence(result.evidence,
                      "unique_index_scan_fallbacks",
                      "0"),
          "IPAR unique-probe used scan fallback");
}

void RequireStatementOverlayEvidence(const api::EngineInsertRowsResult& result,
                                     std::string_view source_kind) {
  Require(HasEvidence(result.evidence, source_kind, "statement_delta_overlay"),
          "IPAR unique-probe same-statement duplicate did not use overlay");
  Require(HasEvidence(result.evidence,
                      "physical_unique_index_probe_path",
                      "statement_delta_overlay"),
          "IPAR unique-probe same-statement duplicate path mismatch");
  Require(HasEvidence(result.evidence,
                      "unique_index_physical_probe_hits",
                      "0"),
          "IPAR unique-probe same-statement path unexpectedly hit index");
  Require(HasEvidence(result.evidence,
                      "unique_index_scan_fallbacks",
                      "0"),
          "IPAR unique-probe same-statement path used scan fallback");
}

void VerifyDuplicateWithinStatementAndTransaction() {
  auto fixture = MakeFixture();

  auto statement = Begin(fixture, "ipar-unique-probe-same-statement");
  const auto same_statement = InsertBatchInto(
      fixture,
      statement,
      {"same-statement-key", "same-statement-key"});
  RequireUniqueViolation(
      same_statement,
      "IPAR unique-probe same-statement duplicate unexpectedly succeeded");
  RequireStatementOverlayEvidence(same_statement,
                                  "insert_unique_probe_candidate_source");
  Rollback(statement);

  auto after_statement = Begin(fixture, "ipar-unique-probe-after-statement");
  const auto clean_statement_key =
      InsertInto(fixture, after_statement, "same-statement-key");
  RequireInsertOk(clean_statement_key,
                  "IPAR unique-probe statement rollback release insert failed");
  RequireSynchronousUniqueInsertEvidence(clean_statement_key);
  Commit(after_statement);

  auto transaction = Begin(fixture, "ipar-unique-probe-same-transaction");
  const auto first = InsertInto(fixture, transaction, "same-transaction-key");
  RequireInsertOk(first, "IPAR unique-probe same-transaction first insert failed");
  RequireSynchronousUniqueInsertEvidence(first);

  const auto same_transaction =
      InsertInto(fixture, transaction, "same-transaction-key");
  RequireUniqueViolation(
      same_transaction,
      "IPAR unique-probe same-transaction duplicate unexpectedly succeeded");
  RequirePhysicalProbeEvidence(same_transaction,
                               "insert_unique_probe_candidate_source");
  Rollback(transaction);
}

void VerifyRollbackSavepointAndRestartRelease() {
  auto fixture = MakeFixture();

  auto rolled_back = Begin(fixture, "ipar-unique-probe-rollback-source");
  const auto rollback_source = InsertInto(fixture, rolled_back, "rollback-key");
  RequireInsertOk(rollback_source, "IPAR unique-probe rollback source insert failed");
  RequireSynchronousUniqueInsertEvidence(rollback_source);
  Rollback(rolled_back);

  auto released = Begin(fixture, "ipar-unique-probe-rollback-release");
  const auto rollback_release = InsertInto(fixture, released, "rollback-key");
  RequireInsertOk(rollback_release,
                  "IPAR unique-probe rollback did not release uniqueness");
  RequireSynchronousUniqueInsertEvidence(rollback_release);
  Commit(released);

  Reopen(fixture);
  auto restart_duplicate = Begin(fixture, "ipar-unique-probe-restart-duplicate");
  const auto restart_violation = InsertInto(fixture, restart_duplicate, "rollback-key");
  RequireUniqueViolation(
      restart_violation,
      "IPAR unique-probe committed duplicate after restart unexpectedly succeeded");
  RequirePhysicalProbeEvidence(restart_violation,
                               "insert_unique_probe_candidate_source");
  Rollback(restart_duplicate);

  auto savepoint = Begin(fixture, "ipar-unique-probe-savepoint");
  CreateSavepoint(savepoint, "before_unique");
  const auto before_rollback = InsertInto(fixture, savepoint, "savepoint-key");
  RequireInsertOk(before_rollback,
                  "IPAR unique-probe savepoint source insert failed");
  RequireSynchronousUniqueInsertEvidence(before_rollback);
  RollbackToSavepoint(savepoint, "before_unique");

  const auto after_savepoint_rollback =
      InsertInto(fixture, savepoint, "savepoint-key");
  RequireInsertOk(after_savepoint_rollback,
                  "IPAR unique-probe savepoint rollback did not release uniqueness");
  RequireSynchronousUniqueInsertEvidence(after_savepoint_rollback);
  Commit(savepoint);

  Reopen(fixture);
  auto savepoint_duplicate =
      Begin(fixture, "ipar-unique-probe-savepoint-restart-duplicate");
  const auto savepoint_violation =
      InsertInto(fixture, savepoint_duplicate, "savepoint-key");
  RequireUniqueViolation(
      savepoint_violation,
      "IPAR unique-probe savepoint committed duplicate unexpectedly succeeded");
  RequirePhysicalProbeEvidence(savepoint_violation,
                               "insert_unique_probe_candidate_source");
  Rollback(savepoint_duplicate);
}

void VerifyPhysicalUniqueProbe() {
  auto fixture = MakeFixture();

  auto seed = Begin(fixture, "ipar-unique-probe-seed");
  const auto seed_insert = InsertInto(fixture, seed, "dup-key");
  RequireInsertOk(seed_insert, "IPAR unique-probe seed insert failed");
  RequireSynchronousUniqueInsertEvidence(seed_insert);
  Commit(seed);

  Reopen(fixture);
  auto duplicate = Begin(fixture, "ipar-unique-probe-duplicate");
  const auto violation = InsertInto(fixture, duplicate, "dup-key");
  RequireUniqueViolation(violation,
                         "IPAR unique-probe duplicate insert unexpectedly succeeded");
  RequirePhysicalProbeEvidence(violation, "insert_unique_probe_candidate_source");
  Rollback(duplicate);

  auto conflict = Begin(fixture, "ipar-unique-probe-on-conflict");
  const auto do_nothing = InsertInto(
      fixture,
      conflict,
      "dup-key",
      {"on_conflict_action:do_nothing", "conflict_target_column:payload"});
  RequireInsertOk(do_nothing, "IPAR unique-probe ON CONFLICT probe failed");
  Require(do_nothing.skipped_count == 1,
          "IPAR unique-probe ON CONFLICT did not skip duplicate");
  RequirePhysicalProbeEvidence(do_nothing, "on_conflict_match_source");
  Rollback(conflict);
}

}  // namespace

int main() {
  VerifyDuplicateWithinStatementAndTransaction();
  VerifyRollbackSavepointAndRestartRelease();
  VerifyPhysicalUniqueProbe();
  return EXIT_SUCCESS;
}
