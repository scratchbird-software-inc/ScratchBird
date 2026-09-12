// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "catalog_value_codec.hpp"
#include "catalog_record_codec.hpp"
#include <optional>
#include <string_view>

namespace scratchbird::core::catalog {
// CATALOG_SCHEMA_BOOTSTRAP_BINARY_PAYLOAD_V1. Physical seed, not the full
// logical sys.catalog.schema descriptor or MGA/name authority.
struct CatalogSchemaRecord {
  TypedUuid schema_object_uuid;
  TypedUuid parent_object_uuid;
  std::string path_cache;
  std::string name_cache;
  bool root_schema = false;
  bool local_single_node_scope = true;
  bool recursive_schema_tree = true;
  u64 creator_transaction_number = 1;
};
struct CatalogSchemaRecordDecodeResult {
  CatalogValueError error = CatalogValueError::none;
  std::optional<CatalogSchemaRecord> record;
  bool ok() const { return error == CatalogValueError::none && record.has_value(); }
};
const CatalogValueSchema& CatalogSchemaRecordSchema();
CatalogValueEncodeResult EncodeCatalogSchemaRecord(const CatalogSchemaRecord& record);
CatalogSchemaRecordDecodeResult DecodeCatalogSchemaRecord(std::string_view bytes);
bool CatalogSchemaPayloadMatchesHeader(const CatalogTypedRecord& record);
// Validates every physical schema parent against the complete catalog inventory.
// No name/path lookup participates in this graph validation.
bool ValidateCatalogSchemaGraph(const std::vector<CatalogTypedRecord>& records);
}  // namespace scratchbird::core::catalog
