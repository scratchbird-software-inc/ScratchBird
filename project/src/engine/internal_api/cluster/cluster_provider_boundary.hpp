// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "api_types.hpp"
#include "cluster_provider/cluster_provider.hpp"

#include <string>
#include <string_view>

namespace scratchbird::engine::internal_api {

// SEARCH_KEY: SB_ENGINE_INTERNAL_API_CLUSTER_PROVIDER_BOUNDARY_HELPER
inline cluster_provider::ClusterProviderRequest MakeInternalClusterProviderRequest(
    const EngineRequestContext& context,
    std::string operation_id,
    std::string opcode,
    std::string trace_key) {
  cluster_provider::ClusterProviderRequest provider_request;
  provider_request.context = context;
  provider_request.envelope.operation_id = std::move(operation_id);
  provider_request.envelope.opcode = std::move(opcode);
  provider_request.envelope.trace_key = std::move(trace_key);
  provider_request.envelope.requires_security_context = true;
  provider_request.envelope.requires_cluster_authority = true;
  provider_request.envelope.contains_sql_text = false;
  provider_request.api_request.context = context;
  provider_request.api_request.operation_id = provider_request.envelope.operation_id;
  return provider_request;
}

inline EngineApiResult ExecuteInternalClusterProviderBoundary(
    const EngineRequestContext& context,
    std::string_view operation_id,
    std::string_view opcode,
    std::string_view trace_key) {
  return cluster_provider::ExecuteClusterOperation(
      MakeInternalClusterProviderRequest(context,
                                         std::string(operation_id),
                                         std::string(opcode),
                                         std::string(trace_key)));
}

inline void CopyClusterProviderBoundaryProof(EngineApiResult* result,
                                             const EngineApiResult& provider_result,
                                             std::string_view operation_id) {
  if (result == nullptr) {
    return;
  }
  result->diagnostics.insert(result->diagnostics.end(),
                             provider_result.diagnostics.begin(),
                             provider_result.diagnostics.end());
  result->unsupported_features.insert(result->unsupported_features.end(),
                                      provider_result.unsupported_features.begin(),
                                      provider_result.unsupported_features.end());
  result->evidence.insert(result->evidence.end(),
                          provider_result.evidence.begin(),
                          provider_result.evidence.end());
  result->evidence.push_back({"cluster_provider_boundary", "provider_invoked"});
  result->evidence.push_back({
      "cluster_provider_boundary_operation",
      provider_result.operation_id.empty() ? std::string(operation_id)
                                           : provider_result.operation_id});
  result->evidence.push_back({"cluster_provider_boundary_api_operation",
                              std::string(operation_id)});
  result->evidence.push_back({"cluster_provider_boundary_result",
                              provider_result.ok ? "admitted" : "failed_closed"});
  result->cluster_authority_required =
      result->cluster_authority_required || provider_result.cluster_authority_required;
  result->embedded_trust_mode_observed =
      result->embedded_trust_mode_observed ||
      provider_result.embedded_trust_mode_observed;
}

inline bool RefuseIfClusterProviderBoundaryClosed(
    EngineApiResult* result,
    const EngineApiResult& provider_result,
    std::string_view operation_id,
    std::string_view refusal_reason) {
  if (provider_result.ok) {
    CopyClusterProviderBoundaryProof(result, provider_result, operation_id);
    return false;
  }
  if (result != nullptr) {
    result->ok = false;
    result->operation_id = std::string(operation_id);
    result->cluster_authority_required = true;
    CopyClusterProviderBoundaryProof(result, provider_result, operation_id);
    result->evidence.push_back({"cluster_route_refused_before_local_execution",
                                std::string(refusal_reason)});
  }
  return true;
}

}  // namespace scratchbird::engine::internal_api
