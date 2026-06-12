// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SEARCH_KEY: SB_AGENT_ADAPTIVE_TUNING_CONTROLLER_ODF_101
// Agent-owned adaptive tuning is advisory/resource-governance only. It selects
// bounded semantics-neutral knob values from metric and policy evidence and
// never owns transaction finality, visibility, parser execution, reference/provider
// authority, client autocommit, or recovery.

#include "adaptive_tuning_metrics_evidence.hpp"
#include "agent_runtime.hpp"
#include "resource_governance_admission.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace scratchbird::core::agents {

enum class AdaptiveTuningKnob : std::uint32_t {
  kUnknown,
  kPrefetchDepth,
  kMergeWorkers,
  kRefreshInterval,
  kCandidateBudget,
  kCachePartition,
  kEvidenceSampleRate,
  kVectorEfSearch,
  kVectorNprobe
};

enum class AdaptiveTuningActionClass : std::uint32_t {
  kRefuse,
  kHold,
  kIncrease,
  kDecrease,
  kReset,
  kDefault
};

struct AdaptiveTuningSafetyPolicy {
  std::string policy_id = "adaptive_tuning_policy_v1";
  bool policy_allowed = false;
  bool semantics_neutral_proof = false;
  bool advisory_resource_governance_only = false;
  bool mga_recheck_required = true;
  bool security_recheck_required = true;
  bool engine_mga_authoritative = false;
  bool security_snapshot_bound = false;
  bool grants_proven = false;
  bool reset_to_default_requested = false;
  bool use_default_requested = false;
  bool allow_increase = true;
  bool allow_decrease = true;
  bool allow_reset = true;
  bool allow_default = true;
  bool parser_or_reference_authority = false;
  bool provider_transaction_finality_authority = false;
  bool provider_visibility_authority = false;
  bool client_autocommit_authority = false;
  bool wal_recovery_authority = false;
};

struct AdaptiveTuningControllerRequest {
  AdaptiveTuningKnob knob = AdaptiveTuningKnob::kUnknown;
  std::string knob_label;
  u64 min_value = 0;
  u64 max_value = 0;
  u64 default_value = 0;
  u64 current_value = 0;
  AdaptiveTuningSafetyPolicy safety;
  scratchbird::core::metrics::AdaptiveTuningMetricEvidence metrics;
  ResourceGovernanceAdmissionRequest resource_governance;
};

struct AdaptiveTuningBudgetEvidence {
  u64 observed_latency_microseconds = 0;
  u64 latency_budget_microseconds = 0;
  u64 observed_memory_bytes = 0;
  u64 memory_budget_bytes = 0;
  u64 backlog_units = 0;
  u64 backlog_budget_units = 0;
  u64 error_count = 0;
  u64 throughput_units_per_second = 0;
  u64 quota_pressure_ppm = 0;
  bool latency_over_budget = false;
  bool memory_over_budget = false;
  bool backlog_over_budget = false;
  bool quota_pressure_high = false;
  bool errors_observed = false;
};

struct AdaptiveTuningControllerResult {
  bool ok = false;
  bool fail_closed = true;
  bool benchmark_clean_evidence = false;
  bool semantics_neutral = false;
  bool advisory_only = true;
  u64 selected_value = 0;
  AdaptiveTuningActionClass action = AdaptiveTuningActionClass::kRefuse;
  std::string diagnostic_code;
  std::vector<std::string> diagnostics;
  std::vector<std::string> evidence;
  std::vector<std::string> metrics_evidence;
  AdaptiveTuningBudgetEvidence budget;
};

const char* AdaptiveTuningKnobName(AdaptiveTuningKnob knob);
const char* AdaptiveTuningActionClassName(AdaptiveTuningActionClass action);

AdaptiveTuningControllerResult EvaluateAdaptiveTuningController(
    const AdaptiveTuningControllerRequest& request);

AdaptiveTuningControllerRequest BuildAdaptiveTuningAgentRequest(
    AdaptiveTuningKnob knob,
    u64 min_value,
    u64 max_value,
    u64 default_value,
    u64 current_value,
    AdaptiveTuningSafetyPolicy safety,
    scratchbird::core::metrics::AdaptiveTuningMetricEvidence metrics);

std::string SerializeAdaptiveTuningControllerEvidence(
    const AdaptiveTuningControllerResult& result);

}  // namespace scratchbird::core::agents
