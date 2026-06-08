// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "metadata/donor_catalog_projection_policy.hpp"

#include <algorithm>
#include <array>
#include <string>
#include <string_view>
#include <utility>

namespace scratchbird::engine::functions {
namespace {

struct ManifestDef {
  std::string_view engine_id;
  std::string_view manifest_id;
};

constexpr std::string_view kImplementationSearchKey =
    "SB_SYSTEM_CATALOG_PROJECTION";
constexpr std::string_view kSeedManifestSchema =
    "scratchbird.donor.catalog_seed_manifest.v1";
constexpr std::string_view kRedacted = "[redacted]";

constexpr auto kManifestDefs = std::to_array<ManifestDef>({
    {"apache_ignite", "donor.catalog.seed_manifest.apache_ignite.v1"},
    {"cassandra", "donor.catalog.seed_manifest.cassandra.v1"},
    {"clickhouse", "donor.catalog.seed_manifest.clickhouse.v1"},
    {"cockroachdb", "donor.catalog.seed_manifest.cockroachdb.v1"},
    {"dolt", "donor.catalog.seed_manifest.dolt.v1"},
    {"firebird", "donor.catalog.seed_manifest.firebird.v1"},
    {"foundationdb", "donor.catalog.seed_manifest.foundationdb.v1"},
    {"immudb", "donor.catalog.seed_manifest.immudb.v1"},
    {"mariadb", "donor.catalog.seed_manifest.mariadb.v1"},
    {"milvus", "donor.catalog.seed_manifest.milvus.v1"},
    {"mysql", "donor.catalog.seed_manifest.mysql.v1"},
    {"neo4j", "donor.catalog.seed_manifest.neo4j.v1"},
    {"opensearch", "donor.catalog.seed_manifest.opensearch.v1"},
    {"redis", "donor.catalog.seed_manifest.redis.v1"},
    {"sqlite", "donor.catalog.seed_manifest.sqlite.v1"},
    {"tidb", "donor.catalog.seed_manifest.tidb.v1"},
    {"vitess", "donor.catalog.seed_manifest.vitess.v1"},
    {"xtdb", "donor.catalog.seed_manifest.xtdb.v1"},
    {"yugabytedb", "donor.catalog.seed_manifest.yugabytedb.v1"},
});

void AddEvidence(DonorCatalogProjectionResult* result,
                 std::string key,
                 std::string value) {
  result->evidence.emplace_back(std::move(key), std::move(value));
}

std::string ManifestResource(std::string_view engine_id) {
  std::string resource = "donor-emulation/";
  resource.append(engine_id);
  resource.append("/catalog_seed_manifest_full.json");
  return resource;
}

std::string CleanDatabaseRecipe(std::string_view engine_id) {
  std::string recipe;
  recipe.append(engine_id);
  recipe.append("_empty_database_beta2_default_recipe_v1");
  return recipe;
}

DonorCatalogProjectionSeedRow BuildSeedRow(
    const DonorCatalogProjectionRequest& request,
    const DonorCatalogSeedManifestDescriptor& manifest,
    const DonorCatalogProjectionContract& contract) {
  DonorCatalogProjectionSeedRow row;
  row.row_key = std::string(request.engine_id);
  row.row_key.push_back(':');
  row.row_key.append(request.inventory_id);
  row.donor_engine_id = std::string(request.engine_id);
  row.inventory_id = std::string(request.inventory_id);
  row.donor_item_name =
      request.metadata_visible ? std::string(request.item_name) : std::string(kRedacted);
  row.capability_family = std::string(request.capability_family);
  row.sb_normalized_target = std::string(request.sb_normalized_target);
  row.sb_system_catalog_source = std::string(contract.sb_system_catalog_source);
  row.projection_view = std::string(contract.sb_projection_view);
  row.seed_rowset_id = std::string(contract.seed_rowset_id);
  row.source_manifest_id = std::string(manifest.manifest_id);
  row.catalog_projection_label = request.metadata_visible
                                     ? std::string(request.sb_catalog_projection)
                                     : std::string(kRedacted);
  row.catalog_exposure = request.metadata_visible
                             ? std::string(request.catalog_exposure)
                             : std::string(kRedacted);
  row.visibility_rule_id = std::string(contract.visibility_rule_id);
  row.redaction_rule_id = std::string(contract.redaction_rule_id);
  row.clean_database_state = "present_from_donor_catalog_seed_manifest";
  row.visible = true;
  row.metadata_redacted = !request.metadata_visible;
  row.engine_owned = contract.engine_owned;
  row.parser_mutable = false;
  row.donor_mutable = false;
  return row;
}

DonorCatalogProjectionResult FailClosed(std::string diagnostic_code,
                                        std::string route_contract_id) {
  DonorCatalogProjectionResult result;
  result.recognized = false;
  result.accepted = false;
  result.denied = true;
  result.clean_database_projection = false;
  result.metadata_redacted = true;
  result.diagnostic_code = std::move(diagnostic_code);
  result.route_contract_id = std::move(route_contract_id);
  AddEvidence(&result, "implementation_search_key", std::string(kImplementationSearchKey));
  AddEvidence(&result, "parser_execution_authority", "false");
  AddEvidence(&result, "donor_execution_authority", "false");
  AddEvidence(&result, "sblr_execution_authority", "false");
  return result;
}

}  // namespace

std::optional<DonorCatalogSeedManifestDescriptor> ResolveDonorCatalogSeedManifest(
    std::string_view engine_id) {
  const auto it = std::find_if(kManifestDefs.begin(),
                               kManifestDefs.end(),
                               [&](const ManifestDef& manifest) {
                                 return manifest.engine_id == engine_id;
                               });
  if (it == kManifestDefs.end()) return std::nullopt;
  return DonorCatalogSeedManifestDescriptor{
      it->engine_id,
      it->manifest_id,
      ManifestResource(engine_id),
      kSeedManifestSchema,
      true,
      true,
      false,
      false,
  };
}

std::optional<DonorCatalogProjectionContract> ResolveDonorCatalogProjectionContract(
    std::string_view sb_normalized_target) {
  if (sb_normalized_target == "SB.ADMIN.PLUGIN_STATUS") {
    return DonorCatalogProjectionContract{
        DonorCatalogProjectionTarget::kAdminPluginStatus,
        "SB.ADMIN.PLUGIN_STATUS",
        "donor_catalog_projection.admin_plugin_status.seed_rowset.v1",
        "sys.catalog.donor_admin_plugin_status_seed.v1",
        "sys.catalog.donor_extension_status",
        "sys.catalog.compat.donor_admin_plugin_status",
        "metadata_visible_or_admin_observability_right",
        "redact_donor_catalog_detail_without_metadata_visibility",
        "clean_database_catalog_projection.plugin_status.v1",
        "derive_clean_database_rows_from_donor_catalog_seed_manifest",
        true,
        true,
        true,
        false,
        false,
        false,
    };
  }
  if (sb_normalized_target == "SB.EXTENSION.LIST") {
    return DonorCatalogProjectionContract{
        DonorCatalogProjectionTarget::kExtensionList,
        "SB.EXTENSION.LIST",
        "donor_catalog_projection.extension_list.seed_rowset.v1",
        "sys.catalog.donor_extension_list_seed.v1",
        "sys.catalog.donor_extension_registry",
        "sys.catalog.compat.donor_extension_list",
        "metadata_visible_or_extension_catalog_right",
        "redact_extension_state_without_metadata_visibility",
        "clean_database_catalog_projection.extension_list.v1",
        "derive_clean_database_rows_from_donor_catalog_seed_manifest",
        true,
        true,
        true,
        false,
        false,
        false,
    };
  }
  if (sb_normalized_target == "SB.EXTENSION.ITEMS") {
    return DonorCatalogProjectionContract{
        DonorCatalogProjectionTarget::kExtensionItems,
        "SB.EXTENSION.ITEMS",
        "donor_catalog_projection.extension_items.seed_rowset.v1",
        "sys.catalog.donor_extension_item_seed.v1",
        "sys.catalog.donor_extension_items",
        "sys.catalog.compat.donor_extension_items",
        "metadata_visible_or_extension_catalog_right",
        "redact_extension_item_signature_without_metadata_visibility",
        "clean_database_catalog_projection.extension_items.v1",
        "derive_clean_database_rows_from_donor_catalog_seed_manifest",
        true,
        true,
        true,
        false,
        false,
        false,
    };
  }
  return std::nullopt;
}

DonorCatalogProjectionResult EvaluateDonorCatalogProjection(
    const DonorCatalogProjectionRequest& request) {
  if (request.implementation_decision != "catalog_projection_only") {
    return FailClosed("SB.DONOR_CATALOG_PROJECTION.NOT_CATALOG_PROJECTION",
                      "donor_catalog_projection.not_catalog_projection.fail_closed.v1");
  }

  auto manifest = ResolveDonorCatalogSeedManifest(request.engine_id);
  if (!manifest.has_value()) {
    return FailClosed("SB.DONOR_CATALOG_PROJECTION.UNKNOWN_DONOR_MANIFEST",
                      "donor_catalog_projection.unknown_manifest.fail_closed.v1");
  }

  auto contract = ResolveDonorCatalogProjectionContract(request.sb_normalized_target);
  if (!contract.has_value()) {
    return FailClosed("SB.DONOR_CATALOG_PROJECTION.UNKNOWN_TARGET",
                      "donor_catalog_projection.unknown_target.fail_closed.v1");
  }

  DonorCatalogProjectionResult result;
  result.recognized = true;
  result.accepted = true;
  result.denied = false;
  result.clean_database_projection = true;
  result.metadata_redacted = !request.metadata_visible;
  result.parser_execution_authority = false;
  result.donor_execution_authority = false;
  result.sblr_execution_authority = false;
  result.diagnostic_code = "SB.DONOR_CATALOG_PROJECTION.READY";
  result.route_contract_id = std::string(contract->route_contract_id);
  result.seed_manifest = manifest;
  result.contract = contract;
  result.rows.push_back(BuildSeedRow(request, *manifest, *contract));

  AddEvidence(&result, "implementation_search_key", std::string(kImplementationSearchKey));
  AddEvidence(&result, "engine_id", std::string(request.engine_id));
  AddEvidence(&result, "inventory_id", std::string(request.inventory_id));
  AddEvidence(&result, "seed_manifest_id", std::string(manifest->manifest_id));
  AddEvidence(&result, "seed_manifest_resource", std::string(manifest->manifest_resource));
  AddEvidence(&result, "seed_manifest_schema", std::string(manifest->seed_manifest_schema));
  AddEvidence(&result, "sb_system_catalog_source", std::string(contract->sb_system_catalog_source));
  AddEvidence(&result, "projection_view", std::string(contract->sb_projection_view));
  AddEvidence(&result, "visibility_rule_id", std::string(contract->visibility_rule_id));
  AddEvidence(&result, "redaction_rule_id", std::string(contract->redaction_rule_id));
  AddEvidence(&result, "clean_database_recipe_id", CleanDatabaseRecipe(request.engine_id));
  AddEvidence(&result, "parser_execution_authority", "false");
  AddEvidence(&result, "donor_execution_authority", "false");
  AddEvidence(&result, "sblr_execution_authority", "false");
  return result;
}

}  // namespace scratchbird::engine::functions
