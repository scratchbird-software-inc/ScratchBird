// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "canonical_query_join_registration.hpp"

#include "canonical_query_physical_registration.hpp"
#include "canonical_query_predicate_support.hpp"
#include "canonical_query_runtime_memory_support.hpp"

#include <algorithm>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <utility>

namespace scratchbird::engine::sblr {

namespace api = scratchbird::engine::internal_api;

// SEARCH_KEY: SB_ENGINE_CANONICAL_QUERY_JOIN_REGISTRATION_AUTHORITY
exec::CanonicalPhysicalExecutorRegistration MakeLiveJoinRegistration(
    std::string implementation_id,
    std::string capability_uuid,
    std::vector<api::EngineSqlTruthValue> predicate_truth_values,
    const std::size_t pair_count,
    const std::size_t output_row_bound,
    const exec::CanonicalAcceptedJoinKind join_kind,
    std::string operation_name,
    api::EngineRequestContext mga_context,
    const bool runtime_bounded_inputs,
    const std::uint32_t runtime_predicate_expression_id,
    CanonicalRelationalExpressionRowBinding runtime_predicate_binding,
    api::TypedRelationalDag runtime_relational_dag,
    CanonicalRelationalExpressionRuntimeServices
        runtime_expression_services,
    std::vector<LiveJoinRuntimeNodeConfiguration>
        runtime_node_configurations,
    std::optional<exec::CanonicalExecutionMgaAuthority>
        runtime_mga_authority,
    const api::TypedRelationalDag* borrowed_runtime_relational_dag,
    const api::EngineRequestContext* borrowed_mga_context,
    const exec::CanonicalExecutionMgaAuthority* borrowed_runtime_mga_authority) {
  std::shared_ptr<const api::TypedRelationalDag> owned_runtime_relational_dag;
  const api::TypedRelationalDag* active_runtime_relational_dag =
      borrowed_runtime_relational_dag;
  if (runtime_bounded_inputs && active_runtime_relational_dag == nullptr) {
    owned_runtime_relational_dag =
        std::make_shared<const api::TypedRelationalDag>(
            std::move(runtime_relational_dag));
    active_runtime_relational_dag = owned_runtime_relational_dag.get();
  }
  std::uint64_t registration_retained_bytes =
      sizeof(std::vector<api::EngineSqlTruthValue>) +
      sizeof(std::string) + sizeof(api::EngineRequestContext) +
      sizeof(CanonicalRelationalExpressionRowBinding) +
      sizeof(CanonicalRelationalExpressionRuntimeServices) +
      sizeof(std::shared_ptr<const api::TypedRelationalDag>) +
      sizeof(std::vector<LiveJoinRuntimeNodeConfiguration>) +
      sizeof(std::optional<exec::CanonicalExecutionMgaAuthority>) +
      7 * sizeof(void*);
  const auto account_registration_array = [&](const std::size_t count,
                                               const std::size_t width) {
    std::uint64_t bytes = 0;
    return (count == 0 ||
            (width <= std::numeric_limits<std::uint64_t>::max() / count &&
             (bytes = static_cast<std::uint64_t>(count * width), true))) &&
           CheckedAdd(registration_retained_bytes, bytes,
                      &registration_retained_bytes);
  };
  const auto account_registration_string = [&](const std::string& value) {
    return value.capacity() != std::numeric_limits<std::size_t>::max() &&
           CheckedAdd(registration_retained_bytes,
                      static_cast<std::uint64_t>(value.capacity()) + 1,
                      &registration_retained_bytes);
  };
  bool registration_memory_bounded =
      borrowed_runtime_relational_dag != nullptr &&
      account_registration_array(predicate_truth_values.capacity(),
                                 sizeof(api::EngineSqlTruthValue)) &&
      account_registration_string(operation_name) &&
      account_registration_array(
          runtime_predicate_binding.row_descriptor_ids.capacity(),
          sizeof(std::uint32_t)) &&
      account_registration_array(
          (runtime_predicate_binding.row_nullable.capacity() + 63) / 64,
          sizeof(std::uint64_t)) &&
      account_registration_array(runtime_predicate_binding.slots.capacity(),
                                 sizeof(CanonicalRelationalExpressionRowSlotBinding)) &&
      account_registration_array(runtime_node_configurations.capacity(),
                                 sizeof(LiveJoinRuntimeNodeConfiguration));
  for (const auto& configuration : runtime_node_configurations) {
    registration_memory_bounded =
        registration_memory_bounded &&
        account_registration_string(configuration.operation_name) &&
        account_registration_array(
            configuration.predicate_binding.row_descriptor_ids.capacity(),
            sizeof(std::uint32_t)) &&
        account_registration_array(
            (configuration.predicate_binding.row_nullable.capacity() + 63) /
                64,
            sizeof(std::uint64_t)) &&
        account_registration_array(configuration.predicate_binding.slots.capacity(),
                                   sizeof(CanonicalRelationalExpressionRowSlotBinding));
  }
  if (borrowed_mga_context == nullptr) {
    registration_memory_bounded = false;
  }
  if (!registration_memory_bounded) registration_retained_bytes = 0;
  exec::CanonicalPhysicalExecutorRegistration registration;
  registration.node_kind = exec::PhysicalNodeKind::kJoin;
  registration.implementation_id = std::move(implementation_id);
  registration.executor_capability_uuid = std::move(capability_uuid);
  registration.executor_capability_abi_version = 1;
  registration.engine_owned = true;
  registration.accepts_optimizer_publication_v2 = true;
  registration.honors_dispatcher_memory_limit_v1 = true;
  registration.retained_live_memory_bytes_v1 = registration_retained_bytes;
  registration.execute =
      [predicate_truth_values = std::move(predicate_truth_values), pair_count,
       output_row_bound, join_kind, operation_name = std::move(operation_name),
       mga_context = std::move(mga_context), runtime_bounded_inputs,
       runtime_predicate_expression_id,
       runtime_predicate_binding = std::move(runtime_predicate_binding),
       runtime_expression_services = std::move(runtime_expression_services),
       owned_runtime_relational_dag =
           std::move(owned_runtime_relational_dag),
       active_runtime_relational_dag,
       runtime_node_configurations =
           std::move(runtime_node_configurations),
       runtime_mga_authority = std::move(runtime_mga_authority),
       borrowed_mga_context, borrowed_runtime_mga_authority](
          const exec::TypedPhysicalNodeDag& dag,
          const exec::PhysicalNodeRecord& node,
          const std::vector<exec::CanonicalPhysicalDispatchInput>& inputs) {
        exec::CanonicalPhysicalDispatchStepResult step;
        const auto selected_node_memory_bound =
            SelectedNodeAggregateMemoryBound(dag, node);
        const auto operator_dag_copy_memory_bound =
            BoundOperatorLocalPhysicalDagCopyMemoryBytes(dag);
        if (!selected_node_memory_bound.has_value() ||
            !operator_dag_copy_memory_bound.has_value() ||
            *operator_dag_copy_memory_bound >
                *selected_node_memory_bound) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "SBLR.PLAN_TREE.RESOURCE_LIMIT";
          step.diagnostic.detail =
              "live join operator-local DAG exceeds the callback memory allowance";
          return step;
        }
        step.executed_physical_node_id = node.physical_node_id;
        step.causal_counter_id = node.causal_counter_id;
        const LiveJoinRuntimeNodeConfiguration* runtime_node_configuration =
            nullptr;
        for (const auto& candidate : runtime_node_configurations) {
          if (candidate.relational_node_id != node.relational_node_id) {
            continue;
          }
          if (runtime_node_configuration != nullptr) {
            step.diagnostic.ok = false;
            step.diagnostic.diagnostic_code =
                "QOW-DIAG-RELATIONAL-LIVE-JOIN-INPUT-V1";
            step.diagnostic.detail =
                "live join executor has duplicate logical-node configuration";
            return step;
          }
          runtime_node_configuration = &candidate;
        }
        if (!runtime_node_configurations.empty() &&
            runtime_node_configuration == nullptr) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "QOW-DIAG-RELATIONAL-LIVE-JOIN-INPUT-V1";
          step.diagnostic.detail =
              "live join executor has no logical-node configuration";
          return step;
        }
        const auto active_join_kind =
            runtime_node_configuration == nullptr
                ? join_kind
                : runtime_node_configuration->join_kind;
        const auto& active_operation_name =
            runtime_node_configuration == nullptr
                ? operation_name
                : runtime_node_configuration->operation_name;
        const auto active_predicate_expression_id =
            runtime_node_configuration == nullptr
                ? runtime_predicate_expression_id
                : runtime_node_configuration->predicate_expression_id;
        const auto& active_predicate_binding =
            runtime_node_configuration == nullptr
                ? runtime_predicate_binding
                : runtime_node_configuration->predicate_binding;
        const auto& active_mga_context =
            borrowed_mga_context == nullptr ? mga_context
                                            : *borrowed_mga_context;
        if (inputs.size() != 2 || node.input_physical_node_ids.size() != 2 ||
            node.input_physical_node_ids[0] ==
                node.input_physical_node_ids[1] ||
            inputs[0].physical_node_id !=
                node.input_physical_node_ids[0] ||
            inputs[1].physical_node_id !=
                node.input_physical_node_ids[1] ||
            !inputs[0].materialized_output_batch.has_value() ||
            !inputs[1].materialized_output_batch.has_value()) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "QOW-DIAG-RELATIONAL-LIVE-JOIN-INPUT-V1";
          step.diagnostic.detail = active_operation_name +
              " executor did not receive two typed input batches";
          return step;
        }
        const auto& left_batch = *inputs[0].materialized_output_batch;
        const auto& right_batch = *inputs[1].materialized_output_batch;
        if (left_batch.rows.size() != 0 &&
            right_batch.rows.size() >
                std::numeric_limits<std::size_t>::max() /
                    left_batch.rows.size()) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "QOW-DIAG-RELATIONAL-LIVE-JOIN-INPUT-V1";
          step.diagnostic.detail =
              active_operation_name + " pair cardinality overflowed";
          return step;
        }
        const auto actual_pair_count =
            left_batch.rows.size() * right_batch.rows.size();
        if ((!runtime_bounded_inputs && actual_pair_count != pair_count) ||
            (runtime_bounded_inputs && actual_pair_count > pair_count) ||
            right_batch.rows.size() >
                std::numeric_limits<std::size_t>::max() -
                    left_batch.rows.size()) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "QOW-DIAG-RELATIONAL-LIVE-JOIN-INPUT-V1";
          step.diagnostic.detail = active_operation_name +
              " input cardinality differs or overflows its selected cost";
          return step;
        }
        const exec::PhysicalAdmissionEvidence* cancellation_policy = nullptr;
        bool duplicate_cancellation_policy = false;
        for (const auto& evidence : dag.admission_evidence) {
          if (evidence.stage !=
              exec::PhysicalAdmissionStage::kPolicyCapability) {
            continue;
          }
          if (cancellation_policy != nullptr) {
            duplicate_cancellation_policy = true;
            break;
          }
          cancellation_policy = &evidence;
        }
        if (cancellation_policy == nullptr ||
            duplicate_cancellation_policy ||
            cancellation_policy->evidence_uuid.empty()) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "QOW-DIAG-RELATIONAL-LIVE-JOIN-CANCELLATION-POLICY-V1";
          step.diagnostic.detail = active_operation_name +
              " requires one exact cancellation policy evidence row";
          return step;
        }
        static const std::function<bool()> kNeverCancel = [] { return false; };
        const auto& cancellation_requested =
            active_mga_context.query_cancellation_requested
                ? active_mga_context.query_cancellation_requested
                : kNeverCancel;
        const auto poll_cancellation = [&](const char* phase) {
          try {
            if (!cancellation_requested()) return false;
            step.diagnostic.ok = false;
            step.diagnostic.diagnostic_code =
                "QOW-DIAG-QRY-012-JOIN-CANCELLED-V1";
            step.diagnostic.detail =
                std::string("live join cancellation observed ") + phase;
            step.cancellation_observed = true;
            step.transient_state_cleanup_proven = true;
            step.cancellation_evidence_uuid =
                cancellation_policy->evidence_uuid;
            return true;
          } catch (const std::exception& exception) {
            step.diagnostic.ok = false;
            step.diagnostic.diagnostic_code =
                "QOW-DIAG-QRY-004-PHYSICAL-CANCELLATION-PROBE-V1";
            step.diagnostic.detail =
                std::string("live join cancellation probe threw: ") +
                exception.what();
            step.transient_state_cleanup_proven = true;
            return true;
          } catch (...) {
            step.diagnostic.ok = false;
            step.diagnostic.diagnostic_code =
                "QOW-DIAG-QRY-004-PHYSICAL-CANCELLATION-PROBE-V1";
            step.diagnostic.detail =
                "live join cancellation probe threw a non-standard exception";
            step.transient_state_cleanup_proven = true;
            return true;
          }
        };
        exec::CanonicalJoinKindRequest join_request;
        auto& key_request = join_request.residual_request.key_request;
        if (runtime_bounded_inputs &&
            active_join_kind != exec::CanonicalAcceptedJoinKind::kCross &&
            active_predicate_expression_id == 0) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "QOW-DIAG-RELATIONAL-LIVE-JOIN-PREDICATE-V1";
          step.diagnostic.detail =
              active_operation_name + " runtime predicate identity is absent";
          return step;
        }
        std::uint64_t join_retained_state_bytes = 0;
        std::uint64_t preflight_retained_bytes = 2;
        std::uint64_t left_input_payload_bytes = 0;
        std::uint64_t right_input_payload_bytes = 0;
        std::uint64_t left_input_live_bytes = 0;
        std::uint64_t right_input_live_bytes = 0;
        const auto account_batch_payload =
            [&](const exec::DescriptorBatch& batch, const char* phase,
                std::uint64_t* batch_payload_bytes) {
              (void)phase;
              if (batch_payload_bytes == nullptr) return false;
              *batch_payload_bytes = 0;
              for (std::size_t row = 0; row < batch.rows.size(); ++row) {
                for (std::size_t column = 0;
                     column < batch.rows[row].values.size(); ++column) {
                  const auto& value = batch.rows[row].values[column];
                  if (value.encoded_value.capacity() ==
                          std::numeric_limits<std::size_t>::max() ||
                      !CheckedAdd(*batch_payload_bytes,
                                  value.encoded_value.capacity() + 1,
                                  batch_payload_bytes) ||
                      !CheckedAdd(*batch_payload_bytes,
                                  value.binary_value.capacity(),
                                  batch_payload_bytes)) {
                    return false;
                  }
                }
              }
              return true;
            };
        const auto largest_row_payload =
            [&](const exec::DescriptorBatch& batch,
                std::uint64_t* largest) {
              *largest = 0;
              for (std::size_t row = 0; row < batch.rows.size(); ++row) {
                std::uint64_t row_bytes = 0;
                if (!CheckedMultiply(batch.rows[row].values.capacity(),
                                     sizeof(api::EngineTypedValue),
                                     &row_bytes)) {
                  return false;
                }
                const auto add_row_string = [&](const std::string& value) {
                  return value.capacity() !=
                             std::numeric_limits<std::size_t>::max() &&
                         CheckedAdd(row_bytes, value.capacity() + 1,
                                    &row_bytes);
                };
                for (std::size_t column = 0;
                     column < batch.rows[row].values.size(); ++column) {
                  const auto& value = batch.rows[row].values[column];
                  if (!add_row_string(
                          value.descriptor.descriptor_uuid) ||
                      !add_row_string(value.descriptor.descriptor_kind) ||
                      !add_row_string(
                          value.descriptor.canonical_type_name) ||
                      !add_row_string(value.descriptor.encoded_descriptor) ||
                      !add_row_string(value.encoded_value) ||
                      !CheckedAdd(row_bytes, value.binary_value.capacity(),
                                  &row_bytes)) {
                    return false;
                  }
                }
                *largest = std::max(*largest, row_bytes);
              }
              return true;
            };
        std::uint64_t left_row_scratch = 0;
        std::uint64_t right_row_scratch = 0;
        std::uint64_t callback_carrier_bytes =
            3 * sizeof(std::vector<std::uint32_t>) +
            sizeof(std::vector<bool>);
        std::uint64_t callback_carrier_array_bytes = 0;
        const auto add_callback_carrier_array =
            [&](const std::size_t count, const std::size_t element_size) {
              return CheckedMultiply(count, element_size,
                                     &callback_carrier_array_bytes) &&
                     CheckedAdd(callback_carrier_bytes,
                                callback_carrier_array_bytes,
                                &callback_carrier_bytes);
            };
        const auto add_callback_carrier_string =
            [&](const std::string& value) {
              return value.capacity() !=
                         std::numeric_limits<std::size_t>::max() &&
                     CheckedAdd(callback_carrier_bytes,
                                value.capacity() + 1,
                                &callback_carrier_bytes);
            };
        const auto add_callback_mga_context =
            [&](const exec::PhysicalMgaStatementContext& context) {
              return add_callback_carrier_string(context.statement_uuid) &&
                     add_callback_carrier_string(
                         context.owning_transaction_uuid) &&
                     add_callback_carrier_string(
                         context.statement_snapshot_uuid) &&
                     add_callback_carrier_string(
                         context.statement_metadata_snapshot_uuid) &&
                     add_callback_carrier_array(
                         context.active_excluded_local_transaction_ids.size(),
                         sizeof(std::uint64_t)) &&
                     add_callback_carrier_array(
                         context.in_doubt_excluded_local_transaction_ids.size(),
                         sizeof(std::uint64_t)) &&
                     add_callback_carrier_string(context.snapshot_kind) &&
                     add_callback_carrier_string(context.statement_timestamp);
            };
        const auto callback_output_width = node.output_descriptor_ids.size();
        const bool callback_carriers_bounded =
            add_callback_carrier_array(callback_output_width,
                                       3 * sizeof(std::uint32_t)) &&
            // Conservative unpacked ceiling for the derived-nullability bits.
            add_callback_carrier_array(callback_output_width,
                                       sizeof(std::uint8_t)) &&
            add_callback_carrier_string(dag.selected_plan_uuid) &&
            add_callback_carrier_string(dag.selected_plan_uuid) &&
            // The callback step and the join result temporarily own separate
            // statement-context copies before the result is moved into step.
            add_callback_mga_context(dag.mga_statement_context) &&
            add_callback_mga_context(dag.mga_statement_context);
        bool preflight_ok =
            selected_node_memory_bound.has_value() &&
            callback_carriers_bounded &&
            CheckedAdd(preflight_retained_bytes, callback_carrier_bytes,
                       &preflight_retained_bytes) &&
            CheckedAdd(preflight_retained_bytes,
                       *operator_dag_copy_memory_bound,
                       &preflight_retained_bytes) &&
            BoundDescriptorBatchLiveMemoryBytes(
                left_batch, &left_input_live_bytes) &&
            BoundDescriptorBatchLiveMemoryBytes(
                right_batch, &right_input_live_bytes) &&
            CheckedAdd(preflight_retained_bytes, left_input_live_bytes,
                       &preflight_retained_bytes) &&
            CheckedAdd(preflight_retained_bytes, right_input_live_bytes,
                       &preflight_retained_bytes) &&
            exec::BoundCanonicalJoinRetainedStateBytes(
                left_batch.rows.size(), right_batch.rows.size(),
                &join_retained_state_bytes) &&
            account_batch_payload(
                left_batch, "while accounting left join input payload",
                &left_input_payload_bytes) &&
            account_batch_payload(
                right_batch, "while accounting right join input payload",
                &right_input_payload_bytes);
        std::uint64_t maximum_output_payload_bytes = 1;
        std::uint64_t maximum_output_structure_bytes =
            sizeof(exec::DescriptorBatch);
        std::uint64_t repeated_left_payload_bytes = 0;
        std::uint64_t repeated_right_payload_bytes = 0;
        if (preflight_ok &&
            active_join_kind != exec::CanonicalAcceptedJoinKind::kLeftSemi &&
            active_join_kind != exec::CanonicalAcceptedJoinKind::kLeftAnti) {
          preflight_ok =
              CheckedMultiply(left_input_payload_bytes,
                              right_batch.rows.size(),
                              &repeated_left_payload_bytes) &&
              CheckedMultiply(right_input_payload_bytes,
                              left_batch.rows.size(),
                              &repeated_right_payload_bytes) &&
              CheckedAdd(maximum_output_payload_bytes,
                         repeated_left_payload_bytes,
                         &maximum_output_payload_bytes) &&
              CheckedAdd(maximum_output_payload_bytes,
                         repeated_right_payload_bytes,
                         &maximum_output_payload_bytes);
          if (preflight_ok &&
              (active_join_kind ==
                   exec::CanonicalAcceptedJoinKind::kLeftOuter ||
               active_join_kind ==
                   exec::CanonicalAcceptedJoinKind::kFullOuter)) {
            preflight_ok = CheckedAdd(maximum_output_payload_bytes,
                                      left_input_payload_bytes,
                                      &maximum_output_payload_bytes);
          }
          if (preflight_ok &&
              (active_join_kind ==
                   exec::CanonicalAcceptedJoinKind::kRightOuter ||
               active_join_kind ==
                   exec::CanonicalAcceptedJoinKind::kFullOuter)) {
            preflight_ok = CheckedAdd(maximum_output_payload_bytes,
                                      right_input_payload_bytes,
                                      &maximum_output_payload_bytes);
          }
        } else if (preflight_ok) {
          preflight_ok = CheckedAdd(maximum_output_payload_bytes,
                                    left_input_payload_bytes,
                                    &maximum_output_payload_bytes);
        }
        if (preflight_ok) {
          const bool left_only_output =
              active_join_kind ==
                  exec::CanonicalAcceptedJoinKind::kLeftSemi ||
              active_join_kind ==
                  exec::CanonicalAcceptedJoinKind::kLeftAnti;
          std::uint64_t maximum_output_rows = actual_pair_count;
          const auto add_output_rows = [&](const std::size_t rows) {
            return CheckedAdd(maximum_output_rows,
                              static_cast<std::uint64_t>(rows),
                              &maximum_output_rows);
          };
          if (left_only_output) {
            maximum_output_rows = left_batch.rows.size();
          } else if (active_join_kind ==
                         exec::CanonicalAcceptedJoinKind::kLeftOuter) {
            preflight_ok = add_output_rows(left_batch.rows.size());
          } else if (active_join_kind ==
                         exec::CanonicalAcceptedJoinKind::kRightOuter) {
            preflight_ok = add_output_rows(right_batch.rows.size());
          } else if (active_join_kind ==
                         exec::CanonicalAcceptedJoinKind::kFullOuter) {
            preflight_ok = add_output_rows(left_batch.rows.size()) &&
                           add_output_rows(right_batch.rows.size());
          }
          maximum_output_rows = std::min<std::uint64_t>(
              maximum_output_rows, output_row_bound);
          const std::size_t output_width =
              left_batch.columns.size() +
              (left_only_output ? 0 : right_batch.columns.size());
          std::uint64_t allocation_bytes = 0;
          const auto add_structure_array = [&](const std::uint64_t count,
                                               const std::size_t size) {
            return CheckedMultiply(count, size, &allocation_bytes) &&
                   CheckedAdd(maximum_output_structure_bytes,
                              allocation_bytes,
                              &maximum_output_structure_bytes);
          };
          const auto add_structure_string = [&](const std::string& value) {
            return value.capacity() !=
                       std::numeric_limits<std::uint64_t>::max() &&
                   CheckedAdd(maximum_output_structure_bytes,
                              static_cast<std::uint64_t>(value.capacity()) + 1,
                              &maximum_output_structure_bytes);
          };
          const auto add_structure_descriptor =
              [&](const api::EngineDescriptor& descriptor,
                  const std::uint64_t copies) {
                std::uint64_t descriptor_bytes = 0;
                const auto account_string = [&](const std::string& value) {
                  return value.capacity() !=
                             std::numeric_limits<std::uint64_t>::max() &&
                         CheckedAdd(
                             descriptor_bytes,
                             static_cast<std::uint64_t>(value.capacity()) + 1,
                             &descriptor_bytes);
                };
                return account_string(descriptor.descriptor_uuid) &&
                       account_string(descriptor.descriptor_kind) &&
                       account_string(descriptor.canonical_type_name) &&
                       account_string(descriptor.encoded_descriptor) &&
                       CheckedAdd(descriptor_bytes, 32,
                                  &descriptor_bytes) &&
                       CheckedMultiply(descriptor_bytes, copies,
                                       &allocation_bytes) &&
                       CheckedAdd(maximum_output_structure_bytes,
                                  allocation_bytes,
                                  &maximum_output_structure_bytes);
              };
          std::uint64_t maximum_output_cells = 0;
          preflight_ok =
              preflight_ok &&
              CheckedMultiply(maximum_output_rows, output_width,
                              &maximum_output_cells) &&
              add_structure_array(output_width,
                                  sizeof(exec::ExecutorColumnDescriptor)) &&
              add_structure_array(maximum_output_rows,
                                  sizeof(exec::DescriptorTuple)) &&
              add_structure_array(maximum_output_cells,
                                  sizeof(api::EngineTypedValue));
          const auto account_output_columns = [&](const auto& columns) {
            for (const auto& column : columns) {
              if (!add_structure_string(column.stable_name) ||
                  !add_structure_descriptor(
                      column.descriptor, maximum_output_rows + 1)) {
                return false;
              }
            }
            return true;
          };
          preflight_ok = preflight_ok &&
                         account_output_columns(left_batch.columns) &&
                         (left_only_output ||
                          account_output_columns(right_batch.columns));
        }
        std::uint64_t maximum_phase_work_bytes =
            join_retained_state_bytes;
        std::uint64_t truth_value_bytes = 0;
        if (preflight_ok && runtime_bounded_inputs) {
          std::uint64_t join_phase_bytes = 0;
          preflight_ok =
              CheckedMultiply(actual_pair_count,
                              sizeof(api::EngineSqlTruthValue),
                              &truth_value_bytes) &&
              CheckedAdd(truth_value_bytes, join_retained_state_bytes,
                         &join_phase_bytes);
          if (preflight_ok) {
            maximum_phase_work_bytes = std::max(
                maximum_phase_work_bytes, join_phase_bytes);
          }
        }
        if (preflight_ok && runtime_bounded_inputs &&
            active_join_kind != exec::CanonicalAcceptedJoinKind::kCross) {
          preflight_ok = largest_row_payload(left_batch, &left_row_scratch) &&
                         largest_row_payload(right_batch,
                                             &right_row_scratch);
          if (preflight_ok) {
            std::uint64_t pair_payload_bytes = 0;
            if (!CheckedAdd(left_row_scratch, right_row_scratch,
                            &pair_payload_bytes)) {
              preflight_ok = false;
            } else {
              const auto predicate_scratch =
                  BoundCanonicalPredicateScratchBytes(
                      *active_runtime_relational_dag,
                      active_predicate_expression_id,
                      active_predicate_binding,
                      pair_payload_bytes,
                      [] { return false; });
              if (!predicate_scratch.ok) {
                step.diagnostic.ok = false;
                step.diagnostic.diagnostic_code =
                    "SBLR.PLAN_TREE.RESOURCE_LIMIT";
                step.diagnostic.detail = active_operation_name + ":" +
                                         predicate_scratch.detail;
                return step;
              }
              std::uint64_t predicate_phase_bytes = 0;
              preflight_ok =
                  CheckedAdd(truth_value_bytes,
                             predicate_scratch.maximum_payload_bytes,
                             &predicate_phase_bytes) &&
                  CheckedAdd(predicate_phase_bytes,
                             predicate_scratch.maximum_structural_bytes,
                             &predicate_phase_bytes);
              if (preflight_ok) {
                maximum_phase_work_bytes = std::max(
                    maximum_phase_work_bytes, predicate_phase_bytes);
              }
            }
          }
        }
        if (preflight_ok) {
          preflight_ok = CheckedAdd(preflight_retained_bytes,
                                    maximum_phase_work_bytes,
                                    &preflight_retained_bytes) &&
                         CheckedAdd(preflight_retained_bytes,
                                    maximum_output_payload_bytes,
                                    &preflight_retained_bytes) &&
                         CheckedAdd(preflight_retained_bytes,
                                    maximum_output_structure_bytes,
                                    &preflight_retained_bytes);
        }
        if (!preflight_ok ||
            preflight_retained_bytes > *selected_node_memory_bound) {
          if (!step.diagnostic.ok) return step;
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "SBLR.PLAN_TREE.RESOURCE_LIMIT";
          step.diagnostic.detail = active_operation_name +
              " retained inputs, truth, or predicate scratch exceed the "
              "selected-node memory grant";
          return step;
        }
        // Only now may the callback allocate receipt carriers. The complete
        // coexistence proof above includes both retained input batches, the
        // operator-local DAG, these carriers, output storage, and the largest
        // sequential predicate/join work phase.
        step.selected_plan_uuid = dag.selected_plan_uuid;
        step.mga_statement_context = dag.mga_statement_context;
        step.output_descriptor_ids = node.output_descriptor_ids;
        step.authority.engine_mga_snapshot_bound = true;
        if (poll_cancellation("before binding input batches")) return step;
        std::string operator_dag_detail;
        if (!BuildOperatorLocalPhysicalDag(
                dag, node.physical_node_id, &key_request.physical_dag,
                &operator_dag_detail)) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "QOW-DIAG-RELATIONAL-LIVE-JOIN-INPUT-V1";
          step.diagnostic.detail =
              active_operation_name + " " + operator_dag_detail;
          return step;
        }
        if (poll_cancellation("after binding operator-local join DAG")) {
          return step;
        }
        const auto bounded_operator_node = std::ranges::find_if(
            key_request.physical_dag.nodes, [&](const auto& candidate) {
              return candidate.physical_node_id == node.physical_node_id;
            });
        if (bounded_operator_node == key_request.physical_dag.nodes.end()) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "QOW-DIAG-RELATIONAL-LIVE-JOIN-INPUT-V1";
          step.diagnostic.detail = active_operation_name +
                                   " operator-local root is absent";
          return step;
        }
        if (bounded_operator_node->memory_bytes_required <
                *selected_node_memory_bound) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "SBLR.PLAN_TREE.RESOURCE_LIMIT";
          step.diagnostic.detail = active_operation_name +
                                   " callback memory exceeds its published grant";
          return step;
        }
        if (!RebindOperatorLocalPhysicalMemoryGrant(
                &*bounded_operator_node, &key_request.physical_dag,
                *selected_node_memory_bound, &operator_dag_detail)) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "SBLR.PLAN_TREE.RESOURCE_LIMIT";
          step.diagnostic.detail = active_operation_name +
                                   " " + operator_dag_detail;
          return step;
        }
        key_request.selected_physical_node_id = node.physical_node_id;
        key_request.borrowed_left_batch = &left_batch;
        key_request.borrowed_right_batch = &right_batch;
        key_request.dispatcher_mga_boundary_active =
            borrowed_runtime_mga_authority != nullptr ||
            runtime_mga_authority.has_value();
        if (borrowed_runtime_mga_authority != nullptr) {
          key_request.borrowed_mga_authority =
              borrowed_runtime_mga_authority;
        } else if (runtime_mga_authority.has_value()) {
          key_request.borrowed_mga_authority = &*runtime_mga_authority;
        } else {
          key_request.mga_authority =
              BuildCanonicalExecutionMgaAuthority(
                  active_mga_context, key_request.physical_dag);
        }
        const auto& active_mga_authority =
            key_request.borrowed_mga_authority == nullptr
                ? key_request.mga_authority
                : *key_request.borrowed_mga_authority;
        std::vector<api::EngineSqlTruthValue> runtime_truth_values;
        const std::vector<api::EngineSqlTruthValue>* bound_truth_values =
            &predicate_truth_values;
        if (runtime_bounded_inputs) {
          runtime_truth_values.reserve(actual_pair_count);
          bound_truth_values = &runtime_truth_values;
          if (active_join_kind == exec::CanonicalAcceptedJoinKind::kCross) {
            for (std::size_t pair = 0; pair < actual_pair_count; ++pair) {
              if (poll_cancellation("while binding CROSS truth")) return step;
              runtime_truth_values.push_back(
                  api::EngineSqlTruthValue::true_value);
            }
          } else {
            CanonicalRelationalExpressionRuntime runtime_expression(
                *active_runtime_relational_dag,
                runtime_expression_services);
            for (const auto& left : left_batch.rows) {
              for (const auto& right : right_batch.rows) {
                if (poll_cancellation("while evaluating ON truth")) return step;
                const CanonicalRelationalExpressionRowView pair_values{
                    left.values, right.values};
                api::EngineSqlTruthValue truth =
                    api::EngineSqlTruthValue::unknown;
                std::string detail;
                if (!runtime_expression.EvaluatePredicateForConsumer(
                        active_predicate_expression_id,
                        active_predicate_binding, pair_values,
                        api::EngineCanonicalExpressionConsumer::join, &truth,
                        &detail)) {
                  step.diagnostic.ok = false;
                  step.diagnostic.diagnostic_code =
                      "QOW-DIAG-RELATIONAL-LIVE-JOIN-PREDICATE-V1";
                  step.diagnostic.detail =
                      active_operation_name + ":" + detail;
                  return step;
                }
                runtime_truth_values.push_back(truth);
              }
            }
          }
        }
        join_request.borrowed_bound_pair_truth_values = bound_truth_values;
        join_request.residual_request.maximum_candidate_rechecks =
            std::max<std::size_t>(1, runtime_bounded_inputs
                                         ? actual_pair_count
                                         : pair_count);
        join_request.join_kind = active_join_kind;
        join_request.bound_pair_truth_profile = true;
        join_request.maximum_output_rows =
            std::max<std::size_t>(1, output_row_bound);
        join_request.borrowed_cancellation_requested =
            &cancellation_requested;
        if (poll_cancellation("before canonical join execution")) return step;
        auto join_result =
            exec::ExecuteCanonicalJoinKind(join_request);
        if (join_result.cancellation_observed) {
          step.diagnostic = std::move(join_result.diagnostic);
          step.cancellation_observed = true;
          step.transient_state_cleanup_proven =
              join_result.transient_state_cleanup_proven;
          step.cancellation_evidence_uuid =
              cancellation_policy->evidence_uuid;
          return step;
        }
        if (!join_result.diagnostic.ok) {
          step.diagnostic = std::move(join_result.diagnostic);
          if (step.diagnostic.diagnostic_code ==
              "QOW-DIAG-QRY-012-CANCELLATION-PROBE-V1") {
            step.diagnostic.diagnostic_code =
                "QOW-DIAG-QRY-004-PHYSICAL-CANCELLATION-PROBE-V1";
          }
          return step;
        }
        if (join_result.selected_plan_uuid !=
                key_request.physical_dag.selected_plan_uuid ||
            join_result.executed_physical_node_id != node.physical_node_id ||
            join_result.causal_counter_id != node.causal_counter_id ||
            !exec::PhysicalMgaStatementContextEqual(
                join_result.mga_statement_context,
                active_mga_authority.statement_context)) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "QOW-DIAG-RELATIONAL-LIVE-JOIN-EXECUTION-V1";
          step.diagnostic.detail = active_operation_name +
                                   " execution receipt changed";
          return step;
        }
        if (poll_cancellation("after canonical join execution")) return step;
        step.result_handle_id = node.physical_node_id;
        step.input_row_count =
            left_batch.rows.size() + right_batch.rows.size();
        step.rows_examined = runtime_bounded_inputs ? actual_pair_count
                                                    : pair_count;
        step.output_row_count = join_result.output_batch.rows.size();
        step.materialized_output_batch = std::move(join_result.output_batch);
        step.mga_statement_context =
            std::move(join_result.mga_statement_context);
        return step;
      };
  return registration;
}
}  // namespace scratchbird::engine::sblr
