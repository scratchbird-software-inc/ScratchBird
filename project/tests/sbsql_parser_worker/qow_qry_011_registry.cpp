// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "descriptor_value_runtime.hpp"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace exec = scratchbird::engine::executor;
namespace api = scratchbird::engine::internal_api;

namespace {

bool Require(const bool condition, const std::string_view detail) {
  if (!condition) {
    std::cerr << "QOW-TEST-QRY-011-REGISTRY-V1: " << detail << '\n';
  }
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

const exec::CanonicalAggregateRegistryEntry& Entry(
    const exec::CanonicalAggregateFunction function) {
  static const auto registry = exec::CanonicalAggregateRuntimeRegistryV1();
  for (const auto& entry : registry) {
    if (entry.function == function) return entry;
  }
  std::abort();
}

exec::CanonicalAggregateRuntimeRequest Request(
    const exec::CanonicalAggregateFunction function,
    const std::size_t value_column,
    const std::uint32_t value_descriptor_id,
    const std::string& result_type,
    const bool count_star = false) {
  const auto int_descriptor = Descriptor(
      "019f0000-0000-7200-8000-000000002101",
      "019f0000-0000-7300-8000-000000002101", "int64");
  const auto real_descriptor = Descriptor(
      "019f0000-0000-7200-8000-000000002102",
      "019f0000-0000-7300-8000-000000002102", "real64");
  const auto bool_descriptor = Descriptor(
      "019f0000-0000-7200-8000-000000002103",
      "019f0000-0000-7300-8000-000000002103", "boolean");
  const auto key_descriptor = Descriptor(
      "019f0000-0000-7200-8000-000000002104",
      "019f0000-0000-7300-8000-000000002104", "int64");
  const auto result_descriptor = Descriptor(
      "019f0000-0000-7200-8000-000000002199",
      "019f0000-0000-7300-8000-000000002199", result_type);

  exec::CanonicalAggregateRuntimeRequest request;
  request.physical_dag.selected_plan_uuid =
      "019f0000-0000-7200-8000-000000002110";
  request.physical_dag.root_physical_node_id = 2102;
  request.physical_dag.local_transaction_id = 2103;
  request.physical_dag.statement_snapshot_id = 2104;
  request.physical_dag.admission_evidence = {
      {exec::PhysicalAdmissionStage::kBoundRequest,
       "019f0000-0000-7200-8000-000000002111"},
      {exec::PhysicalAdmissionStage::kCatalogEpoch,
       "019f0000-0000-7200-8000-000000002112"},
      {exec::PhysicalAdmissionStage::kSecurity,
       "019f0000-0000-7200-8000-000000002113"},
      {exec::PhysicalAdmissionStage::kMgaStatementBoundary,
       "019f0000-0000-7200-8000-000000002114"},
      {exec::PhysicalAdmissionStage::kPolicyCapability,
       "019f0000-0000-7200-8000-000000002115"},
      {exec::PhysicalAdmissionStage::kResource,
       "019f0000-0000-7200-8000-000000002116"},
      {exec::PhysicalAdmissionStage::kStatisticsProvenance,
       "019f0000-0000-7200-8000-000000002117"},
      {exec::PhysicalAdmissionStage::kCanonicalRoute,
       "019f0000-0000-7200-8000-000000002118"},
  };
  request.physical_dag.nodes = {
      {.physical_node_id = 2101,
       .relational_node_id = 2101,
       .node_kind = exec::PhysicalNodeKind::kValues,
       .implementation_id = "values.typed.v1",
       .output_descriptor_ids = {2101, 2102, 2103, 2104},
       .causal_counter_id = 21001},
      {.physical_node_id = 2102,
       .relational_node_id = 2102,
       .node_kind = exec::PhysicalNodeKind::kAggregate,
       .implementation_id = "aggregate.registry-core.v1",
       .input_physical_node_ids = {2101},
       .output_descriptor_ids = {2199},
       .causal_counter_id = 21002},
  };
  request.selected_physical_node_id = 2102;
  request.input_batch = exec::MakeDescriptorBatch(
      {{"amount", int_descriptor, true, 2101},
       {"measure", real_descriptor, true, 2102},
       {"flag", bool_descriptor, true, 2103},
       {"key", key_descriptor, true, 2104}},
      {{{Value(int_descriptor, "1"), Value(real_descriptor, "1.5"),
         Value(bool_descriptor, "true"), Value(key_descriptor, "3")}},
       {{Value(int_descriptor, "2"), Value(real_descriptor, "2.5"),
         Value(bool_descriptor, "false"), Value(key_descriptor, "1")}},
       {{Value(int_descriptor, "2"), Null(real_descriptor),
         Null(bool_descriptor), Null(key_descriptor)}},
       {{Null(int_descriptor), Value(real_descriptor, "4"),
         Value(bool_descriptor, "true"), Value(key_descriptor, "2")}}});
  const auto& registry_entry = Entry(function);
  request.descriptor = {registry_entry.abi_version, registry_entry.function,
                        registry_entry.builtin_id,
                        registry_entry.function_uuid, count_star};
  if (!count_star) {
    request.value_columns = {value_column};
    request.value_expression_descriptor_ids = {value_descriptor_id};
  }
  request.result_column = {"aggregate_result", result_descriptor,
                           function != exec::CanonicalAggregateFunction::count,
                           2199};
  return request;
}

bool SameScalar(const exec::CanonicalAggregateRuntimeResult& left,
                const exec::CanonicalAggregateRuntimeResult& right) {
  if (!left.diagnostic.ok || !right.diagnostic.ok ||
      left.output_batch.rows.size() != 1 ||
      right.output_batch.rows.size() != 1) {
    return false;
  }
  const auto& lhs = left.output_batch.rows[0].values[0];
  const auto& rhs = right.output_batch.rows[0].values[0];
  return lhs.state == rhs.state && lhs.is_null == rhs.is_null &&
         lhs.encoded_value == rhs.encoded_value &&
         lhs.binary_value == rhs.binary_value;
}

// QOW-TEST-QRY-011-REGISTRY-V1
bool ValidateCanonicalAggregateRegistry() {
  bool passed = true;
  const auto registry = exec::CanonicalAggregateRuntimeRegistryV1();
  std::set<std::string> ids;
  std::set<std::string> uuids;
  std::set<exec::CanonicalAggregateFunction> functions;
  std::size_t executable_count = 0;
  std::size_t aggregate_window_count = 0;
  for (const auto& entry : registry) {
    passed &= Require(entry.abi_version == 1 &&
                          entry.function !=
                              exec::CanonicalAggregateFunction::unknown &&
                          ids.insert(entry.builtin_id).second &&
                          uuids.insert(entry.function_uuid).second &&
                          functions.insert(entry.function).second,
                      "registry row is incomplete or duplicated");
    if (entry.executable) ++executable_count;
    if (entry.aggregate_as_window) ++aggregate_window_count;
  }
  passed &= Require(registry.size() == 43 && executable_count == 8 &&
                        aggregate_window_count == 1 &&
                        Entry(exec::CanonicalAggregateFunction::sum)
                            .aggregate_as_window,
                    "registry did not reconcile exact row, executable, and aggregate-window dispositions");
  passed &= Require(Entry(exec::CanonicalAggregateFunction::count).builtin_id ==
                            "sb.aggregate.count" &&
                        Entry(exec::CanonicalAggregateFunction::regr_syy)
                                .function_uuid ==
                            "019dffbb-f000-74f7-98ba-c24ead6d30df",
                    "registry endpoints do not match seed authority");

  struct Case {
    exec::CanonicalAggregateFunction function;
    std::size_t column;
    std::uint32_t descriptor_id;
    const char* result_type;
    const char* expected;
    bool count_star;
  };
  const std::vector<Case> cases = {
      {exec::CanonicalAggregateFunction::count, 0, 0, "int64", "4", true},
      {exec::CanonicalAggregateFunction::count, 0, 2101, "int64", "3", false},
      {exec::CanonicalAggregateFunction::sum, 0, 2101, "int64", "5", false},
      {exec::CanonicalAggregateFunction::min, 3, 2104, "int64", "1", false},
      {exec::CanonicalAggregateFunction::max, 3, 2104, "int64", "3", false},
      {exec::CanonicalAggregateFunction::bool_and, 2, 2103, "boolean", "false", false},
      {exec::CanonicalAggregateFunction::bool_or, 2, 2103, "boolean", "true", false},
      {exec::CanonicalAggregateFunction::every, 2, 2103, "boolean", "false", false},
  };
  for (const auto& test_case : cases) {
    auto request = Request(test_case.function, test_case.column,
                           test_case.descriptor_id, test_case.result_type,
                           test_case.count_star);
    const auto serial = exec::ExecuteCanonicalAggregateRuntime(request);
    request.forced_strategy =
        exec::CanonicalAggregateExecutionStrategy::partitioned_combine;
    const auto partitioned = exec::ExecuteCanonicalAggregateRuntime(request);
    passed &= Require(serial.diagnostic.ok &&
                          serial.output_batch.rows[0].values[0].encoded_value ==
                              test_case.expected &&
                          serial.every_descriptor_field_consumed &&
                          serial.shared_state_authority_used &&
                          serial.authority.engine_mga_snapshot_bound &&
                          !serial.authority.owns_transaction_finality &&
                          SameScalar(serial, partitioned),
                      "core aggregate result or forced-strategy parity failed");
  }

  auto avg_request = Request(exec::CanonicalAggregateFunction::avg, 1, 2102,
                             "real64");
  auto avg = exec::ExecuteCanonicalAggregateRuntime(avg_request);
  passed &= Require(avg.diagnostic.ok &&
                        std::abs(std::stod(
                                     avg.output_batch.rows[0].values[0]
                                         .encoded_value) -
                                 (8.0 / 3.0)) < 1e-12,
                    "AVG did not preserve numeric state semantics");

  auto sum_request = Request(exec::CanonicalAggregateFunction::sum, 0, 2101,
                             "int64");
  sum_request.distinct = true;
  sum_request.filter_truth_values =
      std::vector<api::EngineSqlTruthValue>{
          api::EngineSqlTruthValue::true_value,
          api::EngineSqlTruthValue::true_value,
          api::EngineSqlTruthValue::true_value,
          api::EngineSqlTruthValue::unknown};
  auto sum = exec::ExecuteCanonicalAggregateRuntime(sum_request);
  passed &= Require(sum.diagnostic.ok && sum.filter_applied_before_distinct &&
                        sum.distinct_tuple_count == 2 &&
                        sum.transition_count == 2 &&
                        sum.output_batch.rows[0].values[0].encoded_value == "3",
                    "FILTER then DISTINCT did not feed the shared state exactly once");

  auto empty_sum = Request(exec::CanonicalAggregateFunction::sum, 0, 2101,
                           "int64");
  empty_sum.input_batch.rows.clear();
  auto empty = exec::ExecuteCanonicalAggregateRuntime(empty_sum);
  passed &= Require(empty.diagnostic.ok &&
                        empty.output_batch.rows[0].values[0].state ==
                            api::EngineValueState::sql_null,
                    "empty SUM did not produce typed SQL NULL");
  auto empty_count = Request(exec::CanonicalAggregateFunction::count, 0, 0,
                             "int64", true);
  empty_count.input_batch.rows.clear();
  empty = exec::ExecuteCanonicalAggregateRuntime(empty_count);
  passed &= Require(empty.diagnostic.ok &&
                        empty.output_batch.rows[0].values[0].encoded_value ==
                            "0",
                    "empty COUNT(*) did not produce zero");

  auto malformed = Request(exec::CanonicalAggregateFunction::sum, 0, 2101,
                           "int64");
  malformed.descriptor.function_uuid =
      Entry(exec::CanonicalAggregateFunction::avg).function_uuid;
  auto refusal = exec::ExecuteCanonicalAggregateRuntime(malformed);
  passed &= Require(!refusal.diagnostic.ok &&
                        refusal.diagnostic.diagnostic_code ==
                            "QOW-DIAG-QRY-011-REGISTRY-DESCRIPTOR-V1" &&
                        refusal.output_batch.rows.empty(),
                    "mismatched aggregate UUID substituted another function");

  auto unavailable = Request(exec::CanonicalAggregateFunction::sum, 0, 2101,
                             "int64");
  const auto& array_entry = Entry(exec::CanonicalAggregateFunction::array_agg);
  unavailable.descriptor = {array_entry.abi_version, array_entry.function,
                            array_entry.builtin_id, array_entry.function_uuid,
                            false};
  refusal = exec::ExecuteCanonicalAggregateRuntime(unavailable);
  passed &= Require(!refusal.diagnostic.ok &&
                        refusal.diagnostic.diagnostic_code ==
                            "QOW-DIAG-QRY-011-REGISTRY-UNIMPLEMENTED-V1",
                    "registry-only aggregate was reported executable");

  auto authority = Request(exec::CanonicalAggregateFunction::sum, 0, 2101,
                           "int64");
  authority.transaction_finality_claimed = true;
  refusal = exec::ExecuteCanonicalAggregateRuntime(authority);
  passed &= Require(!refusal.diagnostic.ok && refusal.output_batch.rows.empty(),
                    "aggregate runtime accepted transaction finality authority");
  return passed;
}

}  // namespace

int main() {
  return ValidateCanonicalAggregateRegistry() ? EXIT_SUCCESS : EXIT_FAILURE;
}
