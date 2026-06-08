// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "metric_registry.hpp"

#include <string>
#include <vector>

namespace scratchbird::engine::optimizer {

// SEARCH_KEY: OEIC_OPTIMIZER_METRIC_OWNERSHIP_MATRIX
// Code-owned optimizer metric manifest. This is not metric data and it is not
// optimizer authority. It records the required producer, consumer, freshness,
// retention, redaction, and support-bundle policy for each noncluster optimizer
// metric family. Benchmark-clean planning may consume only metrics that this
// manifest marks as production-consumable and that have registered descriptors.

enum class OptimizerMetricProducerState {
  live_maintained,
  owned_runtime_required,
  derived_from_registered_sources,
  cluster_external
};

enum class OptimizerMetricFreshnessPolicy {
  per_operator_completion,
  per_route_execution,
  per_metric_snapshot,
  per_catalog_epoch,
  per_storage_generation,
  per_index_generation,
  per_transaction_horizon,
  per_support_bundle_capture
};

enum class OptimizerMetricRetentionClass {
  route_short_window,
  plan_feedback_history,
  catalog_statistics_history,
  support_bundle_window,
  security_audit_window
};

enum class OptimizerMetricRedactionClass {
  public_aggregate,
  redacted_scope,
  protected_digest,
  security_restricted
};

enum class OptimizerMetricSupportBundleClass {
  included_redacted,
  digest_only,
  protected_summary,
  omitted_from_default_bundle
};

struct OptimizerEnterpriseMetricEntry {
  std::string metric_family;
  std::string registry_family;
  scratchbird::core::metrics::MetricType metric_type =
      scratchbird::core::metrics::MetricType::gauge;
  scratchbird::core::metrics::MetricUnit metric_unit =
      scratchbird::core::metrics::MetricUnit::count;
  std::string producer_owner;
  std::string consumer_owner;
  std::string producer_anchor;
  std::string consumer_anchor;
  std::vector<std::string> required_evidence;
  OptimizerMetricProducerState producer_state =
      OptimizerMetricProducerState::owned_runtime_required;
  OptimizerMetricFreshnessPolicy freshness_policy =
      OptimizerMetricFreshnessPolicy::per_metric_snapshot;
  OptimizerMetricRetentionClass retention_class =
      OptimizerMetricRetentionClass::route_short_window;
  OptimizerMetricRedactionClass redaction_class =
      OptimizerMetricRedactionClass::redacted_scope;
  OptimizerMetricSupportBundleClass support_bundle_class =
      OptimizerMetricSupportBundleClass::included_redacted;
  bool enterprise_route_consumable = false;
  bool benchmark_clean_consumable = false;
};

struct OptimizerMetricManifestValidation {
  bool ok = false;
  std::vector<std::string> diagnostics;
};

const std::vector<OptimizerEnterpriseMetricEntry>&
OptimizerEnterpriseMetricManifest();

OptimizerMetricManifestValidation ValidateOptimizerEnterpriseMetricManifest();

scratchbird::core::metrics::MetricValidationResult
EnsureOptimizerEnterpriseMetricDescriptors(
    scratchbird::core::metrics::MetricRegistry* registry = nullptr);

const char* OptimizerMetricProducerStateName(OptimizerMetricProducerState value);
const char* OptimizerMetricFreshnessPolicyName(OptimizerMetricFreshnessPolicy value);
const char* OptimizerMetricRetentionClassName(OptimizerMetricRetentionClass value);
const char* OptimizerMetricRedactionClassName(OptimizerMetricRedactionClass value);
const char* OptimizerMetricSupportBundleClassName(OptimizerMetricSupportBundleClass value);

}  // namespace scratchbird::engine::optimizer
