// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "canonical_query_multileg_composition.hpp"
#include "canonical_query_aggregate_registration.hpp"
#include "canonical_query_correlated_registration.hpp"
#include "canonical_query_descriptor_support.hpp"
#include "canonical_query_document_composition.hpp"
#include "canonical_query_graph_composition.hpp"
#include "canonical_query_join_registration.hpp"
#include "canonical_query_key_value_composition.hpp"
#include "canonical_query_model_family_composition_support.hpp"
#include "canonical_query_node_composition.hpp"
#include "canonical_query_object_free_profile.hpp"
#include "canonical_query_object_free_composition_support.hpp"
#include "canonical_query_physical_registration.hpp"
#include "canonical_query_relational_registration.hpp"
#include "canonical_query_runtime_memory_support.hpp"
#include "canonical_query_scalar_support.hpp"
#include "canonical_query_search_composition.hpp"
#include "canonical_query_spatial_columnar_composition.hpp"
#include "canonical_query_time_series_composition.hpp"
#include "canonical_query_vector_composition.hpp"

#include "canonical_relational_expression.hpp"
#include "sblr_dispatch.hpp"

#include "catalog/name_resolution_api.hpp"
#include "engine/executor/executor_foundation.hpp"
#include "engine/executor/model_family_executor.hpp"
#include "engine/internal_api/mga_relation_store/mga_relation_descriptor.hpp"
#include "engine/optimizer/model_family_coordinator.hpp"
#include "engine/optimizer/model_family_profile_factory.hpp"
#include "engine/optimizer/optimizer_contract.hpp"
#include "engine/optimizer/relational_planner.hpp"
#include "mga_relation_store/mga_relation_store.hpp"
#include "nosql/columnar_api.hpp"
#include "nosql/document_api.hpp"
#include "nosql/graph_api.hpp"
#include "nosql/key_value_api.hpp"
#include "nosql/nosql_provider_generation_store.hpp"
#include "nosql/spatial_api.hpp"
#include "nosql/time_series_api.hpp"
#include "nosql/vector_api.hpp"
#include "nosql/search_api.hpp"
#include "query/canonical_heap_optimizer_admission.hpp"
#include "query/canonical_relational_bridge.hpp"
#include "query/expression_api.hpp"
#include "security/security_model.hpp"
#include "transaction/transaction_api.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <exception>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <new>
#include <optional>
#include <ranges>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace scratchbird::engine::sblr {
namespace api = scratchbird::engine::internal_api;
namespace exec = scratchbird::engine::executor;
namespace opt = scratchbird::engine::optimizer;
namespace plan = scratchbird::engine::planner;

// SEARCH_KEY: SB_ENGINE_CANONICAL_QUERY_MULTILEG_COMPOSITION_AUTHORITY
// Owns captured model-source composition, exact result descriptor rebinding,
// ASOF joins, and bounded multi-leg execution over engine-issued MGA context.
// Transaction finality and public query route selection remain engine-owned.

namespace {
std::optional<std::string> Rcp079DescriptorField(
    const std::string_view encoded, const std::string_view key) {
  const std::string prefix = std::string(key) + "=";
  std::optional<std::string> value;
  std::size_t offset = 0;
  while (offset <= encoded.size()) {
    const auto separator = encoded.find(';', offset);
    const auto end = separator == std::string_view::npos
                         ? encoded.size()
                         : separator;
    const auto field = encoded.substr(offset, end - offset);
    if (field.starts_with(prefix)) {
      if (value.has_value()) return std::nullopt;
      value = std::string(field.substr(prefix.size()));
    }
    if (separator == std::string_view::npos) break;
    offset = separator + 1;
  }
  return value;
}

void BindCanonicalPersistedRowDescriptorAuthorityForMultilegV1(
    const api::EngineRequestContext& context,
    CanonicalRelationalExpressionRuntimeServices* services) {
  if (services == nullptr) return;
  services->persisted_row_descriptor_authority =
      [context = &context](
          const std::uint32_t,
          const api::RelationalTypeDescriptor& bound,
          const api::EngineDescriptor& persisted,
          const api::RelationalNullability effective_nullability,
          std::string* refusal_detail) {
        return ValidateCanonicalPersistedTextRowDescriptorAuthorityV1(
            *context, bound, persisted, effective_nullability,
            refusal_detail);
      };
}

exec::CanonicalPhysicalExecutorRegistration
WithMultilegResultDescriptorRebindingV1(
    exec::CanonicalPhysicalExecutorRegistration registration,
    std::vector<opt::MultilegDescriptorAllocationV1> allocations,
    std::string operation_name,
    std::function<bool()> cancellation_requested) {
  auto execute = std::move(registration.execute);
  registration.execute =
      [execute = std::move(execute), allocations = std::move(allocations),
       operation_name = std::move(operation_name),
       cancellation_requested = std::move(cancellation_requested)](
          const exec::TypedPhysicalNodeDag& dag,
          const exec::PhysicalNodeRecord& node,
          const std::vector<exec::CanonicalPhysicalDispatchInput>& inputs)
          mutable {
        auto step = execute(dag, node, inputs);
        if (!step.diagnostic.ok) return step;
        const exec::PhysicalAdmissionEvidence* cancellation_policy = nullptr;
        for (const auto& evidence : dag.admission_evidence) {
          if (evidence.stage !=
              exec::PhysicalAdmissionStage::kPolicyCapability) {
            continue;
          }
          if (cancellation_policy != nullptr) {
            cancellation_policy = nullptr;
            break;
          }
          cancellation_policy = &evidence;
        }
        const auto poll_cancellation = [&](const char* phase) {
          try {
            if (!cancellation_requested || !cancellation_requested()) {
              return false;
            }
            step.diagnostic.ok = false;
            step.diagnostic.diagnostic_code =
                "QOW-DIAG-QRY-012-JOIN-CANCELLED-V1";
            step.diagnostic.detail = operation_name +
                                     " cancellation observed " + phase;
            step.cancellation_observed = true;
            step.transient_state_cleanup_proven = true;
            step.cancellation_evidence_uuid =
                cancellation_policy == nullptr
                    ? std::string{}
                    : cancellation_policy->evidence_uuid;
          } catch (const std::exception& exception) {
            step.diagnostic.ok = false;
            step.diagnostic.diagnostic_code =
                "QOW-DIAG-QRY-004-PHYSICAL-CANCELLATION-PROBE-V1";
            step.diagnostic.detail = operation_name +
                                     " cancellation probe threw: " +
                                     exception.what();
            step.transient_state_cleanup_proven = true;
          } catch (...) {
            step.diagnostic.ok = false;
            step.diagnostic.diagnostic_code =
                "QOW-DIAG-QRY-004-PHYSICAL-CANCELLATION-PROBE-V1";
            step.diagnostic.detail = operation_name +
                " cancellation probe threw a non-standard exception";
            step.transient_state_cleanup_proven = true;
          }
          step.result_handle_id = 0;
          step.output_row_count = 0;
          step.materialized_output_batch.reset();
          return true;
        };
        if (poll_cancellation("before descriptor rebinding")) return step;
        if (!step.materialized_output_batch.has_value() ||
            allocations.size() !=
                step.materialized_output_batch->columns.size() ||
            allocations.size() != node.output_descriptor_ids.size()) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "SB_MODEL_RESULT_DESCRIPTOR_DEMAND_INVALID_V1";
          step.diagnostic.detail =
              operation_name + " publication allocation width changed";
          step.materialized_output_batch.reset();
          return step;
        }
        auto& batch = *step.materialized_output_batch;
        std::uint64_t projected_memory_bytes = 1;
        // Dispatcher inputs remain live throughout wrapper execution, so the
        // rebound publication peak must charge them before output metadata.
        for (const auto& input : inputs) {
          if (!input.materialized_output_batch.has_value()) {
            step.diagnostic.ok = false;
            step.diagnostic.diagnostic_code =
                "SB_MODEL_RESULT_DESCRIPTOR_DEMAND_INVALID_V1";
            step.diagnostic.detail = operation_name +
                " descriptor rebinding input batch is absent";
            step.materialized_output_batch.reset();
            return step;
          }
          for (std::size_t row = 0;
               row < input.materialized_output_batch->rows.size(); ++row) {
            for (std::size_t column = 0;
                 column < input.materialized_output_batch->rows[row]
                              .values.size();
                 ++column) {
              if (poll_cancellation(
                      "while accounting rebound input payload")) {
                return step;
              }
              const auto& value =
                  input.materialized_output_batch->rows[row].values[column];
              if (!CheckedAdd(projected_memory_bytes,
                              value.encoded_value.size(),
                              &projected_memory_bytes) ||
                  !CheckedAdd(projected_memory_bytes,
                              value.binary_value.size(),
                              &projected_memory_bytes)) {
                step.diagnostic.ok = false;
                step.diagnostic.diagnostic_code =
                    "SBLR.PLAN_TREE.RESOURCE_LIMIT";
                step.diagnostic.detail = operation_name +
                    " retained input payload size overflowed";
                step.materialized_output_batch.reset();
                return step;
              }
            }
          }
        }
        for (std::size_t row = 0; row < batch.rows.size(); ++row) {
          for (std::size_t column = 0;
               column < batch.rows[row].values.size(); ++column) {
            if (poll_cancellation("while accounting rebound result payload")) {
              return step;
            }
            const auto& value = batch.rows[row].values[column];
            if (!CheckedAdd(projected_memory_bytes,
                            value.encoded_value.size(),
                            &projected_memory_bytes) ||
                !CheckedAdd(projected_memory_bytes,
                            value.binary_value.size(),
                            &projected_memory_bytes)) {
              step.diagnostic.ok = false;
              step.diagnostic.diagnostic_code =
                  "SBLR.PLAN_TREE.RESOURCE_LIMIT";
              step.diagnostic.detail = operation_name +
                                       " result payload size overflowed";
              step.materialized_output_batch.reset();
              return step;
            }
          }
        }
        for (std::size_t ordinal = 0; ordinal < allocations.size(); ++ordinal) {
          if (poll_cancellation("while planning descriptor rebinding")) {
            return step;
          }
          const auto& allocation = allocations[ordinal];
          if (!allocation.demand.derived) continue;
          const auto encoded_size =
              std::string("type_uuid=").size() + allocation.type_uuid.size() +
              std::string(";nullability=").size() +
              std::string(allocation.demand.nullable ? "nullable"
                                                     : "non_null").size();
          std::uint64_t descriptor_bytes = 0;
          if (!CheckedAdd(allocation.descriptor_uuid.size(),
                          std::string("scalar").size(), &descriptor_bytes) ||
              !CheckedAdd(descriptor_bytes,
                          allocation.demand.canonical_type_name.size(),
                          &descriptor_bytes) ||
              !CheckedAdd(descriptor_bytes, encoded_size,
                          &descriptor_bytes) ||
              !CheckedAdd(projected_memory_bytes, descriptor_bytes,
                          &projected_memory_bytes)) {
            step.diagnostic.ok = false;
            step.diagnostic.diagnostic_code =
                "SBLR.PLAN_TREE.RESOURCE_LIMIT";
            step.diagnostic.detail = operation_name +
                                     " descriptor rebound size overflowed";
            step.materialized_output_batch.reset();
            return step;
          }
          for (std::size_t row = 0; row < batch.rows.size(); ++row) {
            if (poll_cancellation("while planning value rebinding")) {
              return step;
            }
            if (ordinal >= batch.rows[row].values.size() ||
                !CheckedAdd(projected_memory_bytes, descriptor_bytes,
                            &projected_memory_bytes)) {
              step.diagnostic.ok = false;
              step.diagnostic.diagnostic_code =
                  ordinal >= batch.rows[row].values.size()
                      ? "SB_MODEL_RESULT_DESCRIPTOR_DEMAND_INVALID_V1"
                      : "SBLR.PLAN_TREE.RESOURCE_LIMIT";
              step.diagnostic.detail = operation_name +
                  " descriptor rebound row width or size changed";
              step.materialized_output_batch.reset();
              return step;
            }
          }
        }
        if (node.memory_bytes_required == 0 ||
            node.memory_bytes_required > dag.memory_budget_bytes ||
            projected_memory_bytes > node.memory_bytes_required) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "SBLR.PLAN_TREE.RESOURCE_LIMIT";
          step.diagnostic.detail = operation_name +
              " descriptor-rebound output exceeds the selected-node grant";
          step.materialized_output_batch.reset();
          return step;
        }
        for (std::size_t ordinal = 0; ordinal < allocations.size(); ++ordinal) {
          if (poll_cancellation("while rebinding result descriptors")) {
            return step;
          }
          const auto& allocation = allocations[ordinal];
          auto& column = batch.columns[ordinal];
          if (allocation.demand.derived) {
            api::EngineDescriptor rebound;
            rebound.descriptor_uuid.canonical = allocation.descriptor_uuid;
            rebound.descriptor_kind = "scalar";
            rebound.canonical_type_name =
                allocation.demand.canonical_type_name;
            rebound.encoded_descriptor =
                "type_uuid=" + allocation.type_uuid + ";nullability=" +
                (allocation.demand.nullable ? "nullable" : "non_null");
            column.descriptor = std::move(rebound);
            column.nullable = allocation.demand.nullable;
          } else if (column.descriptor.descriptor_uuid.canonical !=
                         allocation.descriptor_uuid ||
                     allocation.type_uuid.empty() ||
                     Rcp079DescriptorField(
                         column.descriptor.encoded_descriptor,
                         "type_uuid") !=
                         std::optional<std::string>(allocation.type_uuid) ||
                     column.nullable != allocation.demand.nullable) {
            step.diagnostic.ok = false;
            step.diagnostic.diagnostic_code =
                "SB_MODEL_RESULT_DESCRIPTOR_SOURCE_BINDING_INVALID_V1";
            step.diagnostic.detail = operation_name +
                                     " persisted publication descriptor changed";
            step.materialized_output_batch.reset();
            return step;
          }
          for (auto& row : batch.rows) {
            if (poll_cancellation("while rebinding result values")) {
              return step;
            }
            if (ordinal >= row.values.size()) {
              step.diagnostic.ok = false;
              step.diagnostic.diagnostic_code =
                  "SB_MODEL_RESULT_DESCRIPTOR_DEMAND_INVALID_V1";
              step.diagnostic.detail =
                  operation_name + " publication row width changed";
              step.materialized_output_batch.reset();
              return step;
            }
            row.values[ordinal].descriptor = column.descriptor;
          }
        }
        bool validation_cancelled = false;
        const auto validated = exec::ValidateCanonicalDescriptorBatch(
            batch, node.output_descriptor_ids, cancellation_requested,
            &validation_cancelled);
        if (!validated.ok) {
          step.diagnostic = validated;
          if (validated.diagnostic_code ==
                  "QOW-DIAG-QRY-012-JOIN-CANCELLED-V1" ||
              validated.diagnostic_code ==
                  "QOW-DIAG-QRY-012-CANCELLATION-PROBE-V1") {
            step.cancellation_observed = validation_cancelled;
            step.transient_state_cleanup_proven = true;
            step.cancellation_evidence_uuid =
                validation_cancelled && cancellation_policy != nullptr
                    ? cancellation_policy->evidence_uuid
                    : std::string{};
          }
          step.result_handle_id = 0;
          step.output_row_count = 0;
          step.materialized_output_batch.reset();
          return step;
        }
        if (poll_cancellation("after descriptor rebinding")) return step;
        return step;
      };
  return registration;
}
}  // namespace

void CaptureRcp079ModelLegV1(
    Rcp079CapturedModelLegV1* capture, const std::uint32_t logical_node_id,
    std::string family_id, std::string implementation_id,
    std::string transformation_rule_id, std::string compatibility_profile_id,
    std::string relation_descriptor_uuid,
    const std::uint64_t relation_descriptor_generation,
    const plan::CanonicalLogicalRelationalNodeKind logical_node_kind,
    const exec::PhysicalNodeKind physical_node_kind,
    const exec::ModelFamilyExecutionRequestV1& execution_request) {
  if (capture == nullptr) return;
  capture->captured = true;
  capture->logical_node_id = logical_node_id;
  capture->family_id = std::move(family_id);
  capture->implementation_id = std::move(implementation_id);
  capture->capability_uuid = execution_request.input.capability_uuid;
  capture->transformation_rule_id = std::move(transformation_rule_id);
  capture->compatibility_profile_id = std::move(compatibility_profile_id);
  capture->current_relation_descriptor_uuid =
      std::move(relation_descriptor_uuid);
  capture->current_relation_descriptor_generation =
      relation_descriptor_generation;
  capture->logical_node_kind = logical_node_kind;
  capture->physical_node_kind = physical_node_kind;
  capture->execution_request = execution_request;
}

namespace {

exec::CanonicalPhysicalExecutorRegistration
MakeRcp079CapturedModelLegRegistration(
    const std::vector<Rcp079CapturedModelLegV1>& captured_legs) {
  exec::CanonicalPhysicalExecutorRegistration registration;
  if (captured_legs.empty()) return registration;
  const auto& representative = captured_legs.front();
  const bool strict_dispatcher_memory =
      std::ranges::all_of(captured_legs, [](const auto& leg) {
        return leg.columnar_runtime_memory_receipt != nullptr &&
               leg.columnar_runtime_memory_receipt->complete &&
               leg.columnar_runtime_memory_receipt
                       ->provider_logical_memory_bytes != 0;
      });
  std::uint64_t registration_retained_bytes =
      sizeof(std::vector<Rcp079CapturedModelLegV1>) + 512;
  std::uint64_t captured_leg_bytes = 0;
  if (!strict_dispatcher_memory ||
      !CheckedMultiply(captured_legs.capacity(),
                       sizeof(Rcp079CapturedModelLegV1),
                       &captured_leg_bytes) ||
      !CheckedAdd(registration_retained_bytes, captured_leg_bytes,
                  &registration_retained_bytes)) {
    registration_retained_bytes = 0;
  }
  for (const auto& leg : captured_legs) {
    if (registration_retained_bytes == 0 ||
        !CheckedAdd(registration_retained_bytes,
                    leg.columnar_runtime_memory_receipt
                        ->provider_logical_memory_bytes,
                    &registration_retained_bytes)) {
      registration_retained_bytes = 0;
      break;
    }
  }
  registration.node_kind = representative.physical_node_kind;
  registration.implementation_id = representative.implementation_id;
  registration.executor_capability_uuid = representative.capability_uuid;
  registration.executor_capability_abi_version = 1;
  registration.engine_owned = true;
  registration.accepts_optimizer_publication_v2 = true;
  registration.publishes_runtime_observation_v1 =
      strict_dispatcher_memory;
  registration.honors_dispatcher_memory_limit_v1 =
      strict_dispatcher_memory;
  registration.retained_live_memory_bytes_v1 =
      registration_retained_bytes;
  registration.execute =
      [captured_legs, strict_dispatcher_memory](
                 const exec::TypedPhysicalNodeDag& selected_dag,
                 const exec::PhysicalNodeRecord& selected_node,
                 const std::vector<exec::CanonicalPhysicalDispatchInput>&
                     inputs) mutable {
        exec::CanonicalPhysicalDispatchStepResult step;
        step.selected_plan_uuid = selected_dag.selected_plan_uuid;
        step.executed_physical_node_id = selected_node.physical_node_id;
        step.causal_counter_id = selected_node.causal_counter_id;
        step.output_descriptor_ids = selected_node.output_descriptor_ids;
        step.mga_statement_context = selected_dag.mga_statement_context;
        step.authority.engine_mga_snapshot_bound = true;
        step.data_access_observation_known = true;
        const auto captured = std::ranges::find_if(
            captured_legs, [&](const auto& candidate) {
              return candidate.logical_node_id == selected_node.relational_node_id;
            });
        if (!inputs.empty() || captured == captured_legs.end() ||
            !captured->captured ||
            selected_node.memory_bytes_required == 0 ||
            selected_node.memory_bytes_required >
                selected_dag.memory_budget_bytes ||
            selected_node.memory_bytes_required !=
                captured->execution_request.input.maximum_memory_bytes ||
            selected_node.implementation_id != captured->implementation_id ||
            selected_node.executor_capability_uuid !=
                captured->capability_uuid ||
            selected_node.output_descriptor_ids !=
                captured->execution_request.input.output_descriptor_ids ||
            !exec::PhysicalMgaStatementContextEqual(
                selected_dag.mga_statement_context,
                captured->execution_request.input.mga_statement_context)) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "SB_MODEL_TYPED_EXCHANGE_INVALID_V1";
          step.diagnostic.detail =
              "selected model-family composition leg was substituted";
          return step;
        }
        if (strict_dispatcher_memory) {
          const auto callback_memory_bound =
              SelectedNodeAggregateMemoryBound(selected_dag, selected_node);
          if (!callback_memory_bound.has_value() ||
              captured->columnar_runtime_memory_receipt == nullptr ||
              captured->columnar_runtime_memory_receipt
                      ->peak_live_memory_bytes > *callback_memory_bound) {
            step.diagnostic.ok = false;
            step.diagnostic.diagnostic_code =
                "SBLR.PLAN_TREE.RESOURCE_LIMIT";
            step.diagnostic.detail =
                "captured model-family leg exceeds the dispatcher callback "
                "memory allowance";
            return step;
          }
        }
        auto request = captured->execution_request;
        request.input.physical_node_id = selected_node.physical_node_id;
        request.input.selected_alternative_uuid =
            selected_node.selected_alternative_uuid;
        request.input.capability_uuid =
            selected_node.executor_capability_uuid;
        request.input.causal_counter_id = selected_node.causal_counter_id;
        request.capability.capability_uuid =
            selected_node.executor_capability_uuid;
        request.current_mga_statement_context =
            selected_dag.mga_statement_context;
        const auto provider = request.execute_provider;
        const auto runtime_input = request.input;
        request.execute_provider =
            [provider, runtime_input](
                const exec::ModelSourceInputDescriptorV1& input) mutable {
              auto produced = provider(input);
              if (produced.ok) {
                produced.provider_batch.selected_alternative_uuid =
                    runtime_input.selected_alternative_uuid;
                produced.provider_batch.capability_uuid =
                    runtime_input.capability_uuid;
                produced.provider_batch.causal_counter_id =
                    runtime_input.causal_counter_id;
                produced.provider_batch.output_descriptor_ids =
                    runtime_input.output_descriptor_ids;
                produced.provider_batch.mga_statement_context =
                    runtime_input.mga_statement_context;
              }
              return produced;
            };
        const auto executed = exec::ExecuteModelFamilySourceV1(request);
        step.data_access_observed = executed.data_access_observed;
        if (captured->cancellation_probe_failed != nullptr &&
            captured->cancellation_probe_failed->load(
                std::memory_order_relaxed)) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "SB_MODEL_COORDINATOR_LEG_FAILED_V1";
          step.diagnostic.detail =
              "model-family cancellation probe failed";
          return step;
        }
        if (executed.diagnostic_id == "SB_MODEL_EXECUTION_CANCELLED_V1") {
          const exec::PhysicalAdmissionEvidence* cancellation_policy = nullptr;
          for (const auto& evidence : selected_dag.admission_evidence) {
            if (evidence.stage !=
                exec::PhysicalAdmissionStage::kPolicyCapability) {
              continue;
            }
            if (cancellation_policy != nullptr) {
              cancellation_policy = nullptr;
              break;
            }
            cancellation_policy = &evidence;
          }
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code = executed.diagnostic_id;
          step.diagnostic.detail = executed.detail;
          step.cancellation_observed = true;
          step.transient_state_cleanup_proven =
              executed.cleanup_complete && executed.cleanup_count == 1;
          step.cancellation_evidence_uuid =
              cancellation_policy == nullptr
                  ? std::string{}
                  : cancellation_policy->evidence_uuid;
          return step;
        }
        if (!executed.accepted || !executed.root_published ||
            !executed.cleanup_complete || executed.cleanup_count != 1 ||
            !executed.output.exact_exchange_validated) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              executed.diagnostic_id.empty()
                  ? "SB_MODEL_COORDINATOR_LEG_FAILED_V1"
                  : executed.diagnostic_id;
          step.diagnostic.detail = executed.detail;
          return step;
        }
        if (!captured->exact_output_columns.empty() &&
            (executed.output.batch.columns.size() !=
                 captured->exact_output_columns.size() ||
             !std::ranges::equal(
                 executed.output.batch.columns,
                 captured->exact_output_columns,
                 [](const auto& actual, const auto& expected) {
                   return Rcp079ExactExecutorColumnV1(actual, expected);
                 }))) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "SB_MODEL_RESULT_DESCRIPTOR_SOURCE_BINDING_INVALID_V1";
          step.diagnostic.detail =
              "captured model-family output descriptor changed";
          return step;
        }
        if (captured->columnar_runtime_memory_receipt != nullptr) {
          std::uint64_t current_memory_bytes = 0;
          const auto& memory = *captured->columnar_runtime_memory_receipt;
          if (!memory.complete ||
              memory.provider_logical_memory_bytes == 0 ||
              memory.memory_grant_bytes !=
                  selected_node.memory_bytes_required ||
              memory.peak_live_memory_bytes > memory.memory_grant_bytes ||
              !RuntimeMaterializedBatchMemoryBytes(executed.output.batch,
                                                   &current_memory_bytes) ||
              current_memory_bytes > memory.peak_live_memory_bytes) {
            step.diagnostic.ok = false;
            step.diagnostic.diagnostic_code =
                "SB_MODEL_RESOURCE_MEMORY_REFUSED_V1";
            step.diagnostic.detail =
                "captured columnar runtime memory receipt is incomplete";
            return step;
          }
          PublishRuntimeMemoryObservation(
              &step, current_memory_bytes, memory.peak_live_memory_bytes);
        }
        step.result_handle_id = selected_node.physical_node_id;
        step.output_row_count = executed.output.batch.rows.size();
        step.rows_examined = executed.rows_examined;
        step.current_relation_descriptor_uuid =
            captured->current_relation_descriptor_uuid;
        step.current_relation_descriptor_generation =
            captured->current_relation_descriptor_generation;
        step.materialized_output_batch = std::move(executed.output.batch);
        return step;
      };
  return registration;
}

exec::CanonicalPhysicalExecutorRegistration MakeRcp079AsofRegistration(
    std::string implementation_id, std::string capability_uuid,
    const exec::CanonicalTimeSeriesAsofInputBindingV1 left_binding,
    const exec::CanonicalTimeSeriesAsofInputBindingV1 right_binding,
    const std::int64_t tolerance_ns, const bool left_outer,
    const std::size_t maximum_output_rows,
    const std::uint64_t maximum_comparisons,
    std::string bound_transformation_receipt,
    api::EngineRequestContext execution_context,
    const api::EngineRequestContext* borrowed_mga_context = nullptr,
    const exec::CanonicalExecutionMgaAuthority* borrowed_mga_authority =
        nullptr) {
  const bool strict_dispatcher_memory =
      borrowed_mga_context != nullptr && borrowed_mga_authority != nullptr;
  exec::CanonicalPhysicalExecutorRegistration registration;
  registration.node_kind = exec::PhysicalNodeKind::kJoin;
  registration.implementation_id = std::move(implementation_id);
  registration.executor_capability_uuid = std::move(capability_uuid);
  registration.executor_capability_abi_version = 1;
  registration.engine_owned = true;
  registration.accepts_optimizer_publication_v2 = true;
  registration.honors_dispatcher_memory_limit_v1 =
      strict_dispatcher_memory;
  registration.retained_live_memory_bytes_v1 =
      strict_dispatcher_memory
          ? sizeof(exec::CanonicalTimeSeriesAsofInputBindingV1) * 2 +
                sizeof(api::EngineRequestContext) +
                bound_transformation_receipt.size() + 1 +
                8 * sizeof(void*) + 512
          : 0;
  registration.execute =
      [left_binding, right_binding, tolerance_ns, left_outer,
       maximum_output_rows, maximum_comparisons,
       bound_transformation_receipt =
           std::move(bound_transformation_receipt),
       execution_context = std::move(execution_context),
       borrowed_mga_authority, strict_dispatcher_memory](
          const exec::TypedPhysicalNodeDag& selected_dag,
          const exec::PhysicalNodeRecord& selected_node,
          const std::vector<exec::CanonicalPhysicalDispatchInput>& inputs) {
        exec::CanonicalPhysicalDispatchStepResult step;
        step.selected_plan_uuid = selected_dag.selected_plan_uuid;
        step.executed_physical_node_id = selected_node.physical_node_id;
        step.causal_counter_id = selected_node.causal_counter_id;
        step.output_descriptor_ids = selected_node.output_descriptor_ids;
        step.mga_statement_context = selected_dag.mga_statement_context;
        step.authority.engine_mga_snapshot_bound = true;
        step.data_access_observation_known = true;
        if (selected_dag.abi_version != 2 || inputs.size() != 2 ||
            selected_node.input_physical_node_ids.size() != 2 ||
            inputs[0].physical_node_id !=
                selected_node.input_physical_node_ids[0] ||
            inputs[1].physical_node_id !=
                selected_node.input_physical_node_ids[1] ||
            !inputs[0].materialized_output_batch.has_value() ||
            !inputs[1].materialized_output_batch.has_value()) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1";
          step.diagnostic.detail =
              "captured model-family ASOF inputs changed";
          return step;
        }
        const auto* cancellation_policy =
            FindLiveCancellationPolicy(selected_dag);
        if (cancellation_policy == nullptr) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "QOW-DIAG-RELATIONAL-LIVE-ASOF-CANCELLATION-POLICY-V1";
          step.diagnostic.detail =
              "captured model-family ASOF requires one exact cancellation "
              "policy evidence row";
          return step;
        }
        const auto cancellation_requested =
            execution_context.query_cancellation_requested
                ? execution_context.query_cancellation_requested
                : std::function<bool()>([] { return false; });
        auto cancellation_state = LiveCancellationProbeState::kRunning;
        const std::function<bool()> guarded_cancellation_requested =
            [&]() noexcept {
          return PollLiveCancellationProbe(cancellation_requested,
                                           &cancellation_state);
        };
        const auto cancelled = [&]() {
          return guarded_cancellation_requested();
        };
        const auto bind_cancellation = [&](const char* detail) {
          exec::DescriptorRuntimeDiagnostic diagnostic;
          diagnostic.ok = false;
          diagnostic.diagnostic_code =
              cancellation_state == LiveCancellationProbeState::kProbeFailed
                  ? "SB_MODEL_COORDINATOR_LEG_FAILED_V1"
                  : "SB_MODEL_EXECUTION_CANCELLED_V1";
          diagnostic.detail =
              cancellation_state == LiveCancellationProbeState::kProbeFailed
                  ? "captured model-family ASOF cancellation probe failed"
                  : detail;
          BindLiveCancellationFailure(std::move(diagnostic),
                                      cancellation_policy, &step);
        };
        if (cancelled()) {
          bind_cancellation(
              "captured model-family ASOF was cancelled before binding");
          return step;
        }
        if (strict_dispatcher_memory) {
          std::uint64_t auxiliary_memory_bytes =
              sizeof(exec::CanonicalTimeSeriesAsofJoinRequestV1);
          std::uint64_t component_bytes = 0;
          if (!CheckedAdd(auxiliary_memory_bytes,
                          bound_transformation_receipt.size() + 1,
                          &auxiliary_memory_bytes) ||
              !CheckedMultiply(
                  inputs[0].materialized_output_batch->rows.size(),
                  sizeof(exec::CanonicalTimeSeriesAsofKeyV1) +
                      sizeof(std::int64_t),
                  &component_bytes) ||
              !CheckedAdd(auxiliary_memory_bytes, component_bytes,
                          &auxiliary_memory_bytes) ||
              !CheckedMultiply(
                  inputs[1].materialized_output_batch->rows.size(),
                  sizeof(exec::CanonicalTimeSeriesAsofKeyV1) +
                      sizeof(std::string),
                  &component_bytes) ||
              !CheckedAdd(auxiliary_memory_bytes, component_bytes,
                          &auxiliary_memory_bytes)) {
            step.diagnostic.ok = false;
            step.diagnostic.diagnostic_code =
                "SBLR.PLAN_TREE.RESOURCE_LIMIT";
            step.diagnostic.detail =
                "captured model-family ASOF workspace bound overflowed";
            return step;
          }
          exec::TypedPhysicalNodeDag operator_dag;
          std::size_t callback_memory_bound = 0;
          std::string callback_memory_detail;
          if (!BuildStrictBinaryOperatorLocalPhysicalDag(
                  selected_dag, selected_node,
                  *inputs[0].materialized_output_batch,
                  *inputs[1].materialized_output_batch,
                  auxiliary_memory_bytes, &operator_dag,
                  &callback_memory_bound, &callback_memory_detail)) {
            step.diagnostic.ok = false;
            step.diagnostic.diagnostic_code =
                "SBLR.PLAN_TREE.RESOURCE_LIMIT";
            step.diagnostic.detail =
                "captured model-family ASOF " + callback_memory_detail;
            return step;
          }
        }
        exec::CanonicalTimeSeriesAsofJoinRequestV1 request;
        request.physical_dag = selected_dag;
        request.selected_physical_node_id = selected_node.physical_node_id;
        request.left_batch = *inputs[0].materialized_output_batch;
        request.right_batch = *inputs[1].materialized_output_batch;
        request.left_binding = left_binding;
        request.right_binding = right_binding;
        request.bound_transformation_receipt =
            bound_transformation_receipt;
        request.tolerance_ns = tolerance_ns;
        request.left_outer = left_outer;
        request.right_is_time_series_raw = right_binding.raw_time_series;
        request.maximum_output_rows = maximum_output_rows;
        request.maximum_comparisons = maximum_comparisons;
        request.maximum_memory_bytes = selected_dag.memory_budget_bytes;
        request.mga_authority =
            strict_dispatcher_memory
                ? *borrowed_mga_authority
                : BuildCanonicalExecutionMgaAuthority(
                      execution_context, selected_dag);
        request.cancellation_requested = cancellation_requested;
        const auto append_keys =
            [&](const exec::DescriptorBatch& batch,
                const exec::CanonicalTimeSeriesAsofInputBindingV1& binding,
                const bool right_input,
                std::vector<exec::CanonicalTimeSeriesAsofKeyV1>* keys) {
              if (keys == nullptr) return false;
              keys->reserve(batch.rows.size());
              for (const auto& row : batch.rows) {
                if (cancelled() ||
                    binding.metric_column_ordinal >= row.values.size() ||
                    binding.tags_column_ordinal >= row.values.size() ||
                    binding.timestamp_column_ordinal >= row.values.size() ||
                    (binding.raw_time_series &&
                     binding.row_uuid_column_ordinal >= row.values.size())) {
                  return false;
                }
                std::int64_t timestamp_ns = 0;
                if (!exec::ParseCanonicalTimeSeriesTimestampNsV1(
                        row.values[binding.timestamp_column_ordinal]
                            .encoded_value,
                        &timestamp_ns)) {
                  return false;
                }
                keys->push_back(
                    {row.values[binding.metric_column_ordinal].encoded_value,
                     row.values[binding.tags_column_ordinal].encoded_value,
                     timestamp_ns});
                if (right_input && binding.raw_time_series) {
                  request.right_tie_break_row_uuids.push_back(
                      row.values[binding.row_uuid_column_ordinal]
                          .encoded_value);
                }
              }
              return true;
            };
        if (!append_keys(request.left_batch, request.left_binding, false,
                         &request.left_keys) ||
            !append_keys(request.right_batch, request.right_binding, true,
                         &request.right_keys)) {
          if (cancellation_state != LiveCancellationProbeState::kRunning) {
            bind_cancellation(
                "captured model-family ASOF was cancelled during key "
                "construction");
          } else {
            step.diagnostic.ok = false;
            step.diagnostic.diagnostic_code =
                "SB_MODEL_TIME_SERIES_IDENTITY_INVALID_V1";
            step.diagnostic.detail =
                "captured model-family ASOF keys are not canonical";
          }
          return step;
        }
        auto executed = exec::ExecuteCanonicalTimeSeriesAsofJoinV1(request);
        if (!executed.diagnostic.ok) {
          BindLiveCancellationFailure(std::move(executed.diagnostic),
                                      cancellation_policy, &step);
          return step;
        }
        if (executed.selected_plan_uuid != selected_dag.selected_plan_uuid ||
            executed.executed_physical_node_id !=
                selected_node.physical_node_id ||
            executed.causal_counter_id != selected_node.causal_counter_id ||
            !exec::PhysicalMgaStatementContextEqual(
                executed.mga_statement_context,
                selected_dag.mga_statement_context)) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1";
          step.diagnostic.detail =
              "captured model-family ASOF execution receipt changed";
          return step;
        }
        step.result_handle_id = selected_node.physical_node_id;
        step.input_row_count = request.left_batch.rows.size() +
                               request.right_batch.rows.size();
        step.rows_examined = request.left_keys.size() *
                             request.right_keys.size();
        step.output_row_count = executed.output_batch.rows.size();
        step.materialized_output_batch = std::move(executed.output_batch);
        return step;
      };
  return registration;
}







}  // namespace

std::string Rcp079CanonicalReal64(const double value) {
  if (!std::isfinite(value)) return {};
  if (value == 0.0) return "0";
  std::array<char, 64> buffer{};
  const auto converted = std::to_chars(
      buffer.data(), buffer.data() + buffer.size(), value,
      std::chars_format::general, std::numeric_limits<double>::max_digits10);
  return converted.ec == std::errc{}
             ? std::string(buffer.data(), converted.ptr)
             : std::string{};
}

plan::CanonicalMgaStatementContext Rcp079LogicalMga(
    const exec::PhysicalMgaStatementContext& mga, const bool current) {
  plan::CanonicalMgaStatementContext logical;
  logical.statement_uuid = mga.statement_uuid;
  logical.statement_timestamp = mga.statement_timestamp;
  logical.owning_transaction_uuid = mga.owning_transaction_uuid;
  logical.statement_snapshot_uuid = mga.statement_snapshot_uuid;
  logical.statement_metadata_snapshot_uuid =
      mga.statement_metadata_snapshot_uuid;
  logical.owning_local_transaction_id = mga.owning_local_transaction_id;
  logical.visible_committed_high_watermark =
      mga.visible_committed_high_watermark;
  logical.oldest_active_transaction_id = mga.oldest_active_transaction_id;
  logical.oldest_interesting_transaction_id =
      mga.oldest_interesting_transaction_id;
  logical.oldest_snapshot_transaction_id =
      mga.oldest_snapshot_transaction_id;
  logical.retention_horizon_transaction_id =
      mga.retention_horizon_transaction_id;
  logical.active_excluded_local_transaction_ids =
      mga.active_excluded_local_transaction_ids;
  logical.in_doubt_excluded_local_transaction_ids =
      mga.in_doubt_excluded_local_transaction_ids;
  logical.snapshot_kind = mga.snapshot_kind;
  logical.publication_inventory_next_local_transaction_id =
      mga.publication_inventory_next_local_transaction_id;
  logical.inventory_authoritative = mga.inventory_authoritative;
  logical.complete = mga.complete;
  logical.current = current;
  return logical;
}


namespace {

api::TypedRelationalDag Rcp079OperatorLocalModelSourceDag(
    const api::TypedRelationalDag& dag,
    const api::RelationalDagNode& source) {
  auto local = dag;
  local.root_node_id = source.node_id;
  std::erase_if(local.nodes,
                [&](const auto& node) { return node.node_id != source.node_id; });
  std::erase_if(local.outputs, [&](const auto& output) {
    return output.relation_node_id != source.node_id;
  });
  std::unordered_set<std::uint32_t> expression_ids;
  std::vector<std::uint32_t> pending = source.bound_expression_ids;
  while (!pending.empty()) {
    const auto expression_id = pending.back();
    pending.pop_back();
    if (!expression_ids.insert(expression_id).second) continue;
    const auto expression = std::ranges::find_if(
        dag.expressions, [&](const auto& candidate) {
          return candidate.expression_id == expression_id;
        });
    if (expression == dag.expressions.end()) continue;
    pending.insert(pending.end(), expression->child_expression_ids.begin(),
                   expression->child_expression_ids.end());
  }
  std::erase_if(local.expressions, [&](const auto& expression) {
    return !expression_ids.contains(expression.expression_id);
  });
  std::unordered_set<std::uint32_t> descriptor_ids(
      source.output_descriptor_ids.begin(), source.output_descriptor_ids.end());
  for (const auto& expression : local.expressions) {
    descriptor_ids.insert(expression.result_descriptor_id);
  }
  std::erase_if(local.descriptors, [&](const auto& descriptor) {
    return !descriptor_ids.contains(descriptor.descriptor_id);
  });
  const std::unordered_set<std::uint32_t> value_row_ids(
      source.values_row_ids.begin(), source.values_row_ids.end());
  std::erase_if(local.values_rows, [&](const auto& row) {
    return !value_row_ids.contains(row.row_id);
  });
  local.grouping_sets.clear();
  return local;
}

std::string Rcp079ModelFamilyForSource(
    const api::TypedRelationalDag& dag,
    const api::RelationalDagNode& source) {
  if (source.node_kind == api::RelationalDagNodeKind::kScan &&
      source.semantic_variant_id == "relation.source.v1") {
    return "relational";
  }
  const auto has = [&](const std::string_view name) {
    for (const auto expression_id : source.bound_expression_ids) {
      const auto expression = std::ranges::find_if(
          dag.expressions, [&](const auto& candidate) {
            return candidate.expression_id == expression_id;
          });
      if (expression != dag.expressions.end() &&
          expression->operator_name == name) {
        return true;
      }
    }
    return false;
  };
  if (has("SPATIAL_SOURCE")) return "spatial";
  if (has("COLUMNAR_SOURCE")) return "columnar";
  if (has("SEARCH_MATCH") || has("SEARCH_TERMS") || has("SEARCH_PHRASE") ||
      has("SEARCH_FUZZY")) {
    return "search";
  }
  if (has("VECTOR_NEAREST")) return "vector";
  if (has("TIME_RANGE")) return "time_series";
  if (has("KV_KEY") || has("KV_MULTI_GET") || has("KV_PREFIX")) {
    return "key_value";
  }
  if (has("GRAPH_MATCH") || has("GRAPH_EXPAND")) return "graph";
  return "document";
}

struct Rcp079MultilegDescriptorPreflightV1 {
  bool accepted{false};
  std::vector<opt::MultilegDescriptorAllocationV1> source_allocations;
  std::vector<opt::MultilegDescriptorAllocationV1> publication_allocations;
  std::string diagnostic_id;
  std::string detail;
};

Rcp079MultilegDescriptorPreflightV1
Rcp079PreflightMultilegResultDescriptorsV1(
    const CanonicalCurrentHeapExecutionRequest& input,
    const std::vector<const api::RelationalDagNode*>& sources,
    const exec::CanonicalAcceptedJoinKind join_kind,
    const bool left_only) {
  Rcp079MultilegDescriptorPreflightV1 result;
  const auto refuse = [&](std::string diagnostic_id, std::string detail) {
    result.source_allocations.clear();
    result.publication_allocations.clear();
    result.diagnostic_id = std::move(diagnostic_id);
    result.detail = std::move(detail);
    return result;
  };
  const auto descriptor_for = [&](const std::uint32_t descriptor_id) {
    return std::ranges::find_if(
        input.relational_dag.descriptors, [&](const auto& descriptor) {
          return descriptor.descriptor_id == descriptor_id;
        });
  };
  const auto expression_for = [&](const std::uint32_t expression_id) {
    return std::ranges::find_if(
        input.relational_dag.expressions, [&](const auto& expression) {
          return expression.expression_id == expression_id;
        });
  };
  const auto derived_type = [](const std::string_view family,
                               const std::string_view name)
      -> std::optional<std::string_view> {
    if (family == "search") {
      if (name == "document_uuid" || name == "analyzer_uuid") return "uuid";
      if (name == "analyzer_generation" || name == "rank") return "uint64";
      if (name == "score") return "real64";
      return std::nullopt;
    }
    if (family == "vector") {
      if (name == "row_uuid") return "uuid";
      if (name == "distance" || name == "score") return "real64";
      return std::nullopt;
    }
    if (family == "spatial") {
      if (name == "row_uuid" || name == "crs_uuid") return "uuid";
      if (name == "spatial_value") return "geometry";
      if (name == "predicate_truth") return "boolean";
      if (name == "distance") return "real64";
      return std::nullopt;
    }
    return std::nullopt;
  };
  const auto exact_derived_inventory = [](const std::string_view family,
                                          const std::vector<std::string>& names) {
    if (family == "search") {
      return names == std::vector<std::string>{
                          "document_uuid", "analyzer_uuid",
                          "analyzer_generation", "score", "rank"};
    }
    if (family == "vector") {
      return names ==
             std::vector<std::string>{"row_uuid", "distance", "score"};
    }
    if (family == "spatial") {
      return names == std::vector<std::string>{
                          "row_uuid", "spatial_value", "crs_uuid"} ||
             names == std::vector<std::string>{
                          "row_uuid", "spatial_value", "crs_uuid",
                          "predicate_truth"} ||
             names == std::vector<std::string>{
                          "row_uuid", "spatial_value", "crs_uuid",
                          "distance"} ||
             names == std::vector<std::string>{
                          "row_uuid", "spatial_value", "crs_uuid",
                          "predicate_truth", "distance"};
    }
    return false;
  };

  std::vector<opt::MultilegDescriptorDemandV1> source_demands;
  for (std::size_t source_ordinal = 0; source_ordinal < sources.size();
       ++source_ordinal) {
    const auto* source = sources[source_ordinal];
    const auto family =
        Rcp079ModelFamilyForSource(input.relational_dag, *source);
    const bool derived_family =
        family == "search" || family == "vector" || family == "spatial";
    std::vector<const api::RelationalOutputRecord*> outputs;
    for (const auto& output : input.relational_dag.outputs) {
      if (output.relation_node_id == source->node_id) outputs.push_back(&output);
    }
    std::ranges::sort(outputs, {}, &api::RelationalOutputRecord::ordinal);
    std::vector<std::string> names;
    names.reserve(outputs.size());
    for (const auto* output : outputs) names.push_back(output->output_name_utf8);
    if (outputs.size() != source->output_descriptor_ids.size() ||
        outputs.empty() ||
        (derived_family && !exact_derived_inventory(family, names))) {
      return refuse("SB_MODEL_RESULT_DESCRIPTOR_DEMAND_INVALID_V1",
                    "model source public field inventory is not exact");
    }
    std::unordered_set<std::uint32_t> output_descriptor_ids;
    std::unordered_set<std::string> output_names;
    for (std::size_t field_ordinal = 0; field_ordinal < outputs.size();
         ++field_ordinal) {
      const auto* output = outputs[field_ordinal];
      const auto descriptor = descriptor_for(output->descriptor_id);
      const auto expression = expression_for(output->expression_id);
      if (!output->visible || output->ordinal != field_ordinal ||
          output->descriptor_id != source->output_descriptor_ids[field_ordinal] ||
          descriptor == input.relational_dag.descriptors.end() ||
          expression == input.relational_dag.expressions.end() ||
          expression->result_descriptor_id != output->descriptor_id ||
          std::ranges::find(source->bound_expression_ids,
                            output->expression_id) ==
              source->bound_expression_ids.end() ||
          descriptor->nullability == api::RelationalNullability::kUnknown ||
          !output_descriptor_ids.insert(output->descriptor_id).second ||
          output->output_name_utf8.empty() ||
          !output_names.insert(output->output_name_utf8).second) {
        return refuse("SB_MODEL_RESULT_DESCRIPTOR_DEMAND_INVALID_V1",
                      "model source output binding is missing or duplicated");
      }
      opt::MultilegDescriptorDemandV1 demand;
      demand.lexical_source_ordinal =
          static_cast<std::uint16_t>(source_ordinal);
      demand.field_ordinal = static_cast<std::uint16_t>(field_ordinal);
      demand.family_id = family;
      demand.field_id = output->output_name_utf8;
      if (derived_family) {
        const auto type = derived_type(family, output->output_name_utf8);
        if (!type.has_value() ||
            descriptor->nullability != api::RelationalNullability::kNonNull ||
            descriptor->collation_uuid.has_value() ||
            descriptor->timezone_profile_id.has_value()) {
          return refuse("SB_MODEL_RESULT_DESCRIPTOR_DEMAND_INVALID_V1",
                        "derived model source field type or nullability drifted");
        }
        demand.canonical_type_name = std::string(*type);
        demand.nullable = false;
        demand.derived = true;
      } else {
        demand.canonical_type_name = "persisted";
        demand.nullable =
            descriptor->nullability == api::RelationalNullability::kNullable;
        demand.derived = false;
        demand.persisted_descriptor_uuid = descriptor->descriptor_uuid;
        demand.persisted_type_uuid = descriptor->type_uuid;
      }
      source_demands.push_back(std::move(demand));
    }
  }
  const bool requires_derived_descriptor_pool =
      std::ranges::any_of(source_demands, [](const auto& demand) {
        return demand.derived || demand.family_id == "columnar";
      });
  opt::MultilegDescriptorDispatchLookupV1 scoped;
  opt::MultilegDescriptorAllocationResultV1 source_allocation;
  if (requires_derived_descriptor_pool) {
    scoped = opt::LookupMultilegDescriptorDispatchScopeV1(
        input.context.statement_uuid.canonical);
    if (!scoped.accepted) {
      return refuse(scoped.diagnostic_id, scoped.detail);
    }
    source_allocation = opt::AllocateMultilegResultDescriptorsV1(
        scoped.profiles, source_demands);
  } else {
    // Persisted-only joins do not consume the statement V10 derived-result
    // pools. Their catalog descriptor and type UUIDs are already the exact
    // allocation authority, including when an outer carrier is nullable.
    source_allocation.accepted = true;
    source_allocation.preflight_complete = true;
    source_allocation.diagnostic_id = "SB_EXECUTOR_OK";
    source_allocation.allocations.reserve(source_demands.size());
    for (const auto& demand : source_demands) {
      opt::MultilegDescriptorAllocationV1 allocation;
      allocation.demand = demand;
      allocation.descriptor_uuid = demand.persisted_descriptor_uuid;
      allocation.type_uuid = demand.persisted_type_uuid;
      source_allocation.allocations.push_back(std::move(allocation));
    }
  }
  if (!source_allocation.accepted || !source_allocation.preflight_complete ||
      source_allocation.allocations.size() != source_demands.size()) {
    return refuse(source_allocation.diagnostic_id, source_allocation.detail);
  }
  std::size_t allocation_ordinal = 0;
  for (const auto* source : sources) {
    for (const auto descriptor_id : source->output_descriptor_ids) {
      const auto descriptor = descriptor_for(descriptor_id);
      const auto& allocation = source_allocation.allocations[allocation_ordinal++];
      if (descriptor == input.relational_dag.descriptors.end() ||
          descriptor->descriptor_uuid != allocation.descriptor_uuid ||
          descriptor->type_uuid != allocation.type_uuid) {
        return refuse("SB_MODEL_RESULT_DESCRIPTOR_SOURCE_BINDING_INVALID_V1",
                      "source DAG descriptor is not its exact V10 allocation");
      }
    }
  }

  auto publication_allocations = source_allocation.allocations;
  if (left_only) {
    publication_allocations.erase(
        std::remove_if(publication_allocations.begin(),
                       publication_allocations.end(), [](const auto& allocation) {
                         return allocation.demand.lexical_source_ordinal != 0;
                       }),
        publication_allocations.end());
  } else {
    std::vector<opt::MultilegDescriptorDemandV1> nullable_derived_demands;
    std::vector<std::size_t> nullable_derived_ordinals;
    for (std::size_t ordinal = 0; ordinal < publication_allocations.size();
         ++ordinal) {
      auto& allocation = publication_allocations[ordinal];
      const bool left_side = allocation.demand.lexical_source_ordinal == 0;
      const bool null_extended =
          (left_side &&
           (join_kind == exec::CanonicalAcceptedJoinKind::kRightOuter ||
            join_kind == exec::CanonicalAcceptedJoinKind::kFullOuter)) ||
          (!left_side &&
           (join_kind == exec::CanonicalAcceptedJoinKind::kLeftOuter ||
            join_kind == exec::CanonicalAcceptedJoinKind::kFullOuter));
      if (!null_extended) continue;

      // Persisted fields retain their catalog UUID/type even when the outer
      // result carrier becomes nullable. Derived fields instead consume the
      // exact nullable V10 pools in lexical source/field order. Keeping every
      // preserved source allocation untouched prevents the opposite join leg
      // from renumbering its already-bound non-null descriptor identities.
      allocation.demand.nullable = true;
      if (allocation.demand.derived) {
        nullable_derived_demands.push_back(allocation.demand);
        nullable_derived_ordinals.push_back(ordinal);
      }
    }
    if (!nullable_derived_demands.empty()) {
      const auto nullable_allocation =
          opt::AllocateMultilegResultDescriptorsV1(scoped.profiles,
                                                   nullable_derived_demands);
      if (!nullable_allocation.accepted ||
          !nullable_allocation.preflight_complete ||
          nullable_allocation.allocations.size() !=
              nullable_derived_demands.size()) {
        return refuse(nullable_allocation.diagnostic_id,
                      nullable_allocation.detail);
      }
      for (std::size_t ordinal = 0;
           ordinal < nullable_derived_ordinals.size(); ++ordinal) {
        publication_allocations[nullable_derived_ordinals[ordinal]] =
            nullable_allocation.allocations[ordinal];
      }
    }
  }
  result.accepted = true;
  result.source_allocations = source_allocation.allocations;
  result.publication_allocations = std::move(publication_allocations);
  result.diagnostic_id = "SB_EXECUTOR_OK";
  return result;
}

bool CaptureRcp079ModelSourceLeg(
    const CanonicalCurrentHeapExecutionRequest& input,
    const api::RelationalDagNode& source,
    Rcp079CapturedModelLegV1* capture,
  CanonicalObjectFreeValuesExecutionResult* attempted) {
  if (capture == nullptr || attempted == nullptr) return false;
  CanonicalCurrentHeapExecutionRequest local = input;
  const auto family = Rcp079ModelFamilyForSource(input.relational_dag, source);
  local.relational_dag =
      Rcp079OperatorLocalModelSourceDag(input.relational_dag, source);
  if (family == "time_series") {
    // QOW-SOURCE-RCP080-OPERATOR-LOCAL-TIME-SERIES-ATTACHMENT-V1
    // Full multileg admission owns alias/start/end/range at the source.  The
    // standalone raw family route owns only the public output prefix and the
    // range root; its three operands remain reachable children.  Derive that
    // narrower top-level view only from the exact already-admitted six-field
    // carrier.  A near shape is left unchanged for ordinary validation to
    // refuse, and the original complete DAG is never mutated.
    const auto complete = api::ValidateTypedRelationalDag(input.relational_dag);
    auto local_source = std::ranges::find_if(
        local.relational_dag.nodes, [&](const auto& candidate) {
          return candidate.node_id == source.node_id;
        });
    std::vector<const api::RelationalOutputRecord*> outputs;
    for (const auto& output : input.relational_dag.outputs) {
      if (output.relation_node_id == source.node_id) outputs.push_back(&output);
    }
    std::ranges::sort(outputs, {},
                      &api::RelationalOutputRecord::ordinal);
    static constexpr std::array<std::string_view, 6> kExactNames{
        "row_uuid", "series_uuid", "metric_uuid", "point_timestamp",
        "tags", "value"};
    bool exact = complete.accepted && complete.issues.empty() &&
                 local_source != local.relational_dag.nodes.end() &&
                 source.output_descriptor_ids.size() == kExactNames.size() &&
                 outputs.size() == kExactNames.size() &&
                 source.bound_expression_ids.size() ==
                     kExactNames.size() + 4 &&
                 source.required_object_uuids.size() == 1;
    std::vector<std::uint32_t> standalone_bound_expression_ids;
    standalone_bound_expression_ids.reserve(kExactNames.size() + 1);
    std::unordered_set<std::uint32_t> output_expression_ids;
    for (std::size_t ordinal = 0; exact && ordinal < outputs.size();
         ++ordinal) {
      const auto* output = outputs[ordinal];
      exact = output->visible && output->ordinal == ordinal &&
              output->output_name_utf8 == kExactNames[ordinal] &&
              output->descriptor_id == source.output_descriptor_ids[ordinal] &&
              source.bound_expression_ids[ordinal] == output->expression_id &&
              output_expression_ids.insert(output->expression_id).second;
      standalone_bound_expression_ids.push_back(output->expression_id);
    }
    const auto expression_for = [&](const std::uint32_t expression_id) {
      return std::ranges::find_if(
          input.relational_dag.expressions, [&](const auto& expression) {
            return expression.expression_id == expression_id;
          });
    };
    if (exact) {
      const auto suffix = source.bound_expression_ids.begin() +
                          static_cast<std::ptrdiff_t>(kExactNames.size());
      const auto alias = expression_for(suffix[0]);
      const auto start = expression_for(suffix[1]);
      const auto end = expression_for(suffix[2]);
      const auto range = expression_for(suffix[3]);
      exact = alias != input.relational_dag.expressions.end() &&
              start != input.relational_dag.expressions.end() &&
              end != input.relational_dag.expressions.end() &&
              range != input.relational_dag.expressions.end() &&
              range->operator_name == "TIME_RANGE" &&
              range->expression_kind ==
                  api::RelationalExpressionKind::kFunctionCall &&
              range->child_expression_ids ==
                  std::vector<std::uint32_t>{alias->expression_id,
                                             start->expression_id,
                                             end->expression_id} &&
              alias->expression_kind ==
                  api::RelationalExpressionKind::kIdentifier &&
              alias->bound_name_uuid == source.required_object_uuids.front() &&
              start->expression_kind ==
                  api::RelationalExpressionKind::kLiteral &&
              end->expression_kind == api::RelationalExpressionKind::kLiteral &&
              start->literal_kind == api::RelationalLiteralKind::kTemporal &&
              end->literal_kind == api::RelationalLiteralKind::kTemporal &&
              start->result_descriptor_id == end->result_descriptor_id &&
              !output_expression_ids.contains(alias->expression_id) &&
              !output_expression_ids.contains(start->expression_id) &&
              !output_expression_ids.contains(end->expression_id) &&
              !output_expression_ids.contains(range->expression_id) &&
              std::unordered_set<std::uint32_t>{alias->expression_id,
                                                start->expression_id,
                                                end->expression_id,
                                                range->expression_id}
                      .size() == 4;
      if (exact) standalone_bound_expression_ids.push_back(range->expression_id);
    }
    if (exact) {
      local_source->bound_expression_ids =
          std::move(standalone_bound_expression_ids);
    }
  }
  if (family == "search") {
    // QOW-SOURCE-RCP080-OPERATOR-LOCAL-SEARCH-AUTHORITY-ENRICHMENT-V1
    // The exact full multileg carrier deliberately owns only alias, query
    // text/query root, analyzer identity, top-k, and SEARCH_MATCH.  Standalone
    // execution additionally requires the current analyzer generation,
    // canonical analyzer digest, and current persisted category identity.
    // Build those records only in a candidate local view, from the same
    // immutable request/catalog authority.  The original DAG is untouched;
    // any mismatch leaves the unenriched near shape for normal refusal.
    const auto complete = api::ValidateTypedRelationalDag(input.relational_dag);
    auto enriched = local.relational_dag;
    auto local_source = std::ranges::find_if(
        enriched.nodes, [&](const auto& candidate) {
          return candidate.node_id == source.node_id;
        });
    std::vector<const api::RelationalOutputRecord*> outputs;
    for (const auto& output : input.relational_dag.outputs) {
      if (output.relation_node_id == source.node_id) outputs.push_back(&output);
    }
    std::ranges::sort(outputs, {}, &api::RelationalOutputRecord::ordinal);
    static constexpr std::array<std::string_view, 5> kExactNames{
        "document_uuid", "analyzer_uuid", "analyzer_generation", "score",
        "rank"};
    bool exact = complete.accepted && complete.issues.empty() &&
                 local_source != enriched.nodes.end() &&
                 source.node_kind == api::RelationalDagNodeKind::kScan &&
                 source.semantic_variant_id == "SBLR_MODEL_SOURCE_V1" &&
                 source.required_object_uuids.size() == 1 &&
                 source.output_descriptor_ids.size() == kExactNames.size() &&
                 source.bound_expression_ids.size() ==
                     kExactNames.size() + 6 &&
                 outputs.size() == kExactNames.size() &&
                 input.context.catalog_generation_id != 0;
    const auto expression_for = [&](const std::uint32_t expression_id) {
      return std::ranges::find_if(
          input.relational_dag.expressions, [&](const auto& expression) {
            return expression.expression_id == expression_id;
          });
    };
    const auto descriptor_for = [&](const std::uint32_t descriptor_id) {
      return std::ranges::find_if(
          input.relational_dag.descriptors, [&](const auto& descriptor) {
            return descriptor.descriptor_id == descriptor_id;
          });
    };
    std::vector<std::uint32_t> output_expression_ids;
    std::unordered_set<std::uint32_t> distinct_output_expression_ids;
    for (std::size_t ordinal = 0; exact && ordinal < outputs.size();
         ++ordinal) {
      const auto* output = outputs[ordinal];
      const auto expression = expression_for(output->expression_id);
      const auto descriptor = descriptor_for(output->descriptor_id);
      exact = output->visible && output->ordinal == ordinal &&
              output->output_name_utf8 == kExactNames[ordinal] &&
              output->descriptor_id == source.output_descriptor_ids[ordinal] &&
              source.bound_expression_ids[ordinal] == output->expression_id &&
              expression != input.relational_dag.expressions.end() &&
              descriptor != input.relational_dag.descriptors.end() &&
              expression->result_descriptor_id == output->descriptor_id &&
              descriptor->nullability ==
                  api::RelationalNullability::kNonNull &&
              !descriptor->collation_uuid.has_value() &&
              !descriptor->timezone_profile_id.has_value() &&
              distinct_output_expression_ids.insert(output->expression_id)
                  .second;
      output_expression_ids.push_back(output->expression_id);
    }
    if (exact) {
      const auto document_descriptor = descriptor_for(outputs[0]->descriptor_id);
      const auto analyzer_descriptor = descriptor_for(outputs[1]->descriptor_id);
      const auto generation_descriptor =
          descriptor_for(outputs[2]->descriptor_id);
      const auto score_descriptor = descriptor_for(outputs[3]->descriptor_id);
      const auto rank_descriptor = descriptor_for(outputs[4]->descriptor_id);
      exact = document_descriptor->type_uuid == analyzer_descriptor->type_uuid &&
              generation_descriptor->type_uuid == rank_descriptor->type_uuid &&
              generation_descriptor->type_uuid != score_descriptor->type_uuid;
    }
    const auto match = std::ranges::find_if(
        input.relational_dag.expressions, [](const auto& expression) {
          return expression.operator_name == "SEARCH_MATCH";
        });
    const auto terms = std::ranges::find_if(
        input.relational_dag.expressions, [](const auto& expression) {
          return expression.operator_name == "SEARCH_TERMS";
        });
    exact = exact && match != input.relational_dag.expressions.end() &&
            terms != input.relational_dag.expressions.end() &&
            std::ranges::count_if(
                input.relational_dag.expressions, [](const auto& expression) {
                  return expression.operator_name == "SEARCH_MATCH";
                }) == 1 &&
            std::ranges::count_if(
                input.relational_dag.expressions, [](const auto& expression) {
                  return expression.operator_name == "SEARCH_TERMS";
                }) == 1 &&
            match->expression_kind ==
                api::RelationalExpressionKind::kFunctionCall &&
            match->child_expression_ids.size() == 4 &&
            terms->expression_kind ==
                api::RelationalExpressionKind::kFunctionCall &&
            terms->child_expression_ids.size() == 1 &&
            match->child_expression_ids[1] == terms->expression_id;
    auto alias = input.relational_dag.expressions.end();
    auto query_text = input.relational_dag.expressions.end();
    auto owned_analyzer = input.relational_dag.expressions.end();
    auto owned_top_k = input.relational_dag.expressions.end();
    std::uint32_t top_k_value = 0;
    if (exact) {
      alias = expression_for(match->child_expression_ids[0]);
      query_text = expression_for(terms->child_expression_ids[0]);
      owned_analyzer = expression_for(match->child_expression_ids[2]);
      owned_top_k = expression_for(match->child_expression_ids[3]);
      exact = alias != input.relational_dag.expressions.end() &&
              query_text != input.relational_dag.expressions.end() &&
              owned_analyzer != input.relational_dag.expressions.end() &&
              owned_top_k != input.relational_dag.expressions.end() &&
              alias->expression_kind ==
                  api::RelationalExpressionKind::kIdentifier &&
              alias->bound_name_uuid == source.required_object_uuids.front() &&
              query_text->expression_kind ==
                  api::RelationalExpressionKind::kLiteral &&
              query_text->literal_kind ==
                  api::RelationalLiteralKind::kString &&
              query_text->literal_or_parameter_ref.has_value() &&
              !query_text->literal_or_parameter_ref->empty() &&
              owned_analyzer->expression_kind ==
                  api::RelationalExpressionKind::kIdentifier &&
              owned_analyzer->bound_name_uuid.has_value() &&
              owned_analyzer->bound_name_uuid !=
                  source.required_object_uuids.front() &&
              owned_top_k->expression_kind ==
                  api::RelationalExpressionKind::kLiteral &&
              owned_top_k->literal_kind ==
                  api::RelationalLiteralKind::kNumeric &&
              owned_top_k->literal_or_parameter_ref.has_value();
    }
    if (exact) {
      const auto& spelling = *owned_top_k->literal_or_parameter_ref;
      const auto parsed = std::from_chars(
          spelling.data(), spelling.data() + spelling.size(), top_k_value);
      exact = !spelling.empty() &&
              (spelling.size() == 1 || spelling.front() != '0') &&
              parsed.ec == std::errc{} &&
              parsed.ptr == spelling.data() + spelling.size() &&
              top_k_value != 0;
    }
    if (exact) {
      const auto analyzer_output = expression_for(output_expression_ids[1]);
      const auto generation_output = expression_for(output_expression_ids[2]);
      const auto rank_output = expression_for(output_expression_ids[4]);
      exact = analyzer_output != input.relational_dag.expressions.end() &&
              generation_output != input.relational_dag.expressions.end() &&
              rank_output != input.relational_dag.expressions.end() &&
              analyzer_output->expression_kind ==
                  api::RelationalExpressionKind::kIdentifier &&
              analyzer_output->bound_name_uuid ==
                  owned_analyzer->bound_name_uuid &&
              generation_output->expression_kind ==
                  api::RelationalExpressionKind::kIdentifier &&
              generation_output->bound_name_uuid ==
                  owned_analyzer->bound_name_uuid;
    }
    if (exact) {
      const auto suffix_begin = source.bound_expression_ids.begin() +
                                static_cast<std::ptrdiff_t>(kExactNames.size());
      const std::unordered_set<std::uint32_t> suffix(
          suffix_begin, source.bound_expression_ids.end());
      const std::unordered_set<std::uint32_t> expected{
          alias->expression_id, query_text->expression_id,
          terms->expression_id, owned_analyzer->expression_id,
          owned_top_k->expression_id, match->expression_id};
      exact = suffix.size() == 6 && suffix == expected &&
              std::ranges::all_of(expected, [&](const auto expression_id) {
                return std::ranges::count_if(
                           input.relational_dag.nodes,
                           [&](const auto& candidate) {
                             return std::ranges::find(
                                        candidate.bound_expression_ids,
                                        expression_id) !=
                                    candidate.bound_expression_ids.end();
                           }) == 1;
              });
    }
    api::MgaRelationStorageDescriptorLoadResult loaded;
    if (exact) {
      loaded = api::LoadMgaRelationStorageDescriptor(
          input.context, source.required_object_uuids.front());
      exact = loaded.ok &&
              loaded.descriptor.relation_uuid.canonical ==
                  source.required_object_uuids.front() &&
              loaded.descriptor.database_uuid.canonical ==
                  input.context.database_uuid.canonical &&
              loaded.descriptor.relation_kind == "table" &&
              loaded.descriptor.storage_profile == "local_mga_rowstore_v1" &&
              loaded.descriptor.descriptor_generation != 0 &&
              loaded.descriptor.columns.size() == 2 &&
              loaded.descriptor.columns[0].ordinal == 0 &&
              loaded.descriptor.columns[0].canonical_name_key == "body" &&
              !loaded.descriptor.columns[0].nullable &&
              loaded.descriptor.columns[0]
                      .value_descriptor.canonical_type_name == "text" &&
              loaded.descriptor.columns[1].ordinal == 1 &&
              loaded.descriptor.columns[1].canonical_name_key == "category" &&
              !loaded.descriptor.columns[1].nullable &&
              loaded.descriptor.columns[1].collation_uuid.empty() &&
              !Rcp079DescriptorField(
                   loaded.descriptor.columns[1]
                       .value_descriptor.encoded_descriptor,
                   "timezone_profile_id")
                   .has_value() &&
              loaded.descriptor.columns[1]
                      .value_descriptor.canonical_type_name == "text";
    }
    auto body_descriptor = input.relational_dag.descriptors.end();
    auto category_descriptor = input.relational_dag.descriptors.end();
    std::uint32_t category_descriptor_id = 0;
    std::optional<std::string> category_type_uuid;
    std::optional<api::RelationalTypeDescriptor> local_category_descriptor;
    if (exact) {
      const auto descriptor_matches = [&](const auto& descriptor,
                                          const auto& engine_descriptor) {
        const auto type_uuid = Rcp079DescriptorField(
            engine_descriptor.encoded_descriptor, "type_uuid");
        return type_uuid.has_value() && CanonicalUuidText(*type_uuid) &&
               CanonicalUuidText(engine_descriptor.descriptor_uuid.canonical) &&
               descriptor.descriptor_uuid ==
                   engine_descriptor.descriptor_uuid.canonical &&
               descriptor.type_uuid == *type_uuid &&
               descriptor.nullability ==
                   api::RelationalNullability::kNonNull &&
               !descriptor.collation_uuid.has_value() &&
               !descriptor.timezone_profile_id.has_value();
      };
      body_descriptor = std::ranges::find_if(
          input.relational_dag.descriptors, [&](const auto& descriptor) {
            return descriptor_matches(
                descriptor,
                loaded.descriptor.columns[0].value_descriptor);
          });
      category_descriptor = std::ranges::find_if(
          input.relational_dag.descriptors, [&](const auto& descriptor) {
            return descriptor_matches(
                descriptor,
                loaded.descriptor.columns[1].value_descriptor);
          });
      category_type_uuid = Rcp079DescriptorField(
          loaded.descriptor.columns[1].value_descriptor.encoded_descriptor,
          "type_uuid");
      exact = body_descriptor != input.relational_dag.descriptors.end() &&
              body_descriptor->descriptor_id ==
                  query_text->result_descriptor_id &&
              CanonicalUuidText(
                  loaded.descriptor.columns[1].column_uuid.canonical) &&
              CanonicalUuidText(loaded.descriptor.columns[1]
                                    .value_descriptor.descriptor_uuid.canonical) &&
              category_type_uuid.has_value() &&
              CanonicalUuidText(*category_type_uuid);
      if (exact &&
          category_descriptor != input.relational_dag.descriptors.end()) {
        category_descriptor_id = category_descriptor->descriptor_id;
      } else if (exact) {
        // QOW-SOURCE-RCP080-OPERATOR-LOCAL-SEARCH-CATEGORY-DESCRIPTOR-V1
        // Only the local numeric handle is new.  Descriptor/type identities
        // and all semantics come from the same current relation metadata that
        // authorized the search enrichment.  max+1 is deterministic and
        // collision-free; the uint32 boundary fails closed.
        std::uint32_t maximum_descriptor_id = 0;
        for (const auto& descriptor : input.relational_dag.descriptors) {
          maximum_descriptor_id =
              std::max(maximum_descriptor_id, descriptor.descriptor_id);
        }
        exact = maximum_descriptor_id !=
                std::numeric_limits<std::uint32_t>::max();
        if (exact) {
          category_descriptor_id = maximum_descriptor_id + 1;
          exact = category_descriptor_id != 0 &&
                  std::ranges::none_of(
                      input.relational_dag.descriptors,
                      [&](const auto& descriptor) {
                        return descriptor.descriptor_id ==
                               category_descriptor_id;
                      });
        }
        if (exact) {
          api::RelationalTypeDescriptor descriptor;
          descriptor.descriptor_id = category_descriptor_id;
          descriptor.descriptor_uuid = loaded.descriptor.columns[1]
                                           .value_descriptor.descriptor_uuid
                                           .canonical;
          descriptor.type_uuid = *category_type_uuid;
          descriptor.nullability = api::RelationalNullability::kNonNull;
          local_category_descriptor = std::move(descriptor);
        }
      }
    }
    std::uint32_t maximum_expression_id = 0;
    if (exact) {
      for (const auto& expression : input.relational_dag.expressions) {
        maximum_expression_id =
            std::max(maximum_expression_id, expression.expression_id);
      }
      exact = maximum_expression_id <=
              std::numeric_limits<std::uint32_t>::max() - 3;
    }
    if (exact) {
      const auto digest_id = maximum_expression_id + 1;
      const auto binding_id = maximum_expression_id + 2;
      const auto category_id = maximum_expression_id + 3;
      const auto local_expression_for = [&](const std::uint32_t expression_id) {
        return std::ranges::find_if(
            enriched.expressions, [&](const auto& expression) {
              return expression.expression_id == expression_id;
            });
      };
      auto local_generation = local_expression_for(output_expression_ids[2]);
      auto local_rank = local_expression_for(output_expression_ids[4]);
      auto local_match = local_expression_for(match->expression_id);
      exact = local_generation != enriched.expressions.end() &&
              local_rank != enriched.expressions.end() &&
              local_match != enriched.expressions.end() &&
              local_expression_for(digest_id) == enriched.expressions.end() &&
              local_expression_for(binding_id) == enriched.expressions.end() &&
              local_expression_for(category_id) == enriched.expressions.end();
      if (exact) {
        local_generation->expression_kind =
            api::RelationalExpressionKind::kLiteral;
        local_generation->function_uuid.reset();
        local_generation->bound_name_uuid.reset();
        local_generation->literal_kind =
            api::RelationalLiteralKind::kNumeric;
        local_generation->literal_or_parameter_ref =
            std::to_string(input.context.catalog_generation_id);
        local_generation->operator_name.reset();
        local_generation->child_expression_ids.clear();
        local_rank->expression_kind = api::RelationalExpressionKind::kLiteral;
        local_rank->function_uuid.reset();
        local_rank->bound_name_uuid.reset();
        local_rank->literal_kind = api::RelationalLiteralKind::kNumeric;
        local_rank->literal_or_parameter_ref = std::to_string(top_k_value);
        local_rank->operator_name.reset();
        local_rank->child_expression_ids.clear();

        api::RelationalExpressionRecord digest;
        digest.expression_id = digest_id;
        digest.expression_kind = api::RelationalExpressionKind::kLiteral;
        digest.result_descriptor_id = body_descriptor->descriptor_id;
        digest.literal_kind = api::RelationalLiteralKind::kString;
        digest.literal_or_parameter_ref =
            "9033908d159ddd442f2042467fd49e0a12b47679f7514e9aa6e55488e151d316";
        enriched.expressions.push_back(std::move(digest));
        api::RelationalExpressionRecord binding;
        binding.expression_id = binding_id;
        binding.expression_kind =
            api::RelationalExpressionKind::kFunctionCall;
        binding.result_descriptor_id = outputs[1]->descriptor_id;
        binding.operator_name = "SEARCH_ANALYZER_BINDING";
        binding.child_expression_ids = {output_expression_ids[1],
                                        output_expression_ids[2], digest_id};
        enriched.expressions.push_back(std::move(binding));
        const auto descriptor_count_before_category =
            enriched.descriptors.size();
        if (local_category_descriptor.has_value()) {
          enriched.descriptors.push_back(*local_category_descriptor);
        } else if (std::ranges::none_of(
                       enriched.descriptors, [&](const auto& descriptor) {
                         return descriptor.descriptor_id ==
                                category_descriptor_id;
                       })) {
          enriched.descriptors.push_back(*category_descriptor);
        }
        const auto local_category_descriptor_record = std::ranges::find_if(
            enriched.descriptors, [&](const auto& descriptor) {
              return descriptor.descriptor_id == category_descriptor_id;
            });
        exact = local_category_descriptor_record != enriched.descriptors.end() &&
                std::ranges::count_if(
                    enriched.descriptors, [&](const auto& descriptor) {
                      return descriptor.descriptor_id ==
                             category_descriptor_id;
                    }) == 1 &&
                local_category_descriptor_record->descriptor_uuid ==
                    loaded.descriptor.columns[1]
                        .value_descriptor.descriptor_uuid.canonical &&
                local_category_descriptor_record->type_uuid ==
                    *category_type_uuid &&
                local_category_descriptor_record->nullability ==
                    api::RelationalNullability::kNonNull &&
                !local_category_descriptor_record->collation_uuid.has_value() &&
                !local_category_descriptor_record->timezone_profile_id
                     .has_value() &&
                std::ranges::none_of(
                    enriched.outputs, [&](const auto& output) {
                      return output.descriptor_id == category_descriptor_id;
                    }) &&
                enriched.descriptors.size() ==
                    descriptor_count_before_category + 1;
        if (exact) {
          api::RelationalExpressionRecord category;
          category.expression_id = category_id;
          category.expression_kind =
              api::RelationalExpressionKind::kIdentifier;
          category.result_descriptor_id = category_descriptor_id;
          category.bound_name_uuid =
              loaded.descriptor.columns[1].column_uuid.canonical;
          enriched.expressions.push_back(std::move(category));
          local_match = local_expression_for(match->expression_id);
          local_match->child_expression_ids[2] = binding_id;
          local_match->child_expression_ids[3] = output_expression_ids[4];
          std::erase_if(enriched.expressions, [&](const auto& expression) {
            return expression.expression_id ==
                       owned_analyzer->expression_id ||
                   expression.expression_id == owned_top_k->expression_id;
          });
          local_source = std::ranges::find_if(
              enriched.nodes, [&](const auto& candidate) {
                return candidate.node_id == source.node_id;
              });
          local_source->bound_expression_ids = output_expression_ids;
          local_source->bound_expression_ids.insert(
              local_source->bound_expression_ids.end(),
              {alias->expression_id, query_text->expression_id,
               terms->expression_id, digest_id, binding_id,
               match->expression_id, category_id});
          exact = std::ranges::count_if(
                      enriched.expressions, [&](const auto& expression) {
                        return expression.result_descriptor_id ==
                               category_descriptor_id;
                      }) == 1 &&
                  std::ranges::find_if(
                      enriched.expressions, [&](const auto& expression) {
                        return expression.result_descriptor_id ==
                               category_descriptor_id;
                      })->expression_id == category_id;
          const auto local_validation =
              api::ValidateTypedRelationalDag(enriched);
          exact = exact && local_validation.accepted &&
                  local_validation.issues.empty();
        }
      }
    }
    if (exact) local.relational_dag = std::move(enriched);
  }
  if (family == "document" || family == "graph") {
    // QOW-SOURCE-RCP080-OPERATOR-LOCAL-NON-TIMESTAMP-VIEW-V1
    // The original multileg DAG and immutable EngineRequestContext retain the
    // one common statement timestamp.  Standalone document/graph typed-DAG
    // forms classify no timestamp; this derived semantic view carries none.
    local.relational_dag.statement_timestamp.clear();
  }
  if (family == "spatial" || family == "columnar") {
    *attempted = ExecuteCanonicalSpatialColumnarFamilyQuery(local, capture);
  } else if (family == "search") {
    *attempted = ExecuteCanonicalSearchFamilyQuery(local, capture);
  } else if (family == "vector") {
    *attempted = ExecuteCanonicalVectorFamilyQuery(local, capture);
  } else if (family == "time_series") {
    *attempted = ExecuteCanonicalTimeSeriesFamilyQuery(local, capture);
  } else if (family == "key_value") {
    *attempted = ExecuteCanonicalKeyValueFamilyQuery(local, capture);
  } else if (family == "graph") {
    *attempted = ExecuteCanonicalGraphFamilyQuery(local, capture);
  } else {
    *attempted = ExecuteCanonicalDocumentFamilyQuery(local, capture);
  }
  return capture->captured && capture->family_id == family &&
         capture->logical_node_id == source.node_id;
}

struct Rcp080BoundedSourceV1 {
  const api::RelationalDagNode* node{nullptr};
  std::string family_id;
  std::string operation_id;
  std::vector<std::string> operation_ids;
  api::MgaRelationStorageDescriptor persisted;
  std::vector<exec::ExecutorColumnDescriptor> columns;
  std::vector<std::string> output_descriptor_uuids;
  std::string spatial_crs_uuid;
  std::uint64_t spatial_crs_generation{0};
};

std::string Rcp080LogicalOperatorV1(const std::string_view family) {
  if (family == "document") return "LOGICAL_DOCUMENT_SOURCE_V1";
  if (family == "graph") return "LOGICAL_GRAPH_SOURCE_V1";
  if (family == "key_value") return "LOGICAL_KEY_VALUE_SOURCE_V1";
  if (family == "time_series") return "LOGICAL_TIME_SERIES_SOURCE_V1";
  if (family == "vector") return "LOGICAL_VECTOR_SOURCE_V1";
  if (family == "search") return "LOGICAL_SEARCH_SOURCE_V1";
  if (family == "spatial") return "LOGICAL_SPATIAL_SOURCE_V1";
  if (family == "columnar") return "LOGICAL_COLUMNAR_SOURCE_V1";
  return {};
}

std::string Rcp080ImplementationV1(const std::string_view family) {
  if (family == "relational") return "physical_relational_heap_scan_v1";
  if (family == "document") return "physical_document_path_scan_v1";
  if (family == "graph") return "physical_graph_adjacency_scan_v1";
  if (family == "key_value") return "physical_key_value_scan_v1";
  if (family == "time_series") return "physical_time_series_range_scan_v1";
  if (family == "vector") return "physical_vector_search_v1";
  if (family == "search") return "physical_search_rank_scan_v1";
  if (family == "spatial") return "physical_spatial_index_scan_v1";
  if (family == "columnar") return "physical_columnar_zone_scan_v1";
  return {};
}

std::string Rcp080OperationV1(const std::string_view family) {
  if (family == "relational") return "RELATIONAL_HEAP_SCAN";
  if (family == "document") return "DOCUMENT_FIND";
  if (family == "graph") return "GRAPH_MATCH";
  if (family == "key_value") return "KEY_VALUE_GET";
  if (family == "time_series") return "TIME_SERIES_RANGE_READ";
  if (family == "vector") return "VECTOR_EXACT_SEARCH";
  if (family == "search") return "SEARCH_RANKED_QUERY";
  if (family == "spatial") return "SPATIAL_SOURCE";
  if (family == "columnar") return "COLUMNAR_SOURCE";
  return {};
}

}  // namespace

CanonicalObjectFreeValuesExecutionResult
ExecuteCanonicalBoundedModelFamilyCompositionQuery(
    const CanonicalCurrentHeapExecutionRequest& input) {
  CanonicalObjectFreeValuesExecutionResult result;
  const auto& dag = input.relational_dag;
  std::vector<const api::RelationalDagNode*> sources;
  std::vector<const api::RelationalDagNode*> joins;
  for (const auto& node : dag.nodes) {
    const bool source =
        node.node_kind == api::RelationalDagNodeKind::kScan &&
        (node.semantic_variant_id == "relation.source.v1" ||
         node.semantic_variant_id == "SBLR_MODEL_SOURCE_V1");
    if (source) {
      sources.push_back(&node);
    } else if (node.node_kind == api::RelationalDagNodeKind::kJoin) {
      joins.push_back(&node);
    } else {
      return result;
    }
  }
  if (sources.size() < 3 || sources.size() > 9 ||
      joins.size() + 1 != sources.size()) {
    return result;
  }
  std::ranges::sort(sources, {}, [](const auto* node) { return node->node_id; });
  std::ranges::sort(joins, {}, [](const auto* node) { return node->node_id; });
  std::uint32_t left_node_id = sources.front()->node_id;
  std::vector<std::uint32_t> accumulated_descriptors =
      sources.front()->output_descriptor_ids;
  for (std::size_t ordinal = 1; ordinal < sources.size(); ++ordinal) {
    const auto* join = joins[ordinal - 1];
    if (join->semantic_variant_id != "join.cross.v1" ||
        join->input_node_ids !=
            std::vector<std::uint32_t>{left_node_id,
                                       sources[ordinal]->node_id} ||
        !join->bound_expression_ids.empty()) {
      return result;
    }
    accumulated_descriptors.insert(
        accumulated_descriptors.end(),
        sources[ordinal]->output_descriptor_ids.begin(),
        sources[ordinal]->output_descriptor_ids.end());
    if (join->output_descriptor_ids != accumulated_descriptors) return result;
    left_node_id = join->node_id;
  }
  if (left_node_id != dag.root_node_id) return result;

  result.profile_matched = true;
  CanonicalObjectFreeValuesExecutionRequest response_context;
  response_context.context = input.context;
  response_context.relational_dag = dag;
  const auto refuse = [&](std::string diagnostic_id, std::string detail) {
    result.optimizer_selected = false;
    result.physical_dag_published = false;
    result.physical_dag_executed = false;
    result.runtime_actuals_attached = false;
    result.canonical_result_published = false;
    result.physical_node_count = 0;
    result.canonical_result_column_count = 0;
    result.canonical_result_row_count = 0;
    result.selected_plan_uuid.clear();
    result.canonical_result_bytes.clear();
    result.api_result = Failure(response_context, std::move(diagnostic_id),
                                std::move(detail));
    return result;
  };

  std::vector<std::string> families;
  families.reserve(sources.size());
  std::size_t model_count = 0;
  std::unordered_set<std::uint32_t> attached_model_roots;
  std::unordered_set<std::uint32_t> attached_model_expressions;
  const auto exact_root_family = [](
                                     const std::optional<std::string>& value) {
    if (!value.has_value()) return std::string{};
    const std::string_view operation = *value;
    if (operation == "DOCUMENT_SOURCE") return std::string("document");
    if (operation == "GRAPH_MATCH") return std::string("graph");
    if (operation == "KV_KEY") return std::string("key_value");
    if (operation == "TIME_RANGE") return std::string("time_series");
    if (operation == "VECTOR_NEAREST") return std::string("vector");
    if (operation == "SEARCH_MATCH") return std::string("search");
    if (operation == "SPATIAL_SOURCE") return std::string("spatial");
    if (operation == "COLUMNAR_SOURCE") return std::string("columnar");
    return std::string{};
  };
  const auto dag_expression_for = [&](const std::uint32_t expression_id)
      -> const api::RelationalExpressionRecord* {
    const auto expression = std::ranges::find_if(
        dag.expressions, [&](const auto& candidate) {
          return candidate.expression_id == expression_id;
        });
    return expression == dag.expressions.end() ? nullptr : &*expression;
  };
  for (const auto* source : sources) {
    const bool model_source =
        source->semantic_variant_id == "SBLR_MODEL_SOURCE_V1";
    // QOW-SOURCE-RCP080-CANONICAL-EXECUTE-SEARCH-TERMS-AUXILIARY-V1
    // SEARCH_TERMS is not a model-family root.  It is the one functionless
    // query constructor attached as ordinal-1 child of this source's exact
    // SEARCH_MATCH root; complete owned reachability below remains an
    // independent mandatory proof.
    const api::RelationalExpressionRecord* candidate_search_root = nullptr;
    const api::RelationalExpressionRecord* attached_search_terms = nullptr;
    for (const auto expression_id : source->bound_expression_ids) {
      const auto* expression = dag_expression_for(expression_id);
      if (expression == nullptr ||
          expression->operator_name != "SEARCH_MATCH") {
        continue;
      }
      if (candidate_search_root != nullptr) {
        return refuse(
            "SB_MODEL_BINDING_INCOMPLETE_V1",
            "bounded composition model operation root is not exactly attached to its source");
      }
      candidate_search_root = expression;
    }
    if (candidate_search_root != nullptr &&
        candidate_search_root->child_expression_ids.size() == 4) {
      const auto* expression = dag_expression_for(
          candidate_search_root->child_expression_ids[1]);
      const bool attached_to_same_source =
          expression != nullptr &&
          std::ranges::find(source->bound_expression_ids,
                            expression->expression_id) !=
              source->bound_expression_ids.end();
      if (attached_to_same_source &&
          expression->expression_kind ==
              api::RelationalExpressionKind::kFunctionCall &&
          !expression->function_uuid.has_value() &&
          !expression->bound_name_uuid.has_value() &&
          !expression->literal_kind.has_value() &&
          !expression->literal_or_parameter_ref.has_value() &&
          expression->operator_name == "SEARCH_TERMS" &&
          expression->child_expression_ids.size() == 1) {
        attached_search_terms = expression;
      }
    }
    std::vector<std::pair<std::uint32_t, std::string>> roots;
    for (const auto expression_id : source->bound_expression_ids) {
      const auto* expression = dag_expression_for(expression_id);
      if (expression == nullptr) {
        return refuse("SB_MODEL_BINDING_INCOMPLETE_V1",
                      "bounded composition source expression is unresolved");
      }
      const auto root_family = exact_root_family(expression->operator_name);
      const bool functionless_operation =
          expression->expression_kind ==
              api::RelationalExpressionKind::kFunctionCall &&
          !expression->function_uuid.has_value();
      if (root_family.empty()) {
        if (functionless_operation && expression != attached_search_terms) {
          return refuse("SB_MODEL_BINDING_INCOMPLETE_V1",
                        "bounded composition contains an unregistered functionless operation root");
        }
        continue;
      }
      const auto expected_arity =
          root_family == "document" ? 1U
          : root_family == "graph" ? 2U
          : root_family == "key_value" ? 1U
          : root_family == "time_series" ? 3U
          : root_family == "vector" || root_family == "search" ? 4U
                                                                  : 0U;
      const auto expected_root_bound_name =
          root_family == "spatial" || root_family == "columnar"
              ? std::optional<std::string>{
                    source->required_object_uuids.empty()
                        ? std::string{}
                        : source->required_object_uuids.front()}
              : std::optional<std::string>{};
      if (!functionless_operation || expression->literal_kind.has_value() ||
          expression->literal_or_parameter_ref.has_value() ||
          expression->child_expression_ids.size() != expected_arity ||
          source->required_object_uuids.size() != 1 ||
          expression->bound_name_uuid != expected_root_bound_name ||
          !attached_model_roots.insert(expression_id).second) {
        return refuse("SB_MODEL_BINDING_INCOMPLETE_V1",
                      "bounded composition model operation root is not exactly attached to its source");
      }
      roots.emplace_back(expression_id, root_family);
    }
    if (model_source != (roots.size() == 1)) {
      return refuse("SB_MODEL_BINDING_INCOMPLETE_V1",
                    "bounded composition model source does not have exactly one recognized attached root");
    }
    const auto family = model_source ? roots.front().second : "relational";
    if (model_source) {
      const auto root_id = roots.front().first;
      const auto* root = dag_expression_for(root_id);
      std::unordered_set<std::uint32_t> output_expression_ids;
      for (const auto& output : dag.outputs) {
        if (output.relation_node_id == source->node_id) {
          output_expression_ids.insert(output.expression_id);
        }
      }
      std::unordered_set<std::uint32_t> owned;
      for (const auto expression_id : source->bound_expression_ids) {
        if (!output_expression_ids.contains(expression_id)) {
          owned.insert(expression_id);
        }
      }
      std::unordered_set<std::uint32_t> reachable;
      std::vector<std::uint32_t> pending{root_id};
      if (family == "key_value") {
        const auto equality = std::ranges::find_if(
            owned, [&](const auto expression_id) {
              const auto* expression = dag_expression_for(expression_id);
              return expression != nullptr &&
                     expression->expression_kind ==
                         api::RelationalExpressionKind::kBinary &&
                     expression->operator_name == "=" &&
                     expression->child_expression_ids.size() == 2 &&
                     expression->child_expression_ids.front() == root_id;
            });
        if (equality == owned.end()) {
          return refuse("SB_MODEL_BINDING_INCOMPLETE_V1",
                        "bounded KV leg lacks its exact key equality");
        }
        pending.push_back(*equality);
      }
      while (!pending.empty()) {
        const auto expression_id = pending.back();
        pending.pop_back();
        if (!reachable.insert(expression_id).second) continue;
        const auto* expression = dag_expression_for(expression_id);
        if (expression == nullptr || !owned.contains(expression_id)) {
          return refuse("SB_MODEL_BINDING_INCOMPLETE_V1",
                        "bounded model operation closure is detached");
        }
        pending.insert(pending.end(), expression->child_expression_ids.begin(),
                       expression->child_expression_ids.end());
      }
      const auto* alias = root->child_expression_ids.empty()
                              ? nullptr
                              : dag_expression_for(
                                    root->child_expression_ids.front());
      const bool alias_exact =
          root->child_expression_ids.empty() ||
          (alias != nullptr &&
           alias->expression_kind ==
               api::RelationalExpressionKind::kIdentifier &&
           alias->bound_name_uuid == source->required_object_uuids.front());
      const bool search_auxiliary_exact =
          family != "search" ||
          (root->child_expression_ids.size() == 4 &&
           [&] {
             const auto* query =
                 dag_expression_for(root->child_expression_ids[1]);
             return query != nullptr &&
                    query->expression_kind ==
                        api::RelationalExpressionKind::kFunctionCall &&
                    !query->function_uuid.has_value() &&
                    !query->bound_name_uuid.has_value() &&
                    query->operator_name == "SEARCH_TERMS" &&
                    query->child_expression_ids.size() == 1;
           }());
      if (!alias_exact || !search_auxiliary_exact || reachable != owned ||
          std::ranges::any_of(owned, [&](const auto expression_id) {
            return !attached_model_expressions.insert(expression_id).second;
          })) {
        return refuse("SB_MODEL_BINDING_INCOMPLETE_V1",
                      "bounded model operation closure is orphaned or crosses legs");
      }
    }
    model_count += model_source ? 1 : 0;
    families.push_back(family);
  }
  if (model_count < 2) return result;
  const auto global_root_count = std::ranges::count_if(
      dag.expressions, [&](const auto& expression) {
        return !exact_root_family(expression.operator_name).empty();
      });
  if (global_root_count != model_count ||
      attached_model_roots.size() != model_count ||
      std::ranges::any_of(dag.expressions, [&](const auto& expression) {
        const bool source_output = std::ranges::any_of(
            dag.outputs, [&](const auto& output) {
              return output.expression_id == expression.expression_id;
            });
        return !source_output &&
               !attached_model_expressions.contains(expression.expression_id);
      })) {
    return refuse("SB_MODEL_BINDING_INCOMPLETE_V1",
                  "bounded composition has an unattached or duplicated model operation root");
  }

  std::string composition_profile_id;
  if (families.size() == 3) {
    composition_profile_id = "COMP-3-LINEAR-V1";
  } else if (families == std::vector<std::string>{
                             "relational", "document", "graph", "vector"}) {
    composition_profile_id = "COMP-4-MIXED-V1";
  } else if (families == std::vector<std::string>{
                             "relational", "document", "graph", "key_value",
                             "time_series", "vector", "search", "spatial",
                             "columnar"}) {
    composition_profile_id = "COMP-9-FULL-UNIVERSE-V1";
  } else {
    return refuse("SB_MODEL_COMPOSITION_PROFILE_REFUSED_V1",
                  "bounded source arity or lexical family order has no signed composition profile");
  }

  // The bounded composition route may only consume the exact engine-issued
  // statement, authorization, and optimizer-admission cohort.  Refuse that
  // cohort before resolving a snapshot, consulting authorization, capturing
  // a provider, loading a relation descriptor, or entering either coordinator.
  // In particular, zero values are absence of authority: they must never be
  // repaired into a plausible generation or resource limit.
  const auto refuse_pre_access = [&](std::string diagnostic_id,
                                     std::string detail) {
    auto refused = refuse(std::move(diagnostic_id), std::move(detail));
    refused.api_result.evidence.push_back(
        {"canonical.model_composition_provider_entry_count", "0"});
    refused.api_result.evidence.push_back(
        {"canonical.model_composition_real_mga_read_count", "0"});
    refused.api_result.evidence.push_back(
        {"canonical.model_composition_observed_data_access_count", "0"});
    refused.api_result.evidence.push_back(
        {"canonical.model_composition_root_publication_count", "0"});
    return refused;
  };
  const auto complete_typed_dag = api::ValidateTypedRelationalDag(dag);
  if (!complete_typed_dag.accepted) {
    const auto& issue = complete_typed_dag.issues.front();
    return refuse_pre_access(issue.diagnostic_id,
                             "complete multileg typed DAG refused:" +
                                 issue.field_id);
  }
  const auto& context = input.context;
  const auto& authorization = context.authorization_context;
  if (!CanonicalUuidText(context.database_uuid.canonical) ||
      !CanonicalUuidText(context.transaction_uuid.canonical) ||
      !CanonicalUuidText(context.statement_uuid.canonical) ||
      !CanonicalUuidText(context.statement_snapshot_uuid.canonical) ||
      !context.statement_metadata_snapshot_engine_owned ||
      !CanonicalUuidText(
          context.statement_metadata_snapshot_uuid.canonical) ||
      context.local_transaction_id == 0 || context.statement_timestamp.empty() ||
      dag.statement_uuid != context.statement_uuid.canonical ||
      dag.statement_timestamp != context.statement_timestamp ||
      dag.owning_transaction_uuid != context.transaction_uuid.canonical ||
      dag.statement_snapshot_uuid !=
          context.statement_snapshot_uuid.canonical ||
      dag.statement_metadata_snapshot_uuid !=
          context.statement_metadata_snapshot_uuid.canonical ||
      dag.local_transaction_id != context.local_transaction_id ||
      dag.snapshot_visible_through_local_transaction_id !=
          context.snapshot_visible_through_local_transaction_id) {
    return refuse_pre_access(
        "SB_MODEL_MGA_CONTEXT_MISMATCH_V1",
        "bounded composition engine-issued statement/MGA cohort is absent or substituted");
  }
  if (!CanonicalUuidText(context.catalog_epoch_uuid.canonical) ||
      context.catalog_generation_id == 0 ||
      authorization.catalog_generation_id != context.catalog_generation_id ||
      dag.bound_catalog_epoch_uuid != context.catalog_epoch_uuid.canonical) {
    return refuse_pre_access(
        "SB_MODEL_CATALOG_GENERATION_STALE_V1",
        "bounded composition catalog epoch or generation cohort is absent or substituted");
  }
  if (!context.security_context_present || !authorization.present ||
      !CanonicalUuidText(context.principal_uuid.canonical) ||
      !CanonicalUuidText(authorization.authority_uuid.canonical) ||
      authorization.principal_uuid.canonical !=
          context.principal_uuid.canonical ||
      context.security_epoch == 0 ||
      authorization.security_epoch != context.security_epoch ||
      authorization.policy_epoch == 0 ||
      dag.bound_security_context_uuid !=
          authorization.authority_uuid.canonical) {
    return refuse_pre_access(
        "SB_MODEL_SECURITY_ADMISSION_REFUSED_V1",
        "bounded composition security or policy authority cohort is absent or substituted");
  }
  std::uint64_t admitted_at_monotonic_ns = 0;
  const auto monotonic_parse = std::from_chars(
      context.current_monotonic_ns.data(),
      context.current_monotonic_ns.data() + context.current_monotonic_ns.size(),
      admitted_at_monotonic_ns);
  if (context.resource_epoch == 0 ||
      !CanonicalUuidText(
          context.optimizer_capability_snapshot_uuid.canonical) ||
      !CanonicalUuidText(
          context.optimizer_resource_snapshot_uuid.canonical) ||
      !CanonicalUuidText(context.optimizer_route_snapshot_uuid.canonical) ||
      context.optimizer_route_epoch == 0 ||
      context.optimizer_route_generation == 0 ||
      context.optimizer_route_generation ==
          std::numeric_limits<std::uint64_t>::max() ||
      context.optimizer_memory_budget_bytes < 64 * 1024 ||
      context.optimizer_maximum_candidate_count == 0 ||
      context.optimizer_maximum_memo_groups == 0 ||
      context.optimizer_maximum_search_steps == 0 ||
      context.optimizer_maximum_planning_time_ns == 0 ||
      context.current_monotonic_ns.empty() ||
      monotonic_parse.ec != std::errc{} ||
      monotonic_parse.ptr != context.current_monotonic_ns.data() +
                                 context.current_monotonic_ns.size() ||
      admitted_at_monotonic_ns == 0) {
    return refuse_pre_access(
        "SBLR.PLAN_TREE.RESOURCE_LIMIT",
        "bounded composition optimizer resource, route, or monotonic cohort is absent or substituted");
  }

  api::EngineResolveStatementSnapshotRequest snapshot_request;
  snapshot_request.context = input.context;
  const auto snapshot = api::EngineResolveStatementSnapshot(snapshot_request);
  if (!snapshot.ok) {
    return refuse("SB_MODEL_MGA_CONTEXT_MISMATCH_V1",
                  "bounded composition statement snapshot is unavailable");
  }
  auto mga = PhysicalMgaContextFromResolvedSnapshot(
      input.context, snapshot.snapshot_vector);
  mga.statement_timestamp = dag.statement_timestamp;
  if (!exec::PhysicalMgaStatementContextValid(mga)) {
    return refuse("SB_MODEL_MGA_CONTEXT_MISMATCH_V1",
                  "bounded composition MGA statement context is invalid");
  }
  const auto identity_scope =
      dag.bound_sblr_tree_uuid + ":" + input.context.statement_uuid.canonical;
  const auto generation = input.context.catalog_generation_id;
  const auto security_generation = input.context.security_epoch;
  const auto policy_generation =
      input.context.authorization_context.policy_epoch;
  const auto resource_generation = input.context.resource_epoch;
  const auto selected_plan_generation =
      input.context.optimizer_route_generation;
  const auto maximum_rows = std::min<std::uint64_t>(
      65'536, input.context.optimizer_maximum_candidate_count);
  const auto statement_memory = input.context.optimizer_memory_budget_bytes;
  const auto retained_grant_count = sources.size() * 3 - 1;
  const auto retained_grant =
      statement_memory / static_cast<std::uint64_t>(retained_grant_count);
  if (retained_grant == 0) {
    return refuse("SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                  "bounded composition retained grant cannot be represented");
  }
  const auto leg_memory = retained_grant;
  const auto exchange_memory = retained_grant;

  const auto descriptor_for = [&](const std::uint32_t descriptor_id) {
    return std::ranges::find_if(dag.descriptors, [&](const auto& descriptor) {
      return descriptor.descriptor_id == descriptor_id;
    });
  };
  const auto expression_for = [&](const std::uint32_t expression_id) {
    return std::ranges::find_if(dag.expressions, [&](const auto& expression) {
      return expression.expression_id == expression_id;
    });
  };
  std::array<std::string, 5> exact_derived_type_uuids;
  if (std::ranges::any_of(families, [](const std::string_view family) {
        return family == "key_value" || family == "time_series" ||
               family == "vector" || family == "search";
      })) {
    exact_derived_type_uuids = {
        ExactCanonicalCoreDatatypeTypeUuidV1("uuid"),
        ExactCanonicalCoreDatatypeTypeUuidV1("character"),
        ExactCanonicalCoreDatatypeTypeUuidV1("timestamp"),
        ExactCanonicalCoreDatatypeTypeUuidV1("real64"),
        ExactCanonicalCoreDatatypeTypeUuidV1("uint64")};
    if (std::ranges::any_of(exact_derived_type_uuids,
                            [](const auto& uuid) { return uuid.empty(); })) {
      return refuse("SB_MODEL_RESULT_DESCRIPTOR_SOURCE_BINDING_INVALID_V1",
                    "bounded derived source core type registry is unavailable");
    }
  }
  const auto exact_derived_type_uuid =
      [&](const std::string_view canonical_type_name) -> std::string_view {
    if (canonical_type_name == "uuid") return exact_derived_type_uuids[0];
    if (canonical_type_name == "text") return exact_derived_type_uuids[1];
    if (canonical_type_name == "timestamp_tz") {
      return exact_derived_type_uuids[2];
    }
    if (canonical_type_name == "real64") return exact_derived_type_uuids[3];
    if (canonical_type_name == "uint64") return exact_derived_type_uuids[4];
    return {};
  };
  std::unordered_set<std::string> object_uuids;
  std::vector<Rcp080BoundedSourceV1> prepared_sources;
  prepared_sources.reserve(sources.size());
  for (std::size_t source_ordinal = 0; source_ordinal < sources.size();
       ++source_ordinal) {
    const auto* source = sources[source_ordinal];
    if (!source->input_node_ids.empty() ||
        source->required_object_uuids.size() != 1 ||
        source->output_descriptor_ids.empty() ||
        !object_uuids.insert(source->required_object_uuids.front()).second) {
      return refuse("SB_MODEL_BINDING_INCOMPLETE_V1",
                    "bounded composition source object binding is missing or duplicated");
    }
    const auto authorization = api::EvaluateMaterializedAuthorization(
        input.context, input.context.authorization_context, "SELECT",
        source->required_object_uuids.front());
    if (!authorization.authorized || authorization.denied ||
        authorization.policy_recheck_required ||
        !authorization.diagnostics.empty()) {
      return refuse("SB_MODEL_SECURITY_ADMISSION_REFUSED_V1",
                    "bounded composition SELECT authorization was refused");
    }
    const auto loaded = api::LoadMgaRelationStorageDescriptor(
        input.context, source->required_object_uuids.front());
    if (!loaded.ok || loaded.descriptor.relation_uuid.canonical !=
                          source->required_object_uuids.front() ||
        loaded.descriptor.database_uuid.canonical !=
            input.context.database_uuid.canonical ||
        loaded.descriptor.storage_profile != "local_mga_rowstore_v1" ||
        loaded.descriptor.descriptor_generation == 0) {
      return refuse("SB_MODEL_TYPED_EXCHANGE_INVALID_V1",
                    "bounded composition current relation descriptor is invalid");
    }
    std::vector<const api::RelationalOutputRecord*> outputs;
    for (const auto& output : dag.outputs) {
      if (output.relation_node_id == source->node_id) outputs.push_back(&output);
    }
    std::ranges::sort(outputs, {}, &api::RelationalOutputRecord::ordinal);
    if (outputs.size() != source->output_descriptor_ids.size()) {
      return refuse("SB_MODEL_TYPED_EXCHANGE_INVALID_V1",
                    "bounded composition source output inventory is incomplete");
    }
    Rcp080BoundedSourceV1 prepared;
    prepared.node = source;
    prepared.family_id = families[source_ordinal];
    prepared.operation_id = Rcp080OperationV1(prepared.family_id);
    if (prepared.family_id == "spatial" ||
        prepared.family_id == "columnar") {
      prepared.operation_ids = {prepared.operation_id};
    }
    prepared.persisted = loaded.descriptor;
    const bool derived_projection =
        prepared.family_id == "key_value" ||
        prepared.family_id == "time_series" ||
        prepared.family_id == "vector" || prepared.family_id == "search";
    const std::vector<std::string_view> derived_names =
        prepared.family_id == "key_value"
            ? std::vector<std::string_view>{"row_uuid", "key", "value"}
        : prepared.family_id == "time_series"
            ? std::vector<std::string_view>{
                  "row_uuid", "series_uuid", "metric_uuid",
                  "point_timestamp", "tags", "value"}
        : prepared.family_id == "vector"
            ? std::vector<std::string_view>{"row_uuid", "distance", "score"}
        : prepared.family_id == "search"
            ? std::vector<std::string_view>{
                  "document_uuid", "analyzer_uuid", "analyzer_generation",
                  "score", "rank"}
            : std::vector<std::string_view>{};
    const std::vector<std::string_view> derived_types =
        prepared.family_id == "key_value"
            ? std::vector<std::string_view>{"uuid", "text", "text"}
        : prepared.family_id == "time_series"
            ? std::vector<std::string_view>{
                  "uuid", "uuid", "uuid", "timestamp_tz", "text",
                  "real64"}
        : prepared.family_id == "vector"
            ? std::vector<std::string_view>{"uuid", "real64", "real64"}
        : prepared.family_id == "search"
            ? std::vector<std::string_view>{"uuid", "uuid", "uint64",
                                            "real64", "uint64"}
            : std::vector<std::string_view>{};
    if ((derived_projection && outputs.size() != derived_names.size()) ||
        (!derived_projection &&
         outputs.size() != prepared.persisted.columns.size())) {
      return refuse("SB_MODEL_TYPED_EXCHANGE_INVALID_V1",
                    "bounded composition source storage/public width is invalid");
    }
    std::optional<std::string> search_analyzer_uuid;
    for (std::size_t ordinal = 0; ordinal < outputs.size(); ++ordinal) {
      const auto descriptor = descriptor_for(outputs[ordinal]->descriptor_id);
      const auto expression = expression_for(outputs[ordinal]->expression_id);
      if (descriptor == dag.descriptors.end() ||
          expression == dag.expressions.end() ||
          outputs[ordinal]->ordinal != ordinal || !outputs[ordinal]->visible ||
          outputs[ordinal]->descriptor_id !=
              source->output_descriptor_ids[ordinal] ||
          expression->result_descriptor_id != descriptor->descriptor_id) {
        return refuse("SB_MODEL_TYPED_EXCHANGE_INVALID_V1",
                      "bounded composition source descriptor binding was substituted");
      }
      if (derived_projection) {
        const auto expected_bound_name =
            prepared.family_id == "search" &&
                    (ordinal == 1 || ordinal == 2)
                ? expression->bound_name_uuid
            : prepared.family_id == "key_value"
                ? expression->bound_name_uuid
            : prepared.family_id == "time_series"
                ? expression->bound_name_uuid
                : std::optional<std::string>{
                      source->required_object_uuids.front()};
        const auto expected_timezone_profile =
            prepared.family_id == "time_series" && ordinal == 3 &&
                    outputs[ordinal]->output_name_utf8 == "point_timestamp"
                ? std::optional<std::string>{"UTC"}
                : std::nullopt;
        if (outputs[ordinal]->output_name_utf8 != derived_names[ordinal] ||
            descriptor->nullability !=
                api::RelationalNullability::kNonNull ||
            descriptor->collation_uuid.has_value() ||
            descriptor->timezone_profile_id != expected_timezone_profile ||
            !expected_bound_name.has_value() ||
            expression->bound_name_uuid != expected_bound_name ||
            (prepared.family_id == "search" &&
             (ordinal == 1 || ordinal == 2) &&
             expression->bound_name_uuid ==
                 source->required_object_uuids.front())) {
          return refuse("SB_MODEL_TYPED_EXCHANGE_INVALID_V1",
                        "bounded derived source public binding was substituted");
        }
        if (prepared.family_id == "search" && ordinal == 1) {
          search_analyzer_uuid = expression->bound_name_uuid;
        } else if (prepared.family_id == "search" && ordinal == 2 &&
                   expression->bound_name_uuid != search_analyzer_uuid) {
          return refuse("SB_MODEL_TYPED_EXCHANGE_INVALID_V1",
                        "bounded search analyzer public binding diverged");
        }
        const auto exact_type_uuid =
            exact_derived_type_uuid(derived_types[ordinal]);
        if (exact_type_uuid.empty() ||
            descriptor->type_uuid != exact_type_uuid) {
          return refuse("SB_MODEL_RESULT_DESCRIPTOR_SOURCE_BINDING_INVALID_V1",
                        "bounded derived source type UUID was substituted");
        }
        api::EngineDescriptor engine_descriptor;
        engine_descriptor.descriptor_uuid.canonical =
            descriptor->descriptor_uuid;
        engine_descriptor.descriptor_kind = "scalar";
        engine_descriptor.canonical_type_name =
            std::string(derived_types[ordinal]);
        engine_descriptor.encoded_descriptor =
            "type_uuid=" + descriptor->type_uuid + ";nullability=non_null";
        prepared.columns.push_back(
            {std::string(derived_names[ordinal]), std::move(engine_descriptor),
             false, descriptor->descriptor_id});
      } else {
        const auto& column = prepared.persisted.columns[ordinal];
        const auto type_uuid = Rcp079DescriptorField(
            column.value_descriptor.encoded_descriptor, "type_uuid");
        if (expression->bound_name_uuid !=
                std::optional<std::string>(column.column_uuid.canonical) ||
            outputs[ordinal]->output_name_utf8 != column.canonical_name_key ||
            descriptor->descriptor_uuid !=
                column.value_descriptor.descriptor_uuid.canonical ||
            column.ordinal != ordinal ||
            column.value_descriptor.descriptor_kind !=
                "canonical_type_descriptor" ||
            column.value_descriptor.canonical_type_name.empty() ||
            column.value_descriptor.encoded_descriptor.empty() ||
            !type_uuid.has_value() || *type_uuid != descriptor->type_uuid ||
            descriptor->nullability !=
                (column.nullable ? api::RelationalNullability::kNullable
                                 : api::RelationalNullability::kNonNull)) {
          return refuse("SB_MODEL_TYPED_EXCHANGE_INVALID_V1",
                        "bounded persisted source descriptor was substituted");
        }
        auto engine_descriptor = column.value_descriptor;
        engine_descriptor.descriptor_kind = "scalar";
        prepared.columns.push_back(
            {column.canonical_name_key, std::move(engine_descriptor),
             column.nullable, descriptor->descriptor_id});
      }
      prepared.output_descriptor_uuids.push_back(descriptor->descriptor_uuid);
    }
    if (prepared.family_id == "spatial") {
      if (prepared.persisted.columns.size() != 3) {
        return refuse("SB_MODEL_SPATIAL_CRS_BINDING_REQUIRED_V1",
                      "bounded composition spatial descriptor lacks its exact column inventory");
      }
      const auto crs = Rcp079DescriptorField(
          prepared.persisted.columns[1].value_descriptor.encoded_descriptor,
          "crs_uuid");
      const auto crs_generation = Rcp079DescriptorField(
          prepared.persisted.columns[1].value_descriptor.encoded_descriptor,
          "crs_generation");
      if (!crs.has_value() || !crs_generation.has_value() ||
          !CanonicalUuidText(*crs)) {
        return refuse("SB_MODEL_SPATIAL_CRS_BINDING_REQUIRED_V1",
                      "bounded composition spatial descriptor lacks CRS authority");
      }
      const auto parsed = std::from_chars(
          crs_generation->data(),
          crs_generation->data() + crs_generation->size(),
          prepared.spatial_crs_generation);
      if (parsed.ec != std::errc{} ||
          parsed.ptr != crs_generation->data() + crs_generation->size() ||
          prepared.spatial_crs_generation == 0) {
        return refuse("SB_MODEL_SPATIAL_CRS_BINDING_REQUIRED_V1",
                      "bounded composition spatial CRS generation is invalid");
      }
      prepared.spatial_crs_uuid = *crs;
    }
    prepared_sources.push_back(std::move(prepared));
  }

  const auto exact_names = [&](const std::size_t ordinal,
                               const std::vector<std::string_view>& names) {
    const auto& columns = prepared_sources[ordinal].columns;
    if (columns.size() != names.size()) return false;
    for (std::size_t index = 0; index < names.size(); ++index) {
      if (columns[index].stable_name != names[index]) return false;
    }
    return true;
  };
  for (std::size_t ordinal = 0; ordinal < prepared_sources.size(); ++ordinal) {
    const auto& family = prepared_sources[ordinal].family_id;
    if ((family == "key_value" &&
         !exact_names(ordinal, {"row_uuid", "key", "value"})) ||
        (family == "time_series" &&
         !exact_names(ordinal, {"row_uuid", "series_uuid", "metric_uuid",
                                "point_timestamp", "tags", "value"})) ||
        (family == "vector" &&
         !exact_names(ordinal, {"row_uuid", "distance", "score"})) ||
        (family == "search" &&
         !exact_names(ordinal, {"document_uuid", "analyzer_uuid",
                                "analyzer_generation", "score", "rank"})) ||
        (family == "spatial" &&
         !exact_names(ordinal,
                      {"row_uuid", "spatial_value", "crs_uuid"}))) {
      return refuse("SB_MODEL_RESULT_DESCRIPTOR_DEMAND_INVALID_V1",
                    "signed composition model source public inventory is not exact");
    }
  }

  std::vector<Rcp079CapturedModelLegV1> captured_sources(sources.size());
  for (std::size_t ordinal = 0; ordinal < sources.size(); ++ordinal) {
    if (families[ordinal] == "relational") continue;
    CanonicalObjectFreeValuesExecutionResult attempted;
    if (!CaptureRcp079ModelSourceLeg(input, *sources[ordinal],
                                     &captured_sources[ordinal], &attempted)) {
      return refuse(attempted.api_result.diagnostics.empty()
                        ? "SB_MODEL_COORDINATOR_LEG_FAILED_V1"
                        : attempted.api_result.diagnostics.front().code,
                    attempted.api_result.diagnostics.empty()
                        ? "bounded composition family provider was not captured"
                        : attempted.api_result.diagnostics.front().detail);
    }
    auto& captured = captured_sources[ordinal];
    auto expected_captured_mga = mga;
    if (families[ordinal] == "document" || families[ordinal] == "graph") {
      expected_captured_mga.statement_timestamp.clear();
    }
    if (captured.family_id != families[ordinal] ||
        captured.logical_node_id != sources[ordinal]->node_id ||
        captured.execution_request.input.operation_id !=
            Rcp080OperationV1(families[ordinal]) ||
        captured.execution_request.input.output_descriptor_ids !=
            sources[ordinal]->output_descriptor_ids ||
        !exec::PhysicalMgaStatementContextEqual(
            captured.execution_request.input.mga_statement_context,
            expected_captured_mga) ||
        !exec::PhysicalMgaStatementContextEqual(
            captured.execution_request.current_mga_statement_context,
            expected_captured_mga)) {
      return refuse("SB_MODEL_MGA_CONTEXT_MISMATCH_V1",
                    "bounded composition captured provider authority diverged");
    }
    // Rebind both provider-facing contexts to the exact one full-composition
    // authority after equality has been proven.  This never derives MGA from
    // the operator-local typed-DAG view.
    captured.execution_request.input.mga_statement_context = mga;
    captured.execution_request.current_mga_statement_context = mga;
  }

  opt::ModelFamilyDependencyCoordinatorRequestV1 coordination;
  coordination.composition_profile_id = composition_profile_id;
  coordination.bound_sblr_tree_uuid = dag.bound_sblr_tree_uuid;
  coordination.selected_plan_generation = selected_plan_generation;
  coordination.current_selected_plan_generation = selected_plan_generation;
  coordination.statement_memory_budget_bytes = statement_memory;
  coordination.backpressure_high_watermark_rows = 64;
  coordination.backpressure_low_watermark_rows = 32;
  coordination.signed_short_circuit_enabled = true;
  coordination.feedback_observation_frozen = true;
  coordination.feedback_target_is_later_plan = true;
  coordination.feedback_observation_generation = selected_plan_generation;
  coordination.feedback_target_plan_generation = selected_plan_generation + 1;

  for (std::size_t ordinal = 0; ordinal < prepared_sources.size(); ++ordinal) {
    auto& prepared = prepared_sources[ordinal];
    const auto suffix = std::to_string(ordinal);
    const auto& captured = captured_sources[ordinal];
    const auto alternative_uuid =
        prepared.family_id == "relational"
            ? DerivedCanonicalUuid(identity_scope,
                                   "rcp080.alternative." + suffix)
            : captured.execution_request.input.selected_alternative_uuid;
    const auto provider_uuid =
        prepared.family_id == "relational"
            ? DerivedCanonicalUuid(identity_scope, "rcp080.provider." + suffix)
            : captured.execution_request.input.provider_uuid;
    const auto capability_uuid =
        prepared.family_id == "relational"
            ? DerivedCanonicalUuid(identity_scope,
                                   "rcp080.capability." + suffix)
            : captured.capability_uuid;
    const auto cost_uuid = DerivedCanonicalUuid(
        identity_scope, "rcp080.cost." + suffix);
    const auto provenance_uuid = DerivedCanonicalUuid(
        identity_scope, "rcp080.cost-provenance." + suffix);
    auto selected_alternative_uuid = alternative_uuid;
    std::string selected_plan_uuid = DerivedCanonicalUuid(
        identity_scope, "rcp080.leg-plan." + suffix);
    auto selected_inventory_receipt_uuid = DerivedCanonicalUuid(
        identity_scope, "rcp080.inventory." + suffix);
    opt::ModelFamilyCostVectorV1 selected_family_cost;
    selected_family_cost.cost_vector_uuid = cost_uuid;
    selected_family_cost.provenance_uuid = provenance_uuid;
    selected_family_cost.provenance_generation = generation;
    selected_family_cost.confidence_basis_points = 10'000;
    selected_family_cost.startup_units = 1;
    selected_family_cost.cpu_units = 1;
    selected_family_cost.sequential_read_units = 1;
    selected_family_cost.memory_bytes_required = leg_memory;
    bool exact_fallback_selected = true;
    if (prepared.family_id != "relational") {
      opt::ModelFamilyCoordinatorRequestV1 family_request;
      family_request.family_id = prepared.family_id;
      family_request.operation_ids = prepared.operation_ids;
      family_request.operation_id = prepared.operation_id;
      family_request.logical_operator_id =
          Rcp080LogicalOperatorV1(prepared.family_id);
      family_request.composition_profile_id = composition_profile_id;
      family_request.composition_lexical_source_ordinal =
          static_cast<std::uint16_t>(ordinal);
      family_request.composition_arity =
          static_cast<std::uint16_t>(sources.size());
      family_request.logical_node_id = prepared.node->node_id;
      family_request.object_uuid = prepared.persisted.relation_uuid.canonical;
      family_request.output_descriptor_ids =
          prepared.node->output_descriptor_ids;
      family_request.mga_statement_context = mga;
      family_request.bound_sblr_tree_uuid = dag.bound_sblr_tree_uuid;
      family_request.catalog_epoch_uuid =
          input.context.catalog_epoch_uuid.canonical;
      family_request.security_context_uuid =
          input.context.authorization_context.authority_uuid.canonical;
      family_request.capability_snapshot_uuid =
          input.context.optimizer_capability_snapshot_uuid.canonical;
      family_request.resource_snapshot_uuid =
          input.context.optimizer_resource_snapshot_uuid.canonical;
      family_request.statistics_snapshot_uuid = DerivedCanonicalUuid(
          identity_scope, "rcp080.statistics." + suffix);
      family_request.route_snapshot_uuid =
          input.context.optimizer_route_snapshot_uuid.canonical;
      family_request.catalog_generation = generation;
      family_request.current_catalog_generation = generation;
      family_request.security_epoch = security_generation;
      family_request.policy_epoch = policy_generation;
      family_request.resource_epoch = resource_generation;
      family_request.statistics_generation = generation;
      family_request.route_epoch = input.context.optimizer_route_epoch;
      family_request.route_generation =
          input.context.optimizer_route_generation;
      family_request.memory_budget_bytes = leg_memory;
      family_request.security_admitted = true;
      std::vector<opt::ModelFamilyCapabilitySnapshotV1> alternatives;
      alternatives.push_back(MakeModelFamilyCapabilitySnapshotForCompositionV1(
          family_request, identity_scope + ".rcp080." + suffix + ".fallback",
          opt::ModelFamilyAlternativeRouteClassV1::kExactCollectionFallback,
          provider_uuid, capability_uuid,
          captured.execution_request.input.provider_generation, true, 1, 1,
          leg_memory));
      const auto selected = PlanCanonicalModelFamilySourceForCompositionV1(
          family_request,
          identity_scope + ".rcp080." + suffix + ".inventory",
          std::move(alternatives));
      if (!selected.accepted || !selected.selected ||
          !selected.data_access_allowed ||
          !selected.optimizer_owned_enumeration ||
          !selected.exact_fallback_selected ||
          selected.selected_candidate.provider_uuid != provider_uuid ||
          selected.selected_candidate.capability_uuid != capability_uuid) {
        return refuse(selected.diagnostic_id.empty()
                          ? "SB_MODEL_CANDIDATE_SEMANTICS_MISSING_V1"
                          : selected.diagnostic_id,
                      selected.detail.empty()
                          ? "family-local exact collection fallback was not selected"
                          : selected.detail);
      }
      selected_alternative_uuid =
          selected.selected_candidate.alternative_uuid;
      selected_plan_uuid = selected.physical_dag.selected_plan_uuid;
      selected_inventory_receipt_uuid =
          selected.candidate_inventory_receipt_uuid;
      selected_family_cost = selected.selected_candidate.cost;
      exact_fallback_selected = selected.exact_fallback_selected;
    }
    if (prepared.family_id == "relational") {
      selected_family_cost.property_snapshot_uuid = DerivedCanonicalUuid(
          identity_scope, "rcp080.cost-property." + suffix);
      selected_family_cost.calibration_profile_uuid = DerivedCanonicalUuid(
          identity_scope, "rcp080.cost-calibration." + suffix);
      selected_family_cost.scalarization_policy_id =
          "model-family.complete-unit-sum-minus-cache-benefit.v1";
      selected_family_cost.memory_grant_units = leg_memory;
      selected_family_cost.memory_allocation_units = leg_memory;
      selected_family_cost.memory_grant_opportunity_units = leg_memory;
      selected_family_cost.mga_units = 1;
      selected_family_cost.mga_visibility_check_units = 1;
      selected_family_cost.complete_dimension_vector = true;
      const auto scalar_score =
          opt::ScalarizeModelFamilyCostVectorV1(selected_family_cost);
      if (!scalar_score.has_value()) {
        return refuse("QOW-DIAG-OPTIMIZER-COST-VECTOR-COMPLETE-V1",
                      "relational composition source cost overflowed");
      }
      selected_family_cost.scalar_score = *scalar_score;
    }

    opt::ModelFamilyDependencyLegV1 leg;
    leg.lexical_source_ordinal = static_cast<std::uint16_t>(ordinal);
    leg.physical_node_uuid = DerivedCanonicalUuid(
        identity_scope, "rcp080.physical-source." + suffix);
    leg.family_id = prepared.family_id;
    leg.operation_ids = prepared.operation_ids;
    leg.operation_id = prepared.operation_id;
    leg.selected_plan_uuid = selected_plan_uuid;
    leg.selected_alternative_uuid = selected_alternative_uuid;
    leg.provider_uuid = provider_uuid;
    leg.capability_uuid = capability_uuid;
    leg.delivered_property_uuid = DerivedCanonicalUuid(
        identity_scope, "rcp080.property." + suffix);
    leg.bound_object_uuid = prepared.persisted.relation_uuid.canonical;
    leg.catalog_snapshot_uuid = leg.current_catalog_snapshot_uuid =
        input.context.catalog_epoch_uuid.canonical;
    leg.descriptor_snapshot_uuid = leg.current_descriptor_snapshot_uuid =
        prepared.persisted.descriptor_uuid.canonical;
    leg.security_context_uuid = leg.current_security_context_uuid =
        input.context.authorization_context.authority_uuid.canonical;
    leg.policy_snapshot_uuid = leg.current_policy_snapshot_uuid =
        DerivedCanonicalUuid(identity_scope, "rcp080.policy." + suffix);
    leg.resource_contract_uuid = leg.current_resource_contract_uuid =
        DerivedCanonicalUuid(identity_scope, "rcp080.resource." + suffix);
    leg.operation_scope_receipt_uuid = DerivedCanonicalUuid(
        identity_scope, "rcp080.operation-scope." + suffix);
    leg.selected_alternative_receipt_uuid = DerivedCanonicalUuid(
        identity_scope, "rcp080.selection." + suffix);
    leg.root_physical_node_id = prepared.node->node_id;
    leg.output_descriptor_ids = prepared.node->output_descriptor_ids;
    leg.output_descriptor_uuids = prepared.output_descriptor_uuids;
    leg.family_local_cost = selected_family_cost;
    opt::ModelFamilyDependencyAlternativeV1 candidate;
    candidate.alternative_uuid = selected_alternative_uuid;
    candidate.candidate_inventory_receipt_uuid =
        selected_inventory_receipt_uuid;
    candidate.implementation_id =
        prepared.family_id == "relational"
            ? Rcp080ImplementationV1(prepared.family_id)
            : captured.implementation_id;
    candidate.operation_ids = prepared.operation_ids;
    candidate.operation_id = prepared.operation_id;
    candidate.operation_scope_receipt_uuid = leg.operation_scope_receipt_uuid;
    candidate.selection_policy_receipt_uuid =
        leg.selected_alternative_receipt_uuid;
    candidate.authority_approved_comparison_rank = 1;
    candidate.family_local_cost = leg.family_local_cost;
    candidate.available = true;
    candidate.exact = true;
    candidate.exact_fallback = exact_fallback_selected;
    candidate.admitted = true;
    leg.candidate_alternatives = {candidate};
    leg.mga_statement_context = mga;
    leg.catalog_generation = leg.current_catalog_generation = generation;
    leg.descriptor_generation = leg.current_descriptor_generation =
        prepared.persisted.descriptor_generation;
    leg.security_generation = leg.current_security_generation =
        security_generation;
    leg.policy_generation = leg.current_policy_generation = policy_generation;
    leg.resource_generation = leg.current_resource_generation =
        resource_generation;
    leg.provider_generation = leg.current_provider_generation =
        prepared.family_id == "relational"
            ? prepared.persisted.descriptor_generation
            : captured.execution_request.input.provider_generation;
    leg.capability_generation = leg.current_capability_generation =
        prepared.family_id == "relational"
            ? prepared.persisted.descriptor_generation
            // Standalone family providers currently version their engine-issued
            // capability in the provider-generation cohort.  Freeze that exact
            // captured cohort for the stricter composition capability gate.
            : captured.execution_request.input.provider_generation;
    leg.memory_grant_bytes = leg_memory;
    leg.exchange_buffer_bytes = exchange_memory;
    leg.maximum_rows = maximum_rows;
    leg.maximum_columns = prepared.columns.size();
    if (!CheckedMultiply(maximum_rows, prepared.columns.size(),
                         &leg.maximum_cells)) {
      return refuse("SB_MODEL_RESOURCE_ROW_LIMIT_V1",
                    "bounded composition leg cell limit overflowed");
    }
    leg.selected = true;
    leg.security_admitted = true;
    leg.capability_admitted = true;
    leg.capability_abi_version = 1;
    leg.exact = true;
    leg.exact_fallback_selected = exact_fallback_selected;
    leg.exact_fallback_available = true;
    leg.cleanup_supported = true;
    leg.cancellation_supported = true;
    leg.parallel_eligible = composition_profile_id == "COMP-4-MIXED-V1";
    leg.spill_eligible = input.context.optimizer_spill_allowed;
    coordination.legs.push_back(std::move(leg));
  }

  std::vector<std::pair<std::uint16_t, std::uint16_t>> dependency_pairs;
  if (composition_profile_id == "COMP-4-MIXED-V1") {
    dependency_pairs = {{0, 1}, {1, 2}, {0, 3}};
  } else {
    for (std::uint16_t ordinal = 0; ordinal + 1 < sources.size(); ++ordinal) {
      dependency_pairs.emplace_back(ordinal, ordinal + 1);
    }
  }
  for (std::size_t ordinal = 0; ordinal < dependency_pairs.size(); ++ordinal) {
    const auto [producer, consumer] = dependency_pairs[ordinal];
    opt::ModelFamilyDependencyEdgeV1 edge;
    edge.edge_uuid = DerivedCanonicalUuid(
        identity_scope, "rcp080.edge." + std::to_string(ordinal));
    edge.producer_lexical_source_ordinal = producer;
    edge.consumer_lexical_source_ordinal = consumer;
    edge.required_property_uuid =
        coordination.legs[producer].delivered_property_uuid;
    edge.delivered_property_uuid = edge.required_property_uuid;
    edge.descriptor_lineage_uuid = DerivedCanonicalUuid(
        identity_scope, "rcp080.lineage." + std::to_string(ordinal));
    edge.producer_output_descriptor_ids =
        coordination.legs[producer].output_descriptor_ids;
    edge.consumer_input_descriptor_ids = edge.producer_output_descriptor_ids;
    edge.producer_output_descriptor_uuids =
        coordination.legs[producer].output_descriptor_uuids;
    edge.consumer_input_descriptor_uuids =
        edge.producer_output_descriptor_uuids;
    coordination.edges.push_back(std::move(edge));
  }

  std::vector<std::uint32_t> consumer_descriptors =
      coordination.legs.front().output_descriptor_ids;
  std::vector<std::string> consumer_descriptor_uuids =
      coordination.legs.front().output_descriptor_uuids;
  std::string consumer_left_uuid = coordination.legs.front().physical_node_uuid;
  for (std::size_t ordinal = 1; ordinal < sources.size(); ++ordinal) {
    consumer_descriptors.insert(
        consumer_descriptors.end(),
        coordination.legs[ordinal].output_descriptor_ids.begin(),
        coordination.legs[ordinal].output_descriptor_ids.end());
    consumer_descriptor_uuids.insert(
        consumer_descriptor_uuids.end(),
        coordination.legs[ordinal].output_descriptor_uuids.begin(),
        coordination.legs[ordinal].output_descriptor_uuids.end());
    opt::ModelFamilyRelationalConsumerV1 consumer;
    consumer.physical_node_uuid = DerivedCanonicalUuid(
        identity_scope, "rcp080.consumer." + std::to_string(ordinal));
    consumer.physical_node_id = joins[ordinal - 1]->node_id;
    consumer.causal_counter_id = sources.size() + ordinal;
    consumer.selected_implementation_uuid = DerivedCanonicalUuid(
        identity_scope,
        "rcp080.consumer-implementation." + std::to_string(ordinal));
    consumer.expected_security_receipt_uuid = DerivedCanonicalUuid(
        identity_scope,
        "rcp080.consumer-security." + std::to_string(ordinal));
    consumer.join_form_id = "CROSS";
    consumer.input_physical_node_uuids = {
        consumer_left_uuid, coordination.legs[ordinal].physical_node_uuid};
    consumer.input_descriptor_ids = consumer_descriptors;
    consumer.output_descriptor_ids = consumer_descriptors;
    consumer.input_descriptor_uuids = consumer_descriptor_uuids;
    consumer.output_descriptor_uuids = consumer_descriptor_uuids;
    consumer.mga_statement_context = mga;
    consumer.maximum_rows = maximum_rows;
    consumer.maximum_columns = consumer_descriptors.size();
    if (!CheckedMultiply(maximum_rows, consumer_descriptors.size(),
                         &consumer.maximum_cells)) {
      return refuse("SB_MODEL_RESOURCE_ROW_LIMIT_V1",
                    "bounded composition consumer cell limit overflowed");
    }
    consumer.memory_grant_bytes = retained_grant;
    consumer.canonical_root = ordinal + 1 == sources.size();
    consumer.exact = true;
    consumer.cleanup_supported = true;
    consumer.cancellation_supported = true;
    consumer_left_uuid = consumer.physical_node_uuid;
    coordination.relational_consumers.push_back(std::move(consumer));
  }
  coordination.canonical_root_physical_node_uuid = consumer_left_uuid;
  coordination.canonical_root_physical_node_id = joins.back()->node_id;

  const auto admitted =
      opt::CoordinateModelFamilyDependencyDagV1(coordination);
  if (!admitted.accepted || !admitted.data_access_allowed ||
      !admitted.root_publication_candidate ||
      admitted.stable_schedule.size() != sources.size() ||
      admitted.relational_consumers.size() != joins.size()) {
    return refuse(admitted.diagnostic_id.empty()
                      ? "SB_MODEL_DEPENDENCY_DAG_INVALID_V1"
                      : admitted.diagnostic_id,
                  admitted.detail.empty()
                      ? "bounded composition dependency coordinator refused the selected graph"
                      : admitted.detail);
  }
  result.optimizer_admitted = true;
  result.optimizer_selected = true;
  result.physical_dag_published = true;
  result.physical_node_count = sources.size() + joins.size();
  result.selected_plan_uuid = admitted.dependency_dag_receipt_uuid;

  // Freeze the exact coordinator-selected graph before any provider access.
  // This is the same ABI-v2 identity retained through execution evidence and
  // final durable-MGA publication; it is not reconstructed from runtime data.
  exec::TypedPhysicalNodeDag physical_dag;
  physical_dag.abi_version = 2;
  physical_dag.publication_contract_version = 1;
  physical_dag.selected_plan_uuid = result.selected_plan_uuid;
  physical_dag.root_physical_node_id = joins.back()->node_id;
  physical_dag.local_transaction_id = mga.owning_local_transaction_id;
  physical_dag.statement_snapshot_id = mga.visible_committed_high_watermark;
  physical_dag.mga_statement_context = mga;
  physical_dag.bound_sblr_tree_uuid = dag.bound_sblr_tree_uuid;
  physical_dag.catalog_epoch_uuid = input.context.catalog_epoch_uuid.canonical;
  physical_dag.security_context_uuid =
      input.context.authorization_context.authority_uuid.canonical;
  physical_dag.capability_snapshot_uuid =
      input.context.optimizer_capability_snapshot_uuid.canonical;
  physical_dag.resource_snapshot_uuid =
      input.context.optimizer_resource_snapshot_uuid.canonical;
  physical_dag.statistics_snapshot_uuid =
      DerivedCanonicalUuid(identity_scope, "rcp080.statistics-snapshot");
  physical_dag.route_snapshot_uuid =
      input.context.optimizer_route_snapshot_uuid.canonical;
  physical_dag.catalog_generation = generation;
  physical_dag.security_epoch = security_generation;
  physical_dag.policy_epoch = policy_generation;
  physical_dag.resource_epoch = resource_generation;
  physical_dag.statistics_generation = generation;
  physical_dag.route_epoch = input.context.optimizer_route_epoch;
  physical_dag.route_generation = selected_plan_generation;
  physical_dag.memory_budget_bytes = statement_memory;
  physical_dag.spill_allowed = input.context.optimizer_spill_allowed;
  physical_dag.optimizer_published = true;
  physical_dag.immutable_node_identity_validated = true;
  physical_dag.capability_validated_before_access = true;
  physical_dag.data_access_observed = false;
  physical_dag.complete_cost_vectors_retained = true;
  physical_dag.descriptor_contract_validated = true;
  physical_dag.property_contract_validated = true;
  physical_dag.dependency_contract_validated = true;
  physical_dag.resource_contract_validated = true;
  physical_dag.mga_contract_validated = true;
  physical_dag.causal_identity_validated = true;
  physical_dag.published_node_count = sources.size() + joins.size();
  physical_dag.first_causal_counter_id = 1;
  physical_dag.admission_evidence = {
      {exec::PhysicalAdmissionStage::kBoundRequest,
       physical_dag.bound_sblr_tree_uuid},
      {exec::PhysicalAdmissionStage::kCatalogEpoch,
       physical_dag.catalog_epoch_uuid},
      {exec::PhysicalAdmissionStage::kSecurity,
       physical_dag.security_context_uuid},
      {exec::PhysicalAdmissionStage::kMgaStatementBoundary,
       mga.statement_snapshot_uuid},
      {exec::PhysicalAdmissionStage::kPolicyCapability,
       physical_dag.capability_snapshot_uuid},
      {exec::PhysicalAdmissionStage::kResource,
       physical_dag.resource_snapshot_uuid},
      {exec::PhysicalAdmissionStage::kStatisticsProvenance,
       physical_dag.statistics_snapshot_uuid},
      {exec::PhysicalAdmissionStage::kCanonicalRoute,
       physical_dag.route_snapshot_uuid},
  };
  const auto retained_cost = [&](exec::PhysicalNodeRecord* node,
                                 const opt::ModelFamilyCostVectorV1& cost)
      -> std::optional<std::uint64_t> {
    opt::RetainModelFamilyCostVectorV1(cost, &node->retained_cost);
    std::uint64_t recomputed_score = 0;
    if (node->retained_cost.cost_vector_uuid != node->cost_vector_uuid ||
        node->retained_cost.memory_bytes_required !=
            node->memory_bytes_required ||
        !exec::ComputePhysicalCostVectorScalarScore(node->retained_cost,
                                                    &recomputed_score) ||
        recomputed_score != node->retained_cost.scalar_score) {
      return std::nullopt;
    }
    return recomputed_score;
  };
  for (std::size_t ordinal = 0; ordinal < sources.size(); ++ordinal) {
    const auto scheduled = std::ranges::find_if(
        admitted.stable_schedule, [&](const auto& candidate) {
          return candidate.leg.lexical_source_ordinal == ordinal;
        });
    if (scheduled == admitted.stable_schedule.end()) {
      return refuse("SB_MODEL_DEPENDENCY_DAG_INVALID_V1",
                    "coordinator schedule lost a lexical source leg");
    }
    const auto& leg = scheduled->leg;
    exec::PhysicalNodeRecord node;
    node.physical_node_id = sources[ordinal]->node_id;
    node.relational_node_id = sources[ordinal]->node_id;
    node.node_kind = exec::PhysicalNodeKind::kScan;
    node.logical_semantic_variant_id =
        leg.family_id == "relational" ? "relation.source.v1"
                                      : "sblr.model_source.v1";
    node.implementation_id =
        leg.family_id == "relational"
            ? Rcp080ImplementationV1(leg.family_id)
            : captured_sources[ordinal].implementation_id;
    node.output_descriptor_ids = sources[ordinal]->output_descriptor_ids;
    node.selected_alternative_uuid = leg.selected_alternative_uuid;
    node.executor_capability_uuid = leg.capability_uuid;
    node.executor_capability_abi_version = 1;
    node.cost_vector_uuid = leg.family_local_cost.cost_vector_uuid;
    node.delivered_property_uuids = {leg.delivered_property_uuid};
    node.memory_bytes_required = leg.memory_grant_bytes;
    node.engine_capability_validated = true;
    node.mga_statement_context = mga;
    node.publication_ordinal = physical_dag.nodes.size();
    node.causal_counter_id =
        physical_dag.first_causal_counter_id + node.publication_ordinal;
    node.transformation_uuid = DerivedCanonicalUuid(
        identity_scope,
        "rcp080.transformation.source." + std::to_string(ordinal));
    node.transformation_rule_id = "rcp080.model-source.v1";
    const auto score = retained_cost(&node, leg.family_local_cost);
    if (!score.has_value() ||
        !CheckedAdd(physical_dag.selected_scalar_score, *score,
                    &physical_dag.selected_scalar_score)) {
      return refuse("QOW-DIAG-OPTIMIZER-COST-VECTOR-COMPLETE-V1",
                    "selected physical source cost was incomplete");
    }
    physical_dag.nodes.push_back(std::move(node));
  }
  for (std::size_t ordinal = 0; ordinal < joins.size(); ++ordinal) {
    const auto& consumer = admitted.relational_consumers[ordinal];
    exec::PhysicalNodeRecord node;
    node.physical_node_id = joins[ordinal]->node_id;
    node.relational_node_id = joins[ordinal]->node_id;
    node.node_kind = exec::PhysicalNodeKind::kJoin;
    node.logical_semantic_variant_id = "join.cross.v1";
    node.implementation_id = "cross.nested-loop.bounded.v1";
    node.input_physical_node_ids = {joins[ordinal]->input_node_ids[0],
                                    joins[ordinal]->input_node_ids[1]};
    node.output_descriptor_ids = joins[ordinal]->output_descriptor_ids;
    node.selected_alternative_uuid = consumer.selected_implementation_uuid;
    node.executor_capability_uuid = DerivedCanonicalUuid(
        identity_scope,
        "rcp080.consumer-capability." + std::to_string(ordinal));
    node.executor_capability_abi_version = 1;
    node.cost_vector_uuid = DerivedCanonicalUuid(
        identity_scope,
        "rcp080.consumer-cost." + std::to_string(ordinal));
    node.memory_bytes_required = consumer.memory_grant_bytes;
    node.engine_capability_validated = true;
    node.mga_statement_context = mga;
    node.publication_ordinal = physical_dag.nodes.size();
    node.causal_counter_id =
        physical_dag.first_causal_counter_id + node.publication_ordinal;
    node.transformation_uuid = DerivedCanonicalUuid(
        identity_scope,
        "rcp080.transformation.consumer." + std::to_string(ordinal));
    node.transformation_rule_id = "rcp080.cross-consumer.v1";
    opt::ModelFamilyCostVectorV1 consumer_cost;
    consumer_cost.cost_vector_uuid = node.cost_vector_uuid;
    consumer_cost.provenance_uuid = DerivedCanonicalUuid(
        identity_scope,
        "rcp080.consumer-cost-provenance." + std::to_string(ordinal));
    consumer_cost.property_snapshot_uuid = DerivedCanonicalUuid(
        identity_scope,
        "rcp080.consumer-cost-property." + std::to_string(ordinal));
    consumer_cost.calibration_profile_uuid = DerivedCanonicalUuid(
        identity_scope,
        "rcp080.consumer-cost-calibration." + std::to_string(ordinal));
    consumer_cost.scalarization_policy_id =
        "model-family.complete-unit-sum-minus-cache-benefit.v1";
    consumer_cost.provenance_generation = generation;
    consumer_cost.confidence_basis_points = 10'000;
    consumer_cost.cpu_units = 1;
    consumer_cost.memory_bytes_required = consumer.memory_grant_bytes;
    consumer_cost.memory_grant_units = consumer.memory_grant_bytes;
    consumer_cost.memory_allocation_units = consumer.memory_grant_bytes;
    consumer_cost.memory_grant_opportunity_units =
        consumer.memory_grant_bytes;
    consumer_cost.complete_dimension_vector = true;
    const auto consumer_scalar =
        opt::ScalarizeModelFamilyCostVectorV1(consumer_cost);
    if (!consumer_scalar.has_value()) {
      return refuse("QOW-DIAG-OPTIMIZER-COST-VECTOR-COMPLETE-V1",
                    "selected physical consumer cost overflowed");
    }
    consumer_cost.scalar_score = *consumer_scalar;
    const auto score = retained_cost(&node, consumer_cost);
    if (!score.has_value() ||
        !CheckedAdd(physical_dag.selected_scalar_score, *score,
                    &physical_dag.selected_scalar_score)) {
      return refuse("QOW-DIAG-OPTIMIZER-COST-VECTOR-COMPLETE-V1",
                    "selected physical consumer cost was incomplete");
    }
    physical_dag.nodes.push_back(std::move(node));
  }
  std::vector<const exec::PhysicalNodeRecord*> signature_nodes;
  for (const auto& node : physical_dag.nodes) signature_nodes.push_back(&node);
  std::ranges::sort(signature_nodes, {},
                    &exec::PhysicalNodeRecord::relational_node_id);
  for (const auto* node : signature_nodes) {
    physical_dag.selected_plan_signature +=
        std::to_string(node->relational_node_id) + "=" +
        node->selected_alternative_uuid + ";";
  }
  const auto physical_validation = exec::ValidateTypedPhysicalNodeDag(
      physical_dag, {physical_dag.nodes.size(), 32, 2});
  if (!physical_validation.accepted) {
    return refuse(physical_validation.issues.empty()
                      ? "QOW-DIAG-PHYSICAL-NODE-ABI-PUBLICATION"
                      : physical_validation.issues.front().diagnostic_id,
                  "bounded composition selected physical DAG is invalid");
  }

  exec::ModelFamilyCompositionExecutionRequestV1 execution_request;
  execution_request.admitted_plan = admitted;
  execution_request.engine_mga_inventory_guard_owned_by_caller = true;
  const auto cancellation_requested =
      input.context.query_cancellation_requested
          ? input.context.query_cancellation_requested
          : std::function<bool()>([] { return false; });
  for (std::size_t ordinal = 0; ordinal < prepared_sources.size(); ++ordinal) {
    const auto& prepared = prepared_sources[ordinal];
    const auto scheduled = std::ranges::find_if(
        admitted.stable_schedule, [&](const auto& candidate) {
          return candidate.leg.lexical_source_ordinal == ordinal;
        });
    if (scheduled == admitted.stable_schedule.end()) {
      return refuse("SB_MODEL_DEPENDENCY_DAG_INVALID_V1",
                    "execution schedule lost a lexical source leg");
    }
    const auto& leg = scheduled->leg;
    exec::ModelFamilyCompositionExecutionLegV1 execution_leg;
    execution_leg.lexical_source_ordinal =
        static_cast<std::uint16_t>(ordinal);
    auto& request = execution_leg.execution;
    if (leg.family_id != "relational") {
      request = captured_sources[ordinal].execution_request;
    }
    request.input.family_id = leg.family_id;
    request.input.operation_ids = leg.operation_ids;
    request.input.operation_id = leg.operation_id;
    request.input.object_uuid = leg.bound_object_uuid;
    request.input.physical_node_id = leg.root_physical_node_id;
    request.input.selected_alternative_uuid = leg.selected_alternative_uuid;
    request.input.capability_uuid = leg.capability_uuid;
    request.input.provider_uuid = leg.provider_uuid;
    request.input.provider_generation = leg.provider_generation;
    request.input.result_handle_uuid = DerivedCanonicalUuid(
        identity_scope, "rcp080.result-handle." + std::to_string(ordinal));
    request.input.causal_counter_id = scheduled->causal_counter_id;
    request.input.output_descriptor_ids = leg.output_descriptor_ids;
    request.input.mga_statement_context = mga;
    request.input.catalog_epoch_uuid = leg.catalog_snapshot_uuid;
    request.input.security_context_uuid = leg.security_context_uuid;
    request.input.policy_snapshot_uuid = leg.policy_snapshot_uuid;
    request.input.resource_contract_uuid = leg.resource_contract_uuid;
    request.input.catalog_generation = leg.catalog_generation;
    request.input.descriptor_generation = leg.descriptor_generation;
    request.input.security_generation = leg.security_generation;
    request.input.policy_generation = leg.policy_generation;
    request.input.resource_generation = leg.resource_generation;
    request.input.maximum_rows = leg.maximum_rows;
    request.input.maximum_cells = leg.maximum_cells;
    request.input.maximum_memory_bytes = leg.memory_grant_bytes;
    request.input.exact_fallback_selected = leg.exact_fallback_selected;
    request.input.multimodel_composition_receipt_uuid =
        admitted.composition_admission_receipt_uuid;
    request.input.multimodel_lexical_source_ordinal =
        static_cast<std::uint16_t>(ordinal);
    request.input.multimodel_composition_arity =
        static_cast<std::uint16_t>(sources.size());
    request.input.multimodel_common_statement_context = true;
    if (leg.family_id == "spatial") {
      request.input.spatial_geometry_descriptor_uuid =
          prepared.columns[1].descriptor.descriptor_uuid.canonical;
      const auto spatial_descriptor = descriptor_for(
          prepared.node->output_descriptor_ids[1]);
      request.input.spatial_geometry_type_uuid =
          spatial_descriptor->type_uuid;
      request.input.spatial_crs_uuid = prepared.spatial_crs_uuid;
      request.input.spatial_crs_generation = prepared.spatial_crs_generation;
    }
    request.capability.capability_uuid = leg.capability_uuid;
    request.capability.family_id = leg.family_id;
    request.capability.provider_uuid = leg.provider_uuid;
    request.capability.provider_generation = leg.provider_generation;
    request.capability.capability_generation = leg.capability_generation;
    request.capability.available = true;
    request.capability.exact = true;
    request.capability.exact_collection_fallback_available = true;
    request.capability.cancellation_supported = true;
    request.capability.cleanup_supported = true;
    request.capability.residual_recheck_supported = true;
    request.capability.base_row_mga_recheck_supported = true;
    request.capability.security_recheck_supported = true;
    request.cancellation_requested = cancellation_requested;
    request.cleanup_provider = [] {};
    request.exact_fallback_selected = leg.exact_fallback_selected;
    request.security_admitted = true;
    request.current_catalog_generation = leg.current_catalog_generation;
    request.current_descriptor_generation = leg.current_descriptor_generation;
    request.current_security_generation = leg.current_security_generation;
    request.current_policy_generation = leg.current_policy_generation;
    request.current_resource_generation = leg.current_resource_generation;
    request.current_provider_generation = leg.current_provider_generation;
    request.current_capability_generation = leg.current_capability_generation;
    request.current_mga_statement_context = mga;
    const auto context = input.context;
    const auto prepared_copy = prepared;
    const auto delivered_property_uuid = leg.delivered_property_uuid;
    const auto security_receipt_uuid = DerivedCanonicalUuid(
        identity_scope, "rcp080.provider-security." + std::to_string(ordinal));
    if (leg.family_id != "relational") {
      const auto provider =
          captured_sources[ordinal].execution_request.execute_provider;
      const auto runtime_input = request.input;
      request.execute_provider =
          [provider, runtime_input, delivered_property_uuid](
              const exec::ModelSourceInputDescriptorV1& input) mutable {
            auto produced = provider(input);
            if (produced.ok) {
              auto& batch = produced.provider_batch;
              batch.selected_alternative_uuid =
                  runtime_input.selected_alternative_uuid;
              batch.capability_uuid = runtime_input.capability_uuid;
              batch.exact_fallback_selected =
                  runtime_input.exact_fallback_selected;
              batch.result_handle_uuid = runtime_input.result_handle_uuid;
              batch.causal_counter_id = runtime_input.causal_counter_id;
              batch.output_descriptor_ids =
                  runtime_input.output_descriptor_ids;
              batch.mga_statement_context =
                  runtime_input.mga_statement_context;
              batch.multimodel_composition_receipt_uuid =
                  runtime_input.multimodel_composition_receipt_uuid;
              batch.multimodel_lexical_source_ordinal =
                  runtime_input.multimodel_lexical_source_ordinal;
              batch.multimodel_composition_arity =
                  runtime_input.multimodel_composition_arity;
              batch.multimodel_common_statement_context =
                  runtime_input.multimodel_common_statement_context;
              batch.properties.property_uuid = delivered_property_uuid;
            }
            return produced;
          };
    } else {
      request.execute_provider =
        [context, prepared_copy, delivered_property_uuid,
         security_receipt_uuid, cancellation_requested](
            const exec::ModelSourceInputDescriptorV1& source_input) mutable {
          exec::ModelProviderExecutionResultV1 provider;
          const auto fail = [&](std::string diagnostic, std::string detail) {
            provider.diagnostic_id = std::move(diagnostic);
            provider.detail = std::move(detail);
            return provider;
          };
          const auto authorization = api::EvaluateMaterializedAuthorization(
              context, context.authorization_context, "SELECT",
              source_input.object_uuid);
          if (!authorization.authorized || authorization.denied ||
              authorization.policy_recheck_required ||
              !authorization.diagnostics.empty()) {
            return fail("SB_MODEL_SECURITY_ADMISSION_REFUSED_V1",
                        "composition provider authorization changed");
          }
          const auto current = api::LoadMgaRelationStorageDescriptor(
              context, source_input.object_uuid);
          if (!current.ok ||
              current.descriptor.descriptor_uuid.canonical !=
                  prepared_copy.persisted.descriptor_uuid.canonical ||
              current.descriptor.descriptor_generation !=
                  prepared_copy.persisted.descriptor_generation) {
            return fail("SB_MODEL_CATALOG_GENERATION_STALE_V1",
                        "composition provider relation descriptor changed");
          }
          api::MgaVisibleHeapRelationReadRequest read_request;
          read_request.relation_uuid = source_input.object_uuid;
          read_request.maximum_scanned_row_versions =
              context.optimizer_maximum_search_steps;
          read_request.maximum_decoded_bytes =
              source_input.maximum_memory_bytes / 2;
          read_request.maximum_output_rows = source_input.maximum_rows;
          read_request.cancellation_requested = cancellation_requested;
          const auto read = api::ReadVisibleMgaHeapRelation(context,
                                                            read_request);
          provider.data_access_observed = true;
          provider.rows_examined = read.scanned_row_version_count;
          if (!read.ok) {
            return fail(read.diagnostic.code.empty()
                            ? "SB_MODEL_MGA_CONTEXT_MISMATCH_V1"
                            : read.diagnostic.code,
                        read.diagnostic.detail.empty()
                            ? "composition MGA-visible relation read failed"
                            : read.diagnostic.detail);
          }
          auto& output = provider.provider_batch;
          output.provider_uuid = source_input.provider_uuid;
          output.provider_generation = source_input.provider_generation;
          output.selected_alternative_uuid =
              source_input.selected_alternative_uuid;
          output.capability_uuid = source_input.capability_uuid;
          output.exact_fallback_selected =
              source_input.exact_fallback_selected;
          output.result_handle_uuid = source_input.result_handle_uuid;
          output.causal_counter_id = source_input.causal_counter_id;
          output.output_descriptor_ids = source_input.output_descriptor_ids;
          output.batch.columns = prepared_copy.columns;
          output.properties.property_uuid = delivered_property_uuid;
          output.properties.partitioning_id = "single_local_partition";
          output.properties.uniqueness_id =
              source_input.family_id == "document" ? "document_uuid"
              : source_input.family_id == "graph" ? "path_uuid"
              : source_input.family_id == "key_value" ? "key"
              : source_input.family_id == "search" ? "document_uuid"
                                                    : "row_uuid";
          output.properties.ordering_id =
              source_input.family_id == "key_value"
                  ? "key_value_unordered_v1"
              : source_input.family_id == "time_series"
                  ? "series_metric_timestamp_tags_row_ascending_v1"
              : source_input.family_id == "vector"
                  ? "vector_distance_row_uuid_ascending_v1"
              : source_input.family_id == "search"
                  ? "search_score_desc_document_uuid_asc_v1"
                  : "fixture_order";
          output.mga_statement_context = source_input.mga_statement_context;
          output.security_receipt_uuid = security_receipt_uuid;
          output.multimodel_composition_receipt_uuid =
              source_input.multimodel_composition_receipt_uuid;
          output.multimodel_lexical_source_ordinal =
              source_input.multimodel_lexical_source_ordinal;
          output.multimodel_composition_arity =
              source_input.multimodel_composition_arity;
          output.multimodel_common_statement_context = true;
          output.properties.exact = true;
          output.properties.residual_recheck_complete = true;
          output.properties.base_row_mga_recheck_complete = true;
          output.properties.security_recheck_complete = true;
          output.residual_recheck_complete = true;
          output.base_row_mga_recheck_complete = true;
          output.security_recheck_complete = true;
          const auto value_for = [](const api::CrudRowVersionRecord& row,
                                    const std::string_view name)
              -> const std::string* {
            const std::string* value = nullptr;
            for (const auto& [field, candidate] : row.values) {
              if (field != name) continue;
              if (value != nullptr) return nullptr;
              value = &candidate;
            }
            return value;
          };
          for (std::size_t row_ordinal = 0;
               row_ordinal < read.visible_rows.size(); ++row_ordinal) {
            const auto& row = read.visible_rows[row_ordinal];
            exec::DescriptorTuple tuple;
            for (const auto& column : prepared_copy.columns) {
              const auto* value = value_for(row, column.stable_name);
              if (value == nullptr && !column.nullable) {
                return fail("SB_MODEL_TYPED_EXCHANGE_INVALID_V1",
                            "composition provider row lacks a non-null column");
              }
              tuple.values.push_back(exec::MakeExecutorValue(
                  column.descriptor, value == nullptr ? std::string{} : *value,
                  value == nullptr));
            }
            output.batch.rows.push_back(std::move(tuple));
            exec::ModelProviderRowIdentityV1 identity;
            if (source_input.family_id == "document") {
              const auto* document_uuid = value_for(row, "document_uuid");
              identity.document_uuid =
                  document_uuid != nullptr ? *document_uuid : row.row_uuid;
              identity.row_uuid = row.row_uuid;
            } else if (source_input.family_id == "graph") {
              const auto* vertex_uuid = value_for(row, "vertex_uuid");
              const auto* path_uuid = value_for(row, "path_uuid");
              identity.row_uuid = row.row_uuid;
              identity.vertex_uuid =
                  vertex_uuid != nullptr ? *vertex_uuid : row.row_uuid;
              identity.path_uuid =
                  path_uuid != nullptr ? *path_uuid : row.row_uuid;
            } else if (source_input.family_id == "key_value") {
              identity.row_uuid = row.row_uuid;
              const auto* key = value_for(row, "key");
              identity.key = key == nullptr ? std::string{} : *key;
            } else if (source_input.family_id == "time_series") {
              identity.row_uuid = row.row_uuid;
              identity.series_uuid = source_input.object_uuid;
              const auto* metric_uuid = value_for(row, "metric_uuid");
              const auto* timestamp = value_for(row, "point_timestamp");
              const auto* tags = value_for(row, "tags");
              const auto* value = value_for(row, "value");
              identity.metric_uuid =
                  metric_uuid == nullptr ? std::string{} : *metric_uuid;
              identity.tags = tags == nullptr ? std::string{} : *tags;
              identity.time_series_payload_kind = "raw.real64.v1";
              identity.time_series_raw_value =
                  value == nullptr ? std::string{} : *value;
              if (timestamp != nullptr) {
                exec::ParseCanonicalTimeSeriesTimestampNsV1(
                    *timestamp, &identity.point_timestamp_ns);
              }
            } else if (source_input.family_id == "vector") {
              identity.row_uuid = row.row_uuid;
              const auto* distance = value_for(row, "distance");
              const auto* score = value_for(row, "score");
              identity.vector_distance =
                  distance == nullptr ? std::string{} : *distance;
              identity.vector_score =
                  score == nullptr ? std::string{} : *score;
            } else if (source_input.family_id == "search") {
              const auto* document_uuid = value_for(row, "document_uuid");
              const auto* analyzer_uuid = value_for(row, "analyzer_uuid");
              const auto* analyzer_generation =
                  value_for(row, "analyzer_generation");
              const auto* score = value_for(row, "score");
              const auto* rank = value_for(row, "rank");
              identity.document_uuid =
                  document_uuid == nullptr ? std::string{} : *document_uuid;
              identity.search_analyzer_uuid =
                  analyzer_uuid == nullptr ? std::string{} : *analyzer_uuid;
              if (analyzer_generation != nullptr) {
                std::from_chars(analyzer_generation->data(),
                                analyzer_generation->data() +
                                    analyzer_generation->size(),
                                identity.search_analyzer_generation);
              }
              identity.search_score = score == nullptr ? std::string{} : *score;
              if (rank != nullptr) {
                std::from_chars(rank->data(), rank->data() + rank->size(),
                                identity.search_rank);
              }
            } else {
              identity.row_uuid = row.row_uuid;
            }
            output.ordered_row_identities.push_back(std::move(identity));
          }
          provider.ok = true;
          return provider;
        };
    }
    execution_leg.pause_exchange = [] {};
    execution_leg.resume_exchange = [] {};
    execution_leg.cleanup_exchange = [] {};
    execution_request.legs.push_back(std::move(execution_leg));
  }

  execution_request.execute_relational_consumer =
      [cancellation_requested](
          const opt::ModelFamilyRelationalConsumerV1& consumer,
          const exec::DescriptorBatch& left,
          const exec::DescriptorBatch& right) {
        exec::ModelFamilyRelationalConsumerExecutionResultV1 consumed;
        const auto fail = [&](std::string diagnostic, std::string detail) {
          consumed.diagnostic_id = std::move(diagnostic);
          consumed.detail = std::move(detail);
          return consumed;
        };
        if (cancellation_requested()) {
          return fail("SB_MODEL_EXECUTION_CANCELLED_V1",
                      "bounded CROSS consumer was cancelled before materialization");
        }
        std::uint64_t output_rows = 0;
        std::uint64_t output_columns = 0;
        std::uint64_t output_cells = 0;
        if (!CheckedMultiply(left.rows.size(), right.rows.size(),
                             &output_rows) ||
            !CheckedAdd(left.columns.size(), right.columns.size(),
                        &output_columns) ||
            !CheckedMultiply(output_rows, output_columns, &output_cells) ||
            output_rows > consumer.maximum_rows ||
            output_columns > consumer.maximum_columns ||
            output_cells > consumer.maximum_cells) {
          return fail("SB_MODEL_RESOURCE_ROW_LIMIT_V1",
                      "bounded CROSS product exceeds its checked row, column, or cell grant");
        }
        std::uint64_t output_memory = sizeof(exec::DescriptorBatch);
        const auto add_memory = [&](const std::uint64_t bytes) {
          return CheckedAdd(output_memory, bytes, &output_memory) &&
                 output_memory <= consumer.memory_grant_bytes;
        };
        const auto add_column = [&](const exec::ExecutorColumnDescriptor& col) {
          return add_memory(sizeof(exec::ExecutorColumnDescriptor)) &&
                 add_memory(col.stable_name.size()) &&
                 add_memory(col.descriptor.descriptor_uuid.canonical.size()) &&
                 add_memory(col.descriptor.descriptor_kind.size()) &&
                 add_memory(col.descriptor.canonical_type_name.size()) &&
                 add_memory(col.descriptor.encoded_descriptor.size());
        };
        const auto add_value = [&](const api::EngineTypedValue& value) {
          return add_memory(sizeof(api::EngineTypedValue)) &&
                 add_memory(value.descriptor.descriptor_uuid.canonical.size()) &&
                 add_memory(value.descriptor.descriptor_kind.size()) &&
                 add_memory(value.descriptor.canonical_type_name.size()) &&
                 add_memory(value.descriptor.encoded_descriptor.size()) &&
                 add_memory(value.encoded_value.size()) &&
                 add_memory(value.binary_value.size());
        };
        for (const auto& column : left.columns) {
          if (!add_column(column)) {
            return fail("SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                        "bounded CROSS column inventory exceeds its memory grant");
          }
        }
        for (const auto& column : right.columns) {
          if (!add_column(column)) {
            return fail("SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                        "bounded CROSS column inventory exceeds its memory grant");
          }
        }
        for (const auto& left_row : left.rows) {
          for (const auto& right_row : right.rows) {
            if (!add_memory(sizeof(exec::DescriptorTuple))) {
              return fail("SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                          "bounded CROSS rows exceed their memory grant");
            }
            for (const auto& value : left_row.values) {
              if (!add_value(value)) {
                return fail("SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                            "bounded CROSS cells exceed their memory grant");
              }
            }
            for (const auto& value : right_row.values) {
              if (!add_value(value)) {
                return fail("SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                            "bounded CROSS cells exceed their memory grant");
              }
            }
          }
        }
        consumed.executed_physical_node_id = consumer.physical_node_id;
        consumed.causal_counter_id = consumer.causal_counter_id;
        consumed.selected_implementation_uuid =
            consumer.selected_implementation_uuid;
        consumed.mga_statement_context = consumer.mga_statement_context;
        consumed.security_receipt_uuid =
            consumer.expected_security_receipt_uuid;
        consumed.output_batch.columns = left.columns;
        consumed.output_batch.columns.insert(consumed.output_batch.columns.end(),
                                              right.columns.begin(),
                                              right.columns.end());
        for (const auto& left_row : left.rows) {
          if (cancellation_requested()) {
            return fail("SB_MODEL_EXECUTION_CANCELLED_V1",
                        "bounded CROSS consumer was cancelled during materialization");
          }
          for (const auto& right_row : right.rows) {
            exec::DescriptorTuple row;
            row.values = left_row.values;
            row.values.insert(row.values.end(), right_row.values.begin(),
                              right_row.values.end());
            consumed.output_batch.rows.push_back(std::move(row));
          }
        }
        consumed.rows_examined = left.rows.size() + right.rows.size();
        consumed.ok = true;
        return consumed;
      };
  execution_request.cleanup_relational_consumer = [](const auto) {};
  execution_request.cancellation_requested = cancellation_requested;
  execution_request.revalidate_publication_state =
      [admitted, mga, context = input.context, prepared_sources,
       identity_scope]() {
        exec::ModelFamilyCompositionPublicationStateV1 publication;
        publication.current_selected_plan_generation =
            context.optimizer_route_generation;
        publication.security_admitted = true;
        api::EngineResolveStatementSnapshotRequest snapshot_request;
        snapshot_request.context = context;
        const auto snapshot = api::EngineResolveStatementSnapshot(
            snapshot_request);
        if (snapshot.ok) {
          publication.current_mga_statement_context =
              PhysicalMgaContextFromResolvedSnapshot(
                  context, snapshot.snapshot_vector);
          publication.current_mga_statement_context.statement_timestamp =
              mga.statement_timestamp;
        }
        for (std::size_t ordinal = 0; ordinal < prepared_sources.size();
             ++ordinal) {
          const auto scheduled = std::ranges::find_if(
              admitted.stable_schedule, [&](const auto& candidate) {
                return candidate.leg.lexical_source_ordinal == ordinal;
              });
          if (scheduled == admitted.stable_schedule.end()) {
            publication.security_admitted = false;
            break;
          }
          const auto& leg = scheduled->leg;
          const auto& prepared = prepared_sources[ordinal];
          const auto authorization = api::EvaluateMaterializedAuthorization(
              context, context.authorization_context, "SELECT",
              leg.bound_object_uuid);
          const auto current = api::LoadMgaRelationStorageDescriptor(
              context, leg.bound_object_uuid);
          const bool current_authority =
              authorization.authorized && !authorization.denied &&
              !authorization.policy_recheck_required &&
              authorization.diagnostics.empty() && current.ok &&
              current.descriptor.relation_uuid.canonical ==
                  leg.bound_object_uuid &&
              current.descriptor.descriptor_uuid.canonical ==
                  prepared.persisted.descriptor_uuid.canonical &&
              current.descriptor.descriptor_generation ==
                  prepared.persisted.descriptor_generation &&
              current.descriptor.columns.size() ==
                  prepared.persisted.columns.size();
          publication.security_admitted =
              publication.security_admitted && current_authority;
          publication.current_catalog_generations.push_back(
              context.catalog_generation_id);
          publication.current_descriptor_generations.push_back(
              current.ok ? current.descriptor.descriptor_generation : 0);
          publication.current_security_generations.push_back(
              context.security_epoch);
          publication.current_policy_generations.push_back(
              context.authorization_context.policy_epoch);
          publication.current_resource_generations.push_back(
              context.resource_epoch);
          publication.current_provider_generations.push_back(
              current.ok ? current.descriptor.descriptor_generation : 0);
          publication.current_capability_generations.push_back(
              current.ok ? current.descriptor.descriptor_generation : 0);
          publication.current_catalog_snapshot_uuids.push_back(
              context.catalog_epoch_uuid.canonical);
          publication.current_descriptor_snapshot_uuids.push_back(
              current.ok ? current.descriptor.descriptor_uuid.canonical
                         : std::string{});
          publication.current_security_context_uuids.push_back(
              context.authorization_context.authority_uuid.canonical);
          publication.current_policy_snapshot_uuids.push_back(
              DerivedCanonicalUuid(
                  identity_scope,
                  "rcp080.policy." + std::to_string(ordinal)));
          publication.current_resource_contract_uuids.push_back(
              DerivedCanonicalUuid(
                  identity_scope,
                  "rcp080.resource." + std::to_string(ordinal)));
          publication.current_provider_uuids.push_back(leg.provider_uuid);
          publication.current_capability_uuids.push_back(leg.capability_uuid);
        }
        return publication;
      };
  execution_request.backpressure_high_watermark_rows = 64;
  execution_request.backpressure_low_watermark_rows = 32;
  execution_request.current_selected_plan_generation =
      admitted.selected_plan_generation;
  execution_request.current_mga_statement_context = mga;
  auto executed =
      exec::ExecuteModelFamilyCompositionV1(execution_request);
  if (!executed.accepted || !executed.execution_started ||
      !executed.root_published || !executed.no_partial_root ||
      !executed.cleanup_complete ||
      executed.started_leg_ordinals.size() != sources.size() ||
      executed.started_relational_consumer_ids.size() != joins.size()) {
    return refuse(executed.diagnostic_id.empty()
                      ? "SB_MODEL_COORDINATOR_LEG_FAILED_V1"
                      : executed.diagnostic_id,
                  executed.detail.empty()
                      ? "bounded composition executor did not publish one complete root"
                      : executed.detail);
  }

  exec::CanonicalResultPublicationRequest publication;
  publication.statement_uuid = input.context.statement_uuid.canonical;
  publication.mga_authority =
      BuildCanonicalExecutionMgaAuthority(input.context, physical_dag);
  publication.selected_physical_dag = std::move(physical_dag);
  publication.selected_catalog_epoch_uuid =
      input.context.catalog_epoch_uuid.canonical;
  publication.execution_attempt_uuid = DerivedCanonicalUuid(
      identity_scope + ":" + input.context.current_monotonic_ns,
      "rcp080.execution-attempt");
  publication.transaction_effect_evidence_uuid = DerivedCanonicalUuid(
      identity_scope + ":" +
          std::to_string(input.context.local_transaction_id),
      "rcp080.transaction-effect-unchanged");
  publication.maximum_row_count = maximum_rows;
  std::vector<const api::RelationalOutputRecord*> root_outputs;
  for (const auto& output : dag.outputs) {
    if (output.relation_node_id == dag.root_node_id) root_outputs.push_back(&output);
  }
  std::ranges::sort(root_outputs, {}, &api::RelationalOutputRecord::ordinal);
  if (root_outputs.size() != executed.root_output_batch.columns.size()) {
    return refuse("SB_MODEL_TYPED_EXCHANGE_INVALID_V1",
                  "bounded composition root publication binding is incomplete");
  }
  publication.physical_output_batch =
      std::move(executed.root_output_batch);
  for (std::size_t ordinal = 0; ordinal < root_outputs.size(); ++ordinal) {
    const auto descriptor = descriptor_for(root_outputs[ordinal]->descriptor_id);
    if (descriptor == dag.descriptors.end() ||
        root_outputs[ordinal]->ordinal != ordinal) {
      return refuse("SB_MODEL_TYPED_EXCHANGE_INVALID_V1",
                    "bounded composition root descriptor is unresolved");
    }
    exec::CanonicalResultColumnDescriptor published;
    published.ordinal = static_cast<std::uint32_t>(ordinal);
    published.name_utf8 = root_outputs[ordinal]->output_name_utf8;
    published.descriptor_uuid = descriptor->descriptor_uuid;
    published.type_uuid = descriptor->type_uuid;
    published.nullability = ResultNullability(descriptor->nullability);
    published.collation_uuid = descriptor->collation_uuid;
    published.timezone_profile_id = descriptor->timezone_profile_id;
    publication.column_bindings.push_back({ordinal, true, std::move(published)});
  }
  auto published = exec::PublishCanonicalResultEnvelope(publication);
  if (!published.published || !published.diagnostic.ok) {
    return refuse(published.diagnostic.diagnostic_code.empty()
                      ? "SB_MODEL_ROOT_PUBLICATION_REFUSED_V1"
                      : published.diagnostic.diagnostic_code,
                  published.diagnostic.detail);
  }
  result.physical_dag_executed = true;
  result.runtime_actuals_attached = true;
  result.canonical_result_published = true;
  result.canonical_result_column_count = published.envelope.column_descriptors.size();
  result.canonical_result_row_count = published.row_stream.rows.size();
  result.canonical_result_bytes =
      std::move(published.canonical_envelope_bytes);
  result.api_result.ok = true;
  result.api_result.operation_id = "query.execute";
  result.api_result.result_shape.result_kind = "rows";
  result.api_result.local_transaction_id = input.context.local_transaction_id;
  result.api_result.transaction_uuid = input.context.transaction_uuid;
  result.api_result.embedded_trust_mode_observed =
      input.context.trust_mode == api::EngineTrustMode::embedded_in_process;
  for (auto& column : published.row_stream.columns) {
    result.api_result.result_shape.columns.push_back(
        std::move(column.descriptor));
  }
  for (auto& row : published.row_stream.rows) {
    api::EngineRowValue api_row;
    for (std::size_t column = 0; column < row.values.size(); ++column) {
      api_row.fields.emplace_back(
          published.envelope.column_descriptors[column].name_utf8,
          std::move(row.values[column]));
    }
    result.api_result.result_shape.rows.push_back(std::move(api_row));
  }
  result.api_result.evidence.push_back(
      {"canonical.model_composition_profile", composition_profile_id});
  result.api_result.evidence.push_back(
      {"canonical.model_composition_coordinator", "entered"});
  result.api_result.evidence.push_back(
      {"canonical.model_composition_executor", "entered"});
  result.api_result.evidence.push_back(
      {"canonical.model_composition_leg_count", std::to_string(sources.size())});
  result.api_result.evidence.push_back(
      {"canonical.model_composition_consumer_count", std::to_string(joins.size())});
  result.api_result.evidence.push_back(
      {"canonical.model_composition_provider_entry_count",
       std::to_string(executed.provider_entry_count)});
  result.api_result.evidence.push_back(
      {"canonical.model_composition_observed_data_access_count",
       std::to_string(executed.observed_data_access_count)});
  result.api_result.evidence.push_back(
      {"canonical.model_composition_real_mga_read_count",
       std::to_string(executed.observed_data_access_count)});
  result.api_result.evidence.push_back(
      {"canonical.model_composition_root_publication_count", "1"});
  result.api_result.evidence.push_back(
      {"canonical.model_composition_cleanup_count",
       std::to_string(executed.total_cleanup_count)});
  return result;
}

CanonicalObjectFreeValuesExecutionResult
ExecuteCanonicalCapturedModelFamilyJoinQuery(
    const CanonicalCurrentHeapExecutionRequest& input) {
  CanonicalObjectFreeValuesExecutionResult result;
  const auto& dag = input.relational_dag;
  std::vector<const api::RelationalDagNode*> sources;
  const api::RelationalDagNode* join = nullptr;
  for (const auto& node : dag.nodes) {
    const bool model_source =
        (node.node_kind == api::RelationalDagNodeKind::kScan ||
         node.node_kind == api::RelationalDagNodeKind::kAggregate) &&
        (node.semantic_variant_id == "SBLR_MODEL_SOURCE_V1" ||
         node.semantic_variant_id == "SBLR_MODEL_AGGREGATE_V1");
    const bool relational_source =
        node.node_kind == api::RelationalDagNodeKind::kScan &&
        node.semantic_variant_id == "relation.source.v1";
    if (model_source || relational_source) {
      sources.push_back(&node);
    } else if (node.node_kind == api::RelationalDagNodeKind::kJoin &&
               join == nullptr) {
      join = &node;
    }
  }
  if (sources.size() != 2 || join == nullptr || dag.nodes.size() != 3) {
    return result;
  }
  const auto has_attached_model_operation = [&](const auto* source) {
    if (source->semantic_variant_id == "relation.source.v1") return true;
    return std::ranges::any_of(
        source->bound_expression_ids, [&](const std::uint32_t expression_id) {
          const auto expression = std::ranges::find_if(
              dag.expressions, [&](const auto& candidate) {
                return candidate.expression_id == expression_id;
              });
          if (expression == dag.expressions.end() ||
              !expression->operator_name.has_value()) {
            return false;
          }
          const auto& operation = *expression->operator_name;
          return operation == "DOCUMENT_SOURCE" ||
                 operation == "DOCUMENT_UNNEST" ||
                 operation == "DOCUMENT_PATH" ||
                 operation == "GRAPH_MATCH" || operation == "GRAPH_EXPAND" ||
                 operation == "KV_KEY" || operation == "KV_MULTI_GET" ||
                 operation == "KV_PREFIX" || operation == "TIME_RANGE" ||
                 operation == "TIME_BUCKET" ||
                 operation == "TIME_DOWNSAMPLE" ||
                 operation == "VECTOR_NEAREST" ||
                 operation == "VECTOR_FILTER" ||
                 operation == "SEARCH_MATCH" ||
                 operation == "SEARCH_TERMS" ||
                 operation == "SEARCH_PHRASE" ||
                 operation == "SEARCH_FUZZY" ||
                 operation == "SEARCH_FILTER" ||
                 operation == "SEARCH_ANALYZER_BINDING" ||
                 operation == "SPATIAL_SOURCE" ||
                 operation == "SPATIAL_MATCH" ||
                 operation == "SPATIAL_NEAREST" ||
                 operation == "COLUMNAR_SOURCE" ||
                 operation == "COLUMNAR_PROJECT" ||
                 operation == "COLUMNAR_FILTER";
        });
  };
  if (!std::ranges::all_of(sources, has_attached_model_operation)) {
    // Adjacent legacy family routes may still carry an operation globally.
    // They remain owned by their dedicated canonical executor; this captured
    // pair route admits only explicitly source-attached operation closures.
    return result;
  }
  std::ranges::sort(sources, [&](const auto* left, const auto* right) {
    const auto position = [&](const std::uint32_t node_id) {
      const auto found = std::ranges::find(join->input_node_ids, node_id);
      return found == join->input_node_ids.end()
                 ? join->input_node_ids.size()
                 : static_cast<std::size_t>(
                       std::distance(join->input_node_ids.begin(), found));
    };
    return position(left->node_id) < position(right->node_id);
  });
  result.profile_matched = true;
  CanonicalObjectFreeValuesExecutionRequest response_context;
  response_context.context = input.context;
  response_context.relational_dag = dag;
  const auto refuse = [&](std::string diagnostic_id, std::string detail) {
    result.optimizer_selected = false;
    result.physical_dag_published = false;
    result.physical_dag_executed = false;
    result.runtime_actuals_attached = false;
    result.canonical_result_published = false;
    result.physical_node_count = 0;
    result.canonical_result_column_count = 0;
    result.canonical_result_row_count = 0;
    result.selected_plan_uuid.clear();
    result.canonical_result_bytes.clear();
    result.api_result = Failure(response_context, std::move(diagnostic_id),
                                std::move(detail));
    return result;
  };

  exec::CanonicalAcceptedJoinKind join_kind;
  std::string join_form;
  std::string join_component;
  LiveLateralSubqueryProfile lateral_profile =
      MatchLiveLateralSubqueryProfileForComposition(join->semantic_variant_id);
  const bool asof_join =
      join->semantic_variant_id == "join.asof.left.v1" ||
      join->semantic_variant_id == "join.asof.inner.v1";
  const bool asof_left_outer =
      join->semantic_variant_id == "join.asof.left.v1";
  if (asof_join) {
    join_kind = asof_left_outer
                    ? exec::CanonicalAcceptedJoinKind::kLeftOuter
                    : exec::CanonicalAcceptedJoinKind::kInner;
    join_form = "ASOF";
    join_component = asof_left_outer ? "asof-left" : "asof-inner";
  } else if (lateral_profile.matched &&
      lateral_profile.form ==
          exec::CanonicalLateralJoinForm::kInnerLateral) {
    join_kind = exec::CanonicalAcceptedJoinKind::kInner;
    join_form = "LATERAL_INNER";
    join_component = "lateral-inner";
  } else if (lateral_profile.matched &&
             lateral_profile.form ==
                 exec::CanonicalLateralJoinForm::kLeftLateral) {
    join_kind = exec::CanonicalAcceptedJoinKind::kLeftOuter;
    join_form = "LATERAL_LEFT";
    join_component = "lateral-left";
  } else if (lateral_profile.matched &&
             lateral_profile.form ==
                 exec::CanonicalLateralJoinForm::kCrossApply) {
    join_kind = exec::CanonicalAcceptedJoinKind::kInner;
    join_form = "LATERAL_INNER";
    join_component = "cross-apply";
  } else if (lateral_profile.matched &&
             lateral_profile.form ==
                 exec::CanonicalLateralJoinForm::kOuterApply) {
    join_kind = exec::CanonicalAcceptedJoinKind::kLeftOuter;
    join_form = "LATERAL_LEFT";
    join_component = "outer-apply";
  } else if (join->semantic_variant_id.starts_with("join.inner")) {
    join_kind = exec::CanonicalAcceptedJoinKind::kInner;
    join_form = "INNER";
    join_component = "inner";
  } else if (join->semantic_variant_id.starts_with("join.left-outer")) {
    join_kind = exec::CanonicalAcceptedJoinKind::kLeftOuter;
    join_form = "LEFT";
    join_component = "left-outer";
  } else if (join->semantic_variant_id.starts_with("join.right-outer")) {
    join_kind = exec::CanonicalAcceptedJoinKind::kRightOuter;
    join_form = "RIGHT";
    join_component = "right-outer";
  } else if (join->semantic_variant_id.starts_with("join.full-outer")) {
    join_kind = exec::CanonicalAcceptedJoinKind::kFullOuter;
    join_form = "FULL";
    join_component = "full-outer";
  } else if (join->semantic_variant_id.starts_with("join.left-semi")) {
    join_kind = exec::CanonicalAcceptedJoinKind::kLeftSemi;
    join_form = "SEMI";
    join_component = "left-semi";
  } else if (join->semantic_variant_id.starts_with("join.left-anti")) {
    join_kind = exec::CanonicalAcceptedJoinKind::kLeftAnti;
    join_form = "ANTI";
    join_component = "left-anti";
  } else if (join->semantic_variant_id == "join.cross.v1") {
    join_kind = exec::CanonicalAcceptedJoinKind::kCross;
    join_form = "CROSS";
    join_component = "cross";
  } else {
    return refuse("SB_MODEL_JOIN_FORM_REFUSED_V1",
                  "captured model-family join form is not yet executable");
  }
  std::string condition_form =
      asof_join ? "ASOF_KEY" : (join_form == "CROSS" ? "NONE" : "ON");
  if (!asof_join &&
      join->semantic_variant_id.find(".using.") != std::string::npos) {
    condition_form = "USING";
  } else if (join->semantic_variant_id.find(".natural.") !=
             std::string::npos) {
    condition_form = "NATURAL";
  }
  if (condition_form == "USING" || condition_form == "NATURAL") {
    return refuse(
        condition_form == "USING"
            ? "SB_MODEL_JOIN_USING_BINDING_REFUSED_V1"
            : "SB_MODEL_JOIN_NATURAL_BINDING_REFUSED_V1",
        "captured model-family USING/NATURAL join lacks binder-owned named "
        "bindings and a coalescing projection");
  }
  const auto left_family = Rcp079ModelFamilyForSource(dag, *sources[0]);
  const auto right_family = Rcp079ModelFamilyForSource(dag, *sources[1]);
  opt::ModelFamilyJoinAdmissionRequestV1 pair_request;
  pair_request.left_family_id = left_family;
  pair_request.right_family_id = right_family;
  pair_request.join_form_id = join_form;
  pair_request.condition_form_id = condition_form;
  const auto pair_admission =
      opt::CoordinateModelFamilyJoinAdmissionV1(pair_request);
  if (!pair_admission.accepted ||
      !pair_admission.root_publication_allowed) {
    return refuse(pair_admission.diagnostic_id, pair_admission.detail);
  }
  const auto expected_provider_route = [](const std::string_view family) {
    return family == "relational"
               ? std::string("canonical.relational.heap-source.v1")
               : std::string("canonical.model-provider.") +
                     std::string(family) + ".v1";
  };
  const auto expected_consumer_route =
      join_form == "LATERAL_INNER" || join_form == "LATERAL_LEFT"
          ? std::string("canonical.relational.lateral-correlated.v1")
          : (join_form == "ASOF"
                 ? std::string("canonical.relational.time-series-asof.v1")
                 : std::string("canonical.relational.join-3vl-nested.v1"));
  const auto expected_condition_route =
      condition_form == "ON"
          ? std::string("canonical.relational.on-typed-predicate.v1")
          : (condition_form == "USING"
                 ? std::string(
                       "canonical.relational.using-descriptor-equality.v1")
                 : (condition_form == "NATURAL"
                        ? std::string(
                              "canonical.relational.natural-to-using.v1")
                        : (condition_form == "NONE"
                               ? std::string(
                                     "canonical.relational.cross-no-condition.v1")
                               : std::string(
                                     "canonical.relational.asof-key-binding.v1"))));
  if (pair_admission.left_provider_route_id !=
          expected_provider_route(left_family) ||
      pair_admission.right_provider_route_id !=
          expected_provider_route(right_family) ||
      pair_admission.relational_consumer_route_id !=
          expected_consumer_route ||
      pair_admission.condition_lowering_route_id !=
          expected_condition_route) {
    return refuse("SB_MODEL_JOIN_SEMANTIC_PRECONDITION_REFUSED_V1",
                  "model-family coordinator route receipt changed");
  }
  const bool lateral_join = lateral_profile.matched;
  const bool predicate_join =
      join_form != "CROSS" && !lateral_join && !asof_join;
  const bool left_only = join_form == "SEMI" || join_form == "ANTI";
  std::vector<std::uint32_t> expected_output =
      sources[0]->output_descriptor_ids;
  if (!left_only) {
    expected_output.insert(expected_output.end(),
                           sources[1]->output_descriptor_ids.begin(),
                           sources[1]->output_descriptor_ids.end());
  }
  if (dag.wire_version != 2 || dag.root_node_id != join->node_id ||
      join->input_node_ids !=
          std::vector<std::uint32_t>{sources[0]->node_id,
                                     sources[1]->node_id} ||
      join->output_descriptor_ids != expected_output ||
      join->bound_expression_ids.size() !=
          (asof_join ? 7U : static_cast<std::size_t>(predicate_join)) ||
      dag.statement_timestamp.empty() ||
      dag.statement_timestamp != input.context.statement_timestamp) {
    return refuse("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                  "captured model-family pair DAG is incomplete");
  }

  std::vector<std::string> authorized_object_uuids;
  authorized_object_uuids.reserve(sources.size());
  for (const auto* source : sources) {
    if (source->required_object_uuids.size() != 1 ||
        !CanonicalUuidText(source->required_object_uuids.front())) {
      return refuse("SB_MODEL_SECURITY_ADMISSION_REFUSED_V1",
                    "captured model-family object closure is not exact");
    }
    if (std::ranges::find(authorized_object_uuids,
                          source->required_object_uuids.front()) ==
        authorized_object_uuids.end()) {
      authorized_object_uuids.push_back(source->required_object_uuids.front());
    }
  }
  const auto complete_object_closure_authorized = [&] {
    return std::ranges::all_of(
        authorized_object_uuids, [&](const auto& object_uuid) {
          const auto authorization = api::EvaluateMaterializedAuthorization(
              input.context, input.context.authorization_context, "SELECT",
              object_uuid);
          return authorization.authorized && !authorization.denied &&
                 !authorization.policy_recheck_required &&
                 authorization.diagnostics.empty();
        });
  };
  if (!complete_object_closure_authorized()) {
    return refuse("SB_MODEL_SECURITY_ADMISSION_REFUSED_V1",
                  "captured model-family SELECT closure was refused");
  }

  const auto descriptor_preflight =
      Rcp079PreflightMultilegResultDescriptorsV1(
          input, sources, join_kind, left_only);
  if (!descriptor_preflight.accepted) {
    return refuse(descriptor_preflight.diagnostic_id,
                  descriptor_preflight.detail);
  }

  const auto identity_scope =
      dag.bound_sblr_tree_uuid + ":" + input.context.statement_uuid.canonical;
  std::array<Rcp079CapturedModelLegV1, 2> captured;
  for (std::size_t ordinal = 0; ordinal < sources.size(); ++ordinal) {
    const auto family =
        Rcp079ModelFamilyForSource(dag, *sources[ordinal]);
    if (family == "relational") {
      auto& leg = captured[ordinal];
      leg.captured = true;
      leg.logical_node_id = sources[ordinal]->node_id;
      leg.family_id = family;
      leg.implementation_id = "scan.heap.v1";
      leg.capability_uuid = DerivedCanonicalUuid(
          identity_scope,
          "captured-relational-scan." +
              std::to_string(sources[ordinal]->node_id) + ".capability");
      leg.transformation_rule_id = "canonical.heap.scan.v1";
      leg.compatibility_profile_id = "canonical.heap.scan.v1";
      leg.logical_node_kind =
          plan::CanonicalLogicalRelationalNodeKind::kRelationSource;
      leg.physical_node_kind = exec::PhysicalNodeKind::kScan;
      if (sources[ordinal]->required_object_uuids.size() != 1) {
        return refuse("SB_MODEL_COORDINATOR_LEG_FAILED_V1",
                      "relational composition leg is not object-bound");
      }
      leg.execution_request.input.object_uuid =
          sources[ordinal]->required_object_uuids.front();
      continue;
    }
    CanonicalObjectFreeValuesExecutionResult attempted;
    if (!CaptureRcp079ModelSourceLeg(input, *sources[ordinal],
                                     &captured[ordinal], &attempted)) {
      const auto diagnostic = attempted.api_result.diagnostics.empty()
                                  ? "SB_MODEL_COORDINATOR_LEG_FAILED_V1"
                                  : attempted.api_result.diagnostics.front().code;
      const auto detail = attempted.api_result.diagnostics.empty()
                              ? "model-family source adapter was not captured"
                              : attempted.api_result.diagnostics.front().detail;
      return refuse(diagnostic, detail);
    }
  }
  const bool exact_columnar_pair =
      std::ranges::all_of(captured, [](const auto& leg) {
        return leg.family_id == "columnar" &&
               !leg.exact_output_columns.empty() &&
               leg.exact_output_columns.size() ==
                   leg.execution_request.input.output_descriptor_ids.size();
      });
  std::vector<exec::ExecutorColumnDescriptor> exact_join_columns;
  if (exact_columnar_pair) {
    exact_join_columns = captured[0].exact_output_columns;
    if (!left_only) {
      exact_join_columns.insert(exact_join_columns.end(),
                                captured[1].exact_output_columns.begin(),
                                captured[1].exact_output_columns.end());
    }
    const auto left_width = captured[0].exact_output_columns.size();
    for (std::size_t ordinal = 0; ordinal < exact_join_columns.size();
         ++ordinal) {
      const bool null_extended =
          (ordinal < left_width &&
           (join_kind == exec::CanonicalAcceptedJoinKind::kRightOuter ||
            join_kind == exec::CanonicalAcceptedJoinKind::kFullOuter)) ||
          (ordinal >= left_width &&
           (join_kind == exec::CanonicalAcceptedJoinKind::kLeftOuter ||
            join_kind == exec::CanonicalAcceptedJoinKind::kFullOuter));
      if (null_extended) {
        exact_join_columns[ordinal].nullable = true;
        if (!exec::DeriveCanonicalNullableDescriptorEncoding(
                &exact_join_columns[ordinal].descriptor)) {
          return refuse(
              "SB_MODEL_RESULT_DESCRIPTOR_SOURCE_BINDING_INVALID_V1",
              "columnar outer result lacks an exact nullable carrier");
        }
      }
    }
  }
  const auto model_leg = std::ranges::find_if(captured, [](const auto& leg) {
    return leg.family_id != "relational";
  });
  if (model_leg == captured.end()) return result;
  const auto common_mga =
      model_leg->execution_request.input.mga_statement_context;
  for (auto& leg : captured) {
    if (leg.family_id == "relational") {
      leg.execution_request.input.mga_statement_context = common_mga;
    } else if (!exec::PhysicalMgaStatementContextEqual(
                   common_mga,
                   leg.execution_request.input.mga_statement_context)) {
      return refuse("SB_MODEL_MGA_CONTEXT_MISMATCH_V1",
                    "captured model-family legs do not share one MGA statement");
    }
  }

  api::CanonicalRelationalPlanningScope planning_scope;
  planning_scope.catalog_epoch_uuid = input.context.catalog_epoch_uuid.canonical;
  planning_scope.security_context_uuid =
      input.context.authorization_context.authority_uuid.canonical;
  planning_scope.statement_uuid = input.context.statement_uuid.canonical;
  planning_scope.statement_timestamp = input.context.statement_timestamp;
  planning_scope.owning_transaction_uuid = input.context.transaction_uuid.canonical;
  planning_scope.statement_snapshot_uuid =
      input.context.statement_snapshot_uuid.canonical;
  planning_scope.statement_metadata_snapshot_uuid =
      input.context.statement_metadata_snapshot_uuid.canonical;
  planning_scope.local_transaction_id = input.context.local_transaction_id;
  planning_scope.snapshot_visible_through_local_transaction_id =
      input.context.snapshot_visible_through_local_transaction_id;
  planning_scope.metadata_snapshot_engine_owned =
      input.context.statement_metadata_snapshot_engine_owned;
  planning_scope.authorization_context_engine_owned =
      input.context.authorization_context.present;
  auto logical = api::PopulateCanonicalLogicalGraphFromAdmittedTypedRelationalDag(
      dag, planning_scope);
  if (!logical.accepted || logical.logical_graph.nodes.size() != 3) {
    return refuse(logical.issues.empty()
                      ? "QOW-DIAG-OPTIMIZER-ADMISSION-BOUND-REQUEST-V1"
                      : logical.issues.front().diagnostic_id,
                  logical.issues.empty()
                      ? "captured model-family logical bridge was refused"
                      : logical.issues.front().field_id);
  }
  const auto& mga = common_mga;
  const auto registered_logical_mga = Rcp079LogicalMga(mga, false);
  const auto current_logical_mga = Rcp079LogicalMga(mga, true);
  if (!plan::CanonicalMgaStatementContextEqual(
          logical.logical_graph.mga_statement_context,
          registered_logical_mga) ||
      !plan::CanonicalMgaStatementContextEqual(
          logical.property_catalog.mga_statement_context,
          registered_logical_mga)) {
    return refuse("SB_MODEL_MGA_CONTEXT_MISMATCH_V1",
                  "captured model-family logical MGA cohort changed");
  }
  logical.logical_graph.mga_statement_context = current_logical_mga;
  logical.property_catalog.mga_statement_context = current_logical_mga;

  opt::CanonicalNativeObjectAdmissionContext admission_context;
  admission_context.statement_uuid = input.context.statement_uuid.canonical;
  admission_context.catalog_snapshot_uuid =
      input.context.statement_metadata_snapshot_uuid.canonical;
  admission_context.security_context_uuid =
      input.context.authorization_context.authority_uuid.canonical;
  admission_context.catalog_generation = input.context.catalog_generation_id;
  admission_context.authorization_catalog_generation =
      input.context.authorization_context.catalog_generation_id;
  admission_context.security_epoch =
      input.context.authorization_context.security_epoch;
  admission_context.policy_epoch =
      input.context.authorization_context.policy_epoch;
  admission_context.resource_epoch = input.context.resource_epoch;
  admission_context.capability_snapshot_uuid =
      input.context.optimizer_capability_snapshot_uuid.canonical;
  admission_context.resource_snapshot_uuid =
      input.context.optimizer_resource_snapshot_uuid.canonical;
  admission_context.route_snapshot_uuid =
      input.context.optimizer_route_snapshot_uuid.canonical;
  admission_context.route_epoch = input.context.optimizer_route_epoch;
  admission_context.route_generation = input.context.optimizer_route_generation;
  admission_context.memory_budget_bytes =
      input.context.optimizer_memory_budget_bytes;
  admission_context.maximum_candidate_count =
      input.context.optimizer_maximum_candidate_count;
  admission_context.maximum_memo_groups =
      input.context.optimizer_maximum_memo_groups;
  admission_context.maximum_search_steps =
      input.context.optimizer_maximum_search_steps;
  admission_context.maximum_planning_time_ns =
      input.context.optimizer_maximum_planning_time_ns;
  admission_context.spill_allowed = input.context.optimizer_spill_allowed;
  admission_context.local_transaction_id = input.context.local_transaction_id;
  admission_context.statement_snapshot_id =
      input.context.snapshot_visible_through_local_transaction_id;
  admission_context.mga_statement_context = current_logical_mga;
  std::uint64_t admitted_at_monotonic_ns = 0;
  const auto monotonic = std::from_chars(
      input.context.current_monotonic_ns.data(),
      input.context.current_monotonic_ns.data() +
          input.context.current_monotonic_ns.size(),
      admitted_at_monotonic_ns);
  if (monotonic.ec != std::errc{} ||
      monotonic.ptr != input.context.current_monotonic_ns.data() +
                           input.context.current_monotonic_ns.size() ||
      admitted_at_monotonic_ns == 0) {
    return refuse("SB_MODEL_OPERATION_SEMANTIC_REFUSED_V1",
                  "captured model-family monotonic context is invalid");
  }
  admission_context.admitted_at_monotonic_ns = admitted_at_monotonic_ns;
  admission_context.metadata_snapshot_engine_owned = true;
  admission_context.authorization_context_engine_owned = true;
  for (const auto& logical_node : logical.logical_graph.nodes) {
    for (const auto& object_uuid : logical_node.required_object_uuids) {
      if (std::ranges::find(admission_context.catalog_object_uuids,
                            object_uuid) ==
          admission_context.catalog_object_uuids.end()) {
        admission_context.catalog_object_uuids.push_back(object_uuid);
        admission_context.authorized_object_uuids.push_back(object_uuid);
      }
    }
  }
  admission_context.catalog_object_evidence_engine_owned = true;
  admission_context.authorization_object_evidence_engine_owned = true;
  auto canonical_admission =
      opt::BuildCanonicalObjectAwareNativeOptimizerAdmissionRequest(
          logical.logical_graph, logical.property_catalog, admission_context);
  if (!canonical_admission.built ||
      !canonical_admission.admission.admitted ||
      !canonical_admission.admission.planning_allowed ||
      canonical_admission.admission.data_access_allowed) {
    return refuse(canonical_admission.diagnostic_id.empty()
                      ? "QOW-DIAG-OPTIMIZER-ADMISSION-BOUND-REQUEST-V1"
                      : canonical_admission.diagnostic_id,
                  canonical_admission.field_id.empty()
                      ? "captured model-family optimizer admission failed"
                      : canonical_admission.field_id);
  }
  result.optimizer_admitted = true;
  result.optimizer_admission_stage_count =
      canonical_admission.admission.evidence.size();
  CanonicalObjectFreeValuesExecutionRequest planning_request{
      input.context, dag, canonical_admission.request,
      canonical_admission.admission};
  const auto maximum_rows = static_cast<std::size_t>(std::min<std::uint64_t>(
      65'536, input.context.optimizer_maximum_candidate_count));
  const auto maximum_columns = join->output_descriptor_ids.size();
  std::uint64_t maximum_pairs = 0;
  std::uint64_t maximum_cells = 0;
  std::uint64_t maximum_total_rows = 0;
  std::uint64_t maximum_total_cells = 0;
  std::uint64_t input_columns = 0;
  std::uint64_t source_cells = 0;
  if (maximum_rows == 0 || maximum_columns == 0 ||
      !CheckedAdd(sources[0]->output_descriptor_ids.size(),
                  sources[1]->output_descriptor_ids.size(), &input_columns) ||
      !CheckedMultiply(maximum_rows, maximum_rows, &maximum_pairs) ||
      maximum_pairs > std::numeric_limits<std::size_t>::max() ||
      !CheckedMultiply(maximum_rows, maximum_columns, &maximum_cells) ||
      !CheckedMultiply(maximum_rows, input_columns, &source_cells) ||
      !CheckedMultiply(maximum_rows, 3, &maximum_total_rows) ||
      !CheckedAdd(source_cells, maximum_cells, &maximum_total_cells) ||
      maximum_cells > std::numeric_limits<std::size_t>::max() ||
      maximum_total_rows > std::numeric_limits<std::size_t>::max() ||
      maximum_total_cells > std::numeric_limits<std::size_t>::max()) {
    return refuse("SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                  "captured model-family execution bounds overflowed");
  }
  exec::CanonicalTimeSeriesAsofInputBindingV1 asof_left_binding;
  exec::CanonicalTimeSeriesAsofInputBindingV1 asof_right_binding;
  std::int64_t asof_tolerance_ns = 0;
  std::uint64_t asof_maximum_comparisons = 0;
  std::string asof_transformation_receipt;
  if (asof_join) {
    const auto bind_asof_input =
        [&](const api::RelationalDagNode& source,
            const std::string_view family,
            exec::CanonicalTimeSeriesAsofInputBindingV1* binding) {
          if (binding == nullptr) return false;
          std::vector<const api::RelationalOutputRecord*> outputs;
          for (const auto& output : dag.outputs) {
            if (output.relation_node_id == source.node_id) {
              outputs.push_back(&output);
            }
          }
          std::ranges::sort(outputs, {},
                            &api::RelationalOutputRecord::ordinal);
          const auto unique_named = [&](const std::string_view name) {
            const api::RelationalOutputRecord* found = nullptr;
            for (const auto* output : outputs) {
              if (output->output_name_utf8 != name) continue;
              if (found != nullptr) {
                return static_cast<const api::RelationalOutputRecord*>(
                    nullptr);
              }
              found = output;
            }
            return found;
          };
          const auto* metric = unique_named("metric_uuid");
          const auto* tags = unique_named("tags");
          const auto* timestamp =
              family == "time_series"
                  ? unique_named(unique_named("point_timestamp") != nullptr
                                     ? "point_timestamp"
                                     : "bucket_start")
                  : unique_named("event_timestamp");
          if (outputs.size() != source.output_descriptor_ids.size() ||
              metric == nullptr || tags == nullptr || timestamp == nullptr) {
            return false;
          }
          const auto bind = [&](const api::RelationalOutputRecord& output,
                                std::uint32_t* expression_id,
                                std::uint32_t* descriptor_id,
                                std::size_t* ordinal) {
            if (output.ordinal >= source.output_descriptor_ids.size() ||
                source.output_descriptor_ids[output.ordinal] !=
                    output.descriptor_id ||
                std::ranges::find(source.bound_expression_ids,
                                  output.expression_id) ==
                    source.bound_expression_ids.end()) {
              return false;
            }
            *expression_id = output.expression_id;
            *descriptor_id = output.descriptor_id;
            *ordinal = output.ordinal;
            return true;
          };
          if (!bind(*metric, &binding->metric_expression_id,
                    &binding->metric_descriptor_id,
                    &binding->metric_column_ordinal) ||
              !bind(*tags, &binding->tags_expression_id,
                    &binding->tags_descriptor_id,
                    &binding->tags_column_ordinal) ||
              !bind(*timestamp, &binding->timestamp_expression_id,
                    &binding->timestamp_descriptor_id,
                    &binding->timestamp_column_ordinal)) {
            return false;
          }
          if (family == "time_series") {
            const auto* row_uuid = unique_named("row_uuid");
            binding->raw_time_series = row_uuid != nullptr;
            binding->downsample_time_series = row_uuid == nullptr;
            if (row_uuid != nullptr &&
                !bind(*row_uuid, &binding->row_uuid_expression_id,
                      &binding->row_uuid_descriptor_id,
                      &binding->row_uuid_column_ordinal)) {
              return false;
            }
          }
          return true;
        };
    if (!bind_asof_input(*sources[0], left_family, &asof_left_binding) ||
        !bind_asof_input(*sources[1], right_family, &asof_right_binding)) {
      return refuse("SB_MODEL_TYPED_EXCHANGE_INVALID_V1",
                    "captured model-family ASOF key binding is incomplete");
    }
    const std::array<std::uint32_t, 6> exact_keys{
        asof_left_binding.metric_expression_id,
        asof_left_binding.tags_expression_id,
        asof_left_binding.timestamp_expression_id,
        asof_right_binding.metric_expression_id,
        asof_right_binding.tags_expression_id,
        asof_right_binding.timestamp_expression_id};
    if (!std::equal(exact_keys.begin(), exact_keys.end(),
                    join->bound_expression_ids.begin())) {
      return refuse("SB_MODEL_ASOF_BINDING_REFUSED_V1",
                    "captured model-family ASOF key order changed");
    }
    CanonicalRelationalExpressionRuntime tolerance_runtime(dag);
    std::uint64_t tolerance = 0;
    std::string tolerance_detail;
    if (!EvaluateNonNegativeRowBoundForComposition(
            &tolerance_runtime, join->bound_expression_ids.back(),
            &tolerance, &tolerance_detail) ||
        tolerance > static_cast<std::uint64_t>(
                        std::numeric_limits<std::int64_t>::max())) {
      return refuse("SB_MODEL_ASOF_BINDING_REFUSED_V1",
                    tolerance_detail.empty()
                        ? "captured model-family ASOF tolerance is invalid"
                        : tolerance_detail);
    }
    asof_tolerance_ns = static_cast<std::int64_t>(tolerance);
    std::uint64_t uniqueness = 0;
    if (maximum_rows > 1 &&
        !CheckedMultiply(maximum_rows, maximum_rows - 1, &uniqueness)) {
      return refuse("SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                    "captured model-family ASOF uniqueness bound overflowed");
    }
    uniqueness /= 2;
    if (!CheckedAdd(maximum_pairs, uniqueness,
                    &asof_maximum_comparisons)) {
      return refuse("SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                    "captured model-family ASOF comparison bound overflowed");
    }
  }
  std::vector<LivePhysicalNodeProfile> profiles;
  for (const auto& leg : captured) {
    const auto source_memory_grant =
        leg.family_id == "relational"
            ? input.context.optimizer_memory_budget_bytes
            : leg.execution_request.input.maximum_memory_bytes;
    if (source_memory_grant == 0 ||
        source_memory_grant > input.context.optimizer_memory_budget_bytes) {
      return refuse("SB_MODEL_RESOURCE_MEMORY_REFUSED_V1",
                    "captured model-family source memory grant is invalid");
    }
    LivePhysicalNodeProfile profile;
    profile.logical_node_id = leg.logical_node_id;
    profile.implementation_id = leg.implementation_id;
    profile.capability_uuid = leg.capability_uuid;
    profile.logical_node_kind = leg.logical_node_kind;
    profile.physical_node_kind = leg.physical_node_kind;
    profile.transformation_rule_id = leg.transformation_rule_id;
    profile.estimated_rows = 1;
    profile.memory_bytes_required = source_memory_grant;
    profile.page_read_sequential_units = 1;
    profile.mga_visibility_checks_expected = 1;
    profile.storage_read_capable = true;
    profile.mga_visibility_capable = true;
    profile.residual_predicate_required = leg.family_id != "relational";
    profile.storage_recheck_required = leg.family_id != "relational";
    profile.compatibility_profile_id = leg.compatibility_profile_id;
    profile.model_family_id = leg.family_id + ".local.v1";
    profiles.push_back(std::move(profile));
  }
  const auto join_capability_uuid = DerivedCanonicalUuid(
      identity_scope, "captured-model-join." + join_component + ".capability");
  LivePhysicalNodeProfile join_profile;
  join_profile.logical_node_id = join->node_id;
  if (asof_join) {
    join_profile.implementation_id =
        asof_left_outer ? "join.asof.left.typed.v1"
                        : "join.asof.inner.typed.v1";
  } else {
    join_profile.implementation_id =
        lateral_join
            ? lateral_profile.implementation_id
            : "join." + join_component + ".3vl.nested.v1";
  }
  join_profile.capability_uuid = join_capability_uuid;
  join_profile.logical_node_kind =
      plan::CanonicalLogicalRelationalNodeKind::kJoin;
  join_profile.physical_node_kind = exec::PhysicalNodeKind::kJoin;
  if (asof_join) {
    exec::CanonicalTimeSeriesAsofJoinRequestV1 receipt;
    receipt.left_binding = asof_left_binding;
    receipt.right_binding = asof_right_binding;
    receipt.tolerance_ns = asof_tolerance_ns;
    receipt.left_outer = asof_left_outer;
    receipt.maximum_output_rows = maximum_rows;
    receipt.maximum_comparisons = asof_maximum_comparisons;
    asof_transformation_receipt =
        exec::CanonicalTimeSeriesAsofTransformationReceiptV1(receipt);
    join_profile.transformation_rule_id = asof_transformation_receipt;
  } else {
    join_profile.transformation_rule_id =
        lateral_join
            ? lateral_profile.transformation_id
            : "canonical.model-family.join." + join_component + ".v1";
  }
  join_profile.estimated_rows = asof_join ? maximum_rows : 1;
  join_profile.memory_bytes_required =
      input.context.optimizer_memory_budget_bytes;
  join_profile.minimum_input_count = 2;
  join_profile.maximum_input_count = 2;
  join_profile.runtime_peak_from_callback_batches = true;
  join_profile.model_family_id = "relational.local.v1";
  profiles.push_back(std::move(join_profile));
  if (!CompleteLiveRuntimeMemoryReceipts(&profiles)) {
    return refuse("QOW-DIAG-OPT-017-REFUSAL-V1",
                  "captured model-family memory receipts are incomplete");
  }
  const auto physical = PlanAndPublishLivePhysicalDag(
      planning_request, profiles, "captured-model-join.selected-plan",
      "captured model-family join", "relational.local.v1");
  if (!physical.ok || physical.physical_dag.nodes.size() != 3) {
    return refuse(physical.diagnostic_id.empty()
                      ? "QOW-DIAG-OPTIMIZER-PHYSICAL-PUBLICATION-V1"
                      : physical.diagnostic_id,
                  physical.detail.empty()
                      ? "captured model-family physical DAG was not published"
                      : physical.detail);
  }
  result.optimizer_selected = true;
  result.physical_dag_published = true;
  result.physical_node_count = physical.physical_dag.nodes.size();
  result.selected_plan_uuid = physical.physical_dag.selected_plan_uuid;

  CanonicalRelationalExpressionRowBinding predicate_binding;
  std::string predicate_detail;
  if (predicate_join) {
    std::vector<std::uint32_t> input_descriptors =
        sources[0]->output_descriptor_ids;
    input_descriptors.insert(input_descriptors.end(),
                             sources[1]->output_descriptor_ids.begin(),
                             sources[1]->output_descriptor_ids.end());
    if (!PrepareInputRowBindingForComposition(dag, join->bound_expression_ids.front(),
                                input_descriptors, &predicate_binding,
                                &predicate_detail)) {
      return refuse("SB_MODEL_JOIN_SEMANTIC_PRECONDITION_REFUSED_V1",
                    predicate_detail);
    }
  }
  exec::CanonicalHeapPhysicalRegistrationResult relational_registration;
  const auto relational_leg =
      std::ranges::find_if(captured, [](const auto& leg) {
        return leg.family_id == "relational";
      });
  if (relational_leg != captured.end()) {
    const auto relational_source = std::ranges::find_if(
        sources, [&](const auto* source) {
          return source->node_id == relational_leg->logical_node_id;
        });
    if (relational_source == sources.end()) {
      return refuse("SB_MODEL_COORDINATOR_LEG_FAILED_V1",
                    "relational composition source identity is absent");
    }
    auto heap_relational_dag =
        Rcp079OperatorLocalModelSourceDag(dag, **relational_source);
    heap_relational_dag.statement_timestamp.clear();
    exec::CanonicalHeapPhysicalDagDispatchRequest heap_request;
    heap_request.context = &input.context;
    heap_request.relational_dag = &heap_relational_dag;
    heap_request.physical_dag = physical.physical_dag;
    std::erase_if(heap_request.physical_dag.nodes, [&](const auto& node) {
      return node.relational_node_id != relational_leg->logical_node_id;
    });
    if (heap_request.physical_dag.nodes.size() != 1) {
      return refuse("SB_MODEL_COORDINATOR_LEG_FAILED_V1",
                    "selected relational composition node is absent");
    }
    heap_request.physical_dag.root_physical_node_id =
        heap_request.physical_dag.nodes.front().physical_node_id;
    heap_request.physical_dag.mga_statement_context.statement_timestamp.clear();
    for (auto& node : heap_request.physical_dag.nodes) {
      node.mga_statement_context.statement_timestamp.clear();
    }
    heap_request.maximum_scanned_row_versions =
        static_cast<std::size_t>(std::min<std::uint64_t>(
            std::min(input.context.optimizer_maximum_search_steps,
                     input.context.optimizer_maximum_candidate_count),
            std::numeric_limits<std::size_t>::max()));
    heap_request.maximum_decoded_bytes =
        static_cast<std::size_t>(std::min<std::uint64_t>(
            input.context.optimizer_memory_budget_bytes,
            std::numeric_limits<std::size_t>::max()));
    heap_request.maximum_output_rows = maximum_rows;
    heap_request.maximum_output_columns = maximum_columns;
    heap_request.maximum_output_cells = maximum_cells;
    heap_request.cancellation_requested =
        input.context.query_cancellation_requested
            ? input.context.query_cancellation_requested
            : std::function<bool()>([] { return false; });
    relational_registration =
        exec::BuildCanonicalHeapPhysicalRegistration(heap_request);
    if (!relational_registration.diagnostic.ok ||
        !relational_registration.registration.has_value()) {
      return refuse(
          relational_registration.diagnostic.diagnostic_code.empty()
              ? "SB_MODEL_COORDINATOR_LEG_FAILED_V1"
              : relational_registration.diagnostic.diagnostic_code,
          relational_registration.diagnostic.detail.empty()
              ? "relational composition executor is unavailable"
              : relational_registration.diagnostic.detail);
    }
    auto registration = std::move(*relational_registration.registration);
    auto execute_heap = std::move(registration.execute);
    registration.execute =
        [execute_heap = std::move(execute_heap)](
            const exec::TypedPhysicalNodeDag& selected_dag,
            const exec::PhysicalNodeRecord& selected_node,
            const std::vector<exec::CanonicalPhysicalDispatchInput>& inputs)
            mutable {
          auto heap_dag = selected_dag;
          std::erase_if(heap_dag.nodes, [&](const auto& node) {
            return node.physical_node_id != selected_node.physical_node_id;
          });
          exec::CanonicalPhysicalDispatchStepResult refused;
          if (heap_dag.nodes.size() != 1) {
            refused.diagnostic.ok = false;
            refused.diagnostic.diagnostic_code =
                "SB_MODEL_MGA_CONTEXT_MISMATCH_V1";
            refused.diagnostic.detail =
                "relational composition node identity changed";
            return refused;
          }
          heap_dag.root_physical_node_id =
              heap_dag.nodes.front().physical_node_id;
          heap_dag.mga_statement_context.statement_timestamp.clear();
          heap_dag.nodes.front().mga_statement_context.statement_timestamp
              .clear();
          heap_dag.nodes.front().dispatcher_callback_memory_limit_bytes =
              selected_node.dispatcher_callback_memory_limit_bytes;
          auto step = execute_heap(heap_dag, heap_dag.nodes.front(), inputs);
          if (step.diagnostic.ok) {
            step.mga_statement_context = selected_dag.mga_statement_context;
          }
          return step;
        };
    relational_registration.registration = std::move(registration);
  }
  api::CanonicalOptimizerSelectedExecutionRequest selected;
  selected.selected_physical_dag = physical.physical_dag;
  selected.pre_access_statistics_snapshot_uuid =
      physical.physical_dag.statistics_snapshot_uuid;
  selected.mga_authority =
      BuildCanonicalExecutionMgaAuthority(input.context,
                                          physical.physical_dag);
  selected.runtime_limits.maximum_rows_per_batch = maximum_rows;
  selected.runtime_limits.maximum_columns_per_batch = maximum_columns;
  selected.runtime_limits.maximum_cells_per_batch = maximum_cells;
  selected.runtime_limits.maximum_total_materialized_rows =
      maximum_total_rows;
  selected.runtime_limits.maximum_total_materialized_cells =
      maximum_total_cells;
  selected.cancellation_requested =
      input.context.query_cancellation_requested
          ? input.context.query_cancellation_requested
          : std::function<bool()>([] { return false; });
  if (relational_registration.registration.has_value()) {
    selected.available_executors.push_back(
        std::move(*relational_registration.registration));
  }
  std::unordered_set<std::string> registered_provider_implementations;
  for (const auto& leg : captured) {
    if (leg.family_id == "relational") continue;
    if (!registered_provider_implementations.insert(leg.implementation_id)
             .second) {
      continue;
    }
    std::vector<Rcp079CapturedModelLegV1> implementation_legs;
    for (const auto& candidate : captured) {
      if (candidate.implementation_id == leg.implementation_id) {
        if (candidate.physical_node_kind != leg.physical_node_kind ||
            candidate.capability_uuid != leg.capability_uuid) {
          return refuse("SB_MODEL_COORDINATOR_LEG_FAILED_V1",
                        "one provider implementation advertised conflicting "
                        "physical capabilities across captured legs");
        }
        implementation_legs.push_back(candidate);
      }
    }
    selected.available_executors.push_back(
        MakeRcp079CapturedModelLegRegistration(implementation_legs));
  }
  exec::CanonicalPhysicalExecutorRegistration join_registration;
  if (asof_join) {
    join_registration = MakeRcp079AsofRegistration(
        asof_left_outer ? "join.asof.left.typed.v1"
                        : "join.asof.inner.typed.v1",
        join_capability_uuid, asof_left_binding, asof_right_binding,
        asof_tolerance_ns, asof_left_outer, maximum_rows,
        asof_maximum_comparisons, asof_transformation_receipt,
        input.context, &input.context,
        &selected.mga_authority);
  } else if (lateral_join) {
    PreparedCorrelatedSubqueryRoot prepared;
    prepared.outer_row_count = maximum_rows;
    prepared.inner_row_count = maximum_rows;
    prepared.pair_count = static_cast<std::size_t>(maximum_pairs);
    prepared.output_row_bound = maximum_rows;
    CanonicalRelationalExpressionRuntimeServices lateral_services;
    lateral_services.comparison_evaluator =
        [context = input.context](const api::EngineTypedValue& left,
                                  const api::EngineTypedValue& right,
                                  int* comparison,
                                  std::string* diagnostic_id,
                                  std::string* refusal_detail) {
          return CompareCanonicalRelationalScalarsV1(
              context, left, right, comparison, diagnostic_id,
              refusal_detail);
        };
    join_registration = MakeLiveLateralSubqueryRegistration(
        prepared, lateral_profile, join_capability_uuid,
        std::move(lateral_services), input.context, true, &input.context,
        &selected.mga_authority);
  } else {
    CanonicalRelationalExpressionRuntimeServices predicate_services;
    predicate_services.comparison_evaluator =
        [context = input.context](const api::EngineTypedValue& left,
                                  const api::EngineTypedValue& right,
                                  int* comparison,
                                  std::string* diagnostic_id,
                                  std::string* refusal_detail) {
          return CompareCanonicalRelationalScalarsV1(
              context, left, right, comparison, diagnostic_id,
              refusal_detail);
        };
    BindCanonicalPersistedRowDescriptorAuthorityForMultilegV1(input.context,
                                                   &predicate_services);
    join_registration = MakeLiveJoinRegistration(
        "join." + join_component + ".3vl.nested.v1", join_capability_uuid, {},
        static_cast<std::size_t>(maximum_pairs), maximum_rows, join_kind,
        "captured model-family join", input.context, true,
        predicate_join ? join->bound_expression_ids.front() : 0,
        std::move(predicate_binding), dag, std::move(predicate_services));
  }
  if (exact_columnar_pair) {
    auto execute_join = std::move(join_registration.execute);
    join_registration.execute =
        [execute_join = std::move(execute_join), exact_join_columns](
            const exec::TypedPhysicalNodeDag& selected_dag,
            const exec::PhysicalNodeRecord& selected_node,
            const std::vector<exec::CanonicalPhysicalDispatchInput>& inputs)
            mutable {
          auto step = execute_join(selected_dag, selected_node, inputs);
          if (!step.diagnostic.ok) return step;
          if (!step.materialized_output_batch.has_value() ||
              step.materialized_output_batch->columns.size() !=
                  exact_join_columns.size() ||
              !std::ranges::equal(
                  step.materialized_output_batch->columns,
                  exact_join_columns,
                  [](const auto& actual, const auto& expected) {
                    return Rcp079ExactExecutorColumnV1(actual, expected);
                  })) {
            step.diagnostic.ok = false;
            step.diagnostic.diagnostic_code =
                "SB_MODEL_RESULT_DESCRIPTOR_SOURCE_BINDING_INVALID_V1";
            step.diagnostic.detail =
                "columnar join result descriptor carrier changed";
            step.materialized_output_batch.reset();
          }
          return step;
        };
  }
  selected.available_executors.push_back(
      WithMultilegResultDescriptorRebindingV1(
          std::move(join_registration),
          descriptor_preflight.publication_allocations,
          "captured model-family join",
          input.context.query_cancellation_requested
              ? input.context.query_cancellation_requested
              : std::function<bool()>([] { return false; })));
  selected.engine_execution_authorized = true;
  selected.result_publication_request.statement_uuid =
      input.context.statement_uuid.canonical;
  selected.result_publication_request.invocation_mode =
      exec::CanonicalResultInvocationMode::kDirect;
  selected.result_publication_request.execution_attempt_uuid =
      DerivedCanonicalUuid(identity_scope + ":" +
                               input.context.current_monotonic_ns,
                           "captured-model-join.execution-attempt");
  selected.result_publication_request.transaction_effect_evidence_uuid =
      DerivedCanonicalUuid(
          identity_scope + ":" +
              std::to_string(input.context.local_transaction_id) + ":" +
              std::to_string(
                  input.context.snapshot_visible_through_local_transaction_id),
          "captured-model-join.transaction-effect-unchanged");
  selected.result_publication_request.result_kind =
      exec::CanonicalResultKind::kRows;
  selected.result_publication_request.maximum_row_count = maximum_rows;
  std::vector<const api::RelationalOutputRecord*> root_outputs;
  for (const auto& output : dag.outputs) {
    if (output.relation_node_id == join->node_id) root_outputs.push_back(&output);
  }
  std::ranges::sort(root_outputs, {}, &api::RelationalOutputRecord::ordinal);
  if (root_outputs.size() != join->output_descriptor_ids.size()) {
    return refuse("SB_MODEL_TYPED_EXCHANGE_INVALID_V1",
                  "captured model-family root bindings are incomplete");
  }
  const auto descriptor_for = [&](const std::uint32_t descriptor_id) {
    return std::ranges::find_if(dag.descriptors, [&](const auto& descriptor) {
      return descriptor.descriptor_id == descriptor_id;
    });
  };
  const auto left_width = sources[0]->output_descriptor_ids.size();
  std::vector<const api::RelationalOutputRecord*> exact_source_outputs;
  if (exact_columnar_pair) {
    for (std::size_t source_ordinal = 0; source_ordinal < sources.size();
         ++source_ordinal) {
      if (left_only && source_ordinal != 0) break;
      std::vector<const api::RelationalOutputRecord*> outputs;
      for (const auto& output : dag.outputs) {
        if (output.relation_node_id == sources[source_ordinal]->node_id) {
          outputs.push_back(&output);
        }
      }
      std::ranges::sort(outputs, {}, &api::RelationalOutputRecord::ordinal);
      exact_source_outputs.insert(exact_source_outputs.end(), outputs.begin(),
                                  outputs.end());
    }
    if (exact_source_outputs.size() != root_outputs.size() ||
        exact_join_columns.size() != root_outputs.size()) {
      return refuse("SB_MODEL_RESULT_DESCRIPTOR_SOURCE_BINDING_INVALID_V1",
                    "columnar join source output closure changed");
    }
  }
  for (std::size_t ordinal = 0; ordinal < root_outputs.size(); ++ordinal) {
    const auto descriptor = descriptor_for(root_outputs[ordinal]->descriptor_id);
    if (descriptor == dag.descriptors.end() ||
        root_outputs[ordinal]->ordinal != ordinal ||
        !root_outputs[ordinal]->visible ||
        root_outputs[ordinal]->descriptor_id !=
            join->output_descriptor_ids[ordinal] ||
        (exact_columnar_pair &&
         (root_outputs[ordinal]->expression_id !=
              exact_source_outputs[ordinal]->expression_id ||
          root_outputs[ordinal]->output_name_utf8 !=
              exact_source_outputs[ordinal]->output_name_utf8 ||
          root_outputs[ordinal]->descriptor_id !=
              exact_source_outputs[ordinal]->descriptor_id))) {
      return refuse("SB_MODEL_TYPED_EXCHANGE_INVALID_V1",
                    "captured model-family root descriptor is not source-exact");
    }
    const bool null_extended =
        ((join_kind == exec::CanonicalAcceptedJoinKind::kRightOuter ||
          join_kind == exec::CanonicalAcceptedJoinKind::kFullOuter) &&
         ordinal < left_width) ||
        ((join_kind == exec::CanonicalAcceptedJoinKind::kLeftOuter ||
          join_kind == exec::CanonicalAcceptedJoinKind::kFullOuter) &&
         ordinal >= left_width);
    exec::CanonicalResultColumnDescriptor published;
    published.ordinal = static_cast<std::uint32_t>(ordinal);
    published.name_utf8 = root_outputs[ordinal]->output_name_utf8;
    const auto& allocation =
        descriptor_preflight.publication_allocations[ordinal];
    const auto exact_type_uuid =
        exact_columnar_pair
            ? Rcp079DescriptorField(
                  exact_join_columns[ordinal].descriptor.encoded_descriptor,
                  "type_uuid")
            : std::optional<std::string>{};
    if (allocation.demand.nullable !=
            (null_extended ||
             descriptor->nullability ==
                 api::RelationalNullability::kNullable) ||
        (!allocation.demand.derived &&
         (allocation.descriptor_uuid != descriptor->descriptor_uuid ||
          allocation.type_uuid != descriptor->type_uuid)) ||
        (exact_columnar_pair &&
         (!exact_type_uuid.has_value() ||
          allocation.descriptor_uuid !=
              exact_join_columns[ordinal]
                  .descriptor.descriptor_uuid.canonical ||
          allocation.type_uuid != *exact_type_uuid ||
          allocation.demand.nullable !=
              exact_join_columns[ordinal].nullable))) {
      return refuse("SB_MODEL_RESULT_DESCRIPTOR_SOURCE_BINDING_INVALID_V1",
                    "captured model-family publication allocation changed");
    }
    published.descriptor_uuid = allocation.descriptor_uuid;
    published.type_uuid = allocation.type_uuid;
    published.nullability = allocation.demand.nullable
                                ? exec::CanonicalResultNullability::kNullable
                                : exec::CanonicalResultNullability::kNonNull;
    if (!allocation.demand.derived) {
      published.collation_uuid = descriptor->collation_uuid;
      published.timezone_profile_id = descriptor->timezone_profile_id;
    }
    selected.result_publication_request.column_bindings.push_back(
        {ordinal, true, std::move(published)});
  }
  if (!complete_object_closure_authorized()) {
    return refuse("SB_MODEL_SECURITY_ADMISSION_REFUSED_V1",
                  "captured model-family SELECT closure changed before execution");
  }
  const auto execution = ExecuteSelectedCanonicalObjectFreeDag(
      input.context, selected, physical.ordinary_runtime_memory_receipts);
  if (!execution.accepted || !execution.exact_selected_nodes_executed ||
      !execution.causal_counters_attached ||
      !execution.canonical_result_published || !execution.data_access_observed ||
      !execution.runtime_actuals.accepted ||
      execution.dispatch.executed_steps.size() != 3 ||
      !execution.issues.empty()) {
    const bool cancellation_observed =
        execution.cancellation_observed ||
        execution.dispatch.diagnostic.diagnostic_code ==
            "SB_MODEL_EXECUTION_CANCELLED_V1" ||
        (!execution.issues.empty() &&
         execution.issues.front().diagnostic_id ==
             "SB_MODEL_EXECUTION_CANCELLED_V1");
    return refuse(
        cancellation_observed
            ? "SB_MODEL_EXECUTION_CANCELLED_V1"
            : execution.issues.empty()
                  ? "SB_MODEL_COORDINATOR_LEG_FAILED_V1"
                  : execution.issues.front().diagnostic_id,
        cancellation_observed &&
                !execution.dispatch.diagnostic.detail.empty()
            ? execution.dispatch.diagnostic.detail
            : execution.issues.empty()
                  ? "captured model-family execution did not complete"
                  : execution.issues.front().field_id);
  }
  result.physical_dag_executed = true;
  result.runtime_actuals_attached = true;
  result.canonical_result_published = true;
  result.canonical_result_column_count =
      execution.result_publication.envelope.column_descriptors.size();
  result.canonical_result_row_count =
      execution.result_publication.row_stream.rows.size();
  result.canonical_result_bytes =
      execution.result_publication.canonical_envelope_bytes;
  result.api_result = SuccessfulApiResult(planning_request, execution);
  result.api_result.evidence.push_back(
      {"canonical.model_composition",
       left_family + "_TO_" + right_family + "_ONE_ROOT_V1"});
  result.api_result.evidence.push_back(
      {"canonical.model_join_form", join_form});
  result.api_result.evidence.push_back(
      {"canonical.model_join_condition_form", condition_form});
  result.api_result.evidence.push_back(
      {"canonical.model_join_left_provider_route",
       pair_admission.left_provider_route_id});
  result.api_result.evidence.push_back(
      {"canonical.model_join_right_provider_route",
       pair_admission.right_provider_route_id});
  result.api_result.evidence.push_back(
      {"canonical.model_join_consumer_route",
       pair_admission.relational_consumer_route_id});
  result.api_result.evidence.push_back(
      {"canonical.model_join_condition_route",
       pair_admission.condition_lowering_route_id});
  return result;
}




}  // namespace scratchbird::engine::sblr
