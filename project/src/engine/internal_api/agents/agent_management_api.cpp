// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "agents/agent_management_api.hpp"

#include "agents/agent_durable_catalog_store_api.hpp"
#include "agent_metric_runtime.hpp"
#include "agent_runtime.hpp"
#include "agent_production_fixture_separation.hpp"
#include "behavior_support/api_behavior_store.hpp"
#include "catalog/sys_information_projection.hpp"
#include "cluster_provider/cluster_provider.hpp"
#include "security/security_model.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <cctype>
#include <optional>
#include <sstream>
#include <string_view>
#include <utility>

namespace scratchbird::engine::internal_api {
namespace {

using scratchbird::core::agents::AgentActionClass;
using scratchbird::core::agents::AgentActionRequest;
using scratchbird::core::agents::AgentActionResultClassName;
using scratchbird::core::agents::AgentActivationProfileName;
using scratchbird::core::agents::AgentAuthorityClassName;
using scratchbird::core::agents::AgentCatalogRecordLayouts;
using scratchbird::core::agents::AgentDeploymentName;
using scratchbird::core::agents::AgentEvidenceRecord;
using scratchbird::core::agents::AgentSecurityCommandFamily;
using scratchbird::core::agents::AgentFeatureAvailabilityName;
using scratchbird::core::agents::AgentInstanceRecord;
using scratchbird::core::agents::AgentLifecycleMode;
using scratchbird::core::agents::AgentLifecycleState;
using scratchbird::core::agents::AgentLifecycleStateName;
using scratchbird::core::agents::AgentMetricRuntimeMode;
using scratchbird::core::agents::AgentMetricSnapshotEvaluationOptions;
using scratchbird::core::agents::AgentMetricSourceQuality;
using scratchbird::core::agents::AgentObservedMetricSnapshot;
using scratchbird::core::agents::AgentPolicy;
using scratchbird::core::agents::AgentRuntimeContext;
using scratchbird::core::agents::AgentSecurityDenialKindName;
using scratchbird::core::agents::AgentTypeDescriptor;
using scratchbird::core::agents::BaselinePolicyForAgent;
using scratchbird::core::agents::CanonicalAgentRegistry;
using scratchbird::core::agents::DurableAgentCatalogImage;
using scratchbird::core::agents::DurableAgentActionState;
using scratchbird::core::agents::DurableAgentActionStateName;
using scratchbird::core::agents::DurableAgentResourceReservationRequest;
using scratchbird::core::agents::DurableAgentHealthRecord;
using scratchbird::core::agents::DurableAgentHistoryRecord;
using scratchbird::core::agents::DurableAgentLeaseState;
using scratchbird::core::agents::DurableAgentResourceReservationState;
using scratchbird::core::agents::EffectiveActivationForLifecycle;
using scratchbird::core::agents::AcquireDurableAgentResourceReservation;
using scratchbird::core::agents::ReleaseDurableAgentResourceReservation;
using scratchbird::core::agents::EvaluateAgentCommandGrant;
using scratchbird::core::agents::EvaluateAgentAction;
using scratchbird::core::agents::EvaluateAgentFeatureAvailability;
using scratchbird::core::agents::EvaluateAgentObservedMetricSnapshots;
using scratchbird::core::agents::FindAgentType;
using scratchbird::core::agents::MakeAgentEvidence;
using scratchbird::core::agents::RecordAgentRuntimeMetric;
using scratchbird::core::agents::ResolveAgentMetricDependencies;
using scratchbird::core::agents::ResolveAgentTimeAuthority;
using scratchbird::core::agents::AgentProductionFixtureSeparationInput;
using scratchbird::core::agents::DeterministicAgentRuntimeObjectUuidFromKey;
using scratchbird::core::agents::ValidateAgentProductionFixtureSeparation;
using scratchbird::core::agents::ValidateAgentPolicy;
using scratchbird::core::agents::ValidateAgentSecurity;
using scratchbird::core::agents::ValidateCanonicalAgentRegistry;
using scratchbird::core::agents::ValidateDurableAgentCatalogForProduction;
using scratchbird::core::agents::ValidateRolloutTransition;

namespace cluster_provider = scratchbird::engine::cluster_provider;
namespace platform = scratchbird::core::platform;
namespace uuid = scratchbird::core::uuid;

struct AgentCommandSurfaceSpec {
  const char* operation_id;
  const char* opcode;
  const char* api_call;
  const char* command_family;
  const char* required_right_primary;
  const char* required_right_secondary;
  const char* evidence_kind;
  const char* result_kind;
  const char* success_state;
  const char* refusal_code;
  const char* permission_code;
  bool cluster_scoped;
  bool mutating;
};

// SEARCH_KEY: EngineAgentCommandSurfaceOperation
constexpr AgentCommandSurfaceSpec kAgentCommandSurfaceSpecs[] = {
    {"agents.metrics.get", "SBLR_AGENT_METRICS_GET", "sb_api_agent_metrics_get", "agent_read",
     "OBS_AGENT_STATE_READ", "OBS_METRICS_READ_FAMILY", "", "agent.metrics.v1", "",
     "METRIC.STALE", "METRIC.PERMISSION_DENIED", false, false},
    {"agents.policy.get", "SBLR_AGENT_POLICY_GET", "sb_api_agent_policy_get", "agent_policy_read",
     "OBS_POLICY_READ", "", "", "agent.policy.v1", "", "POLICY.NOT_FOUND",
     "POLICY.PERMISSION_DENIED", false, false},
    {"agents.evidence.list", "SBLR_AGENT_EVIDENCE_LIST", "sb_api_agent_evidence_list", "agent_evidence",
     "OBS_AGENT_EVIDENCE_READ", "", "agent_read_evidence", "agent.evidence.v1", "",
     "AGENT.EVIDENCE_NOT_FOUND", "AGENT.PERMISSION_DENIED", false, false},
    {"agents.audit.list", "SBLR_AGENT_AUDIT_LIST", "sb_api_agent_audit_list", "agent_audit",
     "OBS_AGENT_EVIDENCE_READ", "", "agent_read_evidence", "agent.audit.v1", "",
     "AGENT.AUDIT_NOT_FOUND", "AGENT.PERMISSION_DENIED", false, false},
    {"agents.actions.list", "SBLR_AGENT_ACTION_LIST", "sb_api_agent_action_list", "agent_action_read",
     "OBS_AGENT_RECOMMENDATION_READ", "", "", "agent.actions.v1", "empty",
     "AGENT.ACTION_NOT_FOUND", "ACTION.PERMISSION_DENIED", false, false},
    {"agents.overrides.list", "SBLR_AGENT_OVERRIDE_LIST", "sb_api_agent_override_list", "agent_override_read",
     "OBS_AGENT_STATE_READ", "", "", "agent.overrides.v1", "empty", "OVERRIDE.NOT_FOUND",
     "AGENT.PERMISSION_DENIED", false, false},
    {"agents.drain", "SBLR_AGENT_LIFECYCLE_DRAIN", "sb_api_agent_drain", "agent_lifecycle",
     "OBS_AGENT_CONTROL", "", "agent_transition_evidence", "agent.command_status.v1", "",
     "AGENT.DRAIN_NOT_SUPPORTED", "AGENT.PERMISSION_DENIED", false, true},
    {"agents.restart", "SBLR_AGENT_LIFECYCLE_RESTART", "sb_api_agent_restart", "agent_lifecycle",
     "OBS_AGENT_CONTROL", "", "agent_transition_evidence", "agent.command_status.v1", "",
     "AGENT.RESTART_REFUSED", "AGENT.PERMISSION_DENIED", false, true},
    {"agents.enable", "SBLR_AGENT_LIFECYCLE_ENABLE", "sb_api_agent_enable", "agent_lifecycle",
     "OBS_AGENT_CONTROL", "", "agent_transition_evidence", "agent.command_status.v1", "",
     "AGENT.ENABLE_REFUSED", "AGENT.PERMISSION_DENIED", false, true},
    {"agents.disable", "SBLR_AGENT_LIFECYCLE_DISABLE", "sb_api_agent_disable", "agent_lifecycle",
     "OBS_AGENT_CONTROL", "", "agent_transition_evidence", "agent.command_status.v1", "",
     "AGENT.DISABLE_REFUSED", "AGENT.PERMISSION_DENIED", false, true},
    {"agents.quarantine", "SBLR_AGENT_QUARANTINE", "sb_api_agent_quarantine", "agent_quarantine",
     "OBS_AGENT_CONTROL", "", "agent_quarantine_evidence", "agent.command_status.v1", "",
     "AGENT.ALREADY_QUARANTINED", "AGENT.PERMISSION_DENIED", false, true},
    {"agents.unquarantine", "SBLR_AGENT_UNQUARANTINE", "sb_api_agent_unquarantine", "agent_quarantine",
     "OBS_AGENT_CONTROL", "", "agent_quarantine_evidence", "agent.command_status.v1", "",
     "AGENT.NOT_QUARANTINED", "AGENT.PERMISSION_DENIED", false, true},
    {"agents.policy.attach", "SBLR_AGENT_POLICY_ATTACH", "sb_api_agent_policy_attach", "agent_policy",
     "OBS_POLICY_APPLY", "OBS_AGENT_CONTROL", "agent_policy_attach_evidence", "agent.command_status.v1", "",
     "POLICY.INVALID", "POLICY.PERMISSION_DENIED", false, true},
    {"agents.policy.detach", "SBLR_AGENT_POLICY_DETACH", "sb_api_agent_policy_detach", "agent_policy",
     "OBS_POLICY_APPLY", "OBS_AGENT_CONTROL", "agent_policy_attach_evidence", "agent.command_status.v1", "",
     "POLICY.REQUIRED", "POLICY.PERMISSION_DENIED", false, true},
    {"agents.policy.validate", "SBLR_AGENT_POLICY_VALIDATE", "sb_api_agent_policy_validate", "agent_policy",
     "OBS_POLICY_VALIDATE", "", "policy_validation_evidence", "agent.policy_validation.v1", "",
     "POLICY.INVALID", "POLICY.PERMISSION_DENIED", false, false},
    {"agents.policy.simulate", "SBLR_AGENT_POLICY_SIMULATE", "sb_api_agent_policy_simulate", "agent_policy",
     "OBS_POLICY_SIMULATE", "", "policy_simulation_evidence", "agent.policy_simulation.v1", "",
     "POLICY.INVALID", "POLICY.PERMISSION_DENIED", false, false},
    {"agents.policy.apply", "SBLR_AGENT_POLICY_APPLY", "sb_api_agent_policy_apply", "agent_policy",
     "OBS_POLICY_APPROVE", "OBS_POLICY_APPLY", "agent_policy_apply_evidence", "agent.command_status.v1", "",
     "POLICY.NOT_APPROVED", "POLICY.PERMISSION_DENIED", false, true},
    {"agents.policy.rollback", "SBLR_AGENT_POLICY_ROLLBACK", "sb_api_agent_policy_rollback", "agent_policy",
     "OBS_POLICY_ROLLBACK", "", "agent_policy_rollback_evidence", "agent.command_status.v1", "",
     "POLICY.ROLLBACK_REFUSED", "POLICY.PERMISSION_DENIED", false, true},
    {"agents.action.approve", "SBLR_AGENT_ACTION_APPROVE", "sb_api_agent_action_approve", "agent_action",
     "OBS_AGENT_ACTION_APPROVE", "", "agent_action_approval_evidence", "agent.command_status.v1", "",
     "ACTION.EXPIRED", "ACTION.PERMISSION_DENIED", false, true},
    {"agents.action.cancel", "SBLR_AGENT_ACTION_CANCEL", "sb_api_agent_action_cancel", "agent_action",
     "OBS_AGENT_ACTION_CANCEL", "", "agent_action_cancel_evidence", "agent.command_status.v1", "",
     "ACTION.NOT_CANCELLABLE", "ACTION.PERMISSION_DENIED", false, true},
    {"agents.action.retry", "SBLR_AGENT_ACTION_RETRY", "sb_api_agent_action_retry", "agent_action",
     "OBS_AGENT_ACTION_APPROVE", "", "agent_action_retry_evidence", "agent.command_status.v1", "",
     "ACTION.RETRY_COOLDOWN_ACTIVE", "ACTION.PERMISSION_DENIED", false, true},
    {"agents.action.suppress", "SBLR_AGENT_ACTION_SUPPRESS", "sb_api_agent_action_suppress", "agent_action",
     "OBS_AGENT_OVERRIDE", "", "agent_override_evidence", "agent.command_status.v1", "",
     "OVERRIDE.EXPIRY_TOO_LONG", "ACTION.PERMISSION_DENIED", false, true},
    {"agents.override.create", "SBLR_AGENT_OVERRIDE_CREATE", "sb_api_agent_override_create", "agent_override",
     "OBS_AGENT_OVERRIDE", "", "agent_override_evidence", "agent.command_status.v1", "",
     "OVERRIDE.EXPIRY_TOO_LONG", "AGENT.PERMISSION_DENIED", false, true},
    {"agents.override.update", "SBLR_AGENT_OVERRIDE_UPDATE", "sb_api_agent_override_update", "agent_override",
     "OBS_AGENT_OVERRIDE", "", "agent_override_evidence", "agent.command_status.v1", "",
     "OVERRIDE.INVALID_PATCH", "AGENT.PERMISSION_DENIED", false, true},
    {"agents.override.drop", "SBLR_AGENT_OVERRIDE_DROP", "sb_api_agent_override_drop", "agent_override",
     "OBS_AGENT_OVERRIDE", "", "agent_override_evidence", "agent.command_status.v1", "",
     "OVERRIDE.NOT_FOUND", "AGENT.PERMISSION_DENIED", false, true},
    {"agents.set_mode", "SBLR_AGENT_SET_MODE", "sb_api_agent_set_mode", "agent_lifecycle",
     "OBS_AGENT_CONTROL", "", "agent_transition_evidence", "agent.command_status.v1", "",
     "AGENT.MODE_REFUSED", "AGENT.PERMISSION_DENIED", false, true},
    {"filespaces.show", "SBLR_SHOW_FILESPACES", "sb_api_filespace_list", "filespace_agent",
     "OBS_CONFIG_INSPECT", "", "", "filespace.list.v1", "empty", "FILESPACE.PERMISSION_DENIED",
     "FILESPACE.PERMISSION_DENIED", false, false},
    {"filespaces.health.show", "SBLR_SHOW_FILESPACE_HEALTH", "sb_api_filespace_health_get", "filespace_agent",
     "OBS_METRICS_READ_FAMILY", "", "", "filespace.health.v1", "", "FILESPACE.NOT_FOUND",
     "METRIC.PERMISSION_DENIED", false, false},
    {"filespaces.capacity.show", "SBLR_SHOW_FILESPACE_CAPACITY", "sb_api_filespace_capacity_get", "filespace_agent",
     "OBS_METRICS_READ_FAMILY", "", "", "filespace.capacity.v1", "", "FILESPACE.NOT_FOUND",
     "METRIC.PERMISSION_DENIED", false, false},
    {"pages.allocation.show", "SBLR_SHOW_PAGE_ALLOCATION", "sb_api_page_allocation_get", "page_agent",
     "OBS_METRICS_READ_FAMILY", "", "", "page.allocation.v1", "", "METRIC.STALE",
     "METRIC.PERMISSION_DENIED", false, false},
    {"pages.allocation.family.show", "SBLR_SHOW_PAGE_ALLOCATION_BY_FAMILY", "sb_api_page_allocation_family_get", "page_agent",
     "OBS_METRICS_READ_FAMILY", "", "", "page.allocation_family.v1", "", "METRIC.STALE",
     "METRIC.PERMISSION_DENIED", false, false},
    {"pages.relocation_backlog.show", "SBLR_SHOW_PAGE_RELOCATION_BACKLOG", "sb_api_page_relocation_backlog_get", "page_agent",
     "OBS_METRICS_READ_FAMILY", "", "", "page.relocation_backlog.v1", "", "METRIC.STALE",
     "METRIC.PERMISSION_DENIED", false, false},
    {"filespaces.shrink_readiness.show", "SBLR_SHOW_FILESPACE_SHRINK_READINESS", "sb_api_filespace_shrink_readiness_get", "filespace_agent",
     "OBS_METRICS_READ_FAMILY", "", "", "filespace.shrink_readiness.v1", "", "FILESPACE.SHRINK_BLOCKED",
     "METRIC.PERMISSION_DENIED", false, false},
    {"cluster.agent.list", "SBLR_CLUSTER_AGENT_LIST", "sb_api_cluster_agent_list", "cluster_agent",
     "OBS_CLUSTER_HEALTH_INSPECT", "OBS_AGENT_STATE_READ", "", "cluster.provider.stub.v1", "",
     "CLUSTER.NOT_AVAILABLE", "CLUSTER.PERMISSION_DENIED", true, false},
    {"cluster.agent.get", "SBLR_CLUSTER_AGENT_GET", "sb_api_cluster_agent_get", "cluster_agent",
     "OBS_CLUSTER_HEALTH_INSPECT", "OBS_AGENT_STATE_READ", "", "cluster.provider.stub.v1", "",
     "CLUSTER.AGENT_NOT_FOUND", "CLUSTER.PERMISSION_DENIED", true, false},
    {"cluster.agent.control", "SBLR_CLUSTER_AGENT_CONTROL", "sb_api_cluster_agent_control", "cluster_agent",
     "OBS_CLUSTER_CONTROL", "OBS_AGENT_CONTROL", "cluster_agent_control_evidence", "cluster.provider.stub.v1", "",
     "CLUSTER.LEASE_REQUIRED", "CLUSTER.PERMISSION_DENIED", true, true},
};

constexpr const char* kZeroGreyResultStates[] = {
    "success",
    "refused",
    "denied",
    "empty",
    "redacted",
    "hidden",
    "unsupported",
    "operator_required",
    "pending_evidence",
    "backpressure",
    "request_accepted",
    "read_result",
    "cluster_not_enabled",
};

constexpr const char* kZeroGreyProhibitedFragments[] = {
    "partial",
    "best_effort",
    "best effort",
    "implementation-defined",
    "implementation defined",
    "may_choose",
    "may choose",
    "tbd",
    "unknown",
};

constexpr const char* kZeroGreyEvidencePayloadFields[] = {
    "evidence_uuid",
    "evidence_type",
    "agent_uuid",
    "scope_uuid",
    "action_uuid",
    "policy_uuid",
    "actor_principal_uuid",
    "created_at",
    "input_metric_snapshot_digest",
    "decision_payload",
    "result_state",
    "diagnostic_code",
    "redaction_class",
    "retention_class",
    "payload_digest",
};

const AgentCommandSurfaceSpec* FindAgentCommandSurfaceSpec(std::string_view operation_id) {
  for (const auto& spec : kAgentCommandSurfaceSpecs) {
    if (operation_id == spec.operation_id) {
      return &spec;
    }
  }
  return nullptr;
}

std::string OptionValue(const EngineApiRequest& request, const std::string& prefix) {
  return SecurityOptionValue(request, prefix);
}

platform::u64 OptionU64(const EngineApiRequest& request,
                        const std::string& prefix,
                        platform::u64 fallback) {
  const auto value = OptionValue(request, prefix);
  if (value.empty()) { return fallback; }
  try {
    return static_cast<platform::u64>(std::stoull(value));
  } catch (...) {
    return fallback;
  }
}

std::string AgentTypeFromRequest(const EngineApiRequest& request) {
  auto type = OptionValue(request, "agent_type:");
  if (!type.empty()) { return type; }
  if (!request.target_object.object_kind.empty()) { return request.target_object.object_kind; }
  if (!request.localized_names.empty() && !request.localized_names.front().name.empty()) { return request.localized_names.front().name; }
  return {};
}

bool StrictObservedMetricSnapshotEvidencePresent(
    const EngineApiRequest& request) {
  const auto digest = OptionValue(request, "agent_metric_snapshot_digest:");
  const auto snapshot_id = OptionValue(request, "agent_metric_snapshot_id:");
  const auto evidence_uuid =
      OptionValue(request, "agent_metric_snapshot_evidence_uuid:");
  const auto source_quality =
      SecurityLower(OptionValue(request, "agent_metric_snapshot_source_quality:"));
  return SecurityOptionBool(request, "agent_metric_snapshot_observed:", false) &&
         SecurityOptionBool(request, "agent_metric_snapshot_trusted:", false) &&
         !digest.empty() && !snapshot_id.empty() && !evidence_uuid.empty() &&
         (source_quality == "trusted" ||
          source_quality == "cluster_confirmed");
}

AgentMetricSourceQuality MetricSourceQualityFromText(std::string value) {
  value = SecurityLower(std::move(value));
  if (value == "trusted") { return AgentMetricSourceQuality::trusted; }
  if (value == "cluster_confirmed") {
    return AgentMetricSourceQuality::cluster_confirmed;
  }
  return AgentMetricSourceQuality::unknown;
}

std::vector<AgentObservedMetricSnapshot> ObservedMetricSnapshotsFromRequest(
    const EngineApiRequest& request,
    const AgentTypeDescriptor& descriptor,
    const AgentRuntimeContext& context) {
  std::vector<AgentObservedMetricSnapshot> snapshots;
  if (!StrictObservedMetricSnapshotEvidencePresent(request)) {
    return snapshots;
  }
  const auto digest = OptionValue(request, "agent_metric_snapshot_digest:");
  const auto snapshot_id = OptionValue(request, "agent_metric_snapshot_id:");
  const auto evidence_uuid =
      OptionValue(request, "agent_metric_snapshot_evidence_uuid:");
  const auto trust_provenance =
      OptionValue(request, "agent_metric_snapshot_trust_provenance:").empty()
          ? "engine_metric_registry"
          : OptionValue(request, "agent_metric_snapshot_trust_provenance:");
  const auto source_quality = MetricSourceQualityFromText(
      OptionValue(request, "agent_metric_snapshot_source_quality:"));
  const auto scope_uuid =
      OptionValue(request, "agent_metric_snapshot_scope_uuid:").empty()
          ? context.database_uuid
          : OptionValue(request, "agent_metric_snapshot_scope_uuid:");
  platform::u64 generation = request.context.resource_epoch == 0
                                 ? 1
                                 : request.context.resource_epoch;
  if (const auto value = OptionValue(request, "agent_metric_snapshot_generation:");
      !value.empty()) {
    try { generation = static_cast<platform::u64>(std::stoull(value)); } catch (...) {}
  }
  platform::u64 observed_wall = context.wall_now_microseconds == 0
                                    ? 1
                                    : context.wall_now_microseconds;
  if (const auto value = OptionValue(request, "agent_metric_snapshot_observed_wall_us:");
      !value.empty()) {
    try { observed_wall = static_cast<platform::u64>(std::stoull(value)); } catch (...) {}
  }

  for (const auto& dependency : descriptor.metric_dependencies) {
    AgentObservedMetricSnapshot snapshot;
    snapshot.metric_family = dependency.metric_family;
    snapshot.namespace_path = dependency.namespace_prefix.empty()
                                  ? dependency.metric_family
                                  : dependency.namespace_prefix + ".observed";
    snapshot.generation = generation;
    snapshot.observed_wall_microseconds = observed_wall;
    snapshot.scope_uuid = scope_uuid;
    snapshot.digest = digest + ":" + dependency.metric_family;
    snapshot.source_quality = source_quality;
    snapshot.present = SecurityOptionBool(request,
                                          "agent_metric_snapshot_observed:",
                                          false);
    snapshot.trusted = SecurityOptionBool(request,
                                         "agent_metric_snapshot_trusted:",
                                         false);
    snapshot.schema_compatible =
        !SecurityOptionPresent(request, "agent_metric_snapshot_schema_incompatible:true");
    snapshot.trust_provenance = trust_provenance;
    snapshot.evidence_uuid = evidence_uuid;
    snapshot.snapshot_id = snapshot_id + ":" + dependency.metric_family;
    snapshots.push_back(std::move(snapshot));
  }
  return snapshots;
}

AgentRuntimeContext AgentContextFromRequest(const EngineApiRequest& request) {
  AgentRuntimeContext context;
  context.security_context_present = request.context.security_context_present;
  context.cluster_authority_available = request.context.cluster_authority_available;
  context.cluster_time_majority_available = SecurityOptionBool(request, "cluster_time_majority:", request.context.cluster_authority_available);
  context.private_features_available = SecurityOptionBool(request, "private_features:", true);
  context.standalone_edition = SecurityOptionBool(request, "standalone_edition:", !request.context.cluster_authority_available);
  context.shutdown_requested = SecurityOptionPresent(request, "lifecycle:shutdown");
  context.read_only_mode = SecurityOptionPresent(request, "lifecycle:read_only");
  context.maintenance_mode = SecurityOptionPresent(request, "lifecycle:maintenance");
  context.restricted_open_mode = SecurityOptionPresent(request, "lifecycle:restricted_open");
  context.repair_mode = SecurityOptionPresent(request, "lifecycle:repair");
  context.backup_hold_mode = SecurityOptionPresent(request, "lifecycle:backup") ||
                             SecurityOptionPresent(request, "lifecycle:restore");
  context.archive_hold_mode = SecurityOptionPresent(request, "lifecycle:archive_hold");
  context.principal_uuid = request.context.principal_uuid.canonical;
  context.database_uuid = request.context.database_uuid.canonical;
  context.cluster_uuid = request.context.cluster_uuid.canonical;
  for (const auto& tag : request.context.trace_tags) {
    if (tag.rfind("right:", 0) == 0) { context.rights.push_back(tag.substr(6)); }
    if (tag.rfind("group:", 0) == 0) { context.groups.push_back(tag.substr(6)); }
    context.trace_tags.push_back(tag);
  }
  if (SecurityContextHasTag(request.context, "security.bootstrap")) { context.trace_tags.push_back("security.bootstrap"); }
  context.wall_now_microseconds = 1;
  const auto wall = OptionValue(request, "wall_now_us:");
  if (!wall.empty()) { try { context.wall_now_microseconds = static_cast<std::uint64_t>(std::stoull(wall)); } catch (...) {} }
  const auto mono = OptionValue(request, "monotonic_now_us:");
  if (!mono.empty()) { try { context.monotonic_now_microseconds = static_cast<std::uint64_t>(std::stoull(mono)); } catch (...) {} }
  const auto cluster = OptionValue(request, "cluster_now_us:");
  if (!cluster.empty()) { try { context.cluster_now_microseconds = static_cast<std::uint64_t>(std::stoull(cluster)); } catch (...) {} }
  return context;
}

DurableAgentResourceReservationRequest DurableResourceReservationForManagement(
    const EngineApiRequest& request,
    const AgentTypeDescriptor& descriptor,
    const AgentRuntimeContext& context,
    std::string_view operation_id) {
  DurableAgentResourceReservationRequest reservation;
  const auto explicit_key =
      OptionValue(request, "agent_resource_reservation_key:");
  reservation.reservation_key =
      explicit_key.empty()
          ? "agent_management:" + descriptor.type_id + ":" +
                std::string(operation_id) + ":" + request.context.request_id
          : explicit_key;
  reservation.reservation_uuid =
      DeterministicAgentRuntimeObjectUuidFromKey(
          "agent_management_resource_reservation|" +
          reservation.reservation_key);
  reservation.owner_scope = context.principal_uuid.empty()
                                ? request.context.principal_uuid.canonical
                                : context.principal_uuid;
  reservation.agent_type_id = descriptor.type_id;
  reservation.operation_id = std::string(operation_id);
  reservation.now_microseconds = context.wall_now_microseconds == 0
                                     ? 1
                                     : context.wall_now_microseconds;
  reservation.memory_bytes = OptionU64(
      request, "agent_resource_reservation_memory_bytes:", 4096);
  reservation.worker_slots = OptionU64(
      request, "agent_resource_reservation_worker_slots:", 1);
  reservation.overhead_microseconds = OptionU64(
      request, "agent_resource_reservation_overhead_us:", 1000);
  reservation.max_active_reservations = OptionU64(
      request, "agent_resource_reservation_max_active:", 1024);
  reservation.max_memory_bytes = OptionU64(
      request, "agent_resource_reservation_max_memory_bytes:",
      64 * 1024 * 1024);
  reservation.max_worker_slots = OptionU64(
      request, "agent_resource_reservation_max_worker_slots:", 8);
  reservation.max_overhead_microseconds = OptionU64(
      request, "agent_resource_reservation_max_overhead_us:",
      10 * 1000 * 1000);
  reservation.evidence_uuid =
      DeterministicAgentRuntimeObjectUuidFromKey(
          "agent_management_resource_reservation_evidence|" +
          reservation.reservation_uuid);
  return reservation;
}

template <typename TResult>
TResult AgentSecurityFailure(const EngineApiRequest& request, const std::string& operation_id, const std::string& detail) {
  return MakeApiBehaviorDiagnostic<TResult>(request.context,
                                            operation_id,
                                            MakeSecurityDiagnostic("SB_AGENT_SECURITY.RIGHT_REQUIRED", detail));
}

template <typename TResult>
TResult AgentFailure(const EngineApiRequest& request,
                     const std::string& operation_id,
                     const std::string& code,
                     const std::string& detail) {
  return MakeApiBehaviorDiagnostic<TResult>(request.context,
                                            operation_id,
                                            MakeEngineApiDiagnostic(code, "agent.runtime.diagnostic", detail, true));
}

bool HasRequiredAgentSurfaceRights(const EngineApiRequest& request,
                                   const AgentCommandSurfaceSpec& spec) {
  if (!SecurityContextHasRight(request.context, spec.required_right_primary)) {
    return false;
  }
  return spec.required_right_secondary == nullptr ||
         std::string_view(spec.required_right_secondary).empty() ||
         SecurityContextHasRight(request.context, spec.required_right_secondary);
}

std::string LowerAscii(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return value;
}

bool ContainsCaseFolded(std::string_view value, std::string_view needle) {
  return LowerAscii(std::string(value)).find(LowerAscii(std::string(needle))) != std::string::npos;
}

// SEARCH_KEY: ARHC_MANAGEMENT_API_DURABLE_MUTATION_STATE
bool DurableRuntimeRequired(const EngineApiRequest& request,
                            const EngineAgentDurableRuntimeState& durable_state) {
  return durable_state.require_durable_runtime_state ||
         durable_state.production_live_path ||
         SecurityOptionPresent(request, "agent_management_production_live:true") ||
         SecurityOptionPresent(request, "agent_management_durable_runtime_required:true");
}

bool DurableCatalogStoreContextAvailable(const EngineApiRequest& request) {
  return !request.context.database_path.empty() &&
         !request.context.transaction_uuid.canonical.empty() &&
         request.context.local_transaction_id != 0;
}

bool DurableCatalogStoreCheckpointEvidencePresent(
    const EngineApiRequest& request) {
  return SecurityOptionBool(
             request,
             "agent_durable_catalog_fsync_or_checkpoint_evidence:",
             false) ||
         SecurityOptionPresent(
             request,
             "agent_durable_catalog_checkpoint_evidence:true") ||
         SecurityOptionPresent(request,
                               "agent_durable_catalog_fsync_evidence:true");
}

const DurableAgentCatalogImage* DurableCatalogForRead(
    const EngineAgentDurableRuntimeState& durable_state) {
  return durable_state.catalog != nullptr ? durable_state.catalog
                                          : durable_state.mutable_catalog;
}

bool AgentManagementProductionLive(const EngineApiRequest& request) {
  return SecurityOptionPresent(request, "agent_management_production_live:true") ||
         SecurityOptionPresent(request,
                               "agent_management_durable_runtime_required:true");
}

template <typename TResult>
std::optional<TResult> RequireProductionDurableCatalogStoreContext(
    const EngineApiRequest& request,
    const std::string& operation_id,
    const EngineAgentDurableRuntimeState& durable_state) {
  if (!DurableRuntimeRequired(request, durable_state)) {
    return std::nullopt;
  }
  if (DurableCatalogStoreContextAvailable(request)) {
    return std::nullopt;
  }
  return AgentFailure<TResult>(
      request,
      operation_id,
      "SB_AGENT_MANAGEMENT.DURABLE_CATALOG_STORE_REQUIRED",
      "production agent management requires MGA-backed durable catalog store context");
}

std::vector<EngineAgentCatalogIdentitySource> IdentitySourcesFromDurableCatalog(
    const DurableAgentCatalogImage& image) {
  std::vector<EngineAgentCatalogIdentitySource> sources;
  sources.reserve(image.instances.size());
  for (const auto& instance : image.instances) {
    EngineAgentCatalogIdentitySource source;
    source.agent_type_id = instance.agent_type_id;
    source.agent_uuid = instance.instance_uuid;
    source.policy_uuid = instance.policy_uuid;
    source.scope_kind = instance.scope;
    source.scope_uuid = instance.scope;
    if (const auto descriptor = FindAgentType(instance.agent_type_id);
        descriptor.has_value()) {
      source.policy_name = BaselinePolicyForAgent(*descriptor).policy_family;
    }
    sources.push_back(std::move(source));
  }
  return sources;
}

template <typename TResult>
std::optional<TResult> StoreFailure(const EngineApiRequest& request,
                                    const std::string& operation_id,
                                    const AgentDurableCatalogStoreResult& store,
                                    const std::string& fallback_code,
                                    const std::string& fallback_detail) {
  const std::string code =
      store.diagnostic.code.empty() ? fallback_code : store.diagnostic.code;
  std::string detail = store.diagnostic.detail;
  if (detail.empty()) { detail = store.diagnostic.message_key; }
  if (detail.empty()) { detail = fallback_detail; }
  return AgentFailure<TResult>(request, operation_id, code, detail);
}

template <typename TResult>
std::optional<TResult> ResolveDurableCatalogForRead(
    const EngineApiRequest& request,
    const EngineAgentDurableRuntimeState& durable_state,
    const std::string& operation_id,
    const DurableAgentCatalogImage** image,
    std::optional<AgentDurableCatalogStoreResult>* loaded_store) {
  if (image == nullptr || loaded_store == nullptr) {
    return AgentFailure<TResult>(
        request,
        operation_id,
        "SB_AGENT_MANAGEMENT.DURABLE_CATALOG_REQUIRED",
        "durable catalog output parameter missing");
  }
  const bool required = DurableRuntimeRequired(request, durable_state);
  if (auto failure =
          RequireProductionDurableCatalogStoreContext<TResult>(
              request, operation_id, durable_state);
      failure.has_value()) {
    return *failure;
  }
  if (required && DurableCatalogStoreContextAvailable(request)) {
    auto loaded = LoadAgentDurableCatalogImage(request.context, true);
    if (!loaded.ok) {
      return StoreFailure<TResult>(
          request,
          operation_id,
          loaded,
          "SB_AGENT_MANAGEMENT.DURABLE_CATALOG_REQUIRED",
          "production agent management requires stored durable runtime state");
    }
    *loaded_store = std::move(loaded);
    *image = &loaded_store->value().image;
  } else {
    *image = DurableCatalogForRead(durable_state);
  }
  if (*image == nullptr) {
    if (!required) { return std::nullopt; }
    return AgentFailure<TResult>(
        request,
        operation_id,
        "SB_AGENT_MANAGEMENT.DURABLE_CATALOG_REQUIRED",
        "production agent management requires durable runtime state");
  }
  const auto status = ValidateDurableAgentCatalogForProduction(**image);
  if (!status.ok) {
    if (!required) {
      *image = nullptr;
      return std::nullopt;
    }
    return AgentFailure<TResult>(request, operation_id, status.diagnostic_code,
                                 status.detail);
  }
  return std::nullopt;
}

template <typename TResult>
std::optional<TResult> ValidateDurableRuntimeForRead(
    const EngineApiRequest& request,
    const EngineAgentDurableRuntimeState& durable_state,
    const std::string& operation_id) {
  const bool required = DurableRuntimeRequired(request, durable_state);
  const auto* image = DurableCatalogForRead(durable_state);
  if (image == nullptr) {
    if (!required) { return std::nullopt; }
    return AgentFailure<TResult>(
        request,
        operation_id,
        "SB_AGENT_MANAGEMENT.DURABLE_CATALOG_REQUIRED",
        "production agent management requires durable runtime state");
  }
  const auto status = ValidateDurableAgentCatalogForProduction(*image);
  if (!status.ok) {
    if (!required) { return std::nullopt; }
    return AgentFailure<TResult>(request, operation_id, status.diagnostic_code,
                                 status.detail);
  }
  return std::nullopt;
}

template <typename TResult>
std::optional<TResult> ValidateDurableRuntimeForMutation(
    const EngineApiRequest& request,
    const EngineAgentDurableRuntimeState& durable_state,
    const std::string& operation_id) {
  if (durable_state.mutable_catalog == nullptr) {
    return AgentFailure<TResult>(
        request,
        operation_id,
        "SB_AGENT_MANAGEMENT.DURABLE_CATALOG_REQUIRED",
        "agent management mutation requires mutable durable runtime catalog");
  }
  const auto status =
      ValidateDurableAgentCatalogForProduction(*durable_state.mutable_catalog);
  if (!status.ok) {
    return AgentFailure<TResult>(request, operation_id, status.diagnostic_code,
                                 status.detail);
  }
  return std::nullopt;
}

template <typename TResult>
std::optional<TResult> ValidateProductionFixtureSeparationForMutation(
    const EngineApiRequest& request,
    const EngineAgentDurableRuntimeState& durable_state,
    const std::string& operation_id,
    bool dry_run) {
  const bool production_live_path = DurableRuntimeRequired(request, durable_state);
  AgentProductionFixtureSeparationInput separation;
#if defined(SCRATCHBIRD_AGENT_PRODUCTION_BUILD)
  separation.production_build = true;
#endif
#if defined(SCRATCHBIRD_ENABLE_DEBUG_LOGS) || \
    defined(SCRATCHBIRD_ENABLE_HOTPATH_TRACE) || \
    defined(SCRATCHBIRD_ENABLE_EXEC_PROFILE_TRACE) || \
    defined(SCRATCHBIRD_ENABLE_PREPARED_TRACE)
  separation.debug_only_paths_enabled = true;
#endif
  separation.production_live_path = production_live_path;
  separation.fixture_auth =
      SecurityOptionPresent(request, "allow_fixture:true") ||
      SecurityOptionPresent(request, "fixture_auth:true");
  separation.test_fixture_mode =
      SecurityOptionPresent(request, "agent_fixture_mode:true");
  separation.fixture_policy =
      SecurityOptionPresent(request, "agent_fixture_policy:true");
  separation.relaxed_metric_path =
      production_live_path &&
      !StrictObservedMetricSnapshotEvidencePresent(request);
  separation.observed_metric_snapshot_required =
      !dry_run || production_live_path;
  separation.test_seed_material =
      SecurityOptionPresent(request, "agent_test_seed:true");
  separation.forced_collision_hooks =
      SecurityOptionPresent(request, "agent_forced_collision_hook:true");
  separation.probe_only_catalog = durable_state.mutable_catalog == nullptr;
  separation.durable_runtime_catalog =
      durable_state.mutable_catalog != nullptr;
  separation.sidecar_only_evidence = false;
  separation.durable_evidence_store = true;
  separation.synthetic_live_management_state =
      durable_state.mutable_catalog == nullptr;
  separation.management_state_durable =
      durable_state.mutable_catalog != nullptr;
  separation.live_agent_surface = production_live_path && !dry_run;

  const auto result = ValidateAgentProductionFixtureSeparation(separation);
  if (result.ok) { return std::nullopt; }
  return AgentFailure<TResult>(request,
                               operation_id,
                               result.status.diagnostic_code,
                               result.status.detail);
}

EngineApiDiagnostic ThirdPartyDiagnostic(std::string code, std::string detail, bool error = true) {
  return MakeEngineApiDiagnostic(std::move(code),
                                 "agent.third_party_management_request.diagnostic",
                                 std::move(detail),
                                 error);
}

bool ParseDurableUuid(platform::UuidKind kind, std::string_view value) {
  return uuid::ParseDurableEngineIdentityUuid(kind, std::string(value)).ok();
}

std::optional<std::string> ValidateThirdPartyUuidField(
    const EngineThirdPartyAgentManagementRequestRecord& record) {
  if (record.request_uuid.empty() ||
      !ParseDurableUuid(platform::UuidKind::object, record.request_uuid)) {
    return "request_uuid";
  }
  if (record.requester_principal_uuid.empty() ||
      !ParseDurableUuid(platform::UuidKind::principal, record.requester_principal_uuid)) {
    return "requester_principal_uuid";
  }
  if (record.policy_ref.empty() ||
      !ParseDurableUuid(platform::UuidKind::object, record.policy_ref)) {
    return "policy_ref";
  }
  return std::nullopt;
}

const EngineAgentCatalogIdentitySource* ResolveThirdPartyAgent(
    const std::vector<EngineAgentCatalogIdentitySource>& sources,
    std::string_view agent_ref) {
  for (const auto& source : sources) {
    if (!source.agent_uuid.empty() && source.agent_uuid == agent_ref) { return &source; }
    if (!source.agent_type_id.empty() && source.agent_type_id == agent_ref) { return &source; }
  }
  return nullptr;
}

bool ThirdPartyResolvedAgentHasUuidAuthority(
    const EngineAgentCatalogIdentitySource* agent) {
  return agent != nullptr &&
         !agent->agent_uuid.empty() &&
         ParseDurableUuid(platform::UuidKind::object, agent->agent_uuid);
}

bool ThirdPartyResolvedPolicyMatchesRequest(
    const EngineAgentCatalogIdentitySource& agent,
    std::string_view policy_ref) {
  return !agent.policy_uuid.empty() &&
         agent.policy_uuid == policy_ref &&
         ParseDurableUuid(platform::UuidKind::object, agent.policy_uuid);
}

bool ThirdPartyDirectActuatorBypass(const EngineThirdPartyAgentManagementRequestRecord& record) {
  const auto operation = LowerAscii(record.operation);
  const auto action = LowerAscii(record.requested_action);
  return operation.rfind("actuator.", 0) == 0 ||
         action.rfind("actuator.", 0) == 0 ||
         ContainsCaseFolded(action, "preallocate_page_family") ||
         ContainsCaseFolded(action, "execute_filespace") ||
         ContainsCaseFolded(action, "storage_executor") ||
         ContainsCaseFolded(action, "direct_actuator");
}

std::string LastSegment(std::string_view value) {
  const auto pos = value.rfind('.');
  return std::string(pos == std::string_view::npos ? value : value.substr(pos + 1));
}

bool ThirdPartyRequestedActionAllowed(const AgentCommandSurfaceSpec& spec,
                                      std::string_view requested_action) {
  if (requested_action == spec.operation_id) { return true; }
  if (requested_action == spec.command_family) { return true; }
  return requested_action == LastSegment(spec.operation_id);
}

std::string RedactThirdPartyPayload(std::string payload, bool* redacted) {
  *redacted = false;
  if (payload.empty()) { return payload; }
  const bool unsafe = ContainsCaseFolded(payload, "password") ||
                      ContainsCaseFolded(payload, "secret") ||
                      ContainsCaseFolded(payload, "token") ||
                      ContainsCaseFolded(payload, "credential") ||
                      payload.find('/') != std::string::npos ||
                      payload.find("://") != std::string::npos;
  if (!unsafe) { return payload; }
  *redacted = true;
  return "<redacted>";
}

void AddThirdPartyRequestRow(EngineApiResult* result,
                             const EngineThirdPartyAgentManagementRequestRecord& record,
                             const AgentCommandSurfaceSpec& spec,
                             const EngineAgentCatalogIdentitySource* agent,
                             const char* result_state,
                             const char* diagnostic_code,
                             bool queued,
                             bool payload_redacted,
                             std::string protected_payload) {
  AddApiBehaviorRow(result,
                    {{"request_uuid", record.request_uuid},
                     {"requester_principal_uuid", record.requester_principal_uuid},
                     {"external_system_id", record.external_system_id},
                     {"agent_ref", record.agent_ref},
                     {"agent_type_id", agent == nullptr ? std::string{} : agent->agent_type_id},
                     {"agent_uuid", agent == nullptr ? std::string{} : agent->agent_uuid},
                     {"operation", record.operation},
                     {"requested_action", record.requested_action},
                     {"sblr_operation", spec.opcode},
                     {"api_call", spec.api_call},
                     {"policy_uuid", record.policy_ref},
                     {"reason_code", record.reason_code},
                     {"requested_expiry", record.requested_expiry},
                     {"redaction_context", record.redaction_context},
                     {"idempotency_key_present", record.idempotency_key.empty() ? "false" : "true"},
                     {"result_state", result_state},
                     {"diagnostic_code", diagnostic_code},
                     {"request_evidence_uuid", record.request_uuid},
                     {"evidence_kind", "agent_third_party_request_evidence"},
                     {"queued", queued ? "true" : "false"},
                     {"third_party_authority", "false"},
                     {"parser_finality_authority", "false"},
                     {"actuator_direct", "false"},
                     {"payload_redacted", payload_redacted ? "true" : "false"},
                     {"protected_payload", std::move(protected_payload)},
                     {"retry_after", record.retry_after}});
}

EngineThirdPartyAgentManagementResult ThirdPartyFailure(
    const EngineThirdPartyAgentManagementRequest& request,
    std::string code,
    std::string detail) {
  return MakeApiBehaviorDiagnostic<EngineThirdPartyAgentManagementResult>(
      request.context,
      "agents.third_party.request",
      ThirdPartyDiagnostic(std::move(code), std::move(detail)));
}

void AddThirdPartyRequestEvidenceIfTyped(EngineApiResult* result,
                                         const EngineThirdPartyAgentManagementRequestRecord& record) {
  if (ParseDurableUuid(platform::UuidKind::object, record.request_uuid)) {
    AddApiBehaviorEvidence(result, "agent_third_party_request_evidence", record.request_uuid);
  }
}

AgentSecurityCommandFamily CommandFamilyForSpec(const AgentCommandSurfaceSpec& spec) {
  const std::string_view operation = spec.operation_id;
  const std::string_view family = spec.command_family;
  if (family == "agent_policy_read") { return AgentSecurityCommandFamily::policy_read; }
  if (family == "agent_evidence" || family == "agent_audit") {
    return AgentSecurityCommandFamily::evidence_read;
  }
  if (family == "agent_action_read") { return AgentSecurityCommandFamily::recommendation_read; }
  if (family == "agent_action") {
    if (operation == "agents.action.cancel") {
      return AgentSecurityCommandFamily::action_cancel;
    }
    if (operation == "agents.action.suppress") {
      return AgentSecurityCommandFamily::override;
    }
    return AgentSecurityCommandFamily::action_approve;
  }
  if (family == "agent_override") { return AgentSecurityCommandFamily::override; }
  if (family == "agent_policy") {
    if (operation == "agents.policy.validate") {
      return AgentSecurityCommandFamily::policy_validate;
    }
    if (operation == "agents.policy.simulate") {
      return AgentSecurityCommandFamily::policy_simulate;
    }
    if (operation == "agents.policy.rollback") {
      return AgentSecurityCommandFamily::policy_rollback;
    }
    return AgentSecurityCommandFamily::policy_apply;
  }
  if (family == "cluster_agent") {
    return spec.mutating ? AgentSecurityCommandFamily::cluster_control
                         : AgentSecurityCommandFamily::cluster_inspect;
  }
  if (spec.mutating) { return AgentSecurityCommandFamily::control; }
  return AgentSecurityCommandFamily::state_read;
}

// SEARCH_KEY: PFAR_014_AGENT_MANAGEMENT_AUTHORIZATION_POLICY_GATE
bool UsesProjectionMatrixRights(const AgentCommandSurfaceSpec& spec) {
  const std::string_view family = spec.command_family;
  return family == "filespace_agent" || family == "page_agent";
}

std::string ManagementPolicyValue(const EngineApiRequest& request) {
  auto value = SecurityOptionValue(request, "agent_management_policy:");
  if (value.empty()) { value = SecurityOptionValue(request, "agent_policy_profile:"); }
  if (value.empty()) { value = SecurityOptionValue(request, "policy_profile:"); }
  return SecurityLower(value);
}

bool RequestReadOnlyMode(const EngineApiRequest& request) {
  return request.context.read_only_mode ||
         SecurityOptionPresent(request, "lifecycle:read_only");
}

// SEARCH_KEY: AgentOpenStateMutationDenialCode
const char* AgentOpenStateMutationDenialCode(const EngineApiRequest& request) {
  if (RequestReadOnlyMode(request)) {
    return "AGENT.MANAGEMENT.READ_ONLY_DENIED";
  }
  if (SecurityOptionPresent(request, "lifecycle:shutdown")) {
    return "AGENT.MANAGEMENT.SHUTDOWN_IN_PROGRESS";
  }
  if (SecurityOptionPresent(request, "lifecycle:restricted_open")) {
    return "AGENT.MANAGEMENT.RESTRICTED_DENIED";
  }
  if (SecurityOptionPresent(request, "lifecycle:repair")) {
    return "AGENT.MANAGEMENT.REPAIR_DENIED";
  }
  if (SecurityOptionPresent(request, "lifecycle:maintenance")) {
    return "AGENT.MANAGEMENT.MAINTENANCE_DENIED";
  }
  if (SecurityOptionPresent(request, "lifecycle:backup") ||
      SecurityOptionPresent(request, "lifecycle:restore")) {
    return "AGENT.MANAGEMENT.BACKUP_HOLD_DENIED";
  }
  if (SecurityOptionPresent(request, "lifecycle:archive_hold")) {
    return "AGENT.MANAGEMENT.ARCHIVE_HOLD_DENIED";
  }
  return nullptr;
}

bool MetricFreshForCommandSurface(const EngineApiRequest& request,
                                  const AgentCommandSurfaceSpec& spec) {
  const std::string_view family = spec.command_family;
  if (family != "agent_read" && family != "filespace_agent" &&
      family != "page_agent") {
    return true;
  }
  return !SecurityOptionPresent(request, "metrics_fresh:false") &&
         !SecurityOptionPresent(request, "metric_freshness:stale") &&
         !SecurityOptionPresent(request, "metric_snapshot:stale");
}

bool PolicyFreshForCommandSurface(const EngineApiRequest& request,
                                  const AgentCommandSurfaceSpec& spec) {
  if (std::string_view(spec.command_family).find("agent_policy") != 0 &&
      !spec.mutating) {
    return true;
  }
  return !SecurityOptionPresent(request, "policy_fresh:false") &&
         !SecurityOptionPresent(request, "policy_snapshot:stale") &&
         !SecurityOptionPresent(request, "policy_generation:stale");
}

std::string AgentScopeKind(const AgentTypeDescriptor& descriptor) {
  if (descriptor.cluster_only) { return "cluster"; }
  if (descriptor.scope == "database" || descriptor.scope == "database_local") {
    return "database";
  }
  if (descriptor.scope == "local_node" || descriptor.scope == "node") {
    return "node";
  }
  return descriptor.scope.empty() ? "database" : descriptor.scope;
}

std::string AgentComponent(const AgentTypeDescriptor& descriptor) {
  const std::string_view type = descriptor.type_id;
  if (type.find("filespace") != std::string_view::npos) { return "storage.filespace"; }
  if (type.find("page") != std::string_view::npos) { return "storage.pages"; }
  if (type.find("storage") != std::string_view::npos) { return "storage.health"; }
  if (type.find("cluster") != std::string_view::npos) { return "cluster"; }
  if (type.find("policy") != std::string_view::npos) { return "policy"; }
  if (type.find("session") != std::string_view::npos ||
      type.find("identity") != std::string_view::npos) {
    return "security";
  }
  return "engine.runtime";
}

const EngineAgentCatalogIdentitySource* FindAgentCatalogIdentitySource(
    const std::vector<EngineAgentCatalogIdentitySource>& sources,
    std::string_view agent_type_id) {
  for (const auto& source : sources) {
    if (source.agent_type_id == agent_type_id) { return &source; }
  }
  return nullptr;
}

std::string CanonicalAgentState(const AgentTypeDescriptor& descriptor,
                                const AgentRuntimeContext& context) {
  const auto feature = EvaluateAgentFeatureAvailability(descriptor, context);
  if (feature != scratchbird::core::agents::AgentFeatureAvailability::available) {
    return "unavailable";
  }
  const auto policy = BaselinePolicyForAgent(descriptor);
  if (policy.activation == scratchbird::core::agents::AgentActivationProfile::disabled) {
    return "disabled";
  }
  if (policy.activation == scratchbird::core::agents::AgentActivationProfile::dry_run) {
    return "dry_run";
  }
  if (policy.activation == scratchbird::core::agents::AgentActivationProfile::observe_only) {
    return "observe_only";
  }
  if (policy.activation ==
      scratchbird::core::agents::AgentActivationProfile::recommend_only) {
    return "recommend_only";
  }
  return "available";
}

SysInformationProjectionContext AgentProjectionContext(const EngineApiRequest& request) {
  SysInformationProjectionContext context;
  context.catalog_display_name = "ScratchBird";
  context.session_language = request.context.language_context.language_tag.empty()
                                 ? "en"
                                 : request.context.language_context.language_tag;
  context.default_language = request.context.language_context.default_language_tag.empty()
                                 ? "en"
                                 : request.context.language_context.default_language_tag;
  context.visible_catalog_generation_id = request.context.catalog_generation_id;
  context.cluster_authority_available = request.context.cluster_authority_available;
  return context;
}

SysInformationAgentSource AgentProjectionSource(const AgentTypeDescriptor& descriptor,
                                                const AgentRuntimeContext& context,
                                                const EngineAgentCatalogIdentitySource* identity) {
  SysInformationAgentSource source;
  source.agent_uuid = identity == nullptr ? std::string{} : identity->agent_uuid;
  source.agent_ref = source.agent_uuid;
  source.agent_name = descriptor.type_id;
  source.agent_type_id = descriptor.type_id;
  source.scope_kind = identity != nullptr && !identity->scope_kind.empty()
                          ? identity->scope_kind
                          : AgentScopeKind(descriptor);
  source.scope_uuid = identity == nullptr ? std::string{} : identity->scope_uuid;
  source.scope_ref = source.scope_uuid;
  source.component = identity != nullptr && !identity->component.empty()
                         ? identity->component
                         : AgentComponent(descriptor);
  source.state = CanonicalAgentState(descriptor, context);
  source.health_state = source.state == "unavailable" ? "unavailable" : "healthy";
  source.enabled = source.state == "disabled" ? "NO" : "YES";
  source.policy_uuid = identity == nullptr ? std::string{} : identity->policy_uuid;
  source.policy_ref = source.policy_uuid;
  source.policy_name = identity != nullptr && !identity->policy_name.empty()
                           ? identity->policy_name
                           : BaselinePolicyForAgent(descriptor).policy_family;
  source.scope_visible = identity == nullptr || identity->scope_visible;
  source.last_transition_at = "engine_runtime";
  source.last_diagnostic_code = source.state == "unavailable" ? "SB_AGENT_FEATURE.UNAVAILABLE" : "";
  return source;
}

const AgentInstanceRecord* FindDurableInstance(const DurableAgentCatalogImage& image,
                                               std::string_view agent_type_id) {
  for (const auto& instance : image.instances) {
    if (instance.agent_type_id == agent_type_id) { return &instance; }
  }
  return nullptr;
}

AgentInstanceRecord* FindMutableDurableInstance(DurableAgentCatalogImage* image,
                                                std::string_view agent_type_id) {
  if (image == nullptr) { return nullptr; }
  for (auto& instance : image->instances) {
    if (instance.agent_type_id == agent_type_id) { return &instance; }
  }
  return nullptr;
}

const DurableAgentHealthRecord* FindDurableHealth(
    const DurableAgentCatalogImage& image,
    std::string_view instance_uuid) {
  for (const auto& health : image.health) {
    if (health.instance_uuid == instance_uuid) { return &health; }
  }
  return nullptr;
}

DurableAgentHealthRecord* FindMutableDurableHealth(
    DurableAgentCatalogImage* image,
    std::string_view instance_uuid) {
  if (image == nullptr) { return nullptr; }
  for (auto& health : image->health) {
    if (health.instance_uuid == instance_uuid) { return &health; }
  }
  return nullptr;
}

const AgentPolicy* FindDurablePolicy(const DurableAgentCatalogImage& image,
                                     std::string_view policy_uuid) {
  for (const auto& policy : image.policies) {
    if (policy.policy_uuid == policy_uuid) { return &policy; }
  }
  return nullptr;
}

bool DurableActionBacklogState(DurableAgentActionState state) {
  return state == DurableAgentActionState::pending ||
         state == DurableAgentActionState::running ||
         state == DurableAgentActionState::replay_pending;
}

std::string DurableLastEvidenceUuid(const DurableAgentCatalogImage& image,
                                    const AgentInstanceRecord& instance) {
  if (!image.health.empty()) {
    for (auto it = image.health.rbegin(); it != image.health.rend(); ++it) {
      if (it->instance_uuid == instance.instance_uuid &&
          !it->evidence_uuid.empty()) {
        return it->evidence_uuid;
      }
    }
  }
  for (auto it = image.actions.rbegin(); it != image.actions.rend(); ++it) {
    if (it->instance_uuid == instance.instance_uuid &&
        !it->evidence_uuid.empty()) {
      return it->evidence_uuid;
    }
  }
  for (auto it = image.evidence.rbegin(); it != image.evidence.rend(); ++it) {
    if (it->instance_uuid == instance.instance_uuid &&
        !it->evidence_uuid.empty()) {
      return it->evidence_uuid;
    }
  }
  return instance.retirement_evidence_uuid;
}

std::string DurableLastDecision(const DurableAgentCatalogImage& image,
                                const AgentInstanceRecord& instance) {
  for (auto it = image.actions.rbegin(); it != image.actions.rend(); ++it) {
    if (it->instance_uuid == instance.instance_uuid) {
      return it->operation_id.empty()
                 ? DurableAgentActionStateName(it->state)
                 : it->operation_id + ":" + DurableAgentActionStateName(it->state);
    }
  }
  for (auto it = image.retained_history.rbegin(); it != image.retained_history.rend(); ++it) {
    if (it->subject_uuid == instance.instance_uuid) {
      return it->event_kind.empty() ? it->diagnostic_code : it->event_kind;
    }
  }
  return AgentLifecycleStateName(instance.state);
}

platform::u64 DurableActionBacklog(const DurableAgentCatalogImage& image,
                                   const AgentInstanceRecord& instance) {
  platform::u64 count = 0;
  for (const auto& action : image.actions) {
    if (action.instance_uuid == instance.instance_uuid &&
        DurableActionBacklogState(action.state)) {
      ++count;
    }
  }
  return count;
}

platform::u64 DurableQueueDepth(const DurableAgentCatalogImage& image,
                                const AgentInstanceRecord& instance) {
  platform::u64 count = DurableActionBacklog(image, instance);
  for (const auto& lease : image.leases) {
    if (lease.instance_uuid == instance.instance_uuid &&
        (lease.state == DurableAgentLeaseState::acquired ||
         lease.state == DurableAgentLeaseState::draining ||
         lease.state == DurableAgentLeaseState::replay_pending)) {
      ++count;
    }
  }
  return count;
}

platform::u64 DurableQuarantineCount(const DurableAgentCatalogImage& image,
                                     const AgentInstanceRecord& instance) {
  platform::u64 count = instance.quarantined ? 1 : 0;
  for (const auto& action : image.actions) {
    if (action.instance_uuid == instance.instance_uuid &&
        action.state == DurableAgentActionState::quarantined) {
      ++count;
    }
  }
  for (const auto& lease : image.leases) {
    if (lease.instance_uuid == instance.instance_uuid &&
        lease.state == DurableAgentLeaseState::quarantined) {
      ++count;
    }
  }
  return count;
}

std::string RetryNotBefore(const AgentInstanceRecord& instance) {
  const auto retry = std::max(instance.restart_not_before_microseconds,
                              instance.cooldown_until_microseconds);
  return retry == 0 ? std::string{} : std::to_string(retry);
}

SysInformationAgentSource DurableAgentProjectionSource(
    const AgentTypeDescriptor& descriptor,
    const AgentRuntimeContext& context,
    const EngineAgentCatalogIdentitySource* identity,
    const DurableAgentCatalogImage& image,
    const AgentInstanceRecord& instance) {
  auto source = AgentProjectionSource(descriptor, context, identity);
  source.agent_uuid = instance.instance_uuid;
  source.agent_ref = instance.instance_uuid;
  source.policy_uuid = instance.policy_uuid;
  source.policy_ref = instance.policy_uuid;
  source.scope_uuid = !instance.scope.empty() ? instance.scope : source.scope_uuid;
  source.scope_ref = source.scope_uuid;
  source.state = AgentLifecycleStateName(instance.state);
  source.enabled = (instance.state == AgentLifecycleState::disabled ||
                    instance.state == AgentLifecycleState::retired)
                       ? "NO"
                       : "YES";
  source.last_transition_at = "durable_runtime_catalog";
  source.last_diagnostic_code = instance.last_failure_diagnostic_code;
  source.policy_generation = instance.policy_generation;
  source.queue_depth = DurableQueueDepth(image, instance);
  source.action_backlog = DurableActionBacklog(image, instance);
  source.failure_count =
      instance.crash_loop_count + instance.supervision_failure_count;
  source.quarantine_count = DurableQuarantineCount(image, instance);
  source.retry_not_before = RetryNotBefore(instance);
  source.last_evidence_uuid = DurableLastEvidenceUuid(image, instance);
  source.last_decision = DurableLastDecision(image, instance);
  source.overhead_budget_units = source.queue_depth + source.failure_count +
                                 source.quarantine_count;
  if (const auto* policy = FindDurablePolicy(image, instance.policy_uuid);
      policy != nullptr) {
    source.policy_name = !policy->policy_name.empty() ? policy->policy_name
                                                      : policy->policy_family;
  }
  if (const auto* health = FindDurableHealth(image, instance.instance_uuid);
      health != nullptr) {
    source.health_state = health->health_state.empty() ? "unknown"
                                                       : health->health_state;
    if (!health->diagnostic_code.empty()) {
      source.last_diagnostic_code = health->diagnostic_code;
    }
  } else {
    source.health_state = "unknown";
  }
  if (!AgentContextHasRight(context, "OBS_AGENT_EVIDENCE_READ") &&
      !source.last_diagnostic_code.empty()) {
    source.last_diagnostic_code = "redacted";
    source.diagnostic_redaction_state = "redacted";
  } else {
    source.diagnostic_redaction_state =
        source.last_diagnostic_code.empty() ? "not_applicable" : "visible";
  }
  return source;
}

std::vector<SysInformationAgentSource> AgentProjectionSources(
    const AgentRuntimeContext& context,
    const std::string& filtered_type,
    bool include_cluster,
    const std::vector<EngineAgentCatalogIdentitySource>& identity_sources) {
  std::vector<SysInformationAgentSource> sources;
  for (const auto& descriptor : CanonicalAgentRegistry()) {
    if (!filtered_type.empty() && descriptor.type_id != filtered_type) { continue; }
    if (descriptor.cluster_only && !include_cluster) { continue; }
    sources.push_back(AgentProjectionSource(
        descriptor,
        context,
        FindAgentCatalogIdentitySource(identity_sources, descriptor.type_id)));
  }
  return sources;
}

std::vector<SysInformationAgentSource> DurableAgentProjectionSources(
    const AgentRuntimeContext& context,
    const std::string& filtered_type,
    bool include_cluster,
    const std::vector<EngineAgentCatalogIdentitySource>& identity_sources,
    const DurableAgentCatalogImage& image) {
  std::vector<SysInformationAgentSource> sources;
  for (const auto& descriptor : CanonicalAgentRegistry()) {
    if (!filtered_type.empty() && descriptor.type_id != filtered_type) { continue; }
    if (descriptor.cluster_only && !include_cluster) { continue; }
    const auto* instance = FindDurableInstance(image, descriptor.type_id);
    if (instance == nullptr) { continue; }
    sources.push_back(DurableAgentProjectionSource(
        descriptor,
        context,
        FindAgentCatalogIdentitySource(identity_sources, descriptor.type_id),
        image,
        *instance));
  }
  return sources;
}

std::vector<std::pair<std::string, std::string>> LegacyAgentDescriptorFields(
    const AgentTypeDescriptor& descriptor,
    const AgentRuntimeContext& context) {
  const auto policy = BaselinePolicyForAgent(descriptor);
  const auto feature = EvaluateAgentFeatureAvailability(descriptor, context);
  std::string metrics;
  for (const auto& dep : descriptor.metric_dependencies) {
    if (!metrics.empty()) { metrics += ","; }
    metrics += dep.metric_family;
  }
  return {{"agent_type", descriptor.type_id},
          {"deployment", AgentDeploymentName(descriptor.deployment)},
          {"scope", descriptor.scope},
          {"authority", AgentAuthorityClassName(descriptor.authority)},
          {"default_activation", AgentActivationProfileName(descriptor.default_activation)},
          {"effective_activation", AgentActivationProfileName(policy.activation)},
          {"feature_availability", AgentFeatureAvailabilityName(feature)},
          {"metric_dependencies", metrics}};
}

void AddAgentDescriptorRow(std::vector<std::pair<std::string, std::string>>* fields,
                           const AgentTypeDescriptor& descriptor,
                           const AgentRuntimeContext& context) {
  if (fields == nullptr) { return; }
  auto descriptor_fields = LegacyAgentDescriptorFields(descriptor, context);
  fields->insert(fields->end(), descriptor_fields.begin(), descriptor_fields.end());
}

void AddAgentProjectionRows(EngineApiResult* result,
                            const EngineApiRequest& request,
                            const std::string& filtered_type,
                            bool include_cluster,
                            bool include_legacy_fields,
                            const std::vector<EngineAgentCatalogIdentitySource>& identity_sources,
                            const DurableAgentCatalogImage* durable_image = nullptr) {
  const auto agent_context = AgentContextFromRequest(request);
  const bool durable_source = durable_image != nullptr;
  const auto sources = durable_source
                           ? DurableAgentProjectionSources(agent_context,
                                                           filtered_type,
                                                           include_cluster,
                                                           identity_sources,
                                                           *durable_image)
                           : AgentProjectionSources(agent_context,
                                                    filtered_type,
                                                    include_cluster,
                                                    identity_sources);
  const auto projection = BuildSysInformationProjection("sys.agents",
                                                        AgentProjectionContext(request),
                                                        {},
                                                        {},
                                                        {},
                                                        {},
                                                        {},
                                                        {},
                                                        {},
                                                        {},
                                                        {},
                                                        sources);
  if (!projection.ok) {
    AddApiBehaviorRow(result,
                      {{"result_state", "refused"},
                       {"diagnostic_code", projection.diagnostic_code},
                       {"diagnostic_detail", projection.diagnostic_detail}});
    return;
  }

  for (const auto& row : projection.rows) {
    std::vector<std::pair<std::string, std::string>> fields = row.fields;
    if (include_legacy_fields) {
      std::string type_id;
      for (const auto& field : row.fields) {
        if (field.first == "agent_type_id") {
          type_id = field.second;
          break;
        }
      }
      const auto descriptor = FindAgentType(type_id);
      if (descriptor.has_value()) {
        AddAgentDescriptorRow(&fields, *descriptor, agent_context);
      }
    }
    fields.push_back({"runtime_state_source",
                      durable_source ? "durable_runtime_catalog"
                                     : "canonical_registry_projection"});
    fields.push_back({"durable_catalog_authority",
                      durable_source && durable_image->authority.durable_catalog_authority
                          ? "true"
                          : "false"});
    fields.push_back({"mga_transaction_evidence",
                      durable_source && durable_image->authority.mga_transaction_evidence
                          ? "true"
                          : "false"});
    AddApiBehaviorRow(result, std::move(fields));
  }
}

std::string SysSurfaceForCommandSpec(const AgentCommandSurfaceSpec& spec) {
  const std::string_view operation = spec.operation_id;
  if (operation == "agents.metrics.get") { return "sys.agent_metric_dependencies"; }
  if (operation == "agents.policy.get") { return "sys.agent_policies"; }
  if (operation == "agents.evidence.list") { return "sys.agent_evidence"; }
  if (operation == "agents.audit.list") { return "sys.agent_audit"; }
  if (operation == "agents.actions.list") { return "sys.agent_actions"; }
  if (operation == "agents.overrides.list") { return "sys.agent_overrides"; }
  return "";
}

std::string AgentTypeForCommandSurface(const EngineApiRequest& request) {
  auto type = AgentTypeFromRequest(request);
  if (type.empty()) { type = "local"; }
  return type;
}

std::string AgentUuidForCommandSurface(
    const EngineAgentCommandSurfaceRequest& request,
    std::string_view agent_type_id) {
  const auto* source = FindAgentCatalogIdentitySource(request.agent_catalog_identity_sources,
                                                     agent_type_id);
  return source == nullptr ? std::string{} : source->agent_uuid;
}

void AddAgentCommandSurfaceRow(EngineApiResult* result,
                               const EngineAgentCommandSurfaceRequest& request,
                               const AgentCommandSurfaceSpec& spec,
                               const char* result_state,
                               const char* diagnostic_code) {
  const auto agent_type = AgentTypeForCommandSurface(request);
  const std::string sys_surface = SysSurfaceForCommandSpec(spec);
  AddApiBehaviorRow(result,
                    {{"command_family", spec.command_family},
                     {"operation_id", spec.operation_id},
                     {"sblr_operation", spec.opcode},
                     {"api_call", spec.api_call},
                     {"sys_surface", sys_surface},
                     {"agent_type_id", agent_type},
                     {"agent_uuid", AgentUuidForCommandSurface(request, agent_type)},
                     {"result_state", result_state},
                     {"diagnostic_code", diagnostic_code},
                     {"evidence_required", spec.evidence_kind[0] == '\0' ? "false" : "true"},
                     {"evidence_kind", spec.evidence_kind},
                     {"redaction_state", std::string(result_state) == "redacted" ? "redacted" : "not_redacted"},
                     {"payload_redacted", std::string(result_state) == "redacted" ? "true" : "false"},
                     {"status_source", "engine_api"},
                     {"cluster_scoped", spec.cluster_scoped ? "true" : "false"}});
}

EngineAgentCommandSurfaceResult AgentCommandSurfaceFailure(
    const EngineAgentCommandSurfaceRequest& request,
    const AgentCommandSurfaceSpec& spec,
    const char* code,
    const char* detail,
    bool permission_denied = false) {
  auto result = MakeApiBehaviorDiagnostic<EngineAgentCommandSurfaceResult>(
      request.context,
      spec.operation_id,
      MakeEngineApiDiagnostic(code, "agent.command_surface.diagnostic", detail, true));
  result.result_shape.result_kind = spec.result_kind;
  if (permission_denied) {
    result.evidence.push_back({"agent_denial_evidence", spec.operation_id});
  } else if (spec.evidence_kind[0] != '\0') {
    result.evidence.push_back({spec.evidence_kind, spec.operation_id});
  }
  result.evidence.push_back({"agent_command_surface", spec.operation_id});
  result.evidence.push_back({"agent_command_api", spec.api_call});
  AddAgentCommandSurfaceRow(&result, request, spec, permission_denied ? "denied" : "refused", code);
  return result;
}

EngineAgentCommandSurfaceResult AgentCommandSurfaceEmptySuccess(
    const EngineAgentCommandSurfaceRequest& request,
    const AgentCommandSurfaceSpec& spec) {
  auto result = MakeApiBehaviorSuccess<EngineAgentCommandSurfaceResult>(
      request.context,
      spec.operation_id);
  result.result_shape.result_kind = spec.result_kind;
  result.evidence.push_back({"agent_command_surface", spec.operation_id});
  result.evidence.push_back({"agent_command_api", spec.api_call});
  result.diagnostics.push_back(MakeEngineApiDiagnostic(
      "AGENT.NONE",
      "agent.command_surface.empty_result",
      spec.operation_id,
      false));
  AddAgentCommandSurfaceRow(&result, request, spec, "empty", "AGENT.NONE");
  return result;
}

EngineAgentCommandSurfaceResult AgentCommandSurfaceRedactedSuccess(
    const EngineAgentCommandSurfaceRequest& request,
    const AgentCommandSurfaceSpec& spec) {
  auto result = MakeApiBehaviorSuccess<EngineAgentCommandSurfaceResult>(
      request.context,
      spec.operation_id);
  result.result_shape.result_kind = spec.result_kind;
  result.diagnostics.push_back(MakeEngineApiDiagnostic(
      "AGENT.REDACTED",
      "agent.command_surface.redacted",
      spec.operation_id,
      false));
  result.evidence.push_back({"agent_command_surface", spec.operation_id});
  result.evidence.push_back({"agent_command_api", spec.api_call});
  result.evidence.push_back({"agent_redaction_evidence", spec.operation_id});
  AddAgentCommandSurfaceRow(&result, request, spec, "redacted", "AGENT.REDACTED");
  AddApiBehaviorRow(&result,
                    {{"result_state", "redacted"},
                     {"diagnostic_code", "AGENT.REDACTED"},
                     {"payload_redacted", "true"},
                     {"redaction_policy", SecurityOptionValue(request, "agent_redaction_policy:").empty()
                                              ? "summary_only"
                                              : SecurityOptionValue(request, "agent_redaction_policy:")},
                     {"candidate_rows_hidden", "false"}});
  return result;
}

AgentLifecycleMode LifecycleModeFromRequest(const EngineApiRequest& request) {
  if (SecurityOptionPresent(request, "lifecycle:database_create")) { return AgentLifecycleMode::database_create; }
  if (SecurityOptionPresent(request, "lifecycle:database_open")) { return AgentLifecycleMode::database_open; }
  if (SecurityOptionPresent(request, "lifecycle:database_close")) { return AgentLifecycleMode::database_close; }
  if (SecurityOptionPresent(request, "lifecycle:backup")) { return AgentLifecycleMode::backup; }
  if (SecurityOptionPresent(request, "lifecycle:restore")) { return AgentLifecycleMode::restore; }
  if (SecurityOptionPresent(request, "lifecycle:shutdown")) { return AgentLifecycleMode::shutdown; }
  if (SecurityOptionPresent(request, "lifecycle:crash_recovery")) { return AgentLifecycleMode::crash_recovery; }
  if (SecurityOptionPresent(request, "lifecycle:restricted_open")) { return AgentLifecycleMode::restricted_open; }
  if (SecurityOptionPresent(request, "lifecycle:read_only")) { return AgentLifecycleMode::read_only; }
  if (SecurityOptionPresent(request, "lifecycle:maintenance")) { return AgentLifecycleMode::maintenance; }
  if (SecurityOptionPresent(request, "lifecycle:repair")) { return AgentLifecycleMode::repair; }
  if (SecurityOptionPresent(request, "lifecycle:archive_hold")) { return AgentLifecycleMode::archive_hold; }
  return AgentLifecycleMode::normal;
}

EngineApiDiagnostic PersistAgentEvidence(const EngineApiRequest& request, const std::string& operation_id, const std::string& detail) {
  if (request.context.database_path.empty()) {
    return MakeInvalidRequestDiagnostic(operation_id, "database_path_required_for_agent_evidence");
  }
  const auto event = std::string("SBAGENT1\tEVIDENCE\t") + operation_id + "\t" + EncodeCrudText(detail);
  return AppendApiBehaviorEvent(request.context, event);
}

void UpdateInstanceStateFields(AgentInstanceRecord* instance,
                               AgentLifecycleState target_state,
                               platform::u64 now_microseconds,
                               bool dry_run) {
  if (instance == nullptr) { return; }
  instance->state = target_state;
  instance->instance_generation += 1;
  instance->disabled_by_operator = target_state == AgentLifecycleState::disabled;
  instance->safe_mode = target_state == AgentLifecycleState::safe_mode;
  instance->quarantined = target_state == AgentLifecycleState::quarantined;
  instance->cancellation_requested = false;
  if (target_state == AgentLifecycleState::running ||
      target_state == AgentLifecycleState::dry_run) {
    instance->run_generation += 1;
    instance->last_run_start_microseconds = now_microseconds;
    if (dry_run) { instance->last_run_end_microseconds = now_microseconds; }
  }
  if (target_state == AgentLifecycleState::stopped ||
      target_state == AgentLifecycleState::paused ||
      target_state == AgentLifecycleState::stopping) {
    instance->last_run_end_microseconds = now_microseconds;
  }
}

void UpsertDurableHealth(DurableAgentCatalogImage* image,
                         const AgentInstanceRecord& instance,
                         const AgentEvidenceRecord& evidence,
                         platform::u64 now_microseconds) {
  if (image == nullptr) { return; }
  auto* health = FindMutableDurableHealth(image, instance.instance_uuid);
  if (health == nullptr) {
    DurableAgentHealthRecord record;
    record.instance_uuid = instance.instance_uuid;
    image->health.push_back(std::move(record));
    health = &image->health.back();
  }
  health->health_state = instance.state == AgentLifecycleState::failed
                             ? "failed"
                             : instance.state == AgentLifecycleState::quarantined
                                   ? "quarantined"
                                   : "healthy";
  health->diagnostic_code = evidence.diagnostic_code;
  health->evidence_uuid = evidence.evidence_uuid;
  health->observed_at_microseconds = now_microseconds;
}

void AppendDurableManagementHistory(DurableAgentCatalogImage* image,
                                    const AgentInstanceRecord& instance,
                                    const AgentEvidenceRecord& evidence,
                                    std::string operation_id,
                                    platform::u64 now_microseconds) {
  if (image == nullptr) { return; }
  DurableAgentHistoryRecord history;
  history.history_uuid = evidence.evidence_uuid + ":history";
  history.subject_uuid = instance.instance_uuid;
  history.event_kind = std::move(operation_id);
  history.diagnostic_code = evidence.diagnostic_code;
  history.evidence_uuid = evidence.evidence_uuid;
  history.recorded_at_microseconds = now_microseconds;
  image->retained_history.push_back(std::move(history));
}

template <typename TResult>
std::optional<TResult> PersistDurableManagementMutation(
    const EngineApiRequest& request,
    const EngineAgentDurableRuntimeState& durable_state,
    const AgentTypeDescriptor& descriptor,
    const AgentRuntimeContext& agent_context,
    const std::string& operation_id,
    AgentLifecycleState target_state,
    bool dry_run) {
  if (auto failure = ValidateDurableRuntimeForMutation<TResult>(
          request, durable_state, operation_id);
      failure.has_value()) {
    return failure;
  }

  auto* image = durable_state.mutable_catalog;
  auto* instance = FindMutableDurableInstance(image, descriptor.type_id);
  if (instance == nullptr) {
    return AgentFailure<TResult>(request,
                                 operation_id,
                                 "SB_AGENT_MANAGEMENT.INSTANCE_NOT_FOUND",
                                 descriptor.type_id);
  }
  if (instance->instance_uuid.empty() || instance->policy_uuid.empty()) {
    return AgentFailure<TResult>(
        request,
        operation_id,
        "SB_AGENT_MANAGEMENT.INSTANCE_DURABLE_IDENTITY_REQUIRED",
        descriptor.type_id);
  }

  const platform::u64 now = agent_context.wall_now_microseconds == 0
                                ? 1
                                : agent_context.wall_now_microseconds;
  const auto previous_state = instance->state;
  UpdateInstanceStateFields(instance, target_state, now, dry_run);

  auto evidence = MakeAgentEvidence(
      agent_context,
      descriptor,
      *instance,
      dry_run ? "agent_management_dry_run_evidence"
              : "agent_management_mutation_evidence",
      "SB_AGENT_MANAGEMENT.DURABLE_STATE_UPDATED",
      operation_id + ":" + AgentLifecycleStateName(previous_state) + "->" +
          AgentLifecycleStateName(target_state));
  evidence.redaction_class = "standard";
  image->evidence.push_back(evidence);
  UpsertDurableHealth(image, *instance, evidence, now);
  AppendDurableManagementHistory(image, *instance, evidence, operation_id, now);
  const auto refresh =
      RefreshDurableAgentCatalogAuthorityDigest(image, evidence.evidence_uuid);
  if (!refresh.ok) {
    return AgentFailure<TResult>(request,
                                 operation_id,
                                 refresh.diagnostic_code,
                                 refresh.detail);
  }
  return std::nullopt;
}

template <typename TResult>
TResult ClusterProviderRoute(const EngineApiRequest& request,
                             std::string operation_id,
                             std::string opcode) {
  cluster_provider::ClusterProviderRequest cluster_request;
  cluster_request.context = request.context;
  cluster_request.envelope.operation_id = operation_id;
  cluster_request.envelope.opcode = std::move(opcode);
  cluster_request.envelope.trace_key = "agent-management-cluster-provider-boundary";
  cluster_request.envelope.requires_security_context = true;
  cluster_request.envelope.requires_cluster_authority = true;
  cluster_request.envelope.contains_sql_text = false;
  cluster_request.api_request = request;
  cluster_request.api_request.operation_id = operation_id;

  TResult result;
  static_cast<EngineApiResult&>(result) =
      cluster_provider::ExecuteClusterOperation(cluster_request);
  AddApiBehaviorEvidence(&result, "agent_cluster_api_route", "provider_boundary");
  return result;
}

template <typename TResult>
TResult MutatingAgentOperation(const EngineApiRequest& request,
                               const EngineAgentDurableRuntimeState& durable_state,
                               const std::string& operation_id,
                               AgentLifecycleState target_state,
                               bool dry_run) {
  const auto registry_status = ValidateCanonicalAgentRegistry();
  if (!registry_status.ok) { return AgentFailure<TResult>(request, operation_id, registry_status.diagnostic_code, registry_status.detail); }
  const auto type_id = AgentTypeFromRequest(request);
  const auto descriptor = FindAgentType(type_id);
  if (!descriptor.has_value()) { return AgentFailure<TResult>(request, operation_id, "SB_AGENT_REGISTRY.UNKNOWN_TYPE", type_id); }
  const auto agent_context = AgentContextFromRequest(request);
  const auto security = ValidateAgentSecurity(agent_context, *descriptor, dry_run ? "inspect" : "control");
  if (!security.ok) { return AgentSecurityFailure<TResult>(request, operation_id, security.detail); }
  if (!dry_run) {
    if (const char* denial = AgentOpenStateMutationDenialCode(request); denial != nullptr) {
      return AgentFailure<TResult>(request, operation_id, denial, operation_id);
    }
  }
  const auto feature = EvaluateAgentFeatureAvailability(*descriptor, agent_context);
  if (feature != scratchbird::core::agents::AgentFeatureAvailability::available) {
    auto result = AgentFailure<TResult>(request, operation_id, "SB_AGENT_FEATURE.UNAVAILABLE", AgentFeatureAvailabilityName(feature));
    if (descriptor->cluster_only) { result.cluster_authority_required = true; }
    return result;
  }
  EngineAgentDurableRuntimeState effective_durable_state = durable_state;
  std::optional<AgentDurableCatalogStoreResult> loaded_store_catalog;
  bool store_backed_catalog = false;
  const bool durable_required =
      DurableRuntimeRequired(request, effective_durable_state);
  if (auto failure =
          RequireProductionDurableCatalogStoreContext<TResult>(
              request, operation_id, effective_durable_state);
      failure.has_value()) {
    return *failure;
  }
  if (durable_required && DurableCatalogStoreContextAvailable(request)) {
    auto loaded = LoadAgentDurableCatalogImage(request.context, true);
    if (!loaded.ok) {
      auto failure = StoreFailure<TResult>(
          request,
          operation_id,
          loaded,
          "SB_AGENT_MANAGEMENT.DURABLE_CATALOG_REQUIRED",
          "production agent management requires stored durable runtime state");
      return *failure;
    }
    loaded_store_catalog = std::move(loaded);
    effective_durable_state.catalog = &loaded_store_catalog->image;
    effective_durable_state.mutable_catalog = &loaded_store_catalog->image;
    store_backed_catalog = true;
  }
  const bool durable_available =
      effective_durable_state.mutable_catalog != nullptr;
  if (durable_required || durable_available) {
    if (auto failure = ValidateDurableRuntimeForMutation<TResult>(
            request, effective_durable_state, operation_id);
        failure.has_value()) {
      return *failure;
    }
    if (!StrictObservedMetricSnapshotEvidencePresent(request)) {
      return AgentFailure<TResult>(
          request,
          operation_id,
          "SB_AGENT_METRIC_SNAPSHOT.PRODUCTION_OBSERVED_SNAPSHOT_REQUIRED",
          descriptor->type_id);
    }
    AgentMetricSnapshotEvaluationOptions metric_options;
    metric_options.mode = AgentMetricRuntimeMode::production_strict;
    metric_options.expected_scope_uuid = agent_context.database_uuid;
    const auto metric_evaluation = EvaluateAgentObservedMetricSnapshots(
        *descriptor,
        agent_context,
        ObservedMetricSnapshotsFromRequest(request, *descriptor, agent_context),
        metric_options);
    if (!metric_evaluation.accepted) {
      return AgentFailure<TResult>(
          request,
          operation_id,
          metric_evaluation.status.diagnostic_code,
          metric_evaluation.status.detail);
    }
    if (auto failure = ValidateProductionFixtureSeparationForMutation<TResult>(
            request,
            effective_durable_state,
            operation_id,
            dry_run);
        failure.has_value()) {
      return *failure;
    }
  }
  const auto metrics = ResolveAgentMetricDependencies(*descriptor, agent_context);
  if (!metrics.ok) { return AgentFailure<TResult>(request, operation_id, metrics.diagnostic_code, metrics.detail); }
  auto policy = BaselinePolicyForAgent(*descriptor);
  policy.activation = EffectiveActivationForLifecycle(policy.activation, LifecycleModeFromRequest(request));
  const auto policy_status = ValidateAgentPolicy(policy, *descriptor);
  if (!policy_status.ok) { return AgentFailure<TResult>(request, operation_id, policy_status.diagnostic_code, policy_status.detail); }

  bool resource_reservation_acquired = false;
  bool resource_reservation_released = false;
  std::string resource_reservation_uuid;
  if (durable_required || durable_available) {
    const auto reservation = DurableResourceReservationForManagement(
        request, *descriptor, agent_context, operation_id);
    const auto acquired = AcquireDurableAgentResourceReservation(
        effective_durable_state.mutable_catalog, reservation);
    if (!acquired.ok) {
      return AgentFailure<TResult>(request,
                                   operation_id,
                                   acquired.diagnostic_code,
                                   acquired.detail);
    }
    resource_reservation_acquired = true;
    resource_reservation_uuid = reservation.reservation_uuid;

    if (auto failure = PersistDurableManagementMutation<TResult>(
            request,
            effective_durable_state,
            *descriptor,
            agent_context,
            operation_id,
            target_state,
            dry_run);
        failure.has_value()) {
      (void)ReleaseDurableAgentResourceReservation(
          effective_durable_state.mutable_catalog,
          reservation.reservation_uuid,
          reservation.evidence_uuid,
          reservation.now_microseconds + 1,
          DurableAgentResourceReservationState::cancelled);
      return *failure;
    }
    const auto released = ReleaseDurableAgentResourceReservation(
        effective_durable_state.mutable_catalog,
        reservation.reservation_uuid,
        effective_durable_state.mutable_catalog->authority.evidence_uuid,
        reservation.now_microseconds + 1,
        DurableAgentResourceReservationState::released);
    if (!released.ok) {
      return AgentFailure<TResult>(request,
                                   operation_id,
                                   released.diagnostic_code,
                                   released.detail);
    }
    resource_reservation_released = true;
    if (store_backed_catalog) {
      AgentDurableCatalogStoreRequest store_request;
      store_request.context = request.context;
      store_request.image = *effective_durable_state.mutable_catalog;
      store_request.evidence_uuid =
          effective_durable_state.mutable_catalog->authority.evidence_uuid;
      store_request.production_live_path = true;
      store_request.fsync_or_checkpoint_evidence =
          DurableCatalogStoreCheckpointEvidencePresent(request);
      const auto stored = PersistAgentDurableCatalogImage(store_request);
      if (!stored.ok) {
        auto failure = StoreFailure<TResult>(
            request,
            operation_id,
            stored,
            "SB_AGENT_MANAGEMENT.DURABLE_CATALOG_STORE_REFUSED",
            "durable catalog mutation was not committed to storage");
        return *failure;
      }
      *effective_durable_state.mutable_catalog = stored.image;
    }
  } else if (!dry_run) {
    const auto evidence = PersistAgentEvidence(request, operation_id, descriptor->type_id + ":" + AgentLifecycleStateName(target_state));
    if (evidence.error) { return MakeApiBehaviorDiagnostic<TResult>(request.context, operation_id, evidence); }
  }
  auto result = MakeApiBehaviorSuccess<TResult>(request.context, operation_id);
  const bool durable_mode = durable_required || durable_available;
  const bool sidecar_used = !durable_mode && !dry_run;
  AddApiBehaviorEvidence(&result, "agent_runtime", operation_id);
  AddApiBehaviorEvidence(&result, "agent_type", descriptor->type_id);
  AddApiBehaviorEvidence(&result,
                         durable_mode
                             ? "agent_durable_runtime_catalog"
                             : "agent_legacy_sidecar_evidence",
                         descriptor->type_id);
  AddApiBehaviorRow(&result,
                    {{"agent_type", descriptor->type_id},
                     {"target_state", AgentLifecycleStateName(target_state)},
                     {"dry_run", dry_run ? "true" : "false"},
                     {"activation", AgentActivationProfileName(policy.activation)},
                     {"feature_availability", AgentFeatureAvailabilityName(feature)},
                     {"runtime_state_source",
                      durable_mode
                          ? "durable_runtime_catalog"
                          : "legacy_sidecar_event"},
                     {"durable_catalog_mutated",
                      durable_mode ? "true" : "false"},
                     {"durable_catalog_store_mutated",
                      store_backed_catalog ? "true" : "false"},
                     {"durable_resource_reservation",
                      resource_reservation_acquired ? "true" : "false"},
                     {"durable_resource_reservation_released",
                      resource_reservation_released ? "true" : "false"},
                     {"durable_resource_reservation_uuid",
                      resource_reservation_uuid},
                     {"sidecar_event_used",
                      sidecar_used ? "true" : "false"}});
  return result;
}

}  // namespace

EngineListAgentsResult EngineListAgents(const EngineListAgentsRequest& request) {
  const auto grant = EvaluateAgentCommandGrant(
      AgentContextFromRequest(request),
      AgentSecurityCommandFamily::state_read,
      "agents.list");
  if (!grant.allowed) {
    return AgentSecurityFailure<EngineListAgentsResult>(
        request, "agents.list", grant.missing_right.empty() ? grant.diagnostic_code
                                                            : grant.missing_right);
  }
  auto result = MakeApiBehaviorSuccess<EngineListAgentsResult>(request.context, "agents.list");
  std::optional<AgentDurableCatalogStoreResult> loaded_store_catalog;
  const DurableAgentCatalogImage* durable_catalog = nullptr;
  if (auto failure = ResolveDurableCatalogForRead<EngineListAgentsResult>(
          request,
          request.durable_runtime_state,
          "agents.list",
          &durable_catalog,
          &loaded_store_catalog);
      failure.has_value()) {
    return *failure;
  }
  const auto filtered_type = AgentTypeFromRequest(request);
  AddAgentProjectionRows(&result,
                         request,
                         filtered_type,
                         request.context.cluster_authority_available,
                         true,
                         request.agent_catalog_identity_sources,
                         durable_catalog);
  AddApiBehaviorEvidence(&result,
                         durable_catalog == nullptr ? "agent_registry"
                                                    : "agent_durable_runtime_catalog",
                         durable_catalog == nullptr ? "canonical" : "production_state");
  AddApiBehaviorEvidence(&result, "sys_surface", "sys.agents");
  return result;
}

EngineShowAgentResult EngineShowAgent(const EngineShowAgentRequest& request) {
  const auto grant = EvaluateAgentCommandGrant(
      AgentContextFromRequest(request),
      AgentSecurityCommandFamily::state_read,
      "agents.show");
  if (!grant.allowed) {
    return AgentSecurityFailure<EngineShowAgentResult>(
        request, "agents.show", grant.missing_right.empty() ? grant.diagnostic_code
                                                            : grant.missing_right);
  }
  const auto type_id = AgentTypeFromRequest(request);
  const auto descriptor = FindAgentType(type_id);
  if (!descriptor.has_value()) { return AgentFailure<EngineShowAgentResult>(request, "agents.show", "SB_AGENT_REGISTRY.UNKNOWN_TYPE", type_id); }
  auto result = MakeApiBehaviorSuccess<EngineShowAgentResult>(request.context, "agents.show");
  std::optional<AgentDurableCatalogStoreResult> loaded_store_catalog;
  const DurableAgentCatalogImage* durable_catalog = nullptr;
  if (auto failure = ResolveDurableCatalogForRead<EngineShowAgentResult>(
          request,
          request.durable_runtime_state,
          "agents.show",
          &durable_catalog,
          &loaded_store_catalog);
      failure.has_value()) {
    return *failure;
  }
  const auto context = AgentContextFromRequest(request);
  AddAgentProjectionRows(&result,
                         request,
                         descriptor->type_id,
                         request.context.cluster_authority_available,
                         true,
                         request.agent_catalog_identity_sources,
                         durable_catalog);
  const auto time = ResolveAgentTimeAuthority(context, descriptor->cluster_only);
  AddApiBehaviorRow(&result,
                    {{"agent_type", descriptor->type_id},
                     {"time_authority", time.clock_quality},
                     {"time_status", time.status.diagnostic_code},
                     {"catalog_layouts", std::to_string(AgentCatalogRecordLayouts().size())},
                     {"runtime_state_source",
                      durable_catalog == nullptr ? "canonical_registry_projection"
                                                 : "durable_runtime_catalog"}});
  AddApiBehaviorEvidence(&result, "agent_type", descriptor->type_id);
  if (durable_catalog != nullptr) {
    AddApiBehaviorEvidence(&result, "agent_durable_runtime_catalog",
                           descriptor->type_id);
  }
  return result;
}

EngineStartAgentResult EngineStartAgent(const EngineStartAgentRequest& request) {
  return MutatingAgentOperation<EngineStartAgentResult>(request, request.durable_runtime_state, "agents.start", AgentLifecycleState::running, false);
}

EngineStopAgentResult EngineStopAgent(const EngineStopAgentRequest& request) {
  return MutatingAgentOperation<EngineStopAgentResult>(request, request.durable_runtime_state, "agents.stop", AgentLifecycleState::stopped, false);
}

EnginePauseAgentResult EnginePauseAgent(const EnginePauseAgentRequest& request) {
  return MutatingAgentOperation<EnginePauseAgentResult>(request, request.durable_runtime_state, "agents.pause", AgentLifecycleState::paused, false);
}

EngineResumeAgentResult EngineResumeAgent(const EngineResumeAgentRequest& request) {
  return MutatingAgentOperation<EngineResumeAgentResult>(request, request.durable_runtime_state, "agents.resume", AgentLifecycleState::running, false);
}

EngineConfigureAgentResult EngineConfigureAgent(const EngineConfigureAgentRequest& request) {
  return MutatingAgentOperation<EngineConfigureAgentResult>(request, request.durable_runtime_state, "agents.configure", AgentLifecycleState::registered, false);
}

EngineRunAgentResult EngineRunAgent(const EngineRunAgentRequest& request) {
  return MutatingAgentOperation<EngineRunAgentResult>(request, request.durable_runtime_state, "agents.run", AgentLifecycleState::running, false);
}

EngineDryRunAgentResult EngineDryRunAgent(const EngineDryRunAgentRequest& request) {
  return MutatingAgentOperation<EngineDryRunAgentResult>(request, request.durable_runtime_state, "agents.dry_run", AgentLifecycleState::dry_run, true);
}

EngineOverrideAgentResult EngineOverrideAgent(const EngineOverrideAgentRequest& request) {
  if (!SecurityContextHasRight(request.context, "OBS_AGENT_OVERRIDE")) {
    return AgentSecurityFailure<EngineOverrideAgentResult>(request, "agents.override", "OBS_AGENT_OVERRIDE");
  }
  return MutatingAgentOperation<EngineOverrideAgentResult>(request, request.durable_runtime_state, "agents.override", AgentLifecycleState::running, false);
}

EngineSysAgentsResult EngineSysAgents(const EngineSysAgentsRequest& request) {
  const auto grant = EvaluateAgentCommandGrant(
      AgentContextFromRequest(request),
      AgentSecurityCommandFamily::state_read,
      "sys.agents");
  if (!grant.allowed) {
    return AgentSecurityFailure<EngineSysAgentsResult>(
        request, "sys.agents", grant.missing_right.empty() ? grant.diagnostic_code
                                                           : grant.missing_right);
  }
  auto result = MakeApiBehaviorSuccess<EngineSysAgentsResult>(request.context, "sys.agents");
  std::optional<AgentDurableCatalogStoreResult> loaded_store_catalog;
  const DurableAgentCatalogImage* durable_catalog = nullptr;
  if (auto failure = ResolveDurableCatalogForRead<EngineSysAgentsResult>(
          request,
          request.durable_runtime_state,
          "sys.agents",
          &durable_catalog,
          &loaded_store_catalog);
      failure.has_value()) {
    return *failure;
  }
  AddAgentProjectionRows(&result,
                         request,
                         "",
                         false,
                         true,
                         request.agent_catalog_identity_sources,
                         durable_catalog);
  AddApiBehaviorEvidence(&result, "sys_surface", "sys.agents");
  if (durable_catalog != nullptr) {
    AddApiBehaviorEvidence(&result, "agent_durable_runtime_catalog",
                           "sys.agents");
  }
  return result;
}

EngineClusterSysAgentsResult EngineClusterSysAgents(const EngineClusterSysAgentsRequest& request) {
  const auto grant = EvaluateAgentCommandGrant(
      AgentContextFromRequest(request),
      AgentSecurityCommandFamily::cluster_inspect,
      "cluster.sys.agents",
      false,
      false);
  if (!grant.allowed) {
    return AgentSecurityFailure<EngineClusterSysAgentsResult>(
        request, "cluster.sys.agents", grant.missing_right.empty() ? grant.diagnostic_code
                                                                   : grant.missing_right);
  }
  return ClusterProviderRoute<EngineClusterSysAgentsResult>(
      request,
      "cluster.sys.agents",
      "SBLR_CLUSTER_SYS_AGENTS");
}

EngineAgentCommandSurfaceResult EngineAgentCommandSurfaceOperation(
    const EngineAgentCommandSurfaceRequest& request) {
  const auto* spec = FindAgentCommandSurfaceSpec(request.operation_id);
  if (spec == nullptr) {
    auto result = MakeApiBehaviorDiagnostic<EngineAgentCommandSurfaceResult>(
        request.context,
        request.operation_id,
        MakeEngineApiDiagnostic("AGENT.COMMAND_SURFACE.UNKNOWN",
                                "agent.command_surface.unknown",
                                request.operation_id,
                                true));
    result.evidence.push_back({"agent_command_surface", "unknown"});
    return result;
  }
  EngineAgentCommandSurfaceRequest effective_request = request;
  std::optional<AgentDurableCatalogStoreResult> loaded_catalog;
  if (AgentManagementProductionLive(request)) {
    if (!DurableCatalogStoreContextAvailable(request)) {
      return AgentCommandSurfaceFailure(
          request,
          *spec,
          "SB_AGENT_MANAGEMENT.DURABLE_CATALOG_REQUIRED",
          "production command surface requires durable catalog store",
          false);
    }
    auto loaded = LoadAgentDurableCatalogImage(request.context, true);
    if (!loaded.ok) {
      return AgentCommandSurfaceFailure(
          request,
          *spec,
          loaded.diagnostic.code.empty()
              ? "SB_AGENT_MANAGEMENT.DURABLE_CATALOG_REQUIRED"
              : loaded.diagnostic.code.c_str(),
          loaded.diagnostic.detail.empty()
              ? "production command surface requires durable catalog store"
              : loaded.diagnostic.detail.c_str(),
          false);
    }
    loaded_catalog = std::move(loaded);
    effective_request.agent_catalog_identity_sources =
        IdentitySourcesFromDurableCatalog(loaded_catalog->image);
  }

  if (!UsesProjectionMatrixRights(*spec)) {
    const auto grant = EvaluateAgentCommandGrant(
        AgentContextFromRequest(effective_request),
        CommandFamilyForSpec(*spec),
        spec->operation_id,
        SecurityOptionBool(effective_request, "agent_scope_restricted:", false),
        false);
    if (!grant.allowed) {
      const char* code = grant.diagnostic_code == "AGENT.SECURITY_CONTEXT_REQUIRED"
                             ? "AGENT.SECURITY_CONTEXT_REQUIRED"
                             : spec->permission_code;
      return AgentCommandSurfaceFailure(effective_request,
                                        *spec,
                                        code,
                                        grant.missing_right.empty()
                                            ? AgentSecurityDenialKindName(grant.denial)
                                            : grant.missing_right.c_str(),
                                        true);
    }
  } else if (!request.context.security_context_present) {
    return AgentCommandSurfaceFailure(effective_request,
                                      *spec,
                                      "AGENT.SECURITY_CONTEXT_REQUIRED",
                                      "missing_security_context",
                                      true);
  }

  if (!HasRequiredAgentSurfaceRights(effective_request, *spec)) {
    return AgentCommandSurfaceFailure(effective_request,
                                      *spec,
                                      spec->permission_code,
                                      spec->required_right_primary,
                                      true);
  }

  if (spec->mutating) {
    if (const char* denial = AgentOpenStateMutationDenialCode(effective_request); denial != nullptr) {
      return AgentCommandSurfaceFailure(effective_request, *spec, denial, spec->operation_id);
    }
  }
  const auto policy = ManagementPolicyValue(effective_request);
  if (policy == "deny" || policy == "deny_all" ||
      (spec->mutating && (policy == "read_only" || policy == "inspect_only"))) {
    return AgentCommandSurfaceFailure(effective_request,
                                      *spec,
                                      "AGENT.POLICY_DENIED",
                                      policy.empty() ? spec->operation_id : policy.c_str());
  }
  if (!MetricFreshForCommandSurface(effective_request, *spec)) {
    return AgentCommandSurfaceFailure(effective_request, *spec, "METRIC.STALE", spec->operation_id);
  }
  if (!PolicyFreshForCommandSurface(effective_request, *spec)) {
    return AgentCommandSurfaceFailure(effective_request, *spec, "POLICY.STALE", spec->operation_id);
  }
  const auto redaction = SecurityLower(SecurityOptionValue(effective_request, "agent_redaction_policy:"));
  if (!spec->mutating && (redaction == "summary_only" || redaction == "redact")) {
    return AgentCommandSurfaceRedactedSuccess(effective_request, *spec);
  }

  if (spec->cluster_scoped) {
    return ClusterProviderRoute<EngineAgentCommandSurfaceResult>(
        effective_request,
        spec->operation_id,
        spec->opcode);
  }

  if (std::string_view(spec->success_state) == "empty") {
    auto result = AgentCommandSurfaceEmptySuccess(effective_request, *spec);
    if (loaded_catalog.has_value()) {
      AddApiBehaviorEvidence(&result,
                             "agent_command_durable_catalog",
                             loaded_catalog->image.authority.catalog_root_digest);
    }
    return result;
  }

  auto result = AgentCommandSurfaceFailure(effective_request,
                                    *spec,
                                    spec->refusal_code,
                                    spec->api_call);
  if (loaded_catalog.has_value()) {
    AddApiBehaviorEvidence(&result,
                           "agent_command_durable_catalog",
                           loaded_catalog->image.authority.catalog_root_digest);
  }
  return result;
}

EngineThirdPartyAgentManagementResult EngineSubmitThirdPartyAgentManagementRequest(
    const EngineThirdPartyAgentManagementRequest& request) {
  constexpr const char* kOperation = "agents.third_party.request";
  EngineThirdPartyAgentManagementRequest effective_request = request;
  std::optional<AgentDurableCatalogStoreResult> loaded_catalog;
  if (AgentManagementProductionLive(request)) {
    if (!DurableCatalogStoreContextAvailable(request)) {
      return ThirdPartyFailure(
          request,
          "SB_AGENT_MANAGEMENT.DURABLE_CATALOG_REQUIRED",
          "production third-party management requires durable catalog store");
    }
    auto loaded = LoadAgentDurableCatalogImage(request.context, true);
    if (!loaded.ok) {
      return ThirdPartyFailure(
          request,
          loaded.diagnostic.code.empty()
              ? "SB_AGENT_MANAGEMENT.DURABLE_CATALOG_REQUIRED"
              : loaded.diagnostic.code,
          loaded.diagnostic.detail.empty()
              ? "production third-party management requires durable catalog store"
              : loaded.diagnostic.detail);
    }
    loaded_catalog = std::move(loaded);
    effective_request.agent_catalog_identity_sources =
        IdentitySourcesFromDurableCatalog(loaded_catalog->image);
  }
  const auto& record = effective_request.management_request;

  if (!effective_request.context.security_context_present) {
    return MakeApiBehaviorDiagnostic<EngineThirdPartyAgentManagementResult>(
        effective_request.context,
        kOperation,
        MakeSecurityDiagnostic("AGENT.SECURITY_CONTEXT_REQUIRED", "security_context_required"));
  }
  if (record.requester_principal_uuid.empty()) {
    auto result = ThirdPartyFailure(effective_request,
                                    "AGENT.THIRD_PARTY.REQUESTER_PRINCIPAL_REQUIRED",
                                    "requester_principal_uuid_required");
    AddThirdPartyRequestEvidenceIfTyped(&result, record);
    return result;
  }
  if (record.idempotency_key.empty()) {
    auto result = ThirdPartyFailure(effective_request,
                                    "AGENT.THIRD_PARTY.IDEMPOTENCY_REQUIRED",
                                    "idempotency_key_required");
    AddThirdPartyRequestEvidenceIfTyped(&result, record);
    return result;
  }
  if (record.external_system_id.empty() || record.agent_ref.empty() ||
      record.operation.empty() || record.requested_action.empty() ||
      record.reason_code.empty() || record.requested_expiry.empty()) {
    auto result = ThirdPartyFailure(effective_request,
                                    "AGENT.THIRD_PARTY.REQUIRED_FIELD_MISSING",
                                    "external_system_agent_operation_action_reason_expiry_required");
    AddThirdPartyRequestEvidenceIfTyped(&result, record);
    return result;
  }
  if (!record.residency_context_present) {
    auto result = ThirdPartyFailure(effective_request,
                                    "AGENT.THIRD_PARTY.RESIDENCY_CONTEXT_REQUIRED",
                                    "residency_context_required");
    AddThirdPartyRequestEvidenceIfTyped(&result, record);
    return result;
  }
  if (!record.redaction_context_present || record.redaction_context.empty()) {
    auto result = ThirdPartyFailure(effective_request,
                                    "AGENT.THIRD_PARTY.REDACTION_CONTEXT_REQUIRED",
                                    "redaction_context_required");
    AddThirdPartyRequestEvidenceIfTyped(&result, record);
    return result;
  }
  if (const auto invalid = ValidateThirdPartyUuidField(record); invalid.has_value()) {
    auto result = ThirdPartyFailure(effective_request,
                                    "AGENT.THIRD_PARTY.INVALID_UUID",
                                    *invalid);
    AddThirdPartyRequestEvidenceIfTyped(&result, record);
    return result;
  }
  if (effective_request.context.principal_uuid.canonical != record.requester_principal_uuid) {
    auto result = ThirdPartyFailure(effective_request,
                                    "AGENT.THIRD_PARTY.REQUESTER_PRINCIPAL_MISMATCH",
                                    "requester_principal_uuid_mismatch");
    AddThirdPartyRequestEvidenceIfTyped(&result, record);
    return result;
  }
  if (ThirdPartyDirectActuatorBypass(record)) {
    auto result = ThirdPartyFailure(effective_request,
                                    "AGENT.THIRD_PARTY.ACTUATOR_BYPASS_DENIED",
                                    "third_party_requests_cannot_call_actuators");
    AddApiBehaviorEvidence(&result, "agent_third_party_request_evidence", record.request_uuid);
    return result;
  }

  const auto* spec = FindAgentCommandSurfaceSpec(record.operation);
  if (spec == nullptr) {
    auto result = ThirdPartyFailure(effective_request,
                                    "AGENT.THIRD_PARTY.OPERATION_NOT_ALLOWED",
                                    record.operation);
    AddApiBehaviorEvidence(&result, "agent_third_party_request_evidence", record.request_uuid);
    return result;
  }
  if (!ThirdPartyRequestedActionAllowed(*spec, record.requested_action)) {
    auto result = ThirdPartyFailure(effective_request,
                                    "AGENT.THIRD_PARTY.ACTION_NOT_ALLOWED",
                                    record.requested_action);
    AddApiBehaviorEvidence(&result, "agent_third_party_request_evidence", record.request_uuid);
    return result;
  }

  const auto* agent = ResolveThirdPartyAgent(effective_request.agent_catalog_identity_sources,
                                            record.agent_ref);
  if (!ThirdPartyResolvedAgentHasUuidAuthority(agent)) {
    auto result = ThirdPartyFailure(effective_request,
                                    "AGENT.THIRD_PARTY.AGENT_NOT_FOUND",
                                    record.agent_ref);
    AddApiBehaviorEvidence(&result, "agent_third_party_request_evidence", record.request_uuid);
    return result;
  }
  if (!ThirdPartyResolvedPolicyMatchesRequest(*agent, record.policy_ref)) {
    auto result = ThirdPartyFailure(effective_request,
                                    "AGENT.THIRD_PARTY.POLICY_MISMATCH",
                                    "agent_policy_uuid_mismatch");
    AddApiBehaviorEvidence(&result, "agent_third_party_request_evidence", record.request_uuid);
    return result;
  }

  if (!HasRequiredAgentSurfaceRights(effective_request, *spec)) {
    auto result = ThirdPartyFailure(effective_request,
                                    "AGENT.THIRD_PARTY.PERMISSION_DENIED",
                                    spec->required_right_primary);
    AddApiBehaviorEvidence(&result, "agent_third_party_request_evidence", record.request_uuid);
    bool payload_redacted = false;
    auto protected_payload = RedactThirdPartyPayload(record.protected_payload, &payload_redacted);
    AddThirdPartyRequestRow(&result,
                            record,
                            *spec,
                            agent,
                            "denied",
                            "AGENT.THIRD_PARTY.PERMISSION_DENIED",
                            false,
                            payload_redacted,
                            std::move(protected_payload));
    return result;
  }
  if (spec->mutating) {
    if (const char* denial = AgentOpenStateMutationDenialCode(effective_request); denial != nullptr) {
      auto result = ThirdPartyFailure(effective_request, denial, record.operation);
      AddApiBehaviorEvidence(&result, "agent_third_party_request_evidence", record.request_uuid);
      bool payload_redacted = false;
      auto protected_payload = RedactThirdPartyPayload(record.protected_payload, &payload_redacted);
      AddThirdPartyRequestRow(&result,
                              record,
                              *spec,
                              agent,
                              "refused",
                              denial,
                              false,
                              payload_redacted,
                              std::move(protected_payload));
      return result;
    }
  }

  bool payload_redacted = false;
  auto protected_payload = RedactThirdPartyPayload(record.protected_payload, &payload_redacted);
  if (!record.residency_allowed) {
    auto result = ThirdPartyFailure(effective_request,
                                    "AGENT.THIRD_PARTY.RESIDENCY_DENIED",
                                    record.external_system_id);
    AddApiBehaviorEvidence(&result, "agent_third_party_request_evidence", record.request_uuid);
    AddThirdPartyRequestRow(&result,
                            record,
                            *spec,
                            agent,
                            "denied",
                            "AGENT.THIRD_PARTY.RESIDENCY_DENIED",
                            false,
                            payload_redacted,
                            std::move(protected_payload));
    return result;
  }
  if (!record.evidence_store_available) {
    auto result = ThirdPartyFailure(effective_request,
                                    "AGENT.THIRD_PARTY.EVIDENCE_REQUIRED",
                                    "request_evidence_store_unavailable");
    AddThirdPartyRequestRow(&result,
                            record,
                            *spec,
                            agent,
                            "pending_evidence",
                            "AGENT.THIRD_PARTY.EVIDENCE_REQUIRED",
                            false,
                            payload_redacted,
                            std::move(protected_payload));
    return result;
  }
  if (record.backpressure) {
    auto result = ThirdPartyFailure(effective_request,
                                    "AGENT.REQUEST_BACKPRESSURE",
                                    record.retry_after.empty() ? "retry_after_required" : record.retry_after);
    AddApiBehaviorEvidence(&result, "agent_third_party_request_evidence", record.request_uuid);
    AddThirdPartyRequestRow(&result,
                            record,
                            *spec,
                            agent,
                            "backpressure",
                            "AGENT.REQUEST_BACKPRESSURE",
                            false,
                            payload_redacted,
                            std::move(protected_payload));
    return result;
  }

  auto result = MakeApiBehaviorSuccess<EngineThirdPartyAgentManagementResult>(
      effective_request.context,
      kOperation);
  result.result_shape.result_kind = "agent.third_party_request.v1";
  AddApiBehaviorEvidence(&result, "agent_third_party_request_evidence", record.request_uuid);
  AddApiBehaviorEvidence(&result, "agent_command_surface", spec->operation_id);
  result.diagnostics.push_back(ThirdPartyDiagnostic(
      spec->mutating ? "AGENT.THIRD_PARTY.REQUEST_ACCEPTED"
                     : "AGENT.THIRD_PARTY.READ_RESULT",
      spec->operation_id,
      false));
  AddThirdPartyRequestRow(&result,
                          record,
                          *spec,
                          agent,
                          spec->mutating ? "request_accepted" : "read_result",
                          spec->mutating ? "AGENT.THIRD_PARTY.REQUEST_ACCEPTED"
                                         : "AGENT.THIRD_PARTY.READ_RESULT",
                          spec->mutating,
                          payload_redacted,
                          std::move(protected_payload));
  if (loaded_catalog.has_value()) {
    AddApiBehaviorEvidence(&result,
                           "agent_third_party_durable_catalog",
                           loaded_catalog->image.authority.catalog_root_digest);
  }
  return result;
}

EngineAgentZeroGreyOutputContract BuiltinAgentZeroGreyOutputContract() {
  EngineAgentZeroGreyOutputContract contract;
  contract.allowed_result_states.assign(std::begin(kZeroGreyResultStates),
                                        std::end(kZeroGreyResultStates));
  contract.prohibited_fragments.assign(std::begin(kZeroGreyProhibitedFragments),
                                       std::end(kZeroGreyProhibitedFragments));
  contract.evidence_payload_fields.assign(std::begin(kZeroGreyEvidencePayloadFields),
                                          std::end(kZeroGreyEvidencePayloadFields));
  return contract;
}

bool EngineAgentZeroGreyResultStateAllowed(std::string_view result_state) {
  if (result_state.empty()) { return false; }
  for (const auto* allowed : kZeroGreyResultStates) {
    if (result_state == allowed) { return true; }
  }
  return false;
}

bool EngineAgentZeroGreyResultStateAmbiguous(std::string_view result_state) {
  const auto lowered = LowerAscii(std::string(result_state));
  for (const auto* fragment : kZeroGreyProhibitedFragments) {
    if (lowered.find(fragment) != std::string::npos) { return true; }
  }
  return false;
}

}  // namespace scratchbird::engine::internal_api
