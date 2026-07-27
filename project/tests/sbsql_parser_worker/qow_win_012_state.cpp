// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#define QOW_WIN_007_FIXTURE_ONLY
#include "qow_win_007.cpp"

namespace {

constexpr std::string_view kInt64SumUuid =
    "019de5fc-2400-72e4-8549-82b2eef5a777";
constexpr std::string_view kAverageUuid =
    "019de5fc-2400-78ac-b50c-45b832831004";

exec::CanonicalWindowAggregateRequest AggregateWindowRequest(
    exec::CanonicalWindowFrameDescriptor frame = WholePartitionFrame()) {
  exec::CanonicalWindowAggregateRequest request;
  request.frames = ValueFrames(std::move(frame));
  request.function = exec::CanonicalWindowAggregateFunction::int64_sum;
  request.function_uuid = kInt64SumUuid;
  request.value_expression_descriptor_id = 4005;
  request.result_column = request.frames.ordered_batch.columns[4];
  request.result_column.stable_name = "window_sum";
  request.result_column.descriptor_id = 5999;
  return request;
}

std::vector<std::string> AggregateTexts(
    const exec::CanonicalWindowAggregateResult& result) {
  std::vector<std::string> values;
  values.reserve(result.values.size());
  for (const auto& value : result.values) {
    values.push_back(value.state == api::EngineValueState::sql_null
                         ? "<NULL>"
                         : value.encoded_value);
  }
  return values;
}

bool AggregateRefused(
    const exec::CanonicalWindowAggregateResult& result,
    const std::initializer_list<std::string_view> codes) {
  if (result.diagnostic.ok || !result.values.empty() ||
      !result.transition_row_indices.empty()) {
    return false;
  }
  for (const auto code : codes) {
    if (result.diagnostic.diagnostic_code == code) return true;
  }
  return false;
}

exec::CanonicalRegistryWindowAggregateRequest RegistryAverageWindowRequest(
    exec::CanonicalWindowFrameDescriptor frame = WholePartitionFrame()) {
  exec::CanonicalRegistryWindowAggregateRequest request;
  request.frames = ValueFrames(std::move(frame));

  auto& aggregate = request.aggregate_template;
  aggregate.physical_dag = Window401Request().physical_dag;
  aggregate.physical_dag.nodes[1].node_kind =
      exec::PhysicalNodeKind::kAggregate;
  aggregate.physical_dag.nodes[1].implementation_id =
      "window.aggregate-registry-kernel.v1";
  aggregate.physical_dag.nodes[1].output_descriptor_ids = {5998};
  aggregate.selected_physical_node_id =
      request.frames.executed_physical_node_id;
  aggregate.descriptor = {
      1, exec::CanonicalAggregateFunction::avg, "sb.aggregate.avg",
      std::string(kAverageUuid), false};
  aggregate.value_columns = {4};
  aggregate.value_expression_descriptor_ids = {4005};
  aggregate.result_column = {
      "window_avg",
      WindowDescriptor(
          5101, "real64",
          "type_uuid=" + WindowUuid(5201) + ";nullability=nullable"),
      true, 5998};
  return request;
}

std::vector<std::string> RegistryAggregateTexts(
    const exec::CanonicalRegistryWindowAggregateResult& result) {
  std::vector<std::string> values;
  values.reserve(result.values.size());
  for (const auto& value : result.values) {
    values.push_back(value.state == api::EngineValueState::sql_null
                         ? "<NULL>"
                         : value.encoded_value);
  }
  return values;
}

bool RegistryAggregateRefused(
    const exec::CanonicalRegistryWindowAggregateResult& result,
    const std::initializer_list<std::string_view> codes) {
  if (result.diagnostic.ok || !result.values.empty() ||
      !result.frame_row_indices.empty()) {
    return false;
  }
  for (const auto code : codes) {
    if (result.diagnostic.diagnostic_code == code) return true;
  }
  return false;
}

std::vector<std::vector<std::size_t>> EffectiveFrameRows(
    const exec::CanonicalWindowFrameResult& frames) {
  std::vector<std::vector<std::size_t>> rows;
  rows.reserve(frames.effective_frames.size());
  for (const auto& frame : frames.effective_frames) {
    rows.push_back(frame.effective_row_indices);
  }
  return rows;
}

bool ValidateAggregateWindowState() {
  auto request = AggregateWindowRequest();
  auto result = exec::ExecuteCanonicalWindowAggregate(request);
  bool passed = Require401(
      result.diagnostic.ok &&
          AggregateTexts(result) ==
              std::vector<std::string>({"413", "413", "413", "413", "413",
                                        "102", "207", "207", "106"}) &&
          result.transition_count == 31 &&
          result.transition_row_indices.front() ==
              std::vector<std::size_t>({0, 1, 2, 3, 4}) &&
          result.shared_aggregate_state_authority_used &&
          result.effective_frame_recomputed &&
          result.authority.engine_mga_snapshot_bound &&
          result.selected_plan_uuid == WindowUuid(4301) &&
          result.causal_counter_id == 40102,
      "SUM window did not reuse canonical state/finalization authority");

  request = AggregateWindowRequest(PrefixFrame());
  result = exec::ExecuteCanonicalWindowAggregate(request);
  passed &= Require401(
      result.diagnostic.ok &&
          AggregateTexts(result) ==
              std::vector<std::string>({"101", "206", "206", "306", "413",
                                        "102", "103", "207", "106"}) &&
          result.transition_count == 20,
      "SUM window did not recompute the exact moving frame");
  return passed;
}

bool ValidateAggregateWindowStateRefusals() {
  bool passed = true;
  auto request = AggregateWindowRequest();
  request.function_uuid =
      "019de5fc-2400-784a-9aec-371f8b95b7ea";
  passed &= Require401(
      AggregateRefused(exec::ExecuteCanonicalWindowAggregate(request),
                       {"QOW-DIAG-WINDOW-AGGREGATE"}),
      "COUNT registry UUID selected the SUM window state");

  request = AggregateWindowRequest();
  request.result_column.nullable = false;
  passed &= Require401(
      AggregateRefused(exec::ExecuteCanonicalWindowAggregate(request),
                       {"QOW-DIAG-WINDOW-AGGREGATE"}),
      "SUM window admitted a result unable to represent empty-frame NULL");

  request = AggregateWindowRequest();
  request.maximum_transition_count = 30;
  passed &= Require401(
      AggregateRefused(exec::ExecuteCanonicalWindowAggregate(request),
                       {"QOW-DIAG-WINDOW-AGGREGATE-FRAME"}),
      "SUM window published partial output after transition exhaustion");

  request = AggregateWindowRequest();
  request.transaction_finality_claimed = true;
  passed &= Require401(
      AggregateRefused(exec::ExecuteCanonicalWindowAggregate(request),
                       {"QOW-DIAG-WINDOW-AUTHORITY"}),
      "SUM window claimed engine transaction finality");
  return passed;
}

bool ValidateRegistryAggregateWindowState() {
  auto request = RegistryAverageWindowRequest();
  auto result = exec::ExecuteCanonicalRegistryWindowAggregate(request);
  bool passed = Require401(
      result.diagnostic.ok &&
          RegistryAggregateTexts(result) ==
              std::vector<std::string>(
                  {"103.25", "103.25", "103.25", "103.25", "103.25",
                   "102", "103.5", "103.5", "106"}) &&
          result.frame_input_row_count == 31 &&
          result.frame_row_indices == EffectiveFrameRows(request.frames) &&
          result.shared_aggregate_state_authority_used &&
          result.effective_frame_recomputed &&
          result.descriptor.function == exec::CanonicalAggregateFunction::avg &&
          result.selected_plan_uuid == request.frames.selected_plan_uuid &&
          result.executed_physical_node_id ==
              request.frames.executed_physical_node_id &&
          result.causal_counter_id == request.frames.causal_counter_id &&
          result.authority.engine_mga_snapshot_bound,
      "AVG window did not execute every frame through aggregate registry state");

  request = RegistryAverageWindowRequest(PrefixFrame());
  result = exec::ExecuteCanonicalRegistryWindowAggregate(request);
  passed &= Require401(
      result.diagnostic.ok &&
          RegistryAggregateTexts(result) ==
              std::vector<std::string>({"101", "103", "103", "102",
                                        "103.25", "102", "103", "103.5",
                                        "106"}) &&
          result.frame_input_row_count == 20,
      "AVG window did not recompute the exact moving prefix frame");

  request.aggregate_template.forced_strategy =
      exec::CanonicalAggregateExecutionStrategy::partitioned_combine;
  const auto combined =
      exec::ExecuteCanonicalRegistryWindowAggregate(request);
  passed &= Require401(
      combined.diagnostic.ok &&
          RegistryAggregateTexts(combined) == RegistryAggregateTexts(result),
      "aggregate window serial and combine state finalization diverged");

  request = RegistryAverageWindowRequest();
  request.aggregate_template.filter_truth_values =
      std::vector<api::EngineSqlTruthValue>{
          api::EngineSqlTruthValue::true_value,
          api::EngineSqlTruthValue::false_value,
          api::EngineSqlTruthValue::true_value,
          api::EngineSqlTruthValue::unknown,
          api::EngineSqlTruthValue::true_value,
          api::EngineSqlTruthValue::false_value,
          api::EngineSqlTruthValue::true_value,
          api::EngineSqlTruthValue::false_value,
          api::EngineSqlTruthValue::true_value};
  request.aggregate_template.distinct = true;
  request.aggregate_template.aggregate_order_terms = {
      {.column = 4, .expression_descriptor_id = 4005}};
  result = exec::ExecuteCanonicalRegistryWindowAggregate(request);
  passed &= Require401(
      result.diagnostic.ok &&
          RegistryAggregateTexts(result) ==
              std::vector<std::string>({"104", "104", "104", "104", "104",
                                        "<NULL>", "103", "103", "106"}) &&
          result.distinct_tuple_count > 0 &&
          result.order_comparison_count > 0,
      "aggregate window did not preserve frame-local FILTER/DISTINCT/order semantics");
  return passed;
}

bool ValidateRegistryAggregateWindowRefusals() {
  bool passed = true;
  auto request = RegistryAverageWindowRequest();
  request.aggregate_template.descriptor.function_uuid =
      std::string(kInt64SumUuid);
  passed &= Require401(
      RegistryAggregateRefused(
          exec::ExecuteCanonicalRegistryWindowAggregate(request),
          {"QOW-DIAG-WINDOW-AGGREGATE-REGISTRY-DESCRIPTOR"}),
      "aggregate window accepted a registry UUID mismatch");

  request = RegistryAverageWindowRequest();
  request.aggregate_template.input_batch.columns =
      request.frames.ordered_batch.columns;
  passed &= Require401(
      RegistryAggregateRefused(
          exec::ExecuteCanonicalRegistryWindowAggregate(request),
          {"QOW-DIAG-WINDOW-AGGREGATE-REGISTRY-AUTHORITY"}),
      "aggregate template supplied shadow frame input authority");

  request = RegistryAverageWindowRequest();
  ++request.aggregate_template.physical_dag.statement_snapshot_id;
  passed &= Require401(
      RegistryAggregateRefused(
          exec::ExecuteCanonicalRegistryWindowAggregate(request),
          {"QOW-DIAG-WINDOW-AGGREGATE-REGISTRY-AUTHORITY"}),
      "aggregate window admitted a different MGA statement snapshot");

  request = RegistryAverageWindowRequest();
  request.aggregate_template.physical_dag.nodes[1].implementation_id =
      "aggregate.registry-core.v1";
  passed &= Require401(
      RegistryAggregateRefused(
          exec::ExecuteCanonicalRegistryWindowAggregate(request),
          {"QOW-DIAG-WINDOW-AGGREGATE-REGISTRY-PHYSICAL"}),
      "ordinary aggregate implementation impersonated the window kernel");

  request = RegistryAverageWindowRequest();
  request.maximum_transition_count = 30;
  passed &= Require401(
      RegistryAggregateRefused(
          exec::ExecuteCanonicalRegistryWindowAggregate(request),
          {"QOW-DIAG-WINDOW-AGGREGATE-REGISTRY-RESOURCE"}),
      "aggregate window published partial output after resource exhaustion");
  return passed;
}

}  // namespace

#ifndef QOW_WIN_012_STATE_FIXTURE_ONLY
// QOW-TEST-WIN-012-STATE-V1
int main() {
  return ValidateAggregateWindowState() &&
                 ValidateAggregateWindowStateRefusals() &&
                 ValidateRegistryAggregateWindowState() &&
                 ValidateRegistryAggregateWindowRefusals()
             ? EXIT_SUCCESS
             : EXIT_FAILURE;
}
#endif
