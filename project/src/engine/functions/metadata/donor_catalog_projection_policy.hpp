// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace scratchbird::engine::functions {

enum class DonorCatalogProjectionTarget {
  kAdminPluginStatus,
  kExtensionList,
  kExtensionItems,
};

struct DonorCatalogSeedManifestDescriptor {
  std::string_view engine_id;
  std::string_view manifest_id;
  std::string manifest_resource;
  std::string_view seed_manifest_schema;
  bool project_owned_packet = true;
  bool clean_database_seedable = true;
  bool mutable_by_parser = false;
  bool mutable_by_donor = false;
};

struct DonorCatalogProjectionContract {
  DonorCatalogProjectionTarget target;
  std::string_view sb_normalized_target;
  std::string_view route_contract_id;
  std::string_view seed_rowset_id;
  std::string_view sb_system_catalog_source;
  std::string_view sb_projection_view;
  std::string_view visibility_rule_id;
  std::string_view redaction_rule_id;
  std::string_view clean_database_recipe_id;
  std::string_view rowset_generation_rule;
  bool engine_owned = true;
  bool generated_from_system_catalog = true;
  bool requires_materialized_authorization = true;
  bool parser_execution_authority = false;
  bool donor_execution_authority = false;
  bool sblr_execution_authority = false;
};

struct DonorCatalogProjectionRequest {
  std::string_view engine_id;
  std::string_view inventory_id;
  std::string_view item_name;
  std::string_view implementation_decision;
  std::string_view capability_family;
  std::string_view sb_normalized_target;
  std::string_view sb_catalog_projection;
  std::string_view catalog_exposure;
  bool metadata_visible = false;
};

struct DonorCatalogProjectionSeedRow {
  std::string row_key;
  std::string donor_engine_id;
  std::string inventory_id;
  std::string donor_item_name;
  std::string capability_family;
  std::string sb_normalized_target;
  std::string sb_system_catalog_source;
  std::string projection_view;
  std::string seed_rowset_id;
  std::string source_manifest_id;
  std::string catalog_projection_label;
  std::string catalog_exposure;
  std::string visibility_rule_id;
  std::string redaction_rule_id;
  std::string clean_database_state;
  bool visible = true;
  bool metadata_redacted = true;
  bool engine_owned = true;
  bool parser_mutable = false;
  bool donor_mutable = false;
};

struct DonorCatalogProjectionResult {
  bool recognized = false;
  bool accepted = false;
  bool denied = true;
  bool clean_database_projection = false;
  bool metadata_redacted = true;
  bool parser_execution_authority = false;
  bool donor_execution_authority = false;
  bool sblr_execution_authority = false;
  std::string diagnostic_code;
  std::string route_contract_id;
  std::optional<DonorCatalogSeedManifestDescriptor> seed_manifest;
  std::optional<DonorCatalogProjectionContract> contract;
  std::vector<DonorCatalogProjectionSeedRow> rows;
  std::vector<std::pair<std::string, std::string>> evidence;
};

std::optional<DonorCatalogSeedManifestDescriptor> ResolveDonorCatalogSeedManifest(
    std::string_view engine_id);
std::optional<DonorCatalogProjectionContract> ResolveDonorCatalogProjectionContract(
    std::string_view sb_normalized_target);
DonorCatalogProjectionResult EvaluateDonorCatalogProjection(
    const DonorCatalogProjectionRequest& request);

}  // namespace scratchbird::engine::functions
