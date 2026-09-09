// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "engine/executor/executor_foundation.hpp"
#include "engine/internal_api/api_types.hpp"
#include "engine/optimizer/relational_planner.hpp"
#include "query/expression_api.hpp"
#include "query/plan_api.hpp"

#include <cstddef>
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace scratchbird::engine::sblr {

namespace api = scratchbird::engine::internal_api;
namespace exec = scratchbird::engine::executor;
namespace plan = scratchbird::engine::planner;

struct PreparedSortRoot;

// Immutable identity/profile data shared by window binding and callback
// registration. It carries no planning, execution, or transaction authority.
struct GlobalRankingWindowProfile {
  std::string_view semantic_variant_id;
  std::string_view builtin_id;
  std::string_view function_uuid;
  std::string_view display_name;
  std::string_view result_type_name;
};

struct PreparedGlobalRowNumberWindowBinding {
  bool ok{false};
  std::string diagnostic_id{"SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1"};
  std::string detail;
  const api::RelationalWindowInvocationRecord* invocation{nullptr};
  const api::RelationalExpressionRecord* function{nullptr};
  const api::RelationalTypeDescriptor* result_descriptor{nullptr};
  std::vector<const api::RelationalOutputRecord*> outputs;
  std::optional<api::EngineTypedValue> ntile_bucket_count_operand;
  std::optional<std::size_t> navigation_value_column;
  std::optional<api::EngineTypedValue> nth_value_position_operand;
  bool aggregate_count_star{false};
  std::string window_property_uuid;
  std::string window_frame_descriptor_uuid;
};

inline constexpr GlobalRankingWindowProfile kGlobalRowNumberProfile{
    "window.row-number.v1", "sb.window.row_number",
    "019de5fc-2400-7539-bcce-00eef3ae7220", "ROW_NUMBER", "int64"};
inline constexpr GlobalRankingWindowProfile kGlobalRankProfile{
    "window.rank.v1", "sb.window.rank",
    "019de5fc-2400-7b94-870d-0dd789ca70ab", "RANK", "int64"};
inline constexpr GlobalRankingWindowProfile kGlobalDenseRankProfile{
    "window.dense-rank.v1", "sb.window.dense_rank",
    "019de5fc-2400-741d-bef0-f079fd3ba494", "DENSE_RANK", "int64"};
inline constexpr GlobalRankingWindowProfile kGlobalPercentRankProfile{
    "window.percent-rank.v1", "sb.window.percent_rank",
    "019de5fc-2400-7d86-86fe-96f3f27b5dd6", "PERCENT_RANK", "real64"};
inline constexpr GlobalRankingWindowProfile kGlobalCumeDistProfile{
    "window.cume-dist.v1", "sb.window.cume_dist",
    "019de5fc-2400-721c-be64-2568b64a02b9", "CUME_DIST", "real64"};
inline constexpr GlobalRankingWindowProfile kGlobalNtileProfile{
    "window.ntile.v1", "sb.window.ntile",
    "019de5fc-2400-7047-9474-232ca488c094", "NTILE", "int64"};
inline constexpr GlobalRankingWindowProfile kGlobalLagProfile{
    "window.lag.v1", "sb.window.lag",
    "019de5fc-2400-782c-8436-9ac310301738", "LAG", "int64"};
inline constexpr GlobalRankingWindowProfile kGlobalLeadProfile{
    "window.lead.v1", "sb.window.lead",
    "019de5fc-2400-7a06-bc3c-6747cf5be66f", "LEAD", "int64"};
inline constexpr GlobalRankingWindowProfile kGlobalFirstValueProfile{
    "window.first-value.v1", "sb.window.first_value",
    "019de5fc-2400-7264-90fb-d25bd0f806f2", "FIRST_VALUE", "int64"};
inline constexpr GlobalRankingWindowProfile kGlobalLastValueProfile{
    "window.last-value.v1", "sb.window.last_value",
    "019de5fc-2400-7d23-a5be-7ed3f1a5c3ec", "LAST_VALUE", "int64"};
inline constexpr GlobalRankingWindowProfile kGlobalNthValueProfile{
    "window.nth-value.v1", "sb.window.nth_value",
    "019de5fc-2400-7dc9-80e6-9f2ccf08076f", "NTH_VALUE", "int64"};

inline constexpr std::uint64_t kRealRankingRatioTextMaximumBytes = 36;
inline constexpr std::uint64_t
    kRealRankingConversionWorkspaceMaximumBytes =
        2 * kRealRankingRatioTextMaximumBytes;

// Narrow binding adapters used by the node-composition module. They validate
// optimizer-published descriptors and properties only; they cannot select a
// plan, access storage, or create transaction/snapshot authority.
bool DirectValueWindowUsesExactTypeForComposition(
    const api::TypedRelationalDag& dag,
    std::uint32_t relation_node_id,
    std::string_view expected_builtin_id,
    std::string_view type_uuid);

bool ExactCanonicalBooleanWindowSourceForComposition(
    const api::RelationalTypeDescriptor& relational_descriptor,
    const api::EngineDescriptor& runtime_descriptor,
    bool runtime_nullable,
    std::string_view boolean_type_uuid,
    std::string_view function_uuid,
    std::string_view result_descriptor_uuid,
    std::string_view ordering_property_uuid,
    std::string_view window_property_uuid,
    std::string_view window_frame_descriptor_uuid);

bool ExactCanonicalScalarWindowOperandForComposition(
    const api::RelationalTypeDescriptor& relational_descriptor,
    const api::EngineDescriptor& runtime_descriptor,
    bool runtime_nullable,
    std::string_view function_uuid,
    std::string_view result_descriptor_uuid,
    std::string_view result_type_uuid,
    std::string_view counterpart_descriptor_uuid,
    std::string_view counterpart_type_uuid,
    bool same_operand_ordinal,
    std::string_view ordering_property_uuid,
    std::string_view window_property_uuid,
    std::string_view window_frame_descriptor_uuid);

bool ExactCanonicalBoundedSignedWindowSourceForComposition(
    const api::RelationalTypeDescriptor& relational_descriptor,
    const api::EngineDescriptor& runtime_descriptor,
    bool runtime_nullable,
    const std::array<std::string, 4>& bounded_signed_type_uuids,
    std::string_view function_uuid,
    std::string_view result_descriptor_uuid,
    std::string_view ordering_property_uuid,
    std::string_view window_property_uuid,
    std::string_view window_frame_descriptor_uuid);

bool ExactCanonicalBoundedSignedWindowOrderForComposition(
    const api::RelationalTypeDescriptor& relational_descriptor,
    const api::EngineDescriptor& runtime_descriptor,
    bool runtime_nullable,
    const std::array<std::string, 4>& bounded_signed_type_uuids,
    std::string_view function_uuid,
    std::string_view result_descriptor_uuid,
    std::string_view result_type_uuid,
    std::string_view ordering_property_uuid,
    std::string_view window_property_uuid,
    std::string_view window_frame_descriptor_uuid);

GlobalRankingWindowProfile GlobalAggregateWindowProfileForComposition(
    const api::TypedRelationalDag& dag,
    std::uint32_t relation_node_id);

PreparedGlobalRowNumberWindowBinding
PrepareGlobalRankingWindowBindingForComposition(
    const api::TypedRelationalDag& dag,
    const plan::CanonicalLogicalPropertyCatalog& logical_properties,
    const api::RelationalDagNode& consumer,
    const plan::CanonicalLogicalRelationalNode& logical_consumer,
    const plan::CanonicalLogicalRelationalNode& previous_logical,
    const PreparedSortRoot& prepared_sort,
    std::size_t materialized_column_count,
    std::size_t result_binding_count,
    const std::string& result_type_uuid,
    const std::string& order_type_uuid,
    const std::string& boolean_type_uuid,
    const std::array<std::string, 4>& bounded_signed_type_uuids,
    std::string_view family_label,
    const GlobalRankingWindowProfile& profile,
    bool allow_project_root = false);

PreparedGlobalRowNumberWindowBinding
PrepareGlobalRowNumberWindowBindingForComposition(
    const api::TypedRelationalDag& dag,
    const plan::CanonicalLogicalPropertyCatalog& logical_properties,
    const api::RelationalDagNode& consumer,
    const plan::CanonicalLogicalRelationalNode& logical_consumer,
    const plan::CanonicalLogicalRelationalNode& previous_logical,
    const PreparedSortRoot& prepared_sort,
    std::size_t materialized_column_count,
    std::size_t result_binding_count,
    const std::string& int64_type_uuid,
    std::string_view family_label,
    bool allow_project_root = false);

// Registers the ROW_NUMBER callback over optimizer-published physical and
// engine-selected MGA authority. Borrowed authority is revalidated only; this
// factory cannot create, refresh, or finalize a transaction or snapshot.
exec::CanonicalPhysicalExecutorRegistration MakeLiveRowNumberRegistration(
    exec::ExecutorColumnDescriptor row_number_column,
    std::string deterministic_order_evidence_uuid,
    std::string capability_uuid,
    std::size_t maximum_input_row_count,
    scratchbird::engine::internal_api::EngineRequestContext mga_context,
    const scratchbird::engine::internal_api::EngineRequestContext*
        borrowed_mga_context = nullptr,
    const exec::CanonicalExecutionMgaAuthority* borrowed_mga_authority =
        nullptr);

// Registers NTILE over a bounded, optimizer-published sorted input. The
// callback consumes and revalidates an engine-selected MGA statement context;
// it cannot create, refresh, or finalize a transaction or snapshot.
exec::CanonicalPhysicalExecutorRegistration MakeLiveNtileRegistration(
    exec::ExecutorColumnDescriptor ntile_column,
    exec::CanonicalDescriptorOrderTerm order_term,
    scratchbird::engine::internal_api::EngineTypedValue bucket_count_operand,
    std::string function_uuid,
    std::string order_term_binding_evidence_uuid,
    std::string deterministic_order_evidence_uuid,
    std::string capability_uuid,
    std::size_t maximum_input_row_count,
    scratchbird::engine::internal_api::EngineRequestContext mga_context);

// Registers RANK/DENSE_RANK/PERCENT_RANK/CUME_DIST over a bounded,
// optimizer-published sorted input. The callback consumes explicit peer
// metadata and can only revalidate the engine-selected MGA statement context.
exec::CanonicalPhysicalExecutorRegistration MakeLivePeerRankingRegistration(
    exec::ExecutorColumnDescriptor ranking_column,
    exec::CanonicalDescriptorOrderTerm order_term,
    std::string order_term_binding_evidence_uuid,
    std::string deterministic_order_evidence_uuid,
    std::string capability_uuid,
    std::size_t maximum_input_row_count,
    std::size_t maximum_peer_comparisons,
    GlobalRankingWindowProfile profile,
    scratchbird::engine::internal_api::EngineRequestContext mga_context);

// Registers LAG/LEAD/FIRST_VALUE/LAST_VALUE/NTH_VALUE over a bounded,
// optimizer-published sorted input and frame. The callback only revalidates
// the engine-selected MGA statement context.
exec::CanonicalPhysicalExecutorRegistration
MakeLiveNavigationWindowRegistration(
    exec::ExecutorColumnDescriptor result_column,
    exec::CanonicalDescriptorOrderTerm order_term,
    std::size_t value_column,
    std::optional<scratchbird::engine::internal_api::EngineTypedValue>
        nth_value_position_operand,
    std::string window_frame_descriptor_uuid,
    std::string order_term_binding_evidence_uuid,
    std::string deterministic_order_evidence_uuid,
    std::string frame_property_binding_evidence_uuid,
    std::string capability_uuid,
    std::size_t maximum_input_row_count,
    std::size_t maximum_pair_comparisons,
    std::size_t maximum_effective_row_references,
    GlobalRankingWindowProfile profile,
    scratchbird::engine::internal_api::EngineRequestContext mga_context);

// Registers aggregate-window execution over a bounded, optimizer-published
// frame. Runtime work is checked against the published grant and the callback
// can only revalidate the engine-selected MGA statement context.
exec::CanonicalPhysicalExecutorRegistration
MakeLiveAggregateWindowRegistration(
    exec::ExecutorColumnDescriptor result_column,
    exec::CanonicalDescriptorOrderTerm order_term,
    std::optional<std::size_t> value_column,
    exec::CanonicalAggregateDescriptor aggregate_descriptor,
    std::string window_frame_descriptor_uuid,
    std::string order_term_binding_evidence_uuid,
    std::string deterministic_order_evidence_uuid,
    std::string frame_property_binding_evidence_uuid,
    std::string capability_uuid,
    std::size_t maximum_input_row_count,
    std::size_t maximum_pair_comparisons,
    std::size_t maximum_effective_row_references,
    std::size_t maximum_transition_count,
    scratchbird::engine::internal_api::EngineRequestContext mga_context);

}  // namespace scratchbird::engine::sblr
