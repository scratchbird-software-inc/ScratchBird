// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "descriptor_value_runtime.hpp"

#include <utility>

namespace scratchbird::engine::executor {
namespace {

DescriptorRuntimeDiagnostic Refusal(std::string code,
                                    std::string detail = {},
                                    const std::size_t row = 0) {
  DescriptorRuntimeDiagnostic diagnostic;
  diagnostic.ok = false;
  diagnostic.diagnostic_code = std::move(code);
  diagnostic.detail = std::move(detail);
  diagnostic.row_index = row;
  return diagnostic;
}

}  // namespace

// QOW-SOURCE-QRY-007-FILTER-V1
// Canonical typed filter-node consumer. Predicate evaluation for WHERE or
// HAVING is supplied as the shared QRY-017 SQL truth state; this node admits
// only TRUE after the physical DAG, MGA context, and descriptor-preserving
// schema validate.
CanonicalDescriptorFilterResult ExecuteCanonicalDescriptorFilter(
    const CanonicalDescriptorFilterRequest& request) {
  using scratchbird::engine::internal_api::EnginePredicateConsumer;
  using scratchbird::engine::internal_api::QowPredicateConsumerPassesV1;

  CanonicalDescriptorFilterResult result;
  const auto refuse = [&](DescriptorRuntimeDiagnostic diagnostic) {
    result.diagnostic = std::move(diagnostic);
    result.output_batch = {};
    return result;
  };
  if (!request.predicate_receipt) {
    return refuse(Refusal(
        "QOW-DIAG-QRY-007-FILTER-PREDICATE-RECEIPT-REFUSAL-V1",
        "descriptor filter requires an engine-issued predicate receipt"));
  }
  if (!request.physical_dag.selected_plan_uuid.empty() ||
      request.physical_dag.root_physical_node_id != 0 ||
      !request.physical_dag.nodes.empty() ||
      !request.physical_dag.bound_sblr_tree_uuid.empty() ||
      request.selected_physical_node_id != 0 ||
      !request.input_batch.columns.empty() || !request.input_batch.rows.empty() ||
      !request.row_truth_values.empty() ||
      request.consumer != EnginePredicateConsumer::filter ||
      request.mga_authority.origin != CanonicalMgaAuthorityOrigin::kMissing ||
      static_cast<bool>(request.mga_authority.resolve_current)) {
    return refuse(Refusal(
        "QOW-DIAG-QRY-007-FILTER-PREDICATE-RECEIPT-REFUSAL-V1",
        "descriptor filter rejects mixed legacy and receipt authority"));
  }
  const auto& receipt = *request.predicate_receipt;
  if (!receipt.exact_current_revalidated_before_issue_ ||
      receipt.predicate_expression_id_ == 0 ||
      receipt.input_batch_.rows.size() > receipt.maximum_input_row_count_ ||
      receipt.row_truth_values_.size() != receipt.input_batch_.rows.size()) {
    return refuse(Refusal(
        "QOW-DIAG-QRY-007-FILTER-PREDICATE-RECEIPT-REFUSAL-V1",
        "descriptor filter predicate receipt is incomplete"));
  }
  const bool filter_consumer =
      receipt.consumer_ == EnginePredicateConsumer::filter &&
      receipt.expression_consumer_ == scratchbird::engine::internal_api::
                                          EngineCanonicalExpressionConsumer::
                                              filter;
  const bool having_consumer =
      receipt.consumer_ == EnginePredicateConsumer::having &&
      receipt.expression_consumer_ == scratchbird::engine::internal_api::
                                          EngineCanonicalExpressionConsumer::
                                              aggregate;
  if (!filter_consumer && !having_consumer) {
    return refuse(Refusal(
        "QOW-DIAG-QRY-017-3VL-REFUSAL-V1",
        "predicate receipt consumer pairing is not canonical"));
  }
  const auto authority_validation = RevalidateCanonicalExecutionMgaAuthority(
      receipt.mga_authority_, receipt.physical_dag_);
  if (!authority_validation.ok) {
    return refuse(authority_validation);
  }
  const PhysicalNodeRecord* selected_node = nullptr;
  const PhysicalNodeRecord* input_node = nullptr;
  for (const auto& node : receipt.physical_dag_.nodes) {
    if (node.physical_node_id == receipt.selected_physical_node_id_) {
      selected_node = &node;
    }
  }
  if (receipt.selected_physical_node_id_ == 0 ||
      receipt.selected_physical_node_id_ !=
          receipt.physical_dag_.root_physical_node_id ||
      selected_node == nullptr ||
      selected_node->node_kind != PhysicalNodeKind::kFilter ||
      selected_node->implementation_id != "filter.3vl.row.v1" ||
      selected_node->input_physical_node_ids.size() != 1) {
    return refuse(Refusal(
        "QOW-DIAG-QRY-007-FILTER-PHYSICAL-ROUTE-V1",
        "descriptor filter requires one selected root filter node"));
  }
  for (const auto& node : receipt.physical_dag_.nodes) {
    if (node.physical_node_id ==
        selected_node->input_physical_node_ids.front()) {
      input_node = &node;
      break;
    }
  }
  if (input_node == nullptr ||
      selected_node->output_descriptor_ids !=
          input_node->output_descriptor_ids ||
      receipt.row_descriptor_ids_ != input_node->output_descriptor_ids) {
    return refuse(Refusal("SBLR.PLAN_TREE.INVALID_HANDLE",
                          "filter schema does not preserve input handles"));
  }
  auto input_validation = ValidateCanonicalDescriptorBatch(
      receipt.input_batch_, input_node->output_descriptor_ids);
  if (!input_validation.ok) return refuse(std::move(input_validation));

  result.output_batch.columns = receipt.input_batch_.columns;
  result.output_batch.rows.reserve(receipt.input_batch_.rows.size());
  for (std::size_t row = 0; row < receipt.input_batch_.rows.size(); ++row) {
    bool passes = false;
    std::string refusal_detail;
    if (!QowPredicateConsumerPassesV1(receipt.row_truth_values_[row],
                                      receipt.consumer_, &passes,
                                      &refusal_detail)) {
      return refuse(Refusal("QOW-DIAG-QRY-017-3VL-REFUSAL-V1",
                            std::move(refusal_detail), row));
    }
    if (passes) {
      result.output_batch.rows.push_back(receipt.input_batch_.rows[row]);
    }
  }
  auto output_validation = ValidateCanonicalDescriptorBatch(
      result.output_batch, selected_node->output_descriptor_ids);
  if (!output_validation.ok) return refuse(std::move(output_validation));
  const auto result_authority = RevalidateCanonicalExecutionMgaAuthority(
      receipt.mga_authority_, receipt.physical_dag_);
  if (!result_authority.ok) return refuse(result_authority);

  result.diagnostic = {};
  result.selected_plan_uuid = receipt.physical_dag_.selected_plan_uuid;
  result.executed_physical_node_id = selected_node->physical_node_id;
  result.causal_counter_id = selected_node->causal_counter_id;
  result.mga_statement_context = receipt.mga_authority_.statement_context;
  return result;
}

}  // namespace scratchbird::engine::executor
