// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "catalog_metric_descriptor.hpp"
#include "metric_value_codec.hpp"
#include "uuid.hpp"

namespace scratchbird::core::catalog {
// Binary bootstrap/current observation. The embedded definition describes the
// value; readers still resolve its exact binding in their catalog snapshot.
// This record does not establish policy activation or MGA publication.
struct CatalogMetricCurrentValue {
  TypedUuid object_uuid;
  TypedUuid database_uuid;
  CatalogMetricDescriptor descriptor;
  metrics::MetricValue value;
};
struct CatalogMetricCurrentValueResult {
  CatalogValueError error = CatalogValueError::invalid_value;
  std::optional<CatalogMetricCurrentValue> record;
  bool ok() const { return error == CatalogValueError::none && record.has_value(); }
};
inline constexpr std::array<CatalogValueFieldSchema,4> kCatalogCurrentValueFields{{
    {1, CatalogValueType::engine_identity, true, 16, UuidKind::object},
    {2, CatalogValueType::engine_identity, true, 16, UuidKind::database},
    {3, CatalogValueType::opaque_bytes, true, 65536},
    {4, CatalogValueType::opaque_bytes, true, 65536},
}};
struct CatalogMetricCurrentValueView {
  TypedUuid object_uuid;
  TypedUuid database_uuid;
  CatalogMetricDescriptorView descriptor;
  metrics::MetricValueView value;
  std::string_view descriptor_bytes;
};
struct CatalogMetricCurrentValueViewResult {
  CatalogValueError error=CatalogValueError::invalid_value;
  std::optional<CatalogMetricCurrentValueView> record;
  bool ok() const { return error==CatalogValueError::none&&record.has_value(); }
};
// All variable data borrow the immutable outer input. Keep its owner and actual
// memory grant alive; valid structure is not catalog admission/publication.
inline CatalogMetricCurrentValueViewResult DecodeCatalogMetricCurrentValueView(std::string_view bytes) {
  std::array<CatalogValueFieldView,4> fields;
  const auto block=DecodeCatalogValueBlockInto({65547,1,kCatalogCurrentValueFields},
      {reinterpret_cast<const byte*>(bytes.data()),bytes.size()},fields);
  if(!block.ok())return {block.error,{}};
  const auto& descriptor_bytes=fields[2].bytes;
  const std::string_view encoded{reinterpret_cast<const char*>(descriptor_bytes.data()),descriptor_bytes.size()};
  auto descriptor=DecodeCatalogMetricDescriptorView(encoded);
  if(!descriptor.ok())return {descriptor.error,{}};
  auto value=metrics::DecodeMetricValueView(descriptor.record->definition,fields[3].bytes);
  if(!value.ok())return {CatalogValueError::invalid_value,{}};
  return {CatalogValueError::none,CatalogMetricCurrentValueView{
      *fields[0].identity(),*fields[1].identity(),std::move(*descriptor.record),std::move(*value.value),encoded}};
}
inline const CatalogValueSchema& CatalogMetricCurrentValueSchema() {
  static const CatalogValueSchema schema{65547,1,
      {kCatalogCurrentValueFields.begin(),kCatalogCurrentValueFields.end()}};
  return schema;
}
inline CatalogValueEncodeResult EncodeCatalogMetricCurrentValue(const CatalogMetricCurrentValue& r) {
  if (r.object_uuid.kind != UuidKind::object || !uuid::IsEngineIdentityUuid(r.object_uuid.value) ||
      r.database_uuid.kind != UuidKind::database || !uuid::IsEngineIdentityUuid(r.database_uuid.value))
    return {CatalogValueError::invalid_value, {}};
  auto descriptor = EncodeCatalogMetricDescriptor(r.descriptor);
  auto value = metrics::EncodeMetricValue(r.descriptor.definition, r.value);
  if (!descriptor.ok() || !value.ok()) return {CatalogValueError::invalid_value, {}};
  return EncodeCatalogValueBlock(CatalogMetricCurrentValueSchema(), {
      {1, r.object_uuid}, {2, r.database_uuid},
      {3, std::move(descriptor.bytes)}, {4, std::move(value.bytes)}});
}
inline CatalogMetricCurrentValueResult DecodeCatalogMetricCurrentValue(std::string_view bytes) {
  const auto decoded=DecodeCatalogMetricCurrentValueView(bytes);
  if(!decoded.ok())return {decoded.error,{}};
  auto descriptor=DecodeCatalogMetricDescriptor(decoded.record->descriptor_bytes);
  if(!descriptor.ok())return {descriptor.error,{}};
  return {CatalogValueError::none, CatalogMetricCurrentValue{
      decoded.record->object_uuid,decoded.record->database_uuid,
      std::move(*descriptor.record),metrics::MaterializeMetricValue(decoded.record->value)}};
}
inline bool IsCatalogMetricCurrentValuePayload(std::string_view bytes) {
  return bytes.size() >= kCatalogValueBlockHeaderBytes && bytes.substr(0, 4) == "SBCV" &&
      platform::LoadLittle32(reinterpret_cast<const byte*>(bytes.data()) + 16) == 65547;
}
inline bool CatalogMetricCurrentValueMatchesHeader(const CatalogTypedRecordView& r) {
  if (r.header.kind != CatalogRecordKind::metric_current_value) return false;
  const auto decoded = DecodeCatalogMetricCurrentValueView(r.payload);
  return decoded.ok() && decoded.record->object_uuid.kind == r.header.object_uuid.kind &&
      decoded.record->object_uuid.value == r.header.object_uuid.value;
}
inline bool CatalogMetricCurrentValueMatchesHeader(const CatalogTypedRecord& r) {
  return CatalogMetricCurrentValueMatchesHeader(BorrowCatalogTypedRecord(r));
}
}  // namespace scratchbird::core::catalog
