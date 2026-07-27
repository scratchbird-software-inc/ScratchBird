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

namespace {

bool Require(const bool condition, const std::string_view detail) {
  if (!condition) {
    std::cerr << "QOW-TEST-QRY-012-STRATEGY-V1: " << detail << '\n';
  }
  return condition;
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

exec::CanonicalJoinStrategyRequest Request() {
  const auto left_key = Descriptor(
      "019f0000-0000-7200-8000-000000002301",
      "019f0000-0000-7300-8000-000000002302");
  const auto left_payload = Descriptor(
      "019f0000-0000-7200-8000-000000002303",
      "019f0000-0000-7300-8000-000000002304");
  const auto right_key = Descriptor(
      "019f0000-0000-7200-8000-000000002305",
      "019f0000-0000-7300-8000-000000002306");
  const auto right_payload = Descriptor(
      "019f0000-0000-7200-8000-000000002307",
      "019f0000-0000-7300-8000-000000002308");

  exec::CanonicalJoinStrategyRequest request;
  auto& residual = request.residual_request;
  auto& key = residual.key_request;
  key.physical_dag.selected_plan_uuid =
      "019f0000-0000-7200-8000-000000002309";
  key.physical_dag.root_physical_node_id = 2303;
  key.physical_dag.local_transaction_id = 2304;
  key.physical_dag.statement_snapshot_id = 2305;
  key.physical_dag.admission_evidence = {
      {exec::PhysicalAdmissionStage::kBoundRequest,
       "019f0000-0000-7200-8000-000000002311"},
      {exec::PhysicalAdmissionStage::kCatalogEpoch,
       "019f0000-0000-7200-8000-000000002312"},
      {exec::PhysicalAdmissionStage::kSecurity,
       "019f0000-0000-7200-8000-000000002313"},
      {exec::PhysicalAdmissionStage::kMgaStatementBoundary,
       "019f0000-0000-7200-8000-000000002314"},
      {exec::PhysicalAdmissionStage::kPolicyCapability,
       "019f0000-0000-7200-8000-000000002315"},
      {exec::PhysicalAdmissionStage::kResource,
       "019f0000-0000-7200-8000-000000002316"},
      {exec::PhysicalAdmissionStage::kStatisticsProvenance,
       "019f0000-0000-7200-8000-000000002317"},
      {exec::PhysicalAdmissionStage::kCanonicalRoute,
       "019f0000-0000-7200-8000-000000002318"},
  };
  key.physical_dag.nodes = {
      {.physical_node_id = 2301,
       .relational_node_id = 2301,
       .node_kind = exec::PhysicalNodeKind::kValues,
       .implementation_id = "values.left.typed.v1",
       .output_descriptor_ids = {2301, 2302},
       .causal_counter_id = 23001},
      {.physical_node_id = 2302,
       .relational_node_id = 2302,
       .node_kind = exec::PhysicalNodeKind::kValues,
       .implementation_id = "values.right.typed.v1",
       .output_descriptor_ids = {2303, 2304},
       .causal_counter_id = 23002},
      {.physical_node_id = 2303,
       .relational_node_id = 2303,
       .node_kind = exec::PhysicalNodeKind::kJoin,
       .implementation_id = "join.hash-inner.int64-equality.v1",
       .input_physical_node_ids = {2301, 2302},
       .output_descriptor_ids = {2301, 2302, 2303, 2304},
       .causal_counter_id = 23003},
  };
  key.selected_physical_node_id = 2303;
  key.left_batch = exec::MakeDescriptorBatch(
      {{"left_key", left_key, true, 2301},
       {"left_payload", left_payload, false, 2302}},
      {{{Value(left_key, "1"), Value(left_payload, "10")}},
       {{Value(left_key, "01"), Value(left_payload, "11")}},
       {{Value(left_key, "2"), Value(left_payload, "12")}},
       {{Null(left_key), Value(left_payload, "13")}}});
  key.right_batch = exec::MakeDescriptorBatch(
      {{"right_key", right_key, true, 2303},
       {"right_payload", right_payload, false, 2304}},
      {{{Value(right_key, "1"), Value(right_payload, "20")}},
       {{Value(right_key, "2"), Value(right_payload, "21")}},
       {{Value(right_key, "01"), Value(right_payload, "22")}},
       {{Null(right_key), Value(right_payload, "23")}}});
  key.key_terms = {
      {.left_column = 0,
       .left_expression_descriptor_id = 2301,
       .right_column = 0,
       .right_expression_descriptor_id = 2303},
  };
  using Truth = api::EngineSqlTruthValue;
  residual.residual_truth_values = {
      Truth::true_value, Truth::true_value, Truth::false_value, Truth::true_value,
      Truth::unknown, Truth::false_value, Truth::true_value, Truth::true_value,
      Truth::true_value, Truth::true_value, Truth::true_value, Truth::true_value,
      Truth::true_value, Truth::true_value, Truth::true_value, Truth::true_value,
  };
  return request;
}

void SelectStrategy(exec::CanonicalJoinStrategyRequest* request,
                    const exec::CanonicalJoinStrategyKind strategy,
                    const std::string& implementation_id) {
  request->strategy = strategy;
  request->residual_request.key_request.physical_dag.nodes.back()
      .implementation_id = implementation_id;
}

// QOW-TEST-QRY-012-STRATEGY-V1
bool ValidateJoinStrategy() {
  bool passed = true;
  auto result = exec::ExecuteCanonicalJoinStrategy(Request());
  const std::vector<std::size_t> expected_pairs = {0, 6, 9};
  passed &= Require(
      result.diagnostic.ok && result.canonical_multiset_proven &&
          result.canonical_pair_indices == expected_pairs &&
          result.strategy_pair_indices == expected_pairs &&
          result.hash_entry_count == 3 &&
          result.retained_entry_count == 3 &&
          result.candidate_probe_count == 5 &&
          result.output_batch.rows.size() == 3 &&
          result.strategy_id == "join.hash-inner.int64-equality.v1" &&
          result.executed_physical_node_id == 2303,
      "hash-inner strategy did not prove the canonical physical-pair multiset");

  auto request = Request();
  SelectStrategy(&request, exec::CanonicalJoinStrategyKind::kNestedLoopInner,
                 "join.nested-loop-inner.v1");
  result = exec::ExecuteCanonicalJoinStrategy(request);
  passed &= Require(
      result.diagnostic.ok && result.canonical_multiset_proven &&
          result.canonical_pair_indices == expected_pairs &&
          result.strategy_pair_indices == expected_pairs &&
          result.hash_entry_count == 0 && result.retained_entry_count == 0 &&
          result.candidate_probe_count == 5 &&
          result.output_batch.rows.size() == 3 &&
          result.strategy_id == "join.nested-loop-inner.v1",
      "nested-loop strategy did not prove the canonical physical-pair multiset");

  request = Request();
  SelectStrategy(&request,
                 exec::CanonicalJoinStrategyKind::kMergeInnerInt64Equality,
                 "join.merge-inner.int64-equality.v1");
  result = exec::ExecuteCanonicalJoinStrategy(request);
  passed &= Require(
      result.diagnostic.ok && result.canonical_multiset_proven &&
          result.canonical_pair_indices == expected_pairs &&
          result.strategy_pair_indices == expected_pairs &&
          result.hash_entry_count == 0 && result.retained_entry_count == 6 &&
          result.candidate_probe_count == 5 &&
          result.output_batch.rows.size() == 3 &&
          result.strategy_id == "join.merge-inner.int64-equality.v1",
      "merge strategy did not prove the canonical physical-pair multiset");

  request = Request();
  request.strategy = static_cast<exec::CanonicalJoinStrategyKind>(99);
  result = exec::ExecuteCanonicalJoinStrategy(request);
  passed &= Require(!result.diagnostic.ok && !result.canonical_multiset_proven &&
                        result.output_batch.rows.empty(),
                    "unaccepted join strategy was executed");

  request = Request();
  request.residual_request.key_request.physical_dag.nodes.back()
      .implementation_id = "join.nested-loop.v1";
  result = exec::ExecuteCanonicalJoinStrategy(request);
  passed &= Require(!result.diagnostic.ok,
                    "strategy identity drift was accepted");

  request = Request();
  request.residual_request.key_request.key_terms.push_back(
      request.residual_request.key_request.key_terms.front());
  result = exec::ExecuteCanonicalJoinStrategy(request);
  passed &= Require(!result.diagnostic.ok,
                    "composite key entered the one-key strategy profile");

  request = Request();
  SelectStrategy(&request, exec::CanonicalJoinStrategyKind::kNestedLoopInner,
                 "join.nested-loop-inner.v1");
  request.residual_request.key_request.key_terms.push_back(
      {.left_column = 1,
       .left_expression_descriptor_id = 2302,
       .right_column = 1,
       .right_expression_descriptor_id = 2304});
  result = exec::ExecuteCanonicalJoinStrategy(request);
  passed &= Require(result.diagnostic.ok && result.canonical_multiset_proven &&
                        result.strategy_pair_indices.empty() &&
                        result.candidate_probe_count == 0,
                    "nested-loop strategy rejected a valid composite key");

  request = Request();
  request.maximum_hash_entries = 2;
  result = exec::ExecuteCanonicalJoinStrategy(request);
  passed &= Require(!result.diagnostic.ok && result.hash_entry_count == 0,
                    "hash strategy entry bound was exceeded");

  request = Request();
  request.maximum_retained_entries = 2;
  result = exec::ExecuteCanonicalJoinStrategy(request);
  passed &= Require(!result.diagnostic.ok && result.retained_entry_count == 0,
                    "hash strategy retained-state bound was exceeded");

  request = Request();
  SelectStrategy(&request,
                 exec::CanonicalJoinStrategyKind::kMergeInnerInt64Equality,
                 "join.merge-inner.int64-equality.v1");
  request.maximum_retained_entries = 5;
  result = exec::ExecuteCanonicalJoinStrategy(request);
  passed &= Require(!result.diagnostic.ok && result.retained_entry_count == 0,
                    "merge strategy retained-state bound was exceeded");

  request = Request();
  request.maximum_candidate_probes = 4;
  result = exec::ExecuteCanonicalJoinStrategy(request);
  passed &= Require(!result.diagnostic.ok && result.candidate_probe_count == 0,
                    "hash strategy probe bound was exceeded");

  request = Request();
  SelectStrategy(&request, exec::CanonicalJoinStrategyKind::kNestedLoopInner,
                 "join.nested-loop-inner.v1");
  request.maximum_candidate_probes = 4;
  result = exec::ExecuteCanonicalJoinStrategy(request);
  passed &= Require(!result.diagnostic.ok && result.candidate_probe_count == 0,
                    "nested-loop strategy probe bound was exceeded");

  request = Request();
  SelectStrategy(&request,
                 exec::CanonicalJoinStrategyKind::kMergeInnerInt64Equality,
                 "join.merge-inner.int64-equality.v1");
  request.maximum_candidate_probes = 4;
  result = exec::ExecuteCanonicalJoinStrategy(request);
  passed &= Require(!result.diagnostic.ok && result.candidate_probe_count == 0,
                    "merge strategy probe bound was exceeded");

  request = Request();
  request.maximum_output_rows = 2;
  result = exec::ExecuteCanonicalJoinStrategy(request);
  passed &= Require(!result.diagnostic.ok && result.output_batch.rows.empty(),
                    "hash strategy output bound was exceeded");

  request = Request();
  SelectStrategy(&request, exec::CanonicalJoinStrategyKind::kNestedLoopInner,
                 "join.nested-loop-inner.v1");
  request.maximum_output_rows = 2;
  result = exec::ExecuteCanonicalJoinStrategy(request);
  passed &= Require(!result.diagnostic.ok && result.output_batch.rows.empty(),
                    "nested-loop strategy output bound was exceeded");

  request = Request();
  SelectStrategy(&request,
                 exec::CanonicalJoinStrategyKind::kMergeInnerInt64Equality,
                 "join.merge-inner.int64-equality.v1");
  request.maximum_output_rows = 2;
  result = exec::ExecuteCanonicalJoinStrategy(request);
  passed &= Require(!result.diagnostic.ok && result.output_batch.rows.empty(),
                    "merge strategy output bound was exceeded");

  request = Request();
  request.residual_request.key_request.left_batch.rows[0]
      .values[0].encoded_value = "malformed";
  result = exec::ExecuteCanonicalJoinStrategy(request);
  passed &= Require(!result.diagnostic.ok,
                    "malformed key reached hash strategy output");

  request = Request();
  request.residual_request.key_request.physical_dag.statement_snapshot_id = 0;
  result = exec::ExecuteCanonicalJoinStrategy(request);
  passed &= Require(!result.diagnostic.ok,
                    "hash strategy bypassed MGA physical admission");

  request = Request();
  request.residual_request.key_request.left_batch.rows.clear();
  request.residual_request.residual_truth_values.clear();
  result = exec::ExecuteCanonicalJoinStrategy(request);
  passed &= Require(result.diagnostic.ok && result.canonical_multiset_proven &&
                        result.output_batch.rows.empty() &&
                        result.candidate_probe_count == 0,
                    "empty join side invented hash matches");
  return passed;
}

}  // namespace

int main() {
  return ValidateJoinStrategy() ? EXIT_SUCCESS : EXIT_FAILURE;
}
