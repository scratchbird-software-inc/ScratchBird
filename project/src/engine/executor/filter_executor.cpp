// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "descriptor_value_runtime.hpp"

#include <limits>
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

bool DescriptorBatchCarrierIsExactDefault(const DescriptorBatch& batch) {
  const DescriptorBatch empty;
  return batch.columns.empty() &&
         batch.columns.capacity() == empty.columns.capacity() &&
         batch.rows.empty() && batch.rows.capacity() == empty.rows.capacity();
}

bool CanonicalExecutionMgaAuthorityCarrierIsExactDefault(
    const CanonicalExecutionMgaAuthority& authority) {
  const CanonicalExecutionMgaAuthority empty;
  const auto exact_empty_string = [](const std::string& value,
                                     const std::string& baseline) {
    return value.empty() && value.capacity() == baseline.capacity();
  };
  const auto& context = authority.statement_context;
  const auto& empty_context = empty.statement_context;
  return authority.origin == empty.origin && !authority.resolve_current &&
         PhysicalMgaStatementContextEqual(context, empty_context) &&
         exact_empty_string(context.statement_uuid,
                            empty_context.statement_uuid) &&
         exact_empty_string(context.owning_transaction_uuid,
                            empty_context.owning_transaction_uuid) &&
         exact_empty_string(context.statement_snapshot_uuid,
                            empty_context.statement_snapshot_uuid) &&
         exact_empty_string(context.statement_metadata_snapshot_uuid,
                            empty_context.statement_metadata_snapshot_uuid) &&
         exact_empty_string(context.snapshot_kind,
                            empty_context.snapshot_kind) &&
         exact_empty_string(context.statement_timestamp,
                            empty_context.statement_timestamp) &&
         context.active_excluded_local_transaction_ids.empty() &&
         context.active_excluded_local_transaction_ids.capacity() ==
             empty_context.active_excluded_local_transaction_ids.capacity() &&
         context.in_doubt_excluded_local_transaction_ids.empty() &&
         context.in_doubt_excluded_local_transaction_ids.capacity() ==
             empty_context.in_doubt_excluded_local_transaction_ids.capacity();
}

bool FilterBatchPayloadBytes(const DescriptorBatch& batch,
                             std::size_t* bytes) {
  if (bytes == nullptr) return false;
  *bytes = 1;
  for (const auto& row : batch.rows) {
    for (const auto& value : row.values) {
      if (value.encoded_value.size() >
          std::numeric_limits<std::size_t>::max() - *bytes) {
        return false;
      }
      *bytes += value.encoded_value.size();
      if (value.binary_value.size() >
          std::numeric_limits<std::size_t>::max() - *bytes) {
        return false;
      }
      *bytes += value.binary_value.size();
    }
  }
  return true;
}

bool FilterTruthPayloadBytes(const std::size_t row_count,
                             std::size_t* bytes) {
  if (bytes == nullptr ||
      row_count > std::numeric_limits<std::size_t>::max() /
                      sizeof(scratchbird::engine::internal_api::
                                 EngineSqlTruthValue)) {
    return false;
  }
  *bytes = row_count * sizeof(
      scratchbird::engine::internal_api::EngineSqlTruthValue);
  return true;
}

}  // namespace

// QOW-SOURCE-QRY-007-FILTER-V1
// Canonical typed filter-node consumer. Predicate evaluation for WHERE or
// HAVING is supplied as the shared QRY-017 SQL truth state; this node admits
// only TRUE after the physical DAG, MGA context, and descriptor-preserving
// schema validate.
CanonicalDescriptorFilterResult ExecuteCanonicalDescriptorFilterBound(
    const CanonicalDescriptorFilterRequest& request,
    const TypedPhysicalNodeDag& borrowed_execution_dag,
    const std::uint64_t scoped_root_physical_node_id,
    const DescriptorBatch& borrowed_input_batch,
    const bool borrowed_execution_carriers) {
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
  const std::vector<scratchbird::engine::internal_api::EngineSqlTruthValue>
      empty_truth_values;
  if (!TypedPhysicalNodeDagCarrierIsExactDefault(request.physical_dag) ||
      request.selected_physical_node_id != 0 ||
      !DescriptorBatchCarrierIsExactDefault(request.input_batch) ||
      !request.row_truth_values.empty() ||
      request.row_truth_values.capacity() != empty_truth_values.capacity() ||
      request.consumer != EnginePredicateConsumer::filter ||
      !CanonicalExecutionMgaAuthorityCarrierIsExactDefault(
          request.mga_authority)) {
    return refuse(Refusal(
        "QOW-DIAG-QRY-007-FILTER-PREDICATE-RECEIPT-REFUSAL-V1",
        "descriptor filter rejects mixed legacy and receipt authority"));
  }
  const auto& receipt = *request.predicate_receipt;
  if (receipt.borrowed_execution_carriers_ !=
      borrowed_execution_carriers) {
    return refuse(Refusal(
        "QOW-DIAG-QRY-007-FILTER-PREDICATE-RECEIPT-REFUSAL-V1",
        "descriptor filter receipt execution-carrier mode is mismatched"));
  }
  if (borrowed_execution_carriers &&
      (!TypedPhysicalNodeDagCarrierIsExactDefault(receipt.physical_dag_) ||
       !DescriptorBatchCarrierIsExactDefault(receipt.input_batch_))) {
    return refuse(Refusal(
        "QOW-DIAG-QRY-007-FILTER-PREDICATE-RECEIPT-REFUSAL-V1",
        "borrowed descriptor filter receipt carries conflicting owned authority"));
  }
  const auto& execution_dag = borrowed_execution_carriers
                                  ? borrowed_execution_dag
                                  : receipt.physical_dag_;
  const auto& execution_input_batch = borrowed_execution_carriers
                                          ? borrowed_input_batch
                                          : receipt.input_batch_;
  const auto execution_root_physical_node_id =
      borrowed_execution_carriers
          ? scoped_root_physical_node_id
          : receipt.physical_dag_.root_physical_node_id;
  if (!receipt.exact_current_revalidated_before_issue_ ||
      receipt.predicate_expression_id_ == 0 ||
      execution_input_batch.rows.size() > receipt.maximum_input_row_count_ ||
      receipt.row_truth_values_.size() != execution_input_batch.rows.size()) {
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
      receipt.mga_authority_, execution_dag);
  if (!authority_validation.ok) {
    return refuse(authority_validation);
  }
  const PhysicalNodeRecord* selected_node = nullptr;
  const PhysicalNodeRecord* input_node = nullptr;
  for (const auto& node : execution_dag.nodes) {
    if (node.physical_node_id == receipt.selected_physical_node_id_) {
      selected_node = &node;
    }
  }
  if (receipt.selected_physical_node_id_ == 0 ||
      receipt.selected_physical_node_id_ !=
          execution_root_physical_node_id ||
      selected_node == nullptr ||
      selected_node->node_kind != PhysicalNodeKind::kFilter ||
      selected_node->implementation_id != "filter.3vl.row.v1" ||
      selected_node->input_physical_node_ids.size() != 1) {
    return refuse(Refusal(
        "QOW-DIAG-QRY-007-FILTER-PHYSICAL-ROUTE-V1",
        "descriptor filter requires one selected root filter node"));
  }
  for (const auto& node : execution_dag.nodes) {
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
      execution_input_batch, input_node->output_descriptor_ids);
  if (!input_validation.ok) return refuse(std::move(input_validation));

  if (execution_dag.memory_budget_bytes == 0 ||
      selected_node->memory_bytes_required == 0 ||
      selected_node->memory_bytes_required >
          execution_dag.memory_budget_bytes ||
      selected_node->memory_bytes_required >
          static_cast<std::uint64_t>(
              std::numeric_limits<std::size_t>::max())) {
    return refuse(Refusal(
        "SBLR.PLAN_TREE.RESOURCE_LIMIT",
        "filter selected-node memory grant is invalid"));
  }
  const auto node_memory_grant =
      static_cast<std::size_t>(selected_node->memory_bytes_required);
  std::size_t input_payload_bytes = 0;
  std::size_t truth_payload_bytes = 0;
  if (!FilterBatchPayloadBytes(execution_input_batch,
                               &input_payload_bytes) ||
      !FilterTruthPayloadBytes(receipt.row_truth_values_.size(),
                               &truth_payload_bytes) ||
      input_payload_bytes > node_memory_grant ||
      truth_payload_bytes > node_memory_grant - input_payload_bytes) {
    return refuse(Refusal(
        "SBLR.PLAN_TREE.RESOURCE_LIMIT",
        "filter retained input and truth state exceed the selected-node grant"));
  }
  const auto retained_payload_bytes =
      input_payload_bytes + truth_payload_bytes;
  std::size_t output_payload_bytes = 1;
  if (output_payload_bytes >
      node_memory_grant - retained_payload_bytes) {
    return refuse(Refusal(
        "SBLR.PLAN_TREE.RESOURCE_LIMIT",
        "filter output payload exceeds the selected-node grant"));
  }

  result.output_batch.columns = execution_input_batch.columns;
  result.output_batch.rows.reserve(execution_input_batch.rows.size());
  for (std::size_t row = 0; row < execution_input_batch.rows.size(); ++row) {
    bool passes = false;
    std::string refusal_detail;
    if (!QowPredicateConsumerPassesV1(receipt.row_truth_values_[row],
                                      receipt.consumer_, &passes,
                                      &refusal_detail)) {
      return refuse(Refusal("QOW-DIAG-QRY-017-3VL-REFUSAL-V1",
                            std::move(refusal_detail), row));
    }
    if (passes) {
      std::size_t row_payload_bytes = 0;
      for (const auto& value : execution_input_batch.rows[row].values) {
        if (value.encoded_value.size() >
                std::numeric_limits<std::size_t>::max() -
                    row_payload_bytes ||
            value.binary_value.size() >
                std::numeric_limits<std::size_t>::max() -
                    row_payload_bytes - value.encoded_value.size()) {
          return refuse(Refusal(
              "SBLR.PLAN_TREE.RESOURCE_LIMIT",
              "filter output payload accounting overflowed", row));
        }
        row_payload_bytes += value.encoded_value.size();
        row_payload_bytes += value.binary_value.size();
      }
      if (row_payload_bytes >
          node_memory_grant - retained_payload_bytes -
              output_payload_bytes) {
        return refuse(Refusal(
            "SBLR.PLAN_TREE.RESOURCE_LIMIT",
            "filter output payload exceeds the selected-node grant", row));
      }
      output_payload_bytes += row_payload_bytes;
      result.output_batch.rows.push_back(execution_input_batch.rows[row]);
    }
  }
  auto output_validation = ValidateCanonicalDescriptorBatch(
      result.output_batch, selected_node->output_descriptor_ids);
  if (!output_validation.ok) return refuse(std::move(output_validation));
  const auto result_authority = RevalidateCanonicalExecutionMgaAuthority(
      receipt.mga_authority_, execution_dag);
  if (!result_authority.ok) return refuse(result_authority);

  result.diagnostic = {};
  result.selected_plan_uuid = execution_dag.selected_plan_uuid;
  result.executed_physical_node_id = selected_node->physical_node_id;
  result.causal_counter_id = selected_node->causal_counter_id;
  result.mga_statement_context = receipt.mga_authority_.statement_context;
  return result;
}

CanonicalDescriptorFilterResult ExecuteCanonicalDescriptorFilter(
    const CanonicalDescriptorFilterRequest& request) {
  return ExecuteCanonicalDescriptorFilterBound(
      request, request.physical_dag, 0, request.input_batch, false);
}

CanonicalDescriptorFilterResult ExecuteCanonicalDescriptorFilter(
    const CanonicalDescriptorFilterRequest& request,
    const TypedPhysicalNodeDag& borrowed_execution_dag,
    const std::uint64_t scoped_root_physical_node_id,
    const DescriptorBatch& borrowed_input_batch) {
  return ExecuteCanonicalDescriptorFilterBound(
      request, borrowed_execution_dag, scoped_root_physical_node_id,
      borrowed_input_batch, true);
}

}  // namespace scratchbird::engine::executor
