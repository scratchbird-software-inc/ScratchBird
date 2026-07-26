// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "executor_foundation.hpp"

#include <cstdlib>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace exec = scratchbird::engine::executor;

namespace {

constexpr std::string_view kInvalidHandle =
    "SBLR.PLAN_TREE.INVALID_HANDLE";

[[noreturn]] void Fail(const std::string_view detail) {
  std::cerr << "QOW-TEST-QRY-009-V1: " << detail << '\n';
  std::exit(1);
}

template <typename Operation>
void RequireInvalidHandle(Operation&& operation,
                          const std::string_view detail) {
  try {
    static_cast<void>(operation());
  } catch (const std::out_of_range& error) {
    if (error.what() == kInvalidHandle) return;
    Fail(detail);
  } catch (...) {
    Fail(detail);
  }
  Fail(detail);
}

}  // namespace

// QOW-TEST-QRY-009-V1
int main() {
  const auto input = exec::MakeBatch(
      "descriptor-v1",
      {{{7, 0, 70}}, {{8, -2, 80}}, {{9, 4, 90}}});

  const auto projected = exec::ProjectColumns(input, {2, 1, 1});
  if (projected.rows.size() != 3 ||
      projected.rows.front().values != std::vector<std::int64_t>({70, 0, 0}) ||
      projected.rows.back().values != std::vector<std::int64_t>({90, 4, 4})) {
    Fail("valid and duplicate projection handles must preserve actual values");
  }

  const auto sorted = exec::SortByColumn(input, 1, true);
  if (sorted.rows.size() != 3 || sorted.rows[0].values[0] != 8 ||
      sorted.rows[1].values[0] != 7 || sorted.rows[2].values[0] != 9) {
    Fail("valid sort handle must retain stable numeric ordering");
  }

  const auto filtered = exec::FilterByInt64Comparison(
      input, 1, exec::Int64ComparisonOperator::kGreaterThanOrEqual, 0);
  if (filtered.rows.size() != 2 || filtered.rows[0].values[0] != 7 ||
      filtered.rows[1].values[0] != 9) {
    Fail("valid expression handle must evaluate its bound column");
  }

  RequireInvalidHandle(
      [&] { return exec::ProjectColumns(input, {3}); },
      "out-of-range projection handle was not refused exactly");
  RequireInvalidHandle(
      [&] { return exec::SortByColumn(exec::MakeBatch("one", {{{1}}}), 1, true); },
      "single-row sort handle must be resolved before sorting");
  RequireInvalidHandle(
      [&] {
        return exec::FilterByInt64Comparison(
            exec::MakeBatch("ragged", {{{1, 2}}, {{3}}}), 1,
            exec::Int64ComparisonOperator::kEqual, 2);
      },
      "ragged expression handle was silently skipped");
  RequireInvalidHandle(
      [&] {
        return exec::ProjectColumns(
            exec::MakeBatch("ragged", {{{1, 2}}, {{3}}}), {0, 1});
      },
      "ragged projection handle became successful zero data");
  RequireInvalidHandle(
      [&] {
        return exec::SortByColumn(
            exec::MakeBatch("ragged", {{{1, 2}}, {{3}}}), 1, true);
      },
      "ragged sort handle became successful zero data");

  const auto empty = exec::MakeBatch("legacy-empty-descriptor", {});
  RequireInvalidHandle(
      [&] { return exec::ProjectColumns(empty, {0}); },
      "empty legacy batch cannot prove a projection handle");
  RequireInvalidHandle(
      [&] { return exec::SortByColumn(empty, 0, true); },
      "empty legacy batch cannot prove a sort handle");
  RequireInvalidHandle(
      [&] { return exec::FilterGreaterThan(empty, 0, 0); },
      "empty legacy batch cannot prove an expression handle");

  if (input.rows.front().values != std::vector<std::int64_t>({7, 0, 70})) {
    Fail("handle validation or execution mutated the input batch");
  }
  return 0;
}
