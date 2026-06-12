// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SEARCH_KEY: CEIC_081_AGENT_REPLAY_QUARANTINE
// SEARCH_KEY: CEIC_081_AGENT_REPLAY_DIGEST_CAPTURE

#include "agent_durable_catalog.hpp"
#include "agent_package_provenance.hpp"

#include <string>

namespace scratchbird::core::agents {

enum class AgentReplayOperationKind {
  mark_replay_pending,
  schedule_retry,
  record_compensation,
  quarantine,
  release_quarantine
};

struct AgentReplayDigestCapture {
  std::string policy_digest;
  u64 policy_generation = 0;
  std::string metric_digest;
  std::string catalog_root_digest;
  std::string security_digest;
  u64 security_epoch = 0;
  std::string resource_reservation_digest;
  std::string binary_package_digest;
  std::string action_input_digest;
  std::string action_evidence_digest;
  std::string action_record_digest;
  std::string evidence_chain_digest;
};

struct AgentReplayDigestCaptureRequest {
  const DurableAgentCatalogImage* catalog = nullptr;
  std::string action_uuid;
  u64 security_epoch = 0;
  AgentPackageProvenanceBundle package_provenance;
  bool production_live_path = true;
};

struct AgentReplayDigestCaptureResult {
  AgentRuntimeStatus status;
  AgentReplayDigestCapture capture;
};

struct AgentReplayControlRequest {
  DurableAgentCatalogImage* catalog = nullptr;
  std::string action_uuid;
  AgentReplayOperationKind operation =
      AgentReplayOperationKind::mark_replay_pending;
  AgentReplayDigestCapture capture;
  AgentPackageProvenanceBundle package_provenance;
  std::string evidence_uuid;
  std::string review_evidence_uuid;
  std::string compensation_evidence_uuid;
  u64 now_microseconds = 0;
  u64 max_retry_count = 0;
  u64 retry_after_microseconds = 0;
  bool production_live_path = true;
  bool cluster_route_requested = false;
  bool external_cluster_provider_attested = false;
  bool parser_authority = false;
  bool client_authority = false;
  bool reference_authority = false;
  bool wal_authority = false;
  bool benchmark_authority = false;
  bool optimizer_plan_authority = false;
  bool index_finality_authority = false;
  bool provider_finality_authority = false;
  bool memory_authority = false;
  bool agent_action_authority = false;
};

struct AgentReplayControlResult {
  AgentRuntimeStatus status;
  DurableAgentReplayRecord replay_record;
  DurableAgentActionRecord action_record;
  bool idempotent = false;
  bool replay_record_written = false;
  bool action_state_updated = false;
  bool retry_scheduled = false;
  bool compensation_recorded = false;
  bool quarantine_released = false;
};

const char* AgentReplayOperationKindName(AgentReplayOperationKind kind);
std::string AgentReplayPolicyDigest(const DurableAgentCatalogImage& catalog,
                                    const DurableAgentActionRecord& action);
std::string AgentReplayResourceReservationDigest(
    const DurableAgentCatalogImage& catalog,
    const DurableAgentActionRecord& action);
std::string AgentReplaySecurityDigest(const AgentEvidenceRecord& evidence,
                                      u64 security_epoch);
std::string AgentDurableActionRecordDigest(
    const DurableAgentActionRecord& action);
std::string AgentReplayEvidenceDigest(const AgentEvidenceRecord& evidence);
std::string AgentReplayRecordDigest(const DurableAgentReplayRecord& replay);
AgentReplayDigestCaptureResult CaptureAgentReplayDigests(
    const AgentReplayDigestCaptureRequest& request);
AgentReplayControlResult ApplyAgentReplayControl(
    const AgentReplayControlRequest& request);

}  // namespace scratchbird::core::agents
