// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "agent_cluster_boundary.hpp"

#include "cluster_provider/cluster_provider.hpp"

// SEARCH_KEY: CLUSTER.EXTERNAL_PROVIDER_REQUIRED

#include <utility>

namespace scratchbird::core::agents {
namespace {

namespace cluster_provider = scratchbird::engine::cluster_provider;
namespace internal_api = scratchbird::engine::internal_api;
namespace sblr = scratchbird::engine::sblr;

std::string SurfaceOperationId(AgentClusterLeaseSurface surface) {
  return std::string("agent.cluster.") + AgentClusterLeaseSurfaceName(surface);
}

std::string SurfaceOpcode(AgentClusterLeaseSurface surface) {
  switch (surface) {
    case AgentClusterLeaseSurface::inspect:
      return "SBLR_AGENT_CLUSTER_LEASE_INSPECT";
    case AgentClusterLeaseSurface::acquire_lease:
      return "SBLR_AGENT_CLUSTER_LEASE_ACQUIRE";
    case AgentClusterLeaseSurface::renew_lease:
      return "SBLR_AGENT_CLUSTER_LEASE_RENEW";
    case AgentClusterLeaseSurface::failover:
      return "SBLR_AGENT_CLUSTER_FAILOVER";
    case AgentClusterLeaseSurface::authorize_action:
      return "SBLR_AGENT_CLUSTER_AUTHORIZE_ACTION";
  }
  return "SBLR_AGENT_CLUSTER_UNKNOWN";
}

internal_api::EngineRequestContext ToEngineContext(const AgentRuntimeContext& context) {
  internal_api::EngineRequestContext out;
  out.security_context_present = context.security_context_present;
  out.cluster_authority_available = context.cluster_authority_available;
  out.database_uuid.canonical = context.database_uuid;
  out.cluster_uuid.canonical = context.cluster_uuid;
  out.principal_uuid.canonical = context.principal_uuid;
  out.trace_tags = context.trace_tags;
  return out;
}

cluster_provider::ClusterProviderRequest ProviderRequest(const AgentRuntimeContext& context,
                                                         std::string operation_id,
                                                         std::string opcode) {
  cluster_provider::ClusterProviderRequest request;
  request.context = ToEngineContext(context);
  request.envelope.operation_id = std::move(operation_id);
  request.envelope.opcode = std::move(opcode);
  request.envelope.trace_key = "agent-cluster-provider-boundary";
  request.envelope.requires_security_context = true;
  request.envelope.requires_cluster_authority = true;
  request.envelope.contains_sql_text = false;
  request.api_request.context = request.context;
  request.api_request.operation_id = request.envelope.operation_id;
  return request;
}

bool HasDiagnosticCode(const AgentClusterBoundaryResult& result, const std::string& code) {
  for (const auto& diagnostic_code : result.diagnostic_codes) {
    if (diagnostic_code == code) {
      return true;
    }
  }
  return false;
}

bool ProviderIsExternalProductionProvider(
    const AgentClusterBoundaryResult& result) {
  return result.provider_type == "external_cluster_provider" &&
         result.provider_support_status == "enabled";
}

void AddDiagnosticCode(AgentClusterBoundaryResult* result, std::string code) {
  if (result == nullptr || code.empty() || HasDiagnosticCode(*result, code)) {
    return;
  }
  result->diagnostic_codes.push_back(std::move(code));
}

void AddEvidence(AgentClusterBoundaryResult* result, std::string kind, std::string id) {
  if (result == nullptr) {
    return;
  }
  result->evidence.push_back({std::move(kind), std::move(id)});
}

void EnforceExternalProviderForProductionLive(
    AgentClusterBoundaryResult* result) {
  if (result == nullptr || ProviderIsExternalProductionProvider(*result)) {
    return;
  }
  result->ok = false;
  result->cluster_path_failed_closed = true;
  result->cluster_authority_required = true;
  result->external_provider_required = true;
  result->preconditions_satisfied = false;
  result->action_authorized = false;
  result->diagnostic_code = kAgentClusterExternalProviderRequiredCode;
  result->detail =
      "production live cluster behavior requires external sb_cluster_provider";
  AddDiagnosticCode(result, result->diagnostic_code);
  AddEvidence(result, "agent_cluster_external_provider_required", "true");
}

AgentClusterBoundaryResult FromProviderResult(const internal_api::EngineApiResult& provider_result) {
  const auto info = cluster_provider::DescribeClusterProvider();
  AgentClusterBoundaryResult out;
  out.ok = provider_result.ok;
  out.provider_called = true;
  out.cluster_path_failed_closed = !provider_result.ok;
  out.cluster_authority_required = provider_result.cluster_authority_required;
  out.operation_id = provider_result.operation_id;
  out.provider_name = std::string(info.provider_name);
  out.provider_type = std::string(info.provider_type);
  out.provider_support_status = std::string(info.support_status);
  for (const auto& diagnostic : provider_result.diagnostics) {
    AddDiagnosticCode(&out, diagnostic.code);
    if (out.diagnostic_code.empty()) {
      out.diagnostic_code = diagnostic.code;
      out.detail = diagnostic.detail;
    }
  }
  for (const auto& unsupported : provider_result.unsupported_features) {
    AddEvidence(&out, "unsupported_feature", unsupported.feature);
    AddEvidence(&out, "unsupported_reason", unsupported.reason);
  }
  for (const auto& evidence : provider_result.evidence) {
    AddEvidence(&out, evidence.evidence_kind, evidence.evidence_id);
  }
  if (out.diagnostic_code.empty()) {
    out.diagnostic_code = out.ok ? "SB_AGENT_CLUSTER_PROVIDER.OK"
                                 : std::string(kAgentClusterSupportNotEnabledCode);
  }
  return out;
}

bool LeaseCurrent(const AgentClusterLeaseState& state,
                  const AgentClusterLeaseRequest& request) {
  return state.state == AgentClusterLeadershipState::leader_active &&
         state.owner_instance_uuid == request.instance_uuid &&
         state.epoch == request.epoch &&
         state.fence_token == request.fence_token &&
         !state.fence_token.empty() &&
         request.now_microseconds < state.lease_until_microseconds;
}

void MarkPreconditionFailure(AgentClusterBoundaryResult* result,
                             std::string diagnostic_code,
                             std::string detail) {
  if (result == nullptr) {
    return;
  }
  result->ok = false;
  result->preconditions_satisfied = false;
  result->action_authorized = false;
  result->cluster_path_failed_closed = true;
  result->diagnostic_code = std::move(diagnostic_code);
  result->detail = std::move(detail);
  AddDiagnosticCode(result, result->diagnostic_code);
}

void AddLeaseEvidence(AgentClusterBoundaryResult* result,
                      const AgentClusterLeaseState& state) {
  AddEvidence(result, "agent_cluster_leadership_state",
              AgentClusterLeadershipStateName(state.state));
  AddEvidence(result, "agent_cluster_lease_owner", state.owner_instance_uuid);
  AddEvidence(result, "agent_cluster_lease_epoch", std::to_string(state.epoch));
  AddEvidence(result, "agent_cluster_fence_token", state.fence_token);
  AddEvidence(result, "agent_cluster_lease_until",
              std::to_string(state.lease_until_microseconds));
}

std::string DeterministicFenceToken(const std::string& instance_uuid, u64 epoch) {
  return "agent-cluster-fence:" + instance_uuid + ":" + std::to_string(epoch);
}

}  // namespace

const char* AgentClusterLeadershipStateName(AgentClusterLeadershipState state) {
  switch (state) {
    case AgentClusterLeadershipState::follower: return "follower";
    case AgentClusterLeadershipState::candidate: return "candidate";
    case AgentClusterLeadershipState::leader_pending_fence: return "leader_pending_fence";
    case AgentClusterLeadershipState::leader_active: return "leader_active";
    case AgentClusterLeadershipState::leader_draining: return "leader_draining";
    case AgentClusterLeadershipState::lease_expired: return "lease_expired";
    case AgentClusterLeadershipState::quarantined: return "quarantined";
  }
  return "quarantined";
}

const char* AgentClusterLeaseSurfaceName(AgentClusterLeaseSurface surface) {
  switch (surface) {
    case AgentClusterLeaseSurface::inspect: return "inspect";
    case AgentClusterLeaseSurface::acquire_lease: return "acquire_lease";
    case AgentClusterLeaseSurface::renew_lease: return "renew_lease";
    case AgentClusterLeaseSurface::failover: return "failover";
    case AgentClusterLeaseSurface::authorize_action: return "authorize_action";
  }
  return "unknown";
}

AgentClusterBoundaryResult RouteAgentClusterProviderBoundary(
    const AgentRuntimeContext& context,
    std::string operation_id,
    bool production_live_path) {
  auto request = ProviderRequest(context, std::move(operation_id),
                                 "SBLR_AGENT_CLUSTER_PROVIDER_BOUNDARY");
  auto result = FromProviderResult(cluster_provider::ExecuteClusterOperation(request));
  AddEvidence(&result, "agent_cluster_boundary", "provider_route");
  if (production_live_path) {
    EnforceExternalProviderForProductionLive(&result);
  }
  return result;
}

AgentClusterBoundaryResult ApplyAgentClusterLeaseSurface(
    const AgentRuntimeContext& context,
    const AgentClusterLeaseRequest& request,
    AgentClusterLeaseState* state) {
  AgentClusterLeaseState fallback_state;
  AgentClusterLeaseState& lease_state = state == nullptr ? fallback_state : *state;
  const std::string operation_id = SurfaceOperationId(request.surface);
  auto provider_request = ProviderRequest(context, operation_id, SurfaceOpcode(request.surface));
  auto result = FromProviderResult(cluster_provider::ExecuteClusterOperation(provider_request));
  AddEvidence(&result, "agent_cluster_surface", AgentClusterLeaseSurfaceName(request.surface));
  if (request.production_live_path) {
    EnforceExternalProviderForProductionLive(&result);
  }

  if (!result.ok) {
    result.cluster_path_failed_closed = true;
    result.preconditions_satisfied = false;
    AddLeaseEvidence(&result, lease_state);
    return result;
  }

  switch (request.surface) {
    case AgentClusterLeaseSurface::inspect:
      result.preconditions_satisfied = true;
      break;
    case AgentClusterLeaseSurface::acquire_lease:
      if (lease_state.state == AgentClusterLeadershipState::quarantined ||
          request.instance_uuid.empty() ||
          request.lease_duration_microseconds == 0) {
        MarkPreconditionFailure(&result, lease_state.state == AgentClusterLeadershipState::quarantined
                                             ? kAgentClusterSplitBrainRefusedCode
                                             : "SB_AGENT_CLUSTER_LEASE.PRECONDITION_FAILED",
                                "acquire_requires_nonquarantined_instance_and_duration");
        break;
      }
      if (lease_state.state == AgentClusterLeadershipState::leader_active &&
          request.now_microseconds < lease_state.lease_until_microseconds) {
        MarkPreconditionFailure(&result, kAgentClusterLeaderExistsCode,
                                "current_leader_lease_still_active");
        break;
      }
      lease_state.owner_instance_uuid = request.instance_uuid;
      lease_state.epoch += 1;
      lease_state.fence_token = DeterministicFenceToken(request.instance_uuid, lease_state.epoch);
      lease_state.lease_until_microseconds =
          request.now_microseconds + request.lease_duration_microseconds;
      lease_state.state = AgentClusterLeadershipState::leader_active;
      result.preconditions_satisfied = true;
      break;
    case AgentClusterLeaseSurface::renew_lease:
      if (lease_state.state == AgentClusterLeadershipState::leader_active &&
          request.now_microseconds >= lease_state.lease_until_microseconds) {
        lease_state.state = AgentClusterLeadershipState::lease_expired;
        MarkPreconditionFailure(&result, kAgentClusterLeaseRequiredCode,
                                "renew_requires_unexpired_lease");
        break;
      }
      if (!LeaseCurrent(lease_state, request) ||
          request.lease_duration_microseconds == 0) {
        MarkPreconditionFailure(&result,
                                request.lease_duration_microseconds == 0
                                    ? "SB_AGENT_CLUSTER_LEASE.PRECONDITION_FAILED"
                                    : kAgentClusterFenceStaleCode,
                                "renew_requires_current_matching_epoch_and_fence");
        break;
      }
      lease_state.lease_until_microseconds =
          request.now_microseconds + request.lease_duration_microseconds;
      result.preconditions_satisfied = true;
      break;
    case AgentClusterLeaseSurface::failover:
      if (lease_state.state == AgentClusterLeadershipState::quarantined ||
          request.instance_uuid.empty() ||
          request.lease_duration_microseconds == 0) {
        MarkPreconditionFailure(&result, lease_state.state == AgentClusterLeadershipState::quarantined
                                             ? kAgentClusterSplitBrainRefusedCode
                                             : "SB_AGENT_CLUSTER_LEASE.PRECONDITION_FAILED",
                                "failover_requires_nonquarantined_candidate_and_duration");
        break;
      }
      if (lease_state.state == AgentClusterLeadershipState::leader_active &&
          request.now_microseconds < lease_state.lease_until_microseconds) {
        MarkPreconditionFailure(&result, kAgentClusterLeaderExistsCode,
                                "failover_requires_expired_or_draining_leader");
        break;
      }
      lease_state.state = AgentClusterLeadershipState::leader_active;
      lease_state.owner_instance_uuid = request.instance_uuid;
      lease_state.epoch += 1;
      lease_state.fence_token = DeterministicFenceToken(request.instance_uuid, lease_state.epoch);
      lease_state.lease_until_microseconds =
          request.now_microseconds + request.lease_duration_microseconds;
      result.preconditions_satisfied = true;
      break;
    case AgentClusterLeaseSurface::authorize_action:
      if (request.destructive_or_control_action &&
          lease_state.state == AgentClusterLeadershipState::leader_active &&
          request.instance_uuid != lease_state.owner_instance_uuid &&
          request.epoch == lease_state.epoch) {
        lease_state.state = AgentClusterLeadershipState::quarantined;
        MarkPreconditionFailure(&result, kAgentClusterSplitBrainRefusedCode,
                                "same_epoch_competing_leader_refused");
        break;
      }
      if (request.destructive_or_control_action &&
          lease_state.state == AgentClusterLeadershipState::leader_active &&
          request.now_microseconds >= lease_state.lease_until_microseconds) {
        lease_state.state = AgentClusterLeadershipState::lease_expired;
        MarkPreconditionFailure(&result, kAgentClusterLeaseRequiredCode,
                                "destructive_or_control_action_requires_unexpired_lease");
        break;
      }
      if (request.destructive_or_control_action && !LeaseCurrent(lease_state, request)) {
        MarkPreconditionFailure(&result, kAgentClusterFenceStaleCode,
                                "destructive_or_control_action_requires_current_lease_epoch_and_fence");
        break;
      }
      result.preconditions_satisfied = true;
      result.action_authorized = true;
      break;
  }

  AddLeaseEvidence(&result, lease_state);
  return result;
}

}  // namespace scratchbird::core::agents
