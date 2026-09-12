// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "catalog_value_codec.hpp"
#include <optional>
#include <string_view>

namespace scratchbird::core::catalog {
// CATALOG_DATABASE_BINARY_PAYLOAD_V1: complete current database identity payload.
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
};
struct CatalogDatabaseRecordDecodeResult {
  CatalogValueError error = CatalogValueError::none;
  std::optional<CatalogDatabaseRecord> record;
  bool ok() const { return error == CatalogValueError::none && record.has_value(); }
};
const CatalogValueSchema& CatalogDatabaseRecordSchema();
CatalogValueEncodeResult EncodeCatalogDatabaseRecord(const CatalogDatabaseRecord& record);
CatalogDatabaseRecordDecodeResult DecodeCatalogDatabaseRecord(std::string_view bytes);
}  // namespace scratchbird::core::catalog
