// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "optimizer_rewrite.hpp"

#include <algorithm>
#include <map>
#include <set>

namespace scratchbird::engine::optimizer {
namespace {

bool IsCommutativeOperator(const std::string& op) {
  return op == "op.eq" || op == "op.add" || op == "op.mul" || op == "op.and" || op == "op.or";
}

bool Contains(const std::vector<std::string>& values, const std::string& value) {
  return std::find(values.begin(), values.end(), value) != values.end();
}

void AddUniqueDiagnostic(std::vector<std::string>* diagnostics, const std::string& diagnostic) {
  if (!Contains(*diagnostics, diagnostic)) diagnostics->push_back(diagnostic);
}

std::string ExpressionReuseKey(const OptimizerExpressionTerm& term) {
  std::vector<std::string> inputs = term.input_term_ids;
  if (IsCommutativeOperator(term.operator_id)) std::sort(inputs.begin(), inputs.end());

  std::string key = term.operator_id + ":" + term.descriptor_digest;
  for (const auto& input : inputs) key += ":input=" + input;
  return key;
}

bool AddTermSafetyDiagnostics(const std::vector<OptimizerExpressionTerm>& terms,
                              std::vector<std::string>* diagnostics) {
  bool safe = true;
  for (const auto& term : terms) {
    if (!term.deterministic) {
      AddUniqueDiagnostic(diagnostics, "SB_OPT_REWRITE_NONDETERMINISTIC_TERM");
      safe = false;
    }
    if (term.volatile_function) {
      AddUniqueDiagnostic(diagnostics, "SB_OPT_REWRITE_VOLATILE_TERM");
      safe = false;
    }
    if (term.side_effecting) {
      AddUniqueDiagnostic(diagnostics, "SB_OPT_REWRITE_SIDE_EFFECTING_TERM");
      safe = false;
    }
    if (term.collation_sensitive) {
      AddUniqueDiagnostic(diagnostics, "SB_OPT_REWRITE_COLLATION_BARRIER");
      safe = false;
    }
    if (term.domain_policy_sensitive) {
      AddUniqueDiagnostic(diagnostics, "SB_OPT_REWRITE_DOMAIN_BARRIER");
      safe = false;
    }
    if (term.operator_id.empty() || term.descriptor_digest.empty()) {
      AddUniqueDiagnostic(diagnostics, "SB_OPT_REWRITE_EXPRESSION_AUTHORITY_MISSING");
      safe = false;
    }
  }
  return safe;
}

bool AddBarrierDiagnostics(const OptimizerBarrierInput& barriers,
                           const OptimizerBarrierDecision& barrier,
                           std::vector<std::string>* diagnostics) {
  for (const auto& diagnostic : barrier.diagnostics) AddUniqueDiagnostic(diagnostics, diagnostic);
  if (!barriers.security_context_present || !barriers.grants_proven || barriers.rls_policy_present ||
      barriers.masking_policy_present || barriers.metadata_redaction_required) {
    AddUniqueDiagnostic(diagnostics, "SB_OPT_REWRITE_SECURITY_GRANT_POLICY_BARRIER");
  }
  if (barriers.collation_sensitive) AddUniqueDiagnostic(diagnostics, "SB_OPT_REWRITE_COLLATION_BARRIER");
  if (barriers.domain_policy_sensitive) AddUniqueDiagnostic(diagnostics, "SB_OPT_REWRITE_DOMAIN_BARRIER");
  if (barriers.function_volatile) AddUniqueDiagnostic(diagnostics, "SB_OPT_REWRITE_VOLATILE_TERM");
  if (barriers.function_side_effecting || barriers.function_dangerous) {
    AddUniqueDiagnostic(diagnostics, "SB_OPT_REWRITE_SIDE_EFFECTING_TERM");
  }
  return OptimizerBarrierAllowsRewrite(barrier) && barrier.may_expose_metadata;
}

}  // namespace

RewriteDecision NormalizeExpression(const OptimizerExpressionTerm& expression) {
  RewriteDecision decision;
  decision.rewrite_kind = "expression_normalization";
  if (expression.operator_id.empty() || expression.descriptor_digest.empty()) {
    decision.diagnostics.push_back("SB_OPT_REWRITE_EXPRESSION_AUTHORITY_MISSING");
    return decision;
  }
  decision.applied = true;
  decision.canonical_form = expression.operator_id + ":" + expression.descriptor_digest;
  if (IsCommutativeOperator(expression.operator_id)) decision.canonical_form += ":commutative_sorted_inputs";
  if (!expression.deterministic) decision.diagnostics.push_back("SB_OPT_REWRITE_NONDETERMINISTIC_EXPRESSION_PRESERVED");
  return decision;
}

RewriteDecision SafeConstantFold(const OptimizerExpressionTerm& expression, const OptimizerBarrierInput& barriers) {
  RewriteDecision decision;
  decision.rewrite_kind = "constant_folding";
  const auto barrier = EvaluateOptimizerBarriers(barriers);
  decision.diagnostics = barrier.diagnostics;
  if (!expression.literal) {
    decision.diagnostics.push_back("SB_OPT_REWRITE_NOT_LITERAL");
    return decision;
  }
  if (expression.volatile_function || expression.side_effecting || expression.collation_sensitive || expression.domain_policy_sensitive || !barrier.may_fold) {
    decision.diagnostics.push_back("SB_OPT_REWRITE_FOLD_REFUSED_BY_BARRIER");
    return decision;
  }
  decision.applied = true;
  decision.canonical_form = "constant_folded:" + expression.descriptor_digest;
  return decision;
}

RewriteDecision NormalizePredicate(const PredicateNormalizationInput& input) {
  RewriteDecision decision;
  decision.rewrite_kind = "predicate_normalization";
  if (input.predicate_kind.empty() || input.left_descriptor_digest.empty()) {
    decision.diagnostics.push_back("SB_OPT_REWRITE_PREDICATE_AUTHORITY_MISSING");
    return decision;
  }
  decision.applied = true;
  if (input.predicate_kind == "scalar_eq" || input.predicate_kind == "unique_eq" || input.predicate_kind == "row_uuid_eq") {
    decision.canonical_form = input.negated ? "not_eq" : "eq";
  } else if (input.predicate_kind == "scalar_range") {
    decision.canonical_form = input.negated ? "not_range" : "range";
  } else if (input.predicate_kind == "full_text") {
    decision.canonical_form = "search_match";
  } else if (input.predicate_kind == "document_path") {
    decision.canonical_form = "document_path";
  } else if (input.predicate_kind == "vector_exact" || input.predicate_kind == "vector_approx") {
    decision.canonical_form = "vector_distance";
  } else {
    decision.canonical_form = "opaque_predicate";
    decision.diagnostics.push_back("SB_OPT_REWRITE_OPAQUE_PREDICATE");
  }
  if (input.nullable) decision.diagnostics.push_back("SB_OPT_REWRITE_THREE_VALUED_LOGIC_PRESERVED");
  return decision;
}

RewriteDecision PruneProjection(const ProjectionPruneInput& input) {
  RewriteDecision decision;
  decision.rewrite_kind = "projection_pruning";
  std::set<std::string> emitted;
  for (const auto& column : input.produced_column_uuids) {
    if (Contains(input.required_column_uuids, column) || Contains(input.masked_column_uuids, column)) {
      if (emitted.insert(column).second) decision.preserved_column_uuids.push_back(column);
    }
  }
  decision.applied = decision.preserved_column_uuids.size() != input.produced_column_uuids.size();
  if (!input.masked_column_uuids.empty()) decision.diagnostics.push_back("SB_OPT_REWRITE_MASKED_COLUMNS_PRESERVED");
  return decision;
}

RewriteDecision DecidePredicatePushdown(const PredicateNormalizationInput& predicate, const OptimizerBarrierInput& barriers) {
  RewriteDecision decision = NormalizePredicate(predicate);
  decision.rewrite_kind = "predicate_pushdown";
  const auto barrier = EvaluateOptimizerBarriers(barriers);
  decision.diagnostics.insert(decision.diagnostics.end(), barrier.diagnostics.begin(), barrier.diagnostics.end());
  if (!barrier.may_pushdown) {
    decision.applied = false;
    decision.diagnostics.push_back("SB_OPT_REWRITE_PUSHDOWN_REFUSED_BY_BARRIER");
  }
  return decision;
}

RewriteDecision DecideCteMaterialization(bool reused, bool volatile_or_side_effecting, std::uint64_t estimated_rows) {
  RewriteDecision decision;
  decision.rewrite_kind = "cte_materialization";
  if (volatile_or_side_effecting) {
    decision.applied = true;
    decision.canonical_form = "materialize";
    decision.diagnostics.push_back("SB_OPT_REWRITE_CTE_MATERIALIZED_FOR_SIDE_EFFECT_BOUNDARY");
    return decision;
  }
  if (reused || estimated_rows > 1024) {
    decision.applied = true;
    decision.canonical_form = "materialize";
  } else {
    decision.applied = true;
    decision.canonical_form = "inline";
  }
  return decision;
}

RewriteDecision SimplifySortLimit(bool input_already_ordered, bool limit_present, bool offset_present) {
  RewriteDecision decision;
  decision.rewrite_kind = "sort_limit_simplification";
  if (input_already_ordered && !offset_present) {
    decision.applied = true;
    decision.canonical_form = limit_present ? "ordered_top_n" : "sort_elided";
  } else if (limit_present) {
    decision.applied = true;
    decision.canonical_form = "top_n";
  } else {
    decision.applied = false;
    decision.canonical_form = "full_sort";
  }
  return decision;
}

RewriteDecision SelectMaterializedSummaryRewrite(const MaterializedSummaryRewriteInput& input,
                                                 const OptimizerBarrierInput& barriers) {
  RewriteDecision decision;
  decision.rewrite_kind = "materialized_summary_rewrite";

  const auto barrier = EvaluateOptimizerBarriers(barriers);
  bool selectable = AddBarrierDiagnostics(barriers, barrier, &decision.diagnostics);
  selectable = AddTermSafetyDiagnostics(input.terms, &decision.diagnostics) && selectable;

  if (input.query_descriptor_digest.empty() || input.summary_descriptor_digest.empty() ||
      input.equivalence_proof_digest.empty() || !input.equivalence_proven) {
    AddUniqueDiagnostic(&decision.diagnostics, "SB_OPT_REWRITE_EQUIVALENCE_PROOF_MISSING");
    selectable = false;
  }
  if (!input.summary_present) {
    AddUniqueDiagnostic(&decision.diagnostics, "SB_OPT_REWRITE_SUMMARY_MISSING");
    selectable = false;
  }
  if (!input.summary_fresh) {
    AddUniqueDiagnostic(&decision.diagnostics, "SB_OPT_REWRITE_SUMMARY_STALE");
    selectable = false;
  }
  if (!input.summary_format_compatible || !input.summary_descriptor_compatible) {
    AddUniqueDiagnostic(&decision.diagnostics, "SB_OPT_REWRITE_SUMMARY_INCOMPATIBLE");
    selectable = false;
  }
  if (!input.base_row_mga_recheck_planned) {
    AddUniqueDiagnostic(&decision.diagnostics, "SB_OPT_REWRITE_BASE_ROW_MGA_RECHECK_MISSING");
    selectable = false;
  }
  if (!input.base_row_security_recheck_planned) {
    AddUniqueDiagnostic(&decision.diagnostics, "SB_OPT_REWRITE_BASE_ROW_SECURITY_RECHECK_MISSING");
    selectable = false;
  }
  if (input.parser_or_reference_authority) {
    AddUniqueDiagnostic(&decision.diagnostics, "SB_OPT_REWRITE_UNSAFE_PARSER_REFERENCE_AUTHORITY");
    selectable = false;
  }
  if (!input.mga_compatible || input.unsafe_state) {
    AddUniqueDiagnostic(&decision.diagnostics, "SB_OPT_REWRITE_MGA_INCOMPATIBLE_STATE");
    selectable = false;
  }
  if (input.estimated_base_cost == 0 || input.estimated_rewrite_cost >= input.estimated_base_cost) {
    AddUniqueDiagnostic(&decision.diagnostics, "SB_OPT_REWRITE_NO_BENEFIT");
    selectable = false;
  }

  if (!selectable) return decision;

  decision.applied = true;
  decision.canonical_form = "materialized_summary:" + input.summary_descriptor_digest +
                            ":proof=" + input.equivalence_proof_digest +
                            ":base_row_recheck=mga_security";
  decision.diagnostics.push_back("SB_OPT_REWRITE_MATERIALIZED_SUMMARY_SELECTED");
  decision.diagnostics.push_back("SB_OPT_REWRITE_METADATA_ONLY_MGA_RECHECK_PRESERVED");
  return decision;
}

RewriteDecision SelectCommonSubexpressionReuse(const CommonSubexpressionReuseInput& input,
                                               const OptimizerBarrierInput& barriers) {
  RewriteDecision decision;
  decision.rewrite_kind = "common_subexpression_reuse";

  const auto barrier = EvaluateOptimizerBarriers(barriers);
  bool selectable = AddBarrierDiagnostics(barriers, barrier, &decision.diagnostics);
  selectable = AddTermSafetyDiagnostics(input.terms, &decision.diagnostics) && selectable;

  if (input.equivalence_proof_digest.empty() || !input.equivalence_proven) {
    AddUniqueDiagnostic(&decision.diagnostics, "SB_OPT_REWRITE_EQUIVALENCE_PROOF_MISSING");
    selectable = false;
  }
  if (input.parser_or_reference_authority) {
    AddUniqueDiagnostic(&decision.diagnostics, "SB_OPT_REWRITE_UNSAFE_PARSER_REFERENCE_AUTHORITY");
    selectable = false;
  }
  if (!input.mga_compatible || input.unsafe_state) {
    AddUniqueDiagnostic(&decision.diagnostics, "SB_OPT_REWRITE_MGA_INCOMPATIBLE_STATE");
    selectable = false;
  }
  if (!input.base_row_mga_recheck_planned) {
    AddUniqueDiagnostic(&decision.diagnostics, "SB_OPT_REWRITE_BASE_ROW_MGA_RECHECK_MISSING");
    selectable = false;
  }
  if (!input.base_row_security_recheck_planned) {
    AddUniqueDiagnostic(&decision.diagnostics, "SB_OPT_REWRITE_BASE_ROW_SECURITY_RECHECK_MISSING");
    selectable = false;
  }

  std::map<std::string, std::vector<std::string>> equivalent_terms;
  for (const auto& term : input.terms) equivalent_terms[ExpressionReuseKey(term)].push_back(term.term_id);

  std::string selected_key;
  std::vector<std::string> selected_terms;
  for (const auto& [key, term_ids] : equivalent_terms) {
    if (term_ids.size() > selected_terms.size()) {
      selected_key = key;
      selected_terms = term_ids;
    }
  }
  if (selected_terms.size() < 2) {
    AddUniqueDiagnostic(&decision.diagnostics, "SB_OPT_REWRITE_NO_COMMON_EXPRESSION");
    selectable = false;
  }
  if (input.estimated_recompute_cost == 0 || input.estimated_reuse_cost >= input.estimated_recompute_cost) {
    AddUniqueDiagnostic(&decision.diagnostics, "SB_OPT_REWRITE_NO_BENEFIT");
    selectable = false;
  }

  if (!selectable) return decision;

  decision.applied = true;
  decision.canonical_form = "cse_reuse:" + selected_key + ":terms=" + std::to_string(selected_terms.size()) +
                            ":proof=" + input.equivalence_proof_digest +
                            ":base_row_recheck=mga_security";
  decision.preserved_column_uuids = selected_terms;
  decision.diagnostics.push_back("SB_OPT_REWRITE_CSE_SELECTED");
  decision.diagnostics.push_back("SB_OPT_REWRITE_METADATA_ONLY_MGA_RECHECK_PRESERVED");
  return decision;
}

}  // namespace scratchbird::engine::optimizer
