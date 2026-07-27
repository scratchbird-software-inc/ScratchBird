// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#define QOW_QRY_011_STATE_SPILL_FIXTURE_ONLY
#include "qow_qry_011_state_spill.cpp"

#include <numeric>

namespace {

exec::CanonicalAggregateStateExchangeRequest StateExchangeRequest(
    const exec::CanonicalAggregateFunction function,
    const std::size_t worker_count = 3) {
  exec::CanonicalAggregateStateExchangeRequest request;
  request.aggregate_request = SpillProfile(function);
  request.aggregate_request.forced_strategy =
      exec::CanonicalAggregateExecutionStrategy::partitioned_combine;
  request.aggregate_request.physical_dag.nodes.back().implementation_id =
      "aggregate.registry-state-exchange.v1";
  request.worker_ordinals.reserve(worker_count);
  for (std::size_t worker = 0; worker < worker_count; ++worker) {
    request.worker_ordinals.push_back(static_cast<std::uint32_t>(worker));
  }
  request.exchange_generation = 4101;
  request.coordinator_exchange_generation = 4101;
  return request;
}

exec::CanonicalAggregateRuntimeResult LocalCombineBaseline(
    exec::CanonicalAggregateRuntimeRequest request) {
  request.physical_dag.nodes.back().implementation_id =
      "aggregate.registry-core.v1";
  return exec::ExecuteCanonicalAggregateRuntime(request);
}

bool ValidateAllRegistryStateExchangeProfiles() {
  bool passed = true;
  const auto registry = exec::CanonicalAggregateRuntimeRegistryV1();
  std::size_t admitted = 0;
  for (const auto& entry : registry) {
    const auto request = StateExchangeRequest(entry.function);
    const auto baseline = LocalCombineBaseline(request.aggregate_request);
    const auto result =
        exec::ExecuteCanonicalAggregateStateExchange(request);
    const auto exchanged_transitions = std::accumulate(
        result.worker_transition_counts.begin(),
        result.worker_transition_counts.end(), std::size_t{0});
    const bool accepted =
        result.diagnostic.ok && result.states_serialized &&
        result.exchange_identity_proven && result.all_states_restored &&
        result.deterministic_merge_order_proven &&
        result.merged_result_equivalent &&
        result.partial_state_count == 3 &&
        result.restored_partial_state_count == 3 &&
        result.merged_partial_state_count == 3 &&
        result.serialized_state_bytes != 0 &&
        result.worker_transition_counts.size() == 3 &&
        result.worker_state_bytes.size() == 3 &&
        result.worker_serialized_state_bytes.size() == 3 &&
        std::all_of(result.worker_serialized_state_bytes.begin(),
                    result.worker_serialized_state_bytes.end(),
                    [](const auto bytes) { return bytes != 0; }) &&
        exchanged_transitions == baseline.transition_count &&
        result.aggregate_result.descriptor.function == entry.function &&
        result.aggregate_result.executed_strategy ==
            exec::CanonicalAggregateExecutionStrategy::partitioned_combine &&
        result.aggregate_result.shared_state_authority_used &&
        result.aggregate_result.authority.engine_mga_snapshot_bound &&
        !result.aggregate_result.authority.owns_transaction_finality &&
        !result.aggregate_result.authority.owns_recovery &&
        SameScalar(baseline, result.aggregate_result);
    passed &= Require(
        accepted,
        "aggregate registry state did not cross the worker exchange exactly");
    if (accepted) ++admitted;
  }
  passed &= Require(admitted == 43,
                    "not every aggregate registry profile exchanged state");

  auto modifiers = StateExchangeRequest(
      exec::CanonicalAggregateFunction::sum, 4);
  modifiers.aggregate_request.distinct = true;
  modifiers.aggregate_request.filter_truth_values =
      std::vector<api::EngineSqlTruthValue>{
          api::EngineSqlTruthValue::true_value,
          api::EngineSqlTruthValue::true_value,
          api::EngineSqlTruthValue::true_value,
          api::EngineSqlTruthValue::unknown};
  const auto modifier_baseline =
      LocalCombineBaseline(modifiers.aggregate_request);
  const auto modified =
      exec::ExecuteCanonicalAggregateStateExchange(modifiers);
  passed &= Require(
      modified.diagnostic.ok && modified.partial_state_count == 4 &&
          modified.aggregate_result.filter_applied_before_distinct &&
          modified.aggregate_result.distinct_tuple_count == 2 &&
          modified.aggregate_result.transition_count == 2 &&
          modified.aggregate_result.output_batch.rows[0].values[0]
                  .encoded_value == "3" &&
          SameScalar(modifier_baseline, modified.aggregate_result),
      "FILTER/DISTINCT exchange diverged from shared transition preparation");

  auto ordered = StateExchangeRequest(
      exec::CanonicalAggregateFunction::array_agg, 3);
  const auto ordered_baseline = LocalCombineBaseline(ordered.aggregate_request);
  const auto ordered_result =
      exec::ExecuteCanonicalAggregateStateExchange(ordered);
  passed &= Require(
      ordered_result.diagnostic.ok &&
          ordered_result.aggregate_result.aggregate_order_applied &&
          SameScalar(ordered_baseline, ordered_result.aggregate_result),
      "ordered aggregate exchange did not preserve global transition order");

  auto empty = StateExchangeRequest(exec::CanonicalAggregateFunction::sum, 4);
  empty.aggregate_request.input_batch.rows.clear();
  const auto empty_result =
      exec::ExecuteCanonicalAggregateStateExchange(empty);
  passed &= Require(
      empty_result.diagnostic.ok && empty_result.partial_state_count == 4 &&
          std::all_of(empty_result.worker_transition_counts.begin(),
                      empty_result.worker_transition_counts.end(),
                      [](const auto count) { return count == 0; }) &&
          empty_result.aggregate_result.output_batch.rows[0].values[0].state ==
              api::EngineValueState::sql_null,
      "empty aggregate did not exchange explicit empty partial states");
  return passed;
}

bool ValidateRegistryStateExchangeRefusals() {
  bool passed = true;
  auto request = StateExchangeRequest(exec::CanonicalAggregateFunction::sum);

  auto direct = exec::ExecuteCanonicalAggregateRuntime(
      request.aggregate_request);
  passed &= Require(
      !direct.diagnostic.ok &&
          direct.diagnostic.diagnostic_code ==
              "QOW-DIAG-QRY-011-REGISTRY-STATE-EXCHANGE-STRATEGY-V1" &&
          direct.output_batch.rows.empty(),
      "direct aggregate runtime consumed an exchange-selected plan");

  auto mismatch = request;
  mismatch.aggregate_request.physical_dag.nodes.back().implementation_id =
      "aggregate.registry-core.v1";
  auto result = exec::ExecuteCanonicalAggregateStateExchange(mismatch);
  passed &= Require(!result.diagnostic.ok &&
                        result.aggregate_result.output_batch.rows.empty(),
                    "exchange payload overrode the selected implementation");

  auto serial = request;
  serial.aggregate_request.forced_strategy =
      exec::CanonicalAggregateExecutionStrategy::serial;
  result = exec::ExecuteCanonicalAggregateStateExchange(serial);
  passed &= Require(!result.diagnostic.ok && !result.states_serialized,
                    "serial aggregate was relabelled as a state exchange");

  auto one_worker = StateExchangeRequest(
      exec::CanonicalAggregateFunction::sum, 1);
  result = exec::ExecuteCanonicalAggregateStateExchange(one_worker);
  passed &= Require(!result.diagnostic.ok && !result.states_serialized,
                    "single-worker execution was relabelled as exchange");

  auto duplicate = request;
  duplicate.worker_ordinals = {0, 0, 2};
  result = exec::ExecuteCanonicalAggregateStateExchange(duplicate);
  passed &= Require(!result.diagnostic.ok && !result.exchange_identity_proven,
                    "duplicate worker exchange identity was accepted");

  auto stale = request;
  stale.coordinator_exchange_generation = stale.exchange_generation + 1;
  result = exec::ExecuteCanonicalAggregateStateExchange(stale);
  passed &= Require(!result.diagnostic.ok && !result.exchange_identity_proven,
                    "stale aggregate exchange generation was accepted");

  auto too_many = request;
  too_many.maximum_partial_state_count = 2;
  result = exec::ExecuteCanonicalAggregateStateExchange(too_many);
  passed &= Require(!result.diagnostic.ok && !result.states_serialized,
                    "aggregate exchange exceeded its partial-state bound");

  auto worker_bytes = request;
  worker_bytes.maximum_serialized_state_bytes_per_worker = 1;
  result = exec::ExecuteCanonicalAggregateStateExchange(worker_bytes);
  passed &= Require(!result.diagnostic.ok && !result.states_serialized &&
                        result.aggregate_result.output_batch.rows.empty(),
                    "aggregate exchange exceeded its worker byte bound");

  const auto accepted =
      exec::ExecuteCanonicalAggregateStateExchange(request);
  auto combined_bytes = request;
  combined_bytes.maximum_combined_serialized_state_bytes =
      accepted.worker_serialized_state_bytes.front();
  result = exec::ExecuteCanonicalAggregateStateExchange(combined_bytes);
  passed &= Require(
      !result.diagnostic.ok && !result.states_serialized &&
          result.worker_serialized_state_bytes.size() == 1 &&
          result.aggregate_result.output_batch.rows.empty(),
      "partial-progress exchange byte exhaustion published aggregate output");

  auto cancelled = request;
  cancelled.cancellation_requested = true;
  result = exec::ExecuteCanonicalAggregateStateExchange(cancelled);
  passed &= Require(!result.diagnostic.ok && result.cancellation_observed &&
                        !result.states_serialized &&
                        result.aggregate_result.output_batch.rows.empty(),
                    "cancelled aggregate exchange published output");

  auto finality = request;
  finality.aggregate_request.transaction_finality_claimed = true;
  result = exec::ExecuteCanonicalAggregateStateExchange(finality);
  passed &= Require(!result.diagnostic.ok && !result.states_serialized &&
                        result.aggregate_result.output_batch.rows.empty(),
                    "aggregate exchange claimed transaction finality");
  return passed;
}

}  // namespace

int main() {
  return ValidateAllRegistryStateExchangeProfiles() &&
                 ValidateRegistryStateExchangeRefusals()
             ? EXIT_SUCCESS
             : EXIT_FAILURE;
}
