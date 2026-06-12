// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "agents/memory_governor.hpp"

// CanonicalAgentRegistry/CanonicalAgentManifest owns production exposure;
// this file provides the local memory-governor handler.

#include <algorithm>
#include <utility>

namespace scratchbird::core::agents::implemented_agents {
namespace {

using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;

Status OkStatus() {
  return {StatusCode::ok, Severity::info, Subsystem::engine};
}

Status ErrorStatus() {
  return {StatusCode::memory_invalid_request, Severity::error, Subsystem::memory};
}

void AddEvidence(MemoryGovernorResult* result,
                 std::string key,
                 std::string value) {
  result->evidence.push_back({std::move(key), std::move(value)});
}

MemoryGovernorResult Finish(MemoryGovernorDecisionKind decision,
                            Status status,
                            std::string code,
                            std::string key,
                            std::string detail,
                            bool fail_closed) {
  MemoryGovernorResult result;
  result.status = status;
  result.decision = decision;
  result.fail_closed = fail_closed;
  result.grant_allowed = decision == MemoryGovernorDecisionKind::allow_grant;
  result.spill_required = decision == MemoryGovernorDecisionKind::force_spill;
  result.cache_shrink_requested =
      decision == MemoryGovernorDecisionKind::shrink_cache;
  result.diagnostic = MakeMemoryGovernorDiagnostic(result.status,
                                                   std::move(code),
                                                   std::move(key),
                                                   std::move(detail));
  AddEvidence(&result, "decision", MemoryGovernorDecisionKindName(result.decision));
  AddEvidence(&result, "failed_closed", fail_closed ? "true" : "false");
  return result;
}

u64 SaturatingAdd(u64 left, u64 right) {
  const u64 sum = left + right;
  return sum < left ? ~u64{0} : sum;
}

}  // namespace

const char* MemoryGovernorDecisionKindName(MemoryGovernorDecisionKind decision) {
  switch (decision) {
    case MemoryGovernorDecisionKind::allow_grant: return "allow_grant";
    case MemoryGovernorDecisionKind::deny_large_grant: return "deny_large_grant";
    case MemoryGovernorDecisionKind::force_spill: return "force_spill";
    case MemoryGovernorDecisionKind::shrink_cache: return "shrink_cache";
    case MemoryGovernorDecisionKind::refused: return "refused";
  }
  return "refused";
}

DiagnosticRecord MakeMemoryGovernorDiagnostic(Status status,
                                              std::string diagnostic_code,
                                              std::string message_key,
                                              std::string detail) {
  return scratchbird::core::platform::MakeDiagnostic(
      status.code,
      status.severity,
      status.subsystem,
      std::move(diagnostic_code),
      std::move(message_key),
      {{"detail", std::move(detail)}},
      {},
      "memory_governor",
      {});
}

MemoryGovernorResult EvaluateMemoryGovernorGrant(
    const MemoryGovernorSnapshot& snapshot,
    const MemoryGovernorPolicy& policy) {
  if (!policy.present || !policy.valid || !policy.scope_compatible) {
    return Finish(MemoryGovernorDecisionKind::refused, ErrorStatus(),
                  "SB_AGENT_MEMORY_GOVERNOR_POLICY_INVALID",
                  "agents.memory_governor.policy_invalid",
                  "policy missing invalid or outside scope",
                  true);
  }
  if (policy.hard_limit_bytes == 0 || policy.soft_limit_bytes == 0 ||
      policy.soft_limit_bytes > policy.hard_limit_bytes) {
    return Finish(MemoryGovernorDecisionKind::refused, ErrorStatus(),
                  "SB_AGENT_MEMORY_GOVERNOR_LIMITS_INVALID",
                  "agents.memory_governor.limits_invalid",
                  "hard and soft memory limits must be configured",
                  true);
  }
  if (!snapshot.memory_metrics_authoritative ||
      !snapshot.resource_reservation_authoritative ||
      snapshot.parser_authority || snapshot.reference_authority) {
    return Finish(MemoryGovernorDecisionKind::refused, ErrorStatus(),
                  "SB_AGENT_MEMORY_GOVERNOR_AUTHORITY_UNTRUSTED",
                  "agents.memory_governor.untrusted_authority",
                  "memory decisions require trusted metrics and reservations",
                  true);
  }

  const u64 projected =
      SaturatingAdd(snapshot.current_bytes, snapshot.requested_grant_bytes);
  if (projected > policy.hard_limit_bytes &&
      policy.deny_large_grants_allowed) {
    auto result = Finish(MemoryGovernorDecisionKind::deny_large_grant,
                         OkStatus(),
                         "SB_AGENT_MEMORY_GOVERNOR_GRANT_DENIED",
                         "agents.memory_governor.grant_denied",
                         "projected allocation exceeds hard limit",
                         false);
    AddEvidence(&result, "projected_bytes", std::to_string(projected));
    AddEvidence(&result, "hard_limit_bytes",
                std::to_string(policy.hard_limit_bytes));
    return result;
  }
  if (projected > policy.soft_limit_bytes && snapshot.grant_is_spillable &&
      policy.force_spill_allowed) {
    auto result = Finish(MemoryGovernorDecisionKind::force_spill,
                         OkStatus(),
                         "SB_AGENT_MEMORY_GOVERNOR_SPILL_REQUIRED",
                         "agents.memory_governor.force_spill",
                         "projected allocation exceeds soft limit and is spillable",
                         false);
    result.bytes_to_spill = std::min(snapshot.spillable_bytes,
                                     projected - policy.soft_limit_bytes);
    AddEvidence(&result, "bytes_to_spill", std::to_string(result.bytes_to_spill));
    return result;
  }
  if (projected > policy.soft_limit_bytes && snapshot.cache_bytes >
      policy.cache_shrink_floor_bytes && policy.shrink_cache_allowed) {
    auto result = Finish(MemoryGovernorDecisionKind::shrink_cache,
                         OkStatus(),
                         "SB_AGENT_MEMORY_GOVERNOR_CACHE_SHRINK",
                         "agents.memory_governor.shrink_cache",
                         "cache shrink can restore soft-limit headroom",
                         false);
    result.bytes_to_shrink = std::min(snapshot.cache_bytes -
                                          policy.cache_shrink_floor_bytes,
                                      projected - policy.soft_limit_bytes);
    AddEvidence(&result, "bytes_to_shrink",
                std::to_string(result.bytes_to_shrink));
    return result;
  }
  auto result = Finish(MemoryGovernorDecisionKind::allow_grant,
                       OkStatus(),
                       "SB_AGENT_MEMORY_GOVERNOR_GRANT_ALLOWED",
                       "agents.memory_governor.grant_allowed",
                       "projected allocation remains within governed limits",
                       false);
  AddEvidence(&result, "projected_bytes", std::to_string(projected));
  AddEvidence(&result, "active_query_count",
              std::to_string(snapshot.active_query_count));
  return result;
}

const char* memory_governor_implementation_anchor() {
  return "memory_governor";
}

}  // namespace scratchbird::core::agents::implemented_agents
