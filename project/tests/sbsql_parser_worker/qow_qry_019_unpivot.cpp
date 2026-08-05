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

constexpr std::uint64_t kOwner = 0xffff'ffff'ffff'ff00ULL;

bool Require(const bool condition, const std::string_view detail) {
  if (!condition) std::cerr << "QOW-TEST-QRY-019-UNPIVOT-V1: " << detail << '\n';
  return condition;
}

api::EngineDescriptor Descriptor(const std::string& descriptor_uuid,
                                 const std::string& type_uuid,
                                 const std::string& type_name) {
  api::EngineDescriptor descriptor;
  descriptor.descriptor_uuid.canonical = descriptor_uuid;
  descriptor.descriptor_kind = "scalar";
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

api::EngineTypedValue Null(const api::EngineDescriptor& descriptor) {
  api::EngineTypedValue value;
  value.descriptor = descriptor;
  value.is_null = true;
  value.state = api::EngineValueState::sql_null;
  return value;
}

exec::CanonicalExecutionMgaAuthority Bind(exec::TypedPhysicalNodeDag* dag) {
  dag->admission_evidence = {
      {exec::PhysicalAdmissionStage::kBoundRequest,
       "019f0000-0000-7200-8000-000000019211"},
      {exec::PhysicalAdmissionStage::kCatalogEpoch,
       "019f0000-0000-7200-8000-000000019212"},
      {exec::PhysicalAdmissionStage::kSecurity,
       "019f0000-0000-7200-8000-000000019213"},
      {exec::PhysicalAdmissionStage::kMgaStatementBoundary,
       "019f0000-0000-7200-8000-000000019203"},
      {exec::PhysicalAdmissionStage::kPolicyCapability,
       "019f0000-0000-7200-8000-000000019214"},
      {exec::PhysicalAdmissionStage::kResource,
       "019f0000-0000-7200-8000-000000019215"},
      {exec::PhysicalAdmissionStage::kStatisticsProvenance,
       "019f0000-0000-7200-8000-000000019216"},
      {exec::PhysicalAdmissionStage::kCanonicalRoute,
       "019f0000-0000-7200-8000-000000019217"},
  };
  const exec::PhysicalMgaStatementContext context{
      "019f0000-0000-7200-8000-000000019201",
      "019f0000-0000-7200-8000-000000019202",
      "019f0000-0000-7200-8000-000000019203",
      "019f0000-0000-7200-8000-000000019204",
      kOwner,
      0,
      kOwner - 8,
      kOwner - 16,
      kOwner - 16,
      kOwner - 16,
      {kOwner - 8, kOwner},
      {},
      "statement_stable",
      kOwner + 1,
      true,
      true,
      true};
  dag->abi_version = 2;
  dag->local_transaction_id = kOwner;
  dag->bound_sblr_tree_uuid =
      "019f0000-0000-7200-8000-000000019211";
  dag->catalog_epoch_uuid =
      "019f0000-0000-7200-8000-000000019212";
  dag->security_context_uuid =
      "019f0000-0000-7200-8000-000000019213";
  dag->capability_snapshot_uuid =
      "019f0000-0000-7200-8000-000000019214";
  dag->resource_snapshot_uuid =
      "019f0000-0000-7200-8000-000000019215";
  dag->statistics_snapshot_uuid =
      "019f0000-0000-7200-8000-000000019216";
  dag->route_snapshot_uuid =
      "019f0000-0000-7200-8000-000000019217";
  dag->catalog_generation = dag->security_epoch = dag->policy_epoch = 1;
  dag->resource_epoch = dag->statistics_generation = dag->route_epoch = 1;
  dag->route_generation = 1;
  dag->memory_budget_bytes = 4096;
  dag->optimizer_published = true;
  dag->immutable_node_identity_validated = true;
  dag->capability_validated_before_access = true;
  dag->mga_statement_context = context;
  for (auto& node : dag->nodes) {
    node.mga_statement_context = context;
    node.selected_alternative_uuid =
        "019f0000-0000-7200-8000-000000019221";
    node.executor_capability_uuid =
        "019f0000-0000-7200-8000-000000019222";
    node.executor_capability_abi_version = 1;
    node.cost_vector_uuid =
        "019f0000-0000-7200-8000-000000019223";
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

exec::CanonicalUnpivotRequest Request(const bool include_nulls) {
  const auto group = Descriptor(
      "019f0000-0000-7300-8000-000000019301",
      "019f0000-0000-7400-8000-000000019301", "int64");
  const auto amount = Descriptor(
      "019f0000-0000-7300-8000-000000019302",
      "019f0000-0000-7400-8000-000000019302", "int64");
  const auto label = Descriptor(
      "019f0000-0000-7300-8000-000000019303",
      "019f0000-0000-7400-8000-000000019303", "text");
  const auto result_group = Descriptor(
      "019f0000-0000-7300-8000-000000019304",
      "019f0000-0000-7400-8000-000000019301", "int64");
  const auto result_label = Descriptor(
      "019f0000-0000-7300-8000-000000019305",
      "019f0000-0000-7400-8000-000000019303", "text");
  const auto result_amount = Descriptor(
      "019f0000-0000-7300-8000-000000019306",
      "019f0000-0000-7400-8000-000000019302", "int64");

  exec::CanonicalUnpivotRequest request;
  request.physical_dag.selected_plan_uuid =
      "019f0000-0000-7200-8000-000000019230";
  request.physical_dag.root_physical_node_id = 1922;
  request.physical_dag.nodes = {
      {.physical_node_id = 1921,
       .relational_node_id = 1921,
       .node_kind = exec::PhysicalNodeKind::kValues,
       .implementation_id = "values.materialize.canonical.v1",
       .output_descriptor_ids = {1, 2, 3},
       .causal_counter_id = 19201},
      {.physical_node_id = 1922,
       .relational_node_id = 1922,
       .node_kind = exec::PhysicalNodeKind::kUnpivot,
       .implementation_id = include_nulls
                                ? "unpivot.canonical.include-nulls.typed.v1"
                                : "unpivot.canonical.exclude-nulls.typed.v1",
       .input_physical_node_ids = {1921},
       .output_descriptor_ids = {4, 5, 6},
       .causal_counter_id = 19202},
  };
  request.selected_physical_node_id = 1922;
  request.input_batch = exec::MakeDescriptorBatch(
      {{"group_key", group, true, 1},
       {"q1", amount, true, 2},
       {"q2", amount, true, 3}},
      {{{Value(group, "1"), Value(amount, "10"), Null(amount)}},
       {{Value(group, "2"), Null(amount), Null(amount)}}});
  request.group_columns = {0};
  request.in_items = {
      {{1}, Value(label, "q1")},
      {{2}, Value(label, "q2")},
  };
  request.result_columns = {
      {"group_key", result_group, true, 4},
      {"quarter", result_label, true, 5},
      {"amount", result_amount, true, 6},
  };
  request.null_policy = include_nulls
                            ? exec::CanonicalPivotNullPolicy::kInclude
                            : exec::CanonicalPivotNullPolicy::kExclude;
  request.maximum_output_row_count = 8;
  request.maximum_output_cell_count = 32;
  request.mga_authority = Bind(&request.physical_dag);
  return request;
}

bool ValidateUnpivot() {
  auto excluded_request = Request(false);
  const auto excluded = exec::ExecuteCanonicalUnpivot(excluded_request);
  if (!excluded.diagnostic.ok) {
    std::cerr << "QOW-TEST-QRY-019-UNPIVOT-V1: first diagnostic "
              << excluded.diagnostic.diagnostic_code << ": "
              << excluded.diagnostic.detail << '\n';
  }
  bool passed = true;
  passed &= Require(
      excluded.diagnostic.ok && excluded.output_batch.rows.size() == 1 &&
          excluded.output_batch.rows[0].values[0].encoded_value == "1" &&
          excluded.output_batch.rows[0].values[1].encoded_value == "q1" &&
          excluded.output_batch.rows[0].values[2].encoded_value == "10" &&
          excluded.null_excluded_row_count == 3 &&
          excluded.executed_physical_node_id == 1922 &&
          excluded.causal_counter_id == 19202,
      "UNPIVOT EXCLUDE NULLS did not convert the exact non-NULL column to a row");

  const auto included = exec::ExecuteCanonicalUnpivot(Request(true));
  if (!included.diagnostic.ok) {
    std::cerr << "QOW-TEST-QRY-019-UNPIVOT-V1: include diagnostic "
              << included.diagnostic.diagnostic_code << ": "
              << included.diagnostic.detail << '\n';
  }
  passed &= Require(
      included.diagnostic.ok && included.output_batch.rows.size() == 4 &&
          included.null_excluded_row_count == 0 &&
          included.output_batch.rows[1].values[2].state ==
              api::EngineValueState::sql_null &&
          included.output_batch.rows[3].values[2].state ==
              api::EngineValueState::sql_null,
      "UNPIVOT INCLUDE NULLS did not preserve NULL-valued items");

  auto bounded_request = Request(true);
  bounded_request.maximum_output_row_count = 3;
  const auto bounded = exec::ExecuteCanonicalUnpivot(bounded_request);
  passed &= Require(
      !bounded.diagnostic.ok && bounded.output_batch.rows.empty() &&
          bounded.diagnostic.diagnostic_code ==
              "SBLR.PLAN_TREE.RESOURCE_LIMIT",
      "UNPIVOT output exhaustion published a partial result");

  auto invalid_request = Request(false);
  invalid_request.in_items[1].source_columns.push_back(1);
  const auto invalid = exec::ExecuteCanonicalUnpivot(invalid_request);
  passed &= Require(
      !invalid.diagnostic.ok && invalid.output_batch.rows.empty() &&
          invalid.diagnostic.diagnostic_code ==
              "QOW-DIAG-QRY-019-UNPIVOT-IN-V1",
      "UNPIVOT admitted a mismatched IN tuple arity");
  return passed;
}

}  // namespace

int main() { return ValidateUnpivot() ? EXIT_SUCCESS : EXIT_FAILURE; }
