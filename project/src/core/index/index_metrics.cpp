// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "index_metrics.hpp"

#include <algorithm>
#include <cctype>
#include <iomanip>
#include <map>
#include <sstream>
#include <string_view>
#include <tuple>
#include <utility>

namespace scratchbird::core::index {
namespace {
using scratchbird::core::metrics::DefaultMetricRegistry;
using scratchbird::core::metrics::MetricDescriptor;
using scratchbird::core::metrics::MetricLabelDescriptor;
using scratchbird::core::metrics::MetricLabelSet;
using scratchbird::core::metrics::MetricReadiness;
using scratchbird::core::metrics::MetricType;
using scratchbird::core::metrics::MetricUnit;
using scratchbird::core::metrics::MetricValidationResult;

MetricLabelSet Labels(const IndexMetricIdentity& identity) {
  return {{"index_uuid", identity.index_uuid},
          {"index_family", identity.index_family},
          {"semantic_profile", identity.semantic_profile_id},
          {"operation", identity.operation},
          {"result", identity.result},
          {"reason", identity.reason},
          {"page_family", identity.page_family},
          {"filespace_uuid", identity.filespace_uuid},
          {"agent_class", identity.agent_class}};
}

MetricDescriptor Descriptor(std::string family, MetricType type, MetricUnit unit, std::string help) {
  MetricDescriptor descriptor;
  descriptor.family = std::move(family);
  descriptor.type = type;
  descriptor.unit = unit;
  descriptor.namespace_path = "sys.metrics.indexes";
  descriptor.help = std::move(help);
  descriptor.producer_owner = "index_runtime";
  descriptor.security_family = "INDEX_METRICS";
  descriptor.readiness = MetricReadiness::implemented;
  descriptor.labels = {MetricLabelDescriptor{"index_uuid", true, false},
                       MetricLabelDescriptor{"index_family", true, false},
                       MetricLabelDescriptor{"semantic_profile", true, false},
                       MetricLabelDescriptor{"operation", true, false},
                       MetricLabelDescriptor{"result", true, false},
                       MetricLabelDescriptor{"reason", true, false},
                       MetricLabelDescriptor{"page_family", true, false},
                       MetricLabelDescriptor{"filespace_uuid", true, false},
                       MetricLabelDescriptor{"agent_class", true, false}};
  if (type == MetricType::histogram) {
    descriptor.histogram_buckets = {1, 10, 100, 1000, 10000, 100000, 1000000};
  }
  return descriptor;
}

MetricValidationResult RegisterIfMissing(MetricDescriptor descriptor) {
  auto& registry = DefaultMetricRegistry();
  if (registry.FindDescriptor(descriptor.family) != nullptr) {
    return scratchbird::core::metrics::MetricOk();
  }
  return registry.RegisterDescriptor(std::move(descriptor));
}

void Push(IndexMetricPublishResult* out, MetricValidationResult result) {
  if (!result.ok) {
    out->ok = false;
  }
  out->results.push_back(std::move(result));
}

void Counter(IndexMetricPublishResult* out, const std::string& family, const IndexMetricIdentity& id, double value) {
  if (value != 0) {
    Push(out, DefaultMetricRegistry().IncrementCounter(family, Labels(id), value, "index_runtime"));
  }
}

void Gauge(IndexMetricPublishResult* out, const std::string& family, const IndexMetricIdentity& id, double value) {
  Push(out, DefaultMetricRegistry().SetGauge(family, Labels(id), value, "index_runtime"));
}

constexpr const char* kCEIC040SearchKey =
    "CEIC_040_INDEX_OPERATION_METRICS_SUPPORT_BUNDLE";
constexpr const char* kCEIC040AuthorityScope =
    "index_operation_metrics.authority_scope=evidence_only_not_transaction_finality_visibility_authorization_security_recovery_parser_donor_wal_benchmark_clean_optimizer_plan_index_finality_provider_finality_local_cluster_cluster_action_or_agent_authority";
constexpr const char* kProtectedExcluded = "<protected-material-excluded>";
constexpr const char* kTruncatedSuffix = "...<truncated>";

bool AnyForbiddenAuthority(const IndexOperationMetricAuthorityBoundary& boundary) {
  return boundary.transaction_finality_authority ||
         boundary.visibility_authority ||
         boundary.authorization_security_authority ||
         boundary.security_authority ||
         boundary.recovery_authority ||
         boundary.parser_authority ||
         boundary.donor_authority ||
         boundary.wal_authority ||
         boundary.benchmark_authority ||
         boundary.benchmark_clean_authority ||
         boundary.optimizer_plan_authority ||
         boundary.optimizer_plan_finality_authority ||
         boundary.index_finality_authority ||
         boundary.provider_finality_authority ||
         boundary.local_cluster_authority ||
         boundary.cluster_authority ||
         boundary.cluster_action_authority ||
         boundary.agent_action_authority;
}

bool AnySuccessorOverclaim(const IndexOperationMetricSample& sample) {
  return sample.ceic_041_crash_matrix_claimed ||
         sample.ceic_042_readiness_drift_claimed ||
         sample.ceic_090_integrated_metrics_coverage_claimed ||
         sample.ceic_091_integrated_support_bundle_claimed ||
         sample.donor_dominance_claimed ||
         sample.all_index_readiness_claimed ||
         sample.enterprise_readiness_claimed;
}

bool AnySuccessorOverclaim(
    const IndexOperationMetricSupportBundleRequest& request) {
  return request.ceic_041_crash_matrix_claimed ||
         request.ceic_042_readiness_drift_claimed ||
         request.ceic_090_integrated_metrics_coverage_claimed ||
         request.ceic_091_integrated_support_bundle_claimed ||
         request.donor_dominance_claimed ||
         request.all_index_readiness_claimed ||
         request.enterprise_readiness_claimed;
}

bool PlaceholderGeneration(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char ch) {
                   return static_cast<char>(std::tolower(ch));
                 });
  return value.empty() || value == "0" ||
         value.find("placeholder") != std::string::npos ||
         value.find("static") != std::string::npos ||
         value.find("stale") != std::string::npos ||
         value.find("missing") != std::string::npos;
}

bool LooksProtected(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char ch) {
                   return static_cast<char>(std::tolower(ch));
                 });
  const char* needles[] = {"secret", "password", "passwd", "pwd=", "token",
                           "private_key", "credential", "verifier", "seed",
                           "cleartext", "plaintext", "api_key", "apikey",
                           "key_material", "raw_key", "kms_plaintext",
                           "bearer ", "hsm", "kms", "protected_reference"};
  for (const char* needle : needles) {
    if (value.find(needle) != std::string::npos) {
      return true;
    }
  }
  return false;
}

std::string TruncateForBundle(std::string value, u64 max_bytes) {
  if (max_bytes == 0) {
    return {};
  }
  if (value.size() <= max_bytes) {
    return value;
  }
  const std::string suffix = kTruncatedSuffix;
  if (max_bytes <= suffix.size()) {
    return value.substr(0, static_cast<std::size_t>(max_bytes));
  }
  value.resize(static_cast<std::size_t>(max_bytes - suffix.size()));
  value.append(suffix);
  return value;
}

std::string RedactForBundle(std::string value, bool* redacted) {
  if (LooksProtected(value)) {
    if (redacted != nullptr) {
      *redacted = true;
    }
    return kProtectedExcluded;
  }
  return value;
}

std::string EvidenceDigest(std::string_view key,
                           std::string_view value,
                           std::string_view labels) {
  u64 hash = 1469598103934665603ull;
  auto mix = [&hash](std::string_view text) {
    for (unsigned char ch : text) {
      hash ^= static_cast<u64>(ch);
      hash *= 1099511628211ull;
    }
  };
  mix(kCEIC040SearchKey);
  mix(key);
  mix(value);
  mix(labels);
  std::ostringstream out;
  out << "fnv64-ceic040-v1:" << std::hex << std::setw(16)
      << std::setfill('0') << hash;
  return out.str();
}

MetricDescriptor OperationDescriptor(std::string family, std::string help) {
  MetricDescriptor descriptor;
  descriptor.family = std::move(family);
  descriptor.type = MetricType::counter;
  descriptor.unit = MetricUnit::count;
  descriptor.namespace_path = "sys.metrics.indexes.operations";
  descriptor.help = std::move(help);
  descriptor.producer_owner = "index_operation_runtime";
  descriptor.security_family = "INDEX_OPERATION_METRICS";
  descriptor.readiness = MetricReadiness::implemented;
  descriptor.labels = {
      MetricLabelDescriptor{"index_uuid", true, false},
      MetricLabelDescriptor{"index_family", true, false},
      MetricLabelDescriptor{"route_kind", true, false},
      MetricLabelDescriptor{"operation", true, false},
      MetricLabelDescriptor{"result", true, false},
      MetricLabelDescriptor{"reason", true, false},
      MetricLabelDescriptor{"index_generation", true, false},
      MetricLabelDescriptor{"route_generation", true, false},
      MetricLabelDescriptor{"source_generation", true, false},
      MetricLabelDescriptor{"freshness_microseconds", true, false},
      MetricLabelDescriptor{"source_kind", true, false},
      MetricLabelDescriptor{"provenance", true, false},
      MetricLabelDescriptor{"evidence_digest", true, true},
      MetricLabelDescriptor{"authority_scope", true, false}};
  return descriptor;
}

MetricLabelSet OperationLabels(const IndexOperationMetricSample& sample,
                               IndexOperationCounterKind kind) {
  const auto& id = sample.identity;
  return {{"index_uuid", id.index_uuid},
          {"index_family", id.index_family},
          {"route_kind", id.route_kind},
          {"operation", IndexOperationCounterName(kind)},
          {"result", id.result},
          {"reason", id.reason},
          {"index_generation", id.index_generation},
          {"route_generation", std::to_string(id.route_generation)},
          {"source_generation", std::to_string(id.source_generation)},
          {"freshness_microseconds", std::to_string(id.freshness_microseconds)},
          {"source_kind", id.source_kind},
          {"provenance", id.provenance},
          {"evidence_digest", id.evidence_digest},
          {"authority_scope", "evidence_only"}};
}

u64 CounterValue(const IndexOperationMetricCounters& counters,
                 IndexOperationCounterKind kind) {
  switch (kind) {
    case IndexOperationCounterKind::probe:
      return counters.probe;
    case IndexOperationCounterKind::insert:
      return counters.insert;
    case IndexOperationCounterKind::remove:
      return counters.remove;
    case IndexOperationCounterKind::split:
      return counters.split;
    case IndexOperationCounterKind::merge:
      return counters.merge;
    case IndexOperationCounterKind::collision:
      return counters.collision;
    case IndexOperationCounterKind::recheck:
      return counters.recheck;
    case IndexOperationCounterKind::false_positive:
      return counters.false_positive;
    case IndexOperationCounterKind::cleanup:
      return counters.cleanup;
    case IndexOperationCounterKind::corruption:
      return counters.corruption;
    case IndexOperationCounterKind::recovery:
      return counters.recovery;
    case IndexOperationCounterKind::benchmark:
      return counters.benchmark;
  }
  return 0;
}

bool AnyCounter(const IndexOperationMetricCounters& counters) {
  for (const auto kind : RequiredIndexOperationCounterKinds()) {
    if (CounterValue(counters, kind) != 0) {
      return true;
    }
  }
  return false;
}

IndexOperationMetricPublishResult RefuseOperationMetric(
    const IndexOperationMetricSample& sample,
    std::string code,
    std::string detail) {
  IndexOperationMetricPublishResult result;
  result.ok = false;
  result.diagnostic_code = std::move(code);
  result.detail = std::move(detail);
  result.evidence.push_back(kCEIC040SearchKey);
  result.evidence.push_back(kCEIC040AuthorityScope);
  result.evidence.push_back("index_operation_metrics.fail_closed=true");
  result.evidence.push_back("index_operation_metrics.index_uuid=" +
                            sample.identity.index_uuid);
  result.evidence.push_back("index_operation_metrics.index_family=" +
                            sample.identity.index_family);
  result.evidence.push_back("index_operation_metrics.refused=" +
                            result.diagnostic_code);
  return result;
}

std::string LabelValue(const MetricLabelSet& labels, const std::string& key) {
  for (const auto& label : labels) {
    if (label.key == key) {
      return label.value;
    }
  }
  return {};
}

bool OperationCounterFamily(const std::string& family) {
  for (const auto kind : RequiredIndexOperationCounterKinds()) {
    if (family == IndexOperationCounterFamily(kind)) {
      return true;
    }
  }
  return false;
}

u64 ParseU64(std::string value, bool* ok) {
  if (ok != nullptr) {
    *ok = false;
  }
  if (value.empty()) {
    return 0;
  }
  u64 output = 0;
  for (char ch : value) {
    if (ch < '0' || ch > '9') {
      return 0;
    }
    output = output * 10 + static_cast<u64>(ch - '0');
  }
  if (ok != nullptr) {
    *ok = true;
  }
  return output;
}

std::string RedactedLabelString(const MetricLabelSet& labels,
                                bool* redacted,
                                u64 max_label_bytes) {
  std::vector<std::pair<std::string, std::string>> sorted;
  for (const auto& label : labels) {
    sorted.push_back({label.key, RedactForBundle(label.value, redacted)});
  }
  std::sort(sorted.begin(), sorted.end());
  std::ostringstream out;
  for (std::size_t i = 0; i < sorted.size(); ++i) {
    if (i != 0) {
      out << ';';
    }
    out << sorted[i].first << '=' << sorted[i].second;
  }
  return TruncateForBundle(out.str(), max_label_bytes);
}

u64 RowSizeEstimate(const IndexOperationMetricSupportBundleRow& row) {
  return static_cast<u64>(row.key.size() + row.value.size() +
                          row.labels.size() + 128);
}

bool BoundedAppend(IndexOperationMetricSupportBundleResult* result,
                   IndexOperationMetricSupportBundleRow row,
                   const IndexOperationMetricSupportBundleLimits& limits) {
  const u64 row_bytes = RowSizeEstimate(row);
  if (result->rows.size() >= limits.max_rows ||
      result->output_bytes > limits.max_output_bytes ||
      row_bytes > limits.max_output_bytes - result->output_bytes) {
    ++result->dropped_row_count;
    return false;
  }
  result->output_bytes += row_bytes;
  result->rows.push_back(std::move(row));
  return true;
}

IndexOperationMetricSupportBundleResult RefuseSupportBundle(
    std::string code,
    std::string detail) {
  IndexOperationMetricSupportBundleResult result;
  result.ok = false;
  result.diagnostic_code = std::move(code);
  result.detail = std::move(detail);
  result.evidence.push_back(kCEIC040SearchKey);
  result.evidence.push_back(kCEIC040AuthorityScope);
  result.evidence.push_back("index_operation_support_bundle.fail_closed=true");
  result.evidence.push_back("index_operation_support_bundle.refused=" +
                            result.diagnostic_code);
  return result;
}
}  // namespace

MetricValidationResult EnsureIndexMetricDescriptors() {
  const MetricDescriptor descriptors[] = {
      Descriptor("sb_index_candidates_total", MetricType::counter, MetricUnit::count, "Index candidates emitted."),
      Descriptor("sb_index_visible_candidates_total", MetricType::counter, MetricUnit::count, "Index candidates accepted after authority recheck."),
      Descriptor("sb_index_rechecks_total", MetricType::counter, MetricUnit::count, "Index candidate rechecks."),
      Descriptor("sb_index_fallback_sorts_total", MetricType::counter, MetricUnit::count, "Fallback sorts required after index access."),
      Descriptor("sb_index_pages_read_total", MetricType::counter, MetricUnit::count, "Index pages read."),
      Descriptor("sb_index_pages_written_total", MetricType::counter, MetricUnit::count, "Index pages written."),
      Descriptor("sb_index_splits_observed_total", MetricType::counter, MetricUnit::count, "Index split events observed by family runtime."),
      Descriptor("sb_index_merges_observed_total", MetricType::counter, MetricUnit::count, "Index merge events observed by family runtime."),
      Descriptor("sb_index_depth", MetricType::gauge, MetricUnit::count, "Index depth."),
      Descriptor("sb_index_density_ratio", MetricType::gauge, MetricUnit::ratio, "Index density ratio."),
      Descriptor("sb_index_fragmentation_ratio", MetricType::gauge, MetricUnit::ratio, "Index fragmentation ratio."),
      Descriptor("sb_index_maintenance_operations_total", MetricType::counter, MetricUnit::count, "Index maintenance operations."),
      Descriptor("sb_index_verify_failures_total", MetricType::counter, MetricUnit::count, "Index verification failures."),
      Descriptor("sb_index_repair_actions_total", MetricType::counter, MetricUnit::count, "Index repair actions."),
      Descriptor("sb_index_stale_resources_total", MetricType::counter, MetricUnit::count, "Index stale resource detections."),
      Descriptor("sb_index_quarantine_events_total", MetricType::counter, MetricUnit::count, "Index quarantine events."),
      Descriptor("sb_index_maintenance_progress_percent", MetricType::gauge, MetricUnit::percent, "Index maintenance progress percent."),
      Descriptor("sb_index_optimizer_estimate_error_ratio", MetricType::gauge, MetricUnit::ratio, "Index optimizer estimate error ratio."),
      Descriptor("sb_index_optimizer_stale_stats_total", MetricType::counter, MetricUnit::count, "Index optimizer stale-stat detections."),
      Descriptor("sb_index_optimizer_invalidations_total", MetricType::counter, MetricUnit::count, "Index optimizer invalidations."),
      Descriptor("sb_index_optimizer_fallback_refusals_total", MetricType::counter, MetricUnit::count, "Index optimizer fallback/refusal events."),
      Descriptor("sb_index_donor_profile_hits_total", MetricType::counter, MetricUnit::count, "Donor semantic profile hits."),
      Descriptor("sb_index_donor_profile_refusals_total", MetricType::counter, MetricUnit::count, "Donor semantic profile refusals."),
      Descriptor("sb_index_donor_rechecks_total", MetricType::counter, MetricUnit::count, "Donor semantic profile rechecks."),
      Descriptor("sb_index_donor_fallback_sorts_total", MetricType::counter, MetricUnit::count, "Donor semantic profile fallback sorts."),
      Descriptor("sb_index_donor_order_proofs_total", MetricType::counter, MetricUnit::count, "Donor semantic order proofs."),
      Descriptor("sb_index_donor_catalog_projections_total", MetricType::counter, MetricUnit::count, "Donor catalog projection events."),
      Descriptor("sb_index_donor_compatibility_diagnostics_total", MetricType::counter, MetricUnit::count, "Donor compatibility diagnostics."),
      Descriptor("sb_index_resident_bytes", MetricType::gauge, MetricUnit::bytes, "Index resident bytes."),
      Descriptor("sb_index_residency_hits_total", MetricType::counter, MetricUnit::count, "Index residency hits."),
      Descriptor("sb_index_residency_misses_total", MetricType::counter, MetricUnit::count, "Index residency misses."),
      Descriptor("sb_index_residency_evictions_total", MetricType::counter, MetricUnit::count, "Index residency evictions."),
      Descriptor("sb_index_residency_pressure_score", MetricType::gauge, MetricUnit::count, "Index residency pressure score."),
      Descriptor("sb_index_residency_degraded_total", MetricType::counter, MetricUnit::count, "Index residency degraded decisions."),
      Descriptor("sb_index_residency_refused_total", MetricType::counter, MetricUnit::count, "Index residency refused decisions."),
      Descriptor("sb_index_page_allocation_requests_total", MetricType::counter, MetricUnit::count, "Index page allocation requests."),
      Descriptor("sb_index_page_relocation_requests_total", MetricType::counter, MetricUnit::count, "Index page relocation requests."),
      Descriptor("sb_index_filespace_shrink_ready_bytes", MetricType::gauge, MetricUnit::bytes, "Index bytes ready for filespace shrink.")};
  for (const auto& descriptor : descriptors) {
    const auto result = RegisterIfMissing(descriptor);
    if (!result.ok) {
      return result;
    }
  }
  return scratchbird::core::metrics::MetricOk();
}

IndexMetricPublishResult PublishIndexLogicalMetrics(const IndexMetricIdentity& identity,
                                                    const IndexLogicalMetricDelta& delta) {
  IndexMetricPublishResult out;
  Push(&out, EnsureIndexMetricDescriptors());
  Counter(&out, "sb_index_candidates_total", identity, delta.candidates);
  Counter(&out, "sb_index_visible_candidates_total", identity, delta.visible);
  Counter(&out, "sb_index_rechecks_total", identity, delta.rechecks);
  Counter(&out, "sb_index_fallback_sorts_total", identity, delta.fallback_sorts);
  return out;
}

IndexMetricPublishResult PublishIndexPhysicalMetrics(const IndexMetricIdentity& identity,
                                                     const IndexPhysicalMetricDelta& delta) {
  IndexMetricPublishResult out;
  Push(&out, EnsureIndexMetricDescriptors());
  Counter(&out, "sb_index_pages_read_total", identity, delta.pages_read);
  Counter(&out, "sb_index_pages_written_total", identity, delta.pages_written);
  Counter(&out, "sb_index_splits_observed_total", identity, delta.splits);
  Counter(&out, "sb_index_merges_observed_total", identity, delta.merges);
  Gauge(&out, "sb_index_depth", identity, delta.depth);
  Gauge(&out, "sb_index_density_ratio", identity, delta.density_ratio);
  Gauge(&out, "sb_index_fragmentation_ratio", identity, delta.fragmentation_ratio);
  return out;
}

IndexMetricPublishResult PublishIndexMaintenanceMetrics(const IndexMetricIdentity& identity,
                                                        const IndexMaintenanceMetricDelta& delta) {
  IndexMetricPublishResult out;
  Push(&out, EnsureIndexMetricDescriptors());
  Counter(&out, "sb_index_maintenance_operations_total", identity, delta.operations);
  Counter(&out, "sb_index_verify_failures_total", identity, delta.verify_failures);
  Counter(&out, "sb_index_repair_actions_total", identity, delta.repair_actions);
  Counter(&out, "sb_index_stale_resources_total", identity, delta.stale_resources);
  Counter(&out, "sb_index_quarantine_events_total", identity, delta.quarantine_events);
  Gauge(&out, "sb_index_maintenance_progress_percent", identity, delta.progress_percent);
  return out;
}

IndexMetricPublishResult PublishIndexOptimizerMetrics(const IndexMetricIdentity& identity,
                                                      const IndexOptimizerMetricDelta& delta) {
  IndexMetricPublishResult out;
  Push(&out, EnsureIndexMetricDescriptors());
  Gauge(&out, "sb_index_optimizer_estimate_error_ratio", identity, delta.estimate_error_ratio);
  Counter(&out, "sb_index_optimizer_stale_stats_total", identity, delta.stale_stats);
  Counter(&out, "sb_index_optimizer_invalidations_total", identity, delta.invalidations);
  Counter(&out, "sb_index_optimizer_fallback_refusals_total", identity, delta.fallback_refusals);
  return out;
}

IndexMetricPublishResult PublishIndexDonorProfileMetrics(const IndexMetricIdentity& identity,
                                                         const IndexDonorProfileMetricDelta& delta) {
  IndexMetricPublishResult out;
  Push(&out, EnsureIndexMetricDescriptors());
  Counter(&out, "sb_index_donor_profile_hits_total", identity, delta.profile_hits);
  Counter(&out, "sb_index_donor_profile_refusals_total", identity, delta.profile_refusals);
  Counter(&out, "sb_index_donor_rechecks_total", identity, delta.rechecks);
  Counter(&out, "sb_index_donor_fallback_sorts_total", identity, delta.fallback_sorts);
  Counter(&out, "sb_index_donor_order_proofs_total", identity, delta.order_proofs);
  Counter(&out, "sb_index_donor_catalog_projections_total", identity, delta.catalog_projections);
  Counter(&out, "sb_index_donor_compatibility_diagnostics_total", identity, delta.compatibility_diagnostics);
  return out;
}

IndexMetricPublishResult PublishIndexResidencyMetrics(const IndexMetricIdentity& identity,
                                                      const IndexResidencyMetricDelta& delta) {
  IndexMetricPublishResult out;
  Push(&out, EnsureIndexMetricDescriptors());
  Gauge(&out, "sb_index_resident_bytes", identity, delta.resident_bytes);
  Counter(&out, "sb_index_residency_hits_total", identity, delta.hits);
  Counter(&out, "sb_index_residency_misses_total", identity, delta.misses);
  Counter(&out, "sb_index_residency_evictions_total", identity, delta.evictions);
  Gauge(&out, "sb_index_residency_pressure_score", identity, delta.pressure_score);
  Counter(&out, "sb_index_residency_degraded_total", identity, delta.degraded);
  Counter(&out, "sb_index_residency_refused_total", identity, delta.refused);
  return out;
}

IndexMetricPublishResult PublishIndexPageFilespaceMetrics(const IndexMetricIdentity& identity,
                                                          const IndexPageFilespaceMetricDelta& delta) {
  IndexMetricPublishResult out;
  Push(&out, EnsureIndexMetricDescriptors());
  Counter(&out, "sb_index_page_allocation_requests_total", identity, delta.allocation_requests);
  Counter(&out, "sb_index_page_relocation_requests_total", identity, delta.relocation_requests);
  Gauge(&out, "sb_index_filespace_shrink_ready_bytes", identity, delta.shrink_ready_bytes);
  return out;
}

const char* IndexOperationCounterName(IndexOperationCounterKind kind) {
  switch (kind) {
    case IndexOperationCounterKind::probe:
      return "probe";
    case IndexOperationCounterKind::insert:
      return "insert";
    case IndexOperationCounterKind::remove:
      return "delete";
    case IndexOperationCounterKind::split:
      return "split";
    case IndexOperationCounterKind::merge:
      return "merge";
    case IndexOperationCounterKind::collision:
      return "collision";
    case IndexOperationCounterKind::recheck:
      return "recheck";
    case IndexOperationCounterKind::false_positive:
      return "false_positive";
    case IndexOperationCounterKind::cleanup:
      return "cleanup";
    case IndexOperationCounterKind::corruption:
      return "corruption";
    case IndexOperationCounterKind::recovery:
      return "recovery";
    case IndexOperationCounterKind::benchmark:
      return "benchmark";
  }
  return "unknown";
}

const char* IndexOperationCounterFamily(IndexOperationCounterKind kind) {
  switch (kind) {
    case IndexOperationCounterKind::probe:
      return "sb_index_operation_probe_total";
    case IndexOperationCounterKind::insert:
      return "sb_index_operation_insert_total";
    case IndexOperationCounterKind::remove:
      return "sb_index_operation_delete_total";
    case IndexOperationCounterKind::split:
      return "sb_index_operation_split_total";
    case IndexOperationCounterKind::merge:
      return "sb_index_operation_merge_total";
    case IndexOperationCounterKind::collision:
      return "sb_index_operation_collision_total";
    case IndexOperationCounterKind::recheck:
      return "sb_index_operation_recheck_total";
    case IndexOperationCounterKind::false_positive:
      return "sb_index_operation_false_positive_total";
    case IndexOperationCounterKind::cleanup:
      return "sb_index_operation_cleanup_total";
    case IndexOperationCounterKind::corruption:
      return "sb_index_operation_corruption_total";
    case IndexOperationCounterKind::recovery:
      return "sb_index_operation_recovery_total";
    case IndexOperationCounterKind::benchmark:
      return "sb_index_operation_benchmark_total";
  }
  return "sb_index_operation_unknown_total";
}

const std::vector<IndexOperationCounterKind>&
RequiredIndexOperationCounterKinds() {
  static const std::vector<IndexOperationCounterKind> kinds = {
      IndexOperationCounterKind::probe,
      IndexOperationCounterKind::insert,
      IndexOperationCounterKind::remove,
      IndexOperationCounterKind::split,
      IndexOperationCounterKind::merge,
      IndexOperationCounterKind::collision,
      IndexOperationCounterKind::recheck,
      IndexOperationCounterKind::false_positive,
      IndexOperationCounterKind::cleanup,
      IndexOperationCounterKind::corruption,
      IndexOperationCounterKind::recovery,
      IndexOperationCounterKind::benchmark};
  return kinds;
}

MetricValidationResult EnsureIndexOperationMetricDescriptors(
    scratchbird::core::metrics::MetricRegistry* registry) {
  auto& target = registry == nullptr ? DefaultMetricRegistry() : *registry;
  for (const auto kind : RequiredIndexOperationCounterKinds()) {
    if (target.FindDescriptor(IndexOperationCounterFamily(kind)) != nullptr) {
      continue;
    }
    const std::string operation = IndexOperationCounterName(kind);
    const auto result = target.RegisterDescriptor(OperationDescriptor(
        IndexOperationCounterFamily(kind),
        "CEIC-040 evidence-only index operation counter for " + operation +
            " operations."));
    if (!result.ok) {
      return result;
    }
  }
  return scratchbird::core::metrics::MetricOk();
}

IndexOperationMetricPublishResult PublishIndexOperationMetrics(
    const IndexOperationMetricSample& sample) {
  const auto& id = sample.identity;
  if (id.index_uuid.empty() || id.index_family.empty() ||
      id.route_kind.empty() || id.result.empty() || id.reason.empty() ||
      id.source_kind.empty() || id.provenance.empty() ||
      id.evidence_digest.empty()) {
    return RefuseOperationMetric(
        sample,
        "SB_INDEX_OPERATION_METRICS.MISSING_IDENTITY",
        "index.operation_metrics.required_identity_missing");
  }
  if (PlaceholderGeneration(id.index_generation) || id.route_generation == 0 ||
      id.source_generation == 0) {
    return RefuseOperationMetric(
        sample,
        "SB_INDEX_OPERATION_METRICS.GENERATION_REQUIRED",
        "index.operation_metrics.generation_evidence_required");
  }
  if (id.freshness_microseconds > id.max_freshness_microseconds) {
    return RefuseOperationMetric(sample,
                                 "SB_INDEX_OPERATION_METRICS.STALE",
                                 "index.operation_metrics.stale_source");
  }
  if (!sample.runtime_operation_path ||
      sample.descriptor_only_static_evidence ||
      !sample.support_bundle_row_producer) {
    return RefuseOperationMetric(
        sample,
        "SB_INDEX_OPERATION_METRICS.RUNTIME_PRODUCER_REQUIRED",
        "index.operation_metrics.descriptor_only_static_evidence_refused");
  }
  if (sample.protected_material_present) {
    return RefuseOperationMetric(
        sample,
        "SB_INDEX_OPERATION_METRICS.PROTECTED_MATERIAL_FORBIDDEN",
        "index.operation_metrics.protected_material_forbidden");
  }
  if (sample.local_cluster_participation ||
      sample.cluster_participation_requested) {
    return RefuseOperationMetric(
        sample,
        "SB_INDEX_OPERATION_METRICS.CLUSTER_PARTICIPATION_REFUSED",
        "index.operation_metrics.local_cluster_participation_fail_closed");
  }
  if (AnyForbiddenAuthority(sample.authority_boundary)) {
    return RefuseOperationMetric(
        sample,
        "SB_INDEX_OPERATION_METRICS.FORBIDDEN_AUTHORITY",
        "index.operation_metrics.forbidden_authority_claim");
  }
  if (AnySuccessorOverclaim(sample)) {
    return RefuseOperationMetric(
        sample,
        "SB_INDEX_OPERATION_METRICS.SUCCESSOR_OVERCLAIM",
        "index.operation_metrics.successor_or_readiness_overclaim");
  }
  if (!AnyCounter(sample.counters)) {
    return RefuseOperationMetric(
        sample,
        "SB_INDEX_OPERATION_METRICS.OPERATION_COUNTER_REQUIRED",
        "index.operation_metrics.operation_counter_required");
  }

  IndexOperationMetricPublishResult result;
  result.ok = true;
  result.diagnostic_code = "SB_INDEX_OPERATION_METRICS.OK";
  result.evidence.push_back(kCEIC040SearchKey);
  result.evidence.push_back(kCEIC040AuthorityScope);
  result.evidence.push_back("index_operation_metrics.fail_closed=false");
  result.evidence.push_back("index_operation_metrics.runtime_operation_path=true");
  result.evidence.push_back("index_operation_metrics.support_bundle_row_producer=true");
  result.evidence.push_back("index_operation_metrics.descriptor_only_static_evidence=false");
  result.evidence.push_back("index_operation_metrics.ceic_041_crash_matrix_claimed=false");
  result.evidence.push_back("index_operation_metrics.ceic_042_readiness_drift_claimed=false");
  result.evidence.push_back("index_operation_metrics.ceic_090_integrated_metrics_coverage_claimed=false");
  result.evidence.push_back("index_operation_metrics.ceic_091_integrated_support_bundle_claimed=false");
  result.evidence.push_back("index_operation_metrics.donor_dominance_claimed=false");
  result.evidence.push_back("index_operation_metrics.enterprise_readiness_claimed=false");

  result.metric_results.push_back(EnsureIndexOperationMetricDescriptors());
  if (!result.metric_results.back().ok) {
    result.ok = false;
  }
  for (const auto kind : RequiredIndexOperationCounterKinds()) {
    const u64 value = CounterValue(sample.counters, kind);
    if (value == 0) {
      continue;
    }
    const std::string family = IndexOperationCounterFamily(kind);
    auto metric_result = DefaultMetricRegistry().IncrementCounter(
        family,
        OperationLabels(sample, kind),
        static_cast<double>(value),
        "index_operation_runtime");
    if (!metric_result.ok) {
      result.ok = false;
      if (result.diagnostic_code == "SB_INDEX_OPERATION_METRICS.OK") {
        result.diagnostic_code =
            "SB_INDEX_OPERATION_METRICS.METRIC_PUBLISH_FAILED";
        result.detail = metric_result.detail;
      }
    } else {
      result.emitted_counter_families.push_back(family);
    }
    result.metric_results.push_back(std::move(metric_result));
  }
  return result;
}

IndexOperationMetricSupportBundleResult BuildIndexOperationMetricSupportBundle(
    IndexOperationMetricSupportBundleRequest request) {
  if (request.limits.max_rows == 0) {
    request.limits.max_rows = 1;
  }
  if (request.limits.max_output_bytes == 0) {
    request.limits.max_output_bytes = 1;
  }
  if (request.limits.max_key_bytes == 0) {
    request.limits.max_key_bytes = 1;
  }
  if (request.limits.max_value_bytes == 0) {
    request.limits.max_value_bytes = 1;
  }
  if (request.limits.max_label_bytes == 0) {
    request.limits.max_label_bytes = 1;
  }

  if (request.descriptor_only_static_evidence) {
    return RefuseSupportBundle(
        "SB_INDEX_OPERATION_SUPPORT_BUNDLE.RUNTIME_ROWS_REQUIRED",
        "index.operation_support_bundle.descriptor_only_static_evidence_refused");
  }
  if (request.local_cluster_participation ||
      request.cluster_participation_requested) {
    return RefuseSupportBundle(
        "SB_INDEX_OPERATION_SUPPORT_BUNDLE.CLUSTER_PARTICIPATION_REFUSED",
        "index.operation_support_bundle.local_cluster_participation_fail_closed");
  }
  if (AnyForbiddenAuthority(request.authority_boundary)) {
    return RefuseSupportBundle(
        "SB_INDEX_OPERATION_SUPPORT_BUNDLE.FORBIDDEN_AUTHORITY",
        "index.operation_support_bundle.forbidden_authority_claim");
  }
  if (AnySuccessorOverclaim(request)) {
    return RefuseSupportBundle(
        "SB_INDEX_OPERATION_SUPPORT_BUNDLE.SUCCESSOR_OVERCLAIM",
        "index.operation_support_bundle.successor_or_readiness_overclaim");
  }

  if (request.metrics.empty()) {
    request.metrics = DefaultMetricRegistry().SnapshotCurrent(false);
  }
  if (request.metrics.empty()) {
    return RefuseSupportBundle(
        "SB_INDEX_OPERATION_SUPPORT_BUNDLE.RUNTIME_ROWS_REQUIRED",
        "index.operation_support_bundle.runtime_metric_rows_required");
  }

  std::sort(request.metrics.begin(),
            request.metrics.end(),
            [](const MetricValue& left, const MetricValue& right) {
              const auto left_labels = RedactedLabelString(left.labels, nullptr, 4096);
              const auto right_labels = RedactedLabelString(right.labels, nullptr, 4096);
              return std::tie(left.family, left_labels) <
                     std::tie(right.family, right_labels);
            });

  IndexOperationMetricSupportBundleResult result;
  result.ok = true;
  result.diagnostic_code = "SB_INDEX_OPERATION_SUPPORT_BUNDLE.OK";
  result.row_limit = request.limits.max_rows;
  result.evidence.push_back(kCEIC040SearchKey);
  result.evidence.push_back(kCEIC040AuthorityScope);
  result.evidence.push_back("index_operation_support_bundle.fail_closed=false");
  result.evidence.push_back("index_operation_support_bundle.bounded=true");
  result.evidence.push_back("index_operation_support_bundle.protected_material_excluded=true");
  result.evidence.push_back("index_operation_support_bundle.deterministic_labels=true");
  result.evidence.push_back("index_operation_support_bundle.ceic_041_crash_matrix_claimed=false");
  result.evidence.push_back("index_operation_support_bundle.ceic_042_readiness_drift_claimed=false");
  result.evidence.push_back("index_operation_support_bundle.ceic_090_integrated_metrics_coverage_claimed=false");
  result.evidence.push_back("index_operation_support_bundle.ceic_091_integrated_support_bundle_claimed=false");
  result.evidence.push_back("index_operation_support_bundle.enterprise_readiness_claimed=false");

  std::map<std::string, bool> emitted_required;
  for (const auto kind : RequiredIndexOperationCounterKinds()) {
    emitted_required[IndexOperationCounterFamily(kind)] = false;
  }

  for (const auto& metric : request.metrics) {
    if (!OperationCounterFamily(metric.family)) {
      continue;
    }
    const std::string raw_index_uuid = LabelValue(metric.labels, "index_uuid");
    if (!request.filter_index_uuid.empty() &&
        raw_index_uuid != request.filter_index_uuid) {
      continue;
    }
    const std::string index_family = LabelValue(metric.labels, "index_family");
    const std::string route_kind = LabelValue(metric.labels, "route_kind");
    const std::string operation = LabelValue(metric.labels, "operation");
    const std::string index_generation =
        LabelValue(metric.labels, "index_generation");
    const std::string route_generation =
        LabelValue(metric.labels, "route_generation");
    const std::string source_generation =
        LabelValue(metric.labels, "source_generation");
    const std::string source_kind = LabelValue(metric.labels, "source_kind");
    const std::string provenance = LabelValue(metric.labels, "provenance");
    const std::string evidence_digest =
        LabelValue(metric.labels, "evidence_digest");
    const std::string freshness =
        LabelValue(metric.labels, "freshness_microseconds");

    bool parsed_freshness = false;
    const u64 freshness_value = ParseU64(freshness, &parsed_freshness);
    if (raw_index_uuid.empty() || index_family.empty() || route_kind.empty() ||
        operation.empty() || PlaceholderGeneration(index_generation) ||
        PlaceholderGeneration(route_generation) ||
        PlaceholderGeneration(source_generation) || source_kind.empty() ||
        provenance.empty() || evidence_digest.empty() || !parsed_freshness) {
      return RefuseSupportBundle(
          "SB_INDEX_OPERATION_SUPPORT_BUNDLE.GENERATION_EVIDENCE_REQUIRED",
          "index.operation_support_bundle.route_family_generation_source_provenance_required");
    }
    if (source_kind == "static_descriptor" ||
        source_kind == "descriptor_only") {
      return RefuseSupportBundle(
          "SB_INDEX_OPERATION_SUPPORT_BUNDLE.RUNTIME_ROWS_REQUIRED",
          "index.operation_support_bundle.static_source_refused");
    }
    if (freshness_value > 60000000) {
      return RefuseSupportBundle(
          "SB_INDEX_OPERATION_SUPPORT_BUNDLE.STALE",
          "index.operation_support_bundle.stale_source");
    }

    bool redacted = false;
    const std::string labels =
        RedactedLabelString(metric.labels, &redacted, request.limits.max_label_bytes);
    std::ostringstream value;
    value << "metric_value=" << metric.value
          << ";metric_type=counter"
          << ";operation=" << operation
          << ";result=" << RedactForBundle(LabelValue(metric.labels, "result"), &redacted)
          << ";reason=" << RedactForBundle(LabelValue(metric.labels, "reason"), &redacted)
          << ";freshness_microseconds=" << freshness_value
          << ";source_kind=" << RedactForBundle(source_kind, &redacted)
          << ";provenance=" << RedactForBundle(provenance, &redacted)
          << ";authority_scope=evidence_only";

    IndexOperationMetricSupportBundleRow row;
    row.key = TruncateForBundle("index.operation_metric." + metric.family,
                                request.limits.max_key_bytes);
    row.value = TruncateForBundle(value.str(), request.limits.max_value_bytes);
    row.metric_family = metric.family;
    row.labels = labels;
    row.index_uuid = RedactForBundle(raw_index_uuid, &redacted);
    row.index_family = index_family;
    row.route_kind = route_kind;
    row.operation = operation;
    row.result = RedactForBundle(LabelValue(metric.labels, "result"), &redacted);
    row.reason = RedactForBundle(LabelValue(metric.labels, "reason"), &redacted);
    row.index_generation = index_generation;
    row.route_generation = route_generation;
    row.source_generation = source_generation;
    row.source_kind = RedactForBundle(source_kind, &redacted);
    row.provenance = RedactForBundle(provenance, &redacted);
    row.evidence_digest = RedactForBundle(evidence_digest, &redacted);
    row.freshness_microseconds = freshness_value;
    row.redacted = redacted;
    row.redaction_class = redacted ? "protected_material" : "public";
    row.tamper_evidence_digest =
        EvidenceDigest(row.key, row.value, row.labels);

    const bool appended = BoundedAppend(&result, std::move(row), request.limits);
    if (appended) {
      if (redacted) {
        ++result.redacted_row_count;
      }
      emitted_required[metric.family] = true;
    }
  }

  for (const auto& [family, emitted] : emitted_required) {
    if (!emitted) {
      result.missing_counter_families.push_back(family);
    }
  }
  result.all_required_counters_present = result.missing_counter_families.empty();
  result.route_family_generation_evidence = !result.rows.empty();
  result.freshness_source_provenance_present = !result.rows.empty();
  if (request.require_all_operation_counters &&
      !result.all_required_counters_present) {
    result.ok = false;
    result.diagnostic_code =
        "SB_INDEX_OPERATION_SUPPORT_BUNDLE.MISSING_OPERATION_COUNTER_ROWS";
    result.detail =
        "index.operation_support_bundle.required_counter_rows_missing";
  }
  return result;
}

}  // namespace scratchbird::core::index
