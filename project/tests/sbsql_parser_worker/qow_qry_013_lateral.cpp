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
    std::cerr << "QOW-TEST-QRY-013-LATERAL-V1: " << detail << '\n';
  }
  return condition;
}

exec::PhysicalMgaStatementContext StatementContext(
    const std::string& statement_snapshot_uuid) {
  return {
      "019f0000-0000-7200-8000-00000000e661",
      "019f0000-0000-7200-8000-00000000e662",
      statement_snapshot_uuid,
      "019f0000-0000-7200-8000-00000000e663",
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
    exec::TypedPhysicalNodeDag* dag,
    const exec::PhysicalMgaStatementContext* supplied_context = nullptr) {
  dag->abi_version = 2;
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
  const auto context = supplied_context
                           ? *supplied_context
                           : StatementContext(
                                 dag->admission_evidence.at(3).evidence_uuid);
  dag->local_transaction_id = context.owning_local_transaction_id;
  dag->statement_snapshot_id = context.visible_committed_high_watermark;
  dag->mga_statement_context = context;
  for (auto& node : dag->nodes) {
    node.mga_statement_context = context;
    node.selected_alternative_uuid =
        "019f0000-0000-7200-8000-00000000e664";
    node.executor_capability_uuid =
        "019f0000-0000-7200-8000-00000000e665";
    node.executor_capability_abi_version = 1;
    node.cost_vector_uuid =
        "019f0000-0000-7200-8000-00000000e666";
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
                                 const std::string& type_uuid,
                                 const std::string& type_name) {
  api::EngineDescriptor descriptor;
  descriptor.descriptor_uuid.canonical = descriptor_uuid;
  descriptor.descriptor_kind = "scalar";
  descriptor.canonical_type_name = type_name;
  descriptor.encoded_descriptor =
      "type_uuid=" + type_uuid + ";nullability=nullable";
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

exec::CanonicalCorrelatedSubqueryRequest CorrelatedRequest() {
  const auto outer_key = Descriptor(
      "019f0000-0000-7200-8000-000000003101",
      "019f0000-0000-7300-8000-000000003102", "int64");
  const auto outer_payload = Descriptor(
      "019f0000-0000-7200-8000-000000003103",
      "019f0000-0000-7300-8000-000000003104", "text");
  const auto inner_key = Descriptor(
      "019f0000-0000-7200-8000-000000003105",
      "019f0000-0000-7300-8000-000000003106", "int64");
  const auto inner_payload = Descriptor(
      "019f0000-0000-7200-8000-000000003107",
      "019f0000-0000-7300-8000-000000003108", "text");

  exec::CanonicalCorrelatedSubqueryRequest request;
  request.physical_dag.selected_plan_uuid =
      "019f0000-0000-7200-8000-000000003109";
  request.physical_dag.root_physical_node_id = 3103;
  request.physical_dag.admission_evidence = {
      {exec::PhysicalAdmissionStage::kBoundRequest,
       "019f0000-0000-7200-8000-000000003111"},
      {exec::PhysicalAdmissionStage::kCatalogEpoch,
       "019f0000-0000-7200-8000-000000003112"},
      {exec::PhysicalAdmissionStage::kSecurity,
       "019f0000-0000-7200-8000-000000003113"},
      {exec::PhysicalAdmissionStage::kMgaStatementBoundary,
       "019f0000-0000-7200-8000-000000003114"},
      {exec::PhysicalAdmissionStage::kPolicyCapability,
       "019f0000-0000-7200-8000-000000003115"},
      {exec::PhysicalAdmissionStage::kResource,
       "019f0000-0000-7200-8000-000000003116"},
      {exec::PhysicalAdmissionStage::kStatisticsProvenance,
       "019f0000-0000-7200-8000-000000003117"},
      {exec::PhysicalAdmissionStage::kCanonicalRoute,
       "019f0000-0000-7200-8000-000000003118"},
  };
  request.physical_dag.nodes = {
      {.physical_node_id = 3101,
       .relational_node_id = 3101,
       .node_kind = exec::PhysicalNodeKind::kValues,
       .implementation_id = "values.lateral-outer.typed.v1",
       .output_descriptor_ids = {3101, 3102},
       .causal_counter_id = 31001},
      {.physical_node_id = 3102,
       .relational_node_id = 3102,
       .node_kind = exec::PhysicalNodeKind::kValues,
       .implementation_id = "values.lateral-inner.typed.v1",
       .output_descriptor_ids = {3103, 3104},
       .causal_counter_id = 31002},
      {.physical_node_id = 3103,
       .relational_node_id = 3103,
       .node_kind = exec::PhysicalNodeKind::kSubquery,
       .implementation_id = "subquery.correlated.int64-equality.typed.v1",
       .input_physical_node_ids = {3101, 3102},
       .output_descriptor_ids = {3103, 3104},
       .causal_counter_id = 31003},
  };
  request.selected_physical_node_id = 3103;
  request.outer_batch = exec::MakeDescriptorBatch(
      {{"outer_key", outer_key, true, 3101},
       {"outer_payload", outer_payload, true, 3102}},
      {{{Value(outer_key, "1"), Value(outer_payload, "outer-a")}},
       {{Null(outer_key), Value(outer_payload, "outer-null")}},
       {{Value(outer_key, "2"), Value(outer_payload, "outer-b")}},
       {{Value(outer_key, "01"), Value(outer_payload, "outer-alias")}}});
  request.inner_batch = exec::MakeDescriptorBatch(
      {{"inner_key", inner_key, true, 3103},
       {"inner_payload", inner_payload, true, 3104}},
      {{{Value(inner_key, "01"), Value(inner_payload, "inner-a")}},
       {{Value(inner_key, "2"), Value(inner_payload, "inner-b")}},
       {{Value(inner_key, "1"), Value(inner_payload, "inner-c")}},
       {{Null(inner_key), Value(inner_payload, "inner-null")}}});
  request.outer_binding_column = 0;
  request.outer_binding_expression_descriptor_id = 3101;
  request.inner_reference_column = 0;
  request.inner_reference_expression_descriptor_id = 3103;
  request.maximum_scope_execution_count = 4;
  request.maximum_comparison_count = 16;
  request.maximum_result_row_count = 5;
  request.mga_authority = BindPhysicalAbiV2(&request.physical_dag);
  return request;
}

exec::CanonicalLateralSubqueryRequest Request() {
  exec::CanonicalLateralSubqueryRequest request;
  request.correlated_request = CorrelatedRequest();
  request.physical_dag.selected_plan_uuid =
      "019f0000-0000-7200-8000-000000003121";
  request.physical_dag.root_physical_node_id = 3113;
  request.physical_dag.admission_evidence = {
      {exec::PhysicalAdmissionStage::kBoundRequest,
       "019f0000-0000-7200-8000-000000003122"},
      {exec::PhysicalAdmissionStage::kCatalogEpoch,
       "019f0000-0000-7200-8000-000000003123"},
      {exec::PhysicalAdmissionStage::kSecurity,
       "019f0000-0000-7200-8000-000000003124"},
      {exec::PhysicalAdmissionStage::kMgaStatementBoundary,
       "019f0000-0000-7200-8000-000000003125"},
      {exec::PhysicalAdmissionStage::kPolicyCapability,
       "019f0000-0000-7200-8000-000000003126"},
      {exec::PhysicalAdmissionStage::kResource,
       "019f0000-0000-7200-8000-000000003127"},
      {exec::PhysicalAdmissionStage::kStatisticsProvenance,
       "019f0000-0000-7200-8000-000000003128"},
      {exec::PhysicalAdmissionStage::kCanonicalRoute,
       "019f0000-0000-7200-8000-000000003129"},
  };
  request.physical_dag.admission_evidence[3].evidence_uuid =
      request.correlated_request.mga_authority.statement_context
          .statement_snapshot_uuid;
  request.physical_dag.nodes = {
      {.physical_node_id = 3111,
       .relational_node_id = 3111,
       .node_kind = exec::PhysicalNodeKind::kValues,
       .implementation_id = "values.lateral-outer.typed.v1",
       .output_descriptor_ids = {3101, 3102},
       .causal_counter_id = 31101},
      {.physical_node_id = 3112,
       .relational_node_id = 3112,
       .node_kind = exec::PhysicalNodeKind::kSubquery,
       .implementation_id = "subquery.lateral-correlated.typed.v1",
       .output_descriptor_ids = {3103, 3104},
       .causal_counter_id = 31102},
      {.physical_node_id = 3113,
       .relational_node_id = 3113,
       .node_kind = exec::PhysicalNodeKind::kJoin,
       .implementation_id = "join.lateral-inner.correlated.typed.v1",
       .input_physical_node_ids = {3111, 3112},
       .output_descriptor_ids = {3101, 3102, 3103, 3104},
       .causal_counter_id = 31103},
  };
  request.selected_physical_node_id = 3113;
  request.maximum_output_row_count = 5;
  request.mga_authority = BindPhysicalAbiV2(
      &request.physical_dag,
      &request.correlated_request.mga_authority.statement_context);
  return request;
}

exec::CanonicalLateralSubqueryRequest RequestFor(
    const exec::CanonicalLateralJoinForm form) {
  auto request = Request();
  request.form = form;
  switch (form) {
    case exec::CanonicalLateralJoinForm::kInnerLateral:
      break;
    case exec::CanonicalLateralJoinForm::kLeftLateral:
      request.physical_dag.nodes[2].implementation_id =
          "join.lateral-left.correlated.typed.v1";
      request.maximum_output_row_count = 6;
      break;
    case exec::CanonicalLateralJoinForm::kCrossApply:
      request.physical_dag.nodes[2].implementation_id =
          "join.cross-apply.correlated.typed.v1";
      break;
    case exec::CanonicalLateralJoinForm::kOuterApply:
      request.physical_dag.nodes[2].implementation_id =
          "join.outer-apply.correlated.typed.v1";
      request.maximum_output_row_count = 6;
      break;
  }
  return request;
}

// QOW-TEST-QRY-013-LATERAL-V1
// QOW-TEST-QRY-013-LATERAL-V2
bool ValidateLateralSubquery() {
  bool passed = true;
  auto result = exec::ExecuteCanonicalLateralSubquery(Request());
  passed &= Require(
      result.diagnostic.ok && result.scope_execution_count == 4 &&
          result.form == exec::CanonicalLateralJoinForm::kInnerLateral &&
          result.matched_scope_count == 3 &&
          result.null_extended_outer_row_count == 0 &&
          result.output_row_count == 5 &&
          result.output_batch.columns.size() == 4 &&
          result.output_batch.rows.size() == 5 &&
          result.output_batch.rows[0].values[1].encoded_value == "outer-a" &&
          result.output_batch.rows[0].values[3].encoded_value == "inner-a" &&
          result.output_batch.rows[1].values[3].encoded_value == "inner-c" &&
          result.output_batch.rows[2].values[1].encoded_value == "outer-b" &&
          result.output_batch.rows[2].values[3].encoded_value == "inner-b" &&
          result.output_batch.rows[3].values[1].encoded_value ==
              "outer-alias" &&
          result.output_batch.rows[4].values[3].encoded_value == "inner-c" &&
          result.correlated_plan_uuid ==
              "019f0000-0000-7200-8000-000000003109" &&
          result.selected_plan_uuid ==
              "019f0000-0000-7200-8000-000000003121" &&
          result.executed_physical_node_id == 3113 &&
          result.causal_counter_id == 31103 &&
          result.mga_statement_context.visible_committed_high_watermark == 0 &&
          exec::PhysicalMgaStatementContextEqual(
              result.mga_statement_context,
              Request().mga_authority.statement_context),
      "inner-LATERAL did not flatten correlated scopes in canonical order");

  auto request = Request();
  request.correlated_request.inner_batch.rows.clear();
  result = exec::ExecuteCanonicalLateralSubquery(request);
  passed &= Require(result.diagnostic.ok &&
                        result.scope_execution_count == 4 &&
                        result.output_row_count == 0 &&
                        result.output_batch.columns.size() == 4 &&
                        result.output_batch.rows.empty(),
                    "empty correlated results lost typed LATERAL schema");

  request = Request();
  request.correlated_request.outer_batch.rows.clear();
  result = exec::ExecuteCanonicalLateralSubquery(request);
  passed &= Require(result.diagnostic.ok &&
                        result.scope_execution_count == 0 &&
                        result.output_row_count == 0,
                    "empty outer relation invented LATERAL output");

  request = RequestFor(exec::CanonicalLateralJoinForm::kLeftLateral);
  result = exec::ExecuteCanonicalLateralSubquery(request);
  passed &= Require(
      result.diagnostic.ok &&
          result.form == exec::CanonicalLateralJoinForm::kLeftLateral &&
          result.scope_execution_count == 4 &&
          result.matched_scope_count == 3 &&
          result.null_extended_outer_row_count == 1 &&
          result.output_row_count == 6 && result.output_batch.rows.size() == 6 &&
          result.output_batch.columns[2].nullable &&
          result.output_batch.columns[3].nullable &&
          result.output_batch.rows[2].values[1].encoded_value ==
              "outer-null" &&
          result.output_batch.rows[2].values[2].state ==
              api::EngineValueState::sql_null &&
          result.output_batch.rows[2].values[3].state ==
              api::EngineValueState::sql_null &&
          result.output_batch.rows[3].values[1].encoded_value == "outer-b" &&
          result.output_batch.rows[5].values[3].encoded_value == "inner-c",
      "left-LATERAL did not null-extend an empty correlated scope");

  request = RequestFor(exec::CanonicalLateralJoinForm::kCrossApply);
  result = exec::ExecuteCanonicalLateralSubquery(request);
  passed &= Require(
      result.diagnostic.ok &&
          result.form == exec::CanonicalLateralJoinForm::kCrossApply &&
          result.matched_scope_count == 3 &&
          result.null_extended_outer_row_count == 0 &&
          result.output_row_count == 5 &&
          result.output_batch.rows[2].values[1].encoded_value == "outer-b",
      "CROSS APPLY did not retain inner-LATERAL cardinality and order");

  request = RequestFor(exec::CanonicalLateralJoinForm::kOuterApply);
  result = exec::ExecuteCanonicalLateralSubquery(request);
  passed &= Require(
      result.diagnostic.ok &&
          result.form == exec::CanonicalLateralJoinForm::kOuterApply &&
          result.matched_scope_count == 3 &&
          result.null_extended_outer_row_count == 1 &&
          result.output_row_count == 6 &&
          result.output_batch.rows[2].values[2].state ==
              api::EngineValueState::sql_null,
      "OUTER APPLY did not retain left-LATERAL cardinality and null extension");

  request = RequestFor(exec::CanonicalLateralJoinForm::kLeftLateral);
  request.correlated_request.inner_batch.rows.clear();
  request.maximum_output_row_count = 4;
  result = exec::ExecuteCanonicalLateralSubquery(request);
  passed &= Require(
      result.diagnostic.ok && result.scope_execution_count == 4 &&
          result.matched_scope_count == 0 &&
          result.null_extended_outer_row_count == 4 &&
          result.output_row_count == 4 &&
          result.output_batch.rows[0].values[2].state ==
              api::EngineValueState::sql_null &&
          result.output_batch.rows[3].values[3].state ==
              api::EngineValueState::sql_null,
      "left-LATERAL empty inner input did not retain every outer row");

  request = RequestFor(exec::CanonicalLateralJoinForm::kLeftLateral);
  request.maximum_output_row_count = 5;
  result = exec::ExecuteCanonicalLateralSubquery(request);
  passed &= Require(!result.diagnostic.ok && result.output_batch.rows.empty() &&
                        result.scope_execution_count == 0 &&
                        result.null_extended_outer_row_count == 0,
                    "left-LATERAL bound ignored null-extended rows");

  request = Request();
  request.maximum_output_row_count = 4;
  result = exec::ExecuteCanonicalLateralSubquery(request);
  passed &= Require(!result.diagnostic.ok && result.output_batch.rows.empty() &&
                        result.scope_execution_count == 0,
                    "LATERAL output bound published partial rows");

  request = Request();
  request.physical_dag.statement_snapshot_id = 3199;
  result = exec::ExecuteCanonicalLateralSubquery(request);
  passed &= Require(!result.diagnostic.ok,
                    "LATERAL/correlation snapshot drift was accepted");

  request = Request();
  request.physical_dag.nodes[2].implementation_id =
      "join.inner.typed.v1";
  result = exec::ExecuteCanonicalLateralSubquery(request);
  passed &= Require(!result.diagnostic.ok,
                    "wrong LATERAL physical profile was accepted");

  request = Request();
  request.form = exec::CanonicalLateralJoinForm::kCrossApply;
  result = exec::ExecuteCanonicalLateralSubquery(request);
  passed &= Require(!result.diagnostic.ok,
                    "APPLY form was relabelled onto an inner-LATERAL plan");

  request = Request();
  request.form = static_cast<exec::CanonicalLateralJoinForm>(99);
  result = exec::ExecuteCanonicalLateralSubquery(request);
  passed &= Require(!result.diagnostic.ok,
                    "unknown LATERAL/APPLY form was accepted");

  request = Request();
  request.physical_dag.nodes[2].output_descriptor_ids =
      {3101, 3103, 3102, 3104};
  result = exec::ExecuteCanonicalLateralSubquery(request);
  passed &= Require(!result.diagnostic.ok,
                    "reordered LATERAL output handles were accepted");

  request = Request();
  request.physical_dag.nodes[1].node_kind = exec::PhysicalNodeKind::kValues;
  result = exec::ExecuteCanonicalLateralSubquery(request);
  passed &= Require(!result.diagnostic.ok,
                    "non-subquery LATERAL input was accepted");

  request = Request();
  request.correlated_request.inner_batch.rows[2].values[0].encoded_value =
      "bad";
  result = exec::ExecuteCanonicalLateralSubquery(request);
  passed &= Require(!result.diagnostic.ok && result.output_batch.rows.empty(),
                    "failed correlation published LATERAL output");

  request = Request();
  request.physical_dag.local_transaction_id = 0;
  result = exec::ExecuteCanonicalLateralSubquery(request);
  passed &= Require(!result.diagnostic.ok && result.output_batch.rows.empty() &&
                        result.scope_execution_count == 0 &&
                        result.output_row_count == 0 &&
                        result.selected_plan_uuid.empty() &&
                        result.correlated_plan_uuid.empty() &&
                        !exec::PhysicalMgaStatementContextValid(
                            result.mga_statement_context),
                    "missing LATERAL engine MGA transaction was accepted");

  request = Request();
  request.mga_authority.statement_context.statement_uuid =
      "019f0000-0000-7200-8000-00000000e667";
  result = exec::ExecuteCanonicalLateralSubquery(request);
  passed &= Require(!result.diagnostic.ok && result.output_batch.rows.empty(),
                    "cross-statement LATERAL authority reached access");

  request = Request();
  auto drift = request.correlated_request.mga_authority.statement_context;
  drift.owning_transaction_uuid =
      "019f0000-0000-7200-8000-00000000e668";
  request.correlated_request.mga_authority.resolve_current = [drift] {
    exec::CanonicalMgaCurrentResolution current;
    current.statement_context = drift;
    return current;
  };
  result = exec::ExecuteCanonicalLateralSubquery(request);
  passed &= Require(!result.diagnostic.ok && result.output_batch.rows.empty(),
                    "current correlated MGA drift reached LATERAL access");
  return passed;
}

}  // namespace

int main() {
  return ValidateLateralSubquery() ? EXIT_SUCCESS : EXIT_FAILURE;
}
