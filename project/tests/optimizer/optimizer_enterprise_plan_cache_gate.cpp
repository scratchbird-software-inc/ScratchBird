// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "optimizer_plan_cache.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace opt = scratchbird::engine::optimizer;
namespace planner = scratchbird::engine::planner;

namespace {

bool Require(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "OEIC-051 gate failure: " << message << '\n';
    return false;
  }
  return true;
}

bool Contains(const std::vector<std::string>& values, const std::string& expected) {
  return std::find(values.begin(), values.end(), expected) != values.end();
}

opt::OptimizerPlanCacheKeyInput EnterpriseInput() {
  opt::OptimizerPlanCacheKeyInput input;
  input.operation_id = "oeic051.select.customer";
  input.sblr_digest = "sha256:sblr-oeic051-customer-point";
  input.descriptor_set_digest = "sha256:descriptor-customer-v51";
  input.statistics_snapshot_id = "stats-snapshot:customer:v51";
  input.catalog_stats_digest = "sha256:catalog-stats-customer-v51";
  input.cost_profile_id = "cost-profile:enterprise-oltp-v51";
  input.executor_capability_set_id = "executor-capability:local-mga-v51";
  input.route_capability_digest = "sha256:route-capability-local-index-v51";
  input.security_policy_digest = "sha256:security-policy-tenant-reader-v51";
  input.redaction_route_digest = "sha256:redaction-route-mask-email-v51";
  input.normalized_optimizer_controls_digest =
      "sha256:controls-bounded-dp-property-frontier-v51";
  input.parameter_shape_digest = "sha256:bind-profile-slot0-int64-not-null-v51";
  input.memory_grant_class = "memory-grant-class:query-small-governed";
  input.memory_grant_digest = "sha256:memory-grant-64k-feedback-v51";
  input.catalog_epoch = 5101;
  input.stats_epoch = 5102;
  input.security_epoch = 5103;
  input.redaction_epoch = 5104;
  input.policy_epoch = 5105;
  input.resource_epoch = 5106;
  input.name_resolution_epoch = 5107;
  input.memory_policy_epoch = 5108;
  input.memory_feedback_generation = 5109;
  input.compatibility_epoch = 5110;
  input.format_compatibility_epoch = 5111;
  input.route_epoch = 5112;
  input.object_uuids = {"rel.customer.051"};
  input.function_uuids = {"fn.mask_email.051"};
  input.index_uuids = {"idx.customer.pk.051"};
  input.filespace_uuids = {"filespace.hot.051"};
  input.dependency_digests = {
      "sha256:dep-rel-customer-v51",
      "sha256:dep-idx-customer-pk-v51",
      "sha256:dep-fn-mask-email-v51",
      "sha256:dep-stats-customer-v51",
      "sha256:dep-route-local-index-v51",
      "sha256:dep-memory-feedback-v51"};
  return input;
}

opt::CachedOptimizerPlan EnterprisePlan(const opt::OptimizerPlanCacheKeyInput& input) {
  opt::CachedOptimizerPlan plan;
  plan.key_input = input;
  plan.cache_key = opt::BuildOptimizerPlanCacheKey(input);
  plan.created_epoch = input.catalog_epoch;
  plan.result.ok = true;
  plan.result.diagnostic_code = "SB_OPT_OK";
  plan.result.plan_id = "oeic051.enterprise_plan";
  opt::PlanCandidate candidate;
  candidate.candidate_id = "local_btree.customer_pk.051";
  candidate.access_kind = planner::PhysicalAccessKind::kScalarBtreeLookup;
  candidate.required_facts = {
      "rel.customer.051",
      "idx.customer.pk.051",
      "sha256:dep-route-local-index-v51"};
  plan.result.candidates.push_back(candidate);
  plan.metadata_only = true;
  plan.mga_visibility_recheck_required = true;
  plan.security_recheck_required = true;
  plan.parser_or_donor_finality_authority = false;
  return plan;
}

opt::OptimizerPlanCachePersistenceRequest PersistenceRequest() {
  opt::OptimizerPlanCachePersistenceRequest request;
  request.storage_scope_uuid = "catalog.plan_cache.scope.051";
  request.persisted_by_principal_uuid = "principal.optimizer.runtime.051";
  request.persisted_epoch = 5120;
  request.catalog_epoch = 5101;
  request.stats_epoch = 5102;
  request.security_epoch = 5103;
  request.redaction_epoch = 5104;
  request.policy_epoch = 5105;
  request.resource_epoch = 5106;
  request.route_epoch = 5112;
  request.memory_policy_epoch = 5108;
  request.memory_feedback_generation = 5109;
  request.durable_catalog_persistence = true;
  request.mga_transaction_committed = true;
  request.security_redaction_evidence_present = true;
  request.fixture_or_test_only = false;
  request.cluster_route_projection_present = false;
  return request;
}

bool EnterpriseKeyRequiresEveryReuseDimension() {
  const auto input = EnterpriseInput();
  const auto validation = opt::ValidateEnterpriseOptimizerPlanCacheKeyInput(input);
  if (!Require(validation.ok, "complete enterprise key was rejected")) return false;
  if (!Require(Contains(validation.evidence, "enterprise_plan_cache_bind_profile_digest_present=true"),
               "bind-profile evidence missing")) {
    return false;
  }
  const auto key = opt::BuildOptimizerPlanCacheKey(input);
  if (!Require(key.find("optimizer_controls=") != std::string::npos,
               "normalized controls missing from key")) return false;
  if (!Require(key.find("memory_feedback_generation=5109") != std::string::npos,
               "memory feedback generation missing from key")) return false;
  if (!Require(key.find("route_cap=sha256:route-capability-local-index-v51") !=
                   std::string::npos,
               "route capability digest missing from key")) {
    return false;
  }

  auto missing_bind = input;
  missing_bind.parameter_shape_digest = "parameters:unbound";
  if (!Require(!opt::ValidateEnterpriseOptimizerPlanCacheKeyInput(missing_bind).ok,
               "unbound bind profile was accepted")) return false;

  auto missing_memory = input;
  missing_memory.memory_feedback_generation = 0;
  if (!Require(!opt::ValidateEnterpriseOptimizerPlanCacheKeyInput(missing_memory).ok,
               "missing memory feedback generation was accepted")) return false;

  auto cluster_route = input;
  cluster_route.route_capability_digest = "sha256:cluster-route-projection";
  const auto cluster_validation =
      opt::ValidateEnterpriseOptimizerPlanCacheKeyInput(cluster_route);
  return Require(!cluster_validation.ok, "cluster route projection was accepted") &&
         Require(Contains(cluster_validation.evidence,
                          "enterprise_plan_cache_cluster_route_refused:route_capability_digest"),
                 "cluster route refusal evidence missing");
}

bool EnterpriseLookupReusesOnlySafePlans() {
  opt::OptimizerPlanCache cache;
  const auto input = EnterpriseInput();
  const auto put = cache.PutEnterprise(EnterprisePlan(input));
  if (!Require(put.ok, "enterprise cache put rejected a complete plan")) return false;

  const auto hit = cache.LookupEnterprise(input);
  if (!Require(hit.hit, "enterprise lookup did not reuse identical key")) return false;
  if (!Require(Contains(hit.evidence, "OEIC_PLAN_CACHE_ENTERPRISE_CLOSURE"),
               "enterprise lookup evidence missing")) return false;

  auto changed_redaction = input;
  ++changed_redaction.redaction_epoch;
  const auto redaction_miss = cache.LookupEnterprise(changed_redaction);
  if (!Require(!redaction_miss.hit, "redaction epoch change reused plan")) return false;
  if (!Require(redaction_miss.diagnostic_code ==
                   "SB_OPTIMIZER_PLAN_CACHE_REDACTION_SECURITY_POLICY_MISMATCH",
               "redaction diagnostic changed")) return false;

  auto changed_memory = input;
  ++changed_memory.memory_feedback_generation;
  const auto memory_miss = cache.LookupEnterprise(changed_memory);
  if (!Require(!memory_miss.hit, "memory feedback change reused plan")) return false;
  if (!Require(memory_miss.diagnostic_code ==
                   "SB_OPTIMIZER_PLAN_CACHE_MEMORY_GRANT_MISMATCH",
               "memory feedback diagnostic changed")) return false;

  opt::OptimizerPlanCache unsafe_cache;
  auto unsafe_plan = EnterprisePlan(input);
  unsafe_plan.security_recheck_required = false;
  const auto unsafe_put = unsafe_cache.PutEnterprise(unsafe_plan);
  return Require(!unsafe_put.ok, "enterprise cache accepted plan without security recheck") &&
         Require(unsafe_put.diagnostic_code == "SB_OPTIMIZER_PLAN_CACHE_AUTHORITY_UNSAFE",
                 "unsafe plan diagnostic changed");
}

bool PersistenceEnvelopeRoundTripsAndInvalidates() {
  opt::OptimizerPlanCache cache;
  const auto input = EnterpriseInput();
  if (!Require(cache.PutEnterprise(EnterprisePlan(input)).ok,
               "enterprise plan setup failed")) return false;

  const auto envelope = cache.ExportPersistenceEnvelope(PersistenceRequest());
  if (!Require(envelope.ok, "enterprise persistence envelope was rejected")) return false;
  if (!Require(envelope.envelope_digest.find("sha256:") == 0,
               "enterprise persistence digest did not use sha256")) return false;
  if (!Require(Contains(envelope.evidence, "OEIC_PLAN_CACHE_ENTERPRISE_CLOSURE"),
               "enterprise persistence evidence missing")) return false;

  opt::OptimizerPlanCache recovered;
  const auto imported = recovered.ImportPersistenceEnvelope(envelope);
  if (!Require(imported.ok, "enterprise persistence import rejected envelope")) return false;

  const auto hit = recovered.LookupEnterprise(input);
  if (!Require(hit.hit, "restored enterprise plan was not reusable")) return false;

  const auto invalidation = recovered.InvalidateWithEvidence(
      opt::OptimizerInvalidationEventForMutation("memory_feedback_publication", "", 5130));
  if (!Require(invalidation.invalidated_count == 1,
               "memory feedback publication did not invalidate restored plan")) return false;
  const auto stale = recovered.LookupEnterprise(input);
  if (!Require(!stale.hit, "invalidated restored plan was reused")) return false;

  auto tampered = envelope;
  tampered.plans[0].result.plan_id = "tampered";
  const auto tampered_import = recovered.ImportPersistenceEnvelope(tampered);
  return Require(!tampered_import.ok, "tampered persistence envelope was accepted") &&
         Require(tampered_import.diagnostic_code ==
                     "SB_OPTIMIZER_PLAN_CACHE_ENTERPRISE_PERSISTENCE_DIGEST_MISMATCH",
                 "tamper digest diagnostic changed");
}

bool PersistenceRefusesNonProductionAuthority() {
  opt::OptimizerPlanCache cache;
  const auto input = EnterpriseInput();
  if (!Require(cache.PutEnterprise(EnterprisePlan(input)).ok,
               "enterprise plan setup failed")) return false;

  auto request = PersistenceRequest();
  request.fixture_or_test_only = true;
  request.cluster_route_projection_present = true;
  request.mga_transaction_committed = false;
  const auto envelope = cache.ExportPersistenceEnvelope(request);
  return Require(!envelope.ok, "unsafe persistence envelope was accepted") &&
         Require(envelope.diagnostic_code ==
                     "SB_OPTIMIZER_PLAN_CACHE_ENTERPRISE_PERSISTENCE_UNSAFE",
                 "unsafe persistence diagnostic changed");
}

}  // namespace

int main() {
  // SEARCH_KEY: OEIC_PLAN_CACHE_ENTERPRISE_CLOSURE
  if (!EnterpriseKeyRequiresEveryReuseDimension()) return EXIT_FAILURE;
  if (!EnterpriseLookupReusesOnlySafePlans()) return EXIT_FAILURE;
  if (!PersistenceEnvelopeRoundTripsAndInvalidates()) return EXIT_FAILURE;
  if (!PersistenceRefusesNonProductionAuthority()) return EXIT_FAILURE;
  return EXIT_SUCCESS;
}
