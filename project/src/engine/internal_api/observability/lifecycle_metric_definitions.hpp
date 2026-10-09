// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "metric_registry.hpp"

namespace scratchbird::engine::internal_api {
// Finite producer schemas only. Catalog/runtime owners supply identities,
// policy bindings and series admission independently of these definitions.
inline const std::vector<core::metrics::MetricDescriptorDefinition>& LifecycleMetricDefinitions() {
  namespace m = core::metrics;
  static const auto definitions = [] {
    std::vector<m::MetricDescriptorDefinition> result;
    for (const auto* family : {"sb_lifecycle_operation_total", "sb_lifecycle_diagnostic_total",
                              "sb_lifecycle_audit_event_total", "sb_lifecycle_cache_invalidation_total"}) {
      m::MetricDescriptorDefinition definition;
      definition.family = family;
      definition.type = m::MetricType::counter;
      definition.value_type = m::MetricScalarType::uint64;
      definition.unit = m::MetricUnit::none;
      definition.producer_owner = "database_lifecycle_observability";
      definition.security_family = "OBS_METRICS_READ_FAMILY";
      definition.namespace_path = "sys.metrics.lifecycle";
      definition.labels = {{"operation", true, false, m::MetricLabelType::text}};
      if (definition.family == "sb_lifecycle_cache_invalidation_total") {
        definition.namespace_path += ".cache";
        definition.labels.push_back({"cache_family", true, false, m::MetricLabelType::text});
        definition.labels.push_back({"reason", true, false, m::MetricLabelType::text});
      } else {
        definition.labels.push_back({"result", true, false, m::MetricLabelType::text});
        if (definition.family != "sb_lifecycle_diagnostic_total") {
          definition.labels.push_back({"route_class", true, false, m::MetricLabelType::text});
          definition.labels.push_back({"database_uuid", true, false, m::MetricLabelType::system_uuid});
          definition.labels.push_back({"session_uuid", true, true, m::MetricLabelType::system_uuid});
        }
        if (definition.family != "sb_lifecycle_operation_total") {
          definition.labels.push_back({"diagnostic_code", true, false, m::MetricLabelType::text});
          definition.namespace_path += definition.family == "sb_lifecycle_diagnostic_total" ? ".diagnostics" : ".audit";
        }
      }
      result.push_back(std::move(definition));
    }
    return result;
  }();
  return definitions;
}
}  // namespace scratchbird::engine::internal_api
