// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#define QOW_WIN_015_MULTIPLE_FIXTURE_ONLY
#include "qow_win_015_multiple.cpp"

namespace {

exec::CanonicalWindowCompositionRequest QualifyRequest() {
  auto aggregate_request = AggregateWindowRequest();
  const auto aggregate =
      exec::ExecuteCanonicalWindowAggregate(aggregate_request);
  exec::CanonicalWindowCompositionRequest request;
  request.input_batch = SourceBatch(aggregate_request.frames);
  request.windows = {Materialization(
      aggregate_request.frames, aggregate_request.result_column,
      aggregate.values, WindowUuid(5601))};
  request.qualify_truth_values =
      std::vector<api::EngineSqlTruthValue>{
          api::EngineSqlTruthValue::true_value,
          api::EngineSqlTruthValue::unknown,
          api::EngineSqlTruthValue::false_value,
          api::EngineSqlTruthValue::true_value,
          api::EngineSqlTruthValue::false_value,
          api::EngineSqlTruthValue::false_value,
          api::EngineSqlTruthValue::true_value,
          api::EngineSqlTruthValue::false_value,
          api::EngineSqlTruthValue::unknown};
  request.qualify_referenced_window_descriptor_ids = {5999};
  request.projection_descriptor_ids = {4005, 5999};
  exec::CanonicalDescriptorOrderTerm order;
  order.column = 0;
  order.expression_descriptor_id = 4005;
  order.direction = exec::CanonicalDescriptorOrderDirection::descending;
  order.null_placement = exec::CanonicalDescriptorNullPlacement::last;
  request.query_order_terms = {order};
  request.query_order_tie_evidence_uuid = WindowUuid(5602);
  request.offset = 1;
  request.row_limit = 2;
  return request;
}

bool ValidateQualifyStageOrder() {
  const auto result =
      exec::ExecuteCanonicalWindowComposition(QualifyRequest());
  return Require401(
      result.diagnostic.ok && result.output_batch.rows.size() == 2 &&
          result.output_batch.rows[0].values[0].encoded_value == "103" &&
          result.output_batch.rows[1].values[0].encoded_value == "100" &&
          result.source_row_indices == std::vector<std::size_t>({3, 0}) &&
          result.all_windows_materialized_before_qualify &&
          result.qualify_uses_true_only_3vl &&
          result.projection_precedes_query_order &&
          result.query_order_precedes_row_limit &&
          result.stage_trace ==
              std::vector<exec::CanonicalQueryEvaluationStage>{
                  exec::CanonicalQueryEvaluationStage::from,
                  exec::CanonicalQueryEvaluationStage::window,
                  exec::CanonicalQueryEvaluationStage::qualify,
                  exec::CanonicalQueryEvaluationStage::projection,
                  exec::CanonicalQueryEvaluationStage::query_order,
                  exec::CanonicalQueryEvaluationStage::offset_limit_fetch_top},
      "QUALIFY did not run after WINDOW and before projection/order/limit");
}

bool ValidateQualifyRefusals() {
  bool passed = true;
  auto request = QualifyRequest();
  request.qualify_referenced_window_descriptor_ids.clear();
  passed &= Require401(
      CompositionRefused(exec::ExecuteCanonicalWindowComposition(request),
                         "QOW-DIAG-WINDOW-QUALIFY"),
      "QUALIFY admitted an unbound window-result reference");

  request = QualifyRequest();
  (*request.qualify_truth_values)[2] =
      api::EngineSqlTruthValue::unspecified;
  passed &= Require401(
      CompositionRefused(exec::ExecuteCanonicalWindowComposition(request),
                         "QOW-DIAG-WINDOW-QUALIFY"),
      "QUALIFY admitted an unbound SQL truth value");
  return passed;
}

}  // namespace

// QOW-TEST-WIN-015-QUALIFY-V1
int main() {
  return ValidateQualifyStageOrder() && ValidateQualifyRefusals()
             ? EXIT_SUCCESS
             : EXIT_FAILURE;
}
