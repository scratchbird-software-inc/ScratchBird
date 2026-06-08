// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "agent_runtime.hpp"

#include <string>
#include <vector>

namespace scratchbird::core::agents::implemented_agents {

using scratchbird::core::platform::DiagnosticRecord;
using scratchbird::core::platform::Status;
using scratchbird::core::platform::u32;
using scratchbird::core::platform::u64;

enum class RuntimeLearningAgentDecisionKind : u32 {
  no_action,
  recommend_planner_correction,
  quarantine_learning_shape,
  refused
};

struct RuntimeLearningAgentPolicy {
  bool present = true;
  bool valid = true;
  bool scope_compatible = true;
  bool planner_correction_allowed = true;
  u64 estimate_error_threshold_per_mille = 2000;
  u64 min_runtime_samples = 3;
};

struct RuntimeLearningAgentSnapshot {
  std::string query_shape_digest;
  u64 runtime_samples = 0;
  u64 estimate_error_ratio_per_mille = 0;
  bool feedback_authoritative = false;
  bool exact_result_fallback_present = false;
  bool mga_recheck_preserved = true;
  bool security_recheck_preserved = true;
  bool parser_authority = false;
  bool benchmark_authority = false;
};

struct RuntimeLearningAgentEvidenceField {
  std::string key;
  std::string value;
};

struct RuntimeLearningAgentResult {
  Status status;
  DiagnosticRecord diagnostic;
  RuntimeLearningAgentDecisionKind decision =
      RuntimeLearningAgentDecisionKind::refused;
  std::vector<RuntimeLearningAgentEvidenceField> evidence;
  bool fail_closed = true;
  bool advisory_only = true;

  bool ok() const { return status.ok() && !fail_closed; }
};

const char* RuntimeLearningAgentDecisionKindName(
    RuntimeLearningAgentDecisionKind decision);
RuntimeLearningAgentResult EvaluateRuntimeLearningAgent(
    const RuntimeLearningAgentSnapshot& snapshot,
    const RuntimeLearningAgentPolicy& policy = {});
DiagnosticRecord MakeRuntimeLearningAgentDiagnostic(Status status,
                                                    std::string diagnostic_code,
                                                    std::string message_key,
                                                    std::string detail = {});

const char* runtime_learning_agent_implementation_anchor();

}  // namespace scratchbird::core::agents::implemented_agents
