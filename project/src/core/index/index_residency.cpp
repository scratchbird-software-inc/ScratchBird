// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "index_residency.hpp"

#include <algorithm>
#include <utility>

namespace scratchbird::core::index {
namespace {
using scratchbird::core::platform::DiagnosticArgument;
using scratchbird::core::platform::MakeDiagnostic;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;

Status OkStatus() { return {StatusCode::ok, Severity::info, Subsystem::memory}; }
Status ErrorStatus() { return {StatusCode::memory_limit_exceeded, Severity::warning, Subsystem::memory}; }
}  // namespace

IndexResidencyDecision PlanIndexResidency(const IndexResidencyRequest& request) {
  IndexResidencyDecision decision;
  if (!request.index_uuid.valid() || request.family == IndexFamily::unknown || request.estimated_hot_bytes == 0) {
    decision.status = ErrorStatus();
    decision.action = IndexResidencyAction::refuse;
    decision.diagnostic = MakeIndexResidencyDiagnostic(decision.status,
                                                       "SB-INDEX-RESIDENCY-INVALID-REQUEST",
                                                       "index.residency.invalid_request");
    return decision;
  }
  if (request.target == IndexResidencyTarget::pinned && !request.policy_allows_pin) {
    decision.status = ErrorStatus();
    decision.action = IndexResidencyAction::degrade;
    decision.granted_target = IndexResidencyTarget::hot;
    decision.admitted_bytes = std::min(request.estimated_hot_bytes, request.memory_budget_bytes);
    decision.diagnostic = MakeIndexResidencyDiagnostic(decision.status,
                                                       "SB-INDEX-RESIDENCY-PIN-DEGRADED",
                                                       "index.residency.pin_degraded");
    return decision;
  }
  if (request.memory_budget_bytes >= request.estimated_hot_bytes && request.current_pressure_score < 80) {
    decision.status = OkStatus();
    decision.action = IndexResidencyAction::grant;
    decision.granted_target = request.target;
    decision.admitted_bytes = request.estimated_hot_bytes;
    return decision;
  }
  if (request.cold_start_image_available && request.current_pressure_score < 95) {
    decision.status = OkStatus();
    decision.action = IndexResidencyAction::degrade;
    decision.granted_target = IndexResidencyTarget::warm;
    decision.admitted_bytes = std::min(request.estimated_hot_bytes, request.memory_budget_bytes);
    return decision;
  }
  decision.status = ErrorStatus();
  decision.action = request.cold_start_image_available ? IndexResidencyAction::evict : IndexResidencyAction::refuse;
  decision.granted_target = IndexResidencyTarget::cold;
  decision.diagnostic = MakeIndexResidencyDiagnostic(decision.status,
                                                     "SB-INDEX-RESIDENCY-BUDGET-REFUSED",
                                                     "index.residency.budget_refused");
  return decision;
}

DiagnosticRecord MakeIndexResidencyDiagnostic(Status status,
                                              std::string diagnostic_code,
                                              std::string message_key,
                                              std::string detail) {
  std::vector<DiagnosticArgument> arguments;
  if (!detail.empty()) {
    arguments.push_back({"detail", std::move(detail)});
  }
  return MakeDiagnostic(status.code, status.severity, status.subsystem,
                        std::move(diagnostic_code), std::move(message_key),
                        std::move(arguments), {}, "core.index.residency");
}

}  // namespace scratchbird::core::index
