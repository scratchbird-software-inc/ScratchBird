// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#include "model_family_coordinator.hpp"
#include "model_family_executor.hpp"
#include "logical_plan.hpp"

#if defined(SB_CES05_GRAPH_PRODUCTION_QUERY_ROUTE)
#include "canonical_aggregate_registry.hpp"
#include "canonical_query_execute.hpp"
#include "database_lifecycle.hpp"
#include "datatype_catalog_manifest.hpp"
#include "ddl/create_api.hpp"
#include "mga_relation_store/mga_relation_store.hpp"
#include "nosql/graph_api.hpp"
#include "transaction/transaction_api.hpp"
#include "uuid.hpp"
#endif

#include <array>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <limits>
#include <ranges>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {
namespace api = scratchbird::engine::internal_api;
namespace exec = scratchbird::engine::executor;
namespace opt = scratchbird::engine::optimizer;
namespace plan = scratchbird::engine::planner;
#if defined(SB_CES05_GRAPH_PRODUCTION_QUERY_ROUTE)
namespace db = scratchbird::storage::database;
namespace dt = scratchbird::core::datatypes;
namespace platform = scratchbird::core::platform;
namespace sblr = scratchbird::engine::sblr;
namespace uuid = scratchbird::core::uuid;
#endif

struct Vector {
  const char* id;
  const char* fixture;
  const char* case_class;
  const char* expected_diagnostic;
  std::uint64_t first_row;
  std::uint64_t second_row;
};

// Exact immutable RCP-072/RCP-074 graph vector inventory. The result counts,
// diagnostic classes, ordering, cleanup, no-access/no-root, fallback, and
// replay assertions below are driven by these twelve signed pairs.
constexpr std::array<Vector, 12> kVectors{{
    {"VEC-FAMILY-GRAPH-01-V1", "FIX-FAMILY-GRAPH-POSITIVE-V1",
     "positive", "not_applicable", 25, 26},
    {"VEC-FAMILY-GRAPH-02-V1", "FIX-FAMILY-GRAPH-BOUNDARY-V1",
     "boundary", "not_applicable", 26, 27},
    {"VEC-FAMILY-GRAPH-03-V1", "FIX-FAMILY-GRAPH-SEMANTIC-REFUSAL-V1",
     "semantic_refusal", "SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1", 27, 28},
    {"VEC-FAMILY-GRAPH-04-V1", "FIX-FAMILY-GRAPH-CANCELLATION-V1",
     "cancellation", "SB_MODEL_EXECUTION_CANCELLED_V1", 28, 29},
    {"VEC-FAMILY-GRAPH-05-V1", "FIX-FAMILY-GRAPH-FAULT-V1", "fault",
     "SB_MODEL_COORDINATOR_LEG_FAILED_V1", 29, 30},
    {"VEC-FAMILY-GRAPH-06-V1", "FIX-FAMILY-GRAPH-STALE-GENERATION-V1",
     "stale_generation", "SB_MODEL_CATALOG_GENERATION_STALE_V1", 30, 31},
    {"VEC-FAMILY-GRAPH-07-V1", "FIX-FAMILY-GRAPH-DESCRIPTOR-MISMATCH-V1",
     "descriptor_mismatch", "SB_MODEL_TYPED_EXCHANGE_INVALID_V1", 31, 32},
    {"VEC-FAMILY-GRAPH-08-V1", "FIX-FAMILY-GRAPH-SECURITY-REDACTION-V1",
     "security_redaction", "SB_MODEL_SECURITY_ADMISSION_REFUSED_V1", 32, 33},
    {"VEC-FAMILY-GRAPH-09-V1",
     "FIX-FAMILY-GRAPH-MGA-CONTEXT-SUBSTITUTION-V1",
     "mga_context_substitution", "SB_MODEL_MGA_CONTEXT_MISMATCH_V1", 33, 34},
    {"VEC-FAMILY-GRAPH-10-V1", "FIX-FAMILY-GRAPH-RESOURCE-EXHAUSTION-V1",
     "resource_exhaustion", "SB_MODEL_RESOURCE_MEMORY_REFUSED_V1", 34, 35},
    {"VEC-FAMILY-GRAPH-11-V1", "FIX-FAMILY-GRAPH-EXACT-FALLBACK-V1",
     "exact_fallback", "not_applicable", 35, 36},
    {"VEC-FAMILY-GRAPH-12-V1",
     "FIX-FAMILY-GRAPH-DETERMINISTIC-REPLAY-V1", "deterministic_replay",
     "not_applicable", 36, 37},
}};

std::string Uuid(const std::uint64_t value) {
  char buffer[37];
  std::snprintf(buffer, sizeof(buffer), "00000000-0000-4000-8000-%012llx",
                static_cast<unsigned long long>(value));
  return buffer;
}

bool Require(const bool condition, const std::string_view detail) {
  if (!condition) std::cerr << "QOW-CES05-GRAPH: " << detail << '\n';
  return condition;
}

exec::PhysicalMgaStatementContext Mga(const std::uint64_t identity = 1) {
  exec::PhysicalMgaStatementContext context;
  context.statement_uuid = Uuid(100 + identity);
  context.owning_transaction_uuid = Uuid(200 + identity);
  context.statement_snapshot_uuid = Uuid(300 + identity);
  context.statement_metadata_snapshot_uuid = Uuid(400 + identity);
  context.owning_local_transaction_id = 5;
  context.visible_committed_high_watermark = 20;
  context.oldest_active_transaction_id = 2;
  context.oldest_interesting_transaction_id = 3;
  context.oldest_snapshot_transaction_id = 3;
  context.retention_horizon_transaction_id = 3;
  context.active_excluded_local_transaction_ids = {5, 9};
  context.in_doubt_excluded_local_transaction_ids = {8};
  context.snapshot_kind = "statement_stable";
  context.publication_inventory_next_local_transaction_id = 30;
  context.inventory_authoritative = true;
  context.complete = true;
  context.current = true;
  return context;
}

plan::CanonicalLogicalRelationalGraph LogicalGraphExpandIdentity() {
  const auto physical = Mga(74);
  plan::CanonicalMgaStatementContext mga;
  mga.statement_uuid = physical.statement_uuid;
  mga.owning_transaction_uuid = physical.owning_transaction_uuid;
  mga.statement_snapshot_uuid = physical.statement_snapshot_uuid;
  mga.statement_metadata_snapshot_uuid =
      physical.statement_metadata_snapshot_uuid;
  mga.owning_local_transaction_id = physical.owning_local_transaction_id;
  mga.visible_committed_high_watermark =
      physical.visible_committed_high_watermark;
  mga.oldest_active_transaction_id = physical.oldest_active_transaction_id;
  mga.oldest_interesting_transaction_id =
      physical.oldest_interesting_transaction_id;
  mga.oldest_snapshot_transaction_id =
      physical.oldest_snapshot_transaction_id;
  mga.retention_horizon_transaction_id =
      physical.retention_horizon_transaction_id;
  mga.active_excluded_local_transaction_ids =
      physical.active_excluded_local_transaction_ids;
  mga.in_doubt_excluded_local_transaction_ids =
      physical.in_doubt_excluded_local_transaction_ids;
  mga.snapshot_kind = physical.snapshot_kind;
  mga.publication_inventory_next_local_transaction_id =
      physical.publication_inventory_next_local_transaction_id;
  mga.inventory_authoritative = physical.inventory_authoritative;
  mga.complete = physical.complete;
  mga.current = physical.current;

  plan::CanonicalLogicalRelationalNode node;
  node.logical_node_id = 1;
  node.node_kind =
      plan::CanonicalLogicalRelationalNodeKind::kRelationSource;
  node.output_descriptor_ids = {101};
  node.bound_expression_ids = {201};
  node.origin_relational_node_ids = {1};
  node.required_object_uuids = {Uuid(7401)};
  node.semantic_variant_id = "SBLR_MODEL_EXPAND_V1";
  node.model_family_identity =
      plan::CanonicalLogicalModelFamilyIdentity::kGraph;

  plan::CanonicalLogicalRelationalGraph graph;
  graph.bound_sblr_tree_uuid = Uuid(7402);
  graph.catalog_epoch_uuid = Uuid(7403);
  graph.security_context_uuid = Uuid(7404);
  graph.local_transaction_id = mga.owning_local_transaction_id;
  graph.statement_snapshot_id = mga.visible_committed_high_watermark;
  graph.mga_statement_context = std::move(mga);
  graph.root_logical_node_id = 1;
  graph.result_descriptor_ids = {101};
  graph.nodes = {std::move(node)};
  return graph;
}

bool LogicalGraphFamilyIdentityMutations() {
  bool passed = true;
  const auto accepted = plan::ValidateCanonicalLogicalRelationalGraph(
      LogicalGraphExpandIdentity());
  passed &= Require(accepted.accepted && accepted.validated_node_count == 1,
                    "exact graph expand logical family identity was refused");
  const auto expect_refusal = [&](auto mutation,
                                  const std::string_view detail) {
    auto graph = LogicalGraphExpandIdentity();
    mutation(graph.nodes.front());
    const auto refused =
        plan::ValidateCanonicalLogicalRelationalGraph(graph);
    return Require(!refused.accepted && refused.validated_node_count == 0,
                   detail);
  };
  passed &= expect_refusal(
      [](auto& node) { node.required_object_uuids.clear(); },
      "object-free graph expand logical identity was admitted");
  passed &= expect_refusal(
      [](auto& node) {
        node.model_family_identity =
            plan::CanonicalLogicalModelFamilyIdentity::kDocument;
      },
      "object-backed document family substitution was admitted");
  passed &= expect_refusal(
      [](auto& node) {
        node.model_family_identity =
            plan::CanonicalLogicalModelFamilyIdentity::kUnspecified;
      },
      "object-backed default family substitution was admitted");
  passed &= expect_refusal(
      [](auto& node) {
        node.semantic_variant_id = "values.literal-table.v1";
      },
      "graph family identity on a non-model semantic was admitted");
  passed &= expect_refusal(
      [](auto& node) {
        node.model_family_identity =
            static_cast<plan::CanonicalLogicalModelFamilyIdentity>(255);
      },
      "unknown canonical logical model family was admitted");
  return passed;
}

api::EngineDescriptor Descriptor(const std::uint64_t identity,
                                 const std::string& type,
                                 const bool nullable) {
  api::EngineDescriptor descriptor;
  descriptor.descriptor_uuid.canonical = Uuid(identity);
  descriptor.descriptor_kind = "scalar";
  descriptor.canonical_type_name = type;
  descriptor.encoded_descriptor =
      "type_uuid=" + Uuid(identity + 100) + ";nullability=" +
      (nullable ? "nullable" : "non_null");
  return descriptor;
}

api::EngineTypedValue Value(const api::EngineDescriptor& descriptor,
                            std::string value) {
  return {descriptor, std::move(value), false};
}

opt::ModelFamilyCoordinatorRequestV1 Planning(const Vector& vector) {
  opt::ModelFamilyCoordinatorRequestV1 request;
  request.family_id = "graph";
  request.operation_id = "GRAPH_MATCH";
  request.logical_operator_id = "LOGICAL_GRAPH_SOURCE_V1";
  request.logical_node_id = 1;
  request.object_uuid = Uuid(7401);
  request.output_descriptor_ids = {101, 102, 103};
  request.mga_statement_context = Mga();
  request.bound_sblr_tree_uuid = Uuid(7402);
  request.catalog_epoch_uuid = Uuid(7403);
  request.security_context_uuid = Uuid(7404);
  request.capability_snapshot_uuid = Uuid(7405);
  request.resource_snapshot_uuid = Uuid(7406);
  request.statistics_snapshot_uuid = Uuid(7407);
  request.route_snapshot_uuid = Uuid(7408);
  request.catalog_generation = 7;
  request.current_catalog_generation = 7;
  request.security_epoch = 7;
  request.policy_epoch = 7;
  request.resource_epoch = 7;
  request.statistics_generation = 7;
  request.route_epoch = 7;
  request.route_generation = 7;
  request.memory_budget_bytes = 1U << 20U;
  request.security_admitted = true;
  opt::ModelFamilyCandidateV1 candidate;
  candidate.alternative_uuid = Uuid(7410);
  candidate.provider_uuid = Uuid(7411);
  candidate.capability_uuid = Uuid(7412);
  candidate.implementation_id = "physical_graph_adjacency_scan_v1";
  candidate.provider_generation = 7;
  candidate.available = true;
  candidate.exact = true;
  candidate.cost.cost_vector_uuid = Uuid(7413);
  candidate.cost.cpu_units = 2;
  candidate.cost.sequential_read_units = 1;
  candidate.cost.memory_bytes_required = 4096;
  request.candidates.push_back(candidate);
  auto fallback = candidate;
  fallback.alternative_uuid = Uuid(7420);
  fallback.provider_uuid = Uuid(7421);
  fallback.capability_uuid = Uuid(7422);
  fallback.cost.cost_vector_uuid = Uuid(7423);
  fallback.cost.cpu_units = 3;
  fallback.available = false;
  fallback.exact_collection_fallback = true;
  request.candidates.push_back(fallback);
  if (std::string_view(vector.case_class) == "semantic_refusal") {
    request.operation_id = "GRAPH_OPAQUE_TEXT";
  } else if (std::string_view(vector.case_class) == "stale_generation") {
    request.current_catalog_generation = 8;
  } else if (std::string_view(vector.case_class) == "security_redaction") {
    request.security_admitted = false;
  } else if (std::string_view(vector.case_class) == "resource_exhaustion") {
    request.memory_budget_bytes = 1;
  } else if (std::string_view(vector.case_class) == "exact_fallback") {
    request.candidates.front().available = false;
    request.candidates.back().available = true;
  }
  return request;
}

exec::ModelFamilyExecutionRequestV1 Execution(const Vector& vector,
                                              const bool empty = false) {
  exec::ModelFamilyExecutionRequestV1 request;
  auto& input = request.input;
  input.family_id = "graph";
  input.operation_id = "GRAPH_MATCH";
  input.object_uuid = Uuid(7401);
  input.physical_node_id = 1;
  input.selected_alternative_uuid = Uuid(7410);
  input.capability_uuid = Uuid(7412);
  input.provider_uuid = Uuid(7411);
  input.provider_generation = 7;
  input.result_handle_uuid = Uuid(7414);
  input.causal_counter_id = 1;
  input.output_descriptor_ids = {101, 102, 103};
  input.mga_statement_context = Mga();
  input.catalog_epoch_uuid = Uuid(7403);
  input.security_context_uuid = Uuid(7404);
  input.policy_snapshot_uuid = Uuid(7415);
  input.resource_contract_uuid = Uuid(7416);
  input.catalog_generation = 7;
  input.descriptor_generation = 7;
  input.security_generation = 7;
  input.policy_generation = 7;
  input.resource_generation = 7;
  input.maximum_rows = 2;
  input.maximum_cells = 6;
  input.maximum_memory_bytes = 1U << 20U;
  request.capability.capability_uuid = input.capability_uuid;
  request.capability.family_id = "graph";
  request.capability.provider_uuid = input.provider_uuid;
  request.capability.provider_generation = input.provider_generation;
  request.capability.available = true;
  request.capability.exact = true;
  request.capability.exact_collection_fallback_available = true;
  request.capability.cancellation_supported = true;
  request.capability.cleanup_supported = true;
  request.capability.residual_recheck_supported = true;
  request.capability.base_row_mga_recheck_supported = true;
  request.capability.security_recheck_supported = true;
  request.security_admitted = true;
  request.current_catalog_generation = 7;
  request.current_descriptor_generation = 7;
  request.current_security_generation = 7;
  request.current_policy_generation = 7;
  request.current_resource_generation = 7;
  request.current_provider_generation = 7;
  request.current_mga_statement_context = input.mga_statement_context;
  request.cancellation_requested = [] { return false; };
  request.cleanup_provider = [] {};
  request.execute_provider = [=](const auto& selected) {
    exec::ModelProviderExecutionResultV1 result;
    result.ok = true;
    result.data_access_observed = true;
    result.rows_examined = empty ? 0 : 2;
    const auto row_uuid = Descriptor(101, "uuid", false);
    const auto join_key = Descriptor(102, "int64", true);
    const auto payload = Descriptor(103, "text", true);
    auto& batch = result.provider_batch;
    batch.provider_uuid = selected.provider_uuid;
    batch.provider_generation = selected.provider_generation;
    batch.result_handle_uuid = selected.result_handle_uuid;
    batch.causal_counter_id = selected.causal_counter_id;
    batch.output_descriptor_ids = selected.output_descriptor_ids;
    batch.batch.columns = {{"row_uuid", row_uuid, false, 101},
                           {"join_key", join_key, true, 102},
                           {"payload", payload, true, 103}};
    if (!empty) {
      batch.batch.rows = {
          {{Value(row_uuid, Uuid(vector.first_row)), Value(join_key, "1"),
            Value(payload, "graph-one")}},
          {{Value(row_uuid, Uuid(vector.second_row)), Value(join_key, "2"),
            Value(payload, "graph-two")}},
      };
      batch.ordered_row_identities = {
          {{}, Uuid(vector.first_row), Uuid(7501), {}, Uuid(7701)},
          {{}, Uuid(vector.second_row), Uuid(7502), {}, Uuid(7702)},
      };
    }
    batch.properties.property_uuid = Uuid(7417);
    batch.properties.ordering_id = "fixture_order";
    batch.properties.partitioning_id = "single_local_partition";
    batch.properties.uniqueness_id = "path_uuid";
    batch.properties.exact = true;
    batch.properties.residual_recheck_complete = true;
    batch.properties.base_row_mga_recheck_complete = true;
    batch.properties.security_recheck_complete = true;
    batch.mga_statement_context = selected.mga_statement_context;
    batch.security_receipt_uuid = Uuid(7418);
    batch.residual_recheck_complete = true;
    batch.base_row_mga_recheck_complete = true;
    batch.security_recheck_complete = true;
    return result;
  };
  if (std::string_view(vector.case_class) == "descriptor_mismatch") {
    request.current_descriptor_generation = 8;
  } else if (std::string_view(vector.case_class) ==
             "mga_context_substitution") {
    request.current_mga_statement_context = Mga(2);
  } else if (std::string_view(vector.case_class) == "fault") {
    request.fault_injected = true;
  } else if (std::string_view(vector.case_class) == "exact_fallback") {
    request.exact_fallback_selected = true;
  }
  return request;
}

exec::ModelFamilyExecutionRequestV1 ExecutionForSelectedPlan(
    const Vector& vector,
    const opt::ModelFamilyCoordinatorResultV1& planned,
    const bool empty = false) {
  auto request = Execution(vector, empty);
  const auto& selected = planned.selected_candidate;
  request.input.selected_alternative_uuid = selected.alternative_uuid;
  request.input.capability_uuid = selected.capability_uuid;
  request.input.provider_uuid = selected.provider_uuid;
  request.input.provider_generation = selected.provider_generation;
  request.capability.capability_uuid = selected.capability_uuid;
  request.capability.provider_uuid = selected.provider_uuid;
  request.capability.provider_generation = selected.provider_generation;
  request.current_provider_generation = selected.provider_generation;
  request.exact_fallback_selected = planned.exact_fallback_selected;
  return request;
}

std::string StablePlanBytes(
    const opt::ModelFamilyCoordinatorResultV1& planned) {
  std::ostringstream bytes;
  const auto& selected = planned.selected_candidate;
  bytes << planned.logical_operator_id << '|' << planned.physical_operator_id
        << '|' << planned.exact_fallback_selected << '|'
        << selected.alternative_uuid << '|' << selected.provider_uuid << '|'
        << selected.capability_uuid << '|' << selected.provider_generation
        << '|' << selected.cost.cost_vector_uuid << '|'
        << selected.cost.cpu_units << '|'
        << selected.cost.sequential_read_units << '|'
        << selected.cost.random_read_units << '|'
        << selected.cost.memory_bytes_required << '|'
        << selected.cost.uncertainty_penalty << '|'
        << selected.cost.risk_penalty << '|'
        << planned.physical_dag.selected_plan_uuid << '|'
        << planned.physical_dag.root_physical_node_id;
  for (const auto& node : planned.physical_dag.nodes) {
    bytes << '|' << node.physical_node_id << '|' << node.causal_counter_id
          << '|' << node.selected_alternative_uuid << '|'
          << node.executor_capability_uuid << '|' << node.cost_vector_uuid
          << '|' << node.memory_bytes_required;
  }
  return bytes.str();
}

std::string StableExecutionBytes(
    const exec::ModelFamilyExecutionResultV1& result) {
  std::ostringstream bytes;
  bytes << result.output.family_id << '|' << result.output.operation_id << '|'
        << result.output.selected_alternative_uuid << '|'
        << result.output.provider_generation << '|'
        << result.output.causal_counter_id;
  for (const auto& identity : result.output.ordered_row_identities) {
    bytes << '|' << identity.row_uuid << '|' << identity.vertex_uuid << '|'
          << identity.edge_uuid << '|' << identity.path_uuid << '|'
          << identity.graph_depth;
  }
  for (const auto& row : result.output.batch.rows) {
    for (const auto& value : row.values) bytes << '|' << value.encoded_value;
  }
  return bytes.str();
}

bool SuccessVector(const Vector& vector, const bool empty,
                   const bool fallback) {
  const auto planned = opt::CoordinateModelFamilySourceV1(Planning(vector));
  bool passed = true;
  passed &= Require(planned.accepted && planned.selected &&
                        planned.data_access_allowed && planned.deterministic &&
                        planned.logical_operator_id ==
                            "LOGICAL_GRAPH_SOURCE_V1" &&
                        planned.physical_operator_id ==
                            "PHYSICAL_GRAPH_ADJACENCY_SCAN_V1" &&
                        planned.exact_fallback_selected == fallback,
                    std::string(vector.id) + " planning outcome drifted");
  const auto executed = exec::ExecuteModelFamilySourceV1(
      ExecutionForSelectedPlan(vector, planned, empty));
  passed &= Require(executed.accepted && executed.execution_started &&
                        executed.data_access_observed &&
                        executed.root_published && executed.cleanup_complete &&
                        executed.cleanup_count == 1 &&
                        executed.output.batch.rows.size() == (empty ? 0 : 2) &&
                        executed.output.exact_exchange_validated &&
                        executed.output.selected_alternative_uuid ==
                            planned.selected_candidate.alternative_uuid &&
                        executed.output.capability_uuid ==
                            planned.selected_candidate.capability_uuid &&
                        executed.output.provider_uuid ==
                            planned.selected_candidate.provider_uuid &&
                        executed.output.provider_generation ==
                            planned.selected_candidate.provider_generation,
                    std::string(vector.id) + " execution outcome drifted");
  return passed;
}

bool RefusalVectors() {
  bool passed = true;
  for (const auto ordinal : {2U, 5U, 7U, 9U}) {
    const auto& vector = kVectors[ordinal];
    const auto planned = opt::CoordinateModelFamilySourceV1(Planning(vector));
    passed &= Require(!planned.accepted && !planned.data_access_allowed &&
                          planned.diagnostic_id == vector.expected_diagnostic,
                      std::string(vector.id) + " planning refusal drifted");
  }

  auto cancelled_request = Execution(kVectors[3]);
  std::size_t cancellation_probes = 0;
  cancelled_request.cancellation_requested = [&] {
    return ++cancellation_probes >= 2;
  };
  const auto cancelled =
      exec::ExecuteModelFamilySourceV1(cancelled_request);
  passed &= Require(!cancelled.accepted && cancelled.execution_started &&
                        cancelled.data_access_observed &&
                        !cancelled.root_published &&
                        cancelled.diagnostic_id ==
                            kVectors[3].expected_diagnostic &&
                        cancelled.cleanup_complete &&
                        cancelled.cleanup_count == 1,
                    "VEC-FAMILY-GRAPH-04-V1 cancellation outcome drifted");

  const auto faulted =
      exec::ExecuteModelFamilySourceV1(Execution(kVectors[4]));
  passed &= Require(!faulted.accepted && faulted.execution_started &&
                        !faulted.data_access_observed &&
                        !faulted.root_published &&
                        faulted.diagnostic_id == kVectors[4].expected_diagnostic &&
                        faulted.cleanup_complete && faulted.cleanup_count == 1,
                    "VEC-FAMILY-GRAPH-05-V1 failure outcome drifted");

  const auto descriptor =
      exec::ExecuteModelFamilySourceV1(Execution(kVectors[6]));
  passed &= Require(!descriptor.accepted && !descriptor.execution_started &&
                        !descriptor.data_access_observed &&
                        descriptor.diagnostic_id ==
                            kVectors[6].expected_diagnostic &&
                        descriptor.cleanup_count == 0,
                    "VEC-FAMILY-GRAPH-07-V1 descriptor refusal drifted");

  const auto mga = exec::ExecuteModelFamilySourceV1(Execution(kVectors[8]));
  passed &= Require(!mga.accepted && !mga.execution_started &&
                        !mga.data_access_observed &&
                        mga.diagnostic_id == kVectors[8].expected_diagnostic &&
                        mga.cleanup_count == 0,
                    "VEC-FAMILY-GRAPH-09-V1 MGA refusal drifted");
  return passed;
}

bool ReplayAndGraphIdentity() {
  bool passed = true;
  const auto first_plan =
      opt::CoordinateModelFamilySourceV1(Planning(kVectors[11]));
  const auto second_plan =
      opt::CoordinateModelFamilySourceV1(Planning(kVectors[11]));
  const auto first = exec::ExecuteModelFamilySourceV1(
      ExecutionForSelectedPlan(kVectors[11], first_plan));
  const auto second = exec::ExecuteModelFamilySourceV1(
      ExecutionForSelectedPlan(kVectors[11], second_plan));
  passed &= Require(first_plan.accepted && second_plan.accepted &&
                        first_plan.deterministic && second_plan.deterministic &&
                        !StablePlanBytes(first_plan).empty() &&
                        StablePlanBytes(first_plan) ==
                            StablePlanBytes(second_plan) &&
                        first.accepted && second.accepted &&
                        !StableExecutionBytes(first).empty() &&
                        StableExecutionBytes(first) == StableExecutionBytes(second),
                    "VEC-FAMILY-GRAPH-12-V1 plan/execution replay digest drifted");

  auto match_seed = Execution(kVectors[0]);
  const auto provider = match_seed.execute_provider;
  match_seed.execute_provider = [provider](const auto& input) {
    auto result = provider(input);
    result.provider_batch.ordered_row_identities.front().edge_uuid.clear();
    return result;
  };
  passed &= Require(exec::ExecuteModelFamilySourceV1(match_seed).accepted,
                    "GRAPH_MATCH seed vertex required a synthetic edge UUID");

  auto expand_seed = match_seed;
  expand_seed.input.operation_id = "GRAPH_EXPAND";
  passed &= Require(exec::ExecuteModelFamilySourceV1(expand_seed).accepted,
                    "GRAPH_EXPAND depth-zero seed refused an empty edge UUID");

  auto expand_depth_zero_with_edge = expand_seed;
  expand_depth_zero_with_edge.execute_provider = [provider](const auto& input) {
    auto result = provider(input);
    result.provider_batch.ordered_row_identities.front().edge_uuid = Uuid(7601);
    return result;
  };
  passed &= Require(
      !exec::ExecuteModelFamilySourceV1(expand_depth_zero_with_edge).accepted,
      "depth-zero GRAPH_EXPAND admitted a nonempty edge UUID");

  auto expand_positive = expand_seed;
  expand_positive.execute_provider = [provider](const auto& input) {
    auto result = provider(input);
    for (std::size_t ordinal = 0;
         ordinal < result.provider_batch.ordered_row_identities.size();
         ++ordinal) {
      auto& identity = result.provider_batch.ordered_row_identities[ordinal];
      identity.edge_uuid = Uuid(7601 + ordinal);
      identity.graph_depth = 1;
    }
    return result;
  };
  passed &= Require(exec::ExecuteModelFamilySourceV1(expand_positive).accepted,
                    "positive-depth GRAPH_EXPAND refused canonical edge UUIDs");

  auto expand_positive_depth = expand_positive;
  const auto positive_provider = expand_positive.execute_provider;
  expand_positive_depth.execute_provider =
      [positive_provider](const auto& input) {
    auto result = positive_provider(input);
    result.provider_batch.ordered_row_identities.front().edge_uuid.clear();
    return result;
  };
  const auto expand_missing_edge =
      exec::ExecuteModelFamilySourceV1(expand_positive_depth);
  passed &= Require(!expand_missing_edge.accepted &&
                        expand_missing_edge.diagnostic_id ==
                            "SB_MODEL_TYPED_EXCHANGE_INVALID_V1",
                    "positive-depth GRAPH_EXPAND admitted an empty edge UUID");

  auto match_positive_depth = Execution(kVectors[0]);
  const auto match_provider = match_positive_depth.execute_provider;
  match_positive_depth.execute_provider = [match_provider](const auto& input) {
    auto result = match_provider(input);
    result.provider_batch.ordered_row_identities.front().graph_depth = 1;
    return result;
  };
  passed &= Require(
      !exec::ExecuteModelFamilySourceV1(match_positive_depth).accepted,
      "GRAPH_MATCH admitted a positive-depth row identity");

  auto row_value_substitution = Execution(kVectors[0]);
  row_value_substitution.execute_provider = [provider](const auto& input) {
    auto result = provider(input);
    result.provider_batch.batch.rows.front().values.front().encoded_value =
        Uuid(7991);
    return result;
  };
  const auto row_value_refused =
      exec::ExecuteModelFamilySourceV1(row_value_substitution);
  passed &= Require(
      !row_value_refused.accepted && !row_value_refused.root_published &&
          row_value_refused.diagnostic_id ==
              "SB_MODEL_TYPED_EXCHANGE_INVALID_V1",
      "graph row_uuid value substitution reached root publication");

  auto incomplete_identity = Execution(kVectors[0]);
  incomplete_identity.execute_provider = [provider](const auto& input) {
    auto result = provider(input);
    result.provider_batch.ordered_row_identities.pop_back();
    return result;
  };
  const auto incomplete_identity_refused =
      exec::ExecuteModelFamilySourceV1(incomplete_identity);
  passed &= Require(
      !incomplete_identity_refused.accepted &&
          !incomplete_identity_refused.root_published &&
          incomplete_identity_refused.diagnostic_id ==
              "SB_MODEL_TYPED_EXCHANGE_INVALID_V1",
      "incomplete graph identity cohort was misclassified or published");

  auto direction_value = Execution(kVectors[0]);
  direction_value.execute_provider = [provider](const auto& input) {
    auto result = provider(input);
    const auto direction = Descriptor(103, "text", false);
    result.provider_batch.batch.columns[2] =
        {"direction", direction, false, 103};
    for (auto& row : result.provider_batch.batch.rows) {
      row.values[2] = Value(direction, "sideways");
    }
    return result;
  };
  const auto direction_refused =
      exec::ExecuteModelFamilySourceV1(direction_value);
  passed &= Require(
      !direction_refused.accepted && !direction_refused.root_published &&
          direction_refused.diagnostic_id ==
              "SB_MODEL_TYPED_EXCHANGE_INVALID_V1",
      "unknown graph direction value reached root publication");

  auto oversized_exchange = Execution(kVectors[0]);
  oversized_exchange.input.maximum_memory_bytes = 1;
  const auto oversized_refused =
      exec::ExecuteModelFamilySourceV1(oversized_exchange);
  passed &= Require(
      !oversized_refused.accepted && !oversized_refused.root_published &&
          oversized_refused.diagnostic_id ==
              "SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
      "oversized graph provider batch reached root publication");

  auto unavailable = Execution(kVectors[10]);
  unavailable.exact_fallback_selected = true;
  unavailable.capability.exact_collection_fallback_available = false;
  const auto refused = exec::ExecuteModelFamilySourceV1(unavailable);
  passed &= Require(!refused.accepted && !refused.execution_started &&
                        refused.diagnostic_id ==
                            "SB_MODEL_GRAPH_EXACT_FALLBACK_UNAVAILABLE_V1",
                    "graph unavailable fallback diagnostic drifted");
  auto shortest_path = Execution(kVectors[0]);
  shortest_path.input.operation_id = "GRAPH_SHORTEST_PATH";
  const auto shortest_path_refused =
      exec::ExecuteModelFamilySourceV1(shortest_path);
  passed &= Require(
      !shortest_path_refused.accepted &&
          !shortest_path_refused.execution_started &&
          shortest_path_refused.diagnostic_id ==
              "SB_MODEL_TYPED_EXCHANGE_INVALID_V1",
      "unsigned GRAPH_SHORTEST_PATH operation was admitted");
  return passed;
}

bool VectorInventory() {
  bool passed = true;
  for (std::size_t index = 0; index < kVectors.size(); ++index) {
    passed &= Require(std::string_view(kVectors[index].id).starts_with(
                          "VEC-FAMILY-GRAPH-"),
                      "graph vector ID inventory drifted");
    passed &= Require(std::string_view(kVectors[index].fixture).starts_with(
                          "FIX-FAMILY-GRAPH-"),
                      "graph fixture ID inventory drifted");
  }
  return passed;
}

#if defined(SB_CES05_GRAPH_PRODUCTION_QUERY_ROUTE)
std::uint64_t ProductionSeed() {
  static std::uint64_t ordinal = 0;
  return static_cast<std::uint64_t>(
             std::chrono::duration_cast<std::chrono::milliseconds>(
                 std::chrono::system_clock::now().time_since_epoch())
                 .count()) +
         ++ordinal;
}

platform::TypedUuid ProductionUuid(const platform::UuidKind kind) {
  return uuid::GenerateEngineIdentityV7(kind, ProductionSeed()).value;
}

std::string ProductionUuidText(const platform::UuidKind kind) {
  return uuid::UuidToString(ProductionUuid(kind).value);
}

struct ProductionFixture {
  std::filesystem::path directory;
  std::filesystem::path database_path;
  std::string database_uuid;
  std::string schema_uuid;
  std::string graph_uuid;
  api::MgaRelationStorageDescriptor graph_descriptor;

  ~ProductionFixture() {
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
  }
};

bool MakeProductionFixture(ProductionFixture* fixture) {
  fixture->directory =
      std::filesystem::temp_directory_path() /
      ("scratchbird_rcp074_graph_" + std::to_string(ProductionSeed()));
  std::error_code error;
  if (!std::filesystem::create_directories(fixture->directory, error) ||
      error) {
    return Require(false, "production graph fixture directory creation failed");
  }
  fixture->database_path = fixture->directory / "graph.sbdb";
  db::DatabaseCreateConfig create;
  create.path = fixture->database_path.string();
  create.database_uuid = ProductionUuid(platform::UuidKind::database);
  create.filespace_uuid = ProductionUuid(platform::UuidKind::filespace);
  create.creation_unix_epoch_millis = ProductionSeed();
  create.require_resource_seed_pack = false;
  create.allow_minimal_resource_bootstrap = true;
  create.allow_overwrite = true;
  if (!db::CreateDatabaseFile(create).ok()) {
    return Require(false, "production graph database creation failed");
  }
  fixture->database_uuid = uuid::UuidToString(create.database_uuid.value);
  fixture->schema_uuid = ProductionUuidText(platform::UuidKind::schema);
  fixture->graph_uuid = ProductionUuidText(platform::UuidKind::object);
  return true;
}

api::EngineRequestContext ProductionBaseContext(
    const ProductionFixture& fixture, std::string request_id) {
  api::EngineRequestContext context;
  context.trust_mode = api::EngineTrustMode::server_isolated;
  context.request_id = std::move(request_id);
  context.database_path = fixture.database_path.string();
  context.database_uuid.canonical = fixture.database_uuid;
  context.principal_uuid.canonical =
      ProductionUuidText(platform::UuidKind::principal);
  context.session_uuid.canonical =
      ProductionUuidText(platform::UuidKind::object);
  context.security_context_present = true;
  context.identifier_profile_uuid = "sbsql_v3";
  context.language_context.language_tag = "en";
  context.language_context.default_language_tag = "en";
  context.catalog_generation_id = 74;
  context.security_epoch = 75;
  context.resource_epoch = 76;
  context.name_resolution_epoch = 77;
  return context;
}

bool BeginProductionTransaction(const ProductionFixture& fixture,
                                std::string request_id,
                                api::EngineRequestContext* context) {
  api::EngineBeginTransactionRequest request;
  request.context = ProductionBaseContext(fixture, std::move(request_id));
  request.isolation_level = "read_committed";
  const auto begun = api::EngineBeginTransaction(request);
  if (!begun.ok) {
    return Require(false, "production graph transaction begin failed");
  }
  *context = request.context;
  context->transaction_uuid = begun.transaction_uuid;
  context->local_transaction_id = begun.local_transaction_id;
  context->snapshot_visible_through_local_transaction_id =
      begun.snapshot_visible_through_local_transaction_id;
  context->transaction_isolation_level = begun.isolation_level;
  return true;
}

bool CommitProductionTransaction(const api::EngineRequestContext& context) {
  api::EngineCommitTransactionRequest request;
  request.context = context;
  return Require(api::EngineCommitTransaction(request).ok,
                 "production graph transaction commit failed");
}

bool RollbackProductionTransaction(const api::EngineRequestContext& context) {
  api::EngineRollbackTransactionRequest request;
  request.context = context;
  return Require(api::EngineRollbackTransaction(request).ok,
                 "production graph transaction rollback failed");
}

api::EngineLocalizedName ProductionName(std::string name) {
  return {"en", "primary", "", std::move(name), true};
}

api::EngineColumnDefinition ProductionColumn(
    const std::uint32_t ordinal,
    std::string name,
    std::string canonical_type,
    const bool nullable) {
  api::EngineColumnDefinition column;
  column.requested_column_uuid.canonical =
      ProductionUuidText(platform::UuidKind::object);
  column.names.push_back(ProductionName(std::move(name)));
  column.descriptor.descriptor_kind = "scalar";
  column.descriptor.canonical_type_name = canonical_type;
  column.descriptor.encoded_descriptor =
      "canonical=" + std::move(canonical_type);
  column.ordinal = ordinal;
  column.nullable = nullable;
  return column;
}

void AddProductionAuthorization(api::EngineRequestContext* context,
                                const std::string& right,
                                const std::string& target_uuid) {
  if (!context->authorization_context.present) {
    context->authorization_context.present = true;
    context->authorization_context.authority_uuid.canonical =
        ProductionUuidText(platform::UuidKind::object);
    context->authorization_context.principal_uuid = context->principal_uuid;
    context->authorization_context.security_epoch = context->security_epoch;
    context->authorization_context.policy_epoch = 78;
    context->authorization_context.catalog_generation_id =
        context->catalog_generation_id;
    context->authorization_context.effective_subjects.push_back(
        {context->principal_uuid, "principal"});
  }
  api::EngineMaterializedAuthorizationGrant grant;
  grant.grant_uuid.canonical =
      ProductionUuidText(platform::UuidKind::object);
  grant.subject_uuid = context->principal_uuid;
  grant.subject_kind = "principal";
  grant.target_uuid.canonical = target_uuid;
  grant.right = right;
  grant.security_epoch = context->security_epoch;
  context->authorization_context.grants.push_back(std::move(grant));
}

bool CreateProductionGraph(ProductionFixture* fixture,
                           const api::EngineRequestContext& context) {
  api::EngineCreateSchemaRequest schema;
  schema.context = context;
  schema.target_object.uuid.canonical = fixture->schema_uuid;
  schema.target_object.object_kind = "schema";
  schema.localized_names.push_back(ProductionName("graph_schema"));
  if (!api::EngineCreateSchema(schema).ok) {
    return Require(false, "production graph schema creation failed");
  }
  api::EngineCreateTableRequest table;
  table.context = context;
  table.context.current_schema_uuid.canonical.clear();
  table.target_schema.uuid.canonical = fixture->schema_uuid;
  table.target_schema.object_kind = "schema";
  table.requested_table_uuid.canonical = fixture->graph_uuid;
  table.table_names.push_back(ProductionName("graph_fixture"));
  table.table_columns.push_back(
      ProductionColumn(0, "vertex_uuid", "uuid", false));
  table.table_columns.push_back(
      ProductionColumn(1, "edge_uuid", "uuid", true));
  table.table_columns.push_back(
      ProductionColumn(2, "path_uuid", "uuid", false));
  table.table_columns.push_back(
      ProductionColumn(3, "vertex_labels", "text", false));
  table.table_columns.push_back(
      ProductionColumn(4, "vertex_properties", "text", false));
  table.table_columns.push_back(
      ProductionColumn(5, "edge_properties", "text", false));
  table.table_columns.push_back(
      ProductionColumn(6, "direction", "text", false));
  table.table_columns.push_back(
      ProductionColumn(7, "depth", "uint64", false));
  table.table_columns.push_back(
      ProductionColumn(8, "cycle_policy", "text", false));
  if (!api::EngineCreateTable(table).ok) {
    return Require(false, "production graph relation creation failed");
  }
  const auto loaded =
      api::LoadMgaRelationStorageDescriptor(context, fixture->graph_uuid);
  if (!loaded.ok || loaded.descriptor.columns.size() != 9 ||
      loaded.descriptor.descriptor_generation == 0) {
    return Require(false, "production graph descriptor load failed");
  }
  fixture->graph_descriptor = loaded.descriptor;
  return true;
}

api::EngineGraphWriteResult WriteProductionGraph(
    const ProductionFixture& fixture,
    const api::EngineRequestContext& context,
    const bool orphan_endpoint = false) {
  api::EngineGraphWriteRequest request;
  request.context = context;
  request.structured_graph_persist = true;
  request.graph_object_uuid = fixture.graph_uuid;
  request.provider_generation = fixture.graph_descriptor.descriptor_generation;
  request.target_object.uuid.canonical = fixture.graph_uuid;
  request.target_object.object_kind = "graph";
  request.bound_object_identity.object_uuid.canonical = fixture.graph_uuid;
  request.bound_object_identity.resolved_object_type = "graph";
  request.bound_object_identity.resolved_schema_uuid.canonical =
      fixture.schema_uuid;
  request.bound_object_identity.catalog_generation_id =
      context.catalog_generation_id;
  request.bound_object_identity.security_epoch = context.security_epoch;
  request.bound_object_identity.resource_epoch = context.resource_epoch;
  request.vertices = {
      {Uuid(8101), {"P"}, {{"name", "one"}}},
      {Uuid(8102), {"P"}, {{"name", "two"}}},
      {Uuid(8103), {"P"}, {{"name", "three"}}},
  };
  request.edges = {
      {Uuid(8201), Uuid(8101), Uuid(8102), "NEXT", {}, 1.0},
      {Uuid(8202), Uuid(8102), Uuid(8103), "NEXT", {}, 1.0},
      {Uuid(8203), Uuid(8101), Uuid(8102), "NEXT", {}, 2.0},
  };
  if (orphan_endpoint) {
    request.edges.front().target_vertex_id = Uuid(8999);
  }
  return api::EngineGraphWrite(request);
}

std::string DescriptorField(const std::string& encoded,
                            const std::string_view key) {
  std::size_t offset = 0;
  while (offset <= encoded.size()) {
    const auto end = encoded.find(';', offset);
    const auto field = std::string_view(encoded).substr(
        offset, end == std::string::npos ? std::string::npos : end - offset);
    const auto equal = field.find('=');
    if (equal != std::string_view::npos && field.substr(0, equal) == key) {
      return std::string(field.substr(equal + 1));
    }
    if (end == std::string::npos) break;
    offset = end + 1;
  }
  return {};
}

api::TypedRelationalDag ProductionGraphDag(
    const api::EngineRequestContext& context,
    const api::MgaRelationStorageDescriptor& graph,
    const bool expand,
    const std::uint64_t minimum_depth = 1) {
  api::TypedRelationalDag dag;
  dag.wire_version = 2;
  dag.bound_sblr_tree_uuid =
      ProductionUuidText(platform::UuidKind::object);
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
    api::RelationalTypeDescriptor descriptor;
    descriptor.descriptor_id = static_cast<std::uint32_t>(ordinal + 1);
    descriptor.descriptor_uuid =
        column.value_descriptor.descriptor_uuid.canonical;
    descriptor.type_uuid =
        DescriptorField(column.value_descriptor.encoded_descriptor,
                        "type_uuid");
    descriptor.nullability =
        column.nullable ? api::RelationalNullability::kNullable
                        : api::RelationalNullability::kNonNull;
    dag.descriptors.push_back(std::move(descriptor));

    api::RelationalExpressionRecord output_expression;
    output_expression.expression_id =
        static_cast<std::uint32_t>(ordinal + 1);
    output_expression.expression_kind =
        api::RelationalExpressionKind::kIdentifier;
    output_expression.result_descriptor_id =
        static_cast<std::uint32_t>(ordinal + 1);
    output_expression.bound_name_uuid = column.column_uuid.canonical;
    dag.expressions.push_back(std::move(output_expression));
    dag.outputs.push_back(
        {static_cast<std::uint32_t>(ordinal + 1), 1,
         static_cast<std::uint32_t>(ordinal + 1),
         column.canonical_name_key, static_cast<std::uint32_t>(ordinal + 1),
         true, static_cast<std::uint32_t>(ordinal)});
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
    add_literal(12, 8, api::RelationalLiteralKind::kNumeric,
                std::to_string(minimum_depth));
    add_literal(13, 8, api::RelationalLiteralKind::kNumeric, "1");
    add_literal(14, 9, api::RelationalLiteralKind::kString, "visited_set");
    operation.operator_name = "GRAPH_EXPAND";
    operation.child_expression_ids = {10, 11, 12, 13, 14};
  } else {
    add_literal(11, 4, api::RelationalLiteralKind::kString,
                "vertex(label=P)");
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
  node.required_object_uuids = {graph.relation_uuid.canonical};
  node.semantic_variant_id =
      expand ? "SBLR_MODEL_EXPAND_V1" : "SBLR_MODEL_SOURCE_V1";
  dag.nodes.push_back(std::move(node));
  return dag;
}

api::TypedRelationalDag ProductionGraphCteDag(
    const api::EngineRequestContext& context,
    const api::MgaRelationStorageDescriptor& graph,
    const bool shareable) {
  auto dag = ProductionGraphDag(context, graph, false);
  dag.root_node_id = 2;
  api::RelationalDagNode cte;
  cte.node_id = 2;
  cte.node_kind = api::RelationalDagNodeKind::kCte;
  cte.shareable = shareable;
  cte.input_node_ids = {1};
  cte.output_descriptor_ids = dag.nodes.front().output_descriptor_ids;
  cte.semantic_variant_id = "cte.bound.v1";
  dag.nodes.push_back(std::move(cte));
  return dag;
}

std::string ProductionCoreTypeUuid(const std::string_view stable_name) {
  const auto manifest = dt::LoadCurrentCoreDatatypeCatalogManifest();
  if (!manifest.ok()) return {};
  const auto descriptor = std::ranges::find_if(
      manifest.manifest.descriptor_rows,
      [&](const auto& row) { return row.stable_name == stable_name; });
  return descriptor == manifest.manifest.descriptor_rows.end()
             ? std::string{}
             : uuid::UuidToString(descriptor->descriptor_uuid.value);
}

api::TypedRelationalDag ProductionGraphCountDag(
    const api::EngineRequestContext& context,
    const api::MgaRelationStorageDescriptor& graph) {
  auto dag = ProductionGraphDag(context, graph, false);
  dag.root_node_id = 2;
  api::RelationalTypeDescriptor count_descriptor;
  count_descriptor.descriptor_id = 10;
  count_descriptor.descriptor_uuid =
      ProductionUuidText(platform::UuidKind::object);
  count_descriptor.type_uuid = ProductionCoreTypeUuid("int64");
  count_descriptor.nullability = api::RelationalNullability::kNonNull;
  dag.descriptors.push_back(std::move(count_descriptor));
  const auto count = exec::LookupCanonicalAggregateByFunctionV1(
      exec::CanonicalAggregateFunction::count);
  api::RelationalExpressionRecord aggregate;
  aggregate.expression_id = 30;
  aggregate.expression_kind = api::RelationalExpressionKind::kFunctionCall;
  aggregate.result_descriptor_id = 10;
  if (count != nullptr) aggregate.function_uuid = count->function_uuid;
  dag.expressions.push_back(std::move(aggregate));
  dag.outputs.push_back({10, 2, 30, "graph_count", 10, true, 0});
  api::RelationalDagNode aggregate_node;
  aggregate_node.node_id = 2;
  aggregate_node.node_kind = api::RelationalDagNodeKind::kAggregate;
  aggregate_node.input_node_ids = {1};
  aggregate_node.output_descriptor_ids = {10};
  aggregate_node.bound_expression_ids = {30};
  aggregate_node.semantic_variant_id = "aggregate.global-count-star.v1";
  dag.nodes.push_back(std::move(aggregate_node));
  return dag;
}

enum class ProductionGraphRecursiveMutation {
  none,
  term_semantic,
  root_semantic,
  output_schema,
  mga_context,
};

api::TypedRelationalDag ProductionGraphRecursiveCteDag(
    const api::EngineRequestContext& context,
    const api::MgaRelationStorageDescriptor& graph,
    const std::string_view upper_bound = "5",
    const ProductionGraphRecursiveMutation mutation =
        ProductionGraphRecursiveMutation::none) {
  auto dag = ProductionGraphCountDag(context, graph);
  dag.root_node_id = 4;
  api::RelationalExpressionRecord bound;
  bound.expression_id = 31;
  bound.expression_kind = api::RelationalExpressionKind::kLiteral;
  bound.result_descriptor_id = 10;
  bound.literal_kind = api::RelationalLiteralKind::kNumeric;
  bound.literal_or_parameter_ref = std::string(upper_bound);
  dag.expressions.push_back(std::move(bound));

  api::RelationalDagNode term;
  term.node_id = 3;
  term.node_kind = api::RelationalDagNodeKind::kCte;
  term.output_descriptor_ids = {
      mutation == ProductionGraphRecursiveMutation::output_schema ? 1U
                                                                  : 10U};
  term.semantic_variant_id =
      mutation == ProductionGraphRecursiveMutation::term_semantic
          ? "cte.recursive-term.substituted.v1"
          : "cte.recursive-term-int64-increment.v1";
  dag.nodes.push_back(std::move(term));

  api::RelationalDagNode recursive;
  recursive.node_id = 4;
  recursive.node_kind = api::RelationalDagNodeKind::kRecursiveCte;
  recursive.input_node_ids = {2, 3};
  recursive.output_descriptor_ids = {10};
  recursive.bound_expression_ids = {31};
  recursive.semantic_variant_id =
      mutation == ProductionGraphRecursiveMutation::root_semantic
          ? "cte.recursive-substituted.v1"
          : "cte.recursive-union-all-int64-increment.v1";
  dag.nodes.push_back(std::move(recursive));
  if (mutation == ProductionGraphRecursiveMutation::mga_context) {
    dag.statement_snapshot_uuid =
        ProductionUuidText(platform::UuidKind::object);
  }
  return dag;
}

enum class ProductionGraphSetMutation {
  none,
  root_semantic,
  values_semantic,
  output_schema,
  input_order,
  orphan_node,
  root_output_lineage,
  literal_type,
  mga_context,
  orphan_expression,
  orphan_descriptor,
  duplicate_operation_child,
  extra_model_operation,
  operation_function_uuid,
  operation_semantic_mismatch,
  object_uuid_substitution,
};

api::TypedRelationalDag ProductionGraphSetDag(
    const api::EngineRequestContext& context,
    const api::MgaRelationStorageDescriptor& graph,
    const ProductionGraphSetMutation mutation =
        ProductionGraphSetMutation::none) {
  auto dag = ProductionGraphDag(context, graph, false);
  dag.root_node_id = 4;
  dag.outputs.push_back({20, 2, 1, "vertex_uuid", 1, true, 0});
  api::RelationalDagNode project;
  project.node_id = 2;
  project.node_kind = api::RelationalDagNodeKind::kProject;
  project.input_node_ids = {1};
  project.output_descriptor_ids = {1};
  project.bound_expression_ids = {1};
  project.semantic_variant_id = "project.select-list.v1";
  dag.nodes.push_back(std::move(project));

  api::RelationalExpressionRecord literal;
  literal.expression_id = 31;
  literal.expression_kind = api::RelationalExpressionKind::kLiteral;
  literal.result_descriptor_id = 1;
  literal.literal_kind = api::RelationalLiteralKind::kUuid;
  if (mutation == ProductionGraphSetMutation::literal_type) {
    literal.literal_kind = api::RelationalLiteralKind::kString;
  }
  literal.literal_or_parameter_ref = Uuid(8999);
  dag.expressions.push_back(std::move(literal));
  dag.values_rows.push_back({1, {31}});
  dag.outputs.push_back({21, 3, 31, "vertex_uuid", 1, true, 0});
  api::RelationalDagNode values;
  values.node_id = 3;
  values.node_kind = api::RelationalDagNodeKind::kValues;
  values.output_descriptor_ids = {1};
  values.values_row_ids = {1};
  values.semantic_variant_id =
      mutation == ProductionGraphSetMutation::values_semantic
          ? "values.substituted.v1"
          : "values.literal-table.v1";
  dag.nodes.push_back(std::move(values));

  api::RelationalDagNode set;
  set.node_id = 4;
  set.node_kind = api::RelationalDagNodeKind::kSetOperation;
  set.input_node_ids =
      mutation == ProductionGraphSetMutation::input_order
          ? std::vector<std::uint32_t>{3, 2}
          : std::vector<std::uint32_t>{2, 3};
  set.output_descriptor_ids = {
      mutation == ProductionGraphSetMutation::output_schema ? 2U : 1U};
  set.semantic_variant_id =
      mutation == ProductionGraphSetMutation::root_semantic
          ? "set-operation.union-distinct.v1"
          : "set-operation.union-all.v1";
  dag.nodes.push_back(std::move(set));
  if (mutation == ProductionGraphSetMutation::orphan_node) {
    api::RelationalDagNode orphan;
    orphan.node_id = 5;
    orphan.node_kind = api::RelationalDagNodeKind::kCte;
    orphan.output_descriptor_ids = {1};
    orphan.semantic_variant_id = "cte.bound.v1";
    dag.nodes.push_back(std::move(orphan));
  }
  if (mutation == ProductionGraphSetMutation::root_output_lineage) {
    dag.outputs.push_back({22, 4, 31, "vertex_uuid", 1, true, 0});
  }
  if (mutation == ProductionGraphSetMutation::mga_context) {
    dag.statement_snapshot_uuid =
        ProductionUuidText(platform::UuidKind::object);
  }
  if (mutation == ProductionGraphSetMutation::orphan_expression) {
    auto orphan = dag.expressions.front();
    orphan.expression_id = 32;
    orphan.bound_name_uuid = Uuid(8998);
    dag.expressions.push_back(std::move(orphan));
  }
  if (mutation == ProductionGraphSetMutation::orphan_descriptor) {
    auto orphan = dag.descriptors.front();
    orphan.descriptor_id = 99;
    orphan.descriptor_uuid = Uuid(8997);
    dag.descriptors.push_back(std::move(orphan));
  }
  if (mutation == ProductionGraphSetMutation::duplicate_operation_child) {
    const auto operation = std::ranges::find_if(
        dag.expressions, [](const auto& expression) {
          return expression.operator_name == "GRAPH_MATCH";
        });
    operation->child_expression_ids = {10, 10};
  }
  if (mutation == ProductionGraphSetMutation::extra_model_operation) {
    auto extra_operation = *std::ranges::find_if(
        dag.expressions, [](const auto& expression) {
          return expression.operator_name == "GRAPH_MATCH";
        });
    extra_operation.expression_id = 32;
    dag.expressions.push_back(std::move(extra_operation));
    api::RelationalDagNode extra_node = dag.nodes.front();
    extra_node.node_id = 5;
    extra_node.semantic_variant_id = "SBLR_MODEL_SOURCE_V1";
    dag.nodes.push_back(std::move(extra_node));
  }
  if (mutation == ProductionGraphSetMutation::operation_function_uuid) {
    const auto operation = std::ranges::find_if(
        dag.expressions, [](const auto& expression) {
          return expression.operator_name == "GRAPH_MATCH";
        });
    operation->function_uuid = Uuid(8996);
  }
  if (mutation == ProductionGraphSetMutation::operation_semantic_mismatch) {
    dag.nodes.front().semantic_variant_id = "SBLR_MODEL_EXPAND_V1";
  }
  if (mutation == ProductionGraphSetMutation::object_uuid_substitution) {
    dag.nodes.front().required_object_uuids = {Uuid(8995)};
  }
  return dag;
}

api::TypedRelationalDag ProductionGraphProjectLimitDag(
    const api::EngineRequestContext& context,
    const api::MgaRelationStorageDescriptor& graph) {
  auto dag = ProductionGraphDag(context, graph, false);
  dag.root_node_id = 3;
  dag.outputs.push_back({20, 2, 1, "vertex_uuid", 1, true, 0});
  dag.outputs.push_back({21, 2, 2, "edge_uuid", 2, true, 1});
  dag.outputs.push_back({22, 2, 3, "path_uuid", 3, true, 2});
  api::RelationalDagNode project;
  project.node_id = 2;
  project.node_kind = api::RelationalDagNodeKind::kProject;
  project.input_node_ids = {1};
  project.output_descriptor_ids = {1, 2, 3};
  project.bound_expression_ids = {1, 2, 3};
  project.semantic_variant_id = "project.select-list.v1";
  dag.nodes.push_back(std::move(project));
  api::RelationalExpressionRecord limit;
  limit.expression_id = 30;
  limit.expression_kind = api::RelationalExpressionKind::kLiteral;
  limit.result_descriptor_id = 8;
  limit.literal_kind = api::RelationalLiteralKind::kNumeric;
  limit.literal_or_parameter_ref = "2";
  dag.expressions.push_back(std::move(limit));
  api::RelationalDagNode limit_node;
  limit_node.node_id = 3;
  limit_node.node_kind = api::RelationalDagNodeKind::kLimit;
  limit_node.input_node_ids = {2};
  limit_node.output_descriptor_ids = {1, 2, 3};
  limit_node.bound_expression_ids = {30};
  limit_node.semantic_variant_id = "limit.bound-count.v1";
  dag.nodes.push_back(std::move(limit_node));
  return dag;
}

api::TypedRelationalDag ProductionGraphFilterProjectLimitDag(
    const api::EngineRequestContext& context,
    const api::MgaRelationStorageDescriptor& graph) {
  auto dag = ProductionGraphProjectLimitDag(context, graph);
  dag.root_node_id = 4;
  for (auto& node : dag.nodes) {
    if (node.node_id == 2) {
      node.node_id = 3;
      node.input_node_ids = {2};
    } else if (node.node_id == 3) {
      node.node_id = 4;
      node.input_node_ids = {3};
    }
  }
  for (auto& output : dag.outputs) {
    if (output.relation_node_id == 2) output.relation_node_id = 3;
  }
  api::RelationalTypeDescriptor boolean_descriptor;
  boolean_descriptor.descriptor_id = 10;
  boolean_descriptor.descriptor_uuid =
      ProductionUuidText(platform::UuidKind::object);
  boolean_descriptor.type_uuid = ProductionCoreTypeUuid("boolean");
  boolean_descriptor.nullability = api::RelationalNullability::kNonNull;
  dag.descriptors.push_back(std::move(boolean_descriptor));
  api::RelationalExpressionRecord predicate;
  predicate.expression_id = 31;
  predicate.expression_kind = api::RelationalExpressionKind::kLiteral;
  predicate.result_descriptor_id = 10;
  predicate.literal_kind = api::RelationalLiteralKind::kBoolean;
  predicate.literal_or_parameter_ref = "TRUE";
  dag.expressions.push_back(std::move(predicate));
  api::RelationalDagNode filter;
  filter.node_id = 2;
  filter.node_kind = api::RelationalDagNodeKind::kFilter;
  filter.input_node_ids = {1};
  filter.output_descriptor_ids = dag.nodes.front().output_descriptor_ids;
  filter.bound_expression_ids = {31};
  filter.semantic_variant_id = "filter.where.v1";
  dag.nodes.insert(dag.nodes.begin() + 1, std::move(filter));
  return dag;
}

api::TypedRelationalDag ProductionGraphSortDag(
    const api::EngineRequestContext& context,
    const api::MgaRelationStorageDescriptor& graph) {
  auto dag = ProductionGraphDag(context, graph, false);
  dag.root_node_id = 2;
  const auto ordering_uuid =
      ProductionUuidText(platform::UuidKind::object);
  api::RelationalPropertyRecord ordering;
  ordering.property_uuid = ordering_uuid;
  ordering.property_kind = api::RelationalPropertyKind::kOrdering;
  ordering.origin_node_id = 2;
  ordering.ordering_terms.push_back(
      {1, api::RelationalPropertySortDirection::kAscending,
       api::RelationalPropertyNullPlacement::kNullsLast, {}});
  dag.properties.push_back(std::move(ordering));
  api::RelationalDagNode sort;
  sort.node_id = 2;
  sort.node_kind = api::RelationalDagNodeKind::kSort;
  sort.input_node_ids = {1};
  sort.output_descriptor_ids = dag.nodes.front().output_descriptor_ids;
  sort.bound_expression_ids = {1};
  sort.semantic_variant_id = "sort.required-order.v1";
  sort.required_property_uuids = {ordering_uuid};
  sort.delivered_property_uuids = {ordering_uuid};
  dag.nodes.push_back(std::move(sort));
  return dag;
}

api::TypedRelationalDag ProductionGraphRowNumberDag(
    const api::EngineRequestContext& context,
    const api::MgaRelationStorageDescriptor& graph) {
  auto dag = ProductionGraphSortDag(context, graph);
  dag.root_node_id = 3;
  api::RelationalTypeDescriptor row_number_descriptor;
  row_number_descriptor.descriptor_id = 10;
  row_number_descriptor.descriptor_uuid =
      ProductionUuidText(platform::UuidKind::object);
  row_number_descriptor.type_uuid = ProductionCoreTypeUuid("int64");
  row_number_descriptor.nullability =
      api::RelationalNullability::kNonNull;
  dag.descriptors.push_back(std::move(row_number_descriptor));
  constexpr std::string_view kRowNumberFunctionUuid =
      "019de5fc-2400-7539-bcce-00eef3ae7220";
  api::RelationalExpressionRecord row_number;
  row_number.expression_id = 30;
  row_number.expression_kind =
      api::RelationalExpressionKind::kFunctionCall;
  row_number.result_descriptor_id = 10;
  row_number.function_uuid = std::string(kRowNumberFunctionUuid);
  dag.expressions.push_back(std::move(row_number));
  for (std::size_t ordinal = 0; ordinal < graph.columns.size(); ++ordinal) {
    dag.outputs.push_back(
        {static_cast<std::uint32_t>(20 + ordinal), 3,
         static_cast<std::uint32_t>(ordinal + 1),
         graph.columns[ordinal].canonical_name_key,
         static_cast<std::uint32_t>(ordinal + 1), true,
         static_cast<std::uint32_t>(ordinal)});
  }
  dag.outputs.push_back({40, 3, 30, "row_number", 10, true, 9});
  const auto ordering_uuid = dag.properties.front().property_uuid;
  const auto window_uuid =
      ProductionUuidText(platform::UuidKind::object);
  api::RelationalPropertyRecord window_property;
  window_property.property_uuid = window_uuid;
  window_property.property_kind = api::RelationalPropertyKind::kWindow;
  window_property.origin_node_id = 3;
  window_property.dependency_property_uuids = {ordering_uuid};
  window_property.window_frame_descriptor_uuid =
      ProductionUuidText(platform::UuidKind::object);
  dag.properties.push_back(std::move(window_property));
  api::RelationalDagNode window;
  window.node_id = 3;
  window.node_kind = api::RelationalDagNodeKind::kWindow;
  window.input_node_ids = {2};
  window.output_descriptor_ids = dag.nodes.front().output_descriptor_ids;
  window.output_descriptor_ids.push_back(10);
  window.bound_expression_ids = {1, 30};
  window.semantic_variant_id = "window.row-number.v1";
  window.required_property_uuids = {ordering_uuid};
  window.delivered_property_uuids = {ordering_uuid, window_uuid};
  dag.nodes.push_back(std::move(window));
  api::RelationalWindowDefinitionRecord definition;
  definition.window_id = 1;
  definition.relation_node_id = 3;
  definition.ordering_terms = {
      {1, api::RelationalPropertySortDirection::kAscending,
       api::RelationalPropertyNullPlacement::kNullsLast, {}}};
  dag.window_definitions.push_back(std::move(definition));
  api::RelationalWindowInvocationRecord invocation;
  invocation.invocation_id = 1;
  invocation.relation_node_id = 3;
  invocation.function_expression_id = 30;
  invocation.window_definition_id = 1;
  invocation.function_abi_version = 1;
  invocation.builtin_id = "sb.window.row_number";
  invocation.function_uuid = std::string(kRowNumberFunctionUuid);
  invocation.result_descriptor_id = 10;
  invocation.output_name_utf8 = "row_number";
  dag.window_invocations.push_back(std::move(invocation));
  return dag;
}

bool HasEvidence(const api::EngineApiResult& result,
                 const std::string_view kind,
                 const std::string_view id) {
  return std::ranges::any_of(result.evidence, [&](const auto& evidence) {
    return evidence.evidence_kind == kind && evidence.evidence_id == id;
  });
}

bool DiagnosticContains(const api::EngineApiResult& result,
                        const std::string_view token) {
  return std::ranges::any_of(result.diagnostics, [&](const auto& diagnostic) {
    return diagnostic.code.find(token) != std::string::npos ||
           diagnostic.detail.find(token) != std::string::npos;
  });
}

std::string ApiRowField(const api::EngineApiResult& result,
                        const std::size_t row_ordinal,
                        const std::string_view field_name) {
  if (row_ordinal >= result.result_shape.rows.size()) return {};
  const auto& row = result.result_shape.rows[row_ordinal];
  const auto field = std::ranges::find_if(row.fields, [&](const auto& item) {
    return item.first == field_name;
  });
  return field == row.fields.end() ? std::string{}
                                   : field->second.encoded_value;
}

api::EngineGraphPhysicalProof ProductionGraphProof() {
  api::EngineGraphPhysicalProof proof;
  proof.proof_supplied = true;
  proof.vertex_index_proof = true;
  proof.edge_index_proof = true;
  proof.adjacency_store_proof = true;
  proof.adjacency_page_proof = true;
  proof.frontier_batching_proof = true;
  proof.visited_cycle_policy_proof = true;
  proof.bidirectional_search_proof = true;
  proof.fusion_seed_proof = true;
  auto& contract = proof.provider_contract;
  contract.family = api::EngineNoSqlProviderFamily::kGraph;
  contract.scope = api::EngineNoSqlProviderScope::kLocal;
  contract.provider_id = "rcp074.local.graph.provider";
  contract.fallback_provider_id = "none";
  contract.local_provider_available = true;
  contract.descriptor_visibility.proof_present = true;
  contract.descriptor_visibility.visible_to_snapshot = true;
  contract.descriptor_visibility.descriptor_shape_compatible = true;
  contract.security_redaction.proof_present = true;
  contract.security_redaction.redaction_policy_bound = true;
  contract.security_redaction.security_snapshot_bound = true;
  contract.index_generation.proof_present = true;
  contract.index_generation.visible_to_snapshot = true;
  contract.index_generation.covers_predicate = true;
  contract.index_generation.required_generation = 74;
  contract.index_generation.available_generation = 74;
  contract.index_generation.index_uuid = Uuid(8301);
  contract.policy.proof_present = true;
  contract.policy.allowed = true;
  contract.mga_recheck.proof_present = true;
  contract.mga_recheck.row_mga_recheck_required = true;
  contract.mga_recheck.row_security_recheck_required = true;
  contract.mga_recheck.authority_source = "engine_transaction_inventory";
  return proof;
}

bool DirectGraphSafetyBoundaries(const api::EngineRequestContext& context) {
  api::EngineGraphQueryRequest request;
  request.context = context;
  request.physical_query = true;
  request.vertices = {{Uuid(8101), {"P"}, {{"name", "one"}}},
                      {Uuid(8102), {"P"}, {{"name", "two"}}}};
  request.edges = {
      {Uuid(8201), Uuid(8101), Uuid(8102), "NEXT", {}, 1.0},
      {Uuid(8203), Uuid(8101), Uuid(8102), "NEXT", {}, 2.0},
  };
  request.seed_vertex_ids = {Uuid(8101)};
  request.min_depth = 1;
  request.max_depth = 1;
  request.maximum_output_rows = 4;
  request.maximum_decoded_bytes = 1024 * 1024;
  request.physical_proof = ProductionGraphProof();
  const auto parallel = api::EngineGraphQuery(request);
  bool passed = true;
  passed &= Require(
      parallel.ok && parallel.result_shape.rows.size() == 2 &&
          ApiRowField(parallel, 0, "path_uuid") !=
              ApiRowField(parallel, 1, "path_uuid"),
      "parallel graph edges did not produce edge-aware path identities");

  auto resource = request;
  resource.maximum_decoded_bytes = 3000;
  const auto refused = api::EngineGraphQuery(resource);
  passed &= Require(
      !refused.ok && refused.result_shape.rows.empty() &&
          DiagnosticContains(refused, "SB_MODEL_RESOURCE_MEMORY_REFUSED_V1"),
      "graph traversal emitted a partial result after memory exhaustion");

  auto materialization_resource = request;
  materialization_resource.vertices[1].properties.push_back(
      {"large", std::string(30 * 1024, 'x')});
  materialization_resource.maximum_decoded_bytes = 300 * 1024;
  const auto materialization_refused =
      api::EngineGraphQuery(materialization_resource);
  passed &= Require(
      !materialization_refused.ok &&
          materialization_refused.result_shape.rows.empty() &&
          DiagnosticContains(materialization_refused,
                             "SB_MODEL_RESOURCE_MEMORY_REFUSED_V1"),
      "high-property fanout graph emitted a partial materialized result");

  auto edge_type_resource = request;
  edge_type_resource.edges[0].edge_type = std::string(30 * 1024, 'E');
  edge_type_resource.maximum_decoded_bytes = 300 * 1024;
  const auto edge_type_refused = api::EngineGraphQuery(edge_type_resource);
  passed &= Require(
      !edge_type_refused.ok && edge_type_refused.result_shape.rows.empty() &&
          DiagnosticContains(edge_type_refused,
                             "SB_MODEL_RESOURCE_MEMORY_REFUSED_V1"),
      "large graph edge type escaped the bounded result materialization gate");

  auto cancelled_request = request;
  std::size_t graph_cancellation_probes = 0;
  cancelled_request.context.query_cancellation_requested = [&] {
    return ++graph_cancellation_probes >= 12;
  };
  const auto cancelled = api::EngineGraphQuery(cancelled_request);
  passed &= Require(
      !cancelled.ok && cancelled.result_shape.rows.empty() &&
          DiagnosticContains(cancelled, "SB_MODEL_EXECUTION_CANCELLED_V1") &&
          graph_cancellation_probes >= 12,
      "mid-traversal graph cancellation published a partial result");

  auto throwing_probe = request;
  throwing_probe.context.query_cancellation_requested = []() -> bool {
    throw std::runtime_error("graph cancellation probe failure");
  };
  const auto coordinator_failed = api::EngineGraphQuery(throwing_probe);
  passed &= Require(
      !coordinator_failed.ok && coordinator_failed.result_shape.rows.empty() &&
          DiagnosticContains(coordinator_failed,
                             "SB_MODEL_COORDINATOR_LEG_FAILED_V1"),
      "throwing graph cancellation probe escaped coordinator refusal");

  auto bidirectional = request;
  bidirectional.edges.resize(1);
  bidirectional.seed_vertex_ids.clear();
  bidirectional.min_depth = 0;
  bidirectional.bidirectional_start_vertex_id = Uuid(8101);
  bidirectional.bidirectional_end_vertex_id = Uuid(8102);
  bidirectional.maximum_output_rows = 2;
  const auto boundary = api::EngineGraphQuery(bidirectional);
  passed &= Require(
      boundary.ok && boundary.result_shape.rows.size() == 2 &&
          ApiRowField(boundary, 0, "vertex_uuid") == Uuid(8101) &&
          ApiRowField(boundary, 1, "vertex_uuid") == Uuid(8102),
      "two-row bidirectional graph boundary was refused or truncated");

  auto finite_large_depth = request;
  finite_large_depth.max_depth = 257;
  finite_large_depth.maximum_decoded_bytes = 3000;
  const auto finite_large_depth_refused =
      api::EngineGraphQuery(finite_large_depth);
  passed &= Require(
      !finite_large_depth_refused.ok &&
          finite_large_depth_refused.result_shape.rows.empty() &&
          DiagnosticContains(finite_large_depth_refused,
                             "SB_MODEL_RESOURCE_MEMORY_REFUSED_V1"),
      "finite graph depth above 256 bypassed the memory resource gate");

  const auto exact_corpus_refused = [&](api::EngineGraphQueryRequest mutated,
                                        const std::string_view detail) {
    const auto result = api::EngineGraphQuery(mutated);
    return Require(
        !result.ok && result.result_shape.rows.empty() &&
            DiagnosticContains(result,
                               "SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1"),
        detail);
  };
  auto duplicate_vertex = request;
  duplicate_vertex.vertices[1].vertex_id = duplicate_vertex.vertices[0].vertex_id;
  passed &= exact_corpus_refused(duplicate_vertex,
                                 "duplicate direct graph vertex was admitted");
  auto noncanonical_vertex = request;
  noncanonical_vertex.vertices[0].vertex_id = "not-a-canonical-uuid";
  passed &= exact_corpus_refused(
      noncanonical_vertex, "noncanonical direct graph vertex was admitted");
  auto duplicate_edge = request;
  duplicate_edge.edges[1].edge_id = duplicate_edge.edges[0].edge_id;
  passed &= exact_corpus_refused(duplicate_edge,
                                 "duplicate direct graph edge was admitted");
  auto duplicate_label = request;
  duplicate_label.vertices[0].labels.push_back("P");
  passed &= exact_corpus_refused(duplicate_label,
                                 "duplicate direct graph label was admitted");
  auto duplicate_property = request;
  duplicate_property.vertices[0].properties.push_back({"name", "duplicate"});
  passed &= exact_corpus_refused(
      duplicate_property, "duplicate direct graph property was admitted");
  auto duplicate_edge_property = request;
  duplicate_edge_property.edges[0].properties = {
      {"weight_source", "one"}, {"weight_source", "two"}};
  passed &= exact_corpus_refused(
      duplicate_edge_property,
      "duplicate direct graph edge property was admitted");
  auto orphan_edge = request;
  orphan_edge.edges[0].target_vertex_id = Uuid(8999);
  passed &= exact_corpus_refused(orphan_edge,
                                 "orphan direct graph edge was admitted");
  auto nonfinite_edge = request;
  nonfinite_edge.edges[0].weight =
      std::numeric_limits<double>::quiet_NaN();
  passed &= exact_corpus_refused(nonfinite_edge,
                                 "nonfinite direct graph weight was admitted");
  auto missing_seed = request;
  missing_seed.seed_vertex_ids = {Uuid(8999)};
  passed &= exact_corpus_refused(missing_seed,
                                 "nonexistent direct graph seed was admitted");
  auto missing_fusion_seed = request;
  missing_fusion_seed.fusion_source_kind =
      api::EngineGraphFusionSourceKind::kVector;
  missing_fusion_seed.fused_candidate_seed_vertex_ids = {Uuid(8999)};
  passed &= exact_corpus_refused(
      missing_fusion_seed, "nonexistent graph fusion seed was admitted");
  auto missing_same_endpoint = request;
  missing_same_endpoint.seed_vertex_ids.clear();
  missing_same_endpoint.bidirectional_start_vertex_id = Uuid(8999);
  missing_same_endpoint.bidirectional_end_vertex_id = Uuid(8999);
  passed &= exact_corpus_refused(
      missing_same_endpoint,
      "nonexistent identical bidirectional endpoints synthesized a vertex");
  auto invalid_direction = request;
  invalid_direction.direction =
      static_cast<api::EngineGraphTraversalDirection>(255);
  passed &= exact_corpus_refused(invalid_direction,
                                 "unknown graph direction enum was admitted");
  auto invalid_cycle = request;
  invalid_cycle.cycle_policy =
      static_cast<api::EngineGraphCyclePolicy>(255);
  const auto invalid_cycle_result = api::EngineGraphQuery(invalid_cycle);
  passed &= Require(
      !invalid_cycle_result.ok &&
          invalid_cycle_result.result_shape.rows.empty() &&
          DiagnosticContains(invalid_cycle_result,
                             "SB_MODEL_GRAPH_UNBOUNDED_EXPANSION_REFUSED_V1"),
      "unknown graph cycle enum was admitted without the bounded-policy refusal");
  auto invalid_fusion = request;
  invalid_fusion.fusion_source_kind =
      static_cast<api::EngineGraphFusionSourceKind>(255);
  passed &= exact_corpus_refused(invalid_fusion,
                                 "unknown graph fusion enum was admitted");
  return passed;
}

bool PersistentGraphLexicalMutationRefusals(
    const ProductionFixture& fixture) {
  const auto run_mutation = [&](const std::string& request_id,
                                api::CrudRowVersionRecord row,
                                const std::string_view detail) {
    api::EngineRequestContext context;
    if (!BeginProductionTransaction(fixture, request_id, &context)) {
      return false;
    }
    row.creator_tx = context.local_transaction_id;
    row.table_uuid = fixture.graph_uuid;
    std::vector<api::CrudRowVersionRecord> rows{std::move(row)};
    std::vector<std::uint64_t> sequences;
    const auto appended = api::AppendMgaRowVersions(
        context, &rows, &sequences);
    if (appended.error || sequences.size() != 1) {
      RollbackProductionTransaction(context);
      return Require(false, "persistent graph mutation append failed");
    }
    context.statement_uuid.canonical =
        ProductionUuidText(platform::UuidKind::object);
    api::EnginePublishStatementSnapshotRequest publish;
    publish.context = context;
    const auto snapshot = api::EnginePublishStatementSnapshot(publish);
    if (!snapshot.ok) {
      RollbackProductionTransaction(context);
      return Require(false, "persistent graph mutation snapshot failed");
    }
    context.statement_snapshot_uuid = snapshot.statement_snapshot_uuid;
    context.snapshot_visible_through_local_transaction_id =
        snapshot.snapshot_vector.visible_committed_high_watermark;
    context.statement_metadata_snapshot_engine_owned = true;
    context.statement_metadata_snapshot_uuid.canonical =
        ProductionUuidText(platform::UuidKind::object);
    context.statement_metadata_snapshot_visible_through_local_transaction_id =
        context.snapshot_visible_through_local_transaction_id;
    AddProductionAuthorization(&context, "SELECT", fixture.graph_uuid);

    api::EngineGraphQueryRequest request;
    request.context = context;
    request.physical_query = true;
    request.persistent_graph_source = true;
    request.graph_object_uuid = fixture.graph_uuid;
    request.provider_generation =
        fixture.graph_descriptor.descriptor_generation;
    request.bound_object_identity.object_uuid.canonical = fixture.graph_uuid;
    request.bound_object_identity.resolved_object_type = "graph";
    request.bound_object_identity.resolved_schema_uuid.canonical =
        fixture.schema_uuid;
    request.bound_object_identity.catalog_generation_id =
        context.catalog_generation_id;
    request.bound_object_identity.security_epoch = context.security_epoch;
    request.bound_object_identity.resource_epoch = context.resource_epoch;
    request.seed_vertex_ids = {Uuid(8101)};
    request.min_depth = 0;
    request.max_depth = 1;
    request.maximum_output_rows = 16;
    request.maximum_decoded_bytes = 4 * 1024 * 1024;
    request.physical_proof = ProductionGraphProof();
    request.physical_proof.provider_contract.provider_generation
        .required_generation = request.provider_generation;
    request.physical_proof.provider_contract.provider_generation
        .available_generation = request.provider_generation;
    const auto refused = api::EngineGraphQuery(request);
    const bool passed = Require(
        !refused.ok && refused.result_shape.rows.empty() &&
            DiagnosticContains(refused,
                               "SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1"),
        detail);
    return RollbackProductionTransaction(context) && passed;
  };

  api::CrudRowVersionRecord duplicate_label;
  duplicate_label.row_uuid = Uuid(8191);
  duplicate_label.version_uuid =
      ProductionUuidText(platform::UuidKind::object);
  duplicate_label.values = {{"record_kind", "vertex"},
                            {"vertex_uuid", duplicate_label.row_uuid},
                            {"label.0", "P"},
                            {"label.1", "P"}};
  bool passed = run_mutation(
      "rcp074-graph-duplicate-label", std::move(duplicate_label),
      "persistent graph duplicate label values were admitted");

  api::CrudRowVersionRecord lexical_weight;
  lexical_weight.row_uuid = Uuid(8291);
  lexical_weight.version_uuid =
      ProductionUuidText(platform::UuidKind::object);
  lexical_weight.values = {{"record_kind", "edge"},
                           {"edge_uuid", lexical_weight.row_uuid},
                           {"source_vertex_uuid", Uuid(8101)},
                           {"target_vertex_uuid", Uuid(8102)},
                           {"edge_type", "NEXT"},
                           {"edge_weight", "1e0"}};
  passed &= run_mutation(
      "rcp074-graph-weight-lexical", std::move(lexical_weight),
      "persistent graph noncanonical weight encoding was admitted");
  return passed;
}

bool ProductionCanonicalGraphQueryRoute() {
  ProductionFixture fixture;
  if (!MakeProductionFixture(&fixture)) return false;
  api::EngineRequestContext writer;
  if (!BeginProductionTransaction(fixture, "rcp074-graph-writer", &writer) ||
      !CreateProductionGraph(&fixture, writer)) {
    return false;
  }
  const auto denied_write = WriteProductionGraph(fixture, writer);
  if (denied_write.ok || denied_write.diagnostics.empty() ||
      denied_write.diagnostics.front().detail.find(
          "SB_MODEL_SECURITY_ADMISSION_REFUSED_V1") == std::string::npos) {
    std::cerr << "QOW-CES05-GRAPH denied write: ok=" << denied_write.ok
              << " rows=" << denied_write.dml_summary.rows_changed;
    for (const auto& diagnostic : denied_write.diagnostics) {
      std::cerr << ' ' << diagnostic.code << ':' << diagnostic.detail;
    }
    std::cerr << '\n';
  }
  if (!Require(!denied_write.ok && denied_write.dml_summary.rows_changed == 0 &&
                   !denied_write.diagnostics.empty() &&
                   denied_write.diagnostics.front().detail.find(
                       "SB_MODEL_SECURITY_ADMISSION_REFUSED_V1") !=
                       std::string::npos,
               "production graph write without INSERT was not refused")) {
    return false;
  }
  AddProductionAuthorization(&writer, "INSERT", fixture.graph_uuid);
  const auto orphan_write = WriteProductionGraph(fixture, writer, true);
  if (!Require(!orphan_write.ok && orphan_write.dml_summary.rows_changed == 0 &&
                   DiagnosticContains(
                       orphan_write,
                       "SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1"),
               "structured graph write admitted an orphan edge endpoint")) {
    return false;
  }
  const auto written = WriteProductionGraph(fixture, writer);
  if (!Require(written.ok && written.dml_summary.rows_changed == 6,
               "production structured graph persistence failed") ||
      !CommitProductionTransaction(writer)) {
    return false;
  }

  api::EngineRequestContext context;
  if (!BeginProductionTransaction(fixture, "rcp074-graph-reader", &context)) {
    return false;
  }
  context.statement_uuid.canonical =
      ProductionUuidText(platform::UuidKind::object);
  api::EnginePublishStatementSnapshotRequest publish;
  publish.context = context;
  const auto snapshot = api::EnginePublishStatementSnapshot(publish);
  if (!snapshot.ok) {
    return Require(false, "production graph statement snapshot publish failed");
  }
  context.statement_snapshot_uuid = snapshot.statement_snapshot_uuid;
  context.snapshot_visible_through_local_transaction_id =
      snapshot.snapshot_vector.visible_committed_high_watermark;
  context.statement_metadata_snapshot_engine_owned = true;
  context.statement_metadata_snapshot_uuid.canonical =
      ProductionUuidText(platform::UuidKind::object);
  context.statement_metadata_snapshot_visible_through_local_transaction_id =
      context.snapshot_visible_through_local_transaction_id;
  context.catalog_epoch_uuid.canonical =
      ProductionUuidText(platform::UuidKind::object);
  context.optimizer_capability_snapshot_uuid.canonical =
      ProductionUuidText(platform::UuidKind::object);
  context.optimizer_resource_snapshot_uuid.canonical =
      ProductionUuidText(platform::UuidKind::object);
  context.optimizer_route_snapshot_uuid.canonical =
      ProductionUuidText(platform::UuidKind::object);
  context.optimizer_route_epoch = 79;
  context.optimizer_route_generation = 80;
  context.optimizer_memory_budget_bytes = 1024 * 1024;
  context.optimizer_maximum_candidate_count = 1024;
  context.optimizer_maximum_memo_groups = 1024;
  context.optimizer_maximum_search_steps = 4096;
  context.optimizer_maximum_planning_time_ns = 1'000'000'000;
  context.current_monotonic_ns = std::to_string(ProductionSeed());
  context.query_cancellation_requested = [] { return false; };

  const auto current_graph =
      api::LoadMgaRelationStorageDescriptor(context, fixture.graph_uuid);
  if (!current_graph.ok || current_graph.descriptor.columns.size() != 9) {
    return Require(false,
                   "production reader graph descriptor load failed");
  }
  const auto denied_match = sblr::ExecuteCanonicalCurrentHeapQuery(
      {context, ProductionGraphDag(context, current_graph.descriptor, false)});
  if (!Require(
          denied_match.profile_matched && !denied_match.optimizer_selected &&
              !denied_match.physical_dag_published &&
              !denied_match.physical_dag_executed &&
              !denied_match.runtime_actuals_attached &&
              !denied_match.canonical_result_published &&
              !denied_match.api_result.ok &&
              !denied_match.api_result.diagnostics.empty() &&
              denied_match.api_result.diagnostics.front().code ==
                  "SB_MODEL_SECURITY_ADMISSION_REFUSED_V1",
          "production graph query without SELECT was not refused before access")) {
    return false;
  }
  AddProductionAuthorization(&context, "SELECT", fixture.graph_uuid);
  if (!DirectGraphSafetyBoundaries(context)) return false;
  const auto match = sblr::ExecuteCanonicalCurrentHeapQuery(
      {context, ProductionGraphDag(context, current_graph.descriptor, false)});
  const auto expand = sblr::ExecuteCanonicalCurrentHeapQuery(
      {context, ProductionGraphDag(context, current_graph.descriptor, true)});
  const auto expand_with_seeds = sblr::ExecuteCanonicalCurrentHeapQuery(
      {context,
       ProductionGraphDag(context, current_graph.descriptor, true, 0)});
  const auto inline_cte = sblr::ExecuteCanonicalCurrentHeapQuery(
      {context,
       ProductionGraphCteDag(context, current_graph.descriptor, false)});
  const auto materialized_cte = sblr::ExecuteCanonicalCurrentHeapQuery(
      {context,
       ProductionGraphCteDag(context, current_graph.descriptor, true)});
  const auto counted = sblr::ExecuteCanonicalCurrentHeapQuery(
      {context, ProductionGraphCountDag(context, current_graph.descriptor)});
  const auto recursive_cte_dag =
      ProductionGraphRecursiveCteDag(context, current_graph.descriptor);
  const auto recursive_cte = sblr::ExecuteCanonicalCurrentHeapQuery(
      {context, recursive_cte_dag});
  const auto replayed_recursive_cte =
      sblr::ExecuteCanonicalCurrentHeapQuery(
          {context, recursive_cte_dag});
  const auto set_union_dag =
      ProductionGraphSetDag(context, current_graph.descriptor);
  const auto set_union = sblr::ExecuteCanonicalCurrentHeapQuery(
      {context, set_union_dag});
  const auto replayed_set_union = sblr::ExecuteCanonicalCurrentHeapQuery(
      {context, set_union_dag});
  const auto projected_limited = sblr::ExecuteCanonicalCurrentHeapQuery(
      {context,
       ProductionGraphProjectLimitDag(context, current_graph.descriptor)});
  const auto filtered_projected_limited =
      sblr::ExecuteCanonicalCurrentHeapQuery(
          {context,
           ProductionGraphFilterProjectLimitDag(
               context, current_graph.descriptor)});
  const auto row_number = sblr::ExecuteCanonicalCurrentHeapQuery(
      {context,
       ProductionGraphRowNumberDag(context, current_graph.descriptor)});
  if ((!match.api_result.ok && !match.api_result.diagnostics.empty()) ||
      (!expand.api_result.ok && !expand.api_result.diagnostics.empty()) ||
      (!expand_with_seeds.api_result.ok &&
       !expand_with_seeds.api_result.diagnostics.empty()) ||
      (!inline_cte.api_result.ok && !inline_cte.api_result.diagnostics.empty()) ||
      (!materialized_cte.api_result.ok &&
       !materialized_cte.api_result.diagnostics.empty()) ||
      (!counted.api_result.ok && !counted.api_result.diagnostics.empty()) ||
      (!recursive_cte.api_result.ok &&
       !recursive_cte.api_result.diagnostics.empty()) ||
      (!set_union.api_result.ok &&
       !set_union.api_result.diagnostics.empty())) {
    if (!projected_limited.api_result.ok &&
        !projected_limited.api_result.diagnostics.empty()) {
      const auto& diagnostic =
          projected_limited.api_result.diagnostics.front();
      std::cerr << "QOW-CES05-GRAPH projected route: " << diagnostic.code
                << ' ' << diagnostic.detail << '\n';
    }
    for (const auto* result : {&match.api_result, &expand.api_result,
                               &expand_with_seeds.api_result,
                               &inline_cte.api_result,
                               &materialized_cte.api_result,
                               &counted.api_result,
                               &recursive_cte.api_result,
                               &set_union.api_result}) {
      for (const auto& diagnostic : result->diagnostics) {
        std::cerr << "QOW-CES05-GRAPH production route: " << diagnostic.code
                  << ' ' << diagnostic.detail << '\n';
      }
    }
  }
  const auto complete = [](const auto& result, const std::size_t rows) {
    return result.profile_matched && result.optimizer_admitted &&
           result.optimizer_selected && result.physical_dag_published &&
           result.physical_dag_executed && result.runtime_actuals_attached &&
           result.canonical_result_published && result.api_result.ok &&
           result.optimizer_admission_stage_count == 8 &&
           result.physical_node_count == 1 &&
           result.canonical_result_column_count == 9 &&
           result.canonical_result_row_count == rows &&
           !result.selected_plan_uuid.empty() &&
           !result.canonical_result_bytes.empty();
  };
  bool passed = true;
  passed &= Require(
      current_graph.descriptor.columns[0]
                  .value_descriptor.canonical_type_name == "uuid" &&
          current_graph.descriptor.columns[1]
                  .value_descriptor.canonical_type_name == "uuid" &&
          current_graph.descriptor.columns[1].nullable &&
          current_graph.descriptor.columns[2]
                  .value_descriptor.canonical_type_name == "uuid" &&
          current_graph.descriptor.columns[7]
                  .value_descriptor.canonical_type_name == "uint64",
      "production graph UUID/depth descriptor identity drifted");
  passed &= Require(complete(match, 3),
                    "normal production GRAPH_MATCH route did not complete");
  passed &= Require(complete(expand, 3),
                    "normal production GRAPH_EXPAND route did not complete");
  passed &= Require(
      complete(expand_with_seeds, 6),
      "depth-zero production GRAPH_EXPAND route did not complete");
  const auto complete_cte = [](const auto& execution,
                               const std::string_view implementation) {
    return execution.profile_matched && execution.optimizer_admitted &&
           execution.optimizer_selected && execution.physical_dag_published &&
           execution.physical_dag_executed &&
           execution.runtime_actuals_attached &&
           execution.canonical_result_published && execution.api_result.ok &&
           execution.physical_node_count == 2 &&
           execution.canonical_result_column_count == 9 &&
           execution.canonical_result_row_count == 3 &&
           HasEvidence(execution.api_result,
                       "canonical.graph_cte_implementation",
                       implementation);
  };
  passed &= Require(
      complete_cte(inline_cte, "cte.bound.inline.typed.v1") &&
          complete_cte(materialized_cte,
                       "cte.bound.materialize.typed.v1"),
      "production graph inline/materialized CTE did not consume the graph batch");
  passed &= Require(
      counted.profile_matched && counted.optimizer_admitted &&
          counted.optimizer_selected && counted.physical_dag_published &&
          counted.physical_dag_executed && counted.runtime_actuals_attached &&
          counted.canonical_result_published && counted.api_result.ok &&
          counted.physical_node_count == 2 &&
          counted.canonical_result_column_count == 1 &&
          counted.canonical_result_row_count == 1 &&
          ApiRowField(counted.api_result, 0, "graph_count") == "3" &&
          HasEvidence(counted.api_result,
                      "canonical.graph_aggregate_implementation",
                      "aggregate.count-star.v1"),
      "production graph COUNT(*) did not consume the graph batch");
  passed &= Require(
      recursive_cte.profile_matched && recursive_cte.optimizer_admitted &&
          recursive_cte.optimizer_selected &&
          recursive_cte.physical_dag_published &&
          recursive_cte.physical_dag_executed &&
          recursive_cte.runtime_actuals_attached &&
          recursive_cte.canonical_result_published &&
          recursive_cte.api_result.ok &&
          recursive_cte.physical_node_count == 4 &&
          recursive_cte.canonical_result_column_count == 1 &&
          recursive_cte.canonical_result_row_count == 3 &&
          ApiRowField(recursive_cte.api_result, 0, "graph_count") == "3" &&
          ApiRowField(recursive_cte.api_result, 1, "graph_count") == "4" &&
          ApiRowField(recursive_cte.api_result, 2, "graph_count") == "5" &&
          replayed_recursive_cte.api_result.ok &&
          replayed_recursive_cte.canonical_result_bytes ==
              recursive_cte.canonical_result_bytes &&
          HasEvidence(recursive_cte.api_result,
                      "canonical.graph_recursive_cte_implementation",
                      "cte.recursive.union-all.typed.v1") &&
          HasEvidence(recursive_cte.api_result,
                      "canonical.graph_recursive_cte_bound", "5") &&
          HasEvidence(recursive_cte.api_result,
                      "canonical.graph_recursive_cte_work_bound", "12"),
      "production graph recursive CTE did not execute the bounded COUNT(*) anchor chain");
  if (!recursive_cte.api_result.ok ||
      recursive_cte.canonical_result_row_count != 3 ||
      ApiRowField(recursive_cte.api_result, 0, "graph_count") != "3" ||
      !HasEvidence(recursive_cte.api_result,
                   "canonical.graph_recursive_cte_implementation",
                   "cte.recursive.union-all.typed.v1") ||
      replayed_recursive_cte.canonical_result_bytes !=
          recursive_cte.canonical_result_bytes) {
    std::cerr << "QOW-CES05-GRAPH recursive detail ok="
              << recursive_cte.api_result.ok << " nodes="
              << recursive_cte.physical_node_count << " rows="
              << recursive_cte.canonical_result_row_count << " values="
              << ApiRowField(recursive_cte.api_result, 0, "graph_count")
              << ','
              << ApiRowField(recursive_cte.api_result, 1, "graph_count")
              << ','
              << ApiRowField(recursive_cte.api_result, 2, "graph_count")
              << " replay_equal="
              << (replayed_recursive_cte.canonical_result_bytes ==
                  recursive_cte.canonical_result_bytes)
              << '\n';
    for (const auto& evidence : recursive_cte.api_result.evidence) {
      if (evidence.evidence_kind.find("recursive") != std::string::npos) {
        std::cerr << evidence.evidence_kind << '=' << evidence.evidence_id
                  << '\n';
      }
    }
  }
  passed &= Require(
      set_union.profile_matched && set_union.optimizer_admitted &&
          set_union.optimizer_selected &&
          set_union.physical_dag_published &&
          set_union.physical_dag_executed &&
          set_union.runtime_actuals_attached &&
          set_union.canonical_result_published &&
          set_union.api_result.ok && set_union.physical_node_count == 4 &&
          set_union.canonical_result_column_count == 1 &&
          set_union.canonical_result_row_count == 4 &&
          ApiRowField(set_union.api_result, 3, "vertex_uuid") ==
              Uuid(8999) &&
          replayed_set_union.api_result.ok &&
          replayed_set_union.canonical_result_bytes ==
              set_union.canonical_result_bytes &&
          HasEvidence(set_union.api_result,
                      "canonical.graph_set_implementation",
                      "setop.union-all.ordinal.typed.v1") &&
          HasEvidence(set_union.api_result,
                      "canonical.graph_set_semantics",
                      "union-all.ordinal.left-then-right.bag.v1"),
      "production graph PROJECT+UNION ALL VALUES did not consume the graph batch");
  if (!set_union.api_result.ok || set_union.canonical_result_row_count != 4 ||
      ApiRowField(set_union.api_result, 3, "vertex_uuid") != Uuid(8999) ||
      !HasEvidence(set_union.api_result,
                   "canonical.graph_set_implementation",
                   "setop.union-all.ordinal.typed.v1") ||
      replayed_set_union.canonical_result_bytes !=
          set_union.canonical_result_bytes) {
    std::cerr << "QOW-CES05-GRAPH set detail ok=" << set_union.api_result.ok
              << " nodes=" << set_union.physical_node_count << " rows="
              << set_union.canonical_result_row_count << " last="
              << ApiRowField(set_union.api_result, 3, "vertex_uuid")
              << " replay_equal="
              << (replayed_set_union.canonical_result_bytes ==
                  set_union.canonical_result_bytes)
              << '\n';
    for (const auto& evidence : set_union.api_result.evidence) {
      if (evidence.evidence_kind.find("set") != std::string::npos) {
        std::cerr << evidence.evidence_kind << '=' << evidence.evidence_id
                  << '\n';
      }
    }
  }
  constexpr std::array<ProductionGraphRecursiveMutation, 4>
      kRecursiveMutations{{
          ProductionGraphRecursiveMutation::term_semantic,
          ProductionGraphRecursiveMutation::root_semantic,
          ProductionGraphRecursiveMutation::output_schema,
          ProductionGraphRecursiveMutation::mga_context,
      }};
  for (const auto mutation : kRecursiveMutations) {
    const auto malformed = sblr::ExecuteCanonicalCurrentHeapQuery(
        {context,
         ProductionGraphRecursiveCteDag(
             context, current_graph.descriptor, "5", mutation)});
    passed &= Require(
        !malformed.api_result.ok && !malformed.physical_dag_executed &&
            !malformed.canonical_result_published &&
            malformed.canonical_result_bytes.empty(),
        "substituted graph recursive term/root/schema/MGA context was admitted");
  }
  const auto over_bound_recursive =
      sblr::ExecuteCanonicalCurrentHeapQuery(
          {context,
           ProductionGraphRecursiveCteDag(
               context, current_graph.descriptor, "4096")});
  passed &= Require(
      !over_bound_recursive.api_result.ok &&
          !over_bound_recursive.physical_dag_executed &&
          !over_bound_recursive.canonical_result_published &&
          over_bound_recursive.canonical_result_bytes.empty(),
      "graph recursive CTE exceeded its admitted row/work bound");

  constexpr std::array<ProductionGraphSetMutation, 15> kSetMutations{{
      ProductionGraphSetMutation::root_semantic,
      ProductionGraphSetMutation::values_semantic,
      ProductionGraphSetMutation::output_schema,
      ProductionGraphSetMutation::input_order,
      ProductionGraphSetMutation::orphan_node,
      ProductionGraphSetMutation::root_output_lineage,
      ProductionGraphSetMutation::literal_type,
      ProductionGraphSetMutation::mga_context,
      ProductionGraphSetMutation::orphan_expression,
      ProductionGraphSetMutation::orphan_descriptor,
      ProductionGraphSetMutation::duplicate_operation_child,
      ProductionGraphSetMutation::extra_model_operation,
      ProductionGraphSetMutation::operation_function_uuid,
      ProductionGraphSetMutation::operation_semantic_mismatch,
      ProductionGraphSetMutation::object_uuid_substitution,
  }};
  for (const auto mutation : kSetMutations) {
    const auto malformed = sblr::ExecuteCanonicalCurrentHeapQuery(
        {context,
         ProductionGraphSetDag(context, current_graph.descriptor,
                               mutation)});
    passed &= Require(
        !malformed.api_result.ok && !malformed.optimizer_admitted &&
            !malformed.physical_dag_executed &&
            !malformed.canonical_result_published &&
            malformed.canonical_result_bytes.empty(),
        "substituted graph set semantic/input/schema/lineage/MGA context was admitted: mutation=" +
            std::to_string(static_cast<unsigned>(mutation)));
  }
  auto cancelled_composition_context = context;
  cancelled_composition_context.query_cancellation_requested = [] {
    return true;
  };
  for (const auto& cancelled_dag : {
           ProductionGraphSetDag(cancelled_composition_context,
                                 current_graph.descriptor),
           ProductionGraphRecursiveCteDag(cancelled_composition_context,
                                          current_graph.descriptor)}) {
    const auto cancelled = sblr::ExecuteCanonicalCurrentHeapQuery(
        {cancelled_composition_context, cancelled_dag});
    passed &= Require(
        !cancelled.api_result.ok && !cancelled.physical_dag_executed &&
            !cancelled.canonical_result_published &&
            cancelled.canonical_result_bytes.empty(),
        "cancelled graph set/recursive composition partially executed or published");
  }
  auto resource_context = context;
  resource_context.optimizer_memory_budget_bytes = 1;
  const auto resource_refused_set = sblr::ExecuteCanonicalCurrentHeapQuery(
      {resource_context,
       ProductionGraphSetDag(resource_context, current_graph.descriptor)});
  passed &= Require(
      !resource_refused_set.api_result.ok &&
          !resource_refused_set.physical_dag_executed &&
          !resource_refused_set.canonical_result_published &&
          resource_refused_set.canonical_result_bytes.empty(),
      "resource-refused graph set composition partially executed or published");
  std::vector<api::TypedRelationalDag> malformed_unary_dags;
  {
    auto malformed = ProductionGraphCteDag(
        context, current_graph.descriptor, false);
    malformed.nodes.back().semantic_variant_id = "cte.substituted.v1";
    malformed_unary_dags.push_back(std::move(malformed));
  }
  {
    auto malformed = ProductionGraphCountDag(
        context, current_graph.descriptor);
    malformed.nodes.back().semantic_variant_id =
        "aggregate.substituted.v1";
    malformed_unary_dags.push_back(std::move(malformed));
  }
  {
    auto malformed = ProductionGraphProjectLimitDag(
        context, current_graph.descriptor);
    malformed.nodes[1].semantic_variant_id = "project.substituted.v1";
    malformed_unary_dags.push_back(std::move(malformed));
  }
  {
    auto malformed = ProductionGraphFilterProjectLimitDag(
        context, current_graph.descriptor);
    malformed.nodes[1].semantic_variant_id = "filter.substituted.v1";
    malformed_unary_dags.push_back(std::move(malformed));
  }
  {
    auto malformed = ProductionGraphSortDag(
        context, current_graph.descriptor);
    malformed.nodes.back().semantic_variant_id = "sort.substituted.v1";
    malformed_unary_dags.push_back(std::move(malformed));
  }
  {
    auto malformed = ProductionGraphRowNumberDag(
        context, current_graph.descriptor);
    malformed.window_invocations.front().function_uuid = Uuid(8998);
    malformed_unary_dags.push_back(std::move(malformed));
  }
  {
    auto malformed = ProductionGraphProjectLimitDag(
        context, current_graph.descriptor);
    const auto limit_expression = std::ranges::find_if(
        malformed.expressions, [](const auto& expression) {
          return expression.expression_id == 30;
        });
    if (limit_expression != malformed.expressions.end()) {
      limit_expression->literal_or_parameter_ref = "-1";
    }
    malformed_unary_dags.push_back(std::move(malformed));
  }
  {
    auto malformed = ProductionGraphProjectLimitDag(
        context, current_graph.descriptor);
    malformed.statement_snapshot_uuid =
        ProductionUuidText(platform::UuidKind::object);
    malformed_unary_dags.push_back(std::move(malformed));
  }
  for (const auto& malformed_dag : malformed_unary_dags) {
    const auto malformed = sblr::ExecuteCanonicalCurrentHeapQuery(
        {context, malformed_dag});
    passed &= Require(
        !malformed.api_result.ok && !malformed.physical_dag_executed &&
            !malformed.canonical_result_published &&
            malformed.canonical_result_bytes.empty(),
        "substituted graph unary composition semantic/schema/MGA context was admitted");
  }
  passed &= Require(
      projected_limited.profile_matched &&
          projected_limited.optimizer_admitted &&
          projected_limited.optimizer_selected &&
          projected_limited.physical_dag_published &&
          projected_limited.physical_dag_executed &&
          projected_limited.runtime_actuals_attached &&
          projected_limited.canonical_result_published &&
          projected_limited.api_result.ok &&
          projected_limited.physical_node_count == 3 &&
          projected_limited.canonical_result_column_count == 3 &&
          projected_limited.canonical_result_row_count == 2 &&
          HasEvidence(projected_limited.api_result,
                      "canonical.graph_project_implementation",
                      "project.descriptor-direct.v1") &&
          HasEvidence(projected_limited.api_result,
                      "canonical.graph_limit_implementation",
                      "limit.typed.v1"),
      "production graph PROJECT+LIMIT did not consume the graph batch");
  passed &= Require(
      filtered_projected_limited.api_result.ok &&
          filtered_projected_limited.physical_dag_executed &&
          filtered_projected_limited.canonical_result_published &&
          filtered_projected_limited.physical_node_count == 4 &&
          filtered_projected_limited.canonical_result_column_count == 3 &&
          filtered_projected_limited.canonical_result_row_count == 2 &&
          HasEvidence(filtered_projected_limited.api_result,
                      "canonical.graph_filter_implementation",
                      "filter.3vl.row.v1"),
      "production graph FILTER+PROJECT+LIMIT did not consume the graph batch");
  passed &= Require(
      row_number.api_result.ok && row_number.physical_dag_executed &&
          row_number.canonical_result_published &&
          row_number.physical_node_count == 3 &&
          row_number.canonical_result_column_count == 10 &&
          row_number.canonical_result_row_count == 3 &&
          ApiRowField(row_number.api_result, 0, "row_number") == "1" &&
          ApiRowField(row_number.api_result, 1, "row_number") == "2" &&
          ApiRowField(row_number.api_result, 2, "row_number") == "3" &&
          HasEvidence(row_number.api_result,
                      "canonical.graph_sort_implementation",
                      "sort.typed.terms.v1") &&
          HasEvidence(row_number.api_result,
                      "canonical.graph_window_implementation",
                      "window.row-number.v1"),
      "production graph SORT+ROW_NUMBER did not consume the graph batch");
  passed &= Require(
      HasEvidence(match.api_result, "canonical.model_route",
                  "SBSQL_GRAPH_SOURCE_TO_SBLR_MODEL_SOURCE_TO_GRAPH_ADJACENCY_SCAN_TO_TYPED_BATCH_V1") &&
          HasEvidence(match.api_result, "canonical.model_search_family",
                      "graph.local.v1") &&
          HasEvidence(match.api_result, "canonical.graph_operation",
                      "GRAPH_MATCH") &&
          HasEvidence(match.api_result, "canonical.graph_source_authority",
                      "persistent_mga_relation_v1") &&
          HasEvidence(match.api_result, "canonical.physical_dispatch",
                      "generic.selected-dag.v1") &&
          HasEvidence(expand.api_result, "canonical.graph_operation",
                      "GRAPH_EXPAND") &&
          HasEvidence(expand.api_result, "canonical.graph_cycle_policy",
                      "visited_set") &&
          HasEvidence(expand.api_result, "canonical.graph_maximum_depth", "1") &&
          HasEvidence(match.api_result, "canonical.graph_typed_row_contract",
                      "uuid-identities+labels+properties+direction+depth+cycle") &&
          HasEvidence(match.api_result,
                      "canonical.graph_depth_zero_edge_state", "sql_null") &&
          HasEvidence(expand.api_result,
                      "canonical.graph_depth_zero_edge_state",
                      "not_present") &&
          HasEvidence(expand_with_seeds.api_result,
                      "canonical.graph_depth_zero_edge_state", "sql_null"),
      "production graph route evidence drifted");
  passed &= CommitProductionTransaction(context);
  passed &= PersistentGraphLexicalMutationRefusals(fixture);
  return passed;
}
#endif
}  // namespace

int main() {
  bool passed = VectorInventory();
  passed &= LogicalGraphFamilyIdentityMutations();
  passed &= SuccessVector(kVectors[0], false, false);
  passed &= SuccessVector(kVectors[1], true, false);
  passed &= SuccessVector(kVectors[10], false, true);
  passed &= RefusalVectors();
  passed &= ReplayAndGraphIdentity();
#if defined(SB_CES05_GRAPH_PRODUCTION_QUERY_ROUTE)
  passed &= ProductionCanonicalGraphQueryRoute();
#endif
  if (passed) {
    std::cout << "QOW-CES05-GRAPH: PASS vectors=12 route="
              << "SBSQL_GRAPH_SOURCE_TO_SBLR_MODEL_SOURCE_TO_"
                 "GRAPH_ADJACENCY_SCAN_TO_TYPED_BATCH_V1\n";
  }
  return passed ? 0 : 1;
}
