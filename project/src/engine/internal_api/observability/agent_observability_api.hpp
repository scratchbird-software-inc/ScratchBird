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
#include <vector>

namespace scratchbird::engine::internal_api {

// SEARCH_KEY: PFAR_016_AGENT_METRICS_AUDIT_DIAGNOSTICS_SUPPORT_BUNDLE
// Engine-owned collector for agent/storage runtime evidence. Parser, listener,
// manager, and client surfaces may render these rows, but they do not own
// metric, audit, diagnostic, support-bundle, catalog UUID, or MGA authority.

struct EngineAgentRuntimeEvidenceRecord {
  std::string source_surface;
  std::string agent_type_id;
  std::string agent_uuid;
  std::string filespace_uuid;
  std::string policy_uuid;
  std::string evidence_uuid;
  std::string action_id;
  std::string evidence_kind;
  std::string result_state;
  std::string diagnostic_code;
  std::string payload_digest;
  std::string redaction_class = "summary";
  std::string physical_path;
  std::string raw_principal;
  std::string unsafe_payload;
  bool payload_redacted = true;
};

struct EngineCollectAgentRuntimeObservabilityRequest : EngineApiRequest {
  std::vector<EngineAgentRuntimeEvidenceRecord> records;
};

struct EngineCollectAgentRuntimeObservabilityResult : EngineApiResult {
  bool metrics_recorded = false;
  bool audit_recorded = false;
  bool diagnostics_rendered = false;
  bool support_bundle_ready = false;
  bool redaction_applied = false;
};

EngineCollectAgentRuntimeObservabilityResult EngineCollectAgentRuntimeObservability(
    const EngineCollectAgentRuntimeObservabilityRequest& request);

}  // namespace scratchbird::engine::internal_api
