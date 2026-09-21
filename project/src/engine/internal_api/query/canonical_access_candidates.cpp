// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "query/canonical_relational_bridge.hpp"

#include <algorithm>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace scratchbird::engine::internal_api {
namespace {

bool QowOpt004CanonicalUuid(const EngineUuid& value) {
  return core::uuid::IsEngineIdentityUuid(value);
}

bool QowOpt004StableId(const std::string_view value) {
  return !value.empty() && value.size() <= 128 &&
         std::ranges::all_of(value, [](const unsigned char ch) {
           return (ch >= 'a' && ch <= 'z') ||
                  (ch >= '0' && ch <= '9') || ch == '.' || ch == '_' ||
                  ch == '-';
         });
}

bool QowOpt004PredicateKeyMatches(
    const CanonicalAccessCandidatePlanningRequestV1& request,
    const CanonicalAccessIndexMetadataV1& index) {
  if (request.predicate_expression_ids.empty() ||
      index.key_expression_ids.size() <
          request.predicate_expression_ids.size()) {
    return false;
  }
  return std::equal(request.predicate_expression_ids.begin(),
                    request.predicate_expression_ids.end(),
                    index.key_expression_ids.begin());
}

}  // namespace

// QOW-SOURCE-OPT-004-V1
// Generate every access alternative from a single engine-owned metadata and
// statistics snapshot before a read is permitted.  Heap remains the legal
// fallback.  Index records remain in the catalog with an exact refusal when
// capability, generation, statistics, predicate, or MGA visibility evidence
// is not current; no unavailable index is silently selected as another route.
CanonicalAccessCandidatePlanningResultV1
QowGenerateCanonicalAccessCandidatesV1(
    const CanonicalAccessCandidatePlanningRequestV1& request) {
  namespace plan = scratchbird::engine::planner;
  CanonicalAccessCandidatePlanningResultV1 result;
  const auto refuse = [&](std::string diagnostic_id, std::string field_id) {
    result = {};
    result.diagnostic_id = std::move(diagnostic_id);
    result.field_id = std::move(field_id);
    return result;
  };

  const auto graph_validation =
      plan::ValidateCanonicalLogicalRelationalGraph(request.logical_graph);
  if (!graph_validation.accepted) {
    const auto& issue = graph_validation.issues.front();
    return refuse(issue.diagnostic_id, issue.field_id);
  }
  const auto node = std::ranges::find_if(
      request.logical_graph.nodes, [&](const auto& candidate) {
        return candidate.logical_node_id == request.logical_node_id;
      });
  if (node == request.logical_graph.nodes.end() ||
      node->node_kind != plan::CanonicalLogicalRelationalNodeKind::kRelationSource ||
      node->semantic_variant_id != "relation.source.v1" ||
      node->required_object_uuids !=
          std::vector<EngineUuid>{request.relation_uuid} ||
      !node->input_logical_node_ids.empty()) {
    return refuse("QOW-DIAG-OPT-004-RELATION-SOURCE-V1",
                  "logical_relation_source");
  }
  if (!QowOpt004CanonicalUuid(request.relation_uuid) ||
      !QowOpt004CanonicalUuid(request.heap_alternative_uuid) ||
      !QowOpt004CanonicalUuid(request.heap_capability_uuid) ||
      !QowOpt004CanonicalUuid(request.statistics_snapshot_uuid) ||
      request.current_catalog_generation == 0 ||
      request.current_relation_descriptor_generation == 0 ||
      request.current_statistics_generation == 0 ||
      request.maximum_candidate_count == 0 ||
      request.indexes.size() >= request.maximum_candidate_count) {
    return refuse("SBLR.PLAN_TREE.RESOURCE_LIMIT",
                  "access_candidate_identity_or_bound");
  }
  if (!request.metadata_snapshot_engine_owned ||
      !request.storage_descriptor_engine_owned ||
      !request.statistics_snapshot_engine_owned ||
      request.data_access_observed ||
      request.parser_planning_authority_claimed ||
      request.transaction_finality_authority_claimed) {
    return refuse("QOW-DIAG-OPT-004-AUTHORITY-V1",
                  "pre_access_engine_owned_authority");
  }
  if ((request.predicate_kind != "exact" &&
       request.predicate_kind != "range") ||
      request.predicate_expression_ids.empty() ||
      std::ranges::any_of(request.predicate_expression_ids,
                          [](const auto id) { return id == 0; })) {
    return refuse("QOW-DIAG-OPT-004-PREDICATE-V1",
                  "bound_predicate_profile");
  }

  result.catalog.bound_sblr_tree_uuid =
      request.logical_graph.bound_sblr_tree_uuid;
  result.catalog.catalog_epoch_uuid = request.logical_graph.catalog_epoch_uuid;
  result.catalog.security_context_uuid =
      request.logical_graph.security_context_uuid;
  result.catalog.local_transaction_id =
      request.logical_graph.local_transaction_id;
  result.catalog.statement_snapshot_id =
      request.logical_graph.statement_snapshot_id;
  result.catalog.mga_statement_context =
      request.logical_graph.mga_statement_context;

  plan::CanonicalPhysicalAlternativeRecord heap;
  heap.alternative_uuid = request.heap_alternative_uuid;
  heap.logical_node_id = request.logical_node_id;
  heap.implementation_id = "scan.heap.v1";
  heap.capability_uuid = request.heap_capability_uuid;
  heap.output_descriptor_ids = node->output_descriptor_ids;
  heap.available = true;
  result.catalog.alternatives.push_back(heap);
  result.receipts.push_back(
      {.alternative_uuid = heap.alternative_uuid,
       .implementation_id = heap.implementation_id,
       .capability_uuid = heap.capability_uuid,
       .logical_node_id = heap.logical_node_id,
       .catalog_generation = request.current_catalog_generation,
       .relation_descriptor_generation =
           request.current_relation_descriptor_generation,
       .statistics_generation = request.current_statistics_generation,
       .available = true,
       .heap_fallback = true,
       .capability_validated = true,
       .generation_validated = true,
       .statistics_validated = true,
       .visibility_validated = true});

  std::unordered_set<EngineUuid, EngineUuidHash> index_uuids;
  std::unordered_set<EngineUuid, EngineUuidHash> alternative_uuids{
      request.heap_alternative_uuid};
  for (const auto& index : request.indexes) {
    if (!QowOpt004CanonicalUuid(index.index_uuid) ||
        !QowOpt004CanonicalUuid(index.relation_uuid) ||
        !QowOpt004CanonicalUuid(index.alternative_uuid) ||
        !QowOpt004CanonicalUuid(index.capability_uuid) ||
        !QowOpt004StableId(index.implementation_id) ||
        !index.implementation_id.starts_with("scan.index.") ||
        !index_uuids.insert(index.index_uuid).second ||
        !alternative_uuids.insert(index.alternative_uuid).second ||
        index.key_expression_ids.empty() ||
        std::ranges::any_of(index.key_expression_ids,
                            [](const auto id) { return id == 0; })) {
      return refuse("QOW-DIAG-OPT-004-INDEX-IDENTITY-V1",
                    "catalog_index_record");
    }

    CanonicalAccessCandidateReceiptV1 receipt;
    receipt.alternative_uuid = index.alternative_uuid;
    receipt.index_uuid = index.index_uuid;
    receipt.implementation_id = index.implementation_id;
    receipt.capability_uuid = index.capability_uuid;
    receipt.logical_node_id = request.logical_node_id;
    receipt.catalog_generation = index.catalog_generation;
    receipt.relation_descriptor_generation =
        index.relation_descriptor_generation;
    receipt.index_generation = index.index_generation;
    receipt.statistics_generation = index.statistics_generation;
    receipt.visible_generation = index.visible_generation;
    receipt.residual_recheck_required = index.residual_recheck_required;

    const bool generation_ok =
        index.catalog_record_current && index.catalog_generation != 0 &&
        index.catalog_generation == request.current_catalog_generation &&
        index.relation_descriptor_generation ==
            request.current_relation_descriptor_generation &&
        index.index_generation != 0;
    const bool capability_ok =
        index.profile_authoritative &&
        index.profile_supports_mga_visibility &&
        index.profile_supports_generation_visibility &&
        (request.predicate_kind == "exact" ? index.supports_exact_lookup
                                            : index.supports_range_scan);
    const bool lifecycle_ok =
        index.lifecycle_ready && index.build_validation_complete;
    const bool visibility_ok =
        index.visibility_evidence_engine_owned &&
        index.visible_to_statement_snapshot && index.visible_generation != 0;
    const bool statistics_ok =
        index.statistics_present && index.statistics_current &&
        !index.statistics_stale && index.statistics_profile_coupled &&
        index.statistics_mga_visible && index.statistics_generation != 0 &&
        index.statistics_generation == request.current_statistics_generation &&
        index.statistics_index_generation == index.index_generation &&
        index.statistics_catalog_generation ==
            request.current_catalog_generation;
    const bool predicate_ok = QowOpt004PredicateKeyMatches(request, index);
    const bool exactness_ok = !index.approximate || index.exact_fallback;

    receipt.generation_validated = generation_ok;
    receipt.capability_validated = capability_ok;
    receipt.statistics_validated = statistics_ok;
    receipt.visibility_validated = visibility_ok;
    if (index.data_access_observed) {
      receipt.refusal_diagnostic_id = "QOW-DIAG-OPT-004-PHASE-V1";
    } else if (index.relation_uuid != request.relation_uuid) {
      receipt.refusal_diagnostic_id = "QOW-DIAG-OPT-004-RELATION-V1";
    } else if (!generation_ok) {
      receipt.refusal_diagnostic_id = "QOW-DIAG-OPT-004-GENERATION-V1";
    } else if (!lifecycle_ok) {
      receipt.refusal_diagnostic_id = "QOW-DIAG-OPT-004-LIFECYCLE-V1";
    } else if (!capability_ok || !exactness_ok) {
      receipt.refusal_diagnostic_id = "QOW-DIAG-OPT-004-CAPABILITY-V1";
    } else if (!visibility_ok) {
      receipt.refusal_diagnostic_id = "QOW-DIAG-OPT-004-VISIBILITY-V1";
    } else if (!statistics_ok) {
      receipt.refusal_diagnostic_id = "QOW-DIAG-OPT-004-STATISTICS-V1";
    } else if (!predicate_ok) {
      receipt.refusal_diagnostic_id = "QOW-DIAG-OPT-004-PREDICATE-V1";
    } else {
      receipt.available = true;
    }

    plan::CanonicalPhysicalAlternativeRecord alternative;
    alternative.alternative_uuid = index.alternative_uuid;
    alternative.logical_node_id = request.logical_node_id;
    alternative.implementation_id = index.implementation_id;
    alternative.capability_uuid = index.capability_uuid;
    alternative.output_descriptor_ids = node->output_descriptor_ids;
    alternative.available = receipt.available;
    alternative.refusal_diagnostic_id = receipt.refusal_diagnostic_id;
    result.catalog.alternatives.push_back(std::move(alternative));
    result.receipts.push_back(std::move(receipt));
  }

  const auto boundary = plan::ValidateCanonicalLogicalPhysicalBoundary(
      request.logical_graph, result.catalog, request.maximum_candidate_count);
  if (!boundary.accepted) {
    const auto& issue = boundary.issues.front();
    return refuse(issue.diagnostic_id, issue.field_id);
  }
  result.accepted = true;
  result.planning_complete_before_access = true;
  result.data_access_allowed = false;
  return result;
}

// A selected access is causal only when the exact available alternative is
// carried into the immutable physical node and the canonical execution result
// reports the same node/counter after completion.  This validates a receipt;
// it neither dispatches a read nor gains transaction-finality authority.
bool QowValidateCanonicalSelectedAccessExecutionV1(
    const CanonicalAccessCandidatePlanningResultV1& planning,
    const EngineUuid& selected_alternative_uuid,
    const scratchbird::engine::executor::TypedPhysicalNodeDag& physical_dag,
    const CanonicalOptimizerSelectedExecutionResult& execution,
    std::string* diagnostic_id,
    std::string* field_id) {
  const auto refuse = [&](std::string diagnostic, std::string field) {
    if (diagnostic_id != nullptr) *diagnostic_id = std::move(diagnostic);
    if (field_id != nullptr) *field_id = std::move(field);
    return false;
  };
  if (diagnostic_id != nullptr) diagnostic_id->clear();
  if (field_id != nullptr) field_id->clear();
  const auto receipt = std::ranges::find_if(
      planning.receipts, [&](const auto& candidate) {
        return candidate.alternative_uuid == selected_alternative_uuid;
      });
  const auto alternative = std::ranges::find_if(
      planning.catalog.alternatives, [&](const auto& candidate) {
        return candidate.alternative_uuid == selected_alternative_uuid;
      });
  if (!planning.accepted || !planning.planning_complete_before_access ||
      planning.data_access_allowed || receipt == planning.receipts.end() ||
      alternative == planning.catalog.alternatives.end() ||
      !receipt->available || !alternative->available) {
    return refuse("QOW-DIAG-OPT-004-SELECTION-V1",
                  "available_planned_alternative");
  }
  if (physical_dag.abi_version != 2 || physical_dag.nodes.size() != 1 ||
      physical_dag.root_physical_node_id !=
          physical_dag.nodes.front().physical_node_id ||
      physical_dag.catalog_generation != receipt->catalog_generation ||
      physical_dag.statistics_generation != receipt->statistics_generation) {
    return refuse("QOW-DIAG-OPT-004-PUBLICATION-V1",
                  "selected_physical_dag_scope");
  }
  const auto& node = physical_dag.nodes.front();
  if (node.relational_node_id != alternative->logical_node_id ||
      node.implementation_id != alternative->implementation_id ||
      node.selected_alternative_uuid != alternative->alternative_uuid ||
      node.executor_capability_uuid != alternative->capability_uuid ||
      node.output_descriptor_ids != alternative->output_descriptor_ids ||
      !node.engine_capability_validated || node.causal_counter_id == 0) {
    return refuse("QOW-DIAG-OPT-004-PUBLICATION-V1",
                  "selected_alternative_identity");
  }
  if (!execution.accepted || !execution.exact_selected_nodes_executed ||
      !execution.causal_counters_attached ||
      !execution.canonical_result_published || execution.replan_required ||
      !execution.issues.empty() || !execution.dispatch.diagnostic.ok ||
      !execution.dispatch.execution_started ||
      execution.dispatch.selected_plan_uuid !=
          physical_dag.selected_plan_uuid ||
      execution.dispatch.executed_steps.size() != 1 ||
      execution.runtime_actuals.node_actuals.size() != 1) {
    return refuse("QOW-DIAG-OPT-004-EXECUTION-V1",
                  "canonical_selected_execution");
  }
  const auto& step = execution.dispatch.executed_steps.front();
  const auto& actual = execution.runtime_actuals.node_actuals.front();
  if (!step.execution_started || !step.execution_finished ||
      !step.counters_captured_after_finish ||
      step.executed_physical_node_id != node.physical_node_id ||
      step.causal_counter_id != node.causal_counter_id ||
      actual.physical_node_id != node.physical_node_id ||
      actual.logical_node_id != node.relational_node_id ||
      actual.causal_counter_id != node.causal_counter_id ||
      !actual.execution_started || !actual.execution_finished ||
      !actual.counters_captured_after_finish) {
    return refuse("QOW-DIAG-OPT-004-CAUSAL-RECEIPT-V1",
                  "executed_node_and_counter");
  }
  return true;
}

}  // namespace scratchbird::engine::internal_api
