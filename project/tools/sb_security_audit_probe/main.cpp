// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "security/audit_api.hpp"

#include <filesystem>
#include <iostream>

using namespace scratchbird::engine::internal_api;

int main() {
  const std::string path = "/tmp/sb_security_audit_probe.sbdb";
  std::filesystem::remove(path);
  EngineRequestContext context;
  context.database_path = path;
  context.security_context_present = true;
  context.local_transaction_id = 7;

  EngineEmitAuditEventRequest request;
  request.context = context;
  request.event_class = "security.test";
  request.outcome = "allow";
  request.option_envelopes.push_back("redact:true");
  const auto emitted = EngineEmitAuditEvent(request);

  EngineEmitAuditEventRequest missing;
  missing.context = context;
  const auto missing_result = EngineEmitAuditEvent(missing);

  const bool ok = emitted.ok && emitted.emitted && emitted.redacted && !missing_result.ok;
  std::cout << "{\"ok\":" << (ok ? "true" : "false")
            << ",\"emitted\":" << (emitted.emitted ? "true" : "false")
            << ",\"redacted\":" << (emitted.redacted ? "true" : "false")
            << ",\"missing_rejected\":" << (!missing_result.ok ? "true" : "false") << "}\n";
  return ok ? 0 : 1;
}
