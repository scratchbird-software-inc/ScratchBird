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
#include "../support/native_int64_fixture.hpp"
#include "../support/binary_uuid_fixture.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace exec = scratchbird::engine::executor;
namespace api = scratchbird::engine::internal_api;
namespace dt = scratchbird::core::datatypes;
namespace uuid = scratchbird::core::uuid;

namespace {

api::EngineUuid CoreTypeUuid(const std::string_view stable_name) {
  return exec::MakeExecutorDescriptor(std::string(stable_name)).type_uuid;
}

bool Require(const bool condition, const std::string_view detail) {
  if (!condition) std::cerr << "QOW-TEST-QRY-007-WINDOW-V1: " << detail << '\n';
  return condition;
}

api::EngineUuid ContextUuid(const unsigned value) {
  return scratchbird::tests::FixtureUuid(66, value);
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
         result.selected_plan_uuid.is_nil() &&
         result.executed_physical_node_id == 0 &&
         result.causal_counter_id == 0 &&
         result.mga_statement_context.statement_uuid.is_nil();
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
  malformed_context.statement_uuid.bytes[6] = 0x40;
  ReplaceCarriedContext(&malformed, std::move(malformed_context), true);
  expect_refusal(std::move(malformed),
                 "malformed statement UUID reached descriptor output");

  auto nil = base;
  auto nil_context = nil.physical_dag.mga_statement_context;
  nil_context.statement_metadata_snapshot_uuid =
      scratchbird::tests::FixtureUuidLiteral("00000000-0000-0000-0000-000000000000");
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


api::EngineDescriptor Descriptor(const api::EngineUuid& descriptor_uuid,
                                 const api::EngineUuid& type_uuid,
                                 const std::string& nullability) {
  auto descriptor = scratchbird::tests::ExactScalarDescriptorFixture(
      dt::CanonicalTypeId::int64, "int64", descriptor_uuid, "nullability=" + nullability);
  if (descriptor.type_uuid != type_uuid) std::abort();
  return descriptor;
}

exec::CanonicalDescriptorRowNumberRequest Request() {
  const auto input_descriptor = Descriptor(
      scratchbird::tests::FixtureUuidLiteral("019f0000-0000-7200-8000-000000007501"),
      CoreTypeUuid("int64"), "nullable");
  const auto row_number_descriptor = Descriptor(
      scratchbird::tests::FixtureUuidLiteral("019f0000-0000-7200-8000-000000007503"),
      CoreTypeUuid("int64"), "non_null");
  const auto value = [&](const std::string& encoded) {
    return scratchbird::tests::NativeInt64Fixture(input_descriptor, encoded);
  };

  exec::CanonicalDescriptorRowNumberRequest request;
  request.physical_dag.selected_plan_uuid =
      scratchbird::tests::FixtureUuidLiteral("019f0000-0000-7200-8000-000000007505");
  request.physical_dag.root_physical_node_id = 753;
  request.physical_dag.local_transaction_id = 754;
  request.physical_dag.statement_snapshot_id = 755;
  request.physical_dag.admission_evidence = {
      {exec::PhysicalAdmissionStage::kBoundRequest,
       scratchbird::tests::FixtureUuidLiteral("019f0000-0000-7200-8000-000000007511")},
      {exec::PhysicalAdmissionStage::kCatalogEpoch,
       scratchbird::tests::FixtureUuidLiteral("019f0000-0000-7200-8000-000000007512")},
      {exec::PhysicalAdmissionStage::kSecurity,
       scratchbird::tests::FixtureUuidLiteral("019f0000-0000-7200-8000-000000007513")},
      {exec::PhysicalAdmissionStage::kMgaStatementBoundary,
       scratchbird::tests::FixtureUuidLiteral("019f0000-0000-7200-8000-000000007514")},
      {exec::PhysicalAdmissionStage::kPolicyCapability,
       scratchbird::tests::FixtureUuidLiteral("019f0000-0000-7200-8000-000000007515")},
      {exec::PhysicalAdmissionStage::kResource,
       scratchbird::tests::FixtureUuidLiteral("019f0000-0000-7200-8000-000000007516")},
      {exec::PhysicalAdmissionStage::kStatisticsProvenance,
       scratchbird::tests::FixtureUuidLiteral("019f0000-0000-7200-8000-000000007517")},
      {exec::PhysicalAdmissionStage::kCanonicalRoute,
       scratchbird::tests::FixtureUuidLiteral("019f0000-0000-7200-8000-000000007518")},
  };
  request.physical_dag.nodes = {
      {.physical_node_id = 751,
       .relational_node_id = 751,
       .node_kind = exec::PhysicalNodeKind::kValues,
       .implementation_id = "values.typed.v1",
       .output_descriptor_ids = {751},
       .causal_counter_id = 7501},
      {.physical_node_id = 752,
       .relational_node_id = 752,
       .node_kind = exec::PhysicalNodeKind::kSort,
       .implementation_id = "sort.typed.order-proof.v1",
       .input_physical_node_ids = {751},
       .output_descriptor_ids = {751},
       .causal_counter_id = 7502},
      {.physical_node_id = 753,
       .relational_node_id = 753,
       .node_kind = exec::PhysicalNodeKind::kWindow,
       .implementation_id = "window.row-number.v1",
       .input_physical_node_ids = {752},
       .output_descriptor_ids = {751, 752},
       .causal_counter_id = 7503},
  };
  UpgradeToCanonicalStatementContext(&request.physical_dag, 7500);
  const auto ordering_property_uuid = ContextUuid(7901);
  const auto window_property_uuid = ContextUuid(7902);
  request.physical_dag.nodes[1].delivered_property_uuids = {
      ordering_property_uuid};
  request.physical_dag.nodes[2].required_property_uuids = {
      ordering_property_uuid};
  request.physical_dag.nodes[2].delivered_property_uuids = {
      ordering_property_uuid, window_property_uuid};
  request.selected_physical_node_id = 753;
  request.ordered_input_batch = exec::MakeDescriptorBatch(
      {{"order_key", input_descriptor, true, 751}},
      {{{value("2")}}, {{value("5")}}});
  request.row_number_column =
      {"row_number", row_number_descriptor, false, 752};
  request.deterministic_order_evidence_uuid =
      scratchbird::tests::FixtureUuidLiteral("019f0000-0000-7200-8000-000000007519");
  request.mga_authority =
      ClosureAuthority(request.physical_dag.mga_statement_context);
  return request;
}

// QOW-TEST-QRY-007-WINDOW-V1
bool ValidatePhysicalRowNumber() {
  bool passed = true;
  auto result = exec::ExecuteCanonicalDescriptorRowNumber(Request());
  passed &= Require(
      result.diagnostic.ok &&
          result.executed_physical_node_id == 753 &&
          result.causal_counter_id == 7503 &&
          exec::PhysicalMgaStatementContextEqual(
              result.mga_statement_context,
              Request().physical_dag.mga_statement_context),
      "typed physical ROW_NUMBER node or statement context was not executable");
  passed &= Require(
      result.output_batch.rows.size() == 2 &&
          scratchbird::tests::NativeInt64Equals(result.output_batch.rows[0].values[0], 2) &&
          scratchbird::tests::NativeInt64Equals(result.output_batch.rows[0].values[1], 1) &&
          scratchbird::tests::NativeInt64Equals(result.output_batch.rows[1].values[0], 5) &&
          scratchbird::tests::NativeInt64Equals(result.output_batch.rows[1].values[1], 2),
      "ROW_NUMBER changed input order or assigned wrong ordinals");
  passed &= Require(result.output_batch.columns.size() == 2 &&
                        result.output_batch.columns[1].descriptor_id == 752 &&
                        !result.output_batch.columns[1].nullable,
                    "ROW_NUMBER lost its bound non-null descriptor");

  auto request = Request();
  // Two native LE8 input cells, one containing-batch byte, and two LE8
  // outputs: the executor retains input and output copies simultaneously.
  constexpr std::uint64_t exact_payload_grant = 2 * (1 + 2 * 8) + 2 * 8;
  request.physical_dag.nodes.back().memory_bytes_required = exact_payload_grant;
  const auto exact = exec::ExecuteCanonicalDescriptorRowNumber(request);
  passed &= Require(exact.diagnostic.ok && exact.output_batch.rows.size() == 2,
                    "exact native ROW_NUMBER payload grant was refused");
  --request.physical_dag.nodes.back().memory_bytes_required;
  const auto short_grant = exec::ExecuteCanonicalDescriptorRowNumber(request);
  passed &= Require(AtomicStatementContextRefusal(short_grant) &&
                        short_grant.diagnostic.diagnostic_code == "SBLR.PLAN_TREE.RESOURCE_LIMIT",
                    "one-byte-short ROW_NUMBER grant published a result");
  request = Request();
  request.ordered_input_batch.rows.clear();
  result = exec::ExecuteCanonicalDescriptorRowNumber(request);
  passed &= Require(
      result.diagnostic.ok && result.output_batch.rows.empty() &&
          result.output_batch.columns.size() == 2 &&
          exec::PhysicalMgaStatementContextEqual(
              result.mga_statement_context,
              request.physical_dag.mga_statement_context),
                    "empty ROW_NUMBER input lost output descriptors");

  request = Request();
  request.deterministic_order_evidence_uuid = {};
  result = exec::ExecuteCanonicalDescriptorRowNumber(request);
  passed &= Require(
      AtomicStatementContextRefusal(result),
      "ROW_NUMBER invented missing deterministic order");

  request = Request();
  request.physical_dag.nodes[1].node_kind =
      exec::PhysicalNodeKind::kProject;
  result = exec::ExecuteCanonicalDescriptorRowNumber(request);
  passed &= Require(
      AtomicStatementContextRefusal(result),
      "ROW_NUMBER accepted unordered project input");

  request = Request();
  request.row_number_column.nullable = true;
  result = exec::ExecuteCanonicalDescriptorRowNumber(request);
  passed &= Require(
      AtomicStatementContextRefusal(result),
      "nullable ROW_NUMBER result descriptor was accepted");
  passed &= ValidateStatementContextMatrix(
      Request(), [](const auto& candidate) {
        return exec::ExecuteCanonicalDescriptorRowNumber(candidate);
      });
  return passed;
}

}  // namespace

int main() {
  if (!ValidatePhysicalRowNumber()) return 1;
  std::cout << "QOW-TEST-QRY-007-WINDOW-V1: PASS\n";
  return 0;
}
