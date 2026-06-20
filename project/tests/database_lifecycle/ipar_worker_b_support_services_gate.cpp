// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "ipar_catalog_support_services.hpp"
#include "ipar_index_support_services.hpp"
#include "ipar_integrity_verifier.hpp"
#include "ipar_optimizer_support_services.hpp"
#include "ipar_sblr_support_services.hpp"
#include "optimizer_plan_cache.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace catalog = scratchbird::core::catalog;
namespace idx = scratchbird::core::index;
namespace optimizer = scratchbird::engine::optimizer;
namespace sblr = scratchbird::engine::sblr;
namespace storage = scratchbird::storage::page;

namespace {

bool Require(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "IPAR Worker B support services gate failure: " << message
              << '\n';
    return false;
  }
  return true;
}

bool CatalogSupportServicesCloseP2Rows() {
  catalog::IparOnlineDdlRequest ddl;
  ddl.job_uuid = "ddl-job-206";
  ddl.object_uuid = "table.customer";
  ddl.dependency_uuids = {"index.customer.pk", "routine.customer.validate"};
  ddl.catalog_epoch = 206;
  ddl.security_epoch = 1206;
  ddl.estimated_rows = 1000000;
  const auto ddl_plan = catalog::PlanIparOnlineDdl(ddl);
  if (!Require(ddl_plan.accepted && ddl_plan.online,
               "IPAR-P2-06 online DDL plan rejected")) {
    return false;
  }
  if (!Require(ddl_plan.final_publish_uses_short_exclusive_lock,
               "IPAR-P2-06 short publish proof missing")) {
    return false;
  }

  catalog::IparSystemMetadataCache metadata_cache;
  catalog::IparSystemMetadataRequest metadata;
  metadata.session_uuid = "session.ipar.p207";
  metadata.catalog_epoch = 207;
  metadata.security_epoch = 208;
  metadata.policy_epoch = 209;
  metadata.language_epoch = 210;
  metadata.requested_sys_views = {"sys.tables", "sys.indexes", "sys.constraints"};
  metadata.authorized_object_uuids = {"table.customer", "index.customer.pk"};
  const auto first = metadata_cache.GetOrMaterialize(metadata);
  const auto second = metadata_cache.GetOrMaterialize(metadata);
  if (!Require(first.accepted && !first.cache_hit && second.cache_hit,
               "IPAR-P2-07 metadata cache did not materialize then hit")) {
    return false;
  }

  catalog::IparConstraintBackfillRequest backfill;
  backfill.job_uuid = "constraint-backfill-211";
  backfill.table_uuid = "table.customer";
  backfill.constraint_uuid = "constraint.customer.fk.region";
  backfill.supporting_index_uuid = "index.region.pk";
  backfill.source_row_count = 2048;
  backfill.validated_row_count = 2048;
  backfill.catalog_epoch = 211;
  const auto backfill_plan = catalog::PlanIparConstraintBackfill(backfill);
  return Require(backfill_plan.accepted && backfill_plan.ready_to_publish &&
                     !backfill_plan.partial_visible_state,
                 "IPAR-P2-11 constraint backfill publish proof missing");
}

bool IndexSupportServicesCloseP4Rows() {
  idx::IparIndexMaintenanceRequest unique_route;
  unique_route.index_uuid = "index.customer.pk";
  unique_route.family = "btree";
  unique_route.unique = true;
  unique_route.row_count = 10;
  const auto unique_plan = idx::SelectIparIndexMaintenanceRoute(unique_route);
  if (!Require(unique_plan.accepted &&
                   unique_plan.route ==
                       idx::IparIndexMaintenanceRoute::exact_unique_probe,
               "IPAR-P4-07 unique route was not exact synchronous")) {
    return false;
  }

  idx::IparIndexMaintenanceRequest overlay_route;
  overlay_route.index_uuid = "index.customer.region";
  overlay_route.family = "btree";
  overlay_route.foreground_exactness_required = false;
  overlay_route.committed_delta_available = true;
  overlay_route.row_count = 10000;
  overlay_route.delta_count = 250;
  const auto overlay_plan = idx::SelectIparIndexMaintenanceRoute(overlay_route);
  if (!Require(overlay_plan.accepted &&
                   overlay_plan.route ==
                       idx::IparIndexMaintenanceRoute::committed_delta_overlay,
               "IPAR-P4-07 non-unique route did not choose committed overlay")) {
    return false;
  }

  const auto support = idx::PlanIparIndexSupportWork(
      {{"index.customer.region", "delta_merge", 4096, true},
       {"index.customer.region", "compaction", 4096, true},
       {"index.customer.region", "post_ddl_validation", 4096, true}},
      {});
  if (!Require(support.accepted && support.merge_count == 1 &&
                   support.compaction_count == 1 && support.validation_count == 1,
               "IPAR-P4-10 support work accounting failed")) {
    return false;
  }

  const auto reserve = idx::PlanIparIndexSplitReserve(
      {"index.customer.monotonic", 8192, 512, 64, 1000, true, {}});
  if (!Require(reserve.accepted && reserve.split_predicted &&
                   reserve.reserve_pages >= 2,
               "IPAR-P4-12 split reserve plan failed")) {
    return false;
  }

  idx::IparProbePrefetchRequest probe;
  probe.constraint_uuid = "constraint.order.customer_fk";
  probe.referenced_index_uuid = "index.customer.pk";
  probe.keys = {"customer-001", "customer-002", "customer-003"};
  probe.foreign_key = true;
  const auto prefetch = idx::PlanIparProbePrefetch(probe);
  return Require(prefetch.accepted && prefetch.prefetch_scheduled &&
                     prefetch.exact_probe_required,
                 "IPAR-P4-13 prefetch exact probe proof missing");
}

bool OptimizerSupportServicesCloseP4Rows() {
  optimizer::OptimizerPlanCache cache;
  optimizer::OptimizerPlanCacheKeyInput key;
  key.operation_id = "ipar.p408";
  key.sblr_digest = "sha256:sblr-p408";
  key.descriptor_set_digest = "sha256:descriptor-p408";
  key.statistics_snapshot_id = "stats:p408";
  key.catalog_stats_digest = "sha256:catalog-stats-p408";
  key.cost_profile_id = "cost:oltp";
  key.executor_capability_set_id = "executor:local";
  key.route_capability_digest = "sha256:route-local";
  key.security_policy_digest = "sha256:security";
  key.redaction_route_digest = "sha256:redaction";
  key.normalized_optimizer_controls_digest = "sha256:controls";
  key.parameter_shape_digest = "sha256:params";
  key.memory_grant_class = "small";
  key.memory_grant_digest = "sha256:memory";
  key.catalog_epoch = 1;
  key.stats_epoch = 2;
  key.security_epoch = 3;
  key.redaction_epoch = 4;
  key.policy_epoch = 5;
  key.resource_epoch = 6;
  key.name_resolution_epoch = 7;
  key.memory_policy_epoch = 8;
  key.memory_feedback_generation = 9;
  key.compatibility_epoch = 10;
  key.format_compatibility_epoch = 11;
  key.route_epoch = 12;
  key.object_uuids = {"table.customer"};
  key.index_uuids = {"index.customer.pk"};
  key.dependency_digests = {"sha256:dep-customer"};
  optimizer::CachedOptimizerPlan cached;
  cached.key_input = key;
  cached.cache_key = optimizer::BuildOptimizerPlanCacheKey(key);
  cached.created_epoch = key.catalog_epoch;
  cached.result.ok = true;
  cached.result.diagnostic_code = "SB_OPT_OK";
  cached.result.plan_id = "plan.ipar.p408";
  cached.metadata_only = true;
  cached.mga_visibility_recheck_required = true;
  cached.security_recheck_required = true;
  if (!Require(cache.PutEnterprise(cached).ok &&
                   cache.LookupEnterprise(key).hit,
               "IPAR-P4-08 plan cache did not safely reuse current stats")) {
    return false;
  }

  const auto bulk = optimizer::SelectIparBulkRoute(
      {"bulk.p411", "sha256:descriptor", "local_mga_read_write", 5000, 128,
       true, false, true, false, false});
  if (!Require(bulk.accepted &&
                   bulk.route == optimizer::IparBulkRouteKind::copy_import_pipeline &&
                   bulk.selected_once,
               "IPAR-P4-11 bulk route was not selected once")) {
    return false;
  }

  optimizer::IparColumnStatsDeltaAccumulator accumulator;
  if (!Require(accumulator.AddDelta(
                   {"table.customer", "column.region", 100, 5, 3, "A", "Z"}),
               "IPAR-P4-18 stat delta refused")) {
    return false;
  }
  if (!Require(accumulator.AddDelta(
                   {"table.order", "column.customer", 9, 1, 1, "A", "B"}),
               "IPAR-P4-18 unrelated stat delta refused")) {
    return false;
  }
  optimizer::IparColumnStatsSnapshot base;
  base.table_uuid = "table.customer";
  base.column_uuid = "column.region";
  base.row_count = 1000;
  base.null_count = 10;
  base.distinct_hint = 20;
  base.stats_epoch = 18;
  const auto merged = accumulator.Merge(base, 19);
  if (!Require(merged.row_count == 1100 && merged.null_count == 15 &&
                   merged.distinct_hint == 23 && merged.stats_epoch == 19,
               "IPAR-P4-18 stat delta merge failed")) {
    return false;
  }
  if (!Require(accumulator.pending_count() == 1,
               "IPAR-P4-18 stat delta merge discarded unrelated pending delta")) {
    return false;
  }

  optimizer::IparRuntimeCostFeedbackAgent feedback_agent;
  const auto feedback = feedback_agent.Record(
      {"route.customer.pk", 100, 400, 20, true, true});
  return Require(feedback.accepted && feedback.adjusted_cost > 0 &&
                     feedback_agent.Lookup("route.customer.pk").accepted,
                 "IPAR-P4-19 runtime feedback was not advisory and reusable");
}

bool SblrSupportServicesCloseP4Rows() {
  sblr::IparDispatchTable dispatch;
  if (!Require(dispatch.Register({"op.add", "int32,int32", "handler.add.i32"}),
               "IPAR-P4-14 dispatch entry refused")) {
    return false;
  }
  if (!Require(dispatch.Resolve("op.add", "int32,int32") != nullptr,
               "IPAR-P4-14 dispatch entry did not resolve")) {
    return false;
  }

  sblr::IparDeterministicExpressionCache expression_cache;
  sblr::IparExpressionCacheKey key;
  key.expression_digest = "sha256:expr.default.region";
  key.volatility_class = "deterministic";
  key.statement_epoch = 15;
  key.transaction_epoch = 150;
  if (!Require(expression_cache.Put({key, "encoded:region-default"}),
               "IPAR-P4-15 deterministic expression refused")) {
    return false;
  }
  std::string value;
  if (!Require(expression_cache.Lookup(key, &value) &&
                   value == "encoded:region-default",
               "IPAR-P4-15 deterministic expression lookup failed")) {
    return false;
  }
  key.security_sensitive = true;
  if (!Require(!expression_cache.Lookup(key, &value),
               "IPAR-P4-15 security-sensitive expression reused cache")) {
    return false;
  }

  const auto vector_plan = sblr::PlanIparValidationVectorization(
      {{{"column.id", "not_null", true}, {"column.qty", "range", true}},
       1024,
       128});
  return Require(vector_plan.accepted && vector_plan.vector_groups == 8 &&
                     vector_plan.exact_row_diagnostics,
                 "IPAR-P4-16 validation vectorization proof failed");
}

bool RoutineCacheAndIntegrityCloseRows() {
  catalog::IparRuntimeRoutineCache routine_cache;
  catalog::IparRuntimeRoutinePlan routine;
  routine.key.object_uuid = "trigger.customer.validate";
  routine.key.sblr_digest = "sha256:sblr-trigger";
  routine.key.catalog_epoch = 409;
  routine.key.security_epoch = 410;
  routine.key.dependency_epoch = 411;
  routine.compiled_handle = "compiled-sblr-trigger";
  routine.trigger = true;
  if (!Require(routine_cache.Put(routine).accepted &&
                   routine_cache.Lookup(routine.key).hit,
               "IPAR-P4-09 runtime routine cache failed")) {
    return false;
  }
  if (!Require(routine_cache.InvalidateDependencyEpoch(411) == 1 &&
                   !routine_cache.Lookup(routine.key).hit,
               "IPAR-P4-09 dependency invalidation failed")) {
    return false;
  }

  const auto report = storage::VerifyIparBackgroundIntegrity(
      {{"page.customer.1", "data_page", 1, true, true, true, true, true},
       {"index.customer.pk", "index_root", 2, true, true, true, true, true}});
  return Require(report.accepted && report.clean && report.checked_count == 2,
                 "IPAR-P6-30 background integrity verifier failed");
}

}  // namespace

int main() {
  // SEARCH_KEY: IPAR_WORKER_B_SUPPORT_SERVICES_CLOSURE
  if (!CatalogSupportServicesCloseP2Rows()) return EXIT_FAILURE;
  if (!IndexSupportServicesCloseP4Rows()) return EXIT_FAILURE;
  if (!OptimizerSupportServicesCloseP4Rows()) return EXIT_FAILURE;
  if (!SblrSupportServicesCloseP4Rows()) return EXIT_FAILURE;
  if (!RoutineCacheAndIntegrityCloseRows()) return EXIT_FAILURE;
  return EXIT_SUCCESS;
}
