// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "canonical_query_key_value_composition.hpp"

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
#include "engine/internal_api/mga_relation_store/mga_relation_store.hpp"
#include "engine/optimizer/optimizer_contract.hpp"
#include "engine/optimizer/relational_planner.hpp"
#include "nosql/key_value_api.hpp"
#include "nosql/nosql_provider_generation_store.hpp"
#include "query/canonical_heap_optimizer_admission.hpp"
#include "query/canonical_relational_bridge.hpp"
#include "query/expression_api.hpp"
#include "security/security_model.hpp"
#include "transaction/transaction_api.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <ranges>
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

// SEARCH_KEY: SB_ENGINE_CANONICAL_QUERY_KEY_VALUE_COMPOSITION_AUTHORITY
// Owns one admitted production key-value source route and its bounded
// relational tail. It consumes and revalidates engine-issued MGA statement
// authority and cannot create snapshots or publish transaction finality.

constexpr std::string_view kValuesImplementationId =
    "values.materialize.canonical.v1";

void BindCanonicalPersistedRowDescriptorAuthorityForKeyValueV1(
    const api::EngineRequestContext& context,
    CanonicalRelationalExpressionRuntimeServices* services) {
  if (services == nullptr) return;
  services->persisted_row_descriptor_authority =
      [context = &context](
          const std::uint32_t,
          const api::RelationalTypeDescriptor& bound,
          const api::EngineDescriptor& persisted,
          const api::RelationalNullability effective_nullability,
          std::string* refusal_detail) {
        return ValidateCanonicalPersistedTextRowDescriptorAuthorityV1(
            *context, bound, persisted, effective_nullability,
            refusal_detail);
      };
}

}  // namespace

// QOW-SOURCE-RCP-075-KEY-VALUE-CANONICAL-QUERY-ROUTE-V1
CanonicalObjectFreeValuesExecutionResult ExecuteCanonicalKeyValueFamilyQuery(
    const CanonicalCurrentHeapExecutionRequest& input,
    Rcp079CapturedModelLegV1* leg_capture) {
  CanonicalObjectFreeValuesExecutionResult result;
  const auto& dag = input.relational_dag;
  const auto scan = std::ranges::find_if(dag.nodes, [](const auto& node) {
    return node.node_kind == api::RelationalDagNodeKind::kScan &&
           node.semantic_variant_id == "SBLR_MODEL_SOURCE_V1";
  });
  const auto operation = std::ranges::find_if(
      dag.expressions, [](const auto& expression) {
        return expression.operator_name == "KV_KEY" ||
               expression.operator_name == "KV_MULTI_GET" ||
               expression.operator_name == "KV_PREFIX";
      });
  if (scan == dag.nodes.end() || operation == dag.expressions.end()) {
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
  const bool exact_get = operation->operator_name == "KV_KEY";
  const bool multi_get = operation->operator_name == "KV_MULTI_GET";
  const bool prefix = operation->operator_name == "KV_PREFIX";
  const auto operation_count = std::ranges::count_if(
      dag.expressions, [](const auto& expression) {
        return expression.operator_name == "KV_KEY" ||
               expression.operator_name == "KV_MULTI_GET" ||
               expression.operator_name == "KV_PREFIX";
      });
  const auto scan_count = std::ranges::count_if(
      dag.nodes, [](const auto& node) {
        return node.node_kind == api::RelationalDagNodeKind::kScan &&
               node.semantic_variant_id == "SBLR_MODEL_SOURCE_V1";
      });
  if (dag.wire_version != 2 || scan_count != 1 ||
      operation_count != 1 || !scan->input_node_ids.empty() ||
      scan->required_object_uuids.size() != 1 ||
      scan->output_descriptor_ids.size() != 3 ||
      operation->expression_kind !=
          api::RelationalExpressionKind::kFunctionCall ||
      operation->function_uuid.has_value() ||
      operation->bound_name_uuid.has_value() ||
      operation->literal_kind.has_value() ||
      operation->literal_or_parameter_ref.has_value() ||
      operation->child_expression_ids.empty() ||
      (exact_get && operation->child_expression_ids.size() != 1) ||
      (prefix && operation->child_expression_ids.size() != 2) ||
      (multi_get && operation->child_expression_ids.size() < 2) ||
      dag.statement_timestamp.empty() ||
      dag.statement_timestamp != input.context.statement_timestamp) {
    return refuse("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                  "key/value canonical source shape is incomplete");
  }

  const auto expression_for = [&](const std::uint32_t expression_id) {
    return std::ranges::find_if(dag.expressions, [&](const auto& expression) {
      return expression.expression_id == expression_id;
    });
  };
  const auto typed_node_for = [&](const std::uint32_t node_id) {
    return std::ranges::find_if(dag.nodes, [&](const auto& node) {
      return node.node_id == node_id;
    });
  };
  const api::RelationalDagNode* recursive_root = nullptr;
  const api::RelationalDagNode* recursive_term = nullptr;
  const api::RelationalDagNode* set_root = nullptr;
  const api::RelationalDagNode* set_values = nullptr;
  LiveSetOperationProfile key_value_set_profile;
  std::uint32_t composition_root_node_id = dag.root_node_id;
  const auto requested_root = typed_node_for(dag.root_node_id);
  if (requested_root != dag.nodes.end() &&
      requested_root->node_kind ==
          api::RelationalDagNodeKind::kSetOperation) {
    key_value_set_profile = ResolveLiveSetOperationProfileForComposition(
        requested_root->semantic_variant_id);
    if (!key_value_set_profile.matched ||
        key_value_set_profile.operation !=
            exec::CanonicalSetOperationKind::kUnion ||
        key_value_set_profile.quantifier !=
            exec::CanonicalSetOperationQuantifier::kAll ||
        key_value_set_profile.alignment !=
            exec::CanonicalSetOperationAlignment::kOrdinal ||
        key_value_set_profile.type_profile !=
            exec::CanonicalSetOperationTypeProfile::kExact ||
        key_value_set_profile.equality_profile !=
            exec::CanonicalSetOperationEqualityProfile::kExactTyped ||
        requested_root->input_node_ids.size() != 2 ||
        requested_root->input_node_ids[0] ==
            requested_root->input_node_ids[1] ||
        !requested_root->bound_expression_ids.empty() ||
        !requested_root->required_object_uuids.empty() ||
        !requested_root->required_property_uuids.empty() ||
        !requested_root->delivered_property_uuids.empty()) {
      return refuse("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                    "key/value set root is not exact ordinal UNION ALL");
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
          "key/value UNION ALL right VALUES input or schema is not exact");
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
          "key/value recursive root is not exact bounded UNION ALL");
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
          "key/value recursive anchor, term, or schema is not exact");
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
  const api::RelationalDagNode* mixed_join = nullptr;
  const api::RelationalDagNode* relational_scan = nullptr;
  auto chain_node = typed_node_for(composition_root_node_id);
  while (chain_node != dag.nodes.end() && chain_node->node_id != scan->node_id) {
    if (!reachable_node_ids.insert(chain_node->node_id).second) {
      return refuse("SBLR.PLAN_TREE.INVALID_HANDLE",
                    "key/value downstream composition contains a cycle");
    }
    if (chain_node->node_kind == api::RelationalDagNodeKind::kJoin) {
      if (mixed_join != nullptr || !consumer_chain.empty() ||
          chain_node->input_node_ids.size() != 2 ||
          chain_node->input_node_ids[0] == chain_node->input_node_ids[1] ||
          std::ranges::find(chain_node->input_node_ids, scan->node_id) ==
              chain_node->input_node_ids.end()) {
        return refuse("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                      "key/value mixed JOIN shape is not exact");
      }
      const auto other_node_id =
          chain_node->input_node_ids[0] == scan->node_id
              ? chain_node->input_node_ids[1]
              : chain_node->input_node_ids[0];
      const auto other = typed_node_for(other_node_id);
      if (other == dag.nodes.end() ||
          other->node_kind != api::RelationalDagNodeKind::kScan ||
          other->semantic_variant_id == "SBLR_MODEL_SOURCE_V1" ||
          !other->input_node_ids.empty() ||
          other->required_object_uuids.size() != 1 ||
          !reachable_node_ids.insert(other->node_id).second) {
        return refuse("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                      "key/value JOIN relational input is not one bound heap scan");
      }
      mixed_join = &*chain_node;
      relational_scan = &*other;
      break;
    }
    if (chain_node->input_node_ids.size() != 1 ||
        (chain_node->node_kind != api::RelationalDagNodeKind::kFilter &&
         chain_node->node_kind != api::RelationalDagNodeKind::kProject &&
         chain_node->node_kind != api::RelationalDagNodeKind::kSort &&
         chain_node->node_kind != api::RelationalDagNodeKind::kWindow &&
         chain_node->node_kind != api::RelationalDagNodeKind::kAggregate &&
         chain_node->node_kind != api::RelationalDagNodeKind::kCte &&
         chain_node->node_kind != api::RelationalDagNodeKind::kLimit)) {
      return refuse(
          "SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
          "key/value downstream composition is not a supported canonical chain");
    }
    consumer_chain.push_back(&*chain_node);
    chain_node = typed_node_for(chain_node->input_node_ids.front());
  }
  if (mixed_join == nullptr && chain_node == dag.nodes.end()) {
    return refuse("SBLR.PLAN_TREE.INVALID_HANDLE",
                  "key/value downstream composition does not reach its source");
  }
  if (!reachable_node_ids.insert(scan->node_id).second ||
      reachable_node_ids.size() != dag.nodes.size()) {
    return refuse("SBLR.PLAN_TREE.INVALID_HANDLE",
                  "key/value downstream composition is disconnected or orphaned");
  }
  std::ranges::reverse(consumer_chain);
  const auto alias = expression_for(operation->child_expression_ids.front());
  if (alias == dag.expressions.end() ||
      alias->expression_kind != api::RelationalExpressionKind::kIdentifier ||
      alias->bound_name_uuid != scan->required_object_uuids.front() ||
      !alias->child_expression_ids.empty() || alias->function_uuid.has_value() ||
      alias->literal_kind.has_value() || alias->operator_name.has_value() ||
      alias->literal_or_parameter_ref.has_value()) {
    return refuse("SB_MODEL_BINDING_INCOMPLETE_V1",
                  "key/value source alias identity is incomplete");
  }

  std::vector<std::uint32_t> request_expression_ids;
  if (exact_get) {
    const auto equality = std::ranges::find_if(
        dag.expressions, [&](const auto& expression) {
          return expression.expression_kind ==
                     api::RelationalExpressionKind::kBinary &&
                 expression.operator_name == "=" &&
                 expression.child_expression_ids.size() == 2 &&
                 expression.child_expression_ids.front() ==
                     operation->expression_id;
        });
    if (equality == dag.expressions.end() ||
        std::ranges::count_if(
            dag.expressions, [&](const auto& expression) {
              return expression.expression_kind ==
                         api::RelationalExpressionKind::kBinary &&
                     expression.operator_name == "=" &&
                     expression.child_expression_ids.size() == 2 &&
                     expression.child_expression_ids.front() ==
                         operation->expression_id;
            }) != 1) {
      return refuse("SB_MODEL_KEY_VALUE_OPERATOR_REFUSED_V1",
                    "KV_KEY requires one exact equality predicate");
    }
    request_expression_ids.push_back(equality->child_expression_ids.back());
  } else {
    request_expression_ids.assign(operation->child_expression_ids.begin() + 1,
                                  operation->child_expression_ids.end());
  }
  std::unordered_set<std::uint32_t> unique_request_expressions(
      request_expression_ids.begin(), request_expression_ids.end());
  if (request_expression_ids.empty() ||
      unique_request_expressions.size() != request_expression_ids.size()) {
    return refuse(multi_get
                      ? "SB_MODEL_KEY_VALUE_MULTI_GET_EMPTY_REFUSED_V1"
                      : "SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                  "key/value request operands are empty or duplicated");
  }
  CanonicalRelationalExpressionRuntime operand_runtime(dag, {});
  std::vector<api::EngineTypedValue> request_values;
  request_values.reserve(request_expression_ids.size());
  for (const auto expression_id : request_expression_ids) {
    const auto expression = expression_for(expression_id);
    api::EngineTypedValue value;
    std::string detail;
    if (expression == dag.expressions.end() ||
        !operand_runtime.EvaluateForConsumer(
            expression_id, "text",
            api::EngineCanonicalExpressionConsumer::filter, &value,
            &detail) ||
        value.state != api::EngineValueState::value || value.is_null ||
        value.descriptor.canonical_type_name != "text" ||
        !value.binary_value.empty()) {
      return refuse("SB_MODEL_KEY_VALUE_KEY_TYPE_REFUSED_V1",
                    detail.empty()
                        ? "key/value request operand is not non-null TEXT"
                        : std::move(detail));
    }
    request_values.push_back(std::move(value));
  }

  std::vector<const api::RelationalOutputRecord*> outputs;
  for (const auto& output : dag.outputs) {
    if (output.relation_node_id == scan->node_id) outputs.push_back(&output);
  }
  std::ranges::sort(outputs, {}, &api::RelationalOutputRecord::ordinal);
  static constexpr std::array<std::string_view, 3> kOutputNames{
      "row_uuid", "key", "value"};
  if (outputs.size() != 3) {
    return refuse("SB_MODEL_TYPED_EXCHANGE_INVALID_V1",
                  "key/value public output coverage is not exact");
  }
  const auto descriptor_for = [&](const std::uint32_t descriptor_id) {
    return std::ranges::find_if(dag.descriptors, [&](const auto& descriptor) {
      return descriptor.descriptor_id == descriptor_id;
    });
  };
  for (std::size_t ordinal = 0; ordinal < outputs.size(); ++ordinal) {
    if (!outputs[ordinal]->visible || outputs[ordinal]->ordinal != ordinal ||
        outputs[ordinal]->output_name_utf8 != kOutputNames[ordinal] ||
        outputs[ordinal]->descriptor_id !=
            scan->output_descriptor_ids[ordinal] ||
        descriptor_for(outputs[ordinal]->descriptor_id) ==
            dag.descriptors.end()) {
      return refuse("SB_MODEL_TYPED_EXCHANGE_INVALID_V1",
                    "key/value public output binding was substituted");
    }
  }

  api::EngineResolveStatementSnapshotRequest snapshot_request;
  snapshot_request.context = input.context;
  const auto snapshot = api::EngineResolveStatementSnapshot(snapshot_request);
  if (!snapshot.ok) {
    return refuse("SB_MODEL_MGA_CONTEXT_MISMATCH_V1",
                  "current engine MGA statement snapshot is unavailable");
  }
  auto mga = PhysicalMgaContextFromResolvedSnapshot(
      input.context, snapshot.snapshot_vector);
  mga.statement_timestamp = input.context.statement_timestamp;
  if (!exec::PhysicalMgaStatementContextValid(mga)) {
    return refuse("SB_MODEL_KEY_VALUE_STATEMENT_TIMESTAMP_INVALID_V1",
                  "engine-issued key/value statement timestamp is invalid");
  }
  const auto object_uuid = scan->required_object_uuids.front();
  const auto authorization = api::EvaluateMaterializedAuthorization(
      input.context, input.context.authorization_context, "SELECT",
      object_uuid);
  if (!authorization.authorized || authorization.denied ||
      authorization.policy_recheck_required ||
      !authorization.diagnostics.empty()) {
    return refuse("SB_MODEL_SECURITY_ADMISSION_REFUSED_V1",
                  "key/value SELECT authorization was refused");
  }
  std::vector<std::string> admitted_object_uuids{object_uuid};
  if (relational_scan != nullptr) {
    const auto relational_object_uuid =
        relational_scan->required_object_uuids.front();
    const auto relational_authorization = api::EvaluateMaterializedAuthorization(
        input.context, input.context.authorization_context, "SELECT",
        relational_object_uuid);
    if (!relational_authorization.authorized ||
        relational_authorization.denied ||
        relational_authorization.policy_recheck_required ||
        !relational_authorization.diagnostics.empty()) {
      return refuse("SB_MODEL_SECURITY_ADMISSION_REFUSED_V1",
                    "key/value mixed JOIN relational SELECT was refused");
    }
    admitted_object_uuids.push_back(relational_object_uuid);
  }
  const auto loaded_relation =
      api::LoadMgaRelationStorageDescriptor(input.context, object_uuid);
  if (!loaded_relation.ok) {
    return refuse("SB_MODEL_KEY_VALUE_EXACT_FALLBACK_UNAVAILABLE_V1",
                  loaded_relation.diagnostic.detail.empty()
                      ? "persistent key/value relation descriptor is unavailable"
                      : loaded_relation.diagnostic.detail);
  }
  const auto& persisted_relation = loaded_relation.descriptor;
  if (persisted_relation.relation_uuid != object_uuid ||
      persisted_relation.database_uuid !=
          input.context.database_uuid ||
      persisted_relation.schema_uuid.is_nil() ||
      persisted_relation.relation_kind != "table" ||
      persisted_relation.storage_profile != "local_mga_rowstore_v1" ||
      persisted_relation.descriptor_uuid.is_nil() ||
      persisted_relation.descriptor_generation == 0) {
    return refuse("SB_MODEL_TYPED_EXCHANGE_INVALID_V1",
                  "persistent key/value relation descriptor is invalid");
  }
  if (!api::ExactKeyValueStorageDescriptorV1(persisted_relation)) {
    return refuse("SB_MODEL_KEY_VALUE_VALUE_TYPE_REFUSED_V1",
                  "key/value storage descriptor is not exact before data access");
  }
  const auto row_descriptor = descriptor_for(outputs[0]->descriptor_id);
  const auto key_descriptor = descriptor_for(outputs[1]->descriptor_id);
  const auto value_descriptor = descriptor_for(outputs[2]->descriptor_id);
  if (row_descriptor == dag.descriptors.end() ||
      key_descriptor == dag.descriptors.end() ||
      value_descriptor == dag.descriptors.end() ||
      row_descriptor->descriptor_uuid !=
          persisted_relation.descriptor_uuid ||
      key_descriptor->descriptor_uuid !=
          persisted_relation.columns[0].value_descriptor.descriptor_uuid
               ||
      value_descriptor->descriptor_uuid !=
          persisted_relation.columns[1].value_descriptor.descriptor_uuid
               ||
      row_descriptor->nullability !=
          api::RelationalNullability::kNonNull ||
      key_descriptor->nullability !=
          api::RelationalNullability::kNonNull ||
      value_descriptor->nullability !=
          api::RelationalNullability::kNonNull) {
    return refuse("SB_MODEL_TYPED_EXCHANGE_INVALID_V1",
                  "key/value public descriptor cohort differs from storage");
  }
  const auto row_type_uuid = ExactCanonicalCoreDatatypeUuidV1("uuid");
  const auto text_type_uuid =
      ExactCanonicalCoreDatatypeTypeUuidV1("character");
  if (row_type_uuid.empty() || text_type_uuid.empty() ||
      !CanonicalDescriptorFieldEqualsForComposition(
          persisted_relation.columns[0].value_descriptor, "type_uuid",
          std::string_view(text_type_uuid)) ||
      !CanonicalDescriptorFieldEqualsForComposition(
          persisted_relation.columns[1].value_descriptor, "type_uuid",
          std::string_view(text_type_uuid)) ||
      row_descriptor->type_uuid != row_type_uuid ||
      key_descriptor->type_uuid != text_type_uuid ||
      value_descriptor->type_uuid != text_type_uuid) {
    return refuse("SB_MODEL_RESULT_DESCRIPTOR_SOURCE_BINDING_INVALID_V1",
                  "key/value public type identity differs from storage");
  }

  const auto identity_scope =
      dag.bound_sblr_tree_uuid + ":" + input.context.statement_uuid;
  const auto physical_alternative_uuid = DerivedCanonicalUuid(
      identity_scope,
      "alternative." + std::to_string(scan->node_id) +
          ".physical_key_value_scan_v1");
  const auto physical_cost_uuid = DerivedCanonicalUuid(
      identity_scope,
      "cost-vector." + std::to_string(scan->node_id) +
          ".physical_key_value_scan_v1");
  const auto provider_uuid =
      DerivedCanonicalUuid(identity_scope, "key-value.provider");
  const auto capability_uuid =
      DerivedCanonicalUuid(identity_scope, "key-value.capability");
  const auto fallback_provider_uuid =
      DerivedCanonicalUuid(identity_scope, "key-value.exact-fallback-provider");
  const auto fallback_capability_uuid =
      DerivedCanonicalUuid(identity_scope, "key-value.exact-fallback-capability");
  const auto result_handle_uuid =
      DerivedCanonicalUuid(identity_scope, "key-value.result-handle");
  const auto property_uuid =
      DerivedCanonicalUuid(identity_scope, "key-value.property");
  const auto security_receipt_uuid =
      DerivedCanonicalUuid(identity_scope, "key-value.security-receipt");
  const auto policy_snapshot_uuid =
      DerivedCanonicalUuid(identity_scope, "key-value.policy-snapshot");
  const auto statistics_snapshot_uuid =
      DerivedCanonicalUuid(identity_scope, "key-value.statistics-snapshot");
  const auto resource_contract_uuid =
      DerivedCanonicalUuid(identity_scope, "key-value.resource-contract");
  const auto generation =
      std::max<std::uint64_t>(1, input.context.catalog_generation_id);

  opt::ModelFamilyCoordinatorRequestV1 planning;
  planning.family_id = "key_value";
  planning.operation_id = exact_get
                              ? "KEY_VALUE_GET"
                              : multi_get ? "KEY_VALUE_MULTI_GET"
                                          : "KEY_VALUE_PREFIX_RANGE";
  planning.logical_operator_id = "LOGICAL_KEY_VALUE_SOURCE_V1";
  planning.logical_node_id = scan->node_id;
  planning.object_uuid = object_uuid;
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
      planning, identity_scope + ".key-value.native",
      opt::ModelFamilyAlternativeRouteClassV1::kNative, provider_uuid,
      capability_uuid, persisted_relation.descriptor_generation, true, 1, 1,
      planning.memory_budget_bytes));
  alternatives.push_back(MakeModelFamilyCapabilitySnapshotForCompositionV1(
      planning, identity_scope + ".key-value.fallback",
      opt::ModelFamilyAlternativeRouteClassV1::kExactCollectionFallback,
      fallback_provider_uuid, fallback_capability_uuid,
      persisted_relation.descriptor_generation, true, 2, 2,
      planning.memory_budget_bytes));
  const auto planned = PlanCanonicalModelFamilySourceForCompositionV1(
      planning, identity_scope + ".key-value.inventory",
      std::move(alternatives));
  if (!planned.accepted || !planned.selected ||
      !planned.data_access_allowed || !planned.optimizer_owned_enumeration ||
      planned.exact_fallback_selected ||
      planned.selected_candidate.provider_uuid != provider_uuid ||
      planned.selected_candidate.capability_uuid != capability_uuid ||
      planned.selected_candidate.provider_generation !=
          persisted_relation.descriptor_generation) {
    return refuse(
        planned.diagnostic_id.empty()
            ? "SB_MODEL_KEY_VALUE_EXACT_FALLBACK_UNAVAILABLE_V1"
            : planned.diagnostic_id,
        planned.detail.empty()
            ? "key/value coordinator did not select the exact candidate"
            : planned.detail);
  }

  api::CanonicalRelationalPlanningScope planning_scope;
  planning_scope.catalog_epoch_uuid =
      input.context.catalog_epoch_uuid;
  planning_scope.security_context_uuid =
      input.context.authorization_context.authority_uuid;
  planning_scope.statement_uuid = input.context.statement_uuid;
  planning_scope.statement_timestamp = input.context.statement_timestamp;
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
  const auto key_value_logical = std::ranges::find_if(
      logical.logical_graph.nodes, [&](const auto& node) {
        return node.logical_node_id == scan->node_id;
      });
  if (!logical.accepted ||
      key_value_logical == logical.logical_graph.nodes.end() ||
      key_value_logical->semantic_variant_id != "SBLR_MODEL_SOURCE_V1" ||
      key_value_logical->model_family_identity !=
          plan::CanonicalLogicalModelFamilyIdentity::kKeyValue) {
    return refuse(logical.issues.empty()
                      ? "QOW-DIAG-OPTIMIZER-ADMISSION-BOUND-REQUEST-V1"
                      : logical.issues.front().diagnostic_id,
                  logical.issues.empty()
                      ? "key/value logical bridge was refused"
                      : logical.issues.front().field_id);
  }
  plan::CanonicalMgaStatementContext current_logical_mga;
  current_logical_mga.statement_uuid = mga.statement_uuid;
  current_logical_mga.statement_timestamp = mga.statement_timestamp;
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
    return refuse("QOW-DIAG-QRY-004-KEY-VALUE-OPTIMIZER-MGA-SNAPSHOT-V1",
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
    return refuse("QOW-DIAG-QRY-004-KEY-VALUE-OPTIMIZER-CONTEXT-V1",
                  "current_monotonic_ns");
  }
  admission_context.admitted_at_monotonic_ns = admitted_at_monotonic_ns;
  admission_context.metadata_snapshot_engine_owned = true;
  admission_context.authorization_context_engine_owned = true;
  admission_context.catalog_object_uuids = admitted_object_uuids;
  admission_context.authorized_object_uuids = admitted_object_uuids;
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
            ? "key/value canonical optimizer admission was refused"
            : canonical_admission.field_id);
  }
  result.optimizer_admitted = true;
  CanonicalObjectFreeValuesExecutionRequest canonical_planning_request{
      input.context, dag, canonical_admission.request,
      canonical_admission.admission};
  // The current-heap entry point derives all authority internally and does
  // not accept caller-supplied expression callbacks.  Mixed relational joins
  // still need the same engine-owned descriptor comparison authority used by
  // the wire route, so bind it here from the admitted statement context.
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
  BindCanonicalPersistedRowDescriptorAuthorityForKeyValueV1(
      input.context, &canonical_planning_request.expression_services);
  std::vector<exec::ExecutorColumnDescriptor> public_columns;
  public_columns.reserve(3);
  static constexpr std::array<std::string_view, 3> kOutputTypes{
      "uuid", "text", "text"};
  MaterializedValues composition_state;
  composition_state.ok = true;
  for (std::size_t ordinal = 0; ordinal < outputs.size(); ++ordinal) {
    const auto descriptor = descriptor_for(outputs[ordinal]->descriptor_id);
    api::EngineDescriptor engine_descriptor;
    engine_descriptor.descriptor_uuid =
        descriptor->descriptor_uuid;
    engine_descriptor.descriptor_kind = "scalar";
    engine_descriptor.canonical_type_name =
        std::string(kOutputTypes[ordinal]);
    engine_descriptor.encoded_descriptor =
        "type_uuid=" + descriptor->type_uuid + ";nullability=non_null";
    engine_descriptor.collation_uuid = descriptor->collation_uuid.value_or(api::EngineUuid{});
    if (descriptor->timezone_profile_id.has_value()) {
      engine_descriptor.encoded_descriptor +=
          ";timezone_profile_id=" + *descriptor->timezone_profile_id;
    }
    public_columns.push_back(
        {std::string(kOutputNames[ordinal]), engine_descriptor, false,
         descriptor->descriptor_id});
    exec::CanonicalResultColumnBinding binding;
    binding.physical_column_ordinal = ordinal;
    binding.visible = true;
    binding.published_descriptor = exec::CanonicalResultColumnDescriptor{
        static_cast<std::uint32_t>(ordinal),
        outputs[ordinal]->output_name_utf8, descriptor->descriptor_uuid,
        descriptor->type_uuid, exec::CanonicalResultNullability::kNonNull,
        descriptor->collation_uuid, descriptor->timezone_profile_id};
    composition_state.result_bindings.push_back(std::move(binding));
  }
  composition_state.batch.columns = public_columns;
  const auto descriptor_validation = exec::ValidateCanonicalDescriptorBatch(
      composition_state.batch, scan->output_descriptor_ids);
  if (!descriptor_validation.ok) {
    return refuse("SB_MODEL_TYPED_EXCHANGE_INVALID_V1",
                  descriptor_validation.detail);
  }

  std::vector<LivePhysicalNodeProfile> profiles;
  LivePhysicalNodeProfile profile;
  profile.logical_node_id = scan->node_id;
  profile.implementation_id = "physical_key_value_scan_v1";
  profile.capability_uuid = planned.selected_candidate.capability_uuid;
  profile.logical_node_kind =
      plan::CanonicalLogicalRelationalNodeKind::kRelationSource;
  profile.physical_node_kind = exec::PhysicalNodeKind::kScan;
  profile.transformation_rule_id = "canonical.key-value.scan.v1";
  profile.estimated_rows = 1;
  profile.memory_bytes_required = std::max<std::uint64_t>(
      1, planned.selected_candidate.cost.memory_bytes_required);
  profile.page_read_sequential_units = 1;
  profile.mga_visibility_checks_expected = 1;
  profile.storage_read_capable = true;
  profile.mga_visibility_capable = true;
  profile.spill_supported = false;
  profile.parallel_safe = false;
  profile.parallel_required = false;
  profile.residual_predicate_required = true;
  profile.storage_recheck_required = true;
  profile.compatibility_profile_id = "key_value.local.v1";
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
                  "key/value producer logical identity is absent");
  }
  std::optional<PreparedFilterRoot> prepared_filter;
  std::optional<PreparedProjectRoot> prepared_project;
  std::optional<PreparedSortRoot> prepared_sort;
  std::optional<exec::ExecutorColumnDescriptor> prepared_row_number;
  std::optional<PreparedGlobalAggregateRoot> prepared_count_star;
  std::optional<PreparedLimitRoot> prepared_limit;
  std::optional<PreparedLiveSetNode> prepared_key_value_set;
  MaterializedValues materialized_key_value_set_values;
  std::optional<PreparedRecursiveCteRoot> prepared_recursive_cte;
  bool prepared_nonrecursive_cte = false;
  std::uint64_t limit_count = 0;
  std::uint64_t limit_offset = 0;
  bool fetch_first_rows_only = false;
  std::string limit_implementation_id;
  std::string cte_implementation_id;
  std::string filter_capability_uuid;
  std::string project_capability_uuid;
  std::string sort_capability_uuid;
  std::string window_capability_uuid;
  std::string window_order_evidence_uuid;
  std::string aggregate_capability_uuid;
  std::string cte_capability_uuid;
  std::string limit_capability_uuid;
  std::string set_values_capability_uuid;
  std::string set_root_capability_uuid;
  std::string recursive_term_capability_uuid;
  std::string recursive_root_capability_uuid;
  std::optional<exec::CanonicalAcceptedJoinKind> mixed_join_kind;
  std::string mixed_join_component;
  std::string mixed_join_operation;
  std::string relational_scan_capability_uuid;
  std::string mixed_join_capability_uuid;
  CanonicalRelationalExpressionRowBinding mixed_join_predicate_binding;
  const auto core_manifest = dt::LoadCurrentCoreDatatypeCatalogManifest();
  if (!core_manifest.ok()) {
    return refuse("SB_MODEL_TYPED_EXCHANGE_INVALID_V1",
                  "current core datatype catalog is unavailable");
  }
  const auto type_uuid_for = [](const std::string_view stable_name) {
    return ExactCanonicalCoreDatatypeTypeUuidV1(stable_name);
  };
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
                    "key/value consumer identity or ordering is invalid");
    }
    LivePhysicalNodeProfile consumer_profile;
    consumer_profile.logical_node_id = consumer->node_id;
    consumer_profile.logical_node_kind = logical_consumer->node_kind;
    consumer_profile.estimated_rows = 65536;
    consumer_profile.memory_bytes_required = 1;
    consumer_profile.minimum_input_count = 1;
    consumer_profile.maximum_input_count = 1;
    consumer_profile.runtime_peak_from_callback_batches = true;
    if (consumer->node_kind == api::RelationalDagNodeKind::kFilter) {
      if (consumer->semantic_variant_id != "filter.where.v1") {
        return refuse("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                      "key/value FILTER semantic is not canonical");
      }
      auto prepared = PrepareFilterRootForComposition(
          dag, *logical_consumer, *previous_logical, composition_state);
      if (!prepared.ok) {
        return refuse("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                      prepared.detail);
      }
      prepared_filter = std::move(prepared);
      filter_capability_uuid = DerivedCanonicalUuid(
          identity_scope, "key-value.filter.capability");
      consumer_profile.implementation_id = "filter.3vl.row.v1";
      consumer_profile.capability_uuid = filter_capability_uuid;
      consumer_profile.physical_node_kind = exec::PhysicalNodeKind::kFilter;
      consumer_profile.transformation_rule_id =
          "canonical.key-value.filter.3vl.v1";
      consumer_profile.memory_bytes_required = planning.memory_budget_bytes;
    } else if (consumer->node_kind ==
               api::RelationalDagNodeKind::kProject) {
      if (consumer->semantic_variant_id != "project.select-list.v1" ||
          consumer->bound_expression_ids.empty()) {
        return refuse("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                      "key/value PROJECT semantic is not canonical");
      }
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
      project_capability_uuid = DerivedCanonicalUuid(
          identity_scope, "key-value.project.capability");
      consumer_profile.implementation_id =
          "project.typed.expression-row.v1";
      consumer_profile.capability_uuid = project_capability_uuid;
      consumer_profile.physical_node_kind = exec::PhysicalNodeKind::kProject;
      consumer_profile.transformation_rule_id =
          "canonical.key-value.project.expression-row.v1";
      consumer_profile.memory_bytes_required = planning.memory_budget_bytes;
    } else if (consumer->node_kind == api::RelationalDagNodeKind::kSort) {
      if (consumer->semantic_variant_id != "sort.required-order.v1") {
        return refuse("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                      "key/value SORT semantic is not canonical");
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
      auto prepared =
          expression_ordering
              ? PrepareExpressionSortRootForComposition(
                    input.context, dag, sort_properties, *logical_consumer,
                    *previous_logical, composition_state,
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
          identity_scope, "key-value.sort.capability");
      consumer_profile.implementation_id =
          prepared_sort->expression_ordering
              ? "sort.typed.expression-row.v1"
              : "sort.typed.terms.v1";
      consumer_profile.capability_uuid = sort_capability_uuid;
      consumer_profile.physical_node_kind = exec::PhysicalNodeKind::kSort;
      consumer_profile.transformation_rule_id =
          prepared_sort->expression_ordering
              ? "canonical.key-value.sort.expression-order.v1"
              : "canonical.key-value.sort.required-order.v1";
      consumer_profile.delivered_property_uuids =
          logical_consumer->delivered_property_uuids;
      consumer_profile.supported_property_kinds = {
          plan::CanonicalLogicalPropertyKind::kOrdering};
      consumer_profile.memory_bytes_required = planning.memory_budget_bytes;
    } else if (consumer->node_kind == api::RelationalDagNodeKind::kWindow) {
      if (!prepared_sort.has_value()) {
        return refuse("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                      "key/value ROW_NUMBER binding is not exact");
      }
      const auto row_number = PrepareGlobalRowNumberWindowBindingForComposition(
          dag, canonical_admission.request.logical_properties, *consumer,
          *logical_consumer, *previous_logical, *prepared_sort,
          composition_state.batch.columns.size(),
          composition_state.result_bindings.size(), type_uuid_for("int64"),
          "key/value");
      if (!row_number.ok) {
        return refuse(row_number.diagnostic_id, row_number.detail);
      }
      const auto* output_descriptor = row_number.result_descriptor;
      const auto& window_outputs = row_number.outputs;
      api::EngineDescriptor descriptor;
      descriptor.descriptor_uuid =
          output_descriptor->descriptor_uuid;
      descriptor.descriptor_kind = "scalar";
      descriptor.canonical_type_name = "int64";
      descriptor.encoded_descriptor =
          "type_uuid=" + output_descriptor->type_uuid +
          ";nullability=non_null";
      exec::ExecutorColumnDescriptor row_number_column{
          window_outputs.back()->output_name_utf8, descriptor, false,
          output_descriptor->descriptor_id};
      if (!api::QowCanonicalDescriptorIdentityV1(descriptor)) {
        return refuse("SB_MODEL_TYPED_EXCHANGE_INVALID_V1",
                      "key/value ROW_NUMBER descriptor is invalid");
      }
      exec::CanonicalResultColumnBinding row_number_binding;
      row_number_binding.physical_column_ordinal =
          composition_state.batch.columns.size();
      row_number_binding.visible = true;
      row_number_binding.published_descriptor =
          exec::CanonicalResultColumnDescriptor{
              static_cast<std::uint32_t>(
                  composition_state.batch.columns.size()),
              row_number_column.stable_name,
              output_descriptor->descriptor_uuid,
              output_descriptor->type_uuid,
              exec::CanonicalResultNullability::kNonNull,
              std::nullopt, std::nullopt};
      composition_state.batch.columns.push_back(row_number_column);
      composition_state.result_bindings.push_back(
          std::move(row_number_binding));
      prepared_row_number = std::move(row_number_column);
      window_capability_uuid = DerivedCanonicalUuid(
          identity_scope, "key-value.window.row-number.capability");
      window_order_evidence_uuid = DerivedCanonicalUuid(
          identity_scope + ":" + prepared_sort->ordering_property_uuid,
          "key-value.window.deterministic-order");
      consumer_profile.implementation_id = "window.row-number.v1";
      consumer_profile.capability_uuid = window_capability_uuid;
      consumer_profile.physical_node_kind = exec::PhysicalNodeKind::kWindow;
      consumer_profile.transformation_rule_id =
          "canonical.key-value.window.row-number.v1";
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
                      "key/value aggregate is not exact global COUNT(*)");
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
            "key/value COUNT(*) result descriptor is not canonical int64");
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
          identity_scope, "key-value.count-star.capability");
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
            "key/value nonrecursive CTE is not an exact schema-preserving "
            "bound CTE");
      }
      const auto validated = exec::ValidateCanonicalDescriptorBatch(
          composition_state.batch, consumer->output_descriptor_ids);
      if (!validated.ok) {
        return refuse("SB_MODEL_TYPED_EXCHANGE_INVALID_V1",
                      "key/value nonrecursive CTE input: " +
                          validated.detail);
      }
      prepared_nonrecursive_cte = true;
      cte_implementation_id =
          consumer->shareable ? "cte.bound.materialize.typed.v1"
                              : "cte.bound.inline.typed.v1";
      cte_capability_uuid = DerivedCanonicalUuid(
          identity_scope,
          consumer->shareable
              ? "key-value.cte.materialize.capability"
              : "key-value.cte.inline.capability");
      consumer_profile.implementation_id = cte_implementation_id;
      consumer_profile.capability_uuid = cte_capability_uuid;
      consumer_profile.physical_node_kind = exec::PhysicalNodeKind::kCte;
      consumer_profile.transformation_rule_id =
          consumer->shareable
              ? "canonical.key-value.cte.materialize.v1"
              : "canonical.key-value.cte.inline.v1";
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
                      "key/value LIMIT/FETCH semantic is not canonical");
      }
      CanonicalRelationalExpressionRuntime runtime(
          dag, canonical_planning_request.expression_services);
      std::string detail;
      if (!EvaluateNonNegativeRowBoundForComposition(
              &runtime, consumer->bound_expression_ids.front(), &limit_count,
              &detail) ||
          (expected_arity == 2 &&
           !EvaluateNonNegativeRowBoundForComposition(
               &runtime, consumer->bound_expression_ids.back(),
               &limit_offset, &detail))) {
        return refuse("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                      detail.empty() ? "key/value LIMIT/FETCH bound is invalid"
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
          identity_scope, "key-value.limit.capability");
      consumer_profile.implementation_id = limit_implementation_id;
      consumer_profile.capability_uuid = limit_capability_uuid;
      consumer_profile.physical_node_kind = exec::PhysicalNodeKind::kLimit;
      consumer_profile.transformation_rule_id =
          fetch_first_rows_only
              ? "canonical.key-value.fetch.first-rows-only.v1"
              : "canonical.key-value.limit.bound-count-offset.v1";
      consumer_profile.memory_bytes_required = planning.memory_budget_bytes;
    } else {
      return refuse("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                    "key/value consumer kind is not prepared");
    }
    profiles.push_back(std::move(consumer_profile));
    previous_logical = logical_consumer;
  }
  constexpr std::size_t kKeyValueCompositionRowBound = 65536;
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
          "key/value UNION ALL logical input identity is not exact");
    }
    auto right = MaterializeValues(
        dag, *logical_values,
        canonical_planning_request.expression_services);
    if (!right.ok) {
      return refuse("SB_MODEL_TYPED_EXCHANGE_INVALID_V1",
                    "key/value UNION ALL right VALUES: " + right.detail);
    }
    if (right.batch.rows.empty() ||
        right.batch.rows.size() >= kKeyValueCompositionRowBound) {
      return refuse(
          "QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1",
          "key/value UNION ALL leaves no bounded source row budget");
    }
    auto prepared = PrepareSetOperationRootForComposition(
        input.context, dag, *logical_root, composition_state, right,
        key_value_set_profile);
    if (!prepared.ok) {
      return refuse("SB_MODEL_TYPED_EXCHANGE_INVALID_V1",
                    "key/value UNION ALL: " + prepared.detail);
    }
    std::uint64_t comparison_bound = 0;
    std::uint64_t collation_comparison_count = 0;
    if (!BoundSetOperationEqualityComparisonsForComposition(
            prepared, key_value_set_profile, kKeyValueCompositionRowBound,
            &comparison_bound, &collation_comparison_count) ||
        comparison_bound > std::numeric_limits<std::size_t>::max()) {
      return refuse(
          "QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
          "key/value UNION ALL comparison bound overflowed");
    }
    std::uint64_t values_memory = 1;
    if (!AddBatchMemoryBytes(right.batch, &values_memory) ||
        values_memory > planning.memory_budget_bytes) {
      return refuse("SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                    "key/value UNION ALL VALUES memory bound was exceeded");
    }
    set_values_capability_uuid = DerivedCanonicalUuid(
        identity_scope, "key-value.set-values.capability");
    set_root_capability_uuid = DerivedCanonicalUuid(
        identity_scope, "key-value.set-union-all.capability");

    LivePhysicalNodeProfile values_profile;
    values_profile.logical_node_id = logical_values->logical_node_id;
    values_profile.implementation_id = std::string(kValuesImplementationId);
    values_profile.capability_uuid = set_values_capability_uuid;
    values_profile.logical_node_kind = logical_values->node_kind;
    values_profile.physical_node_kind = exec::PhysicalNodeKind::kValues;
    values_profile.transformation_rule_id =
        "canonical.key-value.set-values.materialize.v1";
    values_profile.estimated_rows = right.batch.rows.size();
    values_profile.memory_bytes_required = values_memory;
    profiles.push_back(std::move(values_profile));

    LivePhysicalNodeProfile set_profile;
    set_profile.logical_node_id = logical_root->logical_node_id;
    set_profile.implementation_id = key_value_set_profile.implementation_id;
    set_profile.capability_uuid = set_root_capability_uuid;
    set_profile.logical_node_kind = logical_root->node_kind;
    set_profile.physical_node_kind = exec::PhysicalNodeKind::kSetOperation;
    set_profile.transformation_rule_id =
        "canonical.key-value.set.union-all.ordinal.v1";
    set_profile.estimated_rows = kKeyValueCompositionRowBound;
    set_profile.memory_bytes_required = planning.memory_budget_bytes;
    set_profile.minimum_input_count = 2;
    set_profile.maximum_input_count = 2;
    set_profile.runtime_peak_from_callback_batches = true;
    profiles.push_back(std::move(set_profile));

    composition_state.batch.columns = prepared.result_columns;
    composition_state.result_bindings = prepared.result_bindings;
    prepared_key_value_set = PreparedLiveSetNode{
        key_value_set_profile, std::move(prepared),
        kKeyValueCompositionRowBound,
        std::max<std::size_t>(
            1, static_cast<std::size_t>(comparison_bound))};
    materialized_key_value_set_values = std::move(right);
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
          "key/value recursive logical branch is not an exact int64 anchor and empty term");
    }
    CanonicalRelationalExpressionRuntime runtime(
        dag, canonical_planning_request.expression_services);
    std::uint64_t upper_bound = 0;
    std::string detail;
    if (!EvaluateNonNegativeRowBoundForComposition(
            &runtime, recursive_root->bound_expression_ids.front(),
            &upper_bound, &detail) ||
        upper_bound >= kKeyValueCompositionRowBound ||
        upper_bound >= static_cast<std::uint64_t>(
                           std::numeric_limits<std::size_t>::max())) {
      return refuse(
          "QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1",
          detail.empty()
              ? "key/value recursive upper bound exceeds its admitted work bound"
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
            upper_bound, kKeyValueCompositionRowBound)) {
      return refuse(
          "QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
          "key/value recursive cardinality bound is invalid");
    }
    if (!BindPreparedRecursiveCtePeakMemory(
            &prepared, planning.memory_budget_bytes)) {
      return refuse(
          "SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
          "key/value recursive payload peak exceeds its admitted memory budget");
    }
    prepared_recursive_cte = prepared;
    recursive_term_capability_uuid = DerivedCanonicalUuid(
        identity_scope, "key-value.recursive-term.capability");
    recursive_root_capability_uuid = DerivedCanonicalUuid(
        identity_scope, "key-value.recursive-root.capability");

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
  if (mixed_join != nullptr) {
    const auto relational_logical = logical_node_for(relational_scan->node_id);
    const auto join_logical = logical_node_for(mixed_join->node_id);
    if (relational_logical ==
            canonical_admission.request.logical_graph.nodes.end() ||
        join_logical ==
            canonical_admission.request.logical_graph.nodes.end() ||
        join_logical->input_logical_node_ids != mixed_join->input_node_ids) {
      return refuse("SBLR.PLAN_TREE.INVALID_HANDLE",
                    "key/value mixed JOIN logical identity is incomplete");
    }
    if (mixed_join->semantic_variant_id == "join.cross.v1") {
      mixed_join_kind = exec::CanonicalAcceptedJoinKind::kCross;
      mixed_join_component = "cross";
      mixed_join_operation = "CROSS JOIN";
    } else if (mixed_join->semantic_variant_id == "join.inner.v1") {
      mixed_join_kind = exec::CanonicalAcceptedJoinKind::kInner;
      mixed_join_component = "inner";
      mixed_join_operation = "INNER JOIN";
    } else if (mixed_join->semantic_variant_id == "join.left-outer.v1") {
      mixed_join_kind = exec::CanonicalAcceptedJoinKind::kLeftOuter;
      mixed_join_component = "left-outer";
      mixed_join_operation = "LEFT OUTER JOIN";
    } else if (mixed_join->semantic_variant_id == "join.right-outer.v1") {
      mixed_join_kind = exec::CanonicalAcceptedJoinKind::kRightOuter;
      mixed_join_component = "right-outer";
      mixed_join_operation = "RIGHT OUTER JOIN";
    } else if (mixed_join->semantic_variant_id == "join.full-outer.v1") {
      mixed_join_kind = exec::CanonicalAcceptedJoinKind::kFullOuter;
      mixed_join_component = "full-outer";
      mixed_join_operation = "FULL OUTER JOIN";
    } else if (mixed_join->semantic_variant_id == "join.left-semi.v1") {
      mixed_join_kind = exec::CanonicalAcceptedJoinKind::kLeftSemi;
      mixed_join_component = "left-semi";
      mixed_join_operation = "LEFT SEMI JOIN";
    } else if (mixed_join->semantic_variant_id == "join.left-anti.v1") {
      mixed_join_kind = exec::CanonicalAcceptedJoinKind::kLeftAnti;
      mixed_join_component = "left-anti";
      mixed_join_operation = "LEFT ANTI JOIN";
    } else {
      return refuse("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                    "key/value mixed JOIN kind is not canonical");
    }
    const bool predicate_join =
        *mixed_join_kind != exec::CanonicalAcceptedJoinKind::kCross;
    if (mixed_join->bound_expression_ids.size() !=
        static_cast<std::size_t>(predicate_join)) {
      return refuse("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                    "key/value mixed JOIN predicate arity is invalid");
    }
    if (predicate_join) {
      std::vector<std::uint32_t> predicate_descriptors;
      for (const auto input_node_id : mixed_join->input_node_ids) {
        const auto input_node = typed_node_for(input_node_id);
        predicate_descriptors.insert(predicate_descriptors.end(),
                                     input_node->output_descriptor_ids.begin(),
                                     input_node->output_descriptor_ids.end());
      }
      std::string detail;
      if (!PrepareInputRowBindingForComposition(
              dag, mixed_join->bound_expression_ids.front(),
              predicate_descriptors, &mixed_join_predicate_binding,
              &detail)) {
        return refuse("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                      detail.empty()
                          ? "key/value mixed JOIN predicate is not bound"
                          : detail);
      }
    }
    relational_scan_capability_uuid = DerivedCanonicalUuid(
        identity_scope, "key-value.mixed-join.heap-scan.capability");
    mixed_join_capability_uuid = DerivedCanonicalUuid(
        identity_scope, "key-value.mixed-join.capability");
    LivePhysicalNodeProfile heap_profile;
    heap_profile.logical_node_id = relational_scan->node_id;
    heap_profile.implementation_id = "scan.heap.v1";
    heap_profile.capability_uuid = relational_scan_capability_uuid;
    heap_profile.logical_node_kind =
        plan::CanonicalLogicalRelationalNodeKind::kRelationSource;
    heap_profile.physical_node_kind = exec::PhysicalNodeKind::kScan;
    heap_profile.transformation_rule_id =
        "canonical.key-value.mixed-join.heap-scan.v1";
    heap_profile.estimated_rows = 1;
    heap_profile.memory_bytes_required =
        input.context.optimizer_memory_budget_bytes;
    heap_profile.page_read_sequential_units = 1;
    heap_profile.mga_visibility_checks_expected = 1;
    heap_profile.storage_read_capable = true;
    heap_profile.mga_visibility_capable = true;
    profiles.push_back(std::move(heap_profile));
    LivePhysicalNodeProfile join_profile;
    join_profile.logical_node_id = mixed_join->node_id;
    join_profile.implementation_id =
        "join." + mixed_join_component + ".3vl.nested.v1";
    join_profile.capability_uuid = mixed_join_capability_uuid;
    join_profile.logical_node_kind =
        plan::CanonicalLogicalRelationalNodeKind::kJoin;
    join_profile.physical_node_kind = exec::PhysicalNodeKind::kJoin;
    join_profile.transformation_rule_id =
        "canonical.key-value.mixed-join." + mixed_join_component + ".v1";
    join_profile.estimated_rows = 65536;
    join_profile.memory_bytes_required = planning.memory_budget_bytes;
    join_profile.minimum_input_count = 2;
    join_profile.maximum_input_count = 2;
    join_profile.runtime_peak_from_callback_batches = true;
    profiles.push_back(std::move(join_profile));

    composition_state.batch.columns.clear();
    composition_state.result_bindings.clear();
    std::vector<const api::RelationalOutputRecord*> join_outputs;
    for (const auto& output : dag.outputs) {
      if (output.relation_node_id == mixed_join->node_id) {
        join_outputs.push_back(&output);
      }
    }
    std::ranges::sort(join_outputs, {},
                      &api::RelationalOutputRecord::ordinal);
    if (join_outputs.size() != mixed_join->output_descriptor_ids.size()) {
      return refuse("SB_MODEL_TYPED_EXCHANGE_INVALID_V1",
                    "key/value mixed JOIN result coverage is incomplete");
    }
    for (std::size_t ordinal = 0; ordinal < join_outputs.size(); ++ordinal) {
      const auto descriptor = descriptor_for(join_outputs[ordinal]->descriptor_id);
      if (descriptor == dag.descriptors.end() ||
          join_outputs[ordinal]->ordinal != ordinal ||
          join_outputs[ordinal]->descriptor_id !=
              mixed_join->output_descriptor_ids[ordinal]) {
        return refuse("SB_MODEL_TYPED_EXCHANGE_INVALID_V1",
                      "key/value mixed JOIN result descriptor is unresolved");
      }
      exec::CanonicalResultColumnBinding binding;
      binding.physical_column_ordinal = ordinal;
      binding.visible = join_outputs[ordinal]->visible;
      if (binding.visible) {
        const auto left_width = scan->output_descriptor_ids.size();
        const bool null_extended_output =
            ((*mixed_join_kind ==
                  exec::CanonicalAcceptedJoinKind::kRightOuter ||
              *mixed_join_kind ==
                  exec::CanonicalAcceptedJoinKind::kFullOuter) &&
             ordinal < left_width) ||
            ((*mixed_join_kind ==
                  exec::CanonicalAcceptedJoinKind::kLeftOuter ||
              *mixed_join_kind ==
                  exec::CanonicalAcceptedJoinKind::kFullOuter) &&
             ordinal >= left_width);
        binding.published_descriptor =
            exec::CanonicalResultColumnDescriptor{
                static_cast<std::uint32_t>(ordinal),
                join_outputs[ordinal]->output_name_utf8,
                descriptor->descriptor_uuid, descriptor->type_uuid,
                null_extended_output
                    ? exec::CanonicalResultNullability::kNullable
                    : ResultNullability(descriptor->nullability),
                descriptor->collation_uuid,
                descriptor->timezone_profile_id};
      }
      composition_state.result_bindings.push_back(std::move(binding));
    }
  }
  if (!CompleteLiveRuntimeMemoryReceipts(&profiles)) {
    return refuse("QOW-DIAG-OPT-017-REFUSAL-V1",
                  "key/value producer memory receipt is incomplete");
  }
  const auto physical = PlanAndPublishLivePhysicalDag(
      canonical_planning_request, profiles, "key-value.selected-plan",
      "key/value model source", "key_value.local.v1");
  const auto key_physical = std::ranges::find_if(
      physical.physical_dag.nodes, [&](const auto& node) {
        return node.relational_node_id == scan->node_id;
      });
  if (!physical.ok || physical.physical_dag.nodes.size() != profiles.size() ||
      physical.physical_dag.root_physical_node_id != dag.root_node_id ||
      key_physical == physical.physical_dag.nodes.end() ||
      key_physical->implementation_id !=
          "physical_key_value_scan_v1" ||
      key_physical->executor_capability_uuid !=
          planned.selected_candidate.capability_uuid ||
      key_physical->selected_alternative_uuid !=
          physical_alternative_uuid ||
      key_physical->cost_vector_uuid !=
          physical_cost_uuid) {
    return refuse(
        physical.diagnostic_id.empty()
            ? "QOW-DIAG-OPTIMIZER-PHYSICAL-PUBLICATION-V1"
            : physical.diagnostic_id,
        physical.detail.empty()
            ? "key/value canonical physical DAG was not published"
            : physical.detail);
  }
  result.optimizer_selected = true;
  result.physical_dag_published = true;
  result.optimizer_admission_stage_count =
      canonical_admission.admission.evidence.size();
  result.physical_node_count = physical.physical_dag.nodes.size();
  result.selected_plan_uuid = physical.physical_dag.selected_plan_uuid;

  const auto bounded_memory = static_cast<std::uint64_t>(
      std::min<std::uint64_t>(planning.memory_budget_bytes,
                              std::numeric_limits<std::size_t>::max()));
  const auto provider_memory = bounded_memory / 2;
  const auto exchange_memory = bounded_memory - provider_memory;
  const auto per_row_bytes = std::max<std::uint64_t>(
      1, sizeof(api::EngineBoundKeyValueRowV1) +
             3 * sizeof(api::EngineTypedValue));
  const auto maximum_rows = std::min<std::uint64_t>(
      65536, provider_memory / per_row_bytes);
  std::uint64_t maximum_cells = 0;
  if (provider_memory < 4096 || exchange_memory < 4096 ||
      maximum_rows == 0 ||
      !CheckedMultiply(maximum_rows, 3, &maximum_cells) ||
      maximum_cells > std::numeric_limits<std::size_t>::max()) {
    return refuse("SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                  "key/value route memory partition is incomplete");
  }

  api::EngineBoundKeyValueReadRequestV1 key_value_request;
  key_value_request.context = input.context;
  key_value_request.operation =
      exact_get ? api::EngineBoundKeyValueReadOperationV1::kGet
                : multi_get
                      ? api::EngineBoundKeyValueReadOperationV1::kMultiGet
                      : api::EngineBoundKeyValueReadOperationV1::kPrefixRange;
  key_value_request.object_uuid = object_uuid;
  key_value_request.request_values = request_values;
  key_value_request.statement_timestamp = input.context.statement_timestamp;
  key_value_request.expected_descriptor_uuid =
      persisted_relation.descriptor_uuid;
  key_value_request.expected_descriptor_generation =
      persisted_relation.descriptor_generation;
  key_value_request.selected_alternative_uuid =
      key_physical->selected_alternative_uuid;
  key_value_request.capability_uuid =
      key_physical->executor_capability_uuid;
  key_value_request.provider_uuid =
      planned.selected_candidate.provider_uuid;
  key_value_request.provider_generation =
      persisted_relation.descriptor_generation;
  key_value_request.maximum_request_keys =
      std::max<std::size_t>(1, request_values.size());
  key_value_request.maximum_request_bytes = provider_memory / 8;
  key_value_request.maximum_scanned_row_versions = 1'000'000;
  key_value_request.maximum_decoded_bytes = provider_memory / 2;
  key_value_request.maximum_output_rows = maximum_rows;
  key_value_request.maximum_value_bytes = provider_memory / 2;
  key_value_request.maximum_result_bytes = provider_memory / 2;
  key_value_request.maximum_memory_bytes = provider_memory;
  key_value_request.exact_fallback_selected =
      planned.exact_fallback_selected;
  key_value_request.cancellation_requested =
      input.context.query_cancellation_requested
          ? input.context.query_cancellation_requested
          : std::function<bool()>([] { return false; });

  std::vector<std::string> key_value_request_order;
  if (multi_get) {
    std::unordered_set<std::string> first_distinct_keys;
    key_value_request_order.reserve(request_values.size());
    for (const auto& value : request_values) {
      if (first_distinct_keys.insert(value.encoded_value).second) {
        key_value_request_order.push_back(value.encoded_value);
      }
    }
  }

  exec::ModelSourceInputDescriptorV1 source_input;
  source_input.family_id = "key_value";
  source_input.operation_id = planning.operation_id;
  source_input.object_uuid = object_uuid;
  source_input.key_value_request_order =
      std::move(key_value_request_order);
  if (multi_get) {
    source_input.maximum_key_value_request_count =
        key_value_request.maximum_request_keys;
    source_input.maximum_key_value_request_bytes =
        key_value_request.maximum_request_bytes;
  }
  source_input.physical_node_id =
      key_physical->physical_node_id;
  source_input.selected_alternative_uuid =
      key_physical->selected_alternative_uuid;
  source_input.capability_uuid =
      key_physical->executor_capability_uuid;
  source_input.provider_uuid = planned.selected_candidate.provider_uuid;
  source_input.provider_generation =
      persisted_relation.descriptor_generation;
  source_input.result_handle_uuid = result_handle_uuid;
  source_input.causal_counter_id =
      key_physical->causal_counter_id;
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
  source_input.maximum_rows = static_cast<std::size_t>(maximum_rows);
  source_input.maximum_cells = static_cast<std::size_t>(maximum_cells);
  source_input.maximum_memory_bytes = exchange_memory;
  source_input.exact_fallback_selected = planned.exact_fallback_selected;

  exec::ModelFamilyExecutionRequestV1 execution_request;
  execution_request.input = source_input;
  execution_request.capability.capability_uuid = source_input.capability_uuid;
  execution_request.capability.family_id = "key_value";
  execution_request.capability.provider_uuid = source_input.provider_uuid;
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
      key_value_request.cancellation_requested;
  execution_request.cleanup_provider = [] {};
  execution_request.exact_fallback_selected =
      source_input.exact_fallback_selected;
  execution_request.security_admitted = planning.security_admitted;
  execution_request.current_catalog_generation = generation;
  execution_request.current_descriptor_generation =
      persisted_relation.descriptor_generation;
  execution_request.current_security_generation = planning.security_epoch;
  execution_request.current_policy_generation = planning.policy_epoch;
  execution_request.current_resource_generation = planning.resource_epoch;
  execution_request.current_provider_generation =
      persisted_relation.descriptor_generation;
  execution_request.current_mga_statement_context = mga;
  execution_request.execute_provider =
      [key_value_request, source_input, public_columns, property_uuid,
       security_receipt_uuid](
          const exec::ModelSourceInputDescriptorV1& runtime_input) mutable {
        exec::ModelProviderExecutionResultV1 provider;
        if (runtime_input.key_value_request_order !=
                source_input.key_value_request_order ||
            runtime_input.maximum_key_value_request_count !=
                source_input.maximum_key_value_request_count ||
            runtime_input.maximum_key_value_request_bytes !=
                source_input.maximum_key_value_request_bytes) {
          provider.diagnostic_id = "SB_MODEL_TYPED_EXCHANGE_INVALID_V1";
          provider.detail =
              "key/value request-order binding was substituted";
          return provider;
        }
        const auto read = api::EngineBoundKeyValueReadV1(key_value_request);
        provider.data_access_observed = read.data_access_observed;
        provider.rows_examined = read.scanned_row_version_count;
        if (!read.ok) {
          provider.diagnostic_id =
              read.diagnostics.empty()
                  ? "SB_MODEL_COORDINATOR_LEG_FAILED_V1"
                  : read.diagnostics.front().code;
          provider.detail =
              read.diagnostics.empty()
                  ? "engine-bound key/value provider read failed"
                  : read.diagnostics.front().detail;
          return provider;
        }
        if (read.selected_alternative_uuid !=
                source_input.selected_alternative_uuid ||
            read.capability_uuid != source_input.capability_uuid ||
            read.provider_uuid != source_input.provider_uuid ||
            read.provider_generation != source_input.provider_generation ||
            read.descriptor_generation !=
                source_input.descriptor_generation ||
            read.exact_fallback_observed !=
                source_input.exact_fallback_selected) {
          provider.diagnostic_id = "SB_MODEL_TYPED_EXCHANGE_INVALID_V1";
          provider.detail =
              "key/value provider selection or generation receipt changed";
          return provider;
        }
        auto& provider_batch = provider.provider_batch;
        provider_batch.provider_uuid = source_input.provider_uuid;
        provider_batch.provider_generation = source_input.provider_generation;
        provider_batch.selected_alternative_uuid =
            source_input.selected_alternative_uuid;
        provider_batch.capability_uuid = source_input.capability_uuid;
        provider_batch.exact_fallback_selected =
            source_input.exact_fallback_selected;
        provider_batch.result_handle_uuid = source_input.result_handle_uuid;
        provider_batch.causal_counter_id = source_input.causal_counter_id;
        provider_batch.output_descriptor_ids =
            source_input.output_descriptor_ids;
        provider_batch.batch.columns = public_columns;
        provider_batch.mga_statement_context =
            source_input.mga_statement_context;
        provider_batch.security_receipt_uuid = security_receipt_uuid;
        provider_batch.properties.property_uuid = property_uuid;
        provider_batch.properties.ordering_id = read.ordering_id;
        provider_batch.properties.partitioning_id =
            "single_local_partition";
        provider_batch.properties.uniqueness_id = "key";
        provider_batch.properties.exact = true;
        provider_batch.properties.residual_recheck_complete =
            read.residual_recheck_complete;
        provider_batch.properties.base_row_mga_recheck_complete =
            read.base_row_mga_recheck_complete;
        provider_batch.properties.security_recheck_complete =
            read.security_recheck_complete;
        provider_batch.residual_recheck_complete =
            read.residual_recheck_complete;
        provider_batch.base_row_mga_recheck_complete =
            read.base_row_mga_recheck_complete;
        provider_batch.security_recheck_complete =
            read.security_recheck_complete;
        for (const auto& row : read.rows) {
          exec::DescriptorTuple tuple;
          const std::array<std::string, 3> encoded{
              row.row_uuid, row.key, row.value};
          for (std::size_t ordinal = 0; ordinal < encoded.size(); ++ordinal) {
            api::EngineTypedValue value;
            value.descriptor = public_columns[ordinal].descriptor;
            value.encoded_value = encoded[ordinal];
            value.setState(api::EngineValueState::value);
            tuple.values.push_back(std::move(value));
          }
          provider_batch.batch.rows.push_back(std::move(tuple));
          exec::ModelProviderRowIdentityV1 identity;
          identity.row_uuid = row.row_uuid;
          identity.key = row.key;
          provider_batch.ordered_row_identities.push_back(std::move(identity));
        }
        provider.ok = true;
        return provider;
      };

  CaptureRcp079ModelLegV1(
      leg_capture, scan->node_id, "key_value", "physical_key_value_scan_v1",
      "canonical.key-value.scan.v1", "key_value.local.v1",
      persisted_relation.descriptor_uuid,
      persisted_relation.descriptor_generation,
      plan::CanonicalLogicalRelationalNodeKind::kRelationSource,
      exec::PhysicalNodeKind::kScan, execution_request);
  if (leg_capture != nullptr) return result;
  exec::CanonicalPhysicalExecutorRegistration registration;
  registration.node_kind = exec::PhysicalNodeKind::kScan;
  registration.implementation_id = "physical_key_value_scan_v1";
  registration.executor_capability_uuid =
      key_physical->executor_capability_uuid;
  registration.executor_capability_abi_version = 1;
  registration.engine_owned = true;
  registration.accepts_optimizer_publication_v2 = true;
  registration.honors_dispatcher_memory_limit_v1 = true;
  registration.execute =
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
                "physical_key_value_scan_v1" ||
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
              "selected key/value physical node identity was substituted";
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
              "key/value callback memory allowance is absent or invalid";
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
            !executed.cleanup_complete || executed.cleanup_count != 1 ||
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
                selected_node.output_descriptor_ids ||
            executed.output.exact_fallback_selected !=
                execution_request.exact_fallback_selected) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              executed.diagnostic_id.empty()
                  ? "SB_MODEL_COORDINATOR_LEG_FAILED_V1"
                  : executed.diagnostic_id;
          step.diagnostic.detail =
              executed.detail.empty()
                  ? "key/value source execution did not complete"
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

  std::size_t maximum_composition_columns = outputs.size();
  for (const auto& node : dag.nodes) {
    maximum_composition_columns = std::max(
        maximum_composition_columns, node.output_descriptor_ids.size());
  }
  const auto bounded_size = [](const std::uint64_t value) {
    return static_cast<std::size_t>(std::min<std::uint64_t>(
        value, std::numeric_limits<std::size_t>::max()));
  };
  const auto optimizer_row_bound =
      bounded_size(input.context.optimizer_maximum_candidate_count);
  std::size_t execution_row_bound = source_input.maximum_rows;
  if (prepared_key_value_set.has_value()) {
    execution_row_bound = std::max(
        execution_row_bound, kKeyValueCompositionRowBound);
  }
  if (prepared_recursive_cte.has_value()) {
    execution_row_bound = std::max(
        execution_row_bound,
        prepared_recursive_cte->maximum_result_row_count);
  }
  if (mixed_join != nullptr) {
    execution_row_bound = std::max(execution_row_bound,
                                   optimizer_row_bound);
  }
  std::uint64_t maximum_composition_cells = 0;
  if (execution_row_bound == 0 || maximum_composition_columns == 0 ||
      !CheckedMultiply(execution_row_bound, maximum_composition_columns,
                       &maximum_composition_cells) ||
      maximum_composition_cells >
          std::numeric_limits<std::size_t>::max()) {
    return refuse("SBLR.PLAN_TREE.RESOURCE_LIMIT",
                  "key/value composition cell bound overflowed");
  }

  exec::CanonicalHeapPhysicalRegistrationResult mixed_heap_registration;
  std::size_t mixed_join_pair_bound = 0;
  if (mixed_join != nullptr) {
    const auto maximum_scanned_row_versions = bounded_size(std::min(
        input.context.optimizer_maximum_search_steps,
        input.context.optimizer_maximum_candidate_count));
    const auto maximum_decoded_bytes =
        bounded_size(input.context.optimizer_memory_budget_bytes);
    std::uint64_t pair_bound = 0;
    if (maximum_scanned_row_versions == 0 || maximum_decoded_bytes == 0 ||
        optimizer_row_bound == 0 ||
        !CheckedMultiply(source_input.maximum_rows, optimizer_row_bound,
                         &pair_bound) ||
        pair_bound > std::numeric_limits<std::size_t>::max()) {
      return refuse("SBLR.PLAN_TREE.RESOURCE_LIMIT",
                    "key/value mixed JOIN bounds are absent or overflow");
    }
    mixed_join_pair_bound = static_cast<std::size_t>(pair_bound);
    auto heap_relational_dag = dag;
    heap_relational_dag.statement_timestamp.clear();
    heap_relational_dag.root_node_id = relational_scan->node_id;
    std::erase_if(heap_relational_dag.nodes, [&](const auto& node) {
      return node.node_id != relational_scan->node_id;
    });
    std::erase_if(heap_relational_dag.descriptors, [&](const auto& descriptor) {
      return std::ranges::find(relational_scan->output_descriptor_ids,
                               descriptor.descriptor_id) ==
             relational_scan->output_descriptor_ids.end();
    });
    std::erase_if(heap_relational_dag.expressions, [&](const auto& expression) {
      return std::ranges::find(relational_scan->bound_expression_ids,
                               expression.expression_id) ==
             relational_scan->bound_expression_ids.end();
    });
    std::erase_if(heap_relational_dag.outputs, [&](const auto& output) {
      return output.relation_node_id != relational_scan->node_id;
    });
    exec::CanonicalHeapPhysicalDagDispatchRequest heap_request;
    heap_request.context = &input.context;
    heap_request.relational_dag = &heap_relational_dag;
    heap_request.physical_dag = physical.physical_dag;
    std::erase_if(heap_request.physical_dag.nodes, [&](const auto& node) {
      return node.relational_node_id != relational_scan->node_id;
    });
    if (heap_request.physical_dag.nodes.size() != 1) {
      return refuse("SB_MODEL_KEY_VALUE_MIXED_JOIN_HEAP_UNAVAILABLE_V1",
                    "key/value mixed JOIN selected heap node is absent");
    }
    heap_request.physical_dag.root_physical_node_id =
        heap_request.physical_dag.nodes.front().physical_node_id;
    heap_request.physical_dag.mga_statement_context.statement_timestamp.clear();
    for (auto& node : heap_request.physical_dag.nodes) {
      node.mga_statement_context.statement_timestamp.clear();
    }
    heap_request.maximum_scanned_row_versions =
        maximum_scanned_row_versions;
    heap_request.maximum_decoded_bytes = maximum_decoded_bytes;
    heap_request.maximum_output_rows = optimizer_row_bound;
    heap_request.maximum_output_columns = maximum_composition_columns;
    heap_request.maximum_output_cells =
        static_cast<std::size_t>(maximum_composition_cells);
    heap_request.cancellation_requested =
        input.context.query_cancellation_requested
            ? input.context.query_cancellation_requested
            : std::function<bool()>([] { return false; });
    mixed_heap_registration =
        exec::BuildCanonicalHeapPhysicalRegistration(heap_request);
    if (!mixed_heap_registration.diagnostic.ok ||
        !mixed_heap_registration.registration.has_value()) {
      return refuse(
          mixed_heap_registration.diagnostic.diagnostic_code.empty()
              ? "SB_MODEL_KEY_VALUE_MIXED_JOIN_HEAP_UNAVAILABLE_V1"
              : mixed_heap_registration.diagnostic.diagnostic_code,
          mixed_heap_registration.diagnostic.detail.empty()
              ? "key/value mixed JOIN heap executor is unavailable"
              : mixed_heap_registration.diagnostic.detail);
    }
    auto registration = std::move(*mixed_heap_registration.registration);
    auto execute_heap = std::move(registration.execute);
    registration.execute =
        [execute_heap = std::move(execute_heap)](
            const exec::TypedPhysicalNodeDag& selected_dag,
            const exec::PhysicalNodeRecord& selected_node,
            const std::vector<exec::CanonicalPhysicalDispatchInput>& inputs)
            mutable {
          auto heap_dag = selected_dag;
          std::erase_if(heap_dag.nodes, [&](const auto& node) {
            return node.physical_node_id != selected_node.physical_node_id;
          });
          if (heap_dag.nodes.size() != 1) {
            exec::CanonicalPhysicalDispatchStepResult step;
            step.diagnostic.ok = false;
            step.diagnostic.diagnostic_code =
                "SB_MODEL_MGA_CONTEXT_MISMATCH_V1";
            step.diagnostic.detail =
                "key/value mixed JOIN heap node identity changed";
            return step;
          }
          heap_dag.root_physical_node_id =
              heap_dag.nodes.front().physical_node_id;
          heap_dag.mga_statement_context.statement_timestamp.clear();
          for (auto& node : heap_dag.nodes) {
            node.mga_statement_context.statement_timestamp.clear();
          }
          const auto heap_node = std::ranges::find_if(
              heap_dag.nodes, [&](const auto& node) {
                return node.physical_node_id ==
                       selected_node.physical_node_id;
              });
          if (heap_node == heap_dag.nodes.end()) {
            exec::CanonicalPhysicalDispatchStepResult step;
            step.diagnostic.ok = false;
            step.diagnostic.diagnostic_code =
                "SB_MODEL_MGA_CONTEXT_MISMATCH_V1";
            step.diagnostic.detail =
                "key/value mixed JOIN heap node identity changed";
            return step;
          }
          heap_node->dispatcher_callback_memory_limit_bytes =
              selected_node.dispatcher_callback_memory_limit_bytes;
          auto step = execute_heap(heap_dag, *heap_node, inputs);
          if (step.diagnostic.ok) {
            step.mga_statement_context = selected_dag.mga_statement_context;
          }
          return step;
        };
    mixed_heap_registration.registration = std::move(registration);
  }

  api::CanonicalOptimizerSelectedExecutionRequest selected;
  selected.selected_physical_dag = physical.physical_dag;
  selected.pre_access_statistics_snapshot_uuid =
      physical.physical_dag.statistics_snapshot_uuid;
  selected.mga_authority =
      BuildCanonicalExecutionMgaAuthority(input.context,
                                          physical.physical_dag);
  selected.runtime_limits.maximum_rows_per_batch = execution_row_bound;
  selected.runtime_limits.maximum_columns_per_batch =
      maximum_composition_columns;
  selected.runtime_limits.maximum_cells_per_batch =
      static_cast<std::size_t>(maximum_composition_cells);
  selected.runtime_limits.maximum_total_materialized_rows =
      execution_row_bound;
  selected.runtime_limits.maximum_total_materialized_cells =
      static_cast<std::size_t>(maximum_composition_cells);
  selected.cancellation_requested = key_value_request.cancellation_requested;
  selected.available_executors.push_back(std::move(registration));
  if (prepared_key_value_set.has_value()) {
    std::unordered_map<std::uint64_t, exec::DescriptorBatch> values_batches;
    values_batches.emplace(
        set_values->node_id,
        std::move(materialized_key_value_set_values.batch));
    selected.available_executors.push_back(MakeLiveValuesRegistration(
        std::move(values_batches), set_values_capability_uuid,
        "QOW-DIAG-RELATIONAL-LIVE-SET-VALUES-V1",
        "key/value UNION ALL", true));
    std::unordered_map<std::uint64_t, PreparedLiveSetNode>
        prepared_set_nodes;
    prepared_set_nodes.emplace(set_root->node_id,
                               *prepared_key_value_set);
    selected.available_executors.push_back(
        MakeLiveSetOperationRegistration(
            MakeLiveSetRegistrationProfilesForComposition(prepared_set_nodes),
            key_value_set_profile.implementation_id,
            set_root_capability_uuid, input.context));
  }
  if (mixed_join != nullptr) {
    selected.available_executors.push_back(
        std::move(*mixed_heap_registration.registration));
    selected.available_executors.push_back(MakeLiveJoinRegistration(
        "join." + mixed_join_component + ".3vl.nested.v1",
        mixed_join_capability_uuid, {}, mixed_join_pair_bound,
        optimizer_row_bound, *mixed_join_kind,
        "key/value mixed " + mixed_join_operation, input.context, true,
        *mixed_join_kind == exec::CanonicalAcceptedJoinKind::kCross
            ? 0
            : mixed_join->bound_expression_ids.front(),
        mixed_join_predicate_binding, dag,
        canonical_planning_request.expression_services, {}, {}, &dag,
        &input.context, &selected.mga_authority));
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
    selected.available_executors.push_back(MakeLiveProjectRegistration(
        MakeLiveProjectRegistrationProfileForComposition(*prepared_project),
        "project.typed.expression-row.v1",
        project_capability_uuid, source_input.maximum_rows, {},
        canonical_planning_request.expression_services, {}, true, &dag,
        &input.context, &selected.mga_authority));
  }
  if (prepared_sort.has_value()) {
    std::uint64_t comparison_bound = 0;
    if (!CheckedMultiply(source_input.maximum_rows,
                         source_input.maximum_rows, &comparison_bound) ||
        comparison_bound > std::numeric_limits<std::size_t>::max()) {
      return refuse("SBLR.PLAN_TREE.RESOURCE_LIMIT",
                    "key/value SORT comparison bound overflowed");
    }
    const auto tie_uuid = DerivedCanonicalUuid(
        identity_scope + ":" + prepared_sort->ordering_property_uuid,
        "key-value.sort.deterministic-tie");
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
      selected.available_executors.push_back(MakeLiveSortRegistration(
          prepared_sort->order_terms, tie_uuid, sort_capability_uuid,
          source_input.maximum_rows,
          std::max<std::size_t>(
              1, static_cast<std::size_t>(comparison_bound)),
          {}, &input.context, &selected.mga_authority));
    }
  }
  if (prepared_row_number.has_value()) {
    selected.available_executors.push_back(MakeLiveRowNumberRegistration(
        *prepared_row_number, window_order_evidence_uuid,
        window_capability_uuid, source_input.maximum_rows, {},
        &input.context, &selected.mga_authority));
  }
  if (prepared_nonrecursive_cte) {
    selected.available_executors.push_back(
        MakeLiveNonrecursiveCteRegistration(
            cte_implementation_id, cte_capability_uuid,
            source_input.maximum_rows, {}, &input.context,
            &selected.mga_authority));
  }
  if (prepared_count_star.has_value()) {
    selected.available_executors.push_back(MakeLiveCountStarRegistration(
        prepared_count_star->result_column, aggregate_capability_uuid,
        source_input.maximum_rows, {}, &input.context,
        &selected.mga_authority));
  }
  if (prepared_recursive_cte.has_value()) {
    selected.available_executors.push_back(
        MakeLiveRecursiveCteTermRegistration(
            prepared_recursive_cte->term,
            recursive_term_capability_uuid, input.context));
    selected.available_executors.push_back(MakeLiveRecursiveCteRegistration(
        *prepared_recursive_cte, recursive_term_capability_uuid,
        recursive_root_capability_uuid,
        input.context));
  }
  if (prepared_limit.has_value()) {
    selected.available_executors.push_back(MakeLiveLimitRegistration(
        limit_implementation_id, limit_capability_uuid, limit_count,
        limit_offset, fetch_first_rows_only, source_input.maximum_rows,
        {}, &input.context, &selected.mga_authority));
  }
  selected.engine_execution_authorized = true;
  selected.result_publication_request.statement_uuid =
      input.context.statement_uuid;
  selected.result_publication_request.invocation_mode =
      exec::CanonicalResultInvocationMode::kDirect;
  selected.result_publication_request.execution_attempt_uuid =
      DerivedCanonicalUuid(identity_scope + ":" +
                               input.context.current_monotonic_ns,
                           "key-value.execution-attempt");
  selected.result_publication_request.result_kind =
      exec::CanonicalResultKind::kRows;
  selected.result_publication_request.transaction_effect_evidence_uuid =
      DerivedCanonicalUuid(
          identity_scope + ":" +
              std::to_string(input.context.local_transaction_id) + ":" +
              std::to_string(
                  input.context.snapshot_visible_through_local_transaction_id),
          "key-value.transaction-effect-unchanged");
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
      execution.dispatch.selected_plan_uuid !=
          physical.physical_dag.selected_plan_uuid ||
      !execution.issues.empty()) {
    return refuse(
        execution.issues.empty()
            ? "SB_MODEL_COORDINATOR_LEG_FAILED_V1"
            : execution.issues.front().diagnostic_id,
        execution.issues.empty()
            ? "key/value selected physical execution did not complete"
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
       "SBSQL_KEY_VALUE_SOURCE_TO_SBLR_MODEL_SOURCE_TO_KEY_VALUE_SCAN_TO_TYPED_BATCH_V1"});
  result.api_result.evidence.push_back(
      {"canonical.model_search_family", "key_value.local.v1"});
  result.api_result.evidence.push_back(
      {"canonical.key_value_statement_timestamp",
       input.context.statement_timestamp});
  result.api_result.evidence.push_back(
      {"canonical.key_value_operation", planning.operation_id});
  result.api_result.evidence.push_back(
      {"canonical.key_value_properties",
       exact_get ? "key_value_unordered_v1|single_local_partition|key"
                 : multi_get
                       ? "first_distinct_request_order_v1|single_local_partition|key"
                       : "key_utf8_byte_ascending_v1|single_local_partition|key"});
  return result;
}

}  // namespace scratchbird::engine::sblr
