// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "agents/filespace_capacity_manager.hpp"

#include "metric_contracts.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace scratchbird::core::agents::implemented_agents {
namespace {

namespace page = scratchbird::storage::page;

using scratchbird::core::platform::MakeDiagnostic;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;
using scratchbird::core::platform::UuidKind;
using scratchbird::core::uuid::UuidToString;

Status FilespaceOkStatus() {
  return {StatusCode::ok, Severity::info, Subsystem::engine};
}

Status FilespaceRefuseStatus() {
  return {StatusCode::platform_required_feature_missing, Severity::error, Subsystem::engine};
}

bool SameTypedUuid(const TypedUuid& left, const TypedUuid& right) {
  return left.kind == right.kind && left.value == right.value;
}

bool TypedUuidHasKind(const TypedUuid& value, UuidKind kind) {
  return value.valid() && value.kind == kind;
}

TypedUuid NewCapacityEvidenceUuid(const page::PageFilespaceAgentRequestQueue* queue,
                                  u64 salt) {
  const u64 seed = 1860000000000ull +
                   (queue == nullptr ? 0 : queue->next_sequence) +
                   salt;
  const auto generated = scratchbird::core::uuid::GenerateEngineIdentityV7(
      UuidKind::object,
      seed);
  return generated.ok() ? generated.value : TypedUuid{};
}

bool PolicyShapeValid(const FilespaceCapacityManagerPolicy& policy) {
  return policy.valid &&
         TypedUuidHasKind(policy.database_uuid, UuidKind::database) &&
         TypedUuidHasKind(policy.filespace_uuid, UuidKind::filespace) &&
         TypedUuidHasKind(policy.policy_uuid, UuidKind::object) &&
         policy.target_free_pages >= policy.minimum_free_pages &&
         policy.max_capacity_window_pages > 0;
}

bool RequestScopeIdentityValid(const FilespaceCapacityManagerActionRequest& request) {
  return TypedUuidHasKind(request.database_uuid, UuidKind::database) &&
         TypedUuidHasKind(request.filespace_uuid, UuidKind::filespace) &&
         TypedUuidHasKind(request.policy_uuid, UuidKind::object);
}

bool RequestScopeMatches(const FilespaceCapacityManagerActionRequest& request,
                         const FilespaceCapacityManagerMetricSnapshot& snapshot,
                         const FilespaceCapacityManagerPolicy& policy) {
  return snapshot.scope_compatible &&
         policy.scope_compatible &&
         SameTypedUuid(request.database_uuid, snapshot.database_uuid) &&
         SameTypedUuid(request.filespace_uuid, snapshot.filespace_uuid) &&
         SameTypedUuid(request.policy_uuid, snapshot.policy_uuid) &&
         SameTypedUuid(snapshot.database_uuid, policy.database_uuid) &&
         SameTypedUuid(snapshot.filespace_uuid, policy.filespace_uuid) &&
         SameTypedUuid(snapshot.policy_uuid, policy.policy_uuid);
}

bool ScopeIdentityValid(const FilespaceCapacityManagerMetricSnapshot& snapshot,
                        const FilespaceCapacityManagerPolicy& policy,
                        const page::PageFilespaceAgentRequest& request) {
  return TypedUuidHasKind(snapshot.database_uuid, UuidKind::database) &&
         TypedUuidHasKind(snapshot.filespace_uuid, UuidKind::filespace) &&
         TypedUuidHasKind(snapshot.policy_uuid, UuidKind::object) &&
         TypedUuidHasKind(policy.database_uuid, UuidKind::database) &&
         TypedUuidHasKind(policy.filespace_uuid, UuidKind::filespace) &&
         TypedUuidHasKind(policy.policy_uuid, UuidKind::object) &&
         TypedUuidHasKind(request.database_uuid, UuidKind::database) &&
         TypedUuidHasKind(request.filespace_uuid, UuidKind::filespace) &&
         TypedUuidHasKind(request.policy_uuid, UuidKind::object);
}

bool ScopeMatches(const FilespaceCapacityManagerMetricSnapshot& snapshot,
                  const FilespaceCapacityManagerPolicy& policy,
                  const page::PageFilespaceAgentRequest& request) {
  return snapshot.scope_compatible &&
         policy.scope_compatible &&
         SameTypedUuid(snapshot.database_uuid, policy.database_uuid) &&
         SameTypedUuid(snapshot.filespace_uuid, policy.filespace_uuid) &&
         SameTypedUuid(snapshot.policy_uuid, policy.policy_uuid) &&
         SameTypedUuid(request.database_uuid, policy.database_uuid) &&
         SameTypedUuid(request.filespace_uuid, policy.filespace_uuid) &&
         SameTypedUuid(request.policy_uuid, policy.policy_uuid);
}

bool HealthPermitsCapacityWindow(const FilespaceCapacityManagerMetricSnapshot& snapshot,
                                 const FilespaceCapacityManagerPolicy& policy) {
  if (snapshot.health_state == FilespaceCapacityHealthState::healthy) {
    return true;
  }
  return snapshot.health_state == FilespaceCapacityHealthState::degraded &&
         policy.allow_degraded_capacity_window;
}

bool RolePermitsCapacityWindow(FilespaceCapacityRoleState role) {
  switch (role) {
    case FilespaceCapacityRoleState::active_primary:
    case FilespaceCapacityRoleState::secondary_data:
    case FilespaceCapacityRoleState::temporary:
      return true;
    case FilespaceCapacityRoleState::unknown:
    case FilespaceCapacityRoleState::primary_shadow:
    case FilespaceCapacityRoleState::primary_candidate:
    case FilespaceCapacityRoleState::drop_pending:
    case FilespaceCapacityRoleState::forbidden:
      return false;
  }
  return false;
}

bool IsPageOwnedRequestKind(page::PageFilespaceAgentRequestKind kind) {
  return kind == page::PageFilespaceAgentRequestKind::reserve_pages ||
         kind == page::PageFilespaceAgentRequestKind::relocate_pages ||
         kind == page::PageFilespaceAgentRequestKind::release_pages;
}

bool IsForbiddenBoundaryAction(FilespaceCapacityManagerActionKind action) {
  switch (action) {
    case FilespaceCapacityManagerActionKind::forbidden_allocate_page:
    case FilespaceCapacityManagerActionKind::forbidden_relocate_page:
    case FilespaceCapacityManagerActionKind::forbidden_compact_page_family:
    case FilespaceCapacityManagerActionKind::forbidden_rebuild_index:
    case FilespaceCapacityManagerActionKind::forbidden_advance_mga_cleanup:
      return true;
    case FilespaceCapacityManagerActionKind::request_filespace_expand:
    case FilespaceCapacityManagerActionKind::request_filespace_move:
    case FilespaceCapacityManagerActionKind::request_filespace_shrink:
    case FilespaceCapacityManagerActionKind::request_filespace_truncate:
    case FilespaceCapacityManagerActionKind::request_filespace_quarantine:
    case FilespaceCapacityManagerActionKind::recommend_primary_shadow_promotion:
      return false;
  }
  return true;
}

TypedUuid NewActionEvidenceUuid(const FilespaceCapacityManagerActionRequest& request,
                                u64 salt) {
  if (request.evidence_uuid.valid()) {
    return request.evidence_uuid;
  }
  const u64 seed = 1865000000000ull +
                   static_cast<u64>(request.action) * 1000ull +
                   salt +
                   request.target_bytes +
                   request.safe_tail_bytes;
  const auto generated = scratchbird::core::uuid::GenerateEngineIdentityV7(
      UuidKind::object,
      seed);
  return generated.ok() ? generated.value : TypedUuid{};
}

std::string CapacityEvidenceReason(const page::PageFilespaceAgentRequest& request,
                                   u64 granted_pages,
                                   u64 capacity_window_pages) {
  return std::string("filespace capacity window approved requested_pages=") +
         std::to_string(request.requested_pages) +
         " granted_pages=" +
         std::to_string(granted_pages) +
         " capacity_window_pages=" +
         std::to_string(capacity_window_pages);
}

FilespaceCapacityManagerTickResult Finish(Status status,
                                          FilespaceCapacityManagerDecisionKind decision,
                                          std::string diagnostic_code,
                                          std::string message_key,
                                          std::string detail,
                                          bool fail_closed) {
  FilespaceCapacityManagerTickResult result;
  result.status = status;
  result.decision = decision;
  result.fail_closed = fail_closed;
  result.diagnostic = MakeFilespaceCapacityManagerDiagnostic(status,
                                                            std::move(diagnostic_code),
                                                            std::move(message_key),
                                                            std::move(detail));
  return result;
}

FilespaceCapacityManagerTickResult FailClosed(std::string diagnostic_code,
                                              std::string message_key,
                                              std::string detail) {
  return Finish(FilespaceRefuseStatus(),
                FilespaceCapacityManagerDecisionKind::refused,
                std::move(diagnostic_code),
                std::move(message_key),
                std::move(detail),
                true);
}

FilespaceCapacityManagerEvidence EvidenceForRequest(
    const page::PageFilespaceAgentRequest& request,
    const FilespaceCapacityManagerMetricSnapshot& snapshot,
    const FilespaceCapacityManagerPolicy& policy,
    FilespaceCapacityManagerDecisionKind decision,
    TypedUuid evidence_uuid,
    std::string diagnostic_code,
    std::string evidence_state,
    std::string reason,
    u64 granted_pages,
    u64 capacity_window_pages,
    bool durable_state_changed) {
  FilespaceCapacityManagerEvidence evidence;
  evidence.request_uuid = request.request_uuid;
  evidence.evidence_uuid = evidence_uuid;
  evidence.database_uuid = request.database_uuid;
  evidence.filespace_uuid = request.filespace_uuid;
  evidence.policy_uuid = request.policy_uuid;
  evidence.request_kind = request.kind;
  evidence.decision = decision;
  evidence.requested_pages = request.requested_pages;
  evidence.granted_pages = granted_pages;
  evidence.capacity_window_pages = capacity_window_pages;
  evidence.free_pages = snapshot.free_pages;
  evidence.reserved_pages = snapshot.reserved_pages;
  evidence.target_free_pages = policy.target_free_pages;
  evidence.diagnostic_code = std::move(diagnostic_code);
  evidence.evidence_state = std::move(evidence_state);
  evidence.reason = std::move(reason);
  evidence.durable_state_changed = durable_state_changed;
  return evidence;
}

FilespaceCapacityManagerEvidence EvidenceForAction(
    const FilespaceCapacityManagerActionRequest& request,
    const FilespaceCapacityManagerMetricSnapshot& snapshot,
    const FilespaceCapacityManagerPolicy& policy,
    FilespaceCapacityManagerDecisionKind decision,
    TypedUuid evidence_uuid,
    std::string diagnostic_code,
    std::string evidence_state,
    std::string reason) {
  FilespaceCapacityManagerEvidence evidence;
  evidence.request_uuid = request.request_uuid;
  evidence.evidence_uuid = evidence_uuid;
  evidence.database_uuid = request.database_uuid;
  evidence.filespace_uuid = request.filespace_uuid;
  evidence.policy_uuid = request.policy_uuid.valid() ? request.policy_uuid : policy.policy_uuid;
  evidence.decision = decision;
  evidence.requested_pages = request.target_bytes;
  evidence.granted_pages = 0;
  evidence.capacity_window_pages = 0;
  evidence.free_pages = snapshot.free_pages;
  evidence.reserved_pages = snapshot.reserved_pages;
  evidence.target_free_pages = policy.target_free_pages;
  evidence.diagnostic_code = std::move(diagnostic_code);
  evidence.evidence_state = std::move(evidence_state);
  evidence.reason = std::move(reason);
  evidence.durable_state_changed = evidence_uuid.valid();
  return evidence;
}

FilespaceCapacityManagerActionResult FinishAction(
    const FilespaceCapacityManagerActionRequest& request,
    const FilespaceCapacityManagerMetricSnapshot& snapshot,
    const FilespaceCapacityManagerPolicy& policy,
    Status status,
    FilespaceCapacityManagerDecisionKind decision,
    std::string diagnostic_code,
    std::string message_key,
    std::string detail,
    bool fail_closed) {
  FilespaceCapacityManagerActionResult result;
  result.status = status;
  result.action = request.action;
  result.decision = decision;
  result.fail_closed = fail_closed;
  result.refused = fail_closed ||
                   decision == FilespaceCapacityManagerDecisionKind::action_refused ||
                   decision == FilespaceCapacityManagerDecisionKind::refused;
  result.recommended =
      decision == FilespaceCapacityManagerDecisionKind::action_recommended;
  result.suppressed =
      decision == FilespaceCapacityManagerDecisionKind::action_suppressed;
  result.dry_run = decision == FilespaceCapacityManagerDecisionKind::action_dry_run;
  result.approval_required =
      decision == FilespaceCapacityManagerDecisionKind::action_approval_required;
  result.authorized =
      decision == FilespaceCapacityManagerDecisionKind::action_authorized;
  result.physical_filespace_mutation_attempted = false;
  result.page_ledger_mutation_attempted = false;
  result.diagnostic = MakeFilespaceCapacityManagerDiagnostic(
      status,
      diagnostic_code,
      std::move(message_key),
      detail);
  result.evidence = EvidenceForAction(request,
                                      snapshot,
                                      policy,
                                      decision,
                                      NewActionEvidenceUuid(request, 5000000ull),
                                      diagnostic_code,
                                      FilespaceCapacityManagerDecisionKindName(decision),
                                      std::move(detail));
  return result;
}

FilespaceCapacityManagerActionResult RefuseAction(
    const FilespaceCapacityManagerActionRequest& request,
    const FilespaceCapacityManagerMetricSnapshot& snapshot,
    const FilespaceCapacityManagerPolicy& policy,
    std::string diagnostic_code,
    std::string message_key,
    std::string detail) {
  return FinishAction(request,
                      snapshot,
                      policy,
                      FilespaceRefuseStatus(),
                      FilespaceCapacityManagerDecisionKind::action_refused,
                      std::move(diagnostic_code),
                      std::move(message_key),
                      std::move(detail),
                      true);
}

FilespaceCapacityManagerTickResult RefuseRequest(
    page::PageFilespaceAgentRequestQueue* queue,
    const page::PageFilespaceAgentRequest& request,
    const FilespaceCapacityManagerMetricSnapshot& snapshot,
    const FilespaceCapacityManagerPolicy& policy,
    u64 capacity_window_pages,
    std::string diagnostic_code,
    std::string message_key,
    std::string detail) {
  const TypedUuid evidence_uuid = NewCapacityEvidenceUuid(queue, 3000000ull);
  const std::size_t record_count_before = queue == nullptr ? 0 : queue->records.size();
  page::PageFilespaceAgentQueueResult transition =
      page::TransitionPageFilespaceAgentRequestWithEvidence(
          queue,
          request.request_uuid,
          page::PageFilespaceAgentRequestState::refused,
          detail,
          diagnostic_code,
          "capacity_window_refused",
          evidence_uuid);

  FilespaceCapacityManagerTickResult result;
  result.status = transition.status.ok() ? FilespaceRefuseStatus() : transition.status;
  result.decision = FilespaceCapacityManagerDecisionKind::capacity_window_refused;
  result.diagnostic = transition.status.ok()
                          ? MakeFilespaceCapacityManagerDiagnostic(
                                result.status,
                                diagnostic_code,
                                std::move(message_key),
                                detail)
                          : transition.diagnostic;
  result.evidence = EvidenceForRequest(
      request,
      snapshot,
      policy,
      FilespaceCapacityManagerDecisionKind::capacity_window_refused,
      evidence_uuid,
      transition.status.ok() ? diagnostic_code : transition.diagnostic.diagnostic_code,
      "capacity_window_refused",
      detail,
      0,
      capacity_window_pages,
      transition.transitioned);
  result.queue_record = transition.record;
  result.requested_pages = request.requested_pages;
  result.capacity_window_pages = result.evidence.capacity_window_pages;
  result.processed_records = 1;
  result.fail_closed = true;
  result.refused = true;
  result.queue_mutated = queue != nullptr && queue->records.size() == record_count_before &&
                         transition.transitioned;
  (void)scratchbird::core::metrics::RecordFilespaceAgentCapacityRequest(
      request.filespace_uuid.valid() ? UuidToString(request.filespace_uuid.value) : "invalid",
      page::PageFilespaceAgentRequestKindName(request.kind),
      "refused");
  return result;
}

FilespaceCapacityManagerTickResult ApproveCapacityWindow(
    page::PageFilespaceAgentRequestQueue* queue,
    const page::PageFilespaceAgentRequest& request,
    const FilespaceCapacityManagerMetricSnapshot& snapshot,
    const FilespaceCapacityManagerPolicy& policy,
    u64 capacity_window_pages) {
  const TypedUuid evidence_uuid = NewCapacityEvidenceUuid(queue, 1000000ull);
  const std::size_t record_count_before = queue == nullptr ? 0 : queue->records.size();
  const u64 granted_pages = request.requested_pages;
  const std::string reason = CapacityEvidenceReason(request,
                                                    granted_pages,
                                                    capacity_window_pages);
  page::PageFilespaceAgentQueueResult transition =
      page::TransitionPageFilespaceAgentRequestWithEvidence(
          queue,
          request.request_uuid,
          page::PageFilespaceAgentRequestState::approved,
          reason,
          "ok",
          "capacity_window_open",
          evidence_uuid);
  if (!transition.ok()) {
    auto result = FailClosed(transition.diagnostic.diagnostic_code,
                             transition.diagnostic.message_key,
                             "capacity window approval transition failed");
    result.queue_record = transition.record;
    result.requested_pages = request.requested_pages;
    result.capacity_window_pages = capacity_window_pages;
    return result;
  }

  FilespaceCapacityManagerTickResult result;
  result.status = FilespaceOkStatus();
  result.decision = FilespaceCapacityManagerDecisionKind::capacity_window_approved;
  result.diagnostic = MakeFilespaceCapacityManagerDiagnostic(
      result.status,
      "FILESPACE_AGENT.CAPACITY_WINDOW_APPROVED",
      "agents.filespace_capacity.capacity_window_approved",
      "filespace capacity window approved without physical mutation");
  result.evidence = EvidenceForRequest(
      request,
      snapshot,
      policy,
      FilespaceCapacityManagerDecisionKind::capacity_window_approved,
      evidence_uuid,
      "ok",
      "capacity_window_open",
      reason,
      granted_pages,
      capacity_window_pages,
      transition.transitioned);
  result.queue_record = transition.record;
  result.requested_pages = request.requested_pages;
  result.granted_pages = granted_pages;
  result.capacity_window_pages = capacity_window_pages;
  result.processed_records = 1;
  result.approved = true;
  result.queue_mutated = queue != nullptr && queue->records.size() == record_count_before &&
                         transition.transitioned;
  (void)scratchbird::core::metrics::RecordFilespaceAgentCapacityRequest(
      UuidToString(request.filespace_uuid.value),
      page::PageFilespaceAgentRequestKindName(request.kind),
      "approved");
  return result;
}

FilespaceCapacityManagerTickResult ProcessWaitingRequest(
    page::PageFilespaceAgentRequestQueue* queue,
    const page::PageFilespaceAgentRequest& request,
    const FilespaceCapacityManagerMetricSnapshot& snapshot,
    const FilespaceCapacityManagerPolicy& policy,
    const FilespaceCapacityManagerSafetyState& safety,
    u64 available_capacity_window_pages) {
  auto refuse = [&](std::string diagnostic_code,
                    std::string message_key,
                    std::string detail) {
    return RefuseRequest(queue,
                         request,
                         snapshot,
                         policy,
                         available_capacity_window_pages,
                         std::move(diagnostic_code),
                         std::move(message_key),
                         std::move(detail));
  };

  if (IsPageOwnedRequestKind(request.kind)) {
    return refuse("FILESPACE_AGENT.PAGE_AUTHORITY_REQUIRED",
                  "agents.filespace_capacity.page_authority_required",
                  "filespace_capacity_manager must not approve page-owned reserve, relocate, or release requests");
  }
  if (request.kind != page::PageFilespaceAgentRequestKind::extend_filespace) {
    return refuse("FILESPACE_AGENT.ACTION_RECOMMEND_ONLY",
                  "agents.filespace_capacity.storage_lifecycle_boundary",
                  "filespace_capacity_manager only owns capacity-window authorization; physical lifecycle mutation is handled by the storage lifecycle authority");
  }
  if (!ScopeIdentityValid(snapshot, policy, request) || !ScopeMatches(snapshot, policy, request)) {
    return refuse("FILESPACE_AGENT.POLICY_SCOPE_INCOMPATIBLE",
                  "agents.filespace_capacity.scope_incompatible",
                  "database_uuid, filespace_uuid, and policy_uuid must match the request, metrics, and policy");
  }
  if (!snapshot.metrics_present) {
    return refuse("FILESPACE_AGENT.METRIC_MISSING",
                  "agents.filespace_capacity.metric_missing",
                  "required filespace capacity metrics are missing");
  }
  if (!snapshot.metrics_fresh) {
    return refuse("FILESPACE_AGENT.METRIC_STALE",
                  "agents.filespace_capacity.metric_stale",
                  "required filespace capacity metrics are stale");
  }
  if (!snapshot.metrics_trusted) {
    return refuse("FILESPACE_AGENT.METRIC_UNTRUSTED",
                  "agents.filespace_capacity.metric_untrusted",
                  "required filespace capacity metrics are not trusted");
  }
  if (!HealthPermitsCapacityWindow(snapshot, policy)) {
    return refuse("FILESPACE_AGENT.HEALTH_DENIED",
                  "agents.filespace_capacity.health_denied",
                  std::string("filespace health state does not permit capacity action: ") +
                      FilespaceCapacityHealthStateName(snapshot.health_state));
  }
  if (!RolePermitsCapacityWindow(snapshot.role_state)) {
    return refuse("FILESPACE_AGENT.ROLE_DENIED",
                  "agents.filespace_capacity.role_denied",
                  std::string("filespace role state does not permit capacity action: ") +
                      FilespaceCapacityRoleStateName(snapshot.role_state));
  }
  if (!PolicyShapeValid(policy)) {
    return refuse("FILESPACE_AGENT.POLICY_INVALID",
                  "agents.filespace_capacity.policy_invalid",
                  "filespace capacity policy is invalid");
  }
  if (!safety.engine_authoritative) {
    return refuse("FILESPACE_AGENT.PERMISSION_DENIED",
                  "agents.filespace_capacity.engine_authority_required",
                  "filespace capacity decisions require engine-owned authority");
  }
  if (!safety.startup_complete) {
    return refuse("FILESPACE_AGENT.STARTUP_UNSAFE",
                  "agents.filespace_capacity.startup_unsafe",
                  "startup state does not permit capacity window approval");
  }
  if (!safety.recovery_complete) {
    return refuse("FILESPACE_AGENT.RECOVERY_UNSAFE",
                  "agents.filespace_capacity.recovery_unsafe",
                  "recovery state does not permit capacity window approval");
  }
  if (safety.maintenance_mode && !safety.maintenance_allows_capacity_windows) {
    return refuse("FILESPACE_AGENT.MAINTENANCE_UNSAFE",
                  "agents.filespace_capacity.maintenance_unsafe",
                  "maintenance state blocks capacity window approval");
  }
  if (!policy.capacity_window_allowed || !policy.capacity_processing_policy_explicit) {
    return refuse("FILESPACE_AGENT.ACTION_RECOMMEND_ONLY",
                  "agents.filespace_capacity.capacity_window_recommend_only",
                  "capacity window processing requires an explicit filespace_capacity_policy");
  }
  if (!policy.expand_allowed || !policy.expand_request_policy_explicit) {
    return refuse("FILESPACE_AGENT.ACTION_RECOMMEND_ONLY",
                  "agents.filespace_capacity.expand_recommend_only",
                  "extend_filespace capacity windows require explicit expand policy authority");
  }
  if (!snapshot.expand_capacity_proof_present || !snapshot.expand_device_proof_present) {
    return refuse("FILESPACE_AGENT.EVIDENCE_REQUIRED",
                  "agents.filespace_capacity.expand_evidence_required",
                  "extend_filespace capacity windows require capacity proof and device proof");
  }
  if (!snapshot.expand_capacity_proof_fresh || !snapshot.expand_device_proof_fresh) {
    return refuse("FILESPACE_AGENT.METRIC_STALE",
                  "agents.filespace_capacity.expand_proof_stale",
                  "extend_filespace capacity or device proof is stale");
  }
  if (request.requested_pages == 0) {
    return refuse("FILESPACE_AGENT.REQUESTED_PAGES_REQUIRED",
                  "agents.filespace_capacity.requested_pages_required",
                  "capacity request must ask for at least one page");
  }

  if (request.requested_pages > available_capacity_window_pages) {
    return refuse("FILESPACE_AGENT.REQUESTED_PAGES_OVER_LIMIT",
                  "agents.filespace_capacity.requested_pages_over_limit",
                  "requested pages exceed the remaining policy and metric capacity window");
  }

  return ApproveCapacityWindow(queue,
                               request,
                               snapshot,
                               policy,
                               available_capacity_window_pages);
}

FilespaceCapacityManagerActionResult EvaluateCommonActionGates(
    const FilespaceCapacityManagerActionRequest& request,
    const FilespaceCapacityManagerMetricSnapshot& snapshot,
    const FilespaceCapacityManagerPolicy& policy,
    const FilespaceCapacityManagerSafetyState& safety) {
  if (IsForbiddenBoundaryAction(request.action)) {
    return RefuseAction(request,
                        snapshot,
                        policy,
                        "FILESPACE_AGENT.PAGE_AUTHORITY_REQUIRED",
                        "agents.filespace_capacity.page_boundary_forbidden",
                        "filespace_capacity_manager must not allocate, relocate, compact, rebuild, or advance MGA cleanup");
  }
  if (!TypedUuidHasKind(request.request_uuid, UuidKind::object)) {
    return RefuseAction(request,
                        snapshot,
                        policy,
                        "FILESPACE_AGENT.EVIDENCE_REQUIRED",
                        "agents.filespace_capacity.request_uuid_required",
                        "filespace action requires a durable request UUID");
  }
  if (!RequestScopeIdentityValid(request) ||
      !TypedUuidHasKind(snapshot.database_uuid, UuidKind::database) ||
      !TypedUuidHasKind(snapshot.filespace_uuid, UuidKind::filespace) ||
      !TypedUuidHasKind(snapshot.policy_uuid, UuidKind::object)) {
    return RefuseAction(request,
                        snapshot,
                        policy,
                        "FILESPACE_AGENT.POLICY_SCOPE_INCOMPATIBLE",
                        "agents.filespace_capacity.scope_incompatible",
                        "database_uuid, filespace_uuid, and policy_uuid must be UUID-scoped authority");
  }
  if (!snapshot.metrics_present) {
    return RefuseAction(request,
                        snapshot,
                        policy,
                        "FILESPACE_AGENT.METRIC_MISSING",
                        "agents.filespace_capacity.metric_missing",
                        "required filespace capacity metrics are missing");
  }
  if (!snapshot.metrics_fresh) {
    return RefuseAction(request,
                        snapshot,
                        policy,
                        "FILESPACE_AGENT.METRIC_STALE",
                        "agents.filespace_capacity.metric_stale",
                        "required filespace capacity metrics are stale");
  }
  if (!snapshot.metrics_trusted) {
    return RefuseAction(request,
                        snapshot,
                        policy,
                        "FILESPACE_AGENT.METRIC_UNTRUSTED",
                        "agents.filespace_capacity.metric_untrusted",
                        "required filespace capacity metrics are not trusted");
  }
  if (!PolicyShapeValid(policy)) {
    return RefuseAction(request,
                        snapshot,
                        policy,
                        "FILESPACE_AGENT.POLICY_INVALID",
                        "agents.filespace_capacity.policy_invalid",
                        "filespace capacity policy is invalid");
  }
  if (!RequestScopeMatches(request, snapshot, policy)) {
    return RefuseAction(request,
                        snapshot,
                        policy,
                        "FILESPACE_AGENT.POLICY_SCOPE_INCOMPATIBLE",
                        "agents.filespace_capacity.scope_incompatible",
                        "request, metrics, and policy UUID scope must match");
  }
  if (!safety.engine_authoritative) {
    return RefuseAction(request,
                        snapshot,
                        policy,
                        "FILESPACE_AGENT.PERMISSION_DENIED",
                        "agents.filespace_capacity.engine_authority_required",
                        "filespace capacity decisions require engine-owned authority");
  }
  if (!safety.startup_complete) {
    return RefuseAction(request,
                        snapshot,
                        policy,
                        "FILESPACE_AGENT.STARTUP_UNSAFE",
                        "agents.filespace_capacity.startup_unsafe",
                        "startup state does not permit filespace lifecycle action");
  }
  if (!safety.recovery_complete) {
    return RefuseAction(request,
                        snapshot,
                        policy,
                        "FILESPACE_AGENT.RECOVERY_UNSAFE",
                        "agents.filespace_capacity.recovery_unsafe",
                        "recovery state does not permit filespace lifecycle action");
  }
  if (safety.maintenance_mode && !safety.maintenance_allows_capacity_windows) {
    return RefuseAction(request,
                        snapshot,
                        policy,
                        "FILESPACE_AGENT.MAINTENANCE_UNSAFE",
                        "agents.filespace_capacity.maintenance_unsafe",
                        "maintenance state blocks filespace lifecycle action");
  }
  if (!request.explicit_evidence) {
    return RefuseAction(request,
                        snapshot,
                        policy,
                        "FILESPACE_AGENT.EVIDENCE_REQUIRED",
                        "agents.filespace_capacity.evidence_required",
                        "filespace action requires explicit evidence before acceptance");
  }
  if (request.dry_run) {
    return FinishAction(request,
                        snapshot,
                        policy,
                        FilespaceOkStatus(),
                        FilespaceCapacityManagerDecisionKind::action_dry_run,
                        "FILESPACE_AGENT.DRY_RUN",
                        "agents.filespace_capacity.dry_run",
                        "filespace action evaluated as dry-run without mutation",
                        false);
  }
  return FilespaceCapacityManagerActionResult{};
}

bool CommonActionResultReturned(const FilespaceCapacityManagerActionResult& result) {
  return result.diagnostic.diagnostic_code.empty() == false;
}

FilespaceCapacityManagerActionResult AuthorizeAction(
    const FilespaceCapacityManagerActionRequest& request,
    const FilespaceCapacityManagerMetricSnapshot& snapshot,
    const FilespaceCapacityManagerPolicy& policy,
    const std::string& detail) {
  return FinishAction(request,
                      snapshot,
                      policy,
                      FilespaceOkStatus(),
                      FilespaceCapacityManagerDecisionKind::action_authorized,
                      "FILESPACE_AGENT.ACTION_AUTHORIZED",
                      "agents.filespace_capacity.action_authorized",
                      detail,
                      false);
}

FilespaceCapacityManagerActionResult ApprovalRequired(
    const FilespaceCapacityManagerActionRequest& request,
    const FilespaceCapacityManagerMetricSnapshot& snapshot,
    const FilespaceCapacityManagerPolicy& policy,
    const std::string& detail) {
  return FinishAction(request,
                      snapshot,
                      policy,
                      FilespaceOkStatus(),
                      FilespaceCapacityManagerDecisionKind::action_approval_required,
                      "FILESPACE_AGENT.APPROVAL_REQUIRED",
                      "agents.filespace_capacity.approval_required",
                      detail,
                      false);
}

bool PermissionForLifecycleAction(const FilespaceCapacityManagerActionRequest& request,
                                  bool truncate_right) {
  return request.has_obs_agent_action_approve &&
         (truncate_right ? request.has_lifecycle_truncate_control
                         : request.has_filespace_lifecycle_control);
}

}  // namespace

// SEARCH_KEY: SB_AGENT_IMPLEMENTATION_filespace_capacity_manager
// Canonical filespace_capacity_manager behavior is registered in CanonicalAgentRegistry().
// PFAR-008 owns capacity-window approval/refusal for page-agent handoff requests only;
// physical filespace growth/truncation is routed to storage lifecycle authority.
const char* filespace_capacity_manager_implementation_anchor() { return "filespace_capacity_manager"; }

const char* FilespaceCapacityManagerDecisionKindName(
    FilespaceCapacityManagerDecisionKind kind) {
  switch (kind) {
    case FilespaceCapacityManagerDecisionKind::no_action:
      return "no_action";
    case FilespaceCapacityManagerDecisionKind::capacity_window_approved:
      return "capacity_window_approved";
    case FilespaceCapacityManagerDecisionKind::capacity_window_refused:
      return "capacity_window_refused";
    case FilespaceCapacityManagerDecisionKind::action_recommended:
      return "action_recommended";
    case FilespaceCapacityManagerDecisionKind::action_suppressed:
      return "action_suppressed";
    case FilespaceCapacityManagerDecisionKind::action_dry_run:
      return "action_dry_run";
    case FilespaceCapacityManagerDecisionKind::action_approval_required:
      return "action_approval_required";
    case FilespaceCapacityManagerDecisionKind::action_authorized:
      return "action_authorized";
    case FilespaceCapacityManagerDecisionKind::action_refused:
      return "action_refused";
    case FilespaceCapacityManagerDecisionKind::refused:
      return "refused";
  }
  return "unknown";
}

const char* FilespaceCapacityManagerActionKindName(
    FilespaceCapacityManagerActionKind action) {
  switch (action) {
    case FilespaceCapacityManagerActionKind::request_filespace_expand:
      return "request_filespace_expand";
    case FilespaceCapacityManagerActionKind::request_filespace_move:
      return "request_filespace_move";
    case FilespaceCapacityManagerActionKind::request_filespace_shrink:
      return "request_filespace_shrink";
    case FilespaceCapacityManagerActionKind::request_filespace_truncate:
      return "request_filespace_truncate";
    case FilespaceCapacityManagerActionKind::request_filespace_quarantine:
      return "request_filespace_quarantine";
    case FilespaceCapacityManagerActionKind::recommend_primary_shadow_promotion:
      return "recommend_primary_shadow_promotion";
    case FilespaceCapacityManagerActionKind::forbidden_allocate_page:
      return "forbidden_allocate_page";
    case FilespaceCapacityManagerActionKind::forbidden_relocate_page:
      return "forbidden_relocate_page";
    case FilespaceCapacityManagerActionKind::forbidden_compact_page_family:
      return "forbidden_compact_page_family";
    case FilespaceCapacityManagerActionKind::forbidden_rebuild_index:
      return "forbidden_rebuild_index";
    case FilespaceCapacityManagerActionKind::forbidden_advance_mga_cleanup:
      return "forbidden_advance_mga_cleanup";
  }
  return "unknown";
}

const char* FilespaceCapacityHealthStateName(FilespaceCapacityHealthState state) {
  switch (state) {
    case FilespaceCapacityHealthState::unknown:
      return "unknown";
    case FilespaceCapacityHealthState::healthy:
      return "healthy";
    case FilespaceCapacityHealthState::degraded:
      return "degraded";
    case FilespaceCapacityHealthState::critical:
      return "critical";
    case FilespaceCapacityHealthState::failed:
      return "failed";
  }
  return "unknown";
}

const char* FilespaceCapacityRoleStateName(FilespaceCapacityRoleState state) {
  switch (state) {
    case FilespaceCapacityRoleState::unknown:
      return "unknown";
    case FilespaceCapacityRoleState::active_primary:
      return "active_primary";
    case FilespaceCapacityRoleState::primary_shadow:
      return "primary_shadow";
    case FilespaceCapacityRoleState::primary_candidate:
      return "primary_candidate";
    case FilespaceCapacityRoleState::secondary_data:
      return "secondary_data";
    case FilespaceCapacityRoleState::temporary:
      return "temporary";
    case FilespaceCapacityRoleState::drop_pending:
      return "drop_pending";
    case FilespaceCapacityRoleState::forbidden:
      return "forbidden";
  }
  return "unknown";
}

FilespaceCapacityManagerPolicy DefaultFilespaceCapacityManagerPolicy() {
  const auto defaults = DefaultStorageSpaceAgentDefaults();
  FilespaceCapacityManagerPolicy policy;
  policy.minimum_free_pages = defaults.filespace_min_available_pages;
  policy.target_free_pages = defaults.filespace_target_available_pages;
  policy.max_capacity_window_pages = defaults.filespace_target_available_pages;
  policy.capacity_window_allowed = false;
  policy.capacity_processing_policy_explicit = false;
  policy.expand_allowed = false;
  policy.expand_request_policy_explicit = false;
  return policy;
}

u64 FilespaceCapacityManagerEffectiveWindowPages(
    const FilespaceCapacityManagerMetricSnapshot& snapshot,
    const FilespaceCapacityManagerPolicy& policy) {
  return std::min(snapshot.available_capacity_window_pages,
                  policy.max_capacity_window_pages);
}

FilespaceCapacityManagerTickResult EvaluateFilespaceCapacityManagerTick(
    page::PageFilespaceAgentRequestQueue* queue,
    const FilespaceCapacityManagerMetricSnapshot& snapshot,
    const FilespaceCapacityManagerPolicy& policy) {
  return EvaluateFilespaceCapacityManagerTick(queue,
                                             snapshot,
                                             policy,
                                             FilespaceCapacityManagerSafetyState{});
}

FilespaceCapacityManagerTickResult EvaluateFilespaceCapacityManagerTick(
    page::PageFilespaceAgentRequestQueue* queue,
    const FilespaceCapacityManagerMetricSnapshot& snapshot,
    const FilespaceCapacityManagerPolicy& policy,
    const FilespaceCapacityManagerSafetyState& safety) {
  if (queue == nullptr) {
    return FailClosed("FILESPACE_AGENT.QUEUE_MISSING",
                      "agents.filespace_capacity.queue_missing",
                      "durable page/filespace handoff queue is required");
  }

  FilespaceCapacityManagerTickResult result = Finish(
      FilespaceOkStatus(),
      FilespaceCapacityManagerDecisionKind::no_action,
      "FILESPACE_AGENT.NO_ACTION",
      "agents.filespace_capacity.no_action",
      "no waiting filespace capacity manager request was present",
      false);
  const u64 effective_capacity_window_pages =
      FilespaceCapacityManagerEffectiveWindowPages(snapshot, policy);
  u64 remaining_capacity_window_pages = effective_capacity_window_pages;
  result.capacity_window_pages = effective_capacity_window_pages;

  const std::size_t record_count = queue->records.size();
  for (std::size_t index = 0; index < record_count; ++index) {
    const auto request = queue->records[index].request;
    if (request.responding_agent != "filespace_capacity_manager") {
      ++result.skipped_records;
      continue;
    }
    if (request.state != page::PageFilespaceAgentRequestState::waiting_filespace_agent) {
      ++result.skipped_records;
      continue;
    }

    const auto current = ProcessWaitingRequest(queue,
                                               request,
                                               snapshot,
                                               policy,
                                               safety,
                                               remaining_capacity_window_pages);
    result.processed_records += current.processed_records;
    result.requested_pages += current.requested_pages;
    result.granted_pages += current.granted_pages;
    result.queue_mutated = result.queue_mutated || current.queue_mutated;
    result.physical_filespace_mutation_attempted =
        result.physical_filespace_mutation_attempted ||
        current.physical_filespace_mutation_attempted;
    result.page_ledger_mutation_attempted =
        result.page_ledger_mutation_attempted ||
        current.page_ledger_mutation_attempted;
    if (current.evidence.evidence_uuid.valid()) {
      result.evidence = current.evidence;
    }
    result.queue_record = current.queue_record;
    if (current.fail_closed || current.refused) {
      result.status = current.status;
      result.decision = FilespaceCapacityManagerDecisionKind::capacity_window_refused;
      result.diagnostic = current.diagnostic;
      result.fail_closed = true;
      result.refused = true;
      continue;
    }
    if (current.approved) {
      remaining_capacity_window_pages =
          current.granted_pages >= remaining_capacity_window_pages
              ? 0
              : remaining_capacity_window_pages - current.granted_pages;
      result.approved = true;
      if (!result.fail_closed) {
        result.status = current.status;
        result.decision = FilespaceCapacityManagerDecisionKind::capacity_window_approved;
        result.diagnostic = current.diagnostic;
      }
    }
  }

  return result;
}

FilespaceCapacityManagerActionResult EvaluateFilespaceCapacityManagerAction(
    const FilespaceCapacityManagerActionRequest& request,
    const FilespaceCapacityManagerMetricSnapshot& snapshot,
    const FilespaceCapacityManagerPolicy& policy,
    const FilespaceCapacityManagerSafetyState& safety) {
  const auto common = EvaluateCommonActionGates(request, snapshot, policy, safety);
  if (CommonActionResultReturned(common)) {
    return common;
  }

  const auto recommend_only = [&]() {
    return RefuseAction(request,
                        snapshot,
                        policy,
                        "FILESPACE_AGENT.ACTION_RECOMMEND_ONLY",
                        "agents.filespace_capacity.action_recommend_only",
                        std::string(FilespaceCapacityManagerActionKindName(request.action)) +
                            " requires explicit filespace capacity policy authority");
  };
  const auto missing_permission = [&](const std::string& right) {
    return RefuseAction(request,
                        snapshot,
                        policy,
                        "FILESPACE_AGENT.PERMISSION_DENIED",
                        "agents.filespace_capacity.permission_denied",
                        right);
  };
  const auto missing_evidence = [&](const std::string& detail) {
    return RefuseAction(request,
                        snapshot,
                        policy,
                        "FILESPACE_AGENT.EVIDENCE_REQUIRED",
                        "agents.filespace_capacity.evidence_required",
                        detail);
  };
  const auto stale_evidence = [&](const std::string& detail) {
    return RefuseAction(request,
                        snapshot,
                        policy,
                        "FILESPACE_AGENT.METRIC_STALE",
                        "agents.filespace_capacity.evidence_stale",
                        detail);
  };

  switch (request.action) {
    case FilespaceCapacityManagerActionKind::request_filespace_expand: {
      if (!RolePermitsCapacityWindow(snapshot.role_state) ||
          !HealthPermitsCapacityWindow(snapshot, policy)) {
        return RefuseAction(request,
                            snapshot,
                            policy,
                            "FILESPACE_AGENT.HEALTH_DENIED",
                            "agents.filespace_capacity.health_denied",
                            "filespace health or role state does not permit expansion");
      }
      if (!policy.expand_allowed || !policy.expand_request_policy_explicit) {
        return recommend_only();
      }
      if (request.target_bytes == 0 ||
          !request.capacity_proof_uuid.valid() ||
          !request.device_proof_uuid.valid()) {
        return missing_evidence("request_filespace_expand requires target bytes, capacity proof, and device proof");
      }
      if (!request.capacity_proof_fresh || !request.device_proof_fresh) {
        return stale_evidence("request_filespace_expand capacity or device proof is stale");
      }
      if (!request.live_action_requested) {
        return ApprovalRequired(request, snapshot, policy, "request_filespace_expand requires action approval before lifecycle dispatch");
      }
      if (!PermissionForLifecycleAction(request, false)) {
        return missing_permission(request.has_obs_agent_action_approve
                                      ? "FILESPACE_LIFECYCLE_CONTROL"
                                      : "OBS_AGENT_ACTION_APPROVE");
      }
      return AuthorizeAction(request,
                             snapshot,
                             policy,
                             "request_filespace_expand passed authority checks; physical growth remains lifecycle authority");
    }
    case FilespaceCapacityManagerActionKind::request_filespace_move: {
      if (!policy.move_allowed || !policy.move_request_policy_explicit) {
        return recommend_only();
      }
      if (!TypedUuidHasKind(request.source_filespace_uuid, UuidKind::filespace) ||
          !TypedUuidHasKind(request.target_filespace_uuid, UuidKind::filespace) ||
          !request.object_list_proof_uuid.valid() ||
          !request.startup_safety_state_present) {
        return missing_evidence("request_filespace_move requires source UUID, target UUID, object-list proof, and startup safety state");
      }
      if (!request.object_list_proof_fresh) {
        return stale_evidence("request_filespace_move object-list proof is stale");
      }
      if (!request.live_action_requested) {
        return ApprovalRequired(request, snapshot, policy, "request_filespace_move requires action approval before lifecycle dispatch");
      }
      if (!PermissionForLifecycleAction(request, false)) {
        return missing_permission(request.has_obs_agent_action_approve
                                      ? "FILESPACE_LIFECYCLE_CONTROL"
                                      : "OBS_AGENT_ACTION_APPROVE");
      }
      return AuthorizeAction(request,
                             snapshot,
                             policy,
                             "request_filespace_move passed authority checks; movement remains lifecycle/page-agent work");
    }
    case FilespaceCapacityManagerActionKind::request_filespace_shrink: {
      if (!policy.shrink_allowed || !policy.shrink_request_policy_explicit) {
        return recommend_only();
      }
      if (request.target_bytes == 0 ||
          !request.page_relocation_request_uuid.valid()) {
        return missing_evidence("request_filespace_shrink requires a target byte range and page relocation request UUID");
      }
      if (!request.page_agent_proof_fresh) {
        return stale_evidence("request_filespace_shrink page-agent proof is stale");
      }
      if (request.blocker_count != 0) {
        return RefuseAction(request,
                            snapshot,
                            policy,
                            "FILESPACE_AGENT.SHRINK_BLOCKED",
                            "agents.filespace_capacity.shrink_blocked",
                            "request_filespace_shrink requires page-agent blocker count zero");
      }
      if (!request.live_action_requested) {
        return ApprovalRequired(request, snapshot, policy, "request_filespace_shrink requires approval before page-agent dispatch");
      }
      if (!request.has_obs_agent_action_approve) {
        return missing_permission("OBS_AGENT_ACTION_APPROVE");
      }
      return AuthorizeAction(request,
                             snapshot,
                             policy,
                             "request_filespace_shrink passed authority checks as page-agent shrink preparation request");
    }
    case FilespaceCapacityManagerActionKind::request_filespace_truncate: {
      if (!policy.truncate_allowed || !policy.truncate_request_policy_explicit) {
        return recommend_only();
      }
      if (request.safe_tail_bytes == 0 ||
          !request.shrink_ready_evidence_uuid.valid()) {
        return missing_evidence("request_filespace_truncate requires publish_shrink_ready evidence and safe tail bytes");
      }
      if (!request.shrink_ready_evidence_fresh) {
        return stale_evidence("request_filespace_truncate shrink-ready evidence is stale");
      }
      if (request.blocker_count != 0) {
        return RefuseAction(request,
                            snapshot,
                            policy,
                            "FILESPACE_AGENT.SHRINK_BLOCKED",
                            "agents.filespace_capacity.truncate_blocked",
                            "request_filespace_truncate requires blocker count zero");
      }
      if (!request.live_action_requested) {
        return ApprovalRequired(request, snapshot, policy, "request_filespace_truncate requires approval before lifecycle dispatch");
      }
      if (!PermissionForLifecycleAction(request, true)) {
        return missing_permission(request.has_obs_agent_action_approve
                                      ? "FILESPACE_LIFECYCLE_TRUNCATE"
                                      : "OBS_AGENT_ACTION_APPROVE");
      }
      return AuthorizeAction(request,
                             snapshot,
                             policy,
                             "request_filespace_truncate passed authority checks; physical truncation remains lifecycle authority");
    }
    case FilespaceCapacityManagerActionKind::request_filespace_quarantine: {
      if (!policy.quarantine_allowed || !policy.quarantine_request_policy_explicit) {
        return recommend_only();
      }
      const bool proof_present = request.device_health_evidence_uuid.valid() ||
                                 request.checksum_evidence_uuid.valid() ||
                                 request.unknown_page_evidence_uuid.valid();
      if (!proof_present) {
        if (request.operator_review_requested) {
          return ApprovalRequired(request,
                                  snapshot,
                                  policy,
                                  "request_filespace_quarantine requires operator review without device/checksum/unknown-page proof");
        }
        return missing_evidence("request_filespace_quarantine requires device, checksum, unknown-page proof, or operator review");
      }
      if ((request.device_health_evidence_uuid.valid() &&
           !request.device_health_evidence_fresh) ||
          (request.checksum_evidence_uuid.valid() &&
           !request.checksum_evidence_fresh) ||
          (request.unknown_page_evidence_uuid.valid() &&
           !request.unknown_page_evidence_fresh)) {
        return stale_evidence("request_filespace_quarantine evidence is stale");
      }
      if (!request.live_action_requested) {
        return ApprovalRequired(request, snapshot, policy, "request_filespace_quarantine requires approval before lifecycle dispatch");
      }
      if (!request.has_obs_agent_action_approve &&
          !policy.critical_quarantine_automatic_allowed) {
        return missing_permission("OBS_AGENT_ACTION_APPROVE");
      }
      return AuthorizeAction(request,
                             snapshot,
                             policy,
                             "request_filespace_quarantine passed authority checks; quarantine remains lifecycle authority");
    }
    case FilespaceCapacityManagerActionKind::recommend_primary_shadow_promotion: {
      if (!request.has_obs_agent_recommendation_read) {
        return missing_permission("OBS_AGENT_RECOMMENDATION_READ");
      }
      if (!policy.shadow_promotion_allowed ||
          !policy.shadow_promotion_policy_explicit) {
        return FinishAction(request,
                            snapshot,
                            policy,
                            FilespaceOkStatus(),
                            FilespaceCapacityManagerDecisionKind::action_suppressed,
                            "FILESPACE_AGENT.ACTION_RECOMMEND_ONLY",
                            "agents.filespace_capacity.shadow_promotion_suppressed",
                            "primary shadow promotion recommendation suppressed by default policy",
                            false);
      }
      if (!request.primary_degradation_proof_uuid.valid() ||
          !request.candidate_readiness_proof_uuid.valid() ||
          !request.catalog_persistence_migration_requirement_present) {
        return FinishAction(request,
                            snapshot,
                            policy,
                            FilespaceOkStatus(),
                            FilespaceCapacityManagerDecisionKind::action_suppressed,
                            "FILESPACE_AGENT.EVIDENCE_REQUIRED",
                            "agents.filespace_capacity.shadow_promotion_proof_missing",
                            "promotion recommendation suppressed until degradation, candidate readiness, and catalog migration proof exist",
                            false);
      }
      if (!request.primary_degradation_proof_fresh ||
          !request.candidate_readiness_proof_fresh) {
        return stale_evidence("recommend_primary_shadow_promotion proof is stale");
      }
      return FinishAction(request,
                          snapshot,
                          policy,
                          FilespaceOkStatus(),
                          FilespaceCapacityManagerDecisionKind::action_recommended,
                          "FILESPACE_AGENT.RECOMMENDATION_RECORDED",
                          "agents.filespace_capacity.shadow_promotion_recommended",
                          "primary shadow promotion recommendation recorded without moving pages",
                          false);
    }
    case FilespaceCapacityManagerActionKind::forbidden_allocate_page:
    case FilespaceCapacityManagerActionKind::forbidden_relocate_page:
    case FilespaceCapacityManagerActionKind::forbidden_compact_page_family:
    case FilespaceCapacityManagerActionKind::forbidden_rebuild_index:
    case FilespaceCapacityManagerActionKind::forbidden_advance_mga_cleanup:
      return RefuseAction(request,
                          snapshot,
                          policy,
                          "FILESPACE_AGENT.PAGE_AUTHORITY_REQUIRED",
                          "agents.filespace_capacity.page_boundary_forbidden",
                          "filespace_capacity_manager must not perform page/index/MGA boundary actions");
  }

  return RefuseAction(request,
                      snapshot,
                      policy,
                      "FILESPACE_AGENT.POLICY_INVALID",
                      "agents.filespace_capacity.unknown_action",
                      "unknown filespace capacity action");
}

DiagnosticRecord MakeFilespaceCapacityManagerDiagnostic(Status status,
                                                        std::string diagnostic_code,
                                                        std::string message_key,
                                                        std::string detail) {
  return MakeDiagnostic(status.code,
                        status.severity,
                        status.subsystem,
                        std::move(diagnostic_code),
                        std::move(message_key),
                        detail.empty()
                            ? std::vector<scratchbird::core::platform::DiagnosticArgument>{}
                            : std::vector<scratchbird::core::platform::DiagnosticArgument>{
                                  {"detail", std::move(detail)}},
                        {},
                        "filespace_capacity_manager",
                        {});
}

StorageSpaceAgentDefaults filespace_capacity_manager_default_space_policy() {
  return DefaultStorageSpaceAgentDefaults();
}

bool filespace_capacity_manager_should_request_space(u64 available_pages) {
  const auto defaults = DefaultStorageSpaceAgentDefaults();
  return available_pages < defaults.filespace_min_available_pages;
}

bool filespace_capacity_manager_target_satisfied(u64 available_pages) {
  const auto defaults = DefaultStorageSpaceAgentDefaults();
  return available_pages >= defaults.filespace_target_available_pages;
}

u64 filespace_capacity_manager_page_allocation_notify_threshold_pages() {
  return DefaultStorageSpaceAgentDefaults().filespace_page_allocation_notify_pages;
}

}  // namespace scratchbird::core::agents::implemented_agents
