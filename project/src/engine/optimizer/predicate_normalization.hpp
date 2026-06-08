// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "optimizer_statistics_full.hpp"

#include <string>
#include <vector>

namespace scratchbird::engine::optimizer {

struct CanonicalPredicate {
  bool ok = false;
  std::string canonical_text;
  std::string digest;
  std::vector<std::string> diagnostics;
  std::vector<std::string> conjuncts;
  std::vector<std::string> searchable_expression_digests;
  bool boolean_equivalent = false;
  bool like_prefix_predicate = false;
  std::string like_prefix;
  std::string like_refusal_reason;
};

// SEARCH_KEY: SB_OPTIMIZER_CANONICAL_SBLR_EXPRESSION_MATCH
// Production expression matching is based on normalized SBLR expression-tree
// metadata, not parser SQL text. Names are labels only; UUIDs, descriptor
// digests, function UUIDs, literal digests, and canonical child order form the
// optimizer identity used for expression index matching.
struct CanonicalSblrExpressionNode {
  std::string operator_id;
  std::string descriptor_digest;
  std::string object_uuid;
  std::string function_uuid;
  std::string literal_digest;
  std::vector<CanonicalSblrExpressionNode> children;
  bool commutative = false;
  bool normalized_sblr_metadata = true;
  bool raw_sql_text_present = false;
  bool parser_execution_authority_claimed = false;
  bool donor_or_legacy_authority_claimed = false;
  bool name_authority_claimed = false;
};

struct CanonicalSblrExpression {
  bool ok = false;
  std::string canonical_text;
  std::string digest;
  std::vector<std::string> searchable_expression_digests;
  std::vector<std::string> diagnostics;
  std::vector<std::string> evidence;
};

struct PredicateIndexMatchRequest {
  std::string query_predicate_text;
  std::string descriptor_digest;
  std::string collation_identity;
  bool base_row_mga_recheck_planned = true;
  bool base_row_security_recheck_planned = true;
  IndexStats index;
};

struct PredicateIndexMatchResult {
  bool matches = false;
  std::string canonical_predicate_text;
  std::string canonical_predicate_digest;
  std::vector<std::string> acceptance_reasons;
  std::vector<std::string> refusal_reasons;
};

struct SblrExpressionIndexMatchRequest {
  CanonicalSblrExpressionNode query_expression;
  std::string descriptor_digest;
  std::string collation_identity;
  bool base_row_mga_recheck_planned = true;
  bool base_row_security_recheck_planned = true;
  IndexStats index;
};

struct SblrExpressionIndexMatchResult {
  bool matches = false;
  std::string canonical_expression_text;
  std::string canonical_expression_digest;
  std::vector<std::string> acceptance_reasons;
  std::vector<std::string> refusal_reasons;
  std::vector<std::string> evidence;
};

CanonicalPredicate CanonicalizePredicateText(const std::string& predicate_text);
CanonicalPredicate CanonicalizeExpressionText(const std::string& expression_text);
std::string CanonicalPredicateDigest(const std::string& canonical_text);
CanonicalSblrExpression CanonicalizeSblrExpressionTree(
    const CanonicalSblrExpressionNode& expression);
std::string CanonicalSblrExpressionDigest(const std::string& canonical_text);
std::string DeterministicPredicateDescriptorText(const CanonicalPredicate& predicate);
bool CanonicalPredicateImplies(const CanonicalPredicate& query,
                               const CanonicalPredicate& required);
PredicateIndexMatchResult MatchPredicateToIndex(const PredicateIndexMatchRequest& request);
SblrExpressionIndexMatchResult MatchSblrExpressionToIndex(
    const SblrExpressionIndexMatchRequest& request);

}  // namespace scratchbird::engine::optimizer
