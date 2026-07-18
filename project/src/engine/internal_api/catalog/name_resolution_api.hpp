// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "api_types.hpp"
#include "catalog/relation_projection_view.hpp"

namespace scratchbird::engine::internal_api {

// SEARCH_KEY: SB_ENGINE_INTERNAL_API_CATALOG_NAME_RESOLUTION_API
// Neutral engine-owned resource metadata. Compatibility parsers consume this
// result but never own, synthesize, or persist these descriptors.
struct EngineResolvedResourceDescriptor {
  bool present = false;
  std::string resource_family;
  std::string canonical_name;
  EngineUuid resource_uuid;
  EngineUuid parent_resource_uuid;
  std::string parent_canonical_name;
  EngineUuid default_collation_uuid;
  std::string default_collation_name;
  EngineApiU64 resource_epoch = 0;
  EngineApiU64 family_epoch = 0;
  std::string family_version;
  std::uint32_t min_bytes = 0;
  std::uint32_t max_bytes = 0;
  bool variable_width = false;
  bool default_for_parent = false;
  bool case_insensitive = false;
  bool accent_insensitive = false;
};

// Engine-internal UUID lookup used when an already-bound descriptor is
// admitted by DDL.  The lookup revalidates the database-scoped resource UUID
// against the durable resource catalog and the exact active MGA transaction;
// it is not a parser-facing name-resolution surface.
struct EngineResourceDescriptorLookupResult {
  bool ok = false;
  EngineApiDiagnostic diagnostic;
  EngineResolvedResourceDescriptor resource_descriptor;
};

EngineResourceDescriptorLookupResult LookupEngineResourceDescriptorByUuid(
    const EngineRequestContext& context,
    const EngineUuid& resource_uuid,
    const std::string& expected_resource_family);

// Bounded engine-owned semantic projection attached only when name resolution
// resolves a persisted global-aggregate view.  It deliberately omits the
// source relation/column and expression literal retained by the engine-side
// view descriptor.
struct EngineResolvedSemanticProjection {
  bool present = false;
  std::string marker;
  EngineDescriptor projection_descriptor;
  EngineApiU64 descriptor_generation = 0;
  std::string result_alias;
  EngineDescriptor result_descriptor;
  std::vector<EngineRelationProjectionViewSemanticOutput> ordered_outputs;
};

struct EngineResolveNameRequest : EngineApiRequest {};
struct EngineResolveNameResult : EngineApiResult {
  EngineBoundObjectIdentity bound_object_identity;
  EngineResolvedResourceDescriptor resource_descriptor;
  EngineResolvedSemanticProjection semantic_projection;
};
EngineResolveNameResult EngineResolveName(const EngineResolveNameRequest& request);

struct EngineMapUuidToNameRequest : EngineApiRequest {};
struct EngineMapUuidToNameResult : EngineApiResult {
  EngineBoundObjectIdentity bound_object_identity;
};
EngineMapUuidToNameResult EngineMapUuidToName(const EngineMapUuidToNameRequest& request);

}  // namespace scratchbird::engine::internal_api
