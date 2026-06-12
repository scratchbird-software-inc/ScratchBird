// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "optimizer_barrier.hpp"

#include <string>
#include <vector>

namespace scratchbird::engine::optimizer {

// SEARCH_KEY: SB_OPTIMIZER_SAFETY_GATES
struct OptimizerSafetyGateInput {
  bool rls_present = false;
  bool masking_present = false;
  bool grants_proven = false;
  bool metadata_visible = false;
  bool function_safe = true;
  bool transaction_context_present = false;
  bool isolation_preserved = false;
  bool domain_rules_preserved = false;
  bool parser_boundary_clean = true;
};

struct OptimizerSafetyGateResult {
  bool ok = false;
  std::vector<std::string> diagnostics;
};

// SEARCH_KEY: OPCH_OPTIMIZER_PRODUCTION_TEST_SEPARATION
struct OptimizerProductionBuildGateInput {
  bool production_build = true;
  bool fixture_statistics_enabled = false;
  bool legacy_only_defaults_enabled = false;
  bool local_default_statistics_enabled = false;
  bool policy_default_statistics_enabled = false;
  bool reference_produced_evidence_enabled = false;
  bool relaxed_benchmark_clean_paths_enabled = false;
  bool relaxed_metrics_enabled = false;
  bool placeholder_runtime_evidence_enabled = false;
  bool synthetic_agent_recommendations_enabled = false;
  bool synthetic_feedback_enabled = false;
  bool parser_execution_shortcuts_enabled = false;
  bool forced_collision_hooks_enabled = false;
  bool cluster_stub_live_claims_enabled = false;
  bool debug_only_paths_enabled = false;
};

OptimizerSafetyGateResult EvaluateRlsMaskingGate(const OptimizerSafetyGateInput& input);
OptimizerSafetyGateResult EvaluateGrantMetadataGate(const OptimizerSafetyGateInput& input);
OptimizerSafetyGateResult EvaluateFunctionSafetyGate(const OptimizerSafetyGateInput& input);
OptimizerSafetyGateResult EvaluateTransactionIsolationGate(const OptimizerSafetyGateInput& input);
OptimizerSafetyGateResult EvaluateDomainGate(const OptimizerSafetyGateInput& input);
OptimizerSafetyGateResult EvaluateParserBoundaryGate(const OptimizerSafetyGateInput& input);
OptimizerSafetyGateResult EvaluateAllOptimizerSafetyGates(const OptimizerSafetyGateInput& input);
OptimizerSafetyGateResult EvaluateOptimizerProductionBuildGate(
    const OptimizerProductionBuildGateInput& input);

}  // namespace scratchbird::engine::optimizer
