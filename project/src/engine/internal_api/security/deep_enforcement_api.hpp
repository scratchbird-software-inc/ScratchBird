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

namespace scratchbird::engine::internal_api {

// SEARCH_KEY: SB_ENGINE_INTERNAL_API_DEEP_SECURITY_ENFORCEMENT_API
// Unified engine-owned security enforcement decision for executor/storage/catalog
// callers that need one authority point for rights, discovery visibility,
// masking/RLS, UDR invocation, audit-before-success, and refusal side-effect
// behavior. This API is not a parser policy hook.

struct EngineEvaluateDeepSecurityRequest : EngineApiRequest {
  std::string phase = "executor";
  std::string required_right = "SELECT";
  bool mutation = false;
  bool require_audit_before_success = false;
};

struct EngineEvaluateDeepSecurityResult : EngineApiResult {
  bool admitted = false;
  bool authorized = false;
  bool visible = false;
  bool masked = false;
  bool rls_applied = false;
  bool audit_written = false;
  bool side_effect_permitted = false;
  std::string decision;
};

EngineEvaluateDeepSecurityResult EngineEvaluateDeepSecurity(const EngineEvaluateDeepSecurityRequest& request);

}  // namespace scratchbird::engine::internal_api
