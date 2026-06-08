// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <string>
#include <vector>

namespace scratchbird::engine::optimizer {

// SEARCH_KEY: OEIC_CLUSTER_OPTIMIZER_EXTERNAL_BOUNDARY
// Core optimizer cluster behavior is claim-control and refusal behavior only.
// Live cluster routing, live cluster metrics, distributed scheduling, remote
// pushdown execution, and cluster failure policy belong behind the external
// sb_cluster_provider ABI.

struct ClusterOptimizerBoundaryRequest {
  bool cluster_route_requested = false;
  bool cluster_metric_requested = false;
  bool external_provider_available = false;
  bool public_stub_provider = false;
  bool production_live_claim = false;
  bool benchmark_clean_claim = false;
  std::string operation_id;
};

struct ClusterOptimizerBoundaryResult {
  bool ok = false;
  bool externally_routed = false;
  bool refused = false;
  std::string diagnostic_code;
  std::vector<std::string> evidence;
};

ClusterOptimizerBoundaryResult EvaluateClusterOptimizerBoundary(
    const ClusterOptimizerBoundaryRequest& request);

}  // namespace scratchbird::engine::optimizer
