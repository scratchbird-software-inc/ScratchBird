// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "canonical_query_multileg_descriptor_rebinding.hpp"
#include "canonical_query_runtime_memory_support.hpp"
#include "catalog/column_metadata_codec.hpp"
#include "core/uuid/uuid.hpp"

#include <exception>
#include <utility>

// SB_ENGINE_CANONICAL_QUERY_MULTILEG_DESCRIPTOR_REBINDING_AUTHORITY
namespace scratchbird::engine::sblr {
namespace api = scratchbird::engine::internal_api;
namespace exec = scratchbird::engine::executor;
namespace opt = scratchbird::engine::optimizer;

std::optional<api::EngineUuid> Rcp079DescriptorIdentity(
    const api::EngineDescriptor& descriptor, std::string_view key) {
  api::CatalogColumnMetadata fields;
  if (!api::AdmitCatalogColumnMetadata(descriptor.encoded_descriptor, &fields)) return std::nullopt;
  const auto found = fields.identities.find(std::string(key));
  if (key == "type_uuid" && !descriptor.type_uuid.is_nil()) {
    if (!core::uuid::IsEngineIdentityUuid(descriptor.type_uuid) ||
        (found != fields.identities.end() && found->second != descriptor.type_uuid)) return std::nullopt;
    return descriptor.type_uuid;
  }
  if (found == fields.identities.end() || !core::uuid::IsEngineIdentityUuid(found->second)) return std::nullopt;
  return found->second;
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
                    ? api::EngineUuid{}
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
              std::string("type_uuid=").size() + allocation.type_uuid.bytes.size() +
              std::string(";nullability=").size() +
              std::string(allocation.demand.nullable ? "nullable"
                                                     : "non_null").size();
          std::uint64_t descriptor_bytes = 0;
          if (!CheckedAdd(allocation.descriptor_uuid.bytes.size(),
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
            rebound.descriptor_uuid = allocation.descriptor_uuid;
            rebound.descriptor_kind = "scalar";
            rebound.canonical_type_name =
                allocation.demand.canonical_type_name;
            rebound.encoded_descriptor =
                std::string("nullability=") +
                (allocation.demand.nullable ? "nullable" : "non_null");
            rebound.type_uuid = allocation.type_uuid;
            column.descriptor = std::move(rebound);
            column.nullable = allocation.demand.nullable;
          } else if (column.descriptor.descriptor_uuid !=
                         allocation.descriptor_uuid ||
                     allocation.type_uuid.is_nil() ||
                     Rcp079DescriptorIdentity(
                         column.descriptor,
                         "type_uuid") !=
                         std::optional<api::EngineUuid>(allocation.type_uuid) ||
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
                    : api::EngineUuid{};
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

}  // namespace scratchbird::engine::sblr
