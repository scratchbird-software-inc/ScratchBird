// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "observability/explain_api.hpp"
#include "observability/metrics_api.hpp"
#include "observability/show_api.hpp"
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

EngineRequestContext Context(const Args& args, bool secure) {
  EngineRequestContext context;
  context.trust_mode = EngineTrustMode::embedded_in_process;
  context.security_context_present = secure;
  context.request_id = secure ? "sbsql-v3-metrics-show-probe-secure" : "sbsql-v3-metrics-show-probe-open";
  context.database_path = args.path;
  context.database_uuid.canonical = "00000000-0000-7000-8000-000000001021";
  context.session_uuid.canonical = "00000000-0000-7000-8000-000000001022";
  return context;
}

bool HasDiagnosticCode(const EngineApiResult& result, const std::string& code) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.code == code) { return true; }
  }
  return false;
}

bool HasEvidence(const EngineApiResult& result, const std::string& kind, const std::string& id = {}) {
  for (const auto& evidence : result.evidence) {
    if (evidence.evidence_kind == kind && (id.empty() || evidence.evidence_id == id)) { return true; }
  }
  return false;
}

bool HasRows(const EngineApiResult& result) { return result.ok && !result.result_shape.rows.empty(); }

void PrintBool(const std::string& name, bool value, bool comma) {
  std::cout << "  \"" << name << "\": " << (value ? "true" : "false") << (comma ? "," : "") << "\n";
}

}  // namespace

int main(int argc, char** argv) {
  Args args;
  if (!ParseArgs(argc, argv, &args)) {
    std::cerr << "usage: sb_sbsql_v3_metrics_show_probe --path PATH [--overwrite]\n";
    return 2;
  }
  if (args.overwrite) { std::filesystem::remove(args.path); }
  { std::ofstream bootstrap(args.path, std::ios::binary | std::ios::app); }

  const auto open_context = Context(args, false);
  EngineShowMetricsRequest restricted_metrics;
  restricted_metrics.context = open_context;
  restricted_metrics.option_envelopes.push_back("family:restricted");
  const auto restricted_metrics_result = EngineShowMetrics(restricted_metrics);
  const bool restricted_metrics_denied = !restricted_metrics_result.ok &&
                                         HasDiagnosticCode(restricted_metrics_result, "SB_ENGINE_API_SECURITY_CONTEXT_REQUIRED");

  EngineShowVersionRequest show_version;
  show_version.context = open_context;
  EngineShowDatabaseRequest show_database;
  show_database.context = open_context;
  EngineShowSystemRequest show_system;
  show_system.context = open_context;
  EngineShowSessionsRequest show_sessions;
  show_sessions.context = open_context;
  EngineShowLocksRequest show_locks;
  show_locks.context = open_context;
  EngineShowStatementsRequest show_statements;
  show_statements.context = open_context;

  const bool baseline_show_ok = HasRows(EngineShowVersion(show_version)) && HasRows(EngineShowDatabase(show_database)) &&
                                HasRows(EngineShowSystem(show_system)) && HasRows(EngineShowSessions(show_sessions)) &&
                                HasRows(EngineShowLocks(show_locks)) && HasRows(EngineShowStatements(show_statements));

  auto secure_context = Context(args, true);
  EngineBeginTransactionRequest begin;
  begin.context = secure_context;
  begin.isolation_level = "read_committed";
  const auto tx = EngineBeginTransaction(begin);
  secure_context.local_transaction_id = tx.local_transaction_id;
  secure_context.transaction_uuid = tx.transaction_uuid;

  EngineShowTransactionsRequest show_transactions;
  show_transactions.context = secure_context;
  const auto transactions_result = EngineShowTransactions(show_transactions);

  EngineShowMetricsRequest show_metrics;
  show_metrics.context = secure_context;
  show_metrics.option_envelopes.push_back("family:restricted");
  const auto metrics_result = EngineShowMetrics(show_metrics);

  EngineExplainOperationRequest explain;
  explain.context = secure_context;
  explain.operation_id = "dml.select_rows";
  explain.target_object.uuid.canonical = "00000000-0000-7000-8000-000000001023";
  explain.target_object.object_kind = "table";
  const auto explain_result = EngineExplainOperation(explain);

  EngineCommitTransactionRequest commit;
  commit.context = secure_context;
  const bool committed = EngineCommitTransaction(commit).ok;

  const bool metrics_ok = metrics_result.ok && HasEvidence(metrics_result, "metrics_registry", "local_node") &&
                          HasEvidence(metrics_result, "metrics_scope", "permission_checked");
  const bool transactions_ok = transactions_result.ok && HasEvidence(transactions_result, "transaction_rows");
  const bool explain_ok = explain_result.ok && HasEvidence(explain_result, "explain", "dml.select_rows");
  const bool ok = restricted_metrics_denied && baseline_show_ok && tx.ok && transactions_ok &&
                  metrics_ok && explain_ok && committed;

  std::cout << "{\n";
  PrintBool("ok", ok, true);
  PrintBool("restricted_metrics_denied", restricted_metrics_denied, true);
  PrintBool("baseline_show_ok", baseline_show_ok, true);
  PrintBool("metrics_ok", metrics_ok, true);
  PrintBool("explain_ok", explain_ok, false);
  std::cout << "}\n";
  return ok ? 0 : 1;
}

