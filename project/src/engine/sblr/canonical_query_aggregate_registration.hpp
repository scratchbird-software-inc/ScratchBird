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

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace scratchbird::engine::sblr {

namespace exec = scratchbird::engine::executor;

struct PreparedAggregateValueBindingReceipt {
  std::size_t value_column{0};
  std::uint32_t descriptor_id{0};
  std::string descriptor_uuid;
  std::string type_uuid;
  bool nullable{false};
  std::string canonical_type_name;
  std::string encoded_descriptor;
};

struct PreparedGlobalAggregateRoot {
  bool ok{false};
  bool count_star{false};
  bool distinct{false};
  std::vector<std::size_t> value_columns;
  std::vector<std::uint32_t> value_descriptor_ids;
  std::vector<PreparedAggregateValueBindingReceipt>
      exact_value_binding_receipts;
  std::vector<scratchbird::engine::internal_api::EngineTypedValue>
      direct_arguments;
  std::optional<std::size_t> filter_column;
  std::uint32_t filter_descriptor_id{0};
  std::vector<exec::CanonicalDescriptorOrderTerm> aggregate_order_terms;
  std::string aggregate_separator{","};
  exec::CanonicalListaggOverflowMode listagg_overflow_mode =
      exec::CanonicalListaggOverflowMode::none;
  std::size_t listagg_max_output_bytes{0};
  std::string listagg_truncation_indicator{"..."};
  bool listagg_with_count{true};
  exec::CanonicalAggregateDescriptor aggregate_descriptor;
  exec::ExecutorColumnDescriptor result_column;
  std::vector<exec::CanonicalResultColumnBinding> result_bindings;
  std::string detail;
};

struct PreparedGroupedCountSumRoot {
  bool ok{false};
  std::vector<exec::CanonicalDescriptorOrderTerm> key_terms;
  std::vector<PreparedAggregateValueBindingReceipt> key_binding_receipts;
  std::vector<exec::ExecutorColumnDescriptor> key_result_columns;
  std::vector<exec::CanonicalAggregateGroupingSet> grouping_sets;
  std::vector<exec::ExecutorColumnDescriptor> grouping_projection_columns;
  std::size_t maximum_grouping_key_comparison_count{0};
  std::size_t maximum_combined_grouping_key_comparison_count{0};
  PreparedGlobalAggregateRoot count;
  PreparedGlobalAggregateRoot sum;
  std::vector<exec::CanonicalResultColumnBinding> result_bindings;
  std::string detail;
};

std::string ExactCanonicalCoreDatatypeUuidV1(std::string_view stable_name);
std::string ExactCanonicalCoreDatatypeTypeUuidV1(std::string_view stable_name);
std::string ExactCanonicalInt64TypeUuidV1();

bool RevalidatePreparedAggregateValueBindings(
    const std::vector<std::size_t>& value_columns,
    const std::vector<std::uint32_t>& value_descriptor_ids,
    const std::vector<PreparedAggregateValueBindingReceipt>& receipts,
    const exec::DescriptorBatch& input_batch,
    std::string* detail);

bool BindPreparedGroupedComparisonCeilings(
    PreparedGroupedCountSumRoot* prepared,
    std::uint64_t maximum_input_row_count);

bool RevalidatePreparedGroupedKeyBindings(
    const PreparedGroupedCountSumRoot& prepared,
    const exec::DescriptorBatch& input_batch,
    std::string* detail);

bool ValidateAggregateFilterTruthValues(
    const exec::DescriptorBatch& input,
    std::size_t filter_column,
    std::uint32_t filter_descriptor_id,
    std::string* detail) noexcept;

bool MaterializeAggregateFilterTruthValues(
    const exec::DescriptorBatch& input,
    std::size_t filter_column,
    std::uint32_t filter_descriptor_id,
    std::uint64_t maximum_retained_bytes,
    std::vector<scratchbird::engine::internal_api::EngineSqlTruthValue>*
        filter_truth_values,
    std::uint64_t* retained_bytes,
    std::string* detail) noexcept;

bool BindCanonicalDescriptorEqualityTerm(
    const scratchbird::engine::internal_api::EngineRequestContext& context,
    const exec::ExecutorColumnDescriptor& column,
    std::size_t column_ordinal,
    exec::CanonicalDescriptorOrderTerm* term,
    std::string* detail);

bool BindTimezoneOrderAuthority(
    const scratchbird::engine::internal_api::EngineRequestContext& context,
    exec::CanonicalDescriptorOrderTerm* term,
    std::string* detail);

bool BindCanonicalAggregateEqualityTerms(
    const scratchbird::engine::internal_api::EngineRequestContext& context,
    const exec::DescriptorBatch& input_batch,
    exec::CanonicalAggregateRuntimeRequest* request,
    std::string* detail);

exec::CanonicalPhysicalExecutorRegistration
MakeLiveAggregateRegistryRegistration(
    PreparedGlobalAggregateRoot prepared,
    std::string capability_uuid,
    std::size_t maximum_input_row_count,
    std::uint64_t maximum_filter_truth_memory_bytes,
    scratchbird::engine::internal_api::EngineRequestContext mga_context,
    bool strict_dispatcher_memory = false);

exec::CanonicalPhysicalExecutorRegistration
MakeLiveGroupedCountSumRegistration(
    PreparedGroupedCountSumRoot prepared,
    std::string capability_uuid,
    std::size_t maximum_input_row_count,
    std::size_t maximum_output_row_count,
    scratchbird::engine::internal_api::EngineRequestContext mga_context);

}  // namespace scratchbird::engine::sblr
