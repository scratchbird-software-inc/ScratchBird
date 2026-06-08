// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "catalog/schema_tree_api.hpp"
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
  context.request_id = "catalog-schema-tree-command-probe";
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

EngineCreateSchemaResult CreateSchema(const EngineRequestContext& context,
                                      std::string uuid,
                                      std::string parent_uuid,
                                      std::string path,
                                      std::string name) {
  EngineCreateSchemaRequest request;
  request.context = context;
  request.target_object.uuid.canonical = std::move(uuid);
  request.target_object.object_kind = "schema";
  if (!parent_uuid.empty()) {
    request.target_schema.uuid.canonical = std::move(parent_uuid);
    request.target_schema.object_kind = "schema";
  }
  request.localized_names.push_back({"en", "default", path, name, true});
  return EngineCreateSchema(request);
}

std::string FieldValue(const EngineRowValue& row, const std::string& field) {
  for (const auto& [name, value] : row.fields) {
    if (name == field) { return value.encoded_value; }
  }
  return {};
}

bool HasSchemaPayload(const EngineApiResult& result, const std::string& name, const std::string& payload_fragment) {
  for (const auto& row : result.result_shape.rows) {
    if (FieldValue(row, "object_kind") == "schema" && FieldValue(row, "name") == name &&
        FieldValue(row, "payload").find(payload_fragment) != std::string::npos) {
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
    std::cerr << "usage: sb_catalog_schema_tree_command_probe --path PATH [--overwrite]\n";
    return 2;
  }
  if (args.overwrite) { std::filesystem::remove(args.path); }
  { std::ofstream bootstrap(args.path, std::ios::binary | std::ios::app); }

  const auto base = Base(args);
  const auto tx = Begin(base);
  const auto context = Tx(base, tx);
  const auto root = CreateSchema(context, "00000000-0000-7000-8000-000000000811", "", "root", "root");
  const auto app = CreateSchema(context, "00000000-0000-7000-8000-000000000812", root.primary_object.uuid.canonical, "root/app", "app");
  const auto tenant = CreateSchema(context, "00000000-0000-7000-8000-000000000813", app.primary_object.uuid.canonical, "root/app/tenant", "tenant");
  const bool committed = Commit(context);

  const auto read_tx = Begin(base);
  const auto read_context = Tx(base, read_tx);
  EngineListCatalogChildrenRequest list;
  list.context = read_context;
  const auto listed = EngineListCatalogChildren(list);
  const bool recursive_rows = listed.ok && HasSchemaPayload(listed, "root", "localized_name=en,default,root,root,default") &&
                              HasSchemaPayload(listed, "app", "schema=" + root.primary_object.uuid.canonical) &&
                              HasSchemaPayload(listed, "tenant", "schema=" + app.primary_object.uuid.canonical);
  const bool read_committed = Commit(read_context);
  const bool ok = tx.ok && root.ok && app.ok && tenant.ok && committed && read_tx.ok && recursive_rows && read_committed;

  std::cout << "{\n";
  PrintBool("ok", ok, true);
  PrintBool("recursive_schema_rows", recursive_rows, true);
  PrintBool("parent_uuid_payloads", recursive_rows, false);
  std::cout << "}\n";
  return ok ? 0 : 1;
}

