// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#define QOW_WIN_012_STATE_FIXTURE_ONLY
#include "qow_win_012_state.cpp"

#include <set>

namespace {

exec::CanonicalWindowRuntimeDescriptor RuntimeDescriptor(
    const exec::CanonicalWindowRuntimeFunction function) {
  const auto registry = exec::CanonicalWindowRuntimeRegistryV1();
  const auto found = std::ranges::find_if(
      registry, [&](const auto& row) { return row.function == function; });
  return found == registry.end()
             ? exec::CanonicalWindowRuntimeDescriptor{}
             : *found;
}

exec::CanonicalWindowRuntimeDescriptor RuntimeAggregateDescriptor(
    const exec::CanonicalAggregateFunction function) {
  const auto registry = exec::CanonicalAggregateRuntimeRegistryV1();
  const auto found = std::ranges::find_if(
      registry, [&](const auto& row) { return row.function == function; });
  if (found == registry.end()) return {};
  exec::CanonicalWindowRuntimeDescriptor descriptor;
  descriptor.abi_version = found->abi_version;
  descriptor.builtin_id = found->builtin_id;
  descriptor.function_uuid = found->function_uuid;
  descriptor.aggregate_function = found->function;
  return descriptor;
}

exec::CanonicalRegistryWindowAggregateRequest RegistrySumWindowRequest(
    exec::CanonicalWindowFrameDescriptor frame = WholePartitionFrame()) {
  auto request = RegistryAverageWindowRequest(std::move(frame));
  auto& aggregate = request.aggregate_template;
  aggregate.descriptor = {
      1, exec::CanonicalAggregateFunction::sum, "sb.aggregate.sum",
      std::string(kInt64SumUuid), false};
  aggregate.result_column = request.frames.ordered_batch.columns[4];
  aggregate.result_column.stable_name = "window_sum";
  aggregate.result_column.descriptor_id = 5998;
  return request;
}

exec::CanonicalRegistryWindowAggregateRequest RegistryMinimumWindowRequest(
    exec::CanonicalWindowFrameDescriptor frame = WholePartitionFrame()) {
  auto request = RegistryAverageWindowRequest(std::move(frame));
  auto& aggregate = request.aggregate_template;
  aggregate.descriptor = {
      1, exec::CanonicalAggregateFunction::min, "sb.aggregate.min",
      std::string(kMinimumUuid), false};
  aggregate.result_column = request.frames.ordered_batch.columns[4];
  aggregate.result_column.stable_name = "window_min";
  aggregate.result_column.descriptor_id = 5998;
  return request;
}

bool SameValue(const api::EngineTypedValue& left,
               const api::EngineTypedValue& right) {
  return left.descriptor.descriptor_uuid.canonical ==
             right.descriptor.descriptor_uuid.canonical &&
         left.descriptor.descriptor_kind == right.descriptor.descriptor_kind &&
         left.descriptor.canonical_type_name ==
             right.descriptor.canonical_type_name &&
         left.descriptor.encoded_descriptor ==
             right.descriptor.encoded_descriptor &&
         left.encoded_value == right.encoded_value &&
         left.binary_value == right.binary_value &&
         left.is_null == right.is_null && left.state == right.state;
}

bool SameValues(const std::vector<api::EngineTypedValue>& left,
                const std::vector<api::EngineTypedValue>& right) {
  if (left.size() != right.size()) return false;
  for (std::size_t index = 0; index < left.size(); ++index) {
    if (!SameValue(left[index], right[index])) return false;
  }
  return true;
}

exec::CanonicalWindowRankingRequest RuntimeRankingRequest(
    const exec::CanonicalWindowRankingFunction function,
    const exec::CanonicalWindowRuntimeFunction runtime_function) {
  exec::CanonicalWindowRankingRequest request;
  request.frames = ValueFrames(WholePartitionFrame());
  request.function = function;
  request.function_uuid = RuntimeDescriptor(runtime_function).function_uuid;
  const bool real =
      function == exec::CanonicalWindowRankingFunction::percent_rank ||
      function == exec::CanonicalWindowRankingFunction::cume_dist;
  request.output_descriptor = WindowDescriptor(
      real ? 6601 : 6600, real ? "real64" : "int64",
      "type_uuid=" + WindowUuid(real ? 6603 : 6602) +
          ";nullability=non_null");
  if (function == exec::CanonicalWindowRankingFunction::ntile) {
    request.ntile_bucket_count = TypedOffset("int64", "3", 6610);
  }
  return request;
}

bool RuntimeResultAccepted(
    const exec::CanonicalWindowRuntimeResult& result,
    const exec::CanonicalWindowRuntimeStrategy expected_strategy,
    const std::vector<api::EngineTypedValue>& direct_values) {
  const bool strategy_receipt =
      expected_strategy == exec::CanonicalWindowRuntimeStrategy::ranking
          ? result.ranking_strategy_result.has_value() &&
                !result.value_strategy_result.has_value() &&
                !result.aggregate_strategy_result.has_value()
          : expected_strategy == exec::CanonicalWindowRuntimeStrategy::value
                ? !result.ranking_strategy_result.has_value() &&
                      result.value_strategy_result.has_value() &&
                      !result.aggregate_strategy_result.has_value()
                : !result.ranking_strategy_result.has_value() &&
                      !result.value_strategy_result.has_value() &&
                      result.aggregate_strategy_result.has_value();
  return result.diagnostic.ok && strategy_receipt &&
         result.executed_strategy == expected_strategy &&
         result.every_descriptor_field_consumed &&
         result.exactly_one_strategy_payload_consumed &&
         result.retained_strategy_reached &&
         result.canonical_registry_state_frame_executor_used &&
         result.split_runtime_bypass_forbidden &&
         result.authority.engine_mga_snapshot_bound &&
         result.selected_plan_uuid == WindowUuid(4301) &&
         result.causal_counter_id == 40102 &&
         SameValues(result.values, direct_values);
}

bool ValidateRuntimeRegistry() {
  const auto registry = exec::CanonicalWindowRuntimeRegistryV1();
  std::set<exec::CanonicalWindowRuntimeFunction> functions;
  std::set<std::string> builtin_ids;
  std::set<std::string> uuids;
  bool passed = Require401(registry.size() == 11,
                           "canonical runtime registry row count drifted");
  for (const auto& row : registry) {
    passed &= Require401(
        row.abi_version == 1 &&
            row.function != exec::CanonicalWindowRuntimeFunction::unknown &&
            !row.aggregate_function.has_value() &&
            !row.builtin_id.empty() && !row.function_uuid.empty() &&
            functions.insert(row.function).second &&
            builtin_ids.insert(row.builtin_id).second &&
            uuids.insert(row.function_uuid).second,
        "canonical runtime registry contains an incomplete or duplicate row");
  }
  const auto aggregate_registry = exec::CanonicalAggregateRuntimeRegistryV1();
  passed &= Require401(aggregate_registry.size() == 43,
                       "aggregate-as-window registry row count drifted");
  for (const auto& row : aggregate_registry) {
    exec::CanonicalWindowRuntimeRequest request;
    request.descriptor = RuntimeAggregateDescriptor(row.function);
    const auto result = exec::ExecuteCanonicalWindowRuntime(request);
    passed &= Require401(
        !result.diagnostic.ok &&
            result.diagnostic.diagnostic_code ==
                "QOW-DIAG-WINDOW-RUNTIME-PAYLOAD",
        "aggregate identity did not reach unified runtime payload admission");
  }
  return passed;
}

bool ValidateRankingStrategies() {
  struct Case {
    exec::CanonicalWindowRuntimeFunction runtime;
    exec::CanonicalWindowRankingFunction ranking;
  };
  const std::vector<Case> cases = {
      {exec::CanonicalWindowRuntimeFunction::row_number,
       exec::CanonicalWindowRankingFunction::row_number},
      {exec::CanonicalWindowRuntimeFunction::rank,
       exec::CanonicalWindowRankingFunction::rank},
      {exec::CanonicalWindowRuntimeFunction::dense_rank,
       exec::CanonicalWindowRankingFunction::dense_rank},
      {exec::CanonicalWindowRuntimeFunction::percent_rank,
       exec::CanonicalWindowRankingFunction::percent_rank},
      {exec::CanonicalWindowRuntimeFunction::cume_dist,
       exec::CanonicalWindowRankingFunction::cume_dist},
      {exec::CanonicalWindowRuntimeFunction::ntile,
       exec::CanonicalWindowRankingFunction::ntile},
  };
  bool passed = true;
  for (const auto& item : cases) {
    auto ranking = RuntimeRankingRequest(item.ranking, item.runtime);
    const auto direct = exec::ExecuteCanonicalWindowRanking(ranking);
    exec::CanonicalWindowRuntimeRequest request;
    request.descriptor = RuntimeDescriptor(item.runtime);
    request.ranking = ranking;
    request.forced_strategy = exec::CanonicalWindowRuntimeStrategy::ranking;
    passed &= Require401(
        direct.diagnostic.ok &&
            RuntimeResultAccepted(exec::ExecuteCanonicalWindowRuntime(request),
                                  *request.forced_strategy, direct.values),
        "ranking strategy diverged behind the canonical runtime");
  }
  return passed;
}

bool ValidateValueStrategies() {
  struct Case {
    exec::CanonicalWindowRuntimeFunction runtime;
    exec::CanonicalWindowValueFunction value;
  };
  const std::vector<Case> cases = {
      {exec::CanonicalWindowRuntimeFunction::lag,
       exec::CanonicalWindowValueFunction::lag},
      {exec::CanonicalWindowRuntimeFunction::lead,
       exec::CanonicalWindowValueFunction::lead},
      {exec::CanonicalWindowRuntimeFunction::first_value,
       exec::CanonicalWindowValueFunction::first_value},
      {exec::CanonicalWindowRuntimeFunction::last_value,
       exec::CanonicalWindowValueFunction::last_value},
      {exec::CanonicalWindowRuntimeFunction::nth_value,
       exec::CanonicalWindowValueFunction::nth_value},
  };
  bool passed = true;
  for (const auto& item : cases) {
    auto value = ValueRequest(item.value);
    if (item.value == exec::CanonicalWindowValueFunction::nth_value) {
      value.nth_values = RepeatedOperand(
          value.frames.ordered_batch.rows.size(), "int64", "2", 6700);
      value.nth_origin = exec::CanonicalWindowNthOrigin::from_first;
      value.null_treatment =
          exec::CanonicalWindowNullTreatment::respect_nulls;
    }
    const auto direct = exec::ExecuteCanonicalWindowValue(value);
    exec::CanonicalWindowRuntimeRequest request;
    request.descriptor = RuntimeDescriptor(item.runtime);
    request.value = value;
    request.forced_strategy = exec::CanonicalWindowRuntimeStrategy::value;
    passed &= Require401(
        direct.diagnostic.ok &&
            RuntimeResultAccepted(exec::ExecuteCanonicalWindowRuntime(request),
                                  *request.forced_strategy, direct.values),
        "value strategy diverged behind the canonical runtime");
  }
  return passed;
}

bool ValidateAggregateStrategyAndRefusals() {
  auto aggregate = RegistrySumWindowRequest();
  const auto direct =
      exec::ExecuteCanonicalRegistryWindowAggregate(aggregate);
  exec::CanonicalWindowRuntimeRequest request;
  request.descriptor =
      RuntimeAggregateDescriptor(exec::CanonicalAggregateFunction::sum);
  request.registry_aggregate = aggregate;
  request.forced_strategy = exec::CanonicalWindowRuntimeStrategy::aggregate;
  auto runtime = exec::ExecuteCanonicalWindowRuntime(request);
  bool passed = Require401(
      direct.diagnostic.ok &&
          RuntimeResultAccepted(runtime, *request.forced_strategy,
                                direct.values) &&
          runtime.aggregate_registry_bridge_used &&
          runtime.effective_frame_recomputed &&
          !runtime.moving_inverse_state_used &&
          runtime.aggregate_state_strategy_selected_from_physical_plan &&
          runtime.selected_aggregate_state_strategy ==
              exec::CanonicalRegistryWindowAggregateStateStrategy::frame_recompute &&
          runtime.selected_aggregate_state_implementation_id ==
              "window.aggregate-registry-frame-recompute.v1" &&
          runtime.aggregate_transition_count == direct.transition_count,
      "SUM seed did not adopt the aggregate-registry window bridge");

  aggregate = RegistrySumWindowRequest();
  const auto real_payload_descriptor = WindowDescriptor(
      5103, "real64",
      "type_uuid=" + WindowUuid(5203) + ";nullability=nullable");
  aggregate.frames.ordered_batch.columns[4].descriptor =
      real_payload_descriptor;
  for (auto& row : aggregate.frames.ordered_batch.rows) {
    row.values[4].descriptor = real_payload_descriptor;
  }
  aggregate.aggregate_template.result_column =
      RegistryAverageWindowRequest().aggregate_template.result_column;
  aggregate.aggregate_template.result_column.stable_name = "window_sum_real";
  const auto broad_sum_direct =
      exec::ExecuteCanonicalRegistryWindowAggregate(aggregate);
  request = {};
  request.descriptor =
      RuntimeAggregateDescriptor(exec::CanonicalAggregateFunction::sum);
  request.registry_aggregate = aggregate;
  request.forced_strategy = exec::CanonicalWindowRuntimeStrategy::aggregate;
  runtime = exec::ExecuteCanonicalWindowRuntime(request);
  passed &= Require401(
      broad_sum_direct.diagnostic.ok &&
          RuntimeResultAccepted(runtime, *request.forced_strategy,
                                broad_sum_direct.values) &&
          runtime.aggregate_registry_bridge_used &&
          runtime.effective_frame_recomputed,
      "generic SUM identity did not admit its broader result profile");

  request.descriptor =
      RuntimeAggregateDescriptor(exec::CanonicalAggregateFunction::sum);
  request.descriptor.function_uuid =
      RuntimeAggregateDescriptor(exec::CanonicalAggregateFunction::avg)
          .function_uuid;
  auto refused = exec::ExecuteCanonicalWindowRuntime(request);
  passed &= Require401(
      !refused.diagnostic.ok &&
          refused.diagnostic.diagnostic_code ==
              "QOW-DIAG-WINDOW-FUNCTION-DESCRIPTOR" &&
          refused.values.empty(),
      "aggregate window admitted a cross-row registry identity");

  aggregate = RegistryAverageWindowRequest(PrefixFrame());
  SelectRegistryWindowStateStrategy(
      &aggregate,
      exec::CanonicalRegistryWindowAggregateStateStrategy::moving_inverse);
  const auto average_direct =
      exec::ExecuteCanonicalRegistryWindowAggregate(aggregate);
  request = {};
  request.descriptor =
      RuntimeAggregateDescriptor(exec::CanonicalAggregateFunction::avg);
  request.registry_aggregate = aggregate;
  request.forced_strategy = exec::CanonicalWindowRuntimeStrategy::aggregate;
  runtime = exec::ExecuteCanonicalWindowRuntime(request);
  passed &= Require401(
      average_direct.diagnostic.ok &&
          RuntimeResultAccepted(runtime, *request.forced_strategy,
                                average_direct.values) &&
          runtime.aggregate_registry_bridge_used &&
          runtime.moving_inverse_state_used &&
          !runtime.effective_frame_recomputed &&
          runtime.aggregate_state_strategy_selected_from_physical_plan &&
          runtime.selected_aggregate_state_strategy ==
              exec::CanonicalRegistryWindowAggregateStateStrategy::moving_inverse &&
          runtime.selected_aggregate_state_implementation_id ==
              "window.aggregate-registry-moving-inverse.v1" &&
          runtime.aggregate_transition_count == 9 &&
          runtime.aggregate_inverse_transition_count == 8,
      "generic AVG identity did not reach moving state through the unified runtime");

  aggregate = RegistryMinimumWindowRequest();
  const auto minimum_direct =
      exec::ExecuteCanonicalRegistryWindowAggregate(aggregate);
  request = {};
  request.descriptor =
      RuntimeAggregateDescriptor(exec::CanonicalAggregateFunction::min);
  request.registry_aggregate = aggregate;
  request.forced_strategy = exec::CanonicalWindowRuntimeStrategy::aggregate;
  runtime = exec::ExecuteCanonicalWindowRuntime(request);
  passed &= Require401(
      minimum_direct.diagnostic.ok &&
          RuntimeResultAccepted(runtime, *request.forced_strategy,
                                minimum_direct.values) &&
          runtime.aggregate_registry_bridge_used &&
          runtime.effective_frame_recomputed &&
          !runtime.moving_inverse_state_used &&
          runtime.aggregate_inverse_transition_count == 0,
      "generic MIN identity did not retain canonical frame recomputation");

  const auto spill_root = std::filesystem::temp_directory_path() /
                          ("scratchbird_qow406_window_runtime_spill_" +
                           std::to_string(std::chrono::steady_clock::now()
                                              .time_since_epoch()
                                              .count()));
  std::error_code filesystem_error;
  std::filesystem::create_directories(
      spill_root / WindowUuid(5301), filesystem_error);
  const auto spill_expected =
      exec::ExecuteCanonicalRegistryWindowAggregate(
          RegistryAverageWindowRequest(PrefixFrame()));
  auto spill_payload = RegistryWindowSpillRequest(spill_root);
  request = {};
  request.descriptor =
      RuntimeAggregateDescriptor(exec::CanonicalAggregateFunction::avg);
  request.registry_aggregate_spill = spill_payload;
  request.forced_strategy = exec::CanonicalWindowRuntimeStrategy::aggregate;
  runtime = exec::ExecuteCanonicalWindowRuntime(request);
  passed &= Require401(
      spill_expected.diagnostic.ok &&
          RuntimeResultAccepted(runtime, *request.forced_strategy,
                                spill_expected.values) &&
          runtime.aggregate_registry_bridge_used &&
          runtime.aggregate_state_spill_used &&
          runtime.aggregate_spill_reopened &&
          runtime.aggregate_spill_cleanup_proven &&
          runtime.aggregate_spilled_state_count == 9 &&
          runtime.aggregate_serialized_state_bytes != 0 &&
          runtime.aggregate_spilled_state_record_count != 0 &&
          runtime.aggregate_state_strategy_selected_from_physical_plan &&
          runtime.selected_aggregate_state_strategy ==
              exec::CanonicalRegistryWindowAggregateStateStrategy::state_spill &&
          runtime.selected_aggregate_state_implementation_id ==
              "window.aggregate-registry-state-spill.v1" &&
          !HasWindowSpillArtifact(spill_root),
      "unified runtime did not consume the optimizer-selected spill implementation");
  std::filesystem::remove_all(spill_root, filesystem_error);

  request.forced_strategy = exec::CanonicalWindowRuntimeStrategy::ranking;
  refused = exec::ExecuteCanonicalWindowRuntime(request);
  passed &= Require401(
      !refused.diagnostic.ok &&
          refused.diagnostic.diagnostic_code ==
              "QOW-DIAG-WINDOW-STRATEGY" &&
          refused.values.empty() &&
          refused.executed_strategy ==
              exec::CanonicalWindowRuntimeStrategy::unknown,
      "incompatible forced strategy entered window execution");

  request.forced_strategy = exec::CanonicalWindowRuntimeStrategy::aggregate;
  request.ranking = RuntimeRankingRequest(
      exec::CanonicalWindowRankingFunction::row_number,
      exec::CanonicalWindowRuntimeFunction::row_number);
  refused = exec::ExecuteCanonicalWindowRuntime(request);
  passed &= Require401(
      !refused.diagnostic.ok &&
          refused.diagnostic.diagnostic_code ==
              "QOW-DIAG-WINDOW-RUNTIME-PAYLOAD" &&
          refused.values.empty(),
      "multiple strategy payloads entered window execution");

  request = {};
  request.descriptor =
      RuntimeAggregateDescriptor(exec::CanonicalAggregateFunction::avg);
  request.descriptor.aggregate_function =
      exec::CanonicalAggregateFunction::min;
  request.registry_aggregate = RegistryAverageWindowRequest();
  refused = exec::ExecuteCanonicalWindowRuntime(request);
  passed &= Require401(
      !refused.diagnostic.ok &&
          refused.diagnostic.diagnostic_code ==
              "QOW-DIAG-WINDOW-FUNCTION-DESCRIPTOR" &&
          refused.values.empty(),
      "aggregate enum drift selected an aggregate-as-window identity");

  request = {};
  request.descriptor =
      RuntimeAggregateDescriptor(exec::CanonicalAggregateFunction::avg);
  request.registry_aggregate = RegistrySumWindowRequest();
  refused = exec::ExecuteCanonicalWindowRuntime(request);
  passed &= Require401(
      !refused.diagnostic.ok &&
          refused.diagnostic.diagnostic_code ==
              "QOW-DIAG-WINDOW-RUNTIME-PAYLOAD" &&
          refused.values.empty(),
      "SUM payload impersonated the generic AVG runtime descriptor");
  return passed;
}

}  // namespace

#ifndef QOW_WIN_002_FIXTURE_ONLY
// QOW-TEST-WIN-002-V1
int main() {
  return ValidateRuntimeRegistry() && ValidateRankingStrategies() &&
                 ValidateValueStrategies() &&
                 ValidateAggregateStrategyAndRefusals()
             ? EXIT_SUCCESS
             : EXIT_FAILURE;
}
#endif
