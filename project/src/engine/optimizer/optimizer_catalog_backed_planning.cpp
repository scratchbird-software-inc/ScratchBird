// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "optimizer_catalog_backed_planning.hpp"

#include "optimizer_safety_gates.hpp"

#include <algorithm>
#include <string>

namespace scratchbird::engine::optimizer {
namespace {

constexpr const char* kOk = "SB_OPT_CATALOG_BACKED_PLANNING.OK";
constexpr const char* kRefused = "SB_OPT_CATALOG_BACKED_PLANNING.REFUSED";

void AddFailure(CatalogBackedProductionPlanningValidation* validation,
                std::string diagnostic) {
  validation->ok = false;
  validation->diagnostic_code = kRefused;
  validation->diagnostics.push_back(std::move(diagnostic));
}

bool SourceIsCatalogBacked(StatisticSource source) {
  return source == StatisticSource::kCatalogExact ||
         source == StatisticSource::kCatalogSample;
}

bool SourceIsDefaultOrUnavailable(StatisticSource source) {
  return source == StatisticSource::kPolicyDefault ||
         source == StatisticSource::kUnavailable ||
         source == StatisticSource::kClusterMetric;
}

bool Contains(const std::vector<std::string>& values, const std::string& expected) {
  return std::find(values.begin(), values.end(), expected) != values.end();
}

bool HasUsableTableStats(const TableCardinalityStats& stats) {
  return OptimizerStatsIdentityIsUsable(stats.identity) &&
         SourceIsCatalogBacked(stats.identity.source) &&
         stats.row_count != 0 &&
         stats.visible_row_count != 0 &&
         stats.page_count != 0 &&
         stats.average_row_bytes != 0 &&
         stats.visible_row_count <= stats.row_count;
}

bool HasUsableIndexStats(const IndexStats& stats,
                         const AccessPathPlanningRequest& access_request) {
  return OptimizerStatsIdentityIsUsable(stats.identity) &&
         SourceIsCatalogBacked(stats.identity.source) &&
         !stats.index_uuid.empty() &&
         stats.relation_uuid == access_request.relation_uuid &&
         !stats.descriptor_digest.empty() &&
         stats.descriptor_digest == access_request.descriptor_digest &&
         stats.height != 0 &&
         stats.leaf_pages != 0 &&
         stats.distinct_keys != 0 &&
         stats.route_benchmark_clean &&
         stats.exact_recheck_required &&
         stats.mga_recheck_required &&
         stats.security_recheck_required &&
         !stats.rebuild_in_progress &&
         !stats.family_claim_removed;
}

void ValidateProductionBuildSwitches(
    const CatalogBackedProductionPlanningRequest& request,
    const AccessPathPlanningRequest* access_request,
    CatalogBackedProductionPlanningValidation* validation) {
  OptimizerProductionBuildGateInput input;
  input.production_build = request.production_build;
  if (access_request != nullptr) {
    if (!access_request->table_stats ||
        SourceIsDefaultOrUnavailable(access_request->table_stats->identity.source)) {
      input.local_default_statistics_enabled = true;
    }
    for (const auto& index : access_request->candidate_indexes) {
      if (SourceIsDefaultOrUnavailable(index.identity.source)) {
        input.policy_default_statistics_enabled = true;
      }
    }
  }
  const auto gate = EvaluateOptimizerProductionBuildGate(input);
  if (!gate.ok) {
    validation->local_or_policy_default_diagnostic_only = true;
    validation->diagnostics.insert(validation->diagnostics.end(),
                                   gate.diagnostics.begin(),
                                   gate.diagnostics.end());
    validation->ok = false;
    validation->diagnostic_code = kRefused;
  }
}

}  // namespace

CatalogBackedProductionPlanningValidation
ValidateCatalogBackedProductionPlanningRequest(
    const CatalogBackedProductionPlanningRequest& request) {
  CatalogBackedProductionPlanningValidation validation;
  validation.ok = true;
  validation.diagnostic_code = kOk;
  validation.evidence.push_back("PCR061_CATALOG_BACKED_PRODUCTION_PLANNING");

  const auto bound_validation = ValidateBoundOptimizerRequest(request.bound_request);
  if (!bound_validation.ok) {
    validation.diagnostics.insert(validation.diagnostics.end(),
                                  bound_validation.diagnostics.begin(),
                                  bound_validation.diagnostics.end());
    validation.ok = false;
    validation.diagnostic_code = kRefused;
  }

  const auto key_validation =
      ValidateEnterpriseOptimizerPlanCacheKeyInput(request.plan_cache_key_input);
  if (!key_validation.ok) {
    validation.diagnostics.push_back(key_validation.diagnostic_code);
    validation.diagnostics.insert(validation.diagnostics.end(),
                                  key_validation.evidence.begin(),
                                  key_validation.evidence.end());
    validation.ok = false;
    validation.diagnostic_code = kRefused;
  } else {
    validation.evidence.insert(validation.evidence.end(),
                               key_validation.evidence.begin(),
                               key_validation.evidence.end());
  }

  if (!request.bound_request.catalog_access_path_request) {
    AddFailure(&validation,
               "SB_OPT_CATALOG_BACKED_PLANNING.CATALOG_ACCESS_REQUEST_REQUIRED");
    validation.local_or_policy_default_diagnostic_only = true;
  }

  if (request.bound_request.context.stats_epoch == 0 ||
      request.bound_request.context.redaction_epoch == 0 ||
      request.bound_request.context.policy_epoch == 0 ||
      request.bound_request.context.resource_epoch == 0 ||
      request.bound_request.context.name_resolution_epoch == 0 ||
      request.bound_request.context.memory_policy_epoch == 0 ||
      request.bound_request.context.memory_feedback_generation == 0 ||
      request.bound_request.context.route_epoch == 0) {
    AddFailure(&validation,
               "SB_OPT_CATALOG_BACKED_PLANNING.EPOCH_EVIDENCE_REQUIRED");
  }

  const auto* access_request =
      request.bound_request.catalog_access_path_request
          ? &*request.bound_request.catalog_access_path_request
          : nullptr;
  ValidateProductionBuildSwitches(request, access_request, &validation);

  if (access_request != nullptr) {
    if (access_request->relation_uuid.empty()) {
      AddFailure(&validation,
                 "SB_OPT_CATALOG_BACKED_PLANNING.RELATION_UUID_REQUIRED");
    }
    if (access_request->descriptor_digest.empty() ||
        access_request->descriptor_digest !=
            request.bound_request.context.descriptor_set_digest) {
      AddFailure(&validation,
                 "SB_OPT_CATALOG_BACKED_PLANNING.DESCRIPTOR_DIGEST_REQUIRED");
    }
    if (!access_request->visibility_proven) {
      AddFailure(&validation,
                 "SB_OPT_CATALOG_BACKED_PLANNING.VISIBILITY_PROOF_REQUIRED");
    }
    if (!access_request->grants_proven) {
      AddFailure(&validation,
                 "SB_OPT_CATALOG_BACKED_PLANNING.GRANT_PROOF_REQUIRED");
    }
    if (!access_request->base_row_mga_recheck_planned ||
        !access_request->base_row_security_recheck_planned) {
      AddFailure(&validation,
                 "SB_OPT_CATALOG_BACKED_PLANNING.MGA_SECURITY_RECHECK_REQUIRED");
    }
    if (!access_request->table_stats ||
        !HasUsableTableStats(*access_request->table_stats)) {
      AddFailure(&validation,
                 "SB_OPT_CATALOG_BACKED_PLANNING.TABLE_STATS_REQUIRED");
      validation.local_or_policy_default_diagnostic_only = true;
    }
    if (request.require_index_stats && access_request->candidate_indexes.empty()) {
      AddFailure(&validation,
                 "SB_OPT_CATALOG_BACKED_PLANNING.INDEX_STATS_REQUIRED");
    }
    for (const auto& index : access_request->candidate_indexes) {
      if (!HasUsableIndexStats(index, *access_request)) {
        AddFailure(&validation,
                   "SB_OPT_CATALOG_BACKED_PLANNING.INDEX_STATS_REQUIRED");
        if (SourceIsDefaultOrUnavailable(index.identity.source)) {
          validation.local_or_policy_default_diagnostic_only = true;
        }
      }
      if (!Contains(request.plan_cache_key_input.index_uuids, index.index_uuid)) {
        AddFailure(&validation,
                   "SB_OPT_CATALOG_BACKED_PLANNING.INDEX_DEPENDENCY_REQUIRED");
      }
    }
    if (!Contains(request.plan_cache_key_input.object_uuids,
                  access_request->relation_uuid)) {
      AddFailure(&validation,
                 "SB_OPT_CATALOG_BACKED_PLANNING.OBJECT_DEPENDENCY_REQUIRED");
    }
  }

  if (validation.ok) {
    validation.catalog_backed = true;
    validation.benchmark_clean_ready = true;
    validation.evidence.push_back("catalog_backed_production_planning=true");
    validation.evidence.push_back("local_default_statistics_diagnostic_only=true");
    validation.evidence.push_back("policy_default_statistics_diagnostic_only=true");
    validation.evidence.push_back("mga_visibility_recheck=preserved");
    validation.evidence.push_back("security_authorization_recheck=preserved");
  } else if (validation.diagnostic_code.empty()) {
    validation.diagnostic_code = kRefused;
  }
  return validation;
}

CatalogBackedProductionPlanningResult OptimizeCatalogBackedProductionPlan(
    const CatalogBackedProductionPlanningRequest& request) {
  CatalogBackedProductionPlanningResult result;
  result.validation = ValidateCatalogBackedProductionPlanningRequest(request);
  if (!result.validation.ok) {
    result.bound_result = MakeRefusedOptimizerResult(
        request.bound_request,
        result.validation.diagnostic_code.empty() ? kRefused
                                                  : result.validation.diagnostic_code,
        result.validation.diagnostics);
    return result;
  }

  result.bound_result = OptimizeBoundRequest(request.bound_request);
  if (!request.bound_request.catalog_access_path_request) {
    result.validation.ok = false;
    result.validation.benchmark_clean_ready = false;
    result.validation.diagnostic_code = kRefused;
    result.bound_result = MakeRefusedOptimizerResult(
        request.bound_request,
        "SB_OPT_CATALOG_BACKED_PLANNING.CATALOG_ACCESS_REQUEST_REQUIRED",
        {"SB_OPT_CATALOG_BACKED_PLANNING.CATALOG_ACCESS_REQUEST_REQUIRED"});
    return result;
  }

  result.optimized_plan = OptimizeLogicalPlanWithAccessPathRequest(
      request.bound_request.logical_plan,
      *request.bound_request.catalog_access_path_request);
  const auto benchmark = ValidateBenchmarkCleanOptimizedPlan(result.optimized_plan);
  if (!result.bound_result.ok || !result.optimized_plan.ok || !benchmark.ok) {
    result.validation.ok = false;
    result.validation.benchmark_clean_ready = false;
    result.validation.diagnostic_code = benchmark.ok
                                            ? kRefused
                                            : benchmark.diagnostic_code;
    result.validation.diagnostics.push_back(result.validation.diagnostic_code);
  }
  return result;
}

}  // namespace scratchbird::engine::optimizer
