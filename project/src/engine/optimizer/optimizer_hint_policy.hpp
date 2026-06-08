// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "optimizer_barrier.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace scratchbird::engine::optimizer {

// SEARCH_KEY: SB_OPTIMIZER_HINT_POLICY_ADMISSION
// Hint policy reaches the optimizer only as normalized engine/SBLR policy
// tokens. It is advisory optimizer guidance and never SQL text, parser
// execution, donor behavior, name authority, benchmark evidence, transaction
// finality, visibility, security, or recovery authority.
struct OptimizerHintPolicyRequest {
  std::string policy_uuid;
  std::string request_uuid;
  std::string operation_id;
  std::string sblr_digest;
  std::string descriptor_set_digest;
  std::string route_capability_digest;
  std::string security_policy_digest;
  std::string redaction_policy_digest;
  std::string memory_policy_digest;
  std::string cost_profile_id;
  std::string policy_source_kind = "sblr_api";
  std::vector<std::string> normalized_hint_tokens;

  std::uint64_t policy_epoch = 0;
  std::uint64_t catalog_epoch = 0;
  std::uint64_t stats_epoch = 0;
  std::uint64_t security_epoch = 0;
  std::uint64_t redaction_epoch = 0;
  std::uint64_t name_resolution_epoch = 0;
  std::uint64_t resource_epoch = 0;
  std::uint64_t memory_policy_epoch = 0;
  std::uint64_t memory_feedback_generation = 0;
  std::uint64_t route_epoch = 0;

  bool engine_policy_enabled = true;
  bool advisory_only = true;
  bool security_context_present = false;
  bool transaction_context_present = false;
  bool grants_proven = false;
  bool mga_visibility_recheck_required = false;
  bool exact_recheck_required = false;
  bool security_recheck_required = false;
  bool redaction_policy_bound = false;
  bool catalog_descriptor_bound = false;
  bool raw_sql_text_present = false;
  bool parser_execution_authority_claimed = false;
  bool parser_session_directive_unbound = false;
  bool donor_or_legacy_authority_claimed = false;
  bool name_authority_claimed = false;
  bool metric_or_benchmark_authority_claimed = false;
  OptimizerBarrierInput barriers;
};

struct OptimizerHintPolicyAdmission {
  bool accepted = false;
  bool applied = false;
  std::string diagnostic_code;
  std::string policy_digest;
  OptimizerBarrierDecision barrier_decision;
  std::vector<std::string> evidence;
};

OptimizerHintPolicyAdmission EvaluateOptimizerHintPolicyAdmission(
    const OptimizerHintPolicyRequest& request);

}  // namespace scratchbird::engine::optimizer
