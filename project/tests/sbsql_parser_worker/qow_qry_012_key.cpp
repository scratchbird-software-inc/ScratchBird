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
    "019f0000-0000-7400-8000-000000001921";

bool Require(const bool condition, const std::string_view detail) {
  if (!condition) {
    std::cerr << "QOW-TEST-QRY-012-KEY-V1: " << detail << '\n';
  }
  return condition;
}

exec::PhysicalMgaStatementContext StatementContext(
    const std::string& statement_snapshot_uuid) {
  return {
      "019f0000-0000-7200-8000-00000000e501",
      "019f0000-0000-7200-8000-00000000e502",
      statement_snapshot_uuid,
      "019f0000-0000-7200-8000-00000000e503",
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
        "019f0000-0000-7200-8000-00000000e504";
    node.executor_capability_uuid =
        "019f0000-0000-7200-8000-00000000e505";
    node.executor_capability_abi_version = 1;
    node.cost_vector_uuid =
        "019f0000-0000-7200-8000-00000000e506";
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
                                 const std::string& type_uuid) {
  api::EngineDescriptor descriptor;
  descriptor.descriptor_uuid.canonical = descriptor_uuid;
  descriptor.descriptor_kind = "scalar";
  descriptor.canonical_type_name = "int64";
  descriptor.encoded_descriptor =
      "type_uuid=" + type_uuid + ";nullability=nullable";
  return descriptor;
}

api::EngineDescriptor TypedDescriptor(
    const std::string& descriptor_uuid,
    const std::string& canonical_type_name,
    const std::string& encoded_descriptor) {
  api::EngineDescriptor descriptor;
  descriptor.descriptor_uuid.canonical = descriptor_uuid;
  descriptor.descriptor_kind = "scalar";
  descriptor.canonical_type_name = canonical_type_name;
  descriptor.encoded_descriptor = encoded_descriptor;
  return descriptor;
}

dt::DatatypeTextSeedAuthority CollationSeed() {
  dt::DatatypeTextSeedAuthority seed;
  seed.active = true;
  seed.seed_pack_name = "qow.join.seed";
  seed.seed_pack_version = "1";
  seed.charset_name = "utf8";
  seed.collation_name = "qow_join_ci";
  seed.collation_case_insensitive = true;
  return seed;
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

exec::CanonicalCompositeJoinKeyRequest Request() {
  const auto left_first = Descriptor(
      "019f0000-0000-7200-8000-000000001901",
      "019f0000-0000-7300-8000-000000001902");
  const auto left_second = Descriptor(
      "019f0000-0000-7200-8000-000000001903",
      "019f0000-0000-7300-8000-000000001904");
  const auto right_first = Descriptor(
      "019f0000-0000-7200-8000-000000001905",
      "019f0000-0000-7300-8000-000000001906");
  const auto right_second = Descriptor(
      "019f0000-0000-7200-8000-000000001907",
      "019f0000-0000-7300-8000-000000001908");

  exec::CanonicalCompositeJoinKeyRequest request;
  request.physical_dag.selected_plan_uuid =
      "019f0000-0000-7200-8000-000000001909";
  request.physical_dag.root_physical_node_id = 1903;
  request.physical_dag.admission_evidence = {
      {exec::PhysicalAdmissionStage::kBoundRequest,
       "019f0000-0000-7200-8000-000000001911"},
      {exec::PhysicalAdmissionStage::kCatalogEpoch,
       "019f0000-0000-7200-8000-000000001912"},
      {exec::PhysicalAdmissionStage::kSecurity,
       "019f0000-0000-7200-8000-000000001913"},
      {exec::PhysicalAdmissionStage::kMgaStatementBoundary,
       "019f0000-0000-7200-8000-000000001914"},
      {exec::PhysicalAdmissionStage::kPolicyCapability,
       "019f0000-0000-7200-8000-000000001915"},
      {exec::PhysicalAdmissionStage::kResource,
       "019f0000-0000-7200-8000-000000001916"},
      {exec::PhysicalAdmissionStage::kStatisticsProvenance,
       "019f0000-0000-7200-8000-000000001917"},
      {exec::PhysicalAdmissionStage::kCanonicalRoute,
       "019f0000-0000-7200-8000-000000001918"},
  };
  request.physical_dag.nodes = {
      {.physical_node_id = 1901,
       .relational_node_id = 1901,
       .node_kind = exec::PhysicalNodeKind::kValues,
       .implementation_id = "values.left.typed.v1",
       .output_descriptor_ids = {1901, 1902},
       .causal_counter_id = 19001},
      {.physical_node_id = 1902,
       .relational_node_id = 1902,
       .node_kind = exec::PhysicalNodeKind::kValues,
       .implementation_id = "values.right.typed.v1",
       .output_descriptor_ids = {1903, 1904},
       .causal_counter_id = 19002},
      {.physical_node_id = 1903,
       .relational_node_id = 1903,
       .node_kind = exec::PhysicalNodeKind::kJoin,
       .implementation_id = "join.composite-int64-key.v1",
       .input_physical_node_ids = {1901, 1902},
       .output_descriptor_ids = {1901, 1902, 1903, 1904},
       .causal_counter_id = 19003},
  };
  request.selected_physical_node_id = 1903;
  request.left_batch = exec::MakeDescriptorBatch(
      {{"left_first", left_first, true, 1901},
       {"left_second", left_second, true, 1902}},
      {{{Value(left_first, "1"), Value(left_second, "10")}},
       {{Value(left_first, "2"), Null(left_second)}},
       {{Value(left_first, "01"), Value(left_second, "20")}}});
  request.right_batch = exec::MakeDescriptorBatch(
      {{"right_first", right_first, true, 1903},
       {"right_second", right_second, true, 1904}},
      {{{Value(right_first, "1"), Value(right_second, "10")}},
       {{Value(right_first, "1"), Null(right_second)}},
       {{Value(right_first, "2"), Value(right_second, "99")}},
       {{Value(right_first, "2"), Null(right_second)}}});
  request.key_terms = {
      {.left_column = 0,
       .left_expression_descriptor_id = 1901,
       .right_column = 0,
       .right_expression_descriptor_id = 1903},
      {.left_column = 1,
       .left_expression_descriptor_id = 1902,
       .right_column = 1,
       .right_expression_descriptor_id = 1904},
  };
  request.mga_authority = BindPhysicalAbiV2(&request.physical_dag);
  return request;
}

exec::CanonicalCompositeJoinKeyRequest TypedRequest() {
  auto request = Request();
  const auto left_text = TypedDescriptor(
      "019f0000-0000-7200-8000-000000001931", "text",
      "type_uuid=019f0000-0000-7300-8000-000000001932;"
      "nullability=nullable;collation_uuid=" +
          std::string(kCollationUuid));
  const auto left_decimal = TypedDescriptor(
      "019f0000-0000-7200-8000-000000001933", "decimal",
      "type_uuid=019f0000-0000-7300-8000-000000001934;"
      "nullability=non_null;precision=12;scale=2");
  const auto left_boolean = TypedDescriptor(
      "019f0000-0000-7200-8000-000000001935", "boolean",
      "type_uuid=019f0000-0000-7300-8000-000000001936;"
      "nullability=non_null");
  const auto right_text = TypedDescriptor(
      "019f0000-0000-7200-8000-000000001937", "text",
      "type_uuid=019f0000-0000-7300-8000-000000001938;"
      "nullability=nullable;collation_uuid=" +
          std::string(kCollationUuid));
  const auto right_decimal = TypedDescriptor(
      "019f0000-0000-7200-8000-000000001939", "decimal",
      "type_uuid=019f0000-0000-7300-8000-000000001940;"
      "nullability=non_null;precision=12;scale=2");
  const auto right_boolean = TypedDescriptor(
      "019f0000-0000-7200-8000-000000001941", "boolean",
      "type_uuid=019f0000-0000-7300-8000-000000001942;"
      "nullability=non_null");

  request.physical_dag.nodes[0].output_descriptor_ids = {1901, 1902, 1905};
  request.physical_dag.nodes[1].output_descriptor_ids = {1903, 1904, 1906};
  request.physical_dag.nodes[2].output_descriptor_ids =
      {1901, 1902, 1905, 1903, 1904, 1906};
  request.physical_dag.nodes[2].implementation_id =
      "join.composite-typed-key.v1";
  request.left_batch = exec::MakeDescriptorBatch(
      {{"left_text", left_text, true, 1901},
       {"left_decimal", left_decimal, false, 1902},
       {"left_boolean", left_boolean, false, 1905}},
      {{{Value(left_text, "A"), Value(left_decimal, "1.00"),
         Value(left_boolean, "true")}},
       {{Value(left_text, "b"), Value(left_decimal, "2.0"),
         Value(left_boolean, "false")}},
       {{Null(left_text), Value(left_decimal, "1.0"),
         Value(left_boolean, "true")}}});
  request.right_batch = exec::MakeDescriptorBatch(
      {{"right_text", right_text, true, 1903},
       {"right_decimal", right_decimal, false, 1904},
       {"right_boolean", right_boolean, false, 1906}},
      {{{Value(right_text, "a"), Value(right_decimal, "1.0"),
         Value(right_boolean, "true")}},
       {{Value(right_text, "B"), Value(right_decimal, "2.00"),
         Value(right_boolean, "false")}},
       {{Null(right_text), Value(right_decimal, "1.00"),
         Value(right_boolean, "true")}}});

  exec::CanonicalCompositeJoinKeyTerm text_term;
  text_term.left_column = 0;
  text_term.left_expression_descriptor_id = 1901;
  text_term.right_column = 0;
  text_term.right_expression_descriptor_id = 1903;
  text_term.collation_uuid = kCollationUuid;
  text_term.resource_epoch = 51;
  text_term.collation_epoch = 52;
  text_term.text_seed = CollationSeed();
  request.key_terms = {
      text_term,
      {.left_column = 1,
       .left_expression_descriptor_id = 1902,
       .right_column = 1,
       .right_expression_descriptor_id = 1904},
      {.left_column = 2,
       .left_expression_descriptor_id = 1905,
       .right_column = 2,
       .right_expression_descriptor_id = 1906},
  };
  request.maximum_key_comparisons = 27;
  return request;
}

// QOW-TEST-QRY-012-KEY-V1
bool ValidateCompositeJoinKey() {
  using Truth = api::EngineSqlTruthValue;
  bool passed = true;
  auto result = exec::ExecuteCanonicalCompositeJoinKey(Request());
  const std::vector<Truth> expected = {
      Truth::true_value,  Truth::unknown, Truth::false_value,
      Truth::false_value, Truth::false_value, Truth::false_value,
      Truth::unknown,     Truth::unknown, Truth::false_value,
      Truth::unknown,     Truth::false_value, Truth::false_value,
  };
  passed &= Require(result.diagnostic.ok && result.pair_count == 12 &&
                        result.pair_truth_values == expected &&
                        result.executed_physical_node_id == 1903 &&
                        result.mga_statement_context
                                .visible_committed_high_watermark == 0 &&
                        exec::PhysicalMgaStatementContextEqual(
                            result.mga_statement_context,
                            Request().mga_authority.statement_context),
                    "composite equality key produced the wrong 3VL matrix");

  // QOW-TEST-QRY-012-KEY-TYPED-V1
  result = exec::ExecuteCanonicalCompositeJoinKey(TypedRequest());
  const std::vector<Truth> expected_typed = {
      Truth::true_value, Truth::false_value, Truth::unknown,
      Truth::false_value, Truth::true_value, Truth::false_value,
      Truth::unknown, Truth::false_value, Truth::unknown,
  };
  passed &= Require(
      result.diagnostic.ok && result.pair_count == 9 &&
          result.pair_truth_values == expected_typed &&
          result.executed_physical_node_id == 1903,
      "collated text, decimal, and boolean key terms produced the wrong 3VL "
      "matrix");

  auto typed_request = TypedRequest();
  typed_request.key_terms[0].resource_epoch = 0;
  result = exec::ExecuteCanonicalCompositeJoinKey(typed_request);
  passed &= Require(!result.diagnostic.ok && result.pair_truth_values.empty(),
                    "typed join accepted unbound collation authority");

  typed_request = TypedRequest();
  typed_request.right_batch.columns[0].descriptor.encoded_descriptor =
      "type_uuid=019f0000-0000-7300-8000-000000001938;"
      "nullability=nullable;collation_uuid="
      "019f0000-0000-7400-8000-000000001922";
  for (auto& row : typed_request.right_batch.rows) {
    row.values[0].descriptor = typed_request.right_batch.columns[0].descriptor;
  }
  result = exec::ExecuteCanonicalCompositeJoinKey(typed_request);
  passed &= Require(!result.diagnostic.ok,
                    "typed join accepted cross-collation equality");

  typed_request = TypedRequest();
  typed_request.right_batch.rows[2].values[1].encoded_value =
      "not-a-decimal";
  result = exec::ExecuteCanonicalCompositeJoinKey(typed_request);
  passed &= Require(!result.diagnostic.ok && result.pair_truth_values.empty(),
                    "malformed typed key published a partial matrix");

  auto request = Request();
  request.left_batch.rows.clear();
  result = exec::ExecuteCanonicalCompositeJoinKey(request);
  passed &= Require(result.diagnostic.ok && result.pair_count == 0 &&
                        result.pair_truth_values.empty(),
                    "empty join side invented key comparisons");

  request = Request();
  request.maximum_key_comparisons = 23;
  result = exec::ExecuteCanonicalCompositeJoinKey(request);
  passed &= Require(!result.diagnostic.ok && result.pair_truth_values.empty(),
                    "composite key comparison bound was exceeded");

  request = Request();
  request.key_terms.push_back(request.key_terms.front());
  result = exec::ExecuteCanonicalCompositeJoinKey(request);
  passed &= Require(!result.diagnostic.ok && result.pair_truth_values.empty(),
                    "duplicate composite key handle was accepted");

  request = Request();
  request.key_terms[0].left_expression_descriptor_id = 9999;
  result = exec::ExecuteCanonicalCompositeJoinKey(request);
  passed &= Require(!result.diagnostic.ok,
                    "unbound composite key handle was accepted");

  request = Request();
  request.left_batch.rows[2].values[0].encoded_value = "malformed";
  result = exec::ExecuteCanonicalCompositeJoinKey(request);
  passed &= Require(!result.diagnostic.ok && result.pair_truth_values.empty(),
                    "malformed key operand published a partial matrix");

  request = Request();
  request.right_batch.rows[1].values.pop_back();
  result = exec::ExecuteCanonicalCompositeJoinKey(request);
  passed &= Require(!result.diagnostic.ok && result.pair_truth_values.empty(),
                    "ragged join input published a partial matrix");

  request = Request();
  request.physical_dag.nodes.back().output_descriptor_ids =
      {1903, 1904, 1901, 1902};
  result = exec::ExecuteCanonicalCompositeJoinKey(request);
  passed &= Require(!result.diagnostic.ok,
                    "unbound composite join output order was accepted");

  request = Request();
  request.physical_dag.local_transaction_id = 0;
  result = exec::ExecuteCanonicalCompositeJoinKey(request);
  passed &= Require(!result.diagnostic.ok &&
                        result.pair_truth_values.empty() &&
                        result.selected_plan_uuid.empty() &&
                        result.executed_physical_node_id == 0 &&
                        !exec::PhysicalMgaStatementContextValid(
                            result.mga_statement_context),
                    "composite join key bypassed MGA physical admission");

  request = Request();
  request.mga_authority.resolve_current = {};
  result = exec::ExecuteCanonicalCompositeJoinKey(request);
  passed &= Require(!result.diagnostic.ok && result.pair_truth_values.empty(),
                    "missing current MGA authority reached key access");

  request = Request();
  request.physical_dag.mga_statement_context.statement_snapshot_uuid[0] =
      'A';
  result = exec::ExecuteCanonicalCompositeJoinKey(request);
  passed &= Require(!result.diagnostic.ok && result.pair_truth_values.empty(),
                    "malformed snapshot UUID reached key access");

  request = Request();
  request.physical_dag.nodes.back().mga_statement_context.statement_uuid =
      "019f0000-0000-7200-8000-00000000e507";
  result = exec::ExecuteCanonicalCompositeJoinKey(request);
  passed &= Require(!result.diagnostic.ok && result.pair_truth_values.empty(),
                    "node-swapped statement context reached key access");
  return passed;
}

}  // namespace

int main() {
  return ValidateCompositeJoinKey() ? EXIT_SUCCESS : EXIT_FAILURE;
}
