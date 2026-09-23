// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "observability/optimizer_metric_support_bundle.hpp"
#include "metric_support_projection.hpp"

#include "optimizer_metric_manifest.hpp"
#include "metric_value_codec.hpp"
#include "mga_relation_store/mga_metadata_record_codec.hpp"

#include <openssl/sha.h>

#include <algorithm>
#include <iomanip>
#include <map>
#include <sstream>
#include <string_view>
#include <utility>

namespace scratchbird::engine::internal_api::observability {
namespace {

namespace metrics = scratchbird::core::metrics;
namespace opt = scratchbird::engine::optimizer;

bool StartsWith(std::string_view value, std::string_view prefix) {
  return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
}

std::string HexBytes(const unsigned char* bytes, std::size_t size) {
  std::ostringstream out;
  out << std::hex << std::setfill('0');
  for (std::size_t i = 0; i < size; ++i) {
    out << std::setw(2) << static_cast<unsigned int>(bytes[i]);
  }
  return out.str();
}

std::string Sha256Hex(const std::string& payload) {
  unsigned char digest[SHA256_DIGEST_LENGTH];
  SHA256(reinterpret_cast<const unsigned char*>(payload.data()),
         payload.size(), digest);
  return "sha256:" + HexBytes(digest, SHA256_DIGEST_LENGTH);
}

std::string FindLabel(const metrics::MetricValue& value,
                      std::string_view key) {
  for (const auto& label : value.labels) {
    if (label.key == key) {
      const auto* text = std::get_if<std::string>(&label.value);
      return text ? *text : std::string{};
    }
  }
  return {};
}

std::uint64_t SourceGeneration(const metrics::MetricValue& value) {
  const auto source_generation = FindLabel(value, "source_generation");
  if (source_generation.empty()) {
    return 0;
  }
  try {
    return static_cast<std::uint64_t>(std::stoull(source_generation));
  } catch (...) {
    return 0;
  }
}

bool EncodeRedactedMetric(const metrics::MetricDescriptor& descriptor,
                          const metrics::MetricValue& value, bool allow_sensitive,
                          OptimizerMetricSupportBundleRow* output) {
  if (!output) return false;
  metrics::MetricSupportProjection projection;
  if (!metrics::ProjectMetricForSupport(descriptor, value, allow_sensitive, &projection))
    return false;
  auto row = *output;
  row.binding = projection.binding;
  row.omitted_sensitive_labels = std::move(projection.omitted_sensitive_labels);
  row.encoded_redacted_value = std::move(projection.encoded_value);
  *output = std::move(row);
  return true;
}

bool UnsafeAuthority(const OptimizerMetricSupportBundleAuthority& authority) {
  return authority.parser_or_reference_authority ||
         authority.client_finality_or_visibility_authority ||
         authority.metric_visibility_or_finality_authority ||
         authority.metric_recovery_authority ||
         authority.wal_or_redo_authority ||
         authority.cluster_authority ||
         authority.benchmark_authority;
}

void AddEvidence(OptimizerMetricSupportBundleResult* result,
                 std::string evidence) {
  if (result != nullptr) {
    result->evidence.push_back(std::move(evidence));
  }
}

OptimizerMetricSupportBundleResult Refuse(
    const OptimizerMetricSupportBundleRequest& request,
    std::string code,
    std::string detail) {
  OptimizerMetricSupportBundleResult result;
  result.ok = false;
  result.diagnostic_code = std::move(code);
  result.detail = std::move(detail);
  AddEvidence(&result, "OEIC_OPTIMIZER_METRIC_RETENTION_REDACTION");
  AddEvidence(&result, "optimizer.metric_bundle.fail_closed=true");
  result.scope_uuid = request.scope_uuid;
  AddEvidence(&result, "optimizer.metric_bundle.refused=" +
                           result.diagnostic_code);
  return result;
}

std::map<std::string, const opt::OptimizerEnterpriseMetricEntry*>
ManifestByRegistryFamily() {
  std::map<std::string, const opt::OptimizerEnterpriseMetricEntry*> entries;
  for (const auto& entry : opt::OptimizerEnterpriseMetricManifest()) {
    entries.emplace(entry.registry_family, &entry);
  }
  return entries;
}

std::string EncodeBundle(const OptimizerMetricSupportBundleRequest& request,
                         const OptimizerMetricSupportBundleResult& result) {
  std::vector<std::string> fields{"optimizer.metrics.bundle.v2",
      MetadataUuidBytes(request.scope_uuid), request.support_bundle_id,
      request.capture_generation, request.evidence_digest,
      result.redaction_applied ? "sensitive_labels_omitted" : "authorized_labels",
      result.tamper_digest};
  for (const auto& row : result.rows) {
    auto omitted = row.omitted_sensitive_labels;
    omitted.insert(omitted.begin(), "omitted.labels.v2");
    const auto& binding = row.binding;
    const auto encoded = EncodeMgaMetadataFields({"optimizer.metric.row.v2",
        row.registry_family, row.metric_family, row.producer_owner,
        row.retention_class, row.redaction_class, row.support_bundle_class,
        MetadataUuidBytes(binding.metric_uuid), std::to_string(binding.descriptor_generation),
        MetadataUuidBytes(binding.label_schema_uuid), std::to_string(binding.label_schema_generation),
        MetadataUuidBytes(binding.retention_policy_uuid), std::to_string(binding.retention_policy_generation),
        MetadataUuidBytes(binding.visibility_policy_uuid), std::to_string(binding.visibility_policy_generation),
        MetadataUuidBytes(binding.rate_source_counter_uuid), std::to_string(binding.rate_source_counter_generation),
        EncodeMgaMetadataFields(omitted),
        std::string(row.encoded_redacted_value.begin(), row.encoded_redacted_value.end())});
    if (encoded.empty()) return {};
    fields.push_back(encoded);
  }
  return EncodeMgaMetadataFields(fields);
}

}  // namespace

OptimizerMetricSupportBundleResult BuildOptimizerMetricSupportBundle(
    const OptimizerMetricSupportBundleRequest& request) {
  if (!scratchbird::core::uuid::IsEngineIdentityUuid(request.scope_uuid) || request.support_bundle_id.empty() ||
      request.capture_generation.empty() || request.evidence_digest.empty()) {
    return Refuse(request,
                  "SB_OPTIMIZER_METRIC_BUNDLE.MISSING_SCOPE",
                  "optimizer.metric_bundle.required_field_missing");
  }
  if (request.max_metric_values == 0) {
    return Refuse(request,
                  "SB_OPTIMIZER_METRIC_BUNDLE.LIMIT_REQUIRED",
                  "optimizer.metric_bundle.max_metric_values_required");
  }
  if (!request.authority.metric_registry_authoritative ||
      !request.authority.optimizer_manifest_authoritative ||
      !request.authority.support_bundle_request_authorized ||
      !request.authority.redaction_policy_bound ||
      !request.authority.retention_policy_bound ||
      !request.authority.metrics_trusted ||
      !request.authority.snapshot_fresh ||
      !request.authority.engine_scope_bound) {
    return Refuse(request,
                  "SB_OPTIMIZER_METRIC_BUNDLE.AUTHORITY_REQUIRED",
                  "optimizer.metric_bundle.authority_required");
  }
  if (UnsafeAuthority(request.authority)) {
    return Refuse(request,
                  "SB_OPTIMIZER_METRIC_BUNDLE.UNSAFE_AUTHORITY",
                  "optimizer.metric_bundle.unsafe_authority");
  }

  (void)opt::EnsureOptimizerEnterpriseMetricDescriptors();
  const auto manifest = ManifestByRegistryFamily();
  const auto snapshot = request.metric_snapshot.empty()
                            ? metrics::DefaultMetricRegistry().SnapshotCurrent(false)
                            : request.metric_snapshot;

  OptimizerMetricSupportBundleResult result;
  result.ok = true;
  result.scope_uuid = request.scope_uuid;
  result.diagnostic_code = "SB_OPTIMIZER_METRIC_BUNDLE.OK";
  result.redaction_applied = !request.allow_sensitive_labels;
  AddEvidence(&result, "OEIC_OPTIMIZER_METRIC_RETENTION_REDACTION");
  AddEvidence(&result, "optimizer.metric_bundle.fail_closed=false");
  AddEvidence(&result, "optimizer.metric_bundle.advisory_only=true");
  AddEvidence(&result, "optimizer.metric_bundle.finality_authority=false");
  AddEvidence(&result, "optimizer.metric_bundle.visibility_authority=false");
  AddEvidence(&result, "optimizer.metric_bundle.security_authority=false");
  AddEvidence(&result, "optimizer.metric_bundle.recovery_authority=false");
  AddEvidence(&result, "optimizer.metric_bundle.wal_redo_authority=false");
  AddEvidence(&result, "optimizer.metric_bundle.cluster_authority=false");

  for (const auto& value : snapshot) {
    if (!StartsWith(value.family, "sb_optimizer_")) {
      continue;
    }
    const auto manifest_it = manifest.find(value.family);
    if (manifest_it == manifest.end()) {
      return Refuse(request,
                    "SB_OPTIMIZER_METRIC_BUNDLE.UNMANIFESTED_METRIC",
                    "optimizer.metric_bundle.unmanifested_metric:" +
                        value.family);
    }
    const auto& entry = *manifest_it->second;
    if (entry.support_bundle_class ==
        opt::OptimizerMetricSupportBundleClass::omitted_from_default_bundle) {
      continue;
    }
    if (request.benchmark_clean_export && !entry.benchmark_clean_consumable) {
      return Refuse(request,
                    "SB_OPTIMIZER_METRIC_BUNDLE.NOT_BENCHMARK_CLEAN",
                    "optimizer.metric_bundle.metric_not_benchmark_clean:" +
                        entry.metric_family);
    }
    if (SourceGeneration(value) < request.min_source_generation) {
      return Refuse(request,
                    "SB_OPTIMIZER_METRIC_BUNDLE.STALE_METRIC",
                    "optimizer.metric_bundle.stale_metric:" + value.family);
    }
    const auto* descriptor =
        metrics::DefaultMetricRegistry().FindDescriptor(value.family);
    if (descriptor == nullptr) {
      return Refuse(request,
                    "SB_OPTIMIZER_METRIC_BUNDLE.DESCRIPTOR_MISSING",
                    "optimizer.metric_bundle.descriptor_missing:" +
                        value.family);
    }
    OptimizerMetricSupportBundleRow row;
    row.registry_family = value.family;
    row.metric_family = entry.metric_family;
    row.producer_owner = entry.producer_owner;
    row.retention_class =
        opt::OptimizerMetricRetentionClassName(entry.retention_class);
    row.redaction_class =
        opt::OptimizerMetricRedactionClassName(entry.redaction_class);
    row.support_bundle_class =
        opt::OptimizerMetricSupportBundleClassName(entry.support_bundle_class);
    if (!EncodeRedactedMetric(*descriptor, value, request.allow_sensitive_labels, &row))
      return Refuse(request, "SB_OPTIMIZER_METRIC_BUNDLE.INVALID_VALUE",
                    "optimizer.metric_bundle.native_value_invalid");
    result.rows.push_back(std::move(row));
    if (result.rows.size() > request.max_metric_values) {
      return Refuse(request,
                    "SB_OPTIMIZER_METRIC_BUNDLE.TOO_MANY_METRICS",
                    "optimizer.metric_bundle.too_many_metrics");
    }
  }

  if (result.rows.empty()) {
    return Refuse(request,
                  "SB_OPTIMIZER_METRIC_BUNDLE.NO_OPTIMIZER_METRICS",
                  "optimizer.metric_bundle.no_optimizer_metrics");
  }
  const auto unsigned_bundle = EncodeBundle(request, result);
  if (unsigned_bundle.empty()) return Refuse(request,
      "SB_OPTIMIZER_METRIC_BUNDLE.LIMIT_EXCEEDED", "optimizer.metric_bundle.extent_invalid");
  result.tamper_digest = Sha256Hex(unsigned_bundle);
  const auto encoded = EncodeBundle(request, result);
  if (encoded.empty()) return Refuse(request,
      "SB_OPTIMIZER_METRIC_BUNDLE.LIMIT_EXCEEDED", "optimizer.metric_bundle.extent_invalid");
  result.support_bundle_bytes.assign(encoded.begin(), encoded.end());
  return result;
}

}  // namespace scratchbird::engine::internal_api::observability
