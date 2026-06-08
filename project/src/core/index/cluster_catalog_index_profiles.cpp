// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "cluster_catalog_index_profiles.hpp"

#include <algorithm>
#include <set>
#include <utility>

namespace scratchbird::core::index {
namespace {

namespace catalog = scratchbird::core::catalog;

std::string SanitizeId(std::string value) {
  for (char& ch : value) {
    if (ch == '.') {
      ch = '_';
    }
  }
  return value;
}

ClusterCatalogPhysicalIndexMethod ToIndexMethod(
    catalog::ClusterCatalogAccessMethod method) {
  switch (method) {
    case catalog::ClusterCatalogAccessMethod::hash_equality:
      return ClusterCatalogPhysicalIndexMethod::hash_equality;
    case catalog::ClusterCatalogAccessMethod::btree_ordered:
      return ClusterCatalogPhysicalIndexMethod::btree_ordered;
  }
  return ClusterCatalogPhysicalIndexMethod::hash_equality;
}

ClusterCatalogPhysicalIndexProfile FromAccessProfile(
    const catalog::ClusterCatalogAccessProfile& access) {
  ClusterCatalogPhysicalIndexProfile profile;
  profile.index_name = "idx_" + SanitizeId(access.profile_id);
  profile.access_profile_id = access.profile_id;
  profile.table_path = access.table_path;
  profile.method = ToIndexMethod(access.method);
  profile.unique = access.unique;
  profile.external_provider_bound = access.external_provider_bound;
  profile.local_runtime_execution_enabled = false;
  profile.mutable_by_local_core = false;
  profile.cluster_authority = false;
  profile.fail_closed_without_external_provider =
      access.fail_closed_without_external_provider;
  profile.evidence_only = true;
  profile.authority_boundary = access.authority_boundary;
  for (const auto& key : access.key_columns) {
    profile.key_columns.push_back({key.column_name,
                                   key.equality_component,
                                   key.ordered_component,
                                   key.prefix_component});
  }
  return profile;
}

void AddIssue(ClusterCatalogIndexValidationResult* result,
              std::string code,
              std::string detail) {
  result->issues.push_back({std::move(code), std::move(detail)});
  result->ok = false;
}

bool KeysMatchAccess(const ClusterCatalogPhysicalIndexProfile& profile,
                     const catalog::ClusterCatalogAccessProfile& access) {
  if (profile.key_columns.size() != access.key_columns.size()) {
    return false;
  }
  for (std::size_t i = 0; i < profile.key_columns.size(); ++i) {
    const auto& left = profile.key_columns[i];
    const auto& right = access.key_columns[i];
    if (left.column_name != right.column_name ||
        left.equality_component != right.equality_component ||
        left.ordered_component != right.ordered_component ||
        left.prefix_component != right.prefix_component) {
      return false;
    }
  }
  return true;
}

}  // namespace

const char* ClusterCatalogPhysicalIndexMethodName(
    ClusterCatalogPhysicalIndexMethod method) {
  switch (method) {
    case ClusterCatalogPhysicalIndexMethod::hash_equality:
      return "hash_equality";
    case ClusterCatalogPhysicalIndexMethod::btree_ordered:
      return "btree_ordered";
  }
  return "unknown";
}

const std::vector<ClusterCatalogPhysicalIndexProfile>&
BuiltinClusterCatalogIndexProfiles() {
  static const std::vector<ClusterCatalogPhysicalIndexProfile> profiles = [] {
    std::vector<ClusterCatalogPhysicalIndexProfile> out;
    for (const auto& access : catalog::BuiltinClusterCatalogAccessProfiles()) {
      out.push_back(FromAccessProfile(access));
    }
    return out;
  }();
  return profiles;
}

const ClusterCatalogPhysicalIndexProfile* FindClusterCatalogIndexProfile(
    std::string_view index_name) {
  for (const auto& profile : BuiltinClusterCatalogIndexProfiles()) {
    if (std::string_view(profile.index_name) == index_name) {
      return &profile;
    }
  }
  return nullptr;
}

std::vector<ClusterCatalogPhysicalIndexProfile>
ClusterCatalogIndexProfilesForTable(std::string_view table_path) {
  std::vector<ClusterCatalogPhysicalIndexProfile> result;
  for (const auto& profile : BuiltinClusterCatalogIndexProfiles()) {
    if (std::string_view(profile.table_path) == table_path) {
      result.push_back(profile);
    }
  }
  return result;
}

ClusterCatalogIndexValidationResult ValidateClusterCatalogIndexProfile(
    const ClusterCatalogPhysicalIndexProfile& profile) {
  ClusterCatalogIndexValidationResult result;
  if (profile.index_name.empty() || profile.access_profile_id.empty() ||
      profile.table_path.empty() || profile.key_columns.empty()) {
    AddIssue(&result, "SB-CLUSTER-CATALOG-INDEX-INCOMPLETE",
             profile.index_name);
    return result;
  }
  const auto* access =
      catalog::FindClusterCatalogAccessProfile(profile.access_profile_id);
  if (access == nullptr) {
    AddIssue(&result, "SB-CLUSTER-CATALOG-INDEX-ACCESS-UNKNOWN",
             profile.access_profile_id);
    return result;
  }
  if (profile.table_path != access->table_path ||
      profile.unique != access->unique ||
      !KeysMatchAccess(profile, *access)) {
    AddIssue(&result, "SB-CLUSTER-CATALOG-INDEX-ACCESS-MISMATCH",
             profile.index_name);
  }
  if (profile.method != ToIndexMethod(access->method)) {
    AddIssue(&result, "SB-CLUSTER-CATALOG-INDEX-METHOD-MISMATCH",
             profile.index_name);
  }
  if (!profile.external_provider_bound || !profile.evidence_only ||
      !profile.fail_closed_without_external_provider) {
    AddIssue(&result, "SB-CLUSTER-CATALOG-INDEX-AUTHORITY-REFUSED",
             profile.index_name);
  }
  if (profile.local_runtime_execution_enabled ||
      profile.mutable_by_local_core || profile.cluster_authority) {
    AddIssue(&result, "SB-CLUSTER-CATALOG-INDEX-LOCAL-EXECUTION-REFUSED",
             profile.index_name);
  }
  if (profile.authority_boundary.find("external_cluster_provider") ==
      std::string::npos) {
    AddIssue(&result, "SB-CLUSTER-CATALOG-INDEX-BOUNDARY-MISSING",
             profile.index_name);
  }
  return result;
}

ClusterCatalogIndexValidationResult
ValidateBuiltinClusterCatalogIndexProfiles() {
  ClusterCatalogIndexValidationResult result;
  std::set<std::string> names;
  std::set<std::string> access_ids;
  for (const auto& profile : BuiltinClusterCatalogIndexProfiles()) {
    const auto validated = ValidateClusterCatalogIndexProfile(profile);
    if (!validated.ok) {
      result.ok = false;
      result.issues.insert(result.issues.end(),
                           validated.issues.begin(),
                           validated.issues.end());
    }
    if (!names.insert(profile.index_name).second) {
      AddIssue(&result, "SB-CLUSTER-CATALOG-INDEX-DUPLICATE",
               profile.index_name);
    }
    access_ids.insert(profile.access_profile_id);
  }
  for (const auto& access : catalog::BuiltinClusterCatalogAccessProfiles()) {
    if (access_ids.count(access.profile_id) == 0) {
      AddIssue(&result, "SB-CLUSTER-CATALOG-INDEX-COVERAGE-MISSING",
               access.profile_id);
    }
  }
  return result;
}

}  // namespace scratchbird::core::index
