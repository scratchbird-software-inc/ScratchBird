// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "metric_history.hpp"
#include "observability/metrics_api.hpp"
#include "sb_metrics_history_probe_support.hpp"

#include <iostream>

int main() {
  using namespace scratchbird::core::metrics;
  using namespace scratchbird::engine::internal_api;
  using namespace scratchbird::tools::metrics_history_probe;
  const auto path = TempHistoryPath("sb_metrics_retention_policy_probe");
  RemoveTempHistory(path);
  bool ok = true;
  ok &= Require(ConfigureMetricHistoryPersistence(path).ok, "history persistence configured");
  EngineSysMetricsRetentionPoliciesRequest read_request;
  read_request.context = MetricsContext(false, false);
  read_request.option_envelopes.push_back("history_path:" + path);
  auto read_result = EngineSysMetricsRetentionPolicies(read_request);
  ok &= Require(read_result.ok && ContainsValue(read_result, "metrics_current_only"), "baseline policies readable");
  EngineAlterMetricRetentionPolicyRequest denied;
  denied.context = MetricsContext(false, false);
  denied.option_envelopes = {"history_path:" + path, "policy_name:probe_policy", "mode:current_only"};
  ok &= Require(!EngineAlterMetricRetentionPolicy(denied).ok, "policy edit denied without retention control");
  EngineAlterMetricRetentionPolicyRequest allowed;
  allowed.context = MetricsContext(false, true);
  allowed.option_envelopes = {"history_path:" + path,
                              "policy_name:probe_policy",
                              "mode:current_only",
                              "purge_batch_limit:10",
                              "max_cardinality:10"};
  auto edit_result = EngineAlterMetricRetentionPolicy(allowed);
  ok &= Require(edit_result.ok && ContainsValue(edit_result, "probe_policy"), "policy edit allowed with retention control");
  auto store = LoadMetricHistoryStore(path);
  ok &= Require(!store.evidence.empty(), "policy edit wrote evidence");
  RemoveTempHistory(path);
  if (!ok) return 1;
  std::cout << "metrics retention policy probe passed\n";
  return 0;
}
