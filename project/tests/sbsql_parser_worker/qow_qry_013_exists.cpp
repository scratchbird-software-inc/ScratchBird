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
    std::cerr << "QOW-TEST-QRY-013-EXISTS-V1: " << detail << '\n';
  }
  return condition;
}

exec::PhysicalMgaStatementContext StatementContext(
    const std::string& statement_snapshot_uuid) {
  return {
      "019f0000-0000-7200-8000-00000000e631",
      "019f0000-0000-7200-8000-00000000e632",
      statement_snapshot_uuid,
      "019f0000-0000-7200-8000-00000000e633",
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
  dag->memory_budget_bytes = 4096;
  dag->optimizer_published = true;
  dag->immutable_node_identity_validated = true;
  dag->capability_validated_before_access = true;
  const auto context =
      StatementContext(dag->admission_evidence.at(3).evidence_uuid);
  dag->mga_statement_context = context;
  for (auto& node : dag->nodes) {
    node.mga_statement_context = context;
    node.selected_alternative_uuid =
        "019f0000-0000-7200-8000-00000000e634";
    node.executor_capability_uuid =
        "019f0000-0000-7200-8000-00000000e635";
    node.executor_capability_abi_version = 1;
    node.cost_vector_uuid =
        "019f0000-0000-7200-8000-00000000e636";
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
                                 const std::string& type_uuid,
                                 const std::string& type_name,
                                 const std::string& nullability) {
  api::EngineDescriptor descriptor;
  descriptor.descriptor_uuid.canonical = descriptor_uuid;
  descriptor.descriptor_kind = "scalar";
  descriptor.canonical_type_name = type_name;
  descriptor.encoded_descriptor =
      "type_uuid=" + type_uuid + ";nullability=" + nullability;
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

api::EngineTypedValue Null(const api::EngineDescriptor& descriptor) {
  api::EngineTypedValue value;
  value.descriptor = descriptor;
  value.is_null = true;
  value.state = api::EngineValueState::sql_null;
  return value;
}

exec::CanonicalExistsSubqueryRequest Request() {
  const auto source = Descriptor(
      "019f0000-0000-7200-8000-000000002801",
      "019f0000-0000-7300-8000-000000002802", "int64", "nullable");
  const auto exists_result = Descriptor(
      "019f0000-0000-7200-8000-000000002803",
      "019f0000-0000-7300-8000-000000002804", "boolean", "required");

  exec::CanonicalExistsSubqueryRequest request;
  auto& table = request.table_request;
  table.physical_dag.selected_plan_uuid =
      "019f0000-0000-7200-8000-000000002805";
  table.physical_dag.root_physical_node_id = 2802;
  table.physical_dag.admission_evidence = {
      {exec::PhysicalAdmissionStage::kBoundRequest,
       "019f0000-0000-7200-8000-000000002811"},
      {exec::PhysicalAdmissionStage::kCatalogEpoch,
       "019f0000-0000-7200-8000-000000002812"},
      {exec::PhysicalAdmissionStage::kSecurity,
       "019f0000-0000-7200-8000-000000002813"},
      {exec::PhysicalAdmissionStage::kMgaStatementBoundary,
       "019f0000-0000-7200-8000-000000002814"},
      {exec::PhysicalAdmissionStage::kPolicyCapability,
       "019f0000-0000-7200-8000-000000002815"},
      {exec::PhysicalAdmissionStage::kResource,
       "019f0000-0000-7200-8000-000000002816"},
      {exec::PhysicalAdmissionStage::kStatisticsProvenance,
       "019f0000-0000-7200-8000-000000002817"},
      {exec::PhysicalAdmissionStage::kCanonicalRoute,
       "019f0000-0000-7200-8000-000000002818"},
  };
  table.physical_dag.nodes = {
      {.physical_node_id = 2801,
       .relational_node_id = 2801,
       .node_kind = exec::PhysicalNodeKind::kValues,
       .implementation_id = "values.exists-subquery-input.typed.v1",
       .output_descriptor_ids = {2801},
       .causal_counter_id = 28001},
      {.physical_node_id = 2802,
       .relational_node_id = 2802,
       .node_kind = exec::PhysicalNodeKind::kSubquery,
       .implementation_id = "subquery.exists.typed.v1",
       .input_physical_node_ids = {2801},
       .output_descriptor_ids = {2801},
       .causal_counter_id = 28002},
  };
  table.selected_physical_node_id = 2802;
  table.input_batch = exec::MakeDescriptorBatch(
      {{"exists_source", source, true, 2801}},
      {{{Value(source, "1")}}, {{Null(source)}}});
  table.maximum_materialized_row_count = 2;
  table.mga_authority = BindPhysicalAbiV2(&table.physical_dag);
  request.exists_expression_descriptor_id = 2803;
  request.result_column = {"exists_result", exists_result, false, 2803};
  return request;
}

// QOW-TEST-QRY-013-EXISTS-V1
bool ValidateExistsSubquery() {
  bool passed = true;
  auto result = exec::ExecuteCanonicalExistsSubquery(Request());
  passed &= Require(
      result.diagnostic.ok && result.exists && result.source_row_count == 2 &&
          result.output_batch.rows.size() == 1 &&
          result.output_batch.rows[0].values[0].state ==
              api::EngineValueState::value &&
          result.output_batch.rows[0].values[0].encoded_value == "true" &&
          !result.output_batch.rows[0].values[0].is_null &&
          result.selected_plan_uuid ==
              "019f0000-0000-7200-8000-000000002805" &&
          result.executed_physical_node_id == 2802 &&
          result.causal_counter_id == 28002 &&
          result.mga_statement_context.visible_committed_high_watermark == 0 &&
          exec::PhysicalMgaStatementContextEqual(
              result.mga_statement_context,
              Request().table_request.mga_authority.statement_context),
      "nonempty table subquery did not produce canonical TRUE EXISTS");

  auto request = Request();
  request.table_request.input_batch.rows.clear();
  result = exec::ExecuteCanonicalExistsSubquery(request);
  passed &= Require(
      result.diagnostic.ok && !result.exists && result.source_row_count == 0 &&
          result.output_batch.rows.size() == 1 &&
          result.output_batch.rows[0].values[0].encoded_value == "false" &&
          result.output_batch.rows[0].values[0].state ==
              api::EngineValueState::value,
      "empty table subquery did not produce canonical FALSE EXISTS");

  request = Request();
  request.table_request.input_batch.rows.erase(
      request.table_request.input_batch.rows.begin());
  result = exec::ExecuteCanonicalExistsSubquery(request);
  passed &= Require(result.diagnostic.ok && result.exists &&
                        result.source_row_count == 1,
                    "SQL NULL row did not establish row existence");

  request = Request();
  request.table_request.input_batch.rows[1].values[0].descriptor =
      request.result_column.descriptor;
  result = exec::ExecuteCanonicalExistsSubquery(request);
  passed &= Require(!result.diagnostic.ok && result.output_batch.rows.empty() &&
                        !result.exists && result.source_row_count == 0,
                    "malformed later row was hidden by early existence");

  request = Request();
  request.exists_expression_descriptor_id = 0;
  result = exec::ExecuteCanonicalExistsSubquery(request);
  passed &= Require(!result.diagnostic.ok,
                    "unbound EXISTS expression was accepted");

  request = Request();
  request.result_column.descriptor_id = 2899;
  result = exec::ExecuteCanonicalExistsSubquery(request);
  passed &= Require(!result.diagnostic.ok,
                    "EXISTS result handle drift was accepted");

  request = Request();
  request.result_column.nullable = true;
  result = exec::ExecuteCanonicalExistsSubquery(request);
  passed &= Require(!result.diagnostic.ok,
                    "nullable EXISTS result descriptor was accepted");

  request = Request();
  request.result_column.descriptor.canonical_type_name = "int64";
  result = exec::ExecuteCanonicalExistsSubquery(request);
  passed &= Require(!result.diagnostic.ok,
                    "non-boolean EXISTS result descriptor was accepted");

  request = Request();
  request.table_request.maximum_materialized_row_count = 1;
  result = exec::ExecuteCanonicalExistsSubquery(request);
  passed &= Require(!result.diagnostic.ok,
                    "table resource excess published an EXISTS result");

  request = Request();
  request.table_request.physical_dag.local_transaction_id = 0;
  result = exec::ExecuteCanonicalExistsSubquery(request);
  passed &= Require(!result.diagnostic.ok && result.output_batch.rows.empty() &&
                        !result.exists && result.source_row_count == 0 &&
                        result.selected_plan_uuid.empty() &&
                        !exec::PhysicalMgaStatementContextValid(
                            result.mga_statement_context),
                    "missing engine MGA transaction was accepted");

  request = Request();
  request.table_request.physical_dag.mga_statement_context
      .in_doubt_excluded_local_transaction_ids = {
          kOldestActiveLocalTransactionId};
  result = exec::ExecuteCanonicalExistsSubquery(request);
  passed &= Require(!result.diagnostic.ok && result.output_batch.rows.empty(),
                    "overlapping MGA exclusion vectors reached EXISTS access");

  request = Request();
  request.table_request.physical_dag.mga_statement_context.snapshot_kind =
      "transaction_stable";
  result = exec::ExecuteCanonicalExistsSubquery(request);
  passed &= Require(!result.diagnostic.ok && result.output_batch.rows.empty(),
                    "non-statement MGA snapshot reached EXISTS access");
  return passed;
}

}  // namespace

int main() {
  return ValidateExistsSubquery() ? EXIT_SUCCESS : EXIT_FAILURE;
}
