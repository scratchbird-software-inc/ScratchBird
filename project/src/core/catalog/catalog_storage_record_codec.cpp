// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "catalog_storage_record_codec.hpp"

namespace scratchbird::core::catalog {
namespace {
bool Valid(const CatalogStorageRecord& r) {
  return r.page_size >= 8192 && r.page_size <= 131072 &&
      (r.page_size & (r.page_size - 1)) == 0 &&
      r.creator_transaction_number != 0 && !r.descriptor_name.empty() &&
      r.descriptor_name.size() <= 4096;
}
}  // namespace
const CatalogValueSchema& CatalogStorageRecordSchema() {
  using T = CatalogValueType;
  static const CatalogValueSchema schema{65616, 1, {
      {1, T::engine_identity, true, 16, UuidKind::object},
      {2, T::engine_identity, true, 16, UuidKind::filespace},
      {3, T::unsigned_integer, true, 8},
      {4, T::unsigned_integer, true, 8},
      {5, T::utf8_text, true, 4096},
  }};
  return schema;
}
CatalogValueEncodeResult EncodeCatalogStorageRecord(const CatalogStorageRecord& r) {
  if (!Valid(r)) return {CatalogValueError::invalid_value, {}};
  return EncodeCatalogValueBlock(CatalogStorageRecordSchema(), {
      {1, r.descriptor_uuid}, {2, r.filespace_uuid}, {3, r.page_size},
      {4, r.creator_transaction_number}, {5, r.descriptor_name}});
}
CatalogStorageRecordDecodeResult DecodeCatalogStorageRecord(std::string_view bytes) {
  if (bytes.size() < 113 || bytes.size() > 4208)
    return {CatalogValueError::invalid_framing, {}};
  const auto decoded = DecodeCatalogValueBlock(CatalogStorageRecordSchema(),
      std::vector<byte>(bytes.begin(), bytes.end()));
  if (!decoded.ok()) return {decoded.error, {}};
  CatalogStorageRecord r;
  r.descriptor_uuid = std::get<TypedUuid>(decoded.fields[0].value);
  r.filespace_uuid = std::get<TypedUuid>(decoded.fields[1].value);
  r.page_size = std::get<u64>(decoded.fields[2].value);
  r.creator_transaction_number = std::get<u64>(decoded.fields[3].value);
  r.descriptor_name = std::get<std::string>(decoded.fields[4].value);
  if (!Valid(r)) return {CatalogValueError::invalid_value, {}};
  return {CatalogValueError::none, std::move(r)};
}
bool CatalogStoragePayloadMatchesHeader(const CatalogTypedRecord& record) {
  const auto decoded = DecodeCatalogStorageRecord(record.payload);
  return record.header.kind == CatalogRecordKind::storage_descriptor && decoded.ok() &&
      record.header.object_uuid.kind == UuidKind::object &&
      record.header.object_uuid.value == decoded.record->descriptor_uuid.value &&
      record.header.row_uuid.value != record.header.object_uuid.value;
}
}  // namespace scratchbird::core::catalog
