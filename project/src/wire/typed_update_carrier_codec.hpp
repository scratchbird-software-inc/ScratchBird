// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// Exact component codec for SBLR-DML-UPDATE-ROWS-DESCRIPTOR-V1.  SHA-256
// fields are deterministic evidence, not authentication or authorization.
// A caller must still resolve every decoded identity against the authenticated
// statement receipt, MGA inventory, and live Core registries.

#include "runtime_platform.hpp"

#include <array>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace scratchbird::wire {

using scratchbird::core::platform::byte;
using scratchbird::core::platform::u8;
using scratchbird::core::platform::u16;
using scratchbird::core::platform::u32;
using scratchbird::core::platform::u64;

using TypedUpdateUuid = std::array<byte, 16>;
using TypedUpdateHash = std::array<byte, 32>;

inline constexpr u16 kTypedUpdateCarrierVersion = 1;
inline constexpr u32 kTypedUpdateDescriptorBytes = 712;
inline constexpr u32 kTypedUpdateVectorHeaderBytes = 104;
inline constexpr u32 kTypedUpdateAssignmentPrefixBytes = 180;
inline constexpr u32 kTypedUpdatePredicatePrefixBytes = 252;
inline constexpr u32 kTypedUpdatePredicateEvidenceBytes = 32;
inline constexpr u32 kTypedUpdateRowPolicyRecordBytes = 176;
inline constexpr u32 kTypedUpdateConstraintRecordBytes = 160;
inline constexpr u32 kTypedUpdateTriggerRecordBytes = 192;
inline constexpr u32 kTypedUpdateTargetOrderBytes = 160;
inline constexpr u32 kTypedUpdateResourceBudgetBytes = 208;
inline constexpr u32 kTypedUpdateRecoveryTokenBytes = 208;
inline constexpr u32 kTypedUpdateResultBytes = 256;
inline constexpr u32 kTypedUpdateJournalHeaderBytes = 256;
inline constexpr u32 kTypedUpdateJournalWithoutResultBytes = 968;
inline constexpr u32 kTypedUpdateJournalWithResultBytes = 1224;
inline constexpr u32 kTypedUpdateSecurityPolicySourceRecordBytes = 256;
inline constexpr u32 kTypedUpdateSecuritySnapshotProofBytes = 576;
inline constexpr u32 kTypedUpdateMgaRecoveryObservationBytes = 416;
inline constexpr u32 kTypedUpdateDatatypeAuthorityRecordBytes = 256;
inline constexpr u32 kTypedUpdateBuiltinOperatorAuthorityRecordBytes = 288;
inline constexpr u32 kTypedUpdateMaximumAssignments = 1024;
inline constexpr u32 kTypedUpdateMaximumPredicateNodes = 4096;
inline constexpr u32 kTypedUpdateMaximumCandidateRows = 1048576;
inline constexpr u32 kTypedUpdateMaximumFrozenRecords = 1048576;
inline constexpr u32 kTypedUpdateMaximumTriggerDepth = 64;
inline constexpr u32 kTypedUpdateMaximumEffects = 1048576;
inline constexpr u32 kTypedUpdateMaximumCanonicalValueBytesPerValue = 65536;
inline constexpr u64 kTypedUpdateMaximumCanonicalValueBytes = 16777216;

inline constexpr TypedUpdateUuid kTypedUpdateBooleanUuid{{
    0x01, 0x00, 0x00, 0x00, 0x62, 0x6f, 0x7f, 0x6c,
    0xa5, 0x61, 0x6e, 0x00, 0x00, 0x00, 0x00, 0x00}};
inline constexpr TypedUpdateUuid kTypedUpdateEqualOperatorUuid{{
    0x01, 0x9d, 0xe5, 0xfc, 0x24, 0x00, 0x7b, 0x73,
    0x9c, 0x38, 0xdc, 0xf1, 0x02, 0x04, 0xdb, 0xde}};
inline constexpr TypedUpdateUuid kTypedUpdateOperatorSnapshotUuid{{
    0x01, 0x9d, 0x00, 0x00, 0x00, 0x00, 0x70, 0x00,
    0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0xd7, 0x20}};
inline constexpr TypedUpdateUuid kTypedUpdateDatatypeSnapshotUuid{{
    0x01, 0x9d, 0x00, 0x00, 0x00, 0x00, 0x70, 0x00,
    0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0xd7, 0x01}};

enum class TypedUpdateCarrierKind : u8 {
  descriptor,
  assignment_vector,
  predicate_vector,
  row_policy_vector,
  constraint_vector,
  trigger_vector,
  target_order,
  resource_budget,
  recovery_token,
  result,
  journal,
  security_policy_source_vector,
  security_snapshot_proof,
  mga_recovery_observation,
  datatype_authority_vector,
  builtin_operator_authority_vector,
};

enum class TypedUpdateCarrierErrorCode : u16 {
  ok = 0,
  invalid_argument,
  extent_invalid,
  magic_invalid,
  version_invalid,
  flags_invalid,
  reserved_invalid,
  uuid_invalid,
  generation_invalid,
  count_invalid,
  ordinal_invalid,
  duplicate_occurrence,
  duplicate_target,
  codec_id_invalid,
  value_state_invalid,
  canonical_value_invalid,
  canonical_value_hash_mismatch,
  record_evidence_mismatch,
  vector_evidence_mismatch,
  descriptor_evidence_mismatch,
  ownership_mismatch,
  predicate_shape_invalid,
  boolean_identity_invalid,
  operator_identity_invalid,
  frozen_set_invalid,
  target_order_invalid,
  resource_limit_exceeded,
  recovery_identity_invalid,
  result_invalid,
  result_evidence_material_invalid,
  result_evidence_mismatch,
  journal_state_invalid,
  journal_transition_invalid,
  journal_sequence_invalid,
  journal_chain_mismatch,
  journal_evidence_mismatch,
  carrier_set_mismatch,
  recovery_authority_invalid,
  recovery_replay_stale,
  security_policy_source_invalid,
  security_policy_source_duplicate,
  security_snapshot_invalid,
  security_snapshot_evidence_mismatch,
  exact_byte_hash_mismatch,
  recovery_observation_invalid,
  replay_identity_mismatch,
  security_recovery_binding_mismatch,
  fixed_text_invalid,
  datatype_authority_invalid,
  datatype_authority_duplicate,
  builtin_operator_authority_invalid,
  datatype_operator_binding_mismatch,
  hash_failure,
};

struct TypedUpdateCarrierError {
  TypedUpdateCarrierErrorCode code = TypedUpdateCarrierErrorCode::ok;
  std::string diagnostic_code;
  TypedUpdateCarrierKind carrier = TypedUpdateCarrierKind::descriptor;
  std::string field;
  u32 record_index = 0;
  std::string detail;

  bool ok() const { return code == TypedUpdateCarrierErrorCode::ok; }
};

enum class TypedUpdateValueState : u8 {
  absent = 0,
  value = 1,
  null_value = 2,
};

enum class TypedUpdatePredicateNodeKind : u8 {
  column_reference = 1,
  typed_literal = 2,
  comparison = 3,
  canonical_boolean_constant = 9,
};

enum class TypedUpdateJournalState : u8 {
  bound = 1,
  intent = 2,
  prepared = 3,
  published = 4,
  aborted = 5,
};

enum class TypedUpdateTransactionState : u8 {
  active_live = 1,
  committed_final = 2,
  rolled_back_final = 3,
  dead_or_abandoned = 4,
  quarantined = 5,
};

enum class TypedUpdateSavepointState : u8 {
  absent = 0,
  active = 1,
  rolled_back_final = 2,
  released_at_statement_barrier = 3,
};

enum class TypedUpdateDatatypeIdentityCode : u8 {
  boolean_v1 = 1,
  int32_v1 = 2,
  bigint_v1 = 3,
  decimal_v1 = 4,
  int128_v1 = 5,
};

enum class TypedUpdateNullEncodingCode : u8 {
  containing_slot_value_or_null_state = 1,
  unsupported_in_sblr_literal_v1 = 2,
};

enum class TypedUpdateByteOrderCode : u8 {
  single_byte = 1,
  little_endian = 2,
};

enum class TypedUpdateRepresentationCode : u8 {
  canonical_boolean = 1,
  twos_complement = 2,
  decimal_base1e9 = 3,
};

struct TypedUpdateVectorIdentity {
  TypedUpdateUuid vector_uuid{};
  u64 vector_generation = 0;
  TypedUpdateUuid owner_descriptor_uuid{};
  u64 owner_descriptor_generation = 0;
  TypedUpdateHash vector_sha256{};
};

struct TypedUpdateDescriptorCarrier {
  std::vector<byte> exact_bytes;
  TypedUpdateUuid descriptor_uuid{};
  u64 descriptor_generation = 0;
  TypedUpdateUuid authenticated_statement_receipt_uuid{};
  u64 structural_occurrence_id = 0;
  TypedUpdateUuid operation_uuid{};
  u64 operation_generation = 0;
  TypedUpdateUuid owning_transaction_uuid{};
  u64 owning_local_transaction_id = 0;
  TypedUpdateUuid statement_snapshot_uuid{};
  TypedUpdateUuid catalog_snapshot_uuid{};
  u64 catalog_generation = 0;
  u64 datatype_registry_generation = 0;
  TypedUpdateUuid security_context_uuid{};
  TypedUpdateUuid security_snapshot_uuid{};
  u64 security_generation = 0;
  TypedUpdateUuid target_relation_uuid{};
  u64 target_relation_generation = 0;
  TypedUpdateUuid target_relation_occurrence_uuid{};
  u64 target_relation_occurrence_generation = 0;
  TypedUpdateUuid assignment_vector_uuid{};
  u64 assignment_vector_generation = 0;
  u32 assignment_count = 0;
  TypedUpdateHash assignment_vector_sha256{};
  TypedUpdateUuid predicate_expression_uuid{};
  u64 predicate_expression_generation = 0;
  u64 predicate_root_node_id = 0;
  u32 predicate_node_count = 0;
  TypedUpdateHash predicate_vector_sha256{};
  TypedUpdateUuid row_policy_set_uuid{};
  u64 row_policy_set_generation = 0;
  u32 row_policy_count = 0;
  TypedUpdateHash row_policy_set_sha256{};
  TypedUpdateUuid constraint_set_uuid{};
  u64 constraint_set_generation = 0;
  u32 constraint_count = 0;
  TypedUpdateHash ordered_constraint_set_sha256{};
  TypedUpdateUuid trigger_set_uuid{};
  u64 trigger_set_generation = 0;
  u32 trigger_count = 0;
  TypedUpdateHash ordered_trigger_set_sha256{};
  TypedUpdateUuid deterministic_target_order_uuid{};
  u64 deterministic_target_order_generation = 0;
  TypedUpdateUuid resource_budget_uuid{};
  u64 resource_budget_generation = 0;
  TypedUpdateUuid recovery_token_uuid{};
  u64 recovery_generation = 0;
  u64 executor_availability_generation = 0;
  TypedUpdateUuid builtin_operator_snapshot_uuid{};
  u64 builtin_operator_registry_generation = 0;
  TypedUpdateHash descriptor_evidence_sha256{};
};

struct TypedUpdateAssignmentRecord {
  u32 assignment_ordinal = 0;
  TypedUpdateUuid assignment_occurrence_uuid{};
  u64 assignment_occurrence_generation = 0;
  TypedUpdateUuid target_column_occurrence_uuid{};
  u64 target_column_occurrence_generation = 0;
  TypedUpdateUuid target_column_uuid{};
  u64 target_column_generation = 0;
  TypedUpdateUuid value_descriptor_uuid{};
  u64 value_descriptor_generation = 0;
  TypedUpdateUuid value_type_uuid{};
  u64 value_type_generation = 0;
  std::string codec_id;
  u16 codec_version = 0;
  u64 codec_generation = 0;
  TypedUpdateValueState value_state = TypedUpdateValueState::absent;
  std::vector<byte> canonical_value;
  TypedUpdateHash canonical_value_sha256{};
};

struct TypedUpdateAssignmentVector {
  std::vector<byte> exact_bytes;
  TypedUpdateVectorIdentity identity;
  std::vector<TypedUpdateAssignmentRecord> records;
};

struct TypedUpdatePredicateRecord {
  u64 node_id = 0;
  TypedUpdateUuid node_occurrence_uuid{};
  u64 node_occurrence_generation = 0;
  TypedUpdatePredicateNodeKind node_kind =
      TypedUpdatePredicateNodeKind::column_reference;
  TypedUpdateValueState value_state = TypedUpdateValueState::absent;
  TypedUpdateUuid output_descriptor_uuid{};
  u64 output_descriptor_generation = 0;
  TypedUpdateUuid output_type_uuid{};
  u64 output_type_generation = 0;
  std::string output_codec_id;
  u16 output_codec_version = 0;
  u64 output_codec_generation = 0;
  u64 left_child_node_id = 0;
  u64 right_child_node_id = 0;
  TypedUpdateUuid referenced_relation_occurrence_uuid{};
  u64 referenced_relation_occurrence_generation = 0;
  TypedUpdateUuid referenced_column_occurrence_uuid{};
  u64 referenced_column_occurrence_generation = 0;
  TypedUpdateUuid referenced_column_uuid{};
  u64 referenced_column_generation = 0;
  TypedUpdateUuid operator_uuid{};
  u64 operator_generation = 0;
  std::vector<byte> canonical_value;
  TypedUpdateHash canonical_value_sha256{};
  TypedUpdateHash node_evidence_sha256{};
};

struct TypedUpdatePredicateVector {
  std::vector<byte> exact_bytes;
  TypedUpdateVectorIdentity identity;
  std::vector<TypedUpdatePredicateRecord> records;
};

struct TypedUpdateRowPolicyRecord {
  u32 policy_ordinal = 0;
  u8 phase = 0;
  TypedUpdateUuid effective_policy_uuid{};
  u64 effective_policy_generation = 0;
  TypedUpdateUuid expression_uuid{};
  u64 expression_generation = 0;
  TypedUpdateHash expression_evidence_sha256{};
  TypedUpdateUuid security_snapshot_uuid{};
  u64 security_generation = 0;
  TypedUpdateHash source_policy_catalog_vector_sha256{};
  TypedUpdateHash record_evidence_sha256{};
};

struct TypedUpdateRowPolicyVector {
  std::vector<byte> exact_bytes;
  TypedUpdateVectorIdentity identity;
  std::vector<TypedUpdateRowPolicyRecord> records;
};

struct TypedUpdateConstraintRecord {
  u32 constraint_ordinal = 0;
  u8 constraint_class = 0;
  u8 timing = 0;
  u8 reservation_mode = 0;
  TypedUpdateUuid constraint_uuid{};
  u64 constraint_generation = 0;
  TypedUpdateUuid expression_uuid{};
  u64 expression_generation = 0;
  TypedUpdateUuid reservation_profile_uuid{};
  u64 reservation_profile_generation = 0;
  TypedUpdateHash dependency_set_sha256{};
  TypedUpdateHash record_evidence_sha256{};
};

struct TypedUpdateConstraintVector {
  std::vector<byte> exact_bytes;
  TypedUpdateVectorIdentity identity;
  std::vector<TypedUpdateConstraintRecord> records;
};

struct TypedUpdateTriggerRecord {
  u32 trigger_ordinal = 0;
  u8 timing = 0;
  u8 security_mode = 0;
  TypedUpdateUuid trigger_uuid{};
  u64 trigger_generation = 0;
  TypedUpdateUuid body_sblr_uuid{};
  u64 body_sblr_generation = 0;
  TypedUpdateUuid execution_security_context_uuid{};
  u64 execution_security_generation = 0;
  TypedUpdateUuid recursion_profile_uuid{};
  u64 recursion_profile_generation = 0;
  u32 maximum_depth = 0;
  TypedUpdateHash dependency_set_sha256{};
  TypedUpdateHash record_evidence_sha256{};
};

struct TypedUpdateTriggerVector {
  std::vector<byte> exact_bytes;
  TypedUpdateVectorIdentity identity;
  std::vector<TypedUpdateTriggerRecord> records;
};

struct TypedUpdateTargetOrderCarrier {
  std::vector<byte> exact_bytes;
  TypedUpdateUuid target_order_uuid{};
  u64 target_order_generation = 0;
  TypedUpdateUuid authenticated_statement_receipt_uuid{};
  TypedUpdateUuid target_relation_occurrence_uuid{};
  u64 target_relation_occurrence_generation = 0;
  TypedUpdateUuid statement_snapshot_uuid{};
  u64 maximum_candidate_rows = 0;
  TypedUpdateHash evidence_sha256{};
};

struct TypedUpdateResourceBudgetCarrier {
  std::vector<byte> exact_bytes;
  TypedUpdateUuid resource_budget_uuid{};
  u64 resource_budget_generation = 0;
  TypedUpdateUuid authenticated_statement_receipt_uuid{};
  TypedUpdateUuid owning_transaction_uuid{};
  TypedUpdateUuid cancellation_token_uuid{};
  u64 cancellation_generation = 0;
  TypedUpdateUuid grant_receipt_uuid{};
  u64 grant_receipt_generation = 0;
  u32 maximum_assignments = 0;
  u32 maximum_predicate_nodes = 0;
  u64 maximum_candidate_rows = 0;
  u32 maximum_trigger_depth = 0;
  u32 maximum_effects = 0;
  u64 maximum_total_canonical_value_bytes = 0;
  TypedUpdateHash evidence_sha256{};
};

struct TypedUpdateRecoveryTokenCarrier {
  std::vector<byte> exact_bytes;
  TypedUpdateUuid recovery_token_uuid{};
  u64 recovery_generation = 0;
  TypedUpdateUuid authenticated_statement_receipt_uuid{};
  TypedUpdateUuid owning_transaction_uuid{};
  TypedUpdateUuid operation_uuid{};
  TypedUpdateUuid descriptor_uuid{};
  u64 descriptor_generation = 0;
  TypedUpdateUuid statement_savepoint_profile_uuid{};
  u64 statement_savepoint_profile_generation = 0;
  TypedUpdateUuid durable_registry_uuid{};
  u64 durable_registry_generation = 0;
  TypedUpdateHash evidence_sha256{};
};

// One exact native catalog policy row contributing to the immutable security
// snapshot. DUSR records have no in-record magic; their identity is established
// by the enclosing DUSV v1 carrier and exact 256-byte extent.
struct TypedUpdateSecurityPolicySourceRecord {
  u32 source_policy_ordinal = 0;
  u8 phase = 0;
  u8 source_state = 1;
  TypedUpdateUuid policy_uuid{};
  u64 policy_generation = 0;
  TypedUpdateUuid policy_version_uuid{};
  u64 effective_transaction_number = 0;
  TypedUpdateUuid target_relation_uuid{};
  u64 target_relation_generation = 0;
  TypedUpdateUuid source_expression_uuid{};
  u64 source_expression_generation = 0;
  TypedUpdateHash source_expression_evidence_sha256{};
  TypedUpdateUuid catalog_snapshot_uuid{};
  u64 catalog_generation = 0;
  TypedUpdateUuid security_snapshot_uuid{};
  u64 security_snapshot_generation = 0;
  TypedUpdateHash source_policy_catalog_row_sha256{};
  TypedUpdateHash record_evidence_sha256{};
};

struct TypedUpdateSecurityPolicySourceVector {
  std::vector<byte> exact_bytes;
  TypedUpdateVectorIdentity identity;
  std::vector<TypedUpdateSecurityPolicySourceRecord> records;
};

struct TypedUpdateSecuritySnapshotProof {
  std::vector<byte> exact_bytes;
  TypedUpdateUuid security_snapshot_uuid{};
  u64 security_snapshot_generation = 0;
  TypedUpdateUuid security_context_uuid{};
  u64 security_context_generation = 0;
  u64 security_epoch = 0;
  u64 policy_generation = 0;
  TypedUpdateUuid database_uuid{};
  TypedUpdateUuid authenticated_statement_receipt_uuid{};
  TypedUpdateUuid owning_transaction_uuid{};
  u64 owning_local_transaction_id = 0;
  TypedUpdateUuid operation_uuid{};
  u64 operation_generation = 0;
  TypedUpdateUuid recovery_token_uuid{};
  u64 recovery_generation = 0;
  TypedUpdateUuid statement_snapshot_uuid{};
  TypedUpdateUuid catalog_snapshot_uuid{};
  u64 catalog_generation = 0;
  u64 policy_catalog_epoch = 0;
  TypedUpdateUuid target_relation_uuid{};
  u64 target_relation_generation = 0;
  TypedUpdateUuid target_relation_occurrence_uuid{};
  u64 target_relation_occurrence_generation = 0;
  TypedUpdateUuid descriptor_uuid{};
  u64 descriptor_generation = 0;
  TypedUpdateUuid row_policy_set_uuid{};
  u64 row_policy_set_generation = 0;
  TypedUpdateUuid source_policy_vector_uuid{};
  u64 source_policy_vector_generation = 0;
  u32 row_policy_count = 0;
  u32 source_policy_count = 0;
  u8 snapshot_state = 1;
  TypedUpdateHash descriptor_evidence_sha256{};
  TypedUpdateHash row_policy_set_sha256{};
  TypedUpdateHash exact_dudc_sha256{};
  TypedUpdateHash exact_dupv_sha256{};
  TypedUpdateHash source_policy_catalog_vector_sha256{};
  TypedUpdateHash evidence_sha256{};
};

struct TypedUpdateMgaRecoveryObservation {
  std::vector<byte> exact_bytes;
  TypedUpdateUuid observation_uuid{};
  u64 observation_generation = 0;
  TypedUpdateUuid validated_mga_durable_handle_uuid{};
  u64 validated_mga_durable_handle_generation = 0;
  TypedUpdateUuid database_uuid{};
  TypedUpdateUuid descriptor_uuid{};
  u64 descriptor_generation = 0;
  TypedUpdateUuid operation_uuid{};
  u64 operation_generation = 0;
  TypedUpdateUuid authenticated_statement_receipt_uuid{};
  TypedUpdateUuid owning_transaction_uuid{};
  u64 owning_local_transaction_id = 0;
  TypedUpdateUuid recovery_token_uuid{};
  u64 recovery_generation = 0;
  TypedUpdateJournalState latest_journal_state =
      TypedUpdateJournalState::bound;
  TypedUpdateTransactionState transaction_state =
      TypedUpdateTransactionState::active_live;
  TypedUpdateSavepointState savepoint_state =
      TypedUpdateSavepointState::absent;
  bool statement_barrier_present = false;
  bool no_surviving_effect_proven = false;
  TypedUpdateUuid statement_savepoint_uuid{};
  u64 statement_savepoint_generation = 0;
  TypedUpdateUuid reserved_statement_barrier_uuid{};
  u64 reserved_statement_barrier_generation = 0;
  u64 durable_chain_head_sequence = 0;
  TypedUpdateHash durable_chain_head_record_evidence_sha256{};
  TypedUpdateUuid catalog_snapshot_uuid{};
  u64 catalog_generation = 0;
  TypedUpdateUuid security_snapshot_uuid{};
  u64 security_snapshot_generation = 0;
  u64 security_epoch = 0;
  TypedUpdateHash time_independent_replay_identity_sha256{};
  TypedUpdateHash observation_evidence_sha256{};
};

struct TypedUpdateDatatypeAuthorityRecord {
  std::vector<byte> exact_bytes;
  u32 datatype_ordinal = 0;
  TypedUpdateDatatypeIdentityCode datatype_identity_code =
      TypedUpdateDatatypeIdentityCode::boolean_v1;
  TypedUpdateNullEncodingCode null_encoding_code =
      TypedUpdateNullEncodingCode::containing_slot_value_or_null_state;
  TypedUpdateByteOrderCode byte_order_code =
      TypedUpdateByteOrderCode::single_byte;
  bool is_signed = false;
  TypedUpdateUuid descriptor_uuid{};
  u64 descriptor_generation = 0;
  TypedUpdateUuid type_uuid{};
  u64 type_generation = 0;
  u16 codec_version = 0;
  std::string canonical_name;
  std::string codec_id;
  TypedUpdateRepresentationCode representation_code =
      TypedUpdateRepresentationCode::canonical_boolean;
  u64 codec_generation = 0;
  u32 canonical_value_minimum_bytes = 0;
  u32 canonical_value_maximum_bytes = 0;
  u32 canonical_value_exact_bytes = 0;
  TypedUpdateUuid datatype_snapshot_uuid{};
  u64 datatype_catalog_generation = 0;
  u64 datatype_registry_generation = 0;
  TypedUpdateHash record_evidence_sha256{};
};

struct TypedUpdateDatatypeAuthorityVector {
  std::vector<byte> exact_bytes;
  TypedUpdateVectorIdentity identity;
  std::vector<TypedUpdateDatatypeAuthorityRecord> records;
};

struct TypedUpdateBuiltinOperatorAuthorityRecord {
  std::vector<byte> exact_bytes;
  u32 operator_ordinal = 0;
  u8 semantic_code = 1;
  u8 operand_arity = 2;
  u8 null_behavior_code = 1;
  u8 accepted_state = 1;
  TypedUpdateUuid operator_uuid{};
  u64 operator_generation = 0;
  TypedUpdateUuid operator_snapshot_uuid{};
  u64 operator_registry_generation = 0;
  TypedUpdateUuid left_descriptor_uuid{};
  u64 left_descriptor_generation = 0;
  TypedUpdateUuid left_type_uuid{};
  u64 left_type_generation = 0;
  TypedUpdateUuid right_descriptor_uuid{};
  u64 right_descriptor_generation = 0;
  TypedUpdateUuid right_type_uuid{};
  u64 right_type_generation = 0;
  TypedUpdateUuid result_descriptor_uuid{};
  u64 result_descriptor_generation = 0;
  TypedUpdateUuid result_type_uuid{};
  u64 result_type_generation = 0;
  u16 result_codec_version = 0;
  u8 operator_family_code = 1;
  u64 result_codec_generation = 0;
  std::string result_codec_id;
  u8 operand_identity_rule = 1;
  u8 result_null_encoding_code = 1;
  TypedUpdateHash record_evidence_sha256{};
};

struct TypedUpdateBuiltinOperatorAuthorityVector {
  std::vector<byte> exact_bytes;
  TypedUpdateVectorIdentity identity;
  std::vector<TypedUpdateBuiltinOperatorAuthorityRecord> records;
};

struct TypedUpdateResultCarrier {
  std::vector<byte> exact_bytes;
  TypedUpdateUuid update_descriptor_uuid{};
  u64 update_descriptor_generation = 0;
  TypedUpdateUuid operation_uuid{};
  TypedUpdateUuid owning_transaction_uuid{};
  u64 owning_local_transaction_id = 0;
  TypedUpdateUuid relation_uuid{};
  u64 relation_generation = 0;
  u64 matched_count = 0;
  u64 updated_count = 0;
  TypedUpdateHash effect_set_sha256{};
  TypedUpdateHash executor_evidence_sha256{};
  TypedUpdateUuid publication_barrier_uuid{};
  u64 publication_barrier_generation = 0;
  TypedUpdateHash result_evidence_sha256{};
};

struct TypedUpdateResultEvidenceReference {
  std::string evidence_kind;
  std::string evidence_id;
};

struct TypedUpdateJournalRecord {
  std::vector<byte> exact_bytes;
  std::vector<byte> embedded_descriptor_bytes;
  std::optional<std::vector<byte>> embedded_result_bytes;
  TypedUpdateJournalState lifecycle_state = TypedUpdateJournalState::bound;
  u64 journal_sequence = 0;
  TypedUpdateUuid database_uuid{};
  TypedUpdateUuid authenticated_statement_receipt_uuid{};
  TypedUpdateUuid owning_transaction_uuid{};
  u64 owning_local_transaction_id = 0;
  TypedUpdateUuid operation_uuid{};
  TypedUpdateUuid recovery_token_uuid{};
  u64 recovery_generation = 0;
  TypedUpdateUuid statement_savepoint_uuid{};
  u64 statement_savepoint_generation = 0;
  TypedUpdateHash prior_record_sha256{};
  TypedUpdateDescriptorCarrier descriptor;
  std::optional<TypedUpdateResultCarrier> prior_result;
  TypedUpdateHash record_evidence_sha256{};
};

struct TypedUpdateCarrierSet {
  TypedUpdateDescriptorCarrier descriptor;
  TypedUpdateAssignmentVector assignments;
  TypedUpdatePredicateVector predicate;
  TypedUpdateRowPolicyVector row_policies;
  TypedUpdateConstraintVector constraints;
  TypedUpdateTriggerVector triggers;
  TypedUpdateTargetOrderCarrier target_order;
  TypedUpdateResourceBudgetCarrier resource_budget;
  TypedUpdateRecoveryTokenCarrier recovery_token;
};

struct TypedUpdateJournalChainContext {
  bool first_record = true;
  u64 prior_sequence = 0;
  TypedUpdateJournalState prior_state = TypedUpdateJournalState::bound;
  TypedUpdateHash prior_record_evidence_sha256{};
  // When the prior state is intent or prepared, this is its exact savepoint
  // identity.  For bound-to-aborted it is the exact MGA provider authority
  // observed after savepoint creation and rollback when intent publication
  // failed; nil/zero means no savepoint was ever opened.
  TypedUpdateUuid prior_savepoint_uuid{};
  u64 prior_savepoint_generation = 0;
  bool require_same_descriptor = false;
  std::array<byte, kTypedUpdateDescriptorBytes> expected_descriptor_bytes{};
  std::optional<std::array<byte, kTypedUpdateResultBytes>>
      expected_prepared_result_bytes;
};

enum class TypedUpdateRecoveryDecision : u8 {
  append_aborted_no_result,
  replay_published_result,
  append_published_and_replay_result,
  quarantine_update_failed,
  stale_replay,
};

bool EncodeTypedUpdateDescriptor(const TypedUpdateDescriptorCarrier& value,
                                 std::vector<byte>* encoded,
                                 TypedUpdateCarrierError* error);
bool DecodeAndValidateTypedUpdateDescriptor(
    std::span<const byte> encoded,
    TypedUpdateDescriptorCarrier* decoded,
    TypedUpdateCarrierError* error);

bool EncodeTypedUpdateAssignmentVector(const TypedUpdateAssignmentVector& value,
                                       std::vector<byte>* encoded,
                                       TypedUpdateCarrierError* error);
bool DecodeAndValidateTypedUpdateAssignmentVector(
    std::span<const byte> encoded,
    TypedUpdateAssignmentVector* decoded,
    TypedUpdateCarrierError* error);
bool EncodeTypedUpdatePredicateVector(const TypedUpdatePredicateVector& value,
                                      std::vector<byte>* encoded,
                                      TypedUpdateCarrierError* error);
bool DecodeAndValidateTypedUpdatePredicateVector(
    std::span<const byte> encoded,
    TypedUpdatePredicateVector* decoded,
    TypedUpdateCarrierError* error);
bool EncodeTypedUpdateRowPolicyVector(const TypedUpdateRowPolicyVector& value,
                                      std::vector<byte>* encoded,
                                      TypedUpdateCarrierError* error);
bool DecodeAndValidateTypedUpdateRowPolicyVector(
    std::span<const byte> encoded,
    TypedUpdateRowPolicyVector* decoded,
    TypedUpdateCarrierError* error);
bool EncodeTypedUpdateConstraintVector(const TypedUpdateConstraintVector& value,
                                       std::vector<byte>* encoded,
                                       TypedUpdateCarrierError* error);
bool DecodeAndValidateTypedUpdateConstraintVector(
    std::span<const byte> encoded,
    TypedUpdateConstraintVector* decoded,
    TypedUpdateCarrierError* error);
bool EncodeTypedUpdateTriggerVector(const TypedUpdateTriggerVector& value,
                                    std::vector<byte>* encoded,
                                    TypedUpdateCarrierError* error);
bool DecodeAndValidateTypedUpdateTriggerVector(
    std::span<const byte> encoded,
    TypedUpdateTriggerVector* decoded,
    TypedUpdateCarrierError* error);

bool EncodeTypedUpdateTargetOrder(const TypedUpdateTargetOrderCarrier& value,
                                  std::vector<byte>* encoded,
                                  TypedUpdateCarrierError* error);
bool DecodeAndValidateTypedUpdateTargetOrder(
    std::span<const byte> encoded,
    TypedUpdateTargetOrderCarrier* decoded,
    TypedUpdateCarrierError* error);
bool EncodeTypedUpdateResourceBudget(
    const TypedUpdateResourceBudgetCarrier& value,
    std::vector<byte>* encoded,
    TypedUpdateCarrierError* error);
bool DecodeAndValidateTypedUpdateResourceBudget(
    std::span<const byte> encoded,
    TypedUpdateResourceBudgetCarrier* decoded,
    TypedUpdateCarrierError* error);
bool EncodeTypedUpdateRecoveryToken(
    const TypedUpdateRecoveryTokenCarrier& value,
    std::vector<byte>* encoded,
    TypedUpdateCarrierError* error);
bool DecodeAndValidateTypedUpdateRecoveryToken(
    std::span<const byte> encoded,
    TypedUpdateRecoveryTokenCarrier* decoded,
    TypedUpdateCarrierError* error);

bool EncodeTypedUpdateSecurityPolicySourceVector(
    const TypedUpdateSecurityPolicySourceVector& value,
    std::vector<byte>* encoded,
    TypedUpdateCarrierError* error);
bool DecodeAndValidateTypedUpdateSecurityPolicySourceVector(
    std::span<const byte> encoded,
    TypedUpdateSecurityPolicySourceVector* decoded,
    TypedUpdateCarrierError* error);
bool EncodeTypedUpdateSecuritySnapshotProof(
    const TypedUpdateSecuritySnapshotProof& value,
    std::vector<byte>* encoded,
    TypedUpdateCarrierError* error);
bool DecodeAndValidateTypedUpdateSecuritySnapshotProof(
    std::span<const byte> encoded,
    TypedUpdateSecuritySnapshotProof* decoded,
    TypedUpdateCarrierError* error);
bool EncodeTypedUpdateMgaRecoveryObservation(
    const TypedUpdateMgaRecoveryObservation& value,
    std::vector<byte>* encoded,
    TypedUpdateCarrierError* error);
bool DecodeAndValidateTypedUpdateMgaRecoveryObservation(
    std::span<const byte> encoded,
    TypedUpdateMgaRecoveryObservation* decoded,
    TypedUpdateCarrierError* error);
bool EncodeTypedUpdateDatatypeAuthorityVector(
    const TypedUpdateDatatypeAuthorityVector& value,
    std::vector<byte>* encoded,
    TypedUpdateCarrierError* error);
bool DecodeAndValidateTypedUpdateDatatypeAuthorityVector(
    std::span<const byte> encoded,
    TypedUpdateDatatypeAuthorityVector* decoded,
    TypedUpdateCarrierError* error);
bool EncodeTypedUpdateBuiltinOperatorAuthorityVector(
    const TypedUpdateBuiltinOperatorAuthorityVector& value,
    std::vector<byte>* encoded,
    TypedUpdateCarrierError* error);
bool DecodeAndValidateTypedUpdateBuiltinOperatorAuthorityVector(
    std::span<const byte> encoded,
    TypedUpdateBuiltinOperatorAuthorityVector* decoded,
    TypedUpdateCarrierError* error);

bool EncodeTypedUpdateResult(const TypedUpdateResultCarrier& value,
                             std::vector<byte>* encoded,
                             TypedUpdateCarrierError* error);
bool DecodeAndValidateTypedUpdateResult(
    std::span<const byte> encoded,
    TypedUpdateResultCarrier* decoded,
    TypedUpdateCarrierError* error);
bool EncodeTypedUpdateResultEvidenceMaterial(
    const TypedUpdateResultCarrier& value,
    std::span<const TypedUpdateResultEvidenceReference> evidence,
    std::vector<byte>* material,
    TypedUpdateCarrierError* error);
bool ComputeTypedUpdateResultInnerEvidence(
    const TypedUpdateResultCarrier& value,
    std::span<const TypedUpdateResultEvidenceReference> evidence,
    TypedUpdateHash* effect_set_sha256,
    TypedUpdateHash* executor_evidence_sha256,
    TypedUpdateCarrierError* error);

// Journal encoding is nonmutating: the caller supplies sequence/predecessor
// fields, while the encoder computes record evidence into the returned bytes.
// Successor validation requires the prior exact DUDC and, for
// prepared-to-published, the prior exact DURS in the chain context.
bool EncodeTypedUpdateJournalRecord(
    const TypedUpdateJournalRecord& value,
    const TypedUpdateJournalChainContext& chain,
    std::vector<byte>* encoded,
    TypedUpdateCarrierError* error);
bool DecodeAndValidateTypedUpdateJournalRecord(
    std::span<const byte> encoded,
    const TypedUpdateJournalChainContext& chain,
    TypedUpdateJournalRecord* decoded,
    TypedUpdateCarrierError* error);
// Convenience for a just-encoded record. This checks its header, exact outer
// extent, and record-evidence hash; use DecodeAndValidate for full semantics.
bool ExtractTypedUpdateJournalRecordEvidence(
    std::span<const byte> encoded,
    TypedUpdateHash* record_evidence_sha256,
    TypedUpdateCarrierError* error);

bool ValidateTypedUpdateCarrierSet(const TypedUpdateCarrierSet& carriers,
                                   TypedUpdateCarrierError* error);
bool ValidateTypedUpdateSecurityRecoveryAuthority(
    const TypedUpdateDescriptorCarrier& descriptor,
    const TypedUpdateRowPolicyVector& row_policies,
    const TypedUpdateRecoveryTokenCarrier& recovery_token,
    const TypedUpdateSecurityPolicySourceVector& source_policies,
    const TypedUpdateSecuritySnapshotProof& snapshot_proof,
    const TypedUpdateJournalRecord& journal_head,
    const TypedUpdateMgaRecoveryObservation& recovery_observation,
    TypedUpdateCarrierError* error);
bool ValidateTypedUpdateDatatypeOperatorAuthority(
    const TypedUpdateDescriptorCarrier& descriptor,
    const TypedUpdateAssignmentVector& assignments,
    const TypedUpdatePredicateVector& predicate,
    const TypedUpdateDatatypeAuthorityVector& datatypes,
    const TypedUpdateBuiltinOperatorAuthorityVector& operators,
    TypedUpdateCarrierError* error);
bool DecideTypedUpdateRecovery(
    const TypedUpdateMgaRecoveryObservation& observation,
    TypedUpdateRecoveryDecision* decision,
    TypedUpdateCarrierError* error);

const char* TypedUpdateCarrierErrorCodeName(TypedUpdateCarrierErrorCode code);

}  // namespace scratchbird::wire
