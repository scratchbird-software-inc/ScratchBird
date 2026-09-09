// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "canonical_query_vector_composition.hpp"

#include "canonical_query_descriptor_support.hpp"
#include "canonical_query_aggregate_registration.hpp"
#include "canonical_query_object_free_composition_support.hpp"
#include "canonical_query_object_free_profile.hpp"
#include "canonical_query_physical_registration.hpp"
#include "canonical_query_runtime_memory_support.hpp"
#include "canonical_query_scalar_support.hpp"

#include "catalog/name_resolution_api.hpp"
#include "engine/executor/executor_foundation.hpp"
#include "engine/internal_api/mga_relation_store/mga_relation_store.hpp"
#include "engine/optimizer/optimizer_contract.hpp"
#include "engine/optimizer/relational_planner.hpp"
#include "nosql/nosql_provider_generation_store.hpp"
#include "nosql/vector_api.hpp"
#include "query/canonical_heap_optimizer_admission.hpp"
#include "query/canonical_relational_bridge.hpp"
#include "security/security_model.hpp"
#include "transaction/transaction_api.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <limits>
#include <ranges>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace scratchbird::engine::sblr {
namespace api = scratchbird::engine::internal_api;
namespace exec = scratchbird::engine::executor;
namespace opt = scratchbird::engine::optimizer;
namespace plan = scratchbird::engine::planner;

// SEARCH_KEY: SB_ENGINE_CANONICAL_QUERY_VECTOR_COMPOSITION_AUTHORITY
// Owns one admitted production vector source route. It consumes and revalidates
// engine-issued MGA statement authority and cannot create snapshots or publish
// transaction finality.

// QOW-SOURCE-RCP-077-VECTOR-CANONICAL-QUERY-ROUTE-V1
CanonicalObjectFreeValuesExecutionResult ExecuteCanonicalVectorFamilyQuery(
    const CanonicalCurrentHeapExecutionRequest& input,
    Rcp079CapturedModelLegV1* leg_capture) {
  CanonicalObjectFreeValuesExecutionResult result;
  const auto& dag = input.relational_dag;
  const auto source = std::ranges::find_if(dag.nodes, [](const auto& node) {
    return node.node_kind == api::RelationalDagNodeKind::kScan &&
           node.semantic_variant_id == "SBLR_MODEL_SOURCE_V1";
  });
  const auto nearest = std::ranges::find_if(
      dag.expressions, [](const auto& expression) {
        return expression.operator_name == "VECTOR_NEAREST";
      });
  if (source == dag.nodes.end() || nearest == dag.expressions.end()) {
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
  const auto expression_for = [&](const std::uint32_t expression_id) {
    return std::ranges::find_if(dag.expressions, [&](const auto& expression) {
      return expression.expression_id == expression_id;
    });
  };
  const auto descriptor_for = [&](const std::uint32_t descriptor_id) {
    return std::ranges::find_if(dag.descriptors, [&](const auto& descriptor) {
      return descriptor.descriptor_id == descriptor_id;
    });
  };
  const auto filter = std::ranges::find_if(
      dag.expressions, [](const auto& expression) {
        return expression.operator_name == "VECTOR_FILTER";
      });
  const bool filtered = filter != dag.expressions.end();
  const auto nearest_count = std::ranges::count_if(
      dag.expressions, [](const auto& expression) {
        return expression.operator_name == "VECTOR_NEAREST";
      });
  const auto filter_count = std::ranges::count_if(
      dag.expressions, [](const auto& expression) {
        return expression.operator_name == "VECTOR_FILTER";
      });
  const auto source_count = std::ranges::count_if(
      dag.nodes, [](const auto& node) {
        return node.node_kind == api::RelationalDagNodeKind::kScan &&
               node.semantic_variant_id == "SBLR_MODEL_SOURCE_V1";
      });
  const std::size_t expected_expression_count = filtered ? 14 : 8;
  if (dag.wire_version != 2 || dag.nodes.size() != 1 || source_count != 1 ||
      dag.root_node_id != source->node_id || !source->input_node_ids.empty() ||
      source->required_object_uuids.size() != 1 ||
      source->output_descriptor_ids.size() != 3 ||
      source->bound_expression_ids.size() != expected_expression_count ||
      dag.expressions.size() != expected_expression_count ||
      nearest_count != 1 || filter_count != static_cast<std::size_t>(filtered) ||
      nearest->expression_kind !=
          api::RelationalExpressionKind::kFunctionCall ||
      nearest->function_uuid.has_value() || nearest->bound_name_uuid.has_value() ||
      nearest->literal_kind.has_value() ||
      nearest->literal_or_parameter_ref.has_value() ||
      nearest->child_expression_ids.size() != 4 ||
      dag.statement_timestamp.empty() ||
      dag.statement_timestamp != input.context.statement_timestamp) {
    return refuse("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                  "vector canonical source shape is incomplete");
  }
  std::unordered_set<std::uint32_t> bound_expression_ids(
      source->bound_expression_ids.begin(), source->bound_expression_ids.end());
  if (bound_expression_ids.size() != dag.expressions.size() ||
      std::ranges::any_of(dag.expressions, [&](const auto& expression) {
        return expression.expression_id == 0 ||
               !bound_expression_ids.contains(expression.expression_id);
      })) {
    return refuse("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                  "vector bound-expression coverage is incomplete");
  }

  const auto alias = expression_for(nearest->child_expression_ids[0]);
  const auto query = expression_for(nearest->child_expression_ids[1]);
  const auto metric = expression_for(nearest->child_expression_ids[2]);
  const auto top_k = expression_for(nearest->child_expression_ids[3]);
  const auto object_uuid = source->required_object_uuids.front();
  if (alias == dag.expressions.end() || query == dag.expressions.end() ||
      metric == dag.expressions.end() || top_k == dag.expressions.end() ||
      alias->expression_kind != api::RelationalExpressionKind::kIdentifier ||
      alias->bound_name_uuid != object_uuid ||
      !alias->child_expression_ids.empty() || alias->operator_name.has_value() ||
      alias->literal_kind.has_value() ||
      alias->literal_or_parameter_ref.has_value() ||
      query->expression_kind != api::RelationalExpressionKind::kLiteral ||
      query->literal_kind != api::RelationalLiteralKind::kVector ||
      !query->literal_or_parameter_ref.has_value() ||
      !query->child_expression_ids.empty() || query->function_uuid.has_value() ||
      query->bound_name_uuid.has_value() || query->operator_name.has_value() ||
      metric->expression_kind != api::RelationalExpressionKind::kLiteral ||
      metric->literal_kind != api::RelationalLiteralKind::kString ||
      !metric->literal_or_parameter_ref.has_value() ||
      !metric->child_expression_ids.empty() ||
      top_k->expression_kind != api::RelationalExpressionKind::kLiteral ||
      top_k->literal_kind != api::RelationalLiteralKind::kNumeric ||
      !top_k->literal_or_parameter_ref.has_value() ||
      !top_k->child_expression_ids.empty()) {
    return refuse("SB_MODEL_VECTOR_NEAREST_REFUSED_V1",
                  "vector nearest operands are not exact bound literals");
  }
  api::EngineBoundVectorMetricV1 bound_metric =
      api::EngineBoundVectorMetricV1::kUnknown;
  if (*metric->literal_or_parameter_ref == "L2_SQUARED") {
    bound_metric = api::EngineBoundVectorMetricV1::kL2Squared;
  } else if (*metric->literal_or_parameter_ref == "COSINE") {
    bound_metric = api::EngineBoundVectorMetricV1::kCosine;
  } else if (*metric->literal_or_parameter_ref == "INNER_PRODUCT") {
    bound_metric = api::EngineBoundVectorMetricV1::kInnerProduct;
  } else {
    return refuse("SB_MODEL_VECTOR_METRIC_REFUSED_V1",
                  "vector metric is outside the closed v1 set");
  }
  std::uint32_t top_k_value = 0;
  const auto top_k_text = *top_k->literal_or_parameter_ref;
  const auto top_k_parse = std::from_chars(
      top_k_text.data(), top_k_text.data() + top_k_text.size(), top_k_value);
  if (top_k_text.empty() || (top_k_text.size() > 1 && top_k_text[0] == '0') ||
      top_k_parse.ec != std::errc{} ||
      top_k_parse.ptr != top_k_text.data() + top_k_text.size() ||
      top_k_value == 0) {
    return refuse("SB_MODEL_VECTOR_TOP_K_REFUSED_V1",
                  "vector top-k is not a canonical positive UINT32");
  }

  const api::RelationalExpressionRecord* metadata_column = nullptr;
  const api::RelationalExpressionRecord* metadata_value = nullptr;
  if (filtered) {
    if (filter->expression_kind !=
            api::RelationalExpressionKind::kFunctionCall ||
        filter->function_uuid.has_value() || filter->bound_name_uuid.has_value() ||
        filter->literal_kind.has_value() ||
        filter->literal_or_parameter_ref.has_value() ||
        filter->child_expression_ids.size() != 2) {
      return refuse("SB_MODEL_VECTOR_FILTER_REFUSED_V1",
                    "vector filter function shape is incomplete");
    }
    const auto filter_alias = expression_for(filter->child_expression_ids[0]);
    const auto predicate = expression_for(filter->child_expression_ids[1]);
    if (filter_alias == dag.expressions.end() ||
        predicate == dag.expressions.end() ||
        filter_alias->expression_kind !=
            api::RelationalExpressionKind::kIdentifier ||
        filter_alias->bound_name_uuid != object_uuid ||
        predicate->expression_kind != api::RelationalExpressionKind::kBinary ||
        predicate->operator_name != "=" ||
        predicate->child_expression_ids.size() != 2) {
      return refuse("SB_MODEL_VECTOR_FILTER_REFUSED_V1",
                    "vector filter alias or equality identity is incomplete");
    }
    const auto column = expression_for(predicate->child_expression_ids[0]);
    const auto value = expression_for(predicate->child_expression_ids[1]);
    if (column == dag.expressions.end() || value == dag.expressions.end() ||
        column->expression_kind !=
            api::RelationalExpressionKind::kIdentifier ||
        !column->bound_name_uuid.has_value() ||
        value->expression_kind != api::RelationalExpressionKind::kLiteral ||
        value->literal_kind != api::RelationalLiteralKind::kString ||
        !value->literal_or_parameter_ref.has_value() ||
        !value->child_expression_ids.empty()) {
      return refuse("SB_MODEL_VECTOR_FILTER_REFUSED_V1",
                    "vector metadata equality is not a bound TEXT literal");
    }
    metadata_column = &*column;
    metadata_value = &*value;
  }

  std::vector<const api::RelationalOutputRecord*> outputs;
  for (const auto& output : dag.outputs) {
    if (output.relation_node_id == source->node_id) outputs.push_back(&output);
  }
  std::ranges::sort(outputs, {}, &api::RelationalOutputRecord::ordinal);
  static constexpr std::array<std::string_view, 3> kOutputNames{
      "row_uuid", "distance", "score"};
  static constexpr std::array<std::string_view, 3> kOutputTypes{
      "uuid", "real64", "real64"};
  if (outputs.size() != 3) {
    return refuse("SB_MODEL_TYPED_EXCHANGE_INVALID_V1",
                  "vector public output coverage is not exact");
  }
  std::unordered_set<std::string> public_descriptor_uuids;
  std::vector<exec::ExecutorColumnDescriptor> public_columns;
  std::vector<api::EngineDescriptor> output_descriptors;
  std::vector<exec::CanonicalResultColumnBinding> result_bindings;
  const auto uuid_type_uuid = ExactCanonicalCoreDatatypeUuidV1("uuid");
  const auto real64_type_uuid = ExactCanonicalCoreDatatypeUuidV1("real64");
  if (uuid_type_uuid.empty() || real64_type_uuid.empty()) {
    return refuse("SB_MODEL_RESULT_DESCRIPTOR_SOURCE_BINDING_INVALID_V1",
                  "vector public core type registry is unavailable");
  }
  for (std::size_t ordinal = 0; ordinal < outputs.size(); ++ordinal) {
    const auto descriptor = descriptor_for(outputs[ordinal]->descriptor_id);
    if (!outputs[ordinal]->visible || outputs[ordinal]->ordinal != ordinal ||
        outputs[ordinal]->output_name_utf8 != kOutputNames[ordinal] ||
        outputs[ordinal]->descriptor_id !=
            source->output_descriptor_ids[ordinal] ||
        descriptor == dag.descriptors.end() ||
        descriptor->nullability != api::RelationalNullability::kNonNull ||
        !CanonicalUuidText(descriptor->descriptor_uuid) ||
        !CanonicalUuidText(descriptor->type_uuid) ||
        !public_descriptor_uuids.insert(descriptor->descriptor_uuid).second ||
        descriptor->collation_uuid.has_value() ||
        descriptor->timezone_profile_id.has_value() ||
        descriptor->width.has_value() || descriptor->precision.has_value() ||
        descriptor->scale.has_value()) {
      return refuse("SB_MODEL_TYPED_EXCHANGE_INVALID_V1",
                    "vector public output binding was substituted");
    }
    const auto& expected_type_uuid =
        ordinal == 0 ? uuid_type_uuid : real64_type_uuid;
    if (descriptor->type_uuid != expected_type_uuid) {
      return refuse("SB_MODEL_RESULT_DESCRIPTOR_SOURCE_BINDING_INVALID_V1",
                    "vector public type identity was substituted");
    }
    api::EngineDescriptor engine_descriptor;
    engine_descriptor.descriptor_uuid.canonical = descriptor->descriptor_uuid;
    engine_descriptor.descriptor_kind = "scalar";
    engine_descriptor.canonical_type_name = std::string(kOutputTypes[ordinal]);
    engine_descriptor.encoded_descriptor =
        "type_uuid=" + descriptor->type_uuid + ";nullability=non_null";
    output_descriptors.push_back(engine_descriptor);
    public_columns.push_back(
        {std::string(kOutputNames[ordinal]), engine_descriptor, false,
         descriptor->descriptor_id});
    exec::CanonicalResultColumnBinding binding;
    binding.physical_column_ordinal = ordinal;
    binding.visible = true;
    binding.published_descriptor = exec::CanonicalResultColumnDescriptor{
        static_cast<std::uint32_t>(ordinal), outputs[ordinal]->output_name_utf8,
        descriptor->descriptor_uuid, descriptor->type_uuid,
        exec::CanonicalResultNullability::kNonNull, std::nullopt, std::nullopt};
    result_bindings.push_back(std::move(binding));
  }
  if (output_descriptors[1].encoded_descriptor !=
      output_descriptors[2].encoded_descriptor) {
    return refuse("SB_MODEL_TYPED_EXCHANGE_INVALID_V1",
                  "vector distance and score type identities differ");
  }
  exec::DescriptorBatch public_descriptor_batch;
  public_descriptor_batch.columns = public_columns;
  const auto descriptor_validation = exec::ValidateCanonicalDescriptorBatch(
      public_descriptor_batch, source->output_descriptor_ids);
  if (!descriptor_validation.ok) {
    return refuse("SB_MODEL_TYPED_EXCHANGE_INVALID_V1",
                  descriptor_validation.detail);
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
    return refuse("SB_MODEL_MGA_CONTEXT_MISMATCH_V1",
                  "engine-issued vector statement timestamp is invalid");
  }
  const auto authorization = api::EvaluateMaterializedAuthorization(
      input.context, input.context.authorization_context, "SELECT", object_uuid);
  if (!authorization.authorized || authorization.denied ||
      authorization.policy_recheck_required ||
      !authorization.diagnostics.empty()) {
    return refuse("SB_MODEL_SECURITY_ADMISSION_REFUSED_V1",
                  "vector SELECT authorization was refused");
  }
  const auto loaded_relation =
      api::LoadMgaRelationStorageDescriptor(input.context, object_uuid);
  if (!loaded_relation.ok) {
    return refuse("SB_MODEL_VECTOR_EXACT_FALLBACK_UNAVAILABLE_V1",
                  loaded_relation.diagnostic.detail.empty()
                      ? "persistent vector relation descriptor is unavailable"
                      : loaded_relation.diagnostic.detail);
  }
  const auto& persisted_relation = loaded_relation.descriptor;
  if (persisted_relation.relation_uuid.canonical != object_uuid ||
      persisted_relation.database_uuid.canonical !=
          input.context.database_uuid.canonical ||
      persisted_relation.relation_kind != "table" ||
      persisted_relation.storage_profile != "local_mga_rowstore_v1" ||
      persisted_relation.descriptor_generation == 0) {
    return refuse("SB_MODEL_TYPED_EXCHANGE_INVALID_V1",
                  "persistent vector relation descriptor is invalid");
  }
  if (!api::ExactBoundVectorStorageDescriptorV1(persisted_relation,
                                                 object_uuid)) {
    return refuse("SB_MODEL_RESULT_DESCRIPTOR_SOURCE_BINDING_INVALID_V1",
                  "persistent vector relation type binding is not exact");
  }
  const auto query_descriptor = descriptor_for(query->result_descriptor_id);
  const auto metric_descriptor = descriptor_for(metric->result_descriptor_id);
  const auto top_k_descriptor = descriptor_for(top_k->result_descriptor_id);
  const auto stored_embedding_type =
      ExactCanonicalCoreDatatypeTypeUuidV1("dense_vector");
  const auto stored_metadata_type =
      ExactCanonicalCoreDatatypeTypeUuidV1("character");
  const auto top_k_type = ExactCanonicalCoreDatatypeTypeUuidV1("uint64");
  if (query_descriptor == dag.descriptors.end() ||
      metric_descriptor == dag.descriptors.end() ||
      top_k_descriptor == dag.descriptors.end() ||
      query_descriptor->descriptor_uuid !=
          persisted_relation.columns[0].value_descriptor.descriptor_uuid
              .canonical ||
      stored_embedding_type.empty() || stored_metadata_type.empty() ||
      top_k_type.empty() ||
      query_descriptor->type_uuid != stored_embedding_type ||
      metric_descriptor->type_uuid != stored_metadata_type ||
      top_k_descriptor->type_uuid != top_k_type ||
      query_descriptor->nullability !=
          api::RelationalNullability::kNonNull ||
      query_descriptor->width != std::optional<std::uint32_t>(3) ||
      query_descriptor->collation_uuid.has_value() ||
      query_descriptor->timezone_profile_id.has_value() ||
      query_descriptor->precision.has_value() ||
      query_descriptor->scale.has_value() ||
      metric_descriptor->nullability !=
          api::RelationalNullability::kNonNull ||
      metric_descriptor->collation_uuid.has_value() ||
      metric_descriptor->timezone_profile_id.has_value() ||
      metric_descriptor->width.has_value() ||
      metric_descriptor->precision.has_value() ||
      metric_descriptor->scale.has_value() ||
      top_k_descriptor->nullability !=
          api::RelationalNullability::kNonNull ||
      top_k_descriptor->collation_uuid.has_value() ||
      top_k_descriptor->timezone_profile_id.has_value() ||
      top_k_descriptor->width.has_value() ||
      top_k_descriptor->precision.has_value() ||
      top_k_descriptor->scale.has_value()) {
    return refuse("SB_MODEL_VECTOR_VALUE_REFUSED_V1",
                  "vector operation descriptor cohort differs from storage");
  }
  if (filtered) {
    const auto value_descriptor = descriptor_for(metadata_value->result_descriptor_id);
    if (value_descriptor == dag.descriptors.end() ||
        metadata_column->bound_name_uuid !=
            persisted_relation.columns[1].column_uuid.canonical ||
        metadata_column->result_descriptor_id != value_descriptor->descriptor_id ||
        value_descriptor->descriptor_uuid !=
            persisted_relation.columns[1].value_descriptor.descriptor_uuid
                .canonical ||
        value_descriptor->type_uuid != stored_metadata_type ||
        value_descriptor->nullability !=
            api::RelationalNullability::kNonNull ||
        value_descriptor->collation_uuid.has_value() ||
        value_descriptor->timezone_profile_id.has_value() ||
        value_descriptor->width.has_value() ||
        value_descriptor->precision.has_value() ||
        value_descriptor->scale.has_value()) {
      return refuse("SB_MODEL_VECTOR_FILTER_REFUSED_V1",
                    "vector metadata filter descriptor differs from storage");
    }
  }

  const auto identity_scope =
      dag.bound_sblr_tree_uuid + ":" + input.context.statement_uuid.canonical;
  const auto provider_uuid =
      DerivedCanonicalUuid(identity_scope, "vector.provider");
  const auto capability_uuid =
      DerivedCanonicalUuid(identity_scope, "vector.capability");
  const auto result_handle_uuid =
      DerivedCanonicalUuid(identity_scope, "vector.result-handle");
  const auto property_uuid =
      DerivedCanonicalUuid(identity_scope, "vector.property");
  const auto security_receipt_uuid =
      DerivedCanonicalUuid(identity_scope, "vector.security-receipt");
  const auto policy_snapshot_uuid =
      DerivedCanonicalUuid(identity_scope, "vector.policy-snapshot");
  const auto statistics_snapshot_uuid =
      DerivedCanonicalUuid(identity_scope, "vector.statistics-snapshot");
  const auto resource_contract_uuid =
      DerivedCanonicalUuid(identity_scope, "vector.resource-contract");
  const auto suffix = std::to_string(source->node_id) +
                      ".physical_vector_search_v1";
  const auto alternative_uuid =
      DerivedCanonicalUuid(identity_scope, "alternative." + suffix);
  const auto cost_uuid =
      DerivedCanonicalUuid(identity_scope, "cost-vector." + suffix);
  const auto generation =
      std::max<std::uint64_t>(1, input.context.catalog_generation_id);

  opt::ModelFamilyCoordinatorRequestV1 planning;
  planning.family_id = "vector";
  planning.operation_id = filtered ? "VECTOR_FILTERED_SEARCH"
                                   : "VECTOR_EXACT_SEARCH";
  planning.logical_operator_id = "LOGICAL_VECTOR_SOURCE_V1";
  planning.logical_node_id = source->node_id;
  planning.object_uuid = object_uuid;
  planning.output_descriptor_ids = source->output_descriptor_ids;
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
      planning, identity_scope + ".vector.native",
      opt::ModelFamilyAlternativeRouteClassV1::kNative, provider_uuid,
      capability_uuid, persisted_relation.descriptor_generation, true,
      std::max<std::uint64_t>(1, top_k_value), 1,
      std::max<std::uint64_t>(1, planning.memory_budget_bytes / 2)));
  const auto planned = PlanCanonicalModelFamilySourceForCompositionV1(
      planning, identity_scope + ".vector.inventory", std::move(alternatives));
  if (!planned.accepted || !planned.selected ||
      !planned.data_access_allowed || !planned.optimizer_owned_enumeration ||
      planned.exact_fallback_selected ||
      planned.selected_candidate.provider_uuid != provider_uuid ||
      planned.selected_candidate.capability_uuid != capability_uuid ||
      planned.selected_candidate.provider_generation !=
          persisted_relation.descriptor_generation ||
      planned.physical_dag.nodes.size() != 1 ||
      planned.physical_dag.nodes.front().implementation_id !=
          "physical_vector_search_v1") {
    return refuse(
        planned.diagnostic_id.empty()
            ? "SB_MODEL_VECTOR_EXACT_FALLBACK_UNAVAILABLE_V1"
            : planned.diagnostic_id,
        planned.detail.empty()
            ? "vector coordinator did not select the exact engine candidate"
            : planned.detail);
  }

  api::CanonicalRelationalPlanningScope planning_scope;
  planning_scope.catalog_epoch_uuid =
      input.context.catalog_epoch_uuid.canonical;
  planning_scope.security_context_uuid =
      input.context.authorization_context.authority_uuid.canonical;
  planning_scope.statement_uuid = input.context.statement_uuid.canonical;
  planning_scope.statement_timestamp = input.context.statement_timestamp;
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
  if (!logical.accepted || logical.logical_graph.nodes.size() != 1 ||
      logical.logical_graph.nodes.front().logical_node_id != source->node_id ||
      logical.logical_graph.nodes.front().semantic_variant_id !=
          "SBLR_MODEL_SOURCE_V1" ||
      logical.logical_graph.nodes.front().model_family_identity !=
          plan::CanonicalLogicalModelFamilyIdentity::kVector) {
    return refuse(logical.issues.empty()
                      ? "QOW-DIAG-OPTIMIZER-ADMISSION-BOUND-REQUEST-V1"
                      : logical.issues.front().diagnostic_id,
                  logical.issues.empty()
                      ? "vector logical bridge was refused"
                      : logical.issues.front().field_id);
  }
  plan::CanonicalMgaStatementContext current_logical_mga;
  current_logical_mga.statement_uuid = mga.statement_uuid;
  current_logical_mga.statement_timestamp = mga.statement_timestamp;
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
    return refuse("SB_MODEL_MGA_CONTEXT_MISMATCH_V1",
                  "vector logical bridge MGA cohort changed");
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
    return refuse("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                  "vector optimizer monotonic context is invalid");
  }
  admission_context.admitted_at_monotonic_ns = admitted_at_monotonic_ns;
  admission_context.metadata_snapshot_engine_owned = true;
  admission_context.authorization_context_engine_owned = true;
  admission_context.catalog_object_uuids = {object_uuid};
  admission_context.authorized_object_uuids = {object_uuid};
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
    return refuse(
        canonical_admission.diagnostic_id.empty()
            ? "QOW-DIAG-OPTIMIZER-ADMISSION-BOUND-REQUEST-V1"
            : canonical_admission.diagnostic_id,
        canonical_admission.field_id.empty()
            ? "vector canonical optimizer admission was refused"
            : canonical_admission.field_id);
  }
  result.optimizer_admitted = true;
  CanonicalObjectFreeValuesExecutionRequest canonical_planning_request{
      input.context, dag, canonical_admission.request,
      canonical_admission.admission};

  std::vector<LivePhysicalNodeProfile> profiles;
  LivePhysicalNodeProfile profile;
  profile.logical_node_id = source->node_id;
  profile.implementation_id = "physical_vector_search_v1";
  profile.capability_uuid = capability_uuid;
  profile.logical_node_kind =
      plan::CanonicalLogicalRelationalNodeKind::kRelationSource;
  profile.physical_node_kind = exec::PhysicalNodeKind::kScan;
  profile.transformation_rule_id = "canonical.vector.search.v1";
  profile.estimated_rows = top_k_value;
  profile.memory_bytes_required =
      planned.selected_candidate.cost.memory_bytes_required;
  profile.page_read_sequential_units = 1;
  profile.mga_visibility_checks_expected = 1;
  profile.storage_read_capable = true;
  profile.mga_visibility_capable = true;
  profile.residual_predicate_required = true;
  profile.storage_recheck_required = true;
  profile.compatibility_profile_id = "vector.local.v1";
  profiles.push_back(std::move(profile));
  if (!CompleteLiveRuntimeMemoryReceipts(&profiles)) {
    return refuse("QOW-DIAG-OPT-017-REFUSAL-V1",
                  "vector producer memory receipt is incomplete");
  }
  const auto physical = PlanAndPublishLivePhysicalDag(
      canonical_planning_request, profiles, "vector.selected-plan",
      "vector model source", "vector.local.v1");
  if (!physical.ok || physical.physical_dag.nodes.size() != 1 ||
      physical.physical_dag.root_physical_node_id == 0 ||
      physical.physical_dag.nodes.front().relational_node_id !=
          source->node_id ||
      physical.physical_dag.nodes.front().implementation_id !=
          "physical_vector_search_v1" ||
      physical.physical_dag.nodes.front().executor_capability_uuid !=
          capability_uuid ||
      physical.physical_dag.nodes.front().selected_alternative_uuid !=
          alternative_uuid ||
      physical.physical_dag.nodes.front().cost_vector_uuid !=
          cost_uuid) {
    return refuse(
        physical.diagnostic_id.empty()
            ? "QOW-DIAG-OPTIMIZER-PHYSICAL-PUBLICATION-V1"
            : physical.diagnostic_id,
        physical.detail.empty()
            ? "vector canonical physical DAG was not published"
            : physical.detail);
  }
  const auto& vector_physical = physical.physical_dag.nodes.front();
  result.optimizer_selected = true;
  result.physical_dag_published = true;
  result.optimizer_admission_stage_count =
      canonical_admission.admission.evidence.size();
  result.physical_node_count = 1;
  result.selected_plan_uuid = physical.physical_dag.selected_plan_uuid;

  const auto provider_memory = planning.memory_budget_bytes / 2;
  const auto exchange_memory = planning.memory_budget_bytes - provider_memory;
  const auto per_row_bytes = std::max<std::uint64_t>(
      1, sizeof(api::EngineBoundVectorRowV1) +
             3 * sizeof(api::EngineTypedValue));
  const auto maximum_rows = std::min<std::uint64_t>(
      {65'536, input.context.optimizer_maximum_candidate_count,
       provider_memory / per_row_bytes});
  std::uint64_t maximum_cells = 0;
  if (provider_memory < 4096 || exchange_memory < 4096 ||
      maximum_rows < top_k_value ||
      !CheckedMultiply(maximum_rows, 3, &maximum_cells) ||
      maximum_cells > std::numeric_limits<std::size_t>::max()) {
    return refuse("SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                  "vector route memory or result bounds are incomplete");
  }
  api::EngineBoundVectorReadRequestV1 vector_request;
  vector_request.context = input.context;
  vector_request.collection_uuid = object_uuid;
  vector_request.expected_descriptor_uuid =
      persisted_relation.descriptor_uuid.canonical;
  vector_request.expected_descriptor_generation =
      persisted_relation.descriptor_generation;
  vector_request.selected_alternative_uuid = alternative_uuid;
  vector_request.selected_provider_uuid = provider_uuid;
  vector_request.selected_capability_uuid = capability_uuid;
  vector_request.selected_implementation_id = "physical_vector_search_v1";
  vector_request.operation =
      filtered ? api::EngineBoundVectorReadOperationV1::kFilteredSearch
               : api::EngineBoundVectorReadOperationV1::kExactSearch;
  vector_request.metric = bound_metric;
  vector_request.physical_route =
      api::EngineBoundVectorPhysicalRouteV1::kExactScan;
  vector_request.bound_query_vector_literal =
      *query->literal_or_parameter_ref;
  vector_request.top_k = top_k_value;
  vector_request.filter.present = filtered;
  if (filtered) {
    vector_request.filter.canonical_metadata_json =
        *metadata_value->literal_or_parameter_ref;
  }
  vector_request.output_descriptors = output_descriptors;
  vector_request.maximum_scanned_row_versions =
      input.context.optimizer_maximum_search_steps;
  vector_request.maximum_decoded_bytes = provider_memory / 2;
  vector_request.maximum_output_rows = maximum_rows;
  vector_request.maximum_memory_bytes = provider_memory;
  vector_request.cancellation_requested =
      input.context.query_cancellation_requested
          ? input.context.query_cancellation_requested
          : std::function<bool()>([] { return false; });

  exec::ModelSourceInputDescriptorV1 source_input;
  source_input.family_id = "vector";
  source_input.operation_id = planning.operation_id;
  source_input.object_uuid = object_uuid;
  source_input.physical_node_id = vector_physical.physical_node_id;
  source_input.selected_alternative_uuid = alternative_uuid;
  source_input.capability_uuid = capability_uuid;
  source_input.provider_uuid = provider_uuid;
  source_input.provider_generation = persisted_relation.descriptor_generation;
  source_input.result_handle_uuid = result_handle_uuid;
  source_input.causal_counter_id = vector_physical.causal_counter_id;
  source_input.output_descriptor_ids = source->output_descriptor_ids;
  source_input.mga_statement_context = mga;
  source_input.catalog_epoch_uuid =
      input.context.catalog_epoch_uuid.canonical;
  source_input.security_context_uuid =
      input.context.authorization_context.authority_uuid.canonical;
  source_input.policy_snapshot_uuid = policy_snapshot_uuid;
  source_input.resource_contract_uuid = resource_contract_uuid;
  source_input.catalog_generation = generation;
  source_input.descriptor_generation = persisted_relation.descriptor_generation;
  source_input.security_generation = planning.security_epoch;
  source_input.policy_generation = planning.policy_epoch;
  source_input.resource_generation = planning.resource_epoch;
  source_input.maximum_rows = static_cast<std::size_t>(maximum_rows);
  source_input.maximum_cells = static_cast<std::size_t>(maximum_cells);
  source_input.maximum_memory_bytes = exchange_memory;

  exec::ModelFamilyExecutionRequestV1 execution_request;
  execution_request.input = source_input;
  execution_request.capability.capability_uuid = capability_uuid;
  execution_request.capability.family_id = "vector";
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
      vector_request.cancellation_requested;
  execution_request.cleanup_provider = [] {};
  execution_request.security_admitted = planning.security_admitted;
  execution_request.current_catalog_generation = generation;
  execution_request.current_descriptor_generation =
      source_input.descriptor_generation;
  execution_request.current_security_generation = planning.security_epoch;
  execution_request.current_policy_generation = planning.policy_epoch;
  execution_request.current_resource_generation = planning.resource_epoch;
  execution_request.current_provider_generation =
      source_input.provider_generation;
  execution_request.current_mga_statement_context = mga;
  execution_request.execute_provider =
      [vector_request, source_input, public_columns, property_uuid,
       security_receipt_uuid](const exec::ModelSourceInputDescriptorV1&) mutable {
        exec::ModelProviderExecutionResultV1 provider;
        const auto read = api::EngineBoundVectorReadV1(vector_request);
        provider.data_access_observed = read.execution_resource_acquired;
        provider.rows_examined = read.scanned_row_version_count;
        if (!read.ok) {
          provider.diagnostic_id = read.diagnostic.code.empty()
                                       ? "SB_MODEL_COORDINATOR_LEG_FAILED_V1"
                                       : read.diagnostic.code;
          provider.detail = read.diagnostic.detail.empty()
                                ? "engine-bound vector provider read failed"
                                : read.diagnostic.detail;
          return provider;
        }
        if (read.relation_descriptor.relation_uuid.canonical !=
                source_input.object_uuid ||
            read.relation_descriptor.descriptor_generation !=
                source_input.descriptor_generation ||
            !std::ranges::equal(
                read.output_descriptors, vector_request.output_descriptors,
                [](const auto& left, const auto& right) {
                  return left.descriptor_uuid.canonical ==
                             right.descriptor_uuid.canonical &&
                         left.descriptor_kind == right.descriptor_kind &&
                         left.canonical_type_name ==
                             right.canonical_type_name &&
                         left.encoded_descriptor == right.encoded_descriptor;
                }) ||
            read.current_relation_base_generation == 0 ||
            read.cleanup_count != 1 || !read.execution_resource_acquired ||
            read.exact_fallback_selected ||
            !read.full_base_exact_recheck_complete ||
            !read.base_row_mga_recheck_complete ||
            !read.security_recheck_complete) {
          provider.diagnostic_id = "SB_MODEL_TYPED_EXCHANGE_INVALID_V1";
          provider.detail =
              "vector provider descriptor, generation, or recheck receipt changed";
          return provider;
        }
        auto& batch = provider.provider_batch;
        batch.provider_uuid = source_input.provider_uuid;
        batch.provider_generation = source_input.provider_generation;
        batch.selected_alternative_uuid =
            source_input.selected_alternative_uuid;
        batch.capability_uuid = source_input.capability_uuid;
        batch.result_handle_uuid = source_input.result_handle_uuid;
        batch.causal_counter_id = source_input.causal_counter_id;
        batch.output_descriptor_ids = source_input.output_descriptor_ids;
        batch.batch.columns = public_columns;
        batch.mga_statement_context = source_input.mga_statement_context;
        batch.security_receipt_uuid = security_receipt_uuid;
        batch.properties.property_uuid = property_uuid;
        batch.properties.ordering_id =
            "vector_distance_row_uuid_ascending_v1";
        batch.properties.partitioning_id = "single_local_partition";
        batch.properties.uniqueness_id = "row_uuid";
        batch.properties.exact = true;
        batch.properties.residual_recheck_complete = true;
        batch.properties.base_row_mga_recheck_complete = true;
        batch.properties.security_recheck_complete = true;
        batch.residual_recheck_complete = true;
        batch.base_row_mga_recheck_complete = true;
        batch.security_recheck_complete = true;
        for (const auto& row : read.rows) {
          exec::DescriptorTuple tuple;
          const std::array<std::string, 3> encoded{
              row.row_uuid, row.encoded_distance, row.encoded_score};
          for (std::size_t ordinal = 0; ordinal < encoded.size(); ++ordinal) {
            api::EngineTypedValue value;
            value.descriptor = public_columns[ordinal].descriptor;
            value.encoded_value = encoded[ordinal];
            value.setState(api::EngineValueState::value);
            tuple.values.push_back(std::move(value));
          }
          batch.batch.rows.push_back(std::move(tuple));
          exec::ModelProviderRowIdentityV1 identity;
          identity.row_uuid = row.row_uuid;
          identity.vector_distance = row.encoded_distance;
          identity.vector_score = row.encoded_score;
          batch.ordered_row_identities.push_back(std::move(identity));
        }
        provider.ok = true;
        return provider;
      };

  CaptureRcp079ModelLegV1(
      leg_capture, source->node_id, "vector", "physical_vector_search_v1",
      "canonical.vector.search.v1", "vector.local.v1",
      persisted_relation.descriptor_uuid.canonical,
      persisted_relation.descriptor_generation,
      plan::CanonicalLogicalRelationalNodeKind::kRelationSource,
      exec::PhysicalNodeKind::kScan, execution_request);
  if (leg_capture != nullptr) return result;
  exec::CanonicalPhysicalExecutorRegistration registration;
  registration.node_kind = exec::PhysicalNodeKind::kScan;
  registration.implementation_id = "physical_vector_search_v1";
  registration.executor_capability_uuid = capability_uuid;
  registration.executor_capability_abi_version = 1;
  registration.engine_owned = true;
  registration.accepts_optimizer_publication_v2 = true;
  registration.execute =
      [execution_request, persisted_descriptor_uuid =
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
            selected_node.implementation_id != "physical_vector_search_v1" ||
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
              "selected vector physical node identity was substituted";
          return step;
        }
        const auto executed = exec::ExecuteModelFamilySourceV1(execution_request);
        step.data_access_observed = executed.data_access_observed;
        if (!executed.accepted || !executed.root_published ||
            !executed.cleanup_complete || executed.cleanup_count != 1 ||
            !executed.output.exact_exchange_validated ||
            executed.output.family_id != "vector" ||
            executed.output.operation_id != execution_request.input.operation_id ||
            executed.output.object_uuid != execution_request.input.object_uuid ||
            executed.output.physical_node_id != selected_node.physical_node_id ||
            executed.output.selected_alternative_uuid !=
                selected_node.selected_alternative_uuid ||
            executed.output.capability_uuid !=
                selected_node.executor_capability_uuid ||
            executed.output.provider_uuid != execution_request.input.provider_uuid ||
            executed.output.provider_generation !=
                execution_request.input.provider_generation ||
            executed.output.result_handle_uuid !=
                execution_request.input.result_handle_uuid ||
            executed.output.causal_counter_id != selected_node.causal_counter_id ||
            executed.output.output_descriptor_ids !=
                selected_node.output_descriptor_ids ||
            executed.output.exact_fallback_selected) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              executed.diagnostic_id.empty()
                  ? "SB_MODEL_COORDINATOR_LEG_FAILED_V1"
                  : executed.diagnostic_id;
          step.diagnostic.detail =
              executed.detail.empty()
                  ? "vector source execution did not complete"
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
  selected.mga_authority =
      BuildCanonicalExecutionMgaAuthority(input.context, physical.physical_dag);
  selected.runtime_limits.maximum_rows_per_batch = source_input.maximum_rows;
  selected.runtime_limits.maximum_columns_per_batch = 3;
  selected.runtime_limits.maximum_cells_per_batch = source_input.maximum_cells;
  selected.runtime_limits.maximum_total_materialized_rows =
      source_input.maximum_rows;
  selected.runtime_limits.maximum_total_materialized_cells =
      source_input.maximum_cells;
  selected.cancellation_requested = vector_request.cancellation_requested;
  selected.available_executors.push_back(std::move(registration));
  selected.engine_execution_authorized = true;
  selected.result_publication_request.statement_uuid =
      input.context.statement_uuid.canonical;
  selected.result_publication_request.invocation_mode =
      exec::CanonicalResultInvocationMode::kDirect;
  selected.result_publication_request.execution_attempt_uuid =
      DerivedCanonicalUuid(identity_scope + ":" +
                               input.context.current_monotonic_ns,
                           "vector.execution-attempt");
  selected.result_publication_request.result_kind =
      exec::CanonicalResultKind::kRows;
  selected.result_publication_request.transaction_effect_evidence_uuid =
      DerivedCanonicalUuid(
          identity_scope + ":" +
              std::to_string(input.context.local_transaction_id) + ":" +
              std::to_string(
                  input.context.snapshot_visible_through_local_transaction_id),
          "vector.transaction-effect-unchanged");
  selected.result_publication_request.maximum_row_count =
      source_input.maximum_rows;
  selected.result_publication_request.column_bindings = result_bindings;
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
      !execution.issues.empty()) {
    return refuse(execution.issues.empty()
                      ? "SB_MODEL_COORDINATOR_LEG_FAILED_V1"
                      : execution.issues.front().diagnostic_id,
                  execution.issues.empty()
                      ? "vector selected physical execution did not complete"
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
       "SBSQL_VECTOR_SOURCE_TO_SBLR_MODEL_SOURCE_TO_VECTOR_SEARCH_TO_TYPED_BATCH_V1"});
  result.api_result.evidence.push_back(
      {"canonical.model_search_family", "vector.local.v1"});
  result.api_result.evidence.push_back(
      {"canonical.vector_operation", planning.operation_id});
  result.api_result.evidence.push_back(
      {"canonical.vector_metric", *metric->literal_or_parameter_ref});
  result.api_result.evidence.push_back(
      {"canonical.vector_properties",
       "vector_distance_row_uuid_ascending_v1|single_local_partition|row_uuid"});
  return result;
}

}  // namespace scratchbird::engine::sblr
