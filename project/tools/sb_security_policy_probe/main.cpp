// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "security/policy_api.hpp"

#include <iostream>

using namespace scratchbird::engine::internal_api;

namespace {
EngineRequestContext Context(std::initializer_list<const char*> tags) {
  EngineRequestContext context;
  context.security_context_present = true;
  for (const char* tag : tags) { context.trace_tags.emplace_back(tag); }
  return context;
}
}

int main() {
  EngineEvaluatePolicyRequest allow;
  allow.context = Context({"right:POLICY_ADMIN"});
  allow.target_object.uuid.canonical = "018f0000-0000-7000-8000-00000000e001";
  allow.policy_profile.encoded_profiles.push_back("policy:explicit");
  const auto allow_result = EngineEvaluatePolicy(allow);

  EngineEvaluatePolicyRequest invalid;
  invalid.context = Context({"right:POLICY_ADMIN"});
  invalid.policy_profile.encoded_profiles.push_back("policy:unsafe");
  const auto invalid_result = EngineEvaluatePolicy(invalid);

  EngineEvaluatePolicyRequest missing_context;
  missing_context.target_object.uuid.canonical = "018f0000-0000-7000-8000-00000000e002";
  const auto missing_result = EngineEvaluatePolicy(missing_context);

  const bool ok = allow_result.ok && !invalid_result.ok && !missing_result.ok;
  std::cout << "{\"ok\":" << (ok ? "true" : "false")
            << ",\"allow\":" << (allow_result.ok ? "true" : "false")
            << ",\"invalid_denied\":" << (!invalid_result.ok ? "true" : "false")
            << ",\"missing_context_denied\":" << (!missing_result.ok ? "true" : "false") << "}\n";
  return ok ? 0 : 1;
}
