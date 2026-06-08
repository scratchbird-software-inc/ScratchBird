// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "nosql/graph_api.hpp"
#include "nosql/time_series_api.hpp"
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
    if (key == "--path") {
      args->path = value;
    } else {
      return false;
    }
  }
  return !args->path.empty();
}

EngineRequestContext Context(const Args& args) {
  EngineRequestContext context;
  context.trust_mode = EngineTrustMode::embedded_in_process;
  context.security_context_present = true;
  context.request_id = "sbsql-v3-graph-timeseries-probe";
  context.database_path = args.path;
  context.database_uuid.canonical = "00000000-0000-7000-8000-000000001301";
  context.session_uuid.canonical = "00000000-0000-7000-8000-000000001302";
  return context;
}

EngineRequestContext BeginContext(const Args& args) {
  auto context = Context(args);
  EngineBeginTransactionRequest begin;
  begin.context = context;
  begin.isolation_level = "read_committed";
  const auto tx = EngineBeginTransaction(begin);
  if (tx.ok) {
    context.local_transaction_id = tx.local_transaction_id;
    context.transaction_uuid = tx.transaction_uuid;
  }
  return context;
}

bool CommitContext(const EngineRequestContext& context) {
  EngineCommitTransactionRequest commit;
  commit.context = context;
  return EngineCommitTransaction(commit).ok;
}

bool HasEvidence(const EngineApiResult& result, const std::string& kind, const std::string& id = {}) {
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
    std::cerr << "usage: sb_sbsql_v3_graph_timeseries_probe --path PATH [--overwrite]\n";
    return 2;
  }
  if (args.overwrite) { std::filesystem::remove(args.path); }
  { std::ofstream bootstrap(args.path, std::ios::binary | std::ios::app); }

  const auto graph_context = Context(args);
  EngineGraphQueryRequest graph;
  graph.context = graph_context;
  graph.target_object.uuid.canonical = "00000000-0000-7000-8000-000000001311";
  graph.descriptors.push_back({{"00000000-0000-7000-8000-000000001312"}, "graph_descriptor", "property_graph", "edge_model=directed"});
  graph.option_envelopes.push_back("pattern:(customer)-[ordered]->(item)");
  const auto graph_result = EngineGraphQuery(graph);

  auto ts_context = BeginContext(args);
  EngineTimeSeriesAppendRequest append;
  append.context = ts_context;
  append.target_object.uuid.canonical = "00000000-0000-7000-8000-000000001321";
  append.target_object.object_kind = "time_series";
  append.localized_names.push_back({"en", "default", "/native/metrics", "cpu_usage", true});
  append.option_envelopes.push_back("point:timestamp=2026-05-01T00:00:00Z,value=42");
  const auto append_result = EngineTimeSeriesAppend(append);
  const bool ts_committed = ts_context.local_transaction_id != 0 && CommitContext(ts_context);

  EngineGraphQueryRequest cluster_graph;
  cluster_graph.context = graph_context;
  cluster_graph.option_envelopes.push_back("cross_node_graph:true");
  const auto cluster_graph_result = EngineGraphQuery(cluster_graph);

  EngineTimeSeriesAppendRequest cluster_ts;
  cluster_ts.context = graph_context;
  cluster_ts.option_envelopes.push_back("shard:timeseries_remote");
  const auto cluster_ts_result = EngineTimeSeriesAppend(cluster_ts);

  const bool graph_ok = graph_result.ok &&
                        !graph_result.result_shape.rows.empty() &&
                        HasEvidence(graph_result, "nosql_surface", "graph") &&
                        HasEvidence(graph_result, "nosql_behavior", "local_descriptor_scan");
  const bool timeseries_ok = append_result.ok &&
                             ts_committed &&
                             HasEvidence(append_result, "nosql_surface", "time_series") &&
                             HasEvidence(append_result, "nosql_behavior", "persisted_time_series_append");
  const bool cluster_denied = !cluster_graph_result.ok &&
                              cluster_graph_result.cluster_authority_required &&
                              !cluster_ts_result.ok &&
                              cluster_ts_result.cluster_authority_required;
  const bool ok = graph_ok && timeseries_ok && cluster_denied;

  std::cout << "{\n";
  PrintBool("ok", ok, true);
  PrintBool("graph_ok", graph_ok, true);
  PrintBool("timeseries_ok", timeseries_ok, true);
  PrintBool("cluster_denied", cluster_denied, false);
  std::cout << "}\n";
  return ok ? 0 : 1;
}
