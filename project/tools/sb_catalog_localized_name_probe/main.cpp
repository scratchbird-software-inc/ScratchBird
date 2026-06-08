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

struct Args { std::string path; bool overwrite = false; };

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

EngineRequestContext Base(const Args& args) {
  EngineRequestContext context;
  context.trust_mode = EngineTrustMode::embedded_in_process;
  context.security_context_present = true;
  context.request_id = "catalog-localized-name-probe";
  context.database_path = args.path;
  return context;
}

EngineRequestContext Tx(EngineRequestContext base, const EngineBeginTransactionResult& tx) {
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

bool Commit(const EngineRequestContext& context) {
  EngineCommitTransactionRequest request;
  request.context = context;
  return EngineCommitTransaction(request).ok;
}

std::string FieldValue(const EngineRowValue& row, const std::string& field) {
  for (const auto& [name, value] : row.fields) {
    if (name == field) { return value.encoded_value; }
  }
  return {};
}

bool PayloadContains(const EngineApiResult& result, const std::string& kind, const std::string& name, const std::string& fragment) {
  for (const auto& row : result.result_shape.rows) {
    if (FieldValue(row, "object_kind") == kind && FieldValue(row, "name") == name &&
        FieldValue(row, "payload").find(fragment) != std::string::npos) {
      return true;
    }
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
    std::cerr << "usage: sb_catalog_localized_name_probe --path PATH [--overwrite]\n";
    return 2;
  }
  if (args.overwrite) { std::filesystem::remove(args.path); }
  { std::ofstream bootstrap(args.path, std::ios::binary | std::ios::app); }

  const auto base = Base(args);
  const auto tx = Begin(base);
  const auto context = Tx(base, tx);

  EngineCreateSchemaRequest schema;
  schema.context = context;
  schema.target_object.object_kind = "schema";
  schema.localized_names.push_back({"en", "default", "sales", "sales", true});
  schema.localized_names.push_back({"fr-CA", "default", "ventes", "ventes", true});
  schema.localized_names.push_back({"es", "alias", "ventas", "ventas", false});
  const auto created_schema = EngineCreateSchema(schema);

  EngineCommentOnObjectRequest comment;
  comment.context = context;
  comment.target_object.object_kind = "localized_comment";
  comment.related_objects.push_back(created_schema.primary_object);
  comment.localized_names.push_back({"en", "comment", "sales", "Sales schema", true});
  comment.localized_names.push_back({"fr-CA", "comment", "ventes", "Schema des ventes", true});
  comment.option_envelopes.push_back("comment:en:Sales schema");
  comment.option_envelopes.push_back("comment:fr-CA:Schema des ventes");
  const auto created_comment = EngineCommentOnObject(comment);
  const bool committed = Commit(context);

  const auto read_tx = Begin(base);
  const auto read_context = Tx(base, read_tx);
  EngineListCatalogChildrenRequest list;
  list.context = read_context;
  const auto listed = EngineListCatalogChildren(list);
  const bool names_persisted = listed.ok && PayloadContains(listed, "schema", "sales", "localized_name_count=3") &&
                               PayloadContains(listed, "schema", "sales", "localized_name=fr-CA,default,ventes,ventes,default") &&
                               PayloadContains(listed, "schema", "sales", "localized_name=es,alias,ventas,ventas,alias");
  const bool comments_persisted = listed.ok && PayloadContains(listed, "object_comment", "Sales schema", "comment:fr-CA:Schema des ventes");
  const bool read_committed = Commit(read_context);
  const bool ok = tx.ok && created_schema.ok && created_comment.ok && committed && read_tx.ok && names_persisted &&
                  comments_persisted && read_committed;

  std::cout << "{\n";
  PrintBool("ok", ok, true);
  PrintBool("localized_names_persisted", names_persisted, true);
  PrintBool("localized_comments_persisted", comments_persisted, false);
  std::cout << "}\n";
  return ok ? 0 : 1;
}
