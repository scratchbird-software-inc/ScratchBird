// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#define QOW_QRY_011_REGISTRY_FIXTURE_ONLY
#include "../sbsql_parser_worker/qow_qry_011_registry.cpp"
#include "../support/exact_datatype_descriptor_fixture.hpp"
#include "aggregate_executor_internal.hpp"
#include <bit>
#include <cstdlib>
#include <new>
#include <stdexcept>

namespace { long fail_after = -1; }
void* operator new(std::size_t n) {
  if (fail_after == 0) { fail_after = -1; throw std::bad_alloc(); }
  if (fail_after > 0) --fail_after;
  if (auto* p = std::malloc(n ? n : 1)) return p;
  throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

unsigned checks = 0;
void Check(bool ok, const char* message) {
  ++checks;
  if (!ok) throw std::runtime_error(message);
}
api::EngineTypedValue Native(const api::EngineDescriptor& descriptor, std::int64_t n) {
  api::EngineTypedValue value;
  value.descriptor = descriptor;
  for (unsigned i = 0; i < 8; ++i)
    value.binary_value.push_back(static_cast<std::uint8_t>(static_cast<std::uint64_t>(n) >> (8*i)));
  return value;
}
void BindColumn(exec::ExecutorColumnDescriptor& column) {
  column.descriptor = scratchbird::tests::ExactScalarDescriptorFixture(
      dt::CanonicalTypeIdFromStableName(column.descriptor.canonical_type_name),
      column.descriptor.canonical_type_name, column.descriptor.descriptor_uuid,
      column.nullable ? "nullability=nullable" : "nullability=non_null");
}
exec::CanonicalAggregateRuntimeRequest NativeRequest(exec::CanonicalAggregateFunction function) {
  auto request = Request(function, 0, 2101, "int64", function == exec::CanonicalAggregateFunction::count);
  request.result_column.nullable = function == exec::CanonicalAggregateFunction::sum ||
      function == exec::CanonicalAggregateFunction::min ||
      function == exec::CanonicalAggregateFunction::max ||
      function == exec::CanonicalAggregateFunction::mode;
  BindColumn(request.result_column);
  for (std::size_t c = 0; c < request.input_batch.columns.size(); ++c) {
    auto& column = request.input_batch.columns[c];
    BindColumn(column);
    for (auto& row : request.input_batch.rows) {
      auto& value = row.values[c];
      value.descriptor = column.descriptor;
      if (!value.is_null && column.descriptor.canonical_type_name == "int64" && value.binary_value.empty())
        value = Native(column.descriptor, std::stoll(value.encoded_value));
    }
  }
  if (function == exec::CanonicalAggregateFunction::regr_count) {
    request.value_columns = {0, 1};
    request.value_expression_descriptor_ids = {2101, 2102};
  }
  if (function == exec::CanonicalAggregateFunction::rank || function == exec::CanonicalAggregateFunction::dense_rank) {
    request.direct_arguments = {Native(request.input_batch.columns[0].descriptor, 2)};
    request.aggregate_order_terms = {{.column=0, .expression_descriptor_id=2101}};
  }
  if (function == exec::CanonicalAggregateFunction::mode)
    request.aggregate_order_terms = {{.column=0, .expression_descriptor_id=2101}};
  if (function == exec::CanonicalAggregateFunction::approx_count_distinct ||
      function == exec::CanonicalAggregateFunction::mode)
    BindEqualityAuthority(&request);
  return request;
}
void CheckResult(const exec::CanonicalAggregateRuntimeRequest& request,
                 const exec::CanonicalAggregateRuntimeResult& result,
                 std::optional<std::int64_t> expected) {
  if (!result.diagnostic.ok) std::cerr << result.diagnostic.diagnostic_code << ':' << result.diagnostic.detail << '\n';
  Check(result.diagnostic.ok && result.output_batch.rows.size() == 1 &&
        result.output_batch.rows[0].values.size() == 1, "native aggregate did not publish one result");
  const auto& value = result.output_batch.rows[0].values[0];
  Check(value.descriptor == request.result_column.descriptor && value.encoded_value.empty(),
        "aggregate lost binding or published text shadow");
  if (!expected) {
    Check(value.state == api::EngineValueState::sql_null && value.is_null && value.binary_value.empty(),
          "empty SUM did not preserve typed NULL");
    return;
  }
  Check(value.state == api::EngineValueState::value && !value.is_null && value.binary_value.size() == 8,
        "aggregate returned noncanonical INT64 payload/state");
  std::uint64_t bits = 0;
  for (unsigned i = 0; i < 8; ++i) bits |= std::uint64_t(value.binary_value[i]) << (8*i);
  Check(std::bit_cast<std::int64_t>(bits) == *expected, "aggregate value differs from independent integer oracle");
  std::int64_t decoded = 42;
  std::string detail;
  Check(exec::DecodeBoundInt64Value(value, &decoded, &detail) && decoded == *expected,
        "aggregate result rejected by strict downstream decoder");
  Check(result.final_output_bytes == 8 && result.planned_final_output_bytes == 8,
        "aggregate finalization still budgets decimal width");
}
int main() {
  using Fn = exec::CanonicalAggregateFunction;
  const auto admitted = NativeRequest(Fn::sum).result_column;
  // Warm process-wide immutable registries before sweeping this admission's
  // own allocations; startup initialization has its own fault coverage.
  Check(exec::detail::ValidateAggregateInt64ResultDescriptor(admitted).ok,
        "valid aggregate admission failed before allocation sweep");
  bool exhausted_allocations = false;
  for (long allocation = 0; allocation < 1000; ++allocation) {
    fail_after = allocation;
    const auto result = exec::detail::ValidateAggregateInt64ResultDescriptor(admitted);
    const bool injected = fail_after == -1;
    fail_after = -1;
    if (!injected) {
      Check(result.ok, "valid aggregate result descriptor refused");
      exhausted_allocations = true;
      break;
    }
    Check(!result.ok && result.diagnostic_code == "SBLR.PLAN_TREE.RESOURCE_LIMIT",
          "aggregate admission allocation failure lost typed resource diagnostic");
  }
  Check(exhausted_allocations, "aggregate admission allocation sweep did not finish");
  for (const auto [fn, expected] : {std::pair{Fn::min, 1}, {Fn::max, 2}, {Fn::mode, 2}}) {
    auto request = NativeRequest(fn);
    auto& column = request.input_batch.columns[0];
    column.descriptor.canonical_type_name = "int32";
    BindColumn(column);
    for (auto& row : request.input_batch.rows) {
      row.values[0].descriptor = column.descriptor;
      if (!row.values[0].isSqlNull()) row.values[0].binary_value.resize(4);
    }
    if (fn == Fn::mode) BindEqualityAuthority(&request);
    request.maximum_final_output_bytes = 8;
    CheckResult(request, exec::ExecuteCanonicalAggregateRuntime(request), expected);
    request.maximum_final_output_bytes = 7;
    const auto short_grant = exec::ExecuteCanonicalAggregateRuntime(request);
    Check(!short_grant.diagnostic.ok && short_grant.output_batch.rows.empty(),
          "selected INT32 aggregate result bypassed INT64 output grant");
  }
  for (const auto [fn, expected] : {std::pair{Fn::count, 4}, {Fn::sum, 5}, {Fn::regr_count, 2},
                                   {Fn::rank, 2}, {Fn::dense_rank, 2}, {Fn::approx_count_distinct, 2},
                                   {Fn::min, 1}, {Fn::max, 2}, {Fn::mode, 2}}) {
    auto request = NativeRequest(fn);
    for (const bool empty : {false, true}) {
      for (unsigned mutation = 0; mutation < 9; ++mutation) {
        auto invalid = NativeRequest(fn);
        if (empty) invalid.input_batch.rows.clear();
        auto& descriptor = invalid.result_column.descriptor;
        switch (mutation) {
          case 0: descriptor.datatype_descriptor_uuid = {}; break;
          case 1: ++descriptor.datatype_descriptor_generation; break;
          case 2: descriptor.type_uuid = {}; break;
          case 3: descriptor.descriptor_uuid = {}; break;
          case 4: descriptor.charset_uuid = descriptor.type_uuid; break;
          case 5: descriptor.collation_uuid = descriptor.type_uuid; break;
          case 6: descriptor.encoded_descriptor += ";precision=10"; break;
          case 7: descriptor.encoded_descriptor += ";nullable=true"; break;
          case 8: invalid.result_column.nullable = !invalid.result_column.nullable; break;
        }
        const auto rejected = exec::ExecuteCanonicalAggregateRuntime(invalid);
        Check(!rejected.diagnostic.ok && rejected.output_batch.rows.empty(),
              "invalid aggregate binding published a result");
      }
    }
    request.maximum_final_output_bytes = 8;
    CheckResult(request, exec::ExecuteCanonicalAggregateRuntime(request), expected);
    request.maximum_final_output_bytes = 7;
    const auto refused = exec::ExecuteCanonicalAggregateRuntime(request);
    Check(!refused.diagnostic.ok && refused.output_batch.rows.empty(), "short final output grant accepted");
    request.maximum_final_output_bytes = 8;
    request.input_batch.rows.clear();
    CheckResult(request, exec::ExecuteCanonicalAggregateRuntime(request),
                request.result_column.nullable ? std::optional<std::int64_t>{} :
                fn == Fn::rank || fn == Fn::dense_rank ? 1 : 0);
  }
  for (const auto number : {INT64_MIN, std::int64_t{-1}, std::int64_t{0}, INT64_MAX}) {
    auto request = NativeRequest(Fn::sum);
    request.input_batch.rows.resize(1);
    request.input_batch.rows[0].values[0] = Native(request.input_batch.columns[0].descriptor, number);
    request.maximum_final_output_bytes = 8;
    CheckResult(request, exec::ExecuteCanonicalAggregateRuntime(request), number);
    if (number == INT64_MIN || number == INT64_MAX) {
      auto row = request.input_batch.rows[0];
      row.values[0] = Native(row.values[0].descriptor, number < 0 ? -1 : 1);
      request.input_batch.rows.push_back(row);
      const auto overflow = exec::ExecuteCanonicalAggregateRuntime(request);
      Check(!overflow.diagnostic.ok && overflow.output_batch.rows.empty(), "SUM overflow published a value");
    }
  }
  for (const auto fn : {Fn::array_agg, Fn::json_agg, Fn::approx_top_k}) {
    auto request = NativeRequest(fn);
    request.result_column = Request(fn, 0, 2101,
        fn == Fn::array_agg ? "list<int64 nullable>" : "json").result_column;
    request.result_column.nullable = true;
    if (fn != Fn::array_agg) BindColumn(request.result_column);
    auto& rows = request.input_batch.rows;
    const auto& descriptor = request.input_batch.columns[0].descriptor;
    rows[0].values[0] = Native(descriptor, INT64_MIN);
    rows[1].values[0] = Native(descriptor, INT64_MAX);
    rows[2].values[0] = Native(descriptor, INT64_MIN);
    if (fn == Fn::approx_top_k) {
      request.direct_arguments = {Native(descriptor, 2)};
      BindEqualityAuthority(&request);
    } else {
      request.aggregate_order_terms = {{.column=3, .expression_descriptor_id=2104}};
      for (std::size_t i = 0; i < rows.size(); ++i)
        rows[i].values[3] = Native(request.input_batch.columns[3].descriptor, i + 1);
    }
    const std::string expected = fn == Fn::array_agg
        ? "list[int64:-9223372036854775808;int64:9223372036854775807;int64:-9223372036854775808;NULL]"
        : fn == Fn::json_agg
        ? "[-9223372036854775808,9223372036854775807,-9223372036854775808,null]"
        : "[{\"value\":\"-9223372036854775808\",\"count\":2},{\"value\":\"9223372036854775807\",\"count\":1}]";
    request.maximum_final_output_bytes = expected.size();
    const auto result = exec::ExecuteCanonicalAggregateRuntime(request);
    if (!result.diagnostic.ok) std::cerr << result.diagnostic.diagnostic_code << ':' << result.diagnostic.detail << '\n';
    Check(result.diagnostic.ok && result.output_batch.rows.size() == 1,
          "native integer collection failed");
    Check(result.output_batch.rows[0].values[0].encoded_value == expected,
          "native integer collection rendered incorrect value");
    Check(result.final_output_bytes == expected.size() && result.planned_final_output_bytes == expected.size(),
          "collection render width and memory plan disagree");
    --request.maximum_final_output_bytes;
    const auto refused = exec::ExecuteCanonicalAggregateRuntime(request);
    Check(!refused.diagnostic.ok && refused.output_batch.rows.empty(),
          "collection accepted short final-output grant");
  }
  std::cout << "native aggregate checks=" << checks << '\n';
}
