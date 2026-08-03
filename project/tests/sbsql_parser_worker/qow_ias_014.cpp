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
  if (!condition) std::cerr << "QOW-TEST-IAS-014-V1: " << detail << '\n';
  return condition;
}

std::string ContextUuid(const unsigned value) {
  char buffer[37]{};
  std::snprintf(buffer, sizeof(buffer),
                "019f0000-0000-7610-8000-%012u", value);
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
  for (auto& node : dag.nodes) node.mga_statement_context = dag.mga_statement_context;
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
      "successful execution did not retain exact statement context");

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
                 "missing inventory authority reached descriptor output");
  mutate_carried(
      [](auto& context) { context = exec::PhysicalMgaStatementContext{}; },
      true, "missing carried context reached descriptor output");
  mutate_carried([](auto& context) { context.statement_uuid = "malformed"; },
                 true,
                 "malformed statement UUID reached descriptor output");
  mutate_carried(
      [](auto& context) {
        context.statement_metadata_snapshot_uuid =
            "00000000-0000-0000-0000-000000000000";
      },
      true, "nil metadata snapshot UUID reached descriptor output");
  mutate_carried(
      [](auto& context) {
        context.statement_snapshot_uuid =
            context.statement_metadata_snapshot_uuid;
      },
      false, "duplicated snapshot identity reached descriptor output");
  mutate_carried(
      [](auto& context) {
        std::swap(context.statement_snapshot_uuid,
                  context.statement_metadata_snapshot_uuid);
      },
      false, "swapped snapshot identities reached descriptor output");

  auto stale = base;
  auto stale_current = stale.physical_dag.mga_statement_context;
  stale_current.statement_snapshot_uuid = ContextUuid(799911);
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
  mutate_carried(
      [](auto& context) {
        context.active_excluded_local_transaction_ids.push_back(
            context.owning_local_transaction_id + 1);
      },
      false, "mismatched exclusion vector reached descriptor output");
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

api::EngineDescriptor Descriptor(const std::string& uuid) {
  api::EngineDescriptor descriptor;
  descriptor.descriptor_uuid.canonical = uuid;
  descriptor.descriptor_kind = "scalar";
  descriptor.canonical_type_name = "decimal";
  descriptor.encoded_descriptor =
      "type_uuid=019f0000-0000-7300-8000-000000014001;"
      "nullability=nullable;precision=12;scale=2";
  return descriptor;
}

exec::TypedPhysicalNodeDag Dag(const std::string& strategy) {
  exec::TypedPhysicalNodeDag dag;
  dag.selected_plan_uuid = "019f0000-0000-7200-8000-000000014002";
  dag.root_physical_node_id = 1402;
  dag.local_transaction_id = 1403;
  dag.statement_snapshot_id = 1404;
  dag.admission_evidence = {
      {exec::PhysicalAdmissionStage::kBoundRequest,
       "019f0000-0000-7200-8000-000000014011"},
      {exec::PhysicalAdmissionStage::kCatalogEpoch,
       "019f0000-0000-7200-8000-000000014012"},
      {exec::PhysicalAdmissionStage::kSecurity,
       "019f0000-0000-7200-8000-000000014013"},
      {exec::PhysicalAdmissionStage::kMgaStatementBoundary,
       "019f0000-0000-7200-8000-000000014014"},
      {exec::PhysicalAdmissionStage::kPolicyCapability,
       "019f0000-0000-7200-8000-000000014015"},
      {exec::PhysicalAdmissionStage::kResource,
       "019f0000-0000-7200-8000-000000014016"},
      {exec::PhysicalAdmissionStage::kStatisticsProvenance,
       "019f0000-0000-7200-8000-000000014017"},
      {exec::PhysicalAdmissionStage::kCanonicalRoute,
       "019f0000-0000-7200-8000-000000014018"},
  };
  dag.nodes = {
      {.physical_node_id = 1401,
       .relational_node_id = 141,
       .node_kind = exec::PhysicalNodeKind::kValues,
       .implementation_id = "values.typed.v1",
       .output_descriptor_ids = {141},
       .causal_counter_id = 14001},
      {.physical_node_id = 1402,
       .relational_node_id = 142,
       .node_kind = exec::PhysicalNodeKind::kProject,
       .implementation_id = strategy,
       .input_physical_node_ids = {1401},
       .output_descriptor_ids = {141},
       .causal_counter_id = 14002},
  };
  UpgradeToCanonicalStatementContext(&dag, 14000);
  return dag;
}

exec::CanonicalDescriptorProjectionRequest Request(
    const std::string& strategy) {
  const auto descriptor =
      Descriptor("019f0000-0000-7200-8000-000000014021");
  api::EngineTypedValue value;
  value.descriptor = descriptor;
  value.encoded_value = "14.00";
  value.state = api::EngineValueState::value;

  exec::CanonicalDescriptorProjectionRequest request;
  request.physical_dag = Dag(strategy);
  request.selected_physical_node_id = 1402;
  request.input_batch = exec::MakeDescriptorBatch(
      {{"amount", descriptor, true, 141}}, {{{value}}});
  request.projected_columns = {0};
  request.mga_authority =
      ClosureAuthority(request.physical_dag.mga_statement_context);
  return request;
}

// QOW-TEST-IAS-014-V1
bool ValidateCanonicalAdoption() {
  bool passed = true;
  const auto row = exec::ExecuteCanonicalDescriptorProjection(
      Request("project.typed.row.v1"));
  const auto vector = exec::ExecuteCanonicalDescriptorProjection(
      Request("project.typed.vector.v1"));
  passed &= Require(row.diagnostic.ok && vector.diagnostic.ok,
                    "canonical physical strategies did not reach helper");
  passed &= Require(row.executed_physical_node_id == 1402 &&
                        row.causal_counter_id == 14002 &&
                        exec::PhysicalMgaStatementContextEqual(
                            row.mga_statement_context,
                            Request("project.typed.row.v1")
                                .physical_dag.mga_statement_context) &&
                        row.output_batch.rows.size() == 1 &&
                        row.output_batch.rows[0].values[0].encoded_value ==
                            "14.00",
                    "physical execution evidence or typed data was lost");
  passed &= Require(
      exec::PhysicalMgaStatementContextEqual(
          vector.mga_statement_context,
          Request("project.typed.vector.v1")
              .physical_dag.mga_statement_context) &&
          vector.output_batch.rows[0].values[0].encoded_value ==
              row.output_batch.rows[0].values[0].encoded_value &&
          vector.output_batch.rows[0]
                  .values[0]
                  .descriptor.descriptor_uuid.canonical ==
              row.output_batch.rows[0]
                  .values[0]
                  .descriptor.descriptor_uuid.canonical,
      "forced strategies diverged in value or descriptor identity");

  const auto direct = exec::ExecuteCanonicalDescriptorProjection({});
  passed &= Require(AtomicStatementContextRefusal(direct),
                    "plan-free request reached descriptor semantics");

  auto invalid = Request("project.typed.row.v1");
  invalid.physical_dag.admission_evidence.pop_back();
  const auto refused = exec::ExecuteCanonicalDescriptorProjection(invalid);
  passed &= Require(AtomicStatementContextRefusal(refused),
                    "incomplete canonical admission reached helper");
  passed &= ValidateStatementContextMatrix(
      Request("project.typed.row.v1"), [](const auto& request) {
        return exec::ExecuteCanonicalDescriptorProjection(request);
      });
  return passed;
}

}  // namespace

int main() {
  if (!ValidateCanonicalAdoption()) return 1;
  std::cout << "QOW-TEST-IAS-014-V1: PASS\n";
  return 0;
}
