// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "executor_foundation.hpp"

#include "datatype_catalog_manifest.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <array>
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

bool DescriptorBatchCarrierIsExactDefault(const DescriptorBatch& batch) {
  const DescriptorBatch empty;
  return batch.columns.empty() &&
         batch.columns.capacity() == empty.columns.capacity() &&
         batch.rows.empty() && batch.rows.capacity() == empty.rows.capacity();
}

bool RowNumberBatchPayloadBytes(const DescriptorBatch& batch,
                                std::uint64_t* bytes) {
  if (bytes == nullptr) return false;
  *bytes = 1;
  for (const auto& row : batch.rows) {
    for (const auto& value : row.values) {
      if (value.encoded_value.size() >
          std::numeric_limits<std::uint64_t>::max() - *bytes) {
        return false;
      }
      *bytes += value.encoded_value.size();
      if (value.binary_value.size() >
          std::numeric_limits<std::uint64_t>::max() - *bytes) {
        return false;
      }
      *bytes += value.binary_value.size();
    }
  }
  return true;
}

bool RowNumberEncodedPayloadBytes(const std::size_t row_count,
                                  std::uint64_t* bytes) {
  if (bytes == nullptr) return false;
  *bytes = 0;
  const auto maximum_row = static_cast<std::uint64_t>(row_count);
  std::uint64_t first = 1;
  std::uint64_t width = 1;
  while (first <= maximum_row) {
    if (first > std::numeric_limits<std::uint64_t>::max() / 10) {
      return false;
    }
    const auto last = std::min(maximum_row, first * 10 - 1);
    const auto count = last - first + 1;
    if (count > std::numeric_limits<std::uint64_t>::max() / width ||
        count * width >
            std::numeric_limits<std::uint64_t>::max() - *bytes) {
      return false;
    }
    *bytes += count * width;
    if (last == maximum_row) break;
    first *= 10;
    ++width;
  }
  return true;
}

bool NtileOperandPayloadBytes(
    const scratchbird::engine::internal_api::EngineTypedValue& operand,
    std::uint64_t* bytes) {
  if (bytes == nullptr) return false;
  *bytes = 1;
  const std::array<std::size_t, 6> payloads{
      operand.descriptor.descriptor_uuid.canonical.size(),
      operand.descriptor.descriptor_kind.size(),
      operand.descriptor.canonical_type_name.size(),
      operand.descriptor.encoded_descriptor.size(), operand.encoded_value.size(),
      operand.binary_value.size()};
  for (const auto payload : payloads) {
    if (payload > std::numeric_limits<std::uint64_t>::max() - *bytes) {
      return false;
    }
    *bytes += payload;
  }
  return true;
}

constexpr std::uint64_t kRealRankingRatioTextMaximumBytes = 36;
constexpr std::uint64_t kRealRankingConversionWorkspaceMaximumBytes =
    2 * kRealRankingRatioTextMaximumBytes;

bool RealRankingEncodedPayloadBytes(const std::size_t row_count,
                                    std::uint64_t* bytes) {
  if (bytes == nullptr ||
      row_count > std::numeric_limits<std::uint64_t>::max() /
                      kRealRankingRatioTextMaximumBytes) {
    return false;
  }
  *bytes = static_cast<std::uint64_t>(row_count) *
           kRealRankingRatioTextMaximumBytes;
  return true;
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

std::string CanonicalCoreDatatypeUuid(const std::string_view stable_name) {
  static const auto manifest =
      scratchbird::core::datatypes::LoadCurrentCoreDatatypeCatalogManifest();
  if (!manifest.ok()) return {};
  const auto found = std::ranges::find_if(
      manifest.manifest.descriptor_rows, [&](const auto& row) {
        return row.stable_name == stable_name;
      });
  return found == manifest.manifest.descriptor_rows.end()
             ? std::string{}
             : scratchbird::core::uuid::UuidToString(
                   found->descriptor_uuid.value);
}

std::optional<std::string_view> CanonicalDescriptorField(
    const scratchbird::engine::internal_api::EngineDescriptor& descriptor,
    const std::string_view key) {
  const auto prefix = std::string(key) + "=";
  std::optional<std::string_view> result;
  std::size_t begin = 0;
  while (begin <= descriptor.encoded_descriptor.size()) {
    const auto end = descriptor.encoded_descriptor.find(';', begin);
    const auto field = std::string_view(descriptor.encoded_descriptor).substr(
        begin, end == std::string::npos ? std::string::npos : end - begin);
    if (field.starts_with(prefix)) {
      if (result.has_value()) return std::nullopt;
      result = field.substr(prefix.size());
    }
    if (end == std::string::npos) break;
    begin = end + 1;
  }
  return result;
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
  const bool operator_local_stage =
      request.operator_local_parent_implementation_id == "window.lag.v1";
  if (selected_node == nullptr ||
      selected_node->node_kind != PhysicalNodeKind::kWindow ||
      selected_node->implementation_id !=
          (operator_local_stage
               ? request.operator_local_parent_implementation_id
               : "window.partition-order-peer.v1") ||
      (!request.operator_local_parent_implementation_id.empty() &&
       !operator_local_stage) ||
      selected_node->input_physical_node_ids.size() != 1) {
    return refuse("QOW-DIAG-WINDOW-PEER",
                  "selected physical node is not the canonical unary "
                  "partition/order/peer window stage");
  }
  for (const auto& node : request.physical_dag.nodes) {
    if (node.physical_node_id ==
        selected_node->input_physical_node_ids.front()) {
      input_node = &node;
      break;
    }
  }
  const bool exact_stage_output =
      input_node != nullptr &&
      ((!operator_local_stage &&
        selected_node->output_descriptor_ids ==
            input_node->output_descriptor_ids) ||
       (operator_local_stage &&
        selected_node->output_descriptor_ids.size() ==
            input_node->output_descriptor_ids.size() + 1 &&
        std::equal(input_node->output_descriptor_ids.begin(),
                   input_node->output_descriptor_ids.end(),
                   selected_node->output_descriptor_ids.begin())));
  if (!exact_stage_output) {
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
      result.ordered_batch, input_node->output_descriptor_ids);
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
  result.operator_local_stage = operator_local_stage;
  result.operator_local_parent_implementation_id =
      request.operator_local_parent_implementation_id;
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
namespace {
CanonicalDescriptorRowNumberResult ExecuteCanonicalDescriptorRowNumberBound(
    const CanonicalDescriptorRowNumberRequest& request,
    const TypedPhysicalNodeDag& execution_dag,
    const DescriptorBatch& execution_ordered_input_batch,
    const bool borrowed_execution_carriers) {
  using scratchbird::engine::internal_api::EngineTypedValue;
  using scratchbird::engine::internal_api::EngineValueState;

  CanonicalDescriptorRowNumberResult result;
  const auto refuse = [&](DescriptorRuntimeDiagnostic diagnostic) {
    result.diagnostic = std::move(diagnostic);
    result.output_batch = {};
    return result;
  };
  if (borrowed_execution_carriers &&
      (!TypedPhysicalNodeDagCarrierIsExactDefault(request.physical_dag) ||
       !DescriptorBatchCarrierIsExactDefault(
           request.ordered_input_batch))) {
    return refuse(Refusal(
        "QOW-DIAG-QRY-007-WINDOW-PHYSICAL-ROUTE-V1",
        "ROW_NUMBER request carries conflicting owned execution carriers"));
  }
  const auto authority_validation = RevalidateCanonicalExecutionMgaAuthority(
      request.mga_authority, execution_dag);
  if (!authority_validation.ok) {
    return refuse(authority_validation);
  }
  if (request.selected_physical_node_id == 0 ||
      request.selected_physical_node_id !=
          execution_dag.root_physical_node_id) {
    return refuse(Refusal("SBLR.PLAN_TREE.INVALID_HANDLE",
                          "selected window node is not the root"));
  }

  const PhysicalNodeRecord* selected_node = nullptr;
  const PhysicalNodeRecord* input_node = nullptr;
  for (const auto& node : execution_dag.nodes) {
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
  for (const auto& node : execution_dag.nodes) {
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
      execution_ordered_input_batch, input_node->output_descriptor_ids);
  if (!input_validation.ok) return refuse(std::move(input_validation));
  if (execution_ordered_input_batch.rows.size() >
      static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max())) {
    return refuse(Refusal("QOW-DIAG-QRY-007-WINDOW-OVERFLOW-V1",
                          "ROW_NUMBER exceeds int64 result width"));
  }

  std::uint64_t input_payload_bytes = 0;
  std::uint64_t row_number_payload_bytes = 0;
  if (!RowNumberBatchPayloadBytes(execution_ordered_input_batch,
                                  &input_payload_bytes) ||
      !RowNumberEncodedPayloadBytes(
          execution_ordered_input_batch.rows.size(),
          &row_number_payload_bytes) ||
      selected_node->memory_bytes_required == 0 ||
      selected_node->memory_bytes_required > execution_dag.memory_budget_bytes ||
      selected_node->memory_bytes_required >
          static_cast<std::uint64_t>(
              std::numeric_limits<std::size_t>::max())) {
    return refuse(Refusal(
        "SBLR.PLAN_TREE.RESOURCE_LIMIT",
        "ROW_NUMBER memory grant or runtime payload accounting is invalid"));
  }
  auto remaining_memory_bytes = selected_node->memory_bytes_required;
  const auto charge = [&](const std::uint64_t bytes) {
    if (bytes > remaining_memory_bytes) return false;
    remaining_memory_bytes -= bytes;
    return true;
  };
  if (!charge(input_payload_bytes) || !charge(input_payload_bytes) ||
      !charge(row_number_payload_bytes)) {
    return refuse(Refusal(
        "SBLR.PLAN_TREE.RESOURCE_LIMIT",
        "ROW_NUMBER runtime materialization exceeds its selected node grant"));
  }

  result.output_batch.columns = execution_ordered_input_batch.columns;
  result.output_batch.columns.push_back(request.row_number_column);
  result.output_batch.rows = execution_ordered_input_batch.rows;
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
      request.mga_authority, execution_dag);
  if (!result_authority.ok) return refuse(result_authority);

  result.diagnostic = {};
  result.selected_plan_uuid = execution_dag.selected_plan_uuid;
  result.executed_physical_node_id = selected_node->physical_node_id;
  result.causal_counter_id = selected_node->causal_counter_id;
  result.mga_statement_context = request.mga_authority.statement_context;
  return result;
}
}  // namespace

CanonicalDescriptorRowNumberResult ExecuteCanonicalDescriptorRowNumber(
    const CanonicalDescriptorRowNumberRequest& request) {
  return ExecuteCanonicalDescriptorRowNumberBound(
      request, request.physical_dag, request.ordered_input_batch, false);
}

CanonicalDescriptorRowNumberResult ExecuteCanonicalDescriptorRowNumber(
    const CanonicalDescriptorRowNumberRequest& request,
    const TypedPhysicalNodeDag& borrowed_execution_dag,
    const DescriptorBatch& borrowed_ordered_input_batch) {
  return ExecuteCanonicalDescriptorRowNumberBound(
      request, borrowed_execution_dag, borrowed_ordered_input_batch, true);
}

namespace {
CanonicalDescriptorNtileResult ExecuteCanonicalDescriptorNtileBound(
    const CanonicalDescriptorNtileRequest& request,
    const TypedPhysicalNodeDag& execution_dag,
    const DescriptorBatch& execution_ordered_input_batch,
    const bool borrowed_execution_carriers) {
  using scratchbird::engine::internal_api::EngineValueState;

  CanonicalDescriptorNtileResult result;
  const auto refuse = [&](DescriptorRuntimeDiagnostic diagnostic) {
    result.diagnostic = std::move(diagnostic);
    result.output_batch = {};
    return result;
  };
  if (borrowed_execution_carriers &&
      (!TypedPhysicalNodeDagCarrierIsExactDefault(request.physical_dag) ||
       !DescriptorBatchCarrierIsExactDefault(
           request.ordered_input_batch))) {
    return refuse(Refusal(
        "QOW-DIAG-QRY-007-WINDOW-PHYSICAL-ROUTE-V1",
        "NTILE request carries conflicting owned execution carriers"));
  }
  const auto authority_validation = RevalidateCanonicalExecutionMgaAuthority(
      request.mga_authority, execution_dag);
  if (!authority_validation.ok) return refuse(authority_validation);
  if (request.selected_physical_node_id == 0 ||
      request.selected_physical_node_id !=
          execution_dag.root_physical_node_id) {
    return refuse(Refusal("SBLR.PLAN_TREE.INVALID_HANDLE",
                          "selected NTILE window node is not the root"));
  }

  const PhysicalNodeRecord* selected_node = nullptr;
  const PhysicalNodeRecord* input_node = nullptr;
  for (const auto& node : execution_dag.nodes) {
    if (node.physical_node_id == request.selected_physical_node_id) {
      selected_node = &node;
    }
  }
  if (selected_node == nullptr ||
      selected_node->node_kind != PhysicalNodeKind::kWindow ||
      selected_node->implementation_id != "window.ntile.v1" ||
      selected_node->input_physical_node_ids.size() != 1) {
    return refuse(Refusal(
        "QOW-DIAG-QRY-007-WINDOW-PHYSICAL-ROUTE-V1",
        "NTILE requires one selected window node"));
  }
  for (const auto& node : execution_dag.nodes) {
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
  if (input_node == nullptr || input_node->node_kind != PhysicalNodeKind::kSort ||
      input_node->delivered_property_uuids !=
          std::vector<std::string>{ordering_property_uuid} ||
      !exact_window_property || request.function_abi_version != 1 ||
      request.builtin_id != "sb.window.ntile" ||
      request.function_uuid != "019de5fc-2400-7047-9474-232ca488c094" ||
      !IsCanonicalUuid(request.function_uuid) ||
      selected_node->executor_capability_abi_version != 1 ||
      !selected_node->engine_capability_validated ||
      !IsCanonicalUuid(selected_node->executor_capability_uuid) ||
      !IsCanonicalUuid(request.order_term_binding_evidence_uuid) ||
      !IsCanonicalUuid(request.deterministic_order_evidence_uuid) ||
      request.order_term_binding_evidence_uuid == ordering_property_uuid ||
      request.order_term_binding_evidence_uuid == *window_property ||
      request.order_term_binding_evidence_uuid == request.function_uuid ||
      request.deterministic_order_evidence_uuid ==
          request.order_term_binding_evidence_uuid ||
      request.deterministic_order_evidence_uuid == ordering_property_uuid ||
      request.deterministic_order_evidence_uuid == *window_property ||
      request.deterministic_order_evidence_uuid == request.function_uuid) {
    return refuse(Refusal(
        "QOW-DIAG-QRY-007-WINDOW-ORDER-REQUIRED-V1",
        "NTILE input lacks exact registry, ordering, or window evidence"));
  }

  std::vector<std::uint32_t> output_descriptor_ids =
      input_node->output_descriptor_ids;
  output_descriptor_ids.push_back(request.ntile_column.descriptor_id);
  const auto decoded_bucket_count =
      DecodeInt64Value(request.bucket_count_operand);
  const auto output_type_uuid = CanonicalDescriptorField(
      request.ntile_column.descriptor, "type_uuid");
  const auto output_nullability = CanonicalDescriptorField(
      request.ntile_column.descriptor, "nullability");
  const auto operand_type_uuid = CanonicalDescriptorField(
      request.bucket_count_operand.descriptor, "type_uuid");
  const auto operand_nullability = CanonicalDescriptorField(
      request.bucket_count_operand.descriptor, "nullability");
  if (selected_node->output_descriptor_ids != output_descriptor_ids ||
      request.ntile_column.descriptor_id == 0 ||
      request.ntile_column.nullable ||
      request.ntile_column.descriptor.descriptor_kind != "scalar" ||
      request.ntile_column.descriptor.canonical_type_name != "int64" ||
      !scratchbird::engine::internal_api::QowCanonicalDescriptorIdentityV1(
          request.ntile_column.descriptor) ||
      !output_type_uuid.has_value() ||
      !IsCanonicalUuid(*output_type_uuid) ||
      !output_nullability.has_value() ||
      *output_nullability != "non_null" ||
      request.ntile_column.descriptor.descriptor_uuid.canonical ==
          *output_type_uuid ||
      request.ntile_column.descriptor.descriptor_uuid.canonical ==
          request.function_uuid ||
      request.ntile_column.descriptor.descriptor_uuid.canonical ==
          ordering_property_uuid ||
      request.ntile_column.descriptor.descriptor_uuid.canonical ==
          *window_property ||
      request.ntile_column.descriptor.descriptor_uuid.canonical ==
          request.order_term_binding_evidence_uuid ||
      request.ntile_column.descriptor.descriptor_uuid.canonical ==
          request.deterministic_order_evidence_uuid ||
      request.order_term.column >=
          execution_ordered_input_batch.columns.size() ||
      request.bucket_count_operand.state != EngineValueState::value ||
      request.bucket_count_operand.is_null ||
      !request.bucket_count_operand.binary_value.empty() ||
      request.bucket_count_operand.descriptor.descriptor_kind != "scalar" ||
      request.bucket_count_operand.descriptor.canonical_type_name != "int64" ||
      !scratchbird::engine::internal_api::QowCanonicalDescriptorIdentityV1(
          request.bucket_count_operand.descriptor) ||
      !operand_type_uuid.has_value() ||
      !IsCanonicalUuid(*operand_type_uuid) ||
      *operand_type_uuid != *output_type_uuid ||
      !operand_nullability.has_value() ||
      *operand_nullability != "non_null" ||
      request.bucket_count_operand.descriptor.descriptor_uuid.canonical ==
          *operand_type_uuid ||
      request.bucket_count_operand.descriptor.descriptor_uuid.canonical ==
          request.ntile_column.descriptor.descriptor_uuid.canonical ||
      request.bucket_count_operand.descriptor.descriptor_uuid.canonical ==
          request.function_uuid ||
      request.bucket_count_operand.descriptor.descriptor_uuid.canonical ==
          ordering_property_uuid ||
      request.bucket_count_operand.descriptor.descriptor_uuid.canonical ==
          *window_property ||
      request.bucket_count_operand.descriptor.descriptor_uuid.canonical ==
          request.order_term_binding_evidence_uuid ||
      request.bucket_count_operand.descriptor.descriptor_uuid.canonical ==
          request.deterministic_order_evidence_uuid ||
      !decoded_bucket_count.ok() || decoded_bucket_count.value <= 0) {
    return refuse(Refusal(
        "SBLR.PLAN_TREE.INVALID_HANDLE",
        "NTILE output and positive int64 bucket operand are not exact"));
  }
  const auto order_validation = ValidateCanonicalDescriptorOrderTerm(
      request.order_term,
      execution_ordered_input_batch.columns[request.order_term.column]);
  if (!order_validation.ok) return refuse(order_validation);
  const auto input_validation = ValidateCanonicalDescriptorBatch(
      execution_ordered_input_batch, input_node->output_descriptor_ids);
  if (!input_validation.ok) return refuse(std::move(input_validation));
  if (execution_ordered_input_batch.rows.size() >
      static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max())) {
    return refuse(Refusal("QOW-DIAG-QRY-007-WINDOW-OVERFLOW-V1",
                          "NTILE exceeds int64 result width"));
  }

  std::uint64_t input_payload_bytes = 0;
  std::uint64_t ntile_payload_bytes = 0;
  std::uint64_t operand_payload_bytes = 0;
  std::uint64_t binding_receipt_workspace_bytes = 0;
  if (!RowNumberBatchPayloadBytes(execution_ordered_input_batch,
                                  &input_payload_bytes) ||
      !RowNumberEncodedPayloadBytes(execution_ordered_input_batch.rows.size(),
                                    &ntile_payload_bytes) ||
      !NtileOperandPayloadBytes(request.bucket_count_operand,
                                &operand_payload_bytes) ||
      !PlanCanonicalDescriptorOrderTermBindingEvidenceWorkspace(
          request.order_term, ordering_property_uuid,
          &binding_receipt_workspace_bytes) ||
      selected_node->memory_bytes_required == 0 ||
      selected_node->memory_bytes_required > execution_dag.memory_budget_bytes ||
      selected_node->memory_bytes_required >
          static_cast<std::uint64_t>(
              std::numeric_limits<std::size_t>::max())) {
    return refuse(Refusal(
        "SBLR.PLAN_TREE.RESOURCE_LIMIT",
        "NTILE memory grant or runtime payload accounting is invalid"));
  }
  auto remaining_memory_bytes = selected_node->memory_bytes_required;
  const auto charge = [&](const std::uint64_t bytes) {
    if (bytes > remaining_memory_bytes) return false;
    remaining_memory_bytes -= bytes;
    return true;
  };
  if (!charge(input_payload_bytes) || !charge(input_payload_bytes) ||
      !charge(ntile_payload_bytes) || !charge(operand_payload_bytes) ||
      !charge(binding_receipt_workspace_bytes)) {
    return refuse(Refusal(
        "SBLR.PLAN_TREE.RESOURCE_LIMIT",
        "NTILE runtime materialization exceeds its selected node grant"));
  }
  std::uint64_t actual_binding_receipt_workspace_bytes = 0;
  const auto expected_order_term_binding_evidence_uuid =
      ComputeCanonicalDescriptorOrderTermBindingEvidenceUuid(
          request.order_term, ordering_property_uuid,
          binding_receipt_workspace_bytes,
          &actual_binding_receipt_workspace_bytes);
  if (expected_order_term_binding_evidence_uuid.empty() ||
      actual_binding_receipt_workspace_bytes !=
          binding_receipt_workspace_bytes ||
      selected_node->executor_capability_uuid !=
          expected_order_term_binding_evidence_uuid ||
      request.order_term_binding_evidence_uuid !=
          selected_node->executor_capability_uuid) {
    return refuse(Refusal(
        "QOW-DIAG-QRY-007-WINDOW-ORDER-REQUIRED-V1",
        "NTILE order term differs from optimizer-published capability "
        "evidence"));
  }

  result.output_batch.columns = execution_ordered_input_batch.columns;
  result.output_batch.columns.push_back(request.ntile_column);
  result.output_batch.rows = execution_ordered_input_batch.rows;
  const auto row_count = result.output_batch.rows.size();
  const auto buckets = static_cast<std::uint64_t>(decoded_bucket_count.value);
  for (std::size_t row = 0; row < row_count; ++row) {
    CanonicalWindowNtileValueRequest value_request;
    value_request.function_abi_version = request.function_abi_version;
    value_request.builtin_id = request.builtin_id;
    value_request.function_uuid = request.function_uuid;
    value_request.output_descriptor = request.ntile_column.descriptor;
    value_request.zero_based_partition_position = row;
    value_request.partition_row_count = row_count;
    value_request.bucket_count = buckets;
    auto value = ComputeCanonicalWindowNtileValue(value_request);
    if (!value.diagnostic.ok) return refuse(std::move(value.diagnostic));
    result.output_batch.rows[row].values.push_back(std::move(value.value));
  }
  auto output_validation = ValidateCanonicalDescriptorBatch(
      result.output_batch, selected_node->output_descriptor_ids);
  if (!output_validation.ok) return refuse(std::move(output_validation));
  const auto result_authority = RevalidateCanonicalExecutionMgaAuthority(
      request.mga_authority, execution_dag);
  if (!result_authority.ok) return refuse(result_authority);

  result.diagnostic = {};
  result.resolved_bucket_count = buckets;
  result.peak_auxiliary_workspace_bytes =
      binding_receipt_workspace_bytes + operand_payload_bytes;
  result.selected_plan_uuid = execution_dag.selected_plan_uuid;
  result.executed_physical_node_id = selected_node->physical_node_id;
  result.causal_counter_id = selected_node->causal_counter_id;
  result.mga_statement_context = request.mga_authority.statement_context;
  return result;
}
}  // namespace

CanonicalDescriptorNtileResult ExecuteCanonicalDescriptorNtile(
    const CanonicalDescriptorNtileRequest& request) {
  return ExecuteCanonicalDescriptorNtileBound(
      request, request.physical_dag, request.ordered_input_batch, false);
}

CanonicalDescriptorNtileResult ExecuteCanonicalDescriptorNtile(
    const CanonicalDescriptorNtileRequest& request,
    const TypedPhysicalNodeDag& borrowed_execution_dag,
    const DescriptorBatch& borrowed_ordered_input_batch) {
  return ExecuteCanonicalDescriptorNtileBound(
      request, borrowed_execution_dag, borrowed_ordered_input_batch, true);
}

namespace {
CanonicalDescriptorLagWindowResult ExecuteCanonicalDescriptorLagWindowBound(
    const CanonicalDescriptorLagWindowRequest& request,
    const TypedPhysicalNodeDag& execution_dag,
    const DescriptorBatch& execution_ordered_input_batch,
    const bool borrowed_execution_carriers) {
  CanonicalDescriptorLagWindowResult result;
  const auto refuse = [&](DescriptorRuntimeDiagnostic diagnostic) {
    result.diagnostic = std::move(diagnostic);
    result.output_batch = {};
    return result;
  };
  if (borrowed_execution_carriers &&
      (!TypedPhysicalNodeDagCarrierIsExactDefault(request.physical_dag) ||
       !DescriptorBatchCarrierIsExactDefault(request.ordered_input_batch))) {
    return refuse(Refusal(
        "QOW-DIAG-QRY-007-WINDOW-PHYSICAL-ROUTE-V1",
        "navigation window request carries conflicting owned execution carriers"));
  }
  const auto authority_validation = RevalidateCanonicalExecutionMgaAuthority(
      request.mga_authority, execution_dag);
  if (!authority_validation.ok) return refuse(authority_validation);
  if (request.selected_physical_node_id == 0 ||
      request.selected_physical_node_id !=
          execution_dag.root_physical_node_id) {
    return refuse(Refusal("SBLR.PLAN_TREE.INVALID_HANDLE",
                          "selected navigation window node is not the root"));
  }

  const bool lag = request.builtin_id == "sb.window.lag" &&
                   request.function_uuid ==
                       "019de5fc-2400-782c-8436-9ac310301738";
  constexpr std::string_view implementation_id = "window.lag.v1";
  if (request.function_abi_version != 1 || !lag ||
      !IsCanonicalUuid(request.function_uuid)) {
    return refuse(Refusal(
        "QOW-DIAG-WINDOW-FUNCTION-DESCRIPTOR",
        "navigation window registry identity is not exact"));
  }

  const PhysicalNodeRecord* selected_node = nullptr;
  const PhysicalNodeRecord* input_node = nullptr;
  for (const auto& node : execution_dag.nodes) {
    if (node.physical_node_id == request.selected_physical_node_id) {
      selected_node = &node;
    }
  }
  if (selected_node == nullptr ||
      selected_node->node_kind != PhysicalNodeKind::kWindow ||
      selected_node->implementation_id != implementation_id ||
      selected_node->input_physical_node_ids.size() != 1) {
    return refuse(Refusal(
        "QOW-DIAG-QRY-007-WINDOW-PHYSICAL-ROUTE-V1",
        "navigation window requires one exact selected window node"));
  }
  for (const auto& node : execution_dag.nodes) {
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
  if (input_node == nullptr || input_node->node_kind != PhysicalNodeKind::kSort ||
      input_node->delivered_property_uuids !=
          std::vector<std::string>{ordering_property_uuid} ||
      !exact_window_property ||
      selected_node->executor_capability_abi_version != 1 ||
      !selected_node->engine_capability_validated ||
      !IsCanonicalUuid(selected_node->executor_capability_uuid) ||
      selected_node->executor_capability_uuid !=
          request.executor_capability_uuid ||
      !IsCanonicalUuid(request.window_frame_descriptor_uuid) ||
      !IsCanonicalUuid(request.order_term_binding_evidence_uuid) ||
      !IsCanonicalUuid(request.deterministic_order_evidence_uuid) ||
      !IsCanonicalUuid(request.frame_property_binding_evidence_uuid)) {
    return refuse(Refusal(
        "QOW-DIAG-QRY-007-WINDOW-ORDER-REQUIRED-V1",
        "navigation window lacks exact registry, ordering, frame, or window evidence"));
  }
  std::vector<std::uint32_t> output_descriptor_ids =
      input_node->output_descriptor_ids;
  output_descriptor_ids.push_back(request.result_column.descriptor_id);
  if (selected_node->output_descriptor_ids != output_descriptor_ids ||
      request.value_column >= execution_ordered_input_batch.columns.size() ||
      request.order_term.column >=
          execution_ordered_input_batch.columns.size() ||
      request.result_column.descriptor_id == 0 ||
      !request.result_column.nullable ||
      request.result_column.descriptor.descriptor_kind != "scalar" ||
      !scratchbird::engine::internal_api::QowCanonicalDescriptorIdentityV1(
          request.result_column.descriptor) ||
      !CanonicalDerivedDescriptorTypeMatches(
          execution_ordered_input_batch.columns[request.value_column].descriptor,
          execution_ordered_input_batch.columns[request.value_column].nullable,
          request.result_column.descriptor, true)) {
    return refuse(Refusal(
        "SBLR.PLAN_TREE.INVALID_HANDLE",
        "navigation value or nullable result descriptor is not exact"));
  }
  const auto input_validation = ValidateCanonicalDescriptorBatch(
      execution_ordered_input_batch, input_node->output_descriptor_ids);
  if (!input_validation.ok) return refuse(input_validation);
  const auto order_validation = ValidateCanonicalDescriptorOrderTerm(
      request.order_term,
      execution_ordered_input_batch.columns[request.order_term.column]);
  if (!order_validation.ok) return refuse(order_validation);
  const auto result_type_uuid = CanonicalDescriptorField(
      request.result_column.descriptor, "type_uuid");
  const auto canonical_int64_type_uuid = CanonicalCoreDatatypeUuid("int64");
  const std::array<std::string_view, 11> independent_identities{
      request.function_uuid,
      execution_ordered_input_batch.columns[request.value_column]
          .descriptor.descriptor_uuid.canonical,
      request.result_column.descriptor.descriptor_uuid.canonical,
      result_type_uuid.value_or(std::string_view{}),
      ordering_property_uuid,
      *window_property,
      request.window_frame_descriptor_uuid,
      request.executor_capability_uuid,
      request.order_term_binding_evidence_uuid,
      request.deterministic_order_evidence_uuid,
      request.frame_property_binding_evidence_uuid};
  if (!result_type_uuid.has_value() || !IsCanonicalUuid(*result_type_uuid) ||
      canonical_int64_type_uuid.empty() ||
      *result_type_uuid != canonical_int64_type_uuid ||
      execution_ordered_input_batch.columns[request.value_column]
              .descriptor.canonical_type_name != "int64" ||
      request.result_column.descriptor.canonical_type_name != "int64") {
    return refuse(Refusal(
        "SBLR.PLAN_TREE.INVALID_HANDLE",
        "LAG source and result are not exact canonical int64"));
  }
  for (std::size_t left = 0; left < independent_identities.size(); ++left) {
    for (std::size_t right = left + 1;
         right < independent_identities.size(); ++right) {
      if (independent_identities[left] == independent_identities[right]) {
        return refuse(Refusal(
            "QOW-DIAG-WINDOW-PROPERTY-BINDING",
            "navigation window evidence identities are not independent"));
      }
    }
  }
  std::uint64_t planned_receipt_workspace_bytes = 0;
  std::uint64_t actual_receipt_workspace_bytes = 0;
  if (!PlanCanonicalDescriptorOrderTermBindingEvidenceWorkspace(
          request.order_term, ordering_property_uuid,
          &planned_receipt_workspace_bytes) ||
      ComputeCanonicalDescriptorOrderTermBindingEvidenceUuid(
          request.order_term, ordering_property_uuid,
          planned_receipt_workspace_bytes,
          &actual_receipt_workspace_bytes) !=
          request.order_term_binding_evidence_uuid ||
      actual_receipt_workspace_bytes != planned_receipt_workspace_bytes) {
    return refuse(Refusal(
        "QOW-DIAG-WINDOW-PROPERTY-BINDING",
        "navigation order-term binding receipt is not exact"));
  }

  // Refuse before QOW-401/QOW-402 or the value foundation can allocate any
  // pair matrix, frame-reference vector, copied input, or value vector. The
  // selected grant covers the retained input and output payloads plus a
  // conservative peak for every operator-local navigation carrier.
  const auto checked_multiply = [](const std::uint64_t left,
                                   const std::uint64_t right,
                                   std::uint64_t* product) {
    if (product == nullptr ||
        (left != 0 && right > std::numeric_limits<std::uint64_t>::max() / left)) {
      return false;
    }
    *product = left * right;
    return true;
  };
  const auto checked_add = [](const std::uint64_t left,
                              const std::uint64_t right,
                              std::uint64_t* sum) {
    if (sum == nullptr ||
        right > std::numeric_limits<std::uint64_t>::max() - left) {
      return false;
    }
    *sum = left + right;
    return true;
  };
  const auto row_count = execution_ordered_input_batch.rows.size();
  std::uint64_t pair_count_u64 = 0;
  std::uint64_t matrix_bytes = 0;
  std::uint64_t reference_bytes = 0;
  std::uint64_t metadata_bytes = 0;
  std::uint64_t copied_row_metadata_bytes = 0;
  std::uint64_t value_vector_bytes = 0;
  std::uint64_t partition_index_bytes = 0;
  std::uint64_t partition_vector_bytes = 0;
  std::uint64_t input_payload_bytes = 0;
  std::uint64_t navigation_payload_bytes = 0;
  std::uint64_t output_payload_bytes = 0;
  std::uint64_t planned_auxiliary_workspace_bytes = 0;
  std::uint64_t planned_peak_memory_bytes = 0;
  const auto row_count_u64 = static_cast<std::uint64_t>(row_count);
  for (const auto& row : execution_ordered_input_batch.rows) {
    const auto& value = row.values[request.value_column];
    if (!checked_add(navigation_payload_bytes, value.encoded_value.size(),
                     &navigation_payload_bytes) ||
        !checked_add(navigation_payload_bytes, value.binary_value.size(),
                     &navigation_payload_bytes)) {
      return refuse(Refusal("SBLR.PLAN_TREE.RESOURCE_LIMIT",
                            "LAG value payload accounting overflowed"));
    }
  }
  if (!RowNumberBatchPayloadBytes(execution_ordered_input_batch,
                                  &input_payload_bytes) ||
      !checked_multiply(row_count_u64, row_count_u64, &pair_count_u64) ||
      pair_count_u64 > request.maximum_pair_comparisons ||
      pair_count_u64 > request.maximum_effective_row_references ||
      !checked_multiply(pair_count_u64, 2, &matrix_bytes) ||
      !checked_multiply(pair_count_u64, sizeof(std::size_t),
                        &reference_bytes) ||
      !checked_multiply(
          row_count_u64,
          sizeof(CanonicalWindowRowPeerMetadata) +
              sizeof(CanonicalWindowEffectiveFrame),
          &metadata_bytes) ||
      !checked_multiply(row_count_u64,
                        sizeof(CanonicalWindowRowPeerMetadata),
                        &copied_row_metadata_bytes) ||
      !checked_multiply(
          row_count_u64,
          2 * sizeof(scratchbird::engine::internal_api::EngineTypedValue) +
              sizeof(std::uint64_t),
          &value_vector_bytes) ||
      !checked_multiply(row_count_u64, sizeof(std::size_t),
                        &partition_index_bytes) ||
      !checked_multiply(row_count_u64, sizeof(std::vector<std::size_t>),
                        &partition_vector_bytes) ||
      !checked_add(input_payload_bytes, navigation_payload_bytes,
                   &output_payload_bytes) ||
      !checked_add(matrix_bytes, reference_bytes,
                   &planned_auxiliary_workspace_bytes) ||
      !checked_add(planned_auxiliary_workspace_bytes, metadata_bytes,
                   &planned_auxiliary_workspace_bytes) ||
      !checked_add(planned_auxiliary_workspace_bytes,
                   copied_row_metadata_bytes,
                   &planned_auxiliary_workspace_bytes) ||
      !checked_add(planned_auxiliary_workspace_bytes, value_vector_bytes,
                   &planned_auxiliary_workspace_bytes) ||
      !checked_add(planned_auxiliary_workspace_bytes, partition_index_bytes,
                   &planned_auxiliary_workspace_bytes) ||
      !checked_add(planned_auxiliary_workspace_bytes, partition_vector_bytes,
                   &planned_auxiliary_workspace_bytes) ||
      !checked_add(planned_auxiliary_workspace_bytes,
                   sizeof(std::vector<std::vector<std::size_t>>),
                   &planned_auxiliary_workspace_bytes) ||
      !checked_add(planned_auxiliary_workspace_bytes,
                   planned_receipt_workspace_bytes,
                   &planned_auxiliary_workspace_bytes) ||
      // CanonicalWindowPartitionOrderRequest owns one bounded copy until the
      // stage returns; account it as auxiliary beside the retained input.
      !checked_add(planned_auxiliary_workspace_bytes, input_payload_bytes,
                   &planned_auxiliary_workspace_bytes) ||
      // QOW-402 retains its input while materializing the frame result, and
      // the value foundation retains converted sources beside final values.
      !checked_add(planned_auxiliary_workspace_bytes, input_payload_bytes,
                   &planned_auxiliary_workspace_bytes) ||
      !checked_add(planned_auxiliary_workspace_bytes,
                   navigation_payload_bytes,
                   &planned_auxiliary_workspace_bytes) ||
      !checked_add(input_payload_bytes, output_payload_bytes,
                   &planned_peak_memory_bytes) ||
      !checked_add(planned_peak_memory_bytes,
                   planned_auxiliary_workspace_bytes,
                   &planned_peak_memory_bytes) ||
      selected_node->memory_bytes_required == 0 ||
      selected_node->memory_bytes_required > execution_dag.memory_budget_bytes ||
      selected_node->memory_bytes_required >
          static_cast<std::uint64_t>(
              std::numeric_limits<std::size_t>::max()) ||
      planned_peak_memory_bytes > selected_node->memory_bytes_required) {
    return refuse(Refusal(
        "SBLR.PLAN_TREE.RESOURCE_LIMIT",
        "LAG runtime materialization exceeds its selected node grant"));
  }

  CanonicalWindowPartitionOrderRequest partition_request;
  partition_request.physical_dag = execution_dag;
  partition_request.selected_physical_node_id =
      request.selected_physical_node_id;
  partition_request.input_batch = execution_ordered_input_batch;
  partition_request.order_terms = {request.order_term};
  partition_request.window_property_uuid = *window_property;
  partition_request.ordering_property_uuid = ordering_property_uuid;
  partition_request.term_binding_evidence_uuid =
      request.order_term_binding_evidence_uuid;
  partition_request.deterministic_tie_evidence_uuid =
      request.deterministic_order_evidence_uuid;
  partition_request.operator_local_parent_implementation_id =
      std::string(implementation_id);
  partition_request.maximum_term_count = 1;
  partition_request.maximum_pair_comparisons =
      request.maximum_pair_comparisons;
  partition_request.mga_authority = request.mga_authority;
  auto partition = ExecuteCanonicalWindowPartitionOrder(partition_request);
  if (!partition.diagnostic.ok) return refuse(std::move(partition.diagnostic));

  CanonicalWindowFrameRequest frame_request;
  frame_request.partition_order = std::move(partition);
  frame_request.frame.frame_descriptor_uuid =
      request.window_frame_descriptor_uuid;
  frame_request.frame.frame_specified = false;
  frame_request.frame_property_binding_evidence_uuid =
      request.frame_property_binding_evidence_uuid;
  frame_request.maximum_effective_row_references =
      request.maximum_effective_row_references;
  frame_request.mga_authority = request.mga_authority;
  auto frames = ExecuteCanonicalWindowFrames(frame_request);
  if (!frames.diagnostic.ok) return refuse(std::move(frames.diagnostic));

  std::size_t effective_frame_row_reference_count = 0;
  for (const auto& frame : frames.effective_frames) {
    if (frame.effective_row_indices.size() >
        std::numeric_limits<std::size_t>::max() -
            effective_frame_row_reference_count) {
      return refuse(Refusal("SBLR.PLAN_TREE.RESOURCE_LIMIT",
                            "navigation frame reference count overflowed"));
    }
    effective_frame_row_reference_count +=
        frame.effective_row_indices.size();
  }

  CanonicalWindowValueRequest value_request;
  value_request.frames = std::move(frames);
  value_request.function = CanonicalWindowValueFunction::lag;
  value_request.function_uuid = request.function_uuid;
  value_request.value_expression_descriptor_id =
      execution_ordered_input_batch.columns[request.value_column].descriptor_id;
  value_request.result_column = request.result_column;
  value_request.maximum_output_rows =
      execution_ordered_input_batch.rows.empty()
          ? 1
          : execution_ordered_input_batch.rows.size();
  value_request.mga_authority = request.mga_authority;
  auto values = ExecuteCanonicalWindowValue(value_request);
  if (!values.diagnostic.ok) return refuse(std::move(values.diagnostic));
  if (!values.used_implicit_navigation_offset ||
      values.explicit_navigation_default_present ||
      !values.every_function_operand_consumed ||
      !values.partition_metadata_consumed_for_navigation ||
      values.effective_frame_membership_consumed ||
      !values.frame_and_exclusion_validated ||
      !values.frame_and_exclusion_ignored_for_navigation ||
      values.values.size() != execution_ordered_input_batch.rows.size()) {
    return refuse(Refusal(
        "QOW-DIAG-WINDOW-RUNTIME-PAYLOAD",
        "navigation strategy did not consume the exact implicit-offset payload"));
  }

  result.output_batch = std::move(value_request.frames.ordered_batch);
  result.output_batch.columns.push_back(request.result_column);
  for (std::size_t row = 0; row < result.output_batch.rows.size(); ++row) {
    result.output_batch.rows[row].values.push_back(std::move(values.values[row]));
  }
  const auto output_validation = ValidateCanonicalDescriptorBatch(
      result.output_batch, selected_node->output_descriptor_ids);
  if (!output_validation.ok) return refuse(output_validation);
  const auto result_authority = RevalidateCanonicalExecutionMgaAuthority(
      request.mga_authority, execution_dag);
  if (!result_authority.ok) return refuse(result_authority);

  if (pair_count_u64 > std::numeric_limits<std::size_t>::max() ||
      effective_frame_row_reference_count >
          std::numeric_limits<std::uint64_t>::max() / sizeof(std::size_t)) {
    return refuse(Refusal("SBLR.PLAN_TREE.RESOURCE_LIMIT",
                          "navigation workspace accounting overflowed"));
  }
  const auto pair_count = static_cast<std::size_t>(pair_count_u64);
  if (effective_frame_row_reference_count >
      request.maximum_effective_row_references) {
    return refuse(Refusal("SBLR.PLAN_TREE.RESOURCE_LIMIT",
                          "navigation frame reference bound was exceeded"));
  }

  result.diagnostic = {};
  result.partition_order_comparison_count = pair_count;
  result.effective_frame_row_reference_count =
      effective_frame_row_reference_count;
  result.peak_auxiliary_workspace_bytes =
      planned_auxiliary_workspace_bytes;
  result.selected_plan_uuid = execution_dag.selected_plan_uuid;
  result.executed_physical_node_id = selected_node->physical_node_id;
  result.causal_counter_id = selected_node->causal_counter_id;
  result.mga_statement_context = request.mga_authority.statement_context;
  return result;
}
}  // namespace

CanonicalDescriptorLagWindowResult ExecuteCanonicalDescriptorLagWindow(
    const CanonicalDescriptorLagWindowRequest& request) {
  return ExecuteCanonicalDescriptorLagWindowBound(
      request, request.physical_dag, request.ordered_input_batch, false);
}

CanonicalDescriptorLagWindowResult ExecuteCanonicalDescriptorLagWindow(
    const CanonicalDescriptorLagWindowRequest& request,
    const TypedPhysicalNodeDag& borrowed_execution_dag,
    const DescriptorBatch& borrowed_ordered_input_batch) {
  return ExecuteCanonicalDescriptorLagWindowBound(
      request, borrowed_execution_dag, borrowed_ordered_input_batch, true);
}

namespace {
CanonicalDescriptorPeerRankingResult ExecuteCanonicalDescriptorPeerRankingBound(
    const CanonicalDescriptorPeerRankingRequest& request,
    const TypedPhysicalNodeDag& execution_dag,
    const DescriptorBatch& execution_ordered_input_batch,
    const bool borrowed_execution_carriers) {
  CanonicalDescriptorPeerRankingResult result;
  const bool cume_dist =
      request.builtin_id == "sb.window.cume_dist" ||
      request.function_uuid ==
          "019de5fc-2400-721c-be64-2568b64a02b9";
  const bool percent_rank =
      !cume_dist &&
      (request.builtin_id == "sb.window.percent_rank" ||
       request.function_uuid ==
           "019de5fc-2400-7d86-86fe-96f3f27b5dd6");
  const bool dense_rank =
      !cume_dist && !percent_rank &&
      (request.builtin_id == "sb.window.dense_rank" ||
       request.function_uuid ==
           "019de5fc-2400-741d-bef0-f079fd3ba494");
  const bool real_ranking = percent_rank || cume_dist;
  const std::string_view ranking_name =
      cume_dist
          ? "CUME_DIST"
          : (percent_rank ? "PERCENT_RANK"
                          : (dense_rank ? "DENSE_RANK" : "RANK"));
  const std::string_view expected_implementation =
      cume_dist
          ? "window.cume-dist.v1"
          : (percent_rank
                 ? "window.percent-rank.v1"
                 : (dense_rank ? "window.dense-rank.v1" : "window.rank.v1"));
  const std::string_view expected_builtin =
      cume_dist
          ? "sb.window.cume_dist"
          : (percent_rank
                 ? "sb.window.percent_rank"
                 : (dense_rank ? "sb.window.dense_rank" : "sb.window.rank"));
  const std::string_view expected_function_uuid =
      cume_dist
          ? "019de5fc-2400-721c-be64-2568b64a02b9"
          : (percent_rank
                 ? "019de5fc-2400-7d86-86fe-96f3f27b5dd6"
                 : (dense_rank ? "019de5fc-2400-741d-bef0-f079fd3ba494"
                               : "019de5fc-2400-7b94-870d-0dd789ca70ab"));
  const std::string_view expected_result_type =
      real_ranking ? "real64" : "int64";
  const auto refuse = [&](DescriptorRuntimeDiagnostic diagnostic) {
    result.diagnostic = std::move(diagnostic);
    result.output_batch = {};
    return result;
  };
  if (borrowed_execution_carriers &&
      (!TypedPhysicalNodeDagCarrierIsExactDefault(request.physical_dag) ||
       !DescriptorBatchCarrierIsExactDefault(
           request.ordered_input_batch))) {
    return refuse(Refusal(
        "QOW-DIAG-QRY-007-WINDOW-PHYSICAL-ROUTE-V1",
        std::string(ranking_name) +
            " request carries conflicting owned execution carriers"));
  }
  const auto authority_validation = RevalidateCanonicalExecutionMgaAuthority(
      request.mga_authority, execution_dag);
  if (!authority_validation.ok) return refuse(authority_validation);
  if (request.selected_physical_node_id == 0 ||
      request.selected_physical_node_id !=
          execution_dag.root_physical_node_id) {
    return refuse(Refusal(
        "SBLR.PLAN_TREE.INVALID_HANDLE",
        "selected " + std::string(ranking_name) +
            " window node is not the root"));
  }

  const PhysicalNodeRecord* selected_node = nullptr;
  const PhysicalNodeRecord* input_node = nullptr;
  for (const auto& node : execution_dag.nodes) {
    if (node.physical_node_id == request.selected_physical_node_id) {
      selected_node = &node;
    }
  }
  if (selected_node == nullptr ||
      selected_node->node_kind != PhysicalNodeKind::kWindow ||
      selected_node->implementation_id != expected_implementation ||
      selected_node->input_physical_node_ids.size() != 1) {
    return refuse(Refusal(
        "QOW-DIAG-QRY-007-WINDOW-PHYSICAL-ROUTE-V1",
        std::string(ranking_name) + " requires one selected window node"));
  }
  for (const auto& node : execution_dag.nodes) {
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
      request.function_abi_version != 1 ||
      request.builtin_id != expected_builtin ||
      request.function_uuid != expected_function_uuid ||
      !IsCanonicalUuid(request.function_uuid) ||
      selected_node->executor_capability_abi_version != 1 ||
      !selected_node->engine_capability_validated ||
      !IsCanonicalUuid(selected_node->executor_capability_uuid) ||
      !IsCanonicalUuid(request.order_term_binding_evidence_uuid) ||
      !IsCanonicalUuid(request.deterministic_order_evidence_uuid) ||
      request.order_term_binding_evidence_uuid == ordering_property_uuid ||
      request.order_term_binding_evidence_uuid == *window_property ||
      request.order_term_binding_evidence_uuid == request.function_uuid ||
      request.deterministic_order_evidence_uuid ==
          request.order_term_binding_evidence_uuid ||
      request.deterministic_order_evidence_uuid == ordering_property_uuid ||
      request.deterministic_order_evidence_uuid == *window_property) {
    return refuse(Refusal(
        "QOW-DIAG-QRY-007-WINDOW-ORDER-REQUIRED-V1",
        std::string(ranking_name) +
            " input lacks exact ordering/window property evidence"));
  }
  std::vector<std::uint32_t> output_descriptor_ids =
      input_node->output_descriptor_ids;
  output_descriptor_ids.push_back(request.ranking_column.descriptor_id);
  if (selected_node->output_descriptor_ids != output_descriptor_ids ||
      request.ranking_column.descriptor_id == 0 ||
      request.ranking_column.nullable ||
      request.ranking_column.descriptor.canonical_type_name !=
          expected_result_type ||
      request.order_term.column >=
          execution_ordered_input_batch.columns.size()) {
    return refuse(Refusal(
        "SBLR.PLAN_TREE.INVALID_HANDLE",
        std::string(ranking_name) +
            " output or order descriptor is not bound"));
  }
  const auto order_validation = ValidateCanonicalDescriptorOrderTerm(
      request.order_term,
      execution_ordered_input_batch.columns[request.order_term.column]);
  if (!order_validation.ok) return refuse(order_validation);
  auto input_validation = ValidateCanonicalDescriptorBatch(
      execution_ordered_input_batch, input_node->output_descriptor_ids);
  if (!input_validation.ok) return refuse(std::move(input_validation));
  if (execution_ordered_input_batch.rows.size() >
      static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max())) {
    return refuse(Refusal(
        "QOW-DIAG-QRY-007-WINDOW-OVERFLOW-V1",
        std::string(ranking_name) +
            " exceeds its supported partition cardinality"));
  }

  const auto peer_comparison_count =
      execution_ordered_input_batch.rows.empty()
          ? std::size_t{0}
          : execution_ordered_input_batch.rows.size() - 1;
  if (request.maximum_peer_comparisons == 0 ||
      peer_comparison_count > request.maximum_peer_comparisons) {
    return refuse(Refusal(
        "SBLR.PLAN_TREE.RESOURCE_LIMIT",
        std::string(ranking_name) +
            " adjacent-peer comparison bound was exceeded"));
  }
  std::uint64_t peak_comparison_workspace_bytes = 0;
  for (std::size_t row = 1;
       row < execution_ordered_input_batch.rows.size(); ++row) {
    const auto& left = execution_ordered_input_batch.rows[row - 1]
                           .values[request.order_term.column];
    const auto& right = execution_ordered_input_batch.rows[row]
                            .values[request.order_term.column];
    const auto left_plan =
        PlanCanonicalDescriptorEqualityKey(left, request.order_term);
    const auto right_plan =
        PlanCanonicalDescriptorEqualityKey(right, request.order_term);
    std::uint64_t pair_workspace = 0;
    if (!left_plan.diagnostic.ok || !right_plan.diagnostic.ok ||
        left_plan.peak_workspace_bytes >
            std::numeric_limits<std::uint64_t>::max() ||
        right_plan.peak_workspace_bytes >
            std::numeric_limits<std::uint64_t>::max() ||
        left_plan.peak_workspace_bytes >
            std::numeric_limits<std::uint64_t>::max() -
                right_plan.peak_workspace_bytes) {
      return refuse(Refusal(
          "SBLR.PLAN_TREE.RESOURCE_LIMIT",
          std::string(ranking_name) +
              " comparison workspace bound overflowed or was refused"));
    }
    pair_workspace =
        static_cast<std::uint64_t>(left_plan.peak_workspace_bytes) +
        static_cast<std::uint64_t>(right_plan.peak_workspace_bytes);
    peak_comparison_workspace_bytes =
        std::max(peak_comparison_workspace_bytes, pair_workspace);
  }

  std::uint64_t binding_receipt_workspace_bytes = 0;
  if (!PlanCanonicalDescriptorOrderTermBindingEvidenceWorkspace(
          request.order_term, ordering_property_uuid,
          &binding_receipt_workspace_bytes)) {
    return refuse(Refusal(
        "SBLR.PLAN_TREE.RESOURCE_LIMIT",
        std::string(ranking_name) +
            " order-term binding receipt workspace was refused"));
  }
  const auto peak_auxiliary_workspace_bytes = std::max(
      std::max(peak_comparison_workspace_bytes,
               binding_receipt_workspace_bytes),
      real_ranking ? kRealRankingConversionWorkspaceMaximumBytes
                   : std::uint64_t{0});
  std::uint64_t input_payload_bytes = 0;
  std::uint64_t rank_payload_bytes = 0;
  if (!RowNumberBatchPayloadBytes(execution_ordered_input_batch,
                                  &input_payload_bytes) ||
      !(real_ranking
            ? RealRankingEncodedPayloadBytes(
                  execution_ordered_input_batch.rows.size(),
                  &rank_payload_bytes)
            : RowNumberEncodedPayloadBytes(
                  execution_ordered_input_batch.rows.size(),
                  &rank_payload_bytes)) ||
      selected_node->memory_bytes_required == 0 ||
      selected_node->memory_bytes_required > execution_dag.memory_budget_bytes ||
      selected_node->memory_bytes_required >
          static_cast<std::uint64_t>(
              std::numeric_limits<std::size_t>::max())) {
    return refuse(Refusal(
        "SBLR.PLAN_TREE.RESOURCE_LIMIT",
        std::string(ranking_name) +
            " memory grant or runtime payload accounting is invalid"));
  }
  auto remaining_memory_bytes = selected_node->memory_bytes_required;
  const auto charge = [&](const std::uint64_t bytes) {
    if (bytes > remaining_memory_bytes) return false;
    remaining_memory_bytes -= bytes;
    return true;
  };
  if (!charge(input_payload_bytes) || !charge(input_payload_bytes) ||
      !charge(rank_payload_bytes) ||
      !charge(peak_auxiliary_workspace_bytes)) {
    return refuse(Refusal(
        "SBLR.PLAN_TREE.RESOURCE_LIMIT",
        std::string(ranking_name) +
            " runtime materialization exceeds its selected node grant"));
  }
  std::uint64_t actual_binding_receipt_workspace_bytes = 0;
  const auto expected_order_term_binding_evidence_uuid =
      ComputeCanonicalDescriptorOrderTermBindingEvidenceUuid(
          request.order_term, ordering_property_uuid,
          binding_receipt_workspace_bytes,
          &actual_binding_receipt_workspace_bytes);
  if (expected_order_term_binding_evidence_uuid.empty() ||
      actual_binding_receipt_workspace_bytes !=
          binding_receipt_workspace_bytes ||
      selected_node->executor_capability_uuid !=
          expected_order_term_binding_evidence_uuid ||
      request.order_term_binding_evidence_uuid !=
          selected_node->executor_capability_uuid) {
    return refuse(Refusal(
        "QOW-DIAG-QRY-007-WINDOW-ORDER-REQUIRED-V1",
        std::string(ranking_name) +
            " order term differs from optimizer-published capability "
            "evidence"));
  }

  result.output_batch.columns = execution_ordered_input_batch.columns;
  result.output_batch.columns.push_back(request.ranking_column);
  result.output_batch.rows = execution_ordered_input_batch.rows;
  if (cume_dist) {
    std::size_t peer_begin = 0;
    for (std::size_t row = 1; row <= result.output_batch.rows.size(); ++row) {
      bool peer_complete = row == result.output_batch.rows.size();
      if (!peer_complete) {
        const auto compared = CompareCanonicalDescriptorOrderValues(
            execution_ordered_input_batch.rows[row - 1]
                .values[request.order_term.column],
            execution_ordered_input_batch.rows[row]
                .values[request.order_term.column],
            request.order_term);
        if (!compared.diagnostic.ok) return refuse(compared.diagnostic);
        if (compared.comparison > 0) {
          return refuse(Refusal(
              "QOW-DIAG-QRY-007-WINDOW-ORDER-REQUIRED-V1",
              std::string(ranking_name) +
                  " input is not ordered by its canonical term"));
        }
        peer_complete = compared.comparison != 0;
      }
      if (!peer_complete) continue;
      for (std::size_t peer_row = peer_begin; peer_row < row; ++peer_row) {
        CanonicalWindowCumeDistValueRequest rank_request;
        rank_request.function_abi_version = request.function_abi_version;
        rank_request.builtin_id = request.builtin_id;
        rank_request.function_uuid = request.function_uuid;
        rank_request.output_descriptor = request.ranking_column.descriptor;
        rank_request.cumulative_row_count = row;
        rank_request.partition_row_count = result.output_batch.rows.size();
        auto rank = ComputeCanonicalWindowCumeDistValue(rank_request);
        if (!rank.diagnostic.ok) return refuse(std::move(rank.diagnostic));
        result.output_batch.rows[peer_row].values.push_back(
            std::move(rank.value));
      }
      peer_begin = row;
    }
  } else {
    std::size_t current_rank = 1;
    for (std::size_t row = 0; row < result.output_batch.rows.size(); ++row) {
      if (row != 0) {
        const auto compared = CompareCanonicalDescriptorOrderValues(
            execution_ordered_input_batch.rows[row - 1]
                .values[request.order_term.column],
            execution_ordered_input_batch.rows[row]
                .values[request.order_term.column],
            request.order_term);
        if (!compared.diagnostic.ok) return refuse(compared.diagnostic);
        if (compared.comparison > 0) {
          return refuse(Refusal(
              "QOW-DIAG-QRY-007-WINDOW-ORDER-REQUIRED-V1",
              std::string(ranking_name) +
                  " input is not ordered by its canonical term"));
        }
        if (compared.comparison != 0) {
          current_rank = dense_rank ? current_rank + 1 : row + 1;
        }
      }
      internal_api::EngineTypedValue value;
      if (percent_rank) {
        CanonicalWindowPercentRankValueRequest rank_request;
        rank_request.function_abi_version = request.function_abi_version;
        rank_request.builtin_id = request.builtin_id;
        rank_request.function_uuid = request.function_uuid;
        rank_request.output_descriptor = request.ranking_column.descriptor;
        rank_request.one_based_rank = current_rank;
        rank_request.partition_row_count = result.output_batch.rows.size();
        auto rank = ComputeCanonicalWindowPercentRankValue(rank_request);
        if (!rank.diagnostic.ok) return refuse(std::move(rank.diagnostic));
        value = std::move(rank.value);
      } else {
        CanonicalWindowIntegerRankValueRequest rank_request;
        rank_request.function_abi_version = request.function_abi_version;
        rank_request.builtin_id = request.builtin_id;
        rank_request.function_uuid = request.function_uuid;
        rank_request.output_descriptor = request.ranking_column.descriptor;
        rank_request.one_based_rank = current_rank;
        auto rank = ComputeCanonicalWindowIntegerRankValue(rank_request);
        if (!rank.diagnostic.ok) return refuse(std::move(rank.diagnostic));
        value = std::move(rank.value);
      }
      result.output_batch.rows[row].values.push_back(std::move(value));
    }
  }
  auto output_validation = ValidateCanonicalDescriptorBatch(
      result.output_batch, selected_node->output_descriptor_ids);
  if (!output_validation.ok) return refuse(std::move(output_validation));
  const auto result_authority = RevalidateCanonicalExecutionMgaAuthority(
      request.mga_authority, execution_dag);
  if (!result_authority.ok) return refuse(result_authority);

  result.diagnostic = {};
  result.selected_plan_uuid = execution_dag.selected_plan_uuid;
  result.executed_physical_node_id = selected_node->physical_node_id;
  result.causal_counter_id = selected_node->causal_counter_id;
  result.peer_comparison_count = peer_comparison_count;
  result.peak_auxiliary_workspace_bytes =
      peak_auxiliary_workspace_bytes;
  result.mga_statement_context = request.mga_authority.statement_context;
  return result;
}
}  // namespace

CanonicalDescriptorPeerRankingResult ExecuteCanonicalDescriptorPeerRanking(
    const CanonicalDescriptorPeerRankingRequest& request) {
  return ExecuteCanonicalDescriptorPeerRankingBound(
      request, request.physical_dag, request.ordered_input_batch, false);
}

CanonicalDescriptorPeerRankingResult ExecuteCanonicalDescriptorPeerRanking(
    const CanonicalDescriptorPeerRankingRequest& request,
    const TypedPhysicalNodeDag& borrowed_execution_dag,
    const DescriptorBatch& borrowed_ordered_input_batch) {
  return ExecuteCanonicalDescriptorPeerRankingBound(
      request, borrowed_execution_dag, borrowed_ordered_input_batch, true);
}

}  // namespace scratchbird::engine::executor
