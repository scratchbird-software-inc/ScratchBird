// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "optimizer_rewrite.hpp"

#include <algorithm>
#include <iostream>
#include <string>
#include <vector>

namespace opt = scratchbird::engine::optimizer;

namespace {

bool Require(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << message << '\n';
    return false;
  }
  return true;
}

bool Has(const std::vector<std::string>& values, const std::string& expected) {
  return std::find(values.begin(), values.end(), expected) != values.end();
}

opt::OptimizerBarrierInput SafeBarrier() {
  opt::OptimizerBarrierInput barrier;
  barrier.security_context_present = true;
  barrier.grants_proven = true;
  return barrier;
}

opt::OptimizerExpressionTerm SafeTerm(const std::string& term_id,
                                      const std::string& descriptor_digest) {
  opt::OptimizerExpressionTerm term;
  term.term_id = term_id;
  term.operator_id = "op.add";
  term.descriptor_digest = descriptor_digest;
  term.input_term_ids = {"col.amount", "literal.tax"};
  return term;
}

opt::MaterializedSummaryRewriteInput BaseSummaryInput() {
  opt::MaterializedSummaryRewriteInput input;
  input.query_descriptor_digest = "query.sales.sum.amount.by_region:v1";
  input.summary_descriptor_digest = "mv.sales.sum.amount.by_region:v1";
  input.equivalence_proof_digest = "proof.sales.sum.amount.by_region:v1";
  input.equivalence_proven = true;
  input.summary_present = true;
  input.summary_fresh = true;
  input.summary_format_compatible = true;
  input.summary_descriptor_compatible = true;
  input.base_row_mga_recheck_planned = true;
  input.base_row_security_recheck_planned = true;
  input.mga_compatible = true;
  input.estimated_base_cost = 1000;
  input.estimated_rewrite_cost = 100;
  input.terms = {SafeTerm("expr.gross", "digest.gross:v1")};
  return input;
}

opt::CommonSubexpressionReuseInput BaseCseInput() {
  opt::CommonSubexpressionReuseInput input;
  input.equivalence_proof_digest = "proof.cse.gross:v1";
  input.equivalence_proven = true;
  input.mga_compatible = true;
  input.estimated_recompute_cost = 200;
  input.estimated_reuse_cost = 20;
  input.terms = {
      SafeTerm("expr.gross.1", "digest.gross:v1"),
      SafeTerm("expr.gross.2", "digest.gross:v1"),
      SafeTerm("expr.net", "digest.net:v1"),
  };
  return input;
}

bool AcceptedSummaryRewriteIsMetadataOnlyAndProofBacked() {
  const auto decision = opt::SelectMaterializedSummaryRewrite(BaseSummaryInput(), SafeBarrier());
  return Require(decision.applied, "valid materialized summary rewrite was refused") &&
         Require(decision.rewrite_kind == "materialized_summary_rewrite",
                 "summary rewrite kind mismatch") &&
         Require(decision.canonical_form.find("proof=proof.sales.sum.amount.by_region:v1") !=
                     std::string::npos,
                 "summary rewrite omitted equivalence proof") &&
         Require(decision.canonical_form.find("base_row_recheck=mga_security") != std::string::npos,
                 "summary rewrite omitted base-row recheck evidence") &&
         Require(Has(decision.diagnostics, "SB_OPT_REWRITE_MATERIALIZED_SUMMARY_SELECTED"),
                 "summary rewrite selected diagnostic missing") &&
         Require(Has(decision.diagnostics, "SB_OPT_REWRITE_METADATA_ONLY_MGA_RECHECK_PRESERVED"),
                 "summary rewrite did not preserve metadata-only MGA evidence");
}

bool AcceptedCseRewriteIsDeterministicAndProofBacked() {
  const auto decision = opt::SelectCommonSubexpressionReuse(BaseCseInput(), SafeBarrier());
  return Require(decision.applied, "valid CSE rewrite was refused") &&
         Require(decision.rewrite_kind == "common_subexpression_reuse",
                 "CSE rewrite kind mismatch") &&
         Require(decision.canonical_form.find("cse_reuse:op.add:digest.gross:v1") !=
                     std::string::npos,
                 "CSE rewrite did not select deterministic equivalent expression") &&
         Require(decision.canonical_form.find("terms=2") != std::string::npos,
                 "CSE rewrite reused wrong expression count") &&
         Require(decision.canonical_form.find("proof=proof.cse.gross:v1") != std::string::npos,
                 "CSE rewrite omitted equivalence proof") &&
         Require(decision.canonical_form.find("base_row_recheck=mga_security") != std::string::npos,
                 "CSE rewrite omitted base-row recheck evidence") &&
         Require(Has(decision.preserved_column_uuids, "expr.gross.1"),
                 "CSE rewrite omitted first reused expression id") &&
         Require(Has(decision.preserved_column_uuids, "expr.gross.2"),
                 "CSE rewrite omitted second reused expression id") &&
         Require(Has(decision.diagnostics, "SB_OPT_REWRITE_METADATA_ONLY_MGA_RECHECK_PRESERVED"),
                 "CSE rewrite did not preserve metadata-only MGA evidence");
}

bool SummaryRefusalReasonsAreExact() {
  auto input = BaseSummaryInput();
  input.equivalence_proven = false;
  input.equivalence_proof_digest.clear();
  input.summary_fresh = false;
  input.summary_format_compatible = false;
  input.base_row_mga_recheck_planned = false;
  input.base_row_security_recheck_planned = false;
  input.parser_or_reference_authority = true;
  input.mga_compatible = false;
  input.summary_present = false;
  input.estimated_rewrite_cost = input.estimated_base_cost;
  auto term = SafeTerm("expr.unsafe", "digest.unsafe:v1");
  term.deterministic = false;
  term.volatile_function = true;
  term.side_effecting = true;
  term.collation_sensitive = true;
  term.domain_policy_sensitive = true;
  input.terms = {term};

  auto barrier = SafeBarrier();
  barrier.rls_policy_present = true;
  barrier.masking_policy_present = true;
  barrier.metadata_redaction_required = true;
  const auto decision = opt::SelectMaterializedSummaryRewrite(input, barrier);

  return Require(!decision.applied, "unsafe summary rewrite was selected") &&
         Require(Has(decision.diagnostics, "SB_OPT_REWRITE_EQUIVALENCE_PROOF_MISSING"),
                 "missing equivalence proof refusal missing") &&
         Require(Has(decision.diagnostics, "SB_OPT_REWRITE_SUMMARY_MISSING"),
                 "missing summary refusal missing") &&
         Require(Has(decision.diagnostics, "SB_OPT_REWRITE_SUMMARY_STALE"),
                 "stale summary refusal missing") &&
         Require(Has(decision.diagnostics, "SB_OPT_REWRITE_SUMMARY_INCOMPATIBLE"),
                 "incompatible summary refusal missing") &&
         Require(Has(decision.diagnostics, "SB_OPT_REWRITE_BASE_ROW_MGA_RECHECK_MISSING"),
                 "missing MGA base-row recheck refusal missing") &&
         Require(Has(decision.diagnostics, "SB_OPT_REWRITE_BASE_ROW_SECURITY_RECHECK_MISSING"),
                 "missing security base-row recheck refusal missing") &&
         Require(Has(decision.diagnostics, "SB_OPT_REWRITE_SECURITY_GRANT_POLICY_BARRIER"),
                 "security/grant/RLS/masking/redaction barrier refusal missing") &&
         Require(Has(decision.diagnostics, "SB_OPT_REWRITE_COLLATION_BARRIER"),
                 "collation barrier refusal missing") &&
         Require(Has(decision.diagnostics, "SB_OPT_REWRITE_DOMAIN_BARRIER"),
                 "domain barrier refusal missing") &&
         Require(Has(decision.diagnostics, "SB_OPT_REWRITE_NONDETERMINISTIC_TERM"),
                 "non-deterministic term refusal missing") &&
         Require(Has(decision.diagnostics, "SB_OPT_REWRITE_VOLATILE_TERM"),
                 "volatile term refusal missing") &&
         Require(Has(decision.diagnostics, "SB_OPT_REWRITE_SIDE_EFFECTING_TERM"),
                 "side-effecting term refusal missing") &&
         Require(Has(decision.diagnostics, "SB_OPT_REWRITE_UNSAFE_PARSER_REFERENCE_AUTHORITY"),
                 "parser/reference authority refusal missing") &&
         Require(Has(decision.diagnostics, "SB_OPT_REWRITE_NO_BENEFIT"),
                 "no-benefit refusal missing") &&
         Require(Has(decision.diagnostics, "SB_OPT_REWRITE_MGA_INCOMPATIBLE_STATE"),
                 "MGA-incompatible state refusal missing");
}

bool CseRefusalReasonsAreExact() {
  auto input = BaseCseInput();
  input.equivalence_proven = false;
  input.parser_or_reference_authority = true;
  input.unsafe_state = true;
  input.base_row_mga_recheck_planned = false;
  input.base_row_security_recheck_planned = false;
  input.estimated_reuse_cost = input.estimated_recompute_cost;
  auto term = SafeTerm("expr.single", "digest.single:v1");
  term.deterministic = false;
  term.volatile_function = true;
  term.side_effecting = true;
  term.collation_sensitive = true;
  term.domain_policy_sensitive = true;
  input.terms = {term};

  auto barrier = SafeBarrier();
  barrier.grants_proven = false;
  const auto decision = opt::SelectCommonSubexpressionReuse(input, barrier);

  return Require(!decision.applied, "unsafe CSE rewrite was selected") &&
         Require(Has(decision.diagnostics, "SB_OPT_REWRITE_EQUIVALENCE_PROOF_MISSING"),
                 "CSE missing equivalence proof refusal missing") &&
         Require(Has(decision.diagnostics, "SB_OPT_REWRITE_UNSAFE_PARSER_REFERENCE_AUTHORITY"),
                 "CSE parser/reference authority refusal missing") &&
         Require(Has(decision.diagnostics, "SB_OPT_REWRITE_MGA_INCOMPATIBLE_STATE"),
                 "CSE MGA-incompatible state refusal missing") &&
         Require(Has(decision.diagnostics, "SB_OPT_REWRITE_BASE_ROW_MGA_RECHECK_MISSING"),
                 "CSE missing MGA base-row recheck refusal missing") &&
         Require(Has(decision.diagnostics, "SB_OPT_REWRITE_BASE_ROW_SECURITY_RECHECK_MISSING"),
                 "CSE missing security base-row recheck refusal missing") &&
         Require(Has(decision.diagnostics, "SB_OPT_REWRITE_NO_COMMON_EXPRESSION"),
                 "CSE no-common-expression refusal missing") &&
         Require(Has(decision.diagnostics, "SB_OPT_REWRITE_NO_BENEFIT"),
                 "CSE no-benefit refusal missing") &&
         Require(Has(decision.diagnostics, "SB_OPT_REWRITE_SECURITY_GRANT_POLICY_BARRIER"),
                 "CSE grant/security barrier refusal missing") &&
         Require(Has(decision.diagnostics, "SB_OPT_REWRITE_NONDETERMINISTIC_TERM"),
                 "CSE non-deterministic term refusal missing") &&
         Require(Has(decision.diagnostics, "SB_OPT_REWRITE_VOLATILE_TERM"),
                 "CSE volatile term refusal missing") &&
         Require(Has(decision.diagnostics, "SB_OPT_REWRITE_SIDE_EFFECTING_TERM"),
                 "CSE side-effecting term refusal missing") &&
         Require(Has(decision.diagnostics, "SB_OPT_REWRITE_COLLATION_BARRIER"),
                 "CSE collation barrier refusal missing") &&
         Require(Has(decision.diagnostics, "SB_OPT_REWRITE_DOMAIN_BARRIER"),
                 "CSE domain barrier refusal missing");
}

}  // namespace

int main() {
  if (!AcceptedSummaryRewriteIsMetadataOnlyAndProofBacked()) return 1;
  if (!AcceptedCseRewriteIsDeterministicAndProofBacked()) return 1;
  if (!SummaryRefusalReasonsAreExact()) return 1;
  if (!CseRefusalReasonsAreExact()) return 1;
  return 0;
}
