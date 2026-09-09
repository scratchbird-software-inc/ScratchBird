// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "canonical_query_graph_composition.hpp"

#include "canonical_query_aggregate_composition.hpp"
#include "canonical_query_aggregate_registration.hpp"
#include "canonical_query_descriptor_support.hpp"
#include "canonical_query_filter_registration.hpp"
#include "canonical_query_join_registration.hpp"
#include "canonical_query_node_composition.hpp"
#include "canonical_query_object_free_composition_support.hpp"
#include "canonical_query_object_free_profile.hpp"
#include "canonical_query_physical_registration.hpp"
#include "canonical_query_recursive_registration.hpp"
#include "canonical_query_relational_registration.hpp"
#include "canonical_query_runtime_memory_support.hpp"
#include "canonical_query_scalar_support.hpp"
#include "canonical_query_set_composition.hpp"
#include "canonical_query_set_registration.hpp"
#include "canonical_query_sort_registration.hpp"
#include "canonical_query_window_registration.hpp"
#include "canonical_relational_expression.hpp"

#include "catalog/name_resolution_api.hpp"
#include "datatype_catalog_manifest.hpp"
#include "engine/executor/executor_foundation.hpp"
#include "engine/executor/model_family_executor.hpp"
#include "engine/internal_api/mga_relation_store/mga_relation_descriptor.hpp"
#include "engine/internal_api/mga_relation_store/mga_relation_store.hpp"
#include "engine/optimizer/model_family_coordinator.hpp"
#include "engine/optimizer/model_family_profile_factory.hpp"
#include "engine/optimizer/optimizer_contract.hpp"
#include "engine/optimizer/relational_planner.hpp"
#include "nosql/graph_api.hpp"
#include "nosql/nosql_provider_generation_store.hpp"
#include "query/canonical_heap_optimizer_admission.hpp"
#include "query/canonical_relational_bridge.hpp"
#include "query/expression_api.hpp"
#include "security/security_model.hpp"
#include "transaction/transaction_api.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <numeric>
#include <optional>
#include <ranges>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace scratchbird::engine::sblr {
namespace api = scratchbird::engine::internal_api;
namespace dt = scratchbird::core::datatypes;
namespace exec = scratchbird::engine::executor;
namespace opt = scratchbird::engine::optimizer;
namespace plan = scratchbird::engine::planner;

namespace {

// SEARCH_KEY: SB_ENGINE_CANONICAL_QUERY_GRAPH_COMPOSITION_AUTHORITY
// Owns one admitted production graph source route and its bounded relational
// tail. It consumes and revalidates engine-issued MGA statement authority and
// cannot create snapshots or publish transaction finality.

constexpr std::string_view kValuesImplementationId =
    "values.materialize.canonical.v1";

}  // namespace

// QOW-SOURCE-RCP-074-GRAPH-CANONICAL-QUERY-ROUTE-V1
CanonicalObjectFreeValuesExecutionResult ExecuteCanonicalGraphFamilyQuery(
    const CanonicalCurrentHeapExecutionRequest& input,
    Rcp079CapturedModelLegV1* leg_capture) {
  CanonicalObjectFreeValuesExecutionResult result;
  const auto& dag = input.relational_dag;
  const auto scan = std::ranges::find_if(dag.nodes, [](const auto& node) {
    return node.node_kind == api::RelationalDagNodeKind::kScan &&
           (node.semantic_variant_id == "SBLR_MODEL_SOURCE_V1" ||
            node.semantic_variant_id == "SBLR_MODEL_EXPAND_V1");
  });
  const auto graph_operation = std::ranges::find_if(
      dag.expressions, [](const auto& expression) {
        return expression.operator_name == "GRAPH_MATCH" ||
               expression.operator_name == "GRAPH_EXPAND";
      });
  if (scan == dag.nodes.end() || graph_operation == dag.expressions.end()) {
    return result;
  }
  result.profile_matched = true;
  CanonicalObjectFreeValuesExecutionRequest response_context;
  response_context.context = input.context;
  response_context.relational_dag = dag;
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
    result.api_result = Failure(response_context, std::move(diagnostic_id),
                                std::move(detail));
    return result;
  };
  const bool match = graph_operation->operator_name == "GRAPH_MATCH";
  const auto graph_producer_count = std::ranges::count_if(
      dag.nodes, [](const auto& node) {
        return node.node_kind == api::RelationalDagNodeKind::kScan &&
               (node.semantic_variant_id == "SBLR_MODEL_SOURCE_V1" ||
                node.semantic_variant_id == "SBLR_MODEL_EXPAND_V1");
      });
  if (dag.wire_version != 2 || graph_producer_count != 1 ||
      std::ranges::count_if(dag.expressions, [](const auto& expression) {
        return expression.operator_name == "GRAPH_MATCH" ||
               expression.operator_name == "GRAPH_EXPAND";
      }) != 1 ||
      !scan->input_node_ids.empty() || scan->required_object_uuids.size() != 1 ||
      scan->output_descriptor_ids.empty() ||
      (match && scan->semantic_variant_id != "SBLR_MODEL_SOURCE_V1") ||
      (!match && scan->semantic_variant_id != "SBLR_MODEL_EXPAND_V1") ||
      graph_operation->function_uuid.has_value() ||
      graph_operation->expression_kind !=
          api::RelationalExpressionKind::kFunctionCall ||
      graph_operation->child_expression_ids.size() != (match ? 2 : 5)) {
    return refuse("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                  "graph canonical source shape is incomplete");
  }
  const auto typed_node_for = [&](const std::uint32_t node_id) {
    return std::ranges::find_if(dag.nodes, [&](const auto& node) {
      return node.node_id == node_id;
    });
  };
  const api::RelationalDagNode* recursive_root = nullptr;
  const api::RelationalDagNode* recursive_term = nullptr;
  const api::RelationalDagNode* set_root = nullptr;
  const api::RelationalDagNode* set_values = nullptr;
  LiveSetOperationProfile graph_set_profile;
  std::uint32_t composition_root_node_id = dag.root_node_id;
  const auto requested_root = typed_node_for(dag.root_node_id);
  if (requested_root != dag.nodes.end() &&
      requested_root->node_kind ==
          api::RelationalDagNodeKind::kSetOperation) {
    graph_set_profile = ResolveLiveSetOperationProfileForComposition(
        requested_root->semantic_variant_id);
    if (!graph_set_profile.matched ||
        graph_set_profile.operation !=
            exec::CanonicalSetOperationKind::kUnion ||
        graph_set_profile.quantifier !=
            exec::CanonicalSetOperationQuantifier::kAll ||
        graph_set_profile.alignment !=
            exec::CanonicalSetOperationAlignment::kOrdinal ||
        graph_set_profile.type_profile !=
            exec::CanonicalSetOperationTypeProfile::kExact ||
        graph_set_profile.equality_profile !=
            exec::CanonicalSetOperationEqualityProfile::kExactTyped ||
        requested_root->input_node_ids.size() != 2 ||
        requested_root->input_node_ids[0] ==
            requested_root->input_node_ids[1] ||
        !requested_root->bound_expression_ids.empty() ||
        !requested_root->required_object_uuids.empty() ||
        !requested_root->required_property_uuids.empty() ||
        !requested_root->delivered_property_uuids.empty()) {
      return refuse(
          "SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
          "graph set root is not exact ordinal UNION ALL");
    }
    const auto left = typed_node_for(requested_root->input_node_ids[0]);
    const auto right = typed_node_for(requested_root->input_node_ids[1]);
    if (left == dag.nodes.end() || right == dag.nodes.end() ||
        right->node_kind != api::RelationalDagNodeKind::kValues ||
        right->semantic_variant_id != "values.literal-table.v1" ||
        !right->input_node_ids.empty() || right->values_row_ids.empty() ||
        !right->bound_expression_ids.empty() ||
        !right->required_object_uuids.empty() ||
        !right->required_property_uuids.empty() ||
        !right->delivered_property_uuids.empty() ||
        requested_root->output_descriptor_ids !=
            left->output_descriptor_ids ||
        requested_root->output_descriptor_ids !=
            right->output_descriptor_ids) {
      return refuse(
          "SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
          "graph UNION ALL right VALUES input or schema is not exact");
    }
    set_root = &*requested_root;
    set_values = &*right;
    composition_root_node_id = left->node_id;
  } else if (requested_root != dag.nodes.end() &&
             requested_root->node_kind ==
                 api::RelationalDagNodeKind::kRecursiveCte) {
    const auto recursive_profile = MatchLiveRecursiveCteProfileForComposition(
        requested_root->semantic_variant_id);
    if (!recursive_profile.matched || recursive_profile.search_cycle ||
        recursive_profile.union_mode !=
            exec::CanonicalRecursiveCteUnionMode::kAll ||
        requested_root->input_node_ids.size() != 2 ||
        requested_root->input_node_ids[0] ==
            requested_root->input_node_ids[1] ||
        requested_root->bound_expression_ids.size() != 1 ||
        !requested_root->required_object_uuids.empty() ||
        !requested_root->required_property_uuids.empty() ||
        !requested_root->delivered_property_uuids.empty()) {
      return refuse(
          "SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
          "graph recursive root is not exact bounded UNION ALL");
    }
    const auto anchor = typed_node_for(requested_root->input_node_ids[0]);
    const auto term = typed_node_for(requested_root->input_node_ids[1]);
    if (anchor == dag.nodes.end() || term == dag.nodes.end() ||
        anchor->node_kind != api::RelationalDagNodeKind::kAggregate ||
        term->node_kind != api::RelationalDagNodeKind::kCte ||
        term->semantic_variant_id !=
            "cte.recursive-term-int64-increment.v1" ||
        !term->input_node_ids.empty() ||
        !term->bound_expression_ids.empty() ||
        !term->required_object_uuids.empty() ||
        !term->required_property_uuids.empty() ||
        !term->delivered_property_uuids.empty() ||
        anchor->output_descriptor_ids != term->output_descriptor_ids ||
        anchor->output_descriptor_ids !=
            requested_root->output_descriptor_ids) {
      return refuse(
          "SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
          "graph recursive anchor, term, or schema is not exact");
    }
    recursive_root = &*requested_root;
    recursive_term = &*term;
    composition_root_node_id = anchor->node_id;
  }
  std::vector<const api::RelationalDagNode*> consumer_chain;
  std::unordered_set<std::uint32_t> reachable_node_ids;
  if (recursive_root != nullptr) {
    reachable_node_ids.insert(recursive_root->node_id);
    reachable_node_ids.insert(recursive_term->node_id);
  }
  if (set_root != nullptr) {
    reachable_node_ids.insert(set_root->node_id);
    reachable_node_ids.insert(set_values->node_id);
  }
  auto chain_node = typed_node_for(composition_root_node_id);
  while (chain_node != dag.nodes.end() && chain_node->node_id != scan->node_id) {
    if (!reachable_node_ids.insert(chain_node->node_id).second ||
        chain_node->input_node_ids.size() != 1 ||
        (chain_node->node_kind != api::RelationalDagNodeKind::kFilter &&
         chain_node->node_kind != api::RelationalDagNodeKind::kProject &&
         chain_node->node_kind != api::RelationalDagNodeKind::kSort &&
         chain_node->node_kind != api::RelationalDagNodeKind::kWindow &&
         chain_node->node_kind != api::RelationalDagNodeKind::kAggregate &&
         chain_node->node_kind != api::RelationalDagNodeKind::kCte &&
         chain_node->node_kind != api::RelationalDagNodeKind::kLimit)) {
      return refuse(
          "SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
          "graph downstream composition is not an exact supported unary chain");
    }
    consumer_chain.push_back(&*chain_node);
    chain_node = typed_node_for(chain_node->input_node_ids.front());
  }
  if (chain_node == dag.nodes.end() ||
      !reachable_node_ids.insert(scan->node_id).second ||
      reachable_node_ids.size() != dag.nodes.size()) {
    return refuse("SBLR.PLAN_TREE.INVALID_HANDLE",
                  "graph downstream composition is disconnected or orphaned");
  }
  std::ranges::reverse(consumer_chain);
  const auto expression_for = [&](const std::uint32_t expression_id) {
    return std::ranges::find_if(dag.expressions, [&](const auto& expression) {
      return expression.expression_id == expression_id;
    });
  };
  const auto exact_literal = [](const auto& expression,
                                const api::RelationalLiteralKind kind) {
    return expression.expression_kind ==
               api::RelationalExpressionKind::kLiteral &&
           expression.literal_kind == kind &&
           expression.literal_or_parameter_ref.has_value() &&
           !expression.literal_or_parameter_ref->empty() &&
           expression.child_expression_ids.empty() &&
           !expression.function_uuid.has_value() &&
           !expression.bound_name_uuid.has_value() &&
           !expression.operator_name.has_value();
  };
  const auto alias = expression_for(graph_operation->child_expression_ids[0]);
  const std::unordered_set<std::uint32_t> graph_operation_child_ids(
      graph_operation->child_expression_ids.begin(),
      graph_operation->child_expression_ids.end());
  if (alias == dag.expressions.end() ||
      graph_operation_child_ids.size() !=
          graph_operation->child_expression_ids.size() ||
      alias->expression_kind != api::RelationalExpressionKind::kIdentifier ||
      alias->bound_name_uuid != scan->required_object_uuids.front() ||
      !alias->child_expression_ids.empty()) {
    return refuse("SB_MODEL_BINDING_INCOMPLETE_V1",
                  "graph source alias did not retain object UUID authority");
  }
  api::EngineGraphQueryRequest graph_request;
  graph_request.context = input.context;
  graph_request.physical_query = true;
  graph_request.persistent_graph_source = true;
  graph_request.graph_object_uuid = scan->required_object_uuids.front();
  graph_request.cycle_policy = api::EngineGraphCyclePolicy::kVisitedSet;
  if (match) {
    const auto pattern = expression_for(graph_operation->child_expression_ids[1]);
    if (pattern == dag.expressions.end() ||
        !exact_literal(*pattern, api::RelationalLiteralKind::kString)) {
      return refuse("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                    "GRAPH_MATCH pattern literal is incomplete");
    }
    graph_request.typed_pattern_literal =
        *pattern->literal_or_parameter_ref;
    graph_request.min_depth = 0;
    graph_request.max_depth = 0;
    const auto& typed_pattern = graph_request.typed_pattern_literal;
    if (typed_pattern == "vertex(*)") {
      // An empty seed filter means every visible vertex in the bound graph.
    } else if (typed_pattern.starts_with("vertex(label=") &&
               typed_pattern.ends_with(')') && typed_pattern.size() >= 15) {
      graph_request.seed_label =
          typed_pattern.substr(13, typed_pattern.size() - 14);
      const auto identifier_start = [](const unsigned char ch) {
        return (ch >= 'A' && ch <= 'Z') ||
               (ch >= 'a' && ch <= 'z') || ch == '_';
      };
      if (graph_request.seed_label.empty() ||
          !identifier_start(static_cast<unsigned char>(
              graph_request.seed_label.front())) ||
          !std::ranges::all_of(
              std::string_view(graph_request.seed_label).substr(1),
              [&](const unsigned char ch) {
                return identifier_start(ch) || (ch >= '0' && ch <= '9');
              })) {
        return refuse("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                      "GRAPH_MATCH label pattern is not a typed vertex pattern");
      }
    } else {
      return refuse("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                    "GRAPH_MATCH typed pattern is outside the exact profile");
    }
  } else {
    const auto direction =
        expression_for(graph_operation->child_expression_ids[1]);
    const auto minimum = expression_for(graph_operation->child_expression_ids[2]);
    const auto maximum = expression_for(graph_operation->child_expression_ids[3]);
    const auto cycle = expression_for(graph_operation->child_expression_ids[4]);
    std::uint64_t minimum_depth = 0;
    std::uint64_t maximum_depth = 0;
    const auto parse_depth = [](const auto& expression, std::uint64_t* value) {
      if (expression.literal_or_parameter_ref == std::nullopt) return false;
      const auto& text = *expression.literal_or_parameter_ref;
      const auto parsed =
          std::from_chars(text.data(), text.data() + text.size(), *value);
      return parsed.ec == std::errc{} &&
             parsed.ptr == text.data() + text.size();
    };
    if (direction == dag.expressions.end() ||
        minimum == dag.expressions.end() || maximum == dag.expressions.end() ||
        cycle == dag.expressions.end() ||
        !exact_literal(*direction, api::RelationalLiteralKind::kString) ||
        !exact_literal(*minimum, api::RelationalLiteralKind::kNumeric) ||
        !exact_literal(*maximum, api::RelationalLiteralKind::kNumeric) ||
        !exact_literal(*cycle, api::RelationalLiteralKind::kString) ||
        !parse_depth(*minimum, &minimum_depth) ||
        !parse_depth(*maximum, &maximum_depth) ||
        minimum_depth > maximum_depth ||
        *cycle->literal_or_parameter_ref != "visited_set") {
      return refuse("SB_MODEL_GRAPH_UNBOUNDED_EXPANSION_REFUSED_V1",
                    "GRAPH_EXPAND bounds or visited-set policy are incomplete");
    }
    std::string direction_name = *direction->literal_or_parameter_ref;
    std::ranges::transform(direction_name, direction_name.begin(),
                           [](const unsigned char ch) {
                             return static_cast<char>(std::toupper(ch));
                           });
    if (direction_name == "OUTGOING") {
      graph_request.direction = api::EngineGraphTraversalDirection::kOutgoing;
    } else if (direction_name == "INCOMING") {
      graph_request.direction = api::EngineGraphTraversalDirection::kIncoming;
    } else if (direction_name == "BOTH") {
      graph_request.direction = api::EngineGraphTraversalDirection::kBoth;
    } else {
      return refuse("SB_MODEL_GRAPH_UNBOUNDED_EXPANSION_REFUSED_V1",
                    "GRAPH_EXPAND direction is unsupported");
    }
    graph_request.min_depth = minimum_depth;
    graph_request.max_depth = maximum_depth;
  }

  api::EngineResolveStatementSnapshotRequest snapshot_request;
  snapshot_request.context = input.context;
  const auto snapshot = api::EngineResolveStatementSnapshot(snapshot_request);
  if (!snapshot.ok) {
    return refuse("SB_MODEL_MGA_CONTEXT_MISMATCH_V1",
                  "current engine MGA statement snapshot is unavailable");
  }
  const auto mga = PhysicalMgaContextFromResolvedSnapshot(
      input.context, snapshot.snapshot_vector);
  const auto authorization = api::EvaluateMaterializedAuthorization(
      input.context, input.context.authorization_context, "SELECT",
      graph_request.graph_object_uuid);
  if (!authorization.authorized || authorization.denied ||
      authorization.policy_recheck_required ||
      !authorization.diagnostics.empty()) {
    return refuse("SB_MODEL_SECURITY_ADMISSION_REFUSED_V1",
                  "graph SELECT authorization was refused");
  }
  const auto loaded_relation = api::LoadMgaRelationStorageDescriptor(
      input.context, graph_request.graph_object_uuid);
  if (!loaded_relation.ok) {
    return refuse("SB_MODEL_GRAPH_EXACT_FALLBACK_UNAVAILABLE_V1",
                  loaded_relation.diagnostic.detail.empty()
                      ? "persistent graph relation descriptor is unavailable"
                      : loaded_relation.diagnostic.detail);
  }
  const auto& persisted_relation = loaded_relation.descriptor;
  if (persisted_relation.relation_uuid.canonical !=
          graph_request.graph_object_uuid ||
      persisted_relation.database_uuid.canonical !=
          input.context.database_uuid.canonical ||
      persisted_relation.schema_uuid.canonical.empty() ||
      persisted_relation.relation_kind != "table" ||
      persisted_relation.storage_profile != "local_mga_rowstore_v1" ||
      persisted_relation.descriptor_uuid.canonical.empty() ||
      persisted_relation.descriptor_generation == 0 ||
      !api::EngineGraphDescriptorCohortExact(persisted_relation)) {
    return refuse("SB_MODEL_TYPED_EXCHANGE_INVALID_V1",
                  "persistent graph relation descriptor is invalid");
  }
  graph_request.provider_generation =
      persisted_relation.descriptor_generation;
  graph_request.target_object.uuid.canonical = graph_request.graph_object_uuid;
  graph_request.bound_object_identity.object_uuid.canonical =
      graph_request.graph_object_uuid;
  graph_request.bound_object_identity.resolved_object_type = "graph";
  graph_request.bound_object_identity.resolved_schema_uuid =
      persisted_relation.schema_uuid;
  graph_request.bound_object_identity.catalog_generation_id =
      input.context.catalog_generation_id;
  graph_request.bound_object_identity.security_epoch =
      input.context.security_epoch;
  graph_request.bound_object_identity.resource_epoch =
      input.context.resource_epoch;

  const auto identity_scope = dag.bound_sblr_tree_uuid + ":" +
                              input.context.statement_uuid.canonical;
  const auto physical_alternative_uuid = DerivedCanonicalUuid(
      identity_scope,
      "alternative." + std::to_string(scan->node_id) +
          ".physical_graph_adjacency_scan_v1");
  const auto provider_uuid = DerivedCanonicalUuid(identity_scope, "graph.provider");
  const auto capability_uuid =
      DerivedCanonicalUuid(identity_scope, "graph.capability");
  const auto result_handle_uuid =
      DerivedCanonicalUuid(identity_scope, "graph.result-handle");
  const auto property_uuid =
      DerivedCanonicalUuid(identity_scope, "graph.property");
  const auto security_receipt_uuid =
      DerivedCanonicalUuid(identity_scope, "graph.security-receipt");
  const auto policy_snapshot_uuid =
      DerivedCanonicalUuid(identity_scope, "graph.policy-snapshot");
  const auto statistics_snapshot_uuid =
      DerivedCanonicalUuid(identity_scope, "graph.statistics-snapshot");
  const auto resource_contract_uuid =
      DerivedCanonicalUuid(identity_scope, "graph.resource-contract");
  const auto generation =
      std::max<std::uint64_t>(1, input.context.catalog_generation_id);

  opt::ModelFamilyCoordinatorRequestV1 planning;
  planning.family_id = "graph";
  planning.operation_id = match ? "GRAPH_MATCH" : "GRAPH_EXPAND";
  planning.logical_operator_id = "LOGICAL_GRAPH_SOURCE_V1";
  planning.logical_node_id = scan->node_id;
  planning.object_uuid = graph_request.graph_object_uuid;
  planning.output_descriptor_ids = scan->output_descriptor_ids;
  planning.mga_statement_context = mga;
  planning.bound_sblr_tree_uuid = dag.bound_sblr_tree_uuid;
  planning.catalog_epoch_uuid = input.context.catalog_epoch_uuid.canonical;
  planning.security_context_uuid =
      input.context.authorization_context.authority_uuid.canonical;
  planning.capability_snapshot_uuid =
      input.context.optimizer_capability_snapshot_uuid.canonical;
  planning.resource_snapshot_uuid =
      input.context.optimizer_resource_snapshot_uuid.canonical;
  planning.statistics_snapshot_uuid = statistics_snapshot_uuid;
  planning.route_snapshot_uuid =
      input.context.optimizer_route_snapshot_uuid.canonical;
  planning.catalog_generation = generation;
  planning.current_catalog_generation = generation;
  planning.security_epoch =
      std::max<std::uint64_t>(1, input.context.security_epoch);
  planning.policy_epoch = std::max<std::uint64_t>(
      1, input.context.authorization_context.policy_epoch);
  planning.resource_epoch =
      std::max<std::uint64_t>(1, input.context.resource_epoch);
  planning.statistics_generation = generation;
  planning.route_epoch = input.context.optimizer_route_epoch;
  planning.route_generation = input.context.optimizer_route_generation;
  planning.memory_budget_bytes = input.context.optimizer_memory_budget_bytes;
  planning.security_admitted = input.context.security_context_present &&
                               input.context.authorization_context.present;
  std::vector<opt::ModelFamilyCapabilitySnapshotV1> alternatives;
  alternatives.push_back(MakeModelFamilyCapabilitySnapshotForCompositionV1(
      planning, identity_scope + ".graph.fallback",
      opt::ModelFamilyAlternativeRouteClassV1::kExactCollectionFallback,
      provider_uuid, capability_uuid, graph_request.provider_generation, true,
      1, 1, planning.memory_budget_bytes));
  const auto planned = PlanCanonicalModelFamilySourceForCompositionV1(
      planning, identity_scope + ".graph.inventory", std::move(alternatives));
  if (!planned.accepted || !planned.selected || !planned.data_access_allowed ||
      !planned.optimizer_owned_enumeration ||
      !planned.exact_fallback_selected ||
      planned.selected_candidate.provider_uuid != provider_uuid ||
      planned.selected_candidate.capability_uuid != capability_uuid ||
      planned.selected_candidate.provider_generation !=
          graph_request.provider_generation) {
    return refuse(planned.diagnostic_id.empty()
                      ? "SB_MODEL_GRAPH_EXACT_FALLBACK_UNAVAILABLE_V1"
                      : planned.diagnostic_id,
                  planned.detail.empty()
                      ? "graph coordinator did not select the exact candidate"
                      : planned.detail);
  }

  api::CanonicalRelationalPlanningScope planning_scope;
  planning_scope.catalog_epoch_uuid = input.context.catalog_epoch_uuid.canonical;
  planning_scope.security_context_uuid =
      input.context.authorization_context.authority_uuid.canonical;
  planning_scope.statement_uuid = input.context.statement_uuid.canonical;
  planning_scope.owning_transaction_uuid =
      input.context.transaction_uuid.canonical;
  planning_scope.statement_snapshot_uuid =
      input.context.statement_snapshot_uuid.canonical;
  planning_scope.statement_metadata_snapshot_uuid =
      input.context.statement_metadata_snapshot_uuid.canonical;
  planning_scope.local_transaction_id = input.context.local_transaction_id;
  planning_scope.snapshot_visible_through_local_transaction_id =
      input.context.snapshot_visible_through_local_transaction_id;
  planning_scope.metadata_snapshot_engine_owned =
      input.context.statement_metadata_snapshot_engine_owned;
  planning_scope.authorization_context_engine_owned =
      input.context.authorization_context.present;
  auto logical = api::PopulateCanonicalLogicalGraphFromAdmittedTypedRelationalDag(
      dag, planning_scope);
  const auto sort_consumer_count = std::ranges::count_if(
      consumer_chain, [](const auto* node) {
        return node->node_kind == api::RelationalDagNodeKind::kSort;
      });
  const auto window_consumer_count = std::ranges::count_if(
      consumer_chain, [](const auto* node) {
        return node->node_kind == api::RelationalDagNodeKind::kWindow;
      });
  if (!logical.accepted || sort_consumer_count > 1 ||
      window_consumer_count > 1 ||
      logical.property_catalog.properties.size() !=
          sort_consumer_count + window_consumer_count) {
    return refuse(logical.issues.empty()
                      ? "QOW-DIAG-OPTIMIZER-ADMISSION-BOUND-REQUEST-V1"
                      : logical.issues.front().diagnostic_id,
                  logical.issues.empty() ? "graph logical bridge was refused"
                                         : logical.issues.front().field_id);
  }
  plan::CanonicalMgaStatementContext current_logical_mga;
  current_logical_mga.statement_uuid = mga.statement_uuid;
  current_logical_mga.owning_transaction_uuid = mga.owning_transaction_uuid;
  current_logical_mga.statement_snapshot_uuid = mga.statement_snapshot_uuid;
  current_logical_mga.statement_metadata_snapshot_uuid =
      mga.statement_metadata_snapshot_uuid;
  current_logical_mga.owning_local_transaction_id =
      mga.owning_local_transaction_id;
  current_logical_mga.visible_committed_high_watermark =
      mga.visible_committed_high_watermark;
  current_logical_mga.oldest_active_transaction_id =
      mga.oldest_active_transaction_id;
  current_logical_mga.oldest_interesting_transaction_id =
      mga.oldest_interesting_transaction_id;
  current_logical_mga.oldest_snapshot_transaction_id =
      mga.oldest_snapshot_transaction_id;
  current_logical_mga.retention_horizon_transaction_id =
      mga.retention_horizon_transaction_id;
  current_logical_mga.active_excluded_local_transaction_ids =
      mga.active_excluded_local_transaction_ids;
  current_logical_mga.in_doubt_excluded_local_transaction_ids =
      mga.in_doubt_excluded_local_transaction_ids;
  current_logical_mga.snapshot_kind = mga.snapshot_kind;
  current_logical_mga.publication_inventory_next_local_transaction_id =
      mga.publication_inventory_next_local_transaction_id;
  current_logical_mga.inventory_authoritative = mga.inventory_authoritative;
  current_logical_mga.complete = mga.complete;
  current_logical_mga.current = true;
  auto registered_logical_mga = current_logical_mga;
  registered_logical_mga.current = false;
  if (!plan::CanonicalMgaStatementContextEqual(
          logical.logical_graph.mga_statement_context,
          registered_logical_mga) ||
      !plan::CanonicalMgaStatementContextEqual(
          logical.property_catalog.mga_statement_context,
          registered_logical_mga)) {
    return refuse("QOW-DIAG-QRY-004-GRAPH-OPTIMIZER-MGA-SNAPSHOT-V1",
                  "bridge_statement_snapshot_carriage");
  }
  logical.logical_graph.mga_statement_context = current_logical_mga;
  logical.property_catalog.mga_statement_context = current_logical_mga;

  opt::CanonicalNativeObjectAdmissionContext admission_context;
  admission_context.statement_uuid = input.context.statement_uuid.canonical;
  admission_context.catalog_snapshot_uuid =
      input.context.statement_metadata_snapshot_uuid.canonical;
  admission_context.security_context_uuid =
      input.context.authorization_context.authority_uuid.canonical;
  admission_context.catalog_generation = input.context.catalog_generation_id;
  admission_context.authorization_catalog_generation =
      input.context.authorization_context.catalog_generation_id;
  admission_context.security_epoch =
      input.context.authorization_context.security_epoch;
  admission_context.policy_epoch =
      input.context.authorization_context.policy_epoch;
  admission_context.resource_epoch = input.context.resource_epoch;
  admission_context.capability_snapshot_uuid =
      input.context.optimizer_capability_snapshot_uuid.canonical;
  admission_context.resource_snapshot_uuid =
      input.context.optimizer_resource_snapshot_uuid.canonical;
  admission_context.route_snapshot_uuid =
      input.context.optimizer_route_snapshot_uuid.canonical;
  admission_context.route_epoch = input.context.optimizer_route_epoch;
  admission_context.route_generation = input.context.optimizer_route_generation;
  admission_context.memory_budget_bytes =
      input.context.optimizer_memory_budget_bytes;
  admission_context.maximum_candidate_count =
      input.context.optimizer_maximum_candidate_count;
  admission_context.maximum_memo_groups =
      input.context.optimizer_maximum_memo_groups;
  admission_context.maximum_search_steps =
      input.context.optimizer_maximum_search_steps;
  admission_context.maximum_planning_time_ns =
      input.context.optimizer_maximum_planning_time_ns;
  admission_context.spill_allowed = input.context.optimizer_spill_allowed;
  admission_context.local_transaction_id = input.context.local_transaction_id;
  admission_context.statement_snapshot_id =
      input.context.snapshot_visible_through_local_transaction_id;
  admission_context.mga_statement_context = current_logical_mga;
  std::uint64_t admitted_at_monotonic_ns = 0;
  const auto monotonic_parse = std::from_chars(
      input.context.current_monotonic_ns.data(),
      input.context.current_monotonic_ns.data() +
          input.context.current_monotonic_ns.size(),
      admitted_at_monotonic_ns);
  if (input.context.current_monotonic_ns.empty() ||
      monotonic_parse.ec != std::errc{} ||
      monotonic_parse.ptr != input.context.current_monotonic_ns.data() +
                                 input.context.current_monotonic_ns.size() ||
      admitted_at_monotonic_ns == 0) {
    return refuse("QOW-DIAG-QRY-004-GRAPH-OPTIMIZER-CONTEXT-V1",
                  "current_monotonic_ns");
  }
  admission_context.admitted_at_monotonic_ns = admitted_at_monotonic_ns;
  admission_context.metadata_snapshot_engine_owned = true;
  admission_context.authorization_context_engine_owned = true;
  admission_context.catalog_object_uuids = {graph_request.graph_object_uuid};
  admission_context.authorized_object_uuids = {graph_request.graph_object_uuid};
  admission_context.catalog_object_evidence_engine_owned = true;
  admission_context.authorization_object_evidence_engine_owned = true;
  auto canonical_admission =
      opt::BuildCanonicalObjectAwareNativeOptimizerAdmissionRequest(
          logical.logical_graph, logical.property_catalog, admission_context);
  if (!canonical_admission.built ||
      !canonical_admission.admission.admitted ||
      !canonical_admission.admission.planning_allowed ||
      canonical_admission.admission.data_access_allowed ||
      canonical_admission.admission.evidence.size() != 8) {
    return refuse(canonical_admission.diagnostic_id.empty()
                      ? "QOW-DIAG-OPTIMIZER-ADMISSION-BOUND-REQUEST-V1"
                      : canonical_admission.diagnostic_id,
                  canonical_admission.field_id.empty()
                      ? "graph canonical optimizer admission was refused"
                      : canonical_admission.field_id);
  }
  result.optimizer_admitted = true;
  CanonicalObjectFreeValuesExecutionRequest canonical_planning_request{
      input.context, dag, canonical_admission.request,
      canonical_admission.admission};
  canonical_planning_request.expression_services.comparison_evaluator =
      [context = input.context](const api::EngineTypedValue& left,
                                const api::EngineTypedValue& right,
                                int* comparison,
                                std::string* diagnostic_id,
                                std::string* refusal_detail) {
        return CompareCanonicalRelationalScalarsV1(
            context, left, right, comparison, diagnostic_id, refusal_detail);
      };
  const auto descriptor_for_composition =
      [&](const std::uint32_t descriptor_id) {
        return std::ranges::find_if(
            dag.descriptors, [&](const auto& descriptor) {
              return descriptor.descriptor_id == descriptor_id;
            });
      };
  const auto expression_for_composition =
      [&](const std::uint32_t expression_id) {
        return std::ranges::find_if(
            dag.expressions, [&](const auto& expression) {
              return expression.expression_id == expression_id;
            });
      };
  std::vector<const api::RelationalOutputRecord*> producer_outputs;
  for (const auto& output : dag.outputs) {
    if (output.relation_node_id == scan->node_id) {
      producer_outputs.push_back(&output);
    }
  }
  std::ranges::sort(producer_outputs, {},
                    &api::RelationalOutputRecord::ordinal);
  MaterializedValues composition_state;
  composition_state.ok = true;
  for (std::size_t ordinal = 0; ordinal < producer_outputs.size(); ++ordinal) {
    const auto& output = *producer_outputs[ordinal];
    const auto expression = expression_for_composition(output.expression_id);
    const auto descriptor =
        descriptor_for_composition(output.descriptor_id);
    const auto persisted_column =
        expression == dag.expressions.end() ||
                !expression->bound_name_uuid.has_value()
            ? persisted_relation.columns.end()
            : std::ranges::find_if(
                  persisted_relation.columns, [&](const auto& column) {
                    return column.column_uuid.canonical ==
                           *expression->bound_name_uuid;
                  });
    if (output.ordinal != ordinal || !output.visible ||
        descriptor == dag.descriptors.end() ||
        persisted_column == persisted_relation.columns.end()) {
      return refuse("SB_MODEL_TYPED_EXCHANGE_INVALID_V1",
                    "graph producer composition descriptor is incomplete");
    }
    api::EngineDescriptor runtime_descriptor;
    runtime_descriptor.descriptor_uuid.canonical =
        descriptor->descriptor_uuid;
    runtime_descriptor.descriptor_kind = "scalar";
    runtime_descriptor.canonical_type_name =
        persisted_column->value_descriptor.canonical_type_name;
    runtime_descriptor.encoded_descriptor =
        "type_uuid=" + descriptor->type_uuid + ";nullability=" +
        (descriptor->nullability == api::RelationalNullability::kNullable
             ? "nullable"
             : "non_null");
    composition_state.batch.columns.push_back(
        {output.output_name_utf8, std::move(runtime_descriptor),
         descriptor->nullability == api::RelationalNullability::kNullable,
         descriptor->descriptor_id});
    exec::CanonicalResultColumnDescriptor published;
    published.ordinal = static_cast<std::uint32_t>(ordinal);
    published.name_utf8 = output.output_name_utf8;
    published.descriptor_uuid = descriptor->descriptor_uuid;
    published.type_uuid = descriptor->type_uuid;
    published.nullability = ResultNullability(descriptor->nullability);
    published.collation_uuid = descriptor->collation_uuid;
    published.timezone_profile_id = descriptor->timezone_profile_id;
    composition_state.result_bindings.push_back(
        {ordinal, true, std::move(published)});
  }
  const auto core_manifest = dt::LoadCurrentCoreDatatypeCatalogManifest();
  if (!core_manifest.ok()) {
    return refuse("SB_MODEL_TYPED_EXCHANGE_INVALID_V1",
                  "current core datatype descriptor manifest is unavailable");
  }
  const auto type_uuid_for = [](const std::string_view stable_name) {
    return ExactCanonicalCoreDatatypeTypeUuidV1(stable_name);
  };
  LivePhysicalNodeProfile profile;
  profile.logical_node_id = scan->node_id;
  profile.implementation_id = "physical_graph_adjacency_scan_v1";
  profile.capability_uuid = planned.selected_candidate.capability_uuid;
  profile.logical_node_kind =
      plan::CanonicalLogicalRelationalNodeKind::kRelationSource;
  profile.physical_node_kind = exec::PhysicalNodeKind::kScan;
  profile.transformation_rule_id = "canonical.graph.adjacency-scan.v1";
  profile.estimated_rows = 1;
  profile.memory_bytes_required = std::max<std::uint64_t>(
      1, planned.selected_candidate.cost.memory_bytes_required);
  profile.page_read_sequential_units = 1;
  profile.mga_visibility_checks_expected = 1;
  profile.storage_read_capable = true;
  profile.mga_visibility_capable = true;
  profile.residual_predicate_required = true;
  profile.storage_recheck_required = true;
  profile.compatibility_profile_id = "graph.local.v1";
  std::vector<LivePhysicalNodeProfile> profiles{std::move(profile)};
  const auto logical_node_for = [&](const std::uint32_t node_id) {
    return std::ranges::find_if(
        canonical_admission.request.logical_graph.nodes,
        [&](const auto& node) { return node.logical_node_id == node_id; });
  };
  auto previous_logical = logical_node_for(scan->node_id);
  if (previous_logical ==
      canonical_admission.request.logical_graph.nodes.end()) {
    return refuse("SBLR.PLAN_TREE.INVALID_HANDLE",
                  "graph producer logical identity is absent");
  }
  bool prepared_nonrecursive_cte = false;
  std::optional<PreparedFilterRoot> prepared_filter;
  std::optional<PreparedProjectRoot> prepared_project;
  std::vector<std::size_t> direct_projected_columns;
  std::optional<PreparedSortRoot> prepared_sort;
  std::optional<exec::ExecutorColumnDescriptor> prepared_row_number;
  std::optional<PreparedGlobalAggregateRoot> prepared_count_star;
  std::optional<PreparedRecursiveCteRoot> prepared_recursive_cte;
  std::optional<PreparedLiveSetNode> prepared_graph_set;
  std::optional<MaterializedValues> materialized_graph_set_values;
  std::uint64_t graph_set_values_memory_bytes = 0;
  std::optional<PreparedLimitRoot> prepared_limit;
  std::uint64_t limit_count = 0;
  std::uint64_t limit_offset = 0;
  bool fetch_first_rows_only = false;
  std::string limit_implementation_id;
  std::string nonrecursive_cte_implementation_id;
  std::string cte_capability_uuid;
  std::string filter_capability_uuid;
  std::string project_capability_uuid;
  std::string sort_capability_uuid;
  std::string window_capability_uuid;
  std::string window_order_evidence_uuid;
  std::string aggregate_capability_uuid;
  std::string recursive_term_capability_uuid;
  std::string recursive_root_capability_uuid;
  std::string set_values_capability_uuid;
  std::string set_root_capability_uuid;
  std::string limit_capability_uuid;
  std::unordered_set<api::RelationalDagNodeKind> consumer_kinds;
  for (const auto* consumer : consumer_chain) {
    const auto logical_consumer = logical_node_for(consumer->node_id);
    if (logical_consumer ==
            canonical_admission.request.logical_graph.nodes.end() ||
        consumer->input_node_ids !=
            std::vector<std::uint32_t>{previous_logical->logical_node_id} ||
        logical_consumer->input_logical_node_ids !=
            std::vector<std::uint32_t>{previous_logical->logical_node_id} ||
        !consumer_kinds.insert(consumer->node_kind).second) {
      return refuse("SBLR.PLAN_TREE.INVALID_HANDLE",
                    "graph consumer identity or ordering is invalid");
    }
    LivePhysicalNodeProfile consumer_profile;
    consumer_profile.logical_node_id = consumer->node_id;
    consumer_profile.logical_node_kind = logical_consumer->node_kind;
    consumer_profile.estimated_rows = 4096;
    consumer_profile.memory_bytes_required = 1;
    consumer_profile.minimum_input_count = 1;
    consumer_profile.maximum_input_count = 1;
    consumer_profile.runtime_peak_from_callback_batches = true;
    if (consumer->node_kind == api::RelationalDagNodeKind::kFilter) {
      if (consumer->semantic_variant_id != "filter.where.v1") {
        return refuse("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                      "graph FILTER semantic is not canonical");
      }
      auto prepared = PrepareFilterRootForComposition(
          dag, *logical_consumer, *previous_logical, composition_state);
      if (!prepared.ok) {
        return refuse("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                      prepared.detail);
      }
      prepared_filter = std::move(prepared);
      filter_capability_uuid = DerivedCanonicalUuid(
          identity_scope, "graph.filter.capability");
      consumer_profile.implementation_id = "filter.3vl.row.v1";
      consumer_profile.capability_uuid = filter_capability_uuid;
      consumer_profile.physical_node_kind = exec::PhysicalNodeKind::kFilter;
      consumer_profile.transformation_rule_id =
          "canonical.graph.filter.3vl.v1";
      consumer_profile.memory_bytes_required = planning.memory_budget_bytes;
    } else if (consumer->node_kind == api::RelationalDagNodeKind::kProject) {
      if (consumer->semantic_variant_id != "project.select-list.v1" ||
          consumer->bound_expression_ids.empty()) {
        return refuse("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                      "graph PROJECT semantic is not canonical");
      }
      project_capability_uuid = DerivedCanonicalUuid(
          identity_scope, "graph.project.capability");
      std::vector<const api::RelationalOutputRecord*> project_outputs;
      for (const auto& output : dag.outputs) {
        if (output.relation_node_id == consumer->node_id) {
          project_outputs.push_back(&output);
        }
      }
      std::ranges::sort(project_outputs, {},
                        &api::RelationalOutputRecord::ordinal);
      bool descriptor_direct =
          project_outputs.size() == consumer->bound_expression_ids.size() &&
          project_outputs.size() == consumer->output_descriptor_ids.size();
      std::vector<exec::ExecutorColumnDescriptor> projected_columns;
      std::vector<exec::CanonicalResultColumnBinding> projected_bindings;
      for (std::size_t ordinal = 0;
           descriptor_direct && ordinal < project_outputs.size(); ++ordinal) {
        const auto expression = expression_for_composition(
            consumer->bound_expression_ids[ordinal]);
        const auto source_descriptor = std::ranges::find(
            previous_logical->output_descriptor_ids,
            consumer->output_descriptor_ids[ordinal]);
        descriptor_direct =
            expression != dag.expressions.end() &&
            expression->expression_kind ==
                api::RelationalExpressionKind::kIdentifier &&
            expression->result_descriptor_id ==
                consumer->output_descriptor_ids[ordinal] &&
            project_outputs[ordinal]->ordinal == ordinal &&
            project_outputs[ordinal]->expression_id ==
                expression->expression_id &&
            project_outputs[ordinal]->descriptor_id ==
                expression->result_descriptor_id &&
            source_descriptor !=
                previous_logical->output_descriptor_ids.end();
        if (!descriptor_direct) break;
        const auto source_ordinal = static_cast<std::size_t>(std::distance(
            previous_logical->output_descriptor_ids.begin(),
            source_descriptor));
        if (source_ordinal >= composition_state.batch.columns.size()) {
          descriptor_direct = false;
          break;
        }
        direct_projected_columns.push_back(source_ordinal);
        projected_columns.push_back(
            composition_state.batch.columns[source_ordinal]);
        exec::CanonicalResultColumnBinding binding;
        binding.physical_column_ordinal = ordinal;
        binding.visible = project_outputs[ordinal]->visible;
        if (binding.visible) {
          const auto descriptor = descriptor_for_composition(
              project_outputs[ordinal]->descriptor_id);
          if (descriptor == dag.descriptors.end()) {
            descriptor_direct = false;
            break;
          }
          binding.published_descriptor =
              exec::CanonicalResultColumnDescriptor{
                  static_cast<std::uint32_t>(ordinal),
                  project_outputs[ordinal]->output_name_utf8,
                  descriptor->descriptor_uuid, descriptor->type_uuid,
                  ResultNullability(descriptor->nullability),
                  descriptor->collation_uuid,
                  descriptor->timezone_profile_id};
        }
        projected_bindings.push_back(std::move(binding));
      }
      if (descriptor_direct) {
        composition_state.batch.columns = std::move(projected_columns);
        composition_state.result_bindings = std::move(projected_bindings);
        consumer_profile.implementation_id =
            "project.descriptor-direct.v1";
      } else {
        direct_projected_columns.clear();
        auto prepared = PrepareExpressionProjectRootForComposition(
            dag, *logical_consumer, *previous_logical, composition_state,
            canonical_planning_request.expression_services);
        if (!prepared.ok) {
          return refuse("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                        prepared.detail);
        }
        composition_state.batch = prepared.expression_output_batch;
        composition_state.result_bindings = prepared.result_bindings;
        prepared_project = std::move(prepared);
        consumer_profile.implementation_id =
            "project.typed.expression-row.v1";
      }
      consumer_profile.capability_uuid = project_capability_uuid;
      consumer_profile.physical_node_kind = exec::PhysicalNodeKind::kProject;
      consumer_profile.transformation_rule_id =
          descriptor_direct
              ? "canonical.graph.project.descriptor-direct.v1"
              : "canonical.graph.project.expression-row.v1";
      consumer_profile.memory_bytes_required = planning.memory_budget_bytes;
    } else if (consumer->node_kind == api::RelationalDagNodeKind::kSort) {
      if (consumer->semantic_variant_id != "sort.required-order.v1") {
        return refuse("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                      "graph SORT semantic is not canonical");
      }
      const bool expression_ordering = std::ranges::any_of(
          consumer->bound_expression_ids,
          [&](const std::uint32_t expression_id) {
            const auto expression =
                expression_for_composition(expression_id);
            return expression == dag.expressions.end() ||
                   std::ranges::find(
                       previous_logical->output_descriptor_ids,
                       expression->result_descriptor_id) ==
                       previous_logical->output_descriptor_ids.end();
          });
      auto sort_properties =
          canonical_admission.request.logical_properties;
      std::erase_if(sort_properties.properties, [&](const auto& property) {
        return std::ranges::find(
                   logical_consumer->required_property_uuids,
                   property.property_uuid) ==
               logical_consumer->required_property_uuids.end();
      });
      auto prepared =
          expression_ordering
              ? PrepareExpressionSortRootForComposition(
                    input.context, dag, sort_properties,
                    *logical_consumer, *previous_logical,
                    composition_state,
                    canonical_planning_request.expression_services)
              : PrepareSortRootForComposition(input.context, dag, sort_properties,
                                *logical_consumer, *previous_logical,
                                composition_state);
      if (!prepared.ok) {
        return refuse("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                      prepared.detail);
      }
      prepared_sort = std::move(prepared);
      sort_capability_uuid = DerivedCanonicalUuid(
          identity_scope, "graph.sort.capability");
      consumer_profile.implementation_id =
          prepared_sort->expression_ordering
              ? "sort.typed.expression-row.v1"
              : "sort.typed.terms.v1";
      consumer_profile.capability_uuid = sort_capability_uuid;
      consumer_profile.physical_node_kind = exec::PhysicalNodeKind::kSort;
      consumer_profile.transformation_rule_id =
          prepared_sort->expression_ordering
              ? "canonical.graph.sort.expression-order.v1"
              : "canonical.graph.sort.required-order.v1";
      consumer_profile.required_property_uuids.clear();
      consumer_profile.delivered_property_uuids =
          logical_consumer->delivered_property_uuids;
      consumer_profile.supported_property_kinds = {
          plan::CanonicalLogicalPropertyKind::kOrdering};
      consumer_profile.memory_bytes_required = planning.memory_budget_bytes;
    } else if (consumer->node_kind == api::RelationalDagNodeKind::kWindow) {
      constexpr std::string_view kRowNumberFunctionUuid =
          "019de5fc-2400-7539-bcce-00eef3ae7220";
      std::vector<const api::RelationalWindowDefinitionRecord*> definitions;
      std::vector<const api::RelationalWindowInvocationRecord*> invocations;
      std::vector<const api::RelationalOutputRecord*> window_outputs;
      for (const auto& definition : dag.window_definitions) {
        if (definition.relation_node_id == consumer->node_id) {
          definitions.push_back(&definition);
        }
      }
      for (const auto& invocation : dag.window_invocations) {
        if (invocation.relation_node_id == consumer->node_id) {
          invocations.push_back(&invocation);
        }
      }
      for (const auto& output : dag.outputs) {
        if (output.relation_node_id == consumer->node_id) {
          window_outputs.push_back(&output);
        }
      }
      std::ranges::sort(window_outputs, {},
                        &api::RelationalOutputRecord::ordinal);
      const auto function =
          invocations.size() == 1
              ? expression_for_composition(
                    invocations.front()->function_expression_id)
              : dag.expressions.end();
      const auto row_number_descriptor = descriptor_for_composition(
          invocations.size() == 1
              ? invocations.front()->result_descriptor_id
              : 0);
      const auto int64_type_uuid = type_uuid_for("int64");
      const auto typed_sort = typed_node_for(previous_logical->logical_node_id);
      const bool ordered_input =
          previous_logical->node_kind ==
              plan::CanonicalLogicalRelationalNodeKind::kSort &&
          typed_sort != dag.nodes.end() &&
          typed_sort->bound_expression_ids.size() == 1 &&
          prepared_sort.has_value() &&
          consumer->required_property_uuids ==
              std::vector<std::string>{
                  prepared_sort->ordering_property_uuid} &&
          consumer->delivered_property_uuids.size() == 2 &&
          std::ranges::find(consumer->delivered_property_uuids,
                            prepared_sort->ordering_property_uuid) !=
              consumer->delivered_property_uuids.end();
      bool passthrough_outputs =
          window_outputs.size() ==
              previous_logical->output_descriptor_ids.size() + 1 &&
          composition_state.batch.columns.size() ==
              previous_logical->output_descriptor_ids.size() &&
          composition_state.result_bindings.size() ==
              previous_logical->output_descriptor_ids.size();
      for (std::size_t ordinal = 0;
           passthrough_outputs &&
           ordinal < previous_logical->output_descriptor_ids.size();
           ++ordinal) {
        passthrough_outputs =
            window_outputs[ordinal]->ordinal == ordinal &&
            window_outputs[ordinal]->visible &&
            window_outputs[ordinal]->descriptor_id ==
                previous_logical->output_descriptor_ids[ordinal];
      }
      if (consumer->semantic_variant_id != "window.row-number.v1" ||
          consumer->node_id != dag.root_node_id || !ordered_input ||
          definitions.size() != 1 || invocations.size() != 1 ||
          !passthrough_outputs ||
          !consumer->required_object_uuids.empty() ||
          consumer->bound_expression_ids !=
              std::vector<std::uint32_t>{
                  typed_sort->bound_expression_ids.front(),
                  invocations.front()->function_expression_id} ||
          definitions.front()->canonical_name_key.has_value() ||
          definitions.front()->inherited_window_id.has_value() ||
          !definitions.front()->partition_expression_ids.empty() ||
          definitions.front()->ordering_terms.size() != 1 ||
          definitions.front()->ordering_terms.front().expression_id !=
              typed_sort->bound_expression_ids.front() ||
          definitions.front()->frame_unit.has_value() ||
          definitions.front()->frame_start.has_value() ||
          definitions.front()->frame_end.has_value() ||
          definitions.front()->exclusion !=
              api::RelationalWindowFrameExclusion::kNoOthers ||
          invocations.front()->window_definition_id !=
              definitions.front()->window_id ||
          invocations.front()->function_abi_version != 1 ||
          invocations.front()->builtin_id != "sb.window.row_number" ||
          invocations.front()->function_uuid != kRowNumberFunctionUuid ||
          !invocations.front()->argument_expression_ids.empty() ||
          function == dag.expressions.end() ||
          function->expression_kind !=
              api::RelationalExpressionKind::kFunctionCall ||
          function->function_uuid !=
              std::optional<std::string>(kRowNumberFunctionUuid) ||
          function->bound_name_uuid.has_value() ||
          function->operator_name.has_value() ||
          function->literal_kind.has_value() ||
          function->literal_or_parameter_ref.has_value() ||
          !function->child_expression_ids.empty() ||
          function->result_descriptor_id !=
              invocations.front()->result_descriptor_id ||
          row_number_descriptor == dag.descriptors.end() ||
          int64_type_uuid.empty() ||
          row_number_descriptor->type_uuid != int64_type_uuid ||
          row_number_descriptor->nullability !=
              api::RelationalNullability::kNonNull ||
          row_number_descriptor->collation_uuid.has_value() ||
          row_number_descriptor->timezone_profile_id.has_value() ||
          row_number_descriptor->width.has_value() ||
          row_number_descriptor->precision.has_value() ||
          row_number_descriptor->scale.has_value() ||
          consumer->output_descriptor_ids.size() !=
              previous_logical->output_descriptor_ids.size() + 1 ||
          !std::equal(previous_logical->output_descriptor_ids.begin(),
                      previous_logical->output_descriptor_ids.end(),
                      consumer->output_descriptor_ids.begin()) ||
          consumer->output_descriptor_ids.back() !=
              row_number_descriptor->descriptor_id ||
          logical_consumer->output_descriptor_ids !=
              consumer->output_descriptor_ids ||
          window_outputs.back()->ordinal !=
              previous_logical->output_descriptor_ids.size() ||
          !window_outputs.back()->visible ||
          window_outputs.back()->expression_id != function->expression_id ||
          window_outputs.back()->descriptor_id !=
              row_number_descriptor->descriptor_id ||
          window_outputs.back()->output_name_utf8 !=
              invocations.front()->output_name_utf8) {
        return refuse("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                      "graph ROW_NUMBER window binding is not exact");
      }
      const auto window_property_uuid = std::ranges::find_if(
          consumer->delivered_property_uuids, [&](const auto& property_uuid) {
            return property_uuid != prepared_sort->ordering_property_uuid;
          });
      const auto window_property = std::ranges::find_if(
          canonical_admission.request.logical_properties.properties,
          [&](const auto& property) {
            return window_property_uuid !=
                       consumer->delivered_property_uuids.end() &&
                   property.property_uuid == *window_property_uuid;
          });
      if (window_property_uuid == consumer->delivered_property_uuids.end() ||
          window_property == canonical_admission.request.logical_properties
                                 .properties.end() ||
          window_property->property_kind !=
              plan::CanonicalLogicalPropertyKind::kWindow ||
          window_property->origin_logical_node_id != consumer->node_id ||
          window_property->dependency_property_uuids !=
              std::vector<std::string>{
                  prepared_sort->ordering_property_uuid} ||
          window_property->window_frame_descriptor_uuid.empty()) {
        return refuse("QOW-DIAG-WINDOW-PROPERTY-CARRIAGE-V1",
                      "graph ROW_NUMBER property binding is not exact");
      }
      if (row_number_descriptor->descriptor_uuid ==
              row_number_descriptor->type_uuid ||
          row_number_descriptor->descriptor_uuid == kRowNumberFunctionUuid ||
          row_number_descriptor->descriptor_uuid ==
              prepared_sort->ordering_property_uuid ||
          row_number_descriptor->descriptor_uuid == *window_property_uuid ||
          row_number_descriptor->descriptor_uuid ==
              window_property->window_frame_descriptor_uuid) {
        return refuse(
            "SB_MODEL_TYPED_EXCHANGE_INVALID_V1",
            "graph ROW_NUMBER result descriptor identity is not independent");
      }
      api::EngineDescriptor row_number_runtime;
      row_number_runtime.descriptor_uuid.canonical =
          row_number_descriptor->descriptor_uuid;
      row_number_runtime.descriptor_kind = "scalar";
      row_number_runtime.canonical_type_name = "int64";
      row_number_runtime.encoded_descriptor =
          "type_uuid=" + row_number_descriptor->type_uuid +
          ";nullability=non_null";
      exec::ExecutorColumnDescriptor row_number_column{
          window_outputs.back()->output_name_utf8,
          std::move(row_number_runtime), false,
          row_number_descriptor->descriptor_id};
      if (!api::QowCanonicalDescriptorIdentityV1(
              row_number_column.descriptor)) {
        return refuse("SB_MODEL_TYPED_EXCHANGE_INVALID_V1",
                      "graph ROW_NUMBER descriptor is invalid");
      }
      exec::CanonicalResultColumnBinding row_number_binding;
      row_number_binding.physical_column_ordinal =
          composition_state.batch.columns.size();
      row_number_binding.visible = true;
      row_number_binding.published_descriptor =
          exec::CanonicalResultColumnDescriptor{
              static_cast<std::uint32_t>(
                  composition_state.batch.columns.size()),
              window_outputs.back()->output_name_utf8,
              row_number_descriptor->descriptor_uuid,
              row_number_descriptor->type_uuid,
              exec::CanonicalResultNullability::kNonNull,
              std::nullopt, std::nullopt};
      composition_state.batch.columns.push_back(row_number_column);
      composition_state.result_bindings.push_back(
          std::move(row_number_binding));
      prepared_row_number = std::move(row_number_column);
      window_capability_uuid = DerivedCanonicalUuid(
          identity_scope, "graph.window.row-number.capability");
      window_order_evidence_uuid = DerivedCanonicalUuid(
          identity_scope + ":" + prepared_sort->ordering_property_uuid,
          "graph.window.deterministic-order");
      consumer_profile.implementation_id = "window.row-number.v1";
      consumer_profile.capability_uuid = window_capability_uuid;
      consumer_profile.physical_node_kind = exec::PhysicalNodeKind::kWindow;
      consumer_profile.transformation_rule_id =
          "canonical.graph.window.row-number.v1";
      consumer_profile.required_property_uuids =
          logical_consumer->required_property_uuids;
      consumer_profile.delivered_property_uuids =
          logical_consumer->delivered_property_uuids;
      consumer_profile.supported_property_kinds = {
          plan::CanonicalLogicalPropertyKind::kOrdering,
          plan::CanonicalLogicalPropertyKind::kWindow};
      consumer_profile.memory_bytes_required = planning.memory_budget_bytes;
    } else if (consumer->node_kind ==
               api::RelationalDagNodeKind::kAggregate) {
      const auto aggregate_profile = MatchLiveUnaryAggregateExpressionProfileForComposition(
          consumer->semantic_variant_id);
      if (!aggregate_profile.matched || !aggregate_profile.count_star ||
          aggregate_profile.function !=
              exec::CanonicalAggregateFunction::count ||
          aggregate_profile.distinct || aggregate_profile.has_filter) {
        return refuse("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                      "graph aggregate is not exact global COUNT(*)");
      }
      const auto count_descriptor = descriptor_for_composition(
          consumer->output_descriptor_ids.empty()
              ? 0
              : consumer->output_descriptor_ids.front());
      const auto int64_type_uuid = type_uuid_for("int64");
      if (consumer->output_descriptor_ids.size() != 1 ||
          count_descriptor == dag.descriptors.end() ||
          int64_type_uuid.empty() ||
          count_descriptor->type_uuid != int64_type_uuid ||
          count_descriptor->nullability !=
              api::RelationalNullability::kNonNull ||
          count_descriptor->collation_uuid.has_value() ||
          count_descriptor->timezone_profile_id.has_value() ||
          count_descriptor->width.has_value() ||
          count_descriptor->precision.has_value() ||
          count_descriptor->scale.has_value()) {
        return refuse(
            "SB_MODEL_TYPED_EXCHANGE_INVALID_V1",
            "graph COUNT(*) result descriptor is not canonical int64");
      }
      auto prepared = PrepareGlobalAggregateRootForComposition(
          dag, *logical_consumer, *previous_logical, composition_state,
          aggregate_profile.function, true, false, false);
      if (!prepared.ok) {
        return refuse("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                      prepared.detail);
      }
      composition_state.batch.columns = {prepared.result_column};
      composition_state.result_bindings = prepared.result_bindings;
      prepared_count_star = std::move(prepared);
      aggregate_capability_uuid = DerivedCanonicalUuid(
          identity_scope, "graph.count-star.capability");
      consumer_profile.implementation_id = "aggregate.count-star.v1";
      consumer_profile.capability_uuid = aggregate_capability_uuid;
      consumer_profile.physical_node_kind =
          exec::PhysicalNodeKind::kAggregate;
      consumer_profile.transformation_rule_id =
          aggregate_profile.transformation_id;
      consumer_profile.estimated_rows = 1;
      consumer_profile.memory_bytes_required = planning.memory_budget_bytes;
    } else if (consumer->node_kind == api::RelationalDagNodeKind::kCte) {
      if (consumer->semantic_variant_id != "cte.bound.v1" ||
          !consumer->bound_expression_ids.empty() ||
          !consumer->required_object_uuids.empty() ||
          !consumer->required_property_uuids.empty() ||
          !consumer->delivered_property_uuids.empty() ||
          consumer->output_descriptor_ids !=
              previous_logical->output_descriptor_ids ||
          logical_consumer->output_descriptor_ids !=
              previous_logical->output_descriptor_ids ||
          composition_state.batch.columns.size() !=
              consumer->output_descriptor_ids.size()) {
        return refuse(
            "SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
            "graph nonrecursive CTE is not an exact schema-preserving bound CTE");
      }
      prepared_nonrecursive_cte = true;
      const auto validated = exec::ValidateCanonicalDescriptorBatch(
          composition_state.batch, consumer->output_descriptor_ids);
      if (!validated.ok) {
        return refuse("SB_MODEL_TYPED_EXCHANGE_INVALID_V1",
                      "graph nonrecursive CTE input: " + validated.detail);
      }
      nonrecursive_cte_implementation_id =
          consumer->shareable ? "cte.bound.materialize.typed.v1"
                              : "cte.bound.inline.typed.v1";
      cte_capability_uuid = DerivedCanonicalUuid(
          identity_scope,
          consumer->shareable ? "graph.cte.materialize.capability"
                              : "graph.cte.inline.capability");
      consumer_profile.implementation_id =
          nonrecursive_cte_implementation_id;
      consumer_profile.capability_uuid = cte_capability_uuid;
      consumer_profile.physical_node_kind = exec::PhysicalNodeKind::kCte;
      consumer_profile.transformation_rule_id =
          consumer->shareable
              ? "canonical.graph.cte.composed-materialize.v1"
              : "canonical.graph.cte.composed-inline.v1";
      consumer_profile.memory_bytes_required = planning.memory_budget_bytes;
      consumer_profile.runtime_auxiliary_from_first_input_batch =
          consumer->shareable;
    } else if (consumer->node_kind == api::RelationalDagNodeKind::kLimit) {
      const auto expected_arity =
          consumer->semantic_variant_id == "limit.bound-count.v1" ? 1U : 2U;
      fetch_first_rows_only =
          consumer->semantic_variant_id ==
          "fetch.first-rows-only-offset.v1";
      if ((consumer->semantic_variant_id != "limit.bound-count.v1" &&
           consumer->semantic_variant_id != "limit.bound-count-offset.v1" &&
           !fetch_first_rows_only) ||
          consumer->bound_expression_ids.size() != expected_arity) {
        return refuse("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                      "graph LIMIT/FETCH semantic is not canonical");
      }
      CanonicalRelationalExpressionRuntime runtime(
          dag, canonical_planning_request.expression_services);
      std::string detail;
      if (!EvaluateNonNegativeRowBoundForComposition(
              &runtime, consumer->bound_expression_ids.front(),
              &limit_count, &detail) ||
          (expected_arity == 2 &&
           !EvaluateNonNegativeRowBoundForComposition(
               &runtime, consumer->bound_expression_ids.back(),
               &limit_offset, &detail))) {
        return refuse("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                      detail.empty() ? "graph LIMIT/FETCH bound is invalid"
                                     : detail);
      }
      auto prepared = PrepareLimitRootForComposition(
          dag, *logical_consumer, *previous_logical, composition_state);
      if (!prepared.ok) {
        return refuse("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                      prepared.detail);
      }
      prepared_limit = std::move(prepared);
      limit_implementation_id = fetch_first_rows_only
                                    ? "fetch.native.rows-only.v1"
                                    : "limit.typed.v1";
      limit_capability_uuid = DerivedCanonicalUuid(
          identity_scope, "graph.limit.capability");
      consumer_profile.implementation_id = limit_implementation_id;
      consumer_profile.capability_uuid = limit_capability_uuid;
      consumer_profile.physical_node_kind = exec::PhysicalNodeKind::kLimit;
      consumer_profile.transformation_rule_id =
          fetch_first_rows_only
              ? "canonical.graph.fetch.first-rows-only.v1"
              : "canonical.graph.limit.bound-count-offset.v1";
      consumer_profile.memory_bytes_required = planning.memory_budget_bytes;
    } else {
      return refuse("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                    "graph consumer kind is not yet prepared");
    }
    profiles.push_back(std::move(consumer_profile));
    previous_logical = logical_consumer;
  }
  constexpr std::size_t kGraphCompositionRowBound = 4096;
  if (set_root != nullptr) {
    const auto logical_values = logical_node_for(set_values->node_id);
    const auto logical_root = logical_node_for(set_root->node_id);
    if (logical_values ==
            canonical_admission.request.logical_graph.nodes.end() ||
        logical_root ==
            canonical_admission.request.logical_graph.nodes.end() ||
        logical_values->node_kind !=
            plan::CanonicalLogicalRelationalNodeKind::kValues ||
        logical_values->semantic_variant_id != "values.literal-table.v1" ||
        !logical_values->input_logical_node_ids.empty() ||
        logical_root->node_kind !=
            plan::CanonicalLogicalRelationalNodeKind::kSetOperation ||
        logical_root->semantic_variant_id != "set-operation.union-all.v1" ||
        logical_root->input_logical_node_ids !=
            std::vector<std::uint32_t>{previous_logical->logical_node_id,
                                       logical_values->logical_node_id}) {
      return refuse(
          "SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
          "graph UNION ALL logical input identity is not exact");
    }
    auto right = MaterializeValues(
        dag, *logical_values,
        canonical_planning_request.expression_services);
    if (!right.ok) {
      return refuse("SB_MODEL_TYPED_EXCHANGE_INVALID_V1",
                    "graph UNION ALL right VALUES: " + right.detail);
    }
    if (right.batch.rows.empty() ||
        right.batch.rows.size() >= kGraphCompositionRowBound) {
      return refuse(
          "QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1",
          "graph UNION ALL leaves no bounded source row budget");
    }
    auto prepared = PrepareSetOperationRootForComposition(
        input.context, dag, *logical_root, composition_state, right,
        graph_set_profile);
    if (!prepared.ok) {
      return refuse("SB_MODEL_TYPED_EXCHANGE_INVALID_V1",
                    "graph UNION ALL: " + prepared.detail);
    }
    std::uint64_t comparison_bound = 0;
    std::uint64_t collation_comparison_count = 0;
    if (!BoundSetOperationEqualityComparisonsForComposition(
            prepared, graph_set_profile, kGraphCompositionRowBound,
            &comparison_bound, &collation_comparison_count) ||
        comparison_bound > std::numeric_limits<std::size_t>::max()) {
      return refuse(
          "QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
          "graph UNION ALL comparison bound overflowed");
    }
    std::uint64_t values_memory = 1;
    if (!AddBatchMemoryBytes(right.batch, &values_memory) ||
        values_memory > planning.memory_budget_bytes) {
      return refuse("SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                    "graph UNION ALL VALUES memory bound was exceeded");
    }
    graph_set_values_memory_bytes = values_memory;
    set_values_capability_uuid = DerivedCanonicalUuid(
        identity_scope, "graph.set-values.capability");
    set_root_capability_uuid = DerivedCanonicalUuid(
        identity_scope, "graph.set-union-all.capability");

    LivePhysicalNodeProfile values_profile;
    values_profile.logical_node_id = logical_values->logical_node_id;
    values_profile.implementation_id = std::string(kValuesImplementationId);
    values_profile.capability_uuid = set_values_capability_uuid;
    values_profile.logical_node_kind = logical_values->node_kind;
    values_profile.physical_node_kind = exec::PhysicalNodeKind::kValues;
    values_profile.transformation_rule_id =
        "canonical.graph.set-values.materialize.v1";
    values_profile.estimated_rows = right.batch.rows.size();
    values_profile.memory_bytes_required = values_memory;
    profiles.push_back(std::move(values_profile));

    LivePhysicalNodeProfile set_profile;
    set_profile.logical_node_id = logical_root->logical_node_id;
    set_profile.implementation_id = graph_set_profile.implementation_id;
    set_profile.capability_uuid = set_root_capability_uuid;
    set_profile.logical_node_kind = logical_root->node_kind;
    set_profile.physical_node_kind = exec::PhysicalNodeKind::kSetOperation;
    set_profile.transformation_rule_id =
        "canonical.graph.set.union-all.ordinal.v1";
    set_profile.estimated_rows = kGraphCompositionRowBound;
    set_profile.memory_bytes_required = planning.memory_budget_bytes;
    set_profile.minimum_input_count = 2;
    set_profile.maximum_input_count = 2;
    set_profile.runtime_peak_from_callback_batches = true;
    profiles.push_back(std::move(set_profile));

    composition_state.batch.columns = prepared.result_columns;
    composition_state.result_bindings = prepared.result_bindings;
    prepared_graph_set = PreparedLiveSetNode{
        graph_set_profile, std::move(prepared), kGraphCompositionRowBound,
        std::max<std::size_t>(
            1, static_cast<std::size_t>(comparison_bound))};
    materialized_graph_set_values = std::move(right);
    previous_logical = logical_root;
  }
  if (recursive_root != nullptr) {
    const auto logical_term = logical_node_for(recursive_term->node_id);
    const auto logical_root = logical_node_for(recursive_root->node_id);
    if (logical_term ==
            canonical_admission.request.logical_graph.nodes.end() ||
        logical_root ==
            canonical_admission.request.logical_graph.nodes.end() ||
        !logical_term->input_logical_node_ids.empty() ||
        logical_root->input_logical_node_ids !=
            std::vector<std::uint32_t>{previous_logical->logical_node_id,
                                       logical_term->logical_node_id} ||
        composition_state.batch.columns.size() != 1 ||
        composition_state.batch.columns.front().descriptor
                .canonical_type_name != "int64") {
      return refuse(
          "SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
          "graph recursive logical branch is not an exact int64 anchor and empty term");
    }
    CanonicalRelationalExpressionRuntime runtime(
        dag, canonical_planning_request.expression_services);
    std::uint64_t upper_bound = 0;
    std::string detail;
    if (!EvaluateNonNegativeRowBoundForComposition(
            &runtime, recursive_root->bound_expression_ids.front(),
            &upper_bound, &detail) ||
        upper_bound >= kGraphCompositionRowBound ||
        upper_bound >=
            static_cast<std::uint64_t>(
                std::numeric_limits<std::size_t>::max())) {
      return refuse(
          "QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1",
          detail.empty()
              ? "graph recursive upper bound exceeds its admitted work bound"
              : detail);
    }
    PreparedRecursiveCteRoot prepared;
    prepared.profile = MatchLiveRecursiveCteProfileForComposition(
        recursive_root->semantic_variant_id);
    prepared.anchor_columns = composition_state.batch.columns;
    prepared.term = PrepareLiveRecursiveCteTerm(
        prepared.profile, prepared.anchor_columns,
        static_cast<std::int64_t>(upper_bound));
    if (!BindPreparedRecursiveCteCardinality(
            &prepared, profiles, previous_logical->logical_node_id,
            upper_bound, kGraphCompositionRowBound)) {
      return refuse(
          "QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
          "graph recursive cardinality bound is invalid");
    }
    if (!BindPreparedRecursiveCtePeakMemory(
            &prepared, planning.memory_budget_bytes)) {
      return refuse(
          "SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
          "graph recursive payload peak exceeds its admitted memory budget");
    }
    prepared_recursive_cte = prepared;
    recursive_term_capability_uuid = DerivedCanonicalUuid(
        identity_scope, "graph.recursive-term.capability");
    recursive_root_capability_uuid = DerivedCanonicalUuid(
        identity_scope, "graph.recursive-root.capability");

    LivePhysicalNodeProfile term_profile;
    term_profile.logical_node_id = recursive_term->node_id;
    term_profile.implementation_id =
        "cte.recursive-term.int64-increment.typed.v1";
    term_profile.capability_uuid = recursive_term_capability_uuid;
    term_profile.logical_node_kind = logical_term->node_kind;
    term_profile.physical_node_kind = exec::PhysicalNodeKind::kCte;
    term_profile.transformation_rule_id =
        "canonical.cte.recursive-term-int64-increment.v1";
    term_profile.memory_bytes_required = 1;
    profiles.push_back(std::move(term_profile));

    LivePhysicalNodeProfile root_profile;
    root_profile.logical_node_id = recursive_root->node_id;
    root_profile.implementation_id = prepared.profile.implementation_id;
    root_profile.capability_uuid = recursive_root_capability_uuid;
    root_profile.logical_node_kind = logical_root->node_kind;
    root_profile.physical_node_kind = exec::PhysicalNodeKind::kRecursiveCte;
    root_profile.transformation_rule_id = prepared.profile.transformation_id;
    root_profile.estimated_rows = prepared.maximum_result_row_count;
    root_profile.memory_bytes_required =
        prepared.planned_peak_memory_bytes;
    root_profile.minimum_input_count = 2;
    root_profile.maximum_input_count = 2;
    profiles.push_back(std::move(root_profile));
    previous_logical = logical_root;
  }
  if (!CompleteLiveRuntimeMemoryReceipts(&profiles)) {
    return refuse("QOW-DIAG-OPT-017-REFUSAL-V1",
                  "graph producer memory receipt is incomplete");
  }
  const auto physical = PlanAndPublishLivePhysicalDag(
      canonical_planning_request, profiles, "graph.selected-plan",
      "graph model source", "graph.local.v1");
  if (!physical.ok ||
      physical.physical_dag.nodes.size() != profiles.size() ||
      physical.physical_dag.root_physical_node_id != dag.root_node_id ||
      physical.physical_dag.nodes.front().implementation_id !=
          "physical_graph_adjacency_scan_v1" ||
      physical.physical_dag.nodes.front().executor_capability_uuid !=
          planned.selected_candidate.capability_uuid ||
      physical.physical_dag.nodes.front().selected_alternative_uuid !=
          physical_alternative_uuid) {
    return refuse(physical.diagnostic_id.empty()
                      ? "QOW-DIAG-OPTIMIZER-PHYSICAL-PUBLICATION-V1"
                      : physical.diagnostic_id,
                  physical.detail.empty()
                      ? "graph canonical physical DAG was not published"
                      : physical.detail);
  }
  result.optimizer_selected = true;
  result.physical_dag_published = true;
  result.optimizer_admission_stage_count =
      canonical_admission.admission.evidence.size();
  result.physical_node_count = physical.physical_dag.nodes.size();
  result.selected_plan_uuid = physical.physical_dag.selected_plan_uuid;

  std::vector<const api::RelationalOutputRecord*> outputs;
  for (const auto& output : dag.outputs) {
    if (output.relation_node_id == scan->node_id) outputs.push_back(&output);
  }
  std::ranges::sort(outputs, {}, &api::RelationalOutputRecord::ordinal);
  if (outputs.size() != scan->output_descriptor_ids.size()) {
    return refuse("SB_MODEL_TYPED_EXCHANGE_INVALID_V1",
                  "graph output descriptors are incomplete");
  }
  const auto descriptor_for = [&](const std::uint32_t descriptor_id) {
    return std::ranges::find_if(dag.descriptors, [&](const auto& descriptor) {
      return descriptor.descriptor_id == descriptor_id;
    });
  };
  struct GraphOutputBinding {
    std::string field_name;
    exec::ExecutorColumnDescriptor column;
  };
  const auto descriptor_field = [](const std::string& encoded,
                                   const std::string_view key) {
    std::optional<std::string> value;
    std::size_t offset = 0;
    while (offset <= encoded.size()) {
      const auto end = encoded.find(';', offset);
      const auto field = std::string_view(encoded).substr(
          offset, end == std::string::npos ? std::string::npos
                                            : end - offset);
      const auto equal = field.find('=');
      if (equal != std::string_view::npos && field.substr(0, equal) == key) {
        if (value.has_value()) return std::optional<std::string>{};
        value = std::string(field.substr(equal + 1));
      }
      if (end == std::string::npos) break;
      offset = end + 1;
    }
    return value;
  };
  const auto runtime_descriptor = [](const auto& source,
                                     const std::string& type_name) {
    api::EngineDescriptor descriptor;
    descriptor.descriptor_uuid.canonical = source.descriptor_uuid;
    descriptor.descriptor_kind = "scalar";
    descriptor.canonical_type_name = type_name;
    descriptor.encoded_descriptor =
        "type_uuid=" + source.type_uuid + ";nullability=" +
        (source.nullability == api::RelationalNullability::kNullable
             ? "nullable"
             : "non_null");
    if (source.collation_uuid.has_value()) {
      descriptor.encoded_descriptor +=
          ";collation_uuid=" + *source.collation_uuid;
    }
    if (source.timezone_profile_id.has_value()) {
      descriptor.encoded_descriptor +=
          ";timezone_profile_id=" + *source.timezone_profile_id;
    }
    if (source.width.has_value()) {
      descriptor.encoded_descriptor +=
          ";width=" + std::to_string(*source.width);
    }
    if (source.precision.has_value()) {
      descriptor.encoded_descriptor +=
          ";precision=" + std::to_string(*source.precision);
    }
    if (source.scale.has_value()) {
      descriptor.encoded_descriptor +=
          ";scale=" + std::to_string(*source.scale);
    }
    return descriptor;
  };
  std::vector<GraphOutputBinding> output_bindings;
  output_bindings.reserve(outputs.size());
  for (std::size_t ordinal = 0; ordinal < outputs.size(); ++ordinal) {
    const auto& output = *outputs[ordinal];
    const auto expression = expression_for(output.expression_id);
    const auto descriptor = descriptor_for(output.descriptor_id);
    if (!output.visible || output.ordinal != ordinal || output.output_id == 0 ||
        output.descriptor_id != scan->output_descriptor_ids[ordinal] ||
        expression == dag.expressions.end() ||
        expression->expression_kind !=
            api::RelationalExpressionKind::kIdentifier ||
        !expression->bound_name_uuid.has_value() ||
        expression->result_descriptor_id != output.descriptor_id ||
        descriptor == dag.descriptors.end()) {
      return refuse("SB_MODEL_TYPED_EXCHANGE_INVALID_V1",
                    "graph output expression binding is incomplete");
    }
    const auto persisted_column = std::ranges::find_if(
        persisted_relation.columns, [&](const auto& column) {
          return column.column_uuid.canonical == *expression->bound_name_uuid;
        });
    const auto persisted_type_uuid =
        persisted_column == persisted_relation.columns.end()
            ? std::optional<std::string>{}
            : descriptor_field(
                  persisted_column->value_descriptor.encoded_descriptor,
                  "type_uuid");
    const auto expected_u32 = [](const auto& value) {
      return value.has_value()
                 ? std::optional<std::string>(std::to_string(*value))
                 : std::optional<std::string>{};
    };
    if (persisted_column == persisted_relation.columns.end() ||
        persisted_column->canonical_name_key != output.output_name_utf8 ||
        persisted_column->value_descriptor.descriptor_uuid.canonical !=
            descriptor->descriptor_uuid ||
        persisted_column->value_descriptor.descriptor_kind !=
            "canonical_type_descriptor" ||
        persisted_column->value_descriptor.canonical_type_name.empty() ||
        persisted_column->value_descriptor.encoded_descriptor.empty() ||
        !persisted_type_uuid.has_value() ||
        *persisted_type_uuid != descriptor->type_uuid ||
        descriptor_field(
            persisted_column->value_descriptor.encoded_descriptor,
            "collation_uuid") != descriptor->collation_uuid ||
        descriptor_field(
            persisted_column->value_descriptor.encoded_descriptor,
            "timezone_profile_id") != descriptor->timezone_profile_id ||
        descriptor_field(
            persisted_column->value_descriptor.encoded_descriptor,
            "width") != expected_u32(descriptor->width) ||
        descriptor_field(
            persisted_column->value_descriptor.encoded_descriptor,
            "precision") != expected_u32(descriptor->precision) ||
        descriptor_field(
            persisted_column->value_descriptor.encoded_descriptor,
            "scale") != expected_u32(descriptor->scale) ||
        (descriptor->nullability != api::RelationalNullability::kNonNull &&
         descriptor->nullability != api::RelationalNullability::kNullable) ||
        persisted_column->nullable !=
            (descriptor->nullability ==
             api::RelationalNullability::kNullable) ||
        (descriptor->collation_uuid.has_value()
             ? persisted_column->collation_uuid !=
                   *descriptor->collation_uuid
             : !persisted_column->collation_uuid.empty()) ||
        !api::QowCanonicalDescriptorIdentityV1(
            persisted_column->value_descriptor)) {
      return refuse("SB_MODEL_TYPED_EXCHANGE_INVALID_V1",
                    "graph persisted output descriptor was substituted");
    }
    auto executor_descriptor = runtime_descriptor(
        *descriptor,
        persisted_column->value_descriptor.canonical_type_name);
    if (!api::QowCanonicalDescriptorIdentityV1(executor_descriptor)) {
      return refuse("SB_MODEL_TYPED_EXCHANGE_INVALID_V1",
                    "graph executor output descriptor was unresolved");
    }
    output_bindings.push_back(
        {persisted_column->canonical_name_key,
         {output.output_name_utf8, std::move(executor_descriptor),
          persisted_column->nullable, descriptor->descriptor_id}});
  }

  const auto bounded_memory = static_cast<std::size_t>(
      std::min<std::uint64_t>(planning.memory_budget_bytes,
                              std::numeric_limits<std::size_t>::max()));
  if (bounded_memory < output_bindings.size() * sizeof(api::EngineTypedValue)) {
    return refuse("SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                  "graph provider memory budget is too small");
  }
  const auto per_row_bytes = std::max<std::size_t>(
      1, output_bindings.size() * sizeof(api::EngineTypedValue));
  const auto graph_source_row_bound =
      prepared_graph_set.has_value()
          ? kGraphCompositionRowBound -
                materialized_graph_set_values->batch.rows.size()
          : kGraphCompositionRowBound;
  graph_request.maximum_output_rows =
      std::min<std::uint64_t>(graph_source_row_bound,
                              bounded_memory / per_row_bytes);
  graph_request.maximum_scanned_row_versions =
      std::max<std::uint64_t>(graph_request.maximum_output_rows, 4096);
  if (graph_set_values_memory_bytes >= planning.memory_budget_bytes) {
    return refuse("SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                  "graph composition leaves no provider memory budget");
  }
  const auto graph_route_memory_budget =
      planning.memory_budget_bytes - graph_set_values_memory_bytes;
  const auto graph_query_memory_budget = graph_route_memory_budget / 2;
  const auto graph_post_query_memory_budget =
      graph_route_memory_budget - graph_query_memory_budget;
  const auto graph_provider_batch_memory_budget =
      graph_post_query_memory_budget / 2;
  const auto graph_exchange_memory_budget =
      graph_post_query_memory_budget - graph_provider_batch_memory_budget;
  if (graph_query_memory_budget == 0 ||
      graph_provider_batch_memory_budget == 0 ||
      graph_exchange_memory_budget == 0) {
    return refuse("SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                  "graph route memory partitions are incomplete");
  }
  graph_request.maximum_decoded_bytes = graph_query_memory_budget;
  std::uint64_t maximum_cells = 0;
  if (graph_request.maximum_output_rows == 0 ||
      !CheckedMultiply(graph_request.maximum_output_rows,
                       static_cast<std::uint64_t>(output_bindings.size()),
                       &maximum_cells) ||
      maximum_cells == 0 ||
      maximum_cells > std::numeric_limits<std::size_t>::max()) {
    return refuse("SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                  "graph provider cell bound overflowed");
  }

  auto& proof = graph_request.physical_proof;
  proof.proof_supplied = true;
  proof.vertex_index_proof = true;
  proof.edge_index_proof = true;
  proof.adjacency_store_proof = true;
  proof.adjacency_page_proof = true;
  proof.frontier_batching_proof = true;
  proof.visited_cycle_policy_proof = true;
  proof.bidirectional_search_proof = true;
  proof.fusion_seed_proof = true;
  auto& provider_contract = proof.provider_contract;
  provider_contract.family = api::EngineNoSqlProviderFamily::kGraph;
  provider_contract.scope = api::EngineNoSqlProviderScope::kLocal;
  provider_contract.provider_id = provider_uuid;
  provider_contract.fallback_provider_id =
      "GRAPH_BOUNDED_ADJACENCY_SCAN_EXACT_V1";
  provider_contract.local_provider_available = true;
  provider_contract.exact_fallback_available = true;
  provider_contract.estimated_rows = graph_request.maximum_output_rows;
  provider_contract.descriptor_visibility.proof_present = true;
  provider_contract.descriptor_visibility.visible_to_snapshot = true;
  provider_contract.descriptor_visibility.descriptor_shape_compatible = true;
  provider_contract.descriptor_visibility.descriptor_generation =
      graph_request.provider_generation;
  provider_contract.descriptor_visibility.proof_id =
      DerivedCanonicalUuid(identity_scope, "graph.descriptor-proof");
  provider_contract.security_redaction.proof_present = true;
  provider_contract.security_redaction.redaction_policy_bound = true;
  provider_contract.security_redaction.security_snapshot_bound = true;
  provider_contract.security_redaction.redaction_profile =
      "graph.engine-row-security.v1";
  provider_contract.security_redaction.proof_id = security_receipt_uuid;
  provider_contract.index_generation.proof_present = true;
  provider_contract.index_generation.visible_to_snapshot = true;
  provider_contract.index_generation.covers_predicate = true;
  provider_contract.index_generation.required_generation =
      graph_request.provider_generation;
  provider_contract.index_generation.available_generation =
      graph_request.provider_generation;
  provider_contract.index_generation.index_uuid =
      DerivedCanonicalUuid(identity_scope, "graph.adjacency-index");
  provider_contract.index_generation.proof_id =
      DerivedCanonicalUuid(identity_scope, "graph.index-proof");
  provider_contract.delta_overlay.required = false;
  provider_contract.policy.proof_present = true;
  provider_contract.policy.allowed = true;
  provider_contract.policy.policy_snapshot_uuid = policy_snapshot_uuid;
  auto& provider_generation = provider_contract.provider_generation;
  provider_generation.required = true;
  provider_generation.proof_present = true;
  provider_generation.visible_to_snapshot = true;
  provider_generation.publish_state_bound = true;
  provider_generation.validation_state_bound = true;
  provider_generation.backup_restore_repair_metadata_bound = true;
  provider_generation.support_bundle_evidence_bound = true;
  provider_generation.required_generation = graph_request.provider_generation;
  provider_generation.available_generation = graph_request.provider_generation;
  provider_generation.descriptor_epoch = graph_request.provider_generation;
  provider_generation.security_epoch = planning.security_epoch;
  provider_generation.redaction_epoch = planning.policy_epoch;
  provider_generation.catalog_epoch = generation;
  provider_generation.generation_uuid =
      DerivedCanonicalUuid(identity_scope, "graph.provider-generation");
  provider_generation.provider_id = provider_uuid;
  provider_generation.database_uuid = input.context.database_uuid.canonical;
  provider_generation.collection_uuid = graph_request.graph_object_uuid;
  provider_generation.publish_state = "published";
  provider_generation.validation_state = "validated";
  provider_generation.backup_metadata_ref =
      "graph.provider.backup-metadata.v1";
  provider_generation.restore_metadata_ref =
      "graph.provider.restore-metadata.v1";
  provider_generation.repair_metadata_ref =
      "graph.provider.repair-metadata.v1";
  provider_generation.support_bundle_evidence_id =
      DerivedCanonicalUuid(identity_scope, "graph.support-evidence");
  provider_contract.mga_recheck.proof_present = true;
  provider_contract.mga_recheck.row_mga_recheck_required = true;
  provider_contract.mga_recheck.row_security_recheck_required = true;
  provider_contract.mga_recheck.authority_source =
      "engine_transaction_inventory";

  exec::ModelSourceInputDescriptorV1 source_input;
  source_input.family_id = "graph";
  source_input.operation_id = planning.operation_id;
  source_input.object_uuid = graph_request.graph_object_uuid;
  source_input.physical_node_id =
      physical.physical_dag.nodes.front().physical_node_id;
  source_input.selected_alternative_uuid =
      physical.physical_dag.nodes.front().selected_alternative_uuid;
  source_input.capability_uuid =
      physical.physical_dag.nodes.front().executor_capability_uuid;
  source_input.provider_uuid = provider_uuid;
  source_input.provider_generation = graph_request.provider_generation;
  source_input.result_handle_uuid = result_handle_uuid;
  source_input.causal_counter_id =
      physical.physical_dag.nodes.front().causal_counter_id;
  source_input.output_descriptor_ids = scan->output_descriptor_ids;
  source_input.mga_statement_context = mga;
  source_input.catalog_epoch_uuid = input.context.catalog_epoch_uuid.canonical;
  source_input.security_context_uuid =
      input.context.authorization_context.authority_uuid.canonical;
  source_input.policy_snapshot_uuid = policy_snapshot_uuid;
  source_input.resource_contract_uuid = resource_contract_uuid;
  source_input.catalog_generation = generation;
  source_input.descriptor_generation = graph_request.provider_generation;
  source_input.security_generation = planning.security_epoch;
  source_input.policy_generation = planning.policy_epoch;
  source_input.resource_generation = planning.resource_epoch;
  source_input.maximum_rows = graph_request.maximum_output_rows;
  source_input.maximum_cells = static_cast<std::size_t>(maximum_cells);
  source_input.maximum_memory_bytes = graph_exchange_memory_budget;

  exec::ModelFamilyExecutionRequestV1 execution_request;
  execution_request.input = source_input;
  execution_request.capability.capability_uuid = source_input.capability_uuid;
  execution_request.capability.family_id = "graph";
  execution_request.capability.provider_uuid = provider_uuid;
  execution_request.capability.provider_generation =
      source_input.provider_generation;
  execution_request.capability.available = true;
  execution_request.capability.exact = true;
  execution_request.capability.exact_collection_fallback_available = true;
  execution_request.capability.cancellation_supported = true;
  execution_request.capability.cleanup_supported = true;
  execution_request.capability.residual_recheck_supported = true;
  execution_request.capability.base_row_mga_recheck_supported = true;
  execution_request.capability.security_recheck_supported = true;
  execution_request.cancellation_requested =
      input.context.query_cancellation_requested
          ? input.context.query_cancellation_requested
          : std::function<bool()>([] { return false; });
  execution_request.cleanup_provider = [] {};
  execution_request.exact_fallback_selected = true;
  execution_request.security_admitted = planning.security_admitted;
  execution_request.current_catalog_generation = generation;
  execution_request.current_descriptor_generation =
      graph_request.provider_generation;
  execution_request.current_security_generation = planning.security_epoch;
  execution_request.current_policy_generation = planning.policy_epoch;
  execution_request.current_resource_generation = planning.resource_epoch;
  execution_request.current_provider_generation =
      graph_request.provider_generation;
  execution_request.current_mga_statement_context = mga;
  execution_request.execute_provider =
      [graph_request, source_input, output_bindings, property_uuid,
       security_receipt_uuid, identity_scope,
       graph_provider_batch_memory_budget,
       persisted_descriptor_uuid =
           persisted_relation.descriptor_uuid.canonical,
       persisted_descriptor_generation =
           persisted_relation.descriptor_generation](
          const exec::ModelSourceInputDescriptorV1& selected_input) mutable {
        exec::ModelProviderExecutionResultV1 provider;
        if (selected_input.maximum_memory_bytes == 0) {
          provider.diagnostic_id = "SB_MODEL_RESOURCE_MEMORY_REFUSED_V1";
          provider.detail = "graph provider callback memory allowance is absent";
          return provider;
        }
        const auto poll_provider_cancellation = [&]() {
          if (!graph_request.context.query_cancellation_requested) return false;
          try {
            if (!graph_request.context.query_cancellation_requested()) {
              return false;
            }
            provider.diagnostic_id = "SB_MODEL_EXECUTION_CANCELLED_V1";
            provider.detail = "graph provider materialization was cancelled";
          } catch (...) {
            provider.diagnostic_id = "SB_MODEL_COORDINATOR_LEG_FAILED_V1";
            provider.detail =
                "graph provider cancellation coordinator probe failed";
          }
          provider.provider_batch = {};
          return true;
        };
        const auto current_authorization = api::EvaluateMaterializedAuthorization(
            graph_request.context,
            graph_request.context.authorization_context, "SELECT",
            source_input.object_uuid);
        if (!current_authorization.authorized ||
            current_authorization.denied ||
            current_authorization.policy_recheck_required ||
            !current_authorization.diagnostics.empty()) {
          provider.diagnostic_id = "SB_MODEL_SECURITY_ADMISSION_REFUSED_V1";
          provider.detail = "graph SELECT authorization was refused";
          return provider;
        }
        const auto current_relation = api::LoadMgaRelationStorageDescriptor(
            graph_request.context, source_input.object_uuid);
        if (!current_relation.ok ||
            current_relation.descriptor.relation_uuid.canonical !=
                source_input.object_uuid ||
            current_relation.descriptor.database_uuid.canonical !=
                graph_request.context.database_uuid.canonical ||
            current_relation.descriptor.schema_uuid.canonical !=
                graph_request.bound_object_identity.resolved_schema_uuid
                    .canonical ||
            current_relation.descriptor.relation_kind != "table" ||
            current_relation.descriptor.storage_profile !=
                "local_mga_rowstore_v1" ||
            current_relation.descriptor.descriptor_uuid.canonical !=
                persisted_descriptor_uuid ||
            current_relation.descriptor.descriptor_generation !=
                persisted_descriptor_generation ||
            current_relation.descriptor.descriptor_generation !=
                source_input.provider_generation ||
            !api::EngineGraphDescriptorCohortExact(
                current_relation.descriptor)) {
          provider.diagnostic_id = "SB_MODEL_TYPED_EXCHANGE_INVALID_V1";
          provider.detail =
              "graph relation descriptor changed before provider execution";
          return provider;
        }
        const auto found = api::EngineGraphQuery(graph_request);
        provider.data_access_observed = true;
        provider.rows_examined = found.dml_summary.visible_rows_scanned;
        if (!found.ok) {
          provider.diagnostic_id =
              found.diagnostics.empty()
                  ? "SB_MODEL_GRAPH_EXACT_FALLBACK_UNAVAILABLE_V1"
                  : found.diagnostics.front().code;
          provider.detail = found.diagnostics.empty()
                                ? "persistent graph provider query failed"
                                : found.diagnostics.front().detail;
          return provider;
        }
        const auto field_value = [](const auto& row,
                                    const std::string_view field_name)
            -> const api::EngineTypedValue* {
          const auto found_field = std::ranges::find_if(
              row.fields, [&](const auto& field) {
                return field.first == field_name;
              });
          return found_field == row.fields.end() ? nullptr
                                                  : &found_field->second;
        };
        std::uint64_t provider_batch_bytes =
            sizeof(exec::ModelProviderBatchV1) + 4096;
        bool provider_batch_budget_exhausted =
            provider_batch_bytes > graph_provider_batch_memory_budget;
        const auto account_provider_batch = [&](const std::uint64_t bytes) {
          if (bytes > std::numeric_limits<std::uint64_t>::max() -
                          provider_batch_bytes ||
              provider_batch_bytes + bytes >
                  graph_provider_batch_memory_budget) {
            provider_batch_budget_exhausted = true;
            return false;
          }
          provider_batch_bytes += bytes;
          return true;
        };
        const auto account_provider_string = [&](const std::string_view value) {
          const auto bytes = static_cast<std::uint64_t>(value.size());
          return bytes <= std::numeric_limits<std::uint64_t>::max() / 2 &&
                 account_provider_batch(bytes * 2);
        };
        for (const auto& output : output_bindings) {
          if (poll_provider_cancellation()) return provider;
          if (!account_provider_batch(512) ||
              !account_provider_string(output.field_name) ||
              !account_provider_string(output.column.stable_name) ||
              !account_provider_string(
                  output.column.descriptor.descriptor_uuid.canonical) ||
              !account_provider_string(
                  output.column.descriptor.descriptor_kind) ||
              !account_provider_string(
                  output.column.descriptor.canonical_type_name) ||
              !account_provider_string(
                  output.column.descriptor.encoded_descriptor)) {
            provider.diagnostic_id = "SB_MODEL_RESOURCE_MEMORY_REFUSED_V1";
            provider.detail =
                "graph provider batch descriptor budget was exceeded";
            return provider;
          }
        }
        for (const auto& graph_row : found.result_shape.rows) {
          if (poll_provider_cancellation()) return provider;
          const auto* vertex_uuid = field_value(graph_row, "vertex_uuid");
          const auto* edge_uuid = field_value(graph_row, "edge_uuid");
          const auto* path_uuid = field_value(graph_row, "path_uuid");
          const auto* labels = field_value(graph_row, "vertex_labels");
          const auto* vertex_properties =
              field_value(graph_row, "vertex_properties");
          const auto* edge_properties =
              field_value(graph_row, "edge_properties");
          const auto* direction = field_value(graph_row, "direction");
          const auto* depth = field_value(graph_row, "depth");
          const auto* cycle = field_value(graph_row, "cycle_policy");
          std::uint64_t graph_depth = 0;
          const auto parsed_depth =
              depth == nullptr
                  ? std::from_chars_result{}
                  : std::from_chars(depth->encoded_value.data(),
                                    depth->encoded_value.data() +
                                        depth->encoded_value.size(),
                                    graph_depth);
          const std::string_view expected_direction =
              graph_request.direction ==
                      api::EngineGraphTraversalDirection::kIncoming
                  ? "incoming"
                  : graph_request.direction ==
                            api::EngineGraphTraversalDirection::kBoth
                        ? "both"
                        : "outgoing";
          const auto exact_field = [&](const std::string_view name) {
            return std::ranges::count_if(
                       graph_row.fields, [&](const auto& field) {
                         return field.first == name;
                       }) == 1;
          };
          if (!exact_field("vertex_uuid") || !exact_field("edge_uuid") ||
              !exact_field("path_uuid") || !exact_field("vertex_labels") ||
              !exact_field("vertex_properties") ||
              !exact_field("edge_properties") ||
              !exact_field("direction") || !exact_field("depth") ||
              !exact_field("cycle_policy") || vertex_uuid == nullptr ||
              edge_uuid == nullptr || path_uuid == nullptr ||
              labels == nullptr || vertex_properties == nullptr ||
              edge_properties == nullptr || direction == nullptr ||
              depth == nullptr || cycle == nullptr ||
              depth->encoded_value.empty() ||
              parsed_depth.ec != std::errc{} ||
              parsed_depth.ptr != depth->encoded_value.data() +
                                      depth->encoded_value.size() ||
              depth->encoded_value != std::to_string(graph_depth) ||
              graph_depth < graph_request.min_depth ||
              graph_depth > graph_request.max_depth ||
              vertex_uuid->state != api::EngineValueState::value ||
              edge_uuid->state != api::EngineValueState::value ||
              path_uuid->state != api::EngineValueState::value ||
              labels->state != api::EngineValueState::value ||
              vertex_properties->state != api::EngineValueState::value ||
              edge_properties->state != api::EngineValueState::value ||
              direction->state != api::EngineValueState::value ||
              depth->state != api::EngineValueState::value ||
              cycle->state != api::EngineValueState::value ||
              direction->encoded_value != expected_direction ||
              cycle->encoded_value != "visited_set" ||
              !CanonicalUuidText(vertex_uuid->encoded_value) ||
              !CanonicalUuidText(path_uuid->encoded_value) ||
              ((graph_depth == 0) != edge_uuid->encoded_value.empty()) ||
              (graph_depth > 0 &&
               !CanonicalUuidText(edge_uuid->encoded_value)) ||
              !account_provider_batch(4096 +
                                      output_bindings.size() * 256) ||
              !account_provider_string(vertex_uuid->encoded_value) ||
              !account_provider_string(edge_uuid->encoded_value) ||
              !account_provider_string(path_uuid->encoded_value) ||
              !account_provider_batch(2 * 36)) {
            provider.diagnostic_id =
                provider_batch_budget_exhausted
                    ? "SB_MODEL_RESOURCE_MEMORY_REFUSED_V1"
                    : "SB_MODEL_TYPED_EXCHANGE_INVALID_V1";
            provider.detail =
                "graph provider row identity or batch budget is invalid";
            return provider;
          }
          for (const auto& output : output_bindings) {
            if (poll_provider_cancellation()) return provider;
            const auto* value = field_value(graph_row, output.field_name);
            const bool depth_zero_edge =
                output.field_name == "edge_uuid" && graph_depth == 0;
            if ((depth_zero_edge && !output.column.nullable) ||
                (value == nullptr && !output.column.nullable) ||
                (value != nullptr &&
                 (!account_provider_string(value->encoded_value) ||
                  !account_provider_batch(value->binary_value.size())))) {
              provider.diagnostic_id =
                  provider_batch_budget_exhausted
                      ? "SB_MODEL_RESOURCE_MEMORY_REFUSED_V1"
                      : "SB_MODEL_TYPED_EXCHANGE_INVALID_V1";
              provider.detail =
                  "graph provider output or batch budget is invalid";
              return provider;
            }
          }
        }
        auto& provider_batch = provider.provider_batch;
        auto& batch = provider_batch.batch;
        for (const auto& output : output_bindings) {
          if (poll_provider_cancellation()) return provider;
          batch.columns.push_back(output.column);
        }
        for (const auto& graph_row : found.result_shape.rows) {
          if (poll_provider_cancellation()) return provider;
          const auto* vertex_uuid = field_value(graph_row, "vertex_uuid");
          const auto* edge_uuid = field_value(graph_row, "edge_uuid");
          const auto* path_uuid = field_value(graph_row, "path_uuid");
          const auto* depth = field_value(graph_row, "depth");
          std::uint64_t graph_depth = 0;
          const auto parsed_depth =
              depth == nullptr
                  ? std::from_chars_result{}
                  : std::from_chars(depth->encoded_value.data(),
                                    depth->encoded_value.data() +
                                        depth->encoded_value.size(),
                                    graph_depth);
          if (vertex_uuid == nullptr || edge_uuid == nullptr ||
              path_uuid == nullptr || depth == nullptr ||
              depth->encoded_value.empty() ||
              parsed_depth.ec != std::errc{} ||
              parsed_depth.ptr != depth->encoded_value.data() +
                                      depth->encoded_value.size() ||
              vertex_uuid->encoded_value.empty() ||
              path_uuid->encoded_value.empty() ||
              (graph_depth == 0) != edge_uuid->encoded_value.empty()) {
            provider.diagnostic_id = "SB_MODEL_TYPED_EXCHANGE_INVALID_V1";
            provider.detail = "graph provider row identity is incomplete";
            return provider;
          }
          exec::DescriptorTuple tuple;
          for (const auto& output : output_bindings) {
            if (poll_provider_cancellation()) return provider;
            auto* value = field_value(graph_row, output.field_name);
            api::EngineTypedValue typed;
            typed.descriptor = output.column.descriptor;
            const bool depth_zero_edge =
                output.field_name == "edge_uuid" && graph_depth == 0;
            if (depth_zero_edge) {
              if (!output.column.nullable) {
                provider.diagnostic_id = "SB_MODEL_TYPED_EXCHANGE_INVALID_V1";
                provider.detail =
                    "depth-zero graph edge output is not nullable";
                return provider;
              }
              typed.setState(api::EngineValueState::sql_null);
            } else if (value == nullptr) {
              if (!output.column.nullable) {
                provider.diagnostic_id = "SB_MODEL_TYPED_EXCHANGE_INVALID_V1";
                provider.detail =
                    "graph provider omitted a non-null bound output";
                return provider;
              }
              typed.setState(api::EngineValueState::sql_null);
            } else {
              typed.encoded_value = value->encoded_value;
              typed.binary_value = value->binary_value;
              typed.setState(api::EngineValueState::value);
            }
            tuple.values.push_back(std::move(typed));
          }
          batch.rows.push_back(std::move(tuple));
          provider_batch.ordered_row_identities.push_back(
              {{},
               DerivedCanonicalUuid(identity_scope,
                                    "graph.row." + path_uuid->encoded_value),
               vertex_uuid->encoded_value, edge_uuid->encoded_value,
               path_uuid->encoded_value, graph_depth});
        }
        provider_batch.provider_uuid = source_input.provider_uuid;
        provider_batch.provider_generation = source_input.provider_generation;
        provider_batch.result_handle_uuid = source_input.result_handle_uuid;
        provider_batch.causal_counter_id = source_input.causal_counter_id;
        provider_batch.output_descriptor_ids =
            source_input.output_descriptor_ids;
        provider_batch.mga_statement_context =
            source_input.mga_statement_context;
        provider_batch.security_receipt_uuid = security_receipt_uuid;
        provider_batch.properties.property_uuid = property_uuid;
        provider_batch.properties.ordering_id = "fixture_order";
        provider_batch.properties.partitioning_id = "single_local_partition";
        provider_batch.properties.uniqueness_id = "path_uuid";
        provider_batch.properties.exact = true;
        provider_batch.properties.residual_recheck_complete = true;
        provider_batch.properties.base_row_mga_recheck_complete = true;
        provider_batch.properties.security_recheck_complete = true;
        provider_batch.residual_recheck_complete = true;
        provider_batch.base_row_mga_recheck_complete = true;
        provider_batch.security_recheck_complete = true;
        provider.ok = true;
        return provider;
      };

  CaptureRcp079ModelLegV1(
      leg_capture, scan->node_id, "graph",
      "physical_graph_adjacency_scan_v1",
      "canonical.graph.adjacency-scan.v1", "graph.local.v1",
      persisted_relation.descriptor_uuid.canonical,
      persisted_relation.descriptor_generation,
      plan::CanonicalLogicalRelationalNodeKind::kRelationSource,
      exec::PhysicalNodeKind::kScan, execution_request);
  if (leg_capture != nullptr) return result;
  exec::CanonicalPhysicalExecutorRegistration registration;
  registration.node_kind = exec::PhysicalNodeKind::kScan;
  registration.implementation_id = "physical_graph_adjacency_scan_v1";
  registration.executor_capability_uuid =
      physical.physical_dag.nodes.front().executor_capability_uuid;
  registration.executor_capability_abi_version = 1;
  registration.engine_owned = true;
  registration.accepts_optimizer_publication_v2 = true;
  registration.honors_dispatcher_memory_limit_v1 = true;
  registration.execute =
      [execution_request,
       persisted_descriptor_uuid =
           persisted_relation.descriptor_uuid.canonical](
          const exec::TypedPhysicalNodeDag& selected_dag,
          const exec::PhysicalNodeRecord& selected_node,
          const std::vector<exec::CanonicalPhysicalDispatchInput>& inputs) {
        exec::CanonicalPhysicalDispatchStepResult step;
        step.selected_plan_uuid = selected_dag.selected_plan_uuid;
        step.executed_physical_node_id = selected_node.physical_node_id;
        step.causal_counter_id = selected_node.causal_counter_id;
        step.output_descriptor_ids = selected_node.output_descriptor_ids;
        step.mga_statement_context = selected_dag.mga_statement_context;
        step.authority.engine_mga_snapshot_bound = true;
        step.data_access_observation_known = true;
        if (!inputs.empty() || selected_dag.abi_version != 2 ||
            selected_node.implementation_id !=
                "physical_graph_adjacency_scan_v1" ||
            selected_node.selected_alternative_uuid !=
                execution_request.input.selected_alternative_uuid ||
            selected_node.executor_capability_uuid !=
                execution_request.input.capability_uuid ||
            selected_node.physical_node_id !=
                execution_request.input.physical_node_id ||
            selected_node.causal_counter_id !=
                execution_request.input.causal_counter_id ||
            selected_node.output_descriptor_ids !=
                execution_request.input.output_descriptor_ids ||
            !exec::PhysicalMgaStatementContextEqual(
                selected_dag.mga_statement_context,
                execution_request.input.mga_statement_context)) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "SB_MODEL_TYPED_EXCHANGE_INVALID_V1";
          step.diagnostic.detail =
              "selected graph physical node identity was substituted";
          return step;
        }
        const auto callback_memory_limit = std::min(
            selected_node.memory_bytes_required,
            selected_node.dispatcher_callback_memory_limit_bytes);
        if (callback_memory_limit == 0 ||
            callback_memory_limit > selected_dag.memory_budget_bytes) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code = "SBLR.PLAN_TREE.RESOURCE_LIMIT";
          step.diagnostic.detail =
              "graph callback memory allowance is absent or invalid";
          return step;
        }
        auto bounded_execution_request = execution_request;
        bounded_execution_request.input.maximum_memory_bytes = std::min(
            bounded_execution_request.input.maximum_memory_bytes,
            callback_memory_limit);
        const auto executed =
            exec::ExecuteModelFamilySourceV1(bounded_execution_request);
        step.data_access_observed = executed.data_access_observed;
        if (!executed.accepted || !executed.root_published ||
            !executed.output.exact_exchange_validated ||
            executed.output.family_id != execution_request.input.family_id ||
            executed.output.operation_id !=
                execution_request.input.operation_id ||
            executed.output.object_uuid != execution_request.input.object_uuid ||
            executed.output.physical_node_id !=
                selected_node.physical_node_id ||
            executed.output.selected_alternative_uuid !=
                selected_node.selected_alternative_uuid ||
            executed.output.capability_uuid !=
                selected_node.executor_capability_uuid ||
            executed.output.provider_uuid !=
                execution_request.input.provider_uuid ||
            executed.output.provider_generation !=
                execution_request.input.provider_generation ||
            executed.output.result_handle_uuid !=
                execution_request.input.result_handle_uuid ||
            executed.output.causal_counter_id !=
                selected_node.causal_counter_id ||
            executed.output.output_descriptor_ids !=
                selected_node.output_descriptor_ids) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              executed.diagnostic_id.empty()
                  ? "SB_MODEL_COORDINATOR_LEG_FAILED_V1"
                  : executed.diagnostic_id;
          step.diagnostic.detail =
              executed.detail.empty()
                  ? "graph source execution did not complete"
                  : executed.detail;
          return step;
        }
        step.result_handle_id = selected_node.physical_node_id;
        step.output_row_count = executed.output.batch.rows.size();
        step.rows_examined = executed.rows_examined;
        step.current_relation_descriptor_uuid = persisted_descriptor_uuid;
        step.current_relation_descriptor_generation =
            execution_request.input.descriptor_generation;
        step.materialized_output_batch = std::move(executed.output.batch);
        return step;
      };
  api::CanonicalOptimizerSelectedExecutionRequest selected;
  selected.selected_physical_dag = physical.physical_dag;
  selected.pre_access_statistics_snapshot_uuid =
      physical.physical_dag.statistics_snapshot_uuid;
  selected.mga_authority = BuildCanonicalExecutionMgaAuthority(
      input.context, physical.physical_dag);
  std::size_t maximum_composition_columns = outputs.size();
  for (const auto& node : dag.nodes) {
    maximum_composition_columns = std::max(
        maximum_composition_columns, node.output_descriptor_ids.size());
  }
  const auto execution_row_bound =
      prepared_graph_set.has_value()
          ? static_cast<std::uint64_t>(kGraphCompositionRowBound)
          : source_input.maximum_rows;
  std::uint64_t maximum_composition_cells = 0;
  if (!CheckedMultiply(execution_row_bound,
                       maximum_composition_columns,
                       &maximum_composition_cells) ||
      maximum_composition_cells >
          std::numeric_limits<std::size_t>::max()) {
    return refuse("SBLR.PLAN_TREE.RESOURCE_LIMIT",
                  "graph composition cell bound overflowed");
  }
  selected.runtime_limits.maximum_rows_per_batch = execution_row_bound;
  selected.runtime_limits.maximum_columns_per_batch =
      maximum_composition_columns;
  selected.runtime_limits.maximum_cells_per_batch =
      static_cast<std::size_t>(maximum_composition_cells);
  selected.runtime_limits.maximum_total_materialized_rows =
      execution_row_bound;
  selected.runtime_limits.maximum_total_materialized_cells =
      static_cast<std::size_t>(maximum_composition_cells);
  selected.cancellation_requested =
      input.context.query_cancellation_requested
          ? input.context.query_cancellation_requested
          : std::function<bool()>([] { return false; });
  selected.available_executors.push_back(std::move(registration));
  if (prepared_graph_set.has_value()) {
    std::unordered_map<std::uint64_t, exec::DescriptorBatch> values_batches;
    values_batches.emplace(
        set_values->node_id,
        std::move(materialized_graph_set_values->batch));
    selected.available_executors.push_back(
        MakeLiveValuesRegistration(
            std::move(values_batches), set_values_capability_uuid,
            "QOW-DIAG-RELATIONAL-LIVE-SET-VALUES-V1",
            "graph UNION ALL", true));
    std::unordered_map<std::uint64_t, PreparedLiveSetNode>
        prepared_set_nodes;
    prepared_set_nodes.emplace(set_root->node_id, *prepared_graph_set);
    selected.available_executors.push_back(
        MakeLiveSetOperationRegistration(
            MakeLiveSetRegistrationProfilesForComposition(prepared_set_nodes),
            graph_set_profile.implementation_id,
            set_root_capability_uuid, input.context));
  }
  if (prepared_filter.has_value()) {
    selected.available_executors.push_back(
        MakeLiveHeapFilterRegistration(
            prepared_filter->predicate_expression_id,
            prepared_filter->predicate_row_binding, {},
            canonical_planning_request.expression_services,
            filter_capability_uuid, source_input.maximum_rows,
            {}, api::EngineCanonicalExpressionConsumer::filter,
            api::EnginePredicateConsumer::filter, &dag, &input.context,
            &selected.mga_authority));
  }
  if (prepared_project.has_value()) {
    selected.available_executors.push_back(
        MakeLiveProjectRegistration(
            MakeLiveProjectRegistrationProfileForComposition(*prepared_project),
            "project.typed.expression-row.v1",
            project_capability_uuid, source_input.maximum_rows, {},
            canonical_planning_request.expression_services,
            {}, true, &dag, &input.context,
            &selected.mga_authority));
  } else if (!direct_projected_columns.empty()) {
    selected.available_executors.push_back(
        MakeLiveHeapProjectRegistration(
            direct_projected_columns, project_capability_uuid,
            source_input.maximum_rows, {}, &input.context,
            &selected.mga_authority));
  }
  if (prepared_sort.has_value()) {
    std::uint64_t comparison_bound = 0;
    if (!CheckedMultiply(source_input.maximum_rows,
                         source_input.maximum_rows,
                         &comparison_bound) ||
        comparison_bound > std::numeric_limits<std::size_t>::max()) {
      return refuse("SBLR.PLAN_TREE.RESOURCE_LIMIT",
                    "graph SORT comparison bound overflowed");
    }
    const auto tie_uuid = DerivedCanonicalUuid(
        identity_scope + ":" + prepared_sort->ordering_property_uuid,
        "graph.sort.deterministic-tie");
    if (prepared_sort->expression_ordering) {
      selected.available_executors.push_back(
          MakeLiveExpressionSortRegistration(
              std::move(*prepared_sort),
              tie_uuid, sort_capability_uuid, source_input.maximum_rows,
              std::max<std::size_t>(
                  1, static_cast<std::size_t>(comparison_bound)),
              dag, canonical_planning_request.expression_services,
              {}, &dag, &input.context, &selected.mga_authority));
    } else {
      selected.available_executors.push_back(
          MakeLiveSortRegistration(
              prepared_sort->order_terms, tie_uuid, sort_capability_uuid,
              source_input.maximum_rows,
              std::max<std::size_t>(
                  1, static_cast<std::size_t>(comparison_bound)),
              {}, &input.context, &selected.mga_authority));
    }
  }
  if (prepared_row_number.has_value()) {
    selected.available_executors.push_back(
        MakeLiveRowNumberRegistration(
            *prepared_row_number, window_order_evidence_uuid,
            window_capability_uuid, source_input.maximum_rows,
            {}, &input.context, &selected.mga_authority));
  }
  if (prepared_nonrecursive_cte) {
    selected.available_executors.push_back(
        MakeLiveNonrecursiveCteRegistration(
            nonrecursive_cte_implementation_id, cte_capability_uuid,
            source_input.maximum_rows, {}, &input.context,
            &selected.mga_authority));
  }
  if (prepared_count_star.has_value()) {
    selected.available_executors.push_back(
        MakeLiveCountStarRegistration(
            prepared_count_star->result_column,
            aggregate_capability_uuid, source_input.maximum_rows,
            {}, &input.context, &selected.mga_authority));
  }
  if (prepared_recursive_cte.has_value()) {
    selected.available_executors.push_back(
        MakeLiveRecursiveCteTermRegistration(
            prepared_recursive_cte->term,
            recursive_term_capability_uuid, input.context));
    selected.available_executors.push_back(
        MakeLiveRecursiveCteRegistration(
            *prepared_recursive_cte, recursive_term_capability_uuid,
            recursive_root_capability_uuid, input.context));
  }
  if (prepared_limit.has_value()) {
    selected.available_executors.push_back(
        MakeLiveLimitRegistration(
            limit_implementation_id, limit_capability_uuid, limit_count,
            limit_offset, fetch_first_rows_only, source_input.maximum_rows,
            {}, &input.context, &selected.mga_authority));
  }
  selected.engine_execution_authorized = true;
  selected.result_publication_request.statement_uuid =
      input.context.statement_uuid.canonical;
  selected.result_publication_request.invocation_mode =
      exec::CanonicalResultInvocationMode::kDirect;
  selected.result_publication_request.execution_attempt_uuid =
      DerivedCanonicalUuid(identity_scope + ":" +
                               input.context.current_monotonic_ns,
                           "graph.execution-attempt");
  selected.result_publication_request.result_kind =
      exec::CanonicalResultKind::kRows;
  selected.result_publication_request.transaction_effect_evidence_uuid =
      DerivedCanonicalUuid(
          identity_scope + ":" +
              std::to_string(input.context.local_transaction_id) + ":" +
              std::to_string(
                  input.context.snapshot_visible_through_local_transaction_id),
          "graph.transaction-effect-unchanged");
  selected.result_publication_request.maximum_row_count =
      execution_row_bound;
  selected.result_publication_request.column_bindings =
      composition_state.result_bindings;
  const auto execution = ExecuteSelectedCanonicalObjectFreeDag(
      input.context, selected, physical.ordinary_runtime_memory_receipts);
  if (!execution.accepted || !execution.exact_selected_nodes_executed ||
      !execution.causal_counters_attached ||
      !execution.canonical_result_published ||
      !execution.data_access_observed || !execution.runtime_actuals.accepted ||
      execution.dispatch.executed_steps.size() !=
          physical.physical_dag.nodes.size() ||
      execution.dispatch.executed_root_physical_node_id !=
          physical.physical_dag.root_physical_node_id ||
      !execution.issues.empty()) {
    return refuse(execution.issues.empty()
                      ? "SB_MODEL_COORDINATOR_LEG_FAILED_V1"
                      : execution.issues.front().diagnostic_id,
                  execution.issues.empty()
                      ? "graph selected physical execution did not complete"
                      : execution.issues.front().field_id);
  }
  const auto column_ordinal = [&](const std::string_view name) {
    const auto column = std::ranges::find_if(
        execution.result_publication.row_stream.columns,
        [&](const auto& candidate) { return candidate.stable_name == name; });
    return column == execution.result_publication.row_stream.columns.end()
               ? std::optional<std::size_t>{}
               : std::optional<std::size_t>(static_cast<std::size_t>(
                     std::distance(
                         execution.result_publication.row_stream.columns.begin(),
                         column)));
  };
  const auto vertex_ordinal = column_ordinal("vertex_uuid");
  const auto edge_ordinal = column_ordinal("edge_uuid");
  const auto path_ordinal = column_ordinal("path_uuid");
  const auto labels_ordinal = column_ordinal("vertex_labels");
  const auto vertex_properties_ordinal =
      column_ordinal("vertex_properties");
  const auto edge_properties_ordinal = column_ordinal("edge_properties");
  const auto direction_ordinal = column_ordinal("direction");
  const auto depth_ordinal = column_ordinal("depth");
  const auto cycle_ordinal = column_ordinal("cycle_policy");
  const bool complete_typed_graph_projection =
      vertex_ordinal && edge_ordinal && path_ordinal && labels_ordinal &&
      vertex_properties_ordinal && edge_properties_ordinal &&
      direction_ordinal && depth_ordinal && cycle_ordinal;
  // All fallible descriptor/value validation completed in the provider and
  // typed exchange before canonical publication. Post-publication handling is
  // receipt-only: derive evidence without introducing a refusal boundary.
  bool depth_zero_edge_observed = false;
  if (complete_typed_graph_projection && depth_ordinal.has_value()) {
    depth_zero_edge_observed = std::ranges::any_of(
        execution.result_publication.row_stream.rows, [&](const auto& row) {
          return *depth_ordinal < row.values.size() &&
                 row.values[*depth_ordinal].encoded_value == "0";
        });
  }
  result.physical_dag_executed = true;
  result.runtime_actuals_attached = true;
  result.canonical_result_published = true;
  result.canonical_result_column_count =
      execution.result_publication.envelope.column_descriptors.size();
  result.canonical_result_row_count =
      execution.result_publication.row_stream.rows.size();
  result.canonical_result_bytes =
      execution.result_publication.canonical_envelope_bytes;
  result.api_result = SuccessfulApiResult(canonical_planning_request, execution);
  result.api_result.evidence.push_back(
      {"canonical.model_route",
       "SBSQL_GRAPH_SOURCE_TO_SBLR_MODEL_SOURCE_TO_GRAPH_ADJACENCY_SCAN_TO_TYPED_BATCH_V1"});
  result.api_result.evidence.push_back(
      {"canonical.model_search_family", "graph.local.v1"});
  result.api_result.evidence.push_back(
      {"canonical.graph_operation", planning.operation_id});
  result.api_result.evidence.push_back(
      {"canonical.graph_source_authority", "persistent_mga_relation_v1"});
  result.api_result.evidence.push_back(
      {"canonical.graph_cycle_policy", "visited_set"});
  result.api_result.evidence.push_back(
      {"canonical.graph_maximum_depth",
       std::to_string(graph_request.max_depth)});
  result.api_result.evidence.push_back(
      {"canonical.graph_provider_generation",
       std::to_string(source_input.provider_generation)});
  if (prepared_nonrecursive_cte) {
    result.api_result.evidence.push_back(
        {"canonical.graph_cte_implementation",
         nonrecursive_cte_implementation_id});
    result.api_result.evidence.push_back(
        {"canonical.graph_cte_auxiliary_memory",
         nonrecursive_cte_implementation_id ==
                 "cte.bound.materialize.typed.v1"
             ? "runtime_input_batch"
             : "none"});
  }
  if (prepared_count_star.has_value()) {
    result.api_result.evidence.push_back(
        {"canonical.graph_aggregate_implementation",
         "aggregate.count-star.v1"});
    result.api_result.evidence.push_back(
        {"canonical.graph_aggregate_root", "selected-dag-root.v1"});
  }
  if (prepared_recursive_cte.has_value()) {
    result.api_result.evidence.push_back(
        {"canonical.graph_recursive_cte_implementation",
         prepared_recursive_cte->profile.implementation_id});
    result.api_result.evidence.push_back(
         {"canonical.graph_recursive_cte_bound",
         std::to_string(prepared_recursive_cte->term.upper_bound)});
    result.api_result.evidence.push_back(
        {"canonical.graph_recursive_cte_work_bound",
         std::to_string(prepared_recursive_cte->rows_examined)});
  }
  if (prepared_graph_set.has_value()) {
    result.api_result.evidence.push_back(
        {"canonical.graph_set_implementation",
         graph_set_profile.implementation_id});
    result.api_result.evidence.push_back(
        {"canonical.graph_set_semantics",
         "union-all.ordinal.left-then-right.bag.v1"});
    result.api_result.evidence.push_back(
        {"canonical.graph_set_output_bound",
         std::to_string(kGraphCompositionRowBound)});
  }
  if (prepared_project.has_value() || !direct_projected_columns.empty()) {
    result.api_result.evidence.push_back(
        {"canonical.graph_project_implementation",
         prepared_project.has_value() ? "project.typed.expression-row.v1"
                                      : "project.descriptor-direct.v1"});
  }
  if (prepared_filter.has_value()) {
    result.api_result.evidence.push_back(
        {"canonical.graph_filter_implementation", "filter.3vl.row.v1"});
  }
  if (prepared_sort.has_value()) {
    result.api_result.evidence.push_back(
        {"canonical.graph_sort_implementation",
         prepared_sort->expression_ordering
             ? "sort.typed.expression-row.v1"
             : "sort.typed.terms.v1"});
  }
  if (prepared_row_number.has_value()) {
    result.api_result.evidence.push_back(
        {"canonical.graph_window_implementation", "window.row-number.v1"});
    result.api_result.evidence.push_back(
        {"canonical.graph_window_root", "selected-dag-root.v1"});
  }
  if (prepared_limit.has_value()) {
    result.api_result.evidence.push_back(
        {"canonical.graph_limit_implementation", limit_implementation_id});
  }
  result.api_result.evidence.push_back(
      {"canonical.graph_typed_row_contract",
       complete_typed_graph_projection
           ? "uuid-identities+labels+properties+direction+depth+cycle"
           : "projection-subset"});
  result.api_result.evidence.push_back(
      {"canonical.graph_depth_zero_edge_state",
       depth_zero_edge_observed ? "sql_null" : "not_present"});
  result.api_result.evidence.push_back(
      {"canonical.physical_dispatch", "generic.selected-dag.v1"});
  return result;
}

}  // namespace scratchbird::engine::sblr
