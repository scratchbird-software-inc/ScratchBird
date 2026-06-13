// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "ddl/create_api.hpp"
#include "dml/delete_api.hpp"
#include "dml/insert_api.hpp"
#include "dml/select_api.hpp"
#include "dml/update_api.hpp"
#include "database_lifecycle.hpp"
#include "observability/metrics_api.hpp"
#include "transaction/transaction_inspect_api.hpp"
#include "transaction/transaction_api.hpp"
#include "uuid.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

using namespace scratchbird::engine::internal_api;

namespace {

struct Args {
  std::string path;
  std::uint64_t creation_millis = 0;
  bool overwrite = false;
};

bool ParseArgs(int argc, char** argv, Args* args) {
  for (int i = 1; i < argc; ++i) {
    const std::string key = argv[i];
    if (key == "--overwrite") {
      args->overwrite = true;
      continue;
    }
    if (i + 1 >= argc) { return false; }
    const std::string value = argv[++i];
    if (key == "--path") { args->path = value; }
    else if (key == "--creation-ms") { args->creation_millis = static_cast<std::uint64_t>(std::stoull(value)); }
    else { return false; }
  }
  return !args->path.empty() && args->creation_millis != 0;
}

bool HasDiagnostic(const EngineApiResult& result, const std::string& code) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.code == code) { return true; }
  }
  return false;
}

EngineRequestContext BaseContext(const Args& args) {
  EngineRequestContext context;
  context.trust_mode = EngineTrustMode::embedded_in_process;
  context.security_context_present = true;
  context.request_id = "engine-api-transaction-semantics-probe";
  context.database_path = args.path;
  const auto database_uuid =
      scratchbird::core::uuid::GenerateEngineIdentityV7(scratchbird::core::platform::UuidKind::database,
                                                        args.creation_millis + 10);
  const auto principal_uuid =
      scratchbird::core::uuid::GenerateEngineIdentityV7(scratchbird::core::platform::UuidKind::principal,
                                                        args.creation_millis + 12);
  if (database_uuid.ok()) {
    context.database_uuid.canonical = scratchbird::core::uuid::UuidToString(database_uuid.value.value);
  }
  if (principal_uuid.ok()) {
    context.principal_uuid.canonical = scratchbird::core::uuid::UuidToString(principal_uuid.value.value);
  }
  context.session_uuid.canonical = "018f0000-0000-7000-8000-00000000feed";
  context.catalog_generation_id = 1;
  context.security_epoch = 1;
  context.resource_epoch = 1;
  context.name_resolution_epoch = 1;
  return context;
}

bool CreateProbeDatabase(const Args& args) {
  if (args.overwrite) {
    std::filesystem::remove(args.path + ".sb.mga_row_versions");
    std::filesystem::remove(args.path + ".sb.mga_relation_metadata");
    std::filesystem::remove(args.path + ".sb.mga_index_entries");
    std::filesystem::remove(args.path + ".sb.mga_large_values");
    std::filesystem::remove(args.path + ".sb.mga_savepoints");
    std::filesystem::remove(args.path + ".sb.mga_relation_descriptors");
  }
  const auto database_uuid =
      scratchbird::core::uuid::GenerateEngineIdentityV7(scratchbird::core::platform::UuidKind::database,
                                                        args.creation_millis + 10);
  const auto filespace_uuid =
      scratchbird::core::uuid::GenerateEngineIdentityV7(scratchbird::core::platform::UuidKind::filespace,
                                                        args.creation_millis + 11);
  if (!database_uuid.ok()) {
    std::cerr << database_uuid.diagnostic.diagnostic_code << ":" << database_uuid.diagnostic.message_key << "\n";
    return false;
  }
  if (!filespace_uuid.ok()) {
    std::cerr << filespace_uuid.diagnostic.diagnostic_code << ":" << filespace_uuid.diagnostic.message_key << "\n";
    return false;
  }
  scratchbird::storage::database::DatabaseCreateConfig create;
  create.path = args.path;
  create.database_uuid = database_uuid.value;
  create.filespace_uuid = filespace_uuid.value;
  create.page_size = 16384;
  create.creation_unix_epoch_millis = args.creation_millis;
  create.allow_minimal_resource_bootstrap = true;
  create.require_resource_seed_pack = false;
  create.allow_overwrite = args.overwrite;
  const auto created = scratchbird::storage::database::CreateDatabaseFile(create);
  if (!created.ok()) {
    std::cerr << created.diagnostic.diagnostic_code << ":" << created.diagnostic.message_key << "\n";
    return false;
  }
  return true;
}

EngineRequestContext TxContext(EngineRequestContext base, const EngineBeginTransactionResult& tx) {
  base.local_transaction_id = tx.local_transaction_id;
  base.transaction_uuid = tx.transaction_uuid;
  base.transaction_isolation_level = tx.isolation_level;
  base.snapshot_visible_through_local_transaction_id = tx.snapshot_visible_through_local_transaction_id;
  return base;
}

EngineBeginTransactionResult Begin(const EngineRequestContext& base, std::string isolation = "read_committed") {
  EngineBeginTransactionRequest request;
  request.context = base;
  request.isolation_level = std::move(isolation);
  return EngineBeginTransaction(request);
}

EngineCommitTransactionResult CommitResult(const EngineRequestContext& tx_context) {
  EngineCommitTransactionRequest request;
  request.context = tx_context;
  return EngineCommitTransaction(request);
}

EngineRollbackTransactionResult RollbackResult(const EngineRequestContext& tx_context) {
  EngineRollbackTransactionRequest request;
  request.context = tx_context;
  return EngineRollbackTransaction(request);
}

bool Commit(const EngineRequestContext& tx_context) {
  return CommitResult(tx_context).ok;
}

bool Rollback(const EngineRequestContext& tx_context) {
  return RollbackResult(tx_context).ok;
}

EngineColumnDefinition Column(std::string name, std::string type, std::uint32_t ordinal) {
  EngineColumnDefinition column;
  column.ordinal = ordinal;
  column.names.push_back({"en", "default", name, name, true});
  column.descriptor.canonical_type_name = type;
  return column;
}

EngineTypedValue Value(std::string value) {
  EngineTypedValue typed;
  typed.encoded_value = std::move(value);
  typed.is_null = false;
  return typed;
}

EngineRowValue PersonRow(std::string id, std::string name, std::string age) {
  EngineRowValue row;
  row.fields.push_back({"id", Value(std::move(id))});
  row.fields.push_back({"name", Value(std::move(name))});
  row.fields.push_back({"age", Value(std::move(age))});
  return row;
}

std::string FieldValue(const EngineRowValue& row, const std::string& field) {
  for (const auto& [name, value] : row.fields) {
    if (name == field) { return value.encoded_value; }
  }
  return {};
}

bool HasRowName(const EngineSelectRowsResult& result, const std::string& expected_name) {
  if (!result.ok) { return false; }
  for (const auto& row : result.result_shape.rows) {
    if (FieldValue(row, "name") == expected_name) { return true; }
  }
  return false;
}

bool HasOnlyRowName(const EngineSelectRowsResult& result, const std::string& expected_name) {
  return result.ok && result.visible_count == 1 && result.result_shape.rows.size() == 1 &&
         FieldValue(result.result_shape.rows.front(), "name") == expected_name;
}

EngineSelectRowsResult SelectAll(const EngineRequestContext& tx_context, const EngineObjectReference& table) {
  EngineSelectRowsRequest request;
  request.context = tx_context;
  request.source_object = table;
  return EngineSelectRows(request);
}

EngineSelectRowsResult SelectRow(const EngineRequestContext& tx_context,
                                 const EngineObjectReference& table,
                                 const std::string& row_uuid) {
  EngineSelectRowsRequest request;
  request.context = tx_context;
  request.source_object = table;
  request.predicate.predicate_kind = "row_uuid_match";
  request.predicate.canonical_predicate_envelope = row_uuid;
  return EngineSelectRows(request);
}

EngineInsertRowsResult InsertPerson(const EngineRequestContext& tx_context,
                                    const EngineObjectReference& table,
                                    std::string id,
                                    std::string name,
                                    std::string age) {
  EngineInsertRowsRequest request;
  request.context = tx_context;
  request.target_table = table;
  request.input_rows.push_back(PersonRow(std::move(id), std::move(name), std::move(age)));
  return EngineInsertRows(request);
}

EngineUpdateRowsResult UpdateName(const EngineRequestContext& tx_context,
                                  const EngineObjectReference& table,
                                  const std::string& row_uuid,
                                  std::string name) {
  EngineUpdateRowsRequest request;
  request.context = tx_context;
  request.target_table = table;
  request.update_predicate.predicate_kind = "row_uuid_match";
  request.update_predicate.canonical_predicate_envelope = row_uuid;
  request.assignments.push_back({"name", Value(std::move(name))});
  return EngineUpdateRows(request);
}

EngineDeleteRowsResult DeleteRow(const EngineRequestContext& tx_context,
                                 const EngineObjectReference& table,
                                 const std::string& row_uuid) {
  EngineDeleteRowsRequest request;
  request.context = tx_context;
  request.target_table = table;
  request.delete_predicate.predicate_kind = "row_uuid_match";
  request.delete_predicate.canonical_predicate_envelope = row_uuid;
  return EngineDeleteRows(request);
}

EngineCreateTableResult CreatePersonTable(const EngineRequestContext& tx_context) {
  EngineCreateTableRequest request;
  request.context = tx_context;
  request.table_names.push_back({"en", "default", "person", "person", true});
  request.table_columns.push_back(Column("id", "text", 1));
  request.table_columns.push_back(Column("name", "text", 2));
  request.table_columns.push_back(Column("age", "int32", 3));
  return EngineCreateTable(request);
}

EngineCreateTableResult CreateTemporaryPersonTable(const EngineRequestContext& tx_context,
                                                   std::string name,
                                                   std::string on_commit) {
  EngineCreateTableRequest request;
  request.context = tx_context;
  request.table_names.push_back({"en", "default", name, name, true});
  request.table_columns.push_back(Column("id", "text", 1));
  request.table_columns.push_back(Column("name", "text", 2));
  request.table_columns.push_back(Column("age", "int32", 3));
  request.option_envelopes.push_back("temporary:true");
  request.option_envelopes.push_back(std::move(on_commit));
  return EngineCreateTable(request);
}

bool InvalidCrudWithoutTransaction(const EngineRequestContext& base, const EngineObjectReference& table) {
  EngineCreateTableRequest create;
  create.context = base;
  create.table_columns.push_back(Column("id", "text", 1));

  EngineInsertRowsRequest insert;
  insert.context = base;
  insert.target_table = table;
  insert.input_rows.push_back(PersonRow("x", "invalid", "0"));

  EngineSelectRowsRequest select;
  select.context = base;
  select.source_object = table;

  EngineUpdateRowsRequest update;
  update.context = base;
  update.target_table = table;
  update.update_predicate.predicate_kind = "row_uuid_match";
  update.update_predicate.canonical_predicate_envelope = "missing";
  update.assignments.push_back({"name", Value("invalid")});

  EngineDeleteRowsRequest del;
  del.context = base;
  del.target_table = table;
  del.delete_predicate.predicate_kind = "row_uuid_match";
  del.delete_predicate.canonical_predicate_envelope = "missing";

  return HasDiagnostic(EngineCreateTable(create), "SB_ENGINE_API_INVALID_REQUEST") &&
         HasDiagnostic(EngineInsertRows(insert), "SB_ENGINE_API_INVALID_REQUEST") &&
         HasDiagnostic(EngineSelectRows(select), "SB_ENGINE_API_INVALID_REQUEST") &&
         HasDiagnostic(EngineUpdateRows(update), "SB_ENGINE_API_INVALID_REQUEST") &&
         HasDiagnostic(EngineDeleteRows(del), "SB_ENGINE_API_INVALID_REQUEST");
}

bool InvalidCommitRollbackWithoutTransaction(const EngineRequestContext& base) {
  EngineCommitTransactionRequest commit;
  commit.context = base;
  EngineRollbackTransactionRequest rollback;
  rollback.context = base;
  return HasDiagnostic(EngineCommitTransaction(commit), "SB_ENGINE_API_INVALID_REQUEST") &&
         HasDiagnostic(EngineRollbackTransaction(rollback), "SB_ENGINE_API_INVALID_REQUEST");
}

bool FileContainsText(const std::string& path, const std::string& needle) {
  std::ifstream input(path, std::ios::binary);
  if (!input) { return false; }
  const std::string body((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
  return body.find(needle) != std::string::npos;
}

}  // namespace

int main(int argc, char** argv) {
  Args args;
  if (!ParseArgs(argc, argv, &args)) {
    std::cerr << "usage: sb_engine_api_transaction_semantics_probe --path PATH --creation-ms MILLIS [--overwrite]\n";
    return 2;
  }
  if (!CreateProbeDatabase(args)) {
    return 1;
  }

  const auto base = BaseContext(args);

  const auto setup_tx = Begin(base);
  const auto setup_context = TxContext(base, setup_tx);
  const auto table_result = CreatePersonTable(setup_context);
  const auto table = table_result.table_object;
  const auto insert_ada = InsertPerson(setup_context, table, "1", "Ada", "37");
  const std::string ada_row_uuid = insert_ada.row_uuids.empty() ? std::string{} : insert_ada.row_uuids.front().canonical;
  const bool read_your_writes_insert = table_result.ok && insert_ada.ok && HasOnlyRowName(SelectRow(setup_context, table, ada_row_uuid), "Ada");
  const auto setup_commit_result = CommitResult(setup_context);

  const auto committed_read_tx = Begin(base);
  const auto committed_read_context = TxContext(base, committed_read_tx);
  const bool commit_visibility_after_reopen = HasOnlyRowName(SelectRow(committed_read_context, table, ada_row_uuid), "Ada");
  const bool committed_read_commit = Commit(committed_read_context);

  const auto snapshot_seed_tx = Begin(base);
  const auto snapshot_seed_context = TxContext(base, snapshot_seed_tx);
  const auto snapshot_seed_insert = InsertPerson(snapshot_seed_context, table, "snap", "SnapshotBase", "10");
  const std::string snapshot_row_uuid =
      snapshot_seed_insert.row_uuids.empty() ? std::string{} : snapshot_seed_insert.row_uuids.front().canonical;
  const bool snapshot_seed_commit = snapshot_seed_insert.ok && Commit(snapshot_seed_context);

  const auto snapshot_reader_tx = Begin(base, "snapshot");
  const auto snapshot_reader_context = TxContext(base, snapshot_reader_tx);
  const auto snapshot_writer_tx = Begin(base);
  const auto snapshot_writer_context = TxContext(base, snapshot_writer_tx);
  const auto snapshot_writer_update = UpdateName(snapshot_writer_context, table, snapshot_row_uuid, "SnapshotNew");
  const bool snapshot_writer_commit = snapshot_writer_update.ok && Commit(snapshot_writer_context);
  const bool snapshot_reader_stable =
      snapshot_seed_commit && snapshot_writer_commit &&
      HasOnlyRowName(SelectRow(snapshot_reader_context, table, snapshot_row_uuid), "SnapshotBase");
  const bool snapshot_reader_commit = Commit(snapshot_reader_context);
  const auto read_committed_after_snapshot_tx = Begin(base);
  const auto read_committed_after_snapshot_context = TxContext(base, read_committed_after_snapshot_tx);
  const bool read_committed_sees_new_snapshot_version =
      HasOnlyRowName(SelectRow(read_committed_after_snapshot_context, table, snapshot_row_uuid), "SnapshotNew");
  const bool read_committed_after_snapshot_commit = Commit(read_committed_after_snapshot_context);

  const auto rollback_insert_tx = Begin(base);
  const auto rollback_insert_context = TxContext(base, rollback_insert_tx);
  const auto insert_rollback = InsertPerson(rollback_insert_context, table, "2", "RollbackInsert", "1");
  const std::string rollback_insert_uuid = insert_rollback.row_uuids.empty() ? std::string{} : insert_rollback.row_uuids.front().canonical;
  const bool rollback_insert_seen_in_tx = HasOnlyRowName(SelectRow(rollback_insert_context, table, rollback_insert_uuid), "RollbackInsert");
  const bool rollback_insert_done = Rollback(rollback_insert_context);
  const auto rollback_insert_read_tx = Begin(base);
  const auto rollback_insert_read_context = TxContext(base, rollback_insert_read_tx);
  const bool rollback_insert_invisible = rollback_insert_seen_in_tx && SelectRow(rollback_insert_read_context, table, rollback_insert_uuid).visible_count == 0;
  const bool rollback_insert_read_commit = Commit(rollback_insert_read_context);

  const auto rollback_update_tx = Begin(base);
  const auto rollback_update_context = TxContext(base, rollback_update_tx);
  const auto update_temp = UpdateName(rollback_update_context, table, ada_row_uuid, "TempUpdate");
  const bool read_your_writes_update = update_temp.ok && update_temp.updated_count == 1 && HasOnlyRowName(SelectRow(rollback_update_context, table, ada_row_uuid), "TempUpdate");
  const bool rollback_update_done = Rollback(rollback_update_context);
  const auto rollback_update_read_tx = Begin(base);
  const auto rollback_update_read_context = TxContext(base, rollback_update_read_tx);
  const bool rollback_update_restores_prior = HasOnlyRowName(SelectRow(rollback_update_read_context, table, ada_row_uuid), "Ada");
  const bool rollback_update_read_commit = Commit(rollback_update_read_context);

  const auto multi_update_tx = Begin(base);
  const auto multi_update_context = TxContext(base, multi_update_tx);
  const auto update_multi_1 = UpdateName(multi_update_context, table, ada_row_uuid, "MultiOne");
  const auto update_multi_2 = UpdateName(multi_update_context, table, ada_row_uuid, "MultiTwo");
  const bool multiple_update_latest_version = update_multi_1.ok && update_multi_2.ok && HasOnlyRowName(SelectRow(multi_update_context, table, ada_row_uuid), "MultiTwo");
  const bool multi_update_commit = Commit(multi_update_context);
  const auto committed_update_read_tx = Begin(base);
  const auto committed_update_read_context = TxContext(base, committed_update_read_tx);
  const bool committed_update_reopen = HasOnlyRowName(SelectRow(committed_update_read_context, table, ada_row_uuid), "MultiTwo");
  const bool committed_update_read_commit = Commit(committed_update_read_context);

  const auto rollback_delete_tx = Begin(base);
  const auto rollback_delete_context = TxContext(base, rollback_delete_tx);
  const auto delete_temp = DeleteRow(rollback_delete_context, table, ada_row_uuid);
  const bool read_your_writes_delete = delete_temp.ok && delete_temp.deleted_count == 1 && SelectRow(rollback_delete_context, table, ada_row_uuid).visible_count == 0;
  const bool rollback_delete_done = Rollback(rollback_delete_context);
  const auto rollback_delete_read_tx = Begin(base);
  const auto rollback_delete_read_context = TxContext(base, rollback_delete_read_tx);
  const bool rollback_delete_restores_prior = HasOnlyRowName(SelectRow(rollback_delete_read_context, table, ada_row_uuid), "MultiTwo");
  const bool rollback_delete_read_commit = Commit(rollback_delete_read_context);

  const auto isolation_insert_a_tx = Begin(base);
  const auto isolation_insert_a_context = TxContext(base, isolation_insert_a_tx);
  const auto isolation_insert = InsertPerson(isolation_insert_a_context, table, "3", "UncommittedInsert", "3");
  const std::string isolation_insert_uuid = isolation_insert.row_uuids.empty() ? std::string{} : isolation_insert.row_uuids.front().canonical;
  const auto isolation_insert_b_tx = Begin(base);
  const auto isolation_insert_b_context = TxContext(base, isolation_insert_b_tx);
  const bool uncommitted_insert_isolation = SelectRow(isolation_insert_b_context, table, isolation_insert_uuid).visible_count == 0;
  const bool isolation_insert_b_commit = Commit(isolation_insert_b_context);
  const bool isolation_insert_a_rollback = Rollback(isolation_insert_a_context);

  const auto isolation_update_a_tx = Begin(base);
  const auto isolation_update_a_context = TxContext(base, isolation_update_a_tx);
  const auto isolation_update = UpdateName(isolation_update_a_context, table, ada_row_uuid, "UncommittedUpdate");
  const auto isolation_update_b_tx = Begin(base);
  const auto isolation_update_b_context = TxContext(base, isolation_update_b_tx);
  const bool uncommitted_update_isolation = isolation_update.ok && HasOnlyRowName(SelectRow(isolation_update_b_context, table, ada_row_uuid), "MultiTwo");
  const bool isolation_update_b_commit = Commit(isolation_update_b_context);
  const bool isolation_update_a_rollback = Rollback(isolation_update_a_context);

  const auto isolation_delete_a_tx = Begin(base);
  const auto isolation_delete_a_context = TxContext(base, isolation_delete_a_tx);
  const auto isolation_delete = DeleteRow(isolation_delete_a_context, table, ada_row_uuid);
  const auto isolation_delete_b_tx = Begin(base);
  const auto isolation_delete_b_context = TxContext(base, isolation_delete_b_tx);
  const bool uncommitted_delete_isolation = isolation_delete.ok && HasOnlyRowName(SelectRow(isolation_delete_b_context, table, ada_row_uuid), "MultiTwo");
  const bool isolation_delete_b_commit = Commit(isolation_delete_b_context);
  const bool isolation_delete_a_rollback = Rollback(isolation_delete_a_context);

  const bool invalid_crud_without_transaction = InvalidCrudWithoutTransaction(base, table);
  const bool invalid_commit_rollback_without_transaction = InvalidCommitRollbackWithoutTransaction(base);
  const bool transaction_evidence = setup_commit_result.ok && !setup_commit_result.evidence.empty() &&
                                    RollbackResult(TxContext(base, Begin(base))).ok;

  auto inspect_base = base;
  inspect_base.trace_tags.push_back("security.fixture_trace_authority");
  inspect_base.trace_tags.push_back("group:DBA");
  inspect_base.trace_tags.push_back("right:MGA_LINEAGE_INSPECT");
  inspect_base.trace_tags.push_back("right:MGA_RECOVERY_INSPECT");
  inspect_base.trace_tags.push_back("right:MGA_METRICS_READ");
  inspect_base.trace_tags.push_back("right:OBS_METRICS_READ_FAMILY");
  inspect_base.catalog_generation_id = 42;

  EngineInspectTransactionLineageRequest lineage_request;
  lineage_request.context = inspect_base;
  lineage_request.option_envelopes.push_back("schema_epoch:42");
  lineage_request.option_envelopes.push_back("snapshot_capsule:probe_snapshot");
  const auto lineage_result = EngineInspectTransactionLineage(lineage_request);
  const bool lineage_evidence_visible = lineage_result.ok && !lineage_result.result_shape.rows.empty();

  EngineClassifyTransactionRestoreRequest restore_request;
  restore_request.context = inspect_base;
  restore_request.option_envelopes.push_back("schema_epoch:42");
  restore_request.option_envelopes.push_back("snapshot_capsule:probe_snapshot");
  const auto restore_result = EngineClassifyTransactionRestore(restore_request);
  const bool restore_classification_visible = restore_result.ok && restore_result.restore_allowed &&
                                              !restore_result.wal_required &&
                                              !restore_result.result_shape.rows.empty();

  EngineClassifyTransactionRestoreRequest wal_restore_request = restore_request;
  wal_restore_request.option_envelopes.push_back("wal_required:true");
  const bool wal_restore_refused = HasDiagnostic(EngineClassifyTransactionRestore(wal_restore_request),
                                                "SB-MGA-WAL-NOT-AUTHORITY");

  EngineInspectTransactionLineageRequest unauthorized_lineage_request;
  unauthorized_lineage_request.context = base;
  const bool lineage_right_enforced = HasDiagnostic(EngineInspectTransactionLineage(unauthorized_lineage_request),
                                                   "SECURITY.AUTHORIZATION.DENIED");

  EngineSysMetricsRegistryRequest metrics_registry_request;
  metrics_registry_request.context = inspect_base;
  metrics_registry_request.option_envelopes.push_back("family:sb_mga_row_versions_reclaimed_total");
  const auto metrics_registry_result = EngineSysMetricsRegistry(metrics_registry_request);
  const bool mga_metrics_registered = metrics_registry_result.ok && !metrics_registry_result.result_shape.rows.empty();
  const bool row_version_chain_events_present =
      FileContainsText(args.path + ".sb.mga_row_versions", "SBMGA1\tROW_VERSION\t");

  const auto temp_delete_tx = Begin(base);
  const auto temp_delete_context = TxContext(base, temp_delete_tx);
  const auto temp_delete_table_result = CreateTemporaryPersonTable(temp_delete_context,
                                                                  "temp_delete_rows",
                                                                  "on_commit:delete_rows");
  const auto temp_delete_insert = InsertPerson(temp_delete_context,
                                              temp_delete_table_result.table_object,
                                              "td",
                                              "TempDelete",
                                              "1");
  const std::string temp_delete_row_uuid =
      temp_delete_insert.row_uuids.empty() ? std::string{} : temp_delete_insert.row_uuids.front().canonical;
  const bool temp_delete_visible_before_commit =
      temp_delete_table_result.ok && temp_delete_insert.ok &&
      HasOnlyRowName(SelectRow(temp_delete_context, temp_delete_table_result.table_object, temp_delete_row_uuid),
                     "TempDelete");
  const bool temp_delete_commit = Commit(temp_delete_context);
  const auto temp_delete_read_tx = Begin(base);
  const auto temp_delete_read_context = TxContext(base, temp_delete_read_tx);
  const bool temp_delete_rows_removed_on_commit =
      temp_delete_visible_before_commit && temp_delete_commit &&
      SelectRow(temp_delete_read_context, temp_delete_table_result.table_object, temp_delete_row_uuid).visible_count == 0;
  const bool temp_delete_read_commit = Commit(temp_delete_read_context);

  const auto temp_preserve_tx = Begin(base);
  const auto temp_preserve_context = TxContext(base, temp_preserve_tx);
  const auto temp_preserve_table_result = CreateTemporaryPersonTable(temp_preserve_context,
                                                                    "temp_preserve_rows",
                                                                    "on_commit:preserve_rows");
  const auto temp_preserve_insert = InsertPerson(temp_preserve_context,
                                                temp_preserve_table_result.table_object,
                                                "tp",
                                                "TempPreserve",
                                                "2");
  const std::string temp_preserve_row_uuid =
      temp_preserve_insert.row_uuids.empty() ? std::string{} : temp_preserve_insert.row_uuids.front().canonical;
  const bool temp_preserve_commit = temp_preserve_table_result.ok && temp_preserve_insert.ok &&
                                    Commit(temp_preserve_context);
  const auto temp_preserve_read_tx = Begin(base);
  const auto temp_preserve_read_context = TxContext(base, temp_preserve_read_tx);
  const bool temp_preserve_rows_kept_on_commit =
      temp_preserve_commit &&
      HasOnlyRowName(SelectRow(temp_preserve_read_context,
                               temp_preserve_table_result.table_object,
                               temp_preserve_row_uuid),
                     "TempPreserve");
  const bool temp_preserve_read_commit = Commit(temp_preserve_read_context);

  auto other_session_base = base;
  other_session_base.session_uuid.canonical = "018f0000-0000-7000-8000-00000000beef";
  const auto other_session_tx = Begin(other_session_base);
  const auto other_session_context = TxContext(other_session_base, other_session_tx);
  const auto other_session_select = SelectRow(other_session_context,
                                             temp_preserve_table_result.table_object,
                                             temp_preserve_row_uuid);
  const bool temp_hidden_from_other_session = !other_session_select.ok || other_session_select.visible_count == 0;
  const bool other_session_commit = Commit(other_session_context);

  const auto final_delete_tx = Begin(base);
  const auto final_delete_context = TxContext(base, final_delete_tx);
  const auto final_delete = DeleteRow(final_delete_context, table, ada_row_uuid);
  const bool final_delete_commit = final_delete.ok && Commit(final_delete_context);
  const auto final_read_tx = Begin(base);
  const auto final_read_context = TxContext(base, final_read_tx);
  const bool committed_delete_tombstone_after_reopen = final_delete_commit && SelectRow(final_read_context, table, ada_row_uuid).visible_count == 0;
  const bool final_read_commit = Commit(final_read_context);

  const bool ok = read_your_writes_insert && commit_visibility_after_reopen && rollback_insert_invisible &&
                  read_your_writes_update && rollback_update_restores_prior && multiple_update_latest_version &&
                  committed_update_reopen && read_your_writes_delete && rollback_delete_restores_prior &&
                  committed_delete_tombstone_after_reopen && uncommitted_insert_isolation && uncommitted_update_isolation &&
                  uncommitted_delete_isolation && invalid_crud_without_transaction && invalid_commit_rollback_without_transaction &&
                  snapshot_reader_stable && snapshot_reader_commit && read_committed_sees_new_snapshot_version &&
                  read_committed_after_snapshot_commit && row_version_chain_events_present &&
                  transaction_evidence && setup_commit_result.ok && committed_read_commit && rollback_insert_done &&
                  rollback_insert_read_commit && rollback_update_done && rollback_update_read_commit && multi_update_commit &&
                  committed_update_read_commit && rollback_delete_done && rollback_delete_read_commit && isolation_insert_b_commit &&
                  isolation_insert_a_rollback && isolation_update_b_commit && isolation_update_a_rollback && isolation_delete_b_commit &&
                  isolation_delete_a_rollback && temp_delete_rows_removed_on_commit && temp_delete_read_commit &&
                  temp_preserve_rows_kept_on_commit && temp_preserve_read_commit && temp_hidden_from_other_session &&
                  other_session_commit && lineage_evidence_visible && restore_classification_visible &&
                  wal_restore_refused && lineage_right_enforced && mga_metrics_registered && final_read_commit;

  std::cout << "{\n";
  std::cout << "  \"ok\": " << (ok ? "true" : "false") << ",\n";
  std::cout << "  \"read_your_writes_insert\": " << (read_your_writes_insert ? "true" : "false") << ",\n";
  std::cout << "  \"commit_visibility_after_reopen\": " << (commit_visibility_after_reopen ? "true" : "false") << ",\n";
  std::cout << "  \"snapshot_reader_stable\": " << (snapshot_reader_stable ? "true" : "false") << ",\n";
  std::cout << "  \"read_committed_sees_new_snapshot_version\": " << (read_committed_sees_new_snapshot_version ? "true" : "false") << ",\n";
  std::cout << "  \"row_version_chain_events_present\": " << (row_version_chain_events_present ? "true" : "false") << ",\n";
  std::cout << "  \"rollback_insert_invisible\": " << (rollback_insert_invisible ? "true" : "false") << ",\n";
  std::cout << "  \"read_your_writes_update\": " << (read_your_writes_update ? "true" : "false") << ",\n";
  std::cout << "  \"rollback_update_restores_prior\": " << (rollback_update_restores_prior ? "true" : "false") << ",\n";
  std::cout << "  \"multiple_update_latest_version\": " << (multiple_update_latest_version ? "true" : "false") << ",\n";
  std::cout << "  \"committed_update_reopen\": " << (committed_update_reopen ? "true" : "false") << ",\n";
  std::cout << "  \"read_your_writes_delete\": " << (read_your_writes_delete ? "true" : "false") << ",\n";
  std::cout << "  \"rollback_delete_restores_prior\": " << (rollback_delete_restores_prior ? "true" : "false") << ",\n";
  std::cout << "  \"committed_delete_tombstone_after_reopen\": " << (committed_delete_tombstone_after_reopen ? "true" : "false") << ",\n";
  std::cout << "  \"uncommitted_insert_isolation\": " << (uncommitted_insert_isolation ? "true" : "false") << ",\n";
  std::cout << "  \"uncommitted_update_isolation\": " << (uncommitted_update_isolation ? "true" : "false") << ",\n";
  std::cout << "  \"uncommitted_delete_isolation\": " << (uncommitted_delete_isolation ? "true" : "false") << ",\n";
  std::cout << "  \"invalid_crud_without_transaction\": " << (invalid_crud_without_transaction ? "true" : "false") << ",\n";
  std::cout << "  \"invalid_commit_rollback_without_transaction\": " << (invalid_commit_rollback_without_transaction ? "true" : "false") << ",\n";
  std::cout << "  \"transaction_evidence\": " << (transaction_evidence ? "true" : "false") << ",\n";
  std::cout << "  \"temp_delete_rows_removed_on_commit\": " << (temp_delete_rows_removed_on_commit ? "true" : "false") << ",\n";
  std::cout << "  \"temp_preserve_rows_kept_on_commit\": " << (temp_preserve_rows_kept_on_commit ? "true" : "false") << ",\n";
  std::cout << "  \"temp_hidden_from_other_session\": " << (temp_hidden_from_other_session ? "true" : "false") << ",\n";
  std::cout << "  \"lineage_evidence_visible\": " << (lineage_evidence_visible ? "true" : "false") << ",\n";
  std::cout << "  \"restore_classification_visible\": " << (restore_classification_visible ? "true" : "false") << ",\n";
  std::cout << "  \"wal_restore_refused\": " << (wal_restore_refused ? "true" : "false") << ",\n";
  std::cout << "  \"lineage_right_enforced\": " << (lineage_right_enforced ? "true" : "false") << ",\n";
  std::cout << "  \"mga_metrics_registered\": " << (mga_metrics_registered ? "true" : "false") << "\n";
  std::cout << "}\n";
  return ok ? 0 : 1;
}
