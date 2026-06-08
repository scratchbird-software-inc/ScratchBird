// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "cluster_catalog_manifest.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace catalog = scratchbird::core::catalog;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

bool HasColumn(const catalog::ClusterCatalogTableManifest& table,
               std::string_view column_name) {
  return catalog::FindClusterCatalogColumn(table, std::string(column_name)) !=
         nullptr;
}

bool HasRequiredColumn(const catalog::ClusterCatalogTableManifest& table,
                       std::string_view column_name) {
  const auto* column =
      catalog::FindClusterCatalogColumn(table, std::string(column_name));
  return column != nullptr && column->required;
}

const catalog::ClusterCatalogTableManifest* FindTable(
    const catalog::ClusterCatalogManifestSet& manifest,
    std::string_view full_path) {
  for (const auto& table : manifest.tables) {
    if (catalog::ClusterCatalogFullTablePath(table) == full_path) {
      return &table;
    }
  }
  return nullptr;
}

const catalog::ClusterRoleProfileManifest* FindRoleProfile(
    const catalog::ClusterCatalogManifestSet& manifest,
    std::string_view role_code) {
  for (const auto& role_profile : manifest.role_profiles) {
    if (role_profile.role_code == role_code) {
      return &role_profile;
    }
  }
  return nullptr;
}

void RequireTableShape(const catalog::ClusterCatalogTableManifest& table,
                       std::string_view full_path,
                       const std::vector<std::string>& required_columns) {
  Require(catalog::ClusterCatalogFullTablePath(table) == full_path,
          "cluster catalog table path mismatch");
  Require(table.engine_owned, "cluster catalog table must be engine-owned");
  Require(table.cluster_shared, "cluster catalog table must be cluster-shared");
  Require(table.external_provider_bound,
          "cluster catalog table must be external-provider-bound");
  Require(!table.local_runtime_execution_enabled,
          "cluster catalog manifest enabled local runtime execution");
  Require(!table.mutable_by_local_core,
          "cluster catalog manifest allowed local core mutation");
  Require(table.uuid_only_identity,
          "cluster catalog manifest must be UUID-identity only");
  Require(HasRequiredColumn(table, "status"),
          "cluster catalog table missing required status column");
  Require(!table.primary_key_columns.empty(),
          "cluster catalog table missing primary key");

  for (const auto& column_name : required_columns) {
    Require(HasRequiredColumn(table, column_name),
            "cluster catalog table missing required column");
  }

  for (const auto& column : table.columns) {
    Require(column.column_name.find("name") == std::string::npos,
            "cluster catalog table exposed user-layer name column");
    Require(column.column_name.find("comment") == std::string::npos,
            "cluster catalog table exposed comment column");
    Require(column.column_name.find("description") == std::string::npos,
            "cluster catalog table exposed description column");
    Require(column.type_name != "property_bag",
            "cluster catalog table used property-bag typing");
    Require(column.type_name != "json" && column.type_name != "jsonb",
            "cluster catalog table used untyped JSON payload");
  }
}

void TestBuiltinManifestExactShapes() {
  const auto manifest = catalog::BuiltinClusterCatalogManifestSet();
  const auto validated = catalog::ValidateClusterCatalogManifestSet(manifest);
  Require(validated.ok(), "built-in cluster catalog manifest did not validate");
  Require(manifest.engine_owned, "cluster catalog manifest set not engine-owned");
  Require(manifest.external_provider_required,
          "cluster catalog manifest set did not require external provider");
  Require(!manifest.local_runtime_execution_enabled,
          "cluster catalog manifest set enabled local runtime execution");

  const std::map<std::string, std::vector<std::string>> required_tables = {
      {"cluster.sys.catalog.node",
       {"node_uuid", "cluster_uuid", "node_role_uuid",
        "engine_instance_uuid", "default_filespace_uuid", "status",
        "catalog_epoch", "catalog_generation", "security_epoch",
        "provider_record_digest"}},
      {"cluster.sys.catalog.role",
       {"role_uuid", "cluster_uuid", "role_code", "role_generation",
        "status", "catalog_epoch", "catalog_generation",
        "provider_record_digest"}},
      {"cluster.sys.catalog.capability",
       {"capability_uuid", "cluster_uuid", "role_uuid", "capability_code",
        "capability_generation", "status", "catalog_epoch",
        "catalog_generation", "provider_record_digest"}},
      {"cluster.sys.catalog.filespace",
       {"cluster_filespace_uuid", "cluster_uuid", "filespace_uuid",
        "owner_node_uuid", "filespace_role_code", "status", "catalog_epoch",
        "catalog_generation", "provider_record_digest"}},
      {"cluster.sys.catalog.page_family",
       {"page_family_uuid", "cluster_uuid", "filespace_uuid",
        "page_family_code", "placement_profile_uuid", "status",
        "catalog_epoch", "catalog_generation", "provider_record_digest"}},
      {"cluster.sys.catalog.route",
       {"route_uuid", "cluster_uuid", "source_node_uuid",
        "target_node_uuid", "route_profile_uuid", "route_generation",
        "status", "catalog_epoch", "catalog_generation",
        "provider_record_digest"}},
      {"cluster.sys.catalog.route_decision",
       {"route_decision_uuid", "cluster_uuid", "decision_proof_uuid",
        "source_node_uuid", "target_node_uuid", "route_uuid",
        "fence_token_uuid", "status", "decision_epoch",
        "catalog_generation", "provider_record_digest"}},
      {"cluster.sys.catalog.fence_token",
       {"fence_token_uuid", "cluster_uuid", "node_uuid", "route_uuid",
        "fence_epoch", "fence_generation", "status",
        "catalog_generation", "provider_record_digest"}},
      {"cluster.sys.catalog.shard_topology",
       {"shard_uuid", "cluster_uuid", "page_family_uuid",
        "placement_profile_uuid", "primary_node_uuid", "replica_set_uuid",
        "topology_epoch", "topology_generation", "status",
        "provider_record_digest"}},
      {"cluster.sys.security.node_binding",
       {"node_binding_uuid", "cluster_uuid", "node_uuid", "principal_uuid",
        "security_policy_uuid", "credential_material_uuid", "status",
        "security_epoch", "catalog_generation", "provider_record_digest"}},
      {"cluster.sys.metrics.node_metric_profile",
       {"metric_profile_uuid", "cluster_uuid", "node_uuid",
        "metric_family_code", "retention_policy_uuid",
        "redaction_policy_uuid", "status", "metric_epoch",
        "catalog_generation", "provider_record_digest"}},
  };

  Require(manifest.tables.size() == required_tables.size(),
          "cluster catalog manifest table count changed");
  for (const auto& [path, columns] : required_tables) {
    const auto* table = FindTable(manifest, path);
    Require(table != nullptr, "cluster catalog manifest missing table");
    RequireTableShape(*table, path, columns);
  }
}

void TestRoleProfileManifests() {
  const auto manifest = catalog::BuiltinClusterCatalogManifestSet();
  const std::map<std::string, std::vector<std::string>> role_specific = {
      {"coordinator",
       {"decision_service_uuid", "leadership_epoch", "route_policy_uuid"}},
      {"storage", {"filespace_uuid", "page_family_uuid", "shard_set_uuid"}},
      {"query",
       {"executor_capability_uuid", "optimizer_profile_uuid",
        "memory_governance_uuid"}},
      {"router", {"route_table_uuid", "fence_policy_uuid", "topology_view_uuid"}},
      {"witness", {"quorum_profile_uuid", "vote_epoch", "fence_token_uuid"}},
      {"security",
       {"security_epoch", "node_binding_uuid", "credential_policy_uuid"}},
      {"metrics",
       {"metric_profile_uuid", "retention_policy_uuid",
        "redaction_policy_uuid"}},
      {"backup",
       {"backup_profile_uuid", "archive_filespace_uuid",
        "restore_reachability_uuid"}},
  };

  Require(manifest.role_profiles.size() ==
              catalog::BuiltinClusterCatalogRoleCodes().size(),
          "cluster role-profile manifest count mismatch");
  for (const auto& role_code : catalog::BuiltinClusterCatalogRoleCodes()) {
    const auto* role_profile = FindRoleProfile(manifest, role_code);
    Require(role_profile != nullptr, "missing cluster role profile manifest");
    const std::string path =
        "cluster.sys.catalog.node_role_profile_" + role_code;
    std::vector<std::string> required = {
        "role_profile_uuid",
        "cluster_uuid",
        "node_uuid",
        "role_uuid",
        "role_code",
        "capability_set_uuid",
        "placement_profile_uuid",
        "status",
        "catalog_epoch",
        "catalog_generation",
        "provider_record_digest"};
    const auto found = role_specific.find(role_code);
    Require(found != role_specific.end(), "role-specific columns missing");
    required.insert(required.end(), found->second.begin(), found->second.end());
    RequireTableShape(role_profile->table, path, required);
    const auto validated =
        catalog::ValidateClusterRoleProfileManifest(*role_profile);
    Require(validated.ok(), "role profile manifest did not validate");
  }
}

catalog::ClusterCatalogColumnManifest Column(std::string column_name,
                                             std::string type_name) {
  catalog::ClusterCatalogColumnManifest column;
  column.column_name = std::move(column_name);
  column.type_name = std::move(type_name);
  column.required = true;
  column.provider_supplied = true;
  return column;
}

void TestManifestRefusals() {
  auto local_execution = catalog::BuiltinClusterCatalogManifestSet();
  local_execution.local_runtime_execution_enabled = true;
  const auto local_execution_result =
      catalog::ValidateClusterCatalogManifestSet(local_execution);
  Require(!local_execution_result.ok() &&
              local_execution_result.diagnostic.diagnostic_code ==
                  "SB-CLUSTER-CATALOG-MANIFEST-LOCAL-EXECUTION-REFUSED",
          "cluster manifest set accepted local runtime execution");

  auto table_execution = catalog::BuiltinClusterCatalogManifestSet();
  table_execution.tables.front().local_runtime_execution_enabled = true;
  const auto table_execution_result =
      catalog::ValidateClusterCatalogManifestSet(table_execution);
  Require(!table_execution_result.ok() &&
              table_execution_result.diagnostic.diagnostic_code ==
                  "SB-CLUSTER-CATALOG-MANIFEST-LOCAL-EXECUTION-REFUSED",
          "cluster table manifest accepted local runtime execution");

  auto missing_status = catalog::BuiltinClusterCatalogManifestSet();
  auto& columns = missing_status.tables.front().columns;
  columns.erase(std::remove_if(columns.begin(),
                               columns.end(),
                               [](const auto& column) {
                                 return column.column_name == "status";
                               }),
                columns.end());
  const auto missing_status_result =
      catalog::ValidateClusterCatalogManifestSet(missing_status);
  Require(!missing_status_result.ok() &&
              missing_status_result.diagnostic.diagnostic_code ==
                  "SB-CLUSTER-CATALOG-MANIFEST-STATUS-REQUIRED",
          "cluster table manifest accepted missing status column");

  auto name_column = catalog::BuiltinClusterCatalogManifestSet();
  name_column.tables.front().columns.push_back(Column("node_name", "text"));
  const auto name_column_result =
      catalog::ValidateClusterCatalogManifestSet(name_column);
  Require(!name_column_result.ok() &&
              name_column_result.diagnostic.diagnostic_code ==
                  "SB-CLUSTER-CATALOG-MANIFEST-NAME-COLUMN-REFUSED",
          "cluster table manifest accepted user-layer name column");

  auto property_bag = catalog::BuiltinClusterRoleProfileManifests().front();
  property_bag.table.columns.push_back(
      Column("role_properties", "property_bag"));
  const auto property_bag_result =
      catalog::ValidateClusterRoleProfileManifest(property_bag);
  Require(!property_bag_result.ok() &&
              property_bag_result.diagnostic.diagnostic_code ==
                  "SB-CLUSTER-CATALOG-MANIFEST-PROPERTY-BAG-REFUSED",
          "cluster role profile accepted untyped property bag");

  auto missing_role = catalog::BuiltinClusterCatalogManifestSet();
  missing_role.role_profiles.pop_back();
  const auto missing_role_result =
      catalog::ValidateClusterCatalogManifestSet(missing_role);
  Require(!missing_role_result.ok() &&
              missing_role_result.diagnostic.diagnostic_code ==
                  "SB-CLUSTER-CATALOG-MANIFEST-ROLE-REQUIRED",
          "cluster manifest set accepted missing role profile");
}

}  // namespace

int main() {
  TestBuiltinManifestExactShapes();
  TestRoleProfileManifests();
  TestManifestRefusals();
  return EXIT_SUCCESS;
}
