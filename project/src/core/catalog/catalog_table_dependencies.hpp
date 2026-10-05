// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "catalog_table_definition.hpp"
#include <array>

namespace scratchbird::core::catalog {
// Derived fixed-size view of the explicit UUID/generation pairs in one native
// family payload. This is NOT the complete catalog dependency set: common
// metadata, type identity and inherited storage need their owning resolution.
// field_id is the UUID field in schema65700 or65701, not a new durable role ID.
struct CatalogDefinitionDependency {
  u16 field_id = 0;
  CatalogDefinitionReference target;
};
struct CatalogDefinitionDependencies {
  CatalogValueError error = CatalogValueError::invalid_value;
  u32 schema_id = 0;
  TypedUuid source_uuid;
  u64 source_definition_version = 0;
  std::array<CatalogDefinitionDependency,9> entries{};
  std::size_t count = 0;
  bool ok() const { return error == CatalogValueError::none; }
  std::span<const CatalogDefinitionDependency> references() const {
    return std::span<const CatalogDefinitionDependency>(entries).first(count);
  }
};
namespace table_dependency_detail {
inline void Append(CatalogDefinitionDependencies& out, u16 field,
                   const CatalogDefinitionReference& reference) {
  // Called only with a fully validated family: zero means exactly absent.
  if (reference.generation) out.entries[out.count++] = {field,reference};
}
inline bool Same(const CatalogDefinitionDependency& a, const CatalogDefinitionDependency& b) {
  return a.field_id == b.field_id && a.target.uuid.kind == b.target.uuid.kind &&
      a.target.uuid.value == b.target.uuid.value && a.target.generation == b.target.generation;
}
}  // namespace table_dependency_detail

// Each result owns its binary fixed values. No allocation, name resolution,
// default generation, identity issuance, source retention or storage effects.
inline CatalogDefinitionDependencies CatalogTableDefinitionDependencies(const CatalogMetadataVersionView& m) {
  if (!CatalogTableDefinitionMatchesMetadata(m)) return {};
  const auto decoded = DecodeCatalogTableDefinition(m.record.payload);
  const auto& d = *decoded.definition;
  CatalogDefinitionDependencies out;
  out.error = CatalogValueError::none; out.schema_id = 65700;
  out.source_uuid = d.table_uuid; out.source_definition_version = m.definition_version;
  table_dependency_detail::Append(out,6,d.storage_descriptor);
  table_dependency_detail::Append(out,11,d.coercion_profile);
  return out;
}
inline CatalogDefinitionDependencies CatalogColumnDefinitionDependencies(const CatalogMetadataVersionView& m) {
  if (!CatalogColumnDefinitionMatchesMetadata(m)) return {};
  const auto decoded = DecodeCatalogColumnDefinition(m.record.payload);
  const auto& d = *decoded.definition;
  CatalogDefinitionDependencies out;
  out.error = CatalogValueError::none; out.schema_id = 65701;
  out.source_uuid = d.column_uuid; out.source_definition_version = m.definition_version;
  table_dependency_detail::Append(out,6,d.value_descriptor);
  table_dependency_detail::Append(out,8,d.datatype_descriptor);
  table_dependency_detail::Append(out,12,d.domain);
  table_dependency_detail::Append(out,14,d.default_expression);
  table_dependency_detail::Append(out,16,d.generated_expression);
  table_dependency_detail::Append(out,19,d.identity_sequence);
  table_dependency_detail::Append(out,22,d.charset);
  table_dependency_detail::Append(out,24,d.collation);
  table_dependency_detail::Append(out,26,d.storage_profile);
  return out;
}

struct CatalogDefinitionDependencyComparison {
  CatalogValueError error = CatalogValueError::invalid_value;
  CatalogDefinitionDependencies predecessor;
  CatalogDefinitionDependencies successor;
  // Indices address the corresponding owned result above, in field order.
  std::array<std::size_t,9> removed_from_successor{};
  std::array<std::size_t,9> added_in_successor{};
  std::size_t removed_count = 0;
  std::size_t added_count = 0;
  bool ok() const { return error == CatalogValueError::none; }
};
// Structural difference of two caller-selected versions of the same object.
// It does not admit version progression, prove currentness/existence, or grant
// deletion of removed references: retained old versions still require them.
inline CatalogDefinitionDependencyComparison CompareCatalogDefinitionDependencies(
    const CatalogMetadataVersionView& before, const CatalogMetadataVersionView& after) {
  const bool table = before.object_subtype == "ordinary_persistent_table";
  const bool column = before.object_subtype == "persistent_table_column";
  if ((!table && !column) || (table ? !CatalogTableDefinitionPreservesOrigin(before,after)
                                   : !CatalogColumnDefinitionPreservesOrigin(before,after))) return {};
  CatalogDefinitionDependencyComparison out;
  out.predecessor = table ? CatalogTableDefinitionDependencies(before) : CatalogColumnDefinitionDependencies(before);
  out.successor = table ? CatalogTableDefinitionDependencies(after) : CatalogColumnDefinitionDependencies(after);
  if (!out.predecessor.ok() || !out.successor.ok()) return {};
  for (std::size_t i = 0; i < out.predecessor.count; ++i) {
    bool found = false;
    for (const auto& target : out.successor.references())
      found |= table_dependency_detail::Same(out.predecessor.entries[i],target);
    if (!found) out.removed_from_successor[out.removed_count++] = i;
  }
  for (std::size_t i = 0; i < out.successor.count; ++i) {
    bool found = false;
    for (const auto& target : out.predecessor.references())
      found |= table_dependency_detail::Same(out.successor.entries[i],target);
    if (!found) out.added_in_successor[out.added_count++] = i;
  }
  out.error = CatalogValueError::none;
  return out;
}
}  // namespace scratchbird::core::catalog
