// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "catalog_database_record_codec.hpp"
#include <limits>
namespace scratchbird::core::catalog {
namespace {
bool Valid(const CatalogDatabaseRecord& r) {
  constexpr auto max = std::numeric_limits<u32>::max();
  return r.database_header_format_major > 0 && r.database_header_format_major <= max &&
      r.database_header_format_minor <= max &&
      r.catalog_manifest_format_version > 0 && r.catalog_manifest_format_version <= max &&
      r.page_size > 0 && r.page_size <= max && r.creator_transaction_number > 0;
}
}
const CatalogValueSchema& CatalogDatabaseRecordSchema() {
  using T = CatalogValueType;
  static const CatalogValueSchema schema{65537, 1, {
      {1,T::engine_identity,true,16,UuidKind::database},
      {2,T::unsigned_integer,true,8}, {3,T::unsigned_integer,true,8},
      {4,T::unsigned_integer,true,8}, {5,T::unsigned_integer,true,8},
      {6,T::unsigned_integer,true,8}, {7,T::unsigned_integer,true,8},
      {8,T::unsigned_integer,true,8}, {9,T::unsigned_integer,true,8}}};
  return schema;
}
CatalogValueEncodeResult EncodeCatalogDatabaseRecord(const CatalogDatabaseRecord& r) {
  if (!Valid(r)) return {CatalogValueError::invalid_value, {}};
  return EncodeCatalogValueBlock(CatalogDatabaseRecordSchema(), {
      {1,r.database_uuid}, {2,r.database_header_format_major},
      {3,r.database_header_format_minor}, {4,r.catalog_manifest_format_version},
      {5,r.page_size}, {6,r.creation_unix_epoch_millis}, {7,r.feature_flags},
      {8,r.compatibility_flags}, {9,r.creator_transaction_number}});
}
CatalogDatabaseRecordDecodeResult DecodeCatalogDatabaseRecord(std::string_view bytes) {
  // Fixed schema size prevents unbounded input copies before generic admission.
  if (bytes.size() != 176) return {CatalogValueError::invalid_framing, {}};
  const std::vector<byte> input(bytes.begin(), bytes.end());
  const auto decoded = DecodeCatalogValueBlock(CatalogDatabaseRecordSchema(), input);
  if (!decoded.ok()) return {decoded.error, {}};
  CatalogDatabaseRecord r;
  r.database_uuid = std::get<TypedUuid>(decoded.fields[0].value);
  r.database_header_format_major = std::get<u64>(decoded.fields[1].value);
  r.database_header_format_minor = std::get<u64>(decoded.fields[2].value);
  r.catalog_manifest_format_version = std::get<u64>(decoded.fields[3].value);
  r.page_size = std::get<u64>(decoded.fields[4].value);
  r.creation_unix_epoch_millis = std::get<u64>(decoded.fields[5].value);
  r.feature_flags = std::get<u64>(decoded.fields[6].value);
  r.compatibility_flags = std::get<u64>(decoded.fields[7].value);
  r.creator_transaction_number = std::get<u64>(decoded.fields[8].value);
  if (!Valid(r)) return {CatalogValueError::invalid_value, {}};
  return {CatalogValueError::none, r};
}
}  // namespace scratchbird::core::catalog
