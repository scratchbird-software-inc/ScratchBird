// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// CLUSTER_CATALOG_ACCESS_PROFILES
#include "cluster_catalog_manifest.hpp"
#include "runtime_platform.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace scratchbird::core::catalog {

enum class ClusterCatalogAccessMethod {
  hash_equality,
  btree_ordered
};

enum class ClusterCatalogAccessPurpose {
  uuid_exact_lookup,
  node_role_lookup,
  route_lookup,
  decision_proof_lookup,
  shard_placement_lookup,
  epoch_generation_scan,
  projection_invalidation_scan
};

struct ClusterCatalogAccessKeyColumn {
  std::string column_name;
  bool equality_component = true;
  bool ordered_component = false;
  bool prefix_component = false;
};

struct ClusterCatalogAccessProfile {
  std::string profile_id;
  std::string table_path;
  std::string stable_source_id;
  ClusterCatalogAccessMethod method = ClusterCatalogAccessMethod::hash_equality;
  ClusterCatalogAccessPurpose purpose =
      ClusterCatalogAccessPurpose::uuid_exact_lookup;
  std::vector<ClusterCatalogAccessKeyColumn> key_columns;
  bool unique = false;
  bool external_provider_bound = true;
  bool projection_cache_profile = false;
  bool source_authority_required = false;
  bool local_runtime_execution_enabled = false;
  bool mutable_by_local_core = false;
  bool cluster_authority = false;
  bool fail_closed_without_external_provider = true;
  bool supports_uuid_exact_lookup = false;
  bool supports_node_role_lookup = false;
  bool supports_route_lookup = false;
  bool supports_decision_proof_lookup = false;
  bool supports_shard_placement_lookup = false;
  bool supports_epoch_generation_scan = false;
  bool supports_projection_invalidation = false;
  std::string authority_boundary = "external_cluster_provider.cluster_catalog";
};

struct ClusterCatalogAccessValidationIssue {
  std::string diagnostic_code;
  std::string detail;
};

struct ClusterCatalogAccessValidationResult {
  bool ok = true;
  std::vector<ClusterCatalogAccessValidationIssue> issues;
};

const char* ClusterCatalogAccessMethodName(ClusterCatalogAccessMethod method);
const char* ClusterCatalogAccessPurposeName(ClusterCatalogAccessPurpose purpose);
const std::vector<ClusterCatalogAccessPurpose>&
RequiredClusterCatalogAccessPurposes();
const std::vector<ClusterCatalogAccessProfile>&
BuiltinClusterCatalogAccessProfiles();
const ClusterCatalogAccessProfile* FindClusterCatalogAccessProfile(
    std::string_view profile_id);
std::vector<ClusterCatalogAccessProfile> ClusterCatalogAccessProfilesForTable(
    std::string_view table_path);
ClusterCatalogAccessValidationResult ValidateClusterCatalogAccessProfile(
    const ClusterCatalogAccessProfile& profile);
ClusterCatalogAccessValidationResult ValidateBuiltinClusterCatalogAccessProfiles();

}  // namespace scratchbird::core::catalog
