// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "optimizer_safety_gates.hpp"

#include <utility>

namespace scratchbird::engine::optimizer {
namespace {

OptimizerSafetyGateResult Result(bool ok, std::string diagnostic = {}) {
  OptimizerSafetyGateResult result;
  result.ok = ok;
  if (!diagnostic.empty()) result.diagnostics.push_back(std::move(diagnostic));
  return result;
}

void Merge(OptimizerSafetyGateResult* target, const OptimizerSafetyGateResult& source) {
  target->ok = target->ok && source.ok;
  target->diagnostics.insert(target->diagnostics.end(), source.diagnostics.begin(), source.diagnostics.end());
}

}  // namespace

OptimizerSafetyGateResult EvaluateRlsMaskingGate(const OptimizerSafetyGateInput& input) {
  if ((input.rls_present || input.masking_present) && !input.grants_proven) return Result(false, "SB_OPT_GATE_RLS_MASKING_GRANTS_REQUIRED");
  return Result(true);
}

OptimizerSafetyGateResult EvaluateGrantMetadataGate(const OptimizerSafetyGateInput& input) {
  if (!input.grants_proven) return Result(false, "SB_OPT_GATE_GRANTS_NOT_PROVEN");
  if (!input.metadata_visible) return Result(false, "SB_OPT_GATE_METADATA_NOT_VISIBLE");
  return Result(true);
}

OptimizerSafetyGateResult EvaluateFunctionSafetyGate(const OptimizerSafetyGateInput& input) {
  return input.function_safe ? Result(true) : Result(false, "SB_OPT_GATE_FUNCTION_NOT_SAFE");
}

OptimizerSafetyGateResult EvaluateTransactionIsolationGate(const OptimizerSafetyGateInput& input) {
  if (!input.transaction_context_present) return Result(false, "SB_OPT_GATE_TRANSACTION_CONTEXT_REQUIRED");
  if (!input.isolation_preserved) return Result(false, "SB_OPT_GATE_ISOLATION_NOT_PRESERVED");
  return Result(true);
}

OptimizerSafetyGateResult EvaluateDomainGate(const OptimizerSafetyGateInput& input) {
  return input.domain_rules_preserved ? Result(true) : Result(false, "SB_OPT_GATE_DOMAIN_RULES_NOT_PRESERVED");
}

OptimizerSafetyGateResult EvaluateParserBoundaryGate(const OptimizerSafetyGateInput& input) {
  return input.parser_boundary_clean ? Result(true) : Result(false, "SB_OPT_GATE_PARSER_AUTHORITY_REJECTED");
}

OptimizerSafetyGateResult EvaluateAllOptimizerSafetyGates(const OptimizerSafetyGateInput& input) {
  OptimizerSafetyGateResult result;
  result.ok = true;
  Merge(&result, EvaluateRlsMaskingGate(input));
  Merge(&result, EvaluateGrantMetadataGate(input));
  Merge(&result, EvaluateFunctionSafetyGate(input));
  Merge(&result, EvaluateTransactionIsolationGate(input));
  Merge(&result, EvaluateDomainGate(input));
  Merge(&result, EvaluateParserBoundaryGate(input));
  return result;
}

OptimizerSafetyGateResult EvaluateOptimizerProductionBuildGate(
    const OptimizerProductionBuildGateInput& input) {
  OptimizerSafetyGateResult result;
  result.ok = true;
  if (!input.production_build) return result;
  if (input.fixture_statistics_enabled) {
    result.ok = false;
    result.diagnostics.push_back("SB_OPT_PRODUCTION_GATE_FIXTURE_STATS_FORBIDDEN");
  }
  if (input.legacy_only_defaults_enabled) {
    result.ok = false;
    result.diagnostics.push_back("SB_OPT_PRODUCTION_GATE_LEGACY_DEFAULTS_FORBIDDEN");
  }
  if (input.local_default_statistics_enabled) {
    result.ok = false;
    result.diagnostics.push_back("SB_OPT_PRODUCTION_GATE_LOCAL_DEFAULT_STATS_FORBIDDEN");
  }
  if (input.policy_default_statistics_enabled) {
    result.ok = false;
    result.diagnostics.push_back("SB_OPT_PRODUCTION_GATE_POLICY_DEFAULT_STATS_FORBIDDEN");
  }
  if (input.donor_produced_evidence_enabled) {
    result.ok = false;
    result.diagnostics.push_back("SB_OPT_PRODUCTION_GATE_DONOR_EVIDENCE_FORBIDDEN");
  }
  if (input.relaxed_benchmark_clean_paths_enabled) {
    result.ok = false;
    result.diagnostics.push_back("SB_OPT_PRODUCTION_GATE_RELAXED_BENCHMARK_FORBIDDEN");
  }
  if (input.relaxed_metrics_enabled) {
    result.ok = false;
    result.diagnostics.push_back("SB_OPT_PRODUCTION_GATE_RELAXED_METRICS_FORBIDDEN");
  }
  if (input.placeholder_runtime_evidence_enabled) {
    result.ok = false;
    result.diagnostics.push_back("SB_OPT_PRODUCTION_GATE_PLACEHOLDER_RUNTIME_EVIDENCE_FORBIDDEN");
  }
  if (input.synthetic_agent_recommendations_enabled) {
    result.ok = false;
    result.diagnostics.push_back("SB_OPT_PRODUCTION_GATE_SYNTHETIC_AGENT_FORBIDDEN");
  }
  if (input.synthetic_feedback_enabled) {
    result.ok = false;
    result.diagnostics.push_back("SB_OPT_PRODUCTION_GATE_SYNTHETIC_FEEDBACK_FORBIDDEN");
  }
  if (input.parser_execution_shortcuts_enabled) {
    result.ok = false;
    result.diagnostics.push_back("SB_OPT_PRODUCTION_GATE_PARSER_EXECUTION_FORBIDDEN");
  }
  if (input.forced_collision_hooks_enabled) {
    result.ok = false;
    result.diagnostics.push_back("SB_OPT_PRODUCTION_GATE_FORCED_COLLISION_HOOKS_FORBIDDEN");
  }
  if (input.cluster_stub_live_claims_enabled) {
    result.ok = false;
    result.diagnostics.push_back("SB_OPT_PRODUCTION_GATE_CLUSTER_STUB_LIVE_CLAIMS_FORBIDDEN");
  }
  if (input.debug_only_paths_enabled) {
    result.ok = false;
    result.diagnostics.push_back("SB_OPT_PRODUCTION_GATE_DEBUG_ONLY_PATHS_FORBIDDEN");
  }
  return result;
}

}  // namespace scratchbird::engine::optimizer
