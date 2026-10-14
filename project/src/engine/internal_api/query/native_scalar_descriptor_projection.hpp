// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "api_types.hpp"
#include "query/relational_type_descriptor.hpp"
#include "datatype_catalog_manifest.hpp"
#include "datatype_decimal_codec_profile.hpp"
#include "catalog/column_metadata_codec.hpp"
#include "uuid.hpp"
#include <type_traits>
#include <utility>

namespace scratchbird::engine::internal_api {

// Pure projection of already admitted occurrence authority. The owning caller
// must additionally bind its live statement/receipt and storage source.
inline const core::datatypes::DatatypeTypeCodecIdentityRowV3*
FindExactRelationalDatatypeIdentityV3(const RelationalTypeDescriptor& source) {
  if (!source.datatype_identity_authoritative ||
      !core::uuid::IsEngineIdentityUuid(source.descriptor_uuid) ||
      !core::uuid::IsEngineIdentityUuid(source.statement_receipt_uuid)) return nullptr;
  const core::datatypes::DatatypeTypeCodecIdentityRowV3* matched = nullptr;
  for (const auto& identity : core::datatypes::CurrentDatatypeTypeCodecIdentityRowsV3()) {
    const auto& row = identity.legacy_fields;
    if (row.catalog_snapshot_uuid != source.datatype_catalog_snapshot_uuid ||
        row.catalog_generation != source.datatype_catalog_generation ||
        row.registry_generation != source.datatype_registry_generation ||
        row.type_uuid != source.type_uuid ||
        row.descriptor_generation != source.descriptor_generation) continue;
    if (matched) return nullptr;
    matched = &identity;
  }
  if (!matched) return nullptr;
  const auto& row = matched->legacy_fields;
  if (source.decimal_codec_profile &&
      (row.canonical_binary_type_code != static_cast<std::uint32_t>(core::datatypes::CanonicalTypeId::decimal) ||
       !source.precision || !source.scale ||
       !core::datatypes::ExactDecimalCodecProfileMatches(source.decimal_codec_profile->uuid,
           source.decimal_codec_profile->generation, *source.precision, *source.scale))) return nullptr;
  return row.type_generation == source.type_generation &&
      row.codec_id == source.codec_id && row.codec_version == source.codec_version &&
      row.codec_generation == source.codec_generation ? matched : nullptr;
}

inline bool BuildExactCanonicalScalarRuntimeDescriptorV1(
    const RelationalTypeDescriptor& source,
    core::datatypes::CanonicalTypeId expected_type, EngineDescriptor* output) {
  if (!output) return false;
  // Build locally and publish only after every identity/profile check and
  // fallible allocation succeeds. Failed inference must not erase a caller's
  // already selected descriptor (including on allocation failure).
  namespace dt = core::datatypes;
  if (expected_type == dt::CanonicalTypeId::character) {
    const auto* identity = FindExactRelationalDatatypeIdentityV3(source);
    if (!identity || identity->legacy_fields.canonical_binary_type_code !=
            static_cast<std::uint32_t>(expected_type) ||
        (source.nullability != RelationalNullability::kNullable &&
         source.nullability != RelationalNullability::kNonNull) ||
        source.timezone_profile_id || source.precision || source.scale ||
        source.decimal_codec_profile) return false;
    EngineDescriptor prepared;
    prepared.descriptor_uuid = source.descriptor_uuid;
    prepared.descriptor_kind = "scalar";
    prepared.canonical_type_name = dt::CanonicalTypeName(expected_type);
    prepared.type_uuid = source.type_uuid;
    prepared.datatype_descriptor_uuid = identity->legacy_fields.descriptor_uuid;
    prepared.datatype_descriptor_generation = identity->legacy_fields.descriptor_generation;
    prepared.datatype_cohort = {source.datatype_catalog_snapshot_uuid,
        source.datatype_catalog_generation, source.datatype_registry_generation};
    CatalogColumnMetadata fields;
    fields.text["nullability"] = source.nullability == RelationalNullability::kNullable
        ? "nullable" : "non_null";
    if (source.width) fields.text["length"] = std::to_string(*source.width);
    if (source.collation_uuid) {
      prepared.collation_uuid = *source.collation_uuid;
      fields.identities["collation_uuid"] = *source.collation_uuid;
    }
    if (!EncodeCatalogColumnMetadata(fields, &prepared.encoded_descriptor)) return false;
    *output = std::move(prepared);
    return true;
  }
  if (expected_type != dt::CanonicalTypeId::uuid &&
      expected_type != dt::CanonicalTypeId::binary &&
      expected_type != dt::CanonicalTypeId::boolean &&
      expected_type != dt::CanonicalTypeId::int8 &&
      expected_type != dt::CanonicalTypeId::int16 &&
      expected_type != dt::CanonicalTypeId::int32 &&
      expected_type != dt::CanonicalTypeId::int64 &&
      expected_type != dt::CanonicalTypeId::int128 &&
      expected_type != dt::CanonicalTypeId::uint64 &&
      expected_type != dt::CanonicalTypeId::decimal &&
      expected_type != dt::CanonicalTypeId::real64) return false;
  const bool decimal = expected_type == dt::CanonicalTypeId::decimal;
  if (source.decimal_codec_profile &&
      (!decimal || !source.precision || !source.scale ||
       !dt::ExactDecimalCodecProfileMatches(source.decimal_codec_profile->uuid,
           source.decimal_codec_profile->generation, *source.precision, *source.scale))) return false;
  if ((source.nullability != RelationalNullability::kNonNull &&
       source.nullability != RelationalNullability::kNullable) ||
      source.collation_uuid || source.timezone_profile_id || source.width ||
      (decimal ? (!source.precision || !source.scale || *source.precision == 0 ||
                  *source.precision > (source.decimal_codec_profile ? 76u : 38u) ||
                  *source.scale > *source.precision)
               : (source.precision.has_value() || source.scale.has_value()))) return false;
  const auto* identity = FindExactRelationalDatatypeIdentityV3(source);
  if (!identity || identity->legacy_fields.canonical_binary_type_code !=
                       static_cast<std::uint32_t>(expected_type)) return false;
  const auto& row = identity->legacy_fields;
  EngineDescriptor descriptor;
  descriptor.descriptor_uuid = source.descriptor_uuid;
  descriptor.type_uuid = source.type_uuid;
  descriptor.datatype_descriptor_uuid = row.descriptor_uuid;
  descriptor.datatype_descriptor_generation = row.descriptor_generation;
  descriptor.datatype_cohort = {source.datatype_catalog_snapshot_uuid,
      source.datatype_catalog_generation, source.datatype_registry_generation};
  descriptor.descriptor_kind = "scalar";
  descriptor.canonical_type_name = dt::CanonicalTypeName(expected_type);
  descriptor.encoded_descriptor = source.nullability == RelationalNullability::kNullable
      ? "nullability=nullable" : "nullability=non_null";
  if (decimal) descriptor.encoded_descriptor += ";precision=" + std::to_string(*source.precision) +
                                               ";scale=" + std::to_string(*source.scale);
  if (source.decimal_codec_profile) {
    CatalogColumnMetadata fields;
    fields.text = {{"nullability", source.nullability == RelationalNullability::kNullable ? "nullable" : "non_null"},
                   {"precision", std::to_string(*source.precision)},
                   {"scale", std::to_string(*source.scale)},
                   {"decimal_codec_generation", std::to_string(source.decimal_codec_profile->generation)}};
    fields.identities.emplace("decimal_codec_uuid", source.decimal_codec_profile->uuid);
    if (!EncodeCatalogColumnMetadata(fields, &descriptor.encoded_descriptor)) return false;
  }
  static_assert(std::is_nothrow_move_assignable_v<EngineDescriptor>);
  *output = std::move(descriptor);
  return true;
}
}  // namespace scratchbird::engine::internal_api
