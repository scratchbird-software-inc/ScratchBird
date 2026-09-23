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
  core::platform::Uuid function_uuid;
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
  core::platform::Uuid window_property_uuid;
  core::platform::Uuid window_frame_descriptor_uuid;
};

inline constexpr GlobalRankingWindowProfile kGlobalRowNumberProfile{
    "window.row-number.v1", "sb.window.row_number",
    core::platform::Uuid{{0x01, 0x9d, 0xe5, 0xfc, 0x24, 0x00, 0x75, 0x39, 0xbc, 0xce, 0x00, 0xee, 0xf3, 0xae, 0x72, 0x20}}, "ROW_NUMBER", "int64"};
inline constexpr GlobalRankingWindowProfile kGlobalRankProfile{
    "window.rank.v1", "sb.window.rank",
    core::platform::Uuid{{0x01, 0x9d, 0xe5, 0xfc, 0x24, 0x00, 0x7b, 0x94, 0x87, 0x0d, 0x0d, 0xd7, 0x89, 0xca, 0x70, 0xab}}, "RANK", "int64"};
inline constexpr GlobalRankingWindowProfile kGlobalDenseRankProfile{
    "window.dense-rank.v1", "sb.window.dense_rank",
    core::platform::Uuid{{0x01, 0x9d, 0xe5, 0xfc, 0x24, 0x00, 0x74, 0x1d, 0xbe, 0xf0, 0xf0, 0x79, 0xfd, 0x3b, 0xa4, 0x94}}, "DENSE_RANK", "int64"};
inline constexpr GlobalRankingWindowProfile kGlobalPercentRankProfile{
    "window.percent-rank.v1", "sb.window.percent_rank",
    core::platform::Uuid{{0x01, 0x9d, 0xe5, 0xfc, 0x24, 0x00, 0x7d, 0x86, 0x86, 0xfe, 0x96, 0xf3, 0xf2, 0x7b, 0x5d, 0xd6}}, "PERCENT_RANK", "real64"};
inline constexpr GlobalRankingWindowProfile kGlobalCumeDistProfile{
    "window.cume-dist.v1", "sb.window.cume_dist",
    core::platform::Uuid{{0x01, 0x9d, 0xe5, 0xfc, 0x24, 0x00, 0x72, 0x1c, 0xbe, 0x64, 0x25, 0x68, 0xb6, 0x4a, 0x02, 0xb9}}, "CUME_DIST", "real64"};
inline constexpr GlobalRankingWindowProfile kGlobalNtileProfile{
    "window.ntile.v1", "sb.window.ntile",
    core::platform::Uuid{{0x01, 0x9d, 0xe5, 0xfc, 0x24, 0x00, 0x70, 0x47, 0x94, 0x74, 0x23, 0x2c, 0xa4, 0x88, 0xc0, 0x94}}, "NTILE", "int64"};
inline constexpr GlobalRankingWindowProfile kGlobalLagProfile{
    "window.lag.v1", "sb.window.lag",
    core::platform::Uuid{{0x01, 0x9d, 0xe5, 0xfc, 0x24, 0x00, 0x78, 0x2c, 0x84, 0x36, 0x9a, 0xc3, 0x10, 0x30, 0x17, 0x38}}, "LAG", "int64"};
inline constexpr GlobalRankingWindowProfile kGlobalLeadProfile{
    "window.lead.v1", "sb.window.lead",
    core::platform::Uuid{{0x01, 0x9d, 0xe5, 0xfc, 0x24, 0x00, 0x7a, 0x06, 0xbc, 0x3c, 0x67, 0x47, 0xcf, 0x5b, 0xe6, 0x6f}}, "LEAD", "int64"};
inline constexpr GlobalRankingWindowProfile kGlobalFirstValueProfile{
    "window.first-value.v1", "sb.window.first_value",
    core::platform::Uuid{{0x01, 0x9d, 0xe5, 0xfc, 0x24, 0x00, 0x72, 0x64, 0x90, 0xfb, 0xd2, 0x5b, 0xd0, 0xf8, 0x06, 0xf2}}, "FIRST_VALUE", "int64"};
inline constexpr GlobalRankingWindowProfile kGlobalLastValueProfile{
    "window.last-value.v1", "sb.window.last_value",
    core::platform::Uuid{{0x01, 0x9d, 0xe5, 0xfc, 0x24, 0x00, 0x7d, 0x23, 0xa5, 0xbe, 0x7e, 0xd3, 0xf1, 0xa5, 0xc3, 0xec}}, "LAST_VALUE", "int64"};
inline constexpr GlobalRankingWindowProfile kGlobalNthValueProfile{
    "window.nth-value.v1", "sb.window.nth_value",
    core::platform::Uuid{{0x01, 0x9d, 0xe5, 0xfc, 0x24, 0x00, 0x7d, 0xc9, 0x80, 0xe6, 0x9f, 0x2c, 0xcf, 0x08, 0x07, 0x6f}}, "NTH_VALUE", "int64"};

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
    core::platform::Uuid type_uuid);

bool ExactCanonicalBooleanWindowSourceForComposition(
    const api::RelationalTypeDescriptor& relational_descriptor,
    const api::EngineDescriptor& runtime_descriptor,
    bool runtime_nullable,
    core::platform::Uuid boolean_type_uuid,
    core::platform::Uuid function_uuid,
    core::platform::Uuid result_descriptor_uuid,
    core::platform::Uuid ordering_property_uuid,
    core::platform::Uuid window_property_uuid,
    core::platform::Uuid window_frame_descriptor_uuid);

bool ExactCanonicalScalarWindowOperandForComposition(
    const api::RelationalTypeDescriptor& relational_descriptor,
    const api::EngineDescriptor& runtime_descriptor,
    bool runtime_nullable,
    core::platform::Uuid function_uuid,
    core::platform::Uuid result_descriptor_uuid,
    core::platform::Uuid result_type_uuid,
    core::platform::Uuid counterpart_descriptor_uuid,
    core::platform::Uuid counterpart_type_uuid,
    bool same_operand_ordinal,
    core::platform::Uuid ordering_property_uuid,
    core::platform::Uuid window_property_uuid,
    core::platform::Uuid window_frame_descriptor_uuid);

bool ExactCanonicalBoundedSignedWindowSourceForComposition(
    const api::RelationalTypeDescriptor& relational_descriptor,
    const api::EngineDescriptor& runtime_descriptor,
    bool runtime_nullable,
    const std::array<core::platform::Uuid, 4>& bounded_signed_type_uuids,
    core::platform::Uuid function_uuid,
    core::platform::Uuid result_descriptor_uuid,
    core::platform::Uuid ordering_property_uuid,
    core::platform::Uuid window_property_uuid,
    core::platform::Uuid window_frame_descriptor_uuid);

bool ExactCanonicalBoundedSignedWindowOrderForComposition(
    const api::RelationalTypeDescriptor& relational_descriptor,
    const api::EngineDescriptor& runtime_descriptor,
    bool runtime_nullable,
    const std::array<core::platform::Uuid, 4>& bounded_signed_type_uuids,
    core::platform::Uuid function_uuid,
    core::platform::Uuid result_descriptor_uuid,
    core::platform::Uuid result_type_uuid,
    core::platform::Uuid ordering_property_uuid,
    core::platform::Uuid window_property_uuid,
    core::platform::Uuid window_frame_descriptor_uuid);

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
    const core::platform::Uuid& result_type_uuid,
    const core::platform::Uuid& order_type_uuid,
    const core::platform::Uuid& boolean_type_uuid,
    const std::array<core::platform::Uuid, 4>& bounded_signed_type_uuids,
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
    const core::platform::Uuid& int64_type_uuid,
    std::string_view family_label,
    bool allow_project_root = false);

// Registers the ROW_NUMBER callback over optimizer-published physical and
// engine-selected MGA authority. Borrowed authority is revalidated only; this
// factory cannot create, refresh, or finalize a transaction or snapshot.
exec::CanonicalPhysicalExecutorRegistration MakeLiveRowNumberRegistration(
    exec::ExecutorColumnDescriptor row_number_column,
    core::platform::Uuid deterministic_order_evidence_uuid,
    core::platform::Uuid capability_uuid,
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
    core::platform::Uuid function_uuid,
    core::platform::Uuid deterministic_order_evidence_uuid,
    core::platform::Uuid capability_uuid,
    std::size_t maximum_input_row_count,
    scratchbird::engine::internal_api::EngineRequestContext mga_context);

// Registers RANK/DENSE_RANK/PERCENT_RANK/CUME_DIST over a bounded,
// optimizer-published sorted input. The callback consumes explicit peer
// metadata and can only revalidate the engine-selected MGA statement context.
exec::CanonicalPhysicalExecutorRegistration MakeLivePeerRankingRegistration(
    exec::ExecutorColumnDescriptor ranking_column,
    exec::CanonicalDescriptorOrderTerm order_term,
    core::platform::Uuid deterministic_order_evidence_uuid,
    core::platform::Uuid capability_uuid,
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
    core::platform::Uuid window_frame_descriptor_uuid,
    core::platform::Uuid deterministic_order_evidence_uuid,
    core::platform::Uuid frame_property_binding_evidence_uuid,
    core::platform::Uuid capability_uuid,
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
    core::platform::Uuid window_frame_descriptor_uuid,
    core::platform::Uuid deterministic_order_evidence_uuid,
    core::platform::Uuid frame_property_binding_evidence_uuid,
    core::platform::Uuid capability_uuid,
    std::size_t maximum_input_row_count,
    std::size_t maximum_pair_comparisons,
    std::size_t maximum_effective_row_references,
    std::size_t maximum_transition_count,
    scratchbird::engine::internal_api::EngineRequestContext mga_context);

}  // namespace scratchbird::engine::sblr
