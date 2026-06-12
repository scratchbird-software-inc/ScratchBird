// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// ODF-112 bulk ingest benchmark closure gate.

#include "database_lifecycle.hpp"
#include "dml/delete_api.hpp"
#include "dml/import_execution_api.hpp"
#include "dml/native_bulk_ingest_api.hpp"
#include "dml/select_api.hpp"
#include "mga_relation_store/mga_relation_store.hpp"
#include "sblr_dispatch.hpp"
#include "sblr_engine_envelope.hpp"
#include "sblr_opcode_registry.hpp"
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
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace api = scratchbird::engine::internal_api;
namespace db = scratchbird::storage::database;
namespace platform = scratchbird::core::platform;
namespace sblr = scratchbird::engine::sblr;
namespace uuid = scratchbird::core::uuid;

#ifndef ODF112_OUTPUT_JSON
#define ODF112_OUTPUT_JSON "optimizer_deficiency_odf_112_gate.json"
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

platform::u64 UniqueSeed() {
  static platform::u64 counter = 0;
  return NowMillis() + (++counter * 1000);
}

platform::TypedUuid NewUuid(platform::UuidKind kind, platform::u64 salt) {
  const auto generated = uuid::GenerateEngineIdentityV7(kind, UniqueSeed() + salt);
  Require(generated.ok(), "ODF-112 generated UUID creation failed");
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
  typed.is_null = typed.encoded_value == "<NULL>";
  return typed;
}

api::EngineRowValue SimpleRow(std::string id, std::string note) {
  api::EngineRowValue row;
  row.fields.push_back({"id", TextValue(std::move(id))});
  row.fields.push_back({"note", TextValue(std::move(note))});
  return row;
}

api::EngineRowValue SortedRow(std::string id, std::string city) {
  api::EngineRowValue row;
  row.fields.push_back({"id", TextValue(std::move(id))});
  row.fields.push_back({"city", TextValue(std::move(city))});
  return row;
}

api::EngineRowValue ProofRow(std::string id,
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
    if (item.evidence_kind == kind && item.evidence_id == id) {
      return true;
    }
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

std::string EvidenceValue(const std::vector<api::EngineEvidenceReference>& evidence,
                          std::string_view kind) {
  for (const auto& item : evidence) {
    if (item.evidence_kind == kind) {
      return item.evidence_id;
    }
  }
  return {};
}

std::uint64_t EvidenceU64(const std::vector<api::EngineEvidenceReference>& evidence,
                          std::string_view kind) {
  const std::string value = EvidenceValue(evidence, kind);
  Require(!value.empty(), "ODF-112 expected numeric evidence missing");
  std::uint64_t parsed = 0;
  for (const unsigned char ch : value) {
    Require(ch >= '0' && ch <= '9',
            "ODF-112 expected numeric evidence was not numeric");
    parsed = parsed * 10 + static_cast<std::uint64_t>(ch - '0');
  }
  return parsed;
}

std::string FieldValue(const api::EngineResultShape& result,
                       std::size_t row_index,
                       std::string_view field_name) {
  Require(row_index < result.rows.size(), "ODF-112 result row index out of range");
  for (const auto& [name, value] : result.rows[row_index].fields) {
    if (name == field_name) {
      return value.encoded_value;
    }
  }
  return {};
}

void RequireNoForbiddenEvidence(
    const std::vector<api::EngineEvidenceReference>& evidence,
    std::string_view scenario) {
  for (const auto& item : evidence) {
    for (const auto token : {"docs" "/execution-plans",
                             "docs" "/findings",
                             "public_release_evidence",
                             "docs/reference",
                             "execution_plan",
                             "findings",
                             "contracts",
                             "references"}) {
      if (item.evidence_kind.find(token) != std::string::npos ||
          item.evidence_id.find(token) != std::string::npos) {
        std::cerr << "Forbidden runtime evidence token in " << scenario << ": "
                  << item.evidence_kind << '=' << item.evidence_id << '\n';
        Fail("ODF-112 runtime evidence leaked documentation dependency");
      }
    }
  }
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
  std::string route;
  std::uint64_t accepted_rows = 0;
  std::uint64_t inserted_rows = 0;
  std::uint64_t rejected_rows = 0;
  std::uint64_t visible_rows = 0;
  bool ok = true;
  bool benchmark_clean = true;
  bool live_speed_numbers = false;
  std::vector<std::pair<std::string, std::string>> proofs;
  std::string result_hash;
};

std::string ScenarioHashSeed(const ScenarioEvidence& scenario) {
  std::ostringstream seed;
  seed << scenario.name << '|' << scenario.operation << '|' << scenario.route
       << '|' << scenario.accepted_rows << '|' << scenario.inserted_rows
       << '|' << scenario.rejected_rows << '|' << scenario.visible_rows << '|'
       << (scenario.ok ? "true" : "false") << '|'
       << (scenario.benchmark_clean ? "true" : "false") << '|'
       << (scenario.live_speed_numbers ? "true" : "false");
  for (const auto& proof : scenario.proofs) {
    seed << '|' << proof.first << '=' << proof.second;
  }
  return seed.str();
}

void FinalizeScenario(ScenarioEvidence* scenario) {
  scenario->result_hash = StableHash(ScenarioHashSeed(*scenario));
}

void AddProof(ScenarioEvidence* scenario, std::string key, std::string value) {
  scenario->proofs.emplace_back(std::move(key), std::move(value));
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
    if (!dir.empty()) {
      std::filesystem::remove_all(dir, ignored);
    }
  }
};

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
  context.catalog_generation_id = 112;
  context.security_epoch = 113;
  context.resource_epoch = 114;
  context.name_resolution_epoch = 115;
  context.trace_tags = {"optimizer_deficiency_odf_112_gate",
                        "benchmark_clean",
                        "mga_transaction_regression"};
  return context;
}

api::EngineRequestContext Begin(const Fixture& fixture, std::string request_id) {
  api::EngineBeginTransactionRequest request;
  request.context = BaseContext(fixture, std::move(request_id));
  request.isolation_level = "read_committed";
  const auto begun = api::EngineBeginTransaction(request);
  RequireOk(begun, "ODF-112 begin transaction failed");
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
  RequireOk(api::EngineCommitTransaction(request), "ODF-112 commit failed");
}

void Rollback(const api::EngineRequestContext& context) {
  api::EngineRollbackTransactionRequest request;
  request.context = context;
  RequireOk(api::EngineRollbackTransaction(request), "ODF-112 rollback failed");
}

api::CrudIndexRecord IdIndex(const Fixture& fixture,
                             const api::EngineRequestContext& context,
                             bool unique = true) {
  api::CrudIndexRecord index;
  index.creator_tx = context.local_transaction_id;
  index.index_uuid = fixture.id_index_uuid;
  index.table_uuid = fixture.table_uuid;
  index.column_name = "id";
  index.family = api::kCrudIndexFamilyBtree;
  index.profile = api::kCrudIndexProfileRowStoreScalarBtreeV1;
  index.unique = unique;
  index.key_envelopes.push_back("id");
  if (unique) {
    index.key_envelopes.push_back("unique");
  }
  return index;
}

api::CrudTableRecord SimpleTable(const Fixture& fixture,
                                 const api::EngineRequestContext& context) {
  api::CrudTableRecord table;
  table.creator_tx = context.local_transaction_id;
  table.table_uuid = fixture.table_uuid;
  table.default_name = "odf112_bulk_ingest";
  table.columns.push_back({"id", "canonical=character;primary_key=true"});
  table.columns.push_back({"note", "canonical=character;not_null=true"});
  return table;
}

api::CrudTableRecord SortedTable(const Fixture& fixture,
                                 const api::EngineRequestContext& context) {
  api::CrudTableRecord table;
  table.creator_tx = context.local_transaction_id;
  table.table_uuid = fixture.table_uuid;
  table.default_name = "odf112_sorted_bulk";
  table.columns.push_back({"id", "canonical=character;not_null=true"});
  table.columns.push_back({"city", "canonical=character;not_null=true"});
  return table;
}

api::CrudTableRecord ProofTable(const Fixture& fixture,
                                const api::EngineRequestContext& context) {
  api::CrudTableRecord table;
  table.creator_tx = context.local_transaction_id;
  table.table_uuid = fixture.table_uuid;
  table.default_name = "odf112_bulk_constraint_proof";
  table.columns.push_back({"id", "canonical=character;primary_key=true"});
  table.columns.push_back({"parent_id",
                           "canonical=character;foreign_key=" +
                               fixture.table_uuid + ":id"});
  table.columns.push_back({"note", "canonical=character;not_null=true"});
  return table;
}

enum class FixtureKind {
  simple,
  sorted,
  proof,
};

Fixture MakeFixture(std::string name, platform::u64 salt, FixtureKind kind) {
  Fixture fixture;
  fixture.salt = salt;
  fixture.dir = std::filesystem::temp_directory_path() /
                ("scratchbird_odf112_" + name + "_" +
                 std::to_string(UniqueSeed()));
  std::filesystem::create_directories(fixture.dir);
  fixture.database_path = fixture.dir / "odf112.sbdb";

  db::DatabaseCreateConfig create;
  create.path = fixture.database_path.string();
  create.database_uuid = NewUuid(platform::UuidKind::database, salt + 1);
  create.filespace_uuid = NewUuid(platform::UuidKind::filespace, salt + 2);
  create.creation_unix_epoch_millis = UniqueSeed();
  create.require_resource_seed_pack = false;
  create.allow_minimal_resource_bootstrap = true;
  create.allow_overwrite = true;
  const auto created = db::CreateDatabaseFile(create);
  if (!created.ok()) {
    std::cerr << created.diagnostic.diagnostic_code << ':'
              << created.diagnostic.message_key << '\n';
  }
  Require(created.ok(), "ODF-112 database create failed");

  fixture.database_uuid = uuid::UuidToString(create.database_uuid.value);
  fixture.table_uuid = NewUuidText(platform::UuidKind::object, salt + 10);
  fixture.id_index_uuid = NewUuidText(platform::UuidKind::object, salt + 11);

  auto metadata = Begin(fixture, "odf112-metadata");
  switch (kind) {
    case FixtureKind::simple:
      RequireDiagnosticOk(api::AppendMgaTableMetadata(metadata,
                                                      SimpleTable(fixture, metadata)),
                          "ODF-112 simple table metadata append failed");
      RequireDiagnosticOk(api::AppendMgaIndexMetadata(metadata,
                                                      IdIndex(fixture, metadata)),
                          "ODF-112 simple index metadata append failed");
      break;
    case FixtureKind::sorted:
      RequireDiagnosticOk(api::AppendMgaTableMetadata(metadata,
                                                      SortedTable(fixture, metadata)),
                          "ODF-112 sorted table metadata append failed");
      RequireDiagnosticOk(api::AppendMgaIndexMetadata(metadata,
                                                      IdIndex(fixture, metadata)),
                          "ODF-112 sorted index metadata append failed");
      break;
    case FixtureKind::proof:
      RequireDiagnosticOk(api::AppendMgaTableMetadata(metadata,
                                                      ProofTable(fixture, metadata)),
                          "ODF-112 proof table metadata append failed");
      RequireDiagnosticOk(api::AppendMgaIndexMetadata(metadata,
                                                      IdIndex(fixture, metadata)),
                          "ODF-112 proof index metadata append failed");
      break;
  }
  Commit(metadata);
  return fixture;
}

std::vector<api::EngineRowValue> SimpleRows(std::string prefix, int count) {
  std::vector<api::EngineRowValue> rows;
  rows.reserve(static_cast<std::size_t>(count));
  for (int index = 0; index < count; ++index) {
    rows.push_back(SimpleRow(prefix + "-id-" + std::to_string(index + 1),
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
    std::string reject_mode,
    std::vector<std::string> options = {}) {
  api::EngineExecuteImportRowsRequest request;
  request.context = context;
  request.target_table.uuid.canonical = fixture.table_uuid;
  request.target_table.object_kind = "table";
  request.source.source_kind = "csv_stream";
  request.source.source_position = "row:0";
  request.format.format_family = "csv";
  const bool reject_row_mode = reject_mode == "reject_row";
  request.import_policy.reject_mode = std::move(reject_mode);
  request.import_policy.reject_payload_policy = "diagnostic_only";
  request.import_policy.resume_policy = "fail_closed";
  if (reject_row_mode) {
    request.import_policy.reject_limit_rows = 10;
  }
  request.checkpoint_policy.checkpoint_mode = "disabled";
  request.canonical_rows = std::move(rows);
  request.estimated_row_count =
      static_cast<api::EngineApiU64>(request.canonical_rows.size());
  request.option_envelopes = std::move(options);
  if (std::find(request.option_envelopes.begin(),
                request.option_envelopes.end(),
                "copy_append_batching=enabled") ==
      request.option_envelopes.end()) {
    request.option_envelopes.push_back("copy_append_batching=enabled");
  }
  return request;
}

api::EngineExecuteImportRowsRequest StrictImportRequest(
    const Fixture& fixture,
    const api::EngineRequestContext& context,
    std::vector<api::EngineRowValue> rows,
    std::vector<std::string> options = {}) {
  auto request = ImportRequest(fixture,
                               context,
                               std::move(rows),
                               "fail_fast",
                               std::move(options));
  request.import_policy.strict_bulk_load_requested = true;
  if (std::find(request.option_envelopes.begin(),
                request.option_envelopes.end(),
                "feature.strict_bulk_load=enabled") ==
      request.option_envelopes.end()) {
    request.option_envelopes.push_back("feature.strict_bulk_load=enabled");
  }
  return request;
}

api::EngineExecuteNativeBulkIngestRequest NativeRequest(
    const Fixture& fixture,
    const api::EngineRequestContext& context,
    std::vector<api::EngineRowValue> rows,
    std::vector<std::string> options = {},
    bool strict = false) {
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
  request.import_policy.strict_bulk_load_requested = strict;
  request.option_envelopes = std::move(options);
  if (strict &&
      std::find(request.option_envelopes.begin(),
                request.option_envelopes.end(),
                "feature.strict_bulk_load=enabled") ==
          request.option_envelopes.end()) {
    request.option_envelopes.push_back("feature.strict_bulk_load=enabled");
  }
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
  RequireOk(selected, "ODF-112 select failed");
  return selected.visible_count;
}

sblr::SblrOperand Operand(std::string type, std::string name, std::string value) {
  sblr::SblrOperand operand;
  operand.type = std::move(type);
  operand.name = std::move(name);
  operand.value = std::move(value);
  return operand;
}

sblr::SblrOperationEnvelope NativeEnvelope() {
  auto envelope = sblr::MakeSblrEnvelope("dml.execute_native_bulk_ingest",
                                         "SBLR_DML_EXECUTE_NATIVE_BULK_INGEST",
                                         "ODF-112-NATIVE-BULK-INGEST");
  envelope.parser_package_uuid = NewUuidText(platform::UuidKind::object, 112000);
  envelope.registry_snapshot_uuid = NewUuidText(platform::UuidKind::object, 112001);
  envelope.parser_resolved_names_to_uuids = true;
  envelope.requires_security_context = true;
  envelope.requires_transaction_context = true;
  envelope.requires_cluster_authority = false;
  envelope.result_shape = "engine_api_result";
  envelope.diagnostic_shape = "engine_api_diagnostic_vector";
  envelope.operands.push_back(Operand("text", "target_object_kind", "table"));
  envelope.operands.push_back(Operand("text", "native_bulk_ingest_enabled", "true"));
  return envelope;
}

api::EngineApiRequest SblrApiRequest(const Fixture& fixture,
                                     std::vector<api::EngineRowValue> rows) {
  api::EngineApiRequest request;
  request.target_object.uuid.canonical = fixture.table_uuid;
  request.target_object.object_kind = "table";
  request.rows = std::move(rows);
  request.option_envelopes.push_back("estimated_row_count:" +
                                     std::to_string(request.rows.size()));
  return request;
}

void AssertDirectCounters(const api::EngineApiResult& result,
                          std::string_view expected_rows) {
  Require(result.dml_summary.rows_changed != 0,
          "ODF-112 rows_changed counter missing");
  Require(result.dml_summary.append_calls >= 2,
          "ODF-112 append counter too small");
  Require(result.dml_summary.file_opens >= 2,
          "ODF-112 file-open counter too small");
  Require(result.dml_summary.flushes >= 2,
          "ODF-112 flush counter too small");
  Require(result.dml_summary.page_reservations != 0,
          "ODF-112 page reservation counter missing");
  Require(HasEvidence(result.evidence, "mga_hot_append_row_versions", expected_rows),
          "ODF-112 row hot append evidence missing");
  Require(EvidenceContains(result.evidence, "mga_hot_append_index_entries", ""),
          "ODF-112 index hot append evidence missing");
  Require(!AnyEvidenceContains(result.evidence, "delegated_to_dml.insert_rows"),
          "ODF-112 direct lane delegated to EngineInsertRows");
}

void AssertStrictFinalityEvidence(const api::EngineApiResult& result) {
  Require(HasEvidence(result.evidence, "strict_bulk_load_state", "begun"),
          "ODF-112 strict begun evidence missing");
  Require(HasEvidence(result.evidence, "strict_bulk_load_state", "appending"),
          "ODF-112 strict appending evidence missing");
  Require(HasEvidence(result.evidence,
                      "strict_bulk_load_state",
                      "finalize_evidence_durable"),
          "ODF-112 strict durable finalize evidence missing");
  Require(HasEvidence(result.evidence, "strict_bulk_load_state", "published_visible"),
          "ODF-112 strict published-visible evidence missing");
  Require(HasEvidence(result.evidence,
                      "strict_bulk_load_finalize_evidence_durable",
                      "true"),
          "ODF-112 strict durable finalize fence missing");
  Require(HasEvidence(result.evidence,
                      "strict_bulk_load_physical_publication_succeeded",
                      "row_index_append_flush"),
          "ODF-112 strict physical publication evidence missing");
  Require(HasEvidence(result.evidence,
                      "mga_finality_authority",
                      "engine_transaction_inventory"),
          "ODF-112 MGA finality authority evidence missing");
  Require(HasEvidence(result.evidence, "parser_finality_authority", "false"),
          "ODF-112 parser finality authority evidence missing");
  Require(HasEvidence(result.evidence, "reference_finality_authority", "false"),
          "ODF-112 reference finality authority evidence missing");
  Require(EvidenceIndex(result.evidence,
                        "strict_bulk_load_state",
                        "finalize_evidence_durable") <
              EvidenceIndex(result.evidence, "mga_row_version", "row_insert"),
          "ODF-112 strict finalize evidence did not precede MGA row visibility");
}

void AssertPreallocationEvidence(const api::EngineApiResult& result,
                                 std::string_view expected_rows) {
  Require(HasEvidence(result.evidence, "page_extent_preallocation_requested", "true"),
          "ODF-112 preallocation request evidence missing");
  Require(HasEvidence(result.evidence, "page_extent_preallocation_granted", "true"),
          "ODF-112 preallocation grant evidence missing");
  Require(HasEvidence(result.evidence, "page_extent_preallocation_refused", "false"),
          "ODF-112 preallocation was refused");
  Require(HasEvidence(result.evidence, "row_extent_reservation_count", expected_rows),
          "ODF-112 row extent reservation count missing");
  Require(HasEvidence(result.evidence,
                      "version_extent_reservation_count",
                      expected_rows),
          "ODF-112 version extent reservation count missing");
  Require(HasEvidence(result.evidence,
                      "page_agent_demand_decision",
                      "preallocation_completed"),
          "ODF-112 page agent demand preallocation missing");
  Require(HasEvidence(result.evidence,
                      "filespace_agent_demand_decision",
                      "capacity_window_approved"),
          "ODF-112 filespace capacity approval missing");
  Require(HasEvidence(result.evidence,
                      "row_page_allocation_source",
                      "SB-STORAGE-PAGE-ALLOCATION-PREALLOCATED-POOL-HIT"),
          "ODF-112 row allocation did not use preallocated pool");
  Require(HasEvidence(result.evidence,
                      "index_page_allocation_source",
                      "SB-STORAGE-PAGE-ALLOCATION-PREALLOCATED-POOL-HIT"),
          "ODF-112 index allocation did not use preallocated pool");
  Require(result.dml_summary.preallocation_requests >= 2,
          "ODF-112 preallocation request summary missing");
  Require(result.dml_summary.preallocation_granted_pages != 0,
          "ODF-112 preallocation grant summary missing");
  Require(EvidenceIndex(result.evidence,
                        "page_agent_demand_decision",
                        "preallocation_completed") <
              EvidenceIndex(result.evidence,
                            "mga_hot_append_row_versions",
                            expected_rows),
          "ODF-112 preallocation did not precede row append evidence");
  Require(EvidenceIndex(result.evidence,
                        "page_agent_demand_decision",
                        "preallocation_completed") <
              EvidenceIndex(result.evidence,
                            "strict_bulk_load_published_visible",
                            "true"),
          "ODF-112 preallocation did not precede visible publication");
}

void AssertProofAccepted(const api::EngineApiResult& result,
                         std::string_view route_kind,
                         std::string_view route_id) {
  Require(HasEvidence(result.evidence, route_kind, route_id),
          "ODF-112 accepted proof direct route evidence missing");
  Require(HasEvidence(result.evidence,
                      "bulk_constraint_proof_route_selected",
                      "direct_physical_bulk"),
          "ODF-112 bulk proof route evidence missing");
  Require(HasEvidence(result.evidence, "bulk_unique_proof_shape", "sorted"),
          "ODF-112 bulk unique proof shape missing");
  Require(HasEvidence(result.evidence, "bulk_fk_proof_shape", "hash"),
          "ODF-112 bulk FK proof shape missing");
  Require(HasEvidence(result.evidence,
                      "bulk_constraint_disabling",
                      "false"),
          "ODF-112 bulk constraint disabling evidence missing");
  Require(HasEvidence(result.evidence,
                      "bulk_constraint_proof_finality_authority",
                      "mga_transaction_inventory"),
          "ODF-112 bulk proof finality authority missing");
  Require(HasEvidence(result.evidence,
                      "bulk_constraint_proof_result",
                      "accepted"),
          "ODF-112 bulk proof accepted evidence missing");
  Require(HasEvidence(result.evidence,
                      "bulk_fk_proof_parent_existence",
                      "visible_or_batch_parent_hash_hit"),
          "ODF-112 FK parent existence proof missing");
  Require(HasEvidence(result.evidence,
                      "bulk_unique_proof_sorted_duplicate_runs_absent",
                      "true"),
          "ODF-112 sorted duplicate absence proof missing");
  Require(HasEvidence(result.evidence,
                      "mga_finality_authority",
                      "engine_transaction_inventory"),
          "ODF-112 accepted proof MGA finality authority missing");
  RequireNoForbiddenEvidence(result.evidence, "proof accepted");
}

void AssertNoPhysicalAppendEvidence(const api::EngineApiResult& result) {
  Require(!AnyEvidenceContains(result.evidence, "mga_row_version"),
          "ODF-112 refusal wrote row-version evidence");
  Require(!AnyEvidenceContains(result.evidence, "mga_index_store"),
          "ODF-112 refusal wrote index-store evidence");
  Require(!AnyEvidenceContains(result.evidence, "mga_hot_append_row_versions"),
          "ODF-112 refusal appended physical rows");
  Require(!AnyEvidenceContains(result.evidence, "strict_bulk_load_state"),
          "ODF-112 refusal entered strict publication lifecycle");
}

ScenarioEvidence CopyFailFastDirectPreallocationScenario() {
  auto fixture = MakeFixture("copy_direct_preallocation", 112000, FixtureKind::simple);
  auto context = Begin(fixture, "odf112-copy-direct-preallocation");

  const auto result = api::EngineExecuteImportRows(
      StrictImportRequest(fixture,
                          context,
                          SimpleRows("copy", 6),
                          PreallocationOptions(16, 16)));
  RequireOk(result, "ODF-112 COPY direct preallocation import failed");
  Require(result.accepted_rows == 6 && result.inserted_rows == 6,
          "ODF-112 COPY direct preallocation row counts drifted");
  Require(HasEvidence(result.evidence, "import_execution", "direct_physical"),
          "ODF-112 COPY fail-fast did not select direct physical lane");
  Require(HasEvidence(result.evidence, "import_execution_delegate", "none"),
          "ODF-112 COPY fail-fast delegated");
  Require(HasEvidence(result.evidence,
                      "direct_physical_bulk_operation",
                      "copy_import"),
          "ODF-112 COPY direct operation evidence missing");
  AssertDirectCounters(result, "6");
  AssertStrictFinalityEvidence(result);
  AssertPreallocationEvidence(result, "6");
  RequireNoForbiddenEvidence(result.evidence, "copy direct preallocation");

  ScenarioEvidence scenario;
  scenario.name = "copy_fail_fast_direct_preallocation";
  scenario.operation = result.operation_id;
  scenario.route = "copy_import.direct_physical";
  scenario.accepted_rows = result.accepted_rows;
  scenario.inserted_rows = result.inserted_rows;
  scenario.visible_rows = SelectCount(fixture, context);
  AddProof(&scenario, "import_execution", "direct_physical");
  AddProof(&scenario, "import_execution_delegate", "none");
  AddProof(&scenario, "direct_physical_bulk_operation", "copy_import");
  AddProof(&scenario, "page_reservations",
           std::to_string(result.dml_summary.page_reservations));
  AddProof(&scenario, "preallocation_requests",
           std::to_string(result.dml_summary.preallocation_requests));
  AddProof(&scenario, "preallocation_granted_pages",
           std::to_string(result.dml_summary.preallocation_granted_pages));
  AddProof(&scenario, "strict_bulk_load_state", "published_visible");
  AddProof(&scenario, "mga_finality_authority", "engine_transaction_inventory");
  AddProof(&scenario, "parser_finality_authority", "false");
  AddProof(&scenario, "reference_finality_authority", "false");
  FinalizeScenario(&scenario);
  Rollback(context);
  return scenario;
}

ScenarioEvidence NativeApiDirectLaneScenario() {
  auto fixture = MakeFixture("native_api", 112100, FixtureKind::simple);
  auto context = Begin(fixture, "odf112-native-api");

  const auto result = api::EngineExecuteNativeBulkIngest(
      NativeRequest(fixture, context, SimpleRows("native-api", 4)));
  RequireOk(result, "ODF-112 native API direct ingest failed");
  Require(result.accepted_rows == 4 && result.inserted_rows == 4,
          "ODF-112 native API row counts drifted");
  Require(HasEvidence(result.evidence, "native_bulk_ingest", "enabled"),
          "ODF-112 native enabled evidence missing");
  Require(HasEvidence(result.evidence,
                      "native_bulk_ingest_route",
                      "engine_internal_api"),
          "ODF-112 native API route evidence missing");
  Require(HasEvidence(result.evidence,
                      "native_bulk_ingest_source",
                      "binary_typed_rows"),
          "ODF-112 native binary source evidence missing");
  Require(HasEvidence(result.evidence,
                      "native_bulk_ingest_lane",
                      "direct_physical"),
          "ODF-112 native API did not use direct physical lane");
  Require(HasEvidence(result.evidence, "native_bulk_ingest_delegate", "none"),
          "ODF-112 native API delegated");
  Require(!AnyEvidenceContains(result.evidence, "dml.execute_import_rows"),
          "ODF-112 native API delegated to import execution");
  Require(HasEvidence(result.evidence, "parser_finality_authority", "false"),
          "ODF-112 native parser finality evidence missing");
  Require(HasEvidence(result.evidence, "reference_finality_authority", "false"),
          "ODF-112 native reference finality evidence missing");
  AssertDirectCounters(result, "4");
  RequireNoForbiddenEvidence(result.evidence, "native API");

  ScenarioEvidence scenario;
  scenario.name = "native_binary_api_direct_lane";
  scenario.operation = result.operation_id;
  scenario.route = "native_bulk.engine_internal_api.direct_physical";
  scenario.accepted_rows = result.accepted_rows;
  scenario.inserted_rows = result.inserted_rows;
  scenario.visible_rows = SelectCount(fixture, context);
  AddProof(&scenario, "native_bulk_ingest_route", "engine_internal_api");
  AddProof(&scenario, "native_bulk_ingest_source", "binary_typed_rows");
  AddProof(&scenario, "native_bulk_ingest_lane", "direct_physical");
  AddProof(&scenario, "native_bulk_ingest_delegate", "none");
  AddProof(&scenario, "parser_finality_authority", "false");
  AddProof(&scenario, "reference_finality_authority", "false");
  FinalizeScenario(&scenario);
  Rollback(context);
  return scenario;
}

ScenarioEvidence NativeSblrDispatchScenario() {
  auto fixture = MakeFixture("native_sblr", 112200, FixtureKind::simple);
  auto context = Begin(fixture, "odf112-native-sblr");

  const auto* entry = sblr::LookupSblrOperation("dml.execute_native_bulk_ingest");
  Require(entry != nullptr, "ODF-112 native bulk SBLR registry entry missing");
  Require(entry->opcode == "SBLR_DML_EXECUTE_NATIVE_BULK_INGEST",
          "ODF-112 native bulk SBLR opcode drifted");
  Require(entry->requires_transaction_context,
          "ODF-112 native bulk SBLR operation lost transaction context");
  Require(!entry->requires_cluster_authority && !entry->cluster_private,
          "ODF-112 native bulk SBLR operation drifted into cluster scope");
  auto envelope = NativeEnvelope();
  const auto validated = sblr::ValidateSblrOpcodeForEnvelope(envelope);
  Require(validated.ok && validated.entry == entry,
          "ODF-112 native bulk SBLR envelope validation failed");

  sblr::SblrDispatchRequest dispatch;
  dispatch.context = context;
  dispatch.envelope = std::move(envelope);
  dispatch.api_request = SblrApiRequest(fixture, SimpleRows("native-sblr", 3));
  const auto dispatched = sblr::DispatchSblrOperation(dispatch);
  if (!(dispatched.accepted && dispatched.envelope_validated &&
        dispatched.dispatched_to_api && dispatched.api_result.ok)) {
    if (!dispatched.diagnostics.empty()) {
      std::cerr << dispatched.diagnostics.front().code << ':'
                << dispatched.diagnostics.front().message << '\n';
    }
    if (!dispatched.api_result.diagnostics.empty()) {
      std::cerr << dispatched.api_result.diagnostics.front().code << ':'
                << dispatched.api_result.diagnostics.front().detail << '\n';
    }
    Fail("ODF-112 native bulk SBLR dispatch failed");
  }
  const auto& result = dispatched.api_result;
  Require(result.dml_summary.rows_changed == 3,
          "ODF-112 native SBLR row count summary drifted");
  Require(HasEvidence(result.evidence,
                      "native_bulk_ingest_route",
                      "engine_internal_api"),
          "ODF-112 SBLR native route did not reach API route");
  Require(HasEvidence(result.evidence,
                      "native_bulk_ingest_lane",
                      "direct_physical"),
          "ODF-112 SBLR native route did not use direct lane");
  Require(HasEvidence(result.evidence, "native_bulk_ingest_delegate", "none"),
          "ODF-112 SBLR native route delegated");
  RequireNoForbiddenEvidence(result.evidence, "native SBLR");

  ScenarioEvidence scenario;
  scenario.name = "native_binary_sblr_dispatch_route";
  scenario.operation = "dml.execute_native_bulk_ingest";
  scenario.route = "sblr.dispatch.engine_internal_api.direct_physical";
  scenario.accepted_rows = result.dml_summary.rows_changed;
  scenario.inserted_rows = result.dml_summary.rows_changed;
  scenario.visible_rows = SelectCount(fixture, context);
  AddProof(&scenario, "sblr_registry_opcode", "SBLR_DML_EXECUTE_NATIVE_BULK_INGEST");
  AddProof(&scenario, "sblr_envelope_validated",
           dispatched.envelope_validated ? "true" : "false");
  AddProof(&scenario, "sblr_dispatched_to_api",
           dispatched.dispatched_to_api ? "true" : "false");
  AddProof(&scenario, "native_bulk_ingest_lane", "direct_physical");
  AddProof(&scenario, "native_bulk_ingest_delegate", "none");
  FinalizeScenario(&scenario);
  Rollback(context);
  return scenario;
}

void SeedDuplicateRow(const Fixture& fixture) {
  auto context = Begin(fixture, "odf112-reject-seed");
  api::EngineExecuteImportRowsRequest request =
      ImportRequest(fixture,
                    context,
                    {SimpleRow("duplicate-id", "seed")},
                    "fail_fast");
  const auto result = api::EngineExecuteImportRows(request);
  RequireOk(result, "ODF-112 duplicate seed insert failed");
  Commit(context);
}

ScenarioEvidence RejectBisectionScenario() {
  auto fixture = MakeFixture("reject_bisection", 112300, FixtureKind::simple);
  SeedDuplicateRow(fixture);

  std::vector<api::EngineRowValue> rows;
  rows.push_back(SimpleRow("new-id-1", "accepted-1"));
  rows.push_back(SimpleRow("new-id-2", "accepted-2"));
  rows.push_back(SimpleRow("new-id-3", "accepted-3"));
  rows.push_back(SimpleRow("new-id-4", "accepted-4"));
  rows.push_back(SimpleRow("duplicate-id", "rejected-duplicate"));
  rows.push_back(SimpleRow("new-id-6", "accepted-6"));
  rows.push_back(SimpleRow("new-id-7", "accepted-7"));
  rows.push_back(SimpleRow("new-id-8", "accepted-8"));

  auto context = Begin(fixture, "odf112-reject-bisection");
  const auto result = api::EngineExecuteImportRows(
      ImportRequest(fixture,
                    context,
                    std::move(rows),
                    "reject_row",
                    {"copy_append_batching=enabled", "copy_append_batch_rows=8"}));
  RequireOk(result, "ODF-112 reject-row import failed");
  Require(result.accepted_rows == 7 && result.inserted_rows == 7 &&
              result.rejected_rows == 1,
          "ODF-112 reject-row counts drifted");
  Require(result.result_shape.rows.size() == 8,
          "ODF-112 reject-row result shape drifted");
  Require(FieldValue(result.result_shape, 4, "source_row_number") == "5",
          "ODF-112 reject diagnostic source row changed");
  const std::string duplicate_detail =
      FieldValue(result.result_shape, 4, "diagnostic_detail");
  Require(duplicate_detail.find("duplicate_key") != std::string::npos ||
              duplicate_detail.find("unique_index_duplicate") != std::string::npos,
          "ODF-112 reject diagnostic did not describe duplicate key");
  Require(HasEvidence(result.evidence, "import_row_window_route", "borrowed_span"),
          "ODF-112 reject import did not use borrowed row windows");
  Require(HasEvidence(result.evidence, "import_row_vector_copies", "0"),
          "ODF-112 reject import copied canonical rows");
  Require(HasEvidence(result.evidence, "copy_append_reject_fallback", "bisection"),
          "ODF-112 bisection fallback evidence missing");
  Require(HasEvidence(result.evidence,
                      "copy_append_singleton_fallback_batches",
                      "1"),
          "ODF-112 compatibility terminal singleton evidence missing");
  Require(HasEvidence(result.evidence, "copy_append_bisection_split_count", "3"),
          "ODF-112 bisection split count drifted");
  Require(HasEvidence(result.evidence,
                      "copy_append_bisection_terminal_singleton_count",
                      "1"),
          "ODF-112 bisection terminal singleton count drifted");
  const auto attempts =
      EvidenceU64(result.evidence, "copy_append_bisection_batch_attempt_count");
  const auto windows = EvidenceU64(result.evidence, "import_row_window_count");
  Require(attempts == 7, "ODF-112 bisection attempt count changed");
  Require(windows == attempts,
          "ODF-112 row window count did not match bisection attempts");
  Require(attempts < 9,
          "ODF-112 bisection did not beat full singleton replay attempts");
  RequireNoForbiddenEvidence(result.evidence, "reject bisection");

  ScenarioEvidence scenario;
  scenario.name = "copy_reject_row_bisection_sparse_bad_rows";
  scenario.operation = result.operation_id;
  scenario.route = "copy_append.reject_row.bisection";
  scenario.accepted_rows = result.accepted_rows;
  scenario.inserted_rows = result.inserted_rows;
  scenario.rejected_rows = result.rejected_rows;
  scenario.visible_rows = SelectCount(fixture, context);
  AddProof(&scenario, "copy_append_reject_fallback", "bisection");
  AddProof(&scenario, "copy_append_bisection_split_count", "3");
  AddProof(&scenario,
           "copy_append_bisection_batch_attempt_count",
           std::to_string(attempts));
  AddProof(&scenario, "import_row_window_count", std::to_string(windows));
  AddProof(&scenario, "rejected_source_row_number", "5");
  FinalizeScenario(&scenario);
  Rollback(context);
  return scenario;
}

ScenarioEvidence SortedExactBuildScenario() {
  auto fixture = MakeFixture("sorted_exact", 112400, FixtureKind::sorted);
  auto context = Begin(fixture, "odf112-sorted-exact");

  const auto result = api::EngineExecuteImportRows(
      ImportRequest(fixture,
                    context,
                    {SortedRow("004", "oslo"),
                     SortedRow("001", "zurich"),
                     SortedRow("003", "quito"),
                     SortedRow("002", "berlin")},
                    "fail_fast",
                    {"copy_append_batching=enabled",
                     "sorted_bulk_index_build=enabled"}));
  RequireOk(result, "ODF-112 sorted exact bulk import failed");
  Require(result.inserted_rows == 4,
          "ODF-112 sorted exact row count drifted");
  Require(HasEvidence(result.evidence,
                      "sorted_bulk_index_build_selected",
                      "true"),
          "ODF-112 sorted build selected evidence missing");
  Require(HasEvidence(result.evidence,
                      "sorted_bulk_index_exact_append",
                      "mga_index_append_path"),
          "ODF-112 sorted exact append path evidence missing");
  Require(HasEvidence(result.evidence,
                      "sorted_bulk_index_uniqueness_proof",
                      "sorted_duplicate_runs_absent"),
          "ODF-112 sorted uniqueness proof evidence missing");
  Require(HasEvidence(result.evidence,
                      "mga_finality_authority",
                      "engine_transaction_inventory"),
          "ODF-112 sorted MGA finality authority evidence missing");
  RequireNoForbiddenEvidence(result.evidence, "sorted exact build");

  ScenarioEvidence scenario;
  scenario.name = "empty_table_sorted_exact_unique_index_build";
  scenario.operation = result.operation_id;
  scenario.route = "copy_import.sorted_exact_index_build";
  scenario.accepted_rows = result.accepted_rows;
  scenario.inserted_rows = result.inserted_rows;
  scenario.visible_rows = SelectCount(fixture, context);
  AddProof(&scenario, "sorted_bulk_index_build_selected", "true");
  AddProof(&scenario, "sorted_bulk_index_exact_append", "mga_index_append_path");
  AddProof(&scenario,
           "sorted_bulk_index_uniqueness_proof",
           "sorted_duplicate_runs_absent");
  AddProof(&scenario, "mga_finality_authority", "engine_transaction_inventory");
  FinalizeScenario(&scenario);
  Rollback(context);
  return scenario;
}

ScenarioEvidence ProofValidationAcceptedScenario() {
  auto fixture = MakeFixture("proof_accepted", 112500, FixtureKind::proof);
  auto context = Begin(fixture, "odf112-proof-accepted");

  auto copy_request = StrictImportRequest(
      fixture,
      context,
      {ProofRow("root", "<NULL>", "root row"),
       ProofRow("child", "root", "child row")},
      {"copy_append_batching=enabled", "sorted_bulk_index_build=enabled"});
  const auto imported = api::EngineExecuteImportRows(copy_request);
  RequireOk(imported, "ODF-112 proof COPY import failed");
  Require(imported.accepted_rows == 2 && imported.inserted_rows == 2,
          "ODF-112 proof COPY counts drifted");
  AssertProofAccepted(imported, "import_execution", "direct_physical");

  const auto native = api::EngineExecuteNativeBulkIngest(
      NativeRequest(fixture,
                    context,
                    {ProofRow("native-child", "root", "native row")},
                    {"sorted_bulk_index_build=enabled"},
                    true));
  RequireOk(native, "ODF-112 proof native import failed");
  Require(native.accepted_rows == 1 && native.inserted_rows == 1,
          "ODF-112 proof native counts drifted");
  AssertProofAccepted(native, "native_bulk_ingest_lane", "direct_physical");

  ScenarioEvidence scenario;
  scenario.name = "bulk_unique_fk_proof_accepts_valid_copy_and_native";
  scenario.operation = "dml.bulk_proof_validation";
  scenario.route = "direct_physical_bulk.constraint_proof";
  scenario.accepted_rows = imported.accepted_rows + native.accepted_rows;
  scenario.inserted_rows = imported.inserted_rows + native.inserted_rows;
  scenario.visible_rows = SelectCount(fixture, context);
  AddProof(&scenario, "bulk_constraint_proof_route_selected", "direct_physical_bulk");
  AddProof(&scenario, "bulk_unique_proof_shape", "sorted");
  AddProof(&scenario, "bulk_fk_proof_shape", "hash");
  AddProof(&scenario,
           "bulk_constraint_proof_finality_authority",
           "mga_transaction_inventory");
  AddProof(&scenario, "bulk_constraint_proof_result", "accepted");
  AddProof(&scenario,
           "bulk_fk_proof_parent_existence",
           "visible_or_batch_parent_hash_hit");
  AddProof(&scenario,
           "bulk_unique_proof_sorted_duplicate_runs_absent",
           "true");
  FinalizeScenario(&scenario);
  Rollback(context);
  return scenario;
}

ScenarioEvidence ProofValidationRefusalScenario() {
  auto fixture = MakeFixture("proof_refusal", 112600, FixtureKind::proof);

  auto duplicate_context = Begin(fixture, "odf112-proof-duplicate");
  const auto duplicate = api::EngineExecuteImportRows(
      StrictImportRequest(fixture,
                          duplicate_context,
                          {ProofRow("dup", "<NULL>", "first"),
                           ProofRow("dup", "<NULL>", "second")},
                          {"copy_append_batching=enabled",
                           "sorted_bulk_index_build=enabled"}));
  Require(!duplicate.ok, "ODF-112 duplicate proof batch was accepted");
  Require(!duplicate.diagnostics.empty(),
          "ODF-112 duplicate proof batch lacked diagnostic");
  Require(duplicate.diagnostics.front().code ==
              "SB-BULK-CONSTRAINT-UNIQUE-DUPLICATE-BATCH",
          "ODF-112 duplicate proof diagnostic drifted");
  Require(HasEvidence(duplicate.evidence,
                      "bulk_constraint_proof_conflict_reason",
                      "bulk_unique_proof_duplicate_in_batch"),
          "ODF-112 duplicate proof conflict evidence missing");
  AssertNoPhysicalAppendEvidence(duplicate);
  Require(SelectCount(fixture, duplicate_context) == 0,
          "ODF-112 duplicate proof refusal published rows");
  RequireNoForbiddenEvidence(duplicate.evidence, "proof duplicate refusal");
  Rollback(duplicate_context);

  auto fk_context = Begin(fixture, "odf112-proof-missing-fk");
  const auto missing_fk = api::EngineExecuteImportRows(
      StrictImportRequest(fixture,
                          fk_context,
                          {ProofRow("child", "missing-parent", "orphan")},
                          {"copy_append_batching=enabled",
                           "sorted_bulk_index_build=enabled"}));
  Require(!missing_fk.ok, "ODF-112 missing FK proof batch was accepted");
  Require(!missing_fk.diagnostics.empty(),
          "ODF-112 missing FK proof batch lacked diagnostic");
  Require(missing_fk.diagnostics.front().code ==
              "SB-BULK-CONSTRAINT-FK-PARENT-MISSING",
          "ODF-112 missing FK proof diagnostic drifted");
  Require(HasEvidence(missing_fk.evidence,
                      "bulk_constraint_proof_conflict_reason",
                      "bulk_fk_proof_parent_missing"),
          "ODF-112 missing FK conflict evidence missing");
  Require(HasEvidence(missing_fk.evidence,
                      "bulk_fk_proof_missing_parent_key",
                      "missing-parent"),
          "ODF-112 missing FK parent key evidence missing");
  AssertNoPhysicalAppendEvidence(missing_fk);
  Require(SelectCount(fixture, fk_context) == 0,
          "ODF-112 missing FK refusal published rows");
  RequireNoForbiddenEvidence(missing_fk.evidence, "proof FK refusal");
  Rollback(fk_context);

  ScenarioEvidence scenario;
  scenario.name = "bulk_unique_fk_proof_refuses_before_append";
  scenario.operation = "dml.bulk_proof_validation";
  scenario.route = "direct_physical_bulk.constraint_refusal_before_append";
  scenario.ok = true;
  scenario.accepted_rows = 0;
  scenario.inserted_rows = 0;
  scenario.rejected_rows = 2;
  scenario.visible_rows = 0;
  AddProof(&scenario,
           "duplicate_diagnostic",
           "SB-BULK-CONSTRAINT-UNIQUE-DUPLICATE-BATCH");
  AddProof(&scenario,
           "duplicate_conflict_reason",
           "bulk_unique_proof_duplicate_in_batch");
  AddProof(&scenario,
           "fk_diagnostic",
           "SB-BULK-CONSTRAINT-FK-PARENT-MISSING");
  AddProof(&scenario, "fk_conflict_reason", "bulk_fk_proof_parent_missing");
  AddProof(&scenario, "physical_append_before_refusal", "false");
  FinalizeScenario(&scenario);
  return scenario;
}

void RequireNoForbiddenJsonTokens(const std::string& json) {
  for (const auto token : {"docs" "/execution-plans",
                           "docs" "/findings",
                           "public_release_evidence",
                           "docs/reference",
                           "execution_plan",
                           "findings",
                           "contracts",
                           "references"}) {
    Require(json.find(token) == std::string::npos,
            "ODF-112 JSON leaked documentation dependency token");
  }
}

void WriteJson(const std::vector<ScenarioEvidence>& scenarios) {
  std::ostringstream json;
  json << "{\n";
  json << "  \"gate\": \"optimizer_deficiency_odf_112_gate\",\n";
  json << "  \"odf\": \"ODF-112\",\n";
  json << "  \"benchmark_clean\": true,\n";
  json << "  \"live_speed_numbers\": false,\n";
  json << "  \"scenario_count\": " << scenarios.size() << ",\n";
  json << "  \"scenarios\": [\n";
  for (std::size_t index = 0; index < scenarios.size(); ++index) {
    const auto& scenario = scenarios[index];
    json << "    {\n";
    json << "      \"name\": " << Quote(scenario.name) << ",\n";
    json << "      \"operation\": " << Quote(scenario.operation) << ",\n";
    json << "      \"route\": " << Quote(scenario.route) << ",\n";
    json << "      \"ok\": " << (scenario.ok ? "true" : "false") << ",\n";
    json << "      \"benchmark_clean\": "
         << (scenario.benchmark_clean ? "true" : "false") << ",\n";
    json << "      \"live_speed_numbers\": "
         << (scenario.live_speed_numbers ? "true" : "false") << ",\n";
    json << "      \"accepted_rows\": " << scenario.accepted_rows << ",\n";
    json << "      \"inserted_rows\": " << scenario.inserted_rows << ",\n";
    json << "      \"rejected_rows\": " << scenario.rejected_rows << ",\n";
    json << "      \"visible_rows\": " << scenario.visible_rows << ",\n";
    json << "      \"result_hash\": " << Quote(scenario.result_hash) << ",\n";
    json << "      \"proofs\": [\n";
    for (std::size_t proof_index = 0; proof_index < scenario.proofs.size();
         ++proof_index) {
      const auto& proof = scenario.proofs[proof_index];
      json << "        {\"kind\": " << Quote(proof.first)
           << ", \"value\": " << Quote(proof.second) << "}";
      if (proof_index + 1 != scenario.proofs.size()) {
        json << ',';
      }
      json << '\n';
    }
    json << "      ]\n";
    json << "    }";
    if (index + 1 != scenarios.size()) {
      json << ',';
    }
    json << '\n';
  }
  json << "  ]\n";
  json << "}\n";

  const std::string text = json.str();
  RequireNoForbiddenJsonTokens(text);

  const std::filesystem::path output_path = ODF112_OUTPUT_JSON;
  if (!output_path.parent_path().empty()) {
    std::filesystem::create_directories(output_path.parent_path());
  }
  std::ofstream out(output_path, std::ios::binary | std::ios::trunc);
  Require(static_cast<bool>(out), "ODF-112 could not open JSON output");
  out << text;
  out.flush();
  Require(static_cast<bool>(out), "ODF-112 could not write JSON output");
}

}  // namespace

int main() {
  std::vector<ScenarioEvidence> scenarios;
  scenarios.push_back(CopyFailFastDirectPreallocationScenario());
  scenarios.push_back(NativeApiDirectLaneScenario());
  scenarios.push_back(NativeSblrDispatchScenario());
  scenarios.push_back(RejectBisectionScenario());
  scenarios.push_back(SortedExactBuildScenario());
  scenarios.push_back(ProofValidationAcceptedScenario());
  scenarios.push_back(ProofValidationRefusalScenario());
  WriteJson(scenarios);
  return EXIT_SUCCESS;
}
