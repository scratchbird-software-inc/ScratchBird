// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#include "../../src/engine/optimizer/model_family_coordinator.hpp"

#include <cstdio>
#include <iostream>
#include <string>
#include <string_view>

namespace optimizer = scratchbird::engine::optimizer;
namespace executor = scratchbird::engine::executor;

namespace {

std::string Uuid(const std::uint64_t value) {
  char buffer[37];
  std::snprintf(buffer, sizeof(buffer), "019f0000-0000-7000-8000-%012llx",
                static_cast<unsigned long long>(value));
  return buffer;
}

executor::PhysicalMgaStatementContext Mga() {
  executor::PhysicalMgaStatementContext context;
  context.statement_uuid = Uuid(1);
  context.statement_timestamp = "2026-08-12T20:00:00Z";
  context.owning_transaction_uuid = Uuid(2);
  context.statement_snapshot_uuid = Uuid(3);
  context.statement_metadata_snapshot_uuid = Uuid(4);
  context.owning_local_transaction_id = 40;
  context.visible_committed_high_watermark = 39;
  context.oldest_active_transaction_id = 30;
  context.oldest_interesting_transaction_id = 29;
  context.oldest_snapshot_transaction_id = 29;
  context.retention_horizon_transaction_id = 29;
  context.active_excluded_local_transaction_ids = {40};
  context.snapshot_kind = "statement_stable";
  context.publication_inventory_next_local_transaction_id = 41;
  context.inventory_authoritative = true;
  context.complete = true;
  context.current = true;
  return context;
}

optimizer::ModelFamilyCostVectorV1 Cost(const std::uint64_t seed) {
  optimizer::ModelFamilyCostVectorV1 cost;
  cost.cost_vector_uuid = Uuid(seed);
  cost.provenance_uuid = Uuid(seed + 100);
  cost.provenance_generation = 7;
  cost.confidence_basis_points = 9000;
  cost.cpu_units = 10;
  cost.sequential_read_units = 20;
  cost.random_read_units = 5;
  cost.memory_bytes_required = 128;
  cost.uncertainty_penalty = 2;
  cost.risk_penalty = 1;
  return cost;
}

optimizer::ModelFamilyDependencyLegV1 Leg(
    const std::uint16_t ordinal,
    const std::string& family) {
  optimizer::ModelFamilyDependencyLegV1 leg;
  leg.lexical_source_ordinal = ordinal;
  leg.physical_node_uuid = Uuid(100 + ordinal);
  leg.family_id = family;
  leg.operation_id = family == "relational" ? "RELATIONAL_HEAP_SCAN"
                     : family == "document" ? "DOCUMENT_FIND"
                                              : "GRAPH_MATCH";
  leg.selected_plan_uuid = Uuid(200 + ordinal);
  leg.selected_alternative_uuid = Uuid(300 + ordinal);
  leg.provider_uuid = Uuid(400 + ordinal);
  leg.capability_uuid = Uuid(500 + ordinal);
  leg.delivered_property_uuid = Uuid(600 + ordinal);
  leg.bound_object_uuid = Uuid(700 + ordinal);
  leg.catalog_snapshot_uuid = leg.current_catalog_snapshot_uuid = Uuid(800);
  leg.descriptor_snapshot_uuid =
      leg.current_descriptor_snapshot_uuid = Uuid(801);
  leg.security_context_uuid = leg.current_security_context_uuid = Uuid(802);
  leg.policy_snapshot_uuid = leg.current_policy_snapshot_uuid = Uuid(803);
  leg.resource_contract_uuid = leg.current_resource_contract_uuid = Uuid(804);
  leg.operation_scope_receipt_uuid = Uuid(900 + ordinal);
  leg.selected_alternative_receipt_uuid = Uuid(1000 + ordinal);
  leg.root_physical_node_id = ordinal + 1;
  leg.output_descriptor_ids = {static_cast<std::uint32_t>(100 + ordinal)};
  leg.output_descriptor_uuids = {Uuid(1100 + ordinal)};
  leg.family_local_cost = Cost(1200 + ordinal);
  optimizer::ModelFamilyDependencyAlternativeV1 candidate;
  candidate.alternative_uuid = leg.selected_alternative_uuid;
  candidate.candidate_inventory_receipt_uuid = Uuid(1300 + ordinal);
  candidate.implementation_id = "fixture_exact_family_route_v1";
  candidate.operation_ids = leg.operation_ids;
  candidate.operation_id = leg.operation_id;
  candidate.operation_scope_receipt_uuid = leg.operation_scope_receipt_uuid;
  candidate.selection_policy_receipt_uuid =
      leg.selected_alternative_receipt_uuid;
  candidate.authority_approved_comparison_rank = 1;
  candidate.family_local_cost = leg.family_local_cost;
  candidate.available = true;
  candidate.exact = true;
  candidate.admitted = true;
  leg.candidate_alternatives = {candidate};
  leg.mga_statement_context = Mga();
  leg.catalog_generation = leg.current_catalog_generation = 1;
  leg.descriptor_generation = leg.current_descriptor_generation = 2;
  leg.security_generation = leg.current_security_generation = 3;
  leg.policy_generation = leg.current_policy_generation = 4;
  leg.resource_generation = leg.current_resource_generation = 5;
  leg.provider_generation = leg.current_provider_generation = 6;
  leg.capability_generation = leg.current_capability_generation = 7;
  leg.memory_grant_bytes = 128;
  leg.exchange_buffer_bytes = 64;
  leg.maximum_rows = 4;
  leg.maximum_columns = 1;
  leg.maximum_cells = 4;
  leg.selected = true;
  leg.security_admitted = true;
  leg.capability_admitted = true;
  leg.exact = true;
  leg.exact_fallback_available = true;
  leg.cleanup_supported = true;
  leg.cancellation_supported = true;
  leg.spill_eligible = true;
  return leg;
}

optimizer::ModelFamilyDependencyEdgeV1 Edge(
    const optimizer::ModelFamilyDependencyCoordinatorRequestV1& request,
    const std::uint16_t producer,
    const std::uint16_t consumer,
    const std::uint64_t seed) {
  optimizer::ModelFamilyDependencyEdgeV1 edge;
  edge.edge_uuid = Uuid(seed);
  edge.producer_lexical_source_ordinal = producer;
  edge.consumer_lexical_source_ordinal = consumer;
  edge.required_property_uuid =
      request.legs[producer].delivered_property_uuid;
  edge.delivered_property_uuid = edge.required_property_uuid;
  edge.descriptor_lineage_uuid = Uuid(seed + 100);
  edge.producer_output_descriptor_ids =
      request.legs[producer].output_descriptor_ids;
  edge.consumer_input_descriptor_ids = edge.producer_output_descriptor_ids;
  edge.producer_output_descriptor_uuids =
      request.legs[producer].output_descriptor_uuids;
  edge.consumer_input_descriptor_uuids =
      edge.producer_output_descriptor_uuids;
  return edge;
}

optimizer::ModelFamilyRelationalConsumerV1 Consumer(
    const std::string& uuid,
    const std::uint64_t node_id,
    std::vector<std::string> inputs,
    std::vector<std::uint32_t> descriptor_ids,
    std::vector<std::string> descriptor_uuids,
    const bool root) {
  optimizer::ModelFamilyRelationalConsumerV1 consumer;
  consumer.physical_node_uuid = uuid;
  consumer.physical_node_id = node_id;
  consumer.causal_counter_id = node_id + 100;
  consumer.selected_implementation_uuid = Uuid(1500 + node_id);
  consumer.expected_security_receipt_uuid = Uuid(1600 + node_id);
  consumer.join_form_id = "INNER";
  consumer.input_physical_node_uuids = std::move(inputs);
  consumer.input_descriptor_ids = descriptor_ids;
  consumer.output_descriptor_ids = descriptor_ids;
  consumer.input_descriptor_uuids = descriptor_uuids;
  consumer.output_descriptor_uuids = descriptor_uuids;
  consumer.mga_statement_context = Mga();
  consumer.maximum_rows = 4;
  consumer.maximum_columns = descriptor_ids.size();
  consumer.maximum_cells = 4 * descriptor_ids.size();
  consumer.memory_grant_bytes = 128;
  consumer.canonical_root = root;
  consumer.exact = true;
  consumer.cleanup_supported = true;
  consumer.cancellation_supported = true;
  return consumer;
}

optimizer::ModelFamilyDependencyCoordinatorRequestV1 Valid() {
  optimizer::ModelFamilyDependencyCoordinatorRequestV1 request;
  request.composition_profile_id = "COMP-3-LINEAR-V1";
  request.bound_sblr_tree_uuid = Uuid(20);
  request.selected_plan_generation =
      request.current_selected_plan_generation = 9;
  request.canonical_root_physical_node_uuid = Uuid(201);
  request.canonical_root_physical_node_id = 11;
  request.legs = {Leg(0, "relational"), Leg(1, "document"), Leg(2, "graph")};
  request.edges = {Edge(request, 0, 1, 1400), Edge(request, 1, 2, 1401)};
  request.relational_consumers = {
      Consumer(Uuid(200), 10,
               {request.legs[0].physical_node_uuid,
                request.legs[1].physical_node_uuid},
               {100, 101}, {Uuid(1100), Uuid(1101)}, false),
      Consumer(Uuid(201), 11,
               {Uuid(200), request.legs[2].physical_node_uuid},
               {100, 101, 102},
               {Uuid(1100), Uuid(1101), Uuid(1102)}, true)};
  request.statement_memory_budget_bytes = 1024;
  request.backpressure_high_watermark_rows = 4;
  request.backpressure_low_watermark_rows = 2;
  request.signed_short_circuit_enabled = true;
  request.feedback_observation_frozen = true;
  request.feedback_target_is_later_plan = true;
  request.feedback_observation_generation = 9;
  request.feedback_target_plan_generation = 10;
  return request;
}

bool Require(const bool condition, const std::string_view detail) {
  if (!condition) std::cerr << "QOW-OPT-007-COORDINATOR: " << detail << '\n';
  return condition;
}

bool Refuses(optimizer::ModelFamilyDependencyCoordinatorRequestV1 request,
             const std::string_view diagnostic) {
  const auto result = optimizer::CoordinateModelFamilyDependencyDagV1(request);
  return Require(!result.accepted && !result.data_access_allowed &&
                     !result.root_publication_candidate &&
                     result.no_partial_root && result.diagnostic_id == diagnostic,
                 std::string("expected refusal ") + std::string(diagnostic) +
                     ", got " + result.diagnostic_id + ":" + result.detail);
}

optimizer::ModelFamilyCoordinatorRequestV1 SourceRequest(
    const std::string& family, const bool timestamp) {
  optimizer::ModelFamilyCoordinatorRequestV1 request;
  request.family_id = family;
  request.operation_id = family == "document" ? "DOCUMENT_FIND"
                                                : "GRAPH_MATCH";
  request.logical_operator_id = family == "document"
                                    ? "LOGICAL_DOCUMENT_SOURCE_V1"
                                    : "LOGICAL_GRAPH_SOURCE_V1";
  request.logical_node_id = 71;
  request.object_uuid = Uuid(1701);
  request.output_descriptor_ids = {171};
  request.mga_statement_context = Mga();
  if (!timestamp) request.mga_statement_context.statement_timestamp.clear();
  request.bound_sblr_tree_uuid = Uuid(1702);
  request.catalog_epoch_uuid = Uuid(1703);
  request.security_context_uuid = Uuid(1704);
  request.capability_snapshot_uuid = Uuid(1705);
  request.resource_snapshot_uuid = Uuid(1706);
  request.statistics_snapshot_uuid = Uuid(1707);
  request.route_snapshot_uuid = Uuid(1708);
  request.catalog_generation = request.current_catalog_generation = 1;
  request.security_epoch = 1;
  request.policy_epoch = 1;
  request.resource_epoch = 1;
  request.statistics_generation = 1;
  request.route_epoch = 1;
  request.route_generation = 1;
  request.memory_budget_bytes = 4096;
  optimizer::ModelFamilyCandidateV1 candidate;
  candidate.alternative_uuid = Uuid(1710);
  candidate.provider_uuid = Uuid(1711);
  candidate.capability_uuid = Uuid(1712);
  candidate.implementation_id = family == "document"
                                    ? "physical_document_path_scan_v1"
                                    : "physical_graph_adjacency_scan_v1";
  candidate.provider_generation = 1;
  candidate.available = true;
  candidate.exact = true;
  candidate.exact_collection_fallback = true;
  candidate.cost.cost_vector_uuid = Uuid(1713);
  candidate.cost.cpu_units = 1;
  candidate.cost.sequential_read_units = 1;
  candidate.cost.memory_bytes_required = 128;
  request.candidates = {std::move(candidate)};
  return request;
}

bool SourceRefuses(optimizer::ModelFamilyCoordinatorRequestV1 request,
                   const std::string_view diagnostic) {
  const auto result = optimizer::CoordinateModelFamilySourceV1(request);
  return Require(!result.accepted && !result.selected &&
                     !result.data_access_allowed &&
                     result.diagnostic_id == diagnostic,
                 std::string("expected source refusal ") +
                     std::string(diagnostic) + ", got " +
                     result.diagnostic_id + ":" + result.detail);
}

}  // namespace

int main() {
  bool passed = true;
  const auto valid = Valid();
  const auto accepted =
      optimizer::CoordinateModelFamilyDependencyDagV1(valid);
  passed &= Require(accepted.accepted && accepted.deterministic &&
                        accepted.data_access_allowed &&
                        accepted.root_publication_candidate &&
                        accepted.no_partial_root &&
                        accepted.stable_schedule.size() == 3 &&
                        accepted.relational_consumers.size() == 2 &&
                        accepted.rule_receipts.size() == 24 &&
                        accepted.admitted_peak_memory_bytes == 832,
                    "valid 24-rule coordinator admission drifted");
  for (std::size_t index = 0; index < accepted.rule_receipts.size(); ++index) {
    const auto expected_rule = "COORD-" +
        (index + 1 < 10 ? std::string("00") : std::string("0")) +
        std::to_string(index + 1) + "-V1";
    const bool expected_complete =
        index <= 10 || index == 13 || index == 22 || index == 23;
    passed &= Require(accepted.rule_receipts[index].rule_id == expected_rule &&
                          accepted.rule_receipts[index].complete ==
                              expected_complete,
                      expected_rule + " receipt state drifted");
  }

  auto bad = valid;
  bad.legs[0].output_descriptor_uuids.clear();
  passed &= Refuses(bad, "SB_MODEL_BINDING_INCOMPLETE_V1");
  bad = valid;
  bad.legs[0].current_catalog_snapshot_uuid = Uuid(9999);
  passed &= Refuses(bad, "SB_MODEL_CATALOG_GENERATION_STALE_V1");
  bad = valid;
  bad.legs[0].security_admitted = false;
  passed &= Refuses(bad, "SB_MODEL_SECURITY_ADMISSION_REFUSED_V1");
  bad = valid;
  bad.legs[1].mga_statement_context.statement_uuid = Uuid(9999);
  passed &= Refuses(bad, "SB_MODEL_MGA_CONTEXT_MISMATCH_V1");
  bad = valid;
  bad.legs[0].operation_scope_receipt_uuid.clear();
  passed &= Refuses(bad, "SB_MODEL_CAPABILITY_UNAVAILABLE_V1");
  bad = valid;
  bad.legs[0].candidate_alternatives.clear();
  passed &= Refuses(bad, "SB_MODEL_CANDIDATE_SEMANTICS_MISSING_V1");
  bad = valid;
  bad.legs[0].family_local_cost.provenance_uuid.clear();
  passed &= Refuses(bad, "SB_MODEL_COST_VECTOR_INVALID_V1");
  bad = valid;
  bad.edges.pop_back();
  passed &= Refuses(bad, "SB_MODEL_DEPENDENCY_DAG_INVALID_V1");
  bad = valid;
  bad.edges[0].consumer_input_descriptor_uuids[0] = Uuid(9999);
  passed &= Refuses(bad, "SB_MODEL_PROPERTY_UNSATISFIED_V1");
  bad = valid;
  bad.legs[0].candidate_alternatives[0].authority_approved_comparison_rank = 2;
  auto second = bad.legs[0].candidate_alternatives[0];
  second.alternative_uuid = Uuid(50);
  second.authority_approved_comparison_rank = 1;
  bad.legs[0].candidate_alternatives.push_back(second);
  passed &= Refuses(bad, "SB_MODEL_NO_ADMITTED_ALTERNATIVE_V1");
  bad = valid;
  bad.statement_memory_budget_bytes = 831;
  passed &= Refuses(bad, "SB_MODEL_RESOURCE_MEMORY_REFUSED_V1");
  bad = valid;
  bad.spill_required = true;
  bad.maximum_spill_bytes = 4096;
  passed &= Refuses(bad, "SB_MODEL_RESOURCE_SPILL_REFUSED_V1");
  bad = valid;
  bad.legs[0].maximum_cells = 5;
  passed &= Refuses(bad, "SB_MODEL_RESOURCE_ROW_LIMIT_V1");
  bad = valid;
  bad.composition_profile_id = "COMP-3-INDEPENDENT-V1";
  bad.edges.clear();
  passed &= Refuses(bad, "SB_MODEL_PARALLEL_ADMISSION_REFUSED_V1");
  bad = valid;
  bad.backpressure_high_watermark_rows = 2;
  passed &= Refuses(bad, "SB_MODEL_BACKPRESSURE_PROTOCOL_FAILED_V1");
  bad = valid;
  bad.signed_short_circuit_enabled = false;
  passed &= Refuses(bad, "SB_MODEL_SHORT_CIRCUIT_STATE_INVALID_V1");
  bad = valid;
  bad.legs[0].cluster_scope_required = true;
  passed &= Refuses(bad, "SB_MODEL_CLUSTER_CAPABILITY_UNAVAILABLE_V1");
  bad = valid;
  bad.current_plan_mutation_requested = true;
  passed &= Refuses(bad,
                    "SB_MODEL_FEEDBACK_CURRENT_PLAN_MUTATION_REFUSED_V1");
  bad = valid;
  bad.relational_consumers[1].causal_counter_id =
      bad.relational_consumers[0].causal_counter_id;
  passed &= Refuses(bad, "SB_MODEL_DEPENDENCY_DAG_INVALID_V1");
  bad = valid;
  bad.relational_consumers[0].causal_counter_id =
      bad.relational_consumers[1].causal_counter_id + 1;
  passed &= Refuses(bad, "SB_MODEL_DEPENDENCY_DAG_INVALID_V1");
  bad = valid;
  bad.legs[1].operation_id = "DOCUMENT_PATH";
  passed &= Refuses(bad, "SB_MODEL_NO_ADMITTED_ALTERNATIVE_V1");
  bad = valid;
  bad.legs[2].candidate_alternatives[0].operation_id = "GRAPH_EXPAND";
  passed &= Refuses(bad, "SB_MODEL_NO_ADMITTED_ALTERNATIVE_V1");

  const auto standalone = SourceRequest("document", false);
  passed &= Require(
      optimizer::CoordinateModelFamilySourceV1(standalone).accepted,
      "standalone document source with absent timestamp was refused");
  auto source_bad = standalone;
  source_bad.mga_statement_context.statement_timestamp =
      "2026-08-12T20:00:00Z";
  passed &= SourceRefuses(
      source_bad, "SB_MODEL_KEY_VALUE_STATEMENT_TIMESTAMP_INVALID_V1");
  auto composed = source_bad;
  composed.composition_profile_id = "COMP-3-LINEAR-V1";
  composed.composition_lexical_source_ordinal = 1;
  composed.composition_arity = 3;
  passed &= Require(
      optimizer::CoordinateModelFamilySourceV1(composed).accepted,
      "signed three-leg common timestamp carrier was refused");
  source_bad = composed;
  source_bad.composition_arity = 2;
  passed &= SourceRefuses(
      source_bad, "SB_MODEL_KEY_VALUE_STATEMENT_TIMESTAMP_INVALID_V1");
  source_bad = composed;
  source_bad.composition_lexical_source_ordinal = 3;
  passed &= SourceRefuses(
      source_bad, "SB_MODEL_KEY_VALUE_STATEMENT_TIMESTAMP_INVALID_V1");
  auto mixed_graph = SourceRequest("graph", true);
  mixed_graph.composition_profile_id = "COMP-4-MIXED-V1";
  mixed_graph.composition_lexical_source_ordinal = 2;
  mixed_graph.composition_arity = 4;
  passed &= Require(
      optimizer::CoordinateModelFamilySourceV1(mixed_graph).accepted,
      "signed four-leg family/ordinal timestamp carrier was refused");
  source_bad = mixed_graph;
  source_bad.composition_lexical_source_ordinal = 1;
  passed &= SourceRefuses(
      source_bad, "SB_MODEL_KEY_VALUE_STATEMENT_TIMESTAMP_INVALID_V1");

  if (!passed) return 1;
  std::cout << "QOW-OPT-007-COORDINATOR: passed;rules=24;admission_refusals=22;source_composition_carriers=7;runtime_receipts_pending=10\n";
  return 0;
}
