// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "api_types.hpp"
#include "ddl/create_api.hpp"
#include "dml/insert_api.hpp"
#include "dml/merge_api.hpp"
#include "lifecycle/engine_lifecycle_api.hpp"
#include "memory.hpp"
#include "transaction/transaction_api.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

namespace api = scratchbird::engine::internal_api;
namespace memory = scratchbird::core::memory;

namespace {

#ifndef SB_ODFR020_SEED_PACK_ROOT
#define SB_ODFR020_SEED_PACK_ROOT "project/resources/seed-packs/initial-resource-pack"
#endif

constexpr const char* kDatabaseUuid = "019f2200-0000-7000-8000-000000000001";
constexpr const char* kSchemaUuid = "019f2200-0000-7000-8000-000000000101";
constexpr const char* kTableUuid = "019f2200-0000-7000-8000-000000000102";
constexpr const char* kUniqueIdIndexUuid = "019f2200-0000-7000-8000-000000000103";
constexpr const char* kSeedRow = "019f2200-0000-7000-8000-000000000201";
constexpr const char* kRowUuidInsert = "019f2200-0000-7000-8000-000000000202";
constexpr const char* kColumnInsert = "019f2200-0000-7000-8000-000000000203";
constexpr const char* kRollbackRow = "019f2200-0000-7000-8000-000000000205";
constexpr const char* kRollbackDeleteRow = "019f2200-0000-7000-8000-000000000206";

void Require(bool condition, std::string_view message) {
  if (condition) return;
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

memory::AllocationPolicy MemoryPolicy() {
  auto policy = memory::DefaultLocalEngineMemoryPolicy();
  policy.policy_name = "sbsql_merge_multi_action_odfr_020_conformance";
  return policy;
}

void ConfigureMemoryFixture() {
  const auto configured = memory::ConfigureDefaultMemoryManagerForFixture(
      MemoryPolicy(), "sbsql_merge_multi_action_odfr_020_conformance");
  Require(configured.ok(), "ODFR-020 memory fixture configuration failed");
  Require(configured.fixture_mode, "ODFR-020 memory fixture mode was not active");
}

std::filesystem::path MakeTempDir() {
  std::string tmpl = "/tmp/sb_odfr020_merge.XXXXXX";
  std::vector<char> writable(tmpl.begin(), tmpl.end());
  writable.push_back('\0');
  char* made = ::mkdtemp(writable.data());
  return made == nullptr ? std::filesystem::path{} : std::filesystem::path(made);
}

bool HasEvidence(const api::EngineApiResult& result,
                 std::string_view kind,
                 std::string_view id = {}) {
  for (const auto& evidence : result.evidence) {
    if (evidence.evidence_kind == kind &&
        (id.empty() || evidence.evidence_id == id)) {
      return true;
    }
  }
  return false;
}

bool EvidenceContains(const api::EngineApiResult& result,
                      std::string_view kind,
                      std::string_view needle) {
  for (const auto& evidence : result.evidence) {
    if (evidence.evidence_kind == kind &&
        evidence.evidence_id.find(needle) != std::string::npos) {
      return true;
    }
  }
  return false;
}

bool HasNoEvidence(const api::EngineApiResult& result,
                   std::string_view kind,
                   std::string_view id) {
  return !HasEvidence(result, kind, id);
}

std::string FieldValue(const api::EngineApiResult& result,
                       std::string_view field,
                       std::size_t row_index = 0) {
  if (row_index >= result.result_shape.rows.size()) return {};
  for (const auto& [name, value] : result.result_shape.rows[row_index].fields) {
    if (name == field) return value.encoded_value;
  }
  return {};
}

api::EngineRequestContext BaseContext(const std::filesystem::path& database_path,
                                      std::string_view session_suffix = "001") {
  api::EngineRequestContext context;
  context.trust_mode = api::EngineTrustMode::server_isolated;
  context.request_id = "odfr020-merge-multi-action";
  context.database_path = database_path.string();
  context.database_uuid.canonical = kDatabaseUuid;
  context.principal_uuid.canonical = "019f2200-0000-7000-8000-000000000002";
  context.session_uuid.canonical =
      std::string("019f2200-0000-7000-8000-000000000") +
      std::string(session_suffix);
  context.security_context_present = true;
  context.catalog_generation_id = 1;
  context.security_epoch = 1;
  context.resource_epoch = 1;
  context.name_resolution_epoch = 1;
  context.trace_tags.push_back("ODFR-020");
  context.trace_tags.push_back("merge-multi-action");
  return context;
}

std::uint64_t EvidenceU64(const api::EngineApiResult& result,
                          std::string_view kind) {
  for (const auto& evidence : result.evidence) {
    if (evidence.evidence_kind != kind) continue;
    try {
      return static_cast<std::uint64_t>(std::stoull(evidence.evidence_id));
    } catch (...) {
      return 0;
    }
  }
  return 0;
}

api::EngineRequestContext BeginTransaction(const std::filesystem::path& database_path,
                                           std::string_view session_suffix) {
  api::EngineBeginTransactionRequest request;
  request.context = BaseContext(database_path, session_suffix);
  auto begin = api::EngineBeginTransaction(request);
  Require(begin.ok, "transaction begin failed");
  Require(begin.local_transaction_id != 0,
          "transaction begin did not return local transaction id");
  auto context = BaseContext(database_path, session_suffix);
  context.local_transaction_id = begin.local_transaction_id;
  context.transaction_uuid = begin.transaction_uuid;
  context.snapshot_visible_through_local_transaction_id =
      begin.snapshot_visible_through_local_transaction_id != 0
          ? begin.snapshot_visible_through_local_transaction_id
          : EvidenceU64(begin, "snapshot_visible_through_local_transaction_id");
  return context;
}

void Commit(const api::EngineRequestContext& context) {
  api::EngineCommitTransactionRequest request;
  request.context = context;
  auto commit = api::EngineCommitTransaction(request);
  Require(commit.ok, "commit failed");
}

void Rollback(const api::EngineRequestContext& context) {
  api::EngineRollbackTransactionRequest request;
  request.context = context;
  auto rollback = api::EngineRollbackTransaction(request);
  Require(rollback.ok, "rollback failed");
}

api::EngineTypedValue TextValue(std::string value) {
  api::EngineTypedValue typed;
  typed.descriptor.descriptor_kind = "scalar";
  typed.descriptor.canonical_type_name = "text";
  typed.descriptor.encoded_descriptor = "type=text";
  typed.encoded_value = std::move(value);
  return typed;
}

api::EngineLocalizedName Name(std::string name) {
  return {"en", "primary", name, name, true};
}

api::EngineColumnDefinition Column(std::uint32_t ordinal, std::string name) {
  api::EngineColumnDefinition column;
  column.ordinal = ordinal;
  column.requested_column_uuid.canonical =
      "019f2200-0000-7000-8000-00000000030" + std::to_string(ordinal);
  column.names.push_back(Name(std::move(name)));
  column.descriptor.descriptor_uuid.canonical =
      "019f2200-0000-7000-8000-00000000040" + std::to_string(ordinal);
  column.descriptor.descriptor_kind = "scalar";
  column.descriptor.canonical_type_name = "text";
  column.descriptor.encoded_descriptor = "type=text";
  return column;
}

api::EngineIndexDefinition UniqueIdIndex() {
  api::EngineIndexDefinition index;
  index.requested_index_uuid.canonical = kUniqueIdIndexUuid;
  index.names.push_back(Name("odfr020_table_id_unique"));
  index.index_kind = "btree";
  index.key_envelopes.push_back("unique");
  index.key_envelopes.push_back("id");
  return index;
}

api::EngineRowValue Row(std::string row_uuid, std::string id, std::string note) {
  api::EngineRowValue row;
  row.requested_row_uuid.canonical = std::move(row_uuid);
  row.fields.push_back({"id", TextValue(std::move(id))});
  row.fields.push_back({"note", TextValue(std::move(note))});
  return row;
}

void CreateSchemaAndTable(const std::filesystem::path& database_path) {
  auto context = BeginTransaction(database_path, "101");

  api::EngineCreateSchemaRequest schema_request;
  schema_request.context = context;
  schema_request.target_object.uuid.canonical = kSchemaUuid;
  schema_request.target_object.object_kind = "schema";
  schema_request.localized_names.push_back(Name("odfr020_schema"));
  auto schema = api::EngineCreateSchema(schema_request);
  Require(schema.ok, "schema create failed");

  api::EngineCreateTableRequest table_request;
  table_request.context = context;
  table_request.target_schema.uuid.canonical = kSchemaUuid;
  table_request.target_schema.object_kind = "schema";
  table_request.requested_table_uuid.canonical = kTableUuid;
  table_request.target_object.uuid.canonical = kTableUuid;
  table_request.target_object.object_kind = "table";
  table_request.table_names.push_back(Name("odfr020_table"));
  table_request.table_columns.push_back(Column(0, "id"));
  table_request.table_columns.push_back(Column(1, "note"));
  table_request.table_indexes.push_back(UniqueIdIndex());
  auto table = api::EngineCreateTable(table_request);
  Require(table.ok, "table create failed");
  Require(HasEvidence(table, "mga_relation_metadata", "table_create"),
          "table create MGA metadata evidence missing");
  Commit(context);
}

void InsertSeedRow(const api::EngineRequestContext& context) {
  api::EngineInsertRowsRequest request;
  request.context = context;
  request.target_table.uuid.canonical = kTableUuid;
  request.target_table.object_kind = "table";
  request.input_rows.push_back(Row(kSeedRow, "1", "seed"));
  auto inserted = api::EngineInsertRows(request);
  Require(inserted.ok, "seed insert failed");
  Require(inserted.result_shape.rows.size() == 1, "seed insert did not return one row");
}

void RequireCommonMergeEvidence(const api::EngineMergeRowsResult& result) {
  Require(HasEvidence(result, "merge_action_partitioning", "single_pass_source"),
          "merge single-pass partitioning evidence missing");
  Require(HasEvidence(result, "merge_matched_source_rows", "1"),
          "merge matched source count evidence missing");
  Require(HasEvidence(result, "merge_unmatched_source_rows", "1"),
          "merge unmatched source count evidence missing");
  Require(HasEvidence(result, "merge_returning", "affected_rows"),
          "merge returning evidence missing");
  Require(HasEvidence(result, "merge_output_order", "source_order"),
          "merge source-order evidence missing");
  Require(HasEvidence(result, "mga_visibility_recheck", "required"),
          "merge MGA recheck evidence missing");
  Require(HasEvidence(result, "security_recheck", "required"),
          "merge security recheck evidence missing");
  Require(HasEvidence(result, "mga_finality_authority",
                      "engine_transaction_inventory"),
          "merge finality authority evidence missing");
  Require(HasEvidence(result, "parser_or_reference_authority", "false"),
          "merge parser/reference authority evidence missing");
}

void VerifyRowUuidMerge(const api::EngineRequestContext& context) {
  api::EngineMergeRowsRequest request;
  request.context = context;
  request.target_table.uuid.canonical = kTableUuid;
  request.target_table.object_kind = "table";
  request.match_predicate.predicate_kind = "row_uuid_match";
  request.input_rows.push_back(Row(kSeedRow, "1", "rowuuid-updated"));
  request.input_rows.push_back(Row(kRowUuidInsert, "2", "rowuuid-inserted"));
  request.update_assignments.push_back({"note", TextValue("rowuuid-updated")});

  auto merged = api::EngineMergeRows(request);
  Require(merged.ok, "row_uuid merge failed");
  Require(merged.matched_count == 1, "row_uuid merge matched count mismatch");
  Require(merged.inserted_count == 1, "row_uuid merge inserted count mismatch");
  Require(merged.updated_count == 1, "row_uuid merge updated count mismatch");
  Require(merged.result_shape.rows.size() == 2, "row_uuid merge did not return two rows");
  Require(FieldValue(merged, "note", 0) == "rowuuid-updated",
          "row_uuid merge first returning row was not update");
  Require(FieldValue(merged, "note", 1) == "rowuuid-inserted",
          "row_uuid merge second returning row was not insert");
  Require(HasEvidence(merged, "merge_target_access_kind", "row_uuid_singleton"),
          "row_uuid merge access evidence missing");
  Require(HasEvidence(merged, "merge_repeated_full_scan", "false"),
          "row_uuid merge repeated scan evidence missing");
  Require(HasEvidence(merged, "merge_action_execution", "action_batches"),
          "row_uuid merge batched execution evidence missing");
  Require(HasEvidence(merged, "merge_update_batch_count", "1"),
          "row_uuid merge update batch count missing");
  Require(HasEvidence(merged, "merge_insert_batch_count", "1"),
          "row_uuid merge insert batch count missing");
  Require(HasEvidence(merged, "update_target_access_kind", "row_uuid_list"),
          "row_uuid merge update batch did not plan row_uuid_list access");
  Require(HasEvidence(merged, "update_row_candidate_stream", "row_uuid_list"),
          "row_uuid merge update batch did not consume row_uuid_list stream");
  Require(HasNoEvidence(merged, "update_row_candidate_stream", "table_scan"),
          "row_uuid merge update batch fell back to table scan");
  Require(EvidenceContains(merged, "merge_target_access_plan_evidence",
                           "hot_point_lookup_cache_admission=deferred_until_successful_row_locator"),
          "row_uuid merge cache admission was not deferred until lookup success");
  Require(EvidenceContains(merged, "merge_hot_point_lookup_cache",
                           "hot_point_lookup_cache_actual_row_locator"),
          "row_uuid merge did not admit actual successful row locator");
  RequireCommonMergeEvidence(merged);
}

void VerifyUniqueIndexMerge(const api::EngineRequestContext& context) {
  api::EngineMergeRowsRequest request;
  request.context = context;
  request.target_table.uuid.canonical = kTableUuid;
  request.target_table.object_kind = "table";
  request.match_predicate.predicate_kind = "column_equals";
  request.match_predicate.canonical_predicate_envelope = "id";
  request.input_rows.push_back(Row("019f2200-0000-7000-8000-000000000204",
                                   "1",
                                   "unique-updated"));
  request.input_rows.push_back(Row(kColumnInsert, "3", "unique-inserted"));
  request.update_assignments.push_back({"note", TextValue("unique-updated")});

  auto merged = api::EngineMergeRows(request);
  Require(merged.ok, "unique index merge failed");
  Require(merged.matched_count == 1, "unique index merge matched count mismatch");
  Require(merged.inserted_count == 1, "unique index merge inserted count mismatch");
  Require(merged.updated_count == 1, "unique index merge updated count mismatch");
  Require(merged.result_shape.rows.size() == 2,
          "unique index merge did not return two rows");
  Require(FieldValue(merged, "note", 0) == "unique-updated",
          "unique index merge first returning row was not update");
  Require(FieldValue(merged, "note", 1) == "unique-inserted",
          "unique index merge second returning row was not insert");
  Require(HasEvidence(merged, "merge_target_access_kind", "unique_index_lookup"),
          "unique index merge access evidence missing");
  Require(EvidenceContains(merged, "merge_target_access_plan", kUniqueIdIndexUuid),
          "unique index merge serialized plan missing index uuid");
  Require(HasEvidence(merged,
                      "mga_secondary_index_lookup_path",
                      "unique_synchronous_bypass"),
          "unique index merge missing MGA indexed lookup evidence");
  Require(HasEvidence(merged, "merge_unique_conflict_proof", "index_backed"),
          "unique index conflict proof evidence missing");
  Require(HasEvidence(merged, "merge_repeated_full_scan", "false"),
          "unique index merge repeated scan evidence missing");
  Require(HasEvidence(merged, "merge_action_execution", "action_batches"),
          "unique index merge batched execution evidence missing");
  Require(HasEvidence(merged, "merge_update_batch_count", "1"),
          "unique index merge update batch count missing");
  Require(HasEvidence(merged, "merge_insert_batch_count", "1"),
          "unique index merge insert batch count missing");
  Require(HasEvidence(merged, "update_target_access_kind", "row_uuid_list"),
          "unique index merge update batch did not plan row_uuid_list access");
  Require(HasEvidence(merged, "update_row_candidate_stream", "row_uuid_list"),
          "unique index merge update batch did not consume row_uuid_list stream");
  Require(HasNoEvidence(merged, "update_row_candidate_stream", "table_scan"),
          "unique index merge update batch fell back to table scan");
  Require(EvidenceContains(merged, "merge_target_access_plan_evidence",
                           "hot_point_lookup_cache_admission=deferred_until_successful_row_locator"),
          "unique index merge cache admission was not deferred until lookup success");
  Require(EvidenceContains(merged, "merge_hot_point_lookup_cache",
                           "hot_point_lookup_cache_actual_row_locator"),
          "unique index merge did not admit actual successful row locator");
  RequireCommonMergeEvidence(merged);
}

void VerifyDeleteBranch(const api::EngineRequestContext& context) {
  api::EngineMergeRowsRequest request;
  request.context = context;
  request.target_table.uuid.canonical = kTableUuid;
  request.target_table.object_kind = "table";
  request.match_predicate.predicate_kind = "row_uuid_match";
  request.update_when_matched = false;
  request.insert_when_not_matched = false;
  request.input_rows.push_back(Row(kSeedRow, "1", "delete-matched"));
  request.option_envelopes.push_back("delete_when_matched:true");

  auto deleted = api::EngineMergeRows(request);
  Require(deleted.ok, "delete-branch MERGE failed");
  Require(deleted.matched_count == 1, "delete-branch MERGE matched count mismatch");
  Require(deleted.merged_count == 1, "delete-branch MERGE merged count mismatch");
  Require(deleted.result_shape.rows.size() == 1,
          "delete-branch MERGE did not return deleted row");
  Require(HasEvidence(deleted, "merge_action", "delete_batch"),
          "delete-branch MERGE batch evidence missing");
  Require(HasEvidence(deleted, "merge_delete_batch_count", "1"),
          "delete-branch MERGE delete batch count missing");
  Require(HasEvidence(deleted, "delete_target_access_kind", "row_uuid_list"),
          "delete-branch MERGE did not plan row_uuid_list access");
  Require(HasEvidence(deleted, "delete_row_candidate_stream", "row_uuid_list"),
          "delete-branch MERGE did not consume row_uuid_list stream");
  Require(HasNoEvidence(deleted, "delete_row_candidate_stream", "table_scan"),
          "delete-branch MERGE fell back to table scan");
  Require(HasEvidence(deleted, "mga_finality_authority",
                      "engine_transaction_inventory"),
          "delete-branch MERGE finality evidence missing");
  Require(HasEvidence(deleted, "parser_or_reference_authority", "false"),
          "delete-branch MERGE parser/reference evidence missing");
}

void VerifyRollbackMergeNotVisible(const std::filesystem::path& database_path) {
  auto rollback_writer = BeginTransaction(database_path, "301");
  api::EngineMergeRowsRequest merge_insert;
  merge_insert.context = rollback_writer;
  merge_insert.target_table.uuid.canonical = kTableUuid;
  merge_insert.target_table.object_kind = "table";
  merge_insert.match_predicate.predicate_kind = "row_uuid_match";
  merge_insert.input_rows.push_back(Row(kRollbackRow, "rollback", "rollback-merge"));
  auto rolled_back_merge = api::EngineMergeRows(merge_insert);
  Require(rolled_back_merge.ok, "rollback MERGE insert failed before rollback");
  Require(rolled_back_merge.inserted_count == 1,
          "rollback MERGE did not insert test row");
  Rollback(rollback_writer);

  auto reader = BeginTransaction(database_path, "302");
  api::EngineMergeRowsRequest probe;
  probe.context = reader;
  probe.target_table.uuid.canonical = kTableUuid;
  probe.target_table.object_kind = "table";
  probe.match_predicate.predicate_kind = "row_uuid_match";
  probe.update_when_matched = true;
  probe.insert_when_not_matched = false;
  probe.input_rows.push_back(Row(kRollbackRow, "rollback", "should-not-update"));
  probe.update_assignments.push_back({"note", TextValue("should-not-update")});
  auto visibility_probe = api::EngineMergeRows(probe);
  Require(visibility_probe.ok, "rollback visibility probe MERGE failed");
  Require(visibility_probe.matched_count == 0,
          "rolled-back MERGE row became visible to new transaction");
  Require(visibility_probe.merged_count == 0,
          "rolled-back MERGE row was updated in new transaction");
  Require(HasEvidence(visibility_probe, "merge_matched_source_rows", "0"),
          "rollback visibility probe missing zero matched evidence");
  Commit(reader);
}

void VerifyRollbackDeleteBranchReopens(const std::filesystem::path& database_path) {
  auto seeder = BeginTransaction(database_path, "303");
  api::EngineInsertRowsRequest insert;
  insert.context = seeder;
  insert.target_table.uuid.canonical = kTableUuid;
  insert.target_table.object_kind = "table";
  insert.input_rows.push_back(Row(kRollbackDeleteRow,
                                  "rollback-delete",
                                  "rollback-delete-seed"));
  auto inserted = api::EngineInsertRows(insert);
  Require(inserted.ok, "rollback delete seed insert failed");
  Commit(seeder);

  auto deleter = BeginTransaction(database_path, "304");
  api::EngineMergeRowsRequest delete_merge;
  delete_merge.context = deleter;
  delete_merge.target_table.uuid.canonical = kTableUuid;
  delete_merge.target_table.object_kind = "table";
  delete_merge.match_predicate.predicate_kind = "row_uuid_match";
  delete_merge.update_when_matched = false;
  delete_merge.insert_when_not_matched = false;
  delete_merge.option_envelopes.push_back("delete_when_matched:true");
  delete_merge.input_rows.push_back(Row(kRollbackDeleteRow,
                                        "rollback-delete",
                                        "rollback-delete-tombstone"));
  auto deleted = api::EngineMergeRows(delete_merge);
  Require(deleted.ok, "rollback delete MERGE failed before rollback");
  Require(deleted.merged_count == 1,
          "rollback delete MERGE did not delete test row");
  Rollback(deleter);

  auto reader = BeginTransaction(database_path, "305");
  api::EngineMergeRowsRequest probe;
  probe.context = reader;
  probe.target_table.uuid.canonical = kTableUuid;
  probe.target_table.object_kind = "table";
  probe.match_predicate.predicate_kind = "row_uuid_match";
  probe.update_when_matched = true;
  probe.insert_when_not_matched = false;
  probe.input_rows.push_back(Row(kRollbackDeleteRow,
                                 "rollback-delete",
                                 "rollback-delete-visible"));
  probe.update_assignments.push_back({"note", TextValue("rollback-delete-visible")});
  auto visibility_probe = api::EngineMergeRows(probe);
  Require(visibility_probe.ok, "rollback delete visibility probe failed");
  Require(visibility_probe.matched_count == 1,
          "rolled-back delete did not reopen row to new transaction");
  Require(visibility_probe.updated_count == 1,
          "rolled-back delete row could not be updated after rollback");
  Commit(reader);
}

}  // namespace

int main() {
  ConfigureMemoryFixture();
  const auto work = MakeTempDir();
  Require(!work.empty(), "failed to create temp directory");
  const auto database_path = work / "odfr020.sbdb";

  api::EngineCreateLifecycleRequest create;
  create.context = BaseContext(database_path);
  create.option_envelopes.push_back(std::string("resource_seed_pack_root:") +
                                    SB_ODFR020_SEED_PACK_ROOT);
  auto created = api::EngineCreateLifecycle(create);
  Require(created.ok, "lifecycle create database failed");
  CreateSchemaAndTable(database_path);

  auto writer = BeginTransaction(database_path, "201");
  InsertSeedRow(writer);
  VerifyRowUuidMerge(writer);
  VerifyUniqueIndexMerge(writer);
  VerifyDeleteBranch(writer);
  Commit(writer);
  VerifyRollbackMergeNotVisible(database_path);
  VerifyRollbackDeleteBranchReopens(database_path);

  std::cout << "sbsql_merge_multi_action_odfr_020_conformance=passed\n";
  return EXIT_SUCCESS;
}
