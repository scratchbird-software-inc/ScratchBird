// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "api_types.hpp"
#include "catalog/column_metadata_codec.hpp"
#include "datatype_binary_view.hpp"
#include "datatype_catalog_manifest.hpp"

#include <array>
#include <bit>
#include <string>

namespace scratchbird::engine::internal_api {

// Frozen UTC component adapter, never current local-civil TIMESTAMP authority.
// Admission of the containing live statement receipt remains the caller's job.
// Carry the complete immutable cohort here so no consumer guesses bootstrap,
// latest catalog, or a codec from the timestamp enum or its presentation name.
inline bool BuildHistoricalTimestampScalarDescriptorV1(
    const RelationalTypeDescriptor& source, EngineDescriptor* output,
    std::string* detail) {
  namespace dt = core::datatypes;
  if (!output || !detail) return false;
  const auto refuse = [&] {
    *detail = "CTI.TEMPORAL.DESCRIPTOR_INVALID";
    return false;
  };
  if (!source.datatype_identity_authoritative ||
      !core::uuid::IsEngineIdentityUuid(source.descriptor_uuid) ||
      !core::uuid::IsEngineIdentityUuid(source.statement_receipt_uuid) ||
      (source.nullability != RelationalNullability::kNonNull &&
       source.nullability != RelationalNullability::kNullable) ||
      source.collation_uuid || source.width || source.precision || source.scale ||
      (source.timezone_profile_id && *source.timezone_profile_id != "timestamp_timezone_profile"))
    return refuse();
  const dt::DatatypeTypeCodecIdentityRowV1* matched = nullptr;
  for (const auto& row : dt::CurrentDatatypeTypeCodecIdentityRowsV1()) {
    if (row.catalog_snapshot_uuid != source.datatype_catalog_snapshot_uuid ||
        row.catalog_generation != source.datatype_catalog_generation ||
        row.registry_generation != source.datatype_registry_generation ||
        row.type_uuid != source.type_uuid ||
        row.descriptor_generation != source.descriptor_generation) continue;
    if (matched) return refuse();
    matched = &row;
  }
  if (!matched || matched->type_generation != source.type_generation ||
      matched->codec_id != source.codec_id || matched->codec_version != source.codec_version ||
      matched->codec_generation != source.codec_generation) return refuse();
  const std::array<std::uint8_t, 16> epoch{};
  if (!dt::ValidateHistoricalTimestampUtcValueViewV1(*matched,
          {dt::CanonicalTypeId::timestamp, false, false, epoch.data(), epoch.size()}).ok())
    return refuse();
  CatalogColumnMetadata metadata;
  metadata.identities = {{"catalog_snapshot_uuid", matched->catalog_snapshot_uuid},
                         {"codec_uuid", matched->codec_uuid},
                         {"statement_receipt_uuid", source.statement_receipt_uuid}};
  metadata.text = {{"nullability", source.nullability == RelationalNullability::kNullable ? "nullable" : "non_null"},
                   {"catalog_generation", std::to_string(matched->catalog_generation)},
                   {"registry_generation", std::to_string(matched->registry_generation)},
                   {"type_generation", std::to_string(matched->type_generation)},
                   {"codec_id", matched->codec_id},
                   {"codec_version", std::to_string(matched->codec_version)},
                   {"codec_generation", std::to_string(matched->codec_generation)}};
  EngineDescriptor descriptor;
  descriptor.descriptor_uuid = source.descriptor_uuid;
  descriptor.descriptor_kind = "scalar";
  descriptor.canonical_type_name = "timestamp";
  descriptor.type_uuid = matched->type_uuid;
  descriptor.datatype_descriptor_uuid = matched->descriptor_uuid;
  descriptor.datatype_descriptor_generation = matched->descriptor_generation;
  if (!EncodeCatalogColumnMetadata(metadata, &descriptor.encoded_descriptor)) return refuse();
  *output = std::move(descriptor);
  detail->clear();
  return true;
}

inline const core::datatypes::DatatypeTypeCodecIdentityRowV1*
ResolveHistoricalTimestampScalarIdentityV1(const EngineDescriptor& descriptor,
                                         bool* nullable, std::string* detail,
                                         EngineUuid* statement_receipt = nullptr) {
  namespace dt = core::datatypes;
  if (!nullable || !detail) return nullptr;
  *detail = "CTI.TEMPORAL.DESCRIPTOR_INVALID";
  CatalogColumnMetadata fields;
  if (descriptor.descriptor_kind != "scalar" || descriptor.canonical_type_name != "timestamp" ||
      !core::uuid::IsEngineIdentityUuid(descriptor.descriptor_uuid) ||
      !descriptor.charset_uuid.is_nil() || !descriptor.collation_uuid.is_nil() ||
      !DecodeCatalogColumnMetadata(descriptor.encoded_descriptor, &fields) ||
      fields.identities.size() != 3 || fields.text.size() != 7) return nullptr;
  const auto id = [&](const char* key) -> EngineUuid {
    const auto found = fields.identities.find(key);
    return found == fields.identities.end() ? EngineUuid{} : found->second;
  };
  const auto text = [&](const char* key, std::string_view expected) {
    const auto found = fields.text.find(key);
    return found != fields.text.end() && found->second == expected;
  };
  if (!core::uuid::IsEngineIdentityUuid(id("statement_receipt_uuid")) ||
      (!text("nullability", "nullable") && !text("nullability", "non_null"))) return nullptr;
  const dt::DatatypeTypeCodecIdentityRowV1* matched = nullptr;
  for (const auto& row : dt::CurrentDatatypeTypeCodecIdentityRowsV1()) {
    if (row.catalog_snapshot_uuid != id("catalog_snapshot_uuid") ||
        row.descriptor_uuid != descriptor.datatype_descriptor_uuid ||
        row.descriptor_generation != descriptor.datatype_descriptor_generation ||
        row.type_uuid != descriptor.type_uuid || row.codec_uuid != id("codec_uuid") ||
        !text("catalog_generation", std::to_string(row.catalog_generation)) ||
        !text("registry_generation", std::to_string(row.registry_generation)) ||
        !text("type_generation", std::to_string(row.type_generation)) ||
        !text("codec_id", row.codec_id) ||
        !text("codec_version", std::to_string(row.codec_version)) ||
        !text("codec_generation", std::to_string(row.codec_generation))) continue;
    if (matched) return nullptr;
    matched = &row;
  }
  if (!matched) return nullptr;
  const std::array<std::uint8_t, 16> epoch{};
  if (!dt::ValidateHistoricalTimestampUtcValueViewV1(*matched,
          {dt::CanonicalTypeId::timestamp, false, false, epoch.data(), epoch.size()}).ok()) return nullptr;
  *nullable = text("nullability", "nullable");
  if (statement_receipt) *statement_receipt = id("statement_receipt_uuid");
  detail->clear();
  return matched;
}

struct HistoricalTimestampScalarPartsV1 {
  std::int64_t unix_seconds{0};
  std::uint32_t nanoseconds{0};
  bool is_null{false};
};

inline bool DecodeHistoricalTimestampScalarV1(const EngineTypedValue& value,
                                            HistoricalTimestampScalarPartsV1* output,
                                            std::string* detail,
                                            const core::datatypes::DatatypeTypeCodecIdentityRowV1**
                                                admitted_identity = nullptr,
                                            EngineUuid* statement_receipt = nullptr) {
  namespace dt = core::datatypes;
  if (!output || !detail) return false;
  bool nullable = false;
  EngineUuid receipt;
  const auto* identity = ResolveHistoricalTimestampScalarIdentityV1(value.descriptor, &nullable, detail, &receipt);
  if (!identity) return false;
  const bool is_null = value.state == EngineValueState::sql_null;
  if ((!is_null && value.state != EngineValueState::value) ||
      value.is_null != is_null || (is_null && !nullable) || !value.encoded_value.empty()) {
    *detail = is_null ? "DATATYPE.NULL_STATE.INVALID" : "CTI.TEMPORAL.CANONICAL_ENCODING_INVALID";
    return false;
  }
  const auto validated = dt::ValidateHistoricalTimestampUtcValueViewV1(*identity,
      {dt::CanonicalTypeId::timestamp, is_null, false,
       value.binary_value.data(), value.binary_value.size()});
  if (!validated.ok()) {
    *detail = validated.diagnostic.diagnostic_code;
    return false;
  }
  HistoricalTimestampScalarPartsV1 parts;
  parts.is_null = is_null;
  if (!is_null) {
    std::uint64_t seconds = 0;
    for (unsigned byte = 0; byte < 8; ++byte)
      seconds |= static_cast<std::uint64_t>(value.binary_value[byte]) << (8 * byte);
    parts.unix_seconds = std::bit_cast<std::int64_t>(seconds);
    for (unsigned byte = 0; byte < 4; ++byte)
      parts.nanoseconds |= static_cast<std::uint32_t>(value.binary_value[8 + byte]) << (8 * byte);
  }
  *output = parts;
  if (admitted_identity) *admitted_identity = identity;
  if (statement_receipt) *statement_receipt = receipt;
  detail->clear();
  return true;
}

inline bool CompareHistoricalTimestampScalarsV1(const EngineTypedValue& left,
                                              const EngineTypedValue& right,
                                              int* comparison, std::string* detail) {
  if (!comparison || !detail) return false;
  HistoricalTimestampScalarPartsV1 l, r;
  const core::datatypes::DatatypeTypeCodecIdentityRowV1* left_identity = nullptr;
  const core::datatypes::DatatypeTypeCodecIdentityRowV1* right_identity = nullptr;
  EngineUuid left_receipt, right_receipt;
  if (!DecodeHistoricalTimestampScalarV1(left, &l, detail, &left_identity, &left_receipt) ||
      !DecodeHistoricalTimestampScalarV1(right, &r, detail, &right_identity, &right_receipt)) return false;
  if (left_identity != right_identity || left_receipt != right_receipt || l.is_null || r.is_null) {
    *detail = "CTI.TEMPORAL.DESCRIPTOR_INVALID";
    return false;
  }
  *comparison = l.unix_seconds != r.unix_seconds
      ? (l.unix_seconds < r.unix_seconds ? -1 : 1)
      : l.nanoseconds < r.nanoseconds ? -1 : l.nanoseconds > r.nanoseconds ? 1 : 0;
  detail->clear();
  return true;
}

}  // namespace scratchbird::engine::internal_api
