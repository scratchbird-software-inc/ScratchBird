// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "datatype_metrics_redaction.hpp"

#include "metric_contracts.hpp"

#include <sstream>

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

std::string Redacted(const std::string& value, bool allow_sensitive) {
  if (value.empty()) {
    return "none";
  }
  return allow_sensitive ? value : "[redacted]";
}

std::string RenderSupportBundleLine(const metrics::MetricValue& metric,
                                    const std::string& principal_uuid,
                                    const std::string& protected_payload,
                                    bool allow_sensitive) {
  std::ostringstream out;
  out << "namespace=sys.metrics.datatypes"
      << ";family=" << metric.family
      << ";principal_uuid=" << Redacted(principal_uuid, allow_sensitive)
      << ";protected_payload=" << Redacted(protected_payload, false)
      << ";value=" << metric.value;
  return out.str();
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
    if (descriptor != nullptr) {
      value = metrics::RedactSensitiveMetricValue(
          *descriptor, std::move(value), request.allow_sensitive_labels);
    }
    result.visible_metrics.push_back(value);
    if (request.support_bundle_requested) {
      result.support_bundle_lines.push_back(RenderSupportBundleLine(
          value,
          request.principal_uuid,
          request.protected_payload_sample,
          request.allow_sensitive_labels));
    }
  }

  result.redaction_applied = request.support_bundle_requested;
  return result;
}

}  // namespace scratchbird::core::datatypes
