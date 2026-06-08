// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "logical_plan.hpp"
#include "optimizer_plan_cache.hpp"
#include "optimizer_request.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace opt = scratchbird::engine::optimizer;
namespace plan = scratchbird::engine::planner;

namespace {

bool Require(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "OPCH-030/031 gate failure: " << message << '\n';
    return false;
  }
  return true;
}

bool Contains(const std::vector<std::string>& values, const std::string& expected) {
  return std::find(values.begin(), values.end(), expected) != values.end();
}

plan::OptimizerPolicyMetadata SafePolicy() {
  plan::OptimizerPolicyMetadata policy;
  policy.optimizer_policy_metadata_present = true;
  policy.policy_source_kind = "sblr_api";
  policy.policy_epoch = 5030;
  policy.normalized_controls.plan_profile_id = "plan_profile:oltp_point_lookup";
  policy.normalized_controls.join_search_policy_id = "join_search:bounded_dp";
  policy.normalized_controls.memory_policy_id = "memory_policy:governed_small";
  policy.normalized_controls.spill_policy_id = "spill_policy:bounded_local";
  policy.normalized_controls.parallelism_policy_id = "parallelism:intra_node_2";
  policy.normalized_controls.what_if_policy_id = "what_if:index_candidate_safe";
  policy.normalized_controls.safe_control_ids = {
      "session_directive:optimizer_profile_oltp",
      "join_frontier:retain_property",
      "memory_feedback:trusted_generation"};
  policy.safe_control_ids = {
      "spill_budget:governed",
      "parallelism:max_degree_2"};
  return policy;
}

plan::LogicalPlan SafeLogicalPlan() {
  auto logical = plan::BuildQueryShapePlan({plan::QueryShapeKind::kPointLookup});
  logical.optimizer_policy = SafePolicy();
  return logical;
}

opt::BoundOptimizerRequest SafeRequest() {
  opt::BoundOptimizerRequest request;
  request.context.request_uuid = "opch030.request";
  request.context.operation_id = "opch030.select.customer";
  request.context.sblr_digest = "sblr:opch030:uuid-bound";
  request.context.descriptor_set_digest = "descriptor:customer:v030";
  request.context.statistics_snapshot_id = "stats:customer:v031";
  request.context.metric_snapshot_id = "metrics:customer:v031";
  request.context.executor_capability_set_id = "executor:local:mga:v031";
  request.context.catalog_epoch = 730;
  request.context.stats_epoch = 731;
  request.context.security_epoch = 732;
  request.context.redaction_epoch = 733;
  request.context.policy_epoch = 5030;
  request.context.resource_epoch = 734;
  request.context.name_resolution_epoch = 735;
  request.context.memory_policy_epoch = 736;
  request.context.memory_feedback_generation = 737;
  request.context.route_epoch = 738;
  request.context.security_context_present = true;
  request.context.transaction_context_present = true;
  request.logical_plan = SafeLogicalPlan();
  return request;
}

opt::OptimizerPlanCacheKeyInput BaseInput() {
  auto input = opt::BuildOptimizerPlanCacheKeyInput(SafeRequest(),
                                                   "cost:commercial:v031",
                                                   {"rel.customer"},
                                                   {"fn.mask_email"},
                                                   {"idx.customer_pk"},
                                                   {"filespace.hot"});
  input.catalog_stats_digest = "catalog_stats:customer:v031";
  input.route_capability_digest = "route:local:index:v031";
  input.security_policy_digest = "security:tenant_reader:v031";
  input.redaction_route_digest = "redaction:mask_email:v031";
  input.parameter_shape_digest = "slot0:int64:not_null:point";
  input.memory_grant_class = "memory:small";
  input.memory_grant_digest = "grant:small:64k:v031";
  input.dependency_digests = {
      "dep:rel.customer:v031",
      "dep:idx.customer_pk:v031",
      "dep:fn.mask_email:v031",
      "dep:redaction.mask_email:v031",
      "dep:stats.customer:v031"};
  return input;
}

opt::CachedOptimizerPlan CachedPlan(const opt::OptimizerPlanCacheKeyInput& input) {
  opt::CachedOptimizerPlan plan;
  plan.key_input = input;
  plan.cache_key = opt::BuildOptimizerPlanCacheKey(input);
  plan.created_epoch = input.catalog_epoch;
  plan.result.ok = true;
  plan.result.plan_id = "opch031.customer_lookup";
  plan.result.diagnostic_code = "SB_OPT_OK";
  plan.metadata_only = true;
  plan.mga_visibility_recheck_required = true;
  plan.security_recheck_required = true;
  plan.parser_or_donor_finality_authority = false;
  return plan;
}

bool NormalizedControlsAreStableAndParserSafe() {
  auto request = SafeRequest();
  const auto validation = opt::ValidateBoundOptimizerRequest(request);
  if (!Require(validation.ok, "safe normalized optimizer controls were rejected")) return false;

  auto reordered = request;
  std::reverse(reordered.logical_plan.optimizer_policy.normalized_controls.safe_control_ids.begin(),
               reordered.logical_plan.optimizer_policy.normalized_controls.safe_control_ids.end());
  std::reverse(reordered.logical_plan.optimizer_policy.safe_control_ids.begin(),
               reordered.logical_plan.optimizer_policy.safe_control_ids.end());

  const auto left = opt::BuildOptimizerPlanCacheKeyInput(request, "cost:commercial:v031");
  const auto right = opt::BuildOptimizerPlanCacheKeyInput(reordered, "cost:commercial:v031");
  const auto digest = opt::BuildNormalizedOptimizerPolicyControlDigest(
      request.logical_plan.optimizer_policy);

  if (!Require(left.normalized_optimizer_controls_digest ==
                   right.normalized_optimizer_controls_digest,
               "normalized optimizer control digest changed with control order")) {
    return false;
  }
  if (!Require(digest.find("SELECT") == std::string::npos,
               "normalized optimizer control digest exposed raw SQL text")) {
    return false;
  }
  const auto key = opt::BuildOptimizerPlanCacheKey(left);
  return Require(key.find("optimizer_controls=") != std::string::npos,
                 "plan cache key omitted normalized optimizer controls") &&
         Require(key.find("memory_feedback_generation=737") != std::string::npos,
                 "plan cache key omitted memory feedback generation") &&
         Require(key.find("redaction_epoch=733") != std::string::npos,
                 "plan cache key omitted redaction epoch");
}

bool UnsafeControlsAreRejected() {
  auto request = SafeRequest();
  request.logical_plan.optimizer_policy.raw_sql_text_present = true;
  auto validation = opt::ValidateBoundOptimizerRequest(request);
  if (!Require(!validation.ok, "raw SQL optimizer authority was accepted")) return false;
  if (!Require(Contains(validation.diagnostics,
                       "SB_OPT_AUTHORITY_REJECTED.optimizer_policy_raw_sql_text"),
               "raw SQL optimizer authority rejection diagnostic missing")) {
    return false;
  }

  request = SafeRequest();
  request.logical_plan.optimizer_policy.normalized_controls.safe_control_ids.push_back(
      "SELECT * FROM customer");
  validation = opt::ValidateBoundOptimizerRequest(request);
  return Require(!validation.ok, "unsafe control token was accepted") &&
         Require(Contains(validation.diagnostics,
                          "SB_OPT_AUTHORITY_REJECTED.optimizer_policy_unsafe_control_token"),
                 "unsafe control token rejection diagnostic missing");
}

bool LookupRefusesWith(opt::OptimizerPlanCacheKeyInput mutated,
                       const std::string& expected_diagnostic,
                       const std::string& message) {
  opt::OptimizerPlanCache cache;
  cache.Put(CachedPlan(BaseInput()));
  const auto lookup = cache.Lookup(mutated);
  return Require(!lookup.hit, message + " unexpectedly hit") &&
         Require(lookup.diagnostic_code == expected_diagnostic,
                 message + " diagnostic changed: " + lookup.diagnostic_code);
}

bool ReusesOnlyIdenticalSafeSignature() {
  opt::OptimizerPlanCache cache;
  const auto input = BaseInput();
  cache.Put(CachedPlan(input));
  const auto lookup = cache.Lookup(input);

  return Require(lookup.hit, "identical safe plan signature did not reuse cache") &&
         Require(lookup.diagnostic_code == "SB_OPTIMIZER_PLAN_CACHE_HIT",
                 "cache hit diagnostic changed") &&
         Require(Contains(lookup.evidence,
                          "mga_finality_authority=engine_transaction_inventory"),
                 "cache hit did not preserve MGA authority evidence");
}

bool RefusesChangedSignatureDimensions() {
  auto catalog = BaseInput();
  ++catalog.catalog_epoch;
  if (!LookupRefusesWith(catalog,
                         "SB_OPTIMIZER_PLAN_CACHE_STALE_EPOCH",
                         "catalog epoch change")) return false;

  auto stats = BaseInput();
  stats.statistics_snapshot_id = "stats:customer:v032";
  if (!LookupRefusesWith(stats,
                         "SB_OPTIMIZER_PLAN_CACHE_STALE_EPOCH",
                         "statistics snapshot change")) return false;

  auto security = BaseInput();
  ++security.redaction_epoch;
  if (!LookupRefusesWith(security,
                         "SB_OPTIMIZER_PLAN_CACHE_REDACTION_SECURITY_POLICY_MISMATCH",
                         "security/redaction epoch change")) return false;

  auto memory = BaseInput();
  ++memory.memory_feedback_generation;
  if (!LookupRefusesWith(memory,
                         "SB_OPTIMIZER_PLAN_CACHE_MEMORY_GRANT_MISMATCH",
                         "memory feedback generation change")) return false;

  auto controls = BaseInput();
  controls.normalized_optimizer_controls_digest = "controls:changed";
  if (!LookupRefusesWith(controls,
                         "SB_OPTIMIZER_PLAN_CACHE_OPTIMIZER_CONTROL_POLICY_MISMATCH",
                         "normalized optimizer control change")) return false;

  auto dependency = BaseInput();
  dependency.dependency_digests.push_back("dep:idx.customer_covering:v001");
  return LookupRefusesWith(dependency,
                           "SB_OPTIMIZER_PLAN_CACHE_DEPENDENCY_INVALIDATED",
                           "dependency digest change");
}

bool InvalidationEventsCarryDeterministicEvidence() {
  opt::OptimizerPlanCache cache;
  const auto input = BaseInput();
  cache.Put(CachedPlan(input));

  const auto control_event = opt::OptimizerInvalidationEventForMutation(
      "optimizer_control_policy_mutation", std::string{}, 800);
  const auto invalidation = cache.InvalidateWithEvidence(control_event);
  if (!Require(invalidation.invalidated_count == 1,
               "optimizer control policy invalidation did not invalidate plan")) {
    return false;
  }
  if (!Require(invalidation.diagnostic_code ==
                   "SB_OPTIMIZER_PLAN_CACHE_OPTIMIZER_CONTROL_POLICY_MISMATCH",
               "optimizer control invalidation diagnostic changed")) {
    return false;
  }

  const auto lookup = cache.Lookup(input);
  return Require(!lookup.hit, "invalidated exact key was reused") &&
         Require(lookup.diagnostic_code ==
                     "SB_OPTIMIZER_PLAN_CACHE_OPTIMIZER_CONTROL_POLICY_MISMATCH",
                 "invalidated exact key diagnostic changed");
}

bool UnsafeCacheAuthorityFailsClosed() {
  opt::OptimizerPlanCache cache;
  auto plan = CachedPlan(BaseInput());
  plan.parser_or_donor_finality_authority = true;
  cache.Put(plan);
  const auto lookup = cache.Lookup(BaseInput());
  return Require(!lookup.hit, "cache reused parser/donor finality authority") &&
         Require(lookup.diagnostic_code == "SB_OPTIMIZER_PLAN_CACHE_AUTHORITY_UNSAFE",
                 "unsafe cache authority diagnostic changed");
}

}  // namespace

int main() {
  // SEARCH_KEY: OPCH_OPTIMIZER_CONTROL_PROFILE_SURFACE
  if (!NormalizedControlsAreStableAndParserSafe()) return EXIT_FAILURE;
  if (!UnsafeControlsAreRejected()) return EXIT_FAILURE;

  // SEARCH_KEY: OPCH_PLAN_SIGNATURE_CACHE_REUSE_INVALIDATION
  if (!ReusesOnlyIdenticalSafeSignature()) return EXIT_FAILURE;
  if (!RefusesChangedSignatureDimensions()) return EXIT_FAILURE;
  if (!InvalidationEventsCarryDeterministicEvidence()) return EXIT_FAILURE;
  if (!UnsafeCacheAuthorityFailsClosed()) return EXIT_FAILURE;
  return EXIT_SUCCESS;
}
