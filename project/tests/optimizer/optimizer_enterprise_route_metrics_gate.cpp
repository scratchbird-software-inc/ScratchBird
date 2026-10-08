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
#include "../support/metric_projection_fixture.hpp"

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
  sample.scope_uuid = scratchbird::tests::FixtureUuid(1278, 1);
  sample.database_uuid = scratchbird::tests::FixtureUuid(1278, 2);
  sample.node_uuid = scratchbird::tests::FixtureUuid(1278, 3);
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

metrics::MetricLabelSet Labels(const opt::OptimizerRouteMetricSample& sample,
                               const metrics::MetricDescriptorDefinition& definition) {
  metrics::MetricLabelSet labels{{"scope_uuid", sample.scope_uuid}, {"route_label", sample.route_label},
      {"plan_node_id", sample.plan_node_id}, {"metric_family", definition.family.substr(std::string("sb_optimizer_").size())},
      {"source_generation", std::to_string(sample.source_generation)}, {"evidence_digest", sample.evidence_digest}};
  if (definition.type == metrics::MetricType::state) labels.push_back({"result", "ok"});
  return labels;
}

void TestRouteMetricPublication(scratchbird::tests::MetricProjectionFixture& fixture) {
  // SEARCH_KEY: OEIC_ROUTE_DRIVER_OPTIMIZER_METRICS
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
  Require(result.scope_uuid == GoodSample().scope_uuid && result.metric_results.size() == 5, "invalid native route receipt");
  fixture.ExpectProduced(5); fixture.Seal();
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
  for (const auto& value : snapshot) {
    if (value.family == "sb_optimizer_driver_visible_route_count")
      Require(std::holds_alternative<std::uint64_t>(value.value) && std::get<std::uint64_t>(value.value) == 5,
              "route count is not exact native UINT64");
    else {
      Require(std::holds_alternative<metrics::MetricEnumValue>(value.value) &&
                  std::get<metrics::MetricEnumValue>(value.value).code == opt::kOptimizerRouteObservationPresent,
              "route state is not a declared enum code");
      const auto expected = value.family == "sb_optimizer_route_plan_hash" ? GoodSample().plan_hash :
          value.family == "sb_optimizer_route_result_hash" ? GoodSample().result_hash :
          value.family == "sb_optimizer_explain_digest" ? GoodSample().explain_digest :
          opt::ValidateDriverVisibleExplainRouteEquivalence(GoodSample().driver_routes, GoodSample().required_driver_routes).diagnostic_code;
      Require(value.state_text == expected, "route observation lost its evidence text");
    }
  }
}

void TestNativeRefusals(scratchbird::tests::MetricProjectionFixture& fixture) {
  const auto refuse = [&](const opt::OptimizerRouteMetricSample& sample) {
    const auto result = opt::PublishOptimizerRouteMetrics(sample);
    Require(!result.ok && result.metric_results.empty() && result.scope_uuid == sample.scope_uuid,
            "invalid route did not refuse before effects");
    fixture.VerifyReadOnly();
  };
  for (auto member : {&opt::OptimizerRouteMetricSample::scope_uuid, &opt::OptimizerRouteMetricSample::database_uuid,
                      &opt::OptimizerRouteMetricSample::node_uuid}) {
    auto bad = GoodSample(); bad.*member = {}; refuse(bad);
  }
  auto bad = GoodSample(); bad.database_uuid = scratchbird::tests::FixtureUuid(1278, 99); refuse(bad);
  bad = GoodSample(); bad.node_uuid = scratchbird::tests::FixtureUuid(1278, 99); refuse(bad);
  for (auto member : {&opt::OptimizerRouteMetricSample::route_kind, &opt::OptimizerRouteMetricSample::route_label,
                      &opt::OptimizerRouteMetricSample::result_hash, &opt::OptimizerRouteMetricSample::explain_digest,
                      &opt::OptimizerRouteMetricSample::redaction_digest, &opt::OptimizerRouteMetricSample::diagnostic_code}) {
    bad = GoodSample(); bad.*member = "mismatched-sample"; refuse(bad);
  }
  auto& registry = metrics::DefaultMetricRegistry();
  for (const auto& definition : opt::OptimizerRouteMetricDescriptorDefinitions()) {
    auto labels = Labels(GoodSample(), definition);
    if (definition.type == metrics::MetricType::state) {
      Require(!registry.SetState(definition.family, labels, 1.0, "not-an-enum", definition.producer_owner).ok,
              "floating state accepted");
      Require(!registry.SetState(definition.family, labels, metrics::MetricEnumValue{2}, "undeclared", definition.producer_owner).ok,
              "undeclared enum accepted");
    } else Require(!registry.SetGauge(definition.family, labels, 5.0, definition.producer_owner).ok, "floating route count accepted");
    labels[0].value = "019d0000-0000-7000-8000-000000000001";
    Require(!registry.ValidateLabels(*registry.FindDescriptor(definition.family), labels).ok, "text scope UUID accepted");
  }
  fixture.VerifyReadOnly();
  bad = GoodSample(); bad.scope_uuid = scratchbird::tests::FixtureUuid(1278, 98);
  const auto result = opt::PublishOptimizerRouteMetrics(bad);
  Require(!result.ok && result.metric_results.size() == 5 && result.diagnostic_code != "SB_OPTIMIZER_ROUTE_METRICS.OK",
          "unadmitted route series claimed success");
  for (const auto& item : result.metric_results) Require(!item.ok, "unadmitted route series accepted");
  fixture.VerifyReadOnly();
}

void TestRouteMetricRefusals() {
  auto sample = GoodSample();
  sample.authority.parser_or_reference_authority = true;
  auto refused = opt::PublishOptimizerRouteMetrics(sample);
  Require(!refused.ok &&
              refused.diagnostic_code ==
                  "SB_OPTIMIZER_ROUTE_METRICS.UNSAFE_AUTHORITY",
          "parser/reference route authority was not refused");

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
  metrics::MetricRegistry empty;
  Require(!opt::EnsureOptimizerRouteMetricDescriptors(&empty).ok &&
              !empty.FindDescriptor("sb_optimizer_route_plan_hash"), "verification fabricated route admission");
  const auto sample = GoodSample();
  scratchbird::tests::MetricProjectionFixture fixture(sample.database_uuid, sample.node_uuid, 1279);
  for (const auto& definition : opt::OptimizerRouteMetricDescriptorDefinitions())
    fixture.AdmitDefinition(definition, Labels(sample, definition));
  fixture.Seal();
  TestRouteMetricPublication(fixture);
  TestRouteMetricRefusals();
  fixture.VerifyReadOnly();
  TestNativeRefusals(fixture);
  fixture.VerifyAndDrain();
  std::cout << "optimizer enterprise route metrics gate passed\n";
  return 0;
}
