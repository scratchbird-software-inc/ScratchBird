// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SPDX-License-Identifier: Apache-2.0
// Copyright (c) ScratchBird contributors
//
// PUBLIC_CLUSTER_PROJECTION_AUTHORITY_GUARD_GATE

#include "agent_cluster_projection_guard.hpp"
#include "cluster_projection_authority_guard.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace agents = scratchbird::core::agents;
namespace optimizer = scratchbird::engine::optimizer;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) { Fail(message); }
}

bool HasEvidence(const std::vector<std::string>& evidence,
                 std::string_view expected) {
  return std::find(evidence.begin(), evidence.end(), expected) !=
         evidence.end();
}

std::vector<optimizer::OptimizerClusterProjectionAuthoritySource>
OptimizerSources() {
  return {optimizer::OptimizerClusterProjectionAuthoritySource::
              projection_cache,
          optimizer::OptimizerClusterProjectionAuthoritySource::route_cache,
          optimizer::OptimizerClusterProjectionAuthoritySource::metric,
          optimizer::OptimizerClusterProjectionAuthoritySource::
              agent_recommendation};
}

std::vector<agents::AgentClusterProjectionGuardSource> AgentSources() {
  return {agents::AgentClusterProjectionGuardSource::projection_cache,
          agents::AgentClusterProjectionGuardSource::route_cache,
          agents::AgentClusterProjectionGuardSource::metric,
          agents::AgentClusterProjectionGuardSource::
              optimizer_recommendation};
}

optimizer::OptimizerClusterProjectionAuthorityRequest OptimizerRequest(
    optimizer::OptimizerClusterProjectionAuthoritySource source) {
  optimizer::OptimizerClusterProjectionAuthorityRequest request;
  request.source = source;
  request.artifact_id = "optimizer-artifact:" +
                        std::string(
                            optimizer::OptimizerClusterProjectionAuthoritySourceName(
                                source));
  request.projection_digest =
      "sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
  request.local_projection_present = true;
  return request;
}

agents::AgentClusterProjectionGuardRequest AgentRequest(
    agents::AgentClusterProjectionGuardSource source) {
  agents::AgentClusterProjectionGuardRequest request;
  request.source = source;
  request.artifact_id = "agent-artifact:" +
                        std::string(
                            agents::AgentClusterProjectionGuardSourceName(
                                source));
  request.projection_digest =
      "sha256:fedcba9876543210fedcba9876543210fedcba9876543210fedcba9876543210";
  request.local_projection_present = true;
  return request;
}

void AddProviderProof(
    optimizer::OptimizerClusterProjectionAuthorityRequest* request) {
  request->external_provider_authority_digest =
      "sha256:provider-authority-proof-optimizer";
  request->external_provider_authority_proof_present = true;
  request->external_provider_authority_digest_verified = true;
}

void AddProviderProof(agents::AgentClusterProjectionGuardRequest* request) {
  request->external_provider_authority_digest =
      "sha256:provider-authority-proof-agent";
  request->external_provider_authority_proof_present = true;
  request->external_provider_authority_digest_verified = true;
}

void TestOptimizerProjectionArtifactsRemainEvidenceOnly() {
  for (const auto source : OptimizerSources()) {
    const auto result =
        optimizer::EvaluateOptimizerClusterProjectionAuthorityGuard(
            OptimizerRequest(source));
    Require(result.ok, "optimizer evidence-only projection was refused");
    Require(!result.fail_closed,
            "optimizer evidence-only projection failed closed");
    Require(result.evidence_only,
            "optimizer projection was not evidence-only");
    Require(!result.local_projection_authority,
            "optimizer local projection claimed authority");
    Require(!result.cluster_authority_granted,
            "optimizer evidence-only projection gained cluster authority");
    Require(!result.optimizer_plan_authority_granted,
            "optimizer evidence-only projection gained plan authority");
    Require(HasEvidence(result.evidence, "CLUSTER_PROJECTION_AUTHORITY_GUARD"),
            "optimizer authority guard evidence key missing");
  }
}

void TestOptimizerAuthorityPromotionNeedsProviderProof() {
  for (const auto source : OptimizerSources()) {
    auto request = OptimizerRequest(source);
    request.wants_cluster_authority = true;
    request.wants_optimizer_plan_authority = true;
    auto result =
        optimizer::EvaluateOptimizerClusterProjectionAuthorityGuard(request);
    Require(!result.ok && result.fail_closed,
            "optimizer authority promotion without provider proof did not fail");
    Require(result.evidence_only,
            "optimizer failed promotion stopped being evidence-only");
    Require(!result.cluster_authority_granted,
            "optimizer promotion granted cluster authority without proof");
    Require(result.diagnostic_code ==
                "SB_OPTIMIZER_CLUSTER_PROJECTION_AUTHORITY.EXTERNAL_PROVIDER_REQUIRED",
            "optimizer missing-provider diagnostic changed");

    AddProviderProof(&request);
    result = optimizer::EvaluateOptimizerClusterProjectionAuthorityGuard(
        request);
    Require(result.ok, "optimizer provider-delegated projection was refused");
    Require(!result.fail_closed,
            "optimizer provider-delegated projection failed closed");
    Require(!result.evidence_only,
            "optimizer provider-delegated projection stayed evidence-only");
    Require(!result.local_projection_authority,
            "optimizer provider proof promoted local projection authority");
    Require(result.cluster_authority_granted,
            "optimizer provider proof did not grant cluster authority");
    Require(result.optimizer_plan_authority_granted,
            "optimizer provider proof did not grant plan authority");
    Require(result.external_provider_authority_proof_used,
            "optimizer provider proof was not recorded as used");
  }
}

void TestAgentProjectionArtifactsRemainEvidenceOnly() {
  for (const auto source : AgentSources()) {
    const auto result =
        agents::EvaluateAgentClusterProjectionGuard(AgentRequest(source));
    Require(result.ok, "agent evidence-only projection was refused");
    Require(!result.fail_closed,
            "agent evidence-only projection failed closed");
    Require(result.evidence_only, "agent projection was not evidence-only");
    Require(!result.local_projection_authority,
            "agent local projection claimed authority");
    Require(!result.cluster_authority_granted,
            "agent evidence-only projection gained cluster authority");
    Require(!result.agent_action_authority_granted,
            "agent evidence-only projection gained action authority");
    Require(HasEvidence(result.evidence, "CLUSTER_AGENT_PROJECTION_GUARD"),
            "agent authority guard evidence key missing");
  }
}

void TestAgentAuthorityPromotionNeedsProviderProof() {
  for (const auto source : AgentSources()) {
    auto request = AgentRequest(source);
    request.wants_cluster_authority = true;
    request.wants_agent_action_authority = true;
    auto result = agents::EvaluateAgentClusterProjectionGuard(request);
    Require(!result.ok && result.fail_closed,
            "agent authority promotion without provider proof did not fail");
    Require(result.evidence_only,
            "agent failed promotion stopped being evidence-only");
    Require(!result.cluster_authority_granted,
            "agent promotion granted cluster authority without proof");
    Require(result.diagnostic_code ==
                "SB_AGENT_CLUSTER_PROJECTION_GUARD.EXTERNAL_PROVIDER_REQUIRED",
            "agent missing-provider diagnostic changed");

    AddProviderProof(&request);
    result = agents::EvaluateAgentClusterProjectionGuard(request);
    Require(result.ok, "agent provider-delegated projection was refused");
    Require(!result.fail_closed,
            "agent provider-delegated projection failed closed");
    Require(!result.evidence_only,
            "agent provider-delegated projection stayed evidence-only");
    Require(!result.local_projection_authority,
            "agent provider proof promoted local projection authority");
    Require(result.cluster_authority_granted,
            "agent provider proof did not grant cluster authority");
    Require(result.agent_action_authority_granted,
            "agent provider proof did not grant action authority");
    Require(result.external_provider_authority_proof_used,
            "agent provider proof was not recorded as used");
  }
}

}  // namespace

int main() {
  TestOptimizerProjectionArtifactsRemainEvidenceOnly();
  TestOptimizerAuthorityPromotionNeedsProviderProof();
  TestAgentProjectionArtifactsRemainEvidenceOnly();
  TestAgentAuthorityPromotionNeedsProviderProof();
  return EXIT_SUCCESS;
}
