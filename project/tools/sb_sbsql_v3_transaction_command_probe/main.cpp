// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "ddl/create_api.hpp"
#include "dml/insert_api.hpp"
#include "dml/select_api.hpp"
#include "transaction/savepoint_api.hpp"
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
    if (key == "--path") { args->path = value; } else { return false; }
  }
  return !args->path.empty();
}

EngineRequestContext Base(const Args& args) {
  EngineRequestContext context;
  context.trust_mode = EngineTrustMode::embedded_in_process;
  context.security_context_present = true;
  context.request_id = "sbsql-v3-transaction-command-probe";
  context.database_path = args.path;
  return context;
}

EngineRequestContext Tx(EngineRequestContext base, const EngineBeginTransactionResult& tx) {
  base.local_transaction_id = tx.local_transaction_id;
  base.transaction_uuid = tx.transaction_uuid;
  return base;
}

EngineBeginTransactionResult Begin(const EngineRequestContext& base, std::string isolation = "serializable") {
  EngineBeginTransactionRequest request;
  request.context = base;
  request.isolation_level = std::move(isolation);
  request.transaction_policy_profile.encoded_profiles.push_back("idle_timeout_ms:30000");
  request.transaction_policy_profile.encoded_profiles.push_back("max_age_ms:120000");
  request.transaction_policy_profile.encoded_profiles.push_back("long_running_warning_ms:60000");
  request.transaction_policy_profile.encoded_profiles.push_back("read_only:false");
  return EngineBeginTransaction(request);
}

bool HasEvidence(const EngineApiResult& result, const std::string& kind) {
  for (const auto& evidence : result.evidence) {
    if (evidence.evidence_kind == kind) { return true; }
  }
  return false;
}

bool HasDiagnostic(const EngineApiResult& result, const std::string& detail) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.code == detail || diagnostic.detail == detail ||
        diagnostic.detail.find(detail) != std::string::npos ||
        diagnostic.message_key.find(detail) != std::string::npos) {
      return true;
    }
  }
  return false;
}

bool Commit(const EngineRequestContext& context) {
  EngineCommitTransactionRequest request;
  request.context = context;
  return EngineCommitTransaction(request).ok;
}

EngineColumnDefinition Column(std::string name) {
  EngineColumnDefinition column;
  column.ordinal = 1;
  column.names.push_back({"en", "default", name, name, true});
  column.descriptor.descriptor_kind = "scalar";
  column.descriptor.canonical_type_name = "text";
  return column;
}

EngineTypedValue Value(std::string value) {
  EngineTypedValue typed;
  typed.encoded_value = std::move(value);
  return typed;
}

EngineRowValue Row(std::string id) {
  EngineRowValue row;
  row.fields.push_back({"id", Value(std::move(id))});
  return row;
}

EngineSelectRowsResult SelectAll(const EngineRequestContext& context, const EngineObjectReference& table) {
  EngineSelectRowsRequest request;
  request.context = context;
  request.source_object = table;
  return EngineSelectRows(request);
}

void PrintBool(const std::string& name, bool value, bool comma) {
  std::cout << "  \"" << name << "\": " << (value ? "true" : "false") << (comma ? "," : "") << "\n";
}

}  // namespace

int main(int argc, char** argv) {
  Args args;
  if (!ParseArgs(argc, argv, &args)) {
    std::cerr << "usage: sb_sbsql_v3_transaction_command_probe --path PATH [--overwrite]\n";
    return 2;
  }
  if (args.overwrite) { std::filesystem::remove(args.path); }
  { std::ofstream bootstrap(args.path, std::ios::binary | std::ios::app); }

  const auto base = Base(args);
  EngineBeginTransactionRequest bad_isolation;
  bad_isolation.context = base;
  bad_isolation.isolation_level = "magic";
  const auto bad_isolation_result = EngineBeginTransaction(bad_isolation);
  const bool bad_isolation_rejected = !bad_isolation_result.ok && HasDiagnostic(bad_isolation_result, "unsupported_isolation_level");

  EngineBeginTransactionRequest bad_policy;
  bad_policy.context = base;
  bad_policy.isolation_level = "read_committed";
  bad_policy.transaction_policy_profile.encoded_profiles.push_back("idle_forever:true");
  const auto bad_policy_result = EngineBeginTransaction(bad_policy);
  const bool bad_policy_rejected = !bad_policy_result.ok && HasDiagnostic(bad_policy_result, "unsupported_transaction_policy_profile");

  const auto tx = Begin(base, "repeatable-read");
  const auto context = Tx(base, tx);
  const bool begin_evidence = tx.ok && HasEvidence(tx, "crud_event") && HasEvidence(tx, "isolation_level") &&
                              HasEvidence(tx, "transaction_policy_profile_count");

  EngineCreateTableRequest create;
  create.context = context;
  create.table_names.push_back({"en", "default", "tx_probe", "tx_probe", true});
  create.table_columns.push_back(Column("id"));
  const auto table = EngineCreateTable(create);

  EngineInsertRowsRequest first;
  first.context = context;
  first.target_table = table.table_object;
  first.input_rows.push_back(Row("before"));
  const auto first_insert = EngineInsertRows(first);

  EngineCreateSavepointRequest savepoint;
  savepoint.context = context;
  savepoint.option_envelopes.push_back("sp1");
  const auto sp = EngineCreateSavepoint(savepoint);

  EngineInsertRowsRequest second;
  second.context = context;
  second.target_table = table.table_object;
  second.input_rows.push_back(Row("after"));
  const auto second_insert = EngineInsertRows(second);

  EngineRollbackToSavepointRequest rollback_sp;
  rollback_sp.context = context;
  rollback_sp.option_envelopes.push_back("sp1");
  const auto sp_rollback = EngineRollbackToSavepoint(rollback_sp);
  const bool savepoint_rollback_visible = SelectAll(context, table.table_object).visible_count == 1;

  EngineReleaseSavepointRequest release_sp;
  release_sp.context = context;
  release_sp.option_envelopes.push_back("sp1");
  const auto sp_release = EngineReleaseSavepoint(release_sp);

  const bool committed = Commit(context);
  EngineCommitTransactionRequest second_commit;
  second_commit.context = context;
  const auto second_commit_result = EngineCommitTransaction(second_commit);
  const bool double_commit_rejected = !second_commit_result.ok && HasDiagnostic(second_commit_result, "local_transaction_id_not_active");

  const auto read_tx = Begin(base, "snapshot");
  const auto read_context = Tx(base, read_tx);
  const bool reopen_state = SelectAll(read_context, table.table_object).visible_count == 1;
  const bool read_commit = Commit(read_context);

  const bool ok = bad_isolation_rejected && bad_policy_rejected && begin_evidence && table.ok && first_insert.ok &&
                  sp.ok && second_insert.ok && sp_rollback.ok && savepoint_rollback_visible && sp_release.ok &&
                  committed && double_commit_rejected && read_tx.ok && reopen_state && read_commit;

  std::cout << "{\n";
  PrintBool("ok", ok, true);
  PrintBool("bad_isolation_rejected", bad_isolation_rejected, true);
  PrintBool("bad_policy_rejected", bad_policy_rejected, true);
  PrintBool("savepoint_rollback_visible", savepoint_rollback_visible, true);
  PrintBool("double_commit_rejected", double_commit_rejected, false);
  std::cout << "}\n";
  return ok ? 0 : 1;
}
