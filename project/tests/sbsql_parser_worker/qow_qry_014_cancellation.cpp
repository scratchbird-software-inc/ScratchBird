// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "descriptor_value_runtime.hpp"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace exec = scratchbird::engine::executor;
namespace api = scratchbird::engine::internal_api;

namespace {

constexpr std::uint64_t kOwnerLocalTransactionId =
    0xffff'ffff'ffff'ff00ULL;
constexpr std::uint64_t kOldestActiveLocalTransactionId =
    0xffff'ffff'ffff'fee8ULL;
constexpr std::uint64_t kRetentionHorizonLocalTransactionId =
    0xffff'ffff'ffff'fed0ULL;
constexpr std::uint64_t kInDoubtLocalTransactionId =
    0xffff'ffff'ffff'fef0ULL;
constexpr std::uint64_t kInventoryNextLocalTransactionId =
    0xffff'ffff'ffff'fff0ULL;

bool Require(const bool condition, const std::string_view detail) {
  if (!condition) {
    std::cerr << "QOW-TEST-QRY-014-CANCELLATION-V1: " << detail << '\n';
  }
  return condition;
}

exec::PhysicalMgaStatementContext StatementContext(
    const std::string& statement_snapshot_uuid) {
  return {
      "019f0000-0000-7200-8000-00000000f601",
      "019f0000-0000-7200-8000-00000000f602",
      statement_snapshot_uuid,
      "019f0000-0000-7200-8000-00000000f603",
      kOwnerLocalTransactionId,
      0,
      kOldestActiveLocalTransactionId,
      kRetentionHorizonLocalTransactionId,
      kRetentionHorizonLocalTransactionId,
      kRetentionHorizonLocalTransactionId,
      {kOldestActiveLocalTransactionId, kOwnerLocalTransactionId},
      {kInDoubtLocalTransactionId},
      "statement_stable",
      kInventoryNextLocalTransactionId,
      true,
      true,
      true,
  };
}

exec::CanonicalExecutionMgaAuthority BindPhysicalAbiV2(
    exec::TypedPhysicalNodeDag* dag) {
  dag->abi_version = 2;
  dag->local_transaction_id = kOwnerLocalTransactionId;
  dag->statement_snapshot_id = 0;
  dag->bound_sblr_tree_uuid = dag->admission_evidence.at(0).evidence_uuid;
  dag->catalog_epoch_uuid = dag->admission_evidence.at(1).evidence_uuid;
  dag->security_context_uuid = dag->admission_evidence.at(2).evidence_uuid;
  dag->capability_snapshot_uuid = dag->admission_evidence.at(4).evidence_uuid;
  dag->resource_snapshot_uuid = dag->admission_evidence.at(5).evidence_uuid;
  dag->statistics_snapshot_uuid = dag->admission_evidence.at(6).evidence_uuid;
  dag->route_snapshot_uuid = dag->admission_evidence.at(7).evidence_uuid;
  dag->catalog_generation = 1;
  dag->security_epoch = 1;
  dag->policy_epoch = 1;
  dag->resource_epoch = 1;
  dag->statistics_generation = 1;
  dag->route_epoch = 1;
  dag->route_generation = 1;
  dag->memory_budget_bytes = 32ULL * 1024ULL * 1024ULL;
  dag->optimizer_published = true;
  dag->immutable_node_identity_validated = true;
  dag->capability_validated_before_access = true;
  const auto context =
      StatementContext(dag->admission_evidence.at(3).evidence_uuid);
  dag->mga_statement_context = context;
  for (auto& node : dag->nodes) {
    node.mga_statement_context = context;
    node.selected_alternative_uuid =
        "019f0000-0000-7200-8000-00000000f604";
    node.executor_capability_uuid =
        "019f0000-0000-7200-8000-00000000f605";
    node.executor_capability_abi_version = 1;
    node.cost_vector_uuid =
        "019f0000-0000-7200-8000-00000000f606";
    node.memory_bytes_required = 32ULL * 1024ULL * 1024ULL;
    node.engine_capability_validated = true;
  }
  exec::CanonicalExecutionMgaAuthority authority;
  authority.statement_context = context;
  authority.origin = exec::CanonicalMgaAuthorityOrigin::kClosureTestSeam;
  authority.resolve_current = [context] {
    exec::CanonicalMgaCurrentResolution current;
    current.statement_context = context;
    return current;
  };
  return authority;
}

api::EngineDescriptor Descriptor() {
  api::EngineDescriptor descriptor;
  descriptor.descriptor_uuid.canonical =
      "019f0000-0000-7200-8000-000000003601";
  descriptor.descriptor_kind = "scalar";
  descriptor.canonical_type_name = "int64";
  descriptor.encoded_descriptor =
      "type_uuid=019f0000-0000-7300-8000-000000003602;"
      "nullability=non_null";
  return descriptor;
}

api::EngineTypedValue Value(const api::EngineDescriptor& descriptor,
                            const std::int64_t value) {
  api::EngineTypedValue typed;
  typed.descriptor = descriptor;
  typed.encoded_value = std::to_string(value);
  typed.state = api::EngineValueState::value;
  return typed;
}

exec::CanonicalRecursiveCteCancellationRequest Request() {
  const auto descriptor = Descriptor();
  exec::CanonicalRecursiveCteCancellationRequest request;
  auto& working = request.working_request;
  working.physical_dag.selected_plan_uuid =
      "019f0000-0000-7200-8000-000000003603";
  working.physical_dag.root_physical_node_id = 3603;
  working.physical_dag.local_transaction_id = 3604;
  working.physical_dag.statement_snapshot_id = 3605;
  working.physical_dag.admission_evidence = {
      {exec::PhysicalAdmissionStage::kBoundRequest,
       "019f0000-0000-7200-8000-000000003611"},
      {exec::PhysicalAdmissionStage::kCatalogEpoch,
       "019f0000-0000-7200-8000-000000003612"},
      {exec::PhysicalAdmissionStage::kSecurity,
       "019f0000-0000-7200-8000-000000003613"},
      {exec::PhysicalAdmissionStage::kMgaStatementBoundary,
       "019f0000-0000-7200-8000-000000003614"},
      {exec::PhysicalAdmissionStage::kPolicyCapability,
       "019f0000-0000-7200-8000-000000003615"},
      {exec::PhysicalAdmissionStage::kResource,
       "019f0000-0000-7200-8000-000000003616"},
      {exec::PhysicalAdmissionStage::kStatisticsProvenance,
       "019f0000-0000-7200-8000-000000003617"},
      {exec::PhysicalAdmissionStage::kCanonicalRoute,
       "019f0000-0000-7200-8000-000000003618"},
  };
  working.physical_dag.nodes = {
      {.physical_node_id = 3601,
       .relational_node_id = 3601,
       .node_kind = exec::PhysicalNodeKind::kValues,
       .implementation_id = "values.cte-cancellation-anchor.typed.v1",
       .output_descriptor_ids = {3601},
       .causal_counter_id = 36001},
      {.physical_node_id = 3602,
       .relational_node_id = 3602,
       .node_kind = exec::PhysicalNodeKind::kCte,
       .implementation_id = "cte.cancellation-term.typed.v1",
       .output_descriptor_ids = {3601},
       .causal_counter_id = 36002},
      {.physical_node_id = 3603,
       .relational_node_id = 3603,
       .node_kind = exec::PhysicalNodeKind::kRecursiveCte,
       .implementation_id = "cte.recursive.cancellable.typed.v1",
       .input_physical_node_ids = {3601, 3602},
       .output_descriptor_ids = {3601},
       .causal_counter_id = 36003},
  };
  working.selected_physical_node_id = 3603;
  working.anchor_batch = exec::MakeDescriptorBatch(
      {{"n", descriptor, false, 3601}}, {{{Value(descriptor, 1)}}});
  working.recursive_step =
      [descriptor](const exec::DescriptorBatch& current, const std::size_t) {
        exec::DescriptorBatch next;
        next.columns = current.columns;
        for (const auto& row : current.rows) {
          const auto value = std::stoll(row.values[0].encoded_value);
          if (value < 4) next.rows.push_back({{Value(descriptor, value + 1)}});
        }
        return next;
      };
  working.maximum_iteration_count = 8;
  working.maximum_working_row_count = 4;
  working.maximum_result_row_count = 8;
  working.mga_authority = BindPhysicalAbiV2(&working.physical_dag);
  request.cancellation_requested = [](const std::size_t) { return false; };
  request.cancellation_evidence_uuid =
      "019f0000-0000-7200-8000-000000003615";
  return request;
}

// QOW-TEST-QRY-014-CANCELLATION-V1
bool ValidateCancellationBoundary() {
  bool passed = true;
  auto request = Request();
  request.cancellation_requested =
      [](const std::size_t iteration) { return iteration == 2; };
  auto result = exec::ExecuteCanonicalRecursiveCteCancellation(request);
  passed &= Require(
      !result.working_result.diagnostic.ok && result.cancelled &&
          result.cancellation_iteration_ordinal == 2 &&
          result.working_result.output_batch.rows.empty() &&
          result.working_result.iterations.empty() &&
          result.working_state_cleaned &&
          result.cancellation_evidence_uuid ==
              "019f0000-0000-7200-8000-000000003615",
      "mid-recursion cancellation published or retained private state");

  result = exec::ExecuteCanonicalRecursiveCteCancellation(Request());
  passed &= Require(
      result.working_result.diagnostic.ok && !result.cancelled &&
          result.working_result.output_batch.rows.size() == 4 &&
          result.working_result.recursive_iteration_count == 4 &&
          result.working_state_cleaned &&
          result.working_result.executed_physical_node_id == 3603 &&
          result.mga_statement_context.visible_committed_high_watermark == 0 &&
          exec::PhysicalMgaStatementContextEqual(
              result.mga_statement_context,
              Request().working_request.mga_authority.statement_context),
      "uncancelled recursive CTE did not complete through shared working state");

  request = Request();
  request.cancellation_requested =
      [](const std::size_t iteration) { return iteration == 0; };
  result = exec::ExecuteCanonicalRecursiveCteCancellation(request);
  passed &= Require(!result.working_result.diagnostic.ok && result.cancelled &&
                        result.cancellation_iteration_ordinal == 0 &&
                        result.working_result.output_batch.rows.empty() &&
                        result.working_state_cleaned,
                    "pre-anchor cancellation was not an atomic cleanup");

  request = Request();
  request.cancellation_evidence_uuid =
      "019f0000-0000-7200-8000-000000003699";
  result = exec::ExecuteCanonicalRecursiveCteCancellation(request);
  passed &= Require(!result.working_result.diagnostic.ok && !result.cancelled &&
                        result.cancellation_evidence_uuid.empty(),
                    "drifted cancellation evidence was accepted");

  request = Request();
  request.cancellation_requested = {};
  result = exec::ExecuteCanonicalRecursiveCteCancellation(request);
  passed &= Require(!result.working_result.diagnostic.ok &&
                        result.working_state_cleaned,
                    "missing cancellation probe was accepted");

  request = Request();
  request.cancellation_requested = [](const std::size_t iteration) {
    if (iteration == 1) throw std::runtime_error("probe transport failed");
    return false;
  };
  result = exec::ExecuteCanonicalRecursiveCteCancellation(request);
  passed &= Require(!result.working_result.diagnostic.ok && !result.cancelled &&
                        result.working_result.output_batch.rows.empty() &&
                        result.working_state_cleaned,
                    "probe failure was misreported as cancellation or published");

  request = Request();
  request.working_request.physical_dag.nodes[2].implementation_id =
      "cte.recursive.working.typed.v1";
  result = exec::ExecuteCanonicalRecursiveCteCancellation(request);
  passed &= Require(!result.working_result.diagnostic.ok,
                    "uncancellable physical profile bypassed admission");

  request = Request();
  request.working_request.physical_dag.local_transaction_id = 0;
  result = exec::ExecuteCanonicalRecursiveCteCancellation(request);
  passed &= Require(!result.working_result.diagnostic.ok &&
                        result.working_result.output_batch.rows.empty() &&
                        result.working_state_cleaned &&
                        !exec::PhysicalMgaStatementContextValid(
                            result.mga_statement_context),
                    "cancellation route accepted missing MGA transaction");

  request = Request();
  request.working_request.mga_authority.resolve_current = {};
  result = exec::ExecuteCanonicalRecursiveCteCancellation(request);
  passed &= Require(!result.working_result.diagnostic.ok &&
                        result.working_result.output_batch.rows.empty() &&
                        result.working_state_cleaned,
                    "missing current MGA resolver reached cancellation");

  request = Request();
  request.working_request.physical_dag.catalog_epoch_uuid =
      request.working_request.physical_dag.mga_statement_context
          .statement_metadata_snapshot_uuid;
  result = exec::ExecuteCanonicalRecursiveCteCancellation(request);
  passed &= Require(!result.working_result.diagnostic.ok &&
                        result.working_result.output_batch.rows.empty() &&
                        result.working_state_cleaned,
                    "catalog metadata substitution reached cancellation");

  request = Request();
  request.working_request.physical_dag.local_transaction_id =
      static_cast<std::uint32_t>(kOwnerLocalTransactionId);
  result = exec::ExecuteCanonicalRecursiveCteCancellation(request);
  passed &= Require(!result.working_result.diagnostic.ok &&
                        result.working_result.output_batch.rows.empty() &&
                        result.working_state_cleaned,
                    "narrowed MGA identity reached cancellation");
  return passed;
}

}  // namespace

int main() {
  return ValidateCancellationBoundary() ? EXIT_SUCCESS : EXIT_FAILURE;
}
