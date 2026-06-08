// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "extensibility/extension_boundary_manifest.hpp"

#include <algorithm>
#include <array>

namespace scratchbird::engine::internal_api {
namespace {

constexpr std::array<EngineExtensionBoundaryManifestEntry, 7> kBoundaryManifest = {{
    {
        "parser_package.sbsql_v3",
        "parser_package",
        "sb_parser_package_v3",
        "server.parser_package_registry.v3",
        "core_supported",
        "core",
        "parser_package_to_sblr_envelope",
        "supported",
        "none",
        "not_cluster",
        "engine_sblr_internal_api_only",
        "public_package_contract_no_engine_execution_code_edit",
        false,
        true,
        true,
        false,
    },
    {
        "parser_package.private_cluster_acceleration",
        "parser_package",
        "sb_parser_package_v3",
        "private_cluster_acceleration_parser_package.v3",
        "fail_closed_without_cluster_authority",
        "non_core_cluster_private",
        "private_parser_package_to_cluster_provider_boundary",
        "refuse",
        "cluster_mapping_unavailable",
        "external_cluster_provider_required",
        "engine_sblr_internal_api_only",
        "private_parser_packages_must_not_change_core_execution",
        false,
        true,
        true,
        true,
    },
    {
        "udr_package.trusted_cpp_v1",
        "udr_package",
        "sb_udr_v1",
        "sb_udr_runtime_descriptor.v1",
        "core_supported",
        "core",
        "trusted_udr_runtime_descriptor_dispatch",
        "supported",
        "none",
        "not_cluster",
        "engine_sblr_internal_api_only",
        "trusted_udr_contract_no_engine_execution_code_edit",
        false,
        false,
        true,
        false,
    },
    {
        "cluster_provider.v1",
        "cluster_provider",
        "sb_cluster_provider_v1",
        "cluster_provider/cluster_provider.hpp",
        "optional_cluster_provider",
        "non_core_cluster",
        "cluster_provider_inspect_execute_boundary",
        "provider_info_supported_execution_refused",
        "SBLR.CLUSTER.SUPPORT_NOT_ENABLED",
        "external_or_stub_cluster_provider_required",
        "engine_sblr_internal_api_only",
        "cluster_provider_contract_no_core_execution_code_edit",
        false,
        false,
        true,
        true,
    },
    {
        "cluster_metrics.v1",
        "cluster_metrics",
        "sb_cluster_metrics_v1",
        "metric_registry_cluster_descriptors.v1",
        "optional_cluster_provider",
        "non_core_cluster",
        "metric_registry_cluster_only_descriptors",
        "hidden_or_refused",
        "cluster_metric_path_skipped",
        "external_cluster_metrics_provider_required",
        "engine_sblr_internal_api_only",
        "cluster_metrics_contract_no_core_execution_code_edit",
        false,
        false,
        true,
        true,
    },
    {
        "cluster_agents.v1",
        "cluster_agents",
        "sb_cluster_agents_v1",
        "agent_engine_lifecycle_cluster_fail_closed.v1",
        "optional_cluster_provider",
        "non_core_cluster",
        "database_engine_lifecycle_agent_cluster_boundary",
        "standalone_agent_selected_cluster_paths_failed_closed",
        "cluster_paths_failed_closed",
        "external_cluster_agent_provider_required",
        "engine_sblr_internal_api_only",
        "cluster_agents_contract_no_core_execution_code_edit",
        false,
        false,
        true,
        true,
    },
    {
        "cluster_manager.v1",
        "cluster_manager",
        "sb_cluster_manager_v1",
        "engine_internal_api_cluster_control_v1",
        "optional_cluster_provider",
        "non_core_cluster",
        "cluster_control_inspect_replication_apis",
        "refuse",
        "SB_ENGINE_API_CLUSTER_AUTHORITY_UNAVAILABLE",
        "external_cluster_manager_provider_required",
        "engine_sblr_internal_api_only",
        "cluster_manager_contract_no_core_execution_code_edit",
        false,
        false,
        true,
        true,
    },
}};

}  // namespace

std::span<const EngineExtensionBoundaryManifestEntry> BuiltinExtensionBoundaryManifest() {
  return kBoundaryManifest;
}

const EngineExtensionBoundaryManifestEntry* FindExtensionBoundaryManifestEntry(
    std::string_view boundary_id) {
  const auto manifest = BuiltinExtensionBoundaryManifest();
  const auto it = std::find_if(
      manifest.begin(),
      manifest.end(),
      [boundary_id](const EngineExtensionBoundaryManifestEntry& entry) {
        return entry.boundary_id == boundary_id;
      });
  return it == manifest.end() ? nullptr : &*it;
}

bool ExtensionBoundaryManifestHasRequiredCoreRows() {
  constexpr std::array<std::string_view, 7> kRequiredIds = {{
      "parser_package.sbsql_v3",
      "parser_package.private_cluster_acceleration",
      "udr_package.trusted_cpp_v1",
      "cluster_provider.v1",
      "cluster_metrics.v1",
      "cluster_agents.v1",
      "cluster_manager.v1",
  }};
  for (const auto id : kRequiredIds) {
    if (FindExtensionBoundaryManifestEntry(id) == nullptr) { return false; }
  }
  return true;
}

}  // namespace scratchbird::engine::internal_api
