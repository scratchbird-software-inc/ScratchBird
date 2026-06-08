// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "metric_registry.hpp"
#include "optimizer_metric_manifest.hpp"
#include "optimizer_route_metrics.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace metrics = scratchbird::core::metrics;
namespace opt = scratchbird::engine::optimizer;

namespace {

void Require(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "optimizer_enterprise_route_metrics_gate: " << message
              << '\n';
    std::exit(1);
  }
}

opt::OptimizerRouteMetricAuthority GoodAuthority() {
  opt::OptimizerRouteMetricAuthority authority;
  authority.route_executor_authoritative = true;
  authority.optimizer_explain_authoritative = true;
  authority.result_contract_authoritative = true;
  authority.driver_surface_authoritative = true;
  authority.route_equivalence_validated = true;
  authority.engine_scope_bound = true;
  authority.exact_diagnostics_preserved = true;
  authority.redaction_applied = true;
  return authority;
}

opt::DriverVisibleExplainRouteEvidence Route(std::string route_kind) {
  opt::DriverVisibleExplainRouteEvidence route;
  route.route_kind = std::move(route_kind);
  route.route_label = "sblr/select/customer_lookup";
  route.driver_visible_route = true;
  route.plan_evidence_digest = "plan-evidence-digest-1";
  route.explain_digest = "explain-digest-1";
  route.diagnostics = {"SB_OPT_ROUTE.OK"};
  route.result_hash = "result-hash-1";
  route.redaction_digest = "redaction-digest-1";
  route.redaction_applied = true;
  route.diagnostic_code = "SB_OPT_ROUTE.OK";
  return route;
}

opt::OptimizerRouteMetricSample GoodSample() {
  opt::OptimizerRouteMetricSample sample;
  sample.scope_uuid = "database-scope-1";
  sample.route_kind = "embedded";
  sample.route_label = "sblr/select/customer_lookup";
  sample.plan_node_id = "plan-node-route-1";
  sample.plan_hash = "plan-hash-1";
  sample.result_hash = "result-hash-1";
  sample.explain_digest = "explain-digest-1";
  sample.result_contract_hash = "result-contract-1";
  sample.redaction_digest = "redaction-digest-1";
  sample.diagnostic_code = "SB_OPT_ROUTE.OK";
  sample.evidence_digest = "route-evidence-digest-1";
  sample.source_generation = 71;
  sample.driver_routes = {Route("embedded"), Route("ipc"), Route("inet"),
                          Route("cli"), Route("driver")};
  sample.required_driver_routes = {"embedded", "ipc", "inet", "cli",
                                   "driver"};
  sample.authority = GoodAuthority();
  return sample;
}

bool HasMetricValue(const std::vector<metrics::MetricValue>& snapshot,
                    const std::string& family) {
  for (const auto& value : snapshot) {
    if (value.family == family) {
      return true;
    }
  }
  return false;
}

void RequireManifestLive(const std::string& metric_family) {
  for (const auto& entry : opt::OptimizerEnterpriseMetricManifest()) {
    if (entry.metric_family == metric_family) {
      Require(entry.producer_state ==
                  opt::OptimizerMetricProducerState::live_maintained,
              "manifest metric is not live-maintained: " + metric_family);
      Require(entry.benchmark_clean_consumable,
              "manifest metric is not benchmark-clean consumable: " +
                  metric_family);
      return;
    }
  }
  Require(false, "manifest metric missing: " + metric_family);
}

void TestRouteMetricPublication() {
  // SEARCH_KEY: OEIC_ROUTE_DRIVER_OPTIMIZER_METRICS
  Require(opt::EnsureOptimizerEnterpriseMetricDescriptors().ok,
          "optimizer descriptors failed");
  Require(opt::EnsureOptimizerRouteMetricDescriptors().ok,
          "route metric descriptors failed");

  const std::vector<std::string> manifest_families = {
      "route_plan_hash",
      "route_result_hash",
      "explain_digest",
      "route_equivalence_status",
      "driver_visible_route_count"};
  for (const auto& family : manifest_families) {
    RequireManifestLive(family);
  }

  auto result = opt::PublishOptimizerRouteMetrics(GoodSample());
  if (!result.ok) {
    std::cerr << result.diagnostic_code << ": " << result.detail << '\n';
    for (const auto& metric_result : result.metric_results) {
      if (!metric_result.ok) {
        std::cerr << "metric: " << metric_result.diagnostic_code << ": "
                  << metric_result.detail << '\n';
      }
    }
  }
  Require(result.ok, "valid route metric sample was refused");
  Require(result.diagnostic_code == "SB_OPTIMIZER_ROUTE_METRICS.OK",
          "unexpected route metric diagnostic");

  const auto snapshot = metrics::DefaultMetricRegistry().SnapshotCurrent(false);
  const std::vector<std::string> registry_families = {
      "sb_optimizer_route_plan_hash",
      "sb_optimizer_route_result_hash",
      "sb_optimizer_explain_digest",
      "sb_optimizer_route_equivalence_status",
      "sb_optimizer_driver_visible_route_count"};
  for (const auto& family : registry_families) {
    Require(HasMetricValue(snapshot, family),
            "route optimizer metric missing: " + family);
  }
}

void TestRouteMetricRefusals() {
  auto sample = GoodSample();
  sample.authority.parser_or_donor_authority = true;
  auto refused = opt::PublishOptimizerRouteMetrics(sample);
  Require(!refused.ok &&
              refused.diagnostic_code ==
                  "SB_OPTIMIZER_ROUTE_METRICS.UNSAFE_AUTHORITY",
          "parser/donor route authority was not refused");

  sample = GoodSample();
  sample.authority.route_equivalence_validated = false;
  refused = opt::PublishOptimizerRouteMetrics(sample);
  Require(!refused.ok &&
              refused.diagnostic_code ==
                  "SB_OPTIMIZER_ROUTE_METRICS.ROUTE_AUTHORITY_REQUIRED",
          "missing route equivalence authority was not refused");

  sample = GoodSample();
  sample.driver_routes[2].result_hash = "result-hash-drift";
  refused = opt::PublishOptimizerRouteMetrics(sample);
  Require(!refused.ok &&
              refused.diagnostic_code ==
                  "SB_OPTIMIZER_ROUTE_METRICS.ROUTE_EQUIVALENCE_FAILED",
          "route result hash drift was not refused");
}

}  // namespace

int main() {
  TestRouteMetricPublication();
  TestRouteMetricRefusals();
  std::cout << "optimizer enterprise route metrics gate passed\n";
  return 0;
}
