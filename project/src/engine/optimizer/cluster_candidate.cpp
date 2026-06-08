// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "cluster_candidate.hpp"

namespace scratchbird::engine::optimizer {
namespace planner = scratchbird::engine::planner;
namespace {

std::vector<std::string> MissingFacts(const ClusterCandidateFacts& facts) {
  std::vector<std::string> missing;
  if (!facts.cluster_authority_available) missing.push_back("cluster_authority_unavailable");
  if (!facts.route_generation_available) missing.push_back("route_generation_unavailable");
  if (!facts.remote_stats_available) missing.push_back("remote_stats_unavailable");
  if (!facts.safe_execution_fence_available) missing.push_back("safe_execution_fence_unavailable");
  if (!facts.remote_execution_available) missing.push_back("remote_execution_unavailable");
  return missing;
}

PlanCandidate BuildClusterCandidate(const char* candidate_id,
                                    planner::PhysicalAccessKind access_kind,
                                    const ClusterCandidateFacts& facts) {
  PlanCandidate candidate;
  candidate.candidate_id = candidate_id;
  candidate.access_kind = access_kind;
  candidate.scope = "cluster";
  candidate.cluster_candidate = true;
  candidate.required_facts = {"cluster_authority", "route_generation", "remote_stats", "safe_execution_fence", "remote_execution"};
  candidate.missing_facts = MissingFacts(facts);
  if (!candidate.missing_facts.empty()) {
    candidate.cost = RejectedCost(candidate.missing_facts.front());
    return candidate;
  }
  candidate.cost = EstimateNodeCost(planner::MakeLogicalPlanNode(planner::LogicalPlanNodeKind::kDmlRead, access_kind, "query.cluster_fragment", "cluster_candidate"));
  candidate.cost.reason = "cluster_candidate_ready_but_not_preferred_before_cluster_executor";
  candidate.cost.uncertainty_cost += 10000000ULL;
  candidate.cost.total_cost += candidate.cost.uncertainty_cost;
  return candidate;
}

}  // namespace

PlanCandidate BuildClusterFragmentCandidate(const ClusterCandidateFacts& facts) {
  return BuildClusterCandidate("CAND-OPT-013", planner::PhysicalAccessKind::kClusterFragmentScan, facts);
}

PlanCandidate BuildRemoteNodePushdownCandidate(const ClusterCandidateFacts& facts) {
  return BuildClusterCandidate("CAND-OPT-014", planner::PhysicalAccessKind::kRemoteNodePushdown, facts);
}

bool ClusterCandidateMayWin(const PlanCandidate& candidate) {
  return candidate.cluster_candidate && candidate.cost.selectable && candidate.missing_facts.empty();
}

}  // namespace scratchbird::engine::optimizer
