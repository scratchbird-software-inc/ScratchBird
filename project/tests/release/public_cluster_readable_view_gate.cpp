// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SPDX-License-Identifier: Apache-2.0
// Copyright (c) ScratchBird contributors
//
// PUBLIC_CLUSTER_READABLE_VIEW_GATE

#include "cluster_catalog_manifest.hpp"
#include "cluster_schema_gating.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <set>
#include <string>
#include <string_view>

namespace {

namespace catalog = scratchbird::core::catalog;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) { Fail(message); }
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

bool HasColumn(const catalog::ClusterCatalogReadableViewDefinition& view,
               std::string_view column_name) {
  return std::any_of(
      view.columns.begin(),
      view.columns.end(),
      [column_name](const catalog::ClusterCatalogReadableViewColumn& column) {
        return column.column_name == column_name;
      });
}

const catalog::ClusterCatalogReadableViewColumn* Column(
    const catalog::ClusterCatalogReadableViewDefinition& view,
    std::string_view column_name) {
  for (const auto& column : view.columns) {
    if (column.column_name == column_name) { return &column; }
  }
  return nullptr;
}

void TestReadableViewDefinitions() {
  const auto validation =
      catalog::ValidateBuiltinClusterCatalogReadableViewDefinitions();
  Require(validation.ok, "built-in readable cluster catalog views failed validation");

  const auto& views = catalog::BuiltinClusterCatalogReadableViewDefinitions();
  const auto expected_sources = ExpectedSourcePaths();
  Require(views.size() == expected_sources.size(),
          "readable view count does not match cluster catalog sources");

  std::set<std::string> seen_sources;
  std::set<std::string> seen_paths;
  for (const auto& view : views) {
    Require(view.schema_path == "cluster.sys.catalog_readable",
            "readable cluster catalog view used wrong schema");
    Require(view.external_provider_bound,
            "readable view was not external-provider-bound");
    Require(view.requires_joined_cluster_authority,
            "readable view did not require joined cluster authority");
    Require(view.internal_only_without_joined_authority,
            "readable view was not internal-only without joined authority");
    Require(view.resolver_backed,
            "readable view did not require resolver rows");
    Require(view.comment_backed,
            "readable view did not require comment rows");
    Require(!view.local_runtime_execution_enabled,
            "readable view enabled local cluster execution");
    Require(HasColumn(view, "source_record_uuid"),
            "readable view missing source UUID");
    Require(HasColumn(view, "display_name"),
            "readable view missing display name");
    Require(HasColumn(view, "comment_text"),
            "readable view missing comment text");
    Require(HasColumn(view, "catalog_generation"),
            "readable view missing catalog generation");

    const auto* display_name = Column(view, "display_name");
    Require(display_name != nullptr && display_name->resolver_join,
            "display name was not resolver-backed");
    const auto* normalized = Column(view, "normalized_lookup_key");
    Require(normalized != nullptr && normalized->resolver_join,
            "normalized lookup key was not resolver-backed");
    const auto* comment = Column(view, "comment_text");
    Require(comment != nullptr && comment->comment_join,
            "comment text was not comment-join-backed");

    seen_sources.insert(view.source_cluster_table_path);
    seen_paths.insert(catalog::ClusterCatalogReadableViewFullPath(view));
  }
  Require(seen_sources == expected_sources,
          "readable views did not cover all cluster catalog sources");
  Require(seen_paths.size() == views.size(),
          "readable view paths were not unique");
}

void TestReadableViewAccessGating() {
  catalog::ClusterCatalogReadableViewAccessRequest absent;
  auto decision = catalog::EvaluateClusterCatalogReadableViewAccess(absent);
  Require(!decision.ok(), "readable views were visible without cluster authority");
  Require(decision.failed_closed, "readable views did not fail closed");
  Require(decision.views.empty(), "readable views leaked while absent");
  Require(decision.diagnostic.diagnostic_code ==
              "SB-CLUSTER-CATALOG-READABLE-VIEWS-AUTHORITY-REQUIRED",
          "readable view absent diagnostic changed");

  catalog::ClusterCatalogReadableViewAccessRequest joined;
  joined.joined_cluster_catalog_state = true;
  joined.cluster_authority_available = true;
  joined.external_provider_available = true;
  decision = catalog::EvaluateClusterCatalogReadableViewAccess(joined);
  Require(decision.ok(), "readable views were absent with joined authority");
  Require(!decision.failed_closed,
          "readable views stayed fail-closed with joined authority");
  Require(!decision.local_runtime_execution_enabled,
          "readable view access enabled local execution");
  Require(decision.views.size() == ExpectedSourcePaths().size(),
          "readable view access returned wrong view count");

  joined.include_comments = false;
  decision = catalog::EvaluateClusterCatalogReadableViewAccess(joined);
  Require(decision.ok(), "comment-redacted readable view access failed");
  for (const auto& view : decision.views) {
    Require(!view.comment_backed,
            "comment-redacted readable view still claimed comment backing");
    for (const auto& column : view.columns) {
      Require(!column.comment_join,
              "comment-redacted readable view exposed comment column");
    }
  }
}

}  // namespace

int main() {
  TestReadableViewDefinitions();
  TestReadableViewAccessGating();
  return EXIT_SUCCESS;
}
