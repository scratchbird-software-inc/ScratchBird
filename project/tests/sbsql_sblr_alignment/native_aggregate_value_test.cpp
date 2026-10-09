// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#define QOW_QRY_011_REGISTRY_FIXTURE_ONLY
#include "../sbsql_parser_worker/qow_qry_011_registry.cpp"
#include "../support/exact_datatype_descriptor_fixture.hpp"
#include "aggregate_executor_internal.hpp"
#include "sbl_numeric.hpp"
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
void CheckRealResult(exec::CanonicalAggregateRuntimeRequest request, double expected) {
  const bool count_only = request.descriptor.function == exec::CanonicalAggregateFunction::percent_rank ||
                          request.descriptor.function == exec::CanonicalAggregateFunction::cume_dist;
  if (count_only) request.maximum_finalization_workspace_bytes = 1;
  request.maximum_final_output_bytes = 8;
  const auto result = exec::ExecuteCanonicalAggregateRuntime(request);
  if (!result.diagnostic.ok) std::cerr << result.diagnostic.diagnostic_code << ':' << result.diagnostic.detail << '\n';
  Check(NearScalar(result, expected), "native REAL64 aggregate numerical result differs");
  const auto& value = result.output_batch.rows[0].values[0];
  Check(value.descriptor == request.result_column.descriptor && value.encoded_value.empty() &&
        value.binary_value.size() == 8 && result.final_output_bytes == 8 &&
        result.planned_final_output_bytes == 8,
        "REAL64 aggregate lost native carrier, descriptor or exact output accounting");
  if (count_only)
    Check(result.peak_finalization_workspace_bytes == 0,
          "hypothetical distribution still allocates a sorted copy");
  const auto expected_bits = std::bit_cast<std::uint64_t>(expected);
  for (unsigned byte = 0; byte < 8; ++byte)
    Check(value.binary_value[byte] == static_cast<std::uint8_t>(expected_bits >> (8 * byte)),
          "REAL64 aggregate canonical bytes differ from independent IEEE oracle");
  request.maximum_final_output_bytes = 7;
  const auto short_grant = exec::ExecuteCanonicalAggregateRuntime(request);
  Check(!short_grant.diagnostic.ok && short_grant.output_batch.rows.empty(),
        "REAL64 aggregate bypassed short output grant");
  request.maximum_final_output_bytes = 8;
  for (const bool empty : {false, true}) {
    auto candidate = request;
    if (empty) candidate.input_batch.rows.clear();
    const auto baseline = exec::ExecuteCanonicalAggregateRuntime(candidate);
    Check(baseline.diagnostic.ok, "valid REAL64 empty/nonempty admission failed");
    for (unsigned mutation = 0; mutation < 8; ++mutation) {
      auto invalid = candidate;
      auto& descriptor = invalid.result_column.descriptor;
      switch (mutation) {
        case 0: descriptor.datatype_descriptor_uuid = {}; break;
        case 1: ++descriptor.datatype_descriptor_generation; break;
        case 2: descriptor.type_uuid = {}; break;
        case 3: descriptor.descriptor_uuid = {}; break;
        case 4: descriptor.charset_uuid = descriptor.type_uuid; break;
        case 5: descriptor.collation_uuid = descriptor.type_uuid; break;
        case 6: descriptor.encoded_descriptor += ";precision=10"; break;
        case 7: invalid.result_column.nullable = !invalid.result_column.nullable; break;
      }
      const auto refused = exec::ExecuteCanonicalAggregateRuntime(invalid);
      Check(!refused.diagnostic.ok && refused.output_batch.rows.empty(),
            "invalid REAL64 result binding published output");
    }
  }
}

int main() {
  using Fn = exec::CanonicalAggregateFunction;
  const auto real_admitted = Request(Fn::avg, 1, 2102, "real64").result_column;
  Check(exec::detail::ValidateAggregateReal64ResultDescriptor(real_admitted).ok,
        "valid REAL64 admission failed before allocation sweep");
  bool real_exhausted_allocations = false;
  for (long allocation = 0; allocation < 1000; ++allocation) {
    fail_after = allocation;
    const auto result = exec::detail::ValidateAggregateReal64ResultDescriptor(real_admitted);
    const bool injected = fail_after == -1;
    fail_after = -1;
    if (!injected) {
      Check(result.ok, "valid REAL64 descriptor refused");
      real_exhausted_allocations = true;
      break;
    }
    Check(!result.ok && result.diagnostic_code == "SBLR.PLAN_TREE.RESOURCE_LIMIT",
          "REAL64 admission allocation failure lost typed resource diagnostic");
  }
  Check(real_exhausted_allocations, "REAL64 admission allocation sweep did not finish");
  CheckRealResult(Request(Fn::sum, 1, 2102, "real64"), 8.0);
  CheckRealResult(Request(Fn::avg, 1, 2102, "real64"), 8.0 / 3.0);
  CheckRealResult(StatisticalRequest(Fn::variance_pop), 2.0 / 3.0);
  CheckRealResult(StatisticalRequest(Fn::stddev_samp), 1.0);
  CheckRealResult(PairStatisticalRequest(Fn::regr_slope), 2.0);
  CheckRealResult(HypotheticalRequest(Fn::percent_rank), 0.6);
  CheckRealResult(HypotheticalRequest(Fn::cume_dist), 2.0 / 3.0);
  auto quantile = OrderedNumericRequest(Fn::percentile_cont, "real64");
  quantile.direct_arguments = {Value(quantile.input_batch.columns[1].descriptor, "0.25")};
  CheckRealResult(quantile, 17.5);
  namespace numeric = scratchbird::libraries::sbl_numeric;
  for (const auto* text : {"0", "0.5", "1", "0.1", "0.99999999999999999999999999999999999999",
                           "0.00000000000000000000000000000000000001"}) {
    const auto decimal = numeric::EncodeExactDecimalLittleEndian(text);
    Check(decimal.ok, "fraction backend fixture invalid");
    const auto converted = numeric::ExactDecimalUnitFractionToReal64(
        decimal.canonical_bytes.data(), decimal.canonical_bytes.size(),
        {numeric::ExactDecimalCodec::le24_v1, decimal.precision, decimal.scale});
    const auto expected = numeric::EncodeReal64LittleEndian(text);
    Check(converted.bytes && converted.bytes == expected.bytes &&
        converted.numeric.inexact == expected.numeric.inexact,
        "native decimal fraction differs from independent lexical conversion");
  }
  const numeric::ExactDecimalProfile wide_profile{numeric::ExactDecimalCodec::le40_v1, 76, 76};
  const std::string tiny_fraction = "0." + std::string(75, '0') + "1";
  const auto wide_fraction = numeric::EncodeBoundExactDecimal(tiny_fraction, wide_profile);
  Check(wide_fraction.ok(), "wide fraction fixture invalid");
  const auto wide_converted = numeric::ExactDecimalUnitFractionToReal64(
      wide_fraction.bytes.data(), wide_fraction.bytes.size(), wide_profile);
  Check(wide_converted.bytes && wide_converted.bytes == numeric::EncodeReal64LittleEndian(tiny_fraction).bytes,
        "wide fraction coefficient was narrowed before rounding");
  std::uint64_t fraction_seed = 0x317897bd15d321ULL;
  for (unsigned scale = 1; scale <= 76; ++scale) {
    const numeric::ExactDecimalProfile profile{
        scale <= 38 ? numeric::ExactDecimalCodec::le24_v1 : numeric::ExactDecimalCodec::le40_v1,
        scale <= 38 ? 38U : 76U, scale};
    for (unsigned sample = 0; sample < 16; ++sample) {
      std::string text = "0.";
      for (unsigned digit = 0; digit < scale; ++digit) {
        fraction_seed ^= fraction_seed << 13;
        fraction_seed ^= fraction_seed >> 7;
        fraction_seed ^= fraction_seed << 17;
        text.push_back('0' + fraction_seed % 10);
      }
      const auto encoded = numeric::EncodeBoundExactDecimal(text, profile);
      Check(encoded.ok(), "fraction scale sweep fixture invalid");
      fail_after = 0;
      const auto converted = numeric::ExactDecimalUnitFractionToReal64(
          encoded.bytes.data(), encoded.bytes.size(), profile);
      const bool allocation_free = fail_after == 0;
      fail_after = -1;
      const auto reference = numeric::EncodeReal64LittleEndian(text);
      Check(allocation_free && converted.bytes && converted.bytes == reference.bytes &&
            converted.numeric.inexact == reference.numeric.inexact,
            "bounded fraction conversion disagrees with MPFR or allocates");
    }
  }
  for (const auto* text : {
      "0.500000000000000055511151231257827021181583404541015625",
      "0.500000000000000166533453693773481063544750213623046875",
      "0.999999999999999944488848768742172978818416595458984375"}) {
    const numeric::ExactDecimalProfile profile{numeric::ExactDecimalCodec::le40_v1, 76, 76};
    const auto encoded = numeric::EncodeBoundExactDecimal(text, profile);
    Check(encoded.ok(), "halfway fraction fixture invalid");
    const auto converted = numeric::ExactDecimalUnitFractionToReal64(
        encoded.bytes.data(), encoded.bytes.size(), profile);
    Check(converted.bytes && converted.bytes == numeric::EncodeReal64LittleEndian(text).bytes,
          "halfway fraction lost ties-to-even rounding");
  }
  const auto decimal_fraction = [&](std::string_view text) {
    const auto encoded = numeric::EncodeExactDecimalLittleEndian(text);
    Check(encoded.ok, "decimal quantile fixture encoding failed");
    api::EngineTypedValue value;
    value.descriptor = scratchbird::tests::ExactScalarDescriptorFixture(
        dt::CanonicalTypeId::decimal, "decimal",
        scratchbird::tests::FixtureUuid(1086, 800),
        "nullability=non_null;precision=" + std::to_string(encoded.precision) +
        ";scale=" + std::to_string(encoded.scale));
    value.binary_value.assign(encoded.canonical_bytes.begin(), encoded.canonical_bytes.end());
    return value;
  };
  for (const auto& [text, expected] : std::array<std::pair<std::string_view, double>, 4>{{
           {"0", 10.0}, {"0.25", 17.5}, {"0.5", 25.0}, {"1", 40.0}}}) {
    auto decimal_quantile = quantile;
    decimal_quantile.direct_arguments = {decimal_fraction(text)};
    CheckRealResult(decimal_quantile, expected);
  }
  for (const auto* text : {"-0.1", "1.0000000000000000000000000000000000001"}) {
    auto invalid = quantile;
    invalid.direct_arguments = {decimal_fraction(text)};
    const auto refused = exec::ExecuteCanonicalAggregateRuntime(invalid);
    Check(!refused.diagnostic.ok && refused.output_batch.rows.empty(),
          "out-of-range exact fraction admitted after rounding");
  }
  for (unsigned mutation = 0; mutation < 7; ++mutation) {
    auto invalid = quantile;
    invalid.direct_arguments = {decimal_fraction("0.25")};
    auto& value = invalid.direct_arguments.front();
    switch (mutation) {
      case 0: value.encoded_value = "0.25"; break;
      case 1: value.binary_value.pop_back(); break;
      case 2: value.binary_value[3] = 1; break;
      case 3: ++value.descriptor.datatype_descriptor_generation; break;
      case 4: value.descriptor.type_uuid = {}; break;
      case 5: value.descriptor.encoded_descriptor = "nullability=non_null;precision=1;scale=0"; break;
      case 6: value.setState(api::EngineValueState::sql_null); break;
    }
    const auto refused = exec::ExecuteCanonicalAggregateRuntime(invalid);
    Check(!refused.diagnostic.ok && refused.output_batch.rows.empty(),
          "malformed native decimal fraction published output");
  }
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
    if (fn == Fn::rank) request.maximum_finalization_workspace_bytes = 1;
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
