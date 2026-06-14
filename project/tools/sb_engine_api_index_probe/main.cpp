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
#include "memory.hpp"
#include "transaction/transaction_api.hpp"
#include "uuid.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

using namespace scratchbird::engine::internal_api;

namespace {

struct Args {
  std::string path;
  std::string seed_pack_root;
  std::uint64_t creation_millis = 0;
  bool overwrite = false;
};

bool ParseArgs(int argc, char** argv, Args* args) {
  for (int i = 1; i < argc; ++i) {
    const std::string key = argv[i];
    if (key == "--overwrite") { args->overwrite = true; continue; }
    if (i + 1 >= argc) { return false; }
    const std::string value = argv[++i];
    if (key == "--path") { args->path = value; }
    else if (key == "--seed-pack-root") { args->seed_pack_root = value; }
    else if (key == "--creation-ms") { args->creation_millis = static_cast<std::uint64_t>(std::stoull(value)); }
    else { return false; }
  }
  return !args->path.empty() && !args->seed_pack_root.empty() && args->creation_millis != 0;
}

bool HasDiagnostic(const EngineApiResult& result, const std::string& code) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.code == code) { return true; }
  }
  return false;
}

bool HasIndexEvidence(const EngineApiResult& result) {
  for (const auto& evidence : result.evidence) {
    if (evidence.evidence_kind == "index_lookup" && !evidence.evidence_id.empty()) { return true; }
  }
  return false;
}

EngineRequestContext BaseContext(const Args& args) {
  EngineRequestContext context;
  context.trust_mode = EngineTrustMode::embedded_in_process;
  context.security_context_present = true;
  context.request_id = "engine-api-index-probe";
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
  context.session_uuid.canonical = "018f0000-0000-7000-8000-00000000c1de";
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
    std::cerr << database_uuid.diagnostic.diagnostic_code << ":"
              << database_uuid.diagnostic.message_key << "\n";
    return false;
  }
  if (!filespace_uuid.ok()) {
    std::cerr << filespace_uuid.diagnostic.diagnostic_code << ":"
              << filespace_uuid.diagnostic.message_key << "\n";
    return false;
  }
  scratchbird::storage::database::DatabaseCreateConfig create;
  create.path = args.path;
  create.database_uuid = database_uuid.value;
  create.filespace_uuid = filespace_uuid.value;
  create.page_size = 16384;
  create.creation_unix_epoch_millis = args.creation_millis;
  create.resource_seed_pack_root = args.seed_pack_root;
  create.require_resource_seed_pack = true;
  create.allow_overwrite = args.overwrite;
  const auto created = scratchbird::storage::database::CreateDatabaseFile(create);
  if (!created.ok()) {
    std::cerr << created.diagnostic.diagnostic_code << ":"
              << created.diagnostic.message_key << "\n";
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

EngineBeginTransactionResult Begin(const EngineRequestContext& base) {
  EngineBeginTransactionRequest request;
  request.context = base;
  request.isolation_level = "read_committed";
  return EngineBeginTransaction(request);
}

bool Commit(const EngineRequestContext& tx_context) {
  EngineCommitTransactionRequest request;
  request.context = tx_context;
  return EngineCommitTransaction(request).ok;
}

bool Rollback(const EngineRequestContext& tx_context) {
  EngineRollbackTransactionRequest request;
  request.context = tx_context;
  return EngineRollbackTransaction(request).ok;
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

bool HasOnlyName(const EngineSelectRowsResult& result, const std::string& name) {
  return result.ok && result.visible_count == 1 && result.result_shape.rows.size() == 1 &&
         FieldValue(result.result_shape.rows.front(), "name") == name;
}

bool HasZeroRows(const EngineSelectRowsResult& result) {
  return result.ok && result.visible_count == 0 && result.result_shape.rows.empty();
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

EngineCreateIndexResult CreateNameIndex(const EngineRequestContext& tx_context, const EngineObjectReference& table) {
  EngineCreateIndexRequest request;
  request.context = tx_context;
  request.target_object = table;
  EngineIndexDefinition index;
  index.index_kind = "rowstore_scalar_btree_v1";
  index.names.push_back({"en", "default", "ix_person_name", "ix_person_name", true});
  index.key_envelopes.push_back("name");
  request.indexes.push_back(index);
  return EngineCreateIndex(request);
}

EngineCreateTableResult CreateOpaqueTable(const EngineRequestContext& tx_context) {
  EngineCreateTableRequest request;
  request.context = tx_context;
  request.table_names.push_back({"en", "default", "opaque_items", "opaque_items", true});
  request.table_columns.push_back(Column("payload", "opaque_extension", 1));
  return EngineCreateTable(request);
}

EngineCreateIndexResult CreateOpaqueIndex(const EngineRequestContext& tx_context, const EngineObjectReference& table) {
  EngineCreateIndexRequest request;
  request.context = tx_context;
  request.target_object = table;
  EngineIndexDefinition index;
  index.index_kind = "rowstore_scalar_btree_v1";
  index.names.push_back({"en", "default", "ix_opaque_payload", "ix_opaque_payload", true});
  index.key_envelopes.push_back("payload");
  request.indexes.push_back(index);
  return EngineCreateIndex(request);
}

EngineInsertRowsResult InsertOpaquePayload(const EngineRequestContext& tx_context,
                                           const EngineObjectReference& table,
                                           std::string payload) {
  EngineInsertRowsRequest request;
  request.context = tx_context;
  request.target_table = table;
  EngineRowValue row;
  row.fields.push_back({"payload", Value(std::move(payload))});
  request.input_rows.push_back(std::move(row));
  return EngineInsertRows(request);
}

EngineSelectRowsResult SelectOpaquePayload(const EngineRequestContext& tx_context,
                                           const EngineObjectReference& table,
                                           std::string payload) {
  EngineSelectRowsRequest request;
  request.context = tx_context;
  request.source_object = table;
  request.predicate.predicate_kind = "column_equals";
  request.predicate.canonical_predicate_envelope = "payload";
  request.predicate.bound_values.push_back(Value(std::move(payload)));
  return EngineSelectRows(request);
}

EngineUpdateRowsResult UpdateOpaquePayload(const EngineRequestContext& tx_context,
                                           const EngineObjectReference& table,
                                           std::string payload) {
  EngineUpdateRowsRequest request;
  request.context = tx_context;
  request.target_table = table;
  request.assignments.push_back({"payload", Value(std::move(payload))});
  return EngineUpdateRows(request);
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

EngineSelectRowsResult SelectName(const EngineRequestContext& tx_context, const EngineObjectReference& table, std::string name) {
  EngineSelectRowsRequest request;
  request.context = tx_context;
  request.source_object = table;
  request.predicate.predicate_kind = "column_equals";
  request.predicate.canonical_predicate_envelope = "name";
  request.predicate.bound_values.push_back(Value(std::move(name)));
  return EngineSelectRows(request);
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

}  // namespace

int main(int argc, char** argv) {
  Args args;
  if (!ParseArgs(argc, argv, &args)) {
    std::cerr << "usage: sb_engine_api_index_probe --path PATH --seed-pack-root PATH --creation-ms MILLIS [--overwrite]\n";
    return 2;
  }
  auto memory_policy = scratchbird::core::memory::DefaultLocalEngineMemoryPolicy();
  memory_policy.policy_name = "sb_engine_api_index_probe";
  const auto memory_configured =
      scratchbird::core::memory::ConfigureDefaultMemoryManagerForFixture(
          memory_policy, "sb_engine_api_index_probe");
  if (!memory_configured.ok()) {
    std::cerr << memory_configured.diagnostic.diagnostic_code << ":"
              << memory_configured.diagnostic.message_key << "\n";
    return 1;
  }
  if (!CreateProbeDatabase(args)) return 1;

  const auto base = BaseContext(args);
  const auto setup_tx = Begin(base);
  const auto setup_context = TxContext(base, setup_tx);
  const auto table_result = CreatePersonTable(setup_context);
  const auto table = table_result.table_object;
  const auto index_result = CreateNameIndex(setup_context, table);
  const auto insert_ada = InsertPerson(setup_context, table, "1", "Ada", "37");
  const std::string row_uuid = insert_ada.row_uuids.empty() ? std::string{} : insert_ada.row_uuids.front().canonical;
  const auto select_ada_in_tx = SelectName(setup_context, table, "Ada");
  const bool create_insert_index_visible = table_result.ok && index_result.ok && insert_ada.ok && HasOnlyName(select_ada_in_tx, "Ada") && HasIndexEvidence(select_ada_in_tx);
  const bool setup_commit = Commit(setup_context);

  const auto committed_read_tx = Begin(base);
  const auto committed_read_context = TxContext(base, committed_read_tx);
  const auto select_ada_after_reopen = SelectName(committed_read_context, table, "Ada");
  const bool committed_index_reopen = HasOnlyName(select_ada_after_reopen, "Ada") && HasIndexEvidence(select_ada_after_reopen);
  const bool committed_read_commit = Commit(committed_read_context);

  const auto rollback_insert_tx = Begin(base);
  const auto rollback_insert_context = TxContext(base, rollback_insert_tx);
  const auto rollback_insert = InsertPerson(rollback_insert_context, table, "2", "RollbackInsert", "1");
  const bool rollback_insert_seen = HasOnlyName(SelectName(rollback_insert_context, table, "RollbackInsert"), "RollbackInsert");
  const bool rollback_insert_done = Rollback(rollback_insert_context);
  const auto rollback_insert_read_tx = Begin(base);
  const auto rollback_insert_read_context = TxContext(base, rollback_insert_read_tx);
  const bool rollback_insert_hidden = rollback_insert.ok && rollback_insert_seen && HasZeroRows(SelectName(rollback_insert_read_context, table, "RollbackInsert"));
  const bool rollback_insert_read_commit = Commit(rollback_insert_read_context);

  const auto rollback_update_tx = Begin(base);
  const auto rollback_update_context = TxContext(base, rollback_update_tx);
  const auto rollback_update = UpdateName(rollback_update_context, table, row_uuid, "GraceRollback");
  const auto select_grace_rollback = SelectName(rollback_update_context, table, "GraceRollback");
  const bool rollback_update_seen = rollback_update.ok && HasOnlyName(select_grace_rollback, "GraceRollback") && HasIndexEvidence(select_grace_rollback);
  const bool rollback_update_done = Rollback(rollback_update_context);
  const auto rollback_update_read_tx = Begin(base);
  const auto rollback_update_read_context = TxContext(base, rollback_update_read_tx);
  const bool rollback_update_restored = HasOnlyName(SelectName(rollback_update_read_context, table, "Ada"), "Ada") && HasZeroRows(SelectName(rollback_update_read_context, table, "GraceRollback"));
  const bool rollback_update_read_commit = Commit(rollback_update_read_context);

  const auto commit_update_tx = Begin(base);
  const auto commit_update_context = TxContext(base, commit_update_tx);
  const auto commit_update = UpdateName(commit_update_context, table, row_uuid, "Grace");
  const bool commit_update_seen = commit_update.ok && HasOnlyName(SelectName(commit_update_context, table, "Grace"), "Grace");
  const bool commit_update_done = Commit(commit_update_context);
  const auto commit_update_read_tx = Begin(base);
  const auto commit_update_read_context = TxContext(base, commit_update_read_tx);
  const auto select_grace = SelectName(commit_update_read_context, table, "Grace");
  const bool committed_update_index_reopen = commit_update_seen && HasOnlyName(select_grace, "Grace") && HasIndexEvidence(select_grace) && HasZeroRows(SelectName(commit_update_read_context, table, "Ada"));
  const bool commit_update_read_commit = Commit(commit_update_read_context);

  const auto rollback_delete_tx = Begin(base);
  const auto rollback_delete_context = TxContext(base, rollback_delete_tx);
  const auto rollback_delete = DeleteRow(rollback_delete_context, table, row_uuid);
  const bool rollback_delete_seen = rollback_delete.ok && HasZeroRows(SelectName(rollback_delete_context, table, "Grace"));
  const bool rollback_delete_done = Rollback(rollback_delete_context);
  const auto rollback_delete_read_tx = Begin(base);
  const auto rollback_delete_read_context = TxContext(base, rollback_delete_read_tx);
  const bool rollback_delete_restored = HasOnlyName(SelectName(rollback_delete_read_context, table, "Grace"), "Grace");
  const bool rollback_delete_read_commit = Commit(rollback_delete_read_context);

  const auto final_delete_tx = Begin(base);
  const auto final_delete_context = TxContext(base, final_delete_tx);
  const auto final_delete = DeleteRow(final_delete_context, table, row_uuid);
  const bool final_delete_done = final_delete.ok && Commit(final_delete_context);
  const auto final_read_tx = Begin(base);
  const auto final_read_context = TxContext(base, final_read_tx);
  const bool committed_delete_index_hidden = final_delete_done && HasZeroRows(SelectName(final_read_context, table, "Grace"));
  const bool final_read_commit = Commit(final_read_context);

  const auto unsupported_tx = Begin(base);
  const auto unsupported_context = TxContext(base, unsupported_tx);
  EngineCreateIndexRequest unsupported;
  unsupported.context = unsupported_context;
  unsupported.target_object = table;
  EngineIndexDefinition bad;
  bad.index_kind = "unsupported_index_profile_for_probe";
  bad.key_envelopes.push_back("name");
  unsupported.indexes.push_back(bad);
  const auto unsupported_result = EngineCreateIndex(unsupported);
  const bool unsupported_index_rejected = !unsupported_result.ok && HasDiagnostic(unsupported_result, "SB_ENGINE_API_UNSUPPORTED_PROFILE");
  const auto opaque_table = CreateOpaqueTable(unsupported_context);
  const auto opaque_index = CreateOpaqueIndex(unsupported_context, opaque_table.table_object);
  const bool opaque_index_rejected = opaque_table.ok && !opaque_index.ok && HasDiagnostic(opaque_index, "SB_ENGINE_API_UNSUPPORTED_PROFILE");
  const auto opaque_insert = InsertOpaquePayload(unsupported_context, opaque_table.table_object, "render-token");
  const auto opaque_select = SelectOpaquePayload(unsupported_context, opaque_table.table_object, "render-token");
  const auto opaque_update = UpdateOpaquePayload(unsupported_context, opaque_table.table_object, "changed-token");
  const bool opaque_mutation_rejected = opaque_table.ok && !opaque_insert.ok &&
                                        HasDiagnostic(opaque_insert, "SB_ENGINE_API_UNSUPPORTED_PROFILE") &&
                                        !opaque_update.ok &&
                                        HasDiagnostic(opaque_update, "SB_ENGINE_API_UNSUPPORTED_PROFILE");
  const bool opaque_comparison_rejected = opaque_table.ok && !opaque_select.ok &&
                                          HasDiagnostic(opaque_select, "SB_ENGINE_API_UNSUPPORTED_PROFILE");
  const bool unsupported_rollback = Rollback(unsupported_context);

  const bool ok = create_insert_index_visible && setup_commit && committed_index_reopen && committed_read_commit &&
                  rollback_insert_hidden && rollback_insert_done && rollback_insert_read_commit && rollback_update_seen &&
                  rollback_update_done && rollback_update_restored && rollback_update_read_commit && committed_update_index_reopen &&
                  commit_update_done && commit_update_read_commit && rollback_delete_seen && rollback_delete_done &&
                  rollback_delete_restored && rollback_delete_read_commit && committed_delete_index_hidden && final_read_commit &&
                  unsupported_index_rejected && opaque_index_rejected && opaque_mutation_rejected &&
                  opaque_comparison_rejected && unsupported_rollback;

  std::cout << "{\n";
  std::cout << "  \"ok\": " << (ok ? "true" : "false") << ",\n";
  std::cout << "  \"create_insert_index_visible\": " << (create_insert_index_visible ? "true" : "false") << ",\n";
  std::cout << "  \"committed_index_reopen\": " << (committed_index_reopen ? "true" : "false") << ",\n";
  std::cout << "  \"rollback_insert_hidden\": " << (rollback_insert_hidden ? "true" : "false") << ",\n";
  std::cout << "  \"rollback_update_restored\": " << (rollback_update_restored ? "true" : "false") << ",\n";
  std::cout << "  \"committed_update_index_reopen\": " << (committed_update_index_reopen ? "true" : "false") << ",\n";
  std::cout << "  \"rollback_delete_restored\": " << (rollback_delete_restored ? "true" : "false") << ",\n";
  std::cout << "  \"committed_delete_index_hidden\": " << (committed_delete_index_hidden ? "true" : "false") << ",\n";
  std::cout << "  \"unsupported_index_rejected\": " << (unsupported_index_rejected ? "true" : "false") << ",\n";
  std::cout << "  \"opaque_index_rejected\": " << (opaque_index_rejected ? "true" : "false") << ",\n";
  std::cout << "  \"opaque_mutation_rejected\": " << (opaque_mutation_rejected ? "true" : "false") << ",\n";
  std::cout << "  \"opaque_comparison_rejected\": " << (opaque_comparison_rejected ? "true" : "false") << "\n";
  std::cout << "}\n";
  return ok ? 0 : 1;
}
