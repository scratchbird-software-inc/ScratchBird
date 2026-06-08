// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

//
#include "cluster_refusal_path.hpp"

namespace scratchbird::engine::optimizer {

ClusterOptimizerBoundaryResult EvaluateClusterOptimizerBoundary(
    const ClusterOptimizerBoundaryRequest& request) {
  ClusterOptimizerBoundaryResult result;
  result.evidence.push_back("OEIC_CLUSTER_OPTIMIZER_EXTERNAL_BOUNDARY");
  result.evidence.push_back("cluster_optimizer_core_execution_authority=false");
  result.evidence.push_back("cluster_metrics_core_maintenance=false");

  if (!request.cluster_route_requested && !request.cluster_metric_requested) {
    result.ok = true;
    result.diagnostic_code = "SB_OPT_CLUSTER_BOUNDARY.NOT_REQUESTED";
    result.evidence.push_back("cluster_boundary_not_engaged");
    return result;
  }

  if (request.benchmark_clean_claim) {
    result.refused = true;
    result.diagnostic_code = "SB_OPT_CLUSTER_BOUNDARY.BENCHMARK_CLEAN_CORE_REFUSED";
    result.evidence.push_back("cluster_stub_or_external_evidence_cannot_close_core_benchmark_clean_claim");
    return result;
  }

  if (request.public_stub_provider && request.production_live_claim) {
    result.refused = true;
    result.diagnostic_code = "SB_OPT_CLUSTER_BOUNDARY.PUBLIC_STUB_LIVE_CLAIM_REFUSED";
    result.evidence.push_back("public_cluster_stub_is_test_only");
    return result;
  }

  if (!request.external_provider_available) {
    result.refused = true;
    result.diagnostic_code = request.cluster_metric_requested
                                 ? "SB_OPT_CLUSTER_BOUNDARY.CLUSTER_METRIC_EXTERNAL_PROVIDER_REQUIRED"
                                 : "SB_OPT_CLUSTER_BOUNDARY.CLUSTER_ROUTE_EXTERNAL_PROVIDER_REQUIRED";
    result.evidence.push_back("external_sb_cluster_provider_required");
    return result;
  }

  if (request.public_stub_provider) {
    result.refused = true;
    result.diagnostic_code = "SB_OPT_CLUSTER_BOUNDARY.PUBLIC_STUB_EXTERNAL_ROUTE_REFUSED";
    result.evidence.push_back("public_cluster_stub_cannot_satisfy_external_route");
    return result;
  }

  result.ok = true;
  result.externally_routed = true;
  result.diagnostic_code = "SB_OPT_CLUSTER_BOUNDARY.EXTERNAL_PROVIDER_ROUTE_REQUIRED";
  result.evidence.push_back("route_must_dispatch_through_sb_cluster_provider");
  result.evidence.push_back(request.operation_id.empty()
                                ? "operation_id=cluster_optimizer"
                                : "operation_id=" + request.operation_id);
  return result;
}

}  // namespace scratchbird::engine::optimizer
