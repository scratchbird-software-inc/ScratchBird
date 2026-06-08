// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "observability/optimizer_metric_support_bundle.hpp"

#include "optimizer_metric_manifest.hpp"

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

std::string JsonEscape(std::string_view input) {
  std::ostringstream out;
  for (const unsigned char ch : input) {
    switch (ch) {
      case '\\': out << "\\\\"; break;
      case '"': out << "\\\""; break;
      case '\n': out << "\\n"; break;
      default: out << ch;
    }
  }
  return out.str();
}

std::string FindLabel(const metrics::MetricValue& value,
                      std::string_view key) {
  for (const auto& label : value.labels) {
    if (label.key == key) {
      return label.value;
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

std::string SerializeLabels(metrics::MetricLabelSet labels) {
  std::sort(labels.begin(), labels.end(), [](const auto& lhs, const auto& rhs) {
    return lhs.key < rhs.key;
  });
  std::ostringstream out;
  bool first = true;
  for (const auto& label : labels) {
    if (!first) {
      out << ',';
    }
    first = false;
    out << label.key << '=' << label.value;
  }
  return out.str();
}

std::string SerializeValue(const metrics::MetricValue& value) {
  std::ostringstream out;
  out << "family=" << value.family
      << "|labels=" << SerializeLabels(value.labels)
      << "|value=" << value.value
      << "|count=" << value.count
      << "|sum=" << value.sum
      << "|state=" << value.state_text;
  return out.str();
}

bool UnsafeAuthority(const OptimizerMetricSupportBundleAuthority& authority) {
  return authority.parser_or_donor_authority ||
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
  AddEvidence(&result, "optimizer.metric_bundle.scope_uuid=" +
                           request.scope_uuid);
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

std::string RenderBundleJson(const OptimizerMetricSupportBundleRequest& request,
                             const OptimizerMetricSupportBundleResult& result) {
  std::ostringstream out;
  out << "{\"support_bundle\":{\"section\":\"optimizer_metrics\","
      << "\"support_bundle_id\":\"" << JsonEscape(request.support_bundle_id)
      << "\",\"capture_generation\":\""
      << JsonEscape(request.capture_generation)
      << "\",\"redaction_state\":\"sensitive_labels_redacted\","
      << "\"tamper_digest\":\"" << JsonEscape(result.tamper_digest)
      << "\",\"row_count\":" << result.rows.size()
      << ",\"rows\":[";
  for (std::size_t i = 0; i < result.rows.size(); ++i) {
    const auto& row = result.rows[i];
    if (i != 0) {
      out << ',';
    }
    out << "{\"family\":\"" << JsonEscape(row.registry_family)
        << "\",\"metric_family\":\"" << JsonEscape(row.metric_family)
        << "\",\"producer_owner\":\"" << JsonEscape(row.producer_owner)
        << "\",\"retention\":\"" << JsonEscape(row.retention_class)
        << "\",\"redaction\":\"" << JsonEscape(row.redaction_class)
        << "\",\"support_bundle_class\":\""
        << JsonEscape(row.support_bundle_class)
        << "\",\"value\":\"" << JsonEscape(row.serialized_redacted_value)
        << "\"}";
  }
  out << "]}}";
  return out.str();
}

}  // namespace

OptimizerMetricSupportBundleResult BuildOptimizerMetricSupportBundle(
    const OptimizerMetricSupportBundleRequest& request) {
  if (request.scope_uuid.empty() || request.support_bundle_id.empty() ||
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

  std::ostringstream tamper_payload;
  tamper_payload << "support_bundle_id=" << request.support_bundle_id
                 << "|capture_generation=" << request.capture_generation
                 << "|evidence_digest=" << request.evidence_digest;

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
    const auto redacted = metrics::RedactSensitiveMetricValue(
        *descriptor, value, request.allow_sensitive_labels);
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
    row.serialized_redacted_value = SerializeValue(redacted);
    tamper_payload << "|row=" << row.serialized_redacted_value;
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
  result.tamper_digest = Sha256Hex(tamper_payload.str());
  result.support_bundle_json = RenderBundleJson(request, result);
  return result;
}

}  // namespace scratchbird::engine::internal_api::observability
