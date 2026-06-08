// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "metric_contracts.hpp"
#include "metric_history.hpp"
#include "metric_producer.hpp"
#include "observability/metrics_api.hpp"
#include "sb_metrics_history_probe_support.hpp"

#include <iostream>
#include <string>
#include <vector>

namespace {

scratchbird::engine::internal_api::EngineRequestContext NoMetricsRightsContext() {
  scratchbird::engine::internal_api::EngineRequestContext context;
  context.security_context_present = true;
  context.database_uuid.canonical = "018f0000-0000-7000-8000-000000000101";
  context.session_uuid.canonical = "018f0000-0000-7000-8000-000000000102";
  context.principal_uuid.canonical = "018f0000-0000-7000-8000-000000000103";
  context.transaction_uuid.canonical = "018f0000-0000-7000-8000-000000000104";
  return context;
}

scratchbird::engine::internal_api::EngineRequestContext ExportContext() {
  auto context = scratchbird::tools::metrics_history_probe::MetricsContext(false, false, true);
  context.trace_tags.push_back("right:OBS_METRICS_EXPORT");
  return context;
}

bool HasEditablePolicy(const scratchbird::engine::internal_api::EngineApiResult& result, bool editable) {
  const std::string needle = editable ? "true" : "false";
  for (const auto& row : result.result_shape.rows) {
    bool row_has_editable = false;
    bool row_matches = false;
    for (const auto& field : row.fields) {
      if (field.first == "editable") {
        row_has_editable = true;
        row_matches = field.second.encoded_value == needle;
      }
    }
    if (row_has_editable && row_matches) {
      return true;
    }
  }
  return false;
}

}  // namespace

int main() {
  using namespace scratchbird::core::metrics;
  using namespace scratchbird::engine::internal_api;
  using namespace scratchbird::tools::metrics_history_probe;

  const auto path = TempHistoryPath("sb_metrics_security_probe");
  RemoveTempHistory(path);
  bool ok = true;
  ok &= Require(ConfigureMetricHistoryPersistence(path).ok, "history persistence configured");
  ok &= Require(PublishIdentitySessionsActive(1.0,
                                              "local_password",
                                              "self",
                                              Labels({{"session_uuid", "secret-session"},
                                                      {"principal_uuid", "secret-principal"}})).ok,
                "sensitive metric emitted");

  EngineSysMetricsRegistryRequest registry;
  registry.context = NoMetricsRightsContext();
  ok &= Require(!EngineSysMetricsRegistry(registry).ok, "registry denied without metrics read right");
  registry.context = MetricsContext(false, false, false);
  ok &= Require(EngineSysMetricsRegistry(registry).ok, "registry allowed with metrics read right");

  EngineSysMetricsCurrentRequest current;
  current.context = NoMetricsRightsContext();
  ok &= Require(!EngineSysMetricsCurrent(current).ok, "current denied without metrics read right");
  current.context = MetricsContext(false, false, false);
  ok &= Require(EngineSysMetricsCurrent(current).ok, "current allowed with metrics read right");

  EngineSysMetricsHistoryRequest history;
  history.context = NoMetricsRightsContext();
  history.option_envelopes.push_back("history_path:" + path);
  ok &= Require(!EngineSysMetricsHistory(history).ok, "history denied without metrics read right");
  history.context = MetricsContext(false, false, false);
  auto history_redacted = EngineSysMetricsHistory(history);
  ok &= Require(history_redacted.ok, "history allowed with metrics read right");
  ok &= Require(!ContainsValue(history_redacted, "secret-session") && ContainsValue(history_redacted, "<redacted>"),
                "history redacts sensitive labels without broad/export right");
  history.context = MetricsContext(false, false, true);
  auto history_sensitive = EngineSysMetricsHistory(history);
  ok &= Require(history_sensitive.ok && ContainsValue(history_sensitive, "secret-session"),
                "history reveals sensitive labels with broad metrics right");

  EngineSysMetricsPersistentHistoryRequest persistent;
  persistent.context = NoMetricsRightsContext();
  persistent.option_envelopes.push_back("history_path:" + path);
  ok &= Require(!EngineSysMetricsPersistentHistory(persistent).ok, "persistent history denied without metrics read right");
  persistent.context = MetricsContext(false, false, false);
  ok &= Require(EngineSysMetricsPersistentHistory(persistent).ok, "persistent history allowed with metrics read right");

  EngineSysMetricsRollupsRequest rollups;
  rollups.context = NoMetricsRightsContext();
  rollups.option_envelopes.push_back("history_path:" + path);
  ok &= Require(!EngineSysMetricsRollups(rollups).ok, "rollups denied without metrics read right");
  rollups.context = MetricsContext(false, false, false);
  ok &= Require(EngineSysMetricsRollups(rollups).ok, "rollups allowed with metrics read right");

  EngineSysMetricsSeriesRequest series;
  series.context = NoMetricsRightsContext();
  series.option_envelopes.push_back("history_path:" + path);
  ok &= Require(!EngineSysMetricsSeries(series).ok, "series denied without metrics read right");
  series.context = MetricsContext(false, false, false);
  auto series_redacted = EngineSysMetricsSeries(series);
  ok &= Require(series_redacted.ok && !ContainsValue(series_redacted, "secret-session"),
                "series redacts sensitive labels without broad/export right");
  series.context = MetricsContext(false, false, true);
  auto series_sensitive = EngineSysMetricsSeries(series);
  ok &= Require(series_sensitive.ok && ContainsValue(series_sensitive, "secret-session"),
                "series reveals sensitive labels with broad metrics right");

  EngineSysMetricsRetentionPoliciesRequest policies;
  policies.context = NoMetricsRightsContext();
  policies.option_envelopes.push_back("history_path:" + path);
  ok &= Require(!EngineSysMetricsRetentionPolicies(policies).ok, "retention policies denied without read/control right");
  policies.context = MetricsContext(false, false, false);
  auto policies_read = EngineSysMetricsRetentionPolicies(policies);
  ok &= Require(policies_read.ok && HasEditablePolicy(policies_read, false),
                "retention policies read-only with read right only");
  policies.context = MetricsContext(false, true, false);
  auto policies_control = EngineSysMetricsRetentionPolicies(policies);
  ok &= Require(policies_control.ok && HasEditablePolicy(policies_control, true),
                "retention policies editable with retention control right");

  EngineAlterMetricRetentionPolicyRequest alter;
  alter.context = MetricsContext(false, false, false);
  alter.option_envelopes = {"history_path:" + path, "policy_name:security_probe", "mode:current_only"};
  ok &= Require(!EngineAlterMetricRetentionPolicy(alter).ok, "retention edit denied without control right");
  alter.context = MetricsContext(false, true, false);
  auto alter_result = EngineAlterMetricRetentionPolicy(alter);
  ok &= Require(alter_result.ok && ContainsValue(alter_result, "security_probe"),
                "retention edit allowed with control right");

  EngineSysMetricsLabelsRequest labels;
  labels.context = NoMetricsRightsContext();
  ok &= Require(!EngineSysMetricsLabels(labels).ok, "labels denied without metrics read right");
  labels.context = MetricsContext(false, false, false);
  ok &= Require(EngineSysMetricsLabels(labels).ok, "labels allowed with metrics read right");

  EngineSysMetricsProducersRequest producers;
  producers.context = NoMetricsRightsContext();
  ok &= Require(!EngineSysMetricsProducers(producers).ok, "producers denied without metrics read right");
  producers.context = MetricsContext(false, false, false);
  ok &= Require(EngineSysMetricsProducers(producers).ok, "producers allowed with metrics read right");

  EngineInspectMetricAdapterContractRequest adapter;
  adapter.context = NoMetricsRightsContext();
  adapter.option_envelopes.push_back("format:openmetrics");
  ok &= Require(!EngineInspectMetricAdapterContract(adapter).ok, "adapter contract denied without export right");
  adapter.context = ExportContext();
  ok &= Require(EngineInspectMetricAdapterContract(adapter).ok, "adapter contract allowed with export right");

  EngineClusterSysMetricsRegistryRequest cluster_registry;
  cluster_registry.context = MetricsContext(false, false, false);
  ok &= Require(!EngineClusterSysMetricsRegistry(cluster_registry).ok, "cluster registry fails closed without cluster authority");
  cluster_registry.context = MetricsContext(true, false, false);
  ok &= Require(EngineClusterSysMetricsRegistry(cluster_registry).ok, "cluster registry allowed with cluster authority and read right");

  EngineClusterSysMetricsHistoryRequest cluster_history;
  cluster_history.context = MetricsContext(false, false, false);
  cluster_history.option_envelopes.push_back("history_path:" + path);
  ok &= Require(!EngineClusterSysMetricsHistory(cluster_history).ok, "cluster history fails closed without cluster authority");
  cluster_history.context = MetricsContext(true, false, false);
  ok &= Require(EngineClusterSysMetricsHistory(cluster_history).ok, "cluster history allowed with cluster authority and read right");

  RemoveTempHistory(path);
  if (!ok) return 1;
  std::cout << "metrics security probe passed\n";
  return 0;
}
