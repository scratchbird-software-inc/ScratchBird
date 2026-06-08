// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "specialized_planner.hpp"

#include <iostream>
#include <string>
#include <vector>

namespace opt = scratchbird::engine::optimizer;
namespace planner = scratchbird::engine::planner;
namespace {
void Expect(bool condition, const char* message, std::vector<std::string>* errors) { if (!condition) errors->push_back(message); }
int Finish(const std::vector<std::string>& errors) { std::cout << "{\"ok\":" << (errors.empty() ? "true" : "false") << ",\"failure_count\":" << errors.size() << "}\n"; return errors.empty() ? 0 : 1; }
}
int main() {
  std::vector<std::string> errors;
  opt::SpecializedProviderCapability ready{"vector", true, true, true, true, true, 100};
  auto vector = opt::PlanVectorCandidate(ready);
  Expect(vector.cost.selectable && vector.access_kind == planner::PhysicalAccessKind::kVectorApproximateWithFallback, "vector candidate selectable", &errors);
  opt::SpecializedProviderCapability blocked{"search", false, true, false, true, true, 100};
  auto search = opt::PlanSearchCandidate(blocked);
  Expect(!search.cost.selectable && search.cost.rejection_reason == "local_provider_unavailable", "missing provider refused", &errors);
  Expect(opt::PlanAllSpecializedFamilyCandidates({ready, blocked}).size() == 2, "all specialized candidates planned", &errors);
  return Finish(errors);
}
