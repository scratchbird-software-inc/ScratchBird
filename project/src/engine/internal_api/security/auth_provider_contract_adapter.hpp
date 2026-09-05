// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "api_types.hpp"

#include <string>
#include <utility>
#include <vector>

namespace scratchbird::engine::internal_api {

// SEARCH_KEY: SB_ENGINE_INTERNAL_API_AUTH_PROVIDER_CONTRACT_ADAPTER
// Validates only the bounded syntax of an untrusted provider request envelope.
// This type deliberately carries no authentication decision or principal.
// Authentication requires a separate opaque result from a trusted provider
// implementation; no such production provider boundary is registered yet.
struct AuthProviderContractEvidenceResult {
  bool evaluated = false;
  bool ok = false;
  std::string provider_family;
  EngineApiDiagnostic diagnostic;
  std::vector<std::pair<std::string, std::string>> rows;
  std::vector<EngineEvidenceReference> evidence;
};

AuthProviderContractEvidenceResult ValidateAuthProviderContractEvidence(
    const EngineApiRequest& request);

}  // namespace scratchbird::engine::internal_api
