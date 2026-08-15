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

std::size_t PropertyCount(const std::vector<std::string>& properties,
                          const std::string_view property_uuid) {
  return static_cast<std::size_t>(std::ranges::count(properties, property_uuid));
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
  order_term.timezone_epoch = partition_term.timezone_epoch;
  order_term.timezone_seed = partition_term.timezone_seed;
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
// reconstruct peer identity under a weaker rule. Engine-materialized hidden
// expression keys remain separate from the payload, and stable input order is
// used only as a deterministic final tie break without changing peer equality.
CanonicalWindowPartitionOrderResult ExecuteCanonicalWindowPartitionOrder(
    const CanonicalWindowPartitionOrderRequest& request) {
  CanonicalWindowPartitionOrderResult result;
  const auto refuse = [&](std::string code, std::string detail) {
    result = {};
    result.diagnostic = Refusal(std::move(code), std::move(detail));
    return result;
  };
  const auto authority_validation = RevalidateCanonicalExecutionMgaAuthority(
      request.mga_authority, request.physical_dag);
  if (!authority_validation.ok) {
    return refuse(authority_validation.diagnostic_code,
                  authority_validation.detail);
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
  if (request.parser_execution_authority_claimed ||
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
  const DescriptorBatch* key_batch = &request.input_batch;
  if (request.key_batch.has_value()) {
    if (request.key_batch->rows.size() != request.input_batch.rows.size()) {
      return refuse("QOW-DIAG-WINDOW-PROPERTY-BINDING",
                    "materialized window-key cardinality differs from the input");
    }
    std::vector<std::uint32_t> key_descriptor_ids;
    key_descriptor_ids.reserve(request.key_batch->columns.size());
    for (const auto& column : request.key_batch->columns) {
      key_descriptor_ids.push_back(column.descriptor_id);
    }
    const auto key_validation = ValidateCanonicalDescriptorBatch(
        *request.key_batch, key_descriptor_ids);
    if (!key_validation.ok) {
      return refuse("QOW-DIAG-WINDOW-PROPERTY-BINDING",
                    key_validation.detail);
    }
    key_batch = &*request.key_batch;
  }

  if (!IsCanonicalUuid(request.window_property_uuid) ||
      PropertyCount(selected_node->delivered_property_uuids,
                    request.window_property_uuid) != 1) {
    return refuse("QOW-DIAG-WINDOW-PEER",
                  "window peer property identity was not preserved");
  }
  if ((request.partition_terms.empty() !=
       request.partition_property_uuid.empty()) ||
      (!request.partition_terms.empty() &&
       (!IsCanonicalUuid(request.partition_property_uuid) ||
        PropertyCount(selected_node->required_property_uuids,
                      request.partition_property_uuid) != 1))) {
    return refuse("QOW-DIAG-WINDOW-PARTITION",
                  "window partition property identity was not preserved");
  }
  if ((request.order_terms.empty() != request.ordering_property_uuid.empty()) ||
      (!request.order_terms.empty() &&
       (!IsCanonicalUuid(request.ordering_property_uuid) ||
        PropertyCount(selected_node->required_property_uuids,
                      request.ordering_property_uuid) != 1))) {
    return refuse("QOW-DIAG-WINDOW-ORDER",
                  "window ordering property identity was not preserved");
  }
  const auto expected_required_property_count =
      static_cast<std::size_t>(!request.partition_terms.empty()) +
      static_cast<std::size_t>(!request.order_terms.empty());
  if (selected_node->required_property_uuids.size() !=
          expected_required_property_count ||
      request.window_property_uuid == request.partition_property_uuid ||
      request.window_property_uuid == request.ordering_property_uuid ||
      (!request.partition_property_uuid.empty() &&
       request.partition_property_uuid == request.ordering_property_uuid) ||
      !std::ranges::all_of(
          selected_node->delivered_property_uuids,
          [&](const auto& property_uuid) {
            return property_uuid == request.window_property_uuid ||
                   (!request.partition_property_uuid.empty() &&
                    property_uuid == request.partition_property_uuid) ||
                   (!request.ordering_property_uuid.empty() &&
                    property_uuid == request.ordering_property_uuid);
          })) {
    return refuse("QOW-DIAG-WINDOW-PROPERTY-BINDING",
                  "physical Window properties do not exactly bind runtime terms");
  }
  const bool has_bound_terms =
      !request.partition_terms.empty() || !request.order_terms.empty();
  if ((has_bound_terms != !request.term_binding_evidence_uuid.empty()) ||
      (has_bound_terms &&
       (!IsCanonicalUuid(request.term_binding_evidence_uuid) ||
        request.term_binding_evidence_uuid == request.window_property_uuid ||
        request.term_binding_evidence_uuid ==
            request.partition_property_uuid ||
        request.term_binding_evidence_uuid == request.ordering_property_uuid))) {
    return refuse("QOW-DIAG-WINDOW-PROPERTY-BINDING",
                  "window terms lack an exact engine binding receipt");
  }
  if (!IsCanonicalUuid(request.deterministic_tie_evidence_uuid) ||
      request.deterministic_tie_evidence_uuid ==
          request.term_binding_evidence_uuid ||
      request.deterministic_tie_evidence_uuid == request.window_property_uuid ||
      request.deterministic_tie_evidence_uuid ==
          request.partition_property_uuid ||
      request.deterministic_tie_evidence_uuid ==
          request.ordering_property_uuid) {
    return refuse("QOW-DIAG-WINDOW-TIE",
                  "stable deterministic tie evidence is required");
  }
  if (request.maximum_term_count == 0 ||
      request.partition_terms.size() > request.maximum_term_count ||
      request.order_terms.size() > request.maximum_term_count ||
      request.partition_terms.size() >
          request.maximum_term_count - request.order_terms.size()) {
    return refuse("QOW-DIAG-WINDOW-PEER",
                  "window term resource bound was exceeded");
  }
  if (request.key_batch.has_value()) {
    if (!has_bound_terms || request.key_batch->columns.empty()) {
      return refuse("QOW-DIAG-WINDOW-PROPERTY-BINDING",
                    "materialized window keys are present without bound terms");
    }
    std::vector<bool> consumed(request.key_batch->columns.size(), false);
    for (const auto& term : request.partition_terms) {
      if (term.column < consumed.size()) consumed[term.column] = true;
    }
    for (const auto& term : request.order_terms) {
      if (term.column < consumed.size()) consumed[term.column] = true;
    }
    if (!std::ranges::all_of(consumed,
                             [](const bool value) { return value; })) {
      return refuse("QOW-DIAG-WINDOW-PROPERTY-BINDING",
                    "materialized window-key column was not consumed");
    }
  }

  std::vector<CanonicalDescriptorOrderTerm> partition_terms;
  partition_terms.reserve(request.partition_terms.size());
  for (const auto& term : request.partition_terms) {
    if (term.column >= key_batch->columns.size()) {
      return refuse("QOW-DIAG-WINDOW-PARTITION",
                    "partition term is outside the input schema");
    }
    auto comparable = PartitionOrderTerm(term);
    const auto validation = ValidateCanonicalDescriptorOrderTerm(
        comparable, key_batch->columns[term.column]);
    if (!validation.ok) {
      return refuse("QOW-DIAG-WINDOW-PARTITION", validation.detail);
    }
    partition_terms.push_back(std::move(comparable));
  }
  for (const auto& term : request.order_terms) {
    if (term.column >= key_batch->columns.size()) {
      return refuse("QOW-DIAG-WINDOW-ORDER",
                    "order term is outside the input schema");
    }
    const auto validation = ValidateCanonicalDescriptorOrderTerm(
        term, key_batch->columns[term.column]);
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
  // The runtime materializes two pair matrices even when the window has no
  // explicit PARTITION BY or ORDER BY terms. Charge at least one unit per
  // pair so a termless window cannot bypass the only bound on that memory.
  const auto charged_term_count = std::max<std::size_t>(1, term_count);
  if (pair_count >
          std::numeric_limits<std::size_t>::max() / charged_term_count ||
      pair_count * charged_term_count > request.maximum_pair_comparisons) {
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
            key_batch->rows[left].values[term.column],
            key_batch->rows[right].values[term.column], term);
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
            key_batch->rows[left].values[term.column],
            key_batch->rows[right].values[term.column], term);
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
  if (request.key_batch.has_value()) {
    result.ordered_key_batch.emplace();
    result.ordered_key_batch->columns = request.key_batch->columns;
    result.ordered_key_batch->rows.reserve(row_count);
  }
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
        if (result.ordered_key_batch.has_value()) {
          result.ordered_key_batch->rows.push_back(
              request.key_batch->rows[source_row]);
        }
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
  if (result.ordered_key_batch.has_value()) {
    std::vector<std::uint32_t> key_descriptor_ids;
    key_descriptor_ids.reserve(result.ordered_key_batch->columns.size());
    for (const auto& column : result.ordered_key_batch->columns) {
      key_descriptor_ids.push_back(column.descriptor_id);
    }
    const auto key_output_validation = ValidateCanonicalDescriptorBatch(
        *result.ordered_key_batch, key_descriptor_ids);
    if (!key_output_validation.ok) {
      return refuse("QOW-DIAG-WINDOW-PROPERTY-BINDING",
                    key_output_validation.detail);
    }
  }
  const auto result_authority = RevalidateCanonicalExecutionMgaAuthority(
      request.mga_authority, request.physical_dag);
  if (!result_authority.ok) {
    return refuse(result_authority.diagnostic_code,
                  result_authority.detail);
  }
  result.diagnostic = {};
  result.partition_count = partitions.size();
  result.partition_terms = request.partition_terms;
  result.order_terms = request.order_terms;
  result.window_property_uuid = request.window_property_uuid;
  result.partition_property_uuid = request.partition_property_uuid;
  result.ordering_property_uuid = request.ordering_property_uuid;
  result.term_binding_evidence_uuid = request.term_binding_evidence_uuid;
  result.deterministic_tie_evidence_uuid =
      request.deterministic_tie_evidence_uuid;
  result.explicit_peer_metadata = true;
  result.stable_ties_preserved = true;
  result.weaker_peer_recomputation_forbidden = true;
  result.final_query_order_guaranteed = false;
  result.authority.engine_mga_snapshot_bound = true;
  result.mga_statement_context = request.mga_authority.statement_context;
  result.selected_plan_uuid = request.physical_dag.selected_plan_uuid;
  result.executed_physical_node_id = selected_node->physical_node_id;
  result.causal_counter_id = selected_node->causal_counter_id;
  result.physical_dag = request.physical_dag;
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
  const auto authority_validation = RevalidateCanonicalExecutionMgaAuthority(
      request.mga_authority, request.physical_dag);
  if (!authority_validation.ok) {
    return refuse(authority_validation);
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
      selected_node->implementation_id != "window.row-number.v1" ||
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
  const bool exact_ordering_property =
      selected_node->required_property_uuids.size() == 1 &&
      IsCanonicalUuid(selected_node->required_property_uuids.front());
  const std::string ordering_property_uuid =
      exact_ordering_property
          ? selected_node->required_property_uuids.front()
          : std::string{};
  const auto window_property = std::ranges::find_if(
      selected_node->delivered_property_uuids,
      [&](const auto& property_uuid) {
        return property_uuid != ordering_property_uuid;
      });
  const bool exact_window_property =
      exact_ordering_property &&
      selected_node->delivered_property_uuids.size() == 2 &&
      PropertyCount(selected_node->delivered_property_uuids,
                    ordering_property_uuid) == 1 &&
      window_property != selected_node->delivered_property_uuids.end() &&
      IsCanonicalUuid(*window_property) &&
      *window_property != ordering_property_uuid;
  if (input_node == nullptr ||
      input_node->node_kind != PhysicalNodeKind::kSort ||
      input_node->delivered_property_uuids !=
          std::vector<std::string>{ordering_property_uuid} ||
      !exact_window_property ||
      !IsCanonicalUuid(request.deterministic_order_evidence_uuid) ||
      request.deterministic_order_evidence_uuid == ordering_property_uuid ||
      request.deterministic_order_evidence_uuid == *window_property) {
    return refuse(Refusal(
        "QOW-DIAG-QRY-007-WINDOW-ORDER-REQUIRED-V1",
        "ROW_NUMBER input lacks exact ordering/window property evidence"));
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
  const auto result_authority = RevalidateCanonicalExecutionMgaAuthority(
      request.mga_authority, request.physical_dag);
  if (!result_authority.ok) return refuse(result_authority);

  result.diagnostic = {};
  result.selected_plan_uuid = request.physical_dag.selected_plan_uuid;
  result.executed_physical_node_id = selected_node->physical_node_id;
  result.causal_counter_id = selected_node->causal_counter_id;
  result.mga_statement_context = request.mga_authority.statement_context;
  return result;
}

}  // namespace scratchbird::engine::executor
