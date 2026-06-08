// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SB-HASH-DURABLE-PROVIDER-CLOSURE-ANCHOR
// CEIC_035_HASH_DURABLE_PROVIDER_CLOSURE

#include "index_access_method.hpp"
#include "index_filter_access.hpp"
#include "index_mga_recovery_contract.hpp"
#include "index_hash_page.hpp"

#include <string>
#include <vector>

namespace scratchbird::core::index {

using scratchbird::core::platform::u16;
using scratchbird::core::platform::u32;
using scratchbird::core::platform::u64;

enum class HashDurableProviderClosureStatus : u32 {
  admitted_durable_provider_evidence = 1,
  unsupported_family = 2,
  donor_policy_cluster_participation = 3,
  provider_admission_not_admitted = 4,
  mga_recovery_contract_not_admitted = 5,
  provider_mga_identity_mismatch = 6,
  missing_generation_directory_identity = 7,
  missing_hash_physical_report = 8,
  missing_directory_bucket_overflow_proof = 9,
  missing_hash_seed_provenance = 10,
  missing_v2_v3_hash_proof = 11,
  fixture_forced_collision_without_test_evidence = 12,
  missing_full_key_recheck = 13,
  missing_collision_chain_route_proof = 14,
  cleanup_horizon_not_engine_bound = 15,
  crash_corruption_classification_absent = 16,
  repair_rebuild_recommendation_missing = 17,
  adversarial_collision_benchmark_missing = 18,
  deterministic_diagnostics_missing = 19,
  forbidden_authority_claim = 20,
  durable_provider_evidence_missing = 21,
  successor_scope_overclaim = 22,
  enterprise_readiness_overclaim = 23
};

struct HashClosureAuthorityBoundary {
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

struct HashGenerationDirectoryProof {
  TypedUuid index_uuid;
  TypedUuid generation_uuid;
  u64 directory_page_number = 0;
  u64 generation_number = 0;
  u64 cow_generation_number = 0;
  u64 cleanup_generation_floor = 0;
  u64 oldest_active_transaction_id = 0;
  std::string provider_generation_id;
  std::string cleanup_horizon_evidence_id;
  bool cow_generation_publish_proven = false;
  bool directory_root_identity_bound = false;
  bool directory_reopen_identity_stable = false;
  bool provider_generation_matches_ceic031 = false;
  bool generation_matches_mga_contract = false;
  bool cleanup_identity_matches_ceic031_ceic032 = false;
  bool publish_after_durable_mga_evidence = false;
};

struct HashPhysicalStructureProof {
  scratchbird::storage::page::IndexHashPhysicalReport physical_report;
  bool physical_report_valid = false;
  bool directory_page_proof_present = false;
  bool bucket_page_proof_present = false;
  bool overflow_page_proof_present = false;
  bool bucket_split_or_directory_growth_proof_present = false;
  bool bucket_merge_or_compaction_proof_present = false;
  bool collision_chain_metadata_proof_present = false;
  bool deterministic_page_format = false;
  bool route_hash_proof_present = false;
  std::string structural_evidence_id;
};

struct HashSeedAndFingerprintProof {
  u16 hash_algorithm_version =
      scratchbird::storage::page::kIndexHashProductionDefaultAlgorithmVersion;
  bool hash_seed_engine_generated = false;
  bool hash_seed_protected = false;
  bool hash_seed_high64_protected = false;
  bool hash_seed_key_material_128_bits = false;
  bool hash_seed_redacted_from_diagnostics = false;
  bool hash_seed_entropy_source_recorded = false;
  bool keyed_v2_hash_supported = false;
  bool keyed_v3_128bit_fingerprint_supported = false;
  bool high_assurance_fingerprint_proof_present = false;
  bool forced_route_collision_hook_used = false;
  bool forced_fingerprint_collision_hook_used = false;
  bool test_fixture_forced_collision_evidence = false;
  std::string hash_seed_entropy_source;
  std::string seed_evidence_id;
};

struct HashFullKeyRecheckProof {
  HashEqualityProbe equality_probe;
  bool equality_decision_checked = false;
  bool mandatory_full_encoded_key_recheck = false;
  bool hash_match_is_candidate_only = true;
  bool collision_requires_full_key_compare = false;
  bool fingerprint_is_filter_not_authority = true;
  bool mga_recheck_required = true;
  bool security_recheck_required = true;
  bool candidate_set_only = true;
  std::string recheck_evidence_id;
};

struct HashCollisionChainProof {
  bool directory_route_hash_consumed = false;
  bool bucket_index_matches_route_hash = false;
  bool collision_root_metadata_checked = false;
  bool collision_chain_traversal_proven = false;
  bool collision_chain_cycle_refusal_proven = false;
  bool tombstones_excluded_from_probe = false;
  bool overflow_chain_metadata_checked = false;
  u32 max_collision_chain_length = 0;
  u32 max_overflow_depth = 0;
  std::string collision_evidence_id;
};

struct HashCleanupCompactionProof {
  u64 oldest_active_transaction_id = 0;
  u64 cleanup_generation_floor = 0;
  bool engine_mga_horizon_bound = false;
  bool cleanup_uses_engine_horizon = false;
  bool tombstone_cleanup_compaction_proven = false;
  bool overflow_compaction_proven = false;
  bool collision_chains_rebuilt_after_compaction = false;
  bool provider_cleanup_evidence_only = true;
  std::string cleanup_evidence_id;
};

struct HashCrashCorruptionRepairProof {
  IndexCrashRecoveryClassification crash_classification =
      IndexCrashRecoveryClassification::unknown;
  IndexCorruptionClassification corruption_classification =
      IndexCorruptionClassification::unknown;
  scratchbird::storage::page::IndexHashPhysicalCorruptionClass
      physical_corruption_class =
          scratchbird::storage::page::IndexHashPhysicalCorruptionClass::unknown;
  bool crash_reopen_classification_present = false;
  bool corruption_classification_present = false;
  bool durable_provider_evidence_only = true;
  bool validate_before_repair = false;
  bool repair_supported = false;
  bool rebuild_supported = false;
  bool deterministic_recommendation_present = false;
  bool recommendation_matches_mga_recovery_contract = false;
  std::string classification_evidence_id;
  std::string recommendation_evidence_id;
};

struct HashAdversarialCollisionBenchmarkProof {
  bool adversarial_collision_benchmark_present = false;
  bool deterministic_collision_fixture_isolated = false;
  bool benchmark_evidence_only = true;
  bool benchmark_clean_capability_claimed = false;
  bool collision_thresholds_recorded = false;
  bool rebuild_or_reseed_recommendation_recorded = false;
  bool support_bundle_rows_deterministic = false;
  std::string benchmark_evidence_id;
};

struct HashDurableProviderClosureRequest {
  IndexFamily family = IndexFamily::unknown;
  IndexRouteKind route = IndexRouteKind::unknown;
  std::string provider_id;
  IndexProviderAdmissionResult provider_admission;
  IndexMGARecoveryContractResult mga_recovery_contract;
  HashGenerationDirectoryProof generation_directory;
  HashPhysicalStructureProof physical_structure;
  HashSeedAndFingerprintProof seed_fingerprint;
  HashFullKeyRecheckProof full_key_recheck;
  HashCollisionChainProof collision_chain;
  HashCleanupCompactionProof cleanup;
  HashCrashCorruptionRepairProof crash_corruption_repair;
  HashAdversarialCollisionBenchmarkProof adversarial_benchmark;
  HashClosureAuthorityBoundary authority_boundary;
  bool donor_local_participation = false;
  bool policy_local_participation = false;
  bool cluster_local_participation = false;
  bool durable_provider_evidence_claimed = true;
  bool ceic_040_runtime_metric_producer_claimed = false;
  bool ceic_041_crash_matrix_claimed = false;
  bool ceic_042_readiness_drift_gate_claimed = false;
  bool enterprise_ready_claimed = false;
  bool all_index_readiness_claimed = false;
  std::vector<std::string> evidence;
};

struct HashDurableProviderClosureResult {
  Status status;
  bool admitted = false;
  bool fail_closed = true;
  bool durable_provider_evidence = false;
  bool hash_provider_closure_claimed = false;
  bool ceic_040_runtime_metric_producer_claimed = false;
  bool ceic_041_crash_matrix_claimed = false;
  bool ceic_042_readiness_drift_gate_claimed = false;
  bool enterprise_ready_claimed = false;
  bool all_index_readiness_claimed = false;
  HashDurableProviderClosureStatus closure_status =
      HashDurableProviderClosureStatus::provider_admission_not_admitted;
  IndexRecoveryRecommendation recommendation;
  DiagnosticRecord diagnostic;
  std::vector<std::string> evidence;

  bool ok() const { return status.ok() && admitted && !fail_closed; }
};

const char* HashDurableProviderClosureStatusName(
    HashDurableProviderClosureStatus status);
bool HashClosureAuthorityBoundaryClear(
    const HashClosureAuthorityBoundary& boundary);
HashDurableProviderClosureResult AdmitHashDurableProviderClosure(
    const HashDurableProviderClosureRequest& request);

}  // namespace scratchbird::core::index
