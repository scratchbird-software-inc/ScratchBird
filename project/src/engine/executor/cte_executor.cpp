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
#include <cctype>
#include <exception>
#include <limits>
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

bool IsCanonicalCteEvidenceUuid(const std::string_view value) {
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

// QOW-SOURCE-QRY-014-SEARCH-CYCLE-V1
// Execute the accepted breadth-first SEARCH profile and one int64 CYCLE key.
// Parent indices bind every generated row to one current working row, allowing
// path-local cycle detection. A cycle row is emitted once with a typed TRUE
// mark and is never placed into the next working relation.
CanonicalRecursiveCteSearchCycleResult
ExecuteCanonicalRecursiveCteSearchCycle(
    const CanonicalRecursiveCteSearchCycleRequest& request) {
  namespace api = scratchbird::engine::internal_api;

  CanonicalRecursiveCteSearchCycleResult result;
  const auto refuse = [&](std::string detail) {
    result = {};
    result.diagnostic.ok = false;
    result.diagnostic.diagnostic_code =
        "QOW-DIAG-QRY-014-SEARCH-CYCLE-REFUSAL-V1";
    result.diagnostic.detail = std::move(detail);
    return result;
  };

  const auto dag_validation =
      ValidateTypedPhysicalNodeDag(request.physical_dag);
  if (!dag_validation.accepted) {
    const auto& issue = dag_validation.issues.front();
    return refuse(issue.diagnostic_id + ":" + issue.field_id);
  }
  const auto* selected_node = FindPhysicalNode(
      request.physical_dag, request.selected_physical_node_id);
  if (request.selected_physical_node_id == 0 ||
      request.selected_physical_node_id !=
          request.physical_dag.root_physical_node_id ||
      selected_node == nullptr ||
      selected_node->node_kind != PhysicalNodeKind::kRecursiveCte ||
      selected_node->implementation_id !=
          "cte.recursive.search-breadth-cycle-int64.typed.v1" ||
      selected_node->input_physical_node_ids.size() != 2) {
    return refuse("recursive CTE SEARCH/CYCLE physical profile is not bound");
  }
  if (request.search_order !=
      CanonicalRecursiveCteSearchOrder::kBreadthFirst) {
    return refuse("recursive CTE SEARCH order is outside the accepted profile");
  }

  const auto* anchor_node = FindPhysicalNode(
      request.physical_dag, selected_node->input_physical_node_ids[0]);
  const auto* recursive_node = FindPhysicalNode(
      request.physical_dag, selected_node->input_physical_node_ids[1]);
  if (anchor_node == nullptr || recursive_node == nullptr ||
      anchor_node->node_kind != PhysicalNodeKind::kValues ||
      recursive_node->node_kind != PhysicalNodeKind::kCte ||
      anchor_node->output_descriptor_ids !=
          recursive_node->output_descriptor_ids) {
    return refuse("recursive CTE SEARCH/CYCLE inputs are not bound");
  }
  std::vector<std::uint32_t> output_descriptor_ids =
      anchor_node->output_descriptor_ids;
  output_descriptor_ids.push_back(
      request.search_sequence_column.descriptor_id);
  output_descriptor_ids.push_back(request.cycle_mark_column.descriptor_id);
  if (selected_node->output_descriptor_ids != output_descriptor_ids ||
      request.cycle_key_column >= request.anchor_batch.columns.size() ||
      request.cycle_key_expression_descriptor_id == 0 ||
      request.anchor_batch.columns[request.cycle_key_column].descriptor_id !=
          request.cycle_key_expression_descriptor_id ||
      request.anchor_batch.columns[request.cycle_key_column]
              .descriptor.canonical_type_name != "int64" ||
      request.search_sequence_column.descriptor_id == 0 ||
      request.search_sequence_column.nullable ||
      request.search_sequence_column.descriptor.canonical_type_name !=
          "int64" ||
      request.cycle_mark_column.descriptor_id == 0 ||
      request.cycle_mark_column.nullable ||
      request.cycle_mark_column.descriptor.canonical_type_name != "boolean") {
    return refuse("recursive CTE SEARCH/CYCLE descriptors are not exact");
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
    return refuse("recursive CTE SEARCH/CYCLE resource contract is invalid");
  }

  const auto key_for_row = [&](const DescriptorTuple& row) {
    if (request.cycle_key_column >= row.values.size()) {
      throw std::runtime_error("recursive CTE cycle-key row is ragged");
    }
    const auto& value = row.values[request.cycle_key_column];
    if (value.state == api::EngineValueState::sql_null) {
      return std::string("null");
    }
    const auto decoded = DecodeInt64Value(value);
    if (!decoded.ok()) {
      throw std::runtime_error(decoded.diagnostic.diagnostic_code + ":" +
                               decoded.diagnostic.detail);
    }
    return std::string("int64:") + std::to_string(decoded.value);
  };
  const auto sequence_value = [&](const std::uint64_t sequence) {
    api::EngineTypedValue value;
    value.descriptor = request.search_sequence_column.descriptor;
    value.encoded_value = std::to_string(sequence);
    value.state = api::EngineValueState::value;
    return value;
  };
  const auto cycle_value = [&](const bool cycle) {
    api::EngineTypedValue value;
    value.descriptor = request.cycle_mark_column.descriptor;
    value.encoded_value = cycle ? "true" : "false";
    value.state = api::EngineValueState::value;
    return value;
  };

  DescriptorBatch output;
  output.columns = request.anchor_batch.columns;
  output.columns.push_back(request.search_sequence_column);
  output.columns.push_back(request.cycle_mark_column);
  DescriptorBatch working = request.anchor_batch;
  std::vector<std::vector<std::string>> working_paths;
  std::vector<CanonicalRecursiveCteSearchCycleMetadata> metadata;
  std::uint64_t sequence = 0;
  try {
    working_paths.reserve(working.rows.size());
    for (const auto& row : working.rows) {
      const auto key = key_for_row(row);
      working_paths.push_back({key});
      DescriptorTuple projected = row;
      projected.values.push_back(sequence_value(++sequence));
      projected.values.push_back(cycle_value(false));
      output.rows.push_back(std::move(projected));
      metadata.push_back(
          {output.rows.size() - 1, 0, sequence, false});
    }
  } catch (const std::exception& error) {
    return refuse(error.what());
  }

  std::size_t iteration = 0;
  std::size_t cycle_rows = 0;
  while (!working.rows.empty()) {
    if (iteration == request.maximum_iteration_count) {
      return refuse("recursive CTE SEARCH/CYCLE did not converge");
    }
    ++iteration;
    CanonicalRecursiveCteGeneratedBatch generated;
    try {
      generated = request.recursive_step(working, iteration);
    } catch (const std::exception& error) {
      return refuse(std::string("recursive CTE SEARCH/CYCLE step failed:") +
                    error.what());
    } catch (...) {
      return refuse("recursive CTE SEARCH/CYCLE step failed");
    }
    const auto generated_validation = ValidateCanonicalDescriptorBatch(
        generated.batch, recursive_node->output_descriptor_ids);
    if (!generated_validation.ok) {
      return refuse(generated_validation.diagnostic_code + ":" +
                    generated_validation.detail);
    }
    if (generated.parent_working_row_indices.size() !=
            generated.batch.rows.size() ||
        generated.batch.rows.size() > request.maximum_result_row_count -
                                          output.rows.size()) {
      return refuse("recursive CTE SEARCH/CYCLE parent or result bound failed");
    }

    DescriptorBatch next_working;
    next_working.columns = generated.batch.columns;
    std::vector<std::vector<std::string>> next_paths;
    try {
      for (std::size_t row_index = 0;
           row_index < generated.batch.rows.size(); ++row_index) {
        const auto parent_index =
            generated.parent_working_row_indices[row_index];
        if (parent_index >= working.rows.size() ||
            parent_index >= working_paths.size()) {
          return refuse("recursive CTE SEARCH/CYCLE parent is unresolved");
        }
        const auto key = key_for_row(generated.batch.rows[row_index]);
        const auto& parent_path = working_paths[parent_index];
        const bool cycle =
            std::find(parent_path.begin(), parent_path.end(), key) !=
            parent_path.end();

        DescriptorTuple projected = generated.batch.rows[row_index];
        projected.values.push_back(sequence_value(++sequence));
        projected.values.push_back(cycle_value(cycle));
        output.rows.push_back(std::move(projected));
        metadata.push_back(
            {output.rows.size() - 1, iteration, sequence, cycle});
        if (cycle) {
          ++cycle_rows;
          continue;
        }
        if (next_working.rows.size() ==
            request.maximum_working_row_count) {
          return refuse("recursive CTE SEARCH/CYCLE working bound exceeded");
        }
        next_working.rows.push_back(generated.batch.rows[row_index]);
        auto path = parent_path;
        path.push_back(key);
        next_paths.push_back(std::move(path));
      }
    } catch (const std::exception& error) {
      return refuse(error.what());
    }
    working = std::move(next_working);
    working_paths = std::move(next_paths);
  }

  const auto output_validation =
      ValidateCanonicalDescriptorBatch(output, output_descriptor_ids);
  if (!output_validation.ok) {
    return refuse(output_validation.diagnostic_code + ":" +
                  output_validation.detail);
  }
  result.diagnostic = {};
  result.output_batch = std::move(output);
  result.row_metadata = std::move(metadata);
  result.recursive_iteration_count = iteration;
  result.cycle_row_count = cycle_rows;
  result.converged = true;
  result.selected_plan_uuid = request.physical_dag.selected_plan_uuid;
  result.executed_physical_node_id = selected_node->physical_node_id;
  result.causal_counter_id = selected_node->causal_counter_id;
  return result;
}

// QOW-SOURCE-QRY-014-RESOURCE-V1
// Charge every anchor and intermediate encoded value payload against the
// resource admission evidence bound into the selected physical DAG. The
// shared working executor still owns convergence and row/iteration limits;
// this wrapper adds one explicit byte grant and reports cleanup on every exit.
CanonicalRecursiveCteResourceResult ExecuteCanonicalRecursiveCteResource(
    const CanonicalRecursiveCteResourceRequest& request) {
  CanonicalRecursiveCteResourceResult result;
  result.working_state_cleaned = true;
  const auto refuse = [&](std::string detail) {
    result = {};
    result.working_result.diagnostic.ok = false;
    result.working_result.diagnostic.diagnostic_code =
        "QOW-DIAG-QRY-014-RESOURCE-REFUSAL-V1";
    result.working_result.diagnostic.detail = std::move(detail);
    result.working_state_cleaned = true;
    return result;
  };

  const auto dag_validation =
      ValidateTypedPhysicalNodeDag(request.working_request.physical_dag);
  if (!dag_validation.accepted) {
    const auto& issue = dag_validation.issues.front();
    return refuse(issue.diagnostic_id + ":" + issue.field_id);
  }
  const auto* selected_node = FindPhysicalNode(
      request.working_request.physical_dag,
      request.working_request.selected_physical_node_id);
  if (request.working_request.selected_physical_node_id == 0 ||
      request.working_request.selected_physical_node_id !=
          request.working_request.physical_dag.root_physical_node_id ||
      selected_node == nullptr ||
      selected_node->node_kind != PhysicalNodeKind::kRecursiveCte ||
      selected_node->implementation_id !=
          "cte.recursive.resource-bounded.typed.v1") {
    return refuse("recursive CTE resource physical profile is not bound");
  }
  const auto resource_evidence = std::find_if(
      request.working_request.physical_dag.admission_evidence.begin(),
      request.working_request.physical_dag.admission_evidence.end(),
      [](const PhysicalAdmissionEvidence& evidence) {
        return evidence.stage == PhysicalAdmissionStage::kResource;
      });
  if (request.maximum_materialized_value_bytes == 0 ||
      request.memory_grant_evidence_uuid.empty() ||
      resource_evidence ==
          request.working_request.physical_dag.admission_evidence.end() ||
      resource_evidence->evidence_uuid !=
          request.memory_grant_evidence_uuid) {
    return refuse("recursive CTE memory grant evidence is not bound");
  }

  const auto batch_bytes = [](const DescriptorBatch& batch) {
    std::size_t bytes = 0;
    for (const auto& row : batch.rows) {
      for (const auto& value : row.values) {
        if (value.encoded_value.size() >
            std::numeric_limits<std::size_t>::max() - bytes) {
          throw std::runtime_error("recursive CTE byte accounting overflow");
        }
        bytes += value.encoded_value.size();
      }
    }
    return bytes;
  };

  auto materialized_bytes = std::make_shared<std::size_t>(0);
  try {
    *materialized_bytes = batch_bytes(request.working_request.anchor_batch);
  } catch (const std::exception& error) {
    return refuse(error.what());
  }
  if (*materialized_bytes > request.maximum_materialized_value_bytes) {
    return refuse("recursive CTE anchor exceeded the encoded-value byte grant");
  }

  CanonicalRecursiveCteWorkingRequest working = request.working_request;
  for (auto& node : working.physical_dag.nodes) {
    if (node.physical_node_id == working.selected_physical_node_id) {
      node.implementation_id = "cte.recursive.working.typed.v1";
      break;
    }
  }
  const auto recursive_step = working.recursive_step;
  const auto maximum_bytes = request.maximum_materialized_value_bytes;
  working.recursive_step =
      [recursive_step, materialized_bytes, maximum_bytes, batch_bytes](
          const DescriptorBatch& current, const std::size_t iteration) {
        auto intermediate = recursive_step(current, iteration);
        const auto bytes = batch_bytes(intermediate);
        if (bytes > maximum_bytes - *materialized_bytes) {
          throw std::runtime_error(
              "recursive CTE encoded-value byte grant was exceeded");
        }
        *materialized_bytes += bytes;
        return intermediate;
      };

  auto working_result = ExecuteCanonicalRecursiveCteWorking(working);
  if (!working_result.diagnostic.ok) {
    return refuse(working_result.diagnostic.diagnostic_code + ":" +
                  working_result.diagnostic.detail);
  }
  result.working_result = std::move(working_result);
  result.materialized_value_bytes = *materialized_bytes;
  result.working_state_cleaned = true;
  result.memory_grant_evidence_uuid = request.memory_grant_evidence_uuid;
  return result;
}

// QOW-SOURCE-QRY-014-CANCELLATION-V1
// Probe cancellation before the anchor can be published and before each
// recursive step. The shared working executor keeps every accumulated and
// intermediate row private, allowing this wrapper to discard the entire state
// at the exact observed boundary without becoming transaction authority.
CanonicalRecursiveCteCancellationResult
ExecuteCanonicalRecursiveCteCancellation(
    const CanonicalRecursiveCteCancellationRequest& request) {
  CanonicalRecursiveCteCancellationResult result;
  result.working_state_cleaned = true;
  const auto refuse = [&](std::string detail, const bool cancelled = false,
                          const std::size_t ordinal = 0) {
    result = {};
    result.working_result.diagnostic.ok = false;
    result.working_result.diagnostic.diagnostic_code =
        "QOW-DIAG-QRY-014-CANCELLATION-REFUSAL-V1";
    result.working_result.diagnostic.detail = std::move(detail);
    result.cancelled = cancelled;
    result.cancellation_iteration_ordinal = ordinal;
    result.working_state_cleaned = true;
    if (cancelled) {
      result.cancellation_evidence_uuid =
          request.cancellation_evidence_uuid;
    }
    return result;
  };

  const auto dag_validation =
      ValidateTypedPhysicalNodeDag(request.working_request.physical_dag);
  if (!dag_validation.accepted) {
    const auto& issue = dag_validation.issues.front();
    return refuse(issue.diagnostic_id + ":" + issue.field_id);
  }
  const auto* selected_node = FindPhysicalNode(
      request.working_request.physical_dag,
      request.working_request.selected_physical_node_id);
  if (request.working_request.selected_physical_node_id == 0 ||
      request.working_request.selected_physical_node_id !=
          request.working_request.physical_dag.root_physical_node_id ||
      selected_node == nullptr ||
      selected_node->node_kind != PhysicalNodeKind::kRecursiveCte ||
      selected_node->implementation_id !=
          "cte.recursive.cancellable.typed.v1") {
    return refuse("recursive CTE cancellation physical profile is not bound");
  }
  const auto policy_evidence = std::find_if(
      request.working_request.physical_dag.admission_evidence.begin(),
      request.working_request.physical_dag.admission_evidence.end(),
      [](const PhysicalAdmissionEvidence& evidence) {
        return evidence.stage == PhysicalAdmissionStage::kPolicyCapability;
      });
  if (!request.cancellation_requested ||
      request.cancellation_evidence_uuid.empty() ||
      policy_evidence ==
          request.working_request.physical_dag.admission_evidence.end() ||
      policy_evidence->evidence_uuid !=
          request.cancellation_evidence_uuid) {
    return refuse("recursive CTE cancellation evidence is not bound");
  }

  try {
    if (request.cancellation_requested(0)) {
      return refuse("recursive CTE was cancelled before anchor publication",
                    true, 0);
    }
  } catch (const std::exception& error) {
    return refuse(std::string("recursive CTE cancellation probe failed:") +
                  error.what());
  } catch (...) {
    return refuse("recursive CTE cancellation probe failed");
  }

  CanonicalRecursiveCteWorkingRequest working = request.working_request;
  for (auto& node : working.physical_dag.nodes) {
    if (node.physical_node_id == working.selected_physical_node_id) {
      node.implementation_id = "cte.recursive.working.typed.v1";
      break;
    }
  }
  const auto recursive_step = working.recursive_step;
  const auto cancellation_probe = request.cancellation_requested;
  auto cancelled = std::make_shared<bool>(false);
  auto cancellation_ordinal = std::make_shared<std::size_t>(0);
  working.recursive_step =
      [recursive_step, cancellation_probe, cancelled, cancellation_ordinal](
          const DescriptorBatch& current, const std::size_t iteration) {
        if (cancellation_probe(iteration)) {
          *cancelled = true;
          *cancellation_ordinal = iteration;
          throw std::runtime_error("recursive CTE cancellation observed");
        }
        return recursive_step(current, iteration);
      };

  auto working_result = ExecuteCanonicalRecursiveCteWorking(working);
  if (*cancelled) {
    return refuse("recursive CTE cancellation observed", true,
                  *cancellation_ordinal);
  }
  if (!working_result.diagnostic.ok) {
    return refuse(working_result.diagnostic.diagnostic_code + ":" +
                  working_result.diagnostic.detail);
  }
  result.working_result = std::move(working_result);
  result.cancelled = false;
  result.cancellation_iteration_ordinal = 0;
  result.working_state_cleaned = true;
  result.cancellation_evidence_uuid = request.cancellation_evidence_uuid;
  return result;
}

// QOW-SOURCE-QRY-014-MGA-V1
// Consume engine-owned transaction-inventory evidence for the anchor boundary
// and every recursive transition, including the final empty transition. This
// executor validates ordered evidence but never chooses a snapshot, decides
// visibility/security, or acquires commit, rollback, WAL, or finality authority.
CanonicalRecursiveCteMgaResult ExecuteCanonicalRecursiveCteMgaBoundary(
    const CanonicalRecursiveCteMgaRequest& request) {
  CanonicalRecursiveCteMgaResult result;
  const auto refuse = [&](std::string detail) {
    result = {};
    result.working_result.diagnostic.ok = false;
    result.working_result.diagnostic.diagnostic_code =
        "QOW-DIAG-QRY-014-MGA-REFUSAL-V1";
    result.working_result.diagnostic.detail = std::move(detail);
    return result;
  };

  const auto dag_validation =
      ValidateTypedPhysicalNodeDag(request.working_request.physical_dag);
  if (!dag_validation.accepted) {
    const auto& issue = dag_validation.issues.front();
    return refuse(issue.diagnostic_id + ":" + issue.field_id);
  }
  const auto* selected_node = FindPhysicalNode(
      request.working_request.physical_dag,
      request.working_request.selected_physical_node_id);
  if (request.working_request.selected_physical_node_id == 0 ||
      request.working_request.selected_physical_node_id !=
          request.working_request.physical_dag.root_physical_node_id ||
      selected_node == nullptr ||
      selected_node->node_kind != PhysicalNodeKind::kRecursiveCte ||
      selected_node->implementation_id !=
          "cte.recursive.mga-boundary.typed.v1") {
    return refuse("recursive CTE MGA physical profile is not bound");
  }

  const auto mga_admission = std::find_if(
      request.working_request.physical_dag.admission_evidence.begin(),
      request.working_request.physical_dag.admission_evidence.end(),
      [](const PhysicalAdmissionEvidence& evidence) {
        return evidence.stage ==
               PhysicalAdmissionStage::kMgaStatementBoundary;
      });
  if (request.transaction_inventory_id == 0 ||
      request.inventory_local_transaction_id == 0 ||
      request.inventory_statement_snapshot_id == 0 ||
      request.inventory_local_transaction_id !=
          request.working_request.physical_dag.local_transaction_id ||
      request.inventory_statement_snapshot_id !=
          request.working_request.physical_dag.statement_snapshot_id ||
      !IsCanonicalCteEvidenceUuid(
          request.transaction_inventory_evidence_uuid) ||
      mga_admission ==
          request.working_request.physical_dag.admission_evidence.end() ||
      mga_admission->evidence_uuid !=
          request.transaction_inventory_evidence_uuid) {
    return refuse("recursive CTE transaction inventory evidence is not bound");
  }
  if (request.maximum_boundary_rechecks == 0 ||
      request.iteration_evidence.empty() ||
      request.iteration_evidence.size() > request.maximum_boundary_rechecks) {
    return refuse("recursive CTE MGA boundary recheck contract is invalid");
  }

  std::unordered_set<std::string> evidence_uuids;
  evidence_uuids.insert(request.transaction_inventory_evidence_uuid);
  for (std::size_t index = 0; index < request.iteration_evidence.size();
       ++index) {
    const auto& evidence = request.iteration_evidence[index];
    if (evidence.iteration_ordinal != index ||
        evidence.local_transaction_id !=
            request.inventory_local_transaction_id ||
        evidence.statement_snapshot_id !=
            request.inventory_statement_snapshot_id ||
        !IsCanonicalCteEvidenceUuid(evidence.engine_evidence_uuid) ||
        !evidence_uuids.insert(evidence.engine_evidence_uuid).second) {
      return refuse("recursive CTE iteration MGA evidence is not bound");
    }
    if (evidence.visibility != CanonicalMgaVisibilityDecision::kVisible) {
      return refuse("recursive CTE iteration is not MGA-visible");
    }
    if (evidence.security_decision !=
        CanonicalMgaSecurityDecision::kAllowed) {
      return refuse("recursive CTE iteration is not security-allowed");
    }
  }

  CanonicalRecursiveCteWorkingRequest working = request.working_request;
  for (auto& node : working.physical_dag.nodes) {
    if (node.physical_node_id == working.selected_physical_node_id) {
      node.implementation_id = "cte.recursive.working.typed.v1";
      break;
    }
  }
  auto working_result = ExecuteCanonicalRecursiveCteWorking(working);
  if (!working_result.diagnostic.ok) {
    return refuse(working_result.diagnostic.diagnostic_code + ":" +
                  working_result.diagnostic.detail);
  }
  if (request.iteration_evidence.size() !=
      working_result.recursive_iteration_count + 1) {
    return refuse("recursive CTE MGA evidence cardinality is not exact");
  }

  result.working_result = std::move(working_result);
  result.iteration_evidence_count = request.iteration_evidence.size();
  result.mga_boundary_proven = true;
  result.transaction_inventory_evidence_uuid =
      request.transaction_inventory_evidence_uuid;
  return result;
}

}  // namespace scratchbird::engine::executor
