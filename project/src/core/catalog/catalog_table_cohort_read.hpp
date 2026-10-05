// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "catalog_table_definition.hpp"

namespace scratchbird::core::catalog {
enum class CatalogTableCohortDecodeError {
  none, invalid_table, insufficient_output, invalid_columns, insufficient_scratch
};
struct CatalogTableCohortDecodeResult {
  CatalogTableCohortDecodeError error = CatalogTableCohortDecodeError::invalid_table;
  std::optional<CatalogTableDefinition> table;
  std::size_t columns_written = 0;
  bool ok() const { return error == CatalogTableCohortDecodeError::none && table.has_value(); }
};

// Decode one caller-selected complete cohort; not a lookup or visibility grant.
// Input views must remain alive and unchanged for this call; the output buffer
// must not alias them. Output owns fixed values and preserves input column order
// independently of stored ordinals. Unused output slots are never touched.
// Precedence: table family, output capacity, complete cohort, then output copy.
// Existing cohort validation uses O(N) temporary indexes; allocation failure
// propagates before any output write. The caller owns source/memory admission.
// Refusal returns no table/prefix and leaves ALL destination slots unchanged.
inline CatalogTableCohortDecodeResult DecodeCatalogTableColumnCohort(
    const CatalogMetadataVersionView& table,
    std::span<const CatalogMetadataVersionView> columns,
    std::span<CatalogColumnDefinition> output) {
  using E = CatalogTableCohortDecodeError;
  if (!CatalogTableDefinitionMatchesMetadata(table)) return {};
  if (output.size() < columns.size()) return {E::insufficient_output,{},0};
  if (!CatalogTableColumnCohortMatches(table,columns,false)) return {E::invalid_columns,{},0};
  // Validity of every payload and cross-record binding has been established.
  // Fixed-value decoding/copying cannot allocate or introduce a later refusal.
  const auto decoded_table = DecodeCatalogTableDefinition(table.record.payload);
  for (std::size_t i = 0; i < columns.size(); ++i)
    output[i] = *DecodeCatalogColumnDefinition(columns[i].record.payload).definition;
  return {E::none,decoded_table.definition,columns.size()};
}
// Fenced-reader form: caller supplies both admitted output and identity/ordinal
// scratch before acquiring any source guards. No heap allocation or governor
// callback occurs here. Scratch is disposable; semantic output remains atomic.
// Precedence adds scratch capacity between output capacity and cohort validity.
inline CatalogTableCohortDecodeResult DecodeCatalogTableColumnCohort(
    const CatalogMetadataVersionView& table,
    std::span<const CatalogMetadataVersionView> columns,
    std::span<CatalogColumnDefinition> output,
    std::span<CatalogColumnCohortScratch> scratch) {
  using E = CatalogTableCohortDecodeError;
  if (!CatalogTableDefinitionMatchesMetadata(table)) return {};
  if (output.size() < columns.size()) return {E::insufficient_output,{},0};
  if (scratch.size() < columns.size()) return {E::insufficient_scratch,{},0};
  if (!CatalogTableColumnCohortMatchesWithScratch(table,columns,false,scratch))
    return {E::invalid_columns,{},0};
  const auto decoded_table = DecodeCatalogTableDefinition(table.record.payload);
  for (std::size_t i = 0; i < columns.size(); ++i)
    output[i] = *DecodeCatalogColumnDefinition(columns[i].record.payload).definition;
  return {E::none,decoded_table.definition,columns.size()};
}
}  // namespace scratchbird::core::catalog
