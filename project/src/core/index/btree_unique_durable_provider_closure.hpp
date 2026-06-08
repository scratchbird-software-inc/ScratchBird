// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SB-BTREE-UNIQUE-DURABLE-PROVIDER-CLOSURE-ANCHOR
// CEIC_033_BTREE_UNIQUE_DURABLE_PROVIDER_CLOSURE

#include "index_access_method.hpp"
#include "index_mga_recovery_contract.hpp"

#include <string>
#include <vector>

namespace scratchbird::core::index {

using scratchbird::core::platform::u32;
using scratchbird::core::platform::u64;

enum class BtreeUniqueDurableProviderClosureStatus : u32 {
  admitted_durable_provider_evidence = 1,
  unsupported_family = 2,
  donor_policy_cluster_participation = 3,
  provider_admission_not_admitted = 4,
  mga_recovery_contract_not_admitted = 5,
  missing_generation_root_identity = 6,
  missing_split_merge_scan_proof = 7,
  unique_duplicate_proof_required = 8,
  cleanup_horizon_not_engine_bound = 9,
  crash_corruption_classification_absent = 10,
  repair_rebuild_recommendation_missing = 11,
  concurrency_evidence_missing = 12,
  forbidden_authority_claim = 13,
  enterprise_readiness_overclaim = 14,
  successor_scope_overclaim = 15,
  provider_mga_identity_mismatch = 16,
  durable_provider_evidence_missing = 17
};

struct BtreeUniqueClosureAuthorityBoundary {
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

struct BtreeGenerationRootProof {
  TypedUuid index_uuid;
  TypedUuid root_page_uuid;
  TypedUuid generation_uuid;
  u64 root_page_number = 0;
  u64 generation_number = 0;
  u64 cow_generation_number = 0;
  std::string provider_generation_id;
  bool cow_generation_publish_proven = false;
  bool root_identity_bound = false;
  bool root_reopen_identity_stable = false;
  bool provider_generation_matches_mga_contract = false;
  bool publish_after_durable_mga_evidence = false;
};

struct BtreeSplitMergeScanProof {
  bool split_capability_present = false;
  bool split_proof_present = false;
  bool merge_capability_present = false;
  bool merge_proof_present = false;
  bool ordered_scan_capability_present = false;
  bool ordered_scan_proof_present = false;
  bool point_lookup_capability_present = false;
  bool deterministic_page_format = false;
  bool page_integrity_proof_present = false;
  std::string structural_evidence_id;
};

struct UniqueDuplicateReservationProof {
  bool duplicate_preflight_proven = false;
  bool reservation_proof_present = false;
  bool reservation_engine_transaction_bound = false;
  bool duplicate_recheck_engine_bound = false;
  bool ceic_034_unique_finality_protocol_pending = true;
  std::string reservation_evidence_id;
};

struct BtreeCleanupHorizonProof {
  u64 oldest_active_transaction_id = 0;
  u64 cleanup_generation_floor = 0;
  bool engine_mga_horizon_bound = false;
  bool cleanup_uses_engine_horizon = false;
  bool provider_cleanup_evidence_only = true;
  std::string cleanup_evidence_id;
};

struct BtreeCrashCorruptionClosureProof {
  IndexCrashRecoveryClassification crash_classification =
      IndexCrashRecoveryClassification::unknown;
  IndexCorruptionClassification corruption_classification =
      IndexCorruptionClassification::unknown;
  bool crash_reopen_classification_present = false;
  bool corruption_classification_present = false;
  bool durable_provider_evidence_only = true;
  std::string classification_evidence_id;
};

struct BtreeRepairRebuildProof {
  bool validate_before_repair = false;
  bool repair_supported = false;
  bool rebuild_supported = false;
  bool deterministic_recommendation_present = false;
  bool recommendation_matches_mga_recovery_contract = false;
  std::string recommendation_evidence_id;
};

struct BtreeConcurrencyEvidenceHooks {
  bool generation_publish_fence_hook = false;
  bool root_publish_observer_hook = false;
  bool split_merge_latch_boundary_hook = false;
  bool concurrent_scan_mutation_boundary_hook = false;
  bool evidence_only_not_transaction_finality = true;
  std::string concurrency_evidence_id;
};

struct BtreeUniqueDurableProviderClosureRequest {
  IndexFamily family = IndexFamily::unknown;
  IndexRouteKind route = IndexRouteKind::unknown;
  std::string provider_id;
  IndexProviderAdmissionResult provider_admission;
  IndexMGARecoveryContractResult mga_recovery_contract;
  BtreeGenerationRootProof generation_root;
  BtreeSplitMergeScanProof split_merge_scan;
  UniqueDuplicateReservationProof unique_duplicate;
  BtreeCleanupHorizonProof cleanup;
  BtreeCrashCorruptionClosureProof crash_corruption;
  BtreeRepairRebuildProof repair_rebuild;
  BtreeConcurrencyEvidenceHooks concurrency;
  BtreeUniqueClosureAuthorityBoundary authority_boundary;
  bool donor_local_participation = false;
  bool policy_local_participation = false;
  bool cluster_local_participation = false;
  bool durable_provider_evidence_claimed = true;
  bool enterprise_ready_claimed = false;
  bool all_index_readiness_claimed = false;
  bool ceic_034_unique_protocol_pending = true;
  bool ceic_041_crash_matrix_pending = true;
  std::vector<std::string> evidence;
};

struct BtreeUniqueDurableProviderClosureResult {
  Status status;
  bool admitted = false;
  bool fail_closed = true;
  bool durable_provider_evidence = false;
  bool btree_unique_provider_closure_claimed = false;
  bool enterprise_ready_claimed = false;
  bool all_index_readiness_claimed = false;
  bool ceic_034_unique_protocol_pending = true;
  bool ceic_041_crash_matrix_pending = true;
  BtreeUniqueDurableProviderClosureStatus closure_status =
      BtreeUniqueDurableProviderClosureStatus::provider_admission_not_admitted;
  IndexRecoveryRecommendation recommendation;
  DiagnosticRecord diagnostic;
  std::vector<std::string> evidence;

  bool ok() const { return status.ok() && admitted && !fail_closed; }
};

const char* BtreeUniqueDurableProviderClosureStatusName(
    BtreeUniqueDurableProviderClosureStatus status);
bool BtreeUniqueClosureAuthorityBoundaryClear(
    const BtreeUniqueClosureAuthorityBoundary& boundary);
BtreeUniqueDurableProviderClosureResult
AdmitBtreeUniqueDurableProviderClosure(
    const BtreeUniqueDurableProviderClosureRequest& request);

}  // namespace scratchbird::core::index
