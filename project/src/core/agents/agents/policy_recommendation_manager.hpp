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

enum class PolicyRecommendationManagerDecisionKind : u32 {
  no_action,
  create_policy_recommendation,
  refused
};

struct PolicyRecommendationManagerPolicy {
  bool present = true;
  bool valid = true;
  bool scope_compatible = true;
  bool recommendations_allowed = true;
  u64 slo_burn_rate_threshold_per_mille = 1200;
  u64 min_policy_evaluations = 10;
};

struct PolicyRecommendationManagerSnapshot {
  std::string policy_family;
  u64 policy_evaluations_total = 0;
  u64 workload_slo_burn_rate_per_mille = 0;
  bool policy_metrics_authoritative = false;
  bool recommendation_target_valid = false;
  bool redaction_policy_valid = true;
  bool parser_authority = false;
  bool client_authority = false;
};

struct PolicyRecommendationManagerEvidenceField {
  std::string key;
  std::string value;
};

struct PolicyRecommendationManagerResult {
  Status status;
  DiagnosticRecord diagnostic;
  PolicyRecommendationManagerDecisionKind decision =
      PolicyRecommendationManagerDecisionKind::refused;
  std::vector<PolicyRecommendationManagerEvidenceField> evidence;
  bool fail_closed = true;
  bool advisory_only = true;

  bool ok() const { return status.ok() && !fail_closed; }
};

const char* PolicyRecommendationManagerDecisionKindName(
    PolicyRecommendationManagerDecisionKind decision);
PolicyRecommendationManagerResult EvaluatePolicyRecommendationManager(
    const PolicyRecommendationManagerSnapshot& snapshot,
    const PolicyRecommendationManagerPolicy& policy = {});
DiagnosticRecord MakePolicyRecommendationManagerDiagnostic(
    Status status,
    std::string diagnostic_code,
    std::string message_key,
    std::string detail = {});

const char* policy_recommendation_manager_implementation_anchor();

}  // namespace scratchbird::core::agents::implemented_agents
