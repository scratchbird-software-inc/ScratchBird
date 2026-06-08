// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "agents/cleanup_archive_manager.hpp"

// CanonicalAgentRegistry/CanonicalAgentManifest owns production exposure;
// this file provides the local cleanup/archive horizon handler.

#include <algorithm>
#include <utility>

namespace scratchbird::core::agents::implemented_agents {
namespace {

using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;

Status OkStatus() { return {StatusCode::ok, Severity::info, Subsystem::engine}; }
Status ErrorStatus() {
  return {StatusCode::memory_invalid_request, Severity::error, Subsystem::engine};
}

void AddEvidence(CleanupArchiveManagerResult* result,
                 std::string key,
                 std::string value) {
  result->evidence.push_back({std::move(key), std::move(value)});
}

std::string BoolText(bool value) {
  return value ? "true" : "false";
}

void AddSnapshotEvidence(CleanupArchiveManagerResult* result,
                         const CleanupArchiveManagerSnapshot& snapshot) {
  AddEvidence(result,
              "cleanup_horizon_authoritative",
              BoolText(snapshot.cleanup_horizon_authoritative));
  AddEvidence(result,
              "archive_metadata_authoritative",
              BoolText(snapshot.archive_metadata_authoritative));
  AddEvidence(result,
              "disposal_guard_requested",
              BoolText(snapshot.disposal_guard_requested));
  AddEvidence(result,
              "reader_hold_active",
              BoolText(snapshot.reader_hold_active));
  AddEvidence(result,
              "writer_hold_active",
              BoolText(snapshot.writer_hold_active));
  AddEvidence(result,
              "parser_snapshot_hold_active",
              BoolText(snapshot.parser_snapshot_hold_active));
  AddEvidence(result,
              "backup_forward_hold_active",
              BoolText(snapshot.backup_forward_hold_active));
  AddEvidence(result,
              "archive_hold_active",
              BoolText(snapshot.archive_hold_active));
  AddEvidence(result,
              "legal_hold_active",
              BoolText(snapshot.legal_hold_active));
  AddEvidence(result,
              "backup_hold_active",
              BoolText(snapshot.backup_hold_active));
  AddEvidence(result,
              "detached_filespace_hold_active",
              BoolText(snapshot.detached_filespace_hold_active));
  AddEvidence(result,
              "stable_checkpoint_hold_active",
              BoolText(snapshot.stable_checkpoint_hold_active));
  AddEvidence(result,
              "local_durable_hold_active",
              BoolText(snapshot.local_durable_hold_active));
  AddEvidence(result,
              "restore_reachability_hold_active",
              BoolText(snapshot.restore_reachability_hold_active));
  AddEvidence(result,
              "blocked_cleanup_count",
              std::to_string(snapshot.blocked_cleanup_count));
  AddEvidence(result,
              "authoritative_cleanup_horizon",
              std::to_string(snapshot.authoritative_cleanup_horizon));
  AddEvidence(result,
              "current_cleanup_lwm",
              std::to_string(snapshot.current_cleanup_lwm));
}

CleanupArchiveManagerResult Finish(CleanupArchiveManagerDecisionKind decision,
                                   Status status,
                                   std::string code,
                                   std::string key,
                                   std::string detail,
                                   bool fail_closed) {
  CleanupArchiveManagerResult result;
  result.status = status;
  result.decision = decision;
  result.fail_closed = fail_closed;
  result.diagnostic = MakeCleanupArchiveManagerDiagnostic(
      result.status, std::move(code), std::move(key), std::move(detail));
  AddEvidence(&result, "decision",
              CleanupArchiveManagerDecisionKindName(result.decision));
  AddEvidence(&result, "transaction_finality_authority", "false");
  AddEvidence(&result, "recovery_authority", "false");
  return result;
}

bool AnyMultiHorizonHoldActive(const CleanupArchiveManagerSnapshot& snapshot) {
  return snapshot.reader_hold_active ||
         snapshot.writer_hold_active ||
         snapshot.parser_snapshot_hold_active ||
         snapshot.backup_forward_hold_active ||
         snapshot.archive_hold_active ||
         snapshot.legal_hold_active ||
         snapshot.backup_hold_active ||
         snapshot.detached_filespace_hold_active ||
         snapshot.stable_checkpoint_hold_active ||
         snapshot.local_durable_hold_active ||
         snapshot.restore_reachability_hold_active;
}

}  // namespace

const char* CleanupArchiveManagerDecisionKindName(
    CleanupArchiveManagerDecisionKind decision) {
  switch (decision) {
    case CleanupArchiveManagerDecisionKind::no_action: return "no_action";
    case CleanupArchiveManagerDecisionKind::advance_cleanup_lwm:
      return "advance_cleanup_lwm";
    case CleanupArchiveManagerDecisionKind::request_archive_verify:
      return "request_archive_verify";
    case CleanupArchiveManagerDecisionKind::refused: return "refused";
  }
  return "refused";
}

DiagnosticRecord MakeCleanupArchiveManagerDiagnostic(
    Status status,
    std::string diagnostic_code,
    std::string message_key,
    std::string detail) {
  return scratchbird::core::platform::MakeDiagnostic(
      status.code, status.severity, status.subsystem,
      std::move(diagnostic_code), std::move(message_key),
      {{"detail", std::move(detail)}}, {}, "cleanup_archive_manager", {});
}

CleanupArchiveManagerResult EvaluateCleanupArchiveManager(
    const CleanupArchiveManagerSnapshot& snapshot,
    const CleanupArchiveManagerPolicy& policy) {
  if (!policy.present || !policy.valid || !policy.scope_compatible) {
    auto result = Finish(CleanupArchiveManagerDecisionKind::refused, ErrorStatus(),
                         "SB_AGENT_CLEANUP_ARCHIVE_POLICY_INVALID",
                         "agents.cleanup_archive.policy_invalid",
                         "policy missing invalid or outside scope", true);
    AddSnapshotEvidence(&result, snapshot);
    return result;
  }
  if (!snapshot.cleanup_horizon_authoritative ||
      snapshot.parser_authority || snapshot.recovery_authority) {
    auto result = Finish(CleanupArchiveManagerDecisionKind::refused, ErrorStatus(),
                         "SB_AGENT_CLEANUP_ARCHIVE_AUTHORITY_UNTRUSTED",
                         "agents.cleanup_archive.untrusted_authority",
                         "cleanup horizon must be engine authoritative", true);
    AddSnapshotEvidence(&result, snapshot);
    return result;
  }
  if (snapshot.disposal_guard_requested &&
      AnyMultiHorizonHoldActive(snapshot)) {
    auto result = Finish(CleanupArchiveManagerDecisionKind::refused, ErrorStatus(),
                         "SB_AGENT_CLEANUP_ARCHIVE_MULTI_HORIZON_HOLD_ACTIVE",
                         "agents.cleanup_archive.multi_horizon_hold_active",
                         "physical reclaim archive deletion and write-after truncation require all holds clear",
                         true);
    AddSnapshotEvidence(&result, snapshot);
    return result;
  }
  if (snapshot.legal_hold_active || snapshot.backup_hold_active) {
    auto result = Finish(CleanupArchiveManagerDecisionKind::no_action, OkStatus(),
                         "SB_AGENT_CLEANUP_ARCHIVE_HOLD_ACTIVE",
                         "agents.cleanup_archive.hold_active",
                         "legal or backup hold blocks cleanup advance", false);
    AddSnapshotEvidence(&result, snapshot);
    return result;
  }
  if (snapshot.authoritative_cleanup_horizon > snapshot.current_cleanup_lwm &&
      snapshot.blocked_cleanup_count == 0 && policy.cleanup_lwm_allowed) {
    auto result = Finish(CleanupArchiveManagerDecisionKind::advance_cleanup_lwm,
                         OkStatus(),
                         "SB_AGENT_CLEANUP_ARCHIVE_LWM_ADVANCE",
                         "agents.cleanup_archive.lwm_advance",
                         "authoritative horizon permits bounded LWM advance",
                         false);
    result.proposed_cleanup_lwm = std::min(
        snapshot.authoritative_cleanup_horizon,
        snapshot.current_cleanup_lwm + policy.max_lwm_advance);
    AddSnapshotEvidence(&result, snapshot);
    AddEvidence(&result, "proposed_cleanup_lwm",
                std::to_string(result.proposed_cleanup_lwm));
    return result;
  }
  if (snapshot.archive_metadata_authoritative &&
      snapshot.archive_lag_bytes >= policy.archive_lag_bytes_threshold &&
      policy.archive_verify_allowed) {
    auto result = Finish(CleanupArchiveManagerDecisionKind::request_archive_verify,
                         OkStatus(),
                         "SB_AGENT_CLEANUP_ARCHIVE_VERIFY_REQUESTED",
                         "agents.cleanup_archive.verify_requested",
                         "archive lag exceeds verification threshold", false);
    AddSnapshotEvidence(&result, snapshot);
    return result;
  }
  auto result = Finish(CleanupArchiveManagerDecisionKind::no_action, OkStatus(),
                       "SB_AGENT_CLEANUP_ARCHIVE_NO_ACTION",
                       "agents.cleanup_archive.no_action",
                       "cleanup/archive state within policy", false);
  AddSnapshotEvidence(&result, snapshot);
  return result;
}

const char* cleanup_archive_manager_implementation_anchor() {
  return "cleanup_archive_manager";
}

}  // namespace scratchbird::core::agents::implemented_agents
