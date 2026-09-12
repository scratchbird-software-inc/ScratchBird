// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "catalog_value_codec.hpp"
#include <optional>
#include <string_view>

namespace scratchbird::core::catalog {
constexpr std::size_t kCatalogLocalizedPayloadMaxBytes = 130976;
// Presentation metadata, never a substitute for CatalogNameEntry authority.
struct CatalogLocalizedNameRecord {
  TypedUuid target_object_uuid;
  std::string language;
  std::string path;
  std::string name;
  u64 name_class = 1;  // default_name
  u64 creator_transaction_number = 0;
};
struct CatalogLocalizedCommentRecord {
  TypedUuid target_object_uuid;
  std::string language;
  std::string comment;
  u64 creator_transaction_number = 0;
};
template <typename Record>
struct CatalogLocalizedDecodeResult {
  CatalogValueError error = CatalogValueError::none;
  std::optional<Record> record;
  bool ok() const { return error == CatalogValueError::none && record.has_value(); }
};
const CatalogValueSchema& CatalogLocalizedNameSchema();
const CatalogValueSchema& CatalogLocalizedCommentSchema();
CatalogValueEncodeResult EncodeCatalogLocalizedName(const CatalogLocalizedNameRecord& record);
CatalogValueEncodeResult EncodeCatalogLocalizedComment(const CatalogLocalizedCommentRecord& record);
CatalogLocalizedDecodeResult<CatalogLocalizedNameRecord> DecodeCatalogLocalizedName(std::string_view bytes);
CatalogLocalizedDecodeResult<CatalogLocalizedCommentRecord> DecodeCatalogLocalizedComment(std::string_view bytes);
}  // namespace scratchbird::core::catalog
