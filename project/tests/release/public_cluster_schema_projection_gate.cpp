// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "cluster_schema_gating.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <set>
#include <string>
#include <string_view>
#include <utility>

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

bool Contains(const std::vector<std::string>& values, std::string_view value) {
  return std::find(values.begin(), values.end(), value) != values.end();
}

bool HasRequiredColumn(const catalog::ClusterCacheProjectionManifest& projection,
                       std::string_view column_name) {
  const auto* column = catalog::FindClusterCacheProjectionColumn(
      projection, std::string(column_name));
  return column != nullptr && column->required;
}

std::set<std::string> ExpectedSourcePaths() {
  std::set<std::string> paths;
  for (const auto& table : catalog::BuiltinClusterCatalogTableManifests()) {
    paths.insert(catalog::ClusterCatalogFullTablePath(table));
  }
  for (const auto& role_profile :
       catalog::BuiltinClusterRoleProfileManifests()) {
    paths.insert(catalog::ClusterCatalogFullTablePath(role_profile.table));
  }
  return paths;
}

void TestClusterSchemaRootGating() {
  catalog::ClusterSchemaRootRequest standalone;
  const auto standalone_decision =
      catalog::EvaluateClusterSchemaRootAccess(standalone);
  Require(!standalone_decision.ok(), "standalone cluster schema was present");
  Require(!standalone_decision.schema_present,
          "standalone cluster schema decision marked schema present");
  Require(standalone_decision.failed_closed,
          "standalone cluster schema decision did not fail closed");
  Require(standalone_decision.diagnostic.diagnostic_code ==
              catalog::kClusterSchemaAbsentDiagnosticCode,
          "standalone cluster schema diagnostic mismatch");
  Require(Contains(standalone_decision.evidence,
                   "cluster_paths_fail_closed=true"),
          "standalone cluster schema missing fail-closed evidence");

  catalog::ClusterSchemaRootRequest missing_provider;
  missing_provider.joined_cluster_catalog_state = true;
  missing_provider.cluster_authority_available = true;
  const auto missing_provider_decision =
      catalog::EvaluateClusterSchemaRootAccess(missing_provider);
  Require(!missing_provider_decision.ok() &&
              missing_provider_decision.diagnostic.diagnostic_code ==
                  catalog::kClusterSchemaAbsentDiagnosticCode,
          "cluster schema was present without external provider");

  catalog::ClusterSchemaRootRequest local_execution;
  local_execution.joined_cluster_catalog_state = true;
  local_execution.cluster_authority_available = true;
  local_execution.external_provider_available = true;
  local_execution.local_runtime_execution_requested = true;
  const auto local_execution_decision =
      catalog::EvaluateClusterSchemaRootAccess(local_execution);
  Require(!local_execution_decision.ok() &&
              local_execution_decision.diagnostic.diagnostic_code ==
                  "SB-CLUSTER-SCHEMA-LOCAL-EXECUTION-REFUSED",
          "cluster schema accepted local runtime execution");

  catalog::ClusterSchemaRootRequest joined;
  joined.joined_cluster_catalog_state = true;
  joined.cluster_authority_available = true;
  joined.external_provider_available = true;
  const auto joined_decision = catalog::EvaluateClusterSchemaRootAccess(joined);
  Require(joined_decision.ok(), "joined external-provider cluster schema absent");
  Require(joined_decision.schema_present,
          "joined cluster schema decision did not mark schema present");
  Require(!joined_decision.local_runtime_execution_enabled,
          "joined cluster schema enabled local runtime execution");
  Require(Contains(joined_decision.schema_paths, "cluster.sys.catalog"),
          "joined cluster schema omitted cluster.sys.catalog");
  Require(Contains(joined_decision.schema_paths, "cluster.sys.security"),
          "joined cluster schema omitted cluster.sys.security");
  Require(Contains(joined_decision.schema_paths, "cluster.sys.metrics"),
          "joined cluster schema omitted cluster.sys.metrics");
}

void TestProjectionManifestShapes() {
  const auto projections = catalog::BuiltinClusterCacheProjectionManifestSet();
  const auto validated =
      catalog::ValidateClusterCacheProjectionManifestSet(projections);
  Require(validated.ok(), "built-in cluster cache projections did not validate");
  Require(projections.projection_only,
          "cluster cache projection set was not projection-only");
  Require(projections.source_authority_required,
          "cluster cache projection set did not require source authority");
  Require(!projections.local_runtime_execution_enabled,
          "cluster cache projection set enabled local runtime execution");

  const auto expected_sources = ExpectedSourcePaths();
  Require(projections.projections.size() == expected_sources.size(),
          "cluster cache projection count mismatch");

  std::set<std::string> seen_sources;
  for (const auto& projection : projections.projections) {
    Require(projection.schema_path == "sys.catalog.cluster_cache",
            "cluster cache projection used wrong schema path");
    Require(projection.projection_only,
            "cluster cache projection was not projection-only");
    Require(projection.source_authority_required,
            "cluster cache projection did not require source authority");
    Require(!projection.cluster_authority,
            "cluster cache projection claimed cluster authority");
    Require(!projection.local_runtime_execution_enabled,
            "cluster cache projection enabled local runtime execution");
    Require(HasRequiredColumn(projection, "projection_uuid"),
            "cluster cache projection missing projection UUID");
    Require(HasRequiredColumn(projection, "source_cluster_uuid"),
            "cluster cache projection missing source cluster UUID");
    Require(HasRequiredColumn(projection, "source_record_uuid"),
            "cluster cache projection missing source record UUID");
    Require(HasRequiredColumn(projection, "source_authority_epoch"),
            "cluster cache projection missing source authority epoch");
    Require(HasRequiredColumn(projection, "source_generation"),
            "cluster cache projection missing source generation");
    Require(HasRequiredColumn(projection, "source_digest"),
            "cluster cache projection missing source digest");
    Require(HasRequiredColumn(projection, "invalidation_epoch"),
            "cluster cache projection missing invalidation epoch");
    Require(HasRequiredColumn(projection, "freshness_epoch_millis"),
            "cluster cache projection missing freshness field");
    Require(HasRequiredColumn(projection, "status"),
            "cluster cache projection missing status field");
    seen_sources.insert(projection.source_cluster_table_path);
  }

  for (const auto& expected_source : expected_sources) {
    Require(seen_sources.find(expected_source) != seen_sources.end(),
            "cluster cache projection missing source table");
  }
}

catalog::ClusterCatalogColumnManifest Column(std::string column_name,
                                             std::string type_name) {
  catalog::ClusterCatalogColumnManifest column;
  column.column_name = std::move(column_name);
  column.type_name = std::move(type_name);
  column.required = true;
  column.provider_supplied = false;
  return column;
}

void TestProjectionRefusals() {
  auto authority = catalog::BuiltinClusterCacheProjectionManifests().front();
  authority.cluster_authority = true;
  const auto authority_result =
      catalog::ValidateClusterCacheProjectionManifest(authority);
  Require(!authority_result.ok() &&
              authority_result.diagnostic.diagnostic_code ==
                  "SB-CLUSTER-CACHE-PROJECTION-AUTHORITY-REFUSED",
          "cluster cache projection accepted authority claim");

  auto local_execution =
      catalog::BuiltinClusterCacheProjectionManifests().front();
  local_execution.local_runtime_execution_enabled = true;
  const auto local_execution_result =
      catalog::ValidateClusterCacheProjectionManifest(local_execution);
  Require(!local_execution_result.ok() &&
              local_execution_result.diagnostic.diagnostic_code ==
                  "SB-CLUSTER-CACHE-PROJECTION-LOCAL-EXECUTION-REFUSED",
          "cluster cache projection accepted local execution");

  auto name_column = catalog::BuiltinClusterCacheProjectionManifests().front();
  name_column.columns.push_back(Column("source_node_name", "text"));
  const auto name_result =
      catalog::ValidateClusterCacheProjectionManifest(name_column);
  Require(!name_result.ok() &&
              name_result.diagnostic.diagnostic_code ==
                  "SB-CLUSTER-CACHE-PROJECTION-NAME-COLUMN-REFUSED",
          "cluster cache projection accepted user-layer name column");

  auto missing_source_digest =
      catalog::BuiltinClusterCacheProjectionManifests().front();
  auto& columns = missing_source_digest.columns;
  columns.erase(std::remove_if(columns.begin(),
                               columns.end(),
                               [](const auto& column) {
                                 return column.column_name == "source_digest";
                               }),
                columns.end());
  const auto missing_source_result =
      catalog::ValidateClusterCacheProjectionManifest(missing_source_digest);
  Require(!missing_source_result.ok() &&
              missing_source_result.diagnostic.diagnostic_code ==
                  "SB-CLUSTER-CACHE-PROJECTION-COLUMN-REQUIRED",
          "cluster cache projection accepted missing source digest");

  auto missing_projection = catalog::BuiltinClusterCacheProjectionManifestSet();
  missing_projection.projections.pop_back();
  const auto missing_projection_result =
      catalog::ValidateClusterCacheProjectionManifestSet(missing_projection);
  Require(!missing_projection_result.ok() &&
              missing_projection_result.diagnostic.diagnostic_code ==
                  "SB-CLUSTER-CACHE-PROJECTION-SOURCE-REQUIRED",
          "cluster cache projection set accepted missing source projection");
}

}  // namespace

int main() {
  TestClusterSchemaRootGating();
  TestProjectionManifestShapes();
  TestProjectionRefusals();
  return EXIT_SUCCESS;
}
