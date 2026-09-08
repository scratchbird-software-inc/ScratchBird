// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "canonical_query_sort_registration.hpp"

#include "canonical_query_descriptor_support.hpp"
#include "canonical_query_physical_registration.hpp"
#include "canonical_query_runtime_memory_support.hpp"
#include "canonical_query_scalar_support.hpp"

#include <algorithm>
#include <exception>
#include <limits>
#include <memory>
#include <new>
#include <ranges>
#include <string_view>
#include <utility>

namespace scratchbird::engine::sblr {

namespace api = scratchbird::engine::internal_api;

namespace {

struct ExpressionSortKeyReceiptIssueResult {
  exec::DescriptorRuntimeDiagnostic diagnostic;
  std::shared_ptr<const exec::CanonicalDescriptorSortKeyReceipt> receipt;
  std::uint64_t actual_order_key_batch_bytes = 0;
};

}  // namespace

// SEARCH_KEY: SB_ENGINE_CANONICAL_QUERY_SORT_REGISTRATION_AUTHORITY
bool MaterializeExpressionSortBatch(
    const api::TypedRelationalDag& dag,
    const std::vector<PreparedSortExpression>& expressions,
    const exec::DescriptorBatch& input_batch,
    const CanonicalRelationalExpressionRuntimeServices& expression_services,
    exec::DescriptorBatch* sort_batch,
    std::string* detail,
    const std::uint64_t maximum_batch_bytes,
    const exec::DescriptorCancellationProbe cancellation_requested,
    const void* cancellation_context,
    bool* cancellation_observed,
    bool* cancellation_probe_failed) {
  if (cancellation_observed != nullptr) *cancellation_observed = false;
  if (cancellation_probe_failed != nullptr) {
    *cancellation_probe_failed = false;
  }
  const auto poll_cancellation = [&](const char* phase) {
    if (cancellation_requested == nullptr) return false;
    try {
      if (!cancellation_requested(cancellation_context)) return false;
      if (cancellation_observed != nullptr) *cancellation_observed = true;
      *detail = std::string("expression SORT cancellation observed ") + phase;
      return true;
    } catch (const std::exception& exception) {
      if (cancellation_probe_failed != nullptr) {
        *cancellation_probe_failed = true;
      }
      *detail = std::string("expression SORT cancellation probe threw: ") +
                exception.what();
      return true;
    } catch (...) {
      if (cancellation_probe_failed != nullptr) {
        *cancellation_probe_failed = true;
      }
      *detail =
          "expression SORT cancellation probe threw a non-standard exception";
      return true;
    }
  };
  if (sort_batch == nullptr || detail == nullptr || expressions.empty() ||
      input_batch.columns.empty()) {
    if (detail != nullptr) {
      *detail = "expression SORT materialization request is incomplete";
    }
    return false;
  }
  if (poll_cancellation("before materialization")) return false;
  std::uint64_t materialized_bytes = 0;
  if (!RuntimeMaterializedBatchMemoryBytes(
          input_batch, &materialized_bytes, cancellation_requested,
          cancellation_context, cancellation_observed,
          cancellation_probe_failed)) {
    if ((cancellation_observed != nullptr && *cancellation_observed) ||
        (cancellation_probe_failed != nullptr &&
         *cancellation_probe_failed)) {
      *detail = cancellation_probe_failed != nullptr &&
                        *cancellation_probe_failed
                    ? "expression SORT cancellation probe threw while accounting input memory"
                    : "expression SORT cancellation observed while accounting input memory";
      return false;
    }
    *detail = "expression SORT input memory accounting overflowed";
    return false;
  }
  if (materialized_bytes > maximum_batch_bytes) {
    *detail = "expression SORT input exceeds its materialization ceiling";
    return false;
  }
  *sort_batch = input_batch;
  if (poll_cancellation("after input materialization")) {
    *sort_batch = {};
    return false;
  }
  detail->clear();
  for (const auto& expression : expressions) {
    if (poll_cancellation("while materializing key columns")) {
      *sort_batch = {};
      return false;
    }
    sort_batch->columns.push_back(expression.materialized_column);
  }

  CanonicalRelationalExpressionRuntime runtime(dag, expression_services);
  for (std::size_t row_ordinal = 0;
       row_ordinal < input_batch.rows.size(); ++row_ordinal) {
    if (poll_cancellation("while materializing key rows")) {
      *sort_batch = {};
      return false;
    }
    auto& sort_row = sort_batch->rows[row_ordinal];
    const auto& input_row = input_batch.rows[row_ordinal];
    sort_row.values.reserve(input_row.values.size() + expressions.size());
    for (const auto& expression : expressions) {
      if (poll_cancellation("while evaluating a key expression")) {
        *sort_batch = {};
        return false;
      }
      api::EngineTypedValue value;
      if (expression.row_independent_value.has_value()) {
        value = *expression.row_independent_value;
      } else {
        if (!runtime.EvaluateForConsumer(
                expression.expression_id, expression.expected_type,
                expression.row_binding, input_row.values,
                api::EngineCanonicalExpressionConsumer::projection, &value,
                detail)) {
          *sort_batch = {};
          return false;
        }
      }
      exec::DescriptorBatch expression_value_batch;
      expression_value_batch.columns.push_back(
          expression.materialized_column);
      exec::DescriptorTuple expression_value_row;
      expression_value_row.values.push_back(std::move(value));
      expression_value_batch.rows.push_back(
          std::move(expression_value_row));
      const auto value_validation = exec::ValidateDescriptorBatch(
          expression_value_batch, cancellation_requested,
          cancellation_context, cancellation_observed);
      if (!value_validation.ok) {
        if (value_validation.diagnostic_code ==
            "SB_MODEL_COORDINATOR_LEG_FAILED_V1") {
          if (cancellation_probe_failed != nullptr) {
            *cancellation_probe_failed = true;
          }
        }
        *detail = value_validation.diagnostic_code + ":" +
                  value_validation.detail;
        *sort_batch = {};
        return false;
      }
      value = std::move(
          expression_value_batch.rows.front().values.front());
      std::uint64_t value_bytes = 0;
      if (!CheckedAdd(value.encoded_value.size(), value.binary_value.size(),
                      &value_bytes) ||
          !CheckedAdd(materialized_bytes, value_bytes,
                      &materialized_bytes) ||
          materialized_bytes > maximum_batch_bytes) {
        *detail =
            "expression SORT value exceeds its materialization ceiling";
        *sort_batch = {};
        return false;
      }
      sort_row.values.push_back(std::move(value));
    }
  }

  std::vector<std::uint32_t> descriptor_ids;
  descriptor_ids.reserve(sort_batch->columns.size());
  for (const auto& column : sort_batch->columns) {
    if (poll_cancellation("while validating key descriptors")) {
      *sort_batch = {};
      return false;
    }
    descriptor_ids.push_back(column.descriptor_id);
  }
  if (poll_cancellation("before validating the key batch")) {
    *sort_batch = {};
    return false;
  }
  const auto canonical = exec::ValidateCanonicalDescriptorBatch(
      *sort_batch, descriptor_ids, cancellation_requested,
      cancellation_context, cancellation_observed);
  if (!canonical.ok) {
    if (canonical.diagnostic_code ==
        "SB_MODEL_COORDINATOR_LEG_FAILED_V1") {
      if (cancellation_probe_failed != nullptr) {
        *cancellation_probe_failed = true;
      }
    }
    *detail = canonical.diagnostic_code + ":" + canonical.detail;
    *sort_batch = {};
    return false;
  }
  if (poll_cancellation("after validating the key batch")) {
    *sort_batch = {};
    return false;
  }
  return true;
}

class CanonicalDescriptorSortKeyReceiptIssuer {
 public:
  static ExpressionSortKeyReceiptIssueResult Issue(
      const api::TypedRelationalDag& relational_dag,
      const std::vector<PreparedSortExpression>& expressions,
      const exec::DescriptorBatch& input_batch,
      const CanonicalRelationalExpressionRuntimeServices& expression_services,
      const exec::TypedPhysicalNodeDag& physical_dag,
      const std::uint64_t selected_physical_node_id,
      const std::vector<exec::CanonicalDescriptorOrderTerm>& order_terms,
      const std::string& ordering_property_uuid,
      const std::string& deterministic_tie_evidence_uuid,
      const std::size_t maximum_pair_comparisons,
      const std::uint64_t maximum_order_key_batch_bytes,
      const exec::CanonicalExecutionMgaAuthority& mga_authority) {
    return IssueBound(
        relational_dag, expressions, input_batch, expression_services,
        physical_dag, selected_physical_node_id, order_terms,
        ordering_property_uuid, deterministic_tie_evidence_uuid,
        maximum_pair_comparisons, maximum_order_key_batch_bytes,
        mga_authority, false, nullptr, nullptr);
  }

  static exec::CanonicalDescriptorSortResult IssueAndExecuteBorrowed(
      const api::TypedRelationalDag& relational_dag,
      const std::vector<PreparedSortExpression>& expressions,
      const exec::DescriptorBatch& input_batch,
      const CanonicalRelationalExpressionRuntimeServices& expression_services,
      const exec::TypedPhysicalNodeDag& physical_dag,
      const std::uint64_t selected_physical_node_id,
      const std::vector<exec::CanonicalDescriptorOrderTerm>& order_terms,
      const std::string& ordering_property_uuid,
      const std::string& deterministic_tie_evidence_uuid,
      const std::size_t maximum_pair_comparisons,
      const std::uint64_t maximum_order_key_batch_bytes,
      const exec::CanonicalExecutionMgaAuthority& mga_authority,
      std::uint64_t* actual_order_key_batch_bytes = nullptr,
      const exec::DescriptorCancellationProbe cancellation_requested = nullptr,
      const void* cancellation_context = nullptr) {
    if (actual_order_key_batch_bytes != nullptr) {
      *actual_order_key_batch_bytes = 0;
    }
    auto issued = IssueBound(
        relational_dag, expressions, input_batch, expression_services,
        physical_dag, selected_physical_node_id, order_terms,
        ordering_property_uuid, deterministic_tie_evidence_uuid,
        maximum_pair_comparisons, maximum_order_key_batch_bytes,
        mga_authority, true, cancellation_requested, cancellation_context);
    if (!issued.diagnostic.ok || issued.receipt == nullptr) {
      exec::CanonicalDescriptorSortResult result;
      result.diagnostic = std::move(issued.diagnostic);
      return result;
    }
    if (actual_order_key_batch_bytes != nullptr) {
      *actual_order_key_batch_bytes = issued.actual_order_key_batch_bytes;
    }
    exec::CanonicalDescriptorSortRequest request;
    request.order_key_receipt = std::move(issued.receipt);
    return exec::ExecuteCanonicalDescriptorSort(
        request, physical_dag, input_batch, cancellation_requested,
        cancellation_context);
  }

 private:
  static ExpressionSortKeyReceiptIssueResult IssueBound(
      const api::TypedRelationalDag& relational_dag,
      const std::vector<PreparedSortExpression>& expressions,
      const exec::DescriptorBatch& input_batch,
      const CanonicalRelationalExpressionRuntimeServices& expression_services,
      const exec::TypedPhysicalNodeDag& physical_dag,
      const std::uint64_t selected_physical_node_id,
      const std::vector<exec::CanonicalDescriptorOrderTerm>& order_terms,
      const std::string& ordering_property_uuid,
      const std::string& deterministic_tie_evidence_uuid,
      const std::size_t maximum_pair_comparisons,
      const std::uint64_t maximum_order_key_batch_bytes,
      const exec::CanonicalExecutionMgaAuthority& mga_authority,
      const bool borrowed_execution_carriers,
      const exec::DescriptorCancellationProbe cancellation_requested,
      const void* cancellation_context) {
    ExpressionSortKeyReceiptIssueResult result;
    const auto refuse = [&](std::string detail) {
      result.diagnostic.ok = false;
      result.diagnostic.diagnostic_code =
          "QOW-DIAG-QRY-010-ORDER-KEY-RECEIPT-REFUSAL-V1";
      result.diagnostic.detail = std::move(detail);
      result.receipt.reset();
      result.actual_order_key_batch_bytes = 0;
      return result;
    };
    const auto poll_cancellation = [&](const char* phase) {
      if (cancellation_requested == nullptr) return false;
      try {
        if (!cancellation_requested(cancellation_context)) return false;
        result.diagnostic.ok = false;
        result.diagnostic.diagnostic_code =
            "SB_MODEL_EXECUTION_CANCELLED_V1";
        result.diagnostic.detail =
            std::string("expression sort-key cancellation observed ") + phase;
      } catch (const std::exception& exception) {
        result.diagnostic.ok = false;
        result.diagnostic.diagnostic_code =
            "SB_MODEL_COORDINATOR_LEG_FAILED_V1";
        result.diagnostic.detail =
            std::string("expression sort-key cancellation probe threw: ") +
            exception.what();
      } catch (...) {
        result.diagnostic.ok = false;
        result.diagnostic.diagnostic_code =
            "SB_MODEL_COORDINATOR_LEG_FAILED_V1";
        result.diagnostic.detail =
            "expression sort-key cancellation probe threw a non-standard exception";
      }
      result.receipt.reset();
      result.actual_order_key_batch_bytes = 0;
      return true;
    };

    if (poll_cancellation("before authority validation")) return result;
    const auto before = exec::RevalidateCanonicalExecutionMgaAuthority(
        mga_authority, physical_dag);
    if (!before.ok) {
      result.diagnostic = before;
      return result;
    }
    if (!CanonicalUuidText(ordering_property_uuid) ||
        !CanonicalUuidText(deterministic_tie_evidence_uuid) ||
        ordering_property_uuid == deterministic_tie_evidence_uuid) {
      return refuse(
          "expression ordering property and deterministic tie evidence are "
          "not independent");
    }

    const auto selected_node = std::ranges::find_if(
        physical_dag.nodes, [&](const auto& node) {
          return node.physical_node_id == selected_physical_node_id;
        });
    if (selected_node == physical_dag.nodes.end() ||
        selected_physical_node_id == 0 ||
        selected_physical_node_id != physical_dag.root_physical_node_id ||
        selected_node->node_kind != exec::PhysicalNodeKind::kSort ||
        selected_node->implementation_id !=
            "sort.typed.expression-row.v1" ||
        selected_node->input_physical_node_ids.size() != 1 ||
        selected_node->delivered_property_uuids !=
            std::vector<std::string>{ordering_property_uuid} ||
        selected_node->enforced_property_uuids !=
            std::vector<std::string>{ordering_property_uuid} ||
        (!selected_node->required_property_uuids.empty() &&
         selected_node->required_property_uuids !=
             std::vector<std::string>{ordering_property_uuid}) ||
        relational_dag.bound_sblr_tree_uuid !=
            physical_dag.bound_sblr_tree_uuid ||
        maximum_pair_comparisons == 0 ||
        maximum_order_key_batch_bytes == 0 ||
        maximum_order_key_batch_bytes > physical_dag.memory_budget_bytes) {
      return refuse(
          "expression order-key receipt is not bound to its selected plan, "
          "operator, property, or resource ceiling");
    }
    const auto input_node = std::ranges::find_if(
        physical_dag.nodes, [&](const auto& node) {
          return node.physical_node_id ==
                 selected_node->input_physical_node_ids.front();
        });
    if (input_node == physical_dag.nodes.end() ||
        selected_node->output_descriptor_ids !=
            input_node->output_descriptor_ids) {
      return refuse("expression order-key physical schema is not preserved");
    }
    auto validation = exec::ValidateCanonicalDescriptorBatch(
        input_batch, input_node->output_descriptor_ids,
        cancellation_requested, cancellation_context, nullptr);
    if (!validation.ok) {
      result.diagnostic = std::move(validation);
      return result;
    }

    std::uint64_t input_memory_bytes = 0;
    std::uint64_t comparison_memory_bytes = 0;
    std::uint64_t row_order_memory_bytes = 0;
    std::uint64_t fixed_memory_bytes = 0;
    bool input_memory_cancelled = false;
    bool input_memory_probe_failed = false;
    if (!RuntimeMaterializedBatchMemoryBytes(
            input_batch, &input_memory_bytes, cancellation_requested,
            cancellation_context, &input_memory_cancelled,
            &input_memory_probe_failed)) {
      if (input_memory_cancelled || input_memory_probe_failed) {
        result.diagnostic.ok = false;
        result.diagnostic.diagnostic_code =
            input_memory_probe_failed
                ? "SB_MODEL_COORDINATOR_LEG_FAILED_V1"
                : "SB_MODEL_EXECUTION_CANCELLED_V1";
        result.diagnostic.detail = input_memory_probe_failed
            ? "expression sort-key cancellation probe threw while accounting input memory"
            : "expression sort-key cancellation observed while accounting input memory";
        return result;
      }
      return refuse(
          "expression order-key input memory accounting overflowed");
    }
    if (!CheckedMultiply(input_batch.rows.size(), input_batch.rows.size(),
                         &comparison_memory_bytes) ||
        !CheckedMultiply(input_batch.rows.size(), sizeof(std::size_t),
                         &row_order_memory_bytes) ||
        !CheckedAdd(input_memory_bytes, input_memory_bytes,
                    &fixed_memory_bytes) ||
        !CheckedAdd(fixed_memory_bytes, comparison_memory_bytes,
                    &fixed_memory_bytes) ||
        !CheckedAdd(fixed_memory_bytes, row_order_memory_bytes,
                    &fixed_memory_bytes) ||
        selected_node->memory_bytes_required == 0 ||
        selected_node->memory_bytes_required > physical_dag.memory_budget_bytes ||
        selected_node->memory_bytes_required >
            static_cast<std::uint64_t>(
                std::numeric_limits<std::size_t>::max()) ||
        fixed_memory_bytes >= selected_node->memory_bytes_required) {
      return refuse(
          "expression order-key fixed runtime state exceeds the selected "
          "sort-node memory grant");
    }
    const auto selected_order_key_batch_bytes = std::min(
        maximum_order_key_batch_bytes,
        selected_node->memory_bytes_required - fixed_memory_bytes);
    if (selected_order_key_batch_bytes == 0) {
      return refuse(
          "expression order-key batch has no selected sort-node memory "
          "allowance");
    }

    const auto logical_node = std::ranges::find_if(
        relational_dag.nodes, [&](const auto& node) {
          return node.node_id == selected_node->relational_node_id;
        });
    const auto property = std::ranges::find_if(
        relational_dag.properties, [&](const auto& candidate) {
          return candidate.property_uuid == ordering_property_uuid;
        });
    std::vector<std::uint32_t> expression_ids;
    std::vector<std::uint32_t> result_descriptor_ids;
    expression_ids.reserve(expressions.size());
    result_descriptor_ids.reserve(expressions.size());
    for (const auto& expression : expressions) {
      if (poll_cancellation("while binding expression identities")) {
        return result;
      }
      expression_ids.push_back(expression.expression_id);
      result_descriptor_ids.push_back(
          expression.materialized_column.descriptor_id);
    }
    if (logical_node == relational_dag.nodes.end() ||
        logical_node->node_kind != api::RelationalDagNodeKind::kSort ||
        logical_node->semantic_variant_id != "sort.required-order.v1" ||
        logical_node->bound_expression_ids != expression_ids ||
        logical_node->required_property_uuids !=
            std::vector<std::string>{ordering_property_uuid} ||
        logical_node->delivered_property_uuids !=
            std::vector<std::string>{ordering_property_uuid} ||
        property == relational_dag.properties.end() ||
        property->property_kind != api::RelationalPropertyKind::kOrdering ||
        property->origin_node_id != logical_node->node_id ||
        !property->expression_ids.empty() ||
        property->ordering_terms.size() != expressions.size() ||
        order_terms.size() != expressions.size() || expressions.empty()) {
      return refuse(
          "expression order-key receipt lacks exact logical ordering "
          "authority");
    }

    for (std::size_t ordinal = 0; ordinal < expressions.size(); ++ordinal) {
      if (poll_cancellation("while validating expression order terms")) {
        return result;
      }
      const auto& expression = expressions[ordinal];
      const auto expression_record = std::ranges::find_if(
          relational_dag.expressions, [&](const auto& candidate) {
            return candidate.expression_id == expression.expression_id;
          });
      const auto& property_term = property->ordering_terms[ordinal];
      const auto& order_term = order_terms[ordinal];
      const auto key_column = input_batch.columns.size() + ordinal;
      const auto materialized_type_uuid = ExactEncodedDescriptorField(
          expression.materialized_column.descriptor.encoded_descriptor,
          "type_uuid");
      const bool ascending =
          property_term.direction ==
          api::RelationalPropertySortDirection::kAscending;
      const bool nulls_first =
          property_term.null_placement ==
          api::RelationalPropertyNullPlacement::kNullsFirst;
      if (expression_record == relational_dag.expressions.end() ||
          expression_record->result_descriptor_id !=
              expression.materialized_column.descriptor_id ||
          property_term.expression_id != expression.expression_id ||
          property_term.collation_uuid != order_term.collation_uuid ||
          (order_term.direction ==
               exec::CanonicalDescriptorOrderDirection::ascending) !=
              ascending ||
          (order_term.null_placement ==
               exec::CanonicalDescriptorNullPlacement::first) !=
              nulls_first ||
          order_term.column != key_column ||
          order_term.expression_descriptor_id !=
              expression.materialized_column.descriptor_id ||
          !materialized_type_uuid.has_value() ||
          !CanonicalUuidText(
              expression.materialized_column.descriptor.descriptor_uuid
                  .canonical) ||
          !CanonicalUuidText(*materialized_type_uuid) ||
          deterministic_tie_evidence_uuid ==
              expression.materialized_column.descriptor.descriptor_uuid
                  .canonical ||
          deterministic_tie_evidence_uuid == *materialized_type_uuid ||
          (!property_term.collation_uuid.empty() &&
           deterministic_tie_evidence_uuid ==
               property_term.collation_uuid)) {
        return refuse(
            "expression order-key term differs from its typed relational "
            "property");
      }
    }

    exec::DescriptorBatch order_key_batch;
    std::string materialization_detail;
    bool materialization_cancelled = false;
    bool materialization_probe_failed = false;
    if (!MaterializeExpressionSortBatch(
            relational_dag, expressions, input_batch, expression_services,
            &order_key_batch, &materialization_detail,
            selected_order_key_batch_bytes, cancellation_requested,
            cancellation_context, &materialization_cancelled,
            &materialization_probe_failed)) {
      if (materialization_cancelled || materialization_probe_failed) {
        result.diagnostic.ok = false;
        result.diagnostic.diagnostic_code =
            materialization_probe_failed
                ? "SB_MODEL_COORDINATOR_LEG_FAILED_V1"
                : "SB_MODEL_EXECUTION_CANCELLED_V1";
        result.diagnostic.detail = std::move(materialization_detail);
        return result;
      }
      return refuse("expression order-key materialization: " +
                    materialization_detail);
    }
    std::uint64_t actual_order_key_batch_bytes = 1;
    std::uint64_t pair_comparisons = 0;
    bool key_memory_cancelled = false;
    bool key_memory_probe_failed = false;
    if (!AddBatchMemoryBytes(
            order_key_batch, &actual_order_key_batch_bytes,
            cancellation_requested, cancellation_context,
            &key_memory_cancelled, &key_memory_probe_failed)) {
      if (key_memory_cancelled || key_memory_probe_failed) {
        result.diagnostic.ok = false;
        result.diagnostic.diagnostic_code =
            key_memory_probe_failed
                ? "SB_MODEL_COORDINATOR_LEG_FAILED_V1"
                : "SB_MODEL_EXECUTION_CANCELLED_V1";
        result.diagnostic.detail = key_memory_probe_failed
            ? "expression sort-key cancellation probe threw while accounting key memory"
            : "expression sort-key cancellation observed while accounting key memory";
        return result;
      }
      return refuse(
          "expression order-key payload memory accounting overflowed");
    }
    if (actual_order_key_batch_bytes > selected_order_key_batch_bytes ||
        !CheckedMultiply(input_batch.rows.size(), input_batch.rows.size(),
                         &pair_comparisons) ||
        pair_comparisons > maximum_pair_comparisons) {
      return refuse(
          "expression order-key materialization exceeded its selected "
          "resource ceiling");
    }

    const auto after = exec::RevalidateCanonicalExecutionMgaAuthority(
        mga_authority, physical_dag);
    if (!after.ok) {
      result.diagnostic = after;
      return result;
    }
    if (poll_cancellation("before receipt publication")) return result;

    try {
      auto receipt =
          std::shared_ptr<exec::CanonicalDescriptorSortKeyReceipt>(
              new exec::CanonicalDescriptorSortKeyReceipt());
      if (!borrowed_execution_carriers) {
        receipt->physical_dag_ = physical_dag;
        receipt->input_batch_ = input_batch;
      }
      receipt->selected_physical_node_id_ = selected_physical_node_id;
      receipt->order_key_batch_ = std::move(order_key_batch);
      receipt->order_terms_ = order_terms;
      receipt->expression_ids_ = std::move(expression_ids);
      receipt->result_descriptor_ids_ =
          std::move(result_descriptor_ids);
      receipt->ordering_property_uuid_ = ordering_property_uuid;
      receipt->deterministic_tie_evidence_uuid_ =
          deterministic_tie_evidence_uuid;
      receipt->maximum_pair_comparisons_ = maximum_pair_comparisons;
      receipt->maximum_order_key_batch_bytes_ =
          selected_order_key_batch_bytes;
      receipt->mga_authority_ = mga_authority;
      receipt->exact_current_revalidated_before_issue_ = true;
      receipt->borrowed_execution_carriers_ =
          borrowed_execution_carriers;
      result.receipt = std::move(receipt);
    } catch (const std::bad_alloc&) {
      return refuse("expression order-key receipt allocation failed");
    }
    if (poll_cancellation("after receipt publication")) return result;
    result.actual_order_key_batch_bytes = actual_order_key_batch_bytes;
    result.diagnostic = {};
    return result;
  }
};

exec::CanonicalPhysicalExecutorRegistration
MakeLiveExpressionSortRegistration(
    PreparedSortRoot prepared,
    std::string deterministic_tie_evidence_uuid,
    std::string capability_uuid,
    const std::size_t maximum_input_row_count,
    const std::size_t maximum_pair_comparisons,
    api::TypedRelationalDag relational_dag,
    CanonicalRelationalExpressionRuntimeServices expression_services,
    api::EngineRequestContext mga_context,
    const api::TypedRelationalDag* borrowed_relational_dag,
    const api::EngineRequestContext* borrowed_mga_context,
    const exec::CanonicalExecutionMgaAuthority* borrowed_mga_authority) {
  const bool strict_dispatcher_memory =
      borrowed_relational_dag != nullptr && borrowed_mga_context != nullptr &&
      borrowed_mga_authority != nullptr;
  const bool prepared_expression_ordering =
      prepared.expression_ordering && !prepared.expressions.empty() &&
      prepared.order_terms.size() == prepared.expressions.size() &&
      !prepared.ordering_property_uuid.empty();
  std::uint64_t registration_retained_bytes =
      strict_dispatcher_memory
          ? sizeof(std::vector<exec::CanonicalDescriptorOrderTerm>) +
                sizeof(std::vector<PreparedSortExpression>) +
                sizeof(CanonicalRelationalExpressionRuntimeServices) +
                sizeof(api::EngineRequestContext) + 16 * sizeof(void*) + 1024
          : 0;
  const auto account_array = [&](const std::size_t count,
                                 const std::size_t width) {
    std::uint64_t bytes = 0;
    return CheckedMultiply(count, width, &bytes) &&
           CheckedAdd(registration_retained_bytes, bytes,
                      &registration_retained_bytes);
  };
  const auto account_string = [&](const std::string& value) {
    return value.capacity() != std::numeric_limits<std::size_t>::max() &&
           CheckedAdd(registration_retained_bytes,
                      static_cast<std::uint64_t>(value.capacity()) + 1,
                      &registration_retained_bytes);
  };
  if (!strict_dispatcher_memory ||
      !account_array(prepared.order_terms.capacity(),
                     sizeof(exec::CanonicalDescriptorOrderTerm)) ||
      !account_array(prepared.expressions.capacity(),
                     sizeof(PreparedSortExpression)) ||
      !account_string(prepared.ordering_property_uuid) ||
      !account_string(deterministic_tie_evidence_uuid)) {
    registration_retained_bytes = 0;
  }
  for (const auto& term : prepared.order_terms) {
    if (registration_retained_bytes == 0 ||
        !account_string(term.collation_uuid) ||
        !account_string(term.text_seed.seed_pack_name) ||
        !account_string(term.text_seed.seed_pack_version) ||
        !account_string(term.text_seed.charset_name) ||
        !account_string(term.text_seed.collation_name) ||
        !account_string(term.timezone_seed.seed_pack_name) ||
        !account_string(term.timezone_seed.seed_pack_version) ||
        !account_string(term.timezone_seed.content_hash) ||
        !account_array(term.timezone_seed.timezone_names.capacity(),
                       sizeof(std::string))) {
      registration_retained_bytes = 0;
      break;
    }
    for (const auto& name : term.timezone_seed.timezone_names) {
      if (!account_string(name)) {
        registration_retained_bytes = 0;
        break;
      }
    }
  }
  for (const auto& expression : prepared.expressions) {
    const auto& column = expression.materialized_column;
    if (registration_retained_bytes == 0 ||
        !account_string(expression.expected_type) ||
        !account_array(expression.row_binding.row_descriptor_ids.capacity(),
                       sizeof(std::uint32_t)) ||
        !account_array((expression.row_binding.row_nullable.capacity() + 63) /
                           64,
                       sizeof(std::uint64_t)) ||
        !account_array(expression.row_binding.slots.capacity(),
                       sizeof(CanonicalRelationalExpressionRowSlotBinding)) ||
        !account_string(column.stable_name) ||
        !account_string(column.descriptor.descriptor_uuid.canonical) ||
        !account_string(column.descriptor.descriptor_kind) ||
        !account_string(column.descriptor.canonical_type_name) ||
        !account_string(column.descriptor.encoded_descriptor)) {
      registration_retained_bytes = 0;
      break;
    }
    if (expression.row_independent_value.has_value()) {
      const auto& value = *expression.row_independent_value;
      if (!account_string(value.descriptor.descriptor_uuid.canonical) ||
          !account_string(value.descriptor.descriptor_kind) ||
          !account_string(value.descriptor.canonical_type_name) ||
          !account_string(value.descriptor.encoded_descriptor) ||
          !account_string(value.encoded_value) ||
          !account_array(value.binary_value.capacity(), sizeof(std::uint8_t))) {
        registration_retained_bytes = 0;
        break;
      }
    }
  }
  exec::CanonicalPhysicalExecutorRegistration registration;
  registration.node_kind = exec::PhysicalNodeKind::kSort;
  registration.implementation_id = "sort.typed.expression-row.v1";
  registration.executor_capability_uuid = std::move(capability_uuid);
  registration.executor_capability_abi_version = 1;
  registration.engine_owned = true;
  registration.accepts_optimizer_publication_v2 = true;
  registration.publishes_runtime_observation_v1 = true;
  registration.honors_dispatcher_memory_limit_v1 = strict_dispatcher_memory;
  registration.retained_live_memory_bytes_v1 = registration_retained_bytes;
  registration.execute =
      [order_terms = std::move(prepared.order_terms),
       expressions = std::move(prepared.expressions),
       ordering_property_uuid =
           std::move(prepared.ordering_property_uuid),
       deterministic_tie_evidence_uuid =
           std::move(deterministic_tie_evidence_uuid),
       maximum_input_row_count, maximum_pair_comparisons,
       prepared_expression_ordering,
       relational_dag = strict_dispatcher_memory
                            ? api::TypedRelationalDag{}
                            : std::move(relational_dag),
       expression_services = std::move(expression_services),
       mga_context = std::move(mga_context), borrowed_relational_dag,
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
        const auto* cancellation_policy =
            FindLiveCancellationPolicy(dag);
        const auto& active_mga_context =
            borrowed_mga_context == nullptr ? mga_context
                                            : *borrowed_mga_context;
        const void* cancellation_context =
            active_mga_context.query_cancellation_requested
                ? &active_mga_context.query_cancellation_requested
                : nullptr;
        const exec::DescriptorCancellationProbe cancellation_probe =
            cancellation_context == nullptr
                ? nullptr
                : &InvokeLiveSortCancellationProbe;
        if (cancellation_policy == nullptr) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "QOW-DIAG-RELATIONAL-LIVE-SORT-CANCELLATION-POLICY-V1";
          step.diagnostic.detail =
              "expression SORT requires one exact cancellation policy evidence row";
          return step;
        }
        if (!prepared_expression_ordering || inputs.size() != 1 ||
            node.input_physical_node_ids.size() != 1 ||
            inputs.front().physical_node_id !=
                node.input_physical_node_ids.front() ||
            !inputs.front().materialized_output_batch.has_value() ||
            inputs.front().materialized_output_batch->rows.size() >
                maximum_input_row_count) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "QOW-DIAG-RELATIONAL-LIVE-SORT-INPUT-V1";
          step.diagnostic.detail =
              "expression SORT did not receive its bounded typed input "
              "batch";
          return step;
        }
        const auto& input_batch = *inputs.front().materialized_output_batch;
        auto input_validation = exec::ValidateCanonicalDescriptorBatch(
            input_batch, inputs.front().output_descriptor_ids,
            cancellation_probe, cancellation_context, nullptr);
        if (!input_validation.ok) {
          BindLiveCancellationFailure(std::move(input_validation),
                                      cancellation_policy, &step);
          return step;
        }
        const exec::TypedPhysicalNodeDag* execution_dag = &dag;
        std::optional<exec::TypedPhysicalNodeDag> scoped_execution_dag;
        std::size_t callback_memory_bound = 0;
        if (strict_dispatcher_memory) {
          std::uint64_t comparison_memory_bytes = 0;
          std::uint64_t row_order_memory_bytes = 0;
          std::uint64_t auxiliary_memory_bytes = 64 * 1024;
          if (!CheckedMultiply(input_batch.rows.size(),
                               input_batch.rows.size(),
                               &comparison_memory_bytes) ||
              !CheckedMultiply(input_batch.rows.size(), sizeof(std::size_t),
                               &row_order_memory_bytes) ||
              !CheckedAdd(auxiliary_memory_bytes, comparison_memory_bytes,
                          &auxiliary_memory_bytes) ||
              !CheckedAdd(auxiliary_memory_bytes, row_order_memory_bytes,
                          &auxiliary_memory_bytes)) {
            step.diagnostic.ok = false;
            step.diagnostic.diagnostic_code =
                "SBLR.PLAN_TREE.RESOURCE_LIMIT";
            step.diagnostic.detail =
                "expression SORT retained workspace overflows";
            return step;
          }
          scoped_execution_dag.emplace();
          std::string scope_detail;
          if (!BuildStrictUnaryOperatorLocalPhysicalDag(
                  dag, node, input_batch, 1, auxiliary_memory_bytes,
                  &*scoped_execution_dag, &callback_memory_bound,
                  &scope_detail)) {
            step.diagnostic.ok = false;
            step.diagnostic.diagnostic_code =
                "SBLR.PLAN_TREE.RESOURCE_LIMIT";
            step.diagnostic.detail =
                "expression SORT " + std::move(scope_detail);
            return step;
          }
          execution_dag = &*scoped_execution_dag;
        } else if (node.physical_node_id != dag.root_physical_node_id) {
          scoped_execution_dag.emplace();
          std::string scope_detail;
          if (!BuildOperatorLocalPhysicalDag(
                  dag, node.physical_node_id, &*scoped_execution_dag,
                  &scope_detail)) {
            step.diagnostic.ok = false;
            step.diagnostic.diagnostic_code =
                "QOW-DIAG-RELATIONAL-LIVE-SORT-INPUT-V1";
            step.diagnostic.detail =
                "expression SORT execution view is unresolved";
            return step;
          }
          execution_dag = &*scoped_execution_dag;
        }
        const auto& active_relational_dag =
            strict_dispatcher_memory ? *borrowed_relational_dag
                                     : relational_dag;
        const auto mga_authority =
            strict_dispatcher_memory
                ? *borrowed_mga_authority
                : BuildCanonicalExecutionMgaAuthority(mga_context,
                                                       *execution_dag);
        std::uint64_t actual_order_key_batch_bytes = 0;
        auto sorted = CanonicalDescriptorSortKeyReceiptIssuer::
            IssueAndExecuteBorrowed(
                active_relational_dag, expressions, input_batch,
                expression_services, *execution_dag, node.physical_node_id,
                order_terms, ordering_property_uuid,
                deterministic_tie_evidence_uuid, maximum_pair_comparisons,
                strict_dispatcher_memory ? callback_memory_bound
                                         : execution_dag->memory_budget_bytes,
                mga_authority,
                &actual_order_key_batch_bytes, cancellation_probe,
                cancellation_context);
        if (!sorted.diagnostic.ok) {
          BindLiveCancellationFailure(std::move(sorted.diagnostic),
                                      cancellation_policy, &step);
          return step;
        }
        if (!CanonicalOperatorExecutionReceiptMatches(
                sorted, *execution_dag, node,
                mga_authority.statement_context) ||
            sorted.output_batch.rows.size() != input_batch.rows.size()) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "QOW-DIAG-RELATIONAL-LIVE-SORT-EXECUTION-V1";
          step.diagnostic.detail =
              "expression SORT execution receipt changed";
          return step;
        }
        auto output_validation = exec::ValidateCanonicalDescriptorBatch(
            sorted.output_batch, node.output_descriptor_ids,
            cancellation_probe, cancellation_context, nullptr);
        if (!output_validation.ok) {
          BindLiveCancellationFailure(std::move(output_validation),
                                      cancellation_policy, &step);
          return step;
        }
        std::uint64_t input_memory_bytes = 0;
        std::uint64_t current_memory_bytes = 0;
        std::uint64_t comparison_memory_bytes = 0;
        std::uint64_t row_order_memory_bytes = 0;
        std::uint64_t peak_memory_bytes = 0;
        bool memory_cancelled = false;
        bool memory_probe_failed = false;
        const auto account_batch_memory =
            [&](const exec::DescriptorBatch& batch,
                std::uint64_t* memory_bytes) {
              return RuntimeMaterializedBatchMemoryBytes(
                  batch, memory_bytes, cancellation_probe,
                  cancellation_context, &memory_cancelled,
                  &memory_probe_failed);
            };
        if (!account_batch_memory(input_batch, &input_memory_bytes) ||
            !account_batch_memory(sorted.output_batch,
                                  &current_memory_bytes)) {
          if (memory_cancelled || memory_probe_failed) {
            exec::DescriptorRuntimeDiagnostic diagnostic;
            diagnostic.ok = false;
            diagnostic.diagnostic_code =
                memory_probe_failed
                    ? "SB_MODEL_COORDINATOR_LEG_FAILED_V1"
                    : "SB_MODEL_EXECUTION_CANCELLED_V1";
            diagnostic.detail = memory_probe_failed
                ? "expression SORT cancellation probe threw while accounting runtime memory"
                : "expression SORT cancellation observed while accounting runtime memory";
            BindLiveCancellationFailure(std::move(diagnostic),
                                        cancellation_policy, &step);
            return step;
          }
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "QOW-DIAG-OPT-017-REFUSAL-V1";
          step.diagnostic.detail =
              "expression SORT runtime memory observation overflowed";
          return step;
        }
        if (!CheckedMultiply(input_batch.rows.size(),
                             input_batch.rows.size(),
                             &comparison_memory_bytes) ||
            !CheckedMultiply(input_batch.rows.size(), sizeof(std::size_t),
                             &row_order_memory_bytes) ||
            !CheckedAdd(input_memory_bytes, input_memory_bytes,
                        &peak_memory_bytes) ||
            !CheckedAdd(peak_memory_bytes, actual_order_key_batch_bytes,
                        &peak_memory_bytes) ||
            !CheckedAdd(peak_memory_bytes, comparison_memory_bytes,
                        &peak_memory_bytes) ||
            !CheckedAdd(peak_memory_bytes, row_order_memory_bytes,
                        &peak_memory_bytes) ||
            (strict_dispatcher_memory &&
             peak_memory_bytes > callback_memory_bound)) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "QOW-DIAG-OPT-017-REFUSAL-V1";
          step.diagnostic.detail =
              "expression SORT runtime memory observation overflowed";
          return step;
        }
        step.result_handle_id = node.physical_node_id;
        step.input_row_count = input_batch.rows.size();
        step.rows_examined = input_batch.rows.size();
        step.output_row_count = sorted.output_batch.rows.size();
        step.materialized_output_batch = std::move(sorted.output_batch);
        step.mga_statement_context =
            std::move(sorted.mga_statement_context);
        PublishRuntimeMemoryObservation(
            &step, current_memory_bytes, peak_memory_bytes);
        return step;
      };
  return registration;
}

}  // namespace scratchbird::engine::sblr
