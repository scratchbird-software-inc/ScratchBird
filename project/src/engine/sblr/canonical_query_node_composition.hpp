// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "canonical_query_correlated_registration.hpp"
#include "canonical_query_execute.hpp"
#include "canonical_query_recursive_registration.hpp"
#include "canonical_query_relational_registration.hpp"
#include "canonical_relational_expression.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace scratchbird::engine::sblr {

namespace api = scratchbird::engine::internal_api;
namespace exec = scratchbird::engine::executor;

struct LivePredicateSubqueryProfile {
  bool matched{false};
  LivePredicateSubqueryKind kind{LivePredicateSubqueryKind::kExists};
  api::EngineComparisonPredicateOperator comparison_operator =
      api::EngineComparisonPredicateOperator::unspecified;
  exec::CanonicalQuantifiedSubqueryQuantifier quantifier =
      exec::CanonicalQuantifiedSubqueryQuantifier::kAny;
  std::string required_operand_type;
  std::string implementation_id;
  std::string transformation_id;
};

using PreparedCorrelatedSubqueryRoot =
    LiveCorrelatedSubqueryRegistrationProfile;

// Preparation-only adapters retained by the coordinator because other
// admitted routes share them. They consume typed DAG authority but cannot
// select a plan, access storage, or create/finalize MGA authority.
LivePredicateSubqueryProfile
MatchLivePredicateSubqueryProfileForComposition(
    std::string_view semantic_variant_id);

LiveLateralSubqueryProfile MatchLiveLateralSubqueryProfileForComposition(
    std::string_view semantic_variant_id);

LiveRecursiveCteProfile MatchLiveRecursiveCteProfileForComposition(
    std::string_view semantic_variant_id);

bool PrepareInputRowBindingForComposition(
    const api::TypedRelationalDag& dag,
    std::uint32_t root_expression_id,
    const std::vector<std::uint32_t>& input_descriptor_ids,
    CanonicalRelationalExpressionRowBinding* row_binding,
    std::string* detail,
    std::uint64_t* preparation_peak_structural_bytes = nullptr);

bool CanonicalDescriptorFieldEqualsForComposition(
    const api::EngineDescriptor& descriptor,
    std::string_view key,
    std::optional<std::string_view> expected);

// Executes the admitted object-free node chain through the optimizer-selected
// ABI-v2 DAG. The engine-selected MGA statement context is borrowed and
// revalidated only; this module cannot construct snapshots or finalize a
// transaction.
CanonicalObjectFreeValuesExecutionResult
ExecuteCanonicalObjectFreeNodeDrivenCompositionQuery(
    const CanonicalObjectFreeValuesExecutionRequest& request);

}  // namespace scratchbird::engine::sblr
