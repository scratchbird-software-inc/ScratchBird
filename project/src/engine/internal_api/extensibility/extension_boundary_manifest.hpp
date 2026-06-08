// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <span>
#include <string_view>

namespace scratchbird::engine::internal_api {

// SEARCH_KEY: SB_ENGINE_INTERNAL_API_EXTENSION_BOUNDARY_MANIFEST
// Versioned manifest for extension contracts that must remain outside engine
// execution semantics. Extensions may translate or route, but execution
// authority stays with validated SBLR/internal APIs and UUID-resolved operands.
struct EngineExtensionBoundaryManifestEntry {
  std::string_view boundary_id;
  std::string_view boundary_type;
  std::string_view contract_version;
  std::string_view abi_surface;
  std::string_view support_level;
  std::string_view core_classification;
  std::string_view route_surface;
  std::string_view non_cluster_behavior;
  std::string_view non_cluster_refusal_code;
  std::string_view positive_cluster_boundary;
  std::string_view execution_authority;
  std::string_view engine_change_policy;
  bool sql_text_authoritative = false;
  bool parser_translation_boundary = false;
  bool requires_uuid_or_descriptor_authority = true;
  bool cluster_positive_requires_provider = false;
};

std::span<const EngineExtensionBoundaryManifestEntry> BuiltinExtensionBoundaryManifest();

const EngineExtensionBoundaryManifestEntry* FindExtensionBoundaryManifestEntry(
    std::string_view boundary_id);

bool ExtensionBoundaryManifestHasRequiredCoreRows();

}  // namespace scratchbird::engine::internal_api
