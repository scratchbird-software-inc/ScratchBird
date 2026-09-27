// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#define QOW_WIN_003_FIXTURE_ONLY
#include "../sbsql_parser_worker/qow_win_003.cpp"
#include "executor_foundation.hpp"
#include "../support/binary_uuid_fixture.hpp"
#include <array>
#include <stdexcept>
using Bytes = std::vector<std::uint8_t>;
using Cell = std::optional<Bytes>;
void Check(bool ok, const char* message) {
  if (!ok) throw std::runtime_error(message);
}
void NativeWindows(const char* type, unsigned pattern) {
  auto partition = Window401Request();
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
      request.nth_values = std::vector<api::EngineTypedValue>(9, WindowValue(integer_descriptor, "2"));
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
    if (fn < 2) {
      auto with_default = request;
      with_default.offset_values = std::vector<api::EngineTypedValue>(9, WindowValue(integer_descriptor, "1"));
      auto fallback = WindowValue(column.descriptor, {});
      fallback.binary_value = Bytes(std::string_view(type) == "uuid" ? 16 : 3, 0x9c);
      with_default.default_values = std::vector<api::EngineTypedValue>(9, fallback);
      const auto defaulted = exec::ExecuteCanonicalWindowValue(with_default);
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
int main() {
  for (unsigned pattern = 0; pattern < 130; ++pattern) NativeWindows("uuid", pattern);
  NativeWindows("binary", 0);
}
