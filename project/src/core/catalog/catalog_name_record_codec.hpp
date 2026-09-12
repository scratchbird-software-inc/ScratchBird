// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "catalog_value_codec.hpp"
#include <optional>

namespace scratchbird::core::catalog {
// CATALOG_NAME_RECORD_SCHEMAS_V1: two payloads of localized_name, not two
// aliases for the incomplete prototype bootstrap localization record.
enum class CatalogNameLifecycle : u64 {
  creating=1, active=2, renaming=3, dropping=4, dropped=5, archived=6, quarantined=7
};
enum class CatalogNameClass : u64 { primary=1, alias=2, compatibility=3, generated=4, hidden=5 };
enum class CatalogNameQuoteStyle : u64 {
  none=0, double_quote=1, backtick=2, bracket=3, sqlite_any=4, donor_api=5
};
struct CatalogNameVector {
  TypedUuid name_vector_uuid;
  TypedUuid object_uuid;
  std::string object_class;
  std::optional<TypedUuid> owning_schema_uuid;
  std::string default_language_tag;
  TypedUuid default_name_entry_uuid;
  TypedUuid name_collision_policy_uuid;
  u64 catalog_generation_id = 0;
  TypedUuid security_policy_uuid;
  CatalogNameLifecycle lifecycle_state = CatalogNameLifecycle::creating;
};
struct CatalogNameEntry {
  TypedUuid name_entry_uuid;
  TypedUuid name_vector_uuid;
  TypedUuid object_uuid;
  std::string object_class;
  TypedUuid scope_uuid;
  std::optional<TypedUuid> parent_object_uuid;
  std::optional<TypedUuid> parent_schema_uuid;
  std::string language_tag;
  CatalogNameClass name_class = CatalogNameClass::primary;
  std::string donor_id;
  TypedUuid dialect_profile_uuid;
  TypedUuid identifier_profile_uuid;
  std::optional<TypedUuid> case_fold_profile_uuid;
  std::optional<TypedUuid> quoted_identifier_profile_uuid;
  std::string raw_name_text;
  std::string display_name;
  bool was_quoted = false;
  CatalogNameQuoteStyle quote_style = CatalogNameQuoteStyle::none;
  bool requires_exact_match = false;
  std::vector<byte> normalized_lookup_key;
  std::vector<byte> exact_lookup_key;
  std::vector<byte> full_path_lookup_key;
  u64 path_component_count = 0;
  bool search_path_eligible = false;
  bool default_for_language = false;
  bool default_for_object = false;
  u64 catalog_generation_id = 0;
  TypedUuid created_transaction_uuid;
  std::optional<TypedUuid> dropped_transaction_uuid;
  TypedUuid security_policy_uuid;
  u64 resource_epoch = 0;
  u64 name_resolution_epoch = 0;
  CatalogNameLifecycle lifecycle_state = CatalogNameLifecycle::creating;
};
template <typename Record> struct CatalogNameRecordDecodeResult {
  CatalogValueError error = CatalogValueError::none;
  std::optional<Record> record;
  bool ok() const { return error == CatalogValueError::none && record.has_value(); }
};
const CatalogValueSchema& CatalogNameVectorSchema();
const CatalogValueSchema& CatalogNameEntrySchema();
CatalogValueEncodeResult EncodeCatalogNameVector(const CatalogNameVector& record);
CatalogValueEncodeResult EncodeCatalogNameEntry(const CatalogNameEntry& record);
CatalogNameRecordDecodeResult<CatalogNameVector> DecodeCatalogNameVector(const std::vector<byte>& bytes);
CatalogNameRecordDecodeResult<CatalogNameEntry> DecodeCatalogNameEntry(const std::vector<byte>& bytes);
}  // namespace scratchbird::core::catalog
