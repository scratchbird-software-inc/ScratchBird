// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "optimizer_differential_fuzz.hpp"
#include "../../sbsql_sblr_alignment/binary_uuid_fixture.hpp"

#include "access_path_full.hpp"
#include "join_planner_full.hpp"
#include "optimizer_plan_cache.hpp"
#include "optimizer_rewrite.hpp"
#include "partition_segment_pruning.hpp"
#include "predicate_normalization.hpp"

#include <algorithm>
#include <cstdint>
#include <map>
#include <optional>
#include <sstream>
#include <string_view>
#include <utility>

namespace scratchbird::engine::optimizer {
namespace {

using Uuid = scratchbird::core::platform::Uuid;
using scratchbird::tests::BinaryUuid;
// Named fixture constants only; no runtime identity issuance or name binding.
const Uuid kFixture0 = BinaryUuid("019f0000-0000-7300-8000-00000000f4ec");
const Uuid kFixture1 = BinaryUuid("019f0000-0000-7300-8000-00000000f4ed");
const Uuid kFixture2 = BinaryUuid("019f0000-0000-7300-8000-00000000f4ee");
const Uuid kFixture3 = BinaryUuid("019f0000-0000-7300-8000-00000000f4ef");
const Uuid kFixture4 = BinaryUuid("019f0000-0000-7300-8000-00000000f4f0");
const Uuid kFixture5 = BinaryUuid("019f0000-0000-7300-8000-00000000f4f1");
const Uuid kFixture6 = BinaryUuid("019f0000-0000-7300-8000-00000000f4f2");
const Uuid kFixture7 = BinaryUuid("019f0000-0000-7300-8000-00000000f4f3");
const Uuid kFixture8 = BinaryUuid("019f0000-0000-7300-8000-00000000f4f4");
const Uuid kFixture9 = BinaryUuid("019f0000-0000-7300-8000-00000000f4f5");
const Uuid kFixture10 = BinaryUuid("019f0000-0000-7300-8000-00000000f4f6");
const Uuid kFixture11 = BinaryUuid("019f0000-0000-7300-8000-00000000f4f7");

std::string IdentityEvidence(const Uuid& value) {
  // Presentation belongs to this test boundary, never engine identity keys.
  constexpr char digits[] = "0123456789abcdef";
  std::string text;
  for (const auto byte : value.bytes) {
    text.push_back(digits[byte >> 4]); text.push_back(digits[byte & 15]);
  }
  return text;
}
std::vector<std::string> IdentityEvidence(const std::vector<Uuid>& values) {
  std::vector<std::string> out;
  for (const auto& value : values) out.push_back(IdentityEvidence(value));
  return out;
}

bool Has(const std::vector<std::string>& values, std::string_view expected) {
  return std::find(values.begin(), values.end(), expected) != values.end();
}

void AddUnique(std::vector<std::string>* values, std::string value) {
  if (!Has(*values, value)) values->push_back(std::move(value));
}

std::string JoinSorted(std::vector<std::string> values, std::string_view delimiter) {
  std::sort(values.begin(), values.end());
  std::ostringstream out;
  for (std::size_t i = 0; i < values.size(); ++i) {
    if (i != 0) out << delimiter;
    out << values[i];
  }
  return out.str();
}

std::string JoinInOrder(const std::vector<std::string>& values, std::string_view delimiter) {
  std::ostringstream out;
  for (std::size_t i = 0; i < values.size(); ++i) {
    if (i != 0) out << delimiter;
    out << values[i];
  }
  return out.str();
}

OptimizerRouteEvidence AcceptedRoute(std::string digest,
                                     std::string result_class,
                                     std::vector<std::string> evidence) {
  OptimizerRouteEvidence route;
  route.accepted = true;
  route.canonical_semantic_digest = std::move(digest);
  route.result_class = std::move(result_class);
  route.evidence = std::move(evidence);
  AddUnique(&route.evidence, "route_kind=metadata_only");
  AddUnique(&route.evidence, "no_sql_backend_execution=true");
  AddUnique(&route.evidence, "mga_visibility_authority=engine_recheck_required");
  AddUnique(&route.evidence, "mga_finality_authority=engine_transaction_inventory");
  AddUnique(&route.evidence, "parser_or_reference_finality_authority=false");
  return route;
}

OptimizerRouteEvidence RefusedRoute(std::string diagnostic,
                                    std::vector<std::string> evidence) {
  OptimizerRouteEvidence route;
  route.accepted = false;
  route.exact_refusal_diagnostic = std::move(diagnostic);
  route.evidence = std::move(evidence);
  AddUnique(&route.evidence, "route_kind=metadata_only");
  AddUnique(&route.evidence, "no_sql_backend_execution=true");
  AddUnique(&route.evidence, "mga_visibility_authority=engine_recheck_required");
  AddUnique(&route.evidence, "mga_finality_authority=engine_transaction_inventory");
  AddUnique(&route.evidence, "parser_or_reference_finality_authority=false");
  AddUnique(&route.evidence, "refusal_is_exact=true");
  return route;
}

OptimizerRouteEvidence ExpectedRefusal(const OptimizerDifferentialCase& test_case) {
  return RefusedRoute(test_case.expected_refusal_diagnostic,
                      {"baseline_route=declared_component_expectation"});
}

OptimizerDifferentialCaseResult CompareRoutes(OptimizerDifferentialCase test_case,
                                              OptimizerRouteEvidence baseline,
                                              OptimizerRouteEvidence optimized) {
  OptimizerDifferentialCaseResult result;
  result.test_case = std::move(test_case);
  result.baseline = std::move(baseline);
  result.optimized = std::move(optimized);
  const bool expects_acceptance = result.test_case.expected_outcome ==
      OptimizerDifferentialOutcome::kAcceptedEquivalent;
  const bool expects_refusal = result.test_case.expected_outcome ==
      OptimizerDifferentialOutcome::kExactRefusalEquivalent;
  if (expects_acceptance && result.baseline.accepted && result.optimized.accepted &&
      result.baseline.canonical_semantic_digest ==
          result.optimized.canonical_semantic_digest &&
      result.baseline.result_class == result.optimized.result_class) {
    result.outcome = OptimizerDifferentialOutcome::kAcceptedEquivalent;
    return result;
  }
  if (expects_refusal && !result.baseline.accepted && !result.optimized.accepted &&
      !result.test_case.expected_refusal_diagnostic.empty() &&
      result.baseline.exact_refusal_diagnostic == result.test_case.expected_refusal_diagnostic &&
      result.baseline.exact_refusal_diagnostic ==
          result.optimized.exact_refusal_diagnostic) {
    result.outcome = OptimizerDifferentialOutcome::kExactRefusalEquivalent;
    return result;
  }

  result.outcome = OptimizerDifferentialOutcome::kMismatch;
  if ((!expects_acceptance && !expects_refusal) ||
      (expects_acceptance && !result.baseline.accepted && !result.optimized.accepted) ||
      (expects_refusal && result.baseline.accepted && result.optimized.accepted)) {
    result.mismatch_reason = "expected_acceptance_or_refusal_not_observed";
  } else if (result.baseline.accepted != result.optimized.accepted) {
    result.mismatch_reason = "accepted_refused_state_diverged";
  } else if (result.baseline.accepted) {
    result.mismatch_reason = "canonical_semantic_digest_or_result_class_diverged";
  } else {
    result.mismatch_reason = "exact_refusal_diagnostic_diverged";
  }
  return result;
}

OptimizerStatsIdentity FreshIdentity(const Uuid& object_uuid,
                                     const Uuid& statistic_uuid) {
  OptimizerStatsIdentity identity;
  identity.object_uuid = object_uuid;
  identity.statistic_uuid = statistic_uuid;
  identity.stats_epoch = 27;
  identity.catalog_epoch = 27;
  identity.transaction_visibility_epoch = 27;
  identity.freshness = OptimizerStatsFreshnessState::kFresh;
  identity.source = StatisticSource::kCatalogExact;
  identity.confidence = CostConfidence::kHigh;
  return identity;
}

IndexStats BaseIndex(const Uuid& relation_uuid,
                     const Uuid& index_uuid) {
  IndexStats stats;
  stats.identity = FreshIdentity(index_uuid, kFixture11);
  stats.index_uuid = index_uuid;
  stats.relation_uuid = relation_uuid;
  stats.index_family = "btree";
  stats.descriptor_digest = "desc:odf027:customer:v1";
  stats.collation_identity = "unicode.casefold.det";
  stats.key_column_uuids = {kFixture0};
  stats.covered_column_uuids = {kFixture0, kFixture1};
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

OptimizerPruneBoundary Boundary(const std::string& min_value,
                                const std::string& max_value) {
  OptimizerPruneBoundary boundary;
  boundary.scalar_type_key = "int64";
  boundary.encoded_min = min_value;
  boundary.encoded_max = max_value;
  boundary.min_present = true;
  boundary.max_present = true;
  return boundary;
}

OptimizerPrunePredicate PrunePredicate() {
  OptimizerPrunePredicate predicate;
  predicate.scalar_type_key = "int64";
  predicate.encoded_lower = "050";
  predicate.encoded_upper = "060";
  predicate.lower_present = true;
  predicate.upper_present = true;
  return predicate;
}

OptimizerBarrierInput SafeBarrier() {
  OptimizerBarrierInput barrier;
  barrier.security_context_present = true;
  barrier.grants_proven = true;
  return barrier;
}

OptimizerExpressionTerm SafeTerm(const std::string& term_id,
                                 const std::string& descriptor_digest) {
  OptimizerExpressionTerm term;
  term.term_id = term_id;
  term.operator_id = "op.add";
  term.descriptor_digest = descriptor_digest;
  term.input_term_ids = {"col.amount", "literal.tax"};
  return term;
}

MaterializedSummaryRewriteInput BaseSummaryInput() {
  MaterializedSummaryRewriteInput input;
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

CommonSubexpressionReuseInput BaseCseInput() {
  CommonSubexpressionReuseInput input;
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

OptimizerPlanCacheKeyInput BaseCacheInput() {
  OptimizerPlanCacheKeyInput input;
  input.operation_id = "odf027.select";
  input.sblr_digest = "sblr:uuid-bound:odf027";
  input.descriptor_set_digest = "descriptor:rel.customer:v1";
  input.statistics_snapshot_id = "stats:snapshot:27";
  input.catalog_stats_digest = "catalog_stats:rel.customer:27";
  input.cost_profile_id = "cost:local:v1";
  input.executor_capability_set_id = "executor:local:mga:v1";
  input.route_capability_digest = "route:local:scan-index:v1";
  input.security_policy_digest = "security:tenant-reader:v7";
  input.redaction_route_digest = "redaction:mask-email:v3";
  input.parameter_shape_digest = "slot0:int64:not_null:card=point:range=42";
  input.memory_grant_class = "memory:small";
  input.memory_grant_digest = "grant:small:64k";
  input.catalog_epoch = 100;
  input.stats_epoch = 101;
  input.security_epoch = 102;
  input.policy_epoch = 103;
  input.resource_epoch = 104;
  input.name_resolution_epoch = 105;
  input.memory_policy_epoch = 106;
  input.compatibility_epoch = 107;
  input.format_compatibility_epoch = 108;
  input.route_epoch = 109;
  input.object_uuids = {kFixture2};
  input.function_uuids = {kFixture3};
  input.index_uuids = {kFixture4};
  input.filespace_uuids = {kFixture5};
  return input;
}

CachedOptimizerPlan CachedPlan(const OptimizerPlanCacheKeyInput& input) {
  CachedOptimizerPlan plan;
  plan.key_input = input;
  plan.cache_key = BuildOptimizerPlanCacheKey(input);
  plan.created_epoch = input.catalog_epoch;
  plan.result.ok = true;
  plan.result.plan_id = "odf027.index_lookup";
  plan.result.diagnostic_code = "ok";
  return plan;
}

JoinGraph InnerJoinGraph() {
  std::vector<JoinRelationNode> relations = {
      {.relation_uuid = kFixture6, .estimated_rows = 10000},
      {.relation_uuid = kFixture7, .estimated_rows = 200},
      {.relation_uuid = kFixture8, .estimated_rows = 10},
  };
  std::vector<JoinPredicateEdge> predicates;
  JoinPredicateEdge big_medium;
  big_medium.left_relation_uuid = kFixture6;
  big_medium.right_relation_uuid = kFixture7;
  big_medium.predicate_kind = "join.equi";
  big_medium.equality = true;
  big_medium.selectivity = 0.05;
  predicates.push_back(big_medium);
  JoinPredicateEdge medium_small = big_medium;
  medium_small.left_relation_uuid = kFixture7;
  medium_small.right_relation_uuid = kFixture8;
  predicates.push_back(medium_small);
  return BuildJoinGraph(std::move(relations), std::move(predicates), false, false);
}

JoinGraph BarrierJoinGraph(const OptimizerDifferentialCase& test_case) {
  std::vector<JoinRelationNode> relations = {
      {.relation_uuid = kFixture6, .estimated_rows = 10000},
      {.relation_uuid = kFixture8, .estimated_rows = 10},
  };
  JoinPredicateEdge edge;
  edge.left_relation_uuid = kFixture6;
  edge.right_relation_uuid = kFixture8;
  edge.predicate_kind = "join.equi";
  edge.equality = true;
  bool contains_outer = false;
  bool contains_semi_or_anti = false;

  if (test_case.case_id == "join_outer_barrier_exact_refusal") {
    edge.semantic_kind = JoinSemanticKind::kLeftOuter;
    edge.outer_join_sensitive = true;
    contains_outer = true;
  } else if (test_case.case_id == "join_semi_barrier_exact_refusal") {
    edge.semantic_kind = JoinSemanticKind::kSemi;
    contains_semi_or_anti = true;
  } else if (test_case.case_id == "join_anti_barrier_exact_refusal") {
    edge.semantic_kind = JoinSemanticKind::kAnti;
    contains_semi_or_anti = true;
  } else if (test_case.case_id == "join_correlation_barrier_exact_refusal") {
    edge.correlated = true;
  } else if (test_case.case_id == "join_lateral_barrier_exact_refusal") {
    edge.lateral = true;
  } else if (test_case.case_id == "join_volatile_barrier_exact_refusal") {
    edge.volatile_predicate = true;
  } else {
    edge.explicit_order_barrier = true;
  }
  return BuildJoinGraph(std::move(relations), {edge}, contains_outer,
                        contains_semi_or_anti);
}

OptimizerRouteEvidence PredicateRoute(const std::string& expression,
                                      std::string result_class) {
  const auto canonical =
      result_class == "expression_digest"
          ? CanonicalizeExpressionText(expression)
          : CanonicalizePredicateText(expression);
  if (!canonical.ok) {
    return RefusedRoute(JoinSorted(canonical.diagnostics, "+"),
                        {"predicate_canonicalization_refused"});
  }
  return AcceptedRoute("predicate:" + canonical.digest, std::move(result_class),
                       {"canonical_text=" + canonical.canonical_text,
                        "descriptor_digest=" +
                            DeterministicPredicateDescriptorText(canonical)});
}

OptimizerRouteEvidence JoinAcceptedRoute(const JoinGraph& graph,
                                         const JoinOrderPlan& order) {
  if (!order.ok) {
    return RefusedRoute(JoinSorted(order.diagnostics, "+"),
                        {"join_order_not_available"});
  }
  std::vector<Uuid> relation_uuids;
  for (const auto& relation : graph.relations) {
    relation_uuids.push_back(relation.relation_uuid);
  }
  auto ordered_ids = order.ordered_relation_uuids;
  std::sort(ordered_ids.begin(), ordered_ids.end());
  auto expected_ids = relation_uuids;
  std::sort(expected_ids.begin(), expected_ids.end());
  if (ordered_ids != expected_ids) return RefusedRoute("fixture.join_binding_permutation_invalid", {});
  return AcceptedRoute(
      "join:inner:equi:" + JoinSorted(IdentityEvidence(relation_uuids), ","),
      "inner_join_semantic_result",
      {"optimized_order=" + JoinInOrder(IdentityEvidence(order.ordered_relation_uuids), ","),
       "reorder_applied=" + std::string(order.reorder_applied ? "true" : "false"),
       "bounded_enumeration_applied=" +
           std::string(order.bounded_enumeration_applied ? "true" : "false")});
}

OptimizerRouteEvidence JoinBaselineRoute(const JoinGraph& graph) {
  std::vector<Uuid> relation_uuids;
  std::vector<Uuid> input_order;
  for (const auto& relation : graph.relations) {
    relation_uuids.push_back(relation.relation_uuid);
    input_order.push_back(relation.relation_uuid);
  }
  return AcceptedRoute("join:inner:equi:" + JoinSorted(IdentityEvidence(relation_uuids), ","),
                       "inner_join_semantic_result",
                       {"baseline_order=" + JoinInOrder(IdentityEvidence(input_order), ","),
                        "reorder_applied=false",
                        "baseline_route=semantic_reference"});
}

OptimizerRouteEvidence JoinRefusalRoute(const JoinGraph& graph,
                                        const JoinOrderPlan& order) {
  std::vector<Uuid> input_order;
  for (const auto& relation : graph.relations) input_order.push_back(relation.relation_uuid);
  const bool order_preserved = order.ordered_relation_uuids == input_order;
  if (!order.ok || !order_preserved || !order.semantic_order_preserved)
    return RefusedRoute("fixture.join_order_contract_violated", {});
  std::vector<std::string> evidence = {
      "join_reorder_allowed=false",
      "input_order_preserved=" + std::string(order_preserved ? "true" : "false"),
      "semantic_order_preserved=" +
          std::string(order.semantic_order_preserved ? "true" : "false")};
  return RefusedRoute(JoinSorted(order.diagnostics, "+"), std::move(evidence));
}

OptimizerRouteEvidence SummaryRewriteRoute(const MaterializedSummaryRewriteInput& input) {
  const auto decision = SelectMaterializedSummaryRewrite(input, SafeBarrier());
  if (!decision.applied) {
    return RefusedRoute(JoinSorted(decision.diagnostics, "+"),
                        {"rewrite_kind=materialized_summary"});
  }
  return AcceptedRoute("rewrite:summary_semantic:" + input.query_descriptor_digest,
                       "materialized_summary_semantic_result",
                       {"canonical_form=" + decision.canonical_form,
                        "equivalence_proof=" + input.equivalence_proof_digest,
                        "base_row_recheck=planned"});
}

OptimizerRouteEvidence SummaryBaselineRoute(const MaterializedSummaryRewriteInput& input) {
  return AcceptedRoute("rewrite:summary_semantic:" + input.query_descriptor_digest,
                       "materialized_summary_semantic_result",
                       {"baseline_route=base_query",
                        "rewrite_applied=false",
                        "base_row_recheck=planned"});
}

OptimizerRouteEvidence CseRewriteRoute(const CommonSubexpressionReuseInput& input) {
  const auto decision = SelectCommonSubexpressionReuse(input, SafeBarrier());
  if (!decision.applied) {
    return RefusedRoute(JoinSorted(decision.diagnostics, "+"),
                        {"rewrite_kind=common_subexpression"});
  }
  return AcceptedRoute("rewrite:cse_semantic:" + input.equivalence_proof_digest,
                       "common_subexpression_semantic_result",
                       {"canonical_form=" + decision.canonical_form,
                        "preserved_terms=" +
                            JoinInOrder(decision.preserved_column_uuids, ",")});
}

OptimizerRouteEvidence CseBaselineRoute(const CommonSubexpressionReuseInput& input) {
  return AcceptedRoute("rewrite:cse_semantic:" + input.equivalence_proof_digest,
                       "common_subexpression_semantic_result",
                       {"baseline_route=recompute_expression",
                        "rewrite_applied=false"});
}

OptimizerRouteEvidence CacheBaselineRoute(const OptimizerPlanCacheKeyInput& input) {
  return AcceptedRoute("cache_semantic:" + BuildOptimizerPlanCacheKey(input),
                       "safe_cached_plan_reuse",
                       {"baseline_route=replan_with_same_metadata",
                        "cache_lookup=false"});
}

OptimizerRouteEvidence CacheLookupRoute(const OptimizerPlanCacheKeyInput& lookup_input) {
  OptimizerPlanCache cache;
  cache.Put(CachedPlan(BaseCacheInput()));
  const auto lookup = cache.Lookup(lookup_input);
  if (!lookup.hit) {
    return RefusedRoute(lookup.diagnostic_code,
                        {"cache_key=" + lookup.cache_key,
                         "lookup_result=miss_or_refusal"});
  }
  return AcceptedRoute("cache_semantic:" + lookup.cache_key,
                       "safe_cached_plan_reuse",
                       lookup.evidence);
}

OptimizerRouteEvidence PredicateIndexRoute(const PredicateIndexMatchRequest& request) {
  const auto match = MatchPredicateToIndex(request);
  if (!match.matches) {
    return RefusedRoute(JoinSorted(match.refusal_reasons, "+"),
                        {"index_uuid=" + IdentityEvidence(request.index.index_uuid)});
  }
  return AcceptedRoute("index:" + IdentityEvidence(request.index.index_uuid) + ":predicate:" +
                           match.canonical_predicate_digest,
                       "index_access_semantic_result",
                       match.acceptance_reasons);
}

OptimizerRouteEvidence PartitionPruneRoute(
    const OptimizerPartitionSegmentPruneRequest& request) {
  const auto plan = PlanPartitionSegmentPruning(request);
  if (!plan.any_pruned) {
    return RefusedRoute(JoinSorted(plan.evidence.refusal_reasons, "+"),
                        {"partition_segment_pruning=refused"});
  }
  if (!plan.evidence.base_row_mga_recheck_required ||
      !plan.evidence.base_row_security_recheck_required ||
      plan.evidence.pruning_metadata_visibility_authority ||
      plan.evidence.pruning_metadata_finality_authority) {
    return RefusedRoute("partition_pruning_authority_contract_violated",
                        {"partition_segment_pruning=authority_refused"});
  }
  return AcceptedRoute("prune:partition_segment_semantic:" + request.relation_uuid,
                       "advisory_pruning_semantic_result",
                       plan.evidence.acceptance_reasons);
}

OptimizerRouteEvidence PartitionBaselineRoute(
    const OptimizerPartitionSegmentPruneRequest& request) {
  return AcceptedRoute("prune:partition_segment_semantic:" + request.relation_uuid,
                       "advisory_pruning_semantic_result",
                       {"baseline_route=full_scan",
                        "base_row_mga_recheck_required=true",
                        "base_row_security_recheck_required=true"});
}

OptimizerDifferentialCaseResult RunPredicateCase(
    const OptimizerDifferentialCase& test_case) {
  if (test_case.case_id == "predicate_commuted_equality_equivalence") {
    return CompareRoutes(
        test_case,
        PredicateRoute("active and lower(customer_name) = 42", "predicate_digest"),
        PredicateRoute("42 = lower(customer_name) and active = true",
                       "predicate_digest"));
  }
  if (test_case.case_id == "predicate_and_or_flatten_reorder_equivalence") {
    return CompareRoutes(
        test_case,
        PredicateRoute("(tier = 'gold' or tier = 'platinum') or active",
                       "predicate_digest"),
        PredicateRoute("active or tier = 'platinum' or tier = 'gold'",
                       "predicate_digest"));
  }
  if (test_case.case_id == "predicate_double_not_equivalence") {
    return CompareRoutes(test_case,
                         PredicateRoute("active = true", "predicate_digest"),
                         PredicateRoute("not not active", "predicate_digest"));
  }
  return CompareRoutes(
      test_case,
      PredicateRoute("LOWER( customer_name )", "expression_digest"),
      PredicateRoute("lower(customer_name)", "expression_digest"));
}

OptimizerDifferentialCaseResult RunJoinCase(
    const OptimizerDifferentialCase& test_case) {
  if (test_case.case_id == "join_inner_reorder_accepted_equivalence") {
    const auto graph = InnerJoinGraph();
    const auto order = EnumerateDeterministicJoinOrder(graph, 8 * 1024 * 1024);
    return CompareRoutes(test_case, JoinBaselineRoute(graph),
                         JoinAcceptedRoute(graph, order));
  }
  const auto graph = BarrierJoinGraph(test_case);
  const auto order = EnumerateDeterministicJoinOrder(graph, 8 * 1024 * 1024);
  return CompareRoutes(test_case, ExpectedRefusal(test_case),
                       JoinRefusalRoute(graph, order));
}

OptimizerDifferentialCaseResult RunRewriteCase(
    const OptimizerDifferentialCase& test_case) {
  if (test_case.case_id == "rewrite_materialized_summary_proof_equivalence") {
    return CompareRoutes(test_case, SummaryBaselineRoute(BaseSummaryInput()),
                         SummaryRewriteRoute(BaseSummaryInput()));
  }
  if (test_case.case_id == "rewrite_materialized_summary_exact_refusal") {
    auto input = BaseSummaryInput();
    input.equivalence_proven = false;
    input.equivalence_proof_digest.clear();
    input.summary_fresh = false;
    input.base_row_mga_recheck_planned = false;
    input.base_row_security_recheck_planned = false;
    input.mga_compatible = false;
    input.estimated_rewrite_cost = input.estimated_base_cost;
    return CompareRoutes(test_case, ExpectedRefusal(test_case),
                         SummaryRewriteRoute(input));
  }
  if (test_case.case_id == "rewrite_cse_proof_equivalence") {
    return CompareRoutes(test_case, CseBaselineRoute(BaseCseInput()),
                         CseRewriteRoute(BaseCseInput()));
  }
  auto input = BaseCseInput();
  input.equivalence_proven = false;
  input.mga_compatible = false;
  input.base_row_mga_recheck_planned = false;
  input.base_row_security_recheck_planned = false;
  input.estimated_reuse_cost = input.estimated_recompute_cost;
  input.terms = {SafeTerm("expr.single", "digest.single:v1")};
  return CompareRoutes(test_case, ExpectedRefusal(test_case), CseRewriteRoute(input));
}

OptimizerDifferentialCaseResult RunCacheCase(
    const OptimizerDifferentialCase& test_case) {
  auto input = BaseCacheInput();
  if (test_case.case_id == "plan_cache_equivalent_shape_hit") {
    return CompareRoutes(test_case, CacheBaselineRoute(input), CacheLookupRoute(input));
  }
  if (test_case.case_id == "plan_cache_parameter_shape_exact_refusal") {
    input.parameter_shape_digest = "slot0:int64:not_null:card=range:range=1..10";
  } else if (test_case.case_id == "plan_cache_security_redaction_exact_refusal") {
    input.redaction_route_digest = "redaction:none";
  } else if (test_case.case_id == "plan_cache_memory_shape_exact_refusal") {
    input.memory_grant_class = "memory:large";
    input.memory_grant_digest = "grant:large:64m";
  } else {
    input.route_capability_digest = "route:remote-pushdown:v1";
  }
  return CompareRoutes(test_case, ExpectedRefusal(test_case), CacheLookupRoute(input));
}

OptimizerDifferentialCaseResult RunAccessPathCase(
    const OptimizerDifferentialCase& test_case) {
  const Uuid relation_uuid = kFixture9;
  auto index = BaseIndex(relation_uuid, kFixture10);
  index.expression_index = true;
  index.key_expression_digests = {CanonicalizeExpressionText("lower(customer_name)").digest};
  index.partial = true;
  index.partial_predicate_text = "active = true";

  if (test_case.case_id == "access_path_predicate_index_equivalence") {
    PredicateIndexMatchRequest baseline;
    baseline.query_predicate_text = "active and lower(customer_name) = 'ada'";
    baseline.descriptor_digest = index.descriptor_digest;
    baseline.collation_identity = index.collation_identity;
    baseline.index = index;
    PredicateIndexMatchRequest optimized = baseline;
    optimized.query_predicate_text = "lower(customer_name) = 'ada' and active = true";
    return CompareRoutes(test_case, PredicateIndexRoute(baseline),
                         PredicateIndexRoute(optimized));
  }
  if (test_case.case_id == "access_path_missing_recheck_exact_refusal") {
    PredicateIndexMatchRequest request;
    request.query_predicate_text = "active and lower(customer_name) = 'ada'";
    request.descriptor_digest = index.descriptor_digest;
    request.collation_identity = index.collation_identity;
    request.base_row_mga_recheck_planned = false;
    request.base_row_security_recheck_planned = true;
    request.index = index;
    return CompareRoutes(test_case, ExpectedRefusal(test_case),
                         PredicateIndexRoute(request));
  }

  OptimizerPartitionSegmentPruneRequest request;
  request.requested = true;
  request.relation_uuid = "rel.odf027.access"; // Existing legacy pruning carrier, migrated separately.
  request.predicate = PrunePredicate();
  request.base_row_mga_recheck_planned =
      test_case.case_id != "access_path_pruning_recheck_exact_refusal";
  request.base_row_security_recheck_planned =
      test_case.case_id != "access_path_pruning_recheck_exact_refusal";
  request.partitions = {
      {"partition.old", Boundary("001", "010"), 200, true, true, true},
      {"partition.hit", Boundary("050", "070"), 300, true, true, true},
  };
  if (test_case.case_id == "access_path_pruning_advisory_equivalence") {
    return CompareRoutes(test_case, PartitionBaselineRoute(request),
                         PartitionPruneRoute(request));
  }
  return CompareRoutes(test_case, ExpectedRefusal(test_case),
                       PartitionPruneRoute(request));
}

}  // namespace

OptimizerDifferentialCaseResult CompareOptimizerDifferentialRoutes(
    OptimizerDifferentialCase test_case, OptimizerRouteEvidence baseline,
    OptimizerRouteEvidence optimized) {
  return CompareRoutes(std::move(test_case), std::move(baseline), std::move(optimized));
}

std::vector<OptimizerDifferentialCase> GenerateOptimizerDifferentialFuzzCorpus() {
  return {
      {"predicate_commuted_equality_equivalence",
       OptimizerDifferentialCaseClass::kPredicateEquivalence,
       "commuted equality plus boolean predicate canonical digest equivalence",
       OptimizerDifferentialOutcome::kAcceptedEquivalent},
      {"predicate_and_or_flatten_reorder_equivalence",
       OptimizerDifferentialCaseClass::kPredicateEquivalence,
       "AND/OR flatten and reorder canonical digest equivalence",
       OptimizerDifferentialOutcome::kAcceptedEquivalent},
      {"predicate_double_not_equivalence",
       OptimizerDifferentialCaseClass::kPredicateEquivalence,
       "double-NOT boolean canonical digest equivalence",
       OptimizerDifferentialOutcome::kAcceptedEquivalent},
      {"expression_digest_equivalence",
       OptimizerDifferentialCaseClass::kPredicateEquivalence,
       "expression digest equivalence independent of raw text shape",
       OptimizerDifferentialOutcome::kAcceptedEquivalent},
      {"join_inner_reorder_accepted_equivalence",
       OptimizerDifferentialCaseClass::kJoinOrderBarrier,
       "inner join reorder keeps the same join semantic result class",
       OptimizerDifferentialOutcome::kAcceptedEquivalent},
      {"join_outer_barrier_exact_refusal",
       OptimizerDifferentialCaseClass::kJoinOrderBarrier,
       "outer join reorder has exact semantic-barrier refusal",
       OptimizerDifferentialOutcome::kExactRefusalEquivalent,
       "SB_OPT_JOIN_INPUT_ORDER_SELECTED_BY_SEMANTIC_BARRIER+SB_OPT_JOIN_ORDER_PRESERVED_BY_SEMANTIC_BARRIER+SB_OPT_JOIN_ORDER_PRESERVED_OUTER_JOIN"},
      {"join_semi_barrier_exact_refusal",
       OptimizerDifferentialCaseClass::kJoinOrderBarrier,
       "semi join reorder has exact semantic-barrier refusal",
       OptimizerDifferentialOutcome::kExactRefusalEquivalent,
       "SB_OPT_JOIN_INPUT_ORDER_SELECTED_BY_SEMANTIC_BARRIER+SB_OPT_JOIN_ORDER_PRESERVED_BY_SEMANTIC_BARRIER+SB_OPT_JOIN_ORDER_PRESERVED_SEMI_JOIN+SB_OPT_JOIN_ORDER_PRESERVED_SEMI_OR_ANTI_JOIN"},
      {"join_anti_barrier_exact_refusal",
       OptimizerDifferentialCaseClass::kJoinOrderBarrier,
       "anti join reorder has exact semantic-barrier refusal",
       OptimizerDifferentialOutcome::kExactRefusalEquivalent,
       "SB_OPT_JOIN_INPUT_ORDER_SELECTED_BY_SEMANTIC_BARRIER+SB_OPT_JOIN_ORDER_PRESERVED_ANTI_JOIN+SB_OPT_JOIN_ORDER_PRESERVED_BY_SEMANTIC_BARRIER+SB_OPT_JOIN_ORDER_PRESERVED_SEMI_OR_ANTI_JOIN"},
      {"join_correlation_barrier_exact_refusal",
       OptimizerDifferentialCaseClass::kJoinOrderBarrier,
       "correlation reorder has exact semantic-barrier refusal",
       OptimizerDifferentialOutcome::kExactRefusalEquivalent,
       "SB_OPT_JOIN_INPUT_ORDER_SELECTED_BY_SEMANTIC_BARRIER+SB_OPT_JOIN_ORDER_PRESERVED_BY_SEMANTIC_BARRIER+SB_OPT_JOIN_ORDER_PRESERVED_CORRELATION"},
      {"join_lateral_barrier_exact_refusal",
       OptimizerDifferentialCaseClass::kJoinOrderBarrier,
       "lateral dependency reorder has exact semantic-barrier refusal",
       OptimizerDifferentialOutcome::kExactRefusalEquivalent,
       "SB_OPT_JOIN_INPUT_ORDER_SELECTED_BY_SEMANTIC_BARRIER+SB_OPT_JOIN_ORDER_PRESERVED_BY_SEMANTIC_BARRIER+SB_OPT_JOIN_ORDER_PRESERVED_LATERAL"},
      {"join_volatile_barrier_exact_refusal",
       OptimizerDifferentialCaseClass::kJoinOrderBarrier,
       "volatile predicate reorder has exact semantic-barrier refusal",
       OptimizerDifferentialOutcome::kExactRefusalEquivalent,
       "SB_OPT_JOIN_INPUT_ORDER_SELECTED_BY_SEMANTIC_BARRIER+SB_OPT_JOIN_ORDER_PRESERVED_BY_SEMANTIC_BARRIER+SB_OPT_JOIN_ORDER_PRESERVED_VOLATILE"},
      {"join_explicit_barrier_exact_refusal",
       OptimizerDifferentialCaseClass::kJoinOrderBarrier,
       "explicit barrier reorder has exact semantic-barrier refusal",
       OptimizerDifferentialOutcome::kExactRefusalEquivalent,
       "SB_OPT_JOIN_INPUT_ORDER_SELECTED_BY_SEMANTIC_BARRIER+SB_OPT_JOIN_ORDER_PRESERVED_BY_SEMANTIC_BARRIER+SB_OPT_JOIN_ORDER_PRESERVED_EXPLICIT_BARRIER"},
      {"rewrite_materialized_summary_proof_equivalence",
       OptimizerDifferentialCaseClass::kRewriteProof,
       "materialized summary rewrite accepted only with proof and rechecks",
       OptimizerDifferentialOutcome::kAcceptedEquivalent},
      {"rewrite_materialized_summary_exact_refusal",
       OptimizerDifferentialCaseClass::kRewriteProof,
       "unsafe materialized summary rewrite exact refusal",
       OptimizerDifferentialOutcome::kExactRefusalEquivalent,
       "SB_OPT_REWRITE_BASE_ROW_MGA_RECHECK_MISSING+SB_OPT_REWRITE_BASE_ROW_SECURITY_RECHECK_MISSING+SB_OPT_REWRITE_EQUIVALENCE_PROOF_MISSING+SB_OPT_REWRITE_MGA_INCOMPATIBLE_STATE+SB_OPT_REWRITE_NO_BENEFIT+SB_OPT_REWRITE_SUMMARY_STALE"},
      {"rewrite_cse_proof_equivalence",
       OptimizerDifferentialCaseClass::kRewriteProof,
       "deterministic common subexpression reuse accepted with proof",
       OptimizerDifferentialOutcome::kAcceptedEquivalent},
      {"rewrite_cse_exact_refusal",
       OptimizerDifferentialCaseClass::kRewriteProof,
       "unsafe common subexpression reuse exact refusal",
       OptimizerDifferentialOutcome::kExactRefusalEquivalent,
       "SB_OPT_REWRITE_BASE_ROW_MGA_RECHECK_MISSING+SB_OPT_REWRITE_BASE_ROW_SECURITY_RECHECK_MISSING+SB_OPT_REWRITE_EQUIVALENCE_PROOF_MISSING+SB_OPT_REWRITE_MGA_INCOMPATIBLE_STATE+SB_OPT_REWRITE_NO_BENEFIT+SB_OPT_REWRITE_NO_COMMON_EXPRESSION"},
      {"plan_cache_equivalent_shape_hit",
       OptimizerDifferentialCaseClass::kPlanCacheShape,
       "equivalent plan-cache shape reuses metadata-only plan safely",
       OptimizerDifferentialOutcome::kAcceptedEquivalent},
      {"plan_cache_parameter_shape_exact_refusal",
       OptimizerDifferentialCaseClass::kPlanCacheShape,
       "changed parameter shape exact cache refusal",
       OptimizerDifferentialOutcome::kExactRefusalEquivalent,
       "SB_OPTIMIZER_PLAN_CACHE_INCOMPATIBLE_PARAMETER_SHAPE"},
      {"plan_cache_security_redaction_exact_refusal",
       OptimizerDifferentialCaseClass::kPlanCacheShape,
       "changed security/redaction shape exact cache refusal",
       OptimizerDifferentialOutcome::kExactRefusalEquivalent,
       "SB_OPTIMIZER_PLAN_CACHE_REDACTION_SECURITY_POLICY_MISMATCH"},
      {"plan_cache_memory_shape_exact_refusal",
       OptimizerDifferentialCaseClass::kPlanCacheShape,
       "changed memory shape exact cache refusal",
       OptimizerDifferentialOutcome::kExactRefusalEquivalent,
       "SB_OPTIMIZER_PLAN_CACHE_MEMORY_GRANT_MISMATCH"},
      {"plan_cache_route_shape_exact_refusal",
       OptimizerDifferentialCaseClass::kPlanCacheShape,
       "changed route capability exact cache refusal",
       OptimizerDifferentialOutcome::kExactRefusalEquivalent,
       "SB_OPTIMIZER_PLAN_CACHE_ROUTE_CAPABILITY_MISMATCH"},
      {"access_path_predicate_index_equivalence",
       OptimizerDifferentialCaseClass::kAccessPathMetadata,
       "equivalent predicate/index combinations preserve semantic result",
       OptimizerDifferentialOutcome::kAcceptedEquivalent},
      {"access_path_missing_recheck_exact_refusal",
       OptimizerDifferentialCaseClass::kAccessPathMetadata,
       "metadata-only index match refuses without MGA recheck",
       OptimizerDifferentialOutcome::kExactRefusalEquivalent,
       "metadata_match_only_mga_visibility_recheck_missing+partial_predicate_mga_recheck_missing+partial_predicate_not_proven"},
      {"access_path_pruning_advisory_equivalence",
       OptimizerDifferentialCaseClass::kAccessPathMetadata,
       "partition pruning metadata is advisory with MGA/security recheck",
       OptimizerDifferentialOutcome::kAcceptedEquivalent},
      {"access_path_pruning_recheck_exact_refusal",
       OptimizerDifferentialCaseClass::kAccessPathMetadata,
       "partition pruning refuses exactly without rechecks",
       OptimizerDifferentialOutcome::kExactRefusalEquivalent,
       "partition_scanned_mga_recheck_missing+partition_scanned_mga_recheck_missing+partition_segment_prune_mga_recheck_missing+partition_segment_prune_security_recheck_missing"},
  };
}

OptimizerDifferentialCaseResult RunOptimizerDifferentialFuzzCase(
    const OptimizerDifferentialCase& test_case) {
  const auto corpus = GenerateOptimizerDifferentialFuzzCorpus();
  const auto known = std::find_if(corpus.begin(), corpus.end(), [&](const auto& value) {
    return value.case_id == test_case.case_id && value.case_class == test_case.case_class &&
        value.expected_outcome == test_case.expected_outcome &&
        value.expected_refusal_diagnostic == test_case.expected_refusal_diagnostic;
  });
  if (known == corpus.end()) {
    OptimizerDifferentialCaseResult result;
    result.test_case = test_case;
    result.mismatch_reason = "unknown_or_modified_case_contract";
    return result;
  }
  switch (test_case.case_class) {
    case OptimizerDifferentialCaseClass::kPredicateEquivalence:
      return RunPredicateCase(test_case);
    case OptimizerDifferentialCaseClass::kJoinOrderBarrier:
      return RunJoinCase(test_case);
    case OptimizerDifferentialCaseClass::kRewriteProof:
      return RunRewriteCase(test_case);
    case OptimizerDifferentialCaseClass::kPlanCacheShape:
      return RunCacheCase(test_case);
    case OptimizerDifferentialCaseClass::kAccessPathMetadata:
      return RunAccessPathCase(test_case);
  }
  return CompareRoutes(test_case, RefusedRoute("unknown_case_class", {}),
                       RefusedRoute("unknown_case_class", {}));
}

OptimizerDifferentialFuzzReport RunOptimizerDifferentialFuzzCorpus(
    const std::vector<OptimizerDifferentialCase>& corpus) {
  OptimizerDifferentialFuzzReport report;
  for (const auto& test_case : corpus) {
    auto result = RunOptimizerDifferentialFuzzCase(test_case);
    switch (result.outcome) {
      case OptimizerDifferentialOutcome::kAcceptedEquivalent:
        ++report.accepted_equivalent_count;
        break;
      case OptimizerDifferentialOutcome::kExactRefusalEquivalent:
        ++report.exact_refusal_equivalent_count;
        break;
      case OptimizerDifferentialOutcome::kMismatch:
        ++report.mismatch_count;
        break;
    }
    report.results.push_back(std::move(result));
  }
  return report;
}

std::string SerializeOptimizerDifferentialEvidence(
    const OptimizerDifferentialFuzzReport& report) {
  std::ostringstream out;
  out << "optimizer_differential_fuzz_report\n";
  out << "accepted_equivalent_count=" << report.accepted_equivalent_count << '\n';
  out << "exact_refusal_equivalent_count="
      << report.exact_refusal_equivalent_count << '\n';
  out << "mismatch_count=" << report.mismatch_count << '\n';
  for (const auto& result : report.results) {
    out << "case_id=" << result.test_case.case_id << '\n';
    out << "case_class="
        << OptimizerDifferentialCaseClassName(result.test_case.case_class) << '\n';
    out << "outcome=" << OptimizerDifferentialOutcomeName(result.outcome) << '\n';
    out << "baseline_accepted=" << (result.baseline.accepted ? "true" : "false")
        << '\n';
    out << "optimized_accepted="
        << (result.optimized.accepted ? "true" : "false") << '\n';
    out << "baseline_digest=" << result.baseline.canonical_semantic_digest << '\n';
    out << "optimized_digest=" << result.optimized.canonical_semantic_digest << '\n';
    out << "baseline_result_class=" << result.baseline.result_class << '\n';
    out << "optimized_result_class=" << result.optimized.result_class << '\n';
    out << "baseline_refusal=" << result.baseline.exact_refusal_diagnostic << '\n';
    out << "optimized_refusal=" << result.optimized.exact_refusal_diagnostic << '\n';
    out << "baseline_evidence=" << JoinInOrder(result.baseline.evidence, "|") << '\n';
    out << "optimized_evidence=" << JoinInOrder(result.optimized.evidence, "|") << '\n';
    if (!result.mismatch_reason.empty()) {
      out << "mismatch_reason=" << result.mismatch_reason << '\n';
    }
  }
  return out.str();
}

const char* OptimizerDifferentialCaseClassName(
    OptimizerDifferentialCaseClass case_class) {
  switch (case_class) {
    case OptimizerDifferentialCaseClass::kPredicateEquivalence:
      return "predicate_equivalence";
    case OptimizerDifferentialCaseClass::kJoinOrderBarrier:
      return "join_order_barrier";
    case OptimizerDifferentialCaseClass::kRewriteProof:
      return "rewrite_proof";
    case OptimizerDifferentialCaseClass::kPlanCacheShape:
      return "plan_cache_shape";
    case OptimizerDifferentialCaseClass::kAccessPathMetadata:
      return "access_path_metadata";
  }
  return "unknown";
}

const char* OptimizerDifferentialOutcomeName(
    OptimizerDifferentialOutcome outcome) {
  switch (outcome) {
    case OptimizerDifferentialOutcome::kAcceptedEquivalent:
      return "accepted_equivalent";
    case OptimizerDifferentialOutcome::kExactRefusalEquivalent:
      return "exact_refusal_equivalent";
    case OptimizerDifferentialOutcome::kMismatch:
      return "mismatch";
  }
  return "unknown";
}

}  // namespace scratchbird::engine::optimizer
