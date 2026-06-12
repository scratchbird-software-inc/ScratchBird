// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "join_planner_full.hpp"
#include "logical_plan.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace scratchbird::engine::optimizer {

// SEARCH_KEY: OEIC_JOIN_STRATEGY_CONTROL_ENTERPRISE
struct EnterpriseJoinControlAuthority {
  bool engine_optimizer_policy_authority = true;
  bool normalized_sblr_or_api_metadata = true;
  bool parser_execution_authority = false;
  bool raw_sql_text_authority = false;
  bool reference_or_legacy_authority = false;
  bool client_finality_or_visibility_authority = false;
  bool metric_finality_or_visibility_authority = false;
  bool recovery_authority = false;
  bool cluster_authority = false;
  bool fixture_or_test_authority = false;
};

struct EnterpriseJoinControlRequest {
  scratchbird::engine::planner::OptimizerPolicyMetadata policy_metadata;
  std::size_t relation_count = 0;
  std::uint64_t runtime_memory_budget_bytes = 0;
  std::uint64_t max_transition_budget = 0;
  bool production_mode = true;
  EnterpriseJoinControlAuthority authority;
};

struct EnterpriseJoinControlResult {
  bool ok = false;
  JoinSearchPolicy join_policy;
  std::string refusal_diagnostic;
  std::vector<std::string> diagnostics;
  std::vector<std::string> evidence;
};

EnterpriseJoinControlResult BuildEnterpriseJoinSearchPolicy(
    const EnterpriseJoinControlRequest& request);

bool ValidateEnterpriseJoinStrategyTelemetry(const EnterpriseJoinControlResult& controls,
                                             const JoinOrderPlan& plan,
                                             std::vector<std::string>* diagnostics);

}  // namespace scratchbird::engine::optimizer
