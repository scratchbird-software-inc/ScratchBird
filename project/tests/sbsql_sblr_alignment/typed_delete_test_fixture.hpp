// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "typed_delete_carrier_codec.hpp"
#include <algorithm>
#include <stdexcept>

namespace scratchbird::tests::delete_carrier {
namespace w = scratchbird::wire;
using namespace scratchbird::wire;
inline void Check(bool good, const char* message) {
  if (!good) throw std::runtime_error(message);
}
inline TypedUpdateUuid Uuid(unsigned value) {
  TypedUpdateUuid id{}; id[0] = 1; id[6] = 0x70; id[8] = 0x80;
  id[15] = static_cast<byte>(value); return id;
}
inline w::TypedDeleteDescriptorCarrier Descriptor() {
  w::TypedDeleteDescriptorCarrier v;
  v.descriptor_uuid = Uuid(1);
  v.authenticated_statement_receipt_uuid = Uuid(2);
  v.operation_uuid = Uuid(3);
  v.owning_transaction_uuid = Uuid(4);
  v.statement_snapshot_uuid = Uuid(5);
  v.catalog_snapshot_uuid = Uuid(6);
  v.security_context_uuid = Uuid(7);
  v.security_snapshot_uuid = Uuid(8);
  v.target_relation_uuid = Uuid(9);
  v.target_relation_occurrence_uuid = Uuid(10);
  v.predicate_expression_uuid = Uuid(11);
  v.row_policy_set_uuid = Uuid(12);
  v.constraint_set_uuid = Uuid(13);
  v.trigger_set_uuid = Uuid(14);
  v.deterministic_target_order_uuid = Uuid(15);
  v.resource_budget_uuid = Uuid(16);
  v.recovery_token_uuid = Uuid(17);
  v.builtin_operator_snapshot_uuid = Uuid(18);
  v.descriptor_generation = 1;
  v.structural_occurrence_id = 1;
  v.operation_generation = 1;
  v.owning_local_transaction_id = 1;
  v.catalog_generation = 1;
  v.datatype_registry_generation = 1;
  v.security_generation = 1;
  v.target_relation_generation = 1;
  v.target_relation_occurrence_generation = 1;
  v.predicate_expression_generation = 1;
  v.predicate_root_node_id = 1;
  v.row_policy_set_generation = 1;
  v.constraint_set_generation = 1;
  v.trigger_set_generation = 1;
  v.deterministic_target_order_generation = 1;
  v.resource_budget_generation = 1;
  v.recovery_generation = 1;
  v.executor_availability_generation = 1;
  v.builtin_operator_registry_generation = 1;
  v.predicate_vector_sha256.fill(1);
  v.row_policy_set_sha256.fill(2);
  v.ordered_constraint_set_sha256.fill(3);
  v.ordered_trigger_set_sha256.fill(4);
  v.predicate_node_count = 1;
  v.builtin_operator_snapshot_uuid = w::kTypedUpdateOperatorSnapshotUuid;
  return v;
}
constexpr TypedUpdateUuid kBigintDescriptorUuid{{
    0x01, 0x9d, 0x00, 0x00, 0x00, 0x00, 0x70, 0x00,
    0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0xd7, 0x11}};
constexpr TypedUpdateUuid kBigintTypeUuid{{
    0x01, 0x9d, 0x00, 0x00, 0x00, 0x00, 0x70, 0x00,
    0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0xd7, 0x12}};
constexpr TypedUpdateUuid kInt32DescriptorUuid{{
    0x01, 0x9d, 0x00, 0x00, 0x00, 0x00, 0x70, 0x00,
    0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0xd7, 0x16}};
constexpr TypedUpdateUuid kInt32TypeUuid{{
    0x01, 0x9d, 0x00, 0x00, 0x00, 0x00, 0x70, 0x00,
    0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0xd7, 0x17}};

inline TypedUpdateDatatypeAuthorityRecord BooleanDatatypeAuthority(u32 ordinal) {
  TypedUpdateDatatypeAuthorityRecord row;
  row.datatype_ordinal = ordinal;
  row.datatype_identity_code = TypedUpdateDatatypeIdentityCode::boolean_v1;
  row.null_encoding_code =
      TypedUpdateNullEncodingCode::containing_slot_value_or_null_state;
  row.byte_order_code = TypedUpdateByteOrderCode::single_byte;
  row.is_signed = false;
  row.descriptor_uuid = kTypedUpdateBooleanUuid;
  row.descriptor_generation = 1;
  row.type_uuid = kTypedUpdateBooleanUuid;
  row.type_generation = 1;
  row.codec_version = 1;
  row.canonical_name = "boolean";
  row.codec_id = "datatype.boolean.u8.v1";
  row.representation_code =
      TypedUpdateRepresentationCode::canonical_boolean;
  row.codec_generation = 1;
  row.canonical_value_minimum_bytes = 1;
  row.canonical_value_maximum_bytes = 1;
  row.canonical_value_exact_bytes = 1;
  row.datatype_snapshot_uuid = kTypedUpdateDatatypeSnapshotUuid;
  row.datatype_catalog_generation = 1;
  row.datatype_registry_generation = 1;
  return row;
}

inline TypedUpdateDatatypeAuthorityRecord BigintDatatypeAuthority(u32 ordinal) {
  TypedUpdateDatatypeAuthorityRecord row;
  row.datatype_ordinal = ordinal;
  row.datatype_identity_code = TypedUpdateDatatypeIdentityCode::bigint_v1;
  row.null_encoding_code =
      TypedUpdateNullEncodingCode::unsupported_in_sblr_literal_v1;
  row.byte_order_code = TypedUpdateByteOrderCode::little_endian;
  row.is_signed = true;
  row.descriptor_uuid = kBigintDescriptorUuid;
  row.descriptor_generation = 1;
  row.type_uuid = kBigintTypeUuid;
  row.type_generation = 1;
  row.codec_version = 1;
  row.canonical_name = "bigint";
  row.codec_id = "datatype.int64.le.v1";
  row.representation_code = TypedUpdateRepresentationCode::twos_complement;
  row.codec_generation = 1;
  row.canonical_value_minimum_bytes = 8;
  row.canonical_value_maximum_bytes = 8;
  row.canonical_value_exact_bytes = 8;
  row.datatype_snapshot_uuid = kTypedUpdateDatatypeSnapshotUuid;
  row.datatype_catalog_generation = 1;
  row.datatype_registry_generation = 1;
  return row;
}

inline TypedUpdateDatatypeAuthorityRecord Int32DatatypeAuthority(u32 ordinal) {
  auto row = BigintDatatypeAuthority(ordinal);
  row.datatype_identity_code = TypedUpdateDatatypeIdentityCode::int32_v1;
  row.descriptor_uuid = kInt32DescriptorUuid;
  row.type_uuid = kInt32TypeUuid;
  row.canonical_name = "int32";
  row.codec_id = "datatype.int32.le.v1";
  row.canonical_value_minimum_bytes = 4;
  row.canonical_value_maximum_bytes = 4;
  row.canonical_value_exact_bytes = 4;
  row.null_encoding_code =
      TypedUpdateNullEncodingCode::containing_slot_value_or_null_state;
  return row;
}


struct Fixture {
  TypedDeleteDescriptorCarrier descriptor;
  TypedUpdatePredicateVector predicate;
  TypedUpdateDatatypeAuthorityVector datatypes;
  TypedUpdateBuiltinOperatorAuthorityVector operators;
};
inline void Seal(Fixture& f) {
  TypedUpdateCarrierError shared;
  TypedDeleteCarrierError error;
  std::vector<byte> bytes;
  Check(EncodeTypedUpdatePredicateVector(f.predicate, &bytes, &shared), "predicate encode");
  Check(DecodeAndValidateTypedUpdatePredicateVector(bytes, &f.predicate, &shared), "predicate decode");
  std::copy_n(bytes.begin() + 72, 32, f.descriptor.predicate_vector_sha256.begin());
  Check(EncodeTypedDeleteDescriptor(f.descriptor, &bytes, &error), "DELETE encode");
  Check(DecodeAndValidateTypedDeleteDescriptor(bytes, &f.descriptor, &error), "DELETE decode");
  Check(EncodeTypedUpdateDatatypeAuthorityVector(f.datatypes, &bytes, &shared), "datatype encode");
  Check(DecodeAndValidateTypedUpdateDatatypeAuthorityVector(bytes, &f.datatypes, &shared), "datatype decode");
  Check(EncodeTypedUpdateBuiltinOperatorAuthorityVector(f.operators, &bytes, &shared), "operator encode");
  Check(DecodeAndValidateTypedUpdateBuiltinOperatorAuthorityVector(bytes, &f.operators, &shared), "operator decode");
}
// 0 = canonical TRUE; 1 = int32 equality; 2 = BIGINT equality.
inline Fixture Make(unsigned profile) {
  Fixture f;
  auto& d = f.descriptor;
  d = Descriptor();
  const auto identity = [&](TypedUpdateUuid id) {
    TypedUpdateVectorIdentity v;
    v.vector_uuid = id; v.vector_generation = 1;
    v.owner_descriptor_uuid = d.descriptor_uuid; v.owner_descriptor_generation = 1;
    return v;
  };
  f.predicate.identity = identity(d.predicate_expression_uuid);
  f.datatypes.identity = identity(kTypedUpdateDatatypeSnapshotUuid);
  f.operators.identity = identity(kTypedUpdateOperatorSnapshotUuid);
  const auto boolean = BooleanDatatypeAuthority(1);
  TypedUpdatePredicateRecord root;
  root.node_id = 1; root.node_occurrence_uuid = Uuid(40); root.node_occurrence_generation = 1;
  root.node_kind = TypedUpdatePredicateNodeKind::canonical_boolean_constant;
  root.value_state = TypedUpdateValueState::value;
  root.output_descriptor_uuid = root.output_type_uuid = kTypedUpdateBooleanUuid;
  root.output_descriptor_generation = root.output_type_generation = 1;
  root.output_codec_id = boolean.codec_id; root.output_codec_version = 1; root.output_codec_generation = 1;
  root.canonical_value = {1};
  f.datatypes.records.push_back(boolean);
  if (!profile) f.predicate.records = {root};
  else {
    const auto data = profile == 1 ? Int32DatatypeAuthority(2) : BigintDatatypeAuthority(2);
    f.datatypes.records.push_back(data);
    TypedUpdatePredicateRecord column;
    column.node_id = 1; column.node_occurrence_uuid = Uuid(41); column.node_occurrence_generation = 1;
    column.node_kind = TypedUpdatePredicateNodeKind::column_reference;
    column.output_descriptor_uuid = data.descriptor_uuid; column.output_descriptor_generation = 1;
    column.output_type_uuid = data.type_uuid; column.output_type_generation = 1;
    column.output_codec_id = data.codec_id; column.output_codec_version = 1; column.output_codec_generation = 1;
    column.referenced_relation_occurrence_uuid = d.target_relation_occurrence_uuid;
    column.referenced_relation_occurrence_generation = 1;
    column.referenced_column_occurrence_uuid = Uuid(42); column.referenced_column_occurrence_generation = 1;
    column.referenced_column_uuid = Uuid(43); column.referenced_column_generation = 1;
    auto literal = column;
    literal.node_id = 2; literal.node_occurrence_uuid = Uuid(44);
    literal.node_kind = TypedUpdatePredicateNodeKind::typed_literal;
    literal.referenced_relation_occurrence_uuid = {}; literal.referenced_relation_occurrence_generation = 0;
    literal.referenced_column_occurrence_uuid = {}; literal.referenced_column_occurrence_generation = 0;
    literal.referenced_column_uuid = {}; literal.referenced_column_generation = 0;
    literal.value_state = TypedUpdateValueState::value;
    literal.canonical_value.resize(data.canonical_value_exact_bytes);
    literal.canonical_value[0] = 42;
    root.node_id = 3; root.node_kind = TypedUpdatePredicateNodeKind::comparison;
    root.value_state = TypedUpdateValueState::absent; root.canonical_value.clear();
    root.left_child_node_id = 1; root.right_child_node_id = 2;
    root.operator_uuid = kTypedUpdateEqualOperatorUuid; root.operator_generation = 1;
    f.predicate.records = {column, literal, root};
    d.predicate_node_count = 3; d.predicate_root_node_id = 3;
    TypedUpdateBuiltinOperatorAuthorityRecord op;
    op.operator_ordinal = 1; op.operator_uuid = kTypedUpdateEqualOperatorUuid; op.operator_generation = 1;
    op.operator_snapshot_uuid = kTypedUpdateOperatorSnapshotUuid; op.operator_registry_generation = 1;
    op.left_descriptor_uuid = op.right_descriptor_uuid = data.descriptor_uuid;
    op.left_descriptor_generation = op.right_descriptor_generation = 1;
    op.left_type_uuid = op.right_type_uuid = data.type_uuid;
    op.left_type_generation = op.right_type_generation = 1;
    op.result_descriptor_uuid = op.result_type_uuid = kTypedUpdateBooleanUuid;
    op.result_descriptor_generation = op.result_type_generation = 1;
    op.result_codec_version = 1; op.result_codec_generation = 1; op.result_codec_id = boolean.codec_id;
    f.operators.records.push_back(op);
  }
  Seal(f); return f;
}
}  // namespace scratchbird::tests::delete_carrier
