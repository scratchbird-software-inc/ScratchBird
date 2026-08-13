// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#include "../../src/engine/executor/model_family_executor.hpp"

#include <atomic>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <set>
#include <string>
#include <string_view>

namespace optimizer = scratchbird::engine::optimizer;
namespace executor = scratchbird::engine::executor;
namespace memory = scratchbird::core::memory;

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
  cost.provenance_generation = 1;
  cost.confidence_basis_points = 9000;
  cost.cpu_units = 1;
  cost.memory_bytes_required = 64 * 1024;
  return cost;
}

optimizer::ModelFamilyDependencyCoordinatorRequestV1 Admission(
    const std::uint64_t maximum_spill_bytes = 349) {
  optimizer::ModelFamilyDependencyCoordinatorRequestV1 request;
  request.composition_profile_id = "COMP-3-INDEPENDENT-V1";
  request.bound_sblr_tree_uuid = Uuid(20);
  request.selected_plan_generation = request.current_selected_plan_generation = 9;
  request.canonical_root_physical_node_uuid = Uuid(201);
  request.canonical_root_physical_node_id = 11;
  for (std::uint16_t ordinal = 0; ordinal < 3; ++ordinal) {
    optimizer::ModelFamilyDependencyLegV1 leg;
    leg.lexical_source_ordinal = ordinal;
    leg.physical_node_uuid = Uuid(102 - ordinal);
    leg.family_id =
        ordinal == 0 ? "relational" : ordinal == 1 ? "document" : "graph";
    leg.operation_id = ordinal == 0 ? "RELATIONAL_HEAP_SCAN"
                                    : ordinal == 1 ? "DOCUMENT_FIND"
                                                   : "GRAPH_MATCH";
    leg.selected_plan_uuid = Uuid(200 + ordinal);
    leg.selected_alternative_uuid = Uuid(300 + ordinal);
    leg.provider_uuid = Uuid(400 + ordinal);
    leg.capability_uuid = Uuid(500 + ordinal);
    leg.delivered_property_uuid = Uuid(600 + ordinal);
    leg.bound_object_uuid = Uuid(700 + ordinal);
    leg.catalog_snapshot_uuid = leg.current_catalog_snapshot_uuid = Uuid(800);
    leg.descriptor_snapshot_uuid = leg.current_descriptor_snapshot_uuid = Uuid(801);
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
    candidate.implementation_id =
        ordinal == 0 ? "relational_heap_scan_v1"
                     : ordinal == 1 ? "document_find_v1" : "graph_match_v1";
    candidate.operation_ids = leg.operation_ids;
    candidate.operation_id = leg.operation_id;
    candidate.operation_scope_receipt_uuid = leg.operation_scope_receipt_uuid;
    candidate.selection_policy_receipt_uuid = leg.selected_alternative_receipt_uuid;
    candidate.authority_approved_comparison_rank = 1;
    candidate.family_local_cost = leg.family_local_cost;
    candidate.available = candidate.exact = candidate.admitted = true;
    leg.candidate_alternatives = {candidate};
    leg.mga_statement_context = Mga();
    leg.catalog_generation = leg.current_catalog_generation = 1;
    leg.descriptor_generation = leg.current_descriptor_generation = 2;
    leg.security_generation = leg.current_security_generation = 3;
    leg.policy_generation = leg.current_policy_generation = 4;
    leg.resource_generation = leg.current_resource_generation = 5;
    leg.provider_generation = leg.current_provider_generation = 6;
    leg.capability_generation = leg.current_capability_generation = 7;
    leg.memory_grant_bytes = 64 * 1024;
    leg.exchange_buffer_bytes = 64 * 1024;
    leg.maximum_rows = 3;
    leg.maximum_columns = 1;
    leg.maximum_cells = 3;
    leg.selected = leg.security_admitted = leg.capability_admitted = true;
    leg.exact = leg.exact_fallback_available = true;
    leg.cleanup_supported = leg.cancellation_supported = true;
    leg.parallel_eligible = leg.spill_eligible = true;
    request.legs.push_back(std::move(leg));
  }
  const auto consumer = [&](const std::string& uuid,
                            const std::uint64_t id,
                            const std::uint64_t causal,
                            std::vector<std::string> inputs,
                            std::vector<std::uint32_t> ids,
                            std::vector<std::string> uuids,
                            const std::uint64_t rows,
                            const bool root) {
    optimizer::ModelFamilyRelationalConsumerV1 value;
    value.physical_node_uuid = uuid;
    value.physical_node_id = id;
    value.causal_counter_id = causal;
    value.selected_implementation_uuid = Uuid(1500 + id);
    value.expected_security_receipt_uuid = Uuid(1600 + id);
    value.join_form_id = "CROSS";
    value.input_physical_node_uuids = std::move(inputs);
    value.input_descriptor_ids = ids;
    value.output_descriptor_ids = ids;
    value.input_descriptor_uuids = uuids;
    value.output_descriptor_uuids = uuids;
    value.mga_statement_context = Mga();
    value.maximum_rows = rows;
    value.maximum_columns = ids.size();
    value.maximum_cells = rows * ids.size();
    value.memory_grant_bytes = 1024 * 1024;
    value.canonical_root = root;
    value.exact = value.cleanup_supported = value.cancellation_supported = true;
    return value;
  };
  request.relational_consumers = {
      consumer(Uuid(200), 10, 100,
               {request.legs[0].physical_node_uuid,
                request.legs[1].physical_node_uuid},
               {100, 101}, {Uuid(1100), Uuid(1101)}, 9, false),
      consumer(Uuid(201), 11, 101,
               {Uuid(200), request.legs[2].physical_node_uuid},
               {100, 101, 102},
               {Uuid(1100), Uuid(1101), Uuid(1102)}, 27, true)};
  request.statement_memory_budget_bytes = 4 * 1024 * 1024;
  request.maximum_spill_bytes = maximum_spill_bytes;
  request.backpressure_high_watermark_rows = 2;
  request.backpressure_low_watermark_rows = 1;
  request.spill_required = true;
  request.engine_temporary_storage_available = true;
  request.spill_cleanup_path_available = true;
  request.signed_short_circuit_enabled = true;
  request.feedback_observation_frozen = true;
  request.feedback_target_is_later_plan = true;
  request.feedback_observation_generation = 9;
  request.feedback_target_plan_generation = 10;
  return request;
}

optimizer::ModelFamilyDependencyCoordinatorRequestV1 AdmissionForProfile(
    const std::string& profile_id,
    const std::uint64_t maximum_spill_bytes = 349) {
  auto request = Admission(maximum_spill_bytes);
  request.composition_profile_id = profile_id;
  for (auto& leg : request.legs) leg.parallel_eligible = true;
  if (profile_id == "COMP-4-MIXED-V1") {
    auto vector_leg = request.legs.back();
    vector_leg.lexical_source_ordinal = 3;
    vector_leg.physical_node_uuid = Uuid(99);
    vector_leg.family_id = "vector";
    vector_leg.operation_ids.clear();
    vector_leg.operation_id = "VECTOR_EXACT_SEARCH";
    vector_leg.selected_plan_uuid = Uuid(203);
    vector_leg.selected_alternative_uuid = Uuid(303);
    vector_leg.provider_uuid = Uuid(403);
    vector_leg.capability_uuid = Uuid(503);
    vector_leg.delivered_property_uuid = Uuid(603);
    vector_leg.bound_object_uuid = Uuid(703);
    vector_leg.operation_scope_receipt_uuid = Uuid(903);
    vector_leg.selected_alternative_receipt_uuid = Uuid(1003);
    vector_leg.root_physical_node_id = 4;
    vector_leg.output_descriptor_ids = {103, 104, 105};
    vector_leg.output_descriptor_uuids = {Uuid(1103), Uuid(1104), Uuid(1105)};
    vector_leg.family_local_cost = Cost(1203);
    auto& vector_candidate = vector_leg.candidate_alternatives.front();
    vector_candidate.alternative_uuid = vector_leg.selected_alternative_uuid;
    vector_candidate.candidate_inventory_receipt_uuid = Uuid(1303);
    vector_candidate.implementation_id = "vector_exact_search_v1";
    vector_candidate.operation_ids = vector_leg.operation_ids;
    vector_candidate.operation_id = vector_leg.operation_id;
    vector_candidate.operation_scope_receipt_uuid =
        vector_leg.operation_scope_receipt_uuid;
    vector_candidate.selection_policy_receipt_uuid =
        vector_leg.selected_alternative_receipt_uuid;
    vector_candidate.family_local_cost = vector_leg.family_local_cost;
    vector_leg.maximum_columns = 3;
    vector_leg.maximum_cells = 9;
    request.legs.push_back(std::move(vector_leg));

    request.relational_consumers[1].canonical_root = false;
    auto root = request.relational_consumers[1];
    root.physical_node_uuid = Uuid(202);
    root.physical_node_id = 12;
    root.causal_counter_id = 102;
    root.selected_implementation_uuid = Uuid(1512);
    root.expected_security_receipt_uuid = Uuid(1612);
    root.input_physical_node_uuids = {
        request.relational_consumers[1].physical_node_uuid,
        request.legs[3].physical_node_uuid};
    root.input_descriptor_ids = {100, 101, 102, 103, 104, 105};
    root.output_descriptor_ids = root.input_descriptor_ids;
    root.input_descriptor_uuids = {Uuid(1100), Uuid(1101), Uuid(1102),
                                   Uuid(1103), Uuid(1104), Uuid(1105)};
    root.output_descriptor_uuids = root.input_descriptor_uuids;
    root.maximum_rows = 81;
    root.maximum_columns = 6;
    root.maximum_cells = 486;
    root.canonical_root = true;
    request.relational_consumers.push_back(std::move(root));
    request.canonical_root_physical_node_uuid = Uuid(202);
    request.canonical_root_physical_node_id = 12;
  } else if (profile_id == "COMP-3-SHARED-LEG-V1") {
    auto branch = request.relational_consumers.front();
    branch.physical_node_uuid = Uuid(202);
    branch.physical_node_id = 12;
    branch.causal_counter_id = 101;
    branch.selected_implementation_uuid = Uuid(1512);
    branch.expected_security_receipt_uuid = Uuid(1612);
    branch.input_physical_node_uuids = {
        request.legs[0].physical_node_uuid,
        request.legs[2].physical_node_uuid};
    branch.input_descriptor_ids = {100, 102};
    branch.output_descriptor_ids = branch.input_descriptor_ids;
    branch.input_descriptor_uuids = {Uuid(1100), Uuid(1102)};
    branch.output_descriptor_uuids = branch.input_descriptor_uuids;
    branch.canonical_root = false;

    auto root = request.relational_consumers.back();
    root.physical_node_uuid = Uuid(203);
    root.physical_node_id = 13;
    root.causal_counter_id = 102;
    root.selected_implementation_uuid = Uuid(1513);
    root.expected_security_receipt_uuid = Uuid(1613);
    root.input_physical_node_uuids = {
        request.relational_consumers.front().physical_node_uuid,
        branch.physical_node_uuid};
    root.input_descriptor_ids = {100, 101, 100, 102};
    root.output_descriptor_ids = {100, 101, 102};
    root.input_descriptor_uuids = {Uuid(1100), Uuid(1101), Uuid(1100),
                                   Uuid(1102)};
    root.output_descriptor_uuids = {Uuid(1100), Uuid(1101), Uuid(1102)};
    root.maximum_rows = 81;
    root.maximum_columns = 3;
    root.maximum_cells = 243;
    root.canonical_root = true;
    request.relational_consumers = {
        request.relational_consumers.front(), std::move(branch),
        std::move(root)};
    request.canonical_root_physical_node_uuid = Uuid(203);
    request.canonical_root_physical_node_id = 13;
  }
  if (profile_id == "COMP-3-LINEAR-V1" ||
      profile_id == "COMP-3-LATERAL-V1" ||
      profile_id == "COMP-3-SHORT-CIRCUIT-V1") {
    request.edges = {
        optimizer::ModelFamilyDependencyEdgeV1{},
        optimizer::ModelFamilyDependencyEdgeV1{}};
    const auto bind_edge = [&](auto& edge, const std::uint16_t producer,
                               const std::uint16_t consumer,
                               const std::uint64_t seed) {
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
    };
    bind_edge(request.edges[0], 0, 1, 5000);
    bind_edge(request.edges[1], 1, 2, 5001);
  } else if (profile_id == "COMP-3-FANIN-V1" ||
             profile_id == "COMP-3-FAILURE-CLEANUP-V1") {
    request.edges = {
        optimizer::ModelFamilyDependencyEdgeV1{},
        optimizer::ModelFamilyDependencyEdgeV1{}};
    const auto bind_edge = [&](auto& edge, const std::uint16_t producer,
                               const std::uint64_t seed) {
      edge.edge_uuid = Uuid(seed);
      edge.producer_lexical_source_ordinal = producer;
      edge.consumer_lexical_source_ordinal = 2;
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
    };
    bind_edge(request.edges[0], 0, 5010);
    bind_edge(request.edges[1], 1, 5011);
  } else if (profile_id == "COMP-3-SHARED-LEG-V1" ||
             profile_id == "COMP-4-MIXED-V1") {
    const std::vector<std::pair<std::uint16_t, std::uint16_t>> endpoints =
        profile_id == "COMP-3-SHARED-LEG-V1"
            ? std::vector<std::pair<std::uint16_t, std::uint16_t>>{{0, 1},
                                                                   {0, 2}}
            : std::vector<std::pair<std::uint16_t, std::uint16_t>>{{0, 1},
                                                                   {1, 2},
                                                                   {0, 3}};
    for (std::size_t index = 0; index < endpoints.size(); ++index) {
      optimizer::ModelFamilyDependencyEdgeV1 edge;
      edge.edge_uuid = Uuid(5020 + index);
      edge.producer_lexical_source_ordinal = endpoints[index].first;
      edge.consumer_lexical_source_ordinal = endpoints[index].second;
      edge.required_property_uuid =
          request.legs[endpoints[index].first].delivered_property_uuid;
      edge.delivered_property_uuid = edge.required_property_uuid;
      edge.descriptor_lineage_uuid = Uuid(5120 + index);
      edge.producer_output_descriptor_ids =
          request.legs[endpoints[index].first].output_descriptor_ids;
      edge.consumer_input_descriptor_ids =
          edge.producer_output_descriptor_ids;
      edge.producer_output_descriptor_uuids =
          request.legs[endpoints[index].first].output_descriptor_uuids;
      edge.consumer_input_descriptor_uuids =
          edge.producer_output_descriptor_uuids;
      request.edges.push_back(std::move(edge));
    }
  }
  return request;
}

executor::DescriptorBatch Batch(
    const optimizer::ModelFamilyDependencyLegV1& leg) {
  executor::DescriptorBatch batch;
  const bool vector = leg.family_id == "vector";
  const std::vector<std::pair<std::string, std::string>> schema =
      vector ? std::vector<std::pair<std::string, std::string>>{
                   {"row_uuid", "uuid"}, {"distance", "real64"},
                   {"score", "real64"}}
             : std::vector<std::pair<std::string, std::string>>{
                   {"r" + std::to_string(leg.lexical_source_ordinal), "uuid"}};
  for (std::size_t column = 0; column < schema.size(); ++column) {
    auto descriptor = executor::MakeExecutorDescriptor(
        schema[column].second,
        "canonical=" + schema[column].second + ";type_uuid=" +
            Uuid(1800 + leg.lexical_source_ordinal * 10 + column) +
            ";nullable=false");
    descriptor.descriptor_uuid.canonical = leg.output_descriptor_uuids[column];
    descriptor.descriptor_kind = "scalar";
    batch.columns.push_back({schema[column].first, descriptor, false,
                             leg.output_descriptor_ids[column]});
  }
  for (std::uint64_t row = 0; row < 3; ++row) {
    executor::DescriptorTuple tuple;
    tuple.values.push_back(executor::MakeExecutorValue(
        batch.columns[0].descriptor,
        Uuid(2000 + leg.lexical_source_ordinal * 10 + row)));
    if (vector) {
      tuple.values.push_back(executor::MakeExecutorValue(
          batch.columns[1].descriptor, std::to_string(row + 1)));
      tuple.values.push_back(executor::MakeExecutorValue(
          batch.columns[2].descriptor,
          std::array<std::string, 3>{"0.1", "0.2", "0.3"}[row]));
    }
    batch.rows.push_back(std::move(tuple));
  }
  return batch;
}

executor::ModelFamilyExecutionRequestV1 LegExecution(
    const optimizer::ModelFamilyDependencyCoordinatorResultV1& plan,
    const std::uint16_t ordinal,
    std::atomic_uint64_t* cleanup_count) {
  executor::ModelFamilyExecutionRequestV1 request;
  const auto& leg = plan.stable_schedule[std::ranges::distance(
      plan.stable_schedule.begin(),
      std::ranges::find_if(plan.stable_schedule, [&](const auto& value) {
        return value.leg.lexical_source_ordinal == ordinal;
      }))];
  const auto batch = Batch(leg.leg);
  request.input.family_id = leg.leg.family_id;
  request.input.operation_ids = leg.leg.operation_ids;
  request.input.operation_id = leg.leg.operation_id;
  request.input.object_uuid = leg.leg.bound_object_uuid;
  request.input.physical_node_id = leg.leg.root_physical_node_id;
  request.input.selected_alternative_uuid = leg.leg.selected_alternative_uuid;
  request.input.capability_uuid = leg.leg.capability_uuid;
  request.input.provider_uuid = leg.leg.provider_uuid;
  request.input.provider_generation = leg.leg.provider_generation;
  request.input.result_handle_uuid = Uuid(2200 + ordinal);
  request.input.causal_counter_id = leg.causal_counter_id;
  request.input.output_descriptor_ids = leg.leg.output_descriptor_ids;
  request.input.mga_statement_context = Mga();
  request.input.catalog_epoch_uuid = leg.leg.catalog_snapshot_uuid;
  request.input.security_context_uuid = leg.leg.security_context_uuid;
  request.input.policy_snapshot_uuid = leg.leg.policy_snapshot_uuid;
  request.input.resource_contract_uuid = leg.leg.resource_contract_uuid;
  request.input.catalog_generation = leg.leg.catalog_generation;
  request.input.descriptor_generation = leg.leg.descriptor_generation;
  request.input.security_generation = leg.leg.security_generation;
  request.input.policy_generation = leg.leg.policy_generation;
  request.input.resource_generation = leg.leg.resource_generation;
  request.input.maximum_rows = leg.leg.maximum_rows;
  request.input.maximum_cells = leg.leg.maximum_cells;
  request.input.maximum_memory_bytes = leg.leg.memory_grant_bytes;
  request.input.multimodel_composition_receipt_uuid =
      plan.composition_admission_receipt_uuid;
  request.input.multimodel_lexical_source_ordinal = ordinal;
  request.input.multimodel_composition_arity = plan.stable_schedule.size();
  request.input.multimodel_common_statement_context = true;
  request.capability.capability_uuid = request.input.capability_uuid;
  request.capability.family_id = request.input.family_id;
  request.capability.provider_uuid = request.input.provider_uuid;
  request.capability.provider_generation = request.input.provider_generation;
  request.capability.capability_generation = leg.leg.capability_generation;
  request.capability.available = request.capability.exact = true;
  request.capability.exact_collection_fallback_available = true;
  request.capability.cancellation_supported = true;
  request.capability.cleanup_supported = true;
  request.capability.residual_recheck_supported = true;
  request.capability.base_row_mga_recheck_supported = true;
  request.capability.security_recheck_supported = true;
  request.current_catalog_generation = request.input.catalog_generation;
  request.current_descriptor_generation = request.input.descriptor_generation;
  request.current_security_generation = request.input.security_generation;
  request.current_policy_generation = request.input.policy_generation;
  request.current_resource_generation = request.input.resource_generation;
  request.current_provider_generation = request.input.provider_generation;
  request.current_capability_generation = leg.leg.current_capability_generation;
  request.current_mga_statement_context = Mga();
  request.cancellation_requested = [] { return false; };
  request.cleanup_provider = [cleanup_count] {
    cleanup_count->fetch_add(1, std::memory_order_relaxed);
  };
  request.execute_provider =
      [batch, delivered_property_uuid = leg.leg.delivered_property_uuid](
          const auto& input) {
    executor::ModelProviderExecutionResultV1 result;
    result.ok = result.data_access_observed = true;
    result.rows_examined = batch.rows.size();
    auto& output = result.provider_batch;
    output.provider_uuid = input.provider_uuid;
    output.provider_generation = input.provider_generation;
    output.selected_alternative_uuid = input.selected_alternative_uuid;
    output.capability_uuid = input.capability_uuid;
    output.result_handle_uuid = input.result_handle_uuid;
    output.causal_counter_id = input.causal_counter_id;
    output.output_descriptor_ids = input.output_descriptor_ids;
    output.batch = batch;
    for (std::uint64_t row = 0; row < batch.rows.size(); ++row) {
      executor::ModelProviderRowIdentityV1 identity;
      identity.row_uuid = Uuid(2300 + input.physical_node_id * 10 + row);
      if (input.family_id == "document") {
        identity.document_uuid = Uuid(2600 + row);
      } else if (input.family_id == "graph") {
        identity.vertex_uuid = Uuid(2700 + row);
        identity.path_uuid = Uuid(2800 + row);
      } else if (input.family_id == "vector") {
        identity.row_uuid = batch.rows[row].values[0].encoded_value;
        identity.vector_distance = batch.rows[row].values[1].encoded_value;
        identity.vector_score = batch.rows[row].values[2].encoded_value;
      }
      output.ordered_row_identities.push_back(std::move(identity));
    }
    output.properties.property_uuid = delivered_property_uuid;
    output.properties.ordering_id =
        input.family_id == "vector"
            ? "vector_distance_row_uuid_ascending_v1"
            : "fixture_order";
    output.properties.uniqueness_id =
        input.family_id == "graph"
            ? "path_uuid"
            : input.family_id == "document" ? "document_uuid" : "row_uuid";
    output.properties.residual_recheck_complete = true;
    output.properties.base_row_mga_recheck_complete = true;
    output.properties.security_recheck_complete = true;
    output.mga_statement_context = input.mga_statement_context;
    output.security_receipt_uuid = Uuid(2500 + input.physical_node_id);
    output.multimodel_composition_receipt_uuid =
        input.multimodel_composition_receipt_uuid;
    output.multimodel_lexical_source_ordinal =
        input.multimodel_lexical_source_ordinal;
    output.multimodel_composition_arity = input.multimodel_composition_arity;
    output.multimodel_common_statement_context = true;
    output.residual_recheck_complete = true;
    output.base_row_mga_recheck_complete = true;
    output.security_recheck_complete = true;
        return result;
      };
  return request;
}

executor::ModelFamilyCompositionPublicationStateV1 Publication(
    const optimizer::ModelFamilyDependencyCoordinatorResultV1& plan) {
  executor::ModelFamilyCompositionPublicationStateV1 state;
  state.current_selected_plan_generation = plan.selected_plan_generation;
  state.current_mga_statement_context = Mga();
  state.security_admitted = true;
  std::vector<const optimizer::ModelFamilyDependencyLegV1*> legs(
      plan.stable_schedule.size());
  for (const auto& scheduled : plan.stable_schedule)
    legs[scheduled.leg.lexical_source_ordinal] = &scheduled.leg;
  for (const auto* leg : legs) {
    state.current_catalog_generations.push_back(leg->current_catalog_generation);
    state.current_descriptor_generations.push_back(leg->current_descriptor_generation);
    state.current_security_generations.push_back(leg->current_security_generation);
    state.current_policy_generations.push_back(leg->current_policy_generation);
    state.current_resource_generations.push_back(leg->current_resource_generation);
    state.current_provider_generations.push_back(leg->current_provider_generation);
    state.current_capability_generations.push_back(leg->current_capability_generation);
    state.current_catalog_snapshot_uuids.push_back(leg->current_catalog_snapshot_uuid);
    state.current_descriptor_snapshot_uuids.push_back(leg->current_descriptor_snapshot_uuid);
    state.current_security_context_uuids.push_back(leg->current_security_context_uuid);
    state.current_policy_snapshot_uuids.push_back(leg->current_policy_snapshot_uuid);
    state.current_resource_contract_uuids.push_back(leg->current_resource_contract_uuid);
    state.current_provider_uuids.push_back(leg->provider_uuid);
    state.current_capability_uuids.push_back(leg->capability_uuid);
  }
  return state;
}

struct RuntimeCounters {
  std::atomic_uint64_t provider_cleanups{0};
  std::atomic_uint64_t exchange_cleanups{0};
  std::atomic_uint64_t consumer_cleanups{0};
  std::atomic_uint64_t pauses{0};
  std::atomic_uint64_t resumes{0};
  std::atomic_uint64_t publication_revalidations{0};
};

executor::ModelFamilyCompositionExecutionRequestV1 RuntimeRequest(
    const optimizer::ModelFamilyDependencyCoordinatorResultV1& plan,
    memory::TempWorkspaceLifecycleManager* workspace,
    RuntimeCounters* counters,
    const std::uint64_t operation_seed) {
  executor::ModelFamilyCompositionExecutionRequestV1 request;
  request.admitted_plan = plan;
  for (std::uint16_t ordinal = 0; ordinal < plan.stable_schedule.size();
       ++ordinal) {
    executor::ModelFamilyCompositionExecutionLegV1 leg;
    leg.lexical_source_ordinal = ordinal;
    leg.execution =
        LegExecution(plan, ordinal, &counters->provider_cleanups);
    leg.pause_exchange = [counters] {
      counters->pauses.fetch_add(1, std::memory_order_relaxed);
    };
    leg.resume_exchange = [counters] {
      counters->resumes.fetch_add(1, std::memory_order_relaxed);
    };
    leg.cleanup_exchange = [counters] {
      counters->exchange_cleanups.fetch_add(1, std::memory_order_relaxed);
    };
    request.legs.push_back(std::move(leg));
  }
  request.execute_relational_consumer = [](const auto& consumer,
                                           const auto& left,
                                           const auto& right) {
    executor::ModelFamilyRelationalConsumerExecutionResultV1 result;
    result.ok = true;
    result.executed_physical_node_id = consumer.physical_node_id;
    result.causal_counter_id = consumer.causal_counter_id;
    result.selected_implementation_uuid = consumer.selected_implementation_uuid;
    result.mga_statement_context = consumer.mga_statement_context;
    result.security_receipt_uuid = consumer.expected_security_receipt_uuid;
    result.output_batch.columns = left.columns;
    result.output_batch.columns.insert(result.output_batch.columns.end(),
                                       right.columns.begin(),
                                       right.columns.end());
    if (consumer.output_descriptor_ids.size() !=
        result.output_batch.columns.size()) {
      std::set<std::uint32_t> retained;
      std::erase_if(result.output_batch.columns, [&](const auto& column) {
        return !std::ranges::contains(consumer.output_descriptor_ids,
                                      column.descriptor_id) ||
               !retained.insert(column.descriptor_id).second;
      });
    }
    for (const auto& left_row : left.rows) {
      for (const auto& right_row : right.rows) {
        executor::DescriptorTuple row;
        row.values = left_row.values;
        row.values.insert(row.values.end(), right_row.values.begin(),
                          right_row.values.end());
        if (consumer.output_descriptor_ids.size() != row.values.size()) {
          decltype(row.values) projected;
          std::set<std::uint32_t> retained;
          const auto append = [&](const auto& columns, const auto& values) {
            for (std::size_t index = 0; index < columns.size(); ++index) {
              if (std::ranges::contains(consumer.output_descriptor_ids,
                                        columns[index].descriptor_id) &&
                  retained.insert(columns[index].descriptor_id).second) {
                projected.push_back(values[index]);
              }
            }
          };
          retained.clear();
          append(left.columns, left_row.values);
          append(right.columns, right_row.values);
          row.values = std::move(projected);
        }
        result.output_batch.rows.push_back(std::move(row));
      }
    }
    return result;
  };
  request.cleanup_relational_consumer = [counters](const auto) {
    counters->consumer_cleanups.fetch_add(1, std::memory_order_relaxed);
  };
  request.cancellation_requested = [] { return false; };
  request.revalidate_publication_state = [counters, plan] {
    counters->publication_revalidations.fetch_add(1,
                                                   std::memory_order_relaxed);
    return Publication(plan);
  };
  request.engine_temp_workspace = workspace;
  request.spill_operation_uuid = Uuid(operation_seed);
  request.spill_resource_contract_uuid = Uuid(804);
  request.spill_owner.temp_object_uuid = Uuid(operation_seed + 1);
  request.spill_owner.database_id = Uuid(operation_seed + 2);
  request.spill_owner.engine_id = Uuid(operation_seed + 3);
  request.spill_owner.session_id = Uuid(operation_seed + 4);
  request.spill_owner.transaction_id = Mga().owning_transaction_uuid;
  request.spill_owner.statement_id = Mga().statement_uuid;
  request.spill_owner.operation_id = request.spill_operation_uuid;
  request.spill_owner.policy_generation = 4;
  request.spill_owner.security_generation = 3;
  request.spill_owner.snapshot_boundary = Mga().statement_snapshot_uuid;
  request.spill_owner.metadata_boundary = Mga().statement_metadata_snapshot_uuid;
  request.spill_owner.resource_budget_reference =
      request.spill_resource_contract_uuid;
  request.spill_runtime_generation = 1;
  request.backpressure_high_watermark_rows = 2;
  request.backpressure_low_watermark_rows = 1;
  request.current_selected_plan_generation = plan.selected_plan_generation;
  request.current_mga_statement_context = Mga();
  return request;
}

memory::TempWorkspacePolicy WorkspacePolicy(const std::filesystem::path& root,
                                            const std::string& name) {
  memory::TempWorkspacePolicy policy;
  policy.policy_name = name;
  policy.root_path = root;
  policy.filespace_quota_bytes = 1024 * 1024;
  policy.session_quota_bytes = 1024 * 1024;
  policy.transaction_quota_bytes = 1024 * 1024;
  policy.statement_quota_bytes = 1024 * 1024;
  policy.operation_quota_bytes = 1024 * 1024;
  policy.cleanup_files_on_release = true;
  return policy;
}

bool Require(const bool condition, const std::string_view detail) {
  if (!condition) std::cerr << "QOW-OPT-007-DEPENDENCY: " << detail << '\n';
  return condition;
}

}  // namespace

#ifndef SB_RCP080_RUNTIME_FIXTURE_ONLY
int main() {
  const auto plan = optimizer::CoordinateModelFamilyDependencyDagV1(Admission());
  bool passed = Require(plan.accepted && plan.parallel_waves.size() == 1 &&
                            plan.parallel_waves[0] ==
                                std::vector<std::uint16_t>({2, 1, 0}),
                        "stable UUID schedule did not permute lexical order");
  const auto root = std::filesystem::temp_directory_path() /
                    "sb_rcp080_qow_opt_007_dependency";
  std::filesystem::remove_all(root);
  memory::TempWorkspacePolicy policy;
  policy.policy_name = "rcp080_qow_opt_007_dependency";
  policy.root_path = root;
  policy.filespace_quota_bytes = 1024 * 1024;
  policy.session_quota_bytes = 1024 * 1024;
  policy.transaction_quota_bytes = 1024 * 1024;
  policy.statement_quota_bytes = 1024 * 1024;
  policy.operation_quota_bytes = 1024 * 1024;
  policy.cleanup_files_on_release = true;
  memory::TempWorkspaceLifecycleManager workspace(policy);
  std::atomic_uint64_t provider_cleanups{0};
  std::uint64_t exchange_cleanups = 0;
  std::uint64_t consumer_cleanups = 0;
  std::uint64_t pauses = 0;
  std::uint64_t resumes = 0;
  executor::ModelFamilyCompositionExecutionRequestV1 request;
  request.admitted_plan = plan;
  for (std::uint16_t ordinal = 0; ordinal < 3; ++ordinal) {
    executor::ModelFamilyCompositionExecutionLegV1 leg;
    leg.lexical_source_ordinal = ordinal;
    leg.execution = LegExecution(plan, ordinal, &provider_cleanups);
    leg.pause_exchange = [&] { ++pauses; };
    leg.resume_exchange = [&] { ++resumes; };
    leg.cleanup_exchange = [&] { ++exchange_cleanups; };
    request.legs.push_back(std::move(leg));
  }
  request.execute_relational_consumer = [](const auto& consumer,
                                           const auto& left,
                                           const auto& right) {
    executor::ModelFamilyRelationalConsumerExecutionResultV1 result;
    result.ok = true;
    result.executed_physical_node_id = consumer.physical_node_id;
    result.causal_counter_id = consumer.causal_counter_id;
    result.selected_implementation_uuid = consumer.selected_implementation_uuid;
    result.mga_statement_context = consumer.mga_statement_context;
    result.security_receipt_uuid = consumer.expected_security_receipt_uuid;
    result.output_batch.columns = left.columns;
    result.output_batch.columns.insert(result.output_batch.columns.end(),
                                       right.columns.begin(), right.columns.end());
    for (const auto& left_row : left.rows) {
      for (const auto& right_row : right.rows) {
        executor::DescriptorTuple row;
        row.values = left_row.values;
        row.values.insert(row.values.end(), right_row.values.begin(),
                          right_row.values.end());
        result.output_batch.rows.push_back(std::move(row));
      }
    }
    return result;
  };
  request.cleanup_relational_consumer = [&](const auto) { ++consumer_cleanups; };
  request.cancellation_requested = [] { return false; };
  request.revalidate_publication_state = [plan] { return Publication(plan); };
  request.engine_temp_workspace = &workspace;
  request.spill_operation_uuid = Uuid(3000);
  request.spill_resource_contract_uuid = Uuid(804);
  request.spill_owner.temp_object_uuid = Uuid(3001);
  request.spill_owner.database_id = Uuid(3002);
  request.spill_owner.engine_id = Uuid(3003);
  request.spill_owner.session_id = Uuid(3004);
  request.spill_owner.transaction_id = Mga().owning_transaction_uuid;
  request.spill_owner.statement_id = Mga().statement_uuid;
  request.spill_owner.operation_id = request.spill_operation_uuid;
  request.spill_owner.policy_generation = 4;
  request.spill_owner.security_generation = 3;
  request.spill_owner.snapshot_boundary = Mga().statement_snapshot_uuid;
  request.spill_owner.metadata_boundary = Mga().statement_metadata_snapshot_uuid;
  request.spill_owner.resource_budget_reference =
      request.spill_resource_contract_uuid;
  request.spill_runtime_generation = 1;
  request.backpressure_high_watermark_rows = 2;
  request.backpressure_low_watermark_rows = 1;
  request.current_selected_plan_generation = 9;
  request.current_mga_statement_context = Mga();
  const auto refuses_preaccess =
      [&](executor::ModelFamilyCompositionExecutionRequestV1 mutated,
          const std::string_view diagnostic,
          const std::string_view label) {
        const auto before = workspace.Snapshot();
        const auto providers =
            provider_cleanups.load(std::memory_order_relaxed);
        const auto exchanges = exchange_cleanups;
        const auto consumers = consumer_cleanups;
        const auto refused =
            executor::ExecuteModelFamilyCompositionV1(mutated);
        const auto after = workspace.Snapshot();
        return Require(!refused.accepted && !refused.execution_started &&
                           !refused.spill_reserved &&
                           refused.diagnostic_id == diagnostic &&
                           before.allocation_count == after.allocation_count &&
                           before.active_bytes == after.active_bytes &&
                           provider_cleanups.load(std::memory_order_relaxed) ==
                               providers &&
                           exchange_cleanups == exchanges &&
                           consumer_cleanups == consumers,
                       std::string(label) +
                           " substitution was not refused before I/O: " +
                           refused.diagnostic_id + ":" + refused.detail);
      };
  auto substituted = request;
  substituted.legs[1].execution.input.operation_id = "DOCUMENT_PATH";
  passed &= refuses_preaccess(substituted, "SB_MODEL_BINDING_INCOMPLETE_V1",
                              "operation");
  substituted = request;
  substituted.legs[0].execution.input.object_uuid = Uuid(9001);
  passed &= refuses_preaccess(substituted, "SB_MODEL_BINDING_INCOMPLETE_V1",
                              "object");
  substituted = request;
  substituted.legs[0].execution.input.provider_uuid = Uuid(9002);
  substituted.legs[0].execution.capability.provider_uuid = Uuid(9002);
  passed &= refuses_preaccess(substituted, "SB_MODEL_BINDING_INCOMPLETE_V1",
                              "provider");
  substituted = request;
  substituted.legs[1].execution.input.result_handle_uuid =
      substituted.legs[0].execution.input.result_handle_uuid;
  passed &= refuses_preaccess(substituted, "SB_MODEL_BINDING_INCOMPLETE_V1",
                              "result handle");
  substituted = request;
  ++substituted.legs[0].execution.input.causal_counter_id;
  passed &= refuses_preaccess(substituted, "SB_MODEL_BINDING_INCOMPLETE_V1",
                              "causal counter");
  substituted = request;
  substituted.spill_owner.snapshot_boundary = Uuid(9003);
  passed &= refuses_preaccess(substituted,
                              "SB_MODEL_RESOURCE_SPILL_REFUSED_V1",
                              "spill snapshot");
  substituted = request;
  ++substituted.spill_owner.policy_generation;
  passed &= refuses_preaccess(substituted,
                              "SB_MODEL_RESOURCE_SPILL_REFUSED_V1",
                              "spill policy generation");
  const auto below_plan =
      optimizer::CoordinateModelFamilyDependencyDagV1(Admission(348));
  auto below_request = request;
  below_request.admitted_plan = below_plan;
  below_request.spill_operation_uuid = Uuid(3010);
  below_request.spill_owner.operation_id = below_request.spill_operation_uuid;
  for (auto& leg : below_request.legs) {
    leg.execution.input.multimodel_composition_receipt_uuid =
        below_plan.composition_admission_receipt_uuid;
  }
  const auto before_below = workspace.Snapshot();
  const auto below =
      executor::ExecuteModelFamilyCompositionV1(below_request);
  const auto after_below = workspace.Snapshot();
  passed &= Require(below_plan.accepted && !below.accepted &&
                        !below.execution_started && !below.spill_reserved &&
                        below.diagnostic_id == "SB_MODEL_RESOURCE_SPILL_REFUSED_V1" &&
                        before_below.allocation_count == after_below.allocation_count &&
                        before_below.active_bytes == after_below.active_bytes &&
                        provider_cleanups.load(std::memory_order_relaxed) == 0 &&
                        exchange_cleanups == 0 && consumer_cleanups == 0,
                    "below-bound spill refusal performed provider or temp I/O");
  const auto executed = executor::ExecuteModelFamilyCompositionV1(request);
  passed &= Require(executed.accepted && executed.root_published &&
                        executed.no_partial_root &&
                        executed.root_output_batch.rows.size() == 27 &&
                        executed.rows_received == 9 &&
                        executed.rows_published == 27 &&
                        executed.backpressure_complete &&
                        executed.pause_count == 6 && executed.resume_count == 6 &&
                        executed.spill_reserved && executed.spill_io_complete &&
                        executed.spill_reserved_bytes == 349 &&
                        executed.cleanup_complete &&
                        executed.total_cleanup_count == 10 &&
                        provider_cleanups.load(std::memory_order_relaxed) == 3 &&
                        exchange_cleanups == 3 &&
                        consumer_cleanups == 2 && pauses == 6 && resumes == 6 &&
                        workspace.Snapshot().active_bytes == 0,
                    "complete dependency/runtime/spill lifecycle drifted: " +
                        executed.diagnostic_id + ":" + executed.detail +
                        ";providers=" + std::to_string(executed.provider_cleanup_count) +
                        ";exchanges=" + std::to_string(executed.exchange_cleanup_count) +
                        ";consumers=" + std::to_string(executed.relational_consumer_cleanup_count) +
                        ";spill=" + std::to_string(executed.spill_cleanup_count) +
                        ";started=" + std::to_string(executed.started_leg_ordinals.size()) +
                        ";launched=" + std::to_string(executed.launched_leg_ordinals.size()) +
                        ";accepted=" + std::to_string(executed.accepted) +
                        ";root=" + std::to_string(executed.root_published) +
                        ";root_rows=" + std::to_string(executed.root_output_batch.rows.size()) +
                        ";received=" + std::to_string(executed.rows_received) +
                        ";published=" + std::to_string(executed.rows_published) +
                        ";backpressure=" + std::to_string(executed.backpressure_complete) +
                        ";pause=" + std::to_string(executed.pause_count) +
                        ";resume=" + std::to_string(executed.resume_count) +
                        ";spill_reserved=" + std::to_string(executed.spill_reserved) +
                        ";spill_io=" + std::to_string(executed.spill_io_complete) +
                        ";spill_bytes=" + std::to_string(executed.spill_reserved_bytes) +
                        ";cleanup=" + std::to_string(executed.cleanup_complete) +
                        ";total_cleanup=" + std::to_string(executed.total_cleanup_count) +
                        ";provider_callback=" + std::to_string(provider_cleanups.load()) +
                        ";exchange_callback=" + std::to_string(exchange_cleanups) +
                        ";consumer_callback=" + std::to_string(consumer_cleanups) +
                        ";pause_callback=" + std::to_string(pauses) +
                        ";resume_callback=" + std::to_string(resumes) +
                        ";workspace=" + std::to_string(workspace.Snapshot().active_bytes));
  passed &= Require(std::ranges::all_of(executed.rule_receipts,
                                        [](const auto& receipt) {
                                          return receipt.complete;
                                        }),
                    "not all 24 coordinator receipts completed at runtime");
  const auto above_plan =
      optimizer::CoordinateModelFamilyDependencyDagV1(Admission(350));
  auto above_request = request;
  above_request.admitted_plan = above_plan;
  above_request.spill_operation_uuid = Uuid(3020);
  above_request.spill_owner.operation_id = above_request.spill_operation_uuid;
  for (auto& leg : above_request.legs) {
    leg.execution.input.multimodel_composition_receipt_uuid =
        above_plan.composition_admission_receipt_uuid;
  }
  const auto above =
      executor::ExecuteModelFamilyCompositionV1(above_request);
  passed &= Require(above_plan.accepted && above.accepted &&
                        above.root_published && above.spill_reserved &&
                        above.spill_reserved_bytes == 350 &&
                        above.spill_io_complete && above.cleanup_complete &&
                        provider_cleanups.load(std::memory_order_relaxed) == 6 &&
                        exchange_cleanups == 6 && consumer_cleanups == 4 &&
                        workspace.Snapshot().active_bytes == 0,
                    "above-bound spill execution or cleanup drifted");
  auto descriptor_substitution = request;
  descriptor_substitution.spill_operation_uuid = Uuid(3030);
  descriptor_substitution.spill_owner.operation_id =
      descriptor_substitution.spill_operation_uuid;
  const auto original_document_provider =
      descriptor_substitution.legs[1].execution.execute_provider;
  descriptor_substitution.legs[1].execution.execute_provider =
      [original_document_provider](const auto& input) {
        auto output = original_document_provider(input);
        output.provider_batch.batch.columns[0]
            .descriptor.descriptor_uuid.canonical = Uuid(9990);
        for (auto& row : output.provider_batch.batch.rows) {
          row.values[0].descriptor.descriptor_uuid.canonical = Uuid(9990);
        }
        return output;
      };
  std::atomic_uint64_t publication_revalidations{0};
  descriptor_substitution.revalidate_publication_state =
      [&publication_revalidations, plan] {
        publication_revalidations.fetch_add(1, std::memory_order_relaxed);
        return Publication(plan);
      };
  const auto descriptor_refusal =
      executor::ExecuteModelFamilyCompositionV1(descriptor_substitution);
  passed &= Require(!descriptor_refusal.accepted &&
                        !descriptor_refusal.root_published &&
                        descriptor_refusal.no_partial_root &&
                        descriptor_refusal.root_output_batch.rows.empty() &&
                        descriptor_refusal.diagnostic_id ==
                            executor::kModelTypedExchangeInvalid &&
                        descriptor_refusal.provider_cleanup_count == 3 &&
                        descriptor_refusal.relational_consumer_cleanup_count == 0 &&
                        publication_revalidations.load(
                            std::memory_order_relaxed) == 0 &&
                        workspace.Snapshot().active_bytes == 0,
                    "same-ID/different-UUID lineage reached a consumer or root");
  if (!passed) return 1;
  std::cout << "QOW-OPT-007-DEPENDENCY: passed;families=relational,document,graph;legs=3;rows=27;spill_boundary=348/refuse,349/equal,350/above;cleanup=10\n";
  return 0;
}
#endif
