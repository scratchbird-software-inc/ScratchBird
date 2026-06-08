// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "optimizer_barrier.hpp"

namespace scratchbird::engine::optimizer {

OptimizerBarrierDecision EvaluateOptimizerBarriers(const OptimizerBarrierInput& input) {
  OptimizerBarrierDecision decision;

  if (!input.security_context_present) decision.diagnostics.push_back("SB_OPT_BARRIER_SECURITY_CONTEXT_REQUIRED");
  if (!input.grants_proven) decision.diagnostics.push_back("SB_OPT_BARRIER_GRANTS_NOT_PROVEN");
  if (input.rls_policy_present) decision.diagnostics.push_back("SB_OPT_BARRIER_RLS_PRESENT");
  if (input.masking_policy_present) decision.diagnostics.push_back("SB_OPT_BARRIER_MASKING_PRESENT");
  if (input.metadata_redaction_required) decision.diagnostics.push_back("SB_OPT_BARRIER_METADATA_REDACTION_REQUIRED");
  if (input.function_volatile) decision.diagnostics.push_back("SB_OPT_BARRIER_VOLATILE_FUNCTION");
  if (input.function_side_effecting) decision.diagnostics.push_back("SB_OPT_BARRIER_SIDE_EFFECTING_FUNCTION");
  if (input.function_dangerous) decision.diagnostics.push_back("SB_OPT_BARRIER_DANGEROUS_FUNCTION");
  if (input.domain_policy_sensitive) decision.diagnostics.push_back("SB_OPT_BARRIER_DOMAIN_POLICY_SENSITIVE");
  if (input.collation_sensitive) decision.diagnostics.push_back("SB_OPT_BARRIER_COLLATION_SENSITIVE");
  if (input.outer_join_sensitive) decision.diagnostics.push_back("SB_OPT_BARRIER_OUTER_JOIN_SENSITIVE");

  const bool base_security_ok = input.security_context_present && input.grants_proven;
  const bool function_safe = !input.function_volatile && !input.function_side_effecting && !input.function_dangerous;
  decision.may_fold = base_security_ok && function_safe && !input.domain_policy_sensitive && !input.collation_sensitive;
  decision.may_pushdown = base_security_ok && function_safe && !input.rls_policy_present && !input.masking_policy_present && !input.outer_join_sensitive;
  decision.may_reorder = base_security_ok && function_safe && !input.outer_join_sensitive;
  decision.may_expose_metadata = base_security_ok && !input.metadata_redaction_required;
  decision.may_apply_hint = input.parser_hint_claim && input.engine_policy_accepts_hint && base_security_ok;
  if (input.parser_hint_claim && !decision.may_apply_hint) decision.diagnostics.push_back("SB_OPT_BARRIER_HINT_NOT_ACCEPTED");
  return decision;
}

bool OptimizerBarrierAllowsRewrite(const OptimizerBarrierDecision& decision) {
  return decision.may_fold && decision.may_pushdown && decision.may_reorder;
}

}  // namespace scratchbird::engine::optimizer
