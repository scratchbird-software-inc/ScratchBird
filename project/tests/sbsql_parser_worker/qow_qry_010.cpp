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

constexpr std::string_view kCollationUuid =
    "019f0000-0000-7400-8000-000000001001";
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
  if (!condition) std::cerr << "QOW-TEST-QRY-010-V1: " << detail << '\n';
  return condition;
}

exec::PhysicalMgaStatementContext StatementContext(
    const std::string& statement_snapshot_uuid) {
  return {
      "019f0000-0000-7200-8000-00000000ff01",
      "019f0000-0000-7200-8000-00000000ff02",
      statement_snapshot_uuid,
      "019f0000-0000-7200-8000-00000000ff03",
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
        "019f0000-0000-7200-8000-00000000ff04";
    node.executor_capability_uuid =
        "019f0000-0000-7200-8000-00000000ff05";
    node.executor_capability_abi_version = 1;
    node.cost_vector_uuid =
        "019f0000-0000-7200-8000-00000000ff06";
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
                                 const std::string& canonical_type,
                                 const std::string& encoded_descriptor) {
  api::EngineDescriptor descriptor;
  descriptor.descriptor_uuid.canonical = descriptor_uuid;
  descriptor.descriptor_kind = "scalar";
  descriptor.canonical_type_name = canonical_type;
  descriptor.encoded_descriptor = encoded_descriptor;
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

dt::DatatypeTextSeedAuthority CollationSeed() {
  dt::DatatypeTextSeedAuthority seed;
  seed.active = true;
  seed.seed_pack_name = "qow.seed";
  seed.seed_pack_version = "1";
  seed.charset_name = "utf8";
  seed.collation_name = "qow_ci";
  seed.collation_case_insensitive = true;
  return seed;
}

exec::CanonicalDescriptorSortRequest Request() {
  const auto text_descriptor = Descriptor(
      "019f0000-0000-7200-8000-000000001002", "text",
      "type_uuid=019f0000-0000-7300-8000-000000001003;"
      "nullability=non_null;collation_uuid=" +
          std::string(kCollationUuid));
  const auto decimal_descriptor = Descriptor(
      "019f0000-0000-7200-8000-000000001004", "decimal",
      "type_uuid=019f0000-0000-7300-8000-000000001005;"
      "nullability=nullable;precision=12;scale=2");
  const auto int64_descriptor = Descriptor(
      "019f0000-0000-7200-8000-000000001006", "int64",
      "type_uuid=019f0000-0000-7300-8000-000000001007;"
      "nullability=non_null");

  exec::CanonicalDescriptorSortRequest request;
  request.physical_dag.selected_plan_uuid =
      "019f0000-0000-7200-8000-000000001008";
  request.physical_dag.root_physical_node_id = 1012;
  request.physical_dag.admission_evidence = {
      {exec::PhysicalAdmissionStage::kBoundRequest,
       "019f0000-0000-7200-8000-000000001011"},
      {exec::PhysicalAdmissionStage::kCatalogEpoch,
       "019f0000-0000-7200-8000-000000001012"},
      {exec::PhysicalAdmissionStage::kSecurity,
       "019f0000-0000-7200-8000-000000001013"},
      {exec::PhysicalAdmissionStage::kMgaStatementBoundary,
       "019f0000-0000-7200-8000-000000001014"},
      {exec::PhysicalAdmissionStage::kPolicyCapability,
       "019f0000-0000-7200-8000-000000001015"},
      {exec::PhysicalAdmissionStage::kResource,
       "019f0000-0000-7200-8000-000000001016"},
      {exec::PhysicalAdmissionStage::kStatisticsProvenance,
       "019f0000-0000-7200-8000-000000001017"},
      {exec::PhysicalAdmissionStage::kCanonicalRoute,
       "019f0000-0000-7200-8000-000000001018"},
  };
  request.physical_dag.nodes = {
      {.physical_node_id = 1011,
       .relational_node_id = 1011,
       .node_kind = exec::PhysicalNodeKind::kValues,
       .implementation_id = "values.typed.v1",
       .output_descriptor_ids = {1001, 1002, 1003},
       .causal_counter_id = 10101},
      {.physical_node_id = 1012,
       .relational_node_id = 1012,
       .node_kind = exec::PhysicalNodeKind::kSort,
       .implementation_id = "sort.typed.terms.v1",
       .input_physical_node_ids = {1011},
       .output_descriptor_ids = {1001, 1002, 1003},
       .causal_counter_id = 10102},
  };
  request.selected_physical_node_id = 1012;
  request.input_batch = exec::MakeDescriptorBatch(
      {{"normalized_name", text_descriptor, false, 1001},
       {"amount", decimal_descriptor, true, 1002},
       {"row_id", int64_descriptor, false, 1003}},
      {{{Value(text_descriptor, "b"), Null(decimal_descriptor),
         Value(int64_descriptor, "3")}},
       {{Value(text_descriptor, "A"), Value(decimal_descriptor, "2.00"),
         Value(int64_descriptor, "2")}},
       {{Value(text_descriptor, "a"), Value(decimal_descriptor, "2.00"),
         Value(int64_descriptor, "1")}},
       {{Value(text_descriptor, "A"), Value(decimal_descriptor, "1.00"),
         Value(int64_descriptor, "4")}},
       {{Value(text_descriptor, "b"), Value(decimal_descriptor, "5.00"),
         Value(int64_descriptor, "5")}}});

  exec::CanonicalDescriptorOrderTerm name_term;
  name_term.column = 0;
  name_term.expression_descriptor_id = 1001;
  name_term.direction = exec::CanonicalDescriptorOrderDirection::ascending;
  name_term.null_placement = exec::CanonicalDescriptorNullPlacement::last;
  name_term.collation_uuid = kCollationUuid;
  name_term.resource_epoch = 41;
  name_term.collation_epoch = 42;
  name_term.text_seed = CollationSeed();

  exec::CanonicalDescriptorOrderTerm amount_term;
  amount_term.column = 1;
  amount_term.expression_descriptor_id = 1002;
  amount_term.direction = exec::CanonicalDescriptorOrderDirection::descending;
  amount_term.null_placement = exec::CanonicalDescriptorNullPlacement::last;

  exec::CanonicalDescriptorOrderTerm id_term;
  id_term.column = 2;
  id_term.expression_descriptor_id = 1003;
  id_term.direction = exec::CanonicalDescriptorOrderDirection::ascending;
  id_term.null_placement = exec::CanonicalDescriptorNullPlacement::last;

  request.order_terms = {name_term, amount_term, id_term};
  request.deterministic_tie_evidence_uuid =
      "019f0000-0000-7200-8000-000000001019";
  request.mga_authority = BindPhysicalAbiV2(&request.physical_dag);
  return request;
}

exec::CanonicalDescriptorDistinctRequest DistinctRequest() {
  const auto text_descriptor = Descriptor(
      "019f0000-0000-7200-8000-000000001102", "text",
      "type_uuid=019f0000-0000-7300-8000-000000001103;"
      "nullability=non_null;collation_uuid=" +
          std::string(kCollationUuid));
  const auto decimal_descriptor = Descriptor(
      "019f0000-0000-7200-8000-000000001104", "decimal",
      "type_uuid=019f0000-0000-7300-8000-000000001105;"
      "nullability=nullable;precision=12;scale=2");

  exec::CanonicalDescriptorDistinctRequest request;
  request.physical_dag.selected_plan_uuid =
      "019f0000-0000-7200-8000-000000001108";
  request.physical_dag.root_physical_node_id = 1113;
  request.physical_dag.admission_evidence = {
      {exec::PhysicalAdmissionStage::kBoundRequest,
       "019f0000-0000-7200-8000-000000001111"},
      {exec::PhysicalAdmissionStage::kCatalogEpoch,
       "019f0000-0000-7200-8000-000000001112"},
      {exec::PhysicalAdmissionStage::kSecurity,
       "019f0000-0000-7200-8000-000000001113"},
      {exec::PhysicalAdmissionStage::kMgaStatementBoundary,
       "019f0000-0000-7200-8000-000000001114"},
      {exec::PhysicalAdmissionStage::kPolicyCapability,
       "019f0000-0000-7200-8000-000000001115"},
      {exec::PhysicalAdmissionStage::kResource,
       "019f0000-0000-7200-8000-000000001116"},
      {exec::PhysicalAdmissionStage::kStatisticsProvenance,
       "019f0000-0000-7200-8000-000000001117"},
      {exec::PhysicalAdmissionStage::kCanonicalRoute,
       "019f0000-0000-7200-8000-000000001118"},
  };
  request.physical_dag.nodes = {
      {.physical_node_id = 1111,
       .relational_node_id = 1111,
       .node_kind = exec::PhysicalNodeKind::kValues,
       .implementation_id = "values.typed.v1",
       .output_descriptor_ids = {1101, 1102},
       .causal_counter_id = 11101},
      {.physical_node_id = 1112,
       .relational_node_id = 1112,
       .node_kind = exec::PhysicalNodeKind::kAggregate,
       .implementation_id = "aggregate.query-distinct.typed.v1",
       .input_physical_node_ids = {1111},
       .output_descriptor_ids = {1101, 1102},
       .causal_counter_id = 11102},
      {.physical_node_id = 1113,
       .relational_node_id = 1113,
       .node_kind = exec::PhysicalNodeKind::kSort,
       .implementation_id = "sort.typed.terms.v1",
       .input_physical_node_ids = {1112},
       .output_descriptor_ids = {1101, 1102},
       .causal_counter_id = 11103},
  };
  request.selected_physical_node_id = 1112;
  request.input_batch = exec::MakeDescriptorBatch(
      {{"label", text_descriptor, false, 1101},
       {"amount", decimal_descriptor, true, 1102}},
      {{{Value(text_descriptor, "A"), Value(decimal_descriptor, "2.00")}},
       {{Value(text_descriptor, "a"), Value(decimal_descriptor, "2.00")}},
       {{Value(text_descriptor, "B"), Null(decimal_descriptor)}},
       {{Value(text_descriptor, "b"), Null(decimal_descriptor)}},
       {{Value(text_descriptor, "c"), Value(decimal_descriptor, "3.00")}}});

  exec::CanonicalDescriptorOrderTerm label_term;
  label_term.column = 0;
  label_term.expression_descriptor_id = 1101;
  label_term.direction = exec::CanonicalDescriptorOrderDirection::ascending;
  label_term.null_placement = exec::CanonicalDescriptorNullPlacement::first;
  label_term.collation_uuid = kCollationUuid;
  label_term.resource_epoch = 41;
  label_term.collation_epoch = 42;
  label_term.text_seed = CollationSeed();

  exec::CanonicalDescriptorOrderTerm amount_term;
  amount_term.column = 1;
  amount_term.expression_descriptor_id = 1102;
  amount_term.direction = exec::CanonicalDescriptorOrderDirection::ascending;
  amount_term.null_placement = exec::CanonicalDescriptorNullPlacement::first;
  request.equality_terms = {label_term, amount_term};
  request.maximum_value_comparisons = 128;
  request.mga_authority = BindPhysicalAbiV2(&request.physical_dag);
  return request;
}

std::vector<std::string> RowIds(const exec::DescriptorBatch& batch) {
  std::vector<std::string> ids;
  for (const auto& row : batch.rows) {
    ids.push_back(row.values[2].encoded_value);
  }
  return ids;
}

// QOW-TEST-QRY-010-V1
bool ValidateTypedPhysicalOrdering() {
  bool passed = true;
  auto result = exec::ExecuteCanonicalDescriptorSort(Request());
  passed &= Require(result.diagnostic.ok &&
                        result.executed_physical_node_id == 1012 &&
                        result.causal_counter_id == 10102 &&
                        result.mga_statement_context
                                .visible_committed_high_watermark == 0 &&
                        exec::PhysicalMgaStatementContextEqual(
                            result.mga_statement_context,
                            Request().mga_authority.statement_context),
                    "typed physical sort node was not executable");
  passed &= Require(RowIds(result.output_batch) ==
                        std::vector<std::string>({"1", "2", "4", "5", "3"}),
                    "multi-term collation, DESC, or NULLS LAST order is wrong");
  passed &= Require(result.output_batch.columns.size() == 3 &&
                        result.output_batch.columns[0].descriptor_id == 1001 &&
                        result.output_batch.columns[2].descriptor_id == 1003,
                    "sort changed bound output descriptor handles");

  auto request = Request();
  request.order_terms.resize(2);
  result = exec::ExecuteCanonicalDescriptorSort(request);
  passed &= Require(RowIds(result.output_batch) ==
                        std::vector<std::string>({"2", "1", "4", "5", "3"}),
                    "equal order keys did not retain deterministic input order");

  request = Request();
  request.input_batch.rows[1].values[1].encoded_value = "not-a-decimal";
  result = exec::ExecuteCanonicalDescriptorSort(request);
  passed &= Require(!result.diagnostic.ok && result.output_batch.rows.empty(),
                    "malformed numeric operand produced partial sort output");

  request = Request();
  request.order_terms[0].resource_epoch = 0;
  result = exec::ExecuteCanonicalDescriptorSort(request);
  passed &= Require(!result.diagnostic.ok && result.output_batch.rows.empty(),
                    "unbound collation resource authority was accepted");

  request = Request();
  request.deterministic_tie_evidence_uuid.clear();
  result = exec::ExecuteCanonicalDescriptorSort(request);
  passed &= Require(!result.diagnostic.ok && result.output_batch.rows.empty(),
                    "missing deterministic tie evidence was accepted");

  request = Request();
  request.order_terms[0].expression_descriptor_id = 1002;
  result = exec::ExecuteCanonicalDescriptorSort(request);
  passed &= Require(!result.diagnostic.ok && result.output_batch.rows.empty(),
                    "mismatched expression descriptor handle was accepted");

  request = Request();
  request.maximum_pair_comparisons = 24;
  result = exec::ExecuteCanonicalDescriptorSort(request);
  passed &= Require(!result.diagnostic.ok && result.output_batch.rows.empty(),
                    "comparison resource limit was exceeded");

  request = Request();
  request.input_batch.rows.clear();
  result = exec::ExecuteCanonicalDescriptorSort(request);
  passed &= Require(result.diagnostic.ok && result.output_batch.rows.empty() &&
                        result.output_batch.columns.size() == 3,
                    "empty typed input lost its bound output schema");

  request = Request();
  request.mga_authority = {};
  result = exec::ExecuteCanonicalDescriptorSort(request);
  passed &= Require(!result.diagnostic.ok && result.output_batch.rows.empty(),
                    "missing current MGA authority reached sort access");

  request = Request();
  auto swapped = request.physical_dag.mga_statement_context;
  std::swap(swapped.statement_uuid, swapped.owning_transaction_uuid);
  SetStatementContext(&request.physical_dag, swapped);
  result = exec::ExecuteCanonicalDescriptorSort(request);
  passed &= Require(!result.diagnostic.ok && result.output_batch.rows.empty(),
                    "swapped statement/transaction identity reached sort access");

  request = Request();
  request.physical_dag.local_transaction_id =
      static_cast<std::uint32_t>(kOwnerLocalTransactionId);
  result = exec::ExecuteCanonicalDescriptorSort(request);
  passed &= Require(!result.diagnostic.ok && result.output_batch.rows.empty(),
                    "narrowed numeric transaction alias reached sort access");

  request = Request();
  auto duplicate = request.physical_dag.mga_statement_context;
  duplicate.active_excluded_local_transaction_ids.push_back(
      kOwnerLocalTransactionId);
  SetStatementContext(&request.physical_dag, duplicate);
  request.mga_authority.statement_context = duplicate;
  result = exec::ExecuteCanonicalDescriptorSort(request);
  passed &= Require(!result.diagnostic.ok && result.output_batch.rows.empty(),
                    "duplicate snapshot exclusion reached sort access");
  return passed;
}

bool ValidateTypedQueryDistinct() {
  bool passed = true;
  auto request = DistinctRequest();
  auto result = exec::ExecuteCanonicalDescriptorDistinct(request);
  passed &= Require(
      result.diagnostic.ok && result.output_batch.rows.size() == 3 &&
          result.eliminated_duplicate_row_count == 2 &&
          result.value_comparison_count != 0 &&
          result.executed_physical_node_id == 1112 &&
          result.causal_counter_id == 11102 &&
          result.output_batch.rows[0].values[0].encoded_value == "A" &&
          result.output_batch.rows[1].values[0].encoded_value == "B" &&
          result.output_batch.rows[1].values[1].state ==
              api::EngineValueState::sql_null &&
          result.output_batch.rows[2].values[0].encoded_value == "c" &&
          exec::PhysicalMgaStatementContextEqual(
              result.mga_statement_context,
              request.mga_authority.statement_context),
      "query DISTINCT did not execute as an interior typed distinct-aggregate "
      "with NULL and bound-collation equality");

  request = DistinctRequest();
  request.maximum_value_comparisons = 1;
  result = exec::ExecuteCanonicalDescriptorDistinct(request);
  passed &= Require(!result.diagnostic.ok && result.output_batch.rows.empty(),
                    "DISTINCT comparison excess published partial rows");

  request = DistinctRequest();
  request.equality_terms.pop_back();
  result = exec::ExecuteCanonicalDescriptorDistinct(request);
  passed &= Require(!result.diagnostic.ok && result.output_batch.rows.empty(),
                    "incomplete DISTINCT descriptor coverage was accepted");

  request = DistinctRequest();
  request.input_batch.rows[1].values[1].encoded_value = "malformed";
  result = exec::ExecuteCanonicalDescriptorDistinct(request);
  passed &= Require(
      !result.diagnostic.ok && result.output_batch.rows.empty(),
      "duplicate position hid malformed DISTINCT typed input");

  request = DistinctRequest();
  request.physical_dag.nodes[1].implementation_id =
      "aggregate.grouped.typed.v1";
  result = exec::ExecuteCanonicalDescriptorDistinct(request);
  passed &= Require(!result.diagnostic.ok && result.output_batch.rows.empty(),
                    "non-DISTINCT aggregate entered query duplicate removal");
  return passed;
}

}  // namespace

int main() {
  return ValidateTypedPhysicalOrdering() && ValidateTypedQueryDistinct()
             ? EXIT_SUCCESS
             : EXIT_FAILURE;
}
