// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// CLUSTER_METRIC_DESCRIPTORS
#include "metric_registry.hpp"

#include <string>
#include <vector>

namespace scratchbird::core::metrics {

struct ClusterMetricDescriptorManifest {
  std::string family;
  MetricType type = MetricType::counter;
  MetricUnit unit = MetricUnit::count;
  std::string namespace_path = "cluster.sys.metrics";
  std::string producer_owner = "external_cluster_provider";
  MetricReadiness readiness = MetricReadiness::contract_ready_unwired;
  bool cluster_only = true;
  bool external_provider_bound = true;
  bool local_runtime_execution_enabled = false;
  std::vector<MetricLabelDescriptor> labels;
};

struct ClusterMetricDescriptorValidationResult {
  bool ok = false;
  std::string diagnostic_code;
  std::string detail;
  ClusterMetricDescriptorManifest descriptor;
};

struct ClusterMetricDescriptorSetValidationResult {
  bool ok = false;
  std::string diagnostic_code;
  std::string detail;
  std::vector<ClusterMetricDescriptorManifest> descriptors;
};

const std::vector<std::string>& RequiredClusterMetricFamilies();
const std::vector<ClusterMetricDescriptorManifest>&
BuiltinClusterMetricDescriptorManifests();
const ClusterMetricDescriptorManifest* FindClusterMetricDescriptorManifest(
    const std::string& family);
MetricDescriptor ToMetricDescriptor(
    const ClusterMetricDescriptorManifest& descriptor);
ClusterMetricDescriptorValidationResult ValidateClusterMetricDescriptorManifest(
    const ClusterMetricDescriptorManifest& descriptor);
ClusterMetricDescriptorSetValidationResult
ValidateClusterMetricDescriptorManifestSet(
    const std::vector<ClusterMetricDescriptorManifest>& descriptors);

}  // namespace scratchbird::core::metrics
