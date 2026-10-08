// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "optimizer_storage_metrics.hpp"
#include "metric_bound_definition.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <bit>
#include <limits>
#include <utility>

namespace scratchbird::storage::page {
namespace {

namespace metrics = scratchbird::core::metrics;

using metrics::MetricDescriptor;
using metrics::MetricLabelDescriptor;
using metrics::MetricReadiness;
using metrics::MetricType;
using metrics::MetricUnit;
using metrics::MetricValidationResult;

metrics::MetricDescriptorDefinition Descriptor(std::string family,
                            MetricType type,
                            MetricUnit unit,
                            std::string producer_owner,
                            std::string help) {
  metrics::MetricDescriptorDefinition descriptor;
  descriptor.family = std::move(family);
  descriptor.type = type;
  descriptor.unit = unit;
  descriptor.namespace_path = "sys.metrics.optimizer.enterprise";
  descriptor.help = std::move(help);
  descriptor.producer_owner = std::move(producer_owner);
  descriptor.security_family = "OPTIMIZER_METRICS";
  descriptor.value_type = unit == MetricUnit::ratio || unit == MetricUnit::microseconds
      ? metrics::MetricScalarType::float64 : metrics::MetricScalarType::uint64;
  descriptor.labels = {MetricLabelDescriptor{"scope_uuid", true, false, metrics::MetricLabelType::system_uuid},
                       MetricLabelDescriptor{"route_label", true, false},
                       MetricLabelDescriptor{"plan_node_id", false, false},
                       MetricLabelDescriptor{"metric_family", true, false},
                       MetricLabelDescriptor{"result", false, false},
                       MetricLabelDescriptor{"source_generation", true, false},
                       MetricLabelDescriptor{"evidence_digest", true, true}};
  if (type == MetricType::histogram) {
    descriptor.histogram_buckets = {1.0, 10.0, 100.0, 1000.0, 10000.0, 100000.0,
                                    1000000.0, 10000000.0};
  }
  return descriptor;
}


bool UnsafeAuthority(const OptimizerStorageMetricAuthority& authority) {
  return authority.parser_or_reference_authority ||
         authority.client_finality_or_visibility_authority ||
         authority.metric_visibility_or_finality_authority ||
         authority.metric_recovery_authority ||
         authority.benchmark_authority;
}

void AddEvidence(OptimizerStorageMetricPublishResult* result,
                 std::string evidence) {
  if (result != nullptr) {
    result->evidence.push_back(std::move(evidence));
  }
}

OptimizerStorageMetricPublishResult Refuse(const OptimizerStorageMetricSample& sample,
                                           std::string code,
                                           std::string detail) {
  OptimizerStorageMetricPublishResult result;
  result.ok = false;
  result.diagnostic_code = std::move(code);
  result.detail = std::move(detail);
  AddEvidence(&result, "OEIC_STORAGE_IO_OPTIMIZER_METRICS");
  AddEvidence(&result, "optimizer.storage_metrics.fail_closed=true");
  result.filespace_uuid = sample.filespace_uuid;
  AddEvidence(&result, "optimizer.storage_metrics.refused=" +
                           result.diagnostic_code);
  return result;
}

bool EmptyRequiredField(const OptimizerStorageMetricSample& sample,
                        std::string* field) {
  if (!scratchbird::core::uuid::IsEngineIdentityUuid(sample.scope_uuid)) {
    if (field != nullptr) *field = "scope_uuid";
    return true;
  }
  if (!scratchbird::core::uuid::IsEngineIdentityUuid(sample.database_uuid)) {
    if (field != nullptr) *field = "database_uuid";
    return true;
  }
  if (!scratchbird::core::uuid::IsEngineIdentityUuid(sample.filespace_uuid)) {
    if (field != nullptr) *field = "filespace_uuid";
    return true;
  }
  if (!scratchbird::core::uuid::IsEngineIdentityUuid(sample.node_uuid)) {
    if (field != nullptr) *field = "node_uuid";
    return true;
  }
  if (sample.route_label.empty()) {
    if (field != nullptr) *field = "route_label";
    return true;
  }
  if (sample.page_family.empty()) {
    if (field != nullptr) *field = "page_family";
    return true;
  }
  if (sample.page_class.empty()) {
    if (field != nullptr) *field = "page_class";
    return true;
  }
  if (sample.evidence_digest.empty()) {
    if (field != nullptr) *field = "evidence_digest";
    return true;
  }
  if (!sample.resident_bytes) {
    if (field != nullptr) *field = "resident_bytes";
    return true;
  }
  return false;
}

double Ratio(std::uint64_t numerator, std::uint64_t denominator) {
  if (denominator == 0) {
    return 0.0;
  }
  return static_cast<double>(numerator) / static_cast<double>(denominator);
}

bool ExactFloat64Duration(std::uint64_t value) noexcept {
  const auto width = std::bit_width(value);
  return width <= 53 || (value & ((std::uint64_t{1} << (width - 53)) - 1)) == 0;
}

metrics::MetricLabelSet LabelsFor(const OptimizerStorageMetricSample& sample,
                                  std::string metric_family,
                                  std::string result = {}) {
  metrics::MetricLabelSet labels = {
      {"scope_uuid", sample.scope_uuid},
      {"route_label", sample.route_label},
      {"metric_family", std::move(metric_family)},
      {"source_generation", std::to_string(sample.source_generation)},
      {"evidence_digest", sample.evidence_digest}};
  if (!result.empty()) {
    labels.push_back({"result", std::move(result)});
  }
  return labels;
}

void Push(OptimizerStorageMetricPublishResult* result,
          MetricValidationResult metric_result) {
  if (!metric_result.ok && result != nullptr) {
    result->ok = false;
    if (result->diagnostic_code.empty()) {
      result->diagnostic_code = metric_result.diagnostic_code;
      result->detail = metric_result.detail;
    }
  }
  if (result != nullptr) {
    result->metric_results.push_back(std::move(metric_result));
  }
}

void Gauge(OptimizerStorageMetricPublishResult* result,
           const OptimizerStorageMetricSample& sample,
           const std::string& family,
           const std::string& metric_family,
           metrics::MetricScalar value,
           const std::string& producer_owner) {
  Push(result,
       metrics::DefaultMetricRegistry().SetGauge(
           family, LabelsFor(sample, metric_family), value, producer_owner));
}

void Counter(OptimizerStorageMetricPublishResult* result,
             const OptimizerStorageMetricSample& sample,
             const std::string& family,
             const std::string& metric_family,
             std::string outcome,
             std::uint64_t value,
             const std::string& producer_owner) {
  Push(result,
       metrics::DefaultMetricRegistry().IncrementCounter(
           family,
           LabelsFor(sample, metric_family, std::move(outcome)),
           value,
           producer_owner));
}

void Histogram(OptimizerStorageMetricPublishResult* result,
               const OptimizerStorageMetricSample& sample,
               const std::string& family,
               const std::string& metric_family,
               std::uint64_t value,
               const std::string& producer_owner) {
  Push(result,
       metrics::DefaultMetricRegistry().ObserveHistogram(
           family,
           LabelsFor(sample, metric_family),
           static_cast<double>(value),
           producer_owner));
}

}  // namespace

const std::vector<metrics::MetricDescriptorDefinition>&
OptimizerStorageMetricDescriptorDefinitions() {
  static const std::vector<metrics::MetricDescriptorDefinition> descriptors = {
      Descriptor("sb_optimizer_page_cache_hit_miss", MetricType::counter,
                 MetricUnit::none, "storage_page",
                 "Optimizer page cache hit/miss observations."),
      Descriptor("sb_optimizer_prefetch_usefulness", MetricType::gauge,
                 MetricUnit::ratio, "storage_prefetch",
                 "Optimizer prefetch usefulness ratio."),
      Descriptor("sb_optimizer_page_count", MetricType::gauge,
                 MetricUnit::none, "storage_page",
                 "Optimizer-visible page count."),
      Descriptor("sb_optimizer_page_cache_dirty_pressure", MetricType::gauge,
                 MetricUnit::ratio, "storage_page",
                 "Optimizer-visible page cache dirty-page pressure."),
      Descriptor("sb_optimizer_page_cache_pin_pressure", MetricType::gauge,
                 MetricUnit::ratio, "storage_page",
                 "Optimizer-visible page cache pin pressure."),
      Descriptor("sb_optimizer_writeback_pressure", MetricType::gauge,
                 MetricUnit::ratio, "storage_page",
                 "Optimizer-visible writeback pressure."),
      Descriptor("sb_optimizer_sequential_page_cost", MetricType::histogram,
                 MetricUnit::microseconds, "storage_disk",
                 "Sequential page read cost sample."),
      Descriptor("sb_optimizer_random_page_cost", MetricType::histogram,
                 MetricUnit::microseconds, "storage_disk",
                 "Random page read cost sample."),
      Descriptor("sb_optimizer_filespace_pressure", MetricType::gauge,
                 MetricUnit::ratio, "storage_filespace",
                 "Optimizer-visible filespace pressure.")};
  return descriptors;
}

metrics::MetricValidationResult EnsureOptimizerStorageMetricDescriptors(
    metrics::MetricRegistry* registry) {
  for (const auto& descriptor : OptimizerStorageMetricDescriptorDefinitions()) {
    const auto result = metrics::ValidateBoundMetricDefinition(
        registry ? *registry : metrics::DefaultMetricRegistry(), descriptor);
    if (!result.ok) {
      return result;
    }
  }
  return metrics::MetricOk();
}

OptimizerStorageMetricPublishResult PublishOptimizerStorageMetrics(
    const OptimizerStorageMetricSample& sample) {
  std::string missing_field;
  if (EmptyRequiredField(sample, &missing_field)) {
    return Refuse(sample,
                  "SB_OPTIMIZER_STORAGE_METRICS.MISSING_SCOPE",
                  "optimizer.storage_metrics.required_field_missing:" +
                      missing_field);
  }
  if (sample.source_generation == 0) {
    return Refuse(sample,
                  "SB_OPTIMIZER_STORAGE_METRICS.SOURCE_GENERATION_REQUIRED",
                  "optimizer.storage_metrics.source_generation_required");
  }
  if (sample.freshness_microseconds > sample.max_freshness_microseconds) {
    return Refuse(sample,
                  "SB_OPTIMIZER_STORAGE_METRICS.STALE",
                  "optimizer.storage_metrics.stale");
  }
  if (!sample.authority.storage_page_manager_authoritative ||
      !sample.authority.filespace_identity_authoritative ||
      !sample.authority.engine_scope_bound) {
    return Refuse(sample,
                  "SB_OPTIMIZER_STORAGE_METRICS.STORAGE_AUTHORITY_REQUIRED",
                  "optimizer.storage_metrics.storage_authority_required");
  }
  if (UnsafeAuthority(sample.authority)) {
    return Refuse(sample,
                  "SB_OPTIMIZER_STORAGE_METRICS.UNSAFE_AUTHORITY",
                  "optimizer.storage_metrics.unsafe_authority");
  }
  if (sample.resident_pages > sample.page_count ||
      sample.pinned_pages > sample.resident_pages ||
      sample.dirty_pages > sample.resident_pages ||
      sample.writeback_pages > sample.resident_pages ||
      sample.filespace_used_bytes > sample.filespace_total_bytes ||
      sample.filespace_free_bytes > sample.filespace_total_bytes - sample.filespace_used_bytes ||
      sample.filespace_reserved_bytes > sample.filespace_total_bytes - sample.filespace_used_bytes) {
    return Refuse(sample,
                  "SB_OPTIMIZER_STORAGE_METRICS.COUNTERS_INCONSISTENT",
                  "optimizer.storage_metrics.counters_inconsistent");
  }

  const auto& registry = metrics::DefaultMetricRegistry();
  // Existing duration descriptors are FLOAT64. Preserve integer-microsecond
  // measurements exactly; never silently round an observation at publication.
  if (!ExactFloat64Duration(sample.sequential_read_latency_microseconds) ||
      !ExactFloat64Duration(sample.random_read_latency_microseconds))
    return Refuse(sample, "METRIC.VALUE_INVALID", "integer microsecond sample is not exactly representable by its FLOAT64 profile");
  if (!registry.ObservationOwnerMatches(sample.database_uuid, sample.node_uuid))
    return Refuse(sample, "METRIC.OBSERVATION_SOURCE_UNAVAILABLE", "storage sample does not match the bound observation owner");
  const auto descriptors = EnsureOptimizerStorageMetricDescriptors();
  if (!descriptors.ok) return Refuse(sample, descriptors.diagnostic_code, descriptors.detail);

  OptimizerStorageMetricPublishResult result;
  result.ok = true;
  result.filespace_uuid = sample.filespace_uuid;
  result.diagnostic_code = "SB_OPTIMIZER_STORAGE_METRICS.OK";
  AddEvidence(&result, "OEIC_STORAGE_IO_OPTIMIZER_METRICS");
  AddEvidence(&result, "optimizer.storage_metrics.fail_closed=false");
  AddEvidence(&result, "optimizer.storage_metrics.advisory_only=true");
  AddEvidence(&result, "optimizer.storage_metrics.finality_authority=false");
  AddEvidence(&result, "optimizer.storage_metrics.visibility_authority=false");
  AddEvidence(&result, "optimizer.storage_metrics.security_authority=false");
  AddEvidence(&result, "optimizer.storage_metrics.recovery_authority=false");

  Counter(&result, sample, "sb_optimizer_page_cache_hit_miss",
          "page_cache_hit_miss", "hit", sample.cache_hits, "storage_page");
  Counter(&result, sample, "sb_optimizer_page_cache_hit_miss",
          "page_cache_hit_miss", "miss", sample.cache_misses, "storage_page");
  Gauge(&result, sample, "sb_optimizer_page_count", "page_count",
        sample.page_count, "storage_page");
  Gauge(&result, sample, "sb_optimizer_page_cache_dirty_pressure",
        "page_cache_dirty_pressure",
        Ratio(sample.dirty_pages, std::max<std::uint64_t>(sample.resident_pages, 1)),
        "storage_page");
  Gauge(&result, sample, "sb_optimizer_page_cache_pin_pressure",
        "page_cache_pin_pressure",
        Ratio(sample.pinned_pages, std::max<std::uint64_t>(sample.resident_pages, 1)),
        "storage_page");
  Gauge(&result, sample, "sb_optimizer_writeback_pressure",
        "writeback_pressure",
        Ratio(sample.writeback_pages,
              std::max<std::uint64_t>(sample.resident_pages, 1)),
        "storage_page");
  Gauge(&result, sample, "sb_optimizer_prefetch_usefulness",
        "prefetch_usefulness",
        Ratio(sample.prefetch_used,
              std::max<std::uint64_t>(sample.prefetch_scheduled, 1)),
        "storage_prefetch");
  Histogram(&result, sample, "sb_optimizer_sequential_page_cost",
            "sequential_page_cost", sample.sequential_read_latency_microseconds,
            "storage_disk");
  Histogram(&result, sample, "sb_optimizer_random_page_cost",
            "random_page_cost", sample.random_read_latency_microseconds,
            "storage_disk");
  Gauge(&result, sample, "sb_optimizer_filespace_pressure",
        "filespace_pressure",
        Ratio(sample.filespace_used_bytes + sample.filespace_reserved_bytes,
              std::max<std::uint64_t>(sample.filespace_total_bytes, 1)),
        "storage_filespace");

  Push(&result, metrics::PublishPageCacheSnapshot(
                    sample.resident_pages,
                    *sample.resident_bytes,
                    sample.pinned_pages,
                    sample.dirty_pages,
                    sample.database_uuid, sample.filespace_uuid,
                    sample.page_family));
  Push(&result, metrics::PublishFilespaceCapacitySnapshot(
                    sample.filespace_total_bytes,
                    sample.filespace_used_bytes,
                    sample.filespace_free_bytes,
                    sample.database_uuid, sample.filespace_uuid,
                    sample.node_uuid, "primary", sample.device_profile));
  Push(&result, metrics::PublishFilespaceReservedBytes(
                    sample.filespace_reserved_bytes,
                    sample.database_uuid, sample.filespace_uuid,
                    sample.node_uuid, "primary", sample.device_profile,
                    "optimizer_costing"));
  Push(&result, metrics::ObserveFilespaceDeviceReadLatency(
                    static_cast<double>(
                        sample.sequential_read_latency_microseconds),
                    sample.database_uuid, sample.filespace_uuid,
                    sample.node_uuid, "primary", sample.device_profile));

  if (!result.ok && result.diagnostic_code == "SB_OPTIMIZER_STORAGE_METRICS.OK") {
    result.diagnostic_code =
        "SB_OPTIMIZER_STORAGE_METRICS.METRIC_PUBLISH_FAILED";
    result.detail = "optimizer.storage_metrics.metric_publish_failed";
  }
  return result;
}

}  // namespace scratchbird::storage::page
