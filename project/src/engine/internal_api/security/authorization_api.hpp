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

// SEARCH_KEY: SB_ENGINE_INTERNAL_API_SECURITY_AUTHORIZATION_API
struct EngineAuthorizeRequest : EngineApiRequest {
  std::string required_right;
  bool require_cluster_authority = false;
};
struct EngineAuthorizeResult : EngineApiResult {
  bool authorized = false;
  bool policy_recheck_required = false;
  std::string decision;
  std::vector<std::string> policy_recheck_reasons;
};
EngineAuthorizeResult EngineAuthorize(const EngineAuthorizeRequest& request);

}  // namespace scratchbird::engine::internal_api
