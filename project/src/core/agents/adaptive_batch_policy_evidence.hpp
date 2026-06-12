// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "agent_runtime.hpp"

#include <string>
#include <vector>

namespace scratchbird::core::agents {

// SEARCH_KEY: SB_AGENT_ADAPTIVE_BATCH_POLICY_EVIDENCE_ODF_100
// Agents provide workload, resource, backlog, and policy evidence only. The
// optimizer controller remains advisory/resource-governance only and agents do
// not own transaction finality, visibility, parser execution, client
// autocommit, provider, reference, or recovery authority.
struct AdaptiveBatchPolicyEvidence {
  std::string family_label;
  std::string policy_id = "adaptive_batch_policy_v1";
  u64 evidence_epoch = 0;
  u64 required_epoch = 0;
  u64 evidence_age_microseconds = 0;
  u64 max_evidence_age_microseconds = 60000000;
  u64 backlog_units = 0;
  u64 backlog_budget_units = 0;
  u64 worker_pressure_ppm = 0;
  u64 quota_pressure_ppm = 0;
  bool policy_allowed = false;
  bool evidence_present = false;
  bool evidence_authoritative = false;
  bool throttle_allowed = false;
  bool hard_backlog_pressure = false;
  bool engine_mga_authoritative = false;
  bool security_snapshot_bound = false;
  bool grants_proven = false;
  bool mga_recheck_required = true;
  bool security_recheck_required = true;
  bool parser_or_reference_authority = false;
  bool provider_transaction_finality_authority = false;
  bool provider_visibility_authority = false;
  bool client_autocommit_authority = false;
  bool wal_recovery_authority = false;
};

struct AdaptiveBatchPolicyEvidenceRequest {
  std::string family_label;
  std::string policy_id = "adaptive_batch_policy_v1";
  u64 evidence_epoch = 0;
  u64 required_epoch = 0;
  u64 evidence_age_microseconds = 0;
  u64 max_evidence_age_microseconds = 60000000;
  u64 backlog_units = 0;
  u64 backlog_budget_units = 0;
  u64 worker_pressure_ppm = 0;
  u64 quota_pressure_ppm = 0;
  bool policy_allowed = true;
  bool evidence_authoritative = true;
  bool throttle_allowed = true;
  bool hard_backlog_pressure = false;
  bool engine_mga_authoritative = true;
  bool security_snapshot_bound = true;
  bool grants_proven = true;
  bool mga_recheck_required = true;
  bool security_recheck_required = true;
  bool parser_or_reference_authority = false;
  bool provider_transaction_finality_authority = false;
  bool provider_visibility_authority = false;
  bool client_autocommit_authority = false;
  bool wal_recovery_authority = false;
};

AdaptiveBatchPolicyEvidence BuildAdaptiveBatchPolicyEvidence(
    const AdaptiveBatchPolicyEvidenceRequest& request);

std::vector<std::pair<std::string, std::string>>
SerializeAdaptiveBatchPolicyEvidence(
    const AdaptiveBatchPolicyEvidence& evidence);

}  // namespace scratchbird::core::agents
