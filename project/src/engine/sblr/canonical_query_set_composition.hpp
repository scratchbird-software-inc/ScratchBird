// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "canonical_query_execute.hpp"
#include "canonical_query_object_free_composition_support.hpp"
#include "canonical_query_set_registration.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace scratchbird::engine::sblr {

namespace api = scratchbird::engine::internal_api;
namespace exec = scratchbird::engine::executor;

struct PreparedSetOperationRoot {
  bool ok{false};
  std::vector<exec::ExecutorColumnDescriptor> result_columns;
  std::vector<exec::CanonicalResultColumnBinding> result_bindings;
  std::vector<exec::CanonicalSetOperationCollationBinding>
      collation_bindings;
  std::string detail;
};

struct LiveSetOperationProfile {
  bool matched{false};
  exec::CanonicalSetOperationKind operation =
      exec::CanonicalSetOperationKind::kUnion;
  exec::CanonicalSetOperationAlignment alignment =
      exec::CanonicalSetOperationAlignment::kOrdinal;
  exec::CanonicalSetOperationQuantifier quantifier =
      exec::CanonicalSetOperationQuantifier::kAll;
  exec::CanonicalSetOperationEqualityProfile equality_profile =
      exec::CanonicalSetOperationEqualityProfile::kExactTyped;
  exec::CanonicalSetOperationTypeProfile type_profile =
      exec::CanonicalSetOperationTypeProfile::kExact;
  std::string implementation_id;
  std::string physical_semantic_id;
  std::string identity_component;
  std::string operation_name;
};

struct PreparedLiveSetNode {
  LiveSetOperationProfile profile;
  PreparedSetOperationRoot prepared;
  std::size_t maximum_output_row_count{0};
  std::size_t maximum_equality_comparison_count{0};
};

struct MaterializedSetOperationPlanningState {
  MaterializedValues values;
  std::uint64_t output_bound{0};
  std::uint64_t comparison_bound{0};
  std::uint64_t work_bound{0};
};

// These narrow adapters expose set-operation preparation that remains shared
// with admitted model-family compositions. They do not select or execute a
// plan, access storage, or create transaction authority.
LiveSetOperationProfile ResolveLiveSetOperationProfileForComposition(
    std::string_view semantic_variant_id);

PreparedSetOperationRoot PrepareSetOperationRootForComposition(
    const api::EngineRequestContext& context,
    const api::TypedRelationalDag& dag,
    const scratchbird::engine::planner::CanonicalLogicalRelationalNode& root,
    const MaterializedValues& left,
    const MaterializedValues& right,
    const LiveSetOperationProfile& profile);

bool BoundSetOperationEqualityComparisonsForComposition(
    const PreparedSetOperationRoot& prepared,
    const LiveSetOperationProfile& profile,
    std::uint64_t maximum_input_row_count,
    std::uint64_t* comparison_bound,
    std::uint64_t* collation_comparison_count);

LiveSetRegistrationProfiles MakeLiveSetRegistrationProfilesForComposition(
    const std::unordered_map<std::uint64_t, PreparedLiveSetNode>& prepared);

MaterializedSetOperationPlanningState
MaterializeSetOperationPlanningStateForComposition(
    const PreparedSetOperationRoot& prepared,
    const LiveSetOperationProfile& profile,
    const MaterializedValues& left,
    const MaterializedValues& right);

CanonicalObjectFreeValuesExecutionResult
ExecuteCanonicalObjectFreeSetOperationQuery(
    const CanonicalObjectFreeValuesExecutionRequest& request);

CanonicalObjectFreeValuesExecutionResult
ExecuteCanonicalObjectFreeNestedSetOperationQuery(
    const CanonicalObjectFreeValuesExecutionRequest& request);

}  // namespace scratchbird::engine::sblr
