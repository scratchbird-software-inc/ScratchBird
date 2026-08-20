// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "executor_foundation.hpp"

#include "descriptor_value_runtime.hpp"
#include "aggregate_executor_internal.hpp"
#include "temp_spill_executor.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <limits>
#include <map>
#include <numeric>
#include <optional>
#include <set>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace scratchbird::engine::executor {
namespace {

std::string JoinDescriptor(std::string_view left, std::string_view right) {
  return std::string(left) + "+" + std::string(right);
}

std::vector<std::int64_t> ConcatValues(const Tuple& left, const Tuple& right) {
  std::vector<std::int64_t> out = left.values;
  out.insert(out.end(), right.values.begin(), right.values.end());
  return out;
}

bool HasColumn(const Tuple& tuple, std::size_t column) {
  return column < tuple.values.size();
}

bool IsCanonicalUuid(const std::string_view value) {
  if (value.size() != 36 || value[8] != '-' || value[13] != '-' ||
      value[18] != '-' || value[23] != '-') {
    return false;
  }
  for (std::size_t index = 0; index < value.size(); ++index) {
    if (index == 8 || index == 13 || index == 18 || index == 23) continue;
    const auto ch = static_cast<unsigned char>(value[index]);
    if (!std::isxdigit(ch) || std::isupper(ch)) return false;
  }
  return true;
}

std::string_view CanonicalBoundJoinImplementationId(
    const CanonicalAcceptedJoinKind kind) {
  switch (kind) {
    case CanonicalAcceptedJoinKind::kCross:
      return "join.cross.3vl.nested.v1";
    case CanonicalAcceptedJoinKind::kInner:
      return "join.inner.3vl.nested.v1";
    case CanonicalAcceptedJoinKind::kLeftOuter:
      return "join.left-outer.3vl.nested.v1";
    case CanonicalAcceptedJoinKind::kRightOuter:
      return "join.right-outer.3vl.nested.v1";
    case CanonicalAcceptedJoinKind::kFullOuter:
      return "join.full-outer.3vl.nested.v1";
    case CanonicalAcceptedJoinKind::kLeftSemi:
      return "join.left-semi.3vl.nested.v1";
    case CanonicalAcceptedJoinKind::kLeftAnti:
      return "join.left-anti.3vl.nested.v1";
  }
  return {};
}

std::optional<std::size_t> FoundationNodeMemoryGrant(
    const TypedPhysicalNodeDag& dag, const std::uint64_t node_id) {
  const auto node = std::ranges::find_if(
      dag.nodes, [&](const auto& candidate) {
        return candidate.physical_node_id == node_id;
      });
  if (node == dag.nodes.end() || dag.memory_budget_bytes == 0 ||
      node->memory_bytes_required == 0 ||
      node->memory_bytes_required > dag.memory_budget_bytes ||
      node->memory_bytes_required >
          static_cast<std::uint64_t>(
              std::numeric_limits<std::size_t>::max())) {
    return std::nullopt;
  }
  return static_cast<std::size_t>(node->memory_bytes_required);
}

bool FoundationBatchPayloadBytes(const DescriptorBatch& batch,
                                 std::size_t* bytes) {
  if (bytes == nullptr) return false;
  *bytes = 1;
  for (const auto& row : batch.rows) {
    for (const auto& value : row.values) {
      if (*bytes > std::numeric_limits<std::size_t>::max() -
                       value.encoded_value.size()) {
        return false;
      }
      *bytes += value.encoded_value.size();
      if (*bytes > std::numeric_limits<std::size_t>::max() -
                       value.binary_value.size()) {
        return false;
      }
      *bytes += value.binary_value.size();
    }
  }
  return true;
}

bool FoundationTuplePayloadBytes(const DescriptorTuple& tuple,
                                 std::size_t* bytes) {
  if (bytes == nullptr) return false;
  *bytes = 0;
  for (const auto& value : tuple.values) {
    if (*bytes > std::numeric_limits<std::size_t>::max() -
                     value.encoded_value.size()) {
      return false;
    }
    *bytes += value.encoded_value.size();
    if (*bytes > std::numeric_limits<std::size_t>::max() -
                     value.binary_value.size()) {
      return false;
    }
    *bytes += value.binary_value.size();
  }
  return true;
}

// SET memory receipts use the engine's deterministic logical-payload ABI:
// retained encoded/binary bytes plus explicitly charged operator work state.
// Materialized-batch carrier slots, descriptor/result-receipt metadata, and
// implementation-dependent container links or allocator capacity are outside
// this ABI, matching ordinary typed-operator payload observations. It does not
// claim allocator/RSS exactness.
struct FoundationSetMemoryLedger {
  bool enforced = false;
  std::size_t grant_bytes = 0;
  std::size_t live_payload_bytes = 0;
  std::size_t peak_payload_bytes = 0;
  std::size_t live_structural_bytes = 0;
  std::size_t peak_memory_bytes = 0;
  std::string refusal_detail;

  bool Observe(const std::size_t next_payload,
               const std::size_t next_structural,
               const std::string_view phase) {
    if (next_structural >
        std::numeric_limits<std::size_t>::max() - next_payload) {
      refusal_detail =
          "set-operation logical memory accounting overflowed " +
          std::string(phase);
      return false;
    }
    const auto next_memory = next_payload + next_structural;
    if (next_memory > grant_bytes) {
      refusal_detail =
          "set-operation logical memory exceeded its selected-node grant " +
          std::string(phase);
      return false;
    }
    live_payload_bytes = next_payload;
    live_structural_bytes = next_structural;
    peak_payload_bytes = std::max(peak_payload_bytes, next_payload);
    peak_memory_bytes = std::max(peak_memory_bytes, next_memory);
    return true;
  }

  bool RetainPayload(const std::size_t bytes,
                     const std::string_view phase) {
    if (!enforced) return true;
    if (bytes > std::numeric_limits<std::size_t>::max() -
                    live_payload_bytes) {
      refusal_detail =
          "set-operation logical payload accounting overflowed " +
          std::string(phase);
      return false;
    }
    return Observe(live_payload_bytes + bytes, live_structural_bytes, phase);
  }

  bool ReplacePayload(const std::size_t old_bytes,
                      const std::size_t new_bytes,
                      const std::string_view phase) {
    if (!enforced) return true;
    if (old_bytes > live_payload_bytes ||
        new_bytes > std::numeric_limits<std::size_t>::max() -
                        live_payload_bytes) {
      refusal_detail =
          "set-operation logical payload replacement is invalid " +
          std::string(phase);
      return false;
    }
    const auto previous_payload_bytes = live_payload_bytes;
    // The replacement value is materialized while the retained old value is
    // still live. Observe that overlap before publishing the post-move state.
    if (!Observe(previous_payload_bytes + new_bytes,
                 live_structural_bytes, phase)) {
      return false;
    }
    live_payload_bytes =
        previous_payload_bytes - old_bytes + new_bytes;
    return true;
  }

  bool ReleasePayload(const std::size_t bytes,
                      const std::string_view phase) {
    if (!enforced) return true;
    if (bytes > live_payload_bytes) {
      refusal_detail =
          "set-operation logical payload accounting underflowed " +
          std::string(phase);
      return false;
    }
    live_payload_bytes -= bytes;
    return true;
  }

  bool RetainStructural(const std::size_t bytes,
                        const std::string_view phase) {
    if (!enforced) return true;
    if (bytes > std::numeric_limits<std::size_t>::max() -
                    live_structural_bytes) {
      refusal_detail =
          "set-operation logical structural accounting overflowed " +
          std::string(phase);
      return false;
    }
    return Observe(live_payload_bytes, live_structural_bytes + bytes, phase);
  }

  bool ReleaseStructural(const std::size_t bytes,
                         const std::string_view phase) {
    if (!enforced) return true;
    if (bytes > live_structural_bytes) {
      refusal_detail =
          "set-operation logical structural accounting underflowed " +
          std::string(phase);
      return false;
    }
    live_structural_bytes -= bytes;
    return true;
  }
};

CanonicalAggregateRuntimeRequest
CloneFoundationAggregateRequestWithoutRowsOrFilter(
    const CanonicalAggregateRuntimeRequest& source,
    const bool copy_execution_carriers = true) {
  CanonicalAggregateRuntimeRequest clone;
  if (copy_execution_carriers) {
    clone.physical_dag = source.physical_dag;
    clone.input_batch.columns = source.input_batch.columns;
  }
  clone.selected_physical_node_id = source.selected_physical_node_id;
  clone.descriptor = source.descriptor;
  clone.value_columns = source.value_columns;
  clone.value_expression_descriptor_ids =
      source.value_expression_descriptor_ids;
  clone.direct_arguments = source.direct_arguments;
  clone.result_column = source.result_column;
  if (source.filter_truth_values.has_value()) {
    clone.filter_truth_values =
        std::vector<scratchbird::engine::internal_api::EngineSqlTruthValue>{};
  }
  clone.distinct = source.distinct;
  clone.aggregate_equality_terms = source.aggregate_equality_terms;
  clone.aggregate_equality_authority_profile =
      source.aggregate_equality_authority_profile;
  clone.aggregate_order_terms = source.aggregate_order_terms;
  clone.aggregate_separator = source.aggregate_separator;
  clone.listagg_overflow_mode = source.listagg_overflow_mode;
  clone.listagg_max_output_bytes = source.listagg_max_output_bytes;
  clone.listagg_truncation_indicator = source.listagg_truncation_indicator;
  clone.listagg_with_count = source.listagg_with_count;
  clone.forced_strategy = source.forced_strategy;
  clone.maximum_transition_count = source.maximum_transition_count;
  clone.maximum_distinct_value_count = source.maximum_distinct_value_count;
  clone.maximum_equality_key_generation_count =
      source.maximum_equality_key_generation_count;
  clone.maximum_equality_comparison_count =
      source.maximum_equality_comparison_count;
  clone.maximum_aggregate_order_term_count =
      source.maximum_aggregate_order_term_count;
  clone.maximum_order_comparison_count =
      source.maximum_order_comparison_count;
  clone.maximum_state_bytes = source.maximum_state_bytes;
  clone.maximum_final_output_bytes = source.maximum_final_output_bytes;
  clone.maximum_finalization_workspace_bytes =
      source.maximum_finalization_workspace_bytes;
  clone.retained_memory_bytes = source.retained_memory_bytes;
  clone.parser_execution_authority_claimed =
      source.parser_execution_authority_claimed;
  clone.transaction_finality_claimed = source.transaction_finality_claimed;
  clone.recovery_authority_claimed = source.recovery_authority_claimed;
  clone.mga_authority = source.mga_authority;
  return clone;
}

bool FoundationFilterBytes(
    const std::size_t count,
    std::size_t* bytes) {
  if (bytes == nullptr ||
      count > std::numeric_limits<std::size_t>::max() /
                  sizeof(scratchbird::engine::internal_api::EngineSqlTruthValue)) {
    return false;
  }
  *bytes = count *
           sizeof(scratchbird::engine::internal_api::EngineSqlTruthValue);
  return true;
}

bool FoundationCheckedMultiply(const std::size_t left,
                               const std::size_t right,
                               std::size_t* product) {
  if (product == nullptr ||
      (left != 0 &&
       right > std::numeric_limits<std::size_t>::max() / left)) {
    return false;
  }
  *product = left * right;
  return true;
}

bool FoundationCheckedAdd(std::size_t* total, const std::size_t amount) {
  if (total == nullptr ||
      amount > std::numeric_limits<std::size_t>::max() - *total) {
    return false;
  }
  *total += amount;
  return true;
}

std::optional<DescriptorRuntimeDiagnostic> PollFoundationJoinCancellation(
    const std::function<bool()>& probe,
    const char* phase,
    const std::size_t row,
    bool* cancellation_observed) {
  if (!probe) return std::nullopt;
  try {
    if (!probe()) return std::nullopt;
    if (cancellation_observed != nullptr) *cancellation_observed = true;
    DescriptorRuntimeDiagnostic diagnostic;
    diagnostic.ok = false;
    diagnostic.diagnostic_code = "QOW-DIAG-QRY-012-JOIN-CANCELLED-V1";
    diagnostic.detail = std::string("join cancellation observed ") + phase;
    diagnostic.row_index = row;
    return diagnostic;
  } catch (const std::exception& exception) {
    DescriptorRuntimeDiagnostic diagnostic;
    diagnostic.ok = false;
    diagnostic.diagnostic_code =
        "QOW-DIAG-QRY-012-CANCELLATION-PROBE-V1";
    diagnostic.detail = std::string("join cancellation probe threw: ") +
                        exception.what();
    diagnostic.row_index = row;
    return diagnostic;
  } catch (...) {
    DescriptorRuntimeDiagnostic diagnostic;
    diagnostic.ok = false;
    diagnostic.diagnostic_code =
        "QOW-DIAG-QRY-012-CANCELLATION-PROBE-V1";
    diagnostic.detail =
        "join cancellation probe threw a non-standard exception";
    diagnostic.row_index = row;
    return diagnostic;
  }
}

bool CanonicalWindowAuthorityAbsent(
    const CanonicalExecutionMgaAuthority& authority) {
  return authority.origin == CanonicalMgaAuthorityOrigin::kMissing &&
         !authority.resolve_current &&
         !PhysicalMgaStatementContextValid(authority.statement_context);
}

const CanonicalExecutionMgaAuthority& CanonicalWindowFrameExecutionAuthority(
    const CanonicalExecutionMgaAuthority& requested,
    const CanonicalWindowFrameResult& frames) {
  return CanonicalWindowAuthorityAbsent(requested) ? frames.mga_authority
                                                   : requested;
}

DescriptorRuntimeDiagnostic RevalidateCanonicalWindowFrameAuthority(
    const CanonicalExecutionMgaAuthority& requested,
    const CanonicalWindowFrameResult& frames) {
  auto diagnostic = RevalidateCanonicalExecutionMgaAuthority(
      frames.mga_authority, frames.physical_dag);
  if (!diagnostic.ok) return diagnostic;
  if (!PhysicalMgaStatementContextEqual(frames.mga_authority.statement_context,
                                        frames.mga_statement_context)) {
    diagnostic.ok = false;
    diagnostic.diagnostic_code = "QOW-DIAG-WINDOW-MGA-CONTEXT-V1";
    diagnostic.detail =
        "window frame carrier and frame result MGA contexts differ";
    return diagnostic;
  }
  if (!CanonicalWindowAuthorityAbsent(requested)) {
    diagnostic = RevalidateCanonicalExecutionMgaAuthority(
        requested, frames.physical_dag);
    if (!diagnostic.ok) return diagnostic;
    if (!PhysicalMgaStatementContextEqual(requested.statement_context,
                                          frames.mga_statement_context)) {
      diagnostic.ok = false;
      diagnostic.diagnostic_code = "QOW-DIAG-WINDOW-MGA-CONTEXT-V1";
      diagnostic.detail =
          "window request and frame result MGA contexts differ";
    }
  }
  return diagnostic;
}

bool ParseInt64Text(const std::string_view text, std::int64_t* value) {
  if (value == nullptr || text.empty()) return false;
  const auto* begin = text.data();
  const auto* end = begin + text.size();
  const auto parsed = std::from_chars(begin, end, *value);
  return parsed.ec == std::errc{} && parsed.ptr == end;
}

bool HasOwnedAggregateSpillArtifact(const std::filesystem::path& directory,
                                    bool* inspection_ok) {
  if (inspection_ok == nullptr) return false;
  *inspection_ok = false;
  std::error_code error;
  const bool exists = std::filesystem::exists(directory, error);
  if (error) return false;
  if (!exists) {
    *inspection_ok = true;
    return false;
  }
  if (!std::filesystem::is_directory(directory, error) || error) return false;
  for (std::filesystem::directory_iterator iterator(directory, error), end;
       !error && iterator != end; iterator.increment(error)) {
    const auto filename = iterator->path().filename().string();
    if (filename.rfind("orh283_temp_spill-", 0) == 0 &&
        iterator->path().extension() == ".sbtmpidx") {
      *inspection_ok = true;
      return true;
    }
  }
  if (error) return false;
  *inspection_ok = true;
  return false;
}

bool RemoveOwnedAggregateSpillArtifacts(
    const std::filesystem::path& directory) {
  bool inspection_ok = false;
  if (!HasOwnedAggregateSpillArtifact(directory, &inspection_ok)) {
    return inspection_ok;
  }
  std::error_code error;
  for (std::filesystem::directory_iterator iterator(directory, error), end;
       !error && iterator != end; iterator.increment(error)) {
    const auto filename = iterator->path().filename().string();
    if (filename.rfind("orh283_temp_spill-", 0) == 0 &&
        iterator->path().extension() == ".sbtmpidx") {
      std::filesystem::remove(iterator->path(), error);
    }
  }
  if (error) return false;
  return !HasOwnedAggregateSpillArtifact(directory, &inspection_ok) &&
         inspection_ok;
}

bool TransitionCanonicalInt64SumValue(
    CanonicalInt64SumAggregateState* state,
    const scratchbird::engine::internal_api::EngineTypedValue& value,
    std::string* detail) {
  using scratchbird::engine::internal_api::EngineValueState;
  if (state == nullptr || detail == nullptr) return false;
  ++state->transition_count;
  if (value.state == EngineValueState::sql_null) return true;
  const auto decoded = DecodeInt64Value(value);
  if (!decoded.ok()) {
    *detail = decoded.diagnostic.diagnostic_code + ":" +
              decoded.diagnostic.detail;
    return false;
  }
  if ((decoded.value > 0 &&
       state->accumulated_value >
           std::numeric_limits<std::int64_t>::max() - decoded.value) ||
      (decoded.value < 0 &&
       state->accumulated_value <
           std::numeric_limits<std::int64_t>::min() - decoded.value)) {
    *detail = "SUM int64 transition overflowed";
    return false;
  }
  state->accumulated_value += decoded.value;
  ++state->non_null_count;
  state->has_value = true;
  return true;
}

scratchbird::engine::internal_api::EngineTypedValue
FinalizeCanonicalInt64SumValue(
    const CanonicalInt64SumAggregateState& state) {
  using scratchbird::engine::internal_api::EngineTypedValue;
  using scratchbird::engine::internal_api::EngineValueState;
  EngineTypedValue value;
  value.descriptor = state.result_column.descriptor;
  if (state.has_value) {
    value.encoded_value = std::to_string(state.accumulated_value);
    value.state = EngineValueState::value;
  } else {
    value.is_null = true;
    value.state = EngineValueState::sql_null;
  }
  return value;
}

CanonicalDescriptorOrderTerm JoinKeyOrderTerm(
    const CanonicalCompositeJoinKeyTerm& term,
    const bool left) {
  CanonicalDescriptorOrderTerm order_term;
  order_term.column = left ? term.left_column : term.right_column;
  order_term.expression_descriptor_id =
      left ? term.left_expression_descriptor_id
           : term.right_expression_descriptor_id;
  order_term.direction = CanonicalDescriptorOrderDirection::ascending;
  order_term.null_placement = CanonicalDescriptorNullPlacement::last;
  order_term.collation_uuid = term.collation_uuid;
  order_term.resource_epoch = term.resource_epoch;
  order_term.collation_epoch = term.collation_epoch;
  order_term.text_seed = term.text_seed;
  order_term.timezone_epoch = term.timezone_epoch;
  order_term.timezone_seed = term.timezone_seed;
  return order_term;
}

bool CompareCanonicalJoinKeyValues(
    const scratchbird::engine::internal_api::EngineTypedValue& left,
    const scratchbird::engine::internal_api::EngineTypedValue& right,
    const CanonicalCompositeJoinKeyTerm& term,
    bool* equal,
    std::string* detail) {
  if (equal == nullptr || detail == nullptr) return false;
  *equal = false;
  detail->clear();
  const auto compared = CompareCanonicalDescriptorOrderValues(
      left, right, JoinKeyOrderTerm(term, true));
  if (!compared.diagnostic.ok) {
    *detail = compared.diagnostic.diagnostic_code + ":" +
              compared.diagnostic.detail;
    return false;
  }
  *equal = compared.comparison == 0;
  return true;
}

// QOW-SOURCE-QRY-009-V1
// Column positions reaching this legacy executor surface are already-bound
// projection, sort, or expression handles.  Resolve the complete handle set
// before an operator can consume any row so a ragged or out-of-range handle
// cannot be translated into successful zero data.
void RequireResolvedColumnHandles(
    const Batch& input,
    const std::vector<std::size_t>& columns) {
  for (const auto column : columns) {
    if (column >= input.column_count) {
      throw std::out_of_range("SBLR.PLAN_TREE.INVALID_HANDLE");
    }
  }
  for (const auto& row : input.rows) {
    for (const auto column : columns) {
      if (!HasColumn(row, column)) {
        throw std::out_of_range("SBLR.PLAN_TREE.INVALID_HANDLE");
      }
    }
  }
}

}  // namespace

// QOW-SOURCE-QRY-011-STATE-V1
// Build one descriptor-bound SUM(int64) transition state.  SQL NULL inputs do
// not transition the numeric value, while every physical row is retained in
// the transition count.  Validation and overflow complete before state is
// published to the caller.
CanonicalInt64SumStateResult ExecuteCanonicalInt64SumState(
    const CanonicalInt64SumStateRequest& request) {
  using scratchbird::engine::internal_api::EngineValueState;

  CanonicalInt64SumStateResult result;
  const auto refuse = [&](std::string detail) {
    result.diagnostic.ok = false;
    result.diagnostic.diagnostic_code =
        "QOW-DIAG-QRY-011-STATE-REFUSAL-V1";
    result.diagnostic.detail = std::move(detail);
    result.state = {};
    return result;
  };
  const auto authority_validation = RevalidateCanonicalExecutionMgaAuthority(
      request.mga_authority, request.physical_dag);
  if (!authority_validation.ok)
    return refuse(authority_validation.diagnostic_code + ":" +
                  authority_validation.detail);
  if (request.selected_physical_node_id == 0 ||
      request.selected_physical_node_id !=
          request.physical_dag.root_physical_node_id) {
    return refuse("selected aggregate node is not the physical root");
  }

  const PhysicalNodeRecord* selected_node = nullptr;
  const PhysicalNodeRecord* input_node = nullptr;
  for (const auto& node : request.physical_dag.nodes) {
    if (node.physical_node_id == request.selected_physical_node_id) {
      selected_node = &node;
    }
  }
  if (selected_node == nullptr ||
      selected_node->node_kind != PhysicalNodeKind::kAggregate ||
      selected_node->input_physical_node_ids.size() != 1) {
    return refuse("SUM state requires one selected aggregate node");
  }
  for (const auto& node : request.physical_dag.nodes) {
    if (node.physical_node_id ==
        selected_node->input_physical_node_ids.front()) {
      input_node = &node;
      break;
    }
  }
  if (input_node == nullptr) {
    return refuse("aggregate input node is unresolved");
  }

  auto input_validation = ValidateCanonicalDescriptorBatch(
      request.input_batch, input_node->output_descriptor_ids);
  if (!input_validation.ok) {
    return refuse(input_validation.diagnostic_code + ":" +
                  input_validation.detail);
  }
  if (request.value_column >= request.input_batch.columns.size()) {
    return refuse("aggregate value column is outside the input schema");
  }
  const auto& value_column = request.input_batch.columns[request.value_column];
  if (request.value_expression_descriptor_id == 0 ||
      request.value_expression_descriptor_id != value_column.descriptor_id ||
      value_column.descriptor.canonical_type_name != "int64") {
    return refuse("SUM value expression is not bound int64");
  }
  if (selected_node->output_descriptor_ids.size() != 1 ||
      request.result_column.descriptor_id !=
          selected_node->output_descriptor_ids.front() ||
      !request.result_column.nullable ||
      request.result_column.descriptor.canonical_type_name != "int64") {
    return refuse("SUM result is not a bound nullable int64 descriptor");
  }
  DescriptorBatch result_schema;
  result_schema.columns = {request.result_column};
  auto result_validation = ValidateCanonicalDescriptorBatch(
      result_schema, selected_node->output_descriptor_ids);
  if (!result_validation.ok) {
    return refuse(result_validation.diagnostic_code + ":" +
                  result_validation.detail);
  }
  if (request.maximum_transition_count == 0 ||
      request.input_batch.rows.size() > request.maximum_transition_count) {
    return refuse("aggregate transition resource bound was exceeded");
  }

  CanonicalInt64SumAggregateState state;
  state.value_expression_descriptor_id =
      request.value_expression_descriptor_id;
  state.result_column = request.result_column;
  for (const auto& row : request.input_batch.rows) {
    const auto& value = row.values[request.value_column];
    std::string detail;
    if (!TransitionCanonicalInt64SumValue(&state, value, &detail)) {
      return refuse(std::move(detail));
    }
  }

  const auto result_authority = RevalidateCanonicalExecutionMgaAuthority(
      request.mga_authority, request.physical_dag);
  if (!result_authority.ok)
    return refuse(result_authority.diagnostic_code + ":" +
                  result_authority.detail);

  result.diagnostic = {};
  result.state = std::move(state);
  result.selected_plan_uuid = request.physical_dag.selected_plan_uuid;
  result.executed_physical_node_id = selected_node->physical_node_id;
  result.causal_counter_id = selected_node->causal_counter_id;
  result.mga_statement_context = request.mga_authority.statement_context;
  return result;
}

// QOW-SOURCE-QRY-011-EMPTY-V1
// Finalize one validated SUM state.  Empty and all-NULL inputs return one
// canonical SQL NULL row under the bound nullable descriptor; zero is data
// only when a real non-NULL transition produced it.
CanonicalInt64SumFinalizeResult ExecuteCanonicalInt64SumFinalize(
    const CanonicalInt64SumFinalizeRequest& request) {
  using scratchbird::engine::internal_api::EngineTypedValue;
  using scratchbird::engine::internal_api::EngineValueState;

  CanonicalInt64SumFinalizeResult result;
  const auto refuse = [&](std::string detail) {
    result.diagnostic.ok = false;
    result.diagnostic.diagnostic_code =
        "QOW-DIAG-QRY-011-EMPTY-REFUSAL-V1";
    result.diagnostic.detail = std::move(detail);
    result.output_batch = {};
    return result;
  };

  const auto authority_validation = RevalidateCanonicalExecutionMgaAuthority(
      request.mga_authority, request.physical_dag);
  if (!authority_validation.ok)
    return refuse(authority_validation.diagnostic_code + ":" +
                  authority_validation.detail);
  if (request.selected_physical_node_id == 0 ||
      request.selected_physical_node_id !=
          request.physical_dag.root_physical_node_id) {
    return refuse("selected aggregate node is not the physical root");
  }
  const PhysicalNodeRecord* selected_node = nullptr;
  for (const auto& node : request.physical_dag.nodes) {
    if (node.physical_node_id == request.selected_physical_node_id) {
      selected_node = &node;
      break;
    }
  }
  const auto& state = request.state;
  if (selected_node == nullptr ||
      selected_node->node_kind != PhysicalNodeKind::kAggregate ||
      selected_node->input_physical_node_ids.size() != 1 ||
      selected_node->output_descriptor_ids.size() != 1 ||
      state.result_column.descriptor_id !=
          selected_node->output_descriptor_ids.front()) {
    return refuse("SUM finalization physical result handle is unresolved");
  }
  if (state.value_expression_descriptor_id == 0 ||
      !state.result_column.nullable ||
      state.result_column.descriptor.canonical_type_name != "int64" ||
      state.non_null_count > state.transition_count ||
      state.has_value != (state.non_null_count != 0)) {
    return refuse("SUM transition state invariants are invalid");
  }

  DescriptorBatch result_schema;
  result_schema.columns = {state.result_column};
  auto schema_validation = ValidateCanonicalDescriptorBatch(
      result_schema, selected_node->output_descriptor_ids);
  if (!schema_validation.ok) {
    return refuse(schema_validation.diagnostic_code + ":" +
                  schema_validation.detail);
  }

  EngineTypedValue value = FinalizeCanonicalInt64SumValue(state);
  result.output_batch.columns = {state.result_column};
  result.output_batch.rows = {{{std::move(value)}}};
  auto output_validation = ValidateCanonicalDescriptorBatch(
      result.output_batch, selected_node->output_descriptor_ids);
  if (!output_validation.ok) {
    return refuse(output_validation.diagnostic_code + ":" +
                  output_validation.detail);
  }
  const auto result_authority = RevalidateCanonicalExecutionMgaAuthority(
      request.mga_authority, request.physical_dag);
  if (!result_authority.ok)
    return refuse(result_authority.diagnostic_code + ":" +
                  result_authority.detail);
  result.diagnostic = {};
  result.selected_plan_uuid = request.physical_dag.selected_plan_uuid;
  result.executed_physical_node_id = selected_node->physical_node_id;
  result.causal_counter_id = selected_node->causal_counter_id;
  result.mga_statement_context = request.mga_authority.statement_context;
  return result;
}

// QOW-SOURCE-QRY-011-GROUP-V1
// RCP-026-SOURCE-SPECIALIZED-GROUP-DERIVED-DESCRIPTOR-IDENTITY-V1
// Construct deterministic typed SUM states for one int64 grouping key.  The
// admitted grouping-set rule is either (key) or the exact {(key), ()} set;
// an actual SQL NULL key remains distinct from the synthetic grand total.
CanonicalInt64SumGroupResult ExecuteCanonicalInt64SumGroups(
    const CanonicalInt64SumGroupRequest& request) {
  using scratchbird::engine::internal_api::EngineTypedValue;
  using scratchbird::engine::internal_api::EngineValueState;

  CanonicalInt64SumGroupResult result;
  const auto refuse = [&](std::string detail) {
    result.diagnostic.ok = false;
    result.diagnostic.diagnostic_code =
        "QOW-DIAG-QRY-011-GROUP-REFUSAL-V1";
    result.diagnostic.detail = std::move(detail);
    result.groups.clear();
    return result;
  };

  const auto authority_validation = RevalidateCanonicalExecutionMgaAuthority(
      request.mga_authority, request.physical_dag);
  if (!authority_validation.ok)
    return refuse(authority_validation.diagnostic_code + ":" +
                  authority_validation.detail);
  if (request.selected_physical_node_id == 0 ||
      request.selected_physical_node_id !=
          request.physical_dag.root_physical_node_id) {
    return refuse("selected grouped aggregate is not the physical root");
  }
  const PhysicalNodeRecord* selected_node = nullptr;
  const PhysicalNodeRecord* input_node = nullptr;
  for (const auto& node : request.physical_dag.nodes) {
    if (node.physical_node_id == request.selected_physical_node_id) {
      selected_node = &node;
    }
  }
  if (selected_node == nullptr ||
      selected_node->node_kind != PhysicalNodeKind::kAggregate ||
      selected_node->input_physical_node_ids.size() != 1 ||
      selected_node->output_descriptor_ids.size() != 2) {
    return refuse("grouped SUM requires one selected aggregate node");
  }
  for (const auto& node : request.physical_dag.nodes) {
    if (node.physical_node_id ==
        selected_node->input_physical_node_ids.front()) {
      input_node = &node;
      break;
    }
  }
  if (input_node == nullptr) return refuse("grouped aggregate input is unresolved");

  auto input_validation = ValidateCanonicalDescriptorBatch(
      request.input_batch, input_node->output_descriptor_ids);
  if (!input_validation.ok) {
    return refuse(input_validation.diagnostic_code + ":" +
                  input_validation.detail);
  }
  if (request.key_column >= request.input_batch.columns.size() ||
      request.value_column >= request.input_batch.columns.size()) {
    return refuse("group or aggregate value column is outside the schema");
  }
  const auto& key_column = request.input_batch.columns[request.key_column];
  const auto& value_column = request.input_batch.columns[request.value_column];
  if (request.key_expression_descriptor_id == 0 ||
      request.key_expression_descriptor_id != key_column.descriptor_id ||
      key_column.descriptor.canonical_type_name != "int64" ||
      request.value_expression_descriptor_id == 0 ||
      request.value_expression_descriptor_id != value_column.descriptor_id ||
      value_column.descriptor.canonical_type_name != "int64") {
    return refuse("group key or SUM value expression is not bound int64");
  }
  const bool valid_grouping_set_rule =
      request.grouping_set_rule == CanonicalInt64GroupingSetRule::key_only ||
      request.grouping_set_rule ==
          CanonicalInt64GroupingSetRule::key_and_grand_total;
  if (!valid_grouping_set_rule) {
    return refuse("grouping-set or aggregate resource contract is invalid");
  }
  const bool expected_key_nullable =
      key_column.nullable ||
      request.grouping_set_rule ==
          CanonicalInt64GroupingSetRule::key_and_grand_total;
  if (request.key_result_column.descriptor_id !=
          selected_node->output_descriptor_ids[0] ||
      request.sum_result_column.descriptor_id !=
          selected_node->output_descriptor_ids[1] ||
      request.key_result_column.nullable != expected_key_nullable ||
      !request.sum_result_column.nullable ||
      !CanonicalDerivedDescriptorTypeMatches(
          key_column.descriptor, key_column.nullable,
          request.key_result_column.descriptor, expected_key_nullable) ||
      request.sum_result_column.descriptor.canonical_type_name != "int64") {
    return refuse(
        "grouped SUM result descriptors do not preserve exact key type and required nullability");
  }
  DescriptorBatch result_schema;
  result_schema.columns = {request.key_result_column,
                           request.sum_result_column};
  auto schema_validation = ValidateCanonicalDescriptorBatch(
      result_schema, selected_node->output_descriptor_ids);
  if (!schema_validation.ok) {
    return refuse(schema_validation.diagnostic_code + ":" +
                  schema_validation.detail);
  }
  if (request.maximum_group_count == 0 ||
      request.maximum_transition_count == 0) {
    return refuse("grouping-set or aggregate resource contract is invalid");
  }
  const std::size_t transition_multiplier =
      request.grouping_set_rule ==
              CanonicalInt64GroupingSetRule::key_and_grand_total
          ? 2
          : 1;
  if (request.input_batch.rows.size() >
      request.maximum_transition_count / transition_multiplier) {
    return refuse("aggregate transition resource bound was exceeded");
  }

  const auto make_sum_state = [&] {
    CanonicalInt64SumAggregateState state;
    state.value_expression_descriptor_id =
        request.value_expression_descriptor_id;
    state.result_column = request.sum_result_column;
    return state;
  };
  const auto transition = [&](CanonicalInt64SumAggregateState* state,
                              const EngineTypedValue& value,
                              std::string* detail) {
    if (state == nullptr || detail == nullptr) return false;
    ++state->transition_count;
    if (value.state == EngineValueState::sql_null) return true;
    const auto decoded = DecodeInt64Value(value);
    if (!decoded.ok()) {
      *detail = decoded.diagnostic.diagnostic_code + ":" +
                decoded.diagnostic.detail;
      return false;
    }
    if ((decoded.value > 0 &&
         state->accumulated_value >
             std::numeric_limits<std::int64_t>::max() - decoded.value) ||
        (decoded.value < 0 &&
         state->accumulated_value <
             std::numeric_limits<std::int64_t>::min() - decoded.value)) {
      *detail = "grouped SUM int64 transition overflowed";
      return false;
    }
    state->accumulated_value += decoded.value;
    ++state->non_null_count;
    state->has_value = true;
    return true;
  };

  std::vector<CanonicalInt64SumGroupState> groups;
  std::map<std::optional<std::int64_t>, std::size_t> group_by_key;
  for (const auto& row : request.input_batch.rows) {
    const auto& input_key = row.values[request.key_column];
    std::optional<std::int64_t> key;
    if (input_key.state != EngineValueState::sql_null) {
      const auto decoded = DecodeInt64Value(input_key);
      if (!decoded.ok()) {
        return refuse(decoded.diagnostic.diagnostic_code + ":" +
                      decoded.diagnostic.detail);
      }
      key = decoded.value;
    }
    auto found = group_by_key.find(key);
    if (found == group_by_key.end()) {
      if (groups.size() >= request.maximum_group_count) {
        return refuse("group count resource bound was exceeded");
      }
      CanonicalInt64SumGroupState group;
      group.group_key.descriptor = request.key_result_column.descriptor;
      if (key.has_value()) {
        group.group_key.encoded_value = std::to_string(*key);
        group.group_key.state = EngineValueState::value;
      } else {
        group.group_key.is_null = true;
        group.group_key.state = EngineValueState::sql_null;
      }
      group.sum_state = make_sum_state();
      groups.push_back(std::move(group));
      found = group_by_key.emplace(key, groups.size() - 1).first;
    }
    std::string detail;
    if (!transition(&groups[found->second].sum_state,
                    row.values[request.value_column], &detail)) {
      return refuse(std::move(detail));
    }
  }

  if (request.grouping_set_rule ==
      CanonicalInt64GroupingSetRule::key_and_grand_total) {
    if (groups.size() >= request.maximum_group_count) {
      return refuse("grand-total grouping set exceeds group resource bound");
    }
    CanonicalInt64SumGroupState grand_total;
    grand_total.grouping_set_ordinal = 1;
    grand_total.is_grand_total = true;
    grand_total.group_key.descriptor = request.key_result_column.descriptor;
    grand_total.group_key.is_null = true;
    grand_total.group_key.state = EngineValueState::sql_null;
    grand_total.sum_state = make_sum_state();
    for (const auto& row : request.input_batch.rows) {
      std::string detail;
      if (!transition(&grand_total.sum_state,
                      row.values[request.value_column], &detail)) {
        return refuse(std::move(detail));
      }
    }
    groups.push_back(std::move(grand_total));
  }

  const auto result_authority = RevalidateCanonicalExecutionMgaAuthority(
      request.mga_authority, request.physical_dag);
  if (!result_authority.ok)
    return refuse(result_authority.diagnostic_code + ":" +
                  result_authority.detail);

  result.diagnostic = {};
  result.groups = std::move(groups);
  result.selected_plan_uuid = request.physical_dag.selected_plan_uuid;
  result.executed_physical_node_id = selected_node->physical_node_id;
  result.causal_counter_id = selected_node->causal_counter_id;
  result.mga_statement_context = request.mga_authority.statement_context;
  return result;
}

// QOW-SOURCE-QRY-011-FILTER-V1
// Apply aggregate FILTER through the shared SQL 3VL consumer.  The complete
// typed input is validated before TRUE rows are selected, and FALSE/UNKNOWN
// rows never transition the aggregate state.
CanonicalInt64SumFilterResult ExecuteCanonicalInt64SumFilter(
    const CanonicalInt64SumFilterRequest& request) {
  namespace api = scratchbird::engine::internal_api;

  CanonicalInt64SumFilterResult result;
  const auto refuse = [&](std::string detail) {
    result.diagnostic.ok = false;
    result.diagnostic.diagnostic_code =
        "QOW-DIAG-QRY-011-FILTER-REFUSAL-V1";
    result.diagnostic.detail = std::move(detail);
    result.state = {};
    return result;
  };
  const auto& aggregate = request.aggregate_request;
  const auto authority_validation = RevalidateCanonicalExecutionMgaAuthority(
      aggregate.mga_authority, aggregate.physical_dag);
  if (!authority_validation.ok)
    return refuse(authority_validation.diagnostic_code + ":" +
                  authority_validation.detail);
  if (aggregate.selected_physical_node_id == 0 ||
      aggregate.selected_physical_node_id !=
          aggregate.physical_dag.root_physical_node_id) {
    return refuse("selected filtered aggregate is not the physical root");
  }
  const PhysicalNodeRecord* selected_node = nullptr;
  const PhysicalNodeRecord* input_node = nullptr;
  for (const auto& node : aggregate.physical_dag.nodes) {
    if (node.physical_node_id == aggregate.selected_physical_node_id) {
      selected_node = &node;
    }
  }
  if (selected_node == nullptr ||
      selected_node->node_kind != PhysicalNodeKind::kAggregate ||
      selected_node->input_physical_node_ids.size() != 1) {
    return refuse("filtered SUM requires one selected aggregate node");
  }
  for (const auto& node : aggregate.physical_dag.nodes) {
    if (node.physical_node_id ==
        selected_node->input_physical_node_ids.front()) {
      input_node = &node;
      break;
    }
  }
  if (input_node == nullptr) return refuse("filtered aggregate input is unresolved");

  auto input_validation = ValidateCanonicalDescriptorBatch(
      aggregate.input_batch, input_node->output_descriptor_ids);
  if (!input_validation.ok) {
    return refuse(input_validation.diagnostic_code + ":" +
                  input_validation.detail);
  }
  if (aggregate.value_column >= aggregate.input_batch.columns.size() ||
      aggregate.maximum_transition_count == 0 ||
      aggregate.input_batch.rows.size() >
          aggregate.maximum_transition_count ||
      request.row_truth_values.size() != aggregate.input_batch.rows.size()) {
    return refuse("FILTER row, value, or resource cardinality is invalid");
  }
  for (const auto& row : aggregate.input_batch.rows) {
    const auto& value = row.values[aggregate.value_column];
    if (value.state == api::EngineValueState::sql_null) continue;
    const auto decoded = DecodeInt64Value(value);
    if (!decoded.ok()) {
      return refuse(decoded.diagnostic.diagnostic_code + ":" +
                    decoded.diagnostic.detail);
    }
  }

  DescriptorBatch filtered;
  filtered.columns = aggregate.input_batch.columns;
  for (std::size_t row = 0; row < aggregate.input_batch.rows.size(); ++row) {
    bool passes = false;
    std::string detail;
    if (!api::QowPredicateConsumerPassesV1(
            request.row_truth_values[row], api::EnginePredicateConsumer::filter,
            &passes, &detail)) {
      return refuse("aggregate FILTER 3VL refusal: " + detail);
    }
    if (passes) filtered.rows.push_back(aggregate.input_batch.rows[row]);
  }

  auto filtered_request = aggregate;
  filtered_request.input_batch = std::move(filtered);
  auto transitioned = ExecuteCanonicalInt64SumState(filtered_request);
  if (!transitioned.diagnostic.ok) {
    return refuse(transitioned.diagnostic.diagnostic_code + ":" +
                  transitioned.diagnostic.detail);
  }
  if (!PhysicalMgaStatementContextEqual(
          transitioned.mga_statement_context,
          aggregate.mga_authority.statement_context)) {
    return refuse("filtered aggregate state returned a different MGA statement context");
  }
  const auto result_authority = RevalidateCanonicalExecutionMgaAuthority(
      aggregate.mga_authority, aggregate.physical_dag);
  if (!result_authority.ok)
    return refuse(result_authority.diagnostic_code + ":" +
                  result_authority.detail);
  result.diagnostic = {};
  result.state = std::move(transitioned.state);
  result.selected_plan_uuid = std::move(transitioned.selected_plan_uuid);
  result.executed_physical_node_id =
      transitioned.executed_physical_node_id;
  result.causal_counter_id = transitioned.causal_counter_id;
  result.mga_statement_context = aggregate.mga_authority.statement_context;
  return result;
}

// QOW-SOURCE-QRY-011-DISTINCT-V1
// Apply SUM(DISTINCT int64) by validating and decoding the complete typed
// input before exclusion, then transitioning only the first occurrence of
// each numeric value.  SQL NULL does not enter the DISTINCT set or SUM state.
CanonicalInt64SumDistinctResult ExecuteCanonicalInt64SumDistinct(
    const CanonicalInt64SumDistinctRequest& request) {
  using scratchbird::engine::internal_api::EngineValueState;

  CanonicalInt64SumDistinctResult result;
  const auto refuse = [&](std::string detail) {
    result.diagnostic.ok = false;
    result.diagnostic.diagnostic_code =
        "QOW-DIAG-QRY-011-DISTINCT-REFUSAL-V1";
    result.diagnostic.detail = std::move(detail);
    result.state = {};
    result.distinct_value_count = 0;
    return result;
  };
  const auto& aggregate = request.aggregate_request;
  const auto authority_validation = RevalidateCanonicalExecutionMgaAuthority(
      aggregate.mga_authority, aggregate.physical_dag);
  if (!authority_validation.ok)
    return refuse(authority_validation.diagnostic_code + ":" +
                  authority_validation.detail);
  if (aggregate.selected_physical_node_id == 0 ||
      aggregate.selected_physical_node_id !=
          aggregate.physical_dag.root_physical_node_id) {
    return refuse("selected DISTINCT aggregate is not the physical root");
  }

  const PhysicalNodeRecord* selected_node = nullptr;
  const PhysicalNodeRecord* input_node = nullptr;
  for (const auto& node : aggregate.physical_dag.nodes) {
    if (node.physical_node_id == aggregate.selected_physical_node_id) {
      selected_node = &node;
    }
  }
  if (selected_node == nullptr ||
      selected_node->node_kind != PhysicalNodeKind::kAggregate ||
      selected_node->input_physical_node_ids.size() != 1) {
    return refuse("DISTINCT SUM requires one selected aggregate node");
  }
  for (const auto& node : aggregate.physical_dag.nodes) {
    if (node.physical_node_id ==
        selected_node->input_physical_node_ids.front()) {
      input_node = &node;
      break;
    }
  }
  if (input_node == nullptr) {
    return refuse("DISTINCT aggregate input is unresolved");
  }

  auto input_validation = ValidateCanonicalDescriptorBatch(
      aggregate.input_batch, input_node->output_descriptor_ids);
  if (!input_validation.ok) {
    return refuse(input_validation.diagnostic_code + ":" +
                  input_validation.detail);
  }
  if (aggregate.value_column >= aggregate.input_batch.columns.size()) {
    return refuse("DISTINCT value column is outside the input schema");
  }
  const auto& value_column =
      aggregate.input_batch.columns[aggregate.value_column];
  if (aggregate.value_expression_descriptor_id == 0 ||
      aggregate.value_expression_descriptor_id !=
          value_column.descriptor_id ||
      value_column.descriptor.canonical_type_name != "int64") {
    return refuse("DISTINCT value expression is not bound int64");
  }
  if (selected_node->output_descriptor_ids.size() != 1 ||
      aggregate.result_column.descriptor_id !=
          selected_node->output_descriptor_ids.front() ||
      !aggregate.result_column.nullable ||
      aggregate.result_column.descriptor.canonical_type_name != "int64") {
    return refuse(
        "DISTINCT SUM result is not a bound nullable int64 descriptor");
  }
  DescriptorBatch result_schema;
  result_schema.columns = {aggregate.result_column};
  auto result_validation = ValidateCanonicalDescriptorBatch(
      result_schema, selected_node->output_descriptor_ids);
  if (!result_validation.ok) {
    return refuse(result_validation.diagnostic_code + ":" +
                  result_validation.detail);
  }
  if (aggregate.maximum_transition_count == 0 ||
      aggregate.input_batch.rows.size() >
          aggregate.maximum_transition_count) {
    return refuse("DISTINCT aggregate scan resource bound was exceeded");
  }
  if (request.maximum_distinct_value_count == 0) {
    return refuse("DISTINCT value resource bound is zero");
  }

  std::vector<std::optional<std::int64_t>> decoded_values;
  decoded_values.reserve(aggregate.input_batch.rows.size());
  for (const auto& row : aggregate.input_batch.rows) {
    const auto& value = row.values[aggregate.value_column];
    if (value.state == EngineValueState::sql_null) {
      decoded_values.push_back(std::nullopt);
      continue;
    }
    const auto decoded = DecodeInt64Value(value);
    if (!decoded.ok()) {
      return refuse(decoded.diagnostic.diagnostic_code + ":" +
                    decoded.diagnostic.detail);
    }
    decoded_values.push_back(decoded.value);
  }

  DescriptorBatch distinct_batch;
  distinct_batch.columns = aggregate.input_batch.columns;
  std::set<std::int64_t> observed_values;
  for (std::size_t row_index = 0; row_index < decoded_values.size();
       ++row_index) {
    if (!decoded_values[row_index].has_value()) continue;
    const auto value = *decoded_values[row_index];
    if (observed_values.find(value) != observed_values.end()) continue;
    if (observed_values.size() >= request.maximum_distinct_value_count) {
      return refuse("DISTINCT value resource bound was exceeded");
    }
    observed_values.insert(value);
    distinct_batch.rows.push_back(aggregate.input_batch.rows[row_index]);
  }

  auto distinct_request = aggregate;
  distinct_request.input_batch = std::move(distinct_batch);
  auto transitioned = ExecuteCanonicalInt64SumState(distinct_request);
  if (!transitioned.diagnostic.ok) {
    return refuse(transitioned.diagnostic.diagnostic_code + ":" +
                  transitioned.diagnostic.detail);
  }
  if (!PhysicalMgaStatementContextEqual(
          transitioned.mga_statement_context,
          aggregate.mga_authority.statement_context)) {
    return refuse("DISTINCT aggregate state returned a different MGA statement context");
  }
  const auto result_authority = RevalidateCanonicalExecutionMgaAuthority(
      aggregate.mga_authority, aggregate.physical_dag);
  if (!result_authority.ok)
    return refuse(result_authority.diagnostic_code + ":" +
                  result_authority.detail);
  result.diagnostic = {};
  result.state = std::move(transitioned.state);
  result.distinct_value_count = observed_values.size();
  result.selected_plan_uuid = std::move(transitioned.selected_plan_uuid);
  result.executed_physical_node_id =
      transitioned.executed_physical_node_id;
  result.causal_counter_id = transitioned.causal_counter_id;
  result.mga_statement_context = aggregate.mga_authority.statement_context;
  return result;
}

// QOW-SOURCE-QRY-011-ORDERED-V1
// Apply one descriptor-bound int64 aggregate ORDER BY transition.  The full
// input and square comparison matrix are validated before rows are reordered;
// equal keys retain input order and only the ordered batch reaches SUM state.
CanonicalInt64SumOrderedResult ExecuteCanonicalInt64SumOrdered(
    const CanonicalInt64SumOrderedRequest& request) {
  using scratchbird::engine::internal_api::EngineValueState;

  CanonicalInt64SumOrderedResult result;
  const auto refuse = [&](std::string detail) {
    result.diagnostic.ok = false;
    result.diagnostic.diagnostic_code =
        "QOW-DIAG-QRY-011-ORDERED-REFUSAL-V1";
    result.diagnostic.detail = std::move(detail);
    result.state = {};
    result.ordered_input_row_indices.clear();
    return result;
  };
  const auto& aggregate = request.aggregate_request;

  const auto entry_authority = RevalidateCanonicalExecutionMgaAuthority(
      aggregate.mga_authority, aggregate.physical_dag);
  if (!entry_authority.ok)
    return refuse(entry_authority.diagnostic_code + ":" +
                  entry_authority.detail);

  auto route_request = aggregate;
  route_request.input_batch.rows.clear();
  const auto route_validation =
      ExecuteCanonicalInt64SumState(route_request);
  if (!route_validation.diagnostic.ok) {
    return refuse(route_validation.diagnostic.diagnostic_code + ":" +
                  route_validation.diagnostic.detail);
  }
  if (!PhysicalMgaStatementContextEqual(
          route_validation.mga_statement_context,
          aggregate.mga_authority.statement_context)) {
    return refuse("ordered aggregate preflight returned a different MGA statement context");
  }

  const PhysicalNodeRecord* selected_node = nullptr;
  const PhysicalNodeRecord* input_node = nullptr;
  for (const auto& node : aggregate.physical_dag.nodes) {
    if (node.physical_node_id == aggregate.selected_physical_node_id) {
      selected_node = &node;
    }
  }
  for (const auto& node : aggregate.physical_dag.nodes) {
    if (selected_node != nullptr &&
        node.physical_node_id ==
            selected_node->input_physical_node_ids.front()) {
      input_node = &node;
      break;
    }
  }
  if (selected_node == nullptr || input_node == nullptr) {
    return refuse("ordered aggregate physical route is unresolved");
  }
  auto input_validation = ValidateCanonicalDescriptorBatch(
      aggregate.input_batch, input_node->output_descriptor_ids);
  if (!input_validation.ok) {
    return refuse(input_validation.diagnostic_code + ":" +
                  input_validation.detail);
  }
  if (aggregate.input_batch.rows.size() >
      aggregate.maximum_transition_count) {
    return refuse("ordered aggregate transition bound was exceeded");
  }
  if (request.order_column >= aggregate.input_batch.columns.size()) {
    return refuse("ordered aggregate key is outside the input schema");
  }
  const auto& order_column =
      aggregate.input_batch.columns[request.order_column];
  if (request.order_expression_descriptor_id == 0 ||
      request.order_expression_descriptor_id != order_column.descriptor_id ||
      order_column.descriptor.canonical_type_name != "int64") {
    return refuse("ordered aggregate key is not bound int64");
  }
  if ((request.direction !=
           CanonicalDescriptorOrderDirection::ascending &&
       request.direction !=
           CanonicalDescriptorOrderDirection::descending) ||
      (request.null_placement !=
           CanonicalDescriptorNullPlacement::first &&
       request.null_placement != CanonicalDescriptorNullPlacement::last)) {
    return refuse("ordered aggregate direction or NULL placement is invalid");
  }
  if (!IsCanonicalUuid(request.deterministic_tie_evidence_uuid)) {
    return refuse("ordered aggregate deterministic tie evidence is invalid");
  }

  const auto row_count = aggregate.input_batch.rows.size();
  if (request.maximum_pair_comparisons == 0 ||
      (row_count != 0 &&
       row_count > std::numeric_limits<std::size_t>::max() / row_count)) {
    return refuse("ordered aggregate comparison resource bound overflowed");
  }
  const auto matrix_size = row_count * row_count;
  if (matrix_size > request.maximum_pair_comparisons) {
    return refuse("ordered aggregate comparison resource bound was exceeded");
  }

  std::vector<std::optional<std::int64_t>> order_values;
  order_values.reserve(row_count);
  for (const auto& row : aggregate.input_batch.rows) {
    const auto& value = row.values[request.order_column];
    if (value.state == EngineValueState::sql_null) {
      order_values.push_back(std::nullopt);
      continue;
    }
    const auto decoded = DecodeInt64Value(value);
    if (!decoded.ok()) {
      return refuse(decoded.diagnostic.diagnostic_code + ":" +
                    decoded.diagnostic.detail);
    }
    order_values.push_back(decoded.value);
  }

  const auto compare = [&](const std::optional<std::int64_t>& left,
                           const std::optional<std::int64_t>& right) {
    if (!left.has_value() || !right.has_value()) {
      if (!left.has_value() && !right.has_value()) return 0;
      const auto null_comparison =
          request.null_placement == CanonicalDescriptorNullPlacement::first
              ? -1
              : 1;
      return left.has_value() ? -null_comparison : null_comparison;
    }
    int comparison = *left < *right ? -1 : (*left > *right ? 1 : 0);
    if (request.direction ==
        CanonicalDescriptorOrderDirection::descending) {
      comparison = -comparison;
    }
    return comparison;
  };

  std::vector<std::int8_t> comparisons(matrix_size, 0);
  for (std::size_t left = 0; left < row_count; ++left) {
    for (std::size_t right = left + 1; right < row_count; ++right) {
      const auto comparison = compare(order_values[left], order_values[right]);
      comparisons[left * row_count + right] =
          static_cast<std::int8_t>(comparison);
      comparisons[right * row_count + left] =
          static_cast<std::int8_t>(-comparison);
    }
  }

  std::vector<std::size_t> row_order(row_count);
  std::iota(row_order.begin(), row_order.end(), 0);
  std::stable_sort(row_order.begin(), row_order.end(),
                   [&](const std::size_t left, const std::size_t right) {
                     return comparisons[left * row_count + right] < 0;
                   });

  DescriptorBatch ordered_batch;
  ordered_batch.columns = aggregate.input_batch.columns;
  ordered_batch.rows.reserve(row_count);
  for (const auto row_index : row_order) {
    ordered_batch.rows.push_back(aggregate.input_batch.rows[row_index]);
  }
  auto ordered_request = aggregate;
  ordered_request.input_batch = std::move(ordered_batch);
  auto transitioned = ExecuteCanonicalInt64SumState(ordered_request);
  if (!transitioned.diagnostic.ok) {
    return refuse(transitioned.diagnostic.diagnostic_code + ":" +
                  transitioned.diagnostic.detail);
  }
  if (!PhysicalMgaStatementContextEqual(
          transitioned.mga_statement_context,
          aggregate.mga_authority.statement_context)) {
    return refuse("ordered aggregate state returned a different MGA statement context");
  }
  const auto result_authority = RevalidateCanonicalExecutionMgaAuthority(
      aggregate.mga_authority, aggregate.physical_dag);
  if (!result_authority.ok)
    return refuse(result_authority.diagnostic_code + ":" +
                  result_authority.detail);

  result.diagnostic = {};
  result.state = std::move(transitioned.state);
  result.ordered_input_row_indices = std::move(row_order);
  result.selected_plan_uuid = std::move(transitioned.selected_plan_uuid);
  result.executed_physical_node_id =
      transitioned.executed_physical_node_id;
  result.causal_counter_id = transitioned.causal_counter_id;
  result.mga_statement_context = aggregate.mga_authority.statement_context;
  return result;
}

// QOW-SOURCE-QRY-011-SPILL-V1
// Spill and reopen one typed int64 GROUP BY/SUM transition set through the
// engine temporary-work runtime.  Merged SUM/count components must equal the
// canonical in-memory states, and owned artifacts must be absent before any
// state is published (including cancellation and reopen-refusal paths).
CanonicalInt64SumSpillResult ExecuteCanonicalInt64SumSpill(
    const CanonicalInt64SumSpillRequest& request) {
  using scratchbird::engine::internal_api::EngineValueState;

  CanonicalInt64SumSpillResult result;
  const auto refuse = [&](std::string detail) {
    result.diagnostic.ok = false;
    result.diagnostic.diagnostic_code =
        "QOW-DIAG-QRY-011-SPILL-REFUSAL-V1";
    result.diagnostic.detail = std::move(detail);
    result.groups.clear();
    result.selected_plan_uuid.clear();
    result.executed_physical_node_id = 0;
    result.causal_counter_id = 0;
    return result;
  };
  const auto& aggregate = request.aggregate_request;
  const auto entry_authority = RevalidateCanonicalExecutionMgaAuthority(
      aggregate.mga_authority, aggregate.physical_dag);
  if (!entry_authority.ok)
    return refuse(entry_authority.diagnostic_code + ":" +
                  entry_authority.detail);
  if (aggregate.grouping_set_rule !=
      CanonicalInt64GroupingSetRule::key_only) {
    return refuse("aggregate spill admits one ordinary int64 grouping key");
  }
  if (request.spill_root.empty() || !request.spill_root.is_absolute() ||
      !IsCanonicalUuid(request.spill_owner_uuid) ||
      request.runtime_generation == 0 || request.memory_quota_bytes == 0 ||
      request.maximum_spill_record_count == 0) {
    return refuse("aggregate spill ownership or resource context is invalid");
  }
  const auto owner_directory =
      (request.spill_root / request.spill_owner_uuid).lexically_normal();
  if (owner_directory.filename() != request.spill_owner_uuid) {
    return refuse("aggregate spill owner directory is not exact");
  }
  std::error_code filesystem_error;
  if (std::filesystem::is_symlink(owner_directory, filesystem_error) ||
      filesystem_error) {
    return refuse("aggregate spill owner directory is a symlink or unreadable");
  }
  bool ownership_inspection_ok = false;
  if (HasOwnedAggregateSpillArtifact(owner_directory,
                                     &ownership_inspection_ok) ||
      !ownership_inspection_ok) {
    return refuse("aggregate spill owner already has an artifact");
  }

  auto canonical = ExecuteCanonicalInt64SumGroups(aggregate);
  if (!canonical.diagnostic.ok) {
    return refuse(canonical.diagnostic.diagnostic_code + ":" +
                  canonical.diagnostic.detail);
  }
  if (!PhysicalMgaStatementContextEqual(
          canonical.mga_statement_context,
          aggregate.mga_authority.statement_context)) {
    return refuse("aggregate spill baseline returned a different MGA statement context");
  }
  if (aggregate.input_batch.rows.empty() || canonical.groups.empty()) {
    return refuse("aggregate spill requires a nonempty grouped input");
  }

  const auto group_token = [&](const auto& key,
                               std::string* token) {
    if (token == nullptr) return false;
    if (key.state == EngineValueState::sql_null) {
      *token = "null";
      return true;
    }
    const auto decoded = DecodeInt64Value(key);
    if (!decoded.ok()) return false;
    *token = "value." + std::to_string(decoded.value);
    return true;
  };
  const auto field_key = [](const std::string_view field,
                            const std::string& token) {
    return "qow205." + std::string(field) + "." + token;
  };

  TempSpillRequest spill;
  spill.route_kind = TempSpillRouteKind::kHashAggregate;
  spill.route_label = "qow205.aggregate-spill." + request.spill_owner_uuid;
  spill.spill_directory = owner_directory;
  spill.runtime_generation = request.runtime_generation;
  spill.reopen_runtime_generation = request.reopen_runtime_generation;
  spill.memory_quota_bytes = request.memory_quota_bytes;
  spill.cancellation_requested = request.cancellation_requested;
  spill.restart_recovery_proof_available =
      request.restart_recovery_proof_available;
  spill.authority.engine_mga_snapshot_bound = true;
  spill.authority.transaction_inventory_authoritative = true;
  spill.authority.security_recheck_required = true;
  spill.authority.security_context_bound = true;
  spill.authority.exact_recheck_required = true;

  const auto append_spill_row = [&](std::string key, std::int64_t value,
                                    std::uint64_t row_ordinal) {
    if (spill.rows.size() >= request.maximum_spill_record_count) return false;
    spill.rows.push_back({std::move(key), value, row_ordinal});
    return true;
  };
  for (const auto& group : canonical.groups) {
    std::string token;
    if (!group_token(group.group_key, &token) ||
        !append_spill_row(field_key("sum", token), 0, 0) ||
        !append_spill_row(field_key("transition", token), 0, 0) ||
        !append_spill_row(field_key("non_null", token), 0, 0)) {
      return refuse("aggregate spill group header exceeds its resource bound");
    }
  }
  for (std::size_t row_index = 0;
       row_index < aggregate.input_batch.rows.size(); ++row_index) {
    const auto& row = aggregate.input_batch.rows[row_index];
    const auto& key = row.values[aggregate.key_column];
    const auto& value = row.values[aggregate.value_column];
    std::string token;
    if (!group_token(key, &token) ||
        !append_spill_row(field_key("transition", token), 1,
                          row_index + 1)) {
      return refuse("aggregate spill transition exceeds its resource bound");
    }
    if (value.state == EngineValueState::sql_null) continue;
    const auto decoded = DecodeInt64Value(value);
    if (!decoded.ok() ||
        !append_spill_row(field_key("sum", token), decoded.value,
                          row_index + 1) ||
        !append_spill_row(field_key("non_null", token), 1,
                          row_index + 1)) {
      return refuse("aggregate spill value exceeds its resource bound");
    }
  }

  const auto spilled = ExecuteBoundedTempSpillRoute(spill);
  result.spilled = spilled.spilled;
  result.spill_reopened = spilled.reopen_recovery_proven;
  result.cleanup_proven = spilled.cleanup_proven;
  result.cancellation_observed = request.cancellation_requested;
  result.spill_evidence = spilled.evidence;

  bool cleanup_inspection_ok = false;
  const bool artifact_remains = HasOwnedAggregateSpillArtifact(
      owner_directory, &cleanup_inspection_ok);
  if (!cleanup_inspection_ok || artifact_remains) {
    result.cleanup_proven =
        RemoveOwnedAggregateSpillArtifacts(owner_directory) &&
        result.cleanup_proven;
    return refuse("aggregate spill owned-artifact cleanup is incomplete");
  }
  if (!spilled.ok || !spilled.spilled || !spilled.cleanup_proven ||
      !spilled.reopen_recovery_proven) {
    return refuse(spilled.diagnostic_code + ":" + spilled.fallback_reason);
  }

  std::map<std::string, std::int64_t> merged_fields;
  for (const auto& output : spilled.output_rows) {
    const auto separator = output.rfind('=');
    std::int64_t merged_value = 0;
    if (separator == std::string::npos || separator == 0 ||
        !ParseInt64Text(std::string_view(output).substr(separator + 1),
                        &merged_value) ||
        !merged_fields.emplace(output.substr(0, separator), merged_value)
             .second) {
      return refuse("aggregate spill merge output is malformed");
    }
  }
  if (merged_fields.size() != canonical.groups.size() * 3) {
    return refuse("aggregate spill merge field cardinality is invalid");
  }
  for (const auto& group : canonical.groups) {
    std::string token;
    if (!group_token(group.group_key, &token) ||
        group.sum_state.transition_count >
            static_cast<std::uint64_t>(
                std::numeric_limits<std::int64_t>::max()) ||
        group.sum_state.non_null_count >
            static_cast<std::uint64_t>(
                std::numeric_limits<std::int64_t>::max())) {
      return refuse("aggregate spill canonical state cannot be compared");
    }
    const auto sum = merged_fields.find(field_key("sum", token));
    const auto transition =
        merged_fields.find(field_key("transition", token));
    const auto non_null = merged_fields.find(field_key("non_null", token));
    if (sum == merged_fields.end() || transition == merged_fields.end() ||
        non_null == merged_fields.end() ||
        sum->second != group.sum_state.accumulated_value ||
        transition->second !=
            static_cast<std::int64_t>(group.sum_state.transition_count) ||
        non_null->second !=
            static_cast<std::int64_t>(group.sum_state.non_null_count)) {
      return refuse("aggregate spill merge differs from canonical state");
    }
  }

  const auto result_authority = RevalidateCanonicalExecutionMgaAuthority(
      aggregate.mga_authority, aggregate.physical_dag);
  if (!result_authority.ok)
    return refuse(result_authority.diagnostic_code + ":" +
                  result_authority.detail);

  result.diagnostic = {};
  result.groups = std::move(canonical.groups);
  result.selected_plan_uuid = std::move(canonical.selected_plan_uuid);
  result.executed_physical_node_id =
      canonical.executed_physical_node_id;
  result.causal_counter_id = canonical.causal_counter_id;
  result.mga_statement_context = aggregate.mga_authority.statement_context;
  return result;
}

// QOW-SOURCE-QRY-012-KEY-V1
// Evaluate already-bound composite typed equality keys for every physical row
// pair. Terms combine through SQL AND: FALSE dominates UNKNOWN, and only all
// non-NULL equal terms produce TRUE. Character terms require the same durable
// collation authority as canonical typed ordering. Full route/input validation
// precedes the bounded comparison matrix.
CanonicalCompositeJoinKeyResult ExecuteCanonicalCompositeJoinKey(
    const CanonicalCompositeJoinKeyRequest& request) {
  namespace api = scratchbird::engine::internal_api;

  CanonicalCompositeJoinKeyResult result;
  const auto refuse = [&](std::string detail) {
    result.diagnostic.ok = false;
    result.diagnostic.diagnostic_code =
        "QOW-DIAG-QRY-012-KEY-REFUSAL-V1";
    result.diagnostic.detail = std::move(detail);
    result.pair_truth_values.clear();
    result.pair_count = 0;
    return result;
  };
  const auto entry_authority = RevalidateCanonicalExecutionMgaAuthority(
      request.mga_authority, request.physical_dag);
  if (!entry_authority.ok)
    return refuse(entry_authority.diagnostic_code + ":" +
                  entry_authority.detail);
  if (request.key_terms.empty() || request.maximum_key_term_count == 0 ||
      request.key_terms.size() > request.maximum_key_term_count ||
      request.maximum_key_comparisons == 0) {
    return refuse("composite join key term resource contract is invalid");
  }
  const auto left_count = request.left_batch.rows.size();
  const auto right_count = request.right_batch.rows.size();
  if (left_count != 0 &&
      right_count > std::numeric_limits<std::size_t>::max() / left_count) {
    return refuse("join pair cardinality overflowed");
  }
  const auto pair_count = left_count * right_count;
  if (pair_count != 0 &&
      request.key_terms.size() >
          request.maximum_key_comparisons / pair_count) {
    return refuse("composite join key comparison bound was exceeded");
  }
  CanonicalDescriptorInnerJoinRequest route;
  route.physical_dag = request.physical_dag;
  route.selected_physical_node_id = request.selected_physical_node_id;
  route.left_batch = request.left_batch;
  route.right_batch = request.right_batch;
  route.pair_truth_values.assign(
      pair_count, api::EngineSqlTruthValue::false_value);
  route.mga_authority = request.mga_authority;
  const auto route_validation = ExecuteCanonicalDescriptorInnerJoin(route);
  if (!route_validation.diagnostic.ok) {
    return refuse(route_validation.diagnostic.diagnostic_code + ":" +
                  route_validation.diagnostic.detail);
  }
  if (!PhysicalMgaStatementContextEqual(
          route_validation.mga_statement_context,
          request.mga_authority.statement_context)) {
    return refuse("composite join route returned a different MGA statement context");
  }

  std::set<std::pair<std::uint32_t, std::uint32_t>> bound_term_handles;
  for (const auto& term : request.key_terms) {
    if (term.left_column >= request.left_batch.columns.size() ||
        term.right_column >= request.right_batch.columns.size()) {
      return refuse("composite join key column is outside its input schema");
    }
    const auto& left_column = request.left_batch.columns[term.left_column];
    const auto& right_column = request.right_batch.columns[term.right_column];
    if (term.left_expression_descriptor_id == 0 ||
        term.right_expression_descriptor_id == 0 ||
        term.left_expression_descriptor_id != left_column.descriptor_id ||
        term.right_expression_descriptor_id != right_column.descriptor_id ||
        left_column.descriptor.canonical_type_name !=
            right_column.descriptor.canonical_type_name) {
      return refuse("composite join key is not bound to compatible types");
    }
    const auto left_term_validation = ValidateCanonicalDescriptorOrderTerm(
        JoinKeyOrderTerm(term, true), left_column);
    const auto right_term_validation = ValidateCanonicalDescriptorOrderTerm(
        JoinKeyOrderTerm(term, false), right_column);
    if (!left_term_validation.ok || !right_term_validation.ok) {
      const auto& diagnostic = !left_term_validation.ok
                                   ? left_term_validation
                                   : right_term_validation;
      return refuse(diagnostic.diagnostic_code + ":" + diagnostic.detail);
    }
    if (!bound_term_handles
             .emplace(term.left_expression_descriptor_id,
                      term.right_expression_descriptor_id)
             .second) {
      return refuse("composite join key repeats a bound term");
    }
  }

  std::vector<api::EngineSqlTruthValue> pair_truth_values;
  pair_truth_values.reserve(pair_count);
  for (const auto& left_row : request.left_batch.rows) {
    for (const auto& right_row : request.right_batch.rows) {
      bool saw_unknown = false;
      bool saw_false = false;
      for (const auto& term : request.key_terms) {
        const auto& left_value = left_row.values[term.left_column];
        const auto& right_value = right_row.values[term.right_column];
        if (left_value.state == api::EngineValueState::sql_null ||
            right_value.state == api::EngineValueState::sql_null) {
          saw_unknown = true;
          continue;
        }
        bool equal = false;
        std::string detail;
        if (!CompareCanonicalJoinKeyValues(left_value, right_value, term,
                                           &equal, &detail)) {
          return refuse("composite join key operand is invalid:" + detail);
        }
        if (!equal) {
          saw_false = true;
          break;
        }
      }
      pair_truth_values.push_back(
          saw_false
              ? api::EngineSqlTruthValue::false_value
              : (saw_unknown ? api::EngineSqlTruthValue::unknown
                             : api::EngineSqlTruthValue::true_value));
    }
  }

  const auto result_authority = RevalidateCanonicalExecutionMgaAuthority(
      request.mga_authority, request.physical_dag);
  if (!result_authority.ok)
    return refuse(result_authority.diagnostic_code + ":" +
                  result_authority.detail);
  result.diagnostic = {};
  result.pair_truth_values = std::move(pair_truth_values);
  result.pair_count = pair_count;
  result.selected_plan_uuid = route_validation.selected_plan_uuid;
  result.executed_physical_node_id =
      route_validation.executed_physical_node_id;
  result.causal_counter_id = route_validation.causal_counter_id;
  result.mga_statement_context = request.mga_authority.statement_context;
  return result;
}

// QOW-SOURCE-QRY-012-RESIDUAL-V1
// Recheck a fully bound residual ON predicate only for pairs selected by the
// canonical composite-key route.  The complete residual vector is validated
// before candidate filtering so malformed non-candidate input cannot hide.
CanonicalJoinResidualResult ExecuteCanonicalJoinResidual(
    const CanonicalJoinResidualRequest& request) {
  namespace api = scratchbird::engine::internal_api;

  CanonicalJoinResidualResult result;
  const auto refuse = [&](std::string detail) {
    result.diagnostic.ok = false;
    result.diagnostic.diagnostic_code =
        "QOW-DIAG-QRY-012-RESIDUAL-REFUSAL-V1";
    result.diagnostic.detail = std::move(detail);
    result.output_batch = {};
    result.accepted_pair_indices.clear();
    result.candidate_pair_count = 0;
    result.residual_recheck_count = 0;
    result.selected_plan_uuid.clear();
    result.executed_physical_node_id = 0;
    result.causal_counter_id = 0;
    return result;
  };
  if (request.maximum_candidate_rechecks == 0) {
    return refuse("residual join candidate resource contract is invalid");
  }
  const auto keys = ExecuteCanonicalCompositeJoinKey(request.key_request);
  if (!keys.diagnostic.ok) {
    return refuse(keys.diagnostic.diagnostic_code + ":" +
                  keys.diagnostic.detail);
  }
  if (!PhysicalMgaStatementContextEqual(
          keys.mga_statement_context,
          request.key_request.mga_authority.statement_context)) {
    return refuse("residual join key route returned a different MGA statement context");
  }
  if (request.residual_truth_values.size() != keys.pair_count) {
    return refuse("residual join predicate cardinality is not bound");
  }

  for (std::size_t pair = 0; pair < request.residual_truth_values.size();
       ++pair) {
    bool passes = false;
    std::string refusal_detail;
    if (!api::QowPredicateConsumerPassesV1(
            request.residual_truth_values[pair],
            api::EnginePredicateConsumer::join_on, &passes,
            &refusal_detail)) {
      return refuse("residual join predicate at pair " +
                    std::to_string(pair) + " is invalid:" +
                    refusal_detail);
    }
  }

  const auto candidate_pair_count = static_cast<std::size_t>(std::count(
      keys.pair_truth_values.begin(), keys.pair_truth_values.end(),
      api::EngineSqlTruthValue::true_value));
  if (candidate_pair_count > request.maximum_candidate_rechecks) {
    return refuse("residual join candidate recheck bound was exceeded");
  }
  std::vector<api::EngineSqlTruthValue> accepted_pair_truth_values(
      keys.pair_count, api::EngineSqlTruthValue::false_value);
  std::vector<std::size_t> accepted_pair_indices;
  accepted_pair_indices.reserve(candidate_pair_count);
  for (std::size_t pair = 0; pair < keys.pair_count; ++pair) {
    if (keys.pair_truth_values[pair] ==
        api::EngineSqlTruthValue::true_value) {
      accepted_pair_truth_values[pair] =
          request.residual_truth_values[pair];
      if (request.residual_truth_values[pair] ==
          api::EngineSqlTruthValue::true_value) {
        accepted_pair_indices.push_back(pair);
      }
    }
  }

  CanonicalDescriptorInnerJoinRequest join;
  join.physical_dag = request.key_request.physical_dag;
  join.selected_physical_node_id =
      request.key_request.selected_physical_node_id;
  join.left_batch = request.key_request.left_batch;
  join.right_batch = request.key_request.right_batch;
  join.pair_truth_values = std::move(accepted_pair_truth_values);
  join.consumer = api::EnginePredicateConsumer::join_on;
  join.maximum_output_rows = request.maximum_candidate_rechecks;
  join.mga_authority = request.key_request.mga_authority;
  auto joined = ExecuteCanonicalDescriptorInnerJoin(join);
  if (!joined.diagnostic.ok) {
    return refuse(joined.diagnostic.diagnostic_code + ":" +
                  joined.diagnostic.detail);
  }
  if (!PhysicalMgaStatementContextEqual(
          joined.mga_statement_context,
          request.key_request.mga_authority.statement_context)) {
    return refuse("residual join materialization returned a different MGA statement context");
  }
  const auto result_authority = RevalidateCanonicalExecutionMgaAuthority(
      request.key_request.mga_authority, request.key_request.physical_dag);
  if (!result_authority.ok)
    return refuse(result_authority.diagnostic_code + ":" +
                  result_authority.detail);
  result.diagnostic = {};
  result.output_batch = std::move(joined.output_batch);
  result.accepted_pair_indices = std::move(accepted_pair_indices);
  result.candidate_pair_count = candidate_pair_count;
  result.residual_recheck_count = candidate_pair_count;
  result.selected_plan_uuid = std::move(joined.selected_plan_uuid);
  result.executed_physical_node_id = joined.executed_physical_node_id;
  result.causal_counter_id = joined.causal_counter_id;
  result.mga_statement_context =
      request.key_request.mga_authority.statement_context;
  return result;
}

// QOW-SOURCE-QRY-012-KIND-V1
// Produce canonical CROSS, INNER, LEFT/RIGHT/FULL OUTER, LEFT SEMI, and LEFT
// ANTI cardinality from one physical pair identity. Accepted pairs retain
// duplicate multiplicity; outer extensions use descriptor-preserving SQL NULL.
CanonicalJoinKindResult ExecuteCanonicalJoinKind(
    const CanonicalJoinKindRequest& request) {
  namespace api = scratchbird::engine::internal_api;

  CanonicalJoinKindResult result;
  const auto refuse = [&](std::string detail) {
    result.diagnostic.ok = false;
    result.diagnostic.diagnostic_code =
        "QOW-DIAG-QRY-012-KIND-REFUSAL-V1";
    result.diagnostic.detail = std::move(detail);
    result.output_batch = {};
    result.matched_pair_count = 0;
    result.unmatched_left_row_count = 0;
    result.unmatched_right_row_count = 0;
    result.emitted_left_row_count = 0;
    result.selected_plan_uuid.clear();
    result.executed_physical_node_id = 0;
    result.causal_counter_id = 0;
    return result;
  };
  const auto refuse_resource = [&](std::string detail) {
    (void)refuse(std::move(detail));
    result.diagnostic.diagnostic_code = "SBLR.PLAN_TREE.RESOURCE_LIMIT";
    return result;
  };
  const auto& cancellation_requested =
      request.cancellation_requested;
  const auto poll_cancellation = [&](const char* phase,
                                     const std::size_t row = 0) {
    bool observed = false;
    auto diagnostic = PollFoundationJoinCancellation(
        cancellation_requested, phase, row, &observed);
    if (!diagnostic.has_value()) return false;
    result.cancellation_observed = observed;
    result.transient_state_cleanup_proven = true;
    result.diagnostic = std::move(*diagnostic);
    result.output_batch = {};
    result.matched_pair_count = 0;
    result.unmatched_left_row_count = 0;
    result.unmatched_right_row_count = 0;
    result.emitted_left_row_count = 0;
    return true;
  };
  const auto adopt_validation_cancellation =
      [&](const DescriptorRuntimeDiagnostic& diagnostic,
          const bool observed) {
        if (diagnostic.diagnostic_code !=
                "QOW-DIAG-QRY-012-JOIN-CANCELLED-V1" &&
            diagnostic.diagnostic_code !=
                "QOW-DIAG-QRY-012-CANCELLATION-PROBE-V1") {
          return false;
        }
        result.diagnostic = diagnostic;
        result.cancellation_observed = observed;
        result.transient_state_cleanup_proven = true;
        result.output_batch = {};
        result.matched_pair_count = 0;
        result.unmatched_left_row_count = 0;
        result.unmatched_right_row_count = 0;
        result.emitted_left_row_count = 0;
        return true;
      };
  const auto add_payload_bytes = [](const std::size_t additional,
                                    std::size_t* total) {
    if (total == nullptr ||
        additional > std::numeric_limits<std::size_t>::max() - *total) {
      return false;
    }
    *total += additional;
    return true;
  };
  const auto account_tuple_payload =
      [&](const DescriptorTuple& tuple, const char* phase,
          const std::size_t row, std::size_t* total) {
        for (std::size_t column = 0; column < tuple.values.size(); ++column) {
          if (poll_cancellation(phase, row)) return false;
          const auto& value = tuple.values[column];
          if (!add_payload_bytes(value.encoded_value.size(), total) ||
              !add_payload_bytes(value.binary_value.size(), total)) {
            return false;
          }
        }
        return true;
      };
  const auto account_batch_payload =
      [&](const DescriptorBatch& batch, const char* phase,
          std::size_t* total) {
        for (std::size_t row = 0; row < batch.rows.size(); ++row) {
          if (!account_tuple_payload(batch.rows[row], phase, row, total)) {
            return false;
          }
        }
        return true;
      };

  switch (request.join_kind) {
    case CanonicalAcceptedJoinKind::kCross:
    case CanonicalAcceptedJoinKind::kInner:
    case CanonicalAcceptedJoinKind::kLeftOuter:
    case CanonicalAcceptedJoinKind::kRightOuter:
    case CanonicalAcceptedJoinKind::kFullOuter:
    case CanonicalAcceptedJoinKind::kLeftSemi:
    case CanonicalAcceptedJoinKind::kLeftAnti:
      break;
    default:
      return refuse("join kind is outside the accepted canonical profile");
  }
  if (request.maximum_output_rows == 0) {
    return refuse("join-kind output resource contract is invalid");
  }
  const auto& key_request = request.residual_request.key_request;
  const bool borrowed_left = key_request.borrowed_left_batch != nullptr;
  const bool borrowed_right = key_request.borrowed_right_batch != nullptr;
  if (borrowed_left != borrowed_right ||
      (borrowed_left &&
       (!key_request.left_batch.columns.empty() ||
        !key_request.left_batch.rows.empty() ||
        !key_request.right_batch.columns.empty() ||
        !key_request.right_batch.rows.empty())) ||
      (borrowed_left && !request.bound_pair_truth_profile)) {
    return refuse("join input ownership carriage is inconsistent");
  }
  const auto& left_batch = borrowed_left ? *key_request.borrowed_left_batch
                                         : key_request.left_batch;
  const auto& right_batch = borrowed_right ? *key_request.borrowed_right_batch
                                           : key_request.right_batch;
  const auto entry_authority = RevalidateCanonicalExecutionMgaAuthority(
      key_request.mga_authority, key_request.physical_dag);
  if (!entry_authority.ok)
    return refuse(entry_authority.diagnostic_code + ":" +
                  entry_authority.detail);
  const auto left_count = left_batch.rows.size();
  const auto right_count = right_batch.rows.size();
  if (left_count != 0 &&
      right_count > std::numeric_limits<std::size_t>::max() / left_count) {
    return refuse("join pair cardinality overflowed");
  }
  const auto pair_count = left_count * right_count;
  if (poll_cancellation("before join-kind pair allocation")) return result;
  const bool borrowed_bound_truth =
      request.borrowed_bound_pair_truth_values != nullptr;
  if ((borrowed_bound_truth && !request.bound_pair_truth_profile) ||
      (borrowed_bound_truth &&
       !request.residual_request.residual_truth_values.empty())) {
    return refuse("join truth ownership carriage is inconsistent");
  }
  const auto& bound_pair_truth_values =
      borrowed_bound_truth
          ? *request.borrowed_bound_pair_truth_values
          : request.residual_request.residual_truth_values;

  std::vector<std::size_t> accepted_pair_indices;
  std::string selected_plan_uuid;
  std::uint64_t executed_physical_node_id = 0;
  std::uint64_t causal_counter_id = 0;
  std::optional<std::size_t> selected_node_memory_grant;
  std::size_t retained_join_state_bytes = 0;
  if (request.bound_pair_truth_profile) {
    if (request.conditionless_predicate || !key_request.key_terms.empty() ||
        request.residual_request.maximum_candidate_rechecks == 0 ||
        bound_pair_truth_values.size() != pair_count ||
        pair_count > request.residual_request.maximum_candidate_rechecks) {
      return refuse("bound join truth matrix contract is invalid or exhausted");
    }

    const PhysicalNodeRecord* selected_node = nullptr;
    const PhysicalNodeRecord* left_node = nullptr;
    const PhysicalNodeRecord* right_node = nullptr;
    for (const auto& node : key_request.physical_dag.nodes) {
      if (poll_cancellation("while resolving selected join node")) {
        return result;
      }
      if (node.physical_node_id == key_request.selected_physical_node_id) {
        selected_node = &node;
      }
    }
    if (selected_node == nullptr ||
        selected_node->physical_node_id !=
            key_request.physical_dag.root_physical_node_id ||
        selected_node->node_kind != PhysicalNodeKind::kJoin ||
        selected_node->implementation_id !=
            CanonicalBoundJoinImplementationId(request.join_kind) ||
        selected_node->input_physical_node_ids.size() != 2 ||
        selected_node->input_physical_node_ids[0] ==
            selected_node->input_physical_node_ids[1]) {
      return refuse("bound join truth route is not one selected two-input root");
    }
    for (const auto& node : key_request.physical_dag.nodes) {
      if (poll_cancellation("while resolving join input nodes")) {
        return result;
      }
      if (node.physical_node_id ==
          selected_node->input_physical_node_ids[0]) {
        left_node = &node;
      }
      if (node.physical_node_id ==
          selected_node->input_physical_node_ids[1]) {
        right_node = &node;
      }
    }
    if (left_node == nullptr || right_node == nullptr) {
      return refuse("bound join truth input identity is unresolved");
    }
    if (key_request.physical_dag.memory_budget_bytes != 0 &&
        selected_node->memory_bytes_required != 0 &&
        selected_node->memory_bytes_required <=
            key_request.physical_dag.memory_budget_bytes &&
        selected_node->memory_bytes_required <=
            static_cast<std::uint64_t>(
                std::numeric_limits<std::size_t>::max())) {
      selected_node_memory_grant =
          static_cast<std::size_t>(selected_node->memory_bytes_required);
    }
    if (!selected_node_memory_grant.has_value()) {
      return refuse_resource(
          "bound join selected-node memory grant is absent or invalid");
    }
    // The optimizer/runtime memory ABI measures retained encoded/binary
    // payload plus explicit operator work state. Borrowed batches are charged
    // once here, without copying them, before pair state is reserved.
    retained_join_state_bytes = 2;
    std::size_t truth_bytes = 0;
    std::size_t accepted_bytes = 0;
    const bool input_payload_accounted =
        account_batch_payload(left_batch,
                              "while accounting left join input payload",
                              &retained_join_state_bytes) &&
        account_batch_payload(right_batch,
                              "while accounting right join input payload",
                              &retained_join_state_bytes);
    if (!input_payload_accounted) {
      if (!result.diagnostic.ok) return result;
      return refuse_resource("bound join retained-state size overflowed");
    }
    if ((pair_count != 0 &&
         sizeof(api::EngineSqlTruthValue) >
             std::numeric_limits<std::size_t>::max() / pair_count) ||
        (pair_count != 0 &&
         sizeof(std::size_t) >
             std::numeric_limits<std::size_t>::max() / pair_count)) {
      return refuse_resource("bound join retained-state size overflowed");
    }
    truth_bytes = pair_count * sizeof(api::EngineSqlTruthValue);
    accepted_bytes = pair_count * sizeof(std::size_t);
    if (!add_payload_bytes(truth_bytes, &retained_join_state_bytes) ||
        !add_payload_bytes(accepted_bytes, &retained_join_state_bytes) ||
        !add_payload_bytes(left_count, &retained_join_state_bytes) ||
        !add_payload_bytes(right_count, &retained_join_state_bytes)) {
      return refuse_resource("bound join retained-state size overflowed");
    }
    if (retained_join_state_bytes > *selected_node_memory_grant) {
      return refuse_resource(
          "bound join retained inputs, truth, and pair state exceed the "
          "selected-node memory grant");
    }
    if (poll_cancellation("before bound join input validation")) return result;
    bool validation_cancelled = false;
    const auto left_validation = ValidateCanonicalDescriptorBatch(
        left_batch, left_node->output_descriptor_ids, cancellation_requested,
        &validation_cancelled);
    if (!left_validation.ok &&
        adopt_validation_cancellation(left_validation,
                                      validation_cancelled)) {
      return result;
    }
    validation_cancelled = false;
    const auto right_validation = ValidateCanonicalDescriptorBatch(
        right_batch, right_node->output_descriptor_ids, cancellation_requested,
        &validation_cancelled);
    if (!right_validation.ok &&
        adopt_validation_cancellation(right_validation,
                                      validation_cancelled)) {
      return result;
    }
    if (!left_validation.ok || !right_validation.ok) {
      const auto& diagnostic =
          !left_validation.ok ? left_validation : right_validation;
      return refuse(diagnostic.diagnostic_code + ":" + diagnostic.detail);
    }
    if (poll_cancellation("after bound join input validation")) return result;

    std::vector<std::uint32_t> expected_output_descriptor_ids;
    expected_output_descriptor_ids.reserve(
        left_node->output_descriptor_ids.size() +
        right_node->output_descriptor_ids.size());
    for (const auto descriptor_id : left_node->output_descriptor_ids) {
      if (poll_cancellation(
              "while binding expected join output descriptors")) {
        return result;
      }
      expected_output_descriptor_ids.push_back(descriptor_id);
    }
    if (request.join_kind != CanonicalAcceptedJoinKind::kLeftSemi &&
        request.join_kind != CanonicalAcceptedJoinKind::kLeftAnti) {
      for (const auto descriptor_id : right_node->output_descriptor_ids) {
        if (poll_cancellation(
                "while binding expected join output descriptors")) {
          return result;
        }
        expected_output_descriptor_ids.push_back(descriptor_id);
      }
    }
    if (selected_node->output_descriptor_ids !=
        expected_output_descriptor_ids) {
      return refuse("bound join truth output schema is not canonical");
    }

    accepted_pair_indices.reserve(pair_count);
    for (std::size_t pair = 0; pair < pair_count; ++pair) {
      if (poll_cancellation("while scanning bound join truth", pair)) {
        return result;
      }
      bool passes = false;
      std::string refusal_detail;
      if (!api::QowPredicateConsumerPassesV1(
              bound_pair_truth_values[pair],
              api::EnginePredicateConsumer::join_on, &passes,
              &refusal_detail)) {
        return refuse("bound join truth at pair " + std::to_string(pair) +
                      " is invalid:" + refusal_detail);
      }
      if (request.join_kind == CanonicalAcceptedJoinKind::kCross &&
          !passes) {
        return refuse("CROSS join bound truth matrix is not conditionless");
      }
      if (passes) accepted_pair_indices.push_back(pair);
    }
    selected_plan_uuid = key_request.physical_dag.selected_plan_uuid;
    executed_physical_node_id = selected_node->physical_node_id;
    causal_counter_id = selected_node->causal_counter_id;
  } else if (request.conditionless_predicate) {
    const PhysicalNodeRecord* conditionless_node = nullptr;
    for (const auto& node : key_request.physical_dag.nodes) {
      if (node.physical_node_id ==
          key_request.selected_physical_node_id) {
        conditionless_node = &node;
        break;
      }
    }
    if (request.join_kind == CanonicalAcceptedJoinKind::kCross ||
        !key_request.key_terms.empty() ||
        !request.residual_request.residual_truth_values.empty() ||
        request.residual_request.maximum_candidate_rechecks == 0 ||
        conditionless_node == nullptr ||
        conditionless_node->node_kind != PhysicalNodeKind::kJoin ||
        conditionless_node->implementation_id !=
            "join.natural-conditionless.typed.v1" ||
        pair_count >
            request.residual_request.maximum_candidate_rechecks) {
      return refuse("conditionless join contract is invalid or exhausted");
    }
    CanonicalDescriptorInnerJoinRequest conditionless;
    conditionless.physical_dag = key_request.physical_dag;
    conditionless.selected_physical_node_id =
        key_request.selected_physical_node_id;
    conditionless.left_batch = left_batch;
    conditionless.right_batch = right_batch;
    conditionless.pair_truth_values.reserve(pair_count);
    for (std::size_t pair = 0; pair < pair_count; ++pair) {
      if (poll_cancellation("while binding conditionless join truth", pair)) {
        return result;
      }
      conditionless.pair_truth_values.push_back(
          api::EngineSqlTruthValue::true_value);
    }
    conditionless.maximum_output_rows = request.maximum_output_rows;
    conditionless.mga_authority = key_request.mga_authority;
    auto joined = ExecuteCanonicalDescriptorInnerJoin(conditionless);
    if (!joined.diagnostic.ok) {
      return refuse(joined.diagnostic.diagnostic_code + ":" +
                    joined.diagnostic.detail);
    }
    if (!PhysicalMgaStatementContextEqual(
            joined.mga_statement_context,
            key_request.mga_authority.statement_context)) {
      return refuse("conditionless join returned a different MGA statement context");
    }
    accepted_pair_indices.reserve(pair_count);
    for (std::size_t pair = 0; pair < pair_count; ++pair) {
      if (poll_cancellation("while binding conditionless pair identities",
                            pair)) {
        return result;
      }
      accepted_pair_indices.push_back(pair);
    }
    selected_plan_uuid = std::move(joined.selected_plan_uuid);
    executed_physical_node_id = joined.executed_physical_node_id;
    causal_counter_id = joined.causal_counter_id;
  } else if (request.join_kind == CanonicalAcceptedJoinKind::kCross) {
    CanonicalDescriptorInnerJoinRequest cross;
    cross.physical_dag = key_request.physical_dag;
    cross.selected_physical_node_id = key_request.selected_physical_node_id;
    cross.left_batch = left_batch;
    cross.right_batch = right_batch;
    cross.pair_truth_values.reserve(pair_count);
    for (std::size_t pair = 0; pair < pair_count; ++pair) {
      if (poll_cancellation("while binding CROSS join truth", pair)) {
        return result;
      }
      cross.pair_truth_values.push_back(
          api::EngineSqlTruthValue::true_value);
    }
    cross.maximum_output_rows = request.maximum_output_rows;
    cross.mga_authority = key_request.mga_authority;
    auto crossed = ExecuteCanonicalDescriptorInnerJoin(cross);
    if (!crossed.diagnostic.ok) {
      return refuse(crossed.diagnostic.diagnostic_code + ":" +
                    crossed.diagnostic.detail);
    }
    if (!PhysicalMgaStatementContextEqual(
            crossed.mga_statement_context,
            key_request.mga_authority.statement_context)) {
      return refuse("cross join returned a different MGA statement context");
    }
    accepted_pair_indices.reserve(pair_count);
    for (std::size_t pair = 0; pair < pair_count; ++pair) {
      if (poll_cancellation("while binding CROSS pair identities", pair)) {
        return result;
      }
      accepted_pair_indices.push_back(pair);
    }
    selected_plan_uuid = std::move(crossed.selected_plan_uuid);
    executed_physical_node_id = crossed.executed_physical_node_id;
    causal_counter_id = crossed.causal_counter_id;
  } else {
    auto residual = ExecuteCanonicalJoinResidual(request.residual_request);
    if (!residual.diagnostic.ok) {
      return refuse(residual.diagnostic.diagnostic_code + ":" +
                    residual.diagnostic.detail);
    }
    if (!PhysicalMgaStatementContextEqual(
            residual.mga_statement_context,
            key_request.mga_authority.statement_context)) {
      return refuse("join residual returned a different MGA statement context");
    }
    if (residual.accepted_pair_indices.size() !=
        residual.output_batch.rows.size()) {
      return refuse("join residual row and pair identities differ");
    }
    accepted_pair_indices = std::move(residual.accepted_pair_indices);
    selected_plan_uuid = std::move(residual.selected_plan_uuid);
    executed_physical_node_id = residual.executed_physical_node_id;
    causal_counter_id = residual.causal_counter_id;
  }

  std::vector<bool> matched_left_rows(left_count, false);
  std::vector<bool> matched_right_rows(right_count, false);
  std::size_t previous_pair = 0;
  bool has_previous_pair = false;
  for (const auto pair : accepted_pair_indices) {
    if (poll_cancellation("while binding matched pair identities", pair)) {
      return result;
    }
    if (right_count == 0 || pair >= pair_count ||
        (has_previous_pair && pair <= previous_pair)) {
      return refuse("residual pair identity is not canonical");
    }
    matched_left_rows[pair / right_count] = true;
    matched_right_rows[pair % right_count] = true;
    previous_pair = pair;
    has_previous_pair = true;
  }
  std::size_t unmatched_left_row_count = 0;
  for (std::size_t left = 0; left < matched_left_rows.size(); ++left) {
    if (poll_cancellation("while counting unmatched left rows", left)) {
      return result;
    }
    if (!matched_left_rows[left]) ++unmatched_left_row_count;
  }
  std::size_t unmatched_right_row_count = 0;
  for (std::size_t right = 0; right < matched_right_rows.size(); ++right) {
    if (poll_cancellation("while counting unmatched right rows", right)) {
      return result;
    }
    if (!matched_right_rows[right]) ++unmatched_right_row_count;
  }
  const auto matched_left_row_count = left_count - unmatched_left_row_count;

  std::size_t expected_output_rows = accepted_pair_indices.size();
  const auto add_expected_rows = [&](const std::size_t additional) {
    if (expected_output_rows >
        std::numeric_limits<std::size_t>::max() - additional) {
      return false;
    }
    expected_output_rows += additional;
    return true;
  };
  switch (request.join_kind) {
    case CanonicalAcceptedJoinKind::kLeftOuter:
      if (!add_expected_rows(unmatched_left_row_count)) {
        return refuse("left outer join output cardinality overflowed");
      }
      break;
    case CanonicalAcceptedJoinKind::kRightOuter:
      if (!add_expected_rows(unmatched_right_row_count)) {
        return refuse("right outer join output cardinality overflowed");
      }
      break;
    case CanonicalAcceptedJoinKind::kFullOuter:
      if (!add_expected_rows(unmatched_left_row_count) ||
          !add_expected_rows(unmatched_right_row_count)) {
        return refuse("full outer join output cardinality overflowed");
      }
      break;
    case CanonicalAcceptedJoinKind::kLeftSemi:
      expected_output_rows = matched_left_row_count;
      break;
    case CanonicalAcceptedJoinKind::kLeftAnti:
      expected_output_rows = unmatched_left_row_count;
      break;
    case CanonicalAcceptedJoinKind::kCross:
    case CanonicalAcceptedJoinKind::kInner:
      break;
  }
  if (expected_output_rows > request.maximum_output_rows) {
    return refuse("join-kind output row bound was exceeded");
  }

  if (selected_node_memory_grant.has_value()) {
    std::size_t planned_peak_bytes = retained_join_state_bytes;
    const bool left_only_output =
        request.join_kind == CanonicalAcceptedJoinKind::kLeftSemi ||
        request.join_kind == CanonicalAcceptedJoinKind::kLeftAnti;
    const auto account_joined_pair = [&](const std::size_t pair) {
      return right_count != 0 &&
             account_tuple_payload(
                 left_batch.rows[pair / right_count],
                 "while accounting joined left payload", pair,
                 &planned_peak_bytes) &&
             account_tuple_payload(
                 right_batch.rows[pair % right_count],
                 "while accounting joined right payload", pair,
                 &planned_peak_bytes);
    };
    bool output_accounted = true;
    if (left_only_output) {
      const bool emit_matches =
          request.join_kind == CanonicalAcceptedJoinKind::kLeftSemi;
      for (std::size_t left = 0; left < left_count; ++left) {
        if (poll_cancellation("while accounting semi or anti output", left)) {
          return result;
        }
        if (matched_left_rows[left] == emit_matches &&
            !account_tuple_payload(
                left_batch.rows[left],
                "while accounting semi or anti row payload", left,
                &planned_peak_bytes)) {
          output_accounted = false;
          break;
        }
      }
    } else {
      for (const auto pair : accepted_pair_indices) {
        if (poll_cancellation("while accounting joined output", pair)) {
          return result;
        }
        if (!account_joined_pair(pair)) {
          output_accounted = false;
          break;
        }
      }
      if (output_accounted &&
          (request.join_kind == CanonicalAcceptedJoinKind::kLeftOuter ||
           request.join_kind == CanonicalAcceptedJoinKind::kFullOuter)) {
        for (std::size_t left = 0; left < left_count; ++left) {
          if (poll_cancellation("while accounting null-extended left output",
                                left)) {
            return result;
          }
          if (!matched_left_rows[left] &&
              !account_tuple_payload(
                  left_batch.rows[left],
                  "while accounting unmatched left payload", left,
                  &planned_peak_bytes)) {
            output_accounted = false;
            break;
          }
        }
      }
      if (output_accounted &&
          (request.join_kind == CanonicalAcceptedJoinKind::kRightOuter ||
           request.join_kind == CanonicalAcceptedJoinKind::kFullOuter)) {
        for (std::size_t right = 0; right < right_count; ++right) {
          if (poll_cancellation("while accounting null-extended right output",
                                right)) {
            return result;
          }
          if (!matched_right_rows[right] &&
              !account_tuple_payload(
                  right_batch.rows[right],
                  "while accounting unmatched right payload", right,
                  &planned_peak_bytes)) {
            output_accounted = false;
            break;
          }
        }
      }
    }
    if (!output_accounted) {
      if (!result.diagnostic.ok) return result;
      return refuse_resource("bound join output size overflowed");
    }
    if (planned_peak_bytes > *selected_node_memory_grant) {
      return refuse_resource(
          "bound join retained state and exact output exceed the "
          "selected-node memory grant");
    }
  }

  DescriptorBatch output;
  const auto left_width = left_batch.columns.size();
  const bool left_only =
      request.join_kind == CanonicalAcceptedJoinKind::kLeftSemi ||
      request.join_kind == CanonicalAcceptedJoinKind::kLeftAnti;
  if (poll_cancellation("before deriving join output descriptors")) {
    return result;
  }
  output.columns.reserve(
      left_batch.columns.size() +
      (left_only ? 0 : right_batch.columns.size()));
  for (std::size_t column = 0; column < left_batch.columns.size(); ++column) {
    if (poll_cancellation("while copying left join output descriptors",
                          column)) {
      return result;
    }
    output.columns.push_back(left_batch.columns[column]);
  }
  if (!left_only) {
    for (std::size_t column = 0; column < right_batch.columns.size();
         ++column) {
      if (poll_cancellation("while copying right join output descriptors",
                            column)) {
        return result;
      }
      output.columns.push_back(right_batch.columns[column]);
    }
  }
  std::vector<bool> derived_nullable_columns(output.columns.size(), false);
  if (request.join_kind == CanonicalAcceptedJoinKind::kRightOuter ||
      request.join_kind == CanonicalAcceptedJoinKind::kFullOuter) {
    for (std::size_t column = 0; column < left_width; ++column) {
      if (poll_cancellation("while marking nullable left descriptors",
                            column)) {
        return result;
      }
      output.columns[column].nullable = true;
      derived_nullable_columns[column] = true;
    }
  }
  if (request.join_kind == CanonicalAcceptedJoinKind::kLeftOuter ||
      request.join_kind == CanonicalAcceptedJoinKind::kFullOuter) {
    for (std::size_t column = left_width; column < output.columns.size();
         ++column) {
      if (poll_cancellation("while marking nullable right descriptors",
                            column)) {
        return result;
      }
      output.columns[column].nullable = true;
      derived_nullable_columns[column] = true;
    }
  }
  for (std::size_t column = 0; column < output.columns.size(); ++column) {
    if (poll_cancellation("while deriving nullable join descriptors", column)) {
      return result;
    }
    if (derived_nullable_columns[column] &&
        !DeriveCanonicalNullableDescriptorEncoding(
            &output.columns[column].descriptor)) {
      return refuse("outer join result descriptor lacks an exact nullability "
                    "carrier");
    }
  }
  output.rows.reserve(expected_output_rows);

  const auto append_joined = [&](const std::size_t left,
                                 const std::size_t right) {
    DescriptorTuple joined;
    joined.values.reserve(left_batch.rows[left].values.size() +
                          right_batch.rows[right].values.size());
    for (std::size_t column = 0;
         column < left_batch.rows[left].values.size(); ++column) {
      if (poll_cancellation("while copying joined left values", column)) {
        return false;
      }
      joined.values.push_back(left_batch.rows[left].values[column]);
    }
    for (std::size_t column = 0;
         column < right_batch.rows[right].values.size(); ++column) {
      if (poll_cancellation("while copying joined right values", column)) {
        return false;
      }
      joined.values.push_back(right_batch.rows[right].values[column]);
    }
    output.rows.push_back(std::move(joined));
    return true;
  };
  const auto append_unmatched_left = [&](const std::size_t left) {
    DescriptorTuple unmatched;
    unmatched.values.reserve(left_batch.rows[left].values.size() +
                             right_batch.columns.size());
    for (std::size_t column = 0;
         column < left_batch.rows[left].values.size(); ++column) {
      if (poll_cancellation("while copying unmatched left values", column)) {
        return false;
      }
      unmatched.values.push_back(left_batch.rows[left].values[column]);
    }
    for (std::size_t column_index = 0;
         column_index < right_batch.columns.size();
         ++column_index) {
      if (poll_cancellation("while null-extending right columns",
                            column_index)) {
        return false;
      }
      const auto& column = right_batch.columns[column_index];
      api::EngineTypedValue null_value;
      null_value.descriptor = column.descriptor;
      null_value.is_null = true;
      null_value.state = api::EngineValueState::sql_null;
      unmatched.values.push_back(std::move(null_value));
    }
    output.rows.push_back(std::move(unmatched));
    return true;
  };
  const auto append_unmatched_right = [&](const std::size_t right) {
    DescriptorTuple unmatched;
    unmatched.values.reserve(left_batch.columns.size() +
                             right_batch.rows[right].values.size());
    for (std::size_t column_index = 0;
         column_index < left_batch.columns.size();
         ++column_index) {
      if (poll_cancellation("while null-extending left columns",
                            column_index)) {
        return false;
      }
      const auto& column = left_batch.columns[column_index];
      api::EngineTypedValue null_value;
      null_value.descriptor = column.descriptor;
      null_value.is_null = true;
      null_value.state = api::EngineValueState::sql_null;
      unmatched.values.push_back(std::move(null_value));
    }
    for (std::size_t column = 0;
         column < right_batch.rows[right].values.size(); ++column) {
      if (poll_cancellation("while copying unmatched right values", column)) {
        return false;
      }
      unmatched.values.push_back(right_batch.rows[right].values[column]);
    }
    output.rows.push_back(std::move(unmatched));
    return true;
  };

  if (request.join_kind == CanonicalAcceptedJoinKind::kLeftSemi ||
      request.join_kind == CanonicalAcceptedJoinKind::kLeftAnti) {
    const bool emit_matches =
        request.join_kind == CanonicalAcceptedJoinKind::kLeftSemi;
    for (std::size_t left = 0; left < left_count; ++left) {
      if (poll_cancellation("while materializing semi or anti rows", left)) {
        return result;
      }
      if (matched_left_rows[left] == emit_matches) {
        DescriptorTuple emitted;
        emitted.values.reserve(left_batch.rows[left].values.size());
        for (std::size_t column = 0;
             column < left_batch.rows[left].values.size(); ++column) {
          if (poll_cancellation("while copying semi or anti values",
                                column)) {
            return result;
          }
          emitted.values.push_back(left_batch.rows[left].values[column]);
        }
        output.rows.push_back(std::move(emitted));
      }
    }
  } else if (request.join_kind == CanonicalAcceptedJoinKind::kRightOuter) {
    for (std::size_t right = 0; right < right_count; ++right) {
      if (poll_cancellation("while materializing right outer rows", right)) {
        return result;
      }
      bool emitted = false;
      for (const auto pair : accepted_pair_indices) {
        if (poll_cancellation("while scanning right outer pairs", pair)) {
          return result;
        }
        if (pair % right_count == right) {
          if (!append_joined(pair / right_count, right)) return result;
          emitted = true;
        }
      }
      if (!emitted && !append_unmatched_right(right)) return result;
    }
  } else if (request.join_kind == CanonicalAcceptedJoinKind::kLeftOuter ||
             request.join_kind == CanonicalAcceptedJoinKind::kFullOuter) {
    std::size_t accepted = 0;
    for (std::size_t left = 0; left < left_count; ++left) {
      if (poll_cancellation("while materializing left outer rows", left)) {
        return result;
      }
      bool emitted = false;
      while (accepted < accepted_pair_indices.size() &&
             accepted_pair_indices[accepted] / right_count == left) {
        if (poll_cancellation("while scanning left outer pairs",
                              accepted_pair_indices[accepted])) {
          return result;
        }
        if (!append_joined(
                left, accepted_pair_indices[accepted] % right_count)) {
          return result;
        }
        ++accepted;
        emitted = true;
      }
      if (!emitted && !append_unmatched_left(left)) return result;
    }
    if (accepted != accepted_pair_indices.size()) {
      return refuse("residual output did not map to its left input");
    }
    if (request.join_kind == CanonicalAcceptedJoinKind::kFullOuter) {
      for (std::size_t right = 0; right < right_count; ++right) {
        if (poll_cancellation("while materializing full outer rows", right)) {
          return result;
        }
        if (!matched_right_rows[right] && !append_unmatched_right(right)) {
          return result;
        }
      }
    }
  } else {
    for (const auto pair : accepted_pair_indices) {
      if (poll_cancellation("while materializing joined pairs", pair)) {
        return result;
      }
      if (!append_joined(pair / right_count, pair % right_count)) return result;
    }
  }

  for (std::size_t row_index = 0; row_index < output.rows.size(); ++row_index) {
    if (poll_cancellation("while rebinding nullable output values", row_index)) {
      return result;
    }
    auto& row = output.rows[row_index];
    for (std::size_t column = 0; column < output.columns.size(); ++column) {
      if (poll_cancellation("while rebinding nullable output cells",
                            row_index)) {
        return result;
      }
      if (derived_nullable_columns[column]) {
        row.values[column].descriptor = output.columns[column].descriptor;
      }
    }
  }

  std::vector<std::uint32_t> output_descriptor_ids;
  output_descriptor_ids.reserve(output.columns.size());
  for (std::size_t column = 0; column < output.columns.size(); ++column) {
    if (poll_cancellation("while binding output descriptor ids", column)) {
      return result;
    }
    output_descriptor_ids.push_back(output.columns[column].descriptor_id);
  }
  if (poll_cancellation("before join-kind output validation")) return result;
  bool validation_cancelled = false;
  auto validation = ValidateCanonicalDescriptorBatch(
      output, output_descriptor_ids, cancellation_requested,
      &validation_cancelled);
  if (!validation.ok &&
      adopt_validation_cancellation(validation, validation_cancelled)) {
    return result;
  }
  if (!validation.ok) {
    return refuse(validation.diagnostic_code + ":" + validation.detail);
  }
  if (poll_cancellation("after join-kind output validation")) return result;
  const auto result_authority = RevalidateCanonicalExecutionMgaAuthority(
      key_request.mga_authority, key_request.physical_dag);
  if (!result_authority.ok)
    return refuse(result_authority.diagnostic_code + ":" +
                  result_authority.detail);
  if (poll_cancellation("before join-kind publication")) return result;

  result.diagnostic = {};
  result.output_batch = std::move(output);
  result.matched_pair_count = accepted_pair_indices.size();
  result.unmatched_left_row_count = unmatched_left_row_count;
  result.unmatched_right_row_count = unmatched_right_row_count;
  if (request.join_kind == CanonicalAcceptedJoinKind::kLeftSemi) {
    result.emitted_left_row_count = matched_left_row_count;
  } else if (request.join_kind == CanonicalAcceptedJoinKind::kLeftAnti) {
    result.emitted_left_row_count = unmatched_left_row_count;
  } else if (request.join_kind == CanonicalAcceptedJoinKind::kLeftOuter ||
             request.join_kind == CanonicalAcceptedJoinKind::kFullOuter) {
    result.emitted_left_row_count = left_count;
  } else {
    result.emitted_left_row_count = matched_left_row_count;
  }
  result.selected_plan_uuid = std::move(selected_plan_uuid);
  result.executed_physical_node_id = executed_physical_node_id;
  result.causal_counter_id = causal_counter_id;
  result.mga_statement_context = key_request.mga_authority.statement_context;
  return result;
}

// QOW-SOURCE-QRY-012-NAMED-V1
// QOW-SOURCE-QRY-012-NAMED-V2
// Consume binder-owned USING/NATURAL column bindings, lower nonempty bindings
// to the canonical composite-key route and zero-common NATURAL to an explicit
// conditionless join-kind route, then execute the bound named projection.
CanonicalNamedJoinResult ExecuteCanonicalNamedJoin(
    const CanonicalNamedJoinRequest& request) {
  namespace api = scratchbird::engine::internal_api;

  CanonicalNamedJoinResult result;
  const auto refuse = [&](std::string detail) {
    result.diagnostic.ok = false;
    result.diagnostic.diagnostic_code =
        "QOW-DIAG-QRY-012-NAMED-REFUSAL-V1";
    result.diagnostic.detail = std::move(detail);
    result.output_batch = {};
    result.form = CanonicalNamedJoinForm::kUsing;
    result.binding_count = 0;
    result.matched_pair_count = 0;
    result.unmatched_left_row_count = 0;
    result.unmatched_right_row_count = 0;
    result.binding_evidence_uuid.clear();
    result.selected_plan_uuid.clear();
    result.executed_join_node_id = 0;
    result.join_causal_counter_id = 0;
    result.executed_projection_node_id = 0;
    result.projection_causal_counter_id = 0;
    return result;
  };
  const auto join_authority = RevalidateCanonicalExecutionMgaAuthority(
      request.key_request.mga_authority,
      request.key_request.physical_dag);
  if (!join_authority.ok)
    return refuse(join_authority.diagnostic_code + ":" +
                  join_authority.detail);

  if (request.form != CanonicalNamedJoinForm::kUsing &&
      request.form != CanonicalNamedJoinForm::kNatural) {
    return refuse("named join form is outside the accepted profile");
  }
  if (request.join_kind != CanonicalAcceptedJoinKind::kInner &&
      request.join_kind != CanonicalAcceptedJoinKind::kLeftOuter &&
      request.join_kind != CanonicalAcceptedJoinKind::kRightOuter &&
      request.join_kind != CanonicalAcceptedJoinKind::kFullOuter) {
    return refuse("named join kind is outside the accepted profile");
  }
  const bool conditionless_natural =
      request.form == CanonicalNamedJoinForm::kNatural &&
      request.bindings.empty();
  if (!IsCanonicalUuid(request.binding_evidence_uuid) ||
      request.maximum_binding_count == 0 ||
      (request.form == CanonicalNamedJoinForm::kUsing &&
       request.bindings.empty()) ||
      request.bindings.size() > request.maximum_binding_count ||
      request.maximum_candidate_rechecks == 0 ||
      request.maximum_output_rows == 0 ||
      request.key_request.key_terms.size() != request.bindings.size()) {
    return refuse("named join binding or resource contract is invalid");
  }

  const auto seed_equal = [](const auto& left, const auto& right) {
    return left.active == right.active &&
           left.seed_pack_name == right.seed_pack_name &&
           left.seed_pack_version == right.seed_pack_version &&
           left.charset_name == right.charset_name &&
           left.collation_name == right.collation_name &&
           left.collation_case_insensitive ==
               right.collation_case_insensitive &&
           left.collation_accent_insensitive ==
               right.collation_accent_insensitive;
  };
  const auto timezone_seed_equal = [](const auto& left, const auto& right) {
    return left.active == right.active &&
           left.seed_pack_name == right.seed_pack_name &&
           left.seed_pack_version == right.seed_pack_version &&
           left.content_hash == right.content_hash &&
           left.timezone_records == right.timezone_records &&
           left.timezone_transition_records ==
               right.timezone_transition_records &&
           left.timezone_leap_second_records ==
               right.timezone_leap_second_records &&
           left.timezone_names == right.timezone_names;
  };
  const auto term_equal = [&](const auto& left, const auto& right) {
    return left.left_column == right.left_column &&
           left.left_expression_descriptor_id ==
               right.left_expression_descriptor_id &&
           left.right_column == right.right_column &&
           left.right_expression_descriptor_id ==
               right.right_expression_descriptor_id &&
           left.collation_uuid == right.collation_uuid &&
           left.resource_epoch == right.resource_epoch &&
           left.collation_epoch == right.collation_epoch &&
           seed_equal(left.text_seed, right.text_seed) &&
           left.timezone_epoch == right.timezone_epoch &&
           timezone_seed_equal(left.timezone_seed, right.timezone_seed);
  };

  const auto& left_columns = request.key_request.left_batch.columns;
  const auto& right_columns = request.key_request.right_batch.columns;
  std::map<std::string, std::vector<std::size_t>> left_names;
  std::map<std::string, std::vector<std::size_t>> right_names;
  for (std::size_t column = 0; column < left_columns.size(); ++column) {
    if (left_columns[column].stable_name.empty()) {
      return refuse("named join left column lacks a bound stable name");
    }
    left_names[left_columns[column].stable_name].push_back(column);
  }
  for (std::size_t column = 0; column < right_columns.size(); ++column) {
    if (right_columns[column].stable_name.empty()) {
      return refuse("named join right column lacks a bound stable name");
    }
    right_names[right_columns[column].stable_name].push_back(column);
  }

  std::vector<bool> bound_left(left_columns.size(), false);
  std::vector<bool> bound_right(right_columns.size(), false);
  std::set<std::string> bound_names;
  std::set<std::uint32_t> result_descriptor_ids;
  for (std::size_t index = 0; index < request.bindings.size(); ++index) {
    const auto& binding = request.bindings[index];
    const auto& term = binding.key_term;
    if (binding.normalized_name.empty() ||
        !bound_names.insert(binding.normalized_name).second ||
        term.left_column >= left_columns.size() ||
        term.right_column >= right_columns.size() ||
        bound_left[term.left_column] || bound_right[term.right_column] ||
        !term_equal(term, request.key_request.key_terms[index])) {
      return refuse("named join binding identity is duplicated or drifted");
    }
    const auto& left_column = left_columns[term.left_column];
    const auto& right_column = right_columns[term.right_column];
    if (left_names[binding.normalized_name].size() != 1 ||
        right_names[binding.normalized_name].size() != 1 ||
        left_names[binding.normalized_name].front() != term.left_column ||
        right_names[binding.normalized_name].front() != term.right_column ||
        binding.result_column.stable_name != binding.normalized_name ||
        binding.result_column.descriptor_id == 0 ||
        !result_descriptor_ids
             .insert(binding.result_column.descriptor_id)
             .second ||
        binding.result_column.descriptor.canonical_type_name !=
            left_column.descriptor.canonical_type_name ||
        binding.result_column.descriptor.canonical_type_name !=
            right_column.descriptor.canonical_type_name) {
      return refuse("named join binding is not exact for both input schemas");
    }
    bound_left[term.left_column] = true;
    bound_right[term.right_column] = true;
  }

  if (request.form == CanonicalNamedJoinForm::kNatural) {
    std::vector<std::pair<std::string, std::pair<std::size_t, std::size_t>>>
        natural_bindings;
    for (std::size_t left = 0; left < left_columns.size(); ++left) {
      const auto& name = left_columns[left].stable_name;
      if (left_names[name].size() != 1) {
        return refuse("NATURAL join left names are ambiguous");
      }
      const auto right = right_names.find(name);
      if (right == right_names.end()) continue;
      if (right->second.size() != 1) {
        return refuse("NATURAL join right names are ambiguous");
      }
      natural_bindings.push_back({name, {left, right->second.front()}});
    }
    if (natural_bindings.size() != request.bindings.size()) {
      return refuse("NATURAL join binding set omits or invents common names");
    }
    for (std::size_t index = 0; index < natural_bindings.size(); ++index) {
      const auto& binding = request.bindings[index];
      if (binding.normalized_name != natural_bindings[index].first ||
          binding.key_term.left_column != natural_bindings[index].second.first ||
          binding.key_term.right_column !=
              natural_bindings[index].second.second) {
        return refuse("NATURAL join bindings are not in left schema order");
      }
    }
  }

  std::vector<std::uint32_t> expected_projection_ids;
  expected_projection_ids.reserve(request.bindings.size() +
                                  left_columns.size() + right_columns.size());
  for (const auto& binding : request.bindings) {
    expected_projection_ids.push_back(binding.result_column.descriptor_id);
  }
  for (std::size_t column = 0; column < left_columns.size(); ++column) {
    if (!bound_left[column]) {
      if (!result_descriptor_ids.insert(left_columns[column].descriptor_id)
               .second) {
        return refuse("named join output descriptor identity is duplicated");
      }
      expected_projection_ids.push_back(left_columns[column].descriptor_id);
    }
  }
  for (std::size_t column = 0; column < right_columns.size(); ++column) {
    if (!bound_right[column]) {
      if (!result_descriptor_ids.insert(right_columns[column].descriptor_id)
               .second) {
        return refuse("named join output descriptor identity is duplicated");
      }
      expected_projection_ids.push_back(right_columns[column].descriptor_id);
    }
  }

  const auto projection_validation = RevalidateCanonicalExecutionMgaAuthority(
      request.key_request.mga_authority, request.projection_dag);
  if (!projection_validation.ok)
    return refuse(projection_validation.diagnostic_code + ":" +
                  projection_validation.detail);
  if (request.projection_dag.selected_plan_uuid !=
          request.key_request.physical_dag.selected_plan_uuid ||
      request.projection_dag.local_transaction_id !=
          request.key_request.physical_dag.local_transaction_id ||
      request.projection_dag.statement_snapshot_id !=
          request.key_request.physical_dag.statement_snapshot_id ||
      request.selected_projection_node_id == 0 ||
      request.selected_projection_node_id !=
          request.projection_dag.root_physical_node_id) {
    return refuse("named join projection does not share the admitted plan");
  }
  const PhysicalNodeRecord* projection_node = nullptr;
  const PhysicalNodeRecord* projection_join_node = nullptr;
  const PhysicalNodeRecord* key_join_node = nullptr;
  for (const auto& node : request.key_request.physical_dag.nodes) {
    if (node.physical_node_id ==
        request.key_request.selected_physical_node_id) {
      key_join_node = &node;
      break;
    }
  }
  for (const auto& node : request.projection_dag.nodes) {
    if (node.physical_node_id == request.selected_projection_node_id) {
      projection_node = &node;
    }
    if (node.physical_node_id ==
        request.key_request.selected_physical_node_id) {
      projection_join_node = &node;
    }
  }
  const auto expected_implementation =
      request.form == CanonicalNamedJoinForm::kUsing
          ? std::string_view("join.using-projection.v1")
          : std::string_view("join.natural-projection.v1");
  if (projection_node == nullptr || projection_join_node == nullptr ||
      key_join_node == nullptr ||
      projection_node->node_kind != PhysicalNodeKind::kProject ||
      projection_node->implementation_id != expected_implementation ||
      projection_node->input_physical_node_ids.size() != 1 ||
      projection_node->input_physical_node_ids.front() !=
          projection_join_node->physical_node_id ||
      projection_join_node->node_kind != PhysicalNodeKind::kJoin ||
      projection_join_node->implementation_id !=
          key_join_node->implementation_id ||
      projection_join_node->output_descriptor_ids !=
          key_join_node->output_descriptor_ids ||
      projection_join_node->causal_counter_id !=
          key_join_node->causal_counter_id ||
      projection_node->output_descriptor_ids != expected_projection_ids ||
      projection_node->causal_counter_id <=
          projection_join_node->causal_counter_id) {
    return refuse("named join projection node identity or schema is invalid");
  }
  if (conditionless_natural &&
      key_join_node->implementation_id !=
          "join.natural-conditionless.typed.v1") {
    return refuse("zero-common NATURAL physical identity is not bound");
  }

  const auto left_count = request.key_request.left_batch.rows.size();
  const auto right_count = request.key_request.right_batch.rows.size();
  if (left_count != 0 &&
      right_count > std::numeric_limits<std::size_t>::max() / left_count) {
    return refuse("named join pair cardinality overflowed");
  }
  CanonicalJoinKindRequest kind_request;
  kind_request.residual_request.key_request = request.key_request;
  if (!conditionless_natural) {
    kind_request.residual_request.residual_truth_values.assign(
        left_count * right_count, api::EngineSqlTruthValue::true_value);
  }
  kind_request.residual_request.maximum_candidate_rechecks =
      request.maximum_candidate_rechecks;
  kind_request.join_kind = request.join_kind;
  kind_request.conditionless_predicate = conditionless_natural;
  kind_request.maximum_output_rows = request.maximum_output_rows;
  auto joined = ExecuteCanonicalJoinKind(kind_request);
  if (!joined.diagnostic.ok) {
    return refuse(joined.diagnostic.diagnostic_code + ":" +
                  joined.diagnostic.detail);
  }
  if (!PhysicalMgaStatementContextEqual(
          joined.mga_statement_context,
          request.key_request.mga_authority.statement_context)) {
    return refuse("named join route returned a different MGA statement context");
  }
  if (joined.executed_physical_node_id !=
          projection_join_node->physical_node_id ||
      joined.causal_counter_id != projection_join_node->causal_counter_id) {
    return refuse("named join execution drifted from its projection input");
  }

  DescriptorBatch output;
  for (const auto& binding : request.bindings) {
    output.columns.push_back(binding.result_column);
  }
  const auto left_width = left_columns.size();
  for (std::size_t column = 0; column < left_width; ++column) {
    if (!bound_left[column]) output.columns.push_back(joined.output_batch.columns[column]);
  }
  for (std::size_t column = 0; column < right_columns.size(); ++column) {
    if (!bound_right[column]) {
      output.columns.push_back(joined.output_batch.columns[left_width + column]);
    }
  }
  output.rows.reserve(joined.output_batch.rows.size());
  for (const auto& joined_row : joined.output_batch.rows) {
    if (joined_row.values.size() != left_width + right_columns.size()) {
      return refuse("named join input row width is invalid");
    }
    DescriptorTuple projected;
    projected.values.reserve(output.columns.size());
    for (const auto& binding : request.bindings) {
      const auto& left = joined_row.values[binding.key_term.left_column];
      const auto& right = joined_row.values[
          left_width + binding.key_term.right_column];
      api::EngineTypedValue value;
      if (left.state != api::EngineValueState::sql_null) {
        value = left;
      } else if (right.state != api::EngineValueState::sql_null) {
        value = right;
      } else {
        value.is_null = true;
        value.state = api::EngineValueState::sql_null;
      }
      value.descriptor = binding.result_column.descriptor;
      if (value.state == api::EngineValueState::sql_null) {
        value.is_null = true;
        value.encoded_value.clear();
        value.binary_value.clear();
      }
      projected.values.push_back(std::move(value));
    }
    for (std::size_t column = 0; column < left_width; ++column) {
      if (!bound_left[column]) projected.values.push_back(joined_row.values[column]);
    }
    for (std::size_t column = 0; column < right_columns.size(); ++column) {
      if (!bound_right[column]) {
        projected.values.push_back(joined_row.values[left_width + column]);
      }
    }
    output.rows.push_back(std::move(projected));
  }

  const auto output_validation =
      ValidateCanonicalDescriptorBatch(output, expected_projection_ids);
  if (!output_validation.ok) {
    return refuse(output_validation.diagnostic_code + ":" +
                  output_validation.detail);
  }
  const auto result_authority = RevalidateCanonicalExecutionMgaAuthority(
      request.key_request.mga_authority, request.projection_dag);
  if (!result_authority.ok)
    return refuse(result_authority.diagnostic_code + ":" +
                  result_authority.detail);
  result.diagnostic = {};
  result.output_batch = std::move(output);
  result.form = request.form;
  result.binding_count = request.bindings.size();
  result.matched_pair_count = joined.matched_pair_count;
  result.unmatched_left_row_count = joined.unmatched_left_row_count;
  result.unmatched_right_row_count = joined.unmatched_right_row_count;
  result.binding_evidence_uuid = request.binding_evidence_uuid;
  result.selected_plan_uuid = request.projection_dag.selected_plan_uuid;
  result.executed_join_node_id = joined.executed_physical_node_id;
  result.join_causal_counter_id = joined.causal_counter_id;
  result.executed_projection_node_id = projection_node->physical_node_id;
  result.projection_causal_counter_id = projection_node->causal_counter_id;
  result.mga_statement_context =
      request.key_request.mga_authority.statement_context;
  return result;
}

// QOW-SOURCE-QRY-012-STRATEGY-V1
// Execute admitted nested-loop, hash, or merge strategies and prove the
// resulting physical-pair multiset equals the canonical key/residual route.
// Non-inner output is published only through the canonical join-kind
// materializer after that proof. Candidate work, retained state, and output
// remain independently bounded.
CanonicalJoinStrategyResult ExecuteCanonicalJoinStrategy(
    const CanonicalJoinStrategyRequest& request) {
  namespace api = scratchbird::engine::internal_api;

  CanonicalJoinStrategyResult result;
  const auto refuse = [&](std::string detail) {
    result.diagnostic.ok = false;
    result.diagnostic.diagnostic_code =
        "QOW-DIAG-QRY-012-STRATEGY-REFUSAL-V1";
    result.diagnostic.detail = std::move(detail);
    result.output_batch = {};
    result.canonical_pair_indices.clear();
    result.strategy_pair_indices.clear();
    result.hash_entry_count = 0;
    result.retained_entry_count = 0;
    result.candidate_probe_count = 0;
    result.strategy_key_comparison_count = 0;
    result.canonical_multiset_proven = false;
    result.canonical_output_proven = false;
    result.join_kind = CanonicalAcceptedJoinKind::kInner;
    result.matched_pair_count = 0;
    result.unmatched_left_row_count = 0;
    result.unmatched_right_row_count = 0;
    result.emitted_left_row_count = 0;
    result.strategy_id.clear();
    result.selected_plan_uuid.clear();
    result.executed_physical_node_id = 0;
    result.causal_counter_id = 0;
    return result;
  };

  const auto& key_authority_request = request.residual_request.key_request;
  const auto entry_authority = RevalidateCanonicalExecutionMgaAuthority(
      key_authority_request.mga_authority,
      key_authority_request.physical_dag);
  if (!entry_authority.ok)
    return refuse(entry_authority.diagnostic_code + ":" +
                  entry_authority.detail);

  std::string_view join_kind_id;
  switch (request.join_kind) {
    case CanonicalAcceptedJoinKind::kInner:
      join_kind_id = "inner";
      break;
    case CanonicalAcceptedJoinKind::kLeftOuter:
      join_kind_id = "left-outer";
      break;
    case CanonicalAcceptedJoinKind::kRightOuter:
      join_kind_id = "right-outer";
      break;
    case CanonicalAcceptedJoinKind::kFullOuter:
      join_kind_id = "full-outer";
      break;
    case CanonicalAcceptedJoinKind::kLeftSemi:
      join_kind_id = "left-semi";
      break;
    case CanonicalAcceptedJoinKind::kLeftAnti:
      join_kind_id = "left-anti";
      break;
    case CanonicalAcceptedJoinKind::kCross:
      return refuse("cross join does not admit a keyed physical strategy");
    default:
      return refuse("join kind is outside the accepted strategy profile");
  }

  std::string strategy_id;
  switch (request.strategy) {
    case CanonicalJoinStrategyKind::kNestedLoopInner:
      strategy_id = "join.nested-loop-" + std::string(join_kind_id) + ".v1";
      break;
    case CanonicalJoinStrategyKind::kHashInnerInt64Equality:
      strategy_id = "join.hash-" + std::string(join_kind_id) +
                    ".int64-equality.v1";
      break;
    case CanonicalJoinStrategyKind::kMergeInnerInt64Equality:
      strategy_id = "join.merge-" + std::string(join_kind_id) +
                    ".int64-equality.v1";
      break;
    case CanonicalJoinStrategyKind::kHashTypedCompositeEquality:
      strategy_id = "join.hash-" + std::string(join_kind_id) +
                    ".typed-composite-equality.v1";
      break;
    case CanonicalJoinStrategyKind::kMergeTypedCompositeEquality:
      strategy_id = "join.merge-" + std::string(join_kind_id) +
                    ".typed-composite-equality.v1";
      break;
    default:
      return refuse("join strategy is outside the accepted canonical profile");
  }
  if (request.maximum_candidate_probes == 0 ||
      request.maximum_output_rows == 0) {
    return refuse("join strategy resource contract is invalid");
  }
  const auto& key_request = request.residual_request.key_request;
  const bool legacy_int64_strategy =
      request.strategy ==
          CanonicalJoinStrategyKind::kHashInnerInt64Equality ||
      request.strategy ==
          CanonicalJoinStrategyKind::kMergeInnerInt64Equality;
  const bool hash_strategy =
      request.strategy ==
          CanonicalJoinStrategyKind::kHashInnerInt64Equality ||
      request.strategy ==
          CanonicalJoinStrategyKind::kHashTypedCompositeEquality;
  const bool merge_strategy =
      request.strategy ==
          CanonicalJoinStrategyKind::kMergeInnerInt64Equality ||
      request.strategy ==
          CanonicalJoinStrategyKind::kMergeTypedCompositeEquality;
  if (key_request.key_terms.empty() ||
      (legacy_int64_strategy && key_request.key_terms.size() != 1)) {
    return refuse("selected join strategy has incompatible key arity");
  }
  if (hash_strategy &&
      (request.maximum_hash_entries == 0 ||
       request.maximum_retained_entries == 0)) {
    return refuse("hash strategy entry resource contract is invalid");
  }
  if (merge_strategy &&
      request.maximum_retained_entries == 0) {
    return refuse("merge strategy retained-state contract is invalid");
  }
  if ((request.strategy ==
           CanonicalJoinStrategyKind::kHashTypedCompositeEquality ||
       request.strategy ==
           CanonicalJoinStrategyKind::kMergeTypedCompositeEquality) &&
      request.maximum_strategy_key_comparisons == 0) {
    return refuse("typed strategy comparison resource contract is invalid");
  }
  const PhysicalNodeRecord* selected_node = nullptr;
  for (const auto& node : key_request.physical_dag.nodes) {
    if (node.physical_node_id == key_request.selected_physical_node_id) {
      selected_node = &node;
      break;
    }
  }
  if (selected_node == nullptr ||
      selected_node->implementation_id != strategy_id) {
    return refuse("selected physical node does not name the forced strategy");
  }

  auto canonical =
      ExecuteCanonicalJoinResidual(request.residual_request);
  if (!canonical.diagnostic.ok) {
    return refuse(canonical.diagnostic.diagnostic_code + ":" +
                  canonical.diagnostic.detail);
  }
  if (!PhysicalMgaStatementContextEqual(
          canonical.mga_statement_context,
          key_request.mga_authority.statement_context)) {
    return refuse("join strategy canonical route returned a different MGA statement context");
  }
  if (legacy_int64_strategy) {
    const auto& term = key_request.key_terms.front();
    if (key_request.left_batch.columns[term.left_column]
                .descriptor.canonical_type_name != "int64" ||
        key_request.right_batch.columns[term.right_column]
                .descriptor.canonical_type_name != "int64") {
      return refuse(
          "hash and merge strategies require one compatible int64 key");
    }
  }

  const auto finish = [&](std::vector<std::size_t> strategy_pair_indices,
                          const std::size_t retained_entry_count,
                          const std::size_t candidate_probe_count) {
    auto canonical_multiset = canonical.accepted_pair_indices;
    std::sort(canonical_multiset.begin(), canonical_multiset.end());
    std::sort(strategy_pair_indices.begin(), strategy_pair_indices.end());
    if (canonical_multiset != strategy_pair_indices) {
      return refuse("join strategy output differs from canonical pair multiset");
    }
    if (strategy_pair_indices.size() > request.maximum_output_rows) {
      return refuse("join strategy output row bound was exceeded");
    }

    CanonicalJoinKindRequest kind_request;
    kind_request.residual_request = request.residual_request;
    kind_request.join_kind = request.join_kind;
    kind_request.maximum_output_rows = request.maximum_output_rows;
    auto canonical_output = ExecuteCanonicalJoinKind(kind_request);
    if (!canonical_output.diagnostic.ok) {
      return refuse(canonical_output.diagnostic.diagnostic_code + ":" +
                    canonical_output.diagnostic.detail);
    }
    if (!PhysicalMgaStatementContextEqual(
            canonical_output.mga_statement_context,
            key_request.mga_authority.statement_context)) {
      return refuse("join strategy output route returned a different MGA statement context");
    }
    if (canonical_output.matched_pair_count != strategy_pair_indices.size() ||
        canonical_output.selected_plan_uuid != canonical.selected_plan_uuid ||
        canonical_output.executed_physical_node_id !=
            canonical.executed_physical_node_id ||
        canonical_output.causal_counter_id != canonical.causal_counter_id) {
      return refuse("canonical join-kind output identity drifted from strategy");
    }
    const auto result_authority = RevalidateCanonicalExecutionMgaAuthority(
        key_request.mga_authority, key_request.physical_dag);
    if (!result_authority.ok)
      return refuse(result_authority.diagnostic_code + ":" +
                    result_authority.detail);

    result.diagnostic = {};
    result.output_batch = std::move(canonical_output.output_batch);
    result.canonical_pair_indices = canonical_multiset;
    result.strategy_pair_indices = std::move(strategy_pair_indices);
    result.hash_entry_count = hash_strategy ? retained_entry_count : 0;
    result.retained_entry_count = retained_entry_count;
    result.candidate_probe_count = candidate_probe_count;
    result.canonical_multiset_proven = true;
    result.canonical_output_proven = true;
    result.join_kind = request.join_kind;
    result.matched_pair_count = canonical_output.matched_pair_count;
    result.unmatched_left_row_count =
        canonical_output.unmatched_left_row_count;
    result.unmatched_right_row_count =
        canonical_output.unmatched_right_row_count;
    result.emitted_left_row_count = canonical_output.emitted_left_row_count;
    result.strategy_id = strategy_id;
    result.selected_plan_uuid = canonical.selected_plan_uuid;
    result.executed_physical_node_id = canonical.executed_physical_node_id;
    result.causal_counter_id = canonical.causal_counter_id;
    result.mga_statement_context = key_request.mga_authority.statement_context;
    return result;
  };

  if (request.strategy == CanonicalJoinStrategyKind::kNestedLoopInner) {
    std::vector<std::size_t> strategy_pairs;
    std::size_t candidate_probe_count = 0;
    const auto right_count = key_request.right_batch.rows.size();
    for (std::size_t left = 0; left < key_request.left_batch.rows.size();
         ++left) {
      for (std::size_t right = 0; right < right_count; ++right) {
        bool matches = true;
        for (const auto& term : key_request.key_terms) {
          const auto& left_value =
              key_request.left_batch.rows[left].values[term.left_column];
          const auto& right_value =
              key_request.right_batch.rows[right].values[term.right_column];
          if (left_value.state == api::EngineValueState::sql_null ||
              right_value.state == api::EngineValueState::sql_null) {
            matches = false;
            break;
          }
          bool equal = false;
          std::string detail;
          if (!CompareCanonicalJoinKeyValues(left_value, right_value, term,
                                             &equal, &detail)) {
            return refuse("nested-loop strategy key is invalid:" + detail);
          }
          if (!equal) {
            matches = false;
            break;
          }
        }
        if (!matches) continue;
        if (candidate_probe_count == request.maximum_candidate_probes) {
          return refuse("nested-loop candidate probe bound was exceeded");
        }
        ++candidate_probe_count;
        const auto pair = left * right_count + right;
        if (request.residual_request.residual_truth_values[pair] ==
            api::EngineSqlTruthValue::true_value) {
          if (strategy_pairs.size() == request.maximum_output_rows) {
            return refuse("nested-loop output row bound was exceeded");
          }
          strategy_pairs.push_back(pair);
        }
      }
    }
    if (candidate_probe_count != canonical.candidate_pair_count) {
      return refuse("nested-loop candidate set differs from canonical keys");
    }
    return finish(std::move(strategy_pairs), 0, candidate_probe_count);
  }

  // QOW-SOURCE-QRY-012-STRATEGY-TYPED-COMPOSITE-V1
  // Typed hash and merge share one bounded comparator matrix.  The hash route
  // hashes stable equivalence-class ordinals whose membership is established
  // only by the canonical typed comparator; it never hashes display bytes.
  if (request.strategy ==
          CanonicalJoinStrategyKind::kHashTypedCompositeEquality ||
      request.strategy ==
          CanonicalJoinStrategyKind::kMergeTypedCompositeEquality) {
    const auto row_has_null_key = [&](const DescriptorTuple& row,
                                      const bool left) {
      for (const auto& term : key_request.key_terms) {
        const auto column = left ? term.left_column : term.right_column;
        if (row.values[column].state == api::EngineValueState::sql_null) {
          return true;
        }
      }
      return false;
    };
    std::vector<std::size_t> left_key_rows;
    std::vector<std::size_t> right_key_rows;
    for (std::size_t row = 0; row < key_request.left_batch.rows.size();
         ++row) {
      if (!row_has_null_key(key_request.left_batch.rows[row], true)) {
        left_key_rows.push_back(row);
      }
    }
    for (std::size_t row = 0; row < key_request.right_batch.rows.size();
         ++row) {
      if (!row_has_null_key(key_request.right_batch.rows[row], false)) {
        right_key_rows.push_back(row);
      }
    }

    if (request.strategy ==
            CanonicalJoinStrategyKind::kMergeTypedCompositeEquality &&
        left_key_rows.size() > std::numeric_limits<std::size_t>::max() -
                                   right_key_rows.size()) {
      return refuse("typed strategy retained-state cardinality overflowed");
    }
    const auto retained_count =
        request.strategy ==
                CanonicalJoinStrategyKind::kHashTypedCompositeEquality
            ? right_key_rows.size()
            : left_key_rows.size() + right_key_rows.size();
    if (retained_count > request.maximum_retained_entries ||
        (request.strategy ==
             CanonicalJoinStrategyKind::kHashTypedCompositeEquality &&
         right_key_rows.size() > request.maximum_hash_entries)) {
      return refuse("typed strategy retained-state bound was exceeded");
    }

    const auto bounded_product = [](const std::size_t left,
                                    const std::size_t right,
                                    std::size_t* product) {
      if (product == nullptr ||
          (left != 0 &&
           right > std::numeric_limits<std::size_t>::max() / left)) {
        return false;
      }
      *product = left * right;
      return true;
    };
    std::size_t left_cells = 0;
    std::size_t right_cells = 0;
    std::size_t cross_cells = 0;
    if (!bounded_product(left_key_rows.size(), left_key_rows.size(),
                         &left_cells) ||
        !bounded_product(right_key_rows.size(), right_key_rows.size(),
                         &right_cells) ||
        !bounded_product(left_key_rows.size(), right_key_rows.size(),
                         &cross_cells) ||
        left_cells > request.maximum_strategy_key_comparisons ||
        right_cells >
            request.maximum_strategy_key_comparisons - left_cells ||
        cross_cells > request.maximum_strategy_key_comparisons - left_cells -
                          right_cells) {
      return refuse("typed strategy comparison matrix bound was exceeded");
    }

    std::size_t comparison_count = 0;
    const auto compare_rows = [&](const DescriptorBatch& left_batch,
                                  const std::size_t left_row,
                                  const bool left_uses_left_columns,
                                  const DescriptorBatch& right_batch,
                                  const std::size_t right_row,
                                  const bool right_uses_left_columns,
                                  int* comparison,
                                  std::string* detail) {
      if (comparison == nullptr || detail == nullptr) return false;
      *comparison = 0;
      detail->clear();
      for (const auto& term : key_request.key_terms) {
        if (comparison_count ==
            request.maximum_strategy_key_comparisons) {
          *detail = "typed strategy key comparison bound was exceeded";
          return false;
        }
        ++comparison_count;
        const auto left_column = left_uses_left_columns
                                     ? term.left_column
                                     : term.right_column;
        const auto right_column = right_uses_left_columns
                                      ? term.left_column
                                      : term.right_column;
        const auto compared = CompareCanonicalDescriptorOrderValues(
            left_batch.rows[left_row].values[left_column],
            right_batch.rows[right_row].values[right_column],
            JoinKeyOrderTerm(term, left_uses_left_columns));
        if (!compared.diagnostic.ok) {
          *detail = compared.diagnostic.diagnostic_code + ":" +
                    compared.diagnostic.detail;
          return false;
        }
        *comparison = compared.comparison;
        if (*comparison != 0) break;
      }
      return true;
    };

    std::vector<std::int8_t> left_comparisons(left_cells, 0);
    std::vector<std::int8_t> right_comparisons(right_cells, 0);
    std::vector<std::int8_t> cross_comparisons(cross_cells, 0);
    std::string comparison_detail;
    for (std::size_t left = 0; left < left_key_rows.size(); ++left) {
      for (std::size_t right = left + 1; right < left_key_rows.size();
           ++right) {
        int comparison = 0;
        if (!compare_rows(key_request.left_batch, left_key_rows[left], true,
                          key_request.left_batch, left_key_rows[right], true,
                          &comparison, &comparison_detail)) {
          return refuse("typed strategy left-key comparison is invalid:" +
                        comparison_detail);
        }
        left_comparisons[left * left_key_rows.size() + right] =
            static_cast<std::int8_t>(comparison);
        left_comparisons[right * left_key_rows.size() + left] =
            static_cast<std::int8_t>(-comparison);
      }
    }
    for (std::size_t left = 0; left < right_key_rows.size(); ++left) {
      for (std::size_t right = left + 1; right < right_key_rows.size();
           ++right) {
        int comparison = 0;
        if (!compare_rows(key_request.right_batch, right_key_rows[left], false,
                          key_request.right_batch, right_key_rows[right],
                          false, &comparison, &comparison_detail)) {
          return refuse("typed strategy right-key comparison is invalid:" +
                        comparison_detail);
        }
        right_comparisons[left * right_key_rows.size() + right] =
            static_cast<std::int8_t>(comparison);
        right_comparisons[right * right_key_rows.size() + left] =
            static_cast<std::int8_t>(-comparison);
      }
    }
    for (std::size_t left = 0; left < left_key_rows.size(); ++left) {
      for (std::size_t right = 0; right < right_key_rows.size(); ++right) {
        int comparison = 0;
        if (!compare_rows(key_request.left_batch, left_key_rows[left], true,
                          key_request.right_batch, right_key_rows[right],
                          false, &comparison, &comparison_detail)) {
          return refuse("typed strategy cross-key comparison is invalid:" +
                        comparison_detail);
        }
        cross_comparisons[left * right_key_rows.size() + right] =
            static_cast<std::int8_t>(comparison);
      }
    }

    std::vector<std::size_t> strategy_pairs;
    std::size_t candidate_probe_count = 0;
    const auto right_count = key_request.right_batch.rows.size();
    const auto retain_pair = [&](const std::size_t left_row,
                                 const std::size_t right_row) {
      if (candidate_probe_count == request.maximum_candidate_probes) {
        return false;
      }
      ++candidate_probe_count;
      const auto pair = left_row * right_count + right_row;
      if (request.residual_request.residual_truth_values[pair] ==
          api::EngineSqlTruthValue::true_value) {
        if (strategy_pairs.size() == request.maximum_output_rows) {
          return false;
        }
        strategy_pairs.push_back(pair);
      }
      return true;
    };

    if (request.strategy ==
        CanonicalJoinStrategyKind::kHashTypedCompositeEquality) {
      std::vector<std::size_t> representative_positions;
      std::unordered_map<std::size_t, std::vector<std::size_t>> right_hash;
      for (std::size_t position = 0; position < right_key_rows.size();
           ++position) {
        std::size_t bucket_id = representative_positions.size();
        for (std::size_t candidate = 0;
             candidate < representative_positions.size(); ++candidate) {
          if (right_comparisons[position * right_key_rows.size() +
                                representative_positions[candidate]] == 0) {
            bucket_id = candidate;
            break;
          }
        }
        if (bucket_id == representative_positions.size()) {
          representative_positions.push_back(position);
        }
        right_hash[bucket_id].push_back(right_key_rows[position]);
      }
      for (std::size_t left = 0; left < left_key_rows.size(); ++left) {
        std::optional<std::size_t> bucket_id;
        for (std::size_t candidate = 0;
             candidate < representative_positions.size(); ++candidate) {
          if (cross_comparisons[left * right_key_rows.size() +
                                representative_positions[candidate]] == 0) {
            bucket_id = candidate;
            break;
          }
        }
        if (!bucket_id.has_value()) continue;
        for (const auto right_row : right_hash.at(*bucket_id)) {
          if (!retain_pair(left_key_rows[left], right_row)) {
            return refuse("typed hash strategy output or probe bound was "
                          "exceeded");
          }
        }
      }
      if (candidate_probe_count != canonical.candidate_pair_count) {
        return refuse(
            "typed hash strategy candidate set differs from canonical keys");
      }
      result.strategy_key_comparison_count = comparison_count;
      return finish(std::move(strategy_pairs), right_key_rows.size(),
                    candidate_probe_count);
    }

    std::vector<std::size_t> left_order(left_key_rows.size());
    std::vector<std::size_t> right_order(right_key_rows.size());
    std::iota(left_order.begin(), left_order.end(), 0);
    std::iota(right_order.begin(), right_order.end(), 0);
    std::stable_sort(
        left_order.begin(), left_order.end(), [&](const auto left,
                                                   const auto right) {
          return left_comparisons[left * left_key_rows.size() + right] < 0;
        });
    std::stable_sort(
        right_order.begin(), right_order.end(), [&](const auto left,
                                                     const auto right) {
          return right_comparisons[left * right_key_rows.size() + right] < 0;
        });
    std::size_t left = 0;
    std::size_t right = 0;
    while (left < left_order.size() && right < right_order.size()) {
      const auto comparison =
          cross_comparisons[left_order[left] * right_key_rows.size() +
                            right_order[right]];
      if (comparison < 0) {
        ++left;
        continue;
      }
      if (comparison > 0) {
        ++right;
        continue;
      }
      auto left_end = left + 1;
      auto right_end = right + 1;
      while (left_end < left_order.size() &&
             left_comparisons[left_order[left] * left_key_rows.size() +
                              left_order[left_end]] == 0) {
        ++left_end;
      }
      while (right_end < right_order.size() &&
             right_comparisons[right_order[right] * right_key_rows.size() +
                               right_order[right_end]] == 0) {
        ++right_end;
      }
      for (auto left_match = left; left_match < left_end; ++left_match) {
        for (auto right_match = right; right_match < right_end;
             ++right_match) {
          if (!retain_pair(left_key_rows[left_order[left_match]],
                           right_key_rows[right_order[right_match]])) {
            return refuse("typed merge strategy output or probe bound was "
                          "exceeded");
          }
        }
      }
      left = left_end;
      right = right_end;
    }
    if (candidate_probe_count != canonical.candidate_pair_count) {
      return refuse(
          "typed merge strategy candidate set differs from canonical keys");
    }
    result.strategy_key_comparison_count = comparison_count;
    return finish(std::move(strategy_pairs), retained_count,
                  candidate_probe_count);
  }

  if (request.strategy ==
      CanonicalJoinStrategyKind::kMergeInnerInt64Equality) {
    const auto& term = key_request.key_terms.front();
    std::vector<std::pair<std::int64_t, std::size_t>> left_order;
    std::vector<std::pair<std::int64_t, std::size_t>> right_order;
    for (std::size_t left = 0; left < key_request.left_batch.rows.size();
         ++left) {
      const auto& value =
          key_request.left_batch.rows[left].values[term.left_column];
      if (value.state == api::EngineValueState::sql_null) continue;
      const auto decoded = DecodeInt64Value(value);
      if (!decoded.ok()) return refuse("merge strategy left key is invalid");
      if (left_order.size() + right_order.size() ==
          request.maximum_retained_entries) {
        return refuse("merge strategy retained-state bound was exceeded");
      }
      left_order.emplace_back(decoded.value, left);
    }
    for (std::size_t right = 0; right < key_request.right_batch.rows.size();
         ++right) {
      const auto& value =
          key_request.right_batch.rows[right].values[term.right_column];
      if (value.state == api::EngineValueState::sql_null) continue;
      const auto decoded = DecodeInt64Value(value);
      if (!decoded.ok()) return refuse("merge strategy right key is invalid");
      if (left_order.size() + right_order.size() ==
          request.maximum_retained_entries) {
        return refuse("merge strategy retained-state bound was exceeded");
      }
      right_order.emplace_back(decoded.value, right);
    }
    std::stable_sort(left_order.begin(), left_order.end());
    std::stable_sort(right_order.begin(), right_order.end());
    std::vector<std::size_t> strategy_pairs;
    std::size_t candidate_probe_count = 0;
    std::size_t left = 0;
    std::size_t right = 0;
    const auto right_count = key_request.right_batch.rows.size();
    while (left < left_order.size() && right < right_order.size()) {
      if (left_order[left].first < right_order[right].first) {
        ++left;
        continue;
      }
      if (right_order[right].first < left_order[left].first) {
        ++right;
        continue;
      }
      const auto key = left_order[left].first;
      auto left_end = left;
      auto right_end = right;
      while (left_end < left_order.size() &&
             left_order[left_end].first == key) {
        ++left_end;
      }
      while (right_end < right_order.size() &&
             right_order[right_end].first == key) {
        ++right_end;
      }
      for (auto left_match = left; left_match < left_end; ++left_match) {
        for (auto right_match = right; right_match < right_end;
             ++right_match) {
          if (candidate_probe_count == request.maximum_candidate_probes) {
            return refuse("merge strategy candidate probe bound was exceeded");
          }
          ++candidate_probe_count;
          const auto pair = left_order[left_match].second * right_count +
                            right_order[right_match].second;
          if (request.residual_request.residual_truth_values[pair] ==
              api::EngineSqlTruthValue::true_value) {
            if (strategy_pairs.size() == request.maximum_output_rows) {
              return refuse("merge strategy output row bound was exceeded");
            }
            strategy_pairs.push_back(pair);
          }
        }
      }
      left = left_end;
      right = right_end;
    }
    if (candidate_probe_count != canonical.candidate_pair_count) {
      return refuse("merge strategy candidate set differs from canonical keys");
    }
    return finish(std::move(strategy_pairs),
                  left_order.size() + right_order.size(),
                  candidate_probe_count);
  }

  const auto& term = key_request.key_terms.front();
  std::map<std::int64_t, std::vector<std::size_t>> right_hash;
  std::size_t hash_entry_count = 0;
  for (std::size_t right = 0; right < key_request.right_batch.rows.size();
       ++right) {
    const auto& value =
        key_request.right_batch.rows[right].values[term.right_column];
    if (value.state == api::EngineValueState::sql_null) continue;
    const auto decoded = DecodeInt64Value(value);
    if (!decoded.ok()) {
      return refuse("hash strategy right key encoding is invalid");
    }
    if (hash_entry_count == request.maximum_hash_entries ||
        hash_entry_count == request.maximum_retained_entries) {
      return refuse("hash strategy entry bound was exceeded");
    }
    right_hash[decoded.value].push_back(right);
    ++hash_entry_count;
  }

  std::vector<std::size_t> strategy_pair_indices;
  std::size_t candidate_probe_count = 0;
  const auto right_count = key_request.right_batch.rows.size();
  for (std::size_t left = 0; left < key_request.left_batch.rows.size();
       ++left) {
    const auto& left_value =
        key_request.left_batch.rows[left].values[term.left_column];
    if (left_value.state == api::EngineValueState::sql_null) continue;
    const auto left_decoded = DecodeInt64Value(left_value);
    if (!left_decoded.ok()) {
      return refuse("hash strategy left key encoding is invalid");
    }
    const auto bucket = right_hash.find(left_decoded.value);
    if (bucket == right_hash.end()) continue;
    for (const auto right : bucket->second) {
      if (candidate_probe_count == request.maximum_candidate_probes) {
        return refuse("hash strategy candidate probe bound was exceeded");
      }
      ++candidate_probe_count;
      const auto pair = left * right_count + right;
      if (request.residual_request.residual_truth_values[pair] !=
          api::EngineSqlTruthValue::true_value) {
        continue;
      }
      if (strategy_pair_indices.size() == request.maximum_output_rows) {
        return refuse("hash strategy output row bound was exceeded");
      }
      strategy_pair_indices.push_back(pair);
    }
  }
  if (candidate_probe_count != canonical.candidate_pair_count) {
    return refuse("hash strategy candidate set differs from canonical keys");
  }

  return finish(std::move(strategy_pair_indices), hash_entry_count,
                candidate_probe_count);
}

// QOW-SOURCE-QRY-012-MGA-V1
// QOW-SOURCE-QRY-012-MGA-V2
// Recheck strategy candidates against engine-owned transaction-inventory and
// statement-snapshot evidence at the MGA boundary.  Visibility and security
// verdicts are consumed, never synthesized here. The input-row evidence
// profile filters relations before non-inner semantics, then exact-rechecks
// every matched physical pair with the canonical typed composite key contract;
// stale generations or drift fail atomically.
CanonicalJoinMgaResult ExecuteCanonicalJoinMgaBoundary(
    const CanonicalJoinMgaRequest& request) {
  namespace api = scratchbird::engine::internal_api;

  CanonicalJoinMgaResult result;
  const auto refuse = [&](std::string detail) {
    result.diagnostic.ok = false;
    result.diagnostic.diagnostic_code =
        "QOW-DIAG-QRY-012-MGA-REFUSAL-V1";
    result.diagnostic.detail = std::move(detail);
    result.output_batch = {};
    result.candidate_pair_count = 0;
    result.visible_pair_count = 0;
    result.visibility_filtered_pair_count = 0;
    result.security_filtered_pair_count = 0;
    result.visible_left_row_count = 0;
    result.visible_right_row_count = 0;
    result.visibility_filtered_left_row_count = 0;
    result.visibility_filtered_right_row_count = 0;
    result.security_filtered_left_row_count = 0;
    result.security_filtered_right_row_count = 0;
    result.mga_boundary_proven = false;
    result.transaction_inventory_evidence_uuid.clear();
    result.selected_plan_uuid.clear();
    result.executed_physical_node_id = 0;
    result.causal_counter_id = 0;
    return result;
  };

  const auto& physical_dag =
      request.strategy_request.residual_request.key_request.physical_dag;
  const auto authority_validation = RevalidateCanonicalExecutionMgaAuthority(
      request.mga_authority, physical_dag);
  if (!authority_validation.ok ||
      !PhysicalMgaStatementContextEqual(
          request.mga_authority.statement_context,
          request.strategy_request.residual_request.key_request.mga_authority
              .statement_context)) {
    return refuse(authority_validation.ok
                      ? "join key context differs from MGA boundary"
                      : authority_validation.diagnostic_code + ":" +
                            authority_validation.detail);
  }
  if (request.strategy_request.join_kind !=
          CanonicalAcceptedJoinKind::kInner &&
      !request.input_row_evidence_profile) {
    return refuse(
        "non-inner MGA execution requires input-row evidence");
  }
  if (request.transaction_inventory_id == 0 ||
      !IsCanonicalUuid(request.transaction_inventory_evidence_uuid)) {
    return refuse("engine transaction inventory evidence is not bound");
  }
  if (request.maximum_boundary_rechecks == 0) {
    return refuse("MGA join boundary recheck bound was exceeded");
  }
  std::size_t boundary_recheck_count = request.candidate_evidence.size();
  if (request.input_row_evidence_profile) {
    if (request.left_row_evidence.size() >
            std::numeric_limits<std::size_t>::max() -
                boundary_recheck_count ||
        request.right_row_evidence.size() >
            std::numeric_limits<std::size_t>::max() -
                boundary_recheck_count - request.left_row_evidence.size()) {
      return refuse("MGA join boundary recheck cardinality overflowed");
    }
    boundary_recheck_count += request.left_row_evidence.size() +
                              request.right_row_evidence.size();
  }
  if (boundary_recheck_count > request.maximum_boundary_rechecks) {
    return refuse("MGA join boundary recheck bound was exceeded");
  }

  const auto exact_key_truth = [&](const CanonicalCompositeJoinKeyRequest& key,
                                   const std::size_t left_row,
                                   const std::size_t right_row,
                                   std::string* detail) {
    if (detail == nullptr || left_row >= key.left_batch.rows.size() ||
        right_row >= key.right_batch.rows.size() || key.key_terms.empty()) {
      if (detail != nullptr) *detail = "join key row or term is unresolved";
      return api::EngineSqlTruthValue::unspecified;
    }
    bool saw_unknown = false;
    for (const auto& term : key.key_terms) {
      if (term.left_column >= key.left_batch.columns.size() ||
          term.right_column >= key.right_batch.columns.size()) {
        *detail = "join key column is outside the bound input";
        return api::EngineSqlTruthValue::unspecified;
      }
      const auto& left =
          key.left_batch.rows[left_row].values[term.left_column];
      const auto& right =
          key.right_batch.rows[right_row].values[term.right_column];
      if (left.state == api::EngineValueState::sql_null ||
          right.state == api::EngineValueState::sql_null) {
        saw_unknown = true;
        continue;
      }
      bool equal = false;
      if (!CompareCanonicalJoinKeyValues(left, right, term, &equal, detail)) {
        return api::EngineSqlTruthValue::unspecified;
      }
      if (!equal) return api::EngineSqlTruthValue::false_value;
    }
    detail->clear();
    return saw_unknown ? api::EngineSqlTruthValue::unknown
                       : api::EngineSqlTruthValue::true_value;
  };

  if (request.input_row_evidence_profile) {
    const auto& original_key_request =
        request.strategy_request.residual_request.key_request;
    const auto original_left_count = original_key_request.left_batch.rows.size();
    const auto original_right_count =
        original_key_request.right_batch.rows.size();
    if (request.left_row_evidence.size() != original_left_count ||
        request.right_row_evidence.size() != original_right_count) {
      return refuse("MGA input-row evidence cardinality is not bound");
    }
    if (original_left_count != 0 &&
        original_right_count >
            std::numeric_limits<std::size_t>::max() / original_left_count) {
      return refuse("MGA input pair cardinality overflowed");
    }
    const auto original_pair_count =
        original_left_count * original_right_count;
    if (request.strategy_request.residual_request.residual_truth_values.size() !=
        original_pair_count) {
      return refuse("MGA residual matrix is not bound to the input rows");
    }
    const auto original_keys =
        ExecuteCanonicalCompositeJoinKey(original_key_request);
    if (!original_keys.diagnostic.ok) {
      return refuse(original_keys.diagnostic.diagnostic_code + ":" +
                    original_keys.diagnostic.detail);
    }
    if (!PhysicalMgaStatementContextEqual(
            original_keys.mga_statement_context,
            request.mga_authority.statement_context)) {
      return refuse(
          "MGA original input validation returned a different statement context");
    }
    for (const auto truth :
         request.strategy_request.residual_request.residual_truth_values) {
      bool ignored = false;
      std::string detail;
      if (!api::QowPredicateConsumerPassesV1(
              truth, api::EnginePredicateConsumer::join_on, &ignored,
              &detail)) {
        return refuse("MGA residual predicate domain is invalid:" + detail);
      }
    }

    std::vector<std::size_t> visible_left_indices;
    std::vector<std::size_t> visible_right_indices;
    std::size_t visibility_filtered_left_row_count = 0;
    std::size_t visibility_filtered_right_row_count = 0;
    std::size_t security_filtered_left_row_count = 0;
    std::size_t security_filtered_right_row_count = 0;
    const auto collect_visible = [&](const auto& evidence_vector,
                                     std::vector<std::size_t>* visible,
                                     std::size_t* visibility_filtered,
                                     std::size_t* security_filtered) {
      for (std::size_t index = 0; index < evidence_vector.size(); ++index) {
        const auto& evidence = evidence_vector[index];
        if (evidence.row_index != index ||
            evidence.creator_local_transaction_id == 0 ||
            evidence.row_version_id == 0 ||
            evidence.candidate_generation == 0 ||
            evidence.current_generation == 0 ||
            evidence.candidate_generation != evidence.current_generation ||
            !IsCanonicalUuid(evidence.engine_evidence_uuid)) {
          return false;
        }
        if (evidence.visibility == CanonicalMgaVisibilityDecision::kVisible &&
            !CanonicalMgaCreatorVisibleToStatement(
                request.mga_authority.statement_context,
                evidence.creator_local_transaction_id)) {
          return false;
        }
        if (evidence.visibility !=
                CanonicalMgaVisibilityDecision::kVisible &&
            evidence.visibility !=
                CanonicalMgaVisibilityDecision::kInvisible) {
          return false;
        }
        if (evidence.security_decision !=
                CanonicalMgaSecurityDecision::kAllowed &&
            evidence.security_decision !=
                CanonicalMgaSecurityDecision::kDenied) {
          return false;
        }
        if (evidence.visibility ==
            CanonicalMgaVisibilityDecision::kInvisible) {
          ++*visibility_filtered;
        } else if (evidence.security_decision ==
                   CanonicalMgaSecurityDecision::kDenied) {
          ++*security_filtered;
        } else {
          visible->push_back(index);
        }
      }
      return true;
    };
    if (!collect_visible(request.left_row_evidence, &visible_left_indices,
                         &visibility_filtered_left_row_count,
                         &security_filtered_left_row_count) ||
        !collect_visible(request.right_row_evidence, &visible_right_indices,
                         &visibility_filtered_right_row_count,
                         &security_filtered_right_row_count)) {
      return refuse("MGA input-row identity or verdict is invalid");
    }

    auto filtered_request = request.strategy_request;
    auto& filtered_key_request =
        filtered_request.residual_request.key_request;
    filtered_key_request.left_batch.rows.clear();
    filtered_key_request.right_batch.rows.clear();
    for (const auto original : visible_left_indices) {
      filtered_key_request.left_batch.rows.push_back(
          original_key_request.left_batch.rows[original]);
    }
    for (const auto original : visible_right_indices) {
      filtered_key_request.right_batch.rows.push_back(
          original_key_request.right_batch.rows[original]);
    }
    filtered_request.residual_request.residual_truth_values.clear();
    for (const auto left : visible_left_indices) {
      for (const auto right : visible_right_indices) {
        filtered_request.residual_request.residual_truth_values.push_back(
            request.strategy_request.residual_request.residual_truth_values[
                left * original_right_count + right]);
      }
    }

    auto strategy = ExecuteCanonicalJoinStrategy(filtered_request);
    if (!strategy.diagnostic.ok) {
      return refuse(strategy.diagnostic.diagnostic_code + ":" +
                    strategy.diagnostic.detail);
    }
    if (!PhysicalMgaStatementContextEqual(
            strategy.mga_statement_context,
            request.mga_authority.statement_context)) {
      return refuse(
          "MGA filtered join strategy returned a different statement context");
    }
    if (request.candidate_evidence.size() !=
        strategy.strategy_pair_indices.size()) {
      return refuse("MGA matched-pair evidence cardinality is not bound");
    }
    if (!strategy.canonical_multiset_proven ||
        !strategy.canonical_output_proven) {
      return refuse("MGA strategy lacks canonical pair/output proof");
    }

    const auto filtered_right_count = visible_right_indices.size();
    for (std::size_t index = 0; index < request.candidate_evidence.size();
         ++index) {
      if (filtered_right_count == 0) {
        return refuse("matched MGA candidate has no physical right row");
      }
      const auto filtered_pair = strategy.strategy_pair_indices[index];
      const auto filtered_left = filtered_pair / filtered_right_count;
      const auto filtered_right = filtered_pair % filtered_right_count;
      if (filtered_left >= visible_left_indices.size() ||
          filtered_right >= visible_right_indices.size()) {
        return refuse("matched MGA pair identity is outside filtered input");
      }
      const auto original_left = visible_left_indices[filtered_left];
      const auto original_right = visible_right_indices[filtered_right];
      const auto original_pair =
          original_left * original_right_count + original_right;
      const auto& evidence = request.candidate_evidence[index];
      if (evidence.pair_index != original_pair ||
          evidence.left_creator_local_transaction_id !=
              request.left_row_evidence[original_left]
                  .creator_local_transaction_id ||
          evidence.right_creator_local_transaction_id !=
              request.right_row_evidence[original_right]
                  .creator_local_transaction_id ||
          evidence.left_row_version_id !=
              request.left_row_evidence[original_left].row_version_id ||
          evidence.right_row_version_id !=
              request.right_row_evidence[original_right].row_version_id ||
          evidence.left_visibility !=
              CanonicalMgaVisibilityDecision::kVisible ||
          evidence.right_visibility !=
              CanonicalMgaVisibilityDecision::kVisible ||
          evidence.security_decision !=
              CanonicalMgaSecurityDecision::kAllowed ||
          evidence.index_candidate_generation == 0 ||
          evidence.current_index_generation == 0 ||
          evidence.index_candidate_generation !=
              evidence.current_index_generation ||
          !IsCanonicalUuid(evidence.engine_evidence_uuid)) {
        return refuse("matched MGA candidate evidence is not exact");
      }
      if (!CanonicalMgaCreatorVisibleToStatement(
              request.mga_authority.statement_context,
              evidence.left_creator_local_transaction_id) ||
          !CanonicalMgaCreatorVisibleToStatement(
              request.mga_authority.statement_context,
              evidence.right_creator_local_transaction_id)) {
        return refuse(
            "matched visible MGA candidate contradicts captured vector");
      }
      std::string exact_key_detail;
      const auto computed_key_truth = exact_key_truth(
          filtered_key_request, filtered_left, filtered_right,
          &exact_key_detail);
      if (computed_key_truth == api::EngineSqlTruthValue::unspecified) {
        return refuse("matched MGA exact key encoding is invalid:" +
                      exact_key_detail);
      }
      if (computed_key_truth != api::EngineSqlTruthValue::true_value ||
          evidence.exact_key_recheck != computed_key_truth) {
        return refuse("matched MGA candidate failed exact key recheck");
      }
    }

    result.diagnostic = {};
    result.output_batch = std::move(strategy.output_batch);
    result.candidate_pair_count = request.candidate_evidence.size();
    result.visible_pair_count = request.candidate_evidence.size();
    result.visibility_filtered_pair_count = 0;
    result.security_filtered_pair_count = 0;
    result.visible_left_row_count = visible_left_indices.size();
    result.visible_right_row_count = visible_right_indices.size();
    result.visibility_filtered_left_row_count =
        visibility_filtered_left_row_count;
    result.visibility_filtered_right_row_count =
        visibility_filtered_right_row_count;
    result.security_filtered_left_row_count =
        security_filtered_left_row_count;
    result.security_filtered_right_row_count =
        security_filtered_right_row_count;
    result.mga_boundary_proven = true;
    result.transaction_inventory_evidence_uuid =
        request.transaction_inventory_evidence_uuid;
    result.selected_plan_uuid = std::move(strategy.selected_plan_uuid);
    result.executed_physical_node_id = strategy.executed_physical_node_id;
    result.causal_counter_id = strategy.causal_counter_id;
    const auto result_authority = RevalidateCanonicalExecutionMgaAuthority(
        request.mga_authority, physical_dag);
    if (!result_authority.ok) {
      return refuse(result_authority.diagnostic_code + ":" +
                    result_authority.detail);
    }
    result.mga_statement_context = request.mga_authority.statement_context;
    return result;
  }

  auto strategy =
      ExecuteCanonicalJoinStrategy(request.strategy_request);
  if (!strategy.diagnostic.ok) {
    return refuse(strategy.diagnostic.diagnostic_code + ":" +
                  strategy.diagnostic.detail);
  }
  if (!PhysicalMgaStatementContextEqual(
          strategy.mga_statement_context,
          request.mga_authority.statement_context)) {
    return refuse("MGA join strategy returned a different statement context");
  }
  if (request.candidate_evidence.size() !=
          strategy.strategy_pair_indices.size() ||
      strategy.output_batch.rows.size() !=
          strategy.strategy_pair_indices.size()) {
    return refuse("MGA candidate evidence cardinality is not bound");
  }

  std::vector<bool> publish_candidate(request.candidate_evidence.size(),
                                      false);
  std::size_t visible_pair_count = 0;
  std::size_t visibility_filtered_pair_count = 0;
  std::size_t security_filtered_pair_count = 0;
  const auto& key_request =
      request.strategy_request.residual_request.key_request;
  const auto right_count = key_request.right_batch.rows.size();
  for (std::size_t index = 0; index < request.candidate_evidence.size();
       ++index) {
    const auto& evidence = request.candidate_evidence[index];
    if (evidence.pair_index != strategy.strategy_pair_indices[index] ||
        evidence.left_creator_local_transaction_id == 0 ||
        evidence.right_creator_local_transaction_id == 0 ||
        evidence.left_row_version_id == 0 ||
        evidence.right_row_version_id == 0 ||
        !IsCanonicalUuid(evidence.engine_evidence_uuid)) {
      return refuse("MGA candidate identity or evidence is not bound");
    }

    const auto valid_visibility = [](const auto decision) {
      return decision == CanonicalMgaVisibilityDecision::kVisible ||
             decision == CanonicalMgaVisibilityDecision::kInvisible ||
             decision == CanonicalMgaVisibilityDecision::kIndeterminate;
    };
    if (!valid_visibility(evidence.left_visibility) ||
        !valid_visibility(evidence.right_visibility) ||
        evidence.left_visibility ==
            CanonicalMgaVisibilityDecision::kIndeterminate ||
        evidence.right_visibility ==
            CanonicalMgaVisibilityDecision::kIndeterminate) {
      return refuse("MGA visibility decision is invalid or indeterminate");
    }
    if ((evidence.left_visibility ==
             CanonicalMgaVisibilityDecision::kVisible &&
         !CanonicalMgaCreatorVisibleToStatement(
             request.mga_authority.statement_context,
             evidence.left_creator_local_transaction_id)) ||
        (evidence.right_visibility ==
             CanonicalMgaVisibilityDecision::kVisible &&
         !CanonicalMgaCreatorVisibleToStatement(
             request.mga_authority.statement_context,
             evidence.right_creator_local_transaction_id))) {
      return refuse("visible MGA candidate contradicts captured vector");
    }
    if (evidence.security_decision !=
            CanonicalMgaSecurityDecision::kAllowed &&
        evidence.security_decision !=
            CanonicalMgaSecurityDecision::kDenied &&
        evidence.security_decision !=
            CanonicalMgaSecurityDecision::kIndeterminate) {
      return refuse("MGA security decision is invalid");
    }
    if (evidence.security_decision ==
        CanonicalMgaSecurityDecision::kIndeterminate) {
      return refuse("MGA security decision is indeterminate");
    }
    if (evidence.index_candidate_generation == 0 ||
        evidence.current_index_generation == 0 ||
        evidence.index_candidate_generation !=
            evidence.current_index_generation) {
      return refuse("stale index candidate generation requires replanning");
    }
    if (right_count == 0) {
      return refuse("index candidate has no physical right row");
    }
    const auto left = evidence.pair_index / right_count;
    const auto right = evidence.pair_index % right_count;
    std::string exact_key_detail;
    const auto computed_key_truth =
        exact_key_truth(key_request, left, right, &exact_key_detail);
    if (computed_key_truth == api::EngineSqlTruthValue::unspecified) {
      return refuse("index candidate exact key encoding is invalid:" +
                    exact_key_detail);
    }
    if (computed_key_truth != api::EngineSqlTruthValue::true_value ||
        evidence.exact_key_recheck != computed_key_truth) {
      return refuse("index candidate failed exact join-key recheck");
    }

    if (evidence.left_visibility ==
            CanonicalMgaVisibilityDecision::kInvisible ||
        evidence.right_visibility ==
            CanonicalMgaVisibilityDecision::kInvisible) {
      ++visibility_filtered_pair_count;
      continue;
    }
    if (evidence.security_decision ==
        CanonicalMgaSecurityDecision::kDenied) {
      ++security_filtered_pair_count;
      continue;
    }
    publish_candidate[index] = true;
    ++visible_pair_count;
  }

  DescriptorBatch output;
  output.columns = strategy.output_batch.columns;
  for (std::size_t index = 0; index < publish_candidate.size(); ++index) {
    if (publish_candidate[index]) {
      output.rows.push_back(std::move(strategy.output_batch.rows[index]));
    }
  }
  std::vector<std::uint32_t> output_descriptor_ids;
  output_descriptor_ids.reserve(output.columns.size());
  for (const auto& column : output.columns) {
    output_descriptor_ids.push_back(column.descriptor_id);
  }
  auto validation =
      ValidateCanonicalDescriptorBatch(output, output_descriptor_ids);
  if (!validation.ok) {
    return refuse(validation.diagnostic_code + ":" + validation.detail);
  }

  result.diagnostic = {};
  result.output_batch = std::move(output);
  result.candidate_pair_count = request.candidate_evidence.size();
  result.visible_pair_count = visible_pair_count;
  result.visibility_filtered_pair_count = visibility_filtered_pair_count;
  result.security_filtered_pair_count = security_filtered_pair_count;
  result.mga_boundary_proven = true;
  result.transaction_inventory_evidence_uuid =
      request.transaction_inventory_evidence_uuid;
  result.selected_plan_uuid = std::move(strategy.selected_plan_uuid);
  result.executed_physical_node_id =
      strategy.executed_physical_node_id;
  result.causal_counter_id = strategy.causal_counter_id;
  const auto result_authority = RevalidateCanonicalExecutionMgaAuthority(
      request.mga_authority, physical_dag);
  if (!result_authority.ok) {
    return refuse(result_authority.diagnostic_code + ":" +
                  result_authority.detail);
  }
  result.mga_statement_context = request.mga_authority.statement_context;
  return result;
}

// Execute set operations only after the two input
// descriptor vectors and the result descriptor vector have been bound to one
// admitted physical set-operation node.  Multiset membership uses decoded
// typed values; parser text and legacy integer batches are never consulted.
static CanonicalSetOperationAllResult ExecuteCanonicalSetOperationQuantified(
    const CanonicalSetOperationAllRequest& request,
    const CanonicalSetOperationQuantifier admitted_quantifier) {
  using api = scratchbird::engine::internal_api::EngineValueState;
  const bool distinct =
      admitted_quantifier == CanonicalSetOperationQuantifier::kDistinct;

  CanonicalSetOperationAllResult result;
  const auto refuse = [&](std::string detail,
                          std::string diagnostic_code = std::string{}) {
    if (diagnostic_code.empty()) {
      diagnostic_code = distinct
                            ? "QOW-DIAG-QRY-016-DISTINCT-REFUSAL-V1"
                            : "QOW-DIAG-QRY-016-ALL-REFUSAL-V1";
    }
    result.diagnostic.ok = false;
    result.diagnostic.diagnostic_code = std::move(diagnostic_code);
    result.diagnostic.detail = std::move(detail);
    result.output_batch = {};
    result.left_input_row_count = 0;
    result.right_input_row_count = 0;
    result.consumed_right_multiplicity_count = 0;
    result.eliminated_duplicate_row_count = 0;
    result.equality_comparison_count = 0;
    result.coerced_value_count = 0;
    result.reconciled_type_names.clear();
    result.right_to_result_column_indices.clear();
    result.implementation_id.clear();
    result.selected_plan_uuid.clear();
    result.executed_physical_node_id = 0;
    result.causal_counter_id = 0;
    result.output_payload_bytes = 0;
    result.peak_live_payload_bytes = 0;
    result.resident_structural_bytes = 0;
    result.current_live_memory_bytes = 0;
    result.peak_live_memory_bytes = 0;
    result.memory_grant_bytes = 0;
    result.memory_grant_evidence_uuid.clear();
    return result;
  };

  if (request.quantifier != admitted_quantifier) {
    return refuse("set-operation quantifier and entry point differ");
  }
  const bool borrowed_left = request.borrowed_left_batch != nullptr;
  const bool borrowed_right = request.borrowed_right_batch != nullptr;
  const DescriptorBatch empty_batch;
  const auto exact_default_batch = [&](const DescriptorBatch& batch) {
    return batch.columns.empty() &&
           batch.columns.capacity() == empty_batch.columns.capacity() &&
           batch.rows.empty() &&
           batch.rows.capacity() == empty_batch.rows.capacity();
  };
  if (borrowed_left != borrowed_right ||
      (borrowed_left &&
       (!exact_default_batch(request.left_batch) ||
        !exact_default_batch(request.right_batch)))) {
    return refuse("set-operation input ownership carriage is inconsistent");
  }
  const auto& left_batch = borrowed_left ? *request.borrowed_left_batch
                                         : request.left_batch;
  const auto& right_batch = borrowed_right ? *request.borrowed_right_batch
                                           : request.right_batch;
  const bool bound_collation =
      request.equality_profile ==
      CanonicalSetOperationEqualityProfile::kNullEqualBoundCollation;
  if (!bound_collation &&
      request.equality_profile !=
          CanonicalSetOperationEqualityProfile::kExactTyped) {
    return refuse("set-operation equality profile is unknown");
  }
  if (!bound_collation && !request.collation_bindings.empty()) {
    return refuse("collation bindings require the bound-collation profile");
  }
  const bool reconcile_types =
      request.type_profile ==
      CanonicalSetOperationTypeProfile::kLosslessImplicit;
  if (!reconcile_types &&
      request.type_profile != CanonicalSetOperationTypeProfile::kExact) {
    return refuse("set-operation type reconciliation profile is unknown",
                  "QOW-DIAG-QRY-016-TYPE-REFUSAL-V1");
  }

  const auto authority_validation = RevalidateCanonicalExecutionMgaAuthority(
      request.mga_authority, request.physical_dag);
  if (!authority_validation.ok)
    return refuse(authority_validation.diagnostic_code + ":" +
                  authority_validation.detail);
  if (request.selected_physical_node_id == 0 ||
      request.selected_physical_node_id !=
          request.physical_dag.root_physical_node_id) {
    return refuse("selected set-operation node is not the physical root");
  }

  const PhysicalNodeRecord* selected_node = nullptr;
  for (const auto& node : request.physical_dag.nodes) {
    if (node.physical_node_id == request.selected_physical_node_id) {
      selected_node = &node;
      break;
    }
  }
  if (selected_node == nullptr ||
      selected_node->node_kind != PhysicalNodeKind::kSetOperation ||
      selected_node->input_physical_node_ids.size() != 2 ||
      selected_node->input_physical_node_ids[0] ==
          selected_node->input_physical_node_ids[1]) {
    return refuse(
        "set operation requires one selected binary physical node with "
        "two distinct inputs");
  }

  std::size_t memory_grant_bytes = 0;
  std::string memory_grant_evidence_uuid;
  if (request.enforce_payload_memory_grant) {
    if (request.physical_dag.memory_budget_bytes == 0 ||
        selected_node->memory_bytes_required == 0 ||
        selected_node->memory_bytes_required >
            request.physical_dag.memory_budget_bytes ||
        selected_node->memory_bytes_required >
            static_cast<std::uint64_t>(
                std::numeric_limits<std::size_t>::max())) {
      return refuse(
          "set-operation selected-node memory grant is invalid",
          "SBLR.PLAN_TREE.RESOURCE_LIMIT");
    }
    const PhysicalAdmissionEvidence* resource_evidence = nullptr;
    for (const auto& evidence : request.physical_dag.admission_evidence) {
      if (evidence.stage != PhysicalAdmissionStage::kResource) continue;
      if (resource_evidence != nullptr) {
        return refuse(
            "set-operation resource evidence is not unique",
            "SBLR.PLAN_TREE.RESOURCE_LIMIT");
      }
      resource_evidence = &evidence;
    }
    if (resource_evidence == nullptr ||
        resource_evidence->evidence_uuid.empty()) {
      return refuse(
          "set-operation resource evidence is absent",
          "SBLR.PLAN_TREE.RESOURCE_LIMIT");
    }
    memory_grant_bytes =
        static_cast<std::size_t>(selected_node->memory_bytes_required);
    memory_grant_evidence_uuid = resource_evidence->evidence_uuid;
  }

  const std::string expected_implementation = [&] {
    const bool by_name = request.alignment ==
                         CanonicalSetOperationAlignment::kByName;
    if (!by_name && request.alignment !=
                        CanonicalSetOperationAlignment::kOrdinal) {
      return std::string{};
    }
    std::string implementation;
    switch (request.operation) {
      case CanonicalSetOperationKind::kUnion:
        if (distinct) {
          implementation = by_name ? "setop.union-distinct.by-name"
                                   : "setop.union-distinct.ordinal";
          break;
        }
        implementation = by_name ? "setop.union-all.by-name"
                                 : "setop.union-all.ordinal";
        break;
      case CanonicalSetOperationKind::kIntersect:
        if (distinct) {
          implementation = by_name ? "setop.intersect-distinct.by-name"
                                   : "setop.intersect-distinct.ordinal";
          break;
        }
        implementation = by_name ? "setop.intersect-all.by-name"
                                 : "setop.intersect-all.ordinal";
        break;
      case CanonicalSetOperationKind::kExcept:
        if (distinct) {
          implementation = by_name ? "setop.except-distinct.by-name"
                                   : "setop.except-distinct.ordinal";
          break;
        }
        implementation = by_name ? "setop.except-all.by-name"
                                 : "setop.except-all.ordinal";
        break;
    }
    if (implementation.empty()) return implementation;
    if (reconcile_types) implementation += ".type-reconciled";
    if (bound_collation) implementation += ".null-collation";
    implementation += ".typed.v1";
    return implementation;
  }();
  if (expected_implementation.empty() ||
      selected_node->implementation_id != expected_implementation) {
    return refuse(
        "set-operation kind, quantifier, alignment, or type profile drifted",
        reconcile_types ? "QOW-DIAG-QRY-016-TYPE-REFUSAL-V1"
                        : std::string{});
  }

  const PhysicalNodeRecord* left_node = nullptr;
  const PhysicalNodeRecord* right_node = nullptr;
  for (const auto& node : request.physical_dag.nodes) {
    if (node.physical_node_id ==
        selected_node->input_physical_node_ids[0]) {
      left_node = &node;
    }
    if (node.physical_node_id ==
        selected_node->input_physical_node_ids[1]) {
      right_node = &node;
    }
  }
  if (left_node == nullptr || right_node == nullptr) {
    return refuse("set-operation input node is unresolved");
  }

  auto validation = ValidateCanonicalDescriptorBatch(
      left_batch, left_node->output_descriptor_ids);
  if (!validation.ok) {
    return refuse(validation.diagnostic_code + ":" + validation.detail);
  }
  validation = ValidateCanonicalDescriptorBatch(
      right_batch, right_node->output_descriptor_ids);
  if (!validation.ok) {
    return refuse(validation.diagnostic_code + ":" + validation.detail);
  }
  {
    DescriptorBatch output_schema;
    output_schema.columns = request.result_columns;
    validation = ValidateCanonicalDescriptorBatch(
        output_schema, selected_node->output_descriptor_ids);
    if (!validation.ok) {
      return refuse(validation.diagnostic_code + ":" + validation.detail);
    }
  }

  // QOW-SOURCE-QRY-016-ARITY-V1
  // Arity is decided from the three already-bound descriptor vectors, never
  // from the first physical row. Empty operands therefore retain the same
  // exact refusal behavior as populated operands.
  if (left_batch.columns.size() != right_batch.columns.size() ||
      left_batch.columns.size() != request.result_columns.size()) {
    return refuse("set-operation input and result arity differ",
                  "QOW-DIAG-QRY-016-ARITY-REFUSAL-V1");
  }

  FoundationSetMemoryLedger memory_ledger;
  memory_ledger.enforced = request.enforce_payload_memory_grant;
  memory_ledger.grant_bytes = memory_grant_bytes;
  const auto refuse_memory = [&]() {
    return refuse(memory_ledger.refusal_detail.empty()
                      ? "set-operation logical memory receipt is invalid"
                      : std::move(memory_ledger.refusal_detail),
                  "SBLR.PLAN_TREE.RESOURCE_LIMIT");
  };
  std::size_t left_input_payload_bytes = 0;
  std::size_t right_input_payload_bytes = 0;
  if (memory_ledger.enforced &&
      (!FoundationBatchPayloadBytes(left_batch, &left_input_payload_bytes) ||
       !FoundationBatchPayloadBytes(right_batch,
                                    &right_input_payload_bytes) ||
       !memory_ledger.RetainPayload(
           left_input_payload_bytes,
           "while retaining the left operand") ||
       !memory_ledger.RetainPayload(
           right_input_payload_bytes,
           "while retaining the right operand"))) {
    return refuse_memory();
  }

  std::optional<DescriptorBatch> aligned_right_storage;
  const DescriptorBatch* aligned_right = &right_batch;
  if (memory_ledger.enforced &&
      right_batch.columns.size() >
          std::numeric_limits<std::size_t>::max() / sizeof(std::size_t)) {
    memory_ledger.refusal_detail =
        "set-operation result mapping size overflowed";
    return refuse_memory();
  }
  std::vector<std::size_t> right_to_result_column_indices(
      right_batch.columns.size());
  std::iota(right_to_result_column_indices.begin(),
            right_to_result_column_indices.end(), 0);

  // QOW-SOURCE-QRY-016-BY-NAME-V1
  // BY NAME maps only the names from already-bound output descriptor records.
  // The aligned values retain their UUID-addressed descriptors, and duplicate,
  // missing, or result-order-drifted names refuse before multiset evaluation.
  std::size_t aligned_right_payload_bytes = 0;
  if (request.alignment == CanonicalSetOperationAlignment::kByName) {
    using NameMap = std::map<std::string, std::size_t>;
    NameMap left_names;
    NameMap right_names;
    NameMap result_names;
    std::size_t name_map_payload_bytes = 0;
    std::size_t name_map_structural_bytes = 0;
    const auto retain_name_entry = [&](const std::string& name) {
      if (!memory_ledger.enforced) return true;
      constexpr std::size_t kNameEntryStructuralBytes =
          sizeof(std::string) + sizeof(std::size_t);
      if (name.size() > std::numeric_limits<std::size_t>::max() -
                            name_map_payload_bytes ||
          kNameEntryStructuralBytes >
              std::numeric_limits<std::size_t>::max() -
                  name_map_structural_bytes) {
        memory_ledger.refusal_detail =
            "set-operation BY NAME entry size overflowed";
        return false;
      }
      if (!memory_ledger.RetainPayload(
              name.size(), "while retaining BY NAME lookup keys") ||
          !memory_ledger.RetainStructural(
              kNameEntryStructuralBytes,
              "while retaining BY NAME lookup slots")) {
        return false;
      }
      name_map_payload_bytes += name.size();
      name_map_structural_bytes += kNameEntryStructuralBytes;
      return true;
    };
    for (std::size_t column = 0; column < request.result_columns.size();
         ++column) {
      if (!left_names
               .emplace(left_batch.columns[column].stable_name, column)
               .second ||
          !right_names
               .emplace(right_batch.columns[column].stable_name, column)
               .second ||
          !result_names
               .emplace(request.result_columns[column].stable_name, column)
               .second) {
        return refuse("BY NAME requires unique bound column names",
                      "QOW-DIAG-QRY-016-BY-NAME-REFUSAL-V1");
      }
      if (left_batch.columns[column].stable_name !=
          request.result_columns[column].stable_name) {
        return refuse("BY NAME result order must follow the left operand",
                      "QOW-DIAG-QRY-016-BY-NAME-REFUSAL-V1");
      }
      if (!retain_name_entry(left_batch.columns[column].stable_name) ||
          !retain_name_entry(right_batch.columns[column].stable_name) ||
          !retain_name_entry(
              request.result_columns[column].stable_name)) {
        return refuse_memory();
      }
    }
    if (memory_ledger.enforced) {
      aligned_right_payload_bytes = right_input_payload_bytes;
    }
    if (!memory_ledger.RetainPayload(
            aligned_right_payload_bytes,
            "while retaining the BY NAME aligned right operand")) {
      return refuse_memory();
    }
    aligned_right_storage.emplace();
    auto& aligned = *aligned_right_storage;
    aligned.rows.assign(right_batch.rows.size(), {});
    aligned.columns.reserve(request.result_columns.size());
    for (auto& row : aligned.rows) {
      row.values.reserve(request.result_columns.size());
    }
    for (std::size_t result_column = 0;
         result_column < request.result_columns.size(); ++result_column) {
      const auto found = right_names.find(
          request.result_columns[result_column].stable_name);
      if (found == right_names.end()) {
        return refuse("BY NAME operand column sets differ",
                      "QOW-DIAG-QRY-016-BY-NAME-REFUSAL-V1");
      }
      const auto right_column = found->second;
      right_to_result_column_indices[right_column] = result_column;
      aligned.columns.push_back(right_batch.columns[right_column]);
      for (std::size_t row = 0; row < right_batch.rows.size(); ++row) {
        aligned.rows[row].values.push_back(
            right_batch.rows[row].values[right_column]);
      }
    }
    if (left_names.size() != right_names.size() ||
        left_names.size() != result_names.size()) {
      return refuse("BY NAME operand column sets differ",
                    "QOW-DIAG-QRY-016-BY-NAME-REFUSAL-V1");
    }
    if (memory_ledger.enforced) {
      std::size_t measured_aligned_right_payload_bytes = 0;
      if (!FoundationBatchPayloadBytes(
              aligned, &measured_aligned_right_payload_bytes) ||
          measured_aligned_right_payload_bytes !=
              aligned_right_payload_bytes) {
        memory_ledger.refusal_detail =
            "set-operation BY NAME aligned payload receipt drifted";
        return refuse_memory();
      }
    }
    if (!memory_ledger.ReleasePayload(
            name_map_payload_bytes,
            "after releasing BY NAME lookup keys") ||
        !memory_ledger.ReleaseStructural(
            name_map_structural_bytes,
            "after releasing BY NAME lookup slots")) {
      return refuse_memory();
    }
    aligned_right = &aligned;
  }

  namespace dt = scratchbird::core::datatypes;
  std::vector<std::string> reconciled_type_names;
  reconciled_type_names.reserve(request.result_columns.size());

  // QOW-SOURCE-QRY-016-TYPE-V1
  // The result descriptor is the already-bound common type. Only identity and
  // core-classified lossless implicit conversions may reach set equality;
  // explicit-only, lossy, forbidden, and unknown conversions are refusals.
  for (std::size_t column = 0; column < request.result_columns.size(); ++column) {
    const auto& left = left_batch.columns[column];
    const auto& right = aligned_right->columns[column];
    const auto& output = request.result_columns[column];
    bool compatible = left.descriptor.descriptor_kind == "scalar" &&
                      right.descriptor.descriptor_kind == "scalar" &&
                      output.descriptor.descriptor_kind == "scalar";
    if (compatible && output.nullable != (left.nullable || right.nullable)) {
      compatible = false;
    }
    if (compatible && !reconcile_types) {
      compatible =
          left.descriptor.canonical_type_name ==
              right.descriptor.canonical_type_name &&
          left.descriptor.canonical_type_name ==
              output.descriptor.canonical_type_name &&
          left.descriptor.encoded_descriptor ==
              right.descriptor.encoded_descriptor &&
          left.descriptor.encoded_descriptor ==
              output.descriptor.encoded_descriptor;
    }
    if (compatible && reconcile_types) {
      const auto target_type = dt::CanonicalTypeIdFromStableName(
          output.descriptor.canonical_type_name);
      const auto left_type = dt::CanonicalTypeIdFromStableName(
          left.descriptor.canonical_type_name);
      const auto right_type = dt::CanonicalTypeIdFromStableName(
          right.descriptor.canonical_type_name);
      const auto left_cast = dt::ClassifyDatatypeCast(left_type, target_type);
      const auto right_cast = dt::ClassifyDatatypeCast(right_type, target_type);
      const auto admitted = [](const dt::DatatypeCastCategory category) {
        return category == dt::DatatypeCastCategory::identity ||
               category == dt::DatatypeCastCategory::lossless_implicit;
      };
      compatible = target_type != dt::CanonicalTypeId::unknown &&
                   left_type != dt::CanonicalTypeId::unknown &&
                   right_type != dt::CanonicalTypeId::unknown &&
                   admitted(left_cast) && admitted(right_cast);
      if (compatible && bound_collation &&
          target_type == dt::CanonicalTypeId::character) {
        compatible =
            left.descriptor.encoded_descriptor ==
                right.descriptor.encoded_descriptor &&
            left.descriptor.encoded_descriptor ==
                output.descriptor.encoded_descriptor;
      }
    }
    if (!compatible) {
      return refuse(
          reconcile_types
              ? "set-operation type reconciliation is not lossless implicit"
              : "set-operation descriptors require exact reconciliation",
          reconcile_types
              ? "QOW-DIAG-QRY-016-TYPE-REFUSAL-V1"
              : bound_collation
              ? "QOW-DIAG-QRY-016-NULL-COLLATION-REFUSAL-V1"
              : std::string{});
    }
    reconciled_type_names.push_back(
        output.descriptor.canonical_type_name);
  }

  std::optional<DescriptorBatch> reconciled_left_storage;
  std::optional<DescriptorBatch> reconciled_ordinal_right_storage;
  const DescriptorBatch* execution_left = &left_batch;
  const DescriptorBatch* execution_right = aligned_right;
  std::size_t coerced_value_count = 0;
  std::size_t reconciled_left_payload_bytes = 0;
  std::size_t reconciled_ordinal_right_payload_bytes = 0;
  if (reconcile_types) {
    std::string reconciliation_detail;
    const auto reconcile_batch = [&](DescriptorBatch* batch,
                                     std::size_t* component_payload_bytes,
                                     const std::string_view phase) {
      if (batch == nullptr || component_payload_bytes == nullptr) return false;
      for (std::size_t column = 0; column < batch->columns.size(); ++column) {
        const auto source_type = dt::CanonicalTypeIdFromStableName(
            batch->columns[column].descriptor.canonical_type_name);
        const auto target_type = dt::CanonicalTypeIdFromStableName(
            request.result_columns[column].descriptor.canonical_type_name);
        for (auto& row : batch->rows) {
          dt::DatatypeCastRequest source_validation;
          source_validation.value.type_id = source_type;
          source_validation.value.encoded_value =
              row.values[column].encoded_value;
          source_validation.value.is_null =
              row.values[column].state == api::sql_null;
          source_validation.target_type_id = source_type;
          const auto validated = dt::CastDatatypeValue(source_validation);
          if (!validated.ok()) {
            reconciliation_detail =
                validated.diagnostic.diagnostic_code.empty()
                    ? "set-operation source value is not canonical"
                    : validated.diagnostic.diagnostic_code;
            return false;
          }
          dt::DatatypeCastRequest conversion;
          conversion.value = source_validation.value;
          conversion.target_type_id = target_type;
          const auto cast = dt::CastDatatypeValue(conversion);
          if (!cast.ok()) {
            reconciliation_detail =
                cast.diagnostic.diagnostic_code.empty()
                    ? "set-operation lossless implicit cast refused"
                    : cast.diagnostic.diagnostic_code;
            return false;
          }
          if (memory_ledger.enforced) {
            const auto old_encoded_bytes =
                row.values[column].encoded_value.size();
            const auto old_binary_bytes =
                row.values[column].binary_value.size();
            if (old_binary_bytes >
                    std::numeric_limits<std::size_t>::max() -
                        old_encoded_bytes) {
              memory_ledger.refusal_detail =
                  "set-operation reconciliation payload size overflowed";
              return false;
            }
            const auto old_payload_bytes =
                old_encoded_bytes + old_binary_bytes;
            const auto new_payload_bytes = cast.value.encoded_value.size();
            if (old_payload_bytes > *component_payload_bytes ||
                new_payload_bytes >
                    std::numeric_limits<std::size_t>::max() -
                        (*component_payload_bytes - old_payload_bytes) ||
                !memory_ledger.ReplacePayload(
                    old_payload_bytes, new_payload_bytes, phase)) {
              if (memory_ledger.refusal_detail.empty()) {
                memory_ledger.refusal_detail =
                    "set-operation reconciliation payload receipt drifted";
              }
              return false;
            }
            *component_payload_bytes =
                *component_payload_bytes - old_payload_bytes +
                new_payload_bytes;
          }
          if (source_type != target_type) ++coerced_value_count;
          row.values[column].descriptor =
              request.result_columns[column].descriptor;
          row.values[column].encoded_value = cast.value.encoded_value;
          row.values[column].binary_value.clear();
          row.values[column].is_null = cast.value.is_null;
          row.values[column].state =
              cast.value.is_null ? api::sql_null : api::value;
        }
        batch->columns[column].descriptor =
            request.result_columns[column].descriptor;
        batch->columns[column].nullable =
            request.result_columns[column].nullable;
      }
      return true;
    };
    if (memory_ledger.enforced) {
      reconciled_left_payload_bytes = left_input_payload_bytes;
      if (!memory_ledger.RetainPayload(
              reconciled_left_payload_bytes,
              "before retaining the reconciled left operand")) {
        return refuse_memory();
      }
    }
    reconciled_left_storage.emplace(left_batch);
    DescriptorBatch* mutable_right = nullptr;
    std::size_t* mutable_right_payload_bytes = nullptr;
    if (aligned_right_storage.has_value()) {
      mutable_right = &*aligned_right_storage;
      mutable_right_payload_bytes = &aligned_right_payload_bytes;
    } else {
      if (memory_ledger.enforced) {
        reconciled_ordinal_right_payload_bytes =
            right_input_payload_bytes;
        if (!memory_ledger.RetainPayload(
                reconciled_ordinal_right_payload_bytes,
                "before retaining the reconciled right operand")) {
          return refuse_memory();
        }
      }
      reconciled_ordinal_right_storage.emplace(right_batch);
      mutable_right = &*reconciled_ordinal_right_storage;
      mutable_right_payload_bytes =
          &reconciled_ordinal_right_payload_bytes;
    }
    if (!reconcile_batch(&*reconciled_left_storage,
                         &reconciled_left_payload_bytes,
                         "while reconciling the left operand") ||
        !reconcile_batch(mutable_right, mutable_right_payload_bytes,
                         "while reconciling the right operand")) {
      if (!memory_ledger.refusal_detail.empty()) return refuse_memory();
      return refuse(std::move(reconciliation_detail),
                    "QOW-DIAG-QRY-016-TYPE-REFUSAL-V1");
    }
    execution_left = &*reconciled_left_storage;
    execution_right = mutable_right;
  }
  const auto& reconciled_left = *execution_left;
  const auto& reconciled_right = *execution_right;

  std::size_t collation_lookup_structural_bytes = 0;
  if (memory_ledger.enforced) {
    if (request.result_columns.size() >
        std::numeric_limits<std::size_t>::max() /
            sizeof(const CanonicalSetOperationCollationBinding*)) {
      memory_ledger.refusal_detail =
          "set-operation collation lookup size overflowed";
      return refuse_memory();
    }
    collation_lookup_structural_bytes =
        request.result_columns.size() *
        sizeof(const CanonicalSetOperationCollationBinding*);
    if (!memory_ledger.RetainStructural(
            collation_lookup_structural_bytes,
            "while retaining the collation lookup")) {
      return refuse_memory();
    }
  }
  std::vector<const CanonicalSetOperationCollationBinding*> collation_by_column(
      request.result_columns.size(), nullptr);

  // QOW-SOURCE-QRY-016-NULL-COLLATION-V1
  // SQL NULL equality is handled as a set-membership rule. Character equality
  // additionally requires one catalog-bound collation record for every text
  // result column; duplicate/missing fields or incomplete seed authority fail
  // before any equivalence class is constructed.
  if (bound_collation) {
    namespace dt = scratchbird::core::datatypes;
    if (request.maximum_equality_comparison_count == 0) {
      return refuse("set-operation equality comparison bound is zero",
                    "QOW-DIAG-QRY-016-NULL-COLLATION-REFUSAL-V1");
    }
    for (const auto& binding : request.collation_bindings) {
      if (binding.result_column >= request.result_columns.size() ||
          collation_by_column[binding.result_column] != nullptr) {
        return refuse("set-operation collation binding is duplicate or out of range",
                      "QOW-DIAG-QRY-016-NULL-COLLATION-REFUSAL-V1");
      }
      collation_by_column[binding.result_column] = &binding;
    }
    const auto descriptor_field = [](const std::string& encoded,
                                     const std::string_view field,
                                     std::string* value) {
      if (value == nullptr) return false;
      value->clear();
      std::size_t match_count = 0;
      std::size_t begin = 0;
      while (begin <= encoded.size()) {
        const auto end = encoded.find(';', begin);
        const auto token = encoded.substr(
            begin, end == std::string::npos ? std::string::npos : end - begin);
        const std::string prefix = std::string(field) + "=";
        if (token.rfind(prefix, 0) == 0) {
          ++match_count;
          *value = token.substr(prefix.size());
        }
        if (end == std::string::npos) break;
        begin = end + 1;
      }
      return match_count == 1 && !value->empty();
    };
    for (std::size_t column = 0; column < request.result_columns.size();
         ++column) {
      const bool character =
          dt::CanonicalTypeIdFromStableName(
              request.result_columns[column].descriptor.canonical_type_name) ==
          dt::CanonicalTypeId::character;
      const auto* binding = collation_by_column[column];
      if (!character) {
        if (binding != nullptr) {
          return refuse("collation binding targets a non-character column",
                        "QOW-DIAG-QRY-016-NULL-COLLATION-REFUSAL-V1");
        }
        continue;
      }
      std::string descriptor_collation;
      if (binding == nullptr ||
          !descriptor_field(
              request.result_columns[column].descriptor.encoded_descriptor,
              "collation_uuid", &descriptor_collation) ||
          !IsCanonicalUuid(binding->collation_uuid) ||
          descriptor_collation != binding->collation_uuid ||
          binding->resource_epoch == 0 || binding->collation_epoch == 0 ||
          !binding->text_seed.active ||
          binding->text_seed.seed_pack_name.empty() ||
          binding->text_seed.seed_pack_version.empty() ||
          binding->text_seed.charset_name.empty() ||
          binding->text_seed.collation_name.empty()) {
        return refuse("bound set-operation collation authority is incomplete",
                      "QOW-DIAG-QRY-016-NULL-COLLATION-REFUSAL-V1");
      }
    }
  }
  if (request.maximum_output_row_count == 0) {
    return refuse("set-operation output resource bound is zero");
  }
  if (!distinct && request.operation == CanonicalSetOperationKind::kUnion &&
      (left_batch.rows.size() > request.maximum_output_row_count ||
       right_batch.rows.size() >
           request.maximum_output_row_count -
               left_batch.rows.size())) {
    return refuse("UNION ALL output resource bound was exceeded");
  }

  using RowKey = std::vector<std::string>;
  const auto measure_row_key = [&](const RowKey& key,
                                   std::size_t* payload_bytes,
                                   std::size_t* inner_structural_bytes) {
    if (payload_bytes == nullptr || inner_structural_bytes == nullptr ||
        key.size() > std::numeric_limits<std::size_t>::max() /
                         sizeof(std::string)) {
      return false;
    }
    *payload_bytes = 0;
    *inner_structural_bytes = key.size() * sizeof(std::string);
    for (const auto& field : key) {
      if (field.size() > std::numeric_limits<std::size_t>::max() -
                             *payload_bytes) {
        return false;
      }
      *payload_bytes += field.size();
    }
    return true;
  };
  const auto append_key_field = [](std::string* encoded,
                                   const std::string_view field) {
    encoded->append(std::to_string(field.size()));
    encoded->push_back(':');
    encoded->append(field);
  };
  const auto typed_row_key = [&](const DescriptorTuple& row,
                                 RowKey* key,
                                 std::string* detail) {
    if (key == nullptr || detail == nullptr) return false;
    key->clear();
    key->reserve(row.values.size());
    for (const auto& value : row.values) {
      if (value.state == api::sql_null) {
        key->push_back("state:null");
        continue;
      }
      const auto& type = value.descriptor.canonical_type_name;
      if (type == "int64") {
        const auto decoded = DecodeInt64Value(value);
        if (!decoded.ok()) {
          *detail = decoded.diagnostic.diagnostic_code + ":" +
                    decoded.diagnostic.detail;
          return false;
        }
        key->push_back("int64:" + std::to_string(decoded.value));
      } else if (type == "boolean" || type == "bool") {
        const auto decoded = DecodeBoolValue(value);
        if (!decoded.ok()) {
          *detail = decoded.diagnostic.diagnostic_code + ":" +
                    decoded.diagnostic.detail;
          return false;
        }
        key->push_back(decoded.value ? "boolean:1" : "boolean:0");
      } else if (type == "real64" || type == "double" ||
                 type == "double precision") {
        const auto decoded = DecodeReal64Value(value);
        if (!decoded.ok()) {
          *detail = decoded.diagnostic.diagnostic_code + ":" +
                    decoded.diagnostic.detail;
          return false;
        }
        if (decoded.value == 0.0) {
          key->push_back("real64:0");
        } else {
          char buffer[128]{};
          const auto encoded = std::to_chars(
              std::begin(buffer), std::end(buffer), decoded.value,
              std::chars_format::general,
              std::numeric_limits<double>::max_digits10);
          if (encoded.ec != std::errc{}) {
            *detail = "real64 set-operation key encoding failed";
            return false;
          }
          key->push_back("real64:" +
                         std::string(buffer, encoded.ptr));
        }
      } else {
        const auto binary = value.binary_value.empty()
                                ? std::string_view{}
                                : std::string_view(
                                      reinterpret_cast<const char*>(
                                          value.binary_value.data()),
                                      value.binary_value.size());
        std::string encoded = "value:";
        append_key_field(&encoded, type);
        if (type == "binary" || type == "blob" || type == "bytes") {
          append_key_field(&encoded,
                           value.binary_value.empty()
                               ? std::string_view(value.encoded_value)
                               : binary);
        } else {
          append_key_field(&encoded, value.encoded_value);
          append_key_field(&encoded, binary);
        }
        key->push_back(std::move(encoded));
      }
    }
    return true;
  };
  std::vector<RowKey> left_keys;
  std::vector<RowKey> right_keys;
  std::size_t left_key_outer_structural_bytes = 0;
  std::size_t right_key_outer_structural_bytes = 0;
  std::size_t left_key_inner_structural_bytes = 0;
  std::size_t right_key_inner_structural_bytes = 0;
  std::size_t left_key_payload_bytes = 0;
  std::size_t right_key_payload_bytes = 0;
  if (memory_ledger.enforced) {
    if (reconciled_left.rows.size() >
            std::numeric_limits<std::size_t>::max() / sizeof(RowKey) ||
        reconciled_right.rows.size() >
            std::numeric_limits<std::size_t>::max() / sizeof(RowKey)) {
      memory_ledger.refusal_detail =
          "set-operation row-key slot size overflowed";
      return refuse_memory();
    }
    left_key_outer_structural_bytes =
        reconciled_left.rows.size() * sizeof(RowKey);
    right_key_outer_structural_bytes =
        reconciled_right.rows.size() * sizeof(RowKey);
    if (!memory_ledger.RetainStructural(
            left_key_outer_structural_bytes,
            "before retaining the left row-key slots") ||
        !memory_ledger.RetainStructural(
            right_key_outer_structural_bytes,
            "before retaining the right row-key slots")) {
      return refuse_memory();
    }
  }
  left_keys.reserve(reconciled_left.rows.size());
  right_keys.reserve(reconciled_right.rows.size());
  std::string key_detail;
  for (const auto& row : reconciled_left.rows) {
    RowKey key;
    if (!typed_row_key(row, &key, &key_detail)) {
      return refuse(std::move(key_detail));
    }
    if (memory_ledger.enforced) {
      std::size_t key_payload_bytes = 0;
      std::size_t key_structural_bytes = 0;
      if (!measure_row_key(key, &key_payload_bytes,
                           &key_structural_bytes) ||
          key_payload_bytes >
              std::numeric_limits<std::size_t>::max() -
                  left_key_payload_bytes ||
          key_structural_bytes >
              std::numeric_limits<std::size_t>::max() -
                  left_key_inner_structural_bytes) {
        memory_ledger.refusal_detail =
            "set-operation left row-key size overflowed";
        return refuse_memory();
      }
      if (!memory_ledger.RetainPayload(
              key_payload_bytes,
              "while retaining a left row key") ||
          !memory_ledger.RetainStructural(
              key_structural_bytes,
              "while retaining a left row-key field slot")) {
        return refuse_memory();
      }
      left_key_payload_bytes += key_payload_bytes;
      left_key_inner_structural_bytes += key_structural_bytes;
    }
    left_keys.push_back(std::move(key));
  }
  for (const auto& row : reconciled_right.rows) {
    RowKey key;
    if (!typed_row_key(row, &key, &key_detail)) {
      return refuse(std::move(key_detail));
    }
    if (memory_ledger.enforced) {
      std::size_t key_payload_bytes = 0;
      std::size_t key_structural_bytes = 0;
      if (!measure_row_key(key, &key_payload_bytes,
                           &key_structural_bytes) ||
          key_payload_bytes >
              std::numeric_limits<std::size_t>::max() -
                  right_key_payload_bytes ||
          key_structural_bytes >
              std::numeric_limits<std::size_t>::max() -
                  right_key_inner_structural_bytes) {
        memory_ledger.refusal_detail =
            "set-operation right row-key size overflowed";
        return refuse_memory();
      }
      if (!memory_ledger.RetainPayload(
              key_payload_bytes,
              "while retaining a right row key") ||
          !memory_ledger.RetainStructural(
              key_structural_bytes,
              "while retaining a right row-key field slot")) {
        return refuse_memory();
      }
      right_key_payload_bytes += key_payload_bytes;
      right_key_inner_structural_bytes += key_structural_bytes;
    }
    right_keys.push_back(std::move(key));
  }

  std::size_t equality_comparison_count = 0;
  const auto replace_key_payload =
      [&](std::string* field, std::string replacement,
          std::size_t* component_payload_bytes) {
        if (field == nullptr || component_payload_bytes == nullptr) {
          return false;
        }
        if (memory_ledger.enforced) {
          const auto old_bytes = field->size();
          const auto new_bytes = replacement.size();
          if (old_bytes > *component_payload_bytes ||
              new_bytes > std::numeric_limits<std::size_t>::max() -
                              (*component_payload_bytes - old_bytes) ||
              !memory_ledger.ReplacePayload(
                  old_bytes, new_bytes,
                  "while replacing a bound-collation row key")) {
            if (memory_ledger.refusal_detail.empty()) {
              memory_ledger.refusal_detail =
                  "set-operation collation key payload receipt drifted";
            }
            return false;
          }
          *component_payload_bytes =
              *component_payload_bytes - old_bytes + new_bytes;
        }
        *field = std::move(replacement);
        return true;
      };
  if (bound_collation) {
    namespace dt = scratchbird::core::datatypes;
    for (std::size_t column = 0; column < request.result_columns.size();
         ++column) {
      const auto* binding = collation_by_column[column];
      if (binding == nullptr) continue;
      std::vector<const scratchbird::engine::internal_api::EngineTypedValue*>
          representatives;
      std::size_t representative_structural_bytes = 0;
      const auto classify = [&](const auto& value,
                                std::string* equality_key) {
        if (equality_key == nullptr) return false;
        if (value.state == api::sql_null) {
          *equality_key = "collation:null";
          return true;
        }
        for (std::size_t index = 0; index < representatives.size(); ++index) {
          if (equality_comparison_count >=
              request.maximum_equality_comparison_count) {
            key_detail = "collation equality comparison bound was exceeded";
            return false;
          }
          ++equality_comparison_count;
          dt::DatatypeComparisonRequest comparison_request;
          comparison_request.left.type_id = dt::CanonicalTypeId::character;
          comparison_request.left.encoded_value =
              representatives[index]->encoded_value;
          comparison_request.right.type_id = dt::CanonicalTypeId::character;
          comparison_request.right.encoded_value = value.encoded_value;
          comparison_request.case_insensitive_character_compare =
              binding->text_seed.collation_case_insensitive;
          comparison_request.text_seed = binding->text_seed;
          const auto compared = dt::CompareDatatypeValues(comparison_request);
          if (!compared.ok()) {
            key_detail = compared.diagnostic.diagnostic_code.empty()
                             ? "bound collation comparison refused"
                             : compared.diagnostic.diagnostic_code;
            return false;
          }
          if (compared.comparison == 0) {
            *equality_key = "collation:" + std::to_string(index);
            return true;
          }
        }
        if (memory_ledger.enforced) {
          constexpr std::size_t kRepresentativeBytes =
              sizeof(const scratchbird::engine::internal_api::
                         EngineTypedValue*);
          if (kRepresentativeBytes >
                  std::numeric_limits<std::size_t>::max() -
                      representative_structural_bytes ||
              !memory_ledger.RetainStructural(
                  sizeof(const scratchbird::engine::internal_api::
                             EngineTypedValue*),
                  "while retaining a collation representative")) {
            return false;
          }
          representative_structural_bytes += kRepresentativeBytes;
        }
        representatives.push_back(&value);
        *equality_key =
            "collation:" + std::to_string(representatives.size() - 1);
        return true;
      };
      for (std::size_t row = 0; row < reconciled_left.rows.size(); ++row) {
        std::string equality_key;
        if (!classify(reconciled_left.rows[row].values[column],
                      &equality_key) ||
            !replace_key_payload(&left_keys[row][column],
                                 std::move(equality_key),
                                 &left_key_payload_bytes)) {
          if (!memory_ledger.refusal_detail.empty()) return refuse_memory();
          return refuse(std::move(key_detail),
                        "QOW-DIAG-QRY-016-NULL-COLLATION-REFUSAL-V1");
        }
      }
      for (std::size_t row = 0; row < reconciled_right.rows.size(); ++row) {
        std::string equality_key;
        if (!classify(reconciled_right.rows[row].values[column],
                      &equality_key) ||
            !replace_key_payload(&right_keys[row][column],
                                 std::move(equality_key),
                                 &right_key_payload_bytes)) {
          if (!memory_ledger.refusal_detail.empty()) return refuse_memory();
          return refuse(std::move(key_detail),
                        "QOW-DIAG-QRY-016-NULL-COLLATION-REFUSAL-V1");
        }
      }
      if (!memory_ledger.ReleaseStructural(
              representative_structural_bytes,
              "after releasing collation representatives")) {
        return refuse_memory();
      }
    }
  }

  enum class SetEmitStatus : std::uint8_t {
    kOk = 1,
    kOutputBound,
    kMemoryRefusal,
  };
  DescriptorBatch output;
  output.columns = request.result_columns;
  std::size_t output_payload_bytes = 0;
  if (memory_ledger.enforced) {
    output_payload_bytes = 1;
    if (!memory_ledger.RetainPayload(
            output_payload_bytes,
            "while retaining the output batch")) {
      return refuse_memory();
    }
  }
  const auto emit_row = [&](const DescriptorTuple& source) {
    if (output.rows.size() >= request.maximum_output_row_count) {
      return SetEmitStatus::kOutputBound;
    }
    if (memory_ledger.enforced) {
      std::size_t row_payload_bytes = 0;
      if (!FoundationTuplePayloadBytes(source, &row_payload_bytes) ||
          row_payload_bytes >
              std::numeric_limits<std::size_t>::max() -
                  output_payload_bytes) {
        memory_ledger.refusal_detail =
            "set-operation output payload size overflowed";
        return SetEmitStatus::kMemoryRefusal;
      }
      if (!memory_ledger.RetainPayload(
              row_payload_bytes,
              "while retaining an output row")) {
        return SetEmitStatus::kMemoryRefusal;
      }
      output_payload_bytes += row_payload_bytes;
    }
    output.rows.push_back(source);
    auto& output_row = output.rows.back();
    for (std::size_t column = 0; column < output_row.values.size(); ++column) {
      output_row.values[column].descriptor =
          request.result_columns[column].descriptor;
    }
    return SetEmitStatus::kOk;
  };
  const auto retain_branch_key =
      [&](const RowKey& key, const std::size_t mapped_structural_bytes,
          std::size_t* branch_payload_bytes,
          std::size_t* branch_structural_bytes) {
        if (!memory_ledger.enforced) return true;
        std::size_t key_payload_bytes = 0;
        std::size_t key_inner_structural_bytes = 0;
        if (branch_payload_bytes == nullptr ||
            branch_structural_bytes == nullptr ||
            !measure_row_key(key, &key_payload_bytes,
                             &key_inner_structural_bytes) ||
            key_inner_structural_bytes >
                std::numeric_limits<std::size_t>::max() - sizeof(RowKey) ||
            mapped_structural_bytes >
                std::numeric_limits<std::size_t>::max() -
                    sizeof(RowKey) - key_inner_structural_bytes) {
          memory_ledger.refusal_detail =
              "set-operation branch key size overflowed";
          return false;
        }
        const auto key_structural_bytes =
            sizeof(RowKey) + key_inner_structural_bytes +
            mapped_structural_bytes;
        if (key_payload_bytes >
                std::numeric_limits<std::size_t>::max() -
                    *branch_payload_bytes ||
            key_structural_bytes >
                std::numeric_limits<std::size_t>::max() -
                    *branch_structural_bytes) {
          memory_ledger.refusal_detail =
              "set-operation branch state size overflowed";
          return false;
        }
        if (!memory_ledger.RetainPayload(
                key_payload_bytes,
                "while retaining a branch membership key") ||
            !memory_ledger.RetainStructural(
                key_structural_bytes,
                "while retaining a branch membership slot")) {
          return false;
        }
        *branch_payload_bytes += key_payload_bytes;
        *branch_structural_bytes += key_structural_bytes;
        return true;
      };
  const auto release_branch_state =
      [&](const std::size_t branch_payload_bytes,
          const std::size_t branch_structural_bytes) {
        return memory_ledger.ReleasePayload(
                   branch_payload_bytes,
                   "after releasing branch membership keys") &&
               memory_ledger.ReleaseStructural(
                   branch_structural_bytes,
                   "after releasing branch membership slots");
      };
  std::size_t consumed_right_multiplicity_count = 0;
  std::size_t eliminated_duplicate_row_count = 0;

  // QOW-SOURCE-QRY-016-DISTINCT-V1
  // DISTINCT uses the same fully decoded typed row keys as ALL. It validates
  // every input row before eliminating duplicates, so a duplicate position
  // cannot hide malformed transport or value state.
  if (distinct && request.operation == CanonicalSetOperationKind::kUnion) {
    std::set<RowKey> emitted;
    std::size_t branch_payload_bytes = 0;
    std::size_t branch_structural_bytes = 0;
    for (std::size_t index = 0; index < left_keys.size(); ++index) {
      if (!emitted.contains(left_keys[index])) {
        if (!retain_branch_key(left_keys[index], 0,
                               &branch_payload_bytes,
                               &branch_structural_bytes)) {
          return refuse_memory();
        }
        emitted.insert(left_keys[index]);
        const auto emitted_row = emit_row(reconciled_left.rows[index]);
        if (emitted_row == SetEmitStatus::kMemoryRefusal) {
          return refuse_memory();
        }
        if (emitted_row == SetEmitStatus::kOutputBound) {
          return refuse("set-operation output resource bound was exceeded");
        }
      } else {
        ++eliminated_duplicate_row_count;
      }
    }
    for (std::size_t index = 0; index < right_keys.size(); ++index) {
      if (!emitted.contains(right_keys[index])) {
        if (!retain_branch_key(right_keys[index], 0,
                               &branch_payload_bytes,
                               &branch_structural_bytes)) {
          return refuse_memory();
        }
        emitted.insert(right_keys[index]);
        const auto emitted_row = emit_row(reconciled_right.rows[index]);
        if (emitted_row == SetEmitStatus::kMemoryRefusal) {
          return refuse_memory();
        }
        if (emitted_row == SetEmitStatus::kOutputBound) {
          return refuse("set-operation output resource bound was exceeded");
        }
      } else {
        ++eliminated_duplicate_row_count;
      }
    }
    emitted.clear();
    if (!release_branch_state(branch_payload_bytes,
                              branch_structural_bytes)) {
      return refuse_memory();
    }
  } else if (distinct) {
    std::set<RowKey> right_membership;
    std::set<RowKey> emitted;
    std::size_t branch_payload_bytes = 0;
    std::size_t branch_structural_bytes = 0;
    for (const auto& key : right_keys) {
      if (right_membership.contains(key)) continue;
      if (!retain_branch_key(key, 0, &branch_payload_bytes,
                             &branch_structural_bytes)) {
        return refuse_memory();
      }
      right_membership.insert(key);
    }
    for (std::size_t index = 0; index < left_keys.size(); ++index) {
      const bool present_on_right =
          right_membership.contains(left_keys[index]);
      const bool candidate =
          request.operation == CanonicalSetOperationKind::kIntersect
              ? present_on_right
              : !present_on_right;
      if (!candidate) continue;
      if (!emitted.contains(left_keys[index])) {
        if (!retain_branch_key(left_keys[index], 0,
                               &branch_payload_bytes,
                               &branch_structural_bytes)) {
          return refuse_memory();
        }
        emitted.insert(left_keys[index]);
        const auto emitted_row = emit_row(reconciled_left.rows[index]);
        if (emitted_row == SetEmitStatus::kMemoryRefusal) {
          return refuse_memory();
        }
        if (emitted_row == SetEmitStatus::kOutputBound) {
          return refuse("set-operation output resource bound was exceeded");
        }
      } else {
        ++eliminated_duplicate_row_count;
      }
    }
    right_membership.clear();
    emitted.clear();
    if (!release_branch_state(branch_payload_bytes,
                              branch_structural_bytes)) {
      return refuse_memory();
    }
  } else if (request.operation == CanonicalSetOperationKind::kUnion) {
    output.rows.reserve(left_keys.size() + right_keys.size());
    for (const auto& row : reconciled_left.rows) {
      const auto emitted_row = emit_row(row);
      if (emitted_row == SetEmitStatus::kMemoryRefusal) {
        return refuse_memory();
      }
      if (emitted_row == SetEmitStatus::kOutputBound) {
        return refuse("set-operation output resource bound was exceeded");
      }
    }
    for (const auto& row : reconciled_right.rows) {
      const auto emitted_row = emit_row(row);
      if (emitted_row == SetEmitStatus::kMemoryRefusal) {
        return refuse_memory();
      }
      if (emitted_row == SetEmitStatus::kOutputBound) {
        return refuse("set-operation output resource bound was exceeded");
      }
    }
  } else {
    std::map<RowKey, std::size_t> right_multiplicity;
    std::size_t branch_payload_bytes = 0;
    std::size_t branch_structural_bytes = 0;
    for (const auto& key : right_keys) {
      auto found = right_multiplicity.find(key);
      if (found == right_multiplicity.end()) {
        if (!retain_branch_key(key, sizeof(std::size_t),
                               &branch_payload_bytes,
                               &branch_structural_bytes)) {
          return refuse_memory();
        }
        right_multiplicity.emplace(key, 1);
      } else {
        ++found->second;
      }
    }
    for (std::size_t index = 0; index < left_keys.size(); ++index) {
      auto found = right_multiplicity.find(left_keys[index]);
      const bool consumes =
          found != right_multiplicity.end() && found->second != 0;
      if (consumes) {
        --found->second;
        ++consumed_right_multiplicity_count;
      }
      const bool emit =
          request.operation == CanonicalSetOperationKind::kIntersect
              ? consumes
              : !consumes;
      if (emit) {
        const auto emitted_row = emit_row(reconciled_left.rows[index]);
        if (emitted_row == SetEmitStatus::kMemoryRefusal) {
          return refuse_memory();
        }
        if (emitted_row == SetEmitStatus::kOutputBound) {
          return refuse("set-operation output resource bound was exceeded");
        }
      }
    }
    right_multiplicity.clear();
    if (!release_branch_state(branch_payload_bytes,
                              branch_structural_bytes)) {
      return refuse_memory();
    }
  }

  std::vector<RowKey>().swap(left_keys);
  std::vector<RowKey>().swap(right_keys);
  if (!memory_ledger.ReleasePayload(
          left_key_payload_bytes,
          "after releasing left row-key payload") ||
      !memory_ledger.ReleasePayload(
          right_key_payload_bytes,
          "after releasing right row-key payload") ||
      !memory_ledger.ReleaseStructural(
          left_key_inner_structural_bytes,
          "after releasing left row-key field slots") ||
      !memory_ledger.ReleaseStructural(
          right_key_inner_structural_bytes,
          "after releasing right row-key field slots") ||
      !memory_ledger.ReleaseStructural(
          left_key_outer_structural_bytes,
          "after releasing left row-key outer slots") ||
      !memory_ledger.ReleaseStructural(
          right_key_outer_structural_bytes,
          "after releasing right row-key outer slots")) {
    return refuse_memory();
  }
  std::vector<const CanonicalSetOperationCollationBinding*>().swap(
      collation_by_column);
  if (!memory_ledger.ReleaseStructural(
          collation_lookup_structural_bytes,
          "after releasing the collation lookup")) {
    return refuse_memory();
  }
  reconciled_left_storage.reset();
  if (!memory_ledger.ReleasePayload(
          reconciled_left_payload_bytes,
          "after releasing the reconciled left operand")) {
    return refuse_memory();
  }
  reconciled_ordinal_right_storage.reset();
  if (!memory_ledger.ReleasePayload(
          reconciled_ordinal_right_payload_bytes,
          "after releasing the reconciled right operand")) {
    return refuse_memory();
  }
  aligned_right_storage.reset();
  if (!memory_ledger.ReleasePayload(
          aligned_right_payload_bytes,
          "after releasing the BY NAME aligned right operand") ||
      !memory_ledger.ReleasePayload(
          left_input_payload_bytes,
          "after consuming the left operand") ||
      !memory_ledger.ReleasePayload(
          right_input_payload_bytes,
          "after consuming the right operand")) {
    return refuse_memory();
  }
  if (memory_ledger.enforced &&
      (memory_ledger.live_structural_bytes != 0 ||
       memory_ledger.live_payload_bytes != output_payload_bytes)) {
    memory_ledger.refusal_detail =
        "set-operation final logical memory receipt drifted";
    return refuse_memory();
  }
  if (output.rows.size() > request.maximum_output_row_count) {
    return refuse("set-operation output resource bound was exceeded");
  }
  validation = ValidateCanonicalDescriptorBatch(
      output, selected_node->output_descriptor_ids);
  if (!validation.ok) {
    return refuse(validation.diagnostic_code + ":" + validation.detail);
  }
  if (memory_ledger.enforced) {
    std::size_t measured_output_payload_bytes = 0;
    if (!FoundationBatchPayloadBytes(
            output, &measured_output_payload_bytes) ||
        measured_output_payload_bytes != output_payload_bytes ||
        memory_ledger.peak_payload_bytes < output_payload_bytes ||
        memory_ledger.peak_memory_bytes <
            memory_ledger.live_payload_bytes ||
        memory_ledger.peak_memory_bytes > memory_grant_bytes) {
      memory_ledger.refusal_detail =
          "set-operation final logical memory observation is invalid";
      return refuse_memory();
    }
  }

  const auto result_authority = RevalidateCanonicalExecutionMgaAuthority(
      request.mga_authority, request.physical_dag);
  if (!result_authority.ok)
    return refuse(result_authority.diagnostic_code + ":" +
                  result_authority.detail);

  result.diagnostic = {};
  result.output_batch = std::move(output);
  result.left_input_row_count = left_batch.rows.size();
  result.right_input_row_count = right_batch.rows.size();
  result.consumed_right_multiplicity_count =
      consumed_right_multiplicity_count;
  result.eliminated_duplicate_row_count =
      eliminated_duplicate_row_count;
  result.equality_comparison_count = equality_comparison_count;
  result.coerced_value_count = coerced_value_count;
  result.reconciled_type_names = std::move(reconciled_type_names);
  result.right_to_result_column_indices =
      std::move(right_to_result_column_indices);
  result.implementation_id = expected_implementation;
  result.selected_plan_uuid = request.physical_dag.selected_plan_uuid;
  result.executed_physical_node_id = selected_node->physical_node_id;
  result.causal_counter_id = selected_node->causal_counter_id;
  result.mga_statement_context = request.mga_authority.statement_context;
  if (memory_ledger.enforced) {
    result.output_payload_bytes = output_payload_bytes;
    result.peak_live_payload_bytes =
        memory_ledger.peak_payload_bytes;
    result.resident_structural_bytes =
        memory_ledger.live_structural_bytes;
    result.current_live_memory_bytes =
        memory_ledger.live_payload_bytes +
        memory_ledger.live_structural_bytes;
    result.peak_live_memory_bytes =
        memory_ledger.peak_memory_bytes;
    result.memory_grant_bytes = memory_grant_bytes;
    result.memory_grant_evidence_uuid =
        std::move(memory_grant_evidence_uuid);
  }
  return result;
}

// QOW-SOURCE-QRY-016-ALL-V1
CanonicalSetOperationAllResult ExecuteCanonicalSetOperationAll(
    const CanonicalSetOperationAllRequest& request) {
  return ExecuteCanonicalSetOperationQuantified(
      request, CanonicalSetOperationQuantifier::kAll);
}

CanonicalSetOperationAllResult ExecuteCanonicalSetOperationDistinct(
    const CanonicalSetOperationAllRequest& request) {
  return ExecuteCanonicalSetOperationQuantified(
      request, CanonicalSetOperationQuantifier::kDistinct);
}

// QOW-SOURCE-QRY-019-PIVOT-V1
// Reshape one optimizer-selected typed input only after grouping/FOR equality,
// aggregate descriptors, fixed IN keys, result descriptors, resource bounds,
// and MGA authority have all been bound. Aggregate state is delegated to the
// canonical global aggregate registry through private engine-owned aggregate
// requests; PIVOT owns only grouping, cell selection, and reshaping.
CanonicalPivotResult ExecuteCanonicalPivot(
    const CanonicalPivotRequest& request) {
  namespace api = scratchbird::engine::internal_api;
  CanonicalPivotResult result;
  const auto refuse = [&](std::string code, std::string detail) {
    result = {};
    result.diagnostic.ok = false;
    result.diagnostic.diagnostic_code = std::move(code);
    result.diagnostic.detail = std::move(detail);
    return result;
  };

  const auto authority = RevalidateCanonicalExecutionMgaAuthority(
      request.mga_authority, request.physical_dag);
  if (!authority.ok) {
    return refuse(authority.diagnostic_code, authority.detail);
  }
  const PhysicalNodeRecord* pivot_node = nullptr;
  for (const auto& node : request.physical_dag.nodes) {
    if (node.physical_node_id == request.selected_physical_node_id) {
      pivot_node = &node;
      break;
    }
  }
  const std::string expected_implementation =
      request.null_policy == CanonicalPivotNullPolicy::kInclude
          ? "pivot.canonical.include-nulls.typed.v1"
          : "pivot.canonical.exclude-nulls.typed.v1";
  if (pivot_node == nullptr ||
      request.selected_physical_node_id !=
          request.physical_dag.root_physical_node_id ||
      pivot_node->node_kind != PhysicalNodeKind::kPivot ||
      pivot_node->input_physical_node_ids.size() != 1 ||
      pivot_node->implementation_id != expected_implementation) {
    return refuse("QOW-DIAG-QRY-019-PIVOT-PHYSICAL-V1",
                  "PIVOT is not the exact selected unary physical root");
  }
  const PhysicalNodeRecord* pivot_input_node = nullptr;
  for (const auto& node : request.physical_dag.nodes) {
    if (node.physical_node_id == pivot_node->input_physical_node_ids.front()) {
      pivot_input_node = &node;
      break;
    }
  }
  if (pivot_input_node == nullptr) {
    return refuse("QOW-DIAG-QRY-019-PIVOT-PHYSICAL-V1",
                  "PIVOT selected input physical node is absent");
  }
  if (request.null_policy != CanonicalPivotNullPolicy::kInclude &&
      request.null_policy != CanonicalPivotNullPolicy::kExclude) {
    return refuse("QOW-DIAG-QRY-019-PIVOT-BINDING-V1",
                  "PIVOT NULL policy is not canonical");
  }
  if (request.group_key_terms.empty() || request.for_key_terms.empty() ||
      request.in_items.empty() || request.aggregates.empty() ||
      request.maximum_key_comparison_count == 0 ||
      request.maximum_total_aggregate_transition_count == 0 ||
      request.maximum_output_row_count == 0 ||
      request.maximum_output_cell_count == 0) {
    return refuse("QOW-DIAG-QRY-019-PIVOT-BINDING-V1",
                  "PIVOT grouping, FOR, aggregate, IN, or resource binding is absent");
  }
  if (request.aggregates.size() >
          (std::numeric_limits<std::size_t>::max() -
           request.group_key_terms.size()) /
              request.in_items.size()) {
    return refuse("SBLR.PLAN_TREE.RESOURCE_LIMIT",
                  "PIVOT result width overflowed");
  }
  const auto expected_width =
      request.group_key_terms.size() +
      request.in_items.size() * request.aggregates.size();
  if (request.result_columns.size() != expected_width ||
      pivot_node->output_descriptor_ids.size() != expected_width) {
    return refuse("QOW-DIAG-QRY-019-PIVOT-DESCRIPTOR-V1",
                  "PIVOT result descriptor width is inconsistent");
  }
  for (std::size_t column = 0; column < expected_width; ++column) {
    if (request.result_columns[column].descriptor_id == 0 ||
        request.result_columns[column].descriptor_id !=
            pivot_node->output_descriptor_ids[column]) {
      return refuse("QOW-DIAG-QRY-019-PIVOT-DESCRIPTOR-V1",
                    "PIVOT result descriptor order is not exact");
    }
  }
  const auto input_validation = ValidateCanonicalDescriptorBatch(
      request.input_batch, pivot_input_node->output_descriptor_ids);
  if (!input_validation.ok) {
    return refuse(input_validation.diagnostic_code,
                  input_validation.detail);
  }
  const auto node_memory_grant = FoundationNodeMemoryGrant(
      request.physical_dag, request.selected_physical_node_id);
  std::size_t input_payload_bytes = 0;
  if (!node_memory_grant.has_value() ||
      !FoundationBatchPayloadBytes(request.input_batch,
                                   &input_payload_bytes) ||
      input_payload_bytes > *node_memory_grant) {
    return refuse("SBLR.PLAN_TREE.RESOURCE_LIMIT",
                  "PIVOT selected-node memory grant is invalid or exhausted");
  }
  const auto maximum_combined_final_output_bytes =
      request.maximum_combined_final_output_bytes == 0
          ? *node_memory_grant
          : std::min(request.maximum_combined_final_output_bytes,
                     *node_memory_grant);

  std::set<std::size_t> key_columns;
  const auto validate_terms = [&](const auto& terms,
                                  const std::string_view role) {
    for (const auto& term : terms) {
      if (term.column >= request.input_batch.columns.size() ||
          !key_columns.insert(term.column).second) {
        return std::string(role) +
               " key column is out of range or duplicated";
      }
      const auto validated = ValidateCanonicalDescriptorOrderTerm(
          term, request.input_batch.columns[term.column]);
      if (!validated.ok) {
        return std::string(role) + " key is invalid: " +
               validated.diagnostic_code + ":" + validated.detail;
      }
    }
    return std::string{};
  };
  if (const auto detail = validate_terms(request.group_key_terms, "PIVOT group");
      !detail.empty()) {
    return refuse("QOW-DIAG-QRY-019-PIVOT-GROUP-V1", detail);
  }
  if (const auto detail = validate_terms(request.for_key_terms, "PIVOT FOR");
      !detail.empty()) {
    return refuse("QOW-DIAG-QRY-019-PIVOT-FOR-V1", detail);
  }

  for (const auto& item : request.in_items) {
    if (item.values.size() != request.for_key_terms.size()) {
      return refuse("QOW-DIAG-QRY-019-PIVOT-IN-V1",
                    "PIVOT IN tuple arity differs from FOR arity");
    }
  }
  for (std::size_t aggregate = 0; aggregate < request.aggregates.size();
       ++aggregate) {
    const auto& binding = request.aggregates[aggregate];
    const auto& aggregate_request = binding.aggregate_template;
    if (!aggregate_request.physical_dag.nodes.empty() ||
        aggregate_request.selected_physical_node_id != 0 ||
        aggregate_request.retained_memory_bytes != 0 ||
        !aggregate_request.input_batch.columns.empty() ||
        !aggregate_request.input_batch.rows.empty() ||
        aggregate_request.result_column.descriptor_id != 0 ||
        binding.result_columns_by_item.size() != request.in_items.size()) {
      return refuse("QOW-DIAG-QRY-019-PIVOT-AGGREGATE-V1",
                    "PIVOT aggregate template carries execution authority or incomplete results");
    }
    for (std::size_t item = 0; item < request.in_items.size(); ++item) {
      const auto result_column = request.group_key_terms.size() +
                                 item * request.aggregates.size() +
                                 aggregate;
      if (binding.result_columns_by_item[item].descriptor_id !=
          request.result_columns[result_column].descriptor_id) {
        return refuse("QOW-DIAG-QRY-019-PIVOT-DESCRIPTOR-V1",
                      "PIVOT aggregate result descriptor mapping drifted");
      }
    }
  }
  std::size_t source_filter_bytes = 0;
  for (const auto& binding : request.aggregates) {
    if (!binding.aggregate_template.filter_truth_values.has_value()) continue;
    if (binding.aggregate_template.filter_truth_values->size() !=
        request.input_batch.rows.size()) {
      return refuse("QOW-DIAG-QRY-019-PIVOT-AGGREGATE-V1",
                    "PIVOT aggregate FILTER cardinality is not bound to the input");
    }
    std::size_t filter_bytes = 0;
    if (!FoundationFilterBytes(
            binding.aggregate_template.filter_truth_values->size(),
            &filter_bytes) ||
        source_filter_bytes >
            std::numeric_limits<std::size_t>::max() - filter_bytes) {
      return refuse("SBLR.PLAN_TREE.RESOURCE_LIMIT",
                    "PIVOT source FILTER memory size overflowed");
    }
    source_filter_bytes += filter_bytes;
  }
  if (source_filter_bytes > *node_memory_grant - input_payload_bytes) {
    return refuse("SBLR.PLAN_TREE.RESOURCE_LIMIT",
                  "PIVOT source FILTER memory exceeds the selected-node grant");
  }

  std::size_t key_comparisons = 0;
  const auto keys_equal = [&](const DescriptorTuple& left,
                              const DescriptorTuple& right,
                              const auto& terms,
                              bool* equal) {
    *equal = true;
    for (const auto& term : terms) {
      if (key_comparisons == request.maximum_key_comparison_count) {
        return false;
      }
      ++key_comparisons;
      const auto compared = CompareCanonicalDescriptorOrderValues(
          left.values[term.column], right.values[term.column], term);
      if (!compared.diagnostic.ok) return false;
      if (compared.comparison != 0) {
        *equal = false;
        return true;
      }
    }
    return true;
  };
  const auto row_matches_item = [&](const DescriptorTuple& row,
                                    const CanonicalPivotInItem& item,
                                    bool* matches) {
    *matches = true;
    for (std::size_t key = 0; key < request.for_key_terms.size(); ++key) {
      const auto& term = request.for_key_terms[key];
      const auto& value = row.values[term.column];
      if (request.null_policy == CanonicalPivotNullPolicy::kExclude &&
          value.state == api::EngineValueState::sql_null) {
        *matches = false;
        return true;
      }
      if (key_comparisons == request.maximum_key_comparison_count) {
        return false;
      }
      ++key_comparisons;
      const auto compared = CompareCanonicalDescriptorOrderValues(
          value, item.values[key], term);
      if (!compared.diagnostic.ok) return false;
      if (compared.comparison != 0) {
        *matches = false;
        return true;
      }
    }
    return true;
  };

  struct PivotGroup {
    std::size_t representative_row = 0;
    std::vector<std::vector<std::size_t>> item_rows;
  };
  std::vector<PivotGroup> groups;
  std::size_t matched_input_rows = 0;
  for (std::size_t row = 0; row < request.input_batch.rows.size(); ++row) {
    std::optional<std::size_t> group_index;
    for (std::size_t group = 0; group < groups.size(); ++group) {
      bool equal = false;
      if (!keys_equal(request.input_batch.rows[row],
                      request.input_batch.rows[groups[group].representative_row],
                      request.group_key_terms, &equal)) {
        return refuse("SBLR.PLAN_TREE.RESOURCE_LIMIT",
                      "PIVOT grouping comparison bound was exceeded");
      }
      if (equal) {
        group_index = group;
        break;
      }
    }
    if (!group_index.has_value()) {
      if (groups.size() == request.maximum_output_row_count) {
        return refuse("SBLR.PLAN_TREE.RESOURCE_LIMIT",
                      "PIVOT group output bound was exceeded");
      }
      PivotGroup group;
      group.representative_row = row;
      group.item_rows.resize(request.in_items.size());
      groups.push_back(std::move(group));
      group_index = groups.size() - 1;
    }
    std::optional<std::size_t> matched_item;
    for (std::size_t item = 0; item < request.in_items.size(); ++item) {
      bool matches = false;
      if (!row_matches_item(request.input_batch.rows[row],
                            request.in_items[item], &matches)) {
        return refuse("SBLR.PLAN_TREE.RESOURCE_LIMIT",
                      "PIVOT IN comparison bound was exceeded");
      }
      if (!matches) continue;
      if (matched_item.has_value()) {
        return refuse("QOW-DIAG-QRY-019-PIVOT-IN-V1",
                      "PIVOT IN items overlap under canonical equality");
      }
      matched_item = item;
    }
    if (matched_item.has_value()) {
      groups[*group_index].item_rows[*matched_item].push_back(row);
      ++matched_input_rows;
    }
  }
  if (groups.size() != 0 &&
      expected_width > request.maximum_output_cell_count / groups.size()) {
    return refuse("SBLR.PLAN_TREE.RESOURCE_LIMIT",
                  "PIVOT output cell bound was exceeded");
  }

  DescriptorBatch output;
  output.columns = request.result_columns;
  std::size_t total_transitions = 0;
  std::size_t combined_final_output_bytes = 0;
  std::size_t peak_finalization_workspace_bytes = 0;
  std::size_t retained_output_payload_bytes = 0;
  const auto charge_output_value = [&](const auto& value) {
    if (retained_output_payload_bytes >
            std::numeric_limits<std::size_t>::max() -
                value.encoded_value.size()) {
      return false;
    }
    retained_output_payload_bytes += value.encoded_value.size();
    if (retained_output_payload_bytes >
            std::numeric_limits<std::size_t>::max() -
                value.binary_value.size()) {
      return false;
    }
    retained_output_payload_bytes += value.binary_value.size();
    return retained_output_payload_bytes <=
           *node_memory_grant - input_payload_bytes - source_filter_bytes;
  };
  const auto can_materialize_output_value = [&](const auto& source) {
    std::size_t source_payload_bytes = source.encoded_value.size();
    if (source_payload_bytes >
        std::numeric_limits<std::size_t>::max() -
            source.binary_value.size()) {
      return false;
    }
    source_payload_bytes += source.binary_value.size();
    return retained_output_payload_bytes <=
               *node_memory_grant - input_payload_bytes -
                   source_filter_bytes &&
           source_payload_bytes <=
               *node_memory_grant - input_payload_bytes -
                   source_filter_bytes -
                   retained_output_payload_bytes;
  };
  for (const auto& group : groups) {
    DescriptorTuple output_row;
    output_row.values.reserve(expected_width);
    const auto& representative =
        request.input_batch.rows[group.representative_row];
    for (std::size_t key = 0; key < request.group_key_terms.size(); ++key) {
      const auto source_column = request.group_key_terms[key].column;
      if (!DescriptorMatches(representative.values[source_column].descriptor,
                             request.result_columns[key].descriptor)) {
        return refuse("QOW-DIAG-QRY-019-PIVOT-DESCRIPTOR-V1",
                      "PIVOT group key result must preserve its exact descriptor");
      }
      if (!can_materialize_output_value(
              representative.values[source_column])) {
        return refuse("SBLR.PLAN_TREE.RESOURCE_LIMIT",
                      "PIVOT group-key copy exceeds the selected-node grant");
      }
      auto value = representative.values[source_column];
      value.descriptor = request.result_columns[key].descriptor;
      if (!charge_output_value(value)) {
        return refuse("SBLR.PLAN_TREE.RESOURCE_LIMIT",
                      "PIVOT retained output exhausted the selected-node grant");
      }
      output_row.values.push_back(std::move(value));
    }
    for (std::size_t item = 0; item < request.in_items.size(); ++item) {
      std::size_t cell_input_payload_bytes = 1;
      for (const auto row : group.item_rows[item]) {
        for (const auto& value : request.input_batch.rows[row].values) {
          if (cell_input_payload_bytes >
                  std::numeric_limits<std::size_t>::max() -
                      value.encoded_value.size()) {
            return refuse("SBLR.PLAN_TREE.RESOURCE_LIMIT",
                          "PIVOT cell input payload size overflowed");
          }
          cell_input_payload_bytes += value.encoded_value.size();
          if (cell_input_payload_bytes >
                  std::numeric_limits<std::size_t>::max() -
                      value.binary_value.size()) {
            return refuse("SBLR.PLAN_TREE.RESOURCE_LIMIT",
                          "PIVOT cell input payload size overflowed");
          }
          cell_input_payload_bytes += value.binary_value.size();
        }
      }
      if (retained_output_payload_bytes >
              *node_memory_grant - input_payload_bytes -
                  source_filter_bytes ||
          cell_input_payload_bytes >
              *node_memory_grant - input_payload_bytes -
                  source_filter_bytes -
                  retained_output_payload_bytes) {
        return refuse("SBLR.PLAN_TREE.RESOURCE_LIMIT",
                      "PIVOT cell input exceeds the selected-node grant");
      }
      for (std::size_t aggregate = 0;
           aggregate < request.aggregates.size(); ++aggregate) {
        const auto& binding = request.aggregates[aggregate];
        const auto* binding_filter =
            binding.aggregate_template.filter_truth_values.has_value()
                ? &*binding.aggregate_template.filter_truth_values
                : nullptr;
        std::size_t cell_filter_bytes = 0;
        if (binding_filter != nullptr &&
            !FoundationFilterBytes(group.item_rows[item].size(),
                                   &cell_filter_bytes)) {
          return refuse("SBLR.PLAN_TREE.RESOURCE_LIMIT",
                        "PIVOT cell FILTER size overflowed");
        }
        if (retained_output_payload_bytes >
                *node_memory_grant - input_payload_bytes -
                    source_filter_bytes ||
            cell_input_payload_bytes >
                *node_memory_grant - input_payload_bytes -
                    source_filter_bytes -
                    retained_output_payload_bytes ||
            cell_filter_bytes >
                *node_memory_grant - input_payload_bytes -
                    source_filter_bytes -
                    retained_output_payload_bytes -
                    cell_input_payload_bytes) {
          return refuse(
              "SBLR.PLAN_TREE.RESOURCE_LIMIT",
              "PIVOT cell input or FILTER exceeds the selected-node grant");
        }
        auto aggregate_request =
            CloneFoundationAggregateRequestWithoutRowsOrFilter(
                binding.aggregate_template, false);
        DescriptorBatch aggregate_input;
        aggregate_input.columns = request.input_batch.columns;
        aggregate_input.rows.reserve(group.item_rows[item].size());
        for (const auto row : group.item_rows[item]) {
          aggregate_input.rows.push_back(request.input_batch.rows[row]);
        }
        aggregate_request.result_column =
            binding.result_columns_by_item[item];
        aggregate_request.mga_authority = request.mga_authority;
        const auto remaining_finalization_bytes =
            maximum_combined_final_output_bytes -
            combined_final_output_bytes;
        aggregate_request.retained_memory_bytes =
            input_payload_bytes + retained_output_payload_bytes +
            source_filter_bytes;
        if (binding_filter != nullptr) {
          if (binding_filter->size() !=
              request.input_batch.rows.size()) {
            return refuse("QOW-DIAG-QRY-019-PIVOT-AGGREGATE-V1",
                          "PIVOT aggregate FILTER cardinality is not bound to the input");
          }
          std::vector<api::EngineSqlTruthValue> cell_filter;
          cell_filter.reserve(group.item_rows[item].size());
          for (const auto row : group.item_rows[item]) {
            cell_filter.push_back((*binding_filter)[row]);
          }
          aggregate_request.filter_truth_values = std::move(cell_filter);
        }

        TypedPhysicalNodeDag aggregate_dag = request.physical_dag;
        aggregate_dag.nodes.clear();
        // QOW-SOURCE-RCP-065-PRIVATE-KERNEL-PUBLICATION-SEPARATION-V1
        // This is a private aggregate-kernel execution view, not a second
        // publication of the optimizer-selected PIVOT DAG. Preserve the ABI-v2
        // statement admission/MGA authority, but do not copy the complete
        // publication receipt onto nodes whose kind and identity are derived
        // for the private kernel. The immutable outer DAG remains the sole
        // complete publication and is revalidated before result publication.
        aggregate_dag.publication_contract_version = 0;
        aggregate_dag.selected_plan_signature.clear();
        aggregate_dag.selected_scalar_score = 0;
        aggregate_dag.published_node_count = 0;
        aggregate_dag.first_causal_counter_id = 0;
        aggregate_dag.complete_cost_vectors_retained = false;
        aggregate_dag.descriptor_contract_validated = false;
        aggregate_dag.property_contract_validated = false;
        aggregate_dag.dependency_contract_validated = false;
        aggregate_dag.resource_contract_validated = false;
        aggregate_dag.mga_contract_validated = false;
        aggregate_dag.causal_identity_validated = false;
        PhysicalNodeRecord input_node = *pivot_input_node;
        input_node.physical_node_id = 1;
        input_node.relational_node_id = pivot_node->relational_node_id;
        input_node.node_kind = PhysicalNodeKind::kValues;
        input_node.implementation_id = "values.materialize.canonical.v1";
        input_node.input_physical_node_ids.clear();
        input_node.output_descriptor_ids.clear();
        for (const auto& column : aggregate_input.columns) {
          input_node.output_descriptor_ids.push_back(column.descriptor_id);
        }
        PhysicalNodeRecord aggregate_node = *pivot_node;
        aggregate_node.physical_node_id = 2;
        aggregate_node.relational_node_id = pivot_node->relational_node_id;
        aggregate_node.node_kind = PhysicalNodeKind::kAggregate;
        aggregate_node.implementation_id = "aggregate.registry.serial.v1";
        aggregate_node.input_physical_node_ids = {1};
        aggregate_node.output_descriptor_ids = {
            aggregate_request.result_column.descriptor_id};
        aggregate_node.causal_counter_id = pivot_node->causal_counter_id;
        aggregate_dag.nodes = {std::move(input_node),
                               std::move(aggregate_node)};
        aggregate_dag.root_physical_node_id = 2;
        aggregate_request.selected_physical_node_id = 2;
        auto aggregated =
            ExecuteCanonicalAggregateRuntimeWithFinalOutputCeiling(
                aggregate_request, aggregate_dag, aggregate_input,
                remaining_finalization_bytes);
        if (!aggregated.diagnostic.ok ||
            aggregated.output_batch.rows.size() != 1 ||
            aggregated.output_batch.rows.front().values.size() != 1) {
          return refuse(
              aggregated.diagnostic.ok
                  ? "QOW-DIAG-QRY-019-PIVOT-AGGREGATE-V1"
                  : aggregated.diagnostic.diagnostic_code,
              aggregated.diagnostic.ok
                  ? "PIVOT aggregate did not publish one canonical value"
                  : aggregated.diagnostic.detail);
        }
        if (aggregated.transition_count >
                request.maximum_total_aggregate_transition_count -
                    total_transitions) {
          return refuse("SBLR.PLAN_TREE.RESOURCE_LIMIT",
                        "PIVOT aggregate transition bound was exceeded");
        }
        total_transitions += aggregated.transition_count;
        if (aggregated.final_output_bytes >
            maximum_combined_final_output_bytes -
                combined_final_output_bytes) {
          return refuse("SBLR.PLAN_TREE.RESOURCE_LIMIT",
                        "PIVOT combined final output byte bound was exceeded");
        }
        combined_final_output_bytes += aggregated.final_output_bytes;
        peak_finalization_workspace_bytes = std::max(
            peak_finalization_workspace_bytes,
            aggregated.peak_finalization_workspace_bytes);
        auto aggregate_value = std::move(
            aggregated.output_batch.rows.front().values.front());
        if (!charge_output_value(aggregate_value)) {
          return refuse("SBLR.PLAN_TREE.RESOURCE_LIMIT",
                        "PIVOT retained output exhausted the selected-node grant");
        }
        output_row.values.push_back(std::move(aggregate_value));
      }
    }
    output.rows.push_back(std::move(output_row));
  }
  const auto output_validation = ValidateCanonicalDescriptorBatch(
      output, pivot_node->output_descriptor_ids);
  if (!output_validation.ok) {
    return refuse(output_validation.diagnostic_code,
                  output_validation.detail);
  }
  const auto result_authority = RevalidateCanonicalExecutionMgaAuthority(
      request.mga_authority, request.physical_dag);
  if (!result_authority.ok) {
    return refuse(result_authority.diagnostic_code,
                  result_authority.detail);
  }
  result.diagnostic = {};
  result.output_batch = std::move(output);
  result.input_row_count = request.input_batch.rows.size();
  result.group_count = groups.size();
  result.in_item_count = request.in_items.size();
  result.aggregate_count = request.aggregates.size();
  result.matched_input_row_count = matched_input_rows;
  result.key_comparison_count = key_comparisons;
  result.aggregate_transition_count = total_transitions;
  result.combined_final_output_bytes = combined_final_output_bytes;
  result.peak_finalization_workspace_bytes =
      peak_finalization_workspace_bytes;
  result.selected_plan_uuid = request.physical_dag.selected_plan_uuid;
  result.executed_physical_node_id = pivot_node->physical_node_id;
  result.causal_counter_id = pivot_node->causal_counter_id;
  result.mga_statement_context = request.mga_authority.statement_context;
  return result;
}

// QOW-SOURCE-QRY-019-UNPIVOT-V1
CanonicalUnpivotResult ExecuteCanonicalUnpivot(
    const CanonicalUnpivotRequest& request) {
  namespace api = scratchbird::engine::internal_api;
  CanonicalUnpivotResult result;
  const auto refuse = [&](std::string code, std::string detail) {
    result = {};
    result.diagnostic.ok = false;
    result.diagnostic.diagnostic_code = std::move(code);
    result.diagnostic.detail = std::move(detail);
    return result;
  };
  const auto authority = RevalidateCanonicalExecutionMgaAuthority(
      request.mga_authority, request.physical_dag);
  if (!authority.ok) {
    return refuse(authority.diagnostic_code, authority.detail);
  }
  const PhysicalNodeRecord* unpivot_node = nullptr;
  for (const auto& node : request.physical_dag.nodes) {
    if (node.physical_node_id == request.selected_physical_node_id) {
      unpivot_node = &node;
      break;
    }
  }
  const std::string expected_implementation =
      request.null_policy == CanonicalPivotNullPolicy::kInclude
          ? "unpivot.canonical.include-nulls.typed.v1"
          : "unpivot.canonical.exclude-nulls.typed.v1";
  if (unpivot_node == nullptr ||
      request.selected_physical_node_id !=
          request.physical_dag.root_physical_node_id ||
      unpivot_node->node_kind != PhysicalNodeKind::kUnpivot ||
      unpivot_node->input_physical_node_ids.size() != 1 ||
      unpivot_node->implementation_id != expected_implementation) {
    return refuse("QOW-DIAG-QRY-019-UNPIVOT-PHYSICAL-V1",
                  "UNPIVOT is not the exact selected unary physical root");
  }
  const PhysicalNodeRecord* unpivot_input_node = nullptr;
  for (const auto& node : request.physical_dag.nodes) {
    if (node.physical_node_id ==
        unpivot_node->input_physical_node_ids.front()) {
      unpivot_input_node = &node;
      break;
    }
  }
  if (unpivot_input_node == nullptr) {
    return refuse("QOW-DIAG-QRY-019-UNPIVOT-PHYSICAL-V1",
                  "UNPIVOT selected input physical node is absent");
  }
  if (request.null_policy != CanonicalPivotNullPolicy::kInclude &&
      request.null_policy != CanonicalPivotNullPolicy::kExclude) {
    return refuse("QOW-DIAG-QRY-019-UNPIVOT-BINDING-V1",
                  "UNPIVOT NULL policy is not canonical");
  }
  if (request.group_columns.empty() || request.in_items.empty() ||
      request.maximum_output_row_count == 0 ||
      request.maximum_output_cell_count == 0) {
    return refuse("QOW-DIAG-QRY-019-UNPIVOT-BINDING-V1",
                  "UNPIVOT group, IN, or resource binding is absent");
  }
  const auto value_column_count =
      request.in_items.front().source_columns.size();
  const auto expected_width =
      request.group_columns.size() + 1 + value_column_count;
  if (value_column_count == 0 || request.result_columns.size() != expected_width ||
      unpivot_node->output_descriptor_ids.size() != expected_width) {
    return refuse("QOW-DIAG-QRY-019-UNPIVOT-DESCRIPTOR-V1",
                  "UNPIVOT result width or value-column arity is invalid");
  }
  std::set<std::size_t> source_columns;
  for (const auto column : request.group_columns) {
    if (column >= request.input_batch.columns.size() ||
        !source_columns.insert(column).second) {
      return refuse("QOW-DIAG-QRY-019-UNPIVOT-BINDING-V1",
                    "UNPIVOT group column is unresolved or duplicated");
    }
  }
  for (const auto& item : request.in_items) {
    if (item.source_columns.size() != value_column_count) {
      return refuse("QOW-DIAG-QRY-019-UNPIVOT-IN-V1",
                    "UNPIVOT IN item arity differs from its value-column arity");
    }
    for (const auto column : item.source_columns) {
      if (column >= request.input_batch.columns.size() ||
          source_columns.contains(column)) {
        return refuse("QOW-DIAG-QRY-019-UNPIVOT-IN-V1",
                      "UNPIVOT IN source column is unresolved or overlaps a group column");
      }
    }
  }
  for (std::size_t column = 0; column < expected_width; ++column) {
    if (request.result_columns[column].descriptor_id == 0 ||
        request.result_columns[column].descriptor_id !=
            unpivot_node->output_descriptor_ids[column]) {
      return refuse("QOW-DIAG-QRY-019-UNPIVOT-DESCRIPTOR-V1",
                    "UNPIVOT result descriptor order is not exact");
    }
  }
  const auto input_validation = ValidateCanonicalDescriptorBatch(
      request.input_batch, unpivot_input_node->output_descriptor_ids);
  if (!input_validation.ok) {
    return refuse(input_validation.diagnostic_code,
                  input_validation.detail);
  }
  const auto node_memory_grant = FoundationNodeMemoryGrant(
      request.physical_dag, request.selected_physical_node_id);
  std::size_t input_payload_bytes = 0;
  if (!node_memory_grant.has_value() ||
      !FoundationBatchPayloadBytes(request.input_batch,
                                   &input_payload_bytes) ||
      input_payload_bytes > *node_memory_grant) {
    return refuse("SBLR.PLAN_TREE.RESOURCE_LIMIT",
                  "UNPIVOT selected-node memory grant is invalid or "
                  "exhausted");
  }
  if (request.input_batch.rows.size() != 0 &&
      request.in_items.size() >
          request.maximum_output_row_count / request.input_batch.rows.size()) {
    return refuse("SBLR.PLAN_TREE.RESOURCE_LIMIT",
                  "UNPIVOT maximum output row bound was exceeded");
  }
  const auto maximum_rows =
      request.input_batch.rows.size() * request.in_items.size();
  if (maximum_rows != 0 &&
      expected_width > request.maximum_output_cell_count / maximum_rows) {
    return refuse("SBLR.PLAN_TREE.RESOURCE_LIMIT",
                  "UNPIVOT maximum output cell bound was exceeded");
  }

  DescriptorBatch output;
  output.columns = request.result_columns;
  std::size_t excluded = 0;
  std::size_t retained_output_payload_bytes = 0;
  const auto charge_output_value = [&](const auto& value) {
    if (retained_output_payload_bytes >
        std::numeric_limits<std::size_t>::max() -
            value.encoded_value.size()) {
      return false;
    }
    retained_output_payload_bytes += value.encoded_value.size();
    if (retained_output_payload_bytes >
        std::numeric_limits<std::size_t>::max() -
            value.binary_value.size()) {
      return false;
    }
    retained_output_payload_bytes += value.binary_value.size();
    return retained_output_payload_bytes <=
           *node_memory_grant - input_payload_bytes;
  };
  for (const auto& input_row : request.input_batch.rows) {
    for (const auto& item : request.in_items) {
      const bool all_null = std::ranges::all_of(
          item.source_columns, [&](const auto column) {
            return input_row.values[column].state ==
                   api::EngineValueState::sql_null;
          });
      if (request.null_policy == CanonicalPivotNullPolicy::kExclude &&
          all_null) {
        ++excluded;
        continue;
      }
      DescriptorTuple output_row;
      output_row.values.reserve(expected_width);
      for (std::size_t group = 0; group < request.group_columns.size();
           ++group) {
        DescriptorRuntimeDiagnostic cast_diagnostic;
        auto value = CastDescriptorValue(
            input_row.values[request.group_columns[group]],
            request.result_columns[group].descriptor, &cast_diagnostic);
        if (!cast_diagnostic.ok) {
          return refuse("QOW-DIAG-QRY-019-UNPIVOT-DESCRIPTOR-V1",
                        cast_diagnostic.diagnostic_code + ":" +
                            cast_diagnostic.detail);
        }
        if (!charge_output_value(value)) {
          return refuse(
              "SBLR.PLAN_TREE.RESOURCE_LIMIT",
              "UNPIVOT retained output exhausted the selected-node grant");
        }
        output_row.values.push_back(std::move(value));
      }
      DescriptorRuntimeDiagnostic label_diagnostic;
      auto label = CastDescriptorValue(
          item.pivot_value,
          request.result_columns[request.group_columns.size()].descriptor,
          &label_diagnostic);
      if (!label_diagnostic.ok) {
        return refuse("QOW-DIAG-QRY-019-UNPIVOT-IN-V1",
                      label_diagnostic.diagnostic_code + ":" +
                          label_diagnostic.detail);
      }
      if (!charge_output_value(label)) {
        return refuse(
            "SBLR.PLAN_TREE.RESOURCE_LIMIT",
            "UNPIVOT retained output exhausted the selected-node grant");
      }
      output_row.values.push_back(std::move(label));
      for (std::size_t value = 0; value < value_column_count; ++value) {
        DescriptorRuntimeDiagnostic cast_diagnostic;
        auto reshaped = CastDescriptorValue(
            input_row.values[item.source_columns[value]],
            request.result_columns[request.group_columns.size() + 1 + value]
                .descriptor,
            &cast_diagnostic);
        if (!cast_diagnostic.ok) {
          return refuse("QOW-DIAG-QRY-019-UNPIVOT-DESCRIPTOR-V1",
                        cast_diagnostic.diagnostic_code + ":" +
                            cast_diagnostic.detail);
        }
        if (!charge_output_value(reshaped)) {
          return refuse(
              "SBLR.PLAN_TREE.RESOURCE_LIMIT",
              "UNPIVOT retained output exhausted the selected-node grant");
        }
        output_row.values.push_back(std::move(reshaped));
      }
      output.rows.push_back(std::move(output_row));
    }
  }
  const auto output_validation = ValidateCanonicalDescriptorBatch(
      output, unpivot_node->output_descriptor_ids);
  if (!output_validation.ok) {
    return refuse(output_validation.diagnostic_code,
                  output_validation.detail);
  }
  const auto result_authority = RevalidateCanonicalExecutionMgaAuthority(
      request.mga_authority, request.physical_dag);
  if (!result_authority.ok) {
    return refuse(result_authority.diagnostic_code,
                  result_authority.detail);
  }
  result.diagnostic = {};
  result.output_batch = std::move(output);
  result.input_row_count = request.input_batch.rows.size();
  result.in_item_count = request.in_items.size();
  result.emitted_row_count = result.output_batch.rows.size();
  result.null_excluded_row_count = excluded;
  result.selected_plan_uuid = request.physical_dag.selected_plan_uuid;
  result.executed_physical_node_id = unpivot_node->physical_node_id;
  result.causal_counter_id = unpivot_node->causal_counter_id;
  result.mga_statement_context = request.mga_authority.statement_context;
  return result;
}

// QOW-SOURCE-QRY-016-NESTING-V1
// Resolve one three-operand set expression before executing either physical
// node. INTERSECT has higher SQL precedence than UNION/EXCEPT; otherwise the
// unparenthesized expression is left associative. Explicit grouping overrides
// that rule but still executes two independently admitted typed physical nodes.
CanonicalSetOperationNestingResult ExecuteCanonicalSetOperationNesting(
    const CanonicalSetOperationNestingRequest& request) {
  CanonicalSetOperationNestingResult result;
  const auto refuse = [&](std::string detail) {
    result.diagnostic.ok = false;
    result.diagnostic.diagnostic_code =
        "QOW-DIAG-QRY-016-NESTING-REFUSAL-V1";
    result.diagnostic.detail = std::move(detail);
    result.output_batch = {};
    result.resolved_nesting_rule =
        CanonicalSetOperationNestingRule::kSqlPrecedence;
    result.intermediate_row_count = 0;
    result.inner_physical_node_id = 0;
    result.outer_physical_node_id = 0;
    result.inner_causal_counter_id = 0;
    result.outer_causal_counter_id = 0;
    return result;
  };
  const auto known_operation = [](const CanonicalSetOperationKind operation) {
    return operation == CanonicalSetOperationKind::kUnion ||
           operation == CanonicalSetOperationKind::kIntersect ||
           operation == CanonicalSetOperationKind::kExcept;
  };
  const auto known_quantifier = [](
                                    const CanonicalSetOperationQuantifier
                                        quantifier) {
    return quantifier == CanonicalSetOperationQuantifier::kAll ||
           quantifier == CanonicalSetOperationQuantifier::kDistinct;
  };
  const auto known_alignment = [](
                                   const CanonicalSetOperationAlignment
                                       alignment) {
    return alignment == CanonicalSetOperationAlignment::kOrdinal ||
           alignment == CanonicalSetOperationAlignment::kByName;
  };
  if (!known_operation(request.first_operation) ||
      !known_operation(request.second_operation) ||
      !known_quantifier(request.first_quantifier) ||
      !known_quantifier(request.second_quantifier) ||
      !known_alignment(request.first_alignment) ||
      !known_alignment(request.second_alignment)) {
    return refuse("set-operation nesting contains an unknown closed value");
  }
  if (request.maximum_intermediate_row_count == 0) {
    return refuse("set-operation intermediate resource bound is zero");
  }
  const auto inner_authority = RevalidateCanonicalExecutionMgaAuthority(
      request.inner_request_template.mga_authority,
      request.inner_request_template.physical_dag);
  const auto outer_authority = RevalidateCanonicalExecutionMgaAuthority(
      request.outer_request_template.mga_authority,
      request.outer_request_template.physical_dag);
  if (!inner_authority.ok || !outer_authority.ok ||
      !PhysicalMgaStatementContextEqual(
          request.inner_request_template.mga_authority.statement_context,
          request.outer_request_template.mga_authority.statement_context)) {
    return refuse(!inner_authority.ok
                      ? inner_authority.diagnostic_code + ":" +
                            inner_authority.detail
                      : (!outer_authority.ok
                             ? outer_authority.diagnostic_code + ":" +
                                   outer_authority.detail
                             : "nested set operations do not share one MGA statement context"));
  }
  if (request.inner_request_template.physical_dag.local_transaction_id !=
          request.outer_request_template.physical_dag.local_transaction_id ||
      request.inner_request_template.physical_dag.statement_snapshot_id !=
          request.outer_request_template.physical_dag.statement_snapshot_id ||
      request.inner_request_template.physical_dag.selected_plan_uuid !=
          request.outer_request_template.physical_dag.selected_plan_uuid) {
    return refuse("nested physical nodes do not share one engine MGA plan boundary");
  }

  bool right_grouped = false;
  switch (request.nesting_rule) {
    case CanonicalSetOperationNestingRule::kSqlPrecedence:
      right_grouped =
          request.second_operation == CanonicalSetOperationKind::kIntersect &&
          request.first_operation != CanonicalSetOperationKind::kIntersect;
      break;
    case CanonicalSetOperationNestingRule::kExplicitLeft:
      right_grouped = false;
      break;
    case CanonicalSetOperationNestingRule::kExplicitRight:
      right_grouped = true;
      break;
    default:
      return refuse("set-operation nesting rule is unknown");
  }
  const auto resolved_rule =
      right_grouped ? CanonicalSetOperationNestingRule::kExplicitRight
                    : CanonicalSetOperationNestingRule::kExplicitLeft;

  const auto execute = [](const CanonicalSetOperationAllRequest& operation) {
    return operation.quantifier == CanonicalSetOperationQuantifier::kAll
               ? ExecuteCanonicalSetOperationAll(operation)
               : ExecuteCanonicalSetOperationDistinct(operation);
  };

  auto inner = request.inner_request_template;
  inner.operation = right_grouped ? request.second_operation
                                  : request.first_operation;
  inner.quantifier = right_grouped ? request.second_quantifier
                                   : request.first_quantifier;
  inner.alignment = right_grouped ? request.second_alignment
                                  : request.first_alignment;
  inner.left_batch = right_grouped ? request.second_operand
                                   : request.first_operand;
  inner.right_batch = right_grouped ? request.third_operand
                                    : request.second_operand;
  auto inner_result = execute(inner);
  if (!inner_result.diagnostic.ok) {
    return refuse("inner:" + inner_result.diagnostic.diagnostic_code + ":" +
                  inner_result.diagnostic.detail);
  }
  if (!PhysicalMgaStatementContextEqual(
          inner_result.mga_statement_context,
          request.inner_request_template.mga_authority.statement_context)) {
    return refuse("inner set operation returned a different MGA statement context");
  }
  if (inner_result.output_batch.rows.size() >
      request.maximum_intermediate_row_count) {
    return refuse("nested intermediate row resource bound was exceeded");
  }

  auto outer = request.outer_request_template;
  outer.operation = right_grouped ? request.first_operation
                                  : request.second_operation;
  outer.quantifier = right_grouped ? request.first_quantifier
                                   : request.second_quantifier;
  outer.alignment = right_grouped ? request.first_alignment
                                  : request.second_alignment;
  outer.left_batch = right_grouped ? request.first_operand
                                   : inner_result.output_batch;
  outer.right_batch = right_grouped ? inner_result.output_batch
                                    : request.third_operand;
  auto outer_result = execute(outer);
  if (!outer_result.diagnostic.ok) {
    return refuse("outer:" + outer_result.diagnostic.diagnostic_code + ":" +
                  outer_result.diagnostic.detail);
  }
  if (!PhysicalMgaStatementContextEqual(
          outer_result.mga_statement_context,
          request.outer_request_template.mga_authority.statement_context)) {
    return refuse("outer set operation returned a different MGA statement context");
  }
  if (inner_result.causal_counter_id == 0 ||
      outer_result.causal_counter_id <= inner_result.causal_counter_id) {
    return refuse("nested physical causal order is not inner before outer");
  }
  const auto final_inner_authority = RevalidateCanonicalExecutionMgaAuthority(
      request.inner_request_template.mga_authority,
      request.inner_request_template.physical_dag);
  const auto final_outer_authority = RevalidateCanonicalExecutionMgaAuthority(
      request.outer_request_template.mga_authority,
      request.outer_request_template.physical_dag);
  if (!final_inner_authority.ok || !final_outer_authority.ok) {
    const auto& diagnostic = !final_inner_authority.ok
                                 ? final_inner_authority
                                 : final_outer_authority;
    return refuse(diagnostic.diagnostic_code + ":" + diagnostic.detail);
  }

  result.diagnostic = {};
  result.output_batch = std::move(outer_result.output_batch);
  result.resolved_nesting_rule = resolved_rule;
  result.intermediate_row_count = inner_result.output_batch.rows.size();
  result.inner_physical_node_id = inner_result.executed_physical_node_id;
  result.outer_physical_node_id = outer_result.executed_physical_node_id;
  result.inner_causal_counter_id = inner_result.causal_counter_id;
  result.outer_causal_counter_id = outer_result.causal_counter_id;
  result.mga_statement_context =
      request.outer_request_template.mga_authority.statement_context;
  return result;
}

Batch MakeBatch(std::string descriptor_digest,
                std::vector<Tuple> rows,
                std::size_t column_count) {
  if (column_count == 0 && !rows.empty()) {
    column_count = rows.front().values.size();
  }
  return {.descriptor_digest = std::move(descriptor_digest),
          .column_count = column_count,
          .rows = std::move(rows)};
}

OperatorDiagnostic ValidateBatch(const Batch& batch) {
  if (batch.descriptor_digest.empty()) return {.ok = false, .diagnostic_code = "SB_EXECUTOR_DESCRIPTOR_REQUIRED"};
  if (batch.rows.empty()) return {.ok = true, .diagnostic_code = "SB_EXECUTOR_OK"};
  const auto width = batch.column_count == 0
                         ? batch.rows.front().values.size()
                         : batch.column_count;
  for (const auto& row : batch.rows) {
    if (row.values.size() != width) return {.ok = false, .diagnostic_code = "SB_EXECUTOR_ROW_WIDTH_MISMATCH"};
  }
  return {.ok = true, .diagnostic_code = "SB_EXECUTOR_OK"};
}

std::int64_t EvalAdd(std::int64_t lhs, std::int64_t rhs) {
  std::int64_t result = 0;
  if (__builtin_add_overflow(lhs, rhs, &result)) {
    throw std::overflow_error("query_expression_int64_add_overflow");
  }
  return result;
}

std::int64_t EvalMultiply(std::int64_t lhs, std::int64_t rhs) {
  std::int64_t result = 0;
  if (__builtin_mul_overflow(lhs, rhs, &result)) {
    throw std::overflow_error("query_expression_int64_multiply_overflow");
  }
  return result;
}

Batch FilterByInt64Comparison(const Batch& input,
                              std::size_t column,
                              Int64ComparisonOperator op,
                              std::int64_t threshold) {
  RequireResolvedColumnHandles(input, {column});
  std::vector<Tuple> rows;
  for (const auto& row : input.rows) {
    const auto value = row.values[column];
    bool matches = false;
    switch (op) {
      case Int64ComparisonOperator::kGreaterThan:
        matches = value > threshold;
        break;
      case Int64ComparisonOperator::kGreaterThanOrEqual:
        matches = value >= threshold;
        break;
      case Int64ComparisonOperator::kLessThan:
        matches = value < threshold;
        break;
      case Int64ComparisonOperator::kLessThanOrEqual:
        matches = value <= threshold;
        break;
      case Int64ComparisonOperator::kEqual:
        matches = value == threshold;
        break;
      case Int64ComparisonOperator::kNotEqual:
        matches = value != threshold;
        break;
    }
    if (matches) rows.push_back(row);
  }
  return MakeBatch(input.descriptor_digest, std::move(rows),
                   input.column_count);
}

Batch FilterGreaterThan(const Batch& input, std::size_t column, std::int64_t threshold) {
  return FilterByInt64Comparison(input, column, Int64ComparisonOperator::kGreaterThan, threshold);
}

Batch ProjectColumns(const Batch& input, const std::vector<std::size_t>& columns) {
  RequireResolvedColumnHandles(input, columns);
  std::vector<Tuple> rows;
  rows.reserve(input.rows.size());
  for (const auto& row : input.rows) {
    Tuple projected;
    for (auto column : columns) projected.values.push_back(row.values[column]);
    rows.push_back(std::move(projected));
  }
  return MakeBatch(input.descriptor_digest + ":projected", std::move(rows),
                   columns.size());
}

Batch SortByColumn(const Batch& input, std::size_t column, bool ascending) {
  RequireResolvedColumnHandles(input, {column});
  auto rows = input.rows;
  std::stable_sort(rows.begin(), rows.end(), [&](const Tuple& lhs, const Tuple& rhs) {
    const auto lv = lhs.values[column];
    const auto rv = rhs.values[column];
    return ascending ? lv < rv : lv > rv;
  });
  return MakeBatch(input.descriptor_digest, std::move(rows),
                   input.column_count);
}

Batch LimitOffset(const Batch& input, std::size_t limit, std::size_t offset) {
  std::vector<Tuple> rows;
  if (offset >= input.rows.size()) {
    return MakeBatch(input.descriptor_digest, {}, input.column_count);
  }
  const auto end = std::min(input.rows.size(), offset + limit);
  for (std::size_t i = offset; i < end; ++i) rows.push_back(input.rows[i]);
  return MakeBatch(input.descriptor_digest, std::move(rows),
                   input.column_count);
}

Batch AggregateSumByKey(const Batch& input, std::size_t key_column, std::size_t value_column) {
  RequireResolvedColumnHandles(input, {key_column, value_column});
  std::map<std::int64_t, std::int64_t> sums;
  for (const auto& row : input.rows) {
    auto& sum = sums[row.values[key_column]];
    std::int64_t next_sum = 0;
    if (__builtin_add_overflow(sum, row.values[value_column], &next_sum)) {
      throw std::overflow_error("query_aggregate_int64_sum_overflow");
    }
    sum = next_sum;
  }
  std::vector<Tuple> rows;
  for (const auto& [key, sum] : sums) rows.push_back({.values = {key, sum}});
  return MakeBatch(input.descriptor_digest + ":aggregate", std::move(rows), 2);
}

Batch NestedLoopJoinEqual(const Batch& left, const Batch& right, std::size_t left_column, std::size_t right_column) {
  RequireResolvedColumnHandles(left, {left_column});
  RequireResolvedColumnHandles(right, {right_column});
  std::vector<Tuple> rows;
  for (const auto& l : left.rows) {
    for (const auto& r : right.rows) {
      if (l.values[left_column] == r.values[right_column]) rows.push_back({.values = ConcatValues(l, r)});
    }
  }
  return MakeBatch(JoinDescriptor(left.descriptor_digest, right.descriptor_digest),
                   std::move(rows), left.column_count + right.column_count);
}

Batch HashJoinEqual(const Batch& left, const Batch& right, std::size_t left_column, std::size_t right_column) {
  RequireResolvedColumnHandles(left, {left_column});
  RequireResolvedColumnHandles(right, {right_column});
  std::unordered_multimap<std::int64_t, const Tuple*> hash;
  for (const auto& r : right.rows) hash.emplace(r.values[right_column], &r);
  std::vector<Tuple> rows;
  for (const auto& l : left.rows) {
    const auto range = hash.equal_range(l.values[left_column]);
    for (auto it = range.first; it != range.second; ++it) rows.push_back({.values = ConcatValues(l, *it->second)});
  }
  return MakeBatch(JoinDescriptor(left.descriptor_digest, right.descriptor_digest),
                   std::move(rows), left.column_count + right.column_count);
}

Batch MergeJoinEqual(const Batch& left_sorted, const Batch& right_sorted, std::size_t left_column, std::size_t right_column) {
  RequireResolvedColumnHandles(left_sorted, {left_column});
  RequireResolvedColumnHandles(right_sorted, {right_column});
  std::vector<Tuple> rows;
  std::size_t left_index = 0;
  std::size_t right_index = 0;

  while (left_index < left_sorted.rows.size() &&
         right_index < right_sorted.rows.size()) {
    const auto& left = left_sorted.rows[left_index];
    const auto& right = right_sorted.rows[right_index];
    const auto left_key = left.values[left_column];
    const auto right_key = right.values[right_column];
    if (left_key < right_key) {
      ++left_index;
      continue;
    }
    if (right_key < left_key) {
      ++right_index;
      continue;
    }

    const auto match_key = left_key;
    const auto left_begin = left_index;
    while (left_index < left_sorted.rows.size() &&
           left_sorted.rows[left_index].values[left_column] == match_key) {
      ++left_index;
    }
    const auto left_end = left_index;

    const auto right_begin = right_index;
    while (right_index < right_sorted.rows.size() &&
           right_sorted.rows[right_index].values[right_column] == match_key) {
      ++right_index;
    }
    const auto right_end = right_index;

    for (std::size_t i = left_begin; i < left_end; ++i) {
      for (std::size_t j = right_begin; j < right_end; ++j) {
        rows.push_back(
            {.values = ConcatValues(left_sorted.rows[i], right_sorted.rows[j])});
      }
    }
  }

  return MakeBatch(JoinDescriptor(left_sorted.descriptor_digest,
                                  right_sorted.descriptor_digest),
                   std::move(rows),
                   left_sorted.column_count + right_sorted.column_count);
}

Batch AddRowNumberWindow(const Batch& input, std::size_t order_column) {
  auto sorted = SortByColumn(input, order_column, true);
  for (std::size_t i = 0; i < sorted.rows.size(); ++i) sorted.rows[i].values.push_back(static_cast<std::int64_t>(i + 1));
  ++sorted.column_count;
  sorted.descriptor_digest += ":row_number";
  return sorted;
}

Batch AddRankWindow(const Batch& input, std::size_t order_column) {
  auto sorted = SortByColumn(input, order_column, true);
  std::int64_t current_rank = 1;
  std::int64_t previous_value = 0;
  bool have_previous = false;
  for (std::size_t i = 0; i < sorted.rows.size(); ++i) {
    const std::int64_t value = sorted.rows[i].values[order_column];
    if (!have_previous || value != previous_value) {
      current_rank = static_cast<std::int64_t>(i + 1);
      previous_value = value;
      have_previous = true;
    }
    sorted.rows[i].values.push_back(current_rank);
  }
  ++sorted.column_count;
  sorted.descriptor_digest += ":rank";
  return sorted;
}

Batch AddDenseRankWindow(const Batch& input, std::size_t order_column) {
  auto sorted = SortByColumn(input, order_column, true);
  std::int64_t current_dense_rank = 0;
  std::int64_t previous_value = 0;
  bool have_previous = false;
  for (auto& row : sorted.rows) {
    const std::int64_t value = row.values[order_column];
    if (!have_previous || value != previous_value) {
      ++current_dense_rank;
      previous_value = value;
      have_previous = true;
    }
    row.values.push_back(current_dense_rank);
  }
  ++sorted.column_count;
  sorted.descriptor_digest += ":dense_rank";
  return sorted;
}

Batch AddPartitionCountWindow(const Batch& input, std::size_t partition_column) {
  RequireResolvedColumnHandles(input, {partition_column});
  std::unordered_map<std::int64_t, std::int64_t> partition_counts;
  for (const auto& row : input.rows) {
    const std::int64_t key = row.values[partition_column];
    ++partition_counts[key];
  }
  auto out = input;
  for (auto& row : out.rows) {
    const std::int64_t key = row.values[partition_column];
    row.values.push_back(partition_counts[key]);
  }
  ++out.column_count;
  out.descriptor_digest += ":partition_count";
  return out;
}

Batch AddNtileWindow(const Batch& input, std::size_t order_column, std::int64_t bucket_count) {
  auto sorted = SortByColumn(input, order_column, true);
  if (bucket_count <= 0) {
    throw std::invalid_argument("query_window_ntile_bucket_count_invalid");
  }
  if (sorted.rows.empty()) {
    ++sorted.column_count;
    sorted.descriptor_digest += ":ntile";
    return sorted;
  }
  const std::uint64_t row_count = static_cast<std::uint64_t>(sorted.rows.size());
  const std::uint64_t buckets = std::min(
      static_cast<std::uint64_t>(bucket_count), row_count);
  const std::uint64_t base_bucket_size = row_count / buckets;
  const std::uint64_t larger_bucket_count = row_count % buckets;
  const std::uint64_t larger_bucket_rows =
      (base_bucket_size + 1) * larger_bucket_count;
  for (std::uint64_t i = 0; i < row_count; ++i) {
    const auto bucket = static_cast<std::int64_t>(
        i < larger_bucket_rows
            ? i / (base_bucket_size + 1) + 1
            : larger_bucket_count +
                  (i - larger_bucket_rows) / base_bucket_size + 1);
    sorted.rows[static_cast<std::size_t>(i)].values.push_back(bucket);
  }
  ++sorted.column_count;
  sorted.descriptor_digest += ":ntile";
  return sorted;
}

Batch AddLagWindow(const Batch& input, std::size_t order_column, std::size_t value_column) {
  RequireResolvedColumnHandles(input, {order_column, value_column});
  auto sorted = SortByColumn(input, order_column, true);
  for (std::size_t i = 0; i < sorted.rows.size(); ++i) {
    const std::int64_t value =
        i == 0 ? 0 : sorted.rows[i - 1].values[value_column];
    sorted.rows[i].values.push_back(value);
  }
  ++sorted.column_count;
  sorted.descriptor_digest += ":lag";
  return sorted;
}

Batch AddLeadWindow(const Batch& input, std::size_t order_column, std::size_t value_column) {
  RequireResolvedColumnHandles(input, {order_column, value_column});
  auto sorted = SortByColumn(input, order_column, true);
  for (std::size_t i = 0; i < sorted.rows.size(); ++i) {
    const std::int64_t value = i + 1 >= sorted.rows.size()
                                   ? 0
                                   : sorted.rows[i + 1].values[value_column];
    sorted.rows[i].values.push_back(value);
  }
  ++sorted.column_count;
  sorted.descriptor_digest += ":lead";
  return sorted;
}

Batch AddFirstValueWindow(const Batch& input, std::size_t order_column, std::size_t value_column) {
  RequireResolvedColumnHandles(input, {order_column, value_column});
  auto sorted = SortByColumn(input, order_column, true);
  const std::int64_t value =
      sorted.rows.empty() ? 0 : sorted.rows.front().values[value_column];
  for (auto& row : sorted.rows) row.values.push_back(value);
  ++sorted.column_count;
  sorted.descriptor_digest += ":first_value";
  return sorted;
}

Batch AddLastValueWindow(const Batch& input, std::size_t order_column, std::size_t value_column) {
  RequireResolvedColumnHandles(input, {order_column, value_column});
  auto sorted = SortByColumn(input, order_column, true);
  const std::int64_t value =
      sorted.rows.empty() ? 0 : sorted.rows.back().values[value_column];
  for (auto& row : sorted.rows) row.values.push_back(value);
  ++sorted.column_count;
  sorted.descriptor_digest += ":last_value";
  return sorted;
}

Batch MaterializeCte(const Batch& input) {
  return MakeBatch(input.descriptor_digest + ":materialized", input.rows,
                   input.column_count);
}

std::int64_t ScalarSubqueryFirstValue(const Batch& input, std::size_t column) {
  if (input.rows.empty()) {
    throw std::invalid_argument(
        "query_scalar_subquery_zero_row_requires_typed_null");
  }
  if (input.rows.size() != 1) {
    throw std::length_error("query_scalar_subquery_cardinality_violation");
  }
  RequireResolvedColumnHandles(input, {column});
  return input.rows.front().values[column];
}

Batch SetUnionDistinct(const Batch& left, const Batch& right) {
  if (left.column_count != right.column_count) {
    throw std::invalid_argument("query_set_operation_arity_mismatch");
  }
  std::set<std::vector<std::int64_t>> seen;
  std::vector<Tuple> rows;
  for (const auto& row : left.rows) if (seen.insert(row.values).second) rows.push_back(row);
  for (const auto& row : right.rows) if (seen.insert(row.values).second) rows.push_back(row);
  return MakeBatch(left.descriptor_digest, std::move(rows), left.column_count);
}

Batch SetIntersectDistinct(const Batch& left, const Batch& right) {
  if (left.column_count != right.column_count) {
    throw std::invalid_argument("query_set_operation_arity_mismatch");
  }
  std::set<std::vector<std::int64_t>> right_values;
  for (const auto& row : right.rows) right_values.insert(row.values);
  std::set<std::vector<std::int64_t>> emitted;
  std::vector<Tuple> rows;
  for (const auto& row : left.rows) if (right_values.contains(row.values) && emitted.insert(row.values).second) rows.push_back(row);
  return MakeBatch(left.descriptor_digest, std::move(rows), left.column_count);
}

Batch SetExceptDistinct(const Batch& left, const Batch& right) {
  if (left.column_count != right.column_count) {
    throw std::invalid_argument("query_set_operation_arity_mismatch");
  }
  std::set<std::vector<std::int64_t>> right_values;
  for (const auto& row : right.rows) right_values.insert(row.values);
  std::set<std::vector<std::int64_t>> emitted;
  std::vector<Tuple> rows;
  for (const auto& row : left.rows) if (!right_values.contains(row.values) && emitted.insert(row.values).second) rows.push_back(row);
  return MakeBatch(left.descriptor_digest, std::move(rows), left.column_count);
}

Batch SetUnionAll(const Batch& left, const Batch& right) {
  if (left.column_count != right.column_count) {
    throw std::invalid_argument("query_set_operation_arity_mismatch");
  }
  std::vector<Tuple> rows = left.rows;
  rows.insert(rows.end(), right.rows.begin(), right.rows.end());
  return MakeBatch(left.descriptor_digest, std::move(rows), left.column_count);
}

Batch SetIntersectAll(const Batch& left, const Batch& right) {
  if (left.column_count != right.column_count) {
    throw std::invalid_argument("query_set_operation_arity_mismatch");
  }
  std::map<std::vector<std::int64_t>, std::size_t> right_counts;
  for (const auto& row : right.rows) ++right_counts[row.values];
  std::vector<Tuple> rows;
  for (const auto& row : left.rows) {
    auto found = right_counts.find(row.values);
    if (found == right_counts.end() || found->second == 0) continue;
    --found->second;
    rows.push_back(row);
  }
  return MakeBatch(left.descriptor_digest, std::move(rows), left.column_count);
}

Batch SetExceptAll(const Batch& left, const Batch& right) {
  if (left.column_count != right.column_count) {
    throw std::invalid_argument("query_set_operation_arity_mismatch");
  }
  std::map<std::vector<std::int64_t>, std::size_t> right_counts;
  for (const auto& row : right.rows) ++right_counts[row.values];
  std::vector<Tuple> rows;
  for (const auto& row : left.rows) {
    auto found = right_counts.find(row.values);
    if (found != right_counts.end() && found->second != 0) {
      --found->second;
      continue;
    }
    rows.push_back(row);
  }
  return MakeBatch(left.descriptor_digest, std::move(rows), left.column_count);
}

std::vector<OperatorCatalogEntry> Stage6OperatorCatalog() {
  return {{"constant_result", "command", true, false, false}, {"catalog_lookup", "catalog", true, true, true}, {"table_scan", "scan", true, true, false}, {"index_lookup", "scan", true, true, false}, {"index_range_scan", "scan", true, true, false}, {"filter", "relational", true, false, false}, {"projection", "relational", true, false, false}, {"expression_eval", "expression", true, false, false}, {"sort", "relational", true, false, true}, {"limit_offset", "relational", true, false, false}, {"aggregate", "aggregate", true, false, true}, {"hash_aggregate", "aggregate", true, false, true}, {"nested_loop_join", "join", true, false, false}, {"index_nested_loop_join", "join", true, true, false}, {"hash_join", "join", true, false, true}, {"merge_join", "join", true, false, false}, {"window", "window", true, false, true}, {"subquery", "query_nesting", true, false, true}, {"cte_materialize_inline", "query_nesting", true, false, true}, {"set_operation", "setop", true, false, true}, {"nosql_access", "specialized_nosql", true, true, true}, {"temporary_storage", "materialization", true, false, true}, {"spill", "materialization", true, false, true}};
}

bool ValidateOperatorCatalog(const std::vector<OperatorCatalogEntry>& catalog, std::vector<std::string>* errors) {
  const auto before = errors ? errors->size() : 0;
  std::set<std::string> ids;
  for (const auto& entry : catalog) {
    if (entry.operator_id.empty()) {
      if (errors) errors->push_back("operator ID is required");
    } else if (!ids.insert(entry.operator_id).second) {
      if (errors) errors->push_back("duplicate operator ID: " + entry.operator_id);
    }
    if (entry.family.empty() && errors) errors->push_back("operator family is required for " + entry.operator_id);
    if (!entry.descriptor_required && errors) errors->push_back("descriptor is required for every Stage 6 operator: " + entry.operator_id);
  }
  return !errors || errors->size() == before;
}

namespace {

constexpr std::string_view kWindowRowNumberUuid =
    "019de5fc-2400-7539-bcce-00eef3ae7220";
constexpr std::string_view kWindowRankUuid =
    "019de5fc-2400-7b94-870d-0dd789ca70ab";
constexpr std::string_view kWindowDenseRankUuid =
    "019de5fc-2400-741d-bef0-f079fd3ba494";
constexpr std::string_view kWindowPercentRankUuid =
    "019de5fc-2400-7d86-86fe-96f3f27b5dd6";
constexpr std::string_view kWindowCumeDistUuid =
    "019de5fc-2400-721c-be64-2568b64a02b9";
constexpr std::string_view kWindowNtileUuid =
    "019de5fc-2400-7047-9474-232ca488c094";

DescriptorRuntimeDiagnostic WindowRankingRefusal(
    const CanonicalWindowRankingFunction function,
    std::string detail) {
  DescriptorRuntimeDiagnostic diagnostic;
  diagnostic.ok = false;
  diagnostic.diagnostic_code =
      function == CanonicalWindowRankingFunction::ntile
          ? "QOW-DIAG-WINDOW-NTILE"
          : "QOW-DIAG-WINDOW-RANKING";
  diagnostic.detail = std::move(detail);
  return diagnostic;
}

std::string_view RankingFunctionUuid(
    const CanonicalWindowRankingFunction function) {
  switch (function) {
    case CanonicalWindowRankingFunction::row_number:
      return kWindowRowNumberUuid;
    case CanonicalWindowRankingFunction::rank:
      return kWindowRankUuid;
    case CanonicalWindowRankingFunction::dense_rank:
      return kWindowDenseRankUuid;
    case CanonicalWindowRankingFunction::percent_rank:
      return kWindowPercentRankUuid;
    case CanonicalWindowRankingFunction::cume_dist:
      return kWindowCumeDistUuid;
    case CanonicalWindowRankingFunction::ntile:
      return kWindowNtileUuid;
  }
  return {};
}

bool RankingIntegerResult(const CanonicalWindowRankingFunction function) {
  return function == CanonicalWindowRankingFunction::row_number ||
         function == CanonicalWindowRankingFunction::rank ||
         function == CanonicalWindowRankingFunction::dense_rank ||
         function == CanonicalWindowRankingFunction::ntile;
}

bool RankingRealType(const scratchbird::core::datatypes::CanonicalTypeId type) {
  namespace dt = scratchbird::core::datatypes;
  return type == dt::CanonicalTypeId::bfloat16 ||
         type == dt::CanonicalTypeId::real16 ||
         type == dt::CanonicalTypeId::real32 ||
         type == dt::CanonicalTypeId::real64 ||
         type == dt::CanonicalTypeId::real128;
}

std::optional<std::string_view> RankingDescriptorField(
    const scratchbird::engine::internal_api::EngineDescriptor& descriptor,
    const std::string_view key) {
  const std::string prefix = std::string(key) + "=";
  std::optional<std::string_view> value;
  std::size_t begin = 0;
  while (begin <= descriptor.encoded_descriptor.size()) {
    const auto end = descriptor.encoded_descriptor.find(';', begin);
    const auto field = std::string_view(descriptor.encoded_descriptor).substr(
        begin, end == std::string::npos
                   ? std::string_view::npos
                   : end - begin);
    if (field.starts_with(prefix)) {
      if (value.has_value() || field.size() == prefix.size()) {
        return std::nullopt;
      }
      value = field.substr(prefix.size());
    }
    if (end == std::string::npos) break;
    begin = end + 1;
  }
  return value;
}

bool RankingIdentityIndependent(const std::string_view identity,
                                const CanonicalWindowFrameResult& frames,
                                const std::string_view function_uuid) {
  return identity != function_uuid &&
         identity != frames.resolved_frame.frame_descriptor_uuid &&
         identity != frames.window_property_uuid &&
         identity != frames.partition_property_uuid &&
         identity != frames.ordering_property_uuid &&
         identity != frames.term_binding_evidence_uuid &&
         identity != frames.deterministic_tie_evidence_uuid &&
         identity != frames.frame_property_binding_evidence_uuid;
}

bool RankingOutputDescriptorValid(
    const CanonicalWindowRankingRequest& request) {
  namespace dt = scratchbird::core::datatypes;
  const auto& descriptor = request.output_descriptor;
  const auto type_uuid = RankingDescriptorField(descriptor, "type_uuid");
  const auto nullability =
      RankingDescriptorField(descriptor, "nullability");
  if (!IsCanonicalUuid(descriptor.descriptor_uuid.canonical) ||
      descriptor.descriptor_kind != "scalar" ||
      descriptor.encoded_descriptor.empty() || !type_uuid.has_value() ||
      !IsCanonicalUuid(*type_uuid) || !nullability.has_value() ||
      *nullability != "non_null" ||
      descriptor.descriptor_uuid.canonical == *type_uuid ||
      !RankingIdentityIndependent(descriptor.descriptor_uuid.canonical,
                                  request.frames,
                                  request.function_uuid)) {
    return false;
  }
  const auto type =
      dt::CanonicalTypeIdFromStableName(descriptor.canonical_type_name);
  return RankingIntegerResult(request.function)
             ? type == dt::CanonicalTypeId::int64
             : RankingRealType(type);
}

bool CanonicalRankingNtileOperand(
    const scratchbird::engine::internal_api::EngineTypedValue& operand,
    const CanonicalWindowRankingRequest& request,
    std::uint64_t* bucket_count) {
  namespace api = scratchbird::engine::internal_api;
  namespace dt = scratchbird::core::datatypes;
  if (bucket_count == nullptr ||
      !IsCanonicalUuid(operand.descriptor.descriptor_uuid.canonical) ||
      operand.descriptor.descriptor_kind != "scalar" ||
      dt::CanonicalTypeIdFromStableName(
          operand.descriptor.canonical_type_name) !=
          dt::CanonicalTypeId::int64 ||
      operand.descriptor.encoded_descriptor.empty() ||
      !RankingIdentityIndependent(operand.descriptor.descriptor_uuid.canonical,
                                  request.frames,
                                  request.function_uuid) ||
      operand.descriptor.descriptor_uuid.canonical ==
          request.output_descriptor.descriptor_uuid.canonical ||
      operand.state != api::EngineValueState::value || operand.is_null ||
      !operand.binary_value.empty()) {
    return false;
  }
  const auto type_uuid =
      RankingDescriptorField(operand.descriptor, "type_uuid");
  const auto nullability =
      RankingDescriptorField(operand.descriptor, "nullability");
  if (!type_uuid.has_value() || !IsCanonicalUuid(*type_uuid) ||
      operand.descriptor.descriptor_uuid.canonical == *type_uuid ||
      !nullability.has_value() || *nullability != "non_null") {
    return false;
  }
  const auto decoded = DecodeInt64Value(operand);
  if (!decoded.ok() || decoded.value <= 0) return false;
  *bucket_count = static_cast<std::uint64_t>(decoded.value);
  return true;
}

bool RankingFrameUnitValid(const CanonicalWindowFrameUnit unit) {
  switch (unit) {
    case CanonicalWindowFrameUnit::rows:
    case CanonicalWindowFrameUnit::range:
    case CanonicalWindowFrameUnit::groups:
      return true;
  }
  return false;
}

bool RankingFrameExclusionValid(
    const CanonicalWindowFrameExclusion exclusion) {
  switch (exclusion) {
    case CanonicalWindowFrameExclusion::no_others:
    case CanonicalWindowFrameExclusion::current_row:
    case CanonicalWindowFrameExclusion::group:
    case CanonicalWindowFrameExclusion::ties:
      return true;
  }
  return false;
}

bool RankingFrameBoundValid(const CanonicalWindowFrameBound& bound) {
  switch (bound.kind) {
    case CanonicalWindowFrameBoundKind::unbounded_preceding:
    case CanonicalWindowFrameBoundKind::current_row:
    case CanonicalWindowFrameBoundKind::unbounded_following:
      return !bound.offset.has_value();
    case CanonicalWindowFrameBoundKind::offset_preceding:
    case CanonicalWindowFrameBoundKind::offset_following:
      return bound.offset.has_value();
  }
  return false;
}

bool CanonicalWindowDefaultFrameEvidenceValid(
    const CanonicalWindowFrameResult& frames) {
  if (frames.resolved_frame.frame_specified) {
    return !frames.defaulted_with_order &&
           !frames.defaulted_without_order;
  }
  if (frames.defaulted_with_order == frames.defaulted_without_order ||
      frames.resolved_frame.exclusion !=
          CanonicalWindowFrameExclusion::no_others ||
      frames.resolved_frame.start->kind !=
          CanonicalWindowFrameBoundKind::unbounded_preceding ||
      frames.resolved_frame.start->offset.has_value()) {
    return false;
  }
  if (frames.ordering_property_uuid.empty()) {
    return frames.defaulted_without_order &&
           frames.resolved_frame.unit == CanonicalWindowFrameUnit::rows &&
           frames.resolved_frame.end->kind ==
               CanonicalWindowFrameBoundKind::unbounded_following &&
           !frames.resolved_frame.end->offset.has_value();
  }
  return frames.defaulted_with_order &&
         frames.resolved_frame.unit == CanonicalWindowFrameUnit::range &&
         frames.resolved_frame.end->kind ==
             CanonicalWindowFrameBoundKind::current_row &&
         !frames.resolved_frame.end->offset.has_value();
}

bool CanonicalWindowFrameEvidenceValid(
    const CanonicalWindowFrameResult& frames) {
  const auto row_count = frames.ordered_batch.rows.size();
  const PhysicalNodeRecord* selected_node = nullptr;
  const PhysicalNodeRecord* selected_input_node = nullptr;
  for (const auto& node : frames.physical_dag.nodes) {
    if (node.physical_node_id == frames.executed_physical_node_id) {
      selected_node = &node;
      break;
    }
  }
  if (selected_node != nullptr &&
      selected_node->input_physical_node_ids.size() == 1) {
    for (const auto& node : frames.physical_dag.nodes) {
      if (node.physical_node_id ==
          selected_node->input_physical_node_ids.front()) {
        selected_input_node = &node;
        break;
      }
    }
  }
  const bool direct_stage =
      !frames.operator_local_stage &&
      frames.operator_local_parent_implementation_id.empty() &&
      selected_node != nullptr &&
      selected_node->implementation_id == "window.partition-order-peer.v1";
  const bool operator_local_stage =
      frames.operator_local_stage &&
      (frames.operator_local_parent_implementation_id == "window.lag.v1" ||
       frames.operator_local_parent_implementation_id == "window.lead.v1" ||
       frames.operator_local_parent_implementation_id ==
           "window.first-value.v1") &&
      selected_node != nullptr && selected_input_node != nullptr &&
      selected_node->implementation_id ==
          frames.operator_local_parent_implementation_id &&
      selected_node->output_descriptor_ids.size() ==
          selected_input_node->output_descriptor_ids.size() + 1 &&
      std::equal(selected_input_node->output_descriptor_ids.begin(),
                 selected_input_node->output_descriptor_ids.end(),
                 selected_node->output_descriptor_ids.begin());
  if (selected_node == nullptr ||
      selected_input_node == nullptr ||
      selected_node->node_kind != PhysicalNodeKind::kWindow ||
      (!direct_stage && !operator_local_stage) ||
      selected_node->input_physical_node_ids.size() != 1 ||
      selected_node->physical_node_id !=
          frames.physical_dag.root_physical_node_id ||
      frames.selected_plan_uuid != frames.physical_dag.selected_plan_uuid ||
      frames.causal_counter_id != selected_node->causal_counter_id) {
    return false;
  }
  const auto& stage_output_descriptor_ids =
      operator_local_stage ? selected_input_node->output_descriptor_ids
                           : selected_node->output_descriptor_ids;
  const auto batch_diagnostic = ValidateCanonicalDescriptorBatch(
      frames.ordered_batch, stage_output_descriptor_ids);
  if (!frames.diagnostic.ok || !frames.every_frame_operand_consumed ||
      !frames.empty_state_uses_optional_bounds ||
      !frames.base_frame_constructed_before_exclusion ||
      !frames.exactly_one_exclusion_consumed ||
      !frames.authority.engine_mga_snapshot_bound ||
      !batch_diagnostic.ok ||
      !IsCanonicalUuid(frames.resolved_frame.frame_descriptor_uuid) ||
      !IsCanonicalUuid(frames.window_property_uuid) ||
      (!frames.partition_property_uuid.empty() &&
       !IsCanonicalUuid(frames.partition_property_uuid)) ||
      (!frames.ordering_property_uuid.empty() &&
       !IsCanonicalUuid(frames.ordering_property_uuid)) ||
      ((!frames.partition_property_uuid.empty() ||
        !frames.ordering_property_uuid.empty()) !=
       !frames.term_binding_evidence_uuid.empty()) ||
      (!frames.term_binding_evidence_uuid.empty() &&
       !IsCanonicalUuid(frames.term_binding_evidence_uuid)) ||
      !IsCanonicalUuid(frames.deterministic_tie_evidence_uuid) ||
      !IsCanonicalUuid(frames.frame_property_binding_evidence_uuid) ||
      frames.frame_property_binding_evidence_uuid ==
          frames.resolved_frame.frame_descriptor_uuid ||
      frames.frame_property_binding_evidence_uuid ==
          frames.window_property_uuid ||
      frames.frame_property_binding_evidence_uuid ==
          frames.partition_property_uuid ||
      frames.frame_property_binding_evidence_uuid ==
          frames.ordering_property_uuid ||
      frames.frame_property_binding_evidence_uuid ==
          frames.term_binding_evidence_uuid ||
      frames.frame_property_binding_evidence_uuid ==
          frames.deterministic_tie_evidence_uuid ||
      frames.resolved_frame.frame_descriptor_uuid ==
          frames.window_property_uuid ||
      frames.resolved_frame.frame_descriptor_uuid ==
          frames.partition_property_uuid ||
      frames.resolved_frame.frame_descriptor_uuid ==
          frames.ordering_property_uuid ||
      frames.resolved_frame.frame_descriptor_uuid ==
          frames.term_binding_evidence_uuid ||
      frames.resolved_frame.frame_descriptor_uuid ==
          frames.deterministic_tie_evidence_uuid ||
      !IsCanonicalUuid(frames.selected_plan_uuid) ||
      !frames.resolved_frame.start.has_value() ||
      !frames.resolved_frame.end.has_value() ||
      !RankingFrameUnitValid(frames.resolved_frame.unit) ||
      !RankingFrameExclusionValid(frames.resolved_frame.exclusion) ||
      !RankingFrameBoundValid(*frames.resolved_frame.start) ||
      !RankingFrameBoundValid(*frames.resolved_frame.end) ||
      !CanonicalWindowDefaultFrameEvidenceValid(frames) ||
      frames.executed_physical_node_id == 0 || frames.causal_counter_id == 0 ||
      frames.row_metadata.size() != row_count ||
      frames.effective_frames.size() != row_count) {
    return false;
  }
  for (std::size_t row = 0; row < row_count; ++row) {
    const auto& metadata = frames.row_metadata[row];
    const auto& frame = frames.effective_frames[row];
    if (metadata.ordered_row_index != row ||
        !metadata.partition_id.has_value() ||
        !metadata.peer_group_id.has_value() ||
        metadata.partition_begin > row ||
        metadata.partition_end_exclusive <= row ||
        metadata.partition_end_exclusive > row_count ||
        metadata.peer_begin > row || metadata.peer_end_exclusive <= row ||
        metadata.peer_begin < metadata.partition_begin ||
        metadata.peer_end_exclusive > metadata.partition_end_exclusive ||
        frame.ordered_row_index != row ||
        frame.partition_id != metadata.partition_id ||
        !frame.exclusion_operand_consumed ||
        frame.exclusion_applied !=
            (frames.resolved_frame.exclusion !=
             CanonicalWindowFrameExclusion::no_others)) {
      return false;
    }
    if (frame.base_state == CanonicalWindowFrameState::nonempty) {
      if (!frame.base_begin.has_value() ||
          !frame.base_end_exclusive.has_value() ||
          *frame.base_begin < metadata.partition_begin ||
          *frame.base_end_exclusive > metadata.partition_end_exclusive ||
          *frame.base_begin >= *frame.base_end_exclusive) {
        return false;
      }
      std::optional<std::size_t> prior;
      for (const auto member : frame.effective_row_indices) {
        if (member < *frame.base_begin ||
            member >= *frame.base_end_exclusive ||
            (prior.has_value() && member <= *prior)) {
          return false;
        }
        prior = member;
      }
      std::vector<std::size_t> expected;
      for (std::size_t member = *frame.base_begin;
           member < *frame.base_end_exclusive; ++member) {
        const bool peer = member >= metadata.peer_begin &&
                          member < metadata.peer_end_exclusive;
        bool excluded = false;
        switch (frames.resolved_frame.exclusion) {
          case CanonicalWindowFrameExclusion::no_others:
            break;
          case CanonicalWindowFrameExclusion::current_row:
            excluded = member == row;
            break;
          case CanonicalWindowFrameExclusion::group:
            excluded = peer;
            break;
          case CanonicalWindowFrameExclusion::ties:
            excluded = peer && member != row;
            break;
        }
        if (!excluded) expected.push_back(member);
      }
      const auto expected_state =
          expected.empty() ? CanonicalWindowFrameState::empty
                           : CanonicalWindowFrameState::nonempty;
      if (frame.effective_row_indices != expected ||
          frame.effective_state != expected_state ||
          frame.excluded_row_count !=
              (*frame.base_end_exclusive - *frame.base_begin) -
                  expected.size()) {
        return false;
      }
    } else if ((frame.base_state != CanonicalWindowFrameState::empty &&
                frame.base_state !=
                    CanonicalWindowFrameState::reversed_to_empty) ||
               frame.effective_state != frame.base_state ||
               frame.base_begin.has_value() ||
               frame.base_end_exclusive.has_value() ||
               frame.excluded_row_count != 0 ||
               !frame.effective_row_indices.empty()) {
      return false;
    }
  }
  std::size_t row = 0;
  std::size_t expected_partition_id = 0;
  while (row < row_count) {
    const auto partition_begin = row;
    const auto partition_end =
        frames.row_metadata[partition_begin].partition_end_exclusive;
    std::size_t expected_peer_id = 0;
    while (row < partition_end) {
      const auto peer_begin = row;
      const auto peer_end = frames.row_metadata[peer_begin].peer_end_exclusive;
      for (std::size_t member = peer_begin; member < peer_end; ++member) {
        const auto& metadata = frames.row_metadata[member];
        if (*metadata.partition_id != expected_partition_id ||
            *metadata.peer_group_id != expected_peer_id ||
            metadata.partition_begin != partition_begin ||
            metadata.partition_end_exclusive != partition_end ||
            metadata.peer_begin != peer_begin ||
            metadata.peer_end_exclusive != peer_end) {
          return false;
        }
      }
      row = peer_end;
      ++expected_peer_id;
    }
    ++expected_partition_id;
  }
  return true;
}

scratchbird::engine::internal_api::EngineTypedValue RankingInt64Value(
    const scratchbird::engine::internal_api::EngineDescriptor& descriptor,
    const std::uint64_t value) {
  scratchbird::engine::internal_api::EngineTypedValue output;
  output.descriptor = descriptor;
  output.encoded_value = std::to_string(value);
  output.state =
      scratchbird::engine::internal_api::EngineValueState::value;
  return output;
}

std::string ExactRatioText(std::uint64_t numerator,
                           const std::uint64_t denominator) {
  if (denominator == 0) return {};
  std::string output = std::to_string(numerator / denominator);
  auto remainder = numerator % denominator;
  if (remainder == 0) return output;
  output.push_back('.');
  for (std::size_t digit = 0; digit < 34 && remainder != 0; ++digit) {
    const auto scaled = static_cast<unsigned __int128>(remainder) * 10;
    output.push_back(static_cast<char>('0' + scaled / denominator));
    remainder = static_cast<std::uint64_t>(scaled % denominator);
  }
  return output;
}

std::optional<scratchbird::engine::internal_api::EngineTypedValue>
RankingRealValue(
    const scratchbird::engine::internal_api::EngineDescriptor& descriptor,
    const std::uint64_t numerator,
    const std::uint64_t denominator) {
  namespace dt = scratchbird::core::datatypes;
  const auto target_type =
      dt::CanonicalTypeIdFromStableName(descriptor.canonical_type_name);
  dt::DatatypeCastRequest cast;
  cast.value.type_id = dt::CanonicalTypeId::decimal_float;
  cast.value.encoded_value = ExactRatioText(numerator, denominator);
  cast.target_type_id = target_type;
  cast.explicit_cast = true;
  const auto converted = dt::CastDatatypeValue(cast);
  if (!converted.ok() || converted.value.is_null) return std::nullopt;
  scratchbird::engine::internal_api::EngineTypedValue output;
  output.descriptor = descriptor;
  output.encoded_value = converted.value.encoded_value;
  output.state =
      scratchbird::engine::internal_api::EngineValueState::value;
  return output;
}

}  // namespace

CanonicalWindowIntegerRankValueResult ComputeCanonicalWindowIntegerRankValue(
    const CanonicalWindowIntegerRankValueRequest& request) {
  CanonicalWindowIntegerRankValueResult result;
  const bool dense_rank = request.builtin_id == "sb.window.dense_rank";
  const auto function = dense_rank
                            ? CanonicalWindowRankingFunction::dense_rank
                            : CanonicalWindowRankingFunction::rank;
  const std::string_view expected_builtin =
      dense_rank ? "sb.window.dense_rank" : "sb.window.rank";
  const std::string_view expected_uuid =
      dense_rank ? kWindowDenseRankUuid : kWindowRankUuid;
  const std::string_view display_name = dense_rank ? "DENSE_RANK" : "RANK";
  const auto refuse = [&](std::string detail) {
    result = {};
    result.diagnostic = WindowRankingRefusal(function, std::move(detail));
    return result;
  };
  const auto type_uuid =
      RankingDescriptorField(request.output_descriptor, "type_uuid");
  const auto nullability =
      RankingDescriptorField(request.output_descriptor, "nullability");
  if (request.function_abi_version != 1 ||
      request.builtin_id != expected_builtin ||
      request.function_uuid != expected_uuid ||
      !IsCanonicalUuid(request.function_uuid) ||
      request.one_based_rank == 0 ||
      request.one_based_rank >
          static_cast<std::uint64_t>(
              std::numeric_limits<std::int64_t>::max()) ||
      !IsCanonicalUuid(
          request.output_descriptor.descriptor_uuid.canonical) ||
      request.output_descriptor.descriptor_kind != "scalar" ||
      request.output_descriptor.canonical_type_name != "int64" ||
      request.output_descriptor.encoded_descriptor.empty() ||
      !type_uuid.has_value() || !IsCanonicalUuid(*type_uuid) ||
      !nullability.has_value() || *nullability != "non_null" ||
      request.output_descriptor.descriptor_uuid.canonical == *type_uuid ||
      request.output_descriptor.descriptor_uuid.canonical ==
          request.function_uuid) {
    return refuse(std::string(display_name) +
                  " value lacks its exact registry identity, descriptor, "
                  "or positive int64 position");
  }
  result.value = RankingInt64Value(request.output_descriptor,
                                   request.one_based_rank);
  result.diagnostic = {};
  return result;
}

CanonicalWindowPercentRankValueResult ComputeCanonicalWindowPercentRankValue(
    const CanonicalWindowPercentRankValueRequest& request) {
  CanonicalWindowPercentRankValueResult result;
  const auto refuse = [&](std::string detail) {
    result = {};
    result.diagnostic = WindowRankingRefusal(
        CanonicalWindowRankingFunction::percent_rank, std::move(detail));
    return result;
  };
  const auto type_uuid =
      RankingDescriptorField(request.output_descriptor, "type_uuid");
  const auto nullability =
      RankingDescriptorField(request.output_descriptor, "nullability");
  if (request.function_abi_version != 1 ||
      request.builtin_id != "sb.window.percent_rank" ||
      request.function_uuid != kWindowPercentRankUuid ||
      !IsCanonicalUuid(request.function_uuid) ||
      request.partition_row_count == 0 || request.one_based_rank == 0 ||
      request.one_based_rank > request.partition_row_count ||
      request.partition_row_count >
          static_cast<std::uint64_t>(
              std::numeric_limits<std::int64_t>::max()) ||
      !IsCanonicalUuid(
          request.output_descriptor.descriptor_uuid.canonical) ||
      request.output_descriptor.descriptor_kind != "scalar" ||
      request.output_descriptor.canonical_type_name != "real64" ||
      request.output_descriptor.encoded_descriptor.empty() ||
      !type_uuid.has_value() || !IsCanonicalUuid(*type_uuid) ||
      !nullability.has_value() || *nullability != "non_null" ||
      request.output_descriptor.descriptor_uuid.canonical == *type_uuid ||
      request.output_descriptor.descriptor_uuid.canonical ==
          request.function_uuid) {
    return refuse(
        "PERCENT_RANK value lacks its exact registry identity, descriptor, "
        "rank, or partition cardinality");
  }
  const auto value =
      request.partition_row_count <= 1
          ? RankingRealValue(request.output_descriptor, 0, 1)
          : RankingRealValue(request.output_descriptor,
                             request.one_based_rank - 1,
                             request.partition_row_count - 1);
  if (!value.has_value()) {
    return refuse("PERCENT_RANK result conversion failed");
  }
  result.value = *value;
  result.diagnostic = {};
  return result;
}

CanonicalWindowCumeDistValueResult ComputeCanonicalWindowCumeDistValue(
    const CanonicalWindowCumeDistValueRequest& request) {
  CanonicalWindowCumeDistValueResult result;
  const auto refuse = [&](std::string detail) {
    result = {};
    result.diagnostic = WindowRankingRefusal(
        CanonicalWindowRankingFunction::cume_dist, std::move(detail));
    return result;
  };
  const auto type_uuid =
      RankingDescriptorField(request.output_descriptor, "type_uuid");
  const auto nullability =
      RankingDescriptorField(request.output_descriptor, "nullability");
  if (request.function_abi_version != 1 ||
      request.builtin_id != "sb.window.cume_dist" ||
      request.function_uuid != kWindowCumeDistUuid ||
      !IsCanonicalUuid(request.function_uuid) ||
      request.partition_row_count == 0 ||
      request.cumulative_row_count == 0 ||
      request.cumulative_row_count > request.partition_row_count ||
      request.partition_row_count >
          static_cast<std::uint64_t>(
              std::numeric_limits<std::int64_t>::max()) ||
      !IsCanonicalUuid(
          request.output_descriptor.descriptor_uuid.canonical) ||
      request.output_descriptor.descriptor_kind != "scalar" ||
      request.output_descriptor.canonical_type_name != "real64" ||
      request.output_descriptor.encoded_descriptor.empty() ||
      !type_uuid.has_value() || !IsCanonicalUuid(*type_uuid) ||
      !nullability.has_value() || *nullability != "non_null" ||
      request.output_descriptor.descriptor_uuid.canonical == *type_uuid ||
      request.output_descriptor.descriptor_uuid.canonical ==
          request.function_uuid) {
    return refuse(
        "CUME_DIST value lacks its exact registry identity, descriptor, "
        "cumulative row count, or partition cardinality");
  }
  const auto value = RankingRealValue(request.output_descriptor,
                                      request.cumulative_row_count,
                                      request.partition_row_count);
  if (!value.has_value()) {
    return refuse("CUME_DIST result conversion failed");
  }
  result.value = *value;
  result.diagnostic = {};
  return result;
}

CanonicalWindowNtileValueResult ComputeCanonicalWindowNtileValue(
    const CanonicalWindowNtileValueRequest& request) {
  CanonicalWindowNtileValueResult result;
  const auto refuse = [&](std::string detail) {
    result = {};
    result.diagnostic = WindowRankingRefusal(
        CanonicalWindowRankingFunction::ntile, std::move(detail));
    return result;
  };
  const auto type_uuid =
      RankingDescriptorField(request.output_descriptor, "type_uuid");
  const auto nullability =
      RankingDescriptorField(request.output_descriptor, "nullability");
  if (request.function_abi_version != 1 ||
      request.builtin_id != "sb.window.ntile" ||
      request.function_uuid != kWindowNtileUuid ||
      !IsCanonicalUuid(request.function_uuid) ||
      request.partition_row_count == 0 ||
      request.zero_based_partition_position >= request.partition_row_count ||
      request.partition_row_count >
          static_cast<std::uint64_t>(
              std::numeric_limits<std::int64_t>::max()) ||
      request.bucket_count == 0 ||
      request.bucket_count >
          static_cast<std::uint64_t>(
              std::numeric_limits<std::int64_t>::max()) ||
      !IsCanonicalUuid(
          request.output_descriptor.descriptor_uuid.canonical) ||
      request.output_descriptor.descriptor_kind != "scalar" ||
      request.output_descriptor.canonical_type_name != "int64" ||
      request.output_descriptor.encoded_descriptor.empty() ||
      !type_uuid.has_value() || !IsCanonicalUuid(*type_uuid) ||
      !nullability.has_value() || *nullability != "non_null" ||
      request.output_descriptor.descriptor_uuid.canonical == *type_uuid ||
      request.output_descriptor.descriptor_uuid.canonical ==
          request.function_uuid) {
    return refuse(
        "NTILE value lacks its exact registry identity, descriptor, positive "
        "bucket count, or partition position");
  }

  const auto base_size = request.partition_row_count / request.bucket_count;
  const auto larger_bucket_count =
      request.partition_row_count % request.bucket_count;
  const auto larger_bucket_size = base_size + 1;
  if (larger_bucket_count != 0 &&
      larger_bucket_size > std::numeric_limits<std::uint64_t>::max() /
                               larger_bucket_count) {
    return refuse("NTILE bucket boundary arithmetic overflowed");
  }
  const auto larger_rows = larger_bucket_count * larger_bucket_size;
  std::uint64_t bucket = 0;
  if (request.zero_based_partition_position < larger_rows) {
    bucket = request.zero_based_partition_position / larger_bucket_size + 1;
  } else {
    if (base_size == 0) {
      return refuse("NTILE reached an impossible zero-width bucket");
    }
    bucket = larger_bucket_count +
             (request.zero_based_partition_position - larger_rows) /
                 base_size +
             1;
  }
  if (bucket == 0 || bucket > request.bucket_count ||
      bucket > request.partition_row_count) {
    return refuse("NTILE produced a bucket outside its bounded partition");
  }
  result.value = RankingInt64Value(request.output_descriptor, bucket);
  result.diagnostic = {};
  return result;
}

// QOW-SOURCE-WIN-006-V1
// Ranking functions consume the exact partition and typed peer ranges created
// by QOW-401 after QOW-402 has validated the complete frame and exclusion.
// These six functions intentionally ignore effective-frame extent only after
// that validation, as required by WINDOW_CORE.
static CanonicalWindowRankingResult ExecuteCanonicalWindowRankingStrategy(
    const CanonicalWindowRankingRequest& request) {
  CanonicalWindowRankingResult result;
  const auto refuse = [&](std::string detail) {
    result = {};
    result.diagnostic =
        WindowRankingRefusal(request.function, std::move(detail));
    return result;
  };
  const auto authority_validation = RevalidateCanonicalWindowFrameAuthority(
      request.mga_authority, request.frames);
  if (!authority_validation.ok) {
    return refuse(authority_validation.detail);
  }
  const auto& execution_authority = CanonicalWindowFrameExecutionAuthority(
      request.mga_authority, request.frames);
  const auto expected_uuid = RankingFunctionUuid(request.function);
  if (expected_uuid.empty() || request.function_uuid != expected_uuid ||
      !IsCanonicalUuid(request.function_uuid)) {
    return refuse("ranking function kind and registry UUID do not match");
  }
  if (!CanonicalWindowFrameEvidenceValid(request.frames)) {
    return refuse("ranking input is not canonical QOW-402 frame evidence");
  }
  if (request.parser_execution_authority_claimed ||
      request.transaction_finality_claimed ||
      request.recovery_authority_claimed) {
    return refuse("window ranking attempted to claim engine MGA authority");
  }
  const auto row_count = request.frames.ordered_batch.rows.size();
  if (request.maximum_output_rows == 0 ||
      row_count > request.maximum_output_rows ||
      row_count > static_cast<std::size_t>(
                      std::numeric_limits<std::int64_t>::max())) {
    return refuse("window ranking output resource bound was exceeded");
  }
  if (!RankingOutputDescriptorValid(request)) {
    return refuse(
        "ranking output descriptor is malformed, nullable, or identity-substituted");
  }

  std::uint64_t buckets = 0;
  if (request.function == CanonicalWindowRankingFunction::ntile) {
    if (!request.ntile_bucket_count.has_value()) {
      return refuse("NTILE requires an explicitly present bucket count");
    }
    if (!CanonicalRankingNtileOperand(*request.ntile_bucket_count,
                                     request, &buckets)) {
      return refuse(
          "NTILE bucket count must be an independent positive non-NULL canonical int64");
    }
  } else if (request.ntile_bucket_count.has_value()) {
    return refuse("non-NTILE ranking function carries an NTILE operand");
  }

  result.values.reserve(row_count);
  for (std::size_t row = 0; row < row_count; ++row) {
    const auto& metadata = request.frames.row_metadata[row];
    const auto partition_rows =
        metadata.partition_end_exclusive - metadata.partition_begin;
    const auto partition_position = row - metadata.partition_begin;
    const auto rank = metadata.peer_begin - metadata.partition_begin + 1;
    const auto dense_rank = *metadata.peer_group_id + 1;
    const auto cume_rows =
        metadata.peer_end_exclusive - metadata.partition_begin;
    switch (request.function) {
      case CanonicalWindowRankingFunction::row_number:
        result.values.push_back(RankingInt64Value(
            request.output_descriptor, partition_position + 1));
        break;
      case CanonicalWindowRankingFunction::rank:
        {
          CanonicalWindowIntegerRankValueRequest rank_request;
          rank_request.function_abi_version = 1;
          rank_request.builtin_id = "sb.window.rank";
          rank_request.function_uuid = request.function_uuid;
          rank_request.output_descriptor = request.output_descriptor;
          rank_request.one_based_rank = rank;
          auto rank_value =
              ComputeCanonicalWindowIntegerRankValue(rank_request);
          if (!rank_value.diagnostic.ok) {
            return refuse(rank_value.diagnostic.detail);
          }
          result.values.push_back(std::move(rank_value.value));
        }
        break;
      case CanonicalWindowRankingFunction::dense_rank:
        {
          CanonicalWindowIntegerRankValueRequest rank_request;
          rank_request.function_abi_version = 1;
          rank_request.builtin_id = "sb.window.dense_rank";
          rank_request.function_uuid = request.function_uuid;
          rank_request.output_descriptor = request.output_descriptor;
          rank_request.one_based_rank = dense_rank;
          auto rank_value =
              ComputeCanonicalWindowIntegerRankValue(rank_request);
          if (!rank_value.diagnostic.ok) {
            return refuse(rank_value.diagnostic.detail);
          }
          result.values.push_back(std::move(rank_value.value));
        }
        break;
      case CanonicalWindowRankingFunction::percent_rank: {
        const auto value = partition_rows <= 1
                               ? RankingRealValue(request.output_descriptor, 0, 1)
                               : RankingRealValue(request.output_descriptor,
                                                  rank - 1,
                                                  partition_rows - 1);
        if (!value.has_value()) {
          return refuse("PERCENT_RANK result conversion failed");
        }
        result.values.push_back(*value);
        break;
      }
      case CanonicalWindowRankingFunction::cume_dist: {
        const auto value = RankingRealValue(request.output_descriptor,
                                            cume_rows, partition_rows);
        if (!value.has_value()) {
          return refuse("CUME_DIST result conversion failed");
        }
        result.values.push_back(*value);
        break;
      }
      case CanonicalWindowRankingFunction::ntile: {
        CanonicalWindowNtileValueRequest ntile_request;
        ntile_request.function_abi_version = 1;
        ntile_request.builtin_id = "sb.window.ntile";
        ntile_request.function_uuid = request.function_uuid;
        ntile_request.output_descriptor = request.output_descriptor;
        ntile_request.zero_based_partition_position = partition_position;
        ntile_request.partition_row_count = partition_rows;
        ntile_request.bucket_count = buckets;
        auto ntile_value = ComputeCanonicalWindowNtileValue(ntile_request);
        if (!ntile_value.diagnostic.ok) {
          return refuse(ntile_value.diagnostic.detail);
        }
        result.values.push_back(std::move(ntile_value.value));
        break;
      }
    }
  }

  const auto result_authority = RevalidateCanonicalWindowFrameAuthority(
      request.mga_authority, request.frames);
  if (!result_authority.ok) return refuse(result_authority.detail);
  result.diagnostic = {};
  result.function = request.function;
  result.function_uuid = request.function_uuid;
  result.output_descriptor = request.output_descriptor;
  if (request.function == CanonicalWindowRankingFunction::ntile) {
    result.resolved_ntile_bucket_count = buckets;
  }
  result.every_function_operand_consumed = true;
  result.partition_peer_metadata_consumed = true;
  result.frame_and_exclusion_validated_then_ignored = true;
  result.authority = request.frames.authority;
  result.window_property_uuid = request.frames.window_property_uuid;
  result.partition_property_uuid = request.frames.partition_property_uuid;
  result.ordering_property_uuid = request.frames.ordering_property_uuid;
  result.term_binding_evidence_uuid =
      request.frames.term_binding_evidence_uuid;
  result.deterministic_tie_evidence_uuid =
      request.frames.deterministic_tie_evidence_uuid;
  result.frame_property_binding_evidence_uuid =
      request.frames.frame_property_binding_evidence_uuid;
  result.selected_plan_uuid = request.frames.selected_plan_uuid;
  result.executed_physical_node_id =
      request.frames.executed_physical_node_id;
  result.causal_counter_id = request.frames.causal_counter_id;
  result.mga_statement_context = execution_authority.statement_context;
  return result;
}

namespace {

constexpr std::string_view kWindowLagUuid =
    "019de5fc-2400-782c-8436-9ac310301738";
constexpr std::string_view kWindowLeadUuid =
    "019de5fc-2400-7a06-bc3c-6747cf5be66f";
constexpr std::string_view kWindowFirstValueUuid =
    "019de5fc-2400-7264-90fb-d25bd0f806f2";
constexpr std::string_view kWindowLastValueUuid =
    "019de5fc-2400-7d23-a5be-7ed3f1a5c3ec";
constexpr std::string_view kWindowNthValueUuid =
    "019de5fc-2400-7dc9-80e6-9f2ccf08076f";

std::string_view WindowValueFunctionUuid(
    const CanonicalWindowValueFunction function) {
  switch (function) {
    case CanonicalWindowValueFunction::lag:
      return kWindowLagUuid;
    case CanonicalWindowValueFunction::lead:
      return kWindowLeadUuid;
    case CanonicalWindowValueFunction::first_value:
      return kWindowFirstValueUuid;
    case CanonicalWindowValueFunction::last_value:
      return kWindowLastValueUuid;
    case CanonicalWindowValueFunction::nth_value:
      return kWindowNthValueUuid;
  }
  return {};
}

bool SameWindowValueDescriptor(
    const scratchbird::engine::internal_api::EngineDescriptor& left,
    const scratchbird::engine::internal_api::EngineDescriptor& right) {
  return left.descriptor_uuid.canonical == right.descriptor_uuid.canonical &&
         left.descriptor_kind == right.descriptor_kind &&
         left.canonical_type_name == right.canonical_type_name &&
         left.encoded_descriptor == right.encoded_descriptor;
}

bool SameWindowResultColumn(const ExecutorColumnDescriptor& left,
                            const ExecutorColumnDescriptor& right) {
  return left.descriptor_id == right.descriptor_id &&
         left.stable_name == right.stable_name &&
         left.nullable == right.nullable &&
         SameWindowValueDescriptor(left.descriptor, right.descriptor);
}

bool WindowValueResultColumnValid(
    const ExecutorColumnDescriptor& result_column,
    const ExecutorColumnDescriptor& value_column) {
  const auto type_uuid =
      RankingDescriptorField(result_column.descriptor, "type_uuid");
  const auto nullability =
      RankingDescriptorField(result_column.descriptor, "nullability");
  const bool exact_nullable_descriptor =
      value_column.nullable &&
      SameWindowValueDescriptor(result_column.descriptor,
                                value_column.descriptor);
  const bool derived_nullable_descriptor =
      CanonicalDerivedDescriptorTypeMatches(
          value_column.descriptor, value_column.nullable,
          result_column.descriptor, true);
  return result_column.descriptor_id != 0 &&
         result_column.descriptor_id != value_column.descriptor_id &&
         !result_column.stable_name.empty() && result_column.nullable &&
         IsCanonicalUuid(result_column.descriptor.descriptor_uuid.canonical) &&
         result_column.descriptor.descriptor_kind == "scalar" &&
         !result_column.descriptor.canonical_type_name.empty() &&
         !result_column.descriptor.encoded_descriptor.empty() &&
         type_uuid.has_value() && IsCanonicalUuid(*type_uuid) &&
         result_column.descriptor.descriptor_uuid.canonical != *type_uuid &&
         nullability.has_value() && *nullability == "nullable" &&
         (exact_nullable_descriptor || derived_nullable_descriptor);
}

bool WindowAssignmentCastAdmitted(
    const scratchbird::core::datatypes::DatatypeCastCategory category) {
  namespace dt = scratchbird::core::datatypes;
  return category == dt::DatatypeCastCategory::identity ||
         category == dt::DatatypeCastCategory::lossless_implicit;
}

std::optional<scratchbird::engine::internal_api::EngineTypedValue>
ConvertWindowAssignmentValue(
    const scratchbird::engine::internal_api::EngineTypedValue& source,
    const scratchbird::engine::internal_api::EngineDescriptor& target,
    std::string* detail) {
  namespace api = scratchbird::engine::internal_api;
  namespace dt = scratchbird::core::datatypes;
  const auto fail = [&](std::string reason)
      -> std::optional<api::EngineTypedValue> {
    if (detail != nullptr) *detail = std::move(reason);
    return std::nullopt;
  };
  if (!IsCanonicalUuid(source.descriptor.descriptor_uuid.canonical) ||
      source.descriptor.descriptor_kind != "scalar" ||
      source.descriptor.canonical_type_name.empty() ||
      source.descriptor.encoded_descriptor.empty() ||
      (source.state != api::EngineValueState::value &&
       source.state != api::EngineValueState::sql_null)) {
    return fail("window value is not a canonical scalar assignment operand");
  }
  if ((source.state == api::EngineValueState::sql_null &&
       (!source.is_null || !source.encoded_value.empty() ||
        !source.binary_value.empty())) ||
      (source.state == api::EngineValueState::value && source.is_null)) {
    return fail("window value carries contradictory SQL NULL state");
  }
  const auto source_type_uuid =
      RankingDescriptorField(source.descriptor, "type_uuid");
  const auto source_nullability =
      RankingDescriptorField(source.descriptor, "nullability");
  if (!source_type_uuid.has_value() ||
      !IsCanonicalUuid(*source_type_uuid) ||
      source.descriptor.descriptor_uuid.canonical == *source_type_uuid ||
      !source_nullability.has_value() ||
      (*source_nullability != "non_null" &&
       *source_nullability != "nullable") ||
      (source.state == api::EngineValueState::sql_null &&
       *source_nullability != "nullable")) {
    return fail("window value descriptor type or nullability is malformed");
  }
  const auto source_type = dt::CanonicalTypeIdFromStableName(
      source.descriptor.canonical_type_name);
  const auto target_type =
      dt::CanonicalTypeIdFromStableName(target.canonical_type_name);
  if (source_type == dt::CanonicalTypeId::unknown ||
      target_type == dt::CanonicalTypeId::unknown ||
      !WindowAssignmentCastAdmitted(
          dt::ClassifyDatatypeCast(source_type, target_type))) {
    return fail("window value is not assignment-compatible with the result");
  }
  if (source_type == target_type &&
      source.descriptor.encoded_descriptor != target.encoded_descriptor &&
      !CanonicalDerivedDescriptorTypeMatches(
          source.descriptor, *source_nullability == "nullable", target,
          true)) {
    return fail("window value descriptor attributes differ from the result");
  }
  dt::DatatypeCastRequest conversion;
  conversion.value.type_id = source_type;
  conversion.value.encoded_value = source.encoded_value;
  conversion.value.is_null = source.state == api::EngineValueState::sql_null;
  conversion.target_type_id = target_type;
  const auto cast = dt::CastDatatypeValue(conversion);
  if (!cast.ok()) {
    std::string cast_detail = cast.diagnostic.diagnostic_code.empty()
                                  ? "window assignment conversion failed"
                                  : cast.diagnostic.diagnostic_code;
    if (!cast.diagnostic.arguments.empty()) {
      cast_detail += ":" + cast.diagnostic.arguments.front().value;
    }
    return fail(std::move(cast_detail));
  }
  api::EngineTypedValue converted;
  converted.descriptor = target;
  converted.encoded_value = cast.value.encoded_value;
  if (source_type == target_type && !cast.value.is_null) {
    converted.binary_value = source.binary_value;
  }
  converted.is_null = cast.value.is_null;
  converted.state = cast.value.is_null ? api::EngineValueState::sql_null
                                       : api::EngineValueState::value;
  return converted;
}

scratchbird::engine::internal_api::EngineTypedValue WindowTypedNull(
    const scratchbird::engine::internal_api::EngineDescriptor& descriptor) {
  scratchbird::engine::internal_api::EngineTypedValue value;
  value.descriptor = descriptor;
  value.is_null = true;
  value.state =
      scratchbird::engine::internal_api::EngineValueState::sql_null;
  return value;
}

DescriptorRuntimeDiagnostic WindowValueRefusal(std::string code,
                                               std::string detail) {
  DescriptorRuntimeDiagnostic diagnostic;
  diagnostic.ok = false;
  diagnostic.diagnostic_code = std::move(code);
  diagnostic.detail = std::move(detail);
  return diagnostic;
}

bool CanonicalWindowInt64Operand(
    const scratchbird::engine::internal_api::EngineTypedValue& value,
    const CanonicalWindowValueRequest& request,
    std::int64_t* decoded) {
  namespace api = scratchbird::engine::internal_api;
  namespace dt = scratchbird::core::datatypes;
  if (decoded == nullptr ||
      !IsCanonicalUuid(value.descriptor.descriptor_uuid.canonical) ||
      value.descriptor.descriptor_kind != "scalar" ||
      value.descriptor.encoded_descriptor.empty() ||
      dt::CanonicalTypeIdFromStableName(
          value.descriptor.canonical_type_name) != dt::CanonicalTypeId::int64 ||
      value.state != api::EngineValueState::value || value.is_null ||
      !value.binary_value.empty() ||
      value.descriptor.descriptor_uuid.canonical ==
          request.result_column.descriptor.descriptor_uuid.canonical ||
      !RankingIdentityIndependent(
          value.descriptor.descriptor_uuid.canonical, request.frames,
          request.function_uuid)) {
    return false;
  }
  const auto type_uuid =
      RankingDescriptorField(value.descriptor, "type_uuid");
  const auto nullability =
      RankingDescriptorField(value.descriptor, "nullability");
  if (!type_uuid.has_value() || !IsCanonicalUuid(*type_uuid) ||
      value.descriptor.descriptor_uuid.canonical == *type_uuid ||
      !RankingIdentityIndependent(*type_uuid, request.frames,
                                  request.function_uuid) ||
      !nullability.has_value() ||
      (*nullability != "non_null" && *nullability != "nullable")) {
    return false;
  }
  const auto parsed = DecodeInt64Value(value);
  if (!parsed.ok()) return false;
  *decoded = parsed.value;
  return true;
}

}  // namespace

// QOW-SOURCE-WIN-007-V1
// QOW-SOURCE-WIN-008-V1
// QOW-SOURCE-WIN-009-V1
// QOW-SOURCE-WIN-010-V1
// QOW-SOURCE-WIN-011-V1
// Navigation and value functions consume the same typed QOW-401/QOW-402
// partition, peer, frame, exclusion, descriptor, and MGA evidence. LAG/LEAD
// validate but ignore the effective frame; FIRST/LAST/NTH select only from the
// effective row-index vector remaining after exclusion.
static CanonicalWindowValueResult ExecuteCanonicalWindowValueStrategy(
    const CanonicalWindowValueRequest& request) {
  namespace api = scratchbird::engine::internal_api;
  CanonicalWindowValueResult result;
  const auto refuse = [&](std::string code, std::string detail) {
    result = {};
    result.diagnostic =
        WindowValueRefusal(std::move(code), std::move(detail));
    return result;
  };
  const auto authority_validation = RevalidateCanonicalWindowFrameAuthority(
      request.mga_authority, request.frames);
  if (!authority_validation.ok) {
    return refuse(authority_validation.diagnostic_code,
                  authority_validation.detail);
  }
  const auto& execution_authority = CanonicalWindowFrameExecutionAuthority(
      request.mga_authority, request.frames);
  const auto expected_uuid = WindowValueFunctionUuid(request.function);
  if (expected_uuid.empty() || request.function_uuid != expected_uuid ||
      !IsCanonicalUuid(request.function_uuid)) {
    return refuse("QOW-DIAG-WINDOW-FUNCTION-DESCRIPTOR",
                  "window value function kind and registry UUID do not match");
  }
  if (!CanonicalWindowFrameEvidenceValid(request.frames)) {
    return refuse("QOW-DIAG-WINDOW-FRAME",
                  "window value input is not canonical QOW-402 frame evidence");
  }
  if (request.frames.operator_local_stage) {
    const auto selected_node = std::ranges::find_if(
        request.frames.physical_dag.nodes, [&](const auto& node) {
          return node.physical_node_id ==
                 request.frames.executed_physical_node_id;
        });
    const bool exact_lag =
        request.frames.operator_local_parent_implementation_id ==
            "window.lag.v1" &&
        request.function == CanonicalWindowValueFunction::lag;
    const bool exact_lead =
        request.frames.operator_local_parent_implementation_id ==
            "window.lead.v1" &&
        request.function == CanonicalWindowValueFunction::lead;
    const bool exact_first_value =
        request.frames.operator_local_parent_implementation_id ==
            "window.first-value.v1" &&
        request.function == CanonicalWindowValueFunction::first_value;
    if ((!exact_lag && !exact_lead && !exact_first_value) ||
        selected_node == request.frames.physical_dag.nodes.end() ||
        selected_node->output_descriptor_ids.empty() ||
        request.result_column.descriptor_id == 0 ||
        selected_node->output_descriptor_ids.back() !=
            request.result_column.descriptor_id) {
      return refuse(
          "QOW-DIAG-WINDOW-FUNCTION-DESCRIPTOR",
          "operator-local value-window evidence is not bound to its published result");
    }
  }
  if (request.parser_execution_authority_claimed ||
      request.transaction_finality_claimed ||
      request.recovery_authority_claimed) {
    return refuse("QOW-DIAG-WINDOW-AUTHORITY",
                  "window value function attempted to claim engine MGA authority");
  }
  const auto row_count = request.frames.ordered_batch.rows.size();
  if (request.maximum_output_rows == 0 ||
      row_count > request.maximum_output_rows) {
    return refuse("QOW-DIAG-WINDOW-FRAME",
                  "window value output resource bound was exceeded");
  }

  std::optional<std::size_t> value_column_index;
  for (std::size_t column = 0;
       column < request.frames.ordered_batch.columns.size(); ++column) {
    if (request.frames.ordered_batch.columns[column].descriptor_id ==
        request.value_expression_descriptor_id) {
      if (value_column_index.has_value()) {
        return refuse("QOW-DIAG-WINDOW-FUNCTION-DESCRIPTOR",
                      "window value descriptor handle is ambiguous");
      }
      value_column_index = column;
    }
  }
  if (!value_column_index.has_value() ||
      !WindowValueResultColumnValid(
          request.result_column,
          request.frames.ordered_batch.columns[*value_column_index])) {
    return refuse("QOW-DIAG-WINDOW-FUNCTION-DESCRIPTOR",
                  "window value expression or result descriptor is unresolved");
  }

  std::vector<api::EngineTypedValue> source_values;
  source_values.reserve(row_count);
  std::string assignment_detail;
  for (const auto& row : request.frames.ordered_batch.rows) {
    const auto converted = ConvertWindowAssignmentValue(
        row.values[*value_column_index], request.result_column.descriptor,
        &assignment_detail);
    if (!converted.has_value()) {
      return refuse("QOW-DIAG-WINDOW-FUNCTION-DESCRIPTOR",
                    std::move(assignment_detail));
    }
    source_values.push_back(*converted);
  }

  const bool navigation =
      request.function == CanonicalWindowValueFunction::lag ||
      request.function == CanonicalWindowValueFunction::lead;
  std::vector<std::uint64_t> positions;
  std::vector<api::EngineTypedValue> defaults;
  if (navigation) {
    if (request.nth_values.has_value() || request.nth_origin.has_value() ||
        request.null_treatment.has_value()) {
      return refuse("QOW-DIAG-WINDOW-OFFSET",
                    "LAG/LEAD carries an NTH_VALUE-only operand");
    }
    if (request.default_values.has_value() &&
        !request.offset_values.has_value()) {
      return refuse("QOW-DIAG-WINDOW-OFFSET",
                    "LAG/LEAD default cannot omit the offset operand state");
    }
    positions.assign(row_count, 1);
    if (request.offset_values.has_value()) {
      if (request.offset_values->size() != row_count) {
        return refuse("QOW-DIAG-WINDOW-OFFSET",
                      "LAG/LEAD offset cardinality does not match input rows");
      }
      positions.clear();
      positions.reserve(row_count);
      for (const auto& value : *request.offset_values) {
        std::int64_t decoded = 0;
        if (!CanonicalWindowInt64Operand(value, request, &decoded) ||
            decoded < 0) {
          return refuse("QOW-DIAG-WINDOW-OFFSET",
                        "LAG/LEAD offset must be a non-NULL non-negative int64");
        }
        positions.push_back(static_cast<std::uint64_t>(decoded));
      }
    }
    if (request.default_values.has_value()) {
      if (request.default_values->size() != row_count) {
        return refuse("QOW-DIAG-WINDOW-DEFAULT-TYPE",
                      "LAG/LEAD default cardinality does not match input rows");
      }
      defaults.reserve(row_count);
      for (const auto& value : *request.default_values) {
        const auto converted = ConvertWindowAssignmentValue(
            value, request.result_column.descriptor, &assignment_detail);
        if (!converted.has_value()) {
          return refuse("QOW-DIAG-WINDOW-DEFAULT-TYPE",
                        std::move(assignment_detail));
        }
        defaults.push_back(*converted);
      }
    }
  } else if (request.function == CanonicalWindowValueFunction::nth_value) {
    if (request.offset_values.has_value() || request.default_values.has_value()) {
      return refuse("QOW-DIAG-WINDOW-NTH",
                    "NTH_VALUE carries a LAG/LEAD-only operand");
    }
    if (!request.nth_origin.has_value() ||
        !request.null_treatment.has_value()) {
      return refuse("QOW-DIAG-WINDOW-NULL-TREATMENT",
                    "NTH_VALUE origin and NULL treatment must be explicit");
    }
    if (*request.nth_origin != CanonicalWindowNthOrigin::from_first ||
        *request.null_treatment !=
            CanonicalWindowNullTreatment::respect_nulls) {
      return refuse("QOW-DIAG-WINDOW-NULL-TREATMENT",
                    "only NTH_VALUE FROM FIRST RESPECT NULLS is accepted");
    }
    if (!request.nth_values.has_value() ||
        request.nth_values->size() != row_count) {
      return refuse("QOW-DIAG-WINDOW-NTH",
                    "NTH_VALUE requires one explicit n operand per row");
    }
    positions.reserve(row_count);
    for (const auto& value : *request.nth_values) {
      std::int64_t decoded = 0;
      if (!CanonicalWindowInt64Operand(value, request, &decoded) ||
          decoded <= 0) {
        return refuse("QOW-DIAG-WINDOW-NTH",
                      "NTH_VALUE n must be a non-NULL positive int64");
      }
      positions.push_back(static_cast<std::uint64_t>(decoded));
    }
  } else if (request.offset_values.has_value() ||
             request.default_values.has_value() ||
             request.nth_values.has_value() || request.nth_origin.has_value() ||
             request.null_treatment.has_value()) {
    return refuse("QOW-DIAG-WINDOW-FRAME",
                  "FIRST_VALUE/LAST_VALUE carries an unrelated operand");
  }

  result.values.reserve(row_count);
  const auto typed_null = WindowTypedNull(request.result_column.descriptor);
  for (std::size_t row = 0; row < row_count; ++row) {
    if (navigation) {
      const auto& metadata = request.frames.row_metadata[row];
      const auto offset = positions[row];
      std::optional<std::size_t> target;
      if (request.function == CanonicalWindowValueFunction::lag) {
        const auto available = row - metadata.partition_begin;
        if (offset <= available) target = row - static_cast<std::size_t>(offset);
      } else {
        const auto available = metadata.partition_end_exclusive - row - 1;
        if (offset <= available) target = row + static_cast<std::size_t>(offset);
      }
      if (target.has_value()) {
        result.values.push_back(source_values[*target]);
      } else if (!defaults.empty()) {
        result.values.push_back(defaults[row]);
      } else {
        result.values.push_back(typed_null);
      }
      continue;
    }

    const auto& effective =
        request.frames.effective_frames[row].effective_row_indices;
    std::optional<std::size_t> target;
    if (request.function == CanonicalWindowValueFunction::first_value) {
      if (!effective.empty()) target = effective.front();
    } else if (request.function == CanonicalWindowValueFunction::last_value) {
      if (!effective.empty()) target = effective.back();
    } else {
      const auto nth = positions[row];
      if (nth <= effective.size()) {
        target = effective[static_cast<std::size_t>(nth - 1)];
      }
    }
    result.values.push_back(target.has_value() ? source_values[*target]
                                               : typed_null);
  }

  const auto result_authority = RevalidateCanonicalWindowFrameAuthority(
      request.mga_authority, request.frames);
  if (!result_authority.ok) {
    return refuse(result_authority.diagnostic_code,
                  result_authority.detail);
  }
  result.diagnostic = {};
  result.function = request.function;
  result.function_uuid = request.function_uuid;
  result.value_expression_descriptor_id =
      request.value_expression_descriptor_id;
  result.result_column = request.result_column;
  result.resolved_positions = positions;
  result.resolved_nth_origin = request.nth_origin;
  result.resolved_null_treatment = request.null_treatment;
  result.converted_source_value_count = source_values.size();
  result.converted_default_value_count = defaults.size();
  result.used_implicit_navigation_offset =
      navigation && !request.offset_values.has_value();
  result.explicit_navigation_default_present =
      navigation && request.default_values.has_value();
  result.every_function_operand_consumed = true;
  result.partition_metadata_consumed_for_navigation = navigation;
  result.effective_frame_membership_consumed = !navigation;
  result.frame_and_exclusion_validated = true;
  result.frame_and_exclusion_ignored_for_navigation = navigation;
  result.authority = request.frames.authority;
  result.window_property_uuid = request.frames.window_property_uuid;
  result.partition_property_uuid = request.frames.partition_property_uuid;
  result.ordering_property_uuid = request.frames.ordering_property_uuid;
  result.term_binding_evidence_uuid =
      request.frames.term_binding_evidence_uuid;
  result.deterministic_tie_evidence_uuid =
      request.frames.deterministic_tie_evidence_uuid;
  result.frame_property_binding_evidence_uuid =
      request.frames.frame_property_binding_evidence_uuid;
  result.selected_plan_uuid = request.frames.selected_plan_uuid;
  result.executed_physical_node_id =
      request.frames.executed_physical_node_id;
  result.causal_counter_id = request.frames.causal_counter_id;
  result.mga_statement_context = execution_authority.statement_context;
  return result;
}

// QOW-SOURCE-WIN-012-REGISTRY-BRIDGE-V1
// Execute the exact state strategy named by the optimizer-published aggregate
// window node. Direct execution admits recomputation or moving inverse state;
// the spill wrapper alone may consume the aggregate-state-spill implementation.
static CanonicalRegistryWindowAggregateResult
ExecuteCanonicalRegistryWindowAggregateSelected(
    const CanonicalRegistryWindowAggregateRequest& request,
    const bool spill_execution_context) {
  namespace api = scratchbird::engine::internal_api;

  CanonicalRegistryWindowAggregateResult result;
  result.descriptor = request.aggregate_template.descriptor;
  const auto refuse = [&](std::string code, std::string detail) {
    result = {};
    result.diagnostic.ok = false;
    result.diagnostic.diagnostic_code = std::move(code);
    result.diagnostic.detail = std::move(detail);
    result.transient_state_cleanup_proven = true;
    result.all_or_nothing_publication = true;
    return result;
  };
  if (!CanonicalWindowFrameEvidenceValid(request.frames)) {
    return refuse("QOW-DIAG-WINDOW-AGGREGATE-REGISTRY-FRAME",
                  "aggregate registry bridge lacks canonical frame evidence");
  }
  const auto authority_validation = RevalidateCanonicalWindowFrameAuthority(
      request.aggregate_template.mga_authority, request.frames);
  if (!authority_validation.ok) {
    return refuse("QOW-DIAG-WINDOW-AGGREGATE-REGISTRY-AUTHORITY",
                  authority_validation.detail);
  }
  if (request.frames.authority.owns_transaction_finality ||
      request.frames.authority.owns_recovery ||
      request.frames.authority.owns_parser_execution ||
      request.frames.authority.owns_visibility_outside_engine_mga ||
      request.frames.authority.wal_is_transaction_or_recovery_authority) {
    return refuse("QOW-DIAG-WINDOW-AGGREGATE-REGISTRY-AUTHORITY",
                  "aggregate registry window claimed authority outside engine MGA");
  }
  const auto row_count = request.frames.ordered_batch.rows.size();
  if (request.maximum_output_rows == 0 ||
      row_count > request.maximum_output_rows ||
      request.maximum_frame_input_row_count == 0 ||
      request.maximum_transition_count == 0 ||
      request.maximum_distinct_tuple_count == 0 ||
      request.maximum_order_comparison_count == 0 ||
      request.maximum_equality_key_generation_count == 0 ||
      request.maximum_equality_comparison_count == 0 ||
      request.maximum_combined_state_bytes == 0) {
    return refuse("QOW-DIAG-WINDOW-AGGREGATE-REGISTRY-RESOURCE",
                  "aggregate registry window resource contract is invalid");
  }
  const auto* template_filter_truth_values =
      request.aggregate_template.filter_truth_values.has_value()
          ? &*request.aggregate_template.filter_truth_values
          : nullptr;
  auto aggregate_template =
      CloneFoundationAggregateRequestWithoutRowsOrFilter(
          request.aggregate_template);
  aggregate_template.mga_authority = CanonicalWindowFrameExecutionAuthority(
      request.aggregate_template.mga_authority, request.frames);
  const auto aggregate_template_authority =
      RevalidateCanonicalExecutionMgaAuthority(
          aggregate_template.mga_authority,
          aggregate_template.physical_dag);
  if (!aggregate_template_authority.ok) {
    return refuse("QOW-DIAG-WINDOW-AGGREGATE-REGISTRY-AUTHORITY",
                  aggregate_template_authority.detail);
  }
  const auto* aggregate_registry_row = LookupCanonicalAggregateExactV1(
      aggregate_template.descriptor.abi_version,
      aggregate_template.descriptor.function,
      aggregate_template.descriptor.builtin_id,
      aggregate_template.descriptor.function_uuid);
  if (aggregate_registry_row == nullptr ||
      !aggregate_registry_row->executable ||
      !aggregate_registry_row->aggregate_as_window) {
    return refuse("QOW-DIAG-WINDOW-AGGREGATE-REGISTRY-DESCRIPTOR",
                  "aggregate descriptor is not admitted for the window bridge");
  }
  if (!aggregate_template.input_batch.columns.empty() ||
      !aggregate_template.input_batch.rows.empty()) {
    return refuse("QOW-DIAG-WINDOW-AGGREGATE-REGISTRY-AUTHORITY",
                  "aggregate template carries input outside the effective frame");
  }
  if (template_filter_truth_values != nullptr &&
      template_filter_truth_values->size() != row_count) {
    return refuse("QOW-DIAG-WINDOW-AGGREGATE-REGISTRY-FILTER",
                  "aggregate FILTER cardinality does not match window rows");
  }
  if (request.cancellation_requested) {
    auto cancelled = refuse(
        "QOW-DIAG-WINDOW-AGGREGATE-REGISTRY-CANCELLED",
        "aggregate window execution was cancelled before publication");
    cancelled.cancellation_observed = true;
    return cancelled;
  }

  const PhysicalNodeRecord* aggregate_node = nullptr;
  const PhysicalNodeRecord* input_node = nullptr;
  for (const auto& node : aggregate_template.physical_dag.nodes) {
    if (node.physical_node_id ==
        aggregate_template.selected_physical_node_id) {
      aggregate_node = &node;
    }
  }
  if (aggregate_template.selected_physical_node_id == 0 ||
      aggregate_template.selected_physical_node_id !=
          aggregate_template.physical_dag.root_physical_node_id ||
      aggregate_node == nullptr ||
      aggregate_node->node_kind != PhysicalNodeKind::kAggregate ||
      aggregate_node->input_physical_node_ids.size() != 1 ||
      aggregate_node->output_descriptor_ids !=
          std::vector<std::uint32_t>{
              aggregate_template.result_column.descriptor_id}) {
    return refuse("QOW-DIAG-WINDOW-AGGREGATE-REGISTRY-PHYSICAL",
                  "aggregate window kernel node is not exactly bound");
  }
  CanonicalRegistryWindowAggregateStateStrategy selected_state_strategy =
      CanonicalRegistryWindowAggregateStateStrategy::unknown;
  bool aggregate_state_spill_required = false;
  using AggregateRuntimeContext =
      detail::CanonicalAggregateRuntimeExecutionContext;
  auto aggregate_runtime_context =
      AggregateRuntimeContext::window_frame_recompute;
  if (aggregate_node->implementation_id.rfind(
          "window.aggregate-registry-", 0) != 0) {
    return refuse("QOW-DIAG-WINDOW-AGGREGATE-REGISTRY-PHYSICAL",
                  "aggregate implementation is not a window state strategy");
  }
  if (aggregate_node->implementation_id ==
      "window.aggregate-registry-frame-recompute.v1") {
    selected_state_strategy =
        CanonicalRegistryWindowAggregateStateStrategy::frame_recompute;
  } else if (aggregate_node->implementation_id ==
             "window.aggregate-registry-moving-inverse.v1") {
    selected_state_strategy =
        CanonicalRegistryWindowAggregateStateStrategy::moving_inverse;
    aggregate_runtime_context =
        AggregateRuntimeContext::window_moving_inverse;
  } else if (aggregate_node->implementation_id ==
             "window.aggregate-registry-state-spill.v1") {
    selected_state_strategy =
        CanonicalRegistryWindowAggregateStateStrategy::state_spill;
    aggregate_state_spill_required = true;
    aggregate_runtime_context =
        AggregateRuntimeContext::window_state_spill;
  } else {
    return refuse("QOW-DIAG-WINDOW-AGGREGATE-REGISTRY-STRATEGY",
                  "optimizer selected an unknown aggregate window state implementation");
  }
  if (aggregate_state_spill_required != spill_execution_context) {
    return refuse(
        "QOW-DIAG-WINDOW-AGGREGATE-REGISTRY-STRATEGY",
        aggregate_state_spill_required
            ? "selected aggregate window spill requires the spill runtime"
            : "aggregate window spill payload does not match the selected implementation");
  }
  if (selected_state_strategy ==
          CanonicalRegistryWindowAggregateStateStrategy::moving_inverse &&
      request.maximum_inverse_transition_count == 0) {
    return refuse("QOW-DIAG-WINDOW-AGGREGATE-REGISTRY-RESOURCE",
                  "moving aggregate inverse resource contract is invalid");
  }
  for (const auto& node : aggregate_template.physical_dag.nodes) {
    if (node.physical_node_id ==
        aggregate_node->input_physical_node_ids.front()) {
      input_node = &node;
      break;
    }
  }
  std::vector<std::uint32_t> frame_descriptor_ids;
  frame_descriptor_ids.reserve(request.frames.ordered_batch.columns.size());
  for (const auto& column : request.frames.ordered_batch.columns) {
    frame_descriptor_ids.push_back(column.descriptor_id);
  }
  if (input_node == nullptr ||
      input_node->output_descriptor_ids != frame_descriptor_ids ||
      aggregate_template.physical_dag.selected_plan_uuid !=
          request.frames.selected_plan_uuid ||
      aggregate_node->physical_node_id !=
          request.frames.executed_physical_node_id ||
      aggregate_node->causal_counter_id != request.frames.causal_counter_id) {
    return refuse("QOW-DIAG-WINDOW-AGGREGATE-REGISTRY-AUTHORITY",
                  "aggregate kernel and window frame authority diverge");
  }
  const auto node_memory_grant = FoundationNodeMemoryGrant(
      aggregate_template.physical_dag,
      aggregate_template.selected_physical_node_id);
  std::size_t input_payload_bytes = 0;
  std::size_t source_filter_bytes = 0;
  if (!node_memory_grant.has_value() ||
      !FoundationBatchPayloadBytes(request.frames.ordered_batch,
                                   &input_payload_bytes) ||
      (template_filter_truth_values != nullptr &&
       !FoundationFilterBytes(
           template_filter_truth_values->size(),
           &source_filter_bytes)) ||
      aggregate_template.retained_memory_bytes > *node_memory_grant ||
      input_payload_bytes >
          *node_memory_grant -
              aggregate_template.retained_memory_bytes ||
      source_filter_bytes >
          *node_memory_grant - aggregate_template.retained_memory_bytes -
              input_payload_bytes) {
    return refuse("QOW-DIAG-WINDOW-AGGREGATE-REGISTRY-RESOURCE",
                  "aggregate window selected-node memory grant is exhausted");
  }
  const auto maximum_combined_final_output_bytes =
      request.maximum_combined_final_output_bytes == 0
          ? *node_memory_grant
          : std::min(request.maximum_combined_final_output_bytes,
                     *node_memory_grant);
  const auto base_retained_memory_bytes =
      aggregate_template.retained_memory_bytes + input_payload_bytes +
      source_filter_bytes;

  const auto make_borrowed_frame_request = [&]() {
    return CloneFoundationAggregateRequestWithoutRowsOrFilter(
        aggregate_template, false);
  };
  auto preflight_request = make_borrowed_frame_request();
  preflight_request.retained_memory_bytes = base_retained_memory_bytes;
  if (preflight_request.filter_truth_values.has_value()) {
    preflight_request.filter_truth_values =
        std::vector<api::EngineSqlTruthValue>{};
  }
  DescriptorBatch preflight_input_batch;
  preflight_input_batch.columns = request.frames.ordered_batch.columns;
  auto preflight = detail::ExecuteCanonicalAggregateRuntimeBorrowedForContext(
      preflight_request, aggregate_template.physical_dag,
      preflight_input_batch, aggregate_runtime_context);
  if (!preflight.diagnostic.ok) {
    return refuse(preflight.diagnostic.diagnostic_code,
                  preflight.diagnostic.detail);
  }
  if (!PhysicalMgaStatementContextEqual(
          preflight.mga_statement_context,
          aggregate_template.mga_authority.statement_context)) {
    return refuse("QOW-DIAG-WINDOW-AGGREGATE-REGISTRY-AUTHORITY",
                  "aggregate window preflight returned a different MGA statement context");
  }
  const auto expected_modifier_count =
      (template_filter_truth_values != nullptr ? 1U : 0U) +
      (aggregate_template.distinct ? 1U : 0U) +
      (!aggregate_template.aggregate_order_terms.empty() ? 1U : 0U);
  const auto aggregate_receipts_match = [&](const auto& aggregate_result) {
    return aggregate_result.descriptor.abi_version ==
               aggregate_template.descriptor.abi_version &&
           aggregate_result.descriptor.function ==
               aggregate_template.descriptor.function &&
           aggregate_result.descriptor.builtin_id ==
               aggregate_template.descriptor.builtin_id &&
           aggregate_result.descriptor.function_uuid ==
               aggregate_template.descriptor.function_uuid &&
           aggregate_result.descriptor.count_star ==
               aggregate_template.descriptor.count_star &&
           aggregate_result.direct_argument_count ==
               aggregate_template.direct_arguments.size() &&
           aggregate_result.modifier_count == expected_modifier_count &&
           aggregate_result.aggregate_order_term_count ==
               aggregate_template.aggregate_order_terms.size() &&
           aggregate_result.every_descriptor_field_consumed &&
           aggregate_result.modifier_pipeline_validated &&
           aggregate_result.filter_modifier_applied ==
               (template_filter_truth_values != nullptr) &&
           aggregate_result.distinct_modifier_applied ==
               aggregate_template.distinct &&
           aggregate_result.filter_applied_before_distinct &&
           aggregate_result.distinct_applied_before_order &&
           aggregate_result.aggregate_order_applied ==
               !aggregate_template.aggregate_order_terms.empty();
  };
  if (!aggregate_receipts_match(preflight)) {
    return refuse("QOW-DIAG-WINDOW-AGGREGATE-REGISTRY-EVIDENCE",
                  "aggregate window preflight lost descriptor or modifier receipts");
  }
  result.peak_finalization_workspace_bytes =
      preflight.peak_finalization_workspace_bytes;
  preflight.output_batch = {};

  const auto within_total = [](const std::size_t next,
                               const std::size_t current,
                               const std::size_t maximum) {
    return current <= maximum && next <= maximum - current;
  };
  result.values.reserve(row_count);
  result.frame_row_indices.reserve(row_count);
  result.transition_row_indices.reserve(row_count);
  for (const auto& frame : request.frames.effective_frames) {
    if (!within_total(frame.effective_row_indices.size(),
                      result.frame_input_row_count,
                      request.maximum_frame_input_row_count)) {
      return refuse("QOW-DIAG-WINDOW-AGGREGATE-REGISTRY-RESOURCE",
                    "aggregate window frame-input bound is exceeded");
    }
    result.frame_input_row_count += frame.effective_row_indices.size();
    result.frame_row_indices.push_back(frame.effective_row_indices);
  }

  if (selected_state_strategy ==
      CanonicalRegistryWindowAggregateStateStrategy::moving_inverse) {
    const auto moving_retained_memory_bytes =
        aggregate_template.retained_memory_bytes + source_filter_bytes;
    if (source_filter_bytes >
        *node_memory_grant - moving_retained_memory_bytes -
            input_payload_bytes) {
      return refuse("QOW-DIAG-WINDOW-AGGREGATE-REGISTRY-RESOURCE",
                    "moving aggregate FILTER copy exceeds its selected-node grant");
    }
    CanonicalAggregateMovingRuntimeRequest moving_request;
    moving_request.aggregate_request = make_borrowed_frame_request();
    if (template_filter_truth_values != nullptr) {
      moving_request.aggregate_request.filter_truth_values =
          *template_filter_truth_values;
    }
    moving_request.aggregate_request.retained_memory_bytes =
        moving_retained_memory_bytes;
    moving_request.effective_frame_row_indices = result.frame_row_indices;
    moving_request.maximum_output_rows = request.maximum_output_rows;
    moving_request.maximum_addition_transition_count =
        request.maximum_transition_count;
    moving_request.maximum_inverse_transition_count =
        request.maximum_inverse_transition_count;
    moving_request.maximum_cumulative_state_bytes =
        request.maximum_combined_state_bytes;
    moving_request.maximum_combined_final_output_bytes =
        maximum_combined_final_output_bytes;
    moving_request.cancellation_requested = request.cancellation_requested;
    auto moving = ExecuteCanonicalAggregateMovingRuntime(
        moving_request, aggregate_template.physical_dag,
        request.frames.ordered_batch);
    if (!moving.diagnostic.ok) {
      return refuse(moving.diagnostic.diagnostic_code,
                    moving.diagnostic.detail);
    }
    if (!moving.moving_inverse_state_used ||
        moving.frame_recomputation_used ||
        moving.cancellation_observed ||
        !moving.transient_state_cleanup_proven ||
        !moving.all_or_nothing_publication ||
        moving.values.size() != row_count ||
        moving.selected_plan_uuid != request.frames.selected_plan_uuid ||
        moving.executed_physical_node_id !=
            request.frames.executed_physical_node_id ||
        moving.causal_counter_id != request.frames.causal_counter_id ||
        !PhysicalMgaStatementContextEqual(
            moving.mga_statement_context,
            aggregate_template.mga_authority.statement_context)) {
      return refuse("QOW-DIAG-WINDOW-AGGREGATE-REGISTRY-EVIDENCE",
                    "moving aggregate window evidence is inconsistent");
    }
    result.values = std::move(moving.values);
    // The inverse strategy is admitted only without FILTER, DISTINCT, or
    // aggregate ordering, so its transition membership is the effective
    // frame membership itself.
    result.transition_row_indices = result.frame_row_indices;
    result.transition_count = moving.addition_transition_count;
    result.inverse_transition_count = moving.inverse_transition_count;
    result.combined_state_bytes = moving.cumulative_state_bytes;
    result.combined_final_output_bytes =
        moving.combined_final_output_bytes;
    result.peak_finalization_workspace_bytes =
        moving.peak_finalization_workspace_bytes;
    result.moving_inverse_state_used = true;
  } else {
    std::size_t retained_output_payload_bytes = 0;
    for (const auto& frame : request.frames.effective_frames) {
      auto aggregate = make_borrowed_frame_request();
      DescriptorBatch frame_input_batch;
      frame_input_batch.columns = request.frames.ordered_batch.columns;
      const auto remaining_finalization_bytes =
          maximum_combined_final_output_bytes -
          result.combined_final_output_bytes;
      if (retained_output_payload_bytes >
          *node_memory_grant - base_retained_memory_bytes) {
        return refuse("QOW-DIAG-WINDOW-AGGREGATE-REGISTRY-RESOURCE",
                      "aggregate window retained output exhausted its grant");
      }
      aggregate.retained_memory_bytes =
          base_retained_memory_bytes + retained_output_payload_bytes;
      std::size_t frame_input_payload_bytes = 1;
      for (const auto row : frame.effective_row_indices) {
        for (const auto& value : request.frames.ordered_batch.rows[row].values) {
          if (frame_input_payload_bytes >
                  std::numeric_limits<std::size_t>::max() -
                      value.encoded_value.size()) {
            return refuse("QOW-DIAG-WINDOW-AGGREGATE-REGISTRY-RESOURCE",
                          "aggregate window frame input size overflowed");
          }
          frame_input_payload_bytes += value.encoded_value.size();
          if (frame_input_payload_bytes >
                  std::numeric_limits<std::size_t>::max() -
                      value.binary_value.size()) {
            return refuse("QOW-DIAG-WINDOW-AGGREGATE-REGISTRY-RESOURCE",
                          "aggregate window frame input size overflowed");
          }
          frame_input_payload_bytes += value.binary_value.size();
        }
      }
      std::size_t frame_filter_bytes = 0;
      if (template_filter_truth_values != nullptr &&
          !FoundationFilterBytes(frame.effective_row_indices.size(),
                                 &frame_filter_bytes)) {
        return refuse("QOW-DIAG-WINDOW-AGGREGATE-REGISTRY-RESOURCE",
                      "aggregate window frame FILTER size overflowed");
      }
      if (frame_input_payload_bytes >
              *node_memory_grant - aggregate.retained_memory_bytes ||
          frame_filter_bytes >
              *node_memory_grant - aggregate.retained_memory_bytes -
                  frame_input_payload_bytes) {
        return refuse("QOW-DIAG-WINDOW-AGGREGATE-REGISTRY-RESOURCE",
                      "aggregate window frame input or FILTER exceeds its selected-node grant");
      }
      frame_input_batch.rows.reserve(frame.effective_row_indices.size());
      for (const auto row : frame.effective_row_indices) {
        frame_input_batch.rows.push_back(
            request.frames.ordered_batch.rows[row]);
      }
      if (template_filter_truth_values != nullptr) {
        std::vector<api::EngineSqlTruthValue> filter;
        filter.reserve(frame.effective_row_indices.size());
        for (const auto row : frame.effective_row_indices) {
          filter.push_back((*template_filter_truth_values)[row]);
        }
        aggregate.filter_truth_values = std::move(filter);
      }
      auto frame_result =
          detail::ExecuteCanonicalAggregateRuntimeBorrowedForContext(
              aggregate, aggregate_template.physical_dag,
              frame_input_batch, aggregate_runtime_context,
              remaining_finalization_bytes);
      if (!frame_result.diagnostic.ok) {
        return refuse(frame_result.diagnostic.diagnostic_code,
                      frame_result.diagnostic.detail);
      }
      if (!frame_result.shared_state_authority_used ||
          !aggregate_receipts_match(frame_result) ||
          frame_result.output_batch.rows.size() != 1 ||
          frame_result.output_batch.rows.front().values.size() != 1 ||
          frame_result.selected_plan_uuid != request.frames.selected_plan_uuid ||
          frame_result.executed_physical_node_id !=
              request.frames.executed_physical_node_id ||
          frame_result.causal_counter_id != request.frames.causal_counter_id ||
          !PhysicalMgaStatementContextEqual(
              frame_result.mga_statement_context,
              aggregate_template.mga_authority.statement_context)) {
        return refuse("QOW-DIAG-WINDOW-AGGREGATE-REGISTRY-EVIDENCE",
                      "aggregate frame result evidence is inconsistent");
      }
      if (!within_total(frame_result.transition_count, result.transition_count,
                        request.maximum_transition_count) ||
          !within_total(frame_result.distinct_tuple_count,
                        result.distinct_tuple_count,
                        request.maximum_distinct_tuple_count) ||
          !within_total(frame_result.order_comparison_count,
                        result.order_comparison_count,
                        request.maximum_order_comparison_count) ||
          !within_total(frame_result.equality_key_generation_count,
                        result.equality_key_generation_count,
                        request.maximum_equality_key_generation_count) ||
          !within_total(frame_result.equality_comparison_count,
                        result.equality_comparison_count,
                        request.maximum_equality_comparison_count) ||
          !within_total(frame_result.state_bytes, result.combined_state_bytes,
                        request.maximum_combined_state_bytes) ||
          !within_total(frame_result.final_output_bytes,
                        result.combined_final_output_bytes,
                        maximum_combined_final_output_bytes)) {
        return refuse("QOW-DIAG-WINDOW-AGGREGATE-REGISTRY-RESOURCE",
                      "combined aggregate window state bound is exceeded");
      }
      result.transition_count += frame_result.transition_count;
      result.distinct_tuple_count += frame_result.distinct_tuple_count;
      result.order_comparison_count += frame_result.order_comparison_count;
      result.equality_key_generation_count +=
          frame_result.equality_key_generation_count;
      result.equality_comparison_count +=
          frame_result.equality_comparison_count;
      result.frame_non_null_transition_counts.push_back(
          frame_result.non_null_transition_count);
      result.frame_equality_key_generation_counts.push_back(
          frame_result.equality_key_generation_count);
      result.frame_equality_comparison_counts.push_back(
          frame_result.equality_comparison_count);
      result.combined_state_bytes += frame_result.state_bytes;
      result.combined_final_output_bytes +=
          frame_result.final_output_bytes;
      result.peak_finalization_workspace_bytes = std::max(
          result.peak_finalization_workspace_bytes,
          frame_result.peak_finalization_workspace_bytes);
      std::vector<std::size_t> transition_rows;
      transition_rows.reserve(frame_result.transition_row_indices.size());
      for (const auto frame_row : frame_result.transition_row_indices) {
        if (frame_row >= frame.effective_row_indices.size()) {
          return refuse("QOW-DIAG-WINDOW-AGGREGATE-REGISTRY-EVIDENCE",
                        "aggregate transition receipt escaped its effective frame");
        }
        transition_rows.push_back(frame.effective_row_indices[frame_row]);
      }
      result.transition_row_indices.push_back(std::move(transition_rows));
      auto frame_value = std::move(
          frame_result.output_batch.rows.front().values.front());
      if (retained_output_payload_bytes >
              std::numeric_limits<std::size_t>::max() -
                  frame_value.encoded_value.size() ||
          retained_output_payload_bytes + frame_value.encoded_value.size() >
              std::numeric_limits<std::size_t>::max() -
                  frame_value.binary_value.size()) {
        return refuse("QOW-DIAG-WINDOW-AGGREGATE-REGISTRY-RESOURCE",
                      "aggregate window retained output size overflowed");
      }
      retained_output_payload_bytes += frame_value.encoded_value.size();
      retained_output_payload_bytes += frame_value.binary_value.size();
      result.values.push_back(std::move(frame_value));
    }
  }

  DescriptorBatch output;
  output.columns = {aggregate_template.result_column};
  output.rows.reserve(result.values.size());
  for (auto& value : result.values) {
    output.rows.push_back({{std::move(value)}});
  }
  const auto output_validation = ValidateCanonicalDescriptorBatch(
      output, {aggregate_template.result_column.descriptor_id});
  if (!output_validation.ok) {
    return refuse("QOW-DIAG-WINDOW-AGGREGATE-REGISTRY-EVIDENCE",
                  output_validation.diagnostic_code + ":" +
                  output_validation.detail);
  }
  result.values.clear();
  result.values.reserve(output.rows.size());
  for (auto& row : output.rows) {
    result.values.push_back(std::move(row.values.front()));
  }
  const auto result_authority = RevalidateCanonicalExecutionMgaAuthority(
      aggregate_template.mga_authority, aggregate_template.physical_dag);
  if (!result_authority.ok) {
    return refuse(result_authority.diagnostic_code,
                  result_authority.detail);
  }

  result.diagnostic = {};
  result.descriptor = aggregate_template.descriptor;
  result.result_column = aggregate_template.result_column;
  result.value_columns = aggregate_template.value_columns;
  result.value_expression_descriptor_ids =
      aggregate_template.value_expression_descriptor_ids;
  result.direct_argument_count = aggregate_template.direct_arguments.size();
  result.modifier_count = expected_modifier_count;
  result.aggregate_order_term_count =
      aggregate_template.aggregate_order_terms.size();
  result.every_aggregate_descriptor_field_consumed = true;
  result.every_effective_frame_consumed =
      result.frame_row_indices.size() == request.frames.effective_frames.size();
  result.modifier_pipeline_validated = true;
  result.filter_modifier_applied =
      aggregate_template.filter_truth_values.has_value();
  result.distinct_modifier_applied = aggregate_template.distinct;
  result.filter_applied_before_distinct = true;
  result.distinct_applied_before_order = true;
  result.aggregate_order_applied =
      !aggregate_template.aggregate_order_terms.empty();
  result.transient_state_cleanup_proven = true;
  result.all_or_nothing_publication = true;
  result.effective_frame_recomputed =
      selected_state_strategy !=
      CanonicalRegistryWindowAggregateStateStrategy::moving_inverse;
  result.state_strategy_selected_from_physical_plan = true;
  result.aggregate_state_spill_required = aggregate_state_spill_required;
  result.selected_state_strategy = selected_state_strategy;
  result.selected_state_implementation_id =
      aggregate_node->implementation_id;
  result.shared_aggregate_state_authority_used = true;
  result.authority = request.frames.authority;
  result.window_property_uuid = request.frames.window_property_uuid;
  result.partition_property_uuid = request.frames.partition_property_uuid;
  result.ordering_property_uuid = request.frames.ordering_property_uuid;
  result.term_binding_evidence_uuid =
      request.frames.term_binding_evidence_uuid;
  result.deterministic_tie_evidence_uuid =
      request.frames.deterministic_tie_evidence_uuid;
  result.frame_property_binding_evidence_uuid =
      request.frames.frame_property_binding_evidence_uuid;
  result.selected_plan_uuid = request.frames.selected_plan_uuid;
  result.executed_physical_node_id =
      request.frames.executed_physical_node_id;
  result.causal_counter_id = request.frames.causal_counter_id;
  result.mga_statement_context =
      aggregate_template.mga_authority.statement_context;
  return result;
}

static CanonicalRegistryWindowAggregateResult
ExecuteCanonicalRegistryWindowAggregateStrategy(
    const CanonicalRegistryWindowAggregateRequest& request) {
  return ExecuteCanonicalRegistryWindowAggregateSelected(request, false);
}

// QOW-SOURCE-WIN-012-REGISTRY-SPILL-V1
// Recompute each effective frame through the canonical aggregate authority,
// serialize its complete transition state, and restore/finalize that state
// through engine-owned temporary work. The already optimizer-published window
// implementation owns selection of this route; temporary metadata remains
// advisory and never owns visibility, finality, or recovery.
static CanonicalRegistryWindowAggregateSpillResult
ExecuteCanonicalRegistryWindowAggregateSpillStrategy(
    const CanonicalRegistryWindowAggregateSpillRequest& request) {
  CanonicalRegistryWindowAggregateSpillResult result;
  const auto refuse = [&](std::string code, std::string detail) {
    result.diagnostic.ok = false;
    result.diagnostic.diagnostic_code = std::move(code);
    result.diagnostic.detail = std::move(detail);
    result.aggregate_result = {};
    return result;
  };

  const auto& aggregate_request = request.aggregate_request;
  const auto* template_filter_truth_values =
      aggregate_request.aggregate_template.filter_truth_values.has_value()
          ? &*aggregate_request.aggregate_template.filter_truth_values
          : nullptr;
  auto aggregate_template =
      CloneFoundationAggregateRequestWithoutRowsOrFilter(
          aggregate_request.aggregate_template);
  const auto frame_authority = RevalidateCanonicalWindowFrameAuthority(
      aggregate_template.mga_authority, aggregate_request.frames);
  if (!frame_authority.ok) {
    return refuse(frame_authority.diagnostic_code, frame_authority.detail);
  }
  aggregate_template.mga_authority = CanonicalWindowFrameExecutionAuthority(
      aggregate_template.mga_authority, aggregate_request.frames);
  const auto entry_authority = RevalidateCanonicalExecutionMgaAuthority(
      aggregate_template.mga_authority, aggregate_template.physical_dag);
  if (!entry_authority.ok) {
    return refuse(entry_authority.diagnostic_code, entry_authority.detail);
  }

  if (!aggregate_template.aggregate_equality_authority_profile.empty()) {
    const auto function = aggregate_template.descriptor.function;
    const bool frequency_restore_required =
        function == CanonicalAggregateFunction::mode ||
        function == CanonicalAggregateFunction::approx_count_distinct ||
        function == CanonicalAggregateFunction::approx_top_k;
    std::size_t minimum_cumulative_generation_count = 0;
    std::size_t minimum_cumulative_comparison_count = 0;
    if (request.maximum_cumulative_equality_key_generation_count == 0 ||
        request.maximum_cumulative_equality_comparison_count == 0 ||
        !FoundationCheckedMultiply(
            aggregate_request.maximum_equality_key_generation_count, 4,
            &minimum_cumulative_generation_count) ||
        !FoundationCheckedMultiply(
            aggregate_request.maximum_equality_comparison_count, 4,
            &minimum_cumulative_comparison_count) ||
        (frequency_restore_required &&
         !FoundationCheckedAdd(
             &minimum_cumulative_comparison_count,
             aggregate_request.maximum_equality_key_generation_count)) ||
        minimum_cumulative_generation_count >
            request.maximum_cumulative_equality_key_generation_count ||
        minimum_cumulative_comparison_count >
            request.maximum_cumulative_equality_comparison_count) {
      return refuse(
          "QOW-DIAG-WINDOW-AGGREGATE-REGISTRY-SPILL-RESOURCE",
          "window aggregate cumulative equality-work ceiling is insufficient");
    }
  }

  auto canonical = ExecuteCanonicalRegistryWindowAggregateSelected(
      aggregate_request, true);
  if (!canonical.diagnostic.ok) {
    return refuse(canonical.diagnostic.diagnostic_code,
                  canonical.diagnostic.detail);
  }
  if (!PhysicalMgaStatementContextEqual(
          canonical.mga_statement_context,
          aggregate_template.mga_authority.statement_context)) {
    return refuse("QOW-DIAG-WINDOW-AGGREGATE-REGISTRY-SPILL-MGA",
                  "window spill baseline returned a different MGA statement context");
  }
  if (!canonical.aggregate_state_spill_required ||
      canonical.selected_state_implementation_id !=
          "window.aggregate-registry-state-spill.v1") {
    return refuse("QOW-DIAG-WINDOW-AGGREGATE-REGISTRY-STRATEGY",
                  "window aggregate spill did not consume its selected physical implementation");
  }
  if (request.spill_root.empty() || !request.spill_root.is_absolute() ||
      !IsCanonicalUuid(request.spill_owner_uuid) ||
      request.runtime_generation == 0 || request.memory_quota_bytes == 0 ||
      request.maximum_serialized_state_bytes == 0 ||
      request.maximum_spill_record_count == 0 ||
      !aggregate_template.physical_dag.spill_allowed) {
    return refuse("QOW-DIAG-WINDOW-AGGREGATE-REGISTRY-SPILL",
                  "window aggregate spill ownership or resource contract is invalid");
  }
  const auto owner_directory =
      (request.spill_root / request.spill_owner_uuid).lexically_normal();
  if (owner_directory.filename() != request.spill_owner_uuid) {
    return refuse("QOW-DIAG-WINDOW-AGGREGATE-REGISTRY-SPILL",
                  "window aggregate spill owner directory is not exact");
  }
  std::error_code filesystem_error;
  if (std::filesystem::is_symlink(owner_directory, filesystem_error) ||
      filesystem_error) {
    return refuse("QOW-DIAG-WINDOW-AGGREGATE-REGISTRY-SPILL",
                  "window aggregate spill owner is a symlink or unreadable");
  }
  bool ownership_inspection_ok = false;
  if (HasOwnedAggregateSpillArtifact(owner_directory,
                                     &ownership_inspection_ok) ||
      !ownership_inspection_ok) {
    return refuse("QOW-DIAG-WINDOW-AGGREGATE-REGISTRY-SPILL",
                  "window aggregate spill owner already has an artifact");
  }

  if (canonical.frame_row_indices.empty()) {
    return refuse("QOW-DIAG-WINDOW-AGGREGATE-REGISTRY-SPILL-EMPTY",
                  "window aggregate has no transition states to spill");
  }

  namespace api = scratchbird::engine::internal_api;
  const auto expected_combined_final_output_bytes =
      canonical.combined_final_output_bytes;
  const auto baseline_peak_finalization_workspace_bytes =
      canonical.peak_finalization_workspace_bytes;
  result.equality_key_generation_count =
      canonical.equality_key_generation_count;
  result.equality_comparison_count = canonical.equality_comparison_count;
  if (result.equality_key_generation_count >
          request.maximum_cumulative_equality_key_generation_count ||
      result.equality_comparison_count >
          request.maximum_cumulative_equality_comparison_count) {
    return refuse("QOW-DIAG-WINDOW-AGGREGATE-REGISTRY-SPILL-RESOURCE",
                  "window aggregate baseline equality work exceeded its cumulative ceiling");
  }
  const auto expected_value_count = canonical.values.size();
  if (canonical.frame_non_null_transition_counts.size() !=
          expected_value_count ||
      canonical.frame_equality_key_generation_counts.size() !=
          expected_value_count ||
      canonical.frame_equality_comparison_counts.size() !=
          expected_value_count) {
    return refuse("QOW-DIAG-WINDOW-AGGREGATE-REGISTRY-SPILL-EVIDENCE",
                  "window aggregate frame equality receipts are incomplete");
  }
  const auto expected_transition_count = canonical.transition_count;
  const auto expected_inverse_transition_count =
      canonical.inverse_transition_count;
  const auto expected_distinct_tuple_count =
      canonical.distinct_tuple_count;
  const auto expected_order_comparison_count =
      canonical.order_comparison_count;
  const auto expected_combined_state_bytes =
      canonical.combined_state_bytes;
  auto reopened = std::move(canonical);
  reopened.transition_count = 0;
  reopened.inverse_transition_count = 0;
  reopened.distinct_tuple_count = 0;
  reopened.order_comparison_count = 0;
  reopened.combined_state_bytes = 0;
  std::size_t replayed_combined_final_output_bytes = 0;
  std::size_t replay_peak_finalization_workspace_bytes = 0;
  std::size_t ordered_input_payload_bytes = 0;
  std::size_t source_filter_bytes = 0;
  if (!FoundationBatchPayloadBytes(
          aggregate_request.frames.ordered_batch,
          &ordered_input_payload_bytes) ||
      (template_filter_truth_values != nullptr &&
       !FoundationFilterBytes(template_filter_truth_values->size(),
                              &source_filter_bytes))) {
    return refuse("QOW-DIAG-WINDOW-AGGREGATE-REGISTRY-SPILL-RESOURCE",
                  "window aggregate frame input size overflowed");
  }
  const auto node_memory_grant = FoundationNodeMemoryGrant(
      aggregate_template.physical_dag,
      aggregate_template.selected_physical_node_id);
  if (!node_memory_grant.has_value() ||
      aggregate_template.retained_memory_bytes > *node_memory_grant ||
      ordered_input_payload_bytes >
          *node_memory_grant - aggregate_template.retained_memory_bytes ||
      expected_combined_final_output_bytes >
          *node_memory_grant - aggregate_template.retained_memory_bytes -
              ordered_input_payload_bytes ||
      source_filter_bytes >
          *node_memory_grant - aggregate_template.retained_memory_bytes -
              ordered_input_payload_bytes -
              expected_combined_final_output_bytes) {
    return refuse("QOW-DIAG-WINDOW-AGGREGATE-REGISTRY-SPILL-RESOURCE",
                  "window aggregate replay retention exceeds its selected-node grant");
  }
  const auto replay_retained_memory_bytes =
      aggregate_template.retained_memory_bytes +
      ordered_input_payload_bytes + expected_combined_final_output_bytes +
      source_filter_bytes;
  const auto within_total = [](const std::size_t next,
                               const std::size_t current,
                               const std::size_t maximum) {
    return current <= maximum && next <= maximum - current;
  };
  const auto descriptor_equal = [](const auto& left, const auto& right) {
    return left.descriptor_uuid.canonical ==
               right.descriptor_uuid.canonical &&
           left.descriptor_kind == right.descriptor_kind &&
           left.canonical_type_name == right.canonical_type_name &&
           left.encoded_descriptor == right.encoded_descriptor;
  };
  const auto value_equal = [&](const auto& left, const auto& right) {
    return descriptor_equal(left.descriptor, right.descriptor) &&
           left.encoded_value == right.encoded_value &&
           left.binary_value == right.binary_value &&
           left.is_null == right.is_null && left.state == right.state;
  };
  for (std::size_t frame_index = 0;
       frame_index < reopened.frame_row_indices.size(); ++frame_index) {
    const auto remaining_bytes = request.maximum_serialized_state_bytes -
                                 result.serialized_aggregate_state_bytes;
    const auto remaining_records = request.maximum_spill_record_count -
                                   result.spilled_aggregate_state_record_count;
    if (remaining_bytes == 0 || remaining_records == 0) {
      return refuse("QOW-DIAG-WINDOW-AGGREGATE-REGISTRY-SPILL-RESOURCE",
                    "window aggregate state spill bound is exhausted");
    }

    const auto& frame = reopened.frame_row_indices[frame_index];
    std::size_t selected_frame_payload_bytes = 1;
    for (const auto row : frame) {
      for (const auto& value :
           aggregate_request.frames.ordered_batch.rows[row].values) {
        if (selected_frame_payload_bytes >
                std::numeric_limits<std::size_t>::max() -
                    value.encoded_value.size()) {
          return refuse("QOW-DIAG-WINDOW-AGGREGATE-REGISTRY-SPILL-RESOURCE",
                        "window aggregate selected frame size overflowed");
        }
        selected_frame_payload_bytes += value.encoded_value.size();
        if (selected_frame_payload_bytes >
                std::numeric_limits<std::size_t>::max() -
                    value.binary_value.size()) {
          return refuse("QOW-DIAG-WINDOW-AGGREGATE-REGISTRY-SPILL-RESOURCE",
                        "window aggregate selected frame size overflowed");
        }
        selected_frame_payload_bytes += value.binary_value.size();
      }
    }
    if (selected_frame_payload_bytes >
        *node_memory_grant - replay_retained_memory_bytes) {
      return refuse(
          "QOW-DIAG-WINDOW-AGGREGATE-REGISTRY-SPILL-RESOURCE",
          "window aggregate selected frame copy exceeds its selected-node grant");
    }
    std::size_t selected_frame_filter_bytes = 0;
    if (template_filter_truth_values != nullptr &&
        !FoundationFilterBytes(frame.size(),
                               &selected_frame_filter_bytes)) {
      return refuse(
          "QOW-DIAG-WINDOW-AGGREGATE-REGISTRY-SPILL-RESOURCE",
          "window aggregate selected frame FILTER size overflowed");
    }
    if (selected_frame_filter_bytes >
        *node_memory_grant - replay_retained_memory_bytes -
            selected_frame_payload_bytes) {
      return refuse(
          "QOW-DIAG-WINDOW-AGGREGATE-REGISTRY-SPILL-RESOURCE",
          "window aggregate selected frame FILTER exceeds its selected-node grant");
    }

    auto aggregate =
        CloneFoundationAggregateRequestWithoutRowsOrFilter(
            aggregate_template, false);
    bool frequency_restore_required = false;
    std::size_t restore_equality_comparison_ceiling = 0;
    if (!aggregate.aggregate_equality_authority_profile.empty()) {
      const auto function = aggregate.descriptor.function;
      frequency_restore_required =
          function == CanonicalAggregateFunction::mode ||
          function == CanonicalAggregateFunction::approx_count_distinct ||
          function == CanonicalAggregateFunction::approx_top_k;
      if (frequency_restore_required) {
        const auto non_null_count =
            reopened.frame_non_null_transition_counts[frame_index];
        std::size_t pair_count = 0;
        if ((non_null_count > 1 &&
             !FoundationCheckedMultiply(non_null_count, non_null_count - 1,
                                        &pair_count)) ||
            (pair_count /= 2,
             !FoundationCheckedAdd(&pair_count, non_null_count))) {
          return refuse(
              "QOW-DIAG-WINDOW-AGGREGATE-REGISTRY-SPILL-RESOURCE",
              "window aggregate restore equality-work ceiling overflowed");
        }
        restore_equality_comparison_ceiling = pair_count;
        restore_equality_comparison_ceiling = std::max(
            restore_equality_comparison_ceiling,
            reopened.frame_equality_comparison_counts[frame_index]);
      }
    }
    DescriptorBatch frame_input_batch;
    frame_input_batch.columns =
        aggregate_request.frames.ordered_batch.columns;
    frame_input_batch.rows.reserve(frame.size());
    for (const auto row : frame) {
      frame_input_batch.rows.push_back(
          aggregate_request.frames.ordered_batch.rows[row]);
    }
    if (template_filter_truth_values != nullptr) {
      std::vector<api::EngineSqlTruthValue> filter;
      filter.reserve(frame.size());
      for (const auto row : frame) {
        filter.push_back((*template_filter_truth_values)[row]);
      }
      aggregate.filter_truth_values = std::move(filter);
    }

    CanonicalAggregateStateSpillRequest state_request;
    state_request.aggregate_request = std::move(aggregate);
    state_request.spill_root = request.spill_root;
    state_request.spill_owner_uuid = request.spill_owner_uuid;
    state_request.runtime_generation = request.runtime_generation;
    state_request.reopen_runtime_generation =
        request.reopen_runtime_generation;
    state_request.memory_quota_bytes = request.memory_quota_bytes;
    state_request.maximum_serialized_state_bytes = remaining_bytes;
    state_request.maximum_spill_record_count = remaining_records;
    state_request.maximum_cumulative_equality_key_generation_count =
        request.maximum_cumulative_equality_key_generation_count -
        result.equality_key_generation_count;
    state_request.maximum_cumulative_equality_comparison_count =
        request.maximum_cumulative_equality_comparison_count -
        result.equality_comparison_count;
    if (!state_request.aggregate_request
             .aggregate_equality_authority_profile.empty()) {
      state_request.maximum_ordinary_equality_key_generation_count =
          reopened.frame_equality_key_generation_counts[frame_index];
      state_request.maximum_ordinary_equality_comparison_count =
          reopened.frame_equality_comparison_counts[frame_index];
      state_request.maximum_restore_equality_key_generation_count =
          frequency_restore_required
              ? reopened.frame_equality_key_generation_counts[frame_index]
              : 0;
      state_request.maximum_restore_equality_comparison_count =
          restore_equality_comparison_ceiling;
    }
    state_request.retained_memory_bytes =
        ordered_input_payload_bytes + expected_combined_final_output_bytes +
        source_filter_bytes;
    state_request.cancellation_requested = request.cancellation_requested;
    state_request.cleanup_after_cancellation =
        request.cleanup_after_cancellation;
    state_request.restart_recovery_proof_available =
        request.restart_recovery_proof_available;
    auto state = ExecuteCanonicalAggregateStateSpill(
        state_request, aggregate_template.physical_dag,
        frame_input_batch);

    result.spilled = result.spilled || state.spilled;
    result.spill_reopened = result.spill_reopened || state.spill_reopened;
    result.cleanup_proven =
        frame_index == 0
            ? state.cleanup_proven
            : result.cleanup_proven && state.cleanup_proven;
    result.cancellation_observed =
        result.cancellation_observed || state.cancellation_observed;
    result.spill_evidence.insert(result.spill_evidence.end(),
                                 state.spill_evidence.begin(),
                                 state.spill_evidence.end());
    if (!state.diagnostic.ok) {
      return refuse(state.diagnostic.diagnostic_code,
                    state.diagnostic.detail);
    }
    if (!state.state_serialized || !state.spilled ||
        !state.spill_reopened || !state.state_restored ||
        !state.restored_result_equivalent || !state.cleanup_proven ||
        state.aggregate_result.output_batch.rows.size() != 1 ||
            state.aggregate_result.output_batch.rows.front().values.size() != 1 ||
        state.aggregate_result.selected_plan_uuid !=
            reopened.selected_plan_uuid ||
        state.aggregate_result.executed_physical_node_id !=
            reopened.executed_physical_node_id ||
        state.aggregate_result.causal_counter_id != reopened.causal_counter_id ||
        !PhysicalMgaStatementContextEqual(
            state.mga_statement_context,
            aggregate_template.mga_authority.statement_context) ||
        !PhysicalMgaStatementContextEqual(
            state.aggregate_result.mga_statement_context,
            aggregate_template.mga_authority.statement_context)) {
      return refuse("QOW-DIAG-WINDOW-AGGREGATE-REGISTRY-SPILL-EVIDENCE",
                    "restored aggregate frame state evidence is inconsistent");
    }
    if (!within_total(state.serialized_state_bytes,
                      result.serialized_aggregate_state_bytes,
                      request.maximum_serialized_state_bytes) ||
        !within_total(state.spilled_state_record_count,
                      result.spilled_aggregate_state_record_count,
                      request.maximum_spill_record_count) ||
        !within_total(state.aggregate_result.transition_count,
                      reopened.transition_count,
                      aggregate_request.maximum_transition_count) ||
        !within_total(state.aggregate_result.distinct_tuple_count,
                      reopened.distinct_tuple_count,
                      aggregate_request.maximum_distinct_tuple_count) ||
        !within_total(
            state.aggregate_result.order_comparison_count,
            reopened.order_comparison_count,
            aggregate_request.maximum_order_comparison_count) ||
        !within_total(
            state.aggregate_result.equality_key_generation_count,
            result.equality_key_generation_count,
            request.maximum_cumulative_equality_key_generation_count) ||
        !within_total(
            state.aggregate_result.equality_comparison_count,
            result.equality_comparison_count,
            request.maximum_cumulative_equality_comparison_count) ||
        !within_total(state.aggregate_result.state_bytes,
                      reopened.combined_state_bytes,
                      aggregate_request.maximum_combined_state_bytes) ||
        !within_total(state.aggregate_result.final_output_bytes,
                      replayed_combined_final_output_bytes,
                      expected_combined_final_output_bytes)) {
      return refuse("QOW-DIAG-WINDOW-AGGREGATE-REGISTRY-SPILL-RESOURCE",
                    "restored aggregate frame state exceeds its combined bound");
    }
    ++result.spilled_aggregate_state_count;
    result.serialized_aggregate_state_bytes += state.serialized_state_bytes;
    result.spilled_aggregate_state_record_count +=
        state.spilled_state_record_count;
    reopened.transition_count += state.aggregate_result.transition_count;
    reopened.distinct_tuple_count +=
        state.aggregate_result.distinct_tuple_count;
    reopened.order_comparison_count +=
        state.aggregate_result.order_comparison_count;
    result.equality_key_generation_count +=
        state.aggregate_result.equality_key_generation_count;
    result.equality_comparison_count +=
        state.aggregate_result.equality_comparison_count;
    reopened.combined_state_bytes += state.aggregate_result.state_bytes;
    const auto& expected_value = reopened.values[frame_index];
    const auto& actual_value =
        state.aggregate_result.output_batch.rows.front().values.front();
    if (!value_equal(expected_value, actual_value)) {
      return refuse("QOW-DIAG-WINDOW-AGGREGATE-REGISTRY-SPILL-EQUIVALENCE",
                    "restored aggregate window value diverged");
    }
    replayed_combined_final_output_bytes +=
        state.aggregate_result.final_output_bytes;
    replay_peak_finalization_workspace_bytes = std::max(
        replay_peak_finalization_workspace_bytes,
        state.aggregate_result.peak_finalization_workspace_bytes);
    reopened.values[frame_index] = std::move(
        state.aggregate_result.output_batch.rows.front().values.front());
  }

  bool cleanup_inspection_ok = false;
  const bool artifact_remains = HasOwnedAggregateSpillArtifact(
      owner_directory, &cleanup_inspection_ok);
  if (!cleanup_inspection_ok || artifact_remains) {
    result.cleanup_proven =
        RemoveOwnedAggregateSpillArtifacts(owner_directory) &&
        result.cleanup_proven;
    return refuse("QOW-DIAG-WINDOW-AGGREGATE-REGISTRY-SPILL-CLEANUP",
                  "window aggregate state spill cleanup is incomplete");
  }

  if (!reopened.diagnostic.ok ||
      reopened.values.size() != expected_value_count ||
      reopened.transition_count != expected_transition_count ||
      reopened.inverse_transition_count !=
          expected_inverse_transition_count ||
      reopened.distinct_tuple_count != expected_distinct_tuple_count ||
      reopened.order_comparison_count !=
          expected_order_comparison_count ||
      reopened.combined_state_bytes != expected_combined_state_bytes ||
      replayed_combined_final_output_bytes !=
          expected_combined_final_output_bytes ||
      reopened.combined_final_output_bytes !=
          expected_combined_final_output_bytes ||
      replay_peak_finalization_workspace_bytes !=
          baseline_peak_finalization_workspace_bytes) {
    return refuse("QOW-DIAG-WINDOW-AGGREGATE-REGISTRY-SPILL-EQUIVALENCE",
                  "restored and in-memory aggregate window results diverge");
  }
  reopened.peak_finalization_workspace_bytes =
      replay_peak_finalization_workspace_bytes;

  const auto result_authority = RevalidateCanonicalExecutionMgaAuthority(
      aggregate_template.mga_authority, aggregate_template.physical_dag);
  if (!result_authority.ok) {
    return refuse(result_authority.diagnostic_code, result_authority.detail);
  }

  result.diagnostic = {};
  result.aggregate_result = std::move(reopened);
  result.mga_statement_context =
      aggregate_template.mga_authority.statement_context;
  return result;
}

std::vector<CanonicalWindowRuntimeDescriptor>
CanonicalWindowRuntimeRegistryV1() {
  return {
      {1, CanonicalWindowRuntimeFunction::row_number,
       "sb.window.row_number", "019de5fc-2400-7539-bcce-00eef3ae7220"},
      {1, CanonicalWindowRuntimeFunction::rank,
       "sb.window.rank", "019de5fc-2400-7b94-870d-0dd789ca70ab"},
      {1, CanonicalWindowRuntimeFunction::dense_rank,
       "sb.window.dense_rank", "019de5fc-2400-741d-bef0-f079fd3ba494"},
      {1, CanonicalWindowRuntimeFunction::percent_rank,
       "sb.window.percent_rank", "019de5fc-2400-7d86-86fe-96f3f27b5dd6"},
      {1, CanonicalWindowRuntimeFunction::cume_dist,
       "sb.window.cume_dist", "019de5fc-2400-721c-be64-2568b64a02b9"},
      {1, CanonicalWindowRuntimeFunction::ntile,
       "sb.window.ntile", "019de5fc-2400-7047-9474-232ca488c094"},
      {1, CanonicalWindowRuntimeFunction::lag,
       "sb.window.lag", "019de5fc-2400-782c-8436-9ac310301738"},
      {1, CanonicalWindowRuntimeFunction::lead,
       "sb.window.lead", "019de5fc-2400-7a06-bc3c-6747cf5be66f"},
      {1, CanonicalWindowRuntimeFunction::first_value,
       "sb.window.first_value", "019de5fc-2400-7264-90fb-d25bd0f806f2"},
      {1, CanonicalWindowRuntimeFunction::last_value,
       "sb.window.last_value", "019de5fc-2400-7d23-a5be-7ed3f1a5c3ec"},
      {1, CanonicalWindowRuntimeFunction::nth_value,
       "sb.window.nth_value", "019de5fc-2400-7dc9-80e6-9f2ccf08076f"},
  };
}

// QOW-SOURCE-WIN-002-V1
// QOW-SOURCE-WIN-012-REGISTRY-UNIFIED-V1
// Every accepted runtime descriptor selects exactly one retained canonical
// strategy. Native descriptor identity is exact-matched against the strict
// window registry; aggregate-as-window identity is exact-matched against the
// QRY-011 registry without duplicating those identities here. Missing or
// unknown state cannot select ROW_NUMBER, SUM, zero, or another substitute.
// The class-specific helpers remain implementation strategies only and are
// production-reachable through this entry point.
CanonicalWindowRuntimeResult ExecuteCanonicalWindowRuntime(
    const CanonicalWindowRuntimeRequest& request) {
  CanonicalWindowRuntimeResult result;
  const auto refuse = [&](std::string code, std::string detail) {
    result = {};
    result.diagnostic.ok = false;
    result.diagnostic.diagnostic_code = std::move(code);
    result.diagnostic.detail = std::move(detail);
    return result;
  };
  const bool aggregate_registry_identity =
      request.descriptor.aggregate_function.has_value();
  if (request.descriptor.abi_version != 1 ||
      request.descriptor.builtin_id.empty() ||
      !IsCanonicalUuid(request.descriptor.function_uuid)) {
    return refuse("QOW-DIAG-WINDOW-FUNCTION-DESCRIPTOR",
                  "window runtime descriptor is missing or malformed");
  }
  if (aggregate_registry_identity) {
    const auto* aggregate_row = LookupCanonicalAggregateExactV1(
        request.descriptor.abi_version,
        *request.descriptor.aggregate_function,
        request.descriptor.builtin_id,
        request.descriptor.function_uuid);
    if (request.descriptor.function !=
            CanonicalWindowRuntimeFunction::unknown ||
        aggregate_row == nullptr ||
        !aggregate_row->executable || !aggregate_row->aggregate_as_window) {
      return refuse(
          "QOW-DIAG-WINDOW-FUNCTION-DESCRIPTOR",
          "aggregate-as-window descriptor is unknown or registry-drifted");
    }
  } else {
    const auto registry = CanonicalWindowRuntimeRegistryV1();
    const auto registry_row = std::ranges::find_if(
        registry, [&](const CanonicalWindowRuntimeDescriptor& row) {
          return row.function == request.descriptor.function;
        });
    if (request.descriptor.function ==
            CanonicalWindowRuntimeFunction::unknown ||
        registry_row == registry.end() ||
        registry_row->abi_version != request.descriptor.abi_version ||
        registry_row->builtin_id != request.descriptor.builtin_id ||
        registry_row->function_uuid != request.descriptor.function_uuid ||
        registry_row->aggregate_function.has_value()) {
      return refuse("QOW-DIAG-WINDOW-FUNCTION-DESCRIPTOR",
                    "native window descriptor is unknown or registry-drifted");
    }
  }

  CanonicalWindowRuntimeStrategy expected_strategy =
      CanonicalWindowRuntimeStrategy::unknown;
  std::optional<CanonicalWindowRankingFunction> expected_ranking;
  std::optional<CanonicalWindowValueFunction> expected_value;
  std::optional<CanonicalAggregateFunction> expected_aggregate;
  if (aggregate_registry_identity) {
    expected_strategy = CanonicalWindowRuntimeStrategy::aggregate;
    expected_aggregate = request.descriptor.aggregate_function;
  } else {
    switch (request.descriptor.function) {
      case CanonicalWindowRuntimeFunction::row_number:
        expected_strategy = CanonicalWindowRuntimeStrategy::ranking;
        expected_ranking = CanonicalWindowRankingFunction::row_number;
        break;
      case CanonicalWindowRuntimeFunction::rank:
        expected_strategy = CanonicalWindowRuntimeStrategy::ranking;
        expected_ranking = CanonicalWindowRankingFunction::rank;
        break;
      case CanonicalWindowRuntimeFunction::dense_rank:
        expected_strategy = CanonicalWindowRuntimeStrategy::ranking;
        expected_ranking = CanonicalWindowRankingFunction::dense_rank;
        break;
      case CanonicalWindowRuntimeFunction::percent_rank:
        expected_strategy = CanonicalWindowRuntimeStrategy::ranking;
        expected_ranking = CanonicalWindowRankingFunction::percent_rank;
        break;
      case CanonicalWindowRuntimeFunction::cume_dist:
        expected_strategy = CanonicalWindowRuntimeStrategy::ranking;
        expected_ranking = CanonicalWindowRankingFunction::cume_dist;
        break;
      case CanonicalWindowRuntimeFunction::ntile:
        expected_strategy = CanonicalWindowRuntimeStrategy::ranking;
        expected_ranking = CanonicalWindowRankingFunction::ntile;
        break;
      case CanonicalWindowRuntimeFunction::lag:
        expected_strategy = CanonicalWindowRuntimeStrategy::value;
        expected_value = CanonicalWindowValueFunction::lag;
        break;
      case CanonicalWindowRuntimeFunction::lead:
        expected_strategy = CanonicalWindowRuntimeStrategy::value;
        expected_value = CanonicalWindowValueFunction::lead;
        break;
      case CanonicalWindowRuntimeFunction::first_value:
        expected_strategy = CanonicalWindowRuntimeStrategy::value;
        expected_value = CanonicalWindowValueFunction::first_value;
        break;
      case CanonicalWindowRuntimeFunction::last_value:
        expected_strategy = CanonicalWindowRuntimeStrategy::value;
        expected_value = CanonicalWindowValueFunction::last_value;
        break;
      case CanonicalWindowRuntimeFunction::nth_value:
        expected_strategy = CanonicalWindowRuntimeStrategy::value;
        expected_value = CanonicalWindowValueFunction::nth_value;
        break;
      case CanonicalWindowRuntimeFunction::unknown:
        break;
    }
  }
  if (expected_strategy == CanonicalWindowRuntimeStrategy::unknown ||
      (request.forced_strategy.has_value() &&
       (*request.forced_strategy == CanonicalWindowRuntimeStrategy::unknown ||
        *request.forced_strategy != expected_strategy))) {
    return refuse("QOW-DIAG-WINDOW-STRATEGY",
                  "forced window strategy does not match the registry function class");
  }
  const auto payload_count = static_cast<unsigned>(request.ranking.has_value()) +
                             static_cast<unsigned>(request.value.has_value()) +
                             static_cast<unsigned>(
                                 request.registry_aggregate.has_value()) +
                             static_cast<unsigned>(
                                 request.registry_aggregate_spill.has_value());
  const auto aggregate_payload_count =
      static_cast<unsigned>(request.registry_aggregate.has_value()) +
      static_cast<unsigned>(request.registry_aggregate_spill.has_value());
  if (payload_count != 1 ||
      (expected_strategy == CanonicalWindowRuntimeStrategy::ranking) !=
          request.ranking.has_value() ||
      (expected_strategy == CanonicalWindowRuntimeStrategy::value) !=
          request.value.has_value() ||
      (expected_strategy == CanonicalWindowRuntimeStrategy::aggregate) !=
          (aggregate_payload_count == 1)) {
    return refuse("QOW-DIAG-WINDOW-RUNTIME-PAYLOAD",
                  "window runtime requires exactly one class-matching strategy payload");
  }

  const CanonicalWindowFrameResult* payload_frames = nullptr;
  if (request.ranking.has_value()) payload_frames = &request.ranking->frames;
  if (request.value.has_value()) payload_frames = &request.value->frames;
  if (request.registry_aggregate.has_value())
    payload_frames = &request.registry_aggregate->frames;
  if (request.registry_aggregate_spill.has_value())
    payload_frames = &request.registry_aggregate_spill->aggregate_request.frames;
  if (payload_frames == nullptr) {
    return refuse("QOW-DIAG-WINDOW-RUNTIME-PAYLOAD",
                  "window runtime payload has no frame authority");
  }
  const auto runtime_authority = RevalidateCanonicalWindowFrameAuthority(
      request.mga_authority, *payload_frames);
  if (!runtime_authority.ok) {
    return refuse(runtime_authority.diagnostic_code,
                  runtime_authority.detail);
  }
  const auto& execution_authority = CanonicalWindowFrameExecutionAuthority(
      request.mga_authority, *payload_frames);

  // The unified runtime result is the sole owner of the published payload.
  // Strategy receipts remain available for descriptor/authority inspection,
  // but retaining their value vector as well would duplicate the complete
  // result outside the selected node's memory grant.
  const auto publish = [&](auto& strategy_result) {
    result.diagnostic = strategy_result.diagnostic;
    if (!result.diagnostic.ok) {
      result.values.clear();
      return false;
    }
    if (!PhysicalMgaStatementContextEqual(
            strategy_result.mga_statement_context,
            execution_authority.statement_context)) {
      result.diagnostic.ok = false;
      result.diagnostic.diagnostic_code = "QOW-DIAG-WINDOW-RUNTIME-MGA";
      result.diagnostic.detail =
          "window strategy returned a different MGA statement context";
      result.values.clear();
      return false;
    }
    result.values = std::move(strategy_result.values);
    result.authority = strategy_result.authority;
    result.window_property_uuid = strategy_result.window_property_uuid;
    result.selected_plan_uuid = strategy_result.selected_plan_uuid;
    result.executed_physical_node_id =
        strategy_result.executed_physical_node_id;
    result.causal_counter_id = strategy_result.causal_counter_id;
    return true;
  };
  if (expected_strategy == CanonicalWindowRuntimeStrategy::ranking) {
    if (!expected_ranking.has_value() ||
        request.ranking->function != *expected_ranking ||
        request.ranking->function_uuid != request.descriptor.function_uuid) {
      return refuse("QOW-DIAG-WINDOW-RUNTIME-PAYLOAD",
                    "ranking payload does not match the runtime descriptor");
    }
    auto ranking_result =
        ExecuteCanonicalWindowRankingStrategy(*request.ranking);
    std::optional<std::uint64_t> expected_ntile_bucket_count;
    if (request.ranking->ntile_bucket_count.has_value()) {
      std::uint64_t bucket_count = 0;
      if (!CanonicalRankingNtileOperand(
              *request.ranking->ntile_bucket_count,
              *request.ranking, &bucket_count)) {
        return refuse(
            "QOW-DIAG-WINDOW-RUNTIME-PAYLOAD",
            "ranking payload did not retain a canonical NTILE operand");
      }
      expected_ntile_bucket_count = bucket_count;
    }
    if (ranking_result.diagnostic.ok &&
        (ranking_result.function != *expected_ranking ||
         ranking_result.function_uuid != request.descriptor.function_uuid ||
         !SameWindowValueDescriptor(ranking_result.output_descriptor,
                                    request.ranking->output_descriptor) ||
         !ranking_result.every_function_operand_consumed ||
         !ranking_result.partition_peer_metadata_consumed ||
         !ranking_result.frame_and_exclusion_validated_then_ignored ||
         ranking_result.window_property_uuid !=
             request.ranking->frames.window_property_uuid ||
         ranking_result.partition_property_uuid !=
             request.ranking->frames.partition_property_uuid ||
         ranking_result.ordering_property_uuid !=
             request.ranking->frames.ordering_property_uuid ||
         ranking_result.term_binding_evidence_uuid !=
             request.ranking->frames.term_binding_evidence_uuid ||
         ranking_result.deterministic_tie_evidence_uuid !=
             request.ranking->frames.deterministic_tie_evidence_uuid ||
         ranking_result.frame_property_binding_evidence_uuid !=
             request.ranking->frames.frame_property_binding_evidence_uuid ||
         ranking_result.resolved_ntile_bucket_count !=
             expected_ntile_bucket_count)) {
      return refuse(
          "QOW-DIAG-WINDOW-RUNTIME-PAYLOAD",
          "ranking strategy did not return its exact descriptor and binding receipts");
    }
    result.ranking_strategy_result = std::move(ranking_result);
    if (!publish(*result.ranking_strategy_result)) return result;
  } else if (expected_strategy == CanonicalWindowRuntimeStrategy::value) {
    if (!expected_value.has_value() ||
        request.value->function != *expected_value ||
        request.value->function_uuid != request.descriptor.function_uuid) {
      return refuse("QOW-DIAG-WINDOW-RUNTIME-PAYLOAD",
                    "value payload does not match the runtime descriptor");
    }
    auto value_result =
        ExecuteCanonicalWindowValueStrategy(*request.value);
    const bool navigation =
        request.value->function == CanonicalWindowValueFunction::lag ||
        request.value->function == CanonicalWindowValueFunction::lead;
    std::vector<std::uint64_t> expected_positions;
    if (value_result.diagnostic.ok && navigation) {
      expected_positions.assign(request.value->frames.ordered_batch.rows.size(),
                                1);
      if (request.value->offset_values.has_value()) {
        expected_positions.clear();
        expected_positions.reserve(request.value->offset_values->size());
        for (const auto& operand : *request.value->offset_values) {
          std::int64_t decoded = 0;
          if (!CanonicalWindowInt64Operand(operand, *request.value,
                                           &decoded) ||
              decoded < 0) {
            return refuse(
                "QOW-DIAG-WINDOW-RUNTIME-PAYLOAD",
                "value payload did not retain a canonical navigation offset");
          }
          expected_positions.push_back(
              static_cast<std::uint64_t>(decoded));
        }
      }
    } else if (value_result.diagnostic.ok &&
               request.value->function ==
                   CanonicalWindowValueFunction::nth_value) {
      expected_positions.reserve(request.value->nth_values->size());
      for (const auto& operand : *request.value->nth_values) {
        std::int64_t decoded = 0;
        if (!CanonicalWindowInt64Operand(operand, *request.value, &decoded) ||
            decoded <= 0) {
          return refuse(
              "QOW-DIAG-WINDOW-RUNTIME-PAYLOAD",
              "value payload did not retain a canonical NTH_VALUE operand");
        }
        expected_positions.push_back(static_cast<std::uint64_t>(decoded));
      }
    }
    const auto expected_default_count =
        request.value->default_values.has_value()
            ? request.value->default_values->size()
            : 0;
    if (value_result.diagnostic.ok &&
        (value_result.function != *expected_value ||
         value_result.function_uuid != request.descriptor.function_uuid ||
         value_result.value_expression_descriptor_id !=
             request.value->value_expression_descriptor_id ||
         !SameWindowResultColumn(value_result.result_column,
                                 request.value->result_column) ||
         value_result.resolved_positions != expected_positions ||
         value_result.resolved_nth_origin != request.value->nth_origin ||
         value_result.resolved_null_treatment !=
             request.value->null_treatment ||
         value_result.converted_source_value_count !=
             request.value->frames.ordered_batch.rows.size() ||
         value_result.converted_default_value_count !=
             expected_default_count ||
         value_result.used_implicit_navigation_offset !=
             (navigation && !request.value->offset_values.has_value()) ||
         value_result.explicit_navigation_default_present !=
             (navigation && request.value->default_values.has_value()) ||
         !value_result.every_function_operand_consumed ||
         value_result.partition_metadata_consumed_for_navigation !=
             navigation ||
         value_result.effective_frame_membership_consumed != !navigation ||
         !value_result.frame_and_exclusion_validated ||
         value_result.frame_and_exclusion_ignored_for_navigation !=
             navigation ||
         value_result.window_property_uuid !=
             request.value->frames.window_property_uuid ||
         value_result.partition_property_uuid !=
             request.value->frames.partition_property_uuid ||
         value_result.ordering_property_uuid !=
             request.value->frames.ordering_property_uuid ||
         value_result.term_binding_evidence_uuid !=
             request.value->frames.term_binding_evidence_uuid ||
         value_result.deterministic_tie_evidence_uuid !=
             request.value->frames.deterministic_tie_evidence_uuid ||
         value_result.frame_property_binding_evidence_uuid !=
             request.value->frames.frame_property_binding_evidence_uuid)) {
      return refuse(
          "QOW-DIAG-WINDOW-RUNTIME-PAYLOAD",
          "value strategy did not return its exact descriptor, operand, and binding receipts");
    }
    result.value_strategy_result = std::move(value_result);
    if (!publish(*result.value_strategy_result)) return result;
  } else {
    const auto& aggregate = request.registry_aggregate.has_value()
                                ? *request.registry_aggregate
                                : request.registry_aggregate_spill
                                      ->aggregate_request;
    const auto& aggregate_descriptor = aggregate.aggregate_template.descriptor;
    if (!expected_aggregate.has_value() ||
        aggregate_descriptor.abi_version != request.descriptor.abi_version ||
        aggregate_descriptor.function != *expected_aggregate ||
        aggregate_descriptor.builtin_id != request.descriptor.builtin_id ||
        aggregate_descriptor.function_uuid !=
            request.descriptor.function_uuid) {
      return refuse("QOW-DIAG-WINDOW-RUNTIME-PAYLOAD",
                    "aggregate-registry payload does not match the runtime descriptor");
    }
    CanonicalRegistryWindowAggregateResult aggregate_result;
    if (request.registry_aggregate_spill.has_value()) {
      auto spill_result =
          ExecuteCanonicalRegistryWindowAggregateSpillStrategy(
              *request.registry_aggregate_spill);
      if (!spill_result.diagnostic.ok) {
        result.diagnostic = spill_result.diagnostic;
        result.values.clear();
        // A failed spill has already cleared its aggregate payload, so retain
        // the sole failure receipt intact. Cancellation cleanup and spill
        // evidence must remain observable through the public wrapper.
        result.aggregate_spill_strategy_result = std::move(spill_result);
        return result;
      }
      if (!PhysicalMgaStatementContextEqual(
              spill_result.mga_statement_context,
              execution_authority.statement_context)) {
        return refuse("QOW-DIAG-WINDOW-RUNTIME-MGA",
                      "window spill wrapper returned a different MGA statement context");
      }
      // The unified runtime retains exactly one potentially large aggregate
      // receipt. Spill-only evidence is detached below; the compatibility
      // wrapper reconstructs its owning result without duplicating frame or
      // transition vectors here.
      aggregate_result = std::move(spill_result.aggregate_result);
      result.aggregate_state_spill_used = spill_result.spilled;
      result.aggregate_spill_reopened = spill_result.spill_reopened;
      result.aggregate_spill_cleanup_proven = spill_result.cleanup_proven;
      result.aggregate_spilled_state_count =
          spill_result.spilled_aggregate_state_count;
      result.aggregate_serialized_state_bytes =
          spill_result.serialized_aggregate_state_bytes;
      result.aggregate_spilled_state_record_count =
          spill_result.spilled_aggregate_state_record_count;
      result.aggregate_spill_evidence = std::move(spill_result.spill_evidence);
    } else {
      aggregate_result =
          ExecuteCanonicalRegistryWindowAggregateStrategy(aggregate);
    }
    std::vector<std::vector<std::size_t>> expected_frame_rows;
    expected_frame_rows.reserve(aggregate.frames.effective_frames.size());
    std::size_t expected_frame_input_row_count = 0;
    for (const auto& frame : aggregate.frames.effective_frames) {
      expected_frame_rows.push_back(frame.effective_row_indices);
      expected_frame_input_row_count += frame.effective_row_indices.size();
    }
    const auto expected_modifier_count =
        (aggregate.aggregate_template.filter_truth_values.has_value() ? 1U
                                                                      : 0U) +
        (aggregate.aggregate_template.distinct ? 1U : 0U) +
        (!aggregate.aggregate_template.aggregate_order_terms.empty() ? 1U
                                                                      : 0U);
    if (aggregate_result.diagnostic.ok &&
        (aggregate_result.descriptor.abi_version !=
             aggregate.aggregate_template.descriptor.abi_version ||
         aggregate_result.descriptor.function !=
             aggregate.aggregate_template.descriptor.function ||
         aggregate_result.descriptor.builtin_id !=
             aggregate.aggregate_template.descriptor.builtin_id ||
         aggregate_result.descriptor.function_uuid !=
             aggregate.aggregate_template.descriptor.function_uuid ||
         aggregate_result.descriptor.count_star !=
             aggregate.aggregate_template.descriptor.count_star ||
         !SameWindowResultColumn(aggregate_result.result_column,
                                 aggregate.aggregate_template.result_column) ||
         aggregate_result.value_columns !=
             aggregate.aggregate_template.value_columns ||
         aggregate_result.value_expression_descriptor_ids !=
             aggregate.aggregate_template.value_expression_descriptor_ids ||
         aggregate_result.values.size() !=
             aggregate.frames.ordered_batch.rows.size() ||
         aggregate_result.frame_row_indices != expected_frame_rows ||
         aggregate_result.transition_row_indices.size() !=
             aggregate.frames.effective_frames.size() ||
         aggregate_result.frame_input_row_count !=
             expected_frame_input_row_count ||
         aggregate_result.direct_argument_count !=
             aggregate.aggregate_template.direct_arguments.size() ||
         aggregate_result.modifier_count != expected_modifier_count ||
         aggregate_result.aggregate_order_term_count !=
             aggregate.aggregate_template.aggregate_order_terms.size() ||
         !aggregate_result.every_aggregate_descriptor_field_consumed ||
         !aggregate_result.every_effective_frame_consumed ||
         !aggregate_result.modifier_pipeline_validated ||
         aggregate_result.filter_modifier_applied !=
             aggregate.aggregate_template.filter_truth_values.has_value() ||
         aggregate_result.distinct_modifier_applied !=
             aggregate.aggregate_template.distinct ||
         !aggregate_result.filter_applied_before_distinct ||
         !aggregate_result.distinct_applied_before_order ||
         aggregate_result.aggregate_order_applied !=
             !aggregate.aggregate_template.aggregate_order_terms.empty() ||
         aggregate_result.cancellation_observed ||
         !aggregate_result.transient_state_cleanup_proven ||
         !aggregate_result.all_or_nothing_publication ||
         aggregate_result.window_property_uuid !=
             aggregate.frames.window_property_uuid ||
         aggregate_result.partition_property_uuid !=
             aggregate.frames.partition_property_uuid ||
         aggregate_result.ordering_property_uuid !=
             aggregate.frames.ordering_property_uuid ||
         aggregate_result.term_binding_evidence_uuid !=
             aggregate.frames.term_binding_evidence_uuid ||
         aggregate_result.deterministic_tie_evidence_uuid !=
             aggregate.frames.deterministic_tie_evidence_uuid ||
         aggregate_result.frame_property_binding_evidence_uuid !=
             aggregate.frames.frame_property_binding_evidence_uuid)) {
      return refuse(
          "QOW-DIAG-WINDOW-RUNTIME-PAYLOAD",
          "aggregate strategy did not return its exact descriptor, modifier, frame, and binding receipts");
    }
    result.aggregate_strategy_result = std::move(aggregate_result);
    auto& published_aggregate = *result.aggregate_strategy_result;
    if (!publish(published_aggregate)) return result;
    result.aggregate_registry_bridge_used = true;
    result.moving_inverse_state_used =
        published_aggregate.moving_inverse_state_used;
    result.effective_frame_recomputed =
        published_aggregate.effective_frame_recomputed;
    result.aggregate_state_strategy_selected_from_physical_plan =
        published_aggregate.state_strategy_selected_from_physical_plan;
    result.selected_aggregate_state_strategy =
        published_aggregate.selected_state_strategy;
    result.selected_aggregate_state_implementation_id =
        published_aggregate.selected_state_implementation_id;
    result.aggregate_transition_count = published_aggregate.transition_count;
    result.aggregate_inverse_transition_count =
        published_aggregate.inverse_transition_count;
  }

  result.descriptor = request.descriptor;
  result.executed_strategy = expected_strategy;
  result.every_descriptor_field_consumed = true;
  result.exactly_one_strategy_payload_consumed = true;
  result.retained_strategy_reached = true;
  result.canonical_registry_state_frame_executor_used = true;
  result.split_runtime_bypass_forbidden = true;
  const auto result_authority = RevalidateCanonicalWindowFrameAuthority(
      request.mga_authority, *payload_frames);
  if (!result_authority.ok) {
    return refuse(result_authority.diagnostic_code, result_authority.detail);
  }
  result.mga_statement_context = execution_authority.statement_context;
  return result;
}

CanonicalWindowRankingResult ExecuteCanonicalWindowRanking(
    const CanonicalWindowRankingRequest& request) {
  CanonicalWindowRuntimeFunction runtime_function =
      CanonicalWindowRuntimeFunction::unknown;
  switch (request.function) {
    case CanonicalWindowRankingFunction::row_number:
      runtime_function = CanonicalWindowRuntimeFunction::row_number;
      break;
    case CanonicalWindowRankingFunction::rank:
      runtime_function = CanonicalWindowRuntimeFunction::rank;
      break;
    case CanonicalWindowRankingFunction::dense_rank:
      runtime_function = CanonicalWindowRuntimeFunction::dense_rank;
      break;
    case CanonicalWindowRankingFunction::percent_rank:
      runtime_function = CanonicalWindowRuntimeFunction::percent_rank;
      break;
    case CanonicalWindowRankingFunction::cume_dist:
      runtime_function = CanonicalWindowRuntimeFunction::cume_dist;
      break;
    case CanonicalWindowRankingFunction::ntile:
      runtime_function = CanonicalWindowRuntimeFunction::ntile;
      break;
  }
  const auto registry = CanonicalWindowRuntimeRegistryV1();
  const auto descriptor = std::ranges::find_if(
      registry, [&](const auto& row) {
        return row.function == runtime_function;
      });
  if (descriptor == registry.end()) {
    CanonicalWindowRankingResult refused;
    refused.diagnostic = WindowRankingRefusal(
        request.function, "ranking function has no canonical runtime row");
    return refused;
  }
  CanonicalWindowRuntimeRequest runtime_request;
  runtime_request.descriptor = *descriptor;
  runtime_request.ranking = request;
  runtime_request.forced_strategy = CanonicalWindowRuntimeStrategy::ranking;
  runtime_request.mga_authority = request.mga_authority;
  auto runtime = ExecuteCanonicalWindowRuntime(runtime_request);
  if (runtime.diagnostic.ok && runtime.ranking_strategy_result.has_value()) {
    runtime.ranking_strategy_result->values = std::move(runtime.values);
    return std::move(*runtime.ranking_strategy_result);
  }
  CanonicalWindowRankingResult refused;
  refused.diagnostic = std::move(runtime.diagnostic);
  return refused;
}

CanonicalWindowValueResult ExecuteCanonicalWindowValue(
    const CanonicalWindowValueRequest& request) {
  CanonicalWindowRuntimeFunction runtime_function =
      CanonicalWindowRuntimeFunction::unknown;
  switch (request.function) {
    case CanonicalWindowValueFunction::lag:
      runtime_function = CanonicalWindowRuntimeFunction::lag;
      break;
    case CanonicalWindowValueFunction::lead:
      runtime_function = CanonicalWindowRuntimeFunction::lead;
      break;
    case CanonicalWindowValueFunction::first_value:
      runtime_function = CanonicalWindowRuntimeFunction::first_value;
      break;
    case CanonicalWindowValueFunction::last_value:
      runtime_function = CanonicalWindowRuntimeFunction::last_value;
      break;
    case CanonicalWindowValueFunction::nth_value:
      runtime_function = CanonicalWindowRuntimeFunction::nth_value;
      break;
  }
  const auto registry = CanonicalWindowRuntimeRegistryV1();
  const auto descriptor = std::ranges::find_if(
      registry, [&](const auto& row) {
        return row.function == runtime_function;
      });
  if (descriptor == registry.end()) {
    CanonicalWindowValueResult refused;
    refused.diagnostic = WindowValueRefusal(
        "QOW-DIAG-WINDOW-FUNCTION-DESCRIPTOR",
        "window value function has no canonical runtime row");
    return refused;
  }
  CanonicalWindowRuntimeRequest runtime_request;
  runtime_request.descriptor = *descriptor;
  runtime_request.value = request;
  runtime_request.forced_strategy = CanonicalWindowRuntimeStrategy::value;
  runtime_request.mga_authority = request.mga_authority;
  auto runtime = ExecuteCanonicalWindowRuntime(runtime_request);
  if (runtime.diagnostic.ok && runtime.value_strategy_result.has_value()) {
    runtime.value_strategy_result->values = std::move(runtime.values);
    return std::move(*runtime.value_strategy_result);
  }
  CanonicalWindowValueResult refused;
  refused.diagnostic = std::move(runtime.diagnostic);
  return refused;
}

CanonicalRegistryWindowAggregateResult
ExecuteCanonicalRegistryWindowAggregate(
    const CanonicalRegistryWindowAggregateRequest& request) {
  const auto& aggregate = request.aggregate_template.descriptor;
  CanonicalWindowRuntimeDescriptor descriptor;
  descriptor.abi_version = aggregate.abi_version;
  descriptor.builtin_id = aggregate.builtin_id;
  descriptor.function_uuid = aggregate.function_uuid;
  descriptor.aggregate_function = aggregate.function;
  CanonicalWindowRuntimeRequest runtime_request;
  runtime_request.descriptor = std::move(descriptor);
  runtime_request.registry_aggregate = request;
  runtime_request.forced_strategy = CanonicalWindowRuntimeStrategy::aggregate;
  runtime_request.mga_authority = request.aggregate_template.mga_authority;
  auto runtime = ExecuteCanonicalWindowRuntime(runtime_request);
  if (runtime.diagnostic.ok && runtime.aggregate_strategy_result.has_value()) {
    runtime.aggregate_strategy_result->values = std::move(runtime.values);
    return std::move(*runtime.aggregate_strategy_result);
  }
  CanonicalRegistryWindowAggregateResult refused;
  refused.diagnostic = std::move(runtime.diagnostic);
  return refused;
}

// QOW-SOURCE-WIN-012-STATE-V1
// QOW-SOURCE-WIN-012-FILTER-V1
// QOW-SOURCE-WIN-012-DISTINCT-V1
// QOW-SOURCE-WIN-012-ORDER-V1
// QOW-SOURCE-WIN-012-FRAME-V1
// Compatibility entry point for the original int64 SUM-window contract.
// It carries no state implementation: the request is translated onto the
// canonical aggregate registry and enters the same descriptor/state/frame
// dispatcher as every other aggregate-as-window function.
CanonicalWindowAggregateResult ExecuteCanonicalWindowAggregate(
    const CanonicalWindowAggregateRequest& request) {
  namespace api = scratchbird::engine::internal_api;
  CanonicalWindowAggregateResult result;
  const auto refuse = [&](std::string code, std::string detail) {
    CanonicalWindowAggregateResult refused;
    refused.diagnostic.ok = false;
    refused.diagnostic.diagnostic_code = std::move(code);
    refused.diagnostic.detail = std::move(detail);
    return refused;
  };
  const auto* sum = LookupCanonicalAggregateByFunctionV1(
      CanonicalAggregateFunction::sum);
  if (request.function != CanonicalWindowAggregateFunction::int64_sum ||
      sum == nullptr || !sum->executable || !sum->aggregate_as_window ||
      request.function_uuid != sum->function_uuid) {
    return refuse("QOW-DIAG-WINDOW-AGGREGATE",
                  "SUM compatibility descriptor is not the canonical registry row");
  }
  if (request.parser_execution_authority_claimed ||
      request.transaction_finality_claimed ||
      request.recovery_authority_claimed) {
    return refuse("QOW-DIAG-WINDOW-AUTHORITY",
                  "SUM compatibility facade attempted to claim engine MGA authority");
  }
  if (request.aggregate_order_terms.empty()) {
    if (!request.deterministic_tie_evidence_uuid.empty()) {
      return refuse("QOW-DIAG-WINDOW-AGGREGATE-ORDER",
                    "unordered SUM compatibility request carries tie evidence");
    }
  } else if (!IsCanonicalUuid(request.deterministic_tie_evidence_uuid) ||
             request.maximum_pair_comparisons == 0) {
    return refuse("QOW-DIAG-WINDOW-AGGREGATE-ORDER",
                  "ordered SUM compatibility request lacks bounded tie evidence");
  }

  std::optional<std::size_t> value_column;
  for (std::size_t column = 0;
       column < request.frames.ordered_batch.columns.size(); ++column) {
    if (request.frames.ordered_batch.columns[column].descriptor_id !=
        request.value_expression_descriptor_id) {
      continue;
    }
    if (value_column.has_value()) {
      return refuse("QOW-DIAG-WINDOW-AGGREGATE",
                    "SUM compatibility value descriptor is ambiguous");
    }
    value_column = column;
  }
  if (!value_column.has_value()) {
    return refuse("QOW-DIAG-WINDOW-AGGREGATE",
                  "SUM compatibility value descriptor is unresolved");
  }

  CanonicalRegistryWindowAggregateRequest canonical;
  canonical.frames = request.frames;
  canonical.maximum_output_rows = request.maximum_output_rows;
  std::size_t frame_input_row_count = 0;
  for (const auto& frame : request.frames.effective_frames) {
    if (frame_input_row_count >
        std::numeric_limits<std::size_t>::max() -
            frame.effective_row_indices.size()) {
      return refuse("QOW-DIAG-WINDOW-AGGREGATE-FRAME",
                    "SUM compatibility frame-input receipt overflowed");
    }
    frame_input_row_count += frame.effective_row_indices.size();
  }
  canonical.maximum_frame_input_row_count =
      std::max<std::size_t>(1, frame_input_row_count);
  canonical.maximum_transition_count = request.maximum_transition_count;
  const auto canonical_per_frame_distinct_bound =
      !request.distinct
          ? std::size_t{1}
          : request.maximum_distinct_value_count ==
                    std::numeric_limits<std::size_t>::max()
                ? request.maximum_distinct_value_count
                : request.maximum_distinct_value_count + 1;
  const auto distinct_frame_count =
      std::max<std::size_t>(1, request.frames.effective_frames.size());
  canonical.maximum_distinct_tuple_count =
      canonical_per_frame_distinct_bound >
              std::numeric_limits<std::size_t>::max() / distinct_frame_count
          ? std::numeric_limits<std::size_t>::max()
          : canonical_per_frame_distinct_bound * distinct_frame_count;
  canonical.maximum_order_comparison_count =
      request.aggregate_order_terms.empty()
          ? std::size_t{1}
          : request.maximum_pair_comparisons;
  canonical.maximum_combined_final_output_bytes =
      request.maximum_combined_final_output_bytes;

  auto& aggregate = canonical.aggregate_template;
  aggregate.physical_dag = request.frames.physical_dag;
  aggregate.selected_physical_node_id =
      request.frames.executed_physical_node_id;
  bool selected_node_found = false;
  for (auto& node : aggregate.physical_dag.nodes) {
    if (node.physical_node_id != aggregate.selected_physical_node_id) continue;
    node.node_kind = PhysicalNodeKind::kAggregate;
    node.implementation_id =
        "window.aggregate-registry-frame-recompute.v1";
    node.output_descriptor_ids = {request.result_column.descriptor_id};
    selected_node_found = true;
  }
  if (!selected_node_found) {
    return refuse("QOW-DIAG-WINDOW-AGGREGATE-FRAME",
                  "SUM compatibility frame has no selected physical node");
  }
  aggregate.descriptor = {sum->abi_version, sum->function, sum->builtin_id,
                          sum->function_uuid, false};
  aggregate.value_columns = {*value_column};
  aggregate.value_expression_descriptor_ids = {
      request.value_expression_descriptor_id};
  aggregate.result_column = request.result_column;
  aggregate.filter_truth_values = request.filter_truth_values;
  aggregate.distinct = request.distinct;
  aggregate.aggregate_order_terms = request.aggregate_order_terms;
  aggregate.maximum_transition_count = request.maximum_transition_count;
  aggregate.maximum_distinct_value_count =
      canonical_per_frame_distinct_bound;
  aggregate.maximum_order_comparison_count =
      canonical.maximum_order_comparison_count;
  aggregate.maximum_final_output_bytes =
      request.maximum_final_output_bytes;
  aggregate.maximum_finalization_workspace_bytes =
      request.maximum_finalization_workspace_bytes;
  aggregate.mga_authority = request.mga_authority;

  auto registry_result = ExecuteCanonicalRegistryWindowAggregate(canonical);
  if (!registry_result.diagnostic.ok) {
    return refuse(std::move(registry_result.diagnostic.diagnostic_code),
                  std::move(registry_result.diagnostic.detail));
  }

  // Preserve the original facade's proof vocabulary. SQL NULL participates
  // in ordinary SUM transitions, but the historical DISTINCT receipt counted
  // only non-NULL typed values even though the registry correctly owns the
  // actual state transition and finalization.
  result.transition_row_indices =
      std::move(registry_result.transition_row_indices);
  std::size_t transition_count = 0;
  std::size_t distinct_value_count = 0;
  std::size_t pair_comparison_count = 0;
  for (auto& rows : result.transition_row_indices) {
    if (request.distinct) {
      rows.erase(std::remove_if(rows.begin(), rows.end(), [&](const auto row) {
                   return request.frames.ordered_batch.rows[row]
                              .values[*value_column]
                              .state == api::EngineValueState::sql_null;
                 }),
                 rows.end());
      if (rows.size() > request.maximum_distinct_value_count) {
        return refuse("QOW-DIAG-WINDOW-AGGREGATE-DISTINCT",
                      "SUM compatibility frame DISTINCT bound was exceeded");
      }
      if (distinct_value_count >
          std::numeric_limits<std::size_t>::max() - rows.size()) {
        return refuse("QOW-DIAG-WINDOW-AGGREGATE-DISTINCT",
                      "SUM compatibility DISTINCT receipt overflowed");
      }
      distinct_value_count += rows.size();
    }
    if (transition_count >
        std::numeric_limits<std::size_t>::max() - rows.size()) {
      return refuse("QOW-DIAG-WINDOW-AGGREGATE-FRAME",
                    "SUM compatibility transition receipt overflowed");
    }
    transition_count += rows.size();
    if (!request.aggregate_order_terms.empty()) {
      if (rows.size() != 0 &&
          rows.size() > std::numeric_limits<std::size_t>::max() / rows.size()) {
        return refuse("QOW-DIAG-WINDOW-AGGREGATE-ORDER",
                      "SUM compatibility comparison receipt overflowed");
      }
      const auto comparisons = rows.size() * rows.size();
      if (comparisons > request.maximum_pair_comparisons ||
          pair_comparison_count >
              request.maximum_pair_comparisons - comparisons) {
        return refuse("QOW-DIAG-WINDOW-AGGREGATE-ORDER",
                      "SUM compatibility comparison bound was exceeded");
      }
      pair_comparison_count += comparisons;
    }
  }
  if (transition_count > request.maximum_transition_count) {
    return refuse("QOW-DIAG-WINDOW-AGGREGATE-FRAME",
                  "SUM compatibility transition bound was exceeded");
  }
  result.diagnostic = {};
  result.function = request.function;
  result.values = std::move(registry_result.values);
  result.transition_count = transition_count;
  result.distinct_value_count = distinct_value_count;
  result.pair_comparison_count = pair_comparison_count;
  result.combined_final_output_bytes =
      registry_result.combined_final_output_bytes;
  result.peak_finalization_workspace_bytes =
      registry_result.peak_finalization_workspace_bytes;
  result.filter_applied_before_transition =
      request.filter_truth_values.has_value();
  result.distinct_applied_before_transition = request.distinct;
  result.aggregate_order_independent_of_window_order =
      !request.aggregate_order_terms.empty();
  result.effective_frame_recomputed =
      registry_result.effective_frame_recomputed;
  result.shared_aggregate_state_authority_used =
      registry_result.shared_aggregate_state_authority_used;
  result.canonical_registry_state_frame_executor_used = true;
  result.split_runtime_bypass_forbidden = true;
  result.authority = registry_result.authority;
  result.window_property_uuid = registry_result.window_property_uuid;
  result.selected_plan_uuid = registry_result.selected_plan_uuid;
  result.executed_physical_node_id =
      registry_result.executed_physical_node_id;
  result.causal_counter_id = registry_result.causal_counter_id;
  result.mga_statement_context = registry_result.mga_statement_context;
  return result;
}

CanonicalRegistryWindowAggregateSpillResult
ExecuteCanonicalRegistryWindowAggregateSpill(
    const CanonicalRegistryWindowAggregateSpillRequest& request) {
  const auto& aggregate =
      request.aggregate_request.aggregate_template.descriptor;
  CanonicalWindowRuntimeDescriptor descriptor;
  descriptor.abi_version = aggregate.abi_version;
  descriptor.builtin_id = aggregate.builtin_id;
  descriptor.function_uuid = aggregate.function_uuid;
  descriptor.aggregate_function = aggregate.function;
  CanonicalWindowRuntimeRequest runtime_request;
  runtime_request.descriptor = std::move(descriptor);
  runtime_request.registry_aggregate_spill = request;
  runtime_request.forced_strategy = CanonicalWindowRuntimeStrategy::aggregate;
  runtime_request.mga_authority =
      request.aggregate_request.aggregate_template.mga_authority;
  auto runtime = ExecuteCanonicalWindowRuntime(runtime_request);
  if (runtime.aggregate_spill_strategy_result.has_value()) {
    return std::move(*runtime.aggregate_spill_strategy_result);
  }
  if (runtime.diagnostic.ok && runtime.aggregate_strategy_result.has_value()) {
    CanonicalRegistryWindowAggregateSpillResult spill;
    runtime.aggregate_strategy_result->values = std::move(runtime.values);
    spill.diagnostic = runtime.diagnostic;
    spill.aggregate_result = std::move(*runtime.aggregate_strategy_result);
    spill.spilled_aggregate_state_count =
        runtime.aggregate_spilled_state_count;
    spill.serialized_aggregate_state_bytes =
        runtime.aggregate_serialized_state_bytes;
    spill.spilled_aggregate_state_record_count =
        runtime.aggregate_spilled_state_record_count;
    spill.spilled = runtime.aggregate_state_spill_used;
    spill.spill_reopened = runtime.aggregate_spill_reopened;
    spill.cleanup_proven = runtime.aggregate_spill_cleanup_proven;
    spill.spill_evidence = std::move(runtime.aggregate_spill_evidence);
    spill.mga_statement_context = runtime.mga_statement_context;
    return spill;
  }
  CanonicalRegistryWindowAggregateSpillResult refused;
  refused.diagnostic = std::move(runtime.diagnostic);
  return refused;
}

// QOW-SOURCE-WIN-015-MULTIPLE-V1
// QOW-SOURCE-WIN-015-QUALIFY-V1
// QOW-SOURCE-WIN-015-COMPOSITION-V1
// Independent function values are mapped from each exact window ordering back
// to the shared source-row identity before QUALIFY.  QUALIFY then consumes the
// shared TRUE-only 3VL rule before projection, final query order, and row
// limiting.  Optional composition evidence is an optimizer-published physical
// DAG containing ordinary upstream nodes; no query-shape route gains executor
// authority here.
CanonicalWindowCompositionResult ExecuteCanonicalWindowComposition(
    const CanonicalWindowCompositionRequest& request) {
  namespace api = scratchbird::engine::internal_api;
  CanonicalWindowCompositionResult result;
  const auto refuse = [&](std::string code, std::string detail) {
    result = {};
    result.diagnostic.ok = false;
    result.diagnostic.diagnostic_code = std::move(code);
    result.diagnostic.detail = std::move(detail);
    return result;
  };
  const auto descriptor_equal = [](const api::EngineDescriptor& left,
                                   const api::EngineDescriptor& right) {
    return left.descriptor_uuid.canonical == right.descriptor_uuid.canonical &&
           left.descriptor_kind == right.descriptor_kind &&
           left.canonical_type_name == right.canonical_type_name &&
           left.encoded_descriptor == right.encoded_descriptor;
  };
  const auto column_equal = [&](const ExecutorColumnDescriptor& left,
                                const ExecutorColumnDescriptor& right) {
    return left.stable_name == right.stable_name &&
           left.nullable == right.nullable &&
           left.descriptor_id == right.descriptor_id &&
           descriptor_equal(left.descriptor, right.descriptor);
  };
  const auto value_equal = [&](const api::EngineTypedValue& left,
                               const api::EngineTypedValue& right) {
    return descriptor_equal(left.descriptor, right.descriptor) &&
           left.encoded_value == right.encoded_value &&
           left.binary_value == right.binary_value &&
           left.is_null == right.is_null && left.state == right.state;
  };
  const auto authority_equal = [](
      const CanonicalPhysicalDispatchAuthorityEvidence& left,
      const CanonicalPhysicalDispatchAuthorityEvidence& right) {
    return left.engine_mga_snapshot_bound == right.engine_mga_snapshot_bound &&
           left.owns_transaction_finality == right.owns_transaction_finality &&
           left.owns_recovery == right.owns_recovery &&
           left.owns_parser_execution == right.owns_parser_execution &&
           left.owns_visibility_outside_engine_mga ==
               right.owns_visibility_outside_engine_mga &&
           left.wal_is_transaction_or_recovery_authority ==
               right.wal_is_transaction_or_recovery_authority;
  };

  if (request.shape_specific_parser_route_claimed ||
      request.shape_specific_execution_route_claimed) {
    return refuse("QOW-DIAG-WINDOW-COMPOSITION",
                  "shape-specific parser or executor route claimed window authority");
  }
  if (request.parser_execution_authority_claimed ||
      request.transaction_finality_claimed ||
      request.recovery_authority_claimed) {
    return refuse("QOW-DIAG-WINDOW-COMPOSITION",
                  "window composition attempted to claim engine MGA authority");
  }
  if (request.maximum_window_count == 0 || request.windows.empty() ||
      request.windows.size() > request.maximum_window_count ||
      request.maximum_output_rows == 0 ||
      request.input_batch.rows.size() > request.maximum_output_rows) {
    return refuse("QOW-DIAG-WINDOW-MULTIPLE",
                  "window count or output-row resource bound was exceeded");
  }
  std::vector<std::uint32_t> input_descriptor_ids;
  input_descriptor_ids.reserve(request.input_batch.columns.size());
  for (const auto& column : request.input_batch.columns) {
    input_descriptor_ids.push_back(column.descriptor_id);
  }
  const auto input_validation = ValidateCanonicalDescriptorBatch(
      request.input_batch, input_descriptor_ids);
  if (!input_validation.ok) {
    return refuse("QOW-DIAG-WINDOW-MULTIPLE",
                  input_validation.diagnostic_code + ":" +
                      input_validation.detail);
  }

  const auto& first_frames = request.windows.front().frames;
  const auto composition_authority = RevalidateCanonicalWindowFrameAuthority(
      request.mga_authority, first_frames);
  if (!composition_authority.ok) {
    return refuse("QOW-DIAG-WINDOW-COMPOSITION",
                  composition_authority.detail);
  }
  const auto& execution_authority = CanonicalWindowFrameExecutionAuthority(
      request.mga_authority, first_frames);
  if (!CanonicalWindowFrameEvidenceValid(first_frames) ||
      !first_frames.authority.engine_mga_snapshot_bound ||
      first_frames.authority.owns_transaction_finality ||
      first_frames.authority.owns_recovery ||
      first_frames.authority.owns_parser_execution ||
      first_frames.authority.owns_visibility_outside_engine_mga ||
      first_frames.authority.wal_is_transaction_or_recovery_authority) {
    return refuse("QOW-DIAG-WINDOW-MULTIPLE",
                  "first window lacks canonical engine-owned frame authority");
  }

  DescriptorBatch materialized = request.input_batch;
  std::set<std::uint32_t> descriptor_ids(input_descriptor_ids.begin(),
                                         input_descriptor_ids.end());
  std::set<std::string> function_state_uuids;
  const auto row_count = request.input_batch.rows.size();
  for (std::size_t window_index = 0;
       window_index < request.windows.size(); ++window_index) {
    const auto& window = request.windows[window_index];
    const auto window_authority = RevalidateCanonicalWindowFrameAuthority(
        execution_authority, window.frames);
    if (!window_authority.ok ||
        !CanonicalWindowFrameEvidenceValid(window.frames) ||
        window.frames.selected_plan_uuid != first_frames.selected_plan_uuid ||
        !PhysicalMgaStatementContextEqual(
            window.frames.mga_statement_context,
            first_frames.mga_statement_context) ||
        !authority_equal(window.frames.authority, first_frames.authority) ||
        !IsCanonicalUuid(window.function_state_uuid) ||
        !function_state_uuids.insert(window.function_state_uuid).second) {
      return refuse("QOW-DIAG-WINDOW-MULTIPLE",
                    "independent window authority or function-state identity drifted");
    }
    if (window.frames.ordered_batch.columns.size() !=
            request.input_batch.columns.size() ||
        window.frames.ordered_batch.rows.size() != row_count ||
        window.frames.row_metadata.size() != row_count ||
        window.values.size() != row_count) {
      return refuse("QOW-DIAG-WINDOW-MULTIPLE",
                    "window materialization cardinality does not match its source");
    }
    for (std::size_t column = 0;
         column < request.input_batch.columns.size(); ++column) {
      if (!column_equal(window.frames.ordered_batch.columns[column],
                        request.input_batch.columns[column])) {
        return refuse("QOW-DIAG-WINDOW-MULTIPLE",
                      "window materialization source schema drifted");
      }
    }
    if (window.result_column.descriptor_id == 0 ||
        window.result_column.stable_name.empty() ||
        !descriptor_ids.insert(window.result_column.descriptor_id).second ||
        !IsCanonicalUuid(
            window.result_column.descriptor.descriptor_uuid.canonical) ||
        window.result_column.descriptor.descriptor_kind.empty() ||
        window.result_column.descriptor.canonical_type_name.empty() ||
        window.result_column.descriptor.encoded_descriptor.empty()) {
      return refuse("QOW-DIAG-WINDOW-MULTIPLE",
                    "window result descriptor is missing, duplicated, or malformed");
    }

    DescriptorBatch value_validation_batch;
    value_validation_batch.columns = {window.result_column};
    value_validation_batch.rows.reserve(row_count);
    for (const auto& value : window.values) {
      value_validation_batch.rows.push_back({{value}});
    }
    const auto value_validation = ValidateCanonicalDescriptorBatch(
        value_validation_batch, {window.result_column.descriptor_id});
    if (!value_validation.ok) {
      return refuse("QOW-DIAG-WINDOW-MULTIPLE",
                    value_validation.diagnostic_code + ":" +
                        value_validation.detail);
    }

    std::vector<std::uint8_t> source_seen(row_count, 0);
    std::vector<api::EngineTypedValue> values_by_source(row_count);
    for (std::size_t ordered_row = 0; ordered_row < row_count;
         ++ordered_row) {
      const auto source_row =
          window.frames.row_metadata[ordered_row].source_row_index;
      if (source_row >= row_count || source_seen[source_row]) {
        return refuse("QOW-DIAG-WINDOW-MULTIPLE",
                      "window source-row mapping is not a bijection");
      }
      source_seen[source_row] = 1;
      const auto& ordered_values =
          window.frames.ordered_batch.rows[ordered_row].values;
      const auto& source_values = request.input_batch.rows[source_row].values;
      if (ordered_values.size() != source_values.size()) {
        return refuse("QOW-DIAG-WINDOW-MULTIPLE",
                      "window source-row width drifted");
      }
      for (std::size_t column = 0; column < source_values.size(); ++column) {
        if (!value_equal(ordered_values[column], source_values[column])) {
          return refuse("QOW-DIAG-WINDOW-MULTIPLE",
                        "window ordered row no longer matches its source identity");
        }
      }
      values_by_source[source_row] = window.values[ordered_row];
    }
    if (std::ranges::find(source_seen, 0) != source_seen.end()) {
      return refuse("QOW-DIAG-WINDOW-MULTIPLE",
                    "window source-row mapping omitted a source row");
    }
    materialized.columns.push_back(window.result_column);
    for (std::size_t source_row = 0; source_row < row_count; ++source_row) {
      materialized.rows[source_row].values.push_back(
          std::move(values_by_source[source_row]));
    }
    result.materialized_window_descriptor_ids.push_back(
        window.result_column.descriptor_id);
  }

  const auto bound_equal = [&](const auto& left, const auto& right) {
    if (left.kind != right.kind ||
        left.offset.has_value() != right.offset.has_value()) {
      return false;
    }
    return !left.offset.has_value() ||
           value_equal(*left.offset, *right.offset);
  };
  const auto frame_descriptor_equal = [&](const auto& left,
                                          const auto& right) {
    if (left.frame_descriptor_uuid != right.frame_descriptor_uuid ||
        left.frame_specified != right.frame_specified ||
        left.unit != right.unit || left.exclusion != right.exclusion ||
        left.start.has_value() != right.start.has_value() ||
        left.end.has_value() != right.end.has_value()) {
      return false;
    }
    return (!left.start.has_value() ||
            bound_equal(*left.start, *right.start)) &&
           (!left.end.has_value() || bound_equal(*left.end, *right.end));
  };
  if (request.windows.size() > 1 &&
      request.windows.size() - 1 >
          std::numeric_limits<std::size_t>::max() /
              request.windows.size()) {
    return refuse("QOW-DIAG-WINDOW-MULTIPLE",
                  "window pair comparison count overflowed");
  }
  const auto window_pair_comparison_count =
      request.windows.size() * (request.windows.size() - 1) / 2;
  if (request.maximum_pair_comparisons == 0 ||
      window_pair_comparison_count > request.maximum_pair_comparisons) {
    return refuse("QOW-DIAG-WINDOW-MULTIPLE",
                  "window sort-sharing comparison bound was exceeded");
  }
  for (std::size_t left = 0; left < request.windows.size(); ++left) {
    for (std::size_t right = left + 1; right < request.windows.size();
         ++right) {
      const auto& lhs = request.windows[left].frames;
      const auto& rhs = request.windows[right].frames;
      bool compatible_sort =
          lhs.partition_property_uuid == rhs.partition_property_uuid &&
          lhs.ordering_property_uuid == rhs.ordering_property_uuid &&
          lhs.term_binding_evidence_uuid ==
              rhs.term_binding_evidence_uuid &&
          lhs.deterministic_tie_evidence_uuid ==
              rhs.deterministic_tie_evidence_uuid &&
          lhs.row_metadata.size() == rhs.row_metadata.size();
      bool exact = compatible_sort &&
                   lhs.window_property_uuid == rhs.window_property_uuid &&
                   lhs.frame_property_binding_evidence_uuid ==
                       rhs.frame_property_binding_evidence_uuid &&
                   frame_descriptor_equal(lhs.resolved_frame,
                                          rhs.resolved_frame) &&
                   lhs.defaulted_with_order == rhs.defaulted_with_order &&
                   lhs.defaulted_without_order ==
                       rhs.defaulted_without_order &&
                   lhs.every_frame_operand_consumed ==
                       rhs.every_frame_operand_consumed &&
                   lhs.empty_state_uses_optional_bounds ==
                       rhs.empty_state_uses_optional_bounds &&
                   lhs.base_frame_constructed_before_exclusion ==
                       rhs.base_frame_constructed_before_exclusion &&
                   lhs.exactly_one_exclusion_consumed ==
                       rhs.exactly_one_exclusion_consumed &&
                   lhs.effective_frames.size() == rhs.effective_frames.size();
      for (std::size_t row = 0; row < lhs.row_metadata.size() &&
                                (compatible_sort || exact);
           ++row) {
        const auto& left_metadata = lhs.row_metadata[row];
        const auto& right_metadata = rhs.row_metadata[row];
        compatible_sort =
            compatible_sort &&
            left_metadata.source_row_index ==
                right_metadata.source_row_index &&
            left_metadata.ordered_row_index ==
                right_metadata.ordered_row_index &&
            left_metadata.partition_id == right_metadata.partition_id &&
            left_metadata.peer_group_id == right_metadata.peer_group_id &&
            left_metadata.partition_begin ==
                right_metadata.partition_begin &&
            left_metadata.partition_end_exclusive ==
                right_metadata.partition_end_exclusive &&
            left_metadata.peer_begin == right_metadata.peer_begin &&
            left_metadata.peer_end_exclusive ==
                right_metadata.peer_end_exclusive;
        if (exact) {
          const auto& left_frame = lhs.effective_frames[row];
          const auto& right_frame = rhs.effective_frames[row];
          exact = compatible_sort &&
                  left_frame.ordered_row_index ==
                      right_frame.ordered_row_index &&
                  left_frame.partition_id == right_frame.partition_id &&
                  left_frame.base_state == right_frame.base_state &&
                  left_frame.effective_state == right_frame.effective_state &&
                  left_frame.base_begin == right_frame.base_begin &&
                  left_frame.base_end_exclusive ==
                      right_frame.base_end_exclusive &&
                  left_frame.exclusion_applied ==
                      right_frame.exclusion_applied &&
                  left_frame.exclusion_operand_consumed ==
                      right_frame.exclusion_operand_consumed &&
                  left_frame.excluded_row_count ==
                      right_frame.excluded_row_count &&
                  left_frame.effective_row_indices ==
                      right_frame.effective_row_indices;
        }
      }
      if (compatible_sort) ++result.compatible_sort_pair_count;
      if (exact) ++result.shared_materialization_pair_count;
    }
  }
  result.window_pair_comparison_count = window_pair_comparison_count;

  std::vector<std::uint32_t> materialized_ids;
  materialized_ids.reserve(materialized.columns.size());
  for (const auto& column : materialized.columns) {
    materialized_ids.push_back(column.descriptor_id);
  }
  const auto materialized_validation = ValidateCanonicalDescriptorBatch(
      materialized, materialized_ids);
  if (!materialized_validation.ok) {
    return refuse("QOW-DIAG-WINDOW-MULTIPLE",
                  materialized_validation.diagnostic_code + ":" +
                      materialized_validation.detail);
  }

  result.stage_trace.push_back(CanonicalQueryEvaluationStage::from);
  if (request.composition_dag.has_value()) {
    if (request.composition_dag->abi_version != 2 ||
        !request.composition_dag->optimizer_published ||
        request.composition_dag->selected_plan_uuid !=
            first_frames.selected_plan_uuid ||
        !PhysicalMgaStatementContextEqual(
            request.composition_dag->mga_statement_context,
            first_frames.mga_statement_context) ||
        request.composition_dag->root_physical_node_id !=
            first_frames.executed_physical_node_id) {
      return refuse("QOW-DIAG-WINDOW-COMPOSITION",
                    "composition DAG does not match the executed window authority");
    }
    std::set<PhysicalNodeKind> required_kinds;
    bool aggregate_stage_present = false;
    for (const auto kind : request.required_upstream_node_kinds) {
      if ((kind != PhysicalNodeKind::kJoin &&
           kind != PhysicalNodeKind::kAggregate &&
           kind != PhysicalNodeKind::kSubquery &&
           kind != PhysicalNodeKind::kCte &&
           kind != PhysicalNodeKind::kRecursiveCte) ||
          !required_kinds.insert(kind).second) {
        return refuse("QOW-DIAG-WINDOW-COMPOSITION",
                      "composition requires unique ordinary upstream node kinds");
      }
      bool found = false;
      for (const auto& node : request.composition_dag->nodes) {
        if (node.node_kind == kind) {
          found = true;
          break;
        }
      }
      if (!found) {
        return refuse("QOW-DIAG-WINDOW-COMPOSITION",
                      "required ordinary upstream node is absent from the selected DAG");
      }
      aggregate_stage_present |= kind == PhysicalNodeKind::kAggregate;
    }
    if (aggregate_stage_present) {
      result.stage_trace.push_back(
          CanonicalQueryEvaluationStage::group_and_aggregate);
    }
    result.ordinary_physical_nodes_validated = !required_kinds.empty();
  } else if (!request.required_upstream_node_kinds.empty()) {
    return refuse("QOW-DIAG-WINDOW-COMPOSITION",
                  "ordinary composition nodes require a selected physical DAG");
  }
  result.stage_trace.push_back(CanonicalQueryEvaluationStage::window);

  result.source_row_indices.resize(row_count);
  std::iota(result.source_row_indices.begin(), result.source_row_indices.end(),
            0);
  if (request.qualify_predicate.has_value()) {
    const auto& predicate = *request.qualify_predicate;
    if (predicate.referenced_descriptor_ids.empty() ||
        predicate.values.size() != row_count ||
        predicate.result_column.descriptor_id == 0 ||
        predicate.result_column.stable_name.empty()) {
      return refuse("QOW-DIAG-WINDOW-QUALIFY",
                    "QUALIFY predicate descriptor, references, or cardinality are incomplete");
    }
    DescriptorBatch predicate_batch;
    predicate_batch.columns = {predicate.result_column};
    predicate_batch.rows.reserve(row_count);
    for (const auto& value : predicate.values) {
      predicate_batch.rows.push_back({{value}});
    }
    const auto predicate_validation = ValidateCanonicalDescriptorBatch(
        predicate_batch, {predicate.result_column.descriptor_id});
    if (!predicate_validation.ok) {
      return refuse("QOW-DIAG-WINDOW-QUALIFY",
                    "typed QUALIFY predicate refusal: " +
                        predicate_validation.detail);
    }
    std::set<std::uint32_t> qualify_references;
    bool references_window_result = false;
    for (const auto descriptor_id : predicate.referenced_descriptor_ids) {
      if (!qualify_references.insert(descriptor_id).second ||
          std::ranges::find(materialized_ids, descriptor_id) ==
              materialized_ids.end()) {
        return refuse("QOW-DIAG-WINDOW-QUALIFY",
                      "QUALIFY references a missing or duplicate descriptor");
      }
      references_window_result |=
          std::ranges::find(result.materialized_window_descriptor_ids,
                            descriptor_id) !=
          result.materialized_window_descriptor_ids.end();
    }
    if (!references_window_result) {
      return refuse("QOW-DIAG-WINDOW-QUALIFY",
                    "QUALIFY is not bound to a materialized window result");
    }
    std::set<std::string> bound_aliases;
    std::set<std::uint32_t> alias_descriptor_ids;
    for (const auto& alias : predicate.alias_bindings) {
      if (alias.alias.empty() || alias.alias.size() > 128 ||
          !bound_aliases.insert(alias.alias).second ||
          !alias_descriptor_ids.insert(alias.source_descriptor_id).second ||
          !qualify_references.contains(alias.source_descriptor_id)) {
        return refuse("QOW-DIAG-WINDOW-QUALIFY",
                      "QUALIFY alias binding is missing, duplicated, or unreferenced");
      }
      const auto column = std::ranges::find_if(
          materialized.columns, [&](const auto& candidate) {
            return candidate.descriptor_id == alias.source_descriptor_id;
          });
      if (column == materialized.columns.end() ||
          column->stable_name != alias.alias) {
        return refuse("QOW-DIAG-WINDOW-QUALIFY",
                      "QUALIFY alias does not resolve to its bound descriptor");
      }
    }
    DescriptorBatch qualified;
    qualified.columns = materialized.columns;
    std::vector<std::size_t> qualified_sources;
    for (std::size_t source_row = 0; source_row < row_count; ++source_row) {
      bool passes = false;
      std::string detail;
      api::EngineSqlTruthValue truth =
          api::EngineSqlTruthValue::unspecified;
      if (!api::QowCanonicalTruthFromTypedValueV1(
              predicate.values[source_row], &truth, &detail) ||
          !api::QowPredicateConsumerPassesV1(
              truth, api::EnginePredicateConsumer::qualify, &passes,
              &detail)) {
        return refuse("QOW-DIAG-WINDOW-QUALIFY",
                      "typed QUALIFY 3VL refusal: " + detail);
      }
      if (passes) {
        qualified.rows.push_back(materialized.rows[source_row]);
        qualified_sources.push_back(source_row);
      }
    }
    materialized = std::move(qualified);
    result.source_row_indices = std::move(qualified_sources);
    result.stage_trace.push_back(CanonicalQueryEvaluationStage::qualify);
    result.qualify_typed_predicate_consumed = true;
    result.qualify_descriptor_references_resolved = true;
    result.qualify_alias_bindings_resolved = true;
    result.qualify_alias_binding_count = predicate.alias_bindings.size();
    result.qualify_uses_true_only_3vl = true;
  }
  result.all_windows_materialized_before_qualify = true;

  if (request.projection_descriptor_ids.empty()) {
    return refuse("QOW-DIAG-WINDOW-COMPOSITION",
                  "post-window projection descriptor handles are required");
  }
  std::vector<std::size_t> projected_columns;
  std::set<std::uint32_t> projected_ids;
  for (const auto descriptor_id : request.projection_descriptor_ids) {
    if (descriptor_id == 0 || !projected_ids.insert(descriptor_id).second) {
      return refuse("QOW-DIAG-WINDOW-COMPOSITION",
                    "projection descriptor handles are missing or duplicated");
    }
    std::optional<std::size_t> resolved_column;
    for (std::size_t column = 0; column < materialized.columns.size();
         ++column) {
      if (materialized.columns[column].descriptor_id == descriptor_id) {
        resolved_column = column;
        break;
      }
    }
    if (!resolved_column.has_value()) {
      return refuse("QOW-DIAG-WINDOW-COMPOSITION",
                    "projection descriptor handle is unresolved after WINDOW/QUALIFY");
    }
    projected_columns.push_back(*resolved_column);
  }
  materialized = ProjectDescriptorBatch(materialized, projected_columns);
  result.stage_trace.push_back(CanonicalQueryEvaluationStage::projection);

  if (request.query_order_terms.empty()) {
    if (!request.query_order_tie_evidence_uuid.empty()) {
      return refuse("QOW-DIAG-WINDOW-COMPOSITION",
                    "query order evidence exists without bound order terms");
    }
  } else {
    if (!IsCanonicalUuid(request.query_order_tie_evidence_uuid) ||
        request.maximum_pair_comparisons == 0) {
      return refuse("QOW-DIAG-WINDOW-COMPOSITION",
                    "query order tie evidence or resource bound is invalid");
    }
    std::set<std::pair<std::size_t, std::uint32_t>> order_terms;
    for (const auto& term : request.query_order_terms) {
      if (term.column >= materialized.columns.size() ||
          !order_terms.emplace(term.column,
                               term.expression_descriptor_id).second) {
        return refuse("QOW-DIAG-WINDOW-COMPOSITION",
                      "query order term is outside projection or duplicated");
      }
      const auto validation = ValidateCanonicalDescriptorOrderTerm(
          term, materialized.columns[term.column]);
      if (!validation.ok) {
        return refuse("QOW-DIAG-WINDOW-COMPOSITION",
                      "query order refusal: " + validation.detail);
      }
    }
    const auto output_rows = materialized.rows.size();
    if (output_rows != 0 &&
        output_rows > std::numeric_limits<std::size_t>::max() / output_rows) {
      return refuse("QOW-DIAG-WINDOW-COMPOSITION",
                    "query order comparison matrix overflowed");
    }
    const auto matrix_size = output_rows * output_rows;
    if (matrix_size > request.maximum_pair_comparisons) {
      return refuse("QOW-DIAG-WINDOW-COMPOSITION",
                    "query order comparison resource bound was exceeded");
    }
    std::vector<std::int8_t> comparisons(matrix_size, 0);
    for (std::size_t left = 0; left < output_rows; ++left) {
      for (std::size_t right = left + 1; right < output_rows; ++right) {
        int comparison = 0;
        for (const auto& term : request.query_order_terms) {
          const auto compared = CompareCanonicalDescriptorOrderValues(
              materialized.rows[left].values[term.column],
              materialized.rows[right].values[term.column], term);
          if (!compared.diagnostic.ok) {
            return refuse("QOW-DIAG-WINDOW-COMPOSITION",
                          "query order comparison refusal: " +
                              compared.diagnostic.detail);
          }
          comparison = compared.comparison;
          if (comparison != 0) break;
        }
        comparisons[left * output_rows + right] =
            static_cast<std::int8_t>(comparison);
        comparisons[right * output_rows + left] =
            static_cast<std::int8_t>(-comparison);
      }
    }
    std::vector<std::size_t> order(output_rows);
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(),
                     [&](const std::size_t left, const std::size_t right) {
                       return comparisons[left * output_rows + right] < 0;
                     });
    DescriptorBatch ordered;
    ordered.columns = materialized.columns;
    std::vector<std::size_t> ordered_sources;
    ordered.rows.reserve(output_rows);
    ordered_sources.reserve(output_rows);
    for (const auto position : order) {
      ordered.rows.push_back(materialized.rows[position]);
      ordered_sources.push_back(result.source_row_indices[position]);
    }
    materialized = std::move(ordered);
    result.source_row_indices = std::move(ordered_sources);
    result.stage_trace.push_back(CanonicalQueryEvaluationStage::query_order);
  }
  result.projection_precedes_query_order = true;

  if (request.offset != 0 || request.row_limit.has_value()) {
    const auto available = static_cast<std::uint64_t>(materialized.rows.size());
    const auto begin = std::min(request.offset, available);
    const auto remaining = available - begin;
    const auto take = request.row_limit.has_value()
                          ? std::min(*request.row_limit, remaining)
                          : remaining;
    DescriptorBatch limited;
    limited.columns = materialized.columns;
    limited.rows.insert(
        limited.rows.end(),
        materialized.rows.begin() + static_cast<std::ptrdiff_t>(begin),
        materialized.rows.begin() +
            static_cast<std::ptrdiff_t>(begin + take));
    std::vector<std::size_t> limited_sources(
        result.source_row_indices.begin() +
            static_cast<std::ptrdiff_t>(begin),
        result.source_row_indices.begin() +
            static_cast<std::ptrdiff_t>(begin + take));
    materialized = std::move(limited);
    result.source_row_indices = std::move(limited_sources);
    result.stage_trace.push_back(
        CanonicalQueryEvaluationStage::offset_limit_fetch_top);
  }
  result.query_order_precedes_row_limit = true;

  if (materialized.rows.size() > request.maximum_output_rows) {
    return refuse("QOW-DIAG-WINDOW-COMPOSITION",
                  "post-window output row bound was exceeded");
  }
  std::vector<std::uint32_t> output_descriptor_ids;
  output_descriptor_ids.reserve(materialized.columns.size());
  for (const auto& column : materialized.columns) {
    output_descriptor_ids.push_back(column.descriptor_id);
  }
  const auto output_validation = ValidateCanonicalDescriptorBatch(
      materialized, output_descriptor_ids);
  if (!output_validation.ok) {
    return refuse("QOW-DIAG-WINDOW-COMPOSITION",
                  output_validation.diagnostic_code + ":" +
                      output_validation.detail);
  }
  const auto result_authority = RevalidateCanonicalWindowFrameAuthority(
      execution_authority, first_frames);
  if (!result_authority.ok) {
    return refuse(result_authority.diagnostic_code,
                  result_authority.detail);
  }
  if (request.composition_dag.has_value()) {
    const auto composition_result_authority =
        RevalidateCanonicalExecutionMgaAuthority(
            execution_authority, *request.composition_dag);
    if (!composition_result_authority.ok) {
      return refuse(composition_result_authority.diagnostic_code,
                    composition_result_authority.detail);
    }
  }

  result.diagnostic = {};
  result.output_batch = std::move(materialized);
  result.every_window_source_mapping_bijective = true;
  result.every_function_state_independent = true;
  result.authority = first_frames.authority;
  result.selected_plan_uuid = first_frames.selected_plan_uuid;
  result.mga_statement_context = first_frames.mga_statement_context;
  result.executed_physical_node_id =
      first_frames.executed_physical_node_id;
  result.causal_counter_id = first_frames.causal_counter_id;
  return result;
}

}  // namespace scratchbird::engine::executor
