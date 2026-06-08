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

// SEARCH_KEY: SB_ENGINE_INTERNAL_API_SECURITY_EXTERNAL_GROUP_API
struct EngineSyncExternalGroupsRequest : EngineApiRequest {
  std::string provider_family;
};
struct EngineSyncExternalGroupsResult : EngineApiResult {
  bool materialized = false;
};
EngineSyncExternalGroupsResult EngineSyncExternalGroups(const EngineSyncExternalGroupsRequest& request);

struct EngineExplainMembershipRequest : EngineApiRequest {
  std::string provider_family;
};
struct EngineExplainMembershipResult : EngineApiResult {
  bool explainable = false;
};
EngineExplainMembershipResult EngineExplainMembership(const EngineExplainMembershipRequest& request);

}  // namespace scratchbird::engine::internal_api
