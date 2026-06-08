// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "catalog/schema_tree_api.hpp"
#include "ddl/alter_api.hpp"
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
    if (key == "--overwrite") {
      args->overwrite = true;
      continue;
    }
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
  context.request_id = "catalog-ddl-transaction-probe";
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

bool Rollback(const EngineRequestContext& context) {
  EngineRollbackTransactionRequest request;
  request.context = context;
  return EngineRollbackTransaction(request).ok;
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
  request.localized_names.push_back({"en", "default", std::move(path), std::move(name), true});
  return EngineCreateSchema(request);
}

EngineAlterObjectResult RenameOrMoveSchema(const EngineRequestContext& context,
                                           std::string schema_uuid,
                                           std::string parent_uuid,
                                           std::string path,
                                           std::string name) {
  EngineAlterObjectRequest request;
  request.context = context;
  request.target_object.uuid.canonical = std::move(schema_uuid);
  request.target_object.object_kind = "schema";
  if (!parent_uuid.empty()) {
    request.target_schema.uuid.canonical = std::move(parent_uuid);
    request.target_schema.object_kind = "schema";
  }
  request.localized_names.push_back({"en", "default", std::move(path), std::move(name), true});
  return EngineAlterObject(request);
}

std::string FieldValue(const EngineRowValue& row, const std::string& field) {
  for (const auto& [name, value] : row.fields) {
    if (name == field) { return value.encoded_value; }
  }
  return {};
}

bool HasSchema(const EngineApiResult& result, const std::string& uuid, const std::string& name, const std::string& payload_fragment) {
  for (const auto& row : result.result_shape.rows) {
    if (FieldValue(row, "object_kind") != "schema") { continue; }
    if (!uuid.empty() && FieldValue(row, "object_uuid") != uuid) { continue; }
    if (!name.empty() && FieldValue(row, "name") != name) { continue; }
    if (!payload_fragment.empty() && FieldValue(row, "payload").find(payload_fragment) == std::string::npos) { continue; }
    return true;
  }
  return false;
}

EngineListCatalogChildrenResult ListSchemas(const EngineRequestContext& context) {
  EngineListCatalogChildrenRequest request;
  request.context = context;
  return EngineListCatalogChildren(request);
}

bool HasEvidence(const EngineApiResult& result, const std::string& kind, const std::string& id) {
  for (const auto& evidence : result.evidence) {
    if (evidence.evidence_kind == kind && (id.empty() || evidence.evidence_id == id)) { return true; }
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
    std::cerr << "usage: sb_catalog_ddl_transaction_probe --path PATH [--overwrite]\n";
    return 2;
  }
  if (args.overwrite) { std::filesystem::remove(args.path); }
  { std::ofstream bootstrap(args.path, std::ios::binary | std::ios::app); }

  const auto base = Base(args);

  const auto rollback_tx = Begin(base);
  const auto rollback_context = Tx(base, rollback_tx);
  const auto rolled_schema = CreateSchema(rollback_context, "00000000-0000-7000-8000-000000000831", "", "rolled", "rolled");
  const bool rollback_ok = rolled_schema.ok && Rollback(rollback_context);

  const auto rollback_read_tx = Begin(base);
  const auto rollback_read_context = Tx(base, rollback_read_tx);
  const auto rollback_list = ListSchemas(rollback_read_context);
  const bool rolled_back_hidden = rollback_list.ok && !HasSchema(rollback_list, rolled_schema.primary_object.uuid.canonical, "rolled", "");
  const bool rollback_read_commit = Commit(rollback_read_context);

  const auto interrupted_tx = Begin(base);
  const auto interrupted_context = Tx(base, interrupted_tx);
  const auto interrupted_schema = CreateSchema(interrupted_context, "00000000-0000-7000-8000-000000000832", "", "interrupted", "interrupted");

  const auto recovery_read_tx = Begin(base);
  const auto recovery_read_context = Tx(base, recovery_read_tx);
  const auto recovery_list = ListSchemas(recovery_read_context);
  const bool interrupted_hidden = interrupted_schema.ok && recovery_list.ok &&
                                  !HasSchema(recovery_list, interrupted_schema.primary_object.uuid.canonical, "interrupted", "");
  const bool recovery_read_commit = Commit(recovery_read_context);

  const auto commit_tx = Begin(base);
  const auto commit_context = Tx(base, commit_tx);
  const auto root = CreateSchema(commit_context, "00000000-0000-7000-8000-000000000833", "", "root", "root");
  const auto old_parent = CreateSchema(commit_context, "00000000-0000-7000-8000-000000000834", root.primary_object.uuid.canonical, "root/old_parent", "old_parent");
  const auto new_parent = CreateSchema(commit_context, "00000000-0000-7000-8000-000000000835", root.primary_object.uuid.canonical, "root/new_parent", "new_parent");
  const auto child = CreateSchema(commit_context, "00000000-0000-7000-8000-000000000836", old_parent.primary_object.uuid.canonical, "root/old_parent/child", "child");
  const bool row_object_distinct = root.ok && root.catalog_row_uuid.canonical != root.primary_object.uuid.canonical &&
                                   !root.catalog_row_uuid.canonical.empty();
  const bool evidence_before_success = root.ok && HasEvidence(root, "api_behavior_event", "ddl.create_schema");
  const bool commit_ok = root.ok && old_parent.ok && new_parent.ok && child.ok && Commit(commit_context);

  const auto alter_rollback_tx = Begin(base);
  const auto alter_rollback_context = Tx(base, alter_rollback_tx);
  const auto renamed_rollback = RenameOrMoveSchema(alter_rollback_context,
                                                   child.primary_object.uuid.canonical,
                                                   old_parent.primary_object.uuid.canonical,
                                                   "root/old_parent/renamed_rollback",
                                                   "renamed_rollback");
  const bool alter_rollback_ok = renamed_rollback.ok && Rollback(alter_rollback_context);

  const auto alter_rollback_read_tx = Begin(base);
  const auto alter_rollback_read_context = Tx(base, alter_rollback_read_tx);
  const auto alter_rollback_list = ListSchemas(alter_rollback_read_context);
  const bool alter_rollback_preserved_old_identity =
      alter_rollback_list.ok &&
      HasSchema(alter_rollback_list, child.primary_object.uuid.canonical, "child", "schema=" + old_parent.primary_object.uuid.canonical) &&
      !HasSchema(alter_rollback_list, child.primary_object.uuid.canonical, "renamed_rollback", "");
  const bool alter_rollback_read_commit = Commit(alter_rollback_read_context);

  const auto move_tx = Begin(base);
  const auto move_context = Tx(base, move_tx);
  const auto moved = RenameOrMoveSchema(move_context,
                                        child.primary_object.uuid.canonical,
                                        new_parent.primary_object.uuid.canonical,
                                        "root/new_parent/moved_child",
                                        "moved_child");
  const bool move_commit_ok = moved.ok && HasEvidence(moved, "schema_identity_preserved", child.primary_object.uuid.canonical) &&
                              Commit(move_context);

  const auto final_read_tx = Begin(base);
  const auto final_read_context = Tx(base, final_read_tx);
  const auto final_list = ListSchemas(final_read_context);
  const bool committed_visible = final_list.ok && HasSchema(final_list, root.primary_object.uuid.canonical, "root", "");
  const bool move_preserved_uuid = final_list.ok &&
                                   HasSchema(final_list,
                                             child.primary_object.uuid.canonical,
                                             "moved_child",
                                             "schema=" + new_parent.primary_object.uuid.canonical);
  const bool final_read_commit = Commit(final_read_context);

  const bool ok = rollback_tx.ok && rollback_ok && rollback_read_tx.ok && rolled_back_hidden && rollback_read_commit &&
                  interrupted_tx.ok && interrupted_hidden && recovery_read_tx.ok && recovery_read_commit &&
                  commit_tx.ok && row_object_distinct && evidence_before_success && commit_ok &&
                  alter_rollback_tx.ok && alter_rollback_ok && alter_rollback_read_tx.ok &&
                  alter_rollback_preserved_old_identity && alter_rollback_read_commit &&
                  move_tx.ok && move_commit_ok && final_read_tx.ok && committed_visible && move_preserved_uuid &&
                  final_read_commit;

  std::cout << "{\n";
  PrintBool("ok", ok, true);
  PrintBool("rolled_back_hidden", rolled_back_hidden, true);
  PrintBool("interrupted_active_hidden", interrupted_hidden, true);
  PrintBool("committed_visible", committed_visible, true);
  PrintBool("alter_rollback_preserved_old_identity", alter_rollback_preserved_old_identity, true);
  PrintBool("move_preserved_uuid", move_preserved_uuid, true);
  PrintBool("catalog_row_object_uuid_distinct", row_object_distinct, true);
  PrintBool("evidence_before_success", evidence_before_success, false);
  std::cout << "}\n";
  return ok ? 0 : 1;
}
