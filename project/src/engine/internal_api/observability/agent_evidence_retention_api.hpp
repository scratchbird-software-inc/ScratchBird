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

// SEARCH_KEY: PFAR_016A_AGENT_EVIDENCE_AUDIT_REDACTION_RETENTION
// SEARCH_KEY: AUDIT_INTEGRITY_CHAIN
// Engine-owned evidence redaction and retention decision surface. Parser,
// client, listener, and manager code may render these rows, but they do not
// own evidence, security, transaction, catalog UUID, or retention authority.

struct EngineAgentEvidenceAuditRetentionRecord {
  std::string source_surface;
  std::string agent_type_id;
  std::string agent_uuid;
  std::string filespace_uuid;
  std::string policy_uuid;
  std::string evidence_uuid;
  std::string action_uuid;
  std::string actor_uuid;
  std::string evidence_kind;
  std::string result_state;
  std::string diagnostic_code;
  std::string retention_class;
  std::string retention_policy_ref;
  std::string retention_deadline;
  std::string policy_generation;
  std::string reason_text;
  std::string policy_body;
  std::string physical_path;
  std::string raw_principal;
  std::string raw_evidence_body;
  std::string support_bundle_payload;
  bool evidence_write_available = true;
  bool evidence_recoverable = false;
  bool retention_policy_installed = true;
  bool retention_policy_valid = true;
  bool legal_hold = false;
  bool maintenance_hold = false;
  bool retention_deadline_expired = false;
  bool actor_visible = false;
  bool policy_body_visible = false;
  bool cluster_scoped = false;
};

struct EngineEvaluateAgentEvidenceRetentionRequest : EngineApiRequest {
  std::vector<EngineAgentEvidenceAuditRetentionRecord> records;
  bool admin_view = false;
  bool sysarch_view = false;
};

struct EngineEvaluateAgentEvidenceRetentionResult : EngineApiResult {
  bool evidence_before_success_enforced = false;
  bool retention_decision_recorded = false;
  bool redaction_applied = false;
  bool pending_evidence = false;
  bool purge_eligible = false;
};

EngineEvaluateAgentEvidenceRetentionResult EngineEvaluateAgentEvidenceRetention(
    const EngineEvaluateAgentEvidenceRetentionRequest& request);

}  // namespace scratchbird::engine::internal_api
