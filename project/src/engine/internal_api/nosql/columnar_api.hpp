// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "../../executor/descriptor_value_runtime.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace scratchbird::engine::internal_api::nosql {

inline constexpr std::string_view kColumnarLogicalReconstructionV1 =
    "COLUMNAR_LOGICAL_RECONSTRUCTION_V1";
inline constexpr std::string_view kColumnarLogicalReconstructionV2 =
    "COLUMNAR_LOGICAL_RECONSTRUCTION_V2";

enum class ColumnarTestRepresentationV1 : std::uint8_t {
  kPlain = 1,
  kDictionary,
  kRunLength,
};

struct ColumnarZoneProofV1 {
  bool present{false};
  bool profile_active{false};
  bool comparison_exact{false};
  bool source_generation_matches{false};
  bool catalog_generation_matches{false};
  bool summary_generation_matches{false};
  bool fresh{false};
  bool clean{false};
  bool snapshot_safe{false};
  bool safe_negative_prune{false};
  std::vector<std::size_t> candidate_row_ordinals;
};

struct ColumnarExecutionRequestV1 {
  std::uint16_t abi_version{1};
  std::string profile_id{std::string(kColumnarLogicalReconstructionV1)};
  std::vector<std::string> operation_ids;
  std::string operation_id;
  std::string relation_uuid;
  std::vector<std::string> row_uuids;
  executor::DescriptorBatch logical_rows;
  std::vector<std::size_t> projected_columns;
  std::vector<EngineSqlTruthValue> filter_truth_values;
  ColumnarTestRepresentationV1 representation{
      ColumnarTestRepresentationV1::kPlain};
  ColumnarZoneProofV1 zone_proof;
  executor::PhysicalMgaStatementContext statement_context;
  executor::PhysicalMgaStatementContext current_statement_context;
  std::uint64_t source_generation{0};
  std::uint64_t catalog_generation{0};
  std::uint64_t summary_generation{0};
  std::size_t maximum_rows{0};
  std::size_t maximum_cells{0};
  bool security_admitted{false};
  bool canonical_predicate_bound{true};
  bool lossy_coercion_requested{false};
  bool exact_reconstruction_fallback_available{false};
  bool parser_execution_authority_claimed{false};
  bool zone_visibility_authority_claimed{false};
  bool zone_finality_authority_claimed{false};
};

struct ColumnarExecutionResultV1 {
  bool accepted{false};
  bool root_publishable{false};
  bool exact_fallback_selected{false};
  bool exact_reconstruction_complete{false};
  bool predicate_recheck_complete{false};
  bool mga_recheck_complete{false};
  std::vector<std::string> row_uuids;
  executor::DescriptorBatch batch;
  std::string physical_operator_id;
  std::string fallback_reason_id;
  std::string diagnostic_id;
  std::string detail;
};

using ColumnarCancellationProbeV2 = bool (*)(const void* context);

struct ColumnarExecutionRequestV2 {
  std::uint16_t abi_version{2};
  std::string profile_id{std::string(kColumnarLogicalReconstructionV2)};
  std::vector<std::string> operation_ids;
  std::string operation_id;
  std::string relation_uuid;
  std::vector<std::string> row_uuids;
  executor::DescriptorBatch logical_rows;
  std::vector<std::size_t> projected_columns;
  std::vector<EngineSqlTruthValue> filter_truth_values;
  ColumnarTestRepresentationV1 representation{
      ColumnarTestRepresentationV1::kPlain};
  ColumnarZoneProofV1 zone_proof;
  executor::PhysicalMgaStatementContext statement_context;
  executor::PhysicalMgaStatementContext current_statement_context;
  std::uint64_t source_generation{0};
  std::uint64_t catalog_generation{0};
  std::uint64_t summary_generation{0};
  std::size_t maximum_rows{0};
  std::size_t maximum_input_cells{0};
  std::size_t maximum_cells{0};
  std::uint64_t maximum_memory_bytes{0};
  ColumnarCancellationProbeV2 cancellation_requested{nullptr};
  const void* cancellation_context{nullptr};
  bool security_admitted{false};
  bool canonical_predicate_bound{true};
  bool lossy_coercion_requested{false};
  bool exact_reconstruction_fallback_available{false};
  bool parser_execution_authority_claimed{false};
  bool zone_visibility_authority_claimed{false};
  bool zone_finality_authority_claimed{false};
};

struct ColumnarExecutionResultV2 {
  bool accepted{false};
  bool root_publishable{false};
  bool exact_fallback_selected{false};
  bool exact_reconstruction_complete{false};
  bool predicate_recheck_complete{false};
  bool mga_recheck_complete{false};
  bool cancellation_observed{false};
  bool cancellation_probe_failed{false};
  bool memory_receipt_complete{false};
  std::uint64_t current_live_memory_bytes{0};
  std::uint64_t peak_live_memory_bytes{0};
  std::uint64_t memory_grant_bytes{0};
  std::vector<std::string> row_uuids;
  executor::DescriptorBatch batch;
  std::string physical_operator_id;
  std::string fallback_reason_id;
  std::string diagnostic_id;
  std::string detail;
};

ColumnarExecutionResultV1 ExecuteColumnarLogicalV1(
    const ColumnarExecutionRequestV1& request);
ColumnarExecutionResultV2 ExecuteColumnarLogicalV2(
    const ColumnarExecutionRequestV2& request);
ColumnarExecutionResultV2 ExecuteColumnarLogicalV2(
    ColumnarExecutionRequestV2&& request);

}  // namespace scratchbird::engine::internal_api::nosql
