// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "api_types.hpp"
#include "crud_support/crud_store.hpp"
#include "mga_relation_store/mga_binary_identity_codec.hpp"

#include <cstdint>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace scratchbird::engine::internal_api {

// SEARCH_KEY: SB_ENGINE_MGA_RELATION_STORAGE_DESCRIPTOR
// Engine-internal relation storage descriptor. This is UUID/descriptor authority
// for local MGA DML and is not a parser-facing schema object.

struct MgaRelationColumnStorageDescriptor {
  EngineUuid column_uuid;
  // Exact catalog-object generation for this column.  This is distinct from
  // the enclosing storage descriptor generation and is carried into bound
  // DML occurrence records; a zero value is never executable authority.
  std::uint64_t column_generation = 0;
  std::uint32_t ordinal = 0;
  std::string canonical_name_key;
  EngineDescriptor value_descriptor;
  bool nullable = true;
  bool generated = false;
  bool identity_column = false;
  std::string storage_class = "inline_row_value";
  EngineUuid charset_uuid;
  EngineUuid collation_uuid;
  std::uint32_t character_length = 0;
  std::uint64_t max_inline_bytes = 4096;
  std::string overflow_policy = "mga_large_value_locator";
  bool operator==(const MgaRelationColumnStorageDescriptor&) const = default;
};

// Transfer an already-bound column into a staged storage projection. The
// generation is supplied by catalog publication, not a metadata event counter.
// This does not reserve identities, validate a live catalog or publish storage.
inline bool BindMgaColumnStorageIdentity(const EngineColumnDefinition& source,
    std::uint64_t published_generation, MgaRelationColumnStorageDescriptor* output) {
  if (!output || !published_generation || !core::uuid::IsEngineIdentityUuid(source.requested_column_uuid) ||
      !core::uuid::IsEngineIdentityUuid(source.descriptor.descriptor_uuid) ||
      !HasMgaColumnResourceIdentities(source.descriptor) ||
      !HasMgaColumnDatatypeBinding(source.descriptor)) return false;
  auto staged = *output;
  staged.column_uuid = source.requested_column_uuid;
  staged.column_generation = published_generation;
  staged.ordinal = source.ordinal;
  staged.value_descriptor = source.descriptor;
  staged.charset_uuid = source.descriptor.charset_uuid;
  staged.collation_uuid = source.descriptor.collation_uuid;
  staged.nullable = source.nullable;
  static_assert(std::is_nothrow_move_assignable_v<MgaRelationColumnStorageDescriptor>);
  *output = std::move(staged);
  return true;
}

struct MgaRelationIndexStorageDescriptor {
  EngineUuid index_uuid;
  std::string family;
  std::string profile;
  bool unique = false;
  bool approximate = false;
  std::vector<std::string> key_envelopes;
  std::vector<std::string> include_columns;
  std::string predicate_kind;
  std::string predicate_column;
  std::string predicate_value;
  std::string residency_policy = "page_cache_policy";
  bool operator==(const MgaRelationIndexStorageDescriptor&) const = default;
};

struct MgaRelationStorageDescriptor {
  EngineUuid descriptor_uuid;
  EngineUuid database_uuid;
  EngineUuid schema_uuid;
  EngineUuid relation_uuid;
  // Exact catalog-object generation for the relation.  Descriptor generation
  // versions this storage projection; it must not be substituted for the
  // relation object's own generation at a DML authority boundary.
  std::uint64_t relation_generation = 0;
  EngineUuid primary_filespace_uuid;
  std::string relation_kind = "table";
  std::string storage_profile = "local_mga_rowstore_v1";
  std::uint64_t descriptor_generation = 1;
  std::uint32_t page_size = 0;
  std::uint64_t root_page_number = 0;
  std::uint64_t allocation_root_page_number = 0;
  std::string row_identity_rule = "engine_uuid_v7_only";
  std::string version_identity_rule = "engine_uuid_v7_only";
  std::string mutation_rule = "copy_on_write";
  std::string visibility_rule = "local_inventory_snapshot_visibility";
  std::string cleanup_rule = "authoritative_local_horizon";
  std::string recovery_rule = "dirty_manifest_classification_no_wal";
  std::string descriptor_status = "production_descriptor";
  std::vector<MgaRelationColumnStorageDescriptor> columns;
  std::vector<MgaRelationIndexStorageDescriptor> indexes;
  std::vector<std::string> required_evidence_kinds;
  bool operator==(const MgaRelationStorageDescriptor&) const = default;
};

EngineApiDiagnostic ValidateMgaRelationStorageDescriptor(const MgaRelationStorageDescriptor& descriptor);
std::vector<std::pair<std::string, std::string>> SerializeMgaRelationStorageDescriptor(
    const MgaRelationStorageDescriptor& descriptor);
MgaRelationStorageDescriptor DeserializeMgaRelationStorageDescriptor(
    const std::vector<std::pair<std::string, std::string>>& fields);
MgaRelationStorageDescriptor BuildMgaRelationStorageDescriptorFromCrudMetadata(
    const EngineRequestContext& context,
    const CrudTableRecord& table,
    const std::vector<CrudIndexRecord>& indexes,
    const std::vector<std::pair<std::string, std::string>>& persisted_fields);
std::vector<std::pair<std::string, std::string>> BuildPersistedMgaRelationDescriptorFields(
    const EngineRequestContext& context,
    const CrudTableRecord& table,
    const std::vector<CrudIndexRecord>& indexes);

}  // namespace scratchbird::engine::internal_api
