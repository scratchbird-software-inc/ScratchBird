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
namespace dt = scratchbird::core::datatypes;

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

constexpr std::string_view kCollationUuid =
    "019f0000-0000-7400-8000-000000002701";

bool Require(const bool condition, const std::string_view detail) {
  if (!condition) {
    std::cerr << "QOW-TEST-QRY-012-NAMED-V1: " << detail << '\n';
  }
  return condition;
}

exec::PhysicalMgaStatementContext StatementContext(
    const std::string& statement_snapshot_uuid) {
  return {
      "019f0000-0000-7200-8000-00000000e531",
      "019f0000-0000-7200-8000-00000000e532",
      statement_snapshot_uuid,
      "019f0000-0000-7200-8000-00000000e533",
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
        "019f0000-0000-7200-8000-00000000e534";
    node.executor_capability_uuid =
        "019f0000-0000-7200-8000-00000000e535";
    node.executor_capability_abi_version = 1;
    node.cost_vector_uuid =
        "019f0000-0000-7200-8000-00000000e536";
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
                                 const std::string& type,
                                 const std::string& encoded) {
  api::EngineDescriptor descriptor;
  descriptor.descriptor_uuid.canonical = descriptor_uuid;
  descriptor.descriptor_kind = "scalar";
  descriptor.canonical_type_name = type;
  descriptor.encoded_descriptor = encoded;
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

dt::DatatypeTextSeedAuthority CollationSeed() {
  dt::DatatypeTextSeedAuthority seed;
  seed.active = true;
  seed.seed_pack_name = "qow.named.join.seed";
  seed.seed_pack_version = "1";
  seed.charset_name = "utf8";
  seed.collation_name = "qow_named_join_ci";
  seed.collation_case_insensitive = true;
  return seed;
}

exec::CanonicalCompositeJoinKeyTerm NameTerm() {
  exec::CanonicalCompositeJoinKeyTerm term;
  term.left_column = 1;
  term.left_expression_descriptor_id = 2702;
  term.right_column = 1;
  term.right_expression_descriptor_id = 2705;
  term.collation_uuid = kCollationUuid;
  term.resource_epoch = 71;
  term.collation_epoch = 72;
  term.text_seed = CollationSeed();
  return term;
}

exec::CanonicalNamedJoinRequest Request(
    const exec::CanonicalNamedJoinForm form) {
  const auto left_id = Descriptor(
      "019f0000-0000-7200-8000-000000002711", "int64",
      "type_uuid=019f0000-0000-7300-8000-000000002712;"
      "nullability=non_null");
  const auto left_name = Descriptor(
      "019f0000-0000-7200-8000-000000002713", "text",
      "type_uuid=019f0000-0000-7300-8000-000000002714;"
      "nullability=non_null;collation_uuid=" +
          std::string(kCollationUuid));
  const auto left_payload = Descriptor(
      "019f0000-0000-7200-8000-000000002715", "int64",
      "type_uuid=019f0000-0000-7300-8000-000000002716;"
      "nullability=non_null");
  const auto right_id = Descriptor(
      "019f0000-0000-7200-8000-000000002717", "int64",
      "type_uuid=019f0000-0000-7300-8000-000000002718;"
      "nullability=non_null");
  const auto right_name = Descriptor(
      "019f0000-0000-7200-8000-000000002719", "text",
      "type_uuid=019f0000-0000-7300-8000-000000002720;"
      "nullability=non_null;collation_uuid=" +
          std::string(kCollationUuid));
  const auto right_payload = Descriptor(
      "019f0000-0000-7200-8000-000000002721", "int64",
      "type_uuid=019f0000-0000-7300-8000-000000002722;"
      "nullability=non_null");
  const auto result_id = Descriptor(
      "019f0000-0000-7200-8000-000000002723", "int64",
      "type_uuid=019f0000-0000-7300-8000-000000002724;"
      "nullability=nullable");
  const auto result_name = Descriptor(
      "019f0000-0000-7200-8000-000000002725", "text",
      "type_uuid=019f0000-0000-7300-8000-000000002726;"
      "nullability=nullable;collation_uuid=" +
          std::string(kCollationUuid));

  exec::CanonicalNamedJoinRequest request;
  auto& key = request.key_request;
  key.physical_dag.selected_plan_uuid =
      "019f0000-0000-7200-8000-000000002727";
  key.physical_dag.root_physical_node_id = 2713;
  key.physical_dag.admission_evidence = {
      {exec::PhysicalAdmissionStage::kBoundRequest,
       "019f0000-0000-7200-8000-000000002731"},
      {exec::PhysicalAdmissionStage::kCatalogEpoch,
       "019f0000-0000-7200-8000-000000002732"},
      {exec::PhysicalAdmissionStage::kSecurity,
       "019f0000-0000-7200-8000-000000002733"},
      {exec::PhysicalAdmissionStage::kMgaStatementBoundary,
       "019f0000-0000-7200-8000-000000002734"},
      {exec::PhysicalAdmissionStage::kPolicyCapability,
       "019f0000-0000-7200-8000-000000002735"},
      {exec::PhysicalAdmissionStage::kResource,
       "019f0000-0000-7200-8000-000000002736"},
      {exec::PhysicalAdmissionStage::kStatisticsProvenance,
       "019f0000-0000-7200-8000-000000002737"},
      {exec::PhysicalAdmissionStage::kCanonicalRoute,
       "019f0000-0000-7200-8000-000000002738"},
  };
  key.physical_dag.nodes = {
      {.physical_node_id = 2711,
       .relational_node_id = 2711,
       .node_kind = exec::PhysicalNodeKind::kValues,
       .implementation_id = "values.named-left.typed.v1",
       .output_descriptor_ids = {2701, 2702, 2703},
       .causal_counter_id = 27101},
      {.physical_node_id = 2712,
       .relational_node_id = 2712,
       .node_kind = exec::PhysicalNodeKind::kValues,
       .implementation_id = "values.named-right.typed.v1",
       .output_descriptor_ids = {2704, 2705, 2706},
       .causal_counter_id = 27102},
      {.physical_node_id = 2713,
       .relational_node_id = 2713,
       .node_kind = exec::PhysicalNodeKind::kJoin,
       .implementation_id = "join.named-key.v1",
       .input_physical_node_ids = {2711, 2712},
       .output_descriptor_ids = {2701, 2702, 2703, 2704, 2705, 2706},
       .causal_counter_id = 27103},
  };
  key.selected_physical_node_id = 2713;
  key.left_batch = exec::MakeDescriptorBatch(
      {{"id", left_id, false, 2701},
       {"name", left_name, false, 2702},
       {"left_payload", left_payload, false, 2703}},
      {{{Value(left_id, "1"), Value(left_name, "A"),
         Value(left_payload, "10")}},
       {{Value(left_id, "2"), Value(left_name, "B"),
         Value(left_payload, "11")}},
       {{Value(left_id, "3"), Value(left_name, "C"),
         Value(left_payload, "12")}}});
  key.right_batch = exec::MakeDescriptorBatch(
      {{"id", right_id, false, 2704},
       {"name", right_name, false, 2705},
       {"right_payload", right_payload, false, 2706}},
      {{{Value(right_id, "1"), Value(right_name, "a"),
         Value(right_payload, "20")}},
       {{Value(right_id, "2"), Value(right_name, "X"),
         Value(right_payload, "21")}},
       {{Value(right_id, "4"), Value(right_name, "C"),
         Value(right_payload, "22")}}});

  const exec::CanonicalCompositeJoinKeyTerm id_term = {
      .left_column = 0,
      .left_expression_descriptor_id = 2701,
      .right_column = 0,
      .right_expression_descriptor_id = 2704,
  };
  const exec::CanonicalNamedJoinBinding id_binding = {
      .normalized_name = "id",
      .key_term = id_term,
      .result_column = {"id", result_id, true, 2707},
  };
  request.form = form;
  request.binding_evidence_uuid =
      "019f0000-0000-7200-8000-000000002739";
  if (form == exec::CanonicalNamedJoinForm::kUsing) {
    key.key_terms = {id_term};
    request.bindings = {id_binding};
  } else {
    const auto name_term = NameTerm();
    key.key_terms = {id_term, name_term};
    request.bindings = {
        id_binding,
        {.normalized_name = "name",
         .key_term = name_term,
         .result_column = {"name", result_name, true, 2708}},
    };
  }

  key.mga_authority = BindPhysicalAbiV2(&key.physical_dag);
  request.projection_dag = key.physical_dag;
  request.projection_dag.root_physical_node_id = 2714;
  const std::vector<std::uint32_t> projection_ids =
      form == exec::CanonicalNamedJoinForm::kUsing
          ? std::vector<std::uint32_t>{2707, 2702, 2703, 2705, 2706}
          : std::vector<std::uint32_t>{2707, 2708, 2703, 2706};
  request.projection_dag.nodes.push_back(
      {.physical_node_id = 2714,
       .relational_node_id = 2714,
       .node_kind = exec::PhysicalNodeKind::kProject,
       .implementation_id =
           form == exec::CanonicalNamedJoinForm::kUsing
               ? "join.using-projection.v1"
               : "join.natural-projection.v1",
       .input_physical_node_ids = {2713},
       .output_descriptor_ids = projection_ids,
       .causal_counter_id = 27104});
  static_cast<void>(BindPhysicalAbiV2(&request.projection_dag));
  request.selected_projection_node_id = 2714;
  return request;
}

exec::CanonicalNamedJoinRequest ZeroCommonNaturalRequest() {
  auto request = Request(exec::CanonicalNamedJoinForm::kNatural);
  request.key_request.right_batch.columns[0].stable_name = "right_id";
  request.key_request.right_batch.columns[1].stable_name = "right_name";
  request.key_request.key_terms.clear();
  request.bindings.clear();
  request.key_request.physical_dag.nodes[2].implementation_id =
      "join.natural-conditionless.typed.v1";
  request.projection_dag.nodes[2].implementation_id =
      "join.natural-conditionless.typed.v1";
  request.projection_dag.nodes.back().output_descriptor_ids =
      {2701, 2702, 2703, 2704, 2705, 2706};
  return request;
}

bool Encoded(const api::EngineTypedValue& value,
             const std::string_view expected) {
  return value.state == api::EngineValueState::value && !value.is_null &&
         value.encoded_value == expected;
}

bool IsNull(const api::EngineTypedValue& value) {
  return value.state == api::EngineValueState::sql_null && value.is_null &&
         value.encoded_value.empty() && value.binary_value.empty();
}

// QOW-TEST-QRY-012-NAMED-V1
// QOW-TEST-QRY-012-NAMED-V2
bool ValidateNamedJoin() {
  bool passed = true;
  auto result =
      exec::ExecuteCanonicalNamedJoin(Request(exec::CanonicalNamedJoinForm::kUsing));
  passed &= Require(
      result.diagnostic.ok &&
          result.form == exec::CanonicalNamedJoinForm::kUsing &&
          result.binding_count == 1 && result.matched_pair_count == 2 &&
          result.output_batch.columns.size() == 5 &&
          result.output_batch.rows.size() == 2 &&
          result.output_batch.columns[0].descriptor_id == 2707 &&
          result.output_batch.columns[1].descriptor_id == 2702 &&
          result.output_batch.columns[3].descriptor_id == 2705 &&
          Encoded(result.output_batch.rows[0].values[0], "1") &&
          Encoded(result.output_batch.rows[0].values[1], "A") &&
          Encoded(result.output_batch.rows[0].values[3], "a") &&
          Encoded(result.output_batch.rows[1].values[0], "2") &&
          Encoded(result.output_batch.rows[1].values[3], "X") &&
          result.executed_join_node_id == 2713 &&
          result.executed_projection_node_id == 2714 &&
          result.mga_statement_context
                  .visible_committed_high_watermark == 0 &&
          exec::PhysicalMgaStatementContextEqual(
              result.mga_statement_context,
              Request(exec::CanonicalNamedJoinForm::kUsing)
                  .key_request.mga_authority.statement_context),
      "USING join did not coalesce its key or retain non-key columns");

  auto request = Request(exec::CanonicalNamedJoinForm::kUsing);
  request.join_kind = exec::CanonicalAcceptedJoinKind::kFullOuter;
  result = exec::ExecuteCanonicalNamedJoin(request);
  passed &= Require(
      result.diagnostic.ok && result.matched_pair_count == 2 &&
          result.unmatched_left_row_count == 1 &&
          result.unmatched_right_row_count == 1 &&
          result.output_batch.rows.size() == 4 &&
          Encoded(result.output_batch.rows[2].values[0], "3") &&
          IsNull(result.output_batch.rows[2].values[4]) &&
          Encoded(result.output_batch.rows[3].values[0], "4") &&
          IsNull(result.output_batch.rows[3].values[2]) &&
          Encoded(result.output_batch.rows[3].values[4], "22"),
      "USING FULL OUTER did not coalesce unmatched key values");

  request = Request(exec::CanonicalNamedJoinForm::kNatural);
  request.join_kind = exec::CanonicalAcceptedJoinKind::kFullOuter;
  result = exec::ExecuteCanonicalNamedJoin(request);
  passed &= Require(
      result.diagnostic.ok &&
          result.form == exec::CanonicalNamedJoinForm::kNatural &&
          result.binding_count == 2 && result.matched_pair_count == 1 &&
          result.unmatched_left_row_count == 2 &&
          result.unmatched_right_row_count == 2 &&
          result.output_batch.columns.size() == 4 &&
          result.output_batch.rows.size() == 5 &&
          Encoded(result.output_batch.rows[0].values[0], "1") &&
          Encoded(result.output_batch.rows[0].values[1], "A") &&
          Encoded(result.output_batch.rows[0].values[2], "10") &&
          Encoded(result.output_batch.rows[0].values[3], "20") &&
          Encoded(result.output_batch.rows[3].values[0], "2") &&
          Encoded(result.output_batch.rows[3].values[1], "X") &&
          IsNull(result.output_batch.rows[3].values[2]) &&
          Encoded(result.output_batch.rows[3].values[3], "21"),
      "NATURAL FULL OUTER did not bind all common columns in left order");

  request = ZeroCommonNaturalRequest();
  result = exec::ExecuteCanonicalNamedJoin(request);
  passed &= Require(
      result.diagnostic.ok &&
          result.form == exec::CanonicalNamedJoinForm::kNatural &&
          result.binding_count == 0 && result.matched_pair_count == 9 &&
          result.unmatched_left_row_count == 0 &&
          result.unmatched_right_row_count == 0 &&
          result.output_batch.columns.size() == 6 &&
          result.output_batch.rows.size() == 9 &&
          result.output_batch.columns[0].descriptor_id == 2701 &&
          result.output_batch.columns[3].descriptor_id == 2704 &&
          Encoded(result.output_batch.rows[0].values[0], "1") &&
          Encoded(result.output_batch.rows[0].values[3], "1") &&
          Encoded(result.output_batch.rows[2].values[5], "22") &&
          Encoded(result.output_batch.rows[3].values[0], "2") &&
          Encoded(result.output_batch.rows[8].values[3], "4"),
      "zero-common NATURAL did not execute as a conditionless join");

  request = ZeroCommonNaturalRequest();
  request.join_kind = exec::CanonicalAcceptedJoinKind::kLeftOuter;
  request.key_request.right_batch.rows.clear();
  result = exec::ExecuteCanonicalNamedJoin(request);
  passed &= Require(
      result.diagnostic.ok && result.matched_pair_count == 0 &&
          result.unmatched_left_row_count == 3 &&
          result.unmatched_right_row_count == 0 &&
          result.output_batch.rows.size() == 3 &&
          result.output_batch.columns[3].nullable &&
          IsNull(result.output_batch.rows[0].values[3]) &&
          IsNull(result.output_batch.rows[2].values[5]),
      "zero-common NATURAL LEFT did not retain an empty-right outer side");

  request = ZeroCommonNaturalRequest();
  request.join_kind = exec::CanonicalAcceptedJoinKind::kRightOuter;
  request.key_request.left_batch.rows.clear();
  result = exec::ExecuteCanonicalNamedJoin(request);
  passed &= Require(
      result.diagnostic.ok && result.matched_pair_count == 0 &&
          result.unmatched_left_row_count == 0 &&
          result.unmatched_right_row_count == 3 &&
          result.output_batch.rows.size() == 3 &&
          result.output_batch.columns[0].nullable &&
          IsNull(result.output_batch.rows[0].values[0]) &&
          IsNull(result.output_batch.rows[2].values[2]) &&
          Encoded(result.output_batch.rows[2].values[3], "4"),
      "zero-common NATURAL RIGHT did not retain an empty-left outer side");

  request = ZeroCommonNaturalRequest();
  request.maximum_output_rows = 8;
  result = exec::ExecuteCanonicalNamedJoin(request);
  passed &= Require(!result.diagnostic.ok && result.output_batch.rows.empty(),
                    "zero-common NATURAL exceeded its Cartesian output bound");

  request = ZeroCommonNaturalRequest();
  request.key_request.physical_dag.nodes[2].implementation_id =
      "join.named-key.v1";
  request.projection_dag.nodes[2].implementation_id = "join.named-key.v1";
  result = exec::ExecuteCanonicalNamedJoin(request);
  passed &= Require(!result.diagnostic.ok,
                    "zero-common NATURAL used a keyed physical identity");

  request = ZeroCommonNaturalRequest();
  request.form = exec::CanonicalNamedJoinForm::kUsing;
  result = exec::ExecuteCanonicalNamedJoin(request);
  passed &= Require(!result.diagnostic.ok,
                    "empty USING binding list was accepted as NATURAL");

  request = Request(exec::CanonicalNamedJoinForm::kNatural);
  request.bindings.pop_back();
  request.key_request.key_terms.pop_back();
  result = exec::ExecuteCanonicalNamedJoin(request);
  passed &= Require(!result.diagnostic.ok && result.output_batch.rows.empty(),
                    "NATURAL join omitted a common column binding");

  request = Request(exec::CanonicalNamedJoinForm::kUsing);
  request.bindings.push_back(request.bindings.front());
  request.key_request.key_terms.push_back(
      request.key_request.key_terms.front());
  result = exec::ExecuteCanonicalNamedJoin(request);
  passed &= Require(!result.diagnostic.ok,
                    "USING join accepted a duplicate named binding");

  request = Request(exec::CanonicalNamedJoinForm::kUsing);
  request.binding_evidence_uuid.clear();
  result = exec::ExecuteCanonicalNamedJoin(request);
  passed &= Require(!result.diagnostic.ok,
                    "named join accepted missing binder evidence");

  request = Request(exec::CanonicalNamedJoinForm::kUsing);
  request.projection_dag.nodes.back().implementation_id =
      "join.natural-projection.v1";
  result = exec::ExecuteCanonicalNamedJoin(request);
  passed &= Require(!result.diagnostic.ok,
                    "named join accepted projection implementation drift");

  request = Request(exec::CanonicalNamedJoinForm::kNatural);
  request.join_kind = exec::CanonicalAcceptedJoinKind::kFullOuter;
  request.maximum_output_rows = 4;
  result = exec::ExecuteCanonicalNamedJoin(request);
  passed &= Require(!result.diagnostic.ok && result.output_batch.rows.empty(),
                    "named outer join exceeded its output bound");

  request = Request(exec::CanonicalNamedJoinForm::kUsing);
  request.join_kind = exec::CanonicalAcceptedJoinKind::kCross;
  result = exec::ExecuteCanonicalNamedJoin(request);
  passed &= Require(!result.diagnostic.ok,
                    "CROSS join entered the named-key lowering route");

  request = Request(exec::CanonicalNamedJoinForm::kUsing);
  request.projection_dag.mga_statement_context.statement_uuid =
      "019f0000-0000-7200-8000-00000000e537";
  for (auto& node : request.projection_dag.nodes) {
    node.mga_statement_context = request.projection_dag.mga_statement_context;
  }
  result = exec::ExecuteCanonicalNamedJoin(request);
  passed &= Require(!result.diagnostic.ok && result.output_batch.rows.empty() &&
                        result.selected_plan_uuid.empty() &&
                        result.executed_join_node_id == 0 &&
                        result.executed_projection_node_id == 0 &&
                        !exec::PhysicalMgaStatementContextValid(
                            result.mga_statement_context),
                    "cross-statement projection context reached named output");

  request = Request(exec::CanonicalNamedJoinForm::kUsing);
  request.key_request.physical_dag.mga_statement_context
      .publication_inventory_next_local_transaction_id =
      kOwnerLocalTransactionId;
  result = exec::ExecuteCanonicalNamedJoin(request);
  passed &= Require(!result.diagnostic.ok && result.output_batch.rows.empty(),
                    "truncated inventory boundary reached named access");
  return passed;
}

}  // namespace

int main() {
  return ValidateNamedJoin() ? EXIT_SUCCESS : EXIT_FAILURE;
}
