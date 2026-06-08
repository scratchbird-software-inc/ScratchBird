// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "metric_retention_bootstrap.hpp"

#include <utility>

namespace scratchbird::catalog::bootstrap {

std::vector<MetricCatalogBootstrapObject> MetricHistoryBootstrapObjects() {
  return {
      {"sys.metrics.series", "metrics_surface", "en", "series", true},
      {"sys.metrics.history", "metrics_surface", "en", "history", true},
      {"sys.metrics.rollups", "metrics_surface", "en", "rollups", true},
      {"sys.metrics.retention_policies", "metrics_surface", "en", "retention_policies", true},
      {"sys.metrics.retention_evidence", "metrics_surface", "en", "retention_evidence", true},
  };
}

std::vector<MetricRetentionBootstrapPolicyRow> MetricRetentionBootstrapPolicyRows() {
  std::vector<MetricRetentionBootstrapPolicyRow> rows;
  for (auto policy : scratchbird::core::metrics::BaselineMetricRetentionPolicies()) {
    MetricRetentionBootstrapPolicyRow row;
    row.policy = std::move(policy);
    rows.push_back(std::move(row));
  }
  return rows;
}

}  // namespace scratchbird::catalog::bootstrap
