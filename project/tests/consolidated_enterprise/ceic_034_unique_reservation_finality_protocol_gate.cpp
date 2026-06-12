// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// CEIC-034 focused validation for unique_btree reservation protocol evidence.
#include "btree_unique_durable_provider_closure.hpp"
#include "ceic_034_unique_reservation_finality_protocol.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace index = scratchbird::core::index;
using scratchbird::core::platform::TypedUuid;
using scratchbird::core::platform::UuidKind;
using scratchbird::transaction::mga::TransactionState;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

TypedUuid TestUuid(UuidKind kind, unsigned seed) {
  TypedUuid value;
  value.kind = kind;
  for (std::size_t i = 0; i < value.value.bytes.size(); ++i) {
    value.value.bytes[i] =
        static_cast<scratchbird::core::platform::byte>(
            (seed * 41u + i * 23u + 0x71u) & 0xffu);
  }
  value.value.bytes[6] =
      static_cast<scratchbird::core::platform::byte>(
          (value.value.bytes[6] & 0x0fu) | 0x70u);
  value.value.bytes[8] =
      static_cast<scratchbird::core::platform::byte>(
          (value.value.bytes[8] & 0x3fu) | 0x80u);
  return value;
}

bool EvidenceHas(const index::UniqueReservationFinalityProtocolResult& result,
                 std::string_view token) {
  for (const auto& row : result.evidence) {
    if (row.find(token) != std::string::npos) {
      return true;
    }
  }
  return false;
}

index::IndexProviderAccessMethodContract ProviderContract() {
  index::IndexProviderAccessMethodContract contract;
  contract.family = index::IndexFamily::unique_btree;
  contract.route = index::IndexRouteKind::bulk_build;
  contract.provider.provider_id = "ceic034-unique-provider";
  contract.provider.provider_name = "CEIC-034 Unique Provider";
  contract.provider.provider_contract_version = "ceic034.provider.v1";
  contract.provider.persistent_access_method = true;
  contract.provider.provider_backed = true;
  contract.route_boundary.route_capability_present = true;
  contract.route_boundary.provider_route_supported = true;
  contract.route_boundary.static_registry_complete_capability_seen = true;
  contract.route_boundary.external_cluster_provider_only = true;
  contract.route_boundary.route_specific_boundary_declared = true;
  contract.mutation_batch.batch_uuid = TestUuid(UuidKind::object, 10);
  contract.mutation_batch.operation_count = 4;
  contract.mutation_batch.batch_admission_requested = true;
  contract.mutation_batch.provider_batch_admission_supported = true;
  contract.mutation_batch.deterministic_batch_order = true;
  contract.mutation_batch.idempotent_replay_safe = true;
  contract.generation.generation_uuid = TestUuid(UuidKind::object, 11);
  contract.generation.generation_number = 34;
  contract.generation.provider_generation_id = "unique-provider-generation-34";
  contract.generation.root_identity_bound = true;
  contract.generation.cow_generation = true;
  contract.recovery.recovery_context_id = "unique-recovery-context-34";
  contract.recovery.recovery_reopen_supported = true;
  contract.recovery.crash_classification_supported = true;
  contract.recovery.corruption_classification_supported = true;
  contract.recovery.mga_recovery_evidence_only = true;
  contract.cleanup.oldest_active_transaction_id = 21;
  contract.cleanup.cleanup_generation_floor = 34;
  contract.cleanup.engine_mga_horizon_bound = true;
  contract.cleanup.provider_cleanup_supported = true;
  contract.validation_repair.validate_supported = true;
  contract.validation_repair.repair_supported = true;
  contract.validation_repair.rebuild_supported = true;
  contract.validation_repair.deterministic_diagnostics = true;
  contract.provider_evidence = {
      "ceic031_admitted_provider=true",
      "unique_btree_page_provider_contract=true",
      "ceic034_consumes_ceic031=true"};
  return contract;
}

index::IndexMGARecoveryContract MGAContract() {
  index::IndexMGARecoveryContract contract;
  contract.identity.family = index::IndexFamily::unique_btree;
  contract.identity.route = index::IndexRouteKind::bulk_build;
  contract.identity.provider_id = "ceic034-unique-provider";
  contract.identity.provider_contract_version = "ceic034.provider.v1";
  contract.identity.persistent_provider = true;
  contract.identity.external_cluster_provider_only = true;
  contract.mga_authority.inventory_present = true;
  contract.mga_authority.inventory_authoritative = true;
  contract.mga_authority.inventory_durable = true;
  contract.mga_authority.snapshot_present = true;
  contract.mga_authority.snapshot_authoritative = true;
  contract.mga_authority.cleanup_horizon_present = true;
  contract.mga_authority.cleanup_horizon_authoritative = true;
  contract.mga_authority.cleanup_horizon_engine_bound = true;
  contract.mga_authority.inventory_epoch = 34;
  contract.mga_authority.snapshot_epoch = 34;
  contract.mga_authority.cleanup_horizon_epoch = 34;
  contract.mga_authority.required_engine_evidence_epoch = 34;
  contract.mga_authority.inventory_evidence_id = "mga-inventory-34";
  contract.mga_authority.snapshot_evidence_id = "mga-snapshot-34";
  contract.mga_authority.cleanup_horizon_evidence_id = "mga-horizon-34";
  contract.generation.index_uuid = TestUuid(UuidKind::object, 21);
  contract.generation.generation_uuid = TestUuid(UuidKind::object, 22);
  contract.generation.generation_number = 34;
  contract.generation.cow_generation_number = 34;
  contract.generation.provider_generation_id = "unique-provider-generation-34";
  contract.generation.root_identity_bound = true;
  contract.generation.cow_generation_identity_bound = true;
  contract.generation.publish_state =
      index::IndexGenerationPublishState::published;
  contract.recovery.crash_classification =
      index::IndexCrashRecoveryClassification::crash_after_generation_publish;
  contract.recovery.corruption_classification =
      index::IndexCorruptionClassification::checksum_mismatch;
  contract.recovery.recovery_evidence_id = "unique-recovery-34";
  contract.recovery.durable_recovery_evidence = true;
  contract.recovery.replay_idempotent = true;
  contract.recovery.provider_evidence_only = true;
  contract.provider_evidence = {
      "ceic032_admitted_mga_contract=true",
      "unique_btree_mga_recovery_contract=true",
      "ceic034_consumes_ceic032=true"};
  return contract;
}

index::BtreeUniqueDurableProviderClosureRequest ClosureRequest() {
  index::BtreeUniqueDurableProviderClosureRequest request;
  request.family = index::IndexFamily::unique_btree;
  request.route = index::IndexRouteKind::bulk_build;
  request.provider_id = "ceic034-unique-provider";
  request.provider_admission =
      index::AdmitIndexProviderAccessMethod(ProviderContract());
  request.mga_recovery_contract =
      index::AdmitIndexMGARecoveryContract(MGAContract());
  request.generation_root.index_uuid = TestUuid(UuidKind::object, 21);
  request.generation_root.root_page_uuid = TestUuid(UuidKind::page, 23);
  request.generation_root.generation_uuid = TestUuid(UuidKind::object, 22);
  request.generation_root.root_page_number = 1034;
  request.generation_root.generation_number = 34;
  request.generation_root.cow_generation_number = 34;
  request.generation_root.provider_generation_id =
      "unique-provider-generation-34";
  request.generation_root.cow_generation_publish_proven = true;
  request.generation_root.root_identity_bound = true;
  request.generation_root.root_reopen_identity_stable = true;
  request.generation_root.provider_generation_matches_mga_contract = true;
  request.generation_root.publish_after_durable_mga_evidence = true;
  request.split_merge_scan.split_capability_present = true;
  request.split_merge_scan.split_proof_present = true;
  request.split_merge_scan.merge_capability_present = true;
  request.split_merge_scan.merge_proof_present = true;
  request.split_merge_scan.ordered_scan_capability_present = true;
  request.split_merge_scan.ordered_scan_proof_present = true;
  request.split_merge_scan.point_lookup_capability_present = true;
  request.split_merge_scan.deterministic_page_format = true;
  request.split_merge_scan.page_integrity_proof_present = true;
  request.split_merge_scan.structural_evidence_id =
      "unique-split-merge-scan-34";
  request.unique_duplicate.duplicate_preflight_proven = true;
  request.unique_duplicate.reservation_proof_present = true;
  request.unique_duplicate.reservation_engine_transaction_bound = true;
  request.unique_duplicate.duplicate_recheck_engine_bound = true;
  request.unique_duplicate.ceic_034_unique_finality_protocol_pending = true;
  request.unique_duplicate.reservation_evidence_id =
      "unique-duplicate-reservation-34";
  request.cleanup.oldest_active_transaction_id = 21;
  request.cleanup.cleanup_generation_floor = 34;
  request.cleanup.engine_mga_horizon_bound = true;
  request.cleanup.cleanup_uses_engine_horizon = true;
  request.cleanup.provider_cleanup_evidence_only = true;
  request.cleanup.cleanup_evidence_id = "unique-cleanup-horizon-34";
  request.crash_corruption.crash_classification =
      index::IndexCrashRecoveryClassification::crash_after_generation_publish;
  request.crash_corruption.corruption_classification =
      index::IndexCorruptionClassification::checksum_mismatch;
  request.crash_corruption.crash_reopen_classification_present = true;
  request.crash_corruption.corruption_classification_present = true;
  request.crash_corruption.durable_provider_evidence_only = true;
  request.crash_corruption.classification_evidence_id =
      "unique-crash-corruption-34";
  request.repair_rebuild.validate_before_repair = true;
  request.repair_rebuild.repair_supported = true;
  request.repair_rebuild.rebuild_supported = true;
  request.repair_rebuild.deterministic_recommendation_present = true;
  request.repair_rebuild.recommendation_matches_mga_recovery_contract = true;
  request.repair_rebuild.recommendation_evidence_id =
      "unique-repair-rebuild-34";
  request.concurrency.generation_publish_fence_hook = true;
  request.concurrency.root_publish_observer_hook = true;
  request.concurrency.split_merge_latch_boundary_hook = true;
  request.concurrency.concurrent_scan_mutation_boundary_hook = true;
  request.concurrency.evidence_only_not_transaction_finality = true;
  request.concurrency.concurrency_evidence_id = "unique-concurrency-34";
  request.evidence = {
      "unique_btree_durable_provider_evidence=true",
      "ceic034_unique_protocol_consumes_ceic033=true",
      "ceic041_crash_matrix_pending=true"};
  return request;
}

index::UniqueIndexReservationTransactionProof TransactionProof(
    TypedUuid transaction_uuid,
    scratchbird::core::platform::u64 local_transaction_id,
    TransactionState state) {
  index::UniqueIndexReservationTransactionProof proof;
  proof.transaction_uuid = transaction_uuid;
  proof.local_transaction_id = local_transaction_id;
  proof.state = state;
  proof.engine_mga_authority = true;
  proof.durable_transaction_inventory_authoritative = true;
  proof.durable_commit_evidence =
      state == TransactionState::committed ||
      state == TransactionState::archived;
  proof.durable_rollback_evidence = state == TransactionState::rolled_back;
  proof.evidence_token =
      "mga-transaction-proof-" + std::to_string(local_transaction_id);
  return proof;
}

index::UniqueIndexReservationRequest ReservationRequest(unsigned tx_seed,
                                                        unsigned row_seed) {
  index::UniqueIndexReservationRequest request;
  request.index_uuid = TestUuid(UuidKind::object, 21);
  request.table_uuid = TestUuid(UuidKind::object, 31);
  request.constraint_uuid = TestUuid(UuidKind::object, 32);
  request.row_uuid = TestUuid(UuidKind::row, row_seed);
  request.version_uuid = TestUuid(UuidKind::row, row_seed + 100);
  request.transaction_uuid = TestUuid(UuidKind::transaction, tx_seed);
  request.local_transaction_id = tx_seed;
  request.encoded_key = {0x34, 0x55, 0x4e, 0x51};
  request.null_policy = index::UniqueIndexReservationNullPolicy::
      nulls_not_distinct;
  request.null_policy_proven = true;
  request.partial_predicate_participates = true;
  request.partial_predicate_proven = true;
  request.active_conflict_policy =
      index::UniqueIndexReservationActiveConflictPolicy::refuse_candidate;
  return request;
}

index::UniqueReservationFinalityProtocolRequest ProtocolRequest(
    index::UniqueReservationFinalityProtocolMode mode =
        index::UniqueReservationFinalityProtocolMode::immediate) {
  index::UniqueIndexReservationLedger ledger;
  auto reservation = ReservationRequest(34, 70);
  auto reservation_result =
      index::ReserveUniqueIndexKey(&ledger, reservation);
  auto commit_validation =
      index::ValidateUniqueIndexCommitBatch(
          &ledger,
          {reservation.transaction_uuid,
           reservation.local_transaction_id,
           {TransactionProof(reservation.transaction_uuid,
                             reservation.local_transaction_id,
                             TransactionState::active)},
           "deferred-commit-probe-34"});

  index::UniqueReservationFinalityProtocolRequest request;
  request.family = index::IndexFamily::unique_btree;
  request.mode = mode;
  request.ceic033_closure =
      index::AdmitBtreeUniqueDurableProviderClosure(ClosureRequest());
  request.reservation_identity.index_uuid = reservation.index_uuid;
  request.reservation_identity.table_uuid = reservation.table_uuid;
  request.reservation_identity.constraint_uuid = reservation.constraint_uuid;
  request.reservation_identity.row_uuid = reservation.row_uuid;
  request.reservation_identity.version_uuid = reservation.version_uuid;
  request.reservation_identity.transaction_uuid =
      reservation.transaction_uuid;
  request.reservation_identity.local_transaction_id =
      reservation.local_transaction_id;
  request.reservation_identity.encoded_key = reservation.encoded_key;
  request.reservation_identity
      .reservation_identity_bound_to_duplicate_preflight = true;
  request.reservation_identity.reservation_identity_bound_to_ledger = true;
  request.reservation_identity.reservation_identity_bound_to_unique_btree =
      true;
  request.reservation_identity.reservation_evidence_id =
      "reservation-identity-34";
  request.duplicate_preflight.duplicate_preflight_performed = true;
  request.duplicate_preflight.duplicate_preflight_engine_mga_bound = true;
  request.duplicate_preflight.duplicate_preflight_result_resolved = true;
  request.duplicate_preflight.duplicate_preflight_before_reservation = true;
  request.duplicate_preflight.duplicate_preflight_evidence_id =
      "duplicate-preflight-34";
  request.mga_transaction_binding.transaction_uuid =
      reservation.transaction_uuid;
  request.mga_transaction_binding.local_transaction_id =
      reservation.local_transaction_id;
  request.mga_transaction_binding.observed_state = TransactionState::active;
  request.mga_transaction_binding.engine_transaction_handle_bound = true;
  request.mga_transaction_binding.engine_mga_inventory_present = true;
  request.mga_transaction_binding.engine_mga_inventory_authoritative = true;
  request.mga_transaction_binding.durable_transaction_inventory_authoritative =
      true;
  request.mga_transaction_binding.transaction_matches_reservation = true;
  request.mga_transaction_binding.cleanup_horizon_engine_bound = true;
  request.mga_transaction_binding.inventory_evidence_id = "mga-inventory-34";
  request.mga_transaction_binding.transaction_evidence_token =
      "mga-transaction-proof-34";
  request.reservation_result = reservation_result;
  request.commit_probe.commit_probe_present = true;
  request.commit_probe.commit_probe_engine_mga_bound = true;
  request.commit_probe.commit_probe_scanned_reservation_ledger = true;
  request.commit_probe.commit_probe_result_resolved = true;
  request.commit_probe.commit_probe_evidence_id = "deferred-commit-probe-34";
  request.commit_probe.commit_probe_result = commit_validation;
  request.rollback_retry_cleanup.rollback_release_supported = true;
  request.rollback_retry_cleanup.retry_release_supported = true;
  request.rollback_retry_cleanup.cleanup_evidence_present = true;
  request.rollback_retry_cleanup.cleanup_horizon_engine_bound = true;
  request.rollback_retry_cleanup.reservation_ledger_cleanup_evidence_present =
      true;
  request.rollback_retry_cleanup.cleanup_evidence_id =
      "rollback-retry-cleanup-34";
  request.conflict_outcome = index::UniqueReservationConflictOutcome::none;
  request.crash_window.classification =
      index::UniqueReservationCrashWindowClassification::clean_no_crash;
  request.crash_window.classification_present = true;
  request.crash_window.classification_engine_mga_bound = true;
  request.crash_window.classification_evidence_id =
      "unique-crash-window-34";
  request.evidence = {
      "ceic034_unique_protocol_evidence=true",
      "mga_inventory_remains_finality_authority=true",
      "ceic041_ceic042_all_index_readiness_not_claimed=true"};
  return request;
}

void RequireStatus(
    const index::UniqueReservationFinalityProtocolResult& result,
    index::UniqueReservationFinalityProtocolStatus status,
    std::string_view message) {
  Require(result.protocol_status == status, message);
  Require(!result.ok(), "refused CEIC-034 result unexpectedly ok");
  Require(result.fail_closed, "refused CEIC-034 result did not fail closed");
  Require(!result.diagnostic.diagnostic_code.empty(),
          "refused CEIC-034 result lacks diagnostic code");
  Require(!result.enterprise_ready_claimed,
          "refused CEIC-034 result must not claim enterprise readiness");
  Require(!result.all_index_readiness_claimed,
          "refused CEIC-034 result must not claim all-index readiness");
  Require(!result.ceic_041_crash_matrix_claimed,
          "refused CEIC-034 result must not claim CEIC-041");
  Require(!result.ceic_042_readiness_gate_claimed,
          "refused CEIC-034 result must not claim CEIC-042");
}

void ValidImmediateModeAdmitsProtocolEvidenceOnly() {
  auto result =
      index::AdmitUniqueReservationFinalityProtocol(ProtocolRequest());
  Require(result.ok(), "valid immediate CEIC-034 protocol was refused");
  Require(result.protocol_status ==
              index::UniqueReservationFinalityProtocolStatus::
                  admitted_immediate_protocol_evidence,
          "valid immediate CEIC-034 protocol returned wrong status");
  Require(result.unique_protocol_evidence,
          "CEIC-034 must admit unique protocol evidence");
  Require(result.immediate_mode_evidence,
          "CEIC-034 immediate result did not record immediate mode");
  Require(!result.deferred_mode_evidence,
          "CEIC-034 immediate result incorrectly recorded deferred mode");
  Require(!result.enterprise_ready_claimed,
          "CEIC-034 must not claim enterprise readiness");
  Require(!result.all_index_readiness_claimed,
          "CEIC-034 must not claim all-index readiness");
  Require(!result.ceic_041_crash_matrix_claimed,
          "CEIC-034 must not claim CEIC-041");
  Require(!result.ceic_042_readiness_gate_claimed,
          "CEIC-034 must not claim CEIC-042");
  Require(result.proof_token ==
              index::kCeic034UniqueReservationFinalityProtocolGateToken,
          "CEIC-034 proof token missing");
  Require(EvidenceHas(
              result,
              "ceic_search_key=CEIC_034_UNIQUE_RESERVATION_FINALITY_PROTOCOL"),
          "CEIC-034 evidence anchor missing");
  Require(EvidenceHas(result, "transaction_finality_authority=false"),
          "CEIC-034 did not record no transaction-finality authority");
}

void ValidDeferredModeRequiresCommitProbe() {
  auto request = ProtocolRequest(
      index::UniqueReservationFinalityProtocolMode::deferred);
  auto result = index::AdmitUniqueReservationFinalityProtocol(request);
  Require(result.ok(), "valid deferred CEIC-034 protocol was refused");
  Require(result.protocol_status ==
              index::UniqueReservationFinalityProtocolStatus::
                  admitted_deferred_protocol_evidence,
          "valid deferred CEIC-034 protocol returned wrong status");
  Require(result.deferred_mode_evidence,
          "CEIC-034 deferred result did not record deferred mode");

  request.commit_probe.commit_probe_present = false;
  RequireStatus(
      index::AdmitUniqueReservationFinalityProtocol(request),
      index::UniqueReservationFinalityProtocolStatus::
          deferred_commit_probe_missing,
      "missing deferred commit probe did not fail closed");
}

void DuplicateConflictRefusalIsDeterministic() {
  index::UniqueIndexReservationLedger ledger;
  auto first = ReservationRequest(40, 80);
  Require(index::ReserveUniqueIndexKey(&ledger, first).ok(),
          "first duplicate setup reservation failed");
  Require(index::ValidateUniqueIndexCommitBatch(
              &ledger,
              {first.transaction_uuid,
               first.local_transaction_id,
               {TransactionProof(first.transaction_uuid,
                                 first.local_transaction_id,
                                 TransactionState::active)},
               "first-commit-probe"})
              .ok(),
          "first duplicate setup commit probe failed");
  Require(index::PublishUniqueIndexCommit(
              &ledger,
              {first.transaction_uuid,
               first.local_transaction_id,
               TransactionProof(first.transaction_uuid,
                                first.local_transaction_id,
                                TransactionState::committed)})
              .ok(),
          "first duplicate setup publish failed");

  auto second = ReservationRequest(41, 81);
  second.transaction_state_proofs = {
      TransactionProof(first.transaction_uuid,
                       first.local_transaction_id,
                       TransactionState::committed)};
  auto refused = index::ReserveUniqueIndexKey(&ledger, second);
  Require(!refused.ok() && refused.conflict,
          "duplicate reservation setup did not refuse");

  auto request = ProtocolRequest();
  request.reservation_identity.transaction_uuid = second.transaction_uuid;
  request.reservation_identity.local_transaction_id =
      second.local_transaction_id;
  request.mga_transaction_binding.transaction_uuid = second.transaction_uuid;
  request.mga_transaction_binding.local_transaction_id =
      second.local_transaction_id;
  request.mga_transaction_binding.transaction_evidence_token =
      "mga-transaction-proof-41";
  request.reservation_result = refused;
  request.conflict_outcome =
      index::UniqueReservationConflictOutcome::duplicate_refused;
  RequireStatus(
      index::AdmitUniqueReservationFinalityProtocol(request),
      index::UniqueReservationFinalityProtocolStatus::
          duplicate_conflict_refused,
      "duplicate conflict refusal did not produce deterministic status");
}

void ActiveConflictRefusalIsDeterministic() {
  index::UniqueIndexReservationLedger ledger;
  auto first = ReservationRequest(45, 85);
  Require(index::ReserveUniqueIndexKey(&ledger, first).ok(),
          "first active-conflict setup reservation failed");

  auto second = ReservationRequest(46, 86);
  second.transaction_state_proofs = {
      TransactionProof(first.transaction_uuid,
                       first.local_transaction_id,
                       TransactionState::active)};
  second.active_conflict_policy =
      index::UniqueIndexReservationActiveConflictPolicy::refuse_candidate;
  auto refused = index::ReserveUniqueIndexKey(&ledger, second);
  Require(!refused.ok() && refused.conflict,
          "active conflict setup did not refuse");

  auto request = ProtocolRequest();
  request.reservation_identity.transaction_uuid = second.transaction_uuid;
  request.reservation_identity.local_transaction_id =
      second.local_transaction_id;
  request.mga_transaction_binding.transaction_uuid = second.transaction_uuid;
  request.mga_transaction_binding.local_transaction_id =
      second.local_transaction_id;
  request.mga_transaction_binding.transaction_evidence_token =
      "mga-transaction-proof-46";
  request.reservation_result = refused;
  request.conflict_outcome =
      index::UniqueReservationConflictOutcome::active_conflict_refused;
  RequireStatus(
      index::AdmitUniqueReservationFinalityProtocol(request),
      index::UniqueReservationFinalityProtocolStatus::active_conflict_refused,
      "active conflict refusal did not produce deterministic status");
}

void RollbackRetryCleanupAdmitsAfterRelease() {
  index::UniqueIndexReservationLedger ledger;
  auto rolled_back = ReservationRequest(50, 90);
  Require(index::ReserveUniqueIndexKey(&ledger, rolled_back).ok(),
          "rollback setup reservation failed");
  auto cleanup = index::CleanupUniqueIndexReservationsForRollback(
      &ledger,
      {rolled_back.transaction_uuid,
       rolled_back.local_transaction_id,
       TransactionProof(rolled_back.transaction_uuid,
                        rolled_back.local_transaction_id,
                        TransactionState::rolled_back)});
  Require(cleanup.ok(), "rollback setup cleanup failed");

  auto retry = ReservationRequest(51, 91);
  auto retry_result = index::ReserveUniqueIndexKey(&ledger, retry);
  Require(retry_result.ok(), "retry after rollback cleanup did not reserve");

  auto request = ProtocolRequest();
  request.reservation_identity.transaction_uuid = retry.transaction_uuid;
  request.reservation_identity.local_transaction_id =
      retry.local_transaction_id;
  request.mga_transaction_binding.transaction_uuid = retry.transaction_uuid;
  request.mga_transaction_binding.local_transaction_id =
      retry.local_transaction_id;
  request.mga_transaction_binding.transaction_evidence_token =
      "mga-transaction-proof-51";
  request.reservation_result = retry_result;
  request.rollback_retry_cleanup.ledger_cleanup_result_present = true;
  request.rollback_retry_cleanup.cleanup_result = cleanup;
  request.conflict_outcome =
      index::UniqueReservationConflictOutcome::rollback_retry_released;
  request.crash_window.classification =
      index::UniqueReservationCrashWindowClassification::
          retry_after_rollback_cleanup;
  auto result = index::AdmitUniqueReservationFinalityProtocol(request);
  Require(result.ok(),
          "valid rollback/retry cleanup CEIC-034 protocol was refused");
}

void RequiredEvidenceFailuresFailClosed() {
  auto missing_ceic033 = ProtocolRequest();
  missing_ceic033.ceic033_closure = {};
  RequireStatus(
      index::AdmitUniqueReservationFinalityProtocol(missing_ceic033),
      index::UniqueReservationFinalityProtocolStatus::
          ceic033_closure_not_admitted,
      "missing CEIC-033 closure did not fail closed");

  auto wrong_family = ProtocolRequest();
  wrong_family.family = index::IndexFamily::btree;
  RequireStatus(index::AdmitUniqueReservationFinalityProtocol(wrong_family),
                index::UniqueReservationFinalityProtocolStatus::
                    unsupported_family,
                "non-unique_btree family did not fail closed");

  auto missing_preflight = ProtocolRequest();
  missing_preflight.duplicate_preflight.duplicate_preflight_performed = false;
  RequireStatus(
      index::AdmitUniqueReservationFinalityProtocol(missing_preflight),
      index::UniqueReservationFinalityProtocolStatus::
          duplicate_preflight_missing,
      "missing duplicate preflight did not fail closed");

  auto missing_binding = ProtocolRequest();
  missing_binding.mga_transaction_binding.engine_mga_inventory_present = false;
  RequireStatus(
      index::AdmitUniqueReservationFinalityProtocol(missing_binding),
      index::UniqueReservationFinalityProtocolStatus::
          mga_transaction_binding_missing,
      "missing MGA transaction binding did not fail closed");

  auto rolled_back_binding = ProtocolRequest();
  rolled_back_binding.mga_transaction_binding.observed_state =
      TransactionState::rolled_back;
  RequireStatus(
      index::AdmitUniqueReservationFinalityProtocol(rolled_back_binding),
      index::UniqueReservationFinalityProtocolStatus::
          mga_transaction_binding_missing,
      "rolled-back MGA transaction binding did not fail closed");

  auto missing_identity = ProtocolRequest();
  missing_identity.reservation_identity
      .reservation_identity_bound_to_ledger = false;
  RequireStatus(
      index::AdmitUniqueReservationFinalityProtocol(missing_identity),
      index::UniqueReservationFinalityProtocolStatus::
          reservation_identity_missing,
      "missing reservation identity did not fail closed");

  auto unresolved = ProtocolRequest();
  unresolved.conflict_outcome =
      index::UniqueReservationConflictOutcome::unresolved;
  RequireStatus(
      index::AdmitUniqueReservationFinalityProtocol(unresolved),
      index::UniqueReservationFinalityProtocolStatus::
          conflict_outcome_unresolved,
      "unresolved conflict outcome did not fail closed");

  auto missing_crash = ProtocolRequest();
  missing_crash.crash_window.classification =
      index::UniqueReservationCrashWindowClassification::unknown;
  RequireStatus(
      index::AdmitUniqueReservationFinalityProtocol(missing_crash),
      index::UniqueReservationFinalityProtocolStatus::
          crash_window_classification_missing,
      "missing crash classification did not fail closed");

  auto missing_cleanup = ProtocolRequest();
  missing_cleanup.rollback_retry_cleanup.cleanup_horizon_engine_bound = false;
  RequireStatus(
      index::AdmitUniqueReservationFinalityProtocol(missing_cleanup),
      index::UniqueReservationFinalityProtocolStatus::
          cleanup_horizon_not_engine_bound,
      "unbound cleanup horizon did not fail closed");

  auto missing_cleanup_evidence = ProtocolRequest();
  missing_cleanup_evidence.rollback_retry_cleanup.cleanup_evidence_present =
      false;
  RequireStatus(
      index::AdmitUniqueReservationFinalityProtocol(missing_cleanup_evidence),
      index::UniqueReservationFinalityProtocolStatus::
          rollback_retry_cleanup_missing,
      "missing rollback/retry cleanup evidence did not fail closed");

  auto missing_protocol_claim = ProtocolRequest();
  missing_protocol_claim.unique_protocol_evidence_claimed = false;
  RequireStatus(
      index::AdmitUniqueReservationFinalityProtocol(missing_protocol_claim),
      index::UniqueReservationFinalityProtocolStatus::
          unique_protocol_evidence_missing,
      "missing CEIC-034 protocol evidence claim did not fail closed");

  auto inconsistent_active_outcome = ProtocolRequest();
  inconsistent_active_outcome.conflict_outcome =
      index::UniqueReservationConflictOutcome::active_conflict_refused;
  RequireStatus(
      index::AdmitUniqueReservationFinalityProtocol(
          inconsistent_active_outcome),
      index::UniqueReservationFinalityProtocolStatus::
          conflict_outcome_unresolved,
      "active-conflict outcome with a reserved ledger result did not fail closed");

  auto missing_rollback_cleanup_result = ProtocolRequest();
  missing_rollback_cleanup_result.conflict_outcome =
      index::UniqueReservationConflictOutcome::rollback_retry_released;
  missing_rollback_cleanup_result.rollback_retry_cleanup
      .ledger_cleanup_result_present = false;
  RequireStatus(
      index::AdmitUniqueReservationFinalityProtocol(
          missing_rollback_cleanup_result),
      index::UniqueReservationFinalityProtocolStatus::
          rollback_retry_cleanup_missing,
      "rollback/retry outcome without cleanup result did not fail closed");
}

void AuthorityParticipationAndReadinessOverclaimFailClosed() {
  auto reference = ProtocolRequest();
  reference.reference_local_participation = true;
  RequireStatus(
      index::AdmitUniqueReservationFinalityProtocol(reference),
      index::UniqueReservationFinalityProtocolStatus::
          reference_policy_cluster_participation,
      "reference local participation did not fail closed");

  auto policy = ProtocolRequest();
  policy.policy_local_participation = true;
  RequireStatus(
      index::AdmitUniqueReservationFinalityProtocol(policy),
      index::UniqueReservationFinalityProtocolStatus::
          reference_policy_cluster_participation,
      "policy local participation did not fail closed");

  auto cluster = ProtocolRequest();
  cluster.cluster_local_participation = true;
  RequireStatus(
      index::AdmitUniqueReservationFinalityProtocol(cluster),
      index::UniqueReservationFinalityProtocolStatus::
          reference_policy_cluster_participation,
      "cluster local participation did not fail closed");

  auto authority = ProtocolRequest();
  authority.authority_boundary.transaction_finality_authority = true;
  RequireStatus(
      index::AdmitUniqueReservationFinalityProtocol(authority),
      index::UniqueReservationFinalityProtocolStatus::
          forbidden_authority_claim,
      "transaction finality authority claim did not fail closed");

  auto wal = ProtocolRequest();
  wal.authority_boundary.wal_authority = true;
  RequireStatus(
      index::AdmitUniqueReservationFinalityProtocol(wal),
      index::UniqueReservationFinalityProtocolStatus::
          forbidden_authority_claim,
      "WAL authority claim did not fail closed");

  auto enterprise = ProtocolRequest();
  enterprise.enterprise_ready_claimed = true;
  RequireStatus(
      index::AdmitUniqueReservationFinalityProtocol(enterprise),
      index::UniqueReservationFinalityProtocolStatus::readiness_overclaim,
      "enterprise readiness overclaim did not fail closed");

  auto ceic041 = ProtocolRequest();
  ceic041.ceic_041_crash_matrix_claimed = true;
  RequireStatus(
      index::AdmitUniqueReservationFinalityProtocol(ceic041),
      index::UniqueReservationFinalityProtocolStatus::readiness_overclaim,
      "CEIC-041 overclaim did not fail closed");

  auto ceic042 = ProtocolRequest();
  ceic042.ceic_042_readiness_gate_claimed = true;
  RequireStatus(
      index::AdmitUniqueReservationFinalityProtocol(ceic042),
      index::UniqueReservationFinalityProtocolStatus::readiness_overclaim,
      "CEIC-042 overclaim did not fail closed");
}

}  // namespace

int main() {
  ValidImmediateModeAdmitsProtocolEvidenceOnly();
  ValidDeferredModeRequiresCommitProbe();
  DuplicateConflictRefusalIsDeterministic();
  ActiveConflictRefusalIsDeterministic();
  RollbackRetryCleanupAdmitsAfterRelease();
  RequiredEvidenceFailuresFailClosed();
  AuthorityParticipationAndReadinessOverclaimFailClosed();
  std::cout << "ceic_034_unique_reservation_finality_protocol_gate=pass\n";
  return EXIT_SUCCESS;
}
