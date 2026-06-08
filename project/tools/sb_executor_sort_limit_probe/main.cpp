// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "executor_foundation.hpp"
#include <iostream>
#include <string>
#include <string_view>
#include <vector>
using namespace scratchbird::engine::executor;
static std::string JsonEscape(std::string_view text) {
  std::string out;
  for (char ch : text) {
    switch (ch) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      default: out += ch; break;
    }
  }
  return out;
}
static void Expect(bool condition, std::string message, std::vector<std::string>* errors) {
  if (!condition) errors->push_back(std::move(message));
}
static int Finish(const std::vector<std::string>& errors) {
  std::cout << "{\n  \"ok\": " << (errors.empty() ? "true" : "false") << ",\n  \"errors\": [";
  for (std::size_t i = 0; i < errors.size(); ++i) {
    if (i != 0) std::cout << ", ";
    std::cout << '"' << JsonEscape(errors[i]) << '"';
  }
  std::cout << "]\n}\n";
  return errors.empty() ? 0 : 1;
}
int main() {
  std::vector<std::string> errors;
  const auto input = MakeBatch("rel", {{{1, 30}}, {{2, 10}}, {{3, 20}}, {{4, 40}}});
  const auto sorted = SortByColumn(input, 1, true);
  Expect(sorted.rows[0].values[1] == 10, "ascending sort first row mismatch", &errors);
  const auto limited = LimitOffset(sorted, 2, 1);
  Expect(limited.rows.size() == 2, "limit/offset should return two rows", &errors);
  Expect(limited.rows[0].values[1] == 20 && limited.rows[1].values[1] == 30, "limit/offset row values mismatch", &errors);
  return Finish(errors);
}
