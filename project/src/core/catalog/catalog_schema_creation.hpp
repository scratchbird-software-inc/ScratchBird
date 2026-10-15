// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "catalog_named_creation.hpp"
#include "catalog_schema_definition.hpp"

namespace scratchbird::core::catalog {
// Complete proposed first-version bindings. Metadata payload must be empty:
// the real typed schema definition is encoded here, never a legacy name cache.
// Names/profiles/defaults/owner/audit/policy are supplied by the owning producer,
// not generated from names or inferred from a missing reference.
struct CatalogInitialSchemaInput {
  const CatalogSchemaDefinition* definition=nullptr;
  const CatalogMetadataVersion* metadata=nullptr;
  const CatalogMetadataVersion* vector_metadata=nullptr;
  const CatalogNameVector* vector=nullptr;
  std::span<const CatalogCreationNameEntryInput> entries;
};
enum class CatalogInitialSchemaError {
  none,invalid_request,invalid_database,invalid_definition,invalid_metadata,
  invalid_names,duplicate_identity,missing_parent,cyclic_parent,resource_exhausted
};
struct CatalogInitialSchemaResult {
  CatalogInitialSchemaError error=CatalogInitialSchemaError::invalid_request;
  CatalogNamedCreationError name_error=CatalogNamedCreationError::none;
  std::optional<CatalogNamedCreationSet> records;
  bool ok() const noexcept{return error==CatalogInitialSchemaError::none&&records.has_value();}
};
// Assemble the entire supplied LOCAL initial schema forest and its aliases.
// All non-root parents must occur in this same package. No recursive call stack,
// quadratic parent scans, partial output, reference/permission invention or I/O.
// Caller admits immutable inputs/output/scratch memory and the actual external
// owner/audit/default/profile/policy definitions before entry. Limits are not
// memory grants; this package is not full node bootstrap/activation or serving.
CatalogInitialSchemaResult AssembleLocalInitialSchemaRecords(
    const Uuid& database_uuid,const CatalogMetadataVersion& database_definition,
    const TypedUuid& creation_transaction,std::string_view database_default_language,
    std::span<const CatalogInitialSchemaInput>,CatalogNamedCreationLimits) noexcept;
} // namespace scratchbird::core::catalog
