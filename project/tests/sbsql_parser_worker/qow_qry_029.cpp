// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "descriptor_value_runtime.hpp"

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
  if (!condition) std::cerr << "QOW-TEST-QRY-029-V1: " << detail << '\n';
  return condition;
}

exec::PhysicalMgaStatementContext StatementContext(
    const std::string& statement_snapshot_uuid) {
  return {
      "019f0000-0000-7200-8000-00000000f501",
      "019f0000-0000-7200-8000-00000000f502",
      statement_snapshot_uuid,
      "019f0000-0000-7200-8000-00000000f503",
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

void SetStatementContext(
    exec::TypedPhysicalNodeDag* dag,
    const exec::PhysicalMgaStatementContext& context) {
  dag->mga_statement_context = context;
  for (auto& node : dag->nodes) node.mga_statement_context = context;
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
  dag->memory_budget_bytes = 4096;
  dag->optimizer_published = true;
  dag->immutable_node_identity_validated = true;
  dag->capability_validated_before_access = true;
  const auto context = StatementContext(
      dag->admission_evidence.at(3).evidence_uuid);
  SetStatementContext(dag, context);
  for (auto& node : dag->nodes) {
    node.selected_alternative_uuid =
        "019f0000-0000-7200-8000-00000000f504";
    node.executor_capability_uuid =
        "019f0000-0000-7200-8000-00000000f505";
    node.executor_capability_abi_version = 1;
    node.cost_vector_uuid =
        "019f0000-0000-7200-8000-00000000f506";
    node.memory_bytes_required = 1;
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
                                 const std::string& type_name,
                                 const std::string& type_uuid) {
  api::EngineDescriptor descriptor;
  descriptor.descriptor_uuid.canonical = descriptor_uuid;
  descriptor.descriptor_kind = "scalar";
  descriptor.canonical_type_name = type_name;
  descriptor.encoded_descriptor =
      "type_uuid=" + type_uuid + ";nullability=nullable";
  return descriptor;
}

api::EngineTypedValue Value(const api::EngineDescriptor& descriptor,
                            std::string encoded_value) {
  api::EngineTypedValue value;
  value.descriptor = descriptor;
  value.encoded_value = std::move(encoded_value);
  value.state = api::EngineValueState::value;
  return value;
}

api::EngineTypedValue SqlNull(const api::EngineDescriptor& descriptor) {
  api::EngineTypedValue value;
  value.descriptor = descriptor;
  value.is_null = true;
  value.state = api::EngineValueState::sql_null;
  return value;
}

exec::TypedPhysicalNodeDag PhysicalDag(const std::string& project_strategy) {
  exec::TypedPhysicalNodeDag dag;
  dag.selected_plan_uuid = "019f0000-0000-7200-8000-000000002901";
  dag.root_physical_node_id = 20;
  dag.admission_evidence = {
      {exec::PhysicalAdmissionStage::kBoundRequest,
       "019f0000-0000-7200-8000-000000002911"},
      {exec::PhysicalAdmissionStage::kCatalogEpoch,
       "019f0000-0000-7200-8000-000000002912"},
      {exec::PhysicalAdmissionStage::kSecurity,
       "019f0000-0000-7200-8000-000000002913"},
      {exec::PhysicalAdmissionStage::kMgaStatementBoundary,
       "019f0000-0000-7200-8000-000000002914"},
      {exec::PhysicalAdmissionStage::kPolicyCapability,
       "019f0000-0000-7200-8000-000000002915"},
      {exec::PhysicalAdmissionStage::kResource,
       "019f0000-0000-7200-8000-000000002916"},
      {exec::PhysicalAdmissionStage::kStatisticsProvenance,
       "019f0000-0000-7200-8000-000000002917"},
      {exec::PhysicalAdmissionStage::kCanonicalRoute,
       "019f0000-0000-7200-8000-000000002918"},
  };
  dag.nodes = {
      {.physical_node_id = 10,
       .relational_node_id = 1,
       .node_kind = exec::PhysicalNodeKind::kValues,
       .implementation_id = "values.typed.v1",
       .input_physical_node_ids = {},
       .output_descriptor_ids = {101, 102},
       .shareable = false,
       .causal_counter_id = 2901},
      {.physical_node_id = 20,
       .relational_node_id = 2,
       .node_kind = exec::PhysicalNodeKind::kProject,
       .implementation_id = project_strategy,
       .input_physical_node_ids = {10},
       .output_descriptor_ids = {102, 101},
       .shareable = false,
       .causal_counter_id = 2902},
  };
  return dag;
}

exec::DescriptorBatch InputBatch() {
  const auto decimal = Descriptor(
      "019f0000-0000-7200-8000-000000002921", "decimal",
      "019f0000-0000-7300-8000-000000002922");
  const auto boolean = Descriptor(
      "019f0000-0000-7200-8000-000000002923", "boolean",
      "019f0000-0000-7300-8000-000000002924");
  return exec::MakeDescriptorBatch(
      {{"amount", decimal, true, 101}, {"flag", boolean, true, 102}},
      {{{Value(decimal, "0.00"), Value(boolean, "true")}},
       {{SqlNull(decimal), Value(boolean, "false")}}});
}

exec::CanonicalDescriptorProjectionRequest Request(
    const std::string& strategy) {
  exec::CanonicalDescriptorProjectionRequest request;
  request.physical_dag = PhysicalDag(strategy);
  request.selected_physical_node_id = 20;
  request.input_batch = InputBatch();
  request.projected_columns = {1, 0};
  request.mga_authority = BindPhysicalAbiV2(&request.physical_dag);
  return request;
}

bool SameBatch(const exec::DescriptorBatch& left,
               const exec::DescriptorBatch& right) {
  if (left.columns.size() != right.columns.size() ||
      left.rows.size() != right.rows.size()) {
    return false;
  }
  for (std::size_t column = 0; column < left.columns.size(); ++column) {
    if (left.columns[column].descriptor_id !=
            right.columns[column].descriptor_id ||
        left.columns[column].descriptor.descriptor_uuid.canonical !=
            right.columns[column].descriptor.descriptor_uuid.canonical) {
      return false;
    }
  }
  for (std::size_t row = 0; row < left.rows.size(); ++row) {
    for (std::size_t column = 0; column < left.columns.size(); ++column) {
      const auto& lhs = left.rows[row].values[column];
      const auto& rhs = right.rows[row].values[column];
      if (lhs.descriptor.descriptor_uuid.canonical !=
              rhs.descriptor.descriptor_uuid.canonical ||
          lhs.state != rhs.state || lhs.is_null != rhs.is_null ||
          lhs.encoded_value != rhs.encoded_value ||
          lhs.binary_value != rhs.binary_value) {
        return false;
      }
    }
  }
  return true;
}

// QOW-TEST-QRY-029-V1
bool ValidateCanonicalPhysicalReachability() {
  const auto row = exec::ExecuteCanonicalDescriptorProjection(
      Request("project.typed.row.v1"));
  bool passed = true;
  passed &= Require(row.diagnostic.ok, "selected project node was refused");
  passed &= Require(row.executed_physical_node_id == 20 &&
                        row.causal_counter_id == 2902 &&
                        !row.selected_plan_uuid.empty() &&
                        row.mga_statement_context
                                .visible_committed_high_watermark == 0 &&
                        exec::PhysicalMgaStatementContextEqual(
                            row.mga_statement_context,
                            Request("project.typed.row.v1")
                                .mga_authority.statement_context),
                    "selected physical identity or causal evidence was lost");
  passed &= Require(row.output_batch.columns.size() == 2 &&
                        row.output_batch.columns[0].descriptor_id == 102 &&
                        row.output_batch.columns[1].descriptor_id == 101,
                    "bound output descriptor order was not retained");
  passed &= Require(row.output_batch.rows.size() == 2 &&
                        row.output_batch.rows[0].values[1].encoded_value ==
                            "0.00",
                    "numeric zero was not retained as typed data");
  const auto& null_value = row.output_batch.rows[1].values[1];
  passed &= Require(null_value.state == api::EngineValueState::sql_null &&
                        null_value.is_null &&
                        null_value.encoded_value.empty() &&
                        null_value.binary_value.empty(),
                    "SQL NULL was translated into substitute data");

  const auto vector = exec::ExecuteCanonicalDescriptorProjection(
      Request("project.typed.vector.v1"));
  passed &= Require(vector.diagnostic.ok &&
                        SameBatch(row.output_batch, vector.output_batch),
                    "forced legal projection strategies changed typed output");
  return passed;
}

bool ValidateFailClosedRoute() {
  bool passed = true;
  auto request = Request("project.typed.row.v1");
  request.selected_physical_node_id = 10;
  auto result = exec::ExecuteCanonicalDescriptorProjection(request);
  passed &= Require(!result.diagnostic.ok &&
                        result.diagnostic.diagnostic_code ==
                            "SBLR.PLAN_TREE.INVALID_HANDLE",
                    "non-root physical node reached the helper");

  request = Request("project.typed.row.v1");
  request.physical_dag.nodes.back().output_descriptor_ids[1] = 999;
  result = exec::ExecuteCanonicalDescriptorProjection(request);
  passed &= Require(!result.diagnostic.ok &&
                        result.diagnostic.diagnostic_code ==
                            "SBLR.PLAN_TREE.INVALID_HANDLE",
                    "unresolved output descriptor became projected data");

  request = Request("project.typed.row.v1");
  request.physical_dag.nodes.back().node_kind =
      exec::PhysicalNodeKind::kFilter;
  result = exec::ExecuteCanonicalDescriptorProjection(request);
  passed &= Require(
      !result.diagnostic.ok &&
          result.diagnostic.diagnostic_code ==
              "QOW-DIAG-QRY-029-CANONICAL-PHYSICAL-ROUTE-V1",
      "non-project physical operator reached the projection helper");

  request = Request("project.typed.row.v1");
  request.physical_dag.local_transaction_id = 0;
  result = exec::ExecuteCanonicalDescriptorProjection(request);
  passed &= Require(!result.diagnostic.ok &&
                        result.diagnostic.diagnostic_code ==
                            "QOW-DIAG-PHYSICAL-NODE-ABI-ADMISSION",
                    "missing MGA statement context reached execution");

  request = Request("project.typed.row.v1");
  request.mga_authority.resolve_current = {};
  result = exec::ExecuteCanonicalDescriptorProjection(request);
  passed &= Require(!result.diagnostic.ok && result.output_batch.rows.empty(),
                    "unresolved current MGA authority reached projection");

  request = Request("project.typed.row.v1");
  request.physical_dag.nodes.back().mga_statement_context.statement_uuid =
      "019f0000-0000-7200-8000-00000000f507";
  result = exec::ExecuteCanonicalDescriptorProjection(request);
  passed &= Require(!result.diagnostic.ok && result.output_batch.rows.empty(),
                    "node-swapped statement context reached projection");

  request = Request("project.typed.row.v1");
  request.physical_dag.mga_statement_context.snapshot_kind =
      "transaction_stable";
  for (auto& node : request.physical_dag.nodes) {
    node.mga_statement_context.snapshot_kind = "transaction_stable";
  }
  request.mga_authority.statement_context.snapshot_kind =
      "transaction_stable";
  result = exec::ExecuteCanonicalDescriptorProjection(request);
  passed &= Require(!result.diagnostic.ok && result.output_batch.rows.empty(),
                    "wrong snapshot kind reached projection");
  return passed;
}

bool ValidateFailClosedTypedValues() {
  bool passed = true;
  auto request = Request("project.typed.row.v1");
  auto& legacy_null = request.input_batch.rows[1].values[0];
  legacy_null.state = api::EngineValueState::value;
  auto result = exec::ExecuteCanonicalDescriptorProjection(request);
  passed &= Require(
      !result.diagnostic.ok &&
          result.diagnostic.diagnostic_code ==
              "QOW-DIAG-QRY-029-TYPED-VALUE-REFUSAL-V1",
      "legacy NULL flag overrode canonical value state");

  request = Request("project.typed.row.v1");
  request.input_batch.rows[1].values[0].encoded_value = "0.00";
  result = exec::ExecuteCanonicalDescriptorProjection(request);
  passed &= Require(
      !result.diagnostic.ok &&
          result.diagnostic.diagnostic_code ==
              "QOW-DIAG-QRY-029-TYPED-VALUE-REFUSAL-V1",
      "SQL NULL payload became a numeric substitute");

  request = Request("project.typed.row.v1");
  request.input_batch.columns[0].descriptor.descriptor_uuid.canonical[0] =
      'A';
  result = exec::ExecuteCanonicalDescriptorProjection(request);
  passed &= Require(!result.diagnostic.ok &&
                        result.diagnostic.diagnostic_code ==
                            "SBLR.PLAN_TREE.INVALID_HANDLE",
                    "noncanonical descriptor UUID reached execution");

  request = Request("project.typed.row.v1");
  request.input_batch.rows.clear();
  result = exec::ExecuteCanonicalDescriptorProjection(request);
  passed &= Require(result.diagnostic.ok && result.output_batch.rows.empty() &&
                        result.output_batch.columns.size() == 2,
                    "typed empty input lost its descriptor shape");
  return passed;
}

}  // namespace

int main() {
  bool passed = true;
  passed &= ValidateCanonicalPhysicalReachability();
  passed &= ValidateFailClosedRoute();
  passed &= ValidateFailClosedTypedValues();
  if (!passed) return 1;
  std::cout << "QOW-TEST-QRY-029-V1: PASS\n";
  return 0;
}
