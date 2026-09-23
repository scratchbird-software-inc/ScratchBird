// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "datatype_metrics_redaction.hpp"

#include "metric_contracts.hpp"



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

void AddMetricFailure(DatatypeMetricsManagementResult* result,
                      const metrics::MetricValidationResult& status) {
  if (!status.ok) {
    AddDiagnostic(result, status.diagnostic_code + ":" + status.detail);
  }
}

metrics::MetricLabelSet Labels(std::initializer_list<metrics::MetricLabel> labels) {
  return metrics::MetricLabelSet(labels.begin(), labels.end());
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

  const std::string canonical_type = CanonicalTypeName(request.canonical_type);
  const std::string source_type = CanonicalTypeName(request.source_type);
  const std::string target_type = CanonicalTypeName(request.target_type);
  const std::string operation = request.operation.empty() ? "inspect" : request.operation;
  const std::string outcome = request.result.empty() ? "ok" : request.result;
  const std::string reason = request.reason.empty() ? "none" : request.reason;

  AddMetricFailure(&result,
                   metrics::RecordDatatypeOperation(canonical_type,
                                                    operation,
                                                    outcome,
                                                    reason));
  AddMetricFailure(&result,
                   metrics::RecordDatatypeCast(source_type,
                                               target_type,
                                               outcome,
                                               reason));
  AddMetricFailure(&result,
                   metrics::RecordDatatypeNumericBackend("sbl_numeric",
                                                         canonical_type,
                                                         operation,
                                                         outcome,
                                                         reason));
  AddMetricFailure(&result,
                   metrics::PublishDatatypeCatalogDescriptorCount(
                       static_cast<double>(BuiltinDatatypeDescriptors().size()),
                       outcome));

  auto& registry = metrics::DefaultMetricRegistry();
  AddMetricFailure(&result,
                   registry.IncrementCounter(
                       "sb_datatype_physical_encoding_total",
                       Labels({{"component", "datatype.physical"},
                               {"canonical_type", canonical_type},
                               {"operation", "encode"},
                               {"result", outcome},
                               {"reason", reason}}),
                       1.0,
                       "datatype_runtime"));
  AddMetricFailure(&result,
                   registry.IncrementCounter(
                       "sb_datatype_chunk_event_total",
                       Labels({{"component", "datatype.physical"},
                               {"canonical_type", canonical_type},
                               {"operation", "chunk"},
                               {"result", outcome},
                               {"reason", reason}}),
                       1.0,
                       "datatype_runtime"));
  AddMetricFailure(&result,
                   registry.IncrementCounter(
                       "sb_datatype_comparison_total",
                       Labels({{"component", "datatype.operations"},
                               {"canonical_type", canonical_type},
                               {"operation", "compare"},
                               {"result", outcome},
                               {"reason", reason}}),
                       1.0,
                       "datatype_runtime"));
  AddMetricFailure(&result,
                   registry.IncrementCounter(
                       "sb_datatype_locator_event_total",
                       Labels({{"component", "datatype.locator"},
                               {"canonical_type", canonical_type},
                               {"operation", "locator"},
                               {"result", outcome},
                               {"reason", reason}}),
                       1.0,
                       "datatype_runtime"));
  AddMetricFailure(&result,
                   registry.IncrementCounter(
                       "sb_datatype_redaction_total",
                       Labels({{"component", "datatype.redaction"},
                               {"canonical_type", canonical_type},
                               {"operation", "support_bundle"},
                               {"result", "redacted"},
                               {"reason", "protected_payload"}}),
                       1.0,
                       "datatype_runtime"));

  if (!result.diagnostics.empty()) {
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
