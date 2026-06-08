// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// CLUSTER_DECISION_ROUTE_TOPOLOGY_DESCRIPTORS
#include "cluster_catalog_manifest.hpp"
#include "runtime_platform.hpp"

#include <string>
#include <vector>

namespace scratchbird::core::catalog {

enum class ClusterDescriptorCategory {
  decision,
  route,
  fence,
  topology,
  cleanup,
  security,
  metrics,
  authority_provenance
};

struct ClusterDescriptorManifest {
  std::string descriptor_code;
  ClusterDescriptorCategory category = ClusterDescriptorCategory::decision;
  ClusterCatalogTableManifest table;
  bool external_provider_owned = true;
  bool descriptor_only = true;
  bool local_runtime_execution_enabled = false;
  bool mutable_by_local_core = false;
  bool authority_provenance_required = true;
  bool transaction_inventory_remains_finality_authority = true;
};

struct ClusterDescriptorManifestSet {
  bool external_provider_required = true;
  bool local_runtime_execution_enabled = false;
  bool mutable_by_local_core = false;
  bool transaction_inventory_remains_finality_authority = true;
  std::vector<ClusterDescriptorManifest> descriptors;
};

struct ClusterDescriptorValidationResult {
  Status status;
  ClusterDescriptorManifest descriptor;
  DiagnosticRecord diagnostic;

  bool ok() const { return status.ok(); }
};

struct ClusterDescriptorSetValidationResult {
  Status status;
  ClusterDescriptorManifestSet manifest;
  DiagnosticRecord diagnostic;

  bool ok() const { return status.ok(); }
};

const char* ClusterDescriptorCategoryName(ClusterDescriptorCategory category);
const std::vector<std::string>& RequiredClusterDescriptorCodes();
const std::vector<ClusterDescriptorManifest>&
BuiltinClusterDescriptorManifests();
ClusterDescriptorManifestSet BuiltinClusterDescriptorManifestSet();
const ClusterDescriptorManifest* FindClusterDescriptorManifest(
    const std::string& descriptor_code);
ClusterDescriptorValidationResult ValidateClusterDescriptorManifest(
    const ClusterDescriptorManifest& descriptor);
ClusterDescriptorSetValidationResult ValidateClusterDescriptorManifestSet(
    const ClusterDescriptorManifestSet& manifest);
DiagnosticRecord MakeClusterDescriptorManifestDiagnostic(
    Status status,
    std::string diagnostic_code,
    std::string message_key,
    std::string detail = {});

}  // namespace scratchbird::core::catalog
