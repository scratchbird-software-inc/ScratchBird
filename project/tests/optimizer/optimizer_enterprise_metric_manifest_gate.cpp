// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "optimizer_metric_manifest.hpp"
#include "optimizer_route_metrics.hpp"
#include "specialized_workload_metrics.hpp"
#include "../support/binary_uuid_fixture.hpp"
#include "metric_observation_queue.hpp"
#include "metric_value_codec.hpp"

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

std::vector<metrics::MetricDescriptor> AdmittedInputs() {
  // Explicit catalog-boundary fixture inputs, not runtime activation or
  // producer-profile authority. The manifest owns only its declared fields.
  std::vector<metrics::MetricDescriptor> descriptors;
  unsigned ordinal = 100;
  for (const auto& entry : opt::OptimizerEnterpriseMetricManifest()) {
    metrics::MetricDescriptor descriptor;
    descriptor.family = entry.registry_family;
    descriptor.type = entry.metric_type; descriptor.unit = entry.metric_unit;
    descriptor.value_type = entry.metric_type == metrics::MetricType::state ? metrics::MetricScalarType::enumeration :
        (entry.metric_unit == metrics::MetricUnit::ratio || entry.metric_unit == metrics::MetricUnit::microseconds ||
         entry.metric_unit == metrics::MetricUnit::seconds || entry.metric_unit == metrics::MetricUnit::percent)
            ? metrics::MetricScalarType::float64 : metrics::MetricScalarType::uint64;
    if (entry.metric_type == metrics::MetricType::state) descriptor.enum_values = {7};
    if (entry.metric_type == metrics::MetricType::histogram) descriptor.histogram_buckets = {1.0, 10.0, 100.0};
    descriptor.namespace_path = "sys.metrics.optimizer.enterprise";
    descriptor.help = "Explicit manifest verification fixture";
    descriptor.producer_owner = entry.producer_owner;
    descriptor.security_family = "OPTIMIZER_METRICS";
    descriptor.visibility = metrics::MetricVisibilityScope::family;
    descriptor.cluster_only = entry.producer_state == opt::OptimizerMetricProducerState::cluster_external;
    descriptor.readiness = entry.producer_state == opt::OptimizerMetricProducerState::owned_runtime_required || descriptor.cluster_only
        ? metrics::MetricReadiness::contract_ready_unwired : metrics::MetricReadiness::implemented;
    descriptor.labels = {{"scope_uuid", true, false, metrics::MetricLabelType::system_uuid},
        {"route_label", true, false}, {"metric_family", true, false}, {"source_generation", true, false},
        {"evidence_digest", true, true}};
    // Verify real producer schemas too, including optional index labels and
    // actual route enum profiles, without collapsing them to a common template.
    for (const auto& definition : opt::OptimizerRouteMetricDescriptorDefinitions())
      if (definition.family == descriptor.family) static_cast<metrics::MetricDescriptorDefinition&>(descriptor) = definition;
    for (const auto& definition : opt::SpecializedWorkloadMetricDescriptorDefinitions())
      if (definition.family == descriptor.family) static_cast<metrics::MetricDescriptorDefinition&>(descriptor) = definition;
    descriptor.aliases = {entry.metric_family};
    descriptor.metric_uuid = scratchbird::tests::FixtureUuid(1282, ordinal++);
    descriptor.descriptor_generation = 1;
    descriptor.label_schema_uuid = scratchbird::tests::FixtureUuid(1282, ordinal++);
    descriptor.label_schema_generation = 1;
    descriptor.retention_policy_uuid = scratchbird::tests::FixtureUuid(1282, ordinal++);
    descriptor.retention_policy_generation = 1;
    descriptor.visibility_policy_uuid = scratchbird::tests::FixtureUuid(1282, ordinal++);
    descriptor.visibility_policy_generation = 1;
    descriptors.push_back(std::move(descriptor));
  }
  return descriptors;
}

void TestDescriptorRegistration() {
  auto made = metrics::MetricObservationQueue::Create(
      {scratchbird::tests::FixtureUuid(1282, 1), scratchbird::tests::FixtureUuid(1282, 2), {}}, {64, 65536});
  Require(made.ok(), "manifest fixture queue refused");
  std::shared_ptr<metrics::MetricObservationQueue> queue = std::move(made.queue);
  metrics::MetricRegistry registry(queue);
  Require(!opt::EnsureOptimizerEnterpriseMetricDescriptors(&registry).ok && registry.Descriptors(true).empty(),
          "empty manifest verification fabricated admission");
  const auto descriptors = AdmittedInputs();
  for (const auto& descriptor : descriptors) Require(registry.RegisterDescriptor(descriptor).ok, "explicit binding refused: " + descriptor.family);
  const auto ensure = opt::EnsureOptimizerEnterpriseMetricDescriptors(&registry);
  Require(ensure.ok, "descriptor verification failed: " + ensure.diagnostic_code + ":" + ensure.detail);

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

    metrics::MetricLabelSet labels = {{"scope_uuid", scratchbird::tests::FixtureUuid(1282, 3)},
                                      {"route_label", "embedded"},
                                      {"metric_family", entry.metric_family},
                                      {"source_generation", "1"},
                                      {"evidence_digest", "digest-1"}};
    const auto label_result = registry.ValidateLabels(*descriptor, labels);
    Require(label_result.ok,
            "descriptor labels rejected for " + entry.metric_family + ":" +
                label_result.diagnostic_code);
    labels[0].value = "019d0000-0000-7000-8000-000000000001";
    Require(!registry.ValidateLabels(*descriptor, labels).ok, "text scope identity accepted");
  }
  Require(registry.SnapshotCurrent(false).empty() && queue->Stats().admitted == 0 &&
              registry.Descriptors(true).size() == descriptors.size(), "manifest verification emitted or mutated inventory");
  for (int variant = 0; variant < 8; ++variant) {
    metrics::MetricRegistry bad;
    for (std::size_t i = 0; i < descriptors.size(); ++i) {
      auto descriptor = descriptors[i];
      if (i == 0) switch (variant) {
        case 0: continue;
        case 1: descriptor.producer_owner = "foreign_producer"; break;
        case 2: descriptor.unit = metrics::MetricUnit::bytes; break;
        case 3: descriptor.security_family = "foreign_security"; break;
        case 4: descriptor.labels[0].value_type = metrics::MetricLabelType::text; break;
        case 5: descriptor.labels.back().sensitive = false; break;
        case 6: descriptor.readiness = metrics::MetricReadiness::contract_ready_unwired; break;
        case 7: descriptor.value_type = metrics::MetricScalarType::float64; break;
      }
      Require(bad.RegisterDescriptor(descriptor).ok, "negative fixture did not reach manifest boundary");
    }
    const auto count = bad.Descriptors(true).size();
    Require(!opt::EnsureOptimizerEnterpriseMetricDescriptors(&bad).ok && bad.Descriptors(true).size() == count &&
                bad.SnapshotCurrent(false).empty(), "incompatible manifest binding accepted or mutated");
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
