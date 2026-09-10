// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "typed_update_carrier_codec.hpp"

namespace scratchbird::wire {

// Ordinary DELETE (0x0310), never an UPDATE or engine.op.delete alias.
// UUID fields are raw binary16. No assignment representation is admitted.
inline constexpr u32 kTypedDeleteDescriptorBytes = 648;
inline constexpr u32 kTypedDeleteResultBytes = 256;
inline constexpr u32 kTypedDeleteJournalHeaderBytes = 256;
inline constexpr u32 kTypedDeleteJournalWithoutResultBytes = 904;
inline constexpr u32 kTypedDeleteJournalWithResultBytes = 1160;

struct TypedDeleteCarrierError {
  std::string diagnostic_code;
  std::string field;
  bool ok() const noexcept { return diagnostic_code.empty(); }
};
struct TypedDeleteDescriptorCarrier {
  TypedUpdateUuid descriptor_uuid{};
  TypedUpdateUuid authenticated_statement_receipt_uuid{};
  TypedUpdateUuid operation_uuid{};
  TypedUpdateUuid owning_transaction_uuid{};
  TypedUpdateUuid statement_snapshot_uuid{};
  TypedUpdateUuid catalog_snapshot_uuid{};
  TypedUpdateUuid security_context_uuid{};
  TypedUpdateUuid security_snapshot_uuid{};
  TypedUpdateUuid target_relation_uuid{};
  TypedUpdateUuid target_relation_occurrence_uuid{};
  TypedUpdateUuid predicate_expression_uuid{};
  TypedUpdateUuid row_policy_set_uuid{};
  TypedUpdateUuid constraint_set_uuid{};
  TypedUpdateUuid trigger_set_uuid{};
  TypedUpdateUuid deterministic_target_order_uuid{};
  TypedUpdateUuid resource_budget_uuid{};
  TypedUpdateUuid recovery_token_uuid{};
  TypedUpdateUuid builtin_operator_snapshot_uuid{};
  u64 descriptor_generation = 0;
  u64 structural_occurrence_id = 0;
  u64 operation_generation = 0;
  u64 owning_local_transaction_id = 0;
  u64 catalog_generation = 0;
  u64 datatype_registry_generation = 0;
  u64 security_generation = 0;
  u64 target_relation_generation = 0;
  u64 target_relation_occurrence_generation = 0;
  u64 predicate_expression_generation = 0;
  u64 predicate_root_node_id = 0;
  u64 row_policy_set_generation = 0;
  u64 constraint_set_generation = 0;
  u64 trigger_set_generation = 0;
  u64 deterministic_target_order_generation = 0;
  u64 resource_budget_generation = 0;
  u64 recovery_generation = 0;
  u64 executor_availability_generation = 0;
  u64 builtin_operator_registry_generation = 0;
  u32 predicate_node_count = 0;
  u32 row_policy_count = 0;
  u32 constraint_count = 0;
  u32 trigger_count = 0;
  TypedUpdateHash predicate_vector_sha256{};
  TypedUpdateHash row_policy_set_sha256{};
  TypedUpdateHash ordered_constraint_set_sha256{};
  TypedUpdateHash ordered_trigger_set_sha256{};
  TypedUpdateHash descriptor_evidence_sha256{};
  std::vector<byte> exact_bytes;
};
struct TypedDeleteResultCarrier {
  TypedUpdateUuid delete_descriptor_uuid{};
  u64 delete_descriptor_generation = 0;
  TypedUpdateUuid operation_uuid{};
  TypedUpdateUuid owning_transaction_uuid{};
  u64 owning_local_transaction_id = 0;
  TypedUpdateUuid relation_uuid{};
  u64 relation_generation = 0;
  u64 matched_count = 0;
  u64 deleted_count = 0;
  TypedUpdateHash effect_set_sha256{};
  TypedUpdateHash executor_evidence_sha256{};
  TypedUpdateUuid publication_barrier_uuid{};
  u64 publication_barrier_generation = 0;
  TypedUpdateHash result_evidence_sha256{};
  std::vector<byte> exact_bytes;
};
struct TypedDeleteJournalRecord {
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
  TypedDeleteDescriptorCarrier descriptor;
  std::optional<TypedDeleteResultCarrier> prior_result;
  TypedUpdateHash record_evidence_sha256{};
  std::vector<byte> exact_bytes;
};

bool EncodeTypedDeleteDescriptor(const TypedDeleteDescriptorCarrier&,
    std::vector<byte>*, TypedDeleteCarrierError*);
bool DecodeAndValidateTypedDeleteDescriptor(std::span<const byte>,
    TypedDeleteDescriptorCarrier*, TypedDeleteCarrierError*);
// Cross-bind DELETE's shared typed predicate/datatype/operator material. No
// UPDATE descriptor or assignment vector participates in this validation.
bool ValidateTypedDeleteDatatypeOperatorAuthority(
    const TypedDeleteDescriptorCarrier&, const TypedUpdatePredicateVector&,
    const TypedUpdateDatatypeAuthorityVector&, const TypedUpdateBuiltinOperatorAuthorityVector&,
    TypedDeleteCarrierError*);
bool EncodeTypedDeleteResult(const TypedDeleteResultCarrier&,
    std::vector<byte>*, TypedDeleteCarrierError*);
bool DecodeAndValidateTypedDeleteResult(std::span<const byte>,
    TypedDeleteResultCarrier*, TypedDeleteCarrierError*);
// A null predecessor admits only bound/sequence 1. Successors require the
// exact validated predecessor; hashes are not MGA finality authority.
bool EncodeTypedDeleteJournal(const TypedDeleteJournalRecord&,
    const TypedDeleteJournalRecord* predecessor, std::vector<byte>*, TypedDeleteCarrierError*);
bool DecodeAndValidateTypedDeleteJournal(std::span<const byte>,
    const TypedDeleteJournalRecord* predecessor, TypedDeleteJournalRecord*, TypedDeleteCarrierError*);
}  // namespace scratchbird::wire
