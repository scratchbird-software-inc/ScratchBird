// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "canonical_query_table_function_composition.hpp"
#include "canonical_query_object_free_composition_support.hpp"
#include "canonical_query_object_free_profile.hpp"
#include "canonical_query_physical_registration.hpp"
#include "canonical_query_relational_registration.hpp"
#include "canonical_query_runtime_memory_support.hpp"
#include "canonical_query_scalar_support.hpp"
#include "canonical_relational_expression.hpp"

#include "engine/functions/registry/function_seed_registry.hpp"
#include "query/canonical_heap_optimizer_admission.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace scratchbird::engine::sblr {
namespace api = scratchbird::engine::internal_api;
namespace exec = scratchbird::engine::executor;
namespace fn = scratchbird::engine::functions;
namespace plan = scratchbird::engine::planner;

// SEARCH_KEY: SB_ENGINE_CANONICAL_QUERY_TABLE_FUNCTION_COMPOSITION_AUTHORITY
// Owns bounded generate-series materialization and admitted row-pattern route
// composition. Consumes engine-issued MGA context; owns no snapshot creation,
// transaction finality, storage access, parser lowering, or public route selection.

namespace {
constexpr std::string_view kGenerateSeriesFunctionId =
    "sb.rowset.generate_series";
constexpr std::string_view kGenerateSeriesFunctionUuid =
    "019dffbb-f000-7e2c-b437-ebbbc2d4f35b";
constexpr std::size_t kGenerateSeriesMaximumRowCount = 10000;

bool MaterializeCanonicalGenerateSeriesBatch(
    const api::TypedRelationalDag& dag, const api::RelationalDagNode& node,
    const api::EngineRequestContext& context, exec::DescriptorBatch* batch,
    std::string* detail) {
  if (batch == nullptr || detail == nullptr ||
      (node.argument_expression_ids.size() != 2 &&
       node.argument_expression_ids.size() != 3) ||
      node.output_descriptor_ids.size() != 1) {
    return false;
  }
  CanonicalRelationalExpressionRuntime expression_runtime(dag, {});
  std::vector<std::int64_t> arguments;
  arguments.reserve(node.argument_expression_ids.size());
  for (const auto expression_id : node.argument_expression_ids) {
    api::EngineTypedValue value;
    if (!expression_runtime.EvaluateForConsumer(
            expression_id, "int64",
            api::EngineCanonicalExpressionConsumer::projection, &value,
            detail)) {
      return false;
    }
    std::int64_t decoded = 0;
    if (!DecodeCanonicalInt64Scalar(value, &decoded, detail)) return false;
    arguments.push_back(decoded);
  }
  const std::int64_t start = arguments[0];
  const std::int64_t stop = arguments[1];
  const std::int64_t step = arguments.size() == 3 ? arguments[2] : 1;
  if (step == 0) {
    *detail = "generate_series step must not be zero";
    return false;
  }
  const auto output_descriptor = std::ranges::find_if(
      dag.descriptors, [&](const auto& descriptor) {
        return descriptor.descriptor_id ==
               node.output_descriptor_ids.front();
      });
  if (output_descriptor == dag.descriptors.end() ||
      output_descriptor->nullability !=
          api::RelationalNullability::kNonNull ||
      output_descriptor->width.has_value() ||
      output_descriptor->precision.has_value() ||
      output_descriptor->scale.has_value() ||
      output_descriptor->collation_uuid.has_value() ||
      output_descriptor->timezone_profile_id.has_value()) {
    *detail = "generate_series output descriptor is not exact";
    return false;
  }
  api::EngineDescriptor engine_descriptor;
  engine_descriptor.descriptor_uuid.canonical =
      output_descriptor->descriptor_uuid;
  engine_descriptor.descriptor_kind = "scalar";
  engine_descriptor.canonical_type_name = "int64";
  engine_descriptor.encoded_descriptor =
      "type_uuid=" + output_descriptor->type_uuid +
      ";nullability=non_null";
  batch->columns.push_back(
      {"generate_series", engine_descriptor, false,
       output_descriptor->descriptor_id});
  const bool forward = step > 0;
  std::int64_t current = start;
  while ((forward && current <= stop) || (!forward && current >= stop)) {
    if (batch->rows.size() >= kGenerateSeriesMaximumRowCount) {
      *detail = "generate_series exceeds the bounded 10000-row runtime profile";
      return false;
    }
    api::EngineTypedValue value;
    value.descriptor = engine_descriptor;
    value.encoded_value = std::to_string(current);
    value.state = api::EngineValueState::value;
    value.is_null = false;
    batch->rows.push_back({{std::move(value)}});
    if (current == stop ||
        (step > 0 &&
         current > std::numeric_limits<std::int64_t>::max() - step) ||
        (step < 0 &&
         current < std::numeric_limits<std::int64_t>::min() - step)) {
      break;
    }
    const auto next = static_cast<std::int64_t>(current + step);
    if ((step > 0 && next > stop) || (step < 0 && next < stop)) break;
    current = next;
  }
  const auto batch_validation = exec::ValidateCanonicalDescriptorBatch(
      *batch, node.output_descriptor_ids);
  const auto value_validation = exec::ValidateDescriptorBatch(*batch);
  std::uint64_t batch_memory_bytes = 0;
  if (!batch_validation.ok || !value_validation.ok ||
      !RuntimeMaterializedBatchMemoryBytes(*batch, &batch_memory_bytes) ||
      batch_memory_bytes == 0 ||
      batch_memory_bytes > context.optimizer_memory_budget_bytes) {
    *detail = !batch_validation.ok
                  ? batch_validation.detail
                  : (!value_validation.ok
                         ? value_validation.detail
                         : "generate_series materialization exceeds the optimizer memory budget");
    return false;
  }
  return true;
}

}  // namespace

CanonicalObjectFreeValuesExecutionResult
ExecuteCanonicalGenerateSeriesTableFunctionQuery(
    const CanonicalCurrentHeapExecutionRequest& input) {
  CanonicalObjectFreeValuesExecutionResult result;
  const auto& dag = input.relational_dag;
  if (dag.nodes.size() != 1 || dag.root_node_id == 0 ||
      dag.nodes.front().node_id != dag.root_node_id ||
      dag.nodes.front().node_kind !=
          api::RelationalDagNodeKind::kTableFunctionInvoke) {
    return result;
  }
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

  const auto& node = dag.nodes.front();
  if (dag.wire_version != 2 ||
      node.semantic_variant_id !=
          "table-function.generate-series.v1" ||
      node.required_object_uuids !=
          std::vector<std::string>{
              std::string(kGenerateSeriesFunctionUuid)} ||
      !node.input_node_ids.empty() || node.shareable ||
      (node.argument_expression_ids.size() != 2 &&
       node.argument_expression_ids.size() != 3) ||
      node.output_descriptor_ids.size() != 1 || dag.outputs.size() != 1 ||
      dag.outputs.front().relation_node_id != node.node_id ||
      dag.outputs.front().descriptor_id != node.output_descriptor_ids.front() ||
      dag.outputs.front().output_name_utf8 != "generate_series" ||
      !dag.outputs.front().visible || dag.outputs.front().ordinal != 0 ||
      !node.required_property_uuids.empty() ||
      !node.delivered_property_uuids.empty() || !dag.properties.empty()) {
    return refuse("QOW-DIAG-QRY-004-TABLE-FUNCTION-PROFILE-V1",
                  "generate_series runtime profile is not exact");
  }

  const auto package = fn::BuildStandardFunctionSeedPackage();
  const auto* registry_entry =
      package.registry.Lookup(kGenerateSeriesFunctionId);
  if (registry_entry == nullptr ||
      registry_entry->function_uuid != kGenerateSeriesFunctionUuid ||
      registry_entry->family != "rowset.table" ||
      registry_entry->short_name != "generate_series" ||
      registry_entry->implementation_state !=
          fn::FunctionImplementationState::implemented_behavior ||
      registry_entry->package_state != fn::FunctionPackageState::core ||
      !registry_entry->catalog_visible) {
    return refuse("QOW-DIAG-QRY-004-TABLE-FUNCTION-REGISTRY-V1",
                  "generate_series engine function registry identity is unavailable");
  }

  const auto admission = api::BuildCanonicalCurrentHeapOptimizerAdmission(
      {input.context, input.relational_dag});
  if (!admission.built || !admission.admission.admitted ||
      !admission.admission.planning_allowed ||
      admission.admission.data_access_allowed) {
    return refuse(
        admission.issue.diagnostic_id.empty()
            ? "QOW-DIAG-QRY-004-TABLE-FUNCTION-ADMISSION-V1"
            : admission.issue.diagnostic_id,
        admission.issue.field_id.empty()
            ? "generate_series optimizer admission failed"
            : admission.issue.field_id);
  }
  result.optimizer_admitted = true;
  result.optimizer_admission_degraded =
      admission.admission.degraded_for_unknown_statistics;
  result.optimizer_benchmark_clean_ready =
      admission.admission.benchmark_clean_ready;
  result.optimizer_admission_stage_count =
      admission.admission.evidence.size();

  exec::DescriptorBatch batch;
  std::uint64_t batch_memory_bytes = 0;
  std::string materialization_detail;
  if (!MaterializeCanonicalGenerateSeriesBatch(
          dag, node, input.context, &batch, &materialization_detail) ||
      !RuntimeMaterializedBatchMemoryBytes(batch, &batch_memory_bytes) ||
      batch_memory_bytes == 0) {
    return refuse("QOW-DIAG-QRY-004-TABLE-FUNCTION-ARGUMENT-V1",
                  materialization_detail.empty()
                      ? "generate_series materialization failed"
                      : materialization_detail);
  }
  const auto generated_row_count = batch.rows.size();

  const auto output_descriptor = std::ranges::find_if(
      dag.descriptors, [&](const auto& descriptor) {
        return descriptor.descriptor_id ==
               node.output_descriptor_ids.front();
      });

  CanonicalObjectFreeValuesExecutionRequest planning_request{
      input.context, input.relational_dag, admission.request,
      admission.admission};
  const auto& graph = admission.request.logical_graph;
  const auto identity_scope =
      graph.bound_sblr_tree_uuid + ":" + input.context.statement_uuid.canonical;
  const auto capability_uuid =
      DerivedCanonicalUuid(identity_scope,
                           "table-function.generate-series.capability");
  LivePhysicalNodeProfile profile;
  profile.logical_node_id = node.node_id;
  profile.implementation_id = "table-function.generate-series.v1";
  profile.capability_uuid = capability_uuid;
  profile.logical_node_kind =
      plan::CanonicalLogicalRelationalNodeKind::kTableFunctionInvoke;
  profile.physical_node_kind = exec::PhysicalNodeKind::kTableFunctionInvoke;
  profile.transformation_rule_id =
      "canonical.table-function.generate-series.v1";
  profile.estimated_rows = std::max<std::size_t>(1, generated_row_count);
  profile.memory_bytes_required = batch_memory_bytes;
  profile.minimum_input_count = 0;
  profile.maximum_input_count = 0;
  profile.udr_invocation_units =
      std::max<std::uint64_t>(1, generated_row_count);
  std::vector<LivePhysicalNodeProfile> profiles;
  profiles.push_back(std::move(profile));
  if (!CompleteLiveRuntimeMemoryReceipts(&profiles)) {
    return refuse("QOW-DIAG-OPT-017-REFUSAL-V1",
                  "generate_series runtime memory receipt is incomplete");
  }
  auto physical = PlanAndPublishLivePhysicalDag(
      planning_request, profiles,
      "table-function.generate-series.selected-plan",
      "generate_series table function");
  if (!physical.ok) {
    return refuse(physical.diagnostic_id, physical.detail);
  }
  result.optimizer_selected = true;
  result.physical_dag_published = true;
  result.physical_node_count = physical.physical_dag.nodes.size();
  result.selected_plan_uuid = physical.physical_dag.selected_plan_uuid;

  api::CanonicalOptimizerSelectedExecutionRequest selected;
  selected.pre_access_statistics_snapshot_uuid =
      physical.physical_dag.statistics_snapshot_uuid;
  selected.mga_authority =
      BuildCanonicalExecutionMgaAuthority(input.context,
                                          physical.physical_dag);
  selected.selected_physical_dag = std::move(physical.physical_dag);
  std::unordered_map<std::uint64_t, exec::DescriptorBatch> batches;
  batches.emplace(node.node_id, std::move(batch));
  selected.available_executors.push_back(
      MakeLiveMaterializedSourceRegistration(
          std::move(batches), capability_uuid,
          "QOW-DIAG-QRY-004-TABLE-FUNCTION-EXECUTOR-V1",
          "generate_series table function",
          exec::PhysicalNodeKind::kTableFunctionInvoke,
          "table-function.generate-series.v1", "TABLE_FUNCTION_INVOKE",
          true, &selected.mga_authority));
  selected.engine_execution_authorized = true;
  selected.runtime_limits.maximum_rows_per_batch =
      kGenerateSeriesMaximumRowCount;
  selected.runtime_limits.maximum_columns_per_batch = 1;
  selected.runtime_limits.maximum_cells_per_batch =
      kGenerateSeriesMaximumRowCount;
  selected.runtime_limits.maximum_total_materialized_rows =
      kGenerateSeriesMaximumRowCount;
  selected.runtime_limits.maximum_total_materialized_cells =
      kGenerateSeriesMaximumRowCount;
  selected.result_publication_request.statement_uuid =
      input.context.statement_uuid.canonical;
  selected.result_publication_request.invocation_mode =
      exec::CanonicalResultInvocationMode::kDirect;
  selected.result_publication_request.execution_attempt_uuid =
      DerivedCanonicalUuid(
          identity_scope + ":" + input.context.current_monotonic_ns,
          "table-function.generate-series.execution-attempt");
  selected.result_publication_request.result_kind =
      exec::CanonicalResultKind::kRows;
  selected.result_publication_request.transaction_effect_evidence_uuid =
      DerivedCanonicalUuid(
          identity_scope + ":" +
              std::to_string(input.context.local_transaction_id) + ":" +
              std::to_string(
                  input.context.snapshot_visible_through_local_transaction_id),
          "table-function.generate-series.transaction-effect-unchanged");
  selected.result_publication_request.maximum_row_count =
      std::max<std::size_t>(1, generated_row_count);
  exec::CanonicalResultColumnBinding binding;
  binding.physical_column_ordinal = 0;
  binding.visible = true;
  binding.published_descriptor = exec::CanonicalResultColumnDescriptor{
      0, "generate_series", output_descriptor->descriptor_uuid,
      output_descriptor->type_uuid,
      exec::CanonicalResultNullability::kNonNull, std::nullopt,
      std::nullopt};
  selected.result_publication_request.column_bindings.push_back(
      std::move(binding));

  const auto execution = ExecuteSelectedCanonicalObjectFreeDag(
      input.context, selected, physical.ordinary_runtime_memory_receipts);
  if (!execution.accepted || !execution.exact_selected_nodes_executed ||
      !execution.causal_counters_attached ||
      !execution.canonical_result_published ||
      !execution.runtime_actuals.accepted ||
      execution.dispatch.executed_steps.size() != 1 ||
      !execution.issues.empty()) {
    return refuse(
        execution.issues.empty()
            ? "QOW-DIAG-QRY-004-TABLE-FUNCTION-EXECUTION-V1"
            : execution.issues.front().diagnostic_id,
        execution.issues.empty()
            ? "generate_series selected physical DAG did not complete"
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
  result.api_result = SuccessfulApiResult(planning_request, execution);
  result.api_result.evidence.push_back(
      {"canonical.table_function",
       "SBSQL_GENERATE_SERIES_TO_OPTIMIZER_TO_PHYSICAL_SOURCE_V1"});
  return result;
}

CanonicalObjectFreeValuesExecutionResult
ExecuteCanonicalGenerateSeriesMatchRecognizeQuery(
    const CanonicalCurrentHeapExecutionRequest& input) {
  CanonicalObjectFreeValuesExecutionResult result;
  const auto& dag = input.relational_dag;
  const auto source = std::ranges::find_if(dag.nodes, [](const auto& node) {
    return node.node_kind ==
           api::RelationalDagNodeKind::kTableFunctionInvoke;
  });
  const auto match = std::ranges::find_if(dag.nodes, [](const auto& node) {
    return node.node_kind == api::RelationalDagNodeKind::kMatchRecognize;
  });
  if (dag.nodes.size() != 2 || source == dag.nodes.end() ||
      match == dag.nodes.end() || dag.root_node_id != match->node_id) {
    return result;
  }
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
  constexpr std::string_view kFunctionUuid =
      "019dffbb-f000-7e2c-b437-ebbbc2d4f35b";
  const auto& pattern = dag.row_patterns.empty()
                            ? api::RelationalRowPatternRecord{}
                            : dag.row_patterns.front();
  const auto source_output = std::ranges::find_if(
      dag.outputs, [&](const auto& output) {
        return output.relation_node_id == source->node_id;
      });
  const auto match_output = std::ranges::find_if(
      dag.outputs, [&](const auto& output) {
        return output.relation_node_id == match->node_id;
      });
  if (dag.wire_version != 2 ||
      source->semantic_variant_id !=
          "table-function.generate-series.v1" ||
      source->required_object_uuids !=
          std::vector<std::string>{std::string(kFunctionUuid)} ||
      !source->input_node_ids.empty() || source->shareable ||
      (source->argument_expression_ids.size() != 2 &&
       source->argument_expression_ids.size() != 3) ||
      source->output_descriptor_ids.size() != 1 ||
      match->semantic_variant_id !=
          "match-recognize.a-plus.true.all-rows.v1" ||
      match->input_node_ids !=
          std::vector<std::uint32_t>{source->node_id} ||
      match->shareable || !match->required_object_uuids.empty() ||
      match->output_descriptor_ids != source->output_descriptor_ids ||
      match->required_property_uuids.size() != 2 ||
      match->delivered_property_uuids != match->required_property_uuids ||
      dag.row_patterns.size() != 1 || dag.properties.size() != 2 ||
      pattern.pattern_id != 1 || pattern.relation_node_id != match->node_id ||
      pattern.partition_expression_ids.size() != 1 ||
      pattern.ordering_terms.size() != 1 ||
      pattern.partition_expression_ids.front() !=
          pattern.ordering_terms.front().expression_id ||
      pattern.ordering_terms.front().direction !=
          api::RelationalPropertySortDirection::kAscending ||
      pattern.ordering_terms.front().null_placement !=
          api::RelationalPropertyNullPlacement::kNullsLast ||
      !pattern.ordering_terms.front().collation_uuid.empty() ||
      pattern.variables.size() != 1 ||
      pattern.variables.front().canonical_name_key != "a" ||
      pattern.variables.front().minimum_occurrences != 1 ||
      pattern.variables.front().maximum_occurrences.has_value() ||
      pattern.variables.front().reluctant ||
      pattern.variables.front().define_expression_id.has_value() ||
      !pattern.variables.front().define_always_true ||
      !pattern.measure_expression_ids.empty() ||
      pattern.rows_per_match !=
          api::RelationalRowPatternRowsPerMatch::kAll ||
      pattern.after_match_skip !=
          api::RelationalRowPatternAfterMatchSkip::kPastLastRow ||
      pattern.skip_target_key.has_value() ||
      pattern.maximum_partition_rows != kGenerateSeriesMaximumRowCount ||
      pattern.maximum_active_states != 2 ||
      pattern.maximum_output_rows != kGenerateSeriesMaximumRowCount ||
      !pattern.stable_row_identity_tie_break_allowed ||
      dag.outputs.size() != 2 || source_output == dag.outputs.end() ||
      match_output == dag.outputs.end() || !source_output->visible ||
      !match_output->visible || source_output->ordinal != 0 ||
      match_output->ordinal != 0 ||
      source_output->expression_id != match_output->expression_id ||
      source_output->descriptor_id != match_output->descriptor_id ||
      source_output->output_name_utf8 != "generate_series" ||
      match_output->output_name_utf8 != source_output->output_name_utf8) {
    return refuse("QOW-DIAG-QRY-004-MATCH-RECOGNIZE-PROFILE-V1",
                  "MATCH_RECOGNIZE runtime profile is not exact");
  }
  const auto package = fn::BuildStandardFunctionSeedPackage();
  const auto* registry_entry =
      package.registry.Lookup(kGenerateSeriesFunctionId);
  if (registry_entry == nullptr ||
      registry_entry->function_uuid != kFunctionUuid ||
      registry_entry->family != "rowset.table" ||
      registry_entry->short_name != "generate_series" ||
      registry_entry->implementation_state !=
          fn::FunctionImplementationState::implemented_behavior ||
      registry_entry->package_state != fn::FunctionPackageState::core ||
      !registry_entry->catalog_visible) {
    return refuse("QOW-DIAG-QRY-004-MATCH-RECOGNIZE-REGISTRY-V1",
                  "generate_series row-pattern source is unavailable");
  }
  const auto admission = api::BuildCanonicalCurrentHeapOptimizerAdmission(
      {input.context, input.relational_dag});
  if (!admission.built || !admission.admission.admitted ||
      !admission.admission.planning_allowed ||
      admission.admission.data_access_allowed) {
    return refuse(
        admission.issue.diagnostic_id.empty()
            ? "QOW-DIAG-QRY-004-MATCH-RECOGNIZE-ADMISSION-V1"
            : admission.issue.diagnostic_id,
        admission.issue.field_id.empty()
            ? "MATCH_RECOGNIZE optimizer admission failed"
            : admission.issue.field_id);
  }
  result.optimizer_admitted = true;
  result.optimizer_admission_degraded =
      admission.admission.degraded_for_unknown_statistics;
  result.optimizer_benchmark_clean_ready =
      admission.admission.benchmark_clean_ready;
  result.optimizer_admission_stage_count =
      admission.admission.evidence.size();

  exec::DescriptorBatch source_batch;
  std::string materialization_detail;
  std::uint64_t source_batch_memory_bytes = 0;
  if (!MaterializeCanonicalGenerateSeriesBatch(
          dag, *source, input.context, &source_batch,
          &materialization_detail) ||
      !RuntimeMaterializedBatchMemoryBytes(
          source_batch, &source_batch_memory_bytes) ||
      source_batch_memory_bytes == 0) {
    return refuse("QOW-DIAG-QRY-004-MATCH-RECOGNIZE-SOURCE-V1",
                  materialization_detail.empty()
                      ? "MATCH_RECOGNIZE source materialization failed"
                      : materialization_detail);
  }
  const auto generated_row_count = source_batch.rows.size();
  const auto output_descriptor = std::ranges::find_if(
      dag.descriptors, [&](const auto& descriptor) {
        return descriptor.descriptor_id ==
               match->output_descriptor_ids.front();
      });
  if (output_descriptor == dag.descriptors.end() ||
      input.context.optimizer_memory_budget_bytes == 0) {
    return refuse("QOW-DIAG-QRY-004-MATCH-RECOGNIZE-DESCRIPTOR-V1",
                  "MATCH_RECOGNIZE output descriptor or memory grant is absent");
  }

  CanonicalObjectFreeValuesExecutionRequest planning_request{
      input.context, input.relational_dag, admission.request,
      admission.admission};
  const auto& graph = admission.request.logical_graph;
  const auto identity_scope = graph.bound_sblr_tree_uuid + ":" +
                              input.context.statement_uuid.canonical;
  const auto source_capability_uuid = DerivedCanonicalUuid(
      identity_scope, "table-function.generate-series.capability");
  const auto match_capability_uuid = DerivedCanonicalUuid(
      identity_scope, "match-recognize.a-plus.capability");
  std::vector<LivePhysicalNodeProfile> profiles;
  LivePhysicalNodeProfile source_profile;
  source_profile.logical_node_id = source->node_id;
  source_profile.implementation_id = "table-function.generate-series.v1";
  source_profile.capability_uuid = source_capability_uuid;
  source_profile.logical_node_kind =
      plan::CanonicalLogicalRelationalNodeKind::kTableFunctionInvoke;
  source_profile.physical_node_kind =
      exec::PhysicalNodeKind::kTableFunctionInvoke;
  source_profile.transformation_rule_id =
      "canonical.table-function.generate-series.v1";
  source_profile.estimated_rows = std::max<std::size_t>(1,
                                                        generated_row_count);
  source_profile.memory_bytes_required = source_batch_memory_bytes;
  source_profile.minimum_input_count = 0;
  source_profile.maximum_input_count = 0;
  source_profile.udr_invocation_units =
      std::max<std::uint64_t>(1, generated_row_count);
  profiles.push_back(std::move(source_profile));
  LivePhysicalNodeProfile match_profile;
  match_profile.logical_node_id = match->node_id;
  match_profile.implementation_id =
      "match-recognize.partition-order.a-plus.v1";
  match_profile.capability_uuid = match_capability_uuid;
  match_profile.logical_node_kind =
      plan::CanonicalLogicalRelationalNodeKind::kMatchRecognize;
  match_profile.physical_node_kind =
      exec::PhysicalNodeKind::kMatchRecognize;
  match_profile.transformation_rule_id =
      "canonical.match-recognize.partition-order.a-plus.v1";
  match_profile.estimated_rows =
      std::max<std::size_t>(1, generated_row_count);
  match_profile.memory_bytes_required =
      input.context.optimizer_memory_budget_bytes;
  match_profile.minimum_input_count = 1;
  match_profile.maximum_input_count = 1;
  match_profile.required_property_uuids = match->required_property_uuids;
  match_profile.delivered_property_uuids = match->delivered_property_uuids;
  match_profile.supported_property_kinds = {
      plan::CanonicalLogicalPropertyKind::kPartitioning,
      plan::CanonicalLogicalPropertyKind::kOrdering};
  match_profile.memory_grant_units =
      input.context.optimizer_memory_budget_bytes;
  match_profile.predicate_evaluation_units =
      std::max<std::uint64_t>(1, generated_row_count);
  match_profile.runtime_peak_from_callback_batches = true;
  profiles.push_back(std::move(match_profile));
  if (!CompleteLiveRuntimeMemoryReceipts(&profiles)) {
    return refuse("QOW-DIAG-OPT-017-REFUSAL-V1",
                  "MATCH_RECOGNIZE runtime memory receipts are incomplete");
  }
  auto physical = PlanAndPublishLivePhysicalDag(
      planning_request, profiles, "match-recognize.selected-plan",
      "MATCH_RECOGNIZE");
  if (!physical.ok) {
    return refuse(physical.diagnostic_id, physical.detail);
  }
  result.optimizer_selected = true;
  result.physical_dag_published = true;
  result.physical_node_count = physical.physical_dag.nodes.size();
  result.selected_plan_uuid = physical.physical_dag.selected_plan_uuid;

  api::CanonicalOptimizerSelectedExecutionRequest selected;
  selected.pre_access_statistics_snapshot_uuid =
      physical.physical_dag.statistics_snapshot_uuid;
  selected.mga_authority = BuildCanonicalExecutionMgaAuthority(
      input.context, physical.physical_dag);
  selected.selected_physical_dag = std::move(physical.physical_dag);
  std::unordered_map<std::uint64_t, exec::DescriptorBatch> source_batches;
  source_batches.emplace(source->node_id, std::move(source_batch));
  selected.available_executors.push_back(
      MakeLiveMaterializedSourceRegistration(
          std::move(source_batches), source_capability_uuid,
          "QOW-DIAG-QRY-004-MATCH-RECOGNIZE-SOURCE-V1",
          "MATCH_RECOGNIZE generate_series source",
          exec::PhysicalNodeKind::kTableFunctionInvoke,
          "table-function.generate-series.v1", "TABLE_FUNCTION_INVOKE", true,
          &selected.mga_authority));
  selected.available_executors.push_back(
      MakeLiveMatchRecognizeRegistration(
          match_capability_uuid, pattern.maximum_partition_rows,
          pattern.maximum_active_states, pattern.maximum_output_rows,
          &input.context, &selected.mga_authority));
  selected.engine_execution_authorized = true;
  selected.runtime_limits.maximum_rows_per_batch =
      kGenerateSeriesMaximumRowCount;
  selected.runtime_limits.maximum_columns_per_batch = 1;
  selected.runtime_limits.maximum_cells_per_batch =
      kGenerateSeriesMaximumRowCount;
  selected.runtime_limits.maximum_total_materialized_rows =
      kGenerateSeriesMaximumRowCount;
  selected.runtime_limits.maximum_total_materialized_cells =
      kGenerateSeriesMaximumRowCount;
  selected.result_publication_request.statement_uuid =
      input.context.statement_uuid.canonical;
  selected.result_publication_request.invocation_mode =
      exec::CanonicalResultInvocationMode::kDirect;
  selected.result_publication_request.execution_attempt_uuid =
      DerivedCanonicalUuid(
          identity_scope + ":" + input.context.current_monotonic_ns,
          "match-recognize.execution-attempt");
  selected.result_publication_request.result_kind =
      exec::CanonicalResultKind::kRows;
  selected.result_publication_request.transaction_effect_evidence_uuid =
      DerivedCanonicalUuid(
          identity_scope + ":" +
              std::to_string(input.context.local_transaction_id) + ":" +
              std::to_string(
                  input.context.snapshot_visible_through_local_transaction_id),
          "match-recognize.transaction-effect-unchanged");
  selected.result_publication_request.maximum_row_count =
      std::max<std::size_t>(1, generated_row_count);
  exec::CanonicalResultColumnBinding binding;
  binding.physical_column_ordinal = 0;
  binding.visible = true;
  binding.published_descriptor = exec::CanonicalResultColumnDescriptor{
      0, "generate_series", output_descriptor->descriptor_uuid,
      output_descriptor->type_uuid,
      exec::CanonicalResultNullability::kNonNull, std::nullopt,
      std::nullopt};
  selected.result_publication_request.column_bindings.push_back(
      std::move(binding));

  const auto execution = ExecuteSelectedCanonicalObjectFreeDag(
      input.context, selected, physical.ordinary_runtime_memory_receipts);
  if (!execution.accepted || !execution.exact_selected_nodes_executed ||
      !execution.causal_counters_attached ||
      !execution.canonical_result_published ||
      !execution.runtime_actuals.accepted ||
      execution.dispatch.executed_steps.size() != 2 ||
      !execution.issues.empty()) {
    return refuse(
        execution.issues.empty()
            ? "QOW-DIAG-QRY-004-MATCH-RECOGNIZE-EXECUTION-V1"
            : execution.issues.front().diagnostic_id,
        execution.issues.empty()
            ? "MATCH_RECOGNIZE selected physical DAG did not complete"
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
  result.api_result = SuccessfulApiResult(planning_request, execution);
  result.api_result.evidence.push_back(
      {"canonical.match_recognize",
       "SBSQL_GENERATE_SERIES_TO_MATCH_RECOGNIZE_A_PLUS_ALL_ROWS_V1"});
  result.api_result.evidence.push_back(
      {"canonical.match_recognize.stable_tie_break", "source_row_ordinal"});
  return result;
}

}  // namespace scratchbird::engine::sblr
