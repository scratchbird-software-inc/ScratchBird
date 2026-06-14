// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "cluster/cluster_inspect_api.hpp"
#include "ddl/create_api.hpp"
#include "dml/insert_api.hpp"
#include "dml/select_api.hpp"
#include "observability/metrics_api.hpp"
#include "transaction/transaction_api.hpp"

#include <iostream>
#include <string>

using namespace scratchbird::engine::internal_api;

namespace {

bool HasDiagnostic(const EngineApiResult& result, const std::string& code) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.code == code) {
      return true;
    }
  }
  return false;
}

EngineRequestContext EmbeddedContext() {
  EngineRequestContext context;
  context.trust_mode = EngineTrustMode::embedded_in_process;
  context.request_id = "engine-api-smoke-probe";
  context.security_context_present = true;
  context.trace_tags.push_back("security.fixture_trace_authority");
  context.trace_tags.push_back("right:OBS_METRICS_READ_ALL");
  return context;
}

}  // namespace

int main() {
  const auto context = EmbeddedContext();

  EngineCreateTableRequest create_table;
  create_table.context = context;
  const auto create_result = EngineCreateTable(create_table);

  EngineInsertRowsRequest insert_rows;
  insert_rows.context = context;
  const auto insert_result = EngineInsertRows(insert_rows);

  EngineSelectRowsRequest select_rows;
  select_rows.context = context;
  const auto select_result = EngineSelectRows(select_rows);

  EngineBeginTransactionRequest begin_tx;
  begin_tx.context = context;
  const auto begin_result = EngineBeginTransaction(begin_tx);

  EngineShowMetricsRequest show_metrics;
  show_metrics.context = context;
  const auto metrics_result = EngineShowMetrics(show_metrics);

  EngineInspectClusterStateRequest cluster_state;
  cluster_state.context = context;
  const auto cluster_result = EngineInspectClusterState(cluster_state);

  const bool crud_deterministic = HasDiagnostic(create_result, "SB_ENGINE_API_INVALID_REQUEST") &&
                                  HasDiagnostic(insert_result, "SB_ENGINE_API_INVALID_REQUEST") &&
                                  HasDiagnostic(select_result, "SB_ENGINE_API_INVALID_REQUEST") &&
                                  HasDiagnostic(begin_result, "SB_ENGINE_API_INVALID_REQUEST");
  const bool non_crud_stubbed = metrics_result.ok && !HasDiagnostic(metrics_result, "SB_ENGINE_API_NOT_IMPLEMENTED");
  const bool cluster_fail_closed =
      (HasDiagnostic(cluster_result, "SB_ENGINE_API_CLUSTER_AUTHORITY_UNAVAILABLE") ||
       HasDiagnostic(cluster_result, "SBLR.CLUSTER.SUPPORT_NOT_ENABLED")) &&
      cluster_result.cluster_authority_required;
  const bool embedded_reported = create_result.embedded_trust_mode_observed &&
                                 HasDiagnostic(create_result, "SB_ENGINE_API_EMBEDDED_TRUST_MODE");
  const bool ok = crud_deterministic && non_crud_stubbed && cluster_fail_closed && embedded_reported;

  std::cout << "{\n";
  std::cout << "  \"ok\": " << (ok ? "true" : "false") << ",\n";
  std::cout << "  \"crud_deterministic\": " << (crud_deterministic ? "true" : "false") << ",\n";
  std::cout << "  \"non_crud_behavior_implemented\": " << (non_crud_stubbed ? "true" : "false") << ",\n";
  std::cout << "  \"cluster_fail_closed\": " << (cluster_fail_closed ? "true" : "false") << ",\n";
  std::cout << "  \"embedded_trust_mode_reported\": " << (embedded_reported ? "true" : "false") << "\n";
  std::cout << "}\n";
  return ok ? 0 : 1;
}
