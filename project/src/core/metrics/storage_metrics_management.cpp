// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "storage_metrics_management.hpp"

#include "metric_contracts.hpp"

#include <sstream>
#include <utility>

namespace scratchbird::core::metrics {
namespace {

bool StartsWith(const std::string& value, const std::string& prefix) {
  return value.rfind(prefix, 0) == 0;
}

void AddDiagnostic(StorageMetricsManagementResult* result,
                   std::string diagnostic) {
  result->diagnostics.push_back(std::move(diagnostic));
  result->ok = false;
}

void AddMetricFailure(StorageMetricsManagementResult* result,
                      const MetricValidationResult& status) {
  if (!status.ok) {
    AddDiagnostic(result, status.diagnostic_code + ":" + status.detail);
  }
}

MetricLabelSet Labels(std::initializer_list<MetricLabel> labels) {
  return MetricLabelSet(labels.begin(), labels.end());
}

std::string Redacted(const std::string& value, bool allow_sensitive) {
  if (value.empty()) {
    return "none";
  }
  return allow_sensitive ? value : "[redacted]";
}

bool StorageMetricFamily(const std::string& family) {
  return StartsWith(family, "sb_filespace") ||
         StartsWith(family, "sb_page") ||
         StartsWith(family, "sb_archive") ||
         StartsWith(family, "sb_backup") ||
         StartsWith(family, "sb_restore") ||
         StartsWith(family, "sb_storage") ||
         StartsWith(family, "sb_temp") ||
         StartsWith(family, "sb_index_build");
}

std::string RenderSupportBundleLine(const MetricValue& metric,
                                    const StorageMetricsManagementRequest& request) {
  std::ostringstream out;
  out << "namespace=sys.metrics.storage"
      << ";family=" << metric.family
      << ";database_uuid=" << Redacted(request.database_uuid,
                                       request.allow_sensitive_labels)
      << ";filespace_uuid=" << Redacted(request.filespace_uuid,
                                        request.allow_sensitive_labels)
      << ";local_path=" << Redacted(request.local_path_sample, false)
      << ";protected_payload=" << Redacted(request.protected_payload_sample, false)
      << ";value=" << metric.value;
  return out.str();
}

}  // namespace

StorageMetricsManagementResult PublishStorageMetricsManagementSurface(
    const StorageMetricsManagementRequest& request) {
  StorageMetricsManagementResult result;
  result.ok = true;

  if (!request.metrics_read_authorized) {
    AddDiagnostic(&result, "SB-STORAGE-METRICS-VISIBILITY-REFUSED");
    return result;
  }
  if (request.observed_metric_generation == 0 ||
      request.current_metric_generation == 0 ||
      request.observed_metric_generation != request.current_metric_generation) {
    result.stale_invalidated = true;
    AddDiagnostic(&result, "SB-STORAGE-METRICS-STALE-GENERATION-REFUSED");
    return result;
  }

  const std::string database_uuid =
      request.database_uuid.empty() ? "database-storage-metrics" : request.database_uuid;
  const std::string filespace_uuid =
      request.filespace_uuid.empty() ? "filespace-storage-metrics" : request.filespace_uuid;
  const std::string node_uuid =
      request.node_uuid.empty() ? "node-storage-metrics" : request.node_uuid;

  AddMetricFailure(&result, PublishFilespaceCapacitySnapshot(1024 * 1024, 512 * 1024, 512 * 1024, database_uuid, filespace_uuid, node_uuid, "active_primary", "ssd"));
  AddMetricFailure(&result, PublishFilespaceReservedBytes(64 * 1024, database_uuid, filespace_uuid, node_uuid, "active_primary", "ssd", "safety_margin"));
  AddMetricFailure(&result, PublishFilespaceHealthState(1, "healthy", database_uuid, filespace_uuid, node_uuid, "active_primary", "ssd"));
  AddMetricFailure(&result, PublishFilespaceRoleState(1, "active_primary", database_uuid, filespace_uuid, node_uuid, "active_primary", "ssd"));
  AddMetricFailure(&result, ObserveFilespaceDeviceReadLatency(42, database_uuid, filespace_uuid, node_uuid, "active_primary", "ssd"));
  AddMetricFailure(&result, ObserveFilespaceDeviceWriteLatency(43, database_uuid, filespace_uuid, node_uuid, "active_primary", "ssd"));
  AddMetricFailure(&result, ObserveFilespaceFsyncLatency(44, database_uuid, filespace_uuid, node_uuid, "active_primary", "ssd"));
  AddMetricFailure(&result, RecordFilespaceDeviceError("none", database_uuid, filespace_uuid, node_uuid, "active_primary", "ssd"));
  AddMetricFailure(&result, PublishPageAllocationSnapshot(100, 200, database_uuid, filespace_uuid, node_uuid, "row_data", "heap"));
  AddMetricFailure(&result, PublishPageReleasedFreeCount(7, database_uuid, filespace_uuid, node_uuid, "row_data", "heap"));
  AddMetricFailure(&result, PublishPageReservedCount(11, database_uuid, filespace_uuid, node_uuid, "row_data", "heap", "preallocation"));
  AddMetricFailure(&result, ObservePageAllocationLatency(45, database_uuid, filespace_uuid, node_uuid, "row_data", "heap"));
  AddMetricFailure(&result, RecordPageAllocationFailure("none", database_uuid, filespace_uuid, node_uuid, "row_data", "heap"));
  AddMetricFailure(&result, PublishPageCacheSnapshot(20, 327680, 2, 1, database_uuid, filespace_uuid, "row_data"));
  AddMetricFailure(&result, RecordPageCacheEviction(database_uuid, filespace_uuid, "row_data", "pressure"));
  AddMetricFailure(&result, PublishArchiveLagBytes(0, "local", "none"));
  AddMetricFailure(&result, PublishBackupInProgress(1, "backup"));
  AddMetricFailure(&result, PublishBackupProgressPercent(50, "backup"));
  AddMetricFailure(&result, ObserveRestoreDrillDuration(1000, "ok"));

  auto& registry = DefaultMetricRegistry();
  AddMetricFailure(&result, registry.SetGauge("sb_filespace_shrink_candidate_bytes", Labels({{"component", "storage.filespace"}, {"database_uuid", database_uuid}, {"filespace_uuid", filespace_uuid}, {"node_uuid", node_uuid}, {"filespace_role", "active_primary"}, {"device_class", "ssd"}}), 128 * 1024, "storage_filespace"));
  AddMetricFailure(&result, registry.SetGauge("sb_filespace_truncate_ready_bytes", Labels({{"component", "storage.filespace"}, {"database_uuid", database_uuid}, {"filespace_uuid", filespace_uuid}, {"node_uuid", node_uuid}, {"filespace_role", "active_primary"}, {"device_class", "ssd"}}), 32 * 1024, "page_runtime"));
  AddMetricFailure(&result, registry.SetGauge("sb_page_fragmentation_ratio", Labels({{"component", "storage.page"}, {"database_uuid", database_uuid}, {"filespace_uuid", filespace_uuid}, {"node_uuid", node_uuid}, {"page_family", "row_data"}, {"page_type", "heap"}}), 0.125, "page_runtime"));
  AddMetricFailure(&result, registry.IncrementCounter("sb_storage_pressure_total", Labels({{"component", "storage.pressure"}, {"database_uuid", database_uuid}, {"filespace_uuid", filespace_uuid}, {"node_uuid", node_uuid}, {"reason", "safety_margin"}}), 1.0, "metrics_runtime"));
  AddMetricFailure(&result, registry.SetGauge("sb_temp_workspace_bytes", Labels({{"component", "storage.temp"}, {"database_uuid", database_uuid}, {"filespace_uuid", filespace_uuid}, {"node_uuid", node_uuid}, {"reason", "work_table"}}), 4096, "metrics_runtime"));
  AddMetricFailure(&result, registry.SetGauge("sb_index_build_workspace_bytes", Labels({{"component", "storage.index_build"}, {"database_uuid", database_uuid}, {"filespace_uuid", filespace_uuid}, {"node_uuid", node_uuid}, {"reason", "sort_run"}}), 8192, "metrics_runtime"));
  AddMetricFailure(&result, registry.IncrementCounter("sb_storage_support_redaction_total", Labels({{"component", "storage.redaction"}, {"database_uuid", database_uuid}, {"filespace_uuid", filespace_uuid}, {"node_uuid", node_uuid}, {"reason", "support_bundle"}}), 1.0, "metrics_runtime"));

  if (!result.diagnostics.empty()) {
    return result;
  }

  for (MetricValue value : registry.SnapshotCurrent(false)) {
    if (!StorageMetricFamily(value.family)) {
      continue;
    }
    const MetricDescriptor* descriptor = registry.FindDescriptor(value.family);
    if (descriptor != nullptr) {
      value = RedactSensitiveMetricValue(*descriptor, std::move(value), request.allow_sensitive_labels);
    }
    result.visible_metrics.push_back(value);
    if (request.support_bundle_requested) {
      result.support_bundle_lines.push_back(RenderSupportBundleLine(value, request));
    }
  }

  result.redaction_applied = request.support_bundle_requested;
  return result;
}

}  // namespace scratchbird::core::metrics
