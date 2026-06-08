// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <string>
#include <vector>

namespace scratchbird::engine::optimizer {

// SEARCH_KEY: SB_OPTIMIZER_SECURITY_BARRIER_MODEL
struct OptimizerBarrierInput {
  bool security_context_present = false;
  bool grants_proven = false;
  bool rls_policy_present = false;
  bool masking_policy_present = false;
  bool metadata_redaction_required = false;
  bool function_volatile = false;
  bool function_side_effecting = false;
  bool function_dangerous = false;
  bool domain_policy_sensitive = false;
  bool collation_sensitive = false;
  bool outer_join_sensitive = false;
  bool parser_hint_claim = false;
  bool engine_policy_accepts_hint = false;
};

struct OptimizerBarrierDecision {
  bool may_fold = false;
  bool may_pushdown = false;
  bool may_reorder = false;
  bool may_expose_metadata = false;
  bool may_apply_hint = false;
  std::vector<std::string> diagnostics;
};

OptimizerBarrierDecision EvaluateOptimizerBarriers(const OptimizerBarrierInput& input);
bool OptimizerBarrierAllowsRewrite(const OptimizerBarrierDecision& decision);

}  // namespace scratchbird::engine::optimizer
