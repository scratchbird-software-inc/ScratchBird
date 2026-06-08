// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "optimizer_metric_manifest.hpp"

#include <cstdlib>
#include <iostream>
#include <set>
#include <string>

namespace opt = scratchbird::engine::optimizer;
namespace metrics = scratchbird::core::metrics;

namespace {

void Require(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "optimizer_enterprise_metric_manifest_gate: " << message
              << '\n';
    std::exit(1);
  }
}

const opt::OptimizerEnterpriseMetricEntry* FindMetric(const std::string& family) {
  for (const auto& entry : opt::OptimizerEnterpriseMetricManifest()) {
    if (entry.metric_family == family) {
      return &entry;
    }
  }
  return nullptr;
}

void TestMetricManifestCompleteness() {
  // SEARCH_KEY: OEIC_OPTIMIZER_METRIC_OWNERSHIP_MATRIX
  const auto validation = opt::ValidateOptimizerEnterpriseMetricManifest();
  if (!validation.ok) {
    for (const auto& diagnostic : validation.diagnostics) {
      std::cerr << diagnostic << '\n';
    }
  }
  Require(validation.ok, "metric manifest did not validate");
  Require(opt::OptimizerEnterpriseMetricManifest().size() >= 39,
          "optimizer metric manifest is too shallow");

  std::set<std::string> families;
  for (const auto& entry : opt::OptimizerEnterpriseMetricManifest()) {
    families.insert(entry.metric_family);
    Require(!entry.producer_owner.empty(), "producer owner missing");
    Require(!entry.consumer_owner.empty(), "consumer owner missing");
    Require(!entry.producer_anchor.empty(), "producer anchor missing");
    Require(!entry.consumer_anchor.empty(), "consumer anchor missing");
    Require(!entry.required_evidence.empty(), "evidence list missing");
    Require(entry.support_bundle_class !=
                opt::OptimizerMetricSupportBundleClass::omitted_from_default_bundle ||
                !entry.benchmark_clean_consumable,
            "benchmark-clean metric omitted from support bundle");
    Require(entry.producer_state != opt::OptimizerMetricProducerState::cluster_external,
            "cluster metric leaked into noncluster optimizer manifest");
  }

  const char* required[] = {"operator_actual_rows",
                            "memory_grant_bytes",
                            "page_cache_hit_miss",
                            "mga_cleanup_debt",
                            "btree_depth",
                            "hash_collision_depth",
                            "vector_recall_observed",
                            "document_path_selectivity",
                            "route_result_hash",
                            "invalidation_reason"};
  for (const auto* required_family : required) {
    Require(families.count(required_family) == 1,
            std::string("required metric missing: ") + required_family);
  }
}

void TestDescriptorRegistration() {
  metrics::MetricRegistry registry;
  const auto ensure = opt::EnsureOptimizerEnterpriseMetricDescriptors(&registry);
  Require(ensure.ok, "descriptor registration failed: " + ensure.diagnostic_code);

  for (const auto& entry : opt::OptimizerEnterpriseMetricManifest()) {
    const auto* descriptor = registry.FindDescriptor(entry.registry_family);
    Require(descriptor != nullptr,
            "registered descriptor missing for " + entry.metric_family);
    Require(descriptor->producer_owner == entry.producer_owner,
            "descriptor producer owner mismatch for " + entry.metric_family);
    Require(descriptor->type == entry.metric_type,
            "descriptor type mismatch for " + entry.metric_family);
    Require(descriptor->unit == entry.metric_unit,
            "descriptor unit mismatch for " + entry.metric_family);
    Require(descriptor->security_family == "OPTIMIZER_METRICS",
            "descriptor security family mismatch for " + entry.metric_family);
    Require(registry.FindDescriptorOrAlias(entry.metric_family) == descriptor,
            "descriptor alias lookup failed for " + entry.metric_family);

    metrics::MetricLabelSet labels = {{"scope_uuid", "scope-1"},
                                      {"route_label", "embedded"},
                                      {"metric_family", entry.metric_family},
                                      {"source_generation", "1"},
                                      {"evidence_digest", "digest-1"}};
    const auto label_result = registry.ValidateLabels(*descriptor, labels);
    Require(label_result.ok,
            "descriptor labels rejected for " + entry.metric_family + ":" +
                label_result.diagnostic_code);
  }
}

void TestConsumptionTruthfulness() {
  const auto* memory = FindMetric("memory_grant_bytes");
  Require(memory != nullptr, "memory grant metric missing");
  Require(memory->producer_state == opt::OptimizerMetricProducerState::live_maintained,
          "memory grant metric must be owned by a live maintained producer");

  const auto* operator_actuals = FindMetric("operator_actual_rows");
  Require(operator_actuals != nullptr, "operator actual rows metric missing");
  Require(operator_actuals->producer_state ==
              opt::OptimizerMetricProducerState::live_maintained,
          "operator actual rows must be live-maintained by OEIC-011 executor producer");
  Require(!operator_actuals->benchmark_clean_consumable,
          "operator actual rows cannot be benchmark-clean consumable before route proof gates");

  const auto* route_hash = FindMetric("route_result_hash");
  Require(route_hash != nullptr, "route result hash metric missing");
  Require(route_hash->redaction_class == opt::OptimizerMetricRedactionClass::protected_digest,
          "route result hash must be protected digest data");
  Require(route_hash->support_bundle_class ==
              opt::OptimizerMetricSupportBundleClass::digest_only,
          "route result hash must be digest-only in support bundles");
}

}  // namespace

int main() {
  TestMetricManifestCompleteness();
  TestDescriptorRegistration();
  TestConsumptionTruthfulness();
  std::cout << "optimizer enterprise metric manifest gate passed\n";
  return 0;
}
