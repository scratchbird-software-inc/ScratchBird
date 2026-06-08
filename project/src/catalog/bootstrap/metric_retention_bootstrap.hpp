// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SEARCH_KEY: SB_METRICS_HISTORY_BOOTSTRAP
// Catalog/bootstrap descriptors for metric history objects and baseline
// retention policies installed into new databases.

#include "metric_retention_policy.hpp"

#include <string>
#include <vector>

namespace scratchbird::catalog::bootstrap {

struct MetricCatalogBootstrapObject {
  std::string canonical_path;
  std::string object_kind;
  std::string default_language = "en";
  std::string default_name;
  bool engine_owned = true;
};

struct MetricRetentionBootstrapPolicyRow {
  std::string catalog_path = "sys.metrics.retention_policies";
  scratchbird::core::metrics::MetricRetentionPolicy policy;
};

std::vector<MetricCatalogBootstrapObject> MetricHistoryBootstrapObjects();
std::vector<MetricRetentionBootstrapPolicyRow> MetricRetentionBootstrapPolicyRows();

}  // namespace scratchbird::catalog::bootstrap
