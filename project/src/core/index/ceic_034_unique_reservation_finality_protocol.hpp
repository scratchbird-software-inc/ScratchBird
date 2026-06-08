// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SB-CEIC-034-UNIQUE-RESERVATION-FINALITY-PROTOCOL-ANCHOR
// CEIC_034_UNIQUE_RESERVATION_FINALITY_PROTOCOL

#include "btree_unique_durable_provider_closure.hpp"
#include "unique_index_reservation_ledger.hpp"

#include <string>
#include <vector>

namespace scratchbird::core::index {

using scratchbird::core::platform::u32;
using scratchbird::core::platform::u64;
using scratchbird::transaction::mga::TransactionState;

inline constexpr const char*
    kCeic034UniqueReservationFinalityProtocolGateToken =
        "CEIC_034.unique_btree_reservation_finality_protocol";

enum class UniqueReservationFinalityProtocolMode : u32 {
  immediate = 1,
  deferred = 2
};

enum class UniqueReservationConflictOutcome : u32 {
  none = 1,
  duplicate_refused = 2,
  active_conflict_refused = 3,
  rollback_retry_released = 4,
  unresolved = 5
};

enum class UniqueReservationCrashWindowClassification : u32 {
  clean_no_crash = 1,
  crash_before_reservation_publish = 2,
  crash_after_reservation_before_commit_probe = 3,
  crash_after_commit_probe_before_mga_commit = 4,
  crash_after_mga_commit_before_ledger_publish = 5,
  retry_after_rollback_cleanup = 6,
  unknown = 7
};

enum class UniqueReservationFinalityProtocolStatus : u32 {
  admitted_immediate_protocol_evidence = 1,
  admitted_deferred_protocol_evidence = 2,
  unsupported_family = 3,
  ceic033_closure_not_admitted = 4,
  duplicate_preflight_missing = 5,
  reservation_identity_missing = 6,
  mga_transaction_binding_missing = 7,
  deferred_commit_probe_missing = 8,
  rollback_retry_cleanup_missing = 9,
  duplicate_conflict_refused = 10,
  conflict_outcome_unresolved = 11,
  crash_window_classification_missing = 12,
  cleanup_horizon_not_engine_bound = 13,
  donor_policy_cluster_participation = 14,
  forbidden_authority_claim = 15,
  readiness_overclaim = 16,
  reservation_ledger_result_missing = 17,
  unique_protocol_evidence_missing = 18,
  active_conflict_refused = 19
};

struct UniqueReservationProtocolAuthorityBoundary {
  bool transaction_finality_authority = false;
  bool visibility_authority = false;
  bool authorization_security_authority = false;
  bool security_authority = false;
  bool recovery_authority = false;
  bool parser_authority = false;
  bool donor_authority = false;
  bool wal_authority = false;
  bool benchmark_authority = false;
  bool optimizer_plan_authority = false;
  bool index_finality_authority = false;
  bool provider_finality_authority = false;
  bool cluster_authority = false;
  bool agent_action_authority = false;
};

struct UniqueReservationIdentityProof {
  TypedUuid index_uuid;
  TypedUuid table_uuid;
  TypedUuid constraint_uuid;
  TypedUuid row_uuid;
  TypedUuid version_uuid;
  TypedUuid transaction_uuid;
  u64 local_transaction_id = 0;
  std::vector<byte> encoded_key;
  bool reservation_identity_bound_to_duplicate_preflight = false;
  bool reservation_identity_bound_to_ledger = false;
  bool reservation_identity_bound_to_unique_btree = false;
  std::string reservation_evidence_id;
};

struct UniqueReservationDuplicatePreflightProof {
  bool duplicate_preflight_performed = false;
  bool duplicate_preflight_engine_mga_bound = false;
  bool duplicate_preflight_result_resolved = false;
  bool duplicate_preflight_before_reservation = false;
  std::string duplicate_preflight_evidence_id;
};

struct UniqueReservationMGATransactionBindingProof {
  TypedUuid transaction_uuid;
  u64 local_transaction_id = 0;
  TransactionState observed_state = TransactionState::none;
  bool engine_transaction_handle_bound = false;
  bool engine_mga_inventory_present = false;
  bool engine_mga_inventory_authoritative = false;
  bool durable_transaction_inventory_authoritative = false;
  bool transaction_matches_reservation = false;
  bool cleanup_horizon_engine_bound = false;
  std::string inventory_evidence_id;
  std::string transaction_evidence_token;
};

struct UniqueReservationCommitProbeProof {
  bool commit_probe_present = false;
  bool commit_probe_engine_mga_bound = false;
  bool commit_probe_scanned_reservation_ledger = false;
  bool commit_probe_result_resolved = false;
  std::string commit_probe_evidence_id;
  UniqueIndexReservationResult commit_probe_result;
};

struct UniqueReservationRollbackRetryCleanupProof {
  bool rollback_release_supported = false;
  bool retry_release_supported = false;
  bool cleanup_evidence_present = false;
  bool cleanup_horizon_engine_bound = false;
  bool reservation_ledger_cleanup_evidence_present = false;
  bool ledger_cleanup_result_present = false;
  std::string cleanup_evidence_id;
  UniqueIndexReservationResult cleanup_result;
};

struct UniqueReservationCrashWindowProof {
  UniqueReservationCrashWindowClassification classification =
      UniqueReservationCrashWindowClassification::unknown;
  bool classification_present = false;
  bool classification_engine_mga_bound = false;
  std::string classification_evidence_id;
};

struct UniqueReservationFinalityProtocolRequest {
  IndexFamily family = IndexFamily::unknown;
  UniqueReservationFinalityProtocolMode mode =
      UniqueReservationFinalityProtocolMode::immediate;
  BtreeUniqueDurableProviderClosureResult ceic033_closure;
  UniqueReservationIdentityProof reservation_identity;
  UniqueReservationDuplicatePreflightProof duplicate_preflight;
  UniqueReservationMGATransactionBindingProof mga_transaction_binding;
  UniqueIndexReservationResult reservation_result;
  UniqueReservationCommitProbeProof commit_probe;
  UniqueReservationRollbackRetryCleanupProof rollback_retry_cleanup;
  UniqueReservationConflictOutcome conflict_outcome =
      UniqueReservationConflictOutcome::unresolved;
  UniqueReservationCrashWindowProof crash_window;
  UniqueReservationProtocolAuthorityBoundary authority_boundary;
  bool donor_local_participation = false;
  bool policy_local_participation = false;
  bool cluster_local_participation = false;
  bool unique_protocol_evidence_claimed = true;
  bool enterprise_ready_claimed = false;
  bool all_index_readiness_claimed = false;
  bool ceic_041_crash_matrix_claimed = false;
  bool ceic_042_readiness_gate_claimed = false;
  std::vector<std::string> evidence;
};

struct UniqueReservationFinalityProtocolResult {
  Status status;
  bool admitted = false;
  bool fail_closed = true;
  bool unique_protocol_evidence = false;
  bool immediate_mode_evidence = false;
  bool deferred_mode_evidence = false;
  bool enterprise_ready_claimed = false;
  bool all_index_readiness_claimed = false;
  bool ceic_041_crash_matrix_claimed = false;
  bool ceic_042_readiness_gate_claimed = false;
  UniqueReservationFinalityProtocolStatus protocol_status =
      UniqueReservationFinalityProtocolStatus::ceic033_closure_not_admitted;
  UniqueReservationConflictOutcome conflict_outcome =
      UniqueReservationConflictOutcome::unresolved;
  std::string proof_token;
  DiagnosticRecord diagnostic;
  std::vector<std::string> evidence;

  bool ok() const { return status.ok() && admitted && !fail_closed; }
};

const char* UniqueReservationFinalityProtocolModeName(
    UniqueReservationFinalityProtocolMode mode);
const char* UniqueReservationConflictOutcomeName(
    UniqueReservationConflictOutcome outcome);
const char* UniqueReservationCrashWindowClassificationName(
    UniqueReservationCrashWindowClassification classification);
const char* UniqueReservationFinalityProtocolStatusName(
    UniqueReservationFinalityProtocolStatus status);
bool UniqueReservationProtocolAuthorityBoundaryClear(
    const UniqueReservationProtocolAuthorityBoundary& boundary);
UniqueReservationFinalityProtocolResult
AdmitUniqueReservationFinalityProtocol(
    const UniqueReservationFinalityProtocolRequest& request);

}  // namespace scratchbird::core::index
