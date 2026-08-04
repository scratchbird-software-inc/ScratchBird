// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#define QOW_OPT_016_FIXTURE_ONLY
#include "qow_opt_016.cpp"
#include "query/plan_api.hpp"

#include <cstdlib>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>

namespace {

namespace api = scratchbird::engine::internal_api;

bool Require008(const bool condition, const std::string_view detail) {
  if (!condition) std::cerr << "QOW-TEST-OPT-008-V1: " << detail << '\n';
  return condition;
}

exec::CanonicalExecutionMgaAuthority EngineInventoryAuthority(
    const exec::TypedPhysicalNodeDag& dag) {
  exec::CanonicalExecutionMgaAuthority authority;
  authority.statement_context = dag.mga_statement_context;
  authority.origin =
      exec::CanonicalMgaAuthorityOrigin::kEngineTransactionInventory;
  const auto current = authority.statement_context;
  authority.resolve_current = [current] {
    exec::CanonicalMgaCurrentResolution resolution;
    resolution.statement_context = current;
    return resolution;
  };
  return authority;
}

struct StaleResolutionState008 {
  exec::PhysicalMgaStatementContext current;
  std::size_t resolution_count = 0;
  std::size_t stale_on_resolution = 0;
};

std::shared_ptr<StaleResolutionState008> RefuseAtResolution008(
    api::CanonicalOptimizerSelectedExecutionRequest* request,
    const std::size_t stale_on_resolution) {
  auto state = std::make_shared<StaleResolutionState008>();
  state->current = request->mga_authority.statement_context;
  state->stale_on_resolution = stale_on_resolution;
  request->mga_authority.resolve_current = [state] {
    exec::CanonicalMgaCurrentResolution resolution;
    ++state->resolution_count;
    resolution.statement_context = state->current;
    if (state->resolution_count >= state->stale_on_resolution) {
      resolution.statement_context.current = false;
    }
    return resolution;
  };
  return state;
}

bool RequireNoSelectedExposure008(
    const api::CanonicalOptimizerSelectedExecutionResult& result,
    const std::string_view phase) {
  return Require008(
      !result.accepted && !result.exact_selected_nodes_executed &&
          !result.canonical_result_published &&
          result.dispatch.executed_steps.empty() &&
          result.dispatch.root_result_handle_id == 0 &&
          result.runtime_actuals.node_actuals.empty() &&
          !result.runtime_actuals.accepted &&
          !result.result_publication.published &&
          result.result_publication.canonical_envelope_bytes.empty() &&
          result.issues.size() == 1 &&
          result.issues.front().diagnostic_id ==
              "QOW-DIAG-MGA-RUNTIME-CURRENT-V1",
      std::string(phase) +
          " stale authority exposed a step, root batch, runtime actual, or result");
}

api::EngineDescriptor RootResultDescriptor() {
  api::EngineDescriptor descriptor;
  descriptor.descriptor_uuid.canonical = Uuid(8201);
  descriptor.descriptor_kind = "scalar";
  descriptor.canonical_type_name = "int64";
  descriptor.encoded_descriptor =
      "type_uuid=" + Uuid(8202) + ";nullability=non_null";
  return descriptor;
}

exec::DescriptorBatch RootResultBatch() {
  const auto descriptor = RootResultDescriptor();
  const auto value = [&](const std::string& encoded) {
    api::EngineTypedValue typed;
    typed.descriptor = descriptor;
    typed.state = api::EngineValueState::value;
    typed.encoded_value = encoded;
    return typed;
  };
  return exec::MakeDescriptorBatch(
      {{"projected_id", descriptor, false, 104}},
      {{{value("7")}}, {{value("11")}}});
}

exec::CanonicalResultPublicationRequest ResultPublicationRequest() {
  exec::CanonicalResultPublicationRequest request;
  request.statement_uuid = Uuid(8210);
  request.execution_attempt_uuid = Uuid(8211);
  request.transaction_effect_evidence_uuid = Uuid(8212);
  request.selected_catalog_epoch_uuid = Uuid(8298);
  request.result_kind = exec::CanonicalResultKind::kRows;
  request.column_bindings = {{
      0,
      true,
      exec::CanonicalResultColumnDescriptor{
          0, "projected_id", Uuid(8201), Uuid(8202),
          exec::CanonicalResultNullability::kNonNull, std::nullopt,
          std::nullopt},
  }};
  return request;
}

exec::DescriptorBatch NodeBatch008(
    const exec::PhysicalNodeRecord& node,
    const std::size_t row_count) {
  std::vector<exec::ExecutorColumnDescriptor> columns;
  columns.reserve(node.output_descriptor_ids.size());
  for (const auto descriptor_id : node.output_descriptor_ids) {
    auto descriptor = exec::MakeExecutorDescriptor(
        "int64", "type_uuid=" + Uuid(8400 + descriptor_id) +
                     ";nullability=non_null");
    descriptor.descriptor_uuid.canonical = Uuid(8500 + descriptor_id);
    descriptor.descriptor_kind = "scalar";
    columns.push_back({"node_value_" + std::to_string(descriptor_id),
                       descriptor, false, descriptor_id});
  }
  std::vector<exec::DescriptorTuple> rows;
  rows.reserve(row_count);
  for (std::size_t row = 0; row < row_count; ++row) {
    exec::DescriptorTuple tuple;
    tuple.values.reserve(columns.size());
    for (const auto& column : columns) {
      tuple.values.push_back(
          exec::MakeExecutorValue(column.descriptor, std::to_string(row + 1)));
    }
    rows.push_back(std::move(tuple));
  }
  return exec::MakeDescriptorBatch(std::move(columns), std::move(rows));
}

exec::CanonicalPhysicalDispatchStepResult Step008(
    const exec::TypedPhysicalNodeDag& dag,
    const exec::PhysicalNodeRecord& node, const std::uint64_t result_handle,
    const std::uint64_t input_rows, const std::uint64_t output_rows,
    const std::uint64_t rows_examined, const std::uint64_t pages_read,
    const std::uint64_t spill_bytes = 0) {
  exec::CanonicalPhysicalDispatchStepResult result;
  result.selected_plan_uuid = dag.selected_plan_uuid;
  result.executed_physical_node_id = node.physical_node_id;
  result.causal_counter_id = node.causal_counter_id;
  result.result_handle_id = result_handle;
  result.output_descriptor_ids = node.output_descriptor_ids;
  result.authority.engine_mga_snapshot_bound = true;
  result.mga_statement_context = dag.mga_statement_context;
  result.input_row_count = input_rows;
  result.output_row_count = output_rows;
  result.rows_examined = rows_examined;
  result.pages_read = pages_read;
  result.spill_bytes = spill_bytes;
  if (result_handle != 0) {
    result.materialized_output_batch = NodeBatch008(node, output_rows);
  }
  return result;
}

exec::CanonicalPhysicalExecutorRegistration Registration(
    const exec::PhysicalNodeRecord& node,
    exec::CanonicalPhysicalNodeExecutor execute) {
  exec::CanonicalPhysicalExecutorRegistration registration;
  registration.node_kind = node.node_kind;
  registration.implementation_id = node.implementation_id;
  registration.execute = std::move(execute);
  registration.executor_capability_uuid = node.executor_capability_uuid;
  registration.executor_capability_abi_version =
      node.executor_capability_abi_version;
  registration.engine_owned = true;
  registration.accepts_optimizer_publication_v2 = true;
  return registration;
}

api::CanonicalOptimizerSelectedExecutionRequest SelectedExecutionRequest(
    std::vector<std::uint64_t>* invocation_order) {
  const auto inputs = Inputs();
  const auto publication = opt::PublishCanonicalPhysicalDag(
      inputs.request, inputs.admission, inputs.alternatives, inputs.search,
      inputs.capabilities, PublicationIdentity());
  api::CanonicalOptimizerSelectedExecutionRequest request;
  request.selected_physical_dag = publication.physical_dag;
  request.pre_access_statistics_snapshot_uuid =
      publication.physical_dag.statistics_snapshot_uuid;
  request.mga_authority = EngineInventoryAuthority(publication.physical_dag);
  request.engine_execution_authorized = true;
  request.result_publication_request = ResultPublicationRequest();
  request.result_publication_request.invocation_mode =
      exec::CanonicalResultInvocationMode::kPrepared;

  const auto* scan = PhysicalNode(publication.physical_dag, 1);
  const auto* values = PhysicalNode(publication.physical_dag, 2);
  const auto* join = PhysicalNode(publication.physical_dag, 3);
  const auto* project = PhysicalNode(publication.physical_dag, 4);
  request.available_executors.push_back(Registration(
      *project, [invocation_order](const auto& dag, const auto& node,
                                   const auto& inputs) {
        invocation_order->push_back(node.physical_node_id);
        if (inputs.size() != 1 || inputs[0].physical_node_id != 3 ||
            inputs[0].result_handle_id != 20'003) {
          auto result = Step008(dag, node, 0, 0, 0, 0, 0);
          result.diagnostic.ok = false;
          result.diagnostic.diagnostic_code = "TEST_PROJECT_INPUT_IDENTITY";
          return result;
        }
        auto result = Step008(dag, node, 20'004, 2, 2, 2, 0);
        result.materialized_output_batch = RootResultBatch();
        return result;
      }));
  request.available_executors.push_back(Registration(
      *scan, [invocation_order](const auto& dag, const auto& node,
                                const auto& inputs) {
        invocation_order->push_back(node.physical_node_id);
        if (!inputs.empty()) {
          auto result = Step008(dag, node, 0, 0, 0, 0, 0);
          result.diagnostic.ok = false;
          result.diagnostic.diagnostic_code = "TEST_SCAN_INPUT_IDENTITY";
          return result;
        }
        exec::CanonicalScanAccessRequest scan_request;
        scan_request.physical_dag = dag;
        scan_request.selected_physical_node_id = node.physical_node_id;
        scan_request.available_implementation_id = node.implementation_id;
        scan_request.relation_uuid = Uuid(9);
        scan_request.mga_authority = EngineInventoryAuthority(dag);
        scan_request.selected_descriptor_generation = 31;
        scan_request.current_descriptor_generation = 31;
        exec::CanonicalScanCandidateEvidence candidate;
        candidate.candidate_uuid = Uuid(910);
        candidate.record_uuid = Uuid(911);
        candidate.relation_uuid = Uuid(9);
        candidate.visibility_decision_uuid = Uuid(912);
        candidate.creator_local_transaction_id =
            dag.mga_statement_context.owning_local_transaction_id;
        candidate.row_version_id = 913;
        candidate.candidate_generation = 31;
        candidate.observed_generation = 31;
        candidate.source = exec::CanonicalScanCandidateSource::kRelationPage;
        candidate.visibility = exec::CanonicalMgaVisibilityDecision::kVisible;
        candidate.security_decision =
            exec::CanonicalMgaSecurityDecision::kAllowed;
        candidate.residual_truth = api::EngineSqlTruthValue::true_value;
        candidate.locator_identity_matches = true;
        scan_request.candidates = {std::move(candidate)};
        const auto scanned =
            exec::ExecuteCanonicalSelectedScanAccess(scan_request);
        if (!scanned.diagnostic.ok ||
            scanned.accepted_row_version_ids.size() != 1) {
          auto result = Step008(dag, node, 0, 0, 0, 0, 0);
          result.diagnostic = scanned.diagnostic;
          return result;
        }
        return Step008(dag, node, 20'001, 0, 1, 1, 1);
      }));
  request.available_executors.push_back(Registration(
      *join, [invocation_order](const auto& dag, const auto& node,
                                const auto& inputs) {
        invocation_order->push_back(node.physical_node_id);
        if (inputs.size() != 2 || inputs[0].physical_node_id != 1 ||
            inputs[0].result_handle_id != 20'001 ||
            inputs[1].physical_node_id != 2 ||
            inputs[1].result_handle_id != 20'002) {
          auto result = Step008(dag, node, 0, 0, 0, 0, 0);
          result.diagnostic.ok = false;
          result.diagnostic.diagnostic_code = "TEST_JOIN_INPUT_IDENTITY";
          return result;
        }
        return Step008(dag, node, 20'003, 3, 2, 2, 0);
      }));
  request.available_executors.push_back(Registration(
      *values, [invocation_order](const auto& dag, const auto& node,
                                  const auto& inputs) {
        invocation_order->push_back(node.physical_node_id);
        if (!inputs.empty()) {
          auto result = Step008(dag, node, 0, 0, 0, 0, 0);
          result.diagnostic.ok = false;
          result.diagnostic.diagnostic_code = "TEST_VALUES_INPUT_IDENTITY";
          return result;
        }
        return Step008(dag, node, 20'002, 0, 2, 2, 0);
      }));
  return request;
}

bool ValidateExactSelectedExecution() {
  std::vector<std::uint64_t> invocation_order;
  const auto request = SelectedExecutionRequest(&invocation_order);
  const auto result = api::ExecuteCanonicalOptimizerSelectedDag(request);
  bool passed = true;
  passed &= Require008(
      result.accepted && result.exact_selected_nodes_executed &&
          result.causal_counters_attached &&
          result.canonical_result_published && result.data_access_observed &&
          !result.replan_required && result.issues.empty(),
      "ABI-v2 selected physical DAG did not execute canonically");
  passed &= Require008(
      invocation_order == std::vector<std::uint64_t>({1, 2, 3, 4}) &&
          result.dispatch.executed_steps.size() == 4 &&
          result.dispatch.root_result_handle_id == 20'004 &&
          result.dispatch.selected_plan_uuid == Uuid(900),
      "executor did not consume the exact selected dependency graph");
  for (std::size_t index = 0;
       index < result.dispatch.executed_steps.size(); ++index) {
    const auto& step = result.dispatch.executed_steps[index];
    passed &= Require008(
        step.execution_ordinal == index + 1 && step.execution_started &&
            step.execution_finished && step.counters_captured_after_finish,
        "dispatcher did not capture ordered start/finish/counter evidence");
  }
  passed &= Require008(
      result.runtime_actuals.accepted &&
          result.runtime_actuals.post_execution_actuals &&
          result.runtime_actuals.planning_estimates_immutable &&
          !result.runtime_actuals.feedback_authorized &&
          result.runtime_actuals.selected_plan_uuid == Uuid(900) &&
          result.runtime_actuals.node_actuals.size() == 4 &&
          result.runtime_actuals.selected_plan_uuid ==
              request.selected_physical_dag.selected_plan_uuid &&
          request.selected_physical_dag.local_transaction_id == kOwner &&
          request.selected_physical_dag.statement_snapshot_id == 0 &&
          exec::PhysicalMgaStatementContextEqual(
              result.dispatch.mga_statement_context,
              request.mga_authority.statement_context) &&
          exec::PhysicalMgaStatementContextEqual(
              result.runtime_actuals.mga_statement_context,
              request.mga_authority.statement_context) &&
          std::ranges::all_of(
              result.dispatch.executed_steps, [&](const auto& step) {
                return exec::PhysicalMgaStatementContextEqual(
                    step.mga_statement_context,
                    request.mga_authority.statement_context);
              }),
      "runtime actuals were not attached to the selected physical identities");
  passed &= Require008(
      result.result_publication.published &&
          exec::PhysicalMgaStatementContextEqual(
              result.result_publication.envelope.mga_statement_context,
              request.mga_authority.statement_context) &&
          result.result_publication.envelope.statement_uuid ==
              request.mga_authority.statement_context.statement_uuid &&
          result.result_publication.envelope.catalog_epoch_uuid ==
              request.selected_physical_dag.catalog_epoch_uuid &&
          result.result_publication.envelope.result_kind ==
              exec::CanonicalResultKind::kRows &&
          result.result_publication.envelope.row_count == 2 &&
          result.result_publication.envelope.column_descriptors.size() == 1 &&
          result.result_publication.envelope.column_descriptors[0].name_utf8 ==
              "projected_id" &&
          result.result_publication.row_stream.rows.size() == 2 &&
          result.result_publication.delivery_records.size() == 3 &&
          result.result_publication.delivery_records.front().kind ==
              exec::CanonicalResultDeliveryKind::kMetadata,
      "selected root batch did not publish through the statement-bound result ABI");
  passed &= Require008(
      result.runtime_actuals.node_actuals[0].physical_node_id == 1 &&
          result.runtime_actuals.node_actuals[0].causal_counter_id == 10'000 &&
          result.runtime_actuals.node_actuals[0].pages_read == 1 &&
          result.runtime_actuals.node_actuals[1].physical_node_id == 2 &&
          result.runtime_actuals.node_actuals[1].pages_read == 0 &&
          result.runtime_actuals.node_actuals[3].physical_node_id == 4 &&
          result.runtime_actuals.node_actuals[3].output_row_count == 2,
      "zero and nonzero causal runtime counters lost identity");
  return passed;
}

bool ValidatePreflightReplan() {
  std::vector<std::uint64_t> invocation_order;
  auto request = SelectedExecutionRequest(&invocation_order);
  std::erase_if(request.available_executors, [](const auto& registration) {
    return registration.implementation_id == "join.merge.v1";
  });
  auto result = api::ExecuteCanonicalOptimizerSelectedDag(request);
  bool passed = true;
  passed &= Require008(
      !result.accepted && result.replan_required &&
          !result.data_access_observed && result.dispatch.executed_steps.empty() &&
          result.runtime_actuals.node_actuals.empty() &&
          invocation_order.empty() && result.issues.size() == 1 &&
          result.issues.front().diagnostic_id ==
              "QOW-DIAG-QRY-004-PHYSICAL-IMPLEMENTATION-UNAVAILABLE-V1",
      "disabled selected implementation did not fail before every node");

  invocation_order.clear();
  request = SelectedExecutionRequest(&invocation_order);
  const auto join = std::ranges::find_if(
      request.available_executors, [](const auto& registration) {
        return registration.implementation_id == "join.merge.v1";
      });
  join->executor_capability_uuid = Uuid(999);
  result = api::ExecuteCanonicalOptimizerSelectedDag(request);
  passed &= Require008(!result.accepted && result.replan_required &&
                           invocation_order.empty() &&
                           result.dispatch.executed_steps.empty(),
                       "executor capability drift reached a storage read");
  return passed;
}

bool ValidateAuthorityAndAbiRefusal() {
  std::vector<std::uint64_t> invocation_order;
  auto request = SelectedExecutionRequest(&invocation_order);
  request.transaction_finality_claimed = true;
  auto result = api::ExecuteCanonicalOptimizerSelectedDag(request);
  bool passed = true;
  passed &= Require008(
      !result.accepted && invocation_order.empty() &&
          result.issues.size() == 1 &&
          result.issues.front().diagnostic_id ==
              "QOW-DIAG-OPTIMIZER-SELECTED-EXECUTION-AUTHORITY-V1",
      "transaction-finality claim reached selected-node execution");

  request = SelectedExecutionRequest(&invocation_order);
  request.selected_physical_dag.abi_version = 1;
  request.selected_physical_dag.admission_evidence[3].evidence_uuid =
      Uuid(998);
  result = api::ExecuteCanonicalOptimizerSelectedDag(request);
  passed &= Require008(
          !result.accepted && invocation_order.empty() &&
          result.issues.size() == 1 &&
          result.issues.front().diagnostic_id ==
              "QOW-DIAG-PHYSICAL-NODE-ABI-ADMISSION",
      "legacy scalar snapshot alias treated zero as full MGA authority");

  request = SelectedExecutionRequest(&invocation_order);
  request.selected_physical_dag.mga_statement_context.current = false;
  result = api::ExecuteCanonicalOptimizerSelectedDag(request);
  passed &= Require008(
      !result.accepted && invocation_order.empty() &&
          result.issues.size() == 1 &&
          result.issues.front().diagnostic_id ==
              "QOW-DIAG-PHYSICAL-NODE-ABI-PUBLICATION",
      "non-current physical MGA vector reached selected execution");
  return passed;
}

bool ValidateCompletePreAccessMgaRefusals() {
  const auto expect_refusal = [](auto mutation,
                                 const std::string_view diagnostic,
                                 const std::string_view detail) {
    std::vector<std::uint64_t> invocation_order;
    auto request = SelectedExecutionRequest(&invocation_order);
    mutation(request);
    const auto result = api::ExecuteCanonicalOptimizerSelectedDag(request);
    return Require008(
        !result.accepted && !result.data_access_observed &&
            !result.exact_selected_nodes_executed &&
            !result.canonical_result_published && invocation_order.empty() &&
            result.dispatch.executed_steps.empty() &&
            result.dispatch.root_result_handle_id == 0 &&
            result.runtime_actuals.node_actuals.empty() &&
            !result.result_publication.published &&
            result.result_publication.canonical_envelope_bytes.empty() &&
            result.issues.size() == 1 &&
            result.issues.front().diagnostic_id == diagnostic,
        detail);
  };
  bool passed = true;
  passed &= expect_refusal(
      [](auto& request) {
        request.mga_authority.origin =
            exec::CanonicalMgaAuthorityOrigin::kMissing;
      },
      "QOW-DIAG-MGA-RUNTIME-AUTHORITY-V1",
      "missing runtime authority origin reached selected execution");
  passed &= expect_refusal(
      [](auto& request) { request.mga_authority.resolve_current = {}; },
      "QOW-DIAG-MGA-RUNTIME-AUTHORITY-V1",
      "missing current resolver reached selected execution");
  passed &= expect_refusal(
      [](auto& request) {
        request.mga_authority.statement_context.statement_snapshot_uuid =
            Uuid(997);
      },
      "QOW-DIAG-MGA-RUNTIME-AUTHORITY-V1",
      "DAG-swapped runtime statement context reached execution");
  passed &= expect_refusal(
      [](auto& request) {
        request.mga_authority.statement_context.owning_local_transaction_id =
            static_cast<std::uint32_t>(kOwner);
      },
      "QOW-DIAG-MGA-RUNTIME-AUTHORITY-V1",
      "narrowed runtime transaction identity reached execution");
  const auto expect_current_refusal = [&](auto mutation,
                                          const std::string_view detail) {
    return expect_refusal(
        [mutation](auto& request) {
          auto current = request.mga_authority.statement_context;
          mutation(current);
          request.mga_authority.resolve_current = [current] {
            exec::CanonicalMgaCurrentResolution resolution;
            resolution.statement_context = current;
            return resolution;
          };
        },
        "QOW-DIAG-MGA-RUNTIME-CURRENT-V1", detail);
  };
  passed &= expect_current_refusal(
      [](auto& current) { current.statement_snapshot_uuid = Uuid(996); },
      "swapped resolved statement snapshot reached execution");
  passed &= expect_current_refusal(
      [](auto& current) {
        current.active_excluded_local_transaction_ids =
            {kOldestActive, kOwner, kOwner};
      },
      "duplicate resolved exclusion vector reached execution");
  passed &= expect_current_refusal(
      [](auto& current) {
        current.publication_inventory_next_local_transaction_id =
            static_cast<std::uint32_t>(kInventoryNext);
      },
      "truncated resolved inventory ceiling reached execution");
  passed &= expect_current_refusal(
      [](auto& current) { current.current = false; },
      "stale resolved vector reached execution");
  passed &= expect_refusal(
      [](auto& request) {
        request.selected_physical_dag.nodes.front()
            .mga_statement_context.current = false;
      },
      "QOW-DIAG-PHYSICAL-NODE-ABI-CAPABILITY",
      "node-swapped statement context reached execution");
  passed &= expect_refusal(
      [](auto& request) {
        request.selected_physical_dag.catalog_epoch_uuid =
            request.selected_physical_dag.mga_statement_context
                .statement_metadata_snapshot_uuid;
        request.selected_physical_dag.admission_evidence[1].evidence_uuid =
            request.selected_physical_dag.catalog_epoch_uuid;
      },
      "QOW-DIAG-PHYSICAL-NODE-ABI-PUBLICATION",
      "metadata snapshot was accepted as the execution catalog epoch");
  return passed;
}

bool ValidatePostStartFailureIsTruthful() {
  std::vector<std::uint64_t> invocation_order;
  auto request = SelectedExecutionRequest(&invocation_order);
  const auto values = std::ranges::find_if(
      request.available_executors, [](const auto& registration) {
        return registration.implementation_id == "values.materialize.v1";
      });
  values->execute = [&invocation_order](const auto& dag, const auto& node,
                                        const auto&) {
    invocation_order.push_back(node.physical_node_id);
    auto result = Step008(dag, node, 0, 0, 0, 0, 0);
    result.diagnostic.ok = false;
    result.diagnostic.diagnostic_code =
        "QOW-DIAG-TEST-EXECUTION-FAILURE-AFTER-SCAN-V1";
    return result;
  };
  const auto result = api::ExecuteCanonicalOptimizerSelectedDag(request);
  return Require008(
      !result.accepted && result.data_access_observed &&
          !result.exact_selected_nodes_executed &&
          result.runtime_actuals.node_actuals.empty() &&
          invocation_order == std::vector<std::uint64_t>({1, 2}) &&
          result.issues.size() == 1 &&
          result.issues.front().diagnostic_id ==
              "QOW-DIAG-TEST-EXECUTION-FAILURE-AFTER-SCAN-V1",
      "post-start executor failure falsely reported a pre-read refusal");
}

bool ValidateSelectedCancellationPropagation() {
  bool passed = true;
  std::vector<std::uint64_t> invocation_order;
  auto request = SelectedExecutionRequest(&invocation_order);
  request.cancellation_requested = [] { return true; };
  auto result = api::ExecuteCanonicalOptimizerSelectedDag(request);
  passed &= Require008(
      !result.accepted && result.cancellation_observed &&
          !result.data_access_observed && invocation_order.empty() &&
          result.dispatch.executed_steps.empty() &&
          result.issues.size() == 1 &&
          result.issues.front().diagnostic_id ==
              "QOW-DIAG-QRY-004-PHYSICAL-DISPATCH-CANCELLED-V1",
      "pre-dispatch cancellation did not cross the selected execution API");

  invocation_order.clear();
  request = SelectedExecutionRequest(&invocation_order);
  request.cancellation_requested = [&invocation_order] {
    return !invocation_order.empty();
  };
  result = api::ExecuteCanonicalOptimizerSelectedDag(request);
  passed &= Require008(
      !result.accepted && result.cancellation_observed &&
          result.data_access_observed &&
          invocation_order == std::vector<std::uint64_t>{1} &&
          result.dispatch.executed_steps.empty() &&
          result.issues.size() == 1 &&
          result.issues.front().diagnostic_id ==
              "QOW-DIAG-QRY-004-PHYSICAL-DISPATCH-CANCELLED-V1",
      "mid-DAG cancellation exposed a selected step or hid the scan read");
  return passed;
}

// Packet 4 resolves the selected statement authority at these exact execution
// boundaries: selected-entry, dispatch-entry, pre/post each node, dispatch
// root acceptance, runtime-actuals entry/result, immediate pre-result, and the
// result publisher's own final current-authority boundary.
// Returning a revoked vector at each named phase proves that the outer selected
// result does not leak the locally accumulated dispatch/actual/result state.
bool ValidatePhaseSpecificStaleRefusals() {
  struct PhaseCase {
    const char* name;
    std::size_t stale_on_resolution;
    std::vector<std::uint64_t> expected_invocations;
  };
  const std::vector<PhaseCase> cases = {
      {"pre-node", 3, {}},
      {"post-node", 4, {1}},
      {"inter-node", 5, {1}},
      {"pre-actuals", 12, {1, 2, 3, 4}},
      {"immediate-pre-result", 14, {1, 2, 3, 4}},
      {"publisher-boundary", 15, {1, 2, 3, 4}},
  };

  bool passed = true;
  for (const auto& phase : cases) {
    std::vector<std::uint64_t> invocation_order;
    auto request = SelectedExecutionRequest(&invocation_order);
    const auto state =
        RefuseAtResolution008(&request, phase.stale_on_resolution);
    const auto result = api::ExecuteCanonicalOptimizerSelectedDag(request);
    passed &= RequireNoSelectedExposure008(result, phase.name);
    passed &= Require008(
        invocation_order == phase.expected_invocations &&
            state->resolution_count == phase.stale_on_resolution,
        std::string(phase.name) +
            " refusal did not occur at the intended revalidation boundary");
  }
  return passed;
}

// QOW-TEST-INTEGRATION-306-211-V1
bool ValidateResultPublicationRefusalIsTruthful() {
  std::vector<std::uint64_t> invocation_order;
  auto request = SelectedExecutionRequest(&invocation_order);
  request.result_publication_request.column_bindings[0]
      .published_descriptor->type_uuid = Uuid(8299);
  auto result = api::ExecuteCanonicalOptimizerSelectedDag(request);
  bool passed = true;
  passed &= Require008(
      !result.accepted && result.data_access_observed &&
          !result.canonical_result_published &&
          result.result_publication.canonical_envelope_bytes.empty() &&
          invocation_order == std::vector<std::uint64_t>({1, 2, 3, 4}) &&
          result.issues.size() == 1 &&
          result.issues.front().diagnostic_id ==
              "QOW-DIAG-QRY-021-REFUSAL-V1",
      "result descriptor drift published output or hid completed execution");

  invocation_order.clear();
  request = SelectedExecutionRequest(&invocation_order);
  request.result_publication_request.physical_output_batch =
      RootResultBatch();
  result = api::ExecuteCanonicalOptimizerSelectedDag(request);
  passed &= Require008(
      !result.accepted && result.data_access_observed &&
          !result.canonical_result_published &&
          result.issues.size() == 1 &&
          result.issues.front().diagnostic_id ==
              "QOW-DIAG-OPTIMIZER-SELECTED-RESULT-PAYLOAD-V1",
      "caller-supplied substitute root batch bypassed executor output");
  return passed;
}

bool ValidateNestedPublicationAuthorityCannotOverrideSelection() {
  std::vector<std::uint64_t> invocation_order;
  auto request = SelectedExecutionRequest(&invocation_order);
  auto& nested = request.result_publication_request;
  nested.statement_uuid = Uuid(8290);
  nested.mga_authority = request.mga_authority;
  nested.mga_authority.origin = exec::CanonicalMgaAuthorityOrigin::kMissing;
  nested.mga_authority.statement_context.statement_uuid = Uuid(8291);
  nested.selected_physical_dag = request.selected_physical_dag;
  nested.selected_physical_dag.catalog_epoch_uuid = Uuid(8292);
  nested.selected_catalog_epoch_uuid = Uuid(8293);

  const auto result = api::ExecuteCanonicalOptimizerSelectedDag(request);
  return Require008(
      result.accepted && result.canonical_result_published &&
          result.result_publication.published &&
          result.result_publication.envelope.statement_uuid ==
              request.mga_authority.statement_context.statement_uuid &&
          exec::PhysicalMgaStatementContextEqual(
              result.result_publication.envelope.mga_statement_context,
              request.mga_authority.statement_context) &&
          result.result_publication.envelope.catalog_epoch_uuid ==
              request.selected_physical_dag.catalog_epoch_uuid &&
          result.result_publication.envelope.catalog_epoch_uuid !=
              nested.selected_catalog_epoch_uuid,
      "caller-supplied nested statement or catalog authority overrode selection");
}

bool ValidateCanonicalRouteIsolation() {
  std::ifstream source_file(SB_QOW_PLAN_API_SOURCE_FILE);
  const std::string source((std::istreambuf_iterator<char>(source_file)),
                           std::istreambuf_iterator<char>());
  const auto route_start = source.find(
      "CanonicalOptimizerSelectedExecutionResult "
      "ExecuteCanonicalOptimizerSelectedDag(");
  const auto route_end = source.find("\n#ifndef", route_start);
  bool passed = true;
  passed &= Require008(source_file.good() || source_file.eof(),
                       "canonical plan API source could not be read");
  passed &= Require008(source.find("QOW-SOURCE-OPT-008-V1") !=
                           std::string::npos,
                       "canonical selected-DAG execution marker is absent");
  passed &= Require008(
      source.find("bool AttachOptimizerSelectionEvidence(") ==
          std::string::npos &&
          source.find("bool AttachLegacyOptimizerSelectionEvidence(") !=
              std::string::npos,
      "legacy flat selection-evidence helper is not quarantined by name");
  passed &= Require008(route_start != std::string::npos &&
                           route_end != std::string::npos,
                       "canonical selected-DAG route body could not be isolated");
  if (route_start != std::string::npos && route_end != std::string::npos) {
    const auto route = source.substr(route_start, route_end - route_start);
    passed &= Require008(
        route.find("OptimizeLogicalPlan") == std::string::npos &&
            route.find("AttachLegacyOptimizerSelectionEvidence") ==
                std::string::npos &&
            route.find("ExecuteQueryBatch") == std::string::npos,
        "canonical selected-DAG execution fell back to the legacy flat route");
  }
  return passed;
}

}  // namespace

// QOW-TEST-OPT-008-V1
int main() {
  bool passed = true;
  passed &= ValidateExactSelectedExecution();
  passed &= ValidatePreflightReplan();
  passed &= ValidateAuthorityAndAbiRefusal();
  passed &= ValidateCompletePreAccessMgaRefusals();
  passed &= ValidatePostStartFailureIsTruthful();
  passed &= ValidateSelectedCancellationPropagation();
  passed &= ValidatePhaseSpecificStaleRefusals();
  passed &= ValidateResultPublicationRefusalIsTruthful();
  passed &= ValidateNestedPublicationAuthorityCannotOverrideSelection();
  passed &= ValidateCanonicalRouteIsolation();
  return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
