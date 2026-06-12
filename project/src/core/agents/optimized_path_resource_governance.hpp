// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SEARCH_KEY: ORH_RESOURCE_GOVERNANCE_STARVATION_GATE
// Composes engine resource-governance admission with workload quota
// reservations for optimized runtime paths. This records operational evidence
// only; it never owns MGA visibility, finality, commit, rollback, recovery, or
// parser/reference authority.

#include "agent_workload_resource_quota.hpp"
#include "resource_governance_admission.hpp"

#include <string>
#include <vector>

namespace scratchbird::core::agents {

enum class OptimizedPathResourceSurface {
  unknown = 0,
  compression,
  cache,
  vector_maintenance,
  nosql_provider,
  streaming_cursor,
  background_job
};

struct OptimizedPathResourceGovernanceRequest {
  std::string operation_id;
  OptimizedPathResourceSurface surface = OptimizedPathResourceSurface::unknown;
  ResourceGovernanceAdmissionRequest resource_admission;
  WorkloadResourceQuotaController* workload_quota = nullptr;
  WorkloadAdmissionRequest workload_admission;
  bool workload_quota_required = true;
  bool foreground_protection_required = true;
  bool background_workload = false;
  std::string foreground_pool_id;
  bool foreground_capacity_reserved = false;
  bool index_runtime_dependent = false;
  bool index_runtime_correctness_proven = false;
  std::string exact_index_runtime_blocker;
};

struct OptimizedPathResourceGovernanceResult {
  AgentRuntimeStatus status;
  ResourceGovernanceAdmissionResult resource_admission;
  WorkloadAdmissionResult workload_admission;
  bool admitted = false;
  bool throttled = false;
  bool queued = false;
  bool rejected = false;
  bool fail_closed = false;
  bool slowdown = false;
  bool exact_fallback = false;
  bool cancelled = false;
  bool foreground_protected = false;
  bool reservation_created = false;
  bool index_runtime_closure_claimed = false;
  std::string decision;
  std::string diagnostic_code;
  std::vector<std::string> evidence;
};

const char* OptimizedPathResourceSurfaceName(
    OptimizedPathResourceSurface surface);

OptimizedPathResourceGovernanceResult GovernOptimizedPathResources(
    const OptimizedPathResourceGovernanceRequest& request);

std::string SerializeOptimizedPathResourceGovernanceEvidence(
    const OptimizedPathResourceGovernanceResult& result);

}  // namespace scratchbird::core::agents
