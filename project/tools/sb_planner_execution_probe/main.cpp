// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "rule_planner.hpp"
#include "sblr_dispatch.hpp"

#include <iostream>

namespace {

scratchbird::engine::internal_api::EngineRequestContext Context() {
  scratchbird::engine::internal_api::EngineRequestContext context;
  context.database_path = "/tmp/sb_planner_execution_probe.sbdb";
  context.database_uuid.canonical = "018f0000-0000-7000-8000-000000000301";
  context.principal_uuid.canonical = "018f0000-0000-7000-8000-000000000302";
  context.session_uuid.canonical = "018f0000-0000-7000-8000-000000000303";
  context.security_context_present = true;
  return context;
}

}  // namespace

int main() {
  namespace planner = scratchbird::engine::planner;
  namespace sblr = scratchbird::engine::sblr;
  planner::RulePlannerInput input;
  input.envelope = sblr::MakeSblrEnvelope("observability.show_metrics", "show.metrics", "TRACE-PLANNER-EXECUTION");
  const auto plan = planner::BuildDeterministicLogicalPlan(input);

  sblr::SblrDispatchRequest dispatch;
  dispatch.context = Context();
  dispatch.envelope = input.envelope;
  const auto result = sblr::DispatchSblrOperation(dispatch);

  const bool ok = plan.ok && result.accepted && result.api_result.ok;
  std::cout << "{\"ok\":" << (ok ? "true" : "false")
            << ",\"plan_nodes\":" << plan.nodes.size()
            << ",\"api_ok\":" << (result.api_result.ok ? "true" : "false") << "}\n";
  return ok ? 0 : 1;
}
