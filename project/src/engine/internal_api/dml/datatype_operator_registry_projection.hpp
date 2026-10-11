// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "api_types.hpp"
#include "typed_update_carrier_codec.hpp"
#include "datatype_catalog_manifest.hpp"
#include "datatype_type_codec_identity_v3.hpp"
#include "uuid.hpp"
#include <algorithm>

namespace scratchbird::engine::internal_api::datatype_operator_projection {
namespace update_wire = scratchbird::wire;
namespace datatype_catalog = scratchbird::core::datatypes;

// The operation's admitted receipt selects the full immutable V3 row. DUDR's
// fixed legacy fields are only a lossless wire projection, not a fallback to
// the frozen V1 registry. New widths/profiles require their own carrier contract.
inline const datatype_catalog::DatatypeTypeCodecIdentityRowV1* FindDatatypeProjection(
    const EngineRequestContext& context, const EngineUuid& descriptor, std::uint64_t generation) noexcept {
  const auto* selected = datatype_catalog::FindDatatypeTypeCodecIdentityV3(
      context.datatype_catalog_snapshot_uuid, context.datatype_catalog_generation,
      context.datatype_registry_generation, descriptor, generation);
  if (!selected) return nullptr;
  const auto& row = selected->legacy_fields;
  const auto& native = selected->native_fields;
  if (native.present &&
      (native.canonical_value_minimum_bytes != row.canonical_value_minimum_bytes ||
       native.canonical_value_maximum_bytes != row.canonical_value_maximum_bytes ||
       native.canonical_value_transport_width != row.canonical_value_exact_bytes ||
       native.canonical_value_variable_width != row.canonical_value_variable_width ||
       !native.policy_profile_uuid.is_nil() || native.policy_profile_generation != 0 ||
       std::any_of(native.profile_fingerprint_sha256.begin(), native.profile_fingerprint_sha256.end(),
                   [](auto byte) { return byte != 0; }))) return nullptr;
  return &row;
}

// Pure live-registry projection only. This supplies neither a statement
// capability nor mutation/security authority. Each DML provider separately
// authenticates its operation, receipt and descriptor and closes its profile.
inline EngineUuid UuidValue(const update_wire::TypedUpdateUuid& uuid) noexcept {
  return EngineUuid{uuid};
}
inline bool TypedUuid(const EngineUuid& uuid, update_wire::TypedUpdateUuid* out) noexcept {
  if (!out || !core::uuid::IsEngineIdentityUuid(uuid)) return false;
  *out = uuid.bytes;
  return true;
}
struct DatatypeReference {
  update_wire::TypedUpdateUuid descriptor_uuid{};
  std::uint64_t descriptor_generation = 0;
  update_wire::TypedUpdateUuid type_uuid{};
  std::uint64_t type_generation = 0;
  std::string codec_id;
  std::uint16_t codec_version = 0;
  std::uint64_t codec_generation = 0;

  bool operator==(const DatatypeReference&) const = default;
};

inline void AddReference(const DatatypeReference& reference,
                  std::vector<DatatypeReference>* references) {
  if (std::find(references->begin(), references->end(), reference) ==
      references->end()) {
    references->push_back(reference);
  }
}

inline bool DatatypeRowLess(
    const update_wire::TypedUpdateDatatypeAuthorityRecord& left,
    const update_wire::TypedUpdateDatatypeAuthorityRecord& right) {
  if (left.descriptor_uuid != right.descriptor_uuid) {
    return left.descriptor_uuid < right.descriptor_uuid;
  }
  if (left.descriptor_generation != right.descriptor_generation) {
    return left.descriptor_generation < right.descriptor_generation;
  }
  return left.type_uuid < right.type_uuid;
}

inline bool BuildDatatypeRecord(
    const EngineRequestContext& context,
    const DatatypeReference& reference,
    update_wire::TypedUpdateDatatypeAuthorityRecord* record) {
  if (record == nullptr) return false;
  update_wire::TypedUpdateDatatypeAuthorityRecord candidate;
  auto* output = record;
  record = &candidate;
  const auto* selected = FindDatatypeProjection(context,
      UuidValue(reference.descriptor_uuid), reference.descriptor_generation);
  if (!selected || selected->type_uuid != UuidValue(reference.type_uuid) ||
      selected->type_generation != reference.type_generation ||
      selected->codec_id != reference.codec_id ||
      selected->codec_version != reference.codec_version ||
      selected->codec_generation != reference.codec_generation ||
      !TypedUuid(selected->descriptor_uuid, &record->descriptor_uuid) ||
      !TypedUuid(selected->type_uuid, &record->type_uuid) ||
      !TypedUuid(selected->catalog_snapshot_uuid,
                 &record->datatype_snapshot_uuid)) {
    return false;
  }
  // SBLR-DML-UPDATE-ROWS-DATATYPE-AUTHORITY-V2 projects the existing exact
  // TEXT registry identity. Its v1 closed-carrier code fields stay zero;
  // neither a codec spelling nor variable width alone admits another type.
  const auto& row = *selected;
  const bool text = datatype_catalog::IsExactCanonicalTextTypeCodecIdentityV1(row);
  if (!text &&
      (row.datatype_identity_code == 0 || row.null_encoding_code == 0 ||
       row.byte_order_code == 0 || row.representation_code == 0 ||
       row.canonical_name.empty() || row.canonical_value_minimum_bytes == 0 ||
       row.canonical_value_minimum_bytes != row.canonical_value_maximum_bytes ||
       row.canonical_value_minimum_bytes != row.canonical_value_exact_bytes ||
       row.canonical_value_exact_bytes != row.canonical_value_bytes)) {
    return false;
  }
  record->datatype_identity_code =
      text ? update_wire::TypedUpdateDatatypeIdentityCode::text_v2
           : static_cast<update_wire::TypedUpdateDatatypeIdentityCode>(
          row.datatype_identity_code);
  record->null_encoding_code =
      static_cast<update_wire::TypedUpdateNullEncodingCode>(
          row.null_encoding_code);
  record->byte_order_code =
      text ? update_wire::TypedUpdateByteOrderCode::byte_sequence
           : static_cast<update_wire::TypedUpdateByteOrderCode>(
          row.byte_order_code);
  record->is_signed = row.signed_code;
  record->descriptor_generation = row.descriptor_generation;
  record->type_generation = row.type_generation;
  record->codec_version = row.codec_version;
  record->canonical_name = row.canonical_name;
  record->codec_id = row.codec_id;
  record->representation_code =
      text ? update_wire::TypedUpdateRepresentationCode::utf8_scalar_sequence
           : static_cast<update_wire::TypedUpdateRepresentationCode>(
          row.representation_code);
  record->codec_generation = row.codec_generation;
  record->canonical_value_minimum_bytes =
      row.canonical_value_minimum_bytes;
  record->canonical_value_maximum_bytes =
      row.canonical_value_maximum_bytes;
  record->canonical_value_exact_bytes =
      row.canonical_value_exact_bytes;
  record->datatype_catalog_generation = row.catalog_generation;
  record->datatype_registry_generation = row.registry_generation;
  *output = std::move(candidate);
  return true;
}

template<class Descriptor>
inline bool BuildOperatorRecord(
    const Descriptor& descriptor,
    const update_wire::TypedUpdatePredicateVector& predicate,
    update_wire::TypedUpdateBuiltinOperatorAuthorityRecord* record) {
  if (record == nullptr || predicate.records.size() != 3) return false;
  update_wire::TypedUpdateBuiltinOperatorAuthorityRecord candidate;
  auto* output = record;
  record = &candidate;
  const auto& left = predicate.records[0];
  const auto& right = predicate.records[1];
  const auto& comparison = predicate.records[2];
  const auto lookup =
      datatype_catalog::LookupBuiltinOperatorTypeCodecIdentityV1(
          UuidValue(descriptor.builtin_operator_snapshot_uuid),
          descriptor.builtin_operator_registry_generation,
          UuidValue(comparison.operator_uuid), comparison.operator_generation,
          UuidValue(left.output_descriptor_uuid),
          left.output_descriptor_generation, UuidValue(left.output_type_uuid),
          left.output_type_generation, UuidValue(right.output_descriptor_uuid),
          right.output_descriptor_generation,
          UuidValue(right.output_type_uuid), right.output_type_generation);
  if (!lookup.ok ||
      !TypedUuid(lookup.row.operator_uuid, &record->operator_uuid) ||
      !TypedUuid(lookup.row.operator_snapshot_uuid,
                 &record->operator_snapshot_uuid) ||
      !TypedUuid(lookup.row.left_descriptor_uuid,
                 &record->left_descriptor_uuid) ||
      !TypedUuid(lookup.row.left_type_uuid, &record->left_type_uuid) ||
      !TypedUuid(lookup.row.right_descriptor_uuid,
                 &record->right_descriptor_uuid) ||
      !TypedUuid(lookup.row.right_type_uuid, &record->right_type_uuid) ||
      !TypedUuid(lookup.row.result_descriptor_uuid,
                 &record->result_descriptor_uuid) ||
      !TypedUuid(lookup.row.result_type_uuid, &record->result_type_uuid)) {
    return false;
  }
  record->operator_ordinal = 1;
  record->semantic_code = lookup.row.semantic_code;
  record->operand_arity = lookup.row.operand_arity;
  record->null_behavior_code = lookup.row.null_behavior_code;
  record->accepted_state = lookup.row.accepted_state;
  record->operator_generation = lookup.row.operator_generation;
  record->operator_registry_generation =
      lookup.row.operator_registry_generation;
  record->left_descriptor_generation =
      lookup.row.left_descriptor_generation;
  record->left_type_generation = lookup.row.left_type_generation;
  record->right_descriptor_generation =
      lookup.row.right_descriptor_generation;
  record->right_type_generation = lookup.row.right_type_generation;
  record->result_descriptor_generation =
      lookup.row.result_descriptor_generation;
  record->result_type_generation = lookup.row.result_type_generation;
  record->result_codec_version = lookup.row.result_codec_version;
  record->operator_family_code = lookup.row.operator_family_code;
  record->result_codec_generation = lookup.row.result_codec_generation;
  record->result_codec_id = lookup.row.result_codec_id;
  record->operand_identity_rule = lookup.row.operand_identity_rule;
  record->result_null_encoding_code =
      lookup.row.result_null_encoding_code;
  *output = std::move(candidate);
  return true;
}
}  // namespace scratchbird::engine::internal_api::datatype_operator_projection
