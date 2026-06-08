// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// CLUSTER_SCHEMA_ROOT_GATING
// CLUSTER_CACHE_PROJECTIONS
// CLUSTER_CATALOG_READABLE_VIEWS
#include "cluster_catalog_manifest.hpp"
#include "runtime_platform.hpp"

#include <string>
#include <vector>

namespace scratchbird::core::catalog {

inline constexpr const char* kClusterSchemaAbsentDiagnosticCode =
    "SB_DIAG_CLUSTER_SCHEMA_ABSENT";

struct ClusterSchemaRootRequest {
  bool joined_cluster_catalog_state = false;
  bool cluster_authority_available = false;
  bool external_provider_available = false;
  bool local_runtime_execution_requested = false;
};

struct ClusterSchemaRootDecision {
  Status status;
  bool schema_present = false;
  bool failed_closed = false;
  bool external_provider_required = true;
  bool local_runtime_execution_enabled = false;
  std::vector<std::string> schema_paths;
  std::vector<std::string> evidence;
  DiagnosticRecord diagnostic;

  bool ok() const { return status.ok() && schema_present; }
};

struct ClusterCacheProjectionManifest {
  std::string schema_path = "sys.catalog.cluster_cache";
  std::string table_name;
  std::string stable_projection_id;
  std::string source_cluster_table_path;
  bool projection_only = true;
  bool source_authority_required = true;
  bool cluster_authority = false;
  bool local_runtime_execution_enabled = false;
  std::vector<ClusterCatalogColumnManifest> columns;
  std::vector<std::string> primary_key_columns;
  std::vector<std::string> required_columns;
};

struct ClusterCacheProjectionManifestSet {
  bool projection_only = true;
  bool source_authority_required = true;
  bool local_runtime_execution_enabled = false;
  std::vector<ClusterCacheProjectionManifest> projections;
};

struct ClusterCacheProjectionValidationResult {
  Status status;
  ClusterCacheProjectionManifest projection;
  DiagnosticRecord diagnostic;

  bool ok() const { return status.ok(); }
};

struct ClusterCacheProjectionSetValidationResult {
  Status status;
  ClusterCacheProjectionManifestSet projections;
  DiagnosticRecord diagnostic;

  bool ok() const { return status.ok(); }
};

struct ClusterCatalogReadableViewColumn {
  std::string column_name;
  std::string source_kind;
  std::string source_column;
  bool uuid_identity = false;
  bool resolver_join = false;
  bool comment_join = false;
  bool redaction_eligible = true;
};

struct ClusterCatalogReadableViewDefinition {
  std::string schema_path = "cluster.sys.catalog_readable";
  std::string view_name;
  std::string stable_view_id;
  std::string source_cluster_table_path;
  bool external_provider_bound = true;
  bool requires_joined_cluster_authority = true;
  bool internal_only_without_joined_authority = true;
  bool resolver_backed = true;
  bool comment_backed = true;
  bool local_runtime_execution_enabled = false;
  std::vector<ClusterCatalogReadableViewColumn> columns;
};

struct ClusterCatalogReadableViewAccessRequest {
  bool joined_cluster_catalog_state = false;
  bool cluster_authority_available = false;
  bool external_provider_available = false;
  bool include_comments = true;
};

struct ClusterCatalogReadableViewAccessResult {
  Status status;
  bool views_present = false;
  bool failed_closed = true;
  bool external_provider_required = true;
  bool joined_cluster_authority_required = true;
  bool local_runtime_execution_enabled = false;
  std::vector<ClusterCatalogReadableViewDefinition> views;
  DiagnosticRecord diagnostic;

  bool ok() const { return status.ok() && views_present; }
};

struct ClusterCatalogReadableViewValidationIssue {
  std::string diagnostic_code;
  std::string detail;
};

struct ClusterCatalogReadableViewValidationResult {
  bool ok = true;
  std::vector<ClusterCatalogReadableViewValidationIssue> issues;
};

ClusterSchemaRootDecision EvaluateClusterSchemaRootAccess(
    const ClusterSchemaRootRequest& request);
const std::vector<ClusterCacheProjectionManifest>&
BuiltinClusterCacheProjectionManifests();
ClusterCacheProjectionManifestSet BuiltinClusterCacheProjectionManifestSet();
std::string ClusterCacheProjectionFullTablePath(
    const ClusterCacheProjectionManifest& projection);
const ClusterCatalogColumnManifest* FindClusterCacheProjectionColumn(
    const ClusterCacheProjectionManifest& projection,
    const std::string& column_name);
ClusterCacheProjectionValidationResult ValidateClusterCacheProjectionManifest(
    const ClusterCacheProjectionManifest& projection);
ClusterCacheProjectionSetValidationResult
ValidateClusterCacheProjectionManifestSet(
    const ClusterCacheProjectionManifestSet& projections);
const std::vector<ClusterCatalogReadableViewDefinition>&
BuiltinClusterCatalogReadableViewDefinitions();
std::string ClusterCatalogReadableViewFullPath(
    const ClusterCatalogReadableViewDefinition& view);
ClusterCatalogReadableViewAccessResult EvaluateClusterCatalogReadableViewAccess(
    const ClusterCatalogReadableViewAccessRequest& request);
ClusterCatalogReadableViewValidationResult
ValidateClusterCatalogReadableViewDefinition(
    const ClusterCatalogReadableViewDefinition& view);
ClusterCatalogReadableViewValidationResult
ValidateBuiltinClusterCatalogReadableViewDefinitions();
DiagnosticRecord MakeClusterSchemaGatingDiagnostic(Status status,
                                                   std::string diagnostic_code,
                                                   std::string message_key,
                                                   std::string detail = {});

}  // namespace scratchbird::core::catalog
