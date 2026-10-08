// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "metric_registry.hpp"
#include "../support/binary_uuid_fixture.hpp"
#include "../support/metric_projection_fixture.hpp"
#include "metric_value_codec.hpp"
#include <algorithm>
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

void PublishSeedOptimizerMetric(scratchbird::tests::MetricProjectionFixture& fixture,
                                metrics::MetricUuid scope = scratchbird::tests::FixtureUuid(1262, 1)) {
  opt::OptimizerRouteMetricSample sample;
  sample.scope_uuid = scope;
  sample.database_uuid = scratchbird::tests::FixtureUuid(1262, 2);
  sample.node_uuid = scratchbird::tests::FixtureUuid(1262, 3);
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
  for (const auto& definition : opt::OptimizerRouteMetricDescriptorDefinitions()) {
    metrics::MetricLabelSet labels{{"scope_uuid", sample.scope_uuid}, {"route_label", sample.route_label},
        {"plan_node_id", sample.plan_node_id}, {"metric_family", definition.family.substr(std::string("sb_optimizer_").size())},
        {"source_generation", "88"}, {"evidence_digest", sample.evidence_digest}};
    if (definition.type == metrics::MetricType::state) labels.push_back({"result", "ok"});
    fixture.AdmitDefinition(definition, labels);
  }
  const auto published = opt::PublishOptimizerRouteMetrics(sample);
  Require(published.ok, "seed route metric publication failed");
  fixture.ExpectProduced(5); fixture.Seal();
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
  request.scope_uuid = scratchbird::tests::FixtureUuid(1262, 1);
  request.database_uuid = scratchbird::tests::FixtureUuid(1262, 2);
  request.node_uuid = scratchbird::tests::FixtureUuid(1262, 3);
  request.support_bundle_uuid = scratchbird::tests::FixtureUuid(1262, 4);
  request.capture_generation = 1;
  request.evidence_digest = "support-bundle-request-digest";
  request.min_source_generation = 80;
  request.benchmark_clean_export = true;
  request.allow_sensitive_labels = false;
  request.authority = GoodAuthority();
  return request;
}

void TestSupportBundleExport() {
  // SEARCH_KEY: OEIC_OPTIMIZER_METRIC_RETENTION_REDACTION
  Require(opt::EnsureOptimizerRouteMetricDescriptors().ok,
          "route metric descriptors failed");

  auto result = obs::BuildOptimizerMetricSupportBundle(GoodRequest());
  Require(result.ok, "valid optimizer metric support bundle was refused");
  Require(result.diagnostic_code == "SB_OPTIMIZER_METRIC_BUNDLE.OK",
          "unexpected support bundle diagnostic");
  Require(result.rows.size() == 5, "support bundle lost selected scope or included sibling observations");
  Require(result.tamper_digest.rfind("sha256:", 0) == 0,
          "support bundle tamper digest missing SHA-256 prefix");
  Require(result.redaction_applied, "support bundle did not apply redaction");
  const std::string secret = "sensitive-evidence-digest-bundle";
  Require(std::search(result.support_bundle_bytes.begin(), result.support_bundle_bytes.end(),
                      secret.begin(), secret.end()) == result.support_bundle_bytes.end(),
          "support bundle leaked sensitive evidence digest");
  bool saw_redacted_label = false;
  for (const auto& row : result.rows) {
    Require(!row.encoded_redacted_value.empty(), "missing encoded metric value");
    Require(std::search(row.encoded_redacted_value.begin(), row.encoded_redacted_value.end(),
                        secret.begin(), secret.end()) == row.encoded_redacted_value.end(),
            "encoded metric value leaked sensitive evidence digest");
    if (std::find(row.omitted_sensitive_labels.begin(), row.omitted_sensitive_labels.end(),
                  "evidence_digest") != row.omitted_sensitive_labels.end()) {
      saw_redacted_label = true;
    }
    auto descriptor = *metrics::DefaultMetricRegistry().FindDescriptor(row.registry_family);
    std::erase_if(descriptor.labels, [](const auto& label) { return label.sensitive; });
    const auto decoded = metrics::DecodeMetricValue(descriptor, row.encoded_redacted_value);
    Require(decoded.ok(), "redacted native value did not decode");
    bool scope_found = false;
    for (const auto& label : decoded.value->labels) if (label.key == "scope_uuid") {
      Require(std::get<metrics::MetricUuid>(label.value) == GoodRequest().scope_uuid, "bundle relabeled a sibling scope");
      scope_found = true;
    }
    Require(scope_found, "bundle lost binary scope label");
  }
  Require(saw_redacted_label, "support bundle did not redact evidence label");
  const auto repeated = obs::BuildOptimizerMetricSupportBundle(GoodRequest());
  Require(repeated.ok && repeated.support_bundle_bytes == result.support_bundle_bytes && repeated.tamper_digest == result.tamper_digest,
          "unchanged bundle did not replay exactly");
  auto sensitive = GoodRequest(); sensitive.allow_sensitive_labels = true;
  const auto authorized = obs::BuildOptimizerMetricSupportBundle(sensitive);
  Require(authorized.ok && !authorized.redaction_applied &&
              std::search(authorized.support_bundle_bytes.begin(), authorized.support_bundle_bytes.end(), secret.begin(), secret.end()) != authorized.support_bundle_bytes.end(),
          "authorized sensitive labels not preserved");
}

void TestOwnedSelections() {
  const auto refuse = [](const obs::OptimizerMetricSupportBundleRequest& request) {
    const auto result = obs::BuildOptimizerMetricSupportBundle(request);
    Require(!result.ok && result.rows.empty() && result.support_bundle_bytes.empty(), "invalid bundle leaked partial output");
  };
  auto request = GoodRequest(); request.database_uuid = scratchbird::tests::FixtureUuid(1262, 99); refuse(request);
  request = GoodRequest(); request.node_uuid = scratchbird::tests::FixtureUuid(1262, 99); refuse(request);
  request = GoodRequest(); request.scope_uuid = scratchbird::tests::FixtureUuid(1262, 99); refuse(request);
  request = GoodRequest(); request.max_metric_values = 2; refuse(request);
  request = GoodRequest(); request.support_bundle_uuid = {}; refuse(request);
  request = GoodRequest(); request.capture_generation = 0; refuse(request);
  auto current = metrics::DefaultMetricRegistry().SnapshotCurrent(false);
  for (const auto& value : current) {
    bool same_scope = false;
    for (const auto& label : value.labels) if (label.key == "scope_uuid")
      same_scope = std::get<metrics::MetricUuid>(label.value) == GoodRequest().scope_uuid;
    if (same_scope && value.family == "sb_optimizer_driver_visible_route_count") {
      request = GoodRequest(); request.metric_snapshot = {value};
      Require(obs::BuildOptimizerMetricSupportBundle(request).rows.size() == 1, "retained explicit selection refused");
      request.metric_snapshot.push_back(value); refuse(request);
      request.metric_snapshot = {value};
      request.metric_snapshot[0].value = std::uint64_t{999}; refuse(request);
      for (const auto& malformed : {"88junk", "-1", "+88", "088", "18446744073709551616"}) {
        request = GoodRequest(); request.min_source_generation = 0; request.metric_snapshot = {value};
        for (auto& label : request.metric_snapshot[0].labels) if (label.key == "source_generation") label.value = malformed;
        refuse(request);
      }
    }
  }
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
  const auto request = GoodRequest();
  scratchbird::tests::MetricProjectionFixture fixture(request.database_uuid, request.node_uuid, 1280);
  PublishSeedOptimizerMetric(fixture);
  PublishSeedOptimizerMetric(fixture, scratchbird::tests::FixtureUuid(1262, 5));
  TestSupportBundleExport();
  TestSupportBundleRefusals();
  TestOwnedSelections();
  fixture.VerifyAndDrain();
  std::cout << "optimizer enterprise metric bundle gate passed\n";
  return 0;
}
