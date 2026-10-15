// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "catalog_name_envelope.hpp"
#include <span>

namespace scratchbird::core::catalog {
// Complete active first-version definitions. Name resident locators are supplied by
// the actual storage allocator, never guessed by this logical assembly.
struct CatalogCreationRecord {
  CatalogMetadataVersion metadata;
  std::optional<CatalogNamePayload> name;
};
struct CatalogCreationNameEntryInput {
  const CatalogMetadataVersion* metadata=nullptr;
  const CatalogNameEntry* entry=nullptr;
};
struct CatalogNamedCreationInput {
  const CatalogMetadataVersion* definition=nullptr;
  const CatalogMetadataVersion* vector_metadata=nullptr;
  const CatalogNameVector* vector=nullptr;
  std::span<const CatalogCreationNameEntryInput> entries;
};
struct CatalogNamedCreationLimits {
  u64 maximum_records=0;
  u64 maximum_encoded_bytes=0;
};
enum class CatalogNamedCreationError {
  none,invalid_request,invalid_metadata,invalid_names,duplicate_identity,
  resource_exhausted
};
struct CatalogNamedCreationSet {
  std::vector<CatalogCreationRecord> definitions,names;
  u64 encoded_bytes=0;
};
struct CatalogNamedCreationResult {
  CatalogNamedCreationError error=CatalogNamedCreationError::invalid_request;
  std::optional<CatalogNamedCreationSet> records;
  bool ok() const noexcept{return error==CatalogNamedCreationError::none&&records.has_value();}
};
struct CatalogNamedCreationPreflightResult {
  CatalogNamedCreationError error=CatalogNamedCreationError::invalid_request;
  u64 records=0,encoded_bytes=0;
};
// Complete borrowed per-definition/name admission. Definition payload may refer
// to bounded caller backing. No allocation/copies or identity-set admission;
// the owning cohort assembler must separately reject raw identity collisions.
CatalogNamedCreationPreflightResult PreflightLocalNamedCreationRecord(
    const CatalogMetadataVersionView&,const CatalogNamedCreationInput&,
    const TypedUuid& creation_transaction,std::string_view database_default_language,
    CatalogNamedCreationLimits) noexcept;
// All supplied definitions, vectors and aliases are returned together or not
// at all. Complete borrowed preflight precedes owning field/payload copies.
// Initial node construction only: creator local number1 and catalog/schema/
// security generation1. This does not replace ordinary object DDL/activation.
// The caller admits actual namespaces, profiles, collision/security policies,
// permission, selected source and input/output memory. These limits are NOT
// memory grants. No identity, alias, normalization key or authority is inferred.
// Inputs remain immutable under the owner's fence through the entire call.
CatalogNamedCreationResult AssembleLocalNamedCreationRecords(
    const Uuid& database_uuid,const TypedUuid& creation_transaction,
    std::string_view database_default_language,
    std::span<const CatalogNamedCreationInput>,CatalogNamedCreationLimits) noexcept;
} // namespace scratchbird::core::catalog
