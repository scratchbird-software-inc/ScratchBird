// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "access_path.hpp"

namespace scratchbird::engine::optimizer {

// SEARCH_KEY: SB_OPTIMIZER_CLUSTER_CANDIDATE_FAIL_CLOSED
struct ClusterCandidateFacts {
  bool cluster_authority_available = false;
  bool route_generation_available = false;
  bool remote_stats_available = false;
  bool safe_execution_fence_available = false;
  bool remote_execution_available = false;
};

PlanCandidate BuildClusterFragmentCandidate(const ClusterCandidateFacts& facts);
PlanCandidate BuildRemoteNodePushdownCandidate(const ClusterCandidateFacts& facts);
bool ClusterCandidateMayWin(const PlanCandidate& candidate);

}  // namespace scratchbird::engine::optimizer
