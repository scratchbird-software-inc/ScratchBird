// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "cluster_catalog_access_profile.hpp"

#include "cluster_schema_gating.hpp"

#include <algorithm>
#include <cctype>
#include <set>
#include <string_view>
#include <utility>

namespace scratchbird::core::catalog {
namespace {

ClusterCatalogAccessKeyColumn Key(std::string column_name,
                                  bool equality = true,
                                  bool ordered = false,
                                  bool prefix = false) {
  ClusterCatalogAccessKeyColumn key;
  key.column_name = std::move(column_name);
  key.equality_component = equality;
  key.ordered_component = ordered;
  key.prefix_component = prefix;
  return key;
}

bool StartsWith(std::string_view value, std::string_view prefix) {
  return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
}

std::string SanitizeId(std::string value) {
  for (char& ch : value) {
    if (ch == '.') {
      ch = '_';
    }
  }
  return value;
}

void AddIssue(ClusterCatalogAccessValidationResult* result,
              std::string code,
              std::string detail) {
  result->issues.push_back({std::move(code), std::move(detail)});
  result->ok = false;
}

const ClusterCatalogColumnManifest* FindColumn(
    const ClusterCatalogTableManifest& table,
    std::string_view column_name) {
  return FindClusterCatalogColumn(table, std::string(column_name));
}

const ClusterCatalogTableManifest* FindCatalogSourceTable(
    std::string_view table_path) {
  for (const auto& table : BuiltinClusterCatalogTableManifests()) {
    const std::string path = ClusterCatalogFullTablePath(table);
    if (std::string_view(path) == table_path) {
      return &table;
    }
  }
  for (const auto& role_profile : BuiltinClusterRoleProfileManifests()) {
    const std::string path = ClusterCatalogFullTablePath(role_profile.table);
    if (std::string_view(path) == table_path) {
      return &role_profile.table;
    }
  }
  return nullptr;
}

const ClusterCacheProjectionManifest* FindProjectionSourceTable(
    std::string_view table_path) {
  for (const auto& projection : BuiltinClusterCacheProjectionManifests()) {
    const std::string path = ClusterCacheProjectionFullTablePath(projection);
    if (std::string_view(path) == table_path) {
      return &projection;
    }
  }
  return nullptr;
}

std::string Lower(std::string value) {
  for (char& ch : value) {
    ch = static_cast<char>(
        std::tolower(static_cast<unsigned char>(ch)));
  }
  return value;
}

bool IsForbiddenUserLayerKey(std::string_view column_name) {
  const std::string lower = Lower(std::string(column_name));
  return lower.find("name") != std::string::npos ||
         lower.find("comment") != std::string::npos ||
         lower.find("description") != std::string::npos ||
         lower.find("property") != std::string::npos;
}

bool ProjectionHasColumn(const ClusterCacheProjectionManifest& projection,
                         std::string_view column_name) {
  return FindClusterCacheProjectionColumn(projection,
                                          std::string(column_name)) != nullptr;
}

bool AccessKeyColumnExists(const ClusterCatalogAccessProfile& profile,
                           std::string_view column_name) {
  if (const auto* table = FindCatalogSourceTable(profile.table_path)) {
    return FindColumn(*table, column_name) != nullptr;
  }
  if (const auto* projection = FindProjectionSourceTable(profile.table_path)) {
    return ProjectionHasColumn(*projection, column_name);
  }
  return false;
}

bool StableSourceIdMatches(const ClusterCatalogAccessProfile& profile) {
  if (const auto* table = FindCatalogSourceTable(profile.table_path)) {
    return table->stable_table_id == profile.stable_source_id;
  }
  if (const auto* projection = FindProjectionSourceTable(profile.table_path)) {
    return projection->stable_projection_id == profile.stable_source_id;
  }
  return false;
}

std::string PrimaryUuidColumn(const ClusterCatalogTableManifest& table) {
  return table.primary_key_columns.empty() ? std::string{}
                                           : table.primary_key_columns.front();
}

std::string FirstColumnContaining(const ClusterCatalogTableManifest& table,
                                  std::string_view token) {
  for (const auto& column : table.columns) {
    if (column.column_name.find(token) != std::string::npos) {
      return column.column_name;
    }
  }
  return {};
}

ClusterCatalogAccessProfile AccessProfile(
    std::string profile_id,
    std::string table_path,
    std::string stable_source_id,
    ClusterCatalogAccessMethod method,
    ClusterCatalogAccessPurpose purpose,
    std::vector<ClusterCatalogAccessKeyColumn> keys) {
  ClusterCatalogAccessProfile profile;
  profile.profile_id = std::move(profile_id);
  profile.table_path = std::move(table_path);
  profile.stable_source_id = std::move(stable_source_id);
  profile.method = method;
  profile.purpose = purpose;
  profile.key_columns = std::move(keys);
  profile.external_provider_bound = true;
  profile.local_runtime_execution_enabled = false;
  profile.mutable_by_local_core = false;
  profile.cluster_authority = false;
  profile.fail_closed_without_external_provider = true;
  switch (purpose) {
    case ClusterCatalogAccessPurpose::uuid_exact_lookup:
      profile.unique = true;
      profile.supports_uuid_exact_lookup = true;
      break;
    case ClusterCatalogAccessPurpose::node_role_lookup:
      profile.supports_node_role_lookup = true;
      break;
    case ClusterCatalogAccessPurpose::route_lookup:
      profile.supports_route_lookup = true;
      break;
    case ClusterCatalogAccessPurpose::decision_proof_lookup:
      profile.supports_decision_proof_lookup = true;
      break;
    case ClusterCatalogAccessPurpose::shard_placement_lookup:
      profile.supports_shard_placement_lookup = true;
      break;
    case ClusterCatalogAccessPurpose::epoch_generation_scan:
      profile.supports_epoch_generation_scan = true;
      break;
    case ClusterCatalogAccessPurpose::projection_invalidation_scan:
      profile.projection_cache_profile = true;
      profile.source_authority_required = true;
      profile.supports_projection_invalidation = true;
      profile.authority_boundary =
          "external_cluster_provider.cluster_cache_projection";
      break;
  }
  return profile;
}

ClusterCatalogAccessProfile ExactUuidProfile(
    const ClusterCatalogTableManifest& table) {
  const std::string path = ClusterCatalogFullTablePath(table);
  return AccessProfile("cluster_catalog." + SanitizeId(path) + ".uuid_exact",
                       path,
                       table.stable_table_id,
                       ClusterCatalogAccessMethod::hash_equality,
                       ClusterCatalogAccessPurpose::uuid_exact_lookup,
                       {Key(PrimaryUuidColumn(table))});
}

bool AddEpochGenerationProfile(const ClusterCatalogTableManifest& table,
                               std::vector<ClusterCatalogAccessProfile>* out) {
  const std::string epoch = FirstColumnContaining(table, "_epoch");
  const std::string generation = FirstColumnContaining(table, "generation");
  const std::string primary = PrimaryUuidColumn(table);
  if (epoch.empty() || generation.empty() || primary.empty()) {
    return false;
  }
  const std::string path = ClusterCatalogFullTablePath(table);
  out->push_back(AccessProfile(
      "cluster_catalog." + SanitizeId(path) + ".epoch_generation",
      path,
      table.stable_table_id,
      ClusterCatalogAccessMethod::btree_ordered,
      ClusterCatalogAccessPurpose::epoch_generation_scan,
      {Key(epoch, true, true, true),
       Key(generation, true, true, true),
       Key(primary, true, true, false)}));
  return true;
}

void AddTableProfiles(const ClusterCatalogTableManifest& table,
                      std::vector<ClusterCatalogAccessProfile>* out) {
  out->push_back(ExactUuidProfile(table));
  AddEpochGenerationProfile(table, out);
}

bool HasPurposeFlag(const ClusterCatalogAccessProfile& profile) {
  switch (profile.purpose) {
    case ClusterCatalogAccessPurpose::uuid_exact_lookup:
      return profile.supports_uuid_exact_lookup;
    case ClusterCatalogAccessPurpose::node_role_lookup:
      return profile.supports_node_role_lookup;
    case ClusterCatalogAccessPurpose::route_lookup:
      return profile.supports_route_lookup;
    case ClusterCatalogAccessPurpose::decision_proof_lookup:
      return profile.supports_decision_proof_lookup;
    case ClusterCatalogAccessPurpose::shard_placement_lookup:
      return profile.supports_shard_placement_lookup;
    case ClusterCatalogAccessPurpose::epoch_generation_scan:
      return profile.supports_epoch_generation_scan;
    case ClusterCatalogAccessPurpose::projection_invalidation_scan:
      return profile.supports_projection_invalidation;
  }
  return false;
}

bool KeyContains(const ClusterCatalogAccessProfile& profile,
                 std::string_view column_name) {
  return std::any_of(profile.key_columns.begin(),
                     profile.key_columns.end(),
                     [column_name](const auto& key) {
                       return key.column_name == column_name;
                     });
}

std::set<std::string> ExpectedCatalogSourcePaths() {
  std::set<std::string> paths;
  for (const auto& table : BuiltinClusterCatalogTableManifests()) {
    paths.insert(ClusterCatalogFullTablePath(table));
  }
  for (const auto& role_profile : BuiltinClusterRoleProfileManifests()) {
    paths.insert(ClusterCatalogFullTablePath(role_profile.table));
  }
  return paths;
}

}  // namespace

const char* ClusterCatalogAccessMethodName(
    ClusterCatalogAccessMethod method) {
  switch (method) {
    case ClusterCatalogAccessMethod::hash_equality:
      return "hash_equality";
    case ClusterCatalogAccessMethod::btree_ordered:
      return "btree_ordered";
  }
  return "unknown";
}

const char* ClusterCatalogAccessPurposeName(
    ClusterCatalogAccessPurpose purpose) {
  switch (purpose) {
    case ClusterCatalogAccessPurpose::uuid_exact_lookup:
      return "uuid_exact_lookup";
    case ClusterCatalogAccessPurpose::node_role_lookup:
      return "node_role_lookup";
    case ClusterCatalogAccessPurpose::route_lookup:
      return "route_lookup";
    case ClusterCatalogAccessPurpose::decision_proof_lookup:
      return "decision_proof_lookup";
    case ClusterCatalogAccessPurpose::shard_placement_lookup:
      return "shard_placement_lookup";
    case ClusterCatalogAccessPurpose::epoch_generation_scan:
      return "epoch_generation_scan";
    case ClusterCatalogAccessPurpose::projection_invalidation_scan:
      return "projection_invalidation_scan";
  }
  return "unknown";
}

const std::vector<ClusterCatalogAccessPurpose>&
RequiredClusterCatalogAccessPurposes() {
  static const std::vector<ClusterCatalogAccessPurpose> purposes = {
      ClusterCatalogAccessPurpose::uuid_exact_lookup,
      ClusterCatalogAccessPurpose::node_role_lookup,
      ClusterCatalogAccessPurpose::route_lookup,
      ClusterCatalogAccessPurpose::decision_proof_lookup,
      ClusterCatalogAccessPurpose::shard_placement_lookup,
      ClusterCatalogAccessPurpose::epoch_generation_scan,
      ClusterCatalogAccessPurpose::projection_invalidation_scan,
  };
  return purposes;
}

const std::vector<ClusterCatalogAccessProfile>&
BuiltinClusterCatalogAccessProfiles() {
  static const std::vector<ClusterCatalogAccessProfile> profiles = [] {
    std::vector<ClusterCatalogAccessProfile> out;
    for (const auto& table : BuiltinClusterCatalogTableManifests()) {
      AddTableProfiles(table, &out);
    }
    for (const auto& role_profile : BuiltinClusterRoleProfileManifests()) {
      AddTableProfiles(role_profile.table, &out);
    }

    out.push_back(AccessProfile(
        "cluster_catalog.node.node_role_lookup",
        "cluster.sys.catalog.node",
        "cluster_catalog.node",
        ClusterCatalogAccessMethod::btree_ordered,
        ClusterCatalogAccessPurpose::node_role_lookup,
        {Key("cluster_uuid", true, false, true),
         Key("node_role_uuid", true, false, true),
         Key("catalog_generation", true, true, true),
         Key("node_uuid", true, true, false)}));

    out.push_back(AccessProfile(
        "cluster_catalog.route.route_lookup",
        "cluster.sys.catalog.route",
        "cluster_catalog.route",
        ClusterCatalogAccessMethod::btree_ordered,
        ClusterCatalogAccessPurpose::route_lookup,
        {Key("cluster_uuid", true, false, true),
         Key("source_node_uuid", true, false, true),
         Key("target_node_uuid", true, false, true),
         Key("route_generation", true, true, true),
         Key("route_uuid", true, true, false)}));

    out.push_back(AccessProfile(
        "cluster_catalog.route_decision.decision_proof_lookup",
        "cluster.sys.catalog.route_decision",
        "cluster_catalog.route_decision",
        ClusterCatalogAccessMethod::btree_ordered,
        ClusterCatalogAccessPurpose::decision_proof_lookup,
        {Key("decision_proof_uuid", true, false, true),
         Key("decision_epoch", true, true, true),
         Key("route_decision_uuid", true, true, false)}));

    out.push_back(AccessProfile(
        "cluster_catalog.shard_topology.shard_placement_lookup",
        "cluster.sys.catalog.shard_topology",
        "cluster_catalog.shard_topology",
        ClusterCatalogAccessMethod::btree_ordered,
        ClusterCatalogAccessPurpose::shard_placement_lookup,
        {Key("page_family_uuid", true, false, true),
         Key("placement_profile_uuid", true, false, true),
         Key("topology_generation", true, true, true),
         Key("shard_uuid", true, true, false)}));

    for (const auto& projection : BuiltinClusterCacheProjectionManifests()) {
      const std::string path = ClusterCacheProjectionFullTablePath(projection);
      out.push_back(AccessProfile(
          "cluster_catalog." + SanitizeId(path) + ".projection_invalidation",
          path,
          projection.stable_projection_id,
          ClusterCatalogAccessMethod::btree_ordered,
          ClusterCatalogAccessPurpose::projection_invalidation_scan,
          {Key("source_record_uuid", true, false, true),
           Key("invalidation_epoch", true, true, true),
           Key("source_generation", true, true, true),
           Key("projection_uuid", true, true, false)}));
    }
    return out;
  }();
  return profiles;
}

const ClusterCatalogAccessProfile* FindClusterCatalogAccessProfile(
    std::string_view profile_id) {
  for (const auto& profile : BuiltinClusterCatalogAccessProfiles()) {
    if (std::string_view(profile.profile_id) == profile_id) {
      return &profile;
    }
  }
  return nullptr;
}

std::vector<ClusterCatalogAccessProfile> ClusterCatalogAccessProfilesForTable(
    std::string_view table_path) {
  std::vector<ClusterCatalogAccessProfile> result;
  for (const auto& profile : BuiltinClusterCatalogAccessProfiles()) {
    if (std::string_view(profile.table_path) == table_path) {
      result.push_back(profile);
    }
  }
  return result;
}

ClusterCatalogAccessValidationResult ValidateClusterCatalogAccessProfile(
    const ClusterCatalogAccessProfile& profile) {
  ClusterCatalogAccessValidationResult result;
  if (profile.profile_id.empty() || profile.table_path.empty() ||
      profile.key_columns.empty() || profile.stable_source_id.empty()) {
    AddIssue(&result, "SB-CLUSTER-CATALOG-ACCESS-INCOMPLETE",
             profile.profile_id);
    return result;
  }
  if (!StartsWith(profile.table_path, "cluster.") &&
      !StartsWith(profile.table_path, "sys.catalog.cluster_cache.")) {
    AddIssue(&result, "SB-CLUSTER-CATALOG-ACCESS-PATH-INVALID",
             profile.table_path);
  }
  if (!StableSourceIdMatches(profile)) {
    AddIssue(&result, "SB-CLUSTER-CATALOG-ACCESS-SOURCE-MISMATCH",
             profile.profile_id);
  }
  if (!profile.external_provider_bound ||
      !profile.fail_closed_without_external_provider) {
    AddIssue(&result, "SB-CLUSTER-CATALOG-ACCESS-AUTHORITY-REFUSED",
             profile.profile_id);
  }
  if (profile.local_runtime_execution_enabled ||
      profile.mutable_by_local_core || profile.cluster_authority) {
    AddIssue(&result, "SB-CLUSTER-CATALOG-ACCESS-LOCAL-EXECUTION-REFUSED",
             profile.profile_id);
  }
  if (!HasPurposeFlag(profile)) {
    AddIssue(&result, "SB-CLUSTER-CATALOG-ACCESS-PURPOSE-FLAG-MISSING",
             profile.profile_id);
  }
  if (profile.authority_boundary.find("external_cluster_provider") ==
      std::string::npos) {
    AddIssue(&result, "SB-CLUSTER-CATALOG-ACCESS-BOUNDARY-MISSING",
             profile.profile_id);
  }
  for (const auto& key : profile.key_columns) {
    if (IsForbiddenUserLayerKey(key.column_name)) {
      AddIssue(&result, "SB-CLUSTER-CATALOG-ACCESS-USER-LAYER-KEY",
               profile.profile_id + ":" + key.column_name);
    }
    if (!AccessKeyColumnExists(profile, key.column_name)) {
      AddIssue(&result, "SB-CLUSTER-CATALOG-ACCESS-KEY-UNKNOWN",
               profile.profile_id + ":" + key.column_name);
    }
  }
  if (profile.purpose == ClusterCatalogAccessPurpose::uuid_exact_lookup) {
    if (profile.method != ClusterCatalogAccessMethod::hash_equality ||
        !profile.unique || profile.key_columns.size() != 1 ||
        profile.key_columns.front().column_name.find("_uuid") ==
            std::string::npos) {
      AddIssue(&result, "SB-CLUSTER-CATALOG-ACCESS-UUID-EXACT-INVALID",
               profile.profile_id);
    }
  }
  if (profile.purpose == ClusterCatalogAccessPurpose::epoch_generation_scan) {
    if (profile.method != ClusterCatalogAccessMethod::btree_ordered) {
      AddIssue(&result, "SB-CLUSTER-CATALOG-ACCESS-EPOCH-METHOD-INVALID",
               profile.profile_id);
    }
    const bool has_generation = std::any_of(
        profile.key_columns.begin(),
        profile.key_columns.end(),
        [](const auto& key) {
          return key.column_name.find("generation") != std::string::npos;
        });
    if (!has_generation) {
      AddIssue(&result, "SB-CLUSTER-CATALOG-ACCESS-GENERATION-MISSING",
               profile.profile_id);
    }
    const bool has_epoch = std::any_of(
        profile.key_columns.begin(),
        profile.key_columns.end(),
        [](const auto& key) {
          return key.column_name.find("_epoch") != std::string::npos;
        });
    if (!has_epoch) {
      AddIssue(&result, "SB-CLUSTER-CATALOG-ACCESS-EPOCH-MISSING",
               profile.profile_id);
    }
  }
  if (profile.purpose == ClusterCatalogAccessPurpose::projection_invalidation_scan) {
    if (!profile.projection_cache_profile || !profile.source_authority_required ||
        !StartsWith(profile.table_path, "sys.catalog.cluster_cache.") ||
        !KeyContains(profile, "invalidation_epoch") ||
        !KeyContains(profile, "source_generation") ||
        !KeyContains(profile, "source_record_uuid")) {
      AddIssue(&result, "SB-CLUSTER-CATALOG-ACCESS-PROJECTION-INVALID",
               profile.profile_id);
    }
  }
  if (profile.method == ClusterCatalogAccessMethod::hash_equality) {
    for (const auto& key : profile.key_columns) {
      if (!key.equality_component || key.ordered_component ||
          key.prefix_component) {
        AddIssue(&result, "SB-CLUSTER-CATALOG-ACCESS-HASH-KEY-INVALID",
                 profile.profile_id + ":" + key.column_name);
      }
    }
  }
  if (profile.method == ClusterCatalogAccessMethod::btree_ordered) {
    const bool has_ordered = std::any_of(
        profile.key_columns.begin(),
        profile.key_columns.end(),
        [](const auto& key) { return key.ordered_component; });
    if (!has_ordered) {
      AddIssue(&result, "SB-CLUSTER-CATALOG-ACCESS-BTREE-ORDER-MISSING",
               profile.profile_id);
    }
  }
  return result;
}

ClusterCatalogAccessValidationResult
ValidateBuiltinClusterCatalogAccessProfiles() {
  ClusterCatalogAccessValidationResult result;
  std::set<std::string> profile_ids;
  std::set<ClusterCatalogAccessPurpose> purposes;
  std::set<std::string> exact_uuid_tables;
  std::set<std::string> projection_invalidation_tables;

  for (const auto& profile : BuiltinClusterCatalogAccessProfiles()) {
    const auto validated = ValidateClusterCatalogAccessProfile(profile);
    if (!validated.ok) {
      result.ok = false;
      result.issues.insert(result.issues.end(),
                           validated.issues.begin(),
                           validated.issues.end());
    }
    if (!profile_ids.insert(profile.profile_id).second) {
      AddIssue(&result, "SB-CLUSTER-CATALOG-ACCESS-DUPLICATE",
               profile.profile_id);
    }
    purposes.insert(profile.purpose);
    if (profile.purpose == ClusterCatalogAccessPurpose::uuid_exact_lookup) {
      exact_uuid_tables.insert(profile.table_path);
    }
    if (profile.purpose ==
        ClusterCatalogAccessPurpose::projection_invalidation_scan) {
      projection_invalidation_tables.insert(profile.table_path);
    }
  }

  for (const auto purpose : RequiredClusterCatalogAccessPurposes()) {
    if (purposes.count(purpose) == 0) {
      AddIssue(&result, "SB-CLUSTER-CATALOG-ACCESS-PURPOSE-REQUIRED",
               ClusterCatalogAccessPurposeName(purpose));
    }
  }
  for (const auto& table_path : ExpectedCatalogSourcePaths()) {
    if (exact_uuid_tables.count(table_path) == 0) {
      AddIssue(&result, "SB-CLUSTER-CATALOG-ACCESS-UUID-COVERAGE-MISSING",
               table_path);
    }
  }
  for (const auto& projection : BuiltinClusterCacheProjectionManifests()) {
    const std::string path = ClusterCacheProjectionFullTablePath(projection);
    if (projection_invalidation_tables.count(path) == 0) {
      AddIssue(&result,
               "SB-CLUSTER-CATALOG-ACCESS-PROJECTION-COVERAGE-MISSING",
               path);
    }
  }

  return result;
}

}  // namespace scratchbird::core::catalog
