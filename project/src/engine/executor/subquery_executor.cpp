// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

//
// SBSQL bounded source-layout anchor. Runtime behavior for this family is
// implemented by the active dispatcher, executor, planner, or function modules
// linked beside this translation unit and covered by the corresponding proof
// gates. Keep family-specific growth in this bounded area or in the named
// shared runtime module, not in broad catch-all files.

#include "descriptor_value_runtime.hpp"

#include <utility>

namespace scratchbird::engine::executor {
namespace {

DescriptorRuntimeDiagnostic TableSubqueryRefusal(std::string detail) {
  DescriptorRuntimeDiagnostic diagnostic;
  diagnostic.ok = false;
  diagnostic.diagnostic_code =
      "QOW-DIAG-QRY-013-TABLE-REFUSAL-V1";
  diagnostic.detail = std::move(detail);
  return diagnostic;
}

}  // namespace

// QOW-SOURCE-QRY-013-TABLE-V1
// Materialize one table-subquery result through the selected canonical
// relational node. The engine-owned physical DAG supplies MGA statement
// context and immutable descriptor handles; parser or donor syntax is never
// consulted here. Validation and the resource bound complete before any
// result batch is published.
CanonicalTableSubqueryResult ExecuteCanonicalTableSubquery(
    const CanonicalTableSubqueryRequest& request) {
  CanonicalTableSubqueryResult result;
  const auto refuse = [&](std::string detail) {
    result.diagnostic = TableSubqueryRefusal(std::move(detail));
    result.output_batch = {};
    result.materialized_row_count = 0;
    result.selected_plan_uuid.clear();
    result.executed_physical_node_id = 0;
    result.causal_counter_id = 0;
    return result;
  };

  const auto dag_validation =
      ValidateTypedPhysicalNodeDag(request.physical_dag);
  if (!dag_validation.accepted) {
    const auto& issue = dag_validation.issues.front();
    return refuse(issue.diagnostic_id + ":" + issue.field_id);
  }
  if (request.selected_physical_node_id == 0 ||
      request.selected_physical_node_id !=
          request.physical_dag.root_physical_node_id) {
    return refuse("selected table-subquery node is not the physical root");
  }

  const PhysicalNodeRecord* selected_node = nullptr;
  for (const auto& node : request.physical_dag.nodes) {
    if (node.physical_node_id == request.selected_physical_node_id) {
      selected_node = &node;
      break;
    }
  }
  if (selected_node == nullptr ||
      selected_node->node_kind != PhysicalNodeKind::kSubquery ||
      selected_node->input_physical_node_ids.size() != 1) {
    return refuse("table subquery requires one selected subquery node");
  }

  const auto input_id = selected_node->input_physical_node_ids.front();
  const PhysicalNodeRecord* input_node = nullptr;
  for (const auto& node : request.physical_dag.nodes) {
    if (node.physical_node_id == input_id) {
      input_node = &node;
      break;
    }
  }
  if (input_node == nullptr) {
    return refuse("table-subquery relational input is unresolved");
  }
  if (selected_node->output_descriptor_ids !=
      input_node->output_descriptor_ids) {
    return refuse("table-subquery output descriptor handles drifted");
  }

  auto input_validation = ValidateCanonicalDescriptorBatch(
      request.input_batch, input_node->output_descriptor_ids);
  if (!input_validation.ok) {
    return refuse(input_validation.diagnostic_code + ":" +
                  input_validation.detail);
  }
  if (request.maximum_materialized_row_count == 0 ||
      request.input_batch.rows.size() >
          request.maximum_materialized_row_count) {
    return refuse("table-subquery materialization row bound was exceeded");
  }

  DescriptorBatch materialized = request.input_batch;
  auto output_validation = ValidateCanonicalDescriptorBatch(
      materialized, selected_node->output_descriptor_ids);
  if (!output_validation.ok) {
    return refuse(output_validation.diagnostic_code + ":" +
                  output_validation.detail);
  }

  result.diagnostic = {};
  result.output_batch = std::move(materialized);
  result.materialized_row_count = result.output_batch.rows.size();
  result.selected_plan_uuid = request.physical_dag.selected_plan_uuid;
  result.executed_physical_node_id = selected_node->physical_node_id;
  result.causal_counter_id = selected_node->causal_counter_id;
  return result;
}

}  // namespace scratchbird::engine::executor
