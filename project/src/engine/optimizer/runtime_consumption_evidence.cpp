// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "runtime_consumption_evidence.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <set>
#include <string_view>
#include <utility>

namespace scratchbird::engine::optimizer {
namespace {

constexpr std::string_view kEmbeddedRoute = "embedded";
constexpr std::string_view kIpcRoute = "ipc";
constexpr std::string_view kInetRoute = "inet";
constexpr std::string_view kCliRoute = "cli";
constexpr std::string_view kDriverRoute = "driver";

bool Empty(std::string_view value) {
  return value.empty();
}

bool IsLiveBenchmarkRoute(std::string_view route_kind) {
  return route_kind == kEmbeddedRoute || route_kind == kIpcRoute ||
         route_kind == kInetRoute;
}

void RequireField(RuntimeConsumptionValidation* validation,
                  bool present,
                  std::string field_name) {
  if (!present) validation->missing_fields.push_back(std::move(field_name));
}

void RequireField(DonorDominanceTargetValidation* validation,
                  bool present,
                  std::string field_name) {
  if (!present) validation->missing_fields.push_back(std::move(field_name));
}

void RequireField(FixedRouteOverheadValidation* validation,
                  bool present,
                  std::string field_name) {
  if (!present) validation->missing_fields.push_back(std::move(field_name));
}

void RequireField(BenchmarkResultFastPathValidation* validation,
                  bool present,
                  std::string field_name) {
  if (!present) validation->missing_fields.push_back(std::move(field_name));
}

bool Positive(double value) {
  return value > 0.0;
}

bool IsDonorEngine(std::string_view engine) {
  return engine == "firebird" || engine == "mysql" ||
         engine == "postgresql" || engine == "sqlite" ||
         engine == "mariadb" || engine == "oracle" ||
         engine == "sqlserver" || engine == "db2" ||
         engine == "sybase" || engine == "informix" ||
         engine == "teradata" || engine == "snowflake" ||
         engine == "bigquery" || engine == "redshift" ||
         engine == "clickhouse" || engine == "duckdb" ||
         engine == "mongodb" || engine == "cassandra" ||
         engine == "couchbase" || engine == "redis" ||
         engine == "neo4j" || engine == "elasticsearch" ||
         engine == "solr" || engine == "cockroachdb";
}

bool IsDriverVisibleRoute(std::string_view route_kind) {
  return route_kind == kEmbeddedRoute || route_kind == kIpcRoute ||
         route_kind == kInetRoute || route_kind == kCliRoute ||
         route_kind == kDriverRoute;
}

std::string RunPrefix(const BenchmarkMethodologyRunEvidence& run) {
  return run.run_id.empty() ? "unnamed" : run.run_id;
}

std::string LanePrefix(const OptimizerBenchmarkRouteLaneEvidence& lane) {
  if (!lane.lane_id.empty()) return lane.lane_id;
  return lane.route_label.empty() ? "unnamed_lane" : lane.route_label;
}

std::string RoutePrefix(const CrossRouteResultEvidence& route) {
  if (!route.route_kind.empty()) return route.route_kind;
  return route.route_label.empty() ? "unnamed_route" : route.route_label;
}

std::string RoutePrefix(const DriverVisibleExplainRouteEvidence& route) {
  if (!route.route_kind.empty()) return route.route_kind;
  return route.route_label.empty() ? "unnamed_route" : route.route_label;
}

void AddDiagnostic(BenchmarkEquivalenceValidation* validation,
                   std::string diagnostic) {
  validation->diagnostics.push_back(std::move(diagnostic));
}

void AddDiagnostic(BenchmarkMethodologyValidation* validation,
                   const BenchmarkMethodologyRunEvidence& run,
                   std::string diagnostic) {
  validation->diagnostics.push_back(RunPrefix(run) + ":" +
                                    std::move(diagnostic));
}

void AddDiagnostic(CrossRouteEquivalenceValidation* validation,
                   const CrossRouteResultEvidence& route,
                   std::string diagnostic) {
  validation->diagnostics.push_back(RoutePrefix(route) + ":" +
                                    std::move(diagnostic));
}

void AddDiagnostic(OptimizerBenchmarkRouteEvidenceValidation* validation,
                   const OptimizerBenchmarkRouteLaneEvidence& lane,
                   std::string diagnostic) {
  validation->diagnostics.push_back(LanePrefix(lane) + ":" +
                                    std::move(diagnostic));
}

void AddDiagnostic(DriverVisibleExplainRouteValidation* validation,
                   const DriverVisibleExplainRouteEvidence& route,
                   std::string diagnostic) {
  validation->diagnostics.push_back(RoutePrefix(route) + ":" +
                                    std::move(diagnostic));
}

bool HasAuthorityDrift(const OptimizerBenchmarkRouteLaneEvidence& lane) {
  return lane.donor_as_authority || lane.benchmark_evidence_authority ||
         lane.transaction_finality_authority || lane.visibility_authority ||
         lane.security_authority || lane.recovery_authority;
}

bool HasAuthorityDrift(const DriverVisibleExplainRouteEvidence& route) {
  return route.driver_or_benchmark_authority ||
         route.transaction_finality_authority || route.visibility_authority ||
         route.security_authority || route.recovery_authority;
}

bool ContainsEmptyLabel(const std::vector<std::string>& labels) {
  return std::any_of(labels.begin(), labels.end(), [](const auto& label) {
    return label.empty();
  });
}

double NearestRankPercentile(std::vector<double> samples,
                             double percentile) {
  if (samples.empty()) return 0.0;
  std::sort(samples.begin(), samples.end());
  const auto raw_rank =
      static_cast<std::size_t>(std::ceil((percentile / 100.0) *
                                         static_cast<double>(samples.size())));
  const auto index = raw_rank == 0 ? 0 : raw_rank - 1;
  return samples[std::min(index, samples.size() - 1)];
}

bool SameMetric(double lhs, double rhs) {
  return std::abs(lhs - rhs) <= 0.0001;
}

bool SameRoute(const RuntimeOptimizedPathEvidence& evidence,
               const RouteCompletionClaim& claim) {
  return evidence.route_kind == claim.route_kind;
}

bool IsRuntimeConsumedLiveEvidence(
    const RuntimeOptimizedPathEvidence& evidence) {
  return evidence.runtime_consumed && evidence.live_execution &&
         !evidence.contract_only;
}

bool IsContractOnlyBlocker(const RuntimeOptimizedPathEvidence& evidence) {
  return evidence.contract_only && !evidence.runtime_consumed &&
         !evidence.fallback_reason.empty() && !evidence.diagnostic_code.empty();
}

bool HasPlaceholderProductionEvidenceValues(
    const RuntimeOptimizedPathEvidence& evidence) {
  return evidence.catalog_epoch == 1 || evidence.security_epoch == 1 ||
         evidence.redaction_epoch == 1 || evidence.provider_generation == 1 ||
         evidence.result_contract_hash == "result-contract-v1";
}

bool RuntimeEvidenceIsCleanlyConsumed(
    const RuntimeOptimizedPathEvidence& evidence) {
  const auto validation = ValidateRuntimeOptimizedPathEvidence(evidence);
  return validation.ok &&
         validation.state == RuntimeConsumptionState::kRuntimeConsumed &&
         !HasPlaceholderProductionEvidenceValues(evidence);
}

bool RuntimeEvidenceSupportsExactDecision(
    const RuntimeOptimizedPathEvidence& evidence) {
  const auto validation = ValidateRuntimeOptimizedPathEvidence(evidence);
  return validation.ok && !evidence.diagnostic_code.empty() &&
         (validation.state == RuntimeConsumptionState::kRuntimeConsumed ||
          ((validation.state == RuntimeConsumptionState::kSelectionOnly ||
            validation.state == RuntimeConsumptionState::kContractOnlyBlocker) &&
           !evidence.fallback_reason.empty()));
}

bool UsesEngineMgaTransactionAuthority(std::string_view authority) {
  return authority == "engine.mga.transaction_inventory" ||
         authority == "engine.mga.transaction_manager";
}

bool FixedOverheadCountersAreZero(
    const FixedRouteOverheadEvidence& evidence) {
  return evidence.repeated_parse_count == 0 &&
         evidence.repeated_lower_count == 0 &&
         evidence.repeated_descriptor_build_count == 0 &&
         evidence.repeated_result_shape_build_count == 0 &&
         evidence.repeated_text_render_count == 0;
}

void AddFixedRouteFallback(FixedRouteOverheadValidation* validation,
                           std::string diagnostic) {
  validation->exact_fallback = true;
  validation->diagnostics.push_back(std::move(diagnostic));
}

void AddBinaryFastPathFallback(BenchmarkResultFastPathValidation* validation,
                               std::string diagnostic) {
  validation->exact_fallback = true;
  validation->diagnostics.push_back(std::move(diagnostic));
}

}  // namespace

RuntimeConsumptionValidation ValidateRuntimeOptimizedPathEvidence(
    const RuntimeOptimizedPathEvidence& evidence) {
  RuntimeConsumptionValidation validation;
  validation.state = ClassifyRuntimeOptimizedPathEvidence(evidence);

  RequireField(&validation, !Empty(evidence.selected_path), "selected_path");
  RequireField(&validation, !Empty(evidence.route_kind), "route_kind");
  RequireField(&validation,
               !Empty(evidence.transaction_snapshot_class),
               "transaction_snapshot_class");
  RequireField(&validation,
               !Empty(evidence.result_contract_hash),
               "result_contract_hash");
  RequireField(&validation,
               !Empty(evidence.diagnostic_code),
               "diagnostic_code");
  if (evidence.runtime_consumed) {
    RequireField(&validation,
                 !Empty(evidence.consumed_module),
                 "consumed_module");
  }
  if (!evidence.runtime_consumed) {
    RequireField(&validation,
                 !Empty(evidence.fallback_reason),
                 "fallback_reason");
  }
  if (evidence.contract_only && evidence.runtime_consumed) {
    validation.diagnostics.push_back(
        "contract_only evidence cannot be runtime_consumed");
  }
  if (evidence.contract_only && evidence.live_execution) {
    validation.diagnostics.push_back(
        "contract_only evidence cannot assert live_execution");
  }
  if (evidence.runtime_consumed && !evidence.live_execution) {
    validation.diagnostics.push_back(
        "runtime_consumed evidence must assert live_execution");
  }

  validation.ok = validation.missing_fields.empty() &&
                  validation.diagnostics.empty() &&
                  validation.state != RuntimeConsumptionState::kInvalid;
  if (validation.ok) {
    validation.diagnostic_code = "SB_ORH_RUNTIME_CONSUMPTION_EVIDENCE.OK";
  } else if (!validation.missing_fields.empty()) {
    validation.diagnostic_code =
        "SB_ORH_RUNTIME_CONSUMPTION_EVIDENCE.MISSING_REQUIRED_FIELD";
  } else {
    validation.diagnostic_code =
        "SB_ORH_RUNTIME_CONSUMPTION_EVIDENCE.INVALID_STATE";
  }
  return validation;
}

RuntimeConsumptionState ClassifyRuntimeOptimizedPathEvidence(
    const RuntimeOptimizedPathEvidence& evidence) {
  if (evidence.selected_path.empty() || evidence.route_kind.empty()) {
    return RuntimeConsumptionState::kInvalid;
  }
  if (evidence.contract_only) {
    if (evidence.runtime_consumed || evidence.live_execution) {
      return RuntimeConsumptionState::kInvalid;
    }
    return RuntimeConsumptionState::kContractOnlyBlocker;
  }
  if (evidence.runtime_consumed) {
    if (evidence.consumed_module.empty() || !evidence.live_execution) {
      return RuntimeConsumptionState::kInvalid;
    }
    return RuntimeConsumptionState::kRuntimeConsumed;
  }
  return RuntimeConsumptionState::kSelectionOnly;
}

RuntimeOptimizedPathEvidence MakeSelectionOnlyRuntimeEvidence(
    std::string selected_path,
    std::string route_kind,
    std::string diagnostic_code,
    std::string fallback_reason) {
  RuntimeOptimizedPathEvidence evidence;
  evidence.selected_path = std::move(selected_path);
  evidence.route_kind = std::move(route_kind);
  evidence.transaction_snapshot_class = "snapshot";
  evidence.catalog_epoch = 1;
  evidence.security_epoch = 1;
  evidence.redaction_epoch = 1;
  evidence.provider_generation = 1;
  evidence.result_contract_hash = "result-contract-v1";
  evidence.diagnostic_code = std::move(diagnostic_code);
  evidence.fallback_reason = std::move(fallback_reason);
  return evidence;
}

RuntimeOptimizedPathEvidence MarkRuntimeEvidenceConsumed(
    RuntimeOptimizedPathEvidence evidence,
    std::string consumed_module) {
  evidence.runtime_consumed = true;
  evidence.live_execution = true;
  evidence.contract_only = false;
  evidence.consumed_module = std::move(consumed_module);
  evidence.fallback_reason.clear();
  evidence.diagnostic_code = "SB_ORH_RUNTIME_CONSUMPTION_EVIDENCE.CONSUMED";
  return evidence;
}

RouteClaimGuardResult EvaluateRouteCompletionClaim(
    const RouteCompletionClaim& claim,
    const std::vector<RuntimeOptimizedPathEvidence>& evidence) {
  RouteClaimGuardResult result;
  if (!claim.mark_complete) {
    result.can_mark_complete = true;
    result.diagnostic_code = "SB_ORH_ROUTE_CLAIM.NOT_COMPLETION_CLAIM";
    return result;
  }
  if (claim.route_kind.empty()) {
    result.diagnostic_code = "SB_ORH_ROUTE_CLAIM.ROUTE_KIND_REQUIRED";
    result.exact_blocker = true;
    result.diagnostics.push_back("route_kind is required");
    return result;
  }

  const bool guarded_live_route = claim.benchmark_clean && claim.live_route &&
                                  IsLiveBenchmarkRoute(claim.route_kind);
  const auto route_begin = evidence.begin();
  const auto route_end = evidence.end();
  const bool has_route_evidence =
      std::any_of(route_begin, route_end, [&](const auto& item) {
        return SameRoute(item, claim);
      });
  const bool has_live_consumption =
      std::any_of(route_begin, route_end, [&](const auto& item) {
        return SameRoute(item, claim) && RuntimeEvidenceIsCleanlyConsumed(item);
      });
  const bool has_placeholder_live_consumption =
      std::any_of(route_begin, route_end, [&](const auto& item) {
        return SameRoute(item, claim) && IsRuntimeConsumedLiveEvidence(item) &&
               ValidateRuntimeOptimizedPathEvidence(item).ok &&
               HasPlaceholderProductionEvidenceValues(item);
      });

  if (!guarded_live_route) {
    result.can_mark_complete = has_route_evidence;
    result.exact_blocker = !has_route_evidence;
    result.diagnostic_code = has_route_evidence
                                 ? "SB_ORH_ROUTE_CLAIM.OK"
                                 : "SB_ORH_ROUTE_CLAIM.EVIDENCE_REQUIRED";
    return result;
  }

  if (has_live_consumption) {
    result.can_mark_complete = true;
    result.diagnostic_code = "SB_ORH_ROUTE_CLAIM.LIVE_CONSUMPTION_OK";
    return result;
  }

  const bool contract_only_blocker =
      std::any_of(route_begin, route_end, [&](const auto& item) {
        return SameRoute(item, claim) && IsContractOnlyBlocker(item) &&
               ValidateRuntimeOptimizedPathEvidence(item).ok;
      });
  result.can_mark_complete = false;
  result.exact_blocker = true;
  result.diagnostic_code =
      contract_only_blocker
          ? "SB_ORH_ROUTE_CONTRACT_ONLY_LIVE_CLOSURE_BLOCKED"
          : (has_placeholder_live_consumption
                 ? "SB_ORH_ROUTE_PLACEHOLDER_RUNTIME_EVIDENCE"
                 : "SB_ORH_ROUTE_RUNTIME_CONSUMPTION_MISSING");
  result.diagnostics.push_back(
      has_placeholder_live_consumption
          ? "live benchmark-clean routes require real route epochs, provider "
            "generation, and result contract evidence"
          : "live benchmark-clean embedded/ipc/inet routes require "
            "runtime_consumed live evidence and cannot close on contract-only "
            "evidence");
  return result;
}

}  // namespace scratchbird::engine::optimizer
