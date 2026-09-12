// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "catalog_value_codec.hpp"
#include "catalog_record_codec.hpp"
#include <optional>
#include <string_view>

namespace scratchbird::core::catalog {
constexpr std::size_t kCatalogResourcePayloadMaxBytes = 130976;
struct CatalogCharsetRecord {
  std::string canonical_name;
  TypedUuid resource_uuid;
  std::vector<std::string> aliases;
  std::string description;
  u64 min_bytes = 0, max_bytes = 0;
  bool variable_width = false;
  std::string encoding_type, iana_name;
  std::vector<std::string> supported_by;
  std::string default_collation_name;
  std::optional<TypedUuid> default_collation_uuid;
  std::string source_path;
  u64 resource_epoch = 0, family_epoch = 0;
  std::string family_version, resource_seed_pack, resource_seed_version;
  bool loaded_at_database_create = false, engine_owned = false;
  u64 creator_transaction_number = 0;
};
struct CatalogCollationRecord {
  std::string canonical_name;
  TypedUuid resource_uuid;
  std::string charset_name;
  TypedUuid charset_uuid;
  bool default_for_charset = false;
  std::string default_authority;
  bool case_insensitive = false, accent_insensitive = false;
  std::string language, description;
  std::vector<std::string> supported_by;
  std::string source_path;
  u64 resource_epoch = 0, family_epoch = 0;
  std::string family_version, resource_seed_pack, resource_seed_version;
  bool loaded_at_database_create = false, engine_owned = false;
  u64 creator_transaction_number = 0;
};
template <typename Record> struct CatalogResourceDecodeResult {
  CatalogValueError error = CatalogValueError::none;
  std::optional<Record> record;
  bool ok() const { return error == CatalogValueError::none && record.has_value(); }
};
const CatalogValueSchema& CatalogCharsetRecordSchema();
const CatalogValueSchema& CatalogCollationRecordSchema();
CatalogValueEncodeResult EncodeCatalogCharsetRecord(const CatalogCharsetRecord& record);
CatalogValueEncodeResult EncodeCatalogCollationRecord(const CatalogCollationRecord& record);
CatalogResourceDecodeResult<CatalogCharsetRecord> DecodeCatalogCharsetRecord(std::string_view bytes);
CatalogResourceDecodeResult<CatalogCollationRecord> DecodeCatalogCollationRecord(std::string_view bytes);
bool CatalogResourcePayloadMatchesHeader(const CatalogTypedRecord& record);
// Input is the owning admitted catalog record set, not untrusted expectations
// copied from a payload. Full MGA visibility/publication remains caller-owned.
bool ValidateCatalogResourceGraph(const std::vector<CatalogTypedRecord>& records);
}  // namespace scratchbird::core::catalog
