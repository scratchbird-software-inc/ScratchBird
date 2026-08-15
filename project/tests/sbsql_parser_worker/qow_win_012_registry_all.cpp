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
  aggregate.distinct = window.aggregate_template.distinct;
  aggregate.aggregate_order_terms =
      window.aggregate_template.aggregate_order_terms;
  if (window.aggregate_template.filter_truth_values.has_value()) {
    std::vector<api::EngineSqlTruthValue> filter;
    filter.reserve(aggregate.input_batch.rows.size());
    for (const auto row :
         window.frames.effective_frames[frame_ordinal].effective_row_indices) {
      filter.push_back(
          (*window.aggregate_template.filter_truth_values)[row]);
    }
    aggregate.filter_truth_values = std::move(filter);
  }
  return exec::ExecuteCanonicalAggregateRuntime(aggregate);
}

void ApplyEveryAuthorizedModifier(
    exec::CanonicalRegistryWindowAggregateRequest* request) {
  if (request == nullptr) return;
  auto& aggregate = request->aggregate_template;
  aggregate.filter_truth_values =
      std::vector<api::EngineSqlTruthValue>{
          api::EngineSqlTruthValue::true_value,
          api::EngineSqlTruthValue::false_value,
          api::EngineSqlTruthValue::unknown,
          api::EngineSqlTruthValue::true_value,
          api::EngineSqlTruthValue::true_value,
          api::EngineSqlTruthValue::false_value,
          api::EngineSqlTruthValue::true_value,
          api::EngineSqlTruthValue::unknown,
          api::EngineSqlTruthValue::true_value};
  aggregate.distinct = !aggregate.descriptor.count_star;
  if (aggregate.aggregate_order_terms.empty()) {
    aggregate.aggregate_order_terms = {
        {.column = 3,
         .expression_descriptor_id =
             request->frames.ordered_batch.columns[3].descriptor_id,
         .direction =
             exec::CanonicalDescriptorOrderDirection::descending,
         .null_placement = exec::CanonicalDescriptorNullPlacement::first}};
  } else {
    aggregate.aggregate_order_terms.front().direction =
        exec::CanonicalDescriptorOrderDirection::descending;
    aggregate.aggregate_order_terms.front().null_placement =
        exec::CanonicalDescriptorNullPlacement::first;
  }
}

bool ExactRegistryAggregateReceipts(
    const exec::CanonicalRegistryWindowAggregateRequest& request,
    const exec::CanonicalRegistryWindowAggregateResult& result) {
  const auto& aggregate = request.aggregate_template;
  const auto expected_modifier_count =
      (aggregate.filter_truth_values.has_value() ? 1U : 0U) +
      (aggregate.distinct ? 1U : 0U) +
      (!aggregate.aggregate_order_terms.empty() ? 1U : 0U);
  return result.result_column.stable_name == aggregate.result_column.stable_name &&
         result.result_column.nullable == aggregate.result_column.nullable &&
         result.result_column.descriptor_id ==
             aggregate.result_column.descriptor_id &&
         result.result_column.descriptor.descriptor_uuid.canonical ==
             aggregate.result_column.descriptor.descriptor_uuid.canonical &&
         result.result_column.descriptor.descriptor_kind ==
             aggregate.result_column.descriptor.descriptor_kind &&
         result.result_column.descriptor.canonical_type_name ==
             aggregate.result_column.descriptor.canonical_type_name &&
         result.result_column.descriptor.encoded_descriptor ==
             aggregate.result_column.descriptor.encoded_descriptor &&
         result.value_columns == aggregate.value_columns &&
         result.value_expression_descriptor_ids ==
             aggregate.value_expression_descriptor_ids &&
         result.direct_argument_count == aggregate.direct_arguments.size() &&
         result.modifier_count == expected_modifier_count &&
         result.aggregate_order_term_count ==
             aggregate.aggregate_order_terms.size() &&
         result.every_aggregate_descriptor_field_consumed &&
         result.every_effective_frame_consumed &&
         result.modifier_pipeline_validated &&
         result.filter_modifier_applied ==
             aggregate.filter_truth_values.has_value() &&
         result.distinct_modifier_applied == aggregate.distinct &&
         result.filter_applied_before_distinct &&
         result.distinct_applied_before_order &&
         result.aggregate_order_applied ==
             !aggregate.aggregate_order_terms.empty() &&
         !result.cancellation_observed &&
         result.transient_state_cleanup_proven &&
         result.all_or_nothing_publication &&
         result.partition_property_uuid ==
             request.frames.partition_property_uuid &&
         result.ordering_property_uuid ==
             request.frames.ordering_property_uuid &&
         result.term_binding_evidence_uuid ==
             request.frames.term_binding_evidence_uuid &&
         result.deterministic_tie_evidence_uuid ==
             request.frames.deterministic_tie_evidence_uuid &&
         result.frame_property_binding_evidence_uuid ==
             request.frames.frame_property_binding_evidence_uuid;
}

bool ValidateEveryRegistryAggregateAsWindow() {
  bool passed = true;
  const auto registry = exec::CanonicalAggregateRuntimeRegistryV1();
  std::size_t executed_profile_count = 0;
  std::size_t executed_window_value_count = 0;
  std::size_t executed_modifier_profile_count = 0;
  std::size_t executed_modifier_window_value_count = 0;
  std::size_t executed_distinct_modifier_profile_count = 0;
  std::size_t inverse_admitted_registry_profile_count = 0;
  std::size_t moving_inverse_profile_count = 0;
  std::size_t frame_recompute_fallback_profile_count = 0;
  for (const auto& entry : registry) {
    const auto request = RegistryWindowProfile(entry.function);
    const auto direct =
        exec::ExecuteCanonicalRegistryWindowAggregate(request);
    bool profile_ok =
        direct.diagnostic.ok && entry.aggregate_as_window &&
        direct.descriptor.function == entry.function &&
        direct.values.size() == request.frames.ordered_batch.rows.size() &&
        direct.frame_row_indices == EffectiveFrameRows(request.frames) &&
        ExactRegistryAggregateReceipts(request, direct) &&
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
    std::size_t expected_distinct_tuple_count = 0;
    std::size_t expected_order_comparison_count = 0;
    for (std::size_t frame = 0; profile_ok && frame < direct.values.size();
         ++frame) {
      const auto baseline = FrameBaseline(request, frame);
      profile_ok = baseline.diagnostic.ok &&
                   baseline.output_batch.rows.size() == 1 &&
                   baseline.output_batch.rows[0].values.size() == 1 &&
                   SameValue(direct.values[frame],
                             baseline.output_batch.rows[0].values[0]);
      expected_transition_count += baseline.transition_count;
      expected_distinct_tuple_count += baseline.distinct_tuple_count;
      expected_order_comparison_count += baseline.order_comparison_count;
    }
    profile_ok = profile_ok &&
                 direct.transition_count == expected_transition_count &&
                 direct.distinct_tuple_count ==
                     expected_distinct_tuple_count &&
                 direct.order_comparison_count ==
                     expected_order_comparison_count &&
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

    auto strategy_request = request;
    SelectRegistryWindowStateStrategy(
        &strategy_request,
        exec::CanonicalRegistryWindowAggregateStateStrategy::moving_inverse);
    const auto strategy_result =
        exec::ExecuteCanonicalRegistryWindowAggregate(strategy_request);
    if (entry.moving_window_inverse) {
      ++inverse_admitted_registry_profile_count;
      const bool moving_ok =
          SameRegistryAggregateValues(direct, strategy_result) &&
          strategy_result.moving_inverse_state_used &&
          !strategy_result.effective_frame_recomputed &&
          strategy_result.selected_state_strategy ==
              exec::CanonicalRegistryWindowAggregateStateStrategy::moving_inverse &&
          strategy_result.transient_state_cleanup_proven &&
          strategy_result.all_or_nothing_publication;
      passed &= Require401(
          moving_ok,
          "aggregate registry inverse-admitted profile did not match frame recomputation: " +
              entry.builtin_id);
      if (moving_ok) ++moving_inverse_profile_count;
    } else {
      const bool fallback_ok =
          direct.effective_frame_recomputed &&
          direct.selected_state_strategy ==
              exec::CanonicalRegistryWindowAggregateStateStrategy::frame_recompute &&
          RegistryAggregateRefused(
              strategy_result,
              {"QOW-DIAG-QRY-011-REGISTRY-INVERSE-UNAVAILABLE-V1"});
      passed &= Require401(
          fallback_ok,
          "aggregate registry non-invertible profile did not retain canonical frame recomputation: " +
              entry.builtin_id);
      if (fallback_ok) ++frame_recompute_fallback_profile_count;
    }

    auto modified_request = RegistryWindowProfile(entry.function);
    ApplyEveryAuthorizedModifier(&modified_request);
    const auto modified =
        exec::ExecuteCanonicalRegistryWindowAggregate(modified_request);
    bool modifiers_ok =
        modified.diagnostic.ok &&
        ExactRegistryAggregateReceipts(modified_request, modified) &&
        modified.values.size() ==
            modified_request.frames.ordered_batch.rows.size() &&
        modified.frame_row_indices ==
            EffectiveFrameRows(modified_request.frames) &&
        modified.filter_modifier_applied &&
        modified.aggregate_order_applied &&
        modified.distinct_modifier_applied ==
            !modified_request.aggregate_template.descriptor.count_star;
    std::size_t modified_transition_count = 0;
    std::size_t modified_distinct_tuple_count = 0;
    std::size_t modified_order_comparison_count = 0;
    for (std::size_t frame = 0;
         modifiers_ok && frame < modified.values.size(); ++frame) {
      const auto baseline = FrameBaseline(modified_request, frame);
      modifiers_ok =
          baseline.diagnostic.ok &&
          baseline.output_batch.rows.size() == 1 &&
          baseline.output_batch.rows[0].values.size() == 1 &&
          SameValue(modified.values[frame],
                    baseline.output_batch.rows[0].values[0]);
      modified_transition_count += baseline.transition_count;
      modified_distinct_tuple_count += baseline.distinct_tuple_count;
      modified_order_comparison_count += baseline.order_comparison_count;
    }
    modifiers_ok =
        modifiers_ok &&
        modified.transition_count == modified_transition_count &&
        modified.distinct_tuple_count == modified_distinct_tuple_count &&
        modified.order_comparison_count == modified_order_comparison_count;

    exec::CanonicalWindowRuntimeRequest modified_unified_request;
    modified_unified_request.descriptor =
        RuntimeAggregateDescriptor(entry.function);
    modified_unified_request.registry_aggregate = modified_request;
    modified_unified_request.forced_strategy =
        exec::CanonicalWindowRuntimeStrategy::aggregate;
    const auto modified_unified =
        exec::ExecuteCanonicalWindowRuntime(modified_unified_request);
    modifiers_ok =
        modifiers_ok &&
        RuntimeResultAccepted(
            modified_unified,
            exec::CanonicalWindowRuntimeStrategy::aggregate,
            modified.values) &&
        modified_unified.aggregate_registry_bridge_used &&
        modified_unified.aggregate_transition_count ==
            modified.transition_count;
    passed &= Require401(
        modifiers_ok,
        "aggregate registry profile did not execute its authorized FILTER/DISTINCT/order pipeline as a window: " +
            entry.builtin_id + ":direct=" +
            modified.diagnostic.diagnostic_code + ":" +
            modified.diagnostic.detail + ":unified=" +
            modified_unified.diagnostic.diagnostic_code + ":" +
            modified_unified.diagnostic.detail);
    if (modifiers_ok) {
      ++executed_modifier_profile_count;
      executed_modifier_window_value_count += modified.values.size();
      if (modified.distinct_modifier_applied) {
        ++executed_distinct_modifier_profile_count;
      }
    }
  }
  passed &= Require401(
      executed_profile_count == 43 && executed_window_value_count == 387,
      "aggregate-as-window proof did not cover 43 profiles by nine rows");
  passed &= Require401(
      executed_modifier_profile_count == 43 &&
          executed_modifier_window_value_count == 387 &&
          executed_distinct_modifier_profile_count == 42,
      "aggregate-as-window modifier proof did not cover 43 FILTER/order and 42 DISTINCT profiles by nine rows");
  passed &= Require401(
      inverse_admitted_registry_profile_count == 3 &&
          moving_inverse_profile_count == 3 &&
          frame_recompute_fallback_profile_count == 40,
      "aggregate-as-window state-strategy proof did not cover three admitted identities and 40 non-invertible profiles");
  return passed;
}

bool HasRegistryWindowSpillArtifact(const std::filesystem::path& root) {
  std::error_code error;
  if (!std::filesystem::exists(root, error)) return static_cast<bool>(error);
  for (std::filesystem::recursive_directory_iterator iterator(root, error),
       end;
       !error && iterator != end; iterator.increment(error)) {
    if (iterator->path().extension() == ".sbtmpidx") return true;
  }
  return static_cast<bool>(error);
}

bool ValidateEveryRegistryAggregateWindowSpill(
    const std::filesystem::path& root) {
  bool passed = true;
  std::size_t spilled_profile_count = 0;
  std::size_t restored_window_value_count = 0;
  const auto& registry = exec::CanonicalAggregateRuntimeRegistryV1();
  for (std::size_t ordinal = 0; ordinal < registry.size(); ++ordinal) {
    const auto& entry = registry[ordinal];
    const auto baseline_request = RegistryWindowProfile(entry.function);
    const auto baseline =
        exec::ExecuteCanonicalRegistryWindowAggregate(baseline_request);
    exec::CanonicalRegistryWindowAggregateSpillRequest spill;
    spill.aggregate_request = baseline_request;
    SelectRegistryWindowStateStrategy(
        &spill.aggregate_request,
        exec::CanonicalRegistryWindowAggregateStateStrategy::state_spill);
    spill.aggregate_request.aggregate_template.physical_dag.spill_allowed =
        true;
    spill.spill_root = root;
    spill.spill_owner_uuid = WindowUuid(7600 + ordinal);
    spill.runtime_generation = 7700 + ordinal;
    spill.memory_quota_bytes = 128;
    std::error_code owner_error;
    std::filesystem::create_directories(
        root / spill.spill_owner_uuid, owner_error);
    const auto result =
        exec::ExecuteCanonicalRegistryWindowAggregateSpill(spill);
    const bool profile_ok =
        !owner_error && baseline.diagnostic.ok && result.diagnostic.ok &&
        result.spilled &&
        result.spill_reopened && result.cleanup_proven &&
        result.spilled_aggregate_state_count == 9 &&
        result.serialized_aggregate_state_bytes != 0 &&
        result.spilled_aggregate_state_record_count != 0 &&
        SameRegistryAggregateValues(baseline, result.aggregate_result) &&
        ExactRegistryAggregateReceipts(spill.aggregate_request,
                                       result.aggregate_result) &&
        result.aggregate_result.selected_state_strategy ==
            exec::CanonicalRegistryWindowAggregateStateStrategy::state_spill &&
        result.aggregate_result.transient_state_cleanup_proven &&
        result.aggregate_result.all_or_nothing_publication &&
        !HasRegistryWindowSpillArtifact(root);
    passed &= Require401(
        profile_ok,
        "aggregate registry profile did not spill/reopen exact Window state: " +
            entry.builtin_id + ":" + result.diagnostic.diagnostic_code +
            ":" + result.diagnostic.detail);
    if (profile_ok) {
      ++spilled_profile_count;
      restored_window_value_count += result.aggregate_result.values.size();
    }
  }
  passed &= Require401(
      spilled_profile_count == 43 && restored_window_value_count == 387,
      "aggregate Window spill proof did not cover 43 profiles by nine rows");
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
  const auto root = std::filesystem::temp_directory_path() /
                    ("scratchbird_qow405_registry_window_spill_" +
                     std::to_string(std::chrono::steady_clock::now()
                                        .time_since_epoch()
                                        .count()));
  std::error_code error;
  std::filesystem::remove_all(root, error);
  const bool passed = ValidateEveryRegistryAggregateAsWindow() &&
                      ValidateEveryRegistryAggregateWindowSpill(root) &&
                      ValidateRegistryAggregateAsWindowRefusals();
  std::filesystem::remove_all(root, error);
  return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
