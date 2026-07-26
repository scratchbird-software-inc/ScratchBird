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
                                    std::string detail = {}) {
  DescriptorRuntimeDiagnostic diagnostic;
  diagnostic.ok = false;
  diagnostic.diagnostic_code = std::move(code);
  diagnostic.detail = std::move(detail);
  return diagnostic;
}

}  // namespace

// QOW-SOURCE-QRY-007-SORT-LIMIT-V1
// First canonical implementation in this module: a typed LIMIT/OFFSET node.
// Typed ORDER BY terms are deliberately left to QRY-010; this entry does not
// fall back to the legacy one-integer-key sorter.
CanonicalDescriptorLimitResult ExecuteCanonicalDescriptorLimit(
    const CanonicalDescriptorLimitRequest& request) {
  CanonicalDescriptorLimitResult result;
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
                          "selected limit node is not the root"));
  }

  const PhysicalNodeRecord* selected_node = nullptr;
  const PhysicalNodeRecord* input_node = nullptr;
  for (const auto& node : request.physical_dag.nodes) {
    if (node.physical_node_id == request.selected_physical_node_id) {
      selected_node = &node;
    }
  }
  if (selected_node == nullptr ||
      selected_node->node_kind != PhysicalNodeKind::kLimit ||
      selected_node->input_physical_node_ids.size() != 1) {
    return refuse(Refusal(
        "QOW-DIAG-QRY-007-SORT-LIMIT-PHYSICAL-ROUTE-V1",
        "descriptor limit requires one selected limit node"));
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
    return refuse(Refusal("SBLR.PLAN_TREE.INVALID_HANDLE",
                          "limit schema does not preserve input handles"));
  }
  auto input_validation = ValidateCanonicalDescriptorBatch(
      request.input_batch, input_node->output_descriptor_ids);
  if (!input_validation.ok) return refuse(std::move(input_validation));

  result.output_batch.columns = request.input_batch.columns;
  const auto row_count = request.input_batch.rows.size();
  const auto offset = request.offset > row_count
                          ? row_count
                          : static_cast<std::size_t>(request.offset);
  const auto remaining = row_count - offset;
  const auto take = request.limit > remaining
                        ? remaining
                        : static_cast<std::size_t>(request.limit);
  result.output_batch.rows.reserve(take);
  result.output_batch.rows.insert(
      result.output_batch.rows.end(),
      request.input_batch.rows.begin() + static_cast<std::ptrdiff_t>(offset),
      request.input_batch.rows.begin() +
          static_cast<std::ptrdiff_t>(offset + take));

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
