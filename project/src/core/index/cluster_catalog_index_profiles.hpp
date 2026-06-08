// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// CLUSTER_CATALOG_INDEX_PROFILES
#include "cluster_catalog_access_profile.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace scratchbird::core::index {

enum class ClusterCatalogPhysicalIndexMethod {
  hash_equality,
  btree_ordered
};

struct ClusterCatalogPhysicalIndexKey {
  std::string column_name;
  bool equality_component = true;
  bool ordered_component = false;
  bool prefix_component = false;
};

struct ClusterCatalogPhysicalIndexProfile {
  std::string index_name;
  std::string access_profile_id;
  std::string table_path;
  ClusterCatalogPhysicalIndexMethod method =
      ClusterCatalogPhysicalIndexMethod::hash_equality;
  std::vector<ClusterCatalogPhysicalIndexKey> key_columns;
  bool unique = false;
  bool external_provider_bound = true;
  bool local_runtime_execution_enabled = false;
  bool mutable_by_local_core = false;
  bool cluster_authority = false;
  bool fail_closed_without_external_provider = true;
  bool evidence_only = true;
  std::string authority_boundary = "external_cluster_provider.cluster_catalog";
};

struct ClusterCatalogIndexValidationIssue {
  std::string diagnostic_code;
  std::string detail;
};

struct ClusterCatalogIndexValidationResult {
  bool ok = true;
  std::vector<ClusterCatalogIndexValidationIssue> issues;
};

const char* ClusterCatalogPhysicalIndexMethodName(
    ClusterCatalogPhysicalIndexMethod method);
const std::vector<ClusterCatalogPhysicalIndexProfile>&
BuiltinClusterCatalogIndexProfiles();
const ClusterCatalogPhysicalIndexProfile* FindClusterCatalogIndexProfile(
    std::string_view index_name);
std::vector<ClusterCatalogPhysicalIndexProfile>
ClusterCatalogIndexProfilesForTable(std::string_view table_path);
ClusterCatalogIndexValidationResult ValidateClusterCatalogIndexProfile(
    const ClusterCatalogPhysicalIndexProfile& profile);
ClusterCatalogIndexValidationResult ValidateBuiltinClusterCatalogIndexProfiles();

}  // namespace scratchbird::core::index
