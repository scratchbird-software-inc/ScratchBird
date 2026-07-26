// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "descriptor_value_runtime.hpp"

#include <algorithm>
#include <cctype>
#include <limits>
#include <string_view>
#include <utility>

namespace scratchbird::engine::executor {
namespace {

DescriptorRuntimeDiagnostic Refusal(std::string code,
                                    std::string detail = {}) {
  DescriptorRuntimeDiagnostic diagnostic;
  diagnostic.ok = false;
  diagnostic.diagnostic_code = std::move(code);
  diagnostic.detail = std::move(detail);
  return diagnostic;
}

bool IsCanonicalUuid(const std::string_view value) {
  if (value.size() != 36 || value[8] != '-' || value[13] != '-' ||
      value[18] != '-' || value[23] != '-') {
    return false;
  }
  for (std::size_t index = 0; index < value.size(); ++index) {
    if (index == 8 || index == 13 || index == 18 || index == 23) continue;
    const auto ch = static_cast<unsigned char>(value[index]);
    if (!std::isxdigit(ch) || std::isupper(ch)) return false;
  }
  return true;
}

bool HasProperty(const std::vector<std::string>& properties,
                 const std::string_view property_uuid) {
  return std::ranges::find(properties, property_uuid) != properties.end();
}

CanonicalDescriptorOrderTerm PartitionOrderTerm(
    const CanonicalWindowPartitionTerm& partition_term) {
  CanonicalDescriptorOrderTerm order_term;
  order_term.column = partition_term.column;
  order_term.expression_descriptor_id =
      partition_term.expression_descriptor_id;
  order_term.direction = CanonicalDescriptorOrderDirection::ascending;
  order_term.null_placement = CanonicalDescriptorNullPlacement::first;
  order_term.collation_uuid = partition_term.collation_uuid;
  order_term.resource_epoch = partition_term.resource_epoch;
  order_term.collation_epoch = partition_term.collation_epoch;
  order_term.text_seed = partition_term.text_seed;
  return order_term;
}

}  // namespace

// QOW-SOURCE-WIN-004-V1
// QOW-SOURCE-WIN-005-V1
// QOW-SOURCE-WIN-014-V1
// Build one typed partition/order/peer stage from the exact property UUIDs
// carried by the optimizer-published physical window node. Partition and peer
// equality use the same descriptor-aware comparator as canonical ORDER BY;
// downstream window functions consume the explicit ranges and must not
// reconstruct peer identity under a weaker rule.
CanonicalWindowPartitionOrderResult ExecuteCanonicalWindowPartitionOrder(
    const CanonicalWindowPartitionOrderRequest& request) {
  CanonicalWindowPartitionOrderResult result;
  const auto refuse = [&](std::string code, std::string detail) {
    result = {};
    result.diagnostic = Refusal(std::move(code), std::move(detail));
    return result;
  };
  const auto dag_validation = ValidateTypedPhysicalNodeDag(request.physical_dag);
  if (!dag_validation.accepted) {
    const auto& issue = dag_validation.issues.front();
    return refuse(issue.diagnostic_id, issue.field_id);
  }
  if (request.physical_dag.abi_version != 2 ||
      !request.physical_dag.optimizer_published ||
      !request.physical_dag.immutable_node_identity_validated ||
      !request.physical_dag.capability_validated_before_access ||
      request.physical_dag.data_access_observed ||
      request.selected_physical_node_id == 0 ||
      request.selected_physical_node_id !=
          request.physical_dag.root_physical_node_id) {
    return refuse("QOW-DIAG-WINDOW-PEER",
                  "window stage lacks immutable optimizer publication");
  }
  if (request.inventory_local_transaction_id == 0 ||
      request.inventory_local_transaction_id !=
          request.physical_dag.local_transaction_id ||
      request.inventory_statement_snapshot_id == 0 ||
      request.inventory_statement_snapshot_id !=
          request.physical_dag.statement_snapshot_id ||
      request.parser_execution_authority_claimed ||
      request.transaction_finality_claimed ||
      request.recovery_authority_claimed) {
    return refuse("QOW-DIAG-WINDOW-PEER",
                  "window stage is outside engine MGA authority");
  }

  const PhysicalNodeRecord* selected_node = nullptr;
  const PhysicalNodeRecord* input_node = nullptr;
  for (const auto& node : request.physical_dag.nodes) {
    if (node.physical_node_id == request.selected_physical_node_id) {
      selected_node = &node;
      break;
    }
  }
  if (selected_node == nullptr ||
      selected_node->node_kind != PhysicalNodeKind::kWindow ||
      selected_node->input_physical_node_ids.size() != 1) {
    return refuse("QOW-DIAG-WINDOW-PEER",
                  "selected physical node is not a unary window stage");
  }
  for (const auto& node : request.physical_dag.nodes) {
    if (node.physical_node_id ==
        selected_node->input_physical_node_ids.front()) {
      input_node = &node;
      break;
    }
  }
  if (input_node == nullptr ||
      selected_node->output_descriptor_ids !=
          input_node->output_descriptor_ids) {
    return refuse("QOW-DIAG-WINDOW-PEER",
                  "window partition stage does not preserve descriptors");
  }
  const auto input_validation = ValidateCanonicalDescriptorBatch(
      request.input_batch, input_node->output_descriptor_ids);
  if (!input_validation.ok) {
    return refuse(input_validation.diagnostic_code, input_validation.detail);
  }

  if (!IsCanonicalUuid(request.window_property_uuid) ||
      !HasProperty(selected_node->delivered_property_uuids,
                   request.window_property_uuid)) {
    return refuse("QOW-DIAG-WINDOW-PEER",
                  "window peer property identity was not preserved");
  }
  if ((request.partition_terms.empty() !=
       request.partition_property_uuid.empty()) ||
      (!request.partition_terms.empty() &&
       (!IsCanonicalUuid(request.partition_property_uuid) ||
        !HasProperty(selected_node->required_property_uuids,
                     request.partition_property_uuid)))) {
    return refuse("QOW-DIAG-WINDOW-PARTITION",
                  "window partition property identity was not preserved");
  }
  if ((request.order_terms.empty() != request.ordering_property_uuid.empty()) ||
      (!request.order_terms.empty() &&
       (!IsCanonicalUuid(request.ordering_property_uuid) ||
        !HasProperty(selected_node->required_property_uuids,
                     request.ordering_property_uuid)))) {
    return refuse("QOW-DIAG-WINDOW-ORDER",
                  "window ordering property identity was not preserved");
  }
  if (request.maximum_term_count == 0 ||
      request.partition_terms.size() > request.maximum_term_count ||
      request.order_terms.size() > request.maximum_term_count ||
      request.partition_terms.size() >
          request.maximum_term_count - request.order_terms.size()) {
    return refuse("QOW-DIAG-WINDOW-PEER",
                  "window term resource bound was exceeded");
  }

  std::vector<CanonicalDescriptorOrderTerm> partition_terms;
  partition_terms.reserve(request.partition_terms.size());
  for (const auto& term : request.partition_terms) {
    if (term.column >= request.input_batch.columns.size()) {
      return refuse("QOW-DIAG-WINDOW-PARTITION",
                    "partition term is outside the input schema");
    }
    auto comparable = PartitionOrderTerm(term);
    const auto validation = ValidateCanonicalDescriptorOrderTerm(
        comparable, request.input_batch.columns[term.column]);
    if (!validation.ok) {
      return refuse("QOW-DIAG-WINDOW-PARTITION", validation.detail);
    }
    partition_terms.push_back(std::move(comparable));
  }
  for (const auto& term : request.order_terms) {
    if (term.column >= request.input_batch.columns.size()) {
      return refuse("QOW-DIAG-WINDOW-ORDER",
                    "order term is outside the input schema");
    }
    const auto validation = ValidateCanonicalDescriptorOrderTerm(
        term, request.input_batch.columns[term.column]);
    if (!validation.ok) {
      return refuse("QOW-DIAG-WINDOW-ORDER", validation.detail);
    }
  }

  const auto row_count = request.input_batch.rows.size();
  const auto term_count = partition_terms.size() + request.order_terms.size();
  if (request.maximum_pair_comparisons == 0 ||
      (row_count != 0 &&
       row_count > std::numeric_limits<std::size_t>::max() / row_count)) {
    return refuse("QOW-DIAG-WINDOW-PEER",
                  "window comparison resource bound overflowed");
  }
  const auto pair_count = row_count * row_count;
  if (term_count != 0 &&
      (pair_count > std::numeric_limits<std::size_t>::max() / term_count ||
       pair_count * term_count > request.maximum_pair_comparisons)) {
    return refuse("QOW-DIAG-WINDOW-PEER",
                  "window comparison resource bound was exceeded");
  }

  std::vector<std::uint8_t> same_partition(pair_count, 1);
  std::vector<std::int8_t> order_comparison(pair_count, 0);
  for (std::size_t left = 0; left < row_count; ++left) {
    for (std::size_t right = 0; right < row_count; ++right) {
      bool equal = true;
      for (const auto& term : partition_terms) {
        const auto compared = CompareCanonicalDescriptorOrderValues(
            request.input_batch.rows[left].values[term.column],
            request.input_batch.rows[right].values[term.column], term);
        if (!compared.diagnostic.ok) {
          return refuse("QOW-DIAG-WINDOW-PARTITION",
                        compared.diagnostic.detail);
        }
        if (compared.comparison != 0) {
          equal = false;
          break;
        }
      }
      same_partition[left * row_count + right] = equal ? 1 : 0;

      int comparison = 0;
      for (const auto& term : request.order_terms) {
        const auto compared = CompareCanonicalDescriptorOrderValues(
            request.input_batch.rows[left].values[term.column],
            request.input_batch.rows[right].values[term.column], term);
        if (!compared.diagnostic.ok) {
          return refuse("QOW-DIAG-WINDOW-ORDER", compared.diagnostic.detail);
        }
        comparison = compared.comparison;
        if (comparison != 0) break;
      }
      order_comparison[left * row_count + right] =
          static_cast<std::int8_t>(comparison);
    }
  }

  std::vector<std::vector<std::size_t>> partitions;
  for (std::size_t row = 0; row < row_count; ++row) {
    auto partition = std::ranges::find_if(
        partitions, [&](const auto& candidate) {
          return same_partition[row * row_count + candidate.front()] != 0;
        });
    if (partition == partitions.end()) {
      partitions.push_back({row});
    } else {
      partition->push_back(row);
    }
  }

  result.ordered_batch.columns = request.input_batch.columns;
  result.ordered_batch.rows.reserve(row_count);
  result.row_metadata.reserve(row_count);
  std::size_t ordered_index = 0;
  for (std::size_t partition_id = 0; partition_id < partitions.size();
       ++partition_id) {
    auto& partition = partitions[partition_id];
    std::stable_sort(partition.begin(), partition.end(),
                     [&](const auto left, const auto right) {
                       return order_comparison[left * row_count + right] < 0;
                     });
    const auto partition_begin = ordered_index;
    const auto partition_end = partition_begin + partition.size();
    std::size_t partition_offset = 0;
    std::size_t peer_group_id = 0;
    while (partition_offset < partition.size()) {
      std::size_t peer_end_offset = partition_offset + 1;
      while (peer_end_offset < partition.size() &&
             order_comparison[partition[partition_offset] * row_count +
                              partition[peer_end_offset]] == 0) {
        ++peer_end_offset;
      }
      const auto peer_begin = partition_begin + partition_offset;
      const auto peer_end = partition_begin + peer_end_offset;
      for (std::size_t offset = partition_offset; offset < peer_end_offset;
           ++offset) {
        const auto source_row = partition[offset];
        result.ordered_batch.rows.push_back(request.input_batch.rows[source_row]);
        CanonicalWindowRowPeerMetadata metadata;
        metadata.source_row_index = source_row;
        metadata.ordered_row_index = ordered_index++;
        metadata.partition_id = partition_id;
        metadata.peer_group_id = peer_group_id;
        metadata.partition_begin = partition_begin;
        metadata.partition_end_exclusive = partition_end;
        metadata.peer_begin = peer_begin;
        metadata.peer_end_exclusive = peer_end;
        result.row_metadata.push_back(std::move(metadata));
      }
      ++result.peer_group_count;
      ++peer_group_id;
      partition_offset = peer_end_offset;
    }
  }

  const auto output_validation = ValidateCanonicalDescriptorBatch(
      result.ordered_batch, selected_node->output_descriptor_ids);
  if (!output_validation.ok) {
    return refuse(output_validation.diagnostic_code,
                  output_validation.detail);
  }
  result.diagnostic = {};
  result.partition_count = partitions.size();
  result.order_terms = request.order_terms;
  result.window_property_uuid = request.window_property_uuid;
  result.partition_property_uuid = request.partition_property_uuid;
  result.ordering_property_uuid = request.ordering_property_uuid;
  result.explicit_peer_metadata = true;
  result.weaker_peer_recomputation_forbidden = true;
  result.final_query_order_guaranteed = false;
  result.authority.engine_mga_snapshot_bound = true;
  result.inventory_local_transaction_id =
      request.inventory_local_transaction_id;
  result.inventory_statement_snapshot_id =
      request.inventory_statement_snapshot_id;
  result.selected_plan_uuid = request.physical_dag.selected_plan_uuid;
  result.executed_physical_node_id = selected_node->physical_node_id;
  result.causal_counter_id = selected_node->causal_counter_id;
  return result;
}

// QOW-SOURCE-QRY-007-WINDOW-V1
// First canonical implementation in this module: global ROW_NUMBER over a
// typed batch whose deterministic order has already been established by its
// physical sort input and retained as explicit evidence.
CanonicalDescriptorRowNumberResult ExecuteCanonicalDescriptorRowNumber(
    const CanonicalDescriptorRowNumberRequest& request) {
  using scratchbird::engine::internal_api::EngineTypedValue;
  using scratchbird::engine::internal_api::EngineValueState;

  CanonicalDescriptorRowNumberResult result;
  const auto refuse = [&](DescriptorRuntimeDiagnostic diagnostic) {
    result.diagnostic = std::move(diagnostic);
    result.output_batch = {};
    return result;
  };
  const auto dag_validation =
      ValidateTypedPhysicalNodeDag(request.physical_dag);
  if (!dag_validation.accepted) {
    const auto& issue = dag_validation.issues.front();
    return refuse(Refusal(issue.diagnostic_id, issue.field_id));
  }
  if (request.selected_physical_node_id == 0 ||
      request.selected_physical_node_id !=
          request.physical_dag.root_physical_node_id) {
    return refuse(Refusal("SBLR.PLAN_TREE.INVALID_HANDLE",
                          "selected window node is not the root"));
  }

  const PhysicalNodeRecord* selected_node = nullptr;
  const PhysicalNodeRecord* input_node = nullptr;
  for (const auto& node : request.physical_dag.nodes) {
    if (node.physical_node_id == request.selected_physical_node_id) {
      selected_node = &node;
    }
  }
  if (selected_node == nullptr ||
      selected_node->node_kind != PhysicalNodeKind::kWindow ||
      selected_node->input_physical_node_ids.size() != 1) {
    return refuse(Refusal("QOW-DIAG-QRY-007-WINDOW-PHYSICAL-ROUTE-V1",
                          "ROW_NUMBER requires one selected window node"));
  }
  for (const auto& node : request.physical_dag.nodes) {
    if (node.physical_node_id ==
        selected_node->input_physical_node_ids.front()) {
      input_node = &node;
      break;
    }
  }
  if (input_node == nullptr || input_node->node_kind != PhysicalNodeKind::kSort ||
      !IsCanonicalUuid(request.deterministic_order_evidence_uuid)) {
    return refuse(Refusal(
        "QOW-DIAG-QRY-007-WINDOW-ORDER-REQUIRED-V1",
        "ROW_NUMBER input lacks deterministic physical order evidence"));
  }
  std::vector<std::uint32_t> output_descriptor_ids =
      input_node->output_descriptor_ids;
  output_descriptor_ids.push_back(request.row_number_column.descriptor_id);
  if (selected_node->output_descriptor_ids != output_descriptor_ids ||
      request.row_number_column.descriptor_id == 0 ||
      request.row_number_column.nullable ||
      request.row_number_column.descriptor.canonical_type_name != "int64") {
    return refuse(Refusal("SBLR.PLAN_TREE.INVALID_HANDLE",
                          "ROW_NUMBER output descriptor is not bound int64"));
  }
  auto input_validation = ValidateCanonicalDescriptorBatch(
      request.ordered_input_batch, input_node->output_descriptor_ids);
  if (!input_validation.ok) return refuse(std::move(input_validation));
  if (request.ordered_input_batch.rows.size() >
      static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max())) {
    return refuse(Refusal("QOW-DIAG-QRY-007-WINDOW-OVERFLOW-V1",
                          "ROW_NUMBER exceeds int64 result width"));
  }

  result.output_batch.columns = request.ordered_input_batch.columns;
  result.output_batch.columns.push_back(request.row_number_column);
  result.output_batch.rows = request.ordered_input_batch.rows;
  for (std::size_t row = 0; row < result.output_batch.rows.size(); ++row) {
    EngineTypedValue row_number;
    row_number.descriptor = request.row_number_column.descriptor;
    row_number.encoded_value = std::to_string(row + 1);
    row_number.state = EngineValueState::value;
    result.output_batch.rows[row].values.push_back(std::move(row_number));
  }
  auto output_validation = ValidateCanonicalDescriptorBatch(
      result.output_batch, selected_node->output_descriptor_ids);
  if (!output_validation.ok) return refuse(std::move(output_validation));

  result.diagnostic = {};
  result.selected_plan_uuid = request.physical_dag.selected_plan_uuid;
  result.executed_physical_node_id = selected_node->physical_node_id;
  result.causal_counter_id = selected_node->causal_counter_id;
  return result;
}

}  // namespace scratchbird::engine::executor
