// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "api_types.hpp"

#include <string>
#include <vector>

namespace scratchbird::engine::internal_api {

struct EngineSelectRowsRequest;
struct EngineSelectRowsResult;

struct EngineCatalogRelationProjectionViewDescriptor {
  bool present = false;
  std::string semantic_variant;
  std::string source_relation_name;
  std::string function_uuid;
  EngineApiDiagnostic diagnostic;
};

// SEARCH_KEY: SB_ENGINE_CATALOG_RELATION_DESCRIPTOR_PROJECTION_V1
// This is an engine-owned, dialect-neutral projection over an exact persisted
// MGA relation descriptor.  Dialect parsers may render these neutral rows, but
// neither parser SQL nor parser metadata is accepted as catalog authority.
inline constexpr const char* kRelationDescriptorProjectionMarkerV1 =
    "engine.catalog.relation_descriptor_projection.v1";
inline constexpr const char* kRelationDescriptorProjectionSourceKind =
    "catalog_relation_descriptor_projection";
inline constexpr const char* kRelationDescriptorProjectionTypeInventoryVariantV1 =
    "relation.type_inventory.v1";
inline constexpr const char* kRelationDescriptorProjectionCharsetInventoryVariantV1 =
    "relation.charset_inventory.v1";
inline constexpr const char* kRelationTypeNameFunctionDescriptorV1 =
    "engine.catalog.relation_type_name.v1";

bool IsRelationDescriptorProjectionViewCreateRequest(
    const EngineApiRequest& request);

EngineApiDiagnostic ValidateRelationDescriptorProjectionViewCreate(
    const EngineApiRequest& request);

// Returns the complete persisted option envelope for this bounded view class.
// The returned options contain semantic markers and UUIDs only; raw SQL and a
// source relation UUID are intentionally absent because the view may precede
// creation of its source relation.
std::vector<std::string> CanonicalRelationDescriptorProjectionViewOptions(
    const EngineApiRequest& request);

// Read-only catalog classification for generic engine name resolution.  This
// consumes only the persisted engine view record under the exact selected MGA
// metadata snapshot; it never reads parser state or accepts SQL text.
EngineCatalogRelationProjectionViewDescriptor
DescribeEngineCatalogRelationProjectionView(
    const EngineRequestContext& context,
    const std::string& view_uuid);

bool IsRelationDescriptorProjectionSelectRequest(
    const EngineSelectRowsRequest& request);

EngineSelectRowsResult EngineSelectRelationDescriptorProjection(
    const EngineSelectRowsRequest& request);

}  // namespace scratchbird::engine::internal_api
