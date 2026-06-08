// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "cluster_schema_gating.hpp"

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

Status SchemaOkStatus() {
  return {StatusCode::ok, Severity::info, Subsystem::catalog};
}

Status SchemaErrorStatus() {
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
  column.provider_supplied = false;
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

std::string ProjectionTableNameFor(
    const ClusterCatalogTableManifest& source_table) {
  std::string path = ClusterCatalogFullTablePath(source_table);
  const std::string prefix = "cluster.sys.";
  if (path.rfind(prefix, 0) == 0) {
    path.erase(0, prefix.size());
  }
  for (char& ch : path) {
    if (ch == '.') {
      ch = '_';
    }
  }
  return path;
}

ClusterCacheProjectionManifest ProjectionFor(
    const ClusterCatalogTableManifest& source_table) {
  std::vector<ClusterCatalogColumnManifest> columns = {
      Column("projection_uuid", "uuid", true),
      Column("source_cluster_uuid", "uuid", true, true),
      Column("source_record_uuid", "uuid", true, true),
      Column("source_table_path", "text"),
      Column("source_authority_epoch", "uint64"),
      Column("source_generation", "uint64"),
      Column("source_digest", "digest"),
      Column("invalidation_epoch", "uint64"),
      Column("freshness_epoch_millis", "uint64"),
      Column("projection_generated_epoch_millis", "uint64"),
      Column("status", "status_code")};

  ClusterCacheProjectionManifest projection;
  projection.schema_path = "sys.catalog.cluster_cache";
  projection.table_name = ProjectionTableNameFor(source_table);
  projection.stable_projection_id =
      "cluster_cache." + projection.table_name;
  projection.source_cluster_table_path =
      ClusterCatalogFullTablePath(source_table);
  projection.projection_only = true;
  projection.source_authority_required = true;
  projection.cluster_authority = false;
  projection.local_runtime_execution_enabled = false;
  projection.required_columns = ColumnNames(columns);
  projection.columns = std::move(columns);
  projection.primary_key_columns = {"projection_uuid"};
  return projection;
}

std::string ReadableViewNameFor(
    const ClusterCatalogTableManifest& source_table) {
  std::string path = ClusterCatalogFullTablePath(source_table);
  const std::string prefix = "cluster.sys.";
  if (path.rfind(prefix, 0) == 0) {
    path.erase(0, prefix.size());
  }
  for (char& ch : path) {
    if (ch == '.') {
      ch = '_';
    }
  }
  return path;
}

ClusterCatalogReadableViewDefinition ReadableViewFor(
    const ClusterCatalogTableManifest& source_table) {
  ClusterCatalogReadableViewDefinition view;
  view.schema_path = "cluster.sys.catalog_readable";
  view.view_name = ReadableViewNameFor(source_table);
  view.stable_view_id = "cluster_catalog_readable." + view.view_name;
  view.source_cluster_table_path = ClusterCatalogFullTablePath(source_table);
  view.external_provider_bound = true;
  view.requires_joined_cluster_authority = true;
  view.internal_only_without_joined_authority = true;
  view.resolver_backed = true;
  view.comment_backed = true;
  view.local_runtime_execution_enabled = false;
  view.columns = {
      {"source_record_uuid", "base", source_table.primary_key_columns.front(),
       true, false, false, false},
      {"source_table_path", "base", "table_path", false, false, false, false},
      {"status", "base", "status", false, false, false, true},
      {"display_name", "resolver", "display_name", false, true, false, true},
      {"normalized_lookup_key", "resolver", "normalized_lookup_key", false,
       true, false, true},
      {"exact_lookup_key", "resolver", "exact_lookup_key", false, true, false,
       true},
      {"comment_text", "comment", "comment_text", false, false, true, true},
      {"catalog_generation", "resolver", "catalog_generation", false, true,
       false, false}};
  return view;
}

ClusterCacheProjectionValidationResult ProjectionError(
    std::string diagnostic_code,
    std::string message_key,
    std::string detail = {}) {
  ClusterCacheProjectionValidationResult result;
  result.status = SchemaErrorStatus();
  result.diagnostic = MakeClusterSchemaGatingDiagnostic(
      result.status,
      std::move(diagnostic_code),
      std::move(message_key),
      std::move(detail));
  return result;
}

ClusterCacheProjectionSetValidationResult ProjectionSetError(
    std::string diagnostic_code,
    std::string message_key,
    std::string detail = {}) {
  ClusterCacheProjectionSetValidationResult result;
  result.status = SchemaErrorStatus();
  result.diagnostic = MakeClusterSchemaGatingDiagnostic(
      result.status,
      std::move(diagnostic_code),
      std::move(message_key),
      std::move(detail));
  return result;
}

bool HasRequiredColumn(const ClusterCacheProjectionManifest& projection,
                       const std::string& column_name) {
  const auto* column = FindClusterCacheProjectionColumn(projection, column_name);
  return column != nullptr && column->required;
}

std::set<std::string> ExpectedProjectionSourcePaths() {
  std::set<std::string> paths;
  for (const ClusterCatalogTableManifest& table :
       BuiltinClusterCatalogTableManifests()) {
    paths.insert(ClusterCatalogFullTablePath(table));
  }
  for (const ClusterRoleProfileManifest& role_profile :
       BuiltinClusterRoleProfileManifests()) {
    paths.insert(ClusterCatalogFullTablePath(role_profile.table));
  }
  return paths;
}

bool KnownClusterCatalogSourcePath(const std::string& table_path) {
  const auto expected = ExpectedProjectionSourcePaths();
  return expected.count(table_path) != 0;
}

}  // namespace

ClusterSchemaRootDecision EvaluateClusterSchemaRootAccess(
    const ClusterSchemaRootRequest& request) {
  ClusterSchemaRootDecision decision;
  decision.external_provider_required = true;
  decision.local_runtime_execution_enabled = false;

  if (request.local_runtime_execution_requested) {
    decision.status = SchemaErrorStatus();
    decision.failed_closed = true;
    decision.diagnostic = MakeClusterSchemaGatingDiagnostic(
        decision.status,
        "SB-CLUSTER-SCHEMA-LOCAL-EXECUTION-REFUSED",
        "catalog.cluster_schema.local_execution_refused");
    decision.evidence.push_back("cluster_local_execution_enabled=false");
    decision.evidence.push_back("cluster_schema_present=false");
    return decision;
  }

  if (!request.joined_cluster_catalog_state ||
      !request.cluster_authority_available ||
      !request.external_provider_available) {
    decision.status = SchemaErrorStatus();
    decision.failed_closed = true;
    decision.diagnostic = MakeClusterSchemaGatingDiagnostic(
        decision.status,
        kClusterSchemaAbsentDiagnosticCode,
        "catalog.cluster_schema.absent");
    decision.evidence.push_back("cluster_schema_present=false");
    decision.evidence.push_back("cluster_paths_fail_closed=true");
    decision.evidence.push_back("external_provider_required=true");
    return decision;
  }

  decision.status = SchemaOkStatus();
  decision.schema_present = true;
  decision.failed_closed = false;
  decision.schema_paths = {"cluster",
                           "cluster.sys",
                           "cluster.sys.catalog",
                           "cluster.sys.security",
                           "cluster.sys.metrics"};
  decision.evidence.push_back("cluster_schema_present=true");
  decision.evidence.push_back("joined_cluster_catalog_state=true");
  decision.evidence.push_back("external_provider_required=true");
  decision.evidence.push_back("cluster_local_execution_enabled=false");
  return decision;
}

const std::vector<ClusterCacheProjectionManifest>&
BuiltinClusterCacheProjectionManifests() {
  static const std::vector<ClusterCacheProjectionManifest> projections = [] {
    std::vector<ClusterCacheProjectionManifest> built;
    for (const ClusterCatalogTableManifest& table :
         BuiltinClusterCatalogTableManifests()) {
      built.push_back(ProjectionFor(table));
    }
    for (const ClusterRoleProfileManifest& role_profile :
         BuiltinClusterRoleProfileManifests()) {
      built.push_back(ProjectionFor(role_profile.table));
    }
    return built;
  }();
  return projections;
}

ClusterCacheProjectionManifestSet BuiltinClusterCacheProjectionManifestSet() {
  ClusterCacheProjectionManifestSet projections;
  projections.projection_only = true;
  projections.source_authority_required = true;
  projections.local_runtime_execution_enabled = false;
  projections.projections = BuiltinClusterCacheProjectionManifests();
  return projections;
}

const std::vector<ClusterCatalogReadableViewDefinition>&
BuiltinClusterCatalogReadableViewDefinitions() {
  static const std::vector<ClusterCatalogReadableViewDefinition> views = [] {
    std::vector<ClusterCatalogReadableViewDefinition> built;
    for (const ClusterCatalogTableManifest& table :
         BuiltinClusterCatalogTableManifests()) {
      built.push_back(ReadableViewFor(table));
    }
    for (const ClusterRoleProfileManifest& role_profile :
         BuiltinClusterRoleProfileManifests()) {
      built.push_back(ReadableViewFor(role_profile.table));
    }
    return built;
  }();
  return views;
}

std::string ClusterCatalogReadableViewFullPath(
    const ClusterCatalogReadableViewDefinition& view) {
  if (view.schema_path.empty()) {
    return view.view_name;
  }
  if (view.view_name.empty()) {
    return view.schema_path;
  }
  return view.schema_path + "." + view.view_name;
}

ClusterCatalogReadableViewAccessResult EvaluateClusterCatalogReadableViewAccess(
    const ClusterCatalogReadableViewAccessRequest& request) {
  ClusterCatalogReadableViewAccessResult result;
  result.external_provider_required = true;
  result.joined_cluster_authority_required = true;
  result.local_runtime_execution_enabled = false;

  if (!request.joined_cluster_catalog_state ||
      !request.cluster_authority_available ||
      !request.external_provider_available) {
    result.status = SchemaErrorStatus();
    result.views_present = false;
    result.failed_closed = true;
    result.diagnostic = MakeClusterSchemaGatingDiagnostic(
        result.status,
        "SB-CLUSTER-CATALOG-READABLE-VIEWS-AUTHORITY-REQUIRED",
        "catalog.cluster_readable_views.authority_required");
    return result;
  }

  result.status = SchemaOkStatus();
  result.views_present = true;
  result.failed_closed = false;
  result.views = BuiltinClusterCatalogReadableViewDefinitions();
  if (!request.include_comments) {
    for (auto& view : result.views) {
      view.columns.erase(
          std::remove_if(view.columns.begin(),
                         view.columns.end(),
                         [](const ClusterCatalogReadableViewColumn& column) {
                           return column.comment_join;
                         }),
          view.columns.end());
      view.comment_backed = false;
    }
  }
  return result;
}

std::string ClusterCacheProjectionFullTablePath(
    const ClusterCacheProjectionManifest& projection) {
  if (projection.schema_path.empty()) {
    return projection.table_name;
  }
  if (projection.table_name.empty()) {
    return projection.schema_path;
  }
  return projection.schema_path + "." + projection.table_name;
}

const ClusterCatalogColumnManifest* FindClusterCacheProjectionColumn(
    const ClusterCacheProjectionManifest& projection,
    const std::string& column_name) {
  for (const ClusterCatalogColumnManifest& column : projection.columns) {
    if (column.column_name == column_name) {
      return &column;
    }
  }
  return nullptr;
}

ClusterCacheProjectionValidationResult ValidateClusterCacheProjectionManifest(
    const ClusterCacheProjectionManifest& projection) {
  if (projection.schema_path != "sys.catalog.cluster_cache" ||
      projection.table_name.empty() || projection.stable_projection_id.empty() ||
      projection.source_cluster_table_path.empty() ||
      projection.columns.empty() || projection.primary_key_columns.empty()) {
    return ProjectionError("SB-CLUSTER-CACHE-PROJECTION-INCOMPLETE",
                           "catalog.cluster_cache_projection.incomplete",
                           ClusterCacheProjectionFullTablePath(projection));
  }
  if (!projection.projection_only || !projection.source_authority_required ||
      projection.cluster_authority) {
    return ProjectionError("SB-CLUSTER-CACHE-PROJECTION-AUTHORITY-REFUSED",
                           "catalog.cluster_cache_projection.authority_refused",
                           ClusterCacheProjectionFullTablePath(projection));
  }
  if (projection.local_runtime_execution_enabled) {
    return ProjectionError("SB-CLUSTER-CACHE-PROJECTION-LOCAL-EXECUTION-REFUSED",
                           "catalog.cluster_cache_projection.local_execution_refused",
                           ClusterCacheProjectionFullTablePath(projection));
  }
  if (!HasRequiredColumn(projection, "status")) {
    return ProjectionError("SB-CLUSTER-CACHE-PROJECTION-STATUS-REQUIRED",
                           "catalog.cluster_cache_projection.status_required",
                           ClusterCacheProjectionFullTablePath(projection));
  }
  for (const std::string& column_name : projection.primary_key_columns) {
    if (!HasRequiredColumn(projection, column_name)) {
      return ProjectionError("SB-CLUSTER-CACHE-PROJECTION-PRIMARY-KEY-INVALID",
                             "catalog.cluster_cache_projection.primary_key_invalid",
                             column_name);
    }
  }
  for (const std::string& column_name : projection.required_columns) {
    if (!HasRequiredColumn(projection, column_name)) {
      return ProjectionError("SB-CLUSTER-CACHE-PROJECTION-COLUMN-REQUIRED",
                             "catalog.cluster_cache_projection.column_required",
                             column_name);
    }
  }

  std::set<std::string> names;
  for (const ClusterCatalogColumnManifest& column : projection.columns) {
    if (column.column_name.empty() || column.type_name.empty()) {
      return ProjectionError("SB-CLUSTER-CACHE-PROJECTION-COLUMN-INCOMPLETE",
                             "catalog.cluster_cache_projection.column_incomplete",
                             ClusterCacheProjectionFullTablePath(projection));
    }
    if (!names.insert(column.column_name).second) {
      return ProjectionError("SB-CLUSTER-CACHE-PROJECTION-DUPLICATE-COLUMN",
                             "catalog.cluster_cache_projection.duplicate_column",
                             column.column_name);
    }
    if (IsForbiddenUserLayerColumn(column.column_name)) {
      return ProjectionError("SB-CLUSTER-CACHE-PROJECTION-NAME-COLUMN-REFUSED",
                             "catalog.cluster_cache_projection.name_column_refused",
                             column.column_name);
    }
    if (IsUntypedPropertyBagColumn(column)) {
      return ProjectionError("SB-CLUSTER-CACHE-PROJECTION-PROPERTY-BAG-REFUSED",
                             "catalog.cluster_cache_projection.property_bag_refused",
                             column.column_name);
    }
  }

  if (!HasRequiredColumn(projection, "source_authority_epoch") ||
      !HasRequiredColumn(projection, "source_generation") ||
      !HasRequiredColumn(projection, "source_digest") ||
      !HasRequiredColumn(projection, "invalidation_epoch") ||
      !HasRequiredColumn(projection, "freshness_epoch_millis")) {
    return ProjectionError("SB-CLUSTER-CACHE-PROJECTION-SOURCE-EVIDENCE-REQUIRED",
                           "catalog.cluster_cache_projection.source_evidence_required",
                           ClusterCacheProjectionFullTablePath(projection));
  }

  ClusterCacheProjectionValidationResult result;
  result.status = SchemaOkStatus();
  result.projection = projection;
  return result;
}

ClusterCacheProjectionSetValidationResult
ValidateClusterCacheProjectionManifestSet(
    const ClusterCacheProjectionManifestSet& projections) {
  if (!projections.projection_only || !projections.source_authority_required ||
      projections.local_runtime_execution_enabled) {
    return ProjectionSetError("SB-CLUSTER-CACHE-PROJECTION-SET-AUTHORITY-REFUSED",
                              "catalog.cluster_cache_projection.set_authority_refused");
  }

  std::set<std::string> projection_paths;
  std::set<std::string> source_paths;
  for (const ClusterCacheProjectionManifest& projection :
       projections.projections) {
    const auto validated = ValidateClusterCacheProjectionManifest(projection);
    if (!validated.ok()) {
      ClusterCacheProjectionSetValidationResult result;
      result.status = validated.status;
      result.diagnostic = validated.diagnostic;
      return result;
    }
    const std::string path = ClusterCacheProjectionFullTablePath(projection);
    if (!projection_paths.insert(path).second) {
      return ProjectionSetError("SB-CLUSTER-CACHE-PROJECTION-DUPLICATE",
                                "catalog.cluster_cache_projection.duplicate",
                                path);
    }
    source_paths.insert(projection.source_cluster_table_path);
  }

  for (const std::string& expected_source : ExpectedProjectionSourcePaths()) {
    if (source_paths.find(expected_source) == source_paths.end()) {
      return ProjectionSetError("SB-CLUSTER-CACHE-PROJECTION-SOURCE-REQUIRED",
                                "catalog.cluster_cache_projection.source_required",
                                expected_source);
    }
  }

  ClusterCacheProjectionSetValidationResult result;
  result.status = SchemaOkStatus();
  result.projections = projections;
  return result;
}

ClusterCatalogReadableViewValidationResult
ValidateClusterCatalogReadableViewDefinition(
    const ClusterCatalogReadableViewDefinition& view) {
  ClusterCatalogReadableViewValidationResult result;
  auto issue = [&](std::string diagnostic_code, std::string detail) {
    result.ok = false;
    result.issues.push_back({std::move(diagnostic_code), std::move(detail)});
  };

  if (view.schema_path != "cluster.sys.catalog_readable" ||
      view.view_name.empty() || view.stable_view_id.empty() ||
      view.source_cluster_table_path.empty() || view.columns.empty()) {
    issue("SB-CLUSTER-CATALOG-READABLE-VIEW-INCOMPLETE",
          ClusterCatalogReadableViewFullPath(view));
  }
  if (!KnownClusterCatalogSourcePath(view.source_cluster_table_path)) {
    issue("SB-CLUSTER-CATALOG-READABLE-VIEW-SOURCE-UNKNOWN",
          view.source_cluster_table_path);
  }
  if (!view.external_provider_bound ||
      !view.requires_joined_cluster_authority ||
      !view.internal_only_without_joined_authority) {
    issue("SB-CLUSTER-CATALOG-READABLE-VIEW-AUTHORITY-INVALID",
          ClusterCatalogReadableViewFullPath(view));
  }
  if (!view.resolver_backed || !view.comment_backed) {
    issue("SB-CLUSTER-CATALOG-READABLE-VIEW-RESOLVER-REQUIRED",
          ClusterCatalogReadableViewFullPath(view));
  }
  if (view.local_runtime_execution_enabled) {
    issue("SB-CLUSTER-CATALOG-READABLE-VIEW-LOCAL-EXECUTION-REFUSED",
          ClusterCatalogReadableViewFullPath(view));
  }

  bool has_uuid = false;
  bool has_resolver = false;
  bool has_comment = false;
  std::set<std::string> names;
  for (const auto& column : view.columns) {
    if (column.column_name.empty() || column.source_kind.empty() ||
        column.source_column.empty()) {
      issue("SB-CLUSTER-CATALOG-READABLE-VIEW-COLUMN-INCOMPLETE",
            ClusterCatalogReadableViewFullPath(view));
    }
    if (!names.insert(column.column_name).second) {
      issue("SB-CLUSTER-CATALOG-READABLE-VIEW-DUPLICATE-COLUMN",
            column.column_name);
    }
    if ((column.column_name.find("name") != std::string::npos ||
         column.column_name.find("lookup_key") != std::string::npos) &&
        !column.resolver_join) {
      issue("SB-CLUSTER-CATALOG-READABLE-VIEW-NAME-NOT-RESOLVER-BACKED",
            column.column_name);
    }
    if (column.column_name.find("comment") != std::string::npos &&
        !column.comment_join) {
      issue("SB-CLUSTER-CATALOG-READABLE-VIEW-COMMENT-NOT-JOIN-BACKED",
            column.column_name);
    }
    has_uuid = has_uuid || column.uuid_identity;
    has_resolver = has_resolver || column.resolver_join;
    has_comment = has_comment || column.comment_join;
  }
  if (!has_uuid || !has_resolver || !has_comment) {
    issue("SB-CLUSTER-CATALOG-READABLE-VIEW-COLUMN-COVERAGE-INCOMPLETE",
          ClusterCatalogReadableViewFullPath(view));
  }
  return result;
}

ClusterCatalogReadableViewValidationResult
ValidateBuiltinClusterCatalogReadableViewDefinitions() {
  ClusterCatalogReadableViewValidationResult result;
  std::set<std::string> view_paths;
  std::set<std::string> source_paths;
  for (const auto& view : BuiltinClusterCatalogReadableViewDefinitions()) {
    const auto validated = ValidateClusterCatalogReadableViewDefinition(view);
    if (!validated.ok) {
      result.ok = false;
      result.issues.insert(result.issues.end(),
                           validated.issues.begin(),
                           validated.issues.end());
    }
    if (!view_paths.insert(ClusterCatalogReadableViewFullPath(view)).second) {
      result.ok = false;
      result.issues.push_back(
          {"SB-CLUSTER-CATALOG-READABLE-VIEW-DUPLICATE",
           ClusterCatalogReadableViewFullPath(view)});
    }
    source_paths.insert(view.source_cluster_table_path);
  }
  if (source_paths != ExpectedProjectionSourcePaths()) {
    result.ok = false;
    result.issues.push_back(
        {"SB-CLUSTER-CATALOG-READABLE-VIEW-SOURCE-COVERAGE-INCOMPLETE",
         "source_path_set"});
  }
  return result;
}

DiagnosticRecord MakeClusterSchemaGatingDiagnostic(
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
                        "core.catalog.cluster_schema_gating");
}

}  // namespace scratchbird::core::catalog
