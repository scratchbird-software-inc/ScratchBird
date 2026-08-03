// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "descriptor_value_runtime.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace exec = scratchbird::engine::executor;
namespace api = scratchbird::engine::internal_api;

namespace {

bool Require(const bool condition, const std::string_view detail) {
  if (!condition) std::cerr << "QOW-TEST-QRY-007-FILTER-V1: " << detail << '\n';
  return condition;
}

std::string ContextUuid(const unsigned value) {
  char buffer[37]{};
  std::snprintf(buffer, sizeof(buffer),
                "019f0000-0000-7630-8000-%012u", value);
  return buffer;
}

exec::CanonicalExecutionMgaAuthority ClosureAuthority(
    const exec::PhysicalMgaStatementContext& carried,
    const exec::PhysicalMgaStatementContext& current) {
  exec::CanonicalExecutionMgaAuthority authority;
  authority.statement_context = carried;
  authority.origin = exec::CanonicalMgaAuthorityOrigin::kClosureTestSeam;
  authority.resolve_current = [current] {
    exec::CanonicalMgaCurrentResolution resolution;
    resolution.statement_context = current;
    return resolution;
  };
  return authority;
}

exec::CanonicalExecutionMgaAuthority ClosureAuthority(
    const exec::PhysicalMgaStatementContext& context) {
  return ClosureAuthority(context, context);
}

void UpgradeToCanonicalStatementContext(exec::TypedPhysicalNodeDag* dag,
                                        const unsigned seed) {
  const auto local = (std::uint64_t{1} << 32) + dag->local_transaction_id;
  const auto highwater =
      (std::uint64_t{1} << 32) + dag->statement_snapshot_id;
  dag->abi_version = 2;
  dag->local_transaction_id = local;
  dag->statement_snapshot_id = highwater;
  dag->mga_statement_context = {
      ContextUuid(seed + 1), ContextUuid(seed + 2), ContextUuid(seed + 3),
      ContextUuid(seed + 4), local, highwater, local, local, local, local,
      {local}, {}, "statement_stable", highwater + 1, true, true, true};
  dag->bound_sblr_tree_uuid = ContextUuid(seed + 10);
  dag->catalog_epoch_uuid = ContextUuid(seed + 11);
  dag->security_context_uuid = ContextUuid(seed + 12);
  dag->capability_snapshot_uuid = ContextUuid(seed + 13);
  dag->resource_snapshot_uuid = ContextUuid(seed + 14);
  dag->statistics_snapshot_uuid = ContextUuid(seed + 15);
  dag->route_snapshot_uuid = ContextUuid(seed + 16);
  dag->catalog_generation = seed + 21;
  dag->security_epoch = seed + 22;
  dag->policy_epoch = seed + 23;
  dag->resource_epoch = seed + 24;
  dag->statistics_generation = seed + 25;
  dag->route_epoch = seed + 26;
  dag->route_generation = seed + 27;
  dag->memory_budget_bytes = 1U << 20;
  dag->optimizer_published = true;
  dag->immutable_node_identity_validated = true;
  dag->capability_validated_before_access = true;
  dag->admission_evidence = {
      {exec::PhysicalAdmissionStage::kBoundRequest, dag->bound_sblr_tree_uuid},
      {exec::PhysicalAdmissionStage::kCatalogEpoch, dag->catalog_epoch_uuid},
      {exec::PhysicalAdmissionStage::kSecurity, dag->security_context_uuid},
      {exec::PhysicalAdmissionStage::kMgaStatementBoundary,
       dag->mga_statement_context.statement_snapshot_uuid},
      {exec::PhysicalAdmissionStage::kPolicyCapability,
       dag->capability_snapshot_uuid},
      {exec::PhysicalAdmissionStage::kResource, dag->resource_snapshot_uuid},
      {exec::PhysicalAdmissionStage::kStatisticsProvenance,
       dag->statistics_snapshot_uuid},
      {exec::PhysicalAdmissionStage::kCanonicalRoute,
       dag->route_snapshot_uuid},
  };
  for (std::size_t index = 0; index < dag->nodes.size(); ++index) {
    auto& node = dag->nodes[index];
    node.selected_alternative_uuid = ContextUuid(seed + 100 + index * 3);
    node.executor_capability_uuid = ContextUuid(seed + 101 + index * 3);
    node.executor_capability_abi_version = 1;
    node.cost_vector_uuid = ContextUuid(seed + 102 + index * 3);
    node.memory_bytes_required = 1024;
    node.engine_capability_validated = true;
    node.mga_statement_context = dag->mga_statement_context;
  }
}

template <typename Request>
void ReplaceCarriedContext(Request* request,
                           exec::PhysicalMgaStatementContext context,
                           const bool replace_authority) {
  auto& dag = request->physical_dag;
  dag.mga_statement_context = std::move(context);
  dag.local_transaction_id =
      dag.mga_statement_context.owning_local_transaction_id;
  dag.statement_snapshot_id =
      dag.mga_statement_context.visible_committed_high_watermark;
  dag.admission_evidence[3].evidence_uuid =
      dag.mga_statement_context.statement_snapshot_uuid;
  for (auto& node : dag.nodes) {
    node.mga_statement_context = dag.mga_statement_context;
  }
  if (replace_authority) {
    request->mga_authority = ClosureAuthority(dag.mga_statement_context);
  }
}

template <typename Result>
bool AtomicStatementContextRefusal(const Result& result) {
  return !result.diagnostic.ok && result.output_batch.rows.empty() &&
         result.output_batch.columns.empty() &&
         result.selected_plan_uuid.empty() &&
         result.executed_physical_node_id == 0 &&
         result.causal_counter_id == 0 &&
         result.mga_statement_context.statement_uuid.empty();
}

template <typename Request, typename Execute>
bool ValidateStatementContextMatrix(Request base, Execute execute) {
  static_assert(std::is_same_v<
                decltype(base.physical_dag.local_transaction_id),
                std::uint64_t>);
  static_assert(std::is_same_v<
                decltype(base.physical_dag.statement_snapshot_id),
                std::uint64_t>);
  bool passed = true;
  const auto& dag = base.physical_dag;
  passed &= Require(
      dag.abi_version == 2 &&
          dag.local_transaction_id ==
              dag.mga_statement_context.owning_local_transaction_id &&
          dag.statement_snapshot_id ==
              dag.mga_statement_context.visible_committed_high_watermark &&
          dag.local_transaction_id >
              std::numeric_limits<std::uint32_t>::max() &&
          dag.catalog_epoch_uuid !=
              dag.mga_statement_context.statement_metadata_snapshot_uuid,
      "canonical names, uint64 values, or independent catalog identity were lost");

  const auto accepted = execute(base);
  passed &= Require(
      accepted.diagnostic.ok &&
          exec::PhysicalMgaStatementContextEqual(
              accepted.mga_statement_context, dag.mga_statement_context),
      "successful descriptor execution did not retain exact statement context");

  auto zero = base;
  auto zero_context = zero.physical_dag.mga_statement_context;
  zero_context.visible_committed_high_watermark = 0;
  zero_context.publication_inventory_next_local_transaction_id =
      zero_context.owning_local_transaction_id + 1;
  ReplaceCarriedContext(&zero, std::move(zero_context), true);
  const auto zero_result = execute(zero);
  passed &= Require(
      zero_result.diagnostic.ok &&
          zero_result.mga_statement_context.visible_committed_high_watermark ==
              0 &&
          exec::PhysicalMgaStatementContextEqual(
              zero_result.mga_statement_context,
              zero.physical_dag.mga_statement_context),
      "zero visibility high-water was refused or rewritten");

  const auto expect_refusal = [&](Request candidate,
                                  const std::string_view detail) {
    passed &= Require(AtomicStatementContextRefusal(execute(candidate)), detail);
  };

  auto missing_authority = base;
  missing_authority.mga_authority = {};
  expect_refusal(std::move(missing_authority),
                 "missing inventory authority reached descriptor output");

  auto missing_context = base;
  ReplaceCarriedContext(&missing_context, {}, true);
  expect_refusal(std::move(missing_context),
                 "missing carried context reached descriptor output");

  auto malformed = base;
  auto malformed_context = malformed.physical_dag.mga_statement_context;
  malformed_context.statement_uuid = "malformed";
  ReplaceCarriedContext(&malformed, std::move(malformed_context), true);
  expect_refusal(std::move(malformed),
                 "malformed statement UUID reached descriptor output");

  auto nil = base;
  auto nil_context = nil.physical_dag.mga_statement_context;
  nil_context.statement_metadata_snapshot_uuid =
      "00000000-0000-0000-0000-000000000000";
  ReplaceCarriedContext(&nil, std::move(nil_context), true);
  expect_refusal(std::move(nil),
                 "nil metadata snapshot UUID reached descriptor output");

  auto duplicated = base;
  auto duplicated_context = duplicated.physical_dag.mga_statement_context;
  duplicated_context.statement_snapshot_uuid =
      duplicated_context.statement_metadata_snapshot_uuid;
  ReplaceCarriedContext(&duplicated, std::move(duplicated_context), false);
  expect_refusal(std::move(duplicated),
                 "duplicated snapshot identity reached descriptor output");

  auto swapped = base;
  auto swapped_context = swapped.physical_dag.mga_statement_context;
  std::swap(swapped_context.statement_snapshot_uuid,
            swapped_context.statement_metadata_snapshot_uuid);
  ReplaceCarriedContext(&swapped, std::move(swapped_context), false);
  expect_refusal(std::move(swapped),
                 "swapped snapshot identities reached descriptor output");

  auto stale = base;
  auto stale_current = stale.physical_dag.mga_statement_context;
  stale_current.statement_snapshot_uuid = ContextUuid(799901);
  stale.mga_authority = ClosureAuthority(
      stale.physical_dag.mga_statement_context, stale_current);
  expect_refusal(std::move(stale),
                 "stale current inventory vector reached descriptor output");

  auto narrowed = base;
  narrowed.physical_dag.local_transaction_id =
      static_cast<std::uint32_t>(narrowed.physical_dag.local_transaction_id);
  expect_refusal(std::move(narrowed),
                 "narrowed local transaction id reached descriptor output");

  auto overflowing = base;
  overflowing.physical_dag.statement_snapshot_id =
      std::numeric_limits<std::uint64_t>::max();
  expect_refusal(std::move(overflowing),
                 "overflowing visibility boundary reached descriptor output");

  auto vector_mismatch = base;
  auto different_vector = vector_mismatch.physical_dag.mga_statement_context;
  different_vector.active_excluded_local_transaction_ids.push_back(
      different_vector.owning_local_transaction_id + 1);
  ReplaceCarriedContext(&vector_mismatch, std::move(different_vector), false);
  expect_refusal(std::move(vector_mismatch),
                 "mismatched active exclusion vector reached descriptor output");

  auto duplicate_evidence = base;
  duplicate_evidence.physical_dag.admission_evidence[1] =
      duplicate_evidence.physical_dag.admission_evidence[0];
  expect_refusal(std::move(duplicate_evidence),
                 "duplicate admission evidence reached descriptor output");

  auto out_of_order = base;
  std::swap(out_of_order.physical_dag.admission_evidence[1],
            out_of_order.physical_dag.admission_evidence[2]);
  expect_refusal(std::move(out_of_order),
                 "out-of-order admission evidence reached descriptor output");

  auto conflated_catalog = base;
  conflated_catalog.physical_dag.catalog_epoch_uuid =
      conflated_catalog.physical_dag.mga_statement_context
          .statement_metadata_snapshot_uuid;
  conflated_catalog.physical_dag.admission_evidence[1].evidence_uuid =
      conflated_catalog.physical_dag.catalog_epoch_uuid;
  expect_refusal(std::move(conflated_catalog),
                 "metadata snapshot substituted for catalog identity");
  return passed;
}


api::EngineDescriptor Descriptor() {
  api::EngineDescriptor descriptor;
  descriptor.descriptor_uuid.canonical =
      "019f0000-0000-7200-8000-000000007101";
  descriptor.descriptor_kind = "scalar";
  descriptor.canonical_type_name = "decimal";
  descriptor.encoded_descriptor =
      "type_uuid=019f0000-0000-7300-8000-000000007102;"
      "nullability=nullable;precision=12;scale=2";
  return descriptor;
}

exec::TypedPhysicalNodeDag Dag() {
  exec::TypedPhysicalNodeDag dag;
  dag.selected_plan_uuid = "019f0000-0000-7200-8000-000000007103";
  dag.root_physical_node_id = 712;
  dag.local_transaction_id = 713;
  dag.statement_snapshot_id = 714;
  dag.admission_evidence = {
      {exec::PhysicalAdmissionStage::kBoundRequest,
       "019f0000-0000-7200-8000-000000007111"},
      {exec::PhysicalAdmissionStage::kCatalogEpoch,
       "019f0000-0000-7200-8000-000000007112"},
      {exec::PhysicalAdmissionStage::kSecurity,
       "019f0000-0000-7200-8000-000000007113"},
      {exec::PhysicalAdmissionStage::kMgaStatementBoundary,
       "019f0000-0000-7200-8000-000000007114"},
      {exec::PhysicalAdmissionStage::kPolicyCapability,
       "019f0000-0000-7200-8000-000000007115"},
      {exec::PhysicalAdmissionStage::kResource,
       "019f0000-0000-7200-8000-000000007116"},
      {exec::PhysicalAdmissionStage::kStatisticsProvenance,
       "019f0000-0000-7200-8000-000000007117"},
      {exec::PhysicalAdmissionStage::kCanonicalRoute,
       "019f0000-0000-7200-8000-000000007118"},
  };
  dag.nodes = {
      {.physical_node_id = 711,
       .relational_node_id = 711,
       .node_kind = exec::PhysicalNodeKind::kValues,
       .implementation_id = "values.typed.v1",
       .output_descriptor_ids = {711},
       .causal_counter_id = 7101},
      {.physical_node_id = 712,
       .relational_node_id = 712,
       .node_kind = exec::PhysicalNodeKind::kFilter,
       .implementation_id = "filter.3vl.row.v1",
       .input_physical_node_ids = {711},
       .output_descriptor_ids = {711},
       .causal_counter_id = 7102},
  };
  UpgradeToCanonicalStatementContext(&dag, 7100);
  return dag;
}

exec::CanonicalDescriptorFilterRequest Request() {
  const auto descriptor = Descriptor();
  const auto value = [&](const std::string& encoded) {
    api::EngineTypedValue typed;
    typed.descriptor = descriptor;
    typed.encoded_value = encoded;
    return typed;
  };
  exec::CanonicalDescriptorFilterRequest request;
  request.physical_dag = Dag();
  request.selected_physical_node_id = 712;
  request.input_batch = exec::MakeDescriptorBatch(
      {{"amount", descriptor, true, 711}},
      {{{value("1.00")}}, {{value("2.00")}}, {{value("3.00")}}});
  request.row_truth_values = {
      api::EngineSqlTruthValue::true_value,
      api::EngineSqlTruthValue::false_value,
      api::EngineSqlTruthValue::unknown,
  };
  request.mga_authority =
      ClosureAuthority(request.physical_dag.mga_statement_context);
  return request;
}

// QOW-TEST-QRY-007-FILTER-V1
bool ValidatePhysicalFilter() {
  bool passed = true;
  const auto result = exec::ExecuteCanonicalDescriptorFilter(Request());
  passed &= Require(result.diagnostic.ok,
                    "typed physical filter node was not executable");
  passed &= Require(
      result.executed_physical_node_id == 712 &&
          result.causal_counter_id == 7102 &&
          exec::PhysicalMgaStatementContextEqual(
              result.mga_statement_context,
              Request().physical_dag.mga_statement_context),
      "filter node execution identity or statement context was lost");
  passed &= Require(result.output_batch.rows.size() == 1 &&
                        result.output_batch.rows[0].values[0].encoded_value ==
                            "1.00",
                    "filter did not admit TRUE exclusively");
  passed &= Require(result.output_batch.columns[0].descriptor_id == 711,
                    "filter changed the bound output schema");

  auto having = Request();
  having.consumer = api::EnginePredicateConsumer::having;
  const auto having_result =
      exec::ExecuteCanonicalDescriptorFilter(having);
  passed &= Require(
      having_result.diagnostic.ok &&
          exec::PhysicalMgaStatementContextEqual(
              having_result.mga_statement_context,
              having.physical_dag.mga_statement_context) &&
          having_result.output_batch.rows.size() == 1 &&
          having_result.output_batch.rows[0].values[0].encoded_value ==
              "1.00",
      "HAVING consumer did not admit TRUE exclusively at the filter node");

  auto invalid = Request();
  invalid.row_truth_values[1] = api::EngineSqlTruthValue::unspecified;
  auto refused = exec::ExecuteCanonicalDescriptorFilter(invalid);
  passed &= Require(
      refused.diagnostic.diagnostic_code ==
              "QOW-DIAG-QRY-017-3VL-REFUSAL-V1" &&
          AtomicStatementContextRefusal(refused),
      "unbound truth state reached row admission");

  invalid = Request();
  invalid.consumer = api::EnginePredicateConsumer::join_on;
  refused = exec::ExecuteCanonicalDescriptorFilter(invalid);
  passed &= Require(
      AtomicStatementContextRefusal(refused),
      "join-ON consumer was admitted as a filter node");

  invalid = Request();
  invalid.row_truth_values.pop_back();
  refused = exec::ExecuteCanonicalDescriptorFilter(invalid);
  passed &= Require(
      AtomicStatementContextRefusal(refused),
      "predicate cardinality mismatch produced partial rows");
  passed &= ValidateStatementContextMatrix(
      Request(), [](const auto& request) {
        return exec::ExecuteCanonicalDescriptorFilter(request);
      });
  return passed;
}

}  // namespace

int main() {
  if (!ValidatePhysicalFilter()) return 1;
  std::cout << "QOW-TEST-QRY-007-FILTER-V1: PASS\n";
  return 0;
}
