// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "optimizer_plan_cache.hpp"

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

opt::OptimizerPlanCacheKeyInput BaseInput() {
  opt::OptimizerPlanCacheKeyInput input;
  input.operation_id = "odf024.select";
  input.sblr_digest = "sblr:uuid-bound:odf024";
  input.descriptor_set_digest = "descriptor:rel.customer:v1";
  input.statistics_snapshot_id = "stats:snapshot:42";
  input.catalog_stats_digest = "catalog_stats:rel.customer:42";
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
  input.object_uuids = {"rel.customer"};
  input.function_uuids = {"fn.mask_email"};
  input.index_uuids = {"idx.customer_id"};
  input.filespace_uuids = {"filespace.hot"};
  return input;
}

opt::CachedOptimizerPlan CachedPlan(const opt::OptimizerPlanCacheKeyInput& input) {
  opt::CachedOptimizerPlan plan;
  plan.key_input = input;
  plan.cache_key = opt::BuildOptimizerPlanCacheKey(input);
  plan.created_epoch = input.catalog_epoch;
  plan.result.ok = true;
  plan.result.plan_id = "odf024.index_lookup";
  plan.result.diagnostic_code = "ok";
  return plan;
}

bool KeyIncludesRequiredDimensionsAndParameterSensitivity() {
  const auto input = BaseInput();
  const std::string key = opt::BuildOptimizerPlanCacheKey(input);
  return Require(key.find("catalog_stats=") != std::string::npos, "catalog stats digest missing from key") &&
         Require(key.find("security_policy=") != std::string::npos, "security policy digest missing from key") &&
         Require(key.find("redaction_route=") != std::string::npos, "redaction route digest missing from key") &&
         Require(key.find("param_shape=") != std::string::npos, "parameter shape digest missing from key") &&
         Require(key.find("memory_grant_class=") != std::string::npos, "memory grant class missing from key") &&
         Require(key.find("memory_grant=") != std::string::npos, "memory grant digest missing from key") &&
         Require(key.find("compatibility_epoch=") != std::string::npos, "compatibility epoch missing from key") &&
         Require(key.find("format_compatibility_epoch=") != std::string::npos,
                 "format compatibility epoch missing from key") &&
         Require(key.find("route_epoch=") != std::string::npos, "route epoch missing from key");
}

bool ParameterAndMemoryShapesDoNotReuseKeys() {
  const auto base = BaseInput();
  const std::string base_key = opt::BuildOptimizerPlanCacheKey(base);

  auto type_changed = base;
  type_changed.parameter_shape_digest = "slot0:text:not_null:card=point:range=42";
  auto nullability_changed = base;
  nullability_changed.parameter_shape_digest = "slot0:int64:nullable:card=point:range=42";
  auto cardinality_changed = base;
  cardinality_changed.parameter_shape_digest = "slot0:int64:not_null:card=list:range=42";
  auto range_changed = base;
  range_changed.parameter_shape_digest = "slot0:int64:not_null:card=point:range=1..1000";
  auto memory_changed = base;
  memory_changed.memory_grant_class = "memory:large";
  memory_changed.memory_grant_digest = "grant:large:64m";

  return Require(opt::BuildOptimizerPlanCacheKey(type_changed) != base_key,
                 "parameter type shape reused the same key") &&
         Require(opt::BuildOptimizerPlanCacheKey(nullability_changed) != base_key,
                 "parameter nullability shape reused the same key") &&
         Require(opt::BuildOptimizerPlanCacheKey(cardinality_changed) != base_key,
                 "parameter cardinality shape reused the same key") &&
         Require(opt::BuildOptimizerPlanCacheKey(range_changed) != base_key,
                 "parameter range shape reused the same key") &&
         Require(opt::BuildOptimizerPlanCacheKey(memory_changed) != base_key,
                 "memory grant class reused the same key");
}

bool LookupDiagnosticsAreExact() {
  opt::OptimizerPlanCache cache;
  const auto base = BaseInput();
  cache.Put(CachedPlan(base));

  const auto hit = cache.Lookup(base);
  if (!Require(hit.hit, "exact key did not hit") ||
      !Require(hit.diagnostic_code == "SB_OPTIMIZER_PLAN_CACHE_HIT",
               "hit diagnostic mismatch: " + hit.diagnostic_code) ||
      !Require(Has(hit.evidence, "cached_plan_metadata_only=true"),
               "hit evidence did not prove metadata-only cache") ||
      !Require(Has(hit.evidence, "mga_visibility_recheck=preserved"),
               "hit evidence did not preserve MGA visibility recheck") ||
      !Require(Has(hit.evidence, "security_authorization_recheck=preserved"),
               "hit evidence did not preserve security recheck") ||
      !Require(Has(hit.evidence, "mga_finality_authority=engine_transaction_inventory"),
               "hit evidence did not preserve engine MGA finality authority")) {
    return false;
  }

  auto changed = base;
  changed.operation_id = "odf024.other";
  auto lookup = cache.Lookup(changed);
  if (!Require(!lookup.hit && lookup.diagnostic_code == "SB_OPTIMIZER_PLAN_CACHE_MISS",
               "plain miss diagnostic mismatch: " + lookup.diagnostic_code)) return false;

  changed = base;
  changed.catalog_epoch += 1;
  lookup = cache.Lookup(changed);
  if (!Require(!lookup.hit && lookup.diagnostic_code == "SB_OPTIMIZER_PLAN_CACHE_STALE_EPOCH",
               "stale epoch diagnostic mismatch: " + lookup.diagnostic_code)) return false;

  changed = base;
  changed.parameter_shape_digest = "slot0:int64:not_null:card=range:range=1..10";
  lookup = cache.Lookup(changed);
  if (!Require(!lookup.hit &&
                   lookup.diagnostic_code == "SB_OPTIMIZER_PLAN_CACHE_INCOMPATIBLE_PARAMETER_SHAPE",
               "parameter shape diagnostic mismatch: " + lookup.diagnostic_code)) return false;

  changed = base;
  changed.memory_grant_class = "memory:large";
  changed.memory_grant_digest = "grant:large:64m";
  lookup = cache.Lookup(changed);
  if (!Require(!lookup.hit &&
                   lookup.diagnostic_code == "SB_OPTIMIZER_PLAN_CACHE_MEMORY_GRANT_MISMATCH",
               "memory grant diagnostic mismatch: " + lookup.diagnostic_code)) return false;

  changed = base;
  changed.redaction_route_digest = "redaction:none";
  lookup = cache.Lookup(changed);
  if (!Require(!lookup.hit &&
                   lookup.diagnostic_code ==
                       "SB_OPTIMIZER_PLAN_CACHE_REDACTION_SECURITY_POLICY_MISMATCH",
               "security/redaction diagnostic mismatch: " + lookup.diagnostic_code)) return false;

  changed = base;
  changed.route_capability_digest = "route:remote-pushdown:v1";
  lookup = cache.Lookup(changed);
  if (!Require(!lookup.hit &&
                   lookup.diagnostic_code == "SB_OPTIMIZER_PLAN_CACHE_ROUTE_CAPABILITY_MISMATCH",
               "route/capability diagnostic mismatch: " + lookup.diagnostic_code)) return false;

  return true;
}

bool UnsafeCachedAuthorityNeverReusesPlan() {
  const auto base = BaseInput();
  std::vector<opt::CachedOptimizerPlan> unsafe_plans;
  {
    auto plan = CachedPlan(base);
    plan.metadata_only = false;
    unsafe_plans.push_back(plan);
  }
  {
    auto plan = CachedPlan(base);
    plan.mga_visibility_recheck_required = false;
    unsafe_plans.push_back(plan);
  }
  {
    auto plan = CachedPlan(base);
    plan.security_recheck_required = false;
    unsafe_plans.push_back(plan);
  }
  {
    auto plan = CachedPlan(base);
    plan.parser_or_donor_finality_authority = true;
    unsafe_plans.push_back(plan);
  }

  for (auto& plan : unsafe_plans) {
    opt::OptimizerPlanCache cache;
    cache.Put(plan);
    const auto lookup = cache.Lookup(base);
    if (!Require(!lookup.hit, "unsafe cached authority unexpectedly hit") ||
        !Require(lookup.diagnostic_code == "SB_OPTIMIZER_PLAN_CACHE_AUTHORITY_UNSAFE",
                 "unsafe authority diagnostic mismatch: " + lookup.diagnostic_code) ||
        !Require(Has(lookup.evidence, "optimizer_plan_cache_authority_unsafe"),
                 "unsafe authority evidence missing")) {
      return false;
    }
  }
  return true;
}

bool InvalidatesWithDiagnostic(const opt::OptimizerInvalidationEvent& event,
                               const std::string& expected_diagnostic) {
  opt::OptimizerPlanCache cache;
  const auto base = BaseInput();
  cache.Put(CachedPlan(base));
  const auto result = cache.InvalidateWithEvidence(event);
  const auto lookup = cache.Lookup(base);
  return Require(result.invalidated_count == 1, "event did not invalidate exactly one plan: " + event.event_kind) &&
         Require(result.diagnostic_code == expected_diagnostic,
                 "invalidation diagnostic mismatch for " + event.event_kind + ": " + result.diagnostic_code) &&
         Require(!lookup.hit, "invalidated plan still hit for " + event.event_kind) &&
         Require(lookup.diagnostic_code == expected_diagnostic,
                 "post-invalidation lookup diagnostic mismatch for " + event.event_kind + ": " +
                     lookup.diagnostic_code);
}

bool DeterministicInvalidationCoversRequiredSurfaces() {
  const std::vector<std::pair<opt::OptimizerInvalidationEvent, std::string>> events = {
      {{"catalog_epoch", {}, 200}, "SB_OPTIMIZER_PLAN_CACHE_STALE_EPOCH"},
      {{"stats_stale", {}, 200}, "SB_OPTIMIZER_PLAN_CACHE_STALE_EPOCH"},
      {{"security_policy_change", {}, 200},
       "SB_OPTIMIZER_PLAN_CACHE_REDACTION_SECURITY_POLICY_MISMATCH"},
      {{"policy_change", {}, 200}, "SB_OPTIMIZER_PLAN_CACHE_REDACTION_SECURITY_POLICY_MISMATCH"},
      {{"redaction_route_change", {}, 200},
       "SB_OPTIMIZER_PLAN_CACHE_REDACTION_SECURITY_POLICY_MISMATCH"},
      {{"route_capability_change", {}, 200}, "SB_OPTIMIZER_PLAN_CACHE_ROUTE_CAPABILITY_MISMATCH"},
      {{"executor_capability_change", {}, 200}, "SB_OPTIMIZER_PLAN_CACHE_ROUTE_CAPABILITY_MISMATCH"},
      {{"memory_grant_policy_change", {}, 200}, "SB_OPTIMIZER_PLAN_CACHE_MEMORY_GRANT_MISMATCH"},
      {{"compatibility_epoch", {}, 200}, "SB_OPTIMIZER_PLAN_CACHE_STALE_EPOCH"},
      {{"format_change", {}, 200}, "SB_OPTIMIZER_PLAN_CACHE_STALE_EPOCH"},
      {{"catalog_alter", "rel.customer", 200}, "SB_OPTIMIZER_PLAN_CACHE_DEPENDENCY_INVALIDATED"},
      {{"index_change", "idx.customer_id", 200}, "SB_OPTIMIZER_PLAN_CACHE_DEPENDENCY_INVALIDATED"},
      {{"function_change", "fn.mask_email", 200}, "SB_OPTIMIZER_PLAN_CACHE_DEPENDENCY_INVALIDATED"},
      {{"filespace_profile_change", "filespace.hot", 200}, "SB_OPTIMIZER_PLAN_CACHE_DEPENDENCY_INVALIDATED"},
      {{"unrecognized_odf024_event", {}, 200}, "SB_OPTIMIZER_PLAN_CACHE_UNKNOWN_INVALIDATION_KIND"},
  };

  for (const auto& [event, diagnostic] : events) {
    if (!InvalidatesWithDiagnostic(event, diagnostic)) return false;
  }

  opt::OptimizerPlanCache cache;
  const auto base = BaseInput();
  cache.Put(CachedPlan(base));
  const auto unrelated = cache.InvalidateWithEvidence({"index_change", "idx.unrelated", 201});
  const auto lookup = cache.Lookup(base);
  return Require(unrelated.invalidated_count == 0, "unrelated dependency invalidated the plan") &&
         Require(lookup.hit, "unrelated dependency prevented cache hit");
}

}  // namespace

int main() {
  if (!KeyIncludesRequiredDimensionsAndParameterSensitivity()) return 1;
  if (!ParameterAndMemoryShapesDoNotReuseKeys()) return 1;
  if (!LookupDiagnosticsAreExact()) return 1;
  if (!UnsafeCachedAuthorityNeverReusesPlan()) return 1;
  if (!DeterministicInvalidationCoversRequiredSurfaces()) return 1;
  return 0;
}
