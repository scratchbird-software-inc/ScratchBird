// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "descriptor_value_runtime.hpp"

#include <algorithm>
#include <limits>
#include <unordered_set>
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

bool AccountJoinBytes(const std::uint64_t bytes,
                      const std::uint64_t limit,
                      std::uint64_t* total) {
  if (total == nullptr || bytes > limit || *total > limit - bytes) {
    return false;
  }
  *total += bytes;
  return true;
}

bool AccountJoinString(const std::string& value,
                       const std::uint64_t limit,
                       std::uint64_t* total) {
  return AccountJoinBytes(static_cast<std::uint64_t>(value.size()), limit,
                          total);
}

bool AccountJoinDescriptor(const internal_api::EngineDescriptor& descriptor,
                           const std::uint64_t limit,
                           std::uint64_t* total) {
  return AccountJoinString(descriptor.descriptor_uuid.canonical, limit,
                           total) &&
         AccountJoinString(descriptor.descriptor_kind, limit, total) &&
         AccountJoinString(descriptor.canonical_type_name, limit, total) &&
         AccountJoinString(descriptor.encoded_descriptor, limit, total);
}

bool AccountJoinValue(const internal_api::EngineTypedValue& value,
                      const std::uint64_t limit,
                      std::uint64_t* total) {
  return AccountJoinBytes(sizeof(internal_api::EngineTypedValue), limit,
                          total) &&
         AccountJoinDescriptor(value.descriptor, limit, total) &&
         AccountJoinString(value.encoded_value, limit, total) &&
         AccountJoinBytes(static_cast<std::uint64_t>(value.binary_value.size()),
                          limit, total);
}

bool CanonicalJoinUuid(const std::string_view value) {
  if (value.size() != 36 || value[8] != '-' || value[13] != '-' ||
      value[18] != '-' || value[23] != '-') {
    return false;
  }
  for (std::size_t index = 0; index < value.size(); ++index) {
    if (index == 8 || index == 13 || index == 18 || index == 23) continue;
    const auto ch = value[index];
    if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'))) {
      return false;
    }
  }
  return true;
}

std::optional<std::string_view> CanonicalJoinDescriptorField(
    const internal_api::EngineDescriptor& descriptor,
    const std::string_view key) {
  const auto prefix = std::string(key) + "=";
  std::optional<std::string_view> value;
  std::size_t begin = 0;
  while (begin <= descriptor.encoded_descriptor.size()) {
    const auto end = descriptor.encoded_descriptor.find(';', begin);
    const auto field =
        std::string_view(descriptor.encoded_descriptor)
            .substr(begin, end == std::string::npos ? std::string::npos
                                                    : end - begin);
    if (field.starts_with(prefix)) {
      if (value.has_value() || field.size() == prefix.size()) {
        return std::nullopt;
      }
      value = field.substr(prefix.size());
    }
    if (end == std::string::npos) break;
    begin = end + 1;
  }
  return value;
}

}  // namespace

DescriptorRuntimeDiagnostic ValidateCanonicalJoinDescriptorRoleDomains(
    const DescriptorBatch& left_batch,
    const DescriptorBatch& right_batch) {
  std::unordered_set<std::string_view> descriptor_uuids;
  std::unordered_set<std::string_view> type_uuids;
  const auto collect = [&](const DescriptorBatch& batch) {
    for (std::size_t column = 0; column < batch.columns.size(); ++column) {
      const auto& descriptor = batch.columns[column].descriptor;
      const auto type_uuid =
          CanonicalJoinDescriptorField(descriptor, "type_uuid");
      if (!internal_api::QowCanonicalDescriptorIdentityV1(descriptor) ||
          !type_uuid.has_value() || !CanonicalJoinUuid(*type_uuid)) {
        auto diagnostic = Refusal(
            "SBLR.PLAN_TREE.INVALID_HANDLE",
            "join descriptor or type identity is unresolved");
        diagnostic.column_index = column;
        return diagnostic;
      }
      descriptor_uuids.insert(descriptor.descriptor_uuid.canonical);
      type_uuids.insert(*type_uuid);
    }
    return DescriptorRuntimeDiagnostic{};
  };
  auto validation = collect(left_batch);
  if (!validation.ok) return validation;
  validation = collect(right_batch);
  if (!validation.ok) return validation;
  for (const auto descriptor_uuid : descriptor_uuids) {
    if (type_uuids.contains(descriptor_uuid)) {
      return Refusal(
          "SBLR.PLAN_TREE.INVALID_HANDLE",
          "join descriptor and type identity domains are not independent");
    }
  }
  return {};
}

// QOW-SOURCE-QRY-007-JOIN-V1
// First canonical implementation in this module: typed inner join over the
// bound row-pair ON truth vector.  Pair evaluation is shared QRY-017 3VL;
// this physical node constructs rows only for TRUE and never equates UNKNOWN.
CanonicalDescriptorInnerJoinResult ExecuteCanonicalDescriptorInnerJoin(
    const CanonicalDescriptorInnerJoinRequest& request) {
  using scratchbird::engine::internal_api::EnginePredicateConsumer;
  using scratchbird::engine::internal_api::QowPredicateConsumerPassesV1;

  CanonicalDescriptorInnerJoinResult result;
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
                          "selected join node is not the root"));
  }

  const PhysicalNodeRecord* selected_node = nullptr;
  const PhysicalNodeRecord* left_node = nullptr;
  const PhysicalNodeRecord* right_node = nullptr;
  for (const auto& node : request.physical_dag.nodes) {
    if (node.physical_node_id == request.selected_physical_node_id) {
      selected_node = &node;
    }
  }
  if (selected_node == nullptr ||
      selected_node->node_kind != PhysicalNodeKind::kJoin ||
      selected_node->input_physical_node_ids.size() != 2 ||
      selected_node->input_physical_node_ids[0] ==
          selected_node->input_physical_node_ids[1]) {
    return refuse(Refusal("QOW-DIAG-QRY-007-JOIN-PHYSICAL-ROUTE-V1",
                          "inner join requires one selected node with two "
                          "distinct physical inputs"));
  }
  for (const auto& node : request.physical_dag.nodes) {
    if (node.physical_node_id ==
        selected_node->input_physical_node_ids[0]) {
      left_node = &node;
    }
    if (node.physical_node_id ==
        selected_node->input_physical_node_ids[1]) {
      right_node = &node;
    }
  }
  if (left_node == nullptr || right_node == nullptr) {
    return refuse(Refusal("SBLR.PLAN_TREE.INVALID_HANDLE",
                          "join input node is unresolved"));
  }
  if (left_node->output_descriptor_ids.size() >
      std::numeric_limits<std::size_t>::max() -
          right_node->output_descriptor_ids.size()) {
    return refuse(Refusal("SBLR.PLAN_TREE.RESOURCE_LIMIT",
                          "join output descriptor width overflows"));
  }
  const auto output_width = left_node->output_descriptor_ids.size() +
                            right_node->output_descriptor_ids.size();
  if (selected_node->output_descriptor_ids.size() != output_width ||
      !std::equal(left_node->output_descriptor_ids.begin(),
                  left_node->output_descriptor_ids.end(),
                  selected_node->output_descriptor_ids.begin()) ||
      !std::equal(right_node->output_descriptor_ids.begin(),
                  right_node->output_descriptor_ids.end(),
                  selected_node->output_descriptor_ids.begin() +
                      left_node->output_descriptor_ids.size())) {
    return refuse(Refusal("SBLR.PLAN_TREE.INVALID_HANDLE",
                          "join output handles do not concatenate inputs"));
  }
  auto left_validation = ValidateCanonicalDescriptorBatch(
      request.left_batch, left_node->output_descriptor_ids);
  if (!left_validation.ok) return refuse(std::move(left_validation));
  auto right_validation = ValidateCanonicalDescriptorBatch(
      request.right_batch, right_node->output_descriptor_ids);
  if (!right_validation.ok) return refuse(std::move(right_validation));
  auto role_validation = ValidateCanonicalJoinDescriptorRoleDomains(
      request.left_batch, request.right_batch);
  if (!role_validation.ok) return refuse(std::move(role_validation));
  if (request.consumer != EnginePredicateConsumer::join_on) {
    return refuse(Refusal("QOW-DIAG-QRY-017-3VL-REFUSAL-V1",
                          "join predicate consumer is not bound"));
  }
  const auto left_count = request.left_batch.rows.size();
  const auto right_count = request.right_batch.rows.size();
  if (left_count != 0 &&
      right_count > std::numeric_limits<std::size_t>::max() / left_count) {
    return refuse(Refusal("SBLR.PLAN_TREE.RESOURCE_LIMIT",
                          "join predicate cardinality overflows"));
  }
  const auto pair_count = left_count * right_count;
  if (request.pair_truth_values.size() != pair_count) {
    return refuse(Refusal("QOW-DIAG-QRY-017-3VL-REFUSAL-V1",
                          "join predicate cardinality is not bound"));
  }

  if (request.maximum_output_rows == 0 ||
      request.maximum_output_cells == 0 ||
      request.physical_dag.memory_budget_bytes == 0) {
    return refuse(Refusal("SBLR.PLAN_TREE.RESOURCE_LIMIT",
                          "join output resource contract is not bound"));
  }
  std::size_t output_row_count = 0;
  for (std::size_t pair = 0; pair < pair_count; ++pair) {
    bool passes = false;
    std::string refusal_detail;
    if (!QowPredicateConsumerPassesV1(request.pair_truth_values[pair],
                                      request.consumer, &passes,
                                      &refusal_detail)) {
      return refuse(Refusal("QOW-DIAG-QRY-017-3VL-REFUSAL-V1",
                            std::move(refusal_detail), pair));
    }
    if (passes) {
      if (output_row_count == request.maximum_output_rows) {
        return refuse(Refusal("SBLR.PLAN_TREE.RESOURCE_LIMIT",
                              "join output row bound was exceeded", pair));
      }
      ++output_row_count;
    }
  }
  if (output_width != 0 &&
      output_row_count > request.maximum_output_cells / output_width) {
    return refuse(Refusal("SBLR.PLAN_TREE.RESOURCE_LIMIT",
                          "join output cell bound was exceeded"));
  }

  const auto memory_limit = request.physical_dag.memory_budget_bytes;
  std::uint64_t output_memory = sizeof(DescriptorBatch);
  for (const auto& column : request.left_batch.columns) {
    if (!AccountJoinBytes(sizeof(ExecutorColumnDescriptor), memory_limit,
                          &output_memory) ||
        !AccountJoinString(column.stable_name, memory_limit, &output_memory) ||
        !AccountJoinDescriptor(column.descriptor, memory_limit,
                               &output_memory)) {
      return refuse(Refusal("SBLR.PLAN_TREE.RESOURCE_LIMIT",
                            "join output descriptor memory bound was exceeded"));
    }
  }
  for (const auto& column : request.right_batch.columns) {
    if (!AccountJoinBytes(sizeof(ExecutorColumnDescriptor), memory_limit,
                          &output_memory) ||
        !AccountJoinString(column.stable_name, memory_limit, &output_memory) ||
        !AccountJoinDescriptor(column.descriptor, memory_limit,
                               &output_memory)) {
      return refuse(Refusal("SBLR.PLAN_TREE.RESOURCE_LIMIT",
                            "join output descriptor memory bound was exceeded"));
    }
  }
  for (std::size_t left = 0; left < left_count; ++left) {
    for (std::size_t right = 0; right < right_count; ++right) {
      const auto pair = left * right_count + right;
      bool passes = false;
      std::string refusal_detail;
      if (!QowPredicateConsumerPassesV1(request.pair_truth_values[pair],
                                        request.consumer, &passes,
                                        &refusal_detail)) {
        return refuse(Refusal("QOW-DIAG-QRY-017-3VL-REFUSAL-V1",
                              std::move(refusal_detail), pair));
      }
      if (!passes) continue;
      if (!AccountJoinBytes(sizeof(DescriptorTuple), memory_limit,
                            &output_memory)) {
        return refuse(Refusal("SBLR.PLAN_TREE.RESOURCE_LIMIT",
                              "join output tuple memory bound was exceeded",
                              pair));
      }
      for (const auto& value : request.left_batch.rows[left].values) {
        if (!AccountJoinValue(value, memory_limit, &output_memory)) {
          return refuse(Refusal("SBLR.PLAN_TREE.RESOURCE_LIMIT",
                                "join output value memory bound was exceeded",
                                pair));
        }
      }
      for (const auto& value : request.right_batch.rows[right].values) {
        if (!AccountJoinValue(value, memory_limit, &output_memory)) {
          return refuse(Refusal("SBLR.PLAN_TREE.RESOURCE_LIMIT",
                                "join output value memory bound was exceeded",
                                pair));
        }
      }
    }
  }

  result.output_batch.columns = request.left_batch.columns;
  result.output_batch.columns.insert(result.output_batch.columns.end(),
                                     request.right_batch.columns.begin(),
                                     request.right_batch.columns.end());
  result.output_batch.rows.reserve(output_row_count);
  for (std::size_t left = 0; left < left_count; ++left) {
    for (std::size_t right = 0; right < right_count; ++right) {
      const auto pair = left * right_count + right;
      bool passes = false;
      std::string refusal_detail;
      if (!QowPredicateConsumerPassesV1(request.pair_truth_values[pair],
                                        request.consumer, &passes,
                                        &refusal_detail)) {
        return refuse(Refusal("QOW-DIAG-QRY-017-3VL-REFUSAL-V1",
                              std::move(refusal_detail), pair));
      }
      if (!passes) continue;
      DescriptorTuple joined;
      joined.values = request.left_batch.rows[left].values;
      joined.values.insert(joined.values.end(),
                           request.right_batch.rows[right].values.begin(),
                           request.right_batch.rows[right].values.end());
      result.output_batch.rows.push_back(std::move(joined));
    }
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
