// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "descriptor_value_runtime.hpp"

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace exec = scratchbird::engine::executor;
namespace api = scratchbird::engine::internal_api;
namespace dt = scratchbird::core::datatypes;

namespace {

constexpr std::string_view kWindowCollationUuid =
    "019f0000-0000-7400-8000-000000004001";

bool Require401(const bool condition, const std::string_view detail) {
  if (!condition) std::cerr << "QOW-TEST-WIN-401-V1: " << detail << '\n';
  return condition;
}

std::string WindowUuid(const unsigned value) {
  char buffer[37]{};
  std::snprintf(buffer, sizeof(buffer),
                "019f0000-0000-7400-8000-%012u", value);
  return buffer;
}

api::EngineDescriptor WindowDescriptor(
    const unsigned descriptor_uuid, const std::string& canonical_type,
    const std::string& encoded_descriptor) {
  api::EngineDescriptor descriptor;
  descriptor.descriptor_uuid.canonical = WindowUuid(descriptor_uuid);
  descriptor.descriptor_kind = "scalar";
  descriptor.canonical_type_name = canonical_type;
  descriptor.encoded_descriptor = encoded_descriptor;
  return descriptor;
}

api::EngineTypedValue WindowValue(const api::EngineDescriptor& descriptor,
                                  const std::string& encoded) {
  api::EngineTypedValue value;
  value.descriptor = descriptor;
  value.encoded_value = encoded;
  value.state = api::EngineValueState::value;
  return value;
}

api::EngineTypedValue WindowNull(const api::EngineDescriptor& descriptor) {
  api::EngineTypedValue value;
  value.descriptor = descriptor;
  value.is_null = true;
  value.state = api::EngineValueState::sql_null;
  return value;
}

dt::DatatypeTextSeedAuthority WindowCollationSeed() {
  dt::DatatypeTextSeedAuthority seed;
  seed.active = true;
  seed.seed_pack_name = "qow.window.seed";
  seed.seed_pack_version = "1";
  seed.charset_name = "utf8";
  seed.collation_name = "qow_window_ci";
  seed.collation_case_insensitive = true;
  return seed;
}

exec::CanonicalDescriptorOrderTerm WindowTextOrderTerm(
    const std::size_t column, const std::uint32_t descriptor_id,
    const exec::CanonicalDescriptorOrderDirection direction,
    const exec::CanonicalDescriptorNullPlacement null_placement) {
  exec::CanonicalDescriptorOrderTerm term;
  term.column = column;
  term.expression_descriptor_id = descriptor_id;
  term.direction = direction;
  term.null_placement = null_placement;
  term.collation_uuid = kWindowCollationUuid;
  term.resource_epoch = 401;
  term.collation_epoch = 402;
  term.text_seed = WindowCollationSeed();
  return term;
}

exec::CanonicalWindowPartitionOrderRequest Window401Request() {
  const auto text_descriptor = WindowDescriptor(
      4101, "text", "type_uuid=" + WindowUuid(4201) +
                        ";nullability=nullable;collation_uuid=" +
                        std::string(kWindowCollationUuid));
  const auto part_descriptor = WindowDescriptor(
      4102, "int64",
      "type_uuid=" + WindowUuid(4202) + ";nullability=non_null");
  const auto order_descriptor = WindowDescriptor(
      4103, "int64",
      "type_uuid=" + WindowUuid(4203) + ";nullability=nullable");
  const auto tie_descriptor = WindowDescriptor(
      4104, "text", "type_uuid=" + WindowUuid(4204) +
                        ";nullability=non_null;collation_uuid=" +
                        std::string(kWindowCollationUuid));
  const auto payload_descriptor = WindowDescriptor(
      4105, "int64",
      "type_uuid=" + WindowUuid(4205) + ";nullability=non_null");

  exec::CanonicalWindowPartitionOrderRequest request;
  auto& dag = request.physical_dag;
  dag.abi_version = 2;
  dag.selected_plan_uuid = WindowUuid(4301);
  dag.root_physical_node_id = 2;
  dag.local_transaction_id = 4011;
  dag.statement_snapshot_id = 4012;
  dag.bound_sblr_tree_uuid = WindowUuid(4302);
  dag.catalog_epoch_uuid = WindowUuid(4303);
  dag.security_context_uuid = WindowUuid(4304);
  dag.capability_snapshot_uuid = WindowUuid(4305);
  dag.resource_snapshot_uuid = WindowUuid(4306);
  dag.statistics_snapshot_uuid = WindowUuid(4307);
  dag.route_snapshot_uuid = WindowUuid(4308);
  dag.catalog_generation = 41;
  dag.security_epoch = 42;
  dag.policy_epoch = 43;
  dag.resource_epoch = 44;
  dag.statistics_generation = 45;
  dag.route_epoch = 46;
  dag.route_generation = 47;
  dag.memory_budget_bytes = 1 << 20;
  dag.spill_allowed = true;
  dag.optimizer_published = true;
  dag.immutable_node_identity_validated = true;
  dag.capability_validated_before_access = true;
  dag.admission_evidence = {
      {exec::PhysicalAdmissionStage::kBoundRequest, dag.bound_sblr_tree_uuid},
      {exec::PhysicalAdmissionStage::kCatalogEpoch, dag.catalog_epoch_uuid},
      {exec::PhysicalAdmissionStage::kSecurity, dag.security_context_uuid},
      {exec::PhysicalAdmissionStage::kMgaStatementBoundary,
       dag.catalog_epoch_uuid},
      {exec::PhysicalAdmissionStage::kPolicyCapability,
       dag.capability_snapshot_uuid},
      {exec::PhysicalAdmissionStage::kResource, dag.resource_snapshot_uuid},
      {exec::PhysicalAdmissionStage::kStatisticsProvenance,
       dag.statistics_snapshot_uuid},
      {exec::PhysicalAdmissionStage::kCanonicalRoute,
       dag.route_snapshot_uuid},
  };
  const auto partition_property_uuid = WindowUuid(4501);
  const auto ordering_property_uuid = WindowUuid(4502);
  const auto window_property_uuid = WindowUuid(4503);
  dag.nodes = {
      {.physical_node_id = 1,
       .relational_node_id = 1,
       .node_kind = exec::PhysicalNodeKind::kValues,
       .implementation_id = "values.materialize.v1",
       .output_descriptor_ids = {4001, 4002, 4003, 4004, 4005},
       .causal_counter_id = 40101,
       .selected_alternative_uuid = WindowUuid(4601),
       .executor_capability_uuid = WindowUuid(4701),
       .executor_capability_abi_version = 1,
       .cost_vector_uuid = WindowUuid(4801),
       .memory_bytes_required = 4096,
       .engine_capability_validated = true},
      {.physical_node_id = 2,
       .relational_node_id = 2,
       .node_kind = exec::PhysicalNodeKind::kWindow,
       .implementation_id = "window.partition-order-peer.v1",
       .input_physical_node_ids = {1},
       .output_descriptor_ids = {4001, 4002, 4003, 4004, 4005},
       .causal_counter_id = 40102,
       .selected_alternative_uuid = WindowUuid(4602),
       .executor_capability_uuid = WindowUuid(4702),
       .executor_capability_abi_version = 1,
       .cost_vector_uuid = WindowUuid(4802),
       .required_property_uuids = {partition_property_uuid,
                                  ordering_property_uuid},
       .delivered_property_uuids = {window_property_uuid},
       .memory_bytes_required = 16384,
       .engine_capability_validated = true},
  };

  request.selected_physical_node_id = 2;
  request.input_batch = exec::MakeDescriptorBatch(
      {{"part_text", text_descriptor, true, 4001},
       {"part_int", part_descriptor, false, 4002},
       {"order_int", order_descriptor, true, 4003},
       {"order_text", tie_descriptor, false, 4004},
       {"payload", payload_descriptor, false, 4005}},
      {{{WindowValue(text_descriptor, "A"), WindowValue(part_descriptor, "1"),
         WindowValue(order_descriptor, "2"), WindowValue(tie_descriptor, "b"),
         WindowValue(payload_descriptor, "100")}},
       {{WindowValue(text_descriptor, "a"), WindowValue(part_descriptor, "1"),
         WindowValue(order_descriptor, "1"), WindowValue(tie_descriptor, "z"),
         WindowValue(payload_descriptor, "101")}},
       {{WindowValue(text_descriptor, "B"), WindowValue(part_descriptor, "1"),
         WindowValue(order_descriptor, "1"), WindowValue(tie_descriptor, "x"),
         WindowValue(payload_descriptor, "102")}},
       {{WindowNull(text_descriptor), WindowValue(part_descriptor, "2"),
         WindowValue(order_descriptor, "1"), WindowValue(tie_descriptor, "a"),
         WindowValue(payload_descriptor, "103")}},
       {{WindowNull(text_descriptor), WindowValue(part_descriptor, "2"),
         WindowValue(order_descriptor, "1"), WindowValue(tie_descriptor, "A"),
         WindowValue(payload_descriptor, "104")}},
       {{WindowValue(text_descriptor, "A"), WindowValue(part_descriptor, "1"),
         WindowValue(order_descriptor, "1"), WindowValue(tie_descriptor, "Z"),
         WindowValue(payload_descriptor, "105")}},
       {{WindowValue(text_descriptor, "A"), WindowValue(part_descriptor, "2"),
         WindowValue(order_descriptor, "0"), WindowValue(tie_descriptor, "q"),
         WindowValue(payload_descriptor, "106")}},
       {{WindowValue(text_descriptor, "A"), WindowValue(part_descriptor, "1"),
         WindowNull(order_descriptor), WindowValue(tie_descriptor, "a"),
         WindowValue(payload_descriptor, "107")}},
       {{WindowValue(text_descriptor, "A"), WindowValue(part_descriptor, "1"),
         WindowValue(order_descriptor, "1"), WindowValue(tie_descriptor, "a"),
         WindowValue(payload_descriptor, "108")}}});

  exec::CanonicalWindowPartitionTerm text_partition;
  text_partition.column = 0;
  text_partition.expression_descriptor_id = 4001;
  text_partition.collation_uuid = kWindowCollationUuid;
  text_partition.resource_epoch = 401;
  text_partition.collation_epoch = 402;
  text_partition.text_seed = WindowCollationSeed();
  exec::CanonicalWindowPartitionTerm int_partition;
  int_partition.column = 1;
  int_partition.expression_descriptor_id = 4002;
  request.partition_terms = {text_partition, int_partition};

  exec::CanonicalDescriptorOrderTerm integer_order;
  integer_order.column = 2;
  integer_order.expression_descriptor_id = 4003;
  integer_order.direction = exec::CanonicalDescriptorOrderDirection::ascending;
  integer_order.null_placement = exec::CanonicalDescriptorNullPlacement::last;
  request.order_terms = {
      integer_order,
      WindowTextOrderTerm(3, 4004,
                          exec::CanonicalDescriptorOrderDirection::descending,
                          exec::CanonicalDescriptorNullPlacement::first)};
  request.window_property_uuid = window_property_uuid;
  request.partition_property_uuid = partition_property_uuid;
  request.ordering_property_uuid = ordering_property_uuid;
  request.inventory_local_transaction_id = dag.local_transaction_id;
  request.inventory_statement_snapshot_id = dag.statement_snapshot_id;
  return request;
}

std::vector<std::string> WindowPayloads(const exec::DescriptorBatch& batch) {
  std::vector<std::string> values;
  for (const auto& row : batch.rows) {
    values.push_back(row.values[4].encoded_value);
  }
  return values;
}

bool ValidateTypedCompositePartitions() {
  const auto result =
      exec::ExecuteCanonicalWindowPartitionOrder(Window401Request());
  bool passed = true;
  if (!result.diagnostic.ok) {
    std::cerr << "QOW-TEST-WIN-401-V1: initial refusal "
              << result.diagnostic.diagnostic_code << ": "
              << result.diagnostic.detail << '\n';
  }
  passed &= Require401(
      result.diagnostic.ok && result.partition_count == 4 &&
          result.peer_group_count == 7 && result.explicit_peer_metadata &&
          result.weaker_peer_recomputation_forbidden &&
          !result.final_query_order_guaranteed &&
          result.authority.engine_mga_snapshot_bound,
      "typed partition/order/peer construction was not accepted");
  passed &= Require401(
      WindowPayloads(result.ordered_batch) ==
          std::vector<std::string>({"101", "105", "108", "100", "107",
                                    "102", "103", "104", "106"}),
      "composite partition or typed order produced the wrong row sequence");
  passed &= Require401(
      result.row_metadata.size() == 9 &&
          result.row_metadata[0].partition_id.has_value() &&
          *result.row_metadata[0].partition_id == 0 &&
          result.row_metadata[0].partition_begin == 0 &&
          result.row_metadata[0].partition_end_exclusive == 5 &&
          result.row_metadata[5].partition_id == 1 &&
          result.row_metadata[6].partition_id == 2 &&
          result.row_metadata[8].partition_id == 3,
      "composite partition ranges lost explicit identity");

  auto request = Window401Request();
  request.partition_terms.pop_back();
  auto mutated = exec::ExecuteCanonicalWindowPartitionOrder(request);
  passed &= Require401(
      mutated.diagnostic.ok && mutated.partition_count == 3,
      "second partition term was ignored instead of changing partitioning");

  request = Window401Request();
  request.partition_terms[0].expression_descriptor_id = 4002;
  mutated = exec::ExecuteCanonicalWindowPartitionOrder(request);
  passed &= Require401(
      !mutated.diagnostic.ok &&
          mutated.diagnostic.diagnostic_code == "QOW-DIAG-WINDOW-PARTITION" &&
          mutated.ordered_batch.rows.empty(),
      "unresolved partition descriptor produced partial output");

  request = Window401Request();
  request.parser_execution_authority_claimed = true;
  mutated = exec::ExecuteCanonicalWindowPartitionOrder(request);
  passed &= Require401(
      !mutated.diagnostic.ok &&
          mutated.diagnostic.diagnostic_code == "QOW-DIAG-WINDOW-PEER" &&
          mutated.ordered_batch.rows.empty(),
      "parser authority claim entered the window runtime");
  return passed;
}

}  // namespace

#ifndef QOW_WIN_004_FIXTURE_ONLY
// QOW-TEST-WIN-004-V1
int main() {
  return ValidateTypedCompositePartitions() ? EXIT_SUCCESS : EXIT_FAILURE;
}
#endif
