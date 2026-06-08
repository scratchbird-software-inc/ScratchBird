// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "api_unsupported.hpp"

#include <utility>

namespace scratchbird::engine::internal_api {

EngineApiDiagnostic MakeEngineApiDiagnostic(std::string code, std::string message_key, std::string detail, bool error) {
  EngineApiDiagnostic diagnostic;
  diagnostic.code = std::move(code);
  diagnostic.message_key = std::move(message_key);
  diagnostic.detail = std::move(detail);
  diagnostic.error = error;
  return diagnostic;
}

EngineApiDiagnostic MakeUnavailableDiagnostic(std::string operation_id) {
  return MakeEngineApiDiagnostic("SB_ENGINE_API_UNAVAILABLE",
                                 "engine.api.unavailable",
                                 std::move(operation_id));
}

EngineApiDiagnostic MakeUnsupportedProfileDiagnostic(std::string operation_id, std::string profile) {
  return MakeEngineApiDiagnostic("SB_ENGINE_API_UNSUPPORTED_PROFILE",
                                 "engine.api.unsupported_profile",
                                 operation_id + ":" + profile);
}

EngineApiDiagnostic MakeClusterAuthorityUnavailableDiagnostic(std::string operation_id) {
  return MakeEngineApiDiagnostic("SB_ENGINE_API_CLUSTER_AUTHORITY_UNAVAILABLE",
                                 "engine.api.cluster_authority_unavailable",
                                 std::move(operation_id));
}

EngineApiDiagnostic MakeSecurityContextRequiredDiagnostic(std::string operation_id) {
  return MakeEngineApiDiagnostic("SB_ENGINE_API_SECURITY_CONTEXT_REQUIRED",
                                 "engine.api.security_context_required",
                                 std::move(operation_id));
}

EngineApiDiagnostic MakeInvalidRequestDiagnostic(std::string operation_id, std::string detail) {
  return MakeEngineApiDiagnostic("SB_ENGINE_API_INVALID_REQUEST",
                                 "engine.api.invalid_request",
                                 operation_id + ":" + detail);
}

EngineApiDiagnostic MakeEmbeddedTrustModeDiagnostic(std::string operation_id) {
  return MakeEngineApiDiagnostic("SB_ENGINE_API_EMBEDDED_TRUST_MODE",
                                 "engine.api.embedded_trust_mode",
                                 std::move(operation_id),
                                 false);
}

}  // namespace scratchbird::engine::internal_api
