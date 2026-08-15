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
    std::cerr << "QOW-TEST-QRY-014-UNION-V1: " << detail << '\n';
  }
  return condition;
}

exec::PhysicalMgaStatementContext StatementContext(
    const std::string& statement_snapshot_uuid) {
  return {
      "019f0000-0000-7200-8000-00000000f301",
      "019f0000-0000-7200-8000-00000000f302",
      statement_snapshot_uuid,
      "019f0000-0000-7200-8000-00000000f303",
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
        "019f0000-0000-7200-8000-00000000f304";
    node.executor_capability_uuid =
        "019f0000-0000-7200-8000-00000000f305";
    node.executor_capability_abi_version = 1;
    node.cost_vector_uuid =
        "019f0000-0000-7200-8000-00000000f306";
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

api::EngineDescriptor Descriptor() {
  api::EngineDescriptor descriptor;
  descriptor.descriptor_uuid.canonical =
      "019f0000-0000-7200-8000-000000003301";
  descriptor.descriptor_kind = "scalar";
  descriptor.canonical_type_name = "int64";
  descriptor.encoded_descriptor =
      "type_uuid=019f0000-0000-7300-8000-000000003302;"
      "nullability=nullable";
  return descriptor;
}

api::EngineDescriptor Descriptor(const std::string& descriptor_uuid,
                                 const std::string& type_uuid,
                                 const std::string& type_name) {
  auto descriptor = Descriptor();
  descriptor.descriptor_uuid.canonical = descriptor_uuid;
  descriptor.canonical_type_name = type_name;
  descriptor.encoded_descriptor =
      "type_uuid=" + type_uuid + ";nullability=nullable";
  return descriptor;
}

api::EngineTypedValue Value(const api::EngineDescriptor& descriptor,
                            std::string encoded) {
  api::EngineTypedValue value;
  value.descriptor = descriptor;
  value.encoded_value = std::move(encoded);
  value.state = api::EngineValueState::value;
  return value;
}

exec::CanonicalRecursiveCteUnionRequest Request(
    const exec::CanonicalRecursiveCteUnionMode mode) {
  const auto descriptor = Descriptor();
  exec::CanonicalRecursiveCteUnionRequest request;
  request.union_mode = mode;
  auto& working = request.working_request;
  working.physical_dag.selected_plan_uuid =
      "019f0000-0000-7200-8000-000000003303";
  working.physical_dag.root_physical_node_id = 3303;
  working.physical_dag.local_transaction_id = 3304;
  working.physical_dag.statement_snapshot_id = 3305;
  working.physical_dag.admission_evidence = {
      {exec::PhysicalAdmissionStage::kBoundRequest,
       "019f0000-0000-7200-8000-000000003311"},
      {exec::PhysicalAdmissionStage::kCatalogEpoch,
       "019f0000-0000-7200-8000-000000003312"},
      {exec::PhysicalAdmissionStage::kSecurity,
       "019f0000-0000-7200-8000-000000003313"},
      {exec::PhysicalAdmissionStage::kMgaStatementBoundary,
       "019f0000-0000-7200-8000-000000003314"},
      {exec::PhysicalAdmissionStage::kPolicyCapability,
       "019f0000-0000-7200-8000-000000003315"},
      {exec::PhysicalAdmissionStage::kResource,
       "019f0000-0000-7200-8000-000000003316"},
      {exec::PhysicalAdmissionStage::kStatisticsProvenance,
       "019f0000-0000-7200-8000-000000003317"},
      {exec::PhysicalAdmissionStage::kCanonicalRoute,
       "019f0000-0000-7200-8000-000000003318"},
  };
  const std::string root_profile =
      mode == exec::CanonicalRecursiveCteUnionMode::kAll
          ? "cte.recursive.union-all.typed.v1"
          : "cte.recursive.union-distinct-int64.typed.v1";
  working.physical_dag.nodes = {
      {.physical_node_id = 3301,
       .relational_node_id = 3301,
       .node_kind = exec::PhysicalNodeKind::kValues,
       .implementation_id = "values.cte-union-anchor.typed.v1",
       .output_descriptor_ids = {3301},
       .causal_counter_id = 33001},
      {.physical_node_id = 3302,
       .relational_node_id = 3302,
       .node_kind = exec::PhysicalNodeKind::kCte,
       .implementation_id = "cte.recursive-union-term.typed.v1",
       .output_descriptor_ids = {3301},
       .causal_counter_id = 33002},
      {.physical_node_id = 3303,
       .relational_node_id = 3303,
       .node_kind = exec::PhysicalNodeKind::kRecursiveCte,
       .implementation_id = root_profile,
       .input_physical_node_ids = {3301, 3302},
       .output_descriptor_ids = {3301},
       .causal_counter_id = 33003},
  };
  working.selected_physical_node_id = 3303;
  working.anchor_batch = exec::MakeDescriptorBatch(
      {{"n", descriptor, true, 3301}},
      {{{Value(descriptor, "1")}}, {{Value(descriptor, "01")}}});
  working.recursive_step =
      [descriptor](const exec::DescriptorBatch& current, const std::size_t) {
        exec::DescriptorBatch next;
        next.columns = current.columns;
        for (const auto& row : current.rows) {
          const auto value = std::stoll(row.values[0].encoded_value);
          next.rows.push_back({{Value(descriptor, std::to_string(value))}});
          if (value < 3) {
            next.rows.push_back(
                {{Value(descriptor, std::to_string(value + 1))}});
          }
        }
        return next;
      };
  working.maximum_iteration_count = 8;
  working.maximum_working_row_count = 16;
  working.maximum_result_row_count = 64;
  working.mga_authority = BindPhysicalAbiV2(&working.physical_dag);
  return request;
}

exec::CanonicalRecursiveCteUnionRequest CompositeTypedRequest() {
  auto request = Request(exec::CanonicalRecursiveCteUnionMode::kDistinct);
  auto& working = request.working_request;
  const auto amount = Descriptor(
      "019f0000-0000-7200-8000-000000003321",
      "019f0000-0000-7300-8000-000000003322", "real64");
  const auto flag = Descriptor(
      "019f0000-0000-7200-8000-000000003323",
      "019f0000-0000-7300-8000-000000003324", "boolean");
  for (auto& node : working.physical_dag.nodes) {
    node.output_descriptor_ids = {3301, 3302};
  }
  working.physical_dag.nodes[2].implementation_id =
      "cte.recursive.union-distinct.typed.v1";
  working.anchor_batch = exec::MakeDescriptorBatch(
      {{"amount", amount, true, 3301}, {"flag", flag, true, 3302}},
      {{{Value(amount, "1.5"), Value(flag, "true")}},
       {{Value(amount, "1.50"), Value(flag, "true")}},
       {{Value(amount, "1.5"), Value(flag, "false")}}});
  working.recursive_step =
      [amount, flag](const exec::DescriptorBatch& current,
                     const std::size_t iteration) {
        exec::DescriptorBatch next;
        next.columns = current.columns;
        if (iteration == 1) {
          next.rows = {
              {{Value(amount, "1.50"), Value(flag, "true")}},
              {{Value(amount, "2.5"), Value(flag, "true")}},
          };
        } else if (iteration == 2) {
          next.rows = {
              {{Value(amount, "2.50"), Value(flag, "true")}},
              {{Value(amount, "3.5"), Value(flag, "true")}},
          };
        } else if (iteration == 3) {
          next.rows = {
              {{Value(amount, "3.50"), Value(flag, "true")}},
          };
        }
        return next;
      };
  request.equality_terms = {
      {.column = 0,
       .expression_descriptor_id = 3301,
       .direction = exec::CanonicalDescriptorOrderDirection::ascending,
       .null_placement = exec::CanonicalDescriptorNullPlacement::first},
      {.column = 1,
       .expression_descriptor_id = 3302,
       .direction = exec::CanonicalDescriptorOrderDirection::ascending,
       .null_placement = exec::CanonicalDescriptorNullPlacement::first},
  };
  request.maximum_value_comparison_count = 128;
  working.maximum_working_row_count = 8;
  working.maximum_result_row_count = 16;
  return request;
}

// QOW-TEST-QRY-014-UNION-V1
bool ValidateUnionModes() {
  bool passed = true;
  auto result = exec::ExecuteCanonicalRecursiveCteUnion(
      Request(exec::CanonicalRecursiveCteUnionMode::kDistinct));
  passed &= Require(
      result.working_result.diagnostic.ok &&
          result.working_result.converged &&
          result.working_result.recursive_iteration_count == 3 &&
          result.working_result.output_batch.rows.size() == 3 &&
          result.working_result.output_batch.rows[0].values[0].encoded_value ==
              "1" &&
          result.working_result.output_batch.rows[1].values[0].encoded_value ==
              "2" &&
          result.working_result.output_batch.rows[2].values[0].encoded_value ==
              "3" &&
          result.duplicate_row_count == 4 &&
          result.working_result.executed_physical_node_id == 3303 &&
          result.mga_statement_context.visible_committed_high_watermark == 0 &&
          exec::PhysicalMgaStatementContextEqual(
              result.mga_statement_context,
              Request(exec::CanonicalRecursiveCteUnionMode::kDistinct)
                  .working_request.mga_authority.statement_context),
      "UNION DISTINCT did not remove typed duplicates across iterations");

  result = exec::ExecuteCanonicalRecursiveCteUnion(
      CompositeTypedRequest());
  passed &= Require(
      result.working_result.diagnostic.ok &&
          result.working_result.converged &&
          result.working_result.recursive_iteration_count == 3 &&
          result.working_result.output_batch.columns.size() == 2 &&
          result.working_result.output_batch.rows.size() == 4 &&
          result.working_result.output_batch.rows[0]
                  .values[0]
                  .encoded_value == "1.5" &&
          result.working_result.output_batch.rows[1]
                  .values[1]
                  .encoded_value == "false" &&
          result.working_result.output_batch.rows[2]
                  .values[0]
                  .encoded_value == "2.5" &&
          result.working_result.output_batch.rows[3]
                  .values[0]
                  .encoded_value == "3.5" &&
          result.duplicate_row_count == 4,
      "descriptor-wide recursive UNION DISTINCT lost typed row equality");

  auto typed_request = CompositeTypedRequest();
  typed_request.equality_terms.pop_back();
  result = exec::ExecuteCanonicalRecursiveCteUnion(typed_request);
  passed &= Require(!result.working_result.diagnostic.ok,
                    "incomplete recursive DISTINCT equality coverage was accepted");

  auto request = Request(exec::CanonicalRecursiveCteUnionMode::kAll);
  request.working_request.recursive_step =
      [](const exec::DescriptorBatch& current, const std::size_t iteration) {
        exec::DescriptorBatch next;
        next.columns = current.columns;
        if (iteration == 1) next.rows = current.rows;
        return next;
      };
  result = exec::ExecuteCanonicalRecursiveCteUnion(request);
  passed &= Require(result.working_result.diagnostic.ok &&
                        result.working_result.output_batch.rows.size() == 4 &&
                        result.duplicate_row_count == 0,
                    "UNION ALL did not preserve duplicate rows");

  request = Request(exec::CanonicalRecursiveCteUnionMode::kDistinct);
  request.working_request.physical_dag.nodes[2].implementation_id =
      "cte.recursive.union-all.typed.v1";
  result = exec::ExecuteCanonicalRecursiveCteUnion(request);
  passed &= Require(!result.working_result.diagnostic.ok,
                    "UNION mode/profile drift was accepted");

  request = Request(exec::CanonicalRecursiveCteUnionMode::kDistinct);
  request.working_request.recursive_step =
      [](const exec::DescriptorBatch& current, const std::size_t) {
        auto malformed = current;
        malformed.rows[0].values[0].encoded_value = "bad";
        return malformed;
      };
  result = exec::ExecuteCanonicalRecursiveCteUnion(request);
  passed &= Require(!result.working_result.diagnostic.ok &&
                        result.working_result.output_batch.rows.empty() &&
                        result.duplicate_row_count == 0,
                    "malformed DISTINCT row published prior state");

  request = Request(exec::CanonicalRecursiveCteUnionMode::kDistinct);
  request.working_request.anchor_batch.columns[0]
      .descriptor.canonical_type_name = "text";
  request.working_request.anchor_batch.rows[0]
      .values[0].descriptor.canonical_type_name = "text";
  request.working_request.anchor_batch.rows[1]
      .values[0].descriptor.canonical_type_name = "text";
  result = exec::ExecuteCanonicalRecursiveCteUnion(request);
  passed &= Require(!result.working_result.diagnostic.ok,
                    "unsupported DISTINCT descriptor profile was accepted");

  request = Request(exec::CanonicalRecursiveCteUnionMode::kDistinct);
  request.working_request.physical_dag.local_transaction_id = 0;
  result = exec::ExecuteCanonicalRecursiveCteUnion(request);
  passed &= Require(!result.working_result.diagnostic.ok &&
                        result.working_result.output_batch.rows.empty() &&
                        result.duplicate_row_count == 0 &&
                        !exec::PhysicalMgaStatementContextValid(
                            result.mga_statement_context),
                    "missing MGA transaction reached recursive UNION");

  request = Request(exec::CanonicalRecursiveCteUnionMode::kDistinct);
  request.working_request.mga_authority.resolve_current = {};
  result = exec::ExecuteCanonicalRecursiveCteUnion(request);
  passed &= Require(!result.working_result.diagnostic.ok &&
                        result.working_result.output_batch.rows.empty(),
                    "missing current MGA resolver reached recursive UNION");

  request = Request(exec::CanonicalRecursiveCteUnionMode::kDistinct);
  request.working_request.physical_dag.catalog_epoch_uuid =
      request.working_request.physical_dag.mga_statement_context
          .statement_metadata_snapshot_uuid;
  result = exec::ExecuteCanonicalRecursiveCteUnion(request);
  passed &= Require(!result.working_result.diagnostic.ok &&
                        result.working_result.output_batch.rows.empty(),
                    "catalog metadata substitution reached recursive UNION");

  request = Request(exec::CanonicalRecursiveCteUnionMode::kDistinct);
  request.working_request.physical_dag.local_transaction_id =
      static_cast<std::uint32_t>(kOwnerLocalTransactionId);
  result = exec::ExecuteCanonicalRecursiveCteUnion(request);
  passed &= Require(!result.working_result.diagnostic.ok &&
                        result.working_result.output_batch.rows.empty(),
                    "narrowed MGA identity reached recursive UNION");
  return passed;
}

}  // namespace

int main() { return ValidateUnionModes() ? EXIT_SUCCESS : EXIT_FAILURE; }
