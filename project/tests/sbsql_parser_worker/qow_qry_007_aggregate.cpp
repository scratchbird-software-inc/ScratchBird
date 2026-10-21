// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "descriptor_value_runtime.hpp"
#include "sbl_numeric.hpp"
#include "catalog/column_metadata_codec.hpp"
#include "../support/binary_uuid_fixture.hpp"
#include "../support/native_int64_fixture.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <limits>
#include <string>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>

namespace exec = scratchbird::engine::executor;
namespace api = scratchbird::engine::internal_api;

namespace {

bool Require(const bool condition, const std::string_view detail) {
  if (!condition) {
    std::cerr << "QOW-TEST-QRY-007-AGGREGATE-V1: " << detail << '\n';
  }
  return condition;
}

api::EngineUuid ContextUuid(const unsigned value) {
  return scratchbird::tests::FixtureUuid(7620, value);
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
  for (auto& node : dag.nodes) node.mga_statement_context = dag.mga_statement_context;
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
  static_assert(std::is_same_v<decltype(base.physical_dag.local_transaction_id),
                               std::uint64_t>);
  static_assert(std::is_same_v<decltype(base.physical_dag.statement_snapshot_id),
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
      "canonical names, uint64 values, or catalog independence were lost");
  const auto accepted = execute(base);
  passed &= Require(
      accepted.diagnostic.ok &&
          exec::PhysicalMgaStatementContextEqual(
              accepted.mga_statement_context, dag.mga_statement_context),
      "successful aggregate did not retain exact statement context");

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
  auto mutate_carried = [&](auto mutate, const bool replace_authority,
                            const std::string_view detail) {
    auto candidate = base;
    auto context = candidate.physical_dag.mga_statement_context;
    mutate(context);
    ReplaceCarriedContext(&candidate, std::move(context), replace_authority);
    expect_refusal(std::move(candidate), detail);
  };
  auto missing_authority = base;
  missing_authority.mga_authority = {};
  expect_refusal(std::move(missing_authority),
                 "missing inventory authority produced aggregate rows");
  mutate_carried(
      [](auto& context) { context = exec::PhysicalMgaStatementContext{}; },
      true, "missing carried context produced aggregate rows");
  mutate_carried([](auto& context) { context.statement_uuid = scratchbird::tests::FixtureUuidLiteral("019f0000-0000-4620-0000-000000000001"); },
                 true, "malformed statement UUID produced aggregate rows");
  mutate_carried(
      [](auto& context) {
        context.statement_metadata_snapshot_uuid =
            scratchbird::tests::FixtureUuidLiteral("00000000-0000-0000-0000-000000000000");
      },
      true, "nil metadata snapshot UUID produced aggregate rows");
  mutate_carried(
      [](auto& context) {
        context.statement_snapshot_uuid =
            context.statement_metadata_snapshot_uuid;
      },
      false, "duplicated snapshot identity produced aggregate rows");
  mutate_carried(
      [](auto& context) {
        std::swap(context.statement_snapshot_uuid,
                  context.statement_metadata_snapshot_uuid);
      },
      false, "swapped snapshot identities produced aggregate rows");
  auto stale = base;
  auto stale_current = stale.physical_dag.mga_statement_context;
  stale_current.statement_snapshot_uuid = ContextUuid(799921);
  stale.mga_authority = ClosureAuthority(
      stale.physical_dag.mga_statement_context, stale_current);
  expect_refusal(std::move(stale),
                 "stale current inventory vector produced aggregate rows");
  auto narrowed = base;
  narrowed.physical_dag.local_transaction_id =
      static_cast<std::uint32_t>(narrowed.physical_dag.local_transaction_id);
  expect_refusal(std::move(narrowed),
                 "narrowed local transaction id produced aggregate rows");
  auto overflowing = base;
  overflowing.physical_dag.statement_snapshot_id =
      std::numeric_limits<std::uint64_t>::max();
  expect_refusal(std::move(overflowing),
                 "overflowing visibility boundary produced aggregate rows");
  mutate_carried(
      [](auto& context) {
        context.active_excluded_local_transaction_ids.push_back(
            context.owning_local_transaction_id + 1);
      },
      false, "mismatched exclusion vector produced aggregate rows");
  auto duplicate_evidence = base;
  duplicate_evidence.physical_dag.admission_evidence[1] =
      duplicate_evidence.physical_dag.admission_evidence[0];
  expect_refusal(std::move(duplicate_evidence),
                 "duplicate admission evidence produced aggregate rows");
  auto out_of_order = base;
  std::swap(out_of_order.physical_dag.admission_evidence[1],
            out_of_order.physical_dag.admission_evidence[2]);
  expect_refusal(std::move(out_of_order),
                 "out-of-order admission evidence produced aggregate rows");
  auto conflated = base;
  conflated.physical_dag.catalog_epoch_uuid =
      conflated.physical_dag.mga_statement_context
          .statement_metadata_snapshot_uuid;
  conflated.physical_dag.admission_evidence[1].evidence_uuid =
      conflated.physical_dag.catalog_epoch_uuid;
  expect_refusal(std::move(conflated),
                 "metadata snapshot substituted for catalog identity");
  return passed;
}

api::EngineDescriptor Descriptor(const api::EngineUuid& descriptor_uuid,
                                 const std::string& type_name,
                                 const std::string& nullability) {
  return scratchbird::tests::ExactScalarDescriptorFixture(
      scratchbird::core::datatypes::CanonicalTypeIdFromStableName(type_name), type_name,
      descriptor_uuid, "nullability=" + nullability);
}

// A DECIMAL column binding is only admitted with its full codec profile:
// precision, scale, the exact decimal codec identity and its generation. The
// payload is the native binary encoding, never text -- the executor refuses a
// decimal value that carries `encoded_value`. This fixture previously supplied
// `nullability` alone and the text "3.00", so every assertion below failed on
// `DATATYPE.DESCRIPTOR.INVALID: DECIMAL column profile or nullability is
// invalid` before COUNT(*) was ever exercised.
constexpr unsigned kDecimalPrecision = 10;
constexpr unsigned kDecimalScale = 2;

api::EngineDescriptor BoundDecimalDescriptor(const api::EngineUuid& occurrence_uuid,
                                            const std::string& nullability) {
  auto descriptor = Descriptor(occurrence_uuid, "decimal", nullability);
  api::CatalogColumnMetadata metadata;
  metadata.text = {{"nullability", nullability},
                   {"precision", std::to_string(kDecimalPrecision)},
                   {"scale", std::to_string(kDecimalScale)},
                   {"decimal_codec_generation", "1"}};
  // The le24 codec identity, which is what the bound profile resolves to for
  // precision <= 38.
  metadata.identities["decimal_codec_uuid"] = api::EngineUuid{
      {0x01,0xa1,0x18,0x6b,0xdf,0x04,0x76,0x0a,
       0xa3,0xd5,0xd0,0x2d,0x2e,0xe3,0x01,0x54}};
  if (!api::EncodeCatalogColumnMetadata(metadata, &descriptor.encoded_descriptor)) {
    throw std::runtime_error("decimal column binding metadata is invalid");
  }
  return descriptor;
}

exec::CanonicalDescriptorCountRequest Request() {
  namespace numeric = scratchbird::libraries::sbl_numeric;
  const auto input_descriptor = BoundDecimalDescriptor(
      scratchbird::tests::FixtureUuidLiteral("019f0000-0000-7200-8000-000000007301"),
      "nullable");
  const auto count_descriptor = Descriptor(
      scratchbird::tests::FixtureUuidLiteral("019f0000-0000-7200-8000-000000007303"), "int64",
      "non_null");
  const auto encoded = numeric::EncodeBoundExactDecimal(
      "3.00", {numeric::ExactDecimalCodec::le24_v1, kDecimalPrecision, kDecimalScale});
  if (!encoded.ok()) throw std::runtime_error("decimal fixture payload is invalid");
  api::EngineTypedValue value;
  value.descriptor = input_descriptor;
  value.binary_value = encoded.bytes;
  value.state = api::EngineValueState::value;
  api::EngineTypedValue null_value;
  null_value.descriptor = input_descriptor;
  null_value.is_null = true;
  null_value.state = api::EngineValueState::sql_null;

  exec::CanonicalDescriptorCountRequest request;
  request.physical_dag.selected_plan_uuid =
      scratchbird::tests::FixtureUuidLiteral("019f0000-0000-7200-8000-000000007305");
  request.physical_dag.root_physical_node_id = 732;
  request.physical_dag.local_transaction_id = 733;
  request.physical_dag.statement_snapshot_id = 734;
  request.physical_dag.admission_evidence = {
      {exec::PhysicalAdmissionStage::kBoundRequest,
       scratchbird::tests::FixtureUuidLiteral("019f0000-0000-7200-8000-000000007311")},
      {exec::PhysicalAdmissionStage::kCatalogEpoch,
       scratchbird::tests::FixtureUuidLiteral("019f0000-0000-7200-8000-000000007312")},
      {exec::PhysicalAdmissionStage::kSecurity,
       scratchbird::tests::FixtureUuidLiteral("019f0000-0000-7200-8000-000000007313")},
      {exec::PhysicalAdmissionStage::kMgaStatementBoundary,
       scratchbird::tests::FixtureUuidLiteral("019f0000-0000-7200-8000-000000007314")},
      {exec::PhysicalAdmissionStage::kPolicyCapability,
       scratchbird::tests::FixtureUuidLiteral("019f0000-0000-7200-8000-000000007315")},
      {exec::PhysicalAdmissionStage::kResource,
       scratchbird::tests::FixtureUuidLiteral("019f0000-0000-7200-8000-000000007316")},
      {exec::PhysicalAdmissionStage::kStatisticsProvenance,
       scratchbird::tests::FixtureUuidLiteral("019f0000-0000-7200-8000-000000007317")},
      {exec::PhysicalAdmissionStage::kCanonicalRoute,
       scratchbird::tests::FixtureUuidLiteral("019f0000-0000-7200-8000-000000007318")},
  };
  request.physical_dag.nodes = {
      {.physical_node_id = 731,
       .relational_node_id = 731,
       .node_kind = exec::PhysicalNodeKind::kValues,
       .implementation_id = "values.typed.v1",
       .output_descriptor_ids = {731},
       .causal_counter_id = 7301},
      {.physical_node_id = 732,
       .relational_node_id = 732,
       .node_kind = exec::PhysicalNodeKind::kAggregate,
       .implementation_id = "aggregate.count-star.v1",
       .input_physical_node_ids = {731},
       .output_descriptor_ids = {732},
       .causal_counter_id = 7302},
  };
  UpgradeToCanonicalStatementContext(&request.physical_dag, 7300);
  request.selected_physical_node_id = 732;
  request.input_batch = exec::MakeDescriptorBatch(
      {{"amount", input_descriptor, true, 731}},
      {{{value}}, {{null_value}}, {{value}}});
  request.count_column = {"row_count", count_descriptor, false, 732};
  request.mga_authority =
      ClosureAuthority(request.physical_dag.mga_statement_context);
  return request;
}

// QOW-TEST-QRY-007-AGGREGATE-V1
bool ValidatePhysicalCountStar() {
  bool passed = true;
  auto result = exec::ExecuteCanonicalDescriptorCountStar(Request());
  // Report the refusal itself. Without this the fixture said only "not
  // executable", which names the symptom and hides the engine's own cause.
  if (!result.diagnostic.ok) {
    std::cerr << "QOW-TEST-QRY-007-AGGREGATE-V1: refusal code="
              << result.diagnostic.diagnostic_code
              << " detail=" << result.diagnostic.detail
              << " row=" << result.diagnostic.row_index
              << " column=" << result.diagnostic.column_index << '\n';
  }
  if (!result.diagnostic.ok || result.output_batch.rows.size() != 1 ||
      result.output_batch.columns.size() != 1) {
    std::cerr << "QOW-TEST-QRY-007-AGGREGATE-V1: node=" << result.executed_physical_node_id
              << " counter=" << result.causal_counter_id
              << " rows=" << result.output_batch.rows.size()
              << " columns=" << result.output_batch.columns.size() << '\n';
  }
  passed &= Require(result.diagnostic.ok &&
                        result.executed_physical_node_id == 732 &&
                        result.causal_counter_id == 7302 &&
                        exec::PhysicalMgaStatementContextEqual(
                            result.mga_statement_context,
                            Request().physical_dag.mga_statement_context),
                    "typed physical aggregate node was not executable");
  passed &= Require(result.output_batch.rows.size() == 1 &&
                        !result.output_batch.rows[0].values.empty() &&
                        scratchbird::tests::NativeInt64Equals(result.output_batch.rows[0].values[0], 3) &&
                        result.output_batch.rows[0].values[0].state ==
                            api::EngineValueState::value,
                    "COUNT(*) did not count physical rows including NULL");
  // Guarded like the row check above. A refused execution returns an empty
  // batch, so indexing columns[0] unconditionally turned every upstream
  // failure in this fixture into a SIGSEGV -- which reported as a crash
  // instead of reporting which assertion failed.
  passed &= Require(result.output_batch.columns.size() == 1 &&
                        !result.output_batch.columns[0].nullable &&
                        result.output_batch.columns[0].descriptor_id == 732,
                    "COUNT(*) lost its bound non-null result descriptor");

  auto request = Request();
  request.input_batch.rows.clear();
  result = exec::ExecuteCanonicalDescriptorCountStar(request);
  passed &= Require(result.diagnostic.ok &&
                        result.output_batch.rows.size() == 1 &&
                        !result.output_batch.rows[0].values.empty() &&
                        scratchbird::tests::NativeInt64Equals(result.output_batch.rows[0].values[0], 0) &&
                        exec::PhysicalMgaStatementContextEqual(
                            result.mga_statement_context,
                            request.physical_dag.mga_statement_context),
                    "COUNT(*) on empty input did not return one zero row");

  request = Request();
  request.count_column.nullable = true;
  result = exec::ExecuteCanonicalDescriptorCountStar(request);
  passed &= Require(AtomicStatementContextRefusal(result),
                    "nullable COUNT(*) descriptor was silently accepted");

  request = Request();
  request.count_column.descriptor_id = 999;
  result = exec::ExecuteCanonicalDescriptorCountStar(request);
  passed &= Require(AtomicStatementContextRefusal(result),
                    "unresolved aggregate result handle produced data");
  passed &= ValidateStatementContextMatrix(
      Request(), [](const auto& request) {
        return exec::ExecuteCanonicalDescriptorCountStar(request);
      });
  return passed;
}

}  // namespace

int main() {
  if (!ValidatePhysicalCountStar()) return 1;
  std::cout << "QOW-TEST-QRY-007-AGGREGATE-V1: PASS\n";
  return 0;
}
