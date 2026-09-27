// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "catalog_record_codec.hpp"
#include "catalog_value_codec.hpp"
#include <optional>
#include <string_view>

namespace scratchbird::core::catalog {
// STORAGE-DESCRIPTOR-NATIVE-CATALOG-V1. Names are presentation, not identity.
struct CatalogStorageRecord {
  TypedUuid descriptor_uuid{};
  TypedUuid filespace_uuid{};
  u64 page_size = 0;
  u64 creator_transaction_number = 0;
  std::string descriptor_name;
};
struct CatalogStorageRecordDecodeResult {
  CatalogValueError error = CatalogValueError::none;
  std::optional<CatalogStorageRecord> record;
  bool ok() const { return error == CatalogValueError::none && record.has_value(); }
};
const CatalogValueSchema& CatalogStorageRecordSchema();
CatalogValueEncodeResult EncodeCatalogStorageRecord(const CatalogStorageRecord& record);
CatalogStorageRecordDecodeResult DecodeCatalogStorageRecord(std::string_view bytes);
bool CatalogStoragePayloadMatchesHeader(const CatalogTypedRecord& record);
}  // namespace scratchbird::core::catalog
