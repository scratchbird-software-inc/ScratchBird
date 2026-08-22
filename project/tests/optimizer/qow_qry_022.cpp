// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#define QOW_OPT_017_FIXTURE_ONLY
#include "qow_opt_017.cpp"

#include <cstdlib>
#include <functional>
#include <iostream>
#include <limits>
#include <string_view>

namespace {

bool Require022(const bool condition, const std::string_view detail) {
  if (!condition) std::cerr << "QOW-TEST-QRY-022-V1: " << detail << '\n';
  return condition;
}

exec::CanonicalExecutionMgaAuthority DispatchAuthority(
    const exec::TypedPhysicalNodeDag& dag) {
  exec::CanonicalExecutionMgaAuthority authority;
  authority.origin = exec::CanonicalMgaAuthorityOrigin::kClosureTestSeam;
  authority.statement_context = dag.mga_statement_context;
  const auto current = authority.statement_context;
  authority.resolve_current = [current] {
    exec::CanonicalMgaCurrentResolution result;
    result.statement_context = current;
    return result;
  };
  return authority;
}

exec::DescriptorBatch NodeBatch(const exec::PhysicalNodeRecord& node) {
  std::vector<exec::ExecutorColumnDescriptor> columns;
  exec::DescriptorTuple row;
  for (const auto descriptor_id : node.output_descriptor_ids) {
    auto descriptor = exec::MakeExecutorDescriptor(
        "int64", "type_uuid=" + Uuid(20'000 + descriptor_id) +
                     ";nullability=non_null");
    descriptor.descriptor_uuid.canonical = Uuid(30'000 + descriptor_id);
    descriptor.descriptor_kind = "scalar";
    columns.push_back({"value_" + std::to_string(descriptor_id), descriptor,
                       false, descriptor_id});
    row.values.push_back(exec::MakeExecutorValue(descriptor, "1"));
  }
  return exec::MakeDescriptorBatch(std::move(columns), {{std::move(row)}});
}

exec::CanonicalPhysicalNodeRuntimeObservation ProducerRuntime() {
  auto runtime = Runtime(0);
  runtime.elapsed_ns = {};
  runtime.dispatcher_elapsed_frozen = false;
  runtime.counters_frozen_after_finish = false;
  return runtime;
}

std::vector<exec::CanonicalPhysicalExecutorRegistration> Registrations(
    const exec::TypedPhysicalNodeDag& dag,
    std::size_t* callback_count,
    const bool publishes_observation,
    const bool forge_elapsed = false,
    std::function<void(exec::CanonicalPhysicalDispatchStepResult&)>
        mutate_step = {}) {
  std::vector<exec::CanonicalPhysicalExecutorRegistration> registrations;
  for (const auto& node : dag.nodes) {
    exec::CanonicalPhysicalExecutorRegistration registration;
    registration.node_kind = node.node_kind;
    registration.implementation_id = node.implementation_id;
    registration.executor_capability_uuid = node.executor_capability_uuid;
    registration.executor_capability_abi_version =
        node.executor_capability_abi_version;
    registration.engine_owned = true;
    registration.accepts_optimizer_publication_v2 = true;
    registration.publishes_runtime_observation_v1 = publishes_observation;
    registration.execute =
        [callback_count, publishes_observation, forge_elapsed, mutate_step](
            const exec::TypedPhysicalNodeDag& selected,
            const exec::PhysicalNodeRecord& selected_node,
            const std::vector<exec::CanonicalPhysicalDispatchInput>& inputs) {
          ++*callback_count;
          exec::CanonicalPhysicalDispatchStepResult step;
          step.selected_plan_uuid = selected.selected_plan_uuid;
          step.executed_physical_node_id = selected_node.physical_node_id;
          step.causal_counter_id = selected_node.causal_counter_id;
          step.result_handle_id = selected_node.physical_node_id;
          step.output_descriptor_ids = selected_node.output_descriptor_ids;
          step.authority.engine_mga_snapshot_bound = true;
          step.input_row_count = inputs.size();
          step.output_row_count = 1;
          step.rows_examined = inputs.size();
          step.materialized_output_batch = NodeBatch(selected_node);
          step.data_access_observation_known = true;
          step.data_access_observed = false;
          step.mga_statement_context = selected.mga_statement_context;
          if (publishes_observation) {
            step.runtime_observation = ProducerRuntime();
          }
          if (forge_elapsed) {
            step.runtime_observation.elapsed_ns = {
                exec::CanonicalRuntimeMetricState::kObserved, 999'999};
            step.runtime_observation.dispatcher_elapsed_frozen = true;
          }
          if (mutate_step) mutate_step(step);
          return step;
        };
    registrations.push_back(std::move(registration));
  }
  return registrations;
}

exec::CanonicalPhysicalDagDispatchResult Execute(
    const exec::TypedPhysicalNodeDag& dag,
    std::size_t* callback_count,
    const bool publishes_observation,
    const bool forge_elapsed = false,
    std::function<void(exec::CanonicalPhysicalDispatchStepResult&)>
        mutate_step = {}) {
  exec::CanonicalPhysicalDagDispatchRequest request;
  request.physical_dag = dag;
  request.mga_authority = DispatchAuthority(dag);
  request.available_executors = Registrations(
      dag, callback_count, publishes_observation, forge_elapsed,
      std::move(mutate_step));
  return exec::ExecuteCanonicalPhysicalDag(request);
}

bool ValidateIdentityBridge() {
  auto prepare_request = ExplainPrepareRequest();
  cache::CanonicalPreparedPlanStore store;
  const auto prepared =
      cache::PrepareCanonicalPhysicalPlan(prepare_request, &store);
  if (!Require022(prepared.accepted && prepared.prepared_plan,
                  "prepared identity fixture was rejected")) {
    return false;
  }
  std::size_t callback_count = 0;
  auto dispatch = Execute(prepare_request.selected_physical_dag,
                          &callback_count, true);
  bool passed = true;
  passed &= Require022(
      dispatch.diagnostic.ok && dispatch.execution_started &&
          dispatch.executed_steps.size() ==
              prepare_request.selected_physical_dag.nodes.size() &&
          callback_count == prepare_request.selected_physical_dag.nodes.size() &&
          std::ranges::all_of(dispatch.executed_steps, [](const auto& step) {
            return step.runtime_observation.elapsed_ns.state ==
                       exec::CanonicalRuntimeMetricState::kObserved &&
                   step.runtime_observation.dispatcher_elapsed_frozen &&
                   step.runtime_observation.counters_frozen_after_finish;
          }),
      "dispatcher did not freeze producer actuals on every selected node");

  cache::CanonicalExplainRequest analyze;
  analyze.mode = cache::CanonicalExplainMode::kAnalyze;
  analyze.prepared_plan = prepared.prepared_plan;
  analyze.completed_dispatch = &dispatch;
  analyze.completed_result_schema_uuid =
      prepared.prepared_plan->result_schema_uuid;
  analyze.engine_result_schema_evidence = true;
  analyze.mga_authority.origin =
      exec::CanonicalMgaAuthorityOrigin::kClosureTestSeam;
  analyze.mga_authority.statement_context = dispatch.mga_statement_context;
  analyze.disclosure = FullDisclosure(*prepared.prepared_plan);
  const auto rendered = api::RenderCanonicalStoredPlanExplain(analyze);
  passed &= Require022(
      rendered.accepted && rendered.analyzed &&
          rendered.document.selected_plan_uuid == dispatch.selected_plan_uuid &&
          rendered.document.root_physical_node_id ==
              dispatch.executed_root_physical_node_id &&
          rendered.document.nodes.size() == dispatch.executed_steps.size(),
      "stored/selected/dispatched/actual identity bridge was not exact");
  if (rendered.accepted) {
    for (std::size_t index = 0; index < rendered.document.nodes.size(); ++index) {
      const auto& node = rendered.document.nodes[index];
      const auto& step = dispatch.executed_steps[index];
      passed &= Require022(
          node.physical_node_id == step.executed_physical_node_id &&
              node.logical_node_id == step.executed_relational_node_id &&
              node.causal_counter_id == step.causal_counter_id &&
              node.execution_ordinal == step.execution_ordinal &&
              node.input_physical_node_ids ==
                  step.executed_input_physical_node_ids,
          "node logical/physical/causal/order identity drifted");
    }
  }

  const auto callbacks_before_plain = callback_count;
  cache::CanonicalExplainRequest plain = analyze;
  plain.mode = cache::CanonicalExplainMode::kPlain;
  plain.completed_dispatch = nullptr;
  const auto plain_result = api::RenderCanonicalStoredPlanExplain(plain);
  passed &= Require022(
      plain_result.accepted && !plain_result.analyzed &&
          callback_count == callbacks_before_plain,
      "plain EXPLAIN invoked an executor callback");

  const auto expect_identity_refusal = [&](auto mutation,
                                            const std::string_view detail) {
    auto changed = dispatch;
    mutation(changed);
    auto request = analyze;
    request.completed_dispatch = &changed;
    const auto result = api::RenderCanonicalStoredPlanExplain(request);
    return Require022(!result.accepted && result.document.nodes.empty() &&
                          result.issues.front().diagnostic_id ==
                              "QOW-DIAG-OPT-017-REFUSAL-V1",
                      detail);
  };
  passed &= expect_identity_refusal(
      [](auto& value) { value.executed_steps.front().causal_counter_id++; },
      "causal identity mutation reached ANALYZE");
  passed &= expect_identity_refusal(
      [](auto& value) {
        value.executed_steps.back().executed_relational_node_id++;
      },
      "logical identity mutation reached ANALYZE");
  passed &= expect_identity_refusal(
      [](auto& value) {
        value.executed_steps.front().executed_physical_node_id++;
      },
      "physical identity mutation reached ANALYZE");
  passed &= expect_identity_refusal(
      [](auto& value) {
        value.executed_steps.back().executed_input_physical_node_ids.clear();
      },
      "causal input mutation reached ANALYZE");
  passed &= expect_identity_refusal(
      [](auto& value) { value.executed_steps.front().execution_ordinal++; },
      "execution-order mutation reached ANALYZE");
  passed &= expect_identity_refusal(
      [](auto& value) { value.selected_plan_uuid = Uuid(40'001); },
      "selected-plan mutation reached ANALYZE");
  passed &= expect_identity_refusal(
      [](auto& value) {
        value.executed_steps.front().result_handle_id = 0;
      },
      "zero result handle reached ANALYZE");
  passed &= expect_identity_refusal(
      [](auto& value) { value.root_result_handle_id++; },
      "root result handle mutation reached ANALYZE");
  passed &= expect_identity_refusal(
      [](auto& value) {
        value.executed_steps.front().authority.owns_transaction_finality =
            true;
      },
      "forbidden step authority reached ANALYZE");
  passed &= expect_identity_refusal(
      [](auto& value) {
        value.mga_statement_context.statement_snapshot_uuid = Uuid(40'002);
      },
      "MGA context mutation reached ANALYZE");
  passed &= expect_identity_refusal(
      [](auto& value) {
        value.executed_steps.front()
            .mga_statement_context.statement_snapshot_uuid = Uuid(40'003);
      },
      "per-step MGA context mutation reached ANALYZE");

  std::size_t legacy_count = 0;
  auto legacy = Execute(prepare_request.selected_physical_dag,
                        &legacy_count, false);
  auto legacy_request = analyze;
  legacy_request.completed_dispatch = &legacy;
  const auto legacy_rendered =
      api::RenderCanonicalStoredPlanExplain(legacy_request);
  passed &= Require022(
      legacy.diagnostic.ok && legacy_count ==
                                  prepare_request.selected_physical_dag.nodes.size() &&
          !legacy_rendered.accepted,
      "legacy executor compatibility or fail-closed ANALYZE drifted");

  std::size_t forged_count = 0;
  const auto forged = Execute(prepare_request.selected_physical_dag,
                              &forged_count, true, true);
  passed &= Require022(
      !forged.diagnostic.ok && forged.diagnostic.diagnostic_code ==
                                   "QOW-DIAG-OPT-017-REFUSAL-V1",
      "producer-forged elapsed/lifecycle evidence was normalized");

  const auto observed = [](const std::uint64_t value) {
    return exec::CanonicalObservedUint64{
        exec::CanonicalRuntimeMetricState::kObserved, value};
  };
  const auto expect_runtime_refusal =
      [&](std::function<void(exec::CanonicalPhysicalDispatchStepResult&)>
              mutation,
          const std::string_view expected_detail,
          const std::string_view failure_detail) {
        std::size_t refusal_callback_count = 0;
        const auto refused = Execute(prepare_request.selected_physical_dag,
                                     &refusal_callback_count, true, false,
                                     std::move(mutation));
        return Require022(
            !refused.diagnostic.ok &&
                refused.diagnostic.diagnostic_code ==
                    "QOW-DIAG-OPT-017-REFUSAL-V1" &&
                refused.diagnostic.detail == expected_detail &&
                refusal_callback_count == 1,
            failure_detail);
      };
  passed &= expect_runtime_refusal(
      [](auto& step) { step.runtime_observation.operator_wait_ns = {}; },
      "runtime_observation_required_metric",
      "dispatcher accepted a missing required runtime metric");
  passed &= expect_runtime_refusal(
      [observed](auto& step) {
        step.runtime_observation.operator_wait_ns =
            observed(std::numeric_limits<std::uint64_t>::max());
      },
      "runtime_observation_resource_bounds",
      "dispatcher accepted operator wait greater than elapsed time");
  passed &= expect_runtime_refusal(
      [observed, budget = prepare_request.selected_physical_dag
                              .memory_budget_bytes](auto& step) {
        step.runtime_observation.current_memory_bytes = observed(budget);
        step.runtime_observation.peak_memory_bytes = observed(0);
      },
      "runtime_observation_resource_bounds",
      "dispatcher accepted current memory greater than peak memory");
  passed &= expect_runtime_refusal(
      [observed, budget = prepare_request.selected_physical_dag
                              .memory_budget_bytes](auto& step) {
        step.runtime_observation.current_memory_bytes = observed(0);
        step.runtime_observation.peak_memory_bytes = observed(budget + 1);
      },
      "runtime_observation_resource_bounds",
      "dispatcher accepted peak memory greater than the selected budget");
  passed &= expect_runtime_refusal(
      [observed](auto& step) {
        step.runtime_observation.spill_bytes_read =
            observed(std::numeric_limits<std::uint64_t>::max());
        step.runtime_observation.spill_bytes_written = observed(1);
      },
      "runtime_observation_spill_overflow",
      "dispatcher accepted overflowing spill counters");
  passed &= expect_runtime_refusal(
      [observed](auto& step) {
        step.runtime_observation.spill_bytes_written = observed(1);
      },
      "runtime_observation_legacy_counter_mismatch",
      "dispatcher accepted runtime/legacy spill counter mismatch");
  passed &= expect_runtime_refusal(
      [observed](auto& step) {
        step.runtime_observation.bytes_read = observed(1);
      },
      "runtime_observation_zero_access_contradiction",
      "dispatcher accepted nonzero I/O on a proved no-access step");
  return passed;
}

}  // namespace

// QOW-TEST-QRY-022-V1
int main() {
  return ValidateIdentityBridge() ? EXIT_SUCCESS : EXIT_FAILURE;
}
