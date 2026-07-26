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

#include <algorithm>
#include <exception>
#include <memory>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace scratchbird::engine::executor {
namespace {

DescriptorRuntimeDiagnostic RecursiveCteWorkingRefusal(std::string detail) {
  DescriptorRuntimeDiagnostic diagnostic;
  diagnostic.ok = false;
  diagnostic.diagnostic_code =
      "QOW-DIAG-QRY-014-WORKING-REFUSAL-V1";
  diagnostic.detail = std::move(detail);
  return diagnostic;
}

const PhysicalNodeRecord* FindPhysicalNode(const TypedPhysicalNodeDag& dag,
                                           const std::uint64_t node_id) {
  for (const auto& node : dag.nodes) {
    if (node.physical_node_id == node_id) return &node;
  }
  return nullptr;
}

}  // namespace

// QOW-SOURCE-QRY-014-WORKING-V1
// Execute the recursive term against the current working relation, replace the
// working relation with the validated intermediate relation, and stop only
// when that intermediate relation is empty. All intermediate state remains
// local until convergence, so malformed input, resource excess, or a
// non-convergent recursive term cannot publish a partial CTE result.
CanonicalRecursiveCteWorkingResult ExecuteCanonicalRecursiveCteWorking(
    const CanonicalRecursiveCteWorkingRequest& request) {
  CanonicalRecursiveCteWorkingResult result;
  const auto refuse = [&](std::string detail) {
    result.diagnostic = RecursiveCteWorkingRefusal(std::move(detail));
    result.output_batch = {};
    result.iterations.clear();
    result.recursive_iteration_count = 0;
    result.maximum_observed_working_row_count = 0;
    result.converged = false;
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
    return refuse("selected recursive CTE node is not the physical root");
  }

  const auto* selected_node = FindPhysicalNode(
      request.physical_dag, request.selected_physical_node_id);
  if (selected_node == nullptr ||
      selected_node->node_kind != PhysicalNodeKind::kRecursiveCte ||
      selected_node->implementation_id !=
          "cte.recursive.working.typed.v1" ||
      selected_node->input_physical_node_ids.size() != 2) {
    return refuse("recursive CTE working physical profile is not bound");
  }
  const auto* anchor_node = FindPhysicalNode(
      request.physical_dag, selected_node->input_physical_node_ids[0]);
  const auto* recursive_node = FindPhysicalNode(
      request.physical_dag, selected_node->input_physical_node_ids[1]);
  if (anchor_node == nullptr || recursive_node == nullptr ||
      anchor_node->node_kind != PhysicalNodeKind::kValues ||
      recursive_node->node_kind != PhysicalNodeKind::kCte ||
      anchor_node->output_descriptor_ids !=
          recursive_node->output_descriptor_ids ||
      selected_node->output_descriptor_ids !=
          anchor_node->output_descriptor_ids) {
    return refuse("recursive CTE input or output descriptor handles drifted");
  }

  const auto anchor_validation = ValidateCanonicalDescriptorBatch(
      request.anchor_batch, anchor_node->output_descriptor_ids);
  if (!anchor_validation.ok) {
    return refuse(anchor_validation.diagnostic_code + ":" +
                  anchor_validation.detail);
  }
  if (!request.recursive_step || request.maximum_iteration_count == 0 ||
      request.maximum_working_row_count == 0 ||
      request.maximum_result_row_count == 0 ||
      request.anchor_batch.rows.size() >
          request.maximum_working_row_count ||
      request.anchor_batch.rows.size() > request.maximum_result_row_count) {
    return refuse("recursive CTE working resource contract is invalid");
  }

  DescriptorBatch accumulated = request.anchor_batch;
  DescriptorBatch working = request.anchor_batch;
  std::vector<CanonicalRecursiveCteIteration> iterations;
  std::size_t maximum_working = working.rows.size();
  std::size_t iteration_ordinal = 0;
  while (!working.rows.empty()) {
    if (iteration_ordinal == request.maximum_iteration_count) {
      return refuse("recursive CTE did not converge within the iteration bound");
    }
    ++iteration_ordinal;

    DescriptorBatch intermediate;
    try {
      intermediate = request.recursive_step(working, iteration_ordinal);
    } catch (const std::exception& error) {
      return refuse(std::string("recursive CTE step failed:") + error.what());
    } catch (...) {
      return refuse("recursive CTE step failed with an unknown exception");
    }
    const auto intermediate_validation = ValidateCanonicalDescriptorBatch(
        intermediate, recursive_node->output_descriptor_ids);
    if (!intermediate_validation.ok) {
      return refuse(intermediate_validation.diagnostic_code + ":" +
                    intermediate_validation.detail);
    }
    if (intermediate.rows.size() > request.maximum_working_row_count ||
        intermediate.rows.size() >
            request.maximum_result_row_count - accumulated.rows.size()) {
      return refuse("recursive CTE working or result row bound was exceeded");
    }

    iterations.push_back({iteration_ordinal, working.rows.size(),
                          intermediate.rows.size()});
    maximum_working =
        std::max(maximum_working, intermediate.rows.size());
    accumulated.rows.insert(accumulated.rows.end(),
                            intermediate.rows.begin(),
                            intermediate.rows.end());
    working = std::move(intermediate);
  }

  const auto output_validation = ValidateCanonicalDescriptorBatch(
      accumulated, selected_node->output_descriptor_ids);
  if (!output_validation.ok) {
    return refuse(output_validation.diagnostic_code + ":" +
                  output_validation.detail);
  }

  result.diagnostic = {};
  result.output_batch = std::move(accumulated);
  result.iterations = std::move(iterations);
  result.recursive_iteration_count = iteration_ordinal;
  result.maximum_observed_working_row_count = maximum_working;
  result.converged = true;
  result.selected_plan_uuid = request.physical_dag.selected_plan_uuid;
  result.executed_physical_node_id = selected_node->physical_node_id;
  result.causal_counter_id = selected_node->causal_counter_id;
  return result;
}

// QOW-SOURCE-QRY-014-UNION-V1
// Admit either UNION ALL, which preserves every anchor and recursive row, or
// the bounded one-column int64 UNION DISTINCT profile. DISTINCT compares
// decoded typed values (with SQL NULL equal to SQL NULL for set semantics),
// removes duplicates against the complete accumulated result and the current
// intermediate relation, and feeds only newly admitted rows into the next
// recursive working transition.
CanonicalRecursiveCteUnionResult ExecuteCanonicalRecursiveCteUnion(
    const CanonicalRecursiveCteUnionRequest& request) {
  CanonicalRecursiveCteUnionResult result;
  result.union_mode = request.union_mode;
  const auto refuse = [&](std::string detail) {
    result.working_result = {};
    result.working_result.diagnostic.ok = false;
    result.working_result.diagnostic.diagnostic_code =
        "QOW-DIAG-QRY-014-UNION-REFUSAL-V1";
    result.working_result.diagnostic.detail = std::move(detail);
    result.union_mode = request.union_mode;
    result.duplicate_row_count = 0;
    return result;
  };

  if (request.union_mode != CanonicalRecursiveCteUnionMode::kAll &&
      request.union_mode != CanonicalRecursiveCteUnionMode::kDistinct) {
    return refuse("recursive CTE UNION mode is not bound");
  }
  const auto dag_validation =
      ValidateTypedPhysicalNodeDag(request.working_request.physical_dag);
  if (!dag_validation.accepted) {
    const auto& issue = dag_validation.issues.front();
    return refuse(issue.diagnostic_id + ":" + issue.field_id);
  }
  const auto* selected_node = FindPhysicalNode(
      request.working_request.physical_dag,
      request.working_request.selected_physical_node_id);
  const auto expected_profile =
      request.union_mode == CanonicalRecursiveCteUnionMode::kAll
          ? "cte.recursive.union-all.typed.v1"
          : "cte.recursive.union-distinct-int64.typed.v1";
  if (request.working_request.selected_physical_node_id == 0 ||
      request.working_request.selected_physical_node_id !=
          request.working_request.physical_dag.root_physical_node_id ||
      selected_node == nullptr ||
      selected_node->node_kind != PhysicalNodeKind::kRecursiveCte ||
      selected_node->implementation_id != expected_profile) {
    return refuse("recursive CTE UNION physical profile is not bound");
  }

  CanonicalRecursiveCteWorkingRequest working = request.working_request;
  for (auto& node : working.physical_dag.nodes) {
    if (node.physical_node_id == working.selected_physical_node_id) {
      node.implementation_id = "cte.recursive.working.typed.v1";
      break;
    }
  }

  auto duplicate_count = std::make_shared<std::size_t>(0);
  if (request.union_mode == CanonicalRecursiveCteUnionMode::kDistinct) {
    if (working.anchor_batch.columns.size() != 1 ||
        working.anchor_batch.columns.front().descriptor.canonical_type_name !=
            "int64") {
      return refuse("recursive CTE UNION DISTINCT requires one int64 column");
    }

    const auto row_key = [](const DescriptorTuple& row) {
      if (row.values.size() != 1) {
        throw std::runtime_error("recursive UNION DISTINCT row is ragged");
      }
      const auto& value = row.values.front();
      if (value.state ==
          scratchbird::engine::internal_api::EngineValueState::sql_null) {
        return std::string("null");
      }
      const auto decoded = DecodeInt64Value(value);
      if (!decoded.ok()) {
        throw std::runtime_error(decoded.diagnostic.diagnostic_code + ":" +
                                 decoded.diagnostic.detail);
      }
      return std::string("int64:") + std::to_string(decoded.value);
    };

    auto seen = std::make_shared<std::unordered_set<std::string>>();
    DescriptorBatch distinct_anchor;
    distinct_anchor.columns = working.anchor_batch.columns;
    try {
      for (const auto& row : working.anchor_batch.rows) {
        if (seen->insert(row_key(row)).second) {
          distinct_anchor.rows.push_back(row);
        } else {
          ++*duplicate_count;
        }
      }
    } catch (const std::exception& error) {
      return refuse(error.what());
    }
    working.anchor_batch = std::move(distinct_anchor);

    const auto recursive_step = working.recursive_step;
    working.recursive_step =
        [recursive_step, seen, duplicate_count, row_key](
            const DescriptorBatch& current, const std::size_t iteration) {
          auto generated = recursive_step(current, iteration);
          DescriptorBatch distinct;
          distinct.columns = generated.columns;
          for (const auto& row : generated.rows) {
            if (seen->insert(row_key(row)).second) {
              distinct.rows.push_back(row);
            } else {
              ++*duplicate_count;
            }
          }
          return distinct;
        };
  }

  auto working_result = ExecuteCanonicalRecursiveCteWorking(working);
  if (!working_result.diagnostic.ok) {
    return refuse(working_result.diagnostic.diagnostic_code + ":" +
                  working_result.diagnostic.detail);
  }
  result.working_result = std::move(working_result);
  result.union_mode = request.union_mode;
  result.duplicate_row_count = *duplicate_count;
  return result;
}

}  // namespace scratchbird::engine::executor
