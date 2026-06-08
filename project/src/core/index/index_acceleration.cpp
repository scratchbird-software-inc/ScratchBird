// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "index_acceleration.hpp"

namespace scratchbird::core::index {
namespace {
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;
Status OkStatus() { return Status{StatusCode::ok, Severity::info, Subsystem::engine}; }
Status DisabledStatus() { return Status{StatusCode::platform_required_feature_missing, Severity::warning, Subsystem::engine}; }
}

const char* IndexAccelerationKindName(IndexAccelerationKind kind) {
  switch (kind) {
    case IndexAccelerationKind::none: return "none";
    case IndexAccelerationKind::llvm_jit: return "llvm_jit";
    case IndexAccelerationKind::llvm_aot: return "llvm_aot";
    case IndexAccelerationKind::gpu: return "gpu";
  }
  return "unknown";
}

const char* IndexAccelerationDecisionKindName(IndexAccelerationDecisionKind decision) {
  switch (decision) {
    case IndexAccelerationDecisionKind::allowed: return "allowed";
    case IndexAccelerationDecisionKind::disabled_fallback_cpu: return "disabled_fallback_cpu";
    case IndexAccelerationDecisionKind::policy_blocked: return "policy_blocked";
  }
  return "unknown";
}

IndexAccelerationDecision EvaluateIndexAcceleration(IndexFamily family, IndexAccelerationKind requested,
                                                    bool profile_enabled, bool resource_epoch_current) {
  if (requested == IndexAccelerationKind::none) {
    return IndexAccelerationDecision{OkStatus(), IndexAccelerationDecisionKind::disabled_fallback_cpu,
                                     requested, false, "cpu_canonical", {}};
  }
  if (requested == IndexAccelerationKind::gpu && family == IndexFamily::policy_blocked) {
    return IndexAccelerationDecision{DisabledStatus(), IndexAccelerationDecisionKind::policy_blocked,
                                     requested, false, "cpu_canonical",
                                     MakeIndexFamilyDiagnostic(DisabledStatus(), "INDEX.ACCELERATION.POLICY_BLOCKED", "index.acceleration.policy_blocked")};
  }
  if (!profile_enabled || !resource_epoch_current) {
    return IndexAccelerationDecision{DisabledStatus(), IndexAccelerationDecisionKind::disabled_fallback_cpu,
                                     requested, false, "cpu_canonical",
                                     MakeIndexFamilyDiagnostic(DisabledStatus(), "INDEX.ACCELERATION.DISABLED", "index.acceleration.disabled")};
  }
  return IndexAccelerationDecision{OkStatus(), IndexAccelerationDecisionKind::allowed, requested, false,
                                   "cpu_canonical", {}};
}

}  // namespace scratchbird::core::index
