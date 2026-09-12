// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "canonical_query_document_composition.hpp"

#include "canonical_query_aggregate_composition.hpp"
#include "canonical_query_aggregate_registration.hpp"
#include "canonical_query_descriptor_support.hpp"
#include "canonical_query_filter_registration.hpp"
#include "canonical_query_json_support.hpp"
#include "canonical_query_model_family_composition_support.hpp"
#include "canonical_query_node_composition.hpp"
#include "canonical_query_object_free_composition_support.hpp"
#include "canonical_query_object_free_profile.hpp"
#include "canonical_query_physical_registration.hpp"
#include "canonical_query_projection_registration.hpp"
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
#include "nosql/document_api.hpp"
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

// SEARCH_KEY: SB_ENGINE_CANONICAL_QUERY_DOCUMENT_COMPOSITION_AUTHORITY
// Owns one admitted production document source route and its bounded relational
// tail. It consumes and revalidates engine-issued MGA statement authority and
// cannot create snapshots or publish transaction finality.

constexpr std::string_view kValuesImplementationId =
    "values.materialize.canonical.v1";

}  // namespace

// QOW-SOURCE-CES05-DOCUMENT-CANONICAL-QUERY-ROUTE-V1
CanonicalObjectFreeValuesExecutionResult ExecuteCanonicalDocumentFamilyQuery(
    const CanonicalCurrentHeapExecutionRequest& input,
    Rcp079CapturedModelLegV1* leg_capture) {
  CanonicalObjectFreeValuesExecutionResult result;
  const auto& dag = input.relational_dag;
  const auto scan = std::ranges::find_if(dag.nodes, [](const auto& node) {
    return node.node_kind == api::RelationalDagNodeKind::kScan &&
           (node.semantic_variant_id == "SBLR_MODEL_SOURCE_V1" ||
            node.semantic_variant_id == "SBLR_MODEL_EXPAND_V1");
  });
  if (scan == dag.nodes.end()) return result;
  result.profile_matched = true;

  CanonicalObjectFreeValuesExecutionRequest response_context;
  response_context.context = input.context;
  response_context.relational_dag = input.relational_dag;
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
  const auto document_producer_count = std::ranges::count_if(
      dag.nodes, [](const auto& node) {
        return node.node_kind == api::RelationalDagNodeKind::kScan &&
               (node.semantic_variant_id == "SBLR_MODEL_SOURCE_V1" ||
                node.semantic_variant_id == "SBLR_MODEL_EXPAND_V1");
      });
  if (dag.wire_version != 2 || document_producer_count != 1 ||
      !scan->input_node_ids.empty() ||
      scan->output_descriptor_ids.empty() ||
      scan->semantic_variant_id == "SBLR_MODEL_SOURCE_V1" &&
          scan->required_object_uuids.size() != 1 ||
      scan->semantic_variant_id == "SBLR_MODEL_EXPAND_V1" &&
          !scan->required_object_uuids.empty()) {
    return refuse("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                  "document canonical source graph is incomplete");
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
  const auto identity_scope = dag.bound_sblr_tree_uuid + ":" +
                              input.context.statement_uuid;
  const auto physical_alternative_uuid = DerivedCanonicalUuid(
      identity_scope,
      "alternative." + std::to_string(scan->node_id) +
          ".physical_document_path_scan_v1");
  const auto physical_cost_uuid = DerivedCanonicalUuid(
      identity_scope,
      "cost-vector." + std::to_string(scan->node_id) +
          ".physical_document_path_scan_v1");
  const auto provider_uuid =
      DerivedCanonicalUuid(identity_scope, "document.provider");
  const auto capability_uuid =
      DerivedCanonicalUuid(identity_scope, "document.capability");
  const auto policy_snapshot_uuid =
      DerivedCanonicalUuid(identity_scope, "document.policy-snapshot");
  const auto statistics_snapshot_uuid =
      DerivedCanonicalUuid(identity_scope, "document.statistics-snapshot");
  const auto resource_contract_uuid =
      DerivedCanonicalUuid(identity_scope, "document.resource-contract");
  const auto result_handle_uuid =
      DerivedCanonicalUuid(identity_scope, "document.result-handle");
  const auto property_uuid =
      DerivedCanonicalUuid(identity_scope, "document.property");
  const auto security_receipt_uuid =
      DerivedCanonicalUuid(identity_scope, "document.security-receipt");
  const auto generation = std::max<std::uint64_t>(
      1, input.context.catalog_generation_id);

  opt::ModelFamilyCoordinatorRequestV1 planning;
  planning.family_id = "document";
  planning.operation_id = scan->semantic_variant_id == "SBLR_MODEL_EXPAND_V1"
                              ? "DOCUMENT_UNNEST"
                              : "DOCUMENT_FIND";
  planning.logical_operator_id = "LOGICAL_DOCUMENT_SOURCE_V1";
  planning.logical_node_id = scan->node_id;
  if (!scan->required_object_uuids.empty()) {
    planning.object_uuid = scan->required_object_uuids.front();
  }
  planning.output_descriptor_ids = scan->output_descriptor_ids;
  planning.mga_statement_context = mga;
  planning.bound_sblr_tree_uuid = dag.bound_sblr_tree_uuid;
  planning.catalog_epoch_uuid = input.context.catalog_epoch_uuid;
  planning.security_context_uuid =
      input.context.authorization_context.authority_uuid;
  planning.capability_snapshot_uuid =
      input.context.optimizer_capability_snapshot_uuid;
  planning.resource_snapshot_uuid =
      input.context.optimizer_resource_snapshot_uuid;
  planning.statistics_snapshot_uuid = statistics_snapshot_uuid;
  planning.route_snapshot_uuid =
      input.context.optimizer_route_snapshot_uuid;
  planning.catalog_generation = generation;
  planning.current_catalog_generation = generation;
  planning.security_epoch = std::max<std::uint64_t>(1, input.context.security_epoch);
  planning.policy_epoch = std::max<std::uint64_t>(
      1, input.context.authorization_context.policy_epoch);
  planning.resource_epoch = std::max<std::uint64_t>(1, input.context.resource_epoch);
  planning.statistics_generation = generation;
  planning.route_epoch = input.context.optimizer_route_epoch;
  planning.route_generation = input.context.optimizer_route_generation;
  planning.memory_budget_bytes = input.context.optimizer_memory_budget_bytes;
  planning.security_admitted = input.context.security_context_present &&
                               input.context.authorization_context.present;
  std::vector<const api::RelationalOutputRecord*> outputs;
  for (const auto& output : dag.outputs) {
    if (output.relation_node_id == scan->node_id) outputs.push_back(&output);
  }
  std::ranges::sort(outputs, {}, &api::RelationalOutputRecord::ordinal);
  if (outputs.size() != scan->output_descriptor_ids.size()) {
    return refuse("SB_MODEL_TYPED_EXCHANGE_INVALID_V1",
                  "document output descriptors are incomplete");
  }
  if (planning.operation_id == "DOCUMENT_UNNEST") {
    // Expression-backed UNNEST is an object-free model source.  It still
    // traverses canonical logical admission, memo selection, the model-family
    // executor/exchange, generic selected-DAG dispatch, MGA revalidation, and
    // the single final result publisher; no empty object identity is ever
    // promoted into catalog/security evidence.
    const auto typed_node_for = [&](const std::uint32_t node_id) {
      return std::ranges::find_if(dag.nodes, [&](const auto& node) {
        return node.node_id == node_id;
      });
    };
    const api::RelationalDagNode* recursive_root = nullptr;
    const api::RelationalDagNode* recursive_term = nullptr;
    const api::RelationalDagNode* set_root = nullptr;
    const api::RelationalDagNode* set_values = nullptr;
    LiveSetOperationProfile document_set_profile;
    std::uint32_t composition_root_node_id = dag.root_node_id;
    const auto requested_root = typed_node_for(dag.root_node_id);
    if (requested_root != dag.nodes.end() &&
        requested_root->node_kind ==
            api::RelationalDagNodeKind::kSetOperation) {
      document_set_profile = ResolveLiveSetOperationProfileForComposition(
          requested_root->semantic_variant_id);
      if (!document_set_profile.matched ||
          document_set_profile.operation !=
              exec::CanonicalSetOperationKind::kUnion ||
          document_set_profile.quantifier !=
              exec::CanonicalSetOperationQuantifier::kAll ||
          document_set_profile.alignment !=
              exec::CanonicalSetOperationAlignment::kOrdinal ||
          document_set_profile.type_profile !=
              exec::CanonicalSetOperationTypeProfile::kExact ||
          document_set_profile.equality_profile !=
              exec::CanonicalSetOperationEqualityProfile::kExactTyped ||
          requested_root->input_node_ids.size() != 2 ||
          requested_root->input_node_ids[0] != scan->node_id ||
          requested_root->input_node_ids[1] == scan->node_id ||
          !requested_root->bound_expression_ids.empty() ||
          !requested_root->required_object_uuids.empty() ||
          !requested_root->required_property_uuids.empty() ||
          !requested_root->delivered_property_uuids.empty() ||
          requested_root->output_descriptor_ids !=
              scan->output_descriptor_ids) {
        return refuse(
            "SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
            "DOCUMENT_UNNEST set root is not exact ordinal UNION ALL");
      }
      const auto right = typed_node_for(requested_root->input_node_ids[1]);
      if (right == dag.nodes.end() ||
          right->node_kind != api::RelationalDagNodeKind::kValues ||
          right->semantic_variant_id != "values.literal-table.v1" ||
          !right->input_node_ids.empty() ||
          right->values_row_ids.size() != 1 ||
          !right->bound_expression_ids.empty() ||
          !right->required_object_uuids.empty() ||
          !right->required_property_uuids.empty() ||
          !right->delivered_property_uuids.empty() ||
          right->output_descriptor_ids != scan->output_descriptor_ids) {
        return refuse(
            "SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
            "DOCUMENT_UNNEST UNION ALL right VALUES input is not exact");
      }
      set_root = &*requested_root;
      set_values = &*right;
      composition_root_node_id = scan->node_id;
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
            "DOCUMENT_UNNEST recursive root is not the exact bounded UNION ALL int64 profile");
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
            "DOCUMENT_UNNEST recursive anchor, term, or schema is not exact");
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
            "DOCUMENT_UNNEST downstream composition is not an exact supported unary chain");
      }
      consumer_chain.push_back(&*chain_node);
      chain_node = typed_node_for(chain_node->input_node_ids.front());
    }
    if (chain_node == dag.nodes.end() ||
        !reachable_node_ids.insert(scan->node_id).second ||
        reachable_node_ids.size() != dag.nodes.size()) {
      return refuse(
          "SBLR.PLAN_TREE.INVALID_HANDLE",
          "DOCUMENT_UNNEST downstream composition is disconnected or orphaned");
    }
    std::ranges::reverse(consumer_chain);

    const auto descriptor_for = [&](const std::uint32_t descriptor_id) {
      return std::ranges::find_if(dag.descriptors, [&](const auto& descriptor) {
        return descriptor.descriptor_id == descriptor_id;
      });
    };
    const auto expression_for = [&](const std::uint32_t expression_id) {
      return std::ranges::find_if(dag.expressions, [&](const auto& expression) {
        return expression.expression_id == expression_id;
      });
    };
    if (outputs.size() != 1 || outputs.front()->ordinal != 0 ||
        !outputs.front()->visible ||
        outputs.front()->descriptor_id != scan->output_descriptor_ids.front() ||
        scan->bound_expression_ids.size() != 1 ||
        outputs.front()->expression_id != scan->bound_expression_ids.front()) {
      return refuse("SB_MODEL_TYPED_EXCHANGE_INVALID_V1",
                    "DOCUMENT_UNNEST output binding is not exact");
    }
    const auto root = expression_for(scan->bound_expression_ids.front());
    if (root == dag.expressions.end() ||
        root->expression_kind !=
            api::RelationalExpressionKind::kFunctionCall ||
        root->operator_name != "DOCUMENT_UNNEST" ||
        root->function_uuid.has_value() || root->bound_name_uuid.has_value() ||
        root->literal_kind.has_value() ||
        root->literal_or_parameter_ref.has_value() ||
        root->child_expression_ids.size() != 2 ||
        root->child_expression_ids.front() == root->child_expression_ids.back()) {
      return refuse("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                    "DOCUMENT_UNNEST root or ordered operands are incomplete");
    }
    const auto document_expression =
        expression_for(root->child_expression_ids.front());
    const auto path_expression =
        expression_for(root->child_expression_ids.back());
    if (document_expression == dag.expressions.end() ||
        path_expression == dag.expressions.end() ||
        document_expression->expression_kind !=
            api::RelationalExpressionKind::kLiteral ||
        document_expression->literal_kind !=
            api::RelationalLiteralKind::kDocument ||
        !document_expression->literal_or_parameter_ref.has_value() ||
        !document_expression->child_expression_ids.empty() ||
        document_expression->function_uuid.has_value() ||
        document_expression->bound_name_uuid.has_value() ||
        document_expression->operator_name.has_value() ||
        path_expression->expression_kind !=
            api::RelationalExpressionKind::kLiteral ||
        path_expression->literal_kind != api::RelationalLiteralKind::kString ||
        !path_expression->literal_or_parameter_ref.has_value() ||
        !path_expression->child_expression_ids.empty() ||
        path_expression->function_uuid.has_value() ||
        path_expression->bound_name_uuid.has_value() ||
        path_expression->operator_name.has_value()) {
      return refuse("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                    "DOCUMENT_UNNEST typed literal operands are not executable");
    }
    const auto output_descriptor = descriptor_for(root->result_descriptor_id);
    const auto document_descriptor =
        descriptor_for(document_expression->result_descriptor_id);
    const auto path_descriptor =
        descriptor_for(path_expression->result_descriptor_id);
    if (output_descriptor == dag.descriptors.end() ||
        document_descriptor == dag.descriptors.end() ||
        path_descriptor == dag.descriptors.end() ||
        outputs.front()->descriptor_id != root->result_descriptor_id ||
        output_descriptor->type_uuid != document_descriptor->type_uuid ||
        path_descriptor->descriptor_id == document_descriptor->descriptor_id ||
        path_descriptor->descriptor_id == output_descriptor->descriptor_id) {
      return refuse("SB_MODEL_TYPED_EXCHANGE_INVALID_V1",
                    "DOCUMENT_UNNEST typed descriptor identity was substituted");
    }

    const auto core_manifest = dt::LoadCurrentCoreDatatypeCatalogManifest();
    if (!core_manifest.ok()) {
      return refuse("SB_MODEL_TYPED_EXCHANGE_INVALID_V1",
                    "current core datatype descriptor manifest is unavailable");
    }
    const auto type_uuid_for = [](const std::string_view stable_name) {
      return ExactCanonicalCoreDatatypeTypeUuidV1(stable_name);
    };
    const auto json_type_uuid = type_uuid_for("json_document");
    const auto character_type_uuid = type_uuid_for("character");
    if (json_type_uuid.empty() || character_type_uuid.empty() ||
        document_descriptor->type_uuid != json_type_uuid ||
        output_descriptor->type_uuid != json_type_uuid ||
        path_descriptor->type_uuid != character_type_uuid) {
      return refuse("SB_MODEL_TYPED_EXCHANGE_INVALID_V1",
                    "DOCUMENT_UNNEST document or path type UUID is not canonical");
    }
    const auto unquote = [](std::string value) {
      if (value.size() >= 2 &&
          ((value.front() == '\'' && value.back() == '\'') ||
           (value.front() == '"' && value.back() == '"'))) {
        value = value.substr(1, value.size() - 2);
      }
      return value;
    };
    const auto wildcard_path =
        unquote(*path_expression->literal_or_parameter_ref);
    const auto path_shape = ExpandCanonicalDocumentWildcard(
        "{}", wildcard_path, 1, planning.memory_budget_bytes);
    if (!path_shape.ok) {
      return refuse("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                    path_shape.detail);
    }
    const auto build_descriptor = [](const api::RelationalTypeDescriptor& source,
                                     const std::string& type_name) {
      api::EngineDescriptor descriptor;
      descriptor.descriptor_uuid = source.descriptor_uuid;
      descriptor.type_uuid = source.type_uuid;
      descriptor.descriptor_kind = "scalar";
      descriptor.canonical_type_name = type_name;
      descriptor.encoded_descriptor =
          std::string("nullability=") +
          (source.nullability == api::RelationalNullability::kNullable
               ? "nullable"
               : "non_null");
      descriptor.collation_uuid = source.collation_uuid.value_or(api::EngineUuid{});
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
    exec::ExecutorColumnDescriptor unnest_column{
        outputs.front()->output_name_utf8,
        build_descriptor(*output_descriptor, "json_document"),
        output_descriptor->nullability ==
            api::RelationalNullability::kNullable,
        output_descriptor->descriptor_id};
    if (!api::QowCanonicalDescriptorIdentityV1(unnest_column.descriptor)) {
      return refuse("SB_MODEL_TYPED_EXCHANGE_INVALID_V1",
                    "DOCUMENT_UNNEST output descriptor is invalid");
    }

    const auto provider_generation = generation;
    std::vector<opt::ModelFamilyCapabilitySnapshotV1> alternatives;
    alternatives.push_back(MakeModelFamilyCapabilitySnapshotForCompositionV1(
        planning, identity_scope + ".document-unnest.native",
        opt::ModelFamilyAlternativeRouteClassV1::kNative, provider_uuid,
        capability_uuid, provider_generation, true, 1, 1,
        planning.memory_budget_bytes));
    const auto planned = PlanCanonicalModelFamilySourceForCompositionV1(
        planning, identity_scope + ".document-unnest.inventory",
        std::move(alternatives));
    if (!planned.accepted || !planned.selected ||
        !planned.data_access_allowed || !planned.optimizer_owned_enumeration ||
        planned.exact_fallback_selected ||
        planned.selected_candidate.provider_uuid != provider_uuid ||
        planned.selected_candidate.capability_uuid != capability_uuid ||
        planned.selected_candidate.provider_generation != provider_generation ||
        planned.selected_candidate.exact_collection_fallback) {
      return refuse(planned.diagnostic_id.empty()
                        ? "SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1"
                        : planned.diagnostic_id,
                    planned.detail.empty()
                        ? "DOCUMENT_UNNEST coordinator did not select the exact expression candidate"
                        : planned.detail);
    }

    api::CanonicalRelationalPlanningScope planning_scope;
    planning_scope.catalog_epoch_uuid =
        input.context.catalog_epoch_uuid;
    planning_scope.security_context_uuid =
        input.context.authorization_context.authority_uuid;
    planning_scope.statement_uuid = input.context.statement_uuid;
    planning_scope.owning_transaction_uuid =
        input.context.transaction_uuid;
    planning_scope.statement_snapshot_uuid =
        input.context.statement_snapshot_uuid;
    planning_scope.statement_metadata_snapshot_uuid =
        input.context.statement_metadata_snapshot_uuid;
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
      return refuse(
          logical.issues.empty()
              ? "QOW-DIAG-OPTIMIZER-ADMISSION-BOUND-REQUEST-V1"
              : logical.issues.front().diagnostic_id,
          logical.issues.empty() ? "DOCUMENT_UNNEST logical bridge was refused"
                                 : logical.issues.front().field_id);
    }
    plan::CanonicalMgaStatementContext current_logical_mga;
    current_logical_mga.statement_uuid = mga.statement_uuid;
    current_logical_mga.owning_transaction_uuid =
        mga.owning_transaction_uuid;
    current_logical_mga.statement_snapshot_uuid =
        mga.statement_snapshot_uuid;
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
    current_logical_mga.inventory_authoritative =
        mga.inventory_authoritative;
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
      return refuse("QOW-DIAG-QRY-004-DOCUMENT-OPTIMIZER-MGA-SNAPSHOT-V1",
                    "unnest_bridge_statement_snapshot_carriage");
    }
    logical.logical_graph.mga_statement_context = current_logical_mga;
    logical.property_catalog.mga_statement_context = current_logical_mga;

    opt::CanonicalNativeObjectFreeAdmissionContext admission_context;
    admission_context.statement_uuid = input.context.statement_uuid;
    admission_context.catalog_snapshot_uuid =
        input.context.statement_metadata_snapshot_uuid;
    admission_context.security_context_uuid =
        input.context.authorization_context.authority_uuid;
    admission_context.catalog_generation =
        input.context.catalog_generation_id;
    admission_context.authorization_catalog_generation =
        input.context.authorization_context.catalog_generation_id;
    admission_context.security_epoch =
        input.context.authorization_context.security_epoch;
    admission_context.policy_epoch =
        input.context.authorization_context.policy_epoch;
    admission_context.resource_epoch = input.context.resource_epoch;
    admission_context.capability_snapshot_uuid =
        input.context.optimizer_capability_snapshot_uuid;
    admission_context.resource_snapshot_uuid =
        input.context.optimizer_resource_snapshot_uuid;
    admission_context.route_snapshot_uuid =
        input.context.optimizer_route_snapshot_uuid;
    admission_context.route_epoch = input.context.optimizer_route_epoch;
    admission_context.route_generation =
        input.context.optimizer_route_generation;
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
    admission_context.local_transaction_id =
        input.context.local_transaction_id;
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
      return refuse("QOW-DIAG-QRY-004-DOCUMENT-OPTIMIZER-CONTEXT-V1",
                    "current_monotonic_ns");
    }
    admission_context.admitted_at_monotonic_ns = admitted_at_monotonic_ns;
    admission_context.metadata_snapshot_engine_owned = true;
    admission_context.authorization_context_engine_owned = true;
    const auto canonical_admission =
        opt::BuildCanonicalObjectFreeNativeOptimizerAdmissionRequest(
            logical.logical_graph, logical.property_catalog,
            admission_context);
    if (!canonical_admission.built ||
        !canonical_admission.admission.admitted ||
        !canonical_admission.admission.planning_allowed ||
        canonical_admission.admission.data_access_allowed ||
        canonical_admission.admission.evidence.size() != 8 ||
        !canonical_admission.request.catalog.object_uuids.empty() ||
        !canonical_admission.request.security.authorized_object_uuids.empty()) {
      return refuse(
          canonical_admission.diagnostic_id.empty()
              ? "QOW-DIAG-OPTIMIZER-ADMISSION-BOUND-REQUEST-V1"
              : canonical_admission.diagnostic_id,
          canonical_admission.field_id.empty()
              ? "DOCUMENT_UNNEST object-free optimizer admission was refused"
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
              context, left, right, comparison, diagnostic_id,
              refusal_detail);
        };
    const auto bounded_rows = static_cast<std::size_t>(
        std::min<std::uint64_t>(
            65536,
            std::min<std::uint64_t>(
                input.context.optimizer_maximum_candidate_count,
                std::numeric_limits<std::size_t>::max())));
    if (bounded_rows == 0 || planning.memory_budget_bytes <
                                 sizeof(api::EngineTypedValue)) {
      return refuse("SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                    "DOCUMENT_UNNEST resource bound is zero");
    }
    std::vector<LivePhysicalNodeProfile> profiles;
    LivePhysicalNodeProfile profile;
    profile.logical_node_id = scan->node_id;
    profile.implementation_id = "physical_document_path_scan_v1";
    profile.capability_uuid = planned.selected_candidate.capability_uuid;
    profile.logical_node_kind =
        plan::CanonicalLogicalRelationalNodeKind::kRelationSource;
    profile.physical_node_kind = exec::PhysicalNodeKind::kScan;
    profile.transformation_rule_id =
        "canonical.document.expression-unnest.v1";
    profile.estimated_rows = 1;
    profile.memory_bytes_required = std::max<std::uint64_t>(
        1, planned.selected_candidate.cost.memory_bytes_required);
    // The physical scan ABI currently requires source capabilities to carry
    // MGA-safety metadata.  This expression provider performs no storage read;
    // the one expected check is the statement-context revalidation performed
    // by the generic dispatcher and model executor.
    profile.mga_visibility_checks_expected = 1;
    profile.storage_read_capable = true;
    profile.mga_visibility_capable = true;
    profile.spill_supported = false;
    profile.parallel_safe = false;
    profile.parallel_required = false;
    profile.residual_predicate_required = false;
    profile.storage_recheck_required = false;
    profile.compatibility_profile_id = "document.local.v1";
    profiles.push_back(std::move(profile));

    const auto logical_node_for = [&](const std::uint32_t node_id) {
      return std::ranges::find_if(
          canonical_admission.request.logical_graph.nodes,
          [&](const auto& node) { return node.logical_node_id == node_id; });
    };
    auto previous_logical = logical_node_for(scan->node_id);
    if (previous_logical ==
        canonical_admission.request.logical_graph.nodes.end()) {
      return refuse("SBLR.PLAN_TREE.INVALID_HANDLE",
                    "DOCUMENT_UNNEST producer logical identity is absent");
    }
    MaterializedValues composition_state;
    composition_state.ok = true;
    composition_state.batch.columns = {unnest_column};
    exec::CanonicalResultColumnDescriptor producer_published;
    producer_published.ordinal = 0;
    producer_published.name_utf8 = outputs.front()->output_name_utf8;
    producer_published.descriptor_uuid = output_descriptor->descriptor_uuid;
    producer_published.type_uuid = output_descriptor->type_uuid;
    producer_published.nullability = ResultNullability(
        output_descriptor->nullability);
    producer_published.collation_uuid = output_descriptor->collation_uuid;
    producer_published.timezone_profile_id =
        output_descriptor->timezone_profile_id;
    composition_state.result_bindings.push_back(
        {0, outputs.front()->visible, std::move(producer_published)});

    std::optional<PreparedFilterRoot> prepared_filter;
    std::optional<PreparedProjectRoot> prepared_project;
    std::vector<std::size_t> direct_projected_columns;
    std::optional<PreparedSortRoot> prepared_sort;
    std::optional<exec::ExecutorColumnDescriptor> prepared_row_number;
    std::optional<PreparedGlobalAggregateRoot> prepared_count_star;
    bool prepared_nonrecursive_cte = false;
    std::optional<PreparedRecursiveCteRoot> prepared_recursive_cte;
    std::optional<PreparedLiveSetNode> prepared_document_set;
    std::optional<MaterializedValues> materialized_document_set_values;
    std::optional<PreparedLimitRoot> prepared_limit;
    std::size_t document_source_row_bound = bounded_rows;
    std::uint64_t limit_count = 0;
    std::uint64_t limit_offset = 0;
    bool fetch_first_rows_only = false;
    std::string limit_implementation_id;
    std::string filter_capability_uuid;
    std::string project_capability_uuid;
    std::string sort_capability_uuid;
    std::string window_capability_uuid;
    std::string window_order_evidence_uuid;
    std::string aggregate_capability_uuid;
    std::string cte_capability_uuid;
    std::string nonrecursive_cte_implementation_id;
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
                      "DOCUMENT_UNNEST consumer identity or ordering is invalid");
      }
      LivePhysicalNodeProfile consumer_profile;
      consumer_profile.logical_node_id = consumer->node_id;
      consumer_profile.logical_node_kind = logical_consumer->node_kind;
      consumer_profile.estimated_rows = bounded_rows;
      consumer_profile.memory_bytes_required = 1;
      consumer_profile.minimum_input_count = 1;
      consumer_profile.maximum_input_count = 1;
      consumer_profile.runtime_peak_from_callback_batches = true;
      switch (consumer->node_kind) {
        case api::RelationalDagNodeKind::kFilter: {
          if (consumer->semantic_variant_id != "filter.where.v1") {
            return refuse("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                          "DOCUMENT_UNNEST FILTER semantic is not canonical");
          }
        auto prepared = PrepareFilterRootForComposition(
              dag, *logical_consumer, *previous_logical, composition_state);
          if (!prepared.ok) {
            return refuse("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                          prepared.detail);
          }
          prepared_filter = std::move(prepared);
          filter_capability_uuid = DerivedCanonicalUuid(
              identity_scope, "document-unnest.filter.capability");
          consumer_profile.implementation_id = "filter.3vl.row.v1";
          consumer_profile.capability_uuid = filter_capability_uuid;
          consumer_profile.physical_node_kind = exec::PhysicalNodeKind::kFilter;
          consumer_profile.transformation_rule_id =
              "canonical.document-unnest.filter.3vl.v1";
          consumer_profile.memory_bytes_required = planning.memory_budget_bytes;
          break;
        }
        case api::RelationalDagNodeKind::kProject: {
          if (consumer->semantic_variant_id != "project.select-list.v1" ||
              consumer->bound_expression_ids.empty()) {
            return refuse("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                          "DOCUMENT_UNNEST PROJECT semantic is not canonical");
          }
          project_capability_uuid = DerivedCanonicalUuid(
              identity_scope, "document-unnest.project.capability");
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
               descriptor_direct && ordinal < project_outputs.size();
               ++ordinal) {
            const auto expression = expression_for(
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
                source_descriptor != previous_logical->output_descriptor_ids.end();
            if (!descriptor_direct) break;
            const auto source_ordinal = static_cast<std::size_t>(std::distance(
                previous_logical->output_descriptor_ids.begin(),
                source_descriptor));
            direct_projected_columns.push_back(source_ordinal);
            projected_columns.push_back(
                composition_state.batch.columns[source_ordinal]);
            exec::CanonicalResultColumnBinding binding;
            binding.physical_column_ordinal = ordinal;
            binding.visible = project_outputs[ordinal]->visible;
            if (binding.visible) {
              const auto descriptor = descriptor_for(
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
          consumer_profile.physical_node_kind =
              exec::PhysicalNodeKind::kProject;
          consumer_profile.transformation_rule_id =
              descriptor_direct
                  ? "canonical.document-unnest.project.descriptor-direct.v1"
                  : "canonical.document-unnest.project.expression-row.v1";
          consumer_profile.memory_bytes_required =
              planning.memory_budget_bytes;
          break;
        }
        case api::RelationalDagNodeKind::kSort: {
          if (consumer->semantic_variant_id != "sort.required-order.v1") {
            return refuse("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                          "DOCUMENT_UNNEST SORT semantic is not canonical");
          }
          const bool expression_ordering = std::ranges::any_of(
              consumer->bound_expression_ids,
              [&](const std::uint32_t expression_id) {
                const auto expression = expression_for(expression_id);
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
          auto prepared = expression_ordering
                              ? PrepareExpressionSortRootForComposition(
                                    input.context, dag, sort_properties,
                                    *logical_consumer, *previous_logical,
                                    composition_state,
                                    canonical_planning_request
                                        .expression_services)
                              : PrepareSortRootForComposition(
                                    input.context, dag, sort_properties,
                                    *logical_consumer, *previous_logical,
                                    composition_state);
          if (!prepared.ok) {
            return refuse("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                          prepared.detail);
          }
          prepared_sort = std::move(prepared);
          sort_capability_uuid = DerivedCanonicalUuid(
              identity_scope, "document-unnest.sort.capability");
          consumer_profile.implementation_id =
              prepared_sort->expression_ordering
                  ? "sort.typed.expression-row.v1"
                  : "sort.typed.terms.v1";
          consumer_profile.capability_uuid = sort_capability_uuid;
          consumer_profile.physical_node_kind = exec::PhysicalNodeKind::kSort;
          consumer_profile.transformation_rule_id =
              prepared_sort->expression_ordering
                  ? "canonical.document-unnest.sort.expression-order.v1"
                  : "canonical.document-unnest.sort.required-order.v1";
          // SORT is the enforcing node: its implementation supplies the
          // ordering property rather than requiring an already-ordered input.
          consumer_profile.required_property_uuids.clear();
          consumer_profile.delivered_property_uuids =
              logical_consumer->delivered_property_uuids;
          consumer_profile.supported_property_kinds = {
              plan::CanonicalLogicalPropertyKind::kOrdering};
          consumer_profile.memory_bytes_required =
              planning.memory_budget_bytes;
          break;
        }
        case api::RelationalDagNodeKind::kWindow: {
          constexpr std::string_view kRowNumberFunctionUuid =
              "019de5fc-2400-7539-bcce-00eef3ae7220";
          std::vector<const api::RelationalWindowDefinitionRecord*>
              definitions;
          std::vector<const api::RelationalWindowInvocationRecord*>
              invocations;
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
                  ? expression_for(invocations.front()
                                       ->function_expression_id)
                  : dag.expressions.end();
          const auto row_number_descriptor = descriptor_for(
              invocations.size() == 1
                  ? invocations.front()->result_descriptor_id
                  : 0);
          const auto int64_type_uuid = type_uuid_for("int64");
          const auto typed_sort =
              typed_node_for(previous_logical->logical_node_id);
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
          if (consumer->semantic_variant_id != "window.row-number.v1" ||
              consumer->node_id != dag.root_node_id || !ordered_input ||
              definitions.size() != 1 ||
              invocations.size() != 1 || window_outputs.size() != 2 ||
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
              consumer->output_descriptor_ids !=
                  std::vector<std::uint32_t>{
                      previous_logical->output_descriptor_ids.front(),
                      row_number_descriptor->descriptor_id} ||
              logical_consumer->output_descriptor_ids !=
                  consumer->output_descriptor_ids ||
              composition_state.batch.columns.size() != 1 ||
              composition_state.result_bindings.size() != 1 ||
              window_outputs.front()->ordinal != 0 ||
              !window_outputs.front()->visible ||
              window_outputs.front()->descriptor_id !=
                  previous_logical->output_descriptor_ids.front() ||
              window_outputs.front()->expression_id !=
                  outputs.front()->expression_id ||
              window_outputs.front()->output_name_utf8 !=
                  outputs.front()->output_name_utf8 ||
              window_outputs.back()->ordinal != 1 ||
              !window_outputs.back()->visible ||
              window_outputs.back()->expression_id !=
                  function->expression_id ||
              window_outputs.back()->descriptor_id !=
                  row_number_descriptor->descriptor_id ||
              window_outputs.back()->output_name_utf8 !=
                  invocations.front()->output_name_utf8) {
            return refuse(
                "SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                "DOCUMENT_UNNEST ROW_NUMBER window binding is not exact");
          }
          const auto window_property_uuid = std::ranges::find_if(
              consumer->delivered_property_uuids,
              [&](const auto& property_uuid) {
                return property_uuid != prepared_sort->ordering_property_uuid;
              });
          const auto window_property = std::ranges::find_if(
              canonical_admission.request.logical_properties.properties,
              [&](const auto& property) {
                return window_property_uuid !=
                           consumer->delivered_property_uuids.end() &&
                       property.property_uuid == *window_property_uuid;
              });
          if (window_property_uuid ==
                  consumer->delivered_property_uuids.end() ||
              window_property == canonical_admission.request
                                     .logical_properties.properties.end() ||
              window_property->property_kind !=
                  plan::CanonicalLogicalPropertyKind::kWindow ||
              window_property->origin_logical_node_id != consumer->node_id ||
              window_property->dependency_property_uuids !=
                  std::vector<std::string>{
                      prepared_sort->ordering_property_uuid} ||
              window_property->window_frame_descriptor_uuid.empty()) {
            return refuse(
                "QOW-DIAG-WINDOW-PROPERTY-CARRIAGE-V1",
                "DOCUMENT_UNNEST ROW_NUMBER property binding is not exact");
          }
          if (row_number_descriptor->descriptor_uuid ==
                  row_number_descriptor->type_uuid ||
              row_number_descriptor->descriptor_uuid ==
                  kRowNumberFunctionUuid ||
              row_number_descriptor->descriptor_uuid ==
                  prepared_sort->ordering_property_uuid ||
              row_number_descriptor->descriptor_uuid ==
                  *window_property_uuid ||
              row_number_descriptor->descriptor_uuid ==
                  window_property->window_frame_descriptor_uuid) {
            return refuse(
                "SB_MODEL_TYPED_EXCHANGE_INVALID_V1",
                "DOCUMENT_UNNEST ROW_NUMBER result descriptor identity is "
                "not independent");
          }
          exec::ExecutorColumnDescriptor row_number_column{
              window_outputs.back()->output_name_utf8,
              build_descriptor(*row_number_descriptor, "int64"), false,
              row_number_descriptor->descriptor_id};
          if (!api::QowCanonicalDescriptorIdentityV1(
                  row_number_column.descriptor)) {
            return refuse("SB_MODEL_TYPED_EXCHANGE_INVALID_V1",
                          "DOCUMENT_UNNEST ROW_NUMBER descriptor is invalid");
          }
          exec::CanonicalResultColumnBinding row_number_binding;
          row_number_binding.physical_column_ordinal = 1;
          row_number_binding.visible = true;
          row_number_binding.published_descriptor =
              exec::CanonicalResultColumnDescriptor{
                  1, window_outputs.back()->output_name_utf8,
                  row_number_descriptor->descriptor_uuid,
                  row_number_descriptor->type_uuid,
                  exec::CanonicalResultNullability::kNonNull, std::nullopt,
                  std::nullopt};
          composition_state.batch.columns.push_back(row_number_column);
          composition_state.result_bindings.push_back(
              std::move(row_number_binding));
          prepared_row_number = std::move(row_number_column);
          window_capability_uuid = DerivedCanonicalUuid(
              identity_scope, "document-unnest.window.row-number.capability");
          window_order_evidence_uuid = DerivedCanonicalUuid(
              identity_scope + ":" + prepared_sort->ordering_property_uuid,
              "document-unnest.window.deterministic-order");
          consumer_profile.implementation_id = "window.row-number.v1";
          consumer_profile.capability_uuid = window_capability_uuid;
          consumer_profile.physical_node_kind = exec::PhysicalNodeKind::kWindow;
          consumer_profile.transformation_rule_id =
              "canonical.document-unnest.window.row-number.v1";
          consumer_profile.required_property_uuids =
              logical_consumer->required_property_uuids;
          consumer_profile.delivered_property_uuids =
              logical_consumer->delivered_property_uuids;
          consumer_profile.supported_property_kinds = {
              plan::CanonicalLogicalPropertyKind::kOrdering,
              plan::CanonicalLogicalPropertyKind::kWindow};
          consumer_profile.memory_bytes_required =
              planning.memory_budget_bytes;
          break;
        }
        case api::RelationalDagNodeKind::kAggregate: {
          const auto aggregate_profile =
              MatchLiveUnaryAggregateExpressionProfileForComposition(
                  consumer->semantic_variant_id);
          if (!aggregate_profile.matched || !aggregate_profile.count_star ||
              aggregate_profile.function !=
                  exec::CanonicalAggregateFunction::count ||
              aggregate_profile.distinct || aggregate_profile.has_filter) {
            return refuse(
                "SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                "DOCUMENT_UNNEST aggregate is not exact global COUNT(*)");
          }
          const auto count_descriptor = descriptor_for(
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
                "DOCUMENT_UNNEST COUNT(*) result descriptor is not canonical int64");
          }
          auto prepared = PrepareGlobalAggregateRootForComposition(
              dag, *logical_consumer, *previous_logical,
              composition_state, aggregate_profile.function, true, false,
              false);
          if (!prepared.ok) {
            return refuse("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                          prepared.detail);
          }
          composition_state.batch.columns = {prepared.result_column};
          composition_state.result_bindings = prepared.result_bindings;
          prepared_count_star = std::move(prepared);
          aggregate_capability_uuid = DerivedCanonicalUuid(
              identity_scope, "document-unnest.count-star.capability");
          consumer_profile.implementation_id = "aggregate.count-star.v1";
          consumer_profile.capability_uuid = aggregate_capability_uuid;
          consumer_profile.physical_node_kind =
              exec::PhysicalNodeKind::kAggregate;
          consumer_profile.transformation_rule_id =
              aggregate_profile.transformation_id;
          consumer_profile.estimated_rows = 1;
          consumer_profile.memory_bytes_required =
              planning.memory_budget_bytes;
          break;
        }
        case api::RelationalDagNodeKind::kCte: {
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
                "DOCUMENT_UNNEST nonrecursive CTE is not an exact schema-preserving bound CTE");
          }
          const auto validated = exec::ValidateCanonicalDescriptorBatch(
              composition_state.batch, consumer->output_descriptor_ids);
          if (!validated.ok) {
            return refuse("SB_MODEL_TYPED_EXCHANGE_INVALID_V1",
                          "DOCUMENT_UNNEST nonrecursive CTE input: " +
                              validated.detail);
          }
          prepared_nonrecursive_cte = true;
          nonrecursive_cte_implementation_id =
              consumer->shareable ? "cte.bound.materialize.typed.v1"
                                  : "cte.bound.inline.typed.v1";
          cte_capability_uuid = DerivedCanonicalUuid(
              identity_scope,
              consumer->shareable
                  ? "document-unnest.cte.materialize.capability"
                  : "document-unnest.cte.inline.capability");
          consumer_profile.implementation_id =
              nonrecursive_cte_implementation_id;
          consumer_profile.capability_uuid = cte_capability_uuid;
          consumer_profile.physical_node_kind = exec::PhysicalNodeKind::kCte;
          consumer_profile.transformation_rule_id =
              consumer->shareable
                  ? "canonical.document-unnest.cte.composed-materialize.v1"
                  : "canonical.document-unnest.cte.composed-inline.v1";
          consumer_profile.memory_bytes_required =
              planning.memory_budget_bytes;
          consumer_profile.runtime_auxiliary_from_first_input_batch =
              consumer->shareable;
          break;
        }
        case api::RelationalDagNodeKind::kLimit: {
          const auto expected_arity =
              consumer->semantic_variant_id == "limit.bound-count.v1" ? 1U
                                                                        : 2U;
          fetch_first_rows_only =
              consumer->semantic_variant_id ==
              "fetch.first-rows-only-offset.v1";
          if ((consumer->semantic_variant_id != "limit.bound-count.v1" &&
               consumer->semantic_variant_id !=
                   "limit.bound-count-offset.v1" &&
               !fetch_first_rows_only) ||
              consumer->bound_expression_ids.size() != expected_arity) {
            return refuse("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                          "DOCUMENT_UNNEST LIMIT/FETCH semantic is not canonical");
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
                          detail.empty()
                              ? "DOCUMENT_UNNEST LIMIT/FETCH bound is invalid"
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
              identity_scope, "document-unnest.limit.capability");
          consumer_profile.implementation_id = limit_implementation_id;
          consumer_profile.capability_uuid = limit_capability_uuid;
          consumer_profile.physical_node_kind = exec::PhysicalNodeKind::kLimit;
          consumer_profile.transformation_rule_id =
              fetch_first_rows_only
                  ? "canonical.document-unnest.fetch.first-rows-only.v1"
                  : "canonical.document-unnest.limit.bound-count-offset.v1";
          consumer_profile.memory_bytes_required =
              planning.memory_budget_bytes;
          break;
        }
        default:
          return refuse("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                        "DOCUMENT_UNNEST consumer kind is unsupported");
      }
      profiles.push_back(std::move(consumer_profile));
      previous_logical = logical_consumer;
    }
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
            "DOCUMENT_UNNEST UNION ALL logical input identity is not exact");
      }
      MaterializedValues right;
      const auto values_row = std::ranges::find_if(
          dag.values_rows, [&](const auto& row) {
            return row.row_id == set_values->values_row_ids.front();
          });
      std::vector<const api::RelationalOutputRecord*> values_outputs;
      for (const auto& output : dag.outputs) {
        if (output.relation_node_id == set_values->node_id) {
          values_outputs.push_back(&output);
        }
      }
      std::ranges::sort(values_outputs, {},
                        &api::RelationalOutputRecord::ordinal);
      const api::RelationalExpressionRecord* values_expression = nullptr;
      if (values_row != dag.values_rows.end() &&
          values_row->expression_ids.size() == 1) {
        const auto found = expression_for(values_row->expression_ids.front());
        if (found != dag.expressions.end()) values_expression = &*found;
      }
      if (values_expression == nullptr || values_outputs.size() != 1 ||
          values_outputs.front()->ordinal != 0 ||
          !values_outputs.front()->visible ||
          values_outputs.front()->output_name_utf8 !=
              outputs.front()->output_name_utf8 ||
          values_outputs.front()->expression_id !=
              values_expression->expression_id ||
          values_outputs.front()->descriptor_id !=
              set_values->output_descriptor_ids.front() ||
          values_expression->expression_kind !=
              api::RelationalExpressionKind::kLiteral ||
          values_expression->literal_kind !=
              api::RelationalLiteralKind::kDocument ||
          !values_expression->literal_or_parameter_ref.has_value() ||
          values_expression->result_descriptor_id !=
              set_values->output_descriptor_ids.front() ||
          !values_expression->child_expression_ids.empty() ||
          values_expression->function_uuid.has_value() ||
          values_expression->bound_name_uuid.has_value() ||
          values_expression->operator_name.has_value() ||
          output_descriptor->nullability !=
              api::RelationalNullability::kNullable) {
        return refuse(
            "SB_MODEL_TYPED_EXCHANGE_INVALID_V1",
            "DOCUMENT_UNNEST UNION ALL right VALUES document binding is not exact");
      }
      api::EngineCanonicalizeDocumentValueRequest canonicalize_values;
      canonicalize_values.context = input.context;
      canonicalize_values.reference_profile =
          "SBSQL_DOCUMENT_SET_VALUES_V1";
      canonicalize_values.input_value.descriptor.descriptor_kind = "scalar";
      canonicalize_values.input_value.descriptor.canonical_type_name =
          "json_document";
      canonicalize_values.input_value.encoded_value =
          *values_expression->literal_or_parameter_ref;
      canonicalize_values.input_value.setState(api::EngineValueState::value);
      const auto canonical_values =
          api::EngineCanonicalizeDocumentValue(canonicalize_values);
      if (!canonical_values.ok ||
          canonical_values.canonical_format != "json_text" ||
          canonical_values.value.state != api::EngineValueState::value ||
          canonical_values.value.is_null ||
          canonical_values.value.descriptor.canonical_type_name !=
              "json_document") {
        return refuse("SB_MODEL_TYPED_EXCHANGE_INVALID_V1",
                      "DOCUMENT_UNNEST UNION ALL right VALUES JSON is invalid");
      }
      auto values_column = unnest_column;
      values_column.stable_name = values_outputs.front()->output_name_utf8;
      api::EngineTypedValue values_value;
      values_value.descriptor = values_column.descriptor;
      values_value.encoded_value = canonical_values.value.encoded_value;
      values_value.setState(api::EngineValueState::value);
      right.batch.columns = {std::move(values_column)};
      right.batch.rows = {{{std::move(values_value)}}};
      exec::CanonicalResultColumnBinding values_binding;
      values_binding.physical_column_ordinal = 0;
      values_binding.visible = true;
      values_binding.published_descriptor =
          exec::CanonicalResultColumnDescriptor{
              0, values_outputs.front()->output_name_utf8,
              output_descriptor->descriptor_uuid,
              output_descriptor->type_uuid,
              ResultNullability(output_descriptor->nullability),
              output_descriptor->collation_uuid,
              output_descriptor->timezone_profile_id};
      right.result_bindings = {std::move(values_binding)};
      const auto canonical_batch = exec::ValidateCanonicalDescriptorBatch(
          right.batch, set_values->output_descriptor_ids);
      if (!canonical_batch.ok) {
        return refuse(
            "SB_MODEL_TYPED_EXCHANGE_INVALID_V1",
            "DOCUMENT_UNNEST UNION ALL right VALUES descriptor is invalid: " +
                canonical_batch.diagnostic_code + ":" +
                canonical_batch.detail);
      }
      right.ok = true;
      if (right.batch.rows.size() >= bounded_rows) {
        return refuse(
            "QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1",
            "DOCUMENT_UNNEST UNION ALL leaves no bounded source row budget");
      }
      document_source_row_bound = bounded_rows - right.batch.rows.size();
      auto prepared = PrepareSetOperationRootForComposition(
          input.context, dag, *logical_root, composition_state, right,
          document_set_profile);
      if (!prepared.ok) {
        return refuse("SB_MODEL_TYPED_EXCHANGE_INVALID_V1",
                      "DOCUMENT_UNNEST UNION ALL: " + prepared.detail);
      }
      std::uint64_t comparison_bound = 0;
      std::uint64_t collation_comparison_count = 0;
      if (!BoundSetOperationEqualityComparisonsForComposition(
              prepared, document_set_profile, bounded_rows,
              &comparison_bound, &collation_comparison_count) ||
          comparison_bound > std::numeric_limits<std::size_t>::max()) {
        return refuse(
            "QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
            "DOCUMENT_UNNEST UNION ALL comparison bound overflowed");
      }
      std::uint64_t values_memory = 1;
      if (!AddBatchMemoryBytes(right.batch, &values_memory) ||
          values_memory > planning.memory_budget_bytes) {
        return refuse(
            "SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
            "DOCUMENT_UNNEST UNION ALL VALUES memory bound was exceeded");
      }
      set_values_capability_uuid = DerivedCanonicalUuid(
          identity_scope, "document-unnest.set-values.capability");
      set_root_capability_uuid = DerivedCanonicalUuid(
          identity_scope, "document-unnest.set-union-all.capability");

      LivePhysicalNodeProfile values_profile;
      values_profile.logical_node_id = logical_values->logical_node_id;
      values_profile.implementation_id = std::string(kValuesImplementationId);
      values_profile.capability_uuid = set_values_capability_uuid;
      values_profile.logical_node_kind = logical_values->node_kind;
      values_profile.physical_node_kind = exec::PhysicalNodeKind::kValues;
      values_profile.transformation_rule_id =
          "canonical.document-unnest.set-values.materialize.v1";
      values_profile.estimated_rows = right.batch.rows.size();
      values_profile.memory_bytes_required = values_memory;
      profiles.push_back(std::move(values_profile));

      LivePhysicalNodeProfile set_profile;
      set_profile.logical_node_id = logical_root->logical_node_id;
      set_profile.implementation_id = document_set_profile.implementation_id;
      set_profile.capability_uuid = set_root_capability_uuid;
      set_profile.logical_node_kind = logical_root->node_kind;
      set_profile.physical_node_kind = exec::PhysicalNodeKind::kSetOperation;
      set_profile.transformation_rule_id =
          "canonical.document-unnest.set.union-all.ordinal.v1";
      set_profile.estimated_rows = bounded_rows;
      set_profile.memory_bytes_required = planning.memory_budget_bytes;
      set_profile.minimum_input_count = 2;
      set_profile.maximum_input_count = 2;
      set_profile.runtime_peak_from_callback_batches = true;
      profiles.push_back(std::move(set_profile));

      composition_state.batch.columns = prepared.result_columns;
      composition_state.result_bindings = prepared.result_bindings;
      prepared_document_set = PreparedLiveSetNode{
          document_set_profile, std::move(prepared), bounded_rows,
          std::max<std::size_t>(
              1, static_cast<std::size_t>(comparison_bound))};
      materialized_document_set_values = std::move(right);
      previous_logical = logical_root;
    }
    if (recursive_root != nullptr) {
      const auto logical_term = logical_node_for(recursive_term->node_id);
      const auto logical_root = logical_node_for(recursive_root->node_id);
      if (logical_term ==
              canonical_admission.request.logical_graph.nodes.end() ||
          logical_root ==
              canonical_admission.request.logical_graph.nodes.end() ||
          logical_term->input_logical_node_ids.size() != 0 ||
          logical_root->input_logical_node_ids !=
              std::vector<std::uint32_t>{previous_logical->logical_node_id,
                                         logical_term->logical_node_id} ||
          composition_state.batch.columns.size() != 1 ||
          composition_state.batch.columns.front().descriptor
                  .canonical_type_name != "int64") {
        return refuse(
            "SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
            "DOCUMENT_UNNEST recursive logical branch is not an exact int64 anchor and empty term");
      }
      CanonicalRelationalExpressionRuntime runtime(
          dag, canonical_planning_request.expression_services);
      std::uint64_t upper_bound = 0;
      std::string detail;
      if (!EvaluateNonNegativeRowBoundForComposition(
              &runtime, recursive_root->bound_expression_ids.front(),
              &upper_bound, &detail) ||
          upper_bound >= bounded_rows ||
          upper_bound >=
              static_cast<std::uint64_t>(
                  std::numeric_limits<std::size_t>::max())) {
        return refuse(
            "QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1",
            detail.empty()
                ? "DOCUMENT_UNNEST recursive upper bound exceeds its admitted work bound"
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
              upper_bound, bounded_rows)) {
        return refuse(
            "QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
            "DOCUMENT_UNNEST recursive cardinality bound is invalid");
      }
      if (!BindPreparedRecursiveCtePeakMemory(
              &prepared, planning.memory_budget_bytes)) {
        return refuse(
            "SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
            "document recursive payload peak exceeds its admitted memory budget");
      }
      prepared_recursive_cte = prepared;
      recursive_term_capability_uuid = DerivedCanonicalUuid(
          identity_scope, "document-unnest.recursive-term.capability");
      recursive_root_capability_uuid = DerivedCanonicalUuid(
          identity_scope, "document-unnest.recursive-root.capability");

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
      root_profile.physical_node_kind =
          exec::PhysicalNodeKind::kRecursiveCte;
      root_profile.transformation_rule_id =
          prepared.profile.transformation_id;
      root_profile.estimated_rows = prepared.maximum_result_row_count;
      root_profile.memory_bytes_required =
          prepared.planned_peak_memory_bytes;
      root_profile.minimum_input_count = 2;
      root_profile.maximum_input_count = 2;
      profiles.push_back(std::move(root_profile));
    }
    if (!CompleteLiveRuntimeMemoryReceipts(&profiles)) {
      return refuse("QOW-DIAG-OPT-017-REFUSAL-V1",
                    "DOCUMENT_UNNEST composition memory receipts are incomplete");
    }
    const auto physical = PlanAndPublishLivePhysicalDag(
        canonical_planning_request, profiles, "document.unnest.selected-plan",
        "document expression unnest", "document.local.v1");
    const auto producer_physical = std::ranges::find_if(
        physical.physical_dag.nodes, [&](const auto& node) {
          return node.relational_node_id == scan->node_id;
        });
    if (!physical.ok ||
        physical.physical_dag.nodes.size() != profiles.size() ||
        producer_physical == physical.physical_dag.nodes.end() ||
        producer_physical->logical_semantic_variant_id !=
            "sblr.model_expand.v1" ||
        producer_physical->implementation_id !=
            "physical_document_path_scan_v1" ||
        producer_physical->executor_capability_uuid !=
            planned.selected_candidate.capability_uuid ||
        producer_physical->selected_alternative_uuid !=
            physical_alternative_uuid ||
        producer_physical->cost_vector_uuid !=
            physical_cost_uuid) {
      return refuse(
          physical.diagnostic_id.empty()
              ? "QOW-DIAG-OPTIMIZER-PHYSICAL-PUBLICATION-V1"
              : physical.diagnostic_id,
          physical.detail.empty()
              ? "DOCUMENT_UNNEST canonical physical DAG was not published"
              : physical.detail);
    }
    result.optimizer_selected = true;
    result.physical_dag_published = true;
    result.optimizer_admission_stage_count =
        canonical_admission.admission.evidence.size();
    result.physical_node_count = physical.physical_dag.nodes.size();
    result.selected_plan_uuid = physical.physical_dag.selected_plan_uuid;

    exec::ModelSourceInputDescriptorV1 source_input;
    source_input.family_id = "document";
    source_input.operation_id = "DOCUMENT_UNNEST";
    source_input.physical_node_id =
        producer_physical->physical_node_id;
    source_input.selected_alternative_uuid =
        producer_physical->selected_alternative_uuid;
    source_input.capability_uuid =
        producer_physical->executor_capability_uuid;
    source_input.provider_uuid = provider_uuid;
    source_input.provider_generation = provider_generation;
    source_input.result_handle_uuid = result_handle_uuid;
    source_input.causal_counter_id =
        producer_physical->causal_counter_id;
    source_input.output_descriptor_ids = scan->output_descriptor_ids;
    source_input.mga_statement_context = mga;
    source_input.catalog_epoch_uuid =
        input.context.catalog_epoch_uuid;
    source_input.security_context_uuid =
        input.context.authorization_context.authority_uuid;
    source_input.policy_snapshot_uuid = policy_snapshot_uuid;
    source_input.resource_contract_uuid = resource_contract_uuid;
    source_input.catalog_generation = generation;
    source_input.descriptor_generation = generation;
    source_input.security_generation = planning.security_epoch;
    source_input.policy_generation = planning.policy_epoch;
    source_input.resource_generation = planning.resource_epoch;
    source_input.maximum_rows = document_source_row_bound;
    source_input.maximum_cells = document_source_row_bound;
    source_input.maximum_memory_bytes = planning.memory_budget_bytes;

    exec::ModelFamilyExecutionRequestV1 execution_request;
    execution_request.input = source_input;
    execution_request.capability.capability_uuid =
        source_input.capability_uuid;
    execution_request.capability.family_id = "document";
    execution_request.capability.provider_uuid = provider_uuid;
    execution_request.capability.provider_generation = provider_generation;
    execution_request.capability.available = true;
    execution_request.capability.exact = true;
    execution_request.capability.exact_collection_fallback_available = false;
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
    execution_request.exact_fallback_selected = false;
    execution_request.security_admitted = planning.security_admitted;
    execution_request.current_catalog_generation = generation;
    execution_request.current_descriptor_generation = generation;
    execution_request.current_security_generation = planning.security_epoch;
    execution_request.current_policy_generation = planning.policy_epoch;
    execution_request.current_resource_generation = planning.resource_epoch;
    execution_request.current_provider_generation = provider_generation;
    execution_request.current_mga_statement_context = mga;
    execution_request.execute_provider =
        [context = input.context,
         document_payload = *document_expression->literal_or_parameter_ref,
         wildcard_path, unnest_column, identity_scope, property_uuid,
         security_receipt_uuid, source_input](
            const exec::ModelSourceInputDescriptorV1& selected_input) {
          exec::ModelProviderExecutionResultV1 provider;
          provider.data_access_observed = true;
          provider.rows_examined = 1;
          api::EngineCanonicalizeDocumentValueRequest canonicalize;
          canonicalize.context = context;
          canonicalize.reference_profile = "SBSQL_DOCUMENT_UNNEST_V1";
          canonicalize.input_value.descriptor.descriptor_kind = "scalar";
          canonicalize.input_value.descriptor.canonical_type_name = "document";
          canonicalize.input_value.encoded_value = document_payload;
          canonicalize.input_value.setState(api::EngineValueState::value);
          std::string canonical_json;
          {
            auto canonical =
                api::EngineCanonicalizeDocumentValue(canonicalize);
            if (!canonical.ok || canonical.canonical_format != "json_text" ||
                canonical.value.descriptor.canonical_type_name !=
                    "json_document") {
              provider.diagnostic_id =
                  "SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1";
              provider.detail =
                  canonical.diagnostics.empty()
                      ? "DOCUMENT_UNNEST input is not canonical JSON"
                      : canonical.diagnostics.front().detail;
              return provider;
            }
            canonical_json = std::move(canonical.value.encoded_value);
          }
          if (canonical_json.size() > selected_input.maximum_memory_bytes) {
            provider.diagnostic_id = "SB_MODEL_RESOURCE_MEMORY_REFUSED_V1";
            provider.detail =
                "DOCUMENT_UNNEST input exceeded its memory bound";
            return provider;
          }
          auto expansion = ExpandCanonicalDocumentWildcard(
              canonical_json, wildcard_path,
              selected_input.maximum_rows,
              selected_input.maximum_memory_bytes);
          if (!expansion.ok) {
            provider.diagnostic_id =
                expansion.detail.find("bound") != std::string::npos
                    ? "SB_MODEL_RESOURCE_MEMORY_REFUSED_V1"
                    : "SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1";
            provider.detail = expansion.detail;
            return provider;
          }
          // Peak admission covers the simultaneous canonical input, the
          // bounded expansion inventory, the eventual exchange batch, and
          // both identity vectors.  Elements are moved into the batch below,
          // but the conservative calculation intentionally counts their
          // payload in both inventories so allocator/capacity behavior cannot
          // make the claimed peak optimistic.
          std::uint64_t peak_bytes =
              sizeof(exec::ModelProviderExecutionResultV1) +
              sizeof(exec::ModelProviderBatchV1) +
              sizeof(exec::DescriptorBatch) +
              sizeof(CanonicalDocumentWildcardExpansion);
          const auto account_peak = [&](const std::uint64_t bytes) {
            return CheckedAdd(peak_bytes, bytes, &peak_bytes) &&
                   peak_bytes <= selected_input.maximum_memory_bytes;
          };
          bool peak_ok = account_peak(document_payload.size()) &&
                         account_peak(canonical_json.size()) &&
                         account_peak(sizeof(exec::ExecutorColumnDescriptor)) &&
                         account_peak(unnest_column.stable_name.size()) &&
                         account_peak(unnest_column.descriptor.descriptor_uuid
                                          .size()) &&
                         account_peak(unnest_column.descriptor.descriptor_kind
                                          .size()) &&
                         account_peak(unnest_column.descriptor
                                          .canonical_type_name.size()) &&
                         account_peak(unnest_column.descriptor
                                          .encoded_descriptor.size());
          for (const auto& element : expansion.elements) {
            peak_ok = peak_ok && account_peak(sizeof(std::string)) &&
                      account_peak(element.size()) &&
                      account_peak(sizeof(exec::DescriptorTuple)) &&
                      account_peak(sizeof(api::EngineTypedValue)) &&
                      account_peak(unnest_column.descriptor.descriptor_uuid
                                       .size()) &&
                      account_peak(unnest_column.descriptor.descriptor_kind
                                       .size()) &&
                      account_peak(unnest_column.descriptor
                                       .canonical_type_name.size()) &&
                      account_peak(unnest_column.descriptor
                                       .encoded_descriptor.size()) &&
                      account_peak(element.size()) &&
                      account_peak(sizeof(exec::ModelProviderRowIdentityV1)) &&
                      account_peak(72);
          }
          peak_ok = peak_ok &&
                    account_peak(selected_input.provider_uuid.size()) &&
                    account_peak(selected_input.result_handle_uuid.size()) &&
                    account_peak(security_receipt_uuid.size()) &&
                    account_peak(property_uuid.size()) &&
                    account_peak(
                        std::string_view("SB_MODEL_PROPERTY_DESCRIPTOR_V1")
                            .size()) &&
                    account_peak(std::string_view("fixture_order").size()) &&
                    account_peak(
                        std::string_view("single_local_partition").size()) &&
                    account_peak(std::string_view("document_uuid").size()) &&
                    account_peak(selected_input.output_descriptor_ids.size() *
                                 sizeof(std::uint32_t));
          if (!peak_ok) {
            provider.diagnostic_id = "SB_MODEL_RESOURCE_MEMORY_REFUSED_V1";
            provider.detail =
                "DOCUMENT_UNNEST combined producer memory bound was exceeded";
            return provider;
          }
          auto& exchange = provider.provider_batch;
          exchange.provider_uuid = selected_input.provider_uuid;
          exchange.provider_generation = selected_input.provider_generation;
          exchange.result_handle_uuid = selected_input.result_handle_uuid;
          exchange.causal_counter_id = selected_input.causal_counter_id;
          exchange.output_descriptor_ids =
              selected_input.output_descriptor_ids;
          exchange.mga_statement_context =
              selected_input.mga_statement_context;
          exchange.security_receipt_uuid = security_receipt_uuid;
          exchange.batch.columns.reserve(1);
          exchange.batch.rows.reserve(expansion.elements.size());
          exchange.ordered_row_identities.reserve(expansion.elements.size());
          exchange.batch.columns.push_back(unnest_column);
          for (std::size_t ordinal = 0; ordinal < expansion.elements.size();
               ++ordinal) {
            api::EngineTypedValue value;
            value.descriptor = unnest_column.descriptor;
            value.encoded_value = std::move(expansion.elements[ordinal]);
            value.setState(api::EngineValueState::value);
            exchange.batch.rows.push_back({{std::move(value)}});
            const auto ordinal_scope =
                identity_scope + ":" + std::to_string(ordinal);
            exchange.ordered_row_identities.push_back(
                {DerivedCanonicalUuid(ordinal_scope,
                                      "document-unnest.document"),
                 DerivedCanonicalUuid(ordinal_scope,
                                      "document-unnest.row")});
          }
          exchange.properties.property_uuid = property_uuid;
          exchange.properties.exact = true;
          exchange.properties.residual_recheck_complete = true;
          exchange.properties.base_row_mga_recheck_complete = true;
          exchange.properties.security_recheck_complete = true;
          exchange.residual_recheck_complete = true;
          exchange.base_row_mga_recheck_complete = true;
          exchange.security_recheck_complete = true;
          provider.ok = true;
          return provider;
        };

    exec::CanonicalPhysicalExecutorRegistration registration;
    registration.node_kind = exec::PhysicalNodeKind::kScan;
    registration.implementation_id = "physical_document_path_scan_v1";
    registration.executor_capability_uuid = source_input.capability_uuid;
    registration.executor_capability_abi_version = 1;
    registration.engine_owned = true;
    registration.accepts_optimizer_publication_v2 = true;
    registration.honors_dispatcher_memory_limit_v1 = true;
    registration.execute =
        [execution_request](
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
              selected_node.logical_semantic_variant_id !=
                  "sblr.model_expand.v1" ||
              selected_node.implementation_id !=
                  "physical_document_path_scan_v1" ||
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
                "selected DOCUMENT_UNNEST physical node identity was substituted";
            return step;
          }
          const auto callback_memory_limit = std::min(
              selected_node.memory_bytes_required,
              selected_node.dispatcher_callback_memory_limit_bytes);
          if (callback_memory_limit == 0 ||
              callback_memory_limit > selected_dag.memory_budget_bytes) {
            step.diagnostic.ok = false;
            step.diagnostic.diagnostic_code =
                "SBLR.PLAN_TREE.RESOURCE_LIMIT";
            step.diagnostic.detail =
                "DOCUMENT_UNNEST callback memory allowance is absent or invalid";
            return step;
          }
          auto bounded_request = execution_request;
          bounded_request.input.maximum_memory_bytes = std::min(
              bounded_request.input.maximum_memory_bytes,
              callback_memory_limit);
          const auto executed =
              exec::ExecuteModelFamilySourceV1(bounded_request);
          step.data_access_observed = executed.data_access_observed;
          if (!executed.accepted || !executed.root_published ||
              executed.output.object_uuid != "" ||
              executed.output.operation_id != "DOCUMENT_UNNEST" ||
              executed.output.physical_node_id !=
                  selected_node.physical_node_id ||
              executed.output.selected_alternative_uuid !=
                  selected_node.selected_alternative_uuid ||
              executed.output.capability_uuid !=
                  selected_node.executor_capability_uuid ||
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
                    ? "DOCUMENT_UNNEST execution did not complete"
                    : executed.detail;
            return step;
          }
          step.result_handle_id = selected_node.physical_node_id;
          step.output_row_count = executed.output.batch.rows.size();
          step.rows_examined = executed.rows_examined;
          step.materialized_output_batch = std::move(executed.output.batch);
          return step;
        };

    api::CanonicalOptimizerSelectedExecutionRequest selected;
    selected.selected_physical_dag = physical.physical_dag;
    selected.pre_access_statistics_snapshot_uuid =
        physical.physical_dag.statistics_snapshot_uuid;
    selected.mga_authority = BuildCanonicalExecutionMgaAuthority(
        input.context, physical.physical_dag);
    std::size_t maximum_composition_columns = 1;
    for (const auto& node : dag.nodes) {
      maximum_composition_columns = std::max(
          maximum_composition_columns, node.output_descriptor_ids.size());
    }
    const std::uint64_t maximum_batch_rows =
        prepared_document_set.has_value() ? bounded_rows
                                          : source_input.maximum_rows;
    std::uint64_t maximum_total_rows = source_input.maximum_rows;
    std::uint64_t maximum_batch_cells = 0;
    std::uint64_t maximum_total_cells = 0;
    if (maximum_composition_columns == 0 ||
        (prepared_document_set.has_value() &&
         !CheckedMultiply(bounded_rows, 2, &maximum_total_rows)) ||
        !CheckedMultiply(maximum_batch_rows, maximum_composition_columns,
                         &maximum_batch_cells) ||
        !CheckedMultiply(maximum_total_rows, maximum_composition_columns,
                         &maximum_total_cells) ||
        maximum_batch_rows > std::numeric_limits<std::size_t>::max() ||
        maximum_total_rows > std::numeric_limits<std::size_t>::max() ||
        maximum_batch_cells > std::numeric_limits<std::size_t>::max() ||
        maximum_total_cells > std::numeric_limits<std::size_t>::max()) {
      return refuse("SBLR.PLAN_TREE.RESOURCE_LIMIT",
                    "DOCUMENT_UNNEST composition cell bound overflowed");
    }
    selected.runtime_limits.maximum_rows_per_batch =
        static_cast<std::size_t>(maximum_batch_rows);
    selected.runtime_limits.maximum_columns_per_batch =
        maximum_composition_columns;
    selected.runtime_limits.maximum_cells_per_batch =
        static_cast<std::size_t>(maximum_batch_cells);
    selected.runtime_limits.maximum_total_materialized_rows =
        static_cast<std::size_t>(maximum_total_rows);
    selected.runtime_limits.maximum_total_materialized_cells =
        static_cast<std::size_t>(maximum_total_cells);
    selected.cancellation_requested =
        input.context.query_cancellation_requested
            ? input.context.query_cancellation_requested
            : std::function<bool()>([] { return false; });
    selected.available_executors.push_back(std::move(registration));
    if (prepared_document_set.has_value()) {
      std::unordered_map<std::uint64_t, exec::DescriptorBatch> values_batches;
      values_batches.emplace(
          set_values->node_id,
          std::move(materialized_document_set_values->batch));
      selected.available_executors.push_back(
          MakeLiveValuesRegistration(
              std::move(values_batches), set_values_capability_uuid,
              "QOW-DIAG-RELATIONAL-LIVE-SET-VALUES-V1",
              "DOCUMENT_UNNEST UNION ALL", true));
      std::unordered_map<std::uint64_t, PreparedLiveSetNode>
          prepared_set_nodes;
      prepared_set_nodes.emplace(set_root->node_id,
                                 *prepared_document_set);
      selected.available_executors.push_back(
          MakeLiveSetOperationRegistration(
              MakeLiveSetRegistrationProfilesForComposition(prepared_set_nodes),
              document_set_profile.implementation_id,
              set_root_capability_uuid, input.context));
    }
    if (prepared_filter.has_value()) {
      selected.available_executors.push_back(
          MakeLiveHeapFilterRegistration(
              prepared_filter->predicate_expression_id,
              prepared_filter->predicate_row_binding, dag,
              canonical_planning_request.expression_services,
              filter_capability_uuid, bounded_rows, input.context,
              api::EngineCanonicalExpressionConsumer::filter,
              api::EnginePredicateConsumer::filter, &dag, &input.context,
              &selected.mga_authority));
    }
    if (prepared_project.has_value()) {
      selected.available_executors.push_back(
          MakeLiveProjectRegistration(
              MakeLiveProjectRegistrationProfileForComposition(*prepared_project),
              "project.typed.expression-row.v1",
              project_capability_uuid, bounded_rows, dag,
              canonical_planning_request.expression_services,
              input.context, true, &dag, &input.context,
              &selected.mga_authority));
    } else if (!direct_projected_columns.empty()) {
      selected.available_executors.push_back(
          MakeLiveHeapProjectRegistration(
              direct_projected_columns, project_capability_uuid,
              bounded_rows, input.context, &input.context,
              &selected.mga_authority));
    }
    if (prepared_sort.has_value()) {
      std::uint64_t comparison_bound = 0;
      if (!CheckedMultiply(bounded_rows, bounded_rows, &comparison_bound)) {
        return refuse("SBLR.PLAN_TREE.RESOURCE_LIMIT",
                      "DOCUMENT_UNNEST SORT comparison bound overflowed");
      }
      const auto tie_uuid = DerivedCanonicalUuid(
          identity_scope + ":" + prepared_sort->ordering_property_uuid,
          "document-unnest.sort.deterministic-tie");
      if (prepared_sort->expression_ordering) {
        selected.available_executors.push_back(
            MakeLiveExpressionSortRegistration(
                std::move(*prepared_sort),
                tie_uuid, sort_capability_uuid, bounded_rows,
                std::max<std::size_t>(
                    1, static_cast<std::size_t>(comparison_bound)),
                dag, canonical_planning_request.expression_services,
                input.context, &dag, &input.context,
                &selected.mga_authority));
      } else {
        selected.available_executors.push_back(
            MakeLiveSortRegistration(
                prepared_sort->order_terms, tie_uuid, sort_capability_uuid,
                bounded_rows,
                std::max<std::size_t>(
                    1, static_cast<std::size_t>(comparison_bound)),
                input.context, &input.context, &selected.mga_authority));
      }
    }
    if (prepared_row_number.has_value()) {
      selected.available_executors.push_back(
          MakeLiveRowNumberRegistration(
              *prepared_row_number, window_order_evidence_uuid,
              window_capability_uuid, bounded_rows, input.context,
              &input.context, &selected.mga_authority));
    }
    if (prepared_count_star.has_value()) {
      selected.available_executors.push_back(
          MakeLiveCountStarRegistration(
              prepared_count_star->result_column,
              aggregate_capability_uuid, bounded_rows, input.context,
              &input.context, &selected.mga_authority));
    }
    if (prepared_nonrecursive_cte) {
      selected.available_executors.push_back(
          MakeLiveNonrecursiveCteRegistration(
              nonrecursive_cte_implementation_id, cte_capability_uuid,
              bounded_rows, input.context, &input.context,
              &selected.mga_authority));
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
              limit_offset, fetch_first_rows_only, bounded_rows,
              input.context, &input.context, &selected.mga_authority));
    }
    selected.engine_execution_authorized = true;
    selected.result_publication_request.statement_uuid =
        input.context.statement_uuid;
    selected.result_publication_request.invocation_mode =
        exec::CanonicalResultInvocationMode::kDirect;
    selected.result_publication_request.execution_attempt_uuid =
        DerivedCanonicalUuid(
            identity_scope + ":" + input.context.current_monotonic_ns,
            "document-unnest.execution-attempt");
    selected.result_publication_request.result_kind =
        exec::CanonicalResultKind::kRows;
    selected.result_publication_request.transaction_effect_evidence_uuid =
        DerivedCanonicalUuid(
            identity_scope + ":" +
                std::to_string(input.context.local_transaction_id) + ":" +
                std::to_string(
                    input.context.snapshot_visible_through_local_transaction_id),
            "document-unnest.transaction-effect-unchanged");
    selected.result_publication_request.maximum_row_count =
        prepared_document_set.has_value() ? bounded_rows
                                          : source_input.maximum_rows;
    selected.result_publication_request.column_bindings =
        composition_state.result_bindings;
    const auto execution = ExecuteSelectedCanonicalObjectFreeDag(
        input.context, selected, physical.ordinary_runtime_memory_receipts);
    if (!execution.accepted || !execution.exact_selected_nodes_executed ||
        !execution.causal_counters_attached ||
        !execution.canonical_result_published ||
        !execution.data_access_observed ||
        !execution.runtime_actuals.accepted ||
        execution.dispatch.executed_steps.size() !=
            physical.physical_dag.nodes.size() ||
        execution.dispatch.executed_root_physical_node_id !=
            physical.physical_dag.root_physical_node_id ||
        execution.dispatch.selected_plan_uuid !=
            physical.physical_dag.selected_plan_uuid ||
        !execution.issues.empty()) {
      return refuse(
          execution.issues.empty()
              ? "SB_MODEL_COORDINATOR_LEG_FAILED_V1"
              : execution.issues.front().diagnostic_id ==
                        "SBLR.PLAN_TREE.RESOURCE_LIMIT"
                    ? "SB_MODEL_RESOURCE_MEMORY_REFUSED_V1"
                    : execution.issues.front().diagnostic_id,
          execution.issues.empty()
              ? "DOCUMENT_UNNEST selected execution did not complete"
              : execution.issues.front().field_id);
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
    result.api_result =
        SuccessfulApiResult(canonical_planning_request, execution);
    result.api_result.evidence.push_back(
        {"canonical.model_route",
         "SBSQL_DOCUMENT_UNNEST_TO_SBLR_MODEL_EXPAND_TO_DOCUMENT_PATH_SCAN_TO_TYPED_BATCH_V1"});
    result.api_result.evidence.push_back(
        {"canonical.model_search_family", "document.local.v1"});
    result.api_result.evidence.push_back(
        {"canonical.physical_abi",
         std::to_string(physical.physical_dag.abi_version)});
    result.api_result.evidence.push_back(
        {"canonical.physical_dispatch", "generic.selected-dag.v1"});
    result.api_result.evidence.push_back(
        {"canonical.document_input", "bound_expression_no_storage"});
    result.api_result.evidence.push_back(
        {"canonical.document_exact_collection_fallback", "false"});
    result.api_result.evidence.push_back(
        {"canonical.document_properties",
         "fixture_order|single_local_partition|document_uuid"});
    result.api_result.evidence.push_back(
        {"canonical.document_row_identity_count",
         std::to_string(execution.result_publication.row_stream.rows.size())});
    if (prepared_nonrecursive_cte) {
      result.api_result.evidence.push_back(
          {"canonical.document_cte_implementation",
           nonrecursive_cte_implementation_id});
      result.api_result.evidence.push_back(
          {"canonical.document_cte_auxiliary_memory",
           nonrecursive_cte_implementation_id ==
                   "cte.bound.materialize.typed.v1"
               ? "runtime_input_batch"
               : "none"});
    }
    if (prepared_recursive_cte.has_value()) {
      result.api_result.evidence.push_back(
          {"canonical.document_recursive_cte_implementation",
           prepared_recursive_cte->profile.implementation_id});
      result.api_result.evidence.push_back(
          {"canonical.document_recursive_cte_bound",
           std::to_string(prepared_recursive_cte->term.upper_bound)});
      result.api_result.evidence.push_back(
          {"canonical.document_recursive_cte_work_bound",
           std::to_string(prepared_recursive_cte->rows_examined)});
    }
    if (prepared_document_set.has_value()) {
      result.api_result.evidence.push_back(
          {"canonical.document_set_implementation",
           document_set_profile.implementation_id});
      result.api_result.evidence.push_back(
          {"canonical.document_set_semantics",
           "union-all.ordinal.left-then-right.bag.v1"});
      result.api_result.evidence.push_back(
          {"canonical.document_set_output_bound",
           std::to_string(bounded_rows)});
    }
    if (prepared_row_number.has_value()) {
      result.api_result.evidence.push_back(
          {"canonical.document_window_implementation",
           "window.row-number.v1"});
      result.api_result.evidence.push_back(
          {"canonical.document_window_order_evidence",
           window_order_evidence_uuid});
      result.api_result.evidence.push_back(
          {"canonical.document_window_root", "selected-dag-root.v1"});
    }
    return result;
  }
  const auto loaded_relation =
      api::LoadMgaRelationStorageDescriptor(input.context,
                                            planning.object_uuid);
  if (!loaded_relation.ok) {
    return refuse("SB_MODEL_TYPED_EXCHANGE_INVALID_V1",
                  loaded_relation.diagnostic.detail.empty()
                      ? "current document relation descriptor is unavailable"
                      : loaded_relation.diagnostic.detail);
  }
  const auto& persisted_relation = loaded_relation.descriptor;
  if (persisted_relation.relation_uuid != planning.object_uuid ||
      persisted_relation.database_uuid !=
          input.context.database_uuid ||
      persisted_relation.relation_kind != "table" ||
      persisted_relation.storage_profile != "local_mga_rowstore_v1" ||
      persisted_relation.descriptor_uuid.is_nil() ||
      persisted_relation.descriptor_generation == 0 ||
      (persisted_relation.descriptor_status != "production_descriptor" &&
       persisted_relation.descriptor_status !=
           "metadata_bridge_vetted_descriptor") ||
      persisted_relation.columns.empty()) {
    return refuse("SB_MODEL_TYPED_EXCHANGE_INVALID_V1",
                  "current persisted document relation descriptor is invalid");
  }
  const auto authorization = api::EvaluateMaterializedAuthorization(
      input.context, input.context.authorization_context, "SELECT",
      planning.object_uuid);
  if (!authorization.authorized || authorization.denied ||
      authorization.policy_recheck_required ||
      !authorization.diagnostics.empty()) {
    return refuse("SB_MODEL_SECURITY_ADMISSION_REFUSED_V1",
                  "document SELECT authorization was refused");
  }
  const auto provider_generation = persisted_relation.descriptor_generation;
  std::vector<opt::ModelFamilyCapabilitySnapshotV1> alternatives;
  alternatives.push_back(MakeModelFamilyCapabilitySnapshotForCompositionV1(
      planning, identity_scope + ".document.fallback",
      opt::ModelFamilyAlternativeRouteClassV1::kExactCollectionFallback,
      provider_uuid, capability_uuid, provider_generation, true, 1, 1,
      planning.memory_budget_bytes));
  const auto planned = PlanCanonicalModelFamilySourceForCompositionV1(
      planning, identity_scope + ".document.inventory",
      std::move(alternatives));
  if (!planned.accepted || !planned.selected ||
      !planned.data_access_allowed || !planned.optimizer_owned_enumeration ||
      !planned.exact_fallback_selected ||
      planned.selected_candidate.provider_uuid != provider_uuid ||
      planned.selected_candidate.capability_uuid != capability_uuid ||
      planned.selected_candidate.provider_generation != provider_generation) {
    return refuse(planned.diagnostic_id.empty()
                      ? "SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1"
                      : planned.diagnostic_id,
                  planned.detail.empty()
                      ? "document coordinator did not select the exact candidate"
                      : planned.detail);
  }

  api::CanonicalRelationalPlanningScope planning_scope;
  planning_scope.catalog_epoch_uuid = input.context.catalog_epoch_uuid;
  planning_scope.security_context_uuid =
      input.context.authorization_context.authority_uuid;
  planning_scope.statement_uuid = input.context.statement_uuid;
  planning_scope.owning_transaction_uuid =
      input.context.transaction_uuid;
  planning_scope.statement_snapshot_uuid =
      input.context.statement_snapshot_uuid;
  planning_scope.statement_metadata_snapshot_uuid =
      input.context.statement_metadata_snapshot_uuid;
  planning_scope.local_transaction_id = input.context.local_transaction_id;
  planning_scope.snapshot_visible_through_local_transaction_id =
      input.context.snapshot_visible_through_local_transaction_id;
  planning_scope.metadata_snapshot_engine_owned =
      input.context.statement_metadata_snapshot_engine_owned;
  planning_scope.authorization_context_engine_owned =
      input.context.authorization_context.present;
  auto logical = api::PopulateCanonicalLogicalGraphFromAdmittedTypedRelationalDag(
      dag, planning_scope);
  if (!logical.accepted || !logical.property_catalog.properties.empty()) {
    return refuse(
        logical.issues.empty()
            ? "QOW-DIAG-OPTIMIZER-ADMISSION-BOUND-REQUEST-V1"
            : logical.issues.front().diagnostic_id,
        logical.issues.empty() ? "document logical bridge was refused"
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
    return refuse("QOW-DIAG-QRY-004-DOCUMENT-OPTIMIZER-MGA-SNAPSHOT-V1",
                  "bridge_statement_snapshot_carriage");
  }
  logical.logical_graph.mga_statement_context = current_logical_mga;
  logical.property_catalog.mga_statement_context = current_logical_mga;

  opt::CanonicalNativeObjectAdmissionContext admission_context;
  admission_context.statement_uuid = input.context.statement_uuid;
  admission_context.catalog_snapshot_uuid =
      input.context.statement_metadata_snapshot_uuid;
  admission_context.security_context_uuid =
      input.context.authorization_context.authority_uuid;
  admission_context.catalog_generation = input.context.catalog_generation_id;
  admission_context.authorization_catalog_generation =
      input.context.authorization_context.catalog_generation_id;
  admission_context.security_epoch =
      input.context.authorization_context.security_epoch;
  admission_context.policy_epoch =
      input.context.authorization_context.policy_epoch;
  admission_context.resource_epoch = input.context.resource_epoch;
  admission_context.capability_snapshot_uuid =
      input.context.optimizer_capability_snapshot_uuid;
  admission_context.resource_snapshot_uuid =
      input.context.optimizer_resource_snapshot_uuid;
  admission_context.route_snapshot_uuid =
      input.context.optimizer_route_snapshot_uuid;
  admission_context.route_epoch = input.context.optimizer_route_epoch;
  admission_context.route_generation =
      input.context.optimizer_route_generation;
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
    return refuse("QOW-DIAG-QRY-004-DOCUMENT-OPTIMIZER-CONTEXT-V1",
                  "current_monotonic_ns");
  }
  admission_context.admitted_at_monotonic_ns = admitted_at_monotonic_ns;
  admission_context.metadata_snapshot_engine_owned = true;
  admission_context.authorization_context_engine_owned = true;
  admission_context.catalog_object_uuids = {planning.object_uuid};
  admission_context.authorized_object_uuids = {planning.object_uuid};
  admission_context.catalog_object_evidence_engine_owned = true;
  admission_context.authorization_object_evidence_engine_owned = true;
  auto canonical_admission =
      opt::BuildCanonicalObjectAwareNativeOptimizerAdmissionRequest(
          logical.logical_graph, logical.property_catalog,
          admission_context);
  if (!canonical_admission.built ||
      !canonical_admission.admission.admitted ||
      !canonical_admission.admission.planning_allowed ||
      canonical_admission.admission.data_access_allowed ||
      canonical_admission.admission.evidence.size() != 8) {
    return refuse(
        canonical_admission.diagnostic_id.empty()
            ? "QOW-DIAG-OPTIMIZER-ADMISSION-BOUND-REQUEST-V1"
            : canonical_admission.diagnostic_id,
        canonical_admission.field_id.empty()
            ? "document canonical optimizer admission was refused"
            : canonical_admission.field_id);
  }
  result.optimizer_admitted = true;
  CanonicalObjectFreeValuesExecutionRequest canonical_planning_request{
      input.context, dag, canonical_admission.request,
      canonical_admission.admission};
  std::vector<LivePhysicalNodeProfile> document_profiles;
  LivePhysicalNodeProfile document_profile;
  document_profile.logical_node_id = scan->node_id;
  document_profile.implementation_id = "physical_document_path_scan_v1";
  document_profile.capability_uuid =
      planned.selected_candidate.capability_uuid;
  document_profile.logical_node_kind =
      plan::CanonicalLogicalRelationalNodeKind::kRelationSource;
  document_profile.physical_node_kind = exec::PhysicalNodeKind::kScan;
  document_profile.transformation_rule_id =
      "canonical.document.path-scan.v1";
  document_profile.estimated_rows = 1;
  document_profile.memory_bytes_required = std::max<std::uint64_t>(
      1, planned.selected_candidate.cost.memory_bytes_required);
  document_profile.page_read_sequential_units = 1;
  document_profile.mga_visibility_checks_expected = 1;
  document_profile.storage_read_capable = true;
  document_profile.mga_visibility_capable = true;
  document_profile.spill_supported = false;
  document_profile.parallel_safe = false;
  document_profile.parallel_required = false;
  document_profile.residual_predicate_required = true;
  document_profile.storage_recheck_required = true;
  document_profile.compatibility_profile_id = "document.local.v1";
  document_profiles.push_back(std::move(document_profile));
  if (!CompleteLiveRuntimeMemoryReceipts(&document_profiles)) {
    return refuse("QOW-DIAG-OPT-017-REFUSAL-V1",
                  "document producer memory receipt is incomplete");
  }
  const auto physical = PlanAndPublishLivePhysicalDag(
      canonical_planning_request, document_profiles,
      "document.selected-plan", "document model source",
      "document.local.v1");
  if (!physical.ok || physical.physical_dag.nodes.size() != 1 ||
      physical.physical_dag.root_physical_node_id !=
          physical.physical_dag.nodes.front().physical_node_id ||
      physical.physical_dag.nodes.front().implementation_id !=
          "physical_document_path_scan_v1" ||
      physical.physical_dag.nodes.front().executor_capability_uuid !=
          planned.selected_candidate.capability_uuid ||
      physical.physical_dag.nodes.front().selected_alternative_uuid !=
          physical_alternative_uuid ||
      physical.physical_dag.nodes.front().cost_vector_uuid !=
          physical_cost_uuid) {
    return refuse(
        physical.diagnostic_id.empty()
            ? "QOW-DIAG-OPTIMIZER-PHYSICAL-PUBLICATION-V1"
            : physical.diagnostic_id,
        physical.detail.empty()
            ? "document canonical physical DAG was not published"
            : physical.detail);
  }
  result.optimizer_selected = true;
  result.physical_dag_published = true;
  result.optimizer_admission_stage_count =
      canonical_admission.admission.evidence.size();
  result.physical_node_count = physical.physical_dag.nodes.size();
  result.selected_plan_uuid = physical.physical_dag.selected_plan_uuid;
  const auto descriptor_for = [&](const std::uint32_t descriptor_id) {
    return std::ranges::find_if(dag.descriptors, [&](const auto& descriptor) {
      return descriptor.descriptor_id == descriptor_id;
    });
  };
  const auto expression_for = [&](const std::uint32_t expression_id) {
    return std::ranges::find_if(dag.expressions, [&](const auto& expression) {
      return expression.expression_id == expression_id;
    });
  };
  const auto unquote = [](std::string value) {
    if (value.size() >= 2 &&
        ((value.front() == '\'' && value.back() == '\'') ||
         (value.front() == '"' && value.back() == '"'))) {
      value = value.substr(1, value.size() - 2);
    }
    return value;
  };

  struct DocumentProjectionDependency {
    std::string column_uuid;
    std::string path;
    std::uint32_t descriptor_id{0};
    api::EngineDescriptor descriptor;
    bool nullable{false};
  };
  struct DocumentProjectionOutput {
    bool direct_document_path{false};
    bool direct_identifier{false};
    std::size_t direct_dependency_ordinal{0};
    LiveProjectExpressionRegistration expression;
    exec::ExecutorColumnDescriptor column;
  };
  struct EncodedFieldLookup {
    bool valid{true};
    std::optional<std::string> value;
  };
  const auto descriptor_field = [](const std::string& encoded,
                                   const std::string_view name) {
    EncodedFieldLookup result;
    std::size_t offset = 0;
    while (offset <= encoded.size()) {
      const auto end = encoded.find(';', offset);
      const auto field = std::string_view(encoded).substr(
          offset, end == std::string::npos ? std::string::npos
                                            : end - offset);
      const auto equal = field.find('=');
      if (equal == std::string_view::npos || equal == 0 ||
          equal + 1 == field.size()) {
        result.valid = false;
        return result;
      }
      if (field.substr(0, equal) == name) {
        if (result.value.has_value()) {
          result.valid = false;
          return result;
        }
        result.value = std::string(field.substr(equal + 1));
      }
      if (end == std::string::npos) break;
      offset = end + 1;
    }
    return result;
  };
  const auto build_descriptor = [](const api::RelationalTypeDescriptor& source,
                                   const std::string& type_name) {
    api::EngineDescriptor descriptor;
    descriptor.descriptor_uuid = source.descriptor_uuid;
    descriptor.type_uuid = source.type_uuid;
    descriptor.descriptor_kind = "scalar";
    descriptor.canonical_type_name = type_name;
    descriptor.encoded_descriptor =
        std::string("nullability=") +
        (source.nullability == api::RelationalNullability::kNullable
             ? "nullable"
             : "non_null");
    descriptor.collation_uuid = source.collation_uuid.value_or(api::EngineUuid{});
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
  const auto exact_optional_field = [&](const std::string& encoded,
                                        const std::string_view name,
                                        const auto& expected) {
    const auto field = descriptor_field(encoded, name);
    return field.valid && field.value == expected;
  };
  const auto exact_persisted_binding = [&](const auto& column,
                                           const auto& descriptor) {
    const auto type_uuid = descriptor_field(
        column.value_descriptor.encoded_descriptor, "type_uuid");
    const auto canonical_nullability = descriptor_field(
        column.value_descriptor.encoded_descriptor, "nullability");
    const auto storage_nullability = descriptor_field(
        column.value_descriptor.encoded_descriptor, "nullable");
    std::optional<std::string> expected_width;
    if (descriptor.width.has_value()) {
      expected_width = std::to_string(*descriptor.width);
    }
    auto persisted_width = descriptor_field(
        column.value_descriptor.encoded_descriptor, "width");
    if (persisted_width.valid && !persisted_width.value.has_value() &&
        column.character_length != 0) {
      persisted_width.value = std::to_string(column.character_length);
    }
    std::optional<std::string> expected_precision;
    if (descriptor.precision.has_value()) {
      expected_precision = std::to_string(*descriptor.precision);
    }
    std::optional<std::string> expected_scale;
    if (descriptor.scale.has_value()) {
      expected_scale = std::to_string(*descriptor.scale);
    }
    const bool nullable =
        descriptor.nullability == api::RelationalNullability::kNullable;
    const auto nullability_matches = [&] {
      std::optional<bool> persisted;
      if (!canonical_nullability.valid || !storage_nullability.valid) {
        return false;
      }
      if (canonical_nullability.value.has_value()) {
        if (*canonical_nullability.value == "nullable") {
          persisted = true;
        } else if (*canonical_nullability.value == "non_null") {
          persisted = false;
        } else {
          return false;
        }
      }
      if (storage_nullability.value.has_value()) {
        std::optional<bool> storage;
        if (*storage_nullability.value == "true") {
          storage = true;
        } else if (*storage_nullability.value == "false") {
          storage = false;
        } else {
          return false;
        }
        if (persisted.has_value() && persisted != storage) return false;
        persisted = storage;
      }
      return persisted.has_value() && *persisted == nullable;
    }();
    return column.value_descriptor.descriptor_uuid ==
               descriptor.descriptor_uuid &&
           !column.value_descriptor.canonical_type_name.empty() &&
           type_uuid.valid && type_uuid.value == descriptor.type_uuid &&
           nullability_matches &&
           column.nullable == nullable && persisted_width.valid &&
           persisted_width.value == expected_width &&
           exact_optional_field(column.value_descriptor.encoded_descriptor,
                                "precision", expected_precision) &&
           exact_optional_field(column.value_descriptor.encoded_descriptor,
                                "scale", expected_scale) &&
           exact_optional_field(column.value_descriptor.encoded_descriptor,
                                "collation_uuid",
                                descriptor.collation_uuid) &&
           exact_optional_field(column.value_descriptor.encoded_descriptor,
                                "timezone_profile_id",
                                descriptor.timezone_profile_id) &&
           (descriptor.collation_uuid.has_value()
                ? column.collation_uuid == *descriptor.collation_uuid
                : column.collation_uuid.empty());
  };

  std::unordered_map<std::string, std::string> type_names_by_uuid;
  for (const auto& column : persisted_relation.columns) {
    const auto type_uuid = descriptor_field(
        column.value_descriptor.encoded_descriptor, "type_uuid");
    const auto existing_type =
        type_uuid.value.has_value()
            ? type_names_by_uuid.find(*type_uuid.value)
            : type_names_by_uuid.end();
    if (!type_uuid.valid || !type_uuid.value.has_value() ||
        column.column_uuid.is_nil() ||
        column.canonical_name_key.empty() ||
        column.value_descriptor.canonical_type_name.empty() ||
        (existing_type != type_names_by_uuid.end() &&
         existing_type->second !=
             column.value_descriptor.canonical_type_name)) {
      return refuse("SB_MODEL_TYPED_EXCHANGE_INVALID_V1",
                    "persisted document column descriptor is ambiguous");
    }
    type_names_by_uuid.emplace(*type_uuid.value,
                               column.value_descriptor.canonical_type_name);
  }
  const auto core_manifest = dt::LoadCurrentCoreDatatypeCatalogManifest();
  if (!core_manifest.ok()) {
    return refuse("SB_MODEL_TYPED_EXCHANGE_INVALID_V1",
                  "current core datatype descriptor manifest is unavailable");
  }
  for (const auto& row : core_manifest.manifest.descriptor_rows) {
    const auto descriptor_uuid =
        scratchbird::core::uuid::UuidToString(row.descriptor_uuid.value);
    type_names_by_uuid.emplace(descriptor_uuid, row.stable_name);
    const auto identity = dt::LookupDatatypeTypeCodecIdentityV1(
        "019d0000-0000-7000-8000-00000000d701",
        core_manifest.manifest.catalog_epoch, 1, descriptor_uuid,
        row.descriptor_epoch);
    if (identity.ok) {
      type_names_by_uuid.emplace(identity.row.type_uuid, row.stable_name);
    }
  }
  CanonicalRelationalExpressionRuntimeServices expression_services;
  expression_services.descriptor_type_resolver =
      [type_names_by_uuid](const std::string_view type_uuid,
                           std::string* canonical_type_name,
                           std::string* diagnostic_id,
                           std::string* refusal_detail) {
        const auto found = type_names_by_uuid.find(std::string(type_uuid));
        if (found == type_names_by_uuid.end()) {
          if (diagnostic_id != nullptr) {
            *diagnostic_id = "SB_MODEL_TYPED_EXCHANGE_INVALID_V1";
          }
          if (refusal_detail != nullptr) {
            *refusal_detail =
                "document expression type UUID has no current descriptor";
          }
          return false;
        }
        if (canonical_type_name != nullptr) {
          *canonical_type_name = found->second;
        }
        return true;
      };

  std::vector<DocumentProjectionDependency> dependencies;
  std::unordered_map<std::uint32_t, std::size_t> dependency_by_descriptor;
  std::string projection_detail;
  const auto append_identifier_dependency = [&](const auto& expression) {
    if (expression.expression_kind !=
            api::RelationalExpressionKind::kIdentifier ||
        !expression.child_expression_ids.empty() ||
        !expression.bound_name_uuid.has_value() ||
        expression.function_uuid.has_value() ||
        expression.literal_kind.has_value() ||
        expression.operator_name.has_value() ||
        expression.literal_or_parameter_ref.has_value()) {
      projection_detail =
          "document projection identifier binding is malformed";
      return false;
    }
    const auto column = std::ranges::find_if(
        persisted_relation.columns, [&](const auto& candidate) {
          return candidate.column_uuid ==
                 *expression.bound_name_uuid;
        });
    const auto descriptor = descriptor_for(expression.result_descriptor_id);
    if (column == persisted_relation.columns.end() ||
        descriptor == dag.descriptors.end() ||
        !exact_persisted_binding(*column, *descriptor)) {
      projection_detail =
          "document projection column UUID or exact descriptor was substituted";
      return false;
    }
    const auto existing =
        dependency_by_descriptor.find(expression.result_descriptor_id);
    if (existing != dependency_by_descriptor.end()) {
      const auto& dependency = dependencies[existing->second];
      if (dependency.column_uuid != column->column_uuid ||
          dependency.path != column->canonical_name_key) {
        projection_detail =
            "document projection descriptor identity is ambiguous";
        return false;
      }
      return true;
    }
    const auto runtime_descriptor = build_descriptor(
        *descriptor, column->value_descriptor.canonical_type_name);
    if (!api::QowCanonicalDescriptorIdentityV1(runtime_descriptor)) {
      projection_detail =
          "document projection runtime descriptor is invalid";
      return false;
    }
    dependency_by_descriptor.emplace(expression.result_descriptor_id,
                                     dependencies.size());
    dependencies.push_back(
        {column->column_uuid, column->canonical_name_key,
         expression.result_descriptor_id, runtime_descriptor,
         column->nullable});
    return true;
  };

  std::vector<DocumentProjectionOutput> projection_outputs(outputs.size());
  for (std::size_t ordinal = 0; ordinal < outputs.size(); ++ordinal) {
    const auto& output = *outputs[ordinal];
    const auto descriptor = descriptor_for(output.descriptor_id);
    const auto expression = expression_for(output.expression_id);
    if (!output.visible || output.ordinal != ordinal || output.output_id == 0 ||
        output.descriptor_id != scan->output_descriptor_ids[ordinal] ||
        descriptor == dag.descriptors.end() ||
        expression == dag.expressions.end() ||
        expression->result_descriptor_id != output.descriptor_id ||
        std::ranges::find(scan->bound_expression_ids,
                          output.expression_id) ==
            scan->bound_expression_ids.end()) {
      return refuse("SB_MODEL_TYPED_EXCHANGE_INVALID_V1",
                    "document output expression binding is incomplete");
    }

    auto& prepared = projection_outputs[ordinal];
    if (expression->expression_kind ==
            api::RelationalExpressionKind::kFunctionCall &&
        expression->operator_name == "DOCUMENT_PATH") {
      if (expression->function_uuid.has_value() ||
          expression->bound_name_uuid.has_value() ||
          expression->literal_kind.has_value() ||
          expression->literal_or_parameter_ref.has_value() ||
          expression->child_expression_ids.size() != 2) {
        return refuse("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                      "document path projection binding is malformed");
      }
      const auto source =
          expression_for(expression->child_expression_ids.front());
      const auto path_expression =
          expression_for(expression->child_expression_ids.back());
      if (source == dag.expressions.end() ||
          source->expression_kind !=
              api::RelationalExpressionKind::kIdentifier ||
          source->bound_name_uuid != planning.object_uuid ||
          path_expression == dag.expressions.end() ||
          path_expression->expression_kind !=
              api::RelationalExpressionKind::kLiteral ||
          path_expression->literal_kind !=
              api::RelationalLiteralKind::kString ||
          !path_expression->literal_or_parameter_ref.has_value()) {
        return refuse("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                      "document path projection operands are unresolved");
      }
      const auto type_name = type_names_by_uuid.find(descriptor->type_uuid);
      if (type_name == type_names_by_uuid.end()) {
        return refuse("SB_MODEL_TYPED_EXCHANGE_INVALID_V1",
                      "document path output type UUID is unresolved");
      }
      const auto runtime_descriptor =
          build_descriptor(*descriptor, type_name->second);
      if (!api::QowCanonicalDescriptorIdentityV1(runtime_descriptor)) {
        return refuse("SB_MODEL_TYPED_EXCHANGE_INVALID_V1",
                      "document path output descriptor is ambiguous");
      }
      const auto path = unquote(*path_expression->literal_or_parameter_ref);
      const auto matching_column_count = std::ranges::count_if(
          persisted_relation.columns, [&](const auto& column) {
            return column.canonical_name_key == path;
          });
      const auto persisted_column = std::ranges::find_if(
          persisted_relation.columns, [&](const auto& column) {
            return column.canonical_name_key == path;
          });
      if (matching_column_count > 1 ||
          (matching_column_count == 1 &&
           !exact_persisted_binding(*persisted_column, *descriptor)) ||
          (matching_column_count == 0 &&
           descriptor->nullability !=
               api::RelationalNullability::kNullable)) {
        return refuse(
            "SB_MODEL_RESULT_DESCRIPTOR_SOURCE_BINDING_INVALID_V1",
            "document path is not an exact persisted or nullable-missing binding");
      }
      prepared.direct_document_path = true;
      const auto existing_dependency =
          dependency_by_descriptor.find(descriptor->descriptor_id);
      if (existing_dependency != dependency_by_descriptor.end()) {
        const auto& existing = dependencies[existing_dependency->second];
        if (existing.path != path ||
            existing.column_uuid !=
                (persisted_column == persisted_relation.columns.end()
                     ? std::string{}
                     : persisted_column->column_uuid) ||
            existing.descriptor.descriptor_uuid !=
                runtime_descriptor.descriptor_uuid ||
            existing.descriptor.encoded_descriptor !=
                runtime_descriptor.encoded_descriptor) {
          return refuse("SB_MODEL_TYPED_EXCHANGE_INVALID_V1",
                        "document path descriptor identity is ambiguous");
        }
        prepared.direct_dependency_ordinal = existing_dependency->second;
      } else {
        prepared.direct_dependency_ordinal = dependencies.size();
        dependency_by_descriptor.emplace(descriptor->descriptor_id,
                                         dependencies.size());
        dependencies.push_back(
            {persisted_column == persisted_relation.columns.end()
                 ? std::string{}
                 : persisted_column->column_uuid,
             path, descriptor->descriptor_id, runtime_descriptor,
             descriptor->nullability ==
                 api::RelationalNullability::kNullable});
      }
      prepared.column =
          {output.output_name_utf8, runtime_descriptor,
           descriptor->nullability ==
               api::RelationalNullability::kNullable,
           descriptor->descriptor_id};
      continue;
    }

    std::unordered_set<std::uint32_t> reachable;
    std::vector<std::uint32_t> pending{expression->expression_id};
    while (!pending.empty()) {
      const auto expression_id = pending.back();
      pending.pop_back();
      if (!reachable.insert(expression_id).second) continue;
      const auto reachable_expression = expression_for(expression_id);
      if (reachable_expression == dag.expressions.end()) {
        projection_detail = "document projection has a dangling expression";
        break;
      }
      if (reachable_expression->expression_kind ==
          api::RelationalExpressionKind::kIdentifier) {
        if (!append_identifier_dependency(*reachable_expression)) break;
        continue;
      }
      if (reachable_expression->expression_kind ==
          api::RelationalExpressionKind::kFunctionCall) {
        projection_detail =
            "document projection function lacks a canonical callable UUID";
        break;
      }
      pending.insert(pending.end(),
                     reachable_expression->child_expression_ids.begin(),
                     reachable_expression->child_expression_ids.end());
    }
    if (!projection_detail.empty()) {
      return refuse("SB_MODEL_TYPED_EXCHANGE_INVALID_V1",
                    std::move(projection_detail));
    }
    prepared.expression.expression_id = expression->expression_id;
  }
  if (dependencies.empty()) {
    return refuse("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                  "document projection has no persisted row dependency");
  }

  std::vector<std::uint32_t> dependency_descriptor_ids;
  std::vector<api::EngineTypedValue> descriptor_only_row;
  dependency_descriptor_ids.reserve(dependencies.size());
  descriptor_only_row.reserve(dependencies.size());
  for (const auto& dependency : dependencies) {
    dependency_descriptor_ids.push_back(dependency.descriptor_id);
    api::EngineTypedValue value;
    value.descriptor = dependency.descriptor;
    value.setState(api::EngineValueState::value);
    descriptor_only_row.push_back(std::move(value));
  }
  CanonicalRelationalExpressionRuntime projection_runtime(
      dag, expression_services);
  for (std::size_t ordinal = 0; ordinal < outputs.size(); ++ordinal) {
    auto& prepared = projection_outputs[ordinal];
    if (prepared.direct_document_path) continue;
    const auto descriptor = descriptor_for(outputs[ordinal]->descriptor_id);
    if (!PrepareInputRowBindingForComposition(
            dag, prepared.expression.expression_id,
            dependency_descriptor_ids, &prepared.expression.row_binding,
            &projection_detail) ||
        !projection_runtime.InferTypeForConsumer(
            prepared.expression.expression_id,
            prepared.expression.row_binding, descriptor_only_row,
            api::EngineCanonicalExpressionConsumer::projection,
            &prepared.expression.expected_type, &projection_detail)) {
      return refuse("SB_MODEL_TYPED_EXCHANGE_INVALID_V1",
                    projection_detail.empty()
                        ? "document projection type is unresolved"
                        : std::move(projection_detail));
    }
    auto runtime_descriptor =
        build_descriptor(*descriptor, prepared.expression.expected_type);
    if (!api::QowCanonicalDescriptorIdentityV1(runtime_descriptor)) {
      return refuse("SB_MODEL_TYPED_EXCHANGE_INVALID_V1",
                    "document output descriptor identity is invalid");
    }
    const auto root_expression =
        expression_for(prepared.expression.expression_id);
    if (root_expression->expression_kind ==
        api::RelationalExpressionKind::kIdentifier) {
      const auto dependency = dependency_by_descriptor.find(
          root_expression->result_descriptor_id);
      if (dependency == dependency_by_descriptor.end() ||
          outputs[ordinal]->output_name_utf8 !=
              dependencies[dependency->second].path) {
        return refuse("SB_MODEL_TYPED_EXCHANGE_INVALID_V1",
                      "document identifier output label is not its persisted column key");
      }
      prepared.direct_identifier = true;
      prepared.direct_dependency_ordinal = dependency->second;
    }
    prepared.column =
        {outputs[ordinal]->output_name_utf8, std::move(runtime_descriptor),
         descriptor->nullability == api::RelationalNullability::kNullable,
         descriptor->descriptor_id};
  }

  api::EngineDocumentFindRequest document_request;
  document_request.context = input.context;
  const auto document_cancellation_probe_failed =
      std::make_shared<std::atomic_bool>(false);
  const auto raw_document_cancellation =
      input.context.query_cancellation_requested;
  const std::function<bool()> document_cancellation_requested =
      [raw_document_cancellation,
       document_cancellation_probe_failed]() noexcept {
        if (!raw_document_cancellation) return false;
        try {
          return raw_document_cancellation();
        } catch (...) {
          document_cancellation_probe_failed->store(
              true, std::memory_order_relaxed);
          // The provider reads the companion flag and reports coordinator
          // failure.  If the first exception occurs at a later coordinator
          // checkpoint, returning true still prevents result publication.
          return true;
        }
      };
  document_request.context.query_cancellation_requested =
      document_cancellation_requested;
  document_request.cancellation_probe_failed =
      [document_cancellation_probe_failed]() noexcept {
        return document_cancellation_probe_failed->load(
            std::memory_order_relaxed);
      };
  document_request.target_object.uuid = planning.object_uuid;
  document_request.expected_descriptor_uuid =
      persisted_relation.descriptor_uuid;
  document_request.expected_descriptor_generation =
      persisted_relation.descriptor_generation;
  document_request.exact_collection_fallback = true;
  document_request.typed_rows_only = true;
  const auto document_decode_memory = planning.memory_budget_bytes / 2;
  const auto document_result_memory =
      planning.memory_budget_bytes - document_decode_memory;
  const auto bounded_document_memory = static_cast<std::size_t>(
      std::min<std::uint64_t>(document_result_memory,
                              std::numeric_limits<std::size_t>::max()));
  if (document_decode_memory == 0 ||
      bounded_document_memory < sizeof(api::EngineDocumentTypedPathValue) ||
      input.context.optimizer_maximum_search_steps == 0) {
    return refuse("SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                  "document provider memory budget is too small");
  }
  document_request.maximum_memory_bytes = document_result_memory;
  document_request.maximum_scanned_row_versions =
      input.context.optimizer_maximum_search_steps;
  document_request.maximum_decoded_bytes = document_decode_memory;
  document_request.maximum_cells =
      bounded_document_memory / sizeof(api::EngineDocumentTypedPathValue);
  document_request.maximum_rows = std::min<std::size_t>(
      65536,
      document_request.maximum_cells /
          std::max<std::size_t>(1, dependencies.size()));
  if (document_request.maximum_rows == 0) {
    return refuse("SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                  "document provider row bound is zero");
  }
  for (const auto& dependency : dependencies) {
    document_request.projected_paths.push_back(dependency.path);
    document_request.projected_column_uuids.push_back(
        dependency.column_uuid);
    document_request.projected_path_nullable.push_back(dependency.nullable);
    document_request.descriptors.push_back(dependency.descriptor);
  }
  std::uint64_t provider_dependency_cell_bound = 0;
  std::uint64_t output_cell_bound = 0;
  if (!CheckedMultiply(
          static_cast<std::uint64_t>(document_request.maximum_rows),
          static_cast<std::uint64_t>(dependencies.size()),
          &provider_dependency_cell_bound) ||
      !CheckedMultiply(
          static_cast<std::uint64_t>(document_request.maximum_rows),
          static_cast<std::uint64_t>(outputs.size()), &output_cell_bound) ||
      output_cell_bound == 0 ||
      output_cell_bound > std::numeric_limits<std::size_t>::max() ||
      provider_dependency_cell_bound == 0 ||
      provider_dependency_cell_bound >
          std::numeric_limits<std::size_t>::max()) {
    return refuse("SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                  "document provider cell bound overflowed");
  }

  const api::RelationalExpressionRecord* predicate = nullptr;
  std::size_t document_predicate_count = 0;
  bool unsupported_document_predicate_shape = false;
  const auto expression_contains_document_path =
      [&](const std::uint32_t root_expression_id) {
        std::unordered_set<std::uint32_t> visited;
        std::vector<std::uint32_t> pending{root_expression_id};
        while (!pending.empty()) {
          const auto expression_id = pending.back();
          pending.pop_back();
          if (!visited.insert(expression_id).second) continue;
          const auto current = expression_for(expression_id);
          if (current == dag.expressions.end()) return true;
          if (current->expression_kind ==
                  api::RelationalExpressionKind::kFunctionCall &&
              current->operator_name == "DOCUMENT_PATH") {
            return true;
          }
          pending.insert(pending.end(), current->child_expression_ids.begin(),
                         current->child_expression_ids.end());
        }
        return false;
      };
  for (const auto& expression : dag.expressions) {
    if (expression.expression_kind == api::RelationalExpressionKind::kBinary &&
        expression.child_expression_ids.size() == 2) {
      const auto left = expression_for(expression.child_expression_ids[0]);
      const auto right = expression_for(expression.child_expression_ids[1]);
      if (left != dag.expressions.end() &&
          left->operator_name == "DOCUMENT_PATH") {
        ++document_predicate_count;
        if (predicate == nullptr) predicate = &expression;
      } else if ((right != dag.expressions.end() &&
                  right->operator_name == "DOCUMENT_PATH") ||
                 ((expression.operator_name == "AND" ||
                   expression.operator_name == "OR") &&
                  (expression_contains_document_path(
                       expression.child_expression_ids[0]) ||
                   expression_contains_document_path(
                       expression.child_expression_ids[1])))) {
        unsupported_document_predicate_shape = true;
      }
    }
  }
  if (document_predicate_count > 1 ||
      unsupported_document_predicate_shape) {
    return refuse("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                  "document predicate composition is not exact");
  }
  if (predicate != nullptr) {
    const auto path_call = expression_for(predicate->child_expression_ids[0]);
    const auto path_expression =
        path_call == dag.expressions.end() ||
                path_call->child_expression_ids.size() != 2
            ? dag.expressions.end()
            : expression_for(path_call->child_expression_ids[1]);
    if (path_expression == dag.expressions.end() ||
        !path_expression->literal_or_parameter_ref.has_value()) {
      return refuse("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                    "document predicate path is unresolved");
    }
    document_request.path =
        unquote(*path_expression->literal_or_parameter_ref);
    if (!predicate->operator_name.has_value()) {
      return refuse("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                    "document predicate operator is unresolved");
    }
    document_request.comparison_operator = *predicate->operator_name;
    const auto rhs = expression_for(predicate->child_expression_ids[1]);
    if (rhs == dag.expressions.end()) {
      return refuse("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                    "document predicate value is unresolved");
    }
    std::string expected_type;
    CanonicalRelationalExpressionRuntime runtime(dag,
                                                  expression_services);
    std::string detail;
    if (!runtime.InferType(rhs->expression_id, std::nullopt,
                           &expected_type, &detail) ||
        expected_type.empty() || expected_type == "null" ||
        !runtime.EvaluateForConsumer(
            rhs->expression_id, expected_type,
            api::EngineCanonicalExpressionConsumer::filter,
            &document_request.comparison_value, &detail)) {
      return refuse("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                    detail.empty() ? "document predicate value evaluation failed"
                                   : detail);
    }
    document_request.comparison_value_present = true;
  }

  exec::ModelSourceInputDescriptorV1 source_input;
  source_input.family_id = "document";
  source_input.operation_id = predicate == nullptr ? "DOCUMENT_FIND"
                                                    : "DOCUMENT_PATH";
  source_input.object_uuid = planning.object_uuid;
  source_input.physical_node_id =
      physical.physical_dag.nodes.front().physical_node_id;
  source_input.selected_alternative_uuid =
      physical.physical_dag.nodes.front().selected_alternative_uuid;
  source_input.capability_uuid =
      physical.physical_dag.nodes.front().executor_capability_uuid;
  source_input.provider_uuid = provider_uuid;
  source_input.provider_generation = provider_generation;
  source_input.result_handle_uuid = result_handle_uuid;
  source_input.causal_counter_id =
      physical.physical_dag.nodes.front().causal_counter_id;
  source_input.output_descriptor_ids = scan->output_descriptor_ids;
  source_input.mga_statement_context = mga;
  source_input.catalog_epoch_uuid = input.context.catalog_epoch_uuid;
  source_input.security_context_uuid =
      input.context.authorization_context.authority_uuid;
  source_input.policy_snapshot_uuid = policy_snapshot_uuid;
  source_input.resource_contract_uuid = resource_contract_uuid;
  source_input.catalog_generation = generation;
  source_input.descriptor_generation =
      persisted_relation.descriptor_generation;
  source_input.security_generation = planning.security_epoch;
  source_input.policy_generation = planning.policy_epoch;
  source_input.resource_generation = planning.resource_epoch;
  source_input.maximum_rows = document_request.maximum_rows;
  source_input.maximum_cells = static_cast<std::size_t>(output_cell_bound);
  source_input.maximum_memory_bytes = planning.memory_budget_bytes;

  exec::ModelFamilyExecutionRequestV1 execution_request;
  execution_request.input = source_input;
  execution_request.capability.capability_uuid =
      physical.physical_dag.nodes.front().executor_capability_uuid;
  execution_request.capability.family_id = "document";
  execution_request.capability.provider_uuid = provider_uuid;
  execution_request.capability.provider_generation = provider_generation;
  execution_request.capability.available = true;
  execution_request.capability.exact = true;
  execution_request.capability.exact_collection_fallback_available = true;
  execution_request.capability.cancellation_supported = true;
  execution_request.capability.cleanup_supported = true;
  execution_request.capability.residual_recheck_supported = true;
  execution_request.capability.base_row_mga_recheck_supported = true;
  execution_request.capability.security_recheck_supported = true;
  execution_request.cancellation_requested =
      document_cancellation_requested;
  execution_request.cleanup_provider = [] {};
  execution_request.exact_fallback_selected = true;
  execution_request.security_admitted = planning.security_admitted;
  execution_request.current_catalog_generation = generation;
  execution_request.current_descriptor_generation =
      persisted_relation.descriptor_generation;
  execution_request.current_security_generation = planning.security_epoch;
  execution_request.current_policy_generation = planning.policy_epoch;
  execution_request.current_resource_generation = planning.resource_epoch;
  execution_request.current_provider_generation = provider_generation;
  execution_request.current_mga_statement_context = mga;
  execution_request.execute_provider =
      [document_request, property_uuid, security_receipt_uuid,
       source_input, relational_dag = dag, dependencies,
       projection_outputs, expression_services,
       persisted_descriptor_uuid =
           persisted_relation.descriptor_uuid,
       persisted_descriptor_generation =
           persisted_relation.descriptor_generation,
       provider_dependency_cell_bound](
          const exec::ModelSourceInputDescriptorV1& selected_input) mutable {
        exec::ModelProviderExecutionResultV1 provider;
        const auto current_relation = api::LoadMgaRelationStorageDescriptor(
            document_request.context, source_input.object_uuid);
        if (!current_relation.ok ||
            current_relation.descriptor.relation_uuid !=
                source_input.object_uuid ||
            current_relation.descriptor.descriptor_uuid !=
                persisted_descriptor_uuid ||
            current_relation.descriptor.descriptor_generation !=
                persisted_descriptor_generation ||
            current_relation.descriptor.descriptor_generation !=
                source_input.provider_generation) {
          provider.diagnostic_id = "SB_MODEL_TYPED_EXCHANGE_INVALID_V1";
          provider.detail =
              "document relation descriptor changed before provider execution";
          return provider;
        }
        const auto current_authorization =
            api::EvaluateMaterializedAuthorization(
                document_request.context,
                document_request.context.authorization_context, "SELECT",
                source_input.object_uuid);
        if (!current_authorization.authorized ||
            current_authorization.denied ||
            current_authorization.policy_recheck_required ||
            !current_authorization.diagnostics.empty()) {
          provider.diagnostic_id = "SB_MODEL_SECURITY_ADMISSION_REFUSED_V1";
          provider.detail = "document SELECT authorization was refused";
          return provider;
        }
        auto bounded_document_request = document_request;
        bounded_document_request.maximum_memory_bytes = std::min(
            bounded_document_request.maximum_memory_bytes,
            selected_input.maximum_memory_bytes);
        bounded_document_request.maximum_decoded_bytes = std::min(
            bounded_document_request.maximum_decoded_bytes,
            selected_input.maximum_memory_bytes);
        const auto found = api::EngineDocumentFind(bounded_document_request);
        provider.data_access_observed = found.data_access_observed;
        provider.rows_examined = found.scanned_row_versions;
        if (!found.ok) {
          provider.diagnostic_id =
              found.diagnostics.empty()
                  ? "SB_MODEL_COORDINATOR_LEG_FAILED_V1"
                  : found.diagnostics.front().code;
          provider.detail = found.diagnostics.empty()
                                ? "document provider find failed"
                                : found.diagnostics.front().detail;
          return provider;
        }
        exec::DescriptorBatch dependency_batch;
        for (const auto& dependency : dependencies) {
          dependency_batch.columns.push_back(
              {dependency.path, dependency.descriptor,
               dependency.nullable, dependency.descriptor_id});
        }
        for (const auto& typed_row : found.typed_rows) {
          if (typed_row.values.size() != dependency_batch.columns.size()) {
            provider.diagnostic_id = "SB_MODEL_TYPED_EXCHANGE_INVALID_V1";
            provider.detail = "document provider row width is inconsistent";
            return provider;
          }
          exec::DescriptorTuple tuple;
          for (std::size_t ordinal = 0; ordinal < typed_row.values.size();
               ++ordinal) {
            const auto& value = typed_row.values[ordinal];
            const auto& expected = dependency_batch.columns[ordinal];
            if (value.path != expected.stable_name ||
                value.value.descriptor.descriptor_uuid !=
                    expected.descriptor.descriptor_uuid ||
                value.value.descriptor.descriptor_kind !=
                    expected.descriptor.descriptor_kind ||
                value.value.descriptor.canonical_type_name !=
                    expected.descriptor.canonical_type_name ||
                value.value.descriptor.encoded_descriptor !=
                    expected.descriptor.encoded_descriptor ||
                (value.value.state == api::EngineValueState::missing &&
                 !expected.nullable)) {
              provider.diagnostic_id = "SB_MODEL_TYPED_EXCHANGE_INVALID_V1";
              provider.detail =
                  "document provider substituted a dependency descriptor";
              return provider;
            }
            tuple.values.push_back(value.value);
          }
          dependency_batch.rows.push_back(std::move(tuple));
          provider.provider_batch.ordered_row_identities.push_back(
              {typed_row.document_uuid, typed_row.row_uuid});
        }
        std::uint64_t actual_dependency_cells = 0;
        if (!CheckedMultiply(
                static_cast<std::uint64_t>(dependency_batch.rows.size()),
                static_cast<std::uint64_t>(dependency_batch.columns.size()),
                &actual_dependency_cells) ||
            actual_dependency_cells > provider_dependency_cell_bound) {
          provider.diagnostic_id = "SB_MODEL_RESOURCE_MEMORY_REFUSED_V1";
          provider.detail =
              "document provider dependency cell bound was exceeded";
          return provider;
        }

        auto& batch = provider.provider_batch.batch;
        for (const auto& output : projection_outputs) {
          batch.columns.push_back(output.column);
        }
        CanonicalRelationalExpressionRuntime runtime(
            relational_dag, expression_services);
        for (const auto& dependency_row : dependency_batch.rows) {
          std::vector<api::EngineTypedValue> evaluation_values =
              dependency_row.values;
          for (std::size_t ordinal = 0;
               ordinal < evaluation_values.size(); ++ordinal) {
            if (evaluation_values[ordinal].state ==
                api::EngineValueState::missing) {
              if (!dependency_batch.columns[ordinal].nullable) {
                provider.diagnostic_id =
                    "SB_MODEL_DOCUMENT_MISSING_BINDING_REFUSED_V1";
                provider.detail =
                    "missing document dependency is not nullable";
                return provider;
              }
              evaluation_values[ordinal].encoded_value.clear();
              evaluation_values[ordinal].binary_value.clear();
              evaluation_values[ordinal].setState(
                  api::EngineValueState::sql_null);
            }
          }
          exec::DescriptorTuple output_row;
          output_row.values.reserve(projection_outputs.size());
          for (const auto& output : projection_outputs) {
            if (output.direct_document_path || output.direct_identifier) {
              output_row.values.push_back(
                  evaluation_values[output.direct_dependency_ordinal]);
              continue;
            }
            api::EngineTypedValue value;
            std::string detail;
            if (!runtime.EvaluateForConsumer(
                    output.expression.expression_id,
                    output.expression.expected_type,
                    output.expression.row_binding, evaluation_values,
                    api::EngineCanonicalExpressionConsumer::projection,
                    &value, &detail)) {
              provider.diagnostic_id = "SB_MODEL_TYPED_EXCHANGE_INVALID_V1";
              provider.detail = detail.empty()
                                    ? "document expression projection failed"
                                    : std::move(detail);
              return provider;
            }
            output_row.values.push_back(std::move(value));
          }
          batch.rows.push_back(std::move(output_row));
        }
        provider.provider_batch.provider_uuid = source_input.provider_uuid;
        provider.provider_batch.provider_generation =
            source_input.provider_generation;
        provider.provider_batch.result_handle_uuid =
            source_input.result_handle_uuid;
        provider.provider_batch.causal_counter_id =
            source_input.causal_counter_id;
        provider.provider_batch.output_descriptor_ids =
            source_input.output_descriptor_ids;
        provider.provider_batch.mga_statement_context =
            source_input.mga_statement_context;
        provider.provider_batch.security_receipt_uuid = security_receipt_uuid;
        provider.provider_batch.properties.property_uuid = property_uuid;
        provider.provider_batch.properties.exact = true;
        provider.provider_batch.properties.residual_recheck_complete =
            found.residual_recheck_complete;
        provider.provider_batch.properties.base_row_mga_recheck_complete =
            found.base_row_mga_recheck_complete;
        provider.provider_batch.properties.security_recheck_complete =
            found.security_recheck_complete;
        provider.provider_batch.residual_recheck_complete =
            found.residual_recheck_complete;
        provider.provider_batch.base_row_mga_recheck_complete =
            found.base_row_mga_recheck_complete;
        provider.provider_batch.security_recheck_complete =
            found.security_recheck_complete;
        provider.ok = true;
        return provider;
      };
  CaptureRcp079ModelLegV1(
      leg_capture, scan->node_id, "document",
      "physical_document_path_scan_v1", "canonical.document.path-scan.v1",
      "document.local.v1", persisted_relation.descriptor_uuid,
      persisted_relation.descriptor_generation,
      plan::CanonicalLogicalRelationalNodeKind::kRelationSource,
      exec::PhysicalNodeKind::kScan, execution_request);
  if (leg_capture != nullptr) return result;
  exec::CanonicalPhysicalExecutorRegistration document_registration;
  document_registration.node_kind = exec::PhysicalNodeKind::kScan;
  document_registration.implementation_id =
      "physical_document_path_scan_v1";
  document_registration.executor_capability_uuid =
      physical.physical_dag.nodes.front().executor_capability_uuid;
  document_registration.executor_capability_abi_version = 1;
  document_registration.engine_owned = true;
  document_registration.accepts_optimizer_publication_v2 = true;
  document_registration.honors_dispatcher_memory_limit_v1 = true;
  document_registration.execute =
      [execution_request,
       persisted_descriptor_uuid =
           persisted_relation.descriptor_uuid](
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
                "physical_document_path_scan_v1" ||
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
              "selected document physical node identity was substituted";
          return step;
        }
        const auto callback_memory_limit = std::min(
            selected_node.memory_bytes_required,
            selected_node.dispatcher_callback_memory_limit_bytes);
        if (callback_memory_limit == 0 ||
            callback_memory_limit > selected_dag.memory_budget_bytes) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "SBLR.PLAN_TREE.RESOURCE_LIMIT";
          step.diagnostic.detail =
              "document callback memory allowance is absent or invalid";
          return step;
        }
        auto bounded_request = execution_request;
        bounded_request.input.maximum_memory_bytes = std::min(
            bounded_request.input.maximum_memory_bytes,
            callback_memory_limit);
        const auto executed =
            exec::ExecuteModelFamilySourceV1(bounded_request);
        step.data_access_observed = executed.data_access_observed;
        if (!executed.accepted || !executed.root_published ||
            executed.output.physical_node_id !=
                selected_node.physical_node_id ||
            executed.output.selected_alternative_uuid !=
                selected_node.selected_alternative_uuid ||
            executed.output.capability_uuid !=
                selected_node.executor_capability_uuid ||
            executed.output.provider_generation !=
                execution_request.input.provider_generation ||
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
                  ? "document source execution did not complete"
                  : executed.detail;
          return step;
        }
        step.result_handle_id = selected_node.physical_node_id;
        step.output_row_count = executed.output.batch.rows.size();
        step.rows_examined = executed.rows_examined;
        step.current_relation_descriptor_uuid =
            persisted_descriptor_uuid;
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
  selected.runtime_limits.maximum_rows_per_batch =
      source_input.maximum_rows;
  selected.runtime_limits.maximum_columns_per_batch = outputs.size();
  selected.runtime_limits.maximum_cells_per_batch =
      source_input.maximum_cells;
  selected.runtime_limits.maximum_total_materialized_rows =
      source_input.maximum_rows;
  selected.runtime_limits.maximum_total_materialized_cells =
      source_input.maximum_cells;
  selected.cancellation_requested =
      input.context.query_cancellation_requested
          ? input.context.query_cancellation_requested
          : std::function<bool()>([] { return false; });
  selected.available_executors.push_back(std::move(document_registration));
  selected.engine_execution_authorized = true;
  selected.result_publication_request.statement_uuid =
      input.context.statement_uuid;
  selected.result_publication_request.invocation_mode =
      exec::CanonicalResultInvocationMode::kDirect;
  selected.result_publication_request.execution_attempt_uuid =
      DerivedCanonicalUuid(
          identity_scope + ":" + input.context.current_monotonic_ns,
          "document.execution-attempt");
  selected.result_publication_request.result_kind =
      exec::CanonicalResultKind::kRows;
  selected.result_publication_request.transaction_effect_evidence_uuid =
      DerivedCanonicalUuid(
          identity_scope + ":" +
              std::to_string(input.context.local_transaction_id) + ":" +
              std::to_string(
                  input.context.snapshot_visible_through_local_transaction_id),
          "document.transaction-effect-unchanged");
  selected.result_publication_request.maximum_row_count =
      source_input.maximum_rows;
  for (std::size_t ordinal = 0; ordinal < outputs.size(); ++ordinal) {
    const auto descriptor = descriptor_for(outputs[ordinal]->descriptor_id);
    exec::CanonicalResultColumnDescriptor published;
    published.ordinal = static_cast<std::uint32_t>(ordinal);
    published.name_utf8 = outputs[ordinal]->output_name_utf8;
    published.descriptor_uuid = descriptor->descriptor_uuid;
    published.type_uuid = descriptor->type_uuid;
    published.nullability =
        descriptor->nullability == api::RelationalNullability::kNullable
            ? exec::CanonicalResultNullability::kNullable
            : exec::CanonicalResultNullability::kNonNull;
    published.collation_uuid = descriptor->collation_uuid;
    published.timezone_profile_id = descriptor->timezone_profile_id;
    selected.result_publication_request.column_bindings.push_back(
        {ordinal, true, std::move(published)});
  }
  const auto execution = ExecuteSelectedCanonicalObjectFreeDag(
      input.context, selected, physical.ordinary_runtime_memory_receipts);
  if (!execution.accepted || !execution.exact_selected_nodes_executed ||
      !execution.causal_counters_attached ||
      !execution.canonical_result_published ||
      !execution.data_access_observed || !execution.runtime_actuals.accepted ||
      execution.dispatch.executed_steps.size() != 1 ||
      execution.dispatch.executed_root_physical_node_id !=
          physical.physical_dag.root_physical_node_id ||
      execution.dispatch.selected_plan_uuid !=
          physical.physical_dag.selected_plan_uuid ||
      execution.dispatch.root_causal_counter_id !=
          physical.physical_dag.nodes.front().causal_counter_id ||
      execution.dispatch.root_output_descriptor_ids !=
          physical.physical_dag.nodes.front().output_descriptor_ids ||
      !execution.issues.empty()) {
    return refuse(
        execution.issues.empty()
            ? "SB_MODEL_COORDINATOR_LEG_FAILED_V1"
            : execution.issues.front().diagnostic_id ==
                      "SBLR.PLAN_TREE.RESOURCE_LIMIT"
                  ? "SB_MODEL_RESOURCE_MEMORY_REFUSED_V1"
                  : execution.issues.front().diagnostic_id,
        execution.issues.empty()
            ? "document selected physical execution did not complete"
            : execution.issues.front().field_id);
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
  result.api_result =
      SuccessfulApiResult(canonical_planning_request, execution);
  result.api_result.evidence.push_back(
      {"canonical.model_route",
       "SBSQL_DOCUMENT_SOURCE_TO_SBLR_MODEL_SOURCE_TO_DOCUMENT_PATH_SCAN_TO_TYPED_BATCH_V1"});
  result.api_result.evidence.push_back(
      {"canonical.model_search_family", "document.local.v1"});
  result.api_result.evidence.push_back(
      {"canonical.physical_abi",
       std::to_string(physical.physical_dag.abi_version)});
  result.api_result.evidence.push_back(
      {"canonical.physical_dispatch", "generic.selected-dag.v1"});
  result.api_result.evidence.push_back(
      {"canonical.selected_alternative",
       physical.physical_dag.nodes.front().selected_alternative_uuid});
  result.api_result.evidence.push_back(
      {"canonical.selected_capability",
       physical.physical_dag.nodes.front().executor_capability_uuid});
  result.api_result.evidence.push_back(
      {"canonical.selected_cost_vector",
       physical.physical_dag.nodes.front().cost_vector_uuid});
  result.api_result.evidence.push_back(
      {"canonical.provider_generation",
       std::to_string(source_input.provider_generation)});
  result.api_result.evidence.push_back(
      {"canonical.document_properties",
       "fixture_order|single_local_partition|document_uuid"});
  result.api_result.evidence.push_back(
      {"canonical.document_row_identity_count",
       std::to_string(execution.result_publication.row_stream.rows.size())});
  return result;
}

}  // namespace scratchbird::engine::sblr
