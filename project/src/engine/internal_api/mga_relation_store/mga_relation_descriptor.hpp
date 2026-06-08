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

#include <cstdint>
#include <string>
#include <vector>

namespace scratchbird::engine::internal_api {

// SEARCH_KEY: SB_ENGINE_MGA_RELATION_STORAGE_DESCRIPTOR
// Engine-internal relation storage descriptor. This is UUID/descriptor authority
// for local MGA DML and is not a parser-facing schema object.

struct MgaRelationColumnStorageDescriptor {
  EngineUuid column_uuid;
  std::uint32_t ordinal = 0;
  std::string canonical_name_key;
  EngineDescriptor value_descriptor;
  bool nullable = true;
  bool generated = false;
  bool identity_column = false;
  std::string storage_class = "inline_row_value";
  std::string collation_uuid;
  std::uint64_t max_inline_bytes = 4096;
  std::string overflow_policy = "mga_large_value_locator";
};

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
};

struct MgaRelationStorageDescriptor {
  EngineUuid descriptor_uuid;
  EngineUuid database_uuid;
  EngineUuid schema_uuid;
  EngineUuid relation_uuid;
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
