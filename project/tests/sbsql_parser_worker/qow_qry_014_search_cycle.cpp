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
    std::cerr << "QOW-TEST-QRY-014-SEARCH-CYCLE-V1: " << detail << '\n';
  }
  return condition;
}

exec::PhysicalMgaStatementContext StatementContext(
    const std::string& statement_snapshot_uuid) {
  return {
      "019f0000-0000-7200-8000-00000000f401",
      "019f0000-0000-7200-8000-00000000f402",
      statement_snapshot_uuid,
      "019f0000-0000-7200-8000-00000000f403",
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
        "019f0000-0000-7200-8000-00000000f404";
    node.executor_capability_uuid =
        "019f0000-0000-7200-8000-00000000f405";
    node.executor_capability_abi_version = 1;
    node.cost_vector_uuid =
        "019f0000-0000-7200-8000-00000000f406";
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

api::EngineDescriptor Descriptor(const std::string& uuid,
                                 const std::string& type_uuid,
                                 const std::string& type_name) {
  api::EngineDescriptor descriptor;
  descriptor.descriptor_uuid.canonical = uuid;
  descriptor.descriptor_kind = "scalar";
  descriptor.canonical_type_name = type_name;
  descriptor.encoded_descriptor =
      "type_uuid=" + type_uuid + ";nullability=not-null";
  return descriptor;
}

api::EngineTypedValue Value(const api::EngineDescriptor& descriptor,
                            const std::int64_t value) {
  api::EngineTypedValue typed;
  typed.descriptor = descriptor;
  typed.encoded_value = std::to_string(value);
  typed.state = api::EngineValueState::value;
  return typed;
}

exec::CanonicalRecursiveCteSearchCycleRequest Request() {
  const auto key = Descriptor(
      "019f0000-0000-7200-8000-000000003401",
      "019f0000-0000-7300-8000-000000003402", "int64");
  const auto sequence = Descriptor(
      "019f0000-0000-7200-8000-000000003403",
      "019f0000-0000-7300-8000-000000003404", "int64");
  const auto cycle = Descriptor(
      "019f0000-0000-7200-8000-000000003405",
      "019f0000-0000-7300-8000-000000003406", "boolean");

  exec::CanonicalRecursiveCteSearchCycleRequest request;
  request.physical_dag.selected_plan_uuid =
      "019f0000-0000-7200-8000-000000003407";
  request.physical_dag.root_physical_node_id = 3403;
  request.physical_dag.local_transaction_id = 3404;
  request.physical_dag.statement_snapshot_id = 3405;
  request.physical_dag.admission_evidence = {
      {exec::PhysicalAdmissionStage::kBoundRequest,
       "019f0000-0000-7200-8000-000000003411"},
      {exec::PhysicalAdmissionStage::kCatalogEpoch,
       "019f0000-0000-7200-8000-000000003412"},
      {exec::PhysicalAdmissionStage::kSecurity,
       "019f0000-0000-7200-8000-000000003413"},
      {exec::PhysicalAdmissionStage::kMgaStatementBoundary,
       "019f0000-0000-7200-8000-000000003414"},
      {exec::PhysicalAdmissionStage::kPolicyCapability,
       "019f0000-0000-7200-8000-000000003415"},
      {exec::PhysicalAdmissionStage::kResource,
       "019f0000-0000-7200-8000-000000003416"},
      {exec::PhysicalAdmissionStage::kStatisticsProvenance,
       "019f0000-0000-7200-8000-000000003417"},
      {exec::PhysicalAdmissionStage::kCanonicalRoute,
       "019f0000-0000-7200-8000-000000003418"},
  };
  request.physical_dag.nodes = {
      {.physical_node_id = 3401,
       .relational_node_id = 3401,
       .node_kind = exec::PhysicalNodeKind::kValues,
       .implementation_id = "values.cte-search-anchor.typed.v1",
       .output_descriptor_ids = {3401},
       .causal_counter_id = 34001},
      {.physical_node_id = 3402,
       .relational_node_id = 3402,
       .node_kind = exec::PhysicalNodeKind::kCte,
       .implementation_id = "cte.search-cycle-term.typed.v1",
       .output_descriptor_ids = {3401},
       .causal_counter_id = 34002},
      {.physical_node_id = 3403,
       .relational_node_id = 3403,
       .node_kind = exec::PhysicalNodeKind::kRecursiveCte,
       .implementation_id =
           "cte.recursive.search-breadth-cycle-int64.typed.v1",
       .input_physical_node_ids = {3401, 3402},
       .output_descriptor_ids = {3401, 3402, 3403},
       .causal_counter_id = 34003},
  };
  request.selected_physical_node_id = 3403;
  request.anchor_batch = exec::MakeDescriptorBatch(
      {{"node", key, false, 3401}}, {{{Value(key, 1)}}});
  request.recursive_step =
      [key](const exec::DescriptorBatch& current, const std::size_t) {
        exec::CanonicalRecursiveCteGeneratedBatch generated;
        generated.batch.columns = current.columns;
        for (std::size_t parent = 0; parent < current.rows.size(); ++parent) {
          const auto value =
              std::stoll(current.rows[parent].values[0].encoded_value);
          if (value == 1) {
            generated.batch.rows.push_back({{Value(key, 2)}});
            generated.parent_working_row_indices.push_back(parent);
            generated.batch.rows.push_back({{Value(key, 3)}});
            generated.parent_working_row_indices.push_back(parent);
          } else if (value == 2) {
            generated.batch.rows.push_back({{Value(key, 1)}});
            generated.parent_working_row_indices.push_back(parent);
          } else if (value == 3) {
            generated.batch.rows.push_back({{Value(key, 4)}});
            generated.parent_working_row_indices.push_back(parent);
          }
        }
        return generated;
      };
  request.search_order =
      exec::CanonicalRecursiveCteSearchOrder::kBreadthFirst;
  request.cycle_key_column = 0;
  request.cycle_key_expression_descriptor_id = 3401;
  request.search_sequence_column = {"search_sequence", sequence, false, 3402};
  request.cycle_mark_column = {"is_cycle", cycle, false, 3403};
  request.maximum_iteration_count = 8;
  request.maximum_working_row_count = 8;
  request.maximum_result_row_count = 16;
  request.mga_authority = BindPhysicalAbiV2(&request.physical_dag);
  return request;
}

// QOW-TEST-QRY-014-SEARCH-CYCLE-V1
bool ValidateSearchCycle() {
  bool passed = true;
  auto result = exec::ExecuteCanonicalRecursiveCteSearchCycle(Request());
  passed &= Require(
      result.diagnostic.ok && result.converged &&
          result.recursive_iteration_count == 3 &&
          result.cycle_row_count == 1 && result.output_batch.rows.size() == 5 &&
          result.output_batch.columns.size() == 3 &&
          result.output_batch.rows[0].values[0].encoded_value == "1" &&
          result.output_batch.rows[1].values[0].encoded_value == "2" &&
          result.output_batch.rows[2].values[0].encoded_value == "3" &&
          result.output_batch.rows[3].values[0].encoded_value == "1" &&
          result.output_batch.rows[3].values[2].encoded_value == "true" &&
          result.output_batch.rows[4].values[0].encoded_value == "4" &&
          result.output_batch.rows[4].values[1].encoded_value == "5" &&
          result.row_metadata[3].depth == 2 &&
          result.row_metadata[3].cycle &&
          result.executed_physical_node_id == 3403 &&
          result.mga_statement_context.visible_committed_high_watermark == 0 &&
          exec::PhysicalMgaStatementContextEqual(
              result.mga_statement_context,
              Request().mga_authority.statement_context),
      "breadth-first SEARCH/CYCLE output was not path-correct");

  auto request = Request();
  request.search_order = exec::CanonicalRecursiveCteSearchOrder::kDepthFirst;
  result = exec::ExecuteCanonicalRecursiveCteSearchCycle(request);
  passed &= Require(!result.diagnostic.ok,
                    "unapproved depth-first SEARCH route was accepted");

  request = Request();
  request.recursive_step =
      [step = request.recursive_step](const exec::DescriptorBatch& current,
                                      const std::size_t iteration) {
        auto generated = step(current, iteration);
        if (!generated.parent_working_row_indices.empty()) {
          generated.parent_working_row_indices[0] = current.rows.size();
        }
        return generated;
      };
  result = exec::ExecuteCanonicalRecursiveCteSearchCycle(request);
  passed &= Require(!result.diagnostic.ok && result.output_batch.rows.empty(),
                    "unresolved cycle parent published partial rows");

  request = Request();
  request.maximum_result_row_count = 4;
  result = exec::ExecuteCanonicalRecursiveCteSearchCycle(request);
  passed &= Require(!result.diagnostic.ok && result.output_batch.rows.empty(),
                    "SEARCH/CYCLE result excess published partial rows");

  request = Request();
  request.physical_dag.nodes[2].output_descriptor_ids = {3401, 3403, 3402};
  result = exec::ExecuteCanonicalRecursiveCteSearchCycle(request);
  passed &= Require(!result.diagnostic.ok,
                    "SEARCH/CYCLE synthetic descriptor order drifted");

  request = Request();
  request.physical_dag.local_transaction_id = 0;
  result = exec::ExecuteCanonicalRecursiveCteSearchCycle(request);
  passed &= Require(!result.diagnostic.ok && result.output_batch.rows.empty() &&
                        result.row_metadata.empty() &&
                        result.selected_plan_uuid.empty() &&
                        result.executed_physical_node_id == 0 &&
                        !exec::PhysicalMgaStatementContextValid(
                            result.mga_statement_context),
                    "SEARCH/CYCLE accepted a missing MGA transaction");

  request = Request();
  request.mga_authority.resolve_current = {};
  result = exec::ExecuteCanonicalRecursiveCteSearchCycle(request);
  passed &= Require(!result.diagnostic.ok && result.output_batch.rows.empty(),
                    "missing current MGA resolver reached SEARCH/CYCLE");

  request = Request();
  request.physical_dag.catalog_epoch_uuid =
      request.physical_dag.mga_statement_context
          .statement_metadata_snapshot_uuid;
  result = exec::ExecuteCanonicalRecursiveCteSearchCycle(request);
  passed &= Require(!result.diagnostic.ok && result.output_batch.rows.empty(),
                    "catalog metadata substitution reached SEARCH/CYCLE");

  request = Request();
  request.physical_dag.local_transaction_id =
      static_cast<std::uint32_t>(kOwnerLocalTransactionId);
  result = exec::ExecuteCanonicalRecursiveCteSearchCycle(request);
  passed &= Require(!result.diagnostic.ok && result.output_batch.rows.empty(),
                    "narrowed MGA identity reached SEARCH/CYCLE");
  return passed;
}

}  // namespace

int main() { return ValidateSearchCycle() ? EXIT_SUCCESS : EXIT_FAILURE; }
