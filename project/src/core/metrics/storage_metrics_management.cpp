// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "storage_metrics_management.hpp"

#include "uuid.hpp"
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

  if (!core::uuid::IsEngineIdentityUuid(request.database_uuid) ||
      !core::uuid::IsEngineIdentityUuid(request.filespace_uuid) ||
      !core::uuid::IsEngineIdentityUuid(request.node_uuid)) {
    AddDiagnostic(&result, "SB-STORAGE-METRICS-NATIVE-IDENTITY-REQUIRED");
    return result;
  }
  auto& registry = DefaultMetricRegistry();
  if (!registry.ObservationOwnerMatches(request.database_uuid, request.node_uuid)) {
    AddDiagnostic(&result, "METRIC.OBSERVATION_SOURCE_UNAVAILABLE:storage metric owner is not bound");
    return result;
  }

  for (MetricValue value : registry.SnapshotCurrent(false)) {
    if (!StorageMetricFamily(value.family)) {
      continue;
    }
    // Scope comes from the retained observation, never from a requested label
    // or fabricated sample. Node-wide observations have no filespace identity.
    MetricUuid observed_filespace;
    bool selected = true;
    for (const auto& label : value.labels) {
      const MetricUuid* expected = nullptr;
      if (label.key == "database_uuid") expected = &request.database_uuid;
      else if (label.key == "node_uuid") expected = &request.node_uuid;
      else if (label.key == "filespace_uuid") expected = &request.filespace_uuid;
      if (!expected) continue;
      const auto* identity = std::get_if<MetricUuid>(&label.value);
      if (!identity || !core::uuid::IsEngineIdentityUuid(*identity)) {
        result.visible_metrics.clear();
        result.support_bundle_records.clear();
        AddDiagnostic(&result, "SB-STORAGE-METRICS-BINARY-PROJECTION-REFUSED");
        return result;
      }
      if (*identity != *expected) selected = false;
      if (label.key == "filespace_uuid") observed_filespace = *identity;
    }
    if (!selected) continue;
    const MetricDescriptor* descriptor = registry.FindDescriptor(value.family);
    StorageMetricSupportRecord record;
    if (!descriptor || !ProjectMetricForSupport(
        *descriptor, value, request.allow_sensitive_labels, &record.metric, &value)) {
      result.visible_metrics.clear();
      result.support_bundle_records.clear();
      AddDiagnostic(&result, "SB-STORAGE-METRICS-BINARY-PROJECTION-REFUSED");
      return result;
    }
    result.visible_metrics.push_back(std::move(value));
    if (request.support_bundle_requested) {
      record.identities_redacted = !request.allow_sensitive_labels;
      if (request.allow_sensitive_labels) {
        record.database_uuid = request.database_uuid;
        record.filespace_uuid = observed_filespace;
      }
      record.local_path_redacted = !request.local_path_sample.empty();
      record.protected_payload_redacted = !request.protected_payload_sample.empty();
      result.redaction_applied = result.redaction_applied || record.identities_redacted ||
          record.local_path_redacted || record.protected_payload_redacted ||
          !record.metric.omitted_sensitive_labels.empty();
      result.support_bundle_records.push_back(std::move(record));
    }
  }

  return result;
}

}  // namespace scratchbird::core::metrics
