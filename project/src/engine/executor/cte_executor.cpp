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

bool SameCanonicalCteColumns(
    const std::vector<ExecutorColumnDescriptor>& left,
    const std::vector<ExecutorColumnDescriptor>& right) {
  if (left.size() != right.size()) return false;
  for (std::size_t index = 0; index < left.size(); ++index) {
    const auto& left_column = left[index];
    const auto& right_column = right[index];
    if (left_column.descriptor_id != right_column.descriptor_id ||
        left_column.stable_name != right_column.stable_name ||
        left_column.nullable != right_column.nullable ||
        left_column.descriptor.descriptor_uuid.canonical !=
            right_column.descriptor.descriptor_uuid.canonical ||
        left_column.descriptor.descriptor_kind !=
            right_column.descriptor.descriptor_kind ||
        left_column.descriptor.canonical_type_name !=
            right_column.descriptor.canonical_type_name ||
        left_column.descriptor.encoded_descriptor !=
            right_column.descriptor.encoded_descriptor) {
      return false;
    }
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

  const auto authority_validation = RevalidateCanonicalExecutionMgaAuthority(
      request.mga_authority, request.physical_dag);
  if (!authority_validation.ok) {
    return refuse(authority_validation.diagnostic_code + ":" +
                  authority_validation.detail);
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
  const bool literal_values_anchor =
      anchor_node != nullptr &&
      anchor_node->node_kind == PhysicalNodeKind::kValues;
  const bool materialized_count_anchor =
      anchor_node != nullptr &&
      anchor_node->node_kind == PhysicalNodeKind::kAggregate &&
      anchor_node->engine_capability_validated &&
      anchor_node->input_physical_node_ids.size() == 1 &&
      anchor_node->output_descriptor_ids.size() == 1 &&
      anchor_node->logical_semantic_variant_id ==
          "aggregate.global-count-star.v1" &&
      anchor_node->implementation_id == "aggregate.count-star.v1";
  const bool exact_count_recursive_term =
      !materialized_count_anchor ||
      (recursive_node != nullptr &&
       recursive_node->engine_capability_validated &&
       recursive_node->input_physical_node_ids.empty() &&
       recursive_node->output_descriptor_ids.size() == 1 &&
       recursive_node->logical_semantic_variant_id ==
           "cte.recursive-term-int64-increment.v1" &&
       recursive_node->implementation_id ==
           "cte.recursive-term.int64-increment.typed.v1");
  if (anchor_node == nullptr || recursive_node == nullptr ||
      (!literal_values_anchor && !materialized_count_anchor) ||
      !exact_count_recursive_term ||
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
  if (materialized_count_anchor &&
      (request.anchor_batch.columns.size() != 1 ||
       request.anchor_batch.columns.front().descriptor.canonical_type_name !=
           "int64" ||
       request.anchor_batch.rows.size() != 1 ||
       request.anchor_batch.rows.front().values.size() != 1 ||
       request.anchor_batch.rows.front().values.front().is_null ||
       request.anchor_batch.rows.front().values.front().state !=
           scratchbird::engine::internal_api::EngineValueState::value ||
       !DecodeInt64Value(request.anchor_batch.rows.front().values.front())
            .ok())) {
    return refuse(
        "recursive CTE COUNT(*) anchor is not one materialized non-null int64 value");
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
    const auto pre_step_authority = RevalidateCanonicalExecutionMgaAuthority(
        request.mga_authority, request.physical_dag);
    if (!pre_step_authority.ok) {
      return refuse(pre_step_authority.diagnostic_code + ":" +
                    pre_step_authority.detail);
    }
    try {
      intermediate = request.recursive_step(working, iteration_ordinal);
    } catch (const std::exception& error) {
      return refuse(std::string("recursive CTE step failed:") + error.what());
    } catch (...) {
      return refuse("recursive CTE step failed with an unknown exception");
    }
    const auto post_step_authority = RevalidateCanonicalExecutionMgaAuthority(
        request.mga_authority, request.physical_dag);
    if (!post_step_authority.ok) {
      return refuse(post_step_authority.diagnostic_code + ":" +
                    post_step_authority.detail);
    }
    const auto intermediate_validation = ValidateCanonicalDescriptorBatch(
        intermediate, recursive_node->output_descriptor_ids);
    if (!intermediate_validation.ok) {
      return refuse(intermediate_validation.diagnostic_code + ":" +
                    intermediate_validation.detail);
    }
    if (!SameCanonicalCteColumns(intermediate.columns,
                                 request.anchor_batch.columns)) {
      return refuse("recursive CTE generated schema differs from its anchor");
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
  const auto result_authority = RevalidateCanonicalExecutionMgaAuthority(
      request.mga_authority, request.physical_dag);
  if (!result_authority.ok) {
    return refuse(result_authority.diagnostic_code + ":" +
                  result_authority.detail);
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
  result.mga_statement_context = request.mga_authority.statement_context;
  return result;
}

// QOW-SOURCE-QRY-014-UNION-V1
// Admit either UNION ALL, which preserves every anchor and recursive row, or
// descriptor-wide UNION DISTINCT. The legacy one-column int64 identity is an
// exact alias; the general typed profile binds one equality term per output
// column. DISTINCT uses the canonical ordering comparator (with SQL NULL equal
// to SQL NULL for set semantics), removes duplicates against the complete
// accumulated result and the current intermediate relation, and feeds only
// newly admitted rows into the next recursive working transition.
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
  const auto authority_validation = RevalidateCanonicalExecutionMgaAuthority(
      request.working_request.mga_authority,
      request.working_request.physical_dag);
  if (!authority_validation.ok) {
    return refuse(authority_validation.diagnostic_code + ":" +
                  authority_validation.detail);
  }
  const auto* selected_node = FindPhysicalNode(
      request.working_request.physical_dag,
      request.working_request.selected_physical_node_id);
  const bool distinct_profile =
      selected_node != nullptr &&
      (selected_node->implementation_id ==
           "cte.recursive.union-distinct-int64.typed.v1" ||
       selected_node->implementation_id ==
           "cte.recursive.union-distinct.typed.v1");
  const bool profile_matches =
      request.union_mode == CanonicalRecursiveCteUnionMode::kAll
          ? selected_node != nullptr &&
                selected_node->implementation_id ==
                    "cte.recursive.union-all.typed.v1"
          : distinct_profile;
  if (request.working_request.selected_physical_node_id == 0 ||
      request.working_request.selected_physical_node_id !=
          request.working_request.physical_dag.root_physical_node_id ||
      selected_node == nullptr ||
      selected_node->node_kind != PhysicalNodeKind::kRecursiveCte ||
      !profile_matches) {
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
    const bool legacy_int64_profile =
        selected_node->implementation_id ==
        "cte.recursive.union-distinct-int64.typed.v1";
    if (legacy_int64_profile &&
        (working.anchor_batch.columns.size() != 1 ||
         working.anchor_batch.columns.front()
                 .descriptor.canonical_type_name != "int64")) {
      return refuse("recursive CTE UNION DISTINCT requires one int64 column");
    }
    std::vector<CanonicalDescriptorOrderTerm> equality_terms =
        request.equality_terms;
    if (legacy_int64_profile && equality_terms.empty()) {
      CanonicalDescriptorOrderTerm term;
      term.column = 0;
      term.expression_descriptor_id =
          working.anchor_batch.columns.front().descriptor_id;
      term.direction = CanonicalDescriptorOrderDirection::ascending;
      term.null_placement = CanonicalDescriptorNullPlacement::first;
      equality_terms.push_back(std::move(term));
    }
    if (equality_terms.size() != working.anchor_batch.columns.size() ||
        equality_terms.empty() ||
        request.maximum_value_comparison_count == 0) {
      return refuse(
          "recursive CTE UNION DISTINCT requires one bounded equality term "
          "per output column");
    }
    std::vector<bool> covered(working.anchor_batch.columns.size(), false);
    for (const auto& term : equality_terms) {
      if (term.column >= working.anchor_batch.columns.size() ||
          covered[term.column] ||
          term.expression_descriptor_id !=
              working.anchor_batch.columns[term.column].descriptor_id ||
          term.direction != CanonicalDescriptorOrderDirection::ascending ||
          term.null_placement != CanonicalDescriptorNullPlacement::first) {
        return refuse(
            "recursive CTE UNION DISTINCT equality term coverage is invalid");
      }
      const auto term_validation = ValidateCanonicalDescriptorOrderTerm(
          term, working.anchor_batch.columns[term.column]);
      if (!term_validation.ok) {
        return refuse(term_validation.diagnostic_code + ":" +
                      term_validation.detail);
      }
      covered[term.column] = true;
    }

    auto comparison_count = std::make_shared<std::size_t>(0);
    const auto equal_rows =
        [equality_terms = std::move(equality_terms), comparison_count,
         maximum = request.maximum_value_comparison_count](
            const DescriptorTuple& left, const DescriptorTuple& right) {
          if (left.values.size() != equality_terms.size() ||
              right.values.size() != equality_terms.size()) {
            throw std::runtime_error("recursive UNION DISTINCT row is ragged");
          }
          for (const auto& term : equality_terms) {
            if (*comparison_count == maximum) {
              throw std::runtime_error(
                  "recursive CTE UNION DISTINCT value comparison bound was "
                  "exceeded");
            }
            ++*comparison_count;
            const auto compared = CompareCanonicalDescriptorOrderValues(
                left.values[term.column], right.values[term.column], term);
            if (!compared.diagnostic.ok) {
              throw std::runtime_error(compared.diagnostic.diagnostic_code +
                                       ":" + compared.diagnostic.detail);
            }
            if (compared.comparison != 0) return false;
          }
          return true;
        };
    auto seen = std::make_shared<std::vector<DescriptorTuple>>();
    const auto admit = [seen, duplicate_count, equal_rows](
                           const DescriptorTuple& row) {
      if (!equal_rows(row, row)) {
        throw std::runtime_error(
            "recursive UNION DISTINCT self comparison failed");
      }
      for (const auto& representative : *seen) {
        if (equal_rows(row, representative)) {
          ++*duplicate_count;
          return false;
        }
      }
      seen->push_back(row);
      return true;
    };
    DescriptorBatch distinct_anchor;
    distinct_anchor.columns = working.anchor_batch.columns;
    try {
      for (const auto& row : working.anchor_batch.rows) {
        if (admit(row)) {
          distinct_anchor.rows.push_back(row);
        }
      }
    } catch (const std::exception& error) {
      return refuse(error.what());
    }
    working.anchor_batch = std::move(distinct_anchor);

    const auto recursive_step = working.recursive_step;
    working.recursive_step =
        [recursive_step, admit](
            const DescriptorBatch& current, const std::size_t iteration) {
          auto generated = recursive_step(current, iteration);
          DescriptorBatch distinct;
          distinct.columns = generated.columns;
          for (const auto& row : generated.rows) {
            if (admit(row)) {
              distinct.rows.push_back(row);
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
  if (!PhysicalMgaStatementContextEqual(
          working_result.mga_statement_context,
          request.working_request.mga_authority.statement_context)) {
    return refuse("recursive CTE UNION working result changed MGA statement context");
  }
  const auto result_authority = RevalidateCanonicalExecutionMgaAuthority(
      request.working_request.mga_authority,
      request.working_request.physical_dag);
  if (!result_authority.ok) {
    return refuse(result_authority.diagnostic_code + ":" +
                  result_authority.detail);
  }
  result.working_result = std::move(working_result);
  result.union_mode = request.union_mode;
  result.duplicate_row_count = *duplicate_count;
  result.mga_statement_context =
      request.working_request.mga_authority.statement_context;
  return result;
}

// QOW-SOURCE-QRY-014-SEARCH-CYCLE-V1
// Execute the accepted breadth-first SEARCH profile and descriptor-bound typed
// CYCLE keys. Parent indices bind every generated row to one current working
// row, allowing path-local cycle detection. A cycle row is emitted once with a
// typed TRUE mark and is never placed into the next working relation. The
// legacy physical profile is preserved as a one-column int64 specialization.
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

  const auto authority_validation = RevalidateCanonicalExecutionMgaAuthority(
      request.mga_authority, request.physical_dag);
  if (!authority_validation.ok) {
    return refuse(authority_validation.diagnostic_code + ":" +
                  authority_validation.detail);
  }
  const auto* selected_node = FindPhysicalNode(
      request.physical_dag, request.selected_physical_node_id);
  const bool legacy_int64_profile =
      selected_node != nullptr &&
      selected_node->implementation_id ==
          "cte.recursive.search-breadth-cycle-int64.typed.v1";
  const bool generic_typed_profile =
      selected_node != nullptr &&
      selected_node->implementation_id ==
          "cte.recursive.search-breadth-cycle.typed.v1";
  if (request.selected_physical_node_id == 0 ||
      request.selected_physical_node_id !=
          request.physical_dag.root_physical_node_id ||
      selected_node == nullptr ||
      selected_node->node_kind != PhysicalNodeKind::kRecursiveCte ||
      (!legacy_int64_profile && !generic_typed_profile) ||
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
      request.search_sequence_column.descriptor_id == 0 ||
      request.search_sequence_column.nullable ||
      request.search_sequence_column.descriptor.canonical_type_name !=
          "int64" ||
      request.cycle_mark_column.descriptor_id == 0 ||
      request.cycle_mark_column.nullable ||
      request.cycle_mark_column.descriptor.canonical_type_name != "boolean") {
    return refuse("recursive CTE SEARCH/CYCLE descriptors are not exact");
  }

  std::vector<CanonicalDescriptorOrderTerm> cycle_key_terms =
      request.cycle_key_terms;
  if (legacy_int64_profile && cycle_key_terms.empty()) {
    if (request.cycle_key_column >= request.anchor_batch.columns.size() ||
        request.cycle_key_expression_descriptor_id == 0 ||
        request.anchor_batch.columns[request.cycle_key_column].descriptor_id !=
            request.cycle_key_expression_descriptor_id ||
        request.anchor_batch.columns[request.cycle_key_column]
                .descriptor.canonical_type_name != "int64") {
      return refuse("legacy recursive CTE CYCLE key is not one int64 column");
    }
    CanonicalDescriptorOrderTerm term;
    term.column = request.cycle_key_column;
    term.expression_descriptor_id =
        request.cycle_key_expression_descriptor_id;
    term.direction = CanonicalDescriptorOrderDirection::ascending;
    term.null_placement = CanonicalDescriptorNullPlacement::first;
    cycle_key_terms.push_back(std::move(term));
  }
  if (cycle_key_terms.empty() ||
      request.maximum_value_comparison_count == 0) {
    return refuse(
        "recursive CTE SEARCH/CYCLE requires bounded typed cycle keys");
  }
  std::vector<bool> covered(request.anchor_batch.columns.size(), false);
  for (const auto& term : cycle_key_terms) {
    if (term.column >= request.anchor_batch.columns.size() ||
        covered[term.column] ||
        term.expression_descriptor_id !=
            request.anchor_batch.columns[term.column].descriptor_id ||
        term.direction != CanonicalDescriptorOrderDirection::ascending ||
        term.null_placement != CanonicalDescriptorNullPlacement::first) {
      return refuse("recursive CTE SEARCH/CYCLE key binding is invalid");
    }
    const auto term_validation = ValidateCanonicalDescriptorOrderTerm(
        term, request.anchor_batch.columns[term.column]);
    if (!term_validation.ok) {
      return refuse(term_validation.diagnostic_code + ":" +
                    term_validation.detail);
    }
    covered[term.column] = true;
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

  std::size_t value_comparison_count = 0;
  const auto cycle_keys_equal = [&](const DescriptorTuple& left,
                                    const DescriptorTuple& right) {
    if (left.values.size() != request.anchor_batch.columns.size() ||
        right.values.size() != request.anchor_batch.columns.size()) {
      throw std::runtime_error("recursive CTE cycle-key row is ragged");
    }
    for (const auto& term : cycle_key_terms) {
      if (value_comparison_count ==
          request.maximum_value_comparison_count) {
        throw std::runtime_error(
            "recursive CTE SEARCH/CYCLE value comparison bound was exceeded");
      }
      ++value_comparison_count;
      const auto compared = CompareCanonicalDescriptorOrderValues(
          left.values[term.column], right.values[term.column], term);
      if (!compared.diagnostic.ok) {
        throw std::runtime_error(compared.diagnostic.diagnostic_code + ":" +
                                 compared.diagnostic.detail);
      }
      if (compared.comparison != 0) return false;
    }
    return true;
  };
  const auto sequence_value = [&](const std::uint64_t sequence) {
    if (sequence == 0 ||
        sequence > static_cast<std::uint64_t>(
                       std::numeric_limits<std::int64_t>::max())) {
      throw std::runtime_error(
          "recursive CTE SEARCH sequence exceeds int64 result width");
    }
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
  std::vector<std::vector<DescriptorTuple>> working_paths;
  std::vector<CanonicalRecursiveCteSearchCycleMetadata> metadata;
  std::uint64_t sequence = 0;
  try {
    working_paths.reserve(working.rows.size());
    for (const auto& row : working.rows) {
      if (!cycle_keys_equal(row, row)) {
        throw std::runtime_error(
            "recursive CTE cycle key self comparison failed");
      }
      working_paths.push_back({row});
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
    const auto pre_step_authority = RevalidateCanonicalExecutionMgaAuthority(
        request.mga_authority, request.physical_dag);
    if (!pre_step_authority.ok) {
      return refuse(pre_step_authority.diagnostic_code + ":" +
                    pre_step_authority.detail);
    }
    try {
      generated = request.recursive_step(working, iteration);
    } catch (const std::exception& error) {
      return refuse(std::string("recursive CTE SEARCH/CYCLE step failed:") +
                    error.what());
    } catch (...) {
      return refuse("recursive CTE SEARCH/CYCLE step failed");
    }
    const auto post_step_authority = RevalidateCanonicalExecutionMgaAuthority(
        request.mga_authority, request.physical_dag);
    if (!post_step_authority.ok) {
      return refuse(post_step_authority.diagnostic_code + ":" +
                    post_step_authority.detail);
    }
    const auto generated_validation = ValidateCanonicalDescriptorBatch(
        generated.batch, recursive_node->output_descriptor_ids);
    if (!generated_validation.ok) {
      return refuse(generated_validation.diagnostic_code + ":" +
                    generated_validation.detail);
    }
    if (!SameCanonicalCteColumns(generated.batch.columns,
                                 request.anchor_batch.columns)) {
      return refuse(
          "recursive CTE SEARCH/CYCLE generated schema differs from its anchor");
    }
    if (generated.parent_working_row_indices.size() !=
            generated.batch.rows.size() ||
        generated.batch.rows.size() > request.maximum_result_row_count -
                                          output.rows.size()) {
      return refuse("recursive CTE SEARCH/CYCLE parent or result bound failed");
    }

    DescriptorBatch next_working;
    next_working.columns = request.anchor_batch.columns;
    std::vector<std::vector<DescriptorTuple>> next_paths;
    try {
      for (std::size_t row_index = 0;
           row_index < generated.batch.rows.size(); ++row_index) {
        const auto parent_index =
            generated.parent_working_row_indices[row_index];
        if (parent_index >= working.rows.size() ||
            parent_index >= working_paths.size()) {
          return refuse("recursive CTE SEARCH/CYCLE parent is unresolved");
        }
        const auto& generated_row = generated.batch.rows[row_index];
        if (!cycle_keys_equal(generated_row, generated_row)) {
          throw std::runtime_error(
              "recursive CTE cycle key self comparison failed");
        }
        const auto& parent_path = working_paths[parent_index];
        bool cycle = false;
        for (const auto& ancestor : parent_path) {
          if (cycle_keys_equal(generated_row, ancestor)) {
            cycle = true;
            break;
          }
        }

        DescriptorTuple projected = generated_row;
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
        next_working.rows.push_back(generated_row);
        auto path = parent_path;
        path.push_back(generated_row);
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
  const auto result_authority = RevalidateCanonicalExecutionMgaAuthority(
      request.mga_authority, request.physical_dag);
  if (!result_authority.ok) {
    return refuse(result_authority.diagnostic_code + ":" +
                  result_authority.detail);
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
  result.mga_statement_context = request.mga_authority.statement_context;
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

  const auto authority_validation = RevalidateCanonicalExecutionMgaAuthority(
      request.working_request.mga_authority,
      request.working_request.physical_dag);
  if (!authority_validation.ok) {
    return refuse(authority_validation.diagnostic_code + ":" +
                  authority_validation.detail);
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
        if (value.binary_value.size() >
            std::numeric_limits<std::size_t>::max() - bytes) {
          throw std::runtime_error("recursive CTE byte accounting overflow");
        }
        bytes += value.binary_value.size();
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
  if (!PhysicalMgaStatementContextEqual(
          working_result.mga_statement_context,
          request.working_request.mga_authority.statement_context)) {
    return refuse("recursive CTE resource working result changed MGA statement context");
  }
  const auto result_authority = RevalidateCanonicalExecutionMgaAuthority(
      request.working_request.mga_authority,
      request.working_request.physical_dag);
  if (!result_authority.ok) {
    return refuse(result_authority.diagnostic_code + ":" +
                  result_authority.detail);
  }
  result.working_result = std::move(working_result);
  result.materialized_value_bytes = *materialized_bytes;
  result.working_state_cleaned = true;
  result.memory_grant_evidence_uuid = request.memory_grant_evidence_uuid;
  result.mga_statement_context =
      request.working_request.mga_authority.statement_context;
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

  const auto authority_validation = RevalidateCanonicalExecutionMgaAuthority(
      request.working_request.mga_authority,
      request.working_request.physical_dag);
  if (!authority_validation.ok) {
    return refuse(authority_validation.diagnostic_code + ":" +
                  authority_validation.detail);
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
  if (!PhysicalMgaStatementContextEqual(
          working_result.mga_statement_context,
          request.working_request.mga_authority.statement_context)) {
    return refuse("recursive CTE cancellation working result changed MGA statement context");
  }
  const auto result_authority = RevalidateCanonicalExecutionMgaAuthority(
      request.working_request.mga_authority,
      request.working_request.physical_dag);
  if (!result_authority.ok) {
    return refuse(result_authority.diagnostic_code + ":" +
                  result_authority.detail);
  }
  result.working_result = std::move(working_result);
  result.cancelled = false;
  result.cancellation_iteration_ordinal = 0;
  result.working_state_cleaned = true;
  result.cancellation_evidence_uuid = request.cancellation_evidence_uuid;
  result.mga_statement_context =
      request.working_request.mga_authority.statement_context;
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

  const auto authority_validation = RevalidateCanonicalExecutionMgaAuthority(
      request.mga_authority, request.working_request.physical_dag);
  const auto working_authority_validation =
      RevalidateCanonicalExecutionMgaAuthority(
          request.working_request.mga_authority,
          request.working_request.physical_dag);
  if (!authority_validation.ok || !working_authority_validation.ok ||
      !PhysicalMgaStatementContextEqual(
          request.mga_authority.statement_context,
          request.working_request.mga_authority.statement_context)) {
    return refuse(authority_validation.ok && working_authority_validation.ok
                      ? "recursive CTE working context differs from MGA boundary"
                      : (!authority_validation.ok
                             ? authority_validation.diagnostic_code + ":" +
                                   authority_validation.detail
                             : working_authority_validation.diagnostic_code +
                                   ":" + working_authority_validation.detail));
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
        evidence.creator_local_transaction_id == 0 ||
        !IsCanonicalCteEvidenceUuid(evidence.engine_evidence_uuid) ||
        !evidence_uuids.insert(evidence.engine_evidence_uuid).second) {
      return refuse("recursive CTE iteration MGA evidence is not bound");
    }
    if (evidence.visibility != CanonicalMgaVisibilityDecision::kVisible) {
      return refuse("recursive CTE iteration is not MGA-visible");
    }
    if (!CanonicalMgaCreatorVisibleToStatement(
            request.mga_authority.statement_context,
            evidence.creator_local_transaction_id)) {
      return refuse(
          "recursive CTE visible iteration contradicts captured MGA vector");
    }
    if (evidence.security_decision !=
        CanonicalMgaSecurityDecision::kAllowed) {
      return refuse("recursive CTE iteration is not security-allowed");
    }
  }

  CanonicalRecursiveCteWorkingRequest working = request.working_request;
  working.mga_authority = request.mga_authority;
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
  if (!PhysicalMgaStatementContextEqual(
          working_result.mga_statement_context,
          request.mga_authority.statement_context)) {
    return refuse("recursive CTE MGA working result changed MGA statement context");
  }
  const auto result_authority = RevalidateCanonicalExecutionMgaAuthority(
      request.mga_authority, request.working_request.physical_dag);
  if (!result_authority.ok) {
    return refuse(result_authority.diagnostic_code + ":" +
                  result_authority.detail);
  }

  result.working_result = std::move(working_result);
  result.iteration_evidence_count = request.iteration_evidence.size();
  result.mga_boundary_proven = true;
  result.transaction_inventory_evidence_uuid =
      request.transaction_inventory_evidence_uuid;
  result.mga_statement_context = request.mga_authority.statement_context;
  return result;
}

}  // namespace scratchbird::engine::executor
