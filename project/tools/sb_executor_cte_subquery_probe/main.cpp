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
  const auto input = MakeBatch("rel", {{{42, 1}}, {{99, 2}}});
  const auto materialized = MaterializeCte(input);
  Expect(materialized.rows.size() == input.rows.size(), "CTE materialization row count mismatch", &errors);
  Expect(materialized.descriptor_digest.find("materialized") != std::string::npos, "CTE materialization descriptor marker missing", &errors);
  Expect(ScalarSubqueryFirstValue(materialized, 0) == 42, "scalar subquery first value mismatch", &errors);
  return Finish(errors);
}
