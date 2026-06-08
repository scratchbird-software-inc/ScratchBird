// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "cluster/cluster_inspect_api.hpp"

#include "cluster/cluster_provider_boundary.hpp"

namespace scratchbird::engine::internal_api {

// SEARCH_KEY: SB_ENGINE_INTERNAL_API_CLUSTER_CLUSTER_INSPECT_API_STUBS
EngineInspectClusterStateResult EngineInspectClusterState(const EngineInspectClusterStateRequest& request) {
  constexpr std::string_view kOperation = "cluster.inspect_state";
  auto result = EngineInspectClusterStateResult{};
  const auto provider_result = ExecuteInternalClusterProviderBoundary(
      request.context,
      kOperation,
      "SBLR_CLUSTER_INSPECT_STATE",
      "internal-api.cluster-inspect-state");
  if (RefuseIfClusterProviderBoundaryClosed(&result,
                                            provider_result,
                                            kOperation,
                                            "cluster_provider_boundary_closed")) {
    return result;
  }
  result.ok = false;
  result.operation_id = std::string(kOperation);
  result.cluster_authority_required = true;
  result.evidence.push_back({"cluster_inspect_state_refused",
                             "external_provider_result_not_mapped"});
  return result;
}

EngineInspectClusterRoutingPlanResult EngineInspectClusterRoutingPlan(const EngineInspectClusterRoutingPlanRequest& request) {
  constexpr std::string_view kOperation = "cluster.inspect_routing_plan";
  auto result = EngineInspectClusterRoutingPlanResult{};
  const auto provider_result = ExecuteInternalClusterProviderBoundary(
      request.context,
      kOperation,
      "SBLR_CLUSTER_INSPECT_ROUTING_PLAN",
      "internal-api.cluster-inspect-routing-plan");
  if (RefuseIfClusterProviderBoundaryClosed(&result,
                                            provider_result,
                                            kOperation,
                                            "cluster_provider_boundary_closed")) {
    return result;
  }
  result.ok = false;
  result.operation_id = std::string(kOperation);
  result.cluster_authority_required = true;
  result.evidence.push_back({"cluster_inspect_routing_plan_refused",
                             "external_provider_result_not_mapped"});
  return result;
}

}  // namespace scratchbird::engine::internal_api
