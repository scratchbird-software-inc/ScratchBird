// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "canonical_query_physical_registration.hpp"

#include "canonical_query_descriptor_support.hpp"
#include "canonical_query_runtime_memory_support.hpp"
#if !defined(SCRATCHBIRD_QOW_QUERY_ROUTE_CONTRACT_ONLY)
#include "transaction/transaction_api.hpp"
#endif

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <ranges>
#include <string>
#include <utility>
#include <vector>

namespace scratchbird::engine::sblr {

namespace api = scratchbird::engine::internal_api;

namespace {

constexpr std::string_view kValuesImplementationId =
    "values.materialize.canonical.v1";

#if defined(SCRATCHBIRD_QOW_QUERY_ROUTE_CONTRACT_ONLY)
// Deterministic closure-test seam only. A one-node selected execution resolves
// current MGA authority at selected-entry, dispatch-entry, node pre/post,
// dispatch-root, actuals entry/result, and immediate pre-result (resolution 8).
// Production builds have no mutable seam and always resolve through the durable
// transaction inventory below.
thread_local bool g_contract_pre_result_revocation_armed = false;
thread_local std::size_t g_contract_revalidation_resolution_count = 0;
#endif

}  // namespace

#if !defined(SCRATCHBIRD_QOW_QUERY_ROUTE_CONTRACT_ONLY)
exec::PhysicalMgaStatementContext PhysicalMgaContextFromResolvedSnapshot(
    const api::EngineRequestContext& context,
    const scratchbird::transaction::mga::SnapshotVectorDescriptor& descriptor) {
  exec::PhysicalMgaStatementContext expected;
  expected.statement_uuid = context.statement_uuid.canonical;
  expected.owning_transaction_uuid = context.transaction_uuid.canonical;
  expected.statement_snapshot_uuid = context.statement_snapshot_uuid.canonical;
  expected.statement_metadata_snapshot_uuid =
      context.statement_metadata_snapshot_uuid.canonical;
  expected.owning_local_transaction_id = descriptor.owning_transaction.value;
  expected.visible_committed_high_watermark =
      descriptor.visible_committed_high_watermark;
  expected.oldest_active_transaction_id =
      descriptor.oldest_active_transaction.value;
  expected.oldest_interesting_transaction_id =
      descriptor.oldest_interesting_transaction.value;
  expected.oldest_snapshot_transaction_id =
      descriptor.oldest_snapshot_transaction.value;
  expected.retention_horizon_transaction_id =
      descriptor.retention_horizon_transaction.value;
  expected.active_excluded_local_transaction_ids =
      descriptor.active_excluded_local_transaction_ids;
  expected.in_doubt_excluded_local_transaction_ids =
      descriptor.in_doubt_excluded_local_transaction_ids;
  expected.snapshot_kind =
      scratchbird::transaction::mga::SnapshotVectorKindName(
          descriptor.snapshot_kind);
  expected.publication_inventory_next_local_transaction_id =
      descriptor.publication_inventory_next_local_transaction_id;
  expected.inventory_authoritative = descriptor.inventory_authoritative;
  expected.complete = descriptor.complete;
  expected.current = true;
  return expected;
}
#endif

// SEARCH_KEY: SB_ENGINE_CANONICAL_QUERY_PHYSICAL_REGISTRATION_AUTHORITY
bool InvokeLiveSortCancellationProbe(const void* context) {
  if (context == nullptr) return false;
  return (*static_cast<const std::function<bool()>*>(context))();
}

const exec::PhysicalAdmissionEvidence* FindLiveCancellationPolicy(
    const exec::TypedPhysicalNodeDag& dag) {
  const exec::PhysicalAdmissionEvidence* policy = nullptr;
  for (const auto& evidence : dag.admission_evidence) {
    if (evidence.stage != exec::PhysicalAdmissionStage::kPolicyCapability) {
      continue;
    }
    if (policy != nullptr || evidence.evidence_uuid.empty()) return nullptr;
    policy = &evidence;
  }
  return policy;
}

void BindLiveCancellationFailure(
    exec::DescriptorRuntimeDiagnostic diagnostic,
    const exec::PhysicalAdmissionEvidence* cancellation_policy,
    exec::CanonicalPhysicalDispatchStepResult* step) {
  if (step == nullptr) return;
  step->diagnostic = std::move(diagnostic);
  if (step->diagnostic.diagnostic_code ==
      "SB_MODEL_EXECUTION_CANCELLED_V1") {
    step->diagnostic.diagnostic_code =
        "QOW-DIAG-QRY-004-PHYSICAL-DISPATCH-CANCELLED-V1";
    step->cancellation_observed = true;
    step->transient_state_cleanup_proven = true;
    step->cancellation_evidence_uuid =
        cancellation_policy == nullptr
            ? std::string{}
            : cancellation_policy->evidence_uuid;
  } else if (step->diagnostic.diagnostic_code ==
             "SB_MODEL_COORDINATOR_LEG_FAILED_V1") {
    step->diagnostic.diagnostic_code =
        "QOW-DIAG-QRY-004-PHYSICAL-CANCELLATION-PROBE-V1";
    step->transient_state_cleanup_proven = true;
  }
}

exec::CanonicalExecutionMgaAuthority BuildCanonicalExecutionMgaAuthority(
    const api::EngineRequestContext& context,
    const exec::TypedPhysicalNodeDag& physical_dag) {
  exec::CanonicalExecutionMgaAuthority authority;
  authority.statement_context = physical_dag.mga_statement_context;
#if defined(SCRATCHBIRD_QOW_QUERY_ROUTE_CONTRACT_ONLY)
  authority.origin = exec::CanonicalMgaAuthorityOrigin::kClosureTestSeam;
  authority.resolve_current = [expected = authority.statement_context]() {
    exec::CanonicalMgaCurrentResolution resolved;
    resolved.statement_context = expected;
    ++g_contract_revalidation_resolution_count;
    if (g_contract_pre_result_revocation_armed &&
        g_contract_revalidation_resolution_count == 8) {
      resolved.statement_context.current = false;
      g_contract_pre_result_revocation_armed = false;
    }
    return resolved;
  };
#else
  authority.origin =
      exec::CanonicalMgaAuthorityOrigin::kEngineTransactionInventory;
  authority.resolve_current =
      [context, statement_timestamp =
                    authority.statement_context.statement_timestamp]() {
    exec::CanonicalMgaCurrentResolution current;
    if (!statement_timestamp.empty() &&
        context.statement_timestamp != statement_timestamp) {
      current.diagnostic.ok = false;
      current.diagnostic.diagnostic_code =
          "SB_MODEL_KEY_VALUE_STATEMENT_TIMESTAMP_INVALID_V1";
      current.diagnostic.detail =
          "engine statement timestamp differs from selected MGA authority";
      return current;
    }
    api::EngineResolveStatementSnapshotRequest resolve_request;
    resolve_request.context = context;
    const auto resolved = api::EngineResolveStatementSnapshot(resolve_request);
    if (!resolved.ok) {
      current.diagnostic.ok = false;
      current.diagnostic.diagnostic_code =
          "SB_DIAG_MGA_READ_SNAPSHOT_MISSING";
      current.diagnostic.detail =
          "statement snapshot is unknown, revoked, stale, or not current";
      return current;
    }
    current.statement_context =
        PhysicalMgaContextFromResolvedSnapshot(context,
                                               resolved.snapshot_vector);
    current.statement_context.statement_timestamp = statement_timestamp;
    return current;
  };
#endif
  return authority;
}

bool BuildOperatorLocalPhysicalDag(
    const exec::TypedPhysicalNodeDag& dag,
    const std::uint64_t root_physical_node_id,
    exec::TypedPhysicalNodeDag* operator_dag,
    std::string* detail) {
  if (operator_dag == nullptr || detail == nullptr ||
      root_physical_node_id == 0) {
    if (detail != nullptr) {
      *detail = "operator-local physical DAG request is incomplete";
    }
    return false;
  }
  std::vector<std::uint8_t> retained(dag.nodes.size(), 0);
  std::vector<std::size_t> pending;
  pending.reserve(dag.nodes.size());
  const auto root = std::ranges::find_if(
      dag.nodes, [&](const auto& candidate) {
        return candidate.physical_node_id == root_physical_node_id;
      });
  if (root == dag.nodes.end()) {
    *operator_dag = {};
    *detail = "operator-local physical DAG root is unresolved";
    return false;
  }
  const auto root_index =
      static_cast<std::size_t>(std::distance(dag.nodes.begin(), root));
  retained[root_index] = 1;
  pending.push_back(root_index);
  std::size_t retained_count = 1;
  while (!pending.empty()) {
    const auto node_index = pending.back();
    pending.pop_back();
    for (const auto input_id :
         dag.nodes[node_index].input_physical_node_ids) {
      const auto found = std::ranges::find_if(
          dag.nodes, [&](const auto& candidate) {
            return candidate.physical_node_id == input_id;
          });
      if (found == dag.nodes.end()) {
        *operator_dag = {};
        *detail = "operator-local physical DAG input is unresolved";
        return false;
      }
      const auto input_index = static_cast<std::size_t>(
          std::distance(dag.nodes.begin(), found));
      if (retained[input_index] != 0) continue;
      retained[input_index] = 1;
      ++retained_count;
      pending.push_back(input_index);
    }
  }
  exec::TypedPhysicalNodeDag local;
  local.abi_version = dag.abi_version;
  local.selected_plan_uuid = dag.selected_plan_uuid;
  local.root_physical_node_id = root_physical_node_id;
  local.local_transaction_id = dag.local_transaction_id;
  local.statement_snapshot_id = dag.statement_snapshot_id;
  local.mga_statement_context = dag.mga_statement_context;
  local.admission_evidence = dag.admission_evidence;
  local.nodes.reserve(retained_count);
  for (std::size_t index = 0; index < dag.nodes.size(); ++index) {
    if (retained[index] != 0) {
      local.nodes.push_back(dag.nodes[index]);
    }
  }
  if (local.nodes.size() != retained_count) {
    *operator_dag = {};
    *detail = "operator-local physical DAG node identity is not unique";
    return false;
  }
  local.bound_sblr_tree_uuid = dag.bound_sblr_tree_uuid;
  local.catalog_epoch_uuid = dag.catalog_epoch_uuid;
  local.security_context_uuid = dag.security_context_uuid;
  local.capability_snapshot_uuid = dag.capability_snapshot_uuid;
  local.resource_snapshot_uuid = dag.resource_snapshot_uuid;
  local.statistics_snapshot_uuid = dag.statistics_snapshot_uuid;
  local.route_snapshot_uuid = dag.route_snapshot_uuid;
  local.catalog_generation = dag.catalog_generation;
  local.security_epoch = dag.security_epoch;
  local.policy_epoch = dag.policy_epoch;
  local.resource_epoch = dag.resource_epoch;
  local.statistics_generation = dag.statistics_generation;
  local.route_epoch = dag.route_epoch;
  local.route_generation = dag.route_generation;
  local.memory_budget_bytes = dag.memory_budget_bytes;
  local.spill_allowed = dag.spill_allowed;
  local.optimizer_published = dag.optimizer_published;
  local.immutable_node_identity_validated =
      dag.immutable_node_identity_validated;
  local.capability_validated_before_access =
      dag.capability_validated_before_access;
  local.data_access_observed = dag.data_access_observed;
  local.parser_execution_authority_claimed =
      dag.parser_execution_authority_claimed;
  local.transaction_finality_authority_claimed =
      dag.transaction_finality_authority_claimed;
  local.publication_contract_version = dag.publication_contract_version;
  local.selected_plan_signature = dag.selected_plan_signature;
  local.selected_scalar_score = dag.selected_scalar_score;
  local.published_node_count = dag.published_node_count;
  local.first_causal_counter_id = dag.first_causal_counter_id;
  local.complete_cost_vectors_retained = dag.complete_cost_vectors_retained;
  local.descriptor_contract_validated = dag.descriptor_contract_validated;
  local.property_contract_validated = dag.property_contract_validated;
  local.dependency_contract_validated = dag.dependency_contract_validated;
  local.resource_contract_validated = dag.resource_contract_validated;
  local.mga_contract_validated = dag.mga_contract_validated;
  local.causal_identity_validated = dag.causal_identity_validated;
  *operator_dag = std::move(local);
  detail->clear();
  return true;
}

std::optional<std::uint64_t> BoundOperatorLocalPhysicalDagCopyMemoryBytes(
    const exec::TypedPhysicalNodeDag& dag) {
  std::uint64_t bytes = sizeof(exec::TypedPhysicalNodeDag);
  const auto add = [&](const std::uint64_t value) {
    return CheckedAdd(bytes, value, &bytes);
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
    return value.size() != std::numeric_limits<std::uint64_t>::max() &&
           add(static_cast<std::uint64_t>(value.size()) + 1);
  };
  const auto add_context = [&](const exec::PhysicalMgaStatementContext& value) {
    return add_string(value.statement_uuid) &&
           add_string(value.owning_transaction_uuid) &&
           add_string(value.statement_snapshot_uuid) &&
           add_string(value.statement_metadata_snapshot_uuid) &&
           add_array(value.active_excluded_local_transaction_ids.size(),
                     sizeof(std::uint64_t)) &&
           add_array(value.in_doubt_excluded_local_transaction_ids.size(),
                     sizeof(std::uint64_t)) &&
           add_string(value.snapshot_kind) &&
           add_string(value.statement_timestamp);
  };
  const auto add_string_vector = [&](const std::vector<std::string>& values) {
    if (!add_array(values.size(), sizeof(std::string))) return false;
    return std::ranges::all_of(values, add_string);
  };
  if (!add_string(dag.selected_plan_uuid) ||
      !add_context(dag.mga_statement_context) ||
      !add_array(dag.admission_evidence.size(),
                 sizeof(exec::PhysicalAdmissionEvidence)) ||
      !add_array(dag.nodes.size(), sizeof(exec::PhysicalNodeRecord)) ||
      !add_string(dag.bound_sblr_tree_uuid) ||
      !add_string(dag.catalog_epoch_uuid) ||
      !add_string(dag.security_context_uuid) ||
      !add_string(dag.capability_snapshot_uuid) ||
      !add_string(dag.resource_snapshot_uuid) ||
      !add_string(dag.statistics_snapshot_uuid) ||
      !add_string(dag.route_snapshot_uuid) ||
      !add_string(dag.selected_plan_signature) ||
      // Temporary reachability vectors used before the bounded DAG copy.
      !add_array(dag.nodes.size(), sizeof(std::uint8_t)) ||
      !add_array(dag.nodes.size(), sizeof(std::size_t))) {
    return std::nullopt;
  }
  for (const auto& evidence : dag.admission_evidence) {
    if (!add_string(evidence.evidence_uuid)) return std::nullopt;
  }
  for (const auto& node : dag.nodes) {
    if (!add_string(node.implementation_id) ||
        !add_array(node.input_physical_node_ids.size(),
                   sizeof(std::uint64_t)) ||
        !add_array(node.output_descriptor_ids.size(),
                   sizeof(std::uint32_t)) ||
        !add_string(node.selected_alternative_uuid) ||
        !add_string(node.executor_capability_uuid) ||
        !add_string(node.cost_vector_uuid) ||
        !add_string_vector(node.required_property_uuids) ||
        !add_string_vector(node.delivered_property_uuids) ||
        !add_context(node.mga_statement_context) ||
        !add_string(node.logical_semantic_variant_id) ||
        !add_string(node.transformation_uuid) ||
        !add_string(node.transformation_rule_id) ||
        !add_string_vector(node.enforced_property_uuids) ||
        !add_string(node.retained_cost.cost_vector_uuid) ||
        !add_string(node.retained_cost.calibration_profile_uuid) ||
        !add_string(node.retained_cost.scalarization_policy_id)) {
      return std::nullopt;
    }
  }
  return bytes;
}

bool RebindOperatorLocalPhysicalMemoryGrant(
    exec::PhysicalNodeRecord* node,
    exec::TypedPhysicalNodeDag* dag,
    const std::uint64_t rebound_memory_bytes,
    std::string* detail) {
  if (node == nullptr || dag == nullptr || detail == nullptr ||
      rebound_memory_bytes == 0 ||
      node->retained_cost.memory_bytes_required !=
          node->memory_bytes_required ||
      dag->selected_scalar_score < node->retained_cost.scalar_score) {
    if (detail != nullptr) {
      *detail = "operator-local retained memory cost cannot be rebound";
    }
    return false;
  }
  const auto previous_memory_bytes = node->memory_bytes_required;
  const auto previous_scalar_score = node->retained_cost.scalar_score;
  auto rebound_cost = node->retained_cost;
  rebound_cost.memory_bytes_required = rebound_memory_bytes;
  if (!rebound_cost.complete_dimension_vector &&
      rebound_cost.memory_grant_units == previous_memory_bytes) {
    rebound_cost.memory_grant_units = rebound_memory_bytes;
  }
  std::uint64_t rebound_scalar_score = 0;
  if (!exec::ComputePhysicalCostVectorScalarScore(
          rebound_cost, &rebound_scalar_score)) {
    *detail = "operator-local rebound cost scalar overflows";
    return false;
  }
  // The complete Core dimensions describe the selected alternative's cost;
  // a smaller callback-local grant changes only the resource envelope. Legacy
  // vectors included the envelope directly in their scalar and therefore need
  // a scalar rebind until every old producer publishes a complete vector.
  if (rebound_cost.complete_dimension_vector &&
      rebound_scalar_score != previous_scalar_score) {
    *detail = "operator-local complete cost changed with its grant envelope";
    return false;
  }
  const auto unbound_dag_score = dag->selected_scalar_score -
                                 previous_scalar_score;
  std::uint64_t rebound_dag_score = 0;
  if (!CheckedAdd(unbound_dag_score, rebound_scalar_score,
                  &rebound_dag_score)) {
    *detail = "operator-local rebound selected cost scalar overflows";
    return false;
  }
  rebound_cost.scalar_score = rebound_scalar_score;
  node->memory_bytes_required = rebound_memory_bytes;
  node->retained_cost = std::move(rebound_cost);
  dag->selected_scalar_score = rebound_dag_score;
  detail->clear();
  return true;
}

bool BuildStrictUnaryOperatorLocalPhysicalDag(
    const exec::TypedPhysicalNodeDag& dag,
    const exec::PhysicalNodeRecord& node,
    const exec::DescriptorBatch& input_batch,
    const std::uint64_t maximum_additional_batch_copies,
    const std::uint64_t auxiliary_memory_bytes,
    exec::TypedPhysicalNodeDag* operator_dag,
    std::size_t* callback_memory_bound,
    std::string* detail) {
  if (operator_dag == nullptr || callback_memory_bound == nullptr ||
      detail == nullptr || maximum_additional_batch_copies == 0) {
    if (detail != nullptr) {
      *detail = "strict unary preflight request is incomplete";
    }
    return false;
  }
  const auto selected_bound = SelectedNodeAggregateMemoryBound(dag, node);
  const auto dag_copy_bound =
      BoundOperatorLocalPhysicalDagCopyMemoryBytes(dag);
  std::uint64_t dag_validation_scratch_bytes = 0;
  std::uint64_t input_live_bytes = 0;
  bool cancellation_observed = false;
  bool cancellation_probe_failed = false;
  const auto validation_scratch =
      exec::BoundDescriptorBatchValidationScratchMemoryBytes(
          input_batch, {}, &cancellation_observed,
          &cancellation_probe_failed);
  std::uint64_t additional_batch_bytes = 0;
  std::uint64_t callback_carrier_bytes =
      sizeof(exec::CanonicalPhysicalDispatchStepResult) + 64 * 1024;
  const auto add_string = [&](const std::string& value) {
    return value.capacity() != std::numeric_limits<std::size_t>::max() &&
           CheckedAdd(callback_carrier_bytes,
                      static_cast<std::uint64_t>(value.capacity()) + 1,
                      &callback_carrier_bytes);
  };
  std::uint64_t context_array_bytes = 0;
  const auto add_context_array = [&](const std::size_t count,
                                     const std::size_t width) {
    return CheckedMultiply(count, width, &context_array_bytes) &&
           CheckedAdd(callback_carrier_bytes, context_array_bytes,
                      &callback_carrier_bytes);
  };
  const auto& context = dag.mga_statement_context;
  const bool callback_carriers_bounded =
      add_string(dag.selected_plan_uuid) &&
      add_string(dag.selected_plan_uuid) &&
      add_string(context.statement_uuid) &&
      add_string(context.owning_transaction_uuid) &&
      add_string(context.statement_snapshot_uuid) &&
      add_string(context.statement_metadata_snapshot_uuid) &&
      add_context_array(
          context.active_excluded_local_transaction_ids.capacity(),
          sizeof(std::uint64_t)) &&
      add_context_array(
          context.in_doubt_excluded_local_transaction_ids.capacity(),
          sizeof(std::uint64_t)) &&
      add_string(context.snapshot_kind) &&
      add_string(context.statement_timestamp) &&
      add_string(context.statement_uuid) &&
      add_string(context.owning_transaction_uuid) &&
      add_string(context.statement_snapshot_uuid) &&
      add_string(context.statement_metadata_snapshot_uuid) &&
      add_context_array(
          context.active_excluded_local_transaction_ids.capacity(),
          sizeof(std::uint64_t)) &&
      add_context_array(
          context.in_doubt_excluded_local_transaction_ids.capacity(),
          sizeof(std::uint64_t)) &&
      add_string(context.snapshot_kind) &&
      add_string(context.statement_timestamp) &&
      add_context_array(node.output_descriptor_ids.size(),
                        sizeof(std::uint32_t));
  std::uint64_t coexistence_bytes = 0;
  const bool preflight_ok =
      selected_bound.has_value() && dag_copy_bound.has_value() &&
      validation_scratch.has_value() && !cancellation_observed &&
      !cancellation_probe_failed && callback_carriers_bounded &&
      // Revalidation builds node/property/descriptor hash domains and a
      // canonical-node vector. Thirty-two copies of the complete DAG carrier
      // dominate their entries, buckets, allocator headers, and copied UUID
      // strings; the fixed callback allowance covers the container objects.
      CheckedMultiply(*dag_copy_bound, 32,
                      &dag_validation_scratch_bytes) &&
      BoundDescriptorBatchLiveMemoryBytes(input_batch, &input_live_bytes) &&
      CheckedMultiply(input_live_bytes, maximum_additional_batch_copies,
                      &additional_batch_bytes) &&
      CheckedAdd(input_live_bytes, additional_batch_bytes,
                 &coexistence_bytes) &&
      CheckedAdd(coexistence_bytes, *dag_copy_bound,
                 &coexistence_bytes) &&
      CheckedAdd(coexistence_bytes, dag_validation_scratch_bytes,
                 &coexistence_bytes) &&
      CheckedAdd(coexistence_bytes, *validation_scratch,
                 &coexistence_bytes) &&
      CheckedAdd(coexistence_bytes, auxiliary_memory_bytes,
                 &coexistence_bytes) &&
      CheckedAdd(coexistence_bytes, callback_carrier_bytes,
                 &coexistence_bytes) &&
      coexistence_bytes <= *selected_bound;
  if (!preflight_ok) {
    *operator_dag = {};
    *callback_memory_bound = 0;
    *detail =
        "retained input, output, validation, or operator-local state exceeds "
        "the dispatcher callback memory allowance";
    return false;
  }
  if (!BuildOperatorLocalPhysicalDag(
          dag, node.physical_node_id, operator_dag, detail)) {
    *callback_memory_bound = 0;
    return false;
  }
  const auto root = std::ranges::find_if(
      operator_dag->nodes, [&](const auto& candidate) {
        return candidate.physical_node_id == node.physical_node_id;
      });
  if (root == operator_dag->nodes.end()) {
    *operator_dag = {};
    *callback_memory_bound = 0;
    *detail = "operator-local unary root is absent";
    return false;
  }
  if (root->memory_bytes_required < *selected_bound) {
    *operator_dag = {};
    *callback_memory_bound = 0;
    *detail = "dispatcher callback memory exceeds the published unary grant";
    return false;
  }
  if (!RebindOperatorLocalPhysicalMemoryGrant(
          &*root, operator_dag, *selected_bound, detail)) {
    *operator_dag = {};
    *callback_memory_bound = 0;
    return false;
  }
  *callback_memory_bound = *selected_bound;
  detail->clear();
  return true;
}

bool BuildStrictBinaryOperatorLocalPhysicalDag(
    const exec::TypedPhysicalNodeDag& dag,
    const exec::PhysicalNodeRecord& node,
    const exec::DescriptorBatch& left_input_batch,
    const exec::DescriptorBatch& right_input_batch,
    const std::uint64_t auxiliary_memory_bytes,
    exec::TypedPhysicalNodeDag* operator_dag,
    std::size_t* callback_memory_bound,
    std::string* detail) {
  if (operator_dag == nullptr || callback_memory_bound == nullptr ||
      detail == nullptr) {
    if (detail != nullptr) {
      *detail = "strict binary preflight request is incomplete";
    }
    return false;
  }
  const auto selected_bound = SelectedNodeAggregateMemoryBound(dag, node);
  const auto dag_copy_bound =
      BoundOperatorLocalPhysicalDagCopyMemoryBytes(dag);
  bool cancellation_observed = false;
  bool cancellation_probe_failed = false;
  const auto left_validation_scratch =
      exec::BoundDescriptorBatchValidationScratchMemoryBytes(
          left_input_batch, {}, &cancellation_observed,
          &cancellation_probe_failed);
  const auto right_validation_scratch =
      exec::BoundDescriptorBatchValidationScratchMemoryBytes(
          right_input_batch, {}, &cancellation_observed,
          &cancellation_probe_failed);
  std::uint64_t left_input_live_bytes = 0;
  std::uint64_t right_input_live_bytes = 0;
  std::uint64_t input_live_bytes = 0;
  std::uint64_t maximum_operator_batch_bytes = 0;
  std::uint64_t dag_validation_scratch_bytes = 0;
  std::uint64_t callback_carrier_bytes =
      sizeof(exec::CanonicalPhysicalDispatchStepResult) + 64 * 1024;
  const auto add_string = [&](const std::string& value) {
    return value.capacity() != std::numeric_limits<std::size_t>::max() &&
           CheckedAdd(callback_carrier_bytes,
                      static_cast<std::uint64_t>(value.capacity()) + 1,
                      &callback_carrier_bytes);
  };
  const auto& context = dag.mga_statement_context;
  std::uint64_t context_array_bytes = 0;
  const auto add_context_array = [&](const std::size_t count,
                                     const std::size_t width) {
    return CheckedMultiply(count, width, &context_array_bytes) &&
           CheckedAdd(callback_carrier_bytes, context_array_bytes,
                      &callback_carrier_bytes);
  };
  const bool callback_carriers_bounded =
      add_string(dag.selected_plan_uuid) &&
      add_string(context.statement_uuid) &&
      add_string(context.owning_transaction_uuid) &&
      add_string(context.statement_snapshot_uuid) &&
      add_string(context.statement_metadata_snapshot_uuid) &&
      add_context_array(
          context.active_excluded_local_transaction_ids.capacity(),
          sizeof(std::uint64_t)) &&
      add_context_array(
          context.in_doubt_excluded_local_transaction_ids.capacity(),
          sizeof(std::uint64_t)) &&
      add_string(context.snapshot_kind) &&
      add_string(context.statement_timestamp) &&
      add_context_array(node.output_descriptor_ids.size(),
                        sizeof(std::uint32_t));
  std::uint64_t coexistence_bytes = 0;
  const bool preflight_ok =
      selected_bound.has_value() && dag_copy_bound.has_value() &&
      left_validation_scratch.has_value() &&
      right_validation_scratch.has_value() && !cancellation_observed &&
      !cancellation_probe_failed && callback_carriers_bounded &&
      BoundDescriptorBatchLiveMemoryBytes(left_input_batch,
                                          &left_input_live_bytes) &&
      BoundDescriptorBatchLiveMemoryBytes(right_input_batch,
                                          &right_input_live_bytes) &&
      CheckedAdd(left_input_live_bytes, right_input_live_bytes,
                 &input_live_bytes) &&
      // SET evaluation may retain reconciled operands, equality keys,
      // membership state, and the output while both dispatcher inputs remain
      // borrowed. Eight complete input-sized carriers conservatively dominate
      // those bounded copies; the executor's logical-memory ledger still
      // independently enforces the rebound callback grant.
      CheckedMultiply(input_live_bytes, 8,
                      &maximum_operator_batch_bytes) &&
      CheckedMultiply(*dag_copy_bound, 32,
                      &dag_validation_scratch_bytes) &&
      CheckedAdd(maximum_operator_batch_bytes, *dag_copy_bound,
                 &coexistence_bytes) &&
      CheckedAdd(coexistence_bytes, dag_validation_scratch_bytes,
                 &coexistence_bytes) &&
      CheckedAdd(coexistence_bytes, *left_validation_scratch,
                 &coexistence_bytes) &&
      CheckedAdd(coexistence_bytes, *right_validation_scratch,
                 &coexistence_bytes) &&
      CheckedAdd(coexistence_bytes, auxiliary_memory_bytes,
                 &coexistence_bytes) &&
      CheckedAdd(coexistence_bytes, callback_carrier_bytes,
                 &coexistence_bytes) &&
      coexistence_bytes <= *selected_bound;
  if (!preflight_ok) {
    *operator_dag = {};
    *callback_memory_bound = 0;
    *detail =
        "retained inputs, output, validation, or operator-local state "
        "exceeds the dispatcher callback memory allowance";
    return false;
  }
  if (!BuildOperatorLocalPhysicalDag(
          dag, node.physical_node_id, operator_dag, detail)) {
    *callback_memory_bound = 0;
    return false;
  }
  const auto root = std::ranges::find_if(
      operator_dag->nodes, [&](const auto& candidate) {
        return candidate.physical_node_id == node.physical_node_id;
      });
  if (root == operator_dag->nodes.end()) {
    *operator_dag = {};
    *callback_memory_bound = 0;
    *detail = "operator-local binary root is absent";
    return false;
  }
  if (root->memory_bytes_required < *selected_bound) {
    *operator_dag = {};
    *callback_memory_bound = 0;
    *detail = "dispatcher callback memory exceeds the published binary grant";
    return false;
  }
  if (!RebindOperatorLocalPhysicalMemoryGrant(
          &*root, operator_dag, *selected_bound, detail)) {
    *operator_dag = {};
    *callback_memory_bound = 0;
    return false;
  }
  *callback_memory_bound = *selected_bound;
  detail->clear();
  return true;
}

#if defined(SCRATCHBIRD_QOW_QUERY_ROUTE_CONTRACT_ONLY)
void ArmCanonicalPhysicalRegistrationPreResultRevocationForContractTest() {
  g_contract_revalidation_resolution_count = 0;
  g_contract_pre_result_revocation_armed = true;
}

std::size_t CanonicalPhysicalRegistrationRevalidationCountForTest() {
  return g_contract_revalidation_resolution_count;
}
#endif

exec::CanonicalPhysicalExecutorRegistration
MakeLiveMaterializedSourceRegistration(
    std::unordered_map<std::uint64_t, exec::DescriptorBatch> batches,
    std::string capability_uuid,
    std::string diagnostic_id,
    std::string operation_name,
    const exec::PhysicalNodeKind node_kind,
    std::string implementation_id,
    std::string payload_name,
    const bool strict_dispatcher_memory,
    const exec::CanonicalExecutionMgaAuthority* borrowed_mga_authority) {
  std::uint64_t registration_retained_bytes =
      strict_dispatcher_memory
          ? sizeof(std::unordered_map<std::uint64_t,
                                      exec::DescriptorBatch>) +
                sizeof(std::unordered_map<std::uint64_t, std::uint64_t>) +
                sizeof(std::string) * 2 + 8 * sizeof(void*) + 512
          : 0;
  const auto account_bytes = [&](const std::uint64_t bytes) {
    return CheckedAdd(registration_retained_bytes, bytes,
                      &registration_retained_bytes);
  };
  std::unordered_map<std::uint64_t, std::uint64_t> batch_memory_bytes;
  if (strict_dispatcher_memory) {
    std::uint64_t map_storage_bytes = 0;
    if (!CheckedMultiply(
            batches.size(),
            sizeof(std::pair<const std::uint64_t, exec::DescriptorBatch>),
            &map_storage_bytes) ||
        !account_bytes(map_storage_bytes) ||
        !CheckedMultiply(batches.bucket_count(), sizeof(void*),
                         &map_storage_bytes) ||
        !account_bytes(map_storage_bytes) ||
        !account_bytes(diagnostic_id.capacity() + 1) ||
        !account_bytes(operation_name.capacity() + 1) ||
        !account_bytes(payload_name.capacity() + 1)) {
      registration_retained_bytes = 0;
    }
    for (const auto& [node_id, batch] : batches) {
      std::uint64_t bytes = 0;
      if (registration_retained_bytes == 0 ||
          !RuntimeMaterializedBatchMemoryBytes(batch, &bytes) || bytes == 0 ||
          !account_bytes(bytes) ||
          !batch_memory_bytes.emplace(node_id, bytes).second) {
        registration_retained_bytes = 0;
        batch_memory_bytes.clear();
        break;
      }
    }
    if (registration_retained_bytes != 0) {
      if (!CheckedMultiply(
              batch_memory_bytes.size(),
              sizeof(std::pair<const std::uint64_t, std::uint64_t>),
              &map_storage_bytes) ||
          !account_bytes(map_storage_bytes) ||
          !CheckedMultiply(batch_memory_bytes.bucket_count(), sizeof(void*),
                           &map_storage_bytes) ||
          !account_bytes(map_storage_bytes)) {
        registration_retained_bytes = 0;
        batch_memory_bytes.clear();
      }
    }
  }
  exec::CanonicalPhysicalExecutorRegistration registration;
  registration.node_kind = node_kind;
  registration.implementation_id = std::move(implementation_id);
  registration.executor_capability_uuid = std::move(capability_uuid);
  registration.executor_capability_abi_version = 1;
  registration.engine_owned = true;
  registration.accepts_optimizer_publication_v2 = true;
  registration.honors_dispatcher_memory_limit_v1 = strict_dispatcher_memory;
  registration.retained_live_memory_bytes_v1 = registration_retained_bytes;
  registration.execute =
      [batches = std::move(batches), diagnostic_id = std::move(diagnostic_id),
       operation_name = std::move(operation_name), strict_dispatcher_memory,
       payload_name = std::move(payload_name),
       batch_memory_bytes = std::move(batch_memory_bytes),
       borrowed_mga_authority](
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
        const auto batch = batches.find(node.relational_node_id);
        const auto batch_memory =
            batch_memory_bytes.find(node.relational_node_id);
        if (borrowed_mga_authority != nullptr) {
          const auto before = exec::RevalidateCanonicalExecutionMgaAuthority(
              *borrowed_mga_authority, dag);
          if (!before.ok) {
            step.diagnostic = before;
            return step;
          }
          step.mga_statement_context =
              borrowed_mga_authority->statement_context;
        }
        if (!node.input_physical_node_ids.empty() || !inputs.empty() ||
            batch == batches.end() ||
            (strict_dispatcher_memory &&
             (batch_memory == batch_memory_bytes.end() ||
              batch_memory->second == 0 ||
              batch_memory->second >
                  node.dispatcher_callback_memory_limit_bytes))) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              strict_dispatcher_memory && batch != batches.end()
                  ? "SBLR.PLAN_TREE.RESOURCE_LIMIT"
                  : diagnostic_id;
          step.diagnostic.detail =
              strict_dispatcher_memory && batch != batches.end()
                  ? operation_name + " " + payload_name +
                        " output exceeds the dispatcher callback allowance"
                  : operation_name + " " + payload_name +
                        " executor input or payload identity differs";
          return step;
        }
        if (borrowed_mga_authority != nullptr) {
          const auto after = exec::RevalidateCanonicalExecutionMgaAuthority(
              *borrowed_mga_authority, dag);
          if (!after.ok) {
            step.diagnostic = after;
            return step;
          }
        }
        step.result_handle_id = node.physical_node_id;
        step.output_row_count = batch->second.rows.size();
        step.rows_examined = batch->second.rows.size();
        step.materialized_output_batch = batch->second;
        return step;
      };
  return registration;
}

exec::CanonicalPhysicalExecutorRegistration MakeLiveValuesRegistration(
    std::unordered_map<std::uint64_t, exec::DescriptorBatch> batches,
    std::string capability_uuid,
    std::string diagnostic_id,
    std::string operation_name,
    const bool strict_dispatcher_memory) {
  return MakeLiveMaterializedSourceRegistration(
      std::move(batches), std::move(capability_uuid),
      std::move(diagnostic_id), std::move(operation_name),
      exec::PhysicalNodeKind::kValues,
      std::string(kValuesImplementationId), "VALUES",
      strict_dispatcher_memory, nullptr);
}

exec::CanonicalPhysicalExecutorRegistration
MakeLiveTableSubqueryRegistration(
    std::string capability_uuid,
    const std::size_t maximum_input_row_count,
    api::EngineRequestContext mga_context) {
  exec::CanonicalPhysicalExecutorRegistration registration;
  registration.node_kind = exec::PhysicalNodeKind::kSubquery;
  registration.implementation_id =
      "subquery.table.materialize.typed.v1";
  registration.executor_capability_uuid = std::move(capability_uuid);
  registration.executor_capability_abi_version = 1;
  registration.engine_owned = true;
  registration.accepts_optimizer_publication_v2 = true;
  registration.execute =
      [maximum_input_row_count, mga_context = std::move(mga_context)](
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
        if (inputs.size() != 1 ||
            node.input_physical_node_ids.size() != 1 ||
            inputs.front().physical_node_id !=
                node.input_physical_node_ids.front() ||
            !inputs.front().materialized_output_batch.has_value() ||
            inputs.front().materialized_output_batch->rows.size() >
                maximum_input_row_count) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "QOW-DIAG-RELATIONAL-LIVE-SUBQUERY-INPUT-V1";
          step.diagnostic.detail =
              "table subquery did not receive its bounded typed input";
          return step;
        }
        const auto& input_batch =
            *inputs.front().materialized_output_batch;
        exec::CanonicalTableSubqueryRequest subquery_request;
        subquery_request.selected_physical_node_id = node.physical_node_id;
        subquery_request.maximum_materialized_row_count =
            std::max<std::size_t>(1, maximum_input_row_count);
        subquery_request.mga_authority =
            BuildCanonicalExecutionMgaAuthority(mga_context, dag);
        auto materialized =
            exec::ExecuteCanonicalTableSubquery(
                subquery_request, dag, node.physical_node_id, input_batch);
        if (!materialized.diagnostic.ok) {
          step.diagnostic = std::move(materialized.diagnostic);
          return step;
        }
        if (!CanonicalOperatorExecutionReceiptMatches(
                materialized, dag, node,
                subquery_request.mga_authority.statement_context) ||
            materialized.materialized_row_count != input_batch.rows.size() ||
            materialized.output_batch.rows.size() !=
                materialized.materialized_row_count ||
            materialized.materialized_row_count >
                subquery_request.maximum_materialized_row_count) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "QOW-DIAG-RELATIONAL-LIVE-SUBQUERY-EXECUTION-V1";
          step.diagnostic.detail =
              "table subquery execution receipt changed";
          return step;
        }
        if (!CanonicalQueryDescriptorBatchesExactlyEqual(
                materialized.output_batch, input_batch)) {
          step.diagnostic.ok = false;
          step.diagnostic.diagnostic_code =
              "QOW-DIAG-RELATIONAL-LIVE-SUBQUERY-EXECUTION-V1";
          step.diagnostic.detail =
              "table subquery materialized values changed";
          return step;
        }
        const auto output_validation =
            exec::ValidateCanonicalDescriptorBatch(
                materialized.output_batch, node.output_descriptor_ids);
        if (!output_validation.ok) {
          step.diagnostic = output_validation;
          return step;
        }
        step.result_handle_id = node.physical_node_id;
        step.input_row_count = input_batch.rows.size();
        step.rows_examined = input_batch.rows.size();
        step.output_row_count = materialized.output_batch.rows.size();
        step.materialized_output_batch =
            std::move(materialized.output_batch);
        step.mga_statement_context =
            std::move(materialized.mga_statement_context);
        return step;
      };
  return registration;
}

}  // namespace scratchbird::engine::sblr
