// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "agent_runtime.hpp"
#include "metric_registry.hpp"
#include "metric_retention_policy.hpp"

#include <string>
#include <vector>

namespace scratchbird::core::agents::implemented_agents {

using scratchbird::core::platform::DiagnosticRecord;
using scratchbird::core::platform::Status;
using scratchbird::core::platform::u32;
using scratchbird::core::platform::u64;

enum class MetricsRegistryManagerDecisionKind : u32 {
  accept_sample,
  reject_metric_sample,
  rollup_metrics,
  shed_export,
  refused
};

struct MetricsRegistryManagerPolicy {
  bool present = true;
  bool valid = true;
  bool scope_compatible = true;
  bool reject_bad_samples = true;
  bool rollup_allowed = true;
  bool export_shed_allowed = true;
  u64 max_label_cardinality = 256;
  u64 rollup_backlog_threshold = 10000;
  u64 export_queue_depth_threshold = 5000;
};

struct MetricsRegistryManagerSample {
  std::string metric_family;
  std::string namespace_path;
  u64 sample_count = 0;
  u64 label_cardinality = 0;
  u64 rollup_backlog = 0;
  u64 export_queue_depth = 0;
  double numeric_value = 0.0;
  std::string state_text;
  bool schema_compatible = true;
  bool source_trusted = true;
  bool scope_compatible = true;
  bool redaction_policy_valid = true;
  bool cluster_metric_route_requested = false;
  bool parser_authority = false;
  bool sidecar_authority = false;
};

struct MetricsRegistryManagerActionRequest {
  MetricsRegistryManagerSample sample;
  MetricsRegistryManagerPolicy policy;
  scratchbird::core::metrics::MetricRegistry* registry = nullptr;
  scratchbird::core::metrics::MetricLabelSet labels;
  std::string history_path;
  scratchbird::core::metrics::MetricRollupGrain rollup_grain =
      scratchbird::core::metrics::MetricRollupGrain::one_minute;
  u64 observation_time_microseconds = 0;
  u64 export_shed_count = 0;
  bool durable_history_required = true;
};

struct MetricsRegistryManagerEvidenceField {
  std::string key;
  std::string value;
};

struct MetricsRegistryManagerResult {
  Status status;
  DiagnosticRecord diagnostic;
  MetricsRegistryManagerDecisionKind decision =
      MetricsRegistryManagerDecisionKind::refused;
  std::vector<MetricsRegistryManagerEvidenceField> evidence;
  bool fail_closed = true;
  bool sample_accepted = false;
  bool sample_rejected = false;
  bool rollup_requested = false;
  bool export_shed_requested = false;
  bool registry_mutation_written = false;
  bool history_sample_written = false;
  bool rollup_written = false;
  bool export_shed_written = false;
  u64 rollup_rows_created = 0;
  u64 export_queue_depth_after_shed = 0;

  bool ok() const { return status.ok() && !fail_closed; }
};

const char* MetricsRegistryManagerDecisionKindName(
    MetricsRegistryManagerDecisionKind decision);
MetricsRegistryManagerResult EvaluateMetricsRegistryManagerSample(
    const MetricsRegistryManagerSample& sample,
    const MetricsRegistryManagerPolicy& policy = {});
MetricsRegistryManagerResult ApplyMetricsRegistryManagerAction(
    const MetricsRegistryManagerActionRequest& request);
DiagnosticRecord MakeMetricsRegistryManagerDiagnostic(Status status,
                                                      std::string diagnostic_code,
                                                      std::string message_key,
                                                      std::string detail = {});

const char* metrics_registry_manager_implementation_anchor();

}  // namespace scratchbird::core::agents::implemented_agents
