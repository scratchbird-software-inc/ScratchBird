// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "agents/storage_health_manager.hpp"

#include <string>
#include <utility>
#include <vector>

namespace scratchbird::core::agents::implemented_agents {
namespace {

using scratchbird::core::platform::MakeDiagnostic;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;
using scratchbird::core::platform::UuidKind;

Status StorageHealthOkStatus() {
  return {StatusCode::ok, Severity::info, Subsystem::engine};
}

Status StorageHealthRefuseStatus() {
  return {StatusCode::platform_required_feature_missing,
          Severity::error,
          Subsystem::engine};
}

bool SameTypedUuid(const TypedUuid& left, const TypedUuid& right) {
  return left.kind == right.kind && left.value == right.value;
}

bool TypedUuidHasKind(const TypedUuid& value, UuidKind kind) {
  return value.valid() && value.kind == kind;
}

bool RequestScopeIdentityValid(const StorageHealthManagerActionRequest& request) {
  return TypedUuidHasKind(request.database_uuid, UuidKind::database) &&
         TypedUuidHasKind(request.filespace_uuid, UuidKind::filespace) &&
         TypedUuidHasKind(request.policy_uuid, UuidKind::object);
}

bool SnapshotScopeIdentityValid(const StorageHealthManagerMetricSnapshot& snapshot) {
  return TypedUuidHasKind(snapshot.database_uuid, UuidKind::database) &&
         TypedUuidHasKind(snapshot.filespace_uuid, UuidKind::filespace) &&
         TypedUuidHasKind(snapshot.policy_uuid, UuidKind::object);
}

bool PolicyScopeIdentityValid(const StorageHealthManagerPolicy& policy) {
  return TypedUuidHasKind(policy.database_uuid, UuidKind::database) &&
         TypedUuidHasKind(policy.filespace_uuid, UuidKind::filespace) &&
         TypedUuidHasKind(policy.policy_uuid, UuidKind::object);
}

bool RequestScopeMatches(const StorageHealthManagerActionRequest& request,
                         const StorageHealthManagerMetricSnapshot& snapshot,
                         const StorageHealthManagerPolicy& policy) {
  return snapshot.scope_compatible &&
         policy.scope_compatible &&
         SameTypedUuid(request.database_uuid, snapshot.database_uuid) &&
         SameTypedUuid(request.filespace_uuid, snapshot.filespace_uuid) &&
         SameTypedUuid(request.policy_uuid, snapshot.policy_uuid) &&
         SameTypedUuid(snapshot.database_uuid, policy.database_uuid) &&
         SameTypedUuid(snapshot.filespace_uuid, policy.filespace_uuid) &&
         SameTypedUuid(snapshot.policy_uuid, policy.policy_uuid);
}

bool IsForbiddenBoundaryAction(StorageHealthManagerActionKind action) {
  switch (action) {
    case StorageHealthManagerActionKind::forbidden_request_filespace_expand:
    case StorageHealthManagerActionKind::forbidden_request_filespace_move:
    case StorageHealthManagerActionKind::forbidden_request_filespace_shrink:
    case StorageHealthManagerActionKind::forbidden_request_filespace_truncate:
    case StorageHealthManagerActionKind::forbidden_request_filespace_detach:
    case StorageHealthManagerActionKind::forbidden_request_filespace_delete:
    case StorageHealthManagerActionKind::forbidden_promote_filespace:
    case StorageHealthManagerActionKind::forbidden_demote_filespace:
    case StorageHealthManagerActionKind::forbidden_allocate_pages:
    case StorageHealthManagerActionKind::forbidden_relocate_pages:
    case StorageHealthManagerActionKind::forbidden_rebuild_indexes:
    case StorageHealthManagerActionKind::forbidden_override_filespace_policy:
    case StorageHealthManagerActionKind::forbidden_override_page_policy:
      return true;
    case StorageHealthManagerActionKind::request_filespace_quarantine:
    case StorageHealthManagerActionKind::update_storage_cost:
    case StorageHealthManagerActionKind::emit_storage_health_summary:
      return false;
  }
  return true;
}

StorageHealthManagerEvidence EvidenceForAction(
    const StorageHealthManagerActionRequest& request,
    const StorageHealthManagerPolicy& policy,
    StorageHealthManagerDecisionKind decision,
    std::string diagnostic_code,
    std::string evidence_state,
    std::string route_target,
    std::string reason) {
  StorageHealthManagerEvidence evidence;
  evidence.request_uuid = request.request_uuid;
  evidence.evidence_uuid = request.evidence_uuid;
  evidence.database_uuid = request.database_uuid;
  evidence.filespace_uuid = request.filespace_uuid;
  evidence.policy_uuid = request.policy_uuid.valid() ? request.policy_uuid : policy.policy_uuid;
  evidence.action = request.action;
  evidence.decision = decision;
  evidence.evidence_kind = request.evidence_kind;
  evidence.diagnostic_code = std::move(diagnostic_code);
  evidence.evidence_state = std::move(evidence_state);
  evidence.route_target = std::move(route_target);
  evidence.reason = std::move(reason);
  evidence.durable_state_changed = request.evidence_uuid.valid();
  return evidence;
}

StorageHealthManagerActionResult FinishAction(
    const StorageHealthManagerActionRequest& request,
    const StorageHealthManagerPolicy& policy,
    Status status,
    StorageHealthManagerDecisionKind decision,
    std::string diagnostic_code,
    std::string message_key,
    std::string detail,
    std::string route_target,
    bool fail_closed) {
  StorageHealthManagerActionResult result;
  result.status = status;
  result.action = request.action;
  result.decision = decision;
  result.fail_closed = fail_closed;
  result.refused = fail_closed ||
                   decision == StorageHealthManagerDecisionKind::action_refused;
  result.route_recommended =
      decision == StorageHealthManagerDecisionKind::quarantine_route_recommended;
  result.operator_review_required =
      decision == StorageHealthManagerDecisionKind::quarantine_operator_review_required;
  result.cost_update_recommended =
      decision == StorageHealthManagerDecisionKind::storage_cost_update_recommended;
  result.summary_emitted =
      decision == StorageHealthManagerDecisionKind::storage_health_summary_emitted;
  result.recommended = result.route_recommended ||
                       result.operator_review_required ||
                       result.cost_update_recommended;
  result.diagnostic = MakeStorageHealthManagerDiagnostic(
      status,
      diagnostic_code,
      std::move(message_key),
      detail);
  result.evidence = EvidenceForAction(
      request,
      policy,
      decision,
      diagnostic_code,
      StorageHealthManagerDecisionKindName(decision),
      std::move(route_target),
      std::move(detail));
  return result;
}

StorageHealthManagerActionResult RefuseAction(
    const StorageHealthManagerActionRequest& request,
    const StorageHealthManagerPolicy& policy,
    std::string diagnostic_code,
    std::string message_key,
    std::string detail) {
  return FinishAction(request,
                      policy,
                      StorageHealthRefuseStatus(),
                      StorageHealthManagerDecisionKind::action_refused,
                      std::move(diagnostic_code),
                      std::move(message_key),
                      std::move(detail),
                      {},
                      true);
}

StorageHealthManagerActionResult RecommendAction(
    const StorageHealthManagerActionRequest& request,
    const StorageHealthManagerPolicy& policy,
    StorageHealthManagerDecisionKind decision,
    std::string diagnostic_code,
    std::string message_key,
    std::string detail,
    std::string route_target) {
  return FinishAction(request,
                      policy,
                      StorageHealthOkStatus(),
                      decision,
                      std::move(diagnostic_code),
                      std::move(message_key),
                      std::move(detail),
                      std::move(route_target),
                      false);
}

bool CheckMetricGroup(bool present,
                      bool fresh,
                      bool trusted,
                      const char* group,
                      std::string* diagnostic_code,
                      std::string* message_key,
                      std::string* detail) {
  if (!present) {
    *diagnostic_code = "STORAGE_HEALTH_MANAGER.METRIC_MISSING";
    *message_key = "agents.storage_health.metric_missing";
    *detail = std::string(group) + " metric evidence is required by storage_health_policy";
    return false;
  }
  if (!fresh) {
    *diagnostic_code = "STORAGE_HEALTH_MANAGER.METRIC_STALE";
    *message_key = "agents.storage_health.metric_stale";
    *detail = std::string(group) + " metric evidence is stale";
    return false;
  }
  if (!trusted) {
    *diagnostic_code = "STORAGE_HEALTH_MANAGER.METRIC_UNTRUSTED";
    *message_key = "agents.storage_health.metric_untrusted";
    *detail = std::string(group) + " metric evidence is untrusted";
    return false;
  }
  return true;
}

bool ValidateCommonMetricGates(const StorageHealthManagerMetricSnapshot& snapshot,
                               bool require_latency,
                               bool require_page_metric,
                               std::string* diagnostic_code,
                               std::string* message_key,
                               std::string* detail) {
  if (!CheckMetricGroup(snapshot.filespace_health_present,
                        snapshot.filespace_health_fresh,
                        snapshot.filespace_health_trusted,
                        "filespace health",
                        diagnostic_code,
                        message_key,
                        detail)) {
    return false;
  }
  if (!CheckMetricGroup(snapshot.device_error_present,
                        snapshot.device_error_fresh,
                        snapshot.device_error_trusted,
                        "storage device error",
                        diagnostic_code,
                        message_key,
                        detail)) {
    return false;
  }
  if (!CheckMetricGroup(snapshot.checksum_present,
                        snapshot.checksum_fresh,
                        snapshot.checksum_trusted,
                        "checksum",
                        diagnostic_code,
                        message_key,
                        detail)) {
    return false;
  }
  if (!CheckMetricGroup(snapshot.unknown_page_present,
                        snapshot.unknown_page_fresh,
                        snapshot.unknown_page_trusted,
                        "unknown-page",
                        diagnostic_code,
                        message_key,
                        detail)) {
    return false;
  }
  if (require_page_metric && snapshot.page_metric_applicable &&
      !CheckMetricGroup(snapshot.page_metric_present,
                        snapshot.page_metric_fresh,
                        snapshot.page_metric_trusted,
                        "page allocation health",
                        diagnostic_code,
                        message_key,
                        detail)) {
    return false;
  }
  if (require_latency &&
      !CheckMetricGroup(snapshot.storage_latency_present,
                        snapshot.storage_latency_fresh,
                        snapshot.storage_latency_trusted,
                        "storage latency",
                        diagnostic_code,
                        message_key,
                        detail)) {
    return false;
  }
  return true;
}

bool RequestEvidencePresent(const StorageHealthManagerActionRequest& request) {
  return request.explicit_evidence &&
         request.request_uuid.valid() &&
         request.evidence_uuid.valid() &&
         request.metric_evidence_uuid.valid();
}

bool QuarantineSignalPresent(const StorageHealthManagerActionRequest& request,
                             const StorageHealthManagerMetricSnapshot& snapshot,
                             const StorageHealthManagerPolicy& policy) {
  if (request.evidence_kind == StorageHealthEvidenceKind::checksum_failure) {
    return snapshot.checksum_failure_count >=
           policy.checksum_failure_quarantine_threshold;
  }
  if (request.evidence_kind == StorageHealthEvidenceKind::unknown_page) {
    return snapshot.unknown_page_count >= policy.unknown_page_quarantine_threshold;
  }
  if (request.evidence_kind == StorageHealthEvidenceKind::device_error) {
    return snapshot.device_error_count >= policy.device_error_quarantine_threshold;
  }
  return false;
}

StorageHealthManagerActionResult EvaluateCommonGates(
    const StorageHealthManagerActionRequest& request,
    const StorageHealthManagerMetricSnapshot& snapshot,
    const StorageHealthManagerPolicy& policy) {
  if (IsForbiddenBoundaryAction(request.action)) {
    return RefuseAction(
        request,
        policy,
        "STORAGE_HEALTH_MANAGER.FORBIDDEN_ACTION",
        "agents.storage_health.forbidden_action",
        std::string(StorageHealthManagerActionKindName(request.action)) +
            " belongs to filespace_capacity_manager, page_allocation_manager, index_health_manager, or operator policy authority; storage_health_manager may only emit recommendation evidence");
  }
  if (!policy.present || !policy.valid) {
    return RefuseAction(request,
                        policy,
                        "STORAGE_HEALTH_MANAGER.POLICY_REQUIRED",
                        "agents.storage_health.policy_required",
                        "storage_health_policy is required before any storage health manager decision");
  }
  if (!RequestScopeIdentityValid(request) ||
      !SnapshotScopeIdentityValid(snapshot) ||
      !PolicyScopeIdentityValid(policy)) {
    return RefuseAction(request,
                        policy,
                        "STORAGE_HEALTH_MANAGER.SCOPE_REQUIRED",
                        "agents.storage_health.scope_required",
                        "database, filespace, and storage_health_policy UUID scope are required");
  }
  if (!RequestScopeMatches(request, snapshot, policy)) {
    return RefuseAction(request,
                        policy,
                        "STORAGE_HEALTH_MANAGER.SCOPE_MISMATCH",
                        "agents.storage_health.scope_mismatch",
                        "request, metric snapshot, and storage_health_policy UUID scope must match exactly");
  }
  if (!RequestEvidencePresent(request)) {
    return RefuseAction(request,
                        policy,
                        "STORAGE_HEALTH_MANAGER.EVIDENCE_REQUIRED",
                        "agents.storage_health.evidence_required",
                        "explicit request, metric, and decision evidence UUIDs are required before success");
  }
  if (!request.metric_evidence_fresh) {
    return RefuseAction(request,
                        policy,
                        "STORAGE_HEALTH_MANAGER.METRIC_STALE",
                        "agents.storage_health.metric_stale",
                        "request metric evidence is stale");
  }
  if (!request.metric_evidence_trusted) {
    return RefuseAction(request,
                        policy,
                        "STORAGE_HEALTH_MANAGER.METRIC_UNTRUSTED",
                        "agents.storage_health.metric_untrusted",
                        "request metric evidence is untrusted");
  }
  return StorageHealthManagerActionResult{};
}

bool CommonGateReturned(const StorageHealthManagerActionResult& result) {
  return result.fail_closed || result.refused || !result.diagnostic.diagnostic_code.empty();
}

}  // namespace

// SEARCH_KEY: SB_AGENT_IMPLEMENTATION_storage_health_manager
// Canonical storage_health_manager behavior is registered in CanonicalAgentRegistry() and
// executed through the shared fail-closed agent runtime substrate. The manager is a
// recommendation-only summarizer; filespace lifecycle actions belong to
// filespace_capacity_manager and page allocation/relocation actions belong to
// page_allocation_manager.
const char* storage_health_manager_implementation_anchor() { return "storage_health_manager"; }

const char* StorageHealthManagerDecisionKindName(
    StorageHealthManagerDecisionKind kind) {
  switch (kind) {
    case StorageHealthManagerDecisionKind::no_action:
      return "no_action";
    case StorageHealthManagerDecisionKind::quarantine_route_recommended:
      return "quarantine_route_recommended";
    case StorageHealthManagerDecisionKind::quarantine_operator_review_required:
      return "quarantine_operator_review_required";
    case StorageHealthManagerDecisionKind::storage_cost_update_recommended:
      return "storage_cost_update_recommended";
    case StorageHealthManagerDecisionKind::storage_health_summary_emitted:
      return "storage_health_summary_emitted";
    case StorageHealthManagerDecisionKind::action_refused:
      return "action_refused";
  }
  return "unknown";
}

const char* StorageHealthManagerActionKindName(StorageHealthManagerActionKind action) {
  switch (action) {
    case StorageHealthManagerActionKind::request_filespace_quarantine:
      return "request_filespace_quarantine";
    case StorageHealthManagerActionKind::update_storage_cost:
      return "update_storage_cost";
    case StorageHealthManagerActionKind::emit_storage_health_summary:
      return "emit_storage_health_summary";
    case StorageHealthManagerActionKind::forbidden_request_filespace_expand:
      return "forbidden_request_filespace_expand";
    case StorageHealthManagerActionKind::forbidden_request_filespace_move:
      return "forbidden_request_filespace_move";
    case StorageHealthManagerActionKind::forbidden_request_filespace_shrink:
      return "forbidden_request_filespace_shrink";
    case StorageHealthManagerActionKind::forbidden_request_filespace_truncate:
      return "forbidden_request_filespace_truncate";
    case StorageHealthManagerActionKind::forbidden_request_filespace_detach:
      return "forbidden_request_filespace_detach";
    case StorageHealthManagerActionKind::forbidden_request_filespace_delete:
      return "forbidden_request_filespace_delete";
    case StorageHealthManagerActionKind::forbidden_promote_filespace:
      return "forbidden_promote_filespace";
    case StorageHealthManagerActionKind::forbidden_demote_filespace:
      return "forbidden_demote_filespace";
    case StorageHealthManagerActionKind::forbidden_allocate_pages:
      return "forbidden_allocate_pages";
    case StorageHealthManagerActionKind::forbidden_relocate_pages:
      return "forbidden_relocate_pages";
    case StorageHealthManagerActionKind::forbidden_rebuild_indexes:
      return "forbidden_rebuild_indexes";
    case StorageHealthManagerActionKind::forbidden_override_filespace_policy:
      return "forbidden_override_filespace_policy";
    case StorageHealthManagerActionKind::forbidden_override_page_policy:
      return "forbidden_override_page_policy";
  }
  return "unknown";
}

const char* StorageHealthSeverityName(StorageHealthSeverity severity) {
  switch (severity) {
    case StorageHealthSeverity::unknown:
      return "unknown";
    case StorageHealthSeverity::healthy:
      return "healthy";
    case StorageHealthSeverity::degraded:
      return "degraded";
    case StorageHealthSeverity::critical:
      return "critical";
    case StorageHealthSeverity::failed:
      return "failed";
  }
  return "unknown";
}

const char* StorageHealthEvidenceKindName(StorageHealthEvidenceKind kind) {
  switch (kind) {
    case StorageHealthEvidenceKind::none:
      return "none";
    case StorageHealthEvidenceKind::checksum_failure:
      return "checksum_failure";
    case StorageHealthEvidenceKind::unknown_page:
      return "unknown_page";
    case StorageHealthEvidenceKind::device_error:
      return "device_error";
    case StorageHealthEvidenceKind::latency_histogram:
      return "latency_histogram";
    case StorageHealthEvidenceKind::health_summary:
      return "health_summary";
  }
  return "unknown";
}

StorageHealthManagerPolicy DefaultStorageHealthManagerPolicy() {
  return StorageHealthManagerPolicy{};
}

StorageHealthManagerActionResult EvaluateStorageHealthManagerAction(
    const StorageHealthManagerActionRequest& request,
    const StorageHealthManagerMetricSnapshot& snapshot,
    const StorageHealthManagerPolicy& policy) {
  const auto common = EvaluateCommonGates(request, snapshot, policy);
  if (CommonGateReturned(common)) {
    return common;
  }

  std::string diagnostic_code;
  std::string message_key;
  std::string detail;

  switch (request.action) {
    case StorageHealthManagerActionKind::request_filespace_quarantine: {
      if (!ValidateCommonMetricGates(snapshot,
                                     false,
                                     false,
                                     &diagnostic_code,
                                     &message_key,
                                     &detail)) {
        return RefuseAction(request, policy, diagnostic_code, message_key, detail);
      }
      if (!policy.quarantine_recommendation_allowed) {
        return RefuseAction(request,
                            policy,
                            "STORAGE_HEALTH_MANAGER.POLICY_DEFAULT_DENY",
                            "agents.storage_health.policy_default_deny",
                            "storage_health_policy did not allow quarantine recommendation emission");
      }
      if (!QuarantineSignalPresent(request, snapshot, policy)) {
        return RefuseAction(request,
                            policy,
                            "STORAGE_HEALTH_MANAGER.EVIDENCE_REQUIRED",
                            "agents.storage_health.evidence_required",
                            "request_filespace_quarantine requires fresh trusted checksum, unknown-page, or device-error evidence at policy threshold");
      }
      if (policy.critical_automatic_quarantine_policy &&
          !request.operator_review_requested) {
        return RecommendAction(
            request,
            policy,
            StorageHealthManagerDecisionKind::quarantine_route_recommended,
            "STORAGE_HEALTH_MANAGER.QUARANTINE_ROUTE_RECOMMENDATION",
            "agents.storage_health.quarantine_route_recommendation",
            "critical automatic quarantine policy produced a route/recommendation to filespace_capacity_manager only; no physical lifecycle mutation was attempted",
            "filespace_capacity_manager");
      }
      if (!policy.operator_review_route_allowed) {
        return RefuseAction(request,
                            policy,
                            "STORAGE_HEALTH_MANAGER.POLICY_DEFAULT_DENY",
                            "agents.storage_health.policy_default_deny",
                            "storage_health_policy did not allow operator-review quarantine routing");
      }
      return RecommendAction(
          request,
          policy,
          StorageHealthManagerDecisionKind::quarantine_operator_review_required,
          "STORAGE_HEALTH_MANAGER.QUARANTINE_OPERATOR_REVIEW",
          "agents.storage_health.quarantine_operator_review",
          "quarantine evidence produced an operator-review recommendation only; storage_health_manager did not dispatch filespace lifecycle mutation",
          "operator_review");
    }
    case StorageHealthManagerActionKind::update_storage_cost: {
      if (!ValidateCommonMetricGates(snapshot,
                                     true,
                                     false,
                                     &diagnostic_code,
                                     &message_key,
                                     &detail)) {
        return RefuseAction(request, policy, diagnostic_code, message_key, detail);
      }
      if (!policy.storage_cost_recommendation_allowed) {
        return RefuseAction(request,
                            policy,
                            "STORAGE_HEALTH_MANAGER.POLICY_DEFAULT_DENY",
                            "agents.storage_health.policy_default_deny",
                            "storage_health_policy did not allow optimizer-cost recommendations");
      }
      if (request.evidence_kind != StorageHealthEvidenceKind::latency_histogram ||
          snapshot.fsync_latency_p99_microseconds <
              policy.fsync_p99_cost_update_threshold_microseconds) {
        return RefuseAction(request,
                            policy,
                            "STORAGE_HEALTH_MANAGER.EVIDENCE_REQUIRED",
                            "agents.storage_health.evidence_required",
                            "update_storage_cost requires fresh trusted storage latency histogram evidence at policy threshold");
      }
      return RecommendAction(
          request,
          policy,
          StorageHealthManagerDecisionKind::storage_cost_update_recommended,
          "STORAGE_HEALTH_MANAGER.COST_RECOMMENDATION",
          "agents.storage_health.cost_recommendation",
          "optimizer storage cost update request emitted as recommendation evidence only; no physical filespace, page, or optimizer registry mutation was attempted",
          "optimizer_cost_registry");
    }
    case StorageHealthManagerActionKind::emit_storage_health_summary: {
      if (!ValidateCommonMetricGates(snapshot,
                                     true,
                                     true,
                                     &diagnostic_code,
                                     &message_key,
                                     &detail)) {
        return RefuseAction(request, policy, diagnostic_code, message_key, detail);
      }
      if (!policy.health_summary_allowed) {
        return RefuseAction(request,
                            policy,
                            "STORAGE_HEALTH_MANAGER.POLICY_DEFAULT_DENY",
                            "agents.storage_health.policy_default_deny",
                            "storage_health_policy did not allow health summary emission");
      }
      return RecommendAction(
          request,
          policy,
          StorageHealthManagerDecisionKind::storage_health_summary_emitted,
          "STORAGE_HEALTH_MANAGER.HEALTH_SUMMARY_EMITTED",
          "agents.storage_health.health_summary_emitted",
          std::string("storage health summary emitted with filespace_health=") +
              StorageHealthSeverityName(snapshot.filespace_health) +
              "; summary/recommendation evidence only, no physical lifecycle or page mutation",
          "storage_health_evidence");
    }
    case StorageHealthManagerActionKind::forbidden_request_filespace_expand:
    case StorageHealthManagerActionKind::forbidden_request_filespace_move:
    case StorageHealthManagerActionKind::forbidden_request_filespace_shrink:
    case StorageHealthManagerActionKind::forbidden_request_filespace_truncate:
    case StorageHealthManagerActionKind::forbidden_request_filespace_detach:
    case StorageHealthManagerActionKind::forbidden_request_filespace_delete:
    case StorageHealthManagerActionKind::forbidden_promote_filespace:
    case StorageHealthManagerActionKind::forbidden_demote_filespace:
    case StorageHealthManagerActionKind::forbidden_allocate_pages:
    case StorageHealthManagerActionKind::forbidden_relocate_pages:
    case StorageHealthManagerActionKind::forbidden_rebuild_indexes:
    case StorageHealthManagerActionKind::forbidden_override_filespace_policy:
    case StorageHealthManagerActionKind::forbidden_override_page_policy:
      break;
  }

  return RefuseAction(request,
                      policy,
                      "STORAGE_HEALTH_MANAGER.FORBIDDEN_ACTION",
                      "agents.storage_health.forbidden_action",
                      "unknown or forbidden storage health manager action");
}

DiagnosticRecord MakeStorageHealthManagerDiagnostic(Status status,
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
                        "storage_health_manager",
                        status.ok()
                            ? std::string{}
                            : "attach valid storage_health_policy and refresh trusted storage/filespace/page evidence");
}

}  // namespace scratchbird::core::agents::implemented_agents
