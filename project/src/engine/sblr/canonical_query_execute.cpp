// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "canonical_query_execute.hpp"

#include "engine/optimizer/optimizer_contract.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <optional>
#include <ranges>
#include <sstream>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace scratchbird::engine::sblr {
namespace api = scratchbird::engine::internal_api;
namespace exec = scratchbird::engine::executor;
namespace opt = scratchbird::engine::optimizer;
namespace plan = scratchbird::engine::planner;
namespace {

constexpr std::string_view kValuesImplementationId =
    "values.materialize.canonical.v1";

std::uint64_t Fnv1a64(const std::string_view value,
                      std::uint64_t hash = 14695981039346656037ull) {
  for (const auto byte : value) {
    hash ^= static_cast<std::uint8_t>(byte);
    hash *= 1099511628211ull;
  }
  return hash;
}

std::string DerivedCanonicalUuid(const std::string_view scope,
                                 const std::string_view purpose) {
  const auto first = Fnv1a64(purpose, Fnv1a64(scope));
  const auto second = Fnv1a64(scope, Fnv1a64(purpose));
  std::array<std::uint8_t, 16> bytes{};
  for (std::size_t index = 0; index < 8; ++index) {
    bytes[index] = static_cast<std::uint8_t>(first >> ((7 - index) * 8));
    bytes[8 + index] =
        static_cast<std::uint8_t>(second >> ((7 - index) * 8));
  }
  bytes[6] = static_cast<std::uint8_t>((bytes[6] & 0x0f) | 0x50);
  bytes[8] = static_cast<std::uint8_t>((bytes[8] & 0x3f) | 0x80);
  std::ostringstream out;
  out << std::hex << std::setfill('0');
  for (std::size_t index = 0; index < bytes.size(); ++index) {
    if (index == 4 || index == 6 || index == 8 || index == 10) out << '-';
    out << std::setw(2) << static_cast<unsigned>(bytes[index]);
  }
  return out.str();
}

api::EngineApiResult Failure(const CanonicalObjectFreeValuesExecutionRequest& request,
                             std::string diagnostic_id,
                             std::string detail) {
  api::EngineApiResult result;
  result.operation_id = "query.execute";
  result.local_transaction_id = request.context.local_transaction_id;
  result.transaction_uuid = request.context.transaction_uuid;
  result.embedded_trust_mode_observed =
      request.context.trust_mode == api::EngineTrustMode::embedded_in_process;
  api::EngineApiDiagnostic diagnostic;
  diagnostic.code = std::move(diagnostic_id);
  diagnostic.message_key = "engine.sblr.query_execute.refused";
  diagnostic.detail = std::move(detail);
  diagnostic.error = true;
  result.diagnostics.push_back(std::move(diagnostic));
  return result;
}

bool IsCanonicalUuidText(const std::string_view value) {
  if (value.size() != 36 || value[8] != '-' || value[13] != '-' ||
      value[18] != '-' || value[23] != '-') {
    return false;
  }
  for (std::size_t index = 0; index < value.size(); ++index) {
    if (index == 8 || index == 13 || index == 18 || index == 23) continue;
    const auto byte = static_cast<unsigned char>(value[index]);
    if (!std::isxdigit(byte) || std::isupper(byte)) return false;
  }
  return true;
}

std::string LiteralTypeName(const api::RelationalExpressionRecord& expression) {
  if (!expression.literal_kind.has_value()) return {};
  switch (*expression.literal_kind) {
    case api::RelationalLiteralKind::kNumeric: {
      if (!expression.literal_or_parameter_ref.has_value()) return {};
      std::int64_t decoded = 0;
      const auto& encoded = *expression.literal_or_parameter_ref;
      const auto [end, error] = std::from_chars(
          encoded.data(), encoded.data() + encoded.size(), decoded);
      return error == std::errc{} && end == encoded.data() + encoded.size()
                 ? "int64"
                 : std::string{};
    }
    case api::RelationalLiteralKind::kString: return "text";
    case api::RelationalLiteralKind::kUuid:
      return expression.literal_or_parameter_ref.has_value() &&
                     IsCanonicalUuidText(*expression.literal_or_parameter_ref)
                 ? "uuid"
                 : std::string{};
    case api::RelationalLiteralKind::kBoolean: return "boolean";
    case api::RelationalLiteralKind::kNull: return "null";
    case api::RelationalLiteralKind::kTemporal:
      // The current relational literal ABI does not retain DATE/TIME/TIMESTAMP
      // subtype identity. Refuse rather than assigning a possibly wrong type.
      return {};
    default: return {};
  }
}

exec::CanonicalResultNullability ResultNullability(
    const api::RelationalNullability nullability) {
  switch (nullability) {
    case api::RelationalNullability::kNonNull:
      return exec::CanonicalResultNullability::kNonNull;
    case api::RelationalNullability::kNullable:
      return exec::CanonicalResultNullability::kNullable;
    case api::RelationalNullability::kUnknown:
      return exec::CanonicalResultNullability::kUnknown;
  }
  return exec::CanonicalResultNullability::kUnknown;
}

struct MaterializedValues {
  bool ok{false};
  exec::DescriptorBatch batch;
  std::vector<exec::CanonicalResultColumnBinding> result_bindings;
  std::string detail;
};

MaterializedValues MaterializeValues(
    const api::TypedRelationalDag& dag,
    const plan::CanonicalLogicalRelationalNode& logical_node) {
  MaterializedValues result;
  const auto node_it = std::ranges::find_if(
      dag.nodes, [&](const auto& node) {
        return node.node_id == logical_node.logical_node_id;
      });
  if (node_it == dag.nodes.end() ||
      node_it->node_kind != api::RelationalDagNodeKind::kValues ||
      !node_it->input_node_ids.empty() || node_it->values_row_ids.empty() ||
      node_it->output_descriptor_ids.empty()) {
    result.detail = "live VALUES root shape is incomplete";
    return result;
  }

  std::unordered_map<std::uint32_t, const api::RelationalTypeDescriptor*>
      descriptors;
  std::unordered_map<std::uint32_t, const api::RelationalExpressionRecord*>
      expressions;
  std::unordered_map<std::uint32_t, const api::RelationalValuesRowRecord*> rows;
  for (const auto& descriptor : dag.descriptors) {
    descriptors.emplace(descriptor.descriptor_id, &descriptor);
  }
  for (const auto& expression : dag.expressions) {
    expressions.emplace(expression.expression_id, &expression);
  }
  for (const auto& row : dag.values_rows) rows.emplace(row.row_id, &row);

  std::vector<const api::RelationalOutputRecord*> outputs;
  for (const auto& output : dag.outputs) {
    if (output.relation_node_id == node_it->node_id) outputs.push_back(&output);
  }
  std::ranges::sort(outputs, {}, &api::RelationalOutputRecord::ordinal);
  if (outputs.size() != node_it->output_descriptor_ids.size()) {
    result.detail = "live VALUES result output coverage is incomplete";
    return result;
  }

  std::vector<std::string> type_names(node_it->output_descriptor_ids.size());
  for (const auto row_id : node_it->values_row_ids) {
    const auto row = rows.find(row_id);
    if (row == rows.end() ||
        row->second->expression_ids.size() != type_names.size()) {
      result.detail = "live VALUES row width is inconsistent";
      return result;
    }
    for (std::size_t column = 0; column < type_names.size(); ++column) {
      const auto expression =
          expressions.find(row->second->expression_ids[column]);
      if (expression == expressions.end() ||
          expression->second->expression_kind !=
              api::RelationalExpressionKind::kLiteral ||
          expression->second->result_descriptor_id !=
              node_it->output_descriptor_ids[column]) {
        result.detail = "live VALUES expression is not a bound literal";
        return result;
      }
      const auto type_name = LiteralTypeName(*expression->second);
      if (type_name.empty()) {
        result.detail = "live VALUES literal type is outside the admitted profile";
        return result;
      }
      if (type_name == "null") continue;
      if (!type_names[column].empty() && type_names[column] != type_name) {
        result.detail = "live VALUES column has unreconciled literal types";
        return result;
      }
      type_names[column] = type_name;
    }
  }

  std::size_t published_ordinal = 0;
  for (std::size_t column = 0; column < type_names.size(); ++column) {
    const auto descriptor =
        descriptors.find(node_it->output_descriptor_ids[column]);
    if (descriptor == descriptors.end() || type_names[column].empty() ||
        descriptor->second->nullability ==
            api::RelationalNullability::kUnknown ||
        (descriptor->second->collation_uuid.has_value() &&
         type_names[column] != "text") ||
        (descriptor->second->timezone_profile_id.has_value() &&
         type_names[column] != "timestamp") ||
        outputs[column]->ordinal != column ||
        outputs[column]->descriptor_id !=
            node_it->output_descriptor_ids[column] ||
        outputs[column]->output_name_utf8.empty()) {
      result.detail = "live VALUES descriptor or output binding is unresolved";
      return result;
    }
    api::EngineDescriptor engine_descriptor;
    engine_descriptor.descriptor_uuid.canonical =
        descriptor->second->descriptor_uuid;
    engine_descriptor.descriptor_kind = "scalar";
    engine_descriptor.canonical_type_name = type_names[column];
    engine_descriptor.encoded_descriptor =
        "type_uuid=" + descriptor->second->type_uuid + ";nullability=" +
        (descriptor->second->nullability ==
                 api::RelationalNullability::kNullable
             ? "nullable"
             : "non_null");
    if (descriptor->second->collation_uuid.has_value()) {
      engine_descriptor.encoded_descriptor +=
          ";collation_uuid=" + *descriptor->second->collation_uuid;
    }
    if (descriptor->second->timezone_profile_id.has_value()) {
      engine_descriptor.encoded_descriptor +=
          ";timezone_profile_id=" + *descriptor->second->timezone_profile_id;
    }
    if (descriptor->second->width.has_value()) {
      engine_descriptor.encoded_descriptor +=
          ";width=" + std::to_string(*descriptor->second->width);
    }
    if (descriptor->second->precision.has_value()) {
      engine_descriptor.encoded_descriptor +=
          ";precision=" + std::to_string(*descriptor->second->precision);
    }
    if (descriptor->second->scale.has_value()) {
      engine_descriptor.encoded_descriptor +=
          ";scale=" + std::to_string(*descriptor->second->scale);
    }
    result.batch.columns.push_back(
        {outputs[column]->output_name_utf8, engine_descriptor,
         descriptor->second->nullability ==
             api::RelationalNullability::kNullable,
         descriptor->second->descriptor_id});

    exec::CanonicalResultColumnBinding binding;
    binding.physical_column_ordinal = column;
    binding.visible = outputs[column]->visible;
    if (binding.visible) {
      binding.published_descriptor = exec::CanonicalResultColumnDescriptor{
          static_cast<std::uint32_t>(published_ordinal++),
          outputs[column]->output_name_utf8,
          descriptor->second->descriptor_uuid,
          descriptor->second->type_uuid,
          ResultNullability(descriptor->second->nullability),
          descriptor->second->collation_uuid,
          descriptor->second->timezone_profile_id};
    }
    result.result_bindings.push_back(std::move(binding));
  }

  result.batch.rows.reserve(node_it->values_row_ids.size());
  for (const auto row_id : node_it->values_row_ids) {
    const auto* row = rows.at(row_id);
    exec::DescriptorTuple tuple;
    tuple.values.reserve(row->expression_ids.size());
    for (std::size_t column = 0; column < row->expression_ids.size(); ++column) {
      const auto* expression = expressions.at(row->expression_ids[column]);
      api::EngineTypedValue value;
      value.descriptor = result.batch.columns[column].descriptor;
      if (*expression->literal_kind == api::RelationalLiteralKind::kNull) {
        value.state = api::EngineValueState::sql_null;
        value.is_null = true;
      } else {
        value.state = api::EngineValueState::value;
        value.encoded_value = *expression->literal_or_parameter_ref;
      }
      tuple.values.push_back(std::move(value));
    }
    result.batch.rows.push_back(std::move(tuple));
  }
  const auto canonical = exec::ValidateCanonicalDescriptorBatch(
      result.batch, node_it->output_descriptor_ids);
  const auto values = exec::ValidateDescriptorBatch(result.batch);
  if (!canonical.ok || !values.ok) {
    result.batch = {};
    result.result_bindings.clear();
    result.detail = !canonical.ok ? canonical.diagnostic_code + ":" + canonical.detail
                                  : values.diagnostic_code + ":" + values.detail;
    return result;
  }
  result.ok = true;
  return result;
}

api::EngineApiResult SuccessfulApiResult(
    const CanonicalObjectFreeValuesExecutionRequest& request,
    const api::CanonicalOptimizerSelectedExecutionResult& execution) {
  api::EngineApiResult result;
  result.ok = true;
  result.operation_id = "query.execute";
  result.result_shape.result_kind = "rows";
  result.local_transaction_id = request.context.local_transaction_id;
  result.transaction_uuid = request.context.transaction_uuid;
  result.embedded_trust_mode_observed =
      request.context.trust_mode == api::EngineTrustMode::embedded_in_process;
  for (const auto& column : execution.result_publication.row_stream.columns) {
    result.result_shape.columns.push_back(column.descriptor);
  }
  for (const auto& row : execution.result_publication.row_stream.rows) {
    api::EngineRowValue api_row;
    for (std::size_t column = 0; column < row.values.size(); ++column) {
      api_row.fields.emplace_back(
          execution.result_publication.envelope.column_descriptors[column]
              .name_utf8,
          row.values[column]);
    }
    result.result_shape.rows.push_back(std::move(api_row));
  }
  result.evidence.push_back(
      {"canonical.selected_plan",
       execution.dispatch.selected_plan_uuid});
  result.evidence.push_back(
      {"canonical.result_abi", "QOW-RESULT-DIAGNOSTIC-ABI-V1"});
  return result;
}

}  // namespace

CanonicalObjectFreeValuesExecutionResult
ExecuteCanonicalObjectFreeValuesQuery(
    const CanonicalObjectFreeValuesExecutionRequest& request) {
  CanonicalObjectFreeValuesExecutionResult result;
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
    result.api_result = Failure(request, std::move(diagnostic_id),
                                std::move(detail));
    return result;
  };

  const auto& graph = request.optimizer_request.logical_graph;
  if (graph.nodes.size() != 1 ||
      graph.root_logical_node_id != graph.nodes.front().logical_node_id ||
      graph.nodes.front().node_kind !=
          plan::CanonicalLogicalRelationalNodeKind::kValues ||
      graph.nodes.front().semantic_variant_id != "values.literal-table.v1" ||
      !graph.nodes.front().input_logical_node_ids.empty() ||
      !graph.nodes.front().required_object_uuids.empty() ||
      !graph.nodes.front().required_property_uuids.empty() ||
      !graph.nodes.front().delivered_property_uuids.empty() ||
      !request.optimizer_request.logical_properties.properties.empty()) {
    return result;
  }
  result.profile_matched = true;
  if (!request.optimizer_admission.admitted ||
      !request.optimizer_admission.planning_allowed) {
    return refuse("QOW-DIAG-RELATIONAL-LIVE-VALUES-ADMISSION-V1",
                  "live VALUES execution lacks optimizer admission");
  }

  auto materialized = MaterializeValues(request.relational_dag,
                                        graph.nodes.front());
  if (!materialized.ok) {
    return refuse("QOW-DIAG-RELATIONAL-LIVE-VALUES-PAYLOAD-V1",
                  materialized.detail);
  }

  const auto identity_scope =
      graph.bound_sblr_tree_uuid + ":" + request.context.statement_uuid.canonical;
  const auto alternative_uuid =
      DerivedCanonicalUuid(identity_scope, "values.alternative");
  const auto capability_uuid =
      DerivedCanonicalUuid(identity_scope, "values.capability");
  const auto transformation_uuid =
      DerivedCanonicalUuid(identity_scope, "values.transformation");
  const auto cost_vector_uuid =
      DerivedCanonicalUuid(identity_scope, "values.cost-vector");
  const auto calibration_uuid =
      DerivedCanonicalUuid(identity_scope, "values.calibration");

  plan::CanonicalPhysicalAlternativeCatalog alternatives;
  alternatives.bound_sblr_tree_uuid = graph.bound_sblr_tree_uuid;
  alternatives.catalog_epoch_uuid = graph.catalog_epoch_uuid;
  alternatives.security_context_uuid = graph.security_context_uuid;
  alternatives.local_transaction_id = graph.local_transaction_id;
  alternatives.statement_snapshot_id = graph.statement_snapshot_id;
  alternatives.alternatives.push_back(
      {alternative_uuid,
       graph.nodes.front().logical_node_id,
       std::string(kValuesImplementationId),
       capability_uuid,
       graph.nodes.front().output_descriptor_ids,
       true,
       {},
       {},
       {}});

  std::uint64_t memory_bytes = 1;
  for (const auto& row : materialized.batch.rows) {
    for (const auto& value : row.values) {
      if (value.encoded_value.size() >
          std::numeric_limits<std::uint64_t>::max() - memory_bytes) {
        return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-OVERFLOW-V1",
                      "live VALUES materialization size overflowed");
      }
      memory_bytes += value.encoded_value.size();
    }
  }
  if (memory_bytes > request.optimizer_request.resource.memory_budget_bytes) {
    return refuse("QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1",
                  "live VALUES materialization exceeds the admitted memory budget");
  }

  opt::CanonicalOptimizerSearchCandidateInput candidate;
  candidate.alternative_uuid = alternative_uuid;
  candidate.transformation_uuid = transformation_uuid;
  candidate.transformation_rule_id = "canonical.values.materialize.v1";
  candidate.bound_sblr_tree_uuid = graph.bound_sblr_tree_uuid;
  candidate.statistics_snapshot_uuid =
      request.optimizer_admission.statistics_snapshot_uuid;
  candidate.statistics_generation =
      request.optimizer_admission.statistics_generation;
  candidate.model_family_id = "relational.local.v1";
  candidate.cost_terms.cost_vector_uuid = cost_vector_uuid;
  candidate.cost_terms.calibration_profile_uuid = calibration_uuid;
  candidate.cost_terms.cpu_units = materialized.batch.rows.size();
  candidate.cost_terms.memory_bytes_required = memory_bytes;
  candidate.cost_terms.confidence = opt::CostConfidence::kExact;
  candidate.semantic_preserving = true;
  candidate.derived_from_admitted_statistics = true;
  candidate.engine_coster_owned = true;

  opt::CanonicalOptimizerSearchPolicy search_policy;
  search_policy.maximum_exhaustive_plan_count = 1;
  search_policy.bounded_beam_width = 1;
  search_policy.deterministic_step_cost_ns = 1;
  search_policy.engine_owned = true;
  const auto search = opt::SearchCanonicalRelationalMemo(
      request.optimizer_request, request.optimizer_admission, alternatives,
      {candidate}, search_policy);
  if (!search.accepted || !search.selected || !search.issues.empty()) {
    const auto diagnostic = search.issues.empty()
                                ? "QOW-DIAG-OPTIMIZER-SEARCH-NO-PLAN-V1"
                                : search.issues.front().diagnostic_id;
    const auto detail = search.issues.empty()
                            ? "live VALUES search returned no selected plan"
                            : search.issues.front().field_id;
    return refuse(diagnostic, detail);
  }
  result.optimizer_selected = true;

  opt::CanonicalExecutorCapabilityCatalog capabilities;
  capabilities.capability_snapshot_uuid =
      request.optimizer_admission.capability_snapshot_uuid;
  capabilities.policy_epoch = request.optimizer_admission.policy_epoch;
  capabilities.engine_owned = true;
  opt::CanonicalExecutorCapabilityRecord capability;
  capability.capability_uuid = capability_uuid;
  capability.capability_abi_version = 1;
  capability.implementation_id = kValuesImplementationId;
  capability.logical_node_kind =
      plan::CanonicalLogicalRelationalNodeKind::kValues;
  capability.physical_node_kind = exec::PhysicalNodeKind::kValues;
  capability.maximum_memory_bytes =
      request.optimizer_request.resource.memory_budget_bytes;
  capability.spill_supported = request.optimizer_request.resource.spill_allowed;
  capability.available = true;
  capability.engine_owned = true;
  capabilities.capabilities.push_back(std::move(capability));

  opt::CanonicalOptimizerPhysicalPublicationIdentity publication_identity;
  publication_identity.selected_plan_uuid =
      DerivedCanonicalUuid(identity_scope, "values.selected-plan");
  publication_identity.first_causal_counter_id = 1;
  publication_identity.engine_owned = true;
  const auto publication = opt::PublishCanonicalPhysicalDag(
      request.optimizer_request, request.optimizer_admission, alternatives,
      search, capabilities, publication_identity);
  if (!publication.accepted || !publication.published ||
      !publication.issues.empty()) {
    const auto diagnostic = publication.issues.empty()
                                ? "QOW-DIAG-OPTIMIZER-PHYSICAL-PUBLICATION-V1"
                                : publication.issues.front().diagnostic_id;
    const auto detail = publication.issues.empty()
                            ? "live VALUES physical DAG was not published"
                            : publication.issues.front().field_id;
    return refuse(diagnostic, detail);
  }
  result.physical_dag_published = true;
  result.physical_node_count = publication.physical_dag.nodes.size();
  result.selected_plan_uuid = publication.physical_dag.selected_plan_uuid;

  exec::CanonicalPhysicalExecutorRegistration registration;
  registration.node_kind = exec::PhysicalNodeKind::kValues;
  registration.implementation_id = kValuesImplementationId;
  registration.executor_capability_uuid = capability_uuid;
  registration.executor_capability_abi_version = 1;
  registration.engine_owned = true;
  registration.accepts_optimizer_publication_v2 = true;
  registration.execute =
      [batch = materialized.batch](const exec::TypedPhysicalNodeDag& dag,
                                   const exec::PhysicalNodeRecord& node,
                                   const std::vector<
                                       exec::CanonicalPhysicalDispatchInput>&
                                       inputs) {
        exec::CanonicalPhysicalDispatchStepResult step;
        step.selected_plan_uuid = dag.selected_plan_uuid;
        step.executed_physical_node_id = node.physical_node_id;
        step.causal_counter_id = node.causal_counter_id;
        step.output_descriptor_ids = node.output_descriptor_ids;
        step.authority.engine_mga_snapshot_bound = true;
        if (!inputs.empty()) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "QOW-DIAG-RELATIONAL-LIVE-VALUES-INPUT-V1";
          step.diagnostic.detail = "VALUES executor received an input edge";
          return step;
        }
        step.result_handle_id = node.physical_node_id;
        step.output_row_count = batch.rows.size();
        step.rows_examined = batch.rows.size();
        step.materialized_output_batch = batch;
        return step;
      };

  api::CanonicalOptimizerSelectedExecutionRequest execution_request;
  execution_request.selected_physical_dag = publication.physical_dag;
  execution_request.pre_access_statistics_snapshot_uuid =
      publication.physical_dag.statistics_snapshot_uuid;
  execution_request.inventory_local_transaction_id =
      publication.physical_dag.local_transaction_id;
  execution_request.inventory_statement_snapshot_id =
      publication.physical_dag.statement_snapshot_id;
  execution_request.available_executors.push_back(std::move(registration));
  execution_request.engine_execution_authorized = true;
  execution_request.result_publication_request.statement_uuid =
      request.context.statement_uuid.canonical;
  execution_request.result_publication_request.execution_attempt_uuid =
      DerivedCanonicalUuid(
          identity_scope + ":" + request.context.current_monotonic_ns,
          "values.execution-attempt");
  execution_request.result_publication_request.transaction_effect_evidence_uuid =
      DerivedCanonicalUuid(
          identity_scope + ":" +
              std::to_string(request.context.local_transaction_id) + ":" +
              std::to_string(
                  request.context.snapshot_visible_through_local_transaction_id),
          "values.transaction-effect-unchanged");
  execution_request.result_publication_request.result_kind =
      exec::CanonicalResultKind::kRows;
  execution_request.result_publication_request.invocation_mode =
      exec::CanonicalResultInvocationMode::kDirect;
  execution_request.result_publication_request.column_bindings =
      std::move(materialized.result_bindings);
  execution_request.result_publication_request.maximum_row_count =
      std::max<std::size_t>(1, materialized.batch.rows.size());

  const auto execution =
      api::ExecuteCanonicalOptimizerSelectedDag(execution_request);
  if (!execution.accepted || !execution.exact_selected_nodes_executed ||
      !execution.causal_counters_attached ||
      !execution.canonical_result_published || !execution.issues.empty()) {
    const auto diagnostic = execution.issues.empty()
                                ? "QOW-DIAG-RELATIONAL-LIVE-VALUES-EXECUTION-V1"
                                : execution.issues.front().diagnostic_id;
    const auto detail = execution.issues.empty()
                            ? "live VALUES selected DAG was not completed"
                            : execution.issues.front().field_id;
    return refuse(diagnostic, detail);
  }
  result.physical_dag_executed = true;
  result.runtime_actuals_attached = execution.runtime_actuals.accepted;
  result.canonical_result_published =
      execution.result_publication.published;
  result.canonical_result_column_count =
      execution.result_publication.envelope.column_descriptors.size();
  result.canonical_result_row_count =
      execution.result_publication.row_stream.rows.size();
  result.canonical_result_bytes =
      execution.result_publication.canonical_envelope_bytes;
  result.api_result = SuccessfulApiResult(request, execution);
  return result;
}

}  // namespace scratchbird::engine::sblr
