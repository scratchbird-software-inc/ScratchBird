// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "cluster_refusal_path.hpp"
#include "optimizer_enterprise_manifest.hpp"

#include <cstdlib>
#include <iostream>
#include <string>

namespace opt = scratchbird::engine::optimizer;

namespace {

void Require(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "optimizer_enterprise_inventory_gate: " << message << '\n';
    std::exit(1);
  }
}

const opt::EnterpriseOptimizerSurfaceEntry* FindSurface(const std::string& id) {
  for (const auto& entry : opt::EnterpriseOptimizerSurfaceManifest()) {
    if (entry.surface_id == id) {
      return &entry;
    }
  }
  return nullptr;
}

void TestManifestValidation() {
  // SEARCH_KEY: OEIC_LEGACY_CAPABILITY_MAPPING
  const auto validation = opt::ValidateEnterpriseOptimizerManifest();
  if (!validation.ok) {
    for (const auto& diagnostic : validation.diagnostics) {
      std::cerr << diagnostic << '\n';
    }
  }
  Require(validation.ok, "enterprise optimizer manifest did not validate");
  Require(opt::LegacyOptimizerCapabilityClosures().size() >= 16,
          "legacy capability inventory is too shallow");
  Require(opt::EnterpriseOptimizerSurfaceManifest().size() >= 20,
          "optimizer surface inventory is too shallow");
}

void TestNoStubSurfaceClasses() {
  // SEARCH_KEY: OEIC_NO_STUB_OPTIMIZER_SURFACE
  const auto* cluster = FindSurface("cluster_fragment");
  Require(cluster != nullptr, "cluster surface missing");
  Require(cluster->surface_class == opt::EnterpriseOptimizerSurfaceClass::cluster_external,
          "cluster surface is not external-provider-only");
  Require(!cluster->production_route_admissible && !cluster->benchmark_clean_admissible,
          "cluster surface is admissible in core production or benchmark-clean route");

  const auto* reference = FindSurface("reference_authority");
  Require(reference != nullptr, "reference authority removed-claim surface missing");
  Require(reference->surface_class == opt::EnterpriseOptimizerSurfaceClass::removed_claim,
          "reference authority is not removed from optimizer authority claims");
  Require(!reference->production_route_admissible && !reference->benchmark_clean_admissible,
          "reference authority can satisfy production or benchmark-clean optimizer routes");

  const auto* parser = FindSurface("parser_execution_authority");
  Require(parser != nullptr, "parser authority removed-claim surface missing");
  Require(parser->surface_class == opt::EnterpriseOptimizerSurfaceClass::removed_claim,
          "parser authority is not removed from optimizer authority claims");

  const auto* join = FindSurface("join_property_frontier");
  Require(join != nullptr && join->production_route_admissible &&
              join->requires_real_metric_producer,
          "join property frontier is not represented as a production metric-backed surface");

  const auto* llvm = FindSurface("llvm_native_compile");
  Require(llvm != nullptr, "LLVM native compile surface missing");
  Require(llvm->surface_class == opt::EnterpriseOptimizerSurfaceClass::noncluster_exact_fallback,
          "LLVM native compile must be optional exact-fallback acceleration");
  Require(!llvm->benchmark_clean_admissible,
          "LLVM native compile cannot close benchmark-clean routes until OEIC LLVM gates pass");
}

void TestClusterBoundaryRefusals() {
  // SEARCH_KEY: OEIC_CLUSTER_OPTIMIZER_EXTERNAL_BOUNDARY
  opt::ClusterOptimizerBoundaryRequest no_cluster;
  no_cluster.cluster_route_requested = true;
  auto refused = opt::EvaluateClusterOptimizerBoundary(no_cluster);
  Require(refused.refused &&
              refused.diagnostic_code == "SB_OPT_CLUSTER_BOUNDARY.CLUSTER_ROUTE_EXTERNAL_PROVIDER_REQUIRED",
          "no-cluster route did not fail closed with exact diagnostic");

  opt::ClusterOptimizerBoundaryRequest metric;
  metric.cluster_metric_requested = true;
  refused = opt::EvaluateClusterOptimizerBoundary(metric);
  Require(refused.refused &&
              refused.diagnostic_code == "SB_OPT_CLUSTER_BOUNDARY.CLUSTER_METRIC_EXTERNAL_PROVIDER_REQUIRED",
          "cluster metric did not fail closed with exact diagnostic");

  opt::ClusterOptimizerBoundaryRequest public_stub;
  public_stub.cluster_route_requested = true;
  public_stub.external_provider_available = true;
  public_stub.public_stub_provider = true;
  public_stub.production_live_claim = true;
  refused = opt::EvaluateClusterOptimizerBoundary(public_stub);
  Require(refused.refused &&
              refused.diagnostic_code == "SB_OPT_CLUSTER_BOUNDARY.PUBLIC_STUB_LIVE_CLAIM_REFUSED",
          "public stub live claim was not refused");

  opt::ClusterOptimizerBoundaryRequest benchmark;
  benchmark.cluster_route_requested = true;
  benchmark.benchmark_clean_claim = true;
  benchmark.external_provider_available = true;
  refused = opt::EvaluateClusterOptimizerBoundary(benchmark);
  Require(refused.refused &&
              refused.diagnostic_code == "SB_OPT_CLUSTER_BOUNDARY.BENCHMARK_CLEAN_CORE_REFUSED",
          "cluster benchmark-clean claim was not refused in core");

  opt::ClusterOptimizerBoundaryRequest external;
  external.cluster_route_requested = true;
  external.external_provider_available = true;
  external.operation_id = "optimizer.cluster.route";
  auto routed = opt::EvaluateClusterOptimizerBoundary(external);
  Require(routed.ok && routed.externally_routed &&
              routed.diagnostic_code == "SB_OPT_CLUSTER_BOUNDARY.EXTERNAL_PROVIDER_ROUTE_REQUIRED",
          "external cluster provider route did not produce external dispatch evidence");
}

}  // namespace

int main() {
  TestManifestValidation();
  TestNoStubSurfaceClasses();
  TestClusterBoundaryRefusals();
  std::cout << "optimizer enterprise inventory gate passed\n";
  return 0;
}
