// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SB-SPECIALIZED-PERSISTENT-PROVIDER-CLOSURE-ANCHOR
// CEIC_039_SPECIALIZED_PERSISTENT_PROVIDER_CLOSURE
#include "index_access_method.hpp"
#include "index_family_route_classification.hpp"
#include "index_mga_recovery_contract.hpp"
#include "index_recheck.hpp"

#include <string>
#include <vector>

namespace scratchbird::core::index {

using scratchbird::core::platform::u32;
using scratchbird::core::platform::u64;

enum class SpecializedPersistentProviderClass : u32 {
  bitmap_candidate = 1,
  brin_zone_summary_prune = 2,
  bloom_negative_prune = 3,
  full_text_inverted = 4,
  gin_multikey = 5,
  inverted_segment = 6,
  ngram_token = 7,
  sparse_wand_ranking = 8,
  spatial_candidate = 9,
  rtree_spatial = 10,
  gist_spatial = 11,
  spgist_spatial = 12,
  vector_exact = 13,
  vector_hnsw = 14,
  vector_ivf = 15,
  columnar_zone_summary_prune = 16,
  document_path = 17,
  graph_adjacency = 18,
  inherited_exact_provider = 19,
  unsupported = 20
};

enum class SpecializedPersistentProviderClosureStatus : u32 {
  admitted_specialized_provider_evidence = 1,
  unsupported_family = 2,
  already_closed_by_prior_slice = 3,
  reference_emulated_non_runtime = 4,
  policy_blocked_non_runtime = 5,
  cluster_external_provider_only = 6,
  provider_admission_not_admitted = 7,
  mga_recovery_contract_not_admitted = 8,
  route_classification_not_admitted = 9,
  exact_recheck_not_admitted = 10,
  provider_mga_identity_mismatch = 11,
  provider_class_mismatch = 12,
  durable_provider_evidence_missing = 13,
  generation_identity_missing = 14,
  cleanup_horizon_not_engine_bound = 15,
  validation_repair_rebuild_missing = 16,
  candidate_role_mismatch = 17,
  exact_fallback_recheck_rerank_missing = 18,
  forbidden_authority_claim = 19,
  successor_scope_overclaim = 20,
  enterprise_readiness_overclaim = 21
};

struct SpecializedProviderAuthorityBoundary {
  bool transaction_finality_authority = false;
  bool visibility_authority = false;
  bool authorization_authority = false;
  bool authorization_security_authority = false;
  bool security_authority = false;
  bool recovery_authority = false;
  bool parser_authority = false;
  bool reference_authority = false;
  bool wal_authority = false;
  bool provider_authority = false;
  bool benchmark_authority = false;
  bool optimizer_plan_authority = false;
  bool index_finality_authority = false;
  bool provider_finality_authority = false;
  bool row_truth_authority = false;
  bool final_row_authority = false;
  bool result_finality_authority = false;
  bool local_cluster_authority = false;
  bool cluster_action_authority = false;
  bool agent_action_authority = false;
};

struct SpecializedProviderSuccessorClaims {
  bool ceic_040_runtime_metric_producer_claimed = false;
  bool ceic_041_crash_corruption_matrix_claimed = false;
  bool ceic_042_readiness_drift_claimed = false;
  bool all_index_readiness_claimed = false;
  bool reference_dominance_claimed = false;
  bool enterprise_readiness_claimed = false;
};

struct SpecializedProviderFamilyDeclaration {
  IndexFamily family = IndexFamily::unknown;
  SpecializedPersistentProviderClass provider_class =
      SpecializedPersistentProviderClass::unsupported;
  IndexFamilyRouteSemantic semantic = IndexFamilyRouteSemantic::unsupported;
  std::string provider_search_key;
  std::string provider_artifact_kind;
  bool provider_class_declared = false;
  bool durable_provider_requirements_declared = false;
  bool exact_recheck_requirements_declared = false;
  bool generation_identity_requirements_declared = false;
  bool cleanup_horizon_requirements_declared = false;
  bool validation_repair_rebuild_requirements_declared = false;
  bool candidate_role_declared = false;
  bool cluster_external_provider_only = true;
};

struct SpecializedDurableProviderEvidence {
  bool durable_storage_integration_proven = false;
  bool family_specific_physical_payload_proven = false;
  bool provider_artifact_format_version_proven = false;
  bool provider_open_reopen_identity_proven = false;
  bool provider_payload_integrity_proven = false;
  bool provider_evidence_only_not_authority = true;
  std::string durable_storage_evidence_id;
  std::string provider_payload_evidence_id;
  std::string artifact_format_evidence_id;
  std::vector<std::string> durable_evidence_rows;
};

struct SpecializedGenerationIdentityProof {
  TypedUuid index_uuid;
  TypedUuid generation_uuid;
  TypedUuid root_or_segment_uuid;
  u64 generation_number = 0;
  u64 cow_generation_number = 0;
  u64 cleanup_generation_floor = 0;
  u64 oldest_active_transaction_id = 0;
  std::string provider_generation_id;
  std::string root_or_provider_identity_evidence_id;
  bool cow_generation_publish_proven = false;
  bool root_or_provider_identity_bound = false;
  bool provider_generation_matches_ceic031 = false;
  bool generation_matches_ceic032 = false;
  bool cleanup_identity_matches_ceic031_ceic032 = false;
  bool publish_after_durable_mga_evidence = false;
};

struct SpecializedCleanupHorizonProof {
  u64 oldest_active_transaction_id = 0;
  u64 cleanup_generation_floor = 0;
  bool engine_mga_horizon_bound = false;
  bool cleanup_uses_engine_horizon = false;
  bool cleanup_identity_matches_ceic031_ceic032 = false;
  bool provider_cleanup_evidence_only = true;
  std::string cleanup_evidence_id;
};

struct SpecializedValidationRepairRebuildProof {
  bool validation_proven = false;
  bool repair_supported = false;
  bool rebuild_supported = false;
  bool deterministic_diagnostics = false;
  bool recommendation_matches_ceic032 = false;
  bool evidence_only_not_crash_matrix = true;
  std::string validation_evidence_id;
  std::string repair_rebuild_evidence_id;
};

struct SpecializedCandidateDisciplineProof {
  bool negative_prune_only = false;
  bool summary_segment_prune_only = false;
  bool candidate_set_only = false;
  bool ranking_producer = false;
  bool seed_producer = false;
  bool exact_recheck_handoff_required = true;
  bool false_positive_accounting_declared = false;
  bool final_row_authority = false;
  bool row_truth_authority = false;
  bool result_finality_authority = false;
};

struct SpecializedExactFallbackRecheckProof {
  EngineOwnedExactRecheckResult exact_recheck_result;
  bool exact_recheck_result_consumed = false;
  bool exact_recheck_required = true;
  bool exact_fallback_required = false;
  bool exact_fallback_proven = false;
  bool exact_rerank_required = false;
  bool exact_rerank_proven = false;
  bool exact_source_payload_proven = false;
  std::string fallback_evidence_id;
  std::string rerank_evidence_id;
};

struct SpecializedPersistentProviderClosureRequest {
  IndexFamily family = IndexFamily::unknown;
  IndexRouteKind route = IndexRouteKind::unknown;
  std::string provider_id;
  IndexProviderAdmissionResult provider_admission;
  IndexMGARecoveryContractResult mga_recovery_contract;
  IndexRouteFamilyClassificationResult route_classification;
  SpecializedProviderFamilyDeclaration declaration;
  SpecializedDurableProviderEvidence durable_provider;
  SpecializedGenerationIdentityProof generation_identity;
  SpecializedCleanupHorizonProof cleanup;
  SpecializedValidationRepairRebuildProof validation_repair_rebuild;
  SpecializedCandidateDisciplineProof candidate_discipline;
  SpecializedExactFallbackRecheckProof exact_fallback_recheck;
  SpecializedProviderAuthorityBoundary authority_boundary;
  SpecializedProviderSuccessorClaims successor_claims;
  bool reference_local_participation = false;
  bool policy_local_participation = false;
  bool cluster_local_participation = false;
  bool durable_provider_evidence_claimed = true;
  std::vector<std::string> evidence;
};

struct SpecializedPersistentProviderClosureResult {
  Status status;
  bool admitted = false;
  bool fail_closed = true;
  bool specialized_provider_closure_claimed = false;
  bool durable_provider_evidence = false;
  bool ceic_040_runtime_metric_producer_claimed = false;
  bool ceic_041_crash_corruption_matrix_claimed = false;
  bool ceic_042_readiness_drift_claimed = false;
  bool all_index_readiness_claimed = false;
  bool reference_dominance_claimed = false;
  bool enterprise_readiness_claimed = false;
  SpecializedPersistentProviderClosureStatus closure_status =
      SpecializedPersistentProviderClosureStatus::
          provider_admission_not_admitted;
  SpecializedPersistentProviderClass provider_class =
      SpecializedPersistentProviderClass::unsupported;
  IndexRecoveryRecommendation recommendation;
  DiagnosticRecord diagnostic;
  std::vector<std::string> evidence;

  bool ok() const { return status.ok() && admitted && !fail_closed; }
};

const char* SpecializedPersistentProviderClassName(
    SpecializedPersistentProviderClass provider_class);
const char* SpecializedPersistentProviderClosureStatusName(
    SpecializedPersistentProviderClosureStatus status);
bool SpecializedProviderAuthorityBoundaryClear(
    const SpecializedProviderAuthorityBoundary& boundary);
bool SpecializedProviderSuccessorClaimsClear(
    const SpecializedProviderSuccessorClaims& claims);
bool SpecializedProviderFamilyInCeic039Scope(IndexFamily family);
SpecializedProviderFamilyDeclaration
BuildSpecializedProviderFamilyDeclaration(IndexFamily family);
SpecializedPersistentProviderClosureResult
AdmitSpecializedPersistentProviderClosure(
    const SpecializedPersistentProviderClosureRequest& request);

}  // namespace scratchbird::core::index
