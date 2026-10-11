// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "catalog_database_record_codec.hpp"
#include "../datatypes/admitted_datatype_cohort.hpp"
#include <array>
#include <limits>
namespace scratchbird::core::catalog {
namespace {
using T = CatalogValueType;
constexpr std::array<CatalogValueFieldSchema, 12> kDatabaseFields{{
    {1,T::engine_identity,true,16,UuidKind::database},
    {2,T::unsigned_integer,true,8}, {3,T::unsigned_integer,true,8},
    {4,T::unsigned_integer,true,8}, {5,T::unsigned_integer,true,8},
    {6,T::unsigned_integer,true,8}, {7,T::unsigned_integer,true,8},
    {8,T::unsigned_integer,true,8}, {9,T::unsigned_integer,true,8},
    {10,T::engine_identity,true,16,UuidKind::object},
    {11,T::unsigned_integer,true,8}, {12,T::unsigned_integer,true,8}}};
constexpr CatalogValueSchemaView kDatabaseSchemaV1{
    65537, 1, std::span(kDatabaseFields).first<9>()};
constexpr CatalogValueSchemaView kDatabaseSchemaV2{65537, 2, kDatabaseFields};
bool Valid(const CatalogDatabaseRecord& r) {
  constexpr auto max = std::numeric_limits<u32>::max();
  return r.database_header_format_major > 0 && r.database_header_format_major <= max &&
      r.database_header_format_minor <= max &&
      r.catalog_manifest_format_version > 0 && r.catalog_manifest_format_version <= max &&
      r.page_size > 0 && r.page_size <= max && r.creator_transaction_number > 0 &&
      (!r.datatype_cohort ||
       (r.datatype_cohort->catalog_snapshot_uuid.kind == UuidKind::object &&
        datatypes::IsAdmittedDatatypeCohort(
            r.datatype_cohort->catalog_snapshot_uuid.value,
            r.datatype_cohort->catalog_generation,
            r.datatype_cohort->registry_generation)));
}
}
const CatalogValueSchema& CatalogDatabaseRecordSchema() {
  static const CatalogValueSchema schema{65537, 1,
      {kDatabaseFields.begin(), kDatabaseFields.begin() + 9}};
  return schema;
}
const CatalogValueSchema& CatalogDatabaseRecordSchemaV2() {
  static const CatalogValueSchema schema{65537, 2,
      {kDatabaseFields.begin(), kDatabaseFields.end()}};
  return schema;
}
CatalogValueEncodeResult EncodeCatalogDatabaseRecord(const CatalogDatabaseRecord& r) {
  if (!Valid(r)) return {CatalogValueError::invalid_value, {}};
  if (r.datatype_cohort) {
    const auto& cohort = *r.datatype_cohort;
    return EncodeCatalogValueBlock(CatalogDatabaseRecordSchemaV2(), {
        {1,r.database_uuid}, {2,r.database_header_format_major},
        {3,r.database_header_format_minor}, {4,r.catalog_manifest_format_version},
        {5,r.page_size}, {6,r.creation_unix_epoch_millis}, {7,r.feature_flags},
        {8,r.compatibility_flags}, {9,r.creator_transaction_number},
        {10,cohort.catalog_snapshot_uuid}, {11,cohort.catalog_generation},
        {12,cohort.registry_generation}});
  }
  return EncodeCatalogValueBlock(CatalogDatabaseRecordSchema(), {
      {1,r.database_uuid}, {2,r.database_header_format_major},
      {3,r.database_header_format_minor}, {4,r.catalog_manifest_format_version},
      {5,r.page_size}, {6,r.creation_unix_epoch_millis}, {7,r.feature_flags},
      {8,r.compatibility_flags}, {9,r.creator_transaction_number}});
}
CatalogDatabaseRecordDecodeResult DecodeCatalogDatabaseRecord(std::string_view bytes) {
  // Fixed stack-backed structural admission: neither a payload copy nor a
  // heap-allocated field vector is needed. Only native values escape the view.
  if (bytes.size() != 176 && bytes.size() != 232)
    return {CatalogValueError::invalid_framing, {}};
  const bool v2 = bytes.size() == 232;
  std::array<CatalogValueFieldView, 12> fields{};
  const auto decoded = DecodeCatalogValueBlockInto(
      v2 ? kDatabaseSchemaV2 : kDatabaseSchemaV1,
      {reinterpret_cast<const byte*>(bytes.data()), bytes.size()}, fields);
  if (!decoded.ok()) return {decoded.error, {}};
  CatalogDatabaseRecord r;
  r.database_uuid = *decoded.fields[0].identity();
  r.database_header_format_major = *decoded.fields[1].unsigned_value();
  r.database_header_format_minor = *decoded.fields[2].unsigned_value();
  r.catalog_manifest_format_version = *decoded.fields[3].unsigned_value();
  r.page_size = *decoded.fields[4].unsigned_value();
  r.creation_unix_epoch_millis = *decoded.fields[5].unsigned_value();
  r.feature_flags = *decoded.fields[6].unsigned_value();
  r.compatibility_flags = *decoded.fields[7].unsigned_value();
  r.creator_transaction_number = *decoded.fields[8].unsigned_value();
  if (v2) {
    r.datatype_cohort = CatalogDatabaseDatatypeCohort{
        *decoded.fields[9].identity(),
        *decoded.fields[10].unsigned_value(),
        *decoded.fields[11].unsigned_value()};
  }
  if (!Valid(r)) return {CatalogValueError::invalid_value, {}};
  return {CatalogValueError::none, r};
}
}  // namespace scratchbird::core::catalog
