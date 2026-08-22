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
#include <vector>

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
    std::cerr << "QOW-TEST-QRY-012-RESIDUAL-V1: " << detail << '\n';
  }
  return condition;
}

exec::PhysicalMgaStatementContext StatementContext(
    const std::string& statement_snapshot_uuid) {
  return {
      "019f0000-0000-7200-8000-00000000e511",
      "019f0000-0000-7200-8000-00000000e512",
      statement_snapshot_uuid,
      "019f0000-0000-7200-8000-00000000e513",
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
        "019f0000-0000-7200-8000-00000000e514";
    node.executor_capability_uuid =
        "019f0000-0000-7200-8000-00000000e515";
    node.executor_capability_abi_version = 1;
    node.cost_vector_uuid =
        "019f0000-0000-7200-8000-00000000e516";
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

api::EngineDescriptor Descriptor(const std::string& descriptor_uuid,
                                 const std::string& type_uuid) {
  api::EngineDescriptor descriptor;
  descriptor.descriptor_uuid.canonical = descriptor_uuid;
  descriptor.descriptor_kind = "scalar";
  descriptor.canonical_type_name = "int64";
  descriptor.encoded_descriptor =
      "type_uuid=" + type_uuid + ";nullability=non_null";
  return descriptor;
}

api::EngineTypedValue Value(const api::EngineDescriptor& descriptor,
                            const std::string& encoded) {
  api::EngineTypedValue value;
  value.descriptor = descriptor;
  value.encoded_value = encoded;
  value.state = api::EngineValueState::value;
  return value;
}

exec::CanonicalJoinResidualRequest Request() {
  const auto left_key = Descriptor(
      "019f0000-0000-7200-8000-000000002101",
      "019f0000-0000-7300-8000-000000002102");
  const auto left_payload = Descriptor(
      "019f0000-0000-7200-8000-000000002103",
      "019f0000-0000-7300-8000-000000002104");
  const auto right_key = Descriptor(
      "019f0000-0000-7200-8000-000000002105",
      "019f0000-0000-7300-8000-000000002106");
  const auto right_payload = Descriptor(
      "019f0000-0000-7200-8000-000000002107",
      "019f0000-0000-7300-8000-000000002108");

  exec::CanonicalJoinResidualRequest request;
  auto& key = request.key_request;
  key.physical_dag.selected_plan_uuid =
      "019f0000-0000-7200-8000-000000002109";
  key.physical_dag.root_physical_node_id = 2103;
  key.physical_dag.admission_evidence = {
      {exec::PhysicalAdmissionStage::kBoundRequest,
       "019f0000-0000-7200-8000-000000002111"},
      {exec::PhysicalAdmissionStage::kCatalogEpoch,
       "019f0000-0000-7200-8000-000000002112"},
      {exec::PhysicalAdmissionStage::kSecurity,
       "019f0000-0000-7200-8000-000000002113"},
      {exec::PhysicalAdmissionStage::kMgaStatementBoundary,
       "019f0000-0000-7200-8000-000000002114"},
      {exec::PhysicalAdmissionStage::kPolicyCapability,
       "019f0000-0000-7200-8000-000000002115"},
      {exec::PhysicalAdmissionStage::kResource,
       "019f0000-0000-7200-8000-000000002116"},
      {exec::PhysicalAdmissionStage::kStatisticsProvenance,
       "019f0000-0000-7200-8000-000000002117"},
      {exec::PhysicalAdmissionStage::kCanonicalRoute,
       "019f0000-0000-7200-8000-000000002118"},
  };
  key.physical_dag.nodes = {
      {.physical_node_id = 2101,
       .relational_node_id = 2101,
       .node_kind = exec::PhysicalNodeKind::kValues,
       .implementation_id = "values.left.typed.v1",
       .output_descriptor_ids = {2101, 2102},
       .causal_counter_id = 21001},
      {.physical_node_id = 2102,
       .relational_node_id = 2102,
       .node_kind = exec::PhysicalNodeKind::kValues,
       .implementation_id = "values.right.typed.v1",
       .output_descriptor_ids = {2103, 2104},
       .causal_counter_id = 21002},
      {.physical_node_id = 2103,
       .relational_node_id = 2103,
       .node_kind = exec::PhysicalNodeKind::kJoin,
       .implementation_id = "join.composite-key-residual.v1",
       .input_physical_node_ids = {2101, 2102},
       .output_descriptor_ids = {2101, 2102, 2103, 2104},
       .causal_counter_id = 21003},
  };
  key.selected_physical_node_id = 2103;
  key.left_batch = exec::MakeDescriptorBatch(
      {{"left_key", left_key, false, 2101},
       {"left_payload", left_payload, false, 2102}},
      {{{Value(left_key, "1"), Value(left_payload, "10")}},
       {{Value(left_key, "01"), Value(left_payload, "11")}},
       {{Value(left_key, "2"), Value(left_payload, "12")}}});
  key.right_batch = exec::MakeDescriptorBatch(
      {{"right_key", right_key, false, 2103},
       {"right_payload", right_payload, false, 2104}},
      {{{Value(right_key, "1"), Value(right_payload, "20")}},
       {{Value(right_key, "2"), Value(right_payload, "21")}},
       {{Value(right_key, "01"), Value(right_payload, "22")}}});
  key.key_terms = {
      {.left_column = 0,
       .left_expression_descriptor_id = 2101,
       .right_column = 0,
       .right_expression_descriptor_id = 2103},
  };
  using Truth = api::EngineSqlTruthValue;
  request.residual_truth_values = {
      Truth::true_value,  Truth::true_value, Truth::false_value,
      Truth::unknown,     Truth::false_value, Truth::true_value,
      Truth::unknown,     Truth::true_value, Truth::unknown,
  };
  key.mga_authority = BindPhysicalAbiV2(&key.physical_dag);
  return request;
}

bool RowEquals(const exec::DescriptorTuple& row,
               const std::vector<std::string>& expected) {
  if (row.values.size() != expected.size()) return false;
  for (std::size_t index = 0; index < expected.size(); ++index) {
    if (row.values[index].encoded_value != expected[index]) return false;
  }
  return true;
}

// QOW-TEST-QRY-012-RESIDUAL-V1
bool ValidateJoinResidual() {
  bool passed = true;
  auto result = exec::ExecuteCanonicalJoinResidual(Request());
  passed &= Require(
      result.diagnostic.ok && result.candidate_pair_count == 5 &&
          result.residual_recheck_count == 5 &&
          result.output_batch.rows.size() == 3 &&
          result.executed_physical_node_id == 2103 &&
          result.mga_statement_context
                  .visible_committed_high_watermark == 0 &&
          exec::PhysicalMgaStatementContextEqual(
              result.mga_statement_context,
              Request().key_request.mga_authority.statement_context),
      "residual predicate did not recheck the selected key candidates");
  passed &= Require(
      result.output_batch.rows.size() == 3 &&
          RowEquals(result.output_batch.rows[0], {"1", "10", "1", "20"}) &&
          RowEquals(result.output_batch.rows[1], {"01", "11", "01", "22"}) &&
          RowEquals(result.output_batch.rows[2], {"2", "12", "2", "21"}),
      "residual join output lost deterministic pair order");

  auto request = Request();
  request.residual_truth_values[1] =
      static_cast<api::EngineSqlTruthValue>(99);
  result = exec::ExecuteCanonicalJoinResidual(request);
  passed &= Require(!result.diagnostic.ok && result.output_batch.rows.empty() &&
                        result.candidate_pair_count == 0,
                    "invalid non-candidate residual truth was hidden");

  request = Request();
  request.residual_truth_values.pop_back();
  result = exec::ExecuteCanonicalJoinResidual(request);
  passed &= Require(!result.diagnostic.ok && result.output_batch.rows.empty(),
                    "unbound residual predicate cardinality was accepted");

  request = Request();
  request.maximum_candidate_rechecks = 4;
  result = exec::ExecuteCanonicalJoinResidual(request);
  passed &= Require(!result.diagnostic.ok && result.output_batch.rows.empty() &&
                        result.residual_recheck_count == 0,
                    "residual candidate recheck bound was exceeded");

  request = Request();
  request.key_request.right_batch.rows[2].values[0].encoded_value =
      "malformed";
  result = exec::ExecuteCanonicalJoinResidual(request);
  passed &= Require(!result.diagnostic.ok && result.output_batch.rows.empty(),
                    "malformed key input reached residual evaluation");

  request = Request();
  request.key_request.physical_dag.local_transaction_id = 0;
  result = exec::ExecuteCanonicalJoinResidual(request);
  passed &= Require(!result.diagnostic.ok && result.output_batch.rows.empty() &&
                        result.selected_plan_uuid.empty() &&
                        result.executed_physical_node_id == 0 &&
                        !exec::PhysicalMgaStatementContextValid(
                            result.mga_statement_context),
                    "residual join bypassed MGA physical admission");

  request = Request();
  auto conflated = request.key_request.mga_authority.statement_context;
  conflated.statement_metadata_snapshot_uuid =
      request.key_request.physical_dag.catalog_epoch_uuid;
  request.key_request.physical_dag.mga_statement_context = conflated;
  for (auto& node : request.key_request.physical_dag.nodes) {
    node.mga_statement_context = conflated;
  }
  request.key_request.mga_authority.statement_context = conflated;
  request.key_request.mga_authority.resolve_current = [conflated] {
    exec::CanonicalMgaCurrentResolution current;
    current.statement_context = conflated;
    return current;
  };
  result = exec::ExecuteCanonicalJoinResidual(request);
  passed &= Require(!result.diagnostic.ok && result.output_batch.rows.empty(),
                    "catalog and metadata identity conflation reached residual access");

  request = Request();
  request.key_request.physical_dag.mga_statement_context
      .active_excluded_local_transaction_ids.pop_back();
  result = exec::ExecuteCanonicalJoinResidual(request);
  passed &= Require(!result.diagnostic.ok && result.output_batch.rows.empty(),
                    "narrowed active exclusion set reached residual access");

  request = Request();
  request.key_request.left_batch.rows.clear();
  request.residual_truth_values.clear();
  result = exec::ExecuteCanonicalJoinResidual(request);
  passed &= Require(result.diagnostic.ok && result.output_batch.rows.empty() &&
                        result.candidate_pair_count == 0 &&
                        result.residual_recheck_count == 0,
                    "empty join side invented residual candidates");
  return passed;
}

}  // namespace

int main() {
  return ValidateJoinResidual() ? EXIT_SUCCESS : EXIT_FAILURE;
}
