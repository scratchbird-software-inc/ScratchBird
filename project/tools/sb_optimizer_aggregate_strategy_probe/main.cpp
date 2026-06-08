// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "optimizer_contract.hpp"
#include <iostream>
#include <string>
#include <string_view>
#include <vector>
namespace opt = scratchbird::engine::optimizer;
namespace planner = scratchbird::engine::planner;
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
  opt::OptimizerEvidence grouped;
  grouped.grouping_present = true;
  opt::OptimizerEvidence ungrouped;
  ungrouped.grouping_present = false;
  Expect(opt::ChooseAggregateStrategy(grouped).access_kind == planner::PhysicalAccessKind::kAggregateHash, "grouped aggregate should choose hash aggregate", &errors);
  Expect(opt::ChooseAggregateStrategy(ungrouped).access_kind == planner::PhysicalAccessKind::kAggregateGeneric, "ungrouped aggregate should choose generic aggregate", &errors);
  return Finish(errors);
}
