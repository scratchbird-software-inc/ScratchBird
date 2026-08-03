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
    std::cerr << "QOW-TEST-QRY-014-MGA-V1: " << detail << '\n';
  }
  return condition;
}

api::EngineDescriptor Descriptor() {
  api::EngineDescriptor descriptor;
  descriptor.descriptor_uuid.canonical =
      "019f0000-0000-7200-8000-000000003701";
  descriptor.descriptor_kind = "scalar";
  descriptor.canonical_type_name = "int64";
  descriptor.encoded_descriptor =
      "type_uuid=019f0000-0000-7300-8000-000000003702;"
      "nullability=not-null";
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

exec::PhysicalMgaStatementContext RecursiveMgaContext() {
  return {
      "019f0000-0000-7200-8000-00000000f701",
      "019f0000-0000-7200-8000-00000000f702",
      "019f0000-0000-7200-8000-000000003714",
      "019f0000-0000-7200-8000-00000000f703",
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

exec::CanonicalExecutionMgaAuthority RecursiveMgaAuthority(
    const exec::PhysicalMgaStatementContext& context = RecursiveMgaContext()) {
  exec::CanonicalExecutionMgaAuthority authority;
  authority.statement_context = context;
  authority.origin = exec::CanonicalMgaAuthorityOrigin::kClosureTestSeam;
  const auto current = authority.statement_context;
  authority.resolve_current = [current] {
    exec::CanonicalMgaCurrentResolution resolution;
    resolution.statement_context = current;
    return resolution;
  };
  return authority;
}

void BindPhysicalAbiV2(exec::TypedPhysicalNodeDag* dag,
                       const exec::PhysicalMgaStatementContext& context) {
  dag->abi_version = 2;
  dag->local_transaction_id = context.owning_local_transaction_id;
  dag->statement_snapshot_id = context.visible_committed_high_watermark;
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
  dag->memory_budget_bytes = 4096;
  dag->optimizer_published = true;
  dag->immutable_node_identity_validated = true;
  dag->capability_validated_before_access = true;
  dag->mga_statement_context = context;
  for (auto& node : dag->nodes) {
    node.mga_statement_context = context;
    node.selected_alternative_uuid =
        "019f0000-0000-7200-8000-00000000f704";
    node.executor_capability_uuid =
        "019f0000-0000-7200-8000-00000000f705";
    node.executor_capability_abi_version = 1;
    node.cost_vector_uuid =
        "019f0000-0000-7200-8000-00000000f706";
    node.memory_bytes_required = 1;
    node.engine_capability_validated = true;
  }
}

void BindRecursiveContext(
    exec::CanonicalRecursiveCteMgaRequest* request,
    const exec::PhysicalMgaStatementContext& context) {
  BindPhysicalAbiV2(&request->working_request.physical_dag, context);
  request->mga_authority = RecursiveMgaAuthority(context);
  request->working_request.mga_authority = request->mga_authority;
}

bool EmptyRecursiveFailure(
    const exec::CanonicalRecursiveCteMgaResult& result) {
  return !result.working_result.diagnostic.ok &&
         !result.mga_boundary_proven && result.iteration_evidence_count == 0 &&
         result.transaction_inventory_evidence_uuid.empty() &&
         result.working_result.output_batch.columns.empty() &&
         result.working_result.output_batch.rows.empty() &&
         result.working_result.iterations.empty() &&
         result.working_result.executed_physical_node_id == 0 &&
         result.working_result.selected_plan_uuid.empty() &&
         !exec::PhysicalMgaStatementContextValid(
             result.working_result.mga_statement_context) &&
         !exec::PhysicalMgaStatementContextValid(
             result.mga_statement_context);
}

exec::CanonicalRecursiveCteMgaRequest Request() {
  const auto descriptor = Descriptor();
  exec::CanonicalRecursiveCteMgaRequest request;
  auto& working = request.working_request;
  working.physical_dag.selected_plan_uuid =
      "019f0000-0000-7200-8000-000000003703";
  working.physical_dag.root_physical_node_id = 3703;
  working.physical_dag.local_transaction_id = 3704;
  working.physical_dag.statement_snapshot_id = 3705;
  working.physical_dag.admission_evidence = {
      {exec::PhysicalAdmissionStage::kBoundRequest,
       "019f0000-0000-7200-8000-000000003711"},
      {exec::PhysicalAdmissionStage::kCatalogEpoch,
       "019f0000-0000-7200-8000-000000003712"},
      {exec::PhysicalAdmissionStage::kSecurity,
       "019f0000-0000-7200-8000-000000003713"},
      {exec::PhysicalAdmissionStage::kMgaStatementBoundary,
       "019f0000-0000-7200-8000-000000003714"},
      {exec::PhysicalAdmissionStage::kPolicyCapability,
       "019f0000-0000-7200-8000-000000003715"},
      {exec::PhysicalAdmissionStage::kResource,
       "019f0000-0000-7200-8000-000000003716"},
      {exec::PhysicalAdmissionStage::kStatisticsProvenance,
       "019f0000-0000-7200-8000-000000003717"},
      {exec::PhysicalAdmissionStage::kCanonicalRoute,
       "019f0000-0000-7200-8000-000000003718"},
  };
  working.physical_dag.nodes = {
      {.physical_node_id = 3701,
       .relational_node_id = 3701,
       .node_kind = exec::PhysicalNodeKind::kValues,
       .implementation_id = "values.cte-mga-anchor.typed.v1",
       .output_descriptor_ids = {3701},
       .causal_counter_id = 37001},
      {.physical_node_id = 3702,
       .relational_node_id = 3702,
       .node_kind = exec::PhysicalNodeKind::kCte,
       .implementation_id = "cte.mga-term.typed.v1",
       .output_descriptor_ids = {3701},
       .causal_counter_id = 37002},
      {.physical_node_id = 3703,
       .relational_node_id = 3703,
       .node_kind = exec::PhysicalNodeKind::kRecursiveCte,
       .implementation_id = "cte.recursive.mga-boundary.typed.v1",
       .input_physical_node_ids = {3701, 3702},
       .output_descriptor_ids = {3701},
       .causal_counter_id = 37003},
  };
  working.selected_physical_node_id = 3703;
  working.anchor_batch = exec::MakeDescriptorBatch(
      {{"n", descriptor, false, 3701}}, {{{Value(descriptor, 1)}}});
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

  request.transaction_inventory_id = kInventoryNextLocalTransactionId;
  BindRecursiveContext(&request, RecursiveMgaContext());
  request.transaction_inventory_evidence_uuid =
      "019f0000-0000-7200-8000-000000003714";
  request.iteration_evidence = {
      {.iteration_ordinal = 0,
       .creator_local_transaction_id = kOwnerLocalTransactionId,
       .visibility = exec::CanonicalMgaVisibilityDecision::kVisible,
       .security_decision = exec::CanonicalMgaSecurityDecision::kAllowed,
       .engine_evidence_uuid =
           "019f0000-0000-7200-8000-000000003721"},
      {.iteration_ordinal = 1,
       .creator_local_transaction_id = kOwnerLocalTransactionId,
       .visibility = exec::CanonicalMgaVisibilityDecision::kVisible,
       .security_decision = exec::CanonicalMgaSecurityDecision::kAllowed,
       .engine_evidence_uuid =
           "019f0000-0000-7200-8000-000000003722"},
      {.iteration_ordinal = 2,
       .creator_local_transaction_id = kOwnerLocalTransactionId,
       .visibility = exec::CanonicalMgaVisibilityDecision::kVisible,
       .security_decision = exec::CanonicalMgaSecurityDecision::kAllowed,
       .engine_evidence_uuid =
           "019f0000-0000-7200-8000-000000003723"},
      {.iteration_ordinal = 3,
       .creator_local_transaction_id = kOwnerLocalTransactionId,
       .visibility = exec::CanonicalMgaVisibilityDecision::kVisible,
       .security_decision = exec::CanonicalMgaSecurityDecision::kAllowed,
       .engine_evidence_uuid =
           "019f0000-0000-7200-8000-000000003724"},
      {.iteration_ordinal = 4,
       .creator_local_transaction_id = kOwnerLocalTransactionId,
       .visibility = exec::CanonicalMgaVisibilityDecision::kVisible,
       .security_decision = exec::CanonicalMgaSecurityDecision::kAllowed,
       .engine_evidence_uuid =
           "019f0000-0000-7200-8000-000000003725"},
  };
  return request;
}

bool ValidateRecursiveCreatorExclusions() {
  bool passed = true;
  auto excluded = RecursiveMgaContext();
  excluded.visible_committed_high_watermark =
      kInDoubtLocalTransactionId + 1;

  auto request = Request();
  BindRecursiveContext(&request, excluded);
  request.iteration_evidence[1].creator_local_transaction_id =
      kOldestActiveLocalTransactionId;
  auto result = exec::ExecuteCanonicalRecursiveCteMgaBoundary(request);
  passed &= Require(
      EmptyRecursiveFailure(result),
      "visible recursive creator bypassed the captured active exclusion");

  request = Request();
  BindRecursiveContext(&request, excluded);
  request.iteration_evidence[2].creator_local_transaction_id =
      kInDoubtLocalTransactionId;
  result = exec::ExecuteCanonicalRecursiveCteMgaBoundary(request);
  passed &= Require(
      EmptyRecursiveFailure(result),
      "visible recursive creator bypassed the captured in-doubt exclusion");

  request = Request();
  BindRecursiveContext(&request, excluded);
  request.iteration_evidence[1].creator_local_transaction_id =
      kRetentionHorizonLocalTransactionId + 1;
  result = exec::ExecuteCanonicalRecursiveCteMgaBoundary(request);
  passed &= Require(
      result.working_result.diagnostic.ok && result.mga_boundary_proven &&
          result.working_result.output_batch.rows.size() == 4 &&
          exec::PhysicalMgaStatementContextEqual(
              result.mga_statement_context, excluded) &&
          exec::PhysicalMgaStatementContextEqual(
              result.working_result.mga_statement_context,
              excluded),
      "committed non-excluded recursive creator at high-water was refused");

  request = Request();
  request.iteration_evidence[1].creator_local_transaction_id =
      kRetentionHorizonLocalTransactionId + 1;
  result = exec::ExecuteCanonicalRecursiveCteMgaBoundary(request);
  passed &= Require(
      EmptyRecursiveFailure(result),
      "zero high-water admitted a non-owner recursive creator or leaked output");
  return passed;
}

// QOW-TEST-QRY-014-MGA-V1
bool ValidateRecursiveCteMgaBoundary() {
  bool passed = true;
  auto result = exec::ExecuteCanonicalRecursiveCteMgaBoundary(Request());
  passed &= Require(
      result.working_result.diagnostic.ok && result.mga_boundary_proven &&
          result.iteration_evidence_count == 5 &&
          result.working_result.output_batch.rows.size() == 4 &&
          result.working_result.recursive_iteration_count == 4 &&
          result.transaction_inventory_evidence_uuid ==
              "019f0000-0000-7200-8000-000000003714" &&
          result.working_result.executed_physical_node_id == 3703 &&
          result.mga_statement_context.visible_committed_high_watermark == 0 &&
          exec::PhysicalMgaStatementContextEqual(
              result.mga_statement_context,
              Request().mga_authority.statement_context) &&
          exec::PhysicalMgaStatementContextEqual(
              result.working_result.mga_statement_context,
              Request().mga_authority.statement_context),
      "recursive CTE did not retain exact engine MGA boundary evidence");

  auto request = Request();
  request.iteration_evidence[2].visibility =
      exec::CanonicalMgaVisibilityDecision::kInvisible;
  result = exec::ExecuteCanonicalRecursiveCteMgaBoundary(request);
  passed &= Require(!result.working_result.diagnostic.ok &&
                        !result.mga_boundary_proven &&
                        result.working_result.output_batch.rows.empty() &&
                        result.iteration_evidence_count == 0,
                    "invisible recursive transition published output");

  request = Request();
  request.iteration_evidence[2].security_decision =
      exec::CanonicalMgaSecurityDecision::kDenied;
  result = exec::ExecuteCanonicalRecursiveCteMgaBoundary(request);
  passed &= Require(!result.working_result.diagnostic.ok &&
                        result.working_result.output_batch.rows.empty(),
                    "security-denied recursive transition published output");

  request = Request();
  request.mga_authority.statement_context.statement_snapshot_uuid =
      "019f0000-0000-7200-8000-000000009999";
  result = exec::ExecuteCanonicalRecursiveCteMgaBoundary(request);
  passed &= Require(!result.working_result.diagnostic.ok,
                    "inventory snapshot drift was accepted");

  request = Request();
  request.iteration_evidence.pop_back();
  result = exec::ExecuteCanonicalRecursiveCteMgaBoundary(request);
  passed &= Require(!result.working_result.diagnostic.ok &&
                        result.working_result.output_batch.rows.empty(),
                    "missing final empty-transition evidence was accepted");

  request = Request();
  request.iteration_evidence[3].iteration_ordinal = 2;
  result = exec::ExecuteCanonicalRecursiveCteMgaBoundary(request);
  passed &= Require(!result.working_result.diagnostic.ok,
                    "recursive iteration identity drift was accepted");

  request = Request();
  request.iteration_evidence[3].engine_evidence_uuid =
      request.iteration_evidence[2].engine_evidence_uuid;
  result = exec::ExecuteCanonicalRecursiveCteMgaBoundary(request);
  passed &= Require(!result.working_result.diagnostic.ok,
                    "duplicate engine evidence UUID was accepted");

  request = Request();
  request.maximum_boundary_rechecks = 4;
  result = exec::ExecuteCanonicalRecursiveCteMgaBoundary(request);
  passed &= Require(!result.working_result.diagnostic.ok,
                    "recursive MGA boundary recheck bound was exceeded");

  request = Request();
  request.working_request.physical_dag.nodes[2].implementation_id =
      "cte.recursive.working.typed.v1";
  result = exec::ExecuteCanonicalRecursiveCteMgaBoundary(request);
  passed &= Require(!result.working_result.diagnostic.ok,
                    "non-MGA recursive profile bypassed admission");

  request = Request();
  request.working_request.physical_dag.local_transaction_id = 0;
  result = exec::ExecuteCanonicalRecursiveCteMgaBoundary(request);
  passed &= Require(EmptyRecursiveFailure(result),
                    "recursive MGA boundary accepted a missing transaction");

  request = Request();
  request.mga_authority.resolve_current = {};
  request.working_request.mga_authority = request.mga_authority;
  result = exec::ExecuteCanonicalRecursiveCteMgaBoundary(request);
  passed &= Require(EmptyRecursiveFailure(result),
                    "missing current MGA resolver reached recursion");

  request = Request();
  request.working_request.physical_dag.catalog_epoch_uuid =
      request.working_request.physical_dag.mga_statement_context
          .statement_metadata_snapshot_uuid;
  result = exec::ExecuteCanonicalRecursiveCteMgaBoundary(request);
  passed &= Require(EmptyRecursiveFailure(result),
                    "catalog metadata substitution reached recursion");

  request = Request();
  request.working_request.physical_dag.local_transaction_id =
      static_cast<std::uint32_t>(kOwnerLocalTransactionId);
  result = exec::ExecuteCanonicalRecursiveCteMgaBoundary(request);
  passed &= Require(EmptyRecursiveFailure(result),
                    "narrowed MGA identity reached recursion");

  request = Request();
  request.working_request.anchor_batch.rows.clear();
  request.iteration_evidence.resize(1);
  result = exec::ExecuteCanonicalRecursiveCteMgaBoundary(request);
  passed &= Require(result.working_result.diagnostic.ok &&
                        result.mga_boundary_proven &&
                        result.iteration_evidence_count == 1 &&
                        result.working_result.output_batch.rows.empty() &&
                        result.working_result.recursive_iteration_count == 0,
                    "empty anchor invented recursive transition evidence");
  return passed;
}

}  // namespace

int main() {
  return ValidateRecursiveCteMgaBoundary() &&
                 ValidateRecursiveCreatorExclusions()
             ? EXIT_SUCCESS
             : EXIT_FAILURE;
}
