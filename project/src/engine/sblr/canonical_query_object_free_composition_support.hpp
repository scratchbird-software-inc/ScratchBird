// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "canonical_query_execute.hpp"
#include "canonical_query_object_free_profile.hpp"
#include "canonical_query_projection_registration.hpp"
#include "canonical_query_sort_registration.hpp"
#include "canonical_relational_expression.hpp"

#include "engine/executor/executor_foundation.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace scratchbird::engine::sblr {

namespace api = scratchbird::engine::internal_api;
namespace exec = scratchbird::engine::executor;
namespace plan = scratchbird::engine::planner;

struct MaterializedValues {
  bool ok{false};
  exec::DescriptorBatch batch;
  std::vector<exec::CanonicalResultColumnBinding> result_bindings;
  std::string detail;
};

struct PreparedFilterRoot {
  bool ok{false};
  std::uint32_t predicate_expression_id{0};
  CanonicalRelationalExpressionRowBinding predicate_row_binding;
  std::vector<exec::CanonicalResultColumnBinding> result_bindings;
  std::string detail;
};

struct PreparedProjectRoot {
  bool ok{false};
  bool expression_projection{false};
  std::vector<std::size_t> projected_columns;
  std::vector<LiveProjectExpressionRegistration> expressions;
  exec::DescriptorBatch expression_output_batch;
  std::vector<exec::CanonicalResultColumnBinding> result_bindings;
  std::string detail;
};

struct PreparedLimitRoot {
  bool ok{false};
  std::vector<exec::CanonicalResultColumnBinding> result_bindings;
  std::string detail;
};

struct PreparedDistinctRoot {
  bool ok{false};
  std::vector<exec::CanonicalDescriptorOrderTerm> equality_terms;
  std::vector<exec::CanonicalResultColumnBinding> result_bindings;
  std::string detail;
};

struct PreparedGlobalAggregateRoot;

api::EngineApiResult Failure(
    const CanonicalObjectFreeValuesExecutionRequest& request,
    std::string diagnostic_id,
    std::string detail);

// Materializes one already-admitted object-free VALUES leaf into exact typed
// descriptors. It owns no plan selection, data access, or MGA authority.
MaterializedValues MaterializeValues(
    const api::TypedRelationalDag& dag,
    const plan::CanonicalLogicalRelationalNode& logical_node,
    const CanonicalRelationalExpressionRuntimeServices& expression_services);

// Narrow preparation gateways used by larger object-free compositions. They
// consume already-bound DAG/schema state and own no planning, storage, MGA
// snapshot, or transaction-finality authority.
PreparedFilterRoot PrepareFilterRootForComposition(
    const api::TypedRelationalDag& dag,
    const plan::CanonicalLogicalRelationalNode& root,
    const plan::CanonicalLogicalRelationalNode& input_node,
    const MaterializedValues& input);

PreparedProjectRoot PrepareExpressionProjectRootForComposition(
    const api::TypedRelationalDag& dag,
    const plan::CanonicalLogicalRelationalNode& root,
    const plan::CanonicalLogicalRelationalNode& input_node,
    const MaterializedValues& input,
    const CanonicalRelationalExpressionRuntimeServices& expression_services);

PreparedProjectRoot PrepareDescriptorDirectProjectRootForComposition(
    const api::TypedRelationalDag& dag,
    const plan::CanonicalLogicalRelationalNode& root,
    const plan::CanonicalLogicalRelationalNode& input_node,
    const MaterializedValues& input);

PreparedGlobalAggregateRoot PrepareGlobalAggregateRootForComposition(
    const api::TypedRelationalDag& dag,
    const plan::CanonicalLogicalRelationalNode& root,
    const plan::CanonicalLogicalRelationalNode& input_node,
    const MaterializedValues& input,
    exec::CanonicalAggregateFunction function,
    bool count_star,
    bool distinct,
    bool has_filter,
    std::uint32_t expected_output_ordinal = 0,
    bool allow_sibling_outputs = false);

PreparedDistinctRoot PrepareQueryDistinctRootForComposition(
    const api::EngineRequestContext& context,
    const api::TypedRelationalDag& dag,
    const plan::CanonicalLogicalRelationalNode& distinct_node,
    const plan::CanonicalLogicalRelationalNode& input_node,
    const MaterializedValues& input);

PreparedLimitRoot PrepareLimitRootForComposition(
    const api::TypedRelationalDag& dag,
    const plan::CanonicalLogicalRelationalNode& root,
    const plan::CanonicalLogicalRelationalNode& input_node,
    const MaterializedValues& input);

bool PrepareCanonicalSortOrderTermForComposition(
    const api::EngineRequestContext& context,
    const plan::CanonicalLogicalPropertyOrderingTerm& logical_term,
    const exec::ExecutorColumnDescriptor& column,
    std::size_t column_ordinal,
    exec::CanonicalDescriptorOrderTerm* term,
    std::string* detail);

PreparedSortRoot PrepareSortRootForComposition(
    const api::EngineRequestContext& context,
    const api::TypedRelationalDag& dag,
    const plan::CanonicalLogicalPropertyCatalog& properties,
    const plan::CanonicalLogicalRelationalNode& root,
    const plan::CanonicalLogicalRelationalNode& input_node,
    const MaterializedValues& input);

PreparedSortRoot PrepareExpressionSortRootForComposition(
    const api::EngineRequestContext& context,
    const api::TypedRelationalDag& dag,
    const plan::CanonicalLogicalPropertyCatalog& properties,
    const plan::CanonicalLogicalRelationalNode& root,
    const plan::CanonicalLogicalRelationalNode& input_node,
    const MaterializedValues& input,
    const CanonicalRelationalExpressionRuntimeServices& expression_services);

bool EvaluateNonNegativeRowBoundForComposition(
    CanonicalRelationalExpressionRuntime* runtime,
    std::uint32_t expression_id,
    std::uint64_t* value,
    std::string* detail);

LiveProjectRegistrationProfile
MakeLiveProjectRegistrationProfileForComposition(
    const PreparedProjectRoot& prepared_root);

api::EngineApiResult SuccessfulApiResult(
    const CanonicalObjectFreeValuesExecutionRequest& request,
    const api::CanonicalOptimizerSelectedExecutionResult& execution);

// Typed preparation adapters retain the coordinator's existing descriptor
// authority and exact signed-integer identity checks without exposing storage.
void BindCanonicalPersistedRowDescriptorAuthorityForComposition(
    const api::EngineRequestContext& context,
    CanonicalRelationalExpressionRuntimeServices* services);

unsigned ExactBoundedSignedIntegerTypeRankForComposition(
    std::string_view type_uuid);

// Enters the coordinator-owned selected-DAG execution boundary. The gateway
// revalidates the supplied engine-selected MGA statement context but cannot
// create a snapshot or publish transaction finality.
api::CanonicalOptimizerSelectedExecutionResult
ExecuteSelectedCanonicalObjectFreeDag(
    const api::EngineRequestContext& context,
    const api::CanonicalOptimizerSelectedExecutionRequest& request,
    const OrdinaryRuntimeMemoryReceipts& memory_receipts);

}  // namespace scratchbird::engine::sblr
