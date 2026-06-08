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
  Expect(EvalAdd(2, 3) == 5, "EvalAdd failed", &errors);
  Expect(EvalMultiply(7, 6) == 42, "EvalMultiply failed", &errors);
  Expect(ValidateBatch(MakeBatch("descriptor", {{{1, 2}}, {{3, 4}}})).ok, "valid descriptor batch rejected", &errors);
  Expect(!ValidateBatch(MakeBatch("", {{{1}}})).ok, "missing descriptor batch accepted", &errors);
  std::vector<std::string> catalog_errors;
  Expect(ValidateOperatorCatalog(Stage6OperatorCatalog(), &catalog_errors), "operator catalog should validate", &errors);
  Expect(Stage6OperatorCatalog().size() == 23, "operator catalog should contain 23 Stage 6 operators", &errors);
  for (const auto& error : catalog_errors) errors.push_back(error);
  return Finish(errors);
}
