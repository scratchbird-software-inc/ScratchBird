// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "extensibility/parser_package_api.hpp"

#include "behavior_support/api_behavior_store.hpp"
#include "extensibility/extensibility_support.hpp"

namespace scratchbird::engine::internal_api {

// SEARCH_KEY: SB_ENGINE_INTERNAL_API_EXTENSIBILITY_PARSER_PACKAGE_API_BEHAVIOR
EngineRegisterParserPackageResult EngineRegisterParserPackage(const EngineRegisterParserPackageRequest& request) {
  constexpr const char* kOperation = "extensibility.register_parser_package";
  if (!request.context.security_context_present) {
    return EngineExtensionSecurityRequired<EngineRegisterParserPackageResult>(request, kOperation);
  }
  if (!request.context.cluster_authority_available && EngineExtensionRequestsClusterAuthority(request)) {
    return EngineExtensionClusterAuthorityUnavailable<EngineRegisterParserPackageResult>(request, kOperation);
  }
  auto result = PersistedRecordResult<EngineRegisterParserPackageResult>(request, kOperation, "parser_package", true, "registered");
  if (result.ok) {
    AddEngineExtensionEvidence(&result, "parser_package", "untrusted_translation_package_registration");
    AddApiBehaviorEvidence(&result, "parser_trust_boundary", "untrusted_per_connection");
    AddApiBehaviorEvidence(&result, "engine_mutation_authority", "false");
  }
  return result;
}

}  // namespace scratchbird::engine::internal_api
