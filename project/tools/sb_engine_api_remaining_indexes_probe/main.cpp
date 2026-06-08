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
#include "transaction/transaction_api.hpp"

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
    if (key == "--overwrite") { args->overwrite = true; continue; }
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

bool HasIndexFamilyEvidence(const EngineApiResult& result, const std::string& family) {
  for (const auto& evidence : result.evidence) {
    if (evidence.evidence_kind == "index_lookup" && evidence.evidence_id.find("index_family=" + family) != std::string::npos) {
      return true;
    }
  }
  return false;
}

bool HasEvidenceText(const EngineApiResult& result, const std::string& text) {
  for (const auto& evidence : result.evidence) {
    if (evidence.evidence_id.find(text) != std::string::npos) { return true; }
  }
  return false;
}

EngineRequestContext BaseContext(const Args& args) {
  EngineRequestContext context;
  context.trust_mode = EngineTrustMode::embedded_in_process;
  context.security_context_present = true;
  context.request_id = "engine-api-remaining-indexes-probe";
  context.database_path = args.path;
  return context;
}

EngineRequestContext TxContext(EngineRequestContext base, const EngineBeginTransactionResult& tx) {
  base.local_transaction_id = tx.local_transaction_id;
  base.transaction_uuid = tx.transaction_uuid;
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

EngineRowValue Row(std::vector<std::pair<std::string, std::string>> fields) {
  EngineRowValue row;
  for (auto& [field, value] : fields) { row.fields.push_back({field, Value(std::move(value))}); }
  return row;
}

std::string FieldValue(const EngineRowValue& row, const std::string& field) {
  for (const auto& [name, value] : row.fields) {
    if (name == field) { return value.encoded_value; }
  }
  return {};
}

bool FirstRowFieldEquals(const EngineSelectRowsResult& result, const std::string& field, const std::string& value) {
  return result.ok && !result.result_shape.rows.empty() && FieldValue(result.result_shape.rows.front(), field) == value;
}

EngineCreateTableResult CreateIndexedTable(const EngineRequestContext& tx_context) {
  EngineCreateTableRequest request;
  request.context = tx_context;
  request.table_names.push_back({"en", "default", "indexed_probe", "indexed_probe", true});
  request.table_columns.push_back(Column("id", "text", 1));
  request.table_columns.push_back(Column("name", "text", 2));
  request.table_columns.push_back(Column("age", "int32", 3));
  request.table_columns.push_back(Column("status", "text", 4));
  request.table_columns.push_back(Column("body", "text", 5));
  request.table_columns.push_back(Column("bbox", "bbox2d", 6));
  request.table_columns.push_back(Column("embedding", "vector_float32", 7));
  request.table_columns.push_back(Column("embedding_hnsw", "vector_float32", 8));
  request.table_columns.push_back(Column("embedding_ivf", "vector_float32", 9));
  request.table_columns.push_back(Column("score", "int32", 10));
  request.table_columns.push_back(Column("city", "text", 11));
  request.table_columns.push_back(Column("tag", "text", 12));
  request.table_columns.push_back(Column("donor_key", "text", 13));
  return EngineCreateTable(request);
}

EngineInsertRowsResult InsertProbeRow(const EngineRequestContext& tx_context,
                                      const EngineObjectReference& table,
                                      std::vector<std::pair<std::string, std::string>> fields) {
  EngineInsertRowsRequest request;
  request.context = tx_context;
  request.target_table = table;
  request.input_rows.push_back(Row(std::move(fields)));
  return EngineInsertRows(request);
}

EngineCreateIndexResult CreateIndex(const EngineRequestContext& tx_context,
                                    const EngineObjectReference& table,
                                    std::string profile,
                                    std::string name,
                                    std::vector<std::string> keys) {
  EngineCreateIndexRequest request;
  request.context = tx_context;
  request.target_object = table;
  EngineIndexDefinition index;
  index.index_kind = std::move(profile);
  index.names.push_back({"en", "default", name, name, true});
  index.key_envelopes = std::move(keys);
  request.indexes.push_back(std::move(index));
  return EngineCreateIndex(request);
}

EngineSelectRowsResult Select(const EngineRequestContext& tx_context,
                              const EngineObjectReference& table,
                              std::string predicate_kind,
                              std::string envelope,
                              std::vector<std::string> values,
                              EngineApiU64 limit = 0) {
  EngineSelectRowsRequest request;
  request.context = tx_context;
  request.source_object = table;
  request.predicate.predicate_kind = std::move(predicate_kind);
  request.predicate.canonical_predicate_envelope = std::move(envelope);
  for (auto& value : values) { request.predicate.bound_values.push_back(Value(std::move(value))); }
  request.limit = limit;
  return EngineSelectRows(request);
}

EngineUpdateRowsResult UpdateField(const EngineRequestContext& tx_context,
                                   const EngineObjectReference& table,
                                   const std::string& row_uuid,
                                   std::string field,
                                   std::string value) {
  EngineUpdateRowsRequest request;
  request.context = tx_context;
  request.target_table = table;
  request.update_predicate.predicate_kind = "row_uuid_match";
  request.update_predicate.canonical_predicate_envelope = row_uuid;
  request.assignments.push_back({std::move(field), Value(std::move(value))});
  return EngineUpdateRows(request);
}

EngineDeleteRowsResult DeleteRow(const EngineRequestContext& tx_context, const EngineObjectReference& table, const std::string& row_uuid) {
  EngineDeleteRowsRequest request;
  request.context = tx_context;
  request.target_table = table;
  request.delete_predicate.predicate_kind = "row_uuid_match";
  request.delete_predicate.canonical_predicate_envelope = row_uuid;
  return EngineDeleteRows(request);
}

void PrintBool(const std::string& name, bool value, bool comma) {
  std::cout << "  \"" << name << "\": " << (value ? "true" : "false") << (comma ? "," : "") << "\n";
}

}  // namespace

int main(int argc, char** argv) {
  Args args;
  if (!ParseArgs(argc, argv, &args)) {
    std::cerr << "usage: sb_engine_api_remaining_indexes_probe --path PATH --creation-ms MILLIS [--overwrite]\n";
    return 2;
  }
  if (args.overwrite) { std::filesystem::remove(args.path); }
  { std::ofstream bootstrap(args.path, std::ios::binary | std::ios::app); }

  const auto base = BaseContext(args);
  const auto setup_tx = Begin(base);
  const auto setup_context = TxContext(base, setup_tx);
  const auto table_result = CreateIndexedTable(setup_context);
  const auto table = table_result.table_object;

  const auto ada = InsertProbeRow(setup_context, table, {{"id", "1"}, {"name", "Ada"}, {"age", "37"}, {"status", "active"}, {"body", "hello database engine"}, {"bbox", "0,0,2,2"}, {"embedding", "0,0"}, {"embedding_hnsw", "0,0"}, {"embedding_ivf", "0,0"}, {"score", "10"}, {"city", "London"}, {"tag", "hot"}, {"donor_key", "d1"}});
  const auto grace = InsertProbeRow(setup_context, table, {{"id", "2"}, {"name", "Grace"}, {"age", "45"}, {"status", "inactive"}, {"body", "graph vector search"}, {"bbox", "5,5,6,6"}, {"embedding", "10,10"}, {"embedding_hnsw", "10,10"}, {"embedding_ivf", "10,10"}, {"score", "25"}, {"city", "Paris"}, {"tag", "cold"}, {"donor_key", "d2"}});
  const auto lin = InsertProbeRow(setup_context, table, {{"id", "3"}, {"name", "Lin"}, {"age", "31"}, {"status", "active"}, {"body", "hello vector world"}, {"bbox", "1,1,3,3"}, {"embedding", "1,1"}, {"embedding_hnsw", "1,1"}, {"embedding_ivf", "1,1"}, {"score", "15"}, {"city", "London"}, {"tag", "warm"}, {"donor_key", "d3"}});
  const std::string ada_row_uuid = ada.row_uuids.empty() ? std::string{} : ada.row_uuids.front().canonical;

  const bool create_all_indexes =
      table_result.ok && ada.ok && grace.ok && lin.ok &&
      CreateIndex(setup_context, table, "btree", "ix_age", {"age"}).ok &&
      CreateIndex(setup_context, table, "hash", "ix_id_hash", {"id"}).ok &&
      CreateIndex(setup_context, table, "bitmap", "ix_status_bitmap", {"status"}).ok &&
      CreateIndex(setup_context, table, "full_text", "ix_body_fts", {"body"}).ok &&
      CreateIndex(setup_context, table, "spatial", "ix_bbox_spatial", {"bbox"}).ok &&
      CreateIndex(setup_context, table, "vector_exact", "ix_embedding_exact", {"embedding"}).ok &&
      CreateIndex(setup_context, table, "vector_hnsw", "ix_embedding_hnsw", {"embedding_hnsw"}).ok &&
      CreateIndex(setup_context, table, "vector_ivf", "ix_embedding_ivf", {"embedding_ivf"}).ok &&
      CreateIndex(setup_context, table, "columnar_zone", "ix_score_zone", {"score"}).ok &&
      CreateIndex(setup_context, table, "expression", "ix_lower_name", {"lower:name"}).ok &&
      CreateIndex(setup_context, table, "partial", "ix_active_name", {"name", "where_eq:status=active"}).ok &&
      CreateIndex(setup_context, table, "covering", "ix_city_cover", {"city", "include:name"}).ok &&
      CreateIndex(setup_context, table, "in_memory", "ix_tag_memory", {"tag"}).ok &&
      CreateIndex(setup_context, table, "donor_emulated", "ix_donor_key", {"donor_key"}).ok &&
      CreateIndex(setup_context, table, "btree_unique", "ix_id_unique", {"id", "unique"}).ok;

  const auto btree_range = Select(setup_context, table, "column_range", "age", {"30", "40"});
  const auto hash_eq = Select(setup_context, table, "column_equals", "id", {"1"});
  const auto bitmap_in = Select(setup_context, table, "column_in_list", "status", {"active"});
  const auto full_text = Select(setup_context, table, "text_all_terms", "body", {"hello", "vector"});
  const auto spatial = Select(setup_context, table, "spatial_bbox_intersects", "bbox", {"0.5,0.5,1.5,1.5"});
  const auto vector_exact = Select(setup_context, table, "vector_exact_nearest", "embedding", {"0,0"}, 1);
  const auto vector_hnsw = Select(setup_context, table, "vector_approx_nearest", "embedding_hnsw", {"0,0"}, 1);
  const auto vector_ivf = Select(setup_context, table, "vector_approx_nearest", "embedding_ivf", {"0,0"}, 1);
  const auto columnar_zone = Select(setup_context, table, "column_range", "score", {"9", "16"});
  const auto expression = Select(setup_context, table, "expression_equals", "lower:name", {"ada"});
  const auto partial = Select(setup_context, table, "partial_index_probe", "status=active", {});
  const auto covering = Select(setup_context, table, "column_equals", "city", {"London"});
  const auto in_memory = Select(setup_context, table, "column_equals", "tag", {"hot"});
  const auto donor = Select(setup_context, table, "column_equals", "donor_key", {"d1"});

  const bool btree_supported = btree_range.ok && btree_range.visible_count == 2 && HasIndexFamilyEvidence(btree_range, "btree");
  const bool hash_supported = hash_eq.ok && hash_eq.visible_count == 1 && HasIndexFamilyEvidence(hash_eq, "hash");
  const bool bitmap_supported = bitmap_in.ok && bitmap_in.visible_count == 2 && HasIndexFamilyEvidence(bitmap_in, "bitmap");
  const bool full_text_supported = full_text.ok && full_text.visible_count == 1 && FirstRowFieldEquals(full_text, "name", "Lin") && HasIndexFamilyEvidence(full_text, "full_text");
  const bool spatial_supported = spatial.ok && spatial.visible_count == 2 && HasIndexFamilyEvidence(spatial, "spatial");
  const bool vector_exact_supported = vector_exact.ok && vector_exact.visible_count == 1 && FirstRowFieldEquals(vector_exact, "name", "Ada") && HasIndexFamilyEvidence(vector_exact, "vector_exact");
  const bool vector_hnsw_supported = vector_hnsw.ok && vector_hnsw.visible_count == 1 && FirstRowFieldEquals(vector_hnsw, "name", "Ada") && HasIndexFamilyEvidence(vector_hnsw, "vector_hnsw") && HasEvidenceText(vector_hnsw, "fallback_mode=exact_scan_compatibility_fallback");
  const bool vector_ivf_supported = vector_ivf.ok && vector_ivf.visible_count == 1 && FirstRowFieldEquals(vector_ivf, "name", "Ada") && HasIndexFamilyEvidence(vector_ivf, "vector_ivf") && HasEvidenceText(vector_ivf, "fallback_mode=exact_scan_compatibility_fallback");
  const bool columnar_zone_supported = columnar_zone.ok && columnar_zone.visible_count == 2 && HasIndexFamilyEvidence(columnar_zone, "columnar_zone");
  const bool expression_supported = expression.ok && expression.visible_count == 1 && FirstRowFieldEquals(expression, "name", "Ada") && HasIndexFamilyEvidence(expression, "expression");
  const bool partial_supported = partial.ok && partial.visible_count == 2 && HasIndexFamilyEvidence(partial, "partial");
  const bool covering_supported = covering.ok && covering.visible_count == 2 && HasIndexFamilyEvidence(covering, "covering");
  const bool in_memory_supported = in_memory.ok && in_memory.visible_count == 1 && HasIndexFamilyEvidence(in_memory, "in_memory");
  const bool donor_emulated_supported = donor.ok && donor.visible_count == 1 && HasIndexFamilyEvidence(donor, "donor_emulated");

  EngineInsertRowsRequest duplicate_insert;
  duplicate_insert.context = setup_context;
  duplicate_insert.target_table = table;
  duplicate_insert.input_rows.push_back(Row({{"id", "1"}, {"name", "Duplicate"}, {"age", "99"}, {"status", "active"}}));
  const auto duplicate_result = EngineInsertRows(duplicate_insert);
  const bool unique_enforced = !duplicate_result.ok && HasDiagnostic(duplicate_result, "SB_ENGINE_API_INVALID_REQUEST");

  const bool setup_commit = Commit(setup_context);

  const auto rollback_update_tx = Begin(base);
  const auto rollback_update_context = TxContext(base, rollback_update_tx);
  const auto update_ada_tag = UpdateField(rollback_update_context, table, ada_row_uuid, "tag", "rolledback");
  const bool rollback_update_visible_in_tx = update_ada_tag.ok && Select(rollback_update_context, table, "column_equals", "tag", {"rolledback"}).visible_count == 1;
  const bool rollback_update_done = Rollback(rollback_update_context);
  const auto rollback_read_tx = Begin(base);
  const auto rollback_read_context = TxContext(base, rollback_read_tx);
  const bool rollback_update_hidden = Select(rollback_read_context, table, "column_equals", "tag", {"rolledback"}).visible_count == 0 &&
                                      Select(rollback_read_context, table, "column_equals", "tag", {"hot"}).visible_count == 1;
  const bool rollback_read_commit = Commit(rollback_read_context);

  const auto delete_tx = Begin(base);
  const auto delete_context = TxContext(base, delete_tx);
  const auto delete_ada = DeleteRow(delete_context, table, ada_row_uuid);
  const bool delete_hidden_in_tx = delete_ada.ok && Select(delete_context, table, "column_equals", "id", {"1"}).visible_count == 0;
  const bool delete_rollback = Rollback(delete_context);

  const auto reopen_tx = Begin(base);
  const auto reopen_context = TxContext(base, reopen_tx);
  const auto reopen_hash = Select(reopen_context, table, "column_equals", "id", {"1"});
  const auto unsupported_range_hash = Select(reopen_context, table, "column_range", "donor_key", {"a", "z"});
  const bool reopen_supported = reopen_hash.ok && reopen_hash.visible_count == 1 && HasIndexFamilyEvidence(reopen_hash, "hash");
  const bool unsupported_rejected = !unsupported_range_hash.ok && HasDiagnostic(unsupported_range_hash, "SB_ENGINE_API_UNSUPPORTED_PROFILE");
  const bool reopen_commit = Commit(reopen_context);

  const bool rollback_delete_restored = delete_hidden_in_tx && delete_rollback && reopen_supported;

  const bool ok = create_all_indexes && btree_supported && hash_supported && bitmap_supported && full_text_supported &&
                  spatial_supported && vector_exact_supported && vector_hnsw_supported && vector_ivf_supported &&
                  columnar_zone_supported && expression_supported && partial_supported && covering_supported &&
                  in_memory_supported && donor_emulated_supported && unique_enforced && setup_commit &&
                  rollback_update_visible_in_tx && rollback_update_done && rollback_update_hidden && rollback_read_commit &&
                  rollback_delete_restored && unsupported_rejected && reopen_commit;

  std::cout << "{\n";
  PrintBool("ok", ok, true);
  PrintBool("btree_supported", btree_supported, true);
  PrintBool("hash_supported", hash_supported, true);
  PrintBool("bitmap_supported", bitmap_supported, true);
  PrintBool("full_text_supported", full_text_supported, true);
  PrintBool("spatial_supported", spatial_supported, true);
  PrintBool("vector_exact_supported", vector_exact_supported, true);
  PrintBool("vector_hnsw_supported", vector_hnsw_supported, true);
  PrintBool("vector_ivf_supported", vector_ivf_supported, true);
  PrintBool("columnar_zone_supported", columnar_zone_supported, true);
  PrintBool("expression_supported", expression_supported, true);
  PrintBool("partial_supported", partial_supported, true);
  PrintBool("covering_supported", covering_supported, true);
  PrintBool("in_memory_supported", in_memory_supported, true);
  PrintBool("donor_emulated_supported", donor_emulated_supported, true);
  PrintBool("unique_enforced", unique_enforced, true);
  PrintBool("rollback_update_hidden", rollback_update_hidden, true);
  PrintBool("rollback_delete_restored", rollback_delete_restored, true);
  PrintBool("reopen_supported", reopen_supported, true);
  PrintBool("unsupported_rejected", unsupported_rejected, false);
  std::cout << "}\n";
  return ok ? 0 : 1;
}
