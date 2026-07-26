// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#define QOW_WIN_015_MULTIPLE_FIXTURE_ONLY
#include "qow_win_015_multiple.cpp"

namespace {

exec::CanonicalWindowPartitionOrderRequest CompositionPartitionRequest() {
  auto request = Window401Request();
  const auto payload_descriptor = WindowDescriptor(
      4105, "int64",
      "type_uuid=" + WindowUuid(4205) + ";nullability=nullable");
  request.input_batch.columns[4].descriptor = payload_descriptor;
  request.input_batch.columns[4].nullable = true;
  for (auto& row : request.input_batch.rows) {
    if (row.values[4].encoded_value == "108") {
      row.values[4] = WindowNull(payload_descriptor);
    } else {
      row.values[4].descriptor = payload_descriptor;
    }
  }

  auto values_left = request.physical_dag.nodes[0];
  auto values_right = values_left;
  values_right.physical_node_id = 3;
  values_right.relational_node_id = 3;
  values_right.causal_counter_id = 40103;
  values_right.selected_alternative_uuid = WindowUuid(4603);
  values_right.executor_capability_uuid = WindowUuid(4703);
  values_right.cost_vector_uuid = WindowUuid(4803);

  auto join = values_left;
  join.physical_node_id = 4;
  join.relational_node_id = 4;
  join.node_kind = exec::PhysicalNodeKind::kJoin;
  join.implementation_id = "join.hash.v1";
  join.input_physical_node_ids = {1, 3};
  join.causal_counter_id = 40104;
  join.selected_alternative_uuid = WindowUuid(4604);
  join.executor_capability_uuid = WindowUuid(4704);
  join.cost_vector_uuid = WindowUuid(4804);

  auto aggregate = values_left;
  aggregate.physical_node_id = 5;
  aggregate.relational_node_id = 5;
  aggregate.node_kind = exec::PhysicalNodeKind::kAggregate;
  aggregate.implementation_id = "aggregate.hash.v1";
  aggregate.input_physical_node_ids = {4};
  aggregate.causal_counter_id = 40105;
  aggregate.selected_alternative_uuid = WindowUuid(4605);
  aggregate.executor_capability_uuid = WindowUuid(4705);
  aggregate.cost_vector_uuid = WindowUuid(4805);

  auto subquery = values_left;
  subquery.physical_node_id = 6;
  subquery.relational_node_id = 6;
  subquery.node_kind = exec::PhysicalNodeKind::kSubquery;
  subquery.implementation_id = "subquery.table.v1";
  subquery.input_physical_node_ids = {5};
  subquery.causal_counter_id = 40106;
  subquery.selected_alternative_uuid = WindowUuid(4606);
  subquery.executor_capability_uuid = WindowUuid(4706);
  subquery.cost_vector_uuid = WindowUuid(4806);

  auto window = request.physical_dag.nodes[1];
  window.input_physical_node_ids = {6};
  request.physical_dag.nodes = {values_left, values_right, join, aggregate,
                                subquery, window};
  return request;
}

exec::CanonicalWindowCompositionRequest CompositionRequest() {
  auto partition_request = CompositionPartitionRequest();
  auto frames = ExecuteFrame(partition_request, WholePartitionFrame());
  exec::CanonicalWindowAggregateRequest aggregate_request;
  aggregate_request.frames = frames;
  aggregate_request.function =
      exec::CanonicalWindowAggregateFunction::int64_sum;
  aggregate_request.function_uuid = std::string(kInt64SumUuid);
  aggregate_request.value_expression_descriptor_id = 4005;
  aggregate_request.result_column = frames.ordered_batch.columns[4];
  aggregate_request.result_column.stable_name = "composed_window_sum";
  aggregate_request.result_column.descriptor_id = 6100;
  const auto aggregate =
      exec::ExecuteCanonicalWindowAggregate(aggregate_request);

  exec::CanonicalWindowCompositionRequest request;
  request.input_batch = SourceBatch(frames);
  request.windows = {Materialization(
      frames, aggregate_request.result_column, aggregate.values,
      WindowUuid(5701))};
  request.projection_descriptor_ids = {4005, 6100};
  exec::CanonicalDescriptorOrderTerm order;
  order.column = 0;
  order.expression_descriptor_id = 4005;
  order.direction = exec::CanonicalDescriptorOrderDirection::descending;
  order.null_placement = exec::CanonicalDescriptorNullPlacement::last;
  request.query_order_terms = {order};
  request.query_order_tie_evidence_uuid = WindowUuid(5702);
  request.offset = 1;
  request.row_limit = 3;
  request.composition_dag = partition_request.physical_dag;
  request.required_upstream_node_kinds = {
      exec::PhysicalNodeKind::kJoin,
      exec::PhysicalNodeKind::kAggregate,
      exec::PhysicalNodeKind::kSubquery};
  return request;
}

bool ValidateOrdinaryNodeComposition() {
  const auto result =
      exec::ExecuteCanonicalWindowComposition(CompositionRequest());
  return Require401(
      result.diagnostic.ok && result.ordinary_physical_nodes_validated &&
          result.output_batch.rows.size() == 3 &&
          result.output_batch.rows[0].values[0].encoded_value == "106" &&
          result.output_batch.rows[1].values[0].encoded_value == "105" &&
          result.output_batch.rows[2].values[0].encoded_value == "104" &&
          result.source_row_indices ==
              std::vector<std::size_t>({6, 5, 4}) &&
          result.stage_trace ==
              std::vector<exec::CanonicalQueryEvaluationStage>{
                  exec::CanonicalQueryEvaluationStage::from,
                  exec::CanonicalQueryEvaluationStage::group_and_aggregate,
                  exec::CanonicalQueryEvaluationStage::window,
                  exec::CanonicalQueryEvaluationStage::projection,
                  exec::CanonicalQueryEvaluationStage::query_order,
                  exec::CanonicalQueryEvaluationStage::offset_limit_fetch_top} &&
          result.authority.engine_mga_snapshot_bound &&
          !result.authority.owns_transaction_finality &&
          !result.authority.owns_recovery,
      "window did not compose through ordinary join/group/subquery/order/limit stages");
}

bool ValidateCompositionRefusals() {
  bool passed = true;
  auto request = CompositionRequest();
  request.shape_specific_execution_route_claimed = true;
  passed &= Require401(
      CompositionRefused(exec::ExecuteCanonicalWindowComposition(request),
                         "QOW-DIAG-WINDOW-COMPOSITION"),
      "shape-specific executor route gained window composition authority");

  request = CompositionRequest();
  request.required_upstream_node_kinds.push_back(
      exec::PhysicalNodeKind::kCte);
  passed &= Require401(
      CompositionRefused(exec::ExecuteCanonicalWindowComposition(request),
                         "QOW-DIAG-WINDOW-COMPOSITION"),
      "missing ordinary composition node was treated as executed");

  request = CompositionRequest();
  request.transaction_finality_claimed = true;
  passed &= Require401(
      CompositionRefused(exec::ExecuteCanonicalWindowComposition(request),
                         "QOW-DIAG-WINDOW-COMPOSITION"),
      "window composition claimed engine transaction finality");
  return passed;
}

}  // namespace

// QOW-TEST-WIN-015-COMPOSITION-V1
int main() {
  return ValidateOrdinaryNodeComposition() && ValidateCompositionRefusals()
             ? EXIT_SUCCESS
             : EXIT_FAILURE;
}
