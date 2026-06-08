// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SEARCH_KEY: SB_METRICS_OPENMETRICS_EXPORTER

#include "metric_registry.hpp"

#include <string>

namespace scratchbird::core::metrics {

std::string ExportOpenMetrics(const MetricRegistry& registry, bool include_cluster);
std::string MetricFamilyToOpenMetricsName(std::string family);

}  // namespace scratchbird::core::metrics
