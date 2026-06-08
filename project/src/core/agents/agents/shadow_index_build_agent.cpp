// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// DPC_SHADOW_INDEX_BUILD_LIFECYCLE
#include "agents/shadow_index_build_agent.hpp"

#include <utility>

namespace scratchbird::core::agents::implemented_agents {
namespace {

namespace idx = scratchbird::core::index;

using scratchbird::core::platform::DiagnosticArgument;
using scratchbird::core::platform::MakeDiagnostic;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;

Status OkStatus() {
  return {StatusCode::ok, Severity::info, Subsystem::engine};
}

Status RefuseStatus() {
  return {StatusCode::platform_required_feature_missing,
          Severity::error,
          Subsystem::engine};
}

std::string BoolText(bool value) {
  return value ? "true" : "false";
}

void AddEvidence(ShadowIndexBuildAgentResult* result,
                 std::string key,
                 std::string value) {
  result->evidence.push_back({std::move(key), std::move(value)});
}

ShadowIndexBuildAgentResult Finish(ShadowIndexBuildAgentResult result,
                                   std::string diagnostic_code,
                                   std::string message_key,
                                   std::string detail,
                                   bool fail_closed) {
  result.status = fail_closed ? RefuseStatus() : OkStatus();
  result.fail_closed = fail_closed;
  result.diagnostic = MakeShadowIndexBuildAgentDiagnostic(
      result.status,
      std::move(diagnostic_code),
      std::move(message_key),
      std::move(detail));
  AddEvidence(&result,
              "shadow_index_build_agent",
              "dpc040_shadow_index_build_lifecycle_v1");
  AddEvidence(&result, "fail_closed", BoolText(fail_closed));
  AddEvidence(&result,
              "lifecycle_state",
              idx::ShadowIndexBuildStateName(result.lifecycle.record.state));
  AddEvidence(&result,
              "planner_visible",
              BoolText(result.lifecycle.record.planner_visible));
  AddEvidence(&result,
              "read_visible",
              BoolText(result.lifecycle.record.read_visible));
  AddEvidence(&result,
              "validation_evidence_present",
              BoolText(result.lifecycle.record.validation_evidence_present));
  AddEvidence(&result,
              "publish_barrier_evidence_present",
              BoolText(result.lifecycle.record.publish_barrier_evidence_present));
  AddEvidence(&result, "authority_source", "durable_mga_transaction_inventory");
  AddEvidence(&result, "parser_finality_authority", "false");
  AddEvidence(&result, "client_state_authority", "false");
  AddEvidence(&result, "timestamp_ordering_authority", "false");
  AddEvidence(&result, "uuid_ordering_authority", "false");
  AddEvidence(&result, "event_stream_authority", "false");
  AddEvidence(&result, "diagnostic_code", result.diagnostic.diagnostic_code);
  return result;
}

}  // namespace

ShadowIndexBuildAgentResult PublishShadowIndexBuildAgentStep(
    ShadowIndexBuildLedger* ledger,
    ShadowIndexBuildRecord* record,
    const ShadowIndexBuildAgentPublishRequest& request) {
  ShadowIndexBuildAgentResult result;
  if (!request.engine_mga_authoritative || request.agent_evidence_ref.empty()) {
    result.lifecycle = idx::RefuseShadowIndexBuild(
        ledger,
        record,
        "shadow_index_agent_non_authoritative_refusal",
        "shadow publish agent requires engine MGA authority evidence");
    return Finish(std::move(result),
                  "shadow_index_agent_non_authoritative_refusal",
                  "agents.shadow_index_build.non_authoritative_refusal",
                  "agent publish requires engine MGA authority evidence",
                  true);
  }

  result.lifecycle = idx::PublishShadowIndexBuild(ledger, record);
  return Finish(std::move(result),
                result.lifecycle.diagnostic.diagnostic_code,
                result.lifecycle.diagnostic.message_key,
                request.agent_evidence_ref,
                result.lifecycle.fail_closed);
}

DiagnosticRecord MakeShadowIndexBuildAgentDiagnostic(Status status,
                                                     std::string diagnostic_code,
                                                     std::string message_key,
                                                     std::string detail) {
  std::vector<DiagnosticArgument> arguments;
  if (!detail.empty()) {
    arguments.push_back({"detail", std::move(detail)});
  }
  return MakeDiagnostic(status.code,
                        status.severity,
                        status.subsystem,
                        std::move(diagnostic_code),
                        std::move(message_key),
                        std::move(arguments),
                        {},
                        "core.agents.shadow_index_build");
}

// SEARCH_KEY: SB_AGENT_IMPLEMENTATION_shadow_index_build_agent
// DPC_SHADOW_INDEX_BUILD_LIFECYCLE
const char* shadow_index_build_agent_implementation_anchor() {
  return "shadow_index_build_agent";
}

}  // namespace scratchbird::core::agents::implemented_agents
