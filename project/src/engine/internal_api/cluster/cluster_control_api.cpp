// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "cluster/cluster_control_api.hpp"

#include "cluster/cluster_provider_boundary.hpp"

namespace scratchbird::engine::internal_api {

// SEARCH_KEY: SB_ENGINE_INTERNAL_API_CLUSTER_CLUSTER_CONTROL_API_STUBS
EngineControlClusterResult EngineControlCluster(const EngineControlClusterRequest& request) {
  constexpr std::string_view kOperation = "cluster.control_cluster";
  auto result = EngineControlClusterResult{};
  const auto provider_result = ExecuteInternalClusterProviderBoundary(
      request.context,
      kOperation,
      "SBLR_CLUSTER_CONTROL_CLUSTER",
      "internal-api.cluster-control");
  if (RefuseIfClusterProviderBoundaryClosed(&result,
                                            provider_result,
                                            kOperation,
                                            "cluster_provider_boundary_closed")) {
    return result;
  }
  result.ok = false;
  result.operation_id = std::string(kOperation);
  result.cluster_authority_required = true;
  result.evidence.push_back({"cluster_control_refused", "external_provider_result_not_mapped"});
  return result;
}

}  // namespace scratchbird::engine::internal_api
