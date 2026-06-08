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
// PUBLIC_CLUSTER_ACCESS_PROFILE_GATE

#include "cluster_catalog_access_profile.hpp"
#include "cluster_catalog_index_profiles.hpp"
#include "cluster_catalog_manifest.hpp"
#include "cluster_schema_gating.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace catalog = scratchbird::core::catalog;
namespace index = scratchbird::core::index;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

bool HasAccessIssue(const catalog::ClusterCatalogAccessValidationResult& result,
                    std::string_view code) {
  return std::any_of(result.issues.begin(),
                     result.issues.end(),
                     [code](const auto& issue) {
                       return std::string_view(issue.diagnostic_code) == code;
                     });
}

bool HasIndexIssue(const index::ClusterCatalogIndexValidationResult& result,
                   std::string_view code) {
  return std::any_of(result.issues.begin(),
                     result.issues.end(),
                     [code](const auto& issue) {
                       return std::string_view(issue.diagnostic_code) == code;
                     });
}

const catalog::ClusterCatalogAccessProfile* FindAccessProfile(
    std::string_view table_path,
    catalog::ClusterCatalogAccessPurpose purpose) {
  for (const auto& profile : catalog::BuiltinClusterCatalogAccessProfiles()) {
    if (std::string_view(profile.table_path) == table_path &&
        profile.purpose == purpose) {
      return &profile;
    }
  }
  return nullptr;
}

const index::ClusterCatalogPhysicalIndexProfile* FindIndexForAccess(
    std::string_view access_profile_id) {
  for (const auto& profile : index::BuiltinClusterCatalogIndexProfiles()) {
    if (std::string_view(profile.access_profile_id) == access_profile_id) {
      return &profile;
    }
  }
  return nullptr;
}

std::vector<std::string> KeyNames(
    const catalog::ClusterCatalogAccessProfile& profile) {
  std::vector<std::string> keys;
  for (const auto& key : profile.key_columns) {
    keys.push_back(key.column_name);
  }
  return keys;
}

void RequireKeyOrder(const catalog::ClusterCatalogAccessProfile& profile,
                     const std::vector<std::string>& expected) {
  Require(KeyNames(profile) == expected, "cluster access profile key drift");
}

bool ContainsForbiddenUserLayerKey(std::string_view column_name) {
  return column_name.find("name") != std::string::npos ||
         column_name.find("comment") != std::string::npos ||
         column_name.find("description") != std::string::npos ||
         column_name.find("property") != std::string::npos;
}

void TestBuiltinAccessProfilesValidate() {
  const auto result = catalog::ValidateBuiltinClusterCatalogAccessProfiles();
  Require(result.ok, "built-in cluster catalog access profiles did not validate");

  std::set<catalog::ClusterCatalogAccessPurpose> purposes;
  std::set<std::string> profile_ids;
  for (const auto& profile : catalog::BuiltinClusterCatalogAccessProfiles()) {
    Require(profile_ids.insert(profile.profile_id).second,
            "duplicate cluster catalog access profile id");
    purposes.insert(profile.purpose);
    Require(profile.external_provider_bound,
            "cluster access profile was not external-provider-bound");
    Require(profile.fail_closed_without_external_provider,
            "cluster access profile did not fail closed without provider");
    Require(!profile.local_runtime_execution_enabled,
            "cluster access profile enabled local execution");
    Require(!profile.mutable_by_local_core,
            "cluster access profile allowed local mutation");
    Require(!profile.cluster_authority,
            "cluster access profile claimed cluster authority");
    Require(profile.authority_boundary.find("external_cluster_provider") !=
                std::string::npos,
            "cluster access profile lacked external provider boundary");
    for (const auto& key : profile.key_columns) {
      Require(!ContainsForbiddenUserLayerKey(key.column_name),
              "cluster access profile used user-layer text key");
    }
  }

  for (const auto purpose : catalog::RequiredClusterCatalogAccessPurposes()) {
    Require(purposes.count(purpose) == 1,
            "required cluster catalog access purpose missing");
  }
}

void TestUuidExactCoverage() {
  for (const auto& table : catalog::BuiltinClusterCatalogTableManifests()) {
    const std::string path = catalog::ClusterCatalogFullTablePath(table);
    const auto* profile = FindAccessProfile(
        path, catalog::ClusterCatalogAccessPurpose::uuid_exact_lookup);
    Require(profile != nullptr, "cluster table missing UUID exact profile");
    Require(profile->method == catalog::ClusterCatalogAccessMethod::hash_equality,
            "UUID exact profile was not hash equality");
    Require(profile->unique, "UUID exact profile was not unique");
    Require(profile->key_columns.size() == 1,
            "UUID exact profile had wrong key count");
    Require(profile->key_columns.front().column_name ==
                table.primary_key_columns.front(),
            "UUID exact profile did not use primary UUID");
  }

  for (const auto& role_profile :
       catalog::BuiltinClusterRoleProfileManifests()) {
    const std::string path =
        catalog::ClusterCatalogFullTablePath(role_profile.table);
    const auto* profile = FindAccessProfile(
        path, catalog::ClusterCatalogAccessPurpose::uuid_exact_lookup);
    Require(profile != nullptr,
            "cluster role-profile table missing UUID exact profile");
    Require(profile->key_columns.size() == 1,
            "role-profile UUID exact profile had wrong key count");
    Require(profile->key_columns.front().column_name ==
                role_profile.table.primary_key_columns.front(),
            "role-profile UUID exact profile did not use primary UUID");
  }
}

void TestRouteTopologyAndDecisionProfiles() {
  const auto* node_role = catalog::FindClusterCatalogAccessProfile(
      "cluster_catalog.node.node_role_lookup");
  Require(node_role != nullptr, "node-role access profile missing");
  Require(node_role->supports_node_role_lookup,
          "node-role profile support flag missing");
  RequireKeyOrder(*node_role,
                  {"cluster_uuid",
                   "node_role_uuid",
                   "catalog_generation",
                   "node_uuid"});

  const auto* route = catalog::FindClusterCatalogAccessProfile(
      "cluster_catalog.route.route_lookup");
  Require(route != nullptr, "route access profile missing");
  Require(route->supports_route_lookup, "route profile support flag missing");
  RequireKeyOrder(*route,
                  {"cluster_uuid",
                   "source_node_uuid",
                   "target_node_uuid",
                   "route_generation",
                   "route_uuid"});

  const auto* decision = catalog::FindClusterCatalogAccessProfile(
      "cluster_catalog.route_decision.decision_proof_lookup");
  Require(decision != nullptr, "decision-proof access profile missing");
  Require(decision->supports_decision_proof_lookup,
          "decision-proof profile support flag missing");
  RequireKeyOrder(*decision,
                  {"decision_proof_uuid",
                   "decision_epoch",
                   "route_decision_uuid"});

  const auto* shard = catalog::FindClusterCatalogAccessProfile(
      "cluster_catalog.shard_topology.shard_placement_lookup");
  Require(shard != nullptr, "shard-placement access profile missing");
  Require(shard->supports_shard_placement_lookup,
          "shard-placement profile support flag missing");
  RequireKeyOrder(*shard,
                  {"page_family_uuid",
                   "placement_profile_uuid",
                   "topology_generation",
                   "shard_uuid"});
}

void TestEpochGenerationAndProjectionCoverage() {
  std::set<std::string> epoch_tables;
  for (const auto& profile : catalog::BuiltinClusterCatalogAccessProfiles()) {
    if (profile.purpose ==
        catalog::ClusterCatalogAccessPurpose::epoch_generation_scan) {
      epoch_tables.insert(profile.table_path);
      Require(profile.supports_epoch_generation_scan,
              "epoch/generation profile support flag missing");
      Require(profile.method == catalog::ClusterCatalogAccessMethod::btree_ordered,
              "epoch/generation profile was not ordered");
      Require(std::any_of(profile.key_columns.begin(),
                          profile.key_columns.end(),
                          [](const auto& key) {
                            return key.column_name.find("_epoch") !=
                                   std::string::npos;
                          }),
              "epoch/generation profile lacked epoch key");
      Require(std::any_of(profile.key_columns.begin(),
                          profile.key_columns.end(),
                          [](const auto& key) {
                            return key.column_name.find("generation") !=
                                   std::string::npos;
                          }),
              "epoch/generation profile lacked generation key");
    }
  }
  Require(!epoch_tables.empty(), "no epoch/generation access profiles found");

  for (const auto& projection :
       catalog::BuiltinClusterCacheProjectionManifests()) {
    const std::string path =
        catalog::ClusterCacheProjectionFullTablePath(projection);
    const auto* profile = FindAccessProfile(
        path,
        catalog::ClusterCatalogAccessPurpose::projection_invalidation_scan);
    Require(profile != nullptr,
            "cluster projection missing invalidation profile");
    Require(profile->projection_cache_profile,
            "projection invalidation profile was not projection-scoped");
    Require(profile->source_authority_required,
            "projection invalidation profile did not require source authority");
    Require(profile->supports_projection_invalidation,
            "projection invalidation profile support flag missing");
    RequireKeyOrder(*profile,
                    {"source_record_uuid",
                     "invalidation_epoch",
                     "source_generation",
                     "projection_uuid"});
  }
}

void TestIndexProfilesMirrorAccessProfiles() {
  const auto result = index::ValidateBuiltinClusterCatalogIndexProfiles();
  Require(result.ok, "built-in cluster catalog index profiles did not validate");
  Require(index::BuiltinClusterCatalogIndexProfiles().size() ==
              catalog::BuiltinClusterCatalogAccessProfiles().size(),
          "cluster index profile count did not match access profiles");

  std::set<std::string> index_names;
  for (const auto& access : catalog::BuiltinClusterCatalogAccessProfiles()) {
    const auto* profile = FindIndexForAccess(access.profile_id);
    Require(profile != nullptr,
            "cluster index profile missing for access profile");
    Require(index_names.insert(profile->index_name).second,
            "duplicate cluster index profile name");
    Require(profile->table_path == access.table_path,
            "cluster index profile table mismatch");
    Require(profile->key_columns.size() == access.key_columns.size(),
            "cluster index profile key count mismatch");
    Require(profile->unique == access.unique,
            "cluster index profile uniqueness mismatch");
    Require(profile->external_provider_bound,
            "cluster index profile was not external-provider-bound");
    Require(profile->fail_closed_without_external_provider,
            "cluster index profile did not fail closed without provider");
    Require(profile->evidence_only,
            "cluster index profile was not evidence-only");
    Require(!profile->local_runtime_execution_enabled,
            "cluster index profile enabled local execution");
    Require(!profile->mutable_by_local_core,
            "cluster index profile allowed local mutation");
    Require(!profile->cluster_authority,
            "cluster index profile claimed cluster authority");
  }
}

void TestFailClosedRefusals() {
  auto local_execution = catalog::BuiltinClusterCatalogAccessProfiles().front();
  local_execution.local_runtime_execution_enabled = true;
  const auto local_execution_result =
      catalog::ValidateClusterCatalogAccessProfile(local_execution);
  Require(!local_execution_result.ok,
          "local-execution cluster access profile was accepted");
  Require(HasAccessIssue(local_execution_result,
                         "SB-CLUSTER-CATALOG-ACCESS-LOCAL-EXECUTION-REFUSED"),
          "local-execution access refusal diagnostic changed");

  auto user_layer_key = local_execution;
  user_layer_key.local_runtime_execution_enabled = false;
  user_layer_key.key_columns.front().column_name = "display_name";
  const auto user_layer_key_result =
      catalog::ValidateClusterCatalogAccessProfile(user_layer_key);
  Require(!user_layer_key_result.ok,
          "user-layer cluster access key was accepted");
  Require(HasAccessIssue(user_layer_key_result,
                         "SB-CLUSTER-CATALOG-ACCESS-USER-LAYER-KEY"),
          "user-layer access key diagnostic changed");

  auto local_index = index::BuiltinClusterCatalogIndexProfiles().front();
  local_index.cluster_authority = true;
  const auto local_index_result =
      index::ValidateClusterCatalogIndexProfile(local_index);
  Require(!local_index_result.ok,
          "cluster-authoritative local index profile was accepted");
  Require(HasIndexIssue(local_index_result,
                        "SB-CLUSTER-CATALOG-INDEX-LOCAL-EXECUTION-REFUSED"),
          "local index authority refusal diagnostic changed");
}

}  // namespace

int main() {
  TestBuiltinAccessProfilesValidate();
  TestUuidExactCoverage();
  TestRouteTopologyAndDecisionProfiles();
  TestEpochGenerationAndProjectionCoverage();
  TestIndexProfilesMirrorAccessProfiles();
  TestFailClosedRefusals();
  return EXIT_SUCCESS;
}
