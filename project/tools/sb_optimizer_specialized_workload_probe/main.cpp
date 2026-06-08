// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "logical_plan.hpp"
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
  opt::OptimizerEvidence vector_evidence;
  vector_evidence.specialized_kind = "vector";
  vector_evidence.exact_fallback_available = true;
  planner::QueryShapeEvidence document_evidence;
  document_evidence.shape = planner::QueryShapeKind::kSpecializedWorkload;
  document_evidence.specialized_kind = "document";
  opt::OptimizerEvidence search_evidence;
  search_evidence.specialized_kind = "search";
  const auto vector = opt::ChooseSpecializedWorkloadAccess(vector_evidence);
  const auto document_plan = planner::BuildQueryShapePlan(document_evidence);
  const auto search = opt::ChooseSpecializedWorkloadAccess(search_evidence);
  Expect(vector.access_kind == planner::PhysicalAccessKind::kVectorApproximateWithFallback, "vector workload choice mismatch", &errors);
  Expect(vector.llvm_eligible && vector.gpu_eligible, "vector workload should mark LLVM/GPU eligibility only", &errors);
  Expect(document_plan.nodes.front().access_kind == planner::PhysicalAccessKind::kDocumentPathProbe, "document specialized plan mismatch", &errors);
  Expect(search.access_kind == planner::PhysicalAccessKind::kFullTextProbe, "search workload choice mismatch", &errors);
  return Finish(errors);
}
