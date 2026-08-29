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
  if (!condition) std::cerr << "QOW-TEST-QRY-007-JOIN-V1: " << detail << '\n';
  return condition;
}

std::string ContextUuid(const unsigned value) {
  char buffer[37]{};
  std::snprintf(buffer, sizeof(buffer),
                "019f0000-0000-7640-8000-%012u", value);
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

constexpr std::string_view kCanonicalBooleanDescriptorTypeUuid =
    "01000000-626f-7f6c-a561-6e0000000000";

api::EngineDescriptor CanonicalBooleanAliasDescriptor(
    const bool nullable,
    const std::string_view datatype_descriptor_uuid =
        kCanonicalBooleanDescriptorTypeUuid,
    const std::uint64_t datatype_descriptor_generation = 1,
    const std::string_view type_uuid = kCanonicalBooleanDescriptorTypeUuid,
    const std::uint64_t type_generation = 1,
    const std::string_view codec_id = "datatype.boolean.u8.v1",
    const std::uint16_t codec_version = 1,
    const std::uint64_t codec_generation = 1,
    const std::uint8_t null_encoding = 1) {
  api::EngineDescriptor descriptor;
  descriptor.descriptor_uuid.canonical = std::string(datatype_descriptor_uuid);
  descriptor.descriptor_kind = "scalar";
  descriptor.canonical_type_name = "boolean";
  descriptor.encoded_descriptor =
      "datatype_descriptor_uuid=" + std::string(datatype_descriptor_uuid) +
      ";datatype_descriptor_generation=" +
      std::to_string(datatype_descriptor_generation) +
      ";type_uuid=" + std::string(type_uuid) + ";type_generation=" +
      std::to_string(type_generation) + ";codec_id=" +
      std::string(codec_id) + ";codec_version=" +
      std::to_string(codec_version) + ";codec_generation=" +
      std::to_string(codec_generation) + ";null_encoding=" +
      std::to_string(null_encoding) + ";nullability=" +
      (nullable ? "nullable" : "non_null");
  return descriptor;
}

exec::DescriptorBatch CanonicalBooleanAliasBatch(
    api::EngineDescriptor descriptor = CanonicalBooleanAliasDescriptor(true)) {
  return exec::MakeDescriptorBatch(
      {{"boolean_value", std::move(descriptor), true, 801}}, {});
}

bool ValidateCanonicalBooleanJoinAliasException() {
  bool passed = true;
  const auto right = exec::MakeDescriptorBatch(
      {{"right_id",
        Descriptor("019f0000-0000-7200-8000-000000008011",
                   "019f0000-0000-7300-8000-000000008012"),
        true, 802}},
      {});
  const auto exact = exec::ValidateCanonicalJoinDescriptorRoleDomains(
      CanonicalBooleanAliasBatch(), right);
  passed &= Require(exact.ok,
                    "exact canonical boolean descriptor/type alias refused");
  const auto exact_non_null = exec::ValidateCanonicalJoinDescriptorRoleDomains(
      CanonicalBooleanAliasBatch(CanonicalBooleanAliasDescriptor(false)),
      right);
  passed &= Require(
      exact_non_null.ok,
      "exact non-null canonical boolean descriptor/type alias refused");

  const auto expect_collision_refusal = [&](exec::DescriptorBatch left,
                                            const std::string_view detail) {
    const auto diagnostic =
        exec::ValidateCanonicalJoinDescriptorRoleDomains(left, right);
    passed &= Require(
        !diagnostic.ok &&
            diagnostic.diagnostic_code == "SBLR.PLAN_TREE.INVALID_HANDLE" &&
            diagnostic.detail ==
                "join descriptor and type identity domains are not independent",
        detail);
  };

  expect_collision_refusal(
      CanonicalBooleanAliasBatch(CanonicalBooleanAliasDescriptor(
          true, kCanonicalBooleanDescriptorTypeUuid, 2)),
      "stale canonical boolean descriptor generation was admitted");
  expect_collision_refusal(
      CanonicalBooleanAliasBatch(CanonicalBooleanAliasDescriptor(
          true, kCanonicalBooleanDescriptorTypeUuid, 1,
          kCanonicalBooleanDescriptorTypeUuid, 2)),
      "stale canonical boolean type generation was admitted");
  expect_collision_refusal(
      CanonicalBooleanAliasBatch(CanonicalBooleanAliasDescriptor(
          true, kCanonicalBooleanDescriptorTypeUuid, 1,
          kCanonicalBooleanDescriptorTypeUuid, 1,
          "datatype.boolean.lookalike.v1")),
      "canonical boolean codec lookalike was admitted");
  expect_collision_refusal(
      CanonicalBooleanAliasBatch(CanonicalBooleanAliasDescriptor(
          true, kCanonicalBooleanDescriptorTypeUuid, 1,
          kCanonicalBooleanDescriptorTypeUuid, 1,
          "datatype.boolean.u8.v1", 1, 2)),
      "stale canonical boolean codec generation was admitted");
  expect_collision_refusal(
      CanonicalBooleanAliasBatch(CanonicalBooleanAliasDescriptor(
          true, kCanonicalBooleanDescriptorTypeUuid, 1,
          kCanonicalBooleanDescriptorTypeUuid, 1,
          "datatype.boolean.u8.v1", 2)),
      "stale canonical boolean codec version was admitted");
  expect_collision_refusal(
      CanonicalBooleanAliasBatch(CanonicalBooleanAliasDescriptor(
          true, kCanonicalBooleanDescriptorTypeUuid, 1,
          kCanonicalBooleanDescriptorTypeUuid, 1,
          "datatype.boolean.u8.v1", 1, 1, 2)),
      "wrong canonical boolean null encoding was admitted");

  auto extra = CanonicalBooleanAliasDescriptor(true);
  extra.encoded_descriptor += ";unbound_extra=1";
  expect_collision_refusal(CanonicalBooleanAliasBatch(std::move(extra)),
                           "extra boolean alias carrier field was admitted");
  auto duplicate = CanonicalBooleanAliasDescriptor(true);
  duplicate.encoded_descriptor += ";codec_generation=1";
  expect_collision_refusal(
      CanonicalBooleanAliasBatch(std::move(duplicate)),
      "duplicate boolean alias carrier field was admitted");
  auto missing = CanonicalBooleanAliasDescriptor(true);
  const auto missing_begin =
      missing.encoded_descriptor.find(";codec_version=");
  const auto missing_end =
      missing.encoded_descriptor.find(';', missing_begin + 1);
  missing.encoded_descriptor.erase(missing_begin,
                                   missing_end - missing_begin);
  expect_collision_refusal(CanonicalBooleanAliasBatch(std::move(missing)),
                           "incomplete boolean alias carrier was admitted");
  auto reordered = CanonicalBooleanAliasDescriptor(true);
  const auto codec_field = reordered.encoded_descriptor.find(";codec_id=");
  reordered.encoded_descriptor =
      reordered.encoded_descriptor.substr(codec_field + 1) + ";" +
      reordered.encoded_descriptor.substr(0, codec_field);
  expect_collision_refusal(
      CanonicalBooleanAliasBatch(std::move(reordered)),
      "noncanonical boolean alias carrier serialization was admitted");
  auto nullability_mismatch = CanonicalBooleanAliasBatch();
  nullability_mismatch.columns.front().nullable = false;
  expect_collision_refusal(
      std::move(nullability_mismatch),
      "boolean alias containing-slot nullability mismatch was admitted");

  auto non_boolean = CanonicalBooleanAliasDescriptor(true);
  non_boolean.canonical_type_name = "int64";
  expect_collision_refusal(
      CanonicalBooleanAliasBatch(std::move(non_boolean)),
      "non-boolean descriptor/type alias was admitted");

  constexpr std::string_view kLookalikeUuid =
      "01000000-626f-7f6c-a561-6e0000000001";
  expect_collision_refusal(
      CanonicalBooleanAliasBatch(CanonicalBooleanAliasDescriptor(
          true, kLookalikeUuid, 1, kLookalikeUuid)),
      "boolean UUID lookalike was admitted");
  return passed;
}

exec::CanonicalDescriptorInnerJoinRequest Request() {
  const auto left_descriptor = Descriptor(
      "019f0000-0000-7200-8000-000000007401",
      "019f0000-0000-7300-8000-000000007402");
  const auto right_descriptor = Descriptor(
      "019f0000-0000-7200-8000-000000007403",
      "019f0000-0000-7300-8000-000000007404");
  const auto value = [](const api::EngineDescriptor& descriptor,
                        const std::string& encoded) {
    api::EngineTypedValue typed;
    typed.descriptor = descriptor;
    typed.encoded_value = encoded;
    return typed;
  };

  exec::CanonicalDescriptorInnerJoinRequest request;
  request.physical_dag.selected_plan_uuid =
      "019f0000-0000-7200-8000-000000007405";
  request.physical_dag.root_physical_node_id = 743;
  request.physical_dag.local_transaction_id = 744;
  request.physical_dag.statement_snapshot_id = 745;
  request.physical_dag.admission_evidence = {
      {exec::PhysicalAdmissionStage::kBoundRequest,
       "019f0000-0000-7200-8000-000000007411"},
      {exec::PhysicalAdmissionStage::kCatalogEpoch,
       "019f0000-0000-7200-8000-000000007412"},
      {exec::PhysicalAdmissionStage::kSecurity,
       "019f0000-0000-7200-8000-000000007413"},
      {exec::PhysicalAdmissionStage::kMgaStatementBoundary,
       "019f0000-0000-7200-8000-000000007414"},
      {exec::PhysicalAdmissionStage::kPolicyCapability,
       "019f0000-0000-7200-8000-000000007415"},
      {exec::PhysicalAdmissionStage::kResource,
       "019f0000-0000-7200-8000-000000007416"},
      {exec::PhysicalAdmissionStage::kStatisticsProvenance,
       "019f0000-0000-7200-8000-000000007417"},
      {exec::PhysicalAdmissionStage::kCanonicalRoute,
       "019f0000-0000-7200-8000-000000007418"},
  };
  request.physical_dag.nodes = {
      {.physical_node_id = 741,
       .relational_node_id = 741,
       .node_kind = exec::PhysicalNodeKind::kValues,
       .implementation_id = "values.left.typed.v1",
       .output_descriptor_ids = {741},
       .causal_counter_id = 7401},
      {.physical_node_id = 742,
       .relational_node_id = 742,
       .node_kind = exec::PhysicalNodeKind::kValues,
       .implementation_id = "values.right.typed.v1",
       .output_descriptor_ids = {742},
       .causal_counter_id = 7402},
      {.physical_node_id = 743,
       .relational_node_id = 743,
       .node_kind = exec::PhysicalNodeKind::kJoin,
       .implementation_id = "join.inner.3vl.nested.v1",
       .input_physical_node_ids = {741, 742},
       .output_descriptor_ids = {741, 742},
       .causal_counter_id = 7403},
  };
  UpgradeToCanonicalStatementContext(&request.physical_dag, 7400);
  request.selected_physical_node_id = 743;
  request.left_batch = exec::MakeDescriptorBatch(
      {{"left_id", left_descriptor, true, 741}},
      {{{value(left_descriptor, "1")}}, {{value(left_descriptor, "2")}}});
  request.right_batch = exec::MakeDescriptorBatch(
      {{"right_id", right_descriptor, true, 742}},
      {{{value(right_descriptor, "10")}},
       {{value(right_descriptor, "20")}}});
  request.pair_truth_values = {
      api::EngineSqlTruthValue::true_value,
      api::EngineSqlTruthValue::false_value,
      api::EngineSqlTruthValue::unknown,
      api::EngineSqlTruthValue::true_value,
  };
  request.mga_authority =
      ClosureAuthority(request.physical_dag.mga_statement_context);
  return request;
}

// QOW-TEST-QRY-007-JOIN-V1
bool ValidatePhysicalInnerJoin() {
  bool passed = true;
  auto result = exec::ExecuteCanonicalDescriptorInnerJoin(Request());
  passed &= Require(
      result.diagnostic.ok &&
          result.executed_physical_node_id == 743 &&
          result.causal_counter_id == 7403 &&
          exec::PhysicalMgaStatementContextEqual(
              result.mga_statement_context,
              Request().physical_dag.mga_statement_context),
      "typed physical join node or statement context was not executable");
  passed &= Require(
      result.output_batch.rows.size() == 2 &&
          result.output_batch.rows[0].values[0].encoded_value == "1" &&
          result.output_batch.rows[0].values[1].encoded_value == "10" &&
          result.output_batch.rows[1].values[0].encoded_value == "2" &&
          result.output_batch.rows[1].values[1].encoded_value == "20",
      "inner join did not admit TRUE row pairs exclusively");
  passed &= Require(result.output_batch.columns.size() == 2 &&
                        result.output_batch.columns[0].descriptor_id == 741 &&
                        result.output_batch.columns[1].descriptor_id == 742,
                    "join did not concatenate bound output descriptors");

  auto request = Request();
  request.left_batch.rows.clear();
  request.pair_truth_values.clear();
  result = exec::ExecuteCanonicalDescriptorInnerJoin(request);
  passed &= Require(
      result.diagnostic.ok && result.output_batch.rows.empty() &&
          result.output_batch.columns.size() == 2 &&
          exec::PhysicalMgaStatementContextEqual(
              result.mga_statement_context,
              request.physical_dag.mga_statement_context),
                    "empty typed join input lost output schema");

  request = Request();
  request.pair_truth_values[0] = api::EngineSqlTruthValue::unspecified;
  result = exec::ExecuteCanonicalDescriptorInnerJoin(request);
  passed &= Require(
      AtomicStatementContextRefusal(result),
      "unbound ON truth produced partial joined rows");

  request = Request();
  request.consumer = api::EnginePredicateConsumer::filter;
  result = exec::ExecuteCanonicalDescriptorInnerJoin(request);
  passed &= Require(
      AtomicStatementContextRefusal(result),
      "filter consumer was admitted as join-ON");

  request = Request();
  request.physical_dag.nodes.back().output_descriptor_ids = {742, 741};
  result = exec::ExecuteCanonicalDescriptorInnerJoin(request);
  passed &= Require(
      AtomicStatementContextRefusal(result),
      "reordered unresolved join schema produced data");
  passed &= ValidateStatementContextMatrix(
      Request(), [](const auto& candidate) {
        return exec::ExecuteCanonicalDescriptorInnerJoin(candidate);
      });
  return passed;
}

}  // namespace

int main() {
  if (!ValidatePhysicalInnerJoin() ||
      !ValidateCanonicalBooleanJoinAliasException()) {
    return 1;
  }
  std::cout << "QOW-TEST-QRY-007-JOIN-V1: PASS\n";
  return 0;
}
