// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "canonical_query_predicate_support.hpp"
#include "canonical_query_runtime_memory_support.hpp"
#include "canonical_query_scalar_support.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <new>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace scratchbird::engine::sblr {
namespace api = scratchbird::engine::internal_api;
namespace exec = scratchbird::engine::executor;
namespace dt = scratchbird::core::datatypes;

// SEARCH_KEY: SB_ENGINE_CANONICAL_QUERY_FILTER_PREDICATE_RECEIPT_AUTHORITY
// Issues private FILTER/HAVING receipts over supplied typed plans and inputs,
// revalidates supplied MGA authority, and invokes the bound filter executor.
// Owns no plan selection, snapshot construction, or transaction finality.

namespace {

struct FilterPredicateReceiptIssueResult {
  exec::DescriptorRuntimeDiagnostic diagnostic;
  std::shared_ptr<const exec::CanonicalDescriptorFilterPredicateReceipt>
      receipt;
};

}  // namespace

class CanonicalDescriptorFilterPredicateReceiptIssuer {
 public:
  static FilterPredicateReceiptIssueResult Issue(
      const api::TypedRelationalDag& relational_dag,
      const std::uint32_t predicate_expression_id,
      const CanonicalRelationalExpressionRowBinding& predicate_row_binding,
      const exec::DescriptorBatch& input_batch,
      const CanonicalRelationalExpressionRuntimeServices& expression_services,
      const api::EngineCanonicalExpressionConsumer expression_consumer,
      const api::EnginePredicateConsumer predicate_consumer,
      const exec::TypedPhysicalNodeDag& physical_dag,
      const std::uint64_t selected_physical_node_id,
      const std::size_t maximum_input_row_count,
      const exec::CanonicalExecutionMgaAuthority& mga_authority) {
    return IssueBound(
        relational_dag, predicate_expression_id, predicate_row_binding,
        input_batch, expression_services, expression_consumer,
        predicate_consumer, physical_dag, selected_physical_node_id,
        physical_dag.root_physical_node_id, maximum_input_row_count,
        mga_authority, false);
  }

  static exec::CanonicalDescriptorFilterResult IssueAndExecuteBorrowed(
      const api::TypedRelationalDag& relational_dag,
      const std::uint32_t predicate_expression_id,
      const CanonicalRelationalExpressionRowBinding& predicate_row_binding,
      const exec::DescriptorBatch& input_batch,
      const CanonicalRelationalExpressionRuntimeServices& expression_services,
      const api::EngineCanonicalExpressionConsumer expression_consumer,
      const api::EnginePredicateConsumer predicate_consumer,
      const exec::TypedPhysicalNodeDag& physical_dag,
      const std::uint64_t selected_physical_node_id,
      const std::uint64_t scoped_root_physical_node_id,
      const std::size_t maximum_input_row_count,
      const exec::CanonicalExecutionMgaAuthority& mga_authority,
      const bool borrow_mga_authority = false) {
    auto issued = IssueBound(
        relational_dag, predicate_expression_id, predicate_row_binding,
        input_batch, expression_services, expression_consumer,
        predicate_consumer, physical_dag, selected_physical_node_id,
        scoped_root_physical_node_id, maximum_input_row_count, mga_authority,
        true, borrow_mga_authority);
    if (!issued.diagnostic.ok || !issued.receipt) {
      exec::CanonicalDescriptorFilterResult result;
      result.diagnostic = std::move(issued.diagnostic);
      return result;
    }
    exec::CanonicalDescriptorFilterRequest request;
    request.predicate_receipt = std::move(issued.receipt);
    return exec::ExecuteCanonicalDescriptorFilter(
        request, physical_dag, scoped_root_physical_node_id, input_batch);
  }

 private:
  static FilterPredicateReceiptIssueResult IssueBound(
      const api::TypedRelationalDag& relational_dag,
      const std::uint32_t predicate_expression_id,
      const CanonicalRelationalExpressionRowBinding& predicate_row_binding,
      const exec::DescriptorBatch& input_batch,
      const CanonicalRelationalExpressionRuntimeServices& expression_services,
      const api::EngineCanonicalExpressionConsumer expression_consumer,
      const api::EnginePredicateConsumer predicate_consumer,
      const exec::TypedPhysicalNodeDag& physical_dag,
      const std::uint64_t selected_physical_node_id,
      const std::uint64_t scoped_root_physical_node_id,
      const std::size_t maximum_input_row_count,
      const exec::CanonicalExecutionMgaAuthority& mga_authority,
      const bool borrowed_execution_carriers,
      const bool borrow_mga_authority = false) {
    FilterPredicateReceiptIssueResult result;
    const auto refuse = [&](std::string detail) {
      result.diagnostic.ok = false;
      result.diagnostic.diagnostic_code =
          "QOW-DIAG-QRY-007-FILTER-PREDICATE-RECEIPT-REFUSAL-V1";
      result.diagnostic.detail = std::move(detail);
      result.receipt.reset();
      return result;
    };

    const bool filter_consumer =
        predicate_consumer == api::EnginePredicateConsumer::filter &&
        expression_consumer ==
            api::EngineCanonicalExpressionConsumer::filter;
    const bool having_consumer =
        predicate_consumer == api::EnginePredicateConsumer::having &&
        expression_consumer ==
            api::EngineCanonicalExpressionConsumer::aggregate;
    if ((!filter_consumer && !having_consumer) ||
        predicate_expression_id == 0 ||
        input_batch.rows.size() > maximum_input_row_count) {
      return refuse(
          "predicate identity, consumer pairing, or input row ceiling is not "
          "canonical");
    }

    const auto before = exec::RevalidateCanonicalExecutionMgaAuthority(
        mga_authority, physical_dag);
    if (!before.ok) {
      result.diagnostic = before;
      return result;
    }

    const auto selected_node = std::ranges::find_if(
        physical_dag.nodes, [&](const auto& node) {
          return node.physical_node_id == selected_physical_node_id;
        });
    if (selected_node == physical_dag.nodes.end() ||
        selected_physical_node_id == 0 ||
        selected_physical_node_id != scoped_root_physical_node_id ||
        selected_node->node_kind != exec::PhysicalNodeKind::kFilter ||
        selected_node->implementation_id != "filter.3vl.row.v1" ||
        selected_node->input_physical_node_ids.size() != 1 ||
        relational_dag.bound_sblr_tree_uuid !=
            physical_dag.bound_sblr_tree_uuid) {
      return refuse(
          "predicate receipt is not bound to its selected filter plan");
    }
    const auto input_node = std::ranges::find_if(
        physical_dag.nodes, [&](const auto& node) {
          return node.physical_node_id ==
                 selected_node->input_physical_node_ids.front();
        });
    if (input_node == physical_dag.nodes.end() ||
        selected_node->output_descriptor_ids !=
            input_node->output_descriptor_ids ||
        predicate_row_binding.row_descriptor_ids !=
            input_node->output_descriptor_ids) {
      return refuse("predicate receipt physical schema is not preserved");
    }
    auto validation = exec::ValidateCanonicalDescriptorBatch(
        input_batch, input_node->output_descriptor_ids);
    if (!validation.ok) {
      result.diagnostic = std::move(validation);
      return result;
    }

    const auto logical_node = std::ranges::find_if(
        relational_dag.nodes, [&](const auto& node) {
          return node.node_id == selected_node->relational_node_id;
        });
    const auto predicate = std::ranges::find_if(
        relational_dag.expressions, [&](const auto& expression) {
          return expression.expression_id == predicate_expression_id;
        });
    if (logical_node == relational_dag.nodes.end() ||
        logical_node->node_kind != api::RelationalDagNodeKind::kFilter ||
        logical_node->input_node_ids !=
            std::vector<std::uint32_t>{input_node->relational_node_id} ||
        logical_node->output_descriptor_ids !=
            selected_node->output_descriptor_ids ||
        logical_node->bound_expression_ids !=
            std::vector<std::uint32_t>{predicate_expression_id} ||
        predicate == relational_dag.expressions.end()) {
      return refuse(
          "predicate receipt lacks exact typed relational filter authority");
    }

    std::unordered_set<std::uint32_t> row_slot_expression_ids;
    for (const auto& slot : predicate_row_binding.slots) {
      const auto expression = std::ranges::find_if(
          relational_dag.expressions, [&](const auto& candidate) {
            return candidate.expression_id == slot.expression_id;
          });
      if (slot.expression_id == 0 ||
          slot.row_ordinal >=
              predicate_row_binding.row_descriptor_ids.size() ||
          slot.descriptor_id != predicate_row_binding
                                    .row_descriptor_ids[slot.row_ordinal] ||
          expression == relational_dag.expressions.end() ||
          expression->result_descriptor_id != slot.descriptor_id ||
          !row_slot_expression_ids.insert(slot.expression_id).second) {
        return refuse(
            "predicate row binding is not descriptor-exact and unique");
      }
    }

    std::uint64_t truth_memory_bytes = 0;
    std::uint64_t input_memory_bytes = 0;
    if (!CheckedMultiply(input_batch.rows.size(),
                         sizeof(api::EngineSqlTruthValue),
                         &truth_memory_bytes) ||
        !RuntimeMaterializedBatchMemoryBytes(input_batch,
                                             &input_memory_bytes) ||
        selected_node->memory_bytes_required == 0 ||
        selected_node->memory_bytes_required >
            physical_dag.memory_budget_bytes ||
        selected_node->memory_bytes_required >
            static_cast<std::uint64_t>(
                std::numeric_limits<std::size_t>::max()) ||
        input_memory_bytes > selected_node->memory_bytes_required ||
        truth_memory_bytes >
            selected_node->memory_bytes_required - input_memory_bytes) {
      return refuse(
          "predicate input and truth materialization exceed the selected "
          "filter-node memory grant");
    }

    CanonicalRelationalExpressionRuntime expression_runtime(
        relational_dag, expression_services);
    std::vector<api::EngineSqlTruthValue> row_truth_values;
    row_truth_values.reserve(input_batch.rows.size());
    for (std::size_t row = 0; row < input_batch.rows.size(); ++row) {
      api::EngineSqlTruthValue truth = api::EngineSqlTruthValue::unknown;
      std::string detail;
      if (!expression_runtime.EvaluatePredicateForConsumer(
              predicate_expression_id, predicate_row_binding,
              input_batch.rows[row].values, expression_consumer, &truth,
              &detail) ||
          (truth != api::EngineSqlTruthValue::true_value &&
           truth != api::EngineSqlTruthValue::false_value &&
           truth != api::EngineSqlTruthValue::unknown)) {
        return refuse("row=" + std::to_string(row) +
                      ": canonical predicate evaluation: " + detail);
      }
      row_truth_values.push_back(truth);
    }

    const auto after = exec::RevalidateCanonicalExecutionMgaAuthority(
        mga_authority, physical_dag);
    if (!after.ok) {
      result.diagnostic = after;
      return result;
    }

    try {
      auto receipt = std::shared_ptr<
          exec::CanonicalDescriptorFilterPredicateReceipt>(
          new exec::CanonicalDescriptorFilterPredicateReceipt());
      if (!borrowed_execution_carriers) {
        receipt->physical_dag_ = physical_dag;
        receipt->input_batch_ = input_batch;
      }
      receipt->selected_physical_node_id_ = selected_physical_node_id;
      receipt->row_truth_values_ = std::move(row_truth_values);
      receipt->consumer_ = predicate_consumer;
      receipt->expression_consumer_ = expression_consumer;
      receipt->predicate_expression_id_ = predicate_expression_id;
      receipt->row_descriptor_ids_ =
          predicate_row_binding.row_descriptor_ids;
      receipt->row_slot_expression_ids_.assign(
          row_slot_expression_ids.begin(), row_slot_expression_ids.end());
      std::ranges::sort(receipt->row_slot_expression_ids_);
      receipt->maximum_input_row_count_ = maximum_input_row_count;
      if (borrow_mga_authority) {
        receipt->borrowed_mga_authority_ = &mga_authority;
      } else {
        receipt->mga_authority_ = mga_authority;
      }
      receipt->exact_current_revalidated_before_issue_ = true;
      receipt->borrowed_execution_carriers_ =
          borrowed_execution_carriers;
      result.receipt = std::move(receipt);
    } catch (const std::bad_alloc&) {
      return refuse("predicate receipt allocation failed");
    }
    result.diagnostic = {};
    return result;
  }
};

exec::CanonicalDescriptorFilterResult
IssueAndExecuteCanonicalFilterPredicateBorrowed(
    const api::TypedRelationalDag& relational_dag,
    const std::uint32_t predicate_expression_id,
    const CanonicalRelationalExpressionRowBinding& predicate_row_binding,
    const exec::DescriptorBatch& input_batch,
    const CanonicalRelationalExpressionRuntimeServices& expression_services,
    const api::EngineCanonicalExpressionConsumer expression_consumer,
    const api::EnginePredicateConsumer predicate_consumer,
    const exec::TypedPhysicalNodeDag& physical_dag,
    const std::uint64_t selected_physical_node_id,
    const std::uint64_t scoped_root_physical_node_id,
    const std::size_t maximum_input_row_count,
    const exec::CanonicalExecutionMgaAuthority& mga_authority,
    const bool borrow_mga_authority) {
  return CanonicalDescriptorFilterPredicateReceiptIssuer::
      IssueAndExecuteBorrowed(
          relational_dag, predicate_expression_id, predicate_row_binding,
          input_batch, expression_services, expression_consumer,
          predicate_consumer, physical_dag, selected_physical_node_id,
          scoped_root_physical_node_id, maximum_input_row_count, mga_authority,
          borrow_mga_authority);
}

}  // namespace scratchbird::engine::sblr
