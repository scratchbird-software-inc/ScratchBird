// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "logical_plan.hpp"
#include "rule_planner.hpp"

#include <concepts>
#include <fstream>
#include <iterator>
#include <string>

namespace {

using scratchbird::engine::planner::CanonicalLogicalRelationalNode;

template <typename T>
concept HasPhysicalAccessSelection = requires(T node) {
  node.access_kind;
};

int Fail(const int line) { return line; }

}  // namespace

// QOW-TEST-QRY-030-V1
int main() {
  using namespace scratchbird::engine::planner;

  static_assert(kOperationPrefixRulePlannerRemoved);
  static_assert(!HasPhysicalAccessSelection<CanonicalLogicalRelationalNode>);

  std::ifstream input(SB_QOW_RULE_PLANNER_SOURCE_FILE);
  if (!input) return Fail(__LINE__);
  const std::string source((std::istreambuf_iterator<char>(input)),
                           std::istreambuf_iterator<char>());

  if (source.find("QOW-SOURCE-QRY-030-V1") == std::string::npos) {
    return Fail(__LINE__);
  }
  constexpr const char* kForbiddenLegacyTokens[] = {
      "Starts" "With(",
      "Access" "ForPredicate(",
      "Build" "DeterministicLogicalPlan(",
      "input.envelope.operation_id",
      "input.api_request.predicate",
  };
  for (const char* token : kForbiddenLegacyTokens) {
    if (source.find(token) != std::string::npos) return Fail(__LINE__);
  }
  return 0;
}
