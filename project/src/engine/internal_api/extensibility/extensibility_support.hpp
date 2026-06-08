// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "behavior_support/api_behavior_store.hpp"

#include <string>

namespace scratchbird::engine::internal_api {

// SEARCH_KEY: SB_ENGINE_INTERNAL_API_EXTENSIBILITY_SUPPORT
// Shared helpers for extension-command authority checks. Extensions may add
// behavior, but they cannot bypass security, UUID/catalog identity, SBLR, MGA,
// transaction, or engine-owned execution authority.

inline bool EngineExtensionOptionContains(const EngineApiRequest& request, const std::string& token) {
  for (const auto& option : request.option_envelopes) {
    if (option.find(token) != std::string::npos) { return true; }
  }
  return false;
}

inline bool EngineExtensionRequestsClusterAuthority(const EngineApiRequest& request) {
  return EngineExtensionOptionContains(request, "cluster") ||
         EngineExtensionOptionContains(request, "distributed") ||
         EngineExtensionOptionContains(request, "remote_deploy") ||
         EngineExtensionOptionContains(request, "global_deploy");
}

inline bool EngineExtensionRequestsControl(const EngineApiRequest& request) {
  return EngineExtensionOptionContains(request, "enable") ||
         EngineExtensionOptionContains(request, "execute") ||
         EngineExtensionOptionContains(request, "dispatch") ||
         EngineExtensionOptionContains(request, "install") ||
         EngineExtensionOptionContains(request, "register") ||
         EngineExtensionOptionContains(request, "load") ||
         EngineExtensionOptionContains(request, "jit") ||
         EngineExtensionOptionContains(request, "aot") ||
         EngineExtensionOptionContains(request, "compile");
}

template <typename TResult>
TResult EngineExtensionSecurityRequired(const EngineApiRequest& request, const std::string& operation_id) {
  return MakeApiBehaviorDiagnostic<TResult>(
      request.context,
      operation_id,
      MakeSecurityContextRequiredDiagnostic(operation_id));
}

template <typename TResult>
TResult EngineExtensionClusterAuthorityUnavailable(const EngineApiRequest& request, const std::string& operation_id) {
  auto result = MakeApiBehaviorDiagnostic<TResult>(
      request.context,
      operation_id,
      MakeClusterAuthorityUnavailableDiagnostic(operation_id));
  result.cluster_authority_required = true;
  return result;
}

inline void AddEngineExtensionEvidence(EngineApiResult* result,
                                       const std::string& extension_family,
                                       const std::string& behavior) {
  AddApiBehaviorEvidence(result, "extension_family", extension_family);
  AddApiBehaviorEvidence(result, "extension_behavior", behavior);
}

}  // namespace scratchbird::engine::internal_api
