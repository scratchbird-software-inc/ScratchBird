// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "access_path_full.hpp"
#include "predicate_normalization.hpp"

#include <algorithm>
#include <iostream>
#include <string>
#include <vector>

namespace opt = scratchbird::engine::optimizer;
namespace plan = scratchbird::engine::planner;

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

const opt::PlanCandidate* FindCandidate(const std::vector<opt::PlanCandidate>& candidates,
                                        const std::string& id) {
  const auto found = std::find_if(candidates.begin(), candidates.end(), [&](const opt::PlanCandidate& candidate) {
    return candidate.candidate_id == id;
  });
  return found == candidates.end() ? nullptr : &*found;
}

opt::OptimizerStatsIdentity FreshIdentity(const std::string& object_uuid,
                                          const std::string& statistic_uuid) {
  opt::OptimizerStatsIdentity identity;
  identity.object_uuid = object_uuid;
  identity.statistic_uuid = statistic_uuid;
  identity.stats_epoch = 22;
  identity.catalog_epoch = 22;
  identity.transaction_visibility_epoch = 22;
  identity.freshness = opt::OptimizerStatsFreshnessState::kFresh;
  identity.source = opt::StatisticSource::kCatalogExact;
  identity.confidence = opt::CostConfidence::kHigh;
  return identity;
}

opt::TableCardinalityStats TableStats(const std::string& relation_uuid) {
  opt::TableCardinalityStats stats;
  stats.identity = FreshIdentity(relation_uuid, relation_uuid + ":table");
  stats.row_count = 64000;
  stats.visible_row_count = 60000;
  stats.page_count = 1500;
  stats.average_row_bytes = 96;
  return stats;
}

opt::IndexStats BaseIndex(const std::string& relation_uuid,
                          const std::string& index_uuid) {
  opt::IndexStats stats;
  stats.identity = FreshIdentity(index_uuid, index_uuid + ":index");
  stats.index_uuid = index_uuid;
  stats.relation_uuid = relation_uuid;
  stats.index_family = "btree";
  stats.descriptor_digest = "desc:customer:v1";
  stats.collation_identity = "unicode.casefold.det";
  stats.key_column_uuids = {"expr.customer_name"};
  stats.covered_column_uuids = {"expr.customer_name", "col.active"};
  stats.unique = false;
  stats.covering = true;
  stats.height = 3;
  stats.leaf_pages = 80;
  stats.distinct_keys = 40000;
  stats.clustering_factor = 0.80;
  stats.fragmentation_ratio = 0.01;
  stats.visibility_coverage = 1.0;
  stats.predicate_coverage = 1.0;
  return stats;
}

opt::AccessPathPlanningRequest BaseRequest(const std::string& relation_uuid) {
  opt::AccessPathPlanningRequest request;
  request.relation_uuid = relation_uuid;
  request.predicate_kind = "scalar_eq";
  request.predicate_text = "lower(customer_name) = 'ada' and active";
  request.descriptor_digest = "desc:customer:v1";
  request.collation_identity = "unicode.casefold.det";
  request.projected_column_uuids = {"expr.customer_name"};
  request.visibility_proven = true;
  request.grants_proven = true;
  request.base_row_mga_recheck_planned = true;
  request.base_row_security_recheck_planned = true;
  request.index_visibility_native = true;
  request.table_stats = TableStats(relation_uuid);
  return request;
}

bool CanonicalFormsAreStableAndBooleanEquivalent() {
  const auto left = opt::CanonicalizePredicateText("42 = lower(customer_name) and active = true");
  const auto right = opt::CanonicalizePredicateText("active and lower(customer_name) = 42");
  const auto flattened = opt::CanonicalizePredicateText("(active and tier = 'gold') and lower(customer_name) = 42");
  const auto double_not = opt::CanonicalizePredicateText("not not active");
  const auto false_bool = opt::CanonicalizePredicateText("not active");
  const auto or_left = opt::CanonicalizePredicateText("(tier = 'gold' or tier = 'platinum') or active");
  const auto or_right = opt::CanonicalizePredicateText("active or tier = 'platinum' or tier = 'gold'");
  const auto expr_a = opt::CanonicalizeExpressionText("LOWER( customer_name )");
  const auto expr_b = opt::CanonicalizeExpressionText("lower(customer_name)");

  return Require(left.ok && right.ok && flattened.ok && double_not.ok && false_bool.ok &&
                     or_left.ok && or_right.ok && expr_a.ok && expr_b.ok,
                 "canonical parser rejected supported predicate forms") &&
         Require(left.canonical_text == right.canonical_text,
                 "commuted comparison or boolean predicate did not canonicalize equivalently") &&
         Require(Has(flattened.conjuncts, "bool(col(active),true)"),
                 "AND flattening did not expose boolean conjunct") &&
         Require(double_not.canonical_text == "bool(col(active),true)",
                 "safe double-NOT simplification did not normalize to boolean true") &&
         Require(false_bool.canonical_text == "bool(col(active),false)",
                 "safe NOT simplification did not normalize boolean false") &&
         Require(or_left.canonical_text == or_right.canonical_text,
                 "OR flattening/sorting was not deterministic") &&
         Require(expr_a.digest == expr_b.digest,
                 "expression digest still depended on raw SQL text") &&
         Require(opt::DeterministicPredicateDescriptorText(left).find(left.digest) != std::string::npos,
                 "deterministic descriptor text omitted predicate digest");
}

bool ExpressionPartialAndAccessCandidateMatch() {
  const std::string relation_uuid = "rel.odf022.expression";
  const auto lower_name = opt::CanonicalizeExpressionText("lower(customer_name)");
  auto index = BaseIndex(relation_uuid, "idx.odf022.functional.lower_name.active");
  index.expression_index = true;
  index.key_expression_digests = {lower_name.digest};
  index.partial = true;
  index.partial_predicate_text = "active = true";

  auto request = BaseRequest(relation_uuid);
  request.candidate_indexes = {index};
  const auto match = opt::MatchPredicateToIndex({request.predicate_text,
                                                request.descriptor_digest,
                                                request.collation_identity,
                                                true,
                                                true,
                                                index});
  if (!Require(match.matches, "functional partial expression index did not match canonical predicate") ||
      !Require(Has(match.acceptance_reasons, "functional_index_expression_digest_match"),
               "functional expression digest acceptance reason missing") ||
      !Require(Has(match.acceptance_reasons, "partial_predicate_proven"),
               "partial predicate proof acceptance reason missing") ||
      !Require(Has(match.acceptance_reasons, "boolean_equivalent_match"),
               "boolean equivalent acceptance reason missing") ||
      !Require(Has(match.acceptance_reasons, "metadata_match_only_mga_visibility_recheck_required"),
               "MGA recheck metadata acceptance reason missing") ||
      !Require(Has(match.acceptance_reasons, "metadata_finality_not_cached"),
               "metadata-only match cached finality")) {
    return false;
  }

  const auto candidates = opt::GenerateFullAccessPathCandidates(request);
  const auto* candidate = FindCandidate(candidates, "CAND-OPT-INDEX:idx.odf022.functional.lower_name.active");
  return Require(candidate != nullptr, "canonical expression index candidate missing") &&
         Require(candidate->cost.selectable, "canonical expression index candidate was refused") &&
         Require(candidate->access_kind == plan::PhysicalAccessKind::kScalarBtreeLookup,
                 "canonical expression index candidate used wrong access kind") &&
         Require(Has(candidate->acceptance_reasons, "functional_index_expression_digest_match"),
                 "access candidate omitted functional digest match reason") &&
         Require(Has(candidate->acceptance_reasons, "partial_predicate_proven"),
                 "access candidate omitted partial proof reason") &&
         Require(Has(candidate->acceptance_reasons, "metadata_match_only_security_recheck_required"),
                 "access candidate omitted security recheck requirement");
}

bool GeneratedComputedAndPartialRefusalsAreExact() {
  const std::string relation_uuid = "rel.odf022.generated";
  const auto lower_name = opt::CanonicalizeExpressionText("lower(customer_name)");
  const auto net_price = opt::CanonicalizeExpressionText("net_price");
  auto generated = BaseIndex(relation_uuid, "idx.odf022.generated.lower_name");
  generated.generated_column_expression_digest = lower_name.digest;
  auto computed = BaseIndex(relation_uuid, "idx.odf022.computed.net_price");
  computed.computed_expression_digest = net_price.digest;

  auto generated_request = BaseRequest(relation_uuid);
  generated_request.candidate_indexes = {generated};
  const auto generated_match = opt::MatchPredicateToIndex({generated_request.predicate_text,
                                                          generated_request.descriptor_digest,
                                                          generated_request.collation_identity,
                                                          true,
                                                          true,
                                                          generated});
  if (!Require(generated_match.matches, "generated column expression did not match by digest") ||
      !Require(Has(generated_match.acceptance_reasons, "generated_column_expression_digest_match"),
               "generated column acceptance reason missing")) {
    return false;
  }

  auto computed_request = BaseRequest(relation_uuid);
  computed_request.predicate_text = "10 = net_price";
  computed_request.candidate_indexes = {computed};
  const auto computed_match = opt::MatchPredicateToIndex({computed_request.predicate_text,
                                                         computed_request.descriptor_digest,
                                                         computed_request.collation_identity,
                                                         true,
                                                         true,
                                                         computed});
  if (!Require(computed_match.matches, "computed expression did not match by digest") ||
      !Require(Has(computed_match.acceptance_reasons, "computed_expression_digest_match"),
               "computed expression acceptance reason missing")) {
    return false;
  }

  auto partial = generated;
  partial.index_uuid = "idx.odf022.partial.refused";
  partial.partial = true;
  partial.partial_predicate_text = "active = true";
  auto refused_request = BaseRequest(relation_uuid);
  refused_request.predicate_text = "lower(customer_name) = 'ada'";
  const auto refused = opt::MatchPredicateToIndex({refused_request.predicate_text,
                                                  refused_request.descriptor_digest,
                                                  refused_request.collation_identity,
                                                  true,
                                                  true,
                                                  partial});
  if (!Require(!refused.matches, "partial index matched without implication proof") ||
      !Require(Has(refused.refusal_reasons, "partial_predicate_not_proven"),
               "partial refusal reason was not exact")) {
    return false;
  }

  auto no_mga = BaseRequest(relation_uuid);
  no_mga.base_row_mga_recheck_planned = false;
  const auto recheck_refused = opt::MatchPredicateToIndex({no_mga.predicate_text,
                                                          no_mga.descriptor_digest,
                                                          no_mga.collation_identity,
                                                          false,
                                                          true,
                                                          generated});
  return Require(!recheck_refused.matches, "metadata match succeeded without MGA recheck evidence") &&
         Require(Has(recheck_refused.refusal_reasons, "metadata_match_only_mga_visibility_recheck_missing"),
                 "MGA recheck refusal reason missing");
}

bool LikePrefixAcceptanceAndRefusalsAreExact() {
  const std::string relation_uuid = "rel.odf022.like";
  auto index = BaseIndex(relation_uuid, "idx.odf022.like.customer_name");
  index.like_prefix_capable = true;
  index.key_expression_digests = {opt::CanonicalizeExpressionText("customer_name").digest};

  auto accepted = BaseRequest(relation_uuid);
  accepted.predicate_kind = "like_prefix";
  accepted.predicate_text = "customer_name like 'Ada%'";
  const auto accepted_match = opt::MatchPredicateToIndex({accepted.predicate_text,
                                                         accepted.descriptor_digest,
                                                         accepted.collation_identity,
                                                         true,
                                                         true,
                                                         index});
  if (!Require(accepted_match.matches, "LIKE prefix predicate did not match prefix-capable index") ||
      !Require(Has(accepted_match.acceptance_reasons, "like_prefix_accepted"),
               "LIKE prefix acceptance reason missing")) {
    return false;
  }

  auto leading = accepted;
  leading.predicate_text = "customer_name like '%Ada'";
  const auto leading_refused = opt::MatchPredicateToIndex({leading.predicate_text,
                                                          leading.descriptor_digest,
                                                          leading.collation_identity,
                                                          true,
                                                          true,
                                                          index});
  if (!Require(!leading_refused.matches, "leading-wildcard LIKE matched prefix index") ||
      !Require(Has(leading_refused.refusal_reasons, "like_prefix_leading_wildcard_refused"),
               "leading wildcard refusal reason missing")) {
    return false;
  }

  auto nondeterministic = index;
  nondeterministic.collation_deterministic = false;
  const auto collation_refused = opt::MatchPredicateToIndex({accepted.predicate_text,
                                                            accepted.descriptor_digest,
                                                            accepted.collation_identity,
                                                            true,
                                                            true,
                                                            nondeterministic});
  if (!Require(!collation_refused.matches, "nondeterministic collation LIKE matched prefix index") ||
      !Require(Has(collation_refused.refusal_reasons, "like_prefix_nondeterministic_collation_refused"),
               "nondeterministic collation refusal reason missing")) {
    return false;
  }

  auto wrong_expression = index;
  wrong_expression.key_expression_digests = {opt::CanonicalizeExpressionText("other_name").digest};
  const auto expression_refused = opt::MatchPredicateToIndex({accepted.predicate_text,
                                                             accepted.descriptor_digest,
                                                             accepted.collation_identity,
                                                             true,
                                                             true,
                                                             wrong_expression});
  if (!Require(!expression_refused.matches, "LIKE prefix matched unrelated expression index") ||
      !Require(Has(expression_refused.refusal_reasons, "like_prefix_expression_digest_mismatch"),
               "LIKE prefix expression mismatch refusal reason missing")) {
    return false;
  }

  auto escape = accepted;
  escape.predicate_text = "customer_name like 'Ad_%' escape '#'";
  const auto escape_refused = opt::MatchPredicateToIndex({escape.predicate_text,
                                                         escape.descriptor_digest,
                                                         escape.collation_identity,
                                                         true,
                                                         true,
                                                         index});
  return Require(!escape_refused.matches, "unsupported LIKE escape matched prefix index") &&
         Require(Has(escape_refused.refusal_reasons, "like_prefix_unsupported_escape_refused"),
                 "unsupported escape refusal reason missing");
}

bool DescriptorCollationAndVolatilityRefusalsAreExact() {
  const std::string relation_uuid = "rel.odf022.refusal";
  const auto lower_name = opt::CanonicalizeExpressionText("lower(customer_name)");
  auto index = BaseIndex(relation_uuid, "idx.odf022.refusal");
  index.expression_index = true;
  index.key_expression_digests = {lower_name.digest};
  index.descriptor_digest = "desc:customer:v2";
  index.collation_identity = "unicode.nondet";
  index.function_volatility = "volatile";

  auto request = BaseRequest(relation_uuid);
  const auto refused = opt::MatchPredicateToIndex({request.predicate_text,
                                                  request.descriptor_digest,
                                                  request.collation_identity,
                                                  true,
                                                  true,
                                                  index});
  return Require(!refused.matches, "descriptor/collation/volatility mismatch unexpectedly matched") &&
         Require(Has(refused.refusal_reasons, "descriptor_digest_mismatch"),
                 "descriptor mismatch refusal missing") &&
         Require(Has(refused.refusal_reasons, "collation_mismatch"),
                 "collation mismatch refusal missing") &&
         Require(Has(refused.refusal_reasons, "function_volatility_mismatch"),
                 "function volatility refusal missing");
}

}  // namespace

int main() {
  if (!CanonicalFormsAreStableAndBooleanEquivalent()) return 1;
  if (!ExpressionPartialAndAccessCandidateMatch()) return 1;
  if (!GeneratedComputedAndPartialRefusalsAreExact()) return 1;
  if (!LikePrefixAcceptanceAndRefusalsAreExact()) return 1;
  if (!DescriptorCollationAndVolatilityRefusalsAreExact()) return 1;
  return 0;
}
