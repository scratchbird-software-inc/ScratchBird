// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "catalog_value_codec.hpp"
#include <optional>
#include <string_view>

namespace scratchbird::core::catalog {
// Durable node-level datatype binding. Absence in the frozen V1 payload is
// explicit: it is never permission to infer the running binary's default.
struct CatalogDatabaseDatatypeCohort {
  TypedUuid catalog_snapshot_uuid;
  u64 catalog_generation = 0;
  u64 registry_generation = 0;
  bool operator==(const CatalogDatabaseDatatypeCohort& other) const {
    return catalog_snapshot_uuid.kind == other.catalog_snapshot_uuid.kind &&
        catalog_snapshot_uuid.value == other.catalog_snapshot_uuid.value &&
        catalog_generation == other.catalog_generation &&
        registry_generation == other.registry_generation;
  }
};

// CATALOG_DATABASE_BINARY_PAYLOAD_V1/V2. V2 appends the exact binary cohort;
// V1 remains byte-for-byte readable and encodable without manufacturing one.
struct CatalogDatabaseRecord {
  TypedUuid database_uuid;
  u64 database_header_format_major = 0;
  u64 database_header_format_minor = 0;
  u64 catalog_manifest_format_version = 0;
  u64 page_size = 0;
  u64 creation_unix_epoch_millis = 0;
  u64 feature_flags = 0;
  u64 compatibility_flags = 0;
  u64 creator_transaction_number = 0;
  std::optional<CatalogDatabaseDatatypeCohort> datatype_cohort;
};
struct CatalogDatabaseRecordDecodeResult {
  CatalogValueError error = CatalogValueError::none;
  std::optional<CatalogDatabaseRecord> record;
  bool ok() const { return error == CatalogValueError::none && record.has_value(); }
};
const CatalogValueSchema& CatalogDatabaseRecordSchema();
const CatalogValueSchema& CatalogDatabaseRecordSchemaV2();
CatalogValueEncodeResult EncodeCatalogDatabaseRecord(const CatalogDatabaseRecord& record);
CatalogDatabaseRecordDecodeResult DecodeCatalogDatabaseRecord(std::string_view bytes);
}  // namespace scratchbird::core::catalog
