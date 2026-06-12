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

namespace scratchbird::core::metrics {

inline constexpr const char* kStorageMetricsManagementKey =
    "MDF-018-CURRENT-CORE-STORAGE-METRICS-MANAGEMENT";

struct StorageMetricsManagementRequest {
  bool metrics_read_authorized = false;
  bool support_bundle_requested = false;
  bool allow_sensitive_labels = false;
  u64 observed_metric_generation = 0;
  u64 current_metric_generation = 0;
  std::string database_uuid;
  std::string filespace_uuid;
  std::string node_uuid;
  std::string local_path_sample;
  std::string protected_payload_sample;
};

struct StorageMetricsManagementResult {
  bool ok = false;
  std::vector<std::string> diagnostics;
  std::vector<MetricValue> visible_metrics;
  std::vector<std::string> support_bundle_lines;
  bool stale_invalidated = false;
  bool redaction_applied = false;
};

StorageMetricsManagementResult PublishStorageMetricsManagementSurface(
    const StorageMetricsManagementRequest& request);

}  // namespace scratchbird::core::metrics
