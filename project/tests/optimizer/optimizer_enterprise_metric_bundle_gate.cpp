// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "metric_registry.hpp"
#include "observability/optimizer_metric_support_bundle.hpp"
#include "optimizer_metric_manifest.hpp"
#include "optimizer_route_metrics.hpp"

#include <cstdlib>
#include <iostream>
#include <string>

namespace metrics = scratchbird::core::metrics;
namespace obs = scratchbird::engine::internal_api::observability;
namespace opt = scratchbird::engine::optimizer;

namespace {

void Require(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "optimizer_enterprise_metric_bundle_gate: " << message
              << '\n';
    std::exit(1);
  }
}

opt::DriverVisibleExplainRouteEvidence Route(std::string route_kind) {
  opt::DriverVisibleExplainRouteEvidence route;
  route.route_kind = std::move(route_kind);
  route.route_label = "sblr/select/support_bundle";
  route.driver_visible_route = true;
  route.plan_evidence_digest = "plan-evidence-digest-bundle";
  route.explain_digest = "explain-digest-bundle";
  route.diagnostics = {"SB_OPT_ROUTE.OK"};
  route.result_hash = "result-hash-bundle";
  route.redaction_digest = "redaction-digest-bundle";
  route.redaction_applied = true;
  route.diagnostic_code = "SB_OPT_ROUTE.OK";
  return route;
}

void PublishSeedOptimizerMetric() {
  opt::OptimizerRouteMetricSample sample;
  sample.scope_uuid = "database-scope-bundle";
  sample.route_kind = "embedded";
  sample.route_label = "sblr/select/support_bundle";
  sample.plan_node_id = "plan-node-bundle";
  sample.plan_hash = "plan-hash-bundle";
  sample.result_hash = "result-hash-bundle";
  sample.explain_digest = "explain-digest-bundle";
  sample.result_contract_hash = "result-contract-bundle";
  sample.redaction_digest = "redaction-digest-bundle";
  sample.diagnostic_code = "SB_OPT_ROUTE.OK";
  sample.evidence_digest = "sensitive-evidence-digest-bundle";
  sample.source_generation = 88;
  sample.driver_routes = {Route("embedded"), Route("ipc"), Route("inet"),
                          Route("cli"), Route("driver")};
  sample.required_driver_routes = {"embedded", "ipc", "inet", "cli",
                                   "driver"};
  sample.authority.route_executor_authoritative = true;
  sample.authority.optimizer_explain_authoritative = true;
  sample.authority.result_contract_authoritative = true;
  sample.authority.driver_surface_authoritative = true;
  sample.authority.route_equivalence_validated = true;
  sample.authority.engine_scope_bound = true;
  sample.authority.exact_diagnostics_preserved = true;
  sample.authority.redaction_applied = true;
  const auto published = opt::PublishOptimizerRouteMetrics(sample);
  Require(published.ok, "seed route metric publication failed");
}

obs::OptimizerMetricSupportBundleAuthority GoodAuthority() {
  obs::OptimizerMetricSupportBundleAuthority authority;
  authority.metric_registry_authoritative = true;
  authority.optimizer_manifest_authoritative = true;
  authority.support_bundle_request_authorized = true;
  authority.redaction_policy_bound = true;
  authority.retention_policy_bound = true;
  authority.metrics_trusted = true;
  authority.snapshot_fresh = true;
  authority.engine_scope_bound = true;
  return authority;
}

obs::OptimizerMetricSupportBundleRequest GoodRequest() {
  obs::OptimizerMetricSupportBundleRequest request;
  request.scope_uuid = "database-scope-bundle";
  request.support_bundle_id = "support-bundle-1";
  request.capture_generation = "capture-generation-1";
  request.evidence_digest = "support-bundle-request-digest";
  request.min_source_generation = 80;
  request.benchmark_clean_export = true;
  request.allow_sensitive_labels = false;
  request.authority = GoodAuthority();
  return request;
}

void TestSupportBundleExport() {
  // SEARCH_KEY: OEIC_OPTIMIZER_METRIC_RETENTION_REDACTION
  Require(opt::EnsureOptimizerEnterpriseMetricDescriptors().ok,
          "optimizer metric descriptors failed");
  Require(opt::EnsureOptimizerRouteMetricDescriptors().ok,
          "route metric descriptors failed");
  PublishSeedOptimizerMetric();

  auto result = obs::BuildOptimizerMetricSupportBundle(GoodRequest());
  Require(result.ok, "valid optimizer metric support bundle was refused");
  Require(result.diagnostic_code == "SB_OPTIMIZER_METRIC_BUNDLE.OK",
          "unexpected support bundle diagnostic");
  Require(!result.rows.empty(), "support bundle had no optimizer metric rows");
  Require(result.tamper_digest.rfind("sha256:", 0) == 0,
          "support bundle tamper digest missing SHA-256 prefix");
  Require(result.redaction_applied, "support bundle did not apply redaction");
  Require(result.support_bundle_json.find("sensitive-evidence-digest-bundle") ==
              std::string::npos,
          "support bundle leaked sensitive evidence digest");
  bool saw_redacted_label = false;
  for (const auto& row : result.rows) {
    if (row.serialized_redacted_value.find("evidence_digest=<redacted>") !=
        std::string::npos) {
      saw_redacted_label = true;
    }
  }
  Require(saw_redacted_label, "support bundle did not redact evidence label");
}

void TestSupportBundleRefusals() {
  auto request = GoodRequest();
  request.authority.metrics_trusted = false;
  auto refused = obs::BuildOptimizerMetricSupportBundle(request);
  Require(!refused.ok &&
              refused.diagnostic_code ==
                  "SB_OPTIMIZER_METRIC_BUNDLE.AUTHORITY_REQUIRED",
          "untrusted metric support bundle was not refused");

  request = GoodRequest();
  request.authority.parser_or_reference_authority = true;
  refused = obs::BuildOptimizerMetricSupportBundle(request);
  Require(!refused.ok &&
              refused.diagnostic_code ==
                  "SB_OPTIMIZER_METRIC_BUNDLE.UNSAFE_AUTHORITY",
          "parser/reference support bundle authority was not refused");

  request = GoodRequest();
  request.min_source_generation = 999;
  refused = obs::BuildOptimizerMetricSupportBundle(request);
  Require(!refused.ok &&
              refused.diagnostic_code ==
                  "SB_OPTIMIZER_METRIC_BUNDLE.STALE_METRIC",
          "stale optimizer metric was not refused");
}

}  // namespace

int main() {
  TestSupportBundleExport();
  TestSupportBundleRefusals();
  std::cout << "optimizer enterprise metric bundle gate passed\n";
  return 0;
}
