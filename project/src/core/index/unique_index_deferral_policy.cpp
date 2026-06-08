// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// DPC_UNIQUE_INDEX_DEFERRAL_POLICY
#include "unique_index_deferral_policy.hpp"

#include "unique_index_reservation_ledger.hpp"

#include <utility>
#include <vector>

namespace scratchbird::core::index {
namespace {

using scratchbird::core::platform::DiagnosticArgument;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;

Status PolicyOkStatus() {
  return {StatusCode::ok, Severity::info, Subsystem::engine};
}

Status PolicyRefusalStatus() {
  return {StatusCode::platform_required_feature_missing, Severity::error, Subsystem::engine};
}

bool IsUnique(IndexUniquenessClass uniqueness) {
  return uniqueness == IndexUniquenessClass::unique_primary ||
         uniqueness == IndexUniquenessClass::unique_secondary;
}

UniqueIndexDeferralPolicyDecision Accept(const UniqueIndexDeferralPolicyRequest& request,
                                         bool deferred_eligible,
                                         bool reservation_protocol_required,
                                         std::string diagnostic_code,
                                         std::string message_key,
                                         std::string detail) {
  UniqueIndexDeferralPolicyDecision decision;
  decision.status = PolicyOkStatus();
  decision.accepted = true;
  decision.deferred_eligible = deferred_eligible;
  decision.synchronous_required = !deferred_eligible;
  decision.reservation_protocol_required = reservation_protocol_required;
  decision.diagnostic = MakeUniqueIndexDeferralPolicyDiagnostic(
      decision.status,
      std::move(diagnostic_code),
      std::move(message_key),
      std::move(detail));
  decision.diagnostic.arguments.push_back(
      {"uniqueness", IndexUniquenessClassName(request.uniqueness)});
  decision.diagnostic.arguments.push_back(
      {"request_kind", IndexDeferralRequestKindName(request.request_kind)});
  return decision;
}

UniqueIndexDeferralPolicyDecision Refuse(const UniqueIndexDeferralPolicyRequest& request,
                                         bool reservation_protocol_required,
                                         std::string diagnostic_code,
                                         std::string message_key,
                                         std::string detail) {
  UniqueIndexDeferralPolicyDecision decision;
  decision.status = PolicyRefusalStatus();
  decision.accepted = false;
  decision.deferred_eligible = false;
  decision.synchronous_required = true;
  decision.reservation_protocol_required = reservation_protocol_required;
  decision.diagnostic = MakeUniqueIndexDeferralPolicyDiagnostic(
      decision.status,
      std::move(diagnostic_code),
      std::move(message_key),
      std::move(detail));
  decision.diagnostic.arguments.push_back(
      {"uniqueness", IndexUniquenessClassName(request.uniqueness)});
  decision.diagnostic.arguments.push_back(
      {"request_kind", IndexDeferralRequestKindName(request.request_kind)});
  return decision;
}

}  // namespace

const char* IndexUniquenessClassName(IndexUniquenessClass uniqueness) {
  switch (uniqueness) {
    case IndexUniquenessClass::non_unique_secondary:
      return "non_unique_secondary";
    case IndexUniquenessClass::unique_primary:
      return "unique_primary";
    case IndexUniquenessClass::unique_secondary:
      return "unique_secondary";
  }
  return "unknown";
}

const char* IndexDeferralRequestKindName(IndexDeferralRequestKind request_kind) {
  switch (request_kind) {
    case IndexDeferralRequestKind::synchronous_immediate:
      return "synchronous_immediate";
    case IndexDeferralRequestKind::generic_deferred:
      return "generic_deferred";
    case IndexDeferralRequestKind::unique_deferred_with_reservation:
      return "unique_deferred_with_reservation";
  }
  return "unknown";
}

UniqueIndexDeferralPolicyDecision EvaluateUniqueIndexDeferralPolicy(
    const UniqueIndexDeferralPolicyRequest& request) {
  if (request.request_kind == IndexDeferralRequestKind::synchronous_immediate) {
    if (IsUnique(request.uniqueness)) {
      return Accept(request,
                    false,
                    false,
                    "INDEX.UNIQUE_DEFERRAL.SYNCHRONOUS_REQUIRED",
                    "core.index.deferral.unique_synchronous_required",
                    "unique index maintenance is synchronous by default");
    }
    return Accept(request,
                  false,
                  false,
                  "INDEX.DEFERRAL.SYNCHRONOUS_REQUIRED",
                  "core.index.deferral.synchronous_required",
                  "synchronous index maintenance requested");
  }

  if (!IsUnique(request.uniqueness)) {
    if (request.non_unique_deferral_policy_enabled) {
      return Accept(request,
                    true,
                    false,
                    "INDEX.DEFERRAL.NON_UNIQUE_ELIGIBLE",
                    "core.index.deferral.non_unique_eligible",
                    "non-unique secondary index is eligible for deferred maintenance");
    }
    return Accept(request,
                  false,
                  false,
                  "INDEX.DEFERRAL.NON_UNIQUE_POLICY_DISABLED",
                  "core.index.deferral.non_unique_policy_disabled",
                  "non-unique deferred maintenance policy is disabled");
  }

  if (request.request_kind == IndexDeferralRequestKind::generic_deferred) {
    return Refuse(request,
                  true,
                  "INDEX.UNIQUE_DEFERRAL.FORBIDDEN",
                  "core.index.deferral.unique_forbidden",
                  "generic deferred-index requests cannot defer unique indexes");
  }

  if (!request.reservation_protocol_present || !request.reservation_protocol_enabled) {
    return Refuse(request,
                  true,
                  "INDEX.UNIQUE_DEFERRAL.RESERVATION_REQUIRED",
                  "core.index.deferral.unique_reservation_required",
                  "unique deferral requires an explicitly enabled reservation protocol");
  }

  if (!request.reservation_protocol_proven) {
    return Refuse(request,
                  true,
                  "INDEX.UNIQUE_DEFERRAL.RESERVATION_UNPROVEN",
                  "core.index.deferral.unique_reservation_unproven",
                  "unique reservation protocol must be separately proven before deferral");
  }

  if (request.reservation_protocol_gate_token !=
      kUniqueIndexReservationProtocolGateToken) {
    return Refuse(request,
                  true,
                  "INDEX.UNIQUE_DEFERRAL.RESERVATION_UNPROVEN",
                  "core.index.deferral.unique_reservation_unproven",
                  "unique reservation protocol proof must come from the IRC-021 reservation ledger gate");
  }

  return Accept(request,
                true,
                true,
                "INDEX.UNIQUE_DEFERRAL.RESERVATION_ACCEPTED",
                "core.index.deferral.unique_reservation_accepted",
                "unique deferral admitted by the proven IRC-021 reservation ledger protocol gate");
}

DiagnosticRecord MakeUniqueIndexDeferralPolicyDiagnostic(Status status,
                                                         std::string diagnostic_code,
                                                         std::string message_key,
                                                         std::string detail) {
  std::vector<DiagnosticArgument> arguments;
  if (!detail.empty()) {
    arguments.push_back({"detail", std::move(detail)});
  }
  return scratchbird::core::platform::MakeDiagnostic(
      status.code,
      status.severity,
      status.subsystem,
      std::move(diagnostic_code),
      std::move(message_key),
      std::move(arguments),
      {},
      "core.index.unique_index_deferral_policy",
      status.ok() ? "" : "keep unique index maintenance synchronous until a proven reservation protocol is enabled");
}

}  // namespace scratchbird::core::index
