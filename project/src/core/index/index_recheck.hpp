// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SB-INDEX-RECHECK-CLOSURE-ANCHOR

#include "index_access_method.hpp"

namespace scratchbird::core::index {

using scratchbird::core::platform::u32;
using scratchbird::core::platform::u64;

struct IndexRecheckPolicy {
  bool require_mga_visibility = true;
  bool require_predicate_match = true;
  bool require_security_visibility = true;
  bool accept_lossy_without_exact_predicate = false;
};

enum class EngineOwnedExactRecheckStatus : u32 {
  admitted_to_executor = 1,
  missing_locator_uuid = 2,
  missing_local_transaction_id = 3,
  missing_mga_inventory_proof = 4,
  missing_mga_snapshot_proof = 5,
  stale_mga_snapshot = 6,
  missing_security_context = 7,
  missing_authorization_proof = 8,
  missing_predicate_proof = 9,
  missing_exact_source_proof = 10,
  lossy_or_approximate_without_exact_fallback = 11,
  missing_required_exact_rerank = 12,
  forbidden_authority_claim = 13,
  external_authority_refused = 14,
  successor_or_enterprise_overclaim = 15,
  missing_route_evidence = 16,
  mga_visibility_recheck_failed = 17,
  security_recheck_failed = 18,
  predicate_recheck_failed = 19
};

struct EngineOwnedExactRecheckRouteEvidence {
  IndexFamily family = IndexFamily::unknown;
  IndexRouteKind route = IndexRouteKind::unknown;
  bool candidate_route = true;
  bool lossy_candidate = false;
  bool approximate_candidate = false;
  bool exact_fallback_available = false;
  bool exact_rerank_required = false;
  bool exact_rerank_proven = false;
  bool vector_payload_required = false;
  bool document_payload_required = false;
  bool text_payload_required = false;
  bool graph_payload_required = false;
};

struct EngineOwnedExactRecheckProof {
  bool mga_visibility_proven = false;
  bool mga_inventory_proof_present = false;
  bool mga_snapshot_proof_present = false;
  bool mga_snapshot_fresh = false;
  bool security_context_present = false;
  bool authorization_proven = false;
  bool predicate_exactness_proven = false;
  bool exact_source_row_verified = false;
  bool exact_source_payload_verified = false;
  bool exact_vector_payload_verified = false;
  bool exact_document_payload_verified = false;
  bool exact_text_payload_verified = false;
  bool exact_graph_payload_verified = false;
  std::string mga_inventory_proof_id;
  std::string mga_snapshot_proof_id;
  std::string security_context_id;
  std::string authorization_proof_id;
  std::string predicate_proof_id;
  std::string exact_source_proof_id;
  std::string exact_fallback_proof_id;
  std::string exact_rerank_proof_id;
};

struct EngineOwnedExactRecheckAuthorityBoundary {
  bool transaction_finality_authority = false;
  bool visibility_authority = false;
  bool authorization_authority = false;
  bool security_authority = false;
  bool recovery_authority = false;
  bool parser_authority = false;
  bool reference_authority = false;
  bool wal_authority = false;
  bool provider_authority = false;
  bool benchmark_authority = false;
  bool optimizer_plan_authority = false;
  bool index_finality_authority = false;
  bool cluster_action_authority = false;
  bool local_cluster_authority = false;
  bool agent_action_authority = false;
};

struct EngineOwnedExactRecheckSuccessorClaims {
  bool ceic_038_family_classification_claimed = false;
  bool ceic_039_specialized_provider_closure_claimed = false;
  bool ceic_040_runtime_metrics_claimed = false;
  bool ceic_041_crash_matrix_claimed = false;
  bool ceic_042_readiness_drift_claimed = false;
  bool all_index_readiness_claimed = false;
  bool enterprise_readiness_claimed = false;
};

struct EngineOwnedExactRecheckRequest {
  IndexCandidate candidate;
  EngineOwnedExactRecheckRouteEvidence route_evidence;
  EngineOwnedExactRecheckProof proof;
  EngineOwnedExactRecheckAuthorityBoundary authority_boundary;
  EngineOwnedExactRecheckSuccessorClaims successor_claims;
};

struct EngineOwnedExactRecheckResult {
  Status status;
  bool admitted = false;
  bool fail_closed = true;
  bool engine_owned_evidence = true;
  bool row_admitted_to_executor = false;
  EngineOwnedExactRecheckStatus recheck_status =
      EngineOwnedExactRecheckStatus::missing_exact_source_proof;
  DiagnosticRecord diagnostic;
  std::vector<std::string> evidence;

  bool transaction_finality_authority = false;
  bool visibility_authority = false;
  bool authorization_authority = false;
  bool security_authority = false;
  bool recovery_authority = false;
  bool parser_authority = false;
  bool reference_authority = false;
  bool wal_authority = false;
  bool provider_authority = false;
  bool benchmark_authority = false;
  bool optimizer_plan_authority = false;
  bool index_finality_authority = false;
  bool cluster_action_authority = false;
  bool agent_action_authority = false;
  bool ceic_038_family_classification_claimed = false;
  bool ceic_039_specialized_provider_closure_claimed = false;
  bool ceic_040_runtime_metrics_claimed = false;
  bool ceic_041_crash_matrix_claimed = false;
  bool ceic_042_readiness_drift_claimed = false;
  bool all_index_readiness_claimed = false;
  bool enterprise_readiness_claimed = false;

  bool ok() const { return status.ok() && admitted && !fail_closed; }
};

IndexCandidatePipelineResult ApplyIndexRecheckPolicy(std::vector<IndexCandidate> candidates,
                                                     const IndexRecheckPolicy& policy);
const char* EngineOwnedExactRecheckStatusName(
    EngineOwnedExactRecheckStatus status);
bool EngineOwnedExactRecheckAuthorityBoundaryClear(
    const EngineOwnedExactRecheckAuthorityBoundary& boundary);
bool EngineOwnedExactRecheckSuccessorClaimsClear(
    const EngineOwnedExactRecheckSuccessorClaims& claims);
EngineOwnedExactRecheckResult ApplyEngineOwnedExactRecheck(
    const EngineOwnedExactRecheckRequest& request);

}  // namespace scratchbird::core::index
