// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "api_types.hpp"
#include "typed_update_carrier_codec.hpp"
#include "datatype_catalog_manifest.hpp"
#include "uuid.hpp"
#include <algorithm>

namespace scratchbird::engine::internal_api::datatype_operator_projection {
namespace update_wire = scratchbird::wire;
namespace datatype_catalog = scratchbird::core::datatypes;

// Pure live-registry projection only. This supplies neither a statement
// capability nor mutation/security authority. Each DML provider separately
// authenticates its operation, receipt and descriptor and closes its profile.
inline std::string UuidText(const update_wire::TypedUpdateUuid& uuid) {
  scratchbird::core::platform::Uuid value;
  std::copy(uuid.begin(), uuid.end(), value.bytes.begin());
  if (scratchbird::core::uuid::IsNilUuid(value)) return {};
  return scratchbird::core::uuid::UuidToString(value);
}
inline bool TypedUuid(std::string_view text, update_wire::TypedUpdateUuid* out) {
  if (!out) return false;
  const auto parsed = scratchbird::core::uuid::ParseUuid(std::string(text));
  if (!parsed.ok() || scratchbird::core::uuid::IsNilUuid(parsed.value) ||
      scratchbird::core::uuid::UuidToString(parsed.value) != text) return false;
  std::copy(parsed.value.bytes.begin(), parsed.value.bytes.end(), out->begin());
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
  const auto lookup = datatype_catalog::LookupDatatypeTypeCodecIdentityV1(
      context.datatype_catalog_snapshot_uuid,
      context.datatype_catalog_generation,
      context.datatype_registry_generation,
      UuidText(reference.descriptor_uuid), reference.descriptor_generation);
  if (!lookup.ok || lookup.row.type_uuid != UuidText(reference.type_uuid) ||
      lookup.row.type_generation != reference.type_generation ||
      lookup.row.codec_id != reference.codec_id ||
      lookup.row.codec_version != reference.codec_version ||
      lookup.row.codec_generation != reference.codec_generation ||
      !TypedUuid(lookup.row.descriptor_uuid, &record->descriptor_uuid) ||
      !TypedUuid(lookup.row.type_uuid, &record->type_uuid) ||
      !TypedUuid(lookup.row.catalog_snapshot_uuid,
                 &record->datatype_snapshot_uuid)) {
    return false;
  }
  // SBLR-DML-UPDATE-ROWS-DATATYPE-AUTHORITY-V2 projects the existing exact
  // TEXT registry identity. Its v1 closed-carrier code fields stay zero;
  // neither a codec spelling nor variable width alone admits another type.
  const auto& row = lookup.row;
  const bool text =
      row.descriptor_uuid == "019d0000-0000-7000-8000-00000000d718" &&
      row.descriptor_generation == 1 &&
      row.type_uuid == "019d0000-0000-7000-8000-00000000d719" &&
      row.type_generation == 1 &&
      row.codec_uuid == "019d0000-0000-7000-8000-00000000d71a" &&
      row.codec_id == "datatype.text.utf8.v1" && row.codec_version == 1 &&
      row.codec_generation == 1 && row.canonical_name == "text" && row.null_supported &&
      row.datatype_identity_code == 0 && row.null_encoding_code == 1 &&
      row.byte_order_code == 0 && !row.signed_code &&
      row.representation_code == 0 && row.canonical_value_bytes == 0 &&
      row.canonical_value_minimum_bytes == 0 &&
      row.canonical_value_maximum_bytes == 16777216 &&
      row.canonical_value_exact_bytes == 0 &&
      row.canonical_value_variable_width &&
      row.canonical_value_exact_zero_is_width_marker &&
      row.canonical_byte_order == "byte_sequence" &&
      row.canonical_representation ==
          "exact_well_formed_UTF8_scalar_sequence_without_implicit_normalization" &&
      row.canonical_charset == "UTF-8" && row.shortest_form_utf8_required &&
      !row.implicit_normalization_allowed && row.descriptor_bound_collation_required &&
      row.empty_value_distinct_from_sql_null && row.sql_null_requires_zero_payload &&
      row.variable_width_storage_without_truncation &&
      row.invalid_encoding_diagnostic_id == "CTB.TEXT.INVALID_ENCODING";
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
          lookup.row.datatype_identity_code);
  record->null_encoding_code =
      static_cast<update_wire::TypedUpdateNullEncodingCode>(
          lookup.row.null_encoding_code);
  record->byte_order_code =
      text ? update_wire::TypedUpdateByteOrderCode::byte_sequence
           : static_cast<update_wire::TypedUpdateByteOrderCode>(
          lookup.row.byte_order_code);
  record->is_signed = lookup.row.signed_code;
  record->descriptor_generation = lookup.row.descriptor_generation;
  record->type_generation = lookup.row.type_generation;
  record->codec_version = lookup.row.codec_version;
  record->canonical_name = lookup.row.canonical_name;
  record->codec_id = lookup.row.codec_id;
  record->representation_code =
      text ? update_wire::TypedUpdateRepresentationCode::utf8_scalar_sequence
           : static_cast<update_wire::TypedUpdateRepresentationCode>(
          lookup.row.representation_code);
  record->codec_generation = lookup.row.codec_generation;
  record->canonical_value_minimum_bytes =
      lookup.row.canonical_value_minimum_bytes;
  record->canonical_value_maximum_bytes =
      lookup.row.canonical_value_maximum_bytes;
  record->canonical_value_exact_bytes =
      lookup.row.canonical_value_exact_bytes;
  record->datatype_catalog_generation = lookup.row.catalog_generation;
  record->datatype_registry_generation = lookup.row.registry_generation;
  return true;
}

template<class Descriptor>
inline bool BuildOperatorRecord(
    const Descriptor& descriptor,
    const update_wire::TypedUpdatePredicateVector& predicate,
    update_wire::TypedUpdateBuiltinOperatorAuthorityRecord* record) {
  if (record == nullptr || predicate.records.size() != 3) return false;
  const auto& left = predicate.records[0];
  const auto& right = predicate.records[1];
  const auto& comparison = predicate.records[2];
  const auto lookup =
      datatype_catalog::LookupBuiltinOperatorTypeCodecIdentityV1(
          UuidText(descriptor.builtin_operator_snapshot_uuid),
          descriptor.builtin_operator_registry_generation,
          UuidText(comparison.operator_uuid), comparison.operator_generation,
          UuidText(left.output_descriptor_uuid),
          left.output_descriptor_generation, UuidText(left.output_type_uuid),
          left.output_type_generation, UuidText(right.output_descriptor_uuid),
          right.output_descriptor_generation,
          UuidText(right.output_type_uuid), right.output_type_generation);
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
  return true;
}
}  // namespace scratchbird::engine::internal_api::datatype_operator_projection
