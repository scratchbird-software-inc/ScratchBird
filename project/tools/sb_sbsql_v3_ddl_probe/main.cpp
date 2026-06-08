// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "catalog/schema_tree_api.hpp"
#include "ddl/comment_api.hpp"
#include "ddl/create_api.hpp"
#include "transaction/transaction_api.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

using namespace scratchbird::engine::internal_api;

namespace {

struct Args {
  std::string path;
  bool overwrite = false;
};

bool ParseArgs(int argc, char** argv, Args* args) {
  for (int i = 1; i < argc; ++i) {
    const std::string key = argv[i];
    if (key == "--overwrite") { args->overwrite = true; continue; }
    if (i + 1 >= argc) { return false; }
    const std::string value = argv[++i];
    if (key == "--path") { args->path = value; }
    else { return false; }
  }
  return !args->path.empty();
}

EngineRequestContext BaseContext(const Args& args) {
  EngineRequestContext context;
  context.trust_mode = EngineTrustMode::embedded_in_process;
  context.security_context_present = true;
  context.request_id = "sbsql-v3-ddl-probe";
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

EngineColumnDefinition Column(std::string name, std::string type, std::uint32_t ordinal) {
  EngineColumnDefinition column;
  column.ordinal = ordinal;
  column.names.push_back({"en", "default", name, name, true});
  column.descriptor.descriptor_kind = "scalar";
  column.descriptor.canonical_type_name = std::move(type);
  return column;
}

EngineDescriptor IntDescriptor() {
  EngineDescriptor descriptor;
  descriptor.descriptor_uuid.canonical = "00000000-0000-7000-8000-000000000081";
  descriptor.descriptor_kind = "scalar";
  descriptor.canonical_type_name = "int32";
  descriptor.encoded_descriptor = "canonical=int32;nullable=true";
  return descriptor;
}

bool HasEvidence(const EngineApiResult& result, const std::string& kind) {
  for (const auto& evidence : result.evidence) {
    if (evidence.evidence_kind == kind) { return true; }
  }
  return false;
}

std::string FieldValue(const EngineRowValue& row, const std::string& field) {
  for (const auto& [name, value] : row.fields) {
    if (name == field) { return value.encoded_value; }
  }
  return {};
}

bool HasCatalogRow(const EngineApiResult& result, const std::string& kind, const std::string& name) {
  for (const auto& row : result.result_shape.rows) {
    if (FieldValue(row, "object_kind") == kind && FieldValue(row, "name") == name) { return true; }
  }
  return false;
}

void PrintBool(const std::string& name, bool value, bool comma) {
  std::cout << "  \"" << name << "\": " << (value ? "true" : "false") << (comma ? "," : "") << "\n";
}

}  // namespace

int main(int argc, char** argv) {
  Args args;
  if (!ParseArgs(argc, argv, &args)) {
    std::cerr << "usage: sb_sbsql_v3_ddl_probe --path PATH [--overwrite]\n";
    return 2;
  }
  if (args.overwrite) { std::filesystem::remove(args.path); }
  { std::ofstream bootstrap(args.path, std::ios::binary | std::ios::app); }

  const auto base = BaseContext(args);
  const auto tx = Begin(base);
  const auto tx_context = TxContext(base, tx);

  EngineCreateSchemaRequest schema;
  schema.context = tx_context;
  schema.target_object.object_kind = "schema";
  schema.localized_names.push_back({"en", "default", "app", "app", true});
  schema.localized_names.push_back({"fr", "default", "application", "application", true});
  const auto schema_result = EngineCreateSchema(schema);

  EngineCreateTableRequest table;
  table.context = tx_context;
  table.target_schema = schema_result.primary_object;
  table.table_names.push_back({"en", "default", "person", "person", true});
  table.table_columns.push_back(Column("id", "text", 1));
  table.table_columns.push_back(Column("age", "int32", 2));
  const auto table_result = EngineCreateTable(table);

  EngineCreateIndexRequest index;
  index.context = tx_context;
  index.target_object = table_result.table_object;
  EngineIndexDefinition index_definition;
  index_definition.names.push_back({"en", "default", "person_id_idx", "person_id_idx", true});
  index_definition.index_kind = "btree";
  index_definition.key_envelopes.push_back("id");
  index.indexes.push_back(index_definition);
  const auto index_result = EngineCreateIndex(index);

  EngineCreateDomainRequest domain;
  domain.context = tx_context;
  domain.target_schema = schema_result.primary_object;
  domain.localized_names.push_back({"en", "default", "positive_int", "positive_int", true});
  domain.descriptors.push_back(IntDescriptor());
  domain.policy_profile.encoded_profiles.push_back("nullable:false");
  const auto domain_result = EngineCreateDomain(domain);

  EngineCommentOnObjectRequest comment;
  comment.context = tx_context;
  comment.target_object.object_kind = "localized_comment";
  comment.related_objects.push_back(table_result.table_object);
  comment.localized_names.push_back({"en", "comment", "person", "Person table", true});
  comment.option_envelopes.push_back("comment:en:Person table");
  const auto comment_result = EngineCommentOnObject(comment);

  const bool evidence_before_success = schema_result.ok && table_result.ok && index_result.ok && domain_result.ok &&
                                       comment_result.ok && HasEvidence(schema_result, "api_behavior_event") &&
                                       HasEvidence(table_result, "crud_event") &&
                                       HasEvidence(index_result, "crud_event") &&
                                       HasEvidence(domain_result, "domain_event") &&
                                       HasEvidence(comment_result, "api_behavior_event");
  const bool uuid_separation = table_result.table_object.uuid.canonical != table_result.table_catalog_row_uuid.canonical &&
                               !table_result.table_object.uuid.canonical.empty() &&
                               !table_result.table_catalog_row_uuid.canonical.empty();
  const bool committed = Commit(tx_context);

  const auto read_tx = Begin(base);
  const auto read_context = TxContext(base, read_tx);
  EngineListCatalogChildrenRequest list;
  list.context = read_context;
  const auto list_result = EngineListCatalogChildren(list);
  const bool catalog_visible_after_reopen = list_result.ok && HasCatalogRow(list_result, "schema", "app") &&
                                            HasCatalogRow(list_result, "table", "person") &&
                                            HasCatalogRow(list_result, "domain", "positive_int") &&
                                            HasCatalogRow(list_result, "object_comment", "Person table");
  const bool read_committed = Commit(read_context);

  const bool ok = tx.ok && evidence_before_success && uuid_separation && committed && catalog_visible_after_reopen &&
                  read_tx.ok && read_committed;
  std::cout << "{\n";
  PrintBool("ok", ok, true);
  PrintBool("evidence_before_success", evidence_before_success, true);
  PrintBool("uuid_separation", uuid_separation, true);
  PrintBool("catalog_visible_after_reopen", catalog_visible_after_reopen, false);
  std::cout << "}\n";
  return ok ? 0 : 1;
}
