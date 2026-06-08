// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once
#include "api_types.hpp"
namespace scratchbird::engine::internal_api {
// SEARCH_KEY: SB_ENGINE_INTERNAL_API_DIAGNOSTICS
EngineApiDiagnostic MakeEngineApiDiagnostic(std::string code, std::string message_key, std::string detail, bool error = true);
EngineApiDiagnostic MakeUnavailableDiagnostic(std::string operation_id);
EngineApiDiagnostic MakeUnsupportedProfileDiagnostic(std::string operation_id, std::string profile);
EngineApiDiagnostic MakeClusterAuthorityUnavailableDiagnostic(std::string operation_id);
EngineApiDiagnostic MakeSecurityContextRequiredDiagnostic(std::string operation_id);
EngineApiDiagnostic MakeInvalidRequestDiagnostic(std::string operation_id, std::string detail);
EngineApiDiagnostic MakeEmbeddedTrustModeDiagnostic(std::string operation_id);
}
