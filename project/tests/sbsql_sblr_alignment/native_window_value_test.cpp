// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#define QOW_WIN_003_FIXTURE_ONLY
#include "../sbsql_parser_worker/qow_win_003.cpp"
#include "executor_foundation.hpp"
#include "../support/binary_uuid_fixture.hpp"
#include <array>
#include <bit>
#include <limits>
#include <locale>
#include <stdexcept>
using Bytes = std::vector<std::uint8_t>;
using Cell = std::optional<Bytes>;
unsigned checks = 0;
void Check(bool ok, const char* message) {
  ++checks;
  if (!ok) throw std::runtime_error(message);
}
api::EngineDescriptor InvalidRatioDescriptor(api::EngineDescriptor descriptor, unsigned mutation) {
  switch (mutation) {
    case 0: descriptor.datatype_descriptor_uuid = {}; break;
    case 1: ++descriptor.datatype_descriptor_generation; break;
    case 2: descriptor.type_uuid = scratchbird::tests::FixtureUuid(39, 9); break;
    case 3: descriptor.descriptor_uuid = {}; break;
    case 4: descriptor.charset_uuid = scratchbird::tests::FixtureUuid(39, 8); break;
    case 5: descriptor.encoded_descriptor += ";precision=1"; break;
    case 6: descriptor.encoded_descriptor = "nullability=nullable"; break;
    case 7: descriptor.collation_uuid = scratchbird::tests::FixtureUuid(39, 7); break;
  }
  return descriptor;
}
api::EngineTypedValue NativeInteger(const api::EngineDescriptor& descriptor,
                                   std::int64_t number) {
  auto value = WindowValue(descriptor, {});
  const auto bits = static_cast<std::uint64_t>(number);
  for (unsigned byte = 0; byte < 8; ++byte)
    value.binary_value.push_back(static_cast<std::uint8_t>(bits >> (byte * 8)));
  return value;
}
void NativeIntegerOrdering() {
  const auto descriptor = WindowDescriptor(5000, "int64",
      exec::MakeExecutorDescriptor("int64").type_uuid, "nullability=nullable");
  const std::array<std::int64_t, 9> numbers = {
      std::numeric_limits<std::int64_t>::min(), -65536, -256, -1, 0, 1, 255, 256,
      std::numeric_limits<std::int64_t>::max()};
  exec::CanonicalDescriptorOrderTerm term;
  term.expression_descriptor_id = 5000;
  for (auto placement : {exec::CanonicalDescriptorNullPlacement::first,
                         exec::CanonicalDescriptorNullPlacement::last}) {
    term.null_placement = placement;
    for (bool descending : {false, true}) {
      term.direction = descending ? exec::CanonicalDescriptorOrderDirection::descending
                                  : exec::CanonicalDescriptorOrderDirection::ascending;
      for (const auto a : numbers) for (const auto b : numbers) {
        const auto left = NativeInteger(descriptor, a), right = NativeInteger(descriptor, b);
        const auto result = exec::CompareCanonicalDescriptorOrderValues(left, right, term);
        const int expected = a < b ? -1 : (a > b ? 1 : 0);
        Check(result.diagnostic.ok && result.comparison == (descending ? -expected : expected),
              "binary INT64 physical ordering differs from signed integer oracle");
        const auto left_key = exec::MakeCanonicalDescriptorEqualityKey(left, term);
        const auto right_key = exec::MakeCanonicalDescriptorEqualityKey(right, term);
        const auto plan = exec::PlanCanonicalDescriptorEqualityKey(left, term);
        Check(left_key.diagnostic.ok && right_key.diagnostic.ok && plan.diagnostic.ok &&
              (left_key.equality_key == right_key.equality_key) == (a == b) &&
              left_key.equality_key.capacity() <= plan.retained_key_bytes,
              "binary INT64 equality or memory plan differs from independent oracle");
      }
      const auto empty = WindowNull(descriptor), zero = NativeInteger(descriptor, 0);
      const int expected = placement == exec::CanonicalDescriptorNullPlacement::first ? -1 : 1;
      const auto left = exec::CompareCanonicalDescriptorOrderValues(empty, zero, term);
      const auto right = exec::CompareCanonicalDescriptorOrderValues(zero, empty, term);
      const auto null_key = exec::MakeCanonicalDescriptorEqualityKey(empty, term);
      const auto zero_key = exec::MakeCanonicalDescriptorEqualityKey(zero, term);
      Check(left.diagnostic.ok && left.comparison == expected &&
            right.diagnostic.ok && right.comparison == -expected &&
            null_key.diagnostic.ok && zero_key.diagnostic.ok &&
            null_key.equality_key != zero_key.equality_key,
            "INT64 containing NULL placement changed with sort direction or aliases zero");
    }
  }
  for (unsigned mutation = 0; mutation < 10; ++mutation) {
    auto bad = NativeInteger(descriptor, 1);
    switch (mutation) {
      case 0: bad.descriptor.datatype_descriptor_uuid = {}; break;
      case 1: ++bad.descriptor.datatype_descriptor_generation; break;
      case 2: bad.descriptor.type_uuid = {}; break;
      case 3: bad.binary_value.pop_back(); break;
      case 4: bad.binary_value.push_back(0); break;
      case 5: bad.encoded_value = "1"; break;
      case 6: bad.binary_value.clear(); bad.encoded_value = "12345678"; break;
      case 7: bad.state = api::EngineValueState::missing; break;
      case 8: bad.is_null = true; bad.state = api::EngineValueState::sql_null; break;
      case 9: bad.descriptor.encoded_descriptor += ";ordering_profile=unsigned"; break;
    }
    const auto zero = NativeInteger(descriptor, 0);
    const auto key = exec::MakeCanonicalDescriptorEqualityKey(bad, term);
    Check(!exec::CompareCanonicalDescriptorOrderValues(bad, zero, term).diagnostic.ok &&
          !exec::CompareCanonicalDescriptorOrderValues(zero, bad, term).diagnostic.ok &&
          !key.diagnostic.ok && key.equality_key.empty(),
          "physical INT64 accepted an unbound or malformed operand");
  }
}
void NativeWindows(const char* type, unsigned pattern) {
  auto partition = Window401Request();
  for (auto& row : partition.input_batch.rows) {
    for (auto& value : row.values) {
      if (value.descriptor.canonical_type_name == "int64" && !value.is_null)
        value = NativeInteger(value.descriptor, std::stoll(WindowScalarText(value)));
    }
  }
  auto& column = partition.input_batch.columns[4];
  column.descriptor = WindowDescriptor(4105, type, exec::MakeExecutorDescriptor(type).type_uuid,
                                       "nullability=nullable");
  column.nullable = true;
  std::array<Cell, 9> input;
  for (unsigned row = 0; row < input.size(); ++row) {
    Bytes bits(16);
    if (pattern == 1) std::fill(bits.begin(), bits.end(), 0xff);
    if (pattern >= 2) bits[(pattern - 2) / 8] = std::uint8_t(1u << ((pattern - 2) % 8));
    bits.back() ^= std::uint8_t(row);
    if (std::string_view(type) == "binary" && row == 0) bits.clear();
    input[row] = row == 8 ? Cell{} : Cell{bits};
    auto value = input[row] ? WindowValue(column.descriptor, {}) : WindowNull(column.descriptor);
    if (input[row]) value.binary_value = bits;
    partition.input_batch.rows[row].values[4] = std::move(value);
  }
  const auto frames = ExecuteFrame(partition, ExplicitFrame(exec::CanonicalWindowFrameUnit::rows,
      FrameBound(exec::CanonicalWindowFrameBoundKind::unbounded_preceding),
      FrameBound(exec::CanonicalWindowFrameBoundKind::unbounded_following)));
  if (!frames.diagnostic.ok) {
    const auto ordered = exec::ExecuteCanonicalWindowPartitionOrder(partition);
    std::cerr << ordered.diagnostic.detail << '\n' << frames.diagnostic.detail << '\n';
  }
  Check(frames.diagnostic.ok, "native window frame execution failed");
  // Independent oracle from this fixture's fixed partition/order columns.
  const std::array<unsigned, 9> order = {1, 5, 8, 0, 7, 2, 3, 4, 6};
  const std::array<unsigned, 9> begin = {0, 0, 0, 0, 0, 5, 6, 6, 8};
  const std::array<unsigned, 9> end = {5, 5, 5, 5, 5, 6, 8, 8, 9};
  const std::array functions = {exec::CanonicalWindowValueFunction::lag,
      exec::CanonicalWindowValueFunction::lead, exec::CanonicalWindowValueFunction::first_value,
      exec::CanonicalWindowValueFunction::last_value, exec::CanonicalWindowValueFunction::nth_value};
  const std::array ids = {
      scratchbird::tests::FixtureUuidLiteral("019de5fc-2400-782c-8436-9ac310301738"),
      scratchbird::tests::FixtureUuidLiteral("019de5fc-2400-7a06-bc3c-6747cf5be66f"),
      scratchbird::tests::FixtureUuidLiteral("019de5fc-2400-7264-90fb-d25bd0f806f2"),
      scratchbird::tests::FixtureUuidLiteral("019de5fc-2400-7d23-a5be-7ed3f1a5c3ec"),
      scratchbird::tests::FixtureUuidLiteral("019de5fc-2400-7dc9-80e6-9f2ccf08076f")};
  for (unsigned fn = 0; fn < functions.size(); ++fn) {
    exec::CanonicalWindowValueRequest request;
    request.frames = frames; request.function = functions[fn]; request.function_uuid = ids[fn];
    request.value_expression_descriptor_id = 4005;
    request.result_column = column; request.result_column.descriptor_id = 4999;
    request.result_column.stable_name = "window_value";
    request.mga_authority = partition.mga_authority;
    const auto integer_descriptor = WindowDescriptor(5000, "int64",
        exec::MakeExecutorDescriptor("int64").type_uuid, "nullability=non_null");
    if (fn == 4) {
      request.nth_values = std::vector<api::EngineTypedValue>(9, NativeInteger(integer_descriptor, 2));
      request.nth_origin = exec::CanonicalWindowNthOrigin::from_first;
      request.null_treatment = exec::CanonicalWindowNullTreatment::respect_nulls;
    }
    const auto result = exec::ExecuteCanonicalWindowValue(request);
    if (!result.diagnostic.ok) std::cerr << "function=" << fn << ':' << result.diagnostic.detail << '\n';
    Check(result.diagnostic.ok && result.values.size() == 9, "native window value execution failed");
    for (unsigned row = 0; row < 9; ++row) {
      unsigned selected = 9;
      if (fn == 0 && row > begin[row]) selected = row - 1;
      if (fn == 1 && row + 1 < end[row]) selected = row + 1;
      if (fn == 2) selected = begin[row];
      if (fn == 3) selected = end[row] - 1;
      if (fn == 4 && begin[row] + 1 < end[row]) selected = begin[row] + 1;
      const auto expected = selected < 9 ? input[order[selected]] : Cell{};
      const auto& actual = result.values[row];
      Check(actual.descriptor == column.descriptor && actual.encoded_value.empty() &&
            actual.is_null == !expected.has_value() &&
            actual.state == (expected ? api::EngineValueState::value : api::EngineValueState::sql_null) &&
            actual.binary_value == expected.value_or(Bytes{}),
            "native window selection lost bits, target binding, NULL, or empty binary");
    }
    if (pattern != 0) continue;
    if (fn == 4) {
      for (const auto position : {std::int64_t{0}, std::int64_t{1}, std::numeric_limits<std::int64_t>::max()}) {
        auto boundary = request;
        boundary.nth_values = std::vector<api::EngineTypedValue>(9, NativeInteger(integer_descriptor, position));
        const auto result = exec::ExecuteCanonicalWindowValue(boundary);
        if (position == 0) {
          Check(!result.diagnostic.ok && result.values.empty(), "NTH_VALUE accepted zero position");
          continue;
        }
        Check(result.diagnostic.ok && result.values.size() == 9, "NTH_VALUE refused valid INT64 boundary");
        for (unsigned row = 0; row < 9; ++row) {
          const Cell expected = position == 1 ? input[order[begin[row]]] : Cell{};
          Check(result.values[row].is_null == !expected.has_value() &&
                result.values[row].binary_value == expected.value_or(Bytes{}),
                "NTH_VALUE boundary wrapped or lost selected value");
        }
      }
    }
    if (fn < 2) {
      auto with_default = request;
      with_default.offset_values = std::vector<api::EngineTypedValue>(9, NativeInteger(integer_descriptor, 1));
      auto fallback = WindowValue(column.descriptor, {});
      fallback.binary_value = Bytes(std::string_view(type) == "uuid" ? 16 : 3, 0x9c);
      with_default.default_values = std::vector<api::EngineTypedValue>(9, fallback);
      const auto defaulted = exec::ExecuteCanonicalWindowValue(with_default);
      if (!defaulted.diagnostic.ok) std::cerr << defaulted.diagnostic.detail << '\n';
      Check(defaulted.diagnostic.ok && defaulted.values.size() == 9 &&
            defaulted.converted_default_value_count == 9, "native window default conversion failed");
      for (unsigned row = 0; row < 9; ++row) {
        const bool outside = fn == 0 ? row == begin[row] : row + 1 == end[row];
        const Cell expected = outside ? Cell{fallback.binary_value} : input[order[fn == 0 ? row - 1 : row + 1]];
        const auto& actual = defaulted.values[row];
        Check(actual.descriptor == column.descriptor && actual.encoded_value.empty() &&
              actual.is_null == !expected.has_value() && actual.binary_value == expected.value_or(Bytes{}),
              "native window default changed bits or replaced an in-partition NULL");
      }
      with_default.default_values->front().encoded_value = std::string(16, '\0');
      const auto refused_default = exec::ExecuteCanonicalWindowValue(with_default);
      Check(!refused_default.diagnostic.ok && refused_default.values.empty(),
            "ambiguous native window default published results");
      for (unsigned mutation = 0; mutation < 12; ++mutation) {
        auto invalid = request;
        auto offset = NativeInteger(integer_descriptor, 1);
        switch (mutation) {
          case 0: offset.descriptor.datatype_descriptor_uuid = {}; break;
          case 1: ++offset.descriptor.datatype_descriptor_generation; break;
          case 2: offset.binary_value.pop_back(); break;
          case 3: offset.binary_value.push_back(0); break;
          case 4: offset.encoded_value = "1"; break;
          case 5: offset = NativeInteger(integer_descriptor, -1); break;
          case 6: offset = WindowNull(integer_descriptor); break;
          case 7: offset.binary_value.clear(); offset.encoded_value = "12345678"; break;
          case 8: offset.descriptor.encoded_descriptor += ";width=32"; break;
          case 9: offset.descriptor.encoded_descriptor += ";precision=1"; break;
          case 10: offset.descriptor.charset_uuid = WindowUuid(4200); break;
          case 11: offset.descriptor.collation_uuid = kWindowCollationUuid; break;
        }
        invalid.offset_values = std::vector<api::EngineTypedValue>(9, offset);
        const auto refused = exec::ExecuteCanonicalWindowValue(invalid);
        Check(!refused.diagnostic.ok && refused.values.empty(),
              "window accepted malformed/unbound/negative canonical INT64 offset");
        invalid.function = exec::CanonicalWindowValueFunction::nth_value;
        invalid.function_uuid = ids[4];
        invalid.offset_values.reset();
        invalid.nth_values = std::vector<api::EngineTypedValue>(9, offset);
        invalid.nth_origin = exec::CanonicalWindowNthOrigin::from_first;
        invalid.null_treatment = exec::CanonicalWindowNullTreatment::respect_nulls;
        const auto refused_nth = exec::ExecuteCanonicalWindowValue(invalid);
        Check(!refused_nth.diagnostic.ok && refused_nth.values.empty(),
              "NTH_VALUE accepted malformed/unbound/negative canonical INT64 position");
      }
      for (const auto offset : {std::int64_t{0}, std::numeric_limits<std::int64_t>::max()}) {
        auto boundary = request;
        boundary.offset_values = std::vector<api::EngineTypedValue>(9, NativeInteger(integer_descriptor, offset));
        const auto result = exec::ExecuteCanonicalWindowValue(boundary);
        Check(result.diagnostic.ok && result.values.size() == 9,
              "window rejected a valid INT64 offset boundary");
        for (unsigned row = 0; row < 9; ++row) {
          const Cell expected = offset == 0 ? input[order[row]] : Cell{};
          Check(result.values[row].is_null == !expected.has_value() &&
                result.values[row].binary_value == expected.value_or(Bytes{}),
                "window offset boundary wrapped or lost NULL/value data");
        }
      }
    }
    for (unsigned mutation = 0; mutation < 3; ++mutation) {
      auto changed = request;
      auto& value = changed.frames.ordered_batch.rows.front().values[4];
      if (mutation == 0) value.encoded_value = std::string(16, '\0');
      if (mutation == 1) { value.state = api::EngineValueState::sql_null; value.is_null = true; }
      if (mutation == 2) { value.binary_value.clear(); value.encoded_value = std::string(16, '\0'); }
      const auto refused = exec::ExecuteCanonicalWindowValue(changed);
      Check(!refused.diagnostic.ok && refused.values.empty(), "malformed native window operand published results");
    }
  }
}
void NativeIntegerWindowPositions() {
  auto partition = Window401Request();
  for (auto& row : partition.input_batch.rows) for (auto& value : row.values)
    if (value.descriptor.canonical_type_name == "int64" && !value.is_null)
      value = NativeInteger(value.descriptor, std::stoll(WindowScalarText(value)));
  auto descriptor = exec::MakeExecutorDescriptor("int64", "nullability=non_null");
  descriptor.descriptor_kind = "scalar";
  auto& column = partition.input_batch.columns[4];
  column.descriptor = descriptor;
  column.descriptor.encoded_descriptor = "nullability=nullable";
  column.nullable = true;
  for (unsigned row = 0; row < 9; ++row)
    partition.input_batch.rows[row].values[4] = NativeInteger(column.descriptor, 100 + row);
  const auto frames = ExecuteFrame(partition, ExplicitFrame(exec::CanonicalWindowFrameUnit::rows,
      FrameBound(exec::CanonicalWindowFrameBoundKind::unbounded_preceding),
      FrameBound(exec::CanonicalWindowFrameBoundKind::unbounded_following)));
  Check(frames.diagnostic.ok, "integer window frame failed");
  exec::CanonicalWindowRankingRequest ntile;
  ntile.frames = frames;
  ntile.function = exec::CanonicalWindowRankingFunction::ntile;
  ntile.function_uuid = scratchbird::tests::FixtureUuidLiteral("019de5fc-2400-7047-9474-232ca488c094");
  ntile.output_descriptor = descriptor;
  ntile.mga_authority = partition.mga_authority;
  const std::array<std::int64_t, 9> three = {1, 1, 2, 2, 3, 1, 1, 2, 1};
  const std::array<std::int64_t, 9> many = {1, 2, 3, 4, 5, 1, 1, 2, 1};
  for (const auto count : {std::int64_t{1}, std::int64_t{3}, std::int64_t{8},
                         std::numeric_limits<std::int64_t>::max()}) {
    ntile.ntile_bucket_count = NativeInteger(descriptor, count);
    const auto result = exec::ExecuteCanonicalWindowRanking(ntile);
    if (!result.diagnostic.ok) std::cerr << result.diagnostic.detail << '\n';
    Check(result.diagnostic.ok && result.values.size() == 9 && result.resolved_ntile_bucket_count == count,
          "binary NTILE execution refused exact shared datatype identity");
    for (unsigned row = 0; row < 9; ++row) {
      std::int64_t actual = 0;
      std::string detail;
      Check(exec::DecodeBoundInt64Value(result.values[row], &actual, &detail) &&
            actual == (count == 1 ? 1 : count == 3 ? three[row] : many[row]) &&
            result.values[row].encoded_value.empty(), "NTILE output differs from independent bucket oracle");
    }
  }
  for (unsigned mutation = 0; mutation < 7; ++mutation) {
    auto invalid = ntile;
    auto& value = *invalid.ntile_bucket_count;
    switch (mutation) {
      case 0: value = NativeInteger(descriptor, 0); break;
      case 1: value = NativeInteger(descriptor, -1); break;
      case 2: value.encoded_value = "3"; break;
      case 3: value.binary_value.clear(); value.encoded_value = "3"; break;
      case 4: ++value.descriptor.datatype_descriptor_generation; break;
      case 5: value.binary_value.pop_back(); break;
      case 6: ++invalid.output_descriptor.datatype_descriptor_generation; break;
    }
    const auto result = exec::ExecuteCanonicalWindowRanking(invalid);
    Check(!result.diagnostic.ok && result.values.empty(), "NTILE accepted invalid input or published a prefix");
  }
  auto empty_partition = partition;
  empty_partition.input_batch.rows.clear();
  auto empty_ntile = ntile;
  empty_ntile.frames = ExecuteFrame(empty_partition, ExplicitFrame(exec::CanonicalWindowFrameUnit::rows,
      FrameBound(exec::CanonicalWindowFrameBoundKind::unbounded_preceding),
      FrameBound(exec::CanonicalWindowFrameBoundKind::unbounded_following)));
  Check(empty_ntile.frames.diagnostic.ok, "empty integer window frame failed");
  for (bool percent : {false, true}) {
    auto ranking = empty_ntile;
    ranking.function = percent ? exec::CanonicalWindowRankingFunction::percent_rank
                               : exec::CanonicalWindowRankingFunction::cume_dist;
    ranking.function_uuid = percent
        ? scratchbird::tests::FixtureUuidLiteral("019de5fc-2400-7d86-86fe-96f3f27b5dd6")
        : scratchbird::tests::FixtureUuidLiteral("019de5fc-2400-721c-be64-2568b64a02b9");
    ranking.ntile_bucket_count.reset();
    ranking.output_descriptor = WindowDescriptor(6010, "real64",
        exec::MakeExecutorDescriptor("real64").type_uuid, "nullability=non_null");
    const auto good = exec::ExecuteCanonicalWindowRanking(ranking);
    Check(good.diagnostic.ok && good.values.empty(), "exact empty real ranking binding refused");
    for (unsigned mutation = 0; mutation < 8; ++mutation) {
      auto invalid = ranking;
      invalid.output_descriptor = InvalidRatioDescriptor(ranking.output_descriptor, mutation);
      const auto refused = exec::ExecuteCanonicalWindowRanking(invalid);
      Check(!refused.diagnostic.ok && refused.values.empty(), "empty real ranking bypassed binding admission");
    }
  }
  const std::array<exec::CanonicalWindowRankingFunction, 3> ranking_functions = {
      exec::CanonicalWindowRankingFunction::row_number,
      exec::CanonicalWindowRankingFunction::rank,
      exec::CanonicalWindowRankingFunction::dense_rank};
  const std::array<api::EngineUuid, 3> ranking_ids = {
      scratchbird::tests::FixtureUuidLiteral("019de5fc-2400-7539-bcce-00eef3ae7220"),
      scratchbird::tests::FixtureUuidLiteral("019de5fc-2400-7b94-870d-0dd789ca70ab"),
      scratchbird::tests::FixtureUuidLiteral("019de5fc-2400-741d-bef0-f079fd3ba494")};
  const std::array<std::array<std::int64_t, 9>, 3> ranking_oracles = {{
      {1, 2, 3, 4, 5, 1, 1, 2, 1}, {1, 1, 3, 4, 5, 1, 1, 1, 1},
      {1, 1, 2, 3, 4, 1, 1, 1, 1}}};
  for (unsigned function = 0; function < ranking_functions.size(); ++function) {
    for (bool empty : {false, true}) {
      auto ranking = empty ? empty_ntile : ntile;
      ranking.function = ranking_functions[function];
      ranking.function_uuid = ranking_ids[function];
      ranking.ntile_bucket_count.reset();
      ranking.output_descriptor.descriptor_uuid = scratchbird::tests::FixtureUuid(33, function + 1);
      const auto ranked = exec::ExecuteCanonicalWindowRanking(ranking);
      Check(ranked.diagnostic.ok && ranked.values.size() == (empty ? 0 : 9),
            "native integer ranking refused an exact output descriptor");
      for (unsigned row = 0; row < ranked.values.size(); ++row) {
        std::int64_t actual = 0;
        std::string detail;
        Check(exec::DecodeBoundInt64Value(ranked.values[row], &actual, &detail) &&
                  actual == ranking_oracles[function][row] &&
                  ranked.values[row].descriptor == ranking.output_descriptor,
              "native rank bytes, binding or value differ from independent oracle");
      }
      for (unsigned mutation = 0; mutation != 4; ++mutation) {
        auto invalid = ranking;
        if (mutation == 0) ++invalid.output_descriptor.datatype_descriptor_generation;
        if (mutation == 1) invalid.output_descriptor.type_uuid = {};
        if (mutation == 2) invalid.output_descriptor.encoded_descriptor += ";precision=1";
        if (mutation == 3) invalid.output_descriptor.datatype_descriptor_uuid = {};
        const auto refused = exec::ExecuteCanonicalWindowRanking(invalid);
        Check(!refused.diagnostic.ok && refused.values.empty(),
              "integer ranking accepted stale binding or published a partial result");
      }
    }
  }
  // A distinct occurrence UUID prevents the argument identity guard from
  // accidentally detecting stale output metadata instead of output admission.
  empty_ntile.output_descriptor.descriptor_uuid =
      scratchbird::tests::FixtureUuidLiteral("019de5fc-2400-7001-8000-000000004999");
  const auto empty_result = exec::ExecuteCanonicalWindowRanking(empty_ntile);
  Check(empty_result.diagnostic.ok && empty_result.values.empty(),
        "exact empty NTILE output binding was refused");
  for (unsigned mutation = 0; mutation < 3; ++mutation) {
    auto invalid = empty_ntile;
    switch (mutation) {
      case 0: ++invalid.output_descriptor.datatype_descriptor_generation; break;
      case 1: invalid.output_descriptor.encoded_descriptor += ";precision=1"; break;
      case 2: invalid.output_descriptor.datatype_descriptor_uuid = {}; break;
    }
    const auto result = exec::ExecuteCanonicalWindowRanking(invalid);
    Check(!result.diagnostic.ok && result.values.empty(),
          "empty NTILE bypassed exact output binding admission");
  }
  exec::CanonicalWindowValueRequest nth;
  nth.frames = frames;
  nth.function = exec::CanonicalWindowValueFunction::nth_value;
  nth.function_uuid = scratchbird::tests::FixtureUuidLiteral("019de5fc-2400-7dc9-80e6-9f2ccf08076f");
  nth.value_expression_descriptor_id = 4005;
  nth.result_column = column;
  nth.result_column.descriptor_id = 4999;
  nth.result_column.stable_name = "integer_nth";
  nth.mga_authority = partition.mga_authority;
  nth.nth_values = std::vector<api::EngineTypedValue>(9, NativeInteger(descriptor, 2));
  nth.nth_origin = exec::CanonicalWindowNthOrigin::from_first;
  nth.null_treatment = exec::CanonicalWindowNullTreatment::respect_nulls;
  const auto selected = exec::ExecuteCanonicalWindowValue(nth);
  if (!selected.diagnostic.ok) std::cerr << selected.diagnostic.detail << '\n';
  Check(selected.diagnostic.ok && selected.values.size() == 9,
        "NTH_VALUE rejects shared INT64 datatype despite distinct expression handles");
  const std::array<std::int64_t, 9> expected = {105, 105, 105, 105, 105, -1, 104, 104, -1};
  for (unsigned row = 0; row < 9; ++row) {
    std::int64_t actual = 0;
    std::string detail;
    const auto& value = selected.values[row];
    Check(expected[row] == -1 ? value.isSqlNull() && value.binary_value.empty() && value.encoded_value.empty()
        : exec::DecodeBoundInt64Value(value, &actual, &detail) && actual == expected[row],
        "NTH_VALUE binary selection differs from independent partition oracle");
  }
}

void NativeRangeBoundaries() {
  for (bool empty : {false, true}) {
    auto partition = Window401Request();
    partition.order_terms.resize(1);
    if (empty) partition.input_batch.rows.clear();
    for (auto unit : {exec::CanonicalWindowFrameUnit::rows,
                      exec::CanonicalWindowFrameUnit::groups,
                      exec::CanonicalWindowFrameUnit::range}) {
      const auto operand = NativeInteger(exec::MakeExecutorDescriptor("int64", "nullability=non_null"), 1);
      const auto frame = ExplicitFrame(unit,
          FrameBound(exec::CanonicalWindowFrameBoundKind::offset_preceding, operand),
          FrameBound(exec::CanonicalWindowFrameBoundKind::current_row));
      Check(ExecuteFrame(partition, frame).diagnostic.ok, "native frame offset refused before mutation");
      for (unsigned mutation = 0; mutation < 8; ++mutation) {
        auto invalid = frame;
        auto& bad = *invalid.start->offset;
        if (mutation == 0) bad.descriptor.datatype_descriptor_uuid = {};
        if (mutation == 1) ++bad.descriptor.datatype_descriptor_generation;
        if (mutation == 2) bad.descriptor.encoded_descriptor += ";precision=1";
        if (mutation == 3) bad.descriptor.type_uuid = {};
        if (mutation == 4) bad.encoded_value = "1";
        if (mutation == 5) { bad.binary_value.clear(); bad.encoded_value = "1"; }
        if (mutation == 6) bad.binary_value.pop_back();
        if (mutation == 7) bad = NativeInteger(bad.descriptor, -1);
        const auto refused = ExecuteFrame(partition, invalid);
        Check(!refused.diagnostic.ok && refused.effective_frames.empty(),
              "frame offset bypassed exact admission on populated or empty partition");
      }
    }
  }
  for (bool descending : {false, true}) {
    auto partition = Window401Request();
    partition.order_terms.resize(1);
    partition.order_terms[0].direction = descending
        ? exec::CanonicalDescriptorOrderDirection::descending
        : exec::CanonicalDescriptorOrderDirection::ascending;
    partition.input_batch.rows.resize(5);
    const std::array<std::int64_t, 5> keys{INT64_MIN, -1, 0, 1, INT64_MAX};
    for (unsigned row = 0; row < 5; ++row) {
      partition.input_batch.rows[row].values[0] = partition.input_batch.rows[0].values[0];
      partition.input_batch.rows[row].values[1] = partition.input_batch.rows[0].values[1];
      partition.input_batch.rows[row].values[2] = NativeInteger(
          partition.input_batch.columns[2].descriptor, keys[row]);
    }
    for (const auto offset : {std::int64_t{0}, std::int64_t{1}, std::int64_t{INT64_MAX}}) {
      const auto operand = NativeInteger(exec::MakeExecutorDescriptor("int64", "nullability=non_null"), offset);
      auto frame = ExplicitFrame(exec::CanonicalWindowFrameUnit::range,
          FrameBound(exec::CanonicalWindowFrameBoundKind::offset_preceding, operand),
          FrameBound(exec::CanonicalWindowFrameBoundKind::offset_following, operand));
      const auto result = ExecuteFrame(partition, frame);
      if (!result.diagnostic.ok) std::cerr << result.diagnostic.detail << '\n';
      Check(result.diagnostic.ok && result.effective_frames.size() == 5,
            "native RANGE refused exact bound-extreme coordinates");
      for (unsigned row = 0; row < 5; ++row) {
        std::vector<std::size_t> expected;
        const auto key = static_cast<std::uint64_t>(keys[descending ? 4 - row : row]) ^ (std::uint64_t{1} << 63);
        for (unsigned candidate = 0; candidate < 5; ++candidate) {
          const auto other = static_cast<std::uint64_t>(keys[descending ? 4 - candidate : candidate]) ^ (std::uint64_t{1} << 63);
          const auto distance = key > other ? key - other : other - key;
          if (distance <= static_cast<std::uint64_t>(offset)) expected.push_back(candidate);
        }
        Check(result.effective_frames[row].effective_row_indices == expected,
              "RANGE overflowed or disagreed with independent unsigned-distance oracle");
      }
      for (unsigned mutation = 0; mutation < 4; ++mutation) {
        auto invalid = frame;
        auto& bad = *invalid.start->offset;
        if (mutation == 0) ++bad.descriptor.datatype_descriptor_generation;
        if (mutation == 1) bad.encoded_value = "1";
        if (mutation == 2) bad.binary_value.pop_back();
        if (mutation == 3) bad = NativeInteger(bad.descriptor, -1);
        const auto refused = ExecuteFrame(partition, invalid);
        Check(!refused.diagnostic.ok && refused.effective_frames.empty(),
              "RANGE accepted malformed or negative native offset");
      }
    }
  }
}

void ExactRankingRatios() {
  struct CommaNumbers : std::numpunct<char> {
    char do_decimal_point() const override { return ','; }
    char do_thousands_sep() const override { return '.'; }
    std::string do_grouping() const override { return "\3"; }
  };
  struct RestoreLocale {
    std::locale original = std::locale();
    ~RestoreLocale() { std::locale::global(original); }
  } restore;
  std::locale::global(std::locale(std::locale::classic(), new CommaNumbers));
  const auto descriptor = WindowDescriptor(6000, "real64",
      exec::MakeExecutorDescriptor("real64").type_uuid, "nullability=non_null");
  struct Ratio { std::uint64_t numerator, denominator, bits; };
  const std::array<Ratio, 7> ratios{{
      {1, 3, 0x3fd5555555555555ULL}, {2, 3, 0x3fe5555555555555ULL},
      {1, 1, 0x3ff0000000000000ULL},
      {1, INT64_MAX, 0x3c00000000000000ULL},
      {INT64_MAX - 1, INT64_MAX, 0x3ff0000000000000ULL},
      {9007199254740993ULL, 18014398509481984ULL, 0x3fe0000000000000ULL},
      {9007199254740995ULL, 18014398509481984ULL, 0x3fe0000000000002ULL}
  }};
  for (const auto& ratio : ratios) {
    exec::CanonicalWindowCumeDistValueRequest request;
    request.function_abi_version = 1;
    request.builtin_id = "sb.window.cume_dist";
    request.function_uuid = scratchbird::tests::FixtureUuidLiteral("019de5fc-2400-721c-be64-2568b64a02b9");
    request.output_descriptor = descriptor;
    request.cumulative_row_count = ratio.numerator;
    request.partition_row_count = ratio.denominator;
    const auto result = exec::ComputeCanonicalWindowCumeDistValue(request);
    Check(result.diagnostic.ok && result.value.descriptor == descriptor &&
              std::bit_cast<std::uint64_t>(std::stod(result.value.encoded_value)) == ratio.bits,
          "CUME_DIST differs from independent nearest-even binary64 ratio oracle");
    for (unsigned mutation = 0; mutation < 8; ++mutation) {
      auto invalid = request;
      invalid.output_descriptor = InvalidRatioDescriptor(descriptor, mutation);
      const auto refused = exec::ComputeCanonicalWindowCumeDistValue(invalid);
      Check(!refused.diagnostic.ok && refused.value.encoded_value.empty() &&
                refused.value.binary_value.empty(), "CUME_DIST accepted invalid output binding");
    }
    if (ratio.denominator < INT64_MAX) {
      exec::CanonicalWindowPercentRankValueRequest percent;
      percent.function_abi_version = 1;
      percent.builtin_id = "sb.window.percent_rank";
      percent.function_uuid = scratchbird::tests::FixtureUuidLiteral("019de5fc-2400-7d86-86fe-96f3f27b5dd6");
      percent.output_descriptor = descriptor;
      percent.one_based_rank = ratio.numerator + 1;
      percent.partition_row_count = ratio.denominator + 1;
      const auto ranked = exec::ComputeCanonicalWindowPercentRankValue(percent);
      Check(ranked.diagnostic.ok &&
                std::bit_cast<std::uint64_t>(std::stod(ranked.value.encoded_value)) == ratio.bits,
            "PERCENT_RANK differs from independent rational oracle");
      percent.one_based_rank = 1;
      const auto zero = exec::ComputeCanonicalWindowPercentRankValue(percent);
      Check(zero.diagnostic.ok && std::stod(zero.value.encoded_value) == 0,
            "first PERCENT_RANK is not zero");
      for (unsigned mutation = 0; mutation < 8; ++mutation) {
        auto invalid = percent;
        invalid.output_descriptor = InvalidRatioDescriptor(descriptor, mutation);
        const auto refused = exec::ComputeCanonicalWindowPercentRankValue(invalid);
        Check(!refused.diagnostic.ok && refused.value.encoded_value.empty() &&
                  refused.value.binary_value.empty(), "PERCENT_RANK zero bypassed output binding admission");
      }
    }
  }
}

int main() {
  NativeRangeBoundaries();
  ExactRankingRatios();
  NativeIntegerOrdering();
  NativeIntegerWindowPositions();
  for (unsigned pattern = 0; pattern < 130; ++pattern) NativeWindows("uuid", pattern);
  NativeWindows("binary", 0);
  std::cout << "native_window_value checks=" << checks << " uuid_patterns=130 failures=0\n";
}
