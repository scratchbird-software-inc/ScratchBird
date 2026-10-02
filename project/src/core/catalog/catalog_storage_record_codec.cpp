// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "catalog_storage_record_codec.hpp"
#include <array>

namespace scratchbird::core::catalog {
namespace {
template<typename Record> bool Valid(const Record& r) {
  return r.page_size >= 8192 && r.page_size <= 131072 &&
      (r.page_size & (r.page_size - 1)) == 0 &&
      r.creator_transaction_number != 0 && !r.descriptor_name.empty() &&
      r.descriptor_name.size() <= 4096;
}
}  // namespace
constexpr CatalogValueSchemaView CatalogStorageRecordSchemaView() {
  using T = CatalogValueType;
  static constexpr CatalogValueFieldSchema fields[]{
      {1, T::engine_identity, true, 16, UuidKind::object},
      {2, T::engine_identity, true, 16, UuidKind::filespace},
      {3, T::unsigned_integer, true, 8},
      {4, T::unsigned_integer, true, 8},
      {5, T::utf8_text, true, 4096},
  };
  return {65616, 1, fields};
}
const CatalogValueSchema& CatalogStorageRecordSchema() {
  static const CatalogValueSchema schema = [] {
    const auto view = CatalogStorageRecordSchemaView();
    return CatalogValueSchema{view.id, view.version, {view.fields.begin(), view.fields.end()}};
  }();
  return schema;
}
CatalogValueEncodeResult EncodeCatalogStorageRecord(const CatalogStorageRecord& r) {
  if (!Valid(r)) return {CatalogValueError::invalid_value, {}};
  return EncodeCatalogValueBlock(CatalogStorageRecordSchema(), {
      {1, r.descriptor_uuid}, {2, r.filespace_uuid}, {3, r.page_size},
      {4, r.creator_transaction_number}, {5, r.descriptor_name}});
}
CatalogStorageRecordViewResult DecodeCatalogStorageRecordView(std::string_view bytes) {
  if (bytes.size() < 113 || bytes.size() > 4208)
    return {CatalogValueError::invalid_framing, {}};
  std::array<CatalogValueFieldView, 5> fields;
  const auto decoded = DecodeCatalogValueBlockInto(CatalogStorageRecordSchemaView(),
      {reinterpret_cast<const byte*>(bytes.data()), bytes.size()}, fields);
  if (!decoded.ok()) return {decoded.error, {}};
  CatalogStorageRecordView r;
  r.descriptor_uuid = *decoded.fields[0].identity();
  r.filespace_uuid = *decoded.fields[1].identity();
  r.page_size = *decoded.fields[2].unsigned_value();
  r.creator_transaction_number = *decoded.fields[3].unsigned_value();
  r.descriptor_name = std::string_view(reinterpret_cast<const char*>(decoded.fields[4].bytes.data()), decoded.fields[4].bytes.size());
  if (!Valid(r)) return {CatalogValueError::invalid_value, {}};
  return {CatalogValueError::none, std::move(r)};
}
CatalogStorageRecordDecodeResult DecodeCatalogStorageRecord(std::string_view bytes) {
  const auto decoded = DecodeCatalogStorageRecordView(bytes);
  if (!decoded.ok()) return {decoded.error, {}};
  const auto& v = *decoded.record;
  return {CatalogValueError::none, CatalogStorageRecord{
      v.descriptor_uuid, v.filespace_uuid, v.page_size, v.creator_transaction_number,
      std::string(v.descriptor_name)}};
}
bool CatalogStoragePayloadMatchesHeader(const CatalogTypedRecord& record) {
  const auto decoded = DecodeCatalogStorageRecordView(record.payload);
  return record.header.kind == CatalogRecordKind::storage_descriptor && decoded.ok() &&
      record.header.object_uuid.kind == UuidKind::object &&
      record.header.object_uuid.value == decoded.record->descriptor_uuid.value &&
      record.header.row_uuid.value != record.header.object_uuid.value;
}
}  // namespace scratchbird::core::catalog
