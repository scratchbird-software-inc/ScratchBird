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

// SEARCH_KEY: SB_ENGINE_INTERNAL_API_SECURITY_AUDIT_API
// SEARCH_KEY: AUDIT_RETENTION_PRIVACY
struct EngineEmitAuditEventRequest : EngineApiRequest {
  std::string event_class;
  std::string outcome;
};
struct EngineEmitAuditEventResult : EngineApiResult {
  bool emitted = false;
  bool redacted = false;
};
EngineEmitAuditEventResult EngineEmitAuditEvent(const EngineEmitAuditEventRequest& request);

struct EngineEmitLifecycleAuditEventRequest : EngineApiRequest {
  std::string operation_key;
  std::string outcome;
  std::string diagnostic_code;
  std::string correlation_uuid;
  std::string cache_marker_uuid;
  bool cache_invalidation_recorded = false;
};
struct EngineEmitLifecycleAuditEventResult : EngineApiResult {
  bool emitted = false;
  bool redacted = true;
  bool cache_marker_linked = false;
};
EngineEmitLifecycleAuditEventResult EngineEmitLifecycleAuditEvent(
    const EngineEmitLifecycleAuditEventRequest& request);

}  // namespace scratchbird::engine::internal_api
