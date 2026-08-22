// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#define SB_RCP080_RUNTIME_FIXTURE_ONLY 1
#include "qow_opt_007_dependency.cpp"

#include "canonical_aggregate_registry.hpp"

#if defined(SB_RCP080_PRODUCTION_QUERY_ROUTE)
#include "ast/ast.hpp"
#include "binder/binder.hpp"
#include "canonical_query_execute.hpp"
#include "query/canonical_relational_bridge.hpp"
#include "crud_support/crud_store.hpp"
#include "cst/cst.hpp"
#include "database_lifecycle.hpp"
#include "datatype_catalog_manifest.hpp"
#include "lowering/lowering.hpp"
#include "memory.hpp"
#include "mga_relation_store/mga_relation_store.hpp"
#include "nosql/document_api.hpp"
#include "nosql/graph_api.hpp"
#include "nosql/key_value_api.hpp"
#include "nosql/spatial_api.hpp"
#include "scratchbird/engine/engine.h"
#include "server_engine_bridge/statement_context.hpp"
#include "sblr_dispatch.hpp"
#include "sblr_opcode_registry.hpp"
#include "transaction/transaction_api.hpp"
#include "uuid.hpp"

#include <chrono>
#include <cstring>
#endif

namespace {

#if defined(SB_RCP080_PRODUCTION_QUERY_ROUTE)
namespace api = scratchbird::engine::internal_api;
namespace bridge = scratchbird::server_engine_bridge;
namespace db = scratchbird::storage::database;
namespace dt = scratchbird::core::datatypes;
namespace nosql = scratchbird::engine::internal_api::nosql;
namespace platform = scratchbird::core::platform;
namespace planner = scratchbird::engine::planner;
namespace sblr = scratchbird::engine::sblr;
namespace sbsql = scratchbird::parser::sbsql;
namespace uuid = scratchbird::core::uuid;
#endif

const std::array<std::string, 9> kFamilies = {
    "relational", "document", "graph", "key_value", "time_series",
    "vector", "search", "spatial", "columnar"};
const std::array<std::string, 9> kOperations = {
    "RELATIONAL_HEAP_SCAN", "DOCUMENT_FIND", "GRAPH_MATCH", "KEY_VALUE_GET",
    "TIME_SERIES_BUCKET", "VECTOR_EXACT_SEARCH", "SEARCH_RANKED_QUERY",
    "SPATIAL_SOURCE", "COLUMNAR_SOURCE"};
const std::array<std::size_t, 9> kWidths = {1, 1, 1, 3, 1, 3, 5, 3, 1};
constexpr std::string_view kCanonicalInt64TypeUuid =
    "019d0000-0000-7000-8000-00000000d711";

optimizer::ModelFamilyDependencyCoordinatorRequestV1 FullNineAdmission() {
  optimizer::ModelFamilyDependencyCoordinatorRequestV1 request;
  request.composition_profile_id = "COMP-9-FULL-UNIVERSE-V1";
  request.bound_sblr_tree_uuid = Uuid(6000);
  request.selected_plan_generation = request.current_selected_plan_generation = 9;
  request.canonical_root_physical_node_uuid = Uuid(6299);
  request.canonical_root_physical_node_id = 208;
  std::uint32_t descriptor_id = 100;
  for (std::uint16_t ordinal = 0; ordinal < 9; ++ordinal) {
    optimizer::ModelFamilyDependencyLegV1 leg;
    leg.lexical_source_ordinal = ordinal;
    leg.physical_node_uuid = Uuid(6100 + ordinal);
    leg.family_id = kFamilies[ordinal];
    leg.operation_id = kOperations[ordinal];
    if (ordinal >= 7) leg.operation_ids = {kOperations[ordinal]};
    leg.selected_plan_uuid = Uuid(6200 + ordinal);
    leg.selected_alternative_uuid = Uuid(6300 + ordinal);
    leg.provider_uuid = Uuid(6400 + ordinal);
    leg.capability_uuid = Uuid(6500 + ordinal);
    leg.delivered_property_uuid = Uuid(6600 + ordinal);
    leg.bound_object_uuid = Uuid(6700 + ordinal);
    leg.catalog_snapshot_uuid = leg.current_catalog_snapshot_uuid = Uuid(6800);
    leg.descriptor_snapshot_uuid = leg.current_descriptor_snapshot_uuid = Uuid(6801);
    leg.security_context_uuid = leg.current_security_context_uuid = Uuid(6802);
    leg.policy_snapshot_uuid = leg.current_policy_snapshot_uuid = Uuid(6803);
    leg.resource_contract_uuid = leg.current_resource_contract_uuid = Uuid(6804);
    leg.operation_scope_receipt_uuid = Uuid(6900 + ordinal);
    leg.selected_alternative_receipt_uuid = Uuid(7000 + ordinal);
    leg.root_physical_node_id = ordinal + 1;
    for (std::size_t column = 0; column < kWidths[ordinal]; ++column) {
      leg.output_descriptor_ids.push_back(descriptor_id++);
      leg.output_descriptor_uuids.push_back(Uuid(7100 + descriptor_id));
    }
    leg.family_local_cost = Cost(7300 + ordinal);
    optimizer::ModelFamilyDependencyAlternativeV1 candidate;
    candidate.alternative_uuid = leg.selected_alternative_uuid;
    candidate.candidate_inventory_receipt_uuid = Uuid(7400 + ordinal);
    candidate.implementation_id = kFamilies[ordinal] + "_exact_v1";
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
    leg.memory_grant_bytes = 128 * 1024;
    leg.exchange_buffer_bytes = 1024;
    leg.maximum_rows = 1;
    leg.maximum_columns = kWidths[ordinal];
    leg.maximum_cells = kWidths[ordinal];
    leg.selected = leg.security_admitted = leg.capability_admitted = true;
    leg.exact = leg.exact_fallback_available = true;
    leg.cleanup_supported = leg.cancellation_supported = true;
    leg.spill_eligible = true;
    request.legs.push_back(std::move(leg));
  }
  for (std::uint16_t ordinal = 0; ordinal < 8; ++ordinal) {
    optimizer::ModelFamilyDependencyEdgeV1 edge;
    edge.edge_uuid = Uuid(7600 + ordinal);
    edge.producer_lexical_source_ordinal = ordinal;
    edge.consumer_lexical_source_ordinal = ordinal + 1;
    edge.required_property_uuid = request.legs[ordinal].delivered_property_uuid;
    edge.delivered_property_uuid = edge.required_property_uuid;
    edge.descriptor_lineage_uuid = Uuid(7700 + ordinal);
    edge.producer_output_descriptor_ids = request.legs[ordinal].output_descriptor_ids;
    edge.consumer_input_descriptor_ids = edge.producer_output_descriptor_ids;
    edge.producer_output_descriptor_uuids = request.legs[ordinal].output_descriptor_uuids;
    edge.consumer_input_descriptor_uuids = edge.producer_output_descriptor_uuids;
    request.edges.push_back(std::move(edge));
  }
  std::vector<std::uint32_t> accumulated_ids = request.legs[0].output_descriptor_ids;
  std::vector<std::string> accumulated_uuids = request.legs[0].output_descriptor_uuids;
  std::string left_uuid = request.legs[0].physical_node_uuid;
  for (std::uint16_t ordinal = 1; ordinal < 9; ++ordinal) {
    accumulated_ids.insert(accumulated_ids.end(),
                           request.legs[ordinal].output_descriptor_ids.begin(),
                           request.legs[ordinal].output_descriptor_ids.end());
    accumulated_uuids.insert(accumulated_uuids.end(),
                             request.legs[ordinal].output_descriptor_uuids.begin(),
                             request.legs[ordinal].output_descriptor_uuids.end());
    optimizer::ModelFamilyRelationalConsumerV1 consumer;
    consumer.physical_node_uuid = Uuid(6291 + ordinal);
    consumer.physical_node_id = 200 + ordinal;
    consumer.causal_counter_id = 100 + ordinal;
    consumer.selected_implementation_uuid = Uuid(7800 + ordinal);
    consumer.expected_security_receipt_uuid = Uuid(7900 + ordinal);
    consumer.join_form_id = "CROSS";
    consumer.input_physical_node_uuids = {
        left_uuid, request.legs[ordinal].physical_node_uuid};
    consumer.input_descriptor_ids = accumulated_ids;
    consumer.output_descriptor_ids = accumulated_ids;
    consumer.input_descriptor_uuids = accumulated_uuids;
    consumer.output_descriptor_uuids = accumulated_uuids;
    consumer.mga_statement_context = Mga();
    consumer.maximum_rows = 1;
    consumer.maximum_columns = accumulated_ids.size();
    consumer.maximum_cells = accumulated_ids.size();
    consumer.memory_grant_bytes = 1024 * 1024;
    consumer.canonical_root = ordinal == 8;
    consumer.exact = consumer.cleanup_supported =
        consumer.cancellation_supported = true;
    left_uuid = consumer.physical_node_uuid;
    request.relational_consumers.push_back(std::move(consumer));
  }
  request.statement_memory_budget_bytes = 16 * 1024 * 1024;
  request.backpressure_high_watermark_rows = 2;
  request.backpressure_low_watermark_rows = 1;
  request.signed_short_circuit_enabled = true;
  request.feedback_observation_frozen = true;
  request.feedback_target_is_later_plan = true;
  request.feedback_observation_generation = 9;
  request.feedback_target_plan_generation = 10;
  return request;
}

executor::DescriptorBatch FamilyBatch(
    const optimizer::ModelFamilyDependencyLegV1& leg) {
  std::vector<std::pair<std::string, std::string>> schema;
  if (leg.family_id == "key_value") {
    schema = {{"row_uuid", "uuid"}, {"key", "text"}, {"value", "text"}};
  } else if (leg.family_id == "time_series") {
    schema = {{"bucket_start", "timestamp_tz"}};
  } else if (leg.family_id == "vector") {
    schema = {{"row_uuid", "uuid"}, {"distance", "real64"}, {"score", "real64"}};
  } else if (leg.family_id == "search") {
    schema = {{"document_uuid", "uuid"}, {"analyzer_uuid", "uuid"},
              {"analyzer_generation", "uint64"}, {"score", "real64"},
              {"rank", "uint64"}};
  } else if (leg.family_id == "spatial") {
    schema = {{"row_uuid", "uuid"}, {"spatial_value", "geometry"},
              {"crs_uuid", "uuid"}};
  } else {
    for (std::size_t column = 0; column < leg.output_descriptor_ids.size(); ++column)
      schema.emplace_back("c" + std::to_string(column), "uuid");
  }
  executor::DescriptorBatch batch;
  for (std::size_t column = 0; column < schema.size(); ++column) {
    auto descriptor = executor::MakeExecutorDescriptor(
        schema[column].second,
        "canonical=" + schema[column].second + ";type_uuid=" +
            Uuid(8000 + column) + ";nullable=false");
    descriptor.descriptor_uuid.canonical = leg.output_descriptor_uuids[column];
    descriptor.descriptor_kind = "scalar";
    batch.columns.push_back({schema[column].first, descriptor, false,
                             leg.output_descriptor_ids[column]});
  }
  std::vector<std::string> values;
  if (leg.family_id == "key_value") {
    values = {Uuid(9000), "alpha", "value"};
  } else if (leg.family_id == "time_series") {
    values = {"2026-08-12T20:00:00.000000000Z"};
  } else if (leg.family_id == "vector") {
    values = {Uuid(9001), "1", "0.5"};
  } else if (leg.family_id == "search") {
    values = {Uuid(9002), Uuid(9003), "1", "1", "1"};
  } else if (leg.family_id == "spatial") {
    values = {Uuid(9004), {}, Uuid(8202)};
  } else {
    for (std::size_t column = 0; column < schema.size(); ++column)
      values.push_back(Uuid(9100 + leg.lexical_source_ordinal * 10 + column));
  }
  executor::DescriptorTuple row;
  for (std::size_t column = 0; column < values.size(); ++column) {
    row.values.push_back(executor::MakeExecutorValue(
        batch.columns[column].descriptor, values[column]));
  }
  if (leg.family_id == "spatial") {
    row.values[1].binary_value = {1};
  }
  batch.rows.push_back(std::move(row));
  return batch;
}

executor::ModelFamilyExecutionRequestV1 FullNineExecution(
    const optimizer::ModelFamilyDependencyCoordinatorResultV1& plan,
    const std::uint16_t ordinal,
    std::atomic_uint64_t* cleanup_count) {
  const auto scheduled = std::ranges::find_if(
      plan.stable_schedule, [&](const auto& value) {
        return value.leg.lexical_source_ordinal == ordinal;
      });
  const auto& leg = scheduled->leg;
  executor::ModelFamilyExecutionRequestV1 request;
  request.input.family_id = leg.family_id;
  request.input.operation_ids = leg.operation_ids;
  request.input.operation_id = leg.operation_id;
  request.input.object_uuid = leg.bound_object_uuid;
  request.input.physical_node_id = leg.root_physical_node_id;
  request.input.selected_alternative_uuid = leg.selected_alternative_uuid;
  request.input.capability_uuid = leg.capability_uuid;
  request.input.provider_uuid = leg.provider_uuid;
  request.input.provider_generation = leg.provider_generation;
  request.input.result_handle_uuid = Uuid(8100 + ordinal);
  request.input.causal_counter_id = scheduled->causal_counter_id;
  request.input.output_descriptor_ids = leg.output_descriptor_ids;
  request.input.mga_statement_context = Mga();
  request.input.catalog_epoch_uuid = leg.catalog_snapshot_uuid;
  request.input.security_context_uuid = leg.security_context_uuid;
  request.input.policy_snapshot_uuid = leg.policy_snapshot_uuid;
  request.input.resource_contract_uuid = leg.resource_contract_uuid;
  request.input.catalog_generation = leg.catalog_generation;
  request.input.descriptor_generation = leg.descriptor_generation;
  request.input.security_generation = leg.security_generation;
  request.input.policy_generation = leg.policy_generation;
  request.input.resource_generation = leg.resource_generation;
  request.input.maximum_rows = leg.maximum_rows;
  request.input.maximum_cells = leg.maximum_cells;
  request.input.maximum_memory_bytes = leg.memory_grant_bytes;
  request.input.multimodel_composition_receipt_uuid =
      plan.composition_admission_receipt_uuid;
  request.input.multimodel_lexical_source_ordinal = ordinal;
  request.input.multimodel_composition_arity = 9;
  request.input.multimodel_common_statement_context = true;
  if (leg.family_id == "spatial") {
    request.input.spatial_geometry_descriptor_uuid =
        leg.output_descriptor_uuids[1];
    request.input.spatial_geometry_type_uuid = Uuid(8001);
    request.input.spatial_crs_uuid = Uuid(8202);
    request.input.spatial_crs_generation = 1;
  }
  request.capability.capability_uuid = leg.capability_uuid;
  request.capability.family_id = leg.family_id;
  request.capability.provider_uuid = leg.provider_uuid;
  request.capability.provider_generation = leg.provider_generation;
  request.capability.capability_generation = leg.capability_generation;
  request.capability.available = request.capability.exact = true;
  request.capability.exact_collection_fallback_available = true;
  request.capability.cancellation_supported = true;
  request.capability.cleanup_supported = true;
  request.capability.residual_recheck_supported = true;
  request.capability.base_row_mga_recheck_supported = true;
  request.capability.security_recheck_supported = true;
  request.current_catalog_generation = leg.current_catalog_generation;
  request.current_descriptor_generation = leg.current_descriptor_generation;
  request.current_security_generation = leg.current_security_generation;
  request.current_policy_generation = leg.current_policy_generation;
  request.current_resource_generation = leg.current_resource_generation;
  request.current_provider_generation = leg.current_provider_generation;
  request.current_capability_generation = leg.current_capability_generation;
  request.current_mga_statement_context = Mga();
  request.cancellation_requested = [] { return false; };
  request.cleanup_provider = [cleanup_count] { cleanup_count->fetch_add(1); };
  const auto batch = FamilyBatch(leg);
  request.execute_provider = [batch, property_uuid = leg.delivered_property_uuid](
                                 const auto& input) {
    executor::ModelProviderExecutionResultV1 result;
    result.ok = result.data_access_observed = true;
    auto& output = result.provider_batch;
    output.provider_uuid = input.provider_uuid;
    output.provider_generation = input.provider_generation;
    output.selected_alternative_uuid = input.selected_alternative_uuid;
    output.capability_uuid = input.capability_uuid;
    output.result_handle_uuid = input.result_handle_uuid;
    output.causal_counter_id = input.causal_counter_id;
    output.output_descriptor_ids = input.output_descriptor_ids;
    output.batch = batch;
    executor::ModelProviderRowIdentityV1 identity;
    if (input.family_id == "relational") {
      identity.row_uuid = batch.rows[0].values[0].encoded_value;
    } else if (input.family_id == "document") {
      identity.row_uuid = Uuid(9200);
      identity.document_uuid = batch.rows[0].values[0].encoded_value;
    } else if (input.family_id == "graph") {
      identity.row_uuid = batch.rows[0].values[0].encoded_value;
      identity.vertex_uuid = Uuid(9201);
      identity.path_uuid = Uuid(9202);
    } else if (input.family_id == "key_value") {
      identity.row_uuid = batch.rows[0].values[0].encoded_value;
      identity.key = batch.rows[0].values[1].encoded_value;
    } else if (input.family_id == "time_series") {
      identity.row_uuid = Uuid(9203);
      identity.series_uuid = input.object_uuid;
      identity.metric_uuid = Uuid(9204);
      identity.tags = "{}";
      executor::ParseCanonicalTimeSeriesTimestampNsV1(
          batch.rows[0].values[0].encoded_value,
          &identity.bucket_start_ns);
    } else if (input.family_id == "vector") {
      identity.row_uuid = batch.rows[0].values[0].encoded_value;
      identity.vector_distance = batch.rows[0].values[1].encoded_value;
      identity.vector_score = batch.rows[0].values[2].encoded_value;
    } else if (input.family_id == "search") {
      identity.document_uuid = batch.rows[0].values[0].encoded_value;
      identity.search_analyzer_uuid = batch.rows[0].values[1].encoded_value;
      identity.search_analyzer_generation = 1;
      identity.search_score = batch.rows[0].values[3].encoded_value;
      identity.search_rank = 1;
    } else {
      identity.row_uuid = batch.rows[0].values[0].encoded_value;
    }
    output.ordered_row_identities.push_back(std::move(identity));
    output.properties.property_uuid = property_uuid;
    output.properties.ordering_id =
        input.family_id == "key_value" ? "key_value_unordered_v1"
        : input.family_id == "time_series" ? "series_metric_timestamp_tags_row_ascending_v1"
        : input.family_id == "vector" ? "vector_distance_row_uuid_ascending_v1"
        : input.family_id == "search" ? "search_score_desc_document_uuid_asc_v1"
                                      : "fixture_order";
    output.properties.uniqueness_id =
        input.family_id == "graph" ? "path_uuid"
        : input.family_id == "key_value" ? "key"
        : input.family_id == "search" ? "document_uuid"
        : input.family_id == "document" ? "document_uuid" : "row_uuid";
    output.properties.residual_recheck_complete = true;
    output.properties.base_row_mga_recheck_complete = true;
    output.properties.security_recheck_complete = true;
    output.mga_statement_context = input.mga_statement_context;
    output.security_receipt_uuid = Uuid(8300 + input.physical_node_id);
    output.multimodel_composition_receipt_uuid = input.multimodel_composition_receipt_uuid;
    output.multimodel_lexical_source_ordinal = input.multimodel_lexical_source_ordinal;
    output.multimodel_composition_arity = input.multimodel_composition_arity;
    output.multimodel_common_statement_context = true;
    output.residual_recheck_complete = true;
    output.base_row_mga_recheck_complete = true;
    output.security_recheck_complete = true;
    return result;
  };
  return request;
}

executor::CanonicalExecutionMgaAuthority Rcp080ContinuationAuthorityV1(
    const executor::TypedPhysicalNodeDag& dag) {
  executor::CanonicalExecutionMgaAuthority authority;
  authority.statement_context = dag.mga_statement_context;
  authority.origin =
      executor::CanonicalMgaAuthorityOrigin::kEngineTransactionInventory;
  const auto current = authority.statement_context;
  authority.resolve_current = [current] {
    executor::CanonicalMgaCurrentResolution resolution;
    resolution.statement_context = current;
    return resolution;
  };
  return authority;
}

executor::TypedPhysicalNodeDag Rcp080ContinuationDagV1(
    const executor::PhysicalNodeKind root_kind,
    const std::vector<std::uint32_t>& input_descriptors,
    const executor::PhysicalMgaStatementContext& statement_context,
    const std::vector<std::uint32_t>& second_input_descriptors = {}) {
  executor::TypedPhysicalNodeDag dag;
  dag.abi_version = 2;
  dag.selected_plan_uuid =
      Uuid(10'000 + static_cast<std::uint64_t>(root_kind));
  dag.root_physical_node_id = second_input_descriptors.empty() ? 2 : 3;
  dag.local_transaction_id = statement_context.owning_local_transaction_id;
  dag.statement_snapshot_id =
      statement_context.visible_committed_high_watermark;
  dag.mga_statement_context = statement_context;
  dag.bound_sblr_tree_uuid = Uuid(10'100);
  dag.catalog_epoch_uuid = Uuid(10'101);
  dag.security_context_uuid = Uuid(10'102);
  dag.capability_snapshot_uuid = Uuid(10'103);
  dag.resource_snapshot_uuid = Uuid(10'104);
  dag.statistics_snapshot_uuid = Uuid(10'105);
  dag.route_snapshot_uuid = Uuid(10'106);
  dag.admission_evidence = {
      {executor::PhysicalAdmissionStage::kBoundRequest,
       dag.bound_sblr_tree_uuid},
      {executor::PhysicalAdmissionStage::kCatalogEpoch,
       dag.catalog_epoch_uuid},
      {executor::PhysicalAdmissionStage::kSecurity,
       dag.security_context_uuid},
      {executor::PhysicalAdmissionStage::kMgaStatementBoundary,
       dag.mga_statement_context.statement_snapshot_uuid},
      {executor::PhysicalAdmissionStage::kPolicyCapability,
       dag.capability_snapshot_uuid},
      {executor::PhysicalAdmissionStage::kResource,
       dag.resource_snapshot_uuid},
      {executor::PhysicalAdmissionStage::kStatisticsProvenance,
       dag.statistics_snapshot_uuid},
      {executor::PhysicalAdmissionStage::kCanonicalRoute,
       dag.route_snapshot_uuid},
  };
  executor::PhysicalNodeRecord first;
  first.physical_node_id = 1;
  first.relational_node_id = 1;
  first.node_kind = executor::PhysicalNodeKind::kValues;
  first.implementation_id = "values.multimodel-root.typed.v1";
  first.output_descriptor_ids = input_descriptors;
  first.causal_counter_id = 1;
  dag.nodes.push_back(std::move(first));
  auto output_descriptors = input_descriptors;
  if (!second_input_descriptors.empty()) {
    executor::PhysicalNodeRecord second;
    second.physical_node_id = 2;
    second.relational_node_id = 2;
    second.node_kind = executor::PhysicalNodeKind::kValues;
    second.implementation_id = "values.multimodel-root.typed.v1";
    second.output_descriptor_ids = second_input_descriptors;
    second.causal_counter_id = 2;
    dag.nodes.push_back(std::move(second));
    output_descriptors.insert(output_descriptors.end(),
                              second_input_descriptors.begin(),
                              second_input_descriptors.end());
  }
  executor::PhysicalNodeRecord root;
  root.physical_node_id = dag.root_physical_node_id;
  root.relational_node_id =
      static_cast<std::uint32_t>(dag.root_physical_node_id);
  root.node_kind = root_kind;
  root.implementation_id =
      root_kind == executor::PhysicalNodeKind::kProject
          ? "project.descriptor-direct.v1"
      : root_kind == executor::PhysicalNodeKind::kSubquery
          ? "subquery.table.materialize.typed.v1"
      : root_kind == executor::PhysicalNodeKind::kAggregate
          ? "aggregate.registry-core.v1"
      : root_kind == executor::PhysicalNodeKind::kSort
          ? "sort.typed.terms.v1"
      : root_kind == executor::PhysicalNodeKind::kWindow
          ? "window.partition-order-peer.v1"
      : root_kind == executor::PhysicalNodeKind::kLimit
          ? "limit.typed.v1"
      : root_kind == executor::PhysicalNodeKind::kRecursiveCte
          ? "cte.recursive.working.typed.v1"
          : "canonical.multimodel-consumer.typed.v1";
  root.input_physical_node_ids =
      second_input_descriptors.empty()
          ? std::vector<std::uint64_t>{1}
          : std::vector<std::uint64_t>{1, 2};
  root.output_descriptor_ids = std::move(output_descriptors);
  root.causal_counter_id = dag.root_physical_node_id;
  dag.nodes.push_back(std::move(root));
  dag.catalog_generation = 1;
  dag.security_epoch = 1;
  dag.policy_epoch = 1;
  dag.resource_epoch = 1;
  dag.statistics_generation = 1;
  dag.route_epoch = 1;
  dag.route_generation = 1;
  dag.memory_budget_bytes = 1024 * 1024;
  dag.optimizer_published = true;
  dag.immutable_node_identity_validated = true;
  dag.capability_validated_before_access = true;
  dag.publication_contract_version = 1;
  dag.published_node_count = dag.nodes.size();
  dag.first_causal_counter_id = 1;
  dag.complete_cost_vectors_retained = true;
  dag.descriptor_contract_validated = true;
  dag.property_contract_validated = true;
  dag.dependency_contract_validated = true;
  dag.resource_contract_validated = true;
  dag.mga_contract_validated = true;
  dag.causal_identity_validated = true;
  std::uint64_t retained_selected_scalar_score = 0;
  for (std::size_t ordinal = 0; ordinal < dag.nodes.size(); ++ordinal) {
    auto& node = dag.nodes[ordinal];
    node.selected_alternative_uuid = Uuid(10'200 + node.physical_node_id);
    node.executor_capability_uuid = Uuid(10'210 + node.physical_node_id);
    node.executor_capability_abi_version = 1;
    node.cost_vector_uuid = Uuid(10'220 + node.physical_node_id);
    node.memory_bytes_required = 4096;
    node.engine_capability_validated = true;
    node.mga_statement_context = dag.mga_statement_context;
    node.logical_semantic_variant_id =
        "multimodel.relational-continuation.v1";
    node.publication_ordinal = ordinal;
    node.transformation_uuid = Uuid(10'230 + node.physical_node_id);
    node.transformation_rule_id =
        "multimodel.relational-continuation.rule.v1";
    node.retained_cost.cost_vector_uuid = node.cost_vector_uuid;
    node.retained_cost.calibration_profile_uuid =
        Uuid(10'240 + node.physical_node_id);
    node.retained_cost.cpu_units = 1;
    node.retained_cost.memory_bytes_required = node.memory_bytes_required;
    node.retained_cost.scalar_score =
        node.retained_cost.cpu_units + node.retained_cost.memory_bytes_required;
    retained_selected_scalar_score += node.retained_cost.scalar_score;
    dag.selected_plan_signature +=
        std::to_string(node.relational_node_id) + "=" +
        node.selected_alternative_uuid + ";";
  }
  dag.selected_scalar_score = retained_selected_scalar_score;
  return dag;
}

bool ExecuteRcp080RelationalContinuationV1(
    const executor::DescriptorBatch& multimodel_root,
    const executor::PhysicalMgaStatementContext& statement_context) {
  const auto fail = [](const std::string_view stage) {
    std::cerr << "QOW-CES05-MULTIMODEL-CONTINUATION: failed;stage="
              << stage << '\n';
    return false;
  };
  const auto root_validation =
      executor::ValidateDescriptorBatch(multimodel_root);
  if (!root_validation.ok || multimodel_root.columns.size() != 19 ||
      multimodel_root.rows.size() != 1) {
    return fail("coordinator-root");
  }
  std::vector<std::uint32_t> descriptor_ids;
  std::vector<std::size_t> projected_columns;
  for (std::size_t ordinal = 0; ordinal < multimodel_root.columns.size();
       ++ordinal) {
    descriptor_ids.push_back(multimodel_root.columns[ordinal].descriptor_id);
    projected_columns.push_back(ordinal);
  }

  auto select_dag = Rcp080ContinuationDagV1(
      executor::PhysicalNodeKind::kProject, descriptor_ids,
      statement_context);
  executor::CanonicalDescriptorProjectionRequest select_request;
  select_request.physical_dag = select_dag;
  select_request.selected_physical_node_id = select_dag.root_physical_node_id;
  select_request.input_batch = multimodel_root;
  select_request.projected_columns = projected_columns;
  select_request.mga_authority = Rcp080ContinuationAuthorityV1(select_dag);
  const auto selected =
      executor::ExecuteCanonicalDescriptorProjection(select_request);
  if (!selected.diagnostic.ok ||
      selected.output_batch.columns.size() != descriptor_ids.size() ||
      selected.output_batch.rows.size() != multimodel_root.rows.size()) {
    return fail("select");
  }

  auto subquery_dag = Rcp080ContinuationDagV1(
      executor::PhysicalNodeKind::kSubquery, descriptor_ids,
      statement_context);
  executor::CanonicalTableSubqueryRequest subquery_request;
  subquery_request.physical_dag = subquery_dag;
  subquery_request.selected_physical_node_id =
      subquery_dag.root_physical_node_id;
  subquery_request.input_batch = selected.output_batch;
  subquery_request.maximum_materialized_row_count = 16;
  subquery_request.mga_authority =
      Rcp080ContinuationAuthorityV1(subquery_dag);
  const auto subquery =
      executor::ExecuteCanonicalTableSubquery(subquery_request);
  if (!subquery.diagnostic.ok || subquery.materialized_row_count != 1 ||
      subquery.output_batch.columns.size() != descriptor_ids.size()) {
    return fail("subquery");
  }

  auto cte_dag = Rcp080ContinuationDagV1(
      executor::PhysicalNodeKind::kCte, descriptor_ids, statement_context);
  executor::CanonicalPhysicalExecutorRegistration values_registration;
  values_registration.node_kind = executor::PhysicalNodeKind::kValues;
  values_registration.implementation_id =
      "values.multimodel-root.typed.v1";
  values_registration.executor_capability_uuid =
      cte_dag.nodes.front().executor_capability_uuid;
  values_registration.executor_capability_abi_version = 1;
  values_registration.engine_owned = true;
  values_registration.accepts_optimizer_publication_v2 = true;
  const auto subquery_batch = subquery.output_batch;
  values_registration.execute =
      [subquery_batch](const auto& dag, const auto& node,
                       const auto& inputs) {
        executor::CanonicalPhysicalDispatchStepResult step;
        step.selected_plan_uuid = dag.selected_plan_uuid;
        step.executed_physical_node_id = node.physical_node_id;
        step.causal_counter_id = node.causal_counter_id;
        step.output_descriptor_ids = node.output_descriptor_ids;
        step.mga_statement_context = dag.mga_statement_context;
        step.authority.engine_mga_snapshot_bound = true;
        step.data_access_observation_known = true;
        if (!inputs.empty()) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "SB_MODEL_COORDINATOR_LEG_FAILED_V1";
          return step;
        }
        step.result_handle_id = node.physical_node_id;
        step.output_row_count = subquery_batch.rows.size();
        step.materialized_output_batch = subquery_batch;
        return step;
      };
  executor::CanonicalPhysicalExecutorRegistration cte_registration;
  cte_registration.node_kind = executor::PhysicalNodeKind::kCte;
  cte_registration.implementation_id =
      "canonical.multimodel-consumer.typed.v1";
  cte_registration.executor_capability_uuid =
      cte_dag.nodes.back().executor_capability_uuid;
  cte_registration.executor_capability_abi_version = 1;
  cte_registration.engine_owned = true;
  cte_registration.accepts_optimizer_publication_v2 = true;
  cte_registration.execute =
      [](const auto& dag, const auto& node, const auto& inputs) {
        executor::CanonicalPhysicalDispatchStepResult step;
        step.selected_plan_uuid = dag.selected_plan_uuid;
        step.executed_physical_node_id = node.physical_node_id;
        step.causal_counter_id = node.causal_counter_id;
        step.output_descriptor_ids = node.output_descriptor_ids;
        step.mga_statement_context = dag.mga_statement_context;
        step.authority.engine_mga_snapshot_bound = true;
        step.data_access_observation_known = true;
        if (inputs.size() != 1 ||
            !inputs.front().materialized_output_batch.has_value()) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "SB_MODEL_COORDINATOR_LEG_FAILED_V1";
          return step;
        }
        step.result_handle_id = node.physical_node_id;
        step.input_row_count =
            inputs.front().materialized_output_batch->rows.size();
        step.output_row_count = step.input_row_count;
        step.materialized_output_batch =
            *inputs.front().materialized_output_batch;
        return step;
      };
  executor::CanonicalPhysicalDagDispatchRequest cte_request;
  cte_request.physical_dag = cte_dag;
  cte_request.mga_authority = Rcp080ContinuationAuthorityV1(cte_dag);
  cte_request.cancellation_requested = [] { return false; };
  cte_request.available_executors = {std::move(values_registration),
                                     std::move(cte_registration)};
  const auto cte = executor::ExecuteCanonicalPhysicalDag(cte_request);
  if (!cte.diagnostic.ok || cte.executed_steps.size() != 2 ||
      !cte.executed_steps.back().materialized_output_batch.has_value() ||
      cte.executed_steps.back().materialized_output_batch->rows.size() != 1) {
    return fail("cte");
  }

  auto recursive_dag = Rcp080ContinuationDagV1(
      executor::PhysicalNodeKind::kRecursiveCte, descriptor_ids,
      statement_context, descriptor_ids);
  recursive_dag.nodes[1].node_kind = executor::PhysicalNodeKind::kCte;
  recursive_dag.nodes.back().output_descriptor_ids = descriptor_ids;
  executor::CanonicalRecursiveCteWorkingRequest recursive_request;
  recursive_request.physical_dag = recursive_dag;
  recursive_request.selected_physical_node_id =
      recursive_dag.root_physical_node_id;
  recursive_request.anchor_batch =
      *cte.executed_steps.back().materialized_output_batch;
  recursive_request.recursive_step =
      [](const executor::DescriptorBatch& working, const std::size_t) {
        executor::DescriptorBatch empty;
        empty.columns = working.columns;
        return empty;
      };
  recursive_request.maximum_iteration_count = 2;
  recursive_request.maximum_working_row_count = 16;
  recursive_request.maximum_result_row_count = 16;
  recursive_request.mga_authority =
      Rcp080ContinuationAuthorityV1(recursive_dag);
  const auto recursive =
      executor::ExecuteCanonicalRecursiveCteWorking(recursive_request);
  if (!recursive.diagnostic.ok || !recursive.converged ||
      recursive.output_batch.rows.size() != 1 ||
      recursive.output_batch.columns.size() != descriptor_ids.size()) {
    return fail("bounded-recursion");
  }

  auto aggregate_dag = Rcp080ContinuationDagV1(
      executor::PhysicalNodeKind::kAggregate, descriptor_ids,
      statement_context);
  const auto aggregate_descriptor_id = std::uint32_t{80'080};
  aggregate_dag.nodes.back().output_descriptor_ids =
      {aggregate_descriptor_id};
  const auto* count_entry = executor::LookupCanonicalAggregateByFunctionV1(
      executor::CanonicalAggregateFunction::count);
  if (count_entry == nullptr) return fail("aggregate-registry");
  executor::CanonicalAggregateRuntimeRequest aggregate_request;
  aggregate_request.physical_dag = aggregate_dag;
  aggregate_request.selected_physical_node_id =
      aggregate_dag.root_physical_node_id;
  aggregate_request.descriptor =
      {count_entry->abi_version, count_entry->function,
       count_entry->builtin_id, count_entry->function_uuid, true};
  aggregate_request.input_batch = recursive.output_batch;
  auto count_descriptor = executor::MakeExecutorDescriptor(
      "int64", "canonical=int64;type_uuid=" +
                   std::string(kCanonicalInt64TypeUuid) +
                   ";nullable=false");
  count_descriptor.descriptor_uuid.canonical = Uuid(10'301);
  count_descriptor.descriptor_kind = "scalar";
  aggregate_request.result_column =
      {"multimodel_count", count_descriptor, false,
       aggregate_descriptor_id};
  aggregate_request.mga_authority =
      Rcp080ContinuationAuthorityV1(aggregate_dag);
  const auto aggregate =
      executor::ExecuteCanonicalAggregateRuntime(aggregate_request);
  if (!aggregate.diagnostic.ok || aggregate.output_batch.rows.size() != 1 ||
      aggregate.output_batch.columns.size() != 1 ||
      aggregate.output_batch.rows.front().values.front().encoded_value !=
          "1") {
    return fail("aggregate");
  }

  const std::vector<std::uint32_t> aggregate_descriptors =
      {aggregate_descriptor_id};
  auto sort_dag = Rcp080ContinuationDagV1(
      executor::PhysicalNodeKind::kSort, aggregate_descriptors,
      statement_context);
  executor::CanonicalDescriptorSortRequest sort_request;
  sort_request.physical_dag = sort_dag;
  sort_request.selected_physical_node_id = sort_dag.root_physical_node_id;
  sort_request.input_batch = aggregate.output_batch;
  executor::CanonicalDescriptorOrderTerm order_term;
  order_term.column = 0;
  order_term.expression_descriptor_id = aggregate_descriptor_id;
  sort_request.order_terms = {order_term};
  sort_request.deterministic_tie_evidence_uuid = Uuid(10'302);
  sort_request.maximum_pair_comparisons = 16;
  sort_request.mga_authority = Rcp080ContinuationAuthorityV1(sort_dag);
  const auto sorted = executor::ExecuteCanonicalDescriptorSort(sort_request);
  if (!sorted.diagnostic.ok || sorted.output_batch.rows.size() != 1) {
    return fail("sort");
  }

  auto window_dag = Rcp080ContinuationDagV1(
      executor::PhysicalNodeKind::kWindow, aggregate_descriptors,
      statement_context);
  const auto window_property = Uuid(10'303);
  const auto ordering_property = Uuid(10'304);
  const auto partition_property = Uuid(10'311);
  window_dag.nodes.back().required_property_uuids = {
      partition_property, ordering_property};
  window_dag.nodes.back().delivered_property_uuids = {window_property};
  executor::CanonicalWindowPartitionOrderRequest window_request;
  window_request.physical_dag = window_dag;
  window_request.selected_physical_node_id =
      window_dag.root_physical_node_id;
  window_request.input_batch = sorted.output_batch;
  window_request.partition_terms = {
      {.column = 0,
       .expression_descriptor_id = aggregate_descriptor_id}};
  window_request.order_terms = {order_term};
  window_request.window_property_uuid = window_property;
  window_request.partition_property_uuid = partition_property;
  window_request.ordering_property_uuid = ordering_property;
  window_request.term_binding_evidence_uuid = Uuid(10'305);
  window_request.deterministic_tie_evidence_uuid = Uuid(10'306);
  window_request.maximum_pair_comparisons = 16;
  window_request.mga_authority =
      Rcp080ContinuationAuthorityV1(window_dag);
  const auto window =
      executor::ExecuteCanonicalWindowPartitionOrder(window_request);
  executor::CanonicalWindowFrameRequest frame_request;
  frame_request.partition_order = window;
  frame_request.frame.frame_descriptor_uuid = Uuid(10'307);
  frame_request.frame_property_binding_evidence_uuid = Uuid(10'308);
  frame_request.mga_authority =
      Rcp080ContinuationAuthorityV1(window_dag);
  const auto framed = executor::ExecuteCanonicalWindowFrames(frame_request);
  if (!window.diagnostic.ok || window.partition_count != 1 ||
      window.partition_terms.size() != 1 || !framed.diagnostic.ok ||
      framed.ordered_batch.rows.size() != 1) {
    return fail("window");
  }

  auto set_dag = Rcp080ContinuationDagV1(
      executor::PhysicalNodeKind::kSetOperation, aggregate_descriptors,
      statement_context, aggregate_descriptors);
  set_dag.nodes.back().output_descriptor_ids = aggregate_descriptors;
  set_dag.nodes.back().implementation_id =
      "setop.union-all.ordinal.typed.v1";
  executor::CanonicalSetOperationAllRequest set_request;
  set_request.physical_dag = set_dag;
  set_request.selected_physical_node_id = set_dag.root_physical_node_id;
  set_request.left_batch = framed.ordered_batch;
  set_request.right_batch = framed.ordered_batch;
  set_request.result_columns = framed.ordered_batch.columns;
  set_request.operation = executor::CanonicalSetOperationKind::kUnion;
  set_request.alignment =
      executor::CanonicalSetOperationAlignment::kOrdinal;
  set_request.quantifier = executor::CanonicalSetOperationQuantifier::kAll;
  set_request.equality_profile =
      executor::CanonicalSetOperationEqualityProfile::kExactTyped;
  set_request.type_profile =
      executor::CanonicalSetOperationTypeProfile::kExact;
  set_request.maximum_output_row_count = 4;
  set_request.mga_authority = Rcp080ContinuationAuthorityV1(set_dag);
  const auto set_result =
      executor::ExecuteCanonicalSetOperationAll(set_request);
  if (!set_result.diagnostic.ok || set_result.output_batch.rows.size() != 2) {
    return fail("set-operation");
  }

  auto fetch_dag = Rcp080ContinuationDagV1(
      executor::PhysicalNodeKind::kLimit, aggregate_descriptors,
      statement_context);
  executor::CanonicalDescriptorLimitRequest fetch_request;
  fetch_request.physical_dag = fetch_dag;
  fetch_request.selected_physical_node_id = fetch_dag.root_physical_node_id;
  fetch_request.input_batch = set_result.output_batch;
  fetch_request.offset = 1;
  fetch_request.limit = 1;
  fetch_request.mga_authority = Rcp080ContinuationAuthorityV1(fetch_dag);
  const auto fetched =
      executor::ExecuteCanonicalDescriptorLimit(fetch_request);
  if (!fetched.diagnostic.ok || fetched.output_batch.rows.size() != 1 ||
      fetched.output_batch.rows.front().values.front().encoded_value != "1") {
    return fail("limit-offset-fetch");
  }

  executor::CanonicalResultPublicationRequest publication;
  publication.statement_uuid = statement_context.statement_uuid;
  publication.mga_authority = Rcp080ContinuationAuthorityV1(fetch_dag);
  publication.selected_physical_dag = fetch_dag;
  publication.selected_catalog_epoch_uuid = fetch_dag.catalog_epoch_uuid;
  publication.execution_attempt_uuid = Uuid(10'309);
  publication.transaction_effect_evidence_uuid = Uuid(10'310);
  publication.physical_output_batch = fetched.output_batch;
  publication.maximum_row_count = 4;
  executor::CanonicalResultColumnDescriptor published_column;
  published_column.ordinal = 0;
  published_column.name_utf8 = fetched.output_batch.columns.front().stable_name;
  published_column.descriptor_uuid = fetched.output_batch.columns.front()
                                         .descriptor.descriptor_uuid.canonical;
  published_column.type_uuid = kCanonicalInt64TypeUuid;
  published_column.nullability =
      executor::CanonicalResultNullability::kNonNull;
  publication.column_bindings.push_back(
      {0, true, std::move(published_column)});
  const auto published =
      executor::PublishCanonicalResultEnvelope(publication);
  if (!published.diagnostic.ok || !published.published ||
      published.row_stream.rows.size() != 1 ||
      published.row_stream.columns.size() != 1) {
    return fail("canonical-result");
  }
  std::cout <<
      "QOW-CES05-MULTIMODEL-CONTINUATION: passed;select=1;subquery=1;"
      "cte=1;bounded_recursion=1;aggregate=1;sort=1;window=1;set=1;"
      "offset=1;fetch=1;canonical_result=1\n";
  return true;
}

#if defined(SB_RCP080_PRODUCTION_QUERY_ROUTE)
struct ProductionFixtureV1 {
  std::filesystem::path directory;
  std::filesystem::path database_path;
  std::string database_uuid;
  std::string filespace_uuid;
  std::string schema_uuid;
  std::string principal_uuid;
  std::string session_uuid;
  std::array<std::string, 9> relation_uuids;
  std::uint64_t salt{0};

  ~ProductionFixtureV1() {
    std::error_code ignored;
    if (!directory.empty()) std::filesystem::remove_all(directory, ignored);
  }
};

sb_engine_uuid_t ProductionPublicUuidV1(const std::string& value,
                                        const platform::UuidKind kind) {
  sb_engine_uuid_t result{};
  const auto parsed = uuid::ParseTypedUuid(kind, value);
  if (parsed.ok()) {
    std::memcpy(result.bytes, parsed.value.value.bytes.data(),
                parsed.value.value.bytes.size());
  }
  return result;
}

class ProductionPublicSessionV1 {
 public:
  explicit ProductionPublicSessionV1(const ProductionFixtureV1& fixture)
      : database_path_(fixture.database_path.string()) {
    sb_engine_open_params_v1_t open{};
    open.struct_size = sizeof(open);
    open.abi_version = SB_ENGINE_ABI_VERSION_PACKED;
    open.database_path_utf8 = database_path_.data();
    open.database_path_size = database_path_.size();
    open.mode = SB_ENGINE_OPEN_VALIDATION_ONLY;
    if (sb_engine_open(&open, &engine_, nullptr) != SB_ENGINE_STATUS_OK ||
        engine_ == nullptr) {
      engine_ = nullptr;
      return;
    }
    sb_engine_session_params_v1_t begin{};
    begin.struct_size = sizeof(begin);
    begin.abi_version = SB_ENGINE_ABI_VERSION_PACKED;
    begin.effective_user_uuid = ProductionPublicUuidV1(
        fixture.principal_uuid, platform::UuidKind::principal);
    begin.session_uuid = ProductionPublicUuidV1(
        fixture.session_uuid, platform::UuidKind::session);
    begin.default_language_utf8 = "en";
    begin.default_language_size = 2;
    begin.trust_mode = SB_ENGINE_TRUST_SERVER_ISOLATED;
    if (sb_engine_session_begin(engine_, &begin, &session_, nullptr) !=
            SB_ENGINE_STATUS_OK ||
        session_ == nullptr) {
      (void)sb_engine_close(engine_, nullptr);
      engine_ = nullptr;
    }
  }

  ProductionPublicSessionV1(const ProductionPublicSessionV1&) = delete;
  ProductionPublicSessionV1& operator=(const ProductionPublicSessionV1&) =
      delete;

  ~ProductionPublicSessionV1() {
    if (session_ != nullptr) {
      sb_engine_session_end_params_v1_t end{};
      end.struct_size = sizeof(end);
      end.abi_version = SB_ENGINE_ABI_VERSION_PACKED;
      end.rollback_active_transactions = 1;
      end.cancel_open_results = 1;
      (void)sb_engine_session_end(session_, &end, nullptr);
    }
    if (engine_ != nullptr) (void)sb_engine_close(engine_, nullptr);
  }

  [[nodiscard]] sb_engine_session_t get() const { return session_; }

 private:
  std::string database_path_;
  sb_engine_handle_t engine_{nullptr};
  sb_engine_session_t session_{nullptr};
};

bool AcquireProductionStatementAuthorityV1(
    ProductionPublicSessionV1* session,
    api::EngineRequestContext* context,
    bridge::StatementContextReceiptHandle* receipt,
    std::string* bound_ast_uuid) {
  if (session == nullptr || session->get() == nullptr || context == nullptr ||
      receipt == nullptr || bound_ast_uuid == nullptr) {
    return false;
  }
  bridge::StatementContextAcquireRequest request;
  request.engine_context = context;
  request.exact_transaction_uuid = context->transaction_uuid.canonical;
  bridge::StatementContextReceiptView view;
  sb_engine_result_t diagnostic = nullptr;
  const auto status = bridge::AcquireStatementContextReceipt(
      session->get(), &request, receipt, &view, &diagnostic);
  if (diagnostic != nullptr) (void)sb_engine_result_release(diagnostic);
  if (status != SB_ENGINE_STATUS_OK || !*receipt ||
      view.statement_timestamp.empty() || !view.snapshot_complete ||
      !view.inventory_authoritative) {
    return false;
  }
  context->statement_uuid.canonical = view.statement_uuid;
  context->statement_timestamp = view.statement_timestamp;
  context->current_timestamp = view.statement_timestamp;
  context->statement_snapshot_uuid.canonical = view.statement_snapshot_uuid;
  context->snapshot_visible_through_local_transaction_id =
      view.visible_committed_high_watermark;
  context->statement_metadata_snapshot_engine_owned = true;
  context->statement_metadata_snapshot_uuid.canonical =
      view.statement_metadata_snapshot_uuid;
  context->statement_metadata_snapshot_visible_through_local_transaction_id =
      view.visible_committed_high_watermark;
  context->statement_metadata_snapshot_active_excluded_local_transaction_ids =
      view.active_excluded_local_transaction_ids;
  context->statement_metadata_snapshot_in_doubt_excluded_local_transaction_ids =
      view.in_doubt_excluded_local_transaction_ids;
  context->catalog_epoch_uuid.canonical = view.catalog_epoch_uuid;
  context->optimizer_capability_snapshot_uuid.canonical =
      view.optimizer_capability_snapshot_uuid;
  context->optimizer_resource_snapshot_uuid.canonical =
      view.optimizer_resource_snapshot_uuid;
  context->optimizer_route_snapshot_uuid.canonical =
      view.optimizer_route_snapshot_uuid;
  context->catalog_generation_id = view.catalog_generation_id;
  context->security_epoch = view.security_epoch;
  context->resource_epoch = view.resource_epoch;
  context->optimizer_route_epoch = view.optimizer_route_epoch;
  context->optimizer_route_generation = view.optimizer_route_generation;
  *bound_ast_uuid = view.bound_ast_uuid;
  return true;
}

std::uint64_t ProductionNowMillisV1() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

std::string ProductionUuidV1(const platform::UuidKind kind,
                             const std::uint64_t salt) {
  if (!uuid::UuidKindAllowsDurableIdentity(kind)) {
    const auto generated =
        uuid::GenerateCompatibilityUnixTimeV7(ProductionNowMillisV1() + salt);
    if (!generated.ok()) return {};
    const auto typed = uuid::MakeTypedUuid(kind, generated.value);
    return typed.ok() ? uuid::UuidToString(typed.value.value) : std::string{};
  }
  const auto generated = uuid::GenerateEngineIdentityV7(
      kind, ProductionNowMillisV1() + salt);
  return generated.ok() ? uuid::UuidToString(generated.value.value)
                        : std::string{};
}

std::string ProductionCoreTypeUuidV1(const std::string_view stable_name) {
  const auto manifest = dt::LoadCurrentCoreDatatypeCatalogManifest();
  if (!manifest.ok()) return {};
  const auto found = std::ranges::find_if(
      manifest.manifest.descriptor_rows,
      [&](const auto& row) { return row.stable_name == stable_name; });
  return found == manifest.manifest.descriptor_rows.end()
             ? std::string{}
             : uuid::UuidToString(found->descriptor_uuid.value);
}

api::EngineRequestContext ProductionBaseContextV1(
    const ProductionFixtureV1& fixture, std::string request_id) {
  api::EngineRequestContext context;
  context.trust_mode = api::EngineTrustMode::server_isolated;
  context.request_id = std::move(request_id);
  context.database_path = fixture.database_path.string();
  context.database_uuid.canonical = fixture.database_uuid;
  context.default_root_uuid.canonical = fixture.filespace_uuid;
  context.current_schema_uuid.canonical = fixture.schema_uuid;
  context.principal_uuid.canonical = fixture.principal_uuid;
  context.session_uuid.canonical = fixture.session_uuid;
  context.security_context_present = true;
  context.catalog_generation_id = 1;
  context.security_epoch = 1;
  context.resource_epoch = 1;
  context.name_resolution_epoch = 1;
  context.identifier_profile_uuid = "sbsql_v3";
  context.language_context.language_tag = "en";
  context.language_context.default_language_tag = "en";
  return context;
}

bool ProductionBeginV1(const ProductionFixtureV1& fixture,
                       std::string request_id,
                       api::EngineRequestContext* context) {
  if (context == nullptr) return false;
  api::EngineBeginTransactionRequest request;
  request.context = ProductionBaseContextV1(fixture, std::move(request_id));
  request.isolation_level = "repeatable_read";
  const auto begun = api::EngineBeginTransaction(request);
  if (!begun.ok) return false;
  *context = request.context;
  context->local_transaction_id = begun.local_transaction_id;
  context->transaction_uuid = begun.transaction_uuid;
  context->snapshot_visible_through_local_transaction_id =
      begun.snapshot_visible_through_local_transaction_id;
  context->transaction_isolation_level = begun.isolation_level;
  return true;
}

bool ProductionCommitV1(const api::EngineRequestContext& context) {
  api::EngineCommitTransactionRequest request;
  request.context = context;
  return api::EngineCommitTransaction(request).ok;
}

bool ProductionRollbackV1(const api::EngineRequestContext& context) {
  api::EngineRollbackTransactionRequest request;
  request.context = context;
  return api::EngineRollbackTransaction(request).ok;
}

void AddProductionAuthorizationV1(api::EngineRequestContext* context,
                                  const std::string& object_uuid,
                                  const std::uint64_t salt,
                                  const std::string_view right = "SELECT") {
  auto& authorization = context->authorization_context;
  authorization.present = true;
  if (authorization.authority_uuid.canonical.empty()) {
    authorization.authority_uuid.canonical =
        ProductionUuidV1(platform::UuidKind::object, salt);
    authorization.principal_uuid = context->principal_uuid;
    authorization.security_epoch = context->security_epoch;
    authorization.policy_epoch = 1;
    authorization.catalog_generation_id = context->catalog_generation_id;
    authorization.effective_subjects.push_back(
        {context->principal_uuid, "principal"});
  }
  api::EngineMaterializedAuthorizationGrant grant;
  grant.grant_uuid.canonical = ProductionUuidV1(
      platform::UuidKind::object, salt + 1 + authorization.grants.size());
  grant.subject_uuid = context->principal_uuid;
  grant.subject_kind = "principal";
  grant.target_uuid.canonical = object_uuid;
  grant.right = std::string(right);
  grant.security_epoch = context->security_epoch;
  authorization.grants.push_back(std::move(grant));
}

std::string ProductionDescriptorTypeUuidV1(
    const api::EngineDescriptor& descriptor) {
  constexpr std::string_view prefix = "type_uuid=";
  const auto begin = descriptor.encoded_descriptor.find(prefix);
  if (begin == std::string::npos) return {};
  const auto value_begin = begin + prefix.size();
  const auto end = descriptor.encoded_descriptor.find(';', value_begin);
  return descriptor.encoded_descriptor.substr(
      value_begin, end == std::string::npos ? std::string::npos
                                            : end - value_begin);
}

void SetProductionParserAuthorityV1(
    sbsql::NativeRelationalBindingContext* context) {
  auto& authority = context->engine_statement_authority;
  authority.statement_uuid = context->statement_uuid;
  authority.statement_timestamp = context->statement_timestamp;
  authority.transaction_uuid = context->owning_transaction_uuid;
  authority.statement_snapshot_uuid = context->statement_snapshot_uuid;
  authority.statement_metadata_snapshot_uuid =
      context->statement_metadata_snapshot_uuid;
  authority.catalog_epoch_uuid = context->catalog_epoch_uuid;
  authority.local_transaction_id = context->local_transaction_id;
  authority.snapshot_visible_through_local_transaction_id =
      context->snapshot_visible_through_local_transaction_id;
}

sbsql::ParserConfig ProductionParserConfigV1(
    const ProductionFixtureV1& fixture) {
  sbsql::ParserConfig config;
  config.parser_uuid = ProductionUuidV1(platform::UuidKind::object,
                                        fixture.salt + 300);
  config.bundle_contract_id = "sbp_sbsql@rcp080-production-v1";
  config.build_id = "rcp080-production-v1";
  return config;
}

sbsql::SessionContext ProductionParserSessionV1(
    const ProductionFixtureV1& fixture) {
  sbsql::SessionContext session;
  session.authenticated = true;
  session.session_uuid = fixture.session_uuid;
  session.connection_uuid =
      ProductionUuidV1(platform::UuidKind::object, fixture.salt + 301);
  session.database_uuid = fixture.database_uuid;
  session.dialect_profile_uuid =
      ProductionUuidV1(platform::UuidKind::object, fixture.salt + 302);
  session.catalog_epoch = 1;
  session.security_policy_epoch = 1;
  session.descriptor_epoch = 1;
  return session;
}

template <std::size_t SourceCount>
sbsql::NativeRelationalBindingContext ProductionBindingContextV1(
    const sbsql::NativeRelationalAstDocument& ast,
    const api::EngineRequestContext& reader,
    const std::string& bound_ast_uuid,
    const std::array<api::MgaRelationStorageDescriptor, SourceCount>& storage) {
  sbsql::NativeRelationalBindingContext context;
  context.bound_ast_uuid = bound_ast_uuid;
  context.catalog_epoch_uuid = reader.catalog_epoch_uuid.canonical;
  context.security_context_uuid =
      reader.authorization_context.authority_uuid.canonical;
  context.statement_uuid = reader.statement_uuid.canonical;
  context.statement_timestamp = reader.statement_timestamp;
  context.owning_transaction_uuid = reader.transaction_uuid.canonical;
  context.statement_snapshot_uuid = reader.statement_snapshot_uuid.canonical;
  context.statement_metadata_snapshot_uuid =
      reader.statement_metadata_snapshot_uuid.canonical;
  context.local_transaction_id = reader.local_transaction_id;
  context.snapshot_visible_through_local_transaction_id =
      reader.snapshot_visible_through_local_transaction_id;
  context.search_analyzer_uuid =
      ProductionUuidV1(platform::UuidKind::object, 89'001);
  context.search_analyzer_generation = reader.catalog_generation_id;
  std::vector<std::vector<std::uint32_t>> source_expression_ids;
  std::vector<std::optional<std::uint32_t>>
      time_series_boolean_descriptor_ids(storage.size());
  std::uint32_t next_descriptor_id = 1;
  std::uint32_t next_expression_id = 1;
  std::uint32_t next_output_id = 1;
  for (std::size_t ordinal = 0; ordinal < storage.size(); ++ordinal) {
    const auto& ast_source = ast.catalog_relation_sources[ordinal];
    sbsql::NativeCatalogRelationBindingInput source;
    source.source_id = ast_source.source_id;
    source.resolution_state =
        sbsql::NativeCatalogRelationResolutionState::kBound;
    source.object_uuid = storage[ordinal].relation_uuid.canonical;
    source.resolved_schema_uuid = reader.current_schema_uuid.canonical;
    switch (ast_source.source_kind) {
      case sbsql::NativeRelationSourceAstKind::kDocument:
        source.resolved_object_type = "document_collection";
        break;
      case sbsql::NativeRelationSourceAstKind::kGraph:
        source.resolved_object_type = "graph";
        break;
      case sbsql::NativeRelationSourceAstKind::kKeyValue:
        source.resolved_object_type = "key_value";
        break;
      case sbsql::NativeRelationSourceAstKind::kTimeSeries:
        source.resolved_object_type = "time_series";
        break;
      case sbsql::NativeRelationSourceAstKind::kVector:
        source.resolved_object_type = "vector";
        break;
      case sbsql::NativeRelationSourceAstKind::kSearch:
        source.resolved_object_type = "search";
        break;
      case sbsql::NativeRelationSourceAstKind::kSpatial:
        source.resolved_object_type = "spatial_collection";
        break;
      case sbsql::NativeRelationSourceAstKind::kColumnar:
        source.resolved_object_type = "logical_relation";
        break;
      default: source.resolved_object_type = "table"; break;
    }
    source.catalog_generation_id = reader.catalog_generation_id;
    source.security_epoch = reader.security_epoch;
    source.resource_epoch = reader.resource_epoch;
    std::vector<std::uint32_t> expression_ids;
    const bool vector_source = ast_source.source_kind ==
                               sbsql::NativeRelationSourceAstKind::kVector;
    const bool search_source = ast_source.source_kind ==
                               sbsql::NativeRelationSourceAstKind::kSearch;
    const bool key_value_source =
        ast_source.source_kind ==
        sbsql::NativeRelationSourceAstKind::kKeyValue;
    const bool time_series_source =
        ast_source.source_kind ==
        sbsql::NativeRelationSourceAstKind::kTimeSeries;
    const auto bound_storage_column_count =
        search_source ? std::size_t{1}
                      : key_value_source || time_series_source
                            ? std::size_t{0}
                            : storage[ordinal].columns.size();
    if (key_value_source) {
      const std::array<std::string_view, 3> names{
          "row_uuid", "key", "value"};
      for (std::size_t output_ordinal = 0; output_ordinal < names.size();
           ++output_ordinal) {
        const bool row_identity = output_ordinal == 0;
        const auto* column =
            row_identity ? nullptr : &storage[ordinal].columns[output_ordinal - 1];
        const auto descriptor_id = next_descriptor_id++;
        const auto descriptor_uuid =
            row_identity ? storage[ordinal].descriptor_uuid.canonical
                         : column->value_descriptor.descriptor_uuid.canonical;
        const auto type_uuid =
            row_identity ? ProductionCoreTypeUuidV1("uuid")
                         : ProductionDescriptorTypeUuidV1(
                               column->value_descriptor);
        context.descriptors.push_back(
            {descriptor_id, descriptor_uuid, type_uuid,
             sbsql::BoundNullability::kNonNull, std::nullopt, std::nullopt,
             {}, row_identity ? "uuid" : "text"});
        const auto bound_name_uuid =
            row_identity ? storage[ordinal].relation_uuid.canonical
                         : column->column_uuid.canonical;
        source.columns.push_back(
            {static_cast<std::uint32_t>(output_ordinal), bound_name_uuid,
             descriptor_id, std::string(names[output_ordinal])});
        const auto expression_id = next_expression_id++;
        expression_ids.push_back(expression_id);
        context.expressions.push_back(
            {expression_id, descriptor_id, std::nullopt, bound_name_uuid});
        context.outputs.push_back(
            {next_output_id++, expression_id,
             std::string(names[output_ordinal]), descriptor_id, true,
             static_cast<std::uint32_t>(output_ordinal),
             ast.relations[ordinal].relation_id});
      }
    } else if (time_series_source) {
      const std::array<std::string_view, 6> names{
          "row_uuid", "series_uuid", "metric_uuid", "point_timestamp",
          "tags", "value"};
      for (std::size_t output_ordinal = 0; output_ordinal < names.size();
           ++output_ordinal) {
        const bool row_identity = output_ordinal == 0;
        const bool series_identity = output_ordinal == 1;
        const auto* column = row_identity || series_identity
                                 ? nullptr
                                 : &storage[ordinal]
                                        .columns[output_ordinal - 2];
        const auto descriptor_id = next_descriptor_id++;
        const auto descriptor_uuid =
            row_identity ? storage[ordinal].descriptor_uuid.canonical
            : series_identity ? storage[ordinal].schema_uuid.canonical
                              : column->value_descriptor.descriptor_uuid
                                    .canonical;
        const auto type_uuid =
            row_identity || series_identity
                ? ProductionCoreTypeUuidV1("uuid")
                : ProductionDescriptorTypeUuidV1(column->value_descriptor);
        const auto canonical_type =
            row_identity || series_identity
                ? std::string("uuid")
                : column->value_descriptor.canonical_type_name;
        const auto timezone = output_ordinal == 3
                                  ? std::optional<std::string>("UTC")
                                  : std::nullopt;
        context.descriptors.push_back(
            {descriptor_id, descriptor_uuid, type_uuid,
             sbsql::BoundNullability::kNonNull, std::nullopt, timezone, {},
             canonical_type});
        const auto bound_name_uuid =
            row_identity ? storage[ordinal].descriptor_uuid.canonical
            : series_identity ? storage[ordinal].schema_uuid.canonical
                              : column->column_uuid.canonical;
        source.columns.push_back(
            {static_cast<std::uint32_t>(output_ordinal), bound_name_uuid,
             descriptor_id, std::string(names[output_ordinal])});
        const auto expression_id = next_expression_id++;
        expression_ids.push_back(expression_id);
        context.expressions.push_back(
            {expression_id, descriptor_id, std::nullopt, bound_name_uuid});
        context.outputs.push_back(
            {next_output_id++, expression_id,
             std::string(names[output_ordinal]), descriptor_id, true,
             static_cast<std::uint32_t>(output_ordinal),
             ast.relations[ordinal].relation_id});
      }
      const auto boolean_descriptor_id = next_descriptor_id++;
      const auto boolean_type_uuid = ProductionCoreTypeUuidV1("boolean");
      context.descriptors.push_back(
          {boolean_descriptor_id, boolean_type_uuid, boolean_type_uuid,
           sbsql::BoundNullability::kNonNull, std::nullopt, std::nullopt, {},
           "boolean"});
      time_series_boolean_descriptor_ids[ordinal] = boolean_descriptor_id;
    } else {
      for (std::size_t column_ordinal = 0;
           column_ordinal < bound_storage_column_count; ++column_ordinal) {
        const auto& column = storage[ordinal].columns[column_ordinal];
        const auto descriptor_id = next_descriptor_id++;
        const auto canonical_type =
            column.value_descriptor.canonical_type_name;
        context.descriptors.push_back(
            {descriptor_id, column.value_descriptor.descriptor_uuid.canonical,
             ProductionDescriptorTypeUuidV1(column.value_descriptor),
             column.nullable ? sbsql::BoundNullability::kNullable
                             : sbsql::BoundNullability::kNonNull,
             std::nullopt, std::nullopt, {}, canonical_type});
        if (vector_source && column_ordinal == 0) {
          context.descriptors.back().width_precision_scale.width = 3;
        }
        source.columns.push_back(
            {static_cast<std::uint32_t>(column_ordinal),
             column.column_uuid.canonical, descriptor_id,
             column.canonical_name_key});
        if (!vector_source && !search_source) {
          const auto expression_id = next_expression_id++;
          expression_ids.push_back(expression_id);
          context.expressions.push_back(
              {expression_id, descriptor_id, std::nullopt,
               column.column_uuid.canonical});
          context.outputs.push_back(
              {next_output_id++, expression_id, column.canonical_name_key,
               descriptor_id, true,
               static_cast<std::uint32_t>(column_ordinal),
               ast.relations[ordinal].relation_id});
        }
      }
    }
    if (vector_source) {
      const std::array<std::string_view, 3> names{
          "row_uuid", "distance", "score"};
      const std::array<std::string, 3> types{
          ProductionCoreTypeUuidV1("uuid"),
          ProductionCoreTypeUuidV1("real64"),
          ProductionCoreTypeUuidV1("real64")};
      for (std::size_t output_ordinal = 0;
           output_ordinal < names.size(); ++output_ordinal) {
        const auto descriptor_id = next_descriptor_id++;
        context.descriptors.push_back(
            {descriptor_id,
             ProductionUuidV1(platform::UuidKind::object,
                              90'000 + descriptor_id),
             types[output_ordinal], sbsql::BoundNullability::kNonNull,
             std::nullopt, std::nullopt, {},
             output_ordinal == 0 ? "uuid" : "real64"});
        const auto expression_id = next_expression_id++;
        expression_ids.push_back(expression_id);
        context.expressions.push_back(
            {expression_id, descriptor_id, std::nullopt,
             storage[ordinal].relation_uuid.canonical});
        context.outputs.push_back(
            {next_output_id++, expression_id, std::string(names[output_ordinal]),
             descriptor_id, true,
             static_cast<std::uint32_t>(output_ordinal),
             ast.relations[ordinal].relation_id});
      }
    }
    if (search_source) {
      const std::array<std::string_view, 5> names{
          "document_uuid", "analyzer_uuid", "analyzer_generation", "score",
          "rank"};
      const std::array<std::string, 5> types{
          ProductionCoreTypeUuidV1("uuid"),
          ProductionCoreTypeUuidV1("uuid"),
          ProductionCoreTypeUuidV1("uint64"),
          ProductionCoreTypeUuidV1("real64"),
          ProductionCoreTypeUuidV1("uint64")};
      for (std::size_t output_ordinal = 0;
           output_ordinal < names.size(); ++output_ordinal) {
        const auto descriptor_id = next_descriptor_id++;
        context.descriptors.push_back(
            {descriptor_id,
             ProductionUuidV1(platform::UuidKind::object,
                              91'000 + descriptor_id),
             types[output_ordinal], sbsql::BoundNullability::kNonNull,
             std::nullopt, std::nullopt, {},
             output_ordinal == 0 || output_ordinal == 1
                 ? "uuid"
                 : output_ordinal == 3 ? "real64" : "uint64"});
        const auto expression_id = next_expression_id++;
        expression_ids.push_back(expression_id);
        context.expressions.push_back(
            {expression_id, descriptor_id, std::nullopt,
             output_ordinal == 1 || output_ordinal == 2
                 ? context.search_analyzer_uuid
                 : storage[ordinal].relation_uuid.canonical});
        context.outputs.push_back(
            {next_output_id++, expression_id,
             std::string(names[output_ordinal]), descriptor_id, true,
             static_cast<std::uint32_t>(output_ordinal),
             ast.relations[ordinal].relation_id});
      }
    }
    source_expression_ids.push_back(std::move(expression_ids));
    context.catalog_relations.push_back(std::move(source));
  }
  for (std::size_t ordinal = 1; ordinal < storage.size(); ++ordinal) {
    const auto& source = ast.catalog_relation_sources[ordinal];
    std::unordered_set<std::uint32_t> closure;
    std::vector<std::uint32_t> pending{
        source.model_operation_expression_ids.front()};
    const auto relation = std::ranges::find_if(
        ast.relations, [&](const auto& candidate) {
          return candidate.relation_kind ==
                     sbsql::NativeRelationAstKind::kCatalogSource &&
                 candidate.relation_source_ids ==
                     std::vector<std::uint32_t>{source.source_id};
        });
    pending.insert(pending.end(), relation->predicate_expression_ids.begin(),
                   relation->predicate_expression_ids.end());
    while (!pending.empty()) {
      const auto expression_id = pending.back();
      pending.pop_back();
      if (!closure.insert(expression_id).second) continue;
      const auto expression = std::ranges::find_if(
          ast.expressions, [&](const auto& candidate) {
            return candidate.expression_id == expression_id;
          });
      pending.insert(pending.end(), expression->child_expression_ids.begin(),
                     expression->child_expression_ids.end());
    }
    std::vector<std::uint32_t> ordered(closure.begin(), closure.end());
    std::ranges::sort(ordered);
    const auto primary_id = source.model_operation_expression_ids.front();
    const auto primary = std::ranges::find_if(
        ast.expressions, [&](const auto& candidate) {
          return candidate.expression_id == primary_id;
        });
    const auto alias_id = primary->child_expression_ids.empty()
                              ? 0
                              : primary->child_expression_ids.front();
    for (const auto ast_expression_id : ordered) {
      std::optional<std::string> bound_name;
      if (ast_expression_id == alias_id ||
          (ast_expression_id == primary_id &&
           (source.source_kind ==
                sbsql::NativeRelationSourceAstKind::kSpatial ||
            source.source_kind ==
                sbsql::NativeRelationSourceAstKind::kColumnar))) {
        bound_name = storage[ordinal].relation_uuid.canonical;
      } else if (source.source_kind ==
                     sbsql::NativeRelationSourceAstKind::kSearch &&
                 source.model_search_analyzer_expression_id ==
                     ast_expression_id) {
        bound_name = context.search_analyzer_uuid;
      }
      auto descriptor_id =
          context.catalog_relations[ordinal].columns.front().descriptor_id;
      if (source.source_kind ==
          sbsql::NativeRelationSourceAstKind::kKeyValue) {
        descriptor_id =
            context.catalog_relations[ordinal].columns[1].descriptor_id;
      } else if (source.source_kind ==
                 sbsql::NativeRelationSourceAstKind::kTimeSeries) {
        if (ast_expression_id == alias_id) {
          descriptor_id =
              context.catalog_relations[ordinal].columns[0].descriptor_id;
        } else if (ast_expression_id == primary_id) {
          descriptor_id = *time_series_boolean_descriptor_ids[ordinal];
        } else {
          descriptor_id =
              context.catalog_relations[ordinal].columns[3].descriptor_id;
        }
      } else if (source.source_kind ==
              sbsql::NativeRelationSourceAstKind::kGraph &&
          source.model_pattern_expression_id == ast_expression_id) {
        descriptor_id = context.catalog_relations[ordinal].columns[3]
                            .descriptor_id;
      } else if (source.source_kind ==
                     sbsql::NativeRelationSourceAstKind::kVector &&
                 source.model_vector_metric_expression_id ==
                     ast_expression_id) {
        descriptor_id = context.catalog_relations[ordinal].columns[1]
                            .descriptor_id;
      } else if (source.source_kind ==
                     sbsql::NativeRelationSourceAstKind::kVector &&
                 source.model_vector_top_k_expression_id ==
                     ast_expression_id) {
        descriptor_id = next_descriptor_id++;
        context.descriptors.push_back(
            {descriptor_id,
             ProductionUuidV1(platform::UuidKind::object,
                              90'000 + descriptor_id),
             ProductionCoreTypeUuidV1("uint64"),
             sbsql::BoundNullability::kNonNull, std::nullopt, std::nullopt,
             {}, "uint64"});
      }
      context.expressions.push_back(
          {next_expression_id++, descriptor_id, std::nullopt, bound_name});
    }
  }
  std::vector<std::uint32_t> accumulated_expression_ids;
  for (std::size_t join_ordinal = 1; join_ordinal < storage.size();
       ++join_ordinal) {
    if (join_ordinal == 1) {
      accumulated_expression_ids = source_expression_ids.front();
    }
    accumulated_expression_ids.insert(
        accumulated_expression_ids.end(),
        source_expression_ids[join_ordinal].begin(),
        source_expression_ids[join_ordinal].end());
    const auto relation_id =
        static_cast<std::uint32_t>(storage.size() + join_ordinal);
    context.relations.push_back({relation_id, "join.cross.v1"});
    for (std::size_t ordinal = 0;
         ordinal < accumulated_expression_ids.size(); ++ordinal) {
      const auto expression_id = accumulated_expression_ids[ordinal];
      const auto expression = std::ranges::find_if(
          context.expressions, [&](const auto& candidate) {
            return candidate.expression_id == expression_id;
          });
      const auto source_output = std::ranges::find_if(
          context.outputs, [&](const auto& candidate) {
            return candidate.expression_id == expression_id;
          });
      context.outputs.push_back(
          {next_output_id++, expression_id,
           source_output->output_name_utf8, expression->descriptor_id, true,
           static_cast<std::uint32_t>(ordinal), relation_id});
    }
  }
  SetProductionParserAuthorityV1(&context);
  return context;
}

bool ProductionKeyValuePublicProjectionBindingExactV1(
    const sbsql::NativeRelationalBindingContext& context,
    const api::MgaRelationStorageDescriptor& persisted,
    const std::uint32_t relation_id) {
  if (persisted.columns.size() != 3 ||
      persisted.columns[0].canonical_name_key != "key" ||
      persisted.columns[1].canonical_name_key != "value" ||
      persisted.columns[2].canonical_name_key != "expires_at") {
    return false;
  }
  const auto source = std::ranges::find_if(
      context.catalog_relations, [&](const auto& candidate) {
        return candidate.resolved_object_type == "key_value" &&
               candidate.object_uuid == persisted.relation_uuid.canonical;
      });
  if (source == context.catalog_relations.end() ||
      std::ranges::count_if(context.catalog_relations,
                            [&](const auto& candidate) {
                              return candidate.resolved_object_type ==
                                         "key_value" &&
                                     candidate.object_uuid ==
                                         persisted.relation_uuid.canonical;
                            }) != 1 ||
      source->columns.size() != 3) {
    return false;
  }
  const std::array<std::string_view, 3> names{
      "row_uuid", "key", "value"};
  const std::array<std::string, 3> descriptor_uuids{
      persisted.descriptor_uuid.canonical,
      persisted.columns[0].value_descriptor.descriptor_uuid.canonical,
      persisted.columns[1].value_descriptor.descriptor_uuid.canonical};
  const std::array<std::string, 3> type_uuids{
      ProductionCoreTypeUuidV1("uuid"),
      ProductionDescriptorTypeUuidV1(
          persisted.columns[0].value_descriptor),
      ProductionDescriptorTypeUuidV1(
          persisted.columns[1].value_descriptor)};
  const std::array<std::string, 3> canonical_types{"uuid", "text", "text"};
  const std::array<std::string, 3> bound_name_uuids{
      persisted.relation_uuid.canonical,
      persisted.columns[0].column_uuid.canonical,
      persisted.columns[1].column_uuid.canonical};
  const auto descriptor_for = [&](const std::uint32_t descriptor_id) {
    return std::ranges::find_if(
        context.descriptors, [&](const auto& descriptor) {
          return descriptor.descriptor_id == descriptor_id;
        });
  };
  const auto expression_for = [&](const std::uint32_t expression_id) {
    return std::ranges::find_if(
        context.expressions, [&](const auto& expression) {
          return expression.expression_id == expression_id;
        });
  };
  std::vector<const sbsql::NativeOutputBindingInput*> outputs;
  for (const auto& output : context.outputs) {
    if (output.relation_id == relation_id) outputs.push_back(&output);
  }
  std::ranges::sort(outputs, {},
                    &sbsql::NativeOutputBindingInput::ordinal);
  if (outputs.size() != 3) return false;
  std::unordered_set<std::uint32_t> public_expression_ids;
  for (std::size_t ordinal = 0; ordinal < names.size(); ++ordinal) {
    const auto& column = source->columns[ordinal];
    const auto* output = outputs[ordinal];
    const auto descriptor = descriptor_for(column.descriptor_id);
    const auto expression = expression_for(output->expression_id);
    if (column.ordinal != ordinal ||
        column.canonical_name_key != names[ordinal] ||
        column.column_uuid != bound_name_uuids[ordinal] ||
        descriptor == context.descriptors.end() ||
        descriptor->descriptor_uuid != descriptor_uuids[ordinal] ||
        descriptor->type_uuid != type_uuids[ordinal] ||
        descriptor->canonical_type_name != canonical_types[ordinal] ||
        descriptor->nullability != sbsql::BoundNullability::kNonNull ||
        descriptor->collation_uuid.has_value() ||
        descriptor->timezone_profile_id.has_value() || !output->visible ||
        output->ordinal != ordinal ||
        output->output_name_utf8 != names[ordinal] ||
        output->descriptor_id != column.descriptor_id ||
        expression == context.expressions.end() ||
        expression->descriptor_id != column.descriptor_id ||
        expression->bound_name_uuid != bound_name_uuids[ordinal] ||
        !public_expression_ids.insert(output->expression_id).second) {
      return false;
    }
  }
  if (std::ranges::any_of(context.descriptors, [&](const auto& descriptor) {
        return descriptor.descriptor_uuid ==
               persisted.columns[2].value_descriptor.descriptor_uuid.canonical;
      }) ||
      std::ranges::any_of(source->columns, [](const auto& column) {
        return column.canonical_name_key == "expires_at";
      }) ||
      std::ranges::any_of(outputs, [](const auto* output) {
        return output->output_name_utf8 == "expires_at";
      })) {
    return false;
  }
  const auto key_descriptor_id = source->columns[1].descriptor_id;
  std::size_t key_operation_expression_count = 0;
  std::size_t relation_bound_alias_count = 0;
  std::size_t unbound_key_operation_count = 0;
  for (const auto& expression : context.expressions) {
    if (expression.descriptor_id != key_descriptor_id ||
        public_expression_ids.contains(expression.expression_id)) {
      continue;
    }
    ++key_operation_expression_count;
    if (expression.bound_name_uuid == persisted.relation_uuid.canonical) {
      ++relation_bound_alias_count;
    } else if (!expression.bound_name_uuid.has_value()) {
      ++unbound_key_operation_count;
    } else {
      return false;
    }
  }
  return key_operation_expression_count == 4 &&
         relation_bound_alias_count == 1 &&
         unbound_key_operation_count == 3;
}

bool ProductionKeyValuePublicProjectionBindingProofV1(
    const sbsql::NativeRelationalBindingContext& exact,
    const api::MgaRelationStorageDescriptor& persisted,
    const std::uint32_t relation_id) {
  bool passed = Require(
      ProductionKeyValuePublicProjectionBindingExactV1(
          exact, persisted, relation_id),
      "exact key/value public projection binding was refused");
  std::size_t refusal_count = 0;
  const auto require_refusal =
      [&](sbsql::NativeRelationalBindingContext candidate,
          const std::string_view case_id) {
        ++refusal_count;
        return Require(
            !ProductionKeyValuePublicProjectionBindingExactV1(
                candidate, persisted, relation_id),
            "key/value public projection mutation was admitted:" +
                std::string(case_id));
      };
  const auto source_for = [&](auto* context) {
    return std::ranges::find_if(
        context->catalog_relations, [&](const auto& source) {
          return source.resolved_object_type == "key_value" &&
                 source.object_uuid == persisted.relation_uuid.canonical;
        });
  };
  const auto output_for = [&](auto* context, const std::uint32_t ordinal) {
    return std::ranges::find_if(context->outputs, [&](const auto& output) {
      return output.relation_id == relation_id && output.ordinal == ordinal;
    });
  };
  const auto descriptor_for = [&](auto* context,
                                  const std::uint32_t descriptor_id) {
    return std::ranges::find_if(
        context->descriptors, [&](const auto& descriptor) {
          return descriptor.descriptor_id == descriptor_id;
        });
  };
  const auto expression_for = [&](auto* context,
                                  const std::uint32_t expression_id) {
    return std::ranges::find_if(
        context->expressions, [&](const auto& expression) {
          return expression.expression_id == expression_id;
        });
  };

  auto missing = exact;
  source_for(&missing)->columns.pop_back();
  passed &= require_refusal(std::move(missing), "missing");

  auto extra = exact;
  auto extra_column = source_for(&extra)->columns.back();
  extra_column.ordinal = 3;
  source_for(&extra)->columns.push_back(std::move(extra_column));
  passed &= require_refusal(std::move(extra), "extra");

  auto reordered = exact;
  std::swap(source_for(&reordered)->columns[0],
            source_for(&reordered)->columns[1]);
  passed &= require_refusal(std::move(reordered), "reordered");

  auto duplicated = exact;
  source_for(&duplicated)->columns[2].column_uuid =
      source_for(&duplicated)->columns[1].column_uuid;
  passed &= require_refusal(std::move(duplicated), "duplicated");

  auto exposed_ttl = exact;
  auto* exposed_source = &*source_for(&exposed_ttl);
  const auto exposed_output = output_for(&exposed_ttl, 2);
  const auto exposed_descriptor =
      descriptor_for(&exposed_ttl, exposed_source->columns[2].descriptor_id);
  const auto exposed_expression =
      expression_for(&exposed_ttl, exposed_output->expression_id);
  exposed_source->columns[2].column_uuid =
      persisted.columns[2].column_uuid.canonical;
  exposed_source->columns[2].canonical_name_key = "expires_at";
  exposed_descriptor->descriptor_uuid =
      persisted.columns[2].value_descriptor.descriptor_uuid.canonical;
  exposed_descriptor->type_uuid = ProductionDescriptorTypeUuidV1(
      persisted.columns[2].value_descriptor);
  exposed_descriptor->canonical_type_name = "timestamp_tz";
  exposed_descriptor->nullability = sbsql::BoundNullability::kNullable;
  exposed_output->output_name_utf8 = "expires_at";
  exposed_expression->bound_name_uuid =
      persisted.columns[2].column_uuid.canonical;
  passed &= require_refusal(std::move(exposed_ttl), "hidden_expires_at");

  auto wrong_name = exact;
  source_for(&wrong_name)->columns[1].canonical_name_key = "wrong_key";
  output_for(&wrong_name, 1)->output_name_utf8 = "wrong_key";
  passed &= require_refusal(std::move(wrong_name), "wrong_name");

  auto wrong_type = exact;
  const auto wrong_type_descriptor = descriptor_for(
      &wrong_type, source_for(&wrong_type)->columns[1].descriptor_id);
  wrong_type_descriptor->type_uuid = ProductionCoreTypeUuidV1("uuid");
  wrong_type_descriptor->canonical_type_name = "uuid";
  passed &= require_refusal(std::move(wrong_type), "wrong_type");

  auto nullable = exact;
  descriptor_for(&nullable, source_for(&nullable)->columns[1].descriptor_id)
      ->nullability = sbsql::BoundNullability::kNullable;
  passed &= require_refusal(std::move(nullable), "nullable");

  auto wrong_row_descriptor = exact;
  descriptor_for(&wrong_row_descriptor,
                 source_for(&wrong_row_descriptor)->columns[0].descriptor_id)
      ->descriptor_uuid = persisted.columns[0]
                              .value_descriptor.descriptor_uuid.canonical;
  passed &= require_refusal(std::move(wrong_row_descriptor),
                            "wrong_row_descriptor_identity");

  auto wrong_row_bound_name = exact;
  const auto row_output = output_for(&wrong_row_bound_name, 0);
  expression_for(&wrong_row_bound_name, row_output->expression_id)
      ->bound_name_uuid = persisted.columns[0].column_uuid.canonical;
  passed &= require_refusal(std::move(wrong_row_bound_name),
                            "wrong_row_bound_name_identity");

  auto wrong_lineage = exact;
  auto* wrong_lineage_source = &*source_for(&wrong_lineage);
  const auto wrong_lineage_output = output_for(&wrong_lineage, 1);
  const auto wrong_lineage_descriptor = descriptor_for(
      &wrong_lineage, wrong_lineage_source->columns[1].descriptor_id);
  wrong_lineage_source->columns[1].column_uuid =
      persisted.columns[1].column_uuid.canonical;
  wrong_lineage_descriptor->descriptor_uuid =
      persisted.columns[1].value_descriptor.descriptor_uuid.canonical;
  expression_for(&wrong_lineage, wrong_lineage_output->expression_id)
      ->bound_name_uuid = persisted.columns[1].column_uuid.canonical;
  passed &= require_refusal(std::move(wrong_lineage),
                            "wrong_key_value_descriptor_lineage");

  auto operation_on_row = exact;
  const auto* operation_source = &*source_for(&operation_on_row);
  const auto row_descriptor_id = operation_source->columns[0].descriptor_id;
  const auto key_descriptor_id = operation_source->columns[1].descriptor_id;
  const std::unordered_set<std::uint32_t> public_expression_ids{
      output_for(&operation_on_row, 0)->expression_id,
      output_for(&operation_on_row, 1)->expression_id,
      output_for(&operation_on_row, 2)->expression_id};
  const auto operation_expression = std::ranges::find_if(
      operation_on_row.expressions, [&](const auto& expression) {
        return expression.descriptor_id == key_descriptor_id &&
               !public_expression_ids.contains(expression.expression_id) &&
               expression.bound_name_uuid == persisted.relation_uuid.canonical;
      });
  operation_expression->descriptor_id = row_descriptor_id;
  passed &= require_refusal(std::move(operation_on_row),
                            "key_operation_bound_to_row_descriptor");

  passed &= Require(refusal_count == 12,
                    "key/value public projection refusal matrix is not 12");
  return passed;
}

bool ProductionTimeSeriesPublicProjectionBindingExactV1(
    const sbsql::NativeRelationalBindingContext& context,
    const api::MgaRelationStorageDescriptor& persisted,
    const std::uint32_t relation_id) {
  const std::array<std::string_view, 4> persisted_names{
      "metric_uuid", "point_timestamp", "tags", "value"};
  if (persisted.columns.size() != persisted_names.size()) return false;
  for (std::size_t ordinal = 0; ordinal < persisted_names.size(); ++ordinal) {
    if (persisted.columns[ordinal].ordinal != ordinal ||
        persisted.columns[ordinal].canonical_name_key !=
            persisted_names[ordinal] ||
        persisted.columns[ordinal].nullable) {
      return false;
    }
  }
  const auto source = std::ranges::find_if(
      context.catalog_relations, [&](const auto& candidate) {
        return candidate.resolved_object_type == "time_series" &&
               candidate.object_uuid == persisted.relation_uuid.canonical;
      });
  if (source == context.catalog_relations.end() ||
      std::ranges::count_if(context.catalog_relations,
                            [&](const auto& candidate) {
                              return candidate.resolved_object_type ==
                                         "time_series" &&
                                     candidate.object_uuid ==
                                         persisted.relation_uuid.canonical;
                            }) != 1 ||
      source->columns.size() != 6) {
    return false;
  }
  const std::array<std::string_view, 6> names{
      "row_uuid", "series_uuid", "metric_uuid", "point_timestamp", "tags",
      "value"};
  const std::array<std::string, 6> descriptor_uuids{
      persisted.descriptor_uuid.canonical,
      persisted.schema_uuid.canonical,
      persisted.columns[0].value_descriptor.descriptor_uuid.canonical,
      persisted.columns[1].value_descriptor.descriptor_uuid.canonical,
      persisted.columns[2].value_descriptor.descriptor_uuid.canonical,
      persisted.columns[3].value_descriptor.descriptor_uuid.canonical};
  const std::array<std::string, 6> type_uuids{
      ProductionCoreTypeUuidV1("uuid"), ProductionCoreTypeUuidV1("uuid"),
      ProductionDescriptorTypeUuidV1(
          persisted.columns[0].value_descriptor),
      ProductionDescriptorTypeUuidV1(
          persisted.columns[1].value_descriptor),
      ProductionDescriptorTypeUuidV1(
          persisted.columns[2].value_descriptor),
      ProductionDescriptorTypeUuidV1(
          persisted.columns[3].value_descriptor)};
  const std::array<std::string, 6> canonical_types{
      "uuid", "uuid", "uuid", "timestamp_tz", "text", "real64"};
  const std::array<std::string, 6> bound_name_uuids{
      persisted.descriptor_uuid.canonical,
      persisted.schema_uuid.canonical,
      persisted.columns[0].column_uuid.canonical,
      persisted.columns[1].column_uuid.canonical,
      persisted.columns[2].column_uuid.canonical,
      persisted.columns[3].column_uuid.canonical};
  const auto descriptor_for = [&](const std::uint32_t descriptor_id) {
    return std::ranges::find_if(
        context.descriptors, [&](const auto& descriptor) {
          return descriptor.descriptor_id == descriptor_id;
        });
  };
  const auto expression_for = [&](const std::uint32_t expression_id) {
    return std::ranges::find_if(
        context.expressions, [&](const auto& expression) {
          return expression.expression_id == expression_id;
        });
  };
  std::vector<const sbsql::NativeOutputBindingInput*> outputs;
  for (const auto& output : context.outputs) {
    if (output.relation_id == relation_id) outputs.push_back(&output);
  }
  std::ranges::sort(outputs, {},
                    &sbsql::NativeOutputBindingInput::ordinal);
  if (outputs.size() != names.size()) return false;
  std::unordered_set<std::uint32_t> public_expression_ids;
  std::unordered_set<std::uint32_t> public_descriptor_ids;
  for (std::size_t ordinal = 0; ordinal < names.size(); ++ordinal) {
    const auto& column = source->columns[ordinal];
    const auto* output = outputs[ordinal];
    const auto descriptor = descriptor_for(column.descriptor_id);
    const auto expression = expression_for(output->expression_id);
    const auto expected_timezone =
        ordinal == 3 ? std::optional<std::string>("UTC") : std::nullopt;
    if (column.ordinal != ordinal ||
        column.canonical_name_key != names[ordinal] ||
        column.column_uuid != bound_name_uuids[ordinal] ||
        descriptor == context.descriptors.end() ||
        descriptor->descriptor_uuid != descriptor_uuids[ordinal] ||
        descriptor->type_uuid != type_uuids[ordinal] ||
        descriptor->canonical_type_name != canonical_types[ordinal] ||
        descriptor->nullability != sbsql::BoundNullability::kNonNull ||
        descriptor->collation_uuid.has_value() ||
        descriptor->timezone_profile_id != expected_timezone ||
        !public_descriptor_ids.insert(column.descriptor_id).second ||
        !output->visible || output->ordinal != ordinal ||
        output->output_name_utf8 != names[ordinal] ||
        output->descriptor_id != column.descriptor_id ||
        expression == context.expressions.end() ||
        expression->descriptor_id != column.descriptor_id ||
        expression->bound_name_uuid != bound_name_uuids[ordinal] ||
        !public_expression_ids.insert(output->expression_id).second) {
      return false;
    }
  }
  const auto row_descriptor_id = source->columns[0].descriptor_id;
  const auto timestamp_descriptor_id = source->columns[3].descriptor_id;
  for (const auto& descriptor : context.descriptors) {
    const auto expected_timezone =
        descriptor.descriptor_id == timestamp_descriptor_id
            ? std::optional<std::string>("UTC")
            : std::nullopt;
    if (descriptor.timezone_profile_id != expected_timezone) return false;
  }
  const auto boolean_type_uuid = ProductionCoreTypeUuidV1("boolean");
  const auto boolean_descriptor = std::ranges::find_if(
      context.descriptors, [&](const auto& descriptor) {
        return descriptor.descriptor_uuid == boolean_type_uuid &&
               descriptor.type_uuid == boolean_type_uuid &&
               descriptor.canonical_type_name == "boolean";
      });
  if (boolean_descriptor == context.descriptors.end() ||
      boolean_descriptor->nullability != sbsql::BoundNullability::kNonNull ||
      boolean_descriptor->collation_uuid.has_value() ||
      boolean_descriptor->timezone_profile_id.has_value() ||
      public_descriptor_ids.contains(boolean_descriptor->descriptor_id)) {
    return false;
  }
  std::size_t alias_count = 0;
  std::size_t endpoint_count = 0;
  std::size_t root_count = 0;
  for (const auto& expression : context.expressions) {
    if (public_expression_ids.contains(expression.expression_id)) continue;
    if (expression.descriptor_id == row_descriptor_id) {
      if (expression.bound_name_uuid != persisted.relation_uuid.canonical) {
        return false;
      }
      ++alias_count;
    } else if (expression.descriptor_id == timestamp_descriptor_id) {
      if (expression.bound_name_uuid.has_value()) return false;
      ++endpoint_count;
    } else if (expression.descriptor_id == boolean_descriptor->descriptor_id) {
      if (expression.bound_name_uuid.has_value()) return false;
      ++root_count;
    }
  }
  return alias_count == 1 && endpoint_count == 2 && root_count == 1;
}

bool ProductionTimeSeriesPublicProjectionBindingProofV1(
    const sbsql::NativeRelationalBindingContext& exact,
    const api::MgaRelationStorageDescriptor& persisted,
    const std::uint32_t relation_id) {
  bool passed = Require(
      ProductionTimeSeriesPublicProjectionBindingExactV1(
          exact, persisted, relation_id),
      "exact time-series public projection binding was refused");
  std::size_t refusal_count = 0;
  const auto require_refusal =
      [&](sbsql::NativeRelationalBindingContext candidate,
          const std::string_view case_id) {
        ++refusal_count;
        return Require(
            !ProductionTimeSeriesPublicProjectionBindingExactV1(
                candidate, persisted, relation_id),
            "time-series public projection mutation was admitted:" +
                std::string(case_id));
      };
  const auto source_for = [&](auto* context) {
    return std::ranges::find_if(
        context->catalog_relations, [&](const auto& source) {
          return source.resolved_object_type == "time_series" &&
                 source.object_uuid == persisted.relation_uuid.canonical;
        });
  };
  const auto output_for = [&](auto* context, const std::uint32_t ordinal) {
    return std::ranges::find_if(context->outputs, [&](const auto& output) {
      return output.relation_id == relation_id && output.ordinal == ordinal;
    });
  };
  const auto descriptor_for = [&](auto* context,
                                  const std::uint32_t descriptor_id) {
    return std::ranges::find_if(
        context->descriptors, [&](const auto& descriptor) {
          return descriptor.descriptor_id == descriptor_id;
        });
  };
  const auto expression_for = [&](auto* context,
                                  const std::uint32_t expression_id) {
    return std::ranges::find_if(
        context->expressions, [&](const auto& expression) {
          return expression.expression_id == expression_id;
        });
  };
  const auto operation_for_descriptor =
      [&](auto* context, const std::uint32_t descriptor_id,
          const std::unordered_set<std::uint32_t>& public_ids,
          const bool require_bound_name) {
        return std::ranges::find_if(
            context->expressions, [&](const auto& expression) {
              return expression.descriptor_id == descriptor_id &&
                     !public_ids.contains(expression.expression_id) &&
                     (expression.bound_name_uuid.has_value() ==
                      require_bound_name);
            });
      };

  auto missing = exact;
  source_for(&missing)->columns.pop_back();
  passed &= require_refusal(std::move(missing), "missing");

  auto extra = exact;
  auto extra_column = source_for(&extra)->columns.back();
  extra_column.ordinal = 6;
  source_for(&extra)->columns.push_back(std::move(extra_column));
  passed &= require_refusal(std::move(extra), "extra");

  auto reordered = exact;
  std::swap(source_for(&reordered)->columns[0],
            source_for(&reordered)->columns[1]);
  passed &= require_refusal(std::move(reordered), "reordered");

  auto duplicated = exact;
  source_for(&duplicated)->columns[5].column_uuid =
      source_for(&duplicated)->columns[4].column_uuid;
  passed &= require_refusal(std::move(duplicated), "duplicated");

  auto wrong_name = exact;
  source_for(&wrong_name)->columns[2].canonical_name_key = "wrong_metric";
  output_for(&wrong_name, 2)->output_name_utf8 = "wrong_metric";
  passed &= require_refusal(std::move(wrong_name), "wrong_name");

  auto wrong_type = exact;
  auto wrong_type_descriptor = descriptor_for(
      &wrong_type, source_for(&wrong_type)->columns[4].descriptor_id);
  wrong_type_descriptor->type_uuid = ProductionCoreTypeUuidV1("uuid");
  wrong_type_descriptor->canonical_type_name = "uuid";
  passed &= require_refusal(std::move(wrong_type), "wrong_type");

  auto nullable = exact;
  descriptor_for(&nullable, source_for(&nullable)->columns[5].descriptor_id)
      ->nullability = sbsql::BoundNullability::kNullable;
  passed &= require_refusal(std::move(nullable), "nullable");

  auto wrong_row_descriptor = exact;
  descriptor_for(&wrong_row_descriptor,
                 source_for(&wrong_row_descriptor)->columns[0].descriptor_id)
      ->descriptor_uuid = persisted.columns[0]
                              .value_descriptor.descriptor_uuid.canonical;
  passed &= require_refusal(std::move(wrong_row_descriptor),
                            "wrong_row_descriptor_identity");

  auto wrong_row_bound_name = exact;
  expression_for(&wrong_row_bound_name,
                 output_for(&wrong_row_bound_name, 0)->expression_id)
      ->bound_name_uuid = persisted.relation_uuid.canonical;
  passed &= require_refusal(std::move(wrong_row_bound_name),
                            "wrong_row_bound_name_identity");

  auto wrong_series_descriptor = exact;
  descriptor_for(
      &wrong_series_descriptor,
      source_for(&wrong_series_descriptor)->columns[1].descriptor_id)
      ->descriptor_uuid = persisted.descriptor_uuid.canonical;
  passed &= require_refusal(std::move(wrong_series_descriptor),
                            "wrong_series_descriptor_identity");

  auto wrong_series_bound_name = exact;
  expression_for(&wrong_series_bound_name,
                 output_for(&wrong_series_bound_name, 1)->expression_id)
      ->bound_name_uuid = persisted.relation_uuid.canonical;
  passed &= require_refusal(std::move(wrong_series_bound_name),
                            "wrong_series_bound_name_identity");

  auto wrong_lineage = exact;
  auto* wrong_lineage_source = &*source_for(&wrong_lineage);
  const auto wrong_lineage_output = output_for(&wrong_lineage, 2);
  const auto wrong_lineage_descriptor = descriptor_for(
      &wrong_lineage, wrong_lineage_source->columns[2].descriptor_id);
  wrong_lineage_source->columns[2].column_uuid =
      persisted.columns[2].column_uuid.canonical;
  wrong_lineage_descriptor->descriptor_uuid =
      persisted.columns[2].value_descriptor.descriptor_uuid.canonical;
  expression_for(&wrong_lineage, wrong_lineage_output->expression_id)
      ->bound_name_uuid = persisted.columns[2].column_uuid.canonical;
  passed &= require_refusal(std::move(wrong_lineage),
                            "wrong_persistent_column_descriptor_lineage");

  auto missing_timezone = exact;
  descriptor_for(&missing_timezone,
                 source_for(&missing_timezone)->columns[3].descriptor_id)
      ->timezone_profile_id.reset();
  passed &= require_refusal(std::move(missing_timezone),
                            "missing_timestamp_UTC");

  auto non_utc_timezone = exact;
  descriptor_for(&non_utc_timezone,
                 source_for(&non_utc_timezone)->columns[3].descriptor_id)
      ->timezone_profile_id = "America/Toronto";
  passed &= require_refusal(std::move(non_utc_timezone),
                            "non_UTC_timestamp_timezone");

  auto misplaced_timezone = exact;
  descriptor_for(&misplaced_timezone,
                 source_for(&misplaced_timezone)->columns[2].descriptor_id)
      ->timezone_profile_id = "UTC";
  passed &= require_refusal(std::move(misplaced_timezone),
                            "misplaced_time_series_UTC");

  auto cross_family_timezone = exact;
  const auto key_value_source = std::ranges::find_if(
      cross_family_timezone.catalog_relations, [](const auto& source) {
        return source.resolved_object_type == "key_value";
      });
  descriptor_for(&cross_family_timezone,
                 key_value_source->columns.front().descriptor_id)
      ->timezone_profile_id = "UTC";
  passed &= require_refusal(std::move(cross_family_timezone),
                            "cross_family_UTC");

  auto alias_on_non_row = exact;
  const auto* alias_source = &*source_for(&alias_on_non_row);
  const std::unordered_set<std::uint32_t> alias_public_ids{
      output_for(&alias_on_non_row, 0)->expression_id,
      output_for(&alias_on_non_row, 1)->expression_id,
      output_for(&alias_on_non_row, 2)->expression_id,
      output_for(&alias_on_non_row, 3)->expression_id,
      output_for(&alias_on_non_row, 4)->expression_id,
      output_for(&alias_on_non_row, 5)->expression_id};
  operation_for_descriptor(&alias_on_non_row,
                           alias_source->columns[0].descriptor_id,
                           alias_public_ids, true)
      ->descriptor_id = alias_source->columns[2].descriptor_id;
  passed &= require_refusal(std::move(alias_on_non_row),
                            "alias_bound_to_non_row_descriptor");

  auto endpoint_on_non_timestamp = exact;
  const auto* endpoint_source = &*source_for(&endpoint_on_non_timestamp);
  const std::unordered_set<std::uint32_t> endpoint_public_ids{
      output_for(&endpoint_on_non_timestamp, 0)->expression_id,
      output_for(&endpoint_on_non_timestamp, 1)->expression_id,
      output_for(&endpoint_on_non_timestamp, 2)->expression_id,
      output_for(&endpoint_on_non_timestamp, 3)->expression_id,
      output_for(&endpoint_on_non_timestamp, 4)->expression_id,
      output_for(&endpoint_on_non_timestamp, 5)->expression_id};
  operation_for_descriptor(&endpoint_on_non_timestamp,
                           endpoint_source->columns[3].descriptor_id,
                           endpoint_public_ids, false)
      ->descriptor_id = endpoint_source->columns[2].descriptor_id;
  passed &= require_refusal(std::move(endpoint_on_non_timestamp),
                            "endpoint_bound_to_non_timestamp_descriptor");

  auto root_on_non_boolean = exact;
  const auto* root_source = &*source_for(&root_on_non_boolean);
  const std::unordered_set<std::uint32_t> root_public_ids{
      output_for(&root_on_non_boolean, 0)->expression_id,
      output_for(&root_on_non_boolean, 1)->expression_id,
      output_for(&root_on_non_boolean, 2)->expression_id,
      output_for(&root_on_non_boolean, 3)->expression_id,
      output_for(&root_on_non_boolean, 4)->expression_id,
      output_for(&root_on_non_boolean, 5)->expression_id};
  const auto boolean_type_uuid = ProductionCoreTypeUuidV1("boolean");
  const auto boolean_descriptor = std::ranges::find_if(
      root_on_non_boolean.descriptors, [&](const auto& descriptor) {
        return descriptor.descriptor_uuid == boolean_type_uuid &&
               descriptor.type_uuid == boolean_type_uuid;
      });
  operation_for_descriptor(&root_on_non_boolean,
                           boolean_descriptor->descriptor_id,
                           root_public_ids, false)
      ->descriptor_id = root_source->columns[2].descriptor_id;
  passed &= require_refusal(std::move(root_on_non_boolean),
                            "TIME_RANGE_bound_to_non_boolean_descriptor");

  passed &= Require(refusal_count == 19,
                    "time-series public projection refusal matrix is not 19");
  return passed;
}

void AppendProductionLittleEndianU64V1(std::vector<std::uint8_t>* output,
                                       const std::uint64_t value) {
  for (unsigned byte = 0; byte < 8; ++byte) {
    output->push_back(
        static_cast<std::uint8_t>((value >> (byte * 8)) & 0xffu));
  }
}

void SetProductionEngineOperandValueV1(sblr::SblrOperand* operand,
                                       const std::string_view value) {
  operand->value.clear();
  operand->value_kind = sblr::SblrValueKind::literal_typed;
  operand->value_body.assign(16, 0);
  operand->value_body.front() = 0x73;
  AppendProductionLittleEndianU64V1(&operand->value_body, value.size());
  operand->value_body.insert(operand->value_body.end(), value.begin(),
                             value.end());
}

std::string EncodeProductionHexV1(const std::string_view value) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string encoded;
  encoded.reserve(value.size() * 2);
  for (const unsigned char ch : value) {
    encoded.push_back(kHex[ch >> 4]);
    encoded.push_back(kHex[ch & 0x0f]);
  }
  return encoded;
}

std::vector<std::string> SplitProductionFieldsV1(
    const std::string_view encoded) {
  std::vector<std::string> fields;
  std::size_t start = 0;
  while (start <= encoded.size()) {
    const auto separator = encoded.find('|', start);
    fields.emplace_back(encoded.substr(
        start, separator == std::string_view::npos ? encoded.size() - start
                                                   : separator - start));
    if (separator == std::string_view::npos) break;
    start = separator + 1;
  }
  return fields;
}

std::string JoinProductionFieldsV1(
    const std::vector<std::string>& fields) {
  std::string encoded;
  for (std::size_t index = 0; index < fields.size(); ++index) {
    if (index != 0) encoded.push_back('|');
    encoded += fields[index];
  }
  return encoded;
}

std::vector<std::string> SplitProductionHandlesV1(
    const std::string_view encoded) {
  if (encoded == "-") return {};
  std::vector<std::string> handles;
  std::size_t start = 0;
  while (start <= encoded.size()) {
    const auto separator = encoded.find(',', start);
    handles.emplace_back(encoded.substr(
        start, separator == std::string_view::npos ? encoded.size() - start
                                                   : separator - start));
    if (separator == std::string_view::npos) break;
    start = separator + 1;
  }
  return handles;
}

std::string JoinProductionHandlesV1(
    const std::vector<std::string>& handles) {
  if (handles.empty()) return "-";
  std::string encoded;
  for (std::size_t index = 0; index < handles.size(); ++index) {
    if (index != 0) encoded.push_back(',');
    encoded += handles[index];
  }
  return encoded;
}

sbsql::SblrOperand* FindProductionExpressionByOperatorV1(
    sbsql::SblrEnvelope* envelope, const std::string_view operator_name) {
  if (envelope == nullptr) return nullptr;
  const auto encoded_operator = EncodeProductionHexV1(operator_name);
  sbsql::SblrOperand* matched = nullptr;
  for (auto& operand : envelope->operands) {
    if (operand.type != "relational_expression_v1") continue;
    const auto fields = SplitProductionFieldsV1(operand.value);
    if (fields.size() != 8 || fields[6] != encoded_operator) continue;
    if (matched != nullptr) return nullptr;
    matched = &operand;
  }
  return matched;
}

sbsql::SblrOperand* FindProductionBindingContainingV1(
    sbsql::SblrEnvelope* envelope, const std::string_view expression_id) {
  if (envelope == nullptr) return nullptr;
  for (auto& operand : envelope->operands) {
    if (operand.type != "relational_node_binding_v1") continue;
    const auto fields = SplitProductionFieldsV1(operand.value);
    if (fields.size() != 5) continue;
    const auto handles = SplitProductionHandlesV1(fields[1]);
    if (std::ranges::find(handles, expression_id) != handles.end()) {
      return &operand;
    }
  }
  return nullptr;
}

sblr::SblrOperationEnvelope ProductionEngineEnvelopeV1(
    const sbsql::SblrEnvelope& parser_envelope,
    const ProductionFixtureV1& fixture) {
  const auto operation_id = parser_envelope.engine_api_operation_id.empty()
                                ? parser_envelope.operation_id
                                : parser_envelope.engine_api_operation_id;
  const auto* operation = sblr::LookupSblrOperation(operation_id);
  auto engine = sblr::MakeSblrEnvelope(
      operation_id, parser_envelope.sblr_opcode,
      parser_envelope.trace_key.empty()
          ? "rcp080.production.query-execute.v1"
          : parser_envelope.trace_key);
  engine.opcode_code = operation == nullptr ? 0 : operation->code;
  engine.parser_package_uuid =
      ProductionUuidV1(platform::UuidKind::object, fixture.salt + 310);
  engine.registry_snapshot_uuid =
      ProductionUuidV1(platform::UuidKind::object, fixture.salt + 311);
  engine.result_shape = parser_envelope.result_shape_key;
  engine.diagnostic_shape = "diagnostic.canonical_message_vector";
  engine.requires_security_context = true;
  engine.requires_transaction_context = true;
  engine.contains_sql_text = false;
  engine.parser_resolved_names_to_uuids = true;
  for (const auto& parser_operand : parser_envelope.operands) {
    sblr::SblrOperand operand;
    operand.type = parser_operand.type;
    operand.name = parser_operand.name;
    if (!operand.name.empty() &&
        std::ranges::all_of(operand.name, [](const unsigned char ch) {
          return ch >= '0' && ch <= '9';
        })) {
      operand.name = "slot_" + operand.name;
    }
    operand.ordinal = static_cast<std::uint32_t>(engine.operands.size() + 1);
    SetProductionEngineOperandValueV1(&operand, parser_operand.value);
    engine.operands.push_back(std::move(operand));
  }
  return engine;
}

bool HasProductionEvidenceV1(const sblr::SblrDispatchResult& result,
                             const std::string_view kind,
                             const std::string_view value) {
  return std::ranges::any_of(result.api_result.evidence,
                             [&](const auto& evidence) {
                               return evidence.evidence_kind == kind &&
                                      evidence.evidence_id == value;
                             });
}

std::string ProductionMessagesV1(const sbsql::MessageVectorSet& messages) {
  std::string result;
  for (const auto& diagnostic : messages.diagnostics) {
    if (!result.empty()) result += ',';
    result += diagnostic.code + ":" + diagnostic.message;
  }
  return result;
}

std::string ProductionDagInventoryV1(
    const sbsql::BoundNativeRelationalDocument& dag) {
  std::string result;
  for (const auto& node : dag.relations) {
    if (!result.empty()) result += ';';
    result += "node=" + std::to_string(node.relation_id) + ':' +
              node.semantic_variant_id + ":in=";
    for (const auto relation_id : node.input_relation_ids) {
      result += std::to_string(relation_id) + ',';
    }
    result += ":out=";
    for (const auto expression_id : node.output_expression_ids) {
      result += std::to_string(expression_id) + ',';
    }
    result += ":bound=";
    for (const auto expression_id : node.bound_expression_ids) {
      result += std::to_string(expression_id) + ',';
    }
  }
  for (const auto& expression : dag.expressions) {
    result += ";expr=" + std::to_string(expression.expression_id) + ':' +
              expression.canonical_operator_name.value_or("-") + ":child=";
    for (const auto child_id : expression.child_expression_ids) {
      result += std::to_string(child_id) + ',';
    }
  }
  return result;
}

api::TypedRelationalDag ProductionStandaloneGraphClosureDagV1(
    const api::EngineRequestContext& context,
    const api::MgaRelationStorageDescriptor& graph, const bool expand) {
  api::TypedRelationalDag dag;
  dag.wire_version = 2;
  dag.bound_sblr_tree_uuid =
      ProductionUuidV1(platform::UuidKind::object, 881);
  dag.bound_catalog_epoch_uuid = context.catalog_epoch_uuid.canonical;
  dag.bound_security_context_uuid =
      context.authorization_context.authority_uuid.canonical;
  dag.statement_uuid = context.statement_uuid.canonical;
  dag.owning_transaction_uuid = context.transaction_uuid.canonical;
  dag.statement_snapshot_uuid = context.statement_snapshot_uuid.canonical;
  dag.statement_metadata_snapshot_uuid =
      context.statement_metadata_snapshot_uuid.canonical;
  dag.local_transaction_id = context.local_transaction_id;
  dag.snapshot_visible_through_local_transaction_id =
      context.snapshot_visible_through_local_transaction_id;
  dag.root_node_id = 1;
  for (std::size_t ordinal = 0; ordinal < graph.columns.size(); ++ordinal) {
    const auto& column = graph.columns[ordinal];
    const auto descriptor_id = static_cast<std::uint32_t>(ordinal + 1);
    api::RelationalTypeDescriptor descriptor;
    descriptor.descriptor_id = descriptor_id;
    descriptor.descriptor_uuid =
        column.value_descriptor.descriptor_uuid.canonical;
    descriptor.type_uuid =
        ProductionDescriptorTypeUuidV1(column.value_descriptor);
    descriptor.nullability =
        column.nullable ? api::RelationalNullability::kNullable
                        : api::RelationalNullability::kNonNull;
    dag.descriptors.push_back(std::move(descriptor));

    api::RelationalExpressionRecord output_expression;
    output_expression.expression_id = descriptor_id;
    output_expression.expression_kind =
        api::RelationalExpressionKind::kIdentifier;
    output_expression.result_descriptor_id = descriptor_id;
    output_expression.bound_name_uuid = column.column_uuid.canonical;
    dag.expressions.push_back(std::move(output_expression));
    dag.outputs.push_back(
        {descriptor_id, 1, descriptor_id, column.canonical_name_key,
         descriptor_id, true, static_cast<std::uint32_t>(ordinal)});
  }

  api::RelationalExpressionRecord alias;
  alias.expression_id = 10;
  alias.expression_kind = api::RelationalExpressionKind::kIdentifier;
  alias.result_descriptor_id = 1;
  alias.bound_name_uuid = graph.relation_uuid.canonical;
  dag.expressions.push_back(std::move(alias));
  const auto add_literal = [&](const std::uint32_t expression_id,
                               const std::uint32_t descriptor_id,
                               const api::RelationalLiteralKind kind,
                               std::string value) {
    api::RelationalExpressionRecord literal;
    literal.expression_id = expression_id;
    literal.expression_kind = api::RelationalExpressionKind::kLiteral;
    literal.result_descriptor_id = descriptor_id;
    literal.literal_kind = kind;
    literal.literal_or_parameter_ref = std::move(value);
    dag.expressions.push_back(std::move(literal));
  };

  api::RelationalExpressionRecord operation;
  operation.expression_id = 20;
  operation.expression_kind = api::RelationalExpressionKind::kFunctionCall;
  operation.result_descriptor_id = 1;
  if (expand) {
    add_literal(11, 7, api::RelationalLiteralKind::kString, "OUTGOING");
    add_literal(12, 8, api::RelationalLiteralKind::kNumeric, "1");
    add_literal(13, 8, api::RelationalLiteralKind::kNumeric, "2");
    add_literal(14, 9, api::RelationalLiteralKind::kString, "visited_set");
    operation.operator_name = "GRAPH_EXPAND";
    operation.child_expression_ids = {10, 11, 12, 13, 14};
  } else {
    add_literal(11, 4, api::RelationalLiteralKind::kString, "vertex(*)");
    operation.operator_name = "GRAPH_MATCH";
    operation.child_expression_ids = {10, 11};
  }
  dag.expressions.push_back(std::move(operation));

  api::RelationalDagNode node;
  node.node_id = 1;
  node.node_kind = api::RelationalDagNodeKind::kScan;
  for (std::uint32_t descriptor_id = 1;
       descriptor_id <= graph.columns.size(); ++descriptor_id) {
    node.output_descriptor_ids.push_back(descriptor_id);
    node.bound_expression_ids.push_back(descriptor_id);
  }
  node.bound_expression_ids.push_back(20);
  node.bound_expression_ids.push_back(10);
  node.bound_expression_ids.push_back(11);
  if (expand) {
    node.bound_expression_ids.push_back(12);
    node.bound_expression_ids.push_back(13);
    node.bound_expression_ids.push_back(14);
  }
  node.required_object_uuids = {graph.relation_uuid.canonical};
  node.semantic_variant_id =
      expand ? "SBLR_MODEL_EXPAND_V1" : "SBLR_MODEL_SOURCE_V1";
  dag.nodes.push_back(std::move(node));
  return dag;
}

bool ProductionStandaloneGraphClosureWidthProofV1(
    const api::EngineRequestContext& context,
    const api::MgaRelationStorageDescriptor& graph) {
  const auto match = ProductionStandaloneGraphClosureDagV1(
      context, graph, false);
  const auto expand = ProductionStandaloneGraphClosureDagV1(
      context, graph, true);
  const auto match_accepted = api::ValidateTypedRelationalDag(match);
  const auto expand_accepted = api::ValidateTypedRelationalDag(expand);
  bool passed = Require(match_accepted.accepted && expand_accepted.accepted,
                        "exact standalone graph closure widths were refused");

  const auto require_refusal = [&](api::TypedRelationalDag candidate,
                                   const std::string& case_id,
                                   const std::string_view expected_field) {
    const auto result = api::ValidateTypedRelationalDag(candidate);
    const auto observed =
        result.issues.empty()
            ? std::string{"<none>"}
            : result.issues.front().diagnostic_id + ":" +
                  result.issues.front().field_id;
    return Require(
        !result.accepted && !result.issues.empty() &&
            result.issues.front().diagnostic_id ==
                "SBLR.PLAN_TREE.INVALID_HANDLE" &&
            result.issues.front().field_id == expected_field,
        "standalone graph closure mutation did not refuse exactly: " +
            case_id + ";observed=" + observed);
  };

  auto missing = match;
  missing.nodes.front().bound_expression_ids.pop_back();
  passed &= require_refusal(std::move(missing), "missing_suffix",
                            "graph_model_operation_shape");

  auto extra = match;
  auto extra_expression = extra.expressions[10];
  extra_expression.expression_id = 21;
  extra.expressions.push_back(std::move(extra_expression));
  extra.nodes.front().bound_expression_ids.push_back(21);
  passed &= require_refusal(std::move(extra), "extra_suffix",
                            "graph_model_operation_shape");

  auto duplicate = match;
  duplicate.nodes.front().bound_expression_ids.back() = 10;
  passed &= require_refusal(std::move(duplicate), "duplicate_suffix",
                            "bound_expression_ids");

  auto overlap = match;
  overlap.nodes.front().bound_expression_ids.back() = 1;
  passed &= require_refusal(std::move(overlap), "output_overlap",
                            "bound_expression_ids");

  auto foreign = expand;
  auto foreign_expression = foreign.expressions[10];
  foreign_expression.expression_id = 21;
  foreign.expressions.push_back(std::move(foreign_expression));
  foreign.nodes.front().bound_expression_ids.back() = 21;
  passed &= require_refusal(std::move(foreign), "foreign_suffix",
                            "graph_model_operation_shape");

  auto duplicate_output = match;
  duplicate_output.nodes.front().bound_expression_ids[1] = 1;
  passed &= require_refusal(std::move(duplicate_output),
                            "duplicate_output_prefix",
                            "bound_expression_ids");

  auto reordered_output = match;
  std::swap(reordered_output.nodes.front().bound_expression_ids[0],
            reordered_output.nodes.front().bound_expression_ids[1]);
  passed &= require_refusal(std::move(reordered_output),
                            "reordered_output_prefix", "graph_output_binding");

  auto reordered_suffix = expand;
  std::swap(reordered_suffix.nodes.front().bound_expression_ids[9],
            reordered_suffix.nodes.front().bound_expression_ids[14]);
  const auto reordered_result =
      api::ValidateTypedRelationalDag(reordered_suffix);
  passed &= Require(reordered_result.accepted,
                    "exact graph operation suffix was treated as ordered");
  return passed;
}

api::TypedRelationalDag ProductionKeyValueExactClosureDagV1(
    const api::EngineRequestContext& context,
    const api::MgaRelationStorageDescriptor& document,
    const api::MgaRelationStorageDescriptor& key_value) {
  api::TypedRelationalDag dag;
  dag.wire_version = 2;
  dag.bound_sblr_tree_uuid =
      ProductionUuidV1(platform::UuidKind::object, 941);
  dag.bound_catalog_epoch_uuid = context.catalog_epoch_uuid.canonical;
  dag.bound_security_context_uuid =
      context.authorization_context.authority_uuid.canonical;
  dag.statement_uuid = context.statement_uuid.canonical;
  dag.statement_timestamp = context.statement_timestamp;
  dag.owning_transaction_uuid = context.transaction_uuid.canonical;
  dag.statement_snapshot_uuid = context.statement_snapshot_uuid.canonical;
  dag.statement_metadata_snapshot_uuid =
      context.statement_metadata_snapshot_uuid.canonical;
  dag.local_transaction_id = context.local_transaction_id;
  dag.snapshot_visible_through_local_transaction_id =
      context.snapshot_visible_through_local_transaction_id;
  dag.root_node_id = 3;

  std::vector<const api::MgaRelationColumnStorageDescriptor*> columns{
      &document.columns.front(), &key_value.columns[0],
      &key_value.columns[1], &key_value.columns[2]};
  for (std::size_t ordinal = 0; ordinal < columns.size(); ++ordinal) {
    const auto descriptor_id = static_cast<std::uint32_t>(ordinal + 1);
    api::RelationalTypeDescriptor descriptor;
    descriptor.descriptor_id = descriptor_id;
    descriptor.descriptor_uuid =
        columns[ordinal]->value_descriptor.descriptor_uuid.canonical;
    descriptor.type_uuid =
        ProductionDescriptorTypeUuidV1(columns[ordinal]->value_descriptor);
    descriptor.nullability =
        columns[ordinal]->nullable
            ? api::RelationalNullability::kNullable
            : api::RelationalNullability::kNonNull;
    dag.descriptors.push_back(std::move(descriptor));

    api::RelationalExpressionRecord output;
    output.expression_id = descriptor_id;
    output.expression_kind = api::RelationalExpressionKind::kIdentifier;
    output.result_descriptor_id = descriptor_id;
    output.bound_name_uuid = columns[ordinal]->column_uuid.canonical;
    dag.expressions.push_back(std::move(output));
  }
  dag.descriptors.push_back(
      {5, ProductionUuidV1(platform::UuidKind::object, 942),
       ProductionUuidV1(platform::UuidKind::object, 943),
       api::RelationalNullability::kNonNull});

  api::RelationalExpressionRecord document_alias;
  document_alias.expression_id = 10;
  document_alias.expression_kind = api::RelationalExpressionKind::kIdentifier;
  document_alias.result_descriptor_id = 1;
  document_alias.bound_name_uuid = document.relation_uuid.canonical;
  dag.expressions.push_back(std::move(document_alias));
  api::RelationalExpressionRecord document_root;
  document_root.expression_id = 11;
  document_root.expression_kind =
      api::RelationalExpressionKind::kFunctionCall;
  document_root.child_expression_ids = {10};
  document_root.result_descriptor_id = 1;
  document_root.operator_name = "DOCUMENT_SOURCE";
  dag.expressions.push_back(std::move(document_root));

  api::RelationalExpressionRecord alias;
  alias.expression_id = 20;
  alias.expression_kind = api::RelationalExpressionKind::kIdentifier;
  alias.result_descriptor_id = 3;
  alias.bound_name_uuid = key_value.relation_uuid.canonical;
  dag.expressions.push_back(std::move(alias));
  api::RelationalExpressionRecord root;
  root.expression_id = 21;
  root.expression_kind = api::RelationalExpressionKind::kFunctionCall;
  root.child_expression_ids = {20};
  root.result_descriptor_id = 3;
  root.operator_name = "KV_KEY";
  dag.expressions.push_back(std::move(root));
  api::RelationalExpressionRecord literal;
  literal.expression_id = 22;
  literal.expression_kind = api::RelationalExpressionKind::kLiteral;
  literal.result_descriptor_id = 3;
  literal.literal_kind = api::RelationalLiteralKind::kString;
  literal.literal_or_parameter_ref = "alpha";
  dag.expressions.push_back(std::move(literal));
  api::RelationalExpressionRecord equality;
  equality.expression_id = 23;
  equality.expression_kind = api::RelationalExpressionKind::kBinary;
  equality.child_expression_ids = {21, 22};
  equality.result_descriptor_id = 5;
  equality.operator_name = "=";
  dag.expressions.push_back(std::move(equality));

  std::uint32_t output_id = 1;
  dag.outputs.push_back(
      {output_id++, 1, 1, "document_uuid", 1, true, 0});
  for (std::uint32_t ordinal = 0; ordinal < 3; ++ordinal) {
    const auto expression_id = ordinal + 2;
    dag.outputs.push_back(
        {output_id++, 2, expression_id,
         ordinal == 0 ? "row_uuid" : ordinal == 1 ? "key" : "value",
         expression_id, true, ordinal});
  }
  for (std::uint32_t ordinal = 0; ordinal < 4; ++ordinal) {
    const auto expression_id = ordinal + 1;
    dag.outputs.push_back(
        {output_id++, 3, expression_id,
         ordinal == 0 ? "document_uuid"
         : ordinal == 1 ? "row_uuid"
         : ordinal == 2 ? "key"
                        : "value",
         expression_id, true, ordinal});
  }

  api::RelationalDagNode document_node;
  document_node.node_id = 1;
  document_node.node_kind = api::RelationalDagNodeKind::kScan;
  document_node.output_descriptor_ids = {1};
  document_node.bound_expression_ids = {1, 10, 11};
  document_node.required_object_uuids = {document.relation_uuid.canonical};
  document_node.semantic_variant_id = "SBLR_MODEL_SOURCE_V1";
  dag.nodes.push_back(std::move(document_node));
  api::RelationalDagNode key_value_node;
  key_value_node.node_id = 2;
  key_value_node.node_kind = api::RelationalDagNodeKind::kScan;
  key_value_node.output_descriptor_ids = {2, 3, 4};
  key_value_node.bound_expression_ids = {2, 3, 4, 20, 21, 22, 23};
  key_value_node.required_object_uuids = {key_value.relation_uuid.canonical};
  key_value_node.semantic_variant_id = "SBLR_MODEL_SOURCE_V1";
  dag.nodes.push_back(std::move(key_value_node));
  api::RelationalDagNode join;
  join.node_id = 3;
  join.node_kind = api::RelationalDagNodeKind::kJoin;
  join.input_node_ids = {1, 2};
  join.output_descriptor_ids = {1, 2, 3, 4};
  join.semantic_variant_id = "join.cross.v1";
  dag.nodes.push_back(std::move(join));
  return dag;
}

bool ProductionKeyValueExactClosureProofV1(
    const api::EngineRequestContext& context,
    const api::MgaRelationStorageDescriptor& document,
    const api::MgaRelationStorageDescriptor& key_value) {
  const auto exact =
      ProductionKeyValueExactClosureDagV1(context, document, key_value);
  const auto exact_result = api::ValidateTypedRelationalDag(exact);
  bool passed = Require(
      exact_result.accepted,
      exact_result.issues.empty()
          ? "exact key/value owned closure was refused without diagnostic"
          : "exact key/value owned closure was refused:" +
                exact_result.issues.front().diagnostic_id + ":" +
                exact_result.issues.front().field_id);

  api::CanonicalRelationalPlanningScope scope;
  scope.catalog_epoch_uuid = exact.bound_catalog_epoch_uuid;
  scope.security_context_uuid = exact.bound_security_context_uuid;
  scope.statement_uuid = exact.statement_uuid;
  scope.statement_timestamp = exact.statement_timestamp;
  scope.owning_transaction_uuid = exact.owning_transaction_uuid;
  scope.statement_snapshot_uuid = exact.statement_snapshot_uuid;
  scope.statement_metadata_snapshot_uuid =
      exact.statement_metadata_snapshot_uuid;
  scope.local_transaction_id = exact.local_transaction_id;
  scope.snapshot_visible_through_local_transaction_id =
      exact.snapshot_visible_through_local_transaction_id;
  scope.metadata_snapshot_engine_owned = true;
  scope.authorization_context_engine_owned = true;
  const auto bridge =
      api::PopulateCanonicalLogicalGraphFromAdmittedTypedRelationalDag(
          exact, scope);
  passed &= Require(
      bridge.accepted && !bridge.data_access_allowed &&
          bridge.logical_graph.nodes.size() == 3 &&
          bridge.logical_graph.nodes[1].model_family_identity ==
              planner::CanonicalLogicalModelFamilyIdentity::kKeyValue,
      bridge.issues.empty()
          ? "exact key/value owned closure did not populate canonical graph"
          : "key/value bridge:" + bridge.issues.front().diagnostic_id + ":" +
                bridge.issues.front().field_id);

  std::size_t refusal_count = 0;
  const auto require_refusal = [&](api::TypedRelationalDag candidate,
                                   const std::string_view case_id) {
    const auto result = api::ValidateTypedRelationalDag(candidate);
    ++refusal_count;
    return Require(
        !result.accepted && !result.issues.empty(),
        "key/value owned-closure mutation was admitted:" +
            std::string(case_id));
  };
  const auto expression = [](api::TypedRelationalDag* dag,
                             const std::uint32_t id) {
    return std::ranges::find_if(dag->expressions, [&](const auto& candidate) {
      return candidate.expression_id == id;
    });
  };
  const auto add_foreign = [&](api::TypedRelationalDag* dag,
                               const std::uint32_t id) {
    auto foreign = dag->expressions.front();
    foreign.expression_id = id;
    foreign.bound_name_uuid = document.columns.front().column_uuid.canonical;
    dag->expressions.push_back(std::move(foreign));
  };

  auto plus_two = exact;
  plus_two.nodes[1].bound_expression_ids.resize(5);
  passed &= require_refusal(std::move(plus_two), "output_plus_2");
  auto plus_three = exact;
  plus_three.nodes[1].bound_expression_ids.resize(6);
  passed &= require_refusal(std::move(plus_three), "output_plus_3");
  auto plus_five = exact;
  add_foreign(&plus_five, 90);
  plus_five.nodes[1].bound_expression_ids.push_back(90);
  passed &= require_refusal(std::move(plus_five), "output_plus_5");
  auto arbitrary_plus_four = exact;
  add_foreign(&arbitrary_plus_four, 90);
  arbitrary_plus_four.nodes[1].bound_expression_ids.back() = 90;
  passed &= require_refusal(std::move(arbitrary_plus_four),
                            "generic_output_plus_4");
  auto reordered_prefix = exact;
  std::swap(reordered_prefix.nodes[1].bound_expression_ids[0],
            reordered_prefix.nodes[1].bound_expression_ids[1]);
  passed &= require_refusal(std::move(reordered_prefix),
                            "reordered_output_prefix");
  auto duplicate_output = exact;
  duplicate_output.nodes[1].bound_expression_ids[1] = 2;
  passed &= require_refusal(std::move(duplicate_output),
                            "duplicate_output_expression");
  auto overlapping_output = exact;
  overlapping_output.nodes[1].bound_expression_ids[3] = 2;
  passed &= require_refusal(std::move(overlapping_output),
                            "output_suffix_overlap");
  auto foreign_output = exact;
  add_foreign(&foreign_output, 90);
  foreign_output.nodes[1].bound_expression_ids[0] = 90;
  passed &= require_refusal(std::move(foreign_output),
                            "foreign_output_expression");

  for (const auto [id, case_id] :
       std::array<std::pair<std::uint32_t, std::string_view>, 4>{
           {{23, "missing_equality"}, {21, "missing_KV_KEY"},
            {20, "missing_alias"}, {22, "missing_key_literal"}}}) {
    auto missing = exact;
    auto& bound = missing.nodes[1].bound_expression_ids;
    bound.erase(std::ranges::find(bound, id));
    passed &= require_refusal(std::move(missing), case_id);
  }
  auto duplicate_suffix = exact;
  duplicate_suffix.nodes[1].bound_expression_ids.back() = 20;
  passed &= require_refusal(std::move(duplicate_suffix),
                            "duplicate_suffix_id");
  auto foreign_suffix = exact;
  add_foreign(&foreign_suffix, 90);
  foreign_suffix.nodes[1].bound_expression_ids.back() = 90;
  passed &= require_refusal(std::move(foreign_suffix), "foreign_suffix_id");
  auto unattached = exact;
  unattached.nodes[1].bound_expression_ids.erase(
      unattached.nodes[1].bound_expression_ids.begin() + 3,
      unattached.nodes[1].bound_expression_ids.end());
  passed &= require_refusal(std::move(unattached), "unattached_closure");
  auto extra_valid = exact;
  add_foreign(&extra_valid, 90);
  extra_valid.nodes[1].bound_expression_ids.push_back(90);
  passed &= require_refusal(std::move(extra_valid), "extra_valid_expression");

  auto wrong_equality = exact;
  expression(&wrong_equality, 23)->operator_name = "<>";
  passed &= require_refusal(std::move(wrong_equality),
                            "wrong_equality_operator");
  auto wrong_root = exact;
  expression(&wrong_root, 21)->operator_name = "KV_PREFIX";
  passed &= require_refusal(std::move(wrong_root), "wrong_root_operator");
  auto wrong_kind = exact;
  expression(&wrong_kind, 21)->expression_kind =
      api::RelationalExpressionKind::kBinary;
  passed &= require_refusal(std::move(wrong_kind), "wrong_root_kind");
  auto wrong_arity = exact;
  expression(&wrong_arity, 21)->child_expression_ids.push_back(22);
  passed &= require_refusal(std::move(wrong_arity), "wrong_root_arity");
  auto wrong_ordinal = exact;
  std::swap(expression(&wrong_ordinal, 23)->child_expression_ids[0],
            expression(&wrong_ordinal, 23)->child_expression_ids[1]);
  passed &= require_refusal(std::move(wrong_ordinal),
                            "wrong_equality_child_ordinal");
  auto wrong_literal_kind = exact;
  expression(&wrong_literal_kind, 22)->literal_kind =
      api::RelationalLiteralKind::kNumeric;
  passed &= require_refusal(std::move(wrong_literal_kind),
                            "wrong_literal_kind");
  auto wrong_literal_value = exact;
  expression(&wrong_literal_value, 22)->literal_or_parameter_ref = "";
  passed &= require_refusal(std::move(wrong_literal_value),
                            "empty_literal_value");
  auto wrong_object = exact;
  wrong_object.nodes[1].required_object_uuids.front() =
      document.relation_uuid.canonical;
  passed &= require_refusal(std::move(wrong_object), "wrong_object_identity");
  auto missing_object = exact;
  missing_object.nodes[1].required_object_uuids.clear();
  passed &= require_refusal(std::move(missing_object),
                            "missing_object_identity");
  auto wrong_descriptor = exact;
  expression(&wrong_descriptor, 20)->result_descriptor_id = 4;
  passed &= require_refusal(std::move(wrong_descriptor),
                            "wrong_descriptor_lineage");
  auto broken_reachability = exact;
  expression(&broken_reachability, 23)->child_expression_ids[1] = 4;
  passed &= require_refusal(std::move(broken_reachability),
                            "broken_reachability");
  auto cross_leg = exact;
  cross_leg.nodes[0].bound_expression_ids.push_back(22);
  passed &= require_refusal(std::move(cross_leg), "cross_leg_ownership");

  api::RelationalDagLimits constrained;
  constrained.maximum_records = exact.descriptors.size() +
                                exact.expressions.size() +
                                exact.outputs.size() - 1;
  const auto resource = api::ValidateTypedRelationalDag(exact, constrained);
  passed &= Require(!resource.accepted && !resource.issues.empty() &&
                        resource.issues.front().diagnostic_id ==
                            "SBLR.PLAN_TREE.RESOURCE_LIMIT",
                    "key/value resource overflow was admitted");
  ++refusal_count;
  passed &= Require(refusal_count == 29,
                    "key/value refusal matrix count drifted");
  return passed;
}

api::TypedRelationalDag ProductionTimeSeriesExactClosureDagV1(
    const api::EngineRequestContext& context,
    const api::MgaRelationStorageDescriptor& document,
    const api::MgaRelationStorageDescriptor& time_series) {
  api::TypedRelationalDag dag;
  dag.wire_version = 2;
  dag.bound_sblr_tree_uuid =
      ProductionUuidV1(platform::UuidKind::object, 951);
  dag.bound_catalog_epoch_uuid = context.catalog_epoch_uuid.canonical;
  dag.bound_security_context_uuid =
      context.authorization_context.authority_uuid.canonical;
  dag.statement_uuid = context.statement_uuid.canonical;
  dag.statement_timestamp = context.statement_timestamp;
  dag.owning_transaction_uuid = context.transaction_uuid.canonical;
  dag.statement_snapshot_uuid = context.statement_snapshot_uuid.canonical;
  dag.statement_metadata_snapshot_uuid =
      context.statement_metadata_snapshot_uuid.canonical;
  dag.local_transaction_id = context.local_transaction_id;
  dag.snapshot_visible_through_local_transaction_id =
      context.snapshot_visible_through_local_transaction_id;
  dag.root_node_id = 3;

  std::vector<const api::MgaRelationColumnStorageDescriptor*> columns{
      &document.columns.front(), &time_series.columns[0],
      &time_series.columns[1], &time_series.columns[2],
      &time_series.columns[3]};
  for (std::size_t ordinal = 0; ordinal < columns.size(); ++ordinal) {
    const auto descriptor_id = static_cast<std::uint32_t>(ordinal + 1);
    api::RelationalTypeDescriptor descriptor;
    descriptor.descriptor_id = descriptor_id;
    descriptor.descriptor_uuid =
        columns[ordinal]->value_descriptor.descriptor_uuid.canonical;
    descriptor.type_uuid =
        ProductionDescriptorTypeUuidV1(columns[ordinal]->value_descriptor);
    descriptor.nullability =
        columns[ordinal]->nullable
            ? api::RelationalNullability::kNullable
            : api::RelationalNullability::kNonNull;
    if (ordinal == 2) descriptor.timezone_profile_id = "UTC";
    dag.descriptors.push_back(std::move(descriptor));

    api::RelationalExpressionRecord output;
    output.expression_id = descriptor_id;
    output.expression_kind = api::RelationalExpressionKind::kIdentifier;
    output.result_descriptor_id = descriptor_id;
    output.bound_name_uuid = columns[ordinal]->column_uuid.canonical;
    dag.expressions.push_back(std::move(output));
  }
  api::RelationalTypeDescriptor endpoint;
  endpoint.descriptor_id = 6;
  endpoint.descriptor_uuid =
      ProductionUuidV1(platform::UuidKind::object, 952);
  endpoint.type_uuid = ProductionDescriptorTypeUuidV1(
      time_series.columns[1].value_descriptor);
  endpoint.nullability = api::RelationalNullability::kNonNull;
  endpoint.timezone_profile_id = "UTC";
  dag.descriptors.push_back(std::move(endpoint));
  dag.descriptors.push_back(
      {7, ProductionUuidV1(platform::UuidKind::object, 953),
       ProductionUuidV1(platform::UuidKind::object, 954),
       api::RelationalNullability::kNonNull});

  api::RelationalExpressionRecord document_alias;
  document_alias.expression_id = 10;
  document_alias.expression_kind = api::RelationalExpressionKind::kIdentifier;
  document_alias.result_descriptor_id = 1;
  document_alias.bound_name_uuid = document.relation_uuid.canonical;
  dag.expressions.push_back(std::move(document_alias));
  api::RelationalExpressionRecord document_root;
  document_root.expression_id = 11;
  document_root.expression_kind =
      api::RelationalExpressionKind::kFunctionCall;
  document_root.child_expression_ids = {10};
  document_root.result_descriptor_id = 1;
  document_root.operator_name = "DOCUMENT_SOURCE";
  dag.expressions.push_back(std::move(document_root));

  api::RelationalExpressionRecord alias;
  alias.expression_id = 20;
  alias.expression_kind = api::RelationalExpressionKind::kIdentifier;
  alias.result_descriptor_id = 2;
  alias.bound_name_uuid = time_series.relation_uuid.canonical;
  dag.expressions.push_back(std::move(alias));
  for (const auto [expression_id, encoded] :
       std::array<std::pair<std::uint32_t, std::string_view>, 2>{
           {{21, "2026-08-10T12:00:00.000000000Z"},
            {22, "2026-08-10T12:02:00.000000000Z"}}}) {
    api::RelationalExpressionRecord literal;
    literal.expression_id = expression_id;
    literal.expression_kind = api::RelationalExpressionKind::kLiteral;
    literal.result_descriptor_id = 6;
    literal.literal_kind = api::RelationalLiteralKind::kTemporal;
    literal.literal_or_parameter_ref = std::string(encoded);
    dag.expressions.push_back(std::move(literal));
  }
  api::RelationalExpressionRecord range;
  range.expression_id = 23;
  range.expression_kind = api::RelationalExpressionKind::kFunctionCall;
  range.child_expression_ids = {20, 21, 22};
  range.result_descriptor_id = 7;
  range.operator_name = "TIME_RANGE";
  dag.expressions.push_back(std::move(range));

  std::uint32_t output_id = 1;
  dag.outputs.push_back(
      {output_id++, 1, 1, "document_uuid", 1, true, 0});
  static constexpr std::array<std::string_view, 4> kNames{
      "metric_uuid", "point_timestamp", "tags", "value"};
  for (std::uint32_t ordinal = 0; ordinal < kNames.size(); ++ordinal) {
    const auto expression_id = ordinal + 2;
    dag.outputs.push_back(
        {output_id++, 2, expression_id, std::string(kNames[ordinal]),
         expression_id, true, ordinal});
  }
  for (std::uint32_t ordinal = 0; ordinal < 5; ++ordinal) {
    const auto expression_id = ordinal + 1;
    dag.outputs.push_back(
        {output_id++, 3, expression_id,
         ordinal == 0 ? "document_uuid"
         : ordinal == 1 ? "metric_uuid"
         : ordinal == 2 ? "point_timestamp"
         : ordinal == 3 ? "tags"
                        : "value",
         expression_id, true, ordinal});
  }

  api::RelationalDagNode document_node;
  document_node.node_id = 1;
  document_node.node_kind = api::RelationalDagNodeKind::kScan;
  document_node.output_descriptor_ids = {1};
  document_node.bound_expression_ids = {1, 10, 11};
  document_node.required_object_uuids = {document.relation_uuid.canonical};
  document_node.semantic_variant_id = "SBLR_MODEL_SOURCE_V1";
  dag.nodes.push_back(std::move(document_node));
  api::RelationalDagNode time_series_node;
  time_series_node.node_id = 2;
  time_series_node.node_kind = api::RelationalDagNodeKind::kScan;
  time_series_node.output_descriptor_ids = {2, 3, 4, 5};
  time_series_node.bound_expression_ids = {2, 3, 4, 5, 20, 21, 22, 23};
  time_series_node.required_object_uuids = {
      time_series.relation_uuid.canonical};
  time_series_node.semantic_variant_id = "SBLR_MODEL_SOURCE_V1";
  dag.nodes.push_back(std::move(time_series_node));
  api::RelationalDagNode join;
  join.node_id = 3;
  join.node_kind = api::RelationalDagNodeKind::kJoin;
  join.input_node_ids = {1, 2};
  join.output_descriptor_ids = {1, 2, 3, 4, 5};
  join.semantic_variant_id = "join.cross.v1";
  dag.nodes.push_back(std::move(join));
  return dag;
}

bool ProductionTimeSeriesExactClosureProofV1(
    const api::EngineRequestContext& context,
    const api::MgaRelationStorageDescriptor& document,
    const api::MgaRelationStorageDescriptor& time_series) {
  const auto exact =
      ProductionTimeSeriesExactClosureDagV1(context, document, time_series);
  const auto exact_result = api::ValidateTypedRelationalDag(exact);
  bool passed = Require(
      exact_result.accepted,
      exact_result.issues.empty()
          ? "exact time-series owned closure was refused without diagnostic"
          : "exact time-series owned closure was refused:" +
                exact_result.issues.front().diagnostic_id + ":" +
                exact_result.issues.front().field_id);

  const auto scope_for = [](const api::TypedRelationalDag& dag) {
    api::CanonicalRelationalPlanningScope scope;
    scope.catalog_epoch_uuid = dag.bound_catalog_epoch_uuid;
    scope.security_context_uuid = dag.bound_security_context_uuid;
    scope.statement_uuid = dag.statement_uuid;
    scope.statement_timestamp = dag.statement_timestamp;
    scope.owning_transaction_uuid = dag.owning_transaction_uuid;
    scope.statement_snapshot_uuid = dag.statement_snapshot_uuid;
    scope.statement_metadata_snapshot_uuid =
        dag.statement_metadata_snapshot_uuid;
    scope.local_transaction_id = dag.local_transaction_id;
    scope.snapshot_visible_through_local_transaction_id =
        dag.snapshot_visible_through_local_transaction_id;
    scope.metadata_snapshot_engine_owned = true;
    scope.authorization_context_engine_owned = true;
    return scope;
  };
  const auto bridge =
      api::PopulateCanonicalLogicalGraphFromAdmittedTypedRelationalDag(
          exact, scope_for(exact));
  passed &= Require(
      bridge.accepted && !bridge.data_access_allowed &&
          bridge.logical_graph.nodes.size() == 3 &&
          bridge.logical_graph.nodes[1].model_family_identity ==
              planner::CanonicalLogicalModelFamilyIdentity::kTimeSeries,
      bridge.issues.empty()
          ? "exact time-series owned closure did not populate canonical graph"
          : "time-series bridge:" + bridge.issues.front().diagnostic_id +
                ":" + bridge.issues.front().field_id);

  std::size_t refusal_count = 0;
  const auto require_refusal = [&](api::TypedRelationalDag candidate,
                                   const std::string_view case_id) {
    const auto result = api::ValidateTypedRelationalDag(candidate);
    ++refusal_count;
    return Require(!result.accepted && !result.issues.empty(),
                   "time-series owned-closure mutation was admitted:" +
                       std::string(case_id));
  };
  const auto expression = [](api::TypedRelationalDag* dag,
                             const std::uint32_t id) {
    return std::ranges::find_if(dag->expressions, [&](const auto& candidate) {
      return candidate.expression_id == id;
    });
  };
  const auto descriptor = [](api::TypedRelationalDag* dag,
                             const std::uint32_t id) {
    return std::ranges::find_if(dag->descriptors, [&](const auto& candidate) {
      return candidate.descriptor_id == id;
    });
  };
  const auto add_foreign = [&](api::TypedRelationalDag* dag,
                               const std::uint32_t id) {
    auto foreign = dag->expressions.front();
    foreign.expression_id = id;
    foreign.bound_name_uuid = document.columns.front().column_uuid.canonical;
    dag->expressions.push_back(std::move(foreign));
  };

  for (const auto [width, case_id] :
       std::array<std::pair<std::size_t, std::string_view>, 3>{
           {{5, "output_plus_1"}, {6, "output_plus_2"},
            {7, "output_plus_3"}}}) {
    auto candidate = exact;
    candidate.nodes[1].bound_expression_ids.resize(width);
    passed &= require_refusal(std::move(candidate), case_id);
  }
  auto plus_five = exact;
  add_foreign(&plus_five, 90);
  plus_five.nodes[1].bound_expression_ids.push_back(90);
  passed &= require_refusal(std::move(plus_five), "output_plus_5");
  auto arbitrary_plus_four = exact;
  add_foreign(&arbitrary_plus_four, 90);
  arbitrary_plus_four.nodes[1].bound_expression_ids.back() = 90;
  passed &= require_refusal(std::move(arbitrary_plus_four),
                            "generic_output_plus_4");

  auto reordered_output = exact;
  std::swap(reordered_output.nodes[1].bound_expression_ids[0],
            reordered_output.nodes[1].bound_expression_ids[1]);
  passed &= require_refusal(std::move(reordered_output),
                            "reordered_output_prefix");
  auto missing_output = exact;
  missing_output.nodes[1].bound_expression_ids[0] = 0;
  passed &= require_refusal(std::move(missing_output),
                            "missing_output_expression");
  auto duplicate_output = exact;
  duplicate_output.nodes[1].bound_expression_ids[1] = 2;
  passed &= require_refusal(std::move(duplicate_output),
                            "duplicate_output_expression");
  auto overlapping_output = exact;
  overlapping_output.nodes[1].bound_expression_ids[4] = 2;
  passed &= require_refusal(std::move(overlapping_output),
                            "output_suffix_overlap");
  auto foreign_output = exact;
  add_foreign(&foreign_output, 90);
  foreign_output.nodes[1].bound_expression_ids[0] = 90;
  passed &= require_refusal(std::move(foreign_output),
                            "foreign_output_expression");

  for (const auto [id, case_id] :
       std::array<std::pair<std::uint32_t, std::string_view>, 4>{
           {{23, "missing_TIME_RANGE"}, {20, "missing_alias"},
            {21, "missing_start"}, {22, "missing_end"}}}) {
    auto missing = exact;
    auto& bound = missing.nodes[1].bound_expression_ids;
    bound.erase(std::ranges::find(bound, id));
    passed &= require_refusal(std::move(missing), case_id);
  }
  auto duplicate_range = exact;
  auto duplicate_root = *expression(&duplicate_range, 23);
  duplicate_root.expression_id = 90;
  duplicate_range.expressions.push_back(std::move(duplicate_root));
  duplicate_range.nodes[1].bound_expression_ids.push_back(90);
  passed &= require_refusal(std::move(duplicate_range),
                            "duplicate_TIME_RANGE");
  auto duplicate_suffix = exact;
  duplicate_suffix.nodes[1].bound_expression_ids.back() = 20;
  passed &= require_refusal(std::move(duplicate_suffix),
                            "duplicate_suffix_id");
  auto foreign_suffix = exact;
  add_foreign(&foreign_suffix, 90);
  foreign_suffix.nodes[1].bound_expression_ids.back() = 90;
  passed &= require_refusal(std::move(foreign_suffix), "foreign_suffix_id");
  auto unattached = exact;
  unattached.nodes[1].bound_expression_ids.erase(
      unattached.nodes[1].bound_expression_ids.begin() + 4,
      unattached.nodes[1].bound_expression_ids.end());
  passed &= require_refusal(std::move(unattached), "unattached_closure");
  auto cross_leg = exact;
  cross_leg.nodes[0].bound_expression_ids.push_back(21);
  passed &= require_refusal(std::move(cross_leg), "cross_leg_ownership");
  auto extra_valid = exact;
  add_foreign(&extra_valid, 90);
  extra_valid.nodes[1].bound_expression_ids.push_back(90);
  passed &= require_refusal(std::move(extra_valid),
                            "extra_valid_expression");

  auto wrong_operator = exact;
  expression(&wrong_operator, 23)->operator_name = "TIME_BUCKET";
  passed &= require_refusal(std::move(wrong_operator), "wrong_root_operator");
  auto wrong_kind = exact;
  expression(&wrong_kind, 23)->expression_kind =
      api::RelationalExpressionKind::kBinary;
  passed &= require_refusal(std::move(wrong_kind), "wrong_root_kind");
  auto wrong_arity = exact;
  expression(&wrong_arity, 23)->child_expression_ids.pop_back();
  passed &= require_refusal(std::move(wrong_arity), "wrong_root_arity");
  auto wrong_child_order = exact;
  std::swap(expression(&wrong_child_order, 23)->child_expression_ids[0],
            expression(&wrong_child_order, 23)->child_expression_ids[1]);
  passed &= require_refusal(std::move(wrong_child_order), "wrong_child_order");
  auto broken_reachability = exact;
  expression(&broken_reachability, 23)->child_expression_ids[2] = 5;
  passed &= require_refusal(std::move(broken_reachability),
                            "broken_reachability");

  auto wrong_object = exact;
  wrong_object.nodes[1].required_object_uuids.front() =
      document.relation_uuid.canonical;
  passed &= require_refusal(std::move(wrong_object), "wrong_object_identity");
  auto missing_object = exact;
  missing_object.nodes[1].required_object_uuids.clear();
  passed &= require_refusal(std::move(missing_object),
                            "missing_object_identity");
  auto wrong_descriptor = exact;
  expression(&wrong_descriptor, 21)->result_descriptor_id = 3;
  passed &= require_refusal(std::move(wrong_descriptor),
                            "wrong_descriptor_lineage");

  auto non_temporal = exact;
  expression(&non_temporal, 21)->literal_kind =
      api::RelationalLiteralKind::kString;
  passed &= require_refusal(std::move(non_temporal), "non_temporal_start");
  auto nullable = exact;
  descriptor(&nullable, 6)->nullability =
      api::RelationalNullability::kNullable;
  passed &= require_refusal(std::move(nullable), "nullable_range_operand");
  auto empty = exact;
  expression(&empty, 21)->literal_or_parameter_ref = "";
  passed &= require_refusal(std::move(empty), "empty_range_operand");
  auto malformed = exact;
  expression(&malformed, 21)->literal_or_parameter_ref = "malformed";
  passed &= require_refusal(std::move(malformed), "malformed_range_operand");
  auto mismatched = exact;
  auto second_endpoint = *descriptor(&mismatched, 6);
  second_endpoint.descriptor_id = 8;
  second_endpoint.descriptor_uuid =
      ProductionUuidV1(platform::UuidKind::object, 955);
  mismatched.descriptors.push_back(std::move(second_endpoint));
  expression(&mismatched, 22)->result_descriptor_id = 8;
  passed &= require_refusal(std::move(mismatched),
                            "mismatched_range_descriptors");
  auto substituted = exact;
  expression(&substituted, 23)->child_expression_ids[1] = 4;
  passed &= require_refusal(std::move(substituted),
                            "substituted_range_operand");
  auto missing_statement_timestamp = exact;
  missing_statement_timestamp.statement_timestamp.clear();
  passed &= require_refusal(std::move(missing_statement_timestamp),
                            "missing_statement_timestamp");
  auto timestamp_mismatch = exact;
  timestamp_mismatch.statement_timestamp = "2026-08-10T12:01:00Z";
  auto timestamp_scope = scope_for(exact);
  const auto timestamp_bridge =
      api::PopulateCanonicalLogicalGraphFromAdmittedTypedRelationalDag(
          timestamp_mismatch, timestamp_scope);
  ++refusal_count;
  passed &= Require(!timestamp_bridge.accepted &&
                        !timestamp_bridge.issues.empty(),
                    "time-series statement timestamp mismatch was admitted");
  auto mga_scope = scope_for(exact);
  ++mga_scope.local_transaction_id;
  const auto mga_bridge =
      api::PopulateCanonicalLogicalGraphFromAdmittedTypedRelationalDag(
          exact, mga_scope);
  ++refusal_count;
  passed &= Require(!mga_bridge.accepted && !mga_bridge.issues.empty(),
                    "time-series MGA context mismatch was admitted");

  api::RelationalDagLimits constrained;
  constrained.maximum_records = exact.descriptors.size() +
                                exact.expressions.size() +
                                exact.outputs.size() - 1;
  const auto resource = api::ValidateTypedRelationalDag(exact, constrained);
  ++refusal_count;
  passed &= Require(!resource.accepted && !resource.issues.empty() &&
                        resource.issues.front().diagnostic_id ==
                            "SBLR.PLAN_TREE.RESOURCE_LIMIT",
                    "time-series resource overflow was admitted");
  passed &= Require(refusal_count == 38,
                    "time-series refusal matrix count drifted");
  return passed;
}

api::TypedRelationalDag ProductionSearchExactClosureDagV1(
    const api::EngineRequestContext& context,
    const api::MgaRelationStorageDescriptor& document,
    const api::MgaRelationStorageDescriptor& search) {
  api::TypedRelationalDag dag;
  dag.wire_version = 2;
  dag.bound_sblr_tree_uuid =
      ProductionUuidV1(platform::UuidKind::object, 961);
  dag.bound_catalog_epoch_uuid = context.catalog_epoch_uuid.canonical;
  dag.bound_security_context_uuid =
      context.authorization_context.authority_uuid.canonical;
  dag.statement_uuid = context.statement_uuid.canonical;
  dag.statement_timestamp = context.statement_timestamp;
  dag.owning_transaction_uuid = context.transaction_uuid.canonical;
  dag.statement_snapshot_uuid = context.statement_snapshot_uuid.canonical;
  dag.statement_metadata_snapshot_uuid =
      context.statement_metadata_snapshot_uuid.canonical;
  dag.local_transaction_id = context.local_transaction_id;
  dag.snapshot_visible_through_local_transaction_id =
      context.snapshot_visible_through_local_transaction_id;
  dag.root_node_id = 3;

  const std::array<std::string, 6> output_types{
      ProductionDescriptorTypeUuidV1(
          document.columns.front().value_descriptor),
      ProductionCoreTypeUuidV1("uuid"),
      ProductionCoreTypeUuidV1("uuid"),
      ProductionCoreTypeUuidV1("uint64"),
      ProductionCoreTypeUuidV1("real64"),
      ProductionCoreTypeUuidV1("uint64")};
  for (std::uint32_t descriptor_id = 1; descriptor_id <= 6;
       ++descriptor_id) {
    dag.descriptors.push_back(
        {descriptor_id,
         ProductionUuidV1(platform::UuidKind::object, 962 + descriptor_id),
         output_types[descriptor_id - 1],
         api::RelationalNullability::kNonNull});
    api::RelationalExpressionRecord output;
    output.expression_id = descriptor_id;
    output.expression_kind = api::RelationalExpressionKind::kIdentifier;
    output.result_descriptor_id = descriptor_id;
    output.bound_name_uuid =
        descriptor_id == 1
            ? document.columns.front().column_uuid.canonical
            : descriptor_id == 3 || descriptor_id == 4
                  ? ProductionUuidV1(platform::UuidKind::object, 970)
                  : search.relation_uuid.canonical;
    dag.expressions.push_back(std::move(output));
  }
  dag.descriptors.push_back(
      {7, ProductionUuidV1(platform::UuidKind::object, 971),
       ProductionDescriptorTypeUuidV1(search.columns.front().value_descriptor),
       api::RelationalNullability::kNonNull});
  dag.descriptors.push_back(
      {8, ProductionUuidV1(platform::UuidKind::object, 972),
       ProductionCoreTypeUuidV1("uint64"),
       api::RelationalNullability::kNonNull});

  api::RelationalExpressionRecord document_alias;
  document_alias.expression_id = 10;
  document_alias.expression_kind = api::RelationalExpressionKind::kIdentifier;
  document_alias.result_descriptor_id = 1;
  document_alias.bound_name_uuid = document.relation_uuid.canonical;
  dag.expressions.push_back(std::move(document_alias));
  api::RelationalExpressionRecord document_root;
  document_root.expression_id = 11;
  document_root.expression_kind =
      api::RelationalExpressionKind::kFunctionCall;
  document_root.child_expression_ids = {10};
  document_root.result_descriptor_id = 1;
  document_root.operator_name = "DOCUMENT_SOURCE";
  dag.expressions.push_back(std::move(document_root));

  api::RelationalExpressionRecord alias;
  alias.expression_id = 20;
  alias.expression_kind = api::RelationalExpressionKind::kIdentifier;
  alias.result_descriptor_id = 7;
  alias.bound_name_uuid = search.relation_uuid.canonical;
  dag.expressions.push_back(std::move(alias));
  api::RelationalExpressionRecord term;
  term.expression_id = 21;
  term.expression_kind = api::RelationalExpressionKind::kLiteral;
  term.result_descriptor_id = 7;
  term.literal_kind = api::RelationalLiteralKind::kString;
  term.literal_or_parameter_ref = "quick fox";
  dag.expressions.push_back(std::move(term));
  api::RelationalExpressionRecord terms;
  terms.expression_id = 22;
  terms.expression_kind = api::RelationalExpressionKind::kFunctionCall;
  terms.child_expression_ids = {21};
  terms.result_descriptor_id = 7;
  terms.operator_name = "SEARCH_TERMS";
  dag.expressions.push_back(std::move(terms));
  api::RelationalExpressionRecord analyzer;
  analyzer.expression_id = 23;
  analyzer.expression_kind = api::RelationalExpressionKind::kIdentifier;
  analyzer.result_descriptor_id = 7;
  analyzer.bound_name_uuid =
      ProductionUuidV1(platform::UuidKind::object, 970);
  dag.expressions.push_back(std::move(analyzer));
  api::RelationalExpressionRecord limit;
  limit.expression_id = 24;
  limit.expression_kind = api::RelationalExpressionKind::kLiteral;
  limit.result_descriptor_id = 8;
  limit.literal_kind = api::RelationalLiteralKind::kNumeric;
  limit.literal_or_parameter_ref = "3";
  dag.expressions.push_back(std::move(limit));
  api::RelationalExpressionRecord match;
  match.expression_id = 25;
  match.expression_kind = api::RelationalExpressionKind::kFunctionCall;
  match.child_expression_ids = {20, 22, 23, 24};
  match.result_descriptor_id = 7;
  match.operator_name = "SEARCH_MATCH";
  dag.expressions.push_back(std::move(match));

  std::uint32_t output_id = 1;
  dag.outputs.push_back(
      {output_id++, 1, 1, "document_uuid", 1, true, 0});
  static constexpr std::array<std::string_view, 5> kNames{
      "document_uuid", "analyzer_uuid", "analyzer_generation", "score",
      "rank"};
  for (std::uint32_t ordinal = 0; ordinal < kNames.size(); ++ordinal) {
    const auto expression_id = ordinal + 2;
    dag.outputs.push_back(
        {output_id++, 2, expression_id, std::string(kNames[ordinal]),
         expression_id, true, ordinal});
  }
  for (std::uint32_t ordinal = 0; ordinal < 6; ++ordinal) {
    const auto expression_id = ordinal + 1;
    dag.outputs.push_back(
        {output_id++, 3, expression_id,
         ordinal == 0 ? "document_uuid" : std::string(kNames[ordinal - 1]),
         expression_id, true, ordinal});
  }

  api::RelationalDagNode document_node;
  document_node.node_id = 1;
  document_node.node_kind = api::RelationalDagNodeKind::kScan;
  document_node.output_descriptor_ids = {1};
  document_node.bound_expression_ids = {1, 10, 11};
  document_node.required_object_uuids = {document.relation_uuid.canonical};
  document_node.semantic_variant_id = "SBLR_MODEL_SOURCE_V1";
  dag.nodes.push_back(std::move(document_node));
  api::RelationalDagNode search_node;
  search_node.node_id = 2;
  search_node.node_kind = api::RelationalDagNodeKind::kScan;
  search_node.output_descriptor_ids = {2, 3, 4, 5, 6};
  search_node.bound_expression_ids = {2, 3, 4, 5, 6, 20, 21, 22, 23, 24,
                                      25};
  search_node.required_object_uuids = {search.relation_uuid.canonical};
  search_node.semantic_variant_id = "SBLR_MODEL_SOURCE_V1";
  dag.nodes.push_back(std::move(search_node));
  api::RelationalDagNode join;
  join.node_id = 3;
  join.node_kind = api::RelationalDagNodeKind::kJoin;
  join.input_node_ids = {1, 2};
  join.output_descriptor_ids = {1, 2, 3, 4, 5, 6};
  join.semantic_variant_id = "join.cross.v1";
  dag.nodes.push_back(std::move(join));
  return dag;
}

bool ProductionSearchExactClosureProofV1(
    const api::EngineRequestContext& context,
    const api::MgaRelationStorageDescriptor& document,
    const api::MgaRelationStorageDescriptor& search) {
  const auto exact = ProductionSearchExactClosureDagV1(context, document,
                                                        search);
  const auto exact_result = api::ValidateTypedRelationalDag(exact);
  bool passed = Require(
      exact_result.accepted,
      exact_result.issues.empty()
          ? "exact search owned closure was refused without diagnostic"
          : "exact search owned closure was refused:" +
                exact_result.issues.front().diagnostic_id + ":" +
                exact_result.issues.front().field_id);

  const auto scope_for = [](const api::TypedRelationalDag& dag) {
    api::CanonicalRelationalPlanningScope scope;
    scope.catalog_epoch_uuid = dag.bound_catalog_epoch_uuid;
    scope.security_context_uuid = dag.bound_security_context_uuid;
    scope.statement_uuid = dag.statement_uuid;
    scope.statement_timestamp = dag.statement_timestamp;
    scope.owning_transaction_uuid = dag.owning_transaction_uuid;
    scope.statement_snapshot_uuid = dag.statement_snapshot_uuid;
    scope.statement_metadata_snapshot_uuid =
        dag.statement_metadata_snapshot_uuid;
    scope.local_transaction_id = dag.local_transaction_id;
    scope.snapshot_visible_through_local_transaction_id =
        dag.snapshot_visible_through_local_transaction_id;
    scope.metadata_snapshot_engine_owned = true;
    scope.authorization_context_engine_owned = true;
    return scope;
  };
  const auto bridge =
      api::PopulateCanonicalLogicalGraphFromAdmittedTypedRelationalDag(
          exact, scope_for(exact));
  passed &= Require(
      bridge.accepted && !bridge.data_access_allowed &&
          bridge.logical_graph.nodes.size() == 3 &&
          bridge.logical_graph.nodes[1].model_family_identity ==
              planner::CanonicalLogicalModelFamilyIdentity::kSearch &&
          bridge.logical_graph.nodes[1].bound_expression_ids.size() == 11,
      bridge.issues.empty()
          ? "exact search owned closure did not populate canonical graph"
          : "search bridge:" + bridge.issues.front().diagnostic_id + ":" +
                bridge.issues.front().field_id);

  std::size_t refusal_count = 0;
  const auto require_refusal = [&](api::TypedRelationalDag candidate,
                                   const std::string_view case_id) {
    const auto result = api::ValidateTypedRelationalDag(candidate);
    ++refusal_count;
    return Require(!result.accepted && !result.issues.empty(),
                   "search owned-closure mutation was admitted:" +
                       std::string(case_id));
  };
  const auto expression = [](api::TypedRelationalDag* dag,
                             const std::uint32_t id) {
    return std::ranges::find_if(dag->expressions, [&](const auto& candidate) {
      return candidate.expression_id == id;
    });
  };
  const auto descriptor = [](api::TypedRelationalDag* dag,
                             const std::uint32_t id) {
    return std::ranges::find_if(dag->descriptors, [&](const auto& candidate) {
      return candidate.descriptor_id == id;
    });
  };
  const auto add_foreign = [&](api::TypedRelationalDag* dag,
                               const std::uint32_t id) {
    auto foreign = dag->expressions.front();
    foreign.expression_id = id;
    foreign.bound_name_uuid = document.columns.front().column_uuid.canonical;
    dag->expressions.push_back(std::move(foreign));
  };

  for (const auto [width, case_id] :
       std::array<std::pair<std::size_t, std::string_view>, 5>{
           {{6, "output_plus_1"}, {7, "output_plus_2"},
            {8, "output_plus_3"}, {9, "output_plus_4"},
            {10, "output_plus_5"}}}) {
    auto candidate = exact;
    candidate.nodes[1].bound_expression_ids.resize(width);
    passed &= require_refusal(std::move(candidate), case_id);
  }
  auto plus_seven = exact;
  add_foreign(&plus_seven, 90);
  plus_seven.nodes[1].bound_expression_ids.push_back(90);
  passed &= require_refusal(std::move(plus_seven), "output_plus_7");
  auto generic_plus_six = exact;
  add_foreign(&generic_plus_six, 90);
  generic_plus_six.nodes[1].bound_expression_ids.back() = 90;
  passed &= require_refusal(std::move(generic_plus_six),
                            "generic_output_plus_6");

  auto reordered_output = exact;
  std::swap(reordered_output.nodes[1].bound_expression_ids[0],
            reordered_output.nodes[1].bound_expression_ids[1]);
  passed &= require_refusal(std::move(reordered_output),
                            "reordered_output_expression");
  auto missing_output = exact;
  missing_output.nodes[1].bound_expression_ids[0] = 0;
  passed &= require_refusal(std::move(missing_output),
                            "missing_output_expression");
  auto duplicate_output = exact;
  duplicate_output.nodes[1].bound_expression_ids[1] = 2;
  passed &= require_refusal(std::move(duplicate_output),
                            "duplicate_output_expression");
  auto overlapping_output = exact;
  overlapping_output.nodes[1].bound_expression_ids[5] = 2;
  passed &= require_refusal(std::move(overlapping_output),
                            "overlapping_output_expression");
  auto foreign_output = exact;
  add_foreign(&foreign_output, 90);
  foreign_output.nodes[1].bound_expression_ids[0] = 90;
  passed &= require_refusal(std::move(foreign_output),
                            "foreign_output_expression");

  static constexpr std::array<std::pair<std::uint32_t, std::string_view>, 6>
      kClosureRecords{{{20, "alias"}, {21, "term_literal"},
                       {22, "SEARCH_TERMS"}, {23, "analyzer"},
                       {24, "limit"}, {25, "SEARCH_MATCH"}}};
  for (const auto [id, name] : kClosureRecords) {
    auto missing = exact;
    auto& bound = missing.nodes[1].bound_expression_ids;
    bound.erase(std::ranges::find(bound, id));
    passed &= require_refusal(std::move(missing),
                              "missing_" + std::string(name));

    auto duplicate = exact;
    auto duplicate_record = *expression(&duplicate, id);
    duplicate_record.expression_id = 90;
    duplicate.expressions.push_back(std::move(duplicate_record));
    duplicate.nodes[1].bound_expression_ids.push_back(90);
    passed &= require_refusal(std::move(duplicate),
                              "duplicate_" + std::string(name));
  }

  auto duplicate_suffix = exact;
  duplicate_suffix.nodes[1].bound_expression_ids.back() = 20;
  passed &= require_refusal(std::move(duplicate_suffix),
                            "duplicate_suffix_expression");
  auto foreign_suffix = exact;
  add_foreign(&foreign_suffix, 90);
  foreign_suffix.nodes[1].bound_expression_ids.back() = 90;
  passed &= require_refusal(std::move(foreign_suffix),
                            "foreign_suffix_expression");
  auto unattached = exact;
  unattached.nodes[1].bound_expression_ids.resize(5);
  passed &= require_refusal(std::move(unattached),
                            "unattached_suffix_expression");
  auto cross_leg = exact;
  cross_leg.nodes[0].bound_expression_ids.push_back(21);
  passed &= require_refusal(std::move(cross_leg),
                            "cross_leg_suffix_expression");
  auto substituted_suffix = exact;
  substituted_suffix.nodes[1].bound_expression_ids.back() = 6;
  passed &= require_refusal(std::move(substituted_suffix),
                            "substituted_suffix_expression");
  auto extra_valid = exact;
  add_foreign(&extra_valid, 90);
  extra_valid.nodes[1].bound_expression_ids.push_back(90);
  passed &= require_refusal(std::move(extra_valid),
                            "extra_valid_suffix_expression");

  auto wrong_root_operator = exact;
  expression(&wrong_root_operator, 25)->operator_name = "SEARCH_FILTER";
  passed &= require_refusal(std::move(wrong_root_operator),
                            "wrong_root_operator");
  auto wrong_root_kind = exact;
  expression(&wrong_root_kind, 25)->expression_kind =
      api::RelationalExpressionKind::kBinary;
  passed &= require_refusal(std::move(wrong_root_kind), "wrong_root_kind");
  auto wrong_root_arity = exact;
  expression(&wrong_root_arity, 25)->child_expression_ids.pop_back();
  passed &= require_refusal(std::move(wrong_root_arity), "wrong_root_arity");
  auto wrong_root_child_order = exact;
  std::swap(expression(&wrong_root_child_order, 25)->child_expression_ids[0],
            expression(&wrong_root_child_order, 25)->child_expression_ids[1]);
  passed &= require_refusal(std::move(wrong_root_child_order),
                            "wrong_root_child_order");
  auto wrong_aux_operator = exact;
  expression(&wrong_aux_operator, 22)->operator_name = "SEARCH_PHRASE";
  passed &= require_refusal(std::move(wrong_aux_operator),
                            "wrong_auxiliary_operator");
  auto wrong_aux_kind = exact;
  expression(&wrong_aux_kind, 22)->expression_kind =
      api::RelationalExpressionKind::kUnary;
  passed &= require_refusal(std::move(wrong_aux_kind),
                            "wrong_auxiliary_kind");
  auto wrong_aux_arity = exact;
  expression(&wrong_aux_arity, 22)->child_expression_ids.push_back(23);
  passed &= require_refusal(std::move(wrong_aux_arity),
                            "wrong_auxiliary_arity");
  auto broken_reachability = exact;
  expression(&broken_reachability, 25)->child_expression_ids[1] = 5;
  passed &= require_refusal(std::move(broken_reachability),
                            "broken_reachability");

  auto wrong_object = exact;
  wrong_object.nodes[1].required_object_uuids.front() =
      document.relation_uuid.canonical;
  passed &= require_refusal(std::move(wrong_object), "wrong_source_object");
  auto missing_object = exact;
  missing_object.nodes[1].required_object_uuids.clear();
  passed &= require_refusal(std::move(missing_object),
                            "missing_source_object");
  auto wrong_alias_object = exact;
  expression(&wrong_alias_object, 20)->bound_name_uuid =
      document.relation_uuid.canonical;
  passed &= require_refusal(std::move(wrong_alias_object),
                            "wrong_alias_object_binding");
  auto missing_analyzer = exact;
  expression(&missing_analyzer, 23)->bound_name_uuid.reset();
  passed &= require_refusal(std::move(missing_analyzer),
                            "missing_analyzer_binding");
  auto wrong_analyzer = exact;
  expression(&wrong_analyzer, 23)->bound_name_uuid =
      search.relation_uuid.canonical;
  passed &= require_refusal(std::move(wrong_analyzer),
                            "wrong_analyzer_binding");
  auto wrong_descriptor = exact;
  expression(&wrong_descriptor, 21)->result_descriptor_id = 8;
  passed &= require_refusal(std::move(wrong_descriptor),
                            "wrong_descriptor_lineage");

  auto null_term = exact;
  expression(&null_term, 21)->literal_or_parameter_ref.reset();
  passed &= require_refusal(std::move(null_term), "null_term_literal");
  auto empty_term = exact;
  expression(&empty_term, 21)->literal_or_parameter_ref = "";
  passed &= require_refusal(std::move(empty_term), "empty_term_literal");
  auto wrong_term_kind = exact;
  expression(&wrong_term_kind, 21)->literal_kind =
      api::RelationalLiteralKind::kNumeric;
  passed &= require_refusal(std::move(wrong_term_kind),
                            "wrong_kind_term_literal");
  auto malformed_term = exact;
  expression(&malformed_term, 21)->child_expression_ids = {23};
  passed &= require_refusal(std::move(malformed_term),
                            "malformed_term_literal");
  auto substituted_term = exact;
  expression(&substituted_term, 22)->child_expression_ids.front() = 23;
  passed &= require_refusal(std::move(substituted_term),
                            "substituted_term_literal");

  auto null_limit = exact;
  expression(&null_limit, 24)->literal_or_parameter_ref.reset();
  passed &= require_refusal(std::move(null_limit), "null_limit");
  auto nonnumeric_limit = exact;
  expression(&nonnumeric_limit, 24)->literal_or_parameter_ref = "x";
  passed &= require_refusal(std::move(nonnumeric_limit), "nonnumeric_limit");
  auto nonpositive_limit = exact;
  expression(&nonpositive_limit, 24)->literal_or_parameter_ref = "0";
  passed &= require_refusal(std::move(nonpositive_limit),
                            "nonpositive_limit");
  auto overflowed_limit = exact;
  expression(&overflowed_limit, 24)->literal_or_parameter_ref =
      "18446744073709551616";
  passed &= require_refusal(std::move(overflowed_limit),
                            "overflowed_limit");
  auto substituted_limit = exact;
  expression(&substituted_limit, 25)->child_expression_ids[3] = 23;
  passed &= require_refusal(std::move(substituted_limit),
                            "substituted_limit");

  auto missing_statement_timestamp = exact;
  missing_statement_timestamp.statement_timestamp.clear();
  passed &= require_refusal(std::move(missing_statement_timestamp),
                            "missing_statement_timestamp");
  auto mismatched_scope = scope_for(exact);
  ++mismatched_scope.local_transaction_id;
  const auto mga_bridge =
      api::PopulateCanonicalLogicalGraphFromAdmittedTypedRelationalDag(
          exact, mismatched_scope);
  ++refusal_count;
  passed &= Require(!mga_bridge.accepted && !mga_bridge.issues.empty(),
                    "search MGA context mismatch was admitted");

  api::RelationalDagLimits constrained;
  constrained.maximum_records = exact.descriptors.size() +
                                exact.expressions.size() +
                                exact.outputs.size() - 1;
  const auto resource = api::ValidateTypedRelationalDag(exact, constrained);
  ++refusal_count;
  passed &= Require(!resource.accepted && !resource.issues.empty() &&
                        resource.issues.front().diagnostic_id ==
                            "SBLR.PLAN_TREE.RESOURCE_LIMIT",
                    "search resource overflow was admitted");
  passed &= Require(refusal_count == 57,
                    "search refusal matrix count drifted");
  return passed;
}

api::RelationalTypeDescriptor PairCompatibilityDescriptorV1(
    const std::uint32_t id, const std::uint64_t salt,
    const bool timezone = false) {
  api::RelationalTypeDescriptor descriptor;
  descriptor.descriptor_id = id;
  descriptor.descriptor_uuid =
      ProductionUuidV1(platform::UuidKind::object, salt);
  descriptor.type_uuid =
      ProductionUuidV1(platform::UuidKind::object, salt + 1000);
  descriptor.nullability = api::RelationalNullability::kNonNull;
  if (timezone) descriptor.timezone_profile_id = "UTC";
  return descriptor;
}

api::TypedRelationalDag ProductionRcp079TimeSeriesColumnarPairDagV1(
    const api::EngineRequestContext& context,
    const api::MgaRelationStorageDescriptor& time_series,
    const api::MgaRelationStorageDescriptor& columnar) {
  api::TypedRelationalDag dag;
  dag.wire_version = 2;
  dag.bound_sblr_tree_uuid =
      ProductionUuidV1(platform::UuidKind::object, 1101);
  dag.bound_catalog_epoch_uuid = context.catalog_epoch_uuid.canonical;
  dag.bound_security_context_uuid =
      context.authorization_context.authority_uuid.canonical;
  dag.statement_uuid = context.statement_uuid.canonical;
  dag.statement_timestamp = context.statement_timestamp;
  dag.owning_transaction_uuid = context.transaction_uuid.canonical;
  dag.statement_snapshot_uuid = context.statement_snapshot_uuid.canonical;
  dag.statement_metadata_snapshot_uuid =
      context.statement_metadata_snapshot_uuid.canonical;
  dag.local_transaction_id = context.local_transaction_id;
  dag.snapshot_visible_through_local_transaction_id =
      context.snapshot_visible_through_local_transaction_id;
  dag.root_node_id = 3;

  for (std::uint32_t id = 1; id <= 6; ++id) {
    dag.descriptors.push_back(
        PairCompatibilityDescriptorV1(id, 1100 + id, id == 4));
  }
  for (std::uint32_t id = 11; id <= 15; ++id) {
    dag.descriptors.push_back(
        PairCompatibilityDescriptorV1(id, 1200 + id, id == 13));
  }
  dag.descriptors.push_back(PairCompatibilityDescriptorV1(20, 1301));
  dag.descriptors.push_back(PairCompatibilityDescriptorV1(21, 1302));
  dag.descriptors.push_back(
      PairCompatibilityDescriptorV1(22, 1303, true));

  static constexpr std::array<std::string_view, 6> kTimeNames{
      "row_uuid", "series_uuid", "metric_uuid", "point_timestamp", "tags",
      "value"};
  for (std::size_t ordinal = 0; ordinal < kTimeNames.size(); ++ordinal) {
    const auto id = static_cast<std::uint32_t>(ordinal + 1);
    api::RelationalExpressionRecord expression;
    expression.expression_id = id;
    expression.expression_kind = api::RelationalExpressionKind::kIdentifier;
    expression.result_descriptor_id = id;
    expression.bound_name_uuid = ProductionUuidV1(
        platform::UuidKind::object, 1400 + ordinal);
    dag.expressions.push_back(std::move(expression));
    dag.outputs.push_back({id, 1, id, std::string(kTimeNames[ordinal]), id,
                           true, static_cast<std::uint32_t>(ordinal)});
  }
  api::RelationalExpressionRecord time_alias;
  time_alias.expression_id = 20;
  time_alias.expression_kind = api::RelationalExpressionKind::kIdentifier;
  time_alias.result_descriptor_id = 2;
  time_alias.bound_name_uuid = time_series.relation_uuid.canonical;
  dag.expressions.push_back(std::move(time_alias));
  for (const auto [id, value] :
       std::array<std::pair<std::uint32_t, std::string_view>, 2>{
           {{21, "2026-08-10T12:00:00.000000000Z"},
            {22, "2026-08-10T12:02:00.000000000Z"}}}) {
    api::RelationalExpressionRecord endpoint;
    endpoint.expression_id = id;
    endpoint.expression_kind = api::RelationalExpressionKind::kLiteral;
    endpoint.result_descriptor_id = 22;
    endpoint.literal_kind = api::RelationalLiteralKind::kTemporal;
    endpoint.literal_or_parameter_ref = std::string(value);
    dag.expressions.push_back(std::move(endpoint));
  }
  api::RelationalExpressionRecord range;
  range.expression_id = 23;
  range.expression_kind = api::RelationalExpressionKind::kFunctionCall;
  range.child_expression_ids = {20, 21, 22};
  range.result_descriptor_id = 21;
  range.operator_name = "TIME_RANGE";
  dag.expressions.push_back(std::move(range));

  static constexpr std::array<std::string_view, 5> kColumnarNames{
      "join_uuid", "payload", "event_timestamp", "metric_uuid", "tags"};
  for (std::size_t ordinal = 0; ordinal < kColumnarNames.size(); ++ordinal) {
    const auto expression_id = static_cast<std::uint32_t>(51 + ordinal);
    const auto descriptor_id = static_cast<std::uint32_t>(11 + ordinal);
    api::RelationalExpressionRecord expression;
    expression.expression_id = expression_id;
    expression.expression_kind = api::RelationalExpressionKind::kIdentifier;
    expression.result_descriptor_id = descriptor_id;
    expression.bound_name_uuid = ProductionUuidV1(
        platform::UuidKind::object, 1500 + ordinal);
    dag.expressions.push_back(std::move(expression));
    dag.outputs.push_back(
        {expression_id, 2, expression_id, std::string(kColumnarNames[ordinal]),
         descriptor_id, true, static_cast<std::uint32_t>(ordinal)});
  }
  api::RelationalExpressionRecord columnar_source;
  columnar_source.expression_id = 60;
  columnar_source.expression_kind =
      api::RelationalExpressionKind::kFunctionCall;
  columnar_source.result_descriptor_id = 11;
  columnar_source.operator_name = "COLUMNAR_SOURCE";
  columnar_source.bound_name_uuid = columnar.relation_uuid.canonical;
  dag.expressions.push_back(std::move(columnar_source));
  api::RelationalExpressionRecord tolerance;
  tolerance.expression_id = 70;
  tolerance.expression_kind = api::RelationalExpressionKind::kLiteral;
  tolerance.result_descriptor_id = 20;
  tolerance.literal_kind = api::RelationalLiteralKind::kNumeric;
  tolerance.literal_or_parameter_ref = "30000000000";
  dag.expressions.push_back(std::move(tolerance));

  api::RelationalDagNode time_node;
  time_node.node_id = 1;
  time_node.node_kind = api::RelationalDagNodeKind::kScan;
  time_node.output_descriptor_ids = {1, 2, 3, 4, 5, 6};
  time_node.bound_expression_ids = {1, 2, 3, 4, 5, 6, 23};
  time_node.required_object_uuids = {time_series.relation_uuid.canonical};
  time_node.semantic_variant_id = "SBLR_MODEL_SOURCE_V1";
  dag.nodes.push_back(std::move(time_node));
  api::RelationalDagNode columnar_node;
  columnar_node.node_id = 2;
  columnar_node.node_kind = api::RelationalDagNodeKind::kScan;
  columnar_node.output_descriptor_ids = {11, 12, 13, 14, 15};
  columnar_node.bound_expression_ids = {51, 52, 53, 54, 55, 60};
  columnar_node.required_object_uuids = {columnar.relation_uuid.canonical};
  columnar_node.semantic_variant_id = "SBLR_MODEL_SOURCE_V1";
  dag.nodes.push_back(std::move(columnar_node));
  api::RelationalDagNode join;
  join.node_id = 3;
  join.node_kind = api::RelationalDagNodeKind::kJoin;
  join.input_node_ids = {1, 2};
  join.output_descriptor_ids = {1, 2, 3, 4, 5, 6, 11, 12, 13, 14, 15};
  join.bound_expression_ids = {3, 5, 4, 54, 55, 53, 70};
  join.semantic_variant_id = "join.asof.left.v1";
  dag.nodes.push_back(std::move(join));
  std::uint32_t root_output_id = 100;
  const auto source_outputs = dag.outputs;
  for (std::size_t ordinal = 0; ordinal < source_outputs.size(); ++ordinal) {
    auto output = source_outputs[ordinal];
    output.output_id = root_output_id++;
    output.relation_node_id = 3;
    output.ordinal = static_cast<std::uint32_t>(ordinal);
    dag.outputs.push_back(std::move(output));
  }
  return dag;
}

api::TypedRelationalDag ProductionRcp079SpatialSearchPairDagV1(
    const api::EngineRequestContext& context,
    const api::MgaRelationStorageDescriptor& spatial,
    const api::MgaRelationStorageDescriptor& search) {
  api::TypedRelationalDag dag;
  dag.wire_version = 2;
  dag.bound_sblr_tree_uuid =
      ProductionUuidV1(platform::UuidKind::object, 2101);
  dag.bound_catalog_epoch_uuid = context.catalog_epoch_uuid.canonical;
  dag.bound_security_context_uuid =
      context.authorization_context.authority_uuid.canonical;
  dag.statement_uuid = context.statement_uuid.canonical;
  dag.statement_timestamp = context.statement_timestamp;
  dag.owning_transaction_uuid = context.transaction_uuid.canonical;
  dag.statement_snapshot_uuid = context.statement_snapshot_uuid.canonical;
  dag.statement_metadata_snapshot_uuid =
      context.statement_metadata_snapshot_uuid.canonical;
  dag.local_transaction_id = context.local_transaction_id;
  dag.snapshot_visible_through_local_transaction_id =
      context.snapshot_visible_through_local_transaction_id;
  dag.root_node_id = 3;
  for (std::uint32_t id = 1; id <= 8; ++id) {
    dag.descriptors.push_back(
        PairCompatibilityDescriptorV1(id, 2100 + id));
  }
  for (std::uint32_t id = 11; id <= 17; ++id) {
    dag.descriptors.push_back(
        PairCompatibilityDescriptorV1(id, 2200 + id));
  }
  dag.descriptors.push_back(PairCompatibilityDescriptorV1(20, 2301));

  static constexpr std::array<std::string_view, 5> kSpatialNames{
      "row_uuid", "spatial_value", "crs_uuid", "predicate_truth",
      "distance"};
  for (std::size_t ordinal = 0; ordinal < kSpatialNames.size(); ++ordinal) {
    const auto id = static_cast<std::uint32_t>(ordinal + 1);
    api::RelationalExpressionRecord output;
    output.expression_id = id;
    output.expression_kind = api::RelationalExpressionKind::kIdentifier;
    output.result_descriptor_id = id;
    output.bound_name_uuid = ProductionUuidV1(
        platform::UuidKind::object, 2400 + ordinal);
    dag.expressions.push_back(std::move(output));
    dag.outputs.push_back({id, 1, id, std::string(kSpatialNames[ordinal]), id,
                           true, static_cast<std::uint32_t>(ordinal)});
  }
  api::RelationalExpressionRecord spatial_source;
  spatial_source.expression_id = 100;
  spatial_source.expression_kind =
      api::RelationalExpressionKind::kFunctionCall;
  spatial_source.result_descriptor_id = 1;
  spatial_source.operator_name = "SPATIAL_SOURCE";
  spatial_source.bound_name_uuid = spatial.relation_uuid.canonical;
  dag.expressions.push_back(std::move(spatial_source));
  api::RelationalExpressionRecord spatial_alias;
  spatial_alias.expression_id = 101;
  spatial_alias.expression_kind = api::RelationalExpressionKind::kIdentifier;
  spatial_alias.result_descriptor_id = 1;
  spatial_alias.bound_name_uuid = spatial.relation_uuid.canonical;
  dag.expressions.push_back(std::move(spatial_alias));
  api::RelationalExpressionRecord spatial_predicate;
  spatial_predicate.expression_id = 102;
  spatial_predicate.expression_kind = api::RelationalExpressionKind::kLiteral;
  spatial_predicate.result_descriptor_id = 6;
  spatial_predicate.literal_kind = api::RelationalLiteralKind::kString;
  spatial_predicate.literal_or_parameter_ref = "INTERSECTS";
  dag.expressions.push_back(std::move(spatial_predicate));
  for (const auto id : {103U, 104U}) {
    api::RelationalExpressionRecord coordinate;
    coordinate.expression_id = id;
    coordinate.expression_kind = api::RelationalExpressionKind::kLiteral;
    coordinate.result_descriptor_id = 7;
    coordinate.literal_kind = api::RelationalLiteralKind::kNumeric;
    coordinate.literal_or_parameter_ref = "0";
    dag.expressions.push_back(std::move(coordinate));
  }
  api::RelationalExpressionRecord point;
  point.expression_id = 105;
  point.expression_kind = api::RelationalExpressionKind::kFunctionCall;
  point.child_expression_ids = {103, 104};
  point.result_descriptor_id = 2;
  point.operator_name = "POINT";
  dag.expressions.push_back(std::move(point));
  api::RelationalExpressionRecord crs;
  crs.expression_id = 106;
  crs.expression_kind = api::RelationalExpressionKind::kIdentifier;
  crs.result_descriptor_id = 3;
  crs.bound_name_uuid = ProductionUuidV1(platform::UuidKind::object, 2501);
  dag.expressions.push_back(std::move(crs));
  api::RelationalExpressionRecord spatial_match;
  spatial_match.expression_id = 107;
  spatial_match.expression_kind = api::RelationalExpressionKind::kFunctionCall;
  spatial_match.child_expression_ids = {101, 102, 105, 106};
  spatial_match.result_descriptor_id = 4;
  spatial_match.operator_name = "SPATIAL_MATCH";
  dag.expressions.push_back(std::move(spatial_match));
  api::RelationalExpressionRecord top_k;
  top_k.expression_id = 108;
  top_k.expression_kind = api::RelationalExpressionKind::kLiteral;
  top_k.result_descriptor_id = 8;
  top_k.literal_kind = api::RelationalLiteralKind::kNumeric;
  top_k.literal_or_parameter_ref = "3";
  dag.expressions.push_back(std::move(top_k));
  api::RelationalExpressionRecord nearest;
  nearest.expression_id = 109;
  nearest.expression_kind = api::RelationalExpressionKind::kFunctionCall;
  nearest.child_expression_ids = {101, 105, 106, 108};
  nearest.result_descriptor_id = 5;
  nearest.operator_name = "SPATIAL_NEAREST";
  dag.expressions.push_back(std::move(nearest));

  static constexpr std::array<std::string_view, 5> kSearchNames{
      "document_uuid", "analyzer_uuid", "analyzer_generation", "score",
      "rank"};
  for (std::size_t ordinal = 0; ordinal < kSearchNames.size(); ++ordinal) {
    const auto expression_id = static_cast<std::uint32_t>(201 + ordinal);
    const auto descriptor_id = static_cast<std::uint32_t>(11 + ordinal);
    api::RelationalExpressionRecord output;
    output.expression_id = expression_id;
    output.result_descriptor_id = descriptor_id;
    if (ordinal == 0 || ordinal == 3) {
      output.expression_kind = api::RelationalExpressionKind::kIdentifier;
      output.bound_name_uuid = search.relation_uuid.canonical;
    } else if (ordinal == 1) {
      output.expression_kind = api::RelationalExpressionKind::kIdentifier;
      output.bound_name_uuid =
          ProductionUuidV1(platform::UuidKind::object, 2601);
    } else {
      output.expression_kind = api::RelationalExpressionKind::kLiteral;
      output.literal_kind = api::RelationalLiteralKind::kNumeric;
      output.literal_or_parameter_ref = ordinal == 2 ? "7" : "2";
    }
    dag.expressions.push_back(std::move(output));
    dag.outputs.push_back(
        {expression_id, 2, expression_id, std::string(kSearchNames[ordinal]),
         descriptor_id, true, static_cast<std::uint32_t>(ordinal)});
  }
  api::RelationalExpressionRecord search_alias;
  search_alias.expression_id = 206;
  search_alias.expression_kind = api::RelationalExpressionKind::kIdentifier;
  search_alias.result_descriptor_id = 16;
  search_alias.bound_name_uuid = search.relation_uuid.canonical;
  dag.expressions.push_back(std::move(search_alias));
  api::RelationalExpressionRecord search_text;
  search_text.expression_id = 207;
  search_text.expression_kind = api::RelationalExpressionKind::kLiteral;
  search_text.result_descriptor_id = 16;
  search_text.literal_kind = api::RelationalLiteralKind::kString;
  search_text.literal_or_parameter_ref = "alpha";
  dag.expressions.push_back(std::move(search_text));
  api::RelationalExpressionRecord terms;
  terms.expression_id = 208;
  terms.expression_kind = api::RelationalExpressionKind::kFunctionCall;
  terms.child_expression_ids = {207};
  terms.result_descriptor_id = 16;
  terms.operator_name = "SEARCH_TERMS";
  dag.expressions.push_back(std::move(terms));
  api::RelationalExpressionRecord digest;
  digest.expression_id = 209;
  digest.expression_kind = api::RelationalExpressionKind::kLiteral;
  digest.result_descriptor_id = 16;
  digest.literal_kind = api::RelationalLiteralKind::kString;
  digest.literal_or_parameter_ref =
      "9033908d159ddd442f2042467fd49e0a12b47679f7514e9aa6e55488e151d316";
  dag.expressions.push_back(std::move(digest));
  api::RelationalExpressionRecord binding;
  binding.expression_id = 210;
  binding.expression_kind = api::RelationalExpressionKind::kFunctionCall;
  binding.child_expression_ids = {202, 203, 209};
  binding.result_descriptor_id = 12;
  binding.operator_name = "SEARCH_ANALYZER_BINDING";
  dag.expressions.push_back(std::move(binding));
  api::RelationalExpressionRecord search_match;
  search_match.expression_id = 211;
  search_match.expression_kind = api::RelationalExpressionKind::kFunctionCall;
  search_match.child_expression_ids = {206, 208, 210, 205};
  search_match.result_descriptor_id = 11;
  search_match.operator_name = "SEARCH_MATCH";
  dag.expressions.push_back(std::move(search_match));
  api::RelationalExpressionRecord category;
  category.expression_id = 212;
  category.expression_kind = api::RelationalExpressionKind::kIdentifier;
  category.result_descriptor_id = 17;
  category.bound_name_uuid =
      ProductionUuidV1(platform::UuidKind::object, 2602);
  dag.expressions.push_back(std::move(category));
  api::RelationalExpressionRecord join_predicate;
  join_predicate.expression_id = 300;
  join_predicate.expression_kind = api::RelationalExpressionKind::kBinary;
  join_predicate.child_expression_ids = {1, 201};
  join_predicate.result_descriptor_id = 20;
  join_predicate.operator_name = "=";
  dag.expressions.push_back(std::move(join_predicate));

  api::RelationalDagNode spatial_node;
  spatial_node.node_id = 1;
  spatial_node.node_kind = api::RelationalDagNodeKind::kScan;
  spatial_node.output_descriptor_ids = {1, 2, 3, 4, 5};
  spatial_node.bound_expression_ids = {1, 2, 3, 4, 5, 100, 101, 102,
                                       103, 104, 105, 106, 107, 108, 109};
  spatial_node.required_object_uuids = {spatial.relation_uuid.canonical};
  spatial_node.semantic_variant_id = "SBLR_MODEL_SOURCE_V1";
  dag.nodes.push_back(std::move(spatial_node));
  api::RelationalDagNode search_node;
  search_node.node_id = 2;
  search_node.node_kind = api::RelationalDagNodeKind::kScan;
  search_node.output_descriptor_ids = {11, 12, 13, 14, 15};
  search_node.bound_expression_ids = {201, 202, 203, 204, 205, 206,
                                      207, 208, 209, 210, 211, 212};
  search_node.required_object_uuids = {search.relation_uuid.canonical};
  search_node.semantic_variant_id = "SBLR_MODEL_SOURCE_V1";
  dag.nodes.push_back(std::move(search_node));
  api::RelationalDagNode join;
  join.node_id = 3;
  join.node_kind = api::RelationalDagNodeKind::kJoin;
  join.input_node_ids = {1, 2};
  join.output_descriptor_ids = {1, 2, 3, 4, 5, 11, 12, 13, 14, 15};
  join.bound_expression_ids = {300};
  join.semantic_variant_id = "join.left-outer.on.v1";
  dag.nodes.push_back(std::move(join));
  std::uint32_t root_output_id = 400;
  const auto source_outputs = dag.outputs;
  for (std::size_t ordinal = 0; ordinal < source_outputs.size(); ++ordinal) {
    auto output = source_outputs[ordinal];
    output.output_id = root_output_id++;
    output.relation_node_id = 3;
    output.ordinal = static_cast<std::uint32_t>(ordinal);
    dag.outputs.push_back(std::move(output));
  }
  return dag;
}

bool ProductionRcp079PairCompatibilityProofV1(
    const api::EngineRequestContext& context,
    const api::MgaRelationStorageDescriptor& time_series,
    const api::MgaRelationStorageDescriptor& columnar,
    const api::MgaRelationStorageDescriptor& spatial,
    const api::MgaRelationStorageDescriptor& search) {
  const auto time_pair = ProductionRcp079TimeSeriesColumnarPairDagV1(
      context, time_series, columnar);
  const auto spatial_pair = ProductionRcp079SpatialSearchPairDagV1(
      context, spatial, search);
  const auto scope_for = [](const api::TypedRelationalDag& dag) {
    api::CanonicalRelationalPlanningScope scope;
    scope.catalog_epoch_uuid = dag.bound_catalog_epoch_uuid;
    scope.security_context_uuid = dag.bound_security_context_uuid;
    scope.statement_uuid = dag.statement_uuid;
    scope.statement_timestamp = dag.statement_timestamp;
    scope.owning_transaction_uuid = dag.owning_transaction_uuid;
    scope.statement_snapshot_uuid = dag.statement_snapshot_uuid;
    scope.statement_metadata_snapshot_uuid =
        dag.statement_metadata_snapshot_uuid;
    scope.local_transaction_id = dag.local_transaction_id;
    scope.snapshot_visible_through_local_transaction_id =
        dag.snapshot_visible_through_local_transaction_id;
    scope.metadata_snapshot_engine_owned = true;
    scope.authorization_context_engine_owned = true;
    return scope;
  };
  const auto time_validation = api::ValidateTypedRelationalDag(time_pair);
  const auto spatial_validation = api::ValidateTypedRelationalDag(spatial_pair);
  bool passed = Require(
      time_validation.accepted,
      time_validation.issues.empty()
          ? "RCP-079 time-series/columnar compatibility pair was refused"
          : "RCP-079 time-series/columnar compatibility pair was refused:" +
                time_validation.issues.front().diagnostic_id + ":" +
                time_validation.issues.front().field_id);
  passed &= Require(
      spatial_validation.accepted,
      spatial_validation.issues.empty()
          ? "RCP-079 spatial/search compatibility pair was refused"
          : "RCP-079 spatial/search compatibility pair was refused:" +
                spatial_validation.issues.front().diagnostic_id + ":" +
                spatial_validation.issues.front().field_id);
  const auto time_bridge =
      api::PopulateCanonicalLogicalGraphFromAdmittedTypedRelationalDag(
          time_pair, scope_for(time_pair));
  const auto spatial_bridge =
      api::PopulateCanonicalLogicalGraphFromAdmittedTypedRelationalDag(
          spatial_pair, scope_for(spatial_pair));
  passed &= Require(
      time_bridge.accepted && time_bridge.logical_graph.nodes.size() == 3 &&
          time_bridge.logical_graph.nodes[0].model_family_identity ==
              planner::CanonicalLogicalModelFamilyIdentity::kTimeSeries &&
          time_bridge.logical_graph.nodes[1].model_family_identity ==
              planner::CanonicalLogicalModelFamilyIdentity::kColumnar,
      time_bridge.issues.empty()
          ? "RCP-079 time-series/columnar pair did not populate"
          : "RCP-079 time-series/columnar bridge:" +
                time_bridge.issues.front().diagnostic_id + ":" +
                time_bridge.issues.front().field_id);
  passed &= Require(
      spatial_bridge.accepted &&
          spatial_bridge.logical_graph.nodes.size() == 3 &&
          spatial_bridge.logical_graph.nodes[0].model_family_identity ==
              planner::CanonicalLogicalModelFamilyIdentity::kSpatial &&
          spatial_bridge.logical_graph.nodes[1].model_family_identity ==
              planner::CanonicalLogicalModelFamilyIdentity::kSearch,
      spatial_bridge.issues.empty()
          ? "RCP-079 spatial/search pair did not populate"
          : "RCP-079 spatial/search bridge:" +
                spatial_bridge.issues.front().diagnostic_id + ":" +
                spatial_bridge.issues.front().field_id);

  std::size_t refusal_count = 0;
  const auto require_refusal = [&](api::TypedRelationalDag candidate,
                                   const std::string_view case_id) {
    const auto result = api::ValidateTypedRelationalDag(candidate);
    ++refusal_count;
    return Require(!result.accepted && !result.issues.empty(),
                   "RCP-079 pair compatibility mutation was admitted:" +
                       std::string(case_id));
  };
  const auto expression = [](api::TypedRelationalDag* dag,
                             const std::uint32_t id) {
    return std::ranges::find_if(dag->expressions, [&](const auto& candidate) {
      return candidate.expression_id == id;
    });
  };

  auto wrong_time_pair = time_pair;
  expression(&wrong_time_pair, 60)->operator_name = "SPATIAL_SOURCE";
  passed &= require_refusal(std::move(wrong_time_pair),
                            "time_wrong_family_pair");
  auto third_model = time_pair;
  auto duplicate_model = third_model.nodes[1];
  duplicate_model.node_id = 4;
  duplicate_model.required_object_uuids.front() =
      ProductionUuidV1(platform::UuidKind::object, 3101);
  third_model.nodes.push_back(std::move(duplicate_model));
  third_model.nodes[2].input_node_ids.push_back(4);
  passed &= require_refusal(std::move(third_model), "time_third_model_leg");
  auto wrong_time_join = time_pair;
  wrong_time_join.nodes[2].semantic_variant_id = "join.inner.v1";
  passed &= require_refusal(std::move(wrong_time_join),
                            "time_wrong_join_semantic");
  auto duplicate_range = time_pair;
  auto second_range = *expression(&duplicate_range, 23);
  second_range.expression_id = 80;
  duplicate_range.expressions.push_back(std::move(second_range));
  duplicate_range.nodes[0].bound_expression_ids.push_back(80);
  passed &= require_refusal(std::move(duplicate_range),
                            "time_duplicate_operation");
  auto missing_range = time_pair;
  missing_range.nodes[0].bound_expression_ids.pop_back();
  passed &= require_refusal(std::move(missing_range),
                            "time_unattached_operation");
  auto incomplete_time = time_pair;
  incomplete_time.nodes[0].required_object_uuids.clear();
  passed &= require_refusal(std::move(incomplete_time),
                            "time_incomplete_identity");
  auto time_descriptor = time_pair;
  time_descriptor.outputs[2].descriptor_id = 4;
  passed &= require_refusal(std::move(time_descriptor),
                            "time_descriptor_lineage");
  auto time_cycle = time_pair;
  expression(&time_cycle, 23)->child_expression_ids[0] = 23;
  passed &= require_refusal(std::move(time_cycle), "time_expression_cycle");
  auto arbitrary_two_leg = time_pair;
  arbitrary_two_leg.nodes[2].semantic_variant_id = "join.cross.v1";
  passed &= require_refusal(std::move(arbitrary_two_leg),
                            "time_arbitrary_two_leg_shape");
  auto wrong_direction = time_pair;
  std::swap(wrong_direction.nodes[2].bound_expression_ids[0],
            wrong_direction.nodes[2].bound_expression_ids[3]);
  passed &= require_refusal(std::move(wrong_direction),
                            "time_asof_direction_or_key");
  auto bad_tolerance = time_pair;
  expression(&bad_tolerance, 70)->literal_or_parameter_ref = "0";
  passed &= require_refusal(std::move(bad_tolerance), "time_zero_tolerance");
  auto time_output = time_pair;
  time_output.outputs.front().ordinal = 1;
  passed &= require_refusal(std::move(time_output), "time_output_ordinal");

  auto wrong_spatial_pair = spatial_pair;
  expression(&wrong_spatial_pair, 100)->operator_name = "COLUMNAR_SOURCE";
  passed &= require_refusal(std::move(wrong_spatial_pair),
                            "spatial_wrong_family_pair");
  auto wrong_outer = spatial_pair;
  wrong_outer.nodes[2].semantic_variant_id = "join.inner.v1";
  passed &= require_refusal(std::move(wrong_outer),
                            "spatial_wrong_join_semantic");
  auto missing_binding = spatial_pair;
  auto& search_bound = missing_binding.nodes[1].bound_expression_ids;
  search_bound.erase(std::ranges::find(search_bound, 210));
  passed &= require_refusal(std::move(missing_binding),
                            "search_unattached_analyzer_binding");
  auto changed_digest = spatial_pair;
  expression(&changed_digest, 209)->literal_or_parameter_ref = "bad";
  passed &= require_refusal(std::move(changed_digest),
                            "search_digest_mismatch");
  auto changed_category = spatial_pair;
  expression(&changed_category, 212)->bound_name_uuid =
      search.relation_uuid.canonical;
  passed &= require_refusal(std::move(changed_category),
                            "search_category_mismatch");
  auto changed_predicate = spatial_pair;
  expression(&changed_predicate, 300)->operator_name = "<>";
  passed &= require_refusal(std::move(changed_predicate),
                            "spatial_join_predicate_mismatch");
  auto duplicate_search = spatial_pair;
  auto second_terms = *expression(&duplicate_search, 208);
  second_terms.expression_id = 280;
  duplicate_search.expressions.push_back(std::move(second_terms));
  duplicate_search.nodes[1].bound_expression_ids.push_back(280);
  passed &= require_refusal(std::move(duplicate_search),
                            "search_duplicate_operation");
  auto wrong_root_output = spatial_pair;
  wrong_root_output.outputs[10].expression_id = 202;
  wrong_root_output.outputs[10].descriptor_id = 12;
  passed &= require_refusal(std::move(wrong_root_output),
                            "spatial_root_output_lineage");
  auto spatial_cycle = spatial_pair;
  expression(&spatial_cycle, 211)->child_expression_ids[0] = 211;
  passed &= require_refusal(std::move(spatial_cycle),
                            "search_expression_cycle");
  auto wrong_wire = spatial_pair;
  wrong_wire.wire_version = 1;
  passed &= require_refusal(std::move(wrong_wire), "spatial_wrong_wire");
  auto wrong_root = spatial_pair;
  wrong_root.root_node_id = 2;
  passed &= require_refusal(std::move(wrong_root), "spatial_wrong_root");

  auto mismatched_time_scope = scope_for(time_pair);
  ++mismatched_time_scope.local_transaction_id;
  const auto time_scope_refusal =
      api::PopulateCanonicalLogicalGraphFromAdmittedTypedRelationalDag(
          time_pair, mismatched_time_scope);
  ++refusal_count;
  passed &= Require(!time_scope_refusal.accepted &&
                        !time_scope_refusal.issues.empty(),
                    "time pair immutable MGA scope mismatch was admitted");
  auto mismatched_spatial_scope = scope_for(spatial_pair);
  mismatched_spatial_scope.statement_uuid =
      ProductionUuidV1(platform::UuidKind::object, 3201);
  const auto spatial_scope_refusal =
      api::PopulateCanonicalLogicalGraphFromAdmittedTypedRelationalDag(
          spatial_pair, mismatched_spatial_scope);
  ++refusal_count;
  passed &= Require(!spatial_scope_refusal.accepted &&
                        !spatial_scope_refusal.issues.empty(),
                    "spatial pair immutable statement scope mismatch was admitted");
  passed &= Require(refusal_count == 25,
                    "RCP-079 pair compatibility refusal count drifted");
  std::cout << "QOW-CES05-RCP079-PAIR-COMPATIBILITY-V1 positives=2"
            << " refusals=" << refusal_count << '\n';
  return passed;
}

bool ProductionMultimodelQueryExecuteV1() {
  auto memory_policy = memory::DefaultLocalEngineMemoryPolicy();
  memory_policy.policy_name = "qow_ces05_multimodel_production";
  const auto memory_configured = memory::ConfigureDefaultMemoryManagerForFixture(
      memory_policy, "qow_ces05_multimodel_production");
  if (!Require(memory_configured.ok(),
               "production memory manager configuration failed")) {
    return false;
  }
  ProductionFixtureV1 fixture;
  fixture.salt = ProductionNowMillisV1() % 1'000'000;
  fixture.directory = std::filesystem::temp_directory_path() /
                      ("scratchbird_rcp080_multimodel_production_" +
                       std::to_string(fixture.salt));
  std::error_code filesystem_error;
  std::filesystem::create_directories(fixture.directory, filesystem_error);
  if (!Require(!filesystem_error,
               "production fixture directory creation failed")) {
    return false;
  }
  fixture.database_path = fixture.directory / "multimodel.sbdb";
  const auto database_uuid = uuid::GenerateEngineIdentityV7(
      platform::UuidKind::database,
      ProductionNowMillisV1() + fixture.salt + 1);
  const auto filespace_uuid = uuid::GenerateEngineIdentityV7(
      platform::UuidKind::filespace,
      ProductionNowMillisV1() + fixture.salt + 2);
  if (!Require(database_uuid.ok() && filespace_uuid.ok(),
               "production database identity creation failed")) {
    return false;
  }
  db::DatabaseCreateConfig create;
  create.path = fixture.database_path.string();
  create.database_uuid = database_uuid.value;
  create.filespace_uuid = filespace_uuid.value;
  create.creation_unix_epoch_millis = ProductionNowMillisV1();
  create.resource_seed_pack_root = SB_BOOTSTRAP_SEED_PACK_ROOT;
  create.require_resource_seed_pack = true;
  create.allow_overwrite = true;
  const auto created = db::CreateDatabaseFile(create);
  if (!Require(created.ok(), "production database creation failed:" +
                                     created.diagnostic.diagnostic_code)) {
    return false;
  }
  fixture.database_uuid = uuid::UuidToString(database_uuid.value.value);
  fixture.filespace_uuid = uuid::UuidToString(filespace_uuid.value.value);
  fixture.schema_uuid =
      ProductionUuidV1(platform::UuidKind::schema, fixture.salt + 10);
  fixture.principal_uuid =
      ProductionUuidV1(platform::UuidKind::principal, fixture.salt + 11);
  fixture.session_uuid =
      ProductionUuidV1(platform::UuidKind::session, fixture.salt + 12);
  for (std::size_t ordinal = 0; ordinal < fixture.relation_uuids.size();
       ++ordinal) {
    fixture.relation_uuids[ordinal] = ProductionUuidV1(
        platform::UuidKind::object, fixture.salt + 20 + ordinal);
  }
  const auto uuid_type = ProductionCoreTypeUuidV1("uuid");
  const auto geometry_type = ProductionCoreTypeUuidV1("geometry");
  const auto text_type = ProductionCoreTypeUuidV1("character");
  const auto timestamp_type = ProductionCoreTypeUuidV1("timestamp");
  const auto real64_type = ProductionCoreTypeUuidV1("real64");
  const auto uint64_type = ProductionCoreTypeUuidV1("uint64");
  const auto vector_type = ProductionCoreTypeUuidV1("dense_vector");
  const auto spatial_crs_uuid = ProductionUuidV1(
      platform::UuidKind::object, fixture.salt + 29);
  if (!Require(!uuid_type.empty() && !geometry_type.empty() &&
                   !text_type.empty() && !timestamp_type.empty() &&
                   !real64_type.empty() && !uint64_type.empty() &&
                   !vector_type.empty() &&
                   !spatial_crs_uuid.empty(),
               "production UUID/geometry/CRS identity is unavailable")) {
    return false;
  }

  api::EngineRequestContext metadata;
  if (!Require(ProductionBeginV1(fixture, "rcp080-metadata", &metadata),
               "production metadata transaction failed")) {
    return false;
  }
  std::array<api::MgaRelationStorageDescriptor, 9> storage;
  for (std::size_t ordinal = 0; ordinal < storage.size(); ++ordinal) {
    api::CrudTableRecord table;
    table.creator_tx = metadata.local_transaction_id;
    table.table_uuid = fixture.relation_uuids[ordinal];
    if (ordinal == 0) {
      table.default_name = "rcp080_heap";
      table.columns = {
          {"heap_id", "canonical=uuid;type_uuid=" + uuid_type +
                          ";nullable=false"}};
    } else if (ordinal == 1) {
      table.default_name = "rcp080_document";
      table.columns = {
          {"document_id", "canonical=uuid;type_uuid=" + uuid_type +
                              ";nullable=false"}};
    } else if (ordinal == 2) {
      table.default_name = "rcp080_graph";
      table.columns = {
          {"vertex_uuid", "canonical=uuid;type_uuid=" + uuid_type +
                              ";nullable=false"},
          {"edge_uuid", "canonical=uuid;type_uuid=" + uuid_type +
                            ";nullable=true"},
          {"path_uuid", "canonical=uuid;type_uuid=" + uuid_type +
                            ";nullable=false"},
          {"vertex_labels", "canonical=text;type_uuid=" + text_type +
                                ";nullable=false"},
          {"vertex_properties", "canonical=text;type_uuid=" + text_type +
                                    ";nullable=false"},
          {"edge_properties", "canonical=text;type_uuid=" + text_type +
                                  ";nullable=false"},
          {"direction", "canonical=text;type_uuid=" + text_type +
                            ";nullable=false"},
          {"depth", "canonical=uint64;type_uuid=" + uint64_type +
                        ";nullable=false"},
          {"cycle_policy", "canonical=text;type_uuid=" + text_type +
                               ";nullable=false"}};
    } else if (ordinal == 3) {
      table.default_name = "rcp080_key_value";
      table.columns = {
          {"key", "canonical=text;type_uuid=" + text_type +
                      ";nullable=false"},
          {"value", "canonical=text;type_uuid=" + text_type +
                        ";nullable=false"},
          {"expires_at", "canonical=timestamp_tz;type_uuid=" +
                             timestamp_type + ";nullable=true"}};
    } else if (ordinal == 4) {
      table.default_name = "rcp080_time_series";
      table.columns = {
          {"metric_uuid", "canonical=uuid;type_uuid=" + uuid_type +
                              ";nullable=false"},
          {"point_timestamp", "canonical=timestamp_tz;type_uuid=" +
                                  timestamp_type + ";nullable=false"},
          {"tags", "canonical=text;type_uuid=" + text_type +
                       ";nullable=false"},
          {"value", "canonical=real64;type_uuid=" + real64_type +
                        ";nullable=false"}};
    } else if (ordinal == 5) {
      table.default_name = "rcp080_vector";
      table.columns = {
          {"embedding", "canonical=dense_vector;type_uuid=" + vector_type +
                            ";nullable=false;dimension=3;element_type=real32"},
          {"metadata", "canonical=text;type_uuid=" + text_type +
                           ";nullable=false"}};
    } else if (ordinal == 6) {
      table.default_name = "rcp080_search";
      table.columns = {
          {"body", "canonical=text;type_uuid=" + text_type +
                       ";nullable=false"},
          {"category", "canonical=text;type_uuid=" + text_type +
                           ";nullable=false"}};
    } else if (ordinal == 7) {
      table.default_name = "rcp080_spatial";
      table.columns = {
          {"row_uuid", "canonical=uuid;type_uuid=" + uuid_type +
                           ";nullable=false"},
          {"spatial_value",
           "canonical=geometry;type_uuid=" + geometry_type +
               ";nullable=false;subtype=POINT;axes=x,y;crs_uuid=" +
               spatial_crs_uuid + ";crs_generation=1"},
          {"crs_uuid", "canonical=uuid;type_uuid=" + uuid_type +
                           ";nullable=false"}};
    } else {
      table.default_name = "rcp080_columnar";
      table.columns = {
          {"columnar_id", "canonical=uuid;type_uuid=" + uuid_type +
                              ";nullable=false"}};
    }
    if (!Require(!api::AppendMgaTableMetadata(metadata, table).error &&
                     !api::EnsureMgaRelationStorageDescriptor(
                          metadata, table, {}, &storage[ordinal])
                          .error,
                 "production relation descriptor persistence failed")) {
      return false;
    }
  }
  std::array<api::MgaRelationStorageDescriptor, 4>
      rejected_search_storage;
  std::array<std::string, 4> rejected_search_uuids;
  for (std::size_t ordinal = 0; ordinal < rejected_search_storage.size();
       ++ordinal) {
    rejected_search_uuids[ordinal] = ProductionUuidV1(
        platform::UuidKind::object, fixture.salt + 930 + ordinal);
    api::CrudTableRecord table;
    table.creator_tx = metadata.local_transaction_id;
    table.table_uuid = rejected_search_uuids[ordinal];
    table.default_name = "rcp080_rejected_search_" + std::to_string(ordinal);
    const auto category_nullability = ordinal == 0 ? "true" : "false";
    const auto category_type = ordinal == 1 ? uuid_type : text_type;
    auto category_descriptor =
        "canonical=text;type_uuid=" + category_type +
        ";nullable=" + category_nullability;
    if (ordinal == 2) {
      category_descriptor +=
          ";charset_uuid=" +
          ProductionUuidV1(platform::UuidKind::object,
                           fixture.salt + 940) +
          ";collation_uuid=" +
          ProductionUuidV1(platform::UuidKind::object,
                           fixture.salt + 941) +
          ";character_length=32";
    }
    if (ordinal == 3) category_descriptor += ";timezone_profile_id=UTC";
    table.columns = {
        {"body", "canonical=text;type_uuid=" + text_type +
                     ";nullable=false"},
        {"category", std::move(category_descriptor)}};
    if (!Require(!api::AppendMgaTableMetadata(metadata, table).error &&
                     !api::EnsureMgaRelationStorageDescriptor(
                          metadata, table, {}, &rejected_search_storage[ordinal])
                          .error,
                 "rejected search descriptor persistence failed")) {
      return false;
    }
  }
  const auto empty_expiry_relation_uuid = ProductionUuidV1(
      platform::UuidKind::object, fixture.salt + 960);
  api::MgaRelationStorageDescriptor empty_expiry_storage;
  api::CrudTableRecord empty_expiry_table;
  empty_expiry_table.creator_tx = metadata.local_transaction_id;
  empty_expiry_table.table_uuid = empty_expiry_relation_uuid;
  empty_expiry_table.default_name = "rcp080_empty_expiry_negative";
  empty_expiry_table.columns = {
      {"key", "canonical=text;type_uuid=" + text_type +
                  ";nullable=false"},
      {"value", "canonical=text;type_uuid=" + text_type +
                    ";nullable=false"},
      {"expires_at", "canonical=timestamp_tz;type_uuid=" + timestamp_type +
                         ";nullable=true"}};
  if (!Require(!api::AppendMgaTableMetadata(metadata, empty_expiry_table).error &&
                   !api::EnsureMgaRelationStorageDescriptor(
                        metadata, empty_expiry_table, {}, &empty_expiry_storage)
                        .error,
               "empty-expiry negative descriptor persistence failed")) {
    return false;
  }
  if (!Require(ProductionCommitV1(metadata),
               "production metadata commit failed")) {
    return false;
  }

  api::EngineRequestContext writer;
  if (!Require(ProductionBeginV1(fixture, "rcp080-writer", &writer),
               "production writer transaction failed")) {
    return false;
  }
  for (std::size_t ordinal = 0; ordinal < fixture.relation_uuids.size();
       ++ordinal) {
    AddProductionAuthorizationV1(&writer, fixture.relation_uuids[ordinal],
                                 fixture.salt + 60, "INSERT");
  }
  std::array<std::vector<std::string>, 9> row_values;
  row_values[0] = {
      ProductionUuidV1(platform::UuidKind::row, fixture.salt + 40)};
  row_values[1] = {
      ProductionUuidV1(platform::UuidKind::row, fixture.salt + 41)};
  row_values[2] = {};
  row_values[3] = {
      ProductionUuidV1(platform::UuidKind::row, fixture.salt + 43),
      "alpha", "A", "<NULL>"};
  row_values[4] = {
      ProductionUuidV1(platform::UuidKind::row, fixture.salt + 44),
      ProductionUuidV1(platform::UuidKind::object, fixture.salt + 45),
      "2026-08-10T12:01:00.000000000Z", "{}", "1"};
  row_values[5] = {
      ProductionUuidV1(platform::UuidKind::row, fixture.salt + 46),
      "[1,0,0]", "{\"group\":\"a\"}"};
  row_values[6] = {
      ProductionUuidV1(platform::UuidKind::row, fixture.salt + 47),
      "quick fox", "a"};
  const auto spatial_encoded = nosql::EncodeSpatialPoint2dV1({3, 4});
  row_values[7] = {
      ProductionUuidV1(platform::UuidKind::row, fixture.salt + 48),
      std::string(spatial_encoded.begin(), spatial_encoded.end()),
      spatial_crs_uuid};
  row_values[8] = {
      ProductionUuidV1(platform::UuidKind::row, fixture.salt + 49)};
  api::EngineDocumentInsertRequest document_insert;
  document_insert.context = writer;
  document_insert.collection_uuid = fixture.relation_uuids[1];
  document_insert.document_uuid =
      ProductionUuidV1(platform::UuidKind::object, fixture.salt + 43);
  document_insert.row_uuid = row_values[1][0];
  document_insert.target_object.uuid.canonical = document_insert.document_uuid;
  document_insert.localized_names.push_back(
      {"en", "primary", "", "rcp080-explicit-document", true});
  for (std::size_t ordinal = 0; ordinal < storage[1].columns.size(); ++ordinal) {
    api::EngineTypedValue value;
    value.encoded_value = row_values[1][ordinal];
    value.setState(api::EngineValueState::value);
    document_insert.assignments.push_back(
        {storage[1].columns[ordinal].canonical_name_key, std::move(value)});
  }
  if (!Require(api::EngineDocumentInsert(document_insert).ok,
               "production explicit document provider persistence failed")) {
    return false;
  }
  api::EngineGraphWriteRequest graph_write;
  graph_write.context = writer;
  graph_write.structured_graph_persist = true;
  graph_write.graph_object_uuid = fixture.relation_uuids[2];
  graph_write.provider_generation = storage[2].descriptor_generation;
  graph_write.target_object.uuid.canonical = fixture.relation_uuids[2];
  graph_write.target_object.object_kind = "graph";
  graph_write.bound_object_identity.object_uuid.canonical =
      fixture.relation_uuids[2];
  graph_write.bound_object_identity.resolved_object_type = "graph";
  graph_write.bound_object_identity.resolved_schema_uuid.canonical =
      fixture.schema_uuid;
  graph_write.bound_object_identity.catalog_generation_id =
      writer.catalog_generation_id;
  graph_write.bound_object_identity.security_epoch = writer.security_epoch;
  graph_write.bound_object_identity.resource_epoch = writer.resource_epoch;
  graph_write.vertices = {{ProductionUuidV1(platform::UuidKind::object,
                                             fixture.salt + 120),
                           {"P"}, {{"name", "one"}}}};
  if (!Require(api::EngineGraphWrite(graph_write).ok,
               "production explicit graph provider persistence failed")) {
    return false;
  }
  for (std::size_t ordinal = 0; ordinal < storage.size(); ++ordinal) {
    if (ordinal == 2) continue;
    api::CrudRowVersionRecord row;
    row.creator_tx = writer.local_transaction_id;
    row.table_uuid = fixture.relation_uuids[ordinal];
    row.row_uuid = row_values[ordinal].front();
    row.version_uuid = ProductionUuidV1(
        platform::UuidKind::object, fixture.salt + 50 + ordinal);
    const auto value_offset =
        row_values[ordinal].size() == storage[ordinal].columns.size() + 1
            ? 1U
            : 0U;
    for (std::size_t column = 0; column < storage[ordinal].columns.size();
         ++column) {
      row.values.push_back(
          {storage[ordinal].columns[column].canonical_name_key,
           row_values[ordinal][column + value_offset]});
    }
    std::uint64_t sequence = 0;
    if (!Require(!api::AppendMgaRowVersion(writer, row, &sequence).error &&
                     sequence != 0,
                 "production MGA row persistence failed")) {
      return false;
    }
  }
  api::CrudRowVersionRecord empty_expiry_row;
  empty_expiry_row.creator_tx = writer.local_transaction_id;
  empty_expiry_row.table_uuid = empty_expiry_relation_uuid;
  empty_expiry_row.row_uuid =
      ProductionUuidV1(platform::UuidKind::row, fixture.salt + 961);
  empty_expiry_row.version_uuid =
      ProductionUuidV1(platform::UuidKind::object, fixture.salt + 962);
  empty_expiry_row.values = {
      {"key", "empty-expiry-key"}, {"value", "negative"},
      {"expires_at", ""}};
  std::uint64_t empty_expiry_sequence = 0;
  if (!Require(
          !api::AppendMgaRowVersion(writer, empty_expiry_row,
                                    &empty_expiry_sequence)
               .error &&
              empty_expiry_sequence != 0,
          "empty-expiry negative row persistence failed")) {
    return false;
  }
  if (!Require(ProductionCommitV1(writer),
               "production writer commit failed")) {
    return false;
  }

  api::EngineRequestContext reader;
  if (!Require(ProductionBeginV1(fixture, "rcp080-reader", &reader),
               "production reader transaction failed")) {
    return false;
  }
  reader.optimizer_route_epoch = 1;
  reader.optimizer_route_generation = 1;
  reader.optimizer_memory_budget_bytes = 32 * 1024 * 1024;
  reader.optimizer_maximum_candidate_count = 4096;
  reader.optimizer_maximum_memo_groups = 4096;
  reader.optimizer_maximum_search_steps = 16384;
  reader.optimizer_maximum_planning_time_ns = 1'000'000'000;
  reader.current_monotonic_ns = std::to_string(ProductionNowMillisV1());
  reader.query_cancellation_requested = [] { return false; };
  for (std::size_t ordinal = 0; ordinal < fixture.relation_uuids.size();
       ++ordinal) {
    AddProductionAuthorizationV1(&reader, fixture.relation_uuids[ordinal],
                                 fixture.salt + 70);
  }
  for (std::size_t ordinal = 0; ordinal < rejected_search_uuids.size();
       ++ordinal) {
    AddProductionAuthorizationV1(&reader, rejected_search_uuids[ordinal],
                                 fixture.salt + 950 + ordinal);
  }
  AddProductionAuthorizationV1(&reader, empty_expiry_relation_uuid,
                               fixture.salt + 963);
  ProductionPublicSessionV1 public_session(fixture);
  bridge::StatementContextReceiptHandle statement_receipt;
  std::string bound_ast_uuid;
  if (!Require(AcquireProductionStatementAuthorityV1(
                   &public_session, &reader, &statement_receipt,
                   &bound_ast_uuid),
               "production engine statement-context receipt failed")) {
    return false;
  }
  api::EngineTypedValue empty_expiry_key;
  empty_expiry_key.encoded_value = "empty-expiry-key";
  empty_expiry_key.descriptor.canonical_type_name = "text";
  empty_expiry_key.setState(api::EngineValueState::value);
  api::EngineBoundKeyValueReadRequestV1 empty_expiry_request;
  empty_expiry_request.context = reader;
  empty_expiry_request.operation =
      api::EngineBoundKeyValueReadOperationV1::kGet;
  empty_expiry_request.object_uuid = empty_expiry_relation_uuid;
  empty_expiry_request.request_values = {empty_expiry_key};
  empty_expiry_request.statement_timestamp = reader.statement_timestamp;
  empty_expiry_request.expected_descriptor_uuid =
      empty_expiry_storage.descriptor_uuid.canonical;
  empty_expiry_request.expected_descriptor_generation =
      empty_expiry_storage.descriptor_generation;
  empty_expiry_request.selected_alternative_uuid = ProductionUuidV1(
      platform::UuidKind::object, fixture.salt + 964);
  empty_expiry_request.capability_uuid = ProductionUuidV1(
      platform::UuidKind::object, fixture.salt + 965);
  empty_expiry_request.provider_uuid = ProductionUuidV1(
      platform::UuidKind::object, fixture.salt + 966);
  empty_expiry_request.provider_generation =
      empty_expiry_storage.descriptor_generation;
  empty_expiry_request.maximum_request_keys = 1;
  empty_expiry_request.maximum_request_bytes = 4096;
  empty_expiry_request.maximum_scanned_row_versions = 16;
  empty_expiry_request.maximum_decoded_bytes = 64 * 1024;
  empty_expiry_request.maximum_output_rows = 16;
  empty_expiry_request.maximum_value_bytes = 64 * 1024;
  empty_expiry_request.maximum_result_bytes = 64 * 1024;
  empty_expiry_request.maximum_memory_bytes = 128 * 1024;
  empty_expiry_request.cancellation_requested = [] { return false; };
  const auto empty_expiry_refusal =
      api::EngineBoundKeyValueReadV1(empty_expiry_request);
  const auto empty_expiry_code = empty_expiry_refusal.diagnostics.empty()
                                     ? std::string{}
                                     : empty_expiry_refusal.diagnostics.front().code;
  if (!Require(!empty_expiry_refusal.ok &&
                   empty_expiry_refusal.data_access_observed &&
                   empty_expiry_code ==
                       "SB_MODEL_KEY_VALUE_EXPIRES_AT_INVALID_V1",
               "actual empty-string expires_at lost provider refusal:" +
                   empty_expiry_code)) {
    return false;
  }
  bool passed = ProductionStandaloneGraphClosureWidthProofV1(reader,
                                                              storage[2]);
  passed &= ProductionKeyValueExactClosureProofV1(reader, storage[1],
                                                   storage[3]);
  passed &= ProductionTimeSeriesExactClosureProofV1(reader, storage[1],
                                                    storage[4]);
  passed &= ProductionSearchExactClosureProofV1(reader, storage[1],
                                                 storage[6]);
  passed &= ProductionRcp079PairCompatibilityProofV1(
      reader, storage[4], storage[8], storage[7], storage[6]);

  const auto cst = sbsql::BuildCst(
      "SELECT * FROM app.heap CROSS JOIN "
      "SPATIAL_SOURCE(app.spatial) AS s CROSS JOIN "
      "COLUMNAR_SOURCE(app.columnar) AS c;");
  const auto ast = sbsql::BuildAst(cst);
  auto binding = ProductionBindingContextV1(
      ast.native_relational, reader, bound_ast_uuid,
      std::array{storage[0], storage[7], storage[8]});
  const auto parser_session = ProductionParserSessionV1(fixture);
  const auto bound = sbsql::BindAst(
      ast, cst, ProductionParserConfigV1(fixture), parser_session, {},
      &binding);
  const auto lowered = sbsql::LowerToSblr(bound, cst, parser_session);
  const auto verified = sbsql::VerifySblrEnvelope(lowered);
  auto engine_envelope = ProductionEngineEnvelopeV1(lowered, fixture);
  const auto dispatched = sblr::DispatchSblrOperation(
      {reader, engine_envelope, {}});
  std::string operand_inventory;
  for (const auto& operand : lowered.operands) {
    if (!operand_inventory.empty()) operand_inventory += ',';
    operand_inventory += operand.type + ":" + operand.name;
  }
  const auto diagnostic = dispatched.api_result.diagnostics.empty()
                              ? std::string{}
                              : dispatched.api_result.diagnostics.front().code +
                                    ":" + dispatched.api_result.diagnostics.front().detail;
  std::string envelope_diagnostics;
  for (const auto& entry : dispatched.diagnostics) {
    if (!envelope_diagnostics.empty()) envelope_diagnostics += ',';
    envelope_diagnostics += entry.code + ":" + entry.message;
  }
  std::string production_observation =
      "accepted=" + std::to_string(dispatched.accepted) +
      ";executed=" + std::to_string(dispatched.physical_dag_executed) +
      ";published=" + std::to_string(dispatched.canonical_result_published) +
      ";logical=" + std::to_string(dispatched.logical_node_count) +
      ";physical=" + std::to_string(dispatched.physical_node_count) +
      ";columns=" +
      std::to_string(dispatched.canonical_result_column_count) +
      ";rows=" + std::to_string(dispatched.canonical_result_row_count) +
      ";api_rows=" +
      std::to_string(dispatched.api_result.result_shape.rows.size());
  if (!dispatched.api_result.result_shape.rows.empty()) {
    for (const auto& field :
         dispatched.api_result.result_shape.rows.front().fields) {
      production_observation += ";field=" + field.first + ":" +
                                field.second.encoded_value;
    }
  }
  passed &= Require(
      ast.native_relational.accepted() && bound.bound &&
          bound.native_relational.bound && !lowered.messages.has_errors() &&
          verified.admitted && !lowered.parser_executes_sql &&
          dispatched.accepted && dispatched.envelope_validated &&
          dispatched.dispatched_to_api && dispatched.optimizer_admitted &&
          dispatched.optimizer_selected && dispatched.physical_dag_published &&
          dispatched.physical_dag_executed &&
          dispatched.runtime_actuals_attached &&
          dispatched.canonical_result_published && dispatched.api_result.ok &&
          dispatched.logical_node_count == 5 &&
          dispatched.physical_node_count == 5 &&
          dispatched.canonical_result_column_count == 5 &&
          dispatched.canonical_result_row_count == 1 &&
          dispatched.api_result.result_shape.rows.size() == 1 &&
          dispatched.api_result.result_shape.rows.front().fields.size() == 5 &&
          dispatched.api_result.result_shape.rows.front().fields[0].second
                  .encoded_value == row_values[0][0] &&
          dispatched.api_result.result_shape.rows.front().fields[1].second
                  .encoded_value == row_values[7][0] &&
          dispatched.api_result.result_shape.rows.front().fields[2].second
                  .binary_value == spatial_encoded &&
          dispatched.api_result.result_shape.rows.front().fields[3].second
                  .encoded_value == row_values[7][2] &&
          dispatched.api_result.result_shape.rows.front().fields[4].second
                  .encoded_value == row_values[8][0] &&
          HasProductionEvidenceV1(dispatched,
                                  "canonical.model_composition_profile",
                                  "COMP-3-LINEAR-V1") &&
          HasProductionEvidenceV1(dispatched,
                                  "canonical.model_composition_coordinator",
                                  "entered") &&
          HasProductionEvidenceV1(dispatched,
                                  "canonical.model_composition_executor",
                                  "entered") &&
          HasProductionEvidenceV1(dispatched,
                                  "canonical.model_composition_leg_count",
                                  "3") &&
          HasProductionEvidenceV1(dispatched,
                                  "canonical.model_composition_consumer_count",
                                  "2") &&
          HasProductionEvidenceV1(
              dispatched, "canonical.model_composition_provider_entry_count",
              "3") &&
          HasProductionEvidenceV1(
              dispatched,
              "canonical.model_composition_observed_data_access_count", "3") &&
          HasProductionEvidenceV1(
              dispatched, "canonical.model_composition_real_mga_read_count",
              "3") &&
          HasProductionEvidenceV1(
              dispatched, "canonical.model_composition_root_publication_count",
              "1"),
      "production SBSQL/SBLR/query.execute composition failed:" + diagnostic +
          ";envelope=" + envelope_diagnostics +
          ";ast=" + ProductionMessagesV1(ast.messages) +
          ";bind=" + ProductionMessagesV1(bound.messages) +
          ";lower=" + ProductionMessagesV1(lowered.messages) +
          ";verify=" + ProductionMessagesV1(verified.messages) +
          ";operands=" + operand_inventory +
          ";observation=" + production_observation);

  // Prove that a bounded source with explicit operation operands crosses the
  // same ordinary parser, typed-DAG, canonical logical, coordinator, provider,
  // relational-consumer, and result-publication spine.  This uses the real
  // persisted relation descriptor and engine-owned document provider; no test
  // callback or synthetic provider generation participates.
  const auto explicit_cst = sbsql::BuildCst(
      "SELECT * FROM app.heap CROSS JOIN "
      "DOCUMENT_SOURCE(app.docs) AS d CROSS JOIN "
      "COLUMNAR_SOURCE(app.columnar) AS c;");
  const auto explicit_ast = sbsql::BuildAst(explicit_cst);
  auto explicit_binding = ProductionBindingContextV1(
      explicit_ast.native_relational, reader, bound_ast_uuid,
      std::array{storage[0], storage[1], storage[8]});
  const auto explicit_bound = sbsql::BindAst(
      explicit_ast, explicit_cst, ProductionParserConfigV1(fixture),
      parser_session, {}, &explicit_binding);
  const auto explicit_lowered =
      sbsql::LowerToSblr(explicit_bound, explicit_cst, parser_session);
  const auto explicit_verified = sbsql::VerifySblrEnvelope(explicit_lowered);
  const auto explicit_dispatched = sblr::DispatchSblrOperation(
      {reader, ProductionEngineEnvelopeV1(explicit_lowered, fixture), {}});
  const auto explicit_timestamp_operand = std::ranges::find_if(
      explicit_lowered.operands, [](const auto& operand) {
        return operand.type == "text" &&
               operand.name == "relational_statement_timestamp";
      });
  const auto explicit_timestamp =
      explicit_timestamp_operand == explicit_lowered.operands.end()
          ? std::string{"<absent>"}
          : explicit_timestamp_operand->value;
  const auto explicit_diagnostic =
      explicit_dispatched.api_result.diagnostics.empty()
          ? std::string{}
          : explicit_dispatched.api_result.diagnostics.front().code + ":" +
                explicit_dispatched.api_result.diagnostics.front().detail;
  passed &= Require(
      explicit_ast.native_relational.accepted() && explicit_bound.bound &&
          explicit_bound.native_relational.bound &&
          !explicit_lowered.messages.has_errors() &&
          explicit_verified.admitted && explicit_dispatched.accepted &&
          explicit_dispatched.envelope_validated &&
          explicit_dispatched.dispatched_to_api &&
          explicit_dispatched.optimizer_admitted &&
          explicit_dispatched.optimizer_selected &&
          explicit_dispatched.physical_dag_published &&
          explicit_dispatched.physical_dag_executed &&
          explicit_dispatched.runtime_actuals_attached &&
          explicit_dispatched.canonical_result_published &&
          explicit_dispatched.api_result.ok &&
          explicit_dispatched.logical_node_count == 5 &&
          explicit_dispatched.physical_node_count == 5 &&
          explicit_dispatched.canonical_result_column_count == 3 &&
          explicit_dispatched.canonical_result_row_count == 1 &&
          explicit_dispatched.api_result.result_shape.rows.size() == 1 &&
          explicit_dispatched.api_result.result_shape.rows.front()
                  .fields.size() ==
              3 &&
          HasProductionEvidenceV1(
              explicit_dispatched, "canonical.model_composition_profile",
              "COMP-3-LINEAR-V1") &&
          HasProductionEvidenceV1(
              explicit_dispatched,
              "canonical.model_composition_provider_entry_count", "3") &&
          HasProductionEvidenceV1(
              explicit_dispatched,
              "canonical.model_composition_observed_data_access_count", "3") &&
          HasProductionEvidenceV1(
              explicit_dispatched,
              "canonical.model_composition_root_publication_count", "1"),
      "explicit document-operation composition did not cross ordinary dispatch:" +
          explicit_diagnostic + ";ast=" +
          ProductionMessagesV1(explicit_ast.messages) + ";bind=" +
          ProductionMessagesV1(explicit_bound.messages) + ";lower=" +
          ProductionMessagesV1(explicit_lowered.messages) + ";verify=" +
          ProductionMessagesV1(explicit_verified.messages) +
          ";lower_timestamp=" + explicit_timestamp +
          ";engine_timestamp=" + reader.statement_timestamp);

  const auto mixed_cst = sbsql::BuildCst(
      "SELECT * FROM app.heap CROSS JOIN "
      "DOCUMENT_SOURCE(app.docs) AS d CROSS JOIN "
      "GRAPH_SOURCE(app.graph_fixture) AS g CROSS JOIN "
      "VECTOR_SOURCE(app.vectors) AS v WHERE "
      "GRAPH_MATCH(g, 'vertex(*)') AND "
      "VECTOR_NEAREST(v, VECTOR '[1,0,0]', L2_SQUARED, 2);");
  const auto mixed_ast = sbsql::BuildAst(mixed_cst);
  const auto mixed_storage =
      std::array{storage[0], storage[1], storage[2], storage[5]};
  auto mixed_binding = ProductionBindingContextV1(
      mixed_ast.native_relational, reader, bound_ast_uuid, mixed_storage);
  const auto mixed_bound = sbsql::BindAst(
      mixed_ast, mixed_cst, ProductionParserConfigV1(fixture), parser_session,
      {}, &mixed_binding);
  const auto mixed_lowered =
      sbsql::LowerToSblr(mixed_bound, mixed_cst, parser_session);
  const auto mixed_verified = sbsql::VerifySblrEnvelope(mixed_lowered);
  const auto mixed_dispatched = sblr::DispatchSblrOperation(
      {reader, ProductionEngineEnvelopeV1(mixed_lowered, fixture), {}});
  const auto mixed_diagnostic =
      mixed_dispatched.api_result.diagnostics.empty()
          ? std::string{}
          : mixed_dispatched.api_result.diagnostics.front().code + ":" +
                mixed_dispatched.api_result.diagnostics.front().detail;
  passed &= Require(
      mixed_ast.native_relational.accepted() && mixed_bound.bound &&
          mixed_bound.native_relational.bound &&
          !mixed_lowered.messages.has_errors() && mixed_verified.admitted &&
          mixed_dispatched.accepted && mixed_dispatched.optimizer_admitted &&
          mixed_dispatched.optimizer_selected &&
          mixed_dispatched.physical_dag_executed &&
          mixed_dispatched.runtime_actuals_attached &&
          mixed_dispatched.canonical_result_published &&
          mixed_dispatched.api_result.ok &&
          mixed_dispatched.logical_node_count == 7 &&
          mixed_dispatched.physical_node_count == 7 &&
          mixed_dispatched.canonical_result_row_count == 1 &&
          HasProductionEvidenceV1(
              mixed_dispatched, "canonical.model_composition_profile",
              "COMP-4-MIXED-V1") &&
          HasProductionEvidenceV1(
              mixed_dispatched,
              "canonical.model_composition_provider_entry_count", "4") &&
          HasProductionEvidenceV1(
              mixed_dispatched,
              "canonical.model_composition_observed_data_access_count", "4") &&
          HasProductionEvidenceV1(
              mixed_dispatched,
              "canonical.model_composition_root_publication_count", "1"),
      "live COMP-4 established-provider composition failed:" +
          mixed_diagnostic + ";ast=" +
          ProductionMessagesV1(mixed_ast.messages) + ";bind=" +
          ProductionMessagesV1(mixed_bound.messages) + ";lower=" +
          ProductionMessagesV1(mixed_lowered.messages) + ";verify=" +
          ProductionMessagesV1(mixed_verified.messages) + ";dag=" +
          ProductionDagInventoryV1(mixed_bound.native_relational));

  const auto full_nine_cst = sbsql::BuildCst(
      "SELECT * FROM app.heap CROSS JOIN "
      "DOCUMENT_SOURCE(app.docs) AS d CROSS JOIN "
      "GRAPH_SOURCE(app.graph_fixture) AS g CROSS JOIN "
      "KEY_VALUE_SOURCE(app.kv) AS kv CROSS JOIN "
      "TIME_SERIES_SOURCE(app.series) AS ts CROSS JOIN "
      "VECTOR_SOURCE(app.vectors) AS v CROSS JOIN "
      "SEARCH_SOURCE(app.search_fixture) AS q CROSS JOIN "
      "SPATIAL_SOURCE(app.spatial) AS s CROSS JOIN "
      "COLUMNAR_SOURCE(app.columnar) AS c WHERE "
      "GRAPH_MATCH(g, 'vertex(*)') AND KV_KEY(kv) = 'alpha' AND "
      "TIME_RANGE(ts, TIMESTAMP '2026-08-10T12:00:00Z', "
      "TIMESTAMP '2026-08-10T12:02:00Z') AND "
      "VECTOR_NEAREST(v, VECTOR '[1,0,0]', L2_SQUARED, 2) AND "
      "SEARCH_MATCH(q, SEARCH_TERMS('quick fox'), app.ascii_v1, 3);");
  const auto full_nine_ast = sbsql::BuildAst(full_nine_cst);
  auto full_nine_binding = ProductionBindingContextV1(
      full_nine_ast.native_relational, reader, bound_ast_uuid, storage);
  const auto full_nine_key_value_relation = std::ranges::find_if(
      full_nine_ast.native_relational.catalog_relation_sources,
      [](const auto& source) {
        return source.source_kind ==
               sbsql::NativeRelationSourceAstKind::kKeyValue;
      });
  const auto full_nine_key_value_source_id =
      full_nine_key_value_relation ==
              full_nine_ast.native_relational.catalog_relation_sources.end()
          ? 0U
          : full_nine_key_value_relation->source_id;
  const auto full_nine_key_value_relation_record = std::ranges::find_if(
      full_nine_ast.native_relational.relations, [&](const auto& relation) {
        return relation.relation_kind ==
                   sbsql::NativeRelationAstKind::kCatalogSource &&
               relation.relation_source_ids == std::vector<std::uint32_t>{
                   full_nine_key_value_source_id};
      });
  passed &= Require(
      full_nine_key_value_relation !=
              full_nine_ast.native_relational.catalog_relation_sources.end() &&
          full_nine_key_value_relation_record !=
              full_nine_ast.native_relational.relations.end(),
      "full COMP-9 key/value source relation is unavailable");
  if (full_nine_key_value_relation !=
          full_nine_ast.native_relational.catalog_relation_sources.end() &&
      full_nine_key_value_relation_record !=
          full_nine_ast.native_relational.relations.end()) {
    passed &= ProductionKeyValuePublicProjectionBindingProofV1(
        full_nine_binding, storage[3],
        full_nine_key_value_relation_record->relation_id);
  }
  const auto full_nine_time_series_relation = std::ranges::find_if(
      full_nine_ast.native_relational.catalog_relation_sources,
      [](const auto& source) {
        return source.source_kind ==
               sbsql::NativeRelationSourceAstKind::kTimeSeries;
      });
  const auto full_nine_time_series_source_id =
      full_nine_time_series_relation ==
              full_nine_ast.native_relational.catalog_relation_sources.end()
          ? 0U
          : full_nine_time_series_relation->source_id;
  const auto full_nine_time_series_relation_record = std::ranges::find_if(
      full_nine_ast.native_relational.relations, [&](const auto& relation) {
        return relation.relation_kind ==
                   sbsql::NativeRelationAstKind::kCatalogSource &&
               relation.relation_source_ids == std::vector<std::uint32_t>{
                   full_nine_time_series_source_id};
      });
  passed &= Require(
      full_nine_time_series_relation !=
              full_nine_ast.native_relational.catalog_relation_sources.end() &&
          full_nine_time_series_relation_record !=
              full_nine_ast.native_relational.relations.end(),
      "full COMP-9 time-series source relation is unavailable");
  if (full_nine_time_series_relation !=
          full_nine_ast.native_relational.catalog_relation_sources.end() &&
      full_nine_time_series_relation_record !=
          full_nine_ast.native_relational.relations.end()) {
    passed &= ProductionTimeSeriesPublicProjectionBindingProofV1(
        full_nine_binding, storage[4],
        full_nine_time_series_relation_record->relation_id);
  }
  const auto full_nine_bound = sbsql::BindAst(
      full_nine_ast, full_nine_cst, ProductionParserConfigV1(fixture),
      parser_session, {}, &full_nine_binding);
  const auto full_nine_lowered =
      sbsql::LowerToSblr(full_nine_bound, full_nine_cst, parser_session);
  const auto full_nine_verified =
      sbsql::VerifySblrEnvelope(full_nine_lowered);
  const auto full_nine_engine_envelope =
      ProductionEngineEnvelopeV1(full_nine_lowered, fixture);
  const auto full_nine_dispatched = sblr::DispatchSblrOperation(
      {reader, full_nine_engine_envelope, {}});
  const auto full_nine_diagnostic =
      full_nine_dispatched.api_result.diagnostics.empty()
          ? std::string{}
          : full_nine_dispatched.api_result.diagnostics.front().code + ":" +
                full_nine_dispatched.api_result.diagnostics.front().detail;
  std::string full_nine_binding_inventory;
  for (const auto& source : full_nine_ast.native_relational
                                .catalog_relation_sources) {
    const auto relation = std::ranges::find_if(
        full_nine_ast.native_relational.relations, [&](const auto& candidate) {
          return candidate.relation_kind ==
                     sbsql::NativeRelationAstKind::kCatalogSource &&
                 candidate.relation_source_ids ==
                     std::vector<std::uint32_t>{source.source_id};
        });
    const auto relation_id =
        relation == full_nine_ast.native_relational.relations.end()
            ? 0U
            : relation->relation_id;
    const auto output_count = std::ranges::count_if(
        full_nine_binding.outputs, [&](const auto& output) {
          return output.relation_id == relation_id;
        });
    full_nine_binding_inventory +=
        source.model_family_id + ":" + std::to_string(relation_id) + ":" +
        std::to_string(output_count) + ";";
  }
  std::string full_nine_wire_inventory;
  for (const auto& operand : full_nine_lowered.operands) {
    if (operand.type != "relational_node_v1" &&
        operand.type != "relational_node_binding_v1") {
      continue;
    }
    full_nine_wire_inventory += operand.type + ":" + operand.name + ":" +
                                operand.value + ";";
  }
  passed &= Require(
      full_nine_ast.native_relational.accepted() && full_nine_bound.bound &&
          full_nine_bound.native_relational.bound &&
          !full_nine_lowered.messages.has_errors() &&
          full_nine_verified.admitted && full_nine_dispatched.accepted &&
          full_nine_dispatched.optimizer_admitted &&
          full_nine_dispatched.optimizer_selected &&
          full_nine_dispatched.physical_dag_executed &&
          full_nine_dispatched.runtime_actuals_attached &&
          full_nine_dispatched.canonical_result_published &&
          full_nine_dispatched.api_result.ok &&
          full_nine_dispatched.logical_node_count == 17 &&
          full_nine_dispatched.physical_node_count == 17 &&
          full_nine_dispatched.canonical_result_row_count == 1 &&
          HasProductionEvidenceV1(
              full_nine_dispatched, "canonical.model_composition_profile",
              "COMP-9-FULL-UNIVERSE-V1") &&
          HasProductionEvidenceV1(
              full_nine_dispatched,
              "canonical.model_composition_provider_entry_count", "9") &&
          HasProductionEvidenceV1(
              full_nine_dispatched,
              "canonical.model_composition_observed_data_access_count", "9") &&
          HasProductionEvidenceV1(
              full_nine_dispatched,
              "canonical.model_composition_real_mga_read_count", "9") &&
          HasProductionEvidenceV1(
              full_nine_dispatched,
              "canonical.model_composition_consumer_count", "8") &&
          HasProductionEvidenceV1(
              full_nine_dispatched,
              "canonical.model_composition_root_publication_count", "1"),
      "live COMP-9 established-provider composition failed:" +
          full_nine_diagnostic + ";ast=" +
          ProductionMessagesV1(full_nine_ast.messages) + ";bind=" +
          ProductionMessagesV1(full_nine_bound.messages) + ";lower=" +
          ProductionMessagesV1(full_nine_lowered.messages) + ";verify=" +
          ProductionMessagesV1(full_nine_verified.messages) + ";dag=" +
          ProductionDagInventoryV1(full_nine_bound.native_relational) +
          ";binding=" + full_nine_binding_inventory + ";wire=" +
          full_nine_wire_inventory);

  // QOW-TEST-RCP080-OPERATOR-LOCAL-TIME-SERIES-ATTACHMENT-NEGATIVES-V1
  // The successful COMP-9 dispatch above is the product-path positive: its
  // exact plus-four source closure must become the standalone output-plus-root
  // view before provider capture.  These wire mutations prove every near
  // shape remains fail-closed before provider, MGA-read, access, or root
  // publication; the converter therefore cannot be used as a repair path.
  const auto require_time_series_attachment_refusal =
      [&](const std::string_view mutation,
          const std::function<bool(sbsql::SblrEnvelope*)>& mutate) {
        auto malformed_time_series = full_nine_lowered;
        const bool mutation_applied = mutate(&malformed_time_series);
        const auto malformed_engine =
            ProductionEngineEnvelopeV1(malformed_time_series, fixture);
        const auto outcome = sblr::DispatchSblrOperation(
            {reader, malformed_engine, {}});
        const auto refusal_code = outcome.api_result.diagnostics.empty()
                                      ? std::string{}
                                      : outcome.api_result.diagnostics.front().code;
        const auto no_positive_counter = [&](const std::string_view kind) {
          return std::ranges::none_of(
              outcome.api_result.evidence, [&](const auto& evidence) {
                return evidence.evidence_kind == kind &&
                       evidence.evidence_id != "0";
              });
        };
        return Require(
            mutation_applied && !outcome.accepted &&
                outcome.envelope_validated && !outcome.optimizer_admitted &&
                !outcome.optimizer_selected &&
                !outcome.physical_dag_published &&
                !outcome.physical_dag_executed &&
                !outcome.runtime_actuals_attached &&
                !outcome.canonical_result_published &&
                !outcome.api_result.ok && !refusal_code.empty() &&
                no_positive_counter(
                    "canonical.model_composition_provider_entry_count") &&
                no_positive_counter(
                    "canonical.model_composition_real_mga_read_count") &&
                no_positive_counter(
                    "canonical.model_composition_observed_data_access_count") &&
                no_positive_counter(
                    "canonical.model_composition_root_publication_count"),
            "time-series operator-local attachment near-shape crossed "
            "dispatch pre-access:" +
                std::string(mutation) + ":" + refusal_code);
      };
  const auto mutate_time_series_binding =
      [](sbsql::SblrEnvelope* envelope,
         const std::function<bool(std::vector<std::string>*)>& mutate) {
        auto* range =
            FindProductionExpressionByOperatorV1(envelope, "TIME_RANGE");
        if (range == nullptr) return false;
        auto* binding =
            FindProductionBindingContainingV1(envelope, range->name);
        if (binding == nullptr) return false;
        auto fields = SplitProductionFieldsV1(binding->value);
        if (fields.size() != 5) return false;
        auto handles = SplitProductionHandlesV1(fields[1]);
        if (!mutate(&handles)) return false;
        fields[1] = JoinProductionHandlesV1(handles);
        binding->value = JoinProductionFieldsV1(fields);
        return true;
      };
  const auto mutate_expression_by_id =
      [](sbsql::SblrEnvelope* envelope, const std::string_view expression_id,
         const std::function<bool(std::vector<std::string>*)>& mutate) {
        const auto expression = std::ranges::find_if(
            envelope->operands, [&](const auto& operand) {
              return operand.type == "relational_expression_v1" &&
                     operand.name == expression_id;
            });
        if (expression == envelope->operands.end()) return false;
        auto fields = SplitProductionFieldsV1(expression->value);
        if (fields.size() != 8 || !mutate(&fields)) return false;
        expression->value = JoinProductionFieldsV1(fields);
        return true;
      };
  const auto time_series_child_id =
      [](sbsql::SblrEnvelope* envelope, const std::size_t ordinal) {
        auto* range =
            FindProductionExpressionByOperatorV1(envelope, "TIME_RANGE");
        if (range == nullptr) return std::string{};
        const auto fields = SplitProductionFieldsV1(range->value);
        if (fields.size() != 8) return std::string{};
        const auto children = SplitProductionHandlesV1(fields[1]);
        return ordinal < children.size() ? children[ordinal] : std::string{};
      };

  passed &= require_time_series_attachment_refusal(
      "missing_output_prefix", [&](auto* envelope) {
        return mutate_time_series_binding(envelope, [](auto* handles) {
          if (handles->size() != 10) return false;
          handles->erase(handles->begin());
          return true;
        });
      });
  passed &= require_time_series_attachment_refusal(
      "extra_output_prefix", [&](auto* envelope) {
        auto* document =
            FindProductionExpressionByOperatorV1(envelope, "DOCUMENT_SOURCE");
        if (document == nullptr) return false;
        auto* document_binding =
            FindProductionBindingContainingV1(envelope, document->name);
        if (document_binding == nullptr) return false;
        auto document_fields = SplitProductionFieldsV1(document_binding->value);
        if (document_fields.size() != 5) return false;
        const auto document_handles =
            SplitProductionHandlesV1(document_fields[1]);
        if (document_handles.empty()) return false;
        return mutate_time_series_binding(envelope, [&](auto* handles) {
          if (handles->size() != 10) return false;
          handles->insert(handles->begin() + 6, document_handles.front());
          return true;
        });
      });
  passed &= require_time_series_attachment_refusal(
      "reordered_output_prefix", [&](auto* envelope) {
        return mutate_time_series_binding(envelope, [](auto* handles) {
          if (handles->size() != 10) return false;
          std::swap((*handles)[0], (*handles)[1]);
          return true;
        });
      });
  passed &= require_time_series_attachment_refusal(
      "duplicate_output_prefix", [&](auto* envelope) {
        return mutate_time_series_binding(envelope, [](auto* handles) {
          if (handles->size() != 10) return false;
          (*handles)[1] = (*handles)[0];
          return true;
        });
      });
  passed &= require_time_series_attachment_refusal(
      "missing_range_root", [&](auto* envelope) {
        return mutate_time_series_binding(envelope, [](auto* handles) {
          if (handles->size() != 10) return false;
          handles->pop_back();
          return true;
        });
      });
  passed &= require_time_series_attachment_refusal(
      "multiple_range_root", [&](auto* envelope) {
        return mutate_time_series_binding(envelope, [](auto* handles) {
          if (handles->size() != 10) return false;
          handles->push_back(handles->back());
          return true;
        });
      });
  for (const auto [suffix_ordinal, case_id] :
       std::array<std::pair<std::size_t, std::string_view>, 3>{
           {{6, "detached_alias"}, {7, "detached_start"},
            {8, "detached_end"}}}) {
    passed &= require_time_series_attachment_refusal(
        case_id, [=](auto* envelope) {
          return mutate_time_series_binding(envelope, [&](auto* handles) {
            if (handles->size() != 10) return false;
            handles->erase(handles->begin() +
                           static_cast<std::ptrdiff_t>(suffix_ordinal));
            return true;
          });
        });
  }
  passed &= require_time_series_attachment_refusal(
      "reordered_alias_start_end", [&](auto* envelope) {
        return mutate_time_series_binding(envelope, [](auto* handles) {
          if (handles->size() != 10) return false;
          std::swap((*handles)[6], (*handles)[7]);
          return true;
        });
      });
  passed &= require_time_series_attachment_refusal(
      "wrong_child_kind", [&](auto* envelope) {
        const auto alias_id = time_series_child_id(envelope, 0);
        return !alias_id.empty() &&
               mutate_expression_by_id(
                   envelope, alias_id, [](auto* fields) {
                     (*fields)[0] = "2";
                     return true;
                   });
      });
  passed &= require_time_series_attachment_refusal(
      "wrong_child_type", [&](auto* envelope) {
        auto* range =
            FindProductionExpressionByOperatorV1(envelope, "TIME_RANGE");
        if (range == nullptr) return false;
        const auto range_fields = SplitProductionFieldsV1(range->value);
        const auto start_id = time_series_child_id(envelope, 1);
        return range_fields.size() == 8 && !start_id.empty() &&
               mutate_expression_by_id(
                   envelope, start_id, [&](auto* fields) {
                     (*fields)[2] = range_fields[2];
                     return true;
                   });
      });
  passed &= require_time_series_attachment_refusal(
      "wrong_bound_object", [&](auto* envelope) {
        const auto alias_id = time_series_child_id(envelope, 0);
        return !alias_id.empty() &&
               mutate_expression_by_id(
                   envelope, alias_id, [&](auto* fields) {
                     (*fields)[4] = fixture.relation_uuids.front();
                     return true;
                   });
      });
  passed &= require_time_series_attachment_refusal(
      "arbitrary_plus_one_attachment", [&](auto* envelope) {
        auto* document =
            FindProductionExpressionByOperatorV1(envelope, "DOCUMENT_SOURCE");
        if (document == nullptr) return false;
        return mutate_time_series_binding(envelope, [&](auto* handles) {
          if (handles->size() != 10) return false;
          handles->push_back(document->name);
          return true;
        });
      });
  passed &= require_time_series_attachment_refusal(
      "other_family_conversion_attempt", [](auto* envelope) {
        auto* range =
            FindProductionExpressionByOperatorV1(envelope, "TIME_RANGE");
        auto* document =
            FindProductionExpressionByOperatorV1(envelope, "DOCUMENT_SOURCE");
        if (range == nullptr || document == nullptr) return false;
        auto* binding =
            FindProductionBindingContainingV1(envelope, document->name);
        if (binding == nullptr) return false;
        auto fields = SplitProductionFieldsV1(binding->value);
        if (fields.size() != 5) return false;
        auto handles = SplitProductionHandlesV1(fields[1]);
        handles.push_back(range->name);
        fields[1] = JoinProductionHandlesV1(handles);
        binding->value = JoinProductionFieldsV1(fields);
        return true;
      });

  // QOW-TEST-RCP080-OPERATOR-LOCAL-SEARCH-CATEGORY-DESCRIPTOR-NEGATIVES-V1
  // The ordinary COMP-9 dispatch above is the positive for allocating one
  // local category descriptor from current relation metadata.  These wire and
  // current-object mutations prove malformed handles, extraneous descriptor
  // cohorts, stale/missing category metadata, output misuse, and other-family
  // shapes cannot turn the local allocation into a repair or publication
  // path.  Every case must remain before provider/access/MGA/publication.
  const auto require_search_category_descriptor_refusal =
      [&](const std::string_view mutation,
          const std::function<bool(sbsql::SblrEnvelope*)>& mutate) {
        auto malformed_search = full_nine_lowered;
        const bool mutation_applied = mutate(&malformed_search);
        const auto outcome = sblr::DispatchSblrOperation(
            {reader, ProductionEngineEnvelopeV1(malformed_search, fixture), {}});
        const auto refusal_code = outcome.api_result.diagnostics.empty()
                                      ? std::string{}
                                      : outcome.api_result.diagnostics.front().code;
        const auto no_positive_counter = [&](const std::string_view kind) {
          return std::ranges::none_of(
              outcome.api_result.evidence, [&](const auto& evidence) {
                return evidence.evidence_kind == kind &&
                       evidence.evidence_id != "0";
              });
        };
        return Require(
            mutation_applied && !outcome.accepted && !outcome.optimizer_admitted &&
                !outcome.optimizer_selected &&
                !outcome.physical_dag_published &&
                !outcome.physical_dag_executed &&
                !outcome.runtime_actuals_attached &&
                !outcome.canonical_result_published &&
                !outcome.api_result.ok && !refusal_code.empty() &&
                no_positive_counter(
                    "canonical.model_composition_provider_entry_count") &&
                no_positive_counter(
                    "canonical.model_composition_real_mga_read_count") &&
                no_positive_counter(
                    "canonical.model_composition_observed_data_access_count") &&
                no_positive_counter(
                    "canonical.model_composition_root_publication_count"),
            "search local category descriptor mutation crossed pre-access:" +
                std::string(mutation) + ":" + refusal_code);
      };
  const auto append_local_descriptor_probe =
      [&](sbsql::SblrEnvelope* envelope, const std::string& descriptor_id,
          const std::uint64_t identity_salt,
          const std::string_view nullability = "1",
          const std::string_view collation = "-",
          const std::string_view timezone = "-",
          const std::string_view descriptor_uuid = {},
          const std::string_view type_uuid = {}) {
        if (envelope == nullptr) return false;
        const auto insertion = std::ranges::find_if(
            envelope->operands, [](const auto& operand) {
              return operand.type == "relational_expression_v1";
            });
        if (insertion == envelope->operands.end()) return false;
        envelope->operands.insert(
            insertion,
            sbsql::SblrOperand{
                "relational_descriptor_v1", descriptor_id,
                (descriptor_uuid.empty()
                     ? ProductionUuidV1(platform::UuidKind::object,
                                        identity_salt)
                     : std::string(descriptor_uuid)) +
                    "|" +
                    (type_uuid.empty() ? text_type : std::string(type_uuid)) +
                    "|" + std::string(nullability) + "|" +
                    std::string(collation) + "|" + std::string(timezone) +
                    "|-|-|-"});
        return true;
      };
  const auto retarget_search_current_object =
      [&](sbsql::SblrEnvelope* envelope, const std::string& object_uuid) {
        auto* match =
            FindProductionExpressionByOperatorV1(envelope, "SEARCH_MATCH");
        if (match == nullptr) return false;
        auto match_fields = SplitProductionFieldsV1(match->value);
        if (match_fields.size() != 8) return false;
        const auto children = SplitProductionHandlesV1(match_fields[1]);
        if (children.size() != 4) return false;
        if (!mutate_expression_by_id(
                envelope, children[0], [&](auto* fields) {
                  (*fields)[4] = object_uuid;
                  return true;
                })) {
          return false;
        }
        auto* binding = FindProductionBindingContainingV1(envelope, match->name);
        if (binding == nullptr) return false;
        auto binding_fields = SplitProductionFieldsV1(binding->value);
        if (binding_fields.size() != 5) return false;
        binding_fields[2] = object_uuid;
        binding->value = JoinProductionFieldsV1(binding_fields);
        return true;
      };

  passed &= require_search_category_descriptor_refusal(
      "maximum_descriptor_id_overflow", [&](auto* envelope) {
        return append_local_descriptor_probe(
            envelope, "4294967295", fixture.salt + 901);
      });
  passed &= require_search_category_descriptor_refusal(
      "zero_descriptor_id", [&](auto* envelope) {
        return append_local_descriptor_probe(envelope, "0",
                                             fixture.salt + 902);
      });
  passed &= require_search_category_descriptor_refusal(
      "colliding_descriptor_id", [&](auto* envelope) {
        const auto existing = std::ranges::find_if(
            envelope->operands, [](const auto& operand) {
              return operand.type == "relational_descriptor_v1";
            });
        return existing != envelope->operands.end() &&
               append_local_descriptor_probe(envelope, existing->name,
                                             fixture.salt + 903);
      });
  passed &= require_search_category_descriptor_refusal(
      "two_descriptor_addition_collision", [&](auto* envelope) {
        return append_local_descriptor_probe(envelope, "60000",
                                             fixture.salt + 904) &&
               append_local_descriptor_probe(envelope, "60000",
                                             fixture.salt + 905);
      });
  passed &= require_search_category_descriptor_refusal(
      "missing_current_category_metadata", [&](auto* envelope) {
        return retarget_search_current_object(envelope,
                                              storage[1].relation_uuid.canonical);
      });
  passed &= require_search_category_descriptor_refusal(
      "substituted_current_category_metadata", [&](auto* envelope) {
        return retarget_search_current_object(envelope,
                                              storage[4].relation_uuid.canonical);
      });
  passed &= require_search_category_descriptor_refusal(
      "nullable_current_category_metadata", [&](auto* envelope) {
        return retarget_search_current_object(
            envelope, rejected_search_uuids[0]);
      });
  passed &= require_search_category_descriptor_refusal(
      "wrong_type_current_category_metadata", [&](auto* envelope) {
        return retarget_search_current_object(
            envelope, rejected_search_uuids[1]);
      });
  passed &= require_search_category_descriptor_refusal(
      "collated_current_category_metadata", [&](auto* envelope) {
        return retarget_search_current_object(
            envelope, rejected_search_uuids[2]);
      });
  passed &= require_search_category_descriptor_refusal(
      "timezone_current_category_metadata", [&](auto* envelope) {
        return retarget_search_current_object(
            envelope, rejected_search_uuids[3]);
      });
  passed &= require_search_category_descriptor_refusal(
      "zero_descriptor_uuid", [&](auto* envelope) {
        return append_local_descriptor_probe(
            envelope, "60002", fixture.salt + 906, "1", "-", "-",
            "00000000-0000-0000-0000-000000000000");
      });
  passed &= require_search_category_descriptor_refusal(
      "zero_type_uuid", [&](auto* envelope) {
        return append_local_descriptor_probe(
            envelope, "60003", fixture.salt + 907, "1", "-", "-", {},
            "00000000-0000-0000-0000-000000000000");
      });
  passed &= require_search_category_descriptor_refusal(
      "new_descriptor_output_misuse", [&](auto* envelope) {
        if (!append_local_descriptor_probe(envelope, "60005",
                                           fixture.salt + 910)) {
          return false;
        }
        auto* match =
            FindProductionExpressionByOperatorV1(envelope, "SEARCH_MATCH");
        if (match == nullptr) return false;
        auto* binding = FindProductionBindingContainingV1(envelope, match->name);
        if (binding == nullptr) return false;
        auto fields = SplitProductionFieldsV1(binding->value);
        if (fields.size() != 5) return false;
        auto handles = SplitProductionHandlesV1(fields[1]);
        if (handles.size() != 11) return false;
        handles.push_back(handles.front());
        fields[1] = JoinProductionHandlesV1(handles);
        binding->value = JoinProductionFieldsV1(fields);
        return true;
      });
  passed &= require_search_category_descriptor_refusal(
      "other_family_allocation_attempt", [&](auto* envelope) {
        auto* match =
            FindProductionExpressionByOperatorV1(envelope, "SEARCH_MATCH");
        if (match == nullptr) return false;
        auto fields = SplitProductionFieldsV1(match->value);
        if (fields.size() != 8) return false;
        fields[6] = EncodeProductionHexV1("DOCUMENT_SOURCE");
        match->value = JoinProductionFieldsV1(fields);
        return append_local_descriptor_probe(envelope, "60006",
                                             fixture.salt + 911);
      });

  const auto full_nine_original_operand_count = full_nine_lowered.operands.size();
  const auto repeated_full_nine = sblr::DispatchSblrOperation(
      {reader, full_nine_engine_envelope, {}});
  passed &= Require(
      full_nine_lowered.operands.size() == full_nine_original_operand_count &&
          repeated_full_nine.accepted == full_nine_dispatched.accepted &&
          repeated_full_nine.canonical_result_published ==
              full_nine_dispatched.canonical_result_published,
      "search local category enrichment mutated the original full DAG");

  // QOW-TEST-RCP080-EXACT-SEARCH-TERMS-ATTACHED-AUXILIARY-NEGATIVES-V1
  const auto require_search_auxiliary_refusal =
      [&](const std::string_view mutation,
          const std::function<bool(sbsql::SblrEnvelope*)>& mutate) {
        auto malformed_search = full_nine_lowered;
        const bool mutation_applied = mutate(&malformed_search);
        const auto malformed_engine =
            ProductionEngineEnvelopeV1(malformed_search, fixture);
        const auto outcome = sblr::DispatchSblrOperation(
            {reader, malformed_engine, {}});
        const auto refusal_code = outcome.api_result.diagnostics.empty()
                                      ? std::string{}
                                      : outcome.api_result.diagnostics.front().code;
        const auto no_positive_counter = [&](const std::string_view kind) {
          return std::ranges::none_of(
              outcome.api_result.evidence, [&](const auto& evidence) {
                return evidence.evidence_kind == kind &&
                       evidence.evidence_id != "0";
              });
        };
        return Require(
            mutation_applied && !outcome.accepted &&
                outcome.envelope_validated && !outcome.optimizer_admitted &&
                !outcome.optimizer_selected &&
                !outcome.physical_dag_published &&
                !outcome.physical_dag_executed &&
                !outcome.runtime_actuals_attached &&
                !outcome.canonical_result_published &&
                !outcome.api_result.ok &&
                refusal_code == "SB_MODEL_BINDING_INCOMPLETE_V1" &&
                no_positive_counter(
                    "canonical.model_composition_provider_entry_count") &&
                no_positive_counter(
                    "canonical.model_composition_real_mga_read_count") &&
                no_positive_counter(
                    "canonical.model_composition_observed_data_access_count") &&
                no_positive_counter(
                    "canonical.model_composition_root_publication_count"),
            "SEARCH_TERMS near-shape crossed dispatch pre-access:" +
                std::string(mutation) + ":" + refusal_code);
      };
  const auto mutate_expression_fields =
      [](sbsql::SblrEnvelope* envelope, const std::string_view operator_name,
         const std::function<bool(std::vector<std::string>*)>& mutate) {
        auto* operand =
            FindProductionExpressionByOperatorV1(envelope, operator_name);
        if (operand == nullptr) return false;
        auto fields = SplitProductionFieldsV1(operand->value);
        if (fields.size() != 8 || !mutate(&fields)) return false;
        operand->value = JoinProductionFieldsV1(fields);
        return true;
      };
  const auto mutate_search_binding =
      [](sbsql::SblrEnvelope* envelope,
         const std::function<bool(std::vector<std::string>*)>& mutate) {
        auto* terms =
            FindProductionExpressionByOperatorV1(envelope, "SEARCH_TERMS");
        if (terms == nullptr) return false;
        auto* binding =
            FindProductionBindingContainingV1(envelope, terms->name);
        if (binding == nullptr) return false;
        auto fields = SplitProductionFieldsV1(binding->value);
        if (fields.size() != 5) return false;
        auto handles = SplitProductionHandlesV1(fields[1]);
        if (!mutate(&handles)) return false;
        fields[1] = JoinProductionHandlesV1(handles);
        binding->value = JoinProductionFieldsV1(fields);
        return true;
      };

  passed &= require_search_auxiliary_refusal(
      "standalone_or_root_classified",
      [&](auto* envelope) {
        return mutate_expression_fields(
            envelope, "SEARCH_MATCH", [](auto* fields) {
              (*fields)[6] = EncodeProductionHexV1("SEARCH_TERMS");
              return true;
            });
      });
  passed &= require_search_auxiliary_refusal(
      "unattached_from_same_source",
      [&](auto* envelope) {
        auto* terms =
            FindProductionExpressionByOperatorV1(envelope, "SEARCH_TERMS");
        if (terms == nullptr) return false;
        const auto terms_id = terms->name;
        return mutate_search_binding(envelope, [&](auto* handles) {
          const auto found = std::ranges::find(*handles, terms_id);
          if (found == handles->end()) return false;
          handles->erase(found);
          return true;
        });
      });
  passed &= require_search_auxiliary_refusal(
      "foreign_source_attachment",
      [&](auto* envelope) {
        auto* terms =
            FindProductionExpressionByOperatorV1(envelope, "SEARCH_TERMS");
        auto* document =
            FindProductionExpressionByOperatorV1(envelope, "DOCUMENT_SOURCE");
        if (terms == nullptr || document == nullptr) return false;
        const auto terms_id = terms->name;
        auto* search_binding =
            FindProductionBindingContainingV1(envelope, terms_id);
        auto* foreign_binding =
            FindProductionBindingContainingV1(envelope, document->name);
        if (search_binding == nullptr || foreign_binding == nullptr ||
            search_binding == foreign_binding) {
          return false;
        }
        auto search_fields = SplitProductionFieldsV1(search_binding->value);
        auto foreign_fields = SplitProductionFieldsV1(foreign_binding->value);
        if (search_fields.size() != 5 || foreign_fields.size() != 5) {
          return false;
        }
        auto search_handles = SplitProductionHandlesV1(search_fields[1]);
        auto foreign_handles = SplitProductionHandlesV1(foreign_fields[1]);
        const auto found = std::ranges::find(search_handles, terms_id);
        if (found == search_handles.end()) return false;
        search_handles.erase(found);
        foreign_handles.push_back(terms_id);
        search_fields[1] = JoinProductionHandlesV1(search_handles);
        foreign_fields[1] = JoinProductionHandlesV1(foreign_handles);
        search_binding->value = JoinProductionFieldsV1(search_fields);
        foreign_binding->value = JoinProductionFieldsV1(foreign_fields);
        return true;
      });
  passed &= require_search_auxiliary_refusal(
      "not_search_match_child_ordinal_1",
      [&](auto* envelope) {
        return mutate_expression_fields(
            envelope, "SEARCH_MATCH", [](auto* fields) {
              auto children = SplitProductionHandlesV1((*fields)[1]);
              if (children.size() != 4) return false;
              std::swap(children[1], children[2]);
              (*fields)[1] = JoinProductionHandlesV1(children);
              return true;
            });
      });
  passed &= require_search_auxiliary_refusal(
      "duplicated_source_binding",
      [&](auto* envelope) {
        auto* terms =
            FindProductionExpressionByOperatorV1(envelope, "SEARCH_TERMS");
        if (terms == nullptr) return false;
        const auto terms_id = terms->name;
        return mutate_search_binding(envelope, [&](auto* handles) {
          handles->push_back(terms_id);
          return true;
        });
      });
  passed &= require_search_auxiliary_refusal(
      "extra_unreached_search_terms",
      [&](auto* envelope) {
        auto* terms =
            FindProductionExpressionByOperatorV1(envelope, "SEARCH_TERMS");
        if (terms == nullptr) return false;
        const auto extra_id = std::string("60000");
        if (std::ranges::any_of(envelope->operands, [&](const auto& operand) {
              return operand.type == "relational_expression_v1" &&
                     operand.name == extra_id;
            })) {
          return false;
        }
        const auto terms_value = terms->value;
        auto* binding =
            FindProductionBindingContainingV1(envelope, terms->name);
        if (binding == nullptr) return false;
        auto fields = SplitProductionFieldsV1(binding->value);
        if (fields.size() != 5) return false;
        auto handles = SplitProductionHandlesV1(fields[1]);
        handles.push_back(extra_id);
        fields[1] = JoinProductionHandlesV1(handles);
        binding->value = JoinProductionFieldsV1(fields);
        const auto insertion = std::ranges::find_if(
            envelope->operands, [](const auto& operand) {
              return operand.type == "relational_output_v1";
            });
        envelope->operands.insert(
            insertion,
            sbsql::SblrOperand{"relational_expression_v1", extra_id,
                               terms_value});
        return true;
      });
  passed &= require_search_auxiliary_refusal(
      "wrong_arity",
      [&](auto* envelope) {
        return mutate_expression_fields(
            envelope, "SEARCH_TERMS", [](auto* fields) {
              (*fields)[1] = "-";
              return true;
            });
      });
  passed &= require_search_auxiliary_refusal(
      "wrong_operator",
      [&](auto* envelope) {
        return mutate_expression_fields(
            envelope, "SEARCH_TERMS", [](auto* fields) {
              (*fields)[6] = EncodeProductionHexV1("SEARCH_TERMZ");
              return true;
            });
      });
  passed &= require_search_auxiliary_refusal(
      "registered_function_uuid",
      [&](auto* envelope) {
        return mutate_expression_fields(
            envelope, "SEARCH_TERMS", [&](auto* fields) {
              (*fields)[3] = fixture.relation_uuids.front();
              return true;
            });
      });
  passed &= require_search_auxiliary_refusal(
      "object_bound",
      [&](auto* envelope) {
        return mutate_expression_fields(
            envelope, "SEARCH_TERMS", [&](auto* fields) {
              (*fields)[4] = fixture.relation_uuids.front();
              return true;
            });
      });
  passed &= require_search_auxiliary_refusal(
      "literal_bearing",
      [&](auto* envelope) {
        return mutate_expression_fields(
            envelope, "SEARCH_TERMS", [](auto* fields) {
              (*fields)[5] = "1";
              (*fields)[7] = EncodeProductionHexV1("quick fox");
              return true;
            });
      });
  passed &= require_search_auxiliary_refusal(
      "outside_exact_reached_owned_closure",
      [&](auto* envelope) {
        return mutate_expression_fields(
            envelope, "SEARCH_MATCH", [](auto* fields) {
              auto children = SplitProductionHandlesV1((*fields)[1]);
              if (children.size() != 4) return false;
              children[1] = children[3];
              (*fields)[1] = JoinProductionHandlesV1(children);
              return true;
            });
      });

  auto malformed = engine_envelope;
  const auto model_binding = std::ranges::find_if(
      malformed.operands, [](const auto& operand) {
        return operand.type == "relational_node_binding_v1" &&
               operand.name == "slot_2";
      });
  if (model_binding != malformed.operands.end()) {
    SetProductionEngineOperandValueV1(
        &*model_binding,
        "53424c525f4d4f44454c5f534f555243455f5631|-|" +
            fixture.relation_uuids[1] + "|-|-");
  }
  const auto refused = sblr::DispatchSblrOperation({reader, malformed, {}});
  const auto refusal_code = refused.api_result.diagnostics.empty()
                                ? std::string{}
                                : refused.api_result.diagnostics.front().code;
  passed &= Require(!refused.accepted && refused.envelope_validated &&
                        !refused.optimizer_admitted &&
                        !refused.physical_dag_executed &&
                        !refused.canonical_result_published &&
                        refusal_code == "SB_MODEL_BINDING_INCOMPLETE_V1",
                    "unattached model root crossed dispatch pre-access:" +
                        refusal_code);

  const auto require_context_refusal =
      [&](std::string_view mutation,
          const std::function<void(api::EngineRequestContext*)>& mutate) {
        auto substituted = reader;
        mutate(&substituted);
        const auto outcome =
            sblr::DispatchSblrOperation({substituted, engine_envelope, {}});
        const auto zero_counter = [&](const std::string_view kind) {
          return HasProductionEvidenceV1(outcome, kind, "0");
        };
        return Require(
            !outcome.accepted && outcome.envelope_validated &&
                !outcome.optimizer_admitted && !outcome.optimizer_selected &&
                !outcome.physical_dag_published &&
                !outcome.physical_dag_executed &&
                !outcome.runtime_actuals_attached &&
                !outcome.canonical_result_published &&
                zero_counter(
                    "canonical.model_composition_provider_entry_count") &&
                zero_counter(
                    "canonical.model_composition_real_mga_read_count") &&
                zero_counter(
                    "canonical.model_composition_observed_data_access_count") &&
                zero_counter(
                    "canonical.model_composition_root_publication_count"),
            "composition context mutation crossed the pre-access boundary:" +
                std::string(mutation));
      };
  std::size_t context_refusal_count = 0;
  const auto context_refusal =
      [&](const std::string_view mutation,
          const std::function<void(api::EngineRequestContext*)>& mutate) {
        ++context_refusal_count;
        passed &= require_context_refusal(mutation, mutate);
      };
  context_refusal("catalog_generation_zero", [](auto* context) {
    context->catalog_generation_id = 0;
  });
  context_refusal("catalog_generation_substituted", [](auto* context) {
    ++context->catalog_generation_id;
  });
  context_refusal("security_epoch_zero", [](auto* context) {
    context->security_epoch = 0;
  });
  context_refusal("security_epoch_substituted", [](auto* context) {
    ++context->security_epoch;
  });
  context_refusal("policy_epoch_zero", [](auto* context) {
    context->authorization_context.policy_epoch = 0;
  });
  context_refusal("resource_epoch_zero", [](auto* context) {
    context->resource_epoch = 0;
  });
  context_refusal("route_epoch_zero", [](auto* context) {
    context->optimizer_route_epoch = 0;
  });
  context_refusal("route_generation_zero", [](auto* context) {
    context->optimizer_route_generation = 0;
  });
  context_refusal("route_generation_overflow", [](auto* context) {
    context->optimizer_route_generation =
        std::numeric_limits<std::uint64_t>::max();
  });
  context_refusal("candidate_limit_zero", [](auto* context) {
    context->optimizer_maximum_candidate_count = 0;
  });
  context_refusal("memo_limit_zero", [](auto* context) {
    context->optimizer_maximum_memo_groups = 0;
  });
  context_refusal("search_limit_zero", [](auto* context) {
    context->optimizer_maximum_search_steps = 0;
  });
  context_refusal("planning_time_limit_zero", [](auto* context) {
    context->optimizer_maximum_planning_time_ns = 0;
  });
  context_refusal("capability_snapshot_absent", [](auto* context) {
    context->optimizer_capability_snapshot_uuid = {};
  });
  context_refusal("resource_snapshot_absent", [](auto* context) {
    context->optimizer_resource_snapshot_uuid = {};
  });
  context_refusal("route_snapshot_absent", [](auto* context) {
    context->optimizer_route_snapshot_uuid = {};
  });
  context_refusal("monotonic_time_zero", [](auto* context) {
    context->current_monotonic_ns = "0";
  });
  context_refusal("monotonic_time_malformed", [](auto* context) {
    context->current_monotonic_ns += "x";
  });
  passed &= Require(
      bridge::ReleaseStatementContextReceipt(statement_receipt) ==
          SB_ENGINE_STATUS_OK,
      "production statement-context receipt release failed");
  passed &= Require(ProductionRollbackV1(reader),
                    "production reader rollback failed");
  if (passed) {
    std::cout <<
        "QOW-CES05-MULTIMODEL-PRODUCTION-COMP-3: passed;frontdoor=SBSQL;"
        "dispatch=query.execute;providers=3;reads=3;consumers=2;rows=1;"
        "publication=1;explicit_operation_dispatch=1\n"
        "QOW-CES05-MULTIMODEL-PRODUCTION-COMP-4: passed;frontdoor=SBSQL;"
        "dispatch=query.execute;providers=4;reads=4;rows=1;publication=1\n"
        "QOW-CES05-MULTIMODEL-PRODUCTION-COMP-9: passed;frontdoor=SBSQL;"
        "dispatch=query.execute;providers=9;observed_data_access=9;"
        "real_mga_reads=9;consumers=8;root_publications=1;rows=1;"
        "unattached_refusal=1;context_refusals="
              << context_refusal_count << '\n';
  }
  return passed;
}
#endif

}  // namespace

int main() {
#if defined(SB_RCP080_PRODUCTION_QUERY_ROUTE)
  return ProductionMultimodelQueryExecuteV1() ? 0 : 1;
#else
  const auto plan = optimizer::CoordinateModelFamilyDependencyDagV1(
      FullNineAdmission());
  const auto root = std::filesystem::temp_directory_path() /
                    "sb_rcp080_qow_ces05_multimodel_closure";
  std::filesystem::remove_all(root);
  memory::TempWorkspaceLifecycleManager workspace(
      WorkspacePolicy(root, "rcp080_qow_ces05_multimodel_closure"));
  RuntimeCounters counters;
  executor::ModelFamilyCompositionExecutionRequestV1 request;
  request.admitted_plan = plan;
  for (std::uint16_t ordinal = 0; ordinal < 9; ++ordinal) {
    executor::ModelFamilyCompositionExecutionLegV1 leg;
    leg.lexical_source_ordinal = ordinal;
    leg.execution = FullNineExecution(plan, ordinal, &counters.provider_cleanups);
    leg.pause_exchange = [] {};
    leg.resume_exchange = [] {};
    leg.cleanup_exchange = [&counters] { counters.exchange_cleanups.fetch_add(1); };
    request.legs.push_back(std::move(leg));
  }
  request.execute_relational_consumer = RuntimeRequest(
      plan, &workspace, &counters, 8400).execute_relational_consumer;
  request.cleanup_relational_consumer = [&counters](const auto) {
    counters.consumer_cleanups.fetch_add(1);
  };
  request.cancellation_requested = [] { return false; };
  request.revalidate_publication_state = [plan] { return Publication(plan); };
  request.backpressure_high_watermark_rows = 2;
  request.backpressure_low_watermark_rows = 1;
  request.current_selected_plan_generation = 9;
  request.current_mga_statement_context = Mga();
  const auto executed = executor::ExecuteModelFamilyCompositionV1(request);
  const bool continuation = executed.accepted && executed.root_published &&
                            ExecuteRcp080RelationalContinuationV1(
                                executed.root_output_batch,
                                request.current_mga_statement_context);
  const bool passed = Require(plan.accepted && plan.stable_schedule.size() == 9 &&
                                  plan.parallel_waves.size() == 9 &&
                                  plan.relational_consumers.size() == 8 &&
                                  executed.accepted && executed.root_published &&
                                  executed.launched_leg_ordinals ==
                                      std::vector<std::uint16_t>({0, 1, 2, 3, 4, 5, 6, 7, 8}) &&
                                  executed.started_leg_ordinals ==
                                      executed.launched_leg_ordinals &&
                                  executed.completed_leg_ordinals ==
                                      executed.launched_leg_ordinals &&
                                  executed.rows_received == 9 &&
                                  executed.rows_published == 1 &&
                                  executed.root_output_batch.columns.size() == 19 &&
                                  executed.root_output_batch.rows.size() == 1 &&
                                  executed.provider_entry_count == 9 &&
                                  executed.observed_data_access_count == 9 &&
                                  executed.provider_cleanup_count == 9 &&
                                  executed.exchange_cleanup_count == 9 &&
                                  executed.relational_consumer_cleanup_count == 8 &&
                                  executed.total_cleanup_count == 26 &&
                                  executed.cleanup_complete && continuation,
                              "complete nine-family physical spine did not admit, execute, publish, and clean exactly: " +
                                  plan.diagnostic_id + ":" + plan.detail + "|" +
                                  executed.diagnostic_id + ":" + executed.detail +
                                  ";waves=" + std::to_string(plan.parallel_waves.size()) +
                                  ";schedule=" + std::to_string(plan.stable_schedule.size()) +
                                  ";consumers=" + std::to_string(plan.relational_consumers.size()) +
                                  ";started=" + std::to_string(executed.started_leg_ordinals.size()) +
                                  ";exchanges=" + std::to_string(executed.started_exchange_ordinals.size()) +
                                  ";consumer_started=" + std::to_string(executed.started_relational_consumer_ids.size()) +
                                  ";columns=" + std::to_string(executed.root_output_batch.columns.size()) +
                                  ";cleanup=" + std::to_string(executed.total_cleanup_count));
  if (!passed) return 1;
  std::cout << "QOW-CES05-MULTIMODEL-CLOSURE: passed;families=9;waves=9;consumers=8;columns=19;rows=1;cleanup=26;relational_continuation=11_of_11\n";
  return 0;
#endif
}
