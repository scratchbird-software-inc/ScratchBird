// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "cluster_catalog_manifest.hpp"

#include <algorithm>
#include <cctype>
#include <set>
#include <utility>
#include <vector>

namespace scratchbird::core::catalog {
namespace {

using scratchbird::core::platform::DiagnosticArgument;
using scratchbird::core::platform::MakeDiagnostic;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;

Status ManifestOkStatus() {
  return {StatusCode::ok, Severity::info, Subsystem::catalog};
}

Status ManifestErrorStatus() {
  return {StatusCode::platform_required_feature_missing,
          Severity::error,
          Subsystem::catalog};
}

ClusterCatalogColumnManifest Column(std::string column_name,
                                    std::string type_name,
                                    bool uuid_identity = false,
                                    bool authority_reference = false) {
  ClusterCatalogColumnManifest column;
  column.column_name = std::move(column_name);
  column.type_name = std::move(type_name);
  column.required = true;
  column.uuid_identity = uuid_identity;
  column.authority_reference = authority_reference;
  column.provider_supplied = true;
  return column;
}

std::vector<std::string> ColumnNames(
    const std::vector<ClusterCatalogColumnManifest>& columns) {
  std::vector<std::string> names;
  names.reserve(columns.size());
  for (const ClusterCatalogColumnManifest& column : columns) {
    names.push_back(column.column_name);
  }
  return names;
}

ClusterCatalogTableManifest Table(
    std::string schema_path,
    std::string table_name,
    std::string stable_table_id,
    std::string record_family,
    std::vector<ClusterCatalogColumnManifest> columns,
    std::vector<std::string> primary_key_columns) {
  ClusterCatalogTableManifest table;
  table.schema_path = std::move(schema_path);
  table.table_name = std::move(table_name);
  table.stable_table_id = std::move(stable_table_id);
  table.record_family = std::move(record_family);
  table.manifest_version = kClusterCatalogManifestVersionCurrent;
  table.engine_owned = true;
  table.cluster_shared = true;
  table.external_provider_bound = true;
  table.local_runtime_execution_enabled = false;
  table.mutable_by_local_core = false;
  table.uuid_only_identity = true;
  table.required_columns = ColumnNames(columns);
  table.columns = std::move(columns);
  table.primary_key_columns = std::move(primary_key_columns);
  return table;
}

ClusterCatalogManifestValidationResult ManifestError(
    std::string diagnostic_code,
    std::string message_key,
    std::string detail = {}) {
  ClusterCatalogManifestValidationResult result;
  result.status = ManifestErrorStatus();
  result.diagnostic = MakeClusterCatalogManifestDiagnostic(
      result.status,
      std::move(diagnostic_code),
      std::move(message_key),
      std::move(detail));
  return result;
}

ClusterCatalogTableValidationResult TableError(std::string diagnostic_code,
                                               std::string message_key,
                                               std::string detail = {}) {
  ClusterCatalogTableValidationResult result;
  result.status = ManifestErrorStatus();
  result.diagnostic = MakeClusterCatalogManifestDiagnostic(
      result.status,
      std::move(diagnostic_code),
      std::move(message_key),
      std::move(detail));
  return result;
}

std::string Lower(std::string value) {
  for (char& ch : value) {
    ch = static_cast<char>(
        std::tolower(static_cast<unsigned char>(ch)));
  }
  return value;
}

bool IsForbiddenUserLayerColumn(const std::string& column_name) {
  const std::string lower = Lower(column_name);
  return lower.find("name") != std::string::npos ||
         lower.find("comment") != std::string::npos ||
         lower.find("description") != std::string::npos;
}

bool IsUntypedPropertyBagColumn(const ClusterCatalogColumnManifest& column) {
  const std::string type = Lower(column.type_name);
  const std::string name = Lower(column.column_name);
  return type == "property_bag" || type == "json" || type == "jsonb" ||
         name.find("property_bag") != std::string::npos ||
         name.find("properties") != std::string::npos;
}

bool HasColumn(const ClusterCatalogTableManifest& table,
               const std::string& column_name) {
  return FindClusterCatalogColumn(table, column_name) != nullptr;
}

bool HasRequiredColumn(const ClusterCatalogTableManifest& table,
                       const std::string& column_name) {
  const auto* column = FindClusterCatalogColumn(table, column_name);
  return column != nullptr && column->required;
}

bool IsKnownRoleCode(const std::string& role_code) {
  const auto& role_codes = BuiltinClusterCatalogRoleCodes();
  return std::find(role_codes.begin(), role_codes.end(), role_code) !=
         role_codes.end();
}

std::vector<ClusterCatalogColumnManifest> RoleProfileCommonColumns() {
  return {
      Column("role_profile_uuid", "uuid", true),
      Column("cluster_uuid", "uuid", true, true),
      Column("node_uuid", "uuid", true, true),
      Column("role_uuid", "uuid", true, true),
      Column("role_code", "role_code"),
      Column("capability_set_uuid", "uuid", true, true),
      Column("placement_profile_uuid", "uuid", true, true),
      Column("status", "status_code"),
      Column("catalog_epoch", "uint64"),
      Column("catalog_generation", "uint64"),
      Column("provider_record_digest", "digest")};
}

ClusterRoleProfileManifest RoleProfile(
    std::string role_code,
    std::vector<ClusterCatalogColumnManifest> role_columns) {
  std::vector<ClusterCatalogColumnManifest> columns = RoleProfileCommonColumns();
  columns.insert(columns.end(), role_columns.begin(), role_columns.end());

  ClusterRoleProfileManifest role_profile;
  role_profile.role_code = std::move(role_code);
  role_profile.table = Table("cluster.sys.catalog",
                             "node_role_profile_" + role_profile.role_code,
                             "cluster_catalog.node_role_profile." +
                                 role_profile.role_code,
                             "node_role_profile",
                             std::move(columns),
                             {"role_profile_uuid"});
  return role_profile;
}

const std::vector<std::string>& RequiredBaseTablePaths() {
  static const std::vector<std::string> paths = {
      "cluster.sys.catalog.node",
      "cluster.sys.catalog.role",
      "cluster.sys.catalog.capability",
      "cluster.sys.catalog.filespace",
      "cluster.sys.catalog.page_family",
      "cluster.sys.catalog.route",
      "cluster.sys.catalog.route_decision",
      "cluster.sys.catalog.fence_token",
      "cluster.sys.catalog.shard_topology",
      "cluster.sys.security.node_binding",
      "cluster.sys.metrics.node_metric_profile"};
  return paths;
}

}  // namespace

const std::vector<std::string>& BuiltinClusterCatalogRoleCodes() {
  static const std::vector<std::string> role_codes = {
      "coordinator",
      "storage",
      "query",
      "router",
      "witness",
      "security",
      "metrics",
      "backup"};
  return role_codes;
}

const std::vector<ClusterCatalogTableManifest>&
BuiltinClusterCatalogTableManifests() {
  static const std::vector<ClusterCatalogTableManifest> tables = {
      Table("cluster.sys.catalog",
            "node",
            "cluster_catalog.node",
            "node",
            {Column("node_uuid", "uuid", true),
             Column("cluster_uuid", "uuid", true, true),
             Column("node_role_uuid", "uuid", true, true),
             Column("engine_instance_uuid", "uuid", true, true),
             Column("default_filespace_uuid", "uuid", true, true),
             Column("status", "status_code"),
             Column("catalog_epoch", "uint64"),
             Column("catalog_generation", "uint64"),
             Column("security_epoch", "uint64"),
             Column("provider_record_digest", "digest")},
            {"node_uuid"}),
      Table("cluster.sys.catalog",
            "role",
            "cluster_catalog.role",
            "role",
            {Column("role_uuid", "uuid", true),
             Column("cluster_uuid", "uuid", true, true),
             Column("role_code", "role_code"),
             Column("role_generation", "uint64"),
             Column("status", "status_code"),
             Column("catalog_epoch", "uint64"),
             Column("catalog_generation", "uint64"),
             Column("provider_record_digest", "digest")},
            {"role_uuid"}),
      Table("cluster.sys.catalog",
            "capability",
            "cluster_catalog.capability",
            "capability",
            {Column("capability_uuid", "uuid", true),
             Column("cluster_uuid", "uuid", true, true),
             Column("role_uuid", "uuid", true, true),
             Column("capability_code", "capability_code"),
             Column("capability_generation", "uint64"),
             Column("status", "status_code"),
             Column("catalog_epoch", "uint64"),
             Column("catalog_generation", "uint64"),
             Column("provider_record_digest", "digest")},
            {"capability_uuid"}),
      Table("cluster.sys.catalog",
            "filespace",
            "cluster_catalog.filespace",
            "filespace",
            {Column("cluster_filespace_uuid", "uuid", true),
             Column("cluster_uuid", "uuid", true, true),
             Column("filespace_uuid", "uuid", true, true),
             Column("owner_node_uuid", "uuid", true, true),
             Column("filespace_role_code", "filespace_role_code"),
             Column("status", "status_code"),
             Column("catalog_epoch", "uint64"),
             Column("catalog_generation", "uint64"),
             Column("provider_record_digest", "digest")},
            {"cluster_filespace_uuid"}),
      Table("cluster.sys.catalog",
            "page_family",
            "cluster_catalog.page_family",
            "page_family",
            {Column("page_family_uuid", "uuid", true),
             Column("cluster_uuid", "uuid", true, true),
             Column("filespace_uuid", "uuid", true, true),
             Column("page_family_code", "page_family_code"),
             Column("placement_profile_uuid", "uuid", true, true),
             Column("status", "status_code"),
             Column("catalog_epoch", "uint64"),
             Column("catalog_generation", "uint64"),
             Column("provider_record_digest", "digest")},
            {"page_family_uuid"}),
      Table("cluster.sys.catalog",
            "route",
            "cluster_catalog.route",
            "route",
            {Column("route_uuid", "uuid", true),
             Column("cluster_uuid", "uuid", true, true),
             Column("source_node_uuid", "uuid", true, true),
             Column("target_node_uuid", "uuid", true, true),
             Column("route_profile_uuid", "uuid", true, true),
             Column("route_generation", "uint64"),
             Column("status", "status_code"),
             Column("catalog_epoch", "uint64"),
             Column("catalog_generation", "uint64"),
             Column("provider_record_digest", "digest")},
            {"route_uuid"}),
      Table("cluster.sys.catalog",
            "route_decision",
            "cluster_catalog.route_decision",
            "route_decision",
            {Column("route_decision_uuid", "uuid", true),
             Column("cluster_uuid", "uuid", true, true),
             Column("decision_proof_uuid", "uuid", true, true),
             Column("source_node_uuid", "uuid", true, true),
             Column("target_node_uuid", "uuid", true, true),
             Column("route_uuid", "uuid", true, true),
             Column("fence_token_uuid", "uuid", true, true),
             Column("status", "status_code"),
             Column("decision_epoch", "uint64"),
             Column("catalog_generation", "uint64"),
             Column("provider_record_digest", "digest")},
            {"route_decision_uuid"}),
      Table("cluster.sys.catalog",
            "fence_token",
            "cluster_catalog.fence_token",
            "fence_token",
            {Column("fence_token_uuid", "uuid", true),
             Column("cluster_uuid", "uuid", true, true),
             Column("node_uuid", "uuid", true, true),
             Column("route_uuid", "uuid", true, true),
             Column("fence_epoch", "uint64"),
             Column("fence_generation", "uint64"),
             Column("status", "status_code"),
             Column("catalog_generation", "uint64"),
             Column("provider_record_digest", "digest")},
            {"fence_token_uuid"}),
      Table("cluster.sys.catalog",
            "shard_topology",
            "cluster_catalog.shard_topology",
            "shard_topology",
            {Column("shard_uuid", "uuid", true),
             Column("cluster_uuid", "uuid", true, true),
             Column("page_family_uuid", "uuid", true, true),
             Column("placement_profile_uuid", "uuid", true, true),
             Column("primary_node_uuid", "uuid", true, true),
             Column("replica_set_uuid", "uuid", true, true),
             Column("topology_epoch", "uint64"),
             Column("topology_generation", "uint64"),
             Column("status", "status_code"),
             Column("provider_record_digest", "digest")},
            {"shard_uuid"}),
      Table("cluster.sys.security",
            "node_binding",
            "cluster_security.node_binding",
            "node_binding",
            {Column("node_binding_uuid", "uuid", true),
             Column("cluster_uuid", "uuid", true, true),
             Column("node_uuid", "uuid", true, true),
             Column("principal_uuid", "uuid", true, true),
             Column("security_policy_uuid", "uuid", true, true),
             Column("credential_material_uuid", "uuid", true, true),
             Column("status", "status_code"),
             Column("security_epoch", "uint64"),
             Column("catalog_generation", "uint64"),
             Column("provider_record_digest", "digest")},
            {"node_binding_uuid"}),
      Table("cluster.sys.metrics",
            "node_metric_profile",
            "cluster_metrics.node_metric_profile",
            "node_metric_profile",
            {Column("metric_profile_uuid", "uuid", true),
             Column("cluster_uuid", "uuid", true, true),
             Column("node_uuid", "uuid", true, true),
             Column("metric_family_code", "metric_family_code"),
             Column("retention_policy_uuid", "uuid", true, true),
             Column("redaction_policy_uuid", "uuid", true, true),
             Column("status", "status_code"),
             Column("metric_epoch", "uint64"),
             Column("catalog_generation", "uint64"),
             Column("provider_record_digest", "digest")},
            {"metric_profile_uuid"}),
  };
  return tables;
}

const std::vector<ClusterRoleProfileManifest>&
BuiltinClusterRoleProfileManifests() {
  static const std::vector<ClusterRoleProfileManifest> role_profiles = {
      RoleProfile("coordinator",
                  {Column("decision_service_uuid", "uuid", true, true),
                   Column("leadership_epoch", "uint64"),
                   Column("route_policy_uuid", "uuid", true, true)}),
      RoleProfile("storage",
                  {Column("filespace_uuid", "uuid", true, true),
                   Column("page_family_uuid", "uuid", true, true),
                   Column("shard_set_uuid", "uuid", true, true)}),
      RoleProfile("query",
                  {Column("executor_capability_uuid", "uuid", true, true),
                   Column("optimizer_profile_uuid", "uuid", true, true),
                   Column("memory_governance_uuid", "uuid", true, true)}),
      RoleProfile("router",
                  {Column("route_table_uuid", "uuid", true, true),
                   Column("fence_policy_uuid", "uuid", true, true),
                   Column("topology_view_uuid", "uuid", true, true)}),
      RoleProfile("witness",
                  {Column("quorum_profile_uuid", "uuid", true, true),
                   Column("vote_epoch", "uint64"),
                   Column("fence_token_uuid", "uuid", true, true)}),
      RoleProfile("security",
                  {Column("security_epoch", "uint64"),
                   Column("node_binding_uuid", "uuid", true, true),
                   Column("credential_policy_uuid", "uuid", true, true)}),
      RoleProfile("metrics",
                  {Column("metric_profile_uuid", "uuid", true, true),
                   Column("retention_policy_uuid", "uuid", true, true),
                   Column("redaction_policy_uuid", "uuid", true, true)}),
      RoleProfile("backup",
                  {Column("backup_profile_uuid", "uuid", true, true),
                   Column("archive_filespace_uuid", "uuid", true, true),
                   Column("restore_reachability_uuid", "uuid", true, true)}),
  };
  return role_profiles;
}

ClusterCatalogManifestSet BuiltinClusterCatalogManifestSet() {
  ClusterCatalogManifestSet manifest;
  manifest.manifest_version = kClusterCatalogManifestVersionCurrent;
  manifest.engine_owned = true;
  manifest.external_provider_required = true;
  manifest.local_runtime_execution_enabled = false;
  manifest.tables = BuiltinClusterCatalogTableManifests();
  manifest.role_profiles = BuiltinClusterRoleProfileManifests();
  return manifest;
}

std::string ClusterCatalogFullTablePath(
    const ClusterCatalogTableManifest& table) {
  if (table.schema_path.empty()) {
    return table.table_name;
  }
  if (table.table_name.empty()) {
    return table.schema_path;
  }
  return table.schema_path + "." + table.table_name;
}

const ClusterCatalogColumnManifest* FindClusterCatalogColumn(
    const ClusterCatalogTableManifest& table,
    const std::string& column_name) {
  for (const ClusterCatalogColumnManifest& column : table.columns) {
    if (column.column_name == column_name) {
      return &column;
    }
  }
  return nullptr;
}

ClusterCatalogTableValidationResult ValidateClusterCatalogTableManifest(
    const ClusterCatalogTableManifest& table) {
  if (table.manifest_version != kClusterCatalogManifestVersionCurrent ||
      table.schema_path.empty() || table.table_name.empty() ||
      table.stable_table_id.empty() || table.record_family.empty() ||
      table.columns.empty() || table.primary_key_columns.empty()) {
    return TableError("SB-CLUSTER-CATALOG-MANIFEST-INCOMPLETE",
                      "catalog.cluster_manifest.incomplete",
                      ClusterCatalogFullTablePath(table));
  }
  if (!table.engine_owned || !table.cluster_shared ||
      !table.external_provider_bound || !table.uuid_only_identity) {
    return TableError("SB-CLUSTER-CATALOG-MANIFEST-AUTHORITY-REFUSED",
                      "catalog.cluster_manifest.authority_refused",
                      ClusterCatalogFullTablePath(table));
  }
  if (table.local_runtime_execution_enabled || table.mutable_by_local_core) {
    return TableError("SB-CLUSTER-CATALOG-MANIFEST-LOCAL-EXECUTION-REFUSED",
                      "catalog.cluster_manifest.local_execution_refused",
                      ClusterCatalogFullTablePath(table));
  }
  if (!HasRequiredColumn(table, "status")) {
    return TableError("SB-CLUSTER-CATALOG-MANIFEST-STATUS-REQUIRED",
                      "catalog.cluster_manifest.status_required",
                      ClusterCatalogFullTablePath(table));
  }

  for (const std::string& column_name : table.primary_key_columns) {
    if (!HasRequiredColumn(table, column_name)) {
      return TableError("SB-CLUSTER-CATALOG-MANIFEST-PRIMARY-KEY-INVALID",
                        "catalog.cluster_manifest.primary_key_invalid",
                        column_name);
    }
  }

  for (const std::string& column_name : table.required_columns) {
    if (!HasRequiredColumn(table, column_name)) {
      return TableError("SB-CLUSTER-CATALOG-MANIFEST-COLUMN-REQUIRED",
                        "catalog.cluster_manifest.column_required",
                        column_name);
    }
  }

  std::set<std::string> names;
  bool has_uuid_identity = false;
  for (const ClusterCatalogColumnManifest& column : table.columns) {
    if (column.column_name.empty() || column.type_name.empty()) {
      return TableError("SB-CLUSTER-CATALOG-MANIFEST-COLUMN-INCOMPLETE",
                        "catalog.cluster_manifest.column_incomplete",
                        ClusterCatalogFullTablePath(table));
    }
    if (!names.insert(column.column_name).second) {
      return TableError("SB-CLUSTER-CATALOG-MANIFEST-DUPLICATE-COLUMN",
                        "catalog.cluster_manifest.duplicate_column",
                        column.column_name);
    }
    if (IsForbiddenUserLayerColumn(column.column_name)) {
      return TableError("SB-CLUSTER-CATALOG-MANIFEST-NAME-COLUMN-REFUSED",
                        "catalog.cluster_manifest.name_column_refused",
                        column.column_name);
    }
    if (IsUntypedPropertyBagColumn(column)) {
      return TableError("SB-CLUSTER-CATALOG-MANIFEST-PROPERTY-BAG-REFUSED",
                        "catalog.cluster_manifest.property_bag_refused",
                        column.column_name);
    }
    has_uuid_identity = has_uuid_identity || column.uuid_identity;
  }
  if (!has_uuid_identity) {
    return TableError("SB-CLUSTER-CATALOG-MANIFEST-UUID-IDENTITY-REQUIRED",
                      "catalog.cluster_manifest.uuid_identity_required",
                      ClusterCatalogFullTablePath(table));
  }

  ClusterCatalogTableValidationResult result;
  result.status = ManifestOkStatus();
  result.table = table;
  return result;
}

ClusterCatalogTableValidationResult ValidateClusterRoleProfileManifest(
    const ClusterRoleProfileManifest& role_profile) {
  if (!IsKnownRoleCode(role_profile.role_code)) {
    return TableError("SB-CLUSTER-CATALOG-MANIFEST-ROLE-UNKNOWN",
                      "catalog.cluster_manifest.role_unknown",
                      role_profile.role_code);
  }
  const std::string expected_table_name =
      "node_role_profile_" + role_profile.role_code;
  if (role_profile.table.schema_path != "cluster.sys.catalog" ||
      role_profile.table.table_name != expected_table_name ||
      role_profile.table.record_family != "node_role_profile") {
    return TableError("SB-CLUSTER-CATALOG-MANIFEST-ROLE-TABLE-MISMATCH",
                      "catalog.cluster_manifest.role_table_mismatch",
                      expected_table_name);
  }
  if (!HasRequiredColumn(role_profile.table, "role_profile_uuid") ||
      !HasRequiredColumn(role_profile.table, "node_uuid") ||
      !HasRequiredColumn(role_profile.table, "role_uuid") ||
      !HasRequiredColumn(role_profile.table, "role_code") ||
      !HasRequiredColumn(role_profile.table, "capability_set_uuid")) {
    return TableError("SB-CLUSTER-CATALOG-MANIFEST-ROLE-COLUMN-REQUIRED",
                      "catalog.cluster_manifest.role_column_required",
                      expected_table_name);
  }
  return ValidateClusterCatalogTableManifest(role_profile.table);
}

ClusterCatalogManifestValidationResult ValidateClusterCatalogManifestSet(
    const ClusterCatalogManifestSet& manifest) {
  if (manifest.manifest_version != kClusterCatalogManifestVersionCurrent ||
      !manifest.engine_owned || !manifest.external_provider_required) {
    return ManifestError("SB-CLUSTER-CATALOG-MANIFEST-SET-INCOMPLETE",
                         "catalog.cluster_manifest.set_incomplete");
  }
  if (manifest.local_runtime_execution_enabled) {
    return ManifestError("SB-CLUSTER-CATALOG-MANIFEST-LOCAL-EXECUTION-REFUSED",
                         "catalog.cluster_manifest.local_execution_refused");
  }

  std::set<std::string> table_paths;
  for (const ClusterCatalogTableManifest& table : manifest.tables) {
    const auto validated = ValidateClusterCatalogTableManifest(table);
    if (!validated.ok()) {
      ClusterCatalogManifestValidationResult result;
      result.status = validated.status;
      result.diagnostic = validated.diagnostic;
      return result;
    }
    const std::string path = ClusterCatalogFullTablePath(table);
    if (!table_paths.insert(path).second) {
      return ManifestError("SB-CLUSTER-CATALOG-MANIFEST-DUPLICATE-TABLE",
                           "catalog.cluster_manifest.duplicate_table",
                           path);
    }
  }

  for (const std::string& required_path : RequiredBaseTablePaths()) {
    if (table_paths.find(required_path) == table_paths.end()) {
      return ManifestError("SB-CLUSTER-CATALOG-MANIFEST-TABLE-REQUIRED",
                           "catalog.cluster_manifest.table_required",
                           required_path);
    }
  }

  std::set<std::string> role_codes;
  for (const ClusterRoleProfileManifest& role_profile : manifest.role_profiles) {
    const auto validated = ValidateClusterRoleProfileManifest(role_profile);
    if (!validated.ok()) {
      ClusterCatalogManifestValidationResult result;
      result.status = validated.status;
      result.diagnostic = validated.diagnostic;
      return result;
    }
    if (!role_codes.insert(role_profile.role_code).second) {
      return ManifestError("SB-CLUSTER-CATALOG-MANIFEST-DUPLICATE-ROLE",
                           "catalog.cluster_manifest.duplicate_role",
                           role_profile.role_code);
    }
  }

  for (const std::string& role_code : BuiltinClusterCatalogRoleCodes()) {
    if (role_codes.find(role_code) == role_codes.end()) {
      return ManifestError("SB-CLUSTER-CATALOG-MANIFEST-ROLE-REQUIRED",
                           "catalog.cluster_manifest.role_required",
                           role_code);
    }
  }

  ClusterCatalogManifestValidationResult result;
  result.status = ManifestOkStatus();
  result.manifest = manifest;
  return result;
}

DiagnosticRecord MakeClusterCatalogManifestDiagnostic(
    Status status,
    std::string diagnostic_code,
    std::string message_key,
    std::string detail) {
  std::vector<DiagnosticArgument> arguments;
  if (!detail.empty()) {
    arguments.push_back({"detail", std::move(detail)});
  }

  return MakeDiagnostic(status.code,
                        status.severity,
                        status.subsystem,
                        std::move(diagnostic_code),
                        std::move(message_key),
                        std::move(arguments),
                        {},
                        "core.catalog.cluster_catalog_manifest");
}

}  // namespace scratchbird::core::catalog
