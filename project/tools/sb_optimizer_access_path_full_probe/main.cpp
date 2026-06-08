// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "access_path_full.hpp"

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
  opt::OptimizerStatsIdentity identity{"rel:1", "idxstat:1", 1, 1, 1, opt::OptimizerStatsFreshnessState::kFresh, opt::StatisticSource::kCatalogExact, opt::CostConfidence::kExact};
  opt::IndexStats index;
  index.identity = identity;
  index.index_uuid = "idx:1";
  index.relation_uuid = "rel:1";
  index.unique = true;
  index.covering = true;
  index.height = 2;
  index.leaf_pages = 2;
  index.distinct_keys = 100;
  opt::AccessPathPlanningRequest request;
  request.relation_uuid = "rel:1";
  request.predicate_kind = "unique_eq";
  request.descriptor_digest = "desc:v1";
  request.projected_column_uuids = {"col:1"};
  request.visibility_proven = true;
  request.grants_proven = true;
  request.index_visibility_native = true;
  request.candidate_indexes = {index};
  auto candidates = opt::GenerateFullAccessPathCandidates(request);
  bool saw_index = false;
  bool saw_covering = false;
  for (const auto& candidate : candidates) {
    saw_index = saw_index || candidate.access_kind == planner::PhysicalAccessKind::kScalarBtreeLookup;
    saw_covering = saw_covering || candidate.access_kind == planner::PhysicalAccessKind::kCoveringIndexScan;
  }
  Expect(saw_index, "index lookup candidate", &errors);
  Expect(saw_covering, "covering index candidate", &errors);
  Expect(!opt::BuildPhysicalAccessNodes(candidates, "desc:v1").empty(), "physical nodes built", &errors);
  return Finish(errors);
}
