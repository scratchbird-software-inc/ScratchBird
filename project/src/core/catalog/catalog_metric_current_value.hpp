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
inline const CatalogValueSchema& CatalogMetricCurrentValueSchema() {
  using T = CatalogValueType;
  static const CatalogValueSchema schema{65547, 1, {
      {1, T::engine_identity, true, 16, UuidKind::object},
      {2, T::engine_identity, true, 16, UuidKind::database},
      {3, T::opaque_bytes, true, 65536},
      {4, T::opaque_bytes, true, 65536},
  }};
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
  if (bytes.size() > kCatalogValueBlockMaxBytes) return {CatalogValueError::size_limit, {}};
  const auto block = DecodeCatalogValueBlock(CatalogMetricCurrentValueSchema(),
      std::vector<byte>(bytes.begin(), bytes.end()));
  if (!block.ok()) return {block.error, {}};
  const auto& descriptor_bytes = std::get<std::vector<byte>>(block.fields[2].value);
  const auto descriptor = DecodeCatalogMetricDescriptor(
      {reinterpret_cast<const char*>(descriptor_bytes.data()), descriptor_bytes.size()});
  if (!descriptor.ok()) return {descriptor.error, {}};
  const auto value = metrics::DecodeMetricValue(descriptor.record->definition,
      std::get<std::vector<byte>>(block.fields[3].value));
  if (!value.ok()) return {CatalogValueError::invalid_value, {}};
  return {CatalogValueError::none, CatalogMetricCurrentValue{
      std::get<TypedUuid>(block.fields[0].value), std::get<TypedUuid>(block.fields[1].value),
      *descriptor.record, *value.value}};
}
inline bool IsCatalogMetricCurrentValuePayload(std::string_view bytes) {
  return bytes.size() >= kCatalogValueBlockHeaderBytes && bytes.substr(0, 4) == "SBCV" &&
      platform::LoadLittle32(reinterpret_cast<const byte*>(bytes.data()) + 16) == 65547;
}
inline bool CatalogMetricCurrentValueMatchesHeader(const CatalogTypedRecord& r) {
  if (r.header.kind != CatalogRecordKind::metric_current_value) return false;
  const auto decoded = DecodeCatalogMetricCurrentValue(r.payload);
  return decoded.ok() && decoded.record->object_uuid.kind == r.header.object_uuid.kind &&
      decoded.record->object_uuid.value == r.header.object_uuid.value;
}
}  // namespace scratchbird::core::catalog
