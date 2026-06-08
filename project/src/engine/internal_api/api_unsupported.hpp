// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "api_diagnostics.hpp"

#include <string>
#include <utility>

namespace scratchbird::engine::internal_api {

// SEARCH_KEY: SB_ENGINE_INTERNAL_API_UNSUPPORTED_HELPERS

template <typename TResult, typename TRequest>
TResult MakeUnavailableResult(const TRequest& request, std::string operation_id) {
  TResult result;
  result.ok = false;
  result.operation_id = operation_id;
  result.diagnostics.push_back(MakeUnavailableDiagnostic(operation_id));
  if (request.context.trust_mode == EngineTrustMode::embedded_in_process) {
    result.embedded_trust_mode_observed = true;
    result.diagnostics.push_back(MakeEmbeddedTrustModeDiagnostic(operation_id));
  }
  return result;
}

template <typename TResult, typename TRequest>
TResult MakeClusterAuthorityUnavailableResult(const TRequest& request, std::string operation_id) {
  TResult result;
  result.ok = false;
  result.operation_id = operation_id;
  result.cluster_authority_required = true;
  result.diagnostics.push_back(MakeClusterAuthorityUnavailableDiagnostic(operation_id));
  result.evidence.push_back({"cluster_mapping", "unavailable"});
  result.evidence.push_back({"cluster_authority", "unavailable"});
  result.evidence.push_back({"cluster_command", operation_id});
  if (request.context.trust_mode == EngineTrustMode::embedded_in_process) {
    result.embedded_trust_mode_observed = true;
    result.diagnostics.push_back(MakeEmbeddedTrustModeDiagnostic(operation_id));
  }
  return result;
}

template <typename TResult, typename TRequest>
TResult MakeInvalidRequestResult(const TRequest& request, std::string operation_id, std::string detail) {
  TResult result;
  result.ok = false;
  result.operation_id = operation_id;
  result.diagnostics.push_back(MakeInvalidRequestDiagnostic(operation_id, std::move(detail)));
  if (request.context.trust_mode == EngineTrustMode::embedded_in_process) {
    result.embedded_trust_mode_observed = true;
    result.diagnostics.push_back(MakeEmbeddedTrustModeDiagnostic(operation_id));
  }
  return result;
}

}  // namespace scratchbird::engine::internal_api
