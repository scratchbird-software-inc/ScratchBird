// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "descriptor_value_runtime.hpp"

#include "datatype_catalog_manifest.hpp"
#include "uuid.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

namespace exec = scratchbird::engine::executor;
namespace api = scratchbird::engine::internal_api;
namespace dt = scratchbird::core::datatypes;
namespace uuid = scratchbird::core::uuid;

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
    std::cerr << "QOW-TEST-QRY-013-QUANTIFIED-V1: " << detail << '\n';
  }
  return condition;
}

exec::PhysicalMgaStatementContext StatementContext(
    const std::string& statement_snapshot_uuid) {
  return {
      "019f0000-0000-7200-8000-00000000e641",
      "019f0000-0000-7200-8000-00000000e642",
      statement_snapshot_uuid,
      "019f0000-0000-7200-8000-00000000e643",
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
        "019f0000-0000-7200-8000-00000000e644";
    node.executor_capability_uuid =
        "019f0000-0000-7200-8000-00000000e645";
    node.executor_capability_abi_version = 1;
    node.cost_vector_uuid =
        "019f0000-0000-7200-8000-00000000e646";
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

std::string CoreTypeUuid(const std::string_view stable_name) {
  const auto manifest = dt::LoadCurrentCoreDatatypeCatalogManifest();
  if (!manifest.ok()) return {};
  for (const auto& row : manifest.manifest.descriptor_rows) {
    if (row.stable_name == stable_name && row.descriptor_uuid.valid()) {
      return uuid::UuidToString(row.descriptor_uuid.value);
    }
  }
  return {};
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

exec::CanonicalQuantifiedSubqueryRequest Request() {
  const auto right = Descriptor(
      "019f0000-0000-7200-8000-000000002901",
      "019f0000-0000-7300-8000-000000002902", "int64", "nullable");
  const auto left = Descriptor(
      "019f0000-0000-7200-8000-000000002903",
      "019f0000-0000-7300-8000-000000002904", "int64", "nullable");
  const auto boolean_result = Descriptor(
      "019f0000-0000-7200-8000-000000002905",
      CoreTypeUuid("boolean"), "boolean", "nullable");

  exec::CanonicalQuantifiedSubqueryRequest request;
  auto& table = request.table_request;
  table.physical_dag.selected_plan_uuid =
      "019f0000-0000-7200-8000-000000002907";
  table.physical_dag.root_physical_node_id = 2902;
  table.physical_dag.admission_evidence = {
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
  table.physical_dag.nodes = {
      {.physical_node_id = 2901,
       .relational_node_id = 2901,
       .node_kind = exec::PhysicalNodeKind::kValues,
       .implementation_id = "values.quantified-subquery-input.typed.v1",
       .output_descriptor_ids = {2901},
       .causal_counter_id = 29001},
      {.physical_node_id = 2902,
       .relational_node_id = 2902,
       .node_kind = exec::PhysicalNodeKind::kSubquery,
       .implementation_id = "subquery.quantified.int64.typed.v1",
       .input_physical_node_ids = {2901},
       .output_descriptor_ids = {2901},
       .causal_counter_id = 29002},
  };
  table.selected_physical_node_id = 2902;
  table.input_batch = exec::MakeDescriptorBatch(
      {{"quantified_right", right, true, 2901}},
      {{{Value(right, "1")}}, {{Null(right)}}, {{Value(right, "02")}}});
  table.maximum_materialized_row_count = 3;
  table.mga_authority = BindPhysicalAbiV2(&table.physical_dag);
  request.left_operand_column = {"quantified_left", left, true, 2902};
  request.left_value = Value(left, "2");
  request.right_expression_descriptor_id = 2901;
  request.comparison_operator = api::EngineComparisonPredicateOperator::equal;
  request.quantifier = exec::CanonicalQuantifiedSubqueryQuantifier::kAny;
  request.result_expression_descriptor_id = 2903;
  request.result_column =
      {"quantified_result", boolean_result, true, 2903};
  request.maximum_comparison_count = 3;
  return request;
}

exec::CanonicalQuantifiedSubqueryRequest Real64Request() {
  auto request = Request();
  const auto right = Descriptor(
      "019f0000-0000-7200-8000-000000002921",
      "019f0000-0000-7300-8000-000000002922", "real64", "nullable");
  const auto left = Descriptor(
      "019f0000-0000-7200-8000-000000002923",
      "019f0000-0000-7300-8000-000000002924", "real64", "nullable");
  request.table_request.input_batch = exec::MakeDescriptorBatch(
      {{"quantified_right", right, true, 2901}},
      {{{Value(right, "1.5")}},
       {{Null(right)}},
       {{Value(right, "2.5")}}});
  request.left_operand_column = {"quantified_left", left, true, 2902};
  request.left_value = Value(left, "2.5");
  request.table_request.physical_dag.nodes[1].implementation_id =
      "subquery.quantified.typed.v1";
  return request;
}

// QOW-TEST-QRY-013-QUANTIFIED-V1
bool ValidateQuantifiedSubquery() {
  using Truth = api::EngineSqlTruthValue;
  using Quantifier = exec::CanonicalQuantifiedSubqueryQuantifier;
  using Operation = api::EngineComparisonPredicateOperator;

  bool passed = true;
  auto result = exec::ExecuteCanonicalQuantifiedSubquery(Request());
  passed &= Require(
      result.diagnostic.ok && result.truth_value == Truth::true_value &&
          result.comparison_count == 3 &&
          result.output_batch.rows.size() == 1 &&
          result.output_batch.rows[0].values[0].encoded_value == "true" &&
          result.selected_plan_uuid ==
              "019f0000-0000-7200-8000-000000002907" &&
          result.executed_physical_node_id == 2902 &&
          result.causal_counter_id == 29002 &&
          result.mga_statement_context.visible_committed_high_watermark == 0 &&
          exec::PhysicalMgaStatementContextEqual(
              result.mga_statement_context,
              Request().table_request.mga_authority.statement_context),
      "equality ANY did not let TRUE dominate UNKNOWN");

  auto request = Request();
  request.left_value.encoded_value = "4";
  result = exec::ExecuteCanonicalQuantifiedSubquery(request);
  passed &= Require(
      result.diagnostic.ok && result.truth_value == Truth::unknown &&
          result.output_batch.rows[0].values[0].state ==
              api::EngineValueState::sql_null &&
          result.output_batch.rows[0].values[0].is_null,
      "ANY without TRUE did not preserve UNKNOWN");

  request.table_request.input_batch.rows.erase(
      request.table_request.input_batch.rows.begin() + 1);
  result = exec::ExecuteCanonicalQuantifiedSubquery(request);
  passed &= Require(result.diagnostic.ok &&
                        result.truth_value == Truth::false_value &&
                        result.comparison_count == 2 &&
                        result.output_batch.rows[0].values[0].encoded_value ==
                            "false",
                    "ANY all-FALSE result was not FALSE");

  request = Request();
  request.quantifier = Quantifier::kAll;
  request.comparison_operator = Operation::greater_than;
  request.left_value.encoded_value = "5";
  result = exec::ExecuteCanonicalQuantifiedSubquery(request);
  passed &= Require(result.diagnostic.ok &&
                        result.truth_value == Truth::unknown,
                    "ALL without FALSE did not preserve UNKNOWN");

  request.table_request.input_batch.rows.erase(
      request.table_request.input_batch.rows.begin() + 1);
  result = exec::ExecuteCanonicalQuantifiedSubquery(request);
  passed &= Require(result.diagnostic.ok &&
                        result.truth_value == Truth::true_value,
                    "ALL all-TRUE result was not TRUE");

  request = Request();
  request.quantifier = Quantifier::kAll;
  result = exec::ExecuteCanonicalQuantifiedSubquery(request);
  passed &= Require(result.diagnostic.ok &&
                        result.truth_value == Truth::false_value,
                    "ALL did not let FALSE dominate UNKNOWN");

  request = Request();
  request.table_request.input_batch.rows.clear();
  result = exec::ExecuteCanonicalQuantifiedSubquery(request);
  passed &= Require(result.diagnostic.ok &&
                        result.truth_value == Truth::false_value &&
                        result.comparison_count == 0,
                    "ANY empty-set identity was not FALSE");
  request.quantifier = Quantifier::kAll;
  result = exec::ExecuteCanonicalQuantifiedSubquery(request);
  passed &= Require(result.diagnostic.ok &&
                        result.truth_value == Truth::true_value &&
                        result.comparison_count == 0,
                    "ALL empty-set identity was not TRUE");

  request = Request();
  request.left_value = Null(request.left_operand_column.descriptor);
  result = exec::ExecuteCanonicalQuantifiedSubquery(request);
  passed &= Require(result.diagnostic.ok &&
                        result.truth_value == Truth::unknown,
                    "NULL left operand did not produce UNKNOWN comparisons");

  request = Request();
  request.table_request.input_batch.rows[2].values[0].encoded_value = "bad";
  result = exec::ExecuteCanonicalQuantifiedSubquery(request);
  passed &= Require(!result.diagnostic.ok &&
                        result.output_batch.rows.empty() &&
                        result.truth_value == Truth::unspecified &&
                        result.comparison_count == 0,
                    "early TRUE hid a malformed later operand");

  request = Real64Request();
  result = exec::ExecuteCanonicalQuantifiedSubquery(request);
  passed &= Require(
      result.diagnostic.ok && result.truth_value == Truth::true_value &&
          result.comparison_count == 3 &&
          result.output_batch.rows[0].values[0].encoded_value == "true",
      "descriptor-compatible real64 ANY comparison was refused");

  request = Real64Request();
  request.left_value.descriptor.canonical_type_name = "int64";
  result = exec::ExecuteCanonicalQuantifiedSubquery(request);
  passed &= Require(!result.diagnostic.ok && result.output_batch.rows.empty(),
                    "descriptor-incompatible quantified operands were accepted");

  request = Request();
  request.maximum_comparison_count = 2;
  result = exec::ExecuteCanonicalQuantifiedSubquery(request);
  passed &= Require(!result.diagnostic.ok,
                    "quantified comparison resource excess was accepted");

  request = Request();
  request.right_expression_descriptor_id = 2999;
  result = exec::ExecuteCanonicalQuantifiedSubquery(request);
  passed &= Require(!result.diagnostic.ok,
                    "right expression handle drift was accepted");

  request = Request();
  request.comparison_operator = Operation::unspecified;
  result = exec::ExecuteCanonicalQuantifiedSubquery(request);
  passed &= Require(!result.diagnostic.ok,
                    "unbound comparison operator was accepted");

  request = Request();
  request.quantifier = static_cast<Quantifier>(99);
  result = exec::ExecuteCanonicalQuantifiedSubquery(request);
  passed &= Require(!result.diagnostic.ok,
                    "unbound comparison quantifier was accepted");

  request = Request();
  request.result_column.nullable = false;
  result = exec::ExecuteCanonicalQuantifiedSubquery(request);
  passed &= Require(!result.diagnostic.ok,
                    "non-nullable quantified result was accepted");

  request = Request();
  request.table_request.physical_dag.local_transaction_id = 0;
  result = exec::ExecuteCanonicalQuantifiedSubquery(request);
  passed &= Require(!result.diagnostic.ok && result.output_batch.rows.empty() &&
                        result.truth_value == Truth::unspecified &&
                        result.comparison_count == 0 &&
                        result.selected_plan_uuid.empty() &&
                        !exec::PhysicalMgaStatementContextValid(
                            result.mga_statement_context),
                    "missing engine MGA transaction was accepted");

  request = Request();
  auto drift = request.table_request.mga_authority.statement_context;
  drift.statement_uuid =
      "019f0000-0000-7200-8000-00000000e647";
  request.table_request.mga_authority.resolve_current = [drift] {
    exec::CanonicalMgaCurrentResolution current;
    current.statement_context = drift;
    return current;
  };
  result = exec::ExecuteCanonicalQuantifiedSubquery(request);
  passed &= Require(!result.diagnostic.ok && result.output_batch.rows.empty(),
                    "cross-statement current MGA context reached quantified access");

  request = Request();
  request.table_request.physical_dag.mga_statement_context
      .publication_inventory_next_local_transaction_id =
      kOwnerLocalTransactionId;
  result = exec::ExecuteCanonicalQuantifiedSubquery(request);
  passed &= Require(!result.diagnostic.ok && result.output_batch.rows.empty(),
                    "truncated MGA inventory horizon reached quantified access");
  return passed;
}

}  // namespace

int main() {
  return ValidateQuantifiedSubquery() ? EXIT_SUCCESS : EXIT_FAILURE;
}
