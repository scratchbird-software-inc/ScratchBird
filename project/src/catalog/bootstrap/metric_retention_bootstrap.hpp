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
// identity-free retention templates. Installation and UUID assignment belong
// to the native catalog writer, not these definitions.

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

struct MetricRetentionBootstrapDefinition {
  std::string catalog_path = "sys.metrics.retention_policies";
  scratchbird::core::metrics::MetricRetentionPolicyDefinition definition;
};

std::vector<MetricCatalogBootstrapObject> MetricHistoryBootstrapObjects();
std::vector<MetricRetentionBootstrapDefinition> MetricRetentionBootstrapDefinitions();

}  // namespace scratchbird::catalog::bootstrap
