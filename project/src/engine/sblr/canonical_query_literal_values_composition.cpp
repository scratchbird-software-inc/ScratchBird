// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "canonical_query_literal_values_composition.hpp"
#include "canonical_query_object_free_composition_support.hpp"
#include "canonical_query_object_free_profile.hpp"
#include "canonical_query_physical_registration.hpp"
#include "canonical_query_runtime_memory_support.hpp"
#include "canonical_query_scalar_support.hpp"
#include "engine/optimizer/optimizer_contract.hpp"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace scratchbird::engine::sblr {
namespace api = scratchbird::engine::internal_api;
namespace exec = scratchbird::engine::executor;
namespace opt = scratchbird::engine::optimizer;
namespace plan = scratchbird::engine::planner;

// SEARCH_KEY: SB_ENGINE_CANONICAL_QUERY_LITERAL_VALUES_COMPOSITION_AUTHORITY
// Coordinates the admitted standalone literal-VALUES leaf through the
// existing optimizer, callback registration, and guarded selected-execution adapter.
// Owns no public route selection, storage, snapshot construction, or transaction finality.

namespace {

constexpr std::string_view kValuesImplementationId =
    "values.materialize.canonical.v1";

}  // namespace

CanonicalObjectFreeValuesExecutionResult
ExecuteCanonicalObjectFreeLiteralValuesQuery(
    const CanonicalObjectFreeValuesExecutionRequest& request) {
  CanonicalObjectFreeValuesExecutionResult result;
  const auto refuse = [&](std::string diagnostic_id, std::string detail) {
    result.optimizer_selected = false;
    result.physical_dag_published = false;
    result.physical_dag_executed = false;
    result.runtime_actuals_attached = false;
    result.canonical_result_published = false;
    result.physical_node_count = 0;
    result.canonical_result_column_count = 0;
    result.canonical_result_row_count = 0;
    result.selected_plan_uuid.clear();
    result.canonical_result_bytes.clear();
    result.api_result = Failure(request, std::move(diagnostic_id),
                                std::move(detail));
    return result;
  };

  const auto& graph = request.optimizer_request.logical_graph;
  if (graph.nodes.size() != 1 ||
      graph.root_logical_node_id != graph.nodes.front().logical_node_id ||
      graph.nodes.front().node_kind !=
          plan::CanonicalLogicalRelationalNodeKind::kValues ||
      graph.nodes.front().semantic_variant_id != "values.literal-table.v1" ||
      !graph.nodes.front().input_logical_node_ids.empty() ||
      !graph.nodes.front().required_object_uuids.empty() ||
      !graph.nodes.front().required_property_uuids.empty() ||
      !graph.nodes.front().delivered_property_uuids.empty() ||
      !request.optimizer_request.logical_properties.properties.empty()) {
    return result;
  }
  result.profile_matched = true;
  if (!request.optimizer_admission.admitted ||
      !request.optimizer_admission.planning_allowed) {
    return refuse("QOW-DIAG-RELATIONAL-LIVE-VALUES-ADMISSION-V1",
                  "live VALUES execution lacks optimizer admission");
  }

  auto materialized = MaterializeValues(request.relational_dag,
                                        graph.nodes.front(),
                                        request.expression_services);
  if (!materialized.ok) {
    return refuse("QOW-DIAG-RELATIONAL-LIVE-VALUES-PAYLOAD-V1",
                  materialized.detail);
  }

  const auto identity_scope =
      graph.bound_sblr_tree_uuid + ":" + request.context.statement_uuid.canonical;
  const auto alternative_uuid =
      DerivedCanonicalUuid(identity_scope, "values.alternative");
  const auto capability_uuid =
      DerivedCanonicalUuid(identity_scope, "values.capability");
  const auto transformation_uuid =
      DerivedCanonicalUuid(identity_scope, "values.transformation");
  const auto cost_vector_uuid =
      DerivedCanonicalUuid(identity_scope, "values.cost-vector");
  const auto calibration_uuid =
      DerivedCanonicalUuid(identity_scope, "values.calibration");

  std::uint64_t memory_bytes = 1;
  for (const auto& row : materialized.batch.rows) {
    for (const auto& value : row.values) {
      if (!CheckedAdd(memory_bytes, value.encoded_value.size(),
                      &memory_bytes) ||
          !CheckedAdd(memory_bytes, value.binary_value.size(),
                      &memory_bytes)) {
        return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                      "live VALUES materialization size overflowed");
      }
    }
  }
  if (memory_bytes > request.optimizer_request.resource.memory_budget_bytes) {
    return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1",
                  "live VALUES materialization exceeds the admitted memory budget");
  }

  opt::CanonicalOptimizerAlternativeDomainSnapshot domain;
  domain.capability_snapshot_uuid =
      request.optimizer_admission.capability_snapshot_uuid;
  domain.bound_sblr_tree_uuid = graph.bound_sblr_tree_uuid;
  domain.catalog_epoch_uuid = graph.catalog_epoch_uuid;
  domain.security_context_uuid = graph.security_context_uuid;
  domain.local_transaction_id = graph.local_transaction_id;
  domain.statement_snapshot_id = graph.statement_snapshot_id;
  domain.mga_statement_context = graph.mga_statement_context;
  domain.complete_finite_domain = true;
  domain.engine_owned = true;
  opt::CanonicalOptimizerAlternativeDomainRecord domain_record;
  domain_record.alternative_uuid = alternative_uuid;
  domain_record.capability_uuid = capability_uuid;
  domain_record.logical_node_id = graph.nodes.front().logical_node_id;
  domain_record.logical_node_kind =
      plan::CanonicalLogicalRelationalNodeKind::kValues;
  domain_record.semantic_variant_id =
      graph.nodes.front().semantic_variant_id;
  domain_record.implementation_id = std::string(kValuesImplementationId);
  domain_record.memory_bytes_required = memory_bytes;
  domain_record.compatibility_profile_id = "native.sblr.row.v1";
  domain_record.exact_semantics = true;
  domain_record.native_sblr_compatible = true;
  domain_record.available = true;
  domain_record.engine_owned = true;
  domain.records.push_back(std::move(domain_record));
  const auto inventory = opt::EnumerateCanonicalOptimizerAlternativeInventory(
      request.optimizer_request, request.optimizer_admission, domain);
  if (!inventory.accepted || !inventory.inventory_complete ||
      inventory.legal_candidate_count != 1 || !inventory.issues.empty()) {
    const auto diagnostic =
        inventory.issues.empty()
            ? "QOW-DIAG-OPTIMIZER-INVENTORY-COVERAGE-V1"
            : inventory.issues.front().diagnostic_id;
    const auto detail = inventory.issues.empty()
                            ? "live VALUES alternative inventory is incomplete"
                            : inventory.issues.front().field_id;
    return refuse(diagnostic, detail);
  }

  opt::CanonicalOptimizerSearchCandidateInput candidate;
  candidate.alternative_uuid = alternative_uuid;
  candidate.logical_node_id = inventory.receipts.front().logical_node_id;
  candidate.semantic_variant_id =
      inventory.receipts.front().semantic_variant_id;
  candidate.transformation_uuid = transformation_uuid;
  candidate.transformation_rule_id = "canonical.values.materialize.v1";
  candidate.required_property_uuids =
      inventory.receipts.front().required_property_uuids;
  candidate.delivered_property_uuids =
      inventory.receipts.front().delivered_property_uuids;
  candidate.enforced_property_uuids =
      inventory.receipts.front().enforced_property_uuids;
  candidate.bound_sblr_tree_uuid = graph.bound_sblr_tree_uuid;
  candidate.statistics_snapshot_uuid =
      request.optimizer_admission.statistics_snapshot_uuid;
  candidate.statistics_generation =
      request.optimizer_admission.statistics_generation;
  candidate.model_family_id = "relational.local.v1";
  candidate.cost_terms.cost_vector_uuid = cost_vector_uuid;
  candidate.cost_terms.calibration_profile_uuid = calibration_uuid;
  candidate.cost_terms.scalarization_policy_id =
      "canonical.optimizer.complete-unit-sum-minus-cache-benefit.v1";
  candidate.cost_terms.cpu_units = materialized.batch.rows.size();
  candidate.cost_terms.memory_bytes_required = memory_bytes;
  candidate.cost_terms.memory_allocation_units = memory_bytes;
  candidate.cost_terms.complete_dimension_vector = true;
  candidate.cost_terms.confidence = opt::CostConfidence::kExact;
  candidate.semantic_preserving = true;
  candidate.transformation_preconditions_satisfied = true;
  candidate.property_enforcement_required =
      inventory.receipts.front().property_enforcement_required;
  candidate.derived_from_admitted_statistics = true;
  candidate.engine_coster_owned = true;

  opt::CanonicalOptimizerSearchPolicy search_policy;
  search_policy.maximum_exhaustive_plan_count = 1;
  search_policy.bounded_beam_width = 1;
  search_policy.deterministic_step_cost_ns = 1;
  search_policy.engine_owned = true;
  search_policy.allow_timeout_degradation = true;
  const auto search = opt::SearchCanonicalRelationalMemo(
      request.optimizer_request, request.optimizer_admission,
      inventory.catalog,
      {candidate}, search_policy);
  if (!search.accepted || !search.selected || !search.issues.empty()) {
    const auto diagnostic = search.issues.empty()
                                ? "QOW-DIAG-OPTIMIZER-SEARCH-NO-PLAN-V1"
                                : search.issues.front().diagnostic_id;
    const auto detail = search.issues.empty()
                            ? "live VALUES search returned no selected plan"
                            : search.issues.front().field_id;
    return refuse(diagnostic, detail);
  }
  result.optimizer_selected = true;

  opt::CanonicalExecutorCapabilityCatalog capabilities;
  capabilities.capability_snapshot_uuid =
      request.optimizer_admission.capability_snapshot_uuid;
  capabilities.policy_epoch = request.optimizer_admission.policy_epoch;
  capabilities.engine_owned = true;
  opt::CanonicalExecutorCapabilityRecord capability;
  capability.capability_uuid = capability_uuid;
  capability.capability_abi_version = 1;
  capability.implementation_id = kValuesImplementationId;
  capability.logical_node_kind =
      plan::CanonicalLogicalRelationalNodeKind::kValues;
  capability.physical_node_kind = exec::PhysicalNodeKind::kValues;
  capability.maximum_memory_bytes =
      request.optimizer_request.resource.memory_budget_bytes;
  capability.spill_supported = false;
  capability.available = true;
  capability.engine_owned = true;
  capabilities.capabilities.push_back(std::move(capability));

  opt::CanonicalOptimizerPhysicalPublicationIdentity publication_identity;
  publication_identity.selected_plan_uuid =
      DerivedCanonicalUuid(identity_scope, "values.selected-plan");
  publication_identity.first_causal_counter_id = 1;
  publication_identity.engine_owned = true;
  auto publication = opt::PublishCanonicalPhysicalDag(
      request.optimizer_request, request.optimizer_admission,
      inventory.catalog,
      search, capabilities, publication_identity);
  if (!publication.accepted || !publication.published ||
      !publication.issues.empty()) {
    const auto diagnostic = publication.issues.empty()
                                ? "QOW-DIAG-OPTIMIZER-PHYSICAL-PUBLICATION-V1"
                                : publication.issues.front().diagnostic_id;
    const auto detail = publication.issues.empty()
                            ? "live VALUES physical DAG was not published"
                            : publication.issues.front().field_id;
    return refuse(diagnostic, detail);
  }
  result.physical_dag_published = true;
  result.physical_node_count = publication.physical_dag.nodes.size();
  result.selected_plan_uuid = publication.physical_dag.selected_plan_uuid;

  exec::CanonicalPhysicalExecutorRegistration registration;
  registration.node_kind = exec::PhysicalNodeKind::kValues;
  registration.implementation_id = kValuesImplementationId;
  registration.executor_capability_uuid = capability_uuid;
  registration.executor_capability_abi_version = 1;
  registration.engine_owned = true;
  registration.accepts_optimizer_publication_v2 = true;
  registration.execute =
      [batch = materialized.batch](const exec::TypedPhysicalNodeDag& dag,
                                   const exec::PhysicalNodeRecord& node,
                                   const std::vector<
                                       exec::CanonicalPhysicalDispatchInput>&
                                       inputs) {
        exec::CanonicalPhysicalDispatchStepResult step;
        step.selected_plan_uuid = dag.selected_plan_uuid;
        step.mga_statement_context = dag.mga_statement_context;
        step.executed_physical_node_id = node.physical_node_id;
        step.causal_counter_id = node.causal_counter_id;
        step.output_descriptor_ids = node.output_descriptor_ids;
        step.authority.engine_mga_snapshot_bound = true;
        if (!node.input_physical_node_ids.empty() || !inputs.empty()) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "QOW-DIAG-RELATIONAL-LIVE-VALUES-INPUT-V1";
          step.diagnostic.detail = "VALUES executor received an input edge";
          return step;
        }
        step.result_handle_id = node.physical_node_id;
        step.output_row_count = batch.rows.size();
        step.rows_examined = batch.rows.size();
        step.materialized_output_batch = batch;
        return step;
      };

  api::CanonicalOptimizerSelectedExecutionRequest execution_request;
  execution_request.pre_access_statistics_snapshot_uuid =
      publication.physical_dag.statistics_snapshot_uuid;
  execution_request.mga_authority =
      BuildCanonicalExecutionMgaAuthority(request.context,
                                          publication.physical_dag);
  execution_request.available_executors.push_back(std::move(registration));
  execution_request.engine_execution_authorized = true;
  execution_request.result_publication_request.statement_uuid =
      request.context.statement_uuid.canonical;
  execution_request.result_publication_request.execution_attempt_uuid =
      DerivedCanonicalUuid(
          identity_scope + ":" + request.context.current_monotonic_ns,
          "values.execution-attempt");
  execution_request.result_publication_request.transaction_effect_evidence_uuid =
      DerivedCanonicalUuid(
          identity_scope + ":" +
              std::to_string(request.context.local_transaction_id) + ":" +
              std::to_string(
                  request.context.snapshot_visible_through_local_transaction_id),
          "values.transaction-effect-unchanged");
  execution_request.result_publication_request.result_kind =
      exec::CanonicalResultKind::kRows;
  execution_request.result_publication_request.invocation_mode =
      exec::CanonicalResultInvocationMode::kDirect;
  execution_request.result_publication_request.column_bindings =
      std::move(materialized.result_bindings);
  execution_request.result_publication_request.maximum_row_count =
      std::max<std::size_t>(1, materialized.batch.rows.size());

  OrdinaryRuntimeMemoryReceipts producer_memory_receipts;
  if (publication.physical_dag.nodes.size() != 1) {
    return refuse("QOW-DIAG-OPT-017-REFUSAL-V1",
                  "VALUES producer memory receipt lacks one selected node");
  }
  OrdinaryRuntimeMemoryReceipt producer_memory;
  producer_memory.physical_node_id =
      publication.physical_dag.nodes.front().physical_node_id;
  producer_memory.implementation_id = std::string(kValuesImplementationId);
  producer_memory.producer_peak_memory_bytes = memory_bytes;
  producer_memory.producer_peak_memory_exact = true;
  producer_memory.non_accessing_proven = true;
  producer_memory_receipts.emplace(producer_memory.physical_node_id,
                                   std::move(producer_memory));
  execution_request.selected_physical_dag =
      std::move(publication.physical_dag);
  const auto execution =
      ExecuteSelectedCanonicalObjectFreeDag(request.context, execution_request,
                                  producer_memory_receipts);
  if (!execution.accepted || !execution.exact_selected_nodes_executed ||
      !execution.causal_counters_attached ||
      !execution.canonical_result_published || !execution.issues.empty()) {
    const auto diagnostic = execution.issues.empty()
                                ? "QOW-DIAG-RELATIONAL-LIVE-VALUES-EXECUTION-V1"
                                : execution.issues.front().diagnostic_id;
    const auto detail = execution.issues.empty()
                            ? "live VALUES selected DAG was not completed"
                            : execution.issues.front().field_id;
    return refuse(diagnostic, detail);
  }
  result.physical_dag_executed = true;
  result.runtime_actuals_attached = execution.runtime_actuals.accepted;
  result.canonical_result_published =
      execution.result_publication.published;
  result.canonical_result_column_count =
      execution.result_publication.envelope.column_descriptors.size();
  result.canonical_result_row_count =
      execution.result_publication.row_stream.rows.size();
  result.canonical_result_bytes =
      execution.result_publication.canonical_envelope_bytes;
  result.api_result = SuccessfulApiResult(request, execution);
  return result;
}

}  // namespace scratchbird::engine::sblr
