// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "canonical_query_recursive_registration.hpp"

#include "canonical_query_physical_registration.hpp"
#include "canonical_query_runtime_memory_support.hpp"

#include <new>
#include <optional>
#include <stdexcept>
#include <utility>

namespace scratchbird::engine::sblr {

namespace api = scratchbird::engine::internal_api;

bool BoundPreparedRecursiveCtePeakPayload(
    const PreparedRecursiveCteRoot& prepared,
    std::size_t* peak_payload_bytes) {
  if (peak_payload_bytes == nullptr ||
      prepared.anchor_columns.empty() ||
      prepared.maximum_working_row_count == 0 ||
      prepared.maximum_term_output_row_count == 0 ||
      prepared.maximum_result_row_count == 0) {
    return false;
  }
  // The currently admitted live recursive term is an exact signed-int64
  // increment. Reserve both its longest canonical text form and native binary
  // carrier. SEARCH/CYCLE adds an int64 sequence and canonical boolean mark.
  constexpr std::uint64_t kInt64PayloadBytes = 20 + sizeof(std::int64_t);
  constexpr std::uint64_t kBooleanPayloadBytes = 5 + sizeof(bool);
  std::uint64_t base_row_payload = 0;
  if (!CheckedMultiply(prepared.anchor_columns.size(),
                       kInt64PayloadBytes, &base_row_payload)) {
    return false;
  }
  std::uint64_t anchor_payload = 0;
  std::uint64_t working_payload = 0;
  std::uint64_t generated_payload = 0;
  std::uint64_t result_payload = 0;
  if (!CheckedMultiply(prepared.maximum_anchor_row_count, base_row_payload,
                       &anchor_payload) ||
      !CheckedMultiply(prepared.maximum_working_row_count,
                       base_row_payload, &working_payload) ||
      !CheckedMultiply(prepared.maximum_term_output_row_count,
                       base_row_payload, &generated_payload)) {
    return false;
  }
  if (prepared.profile.search_cycle) {
    std::uint64_t projected_row_payload = 0;
    if (!CheckedAdd(base_row_payload, kInt64PayloadBytes,
                    &projected_row_payload) ||
        !CheckedAdd(projected_row_payload, kBooleanPayloadBytes,
                    &projected_row_payload) ||
        !CheckedMultiply(prepared.maximum_result_row_count,
                         projected_row_payload, &result_payload)) {
      return false;
    }
    // Dispatcher anchor + request anchor + projected result + current
    // working + raw generated + next working + one projected row in flight.
    std::uint64_t peak = 1;
    if (!CheckedAdd(peak, anchor_payload, &peak) ||
        !CheckedAdd(peak, anchor_payload, &peak) ||
        !CheckedAdd(peak, result_payload, &peak) ||
        !CheckedAdd(peak, working_payload, &peak) ||
        !CheckedAdd(peak, generated_payload, &peak) ||
        !CheckedAdd(peak, generated_payload, &peak) ||
        !CheckedAdd(peak, projected_row_payload, &peak) ||
        peak > std::numeric_limits<std::size_t>::max()) {
      return false;
    }
    *peak_payload_bytes = static_cast<std::size_t>(peak);
    return true;
  }
  if (!CheckedMultiply(prepared.maximum_result_row_count,
                       base_row_payload, &result_payload)) {
    return false;
  }
  std::uint64_t peak = 1;
  // Dispatcher anchor + outer UNION request anchor + inner Working request
  // anchor + accumulated result + typed DISTINCT representatives + current
  // working + raw/admitted output overlap + the normalized retained copy.
  if (!CheckedAdd(peak, anchor_payload, &peak) ||
      !CheckedAdd(peak, anchor_payload, &peak) ||
      !CheckedAdd(peak, anchor_payload, &peak) ||
      !CheckedAdd(peak, result_payload, &peak) ||
      !CheckedAdd(peak, result_payload, &peak) ||
      !CheckedAdd(peak, working_payload, &peak) ||
      !CheckedAdd(peak, generated_payload, &peak) ||
      !CheckedAdd(peak, generated_payload, &peak) ||
      !CheckedAdd(peak, generated_payload, &peak) ||
      peak > std::numeric_limits<std::size_t>::max()) {
    return false;
  }
  *peak_payload_bytes = static_cast<std::size_t>(peak);
  return true;
}

bool BindPreparedRecursiveCtePeakMemory(
    PreparedRecursiveCteRoot* prepared,
    const std::uint64_t memory_budget_bytes) {
  if (prepared == nullptr || memory_budget_bytes == 0 ||
      !BoundPreparedRecursiveCtePeakPayload(
          *prepared, &prepared->planned_peak_payload_bytes)) {
    return false;
  }
  exec::CanonicalRecursiveCteStructuralCapacity structural_capacity;
  structural_capacity.profile =
      prepared->profile.search_cycle
          ? exec::CanonicalRecursiveCteStructuralProfile::kSearchCycle
          : prepared->profile.union_mode ==
                    exec::CanonicalRecursiveCteUnionMode::kDistinct
                ? exec::CanonicalRecursiveCteStructuralProfile::
                      kUnionDistinctInt64
                : exec::CanonicalRecursiveCteStructuralProfile::kUnionAll;
  structural_capacity.maximum_anchor_row_count =
      prepared->maximum_anchor_row_count;
  structural_capacity.maximum_iteration_count =
      prepared->maximum_iteration_count;
  structural_capacity.maximum_working_row_count =
      prepared->maximum_working_row_count;
  structural_capacity.maximum_recursive_output_row_count =
      prepared->maximum_term_output_row_count;
  structural_capacity.maximum_result_row_count =
      prepared->maximum_result_row_count;
  structural_capacity.equality_term_count =
      prepared->profile.search_cycle ||
              prepared->profile.union_mode ==
                  exec::CanonicalRecursiveCteUnionMode::kDistinct
          ? prepared->anchor_columns.size()
          : 0;
  std::string structural_detail;
  if (!exec::BoundCanonicalRecursiveCteStructuralBytes(
          prepared->anchor_columns,
          prepared->profile.search_cycle
              ? &prepared->search_sequence_column
              : nullptr,
          prepared->profile.search_cycle
              ? &prepared->cycle_mark_column
              : nullptr,
          nullptr,
          structural_capacity,
          &prepared->planned_resident_structural_bytes,
          &structural_detail)) {
    return false;
  }
  std::uint64_t total = 0;
  if (!CheckedAdd(prepared->planned_peak_payload_bytes,
                  prepared->planned_resident_structural_bytes,
                  &total) ||
      total > std::numeric_limits<std::size_t>::max()) {
    return false;
  }
  prepared->planned_peak_memory_bytes =
      static_cast<std::size_t>(total);
  return prepared->planned_peak_memory_bytes <= memory_budget_bytes;
}


// SEARCH_KEY: SB_ENGINE_CANONICAL_QUERY_RECURSIVE_ROOT_REGISTRATION_AUTHORITY
exec::CanonicalPhysicalExecutorRegistration
MakeLiveRecursiveCteRegistration(
    PreparedRecursiveCteRoot prepared,
    std::string recursive_term_capability_uuid,
    std::string capability_uuid,
    api::EngineRequestContext mga_context) {
  exec::CanonicalPhysicalExecutorRegistration registration;
  registration.node_kind = exec::PhysicalNodeKind::kRecursiveCte;
  registration.implementation_id = prepared.profile.implementation_id;
  registration.executor_capability_uuid = std::move(capability_uuid);
  registration.executor_capability_abi_version = 1;
  registration.engine_owned = true;
  registration.accepts_optimizer_publication_v2 = true;
  registration.publishes_runtime_observation_v1 = true;
  registration.execute =
      [prepared = std::move(prepared),
       recursive_term_capability_uuid =
           std::move(recursive_term_capability_uuid),
       mga_context = std::move(mga_context)](
          const exec::TypedPhysicalNodeDag& dag,
          const exec::PhysicalNodeRecord& node,
          const std::vector<exec::CanonicalPhysicalDispatchInput>& inputs) {
        exec::CanonicalPhysicalDispatchStepResult step;
        step.selected_plan_uuid = dag.selected_plan_uuid;
        step.mga_statement_context = dag.mga_statement_context;
        step.executed_physical_node_id = node.physical_node_id;
        step.causal_counter_id = node.causal_counter_id;
        step.output_descriptor_ids = node.output_descriptor_ids;
        step.authority.engine_mga_snapshot_bound = true;
        bool output_schema_matches = false;
        if (!inputs.empty()) {
          output_schema_matches =
              prepared.profile.search_cycle
                  ? node.output_descriptor_ids.size() ==
                            inputs[0].output_descriptor_ids.size() + 2 &&
                        std::equal(
                            inputs[0].output_descriptor_ids.begin(),
                            inputs[0].output_descriptor_ids.end(),
                            node.output_descriptor_ids.begin())
                  : node.output_descriptor_ids ==
                        inputs[0].output_descriptor_ids;
        }
        if (inputs.size() != 2 ||
            node.input_physical_node_ids.size() != 2 ||
            node.input_physical_node_ids[0] ==
                node.input_physical_node_ids[1] ||
            inputs[0].physical_node_id !=
                node.input_physical_node_ids[0] ||
            inputs[1].physical_node_id !=
                node.input_physical_node_ids[1] ||
            !inputs[0].materialized_output_batch.has_value() ||
            !inputs[1].materialized_output_batch.has_value() ||
            inputs[0].materialized_output_batch->rows.size() >
                prepared.maximum_anchor_row_count ||
            !inputs[1].materialized_output_batch->rows.empty() ||
            inputs[0].output_descriptor_ids !=
                inputs[1].output_descriptor_ids ||
            !output_schema_matches) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "QOW-DIAG-RELATIONAL-LIVE-RECURSIVE-CTE-INPUT-V1";
          step.diagnostic.detail =
              "recursive CTE did not receive its exact anchor and term "
              "bindings";
          return step;
        }

        const exec::TypedPhysicalNodeDag* execution_dag = &dag;
        std::optional<exec::TypedPhysicalNodeDag> scoped_execution_dag;
        if (node.physical_node_id != dag.root_physical_node_id) {
          scoped_execution_dag.emplace();
          std::string scope_detail;
          if (!BuildOperatorLocalPhysicalDag(
                  dag, node.physical_node_id, &*scoped_execution_dag,
                  &scope_detail)) {
            step.diagnostic.ok = false;
            step.diagnostic.diagnostic_code =
                "QOW-DIAG-RELATIONAL-LIVE-RECURSIVE-CTE-INPUT-V1";
            step.diagnostic.detail =
                "recursive CTE execution view is unresolved";
            return step;
          }
          execution_dag = &*scoped_execution_dag;
        }

        std::uint64_t retained_input_payload_bytes = 0;
        std::uint64_t anchor_input_payload_bytes = 0;
        for (std::size_t input_index = 0; input_index < inputs.size();
             ++input_index) {
          const auto& input = inputs[input_index];
          std::uint64_t input_payload_bytes = 0;
          if (!RuntimeMaterializedBatchMemoryBytes(
                  *input.materialized_output_batch,
                  &input_payload_bytes) ||
              input_payload_bytes == 0 ||
              input_payload_bytes - 1 >
                  std::numeric_limits<std::uint64_t>::max() -
                      retained_input_payload_bytes) {
            step.diagnostic.ok = false;
            step.diagnostic.diagnostic_code =
                "QOW-DIAG-QRY-014-RESOURCE-REFUSAL-V1";
            step.diagnostic.detail =
                "recursive CTE retained input payload accounting overflowed";
            return step;
          }
          if (input_index == 0) {
            anchor_input_payload_bytes = input_payload_bytes - 1;
          }
          retained_input_payload_bytes += input_payload_bytes - 1;
        }
        if (retained_input_payload_bytes >
            std::numeric_limits<std::size_t>::max()) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "QOW-DIAG-QRY-014-RESOURCE-REFUSAL-V1";
          step.diagnostic.detail =
              "recursive CTE retained input payload exceeds the native "
              "receipt width";
          return step;
        }

        const auto recursive_term_node = std::ranges::find_if(
            execution_dag->nodes, [&](const auto& candidate) {
              return node.input_physical_node_ids.size() == 2 &&
                     candidate.physical_node_id ==
                         node.input_physical_node_ids[1];
            });
        if (recursive_term_node == execution_dag->nodes.end() ||
            !LiveRecursiveCteTermNodeBound(
                prepared.term, *recursive_term_node,
                recursive_term_capability_uuid)) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "QOW-DIAG-RELATIONAL-LIVE-RECURSIVE-CTE-INPUT-V1";
          step.diagnostic.detail =
              "recursive CTE iterative term binding drifted";
          return step;
        }
        const auto cancellation_policy = std::ranges::find_if(
            execution_dag->admission_evidence,
            [](const exec::PhysicalAdmissionEvidence& evidence) {
              return evidence.stage ==
                     exec::PhysicalAdmissionStage::kPolicyCapability;
            });
        const auto cancellation_policy_count = std::ranges::count_if(
            execution_dag->admission_evidence,
            [](const exec::PhysicalAdmissionEvidence& evidence) {
              return evidence.stage ==
                     exec::PhysicalAdmissionStage::kPolicyCapability;
            });
        if (cancellation_policy_count != 1 ||
            cancellation_policy == execution_dag->admission_evidence.end() ||
            cancellation_policy->evidence_uuid.empty()) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "QOW-DIAG-RELATIONAL-LIVE-RECURSIVE-CTE-INPUT-V1";
          step.diagnostic.detail =
              "recursive CTE cancellation policy evidence is missing";
          return step;
        }
        const auto resource_evidence = std::ranges::find_if(
            execution_dag->admission_evidence,
            [](const exec::PhysicalAdmissionEvidence& evidence) {
              return evidence.stage ==
                     exec::PhysicalAdmissionStage::kResource;
            });
        const auto resource_evidence_count = std::ranges::count_if(
            execution_dag->admission_evidence,
            [](const exec::PhysicalAdmissionEvidence& evidence) {
              return evidence.stage ==
                     exec::PhysicalAdmissionStage::kResource;
            });
        if (resource_evidence_count != 1 ||
            resource_evidence == execution_dag->admission_evidence.end() ||
            resource_evidence->evidence_uuid.empty() ||
            prepared.planned_peak_memory_bytes == 0 ||
            prepared.planned_resident_structural_bytes == 0 ||
            node.memory_bytes_required !=
                prepared.planned_peak_memory_bytes ||
            node.memory_bytes_required > execution_dag->memory_budget_bytes ||
            retained_input_payload_bytes >
                node.memory_bytes_required ||
            anchor_input_payload_bytes >
                node.memory_bytes_required -
                    retained_input_payload_bytes ||
            (node.retained_cost.memory_bytes_required != 0 &&
             node.retained_cost.memory_bytes_required !=
                 node.memory_bytes_required)) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "QOW-DIAG-QRY-014-RESOURCE-REFUSAL-V1";
          step.diagnostic.detail =
              "recursive CTE selected-node memory grant is not exactly bound";
          return step;
        }
        const auto memory_grant_evidence_uuid =
            resource_evidence->evidence_uuid;
        const auto cancellation_evidence_uuid =
            cancellation_policy->evidence_uuid;
        const auto cancellation_requested =
            mga_context.query_cancellation_requested
                ? mga_context.query_cancellation_requested
                : std::function<bool()>([] { return false; });
        struct RecursiveCancellationState {
          bool cancellation_observed = false;
          std::exception_ptr probe_failure;
        };
        const auto recursive_cancellation_state =
            std::make_shared<RecursiveCancellationState>();
        const exec::CanonicalRecursiveCteCancellationProbe
            recursive_cancellation_requested =
                [cancellation_requested, recursive_cancellation_state](
                    const std::size_t) {
                  if (recursive_cancellation_state->probe_failure) {
                    std::rethrow_exception(
                        recursive_cancellation_state->probe_failure);
                  }
                  if (recursive_cancellation_state
                          ->cancellation_observed) {
                    return true;
                  }
                  try {
                    recursive_cancellation_state->cancellation_observed =
                        cancellation_requested();
                  } catch (...) {
                    recursive_cancellation_state->probe_failure =
                        std::current_exception();
                    throw;
                  }
                  return recursive_cancellation_state
                      ->cancellation_observed;
                };
        const auto recursive_memory_state =
            std::make_shared<exec::CanonicalRecursiveCteMemoryState>();

        exec::DescriptorBatch output;
        exec::DescriptorRuntimeDiagnostic diagnostic;
        exec::PhysicalMgaStatementContext result_mga;
        std::size_t recursive_iteration_count = 0;
        std::size_t output_payload_bytes = 0;
        std::size_t peak_live_payload_bytes = 0;
        std::size_t resident_structural_bytes = 0;
        std::size_t current_live_memory_bytes = 0;
        std::size_t peak_live_memory_bytes = 0;
        std::size_t memory_grant_bytes = 0;
        std::string result_memory_grant_evidence_uuid;
        if (prepared.profile.search_cycle) {
          exec::CanonicalRecursiveCteSearchCycleRequest recursive;
          recursive.selected_physical_node_id = node.physical_node_id;
          recursive.anchor_batch =
              *inputs[0].materialized_output_batch;
          recursive.search_order =
              exec::CanonicalRecursiveCteSearchOrder::kBreadthFirst;
          recursive.cycle_key_column = 0;
          recursive.cycle_key_expression_descriptor_id =
              recursive.anchor_batch.columns.front().descriptor_id;
          recursive.search_sequence_column =
              prepared.search_sequence_column;
          recursive.cycle_mark_column = prepared.cycle_mark_column;
          recursive.maximum_iteration_count =
              std::max<std::size_t>(1, prepared.maximum_iteration_count);
          recursive.maximum_working_row_count =
              std::max<std::size_t>(1, prepared.maximum_working_row_count);
          recursive.maximum_recursive_output_row_count =
              std::max<std::size_t>(
                  1, prepared.maximum_term_output_row_count);
          recursive.maximum_result_row_count =
              std::max<std::size_t>(1, prepared.maximum_result_row_count);
          recursive.maximum_value_comparison_count =
              std::max<std::size_t>(
                  1, prepared.maximum_value_comparison_count);
          recursive.enforce_payload_memory_grant = true;
          recursive.memory_state = recursive_memory_state;
          recursive.retained_input_payload_bytes =
              static_cast<std::size_t>(retained_input_payload_bytes);
          recursive.cancellation_requested =
              recursive_cancellation_requested;
          recursive.cancellation_evidence_uuid =
              cancellation_evidence_uuid;
          recursive.recursive_step =
              [term = &prepared.term,
               maximum_output_row_count =
                   prepared.maximum_term_output_row_count,
               recursive_memory_state,
               recursive_cancellation_state,
               recursive_cancellation_requested](
                  const exec::DescriptorBatch& current,
                  const std::size_t iteration) {
                auto executed = ExecutePreparedRecursiveCteTerm(
                    *term, current, iteration,
                    maximum_output_row_count,
                    recursive_cancellation_requested);
                if (executed.cancellation_probe_failure) {
                  recursive_cancellation_state->probe_failure =
                      executed.cancellation_probe_failure;
                  return exec::CanonicalRecursiveCteGeneratedBatch{};
                }
                if (executed.cancellation_observed) {
                  recursive_cancellation_state->cancellation_observed = true;
                  return exec::CanonicalRecursiveCteGeneratedBatch{};
                }
                if (executed.resource_refused) {
                  recursive_memory_state->refusal_detail = executed.detail;
                  throw std::runtime_error(executed.detail);
                }
                if (!executed.ok) {
                  throw std::runtime_error(executed.detail);
                }
                return std::move(executed.generated);
              };
          recursive.mga_authority = BuildCanonicalExecutionMgaAuthority(
              mga_context, *execution_dag);
          auto result =
              exec::ExecuteCanonicalRecursiveCteSearchCycle(
                  recursive, *execution_dag, node.physical_node_id);
          if (result.diagnostic.ok &&
              (!CanonicalOperatorExecutionReceiptMatches(
                   result, *execution_dag, node,
                   recursive.mga_authority.statement_context) ||
               !result.converged || !result.working_state_cleaned ||
               result.cancellation_observed)) {
            step.diagnostic.ok = false;
            step.diagnostic.diagnostic_code =
                "QOW-DIAG-QRY-014-SEARCH-CYCLE-REFUSAL-V1";
            step.diagnostic.detail =
                "recursive CTE SEARCH/CYCLE execution receipt changed";
            return step;
          }
          diagnostic = std::move(result.diagnostic);
          output = std::move(result.output_batch);
          result_mga = std::move(result.mga_statement_context);
          recursive_iteration_count = result.recursive_iteration_count;
          output_payload_bytes = result.output_payload_bytes;
          peak_live_payload_bytes = result.peak_live_payload_bytes;
          resident_structural_bytes =
              result.resident_structural_bytes;
          current_live_memory_bytes =
              result.current_live_memory_bytes;
          peak_live_memory_bytes = result.peak_live_memory_bytes;
          memory_grant_bytes = result.memory_grant_bytes;
          result_memory_grant_evidence_uuid =
              std::move(result.memory_grant_evidence_uuid);
          step.cancellation_observed = result.cancellation_observed;
          step.transient_state_cleanup_proven =
              result.working_state_cleaned;
          step.cancellation_evidence_uuid =
              result.cancellation_evidence_uuid;
        } else {
          exec::CanonicalRecursiveCteUnionRequest recursive;
          recursive.union_mode = prepared.profile.union_mode;
          recursive.maximum_value_comparison_count =
              std::max<std::size_t>(
                  1, prepared.maximum_value_comparison_count);
          auto& working = recursive.working_request;
          working.selected_physical_node_id = node.physical_node_id;
          working.anchor_batch = *inputs[0].materialized_output_batch;
          working.maximum_iteration_count =
              std::max<std::size_t>(1, prepared.maximum_iteration_count);
          working.maximum_working_row_count =
              std::max<std::size_t>(1, prepared.maximum_working_row_count);
          working.maximum_recursive_output_row_count =
              std::max<std::size_t>(
                  1, prepared.maximum_term_output_row_count);
          working.maximum_result_row_count =
              std::max<std::size_t>(1, prepared.maximum_result_row_count);
          working.enforce_payload_memory_grant = true;
          working.memory_state = recursive_memory_state;
          working.retained_input_payload_bytes =
              static_cast<std::size_t>(retained_input_payload_bytes);
          working.cancellation_requested =
              recursive_cancellation_requested;
          working.cancellation_evidence_uuid =
              cancellation_evidence_uuid;
          working.recursive_step =
              [term = &prepared.term,
               maximum_output_row_count =
                   prepared.maximum_term_output_row_count,
               recursive_memory_state,
               recursive_cancellation_state,
               recursive_cancellation_requested](
                  const exec::DescriptorBatch& current,
                  const std::size_t iteration) {
                auto executed = ExecutePreparedRecursiveCteTerm(
                    *term, current, iteration,
                    maximum_output_row_count,
                    recursive_cancellation_requested);
                if (executed.cancellation_probe_failure) {
                  recursive_cancellation_state->probe_failure =
                      executed.cancellation_probe_failure;
                  return exec::DescriptorBatch{};
                }
                if (executed.cancellation_observed) {
                  recursive_cancellation_state->cancellation_observed = true;
                  return exec::DescriptorBatch{};
                }
                if (executed.resource_refused) {
                  recursive_memory_state->refusal_detail = executed.detail;
                  throw std::runtime_error(executed.detail);
                }
                if (!executed.ok) {
                  throw std::runtime_error(executed.detail);
                }
                return std::move(executed.generated.batch);
              };
          working.mga_authority = BuildCanonicalExecutionMgaAuthority(
              mga_context, *execution_dag);
          auto result =
              exec::ExecuteCanonicalRecursiveCteUnion(
                  recursive, *execution_dag, node.physical_node_id);
          if (result.working_result.diagnostic.ok &&
              (!CanonicalOperatorExecutionReceiptMatches(
                   result.working_result, *execution_dag, node,
                   working.mga_authority.statement_context) ||
               !exec::PhysicalMgaStatementContextEqual(
                   result.mga_statement_context,
                   working.mga_authority.statement_context) ||
               !result.working_result.converged ||
               !result.working_result.working_state_cleaned ||
               result.working_result.cancellation_observed ||
               result.union_mode != recursive.union_mode)) {
            step.diagnostic.ok = false;
            step.diagnostic.diagnostic_code =
                "QOW-DIAG-QRY-014-UNION-REFUSAL-V1";
            step.diagnostic.detail =
                "recursive CTE UNION execution receipt changed";
            return step;
          }
          diagnostic = std::move(result.working_result.diagnostic);
          output = std::move(result.working_result.output_batch);
          result_mga = std::move(result.mga_statement_context);
          recursive_iteration_count =
              result.working_result.recursive_iteration_count;
          output_payload_bytes =
              result.working_result.output_payload_bytes;
          peak_live_payload_bytes =
              result.working_result.peak_live_payload_bytes;
          resident_structural_bytes =
              result.working_result.resident_structural_bytes;
          current_live_memory_bytes =
              result.working_result.current_live_memory_bytes;
          peak_live_memory_bytes =
              result.working_result.peak_live_memory_bytes;
          memory_grant_bytes =
              result.working_result.memory_grant_bytes;
          result_memory_grant_evidence_uuid =
              std::move(result.working_result
                            .memory_grant_evidence_uuid);
          step.cancellation_observed =
              result.working_result.cancellation_observed;
          step.transient_state_cleanup_proven =
              result.working_result.working_state_cleaned;
          step.cancellation_evidence_uuid =
              result.working_result.cancellation_evidence_uuid;
        }
        if (!diagnostic.ok &&
            diagnostic.diagnostic_code ==
                "QOW-DIAG-QRY-014-CANCELLATION-PROBE-V1") {
          diagnostic.diagnostic_code =
              "QOW-DIAG-QRY-004-PHYSICAL-CANCELLATION-PROBE-V1";
        }
        if (!diagnostic.ok) {
          step.diagnostic = std::move(diagnostic);
          return step;
        }
        std::uint64_t measured_output_payload_bytes = 0;
        if (!RuntimeMaterializedBatchMemoryBytes(
                output, &measured_output_payload_bytes) ||
            measured_output_payload_bytes == 0 ||
            output_payload_bytes != measured_output_payload_bytes - 1 ||
            memory_grant_bytes != node.memory_bytes_required ||
            result_memory_grant_evidence_uuid !=
                memory_grant_evidence_uuid ||
            resident_structural_bytes !=
                prepared.planned_resident_structural_bytes ||
            resident_structural_bytes >
                std::numeric_limits<std::size_t>::max() -
                    output_payload_bytes ||
            current_live_memory_bytes <
                resident_structural_bytes + output_payload_bytes ||
            peak_live_payload_bytes < output_payload_bytes ||
            peak_live_payload_bytes >
                prepared.planned_peak_payload_bytes ||
            peak_live_payload_bytes > memory_grant_bytes ||
            peak_live_memory_bytes < current_live_memory_bytes ||
            resident_structural_bytes >
                std::numeric_limits<std::size_t>::max() -
                    peak_live_payload_bytes ||
            peak_live_memory_bytes !=
                resident_structural_bytes + peak_live_payload_bytes ||
            peak_live_memory_bytes > memory_grant_bytes) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "QOW-DIAG-QRY-014-RESOURCE-REFUSAL-V1";
          step.diagnostic.detail =
              "recursive CTE runtime resident-memory receipt is inconsistent";
          step.transient_state_cleanup_proven = true;
          return step;
        }
        bool output_validation_cancelled = false;
        const auto validated = exec::ValidateCanonicalDescriptorBatch(
            output, node.output_descriptor_ids,
            [&]() {
              return recursive_cancellation_requested(
                  recursive_iteration_count);
            },
            &output_validation_cancelled);
        if (output_validation_cancelled ||
            recursive_cancellation_state->probe_failure) {
          step.diagnostic = validated;
          step.diagnostic.diagnostic_code =
              recursive_cancellation_state->probe_failure
                  ? "QOW-DIAG-QRY-004-PHYSICAL-CANCELLATION-PROBE-V1"
                  : "QOW-DIAG-QRY-004-PHYSICAL-DISPATCH-CANCELLED-V1";
          step.cancellation_observed =
              !recursive_cancellation_state->probe_failure;
          step.transient_state_cleanup_proven = true;
          step.cancellation_evidence_uuid = cancellation_evidence_uuid;
          return step;
        }
        if (!validated.ok ||
            output.rows.size() > prepared.maximum_result_row_count) {
          step.diagnostic = validated;
          if (validated.ok) {
            step.diagnostic.ok = false;
            step.diagnostic.diagnostic_code =
                "QOW-DIAG-RELATIONAL-LIVE-RECURSIVE-CTE-INPUT-V1";
            step.diagnostic.detail =
                "recursive CTE result exceeded its planned bound";
          }
          return step;
        }
        step.result_handle_id = node.physical_node_id;
        step.input_row_count =
            inputs[0].materialized_output_batch->rows.size();
        step.rows_examined = prepared.rows_examined;
        step.output_row_count = output.rows.size();
        step.materialized_output_batch = std::move(output);
        step.mga_statement_context = std::move(result_mga);
        step.data_access_observation_known = true;
        step.data_access_observed = false;
        PublishRuntimeMemoryObservation(
            &step, current_live_memory_bytes, peak_live_memory_bytes);
        return step;
      };
  return registration;
}


// SEARCH_KEY: SB_ENGINE_CANONICAL_QUERY_RECURSIVE_REGISTRATION_AUTHORITY
PreparedRecursiveCteTerm PrepareLiveRecursiveCteTerm(
    const LiveRecursiveCteProfile& profile,
    std::vector<exec::ExecutorColumnDescriptor> columns,
    const std::int64_t upper_bound) {
  PreparedRecursiveCteTerm term;
  term.mode = profile.search_cycle
                  ? LiveRecursiveCteTermMode::kIncrementWrapToOne
                  : profile.emit_current_duplicate
                        ? LiveRecursiveCteTermMode::kBoundedIncrementWithCurrent
                        : LiveRecursiveCteTermMode::kBoundedIncrement;
  term.columns = std::move(columns);
  term.upper_bound = upper_bound;
  return term;
}

bool LiveRecursiveCteTermNodeBound(
    const PreparedRecursiveCteTerm& prepared,
    const exec::PhysicalNodeRecord& node,
    const std::string_view capability_uuid) {
  if (node.node_kind != exec::PhysicalNodeKind::kCte ||
      node.implementation_id !=
          "cte.recursive-term.int64-increment.typed.v1" ||
      node.logical_semantic_variant_id !=
          "cte.recursive-term-int64-increment.v1" ||
      node.executor_capability_uuid != capability_uuid ||
      node.executor_capability_abi_version != 1 ||
      !node.engine_capability_validated ||
      !node.input_physical_node_ids.empty() ||
      node.output_descriptor_ids.size() != prepared.columns.size()) {
    return false;
  }
  for (std::size_t column = 0; column < prepared.columns.size(); ++column) {
    if (node.output_descriptor_ids[column] !=
        prepared.columns[column].descriptor_id) {
      return false;
    }
  }
  return true;
}

LiveRecursiveCteTermExecution ExecutePreparedRecursiveCteTerm(
    const PreparedRecursiveCteTerm& prepared,
    const exec::DescriptorBatch& current,
    const std::size_t iteration,
    const std::size_t maximum_output_row_count,
    const exec::CanonicalRecursiveCteCancellationProbe&
        cancellation_requested) {
  LiveRecursiveCteTermExecution result;
  const auto refuse = [&](std::string detail) {
    result = {};
    result.detail = std::move(detail);
    return result;
  };
  const auto refuse_resource = [&](std::string detail) {
    result = {};
    result.resource_refused = true;
    result.detail = std::move(detail);
    return result;
  };
  if (prepared.columns.size() != 1 ||
      prepared.columns.front().descriptor.canonical_type_name != "int64" ||
      prepared.upper_bound < 0 || maximum_output_row_count == 0 ||
      current.columns.size() != prepared.columns.size()) {
    return refuse("recursive CTE term contract is not one bounded int64 column");
  }
  for (std::size_t column = 0; column < prepared.columns.size(); ++column) {
    const auto& expected = prepared.columns[column];
    const auto& actual = current.columns[column];
    if (actual.descriptor_id != expected.descriptor_id ||
        actual.nullable != expected.nullable ||
        actual.descriptor.descriptor_uuid.canonical !=
            expected.descriptor.descriptor_uuid.canonical ||
        actual.descriptor.descriptor_kind !=
            expected.descriptor.descriptor_kind ||
        actual.descriptor.canonical_type_name !=
            expected.descriptor.canonical_type_name ||
        actual.descriptor.encoded_descriptor !=
            expected.descriptor.encoded_descriptor) {
      return refuse("recursive CTE term input descriptor drifted");
    }
  }
  try {
    result.generated.batch.columns = prepared.columns;
    result.generated.batch.rows.reserve(maximum_output_row_count);
    if (prepared.mode == LiveRecursiveCteTermMode::kIncrementWrapToOne) {
      result.generated.parent_working_row_indices.reserve(
          maximum_output_row_count);
    }
    const auto poll_cancellation = [&]() {
      if (!cancellation_requested) return false;
      try {
        return cancellation_requested(iteration);
      } catch (...) {
        result.cancellation_probe_failure = std::current_exception();
        return true;
      }
    };
    const auto append = [&](exec::DescriptorTuple row,
                            const std::size_t parent_row) {
      if (result.generated.batch.rows.size() == maximum_output_row_count) {
        result.detail = "recursive CTE term output row bound was exceeded";
        return false;
      }
      result.generated.batch.rows.push_back(std::move(row));
      if (prepared.mode == LiveRecursiveCteTermMode::kIncrementWrapToOne) {
        result.generated.parent_working_row_indices.push_back(parent_row);
      }
      return true;
    };

    for (std::size_t row = 0; row < current.rows.size(); ++row) {
      if (poll_cancellation()) {
        result.cancellation_observed =
            result.cancellation_probe_failure == nullptr;
        return result;
      }
      if (current.rows[row].values.size() != 1) {
        return refuse("recursive CTE term received a ragged row");
      }
      const auto& value = current.rows[row].values.front();
      if (prepared.mode ==
              LiveRecursiveCteTermMode::kBoundedIncrementWithCurrent &&
          !append(current.rows[row], row)) {
        return result;
      }
      if (value.state == api::EngineValueState::sql_null || value.is_null) {
        continue;
      }
      const auto decoded = exec::DecodeInt64Value(value);
      if (!decoded.ok()) {
        return refuse(decoded.diagnostic.diagnostic_code + ":" +
                      decoded.diagnostic.detail);
      }
      if (prepared.mode != LiveRecursiveCteTermMode::kIncrementWrapToOne &&
          decoded.value >= prepared.upper_bound) {
        continue;
      }
      api::EngineTypedValue next;
      next.descriptor = value.descriptor;
      next.encoded_value = std::to_string(
          prepared.mode == LiveRecursiveCteTermMode::kIncrementWrapToOne &&
                  decoded.value >= prepared.upper_bound
              ? 1
              : decoded.value + 1);
      next.state = api::EngineValueState::value;
      if (!append({{std::move(next)}}, row)) return result;
    }
    if (poll_cancellation()) {
      result.cancellation_observed =
          result.cancellation_probe_failure == nullptr;
      return result;
    }
    std::vector<std::uint32_t> descriptor_ids;
    descriptor_ids.reserve(prepared.columns.size());
    for (const auto& column : prepared.columns) {
      descriptor_ids.push_back(column.descriptor_id);
    }
    bool validation_cancelled = false;
    const auto validated = exec::ValidateCanonicalDescriptorBatch(
        result.generated.batch, descriptor_ids, poll_cancellation,
        &validation_cancelled);
    if (validation_cancelled || result.cancellation_probe_failure) {
      result.cancellation_observed =
          result.cancellation_probe_failure == nullptr;
      return result;
    }
    if (!validated.ok) {
      return refuse(validated.diagnostic_code + ":" + validated.detail);
    }
  } catch (const std::bad_alloc&) {
    return refuse_resource(
        "recursive CTE term retained allocation was refused");
  } catch (const std::length_error&) {
    return refuse_resource(
        "recursive CTE term retained capacity exceeds the container limit");
  }
  result.ok = true;
  return result;
}

exec::CanonicalPhysicalExecutorRegistration
MakeLiveRecursiveCteTermRegistration(
    PreparedRecursiveCteTerm prepared,
    std::string capability_uuid,
    api::EngineRequestContext mga_context) {
  exec::CanonicalPhysicalExecutorRegistration registration;
  registration.node_kind = exec::PhysicalNodeKind::kCte;
  registration.implementation_id =
      "cte.recursive-term.int64-increment.typed.v1";
  registration.executor_capability_uuid = std::move(capability_uuid);
  registration.executor_capability_abi_version = 1;
  registration.engine_owned = true;
  registration.accepts_optimizer_publication_v2 = true;
  registration.execute =
      [prepared = std::move(prepared),
       capability_uuid = registration.executor_capability_uuid,
       mga_context = std::move(mga_context)](
          const exec::TypedPhysicalNodeDag& dag,
          const exec::PhysicalNodeRecord& node,
          const std::vector<exec::CanonicalPhysicalDispatchInput>& inputs) {
        exec::CanonicalPhysicalDispatchStepResult step;
        step.selected_plan_uuid = dag.selected_plan_uuid;
        step.mga_statement_context = dag.mga_statement_context;
        step.executed_physical_node_id = node.physical_node_id;
        step.causal_counter_id = node.causal_counter_id;
        step.output_descriptor_ids = node.output_descriptor_ids;
        step.authority.engine_mga_snapshot_bound = true;
        if (!inputs.empty() ||
            !LiveRecursiveCteTermNodeBound(
                prepared, node, capability_uuid)) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "QOW-DIAG-RELATIONAL-LIVE-RECURSIVE-CTE-INPUT-V1";
          step.diagnostic.detail =
              "recursive CTE term binding did not preserve its schema";
          return step;
        }
        const exec::TypedPhysicalNodeDag* execution_dag = &dag;
        std::optional<exec::TypedPhysicalNodeDag> scoped_execution_dag;
        if (node.physical_node_id != dag.root_physical_node_id) {
          scoped_execution_dag.emplace();
          std::string scope_detail;
          if (!BuildOperatorLocalPhysicalDag(
                  dag, node.physical_node_id, &*scoped_execution_dag,
                  &scope_detail)) {
            step.diagnostic.ok = false;
            step.diagnostic.diagnostic_code =
                "QOW-DIAG-RELATIONAL-LIVE-RECURSIVE-CTE-INPUT-V1";
            step.diagnostic.detail =
                "recursive CTE term execution view is unresolved";
            return step;
          }
          execution_dag = &*scoped_execution_dag;
        }
        exec::DescriptorBatch output;
        output.columns = prepared.columns;
        const auto validated = exec::ValidateCanonicalDescriptorBatch(
            output, node.output_descriptor_ids);
        const auto authority =
            BuildCanonicalExecutionMgaAuthority(mga_context, *execution_dag);
        const auto revalidated =
            exec::RevalidateCanonicalExecutionMgaAuthority(
                authority, *execution_dag);
        if (!validated.ok || !revalidated.ok) {
          step.diagnostic = validated.ok ? revalidated : validated;
          return step;
        }
        step.result_handle_id = node.physical_node_id;
        step.materialized_output_batch = std::move(output);
        step.mga_statement_context = authority.statement_context;
        return step;
      };
  return registration;
}

}  // namespace scratchbird::engine::sblr
