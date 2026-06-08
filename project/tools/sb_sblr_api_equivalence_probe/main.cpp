// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "observability/show_api.hpp"
#include "sblr_dispatch.hpp"

#include <iostream>

namespace {

scratchbird::engine::internal_api::EngineRequestContext Context() {
  scratchbird::engine::internal_api::EngineRequestContext context;
  context.database_path = "/tmp/sb_sblr_equivalence_probe.sbdb";
  context.database_uuid.canonical = "018f0000-0000-7000-8000-000000000201";
  context.principal_uuid.canonical = "018f0000-0000-7000-8000-000000000202";
  context.session_uuid.canonical = "018f0000-0000-7000-8000-000000000203";
  context.security_context_present = true;
  return context;
}

}  // namespace

int main() {
  namespace api = scratchbird::engine::internal_api;
  namespace sblr = scratchbird::engine::sblr;
  api::EngineShowVersionRequest direct_request;
  direct_request.context = Context();
  direct_request.operation_id = "observability.show_version";
  const auto direct = api::EngineShowVersion(direct_request);

  sblr::SblrDispatchRequest dispatch_request;
  dispatch_request.context = Context();
  dispatch_request.envelope = sblr::MakeSblrEnvelope("observability.show_version", "show.version", "TRACE-SBLR-EQUIVALENCE");
  const auto dispatched = sblr::DispatchSblrOperation(dispatch_request);

  const bool ok = direct.ok && dispatched.api_result.ok &&
                  direct.operation_id == dispatched.api_result.operation_id &&
                  direct.result_shape.rows.size() == dispatched.api_result.result_shape.rows.size();
  std::cout << "{\"ok\":" << (ok ? "true" : "false")
            << ",\"direct_rows\":" << direct.result_shape.rows.size()
            << ",\"dispatch_rows\":" << dispatched.api_result.result_shape.rows.size() << "}\n";
  return ok ? 0 : 1;
}
