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

// SEARCH_KEY: SB_OPTIMIZER_REWRITE_FRAMEWORK
struct OptimizerExpressionTerm {
  std::string term_id;
  std::string operator_id;
  std::string descriptor_digest;
  std::vector<std::string> input_term_ids;
  bool literal = false;
  bool deterministic = true;
  bool volatile_function = false;
  bool side_effecting = false;
  bool collation_sensitive = false;
  bool domain_policy_sensitive = false;
};

struct PredicateNormalizationInput {
  std::string predicate_kind;
  std::string left_descriptor_digest;
  std::string right_descriptor_digest;
  bool negated = false;
  bool nullable = false;
};

struct ProjectionPruneInput {
  std::vector<std::string> produced_column_uuids;
  std::vector<std::string> required_column_uuids;
  std::vector<std::string> masked_column_uuids;
};

struct MaterializedSummaryRewriteInput {
  std::string query_descriptor_digest;
  std::string summary_descriptor_digest;
  std::string equivalence_proof_digest;
  bool equivalence_proven = false;
  bool summary_present = true;
  bool summary_fresh = true;
  bool summary_format_compatible = true;
  bool summary_descriptor_compatible = true;
  bool base_row_mga_recheck_planned = true;
  bool base_row_security_recheck_planned = true;
  bool parser_or_donor_authority = false;
  bool mga_compatible = true;
  bool unsafe_state = false;
  std::uint64_t estimated_base_cost = 0;
  std::uint64_t estimated_rewrite_cost = 0;
  std::vector<OptimizerExpressionTerm> terms;
};

struct CommonSubexpressionReuseInput {
  std::string equivalence_proof_digest;
  bool equivalence_proven = false;
  bool parser_or_donor_authority = false;
  bool mga_compatible = true;
  bool unsafe_state = false;
  bool base_row_mga_recheck_planned = true;
  bool base_row_security_recheck_planned = true;
  std::uint64_t estimated_recompute_cost = 0;
  std::uint64_t estimated_reuse_cost = 0;
  std::vector<OptimizerExpressionTerm> terms;
};

struct RewriteDecision {
  bool applied = false;
  std::string rewrite_kind;
  std::string canonical_form;
  std::vector<std::string> preserved_column_uuids;
  std::vector<std::string> diagnostics;
};

RewriteDecision NormalizeExpression(const OptimizerExpressionTerm& expression);
RewriteDecision SafeConstantFold(const OptimizerExpressionTerm& expression, const OptimizerBarrierInput& barriers);
RewriteDecision NormalizePredicate(const PredicateNormalizationInput& input);
RewriteDecision PruneProjection(const ProjectionPruneInput& input);
RewriteDecision DecidePredicatePushdown(const PredicateNormalizationInput& predicate, const OptimizerBarrierInput& barriers);
RewriteDecision DecideCteMaterialization(bool reused, bool volatile_or_side_effecting, std::uint64_t estimated_rows);
RewriteDecision SimplifySortLimit(bool input_already_ordered, bool limit_present, bool offset_present);
RewriteDecision SelectMaterializedSummaryRewrite(const MaterializedSummaryRewriteInput& input,
                                                 const OptimizerBarrierInput& barriers);
RewriteDecision SelectCommonSubexpressionReuse(const CommonSubexpressionReuseInput& input,
                                               const OptimizerBarrierInput& barriers);

}  // namespace scratchbird::engine::optimizer
