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
    for (const auto& diagnostic : result.diagnostics) {
      std::cerr << diagnostic.code << ':' << diagnostic.detail << '\n';
    }
    Fail(message);
  }
}

platform::u64 TimeSeed() {
  return static_cast<platform::u64>(
      std::chrono::steady_clock::now().time_since_epoch().count());
}

platform::TypedUuid NewUuid(platform::UuidKind kind, platform::u64 salt) {
  const auto generated = uuid::GenerateEngineIdentityV7(kind, 1900000000000ull + salt);
  Require(generated.ok(), "IPAR preallocation UUID generation failed");
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
  platform::u32 page_size = 0;
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
  typed.state = api::EngineValueState::value;
  return typed;
}

api::EngineRowValue Row(std::string id, std::string note) {
  api::EngineRowValue row;
  row.fields.push_back({"id", TextValue(std::move(id))});
  row.fields.push_back({"note", TextValue(std::move(note))});
  return row;
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

api::CrudTableRecord Table(const Fixture& fixture) {
  api::CrudTableRecord table;
  table.creator_tx = fixture.context.local_transaction_id;
  table.table_uuid = fixture.table_uuid;
  table.default_name = "ipar_page_size_preallocation";
  table.columns.push_back({"id", "canonical=character;primary_key=true;not_null=true"});
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
  index.default_name = "ipar_page_size_preallocation_id_pk";
  index.unique = true;
  index.key_envelopes.push_back("id");
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
  RequireOk(begun, "IPAR preallocation begin transaction failed");
  auto context = request.context;
  context.local_transaction_id = begun.local_transaction_id;
  context.transaction_uuid = begun.transaction_uuid;
  context.snapshot_visible_through_local_transaction_id =
      begun.snapshot_visible_through_local_transaction_id;
  context.transaction_isolation_level = begun.isolation_level;
  return context;
}

Fixture MakeFixture(platform::u32 page_size, platform::u64 salt) {
  Fixture fixture;
  fixture.salt = salt;
  fixture.page_size = page_size;
  fixture.dir = std::filesystem::temp_directory_path() /
                ("scratchbird_ipar_page_size_prealloc_" +
                 std::to_string(page_size) + "_" + std::to_string(salt));
  std::filesystem::create_directories(fixture.dir);
  fixture.database_path = fixture.dir / "ipar_page_size_preallocation.sbdb";

  db::DatabaseCreateConfig create;
  create.path = fixture.database_path.string();
  create.database_uuid = NewUuid(platform::UuidKind::database, salt + 1);
  create.filespace_uuid = NewUuid(platform::UuidKind::filespace, salt + 2);
  create.creation_unix_epoch_millis = 1900000000000ull + salt + 3;
  create.page_size = page_size;
  create.require_resource_seed_pack = false;
  create.allow_minimal_resource_bootstrap = true;
  create.allow_overwrite = true;
  const auto created = db::CreateDatabaseFile(create);
  if (!created.ok()) {
    std::cerr << created.diagnostic.diagnostic_code << ':'
              << created.diagnostic.message_key << '\n';
  }
  Require(created.ok(), "IPAR preallocation database create failed");
  Require(created.state.header.page_size == page_size,
          "IPAR preallocation database page size drifted");

  fixture.database_uuid = uuid::UuidToString(create.database_uuid.value);
  fixture.table_uuid = NewUuidText(platform::UuidKind::object, salt + 10);
  fixture.index_uuid = NewUuidText(platform::UuidKind::object, salt + 11);
  fixture.context = Begin(fixture, "ipar-page-size-preallocation-metadata");

  const auto table = api::AppendMgaTableMetadata(fixture.context, Table(fixture));
  Require(!table.error, "IPAR preallocation table metadata append failed");
  const auto index = api::AppendMgaIndexMetadata(fixture.context, UniqueIdIndex(fixture));
  Require(!index.error, "IPAR preallocation index metadata append failed");
  return fixture;
}

std::vector<std::string> DemandOptions() {
  return {"page_allocation.runtime=enabled",
          "dml_demand_hints=enabled",
          "dml_demand_hints.max_pages=8",
          "dml_demand_hints.available_capacity_pages=8"};
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

api::EngineApiU64 EvidenceU64(const std::vector<api::EngineEvidenceReference>& evidence,
                              std::string_view kind) {
  for (const auto& item : evidence) {
    if (item.evidence_kind != kind) {
      continue;
    }
    try {
      return static_cast<api::EngineApiU64>(std::stoull(item.evidence_id));
    } catch (...) {
      return 0;
    }
  }
  return 0;
}

void RequireWorkerPreworkEvidence(const std::vector<api::EngineEvidenceReference>& evidence) {
  Require(HasEvidence(evidence, "page_allocator_worker_capacity_assignment",
                      "page_allocation_manager:slot=1"),
          "IPAR preallocation page worker slot proof missing");
  Require(HasEvidence(evidence, "filespace_capacity_worker_capacity_assignment",
                      "filespace_capacity_manager:slot=2"),
          "IPAR preallocation filespace worker slot proof missing");
  Require(HasEvidence(evidence, "page_allocator_demand_evaluation_mode",
                      "bounded_agent_prework_tick"),
          "IPAR preallocation demand did not use bounded agent prework tick");
  Require(HasEvidence(evidence, "filespace_capacity_demand_evaluation_mode",
                      "bounded_agent_prework_tick"),
          "IPAR preallocation filespace demand did not use bounded agent prework tick");
  Require(HasEvidence(evidence, "page_allocator_preallocation_evaluation_mode",
                      "bounded_agent_prework_tick"),
          "IPAR preallocation did not use bounded agent prework tick");
  Require(HasEvidence(evidence, "page_allocator_prework_inventory_produced", "true"),
          "IPAR preallocation inventory was not produced by page agent prework");
  Require(HasEvidence(evidence, "page_allocator_prework_inventory_state", "preallocated"),
          "IPAR preallocation inventory state proof missing");
  Require(HasEvidence(evidence, "page_allocator_prework_inventory_before_foreground_reserve",
                      "true"),
          "IPAR preallocation inventory was not available before foreground reserve");
  Require(HasEvidence(evidence, "page_allocation_mga_finality_authority",
                      "durable_transaction_inventory"),
          "IPAR preallocation lost MGA finality authority proof");
  Require(HasEvidence(evidence, "page_allocation_agent_finality_authority", "false"),
          "IPAR preallocation incorrectly made agent finality-authoritative");
  Require(HasEvidence(evidence, "page_allocator_inline_degraded_path_count", "0"),
          "IPAR preallocation unexpectedly used degraded page-agent path");
  Require(EvidenceU64(evidence, "page_filespace_agent_queue_depth_after_capacity_enqueue") > 0,
          "IPAR preallocation bounded queue enqueue accounting missing");
  Require(EvidenceU64(evidence, "page_allocator_prework_inventory_pages") > 0,
          "IPAR preallocation inventory page count missing");
}

void RequirePreallocationEvidence(const std::vector<api::EngineEvidenceReference>& evidence,
                                  std::string_view prefix,
                                  bool require_index) {
  Require(HasEvidence(evidence, "dml_demand_hint_decision", "accepted"),
          "IPAR preallocation demand hint was not accepted");
  Require(HasEvidence(evidence, "filespace_agent_demand_decision",
                      "capacity_window_approved"),
          "IPAR preallocation filespace agent did not approve demand");
  Require(HasEvidence(evidence, "page_agent_demand_decision",
                      "preallocation_completed"),
          "IPAR preallocation page agent did not preallocate");
  Require(HasEvidence(evidence, "row_page_allocation_source",
                      "SB-STORAGE-PAGE-ALLOCATION-PREALLOCATED-POOL-HIT"),
          "IPAR preallocation row allocation did not hit preallocated pool");
  Require(HasEvidence(evidence, "row_page_preallocation_claim",
                      "physical_preallocated_pages"),
          "IPAR preallocation row pages were not physically preallocated");
  Require(HasEvidence(evidence, "row_page_preallocated_inventory_consumed", "true"),
          "IPAR preallocation row inventory was not consumed");
  Require(HasEvidence(evidence, "row_page_preallocation_inventory_source",
                      "page_allocation_manager"),
          "IPAR preallocation row inventory did not come from page agent prework");
  Require(EvidenceU64(evidence, "row_page_preallocation_granted_pages") > 0,
          "IPAR preallocation row granted page count missing");
  RequireWorkerPreworkEvidence(evidence);
  if (require_index) {
    Require(HasEvidence(evidence, "index_page_allocation_source",
                        "SB-STORAGE-PAGE-ALLOCATION-PREALLOCATED-POOL-HIT"),
            "IPAR preallocation index allocation did not hit preallocated pool");
    Require(HasEvidence(evidence, "index_page_preallocation_claim",
                        "physical_preallocated_pages"),
            "IPAR preallocation index pages were not physically preallocated");
    Require(HasEvidence(evidence, "index_page_preallocated_inventory_consumed", "true"),
            "IPAR preallocation index inventory was not consumed");
    Require(HasEvidence(evidence, "index_page_preallocation_inventory_source",
                        "page_allocation_manager"),
            "IPAR preallocation index inventory did not come from page agent prework");
    Require(EvidenceU64(evidence, "index_page_preallocation_granted_pages") > 0,
            "IPAR preallocation index granted page count missing");
  }
  Require(!std::string(prefix).empty(), "IPAR preallocation test label missing");
}

void RequireInsertPreworkQueueEvidence(
    const std::vector<api::EngineEvidenceReference>& evidence) {
  Require(HasEvidence(evidence, "insert_prework_queue_enabled", "true"),
          "IPAR insert prework queue was not enabled");
  Require(HasEvidence(evidence, "insert_prework_helper_thread_started", "true"),
          "IPAR insert prework helper did not start");
  Require(EvidenceU64(evidence, "insert_prework_rows_enqueued") > 0,
          "IPAR insert prework queue did not enqueue rows");
  Require(EvidenceU64(evidence, "insert_prework_rows_prepared") > 0,
          "IPAR insert prework queue did not prepare row capacity");
  Require(HasEvidence(evidence, "insert_prework_row_capacity_ready", "true"),
          "IPAR insert prework row capacity was not ready before write");
  Require(HasEvidence(evidence, "insert_transaction_queue_return_before_flush",
                      "false"),
          "IPAR insert prework queue allowed return before flush");
  Require(HasEvidence(evidence, "mga_finality_authority",
                      "engine_transaction_inventory"),
          "IPAR insert prework lost MGA transaction authority proof");
  Require(HasEvidence(evidence, "parser_finality", "false"),
          "IPAR insert prework made parser finality authoritative");
}

api::EngineInsertRowsRequest InsertRequest(Fixture& fixture,
                                           std::string request_id,
                                           std::vector<api::EngineRowValue> rows) {
  api::EngineInsertRowsRequest request;
  request.context = fixture.context;
  request.context.request_id = std::move(request_id);
  request.target_table.uuid.canonical = fixture.table_uuid;
  request.target_table.object_kind = "table";
  request.target_object.uuid.canonical = fixture.table_uuid;
  request.target_object.object_kind = "table";
  request.estimated_row_count = static_cast<api::EngineApiU64>(rows.size());
  request.input_rows = std::move(rows);
  request.option_envelopes = DemandOptions();
  request.option_envelopes.push_back("direct_physical_insert=disabled");
  request.option_envelopes.push_back("dml.insert_prework_queue=enabled");
  request.option_envelopes.push_back("dml.insert_prework_queue.min_rows=1");
  return request;
}

api::EngineExecuteImportRowsRequest ImportRequest(
    Fixture& fixture,
    std::string request_id,
    std::vector<api::EngineRowValue> rows) {
  api::EngineExecuteImportRowsRequest request;
  request.context = fixture.context;
  request.context.request_id = std::move(request_id);
  request.target_table.uuid.canonical = fixture.table_uuid;
  request.target_table.object_kind = "table";
  request.target_object.uuid.canonical = fixture.table_uuid;
  request.target_object.object_kind = "table";
  request.source.source_kind = "csv_stream";
  request.source.source_position = "row:0";
  request.format.format_family = "csv";
  request.import_policy.reject_mode = "fail_fast";
  request.import_policy.reject_payload_policy = "diagnostic_only";
  request.import_policy.resume_policy = "fail_closed";
  request.canonical_rows = std::move(rows);
  request.estimated_row_count =
      static_cast<api::EngineApiU64>(request.canonical_rows.size());
  request.option_envelopes = DemandOptions();
  request.option_envelopes.push_back("copy_append_batching=enabled");
  request.option_envelopes.push_back("copy_append_batch_rows=4");
  return request;
}

void VerifyPageSize(platform::u32 page_size, platform::u64 salt) {
  auto insert_fixture = MakeFixture(page_size, salt);
  const auto inserted = api::EngineInsertRows(InsertRequest(
      insert_fixture,
      "ipar-page-size-insert-" + std::to_string(page_size),
      Rows("insert-" + std::to_string(page_size), 3)));
  RequireOk(inserted, "IPAR page-size insert preallocation failed");
  Require(inserted.inserted_count == 3,
          "IPAR page-size insert row count mismatch");
  RequirePreallocationEvidence(inserted.evidence, "insert", true);
  RequireInsertPreworkQueueEvidence(inserted.evidence);
  Require(inserted.dml_summary.preallocation_granted_pages > 0,
          "IPAR page-size insert summary missing preallocation grants");

  auto copy_fixture = MakeFixture(page_size, salt + 1000);
  const auto imported = api::EngineExecuteImportRows(ImportRequest(
      copy_fixture,
      "ipar-page-size-copy-" + std::to_string(page_size),
      Rows("copy-" + std::to_string(page_size), 4)));
  RequireOk(imported, "IPAR page-size COPY preallocation failed");
  Require(imported.inserted_rows == 4 && imported.accepted_rows == 4,
          "IPAR page-size COPY row count mismatch");
  Require(HasEvidence(imported.evidence, "import_execution", "direct_physical"),
          "IPAR page-size COPY did not use direct physical lane");
  RequirePreallocationEvidence(imported.evidence, "copy", false);
  Require(imported.dml_summary.preallocation_granted_pages > 0,
          "IPAR page-size COPY summary missing preallocation grants");
}

}  // namespace

int main() {
  const platform::u64 salt = TimeSeed();
  const std::vector<platform::u32> page_sizes = {
      4096, 8192, 16384, 32768, 65536, 131072};
  for (std::size_t index = 0; index < page_sizes.size(); ++index) {
    VerifyPageSize(page_sizes[index], salt + static_cast<platform::u64>(index * 10000));
  }
  return EXIT_SUCCESS;
}
