// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "database_lifecycle.hpp"
#include "dml/native_bulk_ingest_api.hpp"
#include "mga_relation_store/mga_relation_store.hpp"
#include "transaction/transaction_api.hpp"
#include "uuid.hpp"

#include <algorithm>
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

platform::u64 NowMillis() {
  return static_cast<platform::u64>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

platform::u64 UniqueMillis() {
  static platform::u64 counter = 0;
  return NowMillis() + (++counter * 1000);
}

platform::TypedUuid NewTypedUuid(platform::UuidKind kind,
                                 platform::u64 salt) {
  const auto generated =
      uuid::GenerateEngineIdentityV7(kind, UniqueMillis() + salt);
  Require(generated.ok(), "ORH-210 UUID generation failed");
  return generated.value;
}

std::string NewUuidText(platform::UuidKind kind, platform::u64 salt) {
  return uuid::UuidToString(NewTypedUuid(kind, salt).value);
}

struct Fixture {
  std::filesystem::path dir;
  std::filesystem::path database_path;
  std::string database_uuid;
  std::string table_uuid;
  std::string index_uuid;
  std::string index_family = api::kCrudIndexFamilyBtree;
  std::string index_profile = api::kCrudIndexProfileRowStoreScalarBtreeV1;
  bool index_unique = false;
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
  return typed;
}

api::EngineRowValue Row(std::string id, std::string name) {
  api::EngineRowValue row;
  row.fields.push_back({"id", TextValue(std::move(id))});
  row.fields.push_back({"name", TextValue(std::move(name))});
  return row;
}

api::CrudTableRecord Table(const Fixture& fixture,
                           const api::EngineRequestContext& context) {
  api::CrudTableRecord table;
  table.creator_tx = context.local_transaction_id;
  table.table_uuid = fixture.table_uuid;
  table.default_name = "orh_batch4_native_bulk";
  table.columns.push_back({"id", "canonical=character"});
  table.columns.push_back({"name", "canonical=character"});
  return table;
}

api::CrudIndexRecord SecondaryIndex(const Fixture& fixture,
                                    const api::EngineRequestContext& context) {
  api::CrudIndexRecord index;
  index.creator_tx = context.local_transaction_id;
  index.index_uuid = fixture.index_uuid;
  index.table_uuid = fixture.table_uuid;
  index.column_name = "name";
  index.family = fixture.index_family;
  index.profile = fixture.index_profile;
  index.unique = fixture.index_unique;
  return index;
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
  RequireOk(begun, "ORH-210 begin transaction failed");
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
  RequireOk(api::EngineCommitTransaction(request), "ORH-210 commit failed");
}

void Rollback(const api::EngineRequestContext& context) {
  api::EngineRollbackTransactionRequest request;
  request.context = context;
  RequireOk(api::EngineRollbackTransaction(request), "ORH-210 rollback failed");
}

Fixture MakeFixture(std::string name,
                    platform::u64 salt,
                    std::string index_family = api::kCrudIndexFamilyBtree,
                    std::string index_profile =
                        api::kCrudIndexProfileRowStoreScalarBtreeV1,
                    bool index_unique = false) {
  Fixture fixture;
  fixture.salt = salt;
  fixture.index_family = std::move(index_family);
  fixture.index_profile = std::move(index_profile);
  fixture.index_unique = index_unique;
  fixture.dir = std::filesystem::temp_directory_path() /
                ("scratchbird_orh_batch4_" + name + "_" +
                 std::to_string(UniqueMillis()));
  std::filesystem::create_directories(fixture.dir);
  fixture.database_path = fixture.dir / "orh_batch4.sbdb";

  db::DatabaseCreateConfig create;
  create.path = fixture.database_path.string();
  create.database_uuid = NewTypedUuid(platform::UuidKind::database, salt + 1);
  create.filespace_uuid = NewTypedUuid(platform::UuidKind::filespace, salt + 2);
  create.creation_unix_epoch_millis = UniqueMillis();
  create.require_resource_seed_pack = false;
  create.allow_minimal_resource_bootstrap = true;
  create.allow_overwrite = true;
  const auto created = db::CreateDatabaseFile(create);
  if (!created.ok()) {
    std::cerr << created.diagnostic.diagnostic_code << ':'
              << created.diagnostic.message_key << '\n';
  }
  Require(created.ok(), "ORH-210 database create failed");

  fixture.database_uuid = uuid::UuidToString(create.database_uuid.value);
  fixture.table_uuid = NewUuidText(platform::UuidKind::object, salt + 10);
  fixture.index_uuid = NewUuidText(platform::UuidKind::object, salt + 11);

  auto metadata = Begin(fixture, "orh-batch4-metadata");
  const auto table = api::AppendMgaTableMetadata(metadata, Table(fixture, metadata));
  Require(!table.error, "ORH-210 table metadata append failed");
  const auto index =
      api::AppendMgaIndexMetadata(metadata, SecondaryIndex(fixture, metadata));
  Require(!index.error, "ORH-210 index metadata append failed");
  Commit(metadata);
  return fixture;
}

std::vector<api::EngineRowValue> Rows(std::string prefix, int count) {
  std::vector<api::EngineRowValue> rows;
  rows.reserve(static_cast<std::size_t>(count));
  for (int index = 0; index < count; ++index) {
    rows.push_back(Row(prefix + "-id-" + std::to_string(index + 1),
                       prefix + "-name-" + std::to_string(index + 1)));
  }
  return rows;
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

api::EnginePredicateEnvelope EqualsPredicate(std::string column,
                                             std::string value) {
  api::EnginePredicateEnvelope predicate;
  predicate.predicate_kind = "column_equals";
  predicate.canonical_predicate_envelope = std::move(column);
  predicate.bound_values.push_back(TextValue(std::move(value)));
  return predicate;
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

std::string EvidenceValue(const std::vector<api::EngineEvidenceReference>& evidence,
                          std::string_view kind) {
  for (const auto& item : evidence) {
    if (item.evidence_kind == kind) {
      return item.evidence_id;
    }
  }
  return {};
}

std::size_t EvidenceIndex(const std::vector<api::EngineEvidenceReference>& evidence,
                          std::string_view kind,
                          std::string_view id) {
  for (std::size_t index = 0; index < evidence.size(); ++index) {
    if (evidence[index].evidence_kind == kind &&
        evidence[index].evidence_id == id) {
      return index;
    }
  }
  return evidence.size();
}

void RequireDiagnostic(const api::EngineApiResult& result,
                       std::string_view code,
                       std::string_view message) {
  Require(!result.ok, message);
  Require(!result.diagnostics.empty(), message);
  if (result.diagnostics.front().code != code) {
    std::cerr << "expected=" << code
              << " actual=" << result.diagnostics.front().code << '\n';
  }
  Require(result.diagnostics.front().code == code, message);
}

api::MgaRelationStoreState LoadedState(
    const api::EngineRequestContext& context) {
  const auto loaded = api::LoadMgaRelationStoreState(context);
  Require(loaded.ok, "ORH-211 MGA relation store load failed");
  return loaded.state;
}

void AddDeferredIndexRouteOptions(
    api::EngineExecuteNativeBulkIngestRequest* request) {
  request->option_envelopes = {
      "sorted_bulk_index_build=enabled",
      "orh.deferred_index.require_benchmark_clean=true",
      "write_result_policy=summary_only"};
}

std::string FieldValue(const api::CrudRowVersionRecord& row,
                       std::string_view field_name) {
  for (const auto& field : row.values) {
    if (field.first == field_name) {
      return field.second;
    }
  }
  return {};
}

std::vector<std::string> VisibleBaseIndexKeys(
    const api::MgaRelationStoreState& state,
    const Fixture& fixture,
    const api::EngineRequestContext& context) {
  const auto crud = api::BuildCrudCompatibilityStateFromMga(state);
  std::vector<std::string> keys;
  for (const auto& row :
       api::VisibleCrudRowsForContext(crud, fixture.table_uuid, context)) {
    keys.push_back(FieldValue(row, "name"));
  }
  std::sort(keys.begin(), keys.end());
  return keys;
}

std::vector<std::string> PersistedIndexKeys(
    const api::MgaRelationStoreState& state,
    const Fixture& fixture) {
  std::vector<std::string> keys;
  for (const auto& entry : state.index_entries) {
    if (entry.index_uuid == fixture.index_uuid &&
        entry.table_uuid == fixture.table_uuid) {
      keys.push_back(entry.key_value);
    }
  }
  std::sort(keys.begin(), keys.end());
  return keys;
}

void RequireIndexLookup(const api::MgaRelationStoreState& state,
                        const Fixture& fixture,
                        const api::EngineRequestContext& context,
                        std::string key,
                        std::size_t expected_count,
                        std::string_view message) {
  const auto crud = api::BuildCrudCompatibilityStateFromMga(state);
  const auto lookup = api::IndexedMgaRowsForPredicateForContext(
      crud,
      fixture.table_uuid,
      EqualsPredicate("name", std::move(key)),
      context,
      16);
  Require(lookup.ok, message);
  Require(!lookup.index_refused, message);
  Require(lookup.rows.size() == expected_count, message);
  if (expected_count > 0) {
    Require(lookup.index_used, "ORH-211 indexed lookup did not consume index");
    Require(lookup.index_evidence_id.find("row_recheck_applied=true") !=
                std::string::npos,
            "ORH-211 exact row recheck evidence missing");
    Require(lookup.index_evidence_id.find("visible_count=" +
                                          std::to_string(expected_count)) !=
                std::string::npos,
            "ORH-211 visible index lookup count evidence drifted");
  }
}

void TestNativeBulkRuntimeAndAllocatorEvidence() {
  auto fixture = MakeFixture("native_allocator", 1000);
  auto context = Begin(fixture, "orh-210-212-native");
  auto request = NativeRequest(fixture, context, Rows("orh", 4));
  request.option_envelopes = {
      "page_allocation.runtime=enabled",
      "page_extent_preallocation=enabled",
      "dml_demand_hints=enabled",
      "dml_demand_hints.max_pages=16",
      "dml_demand_hints.available_capacity_pages=16",
      "dml_demand_hints.minimum_free_pages=1",
      "dml_demand_hints.target_free_pages=8",
      "write_result_policy=summary_only"};

  const auto result = api::EngineExecuteNativeBulkIngest(request);
  RequireOk(result, "ORH-210 native direct bulk ingest failed");
  Require(result.inserted_rows == 4 && result.accepted_rows == 4,
          "ORH-210 native direct bulk row counts drifted");
  Require(HasEvidence(result.evidence, "native_bulk_ingest_source",
                      "binary_typed_rows"),
          "ORH-210 binary batch evidence missing");
  Require(HasEvidence(result.evidence, "native_bulk_ingest_lane",
                      "direct_physical"),
          "ORH-210 direct physical lane evidence missing");
  Require(HasEvidence(result.evidence, "direct_bulk_uuid_generation_mode",
                      "batched"),
          "ORH-210 batched UUID evidence missing");
  Require(HasEvidence(result.evidence, "direct_bulk_version_uuid_generation_mode",
                      "batched"),
          "ORH-210 batched version UUID evidence missing");
  Require(HasEvidence(result.evidence, "direct_mga_append",
                      "row_version_batch"),
          "ORH-210 direct MGA append evidence missing");
  Require(HasEvidence(result.evidence, "orh_210_runtime_consumed", "true"),
          "ORH-210 runtime consumption evidence missing");
  Require(HasEvidence(result.evidence, "parser_finality_authority", "false") &&
              HasEvidence(result.evidence, "reference_finality_authority", "false") &&
              HasEvidence(result.evidence, "mga_finality_authority",
                          "engine_transaction_inventory"),
          "ORH-210 finality authority evidence drifted");

  Require(HasEvidence(result.evidence, "dml_demand_hint_decision", "accepted"),
          "ORH-212 DML demand hint was not accepted");
  Require(HasEvidence(result.evidence, "page_agent_demand_decision",
                      "capacity_request_queued"),
          "ORH-212 page agent did not queue capacity request");
  Require(HasEvidence(result.evidence, "filespace_agent_demand_decision",
                      "capacity_window_approved"),
          "ORH-212 filespace agent did not approve capacity window");
  Require(HasEvidence(result.evidence, "page_agent_demand_decision",
                      "preallocation_completed"),
          "ORH-212 page agent did not preallocate before append");
  Require(HasEvidence(result.evidence, "agent_worker_capacity_planning_snapshot",
                      "ok"),
          "ORH-212 worker-capacity planning evidence missing");
  Require(HasEvidence(result.evidence,
                      "page_filespace_worker_capacity_planned_separate_slots",
                      "true"),
          "ORH-212 planned separate worker-capacity evidence missing");
  Require(HasEvidence(result.evidence,
                      "page_allocator_worker_capacity_planned_ahead_of_foreground",
                      "true"),
          "ORH-212 planned ahead-of-foreground evidence missing");
  Require(HasEvidence(result.evidence, "page_allocator_worker_runtime_proven",
                      "false"),
          "ORH-212 worker runtime must not be proven by planning-only evidence");
  Require(HasEvidence(result.evidence, "page_allocator_worker_runtime_mode",
                      "inline_tick_not_worker_runtime"),
          "ORH-212 page allocator runtime mode must expose inline tick fallback");
  Require(HasEvidence(result.evidence, "filespace_capacity_worker_runtime_mode",
                      "inline_tick_not_worker_runtime"),
          "ORH-212 filespace runtime mode must expose inline tick fallback");
  Require(HasEvidence(result.evidence,
                      "page_allocator_worker_runtime_blocker",
                      "SB_ORH_AHEAD_OF_NEED_PAGE_ALLOCATOR.WORKER_RUNTIME_UNPROVEN"),
          "ORH-212 exact worker-runtime blocker evidence missing");
  Require(HasEvidence(result.evidence, "page_allocator_ahead_of_need_runtime",
                      "blocked"),
          "ORH-212 ahead-of-need runtime must remain blocked");
  Require(HasEvidence(result.evidence,
                      "page_allocator_inline_tick_consumed_demand",
                      "page_allocation_manager"),
          "ORH-212 inline page allocation tick evidence missing");
  Require(HasEvidence(result.evidence,
                      "filespace_capacity_inline_tick_consumed_demand",
                      "filespace_capacity_manager"),
          "ORH-212 inline filespace tick evidence missing");
  Require(!HasEvidence(result.evidence, "page_filespace_separate_workers",
                       "true"),
          "ORH-212 planning-only evidence was mislabeled as runtime workers");
  Require(!HasEvidence(result.evidence, "page_allocator_ahead_of_foreground_demand",
                       "true"),
          "ORH-212 planning-only evidence was mislabeled as ahead-of-foreground runtime");
  Require(!HasEvidence(result.evidence, "page_allocator_agent_consumed_demand",
                       "page_allocation_manager"),
          "ORH-212 inline tick was mislabeled as worker agent consumption");
  Require(!HasEvidence(result.evidence, "filespace_capacity_agent_consumed_demand",
                       "filespace_capacity_manager"),
          "ORH-212 inline filespace tick was mislabeled as worker agent consumption");
  Require(!HasEvidence(result.evidence, "page_allocator_ahead_of_need_runtime",
                       "preallocation_before_foreground_append"),
          "ORH-212 inline preallocation was mislabeled as worker ahead-of-need runtime");
  Require(EvidenceIndex(result.evidence, "row_page_allocation_source",
                        "SB-STORAGE-PAGE-ALLOCATION-PREALLOCATED-POOL-HIT") <
              EvidenceIndex(result.evidence, "direct_mga_append",
                            "row_version_batch"),
          "ORH-212 inline row preallocation did not precede foreground append");
  Rollback(context);
}

void TestDeferredIndexBulkPublishConsumesLiveRoute() {
  auto fixture = MakeFixture("index_publish", 2000);
  auto context = Begin(fixture, "orh-211-live-publish");
  auto request = NativeRequest(fixture, context, Rows("idx", 3));
  AddDeferredIndexRouteOptions(&request);

  const auto result = api::EngineExecuteNativeBulkIngest(request);
  RequireOk(result, "ORH-211 live sorted index publish failed");
  Require(result.inserted_rows == 3 && result.accepted_rows == 3,
          "ORH-211 live sorted index publish row counts drifted");
  Require(HasEvidence(result.evidence,
                      "orh_deferred_index_bulk_publish_selected",
                      "true"),
          "ORH-211 deferred index route was not selected");
  Require(HasEvidence(result.evidence,
                      "orh_deferred_index_bulk_publish_benchmark_clean",
                      "proven"),
          "ORH-211 deferred index publish did not prove benchmark-clean");
  Require(HasEvidence(result.evidence,
                      "orh_deferred_index_bulk_publish_consumed_provider",
                      "core.index.sorted_bulk_index_build"),
          "ORH-211 sorted bulk index provider evidence missing");
  Require(HasEvidence(result.evidence,
                      "orh_deferred_index_bulk_publish_root_publish_provider",
                      "core.index.PublishIndexRootGeneration") &&
              HasEvidence(result.evidence,
                          "orh_deferred_index_bulk_publish_recovery_provider",
                          "core.index.RecoverSortedBulkRootPublish"),
          "ORH-211 root publish/recovery provider evidence missing");
  Require(HasEvidence(result.evidence,
                      "orh_deferred_index_bulk_publish_mga_publish_provider",
                      "mga_relation_store.exact_index_entry_append"),
          "ORH-211 MGA exact index publish provider evidence missing");
  Require(HasEvidence(result.evidence,
                      "orh_deferred_index_bulk_publish_sorted_root_recovery_proven",
                      "true") &&
              HasEvidence(result.evidence,
                          "orh_deferred_index_bulk_publish_root_rollback_safe",
                          "true") &&
              HasEvidence(result.evidence,
                          "orh_deferred_index_bulk_publish_root_reopen_safe",
                          "true"),
          "ORH-211 root publish/recovery proof fields drifted");
  Require(HasEvidence(result.evidence,
                      "orh_deferred_index_bulk_publish_crash_before_root_publish_active_root",
                      "old_root") &&
              HasEvidence(result.evidence,
                          "orh_deferred_index_bulk_publish_crash_before_root_publish_half_root_exposed",
                          "false") &&
              EvidenceValue(result.evidence,
                            "orh_deferred_index_bulk_publish_crash_before_root_publish")
                      .find("old_root_active_candidate_orphan_classified") !=
                  std::string::npos,
          "ORH-211 crash-before-root-publish recovery evidence missing");
  Require(HasEvidence(result.evidence,
                      "orh_deferred_index_bulk_publish_crash_during_root_publish_active_root",
                      "old_root") &&
              HasEvidence(result.evidence,
                          "orh_deferred_index_bulk_publish_crash_during_root_publish_half_root_exposed",
                          "false") &&
              EvidenceValue(result.evidence,
                            "orh_deferred_index_bulk_publish_crash_during_root_publish")
                      .find("durable_metapage_absent_old_root_active") !=
                  std::string::npos,
          "ORH-211 crash-during-root-publish recovery evidence missing");
  Require(HasEvidence(result.evidence,
                      "orh_deferred_index_bulk_publish_crash_after_root_publish_active_root",
                      "new_root") &&
              HasEvidence(result.evidence,
                          "orh_deferred_index_bulk_publish_crash_after_root_publish_half_root_exposed",
                          "false") &&
              EvidenceValue(result.evidence,
                            "orh_deferred_index_bulk_publish_crash_after_root_publish")
                      .find("new_root_active_validated") !=
                  std::string::npos,
          "ORH-211 crash-after-root-publish recovery evidence missing");
  Require(HasEvidence(result.evidence,
                      "orh_deferred_index_bulk_publish_index_metadata_finality_authority",
                      "false") &&
              HasEvidence(result.evidence,
                          "orh_deferred_index_bulk_publish_index_metadata_recovery_authority",
                          "false") &&
              HasEvidence(result.evidence,
                          "orh_deferred_index_bulk_publish_caller_proof_authority",
                          "false") &&
              HasEvidence(result.evidence,
                          "mga_finality_authority",
                          "engine_transaction_inventory"),
          "ORH-211 index metadata or parser claimed finality authority");
  Require(HasEvidence(result.evidence,
                      "sorted_bulk_index_candidate_root_generation_created",
                      "true") &&
              HasEvidence(result.evidence,
                          "sorted_bulk_index_candidate_tree_validated",
                          "true") &&
              HasEvidence(result.evidence,
                          "sorted_bulk_index_physical_leaf_pack",
                          "true") &&
              HasEvidence(result.evidence,
                          "sorted_bulk_index_exact_append",
                          "mga_index_append_path"),
          "ORH-211 sorted bulk physical publish evidence missing");
  Commit(context);

  auto reopen_context = Begin(fixture, "orh-211-reopen-lookup");
  const auto reopened = LoadedState(reopen_context);
  Require(reopened.row_versions.size() == 3,
          "ORH-211 reopen lost committed row versions");
  Require(reopened.index_entries.size() == 3,
          "ORH-211 reopen lost exact index entries");
  RequireIndexLookup(reopened,
                     fixture,
                     reopen_context,
                     "idx-name-2",
                     1,
                     "ORH-211 reopened index lookup did not find committed row");
  Require(VisibleBaseIndexKeys(reopened, fixture, reopen_context) ==
              PersistedIndexKeys(reopened, fixture),
          "ORH-211 authoritative base-row rebuild keys do not match index metadata");
  Rollback(reopen_context);
}

void TestDeferredIndexBulkPublishRollbackVisibility() {
  auto fixture = MakeFixture("index_rollback", 3000);
  auto context = Begin(fixture, "orh-211-rollback");
  auto request = NativeRequest(fixture, context, Rows("idx_rb", 2));
  AddDeferredIndexRouteOptions(&request);

  const auto result = api::EngineExecuteNativeBulkIngest(request);
  RequireOk(result, "ORH-211 rollback setup sorted publish failed");
  Require(HasEvidence(result.evidence,
                      "orh_deferred_index_bulk_publish_benchmark_clean",
                      "proven"),
          "ORH-211 rollback setup did not use live deferred index route");
  Rollback(context);

  auto observer = Begin(fixture, "orh-211-rollback-observer");
  const auto state = LoadedState(observer);
  const auto crud = api::BuildCrudCompatibilityStateFromMga(state);
  Require(api::VisibleCrudRowsForContext(crud, fixture.table_uuid, observer)
              .empty(),
          "ORH-211 rolled-back base rows became visible");
  RequireIndexLookup(state,
                     fixture,
                     observer,
                     "idx_rb-name-1",
                     0,
                     "ORH-211 rolled-back index entry became visible");
  Rollback(observer);
}

void TestDeferredIndexBulkPublishFailClosedSpoofAndFamilies() {
  auto spoof_fixture = MakeFixture("index_spoof_proof", 4000);
  auto spoof_context = Begin(spoof_fixture, "orh-211-spoof-proof");
  auto spoof_request =
      NativeRequest(spoof_fixture, spoof_context, Rows("idx_spoof", 1));
  spoof_request.option_envelopes = {
      "orh.deferred_index.require_benchmark_clean=true",
      "orh.deferred_index.index_correctness_proven=true",
      "orh.deferred_index.sorted_root_publish_recovery_proof=true",
      "orh.deferred_index.rollback_proof=true",
      "orh.deferred_index.reopen_repair_rebuild_proof=true",
      "orh.deferred_index.mga_visibility_recheck_proof=true",
      "orh.deferred_index.security_recheck_proof=true",
      "orh.deferred_index.authoritative_base_repair_proof=true"};

  const auto spoofed = api::EngineExecuteNativeBulkIngest(spoof_request);
  RequireDiagnostic(
      spoofed,
      "SB_ORH_DEFERRED_INDEX_BULK_PUBLISH.ROUTE_NOT_SELECTED",
      "ORH-211 spoofed proof flags selected benchmark-clean route");
  Require(HasEvidence(spoofed.evidence,
                      "orh_deferred_index_bulk_publish_benchmark_clean",
                      "blocked"),
          "ORH-211 spoofed proof path missing blocked evidence");
  Require(HasEvidence(spoofed.evidence,
                      "orh_deferred_index_bulk_publish_caller_proof_authority",
                      "false") &&
              HasEvidence(spoofed.evidence,
                          "orh_deferred_index_bulk_publish_caller_proof_flags_ignored",
                          "true"),
          "ORH-211 caller proof flags were not quarantined");
  Rollback(spoof_context);

  auto hash_fixture =
      MakeFixture("index_hash", 5000, api::kCrudIndexFamilyHash, "hash", false);
  auto hash_context = Begin(hash_fixture, "orh-211-hash-blocked");
  auto hash_request = NativeRequest(hash_fixture, hash_context, Rows("idx_hash", 1));
  AddDeferredIndexRouteOptions(&hash_request);
  const auto hash = api::EngineExecuteNativeBulkIngest(hash_request);
  RequireDiagnostic(
      hash,
      "SB_ORH_DEFERRED_INDEX_BULK_PUBLISH.FAMILY_ROUTE_UNSUPPORTED",
      "ORH-211 hash family was accepted as ordered deferred write route");
  Require(HasEvidence(hash.evidence,
                      "orh_deferred_index_bulk_publish_family_blocked",
                      std::string(api::kCrudIndexFamilyHash) +
                          "=not_ordered_write_family"),
          "ORH-211 hash family blocker evidence missing");
  Rollback(hash_context);

  auto reference_fixture = MakeFixture("index_reference",
                                   6000,
                                   api::kCrudIndexFamilyReferenceEmulated,
                                   "reference_emulated",
                                   false);
  auto reference_context = Begin(reference_fixture, "orh-211-reference-blocked");
  auto reference_request =
      NativeRequest(reference_fixture, reference_context, Rows("idx_reference", 1));
  AddDeferredIndexRouteOptions(&reference_request);
  const auto reference = api::EngineExecuteNativeBulkIngest(reference_request);
  RequireDiagnostic(
      reference,
      "SB_ORH_DEFERRED_INDEX_BULK_PUBLISH.FAMILY_ROUTE_UNSUPPORTED",
      "ORH-211 reference-emulated family was accepted as authority");
  Require(HasEvidence(reference.evidence,
                      "orh_deferred_index_bulk_publish_family_blocked",
                      std::string(api::kCrudIndexFamilyReferenceEmulated) +
                          "=not_ordered_write_family"),
          "ORH-211 reference-emulated family blocker evidence missing");
  Rollback(reference_context);

  auto unique_fixture =
      MakeFixture("index_unique", 7000, api::kCrudIndexFamilyBtree,
                  api::kCrudIndexProfileRowStoreScalarBtreeV1, true);
  auto unique_context = Begin(unique_fixture, "orh-211-unique-blocked");
  auto unique_request =
      NativeRequest(unique_fixture, unique_context, Rows("idx_unique", 1));
  AddDeferredIndexRouteOptions(&unique_request);
  const auto unique = api::EngineExecuteNativeBulkIngest(unique_request);
  RequireDiagnostic(
      unique,
      "SB_ORH_DEFERRED_INDEX_BULK_PUBLISH.UNIQUE_RESERVATION_PROOF_REQUIRED",
      "ORH-211 unique deferred behavior was accepted without reservation proof");
  Require(HasEvidence(unique.evidence,
                      "orh_deferred_index_bulk_publish_unique_deferred_gated",
                      "reservation_ledger_required"),
          "ORH-211 unique reservation blocker evidence missing");
  Rollback(unique_context);
}

}  // namespace

int main() {
  TestNativeBulkRuntimeAndAllocatorEvidence();
  TestDeferredIndexBulkPublishConsumesLiveRoute();
  TestDeferredIndexBulkPublishRollbackVisibility();
  TestDeferredIndexBulkPublishFailClosedSpoofAndFamilies();
  return EXIT_SUCCESS;
}
