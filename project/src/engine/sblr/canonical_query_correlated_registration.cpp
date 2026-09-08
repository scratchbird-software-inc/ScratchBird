// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "canonical_query_correlated_registration.hpp"

#include "canonical_query_physical_registration.hpp"
#include "canonical_query_runtime_memory_support.hpp"

#include "datatype_operations.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <limits>
#include <optional>
#include <ranges>
#include <string>
#include <utility>
#include <vector>

namespace scratchbird::engine::sblr {

namespace api = scratchbird::engine::internal_api;
namespace dt = scratchbird::core::datatypes;

// SEARCH_KEY: SB_ENGINE_CANONICAL_QUERY_CORRELATED_REGISTRATION_AUTHORITY
struct BoundCanonicalCorrelatedComparisonAuthorityV1 {
  bool ok{false};
  bool required{false};
  bool cancellation_observed{false};
  std::string diagnostic_id;
  std::string detail;
  std::vector<std::optional<int>> comparisons;
};

BoundCanonicalCorrelatedComparisonAuthorityV1
BindCanonicalCorrelatedComparisonAuthorityV1(
    const exec::DescriptorBatch& outer,
    const std::vector<std::uint32_t>& expected_outer_descriptor_ids,
    const std::size_t outer_binding_column,
    const exec::DescriptorBatch& inner,
    const std::vector<std::uint32_t>& expected_inner_descriptor_ids,
    const std::size_t inner_reference_column,
    const std::size_t maximum_pair_count,
    const std::uint64_t maximum_authority_memory_bytes,
    const CanonicalRelationalExpressionRuntimeServices& expression_services,
    const std::function<bool()>& cancellation_requested) {
  BoundCanonicalCorrelatedComparisonAuthorityV1 result;
  const auto refuse = [&](std::string diagnostic_id, std::string detail) {
    result.diagnostic_id = std::move(diagnostic_id);
    result.detail = std::move(detail);
    result.comparisons.clear();
    return result;
  };
  bool outer_validation_cancelled = false;
  const auto outer_validation = exec::ValidateCanonicalDescriptorBatch(
      outer, expected_outer_descriptor_ids, cancellation_requested,
      &outer_validation_cancelled);
  if (!outer_validation.ok) {
    result.cancellation_observed = outer_validation_cancelled;
    return refuse(outer_validation.diagnostic_code,
                  outer_validation.detail);
  }
  bool inner_validation_cancelled = false;
  const auto inner_validation = exec::ValidateCanonicalDescriptorBatch(
      inner, expected_inner_descriptor_ids, cancellation_requested,
      &inner_validation_cancelled);
  if (!inner_validation.ok) {
    result.cancellation_observed = inner_validation_cancelled;
    return refuse(inner_validation.diagnostic_code,
                  inner_validation.detail);
  }
  if (outer_binding_column >= outer.columns.size() ||
      inner_reference_column >= inner.columns.size()) {
    return refuse(
        "QOW-DIAG-RELATIONAL-LIVE-CORRELATION-AUTHORITY-V1",
        "correlation comparison authority columns are unresolved");
  }
  result.required = CanonicalRelationalComparisonAuthorityRequiredV1(
      outer.columns[outer_binding_column].descriptor,
      inner.columns[inner_reference_column].descriptor);
  if (!result.required) {
    result.ok = true;
    return result;
  }

  std::uint64_t pair_count = 0;
  std::uint64_t authority_memory_bytes = 0;
  if (!CheckedMultiply(outer.rows.size(), inner.rows.size(), &pair_count) ||
      pair_count > maximum_pair_count ||
      !CheckedMultiply(pair_count, sizeof(std::optional<int>),
                       &authority_memory_bytes) ||
      authority_memory_bytes > maximum_authority_memory_bytes ||
      pair_count > std::numeric_limits<std::size_t>::max()) {
    return refuse(
        "QOW-DIAG-OPTIMIZER-SEARCH-COST-VECTOR-V1",
        "correlation comparison authority exceeds its selected memory or "
        "pair bound");
  }

  const auto poll_cancellation = [&]() {
    if (!cancellation_requested) return false;
    try {
      if (!cancellation_requested()) return false;
      result.cancellation_observed = true;
      result.diagnostic_id =
          "QOW-DIAG-QRY-004-PHYSICAL-DISPATCH-CANCELLED-V1";
      result.detail =
          "correlation comparison authority binding was cancelled";
      return true;
    } catch (const std::exception& exception) {
      result.diagnostic_id =
          "QOW-DIAG-QRY-004-PHYSICAL-CANCELLATION-PROBE-V1";
      result.detail =
          std::string("correlation comparison authority cancellation probe "
                      "threw: ") +
          exception.what();
      return true;
    } catch (...) {
      result.diagnostic_id =
          "QOW-DIAG-QRY-004-PHYSICAL-CANCELLATION-PROBE-V1";
      result.detail =
          "correlation comparison authority cancellation probe threw a "
          "non-standard exception";
      return true;
    }
  };
  const auto validate_keys = [&](const exec::DescriptorBatch& batch,
                                 const std::size_t column) {
    for (const auto& row : batch.rows) {
      if (poll_cancellation()) return false;
      const auto& value = row.values[column];
      if (value.isSqlNull()) continue;
      std::optional<int> comparison;
      if (!BindCanonicalRelationalComparisonAuthorityV1(
              value, value, expression_services, &comparison,
              &result.detail) ||
          !comparison.has_value() || *comparison < -1 || *comparison > 1) {
        if (result.diagnostic_id.empty()) {
          result.diagnostic_id =
              "QOW-DIAG-RELATIONAL-LIVE-CORRELATION-AUTHORITY-V1";
        }
        if (result.detail.empty()) {
          result.detail =
              "correlation key authority self-validation was not canonical";
        }
        return false;
      }
    }
    return true;
  };
  if (!validate_keys(outer, outer_binding_column) ||
      !validate_keys(inner, inner_reference_column)) {
    result.comparisons.clear();
    return result;
  }

  result.comparisons.reserve(static_cast<std::size_t>(pair_count));
  for (const auto& outer_row : outer.rows) {
    const auto& outer_value = outer_row.values[outer_binding_column];
    for (const auto& inner_row : inner.rows) {
      if (poll_cancellation()) {
        result.comparisons.clear();
        return result;
      }
      const auto& inner_value = inner_row.values[inner_reference_column];
      std::optional<int> comparison;
      if (!BindCanonicalRelationalComparisonAuthorityV1(
              outer_value, inner_value, expression_services, &comparison,
              &result.detail) ||
          (comparison.has_value() &&
           (*comparison < -1 || *comparison > 1)) ||
          (comparison.has_value() !=
           (!outer_value.isSqlNull() && !inner_value.isSqlNull()))) {
        result.diagnostic_id =
            "QOW-DIAG-RELATIONAL-LIVE-CORRELATION-AUTHORITY-V1";
        if (result.detail.empty()) {
          result.detail =
              "correlation pair authority result was not canonical";
        }
        result.comparisons.clear();
        return result;
      }
      result.comparisons.push_back(comparison);
    }
  }
  result.ok = true;
  return result;
}

exec::CanonicalPhysicalExecutorRegistration
MakeLiveCorrelatedSubqueryRegistration(
    const LiveCorrelatedSubqueryRegistrationProfile prepared,
    std::string capability_uuid,
    CanonicalRelationalExpressionRuntimeServices expression_services,
    api::EngineRequestContext mga_context) {
  exec::CanonicalPhysicalExecutorRegistration registration;
  registration.node_kind = exec::PhysicalNodeKind::kSubquery;
  registration.implementation_id = prepared.implementation_id;
  registration.executor_capability_uuid = std::move(capability_uuid);
  registration.executor_capability_abi_version = 1;
  registration.engine_owned = true;
  registration.accepts_optimizer_publication_v2 = true;
  registration.execute =
      [prepared, expression_services = std::move(expression_services),
       mga_context = std::move(mga_context)](
          const exec::TypedPhysicalNodeDag& dag,
          const exec::PhysicalNodeRecord& node,
          const std::vector<exec::CanonicalPhysicalDispatchInput>& inputs) {
        exec::CanonicalPhysicalDispatchStepResult step;
        step.selected_plan_uuid = dag.selected_plan_uuid;
        step.mga_statement_context = dag.mga_statement_context;
        step.executed_physical_node_id = node.physical_node_id;
        step.causal_counter_id = node.causal_counter_id;
        step.output_descriptor_ids = node.output_descriptor_ids;
        step.authority.engine_mga_snapshot_bound = true;
        if (inputs.size() != 2 ||
            node.input_physical_node_ids.size() != 2 ||
            node.input_physical_node_ids[0] ==
                node.input_physical_node_ids[1] ||
            inputs[0].physical_node_id !=
                node.input_physical_node_ids[0] ||
            inputs[1].physical_node_id !=
                node.input_physical_node_ids[1] ||
            !inputs[0].materialized_output_batch.has_value() ||
            !inputs[1].materialized_output_batch.has_value() ||
            inputs[0].materialized_output_batch->rows.size() !=
                prepared.outer_row_count ||
            inputs[1].materialized_output_batch->rows.size() !=
                prepared.inner_row_count) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "QOW-DIAG-RELATIONAL-LIVE-SUBQUERY-INPUT-V1";
          step.diagnostic.detail =
              "correlated subquery did not receive its exact two inputs";
          return step;
        }
        const auto cancellation_policy = std::ranges::find_if(
            dag.admission_evidence,
            [](const exec::PhysicalAdmissionEvidence& evidence) {
              return evidence.stage ==
                     exec::PhysicalAdmissionStage::kPolicyCapability;
            });
        const auto cancellation_policy_count = std::ranges::count_if(
            dag.admission_evidence,
            [](const exec::PhysicalAdmissionEvidence& evidence) {
              return evidence.stage ==
                     exec::PhysicalAdmissionStage::kPolicyCapability;
            });
        if (cancellation_policy_count != 1 ||
            cancellation_policy == dag.admission_evidence.end() ||
            cancellation_policy->evidence_uuid.empty()) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "QOW-DIAG-RELATIONAL-LIVE-SUBQUERY-INPUT-V1";
          step.diagnostic.detail =
              "correlated subquery cancellation policy evidence is missing";
          return step;
        }
        const auto cancellation_evidence_uuid =
            cancellation_policy->evidence_uuid;
        const auto cancellation_requested =
            mga_context.query_cancellation_requested
                ? mga_context.query_cancellation_requested
                : std::function<bool()>([] { return false; });
        const auto poll_cancellation = [&](const char* phase) {
          try {
            if (!cancellation_requested()) return false;
            step.diagnostic.ok = false;
            step.diagnostic.diagnostic_code =
                "QOW-DIAG-QRY-004-PHYSICAL-DISPATCH-CANCELLED-V1";
            step.diagnostic.detail =
                std::string("live correlated subquery cancellation observed ") +
                phase;
            step.cancellation_observed = true;
            step.transient_state_cleanup_proven = true;
            step.cancellation_evidence_uuid = cancellation_evidence_uuid;
            return true;
          } catch (const std::exception& exception) {
            step.diagnostic.ok = false;
            step.diagnostic.diagnostic_code =
                "QOW-DIAG-QRY-004-PHYSICAL-CANCELLATION-PROBE-V1";
            step.diagnostic.detail =
                std::string("live correlated subquery cancellation probe threw: ") +
                exception.what();
            step.transient_state_cleanup_proven = true;
            return true;
          } catch (...) {
            step.diagnostic.ok = false;
            step.diagnostic.diagnostic_code =
                "QOW-DIAG-QRY-004-PHYSICAL-CANCELLATION-PROBE-V1";
            step.diagnostic.detail =
                "live correlated subquery cancellation probe threw a "
                "non-standard exception";
            step.transient_state_cleanup_proven = true;
            return true;
          }
        };
        auto comparison_authority =
            BindCanonicalCorrelatedComparisonAuthorityV1(
                *inputs[0].materialized_output_batch,
                inputs[0].output_descriptor_ids,
                prepared.outer_binding_column,
                *inputs[1].materialized_output_batch,
                inputs[1].output_descriptor_ids,
                prepared.inner_reference_column,
                prepared.pair_count, node.memory_bytes_required,
                expression_services, cancellation_requested);
        if (!comparison_authority.ok) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              comparison_authority.diagnostic_id;
          step.diagnostic.detail =
              std::move(comparison_authority.detail);
          step.cancellation_observed =
              comparison_authority.cancellation_observed;
          step.transient_state_cleanup_proven = true;
          if (step.cancellation_observed) {
            step.cancellation_evidence_uuid = cancellation_evidence_uuid;
          }
          return step;
        }
        if (comparison_authority.required !=
            prepared.comparison_authority_required) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "QOW-DIAG-RELATIONAL-LIVE-CORRELATION-AUTHORITY-V1";
          step.diagnostic.detail =
              "correlated comparison authority requirement changed after "
              "planning";
          return step;
        }

        exec::CanonicalCorrelatedSubqueryRequest correlated_request;
        correlated_request.selected_physical_node_id = node.physical_node_id;
        correlated_request.borrowed_outer_batch =
            &*inputs[0].materialized_output_batch;
        correlated_request.borrowed_inner_batch =
            &*inputs[1].materialized_output_batch;
        correlated_request.retain_bound_outer_values = false;
        correlated_request.outer_binding_column =
            prepared.outer_binding_column;
        correlated_request.outer_binding_expression_descriptor_id =
            prepared.outer_binding_descriptor_id;
        correlated_request.inner_reference_column =
            prepared.inner_reference_column;
        correlated_request.inner_reference_expression_descriptor_id =
            prepared.inner_reference_descriptor_id;
        correlated_request.maximum_scope_execution_count =
            std::max<std::size_t>(1, prepared.outer_row_count);
        correlated_request.maximum_comparison_count =
            std::max<std::size_t>(1, prepared.pair_count);
        correlated_request.maximum_result_row_count =
            std::max<std::size_t>(1, prepared.output_row_bound);
        correlated_request.comparison_authority_engine_owned =
            comparison_authority.required;
        correlated_request.precomputed_equality_comparisons =
            std::move(comparison_authority.comparisons);
        correlated_request.cancellation_requested = cancellation_requested;
        correlated_request.cancellation_evidence_uuid =
            cancellation_evidence_uuid;
        correlated_request.mga_authority =
            BuildCanonicalExecutionMgaAuthority(
                mga_context, dag);
        auto correlated =
            exec::ExecuteCanonicalCorrelatedSubquery(
                correlated_request, dag, node.physical_node_id);
        if (!correlated.diagnostic.ok) {
          step.diagnostic = std::move(correlated.diagnostic);
          step.cancellation_observed =
              correlated.cancellation_observed;
          step.transient_state_cleanup_proven =
              correlated.transient_state_cleanup_proven;
          step.cancellation_evidence_uuid =
              std::move(correlated.cancellation_evidence_uuid);
          if (step.cancellation_observed) {
            step.diagnostic.diagnostic_code =
                "QOW-DIAG-QRY-004-PHYSICAL-DISPATCH-CANCELLED-V1";
          } else if (step.diagnostic.diagnostic_code ==
                     "QOW-DIAG-QRY-013-CANCELLATION-PROBE-V1") {
            step.diagnostic.diagnostic_code =
                "QOW-DIAG-QRY-004-PHYSICAL-CANCELLATION-PROBE-V1";
          }
          return step;
        }
        if (!CanonicalOperatorExecutionReceiptMatches(
                correlated, dag, node,
                correlated_request.mga_authority.statement_context) ||
            correlated.scope_execution_count != prepared.outer_row_count ||
            correlated.scopes.size() !=
                correlated.scope_execution_count ||
            correlated.comparison_count > prepared.pair_count ||
            correlated.result_row_count > prepared.output_row_bound ||
            !correlated.transient_state_cleanup_proven ||
            correlated.cancellation_observed) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "QOW-DIAG-QRY-013-CORRELATED-REFUSAL-V1";
          step.diagnostic.detail =
              "correlated subquery execution receipt changed";
          return step;
        }
        if (poll_cancellation("before result flattening preflight")) {
          return step;
        }
        std::size_t flattened_row_count = 0;
        for (std::size_t scope_index = 0;
             scope_index < correlated.scopes.size(); ++scope_index) {
          if (poll_cancellation("while preflighting a result scope")) {
            return step;
          }
          const auto& scope = correlated.scopes[scope_index];
          if (scope.outer_row_index != scope_index ||
              flattened_row_count > correlated.result_row_count ||
              scope.output_batch.rows.size() >
                  correlated.result_row_count - flattened_row_count) {
            step.diagnostic.ok = false;
            step.diagnostic.diagnostic_code =
                "QOW-DIAG-QRY-013-CORRELATED-REFUSAL-V1";
            step.diagnostic.detail =
                "correlated subquery scope preflight changed";
            return step;
          }
          flattened_row_count += scope.output_batch.rows.size();
        }
        if (flattened_row_count != correlated.result_row_count) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "QOW-DIAG-QRY-013-CORRELATED-REFUSAL-V1";
          step.diagnostic.detail =
              "correlated subquery result cardinality changed";
          return step;
        }
        exec::DescriptorBatch output;
        output.columns =
            inputs[1].materialized_output_batch->columns;
        output.rows.reserve(flattened_row_count);
        for (std::size_t scope_index = 0;
             scope_index < correlated.scopes.size(); ++scope_index) {
          if (poll_cancellation("while flattening a result scope")) {
            return step;
          }
          auto& scope = correlated.scopes[scope_index];
          for (auto& row : scope.output_batch.rows) {
            if (poll_cancellation("while flattening a result row")) {
              return step;
            }
            output.rows.push_back(std::move(row));
          }
        }
        bool validation_stopped = false;
        const auto validated = exec::ValidateCanonicalDescriptorBatch(
            output, node.output_descriptor_ids,
            [&] {
              return poll_cancellation("while validating flattened results");
            },
            &validation_stopped);
        if (validation_stopped) return step;
        if (!validated.ok ||
            output.rows.size() != correlated.result_row_count) {
          step.diagnostic = validated;
          if (validated.ok) {
            step.diagnostic.ok = false;
            step.diagnostic.diagnostic_code =
                "QOW-DIAG-RELATIONAL-LIVE-SUBQUERY-INPUT-V1";
            step.diagnostic.detail =
                "correlated scope flattening cardinality drifted";
          }
          return step;
        }
        if (poll_cancellation("before result publication")) return step;
        step.result_handle_id = node.physical_node_id;
        step.input_row_count = prepared.outer_row_count +
                               prepared.inner_row_count;
        step.rows_examined = correlated.comparison_count;
        step.output_row_count = output.rows.size();
        step.materialized_output_batch = std::move(output);
        step.mga_statement_context =
            std::move(correlated.mga_statement_context);
        step.transient_state_cleanup_proven =
            correlated.transient_state_cleanup_proven;
        return step;
      };
  return registration;
}

exec::CanonicalPhysicalExecutorRegistration MakeLiveLateralSubqueryRegistration(
    const LiveCorrelatedSubqueryRegistrationProfile prepared,
    const LiveLateralSubqueryProfile profile,
    std::string capability_uuid,
    CanonicalRelationalExpressionRuntimeServices expression_services,
    api::EngineRequestContext mga_context,
    const bool runtime_bounded_inputs,
    const api::EngineRequestContext* borrowed_mga_context,
    const exec::CanonicalExecutionMgaAuthority* borrowed_mga_authority) {
  const bool strict_dispatcher_memory =
      borrowed_mga_context != nullptr && borrowed_mga_authority != nullptr;
  std::uint64_t registration_retained_bytes = 0;
  if (strict_dispatcher_memory) {
    registration_retained_bytes =
        sizeof(LiveCorrelatedSubqueryRegistrationProfile) +
        sizeof(LiveLateralSubqueryProfile) +
        sizeof(CanonicalRelationalExpressionRuntimeServices) +
        sizeof(api::EngineRequestContext) + 8 * sizeof(void*) + 64 * 1024;
    if (!CheckedAdd(registration_retained_bytes,
                    prepared.implementation_id.capacity() + 1,
                    &registration_retained_bytes) ||
        !CheckedAdd(registration_retained_bytes,
                    prepared.transformation_id.capacity() + 1,
                    &registration_retained_bytes) ||
        !CheckedAdd(registration_retained_bytes,
                    profile.required_operand_type.capacity() + 1,
                    &registration_retained_bytes) ||
        !CheckedAdd(registration_retained_bytes,
                    profile.implementation_id.capacity() + 1,
                    &registration_retained_bytes) ||
        !CheckedAdd(registration_retained_bytes,
                    profile.transformation_id.capacity() + 1,
                    &registration_retained_bytes)) {
      registration_retained_bytes = 0;
    }
  }
  exec::CanonicalPhysicalExecutorRegistration registration;
  registration.node_kind = exec::PhysicalNodeKind::kJoin;
  registration.implementation_id = profile.implementation_id;
  registration.executor_capability_uuid = std::move(capability_uuid);
  registration.executor_capability_abi_version = 1;
  registration.engine_owned = true;
  registration.accepts_optimizer_publication_v2 = true;
  registration.honors_dispatcher_memory_limit_v1 =
      strict_dispatcher_memory;
  registration.retained_live_memory_bytes_v1 =
      registration_retained_bytes;
  registration.execute =
      [prepared, profile,
       expression_services = std::move(expression_services),
       mga_context = std::move(mga_context), runtime_bounded_inputs,
       borrowed_mga_context, borrowed_mga_authority,
       strict_dispatcher_memory](
          const exec::TypedPhysicalNodeDag& dag,
          const exec::PhysicalNodeRecord& node,
          const std::vector<exec::CanonicalPhysicalDispatchInput>& inputs) {
        exec::CanonicalPhysicalDispatchStepResult step;
        step.selected_plan_uuid = dag.selected_plan_uuid;
        step.mga_statement_context = dag.mga_statement_context;
        step.executed_physical_node_id = node.physical_node_id;
        step.causal_counter_id = node.causal_counter_id;
        step.output_descriptor_ids = node.output_descriptor_ids;
        step.authority.engine_mga_snapshot_bound = true;
        if (inputs.size() != 2 ||
            node.input_physical_node_ids.size() != 2 ||
            node.input_physical_node_ids[0] ==
                node.input_physical_node_ids[1] ||
            inputs[0].physical_node_id !=
                node.input_physical_node_ids[0] ||
            inputs[1].physical_node_id !=
                node.input_physical_node_ids[1] ||
            !inputs[0].materialized_output_batch.has_value() ||
            !inputs[1].materialized_output_batch.has_value() ||
            (!runtime_bounded_inputs &&
             (inputs[0].materialized_output_batch->rows.size() !=
                  prepared.outer_row_count ||
              inputs[1].materialized_output_batch->rows.size() !=
                  prepared.inner_row_count))) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "QOW-DIAG-RELATIONAL-LIVE-JOIN-INPUT-V1";
          step.diagnostic.detail =
              "LATERAL/APPLY did not receive its exact two inputs";
          return step;
        }
        const auto& active_mga_context =
            borrowed_mga_context == nullptr ? mga_context
                                            : *borrowed_mga_context;
        exec::TypedPhysicalNodeDag operator_dag;
        std::size_t callback_memory_bound = 0;
        std::string detail;
        const bool operator_dag_ready =
            strict_dispatcher_memory
                ? BuildStrictBinaryOperatorLocalPhysicalDag(
                      dag, node,
                      *inputs[0].materialized_output_batch,
                      *inputs[1].materialized_output_batch,
                      prepared.comparison_authority_memory_bytes,
                      &operator_dag, &callback_memory_bound, &detail)
                : BuildOperatorLocalPhysicalDag(
                      dag, node.physical_node_id, &operator_dag, &detail);
        if (!operator_dag_ready) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code = strict_dispatcher_memory
                                                ? "SBLR.PLAN_TREE.RESOURCE_LIMIT"
                                                : "QOW-DIAG-RELATIONAL-LIVE-JOIN-INPUT-V1";
          step.diagnostic.detail = "LATERAL/APPLY " + std::move(detail);
          return step;
        }
        const auto cancellation_policy = std::ranges::find_if(
            operator_dag.admission_evidence,
            [](const exec::PhysicalAdmissionEvidence& evidence) {
              return evidence.stage ==
                     exec::PhysicalAdmissionStage::kPolicyCapability;
            });
        const auto cancellation_policy_count = std::ranges::count_if(
            operator_dag.admission_evidence,
            [](const exec::PhysicalAdmissionEvidence& evidence) {
              return evidence.stage ==
                     exec::PhysicalAdmissionStage::kPolicyCapability;
            });
        if (cancellation_policy_count != 1 ||
            cancellation_policy == operator_dag.admission_evidence.end() ||
            cancellation_policy->evidence_uuid.empty()) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "QOW-DIAG-RELATIONAL-LIVE-JOIN-INPUT-V1";
          step.diagnostic.detail =
              "LATERAL/APPLY cancellation policy evidence is missing";
          return step;
        }
        const auto cancellation_evidence_uuid =
            cancellation_policy->evidence_uuid;
        const auto cancellation_requested =
            active_mga_context.query_cancellation_requested
                ? active_mga_context.query_cancellation_requested
                : std::function<bool()>([] { return false; });
        const auto selected = std::ranges::find_if(
            operator_dag.nodes, [&](const auto& candidate) {
              return candidate.physical_node_id == node.physical_node_id;
            });
        if (selected == operator_dag.nodes.end() ||
            selected->input_physical_node_ids.size() != 2) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "QOW-DIAG-RELATIONAL-LIVE-JOIN-INPUT-V1";
          step.diagnostic.detail = "LATERAL/APPLY local root is unresolved";
          return step;
        }
        const auto inner_id = selected->input_physical_node_ids[1];
        const auto outer_row_count =
            inputs[0].materialized_output_batch->rows.size();
        const auto inner_row_count =
            inputs[1].materialized_output_batch->rows.size();
        std::uint64_t actual_pair_count = 0;
        if (!CheckedMultiply(outer_row_count, inner_row_count,
                             &actual_pair_count) ||
            outer_row_count > prepared.outer_row_count ||
            inner_row_count > prepared.inner_row_count ||
            actual_pair_count > prepared.pair_count) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "QOW-DIAG-RELATIONAL-LIVE-JOIN-INPUT-V1";
          step.diagnostic.detail =
              "LATERAL/APPLY runtime input exceeded its admitted bounds";
          return step;
        }
        auto outer_binding_column = prepared.outer_binding_column;
        auto inner_reference_column = prepared.inner_reference_column;
        auto outer_binding_descriptor_id =
            prepared.outer_binding_descriptor_id;
        auto inner_reference_descriptor_id =
            prepared.inner_reference_descriptor_id;
        if (runtime_bounded_inputs) {
          const auto& outer_columns =
              inputs[0].materialized_output_batch->columns;
          const auto& inner_columns =
              inputs[1].materialized_output_batch->columns;
          const auto required_column = [&](const auto& columns) {
            std::optional<std::size_t> selected;
            for (std::size_t ordinal = 0; ordinal < columns.size();
                 ++ordinal) {
              if (!profile.required_operand_type.empty() &&
                  columns[ordinal].descriptor.canonical_type_name !=
                      profile.required_operand_type) {
                continue;
              }
              if (selected.has_value()) return std::optional<std::size_t>{};
              selected = ordinal;
              if (profile.required_operand_type.empty()) break;
            }
            return selected;
          };
          const auto outer_column = required_column(outer_columns);
          const auto inner_column = required_column(inner_columns);
          const auto outer_type =
              !outer_column.has_value()
                  ? dt::CanonicalTypeId::unknown
                  : dt::CanonicalTypeIdFromStableName(
                        outer_columns[*outer_column]
                            .descriptor.canonical_type_name);
          const auto inner_type =
              !inner_column.has_value()
                  ? dt::CanonicalTypeId::unknown
                  : dt::CanonicalTypeIdFromStableName(
                        inner_columns[*inner_column]
                            .descriptor.canonical_type_name);
          if (outer_type == dt::CanonicalTypeId::unknown ||
              outer_type != inner_type ||
              !outer_column.has_value() || !inner_column.has_value()) {
            step.diagnostic.ok = false;
            step.diagnostic.diagnostic_code =
                "QOW-DIAG-RELATIONAL-LIVE-JOIN-INPUT-V1";
            step.diagnostic.detail =
                "LATERAL/APPLY lacks one exact type-compatible "
                "correlation handle";
            return step;
          }
          outer_binding_column = *outer_column;
          inner_reference_column = *inner_column;
          outer_binding_descriptor_id =
              outer_columns[*outer_column].descriptor_id;
          inner_reference_descriptor_id =
              inner_columns[*inner_column].descriptor_id;
        }

        auto comparison_authority =
            BindCanonicalCorrelatedComparisonAuthorityV1(
                *inputs[0].materialized_output_batch,
                inputs[0].output_descriptor_ids,
                outer_binding_column,
                *inputs[1].materialized_output_batch,
                inputs[1].output_descriptor_ids,
                inner_reference_column,
                prepared.pair_count,
                strict_dispatcher_memory ? callback_memory_bound
                                         : node.memory_bytes_required,
                expression_services, cancellation_requested);
        if (!comparison_authority.ok) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              comparison_authority.diagnostic_id;
          step.diagnostic.detail =
              std::move(comparison_authority.detail);
          step.cancellation_observed =
              comparison_authority.cancellation_observed;
          step.transient_state_cleanup_proven = true;
          if (step.cancellation_observed) {
            step.cancellation_evidence_uuid = cancellation_evidence_uuid;
          }
          return step;
        }
        if (!runtime_bounded_inputs &&
            comparison_authority.required !=
                prepared.comparison_authority_required) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "QOW-DIAG-RELATIONAL-LIVE-CORRELATION-AUTHORITY-V1";
          step.diagnostic.detail =
              "LATERAL/APPLY comparison authority requirement changed "
              "after planning";
          return step;
        }

        auto correlated_dag = operator_dag;
        auto correlated_root = std::ranges::find_if(
            correlated_dag.nodes, [&](const auto& candidate) {
              return candidate.physical_node_id == node.physical_node_id;
            });
        correlated_root->node_kind = exec::PhysicalNodeKind::kSubquery;
        correlated_root->implementation_id =
            profile.required_operand_type.empty()
                ? "subquery.correlated.equality.typed.v1"
                : "subquery.correlated.int64-equality.typed.v1";
        correlated_root->output_descriptor_ids =
            inputs[1].output_descriptor_ids;

        auto lateral_dag = std::move(operator_dag);
        auto lateral_inner = std::ranges::find_if(
            lateral_dag.nodes, [&](const auto& candidate) {
              return candidate.physical_node_id == inner_id;
            });
        if (lateral_inner == lateral_dag.nodes.end()) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "QOW-DIAG-RELATIONAL-LIVE-JOIN-INPUT-V1";
          step.diagnostic.detail =
              "LATERAL/APPLY correlated input is unresolved";
          return step;
        }
        lateral_inner->node_kind = exec::PhysicalNodeKind::kSubquery;
        lateral_inner->implementation_id =
            "subquery.lateral-correlated.typed.v1";
        lateral_inner->input_physical_node_ids.clear();

        exec::CanonicalCorrelatedSubqueryRequest correlated_request;
        correlated_request.physical_dag = std::move(correlated_dag);
        correlated_request.selected_physical_node_id = node.physical_node_id;
        correlated_request.borrowed_outer_batch =
            &*inputs[0].materialized_output_batch;
        correlated_request.borrowed_inner_batch =
            &*inputs[1].materialized_output_batch;
        correlated_request.retain_bound_outer_values = false;
        correlated_request.outer_binding_column =
            outer_binding_column;
        correlated_request.outer_binding_expression_descriptor_id =
            outer_binding_descriptor_id;
        correlated_request.inner_reference_column =
            inner_reference_column;
        correlated_request.inner_reference_expression_descriptor_id =
            inner_reference_descriptor_id;
        correlated_request.maximum_scope_execution_count =
            std::max<std::size_t>(1, prepared.outer_row_count);
        correlated_request.maximum_comparison_count =
            std::max<std::size_t>(1, prepared.pair_count);
        correlated_request.maximum_result_row_count =
            std::max<std::size_t>(1, prepared.output_row_bound);
        correlated_request.comparison_authority_engine_owned =
            comparison_authority.required;
        correlated_request.precomputed_equality_comparisons =
            std::move(comparison_authority.comparisons);
        correlated_request.cancellation_requested = cancellation_requested;
        correlated_request.cancellation_evidence_uuid =
            cancellation_evidence_uuid;
        correlated_request.mga_authority =
            strict_dispatcher_memory
                ? *borrowed_mga_authority
                : BuildCanonicalExecutionMgaAuthority(
                      active_mga_context,
                      correlated_request.physical_dag);

        exec::CanonicalLateralSubqueryRequest lateral_request;
        lateral_request.correlated_request =
            std::move(correlated_request);
        lateral_request.physical_dag = std::move(lateral_dag);
        lateral_request.selected_physical_node_id = node.physical_node_id;
        lateral_request.form = profile.form;
        lateral_request.maximum_output_row_count =
            std::max<std::size_t>(1, prepared.output_row_bound);
        lateral_request.cancellation_requested = cancellation_requested;
        lateral_request.cancellation_evidence_uuid =
            cancellation_evidence_uuid;
        lateral_request.mga_authority =
            strict_dispatcher_memory
                ? *borrowed_mga_authority
                : BuildCanonicalExecutionMgaAuthority(
                      active_mga_context, lateral_request.physical_dag);
        auto lateral =
            exec::ExecuteCanonicalLateralSubquery(lateral_request);
        if (!lateral.diagnostic.ok) {
          step.diagnostic = std::move(lateral.diagnostic);
          step.cancellation_observed = lateral.cancellation_observed;
          step.transient_state_cleanup_proven =
              lateral.transient_state_cleanup_proven;
          step.cancellation_evidence_uuid =
              std::move(lateral.cancellation_evidence_uuid);
          if (step.cancellation_observed) {
            step.diagnostic.diagnostic_code =
                "QOW-DIAG-QRY-004-PHYSICAL-DISPATCH-CANCELLED-V1";
          } else if (step.diagnostic.diagnostic_code ==
                     "QOW-DIAG-QRY-013-CANCELLATION-PROBE-V1") {
            step.diagnostic.diagnostic_code =
                "QOW-DIAG-QRY-004-PHYSICAL-CANCELLATION-PROBE-V1";
          }
          return step;
        }
        if (lateral.form != lateral_request.form ||
            lateral.correlated_plan_uuid !=
                lateral_request.correlated_request.physical_dag
                    .selected_plan_uuid ||
            lateral.selected_plan_uuid !=
                lateral_request.physical_dag.selected_plan_uuid ||
            lateral.executed_physical_node_id != node.physical_node_id ||
            lateral.causal_counter_id != node.causal_counter_id ||
            !exec::PhysicalMgaStatementContextEqual(
                lateral.mga_statement_context,
                lateral_request.mga_authority.statement_context) ||
            lateral.output_row_count != lateral.output_batch.rows.size() ||
            !lateral.transient_state_cleanup_proven) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "QOW-DIAG-RELATIONAL-LIVE-JOIN-EXECUTION-V1";
          step.diagnostic.detail =
              "LATERAL/APPLY execution receipt changed";
          return step;
        }
        step.result_handle_id = node.physical_node_id;
        step.input_row_count = outer_row_count + inner_row_count;
        step.rows_examined = actual_pair_count;
        step.output_row_count = lateral.output_batch.rows.size();
        step.materialized_output_batch = std::move(lateral.output_batch);
        step.mga_statement_context = std::move(lateral.mga_statement_context);
        step.transient_state_cleanup_proven =
            lateral.transient_state_cleanup_proven;
        return step;
      };
  return registration;
}

}  // namespace scratchbird::engine::sblr
