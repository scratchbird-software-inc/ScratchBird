// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#define QOW_WIN_002_FIXTURE_ONLY
#include "qow_win_002.cpp"

#define QOW_QRY_011_STATE_SPILL_FIXTURE_ONLY
#include "qow_qry_011_state_spill.cpp"

namespace {

exec::CanonicalRegistryWindowAggregateRequest RegistryWindowProfile(
    const exec::CanonicalAggregateFunction function) {
  auto profile = SpillProfile(function);
  auto request = RegistryAverageWindowRequest(WholePartitionFrame());

  const auto frame_row_count = request.frames.row_metadata.size();
  request.frames.ordered_batch.columns = profile.input_batch.columns;
  request.frames.ordered_batch.rows.clear();
  request.frames.ordered_batch.rows.reserve(frame_row_count);
  for (std::size_t row = 0; row < frame_row_count; ++row) {
    request.frames.ordered_batch.rows.push_back(
        profile.input_batch.rows[row % profile.input_batch.rows.size()]);
  }

  const auto window_dag = request.aggregate_template.physical_dag;
  request.aggregate_template = std::move(profile);
  auto& aggregate = request.aggregate_template;
  aggregate.physical_dag = window_dag;
  aggregate.mga_authority = request.frames.mga_authority;
  aggregate.selected_physical_node_id =
      request.frames.executed_physical_node_id;
  aggregate.input_batch = {};
  aggregate.filter_truth_values.reset();
  aggregate.result_column.stable_name = "registry_window_result";
  aggregate.result_column.descriptor_id = 5998;

  std::vector<std::uint32_t> frame_descriptor_ids;
  frame_descriptor_ids.reserve(request.frames.ordered_batch.columns.size());
  for (const auto& column : request.frames.ordered_batch.columns) {
    frame_descriptor_ids.push_back(column.descriptor_id);
  }
  aggregate.physical_dag.nodes[0].output_descriptor_ids =
      std::move(frame_descriptor_ids);
  aggregate.physical_dag.nodes[1].node_kind =
      exec::PhysicalNodeKind::kAggregate;
  aggregate.physical_dag.nodes[1].implementation_id =
      "window.aggregate-registry-frame-recompute.v1";
  aggregate.physical_dag.nodes[1].output_descriptor_ids = {5998};
  return request;
}

exec::CanonicalAggregateRuntimeResult FrameBaseline(
    const exec::CanonicalRegistryWindowAggregateRequest& window,
    const std::size_t frame_ordinal) {
  auto aggregate = SpillProfile(window.aggregate_template.descriptor.function);
  aggregate.input_batch.columns = window.frames.ordered_batch.columns;
  aggregate.input_batch.rows.clear();
  for (const auto row :
       window.frames.effective_frames[frame_ordinal].effective_row_indices) {
    aggregate.input_batch.rows.push_back(window.frames.ordered_batch.rows[row]);
  }
  return exec::ExecuteCanonicalAggregateRuntime(aggregate);
}

bool ValidateEveryRegistryAggregateAsWindow() {
  bool passed = true;
  const auto registry = exec::CanonicalAggregateRuntimeRegistryV1();
  std::size_t executed_profile_count = 0;
  std::size_t executed_window_value_count = 0;
  for (const auto& entry : registry) {
    const auto request = RegistryWindowProfile(entry.function);
    const auto direct =
        exec::ExecuteCanonicalRegistryWindowAggregate(request);
    bool profile_ok =
        direct.diagnostic.ok && entry.aggregate_as_window &&
        direct.descriptor.function == entry.function &&
        direct.values.size() == request.frames.ordered_batch.rows.size() &&
        direct.frame_row_indices == EffectiveFrameRows(request.frames) &&
        direct.effective_frame_recomputed &&
        !direct.moving_inverse_state_used &&
        direct.state_strategy_selected_from_physical_plan &&
        !direct.aggregate_state_spill_required &&
        direct.selected_state_strategy ==
            exec::CanonicalRegistryWindowAggregateStateStrategy::frame_recompute &&
        direct.selected_state_implementation_id ==
            "window.aggregate-registry-frame-recompute.v1" &&
        direct.shared_aggregate_state_authority_used &&
        direct.authority.engine_mga_snapshot_bound &&
        !direct.authority.owns_transaction_finality &&
        exec::PhysicalMgaStatementContextEqual(
            direct.mga_statement_context,
            request.frames.mga_statement_context) &&
        direct.selected_plan_uuid == request.frames.selected_plan_uuid &&
        direct.executed_physical_node_id ==
            request.frames.executed_physical_node_id &&
        direct.causal_counter_id == request.frames.causal_counter_id;

    std::size_t expected_transition_count = 0;
    for (std::size_t frame = 0; profile_ok && frame < direct.values.size();
         ++frame) {
      const auto baseline = FrameBaseline(request, frame);
      profile_ok = baseline.diagnostic.ok &&
                   baseline.output_batch.rows.size() == 1 &&
                   baseline.output_batch.rows[0].values.size() == 1 &&
                   SameValue(direct.values[frame],
                             baseline.output_batch.rows[0].values[0]);
      expected_transition_count += baseline.transition_count;
    }
    profile_ok = profile_ok &&
                 direct.transition_count == expected_transition_count &&
                 direct.combined_state_bytes != 0;

    exec::CanonicalWindowRuntimeRequest unified_request;
    unified_request.descriptor = RuntimeAggregateDescriptor(entry.function);
    unified_request.registry_aggregate = request;
    unified_request.forced_strategy =
        exec::CanonicalWindowRuntimeStrategy::aggregate;
    const auto unified =
        exec::ExecuteCanonicalWindowRuntime(unified_request);
    profile_ok =
        profile_ok &&
        RuntimeResultAccepted(
            unified, exec::CanonicalWindowRuntimeStrategy::aggregate,
            direct.values) &&
        unified.aggregate_registry_bridge_used &&
        unified.effective_frame_recomputed &&
        !unified.moving_inverse_state_used &&
        unified.aggregate_state_strategy_selected_from_physical_plan &&
        unified.selected_aggregate_state_strategy ==
            exec::CanonicalRegistryWindowAggregateStateStrategy::frame_recompute &&
        unified.selected_aggregate_state_implementation_id ==
            "window.aggregate-registry-frame-recompute.v1" &&
        exec::PhysicalMgaStatementContextEqual(
            unified.mga_statement_context,
            request.frames.mga_statement_context) &&
        unified.aggregate_transition_count == direct.transition_count;

    passed &= Require401(
        profile_ok,
        "aggregate registry profile did not execute every effective window frame: " +
            entry.builtin_id + ":direct=" +
            direct.diagnostic.diagnostic_code + ":" +
            direct.diagnostic.detail + ":unified=" +
            unified.diagnostic.diagnostic_code + ":" +
            unified.diagnostic.detail);
    if (profile_ok) {
      ++executed_profile_count;
      executed_window_value_count += direct.values.size();
    }
  }
  passed &= Require401(
      executed_profile_count == 43 && executed_window_value_count == 387,
      "aggregate-as-window proof did not cover 43 profiles by nine rows");
  return passed;
}

bool ValidateRegistryAggregateAsWindowRefusals() {
  bool passed = true;
  auto request = RegistryWindowProfile(
      exec::CanonicalAggregateFunction::regr_syy);
  request.aggregate_template.descriptor.function_uuid =
      Entry(exec::CanonicalAggregateFunction::regr_sxy).function_uuid;
  auto result = exec::ExecuteCanonicalRegistryWindowAggregate(request);
  passed &= Require401(
      RegistryAggregateRefused(
          result,
          {"QOW-DIAG-WINDOW-AGGREGATE-REGISTRY-DESCRIPTOR"}),
      "aggregate-as-window accepted cross-registry UUID drift");

  request = RegistryWindowProfile(exec::CanonicalAggregateFunction::sum);
  request.maximum_combined_state_bytes = 1;
  result = exec::ExecuteCanonicalRegistryWindowAggregate(request);
  passed &= Require401(
      RegistryAggregateRefused(
          result,
          {"QOW-DIAG-WINDOW-AGGREGATE-REGISTRY-RESOURCE"}),
      "aggregate-as-window published partial output after state exhaustion");
  return passed;
}

}  // namespace

int main() {
  return ValidateEveryRegistryAggregateAsWindow() &&
                 ValidateRegistryAggregateAsWindowRefusals()
             ? EXIT_SUCCESS
             : EXIT_FAILURE;
}
