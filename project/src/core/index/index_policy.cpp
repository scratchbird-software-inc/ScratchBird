// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "index_policy.hpp"

namespace scratchbird::core::index {
namespace {
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;
Status OkStatus() { return Status{StatusCode::ok, Severity::info, Subsystem::engine}; }
Status DeniedStatus() { return Status{StatusCode::platform_required_feature_missing, Severity::error, Subsystem::engine}; }
bool IsGroup(std::string_view group, std::string_view expected) { return group == expected; }
}

const char* IndexOperationRightName(IndexOperationRight right) {
  switch (right) {
    case IndexOperationRight::create: return "INDEX_CREATE";
    case IndexOperationRight::alter: return "INDEX_ALTER";
    case IndexOperationRight::drop: return "INDEX_DROP";
    case IndexOperationRight::rebuild: return "INDEX_MAINTAIN";
    case IndexOperationRight::verify: return "INDEX_VERIFY";
    case IndexOperationRight::move: return "INDEX_MOVE";
    case IndexOperationRight::inspect: return "OBS_INDEX_PROFILE_READ";
    case IndexOperationRight::read_metrics: return "OBS_METRICS_READ_FAMILY";
    case IndexOperationRight::helper_use: return "INDEX_HELPER_USE";
  }
  return "INDEX_UNKNOWN";
}

IndexPolicyDecision EvaluateIndexPolicy(IndexOperationRight right, std::string_view principal_group,
                                        bool is_owner, bool security_admin_override) {
  bool allowed = security_admin_override || IsGroup(principal_group, "ROOT") || IsGroup(principal_group, "DBA");
  switch (right) {
    case IndexOperationRight::create:
      allowed = allowed || IsGroup(principal_group, "DEV") || is_owner;
      break;
    case IndexOperationRight::alter:
    case IndexOperationRight::drop:
      allowed = allowed || is_owner;
      break;
    case IndexOperationRight::rebuild:
    case IndexOperationRight::verify:
    case IndexOperationRight::move:
      allowed = allowed || IsGroup(principal_group, "OPS");
      break;
    case IndexOperationRight::inspect:
    case IndexOperationRight::read_metrics:
      allowed = allowed || IsGroup(principal_group, "OPS") || IsGroup(principal_group, "SUP") || IsGroup(principal_group, "AUD");
      break;
    case IndexOperationRight::helper_use:
      allowed = allowed || IsGroup(principal_group, "OPS");
      break;
  }
  const Status status = allowed ? OkStatus() : DeniedStatus();
  IndexPolicyDecision decision{status, allowed, IndexOperationRightName(right), {}};
  if (!allowed) {
    decision.diagnostic = MakeIndexFamilyDiagnostic(status, "INDEX.SECURITY_DENIED", "index.security.denied", decision.required_right);
  }
  return decision;
}

}  // namespace scratchbird::core::index
