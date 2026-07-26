// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "executor_foundation.hpp"

#include "descriptor_value_runtime.hpp"
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

// QOW-SOURCE-QRY-009-V1
// Column positions reaching this legacy executor surface are already-bound
// projection, sort, or expression handles.  Resolve the complete handle set
// before an operator can consume any row so a ragged or out-of-range handle
// cannot be translated into successful zero data.
void RequireResolvedColumnHandles(
    const Batch& input,
    const std::vector<std::size_t>& columns) {
  if (input.rows.empty() && !columns.empty()) {
    throw std::out_of_range("SBLR.PLAN_TREE.INVALID_HANDLE");
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

// QOW-SOURCE-QRY-010-FETCH-TOP-PROFILE-V1
// The native SBSQL development profile admits only FETCH FIRST <bound count>
// ROWS ONLY.  WITH TIES and donor TOP variants stay explicit refusals rather
// than silently degrading to an ordinary limit.
CanonicalDescriptorFetchProfileResult ExecuteCanonicalDescriptorFetchProfile(
    const CanonicalDescriptorFetchProfileRequest& request) {
  CanonicalDescriptorFetchProfileResult result;
  if (request.form !=
          CanonicalFetchTopProfileForm::fetch_first_rows_only ||
      !request.row_count_is_bound) {
    result.diagnostic.ok = false;
    result.diagnostic.diagnostic_code =
        "QOW-DIAG-QRY-010-FETCH-TOP-PROFILE-REFUSAL-V1";
    result.diagnostic.detail =
        "only bound native FETCH FIRST ROWS ONLY is admitted";
    return result;
  }

  CanonicalDescriptorLimitRequest limit_request;
  limit_request.physical_dag = request.physical_dag;
  limit_request.selected_physical_node_id =
      request.selected_physical_node_id;
  limit_request.input_batch = request.input_batch;
  limit_request.limit = request.row_count;
  limit_request.offset = request.offset;
  auto limited = ExecuteCanonicalDescriptorLimit(limit_request);
  result.diagnostic = std::move(limited.diagnostic);
  result.output_batch = std::move(limited.output_batch);
  result.selected_plan_uuid = std::move(limited.selected_plan_uuid);
  result.executed_physical_node_id = limited.executed_physical_node_id;
  result.causal_counter_id = limited.causal_counter_id;
  return result;
}

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

  const auto dag_validation =
      ValidateTypedPhysicalNodeDag(request.physical_dag);
  if (!dag_validation.accepted) {
    const auto& issue = dag_validation.issues.front();
    return refuse(issue.diagnostic_id + ":" + issue.field_id);
  }
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

  result.diagnostic = {};
  result.state = std::move(state);
  result.selected_plan_uuid = request.physical_dag.selected_plan_uuid;
  result.executed_physical_node_id = selected_node->physical_node_id;
  result.causal_counter_id = selected_node->causal_counter_id;
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

  const auto dag_validation =
      ValidateTypedPhysicalNodeDag(request.physical_dag);
  if (!dag_validation.accepted) {
    const auto& issue = dag_validation.issues.front();
    return refuse(issue.diagnostic_id + ":" + issue.field_id);
  }
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

  result.diagnostic = {};
  result.selected_plan_uuid = request.physical_dag.selected_plan_uuid;
  result.executed_physical_node_id = selected_node->physical_node_id;
  result.causal_counter_id = selected_node->causal_counter_id;
  return result;
}

// QOW-SOURCE-QRY-011-GROUP-V1
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

  const auto dag_validation =
      ValidateTypedPhysicalNodeDag(request.physical_dag);
  if (!dag_validation.accepted) {
    const auto& issue = dag_validation.issues.front();
    return refuse(issue.diagnostic_id + ":" + issue.field_id);
  }
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
  if (request.key_result_column.descriptor_id !=
          selected_node->output_descriptor_ids[0] ||
      request.sum_result_column.descriptor_id !=
          selected_node->output_descriptor_ids[1] ||
      !request.key_result_column.nullable ||
      !request.sum_result_column.nullable ||
      request.key_result_column.descriptor.canonical_type_name != "int64" ||
      request.sum_result_column.descriptor.canonical_type_name != "int64") {
    return refuse("grouped SUM result descriptors are not bound nullable int64");
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
  if ((request.grouping_set_rule !=
           CanonicalInt64GroupingSetRule::key_only &&
       request.grouping_set_rule !=
           CanonicalInt64GroupingSetRule::key_and_grand_total) ||
      request.maximum_group_count == 0 ||
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

  result.diagnostic = {};
  result.groups = std::move(groups);
  result.selected_plan_uuid = request.physical_dag.selected_plan_uuid;
  result.executed_physical_node_id = selected_node->physical_node_id;
  result.causal_counter_id = selected_node->causal_counter_id;
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
  const auto dag_validation =
      ValidateTypedPhysicalNodeDag(aggregate.physical_dag);
  if (!dag_validation.accepted) {
    const auto& issue = dag_validation.issues.front();
    return refuse(issue.diagnostic_id + ":" + issue.field_id);
  }
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
  result.diagnostic = {};
  result.state = std::move(transitioned.state);
  result.selected_plan_uuid = std::move(transitioned.selected_plan_uuid);
  result.executed_physical_node_id =
      transitioned.executed_physical_node_id;
  result.causal_counter_id = transitioned.causal_counter_id;
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
  const auto dag_validation =
      ValidateTypedPhysicalNodeDag(aggregate.physical_dag);
  if (!dag_validation.accepted) {
    const auto& issue = dag_validation.issues.front();
    return refuse(issue.diagnostic_id + ":" + issue.field_id);
  }
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
  result.diagnostic = {};
  result.state = std::move(transitioned.state);
  result.distinct_value_count = observed_values.size();
  result.selected_plan_uuid = std::move(transitioned.selected_plan_uuid);
  result.executed_physical_node_id =
      transitioned.executed_physical_node_id;
  result.causal_counter_id = transitioned.causal_counter_id;
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

  auto route_request = aggregate;
  route_request.input_batch.rows.clear();
  const auto route_validation =
      ExecuteCanonicalInt64SumState(route_request);
  if (!route_validation.diagnostic.ok) {
    return refuse(route_validation.diagnostic.diagnostic_code + ":" +
                  route_validation.diagnostic.detail);
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

  result.diagnostic = {};
  result.state = std::move(transitioned.state);
  result.ordered_input_row_indices = std::move(row_order);
  result.selected_plan_uuid = std::move(transitioned.selected_plan_uuid);
  result.executed_physical_node_id =
      transitioned.executed_physical_node_id;
  result.causal_counter_id = transitioned.causal_counter_id;
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

  result.diagnostic = {};
  result.groups = std::move(canonical.groups);
  result.selected_plan_uuid = std::move(canonical.selected_plan_uuid);
  result.executed_physical_node_id =
      canonical.executed_physical_node_id;
  result.causal_counter_id = canonical.causal_counter_id;
  return result;
}

// QOW-SOURCE-QRY-012-KEY-V1
// Evaluate already-bound composite int64 equality keys for every physical row
// pair.  Terms combine through SQL AND: FALSE dominates UNKNOWN, and only all
// non-NULL equal terms produce TRUE.  Full route/input validation precedes the
// bounded comparison matrix.
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
  const auto route_validation = ExecuteCanonicalDescriptorInnerJoin(route);
  if (!route_validation.diagnostic.ok) {
    return refuse(route_validation.diagnostic.diagnostic_code + ":" +
                  route_validation.diagnostic.detail);
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
        left_column.descriptor.canonical_type_name != "int64" ||
        right_column.descriptor.canonical_type_name != "int64") {
      return refuse("composite join key is not bound compatible int64");
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
        const auto left_decoded = DecodeInt64Value(left_value);
        const auto right_decoded = DecodeInt64Value(right_value);
        if (!left_decoded.ok() || !right_decoded.ok()) {
          return refuse("composite join key operand encoding is invalid");
        }
        if (left_decoded.value != right_decoded.value) {
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

  result.diagnostic = {};
  result.pair_truth_values = std::move(pair_truth_values);
  result.pair_count = pair_count;
  result.selected_plan_uuid = route_validation.selected_plan_uuid;
  result.executed_physical_node_id =
      route_validation.executed_physical_node_id;
  result.causal_counter_id = route_validation.causal_counter_id;
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
  auto joined = ExecuteCanonicalDescriptorInnerJoin(join);
  if (!joined.diagnostic.ok) {
    return refuse(joined.diagnostic.diagnostic_code + ":" +
                  joined.diagnostic.detail);
  }

  result.diagnostic = {};
  result.output_batch = std::move(joined.output_batch);
  result.accepted_pair_indices = std::move(accepted_pair_indices);
  result.candidate_pair_count = candidate_pair_count;
  result.residual_recheck_count = candidate_pair_count;
  result.selected_plan_uuid = std::move(joined.selected_plan_uuid);
  result.executed_physical_node_id = joined.executed_physical_node_id;
  result.causal_counter_id = joined.causal_counter_id;
  return result;
}

// QOW-SOURCE-QRY-012-KIND-V1
// Produce canonical left-outer cardinality from the admitted key/residual
// route.  Accepted pair indices retain physical identity for duplicate values;
// unmatched left rows receive descriptor-preserving SQL NULL right fields.
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
    result.selected_plan_uuid.clear();
    result.executed_physical_node_id = 0;
    result.causal_counter_id = 0;
    return result;
  };

  if (request.join_kind != CanonicalAcceptedJoinKind::kLeftOuter) {
    return refuse("join kind is outside the accepted left-outer profile");
  }
  if (request.maximum_output_rows == 0) {
    return refuse("join-kind output resource contract is invalid");
  }
  auto residual =
      ExecuteCanonicalJoinResidual(request.residual_request);
  if (!residual.diagnostic.ok) {
    return refuse(residual.diagnostic.diagnostic_code + ":" +
                  residual.diagnostic.detail);
  }

  const auto& key_request = request.residual_request.key_request;
  const auto left_count = key_request.left_batch.rows.size();
  const auto right_count = key_request.right_batch.rows.size();
  if (left_count > request.maximum_output_rows ||
      residual.accepted_pair_indices.size() !=
          residual.output_batch.rows.size()) {
    return refuse("left-outer output cardinality is invalid or excessive");
  }

  std::vector<bool> matched_left_rows(left_count, false);
  std::size_t previous_pair = 0;
  bool has_previous_pair = false;
  for (const auto pair : residual.accepted_pair_indices) {
    if (right_count == 0 || pair >= left_count * right_count ||
        (has_previous_pair && pair <= previous_pair)) {
      return refuse("residual pair identity is not canonical");
    }
    matched_left_rows[pair / right_count] = true;
    previous_pair = pair;
    has_previous_pair = true;
  }
  const auto unmatched_left_row_count =
      static_cast<std::size_t>(std::count(matched_left_rows.begin(),
                                          matched_left_rows.end(), false));
  if (residual.accepted_pair_indices.size() >
      request.maximum_output_rows - unmatched_left_row_count) {
    return refuse("left-outer output row bound was exceeded");
  }

  DescriptorBatch output;
  output.columns = residual.output_batch.columns;
  const auto left_width = key_request.left_batch.columns.size();
  for (std::size_t column = left_width; column < output.columns.size();
       ++column) {
    output.columns[column].nullable = true;
  }
  output.rows.reserve(residual.accepted_pair_indices.size() +
                      unmatched_left_row_count);

  std::size_t accepted = 0;
  for (std::size_t left = 0; left < left_count; ++left) {
    bool emitted_match = false;
    while (accepted < residual.accepted_pair_indices.size() &&
           residual.accepted_pair_indices[accepted] / right_count == left) {
      output.rows.push_back(std::move(residual.output_batch.rows[accepted]));
      ++accepted;
      emitted_match = true;
    }
    if (emitted_match) continue;

    DescriptorTuple unmatched;
    unmatched.values = key_request.left_batch.rows[left].values;
    for (const auto& column : key_request.right_batch.columns) {
      api::EngineTypedValue null_value;
      null_value.descriptor = column.descriptor;
      null_value.is_null = true;
      null_value.state = api::EngineValueState::sql_null;
      unmatched.values.push_back(std::move(null_value));
    }
    output.rows.push_back(std::move(unmatched));
  }
  if (accepted != residual.accepted_pair_indices.size()) {
    return refuse("residual output did not map to its left input");
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
  result.matched_pair_count = residual.accepted_pair_indices.size();
  result.unmatched_left_row_count = unmatched_left_row_count;
  result.selected_plan_uuid = std::move(residual.selected_plan_uuid);
  result.executed_physical_node_id = residual.executed_physical_node_id;
  result.causal_counter_id = residual.causal_counter_id;
  return result;
}

// QOW-SOURCE-QRY-012-STRATEGY-V1
// Execute one admitted hash-inner strategy for a single int64 equality key and
// prove its physical-pair multiset equals the canonical key/residual route.
// Hash candidates and output remain independently bounded.
CanonicalJoinStrategyResult ExecuteCanonicalJoinStrategy(
    const CanonicalJoinStrategyRequest& request) {
  namespace api = scratchbird::engine::internal_api;
  constexpr std::string_view kStrategyId =
      "join.hash-inner.int64-equality.v1";

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
    result.candidate_probe_count = 0;
    result.canonical_multiset_proven = false;
    result.strategy_id.clear();
    result.selected_plan_uuid.clear();
    result.executed_physical_node_id = 0;
    result.causal_counter_id = 0;
    return result;
  };

  if (request.strategy !=
      CanonicalJoinStrategyKind::kHashInnerInt64Equality) {
    return refuse("join strategy is outside the accepted hash-inner profile");
  }
  if (request.maximum_hash_entries == 0 ||
      request.maximum_candidate_probes == 0 ||
      request.maximum_output_rows == 0) {
    return refuse("join strategy resource contract is invalid");
  }
  const auto& key_request = request.residual_request.key_request;
  if (key_request.key_terms.size() != 1) {
    return refuse("hash-inner strategy requires exactly one equality key");
  }
  const PhysicalNodeRecord* selected_node = nullptr;
  for (const auto& node : key_request.physical_dag.nodes) {
    if (node.physical_node_id == key_request.selected_physical_node_id) {
      selected_node = &node;
      break;
    }
  }
  if (selected_node == nullptr ||
      selected_node->implementation_id != kStrategyId) {
    return refuse("selected physical node does not name the hash strategy");
  }

  auto canonical =
      ExecuteCanonicalJoinResidual(request.residual_request);
  if (!canonical.diagnostic.ok) {
    return refuse(canonical.diagnostic.diagnostic_code + ":" +
                  canonical.diagnostic.detail);
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
    if (hash_entry_count == request.maximum_hash_entries) {
      return refuse("hash strategy entry bound was exceeded");
    }
    right_hash[decoded.value].push_back(right);
    ++hash_entry_count;
  }

  DescriptorBatch output;
  output.columns = key_request.left_batch.columns;
  output.columns.insert(output.columns.end(),
                        key_request.right_batch.columns.begin(),
                        key_request.right_batch.columns.end());
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
      DescriptorTuple joined;
      joined.values = key_request.left_batch.rows[left].values;
      joined.values.insert(joined.values.end(),
                           key_request.right_batch.rows[right].values.begin(),
                           key_request.right_batch.rows[right].values.end());
      output.rows.push_back(std::move(joined));
      strategy_pair_indices.push_back(pair);
    }
  }
  if (candidate_probe_count != canonical.candidate_pair_count) {
    return refuse("hash strategy candidate set differs from canonical keys");
  }

  auto canonical_multiset = canonical.accepted_pair_indices;
  auto strategy_multiset = strategy_pair_indices;
  std::sort(canonical_multiset.begin(), canonical_multiset.end());
  std::sort(strategy_multiset.begin(), strategy_multiset.end());
  if (canonical_multiset != strategy_multiset) {
    return refuse("hash strategy output differs from canonical pair multiset");
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
  result.canonical_pair_indices =
      std::move(canonical.accepted_pair_indices);
  result.strategy_pair_indices = std::move(strategy_pair_indices);
  result.hash_entry_count = hash_entry_count;
  result.candidate_probe_count = candidate_probe_count;
  result.canonical_multiset_proven = true;
  result.strategy_id = std::string(kStrategyId);
  result.selected_plan_uuid = std::move(canonical.selected_plan_uuid);
  result.executed_physical_node_id =
      canonical.executed_physical_node_id;
  result.causal_counter_id = canonical.causal_counter_id;
  return result;
}

// QOW-SOURCE-QRY-012-MGA-V1
// Recheck strategy candidates against engine-owned transaction-inventory and
// statement-snapshot evidence at the MGA boundary.  Visibility and security
// verdicts are consumed, never synthesized here; stale generations or an
// inexact key recheck fail closed before any row is published.
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
    result.mga_boundary_proven = false;
    result.transaction_inventory_evidence_uuid.clear();
    result.selected_plan_uuid.clear();
    result.executed_physical_node_id = 0;
    result.causal_counter_id = 0;
    return result;
  };

  const auto& physical_dag =
      request.strategy_request.residual_request.key_request.physical_dag;
  if (request.transaction_inventory_id == 0 ||
      request.inventory_local_transaction_id == 0 ||
      request.inventory_statement_snapshot_id == 0 ||
      request.inventory_local_transaction_id !=
          physical_dag.local_transaction_id ||
      request.inventory_statement_snapshot_id !=
          physical_dag.statement_snapshot_id ||
      !IsCanonicalUuid(request.transaction_inventory_evidence_uuid)) {
    return refuse("engine transaction inventory evidence is not bound");
  }
  if (request.maximum_boundary_rechecks == 0 ||
      request.candidate_evidence.size() >
          request.maximum_boundary_rechecks) {
    return refuse("MGA join boundary recheck bound was exceeded");
  }

  auto strategy =
      ExecuteCanonicalJoinStrategy(request.strategy_request);
  if (!strategy.diagnostic.ok) {
    return refuse(strategy.diagnostic.diagnostic_code + ":" +
                  strategy.diagnostic.detail);
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
  const auto& key_term = key_request.key_terms.front();
  const auto right_count = key_request.right_batch.rows.size();
  for (std::size_t index = 0; index < request.candidate_evidence.size();
       ++index) {
    const auto& evidence = request.candidate_evidence[index];
    if (evidence.pair_index != strategy.strategy_pair_indices[index] ||
        evidence.local_transaction_id !=
            request.inventory_local_transaction_id ||
        evidence.statement_snapshot_id !=
            request.inventory_statement_snapshot_id ||
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
    const auto& left_key =
        key_request.left_batch.rows[left].values[key_term.left_column];
    const auto& right_key =
        key_request.right_batch.rows[right].values[key_term.right_column];
    auto computed_key_truth = api::EngineSqlTruthValue::unknown;
    if (left_key.state != api::EngineValueState::sql_null &&
        right_key.state != api::EngineValueState::sql_null) {
      const auto left_decoded = DecodeInt64Value(left_key);
      const auto right_decoded = DecodeInt64Value(right_key);
      if (!left_decoded.ok() || !right_decoded.ok()) {
        return refuse("index candidate exact key encoding is invalid");
      }
      computed_key_truth =
          left_decoded.value == right_decoded.value
              ? api::EngineSqlTruthValue::true_value
              : api::EngineSqlTruthValue::false_value;
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
    return result;
  };

  if (request.quantifier != admitted_quantifier) {
    return refuse("set-operation quantifier and entry point differ");
  }
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

  const auto dag_validation = ValidateTypedPhysicalNodeDag(request.physical_dag);
  if (!dag_validation.accepted) {
    const auto& issue = dag_validation.issues.front();
    return refuse(issue.diagnostic_id + ":" + issue.field_id);
  }
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
      selected_node->input_physical_node_ids.size() != 2) {
    return refuse("set operation requires one selected binary physical node");
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
      request.left_batch, left_node->output_descriptor_ids);
  if (!validation.ok) {
    return refuse(validation.diagnostic_code + ":" + validation.detail);
  }
  validation = ValidateCanonicalDescriptorBatch(
      request.right_batch, right_node->output_descriptor_ids);
  if (!validation.ok) {
    return refuse(validation.diagnostic_code + ":" + validation.detail);
  }
  DescriptorBatch output_schema;
  output_schema.columns = request.result_columns;
  validation = ValidateCanonicalDescriptorBatch(
      output_schema, selected_node->output_descriptor_ids);
  if (!validation.ok) {
    return refuse(validation.diagnostic_code + ":" + validation.detail);
  }

  // QOW-SOURCE-QRY-016-ARITY-V1
  // Arity is decided from the three already-bound descriptor vectors, never
  // from the first physical row. Empty operands therefore retain the same
  // exact refusal behavior as populated operands.
  if (request.left_batch.columns.size() != request.right_batch.columns.size() ||
      request.left_batch.columns.size() != request.result_columns.size()) {
    return refuse("set-operation input and result arity differ",
                  "QOW-DIAG-QRY-016-ARITY-REFUSAL-V1");
  }

  DescriptorBatch aligned_right = request.right_batch;
  std::vector<std::size_t> right_to_result_column_indices(
      request.right_batch.columns.size());
  std::iota(right_to_result_column_indices.begin(),
            right_to_result_column_indices.end(), 0);

  // QOW-SOURCE-QRY-016-BY-NAME-V1
  // BY NAME maps only the names from already-bound output descriptor records.
  // The aligned values retain their UUID-addressed descriptors, and duplicate,
  // missing, or result-order-drifted names refuse before multiset evaluation.
  if (request.alignment == CanonicalSetOperationAlignment::kByName) {
    std::map<std::string, std::size_t> left_names;
    std::map<std::string, std::size_t> right_names;
    std::map<std::string, std::size_t> result_names;
    for (std::size_t column = 0; column < request.result_columns.size();
         ++column) {
      if (!left_names
               .emplace(request.left_batch.columns[column].stable_name, column)
               .second ||
          !right_names
               .emplace(request.right_batch.columns[column].stable_name, column)
               .second ||
          !result_names
               .emplace(request.result_columns[column].stable_name, column)
               .second) {
        return refuse("BY NAME requires unique bound column names",
                      "QOW-DIAG-QRY-016-BY-NAME-REFUSAL-V1");
      }
      if (request.left_batch.columns[column].stable_name !=
          request.result_columns[column].stable_name) {
        return refuse("BY NAME result order must follow the left operand",
                      "QOW-DIAG-QRY-016-BY-NAME-REFUSAL-V1");
      }
    }
    aligned_right.columns.clear();
    aligned_right.rows.assign(request.right_batch.rows.size(), {});
    aligned_right.columns.reserve(request.result_columns.size());
    for (auto& row : aligned_right.rows) {
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
      aligned_right.columns.push_back(
          request.right_batch.columns[right_column]);
      for (std::size_t row = 0; row < request.right_batch.rows.size(); ++row) {
        aligned_right.rows[row].values.push_back(
            request.right_batch.rows[row].values[right_column]);
      }
    }
    if (left_names.size() != right_names.size() ||
        left_names.size() != result_names.size()) {
      return refuse("BY NAME operand column sets differ",
                    "QOW-DIAG-QRY-016-BY-NAME-REFUSAL-V1");
    }
  }

  namespace dt = scratchbird::core::datatypes;
  std::vector<std::string> reconciled_type_names;
  reconciled_type_names.reserve(request.result_columns.size());

  // QOW-SOURCE-QRY-016-TYPE-V1
  // The result descriptor is the already-bound common type. Only identity and
  // core-classified lossless implicit conversions may reach set equality;
  // explicit-only, lossy, forbidden, and unknown conversions are refusals.
  for (std::size_t column = 0; column < request.result_columns.size(); ++column) {
    const auto& left = request.left_batch.columns[column];
    const auto& right = aligned_right.columns[column];
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
    reconciled_type_names.push_back(output.descriptor.canonical_type_name);
  }

  DescriptorBatch reconciled_left = request.left_batch;
  DescriptorBatch reconciled_right = aligned_right;
  std::size_t coerced_value_count = 0;
  if (reconcile_types) {
    std::string reconciliation_detail;
    const auto reconcile_batch = [&](DescriptorBatch* batch) {
      if (batch == nullptr) return false;
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
    if (!reconcile_batch(&reconciled_left) ||
        !reconcile_batch(&reconciled_right)) {
      return refuse(std::move(reconciliation_detail),
                    "QOW-DIAG-QRY-016-TYPE-REFUSAL-V1");
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
      (request.left_batch.rows.size() > request.maximum_output_row_count ||
       request.right_batch.rows.size() >
           request.maximum_output_row_count -
               request.left_batch.rows.size())) {
    return refuse("UNION ALL output resource bound was exceeded");
  }

  using RowKey = std::vector<std::string>;
  const auto typed_row_key = [&](const DescriptorTuple& row,
                                 RowKey* key,
                                 std::string* detail) {
    if (key == nullptr || detail == nullptr) return false;
    key->clear();
    key->reserve(row.values.size());
    for (const auto& value : row.values) {
      if (value.state == api::sql_null) {
        key->push_back("null");
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
      } else {
        std::string encoded = type + ":" + value.encoded_value + ":";
        encoded.append(reinterpret_cast<const char*>(value.binary_value.data()),
                       value.binary_value.size());
        key->push_back(std::move(encoded));
      }
    }
    return true;
  };
  const auto retag_row = [&](const DescriptorTuple& source) {
    DescriptorTuple output;
    output.values = source.values;
    for (std::size_t column = 0; column < output.values.size(); ++column) {
      output.values[column].descriptor = request.result_columns[column].descriptor;
    }
    return output;
  };

  std::vector<RowKey> left_keys;
  std::vector<RowKey> right_keys;
  left_keys.reserve(reconciled_left.rows.size());
  right_keys.reserve(reconciled_right.rows.size());
  std::string key_detail;
  for (const auto& row : reconciled_left.rows) {
    RowKey key;
    if (!typed_row_key(row, &key, &key_detail)) {
      return refuse(std::move(key_detail));
    }
    left_keys.push_back(std::move(key));
  }
  for (const auto& row : reconciled_right.rows) {
    RowKey key;
    if (!typed_row_key(row, &key, &key_detail)) {
      return refuse(std::move(key_detail));
    }
    right_keys.push_back(std::move(key));
  }

  std::size_t equality_comparison_count = 0;
  if (bound_collation) {
    namespace dt = scratchbird::core::datatypes;
    for (std::size_t column = 0; column < request.result_columns.size();
         ++column) {
      const auto* binding = collation_by_column[column];
      if (binding == nullptr) continue;
      std::vector<const scratchbird::engine::internal_api::EngineTypedValue*>
          representatives;
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
        representatives.push_back(&value);
        *equality_key =
            "collation:" + std::to_string(representatives.size() - 1);
        return true;
      };
      for (std::size_t row = 0; row < reconciled_left.rows.size(); ++row) {
        if (!classify(reconciled_left.rows[row].values[column],
                      &left_keys[row][column])) {
          return refuse(std::move(key_detail),
                        "QOW-DIAG-QRY-016-NULL-COLLATION-REFUSAL-V1");
        }
      }
      for (std::size_t row = 0; row < reconciled_right.rows.size(); ++row) {
        if (!classify(reconciled_right.rows[row].values[column],
                      &right_keys[row][column])) {
          return refuse(std::move(key_detail),
                        "QOW-DIAG-QRY-016-NULL-COLLATION-REFUSAL-V1");
        }
      }
    }
  }

  DescriptorBatch output;
  output.columns = request.result_columns;
  std::size_t consumed_right_multiplicity_count = 0;
  std::size_t eliminated_duplicate_row_count = 0;

  // QOW-SOURCE-QRY-016-DISTINCT-V1
  // DISTINCT uses the same fully decoded typed row keys as ALL. It validates
  // every input row before eliminating duplicates, so a duplicate position
  // cannot hide malformed transport or value state.
  if (distinct && request.operation == CanonicalSetOperationKind::kUnion) {
    std::set<RowKey> emitted;
    for (std::size_t index = 0; index < left_keys.size(); ++index) {
      if (emitted.insert(left_keys[index]).second) {
        output.rows.push_back(retag_row(reconciled_left.rows[index]));
      } else {
        ++eliminated_duplicate_row_count;
      }
    }
    for (std::size_t index = 0; index < right_keys.size(); ++index) {
      if (emitted.insert(right_keys[index]).second) {
        output.rows.push_back(retag_row(reconciled_right.rows[index]));
      } else {
        ++eliminated_duplicate_row_count;
      }
    }
  } else if (distinct) {
    std::set<RowKey> right_membership(right_keys.begin(), right_keys.end());
    std::set<RowKey> emitted;
    for (std::size_t index = 0; index < left_keys.size(); ++index) {
      const bool present_on_right =
          right_membership.contains(left_keys[index]);
      const bool candidate =
          request.operation == CanonicalSetOperationKind::kIntersect
              ? present_on_right
              : !present_on_right;
      if (!candidate) continue;
      if (emitted.insert(left_keys[index]).second) {
        output.rows.push_back(retag_row(reconciled_left.rows[index]));
      } else {
        ++eliminated_duplicate_row_count;
      }
    }
  } else if (request.operation == CanonicalSetOperationKind::kUnion) {
    output.rows.reserve(left_keys.size() + right_keys.size());
    for (const auto& row : reconciled_left.rows) {
      output.rows.push_back(retag_row(row));
    }
    for (const auto& row : reconciled_right.rows) {
      output.rows.push_back(retag_row(row));
    }
  } else {
    std::map<RowKey, std::size_t> right_multiplicity;
    for (const auto& key : right_keys) ++right_multiplicity[key];
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
      if (emit) output.rows.push_back(retag_row(reconciled_left.rows[index]));
    }
  }
  if (output.rows.size() > request.maximum_output_row_count) {
    return refuse("set-operation output resource bound was exceeded");
  }
  validation = ValidateCanonicalDescriptorBatch(
      output, selected_node->output_descriptor_ids);
  if (!validation.ok) {
    return refuse(validation.diagnostic_code + ":" + validation.detail);
  }

  result.diagnostic = {};
  result.output_batch = std::move(output);
  result.left_input_row_count = request.left_batch.rows.size();
  result.right_input_row_count = request.right_batch.rows.size();
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
  if (inner_result.causal_counter_id == 0 ||
      outer_result.causal_counter_id <= inner_result.causal_counter_id) {
    return refuse("nested physical causal order is not inner before outer");
  }

  result.diagnostic = {};
  result.output_batch = std::move(outer_result.output_batch);
  result.resolved_nesting_rule = resolved_rule;
  result.intermediate_row_count = inner_result.output_batch.rows.size();
  result.inner_physical_node_id = inner_result.executed_physical_node_id;
  result.outer_physical_node_id = outer_result.executed_physical_node_id;
  result.inner_causal_counter_id = inner_result.causal_counter_id;
  result.outer_causal_counter_id = outer_result.causal_counter_id;
  return result;
}

Batch MakeBatch(std::string descriptor_digest, std::vector<Tuple> rows) {
  return {.descriptor_digest = std::move(descriptor_digest), .rows = std::move(rows)};
}

OperatorDiagnostic ValidateBatch(const Batch& batch) {
  if (batch.descriptor_digest.empty()) return {.ok = false, .diagnostic_code = "SB_EXECUTOR_DESCRIPTOR_REQUIRED"};
  if (batch.rows.empty()) return {.ok = true, .diagnostic_code = "SB_EXECUTOR_OK"};
  const auto width = batch.rows.front().values.size();
  for (const auto& row : batch.rows) {
    if (row.values.size() != width) return {.ok = false, .diagnostic_code = "SB_EXECUTOR_ROW_WIDTH_MISMATCH"};
  }
  return {.ok = true, .diagnostic_code = "SB_EXECUTOR_OK"};
}

std::int64_t EvalAdd(std::int64_t lhs, std::int64_t rhs) { return lhs + rhs; }
std::int64_t EvalMultiply(std::int64_t lhs, std::int64_t rhs) { return lhs * rhs; }

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
  return MakeBatch(input.descriptor_digest, std::move(rows));
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
  return MakeBatch(input.descriptor_digest + ":projected", std::move(rows));
}

Batch SortByColumn(const Batch& input, std::size_t column, bool ascending) {
  RequireResolvedColumnHandles(input, {column});
  auto rows = input.rows;
  std::stable_sort(rows.begin(), rows.end(), [&](const Tuple& lhs, const Tuple& rhs) {
    const auto lv = lhs.values[column];
    const auto rv = rhs.values[column];
    return ascending ? lv < rv : lv > rv;
  });
  return MakeBatch(input.descriptor_digest, std::move(rows));
}

Batch LimitOffset(const Batch& input, std::size_t limit, std::size_t offset) {
  std::vector<Tuple> rows;
  if (offset >= input.rows.size()) return MakeBatch(input.descriptor_digest, {});
  const auto end = std::min(input.rows.size(), offset + limit);
  for (std::size_t i = offset; i < end; ++i) rows.push_back(input.rows[i]);
  return MakeBatch(input.descriptor_digest, std::move(rows));
}

Batch AggregateSumByKey(const Batch& input, std::size_t key_column, std::size_t value_column) {
  std::map<std::int64_t, std::int64_t> sums;
  for (const auto& row : input.rows) {
    if (HasColumn(row, key_column) && HasColumn(row, value_column)) sums[row.values[key_column]] += row.values[value_column];
  }
  std::vector<Tuple> rows;
  for (const auto& [key, sum] : sums) rows.push_back({.values = {key, sum}});
  return MakeBatch(input.descriptor_digest + ":aggregate", std::move(rows));
}

Batch NestedLoopJoinEqual(const Batch& left, const Batch& right, std::size_t left_column, std::size_t right_column) {
  std::vector<Tuple> rows;
  for (const auto& l : left.rows) {
    if (!HasColumn(l, left_column)) continue;
    for (const auto& r : right.rows) {
      if (HasColumn(r, right_column) && l.values[left_column] == r.values[right_column]) rows.push_back({.values = ConcatValues(l, r)});
    }
  }
  return MakeBatch(JoinDescriptor(left.descriptor_digest, right.descriptor_digest), std::move(rows));
}

Batch HashJoinEqual(const Batch& left, const Batch& right, std::size_t left_column, std::size_t right_column) {
  std::unordered_multimap<std::int64_t, const Tuple*> hash;
  for (const auto& r : right.rows) if (HasColumn(r, right_column)) hash.emplace(r.values[right_column], &r);
  std::vector<Tuple> rows;
  for (const auto& l : left.rows) {
    if (!HasColumn(l, left_column)) continue;
    const auto range = hash.equal_range(l.values[left_column]);
    for (auto it = range.first; it != range.second; ++it) rows.push_back({.values = ConcatValues(l, *it->second)});
  }
  return MakeBatch(JoinDescriptor(left.descriptor_digest, right.descriptor_digest), std::move(rows));
}

Batch MergeJoinEqual(const Batch& left_sorted, const Batch& right_sorted, std::size_t left_column, std::size_t right_column) {
  std::vector<Tuple> rows;
  std::size_t left_index = 0;
  std::size_t right_index = 0;

  while (left_index < left_sorted.rows.size() &&
         right_index < right_sorted.rows.size()) {
    const auto& left = left_sorted.rows[left_index];
    const auto& right = right_sorted.rows[right_index];
    if (!HasColumn(left, left_column)) {
      ++left_index;
      continue;
    }
    if (!HasColumn(right, right_column)) {
      ++right_index;
      continue;
    }

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
           HasColumn(left_sorted.rows[left_index], left_column) &&
           left_sorted.rows[left_index].values[left_column] == match_key) {
      ++left_index;
    }
    const auto left_end = left_index;

    const auto right_begin = right_index;
    while (right_index < right_sorted.rows.size() &&
           HasColumn(right_sorted.rows[right_index], right_column) &&
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
                   std::move(rows));
}

Batch AddRowNumberWindow(const Batch& input, std::size_t order_column) {
  auto sorted = SortByColumn(input, order_column, true);
  for (std::size_t i = 0; i < sorted.rows.size(); ++i) sorted.rows[i].values.push_back(static_cast<std::int64_t>(i + 1));
  sorted.descriptor_digest += ":row_number";
  return sorted;
}

Batch AddRankWindow(const Batch& input, std::size_t order_column) {
  auto sorted = SortByColumn(input, order_column, true);
  std::int64_t current_rank = 1;
  std::int64_t previous_value = 0;
  bool have_previous = false;
  for (std::size_t i = 0; i < sorted.rows.size(); ++i) {
    const std::int64_t value = HasColumn(sorted.rows[i], order_column) ? sorted.rows[i].values[order_column] : 0;
    if (!have_previous || value != previous_value) {
      current_rank = static_cast<std::int64_t>(i + 1);
      previous_value = value;
      have_previous = true;
    }
    sorted.rows[i].values.push_back(current_rank);
  }
  sorted.descriptor_digest += ":rank";
  return sorted;
}

Batch AddDenseRankWindow(const Batch& input, std::size_t order_column) {
  auto sorted = SortByColumn(input, order_column, true);
  std::int64_t current_dense_rank = 0;
  std::int64_t previous_value = 0;
  bool have_previous = false;
  for (auto& row : sorted.rows) {
    const std::int64_t value = HasColumn(row, order_column) ? row.values[order_column] : 0;
    if (!have_previous || value != previous_value) {
      ++current_dense_rank;
      previous_value = value;
      have_previous = true;
    }
    row.values.push_back(current_dense_rank);
  }
  sorted.descriptor_digest += ":dense_rank";
  return sorted;
}

Batch AddPartitionCountWindow(const Batch& input, std::size_t partition_column) {
  std::unordered_map<std::int64_t, std::int64_t> partition_counts;
  for (const auto& row : input.rows) {
    const std::int64_t key = HasColumn(row, partition_column) ? row.values[partition_column] : 0;
    ++partition_counts[key];
  }
  auto out = input;
  for (auto& row : out.rows) {
    const std::int64_t key = HasColumn(row, partition_column) ? row.values[partition_column] : 0;
    row.values.push_back(partition_counts[key]);
  }
  out.descriptor_digest += ":partition_count";
  return out;
}

Batch AddNtileWindow(const Batch& input, std::size_t order_column, std::int64_t bucket_count) {
  auto sorted = SortByColumn(input, order_column, true);
  if (bucket_count <= 0 || sorted.rows.empty()) {
    sorted.descriptor_digest += ":ntile";
    return sorted;
  }
  const std::uint64_t buckets = static_cast<std::uint64_t>(bucket_count);
  const std::uint64_t row_count = static_cast<std::uint64_t>(sorted.rows.size());
  for (std::uint64_t i = 0; i < row_count; ++i) {
    const auto bucket = static_cast<std::int64_t>((i * buckets) / row_count + 1);
    sorted.rows[static_cast<std::size_t>(i)].values.push_back(bucket);
  }
  sorted.descriptor_digest += ":ntile";
  return sorted;
}

Batch AddLagWindow(const Batch& input, std::size_t order_column, std::size_t value_column) {
  auto sorted = SortByColumn(input, order_column, true);
  for (std::size_t i = 0; i < sorted.rows.size(); ++i) {
    const std::int64_t value =
        i == 0 || !HasColumn(sorted.rows[i - 1], value_column)
            ? 0
            : sorted.rows[i - 1].values[value_column];
    sorted.rows[i].values.push_back(value);
  }
  sorted.descriptor_digest += ":lag";
  return sorted;
}

Batch AddLeadWindow(const Batch& input, std::size_t order_column, std::size_t value_column) {
  auto sorted = SortByColumn(input, order_column, true);
  for (std::size_t i = 0; i < sorted.rows.size(); ++i) {
    const std::int64_t value =
        i + 1 >= sorted.rows.size() || !HasColumn(sorted.rows[i + 1], value_column)
            ? 0
            : sorted.rows[i + 1].values[value_column];
    sorted.rows[i].values.push_back(value);
  }
  sorted.descriptor_digest += ":lead";
  return sorted;
}

Batch AddFirstValueWindow(const Batch& input, std::size_t order_column, std::size_t value_column) {
  auto sorted = SortByColumn(input, order_column, true);
  const std::int64_t value =
      sorted.rows.empty() || !HasColumn(sorted.rows.front(), value_column)
          ? 0
          : sorted.rows.front().values[value_column];
  for (auto& row : sorted.rows) row.values.push_back(value);
  sorted.descriptor_digest += ":first_value";
  return sorted;
}

Batch AddLastValueWindow(const Batch& input, std::size_t order_column, std::size_t value_column) {
  auto sorted = SortByColumn(input, order_column, true);
  const std::int64_t value =
      sorted.rows.empty() || !HasColumn(sorted.rows.back(), value_column)
          ? 0
          : sorted.rows.back().values[value_column];
  for (auto& row : sorted.rows) row.values.push_back(value);
  sorted.descriptor_digest += ":last_value";
  return sorted;
}

Batch MaterializeCte(const Batch& input) { return MakeBatch(input.descriptor_digest + ":materialized", input.rows); }

std::int64_t ScalarSubqueryFirstValue(const Batch& input, std::size_t column) {
  if (input.rows.empty() || !HasColumn(input.rows.front(), column)) return 0;
  return input.rows.front().values[column];
}

Batch SetUnionDistinct(const Batch& left, const Batch& right) {
  std::set<std::vector<std::int64_t>> seen;
  std::vector<Tuple> rows;
  for (const auto& row : left.rows) if (seen.insert(row.values).second) rows.push_back(row);
  for (const auto& row : right.rows) if (seen.insert(row.values).second) rows.push_back(row);
  return MakeBatch(left.descriptor_digest, std::move(rows));
}

Batch SetIntersectDistinct(const Batch& left, const Batch& right) {
  std::set<std::vector<std::int64_t>> right_values;
  for (const auto& row : right.rows) right_values.insert(row.values);
  std::set<std::vector<std::int64_t>> emitted;
  std::vector<Tuple> rows;
  for (const auto& row : left.rows) if (right_values.contains(row.values) && emitted.insert(row.values).second) rows.push_back(row);
  return MakeBatch(left.descriptor_digest, std::move(rows));
}

Batch SetExceptDistinct(const Batch& left, const Batch& right) {
  std::set<std::vector<std::int64_t>> right_values;
  for (const auto& row : right.rows) right_values.insert(row.values);
  std::set<std::vector<std::int64_t>> emitted;
  std::vector<Tuple> rows;
  for (const auto& row : left.rows) if (!right_values.contains(row.values) && emitted.insert(row.values).second) rows.push_back(row);
  return MakeBatch(left.descriptor_digest, std::move(rows));
}

Batch SetUnionAll(const Batch& left, const Batch& right) {
  std::vector<Tuple> rows = left.rows;
  rows.insert(rows.end(), right.rows.begin(), right.rows.end());
  return MakeBatch(left.descriptor_digest, std::move(rows));
}

Batch SetIntersectAll(const Batch& left, const Batch& right) {
  std::map<std::vector<std::int64_t>, std::size_t> right_counts;
  for (const auto& row : right.rows) ++right_counts[row.values];
  std::vector<Tuple> rows;
  for (const auto& row : left.rows) {
    auto found = right_counts.find(row.values);
    if (found == right_counts.end() || found->second == 0) continue;
    --found->second;
    rows.push_back(row);
  }
  return MakeBatch(left.descriptor_digest, std::move(rows));
}

Batch SetExceptAll(const Batch& left, const Batch& right) {
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
  return MakeBatch(left.descriptor_digest, std::move(rows));
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

bool CanonicalWindowFrameEvidenceValid(
    const CanonicalWindowFrameResult& frames) {
  const auto row_count = frames.ordered_batch.rows.size();
  std::vector<std::uint32_t> descriptor_ids;
  descriptor_ids.reserve(frames.ordered_batch.columns.size());
  for (const auto& column : frames.ordered_batch.columns) {
    descriptor_ids.push_back(column.descriptor_id);
  }
  const auto batch_diagnostic = ValidateCanonicalDescriptorBatch(
      frames.ordered_batch, descriptor_ids);
  if (!frames.diagnostic.ok || !frames.every_frame_operand_consumed ||
      !frames.empty_state_uses_optional_bounds ||
      !frames.authority.engine_mga_snapshot_bound ||
      !batch_diagnostic.ok ||
      !IsCanonicalUuid(frames.resolved_frame.frame_descriptor_uuid) ||
      !IsCanonicalUuid(frames.window_property_uuid) ||
      !IsCanonicalUuid(frames.selected_plan_uuid) ||
      !frames.resolved_frame.start.has_value() ||
      !frames.resolved_frame.end.has_value() ||
      !RankingFrameUnitValid(frames.resolved_frame.unit) ||
      !RankingFrameExclusionValid(frames.resolved_frame.exclusion) ||
      !RankingFrameBoundValid(*frames.resolved_frame.start) ||
      !RankingFrameBoundValid(*frames.resolved_frame.end) ||
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
      if (frame.effective_row_indices != expected) return false;
    } else if ((frame.base_state != CanonicalWindowFrameState::empty &&
                frame.base_state !=
                    CanonicalWindowFrameState::reversed_to_empty) ||
               frame.base_begin.has_value() ||
               frame.base_end_exclusive.has_value() ||
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

// QOW-SOURCE-WIN-006-V1
// Ranking functions consume the exact partition and typed peer ranges created
// by QOW-401 after QOW-402 has validated the complete frame and exclusion.
// These six functions intentionally ignore effective-frame extent only after
// that validation, as required by WINDOW_CORE.
CanonicalWindowRankingResult ExecuteCanonicalWindowRanking(
    const CanonicalWindowRankingRequest& request) {
  CanonicalWindowRankingResult result;
  const auto refuse = [&](std::string detail) {
    result = {};
    result.diagnostic =
        WindowRankingRefusal(request.function, std::move(detail));
    return result;
  };
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
  if (!IsCanonicalUuid(request.output_descriptor.descriptor_uuid.canonical) ||
      request.output_descriptor.descriptor_kind != "scalar" ||
      request.output_descriptor.encoded_descriptor.empty()) {
    return refuse("ranking output descriptor is missing or malformed");
  }
  namespace dt = scratchbird::core::datatypes;
  const auto output_type = dt::CanonicalTypeIdFromStableName(
      request.output_descriptor.canonical_type_name);
  if ((RankingIntegerResult(request.function) &&
       output_type != dt::CanonicalTypeId::int64) ||
      (!RankingIntegerResult(request.function) &&
       !RankingRealType(output_type))) {
    return refuse("ranking output descriptor has the wrong numeric family");
  }

  std::uint64_t buckets = 0;
  if (request.function == CanonicalWindowRankingFunction::ntile) {
    if (!request.ntile_bucket_count.has_value()) {
      return refuse("NTILE requires an explicitly present bucket count");
    }
    const auto decoded = DecodeInt64Value(*request.ntile_bucket_count);
    if (!decoded.ok() ||
        request.ntile_bucket_count->state !=
            scratchbird::engine::internal_api::EngineValueState::value ||
        !request.ntile_bucket_count->binary_value.empty() ||
        decoded.value <= 0) {
      return refuse("NTILE bucket count must be a positive non-NULL int64");
    }
    buckets = static_cast<std::uint64_t>(decoded.value);
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
        result.values.push_back(
            RankingInt64Value(request.output_descriptor, rank));
        break;
      case CanonicalWindowRankingFunction::dense_rank:
        result.values.push_back(
            RankingInt64Value(request.output_descriptor, dense_rank));
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
        const auto base_size = partition_rows / buckets;
        const auto larger_bucket_count = partition_rows % buckets;
        const auto larger_bucket_size = base_size + 1;
        const auto larger_rows = larger_bucket_count * larger_bucket_size;
        std::uint64_t bucket = 0;
        if (partition_position < larger_rows) {
          bucket = partition_position / larger_bucket_size + 1;
        } else {
          bucket = larger_bucket_count +
                   (partition_position - larger_rows) / base_size + 1;
        }
        result.values.push_back(
            RankingInt64Value(request.output_descriptor, bucket));
        break;
      }
    }
  }

  result.diagnostic = {};
  result.function = request.function;
  result.frame_and_exclusion_validated_then_ignored = true;
  result.authority = request.frames.authority;
  result.window_property_uuid = request.frames.window_property_uuid;
  result.selected_plan_uuid = request.frames.selected_plan_uuid;
  result.executed_physical_node_id =
      request.frames.executed_physical_node_id;
  result.causal_counter_id = request.frames.causal_counter_id;
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

bool WindowValueResultColumnValid(
    const ExecutorColumnDescriptor& result_column,
    const ExecutorColumnDescriptor& value_column) {
  return result_column.descriptor_id != 0 &&
         !result_column.stable_name.empty() && result_column.nullable &&
         IsCanonicalUuid(result_column.descriptor.descriptor_uuid.canonical) &&
         result_column.descriptor.descriptor_kind == "scalar" &&
         !result_column.descriptor.canonical_type_name.empty() &&
         !result_column.descriptor.encoded_descriptor.empty() &&
         SameWindowValueDescriptor(result_column.descriptor,
                                   value_column.descriptor);
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
       source.state != api::EngineValueState::sql_null) ||
      !source.binary_value.empty()) {
    return fail("window value is not a canonical scalar assignment operand");
  }
  if ((source.state == api::EngineValueState::sql_null &&
       (!source.is_null || !source.encoded_value.empty())) ||
      (source.state == api::EngineValueState::value && source.is_null)) {
    return fail("window value carries contradictory SQL NULL state");
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
      source.descriptor.encoded_descriptor != target.encoded_descriptor) {
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
      !value.binary_value.empty()) {
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
CanonicalWindowValueResult ExecuteCanonicalWindowValue(
    const CanonicalWindowValueRequest& request) {
  namespace api = scratchbird::engine::internal_api;
  CanonicalWindowValueResult result;
  const auto refuse = [&](std::string code, std::string detail) {
    result = {};
    result.diagnostic =
        WindowValueRefusal(std::move(code), std::move(detail));
    return result;
  };
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
        if (!CanonicalWindowInt64Operand(value, &decoded) || decoded < 0) {
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
      if (!CanonicalWindowInt64Operand(value, &decoded) || decoded <= 0) {
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

  result.diagnostic = {};
  result.function = request.function;
  result.frame_and_exclusion_validated = true;
  result.frame_and_exclusion_ignored_for_navigation = navigation;
  result.authority = request.frames.authority;
  result.window_property_uuid = request.frames.window_property_uuid;
  result.selected_plan_uuid = request.frames.selected_plan_uuid;
  result.executed_physical_node_id =
      request.frames.executed_physical_node_id;
  result.causal_counter_id = request.frames.causal_counter_id;
  return result;
}

// QOW-SOURCE-WIN-012-STATE-V1
// QOW-SOURCE-WIN-012-FILTER-V1
// QOW-SOURCE-WIN-012-DISTINCT-V1
// QOW-SOURCE-WIN-012-ORDER-V1
// QOW-SOURCE-WIN-012-FRAME-V1
// Aggregate windows consume the exact QOW-402 effective-row vectors and the
// same int64 SUM transition/finalization helpers as QOW-205.  Each frame is
// recomputed deliberately: inverse transition remains an optional optimizer
// improvement and cannot change values or diagnostics.  FILTER, DISTINCT,
// and aggregate argument ordering are applied before transition in that
// order, independently of the ordering that constructed the window frame.
CanonicalWindowAggregateResult ExecuteCanonicalWindowAggregate(
    const CanonicalWindowAggregateRequest& request) {
  namespace api = scratchbird::engine::internal_api;
  constexpr std::string_view kInt64SumUuid =
      "019de5fc-2400-72e4-8549-82b2eef5a777";

  CanonicalWindowAggregateResult result;
  const auto refuse = [&](std::string code, std::string detail) {
    result = {};
    result.diagnostic.ok = false;
    result.diagnostic.diagnostic_code = std::move(code);
    result.diagnostic.detail = std::move(detail);
    return result;
  };
  if (request.function != CanonicalWindowAggregateFunction::int64_sum ||
      request.function_uuid != kInt64SumUuid ||
      !IsCanonicalUuid(request.function_uuid)) {
    return refuse("QOW-DIAG-WINDOW-AGGREGATE",
                  "aggregate-window function kind and registry UUID do not match");
  }
  if (!CanonicalWindowFrameEvidenceValid(request.frames)) {
    return refuse("QOW-DIAG-WINDOW-AGGREGATE-FRAME",
                  "aggregate-window input is not canonical QOW-402 frame evidence");
  }
  if (request.parser_execution_authority_claimed ||
      request.transaction_finality_claimed ||
      request.recovery_authority_claimed) {
    return refuse("QOW-DIAG-WINDOW-AUTHORITY",
                  "aggregate window attempted to claim engine MGA authority");
  }

  const auto row_count = request.frames.ordered_batch.rows.size();
  if (request.maximum_output_rows == 0 ||
      row_count > request.maximum_output_rows ||
      request.maximum_transition_count == 0) {
    return refuse("QOW-DIAG-WINDOW-AGGREGATE-FRAME",
                  "aggregate-window output or transition resource bound was exceeded");
  }
  std::optional<std::size_t> value_column_index;
  for (std::size_t column = 0;
       column < request.frames.ordered_batch.columns.size(); ++column) {
    if (request.frames.ordered_batch.columns[column].descriptor_id ==
        request.value_expression_descriptor_id) {
      if (value_column_index.has_value()) {
        return refuse("QOW-DIAG-WINDOW-AGGREGATE",
                      "aggregate-window value descriptor handle is ambiguous");
      }
      value_column_index = column;
    }
  }
  if (!value_column_index.has_value()) {
    return refuse("QOW-DIAG-WINDOW-AGGREGATE",
                  "aggregate-window value descriptor handle is unresolved");
  }
  const auto& value_column =
      request.frames.ordered_batch.columns[*value_column_index];
  if (value_column.descriptor.canonical_type_name != "int64" ||
      request.result_column.descriptor_id == 0 ||
      request.result_column.stable_name.empty() ||
      !request.result_column.nullable ||
      !IsCanonicalUuid(
          request.result_column.descriptor.descriptor_uuid.canonical) ||
      request.result_column.descriptor.descriptor_kind != "scalar" ||
      request.result_column.descriptor.canonical_type_name != "int64" ||
      request.result_column.descriptor.encoded_descriptor.empty()) {
    return refuse("QOW-DIAG-WINDOW-AGGREGATE",
                  "SUM window value or result is not a bound nullable int64 descriptor");
  }
  DescriptorBatch result_schema;
  result_schema.columns = {request.result_column};
  const auto result_schema_validation = ValidateCanonicalDescriptorBatch(
      result_schema, {request.result_column.descriptor_id});
  if (!result_schema_validation.ok) {
    return refuse("QOW-DIAG-WINDOW-AGGREGATE",
                  result_schema_validation.diagnostic_code + ":" +
                      result_schema_validation.detail);
  }

  std::vector<std::optional<std::int64_t>> decoded_values;
  decoded_values.reserve(row_count);
  for (const auto& row : request.frames.ordered_batch.rows) {
    const auto& value = row.values[*value_column_index];
    if (value.state == api::EngineValueState::sql_null) {
      decoded_values.push_back(std::nullopt);
      continue;
    }
    const auto decoded = DecodeInt64Value(value);
    if (!decoded.ok()) {
      return refuse("QOW-DIAG-WINDOW-AGGREGATE",
                    decoded.diagnostic.diagnostic_code + ":" +
                        decoded.diagnostic.detail);
    }
    decoded_values.push_back(decoded.value);
  }

  std::vector<std::uint8_t> filter_passes(row_count, 1);
  if (request.filter_truth_values.has_value()) {
    if (request.filter_truth_values->size() != row_count) {
      return refuse("QOW-DIAG-WINDOW-AGGREGATE-FILTER",
                    "aggregate FILTER cardinality does not match window rows");
    }
    for (std::size_t row = 0; row < row_count; ++row) {
      std::string detail;
      bool passes = false;
      if (!api::QowPredicateConsumerPassesV1(
              (*request.filter_truth_values)[row],
              api::EnginePredicateConsumer::filter, &passes, &detail)) {
        return refuse("QOW-DIAG-WINDOW-AGGREGATE-FILTER",
                      "aggregate FILTER 3VL refusal: " + detail);
      }
      filter_passes[row] = passes ? 1 : 0;
    }
  }

  if (request.distinct && request.maximum_distinct_value_count == 0) {
    return refuse("QOW-DIAG-WINDOW-AGGREGATE-DISTINCT",
                  "aggregate-window DISTINCT resource bound is zero");
  }
  if (request.aggregate_order_terms.empty()) {
    if (!request.deterministic_tie_evidence_uuid.empty()) {
      return refuse("QOW-DIAG-WINDOW-AGGREGATE-ORDER",
                    "unordered aggregate window carries ordering evidence");
    }
  } else {
    if (request.aggregate_order_terms.size() > 64 ||
        !IsCanonicalUuid(request.deterministic_tie_evidence_uuid) ||
        request.maximum_pair_comparisons == 0) {
      return refuse("QOW-DIAG-WINDOW-AGGREGATE-ORDER",
                    "aggregate argument order resource or tie evidence is invalid");
    }
    std::set<std::pair<std::size_t, std::uint32_t>> seen_terms;
    for (const auto& term : request.aggregate_order_terms) {
      if (term.column >= request.frames.ordered_batch.columns.size()) {
        return refuse("QOW-DIAG-WINDOW-AGGREGATE-ORDER",
                      "aggregate argument order column is outside the schema");
      }
      const auto term_validation = ValidateCanonicalDescriptorOrderTerm(
          term, request.frames.ordered_batch.columns[term.column]);
      if (!term_validation.ok ||
          !seen_terms.emplace(term.column, term.expression_descriptor_id)
               .second) {
        return refuse("QOW-DIAG-WINDOW-AGGREGATE-ORDER",
                      term_validation.ok
                          ? "aggregate argument order term is duplicated"
                          : term_validation.diagnostic_code + ":" +
                                term_validation.detail);
      }
    }
  }

  result.values.reserve(row_count);
  result.transition_row_indices.reserve(row_count);
  for (std::size_t output_row = 0; output_row < row_count; ++output_row) {
    std::vector<std::size_t> transition_rows;
    for (const auto member :
         request.frames.effective_frames[output_row].effective_row_indices) {
      if (filter_passes[member]) transition_rows.push_back(member);
    }

    std::set<std::int64_t> distinct_values;
    if (request.distinct) {
      std::vector<std::size_t> distinct_rows;
      distinct_rows.reserve(transition_rows.size());
      for (const auto member : transition_rows) {
        if (!decoded_values[member].has_value()) continue;
        if (distinct_values.contains(*decoded_values[member])) continue;
        if (distinct_values.size() >=
            request.maximum_distinct_value_count) {
          return refuse("QOW-DIAG-WINDOW-AGGREGATE-DISTINCT",
                        "aggregate-window DISTINCT resource bound was exceeded");
        }
        distinct_values.insert(*decoded_values[member]);
        distinct_rows.push_back(member);
      }
      transition_rows = std::move(distinct_rows);
      if (result.distinct_value_count >
          std::numeric_limits<std::size_t>::max() - distinct_values.size()) {
        return refuse("QOW-DIAG-WINDOW-AGGREGATE-DISTINCT",
                      "aggregate-window DISTINCT evidence count overflowed");
      }
      result.distinct_value_count += distinct_values.size();
    }

    if (!request.aggregate_order_terms.empty()) {
      const auto candidate_count = transition_rows.size();
      if (candidate_count != 0 &&
          candidate_count >
              std::numeric_limits<std::size_t>::max() / candidate_count) {
        return refuse("QOW-DIAG-WINDOW-AGGREGATE-ORDER",
                      "aggregate argument order comparison matrix overflowed");
      }
      const auto matrix_size = candidate_count * candidate_count;
      if (matrix_size > request.maximum_pair_comparisons ||
          result.pair_comparison_count >
              request.maximum_pair_comparisons - matrix_size) {
        return refuse("QOW-DIAG-WINDOW-AGGREGATE-ORDER",
                      "aggregate argument order comparison bound was exceeded");
      }
      std::vector<std::int8_t> comparisons(matrix_size, 0);
      for (std::size_t left = 0; left < candidate_count; ++left) {
        for (std::size_t right = left + 1; right < candidate_count; ++right) {
          int comparison = 0;
          for (const auto& term : request.aggregate_order_terms) {
            const auto compared = CompareCanonicalDescriptorOrderValues(
                request.frames.ordered_batch.rows[transition_rows[left]]
                    .values[term.column],
                request.frames.ordered_batch.rows[transition_rows[right]]
                    .values[term.column],
                term);
            if (!compared.diagnostic.ok) {
              return refuse("QOW-DIAG-WINDOW-AGGREGATE-ORDER",
                            compared.diagnostic.diagnostic_code + ":" +
                                compared.diagnostic.detail);
            }
            comparison = compared.comparison;
            if (comparison != 0) break;
          }
          comparisons[left * candidate_count + right] =
              static_cast<std::int8_t>(comparison);
          comparisons[right * candidate_count + left] =
              static_cast<std::int8_t>(-comparison);
        }
      }
      std::vector<std::size_t> order(candidate_count);
      std::iota(order.begin(), order.end(), 0);
      std::stable_sort(order.begin(), order.end(),
                       [&](const std::size_t left, const std::size_t right) {
                         return comparisons[left * candidate_count + right] <
                                0;
                       });
      std::vector<std::size_t> ordered_rows;
      ordered_rows.reserve(candidate_count);
      for (const auto position : order) {
        ordered_rows.push_back(transition_rows[position]);
      }
      transition_rows = std::move(ordered_rows);
      result.pair_comparison_count += matrix_size;
    }

    if (transition_rows.size() > request.maximum_transition_count ||
        result.transition_count >
            request.maximum_transition_count - transition_rows.size()) {
      return refuse("QOW-DIAG-WINDOW-AGGREGATE-FRAME",
                    "aggregate-window transition resource bound was exceeded");
    }
    CanonicalInt64SumAggregateState state;
    state.value_expression_descriptor_id =
        request.value_expression_descriptor_id;
    state.result_column = request.result_column;
    for (const auto member : transition_rows) {
      std::string detail;
      if (!TransitionCanonicalInt64SumValue(
              &state,
              request.frames.ordered_batch.rows[member]
                  .values[*value_column_index],
              &detail)) {
        return refuse("QOW-DIAG-WINDOW-AGGREGATE", std::move(detail));
      }
    }
    result.transition_count += transition_rows.size();
    result.transition_row_indices.push_back(std::move(transition_rows));
    result.values.push_back(FinalizeCanonicalInt64SumValue(state));
  }

  DescriptorBatch output_validation_batch;
  output_validation_batch.columns = {request.result_column};
  output_validation_batch.rows.reserve(result.values.size());
  for (const auto& value : result.values) {
    output_validation_batch.rows.push_back({{value}});
  }
  const auto output_validation = ValidateCanonicalDescriptorBatch(
      output_validation_batch, {request.result_column.descriptor_id});
  if (!output_validation.ok) {
    return refuse("QOW-DIAG-WINDOW-AGGREGATE",
                  output_validation.diagnostic_code + ":" +
                      output_validation.detail);
  }

  result.diagnostic = {};
  result.function = request.function;
  result.filter_applied_before_transition =
      request.filter_truth_values.has_value();
  result.distinct_applied_before_transition = request.distinct;
  result.aggregate_order_independent_of_window_order =
      !request.aggregate_order_terms.empty();
  result.effective_frame_recomputed = true;
  result.shared_aggregate_state_authority_used = true;
  result.authority = request.frames.authority;
  result.window_property_uuid = request.frames.window_property_uuid;
  result.selected_plan_uuid = request.frames.selected_plan_uuid;
  result.executed_physical_node_id =
      request.frames.executed_physical_node_id;
  result.causal_counter_id = request.frames.causal_counter_id;
  return result;
}

}  // namespace scratchbird::engine::executor
