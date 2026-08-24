// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "descriptor_value_runtime.hpp"

#include <algorithm>
#include <chrono>
#include <exception>
#include <functional>
#include <limits>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

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

bool HasForbiddenAuthority(
    const CanonicalPhysicalDispatchAuthorityEvidence& authority) {
  return authority.owns_transaction_finality || authority.owns_recovery ||
         authority.owns_parser_execution ||
         authority.owns_visibility_outside_engine_mga ||
         authority.wal_is_transaction_or_recovery_authority;
}

bool HasForbiddenObservationAuthority(
    const CanonicalRuntimeObservationAuthorityEvidence& authority) {
  return authority.owns_execution || authority.owns_visibility ||
         authority.owns_transaction_finality || authority.owns_recovery ||
         authority.owns_feedback || authority.owns_benchmark ||
         authority.owns_parser_execution ||
         authority.wal_is_transaction_or_recovery_authority;
}

bool MetricStateValid(const CanonicalObservedUint64& metric) {
  return metric.state == CanonicalRuntimeMetricState::kObserved ||
         (metric.state == CanonicalRuntimeMetricState::kNotApplicable &&
          metric.value == 0);
}

DescriptorRuntimeDiagnostic ValidateRuntimeObservation(
    const CanonicalPhysicalNodeRuntimeObservation& observation,
    const std::uint64_t memory_budget_bytes,
    const std::uint64_t legacy_pages_read,
    const std::uint64_t legacy_spill_bytes,
    const bool data_access_observation_known,
    const bool data_access_observed) {
  const auto invalid = [](std::string detail) {
    return Refusal("QOW-DIAG-OPT-017-REFUSAL-V1", std::move(detail));
  };
  if (observation.abi_version != 1 ||
      !observation.producer_receipt_complete ||
      !observation.dispatcher_elapsed_frozen ||
      !observation.counters_frozen_after_finish ||
      !observation.authority.engine_execution_observation ||
      HasForbiddenObservationAuthority(observation.authority)) {
    return invalid("runtime_observation_authority");
  }
  if (observation.elapsed_ns.state !=
          CanonicalRuntimeMetricState::kObserved ||
      observation.operator_wait_ns.state !=
          CanonicalRuntimeMetricState::kObserved ||
      observation.current_memory_bytes.state !=
          CanonicalRuntimeMetricState::kObserved ||
      observation.peak_memory_bytes.state !=
          CanonicalRuntimeMetricState::kObserved) {
    return invalid("runtime_observation_required_metric");
  }
  const CanonicalObservedUint64* remaining[] = {
      &observation.decoded_bytes,
      &observation.bytes_read,
      &observation.bytes_written,
      &observation.pages_read,
      &observation.pages_written,
      &observation.spill_bytes_read,
      &observation.spill_bytes_written,
      &observation.visibility_recheck_count,
      &observation.security_recheck_count,
      &observation.storage_recheck_count,
      &observation.index_recheck_count,
      &observation.residual_recheck_count,
      &observation.compatibility_recheck_count,
      &observation.archive_bytes_read,
      &observation.cluster_bytes_sent,
      &observation.cluster_bytes_received,
  };
  if (!std::ranges::all_of(remaining, [](const auto* metric) {
        return MetricStateValid(*metric);
      })) {
    return invalid("runtime_observation_metric_state");
  }
  if (observation.operator_wait_ns.value > observation.elapsed_ns.value ||
      observation.current_memory_bytes.value >
          observation.peak_memory_bytes.value ||
      memory_budget_bytes == 0 ||
      observation.peak_memory_bytes.value > memory_budget_bytes) {
    return invalid("runtime_observation_resource_bounds");
  }
  const CanonicalObservedUint64* access_metrics[] = {
      &observation.decoded_bytes,
      &observation.bytes_read,
      &observation.bytes_written,
      &observation.pages_read,
      &observation.pages_written,
      &observation.archive_bytes_read,
      &observation.cluster_bytes_sent,
      &observation.cluster_bytes_received,
  };
  if (data_access_observation_known && !data_access_observed &&
      std::ranges::any_of(access_metrics, [](const auto* metric) {
        return metric->state == CanonicalRuntimeMetricState::kObserved &&
               metric->value != 0;
      })) {
    return invalid("runtime_observation_zero_access_contradiction");
  }
  std::uint64_t spill_total = 0;
  if (observation.spill_bytes_read.value >
      std::numeric_limits<std::uint64_t>::max() -
          observation.spill_bytes_written.value) {
    return invalid("runtime_observation_spill_overflow");
  }
  spill_total = observation.spill_bytes_read.value +
                observation.spill_bytes_written.value;
  if (((observation.pages_read.state ==
            CanonicalRuntimeMetricState::kObserved &&
        observation.pages_read.value != legacy_pages_read) ||
       (observation.pages_read.state ==
            CanonicalRuntimeMetricState::kNotApplicable &&
        legacy_pages_read != 0)) ||
      spill_total != legacy_spill_bytes) {
    return invalid("runtime_observation_legacy_counter_mismatch");
  }
  return {};
}

bool CheckedAdd(const std::size_t left,
                const std::size_t right,
                std::size_t* output) {
  if (output == nullptr ||
      right > std::numeric_limits<std::size_t>::max() - left) {
    return false;
  }
  *output = left + right;
  return true;
}

bool MaterializedBatchLiveBytes(const DescriptorBatch& batch,
                                std::uint64_t* bytes) {
  if (bytes == nullptr) return false;
  *bytes = sizeof(batch);
  const auto add = [&](const std::uint64_t value) {
    if (value > std::numeric_limits<std::uint64_t>::max() - *bytes) {
      return false;
    }
    *bytes += value;
    return true;
  };
  const auto add_array = [&](const std::size_t count,
                             const std::size_t element_size) {
    if (count != 0 &&
        element_size > std::numeric_limits<std::uint64_t>::max() / count) {
      return false;
    }
    return add(static_cast<std::uint64_t>(count * element_size));
  };
  const auto add_string = [&](const std::string& value) {
    return value.capacity() != std::numeric_limits<std::uint64_t>::max() &&
           add(static_cast<std::uint64_t>(value.capacity()) + 1);
  };
  const auto add_descriptor = [&](const auto& descriptor) {
    return add_string(descriptor.descriptor_uuid.canonical) &&
           add_string(descriptor.descriptor_kind) &&
           add_string(descriptor.canonical_type_name) &&
           add_string(descriptor.encoded_descriptor);
  };
  if (!add_array(batch.columns.capacity(),
                 sizeof(ExecutorColumnDescriptor)) ||
      !add_array(batch.rows.capacity(), sizeof(DescriptorTuple))) {
    return false;
  }
  for (const auto& column : batch.columns) {
    if (!add_string(column.stable_name) ||
        !add_descriptor(column.descriptor)) {
      return false;
    }
  }
  for (const auto& row : batch.rows) {
    if (!add_array(row.values.capacity(),
                   sizeof(internal_api::EngineTypedValue))) {
      return false;
    }
    for (const auto& value : row.values) {
      if (!add_descriptor(value.descriptor) ||
          !add_string(value.encoded_value) ||
          !add(static_cast<std::uint64_t>(value.binary_value.capacity()))) {
        return false;
      }
    }
  }
  return true;
}

bool PhysicalMgaContextDynamicLiveBytes(
    const PhysicalMgaStatementContext& context,
    std::uint64_t* bytes) {
  if (bytes == nullptr) return false;
  const auto add = [&](const std::uint64_t value) {
    if (value > std::numeric_limits<std::uint64_t>::max() - *bytes) {
      return false;
    }
    *bytes += value;
    return true;
  };
  const auto add_array = [&](const std::size_t count,
                             const std::size_t element_size) {
    return count == 0 ||
           (element_size <=
                std::numeric_limits<std::uint64_t>::max() / count &&
            add(static_cast<std::uint64_t>(count * element_size)));
  };
  const auto add_string = [&](const std::string& value) {
    return value.capacity() != std::numeric_limits<std::size_t>::max() &&
           add(static_cast<std::uint64_t>(value.capacity()) + 1);
  };
  return add_string(context.statement_uuid) &&
         add_string(context.owning_transaction_uuid) &&
         add_string(context.statement_snapshot_uuid) &&
         add_string(context.statement_metadata_snapshot_uuid) &&
         add_array(context.active_excluded_local_transaction_ids.capacity(),
                   sizeof(std::uint64_t)) &&
         add_array(context.in_doubt_excluded_local_transaction_ids.capacity(),
                   sizeof(std::uint64_t)) &&
         add_string(context.snapshot_kind) &&
         add_string(context.statement_timestamp);
}

bool PhysicalNodeCopyLiveBytes(const PhysicalNodeRecord& node,
                               std::uint64_t* bytes) {
  if (bytes == nullptr) return false;
  *bytes = sizeof(PhysicalNodeRecord);
  const auto add = [&](const std::uint64_t value) {
    if (value > std::numeric_limits<std::uint64_t>::max() - *bytes) {
      return false;
    }
    *bytes += value;
    return true;
  };
  const auto add_array = [&](const std::size_t count,
                             const std::size_t width) {
    return count == 0 ||
           (width <= std::numeric_limits<std::uint64_t>::max() / count &&
            add(static_cast<std::uint64_t>(count * width)));
  };
  const auto add_string = [&](const std::string& value) {
    return value.capacity() != std::numeric_limits<std::size_t>::max() &&
           add(static_cast<std::uint64_t>(value.capacity()) + 1);
  };
  const auto add_strings = [&](const std::vector<std::string>& values) {
    if (!add_array(values.capacity(), sizeof(std::string))) return false;
    for (const auto& value : values) {
      if (!add_string(value)) return false;
    }
    return true;
  };
  return add_string(node.implementation_id) &&
         add_array(node.input_physical_node_ids.capacity(),
                   sizeof(std::uint64_t)) &&
         add_array(node.output_descriptor_ids.capacity(),
                   sizeof(std::uint32_t)) &&
         add_string(node.selected_alternative_uuid) &&
         add_string(node.executor_capability_uuid) &&
         add_string(node.cost_vector_uuid) &&
         add_strings(node.required_property_uuids) &&
         add_strings(node.delivered_property_uuids) &&
         PhysicalMgaContextDynamicLiveBytes(node.mga_statement_context,
                                            bytes) &&
         add_string(node.logical_semantic_variant_id) &&
         add_string(node.transformation_uuid) &&
         add_string(node.transformation_rule_id) &&
         add_strings(node.enforced_property_uuids) &&
         add_string(node.retained_cost.cost_vector_uuid) &&
         add_string(node.retained_cost.calibration_profile_uuid) &&
         add_string(node.retained_cost.scalarization_policy_id);
}

bool DispatchStepMetadataLiveBytes(
    const CanonicalPhysicalDispatchStepResult& step,
    std::uint64_t* bytes) {
  if (bytes == nullptr) return false;
  *bytes = 0;
  const auto add = [&](const std::uint64_t value) {
    if (value > std::numeric_limits<std::uint64_t>::max() - *bytes) {
      return false;
    }
    *bytes += value;
    return true;
  };
  const auto add_array = [&](const std::size_t count,
                             const std::size_t element_size) {
    return count == 0 ||
           (element_size <=
                std::numeric_limits<std::uint64_t>::max() / count &&
            add(static_cast<std::uint64_t>(count * element_size)));
  };
  const auto add_string = [&](const std::string& value) {
    return value.capacity() != std::numeric_limits<std::size_t>::max() &&
           add(static_cast<std::uint64_t>(value.capacity()) + 1);
  };
  if (!add_string(step.diagnostic.diagnostic_code) ||
      !add_string(step.diagnostic.detail) ||
      !add_string(step.selected_plan_uuid) ||
      !add_string(step.executed_implementation_id) ||
      !add_array(step.executed_input_physical_node_ids.capacity(),
                 sizeof(std::uint64_t)) ||
      !add_array(step.output_descriptor_ids.capacity(),
                 sizeof(std::uint32_t)) ||
      !add_string(step.cancellation_evidence_uuid) ||
      !add_string(step.current_relation_descriptor_uuid) ||
      !PhysicalMgaContextDynamicLiveBytes(step.mga_statement_context,
                                          bytes)) {
    return false;
  }
  if (step.table_sample_actuals.has_value() &&
      (!add_string(step.table_sample_actuals->sample_descriptor_uuid) ||
       !add_string(step.table_sample_actuals->method_id))) {
    return false;
  }
  return true;
}

DescriptorRuntimeDiagnostic ValidateRuntimeLimits(
    const CanonicalPhysicalDagRuntimeLimits& limits) {
  if (limits.maximum_rows_per_batch == 0 ||
      limits.maximum_columns_per_batch == 0 ||
      limits.maximum_cells_per_batch == 0 ||
      limits.maximum_total_materialized_rows == 0 ||
      limits.maximum_total_materialized_cells == 0) {
    return Refusal("SBLR.PLAN_TREE.RESOURCE_LIMIT",
                   "physical typed-batch runtime limits must be nonzero");
  }
  return {};
}

DescriptorRuntimeDiagnostic ValidateMaterializedBatch(
    const DescriptorBatch& batch,
    const std::vector<std::uint32_t>& descriptor_ids,
    const CanonicalPhysicalDagRuntimeLimits& limits,
    const std::size_t total_rows,
    const std::size_t total_cells,
    std::size_t* next_total_rows,
    std::size_t* next_total_cells) {
  const auto row_count = batch.rows.size();
  const auto column_count = batch.columns.size();
  if (row_count > limits.maximum_rows_per_batch ||
      column_count > limits.maximum_columns_per_batch ||
      (column_count != 0 &&
       row_count > limits.maximum_cells_per_batch / column_count)) {
    return Refusal("SBLR.PLAN_TREE.RESOURCE_LIMIT",
                   "physical typed batch exceeds its row, column, or cell "
                   "bound");
  }
  const auto batch_cells = row_count * column_count;
  if (!CheckedAdd(total_rows, row_count, next_total_rows) ||
      !CheckedAdd(total_cells, batch_cells, next_total_cells) ||
      *next_total_rows > limits.maximum_total_materialized_rows ||
      *next_total_cells > limits.maximum_total_materialized_cells) {
    return Refusal("SBLR.PLAN_TREE.RESOURCE_LIMIT",
                   "physical typed DAG exceeds its cumulative batch bound");
  }
  return ValidateCanonicalDescriptorBatch(batch, descriptor_ids);
}

}  // namespace

DescriptorRuntimeDiagnostic RevalidateCanonicalExecutionMgaAuthority(
    const CanonicalExecutionMgaAuthority& authority,
    const TypedPhysicalNodeDag& physical_dag,
    const PhysicalNodeAbiLimits& limits) {
  const auto dag_validation = ValidateTypedPhysicalNodeDag(physical_dag, limits);
  if (!dag_validation.accepted) {
    const auto& issue = dag_validation.issues.front();
    return Refusal(issue.diagnostic_id, issue.field_id);
  }
  if (physical_dag.abi_version != 2) {
    // ABI v1 predates the full immutable statement-context carrier, but its
    // scalar snapshot admission contract still fails closed on zero.  Keep
    // that legacy boundary intact while ABI v2 uses only the full carrier
    // and accepts a valid zero committed high-watermark.
    if (physical_dag.statement_snapshot_id == 0) {
      return Refusal("QOW-DIAG-PHYSICAL-NODE-ABI-ADMISSION",
                     "statement_snapshot_id");
    }
    return {};
  }
  if (authority.origin == CanonicalMgaAuthorityOrigin::kMissing ||
      !authority.resolve_current ||
      !PhysicalMgaStatementContextValid(authority.statement_context) ||
      !PhysicalMgaStatementContextEqual(authority.statement_context,
                                        physical_dag.mga_statement_context)) {
    return Refusal(
        "QOW-DIAG-MGA-RUNTIME-AUTHORITY-V1",
        "missing, malformed, or DAG-swapped MGA runtime authority");
  }
  auto current = authority.resolve_current();
  if (!current.diagnostic.ok) {
    return current.diagnostic;
  }
  if (!PhysicalMgaStatementContextValid(current.statement_context) ||
      !PhysicalMgaStatementContextEqual(current.statement_context,
                                        authority.statement_context)) {
    return Refusal(
        "QOW-DIAG-MGA-RUNTIME-CURRENT-V1",
        "resolved current MGA statement context differs from the selected "
        "execution authority");
  }
  return {};
}

bool CanonicalMgaCreatorVisibleToStatement(
    const PhysicalMgaStatementContext& statement_context,
    const std::uint64_t creator_local_transaction_id) {
  if (!PhysicalMgaStatementContextValid(statement_context) ||
      creator_local_transaction_id == 0) {
    return false;
  }
  if (creator_local_transaction_id ==
      statement_context.owning_local_transaction_id) {
    return true;
  }
  if (creator_local_transaction_id >
      statement_context.visible_committed_high_watermark) {
    return false;
  }
  return !std::binary_search(
             statement_context.active_excluded_local_transaction_ids.begin(),
             statement_context.active_excluded_local_transaction_ids.end(),
             creator_local_transaction_id) &&
         !std::binary_search(
             statement_context.in_doubt_excluded_local_transaction_ids.begin(),
             statement_context.in_doubt_excluded_local_transaction_ids.end(),
             creator_local_transaction_id);
}

// QOW-SOURCE-QRY-004-CONSUMPTION-V1
// Validates and consumes one complete selected physical DAG.  Every exact
// implementation is resolved before execution, inputs run before consumers,
// shared nodes execute once, and every returned handle must retain the
// admitted plan/node/descriptor/causal identities.  The dispatcher coordinates
// engine executors but never acquires parser, MGA-finality, recovery, or WAL
// authority.
CanonicalPhysicalDagDispatchResult ExecuteCanonicalPhysicalDag(
    const CanonicalPhysicalDagDispatchRequest& request) {
  CanonicalPhysicalDagDispatchResult result;
  const auto& physical_dag = request.borrowed_physical_dag == nullptr
                                 ? request.physical_dag
                                 : *request.borrowed_physical_dag;
  const auto& mga_authority = request.borrowed_mga_authority == nullptr
                                  ? request.mga_authority
                                  : *request.borrowed_mga_authority;
  const auto& available_executors =
      request.borrowed_available_executors == nullptr
          ? request.available_executors
          : *request.borrowed_available_executors;
  bool execution_started = false;
  bool data_access_observed = false;
  bool cancellation_observed = false;
  const auto refuse = [&](DescriptorRuntimeDiagnostic diagnostic,
                          const bool replan = false) {
    result = {};
    result.diagnostic = std::move(diagnostic);
    result.replan_required = replan;
    result.execution_started = execution_started;
    result.data_access_observed = data_access_observed;
    result.cancellation_observed = cancellation_observed;
    return result;
  };

  const auto cancellation_diagnostic = [&](const char* phase) {
    DescriptorRuntimeDiagnostic diagnostic;
    if (!request.cancellation_requested) return diagnostic;
    try {
      if (!request.cancellation_requested()) return diagnostic;
      cancellation_observed = true;
      return Refusal(
          "QOW-DIAG-QRY-004-PHYSICAL-DISPATCH-CANCELLED-V1",
          std::string("physical DAG cancellation observed ") + phase);
    } catch (const std::exception& exception) {
      return Refusal(
          "QOW-DIAG-QRY-004-PHYSICAL-CANCELLATION-PROBE-V1",
          std::string("physical cancellation probe threw: ") +
              exception.what());
    } catch (...) {
      return Refusal(
          "QOW-DIAG-QRY-004-PHYSICAL-CANCELLATION-PROBE-V1",
          "physical cancellation probe threw a non-standard exception");
    }
  };

  const auto authority_validation = RevalidateCanonicalExecutionMgaAuthority(
      mga_authority, physical_dag, request.limits);
  if (!authority_validation.ok) {
    return refuse(authority_validation);
  }
  const auto runtime_limits_validation =
      ValidateRuntimeLimits(request.runtime_limits);
  if (!runtime_limits_validation.ok) {
    return refuse(runtime_limits_validation);
  }
  const auto entry_cancellation =
      cancellation_diagnostic("before selected-node dispatch");
  if (!entry_cancellation.ok) return refuse(entry_cancellation);

  const auto executor_for = [&](const std::string_view implementation_id) {
    const CanonicalPhysicalExecutorRegistration* found = nullptr;
    for (const auto& registration : available_executors) {
      if (registration.implementation_id != implementation_id) continue;
      if (found != nullptr) return static_cast<
          const CanonicalPhysicalExecutorRegistration*>(nullptr);
      found = &registration;
    }
    return found;
  };
  for (const auto& registration : available_executors) {
    if (registration.implementation_id.empty() || !registration.execute ||
        executor_for(registration.implementation_id) != &registration) {
      return refuse(Refusal(
          "QOW-DIAG-QRY-004-PHYSICAL-EXECUTOR-REGISTRY-V1",
          "physical executor registration is missing or duplicated"));
    }
  }
  if (request.preexisting_live_memory_bytes != 0) {
    std::uint64_t verified_registration_bytes = sizeof(available_executors);
    std::uint64_t registry_slot_bytes = 0;
    if (available_executors.capacity() >
            std::numeric_limits<std::uint64_t>::max() /
                sizeof(CanonicalPhysicalExecutorRegistration) ||
        (registry_slot_bytes =
             static_cast<std::uint64_t>(available_executors.capacity()) *
             sizeof(CanonicalPhysicalExecutorRegistration),
         registry_slot_bytes >
             std::numeric_limits<std::uint64_t>::max() -
                 verified_registration_bytes)) {
      return refuse(Refusal(
          "SBLR.PLAN_TREE.RESOURCE_LIMIT",
          "executor registration retained-memory verification overflowed"));
    }
    verified_registration_bytes += registry_slot_bytes;
    for (const auto& registration : available_executors) {
      const auto account = [&](const std::uint64_t bytes) {
        if (bytes > std::numeric_limits<std::uint64_t>::max() -
                        verified_registration_bytes) {
          return false;
        }
        verified_registration_bytes += bytes;
        return true;
      };
      if (registration.retained_live_memory_bytes_v1 == 0 ||
          registration.implementation_id.capacity() ==
              std::numeric_limits<std::size_t>::max() ||
          registration.executor_capability_uuid.capacity() ==
              std::numeric_limits<std::size_t>::max() ||
          !account(registration.implementation_id.capacity() + 1) ||
          !account(registration.executor_capability_uuid.capacity() + 1) ||
          !account(registration.retained_live_memory_bytes_v1)) {
        return refuse(Refusal(
            "SBLR.PLAN_TREE.RESOURCE_LIMIT",
            "executor registration retained-memory receipt is incomplete"));
      }
    }
    if (verified_registration_bytes !=
        request.preexisting_live_memory_bytes) {
      return refuse(Refusal(
          "SBLR.PLAN_TREE.RESOURCE_LIMIT",
          "executor registration retained-memory receipt changed before dispatch"));
    }
  }

  const auto node_index_for = [&](const std::uint64_t node_id) {
    const auto node = std::ranges::find_if(
        physical_dag.nodes, [&](const auto& candidate) {
          return candidate.physical_node_id == node_id;
        });
    return node == physical_dag.nodes.end()
               ? physical_dag.nodes.size()
               : static_cast<std::size_t>(
                     std::distance(physical_dag.nodes.begin(), node));
  };
  for (const auto& node : physical_dag.nodes) {
    const auto* registration = executor_for(node.implementation_id);
    if (registration == nullptr ||
        registration->node_kind != node.node_kind ||
        (physical_dag.abi_version == 2 &&
         (registration->executor_capability_uuid !=
              node.executor_capability_uuid ||
          registration->executor_capability_abi_version !=
              node.executor_capability_abi_version ||
          !registration->engine_owned ||
          !registration->accepts_optimizer_publication_v2))) {
      return refuse(
          Refusal("QOW-DIAG-QRY-004-PHYSICAL-IMPLEMENTATION-UNAVAILABLE-V1",
                  "selected physical implementation is unavailable: " +
                      node.implementation_id),
          true);
    }
  }

  const auto node_count = physical_dag.nodes.size();
  std::uint64_t dispatcher_control_live_bytes =
      request.preexisting_live_memory_bytes;
  const auto account_control_array = [&](const std::size_t count,
                                         const std::size_t width) {
    std::uint64_t bytes = 0;
    return (count == 0 ||
            (width <= std::numeric_limits<std::uint64_t>::max() / count &&
             (bytes = static_cast<std::uint64_t>(count * width), true))) &&
           bytes <= std::numeric_limits<std::uint64_t>::max() -
                        dispatcher_control_live_bytes &&
           (dispatcher_control_live_bytes += bytes, true);
  };
  const auto account_control_string = [&](const std::string& value) {
    return value.capacity() != std::numeric_limits<std::size_t>::max() &&
           static_cast<std::uint64_t>(value.capacity()) + 1 <=
               std::numeric_limits<std::uint64_t>::max() -
                   dispatcher_control_live_bytes &&
           (dispatcher_control_live_bytes +=
                static_cast<std::uint64_t>(value.capacity()) + 1,
            true);
  };
  const auto root_node_index =
      node_index_for(physical_dag.root_physical_node_id);
  // All persistent dispatcher arrays are admitted before allocation. Linear
  // node/registration lookup avoids implementation-dependent hash buckets.
  if (!account_control_array(node_count, sizeof(std::uint64_t)) ||
      !account_control_array(node_count, sizeof(std::uint8_t)) ||
      !account_control_array(node_count, sizeof(std::size_t)) ||
      !account_control_array(node_count, sizeof(std::size_t)) ||
      !account_control_array(node_count, sizeof(std::uint64_t)) ||
      !account_control_array(node_count, sizeof(std::uint8_t)) ||
      !account_control_array(
          node_count, sizeof(CanonicalPhysicalDispatchStepResult)) ||
      root_node_index == node_count ||
      !account_control_array(
          physical_dag.nodes[root_node_index].output_descriptor_ids.size(),
          sizeof(std::uint32_t)) ||
      !account_control_string(physical_dag.selected_plan_uuid) ||
      !PhysicalMgaContextDynamicLiveBytes(mga_authority.statement_context,
                                          &dispatcher_control_live_bytes) ||
      dispatcher_control_live_bytes >= physical_dag.memory_budget_bytes) {
    return refuse(Refusal(
        "SBLR.PLAN_TREE.RESOURCE_LIMIT",
        "physical dispatcher control carriers exceed statement memory"));
  }

  std::vector<std::uint64_t> dispatch_order;
  dispatch_order.reserve(node_count);
  std::vector<std::uint8_t> scheduled(node_count, 0);
  const auto schedule = [&](auto&& self, const std::uint64_t node_id) -> bool {
    const auto index = node_index_for(node_id);
    if (index == node_count) return false;
    if (scheduled[index] != 0) return true;
    scheduled[index] = 1;
    for (const auto input_id :
         physical_dag.nodes[index].input_physical_node_ids) {
      if (!self(self, input_id)) return false;
    }
    dispatch_order.push_back(node_id);
    return true;
  };
  if (!schedule(schedule, physical_dag.root_physical_node_id) ||
      dispatch_order.size() != node_count) {
    return refuse(Refusal("SBLR.PLAN_TREE.INVALID_HANDLE",
                          "physical dispatch order omitted a node"));
  }

  const auto kNoResult = std::numeric_limits<std::size_t>::max();
  std::vector<std::size_t> result_index_by_node(node_count, kNoResult);
  // Move a materialized child into its final consumer and leave only the
  // non-payload execution evidence behind. Shared children remain retained
  // until their final edge; earlier consumers receive the existing bounded
  // value copy required by the callback ABI.
  std::vector<std::size_t> remaining_consumer_count(node_count, 0);
  for (const auto& node : physical_dag.nodes) {
    for (const auto input_id : node.input_physical_node_ids) {
      const auto input_index = node_index_for(input_id);
      if (input_index == node_count ||
          remaining_consumer_count[input_index] ==
              std::numeric_limits<std::size_t>::max()) {
        return refuse(Refusal("SBLR.PLAN_TREE.INVALID_HANDLE",
                              "physical consumer count overflowed"));
      }
      ++remaining_consumer_count[input_index];
    }
  }
  std::vector<std::uint64_t> materialized_live_bytes_by_node(node_count, 0);
  std::vector<std::uint8_t> materialized_live_by_node(node_count, 0);
  std::uint64_t retained_materialized_live_bytes = 0;
  std::uint64_t retained_step_metadata_live_bytes = 0;
  std::size_t total_materialized_rows = 0;
  std::size_t total_materialized_cells = 0;
  result.executed_steps.reserve(node_count);
  for (const auto node_id : dispatch_order) {
    const auto node_cancellation =
        cancellation_diagnostic("before a selected node");
    if (!node_cancellation.ok) return refuse(node_cancellation);
    const auto node_index = node_index_for(node_id);
    if (node_index == node_count) {
      return refuse(Refusal("SBLR.PLAN_TREE.INVALID_HANDLE",
                            "scheduled physical node is absent"));
    }
    const auto& node = physical_dag.nodes[node_index];
    std::uint64_t local_dispatch_metadata_live_bytes = 0;
    std::uint64_t allocation_bytes = 0;
    if ((node.input_physical_node_ids.size() != 0 &&
         sizeof(CanonicalPhysicalDispatchInput) >
             std::numeric_limits<std::uint64_t>::max() /
                 node.input_physical_node_ids.size()) ||
        !PhysicalNodeCopyLiveBytes(node, &local_dispatch_metadata_live_bytes)) {
      return refuse(Refusal(
          "SBLR.PLAN_TREE.RESOURCE_LIMIT",
          "physical callback metadata accounting overflowed"));
    }
    allocation_bytes =
        static_cast<std::uint64_t>(node.input_physical_node_ids.size()) *
        sizeof(CanonicalPhysicalDispatchInput);
    if (allocation_bytes > std::numeric_limits<std::uint64_t>::max() -
                               local_dispatch_metadata_live_bytes) {
      return refuse(Refusal(
          "SBLR.PLAN_TREE.RESOURCE_LIMIT",
          "physical callback input carrier accounting overflowed"));
    }
    local_dispatch_metadata_live_bytes += allocation_bytes;
    for (const auto input_id : node.input_physical_node_ids) {
      const auto input_index = node_index_for(input_id);
      if (input_index == node_count ||
          result_index_by_node[input_index] == kNoResult) {
        return refuse(Refusal("SBLR.PLAN_TREE.INVALID_HANDLE",
                              "physical input result is unavailable"));
      }
      const auto& input_result =
          result.executed_steps[result_index_by_node[input_index]];
      if ((input_result.output_descriptor_ids.capacity() != 0 &&
           sizeof(std::uint32_t) >
               std::numeric_limits<std::uint64_t>::max() /
                   input_result.output_descriptor_ids.capacity())) {
        return refuse(Refusal(
            "SBLR.PLAN_TREE.RESOURCE_LIMIT",
            "physical callback descriptor carrier accounting overflowed"));
      }
      allocation_bytes = static_cast<std::uint64_t>(
                             input_result.output_descriptor_ids.capacity()) *
                         sizeof(std::uint32_t);
      if (allocation_bytes > std::numeric_limits<std::uint64_t>::max() -
                                 local_dispatch_metadata_live_bytes) {
        return refuse(Refusal(
            "SBLR.PLAN_TREE.RESOURCE_LIMIT",
            "physical callback input metadata accounting overflowed"));
      }
      local_dispatch_metadata_live_bytes += allocation_bytes;
      if (!PhysicalMgaContextDynamicLiveBytes(
              input_result.mga_statement_context,
              &local_dispatch_metadata_live_bytes)) {
        return refuse(Refusal(
            "SBLR.PLAN_TREE.RESOURCE_LIMIT",
            "physical callback input metadata accounting overflowed"));
      }
    }
    const auto account_local_string = [&](const std::string& value) {
      if (value.capacity() == std::numeric_limits<std::size_t>::max()) {
        return false;
      }
      const auto bytes = static_cast<std::uint64_t>(value.capacity()) + 1;
      if (bytes > std::numeric_limits<std::uint64_t>::max() -
                      local_dispatch_metadata_live_bytes) {
        return false;
      }
      local_dispatch_metadata_live_bytes += bytes;
      return true;
    };
    if (!account_local_string(node.implementation_id) ||
        (node.input_physical_node_ids.capacity() != 0 &&
         sizeof(std::uint64_t) >
             std::numeric_limits<std::uint64_t>::max() /
                 node.input_physical_node_ids.capacity())) {
      return refuse(Refusal(
          "SBLR.PLAN_TREE.RESOURCE_LIMIT",
          "physical step identity metadata accounting overflowed"));
    }
    allocation_bytes =
        static_cast<std::uint64_t>(node.input_physical_node_ids.capacity()) *
        sizeof(std::uint64_t);
    if (allocation_bytes > std::numeric_limits<std::uint64_t>::max() -
                               local_dispatch_metadata_live_bytes) {
      return refuse(Refusal(
          "SBLR.PLAN_TREE.RESOURCE_LIMIT",
          "physical step input identity accounting overflowed"));
    }
    local_dispatch_metadata_live_bytes += allocation_bytes;
    if (dispatcher_control_live_bytes > physical_dag.memory_budget_bytes ||
        retained_materialized_live_bytes >
            physical_dag.memory_budget_bytes -
                dispatcher_control_live_bytes ||
        retained_step_metadata_live_bytes >
            physical_dag.memory_budget_bytes -
                dispatcher_control_live_bytes -
                retained_materialized_live_bytes ||
        local_dispatch_metadata_live_bytes >
            physical_dag.memory_budget_bytes -
                dispatcher_control_live_bytes -
                retained_materialized_live_bytes -
                retained_step_metadata_live_bytes) {
      return refuse(Refusal(
          "SBLR.PLAN_TREE.RESOURCE_LIMIT",
          "physical callback metadata exceeds remaining statement memory"));
    }
    std::vector<CanonicalPhysicalDispatchInput> inputs;
    inputs.reserve(node.input_physical_node_ids.size());
    std::size_t materialized_input_rows = 0;
    std::uint64_t local_input_live_bytes = 0;
    for (const auto input_id : node.input_physical_node_ids) {
      const auto input_index = node_index_for(input_id);
      if (input_index == node_count ||
          result_index_by_node[input_index] == kNoResult) {
        return refuse(Refusal("SBLR.PLAN_TREE.INVALID_HANDLE",
                              "physical input result is unavailable"));
      }
      auto& input_result =
          result.executed_steps[result_index_by_node[input_index]];
      if (!input_result.materialized_output_batch.has_value()) {
        return refuse(Refusal(
            "QOW-DIAG-QRY-004-PHYSICAL-TYPED-BATCH-REQUIRED-V1",
            "selected physical input did not produce a typed batch"));
      }
      auto& remaining = remaining_consumer_count[input_index];
      if (remaining == 0 || materialized_live_by_node[input_index] == 0) {
        return refuse(Refusal(
            "SBLR.PLAN_TREE.INVALID_HANDLE",
            "physical input consumption lifecycle is inconsistent"));
      }
      const auto input_live_bytes =
          materialized_live_bytes_by_node[input_index];
      std::optional<DescriptorBatch> input_batch;
      if (remaining == 1) {
        if (input_live_bytes > retained_materialized_live_bytes) {
          return refuse(Refusal(
              "SBLR.PLAN_TREE.INVALID_HANDLE",
              "physical input ownership lifecycle is inconsistent"));
        }
        retained_materialized_live_bytes -= input_live_bytes;
        input_batch = std::move(input_result.materialized_output_batch);
        input_result.materialized_output_batch.reset();
        materialized_live_bytes_by_node[input_index] = 0;
        materialized_live_by_node[input_index] = 0;
      } else {
        if (dispatcher_control_live_bytes > physical_dag.memory_budget_bytes ||
            retained_materialized_live_bytes >
                physical_dag.memory_budget_bytes -
                    dispatcher_control_live_bytes ||
            retained_step_metadata_live_bytes >
                physical_dag.memory_budget_bytes -
                    dispatcher_control_live_bytes -
                    retained_materialized_live_bytes ||
            local_input_live_bytes >
                physical_dag.memory_budget_bytes -
                    dispatcher_control_live_bytes -
                    retained_materialized_live_bytes -
                    retained_step_metadata_live_bytes ||
            input_live_bytes >
                physical_dag.memory_budget_bytes -
                    dispatcher_control_live_bytes -
                    retained_materialized_live_bytes -
                    retained_step_metadata_live_bytes -
                    local_input_live_bytes) {
          return refuse(Refusal(
              "SBLR.PLAN_TREE.RESOURCE_LIMIT",
              "physical shared-input copy exceeds the remaining statement memory"));
        }
        input_batch = input_result.materialized_output_batch;
      }
      if (input_live_bytes >
          std::numeric_limits<std::uint64_t>::max() -
              local_input_live_bytes) {
        return refuse(Refusal(
            "SBLR.PLAN_TREE.RESOURCE_LIMIT",
            "physical callback input payload accounting overflowed"));
      }
      local_input_live_bytes += input_live_bytes;
      --remaining;
      inputs.push_back({input_result.executed_physical_node_id,
                        input_result.causal_counter_id,
                        input_result.result_handle_id,
                        input_result.output_descriptor_ids,
                        std::move(input_batch),
                        input_result.mga_statement_context});
      if (!inputs.back().materialized_output_batch.has_value() ||
          !CheckedAdd(
              materialized_input_rows,
              inputs.back().materialized_output_batch->rows.size(),
              &materialized_input_rows)) {
        return refuse(Refusal(
            "SBLR.PLAN_TREE.RESOURCE_LIMIT",
            "physical input row count overflowed"));
      }
      if (physical_dag.abi_version == 2 &&
          !PhysicalMgaStatementContextEqual(
              input_result.mga_statement_context,
              mga_authority.statement_context)) {
        return refuse(Refusal(
            "QOW-DIAG-MGA-DISPATCH-INPUT-CONTEXT-V1",
            "physical input batch is not bound to the selected MGA context"));
      }
    }
    std::uint64_t unrelated_retained_live_bytes =
        dispatcher_control_live_bytes;
    if (retained_materialized_live_bytes >
            std::numeric_limits<std::uint64_t>::max() -
                unrelated_retained_live_bytes) {
      return refuse(Refusal(
          "SBLR.PLAN_TREE.RESOURCE_LIMIT",
          "physical retained dispatcher accounting overflowed"));
    }
    unrelated_retained_live_bytes += retained_materialized_live_bytes;
    if (retained_step_metadata_live_bytes >
            std::numeric_limits<std::uint64_t>::max() -
                unrelated_retained_live_bytes) {
      return refuse(Refusal(
          "SBLR.PLAN_TREE.RESOURCE_LIMIT",
          "physical retained dispatcher accounting overflowed"));
    }
    unrelated_retained_live_bytes += retained_step_metadata_live_bytes;
    if (local_dispatch_metadata_live_bytes >
            std::numeric_limits<std::uint64_t>::max() -
                unrelated_retained_live_bytes) {
      return refuse(Refusal(
          "SBLR.PLAN_TREE.RESOURCE_LIMIT",
          "physical retained dispatcher accounting overflowed"));
    }
    unrelated_retained_live_bytes += local_dispatch_metadata_live_bytes;
    if (unrelated_retained_live_bytes >=
            physical_dag.memory_budget_bytes ||
        local_input_live_bytes >
            physical_dag.memory_budget_bytes -
                unrelated_retained_live_bytes) {
      return refuse(Refusal(
          "SBLR.PLAN_TREE.RESOURCE_LIMIT",
          "physical callback inputs exceed the remaining statement memory"));
    }
    const auto available_callback_memory_bytes =
        physical_dag.memory_budget_bytes -
        unrelated_retained_live_bytes;
    auto callback_node = node;
    callback_node.dispatcher_callback_memory_limit_bytes =
        available_callback_memory_bytes;
    const auto* callback_registration = executor_for(node.implementation_id);
    if (!callback_registration->honors_dispatcher_memory_limit_v1 &&
        node.memory_bytes_required > available_callback_memory_bytes) {
      return refuse(Refusal(
          "SBLR.PLAN_TREE.RESOURCE_LIMIT",
          "selected executor grant exceeds the dispatcher callback allowance"));
    }

    CanonicalPhysicalDispatchStepResult step;
    const auto pre_callback_authority =
        RevalidateCanonicalExecutionMgaAuthority(
            mga_authority, physical_dag, request.limits);
    if (!pre_callback_authority.ok) {
      return refuse(pre_callback_authority);
    }
    execution_started = true;
    const auto callback_started = std::chrono::steady_clock::now();
    try {
      step = callback_registration->execute(physical_dag,
                                            callback_node, inputs);
    } catch (const std::exception& exception) {
      data_access_observed = true;
      return refuse(Refusal(
          "QOW-DIAG-QRY-004-PHYSICAL-EXECUTOR-FAILURE-V1",
          std::string("physical executor threw: ") + exception.what()));
    } catch (...) {
      data_access_observed = true;
      return refuse(Refusal(
          "QOW-DIAG-QRY-004-PHYSICAL-EXECUTOR-FAILURE-V1",
          "physical executor threw a non-standard exception"));
    }
    const auto callback_finished = std::chrono::steady_clock::now();
    if (step.runtime_observation.elapsed_ns.state !=
            CanonicalRuntimeMetricState::kUnavailable ||
        step.runtime_observation.elapsed_ns.value != 0 ||
        step.runtime_observation.dispatcher_elapsed_frozen ||
        step.runtime_observation.counters_frozen_after_finish ||
        step.execution_started || step.execution_finished ||
        step.counters_captured_after_finish || step.execution_ordinal != 0) {
      return refuse(Refusal(
          "QOW-DIAG-OPT-017-REFUSAL-V1",
          "runtime_observation_forged_dispatcher_or_lifecycle_evidence"));
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
        callback_finished - callback_started).count();
    if (elapsed < 0 ||
        static_cast<std::uintmax_t>(elapsed) >
            std::numeric_limits<std::uint64_t>::max()) {
      return refuse(Refusal("QOW-DIAG-OPT-017-REFUSAL-V1",
                            "runtime_observation_elapsed_overflow"));
    }
    step.runtime_observation.elapsed_ns = {
        CanonicalRuntimeMetricState::kObserved,
        static_cast<std::uint64_t>(elapsed)};
    step.runtime_observation.dispatcher_elapsed_frozen = true;
    if (step.diagnostic.ok &&
        executor_for(node.implementation_id)
            ->publishes_runtime_observation_v1) {
      // The callback receipt already covers its local inputs, output, and
      // work state. Add only payloads retained for unrelated/shared branches
      // so the validated peak represents the complete dispatcher live set.
      if (step.runtime_observation.peak_memory_bytes.state !=
              CanonicalRuntimeMetricState::kObserved ||
          unrelated_retained_live_bytes >
              std::numeric_limits<std::uint64_t>::max() -
                  step.runtime_observation.peak_memory_bytes.value) {
        return refuse(Refusal(
            "QOW-DIAG-OPT-017-REFUSAL-V1",
            "runtime_observation_dispatcher_retained_memory_overflow"));
      }
      step.runtime_observation.peak_memory_bytes.value +=
          unrelated_retained_live_bytes;
    }

    // QOW-SOURCE-QRY-004-DATA-ACCESS-OBSERVATION-V1
    // Preserve an executor's explicit observation even when its diagnostic
    // refuses the step. Unknown legacy callbacks retain the conservative
    // callback-started fallback.
    data_access_observed =
        data_access_observed ||
        (step.data_access_observation_known ? step.data_access_observed
                                            : execution_started);

    const auto post_callback_authority =
        RevalidateCanonicalExecutionMgaAuthority(
            mga_authority, physical_dag, request.limits);
    if (!post_callback_authority.ok) {
      return refuse(post_callback_authority);
    }

    if (step.cancellation_observed) {
      const PhysicalAdmissionEvidence* cancellation_policy = nullptr;
      bool duplicate_cancellation_policy = false;
      for (const auto& evidence : physical_dag.admission_evidence) {
        if (evidence.stage != PhysicalAdmissionStage::kPolicyCapability) {
          continue;
        }
        if (cancellation_policy != nullptr) {
          duplicate_cancellation_policy = true;
          break;
        }
        cancellation_policy = &evidence;
      }
      const bool exact_selected_identity =
          step.selected_plan_uuid == physical_dag.selected_plan_uuid &&
          step.executed_physical_node_id == node.physical_node_id &&
          step.causal_counter_id == node.causal_counter_id &&
          step.output_descriptor_ids == node.output_descriptor_ids &&
          step.authority.engine_mga_snapshot_bound &&
          (physical_dag.abi_version != 2 ||
           PhysicalMgaStatementContextEqual(
               step.mga_statement_context,
               mga_authority.statement_context)) &&
          !HasForbiddenAuthority(step.authority);
      if (!step.transient_state_cleanup_proven || step.diagnostic.ok ||
          step.result_handle_id != 0 ||
          step.materialized_output_batch.has_value() ||
          !exact_selected_identity || cancellation_policy == nullptr ||
          duplicate_cancellation_policy ||
          step.cancellation_evidence_uuid !=
              cancellation_policy->evidence_uuid) {
        return refuse(Refusal(
            "QOW-DIAG-QRY-004-PHYSICAL-CANCELLATION-RECEIPT-V1",
            "selected executor cancellation receipt lacks exact identity, "
            "policy evidence, or atomic cleanup"));
      }
      cancellation_observed = true;
      const auto cancellation_detail =
          step.diagnostic.detail.empty()
              ? std::string(
                    "physical DAG cancellation observed inside a selected "
                    "node")
              : step.diagnostic.detail;
      return refuse(Refusal(
          "QOW-DIAG-QRY-004-PHYSICAL-DISPATCH-CANCELLED-V1",
          cancellation_detail));
    }
    if (!step.diagnostic.ok) {
      return refuse(std::move(step.diagnostic));
    }
    const auto post_step_cancellation =
        cancellation_diagnostic("after a selected node");
    if (!post_step_cancellation.ok) return refuse(post_step_cancellation);
    step.execution_ordinal = result.executed_steps.size() + 1;
    step.executed_relational_node_id = node.relational_node_id;
    step.executed_implementation_id = node.implementation_id;
    step.executed_input_physical_node_ids = node.input_physical_node_ids;
    step.execution_started = true;
    step.execution_finished = true;
    step.counters_captured_after_finish = true;
    step.runtime_observation.counters_frozen_after_finish = true;
    if (step.selected_plan_uuid != physical_dag.selected_plan_uuid ||
        step.executed_physical_node_id != node.physical_node_id ||
        step.causal_counter_id != node.causal_counter_id ||
        step.result_handle_id == 0 ||
        step.output_descriptor_ids != node.output_descriptor_ids ||
        !step.authority.engine_mga_snapshot_bound ||
        (physical_dag.abi_version == 2 &&
         !PhysicalMgaStatementContextEqual(
             step.mga_statement_context,
             mga_authority.statement_context)) ||
        HasForbiddenAuthority(step.authority)) {
      return refuse(Refusal(
          "QOW-DIAG-QRY-004-PHYSICAL-EXECUTION-EVIDENCE-V1",
          "physical executor result does not match admitted node evidence"));
    }
    if (executor_for(node.implementation_id)
            ->publishes_runtime_observation_v1) {
      const auto observation_validation = ValidateRuntimeObservation(
          step.runtime_observation, physical_dag.memory_budget_bytes,
          step.pages_read, step.spill_bytes,
          step.data_access_observation_known, step.data_access_observed);
      if (!observation_validation.ok) {
        return refuse(observation_validation);
      }
    }
    if (!step.materialized_output_batch.has_value()) {
      return refuse(Refusal(
          "QOW-DIAG-QRY-004-PHYSICAL-TYPED-BATCH-REQUIRED-V1",
          "selected physical node returned only an opaque result handle"));
    }
    if (step.materialized_output_batch.has_value()) {
      std::size_t next_total_rows = 0;
      std::size_t next_total_cells = 0;
      const auto batch_validation = ValidateMaterializedBatch(
          *step.materialized_output_batch, node.output_descriptor_ids,
          request.runtime_limits, total_materialized_rows,
          total_materialized_cells, &next_total_rows, &next_total_cells);
      if (!batch_validation.ok) return refuse(batch_validation);
      if (step.output_row_count !=
          step.materialized_output_batch->rows.size()) {
        return refuse(Refusal(
            "QOW-DIAG-QRY-004-PHYSICAL-CAUSAL-COUNTER-V1",
            "physical output-row counter differs from the typed batch"));
      }
      total_materialized_rows = next_total_rows;
      total_materialized_cells = next_total_cells;
      std::uint64_t materialized_live_bytes = 0;
      std::uint64_t step_metadata_live_bytes = 0;
      if (!MaterializedBatchLiveBytes(*step.materialized_output_batch,
                                      &materialized_live_bytes) ||
          !DispatchStepMetadataLiveBytes(step,
                                         &step_metadata_live_bytes) ||
          dispatcher_control_live_bytes > physical_dag.memory_budget_bytes ||
          retained_materialized_live_bytes >
              physical_dag.memory_budget_bytes -
                  dispatcher_control_live_bytes ||
          retained_step_metadata_live_bytes >
              physical_dag.memory_budget_bytes -
                  dispatcher_control_live_bytes -
                  retained_materialized_live_bytes ||
          materialized_live_bytes >
              physical_dag.memory_budget_bytes -
                  dispatcher_control_live_bytes -
                  retained_materialized_live_bytes -
                  retained_step_metadata_live_bytes ||
          step_metadata_live_bytes >
              physical_dag.memory_budget_bytes -
                  dispatcher_control_live_bytes -
                  retained_materialized_live_bytes -
                  retained_step_metadata_live_bytes -
                  materialized_live_bytes ||
          materialized_live_bytes >
              std::numeric_limits<std::uint64_t>::max() -
                  retained_materialized_live_bytes ||
          step_metadata_live_bytes >
              std::numeric_limits<std::uint64_t>::max() -
                  retained_step_metadata_live_bytes ||
          materialized_live_by_node[node_index] != 0) {
        return refuse(Refusal(
            "SBLR.PLAN_TREE.RESOURCE_LIMIT",
            "physical materialized payload accounting overflowed"));
      }
      retained_materialized_live_bytes += materialized_live_bytes;
      retained_step_metadata_live_bytes += step_metadata_live_bytes;
      materialized_live_bytes_by_node[node_index] = materialized_live_bytes;
      materialized_live_by_node[node_index] = 1;
    }
    if (step.input_row_count != materialized_input_rows) {
      return refuse(Refusal(
          "QOW-DIAG-QRY-004-PHYSICAL-CAUSAL-COUNTER-V1",
          "physical input-row counter differs from causal input batches"));
    }
    if ((node.node_kind == PhysicalNodeKind::kFilter ||
         node.node_kind == PhysicalNodeKind::kProject) &&
        step.rows_examined != materialized_input_rows) {
      return refuse(Refusal(
          "QOW-DIAG-QRY-004-PHYSICAL-CAUSAL-COUNTER-V1",
          "row operator examination counter differs from its typed input"));
    }
    if (node.node_kind == PhysicalNodeKind::kFilter &&
        step.output_row_count > materialized_input_rows) {
      return refuse(Refusal(
          "QOW-DIAG-QRY-004-PHYSICAL-CAUSAL-COUNTER-V1",
          "filter output exceeds its causal typed input"));
    }
    if (node.node_kind == PhysicalNodeKind::kProject &&
        step.output_row_count != materialized_input_rows) {
      return refuse(Refusal(
          "QOW-DIAG-QRY-004-PHYSICAL-CAUSAL-COUNTER-V1",
          "project output cardinality differs from its causal typed input"));
    }
    result_index_by_node[node_index] = result.executed_steps.size();
    result.executed_steps.push_back(std::move(step));
  }

  if (root_node_index == node_count ||
      result_index_by_node[root_node_index] == kNoResult) {
    return refuse(Refusal("SBLR.PLAN_TREE.INVALID_HANDLE",
                          "physical root result is unavailable"));
  }
  const auto publication_cancellation =
      cancellation_diagnostic("before root publication");
  if (!publication_cancellation.ok) return refuse(publication_cancellation);
  const auto root_authority = RevalidateCanonicalExecutionMgaAuthority(
      mga_authority, physical_dag, request.limits);
  if (!root_authority.ok) {
    return refuse(root_authority);
  }
  const auto& root =
      result.executed_steps[result_index_by_node[root_node_index]];
  result.diagnostic = {};
  result.root_result_handle_id = root.result_handle_id;
  result.root_output_descriptor_ids = root.output_descriptor_ids;
  result.authority.engine_mga_snapshot_bound = true;
  result.execution_started = true;
  result.data_access_observed = data_access_observed;
  result.cancellation_observed = false;
  result.selected_plan_uuid = physical_dag.selected_plan_uuid;
  result.executed_root_physical_node_id = root.executed_physical_node_id;
  result.root_causal_counter_id = root.causal_counter_id;
  result.mga_statement_context = mga_authority.statement_context;
  return result;
}

}  // namespace scratchbird::engine::executor
