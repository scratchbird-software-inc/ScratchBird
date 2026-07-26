// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#define QOW_WIN_012_STATE_FIXTURE_ONLY
#include "qow_win_012_state.cpp"

#include <cstdlib>

namespace {

exec::DescriptorBatch SourceBatch(
    const exec::CanonicalWindowFrameResult& frames) {
  exec::DescriptorBatch source;
  source.columns = frames.ordered_batch.columns;
  source.rows.resize(frames.ordered_batch.rows.size());
  for (std::size_t ordered = 0; ordered < frames.row_metadata.size(); ++ordered) {
    source.rows[frames.row_metadata[ordered].source_row_index] =
        frames.ordered_batch.rows[ordered];
  }
  return source;
}

exec::CanonicalWindowMaterialization Materialization(
    exec::CanonicalWindowFrameResult frames,
    exec::ExecutorColumnDescriptor result_column,
    std::vector<api::EngineTypedValue> values,
    const std::string& function_state_uuid) {
  exec::CanonicalWindowMaterialization materialization;
  materialization.frames = std::move(frames);
  materialization.result_column = std::move(result_column);
  materialization.values = std::move(values);
  materialization.function_state_uuid = function_state_uuid;
  return materialization;
}

std::vector<std::string> ValuesBySource(
    const exec::CanonicalWindowFrameResult& frames,
    const std::vector<api::EngineTypedValue>& values) {
  std::vector<std::string> by_source(values.size());
  for (std::size_t ordered = 0; ordered < values.size(); ++ordered) {
    by_source[frames.row_metadata[ordered].source_row_index] =
        values[ordered].state == api::EngineValueState::sql_null
            ? "<NULL>"
            : values[ordered].encoded_value;
  }
  return by_source;
}

bool CompositionRefused(
    const exec::CanonicalWindowCompositionResult& result,
    const std::string_view code) {
  return !result.diagnostic.ok && result.diagnostic.diagnostic_code == code &&
         result.output_batch.rows.empty() && result.stage_trace.empty();
}

bool ValidateMultipleWindowMaterialization() {
  auto first_request = AggregateWindowRequest();
  const auto first_result =
      exec::ExecuteCanonicalWindowAggregate(first_request);

  auto second_request = first_request;
  second_request.result_column.stable_name = "window_filtered_sum";
  second_request.result_column.descriptor_id = 6000;
  second_request.filter_truth_values =
      std::vector<api::EngineSqlTruthValue>(
          second_request.frames.ordered_batch.rows.size(),
          api::EngineSqlTruthValue::true_value);
  (*second_request.filter_truth_values)[1] =
      api::EngineSqlTruthValue::false_value;
  const auto second_result =
      exec::ExecuteCanonicalWindowAggregate(second_request);
  if (!first_result.diagnostic.ok || !second_result.diagnostic.ok) return false;

  exec::CanonicalWindowCompositionRequest request;
  request.input_batch = SourceBatch(first_request.frames);
  request.windows = {
      Materialization(first_request.frames, first_request.result_column,
                      first_result.values, WindowUuid(5501)),
      Materialization(second_request.frames, second_request.result_column,
                      second_result.values, WindowUuid(5502))};
  request.projection_descriptor_ids = {4005, 5999, 6000};
  const auto result = exec::ExecuteCanonicalWindowComposition(request);
  const auto first_by_source =
      ValuesBySource(first_request.frames, first_result.values);
  const auto second_by_source =
      ValuesBySource(second_request.frames, second_result.values);
  bool rows_match = result.output_batch.rows.size() == first_by_source.size();
  for (std::size_t row = 0; rows_match && row < first_by_source.size(); ++row) {
    const auto& values = result.output_batch.rows[row].values;
    const auto first_text = values[1].state == api::EngineValueState::sql_null
                                ? "<NULL>"
                                : values[1].encoded_value;
    const auto second_text = values[2].state == api::EngineValueState::sql_null
                                 ? "<NULL>"
                                 : values[2].encoded_value;
    rows_match = first_text == first_by_source[row] &&
                 second_text == second_by_source[row];
  }
  bool passed = Require401(
      result.diagnostic.ok && rows_match &&
          result.materialized_window_descriptor_ids ==
              std::vector<std::uint32_t>({5999, 6000}) &&
          result.shared_materialization_pair_count == 1 &&
          result.every_function_state_independent &&
          result.stage_trace ==
              std::vector<exec::CanonicalQueryEvaluationStage>{
                  exec::CanonicalQueryEvaluationStage::from,
                  exec::CanonicalQueryEvaluationStage::window,
                  exec::CanonicalQueryEvaluationStage::projection},
      "multiple independent window states were merged or lost source identity");

  request.windows[1].function_state_uuid =
      request.windows[0].function_state_uuid;
  passed &= Require401(
      CompositionRefused(exec::ExecuteCanonicalWindowComposition(request),
                         "QOW-DIAG-WINDOW-MULTIPLE"),
      "duplicate function-state identity merged two window functions");
  return passed;
}

bool ValidateIndependentWindowSpecifications() {
  auto whole_request = AggregateWindowRequest();
  auto prefix_request = AggregateWindowRequest(PrefixFrame());
  prefix_request.result_column.stable_name = "prefix_sum";
  prefix_request.result_column.descriptor_id = 6001;
  const auto whole = exec::ExecuteCanonicalWindowAggregate(whole_request);
  const auto prefix = exec::ExecuteCanonicalWindowAggregate(prefix_request);
  if (!whole.diagnostic.ok || !prefix.diagnostic.ok) return false;

  exec::CanonicalWindowCompositionRequest request;
  request.input_batch = SourceBatch(whole_request.frames);
  request.windows = {
      Materialization(whole_request.frames, whole_request.result_column,
                      whole.values, WindowUuid(5511)),
      Materialization(prefix_request.frames, prefix_request.result_column,
                      prefix.values, WindowUuid(5512))};
  request.projection_descriptor_ids = {5999, 6001};
  const auto result = exec::ExecuteCanonicalWindowComposition(request);
  return Require401(
      result.diagnostic.ok && result.shared_materialization_pair_count == 0 &&
          result.output_batch.rows.size() == request.input_batch.rows.size(),
      "independent window specifications were incorrectly stage-shared");
}

}  // namespace

#ifndef QOW_WIN_015_MULTIPLE_FIXTURE_ONLY
// QOW-TEST-WIN-015-MULTIPLE-V1
int main() {
  return ValidateMultipleWindowMaterialization() &&
                 ValidateIndependentWindowSpecifications()
             ? EXIT_SUCCESS
             : EXIT_FAILURE;
}
#endif
