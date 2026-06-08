// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// ODF-111 DML row-location benchmark closure gate.

#include "database_lifecycle.hpp"
#include "dml/delete_api.hpp"
#include "dml/insert_api.hpp"
#include "dml/select_api.hpp"
#include "dml/update_api.hpp"
#include "mga_relation_store/mga_relation_store.hpp"
#include "transaction/transaction_api.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <map>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace api = scratchbird::engine::internal_api;
namespace db = scratchbird::storage::database;
namespace platform = scratchbird::core::platform;
namespace uuid = scratchbird::core::uuid;

#ifndef ODF111_OUTPUT_JSON
#define ODF111_OUTPUT_JSON "optimizer_deficiency_odf_111_gate.json"
#endif

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

platform::TypedUuid NewUuid(platform::UuidKind kind, platform::u64 salt) {
  const auto generated = uuid::GenerateEngineIdentityV7(kind, NowMillis() + salt);
  Require(generated.ok(), "ODF-111 generated UUID creation failed");
  return generated.value;
}

std::string NewUuidText(platform::UuidKind kind, platform::u64 salt) {
  return uuid::UuidToString(NewUuid(kind, salt).value);
}

api::EngineTypedValue TextValue(std::string value) {
  api::EngineTypedValue typed;
  typed.descriptor.descriptor_kind = "scalar";
  typed.descriptor.canonical_type_name = "character";
  typed.descriptor.encoded_descriptor = "canonical=character";
  typed.encoded_value = std::move(value);
  return typed;
}

api::EngineRowValue Row(std::string id,
                        std::string name,
                        std::string note,
                        std::string tag = "red") {
  api::EngineRowValue row;
  row.fields.push_back({"id", TextValue(std::move(id))});
  row.fields.push_back({"name", TextValue(std::move(name))});
  row.fields.push_back({"note", TextValue(std::move(note))});
  row.fields.push_back({"tag", TextValue(std::move(tag))});
  return row;
}

bool HasEvidence(const std::vector<api::EngineEvidenceReference>& evidence,
                 std::string_view kind,
                 std::string_view id) {
  for (const auto& entry : evidence) {
    if (entry.evidence_kind == kind && entry.evidence_id == id) {
      return true;
    }
  }
  return false;
}

bool EvidenceContains(const std::vector<api::EngineEvidenceReference>& evidence,
                      std::string_view kind,
                      std::string_view token) {
  for (const auto& entry : evidence) {
    if (entry.evidence_kind == kind &&
        entry.evidence_id.find(token) != std::string::npos) {
      return true;
    }
  }
  return false;
}

bool AnyEvidenceContains(const std::vector<api::EngineEvidenceReference>& evidence,
                         std::string_view token) {
  for (const auto& entry : evidence) {
    if (entry.evidence_kind.find(token) != std::string::npos ||
        entry.evidence_id.find(token) != std::string::npos) {
      return true;
    }
  }
  return false;
}

std::uint64_t EvidenceCounter(
    const std::vector<api::EngineEvidenceReference>& evidence,
    std::string_view kind) {
  for (const auto& entry : evidence) {
    if (entry.evidence_kind == kind) {
      try {
        return static_cast<std::uint64_t>(std::stoull(entry.evidence_id));
      } catch (...) {
        return 0;
      }
    }
  }
  return 0;
}

void RequireNoForbiddenEvidence(
    const std::vector<api::EngineEvidenceReference>& evidence,
    std::string_view scenario) {
  for (const auto& entry : evidence) {
    for (const auto forbidden : {"docs" "/execution-plans",
                                 "docs" "/findings",
                                 "public_release_evidence",
                                 "docs/reference",
                                 "execution_plan",
                                 "findings",
                                 "contracts",
                                 "references"}) {
      if (entry.evidence_kind.find(forbidden) != std::string::npos ||
          entry.evidence_id.find(forbidden) != std::string::npos) {
        std::cerr << "Forbidden runtime evidence token in " << scenario << ": "
                  << entry.evidence_kind << '=' << entry.evidence_id << '\n';
        Fail("ODF-111 runtime evidence leaked documentation dependency");
      }
    }
  }
}

std::string FirstDetail(const api::EngineApiResult& result) {
  return result.diagnostics.empty() ? std::string{} : result.diagnostics.front().detail;
}

bool IsDuplicateKeyDetail(std::string_view detail) {
  return detail == "crud.unique_index:unique_index_duplicate" ||
         detail.find("duplicate_key") != std::string_view::npos;
}

std::string FieldValue(const api::EngineInsertRowsResult& result,
                       std::string_view field_name) {
  if (result.result_shape.rows.empty()) {
    return {};
  }
  for (const auto& [field, value] : result.result_shape.rows.front().fields) {
    if (field == field_name) {
      return value.encoded_value;
    }
  }
  return {};
}

std::string JsonEscape(std::string_view value) {
  std::string out;
  out.reserve(value.size() + 8);
  for (const char ch : value) {
    switch (ch) {
      case '\\':
        out += "\\\\";
        break;
      case '"':
        out += "\\\"";
        break;
      case '\b':
        out += "\\b";
        break;
      case '\f':
        out += "\\f";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        if (static_cast<unsigned char>(ch) < 0x20) {
          std::ostringstream escaped;
          escaped << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                  << static_cast<unsigned>(static_cast<unsigned char>(ch));
          out += escaped.str();
        } else {
          out.push_back(ch);
        }
    }
  }
  return out;
}

std::string Quote(std::string_view value) {
  return "\"" + JsonEscape(value) + "\"";
}

std::string StableHash(std::string_view input) {
  std::uint64_t hash = 1469598103934665603ull;
  for (const unsigned char ch : input) {
    hash ^= ch;
    hash *= 1099511628211ull;
  }
  std::ostringstream out;
  out << std::hex << std::setw(16) << std::setfill('0') << hash;
  return out.str();
}

struct ScenarioEvidence {
  std::string name;
  std::string operation;
  std::string selected_stream;
  std::string access_kind;
  std::uint64_t actual_count = 0;
  std::string benchmark_clean;
  std::string fallback_reason;
  std::vector<std::string> proofs;
  std::string result_hash;
};

std::string ScenarioHashSeed(const ScenarioEvidence& scenario) {
  std::ostringstream seed;
  seed << scenario.name << '|' << scenario.operation << '|'
       << scenario.selected_stream << '|' << scenario.access_kind << '|'
       << scenario.actual_count << '|' << scenario.benchmark_clean << '|'
       << scenario.fallback_reason;
  for (const auto& proof : scenario.proofs) {
    seed << '|' << proof;
  }
  return seed.str();
}

void FinalizeScenario(ScenarioEvidence* scenario) {
  scenario->result_hash = StableHash(ScenarioHashSeed(*scenario));
}

struct Fixture {
  std::filesystem::path dir;
  std::filesystem::path database_path;
  std::string database_uuid;
  std::string table_uuid;
  std::string id_index_uuid;
  std::string name_index_uuid;
  platform::u64 salt = 0;

  ~Fixture() {
    std::error_code ignored;
    if (!dir.empty()) {
      std::filesystem::remove_all(dir, ignored);
    }
  }
};

api::EngineRequestContext BaseContext(const Fixture& fixture,
                                      std::string request_id,
                                      bool security_context_present = true) {
  api::EngineRequestContext context;
  context.trust_mode = api::EngineTrustMode::server_isolated;
  context.request_id = std::move(request_id);
  context.database_path = fixture.database_path.string();
  context.database_uuid.canonical = fixture.database_uuid;
  context.principal_uuid.canonical =
      NewUuidText(platform::UuidKind::principal, fixture.salt + 100);
  context.session_uuid.canonical =
      NewUuidText(platform::UuidKind::object, fixture.salt + 101);
  context.security_context_present = security_context_present;
  context.identifier_profile_uuid = "sbsql_v3";
  context.language_context.language_tag = "en";
  context.language_context.default_language_tag = "en";
  context.catalog_generation_id = 10;
  context.security_epoch = 20;
  context.resource_epoch = 30;
  context.name_resolution_epoch = 40;
  context.trace_tags = {"optimizer_deficiency_odf_111_gate",
                        "benchmark_clean",
                        "mga_transaction_regression"};
  return context;
}

api::EngineRequestContext Begin(const Fixture& fixture,
                                std::string request_id,
                                bool security_context_present = true) {
  api::EngineBeginTransactionRequest request;
  request.context =
      BaseContext(fixture, std::move(request_id), security_context_present);
  request.isolation_level = "read_committed";
  const auto begun = api::EngineBeginTransaction(request);
  RequireOk(begun, "ODF-111 begin transaction failed");
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
  RequireOk(api::EngineCommitTransaction(request), "ODF-111 commit failed");
}

void Rollback(const api::EngineRequestContext& context) {
  api::EngineRollbackTransactionRequest request;
  request.context = context;
  RequireOk(api::EngineRollbackTransaction(request), "ODF-111 rollback failed");
}

api::CrudTableRecord Table(const Fixture& fixture,
                           const api::EngineRequestContext& context) {
  api::CrudTableRecord table;
  table.creator_tx = context.local_transaction_id;
  table.table_uuid = fixture.table_uuid;
  table.default_name = "odf111_dml_row_location";
  table.columns.push_back({"id", "canonical=character;primary_key=true"});
  table.columns.push_back({"name", "canonical=character"});
  table.columns.push_back({"note", "canonical=character"});
  table.columns.push_back({"tag", "canonical=character"});
  return table;
}

api::CrudIndexRecord Index(const Fixture& fixture,
                           const api::EngineRequestContext& context,
                           std::string index_uuid,
                           std::string column,
                           bool unique) {
  api::CrudIndexRecord index;
  index.creator_tx = context.local_transaction_id;
  index.index_uuid = std::move(index_uuid);
  index.table_uuid = fixture.table_uuid;
  index.column_name = std::move(column);
  index.family = api::kCrudIndexFamilyBtree;
  index.profile = api::kCrudIndexProfileRowStoreScalarBtreeV1;
  index.unique = unique;
  index.key_envelopes.push_back(index.column_name);
  if (unique) {
    index.key_envelopes.push_back("unique");
  }
  return index;
}

Fixture MakeFixture(std::string name, platform::u64 salt) {
  Fixture fixture;
  fixture.salt = salt;
  fixture.dir = std::filesystem::temp_directory_path() /
                ("scratchbird_odf111_" + name + "_" +
                 std::to_string(NowMillis() + salt));
  std::filesystem::create_directories(fixture.dir);
  fixture.database_path = fixture.dir / "odf111.sbdb";

  db::DatabaseCreateConfig create;
  create.path = fixture.database_path.string();
  create.database_uuid = NewUuid(platform::UuidKind::database, salt + 1);
  create.filespace_uuid = NewUuid(platform::UuidKind::filespace, salt + 2);
  create.creation_unix_epoch_millis = NowMillis() + salt + 3;
  create.require_resource_seed_pack = false;
  create.allow_minimal_resource_bootstrap = true;
  create.allow_overwrite = true;
  const auto created = db::CreateDatabaseFile(create);
  if (!created.ok()) {
    std::cerr << created.diagnostic.diagnostic_code << ':'
              << created.diagnostic.message_key << '\n';
  }
  Require(created.ok(), "ODF-111 database create failed");

  fixture.database_uuid = uuid::UuidToString(create.database_uuid.value);
  fixture.table_uuid = NewUuidText(platform::UuidKind::object, salt + 10);
  fixture.id_index_uuid = NewUuidText(platform::UuidKind::object, salt + 11);
  fixture.name_index_uuid = NewUuidText(platform::UuidKind::object, salt + 12);

  auto context = Begin(fixture, "odf111-metadata");
  RequireDiagnosticOk(api::AppendMgaTableMetadata(context, Table(fixture, context)),
                      "ODF-111 table metadata append failed");
  RequireDiagnosticOk(api::AppendMgaIndexMetadata(
                          context,
                          Index(fixture, context, fixture.id_index_uuid, "id", true)),
                      "ODF-111 id unique index metadata append failed");
  RequireDiagnosticOk(api::AppendMgaIndexMetadata(
                          context,
                          Index(fixture, context, fixture.name_index_uuid, "name", false)),
                      "ODF-111 name index metadata append failed");
  Commit(context);
  return fixture;
}

api::EngineInsertRowsResult InsertRows(
    const Fixture& fixture,
    const api::EngineRequestContext& context,
    std::vector<api::EngineRowValue> rows,
    std::string conflict_action = {},
    std::vector<std::string> conflict_update_columns = {},
    std::vector<std::string> options = {}) {
  api::EngineInsertRowsRequest request;
  request.context = context;
  request.target_table.uuid.canonical = fixture.table_uuid;
  request.target_table.object_kind = "table";
  request.input_rows = std::move(rows);
  request.estimated_row_count = request.input_rows.size();
  request.on_conflict_action = std::move(conflict_action);
  if (!request.on_conflict_action.empty()) {
    request.conflict_target_column = "id";
  }
  request.conflict_update_columns = std::move(conflict_update_columns);
  request.option_envelopes = std::move(options);
  return api::EngineInsertRows(request);
}

std::string SeedRow(Fixture& fixture,
                    std::string id,
                    std::string name,
                    std::string note,
                    std::string tag = "red") {
  auto context = Begin(fixture, "odf111-seed");
  const auto inserted = InsertRows(
      fixture,
      context,
      {Row(std::move(id), std::move(name), std::move(note), std::move(tag))});
  RequireOk(inserted, "ODF-111 seed insert failed");
  Require(inserted.inserted_count == 1, "ODF-111 seed insert count mismatch");
  Require(inserted.row_uuids.size() == 1, "ODF-111 seed row UUID missing");
  RequireNoForbiddenEvidence(inserted.evidence, "seed");
  Commit(context);
  return inserted.row_uuids.front().canonical;
}

void SeedThreeRows(Fixture& fixture) {
  SeedRow(fixture, "1", "amy", "alpha seed", "red");
  SeedRow(fixture, "2", "bob", "beta seed", "blue");
  SeedRow(fixture, "3", "cyd", "gamma seed", "red");
}

api::EnginePredicateEnvelope RowUuidPredicate(std::string row_uuid) {
  api::EnginePredicateEnvelope predicate;
  predicate.predicate_kind = "row_uuid_match";
  predicate.canonical_predicate_envelope = std::move(row_uuid);
  return predicate;
}

api::EnginePredicateEnvelope EqualsPredicate(std::string column,
                                             std::string value) {
  api::EnginePredicateEnvelope predicate;
  predicate.predicate_kind = "column_equals";
  predicate.canonical_predicate_envelope = std::move(column);
  predicate.bound_values.push_back(TextValue(std::move(value)));
  return predicate;
}

api::EnginePredicateEnvelope RangePredicate(std::string column,
                                            std::string lower,
                                            std::string upper) {
  api::EnginePredicateEnvelope predicate;
  predicate.predicate_kind = "column_range";
  predicate.canonical_predicate_envelope = std::move(column);
  predicate.bound_values.push_back(TextValue(std::move(lower)));
  predicate.bound_values.push_back(TextValue(std::move(upper)));
  return predicate;
}

api::EngineUpdateRowsResult Update(const Fixture& fixture,
                                   api::EngineRequestContext context,
                                   api::EnginePredicateEnvelope predicate,
                                   std::string note,
                                   std::vector<std::string> options = {}) {
  api::EngineUpdateRowsRequest request;
  request.context = std::move(context);
  request.target_table.uuid.canonical = fixture.table_uuid;
  request.target_table.object_kind = "table";
  request.update_predicate = std::move(predicate);
  request.assignments.push_back({"note", TextValue(std::move(note))});
  request.option_envelopes = std::move(options);
  return api::EngineUpdateRows(request);
}

api::EngineUpdateRowsResult UpdateNameNote(
    const Fixture& fixture,
    const api::EngineRequestContext& context,
    std::string id,
    std::string name,
    std::string note,
    std::vector<std::string> options = {}) {
  api::EngineUpdateRowsRequest request;
  request.context = context;
  request.target_table.uuid.canonical = fixture.table_uuid;
  request.target_table.object_kind = "table";
  request.update_predicate = EqualsPredicate("id", std::move(id));
  request.assignments.push_back({"name", TextValue(std::move(name))});
  request.assignments.push_back({"note", TextValue(std::move(note))});
  request.option_envelopes = std::move(options);
  return api::EngineUpdateRows(request);
}

api::EngineDeleteRowsResult Delete(const Fixture& fixture,
                                   api::EngineRequestContext context,
                                   api::EnginePredicateEnvelope predicate,
                                   std::vector<std::string> options = {}) {
  api::EngineDeleteRowsRequest request;
  request.context = std::move(context);
  request.target_table.uuid.canonical = fixture.table_uuid;
  request.target_table.object_kind = "table";
  request.delete_predicate = std::move(predicate);
  request.option_envelopes = std::move(options);
  return api::EngineDeleteRows(request);
}

api::MgaRelationStoreState LoadState(const Fixture& fixture,
                                     const api::EngineRequestContext& context) {
  const auto loaded = api::LoadMgaRelationStoreState(context);
  if (!loaded.ok) {
    std::cerr << loaded.diagnostic.code << ':' << loaded.diagnostic.detail << '\n';
  }
  Require(loaded.ok, "ODF-111 relation store load failed");
  (void)fixture;
  return loaded.state;
}

std::size_t CountVisibleIndexEntries(const Fixture& fixture,
                                     const api::EngineRequestContext& context,
                                     const std::string& index_uuid) {
  const auto loaded = LoadState(fixture, context);
  const auto state = api::BuildCrudCompatibilityStateFromMga(loaded);
  std::size_t count = 0;
  for (const auto& entry : loaded.index_entries) {
    if (entry.table_uuid == fixture.table_uuid &&
        entry.index_uuid == index_uuid &&
        api::CrudCreatorVisible(state,
                                entry.creator_tx,
                                entry.event_sequence,
                                context.local_transaction_id)) {
      ++count;
    }
  }
  return count;
}

void RequireAcceptedDmlEvidence(
    const std::vector<api::EngineEvidenceReference>& evidence,
    std::string_view stream_kind,
    std::string_view stream_value,
    std::string_view access_kind,
    std::string_view access_value,
    std::string_view scenario) {
  Require(HasEvidence(evidence, std::string(stream_kind), std::string(stream_value)),
          "ODF-111 selected row candidate stream evidence missing");
  Require(HasEvidence(evidence, std::string(access_kind), std::string(access_value)),
          "ODF-111 selected target access kind evidence missing");
  Require(EvidenceContains(evidence,
                           "dml_target_access_plan_evidence",
                           "mga_visibility_recheck=required"),
          "ODF-111 accepted route missing MGA recheck evidence");
  Require(EvidenceContains(evidence,
                           "dml_target_access_plan_evidence",
                           "security_recheck=required"),
          "ODF-111 accepted route missing security recheck evidence");
  Require(!HasEvidence(evidence, std::string(stream_kind), "table_scan"),
          "ODF-111 optimized route selected table scan");
  RequireNoForbiddenEvidence(evidence, scenario);
}

ScenarioEvidence UpdateScenario(std::string name,
                                api::EngineUpdateRowsResult result,
                                std::string expected_stream,
                                std::string expected_access,
                                std::uint64_t expected_count,
                                std::vector<std::string> extra_proofs = {}) {
  RequireOk(result, "ODF-111 update scenario failed");
  Require(result.updated_count == expected_count,
          "ODF-111 update scenario row count mismatch");
  RequireAcceptedDmlEvidence(result.evidence,
                             "update_row_candidate_stream",
                             expected_stream,
                             "update_target_access_kind",
                             expected_access,
                             name);
  ScenarioEvidence scenario;
  scenario.name = std::move(name);
  scenario.operation = "update";
  scenario.selected_stream = std::move(expected_stream);
  scenario.access_kind = std::move(expected_access);
  scenario.actual_count = result.updated_count;
  scenario.benchmark_clean = "admitted";
  scenario.proofs = {"mga_visibility_recheck",
                     "security_recheck",
                     "no_table_scan_selected"};
  scenario.proofs.insert(scenario.proofs.end(),
                         extra_proofs.begin(),
                         extra_proofs.end());
  FinalizeScenario(&scenario);
  return scenario;
}

ScenarioEvidence DeleteScenario(std::string name,
                                api::EngineDeleteRowsResult result,
                                std::string expected_stream,
                                std::string expected_access,
                                std::uint64_t expected_count,
                                std::vector<std::string> extra_proofs = {}) {
  RequireOk(result, "ODF-111 delete scenario failed");
  Require(result.deleted_count == expected_count,
          "ODF-111 delete scenario row count mismatch");
  RequireAcceptedDmlEvidence(result.evidence,
                             "delete_row_candidate_stream",
                             expected_stream,
                             "delete_target_access_kind",
                             expected_access,
                             name);
  Require(HasEvidence(result.evidence, "mga_row_version", "row_delete_tombstone"),
          "ODF-111 delete scenario missing tombstone evidence");
  ScenarioEvidence scenario;
  scenario.name = std::move(name);
  scenario.operation = "delete";
  scenario.selected_stream = std::move(expected_stream);
  scenario.access_kind = std::move(expected_access);
  scenario.actual_count = result.deleted_count;
  scenario.benchmark_clean = "admitted";
  scenario.proofs = {"mga_visibility_recheck",
                     "security_recheck",
                     "row_delete_tombstone",
                     "no_table_scan_selected"};
  scenario.proofs.insert(scenario.proofs.end(),
                         extra_proofs.begin(),
                         extra_proofs.end());
  FinalizeScenario(&scenario);
  return scenario;
}

ScenarioEvidence TableScanFallbackScenario(
    std::string name,
    std::string operation,
    std::uint64_t count,
    const std::vector<api::EngineEvidenceReference>& evidence,
    std::string stream_kind,
    std::string fallback_kind,
    std::string expected_reason) {
  Require(HasEvidence(evidence, stream_kind, "table_scan"),
          "ODF-111 fallback scenario did not select table scan");
  Require(HasEvidence(evidence, fallback_kind, expected_reason),
          "ODF-111 fallback scenario missing explicit fallback reason");
  RequireNoForbiddenEvidence(evidence, name);
  ScenarioEvidence scenario;
  scenario.name = std::move(name);
  scenario.operation = std::move(operation);
  scenario.selected_stream = "table_scan";
  scenario.access_kind = "table_scan";
  scenario.actual_count = count;
  scenario.benchmark_clean = "refused_for_optimized_timing";
  scenario.fallback_reason = std::move(expected_reason);
  scenario.proofs = {"explicit_table_scan_fallback",
                     "not_silently_admitted_to_optimized_benchmark"};
  FinalizeScenario(&scenario);
  return scenario;
}

std::vector<ScenarioEvidence> RunUpdateDeleteScenarios() {
  std::vector<ScenarioEvidence> scenarios;

  {
    auto fixture = MakeFixture("update_row_uuid", 111000);
    const auto row_uuid = SeedRow(fixture, "1", "amy", "seed");
    SeedRow(fixture, "2", "bob", "seed");
    auto context = Begin(fixture, "odf111-update-row-uuid");
    scenarios.push_back(UpdateScenario(
        "update_row_uuid_singleton",
        Update(fixture, context, RowUuidPredicate(row_uuid), "row-uuid-hit"),
        "row_uuid_singleton",
        "row_uuid_singleton",
        1));
    Commit(context);
  }
  {
    auto fixture = MakeFixture("update_unique_index", 112000);
    SeedThreeRows(fixture);
    auto context = Begin(fixture, "odf111-update-unique-index");
    auto updated = Update(fixture,
                          context,
                          EqualsPredicate("id", "2"),
                          "unique-index-hit");
    Require(EvidenceContains(updated.evidence, "index_lookup", fixture.id_index_uuid),
            "ODF-111 update unique index lookup evidence missing");
    scenarios.push_back(UpdateScenario("update_unique_index_lookup",
                                       std::move(updated),
                                       "indexed_predicate",
                                       "unique_index_lookup",
                                       1,
                                       {"index_lookup"}));
    Commit(context);
  }
  {
    auto fixture = MakeFixture("update_range_index", 113000);
    SeedThreeRows(fixture);
    auto context = Begin(fixture, "odf111-update-range-index");
    auto updated = Update(fixture,
                          context,
                          RangePredicate("name", "amy", "cyd"),
                          "range-index-hit");
    Require(EvidenceContains(updated.evidence, "index_lookup", fixture.name_index_uuid),
            "ODF-111 update range index lookup evidence missing");
    scenarios.push_back(UpdateScenario("update_range_index_lookup",
                                       std::move(updated),
                                       "indexed_predicate",
                                       "range_index_lookup",
                                       3,
                                       {"index_lookup"}));
    Commit(context);
  }
  {
    auto fixture = MakeFixture("update_unindexed", 114000);
    SeedThreeRows(fixture);
    auto context = Begin(fixture, "odf111-update-unindexed");
    const auto updated =
        Update(fixture, context, EqualsPredicate("tag", "red"), "table-scan-hit");
    RequireOk(updated, "ODF-111 unindexed update fallback failed");
    Require(updated.updated_count == 2, "ODF-111 unindexed update count mismatch");
    scenarios.push_back(TableScanFallbackScenario(
        "update_unindexed_table_scan_refused_for_benchmark",
        "update",
        updated.updated_count,
        updated.evidence,
        "update_row_candidate_stream",
        "update_target_access_fallback",
        "unindexed predicate"));
    Commit(context);
  }

  {
    auto fixture = MakeFixture("delete_row_uuid", 115000);
    const auto row_uuid = SeedRow(fixture, "1", "amy", "seed");
    SeedRow(fixture, "2", "bob", "seed");
    auto context = Begin(fixture, "odf111-delete-row-uuid");
    scenarios.push_back(DeleteScenario(
        "delete_row_uuid_singleton",
        Delete(fixture, context, RowUuidPredicate(row_uuid)),
        "row_uuid_singleton",
        "row_uuid_singleton",
        1));
    Commit(context);
  }
  {
    auto fixture = MakeFixture("delete_unique_index", 116000);
    SeedThreeRows(fixture);
    auto context = Begin(fixture, "odf111-delete-unique-index");
    auto deleted = Delete(fixture, context, EqualsPredicate("id", "2"));
    Require(EvidenceContains(deleted.evidence, "index_lookup", fixture.id_index_uuid),
            "ODF-111 delete unique index lookup evidence missing");
    scenarios.push_back(DeleteScenario("delete_unique_index_lookup",
                                       std::move(deleted),
                                       "indexed_predicate",
                                       "unique_index_lookup",
                                       1,
                                       {"index_lookup"}));
    Commit(context);
  }
  {
    auto fixture = MakeFixture("delete_range_index", 117000);
    SeedThreeRows(fixture);
    auto context = Begin(fixture, "odf111-delete-range-index");
    auto deleted =
        Delete(fixture, context, RangePredicate("name", "amy", "cyd"));
    Require(EvidenceContains(deleted.evidence, "index_lookup", fixture.name_index_uuid),
            "ODF-111 delete range index lookup evidence missing");
    scenarios.push_back(DeleteScenario("delete_range_index_lookup",
                                       std::move(deleted),
                                       "indexed_predicate",
                                       "range_index_lookup",
                                       3,
                                       {"index_lookup"}));
    Commit(context);
  }
  {
    auto fixture = MakeFixture("delete_unindexed", 118000);
    SeedThreeRows(fixture);
    auto context = Begin(fixture, "odf111-delete-unindexed");
    const auto deleted = Delete(fixture, context, EqualsPredicate("tag", "red"));
    RequireOk(deleted, "ODF-111 unindexed delete fallback failed");
    Require(deleted.deleted_count == 2, "ODF-111 unindexed delete count mismatch");
    scenarios.push_back(TableScanFallbackScenario(
        "delete_unindexed_table_scan_refused_for_benchmark",
        "delete",
        deleted.deleted_count,
        deleted.evidence,
        "delete_row_candidate_stream",
        "delete_target_access_fallback",
        "unindexed predicate"));
    Commit(context);
  }

  return scenarios;
}

std::vector<ScenarioEvidence> RunUniquePreflightScenarios() {
  std::vector<ScenarioEvidence> scenarios;

  {
    auto fixture = MakeFixture("duplicate_insert", 119000);
    SeedRow(fixture, "1", "amy", "seed");
    auto context = Begin(fixture, "odf111-duplicate-insert");
    const auto duplicate =
        InsertRows(fixture, context, {Row("1", "ada", "duplicate")});
    Require(!duplicate.ok, "ODF-111 duplicate insert was accepted");
    Require(IsDuplicateKeyDetail(FirstDetail(duplicate)),
            "ODF-111 duplicate insert diagnostic drifted");
    Require(HasEvidence(duplicate.evidence,
                        "insert_unique_preflight_path",
                        "index_backed"),
            "ODF-111 duplicate insert missing index-backed preflight evidence");
    Require(HasEvidence(duplicate.evidence,
                        "insert_unique_delta_overlay",
                        "statement"),
            "ODF-111 duplicate insert missing statement overlay evidence");
    Require(HasEvidence(duplicate.evidence,
                        "insert_unique_probe_candidate_source",
                        "persisted_unique_index"),
            "ODF-111 duplicate insert missing persisted index evidence");
    Require(!AnyEvidenceContains(duplicate.evidence, "visible_row_scan"),
            "ODF-111 duplicate insert used visible-row scan fallback");
    Require(!EvidenceContains(duplicate.evidence,
                              "constraint_key_support",
                              fixture.id_index_uuid),
            "ODF-111 duplicate insert fell back to descriptor validation");
    RequireNoForbiddenEvidence(duplicate.evidence, "unique_duplicate_insert");
    ScenarioEvidence scenario;
    scenario.name = "unique_duplicate_insert_index_preflight";
    scenario.operation = "insert";
    scenario.selected_stream = "unique_index_preflight";
    scenario.access_kind = "persisted_unique_index";
    scenario.actual_count = 0;
    scenario.benchmark_clean = "admitted_refusal";
    scenario.proofs = {"index_backed_unique_preflight",
                       "statement_delta_overlay",
                       "persisted_unique_index",
                       "no_visible_row_scan_fallback"};
    FinalizeScenario(&scenario);
    scenarios.push_back(std::move(scenario));
    Commit(context);
  }

  {
    auto fixture = MakeFixture("on_conflict_do_nothing", 120000);
    SeedRow(fixture, "1", "amy", "seed");
    auto context = Begin(fixture, "odf111-do-nothing");
    const auto skipped =
        InsertRows(fixture, context, {Row("1", "ignored", "duplicate")}, "do_nothing");
    RequireOk(skipped, "ODF-111 ON CONFLICT DO NOTHING failed");
    Require(skipped.skipped_count == 1 && skipped.inserted_count == 0,
            "ODF-111 ON CONFLICT DO NOTHING count mismatch");
    Require(HasEvidence(skipped.evidence,
                        "on_conflict_probe_path",
                        "unique_index_lookup"),
            "ODF-111 ON CONFLICT DO NOTHING missing probe evidence");
    Require(HasEvidence(skipped.evidence,
                        "on_conflict_match_source",
                        "persisted_unique_index"),
            "ODF-111 ON CONFLICT DO NOTHING missing persisted index evidence");
    RequireNoForbiddenEvidence(skipped.evidence, "on_conflict_do_nothing");
    ScenarioEvidence scenario;
    scenario.name = "on_conflict_do_nothing_unique_index";
    scenario.operation = "insert";
    scenario.selected_stream = "unique_index_lookup";
    scenario.access_kind = "persisted_unique_index";
    scenario.actual_count = skipped.skipped_count;
    scenario.benchmark_clean = "admitted";
    scenario.proofs = {"on_conflict_probe_path",
                       "persisted_unique_index",
                       "exact_skip_count"};
    FinalizeScenario(&scenario);
    scenarios.push_back(std::move(scenario));
    Commit(context);
  }

  {
    auto fixture = MakeFixture("on_conflict_do_update", 121000);
    SeedRow(fixture, "1", "amy", "old");
    auto context = Begin(fixture, "odf111-do-update");
    const auto updated = InsertRows(fixture,
                                    context,
                                    {Row("1", "grace", "new")},
                                    "do_update",
                                    {"name", "note"});
    RequireOk(updated, "ODF-111 ON CONFLICT DO UPDATE failed");
    Require(updated.updated_count == 1 && updated.inserted_count == 0,
            "ODF-111 ON CONFLICT DO UPDATE count mismatch");
    Require(FieldValue(updated, "name") == "grace" &&
                FieldValue(updated, "note") == "new",
            "ODF-111 ON CONFLICT DO UPDATE result values drifted");
    Require(HasEvidence(updated.evidence,
                        "on_conflict_probe_path",
                        "unique_index_lookup"),
            "ODF-111 ON CONFLICT DO UPDATE missing probe evidence");
    Require(HasEvidence(updated.evidence,
                        "on_conflict_update_path",
                        "persisted_unique_index"),
            "ODF-111 ON CONFLICT DO UPDATE missing persisted update evidence");
    RequireNoForbiddenEvidence(updated.evidence, "on_conflict_do_update");
    ScenarioEvidence scenario;
    scenario.name = "on_conflict_do_update_unique_index";
    scenario.operation = "insert";
    scenario.selected_stream = "unique_index_lookup";
    scenario.access_kind = "persisted_unique_index";
    scenario.actual_count = updated.updated_count;
    scenario.benchmark_clean = "admitted";
    scenario.proofs = {"on_conflict_probe_path",
                       "on_conflict_update_path",
                       "exact_update_count",
                       "exact_return_values"};
    FinalizeScenario(&scenario);
    scenarios.push_back(std::move(scenario));
    Commit(context);
  }

  return scenarios;
}

std::vector<std::string> HotShapeDisabledOptions() {
  return {"runtime.hot_update_shape=disabled"};
}

std::vector<ScenarioEvidence> RunHotUpdateScenarios() {
  std::vector<ScenarioEvidence> scenarios;

  {
    auto fixture = MakeFixture("hot_unchanged_key", 122000);
    SeedRow(fixture, "id-hot", "alpha", "note0");
    auto observer = Begin(fixture, "odf111-hot-unchanged-observer");
    const auto before_index_entries =
        CountVisibleIndexEntries(fixture, observer, fixture.name_index_uuid);
    auto writer = Begin(fixture, "odf111-hot-unchanged-writer");
    const auto updated =
        UpdateNameNote(fixture, writer, "id-hot", "alpha", "note1");
    RequireOk(updated, "ODF-111 hot unchanged-key update failed");
    Require(updated.updated_count == 1,
            "ODF-111 hot unchanged-key update count mismatch");
    Require(HasEvidence(updated.evidence,
                        "DPC_HOT_UPDATE_SHAPE",
                        "version_chain_index_discipline"),
            "ODF-111 hot unchanged-key evidence anchor missing");
    Require(EvidenceCounter(updated.evidence,
                            "dpc_hot_update_shape_index_churn_avoided") >= 1,
            "ODF-111 hot unchanged-key did not avoid index churn");
    Require(EvidenceCounter(
                updated.evidence,
                "dpc_hot_update_shape_synchronous_unchanged_key_skipped") >= 1,
            "ODF-111 hot unchanged-key did not skip synchronous unchanged key");
    Require(EvidenceCounter(
                updated.evidence,
                "dpc_hot_update_shape_synchronous_changed_key_maintained") == 0,
            "ODF-111 hot unchanged-key reported changed-key maintenance");
    Require(CountVisibleIndexEntries(fixture, writer, fixture.name_index_uuid) ==
                before_index_entries,
            "ODF-111 hot unchanged-key appended an unchanged-key index entry");
    RequireNoForbiddenEvidence(updated.evidence, "hot_update_unchanged_key");
    ScenarioEvidence scenario;
    scenario.name = "hot_update_unchanged_key_churn_avoided";
    scenario.operation = "update";
    scenario.selected_stream = "indexed_predicate";
    scenario.access_kind = "unique_index_lookup";
    scenario.actual_count = updated.updated_count;
    scenario.benchmark_clean = "admitted";
    scenario.proofs = {"DPC_HOT_UPDATE_SHAPE",
                       "dpc_hot_update_shape_index_churn_avoided",
                       "dpc_hot_update_shape_synchronous_unchanged_key_skipped"};
    FinalizeScenario(&scenario);
    scenarios.push_back(std::move(scenario));
    Commit(writer);
    Rollback(observer);
  }

  {
    auto fixture = MakeFixture("hot_disabled_baseline", 123000);
    SeedRow(fixture, "id-disabled", "alpha", "note0");
    auto observer = Begin(fixture, "odf111-disabled-observer");
    const auto before_index_entries =
        CountVisibleIndexEntries(fixture, observer, fixture.name_index_uuid);
    auto writer = Begin(fixture, "odf111-disabled-writer");
    const auto updated = UpdateNameNote(fixture,
                                        writer,
                                        "id-disabled",
                                        "alpha",
                                        "note-disabled",
                                        HotShapeDisabledOptions());
    RequireOk(updated, "ODF-111 disabled hot-update baseline failed");
    Require(EvidenceCounter(
                updated.evidence,
                "dpc_hot_update_shape_disabled_baseline_churn_decisions") >= 1,
            "ODF-111 disabled baseline did not report churn");
    Require(EvidenceCounter(updated.evidence,
                            "dpc_hot_update_shape_index_churn_avoided") == 0,
            "ODF-111 disabled baseline reported avoided churn");
    Require(CountVisibleIndexEntries(fixture, writer, fixture.name_index_uuid) ==
                before_index_entries + 1,
            "ODF-111 disabled baseline did not append unchanged-key index entry");
    RequireNoForbiddenEvidence(updated.evidence, "hot_update_disabled_baseline");
    ScenarioEvidence scenario;
    scenario.name = "hot_update_disabled_baseline_reports_churn";
    scenario.operation = "update";
    scenario.selected_stream = "indexed_predicate";
    scenario.access_kind = "unique_index_lookup";
    scenario.actual_count = updated.updated_count;
    scenario.benchmark_clean = "baseline_only";
    scenario.proofs = {"dpc_hot_update_shape_disabled_baseline_churn_decisions",
                       "index_churn_not_avoided_when_disabled"};
    FinalizeScenario(&scenario);
    scenarios.push_back(std::move(scenario));
    Rollback(writer);
    Rollback(observer);
  }

  {
    auto fixture = MakeFixture("hot_changed_key", 124000);
    SeedRow(fixture, "id-changed", "alpha", "note0");
    auto writer = Begin(fixture, "odf111-changed-key-writer");
    const auto updated =
        UpdateNameNote(fixture, writer, "id-changed", "bravo", "note1");
    RequireOk(updated, "ODF-111 changed-key update failed");
    Require(EvidenceCounter(
                updated.evidence,
                "dpc_hot_update_shape_synchronous_changed_key_maintained") >= 1,
            "ODF-111 changed-key update did not report maintenance");
    RequireNoForbiddenEvidence(updated.evidence, "hot_update_changed_key");
    ScenarioEvidence scenario;
    scenario.name = "hot_update_changed_key_maintained";
    scenario.operation = "update";
    scenario.selected_stream = "indexed_predicate";
    scenario.access_kind = "unique_index_lookup";
    scenario.actual_count = updated.updated_count;
    scenario.benchmark_clean = "admitted";
    scenario.proofs = {"dpc_hot_update_shape_synchronous_changed_key_maintained"};
    FinalizeScenario(&scenario);
    scenarios.push_back(std::move(scenario));
    Commit(writer);
  }

  return scenarios;
}

void WriteJsonEvidence(const std::vector<ScenarioEvidence>& scenarios) {
  const std::filesystem::path output_path = ODF111_OUTPUT_JSON;
  if (output_path.has_parent_path()) {
    std::filesystem::create_directories(output_path.parent_path());
  }
  std::ofstream out(output_path);
  Require(out.good(), "ODF-111 could not open JSON evidence output");

  out << "{\n";
  out << "  \"gate\":\"optimizer_deficiency_odf_111_gate\",\n";
  out << "  \"status\":\"passed\",\n";
  out << "  \"runtime_doc_dependency\":\"forbidden_paths_not_read\",\n";
  out << "  \"benchmark_clean_policy\":"
      << Quote("table_scan_fallbacks_marked_refused_for_optimized_timing")
      << ",\n";
  out << "  \"scenario_count\":" << scenarios.size() << ",\n";
  out << "  \"scenarios\":[\n";
  for (std::size_t i = 0; i < scenarios.size(); ++i) {
    const auto& scenario = scenarios[i];
    out << "    {\n";
    out << "      \"name\":" << Quote(scenario.name) << ",\n";
    out << "      \"operation\":" << Quote(scenario.operation) << ",\n";
    out << "      \"selected_stream\":" << Quote(scenario.selected_stream) << ",\n";
    out << "      \"access_kind\":" << Quote(scenario.access_kind) << ",\n";
    out << "      \"actual_count\":" << scenario.actual_count << ",\n";
    out << "      \"benchmark_clean\":" << Quote(scenario.benchmark_clean) << ",\n";
    out << "      \"fallback_reason\":" << Quote(scenario.fallback_reason) << ",\n";
    out << "      \"result_hash\":" << Quote(scenario.result_hash) << ",\n";
    out << "      \"proofs\":[";
    for (std::size_t j = 0; j < scenario.proofs.size(); ++j) {
      out << (j == 0 ? "" : ",") << Quote(scenario.proofs[j]);
    }
    out << "]\n";
    out << "    }" << (i + 1 == scenarios.size() ? "\n" : ",\n");
  }
  out << "  ]\n";
  out << "}\n";
  out.close();
  Require(out.good(), "ODF-111 JSON evidence write failed");
}

}  // namespace

int main() {
  std::vector<ScenarioEvidence> scenarios;
  auto update_delete = RunUpdateDeleteScenarios();
  scenarios.insert(scenarios.end(),
                   std::make_move_iterator(update_delete.begin()),
                   std::make_move_iterator(update_delete.end()));
  auto unique_preflight = RunUniquePreflightScenarios();
  scenarios.insert(scenarios.end(),
                   std::make_move_iterator(unique_preflight.begin()),
                   std::make_move_iterator(unique_preflight.end()));
  auto hot_update = RunHotUpdateScenarios();
  scenarios.insert(scenarios.end(),
                   std::make_move_iterator(hot_update.begin()),
                   std::make_move_iterator(hot_update.end()));

  Require(scenarios.size() == 14, "ODF-111 scenario count drifted");
  WriteJsonEvidence(scenarios);
  std::cout << "optimizer_deficiency_odf_111_gate=passed\n";
  return EXIT_SUCCESS;
}
