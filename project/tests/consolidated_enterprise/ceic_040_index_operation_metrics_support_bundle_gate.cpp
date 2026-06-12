// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// CEIC-040 focused validation for index operation metrics and support bundles.
#include "index_metrics.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <iostream>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace index = scratchbird::core::index;
namespace metrics = scratchbird::core::metrics;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, const char* message) {
  if (!condition) {
    Fail(message);
  }
}

void Require(bool condition, const std::string& message) {
  if (!condition) {
    Fail(message);
  }
}

std::string Lower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return value;
}

bool ContainsProtectedText(const std::string& value) {
  const auto lower = Lower(value);
  return lower.find("secret") != std::string::npos ||
         lower.find("token") != std::string::npos ||
         lower.find("private_key") != std::string::npos ||
         lower.find("plaintext") != std::string::npos;
}

bool EvidenceHas(const std::vector<std::string>& evidence,
                 std::string_view token) {
  for (const auto& row : evidence) {
    if (row.find(token) != std::string::npos) {
      return true;
    }
  }
  return false;
}

index::IndexOperationMetricSample GoodSample(std::string suffix = "positive") {
  index::IndexOperationMetricSample sample;
  sample.identity.index_uuid = "ceic040-index-" + suffix;
  sample.identity.index_family = "hash";
  sample.identity.route_kind = "sql_select";
  sample.identity.index_generation = "ceic040-generation-" + suffix;
  sample.identity.route_generation = 44;
  sample.identity.source_generation = 88;
  sample.identity.freshness_microseconds = 100;
  sample.identity.max_freshness_microseconds = 60000000;
  sample.identity.source_kind = "index_operation_runtime";
  sample.identity.provenance = "project/src/core/index/index_metrics.cpp#CEIC_040_INDEX_OPERATION_METRICS_SUPPORT_BUNDLE";
  sample.identity.evidence_digest = "ceic040-digest-" + suffix;
  sample.identity.result = "ok";
  sample.identity.reason = "none";
  sample.runtime_operation_path = true;
  sample.support_bundle_row_producer = true;
  sample.counters.probe = 1;
  sample.counters.insert = 2;
  sample.counters.remove = 3;
  sample.counters.split = 4;
  sample.counters.merge = 5;
  sample.counters.collision = 6;
  sample.counters.recheck = 7;
  sample.counters.false_positive = 8;
  sample.counters.cleanup = 9;
  sample.counters.corruption = 10;
  sample.counters.recovery = 11;
  sample.counters.benchmark = 12;
  return sample;
}

std::vector<metrics::MetricValue> SnapshotFor(std::string_view index_uuid) {
  std::vector<metrics::MetricValue> out;
  for (const auto& value : metrics::DefaultMetricRegistry().SnapshotCurrent(false)) {
    bool matches = false;
    for (const auto& label : value.labels) {
      if (label.key == "index_uuid" && label.value == index_uuid) {
        matches = true;
        break;
      }
    }
    if (matches) {
      out.push_back(value);
    }
  }
  return out;
}

void SetLabel(metrics::MetricValue* value,
              std::string_view key,
              std::string new_value) {
  for (auto& label : value->labels) {
    if (label.key == key) {
      label.value = std::move(new_value);
      return;
    }
  }
  value->labels.push_back({std::string(key), std::move(new_value)});
}

metrics::MetricValue SyntheticProtectedMetric() {
  metrics::MetricValue value;
  value.family = index::IndexOperationCounterFamily(
      index::IndexOperationCounterKind::probe);
  value.type = metrics::MetricType::counter;
  value.value = 1;
  value.labels = {{"index_uuid", "secret-token-index"},
                  {"index_family", "hash"},
                  {"route_kind", "sql_select"},
                  {"operation", "probe"},
                  {"result", "ok"},
                  {"reason", "none"},
                  {"index_generation", "ceic040-generation-redaction"},
                  {"route_generation", "99"},
                  {"source_generation", "100"},
                  {"freshness_microseconds", "10"},
                  {"source_kind", "index_operation_runtime"},
                  {"provenance", "CEIC-040_RUNTIME"},
                  {"evidence_digest", "secret-token-digest"},
                  {"authority_scope", "evidence_only"}};
  return value;
}

void ValidatePositivePath() {
  const auto sample = GoodSample();
  const auto published = index::PublishIndexOperationMetrics(sample);
  Require(published.ok, "CEIC-040 positive publish failed");
  Require(published.emitted_counter_families.size() ==
              index::RequiredIndexOperationCounterKinds().size(),
          "CEIC-040 did not emit every operation counter family");
  Require(EvidenceHas(published.evidence, "ceic_041_crash_matrix_claimed=false"),
          "CEIC-040 publish must not claim CEIC-041");
  Require(EvidenceHas(published.evidence, "ceic_091_integrated_support_bundle_claimed=false"),
          "CEIC-040 publish must not claim CEIC-091");

  index::IndexOperationMetricSupportBundleRequest request;
  request.metrics = SnapshotFor(sample.identity.index_uuid);
  request.filter_index_uuid = sample.identity.index_uuid;
  request.require_all_operation_counters = true;
  const auto bundle = index::BuildIndexOperationMetricSupportBundle(request);
  Require(bundle.ok, "CEIC-040 positive support bundle failed");
  Require(bundle.all_required_counters_present,
          "CEIC-040 support bundle missed required counters");
  Require(bundle.rows.size() == index::RequiredIndexOperationCounterKinds().size(),
          "CEIC-040 support bundle did not emit one row per operation counter");
  Require(bundle.bounded && bundle.protected_material_excluded &&
              bundle.deterministic_labels,
          "CEIC-040 support bundle must be bounded/redacted/deterministic");
  Require(bundle.route_family_generation_evidence &&
              bundle.freshness_source_provenance_present,
          "CEIC-040 bundle must carry route/family/generation/source/provenance");

  std::map<std::string, bool> seen;
  for (const auto kind : index::RequiredIndexOperationCounterKinds()) {
    seen[index::IndexOperationCounterFamily(kind)] = false;
  }
  for (const auto& row : bundle.rows) {
    seen[row.metric_family] = true;
    Require(row.evidence_only, "CEIC-040 bundle row must be evidence-only");
    Require(!row.transaction_finality_authority &&
                !row.visibility_authority &&
                !row.authorization_security_authority &&
                !row.recovery_authority &&
                !row.parser_authority &&
                !row.reference_authority &&
                !row.wal_authority &&
                !row.benchmark_authority &&
                !row.optimizer_plan_authority &&
                !row.index_finality_authority &&
                !row.provider_finality_authority &&
                !row.local_cluster_authority &&
                !row.cluster_authority &&
                !row.agent_action_authority,
            "CEIC-040 support bundle row granted forbidden authority");
    Require(row.index_family == "hash", "CEIC-040 row family evidence missing");
    Require(row.route_kind == "sql_select", "CEIC-040 row route evidence missing");
    Require(row.index_generation == sample.identity.index_generation,
            "CEIC-040 row generation evidence missing");
    Require(row.route_generation == "44", "CEIC-040 row route generation missing");
    Require(row.source_generation == "88", "CEIC-040 row source generation missing");
    Require(row.source_kind == "index_operation_runtime",
            "CEIC-040 row source field missing");
    Require(!row.provenance.empty(), "CEIC-040 row provenance missing");
  }
  for (const auto& [family, has_row] : seen) {
    Require(has_row, std::string("CEIC-040 missing bundle row for ") + family);
  }
}

void ValidatePublishRefusals() {
  {
    auto sample = GoodSample("descriptor-only");
    sample.runtime_operation_path = false;
    sample.descriptor_only_static_evidence = true;
    const auto result = index::PublishIndexOperationMetrics(sample);
    Require(!result.ok &&
                result.diagnostic_code ==
                    "SB_INDEX_OPERATION_METRICS.RUNTIME_PRODUCER_REQUIRED",
            "CEIC-040 descriptor-only publish must fail closed");
  }
  {
    auto sample = GoodSample("authority");
    sample.authority_boundary.transaction_finality_authority = true;
    const auto result = index::PublishIndexOperationMetrics(sample);
    Require(!result.ok &&
                result.diagnostic_code ==
                    "SB_INDEX_OPERATION_METRICS.FORBIDDEN_AUTHORITY",
            "CEIC-040 forbidden authority publish must fail closed");
  }
  {
    auto sample = GoodSample("missing-generation");
    sample.identity.index_generation = "placeholder-static-generation";
    const auto result = index::PublishIndexOperationMetrics(sample);
    Require(!result.ok &&
                result.diagnostic_code ==
                    "SB_INDEX_OPERATION_METRICS.GENERATION_REQUIRED",
            "CEIC-040 placeholder generation publish must fail closed");
  }
  {
    auto sample = GoodSample("freshness");
    sample.identity.freshness_microseconds = 60000001;
    const auto result = index::PublishIndexOperationMetrics(sample);
    Require(!result.ok &&
                result.diagnostic_code ==
                    "SB_INDEX_OPERATION_METRICS.STALE",
            "CEIC-040 stale publish must fail closed");
  }
  {
    auto sample = GoodSample("cluster");
    sample.local_cluster_participation = true;
    const auto result = index::PublishIndexOperationMetrics(sample);
    Require(!result.ok &&
                result.diagnostic_code ==
                    "SB_INDEX_OPERATION_METRICS.CLUSTER_PARTICIPATION_REFUSED",
            "CEIC-040 local cluster publish must fail closed");
  }
  {
    auto sample = GoodSample("successor");
    sample.ceic_041_crash_matrix_claimed = true;
    sample.ceic_091_integrated_support_bundle_claimed = true;
    const auto result = index::PublishIndexOperationMetrics(sample);
    Require(!result.ok &&
                result.diagnostic_code ==
                    "SB_INDEX_OPERATION_METRICS.SUCCESSOR_OVERCLAIM",
            "CEIC-040 successor publish overclaim must fail closed");
  }
}

void ValidateSupportBundleRefusalsAndBounds() {
  const auto sample = GoodSample("bundle-negative");
  const auto published = index::PublishIndexOperationMetrics(sample);
  Require(published.ok, "CEIC-040 setup publish failed");
  auto metrics = SnapshotFor(sample.identity.index_uuid);
  Require(!metrics.empty(), "CEIC-040 setup metrics missing");

  {
    index::IndexOperationMetricSupportBundleRequest request;
    request.metrics = metrics;
    request.descriptor_only_static_evidence = true;
    const auto bundle = index::BuildIndexOperationMetricSupportBundle(request);
    Require(!bundle.ok &&
                bundle.diagnostic_code ==
                    "SB_INDEX_OPERATION_SUPPORT_BUNDLE.RUNTIME_ROWS_REQUIRED",
            "CEIC-040 descriptor-only bundle must fail closed");
  }
  {
    index::IndexOperationMetricSupportBundleRequest request;
    request.metrics = metrics;
    request.authority_boundary.recovery_authority = true;
    const auto bundle = index::BuildIndexOperationMetricSupportBundle(request);
    Require(!bundle.ok &&
                bundle.diagnostic_code ==
                    "SB_INDEX_OPERATION_SUPPORT_BUNDLE.FORBIDDEN_AUTHORITY",
            "CEIC-040 forbidden authority bundle must fail closed");
  }
  {
    auto bad_metrics = metrics;
    SetLabel(&bad_metrics.front(), "index_generation", "stale-placeholder");
    index::IndexOperationMetricSupportBundleRequest request;
    request.metrics = bad_metrics;
    request.require_all_operation_counters = false;
    const auto bundle = index::BuildIndexOperationMetricSupportBundle(request);
    Require(!bundle.ok &&
                bundle.diagnostic_code ==
                    "SB_INDEX_OPERATION_SUPPORT_BUNDLE.GENERATION_EVIDENCE_REQUIRED",
            "CEIC-040 stale placeholder generation bundle must fail closed");
  }
  {
    auto bad_metrics = metrics;
    SetLabel(&bad_metrics.front(), "freshness_microseconds", "60000001");
    index::IndexOperationMetricSupportBundleRequest request;
    request.metrics = bad_metrics;
    request.require_all_operation_counters = false;
    const auto bundle = index::BuildIndexOperationMetricSupportBundle(request);
    Require(!bundle.ok &&
                bundle.diagnostic_code ==
                    "SB_INDEX_OPERATION_SUPPORT_BUNDLE.STALE",
            "CEIC-040 stale bundle source must fail closed");
  }
  {
    index::IndexOperationMetricSupportBundleRequest request;
    request.metrics = metrics;
    request.local_cluster_participation = true;
    const auto bundle = index::BuildIndexOperationMetricSupportBundle(request);
    Require(!bundle.ok &&
                bundle.diagnostic_code ==
                    "SB_INDEX_OPERATION_SUPPORT_BUNDLE.CLUSTER_PARTICIPATION_REFUSED",
            "CEIC-040 local cluster bundle must fail closed");
  }
  {
    index::IndexOperationMetricSupportBundleRequest request;
    request.metrics = metrics;
    request.ceic_042_readiness_drift_claimed = true;
    request.ceic_090_integrated_metrics_coverage_claimed = true;
    const auto bundle = index::BuildIndexOperationMetricSupportBundle(request);
    Require(!bundle.ok &&
                bundle.diagnostic_code ==
                    "SB_INDEX_OPERATION_SUPPORT_BUNDLE.SUCCESSOR_OVERCLAIM",
            "CEIC-040 successor bundle overclaim must fail closed");
  }
  {
    index::IndexOperationMetricSupportBundleRequest request;
    request.metrics = metrics;
    request.require_all_operation_counters = false;
    request.limits.max_rows = 3;
    request.limits.max_output_bytes = 4096;
    const auto bundle = index::BuildIndexOperationMetricSupportBundle(request);
    Require(bundle.ok, "CEIC-040 bounded reduced bundle should remain valid");
    Require(bundle.rows.size() <= 3, "CEIC-040 bundle row limit not enforced");
    Require(bundle.dropped_row_count > 0, "CEIC-040 bounded bundle did not drop rows");
  }
  {
    index::IndexOperationMetricSupportBundleRequest request;
    request.metrics = {SyntheticProtectedMetric()};
    request.require_all_operation_counters = false;
    const auto bundle = index::BuildIndexOperationMetricSupportBundle(request);
    Require(bundle.ok, "CEIC-040 protected-value redaction bundle failed");
    Require(bundle.redacted_row_count == 1,
            "CEIC-040 protected values must be redacted");
    Require(bundle.rows.size() == 1, "CEIC-040 redaction test emitted wrong row count");
    Require(bundle.rows.front().index_uuid == "<protected-material-excluded>",
            "CEIC-040 protected index UUID must not be exposed");
    Require(!ContainsProtectedText(bundle.rows.front().labels),
            "CEIC-040 redacted labels leaked protected text");
    Require(!ContainsProtectedText(bundle.rows.front().value),
            "CEIC-040 redacted value leaked protected text");
  }
}

}  // namespace

int main() {
  const auto descriptors = index::EnsureIndexOperationMetricDescriptors();
  Require(descriptors.ok, "CEIC-040 operation descriptors failed to register");
  ValidatePositivePath();
  ValidatePublishRefusals();
  ValidateSupportBundleRefusalsAndBounds();
  std::cout << "ceic_040_index_operation_metrics_support_bundle_gate=pass\n";
  return EXIT_SUCCESS;
}
