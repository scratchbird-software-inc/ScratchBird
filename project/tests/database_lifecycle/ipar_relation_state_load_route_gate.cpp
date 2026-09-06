// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "database_lifecycle.hpp"
#include "dml/insert_api.hpp"
#include "dml/mga_relation_read_view.hpp"
#include "dml/transactional_relation_store.hpp"
#include "mga_relation_store/mga_relation_store.hpp"
#include "metric_registry.hpp"
#include "transaction/transaction_api.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <unistd.h>
#include <vector>

namespace {

namespace api = scratchbird::engine::internal_api;
namespace db = scratchbird::storage::database;
namespace metrics = scratchbird::core::metrics;
namespace platform = scratchbird::core::platform;
namespace uuid = scratchbird::core::uuid;

static_assert(!std::is_constructible_v<api::CrudState,
                                       api::RelationReadSnapshot>,
              "compatibility read snapshots must not feed mutable CRUD state");

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
  Require(generated.ok(), "IPAR relation-state UUID generation failed");
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

const api::TransactionalRelationStoreAuthorityRecord* FindAuthority(
    std::string_view artifact) {
  for (const auto& record : api::TransactionalRelationStoreAuthorityMap()) {
    if (record.artifact == artifact) {
      return &record;
    }
  }
  return nullptr;
}

void VerifyCanonicalStoreAuthorityMap() {
  const auto* rows = FindAuthority("row_versions");
  Require(rows != nullptr && rows->classification == "canonical_durable",
          "canonical row-version authority is not explicit");
  const auto* finality = FindAuthority("transaction_finality");
  Require(finality != nullptr &&
              finality->authority == "durable_transaction_inventory" &&
              finality->classification == "canonical_durable",
          "durable transaction inventory finality authority is not explicit");
  const auto* cache = FindAuthority("relation_caches");
  Require(cache != nullptr &&
              cache->classification == "derived_non_authoritative",
          "relation cache was not classified as derived");
  const auto* compatibility = FindAuthority("crud_compatibility_state");
  Require(compatibility != nullptr &&
              compatibility->classification ==
                  "read_only_subordinate_projection",
          "compatibility projection was not classified as read-only/subordinate");
}

std::string EvidenceValue(const std::vector<api::EngineEvidenceReference>& evidence,
                          std::string_view kind) {
  for (const auto& item : evidence) {
    if (item.evidence_kind == kind) {
      return item.evidence_id;
    }
  }
  return {};
}

bool HasMetric(const std::vector<metrics::MetricValue>& values,
               std::string_view family,
               std::string_view object_uuid,
               std::string_view operation,
               std::string_view result,
               double minimum_value) {
  for (const auto& value : values) {
    if (value.family != family || value.value < minimum_value) { continue; }
    bool object_matches = false;
    bool operation_matches = false;
    bool result_matches = false;
    for (const auto& label : value.labels) {
      object_matches = object_matches ||
                       (label.key == "object_uuid" &&
                        label.value == object_uuid);
      operation_matches = operation_matches ||
                          (label.key == "operation" &&
                           label.value == operation);
      result_matches = result_matches ||
                       (label.key == "result" && label.value == result);
    }
    if (object_matches && operation_matches && result_matches) { return true; }
  }
  return false;
}

void RequireSameRows(std::vector<api::CrudRowVersionRecord> left,
                     std::vector<api::CrudRowVersionRecord> right) {
  const auto order = [](const api::CrudRowVersionRecord& lhs,
                        const api::CrudRowVersionRecord& rhs) {
    return std::tie(lhs.row_uuid, lhs.version_uuid, lhs.event_sequence) <
           std::tie(rhs.row_uuid, rhs.version_uuid, rhs.event_sequence);
  };
  std::sort(left.begin(), left.end(), order);
  std::sort(right.begin(), right.end(), order);
  Require(left.size() == right.size(),
          "IPAR scoped/full parity row count differs");
  for (std::size_t i = 0; i < left.size(); ++i) {
    Require(left[i].creator_tx == right[i].creator_tx &&
                left[i].event_sequence == right[i].event_sequence &&
                left[i].sequence == right[i].sequence &&
                left[i].table_uuid == right[i].table_uuid &&
                left[i].row_uuid == right[i].row_uuid &&
                left[i].version_uuid == right[i].version_uuid &&
                left[i].previous_version_uuid == right[i].previous_version_uuid &&
                left[i].previous_sequence == right[i].previous_sequence &&
                left[i].deleted == right[i].deleted &&
                left[i].values == right[i].values,
            "IPAR scoped/full parity row image differs");
  }
}

bool HasDiagnostic(const api::EngineApiResult& result, std::string_view detail) {
  const std::string needle(detail);
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.detail == needle ||
        diagnostic.detail.find(needle) != std::string::npos ||
        diagnostic.code.find(needle) != std::string::npos ||
        diagnostic.message_key.find(needle) != std::string::npos) {
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
  std::string target_table_uuid;
  std::string child_table_uuid;
  std::string unrelated_table_uuid;
  std::string target_index_uuid;
  std::string schema_uuid;
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

api::EngineRowValue Row(std::string value, std::string note = {}) {
  api::EngineRowValue row;
  row.fields.push_back({"payload", TextValue(std::move(value))});
  if (!note.empty()) {
    row.fields.push_back({"note", TextValue(std::move(note))});
  }
  return row;
}

api::CrudTableRecord Table(std::string table_uuid, std::string name,
                           std::uint64_t creator_tx,
                           bool primary_key = false) {
  api::CrudTableRecord table;
  table.creator_tx = creator_tx;
  table.table_uuid = std::move(table_uuid);
  table.default_name = std::move(name);
  table.columns.push_back({"payload",
                           primary_key ? "canonical=character;primary_key=true"
                                       : "canonical=character"});
  table.columns.push_back({"note", "canonical=character"});
  return table;
}

api::CrudTableRecord ChildTable(const Fixture& fixture, std::uint64_t creator_tx) {
  api::CrudTableRecord table;
  table.creator_tx = creator_tx;
  table.table_uuid = fixture.child_table_uuid;
  table.default_name = "ipar_relation_state_child";
  table.columns.push_back({"payload",
                           "canonical=character;referenced_table_uuid=" +
                               fixture.target_table_uuid + ";referenced_column=payload"});
  return table;
}

api::CrudIndexRecord UniquePayloadIndex(const Fixture& fixture,
                                        std::uint64_t creator_tx) {
  api::CrudIndexRecord index;
  index.creator_tx = creator_tx;
  index.index_uuid = fixture.target_index_uuid;
  index.table_uuid = fixture.target_table_uuid;
  index.column_name = "payload";
  index.family = api::kCrudIndexFamilyBtree;
  index.profile = api::kCrudIndexProfileRowStoreScalarBtreeV1;
  index.default_name = "ipar_relation_state_payload_uidx";
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
  context.principal_uuid.canonical = NewUuidText(platform::UuidKind::principal, fixture.salt + 101);
  context.session_uuid.canonical = NewUuidText(platform::UuidKind::object, fixture.salt + 102);
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
  if (!begun.ok) {
    for (const auto& diagnostic : begun.diagnostics) {
      std::cerr << diagnostic.code << ":" << diagnostic.detail << '\n';
    }
  }
  Require(begun.ok, "IPAR relation-state begin transaction failed");
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
  if (!committed.ok) {
    for (const auto& diagnostic : committed.diagnostics) {
      std::cerr << diagnostic.code << ":" << diagnostic.detail << '\n';
    }
  }
  Require(committed.ok, "IPAR relation-state commit failed");
}

Fixture MakeFixture() {
  Fixture fixture;
  fixture.salt = MillisSeed();
  fixture.dir = std::filesystem::temp_directory_path() /
                ("scratchbird_ipar_relation_state_" +
                 std::to_string(fixture.salt) + "_" +
                 std::to_string(static_cast<long long>(getpid())));
  std::filesystem::create_directories(fixture.dir);
  fixture.database_path = fixture.dir / "ipar_relation_state.sbdb";

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
  Require(created.ok(), "IPAR relation-state database create failed");

  fixture.database_uuid = uuid::UuidToString(create.database_uuid.value);
  fixture.schema_uuid = NewUuidText(platform::UuidKind::schema, fixture.salt + 10);
  fixture.target_table_uuid = NewUuidText(platform::UuidKind::object, fixture.salt + 20);
  fixture.child_table_uuid = NewUuidText(platform::UuidKind::object, fixture.salt + 21);
  fixture.target_index_uuid = NewUuidText(platform::UuidKind::object, fixture.salt + 22);
  fixture.unrelated_table_uuid = NewUuidText(platform::UuidKind::object, fixture.salt + 30);

  auto metadata = Begin(fixture, "ipar-relation-state-metadata");
  const auto target = api::AppendMgaTableMetadata(
      metadata,
      Table(fixture.target_table_uuid, "ipar_relation_state_target",
            metadata.local_transaction_id, true));
  Require(!target.error, "IPAR relation-state target metadata append failed");
  const auto child = api::AppendMgaTableMetadata(
      metadata,
      ChildTable(fixture, metadata.local_transaction_id));
  Require(!child.error, "IPAR relation-state child metadata append failed");
  const auto target_index = api::AppendMgaIndexMetadata(
      metadata,
      UniquePayloadIndex(fixture, metadata.local_transaction_id));
  Require(!target_index.error, "IPAR relation-state target index metadata append failed");
  const auto unrelated = api::AppendMgaTableMetadata(
      metadata,
      Table(fixture.unrelated_table_uuid, "ipar_relation_state_unrelated",
            metadata.local_transaction_id));
  Require(!unrelated.error, "IPAR relation-state unrelated metadata append failed");
  Commit(metadata);
  return fixture;
}

api::EngineInsertRowsResult InsertInto(const Fixture& fixture,
                                       const api::EngineRequestContext& context,
                                       const std::string& table_uuid,
                                       std::string payload,
                                       std::vector<std::string> options = {},
                                       std::string note = {}) {
  api::EngineInsertRowsRequest request;
  request.context = context;
  request.target_schema.uuid.canonical = fixture.schema_uuid;
  request.target_table.uuid.canonical = table_uuid;
  request.target_table.object_kind = "table";
  request.target_object.uuid.canonical = table_uuid;
  request.target_object.object_kind = "table";
  request.estimated_row_count = 1;
  request.input_rows.push_back(Row(std::move(payload), std::move(note)));
  request.option_envelopes = std::move(options);
  return api::EngineInsertRows(request);
}

void RequireInsertOk(const api::EngineInsertRowsResult& result,
                     std::string_view message) {
  if (!result.ok) {
    for (const auto& diagnostic : result.diagnostics) {
      std::cerr << diagnostic.code << ":" << diagnostic.detail << '\n';
    }
  }
  Require(result.ok, message);
}

void VerifyRelationStateLoadRoutes() {
  auto fixture = MakeFixture();

  auto seed = Begin(fixture, "ipar-relation-state-seed");
  RequireInsertOk(InsertInto(fixture, seed, fixture.target_table_uuid, "target-1", {}, "seed"),
                  "IPAR relation-state target seed insert failed");
  RequireInsertOk(InsertInto(fixture, seed, fixture.child_table_uuid, "target-1"),
                  "IPAR relation-state child seed insert failed");
  RequireInsertOk(InsertInto(fixture, seed, fixture.unrelated_table_uuid, "unrelated-1"),
                  "IPAR relation-state unrelated seed insert 1 failed");
  RequireInsertOk(InsertInto(fixture, seed, fixture.unrelated_table_uuid, "unrelated-2"),
                  "IPAR relation-state unrelated seed insert 2 failed");
  Commit(seed);

  auto verify = Begin(fixture, "ipar-relation-state-verify");
  api::TransactionalRelationStore relation_store(verify);
  const auto diagnostic_full_load = relation_store.LoadDiagnosticFullState();
  Require(diagnostic_full_load.ok,
          "IPAR relation-state diagnostic full loader failed");
  Require(diagnostic_full_load.full_state_load,
          "IPAR relation-state diagnostic loader did not mark full load");
  Require(!diagnostic_full_load.scoped_state_load,
          "IPAR relation-state diagnostic loader incorrectly marked scoped load");
  Require(diagnostic_full_load.row_versions_retained == 4,
          "IPAR relation-state diagnostic full loader did not retain all rows");
  Require(HasEvidence(diagnostic_full_load.evidence,
                      "transactional_relation_store_route",
                      "normal_dml.diagnostic_full_state.v1"),
          "diagnostic full load did not traverse the canonical store facade");

  const auto refused = InsertInto(
      fixture,
      verify,
      fixture.target_table_uuid,
      "target-refused",
      {"relation_state_load=full"});
  Require(!refused.ok,
          "IPAR relation-state mutation accepted caller-forced full load");
  if (!HasDiagnostic(refused, "relation_state_full_load_diagnostic_only")) {
    DumpDiagnostics(refused);
    Fail("IPAR relation-state full-load refusal diagnostic missing");
  }
  Require(HasEvidence(refused.evidence,
                      "relation_state_full_load_refused",
                      "diagnostic_only"),
          "IPAR relation-state full-load refusal evidence missing");
  Require(HasEvidence(refused.evidence, "relation_state_full_loads", "0"),
          "IPAR relation-state refusal incorrectly loaded full state");

  const auto normal = InsertInto(fixture,
                                 verify,
                                 fixture.target_table_uuid,
                                 "target-2");
  RequireInsertOk(normal, "IPAR relation-state normal scoped insert failed");
  Require(HasEvidence(normal.evidence, "relation_state_full_loads", "0"),
          "IPAR relation-state normal insert performed full load");
  Require(HasEvidence(normal.evidence, "relation_state_scoped_loads", "1"),
          "IPAR relation-state normal insert did not perform scoped load");
  Require(HasEvidence(normal.evidence,
                      "relation_state_load_reason",
                      "target_table_insert_scope"),
          "IPAR relation-state scoped reason evidence missing");
  Require(HasEvidence(normal.evidence,
                      "transactional_relation_store",
                      "canonical_normal_dml_v1"),
          "IPAR relation-state insert did not identify the canonical store");
  Require(HasEvidence(normal.evidence,
                      "transactional_relation_store_route",
                      "normal_dml.insert_target.v1"),
          "IPAR relation-state insert did not identify its runtime route");
  Require(HasEvidence(normal.evidence,
                      "transactional_relation_store_finality_authority",
                      "durable_transaction_inventory"),
          "IPAR relation-state insert did not identify MGA finality authority");
  Require(HasEvidence(normal.evidence,
                      "transactional_relation_store_read_model",
                      "mga_scoped_read_view_v1"),
          "IPAR relation-state insert did not use the MGA read model");
  Require(EvidenceValue(normal.evidence,
                        "mga_relation_state_row_versions_retained") == "2",
          "IPAR relation-state scoped loader did not retain only target and child row state");
  Require(EvidenceValue(normal.evidence,
                        "mga_relation_state_row_versions_scanned") == "2",
          "IPAR relation-state scoped loader scanned unrelated row state");
  Require(EvidenceValue(normal.evidence,
                        "mga_relation_state_index_entries_scanned") == "1",
          "IPAR relation-state scoped loader scanned unrelated index state");
  Require(HasEvidence(normal.evidence,
                      "mga_relation_state_scoped_physical_segments",
                      "true"),
          "IPAR relation-state scoped physical segment evidence missing");
  Require(HasEvidence(normal.evidence,
                      "mga_relation_state_scoped_physical_fallback",
                      "false"),
          "IPAR relation-state scoped loader used global fallback");

  const auto do_nothing = InsertInto(
      fixture,
      verify,
      fixture.target_table_uuid,
      "target-1",
      {"on_conflict_action:do_nothing", "conflict_target_column:payload"},
      "ignored");
  RequireInsertOk(do_nothing, "IPAR relation-state ON CONFLICT DO NOTHING failed");
  Require(do_nothing.skipped_count == 1,
          "IPAR relation-state ON CONFLICT DO NOTHING did not skip duplicate");
  Require(HasEvidence(do_nothing.evidence, "relation_state_full_loads", "0"),
          "IPAR relation-state ON CONFLICT DO NOTHING performed full load");
  Require(HasEvidence(do_nothing.evidence, "relation_state_scoped_loads", "1"),
          "IPAR relation-state ON CONFLICT DO NOTHING did not perform scoped load");
  Require(HasEvidence(do_nothing.evidence,
                      "mga_relation_state_scoped_physical_segments",
                      "true"),
          "IPAR relation-state ON CONFLICT DO NOTHING did not use scoped physical segments");
  Require(HasEvidence(do_nothing.evidence,
                      "mga_relation_state_scoped_physical_fallback",
                      "false"),
          "IPAR relation-state ON CONFLICT DO NOTHING used global fallback");

  const auto do_update = InsertInto(
      fixture,
      verify,
      fixture.target_table_uuid,
      "target-1",
      {"on_conflict_action:do_update",
       "conflict_target_column:payload",
       "on_conflict_update_column:note"},
      "updated");
  RequireInsertOk(do_update, "IPAR relation-state ON CONFLICT DO UPDATE failed");
  Require(do_update.updated_count == 1,
          "IPAR relation-state ON CONFLICT DO UPDATE did not update duplicate");
  Require(HasEvidence(do_update.evidence, "relation_state_full_loads", "0"),
          "IPAR relation-state ON CONFLICT DO UPDATE performed full load");
  Require(HasEvidence(do_update.evidence, "relation_state_scoped_loads", "1"),
          "IPAR relation-state ON CONFLICT DO UPDATE did not perform scoped load");
  Require(HasEvidence(do_update.evidence,
                      "relation_state_load_reason",
                      "target_table_insert_and_child_reference_scope"),
          "IPAR relation-state ON CONFLICT DO UPDATE reference-scope evidence missing");
  Require(EvidenceValue(do_update.evidence,
                        "mga_relation_state_row_versions_retained") == "3",
          "IPAR relation-state ON CONFLICT DO UPDATE retained unrelated row state");
  Require(EvidenceValue(do_update.evidence,
                        "mga_relation_state_row_versions_scanned") == "3",
          "IPAR relation-state ON CONFLICT DO UPDATE scanned unrelated row state");
  Require(HasEvidence(do_update.evidence,
                      "mga_relation_state_scoped_physical_segments",
                      "true"),
          "IPAR relation-state ON CONFLICT DO UPDATE did not use scoped physical segments");
  Require(HasEvidence(do_update.evidence,
                      "mga_relation_state_scoped_physical_fallback",
                      "false"),
          "IPAR relation-state ON CONFLICT DO UPDATE used global fallback");
  Commit(verify);
}

void VerifyScopedMaterializationAndCursorParity() {
  auto fixture = MakeFixture();

  auto seed = Begin(fixture, "ipar-relation-state-shape-seed");
  RequireInsertOk(InsertInto(fixture, seed, fixture.target_table_uuid,
                             "target-shape-1"),
                  "IPAR relation-state shape target insert 1 failed");
  RequireInsertOk(InsertInto(fixture, seed, fixture.target_table_uuid,
                             "target-shape-2"),
                  "IPAR relation-state shape target insert 2 failed");
  constexpr std::size_t kUnrelatedRows = 48;
  for (std::size_t i = 0; i < kUnrelatedRows; ++i) {
    RequireInsertOk(
        InsertInto(fixture, seed, fixture.unrelated_table_uuid,
                   "unrelated-shape-" + std::to_string(i)),
        "IPAR relation-state shape unrelated insert failed");
  }
  Commit(seed);

  auto verify = Begin(fixture, "ipar-relation-state-shape-verify");
  api::TransactionalRelationStore relation_store(verify);
  const auto full = relation_store.LoadDiagnosticFullState();
  const auto scoped = relation_store.OpenRelationScan(fixture.target_table_uuid);
  Require(full.ok && scoped.ok,
          "IPAR relation-state shape loaders failed");
  Require(full.full_state_load && !full.scoped_state_load,
          "IPAR reference loader did not report full-state materialization");
  Require(!scoped.full_state_load && scoped.scoped_state_load,
          "IPAR relation scan did not report scoped materialization");
  Require(full.rows_materialized >= kUnrelatedRows + 2,
          "IPAR full-state shape did not contain the unrelated corpus");
  Require(scoped.rows_materialized == 2,
          "IPAR relation scan materialized rows outside its UUID scope");
  Require(scoped.rows_materialized < full.rows_materialized,
          "IPAR relation scan row materialization was not bounded");
  Require(scoped.metadata_records_materialized <
              full.metadata_records_materialized,
          "IPAR relation scan metadata materialization was not UUID scoped");
  Require(scoped.bytes_materialized < full.bytes_materialized,
          "IPAR relation scan byte materialization was not bounded");
  Require(scoped.allocation_units_materialized <
              full.allocation_units_materialized,
          "IPAR relation scan allocation footprint was not bounded");
  Require(EvidenceValue(scoped.evidence,
                        "mga_relation_state_rows_materialized") == "2",
          "IPAR relation scan actual-row evidence is missing");
  Require(EvidenceValue(scoped.evidence,
                        "mga_relation_state_bytes_materialized") ==
              std::to_string(scoped.bytes_materialized),
          "IPAR relation scan actual-byte evidence is missing");
  Require(EvidenceValue(scoped.evidence,
                        "mga_relation_state_allocation_units_materialized") ==
              std::to_string(scoped.allocation_units_materialized),
          "IPAR relation scan allocation evidence is missing");
  Require(HasEvidence(scoped.evidence,
                      "mga_relation_state_operation_family", "select") &&
              HasEvidence(scoped.evidence,
                          "mga_relation_state_target_relation_uuid",
                          fixture.target_table_uuid) &&
              HasEvidence(scoped.evidence,
                          "mga_relation_state_load_reason",
                          "transaction_visible_relation_scan"),
          "IPAR relation scan identity/reason evidence is incomplete");
  Require(HasEvidence(full.evidence,
                      "mga_relation_state_full_load_policy_reason",
                      "explicit_diagnostic_inventory") &&
              !EvidenceValue(full.evidence,
                             "mga_relation_state_full_load_maximum_rows")
                   .empty() &&
              !EvidenceValue(full.evidence,
                             "mga_relation_state_full_load_maximum_bytes")
                   .empty() &&
              !EvidenceValue(
                   full.evidence,
                   "mga_relation_state_full_load_maximum_allocation_units")
                   .empty(),
          "IPAR diagnostic full load lacks its named bounded policy");

  const auto full_view = api::BuildMgaRelationReadView(full.state);
  const auto scoped_view = api::BuildMgaRelationReadView(scoped.state);
  const auto full_rows = api::VisibleMgaRowsForContext(
      full_view, fixture.target_table_uuid, verify);
  const auto scoped_rows = api::VisibleMgaRowsForContext(
      scoped_view, fixture.target_table_uuid, verify);
  RequireSameRows(full_rows, scoped_rows);
  Require(!scoped_rows.empty(),
          "IPAR scoped/full parity corpus unexpectedly has no rows");

  const auto point = relation_store.OpenRelationPointCursor(
      fixture.target_table_uuid, scoped_rows.front().row_uuid);
  Require(point.ok && point.scoped_state_load && !point.full_state_load,
          "IPAR point cursor did not remain scoped");
  Require(point.state.row_versions.size() == 1 &&
              point.state.row_versions.front().row_uuid ==
                  scoped_rows.front().row_uuid,
          "IPAR point cursor retained rows outside the stable row UUID");
  Require(point.row_versions_scanned == 2 &&
              point.row_versions_retained == 1 &&
              point.rows_materialized == 1,
          "IPAR point cursor scan/retention accounting is not exact");
  Require(HasEvidence(point.evidence,
                      "transactional_relation_store_route",
                      "normal_dml.relation_point_cursor.v1"),
          "IPAR point cursor route evidence is missing");

  const auto index =
      relation_store.OpenRelationIndexCursor(fixture.target_table_uuid);
  Require(index.ok && index.scoped_state_load && !index.full_state_load,
          "IPAR index cursor did not remain relation scoped");
  Require(index.index_entries_retained == 2,
          "IPAR index cursor did not retain the target relation entries");
  Require(index.rows_materialized == 0 && index.state.row_versions.empty(),
          "IPAR index cursor unnecessarily materialized relation rows");
  Require(HasEvidence(index.evidence,
                      "transactional_relation_store_route",
                      "normal_dml.relation_index_cursor.v1"),
          "IPAR index cursor route evidence is missing");

  const auto constraint =
      relation_store.LoadConstraintScope(fixture.target_table_uuid);
  Require(constraint.ok && constraint.scoped_state_load &&
              !constraint.full_state_load,
          "IPAR constraint lookup did not remain scoped");
  Require(constraint.rows_materialized == 2 &&
              constraint.metadata_records_materialized <
                  full.metadata_records_materialized,
          "IPAR constraint lookup retained unrelated relation state");
  Require(HasEvidence(constraint.evidence,
                      "transactional_relation_store_route",
                      "normal_dml.constraint_scope.v1"),
          "IPAR constraint lookup route evidence is missing");

  const auto trigger =
      relation_store.LoadTriggerMetadataScope(fixture.target_table_uuid);
  Require(trigger.ok && trigger.scoped_state_load &&
              !trigger.full_state_load && trigger.rows_materialized == 0 &&
              trigger.state.relation_metadata.tables.size() == 1,
          "IPAR trigger metadata lookup retained non-target relation state");
  Require(HasEvidence(trigger.evidence,
                      "transactional_relation_store_route",
                      "normal_dml.trigger_metadata_scope.v1"),
          "IPAR trigger metadata lookup route evidence is missing");

  const auto metric_values =
      metrics::DefaultMetricRegistry().SnapshotCurrent(false);
  Require(HasMetric(metric_values, "sb_mga_relation_state_load_total",
                    fixture.target_table_uuid, "select", "scoped", 1.0),
          "IPAR scoped relation-load counter was not published");
  Require(HasMetric(metric_values,
                    "sb_mga_relation_state_rows_materialized_total",
                    fixture.target_table_uuid, "select", "scoped", 2.0),
          "IPAR scoped rows-materialized counter was not published");
  Require(HasMetric(metric_values,
                    "sb_mga_relation_state_bytes_materialized_total",
                    fixture.target_table_uuid, "select", "scoped", 1.0),
          "IPAR scoped bytes-materialized counter was not published");
  Require(HasMetric(
              metric_values,
              "sb_mga_relation_state_allocation_units_materialized_total",
              fixture.target_table_uuid, "select", "scoped", 1.0),
          "IPAR scoped allocation-materialized counter was not published");
  Commit(verify);
}

}  // namespace

int main() {
  VerifyCanonicalStoreAuthorityMap();
  VerifyRelationStateLoadRoutes();
  VerifyScopedMaterializationAndCursorParity();
  return EXIT_SUCCESS;
}
