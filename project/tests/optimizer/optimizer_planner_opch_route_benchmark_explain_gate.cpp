// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "runtime_consumption_evidence.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace opt = scratchbird::engine::optimizer;

namespace {

bool Require(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "OPCH-090/091 gate failure: " << message << '\n';
    return false;
  }
  return true;
}

bool HasDiagnosticContaining(const std::vector<std::string>& values,
                             const std::string& expected) {
  return std::any_of(values.begin(), values.end(), [&](const auto& value) {
    return value.find(expected) != std::string::npos;
  });
}

opt::OptimizerBenchmarkRouteLaneEvidence BenchmarkLane(std::string id,
                                                       std::string phase) {
  opt::OptimizerBenchmarkRouteLaneEvidence lane;
  lane.lane_id = std::move(id);
  lane.route_label = "embedded/sql/customer_lookup";
  lane.cache_phase = std::move(phase);
  lane.p50_us = 100.0;
  lane.p95_us = 140.0;
  lane.p99_us = 180.0;
  lane.benchmark_clean_claim = true;
  lane.trusted = true;
  lane.fresh = true;
  lane.evidence_generation = "bench-gen:opch090";
  lane.reference_comparison_required = true;
  lane.reference_comparison_id = "reference-compare:customer_lookup";
  lane.reference_engine = "postgresql";
  lane.reference_oracle_result_hash = "reference-oracle-result:customer_lookup";
  lane.reference_reference_only = true;
  lane.reference_as_authority = false;
  lane.benchmark_evidence_authority = false;
  lane.transaction_finality_authority = false;
  lane.visibility_authority = false;
  lane.security_authority = false;
  lane.recovery_authority = false;
  lane.diagnostic_code = "SB_OPCH_BENCHMARK_ROUTE_EVIDENCE.LANE_OK";
  return lane;
}

std::vector<opt::OptimizerBenchmarkRouteLaneEvidence> BenchmarkLanes() {
  return {BenchmarkLane("cold", "cold"), BenchmarkLane("warm", "warm")};
}

opt::DriverVisibleExplainRouteEvidence ExplainRoute(std::string route_kind) {
  opt::DriverVisibleExplainRouteEvidence route;
  route.route_kind = std::move(route_kind);
  route.route_label = "sql/customer_lookup";
  route.driver_visible_route = true;
  route.plan_evidence_digest = "plan-evidence:customer_lookup";
  route.explain_digest = "explain:customer_lookup:redacted";
  route.diagnostics = {"SB_OPT_OK", "SB_OPT_SORT_ORDER_REUSED"};
  route.result_hash = "result:customer_lookup";
  route.redaction_digest = "redaction:tenant_standard";
  route.redaction_applied = true;
  route.driver_or_benchmark_authority = false;
  route.transaction_finality_authority = false;
  route.visibility_authority = false;
  route.security_authority = false;
  route.recovery_authority = false;
  route.diagnostic_code = "SB_OPT_OK";
  return route;
}

std::vector<opt::DriverVisibleExplainRouteEvidence> ExplainRoutes() {
  return {
      ExplainRoute("embedded"),
      ExplainRoute("ipc"),
      ExplainRoute("inet"),
      ExplainRoute("cli"),
      ExplainRoute("driver")};
}

std::vector<std::string> RequiredRoutes() {
  return {"embedded", "ipc", "inet", "cli", "driver"};
}

bool BenchmarkEvidenceAcceptsCleanColdWarmReferenceLanes() {
  // SEARCH_KEY: OPCH_BENCHMARK_ROUTE_EVIDENCE
  const auto validation =
      opt::ValidateOptimizerBenchmarkRouteEvidence(BenchmarkLanes(), true);
  return Require(validation.ok, "benchmark-clean route lanes were rejected") &&
         Require(validation.benchmark_clean,
                 "benchmark-clean claim was not preserved") &&
         Require(validation.diagnostic_code ==
                     "SB_OPCH_BENCHMARK_ROUTE_EVIDENCE.OK",
                 "benchmark-clean diagnostic changed");
}

bool BenchmarkEvidenceRejectsRequiredGaps() {
  auto lanes = BenchmarkLanes();
  lanes.front().p95_us = 0.0;
  auto validation = opt::ValidateOptimizerBenchmarkRouteEvidence(lanes, true);
  if (!Require(!validation.ok, "missing percentile was accepted") ||
      !Require(HasDiagnosticContaining(
                   validation.diagnostics,
                   "SB_OPCH_BENCHMARK_ROUTE_EVIDENCE.PERCENTILES_MISSING"),
               "missing percentile diagnostic absent")) {
    return false;
  }

  lanes = {BenchmarkLane("cold", "cold")};
  validation = opt::ValidateOptimizerBenchmarkRouteEvidence(lanes, true);
  if (!Require(!validation.ok, "missing cold/warm lane was accepted") ||
      !Require(HasDiagnosticContaining(
                   validation.diagnostics,
                   "SB_OPCH_BENCHMARK_ROUTE_EVIDENCE.COLD_WARM_LANES_MISSING"),
               "cold/warm diagnostic absent")) {
    return false;
  }

  lanes = BenchmarkLanes();
  lanes.front().route_label.clear();
  validation = opt::ValidateOptimizerBenchmarkRouteEvidence(lanes, true);
  if (!Require(!validation.ok, "missing route label was accepted") ||
      !Require(HasDiagnosticContaining(
                   validation.diagnostics,
                   "SB_OPCH_BENCHMARK_ROUTE_EVIDENCE.ROUTE_LABEL_MISSING"),
               "route-label diagnostic absent")) {
    return false;
  }

  lanes = BenchmarkLanes();
  lanes.front().reference_comparison_id.clear();
  validation = opt::ValidateOptimizerBenchmarkRouteEvidence(lanes, true);
  if (!Require(!validation.ok, "missing reference comparison was accepted") ||
      !Require(HasDiagnosticContaining(
                   validation.diagnostics,
                   "SB_OPCH_BENCHMARK_ROUTE_EVIDENCE.REFERENCE_COMPARISON_MISSING"),
               "reference comparison diagnostic absent")) {
    return false;
  }

  lanes = BenchmarkLanes();
  lanes.front().trusted = false;
  validation = opt::ValidateOptimizerBenchmarkRouteEvidence(lanes, true);
  if (!Require(!validation.ok, "untrusted benchmark evidence was accepted") ||
      !Require(HasDiagnosticContaining(
                   validation.diagnostics,
                   "SB_OPCH_BENCHMARK_ROUTE_EVIDENCE.STALE_OR_UNTRUSTED"),
               "stale/untrusted diagnostic absent")) {
    return false;
  }

  lanes = BenchmarkLanes();
  lanes.front().reference_as_authority = true;
  validation = opt::ValidateOptimizerBenchmarkRouteEvidence(lanes, true);
  return Require(!validation.ok, "reference-as-authority evidence was accepted") &&
         Require(HasDiagnosticContaining(
                     validation.diagnostics,
                     "SB_OPCH_BENCHMARK_ROUTE_EVIDENCE.REFERENCE_AUTHORITY_DRIFT"),
                 "reference authority drift diagnostic absent");
}

bool ExplainRouteEquivalenceAcceptsDriverVisibleRoutes() {
  // SEARCH_KEY: OPCH_DRIVER_VISIBLE_ROUTE_EXPLAIN_EQUIVALENCE
  const auto validation =
      opt::ValidateDriverVisibleExplainRouteEquivalence(ExplainRoutes(),
                                                        RequiredRoutes());
  return Require(validation.ok, "driver-visible explain routes were rejected") &&
         Require(validation.diagnostic_code ==
                     "SB_OPCH_DRIVER_ROUTE_EXPLAIN_EQUIVALENCE.OK",
                 "driver-visible route diagnostic changed");
}

bool ExplainRouteEquivalenceRejectsRequiredGaps() {
  auto routes = ExplainRoutes();
  routes[1].route_label = "sql/customer_lookup/ipc-drift";
  auto validation =
      opt::ValidateDriverVisibleExplainRouteEquivalence(routes,
                                                        RequiredRoutes());
  if (!Require(!validation.ok, "route mismatch was accepted") ||
      !Require(HasDiagnosticContaining(
                   validation.diagnostics,
                   "SB_OPCH_DRIVER_ROUTE_EXPLAIN_EQUIVALENCE.ROUTE_MISMATCH"),
               "route mismatch diagnostic absent")) {
    return false;
  }

  routes = ExplainRoutes();
  routes[2].explain_digest = "explain:inet:drift";
  validation = opt::ValidateDriverVisibleExplainRouteEquivalence(
      routes, RequiredRoutes());
  if (!Require(!validation.ok, "explain mismatch was accepted") ||
      !Require(HasDiagnosticContaining(
                   validation.diagnostics,
                   "SB_OPCH_DRIVER_ROUTE_EXPLAIN_EQUIVALENCE.EXPLAIN_MISMATCH"),
               "explain mismatch diagnostic absent")) {
    return false;
  }

  routes = ExplainRoutes();
  routes[3].diagnostics.push_back("SB_OPT_ROUTE_SPECIFIC_WARNING");
  validation = opt::ValidateDriverVisibleExplainRouteEquivalence(
      routes, RequiredRoutes());
  if (!Require(!validation.ok, "diagnostic mismatch was accepted") ||
      !Require(HasDiagnosticContaining(
                   validation.diagnostics,
                   "SB_OPCH_DRIVER_ROUTE_EXPLAIN_EQUIVALENCE.DIAGNOSTIC_MISMATCH"),
               "diagnostic mismatch diagnostic absent")) {
    return false;
  }

  routes = ExplainRoutes();
  routes[4].result_hash = "result:driver:drift";
  validation = opt::ValidateDriverVisibleExplainRouteEquivalence(
      routes, RequiredRoutes());
  if (!Require(!validation.ok, "result hash mismatch was accepted") ||
      !Require(HasDiagnosticContaining(
                   validation.diagnostics,
                   "SB_OPCH_DRIVER_ROUTE_EXPLAIN_EQUIVALENCE.RESULT_HASH_MISMATCH"),
               "result hash mismatch diagnostic absent")) {
    return false;
  }

  routes = ExplainRoutes();
  routes[1].redaction_digest = "redaction:ipc:drift";
  validation = opt::ValidateDriverVisibleExplainRouteEquivalence(
      routes, RequiredRoutes());
  if (!Require(!validation.ok, "redaction mismatch was accepted") ||
      !Require(HasDiagnosticContaining(
                   validation.diagnostics,
                   "SB_OPCH_DRIVER_ROUTE_EXPLAIN_EQUIVALENCE.REDACTION_MISMATCH"),
               "redaction mismatch diagnostic absent")) {
    return false;
  }

  routes = ExplainRoutes();
  routes[4].driver_visible_route = false;
  validation = opt::ValidateDriverVisibleExplainRouteEquivalence(
      routes, RequiredRoutes());
  if (!Require(!validation.ok, "missing driver-visible route was accepted") ||
      !Require(HasDiagnosticContaining(
                   validation.diagnostics,
                   "SB_OPCH_DRIVER_ROUTE_EXPLAIN_EQUIVALENCE.DRIVER_ROUTE_MISSING"),
               "missing driver-visible route diagnostic absent")) {
    return false;
  }

  routes = ExplainRoutes();
  routes.pop_back();
  validation = opt::ValidateDriverVisibleExplainRouteEquivalence(
      routes, RequiredRoutes());
  return Require(!validation.ok, "missing required route was accepted") &&
         Require(HasDiagnosticContaining(
                     validation.diagnostics,
                     "SB_OPCH_DRIVER_ROUTE_EXPLAIN_EQUIVALENCE.MISSING_DRIVER_VISIBLE_ROUTE"),
                 "missing required route diagnostic absent");
}

}  // namespace

int main() {
  if (!BenchmarkEvidenceAcceptsCleanColdWarmReferenceLanes()) return EXIT_FAILURE;
  if (!BenchmarkEvidenceRejectsRequiredGaps()) return EXIT_FAILURE;
  if (!ExplainRouteEquivalenceAcceptsDriverVisibleRoutes()) return EXIT_FAILURE;
  if (!ExplainRouteEquivalenceRejectsRequiredGaps()) return EXIT_FAILURE;
  return EXIT_SUCCESS;
}
