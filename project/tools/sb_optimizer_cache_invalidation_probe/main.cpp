// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "optimizer_plan_cache.hpp"

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
  opt::OptimizerPlanCacheKeyInput key_input;
  key_input.operation_id = "dml.select_rows";
  key_input.sblr_digest = "sblr";
  key_input.descriptor_set_digest = "desc";
  key_input.statistics_snapshot_id = "stats";
  key_input.cost_profile_id = "cost";
  key_input.executor_capability_set_id = "exec";
  key_input.catalog_epoch = 1;
  key_input.security_epoch = 1;
  key_input.policy_epoch = 1;
  key_input.object_uuids = {"rel:1"};
  auto key = opt::BuildOptimizerPlanCacheKey(key_input);
  opt::CachedOptimizerPlan cached;
  cached.cache_key = key;
  cached.created_epoch = 1;
  opt::OptimizerPlanCache cache;
  cache.Put(cached);
  Expect(cache.Get(key).has_value(), "cache hit", &errors);
  Expect(cache.Invalidate({"object", "rel:1", 2}) == 1, "object invalidation", &errors);
  Expect(!cache.Get(key).has_value(), "invalidated plan miss", &errors);
  return Finish(errors);
}
