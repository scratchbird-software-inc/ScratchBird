// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#include "core/hash/hash_digest.hpp"
#include "wire/typed_update_carrier_codec.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace scratchbird::wire;
using scratchbird::core::platform::LoadLittle16;
using scratchbird::core::platform::LoadLittle32;
using scratchbird::core::platform::StoreLittle32;
using scratchbird::core::platform::StoreLittle64;

[[noreturn]] void Die(const std::string& message) {
  std::cerr << "FAIL: " << message << '\n';
  std::exit(1);
}

void Require(bool condition, const std::string& message) {
  if (!condition) {
    Die(message);
  }
}

TypedUpdateUuid Uuid(unsigned seed) {
  TypedUpdateUuid value{};
  for (std::size_t index = 0; index < value.size(); ++index) {
    value[index] = static_cast<byte>((seed * 37u + index * 19u) & 0xffu);
  }
  value[0] |= 1u;
  return value;
}

TypedUpdateHash SeedHash(unsigned seed) {
  TypedUpdateHash value{};
  for (std::size_t index = 0; index < value.size(); ++index) {
    value[index] = static_cast<byte>((seed * 23u + index * 11u) & 0xffu);
  }
  value[0] |= 1u;
  return value;
}

TypedUpdateHash ReadHash(std::span<const byte> bytes, std::size_t offset) {
  TypedUpdateHash value{};
  std::copy_n(bytes.begin() + offset, value.size(), value.begin());
  return value;
}

void WriteHash(std::vector<byte>* bytes,
               std::size_t offset,
               const TypedUpdateHash& value) {
  std::copy(value.begin(), value.end(), bytes->begin() + offset);
}

TypedUpdateHash Hash(std::span<const byte> material) {
  const auto result = scratchbird::core::hash::ComputeSha256Digest(
      material.data(), material.size());
  Require(result.ok(), "independent SHA-256 helper failed");
  TypedUpdateHash value{};
  std::copy(result.digest.begin(), result.digest.end(), value.begin());
  return value;
}

TypedUpdateHash Evidence(std::string_view domain,
                         std::span<const byte> material) {
  std::vector<byte> input;
  input.reserve(domain.size() + material.size());
  for (char character : domain) {
    input.push_back(static_cast<byte>(character));
  }
  input.insert(input.end(), material.begin(), material.end());
  return Hash(input);
}

void RewriteVectorEvidence(std::vector<byte>* bytes,
                           std::string_view domain) {
  WriteHash(bytes, 72,
            Evidence(domain,
                     std::span<const byte>(*bytes).subspan(
                         kTypedUpdateVectorHeaderBytes)));
}

void RewriteJournalEvidence(std::vector<byte>* bytes) {
  std::vector<byte> material;
  material.insert(material.end(), bytes->begin(), bytes->begin() + 224);
  material.insert(material.end(),
                  bytes->begin() + kTypedUpdateJournalHeaderBytes,
                  bytes->end());
  WriteHash(bytes, 224,
            Evidence("ScratchBird.SblrDmlUpdateRowsDurableJournal.V1",
                     material));
}

void AppendUuid(std::vector<byte>* bytes, const TypedUpdateUuid& uuid) {
  bytes->insert(bytes->end(), uuid.begin(), uuid.end());
}

void AppendLittle64(std::vector<byte>* bytes, u64 value) {
  const auto offset = bytes->size();
  bytes->resize(offset + 8);
  StoreLittle64(bytes->data() + offset, value);
}

TypedUpdateVectorIdentity VectorIdentity(
    unsigned seed,
    const TypedUpdateDescriptorCarrier& descriptor) {
  TypedUpdateVectorIdentity identity;
  identity.vector_uuid = Uuid(seed);
  identity.vector_generation = seed + 1;
  identity.owner_descriptor_uuid = descriptor.descriptor_uuid;
  identity.owner_descriptor_generation = descriptor.descriptor_generation;
  return identity;
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

TypedUpdateDatatypeAuthorityRecord BooleanDatatypeAuthority(u32 ordinal) {
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

TypedUpdateDatatypeAuthorityRecord BigintDatatypeAuthority(u32 ordinal) {
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

TypedUpdateDatatypeAuthorityRecord Int32DatatypeAuthority(u32 ordinal) {
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

TypedUpdateAssignmentRecord Assignment(unsigned ordinal,
                                       unsigned seed,
                                       std::vector<byte> value) {
  TypedUpdateAssignmentRecord row;
  row.assignment_ordinal = ordinal;
  row.assignment_occurrence_uuid = Uuid(seed);
  row.assignment_occurrence_generation = seed + 1;
  row.target_column_occurrence_uuid = Uuid(seed + 1);
  row.target_column_occurrence_generation = seed + 2;
  row.target_column_uuid = Uuid(seed + 2);
  row.target_column_generation = seed + 3;
  row.value_descriptor_uuid = Uuid(seed + 3);
  row.value_descriptor_generation = seed + 4;
  row.value_type_uuid = Uuid(seed + 4);
  row.value_type_generation = seed + 5;
  row.codec_id = "datatype.int64.le.v1";
  row.codec_version = 1;
  row.codec_generation = 1;
  row.value_state = TypedUpdateValueState::value;
  row.canonical_value = std::move(value);
  return row;
}

TypedUpdatePredicateRecord TrueNode() {
  TypedUpdatePredicateRecord row;
  row.node_id = 1;
  row.node_occurrence_uuid = Uuid(40);
  row.node_occurrence_generation = 1;
  row.node_kind = TypedUpdatePredicateNodeKind::canonical_boolean_constant;
  row.value_state = TypedUpdateValueState::value;
  row.output_descriptor_uuid = kTypedUpdateBooleanUuid;
  row.output_descriptor_generation = 1;
  row.output_type_uuid = kTypedUpdateBooleanUuid;
  row.output_type_generation = 1;
  row.output_codec_id = "datatype.boolean.u8.v1";
  row.output_codec_version = 1;
  row.output_codec_generation = 1;
  row.canonical_value = {1};
  return row;
}

TypedUpdatePredicateVector EqualsPredicate(
    const TypedUpdateDescriptorCarrier& descriptor,
    const TypedUpdateVectorIdentity& identity,
    const TypedUpdateAssignmentRecord& assignment) {
  TypedUpdatePredicateVector vector;
  vector.identity = identity;
  TypedUpdatePredicateRecord column;
  column.node_id = 1;
  column.node_occurrence_uuid = Uuid(41);
  column.node_occurrence_generation = 2;
  column.node_kind = TypedUpdatePredicateNodeKind::column_reference;
  column.output_descriptor_uuid = assignment.value_descriptor_uuid;
  column.output_descriptor_generation = assignment.value_descriptor_generation;
  column.output_type_uuid = assignment.value_type_uuid;
  column.output_type_generation = assignment.value_type_generation;
  column.output_codec_id = assignment.codec_id;
  column.output_codec_version = assignment.codec_version;
  column.output_codec_generation = assignment.codec_generation;
  column.referenced_relation_occurrence_uuid =
      descriptor.target_relation_occurrence_uuid;
  column.referenced_relation_occurrence_generation =
      descriptor.target_relation_occurrence_generation;
  column.referenced_column_occurrence_uuid =
      assignment.target_column_occurrence_uuid;
  column.referenced_column_occurrence_generation =
      assignment.target_column_occurrence_generation;
  column.referenced_column_uuid = assignment.target_column_uuid;
  column.referenced_column_generation = assignment.target_column_generation;

  auto literal = column;
  literal.node_id = 2;
  literal.node_occurrence_uuid = Uuid(42);
  literal.node_occurrence_generation = 3;
  literal.node_kind = TypedUpdatePredicateNodeKind::typed_literal;
  literal.referenced_relation_occurrence_uuid = {};
  literal.referenced_relation_occurrence_generation = 0;
  literal.referenced_column_occurrence_uuid = {};
  literal.referenced_column_occurrence_generation = 0;
  literal.referenced_column_uuid = {};
  literal.referenced_column_generation = 0;
  literal.value_state = TypedUpdateValueState::value;
  literal.canonical_value = {0x2a};

  TypedUpdatePredicateRecord comparison;
  comparison.node_id = 3;
  comparison.node_occurrence_uuid = Uuid(43);
  comparison.node_occurrence_generation = 4;
  comparison.node_kind = TypedUpdatePredicateNodeKind::comparison;
  comparison.output_descriptor_uuid = kTypedUpdateBooleanUuid;
  comparison.output_descriptor_generation = 1;
  comparison.output_type_uuid = kTypedUpdateBooleanUuid;
  comparison.output_type_generation = 1;
  comparison.output_codec_id = "datatype.boolean.u8.v1";
  comparison.output_codec_version = 1;
  comparison.output_codec_generation = 1;
  comparison.left_child_node_id = 1;
  comparison.right_child_node_id = 2;
  comparison.operator_uuid = kTypedUpdateEqualOperatorUuid;
  comparison.operator_generation = 1;
  vector.records = {column, literal, comparison};
  return vector;
}

TypedUpdateRowPolicyRecord Policy() {
  TypedUpdateRowPolicyRecord row;
  row.policy_ordinal = 1;
  row.phase = 1;
  row.effective_policy_uuid = Uuid(50);
  row.effective_policy_generation = 1;
  row.expression_uuid = Uuid(51);
  row.expression_generation = 2;
  row.expression_evidence_sha256 = SeedHash(52);
  row.security_snapshot_uuid = Uuid(8);
  row.security_generation = 9;
  row.source_policy_catalog_vector_sha256 = SeedHash(53);
  return row;
}

TypedUpdateConstraintRecord Constraint() {
  TypedUpdateConstraintRecord row;
  row.constraint_ordinal = 1;
  row.constraint_class = 3;
  row.timing = 2;
  row.reservation_mode = 1;
  row.constraint_uuid = Uuid(60);
  row.constraint_generation = 2;
  row.reservation_profile_uuid = Uuid(61);
  row.reservation_profile_generation = 3;
  row.dependency_set_sha256 = SeedHash(62);
  return row;
}

TypedUpdateTriggerRecord Trigger() {
  TypedUpdateTriggerRecord row;
  row.trigger_ordinal = 1;
  row.timing = 2;
  row.security_mode = 1;
  row.trigger_uuid = Uuid(70);
  row.trigger_generation = 2;
  row.body_sblr_uuid = Uuid(71);
  row.body_sblr_generation = 3;
  row.execution_security_context_uuid = Uuid(72);
  row.execution_security_generation = 4;
  row.recursion_profile_uuid = Uuid(73);
  row.recursion_profile_generation = 5;
  row.maximum_depth = 8;
  row.dependency_set_sha256 = SeedHash(74);
  return row;
}

TypedUpdateCarrierSet CarrierSet() {
  TypedUpdateCarrierSet set;
  auto& descriptor = set.descriptor;
  descriptor.descriptor_uuid = Uuid(1);
  descriptor.descriptor_generation = 2;
  descriptor.authenticated_statement_receipt_uuid = Uuid(2);
  descriptor.structural_occurrence_id = 3;
  descriptor.operation_uuid = Uuid(3);
  descriptor.operation_generation = 4;
  descriptor.owning_transaction_uuid = Uuid(4);
  descriptor.owning_local_transaction_id = 5;
  descriptor.statement_snapshot_uuid = Uuid(5);
  descriptor.catalog_snapshot_uuid = Uuid(6);
  descriptor.catalog_generation = 7;
  descriptor.datatype_registry_generation = 1;
  descriptor.security_context_uuid = Uuid(7);
  descriptor.security_snapshot_uuid = Uuid(8);
  descriptor.security_generation = 9;
  descriptor.target_relation_uuid = Uuid(9);
  descriptor.target_relation_generation = 10;
  descriptor.target_relation_occurrence_uuid = Uuid(10);
  descriptor.target_relation_occurrence_generation = 11;

  set.assignments.identity = VectorIdentity(20, descriptor);
  set.assignments.records.push_back(
      Assignment(1, 80, {0x00, 0x3b, 0x3d, 0xff}));
  set.predicate.identity = VectorIdentity(21, descriptor);
  set.predicate.records.push_back(TrueNode());
  set.row_policies.identity = VectorIdentity(22, descriptor);
  set.row_policies.records.push_back(Policy());
  set.constraints.identity = VectorIdentity(23, descriptor);
  set.constraints.records.push_back(Constraint());
  set.triggers.identity = VectorIdentity(24, descriptor);
  set.triggers.records.push_back(Trigger());

  std::vector<byte> bytes;
  TypedUpdateCarrierError error;
  Require(EncodeTypedUpdateAssignmentVector(set.assignments, &bytes, &error),
          "fixture DUAV encode");
  descriptor.assignment_vector_uuid = set.assignments.identity.vector_uuid;
  descriptor.assignment_vector_generation =
      set.assignments.identity.vector_generation;
  descriptor.assignment_count = 1;
  descriptor.assignment_vector_sha256 = ReadHash(bytes, 72);
  Require(EncodeTypedUpdatePredicateVector(set.predicate, &bytes, &error),
          "fixture DUEV encode");
  descriptor.predicate_expression_uuid = set.predicate.identity.vector_uuid;
  descriptor.predicate_expression_generation =
      set.predicate.identity.vector_generation;
  descriptor.predicate_root_node_id = 1;
  descriptor.predicate_node_count = 1;
  descriptor.predicate_vector_sha256 = ReadHash(bytes, 72);
  Require(EncodeTypedUpdateRowPolicyVector(set.row_policies, &bytes, &error),
          "fixture DUPV encode");
  descriptor.row_policy_set_uuid = set.row_policies.identity.vector_uuid;
  descriptor.row_policy_set_generation =
      set.row_policies.identity.vector_generation;
  descriptor.row_policy_count = 1;
  descriptor.row_policy_set_sha256 = ReadHash(bytes, 72);
  Require(EncodeTypedUpdateConstraintVector(set.constraints, &bytes, &error),
          "fixture DUCV encode");
  descriptor.constraint_set_uuid = set.constraints.identity.vector_uuid;
  descriptor.constraint_set_generation =
      set.constraints.identity.vector_generation;
  descriptor.constraint_count = 1;
  descriptor.ordered_constraint_set_sha256 = ReadHash(bytes, 72);
  Require(EncodeTypedUpdateTriggerVector(set.triggers, &bytes, &error),
          "fixture DUTV encode");
  descriptor.trigger_set_uuid = set.triggers.identity.vector_uuid;
  descriptor.trigger_set_generation = set.triggers.identity.vector_generation;
  descriptor.trigger_count = 1;
  descriptor.ordered_trigger_set_sha256 = ReadHash(bytes, 72);

  set.target_order.target_order_uuid = Uuid(30);
  set.target_order.target_order_generation = 31;
  set.target_order.authenticated_statement_receipt_uuid =
      descriptor.authenticated_statement_receipt_uuid;
  set.target_order.target_relation_occurrence_uuid =
      descriptor.target_relation_occurrence_uuid;
  set.target_order.target_relation_occurrence_generation =
      descriptor.target_relation_occurrence_generation;
  set.target_order.statement_snapshot_uuid = descriptor.statement_snapshot_uuid;
  set.target_order.maximum_candidate_rows = 100;
  descriptor.deterministic_target_order_uuid =
      set.target_order.target_order_uuid;
  descriptor.deterministic_target_order_generation =
      set.target_order.target_order_generation;

  set.resource_budget.resource_budget_uuid = Uuid(31);
  set.resource_budget.resource_budget_generation = 32;
  set.resource_budget.authenticated_statement_receipt_uuid =
      descriptor.authenticated_statement_receipt_uuid;
  set.resource_budget.owning_transaction_uuid =
      descriptor.owning_transaction_uuid;
  set.resource_budget.cancellation_token_uuid = Uuid(32);
  set.resource_budget.cancellation_generation = 33;
  set.resource_budget.grant_receipt_uuid = Uuid(33);
  set.resource_budget.grant_receipt_generation = 34;
  set.resource_budget.maximum_assignments = 16;
  set.resource_budget.maximum_predicate_nodes = 16;
  set.resource_budget.maximum_candidate_rows = 100;
  set.resource_budget.maximum_trigger_depth = 16;
  set.resource_budget.maximum_effects = 100;
  set.resource_budget.maximum_total_canonical_value_bytes = 1024;
  descriptor.resource_budget_uuid = set.resource_budget.resource_budget_uuid;
  descriptor.resource_budget_generation =
      set.resource_budget.resource_budget_generation;

  set.recovery_token.recovery_token_uuid = Uuid(34);
  set.recovery_token.recovery_generation = 35;
  set.recovery_token.authenticated_statement_receipt_uuid =
      descriptor.authenticated_statement_receipt_uuid;
  set.recovery_token.owning_transaction_uuid =
      descriptor.owning_transaction_uuid;
  set.recovery_token.operation_uuid = descriptor.operation_uuid;
  set.recovery_token.descriptor_uuid = descriptor.descriptor_uuid;
  set.recovery_token.descriptor_generation = descriptor.descriptor_generation;
  set.recovery_token.statement_savepoint_profile_uuid = Uuid(35);
  set.recovery_token.statement_savepoint_profile_generation = 36;
  set.recovery_token.durable_registry_uuid = Uuid(36);
  set.recovery_token.durable_registry_generation = 37;
  descriptor.recovery_token_uuid = set.recovery_token.recovery_token_uuid;
  descriptor.recovery_generation = set.recovery_token.recovery_generation;
  descriptor.executor_availability_generation = 38;
  descriptor.builtin_operator_snapshot_uuid =
      kTypedUpdateOperatorSnapshotUuid;
  descriptor.builtin_operator_registry_generation = 1;
  return set;
}

struct DatatypeOperatorFixture {
  TypedUpdateDescriptorCarrier descriptor;
  TypedUpdateAssignmentVector assignments;
  TypedUpdatePredicateVector predicate;
  TypedUpdateDatatypeAuthorityVector datatypes;
  TypedUpdateBuiltinOperatorAuthorityVector operators;
};

DatatypeOperatorFixture MakeDatatypeOperatorFixture(bool equality) {
  auto carriers = CarrierSet();
  auto& assignment = carriers.assignments.records.front();
  assignment.value_descriptor_uuid = kBigintDescriptorUuid;
  assignment.value_descriptor_generation = 1;
  assignment.value_type_uuid = kBigintTypeUuid;
  assignment.value_type_generation = 1;
  assignment.codec_id = "datatype.int64.le.v1";
  assignment.codec_version = 1;
  assignment.codec_generation = 1;
  assignment.canonical_value = {0x2a, 0, 0, 0, 0, 0, 0, 0};

  if (equality) {
    carriers.predicate = EqualsPredicate(
        carriers.descriptor, carriers.predicate.identity, assignment);
    carriers.predicate.records[1].canonical_value =
        {0x2a, 0, 0, 0, 0, 0, 0, 0};
  }

  TypedUpdateCarrierError error;
  std::vector<byte> bytes;
  Require(EncodeTypedUpdateAssignmentVector(carriers.assignments, &bytes,
                                             &error),
          "datatype fixture DUAV encode: " + error.detail);
  carriers.descriptor.assignment_vector_sha256 = ReadHash(bytes, 72);
  carriers.descriptor.assignment_count = 1;
  TypedUpdateAssignmentVector decoded_assignments;
  Require(DecodeAndValidateTypedUpdateAssignmentVector(
              bytes, &decoded_assignments, &error),
          "datatype fixture DUAV decode: " + error.detail);

  Require(EncodeTypedUpdatePredicateVector(carriers.predicate, &bytes,
                                            &error),
          "datatype fixture DUEV encode: " + error.detail);
  carriers.descriptor.predicate_vector_sha256 = ReadHash(bytes, 72);
  carriers.descriptor.predicate_node_count =
      static_cast<u32>(carriers.predicate.records.size());
  carriers.descriptor.predicate_root_node_id =
      carriers.predicate.records.back().node_id;
  TypedUpdatePredicateVector decoded_predicate;
  Require(DecodeAndValidateTypedUpdatePredicateVector(
              bytes, &decoded_predicate, &error),
          "datatype fixture DUEV decode: " + error.detail);

  carriers.descriptor.datatype_registry_generation = 1;
  carriers.descriptor.builtin_operator_snapshot_uuid =
      kTypedUpdateOperatorSnapshotUuid;
  carriers.descriptor.builtin_operator_registry_generation = 1;
  Require(EncodeTypedUpdateDescriptor(carriers.descriptor, &bytes, &error),
          "datatype fixture DUDC encode: " + error.detail);
  TypedUpdateDescriptorCarrier decoded_descriptor;
  Require(DecodeAndValidateTypedUpdateDescriptor(
              bytes, &decoded_descriptor, &error),
          "datatype fixture DUDC decode: " + error.detail);

  TypedUpdateDatatypeAuthorityVector datatypes;
  datatypes.identity.vector_uuid = kTypedUpdateDatatypeSnapshotUuid;
  datatypes.identity.vector_generation = 1;
  datatypes.identity.owner_descriptor_uuid =
      decoded_descriptor.descriptor_uuid;
  datatypes.identity.owner_descriptor_generation =
      decoded_descriptor.descriptor_generation;
  datatypes.records = {BooleanDatatypeAuthority(1),
                       BigintDatatypeAuthority(2)};
  Require(EncodeTypedUpdateDatatypeAuthorityVector(datatypes, &bytes,
                                                    &error),
          "datatype fixture DUDV encode: " + error.detail);
  TypedUpdateDatatypeAuthorityVector decoded_datatypes;
  Require(DecodeAndValidateTypedUpdateDatatypeAuthorityVector(
              bytes, &decoded_datatypes, &error),
          "datatype fixture DUDV decode: " + error.detail);

  TypedUpdateBuiltinOperatorAuthorityVector operators;
  operators.identity.vector_uuid = kTypedUpdateOperatorSnapshotUuid;
  operators.identity.vector_generation = 1;
  operators.identity.owner_descriptor_uuid =
      decoded_descriptor.descriptor_uuid;
  operators.identity.owner_descriptor_generation =
      decoded_descriptor.descriptor_generation;
  if (equality) {
    const auto& left = decoded_predicate.records[0];
    TypedUpdateBuiltinOperatorAuthorityRecord row;
    row.operator_ordinal = 1;
    row.operator_uuid = kTypedUpdateEqualOperatorUuid;
    row.operator_generation = 1;
    row.operator_snapshot_uuid = kTypedUpdateOperatorSnapshotUuid;
    row.operator_registry_generation = 1;
    row.left_descriptor_uuid = left.output_descriptor_uuid;
    row.left_descriptor_generation = left.output_descriptor_generation;
    row.left_type_uuid = left.output_type_uuid;
    row.left_type_generation = left.output_type_generation;
    row.right_descriptor_uuid = left.output_descriptor_uuid;
    row.right_descriptor_generation = left.output_descriptor_generation;
    row.right_type_uuid = left.output_type_uuid;
    row.right_type_generation = left.output_type_generation;
    row.result_descriptor_uuid = kTypedUpdateBooleanUuid;
    row.result_descriptor_generation = 1;
    row.result_type_uuid = kTypedUpdateBooleanUuid;
    row.result_type_generation = 1;
    row.result_codec_version = 1;
    row.result_codec_generation = 1;
    row.result_codec_id = "datatype.boolean.u8.v1";
    operators.records.push_back(std::move(row));
  }
  Require(EncodeTypedUpdateBuiltinOperatorAuthorityVector(
              operators, &bytes, &error),
          "datatype fixture DUOV encode: " + error.detail);
  TypedUpdateBuiltinOperatorAuthorityVector decoded_operators;
  Require(DecodeAndValidateTypedUpdateBuiltinOperatorAuthorityVector(
              bytes, &decoded_operators, &error),
          "datatype fixture DUOV decode: " + error.detail);

  return {std::move(decoded_descriptor),
          std::move(decoded_assignments),
          std::move(decoded_predicate),
          std::move(decoded_datatypes),
          std::move(decoded_operators)};
}

TypedUpdateResultCarrier Result(const TypedUpdateDescriptorCarrier& descriptor) {
  TypedUpdateResultCarrier result;
  result.update_descriptor_uuid = descriptor.descriptor_uuid;
  result.update_descriptor_generation = descriptor.descriptor_generation;
  result.operation_uuid = descriptor.operation_uuid;
  result.owning_transaction_uuid = descriptor.owning_transaction_uuid;
  result.owning_local_transaction_id =
      descriptor.owning_local_transaction_id;
  result.relation_uuid = descriptor.target_relation_uuid;
  result.relation_generation = descriptor.target_relation_generation;
  result.matched_count = 3;
  result.updated_count = 2;
  result.effect_set_sha256 = SeedHash(90);
  result.executor_evidence_sha256 = SeedHash(91);
  result.publication_barrier_uuid = Uuid(90);
  result.publication_barrier_generation = 91;
  return result;
}

TypedUpdateJournalRecord BoundJournal(
    const TypedUpdateDescriptorCarrier& descriptor) {
  TypedUpdateJournalRecord journal;
  journal.lifecycle_state = TypedUpdateJournalState::bound;
  journal.journal_sequence = 1;
  journal.database_uuid = Uuid(100);
  journal.authenticated_statement_receipt_uuid =
      descriptor.authenticated_statement_receipt_uuid;
  journal.owning_transaction_uuid = descriptor.owning_transaction_uuid;
  journal.owning_local_transaction_id =
      descriptor.owning_local_transaction_id;
  journal.operation_uuid = descriptor.operation_uuid;
  journal.recovery_token_uuid = descriptor.recovery_token_uuid;
  journal.recovery_generation = descriptor.recovery_generation;
  journal.descriptor = descriptor;
  return journal;
}

TypedUpdateJournalChainContext SuccessorContext(
    std::span<const byte> prior,
    TypedUpdateJournalState prior_state,
    const TypedUpdateUuid& savepoint,
    u64 savepoint_generation,
    std::span<const byte> descriptor_bytes) {
  TypedUpdateJournalChainContext context;
  context.first_record = false;
  context.prior_sequence =
      scratchbird::core::platform::LoadLittle64(prior.data() + 24);
  context.prior_state = prior_state;
  context.prior_record_evidence_sha256 = ReadHash(prior, 224);
  context.prior_savepoint_uuid = savepoint;
  context.prior_savepoint_generation = savepoint_generation;
  context.require_same_descriptor = true;
  Require(descriptor_bytes.size() == kTypedUpdateDescriptorBytes,
          "descriptor context extent");
  std::copy(descriptor_bytes.begin(), descriptor_bytes.end(),
            context.expected_descriptor_bytes.begin());
  if (prior.size() == kTypedUpdateJournalWithResultBytes) {
    std::array<byte, kTypedUpdateResultBytes> result_bytes{};
    std::copy(prior.end() - kTypedUpdateResultBytes, prior.end(),
              result_bytes.begin());
    context.expected_prepared_result_bytes = result_bytes;
  }
  return context;
}

struct SecurityRecoveryFixture {
  TypedUpdateDescriptorCarrier descriptor;
  TypedUpdateRowPolicyVector row_policies;
  TypedUpdateRecoveryTokenCarrier recovery_token;
  TypedUpdateSecurityPolicySourceVector source_policies;
  TypedUpdateSecuritySnapshotProof snapshot_proof;
  TypedUpdateJournalRecord journal_head;
  TypedUpdateMgaRecoveryObservation recovery_observation;
};

SecurityRecoveryFixture MakeSecurityRecoveryFixture() {
  auto carriers = CarrierSet();
  TypedUpdateCarrierError error;
  std::vector<byte> bytes;

  TypedUpdateSecurityPolicySourceVector source_policies;
  source_policies.identity = VectorIdentity(110, carriers.descriptor);
  TypedUpdateSecurityPolicySourceRecord source;
  source.source_policy_ordinal = 1;
  source.phase = 1;
  source.source_state = 1;
  source.policy_uuid = carriers.row_policies.records[0].effective_policy_uuid;
  source.policy_generation =
      carriers.row_policies.records[0].effective_policy_generation;
  source.policy_version_uuid = Uuid(111);
  source.effective_transaction_number = 112;
  source.target_relation_uuid = carriers.descriptor.target_relation_uuid;
  source.target_relation_generation =
      carriers.descriptor.target_relation_generation;
  source.source_expression_uuid =
      carriers.row_policies.records[0].expression_uuid;
  source.source_expression_generation =
      carriers.row_policies.records[0].expression_generation;
  source.source_expression_evidence_sha256 =
      carriers.row_policies.records[0].expression_evidence_sha256;
  source.catalog_snapshot_uuid = carriers.descriptor.catalog_snapshot_uuid;
  source.catalog_generation = carriers.descriptor.catalog_generation;
  source.security_snapshot_uuid = carriers.descriptor.security_snapshot_uuid;
  source.security_snapshot_generation = 109;
  source_policies.records.push_back(source);
  Require(EncodeTypedUpdateSecurityPolicySourceVector(
              source_policies, &bytes, &error),
          "fixture DUSV encode: " + error.detail);
  TypedUpdateSecurityPolicySourceVector decoded_source_policies;
  Require(DecodeAndValidateTypedUpdateSecurityPolicySourceVector(
              bytes, &decoded_source_policies, &error),
          "fixture DUSV decode: " + error.detail);
  const auto source_vector_hash = ReadHash(bytes, 72);

  carriers.row_policies.records[0].source_policy_catalog_vector_sha256 =
      source_vector_hash;
  Require(EncodeTypedUpdateRowPolicyVector(carriers.row_policies, &bytes,
                                           &error),
          "fixture DUPV security encode: " + error.detail);
  carriers.descriptor.row_policy_set_sha256 = ReadHash(bytes, 72);
  TypedUpdateRowPolicyVector decoded_row_policies;
  Require(DecodeAndValidateTypedUpdateRowPolicyVector(
              bytes, &decoded_row_policies, &error),
          "fixture DUPV security decode: " + error.detail);
  const auto exact_dupv_hash = Hash(bytes);

  Require(EncodeTypedUpdateDescriptor(carriers.descriptor, &bytes, &error),
          "fixture security DUDC encode: " + error.detail);
  const auto exact_dudc_hash = Hash(bytes);
  TypedUpdateDescriptorCarrier decoded_descriptor;
  Require(DecodeAndValidateTypedUpdateDescriptor(
              bytes, &decoded_descriptor, &error),
          "fixture security DUDC decode: " + error.detail);

  Require(EncodeTypedUpdateRecoveryToken(carriers.recovery_token, &bytes,
                                         &error),
          "fixture security DURC encode: " + error.detail);
  TypedUpdateRecoveryTokenCarrier decoded_recovery;
  Require(DecodeAndValidateTypedUpdateRecoveryToken(
              bytes, &decoded_recovery, &error),
          "fixture security DURC decode: " + error.detail);

  TypedUpdateSecuritySnapshotProof proof;
  proof.security_snapshot_uuid = decoded_descriptor.security_snapshot_uuid;
  proof.security_snapshot_generation = 109;
  proof.security_context_uuid = decoded_descriptor.security_context_uuid;
  proof.security_context_generation = 107;
  proof.security_epoch = decoded_descriptor.security_generation;
  proof.policy_generation = 108;
  proof.database_uuid = Uuid(100);
  proof.authenticated_statement_receipt_uuid =
      decoded_descriptor.authenticated_statement_receipt_uuid;
  proof.owning_transaction_uuid =
      decoded_descriptor.owning_transaction_uuid;
  proof.owning_local_transaction_id =
      decoded_descriptor.owning_local_transaction_id;
  proof.operation_uuid = decoded_descriptor.operation_uuid;
  proof.operation_generation = decoded_descriptor.operation_generation;
  proof.recovery_token_uuid = decoded_descriptor.recovery_token_uuid;
  proof.recovery_generation = decoded_descriptor.recovery_generation;
  proof.statement_snapshot_uuid = decoded_descriptor.statement_snapshot_uuid;
  proof.catalog_snapshot_uuid = decoded_descriptor.catalog_snapshot_uuid;
  proof.catalog_generation = decoded_descriptor.catalog_generation;
  proof.policy_catalog_epoch = 113;
  proof.target_relation_uuid = decoded_descriptor.target_relation_uuid;
  proof.target_relation_generation =
      decoded_descriptor.target_relation_generation;
  proof.target_relation_occurrence_uuid =
      decoded_descriptor.target_relation_occurrence_uuid;
  proof.target_relation_occurrence_generation =
      decoded_descriptor.target_relation_occurrence_generation;
  proof.descriptor_uuid = decoded_descriptor.descriptor_uuid;
  proof.descriptor_generation = decoded_descriptor.descriptor_generation;
  proof.row_policy_set_uuid = decoded_descriptor.row_policy_set_uuid;
  proof.row_policy_set_generation =
      decoded_descriptor.row_policy_set_generation;
  proof.source_policy_vector_uuid =
      decoded_source_policies.identity.vector_uuid;
  proof.source_policy_vector_generation =
      decoded_source_policies.identity.vector_generation;
  proof.row_policy_count = 1;
  proof.source_policy_count = 1;
  proof.snapshot_state = 1;
  proof.descriptor_evidence_sha256 =
      decoded_descriptor.descriptor_evidence_sha256;
  proof.row_policy_set_sha256 = decoded_descriptor.row_policy_set_sha256;
  proof.exact_dudc_sha256 = exact_dudc_hash;
  proof.exact_dupv_sha256 = exact_dupv_hash;
  proof.source_policy_catalog_vector_sha256 = source_vector_hash;
  Require(EncodeTypedUpdateSecuritySnapshotProof(proof, &bytes, &error),
          "fixture DUSP encode: " + error.detail);
  TypedUpdateSecuritySnapshotProof decoded_proof;
  Require(DecodeAndValidateTypedUpdateSecuritySnapshotProof(
              bytes, &decoded_proof, &error),
          "fixture DUSP decode: " + error.detail);

  auto journal = BoundJournal(decoded_descriptor);
  TypedUpdateJournalChainContext first;
  Require(EncodeTypedUpdateJournalRecord(journal, first, &bytes, &error),
          "fixture security DUJR encode: " + error.detail);
  TypedUpdateJournalRecord decoded_journal;
  Require(DecodeAndValidateTypedUpdateJournalRecord(
              bytes, first, &decoded_journal, &error),
          "fixture security DUJR decode: " + error.detail);

  TypedUpdateMgaRecoveryObservation observation;
  observation.observation_uuid = Uuid(114);
  observation.observation_generation = 1;
  observation.validated_mga_durable_handle_uuid =
      decoded_recovery.durable_registry_uuid;
  observation.validated_mga_durable_handle_generation =
      decoded_recovery.durable_registry_generation;
  observation.database_uuid = decoded_journal.database_uuid;
  observation.descriptor_uuid = decoded_descriptor.descriptor_uuid;
  observation.descriptor_generation = decoded_descriptor.descriptor_generation;
  observation.operation_uuid = decoded_descriptor.operation_uuid;
  observation.operation_generation = decoded_descriptor.operation_generation;
  observation.authenticated_statement_receipt_uuid =
      decoded_descriptor.authenticated_statement_receipt_uuid;
  observation.owning_transaction_uuid =
      decoded_descriptor.owning_transaction_uuid;
  observation.owning_local_transaction_id =
      decoded_descriptor.owning_local_transaction_id;
  observation.recovery_token_uuid = decoded_descriptor.recovery_token_uuid;
  observation.recovery_generation = decoded_descriptor.recovery_generation;
  observation.latest_journal_state = TypedUpdateJournalState::bound;
  observation.transaction_state = TypedUpdateTransactionState::active_live;
  observation.savepoint_state = TypedUpdateSavepointState::absent;
  observation.statement_barrier_present = false;
  observation.no_surviving_effect_proven = true;
  observation.reserved_statement_barrier_uuid = Uuid(90);
  observation.reserved_statement_barrier_generation = 91;
  observation.durable_chain_head_sequence = decoded_journal.journal_sequence;
  observation.durable_chain_head_record_evidence_sha256 =
      decoded_journal.record_evidence_sha256;
  observation.catalog_snapshot_uuid = decoded_proof.catalog_snapshot_uuid;
  observation.catalog_generation = decoded_proof.catalog_generation;
  observation.security_snapshot_uuid = decoded_proof.security_snapshot_uuid;
  observation.security_snapshot_generation =
      decoded_proof.security_snapshot_generation;
  observation.security_epoch = decoded_proof.security_epoch;
  Require(EncodeTypedUpdateMgaRecoveryObservation(
              observation, &bytes, &error),
          "fixture DUMO encode: " + error.detail);
  TypedUpdateMgaRecoveryObservation decoded_observation;
  Require(DecodeAndValidateTypedUpdateMgaRecoveryObservation(
              bytes, &decoded_observation, &error),
          "fixture DUMO decode: " + error.detail);

  return {std::move(decoded_descriptor), std::move(decoded_row_policies),
          std::move(decoded_recovery),
          std::move(decoded_source_policies), std::move(decoded_proof),
          std::move(decoded_journal), std::move(decoded_observation)};
}

void TestRoundTripsAndDomains() {
  auto set = CarrierSet();
  TypedUpdateCarrierError error;
  Require(ValidateTypedUpdateCarrierSet(set, &error),
          "complete carrier set validates: " + error.detail);

  std::vector<byte> dudc;
  Require(EncodeTypedUpdateDescriptor(set.descriptor, &dudc, &error),
          "DUDC encode");
  Require(dudc.size() == 712 && LoadLittle16(dudc.data() + 6) == 712,
          "DUDC exact 712-byte extent");
  Require(std::equal(kTypedUpdateOperatorSnapshotUuid.begin(),
                     kTypedUpdateOperatorSnapshotUuid.end(),
                     dudc.begin() + 656),
          "DUDC operator snapshot offset 656");
  Require(ReadHash(dudc, 680) ==
              Evidence("ScratchBird.SblrDmlUpdateRowsDescriptor.V1",
                       std::span<const byte>(dudc).first(680)),
          "DUDC exact descriptor hash domain/material");
  TypedUpdateDescriptorCarrier decoded_dudc;
  Require(DecodeAndValidateTypedUpdateDescriptor(dudc, &decoded_dudc,
                                                  &error),
          "DUDC decode");
  Require(decoded_dudc.exact_bytes == dudc,
          "DUDC retains byte-identical input");

  struct VectorCase {
    std::string_view magic;
    std::string_view domain;
    std::vector<byte> bytes;
  };
  std::vector<VectorCase> vectors;
  std::vector<byte> bytes;
  Require(EncodeTypedUpdateAssignmentVector(set.assignments, &bytes, &error),
          "DUAV encode");
  Require(LoadLittle32(bytes.data() + 104) ==
              kTypedUpdateAssignmentPrefixBytes +
                  set.assignments.records[0].codec_id.size() +
                  set.assignments.records[0].canonical_value.size(),
          "DUAV 180+codec+value extent");
  vectors.push_back({"DUAV",
                     "ScratchBird.SblrDmlUpdateRowsAssignmentVector.V1",
                     bytes});
  TypedUpdateAssignmentVector decoded_assignments;
  Require(DecodeAndValidateTypedUpdateAssignmentVector(
              bytes, &decoded_assignments, &error),
          "DUAV decode");
  Require(decoded_assignments.exact_bytes == bytes &&
              decoded_assignments.records[0].canonical_value ==
                  set.assignments.records[0].canonical_value,
          "DUAV exact/binary value round-trip");

  Require(EncodeTypedUpdatePredicateVector(set.predicate, &bytes, &error),
          "DUEV encode");
  Require(LoadLittle32(bytes.data() + 104) ==
              kTypedUpdatePredicatePrefixBytes +
                  set.predicate.records[0].output_codec_id.size() + 1 +
                  kTypedUpdatePredicateEvidenceBytes,
          "DUEV 284+codec+value extent");
  vectors.push_back({"DUEV",
                     "ScratchBird.SblrDmlUpdateRowsPredicateVector.V1",
                     bytes});
  TypedUpdatePredicateVector decoded_predicate;
  Require(DecodeAndValidateTypedUpdatePredicateVector(
              bytes, &decoded_predicate, &error),
          "DUEV decode");
  Require(decoded_predicate.records[0].canonical_value_sha256 ==
              Hash(std::span<const byte>(
                  decoded_predicate.records[0].canonical_value)),
          "DUEV canonical value SHA at fixed field");

  auto equality = EqualsPredicate(
      set.descriptor, VectorIdentity(25, set.descriptor),
      set.assignments.records[0]);
  std::vector<byte> equality_bytes;
  Require(EncodeTypedUpdatePredicateVector(equality, &equality_bytes,
                                            &error),
          "three-node column-equality-literal DUEV encode");
  TypedUpdatePredicateVector decoded_equality;
  Require(DecodeAndValidateTypedUpdatePredicateVector(
              equality_bytes, &decoded_equality, &error) &&
              decoded_equality.records.size() == 3 &&
              decoded_equality.records[2].operator_uuid ==
                  kTypedUpdateEqualOperatorUuid &&
              decoded_equality.records[2].operator_generation == 1 &&
              decoded_equality.records[0].canonical_value_sha256 ==
                  Hash(std::span<const byte>{}) &&
              decoded_equality.records[2].canonical_value_sha256 ==
                  Hash(std::span<const byte>{}),
          "three-node equality DUEV has exact operator identity");
  auto wrong_equality = equality;
  wrong_equality.records[2].operator_generation = 2;
  Require(!EncodeTypedUpdatePredicateVector(wrong_equality, &equality_bytes,
                                             &error) &&
              error.code ==
                  TypedUpdateCarrierErrorCode::operator_identity_invalid,
          "non-authoritative equality generation is refused");

  Require(EncodeTypedUpdateRowPolicyVector(set.row_policies, &bytes, &error),
          "DUPV encode");
  vectors.push_back({"DUPV",
                     "ScratchBird.SblrDmlUpdateRowsRowPolicyVector.V1",
                     bytes});
  TypedUpdateRowPolicyVector decoded_policy;
  Require(DecodeAndValidateTypedUpdateRowPolicyVector(bytes, &decoded_policy,
                                                       &error),
          "DUPV decode");
  Require(ReadHash(bytes, 104 + 144) ==
              Evidence("ScratchBird.SblrDmlUpdateRowsRowPolicyRecord.V1",
                       std::span<const byte>(bytes).subspan(104, 144)),
          "DUPV record hash domain/material");

  Require(EncodeTypedUpdateConstraintVector(set.constraints, &bytes, &error),
          "DUCV encode");
  vectors.push_back({"DUCV",
                     "ScratchBird.SblrDmlUpdateRowsConstraintVector.V1",
                     bytes});
  TypedUpdateConstraintVector decoded_constraints;
  Require(DecodeAndValidateTypedUpdateConstraintVector(
              bytes, &decoded_constraints, &error),
          "DUCV decode");
  Require(ReadHash(bytes, 104 + 112) ==
              Evidence("ScratchBird.SblrDmlUpdateRowsConstraintRecord.V1",
                       std::span<const byte>(bytes).subspan(104, 112)),
          "DUCV record hash domain/material");

  Require(EncodeTypedUpdateTriggerVector(set.triggers, &bytes, &error),
          "DUTV encode");
  vectors.push_back({"DUTV",
                     "ScratchBird.SblrDmlUpdateRowsTriggerVector.V1",
                     bytes});
  TypedUpdateTriggerVector decoded_triggers;
  Require(DecodeAndValidateTypedUpdateTriggerVector(bytes, &decoded_triggers,
                                                     &error),
          "DUTV decode");
  Require(ReadHash(bytes, 104 + 144) ==
              Evidence("ScratchBird.SblrDmlUpdateRowsTriggerRecord.V1",
                       std::span<const byte>(bytes).subspan(104, 144)),
          "DUTV record hash domain/material");
  for (const auto& row : vectors) {
    Require(std::equal(row.magic.begin(), row.magic.end(), row.bytes.begin()),
            "vector magic");
    Require(ReadHash(row.bytes, 72) ==
                Evidence(row.domain,
                         std::span<const byte>(row.bytes).subspan(104)),
            "vector direct-domain evidence");
  }

  Require(EncodeTypedUpdateTargetOrder(set.target_order, &bytes, &error),
          "DUOR encode");
  Require(bytes.size() == 160 && ReadHash(bytes, 112) ==
              Evidence("ScratchBird.SblrDmlUpdateRowsTargetOrder.V1",
                       std::span<const byte>(bytes).first(112)),
          "DUOR exact extent/domain");
  TypedUpdateTargetOrderCarrier decoded_order;
  Require(DecodeAndValidateTypedUpdateTargetOrder(bytes, &decoded_order,
                                                   &error),
          "DUOR decode");

  Require(EncodeTypedUpdateResourceBudget(set.resource_budget, &bytes, &error),
          "DUBR encode");
  Require(bytes.size() == 208 && ReadHash(bytes, 152) ==
              Evidence("ScratchBird.SblrDmlUpdateRowsResourceBudget.V1",
                       std::span<const byte>(bytes).first(152)),
          "DUBR exact extent/domain");
  TypedUpdateResourceBudgetCarrier decoded_budget;
  Require(DecodeAndValidateTypedUpdateResourceBudget(bytes, &decoded_budget,
                                                      &error),
          "DUBR decode");

  Require(EncodeTypedUpdateRecoveryToken(set.recovery_token, &bytes, &error),
          "DURC encode");
  Require(bytes.size() == 208 && ReadHash(bytes, 168) ==
              Evidence("ScratchBird.SblrDmlUpdateRowsRecoveryToken.V1",
                       std::span<const byte>(bytes).first(168)),
          "DURC exact extent/domain");
  TypedUpdateRecoveryTokenCarrier decoded_recovery;
  Require(DecodeAndValidateTypedUpdateRecoveryToken(bytes, &decoded_recovery,
                                                     &error),
          "DURC decode");

  auto result = Result(set.descriptor);
  Require(EncodeTypedUpdateResult(result, &bytes, &error), "DURS encode");
  Require(bytes.size() == 256 && ReadHash(bytes, 216) ==
              Evidence("ScratchBird.SblrDmlUpdateRowsResult.V1",
                       std::span<const byte>(bytes).first(216)),
          "DURS exact extent/domain");
  TypedUpdateResultCarrier decoded_result;
  Require(DecodeAndValidateTypedUpdateResult(bytes, &decoded_result, &error),
          "DURS decode");
  Require(decoded_result.exact_bytes == bytes,
          "DURS retains exact bytes");
}

void TestExplicitNullAndInjectivity() {
  auto set = CarrierSet();
  auto first = Assignment(1, 110, {});
  auto second = Assignment(2, 120, {});
  second.value_state = TypedUpdateValueState::null_value;
  set.assignments.records = {first, second};
  std::vector<byte> bytes;
  TypedUpdateCarrierError error;
  Require(EncodeTypedUpdateAssignmentVector(set.assignments, &bytes, &error),
          "empty VALUE and NULL encode");
  const auto first_bytes = LoadLittle32(bytes.data() + 104);
  Require(bytes[104 + 140] == 1 &&
              bytes[104 + first_bytes + 140] == 2,
          "empty VALUE and NULL have disjoint value-state bytes");
  TypedUpdateAssignmentVector decoded;
  Require(DecodeAndValidateTypedUpdateAssignmentVector(bytes, &decoded,
                                                        &error),
          "empty VALUE and NULL decode");
  Require(decoded.records[0].canonical_value.empty() &&
              decoded.records[1].canonical_value.empty() &&
              decoded.records[0].value_state == TypedUpdateValueState::value &&
              decoded.records[1].value_state ==
                  TypedUpdateValueState::null_value &&
              decoded.records[0].canonical_value_sha256 ==
                  Hash(std::span<const byte>{}) &&
              decoded.records[1].canonical_value_sha256 ==
                  Hash(std::span<const byte>{}),
          "empty VALUE remains distinct from NULL");

  auto duplicate = set.assignments;
  duplicate.records[1].target_column_uuid =
      duplicate.records[0].target_column_uuid;
  Require(!EncodeTypedUpdateAssignmentVector(duplicate, &bytes, &error) &&
              error.code == TypedUpdateCarrierErrorCode::duplicate_target,
          "duplicate target is refused before encoding");
  auto bad_codec = set.assignments;
  bad_codec.records[0].codec_id = "datatype;bad=codec";
  Require(!EncodeTypedUpdateAssignmentVector(bad_codec, &bytes, &error) &&
              error.code == TypedUpdateCarrierErrorCode::codec_id_invalid,
          "semicolon/equal codec ID is not canonical authority");
  bad_codec.records[0].codec_id.assign(256, 'a');
  Require(!EncodeTypedUpdateAssignmentVector(bad_codec, &bytes, &error) &&
              error.code == TypedUpdateCarrierErrorCode::codec_id_invalid,
          "codec ID above the exact 255-byte limit is refused");
  auto oversized_value = set.assignments;
  oversized_value.records[0].canonical_value.assign(
      kTypedUpdateMaximumCanonicalValueBytesPerValue + 1, 0x5a);
  Require(!EncodeTypedUpdateAssignmentVector(oversized_value, &bytes,
                                              &error) &&
              error.code ==
                  TypedUpdateCarrierErrorCode::resource_limit_exceeded,
          "canonical value above the exact 65536-byte per-value limit is refused");
}

void TestResultInnerEvidence() {
  auto set = CarrierSet();
  auto result = Result(set.descriptor);
  TypedUpdateCarrierError error;
  std::vector<TypedUpdateResultEvidenceReference> evidence = {
      {"zeta", "id=2"}, {"alpha", "id1"}, {"alpha", "id1"}};
  std::vector<byte> material;
  Require(EncodeTypedUpdateResultEvidenceMaterial(result, evidence, &material,
                                                   &error),
          "result inner evidence material encode");
  std::vector<byte> expected;
  expected.insert(expected.end(), result.update_descriptor_uuid.begin(),
                  result.update_descriptor_uuid.end());
  expected.insert(expected.end(), result.operation_uuid.begin(),
                  result.operation_uuid.end());
  expected.insert(expected.end(), result.owning_transaction_uuid.begin(),
                  result.owning_transaction_uuid.end());
  expected.insert(expected.end(), result.relation_uuid.begin(),
                  result.relation_uuid.end());
  const auto append_record = [&expected](std::string_view record) {
    expected.insert(expected.end(), record.begin(), record.end());
    expected.push_back(0);
  };
  append_record("alpha=id1");
  append_record("alpha=id1");
  append_record("zeta=id=2");
  Require(material == expected,
          "result evidence records sort bytewise, retain duplicates, and allow equals in ID");
  TypedUpdateHash effect{};
  TypedUpdateHash executor{};
  Require(ComputeTypedUpdateResultInnerEvidence(
              result, evidence, &effect, &executor, &error) &&
              effect == Evidence("ScratchBird.SblrDmlUpdateRowsEffectSet.V1",
                                 material) &&
              executor == Evidence(
                              "ScratchBird.SblrDmlUpdateRowsExecutorEvidence.V1",
                              material) &&
              effect != executor,
          "effect/executor hashes use disjoint exact direct domains");

  std::vector<TypedUpdateResultEvidenceReference> empty;
  Require(EncodeTypedUpdateResultEvidenceMaterial(result, empty, &material,
                                                   &error) &&
              material.size() == 64,
          "empty evidence vector is exactly the four UUID identities");
  std::vector<TypedUpdateResultEvidenceReference> invalid = {
      {"bad=kind", "id"}};
  Require(!EncodeTypedUpdateResultEvidenceMaterial(result, invalid, &material,
                                                    &error) &&
              error.code == TypedUpdateCarrierErrorCode::
                                result_evidence_material_invalid,
          "equals in evidence kind is refused injectively");
  invalid = {{"kind", std::string("bad\0id", 6)}};
  Require(!EncodeTypedUpdateResultEvidenceMaterial(result, invalid, &material,
                                                    &error) &&
              error.code == TypedUpdateCarrierErrorCode::
                                result_evidence_material_invalid,
          "NUL in evidence ID is refused injectively");
  invalid = {{"kind", std::string("\xc0\x80", 2)}};
  Require(!EncodeTypedUpdateResultEvidenceMaterial(result, invalid, &material,
                                                    &error) &&
              error.code == TypedUpdateCarrierErrorCode::
                                result_evidence_material_invalid,
          "noncanonical UTF-8 evidence ID is refused");
}

void TestEmptyFrozenSets() {
  auto set = CarrierSet();
  TypedUpdateCarrierError error;
  std::vector<byte> bytes;

  set.row_policies.records.clear();
  Require(EncodeTypedUpdateRowPolicyVector(set.row_policies, &bytes, &error) &&
              bytes.size() == kTypedUpdateVectorHeaderBytes &&
              LoadLittle32(bytes.data() + 64) == 0 &&
              ReadHash(bytes, 72) ==
                  Evidence(
                      "ScratchBird.SblrDmlUpdateRowsRowPolicyVector.V1",
                      std::span<const byte>{}),
          "empty DUPV hashes the exact empty records sequence");
  TypedUpdateRowPolicyVector policies;
  Require(DecodeAndValidateTypedUpdateRowPolicyVector(bytes, &policies,
                                                       &error) &&
              policies.records.empty(),
          "empty DUPV round-trip");

  set.constraints.records.clear();
  Require(EncodeTypedUpdateConstraintVector(set.constraints, &bytes,
                                             &error) &&
              bytes.size() == kTypedUpdateVectorHeaderBytes &&
              LoadLittle32(bytes.data() + 64) == 0 &&
              ReadHash(bytes, 72) ==
                  Evidence(
                      "ScratchBird.SblrDmlUpdateRowsConstraintVector.V1",
                      std::span<const byte>{}),
          "empty DUCV hashes the exact empty records sequence");
  TypedUpdateConstraintVector constraints;
  Require(DecodeAndValidateTypedUpdateConstraintVector(bytes, &constraints,
                                                        &error) &&
              constraints.records.empty(),
          "empty DUCV round-trip");

  set.triggers.records.clear();
  Require(EncodeTypedUpdateTriggerVector(set.triggers, &bytes, &error) &&
              bytes.size() == kTypedUpdateVectorHeaderBytes &&
              LoadLittle32(bytes.data() + 64) == 0 &&
              ReadHash(bytes, 72) ==
                  Evidence(
                      "ScratchBird.SblrDmlUpdateRowsTriggerVector.V1",
                      std::span<const byte>{}),
          "empty DUTV hashes the exact empty records sequence");
  TypedUpdateTriggerVector triggers;
  Require(DecodeAndValidateTypedUpdateTriggerVector(bytes, &triggers,
                                                     &error) &&
              triggers.records.empty(),
          "empty DUTV round-trip");
}

void TestMalformedPrecedence() {
  auto set = CarrierSet();
  TypedUpdateCarrierError error;
  std::vector<byte> bytes;
  Require(EncodeTypedUpdateDescriptor(set.descriptor, &bytes, &error),
          "malformed fixture DUDC");
  auto bad = bytes;
  bad[0] ^= 1;
  bad[680] ^= 1;
  TypedUpdateDescriptorCarrier descriptor;
  Require(!DecodeAndValidateTypedUpdateDescriptor(bad, &descriptor, &error) &&
              error.code == TypedUpdateCarrierErrorCode::magic_invalid,
          "magic refusal precedes evidence mismatch");
  bad = bytes;
  bad[276] = 1;
  bad[680] ^= 1;
  Require(!DecodeAndValidateTypedUpdateDescriptor(bad, &descriptor, &error) &&
              error.code == TypedUpdateCarrierErrorCode::reserved_invalid,
          "reserved refusal precedes evidence mismatch");
  bad = bytes;
  bad.push_back(0);
  Require(!DecodeAndValidateTypedUpdateDescriptor(bad, &descriptor, &error) &&
              error.code == TypedUpdateCarrierErrorCode::extent_invalid,
          "trailing DUDC byte is refused by extent");
  bad = bytes;
  bad[680] ^= 1;
  Require(!DecodeAndValidateTypedUpdateDescriptor(bad, &descriptor, &error) &&
              error.code ==
                  TypedUpdateCarrierErrorCode::descriptor_evidence_mismatch,
          "DUDC evidence mismatch is field-specific");

  Require(EncodeTypedUpdateAssignmentVector(set.assignments, &bytes, &error),
          "malformed fixture DUAV");
  bad = bytes;
  bad[104 + 148] ^= 1;
  RewriteVectorEvidence(
      &bad, "ScratchBird.SblrDmlUpdateRowsAssignmentVector.V1");
  TypedUpdateAssignmentVector assignments;
  Require(!DecodeAndValidateTypedUpdateAssignmentVector(
              bad, &assignments, &error) &&
              error.code ==
                  TypedUpdateCarrierErrorCode::canonical_value_hash_mismatch,
          "DUAV canonical value hash is independently checked");

  Require(EncodeTypedUpdatePredicateVector(set.predicate, &bytes, &error),
          "malformed fixture DUEV");
  bad = bytes;
  bad[104 + 220] ^= 1;
  RewriteVectorEvidence(
      &bad, "ScratchBird.SblrDmlUpdateRowsPredicateVector.V1");
  TypedUpdatePredicateVector predicate;
  Require(!DecodeAndValidateTypedUpdatePredicateVector(
              bad, &predicate, &error) &&
              error.code ==
                  TypedUpdateCarrierErrorCode::canonical_value_hash_mismatch,
          "DUEV canonical value hash is allocated and checked");

  Require(EncodeTypedUpdateRowPolicyVector(set.row_policies, &bytes, &error),
          "malformed fixture DUPV");
  bad = bytes;
  bad[104 + 144] ^= 1;
  RewriteVectorEvidence(
      &bad, "ScratchBird.SblrDmlUpdateRowsRowPolicyVector.V1");
  TypedUpdateRowPolicyVector policies;
  Require(!DecodeAndValidateTypedUpdateRowPolicyVector(
              bad, &policies, &error) &&
              error.code ==
                  TypedUpdateCarrierErrorCode::record_evidence_mismatch,
          "frozen record evidence is checked below outer vector evidence");

  auto zero_generation = set.descriptor;
  zero_generation.executor_availability_generation = 0;
  Require(!EncodeTypedUpdateDescriptor(zero_generation, &bytes, &error) &&
              error.code == TypedUpdateCarrierErrorCode::generation_invalid,
          "zero descriptor generation is refused");
  auto wrong_operator = set.descriptor;
  wrong_operator.builtin_operator_snapshot_uuid = Uuid(200);
  Require(!EncodeTypedUpdateDescriptor(wrong_operator, &bytes, &error) &&
              error.code ==
                  TypedUpdateCarrierErrorCode::operator_identity_invalid,
          "non-authoritative operator snapshot is refused");
  auto excessive = set.resource_budget;
  excessive.maximum_assignments = kTypedUpdateMaximumAssignments + 1;
  Require(!EncodeTypedUpdateResourceBudget(excessive, &bytes, &error) &&
              error.code ==
                  TypedUpdateCarrierErrorCode::resource_limit_exceeded,
          "resource hard cap is refused");

  auto invalid_result = Result(set.descriptor);
  invalid_result.updated_count = invalid_result.matched_count + 1;
  Require(!EncodeTypedUpdateResult(invalid_result, &bytes, &error) &&
              error.code == TypedUpdateCarrierErrorCode::result_invalid,
          "DURS updated count cannot exceed matched count");

  auto duplicate_constraints = set.constraints;
  duplicate_constraints.records.push_back(duplicate_constraints.records[0]);
  duplicate_constraints.records[1].constraint_ordinal = 2;
  Require(!EncodeTypedUpdateConstraintVector(duplicate_constraints, &bytes,
                                              &error) &&
              error.code ==
                  TypedUpdateCarrierErrorCode::duplicate_occurrence,
          "duplicate frozen constraint UUID is refused");
  auto unordered_triggers = set.triggers;
  unordered_triggers.records.push_back(unordered_triggers.records[0]);
  unordered_triggers.records[0].timing = 2;
  unordered_triggers.records[1].trigger_ordinal = 2;
  unordered_triggers.records[1].trigger_uuid = Uuid(201);
  unordered_triggers.records[1].timing = 1;
  Require(!EncodeTypedUpdateTriggerVector(unordered_triggers, &bytes,
                                           &error) &&
              error.code == TypedUpdateCarrierErrorCode::frozen_set_invalid,
          "DUTV descending timing order is refused");
}

void TestJournalAndCutpoints() {
  auto set = CarrierSet();
  TypedUpdateCarrierError error;
  auto bound = BoundJournal(set.descriptor);
  TypedUpdateJournalChainContext first;
  std::vector<byte> bound_bytes;
  Require(EncodeTypedUpdateJournalRecord(bound, first, &bound_bytes, &error),
          "bound DUJR encode");
  Require(bound_bytes.size() == kTypedUpdateJournalWithoutResultBytes,
          "bound DUJR exact 968 bytes");
  TypedUpdateJournalRecord decoded_bound;
  Require(DecodeAndValidateTypedUpdateJournalRecord(
              bound_bytes, first, &decoded_bound, &error),
          "bound DUJR decode");
  Require(decoded_bound.exact_bytes == bound_bytes &&
              decoded_bound.embedded_descriptor_bytes.size() == 712 &&
              !decoded_bound.embedded_result_bytes.has_value(),
          "DUJR exposes exact outer and embedded DUDC bytes");
  TypedUpdateHash extracted_evidence{};
  Require(ExtractTypedUpdateJournalRecordEvidence(
              bound_bytes, &extracted_evidence, &error) &&
              extracted_evidence == ReadHash(bound_bytes, 224) &&
              extracted_evidence == decoded_bound.record_evidence_sha256,
          "DUJR evidence extractor returns the encoded record evidence");

  const auto savepoint = Uuid(150);
  auto intent = bound;
  intent.lifecycle_state = TypedUpdateJournalState::intent;
  intent.journal_sequence = 2;
  intent.prior_record_sha256 = ReadHash(bound_bytes, 224);
  intent.statement_savepoint_uuid = savepoint;
  intent.statement_savepoint_generation = 151;
  auto intent_context = SuccessorContext(
      bound_bytes, TypedUpdateJournalState::bound, {}, 0,
      decoded_bound.embedded_descriptor_bytes);
  std::vector<byte> intent_bytes;
  Require(EncodeTypedUpdateJournalRecord(intent, intent_context,
                                         &intent_bytes, &error),
          "intent DUJR encode");

  auto aborted = bound;
  aborted.lifecycle_state = TypedUpdateJournalState::aborted;
  aborted.journal_sequence = 2;
  aborted.prior_record_sha256 = ReadHash(bound_bytes, 224);
  std::vector<byte> aborted_bytes;
  Require(EncodeTypedUpdateJournalRecord(aborted, intent_context,
                                         &aborted_bytes, &error) &&
              aborted_bytes.size() ==
                  kTypedUpdateJournalWithoutResultBytes,
          "bound-to-aborted DUJR preserves the nil savepoint and no result");

  auto provider_rollback_context = SuccessorContext(
      bound_bytes, TypedUpdateJournalState::bound, savepoint, 151,
      decoded_bound.embedded_descriptor_bytes);
  auto aborted_after_savepoint = aborted;
  aborted_after_savepoint.statement_savepoint_uuid = savepoint;
  aborted_after_savepoint.statement_savepoint_generation = 151;
  std::vector<byte> aborted_after_savepoint_bytes;
  Require(EncodeTypedUpdateJournalRecord(
              aborted_after_savepoint, provider_rollback_context,
              &aborted_after_savepoint_bytes, &error) &&
              aborted_after_savepoint_bytes.size() ==
                  kTypedUpdateJournalWithoutResultBytes,
          "bound-to-aborted carries the exact MGA savepoint when intent publication failed after open");
  TypedUpdateJournalRecord decoded_aborted_after_savepoint;
  Require(DecodeAndValidateTypedUpdateJournalRecord(
              aborted_after_savepoint_bytes, provider_rollback_context,
              &decoded_aborted_after_savepoint, &error) &&
              decoded_aborted_after_savepoint.statement_savepoint_uuid ==
                  savepoint &&
              decoded_aborted_after_savepoint.statement_savepoint_generation ==
                  151,
          "bound-to-aborted decodes only with the matching MGA savepoint authority");

  auto unexpected_savepoint = aborted_after_savepoint;
  Require(!EncodeTypedUpdateJournalRecord(
              unexpected_savepoint, intent_context, &aborted_bytes, &error) &&
              error.code ==
                  TypedUpdateCarrierErrorCode::journal_chain_mismatch,
          "bound-to-aborted refuses a savepoint when the provider reports none");
  auto missing_savepoint = aborted;
  Require(!EncodeTypedUpdateJournalRecord(
              missing_savepoint, provider_rollback_context, &aborted_bytes,
              &error) &&
              error.code ==
                  TypedUpdateCarrierErrorCode::journal_chain_mismatch,
          "bound-to-aborted refuses nil when the provider reports an opened savepoint");
  auto wrong_savepoint = aborted_after_savepoint;
  wrong_savepoint.statement_savepoint_uuid = Uuid(151);
  Require(!EncodeTypedUpdateJournalRecord(
              wrong_savepoint, provider_rollback_context, &aborted_bytes,
              &error) &&
              error.code ==
                  TypedUpdateCarrierErrorCode::journal_chain_mismatch,
          "bound-to-aborted refuses a nonmatching provider savepoint UUID");
  auto wrong_savepoint_generation = aborted_after_savepoint;
  wrong_savepoint_generation.statement_savepoint_generation = 152;
  Require(!EncodeTypedUpdateJournalRecord(
              wrong_savepoint_generation, provider_rollback_context,
              &aborted_bytes, &error) &&
              error.code ==
                  TypedUpdateCarrierErrorCode::journal_chain_mismatch,
          "bound-to-aborted refuses a nonmatching provider savepoint generation");
  auto malformed_provider_context = provider_rollback_context;
  malformed_provider_context.prior_savepoint_generation = 0;
  Require(!EncodeTypedUpdateJournalRecord(
              aborted_after_savepoint, malformed_provider_context,
              &aborted_bytes, &error) &&
              error.code ==
                  TypedUpdateCarrierErrorCode::journal_chain_mismatch,
          "bound-to-aborted refuses malformed provider savepoint authority");

  auto mutated_aborted = aborted_after_savepoint_bytes;
  mutated_aborted[152] ^= 1;
  RewriteJournalEvidence(&mutated_aborted);
  Require(!DecodeAndValidateTypedUpdateJournalRecord(
              mutated_aborted, provider_rollback_context,
              &decoded_aborted_after_savepoint, &error) &&
              error.code ==
                  TypedUpdateCarrierErrorCode::journal_chain_mismatch,
          "bound-to-aborted savepoint mutation is refused before outer evidence");

  auto prepared = intent;
  prepared.lifecycle_state = TypedUpdateJournalState::prepared;
  prepared.journal_sequence = 3;
  prepared.prior_record_sha256 = ReadHash(intent_bytes, 224);
  prepared.prior_result = Result(set.descriptor);
  auto prepared_context = SuccessorContext(
      intent_bytes, TypedUpdateJournalState::intent, savepoint, 151,
      decoded_bound.embedded_descriptor_bytes);
  std::vector<byte> prepared_bytes;
  Require(EncodeTypedUpdateJournalRecord(prepared, prepared_context,
                                         &prepared_bytes, &error),
          "prepared DUJR encode");
  Require(prepared_bytes.size() == kTypedUpdateJournalWithResultBytes,
          "prepared DUJR exact 1224 bytes");
  TypedUpdateJournalRecord decoded_prepared;
  Require(DecodeAndValidateTypedUpdateJournalRecord(
              prepared_bytes, prepared_context, &decoded_prepared, &error),
          "prepared DUJR decode");
  Require(decoded_prepared.embedded_result_bytes.has_value() &&
              decoded_prepared.embedded_result_bytes->size() == 256,
          "DUJR exposes exact embedded DURS bytes");

  auto published = prepared;
  published.lifecycle_state = TypedUpdateJournalState::published;
  published.journal_sequence = 4;
  published.prior_record_sha256 = ReadHash(prepared_bytes, 224);
  auto published_context = SuccessorContext(
      prepared_bytes, TypedUpdateJournalState::prepared, savepoint, 151,
      decoded_bound.embedded_descriptor_bytes);
  std::vector<byte> published_bytes;
  Require(EncodeTypedUpdateJournalRecord(published, published_context,
                                         &published_bytes, &error),
          "published DUJR encode");
  TypedUpdateJournalRecord decoded_published;
  Require(DecodeAndValidateTypedUpdateJournalRecord(
              published_bytes, published_context, &decoded_published,
              &error) &&
              decoded_published.embedded_result_bytes ==
                  decoded_prepared.embedded_result_bytes,
          "published DUJR retains and decodes byte-identical prepared DURS");
  auto changed_published = published;
  changed_published.prior_result->matched_count += 1;
  std::vector<byte> changed_published_bytes;
  Require(!EncodeTypedUpdateJournalRecord(
              changed_published, published_context,
              &changed_published_bytes, &error) &&
              error.code ==
                  TypedUpdateCarrierErrorCode::journal_chain_mismatch,
          "published DUJR must retain byte-identical prepared DURS");

  auto illegal = published;
  illegal.journal_sequence = 2;
  illegal.prior_record_sha256 = ReadHash(bound_bytes, 224);
  Require(!EncodeTypedUpdateJournalRecord(illegal, intent_context,
                                          &published_bytes, &error) &&
              error.code ==
                  TypedUpdateCarrierErrorCode::journal_transition_invalid,
          "bound-to-published transition is refused");

  auto bad = prepared_bytes;
  bad[224] ^= 1;
  Require(!DecodeAndValidateTypedUpdateJournalRecord(
              bad, prepared_context, &decoded_prepared, &error) &&
              error.code ==
                  TypedUpdateCarrierErrorCode::journal_evidence_mismatch,
          "DUJR record evidence mismatch is exact");
  bad = prepared_bytes;
  StoreLittle64(bad.data() + 24, 99);
  Require(!DecodeAndValidateTypedUpdateJournalRecord(
              bad, prepared_context, &decoded_prepared, &error) &&
              error.code ==
                  TypedUpdateCarrierErrorCode::journal_sequence_invalid,
          "DUJR sequence refusal precedes outer evidence verification");
  bad = prepared_bytes;
  bad[192] ^= 1;
  Require(!DecodeAndValidateTypedUpdateJournalRecord(
              bad, prepared_context, &decoded_prepared, &error) &&
              error.code ==
                  TypedUpdateCarrierErrorCode::journal_chain_mismatch,
          "DUJR predecessor refusal precedes outer evidence verification");
  bad = prepared_bytes;
  bad[kTypedUpdateJournalHeaderBytes + 680] ^= 1;
  RewriteJournalEvidence(&bad);
  Require(!DecodeAndValidateTypedUpdateJournalRecord(
              bad, prepared_context, &decoded_prepared, &error) &&
              error.field.find("embedded_DUDC") == 0,
          "DUJR validates embedded DUDC after outer evidence");

}

void TestSecurityRecoveryCarriers() {
  auto fixture = MakeSecurityRecoveryFixture();
  TypedUpdateCarrierError error;
  Require(ValidateTypedUpdateSecurityRecoveryAuthority(
              fixture.descriptor, fixture.row_policies,
              fixture.recovery_token, fixture.source_policies,
              fixture.snapshot_proof, fixture.journal_head,
              fixture.recovery_observation, &error),
          "DUSV/DUSP/DUMO authority set validates: " + error.detail);

  const auto& dusv = fixture.source_policies.exact_bytes;
  Require(dusv.size() ==
              kTypedUpdateVectorHeaderBytes +
                  kTypedUpdateSecurityPolicySourceRecordBytes &&
              LoadLittle16(dusv.data() + 6) ==
                  kTypedUpdateVectorHeaderBytes &&
              LoadLittle32(dusv.data() + 64) == 1 &&
              LoadLittle32(dusv.data() + 68) ==
                  kTypedUpdateSecurityPolicySourceRecordBytes,
          "DUSV uses exact canonical header and one DUSR256 record");
  const auto dusr = std::span<const byte>(dusv).subspan(
      kTypedUpdateVectorHeaderBytes,
      kTypedUpdateSecurityPolicySourceRecordBytes);
  Require(ReadHash(dusr, 184) ==
              Evidence(
                  "ScratchBird.SblrDmlUpdateRowsSecurityPolicyCatalogRow.V1",
                  dusr.first(184)) &&
              ReadHash(dusr, 216) ==
                  Evidence("ScratchBird.SblrDmlUpdateRowsSecurityPolicySnapshotRecord.V1",
                           dusr.first(216)) &&
              ReadHash(dusv, 72) ==
                  Evidence("ScratchBird.SblrDmlUpdateRowsSecurityPolicySnapshotVector.V1",
                           dusr),
          "DUSR source-row, record, and DUSV vector domains/materials are exact");

  const auto& dusp = fixture.snapshot_proof.exact_bytes;
  Require(dusp.size() == kTypedUpdateSecuritySnapshotProofBytes &&
              LoadLittle16(dusp.data() + 6) ==
                  kTypedUpdateSecuritySnapshotProofBytes &&
              ReadHash(dusp, 528) ==
                  Evidence("ScratchBird.SblrDmlUpdateRowsSecuritySnapshotProof.V1",
                           std::span<const byte>(dusp).first(528)) &&
              ReadHash(dusp, 432) == Hash(fixture.descriptor.exact_bytes) &&
              ReadHash(dusp, 464) ==
                  Hash(fixture.row_policies.exact_bytes) &&
              ReadHash(dusp, 496) == ReadHash(dusv, 72),
          "DUSP576 exact raw-carrier and domain-separated hashes are fixed");

  const auto& dumo = fixture.recovery_observation.exact_bytes;
  std::vector<byte> replay_material;
  AppendUuid(&replay_material,
             fixture.recovery_observation.database_uuid);
  AppendUuid(&replay_material,
             fixture.recovery_observation.descriptor_uuid);
  AppendLittle64(&replay_material,
                 fixture.recovery_observation.descriptor_generation);
  AppendUuid(&replay_material,
             fixture.recovery_observation.operation_uuid);
  AppendLittle64(&replay_material,
                 fixture.recovery_observation.operation_generation);
  AppendUuid(&replay_material,
             fixture.recovery_observation
                 .authenticated_statement_receipt_uuid);
  AppendUuid(&replay_material,
             fixture.recovery_observation.owning_transaction_uuid);
  AppendLittle64(&replay_material,
                 fixture.recovery_observation
                     .owning_local_transaction_id);
  AppendUuid(&replay_material,
             fixture.recovery_observation.recovery_token_uuid);
  AppendLittle64(&replay_material,
                 fixture.recovery_observation.recovery_generation);
  Require(dumo.size() == kTypedUpdateMgaRecoveryObservationBytes &&
              replay_material.size() == 128 &&
              ReadHash(dumo, 344) ==
                  Evidence("ScratchBird.SblrDmlUpdateRowsMgaRecoveryReplayIdentity.V1",
                           replay_material) &&
              ReadHash(dumo, 376) ==
                  Evidence("ScratchBird.SblrDmlUpdateRowsMgaRecoveryObservation.V1",
                           std::span<const byte>(dumo).first(376)),
          "DUMO416 replay and observation domains/materials are exact");

  std::vector<byte> bad = dusv;
  bad[kTypedUpdateVectorHeaderBytes + 6] = 1;
  bad[kTypedUpdateVectorHeaderBytes + 216] ^= 1;
  TypedUpdateSecurityPolicySourceVector decoded_source;
  Require(!DecodeAndValidateTypedUpdateSecurityPolicySourceVector(
              bad, &decoded_source, &error) &&
              error.code == TypedUpdateCarrierErrorCode::reserved_invalid &&
              error.diagnostic_code == "DML.UPDATE_FAILED",
          "DUSR reserved refusal precedes record/vector hash failures");
  bad = dusv;
  bad[kTypedUpdateVectorHeaderBytes + 216] ^= 1;
  RewriteVectorEvidence(
      &bad,
      "ScratchBird.SblrDmlUpdateRowsSecurityPolicySnapshotVector.V1");
  Require(!DecodeAndValidateTypedUpdateSecurityPolicySourceVector(
              bad, &decoded_source, &error) &&
              error.code ==
                  TypedUpdateCarrierErrorCode::record_evidence_mismatch,
          "DUSR record evidence is checked before DUSV outer evidence");
  bad = dusv;
  bad.push_back(0);
  Require(!DecodeAndValidateTypedUpdateSecurityPolicySourceVector(
              bad, &decoded_source, &error) &&
              error.code == TypedUpdateCarrierErrorCode::extent_invalid,
          "DUSV trailing bytes are refused");

  auto duplicate_source = fixture.source_policies;
  duplicate_source.exact_bytes.clear();
  duplicate_source.records.push_back(duplicate_source.records.front());
  duplicate_source.records.back().source_policy_ordinal = 2;
  duplicate_source.records.back().policy_version_uuid = Uuid(115);
  Require(!EncodeTypedUpdateSecurityPolicySourceVector(
              duplicate_source, &bad, &error) &&
              error.code == TypedUpdateCarrierErrorCode::
                                security_policy_source_duplicate,
          "DUSV duplicate phase/policy/generation key is refused");

  bad = dusp;
  bad[361] = 1;
  bad[528] ^= 1;
  TypedUpdateSecuritySnapshotProof decoded_proof;
  Require(!DecodeAndValidateTypedUpdateSecuritySnapshotProof(
              bad, &decoded_proof, &error) &&
              error.code == TypedUpdateCarrierErrorCode::reserved_invalid,
          "DUSP reserved refusal precedes outer evidence failure");
  bad = dusp;
  bad[528] ^= 1;
  Require(!DecodeAndValidateTypedUpdateSecuritySnapshotProof(
              bad, &decoded_proof, &error) &&
              error.code == TypedUpdateCarrierErrorCode::
                                security_snapshot_evidence_mismatch,
          "DUSP evidence mismatch is field-specific");
  bad = dusp;
  bad.push_back(0);
  Require(!DecodeAndValidateTypedUpdateSecuritySnapshotProof(
              bad, &decoded_proof, &error) &&
              error.code == TypedUpdateCarrierErrorCode::extent_invalid,
          "DUSP trailing bytes are refused");

  bad = dumo;
  bad[195] = 2;
  bad[376] ^= 1;
  TypedUpdateMgaRecoveryObservation decoded_observation;
  Require(!DecodeAndValidateTypedUpdateMgaRecoveryObservation(
              bad, &decoded_observation, &error) &&
              error.code == TypedUpdateCarrierErrorCode::
                                recovery_observation_invalid,
          "DUMO exact-boolean refusal precedes observation evidence");
  bad = dumo;
  bad[344] ^= 1;
  WriteHash(&bad, 376,
            Evidence("ScratchBird.SblrDmlUpdateRowsMgaRecoveryObservation.V1",
                     std::span<const byte>(bad).first(376)));
  Require(!DecodeAndValidateTypedUpdateMgaRecoveryObservation(
              bad, &decoded_observation, &error) &&
              error.code ==
                  TypedUpdateCarrierErrorCode::replay_identity_mismatch,
          "DUMO replay identity is checked independently of outer evidence");
  bad = dumo;
  bad.push_back(0);
  Require(!DecodeAndValidateTypedUpdateMgaRecoveryObservation(
              bad, &decoded_observation, &error) &&
              error.code == TypedUpdateCarrierErrorCode::extent_invalid,
          "DUMO trailing bytes are refused");

  TypedUpdateSecurityPolicySourceVector empty_source;
  empty_source.identity = VectorIdentity(116, fixture.descriptor);
  Require(EncodeTypedUpdateSecurityPolicySourceVector(
              empty_source, &bad, &error) &&
              bad.size() == kTypedUpdateVectorHeaderBytes &&
              ReadHash(bad, 72) ==
                  Evidence("ScratchBird.SblrDmlUpdateRowsSecurityPolicySnapshotVector.V1",
                           std::span<const byte>()),
          "empty DUSV has a nonzero identity and domain-only vector hash");
  auto empty_proof = fixture.snapshot_proof;
  empty_proof.row_policy_count = 0;
  empty_proof.source_policy_count = 0;
  empty_proof.source_policy_vector_uuid = empty_source.identity.vector_uuid;
  empty_proof.source_policy_vector_generation =
      empty_source.identity.vector_generation;
  empty_proof.source_policy_catalog_vector_sha256 = ReadHash(bad, 72);
  Require(EncodeTypedUpdateSecuritySnapshotProof(empty_proof, &bad, &error),
          "both-empty DUSP policy counts are admitted");
  empty_proof.source_policy_count = 1;
  Require(!EncodeTypedUpdateSecuritySnapshotProof(empty_proof, &bad, &error) &&
              error.code ==
                  TypedUpdateCarrierErrorCode::security_snapshot_invalid,
          "DUSP one-empty/one-nonempty policy counts are refused");

  auto round_trip_proof = [&](TypedUpdateSecuritySnapshotProof value) {
    std::vector<byte> encoded;
    Require(EncodeTypedUpdateSecuritySnapshotProof(value, &encoded, &error),
            "mutated DUSP encode: " + error.detail);
    TypedUpdateSecuritySnapshotProof decoded;
    Require(DecodeAndValidateTypedUpdateSecuritySnapshotProof(
                encoded, &decoded, &error),
            "mutated DUSP decode: " + error.detail);
    return decoded;
  };
  auto wrong_epoch = fixture.snapshot_proof;
  ++wrong_epoch.security_epoch;
  wrong_epoch = round_trip_proof(std::move(wrong_epoch));
  Require(!ValidateTypedUpdateSecurityRecoveryAuthority(
              fixture.descriptor, fixture.row_policies,
              fixture.recovery_token, fixture.source_policies, wrong_epoch,
              fixture.journal_head, fixture.recovery_observation, &error) &&
              error.code == TypedUpdateCarrierErrorCode::
                                security_recovery_binding_mismatch,
          "individually valid DUSP with cross-bound security epoch is refused");
  auto wrong_dudc_hash = fixture.snapshot_proof;
  wrong_dudc_hash.exact_dudc_sha256[0] ^= 1;
  wrong_dudc_hash = round_trip_proof(std::move(wrong_dudc_hash));
  Require(!ValidateTypedUpdateSecurityRecoveryAuthority(
              fixture.descriptor, fixture.row_policies,
              fixture.recovery_token, fixture.source_policies,
              wrong_dudc_hash, fixture.journal_head,
              fixture.recovery_observation, &error) &&
              error.code ==
                  TypedUpdateCarrierErrorCode::exact_byte_hash_mismatch,
          "DUSP wrong raw DUDC SHA is refused after structural binding");

  auto round_trip_observation =
      [&](TypedUpdateMgaRecoveryObservation value) {
        std::vector<byte> encoded;
        Require(EncodeTypedUpdateMgaRecoveryObservation(value, &encoded,
                                                         &error),
                "mutated DUMO encode: " + error.detail);
        TypedUpdateMgaRecoveryObservation decoded;
        Require(DecodeAndValidateTypedUpdateMgaRecoveryObservation(
                    encoded, &decoded, &error),
                "mutated DUMO decode: " + error.detail);
        return decoded;
      };
  auto wrong_handle = fixture.recovery_observation;
  wrong_handle.validated_mga_durable_handle_uuid = Uuid(117);
  wrong_handle = round_trip_observation(std::move(wrong_handle));
  Require(!ValidateTypedUpdateSecurityRecoveryAuthority(
              fixture.descriptor, fixture.row_policies,
              fixture.recovery_token, fixture.source_policies,
              fixture.snapshot_proof, fixture.journal_head, wrong_handle,
              &error) &&
              error.code == TypedUpdateCarrierErrorCode::
                                security_recovery_binding_mismatch,
          "individually valid cross-bound MGA durable handle is refused");

  TypedUpdateRecoveryDecision decision;
  Require(DecideTypedUpdateRecovery(fixture.recovery_observation, &decision,
                                    &error) &&
              decision ==
                  TypedUpdateRecoveryDecision::append_aborted_no_result,
          "active bound DUMO selects abort cleanup");
  auto prepared_active = fixture.recovery_observation;
  prepared_active.latest_journal_state = TypedUpdateJournalState::prepared;
  prepared_active.savepoint_state = TypedUpdateSavepointState::active;
  prepared_active.statement_savepoint_uuid = Uuid(118);
  prepared_active.statement_savepoint_generation = 119;
  prepared_active.no_surviving_effect_proven = false;
  prepared_active = round_trip_observation(std::move(prepared_active));
  Require(DecideTypedUpdateRecovery(prepared_active, &decision, &error) &&
              decision ==
                  TypedUpdateRecoveryDecision::append_aborted_no_result,
          "active prepared prebarrier DUMO selects rollback and abort");
  auto prepared_rolled_back = prepared_active;
  prepared_rolled_back.savepoint_state =
      TypedUpdateSavepointState::rolled_back_final;
  prepared_rolled_back.no_surviving_effect_proven = true;
  prepared_rolled_back =
      round_trip_observation(std::move(prepared_rolled_back));
  Require(DecideTypedUpdateRecovery(prepared_rolled_back, &decision, &error) &&
              decision ==
                  TypedUpdateRecoveryDecision::append_aborted_no_result,
          "rolled-back prepared prebarrier DUMO selects abort cleanup");
  auto prepared_released = prepared_active;
  prepared_released.savepoint_state =
      TypedUpdateSavepointState::released_at_statement_barrier;
  prepared_released.statement_barrier_present = true;
  prepared_released =
      round_trip_observation(std::move(prepared_released));
  Require(DecideTypedUpdateRecovery(prepared_released, &decision, &error) &&
              decision == TypedUpdateRecoveryDecision::
                                append_published_and_replay_result,
          "active prepared postbarrier DUMO publishes then replays");
  auto published = prepared_released;
  published.latest_journal_state = TypedUpdateJournalState::published;
  published = round_trip_observation(std::move(published));
  Require(DecideTypedUpdateRecovery(published, &decision, &error) &&
              decision ==
                  TypedUpdateRecoveryDecision::replay_published_result,
          "active published DUMO replays exact prior result");
  auto ended_prepared = prepared_active;
  ended_prepared.transaction_state =
      TypedUpdateTransactionState::rolled_back_final;
  ended_prepared = round_trip_observation(std::move(ended_prepared));
  Require(DecideTypedUpdateRecovery(ended_prepared, &decision, &error) &&
              decision ==
                  TypedUpdateRecoveryDecision::append_aborted_no_result,
          "ended prepared DUMO still selects legal prepared-to-aborted cleanup");
  auto ended_published = published;
  ended_published.transaction_state =
      TypedUpdateTransactionState::committed_final;
  ended_published = round_trip_observation(std::move(ended_published));
  Require(!DecideTypedUpdateRecovery(ended_published, &decision, &error) &&
              decision == TypedUpdateRecoveryDecision::stale_replay &&
              error.diagnostic_code == "MGA.TRANSACTION.STALE",
          "ended published DUMO cannot authorize result replay");
  auto quarantined = fixture.recovery_observation;
  quarantined.transaction_state = TypedUpdateTransactionState::quarantined;
  quarantined = round_trip_observation(std::move(quarantined));
  Require(!DecideTypedUpdateRecovery(quarantined, &decision, &error) &&
              decision ==
                  TypedUpdateRecoveryDecision::quarantine_update_failed &&
              error.diagnostic_code == "DML.UPDATE_FAILED",
          "quarantined DUMO remains fail-closed");
  auto contradictory = prepared_active;
  contradictory.statement_barrier_present = true;
  Require(!EncodeTypedUpdateMgaRecoveryObservation(
              contradictory, &bad, &error) &&
              error.code == TypedUpdateCarrierErrorCode::
                                recovery_observation_invalid,
          "DUMO contradictory active-savepoint/barrier state is refused before decision");
  auto rolled_back_without_proof = prepared_rolled_back;
  rolled_back_without_proof.no_surviving_effect_proven = false;
  Require(!EncodeTypedUpdateMgaRecoveryObservation(
              rolled_back_without_proof, &bad, &error) &&
              error.code == TypedUpdateCarrierErrorCode::
                                recovery_observation_invalid,
          "prepared rolled-back DUMO without no-surviving-effect proof is refused");
  auto rolled_back_with_barrier = prepared_rolled_back;
  rolled_back_with_barrier.statement_barrier_present = true;
  Require(!EncodeTypedUpdateMgaRecoveryObservation(
              rolled_back_with_barrier, &bad, &error) &&
              error.code == TypedUpdateCarrierErrorCode::
                                recovery_observation_invalid,
          "prepared rolled-back DUMO with a statement barrier is refused");
  auto missing_exact = fixture.recovery_observation;
  missing_exact.exact_bytes.clear();
  Require(!DecideTypedUpdateRecovery(missing_exact, &decision, &error) &&
              decision ==
                  TypedUpdateRecoveryDecision::quarantine_update_failed,
          "recovery decision refuses a caller-projected DUMO without exact bytes");
}

void TestDatatypeOperatorAuthorityCarriers() {
  auto canonical_true = MakeDatatypeOperatorFixture(false);
  TypedUpdateCarrierError error;
  Require(ValidateTypedUpdateDatatypeOperatorAuthority(
              canonical_true.descriptor, canonical_true.assignments,
              canonical_true.predicate, canonical_true.datatypes,
              canonical_true.operators, &error),
          "DUDV/empty-DUOV canonical TRUE authority validates: " +
              error.detail);

  const auto& dudv = canonical_true.datatypes.exact_bytes;
  Require(dudv.size() == kTypedUpdateVectorHeaderBytes +
                             2 * kTypedUpdateDatatypeAuthorityRecordBytes &&
              std::equal(dudv.begin(), dudv.begin() + 4,
                         std::string_view("DUDV").begin()) &&
              LoadLittle32(dudv.data() + 64) == 2 &&
              LoadLittle32(dudv.data() + 68) ==
                  2 * kTypedUpdateDatatypeAuthorityRecordBytes,
          "DUDV has exact canonical header and two DUDR256 records");
  const auto boolean_record = std::span<const byte>(dudv).subspan(
      kTypedUpdateVectorHeaderBytes,
      kTypedUpdateDatatypeAuthorityRecordBytes);
  const auto bigint_record = std::span<const byte>(dudv).subspan(
      kTypedUpdateVectorHeaderBytes +
          kTypedUpdateDatatypeAuthorityRecordBytes,
      kTypedUpdateDatatypeAuthorityRecordBytes);
  Require(boolean_record[4] == 1 && boolean_record[5] == 1 &&
              boolean_record[6] == 1 && boolean_record[7] == 0 &&
              LoadLittle32(boolean_record.data() + 72) == 1 &&
              LoadLittle32(bigint_record.data() + 72) == 8 &&
              ReadHash(boolean_record, 216) ==
                  Evidence("ScratchBird.SblrDmlUpdateRowsDatatypeAuthorityRecord.V1",
                           boolean_record.first(216)) &&
              ReadHash(bigint_record, 216) ==
                  Evidence("ScratchBird.SblrDmlUpdateRowsDatatypeAuthorityRecord.V1",
                           bigint_record.first(216)) &&
              ReadHash(dudv, 72) ==
                  Evidence("ScratchBird.SblrDmlUpdateRowsDatatypeAuthorityVector.V1",
                           std::span<const byte>(dudv).subspan(
                               kTypedUpdateVectorHeaderBytes)) &&
              canonical_true.datatypes.records[0].exact_bytes ==
                  std::vector<byte>(boolean_record.begin(),
                                    boolean_record.end()) &&
              canonical_true.datatypes.records[1].exact_bytes ==
                  std::vector<byte>(bigint_record.begin(),
                                    bigint_record.end()),
          "DUDR fixed widths, row domains, vector domain, and retained exact bytes are canonical");

  const auto& empty_duov = canonical_true.operators.exact_bytes;
  Require(empty_duov.size() == kTypedUpdateVectorHeaderBytes &&
              LoadLittle32(empty_duov.data() + 64) == 0 &&
              ReadHash(empty_duov, 72) ==
                  Evidence("ScratchBird.SblrDmlUpdateRowsBuiltinOperatorAuthorityVector.V1",
                           std::span<const byte>()),
          "canonical TRUE DUOV is exact empty domain-only authority");

  std::vector<byte> bad = dudv;
  bad[kTypedUpdateVectorHeaderBytes + 120 + 7] = 'x';
  TypedUpdateDatatypeAuthorityVector decoded_datatypes;
  Require(!DecodeAndValidateTypedUpdateDatatypeAuthorityVector(
              bad, &decoded_datatypes, &error) &&
              error.code == TypedUpdateCarrierErrorCode::fixed_text_invalid &&
              error.diagnostic_code == "DML.UPDATE_FAILED",
          "DUDR fixed-text padding refusal precedes evidence checks");
  bad = dudv;
  bad[kTypedUpdateVectorHeaderBytes + 216] ^= 1;
  RewriteVectorEvidence(
      &bad,
      "ScratchBird.SblrDmlUpdateRowsDatatypeAuthorityVector.V1");
  Require(!DecodeAndValidateTypedUpdateDatatypeAuthorityVector(
              bad, &decoded_datatypes, &error) &&
              error.code ==
                  TypedUpdateCarrierErrorCode::record_evidence_mismatch,
          "DUDR evidence is checked independently before DUDV evidence");
  bad = dudv;
  bad.push_back(0);
  Require(!DecodeAndValidateTypedUpdateDatatypeAuthorityVector(
              bad, &decoded_datatypes, &error) &&
              error.code == TypedUpdateCarrierErrorCode::extent_invalid,
          "DUDV trailing bytes are refused");

  auto reversed = canonical_true.datatypes;
  reversed.exact_bytes.clear();
  reversed.records = {BigintDatatypeAuthority(1),
                      BooleanDatatypeAuthority(2)};
  Require(!EncodeTypedUpdateDatatypeAuthorityVector(reversed, &bad, &error) &&
              error.code ==
                  TypedUpdateCarrierErrorCode::datatype_authority_duplicate,
          "DUDV noncanonical descriptor/type order is refused");
  auto duplicate = canonical_true.datatypes;
  duplicate.exact_bytes.clear();
  duplicate.records = {BooleanDatatypeAuthority(1),
                       BooleanDatatypeAuthority(2)};
  Require(!EncodeTypedUpdateDatatypeAuthorityVector(duplicate, &bad, &error) &&
              error.code ==
                  TypedUpdateCarrierErrorCode::datatype_authority_duplicate,
          "DUDV duplicate datatype identity is refused");

  auto equality = MakeDatatypeOperatorFixture(true);
  Require(ValidateTypedUpdateDatatypeOperatorAuthority(
              equality.descriptor, equality.assignments,
              equality.predicate, equality.datatypes,
              equality.operators, &error),
          "DUDV/DUOE equality authority validates: " + error.detail);
  const auto& duov = equality.operators.exact_bytes;
  const auto duoe = std::span<const byte>(duov).subspan(
      kTypedUpdateVectorHeaderBytes,
      kTypedUpdateBuiltinOperatorAuthorityRecordBytes);
  Require(duov.size() == kTypedUpdateVectorHeaderBytes +
                             kTypedUpdateBuiltinOperatorAuthorityRecordBytes &&
              LoadLittle32(duov.data() + 64) == 1 &&
              duoe[4] == 1 && duoe[5] == 2 && duoe[6] == 1 &&
              duoe[7] == 1 && duoe[244] == 1 && duoe[245] == 1 &&
              ReadHash(duoe, 248) ==
                  Evidence("ScratchBird.SblrDmlUpdateRowsBuiltinOperatorAuthorityRecord.V1",
                           duoe.first(248)) &&
              ReadHash(duov, 72) ==
                  Evidence("ScratchBird.SblrDmlUpdateRowsBuiltinOperatorAuthorityVector.V1",
                           duoe) &&
              equality.operators.records[0].exact_bytes ==
                  std::vector<byte>(duoe.begin(), duoe.end()),
          "DUOE288 identity codes, record/vector domains, and retained exact bytes are canonical");
  bad = duov;
  bad[kTypedUpdateVectorHeaderBytes + 4] = 2;
  TypedUpdateBuiltinOperatorAuthorityVector decoded_operators;
  Require(!DecodeAndValidateTypedUpdateBuiltinOperatorAuthorityVector(
              bad, &decoded_operators, &error) &&
              error.code == TypedUpdateCarrierErrorCode::
                                builtin_operator_authority_invalid,
          "DUOE wrong semantic is refused before evidence verification");
  bad = duov;
  bad.push_back(0);
  Require(!DecodeAndValidateTypedUpdateBuiltinOperatorAuthorityVector(
              bad, &decoded_operators, &error) &&
              error.code == TypedUpdateCarrierErrorCode::extent_invalid,
          "DUOV trailing bytes are refused");

  auto missing_datatype = canonical_true.datatypes;
  missing_datatype.exact_bytes.clear();
  missing_datatype.records = {BooleanDatatypeAuthority(1)};
  Require(EncodeTypedUpdateDatatypeAuthorityVector(
              missing_datatype, &bad, &error) &&
              DecodeAndValidateTypedUpdateDatatypeAuthorityVector(
                  bad, &missing_datatype, &error),
          "individually valid missing-row DUDV fixture");
  Require(!ValidateTypedUpdateDatatypeOperatorAuthority(
              canonical_true.descriptor, canonical_true.assignments,
              canonical_true.predicate, missing_datatype,
              canonical_true.operators, &error) &&
              error.code == TypedUpdateCarrierErrorCode::
                                datatype_operator_binding_mismatch,
          "DUDV missing referenced datatype row is cross-refused");

  auto unreferenced_datatype = canonical_true.datatypes;
  unreferenced_datatype.exact_bytes.clear();
  unreferenced_datatype.records.push_back(Int32DatatypeAuthority(3));
  Require(EncodeTypedUpdateDatatypeAuthorityVector(
              unreferenced_datatype, &bad, &error) &&
              DecodeAndValidateTypedUpdateDatatypeAuthorityVector(
                  bad, &unreferenced_datatype, &error),
          "individually valid unreferenced-row DUDV fixture");
  Require(!ValidateTypedUpdateDatatypeOperatorAuthority(
              canonical_true.descriptor, canonical_true.assignments,
              canonical_true.predicate, unreferenced_datatype,
              canonical_true.operators, &error) &&
              error.field == "unreferenced_datatype",
          "DUDV unreferenced datatype row is cross-refused");

  Require(!ValidateTypedUpdateDatatypeOperatorAuthority(
              canonical_true.descriptor, canonical_true.assignments,
              canonical_true.predicate, canonical_true.datatypes,
              equality.operators, &error) &&
              error.field == "operator_inclusion",
          "canonical TRUE with unreferenced DUOE is refused");
  Require(!ValidateTypedUpdateDatatypeOperatorAuthority(
              equality.descriptor, equality.assignments,
              equality.predicate, equality.datatypes,
              canonical_true.operators, &error) &&
              error.field == "operator_inclusion",
          "equality DUEV without DUOE is refused");

  auto wrong_operands = equality.operators;
  wrong_operands.exact_bytes.clear();
  auto& wrong_row = wrong_operands.records.front();
  wrong_row.exact_bytes.clear();
  wrong_row.left_descriptor_uuid = kTypedUpdateBooleanUuid;
  wrong_row.left_descriptor_generation = 1;
  wrong_row.left_type_uuid = kTypedUpdateBooleanUuid;
  wrong_row.left_type_generation = 1;
  wrong_row.right_descriptor_uuid = kTypedUpdateBooleanUuid;
  wrong_row.right_descriptor_generation = 1;
  wrong_row.right_type_uuid = kTypedUpdateBooleanUuid;
  wrong_row.right_type_generation = 1;
  Require(EncodeTypedUpdateBuiltinOperatorAuthorityVector(
              wrong_operands, &bad, &error) &&
              DecodeAndValidateTypedUpdateBuiltinOperatorAuthorityVector(
                  bad, &wrong_operands, &error),
          "individually valid cross-bound DUOE operand fixture");
  Require(!ValidateTypedUpdateDatatypeOperatorAuthority(
              equality.descriptor, equality.assignments,
              equality.predicate, equality.datatypes, wrong_operands,
              &error) &&
              error.field == "DUEV_binding",
          "DUOE operand identities must equal exact DUEV children");

  auto missing_exact = canonical_true.datatypes;
  missing_exact.records.front().exact_bytes.clear();
  Require(!ValidateTypedUpdateDatatypeOperatorAuthority(
              canonical_true.descriptor, canonical_true.assignments,
              canonical_true.predicate, missing_exact,
              canonical_true.operators, &error) &&
              error.field == "records.exact_bytes",
          "cross-validator refuses caller-projected DUDR without exact bytes");

  auto short_assignment = canonical_true.assignments;
  short_assignment.exact_bytes.clear();
  short_assignment.records.front().canonical_value.pop_back();
  Require(EncodeTypedUpdateAssignmentVector(short_assignment, &bad, &error),
          "short canonical assignment remains structurally encodable");
  TypedUpdateAssignmentVector decoded_short_assignment;
  Require(DecodeAndValidateTypedUpdateAssignmentVector(
              bad, &decoded_short_assignment, &error),
          "short canonical assignment remains structurally decodable");
  auto short_descriptor = canonical_true.descriptor;
  short_descriptor.exact_bytes.clear();
  short_descriptor.assignment_vector_sha256 = ReadHash(bad, 72);
  Require(EncodeTypedUpdateDescriptor(short_descriptor, &bad, &error),
          "short canonical assignment DUDC rebind encode");
  Require(DecodeAndValidateTypedUpdateDescriptor(
              bad, &short_descriptor, &error),
          "short canonical assignment DUDC rebind decode");
  Require(!ValidateTypedUpdateDatatypeOperatorAuthority(
              short_descriptor, decoded_short_assignment,
              canonical_true.predicate, canonical_true.datatypes,
              canonical_true.operators, &error) &&
              error.field == "canonical_value",
          "DUDR exact width rejects structurally valid short canonical value");

  Require(std::string_view(TypedUpdateCarrierErrorCodeName(
              TypedUpdateCarrierErrorCode::fixed_text_invalid)) ==
              "fixed_text_invalid" &&
              std::string_view(TypedUpdateCarrierErrorCodeName(
                  TypedUpdateCarrierErrorCode::
                      datatype_operator_binding_mismatch)) ==
                  "datatype_operator_binding_mismatch",
          "new datatype/operator errors have stable field names");
}

void TestCarrierSetContradictions() {
  auto set = CarrierSet();
  TypedUpdateCarrierError error;
  set.descriptor.assignment_count = 2;
  Require(!ValidateTypedUpdateCarrierSet(set, &error) &&
              error.code ==
                  TypedUpdateCarrierErrorCode::carrier_set_mismatch,
          "descriptor/vector count contradiction is refused");
  set = CarrierSet();
  set.target_order.maximum_candidate_rows = 99;
  Require(!ValidateTypedUpdateCarrierSet(set, &error) &&
              error.code ==
                  TypedUpdateCarrierErrorCode::resource_limit_exceeded,
          "DUOR/DUBR candidate limit contradiction is refused");
}

}  // namespace

int main() {
  static_assert(kTypedUpdateDescriptorBytes == 712);
  static_assert(kTypedUpdateAssignmentPrefixBytes == 180);
  static_assert(kTypedUpdatePredicatePrefixBytes == 252);
  static_assert(kTypedUpdatePredicatePrefixBytes +
                    kTypedUpdatePredicateEvidenceBytes ==
                284);
  static_assert(kTypedUpdateJournalWithoutResultBytes == 256 + 712);
  static_assert(kTypedUpdateJournalWithResultBytes == 256 + 712 + 256);
  static_assert(kTypedUpdateSecurityPolicySourceRecordBytes == 256);
  static_assert(kTypedUpdateSecuritySnapshotProofBytes == 576);
  static_assert(kTypedUpdateMgaRecoveryObservationBytes == 416);
  static_assert(kTypedUpdateDatatypeAuthorityRecordBytes == 256);
  static_assert(kTypedUpdateBuiltinOperatorAuthorityRecordBytes == 288);
  TestRoundTripsAndDomains();
  TestExplicitNullAndInjectivity();
  TestResultInnerEvidence();
  TestEmptyFrozenSets();
  TestMalformedPrecedence();
  TestJournalAndCutpoints();
  TestSecurityRecoveryCarriers();
  TestDatatypeOperatorAuthorityCarriers();
  TestCarrierSetContradictions();
  std::cout << "PASS typed update carrier codec exact layouts, hashes, "
               "injectivity, malformed precedence, journal, security "
               "snapshot, MGA recovery, datatype, and operator authority\n";
  return 0;
}
