// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "metric_export.hpp"
#include "metric_contracts.hpp"
#include "metric_producer.hpp"
#include "metric_registry.hpp"

#include <iostream>
#include <set>
#include <string>
#include <vector>

namespace {

bool Require(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << "\n";
    return false;
  }
  return true;
}

}  // namespace

int main() {
  using namespace scratchbird::core::metrics;
  bool ok = true;
  auto& registry = DefaultMetricRegistry();
  const auto descriptors = registry.Descriptors(true);
  ok &= Require(!descriptors.empty(), "registry has descriptors");

  std::set<std::string> names;
  for (const auto& descriptor : descriptors) {
    ok &= Require(names.insert(descriptor.family).second, "duplicate descriptor " + descriptor.family);
    ok &= Require(!descriptor.namespace_path.empty(), "namespace missing " + descriptor.family);
    ok &= Require(!descriptor.producer_owner.empty(), "producer missing " + descriptor.family);
    if (!descriptor.cluster_only) {
      ok &= Require(descriptor.readiness != MetricReadiness::contract_ready_unwired,
                    "non-cluster metric is contract-ready but unwired " + descriptor.family);
    }
  }

  const std::vector<std::string> required = {
      "sb_metric_samples_rejected_total",
      "sb_memory_allocated_bytes",
      "sb_storage_device_read_latency_microseconds",
      "sb_tx_active_transactions",
      "sb_auth_failures_total",
      "sb_identity_auth_attempts_total",
      "sb_archive_slice_age_microseconds",
      "sb_archive_slice_count",
      "sb_archive_slice_bytes",
      "sb_cluster_rolling_upgrade_readiness_state",
      "sb_identity_users_online",
      "sb_listener_sessions_active",
      "sb_management_frontend_requests_total",
      "sb_archive_health_state",
      "sb_archive_delta_lag_transactions",
      "sb_archive_delta_apply_lag_transactions",
      "sb_archive_checksum_failures_total",
      "sb_archive_restore_refusals_total",
      "sb_backup_in_progress",
      "sb_export_adapter_failures_total",
      "sb_query_fragment_propagation_delay_microseconds",
      "sb_query_fragment_local_connection_delay_microseconds",
      "sb_query_fragment_sample_freshness_microseconds",
      "sb_filespace_total_bytes",
      "sb_filespace_free_bytes",
      "sb_filespace_health_state",
      "sb_filespace_role_state",
      "sb_filespace_device_error_total",
      "sb_page_free_count",
      "sb_page_allocated_count",
      "sb_page_allocation_latency_microseconds",
      "sb_page_allocation_failures_total",
      "sb_page_relocation_ready_for_filespace_shrink",
  };
  for (const auto& family : required) {
    ok &= Require(registry.FindDescriptorOrAlias(family) != nullptr, "required family missing " + family);
  }

  ok &= Require(IncrementCounter("sb_metric_samples_rejected_total",
                                 Labels({{"metric_family", "probe"}, {"reason", "probe"}}),
                                 1.0,
                                 "metrics_runtime").ok,
                "counter update accepted");
  ok &= Require(SetGauge("sb_memory_allocated_bytes",
                         Labels({{"component", "probe"}}),
                         42.0,
                         "core_memory").ok,
                "gauge update accepted");
  ok &= Require(ObserveHistogram("sb_storage_device_read_latency_microseconds",
                                 Labels({{"component", "probe"}, {"operation", "read_at"}, {"result", "ok"}}),
                                 12.0,
                                 "storage_disk").ok,
                "histogram update accepted");
  ok &= Require(!IncrementCounter("sb_no_such_metric_total", {}, 1.0, "probe").ok,
                "unknown family rejected");
  ok &= Require(IncrementCounter("sb_parser_failures_total",
                                 Labels({{"component", "probe"}, {"parser_family", "native_v3"}, {"reason", "probe"}}),
                                 1.0,
                                 "parser_listener").ok,
                "parser producer contract accepts bounded sample");
  ok &= Require(RecordAgentAction("memory_governor", "probe", "ok").ok,
                "agent producer contract accepts bounded agent action");
  ok &= Require(ObserveAgentDecisionLatency(7.0, "memory_governor", "probe").ok,
                "agent producer contract accepts bounded decision latency");
  ok &= Require(RecordAlertFired("warning", "degraded", "OPS").ok,
                "alert producer contract accepts alert event");
  ok &= Require(!RecordAgentAction("", "probe", "ok").ok,
                "agent producer identity is required");
  ok &= Require(!PublishBackupProgressPercent(101.0, "backup").ok,
                "backup percent range is enforced before producer emission");
  ok &= Require(PublishArchiveLagBytes(42.0, "primary", "probe").ok,
                "archive producer contract accepts bounded sample");
  ok &= Require(PublishArchiveSliceCount(1.0, "primary", "probe").ok,
                "archive slice count producer contract accepts bounded sample");
  ok &= Require(PublishArchiveSliceBytes(4096.0, "primary", "probe").ok,
                "archive slice bytes producer contract accepts bounded sample");
  ok &= Require(PublishArchiveDeltaLagTransactions(2.0, "local_delta", "probe").ok,
                "archive delta lag producer contract accepts bounded sample");
  ok &= Require(PublishArchiveDeltaApplyLagTransactions(1.0, "local_delta", "probe").ok,
                "archive delta apply lag producer contract accepts bounded sample");
  ok &= Require(RecordArchiveChecksumFailure("local_delta", "probe").ok,
                "archive checksum failure producer contract accepts bounded sample");
  ok &= Require(RecordArchiveRestoreRefusal("local_delta", "probe").ok,
                "archive restore refusal producer contract accepts bounded sample");
  ok &= Require(PublishFilespaceCapacitySnapshot(1024.0,
                                                 256.0,
                                                 768.0,
                                                 "database:probe",
                                                 "filespace:probe",
                                                 "node:probe",
                                                 "active_primary",
                                                 "file").ok,
                "filespace capacity producer contract accepts bounded sample");
  ok &= Require(PublishFilespaceHealthState(1.0,
                                            "healthy",
                                            "database:probe",
                                            "filespace:probe",
                                            "node:probe",
                                            "active_primary",
                                            "file").ok,
                "filespace health producer contract accepts bounded sample");
  ok &= Require(ObservePageAllocationLatency(3.0,
                                             "database:probe",
                                             "filespace:probe",
                                             "node:probe",
                                             "data",
                                             "row_data").ok,
                "page allocation latency producer contract accepts bounded sample");
  ok &= Require(RecordPageAllocationFailure("probe_failure",
                                            "database:probe",
                                            "filespace:probe",
                                            "node:probe",
                                            "data",
                                            "row_data").ok,
                "page allocation failure producer contract accepts bounded sample");

  for (const auto& descriptor : descriptors) {
    if (descriptor.cluster_only || descriptor.readiness == MetricReadiness::derived) {
      continue;
    }
    MetricLabelSet labels;
    for (const auto& label : descriptor.labels) {
      if (label.required) {
        labels.push_back({label.key, "probe"});
      }
    }
    MetricValidationResult sample;
    switch (descriptor.type) {
      case MetricType::counter:
        sample = IncrementCounter(descriptor.family, labels, 1.0, descriptor.producer_owner);
        break;
      case MetricType::gauge:
        sample = SetGauge(descriptor.family, labels, 1.0, descriptor.producer_owner);
        break;
      case MetricType::histogram:
        sample = ObserveHistogram(descriptor.family, labels, 1.0, descriptor.producer_owner);
        break;
      case MetricType::state:
        sample = SetState(descriptor.family, labels, 1.0, "probe", descriptor.producer_owner);
        break;
      case MetricType::derived:
        sample = MetricOk();
        break;
    }
    ok &= Require(sample.ok, "non-cluster descriptor accepts owner sample " + descriptor.family);
  }

  const auto exported = ExportOpenMetrics(registry, true);
  ok &= Require(exported.find("sb_storage_device_read_latency_microseconds_bucket") != std::string::npos,
                "histogram bucket exported");
  ok &= Require(exported.find("# EOF") != std::string::npos, "openmetrics EOF exported");
  (void)SetGauge("sb_identity_sessions_active",
                 Labels({{"component", "probe"}, {"provider_family", "local_password"}, {"session_uuid", "session-secret"}}),
                 1.0,
                 "security_session");
  const auto redacted = ExportOpenMetrics(registry, false);
  ok &= Require(redacted.find("session-secret") == std::string::npos,
                "sensitive labels redacted from OpenMetrics export");

  if (!ok) {
    return 1;
  }
  std::cout << "metrics registry probe passed\n";
  return 0;
}
