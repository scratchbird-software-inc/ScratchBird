// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "cluster_projection_authority_guard.hpp"

#include <utility>

namespace scratchbird::engine::optimizer {
namespace {

std::string BoolText(bool value) {
  return value ? "true" : "false";
}

void Add(OptimizerClusterProjectionAuthorityResult* result,
         std::string evidence) {
  if (result != nullptr) { result->evidence.push_back(std::move(evidence)); }
}

bool AuthorityRequested(
    const OptimizerClusterProjectionAuthorityRequest& request) {
  return request.wants_cluster_authority ||
         request.wants_optimizer_plan_authority;
}

bool ExternalProviderProofValid(
    const OptimizerClusterProjectionAuthorityRequest& request) {
  return request.external_provider_authority_proof_present &&
         request.external_provider_authority_digest_verified &&
         !request.external_provider_authority_digest.empty();
}

OptimizerClusterProjectionAuthorityResult Finish(
    const OptimizerClusterProjectionAuthorityRequest& request,
    bool ok,
    bool fail_closed,
    bool evidence_only,
    bool cluster_authority_granted,
    bool optimizer_plan_authority_granted,
    std::string diagnostic_code,
    std::string detail) {
  OptimizerClusterProjectionAuthorityResult result;
  result.ok = ok;
  result.fail_closed = fail_closed;
  result.evidence_only = evidence_only;
  result.local_projection_authority = false;
  result.cluster_authority_granted = cluster_authority_granted;
  result.optimizer_plan_authority_granted = optimizer_plan_authority_granted;
  result.external_provider_authority_proof_used =
      cluster_authority_granted || optimizer_plan_authority_granted;
  result.source =
      OptimizerClusterProjectionAuthoritySourceName(request.source);
  result.diagnostic_code = std::move(diagnostic_code);
  result.detail = std::move(detail);
  Add(&result, "CLUSTER_PROJECTION_AUTHORITY_GUARD");
  Add(&result, "optimizer.cluster_projection_guard.source=" + result.source);
  Add(&result, "optimizer.cluster_projection_guard.artifact_id=" +
                   request.artifact_id);
  Add(&result, "optimizer.cluster_projection_guard.projection_digest_present=" +
                   BoolText(!request.projection_digest.empty()));
  Add(&result, "optimizer.cluster_projection_guard.local_projection_present=" +
                   BoolText(request.local_projection_present));
  Add(&result, "optimizer.cluster_projection_guard.evidence_only=" +
                   BoolText(result.evidence_only));
  Add(&result, "optimizer.cluster_projection_guard.fail_closed=" +
                   BoolText(result.fail_closed));
  Add(&result, "optimizer.cluster_projection_guard.local_projection_authority=false");
  Add(&result, "optimizer.cluster_projection_guard.cluster_authority_granted=" +
                   BoolText(result.cluster_authority_granted));
  Add(&result,
      "optimizer.cluster_projection_guard.optimizer_plan_authority_granted=" +
          BoolText(result.optimizer_plan_authority_granted));
  Add(&result,
      "optimizer.cluster_projection_guard.external_provider_proof_present=" +
          BoolText(request.external_provider_authority_proof_present));
  Add(&result,
      "optimizer.cluster_projection_guard.external_provider_digest_verified=" +
          BoolText(request.external_provider_authority_digest_verified));
  Add(&result, "optimizer.cluster_projection_guard.diagnostic_code=" +
                   result.diagnostic_code);
  return result;
}

}  // namespace

const char* OptimizerClusterProjectionAuthoritySourceName(
    OptimizerClusterProjectionAuthoritySource source) {
  switch (source) {
    case OptimizerClusterProjectionAuthoritySource::projection_cache:
      return "projection_cache";
    case OptimizerClusterProjectionAuthoritySource::route_cache:
      return "route_cache";
    case OptimizerClusterProjectionAuthoritySource::metric:
      return "metric";
    case OptimizerClusterProjectionAuthoritySource::agent_recommendation:
      return "agent_recommendation";
  }
  return "unknown";
}

// SEARCH_KEY: CLUSTER_PROJECTION_AUTHORITY_GUARD
OptimizerClusterProjectionAuthorityResult
EvaluateOptimizerClusterProjectionAuthorityGuard(
    const OptimizerClusterProjectionAuthorityRequest& request) {
  if (request.artifact_id.empty() || request.projection_digest.empty()) {
    return Finish(
        request,
        false,
        true,
        true,
        false,
        false,
        "SB_OPTIMIZER_CLUSTER_PROJECTION_AUTHORITY.IDENTITY_REQUIRED",
        "artifact_id_and_projection_digest_required");
  }
  if (!request.local_projection_present) {
    return Finish(
        request,
        false,
        true,
        true,
        false,
        false,
        "SB_OPTIMIZER_CLUSTER_PROJECTION_AUTHORITY.PROJECTION_REQUIRED",
        "local_projection_evidence_required");
  }
  if (!AuthorityRequested(request)) {
    return Finish(
        request,
        true,
        false,
        true,
        false,
        false,
        "SB_OPTIMIZER_CLUSTER_PROJECTION_AUTHORITY.EVIDENCE_ONLY",
        "local_cluster_projection_remains_optimizer_evidence_only");
  }
  if (!ExternalProviderProofValid(request)) {
    return Finish(
        request,
        false,
        true,
        true,
        false,
        false,
        "SB_OPTIMIZER_CLUSTER_PROJECTION_AUTHORITY.EXTERNAL_PROVIDER_REQUIRED",
        "cluster_projection_authority_requires_external_provider_proof");
  }
  return Finish(
      request,
      true,
      false,
      false,
      request.wants_cluster_authority,
      request.wants_optimizer_plan_authority,
      "SB_OPTIMIZER_CLUSTER_PROJECTION_AUTHORITY.PROVIDER_DELEGATED",
      "cluster_authority_delegated_to_external_provider_proof");
}

}  // namespace scratchbird::engine::optimizer
