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

exec::CanonicalAggregateRuntimeRequest StatisticalRequest(
    const exec::CanonicalAggregateFunction function) {
  auto request = Request(function, 0, 2101, "real64");
  const auto descriptor = request.input_batch.columns[0].descriptor;
  request.input_batch.rows[0].values[0] = Value(descriptor, "1");
  request.input_batch.rows[1].values[0] = Value(descriptor, "2");
  request.input_batch.rows[2].values[0] = Value(descriptor, "3");
  request.input_batch.rows[3].values[0] = Null(descriptor);
  return request;
}

exec::CanonicalAggregateRuntimeRequest PairStatisticalRequest(
    const exec::CanonicalAggregateFunction function) {
  auto request = Request(function, 0, 2101,
                         function ==
                                 exec::CanonicalAggregateFunction::regr_count
                             ? "int64"
                             : "real64");
  request.value_columns = {0, 3};
  request.value_expression_descriptor_ids = {2101, 2104};
  request.result_column.nullable =
      function != exec::CanonicalAggregateFunction::regr_count;
  const auto y_descriptor = request.input_batch.columns[0].descriptor;
  const auto x_descriptor = request.input_batch.columns[3].descriptor;
  request.input_batch.rows[0].values[0] = Value(y_descriptor, "2");
  request.input_batch.rows[0].values[3] = Value(x_descriptor, "1");
  request.input_batch.rows[1].values[0] = Value(y_descriptor, "4");
  request.input_batch.rows[1].values[3] = Value(x_descriptor, "2");
  request.input_batch.rows[2].values[0] = Value(y_descriptor, "6");
  request.input_batch.rows[2].values[3] = Value(x_descriptor, "3");
  request.input_batch.rows[3].values[0] = Null(y_descriptor);
  request.input_batch.rows[3].values[3] = Value(x_descriptor, "4");
  return request;
}

exec::CanonicalAggregateRuntimeRequest CollectionRequest(
    const exec::CanonicalAggregateFunction function) {
  const auto result_type =
      function == exec::CanonicalAggregateFunction::array_agg
          ? "list<text nullable>"
          : (function == exec::CanonicalAggregateFunction::string_agg ||
                     function == exec::CanonicalAggregateFunction::listagg
                 ? "text"
                 : "json");
  auto request = Request(function, 0, 2101, result_type);
  const auto text_descriptor = Descriptor(
      "019f0000-0000-7200-8000-000000002101",
      "019f0000-0000-7300-8000-000000002105", "text");
  request.input_batch.columns[0].descriptor = text_descriptor;
  request.input_batch.rows[0].values[0] = Value(text_descriptor, "a");
  request.input_batch.rows[1].values[0] = Value(text_descriptor, "b");
  request.input_batch.rows[2].values[0] = Null(text_descriptor);
  request.input_batch.rows[3].values[0] = Value(text_descriptor, "d");
  request.aggregate_order_terms = {{.column = 3,
                                    .expression_descriptor_id = 2104}};
  request.aggregate_separator = "|";
  if (function == exec::CanonicalAggregateFunction::json_object_agg) {
    request.input_batch.rows[0].values[0] = Value(text_descriptor, "dup");
    request.input_batch.rows[1].values[0] = Value(text_descriptor, "other");
    request.input_batch.rows[2].values[0] = Value(text_descriptor, "dup");
    request.input_batch.rows[3].values[0] = Value(text_descriptor, "tail");
    request.value_columns = {0, 1};
    request.value_expression_descriptor_ids = {2101, 2102};
  }
  return request;
}

bool NearScalar(const exec::CanonicalAggregateRuntimeResult& result,
                const double expected) {
  return result.diagnostic.ok && result.output_batch.rows.size() == 1 &&
         result.output_batch.rows[0].values[0].state ==
             api::EngineValueState::value &&
         std::abs(std::stod(
                      result.output_batch.rows[0].values[0].encoded_value) -
                  expected) < 1e-12;
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
  passed &= Require(registry.size() == 43 && executable_count == 31 &&
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

  struct StatisticalCase {
    exec::CanonicalAggregateFunction function;
    double expected;
  };
  const std::vector<StatisticalCase> univariate_cases = {
      {exec::CanonicalAggregateFunction::variance_pop, 2.0 / 3.0},
      {exec::CanonicalAggregateFunction::stddev_pop,
       std::sqrt(2.0 / 3.0)},
      {exec::CanonicalAggregateFunction::variance, 1.0},
      {exec::CanonicalAggregateFunction::variance_samp, 1.0},
      {exec::CanonicalAggregateFunction::stddev, 1.0},
      {exec::CanonicalAggregateFunction::stddev_samp, 1.0},
  };
  for (const auto& test_case : univariate_cases) {
    auto request = StatisticalRequest(test_case.function);
    const auto serial = exec::ExecuteCanonicalAggregateRuntime(request);
    request.forced_strategy =
        exec::CanonicalAggregateExecutionStrategy::partitioned_combine;
    const auto partitioned = exec::ExecuteCanonicalAggregateRuntime(request);
    passed &= Require(NearScalar(serial, test_case.expected) &&
                          SameScalar(serial, partitioned) &&
                          serial.non_null_transition_count == 3,
                      "univariate statistical state or merge parity failed");
  }

  const std::vector<StatisticalCase> pair_cases = {
      {exec::CanonicalAggregateFunction::corr, 1.0},
      {exec::CanonicalAggregateFunction::covar_pop, 4.0 / 3.0},
      {exec::CanonicalAggregateFunction::covar_samp, 2.0},
      {exec::CanonicalAggregateFunction::regr_count, 3.0},
      {exec::CanonicalAggregateFunction::regr_avgx, 2.0},
      {exec::CanonicalAggregateFunction::regr_avgy, 4.0},
      {exec::CanonicalAggregateFunction::regr_intercept, 0.0},
      {exec::CanonicalAggregateFunction::regr_r2, 1.0},
      {exec::CanonicalAggregateFunction::regr_slope, 2.0},
      {exec::CanonicalAggregateFunction::regr_sxx, 2.0},
      {exec::CanonicalAggregateFunction::regr_sxy, 4.0},
      {exec::CanonicalAggregateFunction::regr_syy, 8.0},
  };
  for (const auto& test_case : pair_cases) {
    auto request = PairStatisticalRequest(test_case.function);
    const auto serial = exec::ExecuteCanonicalAggregateRuntime(request);
    request.forced_strategy =
        exec::CanonicalAggregateExecutionStrategy::partitioned_combine;
    const auto partitioned = exec::ExecuteCanonicalAggregateRuntime(request);
    passed &= Require(NearScalar(serial, test_case.expected) &&
                          SameScalar(serial, partitioned) &&
                          serial.transition_count == 4 &&
                          serial.non_null_transition_count == 3,
                      "pair statistical state or merge parity failed");
  }

  auto singleton = StatisticalRequest(
      exec::CanonicalAggregateFunction::stddev_samp);
  singleton.input_batch.rows.resize(1);
  auto boundary = exec::ExecuteCanonicalAggregateRuntime(singleton);
  passed &= Require(boundary.diagnostic.ok &&
                        boundary.output_batch.rows[0].values[0].state ==
                            api::EngineValueState::sql_null,
                    "sample standard deviation singleton was not NULL");
  singleton = StatisticalRequest(
      exec::CanonicalAggregateFunction::variance_pop);
  singleton.input_batch.rows.resize(1);
  boundary = exec::ExecuteCanonicalAggregateRuntime(singleton);
  passed &= Require(NearScalar(boundary, 0.0),
                    "population variance singleton was not zero");

  auto pair_singleton =
      PairStatisticalRequest(exec::CanonicalAggregateFunction::corr);
  pair_singleton.input_batch.rows.resize(1);
  boundary = exec::ExecuteCanonicalAggregateRuntime(pair_singleton);
  passed &= Require(boundary.diagnostic.ok &&
                        boundary.output_batch.rows[0].values[0].state ==
                            api::EngineValueState::sql_null,
                    "correlation singleton was not NULL");
  pair_singleton =
      PairStatisticalRequest(exec::CanonicalAggregateFunction::covar_pop);
  pair_singleton.input_batch.rows.resize(1);
  boundary = exec::ExecuteCanonicalAggregateRuntime(pair_singleton);
  passed &= Require(NearScalar(boundary, 0.0),
                    "population covariance singleton was not zero");

  auto constant_y =
      PairStatisticalRequest(exec::CanonicalAggregateFunction::regr_r2);
  const auto y_descriptor = constant_y.input_batch.columns[0].descriptor;
  for (std::size_t row = 0; row < 3; ++row) {
    constant_y.input_batch.rows[row].values[0] =
        Value(y_descriptor, "5");
  }
  boundary = exec::ExecuteCanonicalAggregateRuntime(constant_y);
  passed &= Require(NearScalar(boundary, 1.0),
                    "REGR_R2 constant-dependent-variable rule drifted");

  auto constant_x =
      PairStatisticalRequest(exec::CanonicalAggregateFunction::regr_slope);
  const auto x_descriptor = constant_x.input_batch.columns[3].descriptor;
  for (std::size_t row = 0; row < 3; ++row) {
    constant_x.input_batch.rows[row].values[3] =
        Value(x_descriptor, "1");
  }
  boundary = exec::ExecuteCanonicalAggregateRuntime(constant_x);
  passed &= Require(boundary.diagnostic.ok &&
                        boundary.output_batch.rows[0].values[0].state ==
                            api::EngineValueState::sql_null,
                    "REGR_SLOPE constant-independent-variable rule drifted");

  auto bad_pair =
      PairStatisticalRequest(exec::CanonicalAggregateFunction::corr);
  bad_pair.value_columns.pop_back();
  bad_pair.value_expression_descriptor_ids.pop_back();
  auto statistical_refusal =
      exec::ExecuteCanonicalAggregateRuntime(bad_pair);
  passed &= Require(!statistical_refusal.diagnostic.ok &&
                        statistical_refusal.diagnostic.diagnostic_code ==
                            "QOW-DIAG-QRY-011-REGISTRY-ARITY-V1",
                    "pair statistical aggregate accepted one input");

  struct CollectionCase {
    exec::CanonicalAggregateFunction function;
    const char* expected;
  };
  const std::vector<CollectionCase> collection_cases = {
      {exec::CanonicalAggregateFunction::array_agg,
       "list[text:b;text:d;text:a;NULL]"},
      {exec::CanonicalAggregateFunction::string_agg, "b|d|a"},
      {exec::CanonicalAggregateFunction::json_agg,
       R"(["b","d","a",null])"},
      {exec::CanonicalAggregateFunction::json_object_agg,
       R"({"other":2.5,"tail":4,"dup":null})"},
      {exec::CanonicalAggregateFunction::listagg, "b|d|a"},
  };
  for (const auto& test_case : collection_cases) {
    auto request = CollectionRequest(test_case.function);
    const auto serial = exec::ExecuteCanonicalAggregateRuntime(request);
    request.forced_strategy =
        exec::CanonicalAggregateExecutionStrategy::partitioned_combine;
    const auto partitioned = exec::ExecuteCanonicalAggregateRuntime(request);
    passed &= Require(serial.diagnostic.ok &&
                          serial.output_batch.rows[0].values[0].encoded_value ==
                              test_case.expected &&
                          serial.aggregate_order_applied &&
                          serial.order_comparison_count != 0 &&
                          serial.state_bytes != 0 &&
                          SameScalar(serial, partitioned),
                      "ordered collection state or merge parity failed");
  }

  auto listagg = CollectionRequest(
      exec::CanonicalAggregateFunction::listagg);
  const auto text_descriptor = listagg.input_batch.columns[0].descriptor;
  listagg.input_batch.rows[0].values[0] = Value(text_descriptor, "north");
  listagg.input_batch.rows[1].values[0] = Value(text_descriptor, "east");
  listagg.input_batch.rows[2].values[0] = Value(text_descriptor, "south");
  listagg.input_batch.rows[3].values[0] = Null(text_descriptor);
  listagg.listagg_overflow_mode =
      exec::CanonicalListaggOverflowMode::truncate;
  listagg.listagg_max_output_bytes = 12;
  auto collection = exec::ExecuteCanonicalAggregateRuntime(listagg);
  passed &= Require(collection.diagnostic.ok &&
                        collection.output_batch.rows[0].values[0]
                                .encoded_value == "east|...(2)",
                    "LISTAGG truncation did not preserve ordered boundaries");

  listagg.listagg_overflow_mode = exec::CanonicalListaggOverflowMode::error;
  collection = exec::ExecuteCanonicalAggregateRuntime(listagg);
  passed &= Require(!collection.diagnostic.ok &&
                        collection.diagnostic.diagnostic_code ==
                            "QOW-DIAG-QRY-011-REGISTRY-LISTAGG-OVERFLOW-V1",
                    "LISTAGG overflow error did not fail atomically");

  auto missing_order = CollectionRequest(
      exec::CanonicalAggregateFunction::array_agg);
  missing_order.aggregate_order_terms.clear();
  collection = exec::ExecuteCanonicalAggregateRuntime(missing_order);
  passed &= Require(!collection.diagnostic.ok &&
                        collection.diagnostic.diagnostic_code ==
                            "QOW-DIAG-QRY-011-REGISTRY-ORDER-V1",
                    "ordered collection aggregate accepted missing order");

  auto bounded_collection = CollectionRequest(
      exec::CanonicalAggregateFunction::json_agg);
  bounded_collection.maximum_state_bytes = 1;
  collection = exec::ExecuteCanonicalAggregateRuntime(bounded_collection);
  passed &= Require(!collection.diagnostic.ok &&
                        collection.diagnostic.diagnostic_code ==
                            "SBLR.PLAN_TREE.RESOURCE_LIMIT" &&
                        collection.output_batch.rows.empty(),
                    "collection state exceeded its byte bound");

  auto null_key = CollectionRequest(
      exec::CanonicalAggregateFunction::json_object_agg);
  null_key.input_batch.rows[0].values[0] =
      Null(null_key.input_batch.columns[0].descriptor);
  collection = exec::ExecuteCanonicalAggregateRuntime(null_key);
  passed &= Require(!collection.diagnostic.ok &&
                        collection.diagnostic.diagnostic_code ==
                            "QOW-DIAG-QRY-011-REGISTRY-JSON-KEY-V1",
                    "JSON object aggregate accepted a NULL key");

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
  const auto& rank_entry = Entry(exec::CanonicalAggregateFunction::rank);
  unavailable.descriptor = {rank_entry.abi_version, rank_entry.function,
                            rank_entry.builtin_id, rank_entry.function_uuid,
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
