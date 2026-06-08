// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SB-PARTIAL-INDEX-IMPLICATION-ENGINE-ANCHOR
// Partial-index predicate proofs are optimizer/index admission evidence only.
// They never decide row visibility, authorization, transaction finality,
// cleanup eligibility, or recovery truth.

#include <string>
#include <vector>

namespace scratchbird::core::index {

struct PartialPredicateCanonicalForm {
  bool ok = false;
  std::string canonical_text;
  std::string digest;
  std::vector<std::string> diagnostics;
  std::vector<std::string> conjuncts;
  std::vector<std::string> searchable_expression_digests;
  std::vector<std::string> function_names;
  std::vector<std::string> unsafe_function_names;
  bool boolean_equivalent = false;
  bool like_prefix_predicate = false;
  std::string like_prefix;
  std::string like_refusal_reason;
};

struct PartialPredicateImplicationRequest {
  std::string query_predicate_text;
  std::string index_predicate_text;
  bool predicate_immutable = false;
  bool predicate_security_safe = false;
  bool descriptor_epoch_valid = false;
  bool resource_epoch_valid = false;
  bool collation_epoch_valid = false;
  bool function_epoch_valid = false;
  bool base_row_mga_recheck_planned = false;
  bool base_row_security_recheck_planned = false;
};

struct PartialPredicateImplicationProof {
  bool safe_to_consider_index = false;
  bool predicate_implied = false;
  bool base_row_mga_recheck_required = true;
  bool base_row_security_recheck_required = true;
  bool visibility_authority = false;
  bool authorization_authority = false;
  bool transaction_finality_authority = false;
  bool cleanup_authority = false;
  bool recovery_authority = false;
  std::string canonical_query_text;
  std::string canonical_query_digest;
  std::string canonical_index_predicate_text;
  std::string canonical_index_predicate_digest;
  std::vector<std::string> implied_query_conjuncts;
  std::vector<std::string> index_predicate_conjuncts;
  std::vector<std::string> remaining_query_conjuncts;
  std::vector<std::string> acceptance_reasons;
  std::vector<std::string> refusal_reasons;
  std::vector<std::string> evidence;
};

std::string PartialIndexPredicateDigest(const std::string& canonical_text);
PartialPredicateCanonicalForm CanonicalizePartialIndexPredicateText(
    const std::string& predicate_text);
PartialPredicateCanonicalForm CanonicalizePartialIndexExpressionText(
    const std::string& expression_text);
PartialPredicateImplicationProof ProvePartialIndexPredicateImplication(
    const PartialPredicateImplicationRequest& request);

}  // namespace scratchbird::core::index
