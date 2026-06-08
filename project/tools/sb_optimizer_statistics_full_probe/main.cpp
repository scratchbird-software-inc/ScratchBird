// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "optimizer_statistics_full.hpp"

#include <iostream>
#include <string>
#include <vector>

namespace opt = scratchbird::engine::optimizer;
namespace {
void Expect(bool condition, const char* message, std::vector<std::string>* errors) { if (!condition) errors->push_back(message); }
int Finish(const std::vector<std::string>& errors) { std::cout << "{\"ok\":" << (errors.empty() ? "true" : "false") << ",\"failure_count\":" << errors.size() << "}\n"; return errors.empty() ? 0 : 1; }
}
int main() {
  std::vector<std::string> errors;
  opt::AnalyzeSampleInput sample{"rel:1", 100, 100, 4, 64, 1, 1};
  auto table = opt::BuildTableStatsFromAnalyzeSample(sample);
  opt::OptimizerStatisticsStore store;
  store.UpsertTable(table);
  Expect(store.FindTable("rel:1").has_value(), "table stats find", &errors);
  opt::ColumnStats column;
  column.identity = table.identity;
  column.identity.statistic_uuid = "rel:1:col:1";
  column.column_uuid = "col:1";
  column.descriptor_digest = "int64";
  column.distinct_count = 10;
  store.UpsertColumn(column);
  Expect(opt::EstimateEqualitySelectivityFromColumnStats(column, 100) <= 0.10, "equality selectivity", &errors);
  auto snapshot = store.Snapshot("snapshot:1");
  Expect(opt::ValidateOptimizerStatsSnapshot(snapshot).front().ok, "snapshot validates", &errors);
  store.MarkStaleByObject("rel:1", 2);
  Expect(!opt::OptimizerStatsIdentityIsUsable(store.FindTable("rel:1")->identity), "stale stats not usable", &errors);
  return Finish(errors);
}
