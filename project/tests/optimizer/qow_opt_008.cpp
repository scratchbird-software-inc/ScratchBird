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
#include <string>
#include <string_view>

namespace {

namespace api = scratchbird::engine::internal_api;

bool Require008(const bool condition, const std::string_view detail) {
  if (!condition) std::cerr << "QOW-TEST-OPT-008-V1: " << detail << '\n';
  return condition;
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
  result.input_row_count = input_rows;
  result.output_row_count = output_rows;
  result.rows_examined = rows_examined;
  result.pages_read = pages_read;
  result.spill_bytes = spill_bytes;
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
  request.inventory_local_transaction_id =
      publication.physical_dag.local_transaction_id;
  request.inventory_statement_snapshot_id =
      publication.physical_dag.statement_snapshot_id;
  request.engine_execution_authorized = true;

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
        return Step008(dag, node, 20'004, 2, 2, 2, 0);
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
        scan_request.inventory_local_transaction_id = dag.local_transaction_id;
        scan_request.inventory_statement_snapshot_id =
            dag.statement_snapshot_id;
        scan_request.selected_descriptor_generation = 31;
        scan_request.current_descriptor_generation = 31;
        exec::CanonicalScanCandidateEvidence candidate;
        candidate.candidate_uuid = Uuid(910);
        candidate.record_uuid = Uuid(911);
        candidate.relation_uuid = Uuid(9);
        candidate.visibility_decision_uuid = Uuid(912);
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
          result.causal_counters_attached && result.data_access_observed &&
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
          result.runtime_actuals.node_actuals.size() == 4,
      "runtime actuals were not attached to the selected physical identities");
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
              "QOW-DIAG-OPTIMIZER-SELECTED-EXECUTION-SCOPE-V1",
      "legacy evidence-only physical root reached canonical execution");
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
  passed &= ValidatePostStartFailureIsTruthful();
  passed &= ValidateCanonicalRouteIsolation();
  return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
