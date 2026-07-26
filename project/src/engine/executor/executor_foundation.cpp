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
  state.transition_count = request.input_batch.rows.size();
  for (const auto& row : request.input_batch.rows) {
    const auto& value = row.values[request.value_column];
    if (value.state == EngineValueState::sql_null) continue;
    const auto decoded = DecodeInt64Value(value);
    if (!decoded.ok()) {
      return refuse(decoded.diagnostic.diagnostic_code + ":" +
                    decoded.diagnostic.detail);
    }
    if ((decoded.value > 0 &&
         state.accumulated_value >
             std::numeric_limits<std::int64_t>::max() - decoded.value) ||
        (decoded.value < 0 &&
         state.accumulated_value <
             std::numeric_limits<std::int64_t>::min() - decoded.value)) {
      return refuse("SUM int64 transition overflowed");
    }
    state.accumulated_value += decoded.value;
    ++state.non_null_count;
    state.has_value = true;
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

  EngineTypedValue value;
  value.descriptor = state.result_column.descriptor;
  if (state.has_value) {
    value.encoded_value = std::to_string(state.accumulated_value);
    value.state = EngineValueState::value;
  } else {
    value.is_null = true;
    value.state = EngineValueState::sql_null;
  }
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
  for (std::size_t pair = 0; pair < keys.pair_count; ++pair) {
    if (keys.pair_truth_values[pair] ==
        api::EngineSqlTruthValue::true_value) {
      accepted_pair_truth_values[pair] =
          request.residual_truth_values[pair];
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
  result.candidate_pair_count = candidate_pair_count;
  result.residual_recheck_count = candidate_pair_count;
  result.selected_plan_uuid = std::move(joined.selected_plan_uuid);
  result.executed_physical_node_id = joined.executed_physical_node_id;
  result.causal_counter_id = joined.causal_counter_id;
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

}  // namespace scratchbird::engine::executor
