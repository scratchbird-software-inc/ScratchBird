// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "database_lifecycle.hpp"
#include "dml/import_execution_api.hpp"
#include "dml/insert_api.hpp"
#include "mga_relation_store/mga_relation_store.hpp"
#include "strict_bulk_load_lifecycle.hpp"
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
namespace bulk = scratchbird::core::bulk_load;
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
                << result.diagnostics.front().detail << '\n';
    }
    Fail(message);
  }
}

platform::u64 MillisSeed() {
  return static_cast<platform::u64>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

platform::TypedUuid NewUuid(platform::UuidKind kind, platform::u64 salt) {
  const auto generated = uuid::GenerateEngineIdentityV7(kind, MillisSeed() + salt);
  Require(generated.ok(), "CDP-013 UUID generation failed");
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
  api::EngineRequestContext context;

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
  return typed;
}

api::EngineRowValue Row(std::string id, std::string note) {
  api::EngineRowValue row;
  row.fields.push_back({"id", TextValue(std::move(id))});
  row.fields.push_back({"note", TextValue(std::move(note))});
  return row;
}

api::EngineRequestContext BaseContext(const Fixture& fixture, std::string request_id) {
  api::EngineRequestContext context;
  context.trust_mode = api::EngineTrustMode::server_isolated;
  context.request_id = std::move(request_id);
  context.database_path = fixture.database_path.string();
  context.database_uuid.canonical = fixture.database_uuid;
  context.principal_uuid.canonical = NewUuidText(platform::UuidKind::principal, fixture.salt + 100);
  context.session_uuid.canonical = NewUuidText(platform::UuidKind::object, fixture.salt + 101);
  context.security_context_present = true;
  context.identifier_profile_uuid = "sbsql_v3";
  context.language_context.language_tag = "en";
  context.language_context.default_language_tag = "en";
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
  RequireOk(begun, "CDP-013 begin transaction failed");
  auto context = request.context;
  context.local_transaction_id = begun.local_transaction_id;
  context.transaction_uuid = begun.transaction_uuid;
  context.snapshot_visible_through_local_transaction_id =
      begun.snapshot_visible_through_local_transaction_id;
  context.transaction_isolation_level = begun.isolation_level;
  return context;
}

api::CrudTableRecord Table(const Fixture& fixture) {
  api::CrudTableRecord table;
  table.creator_tx = fixture.context.local_transaction_id;
  table.table_uuid = fixture.table_uuid;
  table.default_name = "cdp_page_filespace_demand";
  table.columns.push_back({"id", "canonical=character"});
  table.columns.push_back({"note", "canonical=character"});
  return table;
}

api::CrudIndexRecord UniqueIdIndex(const Fixture& fixture) {
  api::CrudIndexRecord index;
  index.creator_tx = fixture.context.local_transaction_id;
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
                ("scratchbird_cdp013_" + name + "_" + std::to_string(MillisSeed() + salt));
  std::filesystem::create_directories(fixture.dir);
  fixture.database_path = fixture.dir / "cdp013.sbdb";

  db::DatabaseCreateConfig create;
  create.path = fixture.database_path.string();
  create.database_uuid = NewUuid(platform::UuidKind::database, salt + 1);
  create.filespace_uuid = NewUuid(platform::UuidKind::filespace, salt + 2);
  create.creation_unix_epoch_millis = MillisSeed() + salt + 3;
  create.require_resource_seed_pack = false;
  create.allow_minimal_resource_bootstrap = true;
  create.allow_overwrite = true;
  const auto created = db::CreateDatabaseFile(create);
  if (!created.ok()) {
    std::cerr << created.diagnostic.diagnostic_code << ':'
              << created.diagnostic.message_key << '\n';
  }
  Require(created.ok(), "CDP-013 database create failed");

  fixture.database_uuid = uuid::UuidToString(create.database_uuid.value);
  fixture.table_uuid = NewUuidText(platform::UuidKind::object, salt + 10);
  fixture.index_uuid = NewUuidText(platform::UuidKind::object, salt + 11);
  fixture.context = Begin(fixture, "cdp013-" + name + "-metadata");

  const auto table = api::AppendMgaTableMetadata(fixture.context, Table(fixture));
  Require(!table.error, "CDP-013 table metadata append failed");
  const auto index = api::AppendMgaIndexMetadata(fixture.context, UniqueIdIndex(fixture));
  Require(!index.error, "CDP-013 index metadata append failed");
  return fixture;
}

std::vector<std::string> DemandOptions(platform::u64 max_pages,
                                       platform::u64 capacity_pages) {
  return {"page_allocation.runtime=enabled",
          "dml_demand_hints=enabled",
          "dml_demand_hints.max_pages=" + std::to_string(max_pages),
          "dml_demand_hints.available_capacity_pages=" + std::to_string(capacity_pages)};
}

api::EngineInsertRowsRequest InsertRequest(Fixture& fixture,
                                           std::string request_id,
                                           std::vector<api::EngineRowValue> rows,
                                           std::vector<std::string> options) {
  api::EngineInsertRowsRequest request;
  request.context = fixture.context;
  request.context.request_id = std::move(request_id);
  request.target_table.uuid.canonical = fixture.table_uuid;
  request.target_table.object_kind = "table";
  request.input_rows = std::move(rows);
  request.estimated_row_count = static_cast<api::EngineApiU64>(request.input_rows.size());
  request.option_envelopes = std::move(options);
  return request;
}

api::EngineExecuteImportRowsRequest ImportRequest(
    Fixture& fixture,
    std::string request_id,
    std::vector<api::EngineRowValue> rows,
    std::vector<std::string> options) {
  api::EngineExecuteImportRowsRequest request;
  request.context = fixture.context;
  request.context.request_id = std::move(request_id);
  request.target_table.uuid.canonical = fixture.table_uuid;
  request.target_table.object_kind = "table";
  request.source.source_kind = "csv_stream";
  request.source.source_position = "row:0";
  request.format.format_family = "csv";
  request.import_policy.reject_mode = "fail_fast";
  request.import_policy.reject_payload_policy = "diagnostic_only";
  request.import_policy.resume_policy = "fail_closed";
  request.canonical_rows = std::move(rows);
  request.estimated_row_count = static_cast<api::EngineApiU64>(request.canonical_rows.size());
  request.option_envelopes = std::move(options);
  return request;
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

std::size_t EvidenceIndex(const std::vector<api::EngineEvidenceReference>& evidence,
                          std::string_view kind,
                          std::string_view id) {
  for (std::size_t index = 0; index < evidence.size(); ++index) {
    if (evidence[index].evidence_kind == kind && evidence[index].evidence_id == id) {
      return index;
    }
  }
  return evidence.size();
}

api::MgaRelationStoreState LoadedState(const api::EngineRequestContext& context) {
  const auto loaded = api::LoadMgaRelationStoreState(context);
  Require(loaded.ok, "CDP-013 MGA relation store load failed");
  return loaded.state;
}

platform::TypedUuid MakeBulkUuid(platform::UuidKind kind, platform::u64 seed) {
  const auto generated = uuid::GenerateEngineIdentityV7(kind, 1900000000000ull + seed);
  Require(generated.ok(), "CDP-013 bulk UUID generation failed");
  return generated.value;
}

bulk::DmlPageFilespaceDemandHintRequest HintRequest(platform::u64 requested_pages,
                                                    platform::u64 max_pages) {
  bulk::DmlPageFilespaceDemandHintRequest request;
  request.database_uuid = MakeBulkUuid(platform::UuidKind::database, 100);
  request.object_uuid = MakeBulkUuid(platform::UuidKind::object, 200);
  request.filespace_uuid = MakeBulkUuid(platform::UuidKind::filespace, 300);
  request.transaction_uuid = MakeBulkUuid(platform::UuidKind::transaction, 400);
  request.local_transaction_id = 77;
  request.batch_sequence = 1;
  request.batch_row_count = 16;
  request.requested_page_count = requested_pages;
  request.max_preallocation_pages = max_pages;
  request.source = "CDP-013";
  return request;
}

void TestDemandHintSurfaceAcceptedCappedDisabledAndRefused() {
  const auto accepted = bulk::MakeDmlPageFilespaceDemandHint(HintRequest(4, 16));
  Require(accepted.ok(), "CDP-013 accepted demand hint was refused");
  Require(accepted.record.decision == bulk::DmlPageFilespaceDemandHintDecision::accepted,
          "CDP-013 accepted demand hint decision mismatch");
  Require(accepted.record.granted_page_count == 4,
          "CDP-013 accepted demand hint grant mismatch");

  const auto capped = bulk::MakeDmlPageFilespaceDemandHint(HintRequest(20, 8));
  Require(capped.ok(), "CDP-013 capped demand hint was refused");
  Require(capped.record.decision == bulk::DmlPageFilespaceDemandHintDecision::capped,
          "CDP-013 capped demand hint decision mismatch");
  Require(capped.record.granted_page_count == 8,
          "CDP-013 capped demand hint was not bounded by policy");

  auto disabled_request = HintRequest(4, 8);
  disabled_request.enabled = false;
  const auto disabled = bulk::MakeDmlPageFilespaceDemandHint(disabled_request);
  Require(disabled.status.ok() && !disabled.accepted,
          "CDP-013 disabled demand hint did not no-op successfully");
  Require(disabled.record.decision == bulk::DmlPageFilespaceDemandHintDecision::disabled,
          "CDP-013 disabled demand hint decision mismatch");

  auto refused_request = HintRequest(4, 0);
  const auto refused = bulk::MakeDmlPageFilespaceDemandHint(refused_request);
  Require(!refused.ok(), "CDP-013 unbounded demand hint was accepted");
  Require(refused.diagnostic.diagnostic_code == "dml_demand_hint_missing_bound",
          "CDP-013 refused demand hint diagnostic mismatch");
}

void TestInsertDemandReachesAgentsBeforeForegroundAllocation() {
  auto fixture = MakeFixture("insert_agents", 1000);
  const auto inserted = api::EngineInsertRows(InsertRequest(
      fixture,
      "cdp013-insert-agents",
      Rows("insert", 3),
      DemandOptions(8, 8)));
  RequireOk(inserted, "CDP-013 insert with demand hints failed");
  Require(inserted.inserted_count == 3, "CDP-013 inserted row count mismatch");
  Require(HasEvidence(inserted.evidence, "dml_demand_hint_decision", "accepted"),
          "CDP-013 demand hint acceptance evidence missing");
  Require(HasEvidence(inserted.evidence, "filespace_agent_demand_decision",
                      "capacity_window_approved"),
          "CDP-013 filespace agent did not approve demand before allocation");
  Require(HasEvidence(inserted.evidence, "page_agent_demand_decision",
                      "preallocation_completed"),
          "CDP-013 page agent did not preallocate demand");
  Require(HasEvidence(inserted.evidence, "row_page_allocation_source",
                      "SB-STORAGE-PAGE-ALLOCATION-PREALLOCATED-POOL-HIT"),
          "CDP-013 row allocation did not consume demanded preallocation");
  Require(HasEvidence(inserted.evidence, "index_page_allocation_source",
                      "SB-STORAGE-PAGE-ALLOCATION-PREALLOCATED-POOL-HIT"),
          "CDP-013 index allocation did not consume demanded preallocation");
  Require(EvidenceIndex(inserted.evidence, "page_agent_demand_decision",
                        "preallocation_completed") <
              EvidenceIndex(inserted.evidence, "row_page_allocation_source",
                            "SB-STORAGE-PAGE-ALLOCATION-PREALLOCATED-POOL-HIT"),
          "CDP-013 page agent evidence did not precede foreground row allocation");
  Require(EvidenceIndex(inserted.evidence, "filespace_agent_demand_decision",
                        "capacity_window_approved") <
              EvidenceIndex(inserted.evidence, "row_page_allocation_source",
                            "SB-STORAGE-PAGE-ALLOCATION-PREALLOCATED-POOL-HIT"),
          "CDP-013 filespace agent evidence did not precede foreground row allocation");

  const auto state = LoadedState(fixture.context);
  Require(state.row_versions.size() == 3, "CDP-013 inserted row versions missing");
  Require(state.index_entries.size() == 3, "CDP-013 inserted index entries missing");
}

void TestCopyDemandEvidenceFlowsThroughImport() {
  auto fixture = MakeFixture("copy_agents", 2000);
  auto options = DemandOptions(8, 8);
  options.push_back("copy_append_batching=enabled");
  options.push_back("copy_append_batch_rows=4");

  const auto imported = api::EngineExecuteImportRows(ImportRequest(
      fixture,
      "cdp013-copy-agents",
      Rows("copy", 4),
      std::move(options)));
  RequireOk(imported, "CDP-013 COPY/import with demand hints failed");
  Require(imported.inserted_rows == 4 && imported.accepted_rows == 4,
          "CDP-013 COPY/import row count mismatch");
  Require(HasEvidence(imported.evidence, "import_execution", "direct_physical"),
          "CDP-013 COPY/import did not use direct physical lane");
  Require(HasEvidence(imported.evidence, "import_execution_delegate", "none"),
          "CDP-013 COPY/import delegated instead of using direct physical lane");
  Require(HasEvidence(imported.evidence, "dml_demand_hint_decision", "accepted"),
          "CDP-013 COPY/import demand evidence missing");
  Require(HasEvidence(imported.evidence, "filespace_agent_demand_decision",
                      "capacity_window_approved"),
          "CDP-013 COPY/import filespace demand evidence missing");
  Require(HasEvidence(imported.evidence, "row_page_allocation_source",
                      "SB-STORAGE-PAGE-ALLOCATION-PREALLOCATED-POOL-HIT"),
          "CDP-013 COPY/import foreground allocation did not hit demanded pages");
}

void TestDeniedDemandLeavesForegroundCoherent() {
  auto denied_fixture = MakeFixture("resource_denied", 3000);
  const auto denied = api::EngineInsertRows(InsertRequest(
      denied_fixture,
      "cdp013-resource-denied",
      Rows("denied", 2),
      DemandOptions(2, 0)));
  Require(!denied.ok, "CDP-013 resource-denied demand unexpectedly inserted rows");
  Require(!denied.diagnostics.empty() &&
              denied.diagnostics.front().code ==
                  "SB-STORAGE-PAGE-ALLOCATION-INSUFFICIENT-FREE-SPACE",
          "CDP-013 resource-denied foreground diagnostic mismatch");
  Require(HasEvidence(denied.evidence, "dml_demand_runtime_outcome", "capacity_refused"),
          "CDP-013 resource-denied demand outcome evidence missing");
  Require(HasEvidence(denied.evidence, "filespace_agent_demand_decision",
                      "capacity_window_refused"),
          "CDP-013 filespace capacity refusal evidence missing");
  Require(LoadedState(denied_fixture.context).row_versions.empty(),
          "CDP-013 denied demand left partial row versions");

  auto fallback_fixture = MakeFixture("refused_fallback", 4000);
  const auto fallback = api::EngineInsertRows(InsertRequest(
      fallback_fixture,
      "cdp013-refused-fallback",
      Rows("fallback", 1),
      {"page_allocation.runtime=enabled",
       "dml_demand_hints=enabled",
       "page_allocation.free_pages=2"}));
  RequireOk(fallback, "CDP-013 refused demand did not fall back coherently");
  Require(HasEvidence(fallback.evidence, "dml_demand_hint_diagnostic",
                      "dml_demand_hint_missing_bound"),
          "CDP-013 refused demand diagnostic evidence missing");
  Require(HasEvidence(fallback.evidence, "row_page_allocation_source",
                      "SB-STORAGE-PAGE-ALLOCATION-FREE-EXTENT-FALLBACK"),
          "CDP-013 refused demand did not use coherent foreground fallback");
  Require(LoadedState(fallback_fixture.context).row_versions.size() == 1,
          "CDP-013 fallback insert row version missing");
}

}  // namespace

int main() {
  TestDemandHintSurfaceAcceptedCappedDisabledAndRefused();
  TestInsertDemandReachesAgentsBeforeForegroundAllocation();
  TestCopyDemandEvidenceFlowsThroughImport();
  TestDeniedDemandLeavesForegroundCoherent();
  return EXIT_SUCCESS;
}
