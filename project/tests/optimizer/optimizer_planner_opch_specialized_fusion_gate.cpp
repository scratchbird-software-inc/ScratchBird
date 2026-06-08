// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "specialized_planner.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace opt = scratchbird::engine::optimizer;

namespace {

bool Require(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "OPCH specialized/fusion gate failure: " << message << '\n';
    return false;
  }
  return true;
}

bool Has(const std::vector<std::string>& values, const std::string& expected) {
  return std::find(values.begin(), values.end(), expected) != values.end();
}

opt::SpecializedWorkloadFamilyCoverage Coverage(std::string family,
                                                std::string index_family,
                                                double false_positive = 0.0) {
  opt::SpecializedWorkloadFamilyCoverage coverage;
  coverage.family = std::move(family);
  coverage.index_family = std::move(index_family);
  coverage.cost_statistics_present = true;
  coverage.selectivity_statistics_present = true;
  coverage.false_positive_statistics_present = true;
  coverage.route_gate_present = true;
  coverage.exact_recheck_required = true;
  coverage.mga_recheck_required = true;
  coverage.security_recheck_required = true;
  coverage.false_positive_ratio = false_positive;
  return coverage;
}

bool SpecializedFamiliesRequireCostRouteAndRecheckCoverage() {
  // SEARCH_KEY: OPCH_SPECIALIZED_WORKLOAD_FAMILY_COVERAGE
  std::vector<opt::SpecializedWorkloadFamilyCoverage> families = {
      Coverage("document", "document_path", 0.02),
      Coverage("vector_exact", "vector_exact", 0.0),
      Coverage("vector_hnsw", "hnsw", 0.08),
      Coverage("vector_ivf", "ivf_pq_sq8", 0.10),
      Coverage("search", "full_text_sparse_wand", 0.05),
      Coverage("graph", "graph_adjacency", 0.0),
      Coverage("time_series", "time_range_summary", 0.01),
      Coverage("bitmap", "compressed_bitmap", 0.03),
      Coverage("columnar", "columnar_zone", 0.02),
      Coverage("spatial", "rtree", 0.02),
      Coverage("gin", "gin", 0.04),
      Coverage("gist", "gist", 0.04),
      Coverage("spgist", "spgist", 0.04)};

  const auto ok = opt::ValidateSpecializedWorkloadFamilyCoverage(families);
  auto bad = Coverage("vector_hnsw", "hnsw", 0.2);
  bad.exact_recheck_required = false;
  const auto rejected = opt::ValidateSpecializedWorkloadFamilyCoverage({bad});

  return Require(ok.ok, "complete specialized family coverage rejected") &&
         Require(Has(ok.evidence, "OPCH_SPECIALIZED_WORKLOAD_FAMILY_COVERAGE"),
                 "coverage evidence missing") &&
         Require(!rejected.ok, "missing exact recheck accepted") &&
         Require(Has(rejected.diagnostics,
                     "SB_OPT_SPECIALIZED_FAMILY_RECHECK_REQUIRED:vector_hnsw"),
                 "recheck rejection diagnostic missing");
}

opt::SpecializedProviderCapability Capability(std::string family) {
  opt::SpecializedProviderCapability capability;
  capability.family = std::move(family);
  capability.local_provider_available = true;
  capability.index_available = true;
  capability.exact_fallback_available = true;
  capability.descriptor_compatible = true;
  capability.policy_allowed = true;
  capability.descriptor_visibility_proof_present = true;
  capability.descriptor_visible_to_snapshot = true;
  capability.security_redaction_proof_present = true;
  capability.security_snapshot_proof_present = true;
  capability.index_generation_proof_present = true;
  capability.index_generation_visible_to_snapshot = true;
  capability.index_covers_predicate = true;
  capability.required_index_generation = 5;
  capability.available_index_generation = 5;
  capability.policy_proof_present = true;
  capability.mga_recheck_proof_present = true;
  capability.row_mga_recheck_required = true;
  capability.row_security_recheck_required = true;
  capability.estimated_rows = 100;
  return capability;
}

bool NoSqlSqlFusionRequiresLiveRoutesAndNoDescriptorFallback() {
  // SEARCH_KEY: OPCH_NOSQL_SQL_FUSION_OPTIMIZER_ROUTES
  auto candidates = opt::PlanAllSpecializedFamilyCandidates({
      Capability("document"),
      Capability("vector"),
      Capability("search"),
      Capability("graph")});
  for (auto& candidate : candidates) {
    candidate.selected = true;
  }

  opt::NoSqlSqlFusionRouteRequest request;
  request.route_label = "embedded/sql+document+vector+search+graph";
  request.sql_result_hash = "result:stable";
  request.fusion_result_hash = "result:stable";
  request.sql_route_consumed = true;
  request.document_route_consumed = true;
  request.vector_route_consumed = true;
  request.search_route_consumed = true;
  request.graph_route_consumed = true;
  request.candidate_set_route_consumed = true;
  request.exact_recheck_required = true;
  request.mga_recheck_required = true;
  request.security_recheck_required = true;
  request.descriptor_scan_fallback = false;
  request.behavior_store_scan_fallback = false;
  request.candidates = candidates;

  const auto ok = opt::ValidateNoSqlSqlFusionRoute(request);
  request.descriptor_scan_fallback = true;
  const auto rejected = opt::ValidateNoSqlSqlFusionRoute(request);

  return Require(ok.ok, "valid NoSQL SQL fusion route rejected") &&
         Require(ok.diagnostic_code == "SB_OPT_NOSQL_SQL_FUSION.OK",
                 "fusion OK diagnostic changed") &&
         Require(Has(ok.evidence, "nosql_sql_fusion.document_route_consumed=true"),
                 "document route evidence missing") &&
         Require(Has(ok.evidence, "nosql_sql_fusion.vector_route_consumed=true"),
                 "vector route evidence missing") &&
         Require(Has(ok.evidence, "nosql_sql_fusion.search_route_consumed=true"),
                 "search route evidence missing") &&
         Require(Has(ok.evidence, "nosql_sql_fusion.graph_route_consumed=true"),
                 "graph route evidence missing") &&
         Require(!rejected.ok &&
                     rejected.diagnostic_code ==
                         "SB_OPT_NOSQL_SQL_FUSION.DESCRIPTOR_SCAN_FALLBACK_FORBIDDEN",
                 "descriptor scan fallback accepted");
}

}  // namespace

int main() {
  if (!SpecializedFamiliesRequireCostRouteAndRecheckCoverage()) return EXIT_FAILURE;
  if (!NoSqlSqlFusionRequiresLiveRoutesAndNoDescriptorFallback()) return EXIT_FAILURE;
  return EXIT_SUCCESS;
}
