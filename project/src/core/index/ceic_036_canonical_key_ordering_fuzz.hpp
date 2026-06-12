// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// CEIC-036 canonical key ordering and encoding fuzz proof.
// This proof layer is deterministic encoding/order evidence only. It does not
// decide row visibility, authorization, transaction finality, recovery, parser
// behavior, reference dominance, optimizer finality, cluster action, or agent action.

#include "index_key_encoding.hpp"

#include <string>
#include <vector>

namespace scratchbird::core::index {

struct Ceic036AuthorityBoundaryClaims {
  bool visibility_authority = false;
  bool authorization_security_authority = false;
  bool transaction_finality_authority = false;
  bool recovery_authority = false;
  bool parser_authority = false;
  bool reference_authority = false;
  bool wal_authority = false;
  bool benchmark_authority = false;
  bool optimizer_plan_authority = false;
  bool index_finality_authority = false;
  bool cluster_action_authority = false;
  bool agent_action_authority = false;
};

struct Ceic036CanonicalKeyOrderingFuzzRequest {
  IndexKeySemanticProfile semantic_profile;
  bool typed_comparable_payload_contract_declared = false;
  bool raw_textual_numeric_ordering_claimed = false;
  bool reference_comparison_recorded = false;
  bool reference_comparison_evidence_only = true;
  Ceic036AuthorityBoundaryClaims authority_claims;
};

struct Ceic036CanonicalKeyOrderingFuzzResult {
  Status status;
  bool fail_closed = false;
  bool canonical_ordering_proven = false;
  bool encoding_order_matches_semantic_order = false;
  bool typed_comparable_payload_contract_proven = false;
  bool raw_textual_numeric_ordering_rejected = false;
  bool null_ordering_proven = false;
  bool numeric_ordering_proven = false;
  bool text_collation_ordering_proven = false;
  bool binary_embedded_nul_ordering_proven = false;
  bool composite_ordering_proven = false;
  bool expression_envelope_validation_proven = false;
  bool partial_predicate_implication_evidence_proven = false;
  bool prefix_bounds_proven = false;
  bool reference_comparison_authority = false;
  bool ceic_037_exact_recheck_claimed = false;
  bool ceic_040_runtime_metrics_claimed = false;
  bool ceic_041_crash_matrix_claimed = false;
  bool ceic_042_readiness_drift_claimed = false;
  bool all_index_readiness_claimed = false;
  bool enterprise_readiness_claimed = false;
  Ceic036AuthorityBoundaryClaims authority_claims;
  std::vector<std::string> evidence;
  DiagnosticRecord diagnostic;

  bool ok() const { return status.ok() && canonical_ordering_proven; }
};

Ceic036CanonicalKeyOrderingFuzzResult
ProveCeic036CanonicalKeyOrderingAndEncodingFuzz(
    const Ceic036CanonicalKeyOrderingFuzzRequest& request);

}  // namespace scratchbird::core::index
