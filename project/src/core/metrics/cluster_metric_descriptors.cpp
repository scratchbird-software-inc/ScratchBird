// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "cluster_metric_descriptors.hpp"

#include <algorithm>
#include <set>
#include <utility>

namespace scratchbird::core::metrics {
namespace {

MetricLabelDescriptor Label(std::string key, bool required = true) {
  MetricLabelDescriptor label;
  label.key = std::move(key);
  label.required = required;
  label.sensitive = false;
  return label;
}

std::vector<MetricLabelDescriptor> CommonLabels() {
  return {Label("cluster_uuid"),
          Label("node_uuid"),
          Label("external_provider_uuid"),
          Label("provider_epoch"),
          Label("authority_provenance_uuid"),
          Label("result")};
}

ClusterMetricDescriptorManifest Descriptor(std::string family,
                                           MetricType type,
                                           MetricUnit unit,
                                           std::string namespace_path,
                                           std::vector<MetricLabelDescriptor> labels) {
  ClusterMetricDescriptorManifest descriptor;
  descriptor.family = std::move(family);
  descriptor.type = type;
  descriptor.unit = unit;
  descriptor.namespace_path = std::move(namespace_path);
  descriptor.producer_owner = "external_cluster_provider";
  descriptor.readiness = MetricReadiness::contract_ready_unwired;
  descriptor.cluster_only = true;
  descriptor.external_provider_bound = true;
  descriptor.local_runtime_execution_enabled = false;
  descriptor.labels = CommonLabels();
  descriptor.labels.insert(descriptor.labels.end(),
                           labels.begin(),
                           labels.end());
  return descriptor;
}

ClusterMetricDescriptorValidationResult Error(std::string code,
                                              std::string detail) {
  ClusterMetricDescriptorValidationResult result;
  result.ok = false;
  result.diagnostic_code = std::move(code);
  result.detail = std::move(detail);
  return result;
}

ClusterMetricDescriptorSetValidationResult SetError(std::string code,
                                                    std::string detail) {
  ClusterMetricDescriptorSetValidationResult result;
  result.ok = false;
  result.diagnostic_code = std::move(code);
  result.detail = std::move(detail);
  return result;
}

bool HasRequiredLabel(const ClusterMetricDescriptorManifest& descriptor,
                      const std::string& key) {
  return std::any_of(descriptor.labels.begin(),
                     descriptor.labels.end(),
                     [&key](const MetricLabelDescriptor& label) {
                       return label.key == key && label.required;
                     });
}

}  // namespace

const std::vector<std::string>& RequiredClusterMetricFamilies() {
  static const std::vector<std::string> families = {
      "sb_cluster_decision_request_total",
      "sb_cluster_decision_proof_projection_total",
      "sb_cluster_route_publish_total",
      "sb_cluster_route_binding_state",
      "sb_cluster_fence_token_state",
      "sb_cluster_shard_placement_state",
      "sb_cluster_replica_topology_generation",
      "sb_cluster_cleanup_low_water_lag_transactions",
      "sb_cluster_limbo_reconciliation_total",
      "sb_cluster_security_binding_state",
      "sb_cluster_metric_profile_binding_state",
      "sb_cluster_authority_provenance_gap_total",
      "sb_cluster_provider_boundary_refusal_total"};
  return families;
}

const std::vector<ClusterMetricDescriptorManifest>&
BuiltinClusterMetricDescriptorManifests() {
  static const std::vector<ClusterMetricDescriptorManifest> descriptors = {
      Descriptor("sb_cluster_decision_request_total",
                 MetricType::counter,
                 MetricUnit::count,
                 "cluster.sys.metrics.decision",
                 {Label("decision_service_uuid"), Label("decision_request_uuid")}),
      Descriptor("sb_cluster_decision_proof_projection_total",
                 MetricType::counter,
                 MetricUnit::count,
                 "cluster.sys.metrics.decision",
                 {Label("decision_proof_uuid"), Label("projection_generation")}),
      Descriptor("sb_cluster_route_publish_total",
                 MetricType::counter,
                 MetricUnit::count,
                 "cluster.sys.metrics.route",
                 {Label("route_uuid"), Label("route_generation")}),
      Descriptor("sb_cluster_route_binding_state",
                 MetricType::state,
                 MetricUnit::state,
                 "cluster.sys.metrics.route",
                 {Label("route_binding_uuid"), Label("shard_uuid")}),
      Descriptor("sb_cluster_fence_token_state",
                 MetricType::state,
                 MetricUnit::state,
                 "cluster.sys.metrics.fence",
                 {Label("fence_token_uuid"), Label("fence_generation")}),
      Descriptor("sb_cluster_shard_placement_state",
                 MetricType::state,
                 MetricUnit::state,
                 "cluster.sys.metrics.topology",
                 {Label("shard_uuid"), Label("placement_generation")}),
      Descriptor("sb_cluster_replica_topology_generation",
                 MetricType::gauge,
                 MetricUnit::count,
                 "cluster.sys.metrics.topology",
                 {Label("replica_topology_uuid"), Label("topology_generation")}),
      Descriptor("sb_cluster_cleanup_low_water_lag_transactions",
                 MetricType::gauge,
                 MetricUnit::count,
                 "cluster.sys.metrics.cleanup",
                 {Label("cleanup_low_water_uuid"), Label("page_family_uuid")}),
      Descriptor("sb_cluster_limbo_reconciliation_total",
                 MetricType::counter,
                 MetricUnit::count,
                 "cluster.sys.metrics.cleanup",
                 {Label("limbo_reconciliation_uuid"), Label("finality_state_code")}),
      Descriptor("sb_cluster_security_binding_state",
                 MetricType::state,
                 MetricUnit::state,
                 "cluster.sys.metrics.security",
                 {Label("security_binding_uuid"), Label("security_policy_uuid")}),
      Descriptor("sb_cluster_metric_profile_binding_state",
                 MetricType::state,
                 MetricUnit::state,
                 "cluster.sys.metrics.metrics",
                 {Label("metric_binding_uuid"), Label("metric_family_code")}),
      Descriptor("sb_cluster_authority_provenance_gap_total",
                 MetricType::counter,
                 MetricUnit::count,
                 "cluster.sys.metrics.provenance",
                 {Label("provider_generation"), Label("source_manifest_digest")}),
      Descriptor("sb_cluster_provider_boundary_refusal_total",
                 MetricType::counter,
                 MetricUnit::count,
                 "cluster.sys.metrics.provider_boundary",
                 {Label("operation"), Label("reason")}),
  };
  return descriptors;
}

const ClusterMetricDescriptorManifest* FindClusterMetricDescriptorManifest(
    const std::string& family) {
  for (const auto& descriptor : BuiltinClusterMetricDescriptorManifests()) {
    if (descriptor.family == family) {
      return &descriptor;
    }
  }
  return nullptr;
}

MetricDescriptor ToMetricDescriptor(
    const ClusterMetricDescriptorManifest& descriptor) {
  MetricDescriptor metric;
  metric.family = descriptor.family;
  metric.type = descriptor.type;
  metric.unit = descriptor.unit;
  metric.namespace_path = descriptor.namespace_path;
  metric.help = "External cluster provider descriptor-only metric.";
  metric.producer_owner = descriptor.producer_owner;
  metric.security_family = "OBS_METRICS_READ_FAMILY:cluster";
  metric.visibility = MetricVisibilityScope::cluster;
  metric.readiness = descriptor.readiness;
  metric.cluster_only = descriptor.cluster_only;
  metric.labels = descriptor.labels;
  return metric;
}

ClusterMetricDescriptorValidationResult ValidateClusterMetricDescriptorManifest(
    const ClusterMetricDescriptorManifest& descriptor) {
  if (descriptor.family.empty() || descriptor.namespace_path.empty() ||
      descriptor.producer_owner.empty()) {
    return Error("SB-CLUSTER-METRIC-DESCRIPTOR-INCOMPLETE",
                 descriptor.family);
  }
  if (!descriptor.cluster_only || !descriptor.external_provider_bound ||
      descriptor.readiness != MetricReadiness::contract_ready_unwired ||
      descriptor.producer_owner != "external_cluster_provider") {
    return Error("SB-CLUSTER-METRIC-DESCRIPTOR-AUTHORITY-REFUSED",
                 descriptor.family);
  }
  if (descriptor.local_runtime_execution_enabled) {
    return Error("SB-CLUSTER-METRIC-DESCRIPTOR-LOCAL-EXECUTION-REFUSED",
                 descriptor.family);
  }
  if (!HasRequiredLabel(descriptor, "cluster_uuid") ||
      !HasRequiredLabel(descriptor, "external_provider_uuid") ||
      !HasRequiredLabel(descriptor, "provider_epoch") ||
      !HasRequiredLabel(descriptor, "authority_provenance_uuid") ||
      !HasRequiredLabel(descriptor, "result")) {
    return Error("SB-CLUSTER-METRIC-DESCRIPTOR-LABEL-REQUIRED",
                 descriptor.family);
  }

  ClusterMetricDescriptorValidationResult result;
  result.ok = true;
  result.descriptor = descriptor;
  return result;
}

ClusterMetricDescriptorSetValidationResult
ValidateClusterMetricDescriptorManifestSet(
    const std::vector<ClusterMetricDescriptorManifest>& descriptors) {
  std::set<std::string> seen;
  for (const auto& descriptor : descriptors) {
    const auto validated = ValidateClusterMetricDescriptorManifest(descriptor);
    if (!validated.ok) {
      ClusterMetricDescriptorSetValidationResult result;
      result.ok = false;
      result.diagnostic_code = validated.diagnostic_code;
      result.detail = validated.detail;
      return result;
    }
    if (!seen.insert(descriptor.family).second) {
      return SetError("SB-CLUSTER-METRIC-DESCRIPTOR-DUPLICATE",
                      descriptor.family);
    }
  }

  for (const std::string& family : RequiredClusterMetricFamilies()) {
    if (seen.count(family) == 0) {
      return SetError("SB-CLUSTER-METRIC-DESCRIPTOR-REQUIRED-MISSING",
                      family);
    }
  }

  ClusterMetricDescriptorSetValidationResult result;
  result.ok = true;
  result.descriptors = descriptors;
  return result;
}

}  // namespace scratchbird::core::metrics
