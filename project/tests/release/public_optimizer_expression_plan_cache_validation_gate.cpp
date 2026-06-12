// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "access_path_full.hpp"
#include "logical_plan.hpp"
#include "optimizer_plan_cache.hpp"
#include "optimizer_request.hpp"
#include "predicate_normalization.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace opt = scratchbird::engine::optimizer;
namespace plan = scratchbird::engine::planner;

namespace {

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

bool Contains(const std::vector<std::string>& values, std::string_view expected) {
  return std::find(values.begin(), values.end(), std::string(expected)) != values.end();
}

bool ContainsText(const std::vector<std::string>& values, std::string_view expected) {
  return std::any_of(values.begin(), values.end(), [&](const auto& value) {
    return value.find(expected) != std::string::npos;
  });
}

bool StartsWith(std::string_view value, std::string_view prefix) {
  return value.rfind(prefix, 0) == 0;
}

std::string Id(std::string_view suffix) {
  return "pcr063." + std::string(suffix);
}

opt::CanonicalSblrExpressionNode ColumnExpression(std::string column_uuid) {
  opt::CanonicalSblrExpressionNode node;
  node.operator_id = "column_ref";
  node.descriptor_digest = "sha256:descriptor-pcr063-customer";
  node.object_uuid = std::move(column_uuid);
  return node;
}

opt::CanonicalSblrExpressionNode LowerCustomerNameExpression() {
  opt::CanonicalSblrExpressionNode root;
  root.operator_id = "fn:lower";
  root.descriptor_digest = "sha256:descriptor-pcr063-customer";
  root.function_uuid = Id("function.lower");
  root.children = {ColumnExpression(Id("column.customer_name"))};
  return root;
}

opt::CanonicalSblrExpressionNode AddExpression(std::string left,
                                               std::string right) {
  opt::CanonicalSblrExpressionNode root;
  root.operator_id = "fn:add";
  root.descriptor_digest = "sha256:descriptor-pcr063-customer";
  root.function_uuid = Id("function.add");
  root.commutative = true;
  opt::CanonicalSblrExpressionNode left_node;
  left_node.operator_id = "literal";
  left_node.literal_digest = std::move(left);
  opt::CanonicalSblrExpressionNode right_node;
  right_node.operator_id = "literal";
  right_node.literal_digest = std::move(right);
  root.children = {std::move(left_node), std::move(right_node)};
  return root;
}

opt::OptimizerStatsIdentity FreshIdentity(std::string object_uuid,
                                          std::string statistic_uuid) {
  opt::OptimizerStatsIdentity identity;
  identity.object_uuid = std::move(object_uuid);
  identity.statistic_uuid = std::move(statistic_uuid);
  identity.stats_epoch = 6301;
  identity.catalog_epoch = 6300;
  identity.transaction_visibility_epoch = 6299;
  identity.freshness = opt::OptimizerStatsFreshnessState::kFresh;
  identity.source = opt::StatisticSource::kCatalogExact;
  identity.confidence = opt::CostConfidence::kHigh;
  return identity;
}

opt::TableCardinalityStats TableStats() {
  opt::TableCardinalityStats stats;
  stats.identity = FreshIdentity(Id("relation.customer"), Id("table_stats.customer"));
  stats.row_count = 10000;
  stats.visible_row_count = 9900;
  stats.page_count = 96;
  stats.average_row_bytes = 96;
  return stats;
}

opt::IndexStats ExpressionIndex(const std::string& expression_digest) {
  opt::IndexStats index;
  index.identity = FreshIdentity(Id("relation.customer"), Id("index.customer_name_lower.stats"));
  index.index_uuid = Id("index.customer_name_lower");
  index.relation_uuid = Id("relation.customer");
  index.index_family = "btree";
  index.descriptor_digest = "sha256:descriptor-pcr063-customer";
  index.collation_identity = "collation.pcr063.binary";
  index.key_expression_digests = {expression_digest};
  index.height = 2;
  index.leaf_pages = 24;
  index.distinct_keys = 8000;
  index.visibility_coverage = 1.0;
  index.expression_index = true;
  index.equality_lookup_supported = true;
  index.ordered_range_supported = true;
  index.exact_recheck_required = true;
  index.mga_recheck_required = true;
  index.security_recheck_required = true;
  index.route_benchmark_clean = true;
  return index;
}

plan::OptimizerPolicyMetadata SafePolicy() {
  plan::OptimizerPolicyMetadata policy;
  policy.optimizer_policy_metadata_present = true;
  policy.policy_source_kind = "sblr_api";
  policy.policy_epoch = 6300;
  policy.normalized_controls.plan_profile_id = "plan_profile:pcr063_cache";
  policy.normalized_controls.join_search_policy_id = "join_search:pcr063_bounded";
  policy.normalized_controls.memory_policy_id = "memory_policy:pcr063_governed";
  policy.normalized_controls.spill_policy_id = "spill_policy:pcr063_bounded";
  policy.normalized_controls.parallelism_policy_id = "parallelism:pcr063_local";
  policy.normalized_controls.what_if_policy_id = "what_if:pcr063_disabled";
  policy.normalized_controls.safe_control_ids = {
      "expression:sblr_canonical",
      "plan_cache:enterprise_key"};
  policy.safe_control_ids = {
      "security_recheck:preserved",
      "mga_recheck:preserved"};
  return policy;
}

plan::LogicalPlan LogicalPlan() {
  plan::LogicalPlan logical;
  logical.ok = true;
  logical.plan_id = Id("logical_plan.customer_lower_lookup");
  logical.optimizer_policy = SafePolicy();
  auto node = plan::MakeLogicalPlanNode(plan::LogicalPlanNodeKind::kDmlRead,
                                        plan::PhysicalAccessKind::kNone,
                                        Id("operation.customer_lower_lookup"),
                                        "customer_lower_lookup");
  node.required_object_uuids.push_back(Id("relation.customer"));
  node.required_descriptors.push_back("sha256:descriptor-pcr063-customer");
  logical.nodes.push_back(std::move(node));
  return logical;
}

opt::BoundOptimizerRequest BoundRequest() {
  opt::BoundOptimizerRequest request;
  request.context.request_uuid = Id("request.customer_lower_lookup");
  request.context.operation_id = Id("operation.customer_lower_lookup");
  request.context.sblr_digest = "sha256:sblr-pcr063-customer-lower-lookup";
  request.context.descriptor_set_digest = "sha256:descriptor-pcr063-customer";
  request.context.statistics_snapshot_id = "sha256:stats-pcr063-customer";
  request.context.metric_snapshot_id = "sha256:metrics-pcr063-customer";
  request.context.executor_capability_set_id = "executor-capability:pcr063-local-mga";
  request.context.catalog_epoch = 6300;
  request.context.stats_epoch = 6301;
  request.context.security_epoch = 6302;
  request.context.redaction_epoch = 6303;
  request.context.policy_epoch = 6304;
  request.context.resource_epoch = 6305;
  request.context.name_resolution_epoch = 6306;
  request.context.memory_policy_epoch = 6307;
  request.context.memory_feedback_generation = 6308;
  request.context.route_epoch = 6309;
  request.context.security_context_present = true;
  request.context.transaction_context_present = true;
  request.logical_plan = LogicalPlan();
  return request;
}

opt::OptimizerProductionPlanCacheKeyRequest ProductionCacheKeyRequest() {
  opt::OptimizerProductionPlanCacheKeyRequest request;
  request.bound_request = BoundRequest();
  request.catalog_stats_digest = "sha256:catalog-stats-pcr063-customer";
  request.cost_profile_id = "cost-profile:pcr063-catalog-v1";
  request.route_capability_digest = "sha256:route-capability-pcr063-local-index";
  request.security_policy_digest = "sha256:security-policy-pcr063-reader";
  request.redaction_route_digest = "sha256:redaction-route-pcr063-mask";
  request.parameter_shape_digest = "sha256:parameter-shape-pcr063-name-eq";
  request.memory_grant_class = "grant-class:pcr063-small-governed";
  request.memory_grant_digest = "sha256:memory-grant-pcr063-small";
  request.compatibility_epoch = 6310;
  request.format_compatibility_epoch = 6311;
  request.object_uuids = {Id("relation.customer")};
  request.function_uuids = {Id("function.lower")};
  request.index_uuids = {Id("index.customer_name_lower")};
  request.filespace_uuids = {Id("filespace.hot")};
  request.dependency_digests = {
      "sha256:dep-pcr063-relation-customer",
      "sha256:dep-pcr063-function-lower",
      "sha256:dep-pcr063-index-customer-name-lower",
      "sha256:dep-pcr063-filespace-hot",
      "sha256:dep-pcr063-route-local-index",
      "sha256:dep-pcr063-redaction-mask",
      "sha256:dep-pcr063-memory-grant",
      "sha256:dep-pcr063-cost-profile"};
  return request;
}

opt::CachedOptimizerPlan CachedPlan(const opt::OptimizerPlanCacheKeyInput& input) {
  opt::PlanCandidate candidate;
  candidate.candidate_id = "CAND-OPT-INDEX:" + Id("index.customer_name_lower");
  candidate.access_kind = plan::PhysicalAccessKind::kScalarBtreeLookup;
  candidate.required_facts = {
      Id("relation.customer"),
      Id("function.lower"),
      Id("index.customer_name_lower"),
      Id("filespace.hot")};
  candidate.cost.total_cost = 42;
  candidate.cost.confidence = opt::CostConfidence::kHigh;
  candidate.selected = true;

  opt::CachedOptimizerPlan cached;
  cached.key_input = input;
  cached.cache_key = opt::BuildOptimizerPlanCacheKey(input);
  cached.created_epoch = input.catalog_epoch;
  cached.result.ok = true;
  cached.result.diagnostic_code = "SB_OPT_OK";
  cached.result.plan_id = Id("cached_plan.customer_lower_lookup");
  cached.result.candidates.push_back(std::move(candidate));
  cached.metadata_only = true;
  cached.mga_visibility_recheck_required = true;
  cached.security_recheck_required = true;
  cached.parser_or_reference_finality_authority = false;
  return cached;
}

void CanonicalSblrExpressionMatchingIsProductionRoutable() {
  const auto expression = LowerCustomerNameExpression();
  const auto canonical = opt::CanonicalizeSblrExpressionTree(expression);
  Require(canonical.ok, "canonical SBLR expression should be accepted");
  Require(StartsWith(canonical.digest, "sblrexpr64:"),
          "canonical SBLR expression should produce SBLR digest");
  Require(Contains(canonical.evidence, "parser_sql_expression_authority=false"),
          "canonical SBLR expression must reject parser SQL authority");
  Require(Contains(canonical.evidence,
                   "transaction_finality_authority=engine_transaction_inventory"),
          "canonical SBLR expression must preserve MGA finality authority");

  const auto add_left = opt::CanonicalizeSblrExpressionTree(
      AddExpression("sha256:literal-a", "sha256:literal-b"));
  const auto add_right = opt::CanonicalizeSblrExpressionTree(
      AddExpression("sha256:literal-b", "sha256:literal-a"));
  Require(add_left.digest == add_right.digest,
          "commutative SBLR expression children should canonicalize deterministically");

  opt::SblrExpressionIndexMatchRequest match_request;
  match_request.query_expression = expression;
  match_request.descriptor_digest = "sha256:descriptor-pcr063-customer";
  match_request.collation_identity = "collation.pcr063.binary";
  match_request.base_row_mga_recheck_planned = true;
  match_request.base_row_security_recheck_planned = true;
  match_request.index = ExpressionIndex(canonical.digest);
  const auto match = opt::MatchSblrExpressionToIndex(match_request);
  Require(match.matches, "canonical SBLR expression should match expression index");
  Require(Contains(match.acceptance_reasons,
                   "canonical_sblr_expression_digest_match"),
          "expression match should prove canonical SBLR digest equality");
  Require(Contains(match.acceptance_reasons,
                   "metadata_match_only_mga_visibility_recheck_required"),
          "expression match must require MGA recheck");

  opt::AccessPathPlanningRequest access;
  access.relation_uuid = Id("relation.customer");
  access.predicate_kind = "scalar_eq";
  access.sblr_expression = expression;
  access.descriptor_digest = "sha256:descriptor-pcr063-customer";
  access.collation_identity = "collation.pcr063.binary";
  access.visibility_proven = true;
  access.grants_proven = true;
  access.base_row_mga_recheck_planned = true;
  access.base_row_security_recheck_planned = true;
  access.projected_column_uuids = {Id("column.customer_name")};
  access.table_stats = TableStats();
  access.candidate_indexes = {ExpressionIndex(canonical.digest)};
  const auto candidates = opt::GenerateFullAccessPathCandidates(access);
  const auto selected = std::find_if(candidates.begin(), candidates.end(),
                                    [](const auto& candidate) {
                                      return candidate.candidate_id.find("CAND-OPT-INDEX:") == 0 &&
                                             candidate.cost.selectable;
                                    });
  Require(selected != candidates.end(),
          "access-path planner should route expression index from SBLR metadata");
  Require(Contains(selected->acceptance_reasons,
                   "canonical_sblr_expression_digest_match"),
          "access-path evidence should include SBLR expression digest match");
  Require(Contains(selected->acceptance_reasons,
                   "parser_sql_expression_authority=false"),
          "access-path evidence should exclude parser SQL authority");

  auto unsafe = expression;
  unsafe.raw_sql_text_present = true;
  const auto unsafe_expression = opt::CanonicalizeSblrExpressionTree(unsafe);
  Require(!unsafe_expression.ok &&
              Contains(unsafe_expression.diagnostics,
                       "sblr_expression_parser_sql_authority_refused"),
          "raw SQL text cannot become expression matching authority");
  auto name_authority = expression;
  name_authority.name_authority_claimed = true;
  Require(!opt::CanonicalizeSblrExpressionTree(name_authority).ok,
          "durable expression identity cannot come from names");
}

void ProductionPlanCacheKeysRequireRealDigestsAndEpochs() {
  const auto build = opt::BuildProductionOptimizerPlanCacheKeyInput(
      ProductionCacheKeyRequest());
  Require(build.ok, "production plan-cache key should build with real digests");
  Require(build.diagnostic_code == "SB_OPTIMIZER_PLAN_CACHE_ENTERPRISE_OK",
          "production plan-cache key should pass enterprise validation");
  Require(Contains(build.evidence, "production_plan_cache_key_complete=true"),
          "production plan-cache builder should emit completion evidence");
  Require(build.input.parameter_shape_digest ==
              "sha256:parameter-shape-pcr063-name-eq",
          "production plan-cache key should bind caller parameter shape digest");
  Require(build.input.redaction_route_digest ==
              "sha256:redaction-route-pcr063-mask",
          "production plan-cache key should bind caller redaction route digest");
  Require(build.input.memory_grant_digest ==
              "sha256:memory-grant-pcr063-small",
          "production plan-cache key should bind caller memory grant digest");
  Require(build.input.compatibility_epoch == 6310 &&
              build.input.format_compatibility_epoch == 6311,
          "production plan-cache key should bind compatibility epochs");
  Require(build.input.object_uuids.size() == 1 &&
              build.input.function_uuids.size() == 1 &&
              build.input.index_uuids.size() == 1 &&
              build.input.filespace_uuids.size() == 1,
          "production plan-cache key should bind object function index and filespace dependencies");

  auto generic = opt::BuildOptimizerPlanCacheKeyInput(
      BoundRequest(),
      "cost-profile:pcr063-catalog-v1",
      {Id("relation.customer")},
      {Id("function.lower")},
      {Id("index.customer_name_lower")},
      {Id("filespace.hot")});
  const auto generic_validation =
      opt::ValidateEnterpriseOptimizerPlanCacheKeyInput(generic);
  Require(!generic_validation.ok,
          "compatibility cache builder should not satisfy enterprise production validation");
  Require(ContainsText(generic_validation.evidence,
                       "redaction_route_digest") ||
              ContainsText(generic_validation.evidence,
                           "parameter_shape_digest") ||
              ContainsText(generic_validation.evidence,
                           "memory_grant_digest"),
          "generic builder should fail on placeholder redaction parameter or memory fields");

  auto missing_parameter = ProductionCacheKeyRequest();
  missing_parameter.parameter_shape_digest = "parameters:unbound";
  Require(!opt::BuildProductionOptimizerPlanCacheKeyInput(missing_parameter).ok,
          "production builder must reject unbound parameter shape");

  auto parser_authority = ProductionCacheKeyRequest();
  parser_authority.parser_or_reference_authority_claimed = true;
  Require(opt::BuildProductionOptimizerPlanCacheKeyInput(parser_authority)
              .diagnostic_code ==
              "SB_OPTIMIZER_PLAN_CACHE_PRODUCTION_REQUEST_REFUSED",
          "production builder must reject parser or reference cache authority");

  auto cluster_route = ProductionCacheKeyRequest();
  cluster_route.cluster_route_requested = true;
  Require(opt::BuildProductionOptimizerPlanCacheKeyInput(cluster_route)
              .diagnostic_code ==
              "SB_OPTIMIZER_PLAN_CACHE_ROUTE_CAPABILITY_MISMATCH",
          "production builder must reject local cluster route cache keys");
}

void PlanCacheLookupAndInvalidationAreExact() {
  const auto build = opt::BuildProductionOptimizerPlanCacheKeyInput(
      ProductionCacheKeyRequest());
  Require(build.ok, "production cache key build failed before lookup test");
  opt::OptimizerPlanCache cache;
  const auto put_validation = cache.PutEnterprise(CachedPlan(build.input));
  Require(put_validation.ok, "enterprise cached plan should be admitted");

  const auto hit = cache.LookupEnterprise(build.input);
  Require(hit.hit &&
              hit.diagnostic_code == "SB_OPTIMIZER_PLAN_CACHE_HIT",
          "exact enterprise cache key should hit");
  Require(Contains(hit.evidence, "cached_plan_metadata_only=true"),
          "plan cache hit should prove metadata-only reuse");
  Require(Contains(hit.evidence, "mga_visibility_recheck=preserved"),
          "plan cache hit should preserve MGA recheck");

  auto parameter_changed = build.input;
  parameter_changed.parameter_shape_digest =
      "sha256:parameter-shape-pcr063-range";
  Require(cache.LookupEnterprise(parameter_changed).diagnostic_code ==
              "SB_OPTIMIZER_PLAN_CACHE_INCOMPATIBLE_PARAMETER_SHAPE",
          "changed parameter shape must refuse cache reuse");

  auto redaction_changed = build.input;
  redaction_changed.redaction_route_digest =
      "sha256:redaction-route-pcr063-alt-mask";
  Require(cache.LookupEnterprise(redaction_changed).diagnostic_code ==
              "SB_OPTIMIZER_PLAN_CACHE_REDACTION_SECURITY_POLICY_MISMATCH",
          "changed redaction route must refuse cache reuse");

  auto route_changed = build.input;
  route_changed.route_capability_digest =
      "sha256:route-capability-pcr063-local-covering";
  Require(cache.LookupEnterprise(route_changed).diagnostic_code ==
              "SB_OPTIMIZER_PLAN_CACHE_ROUTE_CAPABILITY_MISMATCH",
          "changed route capability must refuse cache reuse");

  auto dependency_changed = build.input;
  dependency_changed.dependency_digests.push_back(
      "sha256:dep-pcr063-new-index");
  Require(cache.LookupEnterprise(dependency_changed).diagnostic_code ==
              "SB_OPTIMIZER_PLAN_CACHE_DEPENDENCY_INVALIDATED",
          "changed dependency digest set must refuse cache reuse");

  const auto invalidation = cache.InvalidateWithEvidence(
      opt::OptimizerInvalidationEventForMutation(
          "index_alter", Id("index.customer_name_lower"), 6312));
  Require(invalidation.invalidated_count == 1,
          "index dependency invalidation should mark cached plan invalid");
  Require(invalidation.diagnostic_code ==
              "SB_OPTIMIZER_PLAN_CACHE_DEPENDENCY_INVALIDATED",
          "index invalidation should carry dependency diagnostic");
  const auto invalidated_lookup = cache.LookupEnterprise(build.input);
  Require(!invalidated_lookup.hit &&
              invalidated_lookup.diagnostic_code ==
                  "SB_OPTIMIZER_PLAN_CACHE_DEPENDENCY_INVALIDATED",
          "invalidated exact key should no longer hit");
  Require(Contains(invalidated_lookup.evidence,
                   "optimizer_plan_cache_dependency_invalidation"),
          "invalidated lookup should expose dependency evidence");
}

void BoundRequestValidationRejectsParserAndNameAuthority() {
  auto request = BoundRequest();
  request.context.parser_owned_claims_present = true;
  const auto parser_validation = opt::ValidateBoundOptimizerRequest(request);
  Require(!parser_validation.ok &&
              Contains(parser_validation.diagnostics,
                       "SB_OPT_AUTHORITY_REJECTED.parser_owned_claims"),
          "optimizer request boundary must reject parser-owned claims");

  request = BoundRequest();
  request.context.name_authority_present = true;
  const auto name_validation = opt::ValidateBoundOptimizerRequest(request);
  Require(!name_validation.ok &&
              Contains(name_validation.diagnostics,
                       "SB_OPT_AUTHORITY_REJECTED.name_authority"),
          "optimizer request boundary must reject names as authority");

  request = BoundRequest();
  request.logical_plan.optimizer_policy.raw_sql_text_present = true;
  const auto policy_validation = opt::ValidateBoundOptimizerRequest(request);
  Require(!policy_validation.ok &&
              Contains(policy_validation.diagnostics,
                       "SB_OPT_AUTHORITY_REJECTED.optimizer_policy_raw_sql_text"),
          "optimizer policy boundary must reject raw SQL text");
}

}  // namespace

int main() {
  CanonicalSblrExpressionMatchingIsProductionRoutable();
  ProductionPlanCacheKeysRequireRealDigestsAndEpochs();
  PlanCacheLookupAndInvalidationAreExact();
  BoundRequestValidationRejectsParserAndNameAuthority();
  return EXIT_SUCCESS;
}
