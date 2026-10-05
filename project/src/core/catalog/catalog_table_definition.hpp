// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "catalog_record_codec.hpp"
#include "catalog_value_codec.hpp"
#include <optional>

namespace scratchbird::core::catalog {
// Versioned object references, never a request to bind the current/default
// version. Optional absence is exactly unknown-kind/nil/zero in memory and
// omission of BOTH fields on disk. The owner resolves existence and visibility.
struct CatalogDefinitionReference {
  TypedUuid uuid;
  u64 generation = 0;
};
enum class CatalogTableTypeEnforcement : u64 { strict = 1, coercing = 2 };
enum class CatalogGeneratedColumnStorage : u64 { none = 0, virtual_value = 1, stored = 2 };
enum class CatalogIdentityColumnMode : u64 { none = 0, by_default = 1, always = 2 };

// Native family definitions. Common SBCMV001 metadata owns schema, names,
// security, visibility, current creator and generations. Physical roots belong
// to the exact storage descriptor reference; this record does not allocate them.
struct CatalogTableDefinition {
  TypedUuid table_uuid;
  TypedUuid database_uuid;
  TypedUuid origin_transaction_uuid;
  u64 origin_local_transaction_id = 0;
  u64 next_column_ordinal = 0;
  CatalogDefinitionReference storage_descriptor;
  CatalogTableTypeEnforcement type_enforcement = CatalogTableTypeEnforcement::strict;
  u64 type_enforcement_generation = 0;
  TypedUuid type_enforcement_transaction_uuid;
  CatalogDefinitionReference coercion_profile;
};
struct CatalogColumnDefinition {
  TypedUuid column_uuid;
  TypedUuid table_uuid;
  TypedUuid origin_transaction_uuid;
  u64 origin_local_transaction_id = 0;
  u64 ordinal = 0;  // exact SBCV u64 field, restricted to stored-u32 domain
  CatalogDefinitionReference value_descriptor;
  CatalogDefinitionReference datatype_descriptor;
  TypedUuid type_uuid;
  bool nullable = false;
  CatalogDefinitionReference domain;
  CatalogDefinitionReference default_expression;
  CatalogDefinitionReference generated_expression;
  CatalogGeneratedColumnStorage generated_storage = CatalogGeneratedColumnStorage::none;
  CatalogDefinitionReference identity_sequence;
  CatalogIdentityColumnMode identity_mode = CatalogIdentityColumnMode::none;
  CatalogDefinitionReference charset;
  CatalogDefinitionReference collation;
  CatalogDefinitionReference storage_profile;
};
template<class Definition> struct CatalogDefinitionResult {
  CatalogValueError error = CatalogValueError::invalid_value;
  std::optional<Definition> definition;
  bool ok() const { return error == CatalogValueError::none && definition.has_value(); }
};
CatalogValueSchemaView CatalogTableDefinitionSchemaView();
CatalogValueSchemaView CatalogColumnDefinitionSchemaView();
bool IsCatalogTableDefinitionPayload(std::string_view);
bool IsCatalogColumnDefinitionPayload(std::string_view);
CatalogValueEncodeResult EncodeCatalogTableDefinition(const CatalogTableDefinition&);
CatalogValueEncodeResult EncodeCatalogColumnDefinition(const CatalogColumnDefinition&);
// Decoding uses fixed stack field views, with no C++ heap allocations. Results
// contain native fixed values, not borrowed input or a partial accepted prefix.
CatalogDefinitionResult<CatalogTableDefinition> DecodeCatalogTableDefinition(std::string_view);
CatalogDefinitionResult<CatalogColumnDefinition> DecodeCatalogColumnDefinition(std::string_view);
bool CatalogTableDefinitionMatchesMetadata(const CatalogMetadataVersionView&);
bool CatalogColumnDefinitionMatchesMetadata(const CatalogMetadataVersionView&);
bool CatalogTableDefinitionPreservesOrigin(const CatalogMetadataVersionView&, const CatalogMetadataVersionView&);
bool CatalogColumnDefinitionPreservesOrigin(const CatalogMetadataVersionView&, const CatalogMetadataVersionView&);
// Explicit payload/header validation for owners staging native typed records.
// No dependency lookup, catalog-source admission, value evaluation or I/O.
bool CatalogTableDefinitionMatchesHeader(const CatalogTypedRecordView&);
bool CatalogColumnDefinitionMatchesHeader(const CatalogTypedRecordView&);
// Complete caller-selected cohort, not a lookup or a completeness receipt.
struct CatalogColumnCohortScratch {
  core::platform::Uuid column_uuid;
  u64 ordinal = 0;
};
struct CatalogColumnHistoryScratch {
  std::span<CatalogColumnCohortScratch> previous;
  std::span<CatalogColumnCohortScratch> successor;
  std::span<std::size_t> previous_order;
};
// Structural history only; no generation/currentness/publication admission.
// All three scratch buffers must be admitted before source guards, distinct
// from one another and disjoint from immutable inputs. Requires one previous
// scratch/order slot per old column and one successor slot per new column.
// Scratch is disposable; unused tails and all inputs remain unchanged. No heap.
bool CatalogTableColumnCohortPreservesHistoryWithScratch(
    const CatalogMetadataVersionView& previous_table,
    std::span<const CatalogMetadataVersionView> previous_columns,
    const CatalogMetadataVersionView& successor_table,
    std::span<const CatalogMetadataVersionView> successor_columns,
    CatalogColumnHistoryScratch scratch);
// All scratch must be admitted before an enclosing storage fence. No heap
// allocation; scratch contents/order are disposable on every return. Inputs
// must remain immutable, and scratch must not alias any input/output storage.
bool CatalogTableColumnCohortMatchesWithScratch(
    const CatalogMetadataVersionView& table,
    std::span<const CatalogMetadataVersionView> columns, bool fresh_creation,
    std::span<CatalogColumnCohortScratch> scratch);
// Uses O(N) temporary identity/ordinal indexes; allocation failure propagates
// without mutating any input. The caller owns source retention and admission.
bool CatalogTableColumnCohortMatches(
    const CatalogMetadataVersionView& table,
    std::span<const CatalogMetadataVersionView> columns, bool fresh_creation);
bool CatalogTableColumnCohortPreservesHistory(
    const CatalogMetadataVersionView& previous_table,
    std::span<const CatalogMetadataVersionView> previous_columns,
    const CatalogMetadataVersionView& successor_table,
    std::span<const CatalogMetadataVersionView> successor_columns);
}  // namespace scratchbird::core::catalog
