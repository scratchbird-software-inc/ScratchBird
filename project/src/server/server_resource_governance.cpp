// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "server_resource_governance.hpp"

namespace scratchbird::server {
namespace agents = scratchbird::core::agents;

ServerResourceGovernanceResult AdmitServerResourceGovernance(
    const ServerResourceGovernanceContext& context,
    agents::ResourceGovernanceAdmissionRequest request) {
  ServerResourceGovernanceResult result;
  result.evidence.push_back("server.resource_governance.route=odf106");
  result.evidence.push_back("server.resource_governance.engine_runtime_bound=" +
                            std::string(context.engine_runtime_bound ? "true"
                                                                     : "false"));
  result.evidence.push_back(
      "server.resource_governance.security_context_present=" +
      std::string(context.security_context_present ? "true" : "false"));
  result.evidence.push_back(
      "server.resource_governance.parser_or_client_authority=false");
  result.evidence.push_back(
      "server.resource_governance.mga_finality_authority=engine_transaction_inventory");
  result.evidence.push_back("server.resource_governance.principal=redacted");

  if (!context.engine_runtime_bound || !context.security_context_present ||
      context.parser_or_client_authority) {
    result.ok = false;
    result.fail_closed = true;
    result.diagnostic_code =
        "SB_SERVER_RESOURCE_GOVERNANCE.CONTEXT_REFUSED";
    return result;
  }

  request.descriptor.source =
      agents::ResourceGovernanceDescriptorSource::kServerRuntimeApi;
  result.admission = agents::AdmitResourceGovernance(request);
  result.ok = result.admission.ok;
  result.fail_closed = result.admission.fail_closed;
  result.diagnostic_code = result.admission.diagnostic_code;
  result.evidence.insert(result.evidence.end(), result.admission.evidence.begin(),
                         result.admission.evidence.end());
  return result;
}

}  // namespace scratchbird::server
