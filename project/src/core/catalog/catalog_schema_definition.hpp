// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "catalog_record_codec.hpp"
#include "catalog_value_codec.hpp"
#include <optional>
#include <string_view>

namespace scratchbird::core::catalog {
enum class CatalogSchemaType : u64 {
  system = 1, user_home, remote_native, remote_emulated, public_compat,
  application, cluster
};

// Mutable family definition, distinct from physical bootstrap seed caches.
// Common metadata carries owner, names, current creator, visibility and epochs.
struct CatalogSchemaDefinition {
  TypedUuid schema_object_uuid;
  TypedUuid database_catalog_object_uuid;
  TypedUuid parent_schema_uuid;
  CatalogSchemaType schema_type = CatalogSchemaType::application;
  TypedUuid default_filespace_uuid;
  TypedUuid default_charset_uuid;
  TypedUuid default_collation_uuid;
  TypedUuid permissions_policy_uuid;
  TypedUuid origin_transaction_uuid;
  u64 origin_local_transaction_id = 0;
};
struct CatalogSchemaDefinitionResult {
  CatalogValueError error = CatalogValueError::invalid_value;
  std::optional<CatalogSchemaDefinition> definition;
  bool ok() const { return error == CatalogValueError::none && definition.has_value(); }
};
const char* CatalogSchemaTypeName(CatalogSchemaType type);
const CatalogValueSchema& CatalogSchemaDefinitionSchema();
CatalogValueEncodeResult EncodeCatalogSchemaDefinition(const CatalogSchemaDefinition&);
CatalogSchemaDefinitionResult DecodeCatalogSchemaDefinition(std::string_view);
// Structural bindings only; these functions perform no authorization, graph
// lookup, name/default resolution, cluster routing or storage publication.
bool CatalogSchemaDefinitionMatchesMetadata(const CatalogMetadataVersion&);
bool CatalogSchemaDefinitionPreservesOrigin(
    const CatalogMetadataVersion& previous, const CatalogMetadataVersion& successor);
}  // namespace scratchbird::core::catalog
