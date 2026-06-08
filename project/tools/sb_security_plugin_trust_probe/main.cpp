// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "security/plugin_trust_api.hpp"

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
  EngineEvaluateUdrTrustRequest udr;
  udr.context = Context({"right:UDR_TRUST_ADMIN"});
  udr.option_envelopes.push_back("signature_valid:true");
  udr.option_envelopes.push_back("provenance_valid:true");
  const auto udr_result = EngineEvaluateUdrTrust(udr);

  EngineEvaluateManagerAdmissionRequest manager;
  manager.context = Context({"right:MANAGER_ADMISSION_ADMIN"});
  manager.option_envelopes.push_back("identity_valid:true");
  manager.option_envelopes.push_back("key_valid:true");
  manager.option_envelopes.push_back("expected_member:true");
  const auto manager_result = EngineEvaluateManagerAdmission(manager);

  EngineEvaluateUdrTrustRequest denied;
  denied.context = Context({"right:UDR_TRUST_ADMIN"});
  denied.option_envelopes.push_back("signature_valid:false");
  denied.option_envelopes.push_back("provenance_valid:true");
  const auto denied_result = EngineEvaluateUdrTrust(denied);

  const bool ok = udr_result.ok && udr_result.admitted && manager_result.ok && manager_result.admitted && !denied_result.ok;
  std::cout << "{\"ok\":" << (ok ? "true" : "false")
            << ",\"udr\":" << (udr_result.admitted ? "true" : "false")
            << ",\"manager\":" << (manager_result.admitted ? "true" : "false")
            << ",\"denied\":" << (!denied_result.ok ? "true" : "false") << "}\n";
  return ok ? 0 : 1;
}
