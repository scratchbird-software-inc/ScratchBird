// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "datatype_metrics_redaction.hpp"

#include "uuid.hpp"

namespace scratchbird::core::datatypes {
namespace {

namespace metrics = scratchbird::core::metrics;

bool StartsWith(const std::string& value, const std::string& prefix) {
  return value.rfind(prefix, 0) == 0;
}

void AddDiagnostic(DatatypeMetricsManagementResult* result,
                   std::string diagnostic) {
  result->diagnostics.push_back(std::move(diagnostic));
  result->ok = false;
}

bool DatatypeMetricFamily(const std::string& family) {
  return StartsWith(family, "sb_datatype") ||
         StartsWith(family, "sb_domain_method");
}

}  // namespace

DatatypeMetricsManagementResult PublishDatatypeMetricsManagementSurface(
    const DatatypeMetricsManagementRequest& request) {
  DatatypeMetricsManagementResult result;
  result.ok = true;

  if (!request.metrics_read_authorized) {
    AddDiagnostic(&result, "SB-DATATYPE-METRICS-VISIBILITY-REFUSED");
    return result;
  }

  auto& registry = metrics::DefaultMetricRegistry();
  if (!core::uuid::IsEngineIdentityUuid(request.database_uuid) ||
      !core::uuid::IsEngineIdentityUuid(request.node_uuid) ||
      !registry.ObservationOwnerMatches(request.database_uuid, request.node_uuid)) {
    AddDiagnostic(&result, "METRIC.OBSERVATION_SOURCE_UNAVAILABLE:datatype metric owner is not bound");
    return result;
  }

  for (metrics::MetricValue value : registry.SnapshotCurrent(false)) {
    if (!DatatypeMetricFamily(value.family)) {
      continue;
    }
    const metrics::MetricDescriptor* descriptor =
        registry.FindDescriptor(value.family);
    DatatypeMetricSupportRecord record;
    if (!descriptor || !metrics::ProjectMetricForSupport(
        *descriptor, value, request.allow_sensitive_labels, &record.metric, &value)) {
      result.visible_metrics.clear();
      result.support_bundle_records.clear();
      AddDiagnostic(&result, "SB-DATATYPE-METRICS-BINARY-PROJECTION-REFUSED");
      return result;
    }
    result.visible_metrics.push_back(std::move(value));
    if (request.support_bundle_requested) {
      record.principal_redacted = !request.allow_sensitive_labels && !request.principal_uuid.is_nil();
      if (request.allow_sensitive_labels) record.principal_uuid = request.principal_uuid;
      record.protected_payload_redacted = !request.protected_payload_sample.empty();
      result.redaction_applied = result.redaction_applied || record.principal_redacted ||
          record.protected_payload_redacted || !record.metric.omitted_sensitive_labels.empty();
      result.support_bundle_records.push_back(std::move(record));
    }
  }

  return result;
}

}  // namespace scratchbird::core::datatypes
